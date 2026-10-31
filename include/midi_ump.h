#pragma once

// Universal MIDI Packet (UMP) helpers used by the MIDI patchbay.
// Windows MIDI Services delivers every message as UMP words, including MIDI 1.0
// traffic from legacy ports, so routing works on UMP only.

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
    const auto type = Type(word0);
    return type == 0x2 || type == 0x4;
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

// Note number lives in bits 8..15 of word 0 for MIDI 1.0 note/poly-pressure
// messages and for MIDI 2.0 per-note messages.
inline bool CarriesNoteNumber(std::uint32_t word0) noexcept {
    const auto status = Status(word0);
    if (Type(word0) == 0x2) return status == 0x8 || status == 0x9 || status == 0xA;
    if (Type(word0) == 0x4)
        return status == 0x0 || status == 0x1 || status == 0x6 || status == 0x8 ||
               status == 0x9 || status == 0xA || status == 0xF;
    return false;
}

inline std::uint8_t NoteNumber(std::uint32_t word0) noexcept {
    return static_cast<std::uint8_t>((word0 >> 8) & 0x7F);
}

inline std::uint32_t WithNoteNumber(std::uint32_t word0, std::uint8_t note) noexcept {
    return (word0 & ~0x0000FF00u) | (static_cast<std::uint32_t>(note & 0x7F) << 8);
}

// Message categories a patchbay route can filter on.
enum MessageCategory : std::uint32_t {
    Notes = 1u << 0,           // note on / note off
    PolyPressure = 1u << 1,
    ControlChange = 1u << 2,   // CC, RPN/NRPN and MIDI 2.0 per-note controllers
    ProgramChange = 1u << 3,
    ChannelPressure = 1u << 4,
    PitchBend = 1u << 5,       // includes MIDI 2.0 per-note pitch bend
    SystemRealtime = 1u << 6,  // clock, start, continue, stop, active sensing, reset
    SystemCommon = 1u << 7,    // MTC quarter frame, song position, song select, tune request
    SysEx = 1u << 8,
    FlexData = 1u << 9,        // MIDI 2.0 flex data (tempo, metadata, ...)
    AllCategories = (1u << 10) - 1,
};

// Returns 0 for messages that are never routed: utility (jitter-reduction
// timestamps), endpoint stream configuration and reserved types. These belong
// to a single connection and must not cross the patchbay.
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
    case 0x4:
        switch (Status(word0)) {
        case 0x8: case 0x9: return Notes;
        case 0xA: return PolyPressure;
        case 0x0: case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: case 0xB: case 0xF:
            return ControlChange;
        case 0xC: return ProgramChange;
        case 0xD: return ChannelPressure;
        case 0x6: case 0xE: return PitchBend;
        default: return 0;
        }
    case 0x3: case 0x5: return SysEx;
    case 0xD: return FlexData;
    default: return 0;
    }
}

}  // namespace midi_ump
