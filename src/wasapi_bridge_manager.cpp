#include "wasapi_bridge_manager.h"
#include <audioclient.h>
#include <algorithm>
#include <sstream>
#include <thread>

namespace {
bool SameFormat(const WasapiMixFormat& a,const WasapiMixFormat& b) {
    return a.sampleRate==b.sampleRate && a.channels==b.channels && a.containerBits==b.containerBits &&
        a.validBits==b.validBits && a.blockAlign==b.blockAlign && a.channelMask==b.channelMask && a.encoding==b.encoding;
}
std::string Failure(const char* operation,HRESULT hr) {
    std::ostringstream text;text<<operation<<" (HRESULT 0x"<<std::hex<<static_cast<unsigned long>(hr)<<')';return text.str();
}
constexpr unsigned retryDelays[]={250,500,1000,2000,5000};
HRESULT ResolveEndpoint(const WasapiOpenConfig& config,std::wstring& resolved) {
    if(config.kind==WasapiStreamKind::ProcessLoopback)return S_OK;
    const auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(apartment) && apartment!=RPC_E_CHANGED_MODE)return apartment;
    struct ReleaseApartment {bool owned;~ReleaseApartment(){if(owned)CoUninitialize();}} release{SUCCEEDED(apartment)};
    std::string error;
    return WasapiDeviceEnumerator{}.resolve(config.kind==WasapiStreamKind::Capture?
        WasapiDataFlow::Capture:WasapiDataFlow::Render,config.endpointId,config.defaultRole,resolved,error)?S_OK:E_FAIL;
}
bool ProcessIdentityStillMatches(const WasapiOpenConfig& config) {
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,config.processId);
    if(!process)return false;
    struct Close {HANDLE value;~Close(){CloseHandle(value);}} close{process};
    FILETIME created{},exited{},kernel{},user{};
    if(!GetProcessTimes(process,&created,&exited,&kernel,&user))return false;
    const auto ticks=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    if(ticks!=config.processCreationTime)return false;
    std::wstring path(32768,L'\0');DWORD length=static_cast<DWORD>(path.size());
    if(!QueryFullProcessImageNameW(process,0,path.data(),&length))return false;
    path.resize(length);
    return CompareStringOrdinal(path.c_str(),static_cast<int>(path.size()),config.processImagePath.c_str(),
        static_cast<int>(config.processImagePath.size()),TRUE)==CSTR_EQUAL;
}
// Never reuse a generation even across a complete master-clock reconstruction.
const auto processGeneration=std::make_shared<std::atomic<std::uint64_t>>(1);
// Discovery and actual Open may race endpoint/default changes. Pin the resolved
// ID and require the entire effective format to match before publishing IO.
class VerifiedBackend final:public IWasapiBackend {
public:
    VerifiedBackend(std::unique_ptr<IWasapiBackend> backend,WasapiStreamInfo expected,
                    std::shared_ptr<WasapiStreamInfo> actual)
        : backend_(std::move(backend)),expected_(std::move(expected)),actual_(std::move(actual)) {}
    HRESULT Open(const WasapiOpenConfig& config)noexcept override {
        if(!backend_)return E_POINTER;
        const auto hr=backend_->Open(config);if(FAILED(hr))return hr;
        const auto* actual=backend_->Info();
        if(!actual || actual->endpointId!=expected_.endpointId || actual->kind!=expected_.kind ||
            !SameFormat(actual->format,expected_.format))return AUDCLNT_E_UNSUPPORTED_FORMAT;
        // Discovery sized the graph reserve before this second Open. A larger
        // native buffer invalidates that reserve; retry a fresh generation.
        if(actual->bufferFrames>expected_.bufferFrames)return AUDCLNT_E_BUFFER_SIZE_ERROR;
        try {*actual_=*actual;}catch(...){return E_OUTOFMEMORY;}
        return S_OK;
    }
    const WasapiStreamInfo* Info()const noexcept override{return backend_?backend_->Info():nullptr;}
    HRESULT Start()noexcept override{return backend_->Start();}
    HRESULT Stop()noexcept override{return backend_?backend_->Stop():S_OK;}
    void Close()noexcept override{if(backend_)backend_->Close();}
    WasapiWaitResult Wait(HANDLE stop,DWORD timeout)noexcept override{return backend_->Wait(stop,timeout);}
    HRESULT NextCapturePacketFrames(std::uint32_t& frames)noexcept override{return backend_->NextCapturePacketFrames(frames);}
    HRESULT AcquireCapture(WasapiCapturePacket& p)noexcept override{return backend_->AcquireCapture(p);}
    HRESULT ReleaseCapture(std::uint32_t frames)noexcept override{return backend_->ReleaseCapture(frames);}
    HRESULT RenderWritableFrames(std::uint32_t& frames)noexcept override{return backend_->RenderWritableFrames(frames);}
    HRESULT AcquireRender(std::uint32_t frames,std::uint8_t*& bytes)noexcept override{return backend_->AcquireRender(frames,bytes);}
    HRESULT ReleaseRender(std::uint32_t frames,bool silent)noexcept override{return backend_->ReleaseRender(frames,silent);}
private:
    std::unique_ptr<IWasapiBackend> backend_;
    WasapiStreamInfo expected_;
    std::shared_ptr<WasapiStreamInfo> actual_;
};

HRESULT Discover(const WasapiBridge::BackendFactory& factory,const WasapiOpenConfig& config,WasapiStreamInfo& info,
                 std::string& detail) {
    detail.clear();
    HRESULT result=E_FAIL;
    std::thread worker([&] {
        const auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(apartment)){result=apartment;return;}
        try {
            auto backend=factory();
            if(!backend) result=E_POINTER;
            else {
                result=backend->Open(config);
                if(FAILED(result)) {
                    if(const auto* message=backend->OpenErrorDetail())detail=message;
                }
                if(SUCCEEDED(result)) {
                    if(const auto* effective=backend->Info())info=*effective;
                    else result=E_UNEXPECTED;
                }
                backend->Close();backend.reset();
            }
        } catch(const std::bad_alloc&){result=E_OUTOFMEMORY;} catch(...){result=E_FAIL;}
        CoUninitialize();
    });
    worker.join();return result;
}
}

struct WasapiBridgeManager::Session {
    WasapiBridgeConfiguration config;
    std::optional<WasapiStreamInfo> effective;
    std::shared_ptr<WasapiBridgeIo> io;
    std::unique_ptr<WasapiBridge> bridge;
    WasapiBridgeHealth lastHealth;
    std::uint64_t generation=0;
    std::uint32_t retryAttempt=0;
    Clock::time_point retryAt{};
    bool identityStale=false;
    ~Session(){if(bridge)bridge->Stop();}
};

bool WasapiBridgeManager::Same(const WasapiBridgeConfiguration& a,const WasapiBridgeConfiguration& b) {
    return a.id==b.id && a.enabled==b.enabled && a.open.kind==b.open.kind &&
        a.open.endpointId==b.open.endpointId && a.open.defaultRole==b.open.defaultRole &&
        a.open.processId==b.open.processId && a.open.processCreationTime==b.open.processCreationTime &&
        a.open.processImagePath==b.open.processImagePath &&
        a.open.bufferDuration100ns==b.open.bufferDuration100ns;
}
bool WasapiBridgeManager::MatchesFormat(std::uint32_t rate,std::uint32_t quantum)const noexcept {
    return rate_==rate && quantum_==quantum;
}
std::shared_ptr<WasapiBridgeManager::Session> WasapiBridgeManager::Create(
    const WasapiBridgeConfiguration& config,std::string& error)const {
    auto session=std::make_shared<Session>();session->config=config;
    if(!config.enabled)return session;
    WasapiStreamInfo effective;
    std::string detail;
    const auto hr=Discover(factory_,config.open,effective,detail);
    if(FAILED(hr)) {
        if(config.open.kind==WasapiStreamKind::ProcessLoopback &&
            hr==HRESULT_FROM_WIN32(ERROR_INVALID_OWNER)) {
            session->identityStale=true;
            session->lastHealth.state=WasapiBridgeState::Failed;
            session->lastHealth.error=hr;
            session->retryAt=Clock::time_point::max();
            error.clear();
            return session;
        }
        error=Failure("WASAPI discovery failed",hr);
        if(!detail.empty())error+=": "+detail;
        return {};
    }
    const auto valid=ValidateWasapiStreamFormat(effective.format,effective.bufferFrames);
    if(FAILED(valid) || effective.endpointId.empty() || effective.kind!=config.open.kind) {
        error=Failure("Invalid WASAPI discovery format",FAILED(valid)?valid:E_UNEXPECTED);return {};
    }
    session->effective=effective;
    session->generation=nextGeneration_->fetch_add(1);
    const auto capacity=WasapiBridgeCapacityFrames(rate_,quantum_,effective.format.sampleRate,effective.bufferFrames);
    if(!capacity) {
        error="WASAPI FIFO reserve exceeds 65536 graph frames or has invalid dimensions "
            "(native buffer + 20 ms scheduler budget + graph quantum + SRC phase); "
            "choose a smaller native buffer or a closer sample-rate ratio";
        return {};
    }
    session->io=std::make_shared<WasapiBridgeIo>(WasapiBridgeFormat{
        config.open.kind==WasapiStreamKind::Render?WasapiBridgeDirection::Render:WasapiBridgeDirection::Capture,
        effective.format.channels,rate_,quantum_,session->generation,capacity});
    WasapiBridgeConfig options;options.endpoint=config.open;
    if(config.open.kind!=WasapiStreamKind::ProcessLoopback) {
        options.endpoint.endpointId=effective.endpointId;options.endpoint.defaultRole.reset();
    }
    auto factory=factory_;
    auto actual=std::make_shared<WasapiStreamInfo>();
    session->bridge=std::make_unique<WasapiBridge>(options,*session->io,[factory,effective,actual] {
        return std::make_unique<VerifiedBackend>(factory(),effective,actual);
    });
    const auto started=session->bridge->Start();
    if(FAILED(started)) {
        if(config.open.kind==WasapiStreamKind::ProcessLoopback && !ProcessIdentityStillMatches(config.open)) {
            session->bridge.reset();session->io.reset();session->effective.reset();
            session->identityStale=true;session->lastHealth.state=WasapiBridgeState::Failed;
            session->lastHealth.error=started;session->retryAt=Clock::time_point::max();error.clear();return session;
        }
        error=Failure("WASAPI worker preparation failed",started);return {};
    }
    session->effective=*actual; // Start handshake synchronizes the worker's Open metadata.
    return session;
}
std::shared_ptr<WasapiBridgeManager> WasapiBridgeManager::Prepare(
    const std::vector<WasapiBridgeConfiguration>& desired,std::uint32_t rate,std::uint32_t quantum,
    const std::shared_ptr<WasapiBridgeManager>& previous,const WasapiBridge::BackendFactory& factory,
    std::string& error,bool allowUnavailable,const EndpointResolver& resolver,
    const std::vector<std::string>& unavailableBridgeIds) {
    if(!ValidateWasapiBridgeConfigurations(desired,error))return {};
    auto candidate=std::make_shared<WasapiBridgeManager>();candidate->rate_=rate;candidate->quantum_=quantum;
    candidate->factory_=factory?factory:WasapiBridge::BackendFactory{CreateWasapiBackend};
    candidate->resolver_=resolver?resolver:EndpointResolver{ResolveEndpoint};
    candidate->nextDefaultCheck_=Clock::now()+std::chrono::seconds(2);
    candidate->nextGeneration_=previous?previous->nextGeneration_:processGeneration;
    for(const auto& config:desired) {
        std::shared_ptr<Session> session;
        if(previous && previous->MatchesFormat(rate,quantum))
            for(const auto& old:previous->sessions_)if(Same(config,old->config)){session=old;break;}
        if(!session)session=candidate->Create(config,error);
        const bool mayStayUnavailable=allowUnavailable ||
            std::find(unavailableBridgeIds.begin(),unavailableBridgeIds.end(),config.id)!=unavailableBridgeIds.end();
        if(!session && mayStayUnavailable) {
            session=std::make_shared<Session>();session->config=config;
            session->lastHealth.state=WasapiBridgeState::Failed;session->lastHealth.error=E_FAIL;
            session->retryAt=Clock::now()+std::chrono::milliseconds(retryDelays[0]);
        }
        if(!session)return {};
        candidate->sessions_.push_back(std::move(session));
    }
    error.clear();return candidate;
}

std::vector<WasapiRoutingBinding> WasapiBridgeManager::Bindings()const {
    std::vector<WasapiRoutingBinding> result;
    for(const auto& session:sessions_) {
        if(!session->bridge || !session->io || !session->effective)continue;
        const auto state=session->bridge->Health().state;
        if(state==WasapiBridgeState::Failed || state==WasapiBridgeState::Stopped)continue;
        result.push_back({session->config.id,session->io,session->effective->endpointId,
                          session->config.open.kind==WasapiStreamKind::EndpointLoopback});
    }
    return result;
}
std::vector<WasapiBridgeSnapshot> WasapiBridgeManager::Snapshots()const {
    std::vector<WasapiBridgeSnapshot> result;const auto now=Clock::now();
    for(const auto& session:sessions_) {
        WasapiBridgeSnapshot snapshot;snapshot.id=session->config.id;snapshot.effectiveInfo=session->effective;
        snapshot.health=session->bridge?session->bridge->Health():session->lastHealth;
        snapshot.generation=session->generation;snapshot.retryAttempt=session->retryAttempt;
        if(!session->config.enabled)snapshot.state="disabled";
        else if(session->identityStale)snapshot.state="unavailable";
        else if(!session->bridge)snapshot.state="retrying";
        else switch(snapshot.health.state) {
            case WasapiBridgeState::Priming:snapshot.state="priming";break;
            case WasapiBridgeState::Running:snapshot.state="running";break;
            case WasapiBridgeState::Failed:snapshot.state="failed";break;
            case WasapiBridgeState::Stopped:snapshot.state="stopped";break;
            default:snapshot.state="starting";break;
        }
        if(!session->identityStale && session->retryAt>now)snapshot.retryInMs=static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(session->retryAt-now).count());
        if(publicationFailures_ && !session->bridge && session->config.enabled) {
            snapshot.health.error=E_INVALIDARG;
            snapshot.retryAttempt=(std::max)(snapshot.retryAttempt,publicationFailures_);
            if(publicationRetryAt_>now)snapshot.retryInMs=(std::max)(snapshot.retryInMs,
                static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(publicationRetryAt_-now).count()));
        }
        if(session->io) {
            snapshot.graphFormat=session->io->Format();snapshot.bufferedFrames=session->io->BufferedFrames();
            snapshot.rejectedFrames=session->io->RejectedFrames();snapshot.silencedFrames=session->io->SilencedFrames();
        }
        result.push_back(std::move(snapshot));
    }
    return result;
}
std::shared_ptr<WasapiBridgeManager> WasapiBridgeManager::Reconcile(Clock::time_point now) {
    if(now<publicationRetryAt_)return {};
    const bool pollDefaults=now>=nextDefaultCheck_;
    if(pollDefaults)nextDefaultCheck_=now+std::chrono::seconds(2);
    std::shared_ptr<WasapiBridgeManager> candidate;
    for(std::size_t index=0;index<sessions_.size();++index) {
        const auto& current=sessions_[index];if(!current->config.enabled)continue;
        const bool failed=current->bridge && current->bridge->Health().state==WasapiBridgeState::Failed;
        const bool retry=!current->bridge && now>=current->retryAt;
        bool defaultChanged=false;
        if(!failed && !retry && pollDefaults && current->bridge && current->effective && current->config.open.defaultRole) {
            std::wstring resolved;
            defaultChanged=SUCCEEDED(resolver_(current->config.open,resolved)) && !resolved.empty() &&
                resolved!=current->effective->endpointId;
        }
        if(!failed && !retry && !defaultChanged)continue;
        if(!candidate) {
            candidate=std::make_shared<WasapiBridgeManager>(*this);
            candidate->publicationFailures_=0;candidate->publicationRetryAt_={};
        }
        std::shared_ptr<Session> replacement;
        if(retry || defaultChanged) {std::string error;replacement=Create(current->config,error);}
        if(!replacement) {
            replacement=std::make_shared<Session>();replacement->config=current->config;
            replacement->effective=current->effective;replacement->generation=current->generation;
            replacement->lastHealth=current->bridge?current->bridge->Health():current->lastHealth;
            replacement->lastHealth.state=WasapiBridgeState::Failed;
            replacement->retryAttempt=retry?current->retryAttempt+1:0;
            replacement->retryAt=now+std::chrono::milliseconds(retryDelays[(std::min)(replacement->retryAttempt,4U)]);
        }
        candidate->sessions_[index]=std::move(replacement);
    }
    return candidate;
}

void WasapiBridgeManager::DeferPublication(Clock::time_point now)noexcept {
    publicationRetryAt_=now+std::chrono::milliseconds(retryDelays[(std::min)(publicationFailures_,4U)]);
    if(publicationFailures_<UINT32_MAX)++publicationFailures_;
    nextDefaultCheck_=publicationRetryAt_;
}
