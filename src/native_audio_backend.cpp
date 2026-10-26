#include "native_audio_backend.h"
#include "native_audio_packet_validation.h"
#include "drift_resampler.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <xmmintrin.h>

namespace {
constexpr std::uint64_t TickNs=5000000;
void Put(unsigned char* p,unsigned offset,std::uint64_t value,unsigned width) noexcept {
    for(unsigned i=0;i<width;++i)p[offset+i]=static_cast<unsigned char>(value>>(i*8));
}
class Scheduler final : public INativeAudioScheduler {
    std::mutex mutex;std::condition_variable event;bool stop=false;
public:
    std::uint64_t NowNanoseconds() noexcept override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    bool WaitUntil(std::uint64_t deadline) noexcept override {
        std::unique_lock<std::mutex> lock(mutex);
        event.wait_until(lock,std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline)),[&]{return stop;});
        return !stop;
    }
    void RequestStop() noexcept override {std::lock_guard<std::mutex> lock(mutex);stop=true;event.notify_all();}
};
struct Flow {
    DriftResampler src;
    FillLevelController controller;
    const double nominal,target;
    std::array<float,1024> input{};
    std::array<float,16384> output{};
    std::uint32_t inputOffset=0,inputCount=0,outputOffset=0,outputCount=0;
    bool primed=false,observing=false;
    double before=0;
    std::uint64_t since=0,lastUpdate=0;
    Flow(bool source,std::uint32_t rate,std::uint32_t quantum,std::size_t capacity)
        :src(2,source?static_cast<double>(rate)/48000:48000.0/rate),
        controller(FillLevelController::ForPacketCadence(source?FillLevelController::Direction::Capture:
            FillLevelController::Direction::Render,capacity*.5,.000003,.000001,rate,quantum)),
        nominal(source?static_cast<double>(rate)/48000:48000.0/rate),target(capacity*.5){}
    double Fill(const WasapiBridgeIo& io,bool source) const noexcept {
        return static_cast<double>(io.BufferedFrames())+(source?outputCount:inputCount);
    }
};
}
struct NativeAudioBackend::Impl {
    std::unique_ptr<INativeAudioTransport> transport;
    std::unique_ptr<INativeAudioScheduler> clock;
    std::shared_ptr<NativeAudioGeneration> generation;
    std::unique_ptr<Flow> sourceFlow,destinationFlow;
    std::thread worker;
    std::mutex startupMutex;std::condition_variable startupEvent;bool ready=false,used=false;
    std::atomic<bool> stopping{false};
    std::atomic<NativeAudioState> state{NativeAudioState::Idle};
    std::atomic<NativeTransferCode> error{NativeTransferCode::Ok};
    std::atomic<std::uint32_t> systemError{0};
    std::atomic<std::uint64_t> renderFrames{0},captureFrames{0},silenced{0},graphSilenced{0},skipped{0},discarded{0},backlog{0},discontinuities{0};
    std::array<std::atomic<std::uint64_t>,3> kernel{};
    std::atomic<double> sourceRatio{1},destinationRatio{1};
    NativeTransferCode startupResult=NativeTransferCode::Failed;
    std::array<unsigned char,4160> readPacket{},writePacket{};
    std::uint32_t pendingBytes=0,pendingFrames=0;
    std::uint64_t writeSequence=0,lastReadSequence=0;
    bool wroteMax=false,haveReadSequence=false;
    Impl(std::unique_ptr<INativeAudioTransport> t,std::unique_ptr<INativeAudioScheduler> c)
        :transport(std::move(t)),clock(std::move(c)) {
        if(!transport || !clock)throw std::invalid_argument("Native backend dependencies required");
    }
    void SignalReady(NativeTransferCode result) noexcept {
        std::lock_guard<std::mutex> lock(startupMutex);startupResult=result;ready=true;startupEvent.notify_all();
    }
    void ObserveBegin(Flow& flow,WasapiBridgeIo& io,bool source,std::uint64_t now) noexcept {
        if(!flow.primed){flow.lastUpdate=now;return;}
        if(!flow.observing){flow.before=flow.Fill(io,source);flow.since=now;flow.observing=true;}
    }
    void ObserveEnd(Flow& flow,WasapiBridgeIo& io,bool source,bool more,std::uint64_t now) noexcept {
        const double fill=flow.Fill(io,source);
        if(!flow.primed){
            if(fill>=flow.target){flow.primed=true;if(source)io.CompleteCapturePriming();}
            flow.lastUpdate=now;return;
        }
        if(!flow.observing)return;
        const bool forced=more && now>=flow.since && now-flow.since>=100000000;
        if(more && !forced)return;
        const double elapsed=now>flow.lastUpdate?(std::min)(1.0,static_cast<double>(now-flow.lastUpdate)/1e9):0;
        if(elapsed>0)flow.src.SetRatio(flow.nominal*flow.controller.Update((flow.before+fill)*.5,elapsed));
        (source?sourceRatio:destinationRatio).store(flow.src.Ratio(),std::memory_order_relaxed);
        flow.lastUpdate=now;flow.observing=false;if(forced)++backlog;
    }
    NativeTransferResult ReadSource(bool& more) noexcept {
        auto& f=*sourceFlow;auto& io=*generation->windowsRenderSource;more=true;
        // Output at maximum supported ratio fits 8192 frames, but publication
        // may span ticks if graph consumption stalls. Never overwrite a tail.
        if(f.outputCount){
            const auto count=(std::min)(static_cast<std::size_t>(f.outputCount),io.Format().capacityFrames);
            const auto written=io.WriteCaptureInterleaved(f.output.data()+f.outputOffset*2,count,count*2);
            f.outputOffset+=static_cast<std::uint32_t>(written);f.outputCount-=static_cast<std::uint32_t>(written);
            if(f.outputCount)return {NativeTransferCode::Ok};
        }
        const auto read=transport->ReadRender(512,readPacket.data(),static_cast<std::uint32_t>(readPacket.size()));
        if(read.code==NativeTransferCode::Empty){more=false;return {NativeTransferCode::Ok};}
        if(read.code!=NativeTransferCode::Ok)return read;
        if(read.bytes>readPacket.size())return {NativeTransferCode::InvalidPacket};
        const auto packet=timox_native::ValidatePacket(readPacket.data(),read.bytes,{generation->wireGeneration,1,512});
        if(packet.error!=timox_native::PacketError::None || (haveReadSequence && packet.metadata.sequence<=lastReadSequence))
            return {NativeTransferCode::InvalidPacket};
        haveReadSequence=true;lastReadSequence=packet.metadata.sequence;
        if(packet.metadata.flags&TIMOX_NATIVE_DISCONTINUITY)++discontinuities;
        for(unsigned sample=0;sample<packet.metadata.frames*2;++sample){
            const auto raw=timox_native::detail::ReadLittleEndian(readPacket.data(),64+sample*4,4);
            const auto signedValue=raw<0x80000000ULL?static_cast<std::int64_t>(raw):static_cast<std::int64_t>(raw)-0x100000000LL;
            f.input[sample]=static_cast<float>(static_cast<double>(signedValue)/2147483648.0);
        }
        const auto progress=f.src.Process(f.input.data(),packet.metadata.frames,f.output.data(),8192);
        // 512 inputs * maximum ratio 8.016 plus residual < 4114; capacity8192.
        if(progress.consumed!=packet.metadata.frames)return {NativeTransferCode::Failed};
        renderFrames+=packet.metadata.frames;f.outputOffset=0;f.outputCount=progress.produced;
        if(f.outputCount){
            const auto count=(std::min)(static_cast<std::size_t>(f.outputCount),io.Format().capacityFrames);
            const auto written=io.WriteCaptureInterleaved(f.output.data(),count,count*2);
            f.outputOffset=static_cast<std::uint32_t>(written);f.outputCount-=static_cast<std::uint32_t>(written);
        }
        return {NativeTransferCode::Ok};
    }
    NativeTransferResult MakeCapture(std::uint32_t frames) noexcept {
        if(wroteMax)return {NativeTransferCode::InvalidPacket};
        auto& f=*destinationFlow;auto& io=*generation->windowsCaptureDestination;
        bool shortage=false;std::uint32_t produced=0;
        if(!f.primed && f.Fill(io,false)>=f.target)f.primed=true;
        if(f.primed){
            // At graph/native <=8, 512 native outputs need <=4105 graph
            // inputs. This bound also includes the SRC pending-output pass.
            for(unsigned step=0;step<32 && produced<frames && !stopping;++step){
                const auto p=f.src.Process(f.input.data()+f.inputOffset*2,f.inputCount,
                    f.output.data()+produced*2,frames-produced);
                f.inputOffset+=p.consumed;f.inputCount-=p.consumed;produced+=p.produced;
                if(p.consumed || p.produced)continue;
                const auto needed=static_cast<std::uint32_t>((std::min)(512.0,(std::max)(1.0,std::ceil((frames-produced)/f.src.Ratio()))));
                const auto read=io.ReadRenderInterleaved(f.input.data(),needed,f.input.size());
                if(read<needed){shortage=true;graphSilenced+=needed-read;}
                for(unsigned i=0;i<needed*2;++i)if(!std::isfinite(f.input[i]))f.input[i]=0;
                f.inputOffset=0;f.inputCount=needed;
            }
            if(stopping)return {NativeTransferCode::Cancelled};
            if(produced!=frames)return {NativeTransferCode::Failed};
        }else{std::fill_n(f.output.data(),frames*2,0.f);silenced+=frames;}
        if(shortage){f.primed=false;f.observing=false;}
        writePacket.fill(0);auto* packet=writePacket.data();Put(packet,0,0x41575654,4);Put(packet,4,64,4);
        Put(packet,8,1,2);Put(packet,12,2,2);Put(packet,14,2,2);Put(packet,16,generation->wireGeneration,8);
        Put(packet,24,writeSequence,8);Put(packet,40,frames,4);Put(packet,44,48000,4);
        Put(packet,48,2,2);Put(packet,50,1,2);Put(packet,52,frames*8,4);
        for(unsigned sample=0;sample<frames*2;++sample){
            const double value=std::isfinite(f.output[sample])?f.output[sample]:0;
            const auto pcm=static_cast<std::int64_t>(std::llround((std::max)(-2147483648.0,(std::min)(2147483647.0,value*2147483648.0))));
            Put(packet,64+sample*4,static_cast<std::uint32_t>(pcm),4);
        }
        pendingBytes=64+frames*8;pendingFrames=frames;return {NativeTransferCode::Ok};
    }
    NativeTransferResult WriteDestination(std::uint32_t& credit,bool& full) noexcept {
        full=false;
        if(!pendingBytes){
            if(!credit)return {NativeTransferCode::Ok};
            const auto frames=(std::min)(credit,512u);const auto result=MakeCapture(frames);
            if(result.code!=NativeTransferCode::Ok)return result;credit-=frames;
        }
        const auto result=transport->WriteCapture(writePacket.data(),pendingBytes);
        if(result.code==NativeTransferCode::Full){full=true;return {NativeTransferCode::Ok};}
        if(result.code!=NativeTransferCode::Ok)return result;
        captureFrames+=pendingFrames;pendingBytes=pendingFrames=0;
        if(writeSequence==(std::numeric_limits<std::uint64_t>::max)())wroteMax=true;else ++writeSequence;
        return result;
    }
    NativeTransferResult Pump() noexcept {
        std::uint64_t previous=clock->NowNanoseconds(),next=previous+TickNs,lastStatus=previous,remainder=0;
        std::uint32_t credit=0;
        auto& source=*generation->windowsRenderSource;auto& destination=*generation->windowsCaptureDestination;
        while(!stopping && clock->WaitUntil(next)) {
            const auto now=clock->NowNanoseconds();const auto elapsed=now>=previous?now-previous:0;
            if(elapsed>TickNs)skipped+=(elapsed/TickNs)-1;
            previous=now;next=now+TickNs;
            // Divide first: elapsed*48000 would overflow after a long suspension.
            const auto frames=(elapsed/1000000000)*48000;
            const auto fractional=(elapsed%1000000000)*48000+remainder;
            const auto added=frames+fractional/1000000000;remainder=fractional%1000000000;
            const auto available=static_cast<std::uint64_t>(credit)+added;
            credit=static_cast<std::uint32_t>((std::min)(available,std::uint64_t{2048}));discarded+=available-credit;
            ObserveBegin(*sourceFlow,source,true,now);ObserveBegin(*destinationFlow,destination,false,now);
            bool more=true,full=false;
            for(unsigned pass=0;pass<4 && !stopping;++pass){
                if(more){const auto result=ReadSource(more);if(result.code!=NativeTransferCode::Ok)return result;}
                if(!full && (credit || pendingBytes)){
                    const auto result=WriteDestination(credit,full);if(result.code!=NativeTransferCode::Ok)return result;
                }
            }
            ObserveEnd(*sourceFlow,source,true,more,clock->NowNanoseconds());
            ObserveEnd(*destinationFlow,destination,false,full || credit || pendingBytes,clock->NowNanoseconds());
            if(now>=lastStatus && now-lastStatus>=250000000 && !stopping){
                std::uint64_t counters[3]{};const auto result=transport->ReadStatus(counters);
                if(result.code!=NativeTransferCode::Ok)return result;
                for(unsigned i=0;i<3;++i)kernel[i]=counters[i];lastStatus=now;
            }
        }
        return {NativeTransferCode::Cancelled};
    }
    void Run(std::uint64_t epoch,std::shared_ptr<WasapiBridgeIo> source,std::shared_ptr<WasapiBridgeIo> destination) noexcept {
        const auto fp=_mm_getcsr();_mm_setcsr(fp|0x8040); // worker FTZ/DAZ, restored on exit
        NativeSessionInfo session;auto result=stopping?NativeTransferResult{NativeTransferCode::Cancelled}:transport->Open(session);
        if(result.code==NativeTransferCode::Ok && stopping)result.code=NativeTransferCode::Cancelled;
        if(result.code==NativeTransferCode::Ok && !session.wireGeneration)result.code=NativeTransferCode::InvalidPacket;
        if(result.code==NativeTransferCode::Ok) {
            try{generation=std::make_shared<NativeAudioGeneration>(NativeAudioGeneration{epoch,session.wireGeneration,source,destination});}
            catch(...){result.code=NativeTransferCode::Failed;}
        }
        if(result.code!=NativeTransferCode::Ok){error=result.code;systemError=result.systemError;state=NativeAudioState::Failed;transport->Close();SignalReady(result.code);_mm_setcsr(fp);return;}
        state=NativeAudioState::Running;SignalReady(NativeTransferCode::Ok);
        result=Pump();transport->Close();error=result.code;systemError=result.systemError;
        state=stopping?NativeAudioState::Stopped:NativeAudioState::Failed;_mm_setcsr(fp);
    }
};
std::unique_ptr<INativeAudioScheduler> CreateNativeAudioScheduler(){return std::make_unique<Scheduler>();}
NativeAudioBackend::NativeAudioBackend(std::unique_ptr<INativeAudioTransport> t,std::unique_ptr<INativeAudioScheduler> c)
    :impl_(std::make_unique<Impl>(std::move(t),std::move(c))){}
NativeAudioBackend::~NativeAudioBackend(){Stop();}
NativeTransferCode NativeAudioBackend::Start(std::uint32_t rate,std::uint32_t quantum,std::uint64_t epoch) noexcept {
    auto& p=*impl_;if(p.used)return NativeTransferCode::Failed;p.used=true;
    const auto capacity=WasapiBridgeCapacityFrames(rate,quantum,48000,4096);
    if(!capacity || !epoch){p.state=NativeAudioState::Failed;p.error=NativeTransferCode::InvalidPacket;return NativeTransferCode::InvalidPacket;}
    try{
        auto source=std::make_shared<WasapiBridgeIo>(WasapiBridgeFormat{WasapiBridgeDirection::Capture,2,rate,quantum,epoch,capacity});
        auto destination=std::make_shared<WasapiBridgeIo>(WasapiBridgeFormat{WasapiBridgeDirection::Render,2,rate,quantum,epoch,capacity});
        p.sourceFlow=std::make_unique<Flow>(true,rate,quantum,capacity);p.destinationFlow=std::make_unique<Flow>(false,rate,quantum,capacity);
        p.sourceRatio=p.sourceFlow->nominal;p.destinationRatio=p.destinationFlow->nominal;
        source->BeginCapturePriming();p.state=NativeAudioState::Starting;
        p.worker=std::thread([&p,epoch,source,destination]{p.Run(epoch,source,destination);});
        std::unique_lock<std::mutex> lock(p.startupMutex);p.startupEvent.wait(lock,[&]{return p.ready;});return p.startupResult;
    }catch(...){p.state=NativeAudioState::Failed;p.error=NativeTransferCode::Failed;return NativeTransferCode::Failed;}
}
void NativeAudioBackend::RequestStop() noexcept {auto& p=*impl_;p.stopping=true;p.transport->RequestCancel();p.clock->RequestStop();}
void NativeAudioBackend::Stop() noexcept {RequestStop();if(impl_->worker.joinable())impl_->worker.join();}
std::shared_ptr<NativeAudioGeneration> NativeAudioBackend::Generation() const noexcept{return impl_->generation;}
NativeAudioHealth NativeAudioBackend::Health() const noexcept {
    const auto& p=*impl_;NativeAudioHealth h;h.state=p.state.load();h.error=p.error.load();h.systemError=p.systemError.load();
    h.renderNativeFrames=p.renderFrames;h.captureNativeFrames=p.captureFrames;h.captureSilencedNativeFrames=p.silenced;
    h.captureSilencedGraphFrames=p.graphSilenced;
    h.skippedDeadlines=p.skipped;h.discardedPacingFrames=p.discarded;h.controlBacklogUpdates=p.backlog;h.discontinuities=p.discontinuities;
    h.kernelRenderDroppedNativeFrames=p.kernel[0];h.kernelCaptureDroppedNativeFrames=p.kernel[1];h.kernelCaptureSilencedNativeFrames=p.kernel[2];
    h.renderSourceRatio=p.sourceRatio;h.captureDestinationRatio=p.destinationRatio;return h;
}
