#include "audio_routing_runtime.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace {
thread_local bool tracking = false;
thread_local unsigned allocations = 0;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Near(float a, float b) { return std::abs(a-b) < 1e-5f; }
auto Io(WasapiBridgeDirection direction, unsigned rate = 48000, unsigned quantum = 16) {
    return std::make_shared<WasapiBridgeIo>(WasapiBridgeFormat{direction, 2, rate, quantum, 1, 32});
}
AudioRoutingLayout Layout() { AudioRoutingLayout layout; layout.sampleRate = 48000; layout.bufferFrames = 16; return layout; }
auto Create(AudioRoutingLayout layout) {
    std::string error;
    auto runtime = AudioRoutingRuntime::Create(std::move(layout), error);
    if (!runtime) throw std::runtime_error(error);
    return runtime;
}
}
void* operator new(std::size_t size) {
    if (tracking) ++allocations;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static void Transfer() {
    auto capture = Io(WasapiBridgeDirection::Capture), render = Io(WasapiBridgeDirection::Render);
    auto layout = Layout();
    layout.wasapiBindings = {{"capture", capture, L"microphone", false}, {"render", render, L"speakers", false}};
    layout.routes = {{"gain", "wasapi:capture:input:1", "wasapi:render:output:2", -6.020599913, false},
        {"muted", "wasapi:capture:input:2", "wasapi:render:output:1", 0, true}};
    auto runtime = Create(std::move(layout));
    runtime->SetStageMeasurementsEnabled(true);
    float input[32], output[32];
    for (unsigned n=0; n<16; ++n) { input[2*n]=0.5f; input[2*n+1]=0.75f; }
    const auto captureOwners = capture.use_count(), renderOwners = render.use_count();
    Require(capture->WriteCaptureInterleaved(input, 16, 32)==16, "prime capture");
    allocations=0; tracking=true;
    runtime->Process(nullptr, nullptr, 16, true);
    tracking=false;
    Require(allocations==0 && capture.use_count()==captureOwners && render.use_count()==renderOwners,
        "callback allocates or changes shared ownership");
    Require(render->ReadRenderInterleaved(output,16,32)==16, "render output missing");
    for (unsigned n=0; n<16; ++n) Require(output[2*n]==0 && Near(output[2*n+1],0.25f), "gain/mute/channel routing");
    const auto meters = runtime->ReadMeters();
    const auto meter = std::find_if(meters.begin(),meters.end(),[](const auto& m){ return m.endpointId=="wasapi:render:output:2"; });
    Require(meter!=meters.end() && Near(meter->peakLinear,0.25f), "WASAPI meter absent");
    for (unsigned block=1; block<16; ++block) {
        runtime->Process(nullptr,nullptr,16,true);
        Require(render->ReadRenderInterleaved(output,16,32)==16,"silent output missing");
        for (float value : output) Require(value==0,"empty capture or precleared flag leaves stale WASAPI output");
    }
    const auto stages=runtime->ConsumeStageCounters();
    Require(stages.routing.samples==1 && stages.clientRead.samples==0,"bridge-only routing stage unobserved or attributed to client");
    // A new runtime may share an IO only when the single graph callback switches
    // between runtimes serially. References keep the retired generation alive.
    std::weak_ptr<WasapiBridgeIo> lifetime=capture;
    capture.reset();
    Require(!lifetime.expired(),"runtime fails to own borrowed generation");
    runtime.reset();
    Require(lifetime.expired(),"runtime retains generation after off-thread destruction");
}

static void PhysicalDirections() {
    auto capture=Io(WasapiBridgeDirection::Capture), render=Io(WasapiBridgeDirection::Render);
    auto layout=Layout(); layout.physicalDriverId="card";
    layout.physicalInputChannels=layout.physicalOutputChannels=1;
    layout.wasapiBindings={{"in",capture,L"capture",false},{"out",render,L"render",false}};
    layout.routes={{"to-physical","wasapi:in:input:2","physical:card:output:1",0,false},
        {"to-wasapi","physical:card:input:1","wasapi:out:output:1",0,false}};
    auto runtime=Create(std::move(layout));
    float samples[32], input[16], output[16]{}, rendered[32];
    for(unsigned n=0;n<16;++n){samples[2*n]=0.1f;samples[2*n+1]=0.3f;input[n]=0.6f;}
    capture->WriteCaptureInterleaved(samples,16,32);
    const float* inputs[]={input}; float* outputs[]={output};
    runtime->Process(inputs,outputs,16,true);
    Require(render->ReadRenderInterleaved(rendered,16,32)==16,"physical render missing");
    for(unsigned n=0;n<16;++n) Require(Near(output[n],0.3f)&&Near(rendered[2*n],0.6f)&&rendered[2*n+1]==0,"physical/WASAPI bridge values");
    Require(!RoutingGraph::SupportsLink(RoutingEndpointType::PhysicalInput,RoutingEndpointType::PhysicalOutput),"legacy forbidden link enabled");
    Require(RoutingGraph::SupportsLink(RoutingEndpointType::VirtualOutput,RoutingEndpointType::VirtualInput)&&
        RoutingGraph::SupportsLink(RoutingEndpointType::VirtualOutput,RoutingEndpointType::PhysicalOutput)&&
        RoutingGraph::SupportsLink(RoutingEndpointType::PhysicalInput,RoutingEndpointType::VirtualInput),"legacy allowed links changed");
}

static void ApplicationDirections() {
    constexpr std::uint32_t frames = 16;
    const auto pid = GetCurrentProcessId();
    // Separate named-memory namespace from the user's driver slot 1. Public
    // graph metadata remains driver 1 so the real TimoxVasio routing path runs.
    constexpr std::uint32_t privateDriverSlot = 0x57415349u;
    auto created = AudioClientMapping::CreateClient(privateDriverSlot, pid, 48000, frames);
    Require(!!created, "create isolated application transport");
    Require(created->SetEngineAttached(true), "attach isolated application transport");
    AudioClientMapping::ChannelMask inputs{}, outputs{};
    inputs[0] = std::uint64_t{1} << 2;  // Sparse application input 3.
    outputs[0] = std::uint64_t{1} << 6; // Sparse application output 7.
    Require(created->SetActiveChannels(inputs, outputs), "activate sparse application channels");
    auto mapping = std::shared_ptr<AudioClientMapping>(std::move(created));
    auto capture = Io(WasapiBridgeDirection::Capture), render = Io(WasapiBridgeDirection::Render);
    auto layout = Layout();
    layout.clients = {{{1, pid, L"wasapi-routing-test.exe"}, mapping}};
    layout.wasapiBindings = {{"capture", capture, L"microphone", false}, {"render", render, L"speakers", false}};
    const auto prefix = "virtual:TimoxVasio:" + std::to_string(pid);
    layout.routes = {{"capture-to-app", "wasapi:capture:input:2", prefix + ":input:3", 0, false},
        {"app-to-render", prefix + ":output:7", "wasapi:render:output:1", -6.020599913, false}};
    auto runtime = Create(std::move(layout));
    float nativeCapture[frames * 2], applicationOutput[frames], applicationInput[frames]{}, nativeRender[frames * 2];
    for (std::uint32_t n = 0; n < frames; ++n) {
        nativeCapture[n * 2] = -0.75f;
        nativeCapture[n * 2 + 1] = 0.25f + static_cast<float>(n) / 1000;
        applicationOutput[n] = 0.4f + static_cast<float>(n) / 1000;
    }
    Require(capture->WriteCaptureInterleaved(nativeCapture, frames, frames * 2) == frames, "queue capture for application");
    Require(mapping->WriteClientOutput(applicationOutput, frames), "queue actual application output transport");
    allocations = 0; tracking = true;
    runtime->Process(nullptr, nullptr, frames, true);
    tracking = false;
    Require(allocations == 0, "application and WASAPI callback allocates");
    Require(mapping->WaitForClientSignal(0), "routed application not signalled");
    Require(mapping->ReadClientInput(applicationInput, frames) == frames, "application input transport missing capture block");
    Require(render->ReadRenderInterleaved(nativeRender, frames, frames * 2) == frames, "render missing application block");
    for (std::uint32_t n = 0; n < frames; ++n) {
        Require(Near(applicationInput[n], 0.25f + static_cast<float>(n) / 1000), "capture samples reached wrong sparse application channel");
        Require(Near(nativeRender[n * 2], (0.4f + static_cast<float>(n) / 1000) * 0.5f) && nativeRender[n * 2 + 1] == 0,
            "application samples/gain/unrouted silence wrong in render");
    }
    const auto meters = runtime->ReadMeters();
    const auto captureMeter = std::find_if(meters.begin(), meters.end(), [](const auto& meter) {
        return meter.endpointId == "wasapi:capture:input:2";
    });
    Require(captureMeter != meters.end() && Near(captureMeter->peakLinear, 0.265f), "capture source peak missing or from wrong plane");
}

static void BridgeFrameCounters() {
    auto capture = Io(WasapiBridgeDirection::Capture), render = Io(WasapiBridgeDirection::Render);
    auto layout = Layout();
    layout.wasapiBindings = {{"capture", capture, L"capture", false}, {"render", render, L"render", false}};
    layout.routes = {{"direct", "wasapi:capture:input:1", "wasapi:render:output:1", 0, false}};
    auto runtime = Create(std::move(layout));
    float samples[64]{};
    Require(capture->WriteCaptureInterleaved(samples, 32, 64) == 32, "fill capture capacity");
    Require(capture->WriteCaptureInterleaved(samples, 7, 14) == 0, "full capture should reject seven frames");
    // Two blocks exhaust capture and fill render. Two more blocks silence the
    // capture input and reject render output while its backend is not draining.
    for (unsigned block = 0; block < 4; ++block) runtime->Process(nullptr, nullptr, 16, true);
    Require(capture->RejectedFrames() == 7 && capture->SilencedFrames() == 32,
        "capture counters must count frames, not two-channel samples or incidents");
    Require(render->RejectedFrames() == 32 && render->SilencedFrames() == 0,
        "render overflow must count frames rejected by graph writes");
    Require(render->ReadRenderInterleaved(samples, 32, 64) == 32, "drain queued render frames");
    Require(render->ReadRenderInterleaved(samples, 7, 14) == 0 && render->SilencedFrames() == 7,
        "backend render starvation must count seven silence frames");
    // These are bridge frame counters, deliberately not AudioMeterSnapshot's
    // legacy client-transport underrun/overrun incident counters.
}

static void InvalidBindings() {
    const auto reject=[](AudioRoutingLayout layout){std::string error;Require(!AudioRoutingRuntime::Create(std::move(layout),error)&&!error.empty(),"invalid binding accepted");};
    auto capture=Io(WasapiBridgeDirection::Capture), render=Io(WasapiBridgeDirection::Render);
    auto base=Layout(); base.wasapiBindings={{"capture",capture,L"device",true},{"render",render,L"device",false}};
    auto bad=base;bad.routes={{"feedback","wasapi:capture:input:1","wasapi:render:output:1",0,false}};reject(bad);
    bad.wasapiBindings[1].resolvedEndpointId=L"different";Create(bad);
    bad=base;bad.wasapiBindings[1].endpointLoopback=true;reject(bad);
    bad=base;bad.wasapiBindings[0].resolvedEndpointId.clear();reject(bad);
    bad=base;bad.wasapiBindings[0].bridgeId="bad:input:1";reject(bad);
    bad=base;bad.wasapiBindings[0].bridgeId="";reject(bad);
    bad=base;bad.wasapiBindings[1].bridgeId="capture";reject(bad);
    bad=base;bad.wasapiBindings[1].io=capture;reject(bad);
    bad=base;bad.wasapiBindings[0].io.reset();reject(bad);
    bad=base;bad.wasapiBindings[0].io=Io(WasapiBridgeDirection::Capture,96000);reject(bad);
    bad=base;bad.bufferFrames=32;reject(bad);
    bad=base;bad.routes={{"missing-channel","wasapi:capture:input:3","wasapi:render:output:1",0,false}};reject(bad);
    bad=base;bad.routes={{"backwards","wasapi:render:output:1","wasapi:capture:input:1",0,false}};reject(bad);
}
int main(){
    try{Transfer();PhysicalDirections();ApplicationDirections();BridgeFrameCounters();InvalidBindings();std::puts("WASAPI routing runtime tests passed");}
    catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
