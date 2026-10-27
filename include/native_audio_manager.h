#pragma once
#include "native_audio_backend.h"
#include <atomic>
#include <chrono>
#include <functional>

enum class NativeAudioAction { None, Detach, AttemptOpen };
enum class NativeAudioManagerState { Disabled, WaitingForMaster, Prepared, Attached, Detaching, Retrying, Failed };
struct NativeAudioManagerSnapshot {
    NativeAudioManagerState state=NativeAudioManagerState::Disabled;
    bool enabled=false,masterRunning=false,attached=false;
    std::uint32_t desiredRate=0,desiredQuantum=0,graphRate=0,graphQuantum=0;
    std::uint64_t epoch=0,wireGeneration=0;
    // Worker health, or its last observation after retirement. Manager state
    // and attached are authoritative for current graph availability: retained
    // health may still say Running while the manager is now Disabled.
    NativeAudioHealth health;
    NativeTransferResult lastResult{NativeTransferCode::Ok};
    std::uint32_t retryAttempt=0,retryInMs=0;
};

// Control-owner only: no method belongs in a graph callback. SetDesired and
// Inspect never open/stop a transport. The owner publishes/removes runtimes and
// guarantees callback quiescence before StopAfterQuiescence. A backend that has
// failed still requires Detach before another Open; borrowed IO must be retired.
class NativeAudioManager final {
public:
    using Clock=std::chrono::steady_clock;
    using TransportFactory=std::function<std::unique_ptr<INativeAudioTransport>()>;
    using SchedulerFactory=std::function<std::unique_ptr<INativeAudioScheduler>()>;
    // Missing factory means unavailable, never the direct laboratory adapter.
    // Share an injected epoch allocator across manager replacements. The default
    // allocator is process-wide; zero/exhaustion is rejected without wrapping.
    NativeAudioManager(TransportFactory={},SchedulerFactory={},
        std::shared_ptr<std::atomic<std::uint64_t>> epochs={});
    // Destruction also requires all graph borrowers to have quiesced.
    ~NativeAudioManager();
    void SetDesired(bool enabled,bool masterRunning,std::uint32_t rate,std::uint32_t quantum) noexcept;
    NativeAudioAction Inspect(Clock::time_point now) const noexcept;
    NativeTransferResult AttemptOpen(Clock::time_point now) noexcept;
    bool MarkAttached() noexcept;
    // A publication failure can explicitly stop automatic retry; pass its typed
    // error (e.g. InvalidPacket for graph validation). Ok uses backend health.
    void StopAfterQuiescence(Clock::time_point now,
        NativeTransferResult reason={NativeTransferCode::Ok}) noexcept;
    std::shared_ptr<NativeAudioGeneration> Generation() const noexcept;
    NativeAudioManagerSnapshot Snapshot(Clock::time_point now) const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
