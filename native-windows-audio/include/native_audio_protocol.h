#ifndef TIMOX_NATIVE_AUDIO_PROTOCOL_H
#define TIMOX_NATIVE_AUDIO_PROTOCOL_H

#if defined(_KERNEL_MODE)
/* Fundamental fixed-width types avoid user CRT headers in WDK builds. */
typedef unsigned long long timox_packet_u64;
typedef unsigned int timox_packet_u32;
typedef unsigned short timox_packet_u16;
#else
#include <stdint.h>
typedef uint64_t timox_packet_u64;
typedef uint32_t timox_packet_u32;
typedef uint16_t timox_packet_u16;
#endif

/* Packet wire format is little-endian, 64-byte header followed by interleaved
 * signed PCM32. These are offsets, not an ABI-dependent packed C structure. */
#define TIMOX_NATIVE_HEADER_BYTES 64u
#define TIMOX_NATIVE_MAX_FRAMES 4096u
#define TIMOX_NATIVE_SAMPLE_RATE 48000u
#define TIMOX_NATIVE_CHANNELS 2u
#define TIMOX_NATIVE_PCM_S32 1u
#define TIMOX_NATIVE_RENDER_PACKET 1u
#define TIMOX_NATIVE_CAPTURE_PACKET 2u
#define TIMOX_NATIVE_FROM_WINDOWS_RENDER 1u
#define TIMOX_NATIVE_TO_WINDOWS_CAPTURE 2u
#define TIMOX_NATIVE_DISCONTINUITY 1u
#define TIMOX_NATIVE_QPC_VALID 2u

/* Decoded metadata only; NEVER cast a packet buffer to this structure. */
typedef struct TimoxNativePacketMetadata {
    timox_packet_u64 generation;
    timox_packet_u64 sequence;
    timox_packet_u64 qpc;
    timox_packet_u32 frames;
    timox_packet_u32 payloadBytes;
    timox_packet_u32 flags;
    timox_packet_u16 direction;
} TimoxNativePacketMetadata;

#endif
