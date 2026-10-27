#pragma once
#include "wasapi_bridge_configuration.h"
#include "wasapi_bridge.h"
#include "audio_routing_runtime.h"
#include <chrono>

struct WasapiBridgeSnapshot {
    std::string id;
    std::string state;
    WasapiBridgeHealth health;
    std::optional<WasapiStreamInfo> effectiveInfo;
    std::optional<WasapiBridgeFormat> graphFormat;
    std::uint64_t generation=0,bufferedFrames=0,rejectedFrames=0,silencedFrames=0;
    std::uint32_t retryAttempt=0,retryInMs=0;
};

// Control-thread ownership only. A candidate shares unchanged sessions; new
// workers start before graph publication. Destroy old runtimes AFTER callback
// quiescence and BEFORE dropping the retired manager, so workers/IO never die
// while a callback still borrows their generation. No callback owns a manager.
class WasapiBridgeManager final {
public:
    using Clock=std::chrono::steady_clock;
    using EndpointResolver=std::function<HRESULT(const WasapiOpenConfig&,std::wstring&)>;
    static std::shared_ptr<WasapiBridgeManager> Prepare(
        const std::vector<WasapiBridgeConfiguration>& desired,std::uint32_t rate,std::uint32_t quantum,
        const std::shared_ptr<WasapiBridgeManager>& previous,const WasapiBridge::BackendFactory& factory,
        std::string& error,bool allowUnavailable=false,const EndpointResolver& resolver={},
        const std::vector<std::string>& unavailableBridgeIds={});
    // Null means no change due. Failures detach the old generation first;
    // retries use monotonic deadlines (250/500/1000/2000/5000 ms), no sleeps.
    std::shared_ptr<WasapiBridgeManager> Reconcile(Clock::time_point now);
    // A fully opened candidate can still violate graph constraints (for example
    // a default loopback resolving to its render destination). Back off these
    // failures too; never repeatedly open native streams each control tick.
    void DeferPublication(Clock::time_point now) noexcept;
    std::vector<WasapiRoutingBinding> Bindings() const;
    std::vector<WasapiBridgeSnapshot> Snapshots() const;
    bool MatchesFormat(std::uint32_t rate,std::uint32_t quantum) const noexcept;
private:
    struct Session;
    std::shared_ptr<Session> Create(const WasapiBridgeConfiguration& config,std::string& error) const;
    static bool Same(const WasapiBridgeConfiguration& a,const WasapiBridgeConfiguration& b);
    std::vector<std::shared_ptr<Session>> sessions_;
    WasapiBridge::BackendFactory factory_;
    EndpointResolver resolver_;
    std::shared_ptr<std::atomic<std::uint64_t>> nextGeneration_;
    std::uint32_t rate_=0,quantum_=0;
    Clock::time_point publicationRetryAt_{};
    std::uint32_t publicationFailures_=0;
    Clock::time_point nextDefaultCheck_{};
};
