#include "wasapi_device_enumerator.h"
#include <Windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <sstream>

namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID kUnspecifiedContainerId = {0, 0, 0, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
// Success status: FAILED(hr) does not detect sessions that span processes.
constexpr HRESULT kAudioClientNoSingleProcess = static_cast<HRESULT>(0x0889000D);
bool IsUsableContainerId(const GUID& id) noexcept {
    return !IsEqualGUID(id, GUID_NULL) && !IsEqualGUID(id, kUnspecifiedContainerId);
}
template<class T> struct TaskMemory {
    T* value = nullptr;
    ~TaskMemory() { CoTaskMemFree(value); }
    TaskMemory() = default;
    TaskMemory(const TaskMemory&) = delete;
    TaskMemory& operator=(const TaskMemory&) = delete;
};
struct PropertyValue {
    PROPVARIANT value{};
    ~PropertyValue() { PropVariantClear(&value); }
};
class SessionObserver final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IAudioSessionNotification, Microsoft::WRL::FtmBase> {
public:
    STDMETHOD(OnSessionCreated)(IAudioSessionControl* session) noexcept override {
        if (!session) return E_POINTER;
        try { std::lock_guard<std::mutex> lock(mutex_); created_.emplace_back(session); return S_OK; }
        catch (...) { return E_OUTOFMEMORY; }
    }
    std::vector<ComPtr<IAudioSessionControl>> TakeCreated() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = std::move(created_);
        created_.clear();
        return result;
    }
private:
    std::mutex mutex_;
    std::vector<ComPtr<IAudioSessionControl>> created_;
};
bool Fail(std::string& error, const char* message) {
    error = message;
    return false;
}
bool Check(HRESULT hr, const char* operation, std::string& error) {
    if (SUCCEEDED(hr)) return true;
    std::ostringstream text;
    text << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ')';
    error = text.str();
    return false;
}
bool NativeFlow(WasapiDataFlow flow, EDataFlow& native, std::string& error) {
    switch (flow) {
    case WasapiDataFlow::Capture: native = eCapture; return true;
    case WasapiDataFlow::Render: native = eRender; return true;
    }
    return Fail(error, "Invalid WASAPI data flow");
}
bool CreateEnumerator(ComPtr<IMMDeviceEnumerator>& enumerator, std::string& error) {
    return Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(enumerator.GetAddressOf())), "Create MMDeviceEnumerator (caller must initialize COM)", error);
}
bool DeviceId(IMMDevice* device, std::wstring& id, std::string& error) {
    TaskMemory<wchar_t> text;
    if (!Check(device->GetId(&text.value), "Get endpoint ID", error)) return false;
    if (!text.value || !*text.value) return Fail(error, "Endpoint returned an empty ID");
    id = text.value;
    return true;
}
void ReadMixFormat(IMMDevice* device, WasapiDeviceInfo& info) {
    if (!(info.state & DEVICE_STATE_ACTIVE)) {
        info.formatError = "Endpoint is not active";
        return;
    }
    ComPtr<IAudioClient> client;
    if (!Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(client.GetAddressOf())), "Activate audio client", info.formatError)) return;
    TaskMemory<WAVEFORMATEX> format;
    if (!Check(client->GetMixFormat(&format.value), "Get shared mix format", info.formatError)) return;
    if (!format.value) { info.formatError = "Endpoint returned no mix format"; return; }
    WasapiMixFormat parsed;
    // The OS owns the allocation contract: cbSize bytes follow WAVEFORMATEX.
    if (ParseWasapiMixFormat(format.value, sizeof(WAVEFORMATEX) + format.value->cbSize, parsed, info.formatError))
        info.mixFormat = parsed;
}
}

bool ParseWasapiMixFormat(const void* bytes, std::size_t size,
    WasapiMixFormat& result, std::string& error) {
    result = {};
    error.clear();
    if (!bytes || size < sizeof(WAVEFORMATEX)) return Fail(error, "Truncated WAVEFORMATEX");
    WAVEFORMATEX base{};
    std::memcpy(&base, bytes, sizeof(base));
    if (base.cbSize > size - sizeof(base)) return Fail(error, "Truncated wave format extension");
    WasapiMixFormat candidate;
    WORD tag = base.wFormatTag;
    candidate.validBits = base.wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (base.cbSize < 22 || size < sizeof(WAVEFORMATEXTENSIBLE))
            return Fail(error, "Truncated WAVEFORMATEXTENSIBLE");
        WAVEFORMATEXTENSIBLE ext{};
        std::memcpy(&ext, bytes, sizeof(ext));
        const GUID pcm = {WAVE_FORMAT_PCM, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
        const GUID floating = {WAVE_FORMAT_IEEE_FLOAT, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
        if (IsEqualGUID(ext.SubFormat, pcm)) tag = WAVE_FORMAT_PCM;
        else if (IsEqualGUID(ext.SubFormat, floating)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else return Fail(error, "Unsupported extensible sample encoding");
        candidate.validBits = ext.Samples.wValidBitsPerSample;
        candidate.channelMask = ext.dwChannelMask;
        unsigned positions = 0;
        for (auto mask = ext.dwChannelMask; mask; mask &= mask - 1) ++positions;
        if (positions && positions != base.nChannels) return Fail(error, "Channel mask does not match channel count");
    }
    if (tag != WAVE_FORMAT_PCM && tag != WAVE_FORMAT_IEEE_FLOAT)
        return Fail(error, "Unsupported sample encoding");
    if (!base.nChannels || !base.nSamplesPerSec) return Fail(error, "Empty channel count or sample rate");
    const bool floating = tag == WAVE_FORMAT_IEEE_FLOAT;
    if (floating ? base.wBitsPerSample != 32 && base.wBitsPerSample != 64
        : base.wBitsPerSample != 8 && base.wBitsPerSample != 16 && base.wBitsPerSample != 24 && base.wBitsPerSample != 32)
        return Fail(error, "Unsupported sample container size");
    if (!candidate.validBits || candidate.validBits > base.wBitsPerSample ||
        (floating && candidate.validBits != base.wBitsPerSample)) return Fail(error, "Invalid valid-bit count");
    const std::uint32_t alignment = static_cast<std::uint32_t>(base.nChannels) * (base.wBitsPerSample / 8);
    const std::uint64_t byteRate = static_cast<std::uint64_t>(base.nSamplesPerSec) * alignment;
    if (alignment != base.nBlockAlign || byteRate > (std::numeric_limits<DWORD>::max)() ||
        byteRate != base.nAvgBytesPerSec) return Fail(error, "Inconsistent block alignment or byte rate");
    candidate.sampleRate = base.nSamplesPerSec;
    candidate.channels = base.nChannels;
    candidate.containerBits = base.wBitsPerSample;
    candidate.blockAlign = base.nBlockAlign;
    candidate.encoding = floating ? WasapiSampleEncoding::IeeeFloat : WasapiSampleEncoding::PcmInteger;
    result = candidate;
    return true;
}

bool WasapiDeviceEnumerator::enumerate(std::vector<WasapiDeviceInfo>& devices, std::string& error) const {
    devices.clear();
    error.clear();
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (!CreateEnumerator(enumerator, error)) return false;
    std::vector<WasapiDeviceInfo> inventory;
    for (const auto flow : {WasapiDataFlow::Capture, WasapiDataFlow::Render}) {
        EDataFlow native = eAll;
        if (!NativeFlow(flow, native, error)) return false;
        ComPtr<IMMDeviceCollection> collection;
        if (!Check(enumerator->EnumAudioEndpoints(native, DEVICE_STATEMASK_ALL, collection.GetAddressOf()),
            "Enumerate audio endpoints", error)) return false;
        UINT count = 0;
        if (!Check(collection->GetCount(&count), "Count audio endpoints", error)) return false;
        for (UINT index = 0; index < count; ++index) {
            ComPtr<IMMDevice> device;
            if (!Check(collection->Item(index, device.GetAddressOf()), "Read audio endpoint", error)) return false;
            WasapiDeviceInfo info;
            info.flow = flow;
            if (!DeviceId(device.Get(), info.id, error)) return false;
            DWORD state = 0;
            if (!Check(device->GetState(&state), "Read endpoint state", error)) return false;
            info.state = state;
            info.name = info.id; // Stable, nonempty fallback when the display property is unavailable.
            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) {
                PropertyValue property;
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &property.value)) &&
                    property.value.vt == VT_LPWSTR && property.value.pwszVal && *property.value.pwszVal)
                    info.name = property.value.pwszVal;
                PropVariantClear(&property.value);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_ContainerId, &property.value)) &&
                    property.value.vt == VT_CLSID && property.value.puuid) {
                    wchar_t containerId[39]{};
                    if (IsUsableContainerId(*property.value.puuid) &&
                        StringFromGUID2(*property.value.puuid, containerId, ARRAYSIZE(containerId)) > 0)
                        info.containerId = containerId;
                }
            }
            ReadMixFormat(device.Get(), info);
            inventory.push_back(std::move(info));
        }
    }
    devices.swap(inventory);
    return true;
}

bool WasapiDeviceEnumerator::enumerateActiveRenderProcesses(
    std::vector<WasapiProcessInfo>& processes, std::string& error) const {
    processes.clear(); error.clear();
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (!CreateEnumerator(enumerator, error)) return false;
    ComPtr<IMMDeviceCollection> endpoints;
    if (!Check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, endpoints.GetAddressOf()),
        "Enumerate active render endpoints", error)) return false;
    UINT endpointCount = 0;
    if (!Check(endpoints->GetCount(&endpointCount), "Count active render endpoints", error)) return false;
    std::unordered_map<DWORD, std::size_t> byPid;
    for (UINT endpointIndex = 0; endpointIndex < endpointCount; ++endpointIndex) {
        ComPtr<IMMDevice> endpoint;
        if (!Check(endpoints->Item(endpointIndex, endpoint.GetAddressOf()), "Read render endpoint", error)) return false;
        std::wstring endpointId;
        if (!DeviceId(endpoint.Get(), endpointId, error)) return false;
        std::wstring endpointName = endpointId;
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) {
            PropertyValue name;
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name.value)) &&
                name.value.vt == VT_LPWSTR && name.value.pwszVal && *name.value.pwszVal)
                endpointName = name.value.pwszVal;
        }
        ComPtr<IAudioSessionManager2> manager;
        HRESULT hr = endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(manager.GetAddressOf()));
        if (FAILED(hr)) continue; // One broken endpoint must not hide other sessions.
        auto observer = Microsoft::WRL::Make<SessionObserver>();
        if (!observer) return Fail(error, "Cannot allocate Windows audio session observer");
        if (FAILED(manager->RegisterSessionNotification(observer.Get()))) continue;
        struct Registration {
            IAudioSessionManager2* manager;
            IAudioSessionNotification* observer;
            ~Registration() { if (manager) manager->UnregisterSessionNotification(observer); }
        } registration{manager.Get(), observer.Get()};
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(sessions.GetAddressOf()))) continue;
        int sessionCount = 0;
        if (FAILED(sessions->GetCount(&sessionCount))) continue;
        std::vector<ComPtr<IAudioSessionControl>> observed;
        observed.reserve(static_cast<std::size_t>(sessionCount));
        for (int sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex) {
            ComPtr<IAudioSessionControl> control;
            if (SUCCEEDED(sessions->GetSession(sessionIndex, control.GetAddressOf()))) observed.push_back(std::move(control));
        }
        auto duringSnapshot = observer->TakeCreated();
        observed.insert(observed.end(), std::make_move_iterator(duringSnapshot.begin()), std::make_move_iterator(duringSnapshot.end()));
        manager->UnregisterSessionNotification(observer.Get());
        registration.manager = nullptr;
        auto beforeUnregister = observer->TakeCreated();
        observed.insert(observed.end(), std::make_move_iterator(beforeUnregister.begin()), std::make_move_iterator(beforeUnregister.end()));
        for (const auto& control : observed) {
            AudioSessionState state = AudioSessionStateInactive;
            if (FAILED(control->GetState(&state)) || state != AudioSessionStateActive) continue;
            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control.As(&control2))) continue;
            DWORD pid = 0;
            hr = control2->GetProcessId(&pid);
            if (hr == kAudioClientNoSingleProcess || FAILED(hr) || pid == 0) continue;
            auto found = byPid.find(pid);
            if (found != byPid.end()) {
                auto& labels = processes[found->second].renderEndpoints;
                if (std::find(labels.begin(), labels.end(), endpointName) == labels.end()) labels.push_back(endpointName);
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process) continue;
            struct Handle { HANDLE value; ~Handle() { if (value) CloseHandle(value); } } close{process};
            std::wstring path(32768, L'\0'); DWORD pathLength = static_cast<DWORD>(path.size());
            if (!QueryFullProcessImageNameW(process, 0, path.data(), &pathLength) || !pathLength) continue;
            path.resize(pathLength);
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) continue;
            const auto ticks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            const auto slash = path.find_last_of(L"\\/");
            WasapiProcessInfo info;
            info.processId = pid; info.creationTime = ticks; info.imagePath = path;
            info.displayName = slash == std::wstring::npos ? path : path.substr(slash + 1);
            info.renderEndpoints.push_back(endpointName);
            byPid.emplace(pid, processes.size());
            processes.push_back(std::move(info));
        }
    }
    return true;
}

bool WasapiDeviceEnumerator::resolve(WasapiDataFlow flow, const std::wstring& explicitId,
    std::optional<WasapiDefaultRole> defaultRole, std::wstring& resolvedId, std::string& error) const {
    resolvedId.clear();
    error.clear();
    if (explicitId.empty() == !defaultRole.has_value())
        return Fail(error, "Select exactly one endpoint ID or explicit default role");
    EDataFlow native = eAll;
    if (!NativeFlow(flow, native, error)) return false;
    ERole role = eConsole;
    if (defaultRole) {
        switch (*defaultRole) {
        case WasapiDefaultRole::Console: role = eConsole; break;
        case WasapiDefaultRole::Multimedia: role = eMultimedia; break;
        case WasapiDefaultRole::Communications: role = eCommunications; break;
        default: return Fail(error, "Invalid WASAPI default role");
        }
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (!CreateEnumerator(enumerator, error)) return false;
    ComPtr<IMMDevice> device;
    const HRESULT hr = defaultRole
        ? enumerator->GetDefaultAudioEndpoint(native, role, device.GetAddressOf())
        : enumerator->GetDevice(explicitId.c_str(), device.GetAddressOf());
    if (!Check(hr, "Resolve audio endpoint", error)) return false;
    ComPtr<IMMEndpoint> endpoint;
    if (!Check(device.As(&endpoint), "Read endpoint flow interface", error)) return false;
    EDataFlow actual = eAll;
    if (!Check(endpoint->GetDataFlow(&actual), "Read endpoint flow", error)) return false;
    if (actual != native) return Fail(error, "Endpoint direction does not match requested flow");
    DWORD state = 0;
    if (!Check(device->GetState(&state), "Read endpoint state", error)) return false;
    if (!(state & DEVICE_STATE_ACTIVE)) return Fail(error, "Endpoint is not active");
    return DeviceId(device.Get(), resolvedId, error);
}
