#pragma once

#include "wasapi_device_enumerator.h"

// Pure format validation; no Windows/COM calls, allocation or exceptions.
bool IsSupportedWasapiSampleFormat(const WasapiMixFormat& format) noexcept;

// Convert interleaved native little-endian WASAPI data to/from interleaved float.
// Supports unsigned PCM8, signed PCM16/24/32, left-aligned extensible valid bits,
// and IEEE float32/64. Native and float buffers may be unaligned. Source and
// destination ranges MUST NOT overlap; caller owns their full stated lengths.
//
// NaN and either infinity become silence. Finite floating-point amplitudes are
// preserved outside [-1,1]; float64 exceeding float32 range saturates to FLT_MAX.
// Integer encoding saturates [-1,1], rounds to nearest with ties away from zero,
// and clears unused low bits. Integer decoding ignores unused low bits.
//
// Return false before writing anything on invalid format, pointers or lengths.
// Max 384000 frames and 64 MiB per required native/float buffer. Zero frames
// allow null pointers. No allocation, locks, logging, COM, SRC or exceptions.
// Silent capture permits null native data and emits float zeros; nativeBytes
// is ignored in that case. Encoding float zero yields PCM8 midpoint 0x80.
bool WasapiNativeToFloat(const WasapiMixFormat& format,
    const void* native, std::size_t nativeBytes,
    float* output, std::size_t outputSamples, std::size_t frames,
    bool silent = false) noexcept;

bool WasapiFloatToNative(const WasapiMixFormat& format,
    const float* input, std::size_t inputSamples,
    void* native, std::size_t nativeBytes, std::size_t frames) noexcept;
