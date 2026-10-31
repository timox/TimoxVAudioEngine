#pragma once

// Universal MIDI Packet (UMP) helpers used by the MIDI 1.0 patchbay.
// Windows MIDI Services delivers every message as UMP words, including MIDI 1.0
// traffic from MIDI 1.0 ports: a MIDI 1.0 port is one group of an endpoint, and
// its messages are UMP types 1 (system), 2 (channel voice) and 3 (SysEx 7-bit).

#include <cstddef>
#include <cstdint>

namespace midi_ump {

enum class MessageType : std::uint8_t {
    Utility = 0x0,
    System = 0x1,
    Midi1ChannelVoice = 0x2,
    Data64 = 0x3,          // SysEx 7-bit
    Midi2ChannelVoice = 0x4,
    Data128 = 0x5,         // SysEx 8-bit and mixed data sets
    FlexData = 0xD,
    Stream = 0xF,
};

inline std::uint8_t Type(std::uint32_t word0) noexcept {
    return static_cast<std::uint8_t>(word0 >> 28);
}

// Number of 32-bit words in a UMP message, from its message type.
inline std::size_t WordCount(std::uint32_t word0) noexcept {
    static constexpr std::uint8_t kCounts[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    return kCounts[Type(word0)];
}

inline bool HasGroup(std::uint32_t word0) noexcept {
    const auto type = Type(word0);
    return type != 0x0 && type != 0xF;
}

inline std::uint8_t Group(std::uint32_t word0) noexcept {
    return static_cast<std::uint8_t>((word0 >> 24) & 0x0F);
}

inline std::uint32_t WithGroup(std::uint32_t word0, std::uint8_t group) noexcept {
    return (word0 & ~0x0F000000u) | (static_cast<std::uint32_t>(group & 0x0F) << 24);
}

inline bool IsChannelVoice(std::uint32_t word0) noexcept {
    return Type(word0) == 0x2;
}

inline std::uint8_t Status(std::uint32_t word0) noexcept {
    return static_cast<std::uint8_t>((word0 >> 20) & 0x0F);
}

inline std::uint8_t Channel(std::uint32_t word0) noexcept {
    return static_cast<std::uint8_t>((word0 >> 16) & 0x0F);
}

inline std::uint32_t WithChannel(std::uint32_t word0, std::uint8_t channel) noexcept {
    return (word0 & ~0x000F0000u) | (static_cast<std::uint32_t>(channel & 0x0F) << 16);
}

// Note number lives in bits 8..15 of word 0 for MIDI 1.0 note and
// poly-pressure messages.
inline bool CarriesNoteNumber(std::uint32_t word0) noexcept {
    const auto status = Status(word0);
    return Type(word0) == 0x2 && (status == 0x8 || status == 0x9 || status == 0xA);
}

inline std::uint8_t NoteNumber(std::uint32_t word0) noexcept {
    return static_cast<std::uint8_t>((word0 >> 8) & 0x7F);
}

inline std::uint32_t WithNoteNumber(std::uint32_t word0, std::uint8_t note) noexcept {
    return (word0 & ~0x0000FF00u) | (static_cast<std::uint32_t>(note & 0x7F) << 8);
}

// Message categories a patchbay route can filter on. The patchbay carries
// MIDI 1.0 only, so these are MIDI 1.0 message families.
enum MessageCategory : std::uint32_t {
    Notes = 1u << 0,           // note on / note off
    PolyPressure = 1u << 1,
    ControlChange = 1u << 2,
    ProgramChange = 1u << 3,
    ChannelPressure = 1u << 4,
    PitchBend = 1u << 5,
    SystemRealtime = 1u << 6,  // clock, start, continue, stop, active sensing, reset
    SystemCommon = 1u << 7,    // MTC quarter frame, song position, song select, tune request
    SysEx = 1u << 8,
    AllCategories = (1u << 9) - 1,
};

// Category of a MIDI 1.0 UMP message (types 1, 2 and 3). Returns 0 for every
// other type: utility (jitter-reduction timestamps), MIDI 2.0 messages,
// endpoint stream messages and reserved types are not MIDI 1.0 traffic.
inline std::uint32_t Category(std::uint32_t word0) noexcept {
    switch (Type(word0)) {
    case 0x1: {
        const auto status = static_cast<std::uint8_t>((word0 >> 16) & 0xFF);
        return status >= 0xF8 ? SystemRealtime : SystemCommon;
    }
    case 0x2:
        switch (Status(word0)) {
        case 0x8: case 0x9: return Notes;
        case 0xA: return PolyPressure;
        case 0xB: return ControlChange;
        case 0xC: return ProgramChange;
        case 0xD: return ChannelPressure;
        case 0xE: return PitchBend;
        default: return 0;
        }
    case 0x3: return SysEx;
    default: return 0;
    }
}

}  // namespace midi_ump
