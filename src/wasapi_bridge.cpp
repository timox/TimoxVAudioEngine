#include "wasapi_bridge.h"
#include "wasapi_sample_conversion.h"
#include "drift_resampler.h"
#include <audioclient.h>
#include <avrt.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>
#if defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr HRESULT cancelled = HRESULT_FROM_WIN32(ERROR_CANCELLED);
struct Event {
    HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() { if (!value) throw std::system_error(static_cast<int>(GetLastError()), std::system_category()); }
    ~Event() { CloseHandle(value); }
};
struct Apartment {
    HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() { if (SUCCEEDED(status)) CoUninitialize(); }
};
struct FloatingPointMode {
#if defined(_M_X64) || defined(_M_IX86)
    unsigned previous = _mm_getcsr();
    FloatingPointMode() { _mm_setcsr(previous | 0x8040U); }
    ~FloatingPointMode() { _mm_setcsr(previous); }
#endif
};
struct Scheduling {
    HANDLE handle = nullptr;
    HRESULT error = S_OK;
    Scheduling() {
        DWORD index = 0;
        handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
        if (!handle) { error = HRESULT_FROM_WIN32(GetLastError()); return; }
        if (!AvSetMmThreadPriority(handle, AVRT_PRIORITY_HIGH)) {
            error = HRESULT_FROM_WIN32(GetLastError());
            AvRevertMmThreadCharacteristics(handle); handle = nullptr;
        }
    }
    ~Scheduling() { if (handle) AvRevertMmThreadCharacteristics(handle); }
};
struct BackendLifetime {
    std::unique_ptr<IWasapiBackend> backend;
    ~BackendLifetime() {
        if (backend) { backend->Stop(); backend->Close(); backend.reset(); }
    }
};
}

struct WasapiBridge::Impl {
    const WasapiBridgeConfig config;
    WasapiBridgeIo& io;
    BackendFactory factory;
    Event stop, ready;
    std::mutex lifecycle;
    std::thread worker;
    bool used = false;
    std::atomic<bool> startupDelivered{false};
    std::atomic<HRESULT> startupResult{E_PENDING}, error{S_OK}, mmcssError{S_OK};
    std::atomic<WasapiBridgeState> state{WasapiBridgeState::Idle};
    std::atomic<bool> mmcssActive{false}, primingTimedOut{false};
    std::atomic<std::uint64_t> capturedFrames{0}, renderedFrames{0}, droppedCaptureFrames{0};
    std::atomic<std::uint64_t> renderSilencedInputFrames{0}, renderSilencedDeviceFrames{0};
    std::atomic<std::uint64_t> discontinuities{0}, timestampErrors{0};
    std::atomic<std::uint64_t> controlBacklogUpdates{0};
    std::atomic<double> ratio{1};

    Impl(WasapiBridgeConfig options, WasapiBridgeIo& generation, BackendFactory create)
        : config(std::move(options)), io(generation), factory(std::move(create)) {}
    bool Stopping() const noexcept { return WaitForSingleObject(stop.value, 0) == WAIT_OBJECT_0; }
    double Target() const noexcept { return config.targetFillFrames ? config.targetFillFrames : io.Format().capacityFrames*0.5; }
    Clock::time_point Now() const noexcept {
        if(!config.controlTimeNanoseconds)return Clock::now();
        return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
            std::chrono::nanoseconds(config.controlTimeNanoseconds(config.controlTimeContext))));
    }
    HRESULT Validate() const noexcept {
        const bool render = config.endpoint.kind == WasapiStreamKind::Render;
        if (!factory || !config.maxFramesPerPass || config.maxFramesPerPass > 8192 ||
            !config.maxPacketsPerPass || config.maxPacketsPerPass > 64 ||
            !config.waitTimeoutMs || config.waitTimeoutMs > 5000 ||
            !config.primingTimeoutMs || config.primingTimeoutMs > 60000 ||
            !std::isfinite(config.targetFillFrames) || config.targetFillFrames < 0 ||
            Target() < 1 || Target() > io.Format().capacityFrames ||
            !std::isfinite(config.kp) || config.kp < 0 || !std::isfinite(config.ki) || config.ki < 0 ||
            render != (io.Format().direction == WasapiBridgeDirection::Render)) return E_INVALIDARG;
        return S_OK;
    }
    void CompleteStartup(HRESULT result) noexcept {
        startupResult.store(result, std::memory_order_release);
        startupDelivered.store(true, std::memory_order_release);
        SetEvent(ready.value);
    }
    void Run() noexcept {
        HRESULT result = E_FAIL;
        try { result = RunChecked(); }
        catch (const std::bad_alloc&) { result = E_OUTOFMEMORY; }
        catch (...) { result = E_FAIL; }
        mmcssActive.store(false, std::memory_order_relaxed);
        if (FAILED(result) && result != cancelled) {
            error.store(result, std::memory_order_release);
            state.store(WasapiBridgeState::Failed, std::memory_order_release);
        } else state.store(WasapiBridgeState::Stopped, std::memory_order_release);
        if (!startupDelivered.load(std::memory_order_acquire)) CompleteStartup(result);
    }

    struct Processing {
        Impl& owner;
        IWasapiBackend& backend;
        const WasapiStreamInfo& info;
        const bool render;
        const std::uint32_t channels;
        const std::uint32_t inputCapacity;
        const double nominal;
        DriftResampler src;
        FillLevelController controller;
        std::vector<std::uint8_t> raw;
        std::vector<float> input, output;
        std::uint32_t rawOffset=0, rawRemaining=0, inputOffset=0, inputRemaining=0;
        bool rawSilent=false;
        Clock::time_point primingSince=owner.Now(), lastUpdate=primingSince;

        Processing(Impl& workerOwner,IWasapiBackend& native,const WasapiStreamInfo& effective)
            : owner(workerOwner),backend(native),info(effective),render(effective.kind==WasapiStreamKind::Render),
              channels(effective.format.channels),
              inputCapacity(static_cast<std::uint32_t>((std::min)(owner.io.Format().capacityFrames,
                  static_cast<std::size_t>(owner.config.maxFramesPerPass)))),
              nominal(render ? static_cast<double>(info.format.sampleRate)/owner.io.Format().graphSampleRate
                             : static_cast<double>(owner.io.Format().graphSampleRate)/info.format.sampleRate),
              src(channels,nominal),
              controller(FillLevelController::ForPacketCadence(
                  render ? FillLevelController::Direction::Render : FillLevelController::Direction::Capture,
                  owner.Target(),owner.config.kp,owner.config.ki,owner.io.Format().graphSampleRate,
                  static_cast<std::uint32_t>(owner.io.Format().maxQuantum))),
              raw(render ? 0 : static_cast<std::size_t>(info.bufferFrames)*info.format.blockAlign,0),
              input(static_cast<std::size_t>(inputCapacity)*channels,0),
              output(static_cast<std::size_t>(owner.config.maxFramesPerPass)*channels,0) {
            owner.ratio.store(nominal,std::memory_order_relaxed);
        }

        double ControlFill() const noexcept {
            return static_cast<double>(owner.io.BufferedFrames())+(render ? inputRemaining : 0);
        }
        void UpdateControl(double fill) noexcept {
            const auto now=owner.Now();
            if (owner.state.load(std::memory_order_relaxed)==WasapiBridgeState::Priming) {
                const bool timedOut=now-primingSince>=std::chrono::milliseconds(owner.config.primingTimeoutMs);
                if (timedOut) owner.primingTimedOut.store(true,std::memory_order_relaxed);
                // A timeout is diagnostic, not permission to drain an unfilled
                // capture FIFO. Late-starting endpoints must still prefill.
                if (fill>=owner.Target()) {
                    if(!render)owner.io.CompleteCapturePriming();
                    owner.state.store(WasapiBridgeState::Running,std::memory_order_release);
                }
                lastUpdate=now;
                return;
            }
            const double elapsed=(std::min)(1.0,std::chrono::duration<double>(now-lastUpdate).count());
            lastUpdate=now;
            if (elapsed>0) {
                src.SetRatio(nominal*controller.Update(fill,elapsed));
                owner.ratio.store(src.Ratio(),std::memory_order_relaxed);
            }
        }

        HRESULT LoadCapture() noexcept {
            std::uint32_t next=0;
            HRESULT hr=backend.NextCapturePacketFrames(next);
            if (FAILED(hr)) return hr;
            if (!next) return S_FALSE;
            WasapiCapturePacket packet;
            hr=backend.AcquireCapture(packet);
            if (FAILED(hr)) return hr;
            if (hr==AUDCLNT_S_BUFFER_EMPTY) return S_FALSE;
            if (!packet.frames || packet.frames>info.bufferFrames || (!packet.silent && !packet.data)) {
                backend.ReleaseCapture(0); return E_UNEXPECTED;
            }
            if (!packet.silent) std::memcpy(raw.data(),packet.data,static_cast<std::size_t>(packet.frames)*info.format.blockAlign);
            hr=backend.ReleaseCapture(packet.frames);
            if (FAILED(hr)) return hr;
            rawOffset=0; rawRemaining=packet.frames; rawSilent=packet.silent;
            owner.capturedFrames.fetch_add(packet.frames,std::memory_order_relaxed);
            if (packet.discontinuity) owner.discontinuities.fetch_add(1,std::memory_order_relaxed);
            if (packet.timestampError) owner.timestampErrors.fetch_add(1,std::memory_order_relaxed);
            return S_OK;
        }

        HRESULT CapturePass(bool& more) noexcept {
            more=false;
            std::uint32_t budget=owner.config.maxFramesPerPass,packets=0;
            while (budget && !owner.Stopping()) {
                // Drain already available interpolated output before loading more
                // input, including upsampling outputs after a packet ends.
                const auto p=src.Process(input.data()+static_cast<std::size_t>(inputOffset)*channels,
                                         (std::min)(inputRemaining,budget),output.data(),budget);
                inputOffset+=p.consumed; inputRemaining-=p.consumed;
                if (p.produced) {
                    // IO bounds each transfer to capacity even when output
                    // scratch/pass is larger. Full FIFO policy is drop-newest.
                    std::uint32_t offset=0;
                    while (offset<p.produced) {
                        const auto count=(std::min)(static_cast<std::size_t>(p.produced-offset),owner.io.Format().capacityFrames);
                        const auto written=owner.io.WriteCaptureInterleaved(output.data()+static_cast<std::size_t>(offset)*channels,
                                                                             count,count*channels);
                        owner.droppedCaptureFrames.fetch_add(count-written,std::memory_order_relaxed);
                        offset+=static_cast<std::uint32_t>(count);
                    }
                }
                if (p.consumed || p.produced) { budget-=(std::max)(p.consumed,p.produced); continue; }
                if (inputRemaining) return E_UNEXPECTED;
                if (!rawRemaining) {
                    if (packets==owner.config.maxPacketsPerPass) {more=true;return S_OK;}
                    const HRESULT hr=LoadCapture();
                    if (hr==S_FALSE) return S_OK;
                    if (FAILED(hr)) return hr;
                    ++packets;
                }
                const auto decodeFrames=(std::min)({rawRemaining,inputCapacity,budget});
                const auto bytes=static_cast<std::size_t>(decodeFrames)*info.format.blockAlign;
                if (!WasapiNativeToFloat(info.format,rawSilent ? nullptr : raw.data()+static_cast<std::size_t>(rawOffset)*info.format.blockAlign,
                                         bytes,input.data(),input.size(),decodeFrames,rawSilent)) return E_UNEXPECTED;
                rawOffset+=decodeFrames;rawRemaining-=decodeFrames;inputOffset=0;inputRemaining=decodeFrames;
            }
            more=!budget || rawRemaining || inputRemaining;
            return S_OK;
        }

        HRESULT RenderPass(bool& more) noexcept {
            more=false;
            std::uint32_t writable=0;
            HRESULT hr=backend.RenderWritableFrames(writable);
            if (FAILED(hr) || !writable) return hr;
            if (writable>info.bufferFrames) return E_UNEXPECTED;
            const auto frames=(std::min)(writable,owner.config.maxFramesPerPass);
            const bool priming=owner.state.load(std::memory_order_relaxed)==WasapiBridgeState::Priming;
            bool underflow=false;
            if (!priming) {
                std::uint32_t produced=0;
                while (produced<frames) {
                    if (owner.Stopping()) return S_OK;
                    const auto p=src.Process(input.data()+static_cast<std::size_t>(inputOffset)*channels,inputRemaining,
                                             output.data()+static_cast<std::size_t>(produced)*channels,frames-produced);
                    inputOffset+=p.consumed; inputRemaining-=p.consumed;produced+=p.produced;
                    if (p.consumed || p.produced) continue;
                    if (inputRemaining) return E_UNEXPECTED;
                    const auto needed=static_cast<std::uint32_t>((std::max)(1.0,
                        (std::min)(static_cast<double>(inputCapacity),std::ceil((frames-produced)/src.Ratio()))));
                    const auto read=owner.io.ReadRenderInterleaved(input.data(),needed,input.size());
                    if (read<needed) {
                        owner.renderSilencedInputFrames.fetch_add(needed-read,std::memory_order_relaxed);
                        underflow=true;
                    }
                    for (std::size_t sample=0;sample<static_cast<std::size_t>(needed)*channels;++sample)
                        if (!std::isfinite(input[sample])) input[sample]=0;
                    inputOffset=0;inputRemaining=needed;
                }
            }
            if (owner.Stopping()) return S_OK;
            std::uint8_t* native=nullptr;
            hr=backend.AcquireRender(frames,native);
            if (FAILED(hr)) return hr;
            if (!priming && !WasapiFloatToNative(info.format,output.data(),output.size(),native,
                                                 static_cast<std::size_t>(frames)*info.format.blockAlign,frames)) {
                backend.ReleaseRender(0,true);return E_UNEXPECTED;
            }
            hr=backend.ReleaseRender(frames,priming);
            if (FAILED(hr)) return hr;
            owner.renderedFrames.fetch_add(frames,std::memory_order_relaxed);
            if (priming) owner.renderSilencedDeviceFrames.fetch_add(frames,std::memory_order_relaxed);
            if (underflow) {
                owner.state.store(WasapiBridgeState::Priming,std::memory_order_release);
                primingSince=owner.Now();
            }
            more=writable>frames;
            return S_OK;
        }
    };

    HRESULT RunChecked() {
        Apartment apartment;
        if (FAILED(apartment.status)) return apartment.status;
        FloatingPointMode floatingPoint;
        if (Stopping()) return cancelled;
        BackendLifetime lifetime{factory()};
        if (!lifetime.backend) return E_POINTER;
        auto& backend=*lifetime.backend;
        HRESULT hr=backend.Open(config.endpoint);
        if (FAILED(hr)) return hr;
        if (Stopping()) return cancelled;
        const auto* info=backend.Info();
        if (!info || info->kind!=config.endpoint.kind || info->format.channels!=io.Format().channels)
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        hr=ValidateWasapiStreamFormat(info->format,info->bufferFrames);
        if (FAILED(hr)) return hr;
        if (!IsSupportedWasapiSampleFormat(info->format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        Processing processing(*this,backend,*info); // All scratch/SRC allocated and touched before Start.
        Scheduling scheduling;
        mmcssError.store(scheduling.error,std::memory_order_relaxed);
        mmcssActive.store(scheduling.handle!=nullptr,std::memory_order_relaxed);
        if (Stopping()) return cancelled;
        hr=backend.Start();
        if (FAILED(hr)) return hr;
        if (Stopping()) return cancelled;
        state.store(WasapiBridgeState::Priming,std::memory_order_release);
        CompleteStartup(S_OK);
        bool more=true;
        bool observing=false;
        double observationStartFill=0;
        Clock::time_point observationSince{};
        while (!Stopping()) {
            if (!more) {
                const auto wait=backend.Wait(stop.value,config.waitTimeoutMs);
                if (wait.kind==WasapiWaitKind::Stop) break;
                if (wait.kind==WasapiWaitKind::Failed || FAILED(wait.status))
                    return FAILED(wait.status) ? wait.status : E_FAIL;
            }
            if (Stopping()) break;
            if(state.load(std::memory_order_relaxed)==WasapiBridgeState::Priming) {
                observing=false;
                processing.UpdateControl(processing.ControlFill());
            }
            if(!observing && state.load(std::memory_order_relaxed)==WasapiBridgeState::Running) {
                observationStartFill=processing.ControlFill();observationSince=Now();observing=true;
            }
            hr=processing.render ? processing.RenderPass(more) : processing.CapturePass(more);
            if (FAILED(hr)) return hr;
            // Observe the whole native drain, including retained input/output
            // SRC tails. Updating each bounded pass would assign the complete
            // wake interval to only the first fraction of a packet. Endpoints
            // give a burst-midpoint estimate, not an exact time integral when
            // graph callbacks interleave with a long drain.
            if(observing && state.load(std::memory_order_relaxed)==WasapiBridgeState::Running) {
                const bool prolonged=more && Now()-observationSince>=std::chrono::milliseconds(100);
                if(!more || prolonged) {
                    processing.UpdateControl((observationStartFill+processing.ControlFill())*0.5);
                    observing=false;
                    if(prolonged)controlBacklogUpdates.fetch_add(1,std::memory_order_relaxed);
                }
            } else observing=false;
        }
        return backend.Stop();
    }
};

WasapiBridge::WasapiBridge(WasapiBridgeConfig config,WasapiBridgeIo& io,BackendFactory factory)
    : impl_(std::make_unique<Impl>(std::move(config),io,std::move(factory))) {}
WasapiBridge::~WasapiBridge() { Stop(); }

HRESULT WasapiBridge::Start() noexcept {
    auto& p=*impl_;
    std::lock_guard<std::mutex> lock(p.lifecycle);
    if (p.used) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    p.used=true;
    HRESULT hr=p.Validate();
    if (FAILED(hr)) {p.error=hr;p.state=WasapiBridgeState::Failed;return hr;}
    p.io.BeginCapturePriming(); // Start completes before this generation is published.
    p.state.store(WasapiBridgeState::Starting,std::memory_order_release);
    try {p.worker=std::thread([&p]{p.Run();});}
    catch (...) {p.error=E_OUTOFMEMORY;p.state=WasapiBridgeState::Failed;return E_OUTOFMEMORY;}
    const DWORD wait=WaitForSingleObject(p.ready.value,INFINITE);
    if (wait!=WAIT_OBJECT_0) {RequestStop();return HRESULT_FROM_WIN32(GetLastError());}
    return p.startupResult.load(std::memory_order_acquire);
}
void WasapiBridge::RequestStop() noexcept { SetEvent(impl_->stop.value); }
void WasapiBridge::Stop() noexcept {
    RequestStop();
    auto& p=*impl_;
    std::lock_guard<std::mutex> lock(p.lifecycle);
    if (p.worker.joinable()) p.worker.join();
    if (!p.used) {p.used=true;p.state=WasapiBridgeState::Stopped;}
}
WasapiBridgeHealth WasapiBridge::Health() const noexcept {
    const auto& p=*impl_;
    WasapiBridgeHealth h;
    h.state=p.state.load(std::memory_order_acquire);h.error=p.error.load(std::memory_order_acquire);
    h.mmcssError=p.mmcssError.load(std::memory_order_relaxed);h.mmcssActive=p.mmcssActive.load(std::memory_order_relaxed);
    h.primingTimedOut=p.primingTimedOut.load(std::memory_order_relaxed);
    h.capturedFrames=p.capturedFrames.load(std::memory_order_relaxed);h.renderedFrames=p.renderedFrames.load(std::memory_order_relaxed);
    h.droppedCaptureFrames=p.droppedCaptureFrames.load(std::memory_order_relaxed);
    h.controlBacklogUpdates=p.controlBacklogUpdates.load(std::memory_order_relaxed);
    h.renderSilencedInputFrames=p.renderSilencedInputFrames.load(std::memory_order_relaxed);
    h.renderSilencedDeviceFrames=p.renderSilencedDeviceFrames.load(std::memory_order_relaxed);
    h.discontinuities=p.discontinuities.load(std::memory_order_relaxed);h.timestampErrors=p.timestampErrors.load(std::memory_order_relaxed);
    h.ratio=p.ratio.load(std::memory_order_relaxed);
    return h;
}
