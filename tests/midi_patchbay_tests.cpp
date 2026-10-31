#include "midi_patchbay.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); return false; } } while (0)

class FakeBackend final : public MidiBackend {
public:
    struct State {
        std::vector<MidiEndpointInfo> endpoints;
        std::map<std::uint64_t, std::string> open;  // cookie -> endpoint id or "virtual:<name>"
        std::vector<std::pair<std::uint64_t, std::vector<std::uint32_t>>> sent;
        std::set<std::string> failOpen;
        bool failVirtual = false;
        ReceiveHandler handler;
    };
    explicit FakeBackend(State& state) : state_(state) {}
    std::string Name() const override { return "fake"; }
    void SetReceiveHandler(ReceiveHandler handler) override { state_.handler = std::move(handler); }
    bool Enumerate(std::vector<MidiEndpointInfo>& endpoints, std::string&) override {
        endpoints = state_.endpoints;
        return true;
    }
    bool OpenEndpoint(std::uint64_t cookie, const std::string& endpointId, std::string& error) override {
        if (state_.failOpen.count(endpointId)) { error = "busy"; return false; }
        state_.open[cookie] = endpointId;
        return true;
    }
    bool CreateVirtualPort(std::uint64_t cookie, const std::string&, const std::string& name,
                           std::string& clientId, std::string& error) override {
        if (state_.failVirtual) { error = "transport unavailable"; return false; }
        state_.open[cookie] = "virtual:" + name;
        clientId = "client-" + name;
        return true;
    }
    void Close(std::uint64_t cookie) noexcept override { state_.open.erase(cookie); }
    bool Send(std::uint64_t cookie, const std::uint32_t* words, std::size_t count) noexcept override {
        state_.sent.push_back({cookie, std::vector<std::uint32_t>(words, words + count)});
        return true;
    }
private:
    State& state_;
};

std::uint64_t cookieOf(const FakeBackend::State& state, const std::string& target) {
    for (const auto& entry : state.open) if (entry.second == target) return entry.first;
    return 0;
}

std::filesystem::path tempStore(const char* name) {
    const auto path = std::filesystem::temp_directory_path() / "timox-midi-tests" / name;
    std::error_code code;
    std::filesystem::remove(path, code);
    return path;
}

// Endpoints as Windows MIDI Services lists them, with their MIDI 1.0 ports.
std::vector<MidiEndpointInfo> endpoints() {
    return {
        {"usb-synth", "Synth", "KS", {{0, "Synth MIDI In", false, true}}},
        {"usb-keys", "Keys", "KS", {{0, "Keys MIDI Out", true, false}}},
        {"usb-multi", "Multi", "KS", {{0, "Multi Port 1", true, true}, {1, "Multi Port 2", true, true}}},
    };
}

const char* kConfiguration = R"({
  "virtualPorts": [{"id": "daw", "name": "Timox DAW"}],
  "routes": [
    {"id": "to-synth", "source": "midi:virtual:daw", "destination": "midi:port:1:usb-synth",
     "channels": [1], "outputChannel": 2},
    {"id": "from-keys", "source": "midi:port:1:usb-keys", "destination": "midi:virtual:daw"},
    {"id": "to-multi-2", "source": "midi:virtual:daw", "destination": "midi:port:2:usb-multi"},
    {"id": "to-absent", "source": "midi:virtual:daw", "destination": "midi:port:1:unplugged"},
    {"id": "wrong-way", "source": "midi:virtual:daw", "destination": "midi:port:1:usb-keys"}
  ]
})";

bool testApplyRoutesAndPersists() {
    FakeBackend::State state;
    state.endpoints = endpoints();
    const auto store = tempStore("apply.json");
    MidiPatchbay patchbay(std::make_unique<FakeBackend>(state), store);
    CHECK(patchbay.Start().success);

    MidiPatchbayConfiguration configuration;
    std::string error;
    CHECK(midi_patchbay_json::Decode(kConfiguration, configuration, error));
    CHECK(patchbay.Apply(configuration).success);
    // One connection per endpoint, only for endpoints used by live routes.
    CHECK(state.open.size() == 4);
    const auto daw = cookieOf(state, "virtual:Timox DAW");
    const auto synth = cookieOf(state, "usb-synth");
    const auto keys = cookieOf(state, "usb-keys");
    const auto multi = cookieOf(state, "usb-multi");
    CHECK(daw && synth && keys && multi);

    // DAW channel 1 goes to the synth on channel 2; channel 2 is filtered out.
    // Every DAW message also reaches port 2 of the multi-port interface (group 2).
    const std::uint32_t ch1 = 0x20903C64u, ch2 = 0x20913C64u;
    state.handler(daw, &ch1, 1);
    CHECK(state.sent.size() == 2);
    CHECK(state.sent[0].first == synth && state.sent[0].second[0] == 0x20913C64u);
    CHECK(state.sent[1].first == multi && state.sent[1].second[0] == 0x21903C64u);
    state.handler(daw, &ch2, 1);
    CHECK(state.sent.size() == 3 && state.sent[2].first == multi);

    // Keyboard to DAW. Another group of the same endpoint is another port.
    state.handler(keys, &ch2, 1);
    CHECK(state.sent.size() == 4 && state.sent[3].first == daw && state.sent[3].second[0] == ch2);
    const std::uint32_t otherGroup = 0x23913C64u;
    state.handler(keys, &otherGroup, 1);
    CHECK(state.sent.size() == 4);
    // MIDI 2.0 from a MIDI 2.0 keyboard is carried as MIDI 1.0.
    const std::uint32_t midi2Note[] = {0x40923C00u, 0xFFFF0000u};
    state.handler(keys, midi2Note, 2);
    CHECK(state.sent.size() == 5 && state.sent[4].second.size() == 1 && state.sent[4].second[0] == 0x20923C7Fu);

    const auto snapshot = patchbay.Snapshot();
    CHECK(snapshot.available && snapshot.routes.size() == 5);
    CHECK(snapshot.routes[0].active && snapshot.routes[0].forwarded == 1 && snapshot.routes[0].filtered == 1);
    CHECK(snapshot.routes[2].active);
    CHECK(!snapshot.routes[3].active);  // unplugged
    CHECK(!snapshot.routes[4].active);  // keyboard port cannot receive
    bool absentListed = false, multiPort2 = false;
    for (const auto& port : snapshot.ports) {
        if (port.id == "midi:port:1:unplugged") absentListed = !port.online;
        if (port.id == "midi:port:2:usb-multi")
            multiPort2 = port.name == "Multi Port 2" && port.group == 2 && port.source && port.destination;
        if (port.id == "midi:virtual:daw") CHECK(port.clientEndpointId == "client-Timox DAW");
    }
    CHECK(absentListed && multiPort2);

    // Persisted file reloads into an equivalent configuration.
    std::ifstream input(store);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    MidiPatchbayConfiguration reloaded;
    CHECK(midi_patchbay_json::Decode(text, reloaded, error));
    CHECK(midi_patchbay_json::Encode(reloaded) == midi_patchbay_json::Encode(configuration));
    return true;
}

bool testReapplyKeepsConnectionsAndHotplugReopens() {
    FakeBackend::State state;
    state.endpoints = {endpoints()[0]};
    MidiPatchbay patchbay(std::make_unique<FakeBackend>(state), tempStore("hotplug.json"));
    CHECK(patchbay.Start().success);
    MidiPatchbayConfiguration configuration;
    std::string error;
    CHECK(midi_patchbay_json::Decode(kConfiguration, configuration, error));
    CHECK(patchbay.Apply(configuration).success);
    const auto daw = cookieOf(state, "virtual:Timox DAW");
    CHECK(daw && !cookieOf(state, "usb-keys"));

    // Same configuration: the virtual port is not recreated (apps keep it open).
    CHECK(patchbay.Apply(configuration).success);
    CHECK(cookieOf(state, "virtual:Timox DAW") == daw);

    // Keyboard plugged in later.
    state.endpoints.push_back(endpoints()[1]);
    patchbay.Refresh();
    CHECK(cookieOf(state, "usb-keys") != 0);
    // Synth unplugged: its connection is released.
    state.endpoints.erase(state.endpoints.begin());
    patchbay.Refresh();
    CHECK(cookieOf(state, "usb-synth") == 0);

    // Renaming recreates the virtual port; removing everything closes everything.
    configuration.virtualPorts[0].name = "Timox Live";
    CHECK(patchbay.Apply(configuration).success);
    CHECK(!cookieOf(state, "virtual:Timox DAW") && cookieOf(state, "virtual:Timox Live"));
    CHECK(patchbay.Apply(MidiPatchbayConfiguration{}).success);
    CHECK(state.open.empty());
    return true;
}

bool testFailuresKeepPreviousConfiguration() {
    FakeBackend::State state;
    state.endpoints = endpoints();
    MidiPatchbay patchbay(std::make_unique<FakeBackend>(state), tempStore("failure.json"));
    CHECK(patchbay.Start().success);
    MidiPatchbayConfiguration configuration;
    std::string error;
    CHECK(midi_patchbay_json::Decode(kConfiguration, configuration, error));

    // Endpoint failure is not fatal: only its routes are inactive.
    state.failOpen.insert("usb-keys");
    CHECK(patchbay.Apply(configuration).success);
    auto snapshot = patchbay.Snapshot();
    CHECK(snapshot.routes[0].active && !snapshot.routes[1].active);
    for (const auto& port : snapshot.ports)
        if (port.id == "midi:port:1:usb-keys") CHECK(port.error == "busy");

    // Virtual port failure rejects the whole change.
    auto changed = configuration;
    changed.virtualPorts.push_back({"live", "Timox Live"});
    state.failVirtual = true;
    CHECK(!patchbay.Apply(changed).success);
    snapshot = patchbay.Snapshot();
    CHECK(snapshot.configuration.virtualPorts.size() == 1);
    CHECK(!cookieOf(state, "virtual:Timox Live") && cookieOf(state, "virtual:Timox DAW"));
    return true;
}

bool testValidation() {
    MidiPatchbayConfiguration configuration;
    std::string error;
    CHECK(!midi_patchbay_json::Decode(R"({"virtualPorts":[],"routes":[],"extra":1})", configuration, error));
    // Groups are not a route option: each MIDI 1.0 port already is one group.
    CHECK(!midi_patchbay_json::Decode(R"({"virtualPorts":[],"routes":[{"id":"r","source":"a","destination":"b","groups":[1]}]})",
        configuration, error));
    CHECK(!midi_patchbay_json::Decode(R"({"virtualPorts":[],"routes":[{"id":"r","source":"a","destination":"b","channels":[17]}]})",
        configuration, error));
    CHECK(!midi_patchbay_json::Decode(R"({"virtualPorts":[],"routes":[{"id":"r","source":"a","destination":"b","messages":["noise"]}]})",
        configuration, error));
    CHECK(midi_patchbay_json::Decode(
        R"({"virtualPorts":[],"routes":[{"id":"r","source":"midi:virtual:missing","destination":"midi:port:1:x"}]})",
        configuration, error));
    CHECK(!MidiPatchbay::Validate(configuration, error));  // undefined virtual port
    for (const char* badPort : {"midi:port:0:x", "midi:port:17:x", "midi:port:01:x", "midi:port:1:", "midi:endpoint:x"}) {
        MidiPatchbayConfiguration bad;
        bad.virtualPorts.push_back({"daw", "Timox DAW"});
        MidiRoute route;
        route.id = "r"; route.sourcePortId = "midi:virtual:daw"; route.destinationPortId = badPort;
        bad.routes.push_back(route);
        CHECK(!MidiPatchbay::Validate(bad, error));
    }
    std::uint8_t group = 0;
    std::string endpointId;
    CHECK(MidiPatchbay::ParsePortId("midi:port:16:\\\\?\\swd#midisrv#a:b", group, endpointId));
    CHECK(group == 15 && endpointId == "\\\\?\\swd#midisrv#a:b");
    CHECK(MidiPatchbay::PortId(15, endpointId) == "midi:port:16:\\\\?\\swd#midisrv#a:b");
    CHECK(midi_patchbay_json::Decode(R"({"virtualPorts":[{"id":"Bad Id","name":"x"}],"routes":[]})", configuration, error));
    CHECK(!MidiPatchbay::Validate(configuration, error));
    CHECK(midi_patchbay_json::Decode(
        R"({"virtualPorts":[{"id":"a","name":"Same"},{"id":"b","name":"Same"}],"routes":[]})", configuration, error));
    CHECK(!MidiPatchbay::Validate(configuration, error));

    MidiPatchbay unavailable(nullptr, {}, "Windows MIDI Services is not installed");
    CHECK(!unavailable.Start().success);
    const auto snapshot = unavailable.Snapshot();
    CHECK(!snapshot.available && snapshot.error == "Windows MIDI Services is not installed");
    return true;
}
}

int main() {
    bool ok = true;
    ok &= testApplyRoutesAndPersists();
    ok &= testReapplyKeepsConnectionsAndHotplugReopens();
    ok &= testFailuresKeepPreviousConfiguration();
    ok &= testValidation();
    if (!ok) return 1;
    std::puts("MidiPatchbayTests passed");
    return 0;
}
