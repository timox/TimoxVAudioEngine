#pragma once

#include "asiosys.h"
#include "asio.h"

#include <cstdint>

namespace AsioSampleConversion {
using DecodeBlockFn = void (*)(float* dst, const std::uint8_t* src, std::uint32_t frames) noexcept;
using EncodeBlockFn = void (*)(std::uint8_t* dst, const float* src, std::uint32_t frames) noexcept;
// Unsupported types return nullptr. Buffers may be unaligned; zero frames touches neither buffer.
DecodeBlockFn SelectDecoder(ASIOSampleType type) noexcept;
EncodeBlockFn SelectEncoder(ASIOSampleType type) noexcept;
bool IsSupported(ASIOSampleType type) noexcept;
std::uint32_t BytesPerSample(ASIOSampleType type) noexcept;
float Decode(const void* bytes, ASIOSampleType type) noexcept;
void Encode(void* bytes, ASIOSampleType type, float sample) noexcept;
}
