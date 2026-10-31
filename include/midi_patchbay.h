#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "midi_backend.h"
#include "midi_routing_graph.h"

// MIDI 1.0 patchbay. Every port is a MIDI 1.0 port; port ids are:
//   midi:virtual:<id>                  application port created by the engine:
//                                      "<name> In" and/or "<name> Out" loopbacks
//   midi:port:<group 1-16>:<endpointId> MIDI 1.0 port of an OS endpoint (USB
//                                      interface, network, another application)
// One application port: a dedicated MIDI 1.0 "In" port (the application sends
// to the patchbay) and "Out" port (the patchbay sends to the application). Each
// direction is a separate loopback and can be turned on or off on its own.
struct MidiVirtualPortConfig {
    std::string id;      // [a-z0-9-], 1..24 characters
    std::string name;    // shown to applications, 1..24 characters
    bool input = true;   // create "<name> In"
    bool output = true;  // create "<name> Out"
};

struct MidiPatchbayConfiguration {
    std::vector<MidiVirtualPortConfig> virtualPorts;
    std::vector<MidiRoute> routes;
};

struct MidiPortStatus {
    std::string id;
    std::string name;
    std::string kind;              // "virtual" or "port"
    std::string endpointId;        // "port" only
    std::string endpointName;      // "port" only
    int group = 1;                 // 1-based UMP group carrying this MIDI 1.0 port
    bool source = false;           // usable as a route source (MIDI In)
    bool destination = false;      // usable as a route destination (MIDI Out)
    bool online = false;           // port present / virtual port created
    bool open = false;             // engine holds a connection
    // Traffic the engine observed on this port since the engine started.
    // received: MIDI 1.0 messages that arrived from the port (routed or not);
    // sent: messages the engine delivered to the port. 0 = never.
    std::uint64_t received = 0;
    std::uint64_t sent = 0;
    std::int64_t lastReceivedUnixMs = 0;
    std::int64_t lastSentUnixMs = 0;
    // What arrived from the port, split so clock ticks never look like playing:
    // activity = every MIDI 1.0 message except timing clock (F8) and active
    // sensing (FE); clock = F8 ticks with the measured tempo and transport
    // (FA start, FB continue, FC stop).
    std::uint64_t activity = 0;
    std::int64_t lastActivityUnixMs = 0;
    std::uint64_t clockTicks = 0;
    std::int64_t lastClockUnixMs = 0;
    bool clockRunning = false;     // a tick arrived less than 1 s ago
    double clockBpm = 0;           // 0 = not measured yet (needs 2 ticks)
    bool clockEchoSuspected = false;  // the engine also sends clock to this port
    std::string transport;         // "start", "continue", "stop" or empty
    std::int64_t transportUnixMs = 0;
    std::uint64_t activeSensing = 0;
    std::int64_t lastActiveSensingUnixMs = 0;
    std::string inputEndpointId;   // virtual ports: "<name> In", apps send here
    std::string outputEndpointId;  // virtual ports: "<name> Out", apps listen here
    std::string error;
};

struct MidiRouteStatus {
    std::string id;
    bool active = false;
    std::uint64_t forwarded = 0;
    std::uint64_t filtered = 0;
};

struct MidiPatchbaySnapshot {
    bool available = false;
    std::string backend;
    std::string error;
    MidiPatchbayConfiguration configuration;
    std::vector<MidiPortStatus> ports;
    std::vector<MidiRouteStatus> routes;
};

struct MidiApplyResult {
    bool success = false;
    std::string error;
};

namespace midi_patchbay_json {
// Strict decode of the API/persistence format; returns false with an error.
bool Decode(const std::string& text, MidiPatchbayConfiguration& configuration, std::string& error);
std::string Encode(const MidiPatchbayConfiguration& configuration);
}

class MidiPatchbay final {
public:
    static constexpr const char* kVirtualPrefix = "midi:virtual:";
    static constexpr const char* kPortPrefix = "midi:port:";

    // Parses midi:port:<group>:<endpointId>; group is returned 0-based.
    static bool ParsePortId(const std::string& portId, std::uint8_t& group, std::string& endpointId);
    static std::string PortId(std::uint8_t group, const std::string& endpointId);

    // backend may be null when MIDI is unavailable; unavailableReason explains why.
    MidiPatchbay(std::unique_ptr<MidiBackend> backend, std::filesystem::path storePath,
        std::string unavailableReason = {});
    ~MidiPatchbay();
    MidiPatchbay(const MidiPatchbay&) = delete;
    MidiPatchbay& operator=(const MidiPatchbay&) = delete;

    // Loads the persisted configuration and applies it. Failures are reported
    // but leave the patchbay usable.
    MidiApplyResult Start();
    void Stop() noexcept;

    static bool Validate(const MidiPatchbayConfiguration& configuration, std::string& error);

    // Validates, applies and (when persist is true) saves the configuration.
    // On failure the previous configuration stays in effect.
    MidiApplyResult Apply(const MidiPatchbayConfiguration& configuration, bool persist = true);

    // Re-enumerates MIDI 1.0 ports and reopens routed ones that came back.
    void Refresh();

    MidiPatchbaySnapshot Snapshot() const;

    // Tests only: replace the monotonic (ns) and wall (Unix ms) clocks used
    // for traffic timing. Call before any traffic.
    using TimeSource = std::int64_t (*)();
    void SetTimeSourcesForTesting(TimeSource steadyNs, TimeSource unixMs) noexcept;

private:
    struct Runtime;
    struct PortCounters;
    struct OpenConnection {
        std::uint64_t cookie = 0;
        std::string endpointId;        // virtual port loopbacks only
        std::string displayName;       // virtual port loopbacks only
    };

    MidiApplyResult ApplyLocked(const MidiPatchbayConfiguration& configuration);
    void OnReceive(std::uint64_t cookie, const std::uint32_t* words, std::size_t count) noexcept;
    void Route(const Runtime& runtime, std::uint64_t cookie, const std::uint32_t* words, std::size_t count) noexcept;
    bool Save(const MidiPatchbayConfiguration& configuration, std::string& error) const;

    std::unique_ptr<MidiBackend> backend_;
    std::filesystem::path storePath_;
    std::string unavailableReason_;

    mutable std::mutex controlMutex_;
    MidiPatchbayConfiguration configuration_;
    std::vector<MidiEndpointInfo> endpoints_;
    std::string enumerationError_;
    // By "<virtual port id>:in", "<virtual port id>:out" or "endpoint:<id>".
    std::map<std::string, OpenConnection> connections_;
    std::map<std::string, std::string> endpointErrors_;  // by endpoint id
    std::uint64_t nextCookie_ = 1;
    // By port id; kept across configuration changes so counters never reset.
    std::map<std::string, std::shared_ptr<PortCounters>> counters_;
    TimeSource steadyNs_;
    TimeSource unixMs_;

    mutable std::mutex runtimeMutex_;
    std::shared_ptr<const Runtime> runtime_;
};
