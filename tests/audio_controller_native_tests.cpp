#include "audio_controller.h"
#include "native_audio_manager.h"
#include "native_audio_api.h"
#include "native_audio_packet_validation.h"
#include "vasio_client_manager.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
void Require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void PublicStateProjection(){
    NativeAudioManagerSnapshot s;
    s.health.state=NativeAudioState::Running;
    Require(NativeAudioApiJson(s)["state"]=="disabled","retained worker health cannot make disabled backend running");
    s.enabled=true;s.state=NativeAudioManagerState::WaitingForMaster;
    Require(NativeAudioApiJson(s)["state"]=="starting","master absent must not report running");
    s.state=NativeAudioManagerState::Attached;s.attached=true;s.graphRate=44100;s.graphQuantum=128;
    s.epoch=UINT64_MAX;s.wireGeneration=UINT64_MAX;
    const auto running=NativeAudioApiJson(s);
    Require(running["state"]=="running" && running["graphFormat"]["sampleRate"]==44100 &&
        running["nativeFormat"]["sampleRate"]==48000 && running["epoch"]=="18446744073709551615" &&
        running["wireGeneration"]=="18446744073709551615",
        "native and graph rates stay distinct; generation retains full precision");
    s.state=NativeAudioManagerState::Detaching;s.health.state=NativeAudioState::Failed;
    for(const auto code:{NativeTransferCode::AccessDenied,NativeTransferCode::OwnedElsewhere,NativeTransferCode::Removed,NativeTransferCode::Unavailable,NativeTransferCode::InvalidPacket}){
        s.health.error=code;s.health.systemError=5;
        const auto status=NativeAudioApiJson(s);
        const char* expected=code==NativeTransferCode::AccessDenied?"denied":code==NativeTransferCode::OwnedElsewhere?"ownedElsewhere":code==NativeTransferCode::Removed?"removed":code==NativeTransferCode::Unavailable?"unavailable":"failed";
        Require(status["state"]==expected && status["graphFormat"].is_null() && status["systemError"]==5,"worker failure retains typed status without running graph format");
    }
    s.enabled=false;
    Require(NativeAudioApiJson(s)["state"]=="disabled","disabled state overrides historical failure");
    s={};s.enabled=true;s.state=NativeAudioManagerState::Retrying;
    s.lastResult={NativeTransferCode::Unavailable,0,2};
    Require(NativeAudioApiJson(s)["state"]=="unavailable" && NativeAudioApiJson(s)["systemError"]==2,
        "missing transport uses last attempt error without worker health");
    s.state=NativeAudioManagerState::Failed;s.lastResult={NativeTransferCode::AccessDenied,0,5};
    Require(NativeAudioApiJson(s)["state"]=="denied","failed admission remains distinct from absence");
}
template<class Predicate> void Await(Predicate predicate,const char* why){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6);
    while(!predicate()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Require(predicate(),why);
}
struct ClockState {
    std::atomic<unsigned> starts{0},stops{0},callbacks{0},failRate{0};
    std::atomic<bool> heardNative{false};
    std::mutex mutex;std::vector<std::uint32_t> inputs,outputs;
};
class Master final:public AudioClockHost {
    std::shared_ptr<ClockState> state;AudioClockCapabilities caps;
    std::thread thread;std::atomic<bool> stopped{true};
    std::array<float,8192> left{},right{},outLeft{},outRight{};
public:
    explicit Master(std::shared_ptr<ClockState> s):state(std::move(s)){left.fill(.25f);right.fill(-.25f);}
    ~Master()override{stop();}
    std::vector<AudioClockDeviceInfo> enumerate()const override{return {{L"fake","Native test master"}};}
    bool open(const std::wstring&,void*,AudioClockCapabilities& output,std::string&)override{
        caps={};caps.inputChannels=caps.outputChannels=2;caps.currentSampleRate=48000;
        caps.minBufferFrames=16;caps.maxBufferFrames=8192;caps.preferredBufferFrames=256;caps.bufferGranularity=-1;
        output=caps;return true;
    }
    bool setSampleRate(double rate,std::string& error)override{
        if(rate==state->failRate.load()){error="Injected master rate rejection";return false;}
        caps.currentSampleRate=rate;return true;
    }
    bool refreshCapabilities(AudioClockCapabilities& output,std::string&)override{output=caps;return true;}
    bool consumeSampleRateChange()noexcept override{return false;}
    AudioClockTimingCounters consumeTimingCounters()noexcept override{return {};}
    void setStageMeasurementsEnabled(bool)noexcept override{}
    StageTimingCounters consumeOutputCopyStageCounters()noexcept override{return {};}
    bool start(std::uint32_t frames,const std::vector<std::uint32_t>& inputs,const std::vector<std::uint32_t>& outputs,
        AudioClockProcessCallback callback,void* context,std::string&)override{
        stop();{std::lock_guard<std::mutex> lock(state->mutex);state->inputs=inputs;state->outputs=outputs;}
        ++state->starts;stopped=false;
        thread=std::thread([this,frames,inputs,outputs,callback,context]{
            const float* in[2]={nullptr,nullptr};float* out[2]={nullptr,nullptr};
            for(const auto channel:inputs)if(channel<2)in[channel]=channel?right.data():left.data();
            for(const auto channel:outputs)if(channel<2)out[channel]=channel?outRight.data():outLeft.data();
            const auto period=std::chrono::nanoseconds(static_cast<std::int64_t>(1e9*frames/caps.currentSampleRate));
            auto next=std::chrono::steady_clock::now();
            while(!stopped){callback(in,out,frames,context);++state->callbacks;
                if(out[1])for(unsigned i=0;i<frames;++i)if(std::abs(out[1][i]+.25f)<.002f)state->heardNative=true;
                next+=period;std::this_thread::sleep_until(next);}
        });return true;
    }
    void stop()noexcept override{stopped=true;if(thread.joinable()){thread.join();++state->stops;}}
    void close()noexcept override{stop();}
};
struct TransportState {
    std::atomic<unsigned> opens{0},closes{0},owners{0},serial{0},invalidateThrough{0};
    std::atomic<NativeTransferCode> openCode{NativeTransferCode::Ok};
    std::atomic<bool> overlap{false},wrongThread{false},heardCapture{false};
};
class Transport final:public INativeAudioTransport {
    std::shared_ptr<TransportState> state;unsigned serial;DWORD thread=0;bool owned=false;
    std::atomic<bool> cancelled{false};
    std::chrono::steady_clock::time_point next;
    std::uint64_t sequence=0;
    static void Put(unsigned char* p,unsigned offset,std::uint64_t value,unsigned width)noexcept{
        for(unsigned i=0;i<width;++i)p[offset+i]=static_cast<unsigned char>(value>>(i*8));
    }
    void Check()noexcept{if(thread!=GetCurrentThreadId())state->wrongThread=true;}
public:
    explicit Transport(std::shared_ptr<TransportState> s):state(std::move(s)),serial(++state->serial){}
    NativeTransferResult Open(NativeSessionInfo& session)noexcept override{
        thread=GetCurrentThreadId();++state->opens;const auto code=state->openCode.load();
        if(code!=NativeTransferCode::Ok)return {code,0,code==NativeTransferCode::AccessDenied?5u:2u};
        if(++state->owners!=1){state->overlap=true;--state->owners;return {NativeTransferCode::OwnedElsewhere};}
        owned=true;session.wireGeneration=7;next=std::chrono::steady_clock::now()+std::chrono::milliseconds(5);return {NativeTransferCode::Ok};
    }
    NativeTransferResult ReadRender(std::uint32_t maximum,void* output,std::uint32_t capacity)noexcept override{
        Check();if(cancelled)return {NativeTransferCode::Cancelled};
        if(serial<=state->invalidateThrough)return {NativeTransferCode::Removed,0,1617};
        const auto now=std::chrono::steady_clock::now();if(now<next)return {NativeTransferCode::Empty};
        if(maximum<240||capacity<1984||!output)return {NativeTransferCode::InvalidPacket};
        if(now-next>std::chrono::milliseconds(20))next=now;next+=std::chrono::milliseconds(5);
        auto* p=static_cast<unsigned char*>(output);std::memset(p,0,1984);
        Put(p,0,0x41575654,4);Put(p,4,64,4);Put(p,8,1,2);Put(p,12,1,2);Put(p,14,1,2);
        Put(p,16,7,8);Put(p,24,sequence++,8);Put(p,40,240,4);Put(p,44,48000,4);
        Put(p,48,2,2);Put(p,50,1,2);Put(p,52,1920,4);
        for(unsigned frame=0;frame<240;++frame){Put(p,64+frame*8,0x20000000,4);Put(p,68+frame*8,0xe0000000,4);}
        return {NativeTransferCode::Ok,1984};
    }
    NativeTransferResult WriteCapture(const void* packet,std::uint32_t bytes)noexcept override{
        Check();if(cancelled)return {NativeTransferCode::Cancelled};
        if(serial<=state->invalidateThrough)return {NativeTransferCode::Removed,0,1617};
        const auto valid=timox_native::ValidatePacket(packet,bytes,{7,2,512});
        if(valid.error!=timox_native::PacketError::None)return {NativeTransferCode::InvalidPacket};
        const auto* data=static_cast<const unsigned char*>(packet);
        for(unsigned f=0;f<valid.metadata.frames;++f){
            const auto sample=timox_native::detail::ReadLittleEndian(data,64+f*8,4);
            if(sample>0x10000000&&sample<0x30000000)state->heardCapture=true;
        }
        return {NativeTransferCode::Ok};
    }
    NativeTransferResult ReadStatus(std::uint64_t (&c)[3])noexcept override{Check();c[0]=c[1]=c[2]=0;return {NativeTransferCode::Ok};}
    void RequestCancel()noexcept override{cancelled=true;}
    void Close()noexcept override{Check();if(owned){--state->owners;owned=false;}++state->closes;}
};
struct StoreLock {
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit StoreLock(const std::wstring& path){handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);Require(handle!=INVALID_HANDLE_VALUE,"lock configuration fixture");}
    ~StoreLock(){CloseHandle(handle);}
};
AudioControllerConfiguration Configuration(){
    AudioControllerConfiguration config;config.physicalDriverId="fake";config.sampleRate=48000;config.bufferFrames=256;
    config.nativeAudio.enabled=true;
    config.routes={{"capture","physical:fake:input:1","native:windows:output:1",0,false},
        {"render","native:windows:input:2","physical:fake:output:2",0,false}};
    return config;
}
AudioControllerResult Apply(AudioController& controller,const AudioControllerConfiguration& config){
    return controller.ApplyConfiguration(config,std::nullopt,false,false,false);
}
void AvailabilityAndRetirement(const std::wstring& path){
    auto master=std::make_shared<ClockState>();auto native=std::make_shared<TransportState>();native->openCode=NativeTransferCode::Unavailable;
    VasioClientManager clients;
    AudioController controller(clients,path,path+L".profiles",[master]{return std::make_unique<Master>(master);},{},{},
        [native]{return std::make_unique<Transport>(native);});
    Require(controller.Start(),"controller starts");auto config=Configuration();
    Require(Apply(controller,config).success,"unavailable native remains a valid desired configuration");
    Await([&]{return native->opens>0;},"native factory was not invoked");
    auto absent=controller.Snapshot();Require(absent.state=="running"&&absent.routes.empty()&&absent.configuredRoutes.size()==2&&absent.configuredNativeAudio.enabled,"absence stopped master or deleted desired routes");
    Require(!absent.nativeAudio.attached&&absent.nativeAudio.lastResult.code==NativeTransferCode::Unavailable,"absence not exposed distinctly");
    {std::lock_guard<std::mutex> lock(master->mutex);Require(master->inputs==std::vector<std::uint32_t>{0}&&master->outputs==std::vector<std::uint32_t>{1},"dormant native physical channels not reserved");}
    const auto starts=master->starts.load(),callbacks=master->callbacks.load();native->openCode=NativeTransferCode::Ok;
    Await([&]{return controller.Snapshot().nativeAudio.attached&&controller.Snapshot().routes.size()==2;},"native unavailable did not recover to active routes");
    Require(master->starts==starts&&master->callbacks>callbacks,"native recovery restarted or stalled master");
    Await([&]{return native->heardCapture.load();},"real graph/SRC did not transfer physical capture PCM");
    Await([&]{return master->heardNative.load();},"real graph/SRC did not transfer native PCM to physical output2");
    const auto active=controller.Snapshot();const auto opens=native->opens.load();config.routes[0].gainDb=-6;
    Require(Apply(controller,config).success&&native->opens==opens&&controller.Snapshot().nativeAudio.epoch==active.nativeAudio.epoch,"route-only edit reopened native session");
    native->invalidateThrough=native->serial.load();
    Await([&]{const auto s=controller.Snapshot();return s.nativeAudio.attached&&s.nativeAudio.epoch>active.nativeAudio.epoch&&s.routes.size()==2;},"removed native did not republish a fresh epoch");
    Require(master->starts==starts&&!native->overlap,"removed retry overlapped ownership or restarted master");
    config.nativeAudio.enabled=false;Require(Apply(controller,config).success,"disable native");
    auto disabled=controller.Snapshot();Require(!disabled.nativeAudio.attached&&disabled.routes.empty()&&disabled.configuredRoutes.size()==2&&native->owners==0,"disable failed to retire without deleting routes");
    const auto disabledOpens=native->opens.load();std::this_thread::sleep_for(std::chrono::milliseconds(1200));Require(native->opens==disabledOpens,"disabled manager retried");
    config.nativeAudio.enabled=true;Require(Apply(controller,config).success,"reenable native");Await([&]{return controller.Snapshot().nativeAudio.attached;},"reenable never attached");
    Require(controller.StopAudio().success&&native->owners==0,"StopAudio did not join native owner");
    const auto stoppedOpens=native->opens.load();std::this_thread::sleep_for(std::chrono::milliseconds(1200));Require(native->opens==stoppedOpens,"explicit StopAudio retried");
    Require(controller.StartAudio().success,"StartAudio failed");Await([&]{return controller.Snapshot().nativeAudio.attached;},"StartAudio did not restore native desire");
    controller.Stop();Require(native->owners==0&&!native->wrongThread&&!native->overlap,"native lifetime escaped worker or shutdown");
}
void Transactions(const std::wstring& path){
    auto master=std::make_shared<ClockState>();auto native=std::make_shared<TransportState>();VasioClientManager clients;
    AudioController controller(clients,path,path+L".profiles",[master]{return std::make_unique<Master>(master);},{},{},
        [native]{return std::make_unique<Transport>(native);});
    Require(controller.Start(),"transaction controller start");auto config=Configuration();Require(Apply(controller,config).success,"initial native activation");
    Await([&]{return controller.Snapshot().nativeAudio.attached;},"initial generation not attached");const auto first=controller.Snapshot();
    const auto opens=native->opens.load();
    auto conflict=config;conflict.nativeAudio.enabled=false;
    Require(controller.ApplyConfiguration(conflict,first.configurationRevision-1,false,false,false).conflict&&native->opens==opens&&controller.Snapshot().nativeAudio.epoch==first.nativeAudio.epoch,"revision conflict mutated native state");
    {
        StoreLock lock(path);auto rejected=config;rejected.nativeAudio.enabled=false;
        Require(!Apply(controller,rejected).success,"locked store accepted disable");
        auto s=controller.Snapshot();Require(s.nativeAudio.attached&&s.nativeAudio.epoch==first.nativeAudio.epoch&&native->opens==opens&&s.configurationRevision==first.configurationRevision,"failed live save changed native owner");
        rejected=config;rejected.bufferFrames=128;
        Require(!Apply(controller,rejected).success,"locked store accepted format change");
        s=controller.Snapshot();Require(s.state=="running"&&s.bufferFrames==256&&s.nativeAudio.attached&&s.nativeAudio.epoch==first.nativeAudio.epoch&&native->opens==opens&&s.configurationRevision==first.configurationRevision,"format save rollback lost old native generation");
    }
    master->failRate=44100;auto rejected=config;rejected.sampleRate=44100;
    Require(!Apply(controller,rejected).success,"injected master failure accepted");
    Require(controller.Snapshot().sampleRate==48000&&controller.Snapshot().nativeAudio.epoch==first.nativeAudio.epoch&&native->opens==opens,"master preparation rollback reopened native transport");master->failRate=0;
    config.bufferFrames=128;Require(Apply(controller,config).success,"successful format replacement");
    Await([&]{const auto s=controller.Snapshot();return s.nativeAudio.attached&&s.nativeAudio.epoch>first.nativeAudio.epoch;},"new format did not replace native generation");
    Require(!native->overlap&&controller.Snapshot().nativeAudio.graphQuantum==128,"format replacement opened before retiring old owner");
    native->openCode=NativeTransferCode::Unavailable;config.bufferFrames=256;
    Require(Apply(controller,config).success,"native absence incorrectly rolled back valid master change");
    auto committed=controller.Snapshot();Require(committed.state=="running"&&committed.bufferFrames==256&&committed.configuredNativeAudio.enabled&&committed.configuredRoutes.size()==2&&!committed.nativeAudio.attached,"native replacement failure lost committed intent/master");
    controller.Stop();Require(!native->overlap&&!native->wrongThread&&native->owners==0,"transaction cleanup owner violation");
}
void DefaultUnavailable(const std::wstring& path){
    auto master=std::make_shared<ClockState>();VasioClientManager clients;
    AudioController controller(clients,path,path+L".profiles",[master]{return std::make_unique<Master>(master);});
    Require(controller.Start(),"default unavailable controller starts");Require(Apply(controller,Configuration()).success,"missing transport factory is nonfatal");
    const auto s=controller.Snapshot();Require(s.state=="running"&&!s.nativeAudio.attached&&s.nativeAudio.lastResult.code==NativeTransferCode::Unavailable,"default factory must expose unavailable without direct fallback");controller.Stop();
}
void RestoredDenial(const std::wstring& path){
    auto master=std::make_shared<ClockState>();auto native=std::make_shared<TransportState>();native->openCode=NativeTransferCode::AccessDenied;
    auto config=Configuration();std::string error;Require(AudioConfigurationStore(path).Save(config,error),"save native restore fixture");
    VasioClientManager clients;
    AudioController controller(clients,path,path+L".profiles",[master]{return std::make_unique<Master>(master);},{},{},
        [native]{return std::make_unique<Transport>(native);});
    Require(controller.Start(),"restore denied native configuration");
    const auto denied=controller.Snapshot();Require(denied.state=="running"&&denied.configuredNativeAudio.enabled&&denied.nativeAudio.lastResult.code==NativeTransferCode::AccessDenied&&denied.nativeAudio.lastResult.systemError==5,"restore must preserve typed denial while master runs");
    const auto opens=native->opens.load();std::this_thread::sleep_for(std::chrono::milliseconds(1200));Require(native->opens==opens,"AccessDenied automatically retried");
    auto legacy=config;legacy.nativeAudio.enabled=false;
    Require(controller.ApplyConfiguration(legacy).success&&controller.Snapshot().configuredNativeAudio.enabled&&native->opens==opens,"legacy omission changed native desire or retried denial");
    config.nativeAudio.enabled=false;Require(Apply(controller,config).success,"disable denied native desire");
    native->openCode=NativeTransferCode::Ok;config.nativeAudio.enabled=true;Require(Apply(controller,config).success,"explicit reenable after denied attempt");
    Await([&]{return controller.Snapshot().nativeAudio.attached;},"explicit reenable did not acquire fresh session");
    controller.Stop();Require(native->owners==0&&!native->overlap,"restore fixture owner leaked");
}
std::wstring TempPath(const wchar_t* suffix){wchar_t directory[MAX_PATH]{};Require(GetTempPathW(MAX_PATH,directory)!=0,"temporary directory");return std::wstring(directory)+L"TimoxVasio-native-controller-"+std::to_wstring(GetCurrentProcessId())+suffix;}
void Clean(const std::wstring& path){DeleteFileW(path.c_str());DeleteFileW((path+L".tmp").c_str());DeleteFileW((path+L".profiles").c_str());}
}
int main(){
    const auto availability=TempPath(L"-availability.json"),transactions=TempPath(L"-transactions.json"),missing=TempPath(L"-missing.json"),restored=TempPath(L"-restored.json");
    Clean(availability);Clean(transactions);Clean(missing);Clean(restored);
    try{PublicStateProjection();AvailabilityAndRetirement(availability);Transactions(transactions);DefaultUnavailable(missing);RestoredDenial(restored);
        Clean(availability);Clean(transactions);Clean(missing);Clean(restored);std::cout<<"Native controller tests passed (fake transport/master)\n";return 0;
    }catch(const std::exception& e){Clean(availability);Clean(transactions);Clean(missing);Clean(restored);std::cerr<<e.what()<<'\n';return 1;}
}
