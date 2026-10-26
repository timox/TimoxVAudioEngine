#pragma once
#include "native_audio_protocol.h"


namespace timox_native {
using packet_size = decltype(sizeof(0));
static_assert(sizeof(timox_packet_u64)==8 && sizeof(timox_packet_u32)==4 && sizeof(timox_packet_u16)==2, "wire integer widths" );
enum class PacketError {
    None, InvalidContext, Truncated, Header, Version, Direction, Generation,
    Format, Frames, Payload, Flags, Reserved
};
struct PacketContext {
    timox_packet_u64 generation;
    timox_packet_u16 direction;
    timox_packet_u32 capacityFrames;
};
struct PacketResult {
    PacketError error;
    TimoxNativePacketMetadata metadata;
};

namespace detail {
inline timox_packet_u64 ReadLittleEndian(const unsigned char* bytes, packet_size offset, unsigned width) noexcept {
    timox_packet_u64 value = 0;
    for (unsigned index = 0; index < width; ++index)
        value |= static_cast<timox_packet_u64>(bytes[offset + index]) << (index * 8);
    return value;
}
}

// Validate a stable, readable private buffer (for example an IOCTL system
// buffer), never mutable user memory. Does not probe pointers or establish
// caller identity. No allocation, OS call, payload scan or mutable state.
inline PacketResult ValidatePacket(const void* packet, packet_size bytes, const PacketContext& context) noexcept {
    if (!context.generation || !context.capacityFrames || context.capacityFrames > TIMOX_NATIVE_MAX_FRAMES ||
        (context.direction != TIMOX_NATIVE_FROM_WINDOWS_RENDER && context.direction != TIMOX_NATIVE_TO_WINDOWS_CAPTURE))
        return {PacketError::InvalidContext, {}};
    if (!packet || bytes < TIMOX_NATIVE_HEADER_BYTES) return {PacketError::Truncated, {}};
    const auto* data = static_cast<const unsigned char*>(packet);
    const auto read = [data](packet_size offset, unsigned width) noexcept {
        return detail::ReadLittleEndian(data, offset, width);
    };
    if (read(0, 4) != 0x41575654u || read(4, 4) != TIMOX_NATIVE_HEADER_BYTES)
        return {PacketError::Header, {}};
    if (read(8, 2) != 1 || read(10, 2) != 0) return {PacketError::Version, {}};
    const auto opcode = read(12, 2);
    const auto direction = read(14, 2);
    if (direction != context.direction ||
        (direction == TIMOX_NATIVE_FROM_WINDOWS_RENDER && opcode != TIMOX_NATIVE_RENDER_PACKET) ||
        (direction == TIMOX_NATIVE_TO_WINDOWS_CAPTURE && opcode != TIMOX_NATIVE_CAPTURE_PACKET))
        return {PacketError::Direction, {}};
    const auto generation = read(16, 8);
    if (generation != context.generation) return {PacketError::Generation, {}};
    if (read(44, 4) != TIMOX_NATIVE_SAMPLE_RATE || read(48, 2) != TIMOX_NATIVE_CHANNELS ||
        read(50, 2) != TIMOX_NATIVE_PCM_S32) return {PacketError::Format, {}};
    const auto frames = read(40, 4);
    if (!frames || frames > context.capacityFrames) return {PacketError::Frames, {}};
    // Format and frame bounds precede multiplication. Product <= 32768;
    // use 64 bits anyway and subtract header from already-checked host length.
    const auto payload = read(52, 4);
    if (payload != frames * TIMOX_NATIVE_CHANNELS * 4u || bytes - TIMOX_NATIVE_HEADER_BYTES != payload)
        return {PacketError::Payload, {}};
    const auto flags = read(56, 4);
    const auto qpc = read(32, 8);
    if ((flags & ~(timox_packet_u64{TIMOX_NATIVE_DISCONTINUITY} | TIMOX_NATIVE_QPC_VALID)) ||
        (!(flags & TIMOX_NATIVE_QPC_VALID) && qpc != 0)) return {PacketError::Flags, {}};
    if (read(60, 4) != 0) return {PacketError::Reserved, {}};
    return {PacketError::None, {generation, read(24, 8), qpc, static_cast<timox_packet_u32>(frames),
        static_cast<timox_packet_u32>(payload), static_cast<timox_packet_u32>(flags), static_cast<timox_packet_u16>(direction)}};
}
}
