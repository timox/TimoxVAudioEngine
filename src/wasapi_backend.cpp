#include "wasapi_backend.h"
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <propvarutil.h>
#include <new>
#include <sstream>
#include <utility>

namespace {
using Microsoft::WRL::ComPtr;
constexpr std::int64_t maxDuration100ns = 10000000;
constexpr std::uint64_t maxBufferBytes = 64ULL*1024*1024;

struct Event {
    HANDLE handle = nullptr;
    ~Event() { if (handle) CloseHandle(handle); }
    Event() = default;
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
};
struct ProcessHandle {
    HANDLE value = nullptr;
    ~ProcessHandle() { if (value) CloseHandle(value); }
    ProcessHandle() = default;
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
};
struct WaveFormat {
    WAVEFORMATEX* value = nullptr;
    ~WaveFormat() { CoTaskMemFree(value); }
};

HRESULT ValidateConfig(const WasapiOpenConfig& config) noexcept {
    if (config.kind == WasapiStreamKind::ProcessLoopback) {
        return config.processId && config.processCreationTime && !config.processImagePath.empty() &&
            config.endpointId.empty() && !config.defaultRole ? S_OK : E_INVALIDARG;
    }
    if (config.kind != WasapiStreamKind::Capture && config.kind != WasapiStreamKind::Render &&
        config.kind != WasapiStreamKind::EndpointLoopback) return E_INVALIDARG;
    if (config.endpointId.empty() == !config.defaultRole.has_value()) return E_INVALIDARG;
    if (config.defaultRole && *config.defaultRole != WasapiDefaultRole::Console &&
        *config.defaultRole != WasapiDefaultRole::Multimedia &&
        *config.defaultRole != WasapiDefaultRole::Communications) return E_INVALIDARG;
    if (config.bufferDuration100ns < 0 || config.bufferDuration100ns > maxDuration100ns) return E_INVALIDARG;
    return S_OK;
}

struct Stream {
    // Reverse member destruction releases services/client, event, then process handle.
    ProcessHandle process;
    Event event;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioRenderClient> render;
    WasapiStreamInfo info;
    std::optional<WasapiOpenConfig> processIdentity;
    bool started = false;
    std::uint32_t acquiredFrames = 0;
    ~Stream() {
        if (acquiredFrames) {
            if (capture) capture->ReleaseBuffer(0);
            if (render) render->ReleaseBuffer(acquiredFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        if (started && client) client->Stop();
    }
};

bool ProcessStillRunning(HANDLE process) noexcept {
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

bool ProcessIdentityMatches(HANDLE process, const WasapiOpenConfig& config) {
    if (!ProcessStillRunning(process)) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
    if (exited.dwHighDateTime || exited.dwLowDateTime) return false;
    const auto creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    if (creation != config.processCreationTime) return false;
    std::wstring path(32768, L'\0'); DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) return false;
    path.resize(length);
    return CompareStringOrdinal(path.c_str(), static_cast<int>(path.size()),
        config.processImagePath.c_str(), static_cast<int>(config.processImagePath.size()), TRUE) == CSTR_EQUAL;
}

class ActivationHandler final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IActivateAudioInterfaceCompletionHandler, Microsoft::WRL::FtmBase> {
public:
    ActivationHandler() : completed_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationHandler() {
        // pBlobData points into activation_; do not let PropVariantClear free it.
        activationParameters_.vt = VT_EMPTY;
        PropVariantClear(&activationParameters_);
        if (completed_) CloseHandle(completed_);
    }
    HANDLE CompletedEvent() const noexcept { return completed_; }
    PROPVARIANT* ConfigureProcessLoopback(DWORD processId) noexcept {
        activation_ = {};
        activation_.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        activation_.ProcessLoopbackParams.TargetProcessId = processId;
        activation_.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PropVariantInit(&activationParameters_);
        activationParameters_.vt = VT_BLOB;
        activationParameters_.blob.cbSize = sizeof(activation_);
        activationParameters_.blob.pBlobData = reinterpret_cast<BYTE*>(&activation_);
        return &activationParameters_;
    }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) noexcept override {
        ComPtr<IUnknown> activated;
        result_ = operation->GetActivateResult(&activationResult_, activated.GetAddressOf());
        if (SUCCEEDED(result_) && SUCCEEDED(activationResult_)) result_ = activated.As(&client_);
        if (completed_) SetEvent(completed_);
        return S_OK;
    }
    HRESULT Result() const noexcept { return FAILED(result_) ? result_ : activationResult_; }
    ComPtr<IAudioClient> Client() const { return client_; }
private:
    HANDLE completed_ = nullptr;
    AUDIOCLIENT_ACTIVATION_PARAMS activation_{};
    PROPVARIANT activationParameters_{};
    HRESULT result_ = E_PENDING;
    HRESULT activationResult_ = E_PENDING;
    ComPtr<IAudioClient> client_;
};

class WasapiBackend final : public IWasapiBackend {
public:
    HRESULT Open(const WasapiOpenConfig& config) noexcept override {
        openErrorDetail_.clear();
        if (stream_) return AUDCLNT_E_ALREADY_INITIALIZED;
        const HRESULT validation = ValidateConfig(config);
        if (FAILED(validation)) return validation;
        try { return OpenValidated(config); }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }
    const WasapiStreamInfo* Info() const noexcept override { return stream_ ? &stream_->info : nullptr; }
    const char* OpenErrorDetail() const noexcept override { return openErrorDetail_.c_str(); }
    HRESULT Start() noexcept override {
        if (!stream_) return AUDCLNT_E_NOT_INITIALIZED;
        if (stream_->processIdentity && !ProcessStillRunning(stream_->process.value)) return AUDCLNT_E_DEVICE_INVALIDATED;
        if (stream_->acquiredFrames) return AUDCLNT_E_OUT_OF_ORDER;
        if (stream_->started) return S_OK;
        if (!ResetEvent(stream_->event.handle)) return HRESULT_FROM_WIN32(GetLastError());
        const HRESULT hr = stream_->client->Start();
        if (SUCCEEDED(hr)) stream_->started = true;
        return hr;
    }
    HRESULT Stop() noexcept override {
        if (!stream_) return S_OK;
        if (stream_->acquiredFrames) return AUDCLNT_E_OUT_OF_ORDER;
        if (!stream_->started) return S_OK;
        const HRESULT hr = stream_->client->Stop();
        if (SUCCEEDED(hr)) stream_->started = false;
        return hr;
    }
    void Close() noexcept override { stream_.reset(); }
    WasapiWaitResult Wait(HANDLE stopEvent, DWORD timeoutMs) noexcept override {
        if (!stream_) return {WasapiWaitKind::Failed, AUDCLNT_E_NOT_INITIALIZED};
        // Stop comes first so a continually signaled audio event cannot starve it.
        HANDLE handles[2] = {stopEvent, stream_->event.handle};
        const DWORD count = stopEvent ? 2 : 1;
        const DWORD result = WaitForMultipleObjects(count, stopEvent ? handles : handles+1, FALSE, timeoutMs);
        if (result == WAIT_OBJECT_0) return {stopEvent ? WasapiWaitKind::Stop : WasapiWaitKind::Audio, S_OK};
        if (stopEvent && result == WAIT_OBJECT_0+1) return {WasapiWaitKind::Audio, S_OK};
        if (result == WAIT_TIMEOUT) return {WasapiWaitKind::Timeout, S_OK};
        return {WasapiWaitKind::Failed, result == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_UNEXPECTED};
    }
    HRESULT NextCapturePacketFrames(std::uint32_t& frames) noexcept override {
        frames = 0;
        if (!stream_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!stream_->capture) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        if (stream_->processIdentity && !ProcessStillRunning(stream_->process.value)) return AUDCLNT_E_DEVICE_INVALIDATED;
        if (stream_->acquiredFrames) return AUDCLNT_E_OUT_OF_ORDER;
        UINT32 native = 0;
        const HRESULT hr = stream_->capture->GetNextPacketSize(&native);
        if (FAILED(hr)) return hr;
        if (native > stream_->info.bufferFrames) return AUDCLNT_E_BUFFER_TOO_LARGE;
        frames = native;
        return hr;
    }
    HRESULT AcquireCapture(WasapiCapturePacket& packet) noexcept override {
        packet = {};
        if (!stream_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!stream_->capture) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        if (stream_->processIdentity && !ProcessStillRunning(stream_->process.value)) return AUDCLNT_E_DEVICE_INVALIDATED;
        if (stream_->acquiredFrames) return AUDCLNT_E_OUT_OF_ORDER;
        BYTE* bytes = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 device = 0, qpc = 0;
        const HRESULT hr = stream_->capture->GetBuffer(&bytes, &frames, &flags, &device, &qpc);
        if (FAILED(hr) || hr == AUDCLNT_S_BUFFER_EMPTY) return hr;
        if (!frames || frames > stream_->info.bufferFrames || (!bytes && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))) {
            stream_->capture->ReleaseBuffer(0);
            return E_UNEXPECTED;
        }
        stream_->acquiredFrames = frames;
        packet.data = bytes;
        packet.frames = frames;
        packet.silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        packet.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        packet.timestampError = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
        packet.devicePosition = device;
        packet.qpcPosition100ns = qpc;
        return hr;
    }
    HRESULT ReleaseCapture(std::uint32_t frames) noexcept override {
        if (!stream_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!stream_->capture) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        if (!stream_->acquiredFrames) return AUDCLNT_E_OUT_OF_ORDER;
        if (frames && frames != stream_->acquiredFrames) return AUDCLNT_E_INVALID_SIZE;
        const HRESULT hr = stream_->capture->ReleaseBuffer(frames);
        if (SUCCEEDED(hr)) {
            stream_->acquiredFrames = 0;
            if (stream_->processIdentity && !ProcessStillRunning(stream_->process.value))
                return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        return hr;
    }
    HRESULT RenderWritableFrames(std::uint32_t& frames) noexcept override {
        frames = 0;
        if (!stream_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!stream_->render) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        if (stream_->acquiredFrames) return AUDCLNT_E_OUT_OF_ORDER;
        UINT32 padding = 0;
        const HRESULT hr = stream_->client->GetCurrentPadding(&padding);
        if (FAILED(hr)) return hr;
        if (padding > stream_->info.bufferFrames) return E_UNEXPECTED;
        frames = stream_->info.bufferFrames-padding;
        return hr;
    }
    HRESULT AcquireRender(std::uint32_t frames, std::uint8_t*& data) noexcept override {
        data = nullptr;
        if (!stream_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!stream_->render) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        if (stream_->acquiredFrames) return AUDCLNT_E_OUT_OF_ORDER;
        if (!frames) return E_INVALIDARG;
        if (frames > stream_->info.bufferFrames) return AUDCLNT_E_BUFFER_TOO_LARGE;
        BYTE* bytes = nullptr;
        const HRESULT hr = stream_->render->GetBuffer(frames, &bytes);
        if (FAILED(hr)) return hr;
        if (!bytes) { stream_->render->ReleaseBuffer(0, 0); return E_UNEXPECTED; }
        stream_->acquiredFrames = frames;
        data = bytes;
        return hr;
    }
    HRESULT ReleaseRender(std::uint32_t frames, bool silent) noexcept override {
        if (!stream_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!stream_->render) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        if (!stream_->acquiredFrames) return AUDCLNT_E_OUT_OF_ORDER;
        if (frames && frames != stream_->acquiredFrames) return AUDCLNT_E_INVALID_SIZE;
        const HRESULT hr = stream_->render->ReleaseBuffer(frames, silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
        if (SUCCEEDED(hr)) stream_->acquiredFrames = 0;
        return hr;
    }

private:
    HRESULT OpenFailure(const char* operation, HRESULT result) {
        std::ostringstream text;
        text << operation << " failed (HRESULT 0x" << std::hex
             << static_cast<unsigned long>(result) << ')';
        openErrorDetail_ = text.str();
        return result;
    }

    HRESULT OpenValidated(const WasapiOpenConfig& config) {
        auto candidate = std::make_unique<Stream>();
        candidate->info.kind = config.kind;
        if (config.kind == WasapiStreamKind::ProcessLoopback) candidate->processIdentity = config;
        if (config.kind == WasapiStreamKind::ProcessLoopback)
            return OpenProcessLoopback(config, std::move(candidate));
        const auto flow = config.kind == WasapiStreamKind::Capture ? WasapiDataFlow::Capture : WasapiDataFlow::Render;
        WasapiDeviceEnumerator resolver;
        std::string error;
        if (!resolver.resolve(flow, config.endpointId, config.defaultRole, candidate->info.endpointId, error)) {
            openErrorDetail_ = std::move(error);
            return E_FAIL;
        }
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(enumerator.GetAddressOf()));
        if (FAILED(hr)) return OpenFailure("Create MMDeviceEnumerator", hr);
        ComPtr<IMMDevice> device;
        hr = enumerator->GetDevice(candidate->info.endpointId.c_str(), device.GetAddressOf());
        if (FAILED(hr)) return OpenFailure("Get WASAPI endpoint", hr);
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(candidate->client.GetAddressOf()));
        if (FAILED(hr)) return OpenFailure("Activate WASAPI audio client", hr);
        WaveFormat native;
        hr = candidate->client->GetMixFormat(&native.value);
        if (FAILED(hr)) return OpenFailure("Get WASAPI mix format", hr);
        if (!native.value || !ParseWasapiMixFormat(native.value, sizeof(WAVEFORMATEX)+native.value->cbSize,
                                                  candidate->info.format, error)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        hr = ValidateWasapiStreamFormat(candidate->info.format, 1);
        if (FAILED(hr)) return hr;
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (config.kind == WasapiStreamKind::EndpointLoopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        candidate->event.handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!candidate->event.handle) return HRESULT_FROM_WIN32(GetLastError());
        // Native shared format: no AUTOCONVERTPCM, no SRC_DEFAULT_QUALITY.
        // Shared periodicity must be zero; this is not IAudioClient3 low latency.
        hr = candidate->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, config.bufferDuration100ns,
                                            0, native.value, nullptr);
        if (FAILED(hr)) return OpenFailure("Initialize shared WASAPI stream", hr);
        // Install immediately: later metadata failures must not release an
        // EVENTCALLBACK client with no event attached (Windows can delay that).
        hr = candidate->client->SetEventHandle(candidate->event.handle);
        if (FAILED(hr)) return OpenFailure("Set WASAPI event handle", hr);
        UINT32 frames = 0;
        hr = candidate->client->GetBufferSize(&frames);
        if (FAILED(hr)) return OpenFailure("Get WASAPI buffer size", hr);
        hr = ValidateWasapiStreamFormat(candidate->info.format, frames);
        if (FAILED(hr)) return hr;
        candidate->info.bufferFrames = frames;
        REFERENCE_TIME latency = 0, defaultPeriod = 0, minimumPeriod = 0;
        hr = candidate->client->GetStreamLatency(&latency);
        if (FAILED(hr)) return OpenFailure("Get WASAPI stream latency", hr);
        hr = candidate->client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
        if (FAILED(hr)) return OpenFailure("Get WASAPI device period", hr);
        if (latency < 0 || defaultPeriod <= 0 || minimumPeriod <= 0) return E_UNEXPECTED;
        candidate->info.streamLatency100ns = latency;
        candidate->info.defaultPeriod100ns = defaultPeriod;
        candidate->info.minimumPeriod100ns = minimumPeriod;
        hr = config.kind == WasapiStreamKind::Render
            ? candidate->client->GetService(IID_PPV_ARGS(candidate->render.GetAddressOf()))
            : candidate->client->GetService(IID_PPV_ARGS(candidate->capture.GetAddressOf()));
        if (FAILED(hr)) return OpenFailure("Get WASAPI stream service", hr);
        stream_ = std::move(candidate);
        return S_OK;
    }
    HRESULT OpenProcessLoopback(const WasapiOpenConfig& config, std::unique_ptr<Stream> candidate) {
        candidate->process.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE, config.processId);
        if (!candidate->process.value || !ProcessIdentityMatches(candidate->process.value, config)) {
            openErrorDetail_ = "Windows application identity changed; capture was not started";
            return HRESULT_FROM_WIN32(ERROR_INVALID_OWNER);
        }
        auto handler = Microsoft::WRL::Make<ActivationHandler>();
        if (!handler) return E_OUTOFMEMORY;
        if (!handler->CompletedEvent()) return HRESULT_FROM_WIN32(GetLastError());
        auto* parameters = handler->ConfigureProcessLoopback(config.processId);
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof(IAudioClient), parameters, handler.Get(), operation.GetAddressOf());
        if (FAILED(hr)) return OpenFailure("Activate process-loopback audio interface", hr);
        const DWORD wait = WaitForSingleObject(handler->CompletedEvent(), 15000);
        if (wait != WAIT_OBJECT_0) return OpenFailure("Wait for process-loopback activation",
            wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : HRESULT_FROM_WIN32(GetLastError()));
        hr = handler->Result();
        if (FAILED(hr)) return OpenFailure("Complete process-loopback activation", hr);
        if (!ProcessIdentityMatches(candidate->process.value, config)) {
            openErrorDetail_ = "Windows application identity changed during activation; capture was not started";
            return HRESULT_FROM_WIN32(ERROR_INVALID_OWNER);
        }
        candidate->client = handler->Client();
        if (!candidate->client) return OpenFailure("Obtain process-loopback audio client", E_NOINTERFACE);
        // Process-loopback IAudioClient can return E_NOTIMPL from GetMixFormat
        // on supported Windows builds. Use the documented shared-mode PCM
        // format and let the Windows audio engine convert the process stream.
        WAVEFORMATEX native{};
        native.wFormatTag = WAVE_FORMAT_PCM;
        native.nChannels = 2;
        native.nSamplesPerSec = 44100;
        native.wBitsPerSample = 16;
        native.nBlockAlign = native.nChannels * native.wBitsPerSample / 8;
        native.nAvgBytesPerSec = native.nSamplesPerSec * native.nBlockAlign;
        std::string error;
        if (!ParseWasapiMixFormat(&native, sizeof(native), candidate->info.format, error)) {
            openErrorDetail_ = std::move(error);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        hr = ValidateWasapiStreamFormat(candidate->info.format, 1);
        if (FAILED(hr)) return hr;
        candidate->event.handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!candidate->event.handle) return HRESULT_FROM_WIN32(GetLastError());
        hr = candidate->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_LOOPBACK |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            config.bufferDuration100ns, 0, &native, nullptr);
        if (FAILED(hr)) return OpenFailure("Initialize process-loopback stream", hr);
        hr = candidate->client->SetEventHandle(candidate->event.handle);
        if (FAILED(hr)) return OpenFailure("Set process-loopback event handle", hr);
        UINT32 frames = 0;
        hr = candidate->client->GetBufferSize(&frames);
        if (FAILED(hr)) return OpenFailure("Get process-loopback buffer size", hr);
        hr = ValidateWasapiStreamFormat(candidate->info.format, frames);
        if (FAILED(hr)) return hr;
        candidate->info.bufferFrames = frames;
        // The process-loopback virtual device does not promise physical
        // endpoint latency/period metadata; its format and buffer size are the
        // authoritative values for the existing bridge FIFO sizing.
        hr = candidate->client->GetService(IID_PPV_ARGS(candidate->capture.GetAddressOf()));
        if (FAILED(hr)) return OpenFailure("Get process-loopback capture service", hr);
        candidate->info.endpointId = L"process:" + std::to_wstring(config.processId) + L":" + std::to_wstring(config.processCreationTime);
        stream_ = std::move(candidate);
        return S_OK;
    }
    std::unique_ptr<Stream> stream_;
    std::string openErrorDetail_;
};
}

HRESULT ValidateWasapiStreamFormat(const WasapiMixFormat& format, std::uint32_t frames) noexcept {
    const bool floating = format.encoding == WasapiSampleEncoding::IeeeFloat;
    if (format.encoding != WasapiSampleEncoding::IeeeFloat && format.encoding != WasapiSampleEncoding::PcmInteger)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (!format.channels || format.channels > 256 || format.sampleRate < 8000 || format.sampleRate > 384000 ||
        !format.validBits || format.validBits > format.containerBits ||
        (floating && format.validBits != format.containerBits)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (floating ? format.containerBits != 32 && format.containerBits != 64 :
        format.containerBits != 8 && format.containerBits != 16 && format.containerBits != 24 && format.containerBits != 32)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (format.blockAlign != static_cast<std::uint32_t>(format.channels)*(format.containerBits/8))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    unsigned positions = 0;
    for (auto mask = format.channelMask; mask; mask &= mask-1) ++positions;
    if (positions && positions != format.channels) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (!frames || frames > format.sampleRate || static_cast<std::uint64_t>(frames)*format.blockAlign > maxBufferBytes)
        return AUDCLNT_E_BUFFER_SIZE_ERROR;
    return S_OK;
}

std::unique_ptr<IWasapiBackend> CreateWasapiBackend() { return std::make_unique<WasapiBackend>(); }
