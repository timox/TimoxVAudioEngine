#include "wasapi_sample_conversion.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr std::size_t MaximumBytes = 64u * 1024u * 1024u;
constexpr std::size_t MaximumFrames = 384000;
static_assert(sizeof(float) == 4 && sizeof(double) == 8 &&
    std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559,
    "WASAPI sample conversion requires IEEE binary32 and binary64");

bool Dimensions(const WasapiMixFormat& format, std::size_t frames,
    std::size_t& samples, std::size_t& bytes) noexcept {
    if (!IsSupportedWasapiSampleFormat(format) || frames > MaximumFrames) return false;
    samples = frames * format.channels; // Bounded above before multiplication.
    const auto width = format.containerBits / 8;
    if (samples > MaximumBytes / sizeof(float) || samples > MaximumBytes / width) return false;
    bytes = samples * width;
    return true;
}
std::uint64_t ReadLittleEndian(const unsigned char* data, unsigned width) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= std::uint64_t{data[i]} << (8 * i);
    return value;
}
void WriteLittleEndian(unsigned char* data, unsigned width, std::uint64_t value) noexcept {
    for (unsigned i = 0; i < width; ++i) data[i] = static_cast<unsigned char>(value >> (8 * i));
}
float NarrowFinite(double value) noexcept {
    if (!std::isfinite(value)) return 0.0f;
    const auto limit = static_cast<double>((std::numeric_limits<float>::max)());
    return static_cast<float>((std::max)(-limit, (std::min)(limit, value)));
}
float Decode(const WasapiMixFormat& format, const unsigned char* data) noexcept {
    const auto bits = ReadLittleEndian(data, format.containerBits / 8);
    if (format.encoding == WasapiSampleEncoding::IeeeFloat) {
        if (format.containerBits == 32) {
            const auto raw = static_cast<std::uint32_t>(bits);
            float value;
            std::memcpy(&value, &raw, sizeof(value));
            return std::isfinite(value) ? value : 0.0f;
        }
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return NarrowFinite(value);
    }
    const auto raw = bits >> (format.containerBits - format.validBits);
    // uint64 shifts are safe even for validBits==32; no signed shift or
    // implementation-defined conversion from an out-of-range unsigned value.
    const auto midpoint = std::uint64_t{1} << (format.validBits - 1);
    const auto modulus = std::uint64_t{1} << format.validBits;
    const std::int64_t signedValue = format.containerBits == 8
        ? static_cast<std::int64_t>(raw) - static_cast<std::int64_t>(midpoint)
        : raw >= midpoint ? static_cast<std::int64_t>(raw) - static_cast<std::int64_t>(modulus)
                          : static_cast<std::int64_t>(raw);
    return static_cast<float>(static_cast<double>(signedValue) / static_cast<double>(midpoint));
}
void Encode(const WasapiMixFormat& format, float value, unsigned char* data) noexcept {
    if (!std::isfinite(value)) value = 0.0f;
    if (format.encoding == WasapiSampleEncoding::IeeeFloat) {
        if (format.containerBits == 32) {
            std::uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            WriteLittleEndian(data, 4, bits);
        } else {
            const double wide = value;
            std::uint64_t bits;
            std::memcpy(&bits, &wide, sizeof(bits));
            WriteLittleEndian(data, 8, bits);
        }
        return;
    }
    const auto midpoint = std::int64_t{1} << (format.validBits - 1);
    const auto modulus = std::int64_t{1} << format.validBits;
    const double clipped = (std::max)(-1.0, (std::min)(1.0, static_cast<double>(value)));
    const double rounded = std::round(clipped * static_cast<double>(midpoint));
    // The bounded rounded result lies within int64, including positive 2^31.
    const auto quantized = (std::max)(-midpoint, (std::min)(midpoint - 1, static_cast<std::int64_t>(rounded)));
    const auto raw = static_cast<std::uint64_t>(format.containerBits == 8
        ? quantized + midpoint : quantized < 0 ? quantized + modulus : quantized);
    WriteLittleEndian(data, format.containerBits / 8, raw << (format.containerBits - format.validBits));
}
}

bool IsSupportedWasapiSampleFormat(const WasapiMixFormat& format) noexcept {
    if (!format.channels || format.channels > 256 || format.sampleRate < 8000 || format.sampleRate > 384000 ||
        !format.validBits || format.validBits > format.containerBits) return false;
    if (format.encoding == WasapiSampleEncoding::PcmInteger) {
        if (format.containerBits != 8 && format.containerBits != 16 &&
            format.containerBits != 24 && format.containerBits != 32) return false;
    } else if (format.encoding == WasapiSampleEncoding::IeeeFloat) {
        if ((format.containerBits != 32 && format.containerBits != 64) || format.validBits != format.containerBits) return false;
    } else return false;
    return format.blockAlign == format.channels * (format.containerBits / 8);
}

bool WasapiNativeToFloat(const WasapiMixFormat& format, const void* native, std::size_t nativeBytes,
    float* output, std::size_t outputSamples, std::size_t frames, bool silent) noexcept {
    std::size_t samples = 0, required = 0;
    if (!Dimensions(format, frames, samples, required) || outputSamples < samples ||
        (!silent && nativeBytes < required) || (samples && (!output || (!silent && !native)))) return false;
    const auto* input = static_cast<const unsigned char*>(native);
    auto* destination = reinterpret_cast<unsigned char*>(output);
    const auto width = format.containerBits / 8;
    for (std::size_t sample = 0; sample < samples; ++sample) {
        const float value = silent ? 0.0f : Decode(format, input + sample * width);
        std::memcpy(destination + sample * sizeof(float), &value, sizeof(value));
    }
    return true;
}

bool WasapiFloatToNative(const WasapiMixFormat& format, const float* input, std::size_t inputSamples,
    void* native, std::size_t nativeBytes, std::size_t frames) noexcept {
    std::size_t samples = 0, required = 0;
    if (!Dimensions(format, frames, samples, required) || inputSamples < samples || nativeBytes < required ||
        (samples && (!input || !native))) return false;
    const auto* source = reinterpret_cast<const unsigned char*>(input);
    auto* destination = static_cast<unsigned char*>(native);
    const auto width = format.containerBits / 8;
    for (std::size_t sample = 0; sample < samples; ++sample) {
        float value;
        std::memcpy(&value, source + sample * sizeof(float), sizeof(value));
        Encode(format, value, destination + sample * width);
    }
    return true;
}
