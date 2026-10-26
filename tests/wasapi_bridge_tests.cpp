#include "wasapi_bridge.h"
#include "drift_resampler.h"
#include <audioclient.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
thread_local bool trackStreamingAllocations=false;
thread_local unsigned streamingAllocations=0;
}
void* operator new(std::size_t size) {
    if(trackStreamingAllocations) ++streamingAllocations;
    if(void* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

namespace {
constexpr double pi=3.14159265358979323846;
void Require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class Predicate> void Await(Predicate ready,const char* message) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!ready() && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(ready(),message);
}

struct FakeState {
    WasapiStreamInfo info;
    std::vector<float> capture, rendered;
    std::uint32_t capturePacket=97, renderPacket=37;
    std::atomic<std::uint32_t> renderLimit{0};
    std::atomic<unsigned> captureLimit{UINT32_MAX};
    bool manualControlClock=false;
    std::atomic<std::int64_t> controlNanos{0};
    std::int64_t advanceClockOnCaptureRelease=0;
    std::atomic<bool> waitingAfterCapture{false};
    std::atomic<unsigned> renderDrainCompleted{0};
    std::atomic<std::int64_t> renderWakeNanos{1000000000};
    unsigned graphWriteAt=0;
    std::atomic<unsigned> graphWrites{0};
    WasapiBridgeIo* graphIo=nullptr;
    unsigned graphReadAt=0;
    std::atomic<unsigned> graphReads{0};
    float graphLeft[2048]{},graphRight[2048]{};
    bool captureSilent=false, captureFlags=false;
    HRESULT openError=S_OK, startError=S_OK, waitError=S_OK, releaseError=S_OK;
    std::atomic<unsigned> captureOffset{0}, renderedFrames{0}, starts{0}, closes{0}, stops{0}, waits{0}, allocations{0};
    std::atomic<bool> enteredOpen{false}, unblockOpen{true}, destroyed{false}, wrongThread{false};
    DWORD workerThread=0;
    bool held=false;
    unsigned heldFrames=0;
    FakeState() {
        info.kind=WasapiStreamKind::Capture; info.endpointId=L"fake";
        info.format={48000,2,32,32,8,3,WasapiSampleEncoding::IeeeFloat};
        info.bufferFrames=257; info.defaultPeriod100ns=100000; info.minimumPeriod100ns=30000;
        rendered.resize(20000*2);
    }
};

class FakeBackend final : public IWasapiBackend {
public:
    explicit FakeBackend(std::shared_ptr<FakeState> state):s(std::move(state)){}
    ~FakeBackend() override { CheckThread(); s->destroyed=true; }
    HRESULT Open(const WasapiOpenConfig&) noexcept override {
        s->workerThread=GetCurrentThreadId(); s->enteredOpen=true;
        while(!s->unblockOpen.load()) Sleep(1);
        return s->openError;
    }
    const WasapiStreamInfo* Info() const noexcept override { return &s->info; }
    HRESULT Start() noexcept override {
        CheckThread(); ++s->starts; streamingAllocations=0;
        trackStreamingAllocations=SUCCEEDED(s->startError); return s->startError;
    }
    HRESULT Stop() noexcept override {
        trackStreamingAllocations=false; s->allocations=streamingAllocations;
        CheckThread(); ++s->stops; return S_OK;
    }
    void Close() noexcept override { CheckThread(); s->held=false; ++s->closes; }
    WasapiWaitResult Wait(HANDLE stop,DWORD timeout) noexcept override {
        CheckThread(); ++s->waits;
        if(s->manualControlClock && s->info.kind==WasapiStreamKind::Render)
            s->renderDrainCompleted=s->renderedFrames.load();
        if(s->manualControlClock && s->captureOffset==s->capture.size()/2)s->waitingAfterCapture=true;
        if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0) return {WasapiWaitKind::Stop,S_OK};
        if(FAILED(s->waitError)) return {WasapiWaitKind::Failed,s->waitError};
        const bool available=s->info.kind==WasapiStreamKind::Render
            ? s->renderedFrames.load()<s->renderLimit : s->captureOffset.load()<std::min<std::size_t>(s->capture.size()/2,s->captureLimit.load());
        if(available) {
            if(s->manualControlClock)s->controlNanos=s->info.kind==WasapiStreamKind::Render?s->renderWakeNanos.load():1000000000;
            return {WasapiWaitKind::Audio,S_OK};
        }
        if(WaitForSingleObject(stop,timeout)==WAIT_OBJECT_0)return {WasapiWaitKind::Stop,S_OK};
        if(s->manualControlClock) {
            if(s->info.kind==WasapiStreamKind::Render) {
                if(s->renderedFrames<s->renderLimit)s->controlNanos=s->renderWakeNanos.load();
            } else if(s->captureOffset<s->captureLimit)s->controlNanos=1000000000;
        }
        return {WasapiWaitKind::Timeout,S_OK};
    }
    HRESULT NextCapturePacketFrames(std::uint32_t& frames) noexcept override {
        CheckThread(); frames=std::min<unsigned>(s->capturePacket,
            std::min<unsigned>(static_cast<unsigned>(s->capture.size()/2),s->captureLimit.load())-s->captureOffset.load()); return S_OK;
    }
    HRESULT AcquireCapture(WasapiCapturePacket& packet) noexcept override {
        CheckThread(); packet={};
        if(s->held) return AUDCLNT_E_OUT_OF_ORDER;
        NextCapturePacketFrames(packet.frames);
        if(!packet.frames) return AUDCLNT_S_BUFFER_EMPTY;
        packet.data=reinterpret_cast<const std::uint8_t*>(s->capture.data()+s->captureOffset.load()*2);
        packet.silent=s->captureSilent; if(packet.silent) packet.data=nullptr;
        packet.discontinuity=packet.timestampError=s->captureFlags;
        s->held=true; s->heldFrames=packet.frames; return S_OK;
    }
    HRESULT ReleaseCapture(std::uint32_t frames) noexcept override {
        CheckThread(); if(!s->held) return AUDCLNT_E_OUT_OF_ORDER;
        if(FAILED(s->releaseError)) return s->releaseError;
        s->captureOffset+=frames; s->held=false;
        s->controlNanos.fetch_add(s->advanceClockOnCaptureRelease);
        if(s->graphIo && s->captureOffset>=s->graphReadAt && !s->graphReads.load()) {
            float* planes[]={s->graphLeft,s->graphRight};
            s->graphReads=static_cast<unsigned>(s->graphIo->ReadCaptureBlock(planes,2048,2));
        }
        return S_OK;
    }
    HRESULT RenderWritableFrames(std::uint32_t& frames) noexcept override {
        CheckThread(); frames=std::min(s->renderPacket,s->renderLimit-s->renderedFrames.load()); return S_OK;
    }
    HRESULT AcquireRender(std::uint32_t frames,std::uint8_t*& data) noexcept override {
        CheckThread(); data=nullptr;
        if(s->held) return AUDCLNT_E_OUT_OF_ORDER;
        s->held=true; s->heldFrames=frames;
        data=reinterpret_cast<std::uint8_t*>(s->rendered.data()+s->renderedFrames.load()*2); return S_OK;
    }
    HRESULT ReleaseRender(std::uint32_t frames,bool silent) noexcept override {
        CheckThread(); if(!s->held) return AUDCLNT_E_OUT_OF_ORDER;
        if(FAILED(s->releaseError)) return s->releaseError;
        if(silent) std::fill_n(s->rendered.data()+s->renderedFrames.load()*2,frames*2,0.0f);
        s->renderedFrames+=frames; s->held=false;
        if(s->graphIo && s->graphWriteAt && s->renderedFrames>=s->graphWriteAt && !s->graphWrites.load()) {
            const float* planes[]={s->graphLeft,s->graphRight};
            s->graphWrites=static_cast<unsigned>(s->graphIo->WriteRenderBlock(planes,2048,2));
        }
        return S_OK;
    }
private:
    void CheckThread() noexcept { if(s->workerThread && GetCurrentThreadId()!=s->workerThread) s->wrongThread=true; }
    std::shared_ptr<FakeState> s;
};

WasapiBridgeConfig Config(WasapiStreamKind kind) {
    WasapiBridgeConfig config; config.endpoint.kind=kind; config.endpoint.endpointId=L"fake";
    config.maxFramesPerPass=31; config.maxPacketsPerPass=2; config.waitTimeoutMs=10;
    config.targetFillFrames=32; config.kp=0; config.ki=0; return config;
}
std::vector<float> Tone(unsigned frames,double rate) {
    std::vector<float> result(frames*2);
    for(unsigned n=0;n<frames;++n) {
        result[2*n]=static_cast<float>(0.6*std::sin(2*pi*1000*n/rate));
        result[2*n+1]=static_cast<float>(0.2*std::cos(2*pi*1000*n/rate));
    }
    return result;
}
WasapiBridge::BackendFactory Factory(const std::shared_ptr<FakeState>& state) {
    return [state]{return std::make_unique<FakeBackend>(state);};
}
void CheckCleanup(const std::shared_ptr<FakeState>& s) {
    Require(s->destroyed && s->closes==1 && !s->wrongThread && !s->held,"backend lifetime escaped owning worker");
    Require(s->allocations==0,"worker allocated after native Start");
}

void CaptureObservationIgnoresPassBudget() {
    for(unsigned nativeRate : {44100U,48000U})for(unsigned budget : {31U,4096U,8192U})
    for(unsigned packetBudget : {1U,8U}) {
        auto s=std::make_shared<FakeState>();s->capture=Tone(8192,nativeRate);s->captureLimit=0;
        s->info.format.sampleRate=nativeRate;s->info.bufferFrames=8192;s->capturePacket=4096;
        s->manualControlClock=true;s->graphReadAt=8192;
        WasapiBridgeIo io({WasapiBridgeDirection::Capture,2,48000,2048,102,32768});s->graphIo=&io;
        std::vector<float> prefill(8192*2,0);io.WriteCaptureInterleaved(prefill.data(),8192,prefill.size());
        auto config=Config(WasapiStreamKind::Capture);config.targetFillFrames=8192;
        config.maxFramesPerPass=budget;config.kp=3e-6;config.ki=1e-6;
        config.maxPacketsPerPass=packetBudget;
        config.controlTimeContext=s.get();config.controlTimeNanoseconds=[](void* context)noexcept {
            return static_cast<FakeState*>(context)->controlNanos.load();
        };
        WasapiBridge bridge(config,io,Factory(s));Require(bridge.Start()==S_OK,"observation startup");
        Await([&]{return s->waits>0;},"initial empty wake did not finish");
        s->captureLimit=8192;
        Await([&]{return s->waitingAfterCapture.load();},"capture observation did not drain all SRC tails");
        const auto expectedFrames=8192+static_cast<unsigned>(std::ceil(8192.0*48000/nativeRate))-2048;
        Require(io.BufferedFrames()==expectedFrames && s->graphReads==2048,"observation lost pending SRC or graph callback");
        auto expected=FillLevelController::ForPacketCadence(FillLevelController::Direction::Capture,8192,3e-6,1e-6,48000,2048);
        const double ratio=48000.0/nativeRate*expected.Update((8192+expectedFrames)*0.5,1);
        Require(std::abs(bridge.Health().ratio-ratio)<1e-12,"capture PI observation depends on partial-pass budget");
        Require(bridge.Health().controlBacklogUpdates==0,"normal bounded drain used prolonged-backlog fallback");
        bridge.Stop();CheckCleanup(s);
    }
}

void RenderObservationIgnoresPassBudget() {
    for(unsigned nativeRate : {44100U,48000U,96000U})for(unsigned budget : {31U,257U,4096U,8192U}) {
        auto s=std::make_shared<FakeState>();s->info.kind=WasapiStreamKind::Render;
        s->info.format.sampleRate=nativeRate;s->info.bufferFrames=8192;s->renderPacket=8192;
        s->manualControlClock=true;s->graphWriteAt=4096;
        WasapiBridgeIo io({WasapiBridgeDirection::Render,2,48000,2048,104,32768});s->graphIo=&io;
        const auto signal=Tone(14336,48000);
        for(unsigned start=0;start<14336;start+=2048) {
            for(unsigned n=0;n<2048;++n){s->graphLeft[n]=signal[(start+n)*2];s->graphRight[n]=signal[(start+n)*2+1];}
            if(start<12288){const float* planes[]={s->graphLeft,s->graphRight};Require(io.WriteRenderBlock(planes,2048,2)==2048,"render observation prefill");}
        }
        auto config=Config(WasapiStreamKind::Render);config.targetFillFrames=12288;
        config.maxFramesPerPass=budget;config.kp=WasapiBridgeConfig{}.kp;config.ki=WasapiBridgeConfig{}.ki;
        config.controlTimeContext=s.get();config.controlTimeNanoseconds=[](void* context)noexcept {
            return static_cast<FakeState*>(context)->controlNanos.load();
        };
        WasapiBridge bridge(config,io,Factory(s));Require(bridge.Start()==S_OK,"render observation startup");
        Await([&]{return s->waits>0;},"render initial empty wake did not finish");
        DriftResampler reference(2,nativeRate/48000.0);
        auto controller=FillLevelController::ForPacketCadence(FillLevelController::Direction::Render,
            12288,config.kp,config.ki,48000,2048);
        std::vector<float> expected(8256*2);unsigned consumed=0,produced=0;
        double previousFill=12288;
        // Odd first output count retains an interpolation tail at ratio 2.
        // Second wake advances time and proves that both SRC phase and PI state survive.
        for(unsigned end : {8191U,8256U}) {
            const auto progress=reference.Process(signal.data()+consumed*2,14336-consumed,
                expected.data()+produced*2,end-produced);
            Require(progress.produced==end-produced,"reference render could not produce native span");
            consumed+=progress.consumed;produced=end;
            const double fill=14336-consumed;
            const double expectedRatio=nativeRate/48000.0*controller.Update((previousFill+fill)*0.5,1);
            s->renderWakeNanos=end==8191?1000000000:2000000000;s->renderLimit=end;
            Await([&]{return s->renderDrainCompleted>=end;},"render partial passes never reached completed observation");
            const auto health=bridge.Health();
            Require(std::abs(expectedRatio-nativeRate/48000.0)>1e-5,
                "render observation did not exercise active PI correction");
            Require(health.state==WasapiBridgeState::Running && s->graphWrites==2048 &&
                !health.renderSilencedInputFrames && !health.renderSilencedDeviceFrames && !health.controlBacklogUpdates,
                "render observation unexpectedly primed, starved, or used deadline fallback");
            Require(std::abs(health.ratio-expectedRatio)<1e-12,"render PI observation depends on partial-pass budget");
            for(unsigned n=0;n<end*2;++n)Require(std::abs(s->rendered[n]-expected[n])<1e-6f,
                "render partial passes lost SRC phase or changed audio samples");
            reference.SetRatio(expectedRatio);previousFill=fill;
        }
        bridge.Stop();CheckCleanup(s);
    }
}

void ProlongedCaptureDrainStillUpdatesControl() {
    auto s=std::make_shared<FakeState>();s->capture=Tone(8192,48000);s->captureLimit=0;
    s->info.bufferFrames=4096;s->capturePacket=4096;s->manualControlClock=true;
    s->advanceClockOnCaptureRelease=110000000; // Each native release takes 110 simulated ms.
    WasapiBridgeIo io({WasapiBridgeDirection::Capture,2,48000,2048,103,32768});
    std::vector<float> prefill(8192*2,0);io.WriteCaptureInterleaved(prefill.data(),8192,prefill.size());
    auto config=Config(WasapiStreamKind::Capture);config.targetFillFrames=8192;config.maxFramesPerPass=31;
    config.kp=3e-6;config.ki=1e-6;config.controlTimeContext=s.get();
    config.controlTimeNanoseconds=[](void* context)noexcept{return static_cast<FakeState*>(context)->controlNanos.load();};
    WasapiBridge bridge(config,io,Factory(s));Require(bridge.Start()==S_OK,"prolonged drain startup");
    Await([&]{return s->waits>0;},"prolonged drain initial wait");s->captureLimit=8192;
    Await([&]{return s->waitingAfterCapture.load();},"prolonged drain failed to finish");
    const auto health=bridge.Health();
    Require(health.controlBacklogUpdates==2 && health.capturedFrames==8192,
        "continuing backlog suppressed bounded control observations");
    Require(std::isfinite(health.ratio) && health.ratio>=0.998 && health.ratio<=1.002,
        "prolonged drain escaped ratio safety bounds");
    // Budget invariance is intentionally NOT asserted after a timed fallback:
    // the diagnostic identifies a partial observation under prolonged work.
    bridge.Stop();CheckCleanup(s);
}

void CapturePrimingWithLiveGraph() {
    for(bool delayed : {false,true}) {
    auto s=std::make_shared<FakeState>();s->capture=Tone(512,48000);s->captureLimit=0;
    WasapiBridgeIo io({WasapiBridgeDirection::Capture,2,48000,256,101,1024});
    auto config=Config(WasapiStreamKind::Capture);config.targetFillFrames=512;
    config.kp=3e-6;config.ki=1e-6;
    config.primingTimeoutMs=delayed?20:2000;
    WasapiBridge bridge(config,io,Factory(s));Require(bridge.Start()==S_OK,"priming startup");
    float left[256],right[256];float* planes[]={left,right};
    if(delayed) {
        s->captureLimit=128;
        Await([&]{return io.BufferedFrames()==128;},"partial prefill packet missing");
        Await([&]{return bridge.Health().primingTimedOut;},"capture timeout diagnostic missing");
        Require(bridge.Health().state==WasapiBridgeState::Priming,"timeout released unfilled capture FIFO");
    }
    for(unsigned packet=1;packet<=3;++packet) {
        s->captureLimit=packet*128;
        Await([&]{return io.BufferedFrames()>=packet*128;},"priming packet missing");
        std::fill_n(left,256,1.0f);std::fill_n(right,256,1.0f);
        Require(io.ReadCaptureBlock(planes,256,2)==0,"live graph drained capture before prefill target");
        Require(std::all_of(left,left+256,[](float v){return v==0;}) &&
            std::all_of(right,right+256,[](float v){return v==0;}),"priming callback was not silent");
        Require(bridge.Health().state==WasapiBridgeState::Priming,"capture released before target");
    }
    s->captureLimit=512;
    Await([&]{return bridge.Health().state==WasapiBridgeState::Running;},"live graph prevented capture priming");
    Require(bridge.Health().primingTimedOut==delayed && io.BufferedFrames()==512,"capture failed to prefill with active graph");
    Require(io.ReadCaptureBlock(planes,256,2)==256 && io.BufferedFrames()==256,"prefill was lost at gate release");
    bridge.Stop();CheckCleanup(s);
    }
}

void CaptureSignal() {
    for(unsigned graphRate : {44100U,48000U}) {
        auto s=std::make_shared<FakeState>(); s->capture=Tone(4096,48000);
        WasapiBridgeIo io({WasapiBridgeDirection::Capture,2,graphRate,256,1,8192});
        WasapiBridge bridge(Config(WasapiStreamKind::Capture),io,Factory(s));
        Require(bridge.Start()==S_OK,"capture startup failed");
        const auto expected=static_cast<unsigned>(std::ceil(4096.0*graphRate/48000));
        Await([&]{return io.BufferedFrames()>=expected;},"capture tail was lost across passes");
        bridge.Stop();
        auto health=bridge.Health();
        Require(health.state==WasapiBridgeState::Stopped && health.error==S_OK && health.capturedFrames==4096,
                "capture health does not report transferred frames");
        Require(health.droppedCaptureFrames==0,"capture dropped available capacity");
        std::vector<float> left(expected),right(expected);
        unsigned offset=0;
        while(offset<expected) {
            float* planes[]={left.data()+offset,right.data()+offset};
            unsigned count=std::min(256U,expected-offset);
            Require(io.ReadCaptureBlock(planes,count,2)==count,"capture graph read missing frames"); offset+=count;
        }
        double error=0,power=0;
        for(unsigned j=512;j<expected;++j) {
            const double time=(j*48000.0/graphRate-127.5)/48000;
            double a=0.6*std::sin(2*pi*1000*time),b=0.2*std::cos(2*pi*1000*time);
            error+=(left[j]-a)*(left[j]-a)+(right[j]-b)*(right[j]-b); power+=a*a+b*b;
        }
        Require(10*std::log10(power/error)>90,"capture conversion/SRC signal corrupted");
        CheckCleanup(s);
        Require(FAILED(bridge.Start()),"same immutable generation restarted");
    }
}

void RenderSignal() {
    auto s=std::make_shared<FakeState>(); s->info.kind=WasapiStreamKind::Render; s->renderLimit=3000;
    WasapiBridgeIo io({WasapiBridgeDirection::Render,2,44100,256,2,8192});
    auto tone=Tone(4096,44100);
    float left[256],right[256];
    for(unsigned offset=0;offset<4096;offset+=256) {
        for(unsigned n=0;n<256;++n) {left[n]=tone[2*(offset+n)];right[n]=tone[2*(offset+n)+1];}
        const float* planes[]={left,right}; Require(io.WriteRenderBlock(planes,256,2)==256,"render fixture fill");
    }
    WasapiBridge bridge(Config(WasapiStreamKind::Render),io,Factory(s));
    Require(bridge.Start()==S_OK,"render startup failed");
    Await([&]{return bridge.Health().renderedFrames==3000;},"render tails stalled"); bridge.Stop();
    double error=0,power=0;
    for(unsigned j=512;j<3000;++j) {
        const double time=(j*44100.0/48000-127.5)/44100;
        double a=0.6*std::sin(2*pi*1000*time),b=0.2*std::cos(2*pi*1000*time);
        error+=(s->rendered[2*j]-a)*(s->rendered[2*j]-a)+(s->rendered[2*j+1]-b)*(s->rendered[2*j+1]-b);
        power+=a*a+b*b;
    }
    Require(10*std::log10(power/error)>90,"render conversion/SRC signal corrupted");
    auto health=bridge.Health();
    Require(health.renderedFrames==3000 && health.renderSilencedInputFrames==0 && health.error==S_OK,
            "render health incorrect"); CheckCleanup(s);
}

void PrimingOverflowAndFailures() {
    {
        auto s=std::make_shared<FakeState>(); s->info.kind=WasapiStreamKind::Render; s->renderLimit=100;
        WasapiBridgeIo io({WasapiBridgeDirection::Render,2,48000,64,3,256});
        auto config=Config(WasapiStreamKind::Render); config.kp=0.000003; config.ki=0.000001;
        WasapiBridge bridge(config,io,Factory(s)); Require(bridge.Start()==S_OK,"priming startup failed");
        Await([&]{const auto h=bridge.Health();return h.renderedFrames==100 && h.renderSilencedDeviceFrames==100 &&
            h.state==WasapiBridgeState::Priming;},"priming does not service native silence");
        auto h=bridge.Health();
        Require(h.state==WasapiBridgeState::Priming && h.ratio==1 && h.renderSilencedInputFrames==0 &&
                h.renderSilencedDeviceFrames==100,"empty startup feeds false drift or consumes graph silence");
        bridge.Stop(); Require(std::all_of(s->rendered.begin(),s->rendered.begin()+200,[](float x){return x==0;}),"priming not silent");
        CheckCleanup(s);
    }
    {
        auto s=std::make_shared<FakeState>(); s->capture=Tone(1024,48000);
        WasapiBridgeIo io({WasapiBridgeDirection::Capture,2,48000,64,4,128});
        WasapiBridge bridge(Config(WasapiStreamKind::Capture),io,Factory(s)); Require(bridge.Start()==S_OK,"overflow startup");
        Await([&]{return bridge.Health().droppedCaptureFrames>=896;},"capture overflow not counted"); bridge.Stop();
        Require(io.BufferedFrames()==128 && bridge.Health().droppedCaptureFrames==896,"capture overflow overwrote or lost count"); CheckCleanup(s);
    }
    for(unsigned failure=0;failure<5;++failure) {
        auto s=std::make_shared<FakeState>();
        if(failure==0) s->openError=AUDCLNT_E_DEVICE_INVALIDATED;
        if(failure==1) s->info.format.channels=1;
        if(failure==2) s->startError=AUDCLNT_E_DEVICE_INVALIDATED;
        if(failure==3) s->waitError=AUDCLNT_E_DEVICE_INVALIDATED;
        if(failure==4) {s->capture=Tone(100,48000);s->releaseError=AUDCLNT_E_DEVICE_INVALIDATED;}
        WasapiBridgeIo io({WasapiBridgeDirection::Capture,2,48000,64,10+failure,256});
        WasapiBridge bridge(Config(WasapiStreamKind::Capture),io,Factory(s));
        const auto hr=bridge.Start();
        if(failure<3) Require(FAILED(hr),"startup failure hidden");
        Await([&]{return bridge.Health().state==WasapiBridgeState::Failed;},"runtime failure hidden");
        bridge.Stop(); Require(FAILED(bridge.Health().error),"failed health HRESULT missing"); CheckCleanup(s);
    }
}

void CancelOpeningAndWait() {
    auto s=std::make_shared<FakeState>(); s->unblockOpen=false;
    WasapiBridgeIo io({WasapiBridgeDirection::Capture,2,48000,64,20,256});
    WasapiBridge bridge(Config(WasapiStreamKind::Capture),io,Factory(s));
    HRESULT result=S_OK;
    std::thread starter([&]{result=bridge.Start();});
    Await([&]{return s->enteredOpen.load();},"worker never entered Open");
    bridge.RequestStop(); s->unblockOpen=true; starter.join(); bridge.Stop();
    Require(result==HRESULT_FROM_WIN32(ERROR_CANCELLED) && s->starts==0,"canceled Open still starts native stream");
    CheckCleanup(s);
    auto idle=std::make_shared<FakeState>();
    WasapiBridgeIo io2({WasapiBridgeDirection::Capture,2,48000,64,21,256});
    auto config=Config(WasapiStreamKind::Capture); config.waitTimeoutMs=1000;
    WasapiBridge waiting(config,io2,Factory(idle)); Require(waiting.Start()==S_OK,"idle startup failed");
    Await([&]{return idle->waits.load()>0;},"worker never waits");
    const auto begin=std::chrono::steady_clock::now(); waiting.Stop();
    Require(std::chrono::steady_clock::now()-begin<std::chrono::milliseconds(500),"Stop waits for audio timeout");
    CheckCleanup(idle);
}

void SilenceAndReprime() {
    {
        auto s=std::make_shared<FakeState>(); s->capture=Tone(256,48000); s->captureSilent=true;s->captureFlags=true;
        WasapiBridgeIo io({WasapiBridgeDirection::Capture,2,48000,64,30,512});
        WasapiBridge bridge(Config(WasapiStreamKind::Capture),io,Factory(s));
        Require(bridge.Start()==S_OK,"silent capture startup");
        Await([&]{return io.BufferedFrames()==256;},"silent native packet not converted");bridge.Stop();
        float left[64],right[64];float* planes[]={left,right};
        for(unsigned n=0;n<4;++n) {
            Require(io.ReadCaptureBlock(planes,64,2)==64,"silent capture count");
            for(unsigned j=0;j<64;++j) Require(left[j]==0 && right[j]==0,"silent capture reads native null data");
        }
        Require(bridge.Health().discontinuities==3 && bridge.Health().timestampErrors==3,"native packet flags lost");
        CheckCleanup(s);
    }
    {
        auto s=std::make_shared<FakeState>();s->info.kind=WasapiStreamKind::Render;s->renderLimit=100;
        WasapiBridgeIo io({WasapiBridgeDirection::Render,2,48000,128,31,512});
        auto first=Tone(32,48000),second=Tone(128,48000);
        float left[128],right[128];
        for(unsigned n=0;n<32;++n){left[n]=first[2*n];right[n]=first[2*n+1];}
        const float* planes[]={left,right};Require(io.WriteRenderBlock(planes,32,2)==32,"initial prime");
        WasapiBridge bridge(Config(WasapiStreamKind::Render),io,Factory(s));Require(bridge.Start()==S_OK,"reprime startup");
        Await([&]{const auto h=bridge.Health();return h.renderedFrames==100 && h.renderSilencedDeviceFrames==38 &&
            h.state==WasapiBridgeState::Priming;},"underflow render stalled");
        auto health=bridge.Health();
        Require(health.state==WasapiBridgeState::Priming && health.renderSilencedInputFrames>0 &&
                health.renderSilencedDeviceFrames>0,"underflow did not enter counted reprime");
        const auto zeroInputs=health.renderSilencedInputFrames;
        const auto gap=health.renderSilencedDeviceFrames;
        for(unsigned n=0;n<128;++n){left[n]=second[2*n];right[n]=second[2*n+1];}
        Require(io.WriteRenderBlock(planes,128,2)==128,"reprime refill");
        s->renderLimit=200;
        Await([&]{return bridge.Health().renderedFrames==200;},"refill did not resume rendering");bridge.Stop();
        Require(bridge.Health().renderSilencedInputFrames==zeroInputs,"unexpected second underflow");
        std::vector<float> continuous=first;
        continuous.resize(continuous.size()+static_cast<std::size_t>(zeroInputs)*2,0);
        continuous.insert(continuous.end(),second.begin(),second.end());
        DriftResampler reference(2,1);
        std::vector<float> expected(continuous.size());
        const auto p=reference.Process(continuous.data(),static_cast<std::uint32_t>(continuous.size()/2),
                                       expected.data(),static_cast<std::uint32_t>(expected.size()/2));
        Require(p.produced>=200-gap,"reference fixture too short");
        for(unsigned frame=100;frame<200;++frame)
            for(unsigned channel=0;channel<2;++channel)
                Require(s->rendered[frame*2+channel]==expected[(frame-gap)*2+channel],"reprime reset history or lost SRC tail");
        CheckCleanup(s);
    }
}
}

int main() {
    try {RenderObservationIgnoresPassBudget(); CaptureObservationIgnoresPassBudget(); ProlongedCaptureDrainStillUpdatesControl(); CapturePrimingWithLiveGraph(); CaptureSignal(); RenderSignal(); PrimingOverflowAndFailures(); CancelOpeningAndWait(); SilenceAndReprime();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"WASAPI bridge tests passed\n";
}
