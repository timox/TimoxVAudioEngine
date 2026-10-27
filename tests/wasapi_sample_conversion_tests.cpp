#include "wasapi_sample_conversion.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
thread_local bool tracking = false;
thread_local std::size_t allocations = 0;
void Require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
WasapiMixFormat Format(unsigned bits, unsigned valid = 0, unsigned channels = 1,
    WasapiSampleEncoding encoding = WasapiSampleEncoding::PcmInteger) {
    return {48000, static_cast<std::uint16_t>(channels), static_cast<std::uint16_t>(bits),
        static_cast<std::uint16_t>(valid ? valid : bits), static_cast<std::uint16_t>(channels * bits / 8), 0, encoding};
}
}
void* operator new(std::size_t size) {
    if (tracking) ++allocations;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

static void IntegerReferences() {
    struct Case { WasapiMixFormat format; unsigned char bytes[16]; std::size_t size; };
    // Independently hand-encoded {-1, 0, +0.5, +maximum} in little endian.
    const Case cases[] = {
        {Format(8), {0x00, 0x80, 0xc0, 0xff}, 4},
        {Format(16), {0,0x80, 0,0, 0,0x40, 0xff,0x7f}, 8},
        {Format(24), {0,0,0x80, 0,0,0, 0,0,0x40, 0xff,0xff,0x7f}, 12},
        {Format(32), {0,0,0,0x80, 0,0,0,0, 0,0,0,0x40, 0xff,0xff,0xff,0x7f}, 16},
        {Format(32,24), {0,0,0,0x80, 0,0,0,0, 0,0,0,0x40, 0,0xff,0xff,0x7f}, 16},
        {Format(24,20), {0,0,0x80, 0,0,0, 0,0,0x40, 0xf0,0xff,0x7f}, 12},
        {Format(8,4), {0x00,0x80,0xc0,0xf0}, 4},
        {Format(32,1), {0,0,0,0x80, 0,0,0,0, 0,0,0,0, 0,0,0,0}, 16}
    };
    for (const auto& test : cases) {
        float output[4]{};
        Require(WasapiNativeToFloat(test.format, test.bytes, test.size, output, 4, 4), "PCM reference decode failed");
        Require(output[0] == -1 && output[1] == 0 &&
            output[2] == (test.format.validBits == 1 ? 0 : 0.5f), "PCM sign/zero/half decode");
        const auto maximum = static_cast<float>(1.0 - std::ldexp(1.0, 1 - test.format.validBits));
        Require(output[3] == maximum, "PCM positive maximum decode including binary32 rounding");
        const float input[] = {-1, 0, 0.5f, 1};
        unsigned char encoded[16]{};
        Require(WasapiFloatToNative(test.format, input, 4, encoded, test.size, 4), "PCM reference encode failed");
        Require(std::memcmp(encoded, test.bytes, test.size) == 0, "PCM reference bytes differ");
    }
    const unsigned char padded[] = {0xff,0xff,0xff,0x7f, 0xff,0,0,0x80};
    float output[2]{};
    Require(WasapiNativeToFloat(Format(32,24), padded, 8, output, 2, 2) &&
        output[0] == 8388607.0f / 8388608.0f && output[1] == -1, "decode ignores low padding bits");
    const float ties[] = {0.5f / 32768, -0.5f / 32768};
    unsigned char rounded[4]{};
    Require(WasapiFloatToNative(Format(16), ties, 2, rounded, 4, 2) && rounded[0] == 1 && rounded[1] == 0 &&
        rounded[2] == 0xff && rounded[3] == 0xff, "round nearest ties away from zero");
}
static void FloatingAndNonfinite() {
    const unsigned char ieee32[] = {0,0,0x80,0xbf, 0,0,0,0, 0,0,0,0x3f};
    const unsigned char ieee64[] = {0,0,0,0,0,0,0xf0,0xbf, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0xe0,0x3f};
    const float reference[] = {-1, 0, 0.5f};
    for (unsigned bits : {32u, 64u}) {
        const auto f = Format(bits, bits, 1, WasapiSampleEncoding::IeeeFloat);
        const auto* bytes = bits == 32 ? ieee32 : ieee64;
        const std::size_t size = bits == 32 ? sizeof(ieee32) : sizeof(ieee64);
        float decoded[3]{};
        unsigned char encoded[24]{};
        Require(WasapiNativeToFloat(f, bytes, size, decoded, 3, 3) &&
            decoded[0] == -1 && decoded[1] == 0 && decoded[2] == 0.5f, "IEEE reference bytes decode");
        Require(WasapiFloatToNative(f, reference, 3, encoded, size, 3) &&
            std::memcmp(encoded, bytes, size) == 0, "IEEE reference bytes encode");
    }
    const float nonfinite[] = {std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -2, 2};
    const unsigned char expected[] = {0x80,0x80,0x80,0,0xff};
    unsigned char pcm[5]{};
    Require(WasapiFloatToNative(Format(8), nonfinite, 5, pcm, 5, 5) &&
        std::memcmp(pcm, expected, 5) == 0, "nonfinite silence and integer clipping");
    for (unsigned bits : {32u, 64u}) {
        const auto f = Format(bits, bits, 1, WasapiSampleEncoding::IeeeFloat);
        unsigned char native[40]{};
        float decoded[5]{};
        Require(WasapiFloatToNative(f, nonfinite, 5, native, sizeof(native), 5) &&
            WasapiNativeToFloat(f, native, sizeof(native), decoded, 5, 5), "float conversion");
        Require(decoded[0] == 0 && decoded[1] == 0 && decoded[2] == 0 && decoded[3] == -2 && decoded[4] == 2,
            "finite float headroom retained and nonfinite sanitized");
    }
    const double source[] = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), (std::numeric_limits<double>::max)(), -(std::numeric_limits<double>::max)(), 0.25};
    float output[6]{};
    Require(WasapiNativeToFloat(Format(64,64,1,WasapiSampleEncoding::IeeeFloat), source, sizeof(source), output, 6, 6) &&
        output[0] == 0 && output[1] == 0 && output[2] == 0 && output[3] == (std::numeric_limits<float>::max)() &&
        output[4] == -(std::numeric_limits<float>::max)() && output[5] == 0.25f, "float64 narrowing stays finite");
    Require(WasapiNativeToFloat(Format(32,32,1,WasapiSampleEncoding::IeeeFloat), nonfinite, sizeof(nonfinite), output, 6, 5) &&
        output[0] == 0 && output[1] == 0 && output[2] == 0, "native float32 nonfinite sanitized");
}
static void BoundsAndNoAllocation() {
    const auto f = Format(16,16,2);
    const unsigned char native[] = {0x55, 0,0x80, 0,0x40, 0,0, 0xff,0x7f};
    unsigned char outputBytes[17]{};
    float* unaligned = reinterpret_cast<float*>(outputBytes + 1);
    unsigned char encoded[9]{};
    allocations = 0; tracking = true;
    const bool decoded = WasapiNativeToFloat(f, native + 1, 8, unaligned, 4, 2);
    const bool written = WasapiFloatToNative(f, unaligned, 4, encoded + 1, 8, 2);
    const bool silent = WasapiNativeToFloat(f, nullptr, 0, unaligned, 4, 2, true);
    tracking = false;
    Require(decoded && written && silent && allocations == 0, "unaligned multichannel conversion no allocation");
    Require(std::memcmp(encoded + 1, native + 1, 8) == 0, "unaligned channel order preserved");
    for (unsigned i = 1; i < sizeof(outputBytes); ++i) Require(outputBytes[i] == 0, "silent native packet fills zeros");
    float sentinel[4] = {9,9,9,9};
    allocations = 0; tracking = true;
    const bool rejected = !WasapiNativeToFloat(f, native, 7, sentinel, 4, 2) &&
        !WasapiNativeToFloat(f, native, 8, sentinel, 3, 2) && !WasapiNativeToFloat(f, nullptr, 8, sentinel, 4, 2) &&
        !WasapiNativeToFloat(f, native, 8, nullptr, 4, 2) &&
        !WasapiNativeToFloat(f, native, 8, sentinel, 4, (std::numeric_limits<std::size_t>::max)()) &&
        !WasapiFloatToNative(f, sentinel, 3, encoded, 8, 2) && !WasapiFloatToNative(f, sentinel, 4, encoded, 7, 2) &&
        !WasapiFloatToNative(f, nullptr, 4, encoded, 8, 2) && !WasapiFloatToNative(f, sentinel, 4, nullptr, 8, 2);
    const bool empty = WasapiNativeToFloat(f, nullptr, 0, nullptr, 0, 0) && WasapiFloatToNative(f, nullptr, 0, nullptr, 0, 0);
    tracking = false;
    Require(rejected && empty && allocations == 0 && sentinel[0] == 9 && sentinel[3] == 9, "invalid sizes/null no writes or allocation");
    unsigned char before[sizeof(encoded)];
    std::memcpy(before, encoded, sizeof(encoded));
    const auto large = Format(64,64,256,WasapiSampleEncoding::IeeeFloat);
    Require(!WasapiFloatToNative(large, sentinel, (std::numeric_limits<std::size_t>::max)(), encoded,
        (std::numeric_limits<std::size_t>::max)(), 384000) &&
        !WasapiNativeToFloat(large, native, (std::numeric_limits<std::size_t>::max)(), sentinel,
            (std::numeric_limits<std::size_t>::max)(), 384000), "64 MiB bound checked before buffers are accessed");
    Require(!WasapiFloatToNative(f, sentinel, 4, encoded, sizeof(encoded), 384001) &&
        std::memcmp(before, encoded, sizeof(encoded)) == 0, "frame limit and invalid conversion preserve native destination");
    auto invalid = f;
    invalid.validBits = 0; Require(!IsSupportedWasapiSampleFormat(invalid), "zero valid bits");
    invalid = f; invalid.channels = 257; Require(!IsSupportedWasapiSampleFormat(invalid), "too many channels");
    invalid = f; invalid.blockAlign = 3; Require(!IsSupportedWasapiSampleFormat(invalid), "bad block alignment");
    invalid = f; invalid.sampleRate = 7999; Require(!IsSupportedWasapiSampleFormat(invalid), "rate too low");
    invalid = f; invalid.sampleRate = 384001; Require(!IsSupportedWasapiSampleFormat(invalid), "rate too high");
    invalid = f; invalid.validBits = 17; Require(!IsSupportedWasapiSampleFormat(invalid), "invalid bit shift");
    invalid = f; invalid.encoding = static_cast<WasapiSampleEncoding>(99); Require(!IsSupportedWasapiSampleFormat(invalid), "unknown encoding");
    invalid = Format(32,24,1,WasapiSampleEncoding::IeeeFloat); Require(!IsSupportedWasapiSampleFormat(invalid), "partial float bits");
    invalid = Format(12); Require(!IsSupportedWasapiSampleFormat(invalid), "packed nonbyte PCM");
}
static_assert(noexcept(WasapiNativeToFloat(WasapiMixFormat{}, nullptr, 0, nullptr, 0, 0)));
static_assert(noexcept(WasapiFloatToNative(WasapiMixFormat{}, nullptr, 0, nullptr, 0, 0)));
int main() {
    try { IntegerReferences(); FloatingAndNonfinite(); BoundsAndNoAllocation();
        std::puts("WASAPI sample conversion tests passed"); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
