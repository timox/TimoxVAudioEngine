#include "wasapi_bridge_io.h"
#include <array>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace {
thread_local bool trackAllocations = false;
thread_local std::size_t allocations = 0;
void Require(bool ok, const char* description) { if (!ok) throw std::runtime_error(description); }
}
void* operator new(std::size_t size) {
    if (trackAllocations) ++allocations;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

static WasapiBridgeFormat Format(WasapiBridgeDirection direction) { return {direction, 2, 48000, 16, 7, 16}; }
static void NativeBurstCapacity() {
    // Integer reference: bound both capture output and render input at +/-0.2%.
    for (unsigned graphRate : {8000U,44100U,48000U,384000U})
    for (unsigned nativeRate : {8000U,44100U,48000U,384000U})
    for (unsigned quantum : {16U,32U,64U,128U,256U,512U,2048U,8192U}) {
        const unsigned nativeFrames=nativeRate/50; // One 20 ms native buffer.
        const std::uint64_t denominator=static_cast<std::uint64_t>(nativeRate)*998;
        const auto burst=(static_cast<std::uint64_t>(nativeFrames)*graphRate*1000+denominator-1)/denominator;
        const auto jitter=(static_cast<std::uint64_t>(graphRate)*20+999)/1000;
        const auto phase=(static_cast<std::uint64_t>(graphRate)*1000+denominator-1)/denominator;
        const auto required=burst+jitter+quantum+phase+1;
        const auto capacity=WasapiBridgeCapacityFrames(graphRate,quantum,nativeRate,nativeFrames);
        Require(capacity<=65536 && capacity/2>=required && capacity>=8*quantum,
            "native burst and scheduler reserve do not fit around prefill target");
        for(auto direction : {WasapiBridgeDirection::Capture,WasapiBridgeDirection::Render}) {
            WasapiBridgeIo io({direction,1,graphRate,quantum,1,capacity});
            std::vector<float> samples(capacity,0.25f);float* output[]={samples.data()};const float* input[]={samples.data()};
            const auto write=[&](std::size_t frames) {
                if(direction==WasapiBridgeDirection::Capture)return io.WriteCaptureInterleaved(samples.data(),frames,samples.size());
                std::size_t total=0;while(total<frames){const auto n=(std::min)(frames-total,static_cast<std::size_t>(quantum));const auto written=io.WriteRenderBlock(input,n,1);if(written!=n)return total+written;total+=written;}return total;
            };
            const auto read=[&](std::size_t frames) {
                if(direction==WasapiBridgeDirection::Render)return io.ReadRenderInterleaved(samples.data(),frames,samples.size());
                std::size_t total=0;while(total<frames){const auto n=(std::min)(frames-total,static_cast<std::size_t>(quantum));const auto got=io.ReadCaptureBlock(output,n,1);if(got!=n)return total+got;total+=got;}return total;
            };
            Require(write(capacity/2)==capacity/2,"prefill failed");
            Require(read(required)==required,"bounded missing service starved prefill");
            Require(write(required*2)==required*2,"bounded burst overflowed headroom");
            Require(!io.RejectedFrames() && !io.SilencedFrames(),"reserve exercise lost frames");
        }
    }
    Require(WasapiBridgeCapacityFrames(48000,16,44100,960)>2090,"small quantum regression");
    Require(WasapiBridgeCapacityFrames(384000,16,8000,8000)==0,"oversized conversion silently clamped");
    Require(WasapiBridgeCapacityFrames(0,16,48000,960)==0 &&
        WasapiBridgeCapacityFrames(48000,16,0,960)==0 &&
        WasapiBridgeCapacityFrames(48000,15,48000,960)==0 &&
        WasapiBridgeCapacityFrames(48000,16,48000,UINT32_MAX)==0,"invalid capacity dimensions accepted");
}
static void CaptureReadiness() {
    WasapiBridgeIo io(Format(WasapiBridgeDirection::Capture));
    const float samples[]={1,11,2,12,3,13};float left[16],right[16];float* planes[]={left,right};
    io.BeginCapturePriming();io.WriteCaptureInterleaved(samples,3,6);
    trackAllocations=true;
    const auto before=allocations;
    const auto gated=io.ReadCaptureBlock(planes,16,2);
    const bool silence=std::all_of(left,left+16,[](float x){return x==0;}) &&
        std::all_of(right,right+16,[](float x){return x==0;});
    const auto retained=io.BufferedFrames();
    io.CompleteCapturePriming();const auto read=io.ReadCaptureBlock(planes,3,2);
    const bool noAllocation=allocations==before;trackAllocations=false;
    Require(gated==0 && silence && retained==3,"priming gate drained or failed to silence");
    Require(read==3 && left[0]==1 && right[2]==13 && io.SilencedFrames()==16,"gate lost/reordered prefill or counted silence incorrectly");
    Require(noAllocation,"readiness allocated in callback");
}
static void Capture() {
    auto format = Format(WasapiBridgeDirection::Capture);
    WasapiBridgeIo capture(format);
    format.channels = 1; format.generationId = 999;
    Require(capture.Format().channels == 2 && capture.Format().generationId == 7, "generation copied immutably");
    float left[16], right[16];
    std::fill_n(left, 16, -1.0f); std::fill_n(right, 16, -1.0f);
    float* planes[] = {left, right};
    Require(capture.ReadCaptureBlock(planes, 16, 2) == 0, "empty capture frames");
    for (unsigned i = 0; i < 16; ++i) Require(left[i] == 0 && right[i] == 0, "empty capture must silence every plane");
    const float samples[] = {1, 11, 2, 12, 3, 13};
    Require(capture.WriteCaptureInterleaved(samples, 3, 6) == 3, "capture write");
    Require(capture.ReadCaptureBlock(planes, 5, 2) == 3, "capture partial read");
    Require(left[0] == 1 && left[2] == 3 && right[0] == 11 && right[2] == 13 &&
        left[3] == 0 && right[4] == 0, "capture channel order and partial silence");
    Require(capture.SilencedFrames() == 18 && capture.RejectedFrames() == 0, "silence measured in frames, not channel samples or operations");
}
static void RenderAndNoAllocation() {
    WasapiBridgeIo render(Format(WasapiBridgeDirection::Render));
    float left[16], right[16], output[36];
    for (unsigned i = 0; i < 16; ++i) { left[i] = static_cast<float>(i); right[i] = static_cast<float>(100 + i); }
    const float* planes[] = {left, right};
    std::fill_n(output, 36, -1.0f);
    trackAllocations = true; allocations = 0;
    const auto first = render.WriteRenderBlock(planes, 12, 2);
    const auto partial = render.WriteRenderBlock(planes, 8, 2);
    const auto read = render.ReadRenderInterleaved(output, 16, 32);
    const auto empty = render.ReadRenderInterleaved(output + 32, 2, 4);
    const auto rejected = render.RejectedFrames();
    const auto silenced = render.SilencedFrames();
    trackAllocations = false;
    Require(allocations == 0, "render transfer and stats must allocate nothing");
    Require(first == 12 && partial == 4 && read == 16 && empty == 0, "render partial frames");
    Require(output[0] == 0 && output[1] == 100 && output[22] == 11 && output[23] == 111 &&
        output[24] == 0 && output[31] == 103 && output[32] == 0 && output[35] == 0, "render FIFO/channel order/silence");
    Require(rejected == 4 && silenced == 2, "render rejected/silenced frame accounting");
    Require(render.WriteRenderBlock(planes, 2, 2) == 2, "render wrap input");
    Require(render.ReadRenderInterleaved(output, 4, 8) == 2 && output[2] == 1 && output[3] == 101 &&
        output[4] == 0 && output[7] == 0, "render short read pads tail");
}
static void InvalidCalls() {
    WasapiBridgeIo capture(Format(WasapiBridgeDirection::Capture));
    WasapiBridgeIo render(Format(WasapiBridgeDirection::Render));
    float input[32]{};
    float left[16], right[16];
    std::fill_n(left, 16, 9.0f); std::fill_n(right, 16, 9.0f);
    float* output[] = {left, right};
    float* broken[] = {left, nullptr};
    const float* source[] = {left, right};
    const float* badSource[] = {left, nullptr};
    Require(capture.WriteCaptureInterleaved(input, 1, 2) == 1, "prime capture");
    allocations = 0; trackAllocations = true;
    bool correct = capture.ReadCaptureBlock(output, 17, 2) == 0 &&
        capture.ReadCaptureBlock(output, 1, 1) == 0 && capture.ReadCaptureBlock(broken, 1, 2) == 0 &&
        capture.ReadCaptureBlock(nullptr, 1, 2) == 0 && capture.ReadCaptureBlock(nullptr, 0, 2) == 0 &&
        capture.WriteCaptureInterleaved(input, 17, 32) == 0 && capture.WriteCaptureInterleaved(input, 1, 1) == 0 &&
        capture.WriteCaptureInterleaved(nullptr, 1, 2) == 0 && capture.WriteCaptureInterleaved(nullptr, 0, 0) == 0 &&
        render.WriteRenderBlock(source, 17, 2) == 0 && render.WriteRenderBlock(source, 1, 1) == 0 &&
        render.WriteRenderBlock(badSource, 1, 2) == 0 && render.WriteRenderBlock(nullptr, 1, 2) == 0 &&
        render.ReadRenderInterleaved(input, 1, 1) == 0 && render.ReadRenderInterleaved(input, 17, 32) == 0 &&
        render.ReadRenderInterleaved(nullptr, 1, 2) == 0 &&
        capture.WriteRenderBlock(source, 1, 2) == 0 && capture.ReadRenderInterleaved(input, 1, 2) == 0 &&
        render.WriteCaptureInterleaved(input, 1, 2) == 0 && render.ReadCaptureBlock(output, 1, 2) == 0 &&
        capture.RejectedFrames() == 0 && capture.SilencedFrames() == 0 &&
        render.RejectedFrames() == 0 && render.SilencedFrames() == 0;
    trackAllocations = false;
    Require(correct && allocations == 0, "invalid/zero/direction calls return zero without allocation or counters");
    Require(left[0] == 9 && right[0] == 9, "invalid reads leave buffers untouched");
    trackAllocations = true;
    const auto got = capture.ReadCaptureBlock(output, 2, 2);
    trackAllocations = false;
    Require(got == 1 && left[0] == 0 && right[0] == 0 && left[1] == 0 && allocations == 0,
        "invalid calls do not consume capture and valid capture allocates nothing");
}
static void InvalidFormats() {
    const auto good = Format(WasapiBridgeDirection::Capture);
    auto bad = good;
    const auto reject = [](const WasapiBridgeFormat& f) {
        bool rejected = false;
        try { WasapiBridgeIo io(f); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "invalid generation format must fail before storage allocation");
    };
    for (std::size_t channels : {std::size_t{0}, std::size_t{257}, (std::numeric_limits<std::size_t>::max)()}) { bad = good; bad.channels = channels; reject(bad); }
    for (std::uint32_t rate : {0u, 7999u, 384001u}) { bad = good; bad.graphSampleRate = rate; reject(bad); }
    for (std::size_t quantum : {std::size_t{0}, std::size_t{15}, std::size_t{8193}}) { bad = good; bad.maxQuantum = quantum; reject(bad); }
    for (std::size_t capacity : {std::size_t{0}, std::size_t{15}, std::size_t{65537}, (std::numeric_limits<std::size_t>::max)()}) { bad = good; bad.capacityFrames = capacity; reject(bad); }
    bad = good; bad.direction = static_cast<WasapiBridgeDirection>(99); reject(bad);
    bad = good; bad.generationId = 0; reject(bad);
    for (std::uint32_t rate : {8000u, 384000u}) { bad = good; bad.graphSampleRate = rate; WasapiBridgeIo valid(bad); }
    bad = good; bad.channels = 256; bad.maxQuantum = 8192; bad.capacityFrames = 8192; WasapiBridgeIo maximum(bad);
}
static void BackendBlocksAndGenerations() {
    auto format = Format(WasapiBridgeDirection::Capture);
    format.channels = 3; format.capacityFrames = 32;
    WasapiBridgeIo oldGeneration(format);
    format.generationId = 8;
    WasapiBridgeIo newGeneration(format);
    float input[96];
    for (unsigned i = 0; i < 32; ++i)
        for (unsigned channel = 0; channel < 3; ++channel)
            input[i * 3 + channel] = static_cast<float>(i * 10 + channel);
    float first[16]{}, second[16]{}, third[16]{};
    float* planes[] = {first, second, third};
    allocations = 0; trackAllocations = true;
    const auto accepted = oldGeneration.WriteCaptureInterleaved(input, 32, 96);
    const auto full = oldGeneration.WriteCaptureInterleaved(input, 1, 3);
    const auto emptySuccessor = newGeneration.ReadCaptureBlock(planes, 16, 3);
    const auto firstBlock = oldGeneration.ReadCaptureBlock(planes, 16, 3);
    const bool firstOrder = first[15] == 150 && second[15] == 151 && third[15] == 152;
    const auto secondBlock = oldGeneration.ReadCaptureBlock(planes, 16, 3);
    const bool secondOrder = first[0] == 160 && second[15] == 311 && third[15] == 312;
    const auto overflowArgument = oldGeneration.WriteCaptureInterleaved(input, (std::numeric_limits<std::size_t>::max)(), 96);
    trackAllocations = false;
    Require(allocations == 0 && accepted == 32 && full == 0 && emptySuccessor == 0 &&
        firstBlock == 16 && secondBlock == 16 && firstOrder && secondOrder && overflowArgument == 0,
        "backend blocks span graph quanta without allocations or generation contamination");
    Require(oldGeneration.RejectedFrames() == 1 && oldGeneration.SilencedFrames() == 0 &&
        newGeneration.RejectedFrames() == 0 && newGeneration.SilencedFrames() == 16,
        "generation counters independent and extreme frame counts rejected");
}
static void BufferedSnapshot() {
    WasapiBridgeIo capture(Format(WasapiBridgeDirection::Capture));
    float input[32]{}, left[16]{}, right[16]{};
    float* planes[] = {left, right};
    allocations = 0; trackAllocations = true;
    const auto empty = capture.BufferedFrames();
    capture.WriteCaptureInterleaved(input, 12, 24);
    const auto partial = capture.BufferedFrames();
    capture.WriteCaptureInterleaved(input, 8, 16);
    const auto full = capture.BufferedFrames();
    capture.ReadCaptureBlock(planes, 5, 2);
    const auto drained = capture.BufferedFrames();
    capture.ReadCaptureBlock(planes, 16, 2);
    const auto final = capture.BufferedFrames();
    trackAllocations = false;
    Require(empty == 0 && partial == 12 && full == 16 && drained == 11 && final == 0 && allocations == 0,
        "buffered snapshot follows accepted writes and reads without allocation");
}
static_assert(!std::is_copy_constructible_v<WasapiBridgeIo> && !std::is_move_constructible_v<WasapiBridgeIo>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<WasapiBridgeIo>().Format())>>);
static_assert(noexcept(std::declval<WasapiBridgeIo>().ReadCaptureBlock(nullptr, 0, 0)));
static_assert(noexcept(std::declval<WasapiBridgeIo>().WriteRenderBlock(nullptr, 0, 0)));
static_assert(noexcept(std::declval<WasapiBridgeIo>().WriteCaptureInterleaved(nullptr, 0, 0)));
static_assert(noexcept(std::declval<WasapiBridgeIo>().ReadRenderInterleaved(nullptr, 0, 0)));
static_assert(noexcept(std::declval<WasapiBridgeIo>().BufferedFrames()));
int main() {
    try { NativeBurstCapacity(); CaptureReadiness(); Capture(); RenderAndNoAllocation(); InvalidCalls(); InvalidFormats(); BackendBlocksAndGenerations(); BufferedSnapshot();
        std::puts("WASAPI bridge IO tests passed"); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
