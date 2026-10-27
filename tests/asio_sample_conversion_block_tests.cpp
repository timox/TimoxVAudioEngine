#include "asio_sample_conversion.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {
constexpr ASIOSampleType types[] = {
    ASIOSTInt16LSB, ASIOSTInt24LSB, ASIOSTInt32LSB,
    ASIOSTInt32LSB16, ASIOSTInt32LSB18, ASIOSTInt32LSB20, ASIOSTInt32LSB24,
    ASIOSTFloat32LSB, ASIOSTFloat64LSB,
    ASIOSTInt16MSB, ASIOSTInt24MSB, ASIOSTInt32MSB,
    ASIOSTInt32MSB16, ASIOSTInt32MSB18, ASIOSTInt32MSB20, ASIOSTInt32MSB24,
    ASIOSTFloat32MSB, ASIOSTFloat64MSB
};

std::uint32_t randomBits(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

bool checkDecode(ASIOSampleType type, const std::vector<std::uint8_t>& bytes, std::uint32_t count) {
    const auto stride = AsioSampleConversion::BytesPerSample(type);
    const auto decode = AsioSampleConversion::SelectDecoder(type);
    if (!decode) return false;
    std::vector<float> actual(count + 2, 123.0f);
    decode(actual.data() + 1, bytes.data() + 1, count);
    if (actual.front() != 123.0f || actual.back() != 123.0f) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const float expected = AsioSampleConversion::Decode(bytes.data() + 1 + i * stride, type);
        if (std::memcmp(&expected, &actual[i + 1], sizeof(float)) != 0) {
            std::fprintf(stderr, "decode mismatch: type=%ld index=%u\n", type, i);
            return false;
        }
    }
    return true;
}

bool checkEncode(ASIOSampleType type, const std::vector<float>& samples) {
    const auto stride = AsioSampleConversion::BytesPerSample(type);
    std::vector<std::uint8_t> actual(samples.size() * stride + 2, 0xa5);
    auto expected = actual;
    for (std::size_t i = 0; i < samples.size(); ++i)
        AsioSampleConversion::Encode(expected.data() + 1 + i * stride, type, samples[i]);
    AsioSampleConversion::SelectEncoder(type)(actual.data() + 1, samples.data(),
        static_cast<std::uint32_t>(samples.size()));
    if (actual != expected) {
        std::fprintf(stderr, "encode mismatch: type=%ld\n", type);
        return false;
    }
    return true;
}

// Informational CPU benchmark, never a timing-based test gate. Same eight-channel
// 256-frame workload for scalar and block conversion; both outputs are consumed.
void benchmark(ASIOSampleType type) {
    constexpr std::uint32_t frames = 256;
    constexpr int channels = 8, rounds = 1000;
    const auto stride = AsioSampleConversion::BytesPerSample(type);
    std::vector<float> samples(frames * channels);
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = float(i % 127) / 64.0f - 1.0f;
    std::vector<std::uint8_t> bytes(samples.size() * stride);
    std::vector<float> decoded(samples.size());
    const auto encode = AsioSampleConversion::SelectEncoder(type);
    const auto decode = AsioSampleConversion::SelectDecoder(type);
    volatile float sink = 0.0f;
    const auto run = [&](bool block) {
        const auto begin = std::chrono::steady_clock::now();
        for (int round = 0; round < rounds; ++round) {
            for (int c = 0; c < channels; ++c) {
                auto* dst = bytes.data() + c * frames * stride;
                const auto* src = samples.data() + c * frames;
                auto* output = decoded.data() + c * frames;
                if (block) {
                    encode(dst, src, frames);
                    decode(output, dst, frames);
                } else {
                    for (std::uint32_t f = 0; f < frames; ++f)
                        AsioSampleConversion::Encode(dst + f * stride, type, src[f]);
                    for (std::uint32_t f = 0; f < frames; ++f)
                        output[f] = AsioSampleConversion::Decode(dst + f * stride, type);
                }
            }
            sink = decoded[round % decoded.size()];
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    };
    run(false); run(true); // Warm both paths.
    const double scalarMs = run(false), blockMs = run(true);
    std::printf("CPU type=%ld: scalar=%.3f ms block=%.3f ms ratio=%.2fx (8 x 256 x %d)\n",
        type, scalarMs, blockMs, scalarMs / blockMs, rounds);
    (void)sink;
}
}

int main() {
    // For every odd integer maxValue, +/-0.5f * maxValue is an exact half-integer.
    std::vector<float> samples{0.0f, -0.0f, 1.0f, -1.0f, 1.5f, -1.5f, 0.5f, -0.5f,
        std::nextafter(0.5f, 0.0f), std::nextafter(0.5f, 1.0f),
        std::nextafter(-0.5f, 0.0f), std::nextafter(-0.5f, -1.0f),
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::denorm_min()};
    for (const int bits : {16, 18, 20, 24, 32}) {
        const double maxValue = std::ldexp(1.0, bits - 1) - 1;
        for (int k = -100; k <= 100; ++k) {
            const float half = static_cast<float>((k + 0.5) / maxValue);
            samples.push_back(half);
            samples.push_back(std::nextafter(half, -std::numeric_limits<float>::infinity()));
            samples.push_back(std::nextafter(half, std::numeric_limits<float>::infinity()));
        }
    }
    for (int i = 0; i <= 131072; ++i) samples.push_back(-1.5f + 3.0f * i / 131072.0f);
    std::uint32_t seed = 0x12345678;
    for (int i = 0; i < 1000000; ++i) {
        const auto bits = randomBits(seed);
        float sample;
        std::memcpy(&sample, &bits, sizeof(sample));
        samples.push_back(sample);
    }
    for (const auto type : types) {
        const auto decode = AsioSampleConversion::SelectDecoder(type);
        const auto encode = AsioSampleConversion::SelectEncoder(type);
        if (!decode || !encode) {
            std::fprintf(stderr, "Missing block converter for supported type %ld\n", type);
            return 1;
        }
        decode(nullptr, nullptr, 0);
        encode(nullptr, nullptr, 0);
        const auto stride = AsioSampleConversion::BytesPerSample(type);
        constexpr std::uint32_t count = 1000000 + 65536;
        std::vector<std::uint8_t> bytes(count * stride + 2, 0xa5);
        // First 65536 samples exhaust every 16-bit encoding; for wider formats,
        // this is a raw-byte ramp (including padding bits of reduced-width PCM).
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint64_t value = i < 65536 ? i :
                (static_cast<std::uint64_t>(randomBits(seed)) << 32) | randomBits(seed);
            for (std::uint32_t b = 0; b < stride; ++b)
                bytes[1 + i * stride + b] = static_cast<std::uint8_t>(value >> (8 * b));
        }
        const auto original = bytes;
        for (const auto size : {1u, 7u, 255u, 256u, 257u, count})
            if (!checkDecode(type, bytes, size)) return 2;
        if (bytes != original || !checkEncode(type, samples)) return 3;
        for (const std::size_t size : {0u, 1u, 7u, 255u, 256u, 257u})
            if (!checkEncode(type, std::vector<float>(samples.begin(), samples.begin() + size))) return 4;
    }
    for (const ASIOSampleType type : std::array<ASIOSampleType, 2>{ASIOSTDSDInt8LSB1, -1})
        if (AsioSampleConversion::SelectDecoder(type) || AsioSampleConversion::SelectEncoder(type)) return 5;
    std::puts("PASS: 18 formats bit-exact against scalar conversion, exhaustive 16-bit and million-sample cases.");
    for (const auto type : {ASIOSTInt16LSB, ASIOSTInt24LSB, ASIOSTInt32LSB, ASIOSTInt32LSB24, ASIOSTFloat32LSB})
        benchmark(type);
    return 0;
}
