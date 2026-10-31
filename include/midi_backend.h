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
class MidiBackend {
public:
    using ReceiveHandler = std::function<void(std::uint64_t cookie, const std::uint32_t* words,
        std::size_t count)>;

    virtual ~MidiBackend() = default;
    virtual std::string Name() const = 0;

    // Set once, before any endpoint is opened.
    virtual void SetReceiveHandler(ReceiveHandler handler) = 0;

    // Lists endpoints and their MIDI 1.0 ports, excluding the client side of
    // virtual ports this backend created.
    virtual bool Enumerate(std::vector<MidiEndpointInfo>& endpoints, std::string& error) = 0;

    virtual bool OpenEndpoint(std::uint64_t cookie, const std::string& endpointId, std::string& error) = 0;

    // Creates an app-to-app MIDI 1.0 port (one input and one output, group 1)
    // that other applications see under displayName. stableId stays the same across engine restarts so the OS
    // can give applications the same port again. Messages applications send
    // arrive through the receive handler with this cookie; Send() with this
    // cookie delivers to them.
    virtual bool CreateVirtualPort(std::uint64_t cookie, const std::string& stableId,
        const std::string& displayName, std::string& clientEndpointId, std::string& error) = 0;

    // Closes an endpoint connection or destroys a virtual port.
    virtual void Close(std::uint64_t cookie) noexcept = 0;

    // words is one UMP message whose group already selects the MIDI 1.0 port.
    virtual bool Send(std::uint64_t cookie, const std::uint32_t* words, std::size_t count) noexcept = 0;
};
