#include "wasapi_bridge_manager.h"
#include "audio_controller.h"
#include "vasio_client_manager.h"
#include <audioclient.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void Await(F predicate,const char* message){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!predicate() && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Require(predicate(),message);
}
struct NativeState {
    std::atomic<unsigned> opens{0},starts{0},closes{0},serial{0},invalidateThrough{0};
    std::atomic<bool> wrongThread{false},mismatch{false},offline{false},defaultCollision{false};
    unsigned rate=48000,bufferFrames=128;
    bool growSecondBuffer=false;
    std::atomic<unsigned> captureAvailable{0};
};
class Backend final:public IWasapiBackend {
public:
    explicit Backend(std::shared_ptr<NativeState> state):s(std::move(state)),thread(GetCurrentThreadId()),serial(++s->serial){}
    ~Backend()override{Check();}
    HRESULT Open(const WasapiOpenConfig& config) noexcept override{
        Check();++s->opens;
        if(s->offline || config.endpointId==L"missing")return AUDCLNT_E_DEVICE_INVALIDATED;
        info.kind=config.kind;info.endpointId=config.endpointId.empty()?(s->defaultCollision?L"render":L"default"):config.endpointId;
        info.format={s->rate,2,32,32,8,3,WasapiSampleEncoding::IeeeFloat};
        if(s->mismatch && serial%2==0){info.format.channels=1;info.format.blockAlign=4;info.format.channelMask=1;}
        info.bufferFrames=s->bufferFrames+(s->growSecondBuffer && serial%2==0?1:0);return S_OK;
    }
    const WasapiStreamInfo* Info()const noexcept override{return &info;}
    const char* OpenErrorDetail()const noexcept override{return "Endpoint is not active";}
    HRESULT Start()noexcept override{Check();++s->starts;return S_OK;}
    HRESULT Stop()noexcept override{Check();return S_OK;}
    void Close()noexcept override{Check();++s->closes;}
    WasapiWaitResult Wait(HANDLE stop,DWORD timeout)noexcept override{
        Check();
        if(serial<=s->invalidateThrough)return {WasapiWaitKind::Failed,AUDCLNT_E_DEVICE_INVALIDATED};
        return WaitForSingleObject(stop,(std::min)(timeout,DWORD{10}))==WAIT_OBJECT_0
            ?WasapiWaitResult{WasapiWaitKind::Stop,S_OK}:WasapiWaitResult{WasapiWaitKind::Timeout,S_OK};
    }
    HRESULT NextCapturePacketFrames(std::uint32_t& n)noexcept override{Check();n=(std::min)(s->captureAvailable.load(),info.bufferFrames);return S_OK;}
    HRESULT AcquireCapture(WasapiCapturePacket& p)noexcept override{
        p={};p.frames=(std::min)(s->captureAvailable.load(),info.bufferFrames);p.silent=true;
        return p.frames?S_OK:AUDCLNT_S_BUFFER_EMPTY;
    }
    HRESULT ReleaseCapture(std::uint32_t frames)noexcept override{s->captureAvailable-=frames;return S_OK;}
    HRESULT RenderWritableFrames(std::uint32_t& n)noexcept override{n=0;return S_OK;}
    HRESULT AcquireRender(std::uint32_t,std::uint8_t*& p)noexcept override{p=nullptr;return E_FAIL;}
    HRESULT ReleaseRender(std::uint32_t,bool)noexcept override{return S_OK;}
private:
    void Check()noexcept{if(thread!=GetCurrentThreadId())s->wrongThread=true;}
    std::shared_ptr<NativeState>s;DWORD thread;unsigned serial;WasapiStreamInfo info;
};
WasapiBridgeConfiguration Bridge(const char* id,WasapiStreamKind kind,const wchar_t* endpoint){
    WasapiBridgeConfiguration b;b.id=id;b.open.kind=kind;b.open.endpointId=endpoint;return b;
}
void NativeCapacityPreparation() {
    std::string error;
    auto unavailable=std::make_shared<NativeState>();unavailable->offline=true;
    Require(!WasapiBridgeManager::Prepare({Bridge("offline",WasapiStreamKind::Capture,L"missing")},
        48000,256,{},[unavailable]{return std::make_unique<Backend>(unavailable);},error)
        && error.find("Endpoint is not active")!=std::string::npos
        && error.find("HRESULT")!=std::string::npos,
        "discovery discarded endpoint failure detail or HRESULT");
    const auto config=Bridge("source",WasapiStreamKind::Capture,L"capture");
    auto native=std::make_shared<NativeState>();native->rate=44100;native->bufferFrames=960;
    auto factory=[native]{return std::make_unique<Backend>(native);};
    auto manager=WasapiBridgeManager::Prepare({config},48000,16,{},factory,error);
    Require(manager!=nullptr,"small-quantum native reserve preparation failed");
    auto io=manager->Bindings().at(0).io;
    Require(io->Format().capacityFrames==WasapiBridgeCapacityFrames(48000,16,44100,960),
        "manager did not size FIFO from native buffer");
    native->captureAvailable=960;
    Await([&]{return io->BufferedFrames()==1045;},"initial native burst was dropped");
    float left[16],right[16];float* planes[]={left,right};
    Require(io->ReadCaptureBlock(planes,16,2)==0 && io->BufferedFrames()==1045,
        "graph drained capture before native-aware prefill");
    native->captureAvailable=960;
    Await([&]{return manager->Snapshots().at(0).state=="running";},"second burst never reached native-aware prefill");
    Require(io->ReadCaptureBlock(planes,16,2)==16 && !io->RejectedFrames(),"prefill lost native burst");
    manager.reset();io.reset();
    auto changed=std::make_shared<NativeState>();changed->growSecondBuffer=true;
    Require(!WasapiBridgeManager::Prepare({config},48000,16,{},[changed]{return std::make_unique<Backend>(changed);},error)
        && changed->starts==0,"larger second Open bypassed reserve validation");
    auto oversized=std::make_shared<NativeState>();oversized->rate=8000;oversized->bufferFrames=8000;
    Require(!WasapiBridgeManager::Prepare({config},384000,16,{},[oversized]{return std::make_unique<Backend>(oversized);},error)
        && oversized->starts==0 && error.find("65536")!=std::string::npos,
        "unrepresentable reserve was clamped or lacked diagnostic");
    for(auto rates : {std::pair<unsigned,unsigned>{8000,384000},{384000,8000}}) {
        auto extreme=std::make_shared<NativeState>();extreme->rate=rates.first;extreme->bufferFrames=rates.first/50;
        auto prepared=WasapiBridgeManager::Prepare({config},rates.second,16,{},[extreme]{return std::make_unique<Backend>(extreme);},error);
        Require(prepared && prepared->Bindings().at(0).io->Format().capacityFrames>=1024,
            "valid extreme ratio preparation failed");
    }
}
void Transactions(const std::wstring& path){
    auto native=std::make_shared<NativeState>();VasioClientManager clients;
    AudioController controller(clients,path,path+L".profiles",{},[native]{return std::make_unique<Backend>(native);});
    Require(controller.Start(),"controller startup");
    AudioControllerConfiguration config;config.clockSource=AudioClockSource::Software;config.sampleRate=48000;config.bufferFrames=64;
    config.wasapiBridges={Bridge("source",WasapiStreamKind::Capture,L"capture"),Bridge("sink",WasapiStreamKind::Render,L"render")};
    config.routes={{"through","wasapi:source:input:1","wasapi:sink:output:1",0,false}};
    Require(controller.ApplyConfiguration(config).success,"WASAPI graph configuration rejected");
    auto first=controller.Snapshot();
    Require(first.state=="running" && first.routes.size()==1 && first.wasapiBridges.size()==2 &&
            first.configuredWasapiBridges.size()==2,"bridge graph not published");
    const auto generation=first.wasapiBridges[0].generation;
    const auto opens=native->opens.load();
    config.routes[0].gainDb=-6;
    Require(controller.ApplyConfiguration(config,first.configurationRevision).success,"route-only transaction failed");
    auto routed=controller.Snapshot();
    Require(native->opens==opens && routed.wasapiBridges[0].generation==generation,"route edit reopened/reset IO generation");

    auto invalid=config;invalid.wasapiBridges.push_back(Bridge("bad",WasapiStreamKind::Capture,L"missing"));
    const auto rejected=controller.ApplyConfiguration(invalid);
    Require(!rejected.success,"missing endpoint transaction accepted");
    Require(rejected.error.find("Endpoint is not active")!=std::string::npos,
        "configuration result discarded endpoint diagnostic");
    auto preserved=controller.Snapshot();
    Require(preserved.configurationRevision==routed.configurationRevision && preserved.state=="running" &&
            preserved.routes[0].gainDb==-6 && preserved.wasapiBridges[0].generation==generation,
            "failed preparation destroyed old graph/configuration");

    HANDLE locked=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Require(locked!=INVALID_HANDLE_VALUE,"lock saved configuration");
    auto saveFailure=config;saveFailure.routes[0].mute=true;
    const auto failedSave=controller.ApplyConfiguration(saveFailure);
    CloseHandle(locked);
    Require(!failedSave.success,"locked store unexpectedly saved");
    preserved=controller.Snapshot();
    Require(preserved.configurationRevision==routed.configurationRevision && preserved.state=="running" &&
            !preserved.routes[0].mute && preserved.wasapiBridges[0].generation==generation,
            "store failure did not preserve old runtime");

    // The second Open must match discovery even if channel count stays supported.
    native->mismatch=true;
    auto changed=config;changed.wasapiBridges[0].open.endpointId=L"changed";
    Require(!controller.ApplyConfiguration(changed).success,"discovery/Open format race accepted");
    native->mismatch=false;
    Require(controller.Snapshot().wasapiBridges[0].generation==generation &&
            controller.Snapshot().configurationRevision==routed.configurationRevision,
            "format mismatch changed live generation");

    locked=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Require(locked!=INVALID_HANDLE_VALUE,"lock store before master change");
    auto masterChange=config;masterChange.bufferFrames=128;
    const auto failedMaster=controller.ApplyConfiguration(masterChange);CloseHandle(locked);
    Require(!failedMaster.success && controller.Snapshot().state=="running" &&
            controller.Snapshot().bufferFrames==64 &&
            controller.Snapshot().configurationRevision==routed.configurationRevision,
            "failed master transaction was not restored");

    auto omitted=config;omitted.wasapiBridges.clear();
    Require(controller.ApplyConfiguration(omitted,std::nullopt,false,true).success &&
            controller.Snapshot().configuredWasapiBridges.size()==2,"omitted bridges not preserved transactionally");
    const auto beforeInvalidation=controller.Snapshot().wasapiBridges[0].generation;
    const auto opensBeforeInvalidation=native->opens.load();
    native->invalidateThrough=native->serial.load();
    Await([&]{const auto s=controller.Snapshot();return s.wasapiBridges.size()==2 &&
        s.wasapiBridges[0].generation>beforeInvalidation && native->opens>opensBeforeInvalidation &&
        (s.wasapiBridges[0].state=="running" || s.wasapiBridges[0].state=="priming") && s.routes.size()==1;},
        "invalidated bridge did not recover with a new generation");
    Require(controller.Snapshot().state=="running","slave failure stopped master");
    auto clear=config;clear.wasapiBridges.clear();
    Require(controller.ApplyConfiguration(clear).success,"explicit empty bridge list failed");
    auto empty=controller.Snapshot();
    Require(empty.routes.empty() && empty.configuredRoutes.size()==1 && empty.configuredWasapiBridges.empty(),
            "removed bridge route not retained dormant");
    controller.Stop();Require(!native->wrongThread,"native ownership escaped worker");
}

struct ClockTrace { unsigned starts=0;std::vector<std::uint32_t> inputs,outputs; };
class Clock final:public AudioClockHost {
public:
    explicit Clock(std::shared_ptr<ClockTrace> trace={}):trace_(std::move(trace)){}
    std::vector<AudioClockDeviceInfo> enumerate()const override{return {{L"fake","Fake clock"}};}
    bool open(const std::wstring&,void*,AudioClockCapabilities& output,std::string&)override {
        caps={};caps.currentSampleRate=48000;caps.minBufferFrames=16;caps.maxBufferFrames=256;
        caps.preferredBufferFrames=64;caps.bufferGranularity=-1;
        if(trace_){caps.inputChannels=4;caps.outputChannels=4;}output=caps;return true;
    }
    bool setSampleRate(double rate,std::string&)override{caps.currentSampleRate=rate;return true;}
    bool refreshCapabilities(AudioClockCapabilities& output,std::string&)override{output=caps;return true;}
    bool consumeSampleRateChange()noexcept override{return false;}
    std::uint32_t consumeErrorCode()noexcept override{return 0;}
    AudioClockTimingCounters consumeTimingCounters()noexcept override{return {};}
    void setStageMeasurementsEnabled(bool)noexcept override{}
    StageTimingCounters consumeOutputCopyStageCounters()noexcept override{return {};}
    bool start(std::uint32_t,const std::vector<std::uint32_t>& inputs,const std::vector<std::uint32_t>& outputs,
               AudioClockProcessCallback,void*,std::string&)override{
        if(trace_){++trace_->starts;trace_->inputs=inputs;trace_->outputs=outputs;}return true;
    }
    void stop()noexcept override{}
    void close()noexcept override{}
private:AudioClockCapabilities caps;std::shared_ptr<ClockTrace> trace_;
};

void OfflineRestoreAndStop(const std::wstring& path) {
    DeleteFileW(path.c_str());
    AudioControllerConfiguration config;config.physicalDriverId="fake";config.sampleRate=48000;config.bufferFrames=64;
    config.wasapiBridges={Bridge("source",WasapiStreamKind::Capture,L"capture"),Bridge("sink",WasapiStreamKind::Render,L"render")};
    config.routes={{"through","wasapi:source:input:1","wasapi:sink:output:1",0,false}};
    std::string error;Require(AudioConfigurationStore(path).Save(config,error),"prepare saved offline configuration");
    auto native=std::make_shared<NativeState>();native->offline=true;VasioClientManager clients;
    AudioController controller(clients,path,path+L".profiles",[]{return std::make_unique<Clock>();},
        [native]{return std::make_unique<Backend>(native);});
    Require(controller.Start(),"offline restore startup");
    Require(controller.Snapshot().state=="running" && controller.Snapshot().routes.empty(),
        "selected ASIO master did not remain live for enabled dormant bridges");
    native->offline=false;
    Await([&]{return controller.Snapshot().routes.size()==1;},"restored offline bridges never retried");
    Require(controller.StopAudio().success,"explicit stop failed");
    const auto opens=native->opens.load();std::this_thread::sleep_for(std::chrono::milliseconds(350));
    Require(controller.Snapshot().state=="stopped" && native->opens==opens,"explicit stop still retried bridges");
    controller.Stop();
}

void NativeDesireKeepsMaster(const std::wstring& path) {
    DeleteFileW(path.c_str());
    AudioControllerConfiguration config;
    config.physicalDriverId="fake";config.sampleRate=48000;config.bufferFrames=64;
    config.nativeAudio.enabled=true;
    std::string error;Require(AudioConfigurationStore(path).Save(config,error),"save native master fixture");
    VasioClientManager clients;
    AudioController controller(clients,path,path+L".profiles",[]{return std::make_unique<Clock>();});
    Require(controller.Start(),"native desired master startup");
    Require(controller.Snapshot().state=="running" && controller.Snapshot().routes.empty() &&
        controller.Snapshot().configuredNativeAudio.enabled,
        "native desire must keep ASIO master ready while native routes are unavailable");
    Require(controller.StopAudio().success && controller.Snapshot().state=="stopped",
        "explicit StopAudio overrides native desire");
    Require(controller.StartAudio().success && controller.Snapshot().state=="running",
        "explicit StartAudio restores master for native desire");
    controller.Stop();
}

void DormantNativePhysicalReservations(const std::wstring& path) {
    DeleteFileW(path.c_str());
    auto trace=std::make_shared<ClockTrace>();VasioClientManager clients;
    AudioController controller(clients,path,path+L".profiles",[trace]{return std::make_unique<Clock>(trace);});
    Require(controller.Start(),"reservation startup");
    AudioControllerConfiguration config;config.physicalDriverId="fake";
    config.sampleRate=48000;config.bufferFrames=64;config.nativeAudio.enabled=true;
    config.routes={{"in","physical:fake:input:3","native:windows:output:1",-6,true},
        {"out","native:windows:input:2","physical:fake:output:4",-3,false},
        {"bad-driver","native:windows:input:1","physical:other:output:1",0,false},
        {"bad-range","native:windows:input:1","physical:fake:output:5",0,false},
        {"bad-number","native:windows:input:1","physical:fake:output:01",0,false},
        {"bad-suffix","native:windows:input:1","physical:fake:output:2x",0,false},
        {"bad-native","native:windows:input:3","physical:fake:output:2",0,false},
        {"bad-direction","native:windows:output:1","physical:fake:input:1",0,false}};
    Require(controller.ApplyConfiguration(config,std::nullopt,false,false,false).success,"dormant native physical setup");
    Require(trace->inputs==std::vector<std::uint32_t>{2} && trace->outputs==std::vector<std::uint32_t>{3},
        "dormant native routes did not reserve sparse global channels");
    Require(controller.Snapshot().routes.empty(),"unavailable native routes activated");
    const auto starts=trace->starts;
    config.routes[0].gainDb=-12;config.routes[0].mute=false;
    Require(controller.ApplyConfiguration(config).success && trace->starts==starts,
        "compatible dormant route edit reopened master");
    auto snapshot=controller.Snapshot();
    Require(snapshot.configuredRoutes[0].gainDb==-12 && !snapshot.configuredRoutes[0].mute,
        "reservation changed desired gain or mute");
    config.routes.push_back({"extra","native:windows:input:1","physical:fake:output:2",0,false});
    Require(controller.ApplyConfiguration(config).success && trace->starts==starts+1 &&
        trace->outputs==std::vector<std::uint32_t>({1,3}),"new dormant reservation bypassed host restart");
    const auto expanded=trace->starts;
    config.routes.pop_back();
    Require(controller.ApplyConfiguration(config).success && trace->starts==expanded,
        "reserved superset was not reusable");
    controller.Stop();
}

void PublicationBackoff(const std::wstring& path) {
    DeleteFileW(path.c_str());auto native=std::make_shared<NativeState>();VasioClientManager clients;
    AudioController controller(clients,path,path+L".profiles",{},[native]{return std::make_unique<Backend>(native);});
    Require(controller.Start(),"publication test startup");
    AudioControllerConfiguration config;config.clockSource=AudioClockSource::Software;config.sampleRate=48000;config.bufferFrames=64;
    auto loop=Bridge("loop",WasapiStreamKind::EndpointLoopback,L"");loop.open.defaultRole=WasapiDefaultRole::Console;
    config.wasapiBridges={loop,Bridge("sink",WasapiStreamKind::Render,L"render")};
    config.routes={{"through","wasapi:loop:input:1","wasapi:sink:output:1",0,false}};
    Require(controller.ApplyConfiguration(config).success,"initial safe loopback route");
    native->defaultCollision=true;native->invalidateThrough=native->serial.load();
    Await([&]{return controller.Snapshot().lastError.find("reconnection rejected")!=std::string::npos;},
        "unsafe recovered loopback was not rejected visibly");
    const auto opens=native->opens.load();std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Require(native->opens==opens,"publication failure reopened streams every control tick");
    Await([&]{const auto snapshot=controller.Snapshot();return native->opens>opens &&
        !snapshot.wasapiBridges.empty() && snapshot.wasapiBridges[0].retryAttempt>=2;},
        "publication rejection did not use increasing retry backoff");
    Require(controller.Snapshot().state=="running" && controller.Snapshot().routes.empty() &&
        controller.Snapshot().configuredRoutes.size()==1,"publication rejection destroyed master or saved route");
    controller.Stop();
}

void ActiveDefaultFollowsRole(const std::wstring& path) {
    DeleteFileW(path.c_str());auto native=std::make_shared<NativeState>();VasioClientManager clients;
    std::atomic<unsigned> resolutions{0};
    AudioController controller(clients,path,path+L".profiles",{},[native]{return std::make_unique<Backend>(native);},
        [&](const WasapiOpenConfig&,std::wstring& endpoint){
            ++resolutions;endpoint=native->defaultCollision?L"render":L"default";return S_OK;
        });
    Require(controller.Start(),"default-role test startup");
    AudioControllerConfiguration config;config.clockSource=AudioClockSource::Software;config.sampleRate=48000;config.bufferFrames=64;
    auto bridge=Bridge("source",WasapiStreamKind::Capture,L"");bridge.open.defaultRole=WasapiDefaultRole::Console;
    config.wasapiBridges={bridge};
    Require(controller.ApplyConfiguration(config).success,"default-role configuration");
    const auto generation=controller.Snapshot().wasapiBridges[0].generation;
    const auto opens=native->opens.load();
    Await([&]{return resolutions>0;},"active role was not polled");
    Require(native->opens==opens && controller.Snapshot().wasapiBridges[0].generation==generation,
        "unchanged default unnecessarily reopened stream");
    native->defaultCollision=true;
    Await([&]{const auto s=controller.Snapshot();return s.wasapiBridges[0].generation>generation &&
        s.wasapiBridges[0].effectiveInfo && s.wasapiBridges[0].effectiveInfo->endpointId==L"render";},
        "active default-role change did not publish a new endpoint generation");
    Require(controller.Snapshot().state=="running","role change stopped master");
    controller.Stop();
}
}
int main(int argc,char** argv){
    wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
    const auto path=std::wstring(temp)+L"TimoxVasio-controller-wasapi-"+std::to_wstring(GetCurrentProcessId())+L".json";
    DeleteFileW(path.c_str());
    if(argc>1&&std::string(argv[1])=="--inventory-probe") {
        try {
            VasioClientManager clients;AudioController controller(clients,path,path+L".profiles");
            Require(controller.Start(),"inventory controller start");
            const auto before=controller.Snapshot();
            controller.EnumerateWasapiDevices();
            // The first request publishes once. The observer's initial pending
            // change must cause a SECOND publication on the idle control worker.
            Await([&]{return controller.Snapshot().revision>=before.revision+2;},"idle worker did not refresh subscribed inventory");
            const auto after=controller.Snapshot();
            Require(after.wasapiInventoryError.empty(),"real inventory refresh failed");
            Require(after.state==before.state,"inventory refresh changed audio state");
            Require(controller.EventsSince(before.revision).size()>=2,"inventory publication journal missing");
            controller.Stop();
            std::cout<<"Idle worker inventory refresh: "<<after.wasapiDevices.size()<<" endpoints; state="<<after.state<<'\n';
            DeleteFileW(path.c_str());DeleteFileW((path+L".profiles").c_str());return 0;
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';DeleteFileW(path.c_str());return 1;}
    }
    try{DormantNativePhysicalReservations(path);NativeCapacityPreparation();Transactions(path);OfflineRestoreAndStop(path);PublicationBackoff(path);ActiveDefaultFollowsRole(path);NativeDesireKeepsMaster(path);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';DeleteFileW(path.c_str());return 1;}
    DeleteFileW(path.c_str());DeleteFileW((path+L".profiles").c_str());
    std::cout<<"Audio controller WASAPI tests passed\n";
}
