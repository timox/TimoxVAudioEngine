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

// Port ids exposed by the API:
//   midi:virtual:<id>          app-to-app port created by the engine
//   midi:endpoint:<endpointId> OS MIDI endpoint (hardware, network, other apps)
struct MidiVirtualPortConfig {
    std::string id;    // [a-z0-9-], 1..32 characters
    std::string name;  // shown to applications, 1..24 characters
};

struct MidiPatchbayConfiguration {
    std::vector<MidiVirtualPortConfig> virtualPorts;
    std::vector<MidiRoute> routes;
};

struct MidiPortStatus {
    std::string id;
    std::string name;
    std::string kind;              // "virtual" or "endpoint"
    std::string transport;
    bool online = false;           // endpoint present / virtual port created
    bool open = false;             // engine holds a connection
    std::string clientEndpointId;  // virtual ports: id other apps open
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
    static constexpr const char* kEndpointPrefix = "midi:endpoint:";

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

    // Re-enumerates endpoints and reopens routed endpoints that came back.
    void Refresh();

    MidiPatchbaySnapshot Snapshot() const;

private:
    struct Runtime;
    struct OpenConnection {
        std::uint64_t cookie = 0;
        std::string clientEndpointId;  // virtual ports only
        std::string displayName;       // virtual ports only
    };

    MidiApplyResult ApplyLocked(const MidiPatchbayConfiguration& configuration);
    void OnReceive(std::uint64_t cookie, const std::uint32_t* words, std::size_t count) noexcept;
    bool Save(const MidiPatchbayConfiguration& configuration, std::string& error) const;

    std::unique_ptr<MidiBackend> backend_;
    std::filesystem::path storePath_;
    std::string unavailableReason_;

    mutable std::mutex controlMutex_;
    MidiPatchbayConfiguration configuration_;
    std::vector<MidiEndpointInfo> endpoints_;
    std::string enumerationError_;
    std::map<std::string, OpenConnection> connections_;  // by port id
    std::map<std::string, std::string> portErrors_;
    std::uint64_t nextCookie_ = 1;

    mutable std::mutex runtimeMutex_;
    std::shared_ptr<const Runtime> runtime_;
};
