#include "native_audio_manager.h"
#include <algorithm>
#include <limits>

namespace {
const auto processEpochs=std::make_shared<std::atomic<std::uint64_t>>(1);
bool Retryable(NativeTransferCode code)noexcept {
    return code==NativeTransferCode::Unavailable || code==NativeTransferCode::Removed;
}
}
struct NativeAudioManager::Impl {
    TransportFactory transportFactory;SchedulerFactory schedulerFactory;
    std::shared_ptr<std::atomic<std::uint64_t>> epochs;
    std::unique_ptr<NativeAudioBackend> backend;
    bool enabled=false,running=false,valid=true,attached=false,attemptAllowed=true;
    std::uint32_t rate=0,quantum=0,actualRate=0,actualQuantum=0,retryAttempt=0;
    NativeTransferResult lastResult{NativeTransferCode::Ok};
    NativeAudioHealth lastHealth;
    Clock::time_point retryAt{};
    bool Matches()const noexcept{return enabled && running && valid && rate==actualRate && quantum==actualQuantum;}
    void Failure(NativeTransferResult result,Clock::time_point now)noexcept {
        lastResult=result;attemptAllowed=Retryable(result.code);
        if(!attemptAllowed){retryAt={};return;}
        constexpr unsigned seconds[]={1,2,4,8,30};
        const auto delay=std::chrono::seconds(seconds[(std::min)(retryAttempt,4u)]);
        if(retryAttempt<(std::numeric_limits<std::uint32_t>::max)())++retryAttempt;
        retryAt=now>Clock::time_point::max()-delay?Clock::time_point::max():now+delay;
    }
    std::uint64_t AllocateEpoch()noexcept {
        auto next=epochs->load(std::memory_order_relaxed);
        while(next && next!=(std::numeric_limits<std::uint64_t>::max)())
            if(epochs->compare_exchange_weak(next,next+1,std::memory_order_relaxed))return next;
        return 0;
    }
};
NativeAudioManager::NativeAudioManager(TransportFactory transport,SchedulerFactory scheduler,
    std::shared_ptr<std::atomic<std::uint64_t>> epochs):impl_(std::make_unique<Impl>()) {
    impl_->transportFactory=std::move(transport);impl_->schedulerFactory=std::move(scheduler);
    impl_->epochs=epochs?std::move(epochs):processEpochs;
}
// Like StopAfterQuiescence, destruction requires the control owner to have
// retired every graph borrower. NativeAudioBackend then joins its worker.
NativeAudioManager::~NativeAudioManager()=default;
void NativeAudioManager::SetDesired(bool enabled,bool running,std::uint32_t rate,std::uint32_t quantum)noexcept {
    auto& p=*impl_;
    const bool changed=p.enabled!=enabled || p.running!=running || p.rate!=rate || p.quantum!=quantum;
    if(!changed)return;
    p.enabled=enabled;p.running=running;p.rate=rate;p.quantum=quantum;
    p.valid=!enabled || !running || WasapiBridgeCapacityFrames(rate,quantum,48000,4096)!=0;
    p.attemptAllowed=p.valid;p.retryAttempt=0;p.retryAt={};
    // Explicit intent/clock changes begin a new attempt policy. Reapplying the
    // identical desired state leaves denial and retry deadlines untouched.
    p.lastResult={p.valid?NativeTransferCode::Ok:NativeTransferCode::InvalidPacket};
    if(!p.valid){p.lastHealth.state=NativeAudioState::Failed;p.lastHealth.error=NativeTransferCode::InvalidPacket;}
}
NativeAudioAction NativeAudioManager::Inspect(Clock::time_point now)const noexcept {
    const auto& p=*impl_;
    if(p.backend){
        const auto state=p.backend->Health().state;
        if(!p.Matches() || state==NativeAudioState::Failed || state==NativeAudioState::Stopped)
            return NativeAudioAction::Detach;
        return NativeAudioAction::None;
    }
    if(p.enabled && p.running && p.valid && p.attemptAllowed && now>=p.retryAt)return NativeAudioAction::AttemptOpen;
    return NativeAudioAction::None;
}
NativeTransferResult NativeAudioManager::AttemptOpen(Clock::time_point now)noexcept {
    auto& p=*impl_;
    if(Inspect(now)!=NativeAudioAction::AttemptOpen)return {NativeTransferCode::Failed};
    if(!p.transportFactory){
        p.lastHealth={};p.lastHealth.state=NativeAudioState::Failed;p.lastHealth.error=NativeTransferCode::Unavailable;
        p.Failure({NativeTransferCode::Unavailable},now);return p.lastResult;
    }
    try{
        const auto epoch=p.AllocateEpoch();
        if(!epoch){p.Failure({NativeTransferCode::Failed},now);p.lastHealth.state=NativeAudioState::Failed;p.lastHealth.error=NativeTransferCode::Failed;return p.lastResult;}
        auto transport=p.transportFactory();
        if(!transport){p.Failure({NativeTransferCode::Unavailable},now);p.lastHealth.state=NativeAudioState::Failed;p.lastHealth.error=NativeTransferCode::Unavailable;return p.lastResult;}
        auto scheduler=p.schedulerFactory?p.schedulerFactory():CreateNativeAudioScheduler();
        p.backend=std::make_unique<NativeAudioBackend>(std::move(transport),std::move(scheduler));
        p.actualRate=p.rate;p.actualQuantum=p.quantum;p.attached=false;
        const auto result=p.backend->Start(p.rate,p.quantum,epoch);p.lastHealth=p.backend->Health();
        p.lastResult={result,0,p.lastHealth.systemError};
        if(result==NativeTransferCode::Ok){p.retryAttempt=0;p.retryAt={};p.attemptAllowed=true;}
        // Failed Start objects remain owned until explicit retirement. The
        // controller must acknowledge even an unpublished failed preparation.
        return p.lastResult;
    }catch(...){p.lastHealth.state=NativeAudioState::Failed;p.lastHealth.error=NativeTransferCode::Failed;
        p.Failure({NativeTransferCode::Failed},now);return p.lastResult;}
}
bool NativeAudioManager::MarkAttached()noexcept {
    auto& p=*impl_;
    if(!p.backend || !p.Matches() || p.backend->Health().state!=NativeAudioState::Running)return false;
    p.attached=true;return true;
}
void NativeAudioManager::StopAfterQuiescence(Clock::time_point now,NativeTransferResult reason)noexcept {
    auto& p=*impl_;if(!p.backend)return;
    const bool sameIntent=p.Matches();p.lastHealth=p.backend->Health();
    const auto result=NativeTransferResult{p.lastHealth.error,0,p.lastHealth.systemError};
    const bool failed=p.lastHealth.state==NativeAudioState::Failed;
    p.attached=false;p.backend->Stop();p.backend.reset();p.actualRate=p.actualQuantum=0;
    if(sameIntent && reason.code!=NativeTransferCode::Ok)p.Failure(reason,now);
    else if(sameIntent && failed)p.Failure(result,now);
    else {p.retryAttempt=0;p.retryAt={};p.attemptAllowed=p.valid;}
}
std::shared_ptr<NativeAudioGeneration> NativeAudioManager::Generation()const noexcept {
    return impl_->backend?impl_->backend->Generation():nullptr;
}
NativeAudioManagerSnapshot NativeAudioManager::Snapshot(Clock::time_point now)const noexcept {
    const auto& p=*impl_;NativeAudioManagerSnapshot result;
    result.enabled=p.enabled;result.masterRunning=p.running;result.attached=p.attached;
    result.desiredRate=p.rate;result.desiredQuantum=p.quantum;
    result.health=p.backend?p.backend->Health():p.lastHealth;result.lastResult=p.lastResult;
    result.retryAttempt=p.retryAttempt;
    if(const auto generation=Generation()){
        result.epoch=generation->epoch;result.wireGeneration=generation->wireGeneration;
        result.graphRate=p.actualRate;result.graphQuantum=p.actualQuantum;
    }
    if(p.backend && Inspect(now)==NativeAudioAction::Detach)result.state=NativeAudioManagerState::Detaching;
    else if(!p.enabled)result.state=NativeAudioManagerState::Disabled;
    else if(!p.running)result.state=NativeAudioManagerState::WaitingForMaster;
    else if(p.backend)result.state=p.attached?NativeAudioManagerState::Attached:NativeAudioManagerState::Prepared;
    else result.state=p.valid && p.attemptAllowed?NativeAudioManagerState::Retrying:NativeAudioManagerState::Failed;
    if(!p.backend && p.attemptAllowed && p.enabled && p.running && now<p.retryAt){
        const auto milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(p.retryAt-now).count();
        result.retryInMs=static_cast<std::uint32_t>((std::min)(milliseconds,static_cast<decltype(milliseconds)>((std::numeric_limits<std::uint32_t>::max)())));
    }
    return result;
}
