#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// One MIDI 1.0 port of an endpoint, as MIDI 1.0 applications (WinMM, WinRT
// MIDI 1.0) see it. Windows MIDI Services maps each MIDI 1.0 port to one UMP
// group of its endpoint.
struct Midi1PortInfo {
    std::uint8_t group = 0;    // 0-based
    std::string name;
    bool source = false;       // the engine can receive from it (MIDI In)
    bool destination = false;  // the engine can send to it (MIDI Out)
};

struct MidiEndpointInfo {
    std::string endpointId;   // OS endpoint device id
    std::string name;
    std::string transport;    // informational
    std::vector<Midi1PortInfo> ports;
};

// Platform MIDI access used by MidiPatchbay. Every connection is identified by
// a caller-chosen cookie so the receive path needs no string lookup.
// Implementations must be thread-safe: Send() and the receive handler run on
// arbitrary threads while control calls run on the API thread.
// Direction of an application port loopback.
enum class MidiVirtualDirection {
    ToEngine,    // "<name> In": applications send, the engine only reads
    FromEngine,  // "<name> Out": the engine only writes, applications read
};

class MidiBackend {
public:
    using ReceiveHandler = std::function<void(std::uint64_t cookie, const std::uint32_t* words,
        std::size_t count)>;

    virtual ~MidiBackend() = default;
    virtual std::string Name() const = 0;

    // Set once, before any endpoint is opened.
    virtual void SetReceiveHandler(ReceiveHandler handler) = 0;

    // Lists native MIDI 1.0 endpoints and their MIDI 1.0 ports, excluding MIDI
    // 2.0 endpoints and the client side of virtual ports this backend created.
    virtual bool Enumerate(std::vector<MidiEndpointInfo>& endpoints, std::string& error) = 0;

    virtual bool OpenEndpoint(std::uint64_t cookie, const std::string& endpointId, std::string& error) = 0;

    // Creates one plain MIDI 1.0 port (never MIDI 2.0) for an application:
    // "<displayName> In" (ToEngine) or "<displayName> Out" (FromEngine).
    // stableId stays the same across engine restarts so applications find the
    // same port again. A ToEngine port delivers what applications send through
    // the receive handler with this cookie and refuses Send(); a FromEngine
    // port delivers Send() to applications and is never read.
    virtual bool CreateVirtualPort(std::uint64_t cookie, const std::string& stableId,
        const std::string& displayName, MidiVirtualDirection direction, std::string& endpointId,
        std::string& error) = 0;

    // Closes an endpoint connection or destroys a virtual port.
    virtual void Close(std::uint64_t cookie) noexcept = 0;

    // words is one UMP message whose group already selects the MIDI 1.0 port.
    virtual bool Send(std::uint64_t cookie, const std::uint32_t* words, std::size_t count) noexcept = 0;

    // Sends at a host time (std::chrono::steady_clock, ns). Windows MIDI
    // Services holds messages with a future timestamp and sends them on time;
    // this is how clock ticks follow the sound card instead of thread wake-ups.
    // A time in the past sends immediately.
    virtual bool SendAt(std::uint64_t cookie, const std::uint32_t* words, std::size_t count,
                        std::int64_t /*hostNs*/) noexcept {
        return Send(cookie, words, count);
    }
};
