#pragma once
#include "native_audio_transport.h"
#include "wasapi_bridge_io.h"
#include <memory>

// Worker clock/deadline, not an audio timestamp or latency measurement.
// RequestStop is sticky and the only concurrently callable method.
class INativeAudioScheduler {
public:
    virtual ~INativeAudioScheduler() = default;
    virtual std::uint64_t NowNanoseconds() noexcept = 0;
    virtual bool WaitUntil(std::uint64_t deadlineNanoseconds) noexcept = 0;
    virtual void RequestStop() noexcept = 0;
};
std::unique_ptr<INativeAudioScheduler> CreateNativeAudioScheduler();

struct NativeAudioGeneration {
    const std::uint64_t epoch;
    const std::uint64_t wireGeneration;
    const std::shared_ptr<WasapiBridgeIo> windowsRenderSource;
    const std::shared_ptr<WasapiBridgeIo> windowsCaptureDestination;
};
enum class NativeAudioState { Idle, Starting, Running, Stopped, Failed };
struct NativeAudioHealth {
    NativeAudioState state = NativeAudioState::Idle;
    NativeTransferCode error = NativeTransferCode::Ok;
    std::uint32_t systemError = 0;
    std::uint64_t renderNativeFrames = 0, captureNativeFrames = 0;
    // Entire native silence packets GENERATED while priming (including pending
    // Full packets); distinct from accepted captureNativeFrames and driver silence.
    std::uint64_t captureSilencedNativeFrames = 0;
    // Missing graph input replaced with zeros before SRC. No native-frame or
    // audible-silence equivalence is claimed through the filter's retained tail.
    std::uint64_t captureSilencedGraphFrames = 0, skippedDeadlines = 0;
    std::uint64_t discardedPacingFrames = 0, controlBacklogUpdates = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t kernelRenderDroppedNativeFrames = 0, kernelCaptureDroppedNativeFrames = 0;
    std::uint64_t kernelCaptureSilencedNativeFrames = 0;
    double renderSourceRatio = 1, captureDestinationRatio = 1;
};

// One backend instance = one duplex generation. Construct dependencies off RT.
// Start is single-use, even on failure. Stop is idempotent.
// Start/Stop/Generation/destruction are serialized by the control owner;
// RequestStop and Health may run concurrently, including during Start/Open.
// Retain Generation until graph quiescence, even after Stop retires the worker.
// No method of this class is a graph callback: graph borrows the two IO pointers.
class NativeAudioBackend final {
public:
    NativeAudioBackend(std::unique_ptr<INativeAudioTransport>,
        std::unique_ptr<INativeAudioScheduler> = CreateNativeAudioScheduler());
    ~NativeAudioBackend();
    NativeAudioBackend(const NativeAudioBackend&) = delete;
    NativeAudioBackend& operator=(const NativeAudioBackend&) = delete;
    NativeTransferCode Start(std::uint32_t graphRate, std::uint32_t maxQuantum,
        std::uint64_t epoch) noexcept;
    void RequestStop() noexcept;
    void Stop() noexcept;
    std::shared_ptr<NativeAudioGeneration> Generation() const noexcept;
    NativeAudioHealth Health() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
