#include "spsc_audio_ring.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
thread_local bool trackAllocations = false;
thread_local std::size_t allocations = 0;
int failures = 0;
void Check(bool condition, const char* description) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", description); ++failures; }
}
}

void* operator new(std::size_t size) {
    if (trackAllocations) ++allocations;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
void Dimensions() {
    const std::size_t invalid[][2] = {
        {0, 2}, {5, 0}, {std::numeric_limits<std::size_t>::max(), 2},
        {2, std::numeric_limits<std::size_t>::max()},
        {std::numeric_limits<std::size_t>::max() / sizeof(float) + 1, 1}
    };
    for (const auto& dimensions : invalid) {
        bool rejected = false;
        try { SpscAudioRing ring(dimensions[0], dimensions[1]); }
        catch (const std::invalid_argument&) { rejected = true; }
        catch (const std::length_error&) { rejected = true; }
        Check(rejected, "invalid dimensions rejected before allocating");
    }
}

void PartialAndWrap() {
    SpscAudioRing ring(2, 5);
    const float input[] = {1, 11, 2, 12, 3, 13, 4, 14, 5, 15, 6, 16, 7, 17};
    float output[14];
    for (float& sample : output) sample = -123;
    Check(ring.Read(output, 1) == 0 && output[0] == -123, "empty read leaves caller buffer untouched");
    Check(ring.Write(input, 7) == 5, "full write returns partial frames");
    Check(ring.Capacity() == 5 && ring.Available() == 5, "full capacity is usable");
    Check(ring.Write(input, 1) == 0, "full ring never overwrites oldest");
    Check(ring.Read(output, 3) == 3, "partial drain");
    Check(ring.Write(input + 10, 2) == 2, "write wraps arbitrary capacity");
    Check(ring.Read(output + 6, 4) == 4, "read crosses wrap");
    for (std::size_t i = 0; i < 14; ++i) Check(output[i] == input[i], "FIFO frame/channel order");
    Check(ring.Available() == 0, "drained ring empty");
    Check(ring.Read(output, 2) == 0, "empty read count");
    Check(ring.Write(nullptr, 0) == 0 && ring.Read(nullptr, 0) == 0, "zero requests accept null");
    Check(ring.WritePlanar(nullptr, 0) == 0 && ring.ReadPlanar(nullptr, 0) == 0, "zero planar requests accept null");
    Check(ring.Underruns() == 2 && ring.Overruns() == 2, "counters count short operations once, not frames or zero requests");
    for (float& sample : output) sample = -123;
    Check(ring.Write(input, 2) == 2 && ring.Read(output, 3) == 2, "short nonempty read returns available frames");
    Check(output[0] == 1 && output[1] == 11 && output[2] == 2 && output[3] == 12 &&
          output[4] == -123 && output[5] == -123, "short interleaved read leaves unwritten frame untouched");
    Check(ring.Underruns() == 3, "partial nonempty read counts one underrun");
    SpscAudioRing one(1, 1);
    for (int i = 0; i < 20; ++i) {
        Check(one.Write(input, 1) == 1 && one.Read(output, 1) == 1 && output[0] == 1,
              "single frame capacity wraps repeatedly");
    }
}

void PlanarAndNoAllocation() {
    SpscAudioRing ring(2, 3);
    const float left[] = {1, 2, 3, 4};
    const float right[] = {11, 12, 13, 14};
    const float* source[] = {left, right};
    float interleaved[8]{};
    float outLeft[] = {-1, -1, -1, -1};
    float outRight[] = {-1, -1, -1, -1};
    float* destination[] = {outLeft, outRight};
    allocations = 0;
    trackAllocations = true;
    const auto written = ring.WritePlanar(source, 4);
    const auto read = ring.Read(interleaved, 2);
    const auto writtenAgain = ring.WritePlanar(source, 2);
    const auto readPlanar = ring.ReadPlanar(destination, 4);
    const bool wrappedOrder = outLeft[0] == 3 && outRight[0] == 13 &&
        outLeft[1] == 1 && outRight[1] == 11 && outLeft[2] == 2 && outRight[2] == 12;
    const auto writtenInterleaved = ring.Write(interleaved, 2);
    const auto readAgain = ring.ReadPlanar(destination, 2);
    const auto available = ring.Available();
    const auto underruns = ring.Underruns();
    const auto overruns = ring.Overruns();
    trackAllocations = false;
    Check(allocations == 0, "all transfer and observation operations allocate no memory");
    Check(written == 3 && read == 2 && writtenAgain == 2 && readPlanar == 3,
          "planar transfers return partial frame counts");
    Check(wrappedOrder, "planar read crosses wrap without mixing channels");
    Check(interleaved[0] == 1 && interleaved[1] == 11 && interleaved[2] == 2 && interleaved[3] == 12,
          "planar source converted to interleaved channels");
    Check(writtenInterleaved == 2 && readAgain == 2 && available == 0, "interleaved writes can be drained planar");
    Check(outLeft[0] == 1 && outRight[0] == 11 && outLeft[1] == 2 && outRight[1] == 12,
          "interleaved source converted to planar channels");
    Check(outLeft[2] == 2 && outRight[2] == 12 && outLeft[3] == -1 && outRight[3] == -1,
          "wrapped planar read preserves channel order and leaves unread tail untouched");
    Check(underruns == 1 && overruns == 1, "planar short operations update counters");
}

// Dropping/duplicating/reordering a frame or publishing before copying corrupts this sequence.
// Completion depends on transferred frames, never on an assumed scheduler speed.
void ConcurrentSequence(bool planar) {
    static constexpr std::size_t frames = 250000;
    static constexpr std::size_t channels = 3;
    SpscAudioRing ring(channels, 127);
    std::atomic<bool> corrupted{false};
    std::atomic<std::size_t> allocationCount{0};
    std::thread producer([&] {
        std::array<float, 173 * channels> buffer{};
        std::array<std::array<float, 173>, channels> planes{};
        const float* pointers[] = {planes[0].data(), planes[1].data(), planes[2].data()};
        std::size_t sent = 0, iteration = 0;
        allocations = 0;
        trackAllocations = true;
        while (sent < frames) {
            std::size_t count = 1 + (iteration++ * 37) % 173;
            if (count > frames - sent) count = frames - sent;
            for (std::size_t f = 0; f < count; ++f)
                for (std::size_t c = 0; c < channels; ++c)
                    buffer[f * channels + c] = planes[c][f] = static_cast<float>((sent + f) * channels + c);
            const auto n = planar ? ring.WritePlanar(pointers, count) : ring.Write(buffer.data(), count);
            sent += n;
            if (n == 0) std::this_thread::yield();
        }
        trackAllocations = false;
        allocationCount.fetch_add(allocations);
    });
    std::thread consumer([&] {
        std::array<float, 157 * channels> buffer{};
        std::array<std::array<float, 157>, channels> planes{};
        float* pointers[] = {planes[0].data(), planes[1].data(), planes[2].data()};
        std::size_t received = 0, iteration = 0;
        allocations = 0;
        trackAllocations = true;
        while (received < frames) {
            const auto count = 1 + (iteration++ * 53) % 157;
            const auto n = planar ? ring.ReadPlanar(pointers, count) : ring.Read(buffer.data(), count);
            for (std::size_t f = 0; f < n; ++f)
                for (std::size_t c = 0; c < channels; ++c)
                    if ((planar ? planes[c][f] : buffer[f * channels + c]) != static_cast<float>((received + f) * channels + c))
                        corrupted.store(true);
            received += n;
            if (n == 0) std::this_thread::yield();
        }
        trackAllocations = false;
        allocationCount.fetch_add(allocations);
    });
    producer.join();
    consumer.join();
    Check(!corrupted.load(), "concurrent irregular packet stream preserves every frame/channel");
    Check(ring.Available() == 0, "concurrent stream completely drained");
    Check(allocationCount.load() == 0, "concurrent transfer paths allocate no memory");
}
}

int main() {
    Dimensions();
    PartialAndWrap();
    PlanarAndNoAllocation();
    ConcurrentSequence(false);
    ConcurrentSequence(true);
    if (failures == 0) std::puts("SpscAudioRing tests passed.");
    return failures == 0 ? 0 : 1;
}
