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
                           MidiVirtualDirection direction, std::string& endpointId, std::string& error) override {
        if (state_.failVirtual) { error = "transport unavailable"; return false; }
        endpointId = name + (direction == MidiVirtualDirection::ToEngine ? " In" : " Out");
        state_.open[cookie] = "virtual:" + endpointId;
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
    // DAW In and DAW Out, plus one connection per endpoint used by live routes.
    CHECK(state.open.size() == 5);
    const auto daw = cookieOf(state, "virtual:Timox DAW In");
    const auto dawOut = cookieOf(state, "virtual:Timox DAW Out");
    const auto synth = cookieOf(state, "usb-synth");
    const auto keys = cookieOf(state, "usb-keys");
    const auto multi = cookieOf(state, "usb-multi");
    CHECK(daw && dawOut && synth && keys && multi);

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
    CHECK(state.sent.size() == 4 && state.sent[3].first == dawOut && state.sent[3].second[0] == ch2);
    const std::uint32_t otherGroup = 0x23913C64u;
    state.handler(keys, &otherGroup, 1);
    CHECK(state.sent.size() == 4);
    // MIDI 2.0 messages are never carried.
    const std::uint32_t midi2Note[] = {0x40923C00u, 0xFFFF0000u};
    state.handler(keys, midi2Note, 2);
    CHECK(state.sent.size() == 4);
    state.handler(keys, &ch1, 1);
    CHECK(state.sent.size() == 5 && state.sent[4].second[0] == ch1);

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
        if (port.id == "midi:virtual:daw")
            CHECK(port.inputEndpointId == "Timox DAW In" && port.outputEndpointId == "Timox DAW Out" &&
                  port.source && port.destination);
    }
    CHECK(absentListed && multiPort2);
    // Traffic is observed per port: everything that arrives, routed or not,
    // and everything delivered.
    for (const auto& port : snapshot.ports) {
        if (port.id == "midi:virtual:daw") CHECK(port.received == 2 && port.sent == 2 && port.lastSentUnixMs > 0);
        if (port.id == "midi:port:1:usb-keys") CHECK(port.received == 2 && port.sent == 0 && port.lastReceivedUnixMs > 0);
        if (port.id == "midi:port:1:usb-synth") CHECK(port.sent == 1 && port.received == 0);
    }

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
    const auto daw = cookieOf(state, "virtual:Timox DAW In");
    CHECK(daw && !cookieOf(state, "usb-keys"));

    const std::uint32_t note = 0x20903C64u;
    state.handler(daw, &note, 1);
    // Same configuration: the virtual port is not recreated (apps keep it open)
    // and its traffic counters are kept.
    CHECK(patchbay.Apply(configuration).success);
    CHECK(cookieOf(state, "virtual:Timox DAW In") == daw);
    for (const auto& port : patchbay.Snapshot().ports)
        if (port.id == "midi:virtual:daw") CHECK(port.received == 1);

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
    CHECK(!cookieOf(state, "virtual:Timox DAW In") && cookieOf(state, "virtual:Timox Live In"));
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
    CHECK(!cookieOf(state, "virtual:Timox Live In") && cookieOf(state, "virtual:Timox DAW In"));
    return true;
}

bool testInAndOutAreSwitchedIndependently() {
    FakeBackend::State state;
    state.endpoints = endpoints();
    MidiPatchbay patchbay(std::make_unique<FakeBackend>(state), tempStore("directions.json"));
    CHECK(patchbay.Start().success);
    MidiPatchbayConfiguration configuration;
    std::string error;
    CHECK(midi_patchbay_json::Decode(kConfiguration, configuration, error));
    CHECK(patchbay.Apply(configuration).success);
    const auto dawIn = cookieOf(state, "virtual:Timox DAW In");

    // Out turned off: only "Timox DAW Out" goes away; routes to the DAW go inactive.
    configuration.virtualPorts[0].output = false;
    CHECK(patchbay.Apply(configuration).success);
    CHECK(cookieOf(state, "virtual:Timox DAW In") == dawIn && !cookieOf(state, "virtual:Timox DAW Out"));
    auto snapshot = patchbay.Snapshot();
    CHECK(snapshot.routes[0].active && !snapshot.routes[1].active);
    for (const auto& port : snapshot.ports)
        if (port.id == "midi:virtual:daw") CHECK(port.source && !port.destination && port.outputEndpointId.empty());
    const std::uint32_t note = 0x20903C64u;
    const auto keys = cookieOf(state, "usb-keys");
    state.handler(keys, &note, 1);
    CHECK(state.sent.empty());

    // Out back on, In off: In goes away, Out is created again.
    configuration.virtualPorts[0].output = true;
    configuration.virtualPorts[0].input = false;
    CHECK(patchbay.Apply(configuration).success);
    CHECK(!cookieOf(state, "virtual:Timox DAW In") && cookieOf(state, "virtual:Timox DAW Out"));
    snapshot = patchbay.Snapshot();
    CHECK(!snapshot.routes[0].active && snapshot.routes[1].active);

    // Both off is rejected, and the direction flags round-trip through JSON.
    configuration.virtualPorts[0].output = false;
    CHECK(!patchbay.Apply(configuration).success);
    MidiPatchbayConfiguration decoded;
    CHECK(midi_patchbay_json::Decode(
        R"({"virtualPorts":[{"id":"daw","name":"Timox DAW","input":false}],"routes":[]})", decoded, error));
    CHECK(!decoded.virtualPorts[0].input && decoded.virtualPorts[0].output);
    CHECK(!midi_patchbay_json::Decode(
        R"({"virtualPorts":[{"id":"daw","name":"Timox DAW","input":"no"}],"routes":[]})", decoded, error));
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
    CHECK(midi_patchbay_json::Decode(R"({"virtualPorts":[{"id":"abcdefghijklmnopqrstuvwxy","name":"x"}],"routes":[]})",
        configuration, error));
    CHECK(!MidiPatchbay::Validate(configuration, error));  // 25 characters: loopback unique id too long
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

std::int64_t gSteadyNs = 1'000'000'000;
std::int64_t fakeSteadyNs() { return gSteadyNs; }
std::int64_t fakeUnixMs() { return 1'700'000'000'000 + gSteadyNs / 1'000'000; }

const MidiPortStatus* findPort(const MidiPatchbaySnapshot& snapshot, const std::string& id) {
    for (const auto& port : snapshot.ports) if (port.id == id) return &port;
    return nullptr;
}

bool testClockIsToldApartFromActivity() {
    FakeBackend::State state;
    state.endpoints = endpoints();
    MidiPatchbay patchbay(std::make_unique<FakeBackend>(state), tempStore("clock.json"));
    patchbay.SetTimeSourcesForTesting(fakeSteadyNs, fakeUnixMs);
    CHECK(patchbay.Start().success);
    MidiPatchbayConfiguration configuration;
    std::string error;
    CHECK(midi_patchbay_json::Decode(kConfiguration, configuration, error));
    CHECK(patchbay.Apply(configuration).success);
    const auto keys = cookieOf(state, "usb-keys");
    const auto dawIn = cookieOf(state, "virtual:Timox DAW In");

    // The keyboard is a clock source at 120 BPM: 24 ticks per quarter note.
    const std::uint32_t tick = 0x10F80000u, start = 0x10FA0000u, sensing = 0x10FE0000u, note = 0x20903C64u;
    const std::int64_t interval = 60'000'000'000LL / (120 * 24);
    state.handler(keys, &start, 1);
    for (int i = 0; i < 48; ++i) {
        gSteadyNs += interval;
        state.handler(keys, &tick, 1);
    }
    state.handler(keys, &sensing, 1);
    auto snapshot = patchbay.Snapshot();
    auto port = findPort(snapshot, "midi:port:1:usb-keys");
    CHECK(port && port->clockTicks == 48 && port->clockRunning && port->clockBpm == 120.0);
    CHECK(port->activity == 1 && port->transport == "start");  // ticks and sensing are not activity
    CHECK(port->activeSensing == 1 && port->received == 50 && !port->clockEchoSuspected);

    // The DAW gets that clock through the patchbay. If it sends clock back on
    // its own In (MIDI thru), it is flagged as a possible echo, not a source.
    auto daw = findPort(snapshot, "midi:virtual:daw");
    CHECK(daw && daw->sent == 50 && daw->clockTicks == 0);
    gSteadyNs += interval;
    state.handler(dawIn, &tick, 1);
    gSteadyNs += interval;
    state.handler(dawIn, &tick, 1);
    snapshot = patchbay.Snapshot();
    daw = findPort(snapshot, "midi:virtual:daw");
    CHECK(daw->clockRunning && daw->clockEchoSuspected && daw->activity == 0);

    // A note is activity; one second without ticks stops the clock.
    state.handler(dawIn, &note, 1);
    gSteadyNs += 2'000'000'000;
    snapshot = patchbay.Snapshot();
    port = findPort(snapshot, "midi:port:1:usb-keys");
    daw = findPort(snapshot, "midi:virtual:daw");
    CHECK(!port->clockRunning && port->clockBpm == 0 && port->lastClockUnixMs > 0);
    CHECK(daw->activity == 1 && daw->lastActivityUnixMs > 0 && !daw->clockEchoSuspected);
    return true;
}

int main() {
    bool ok = true;
    ok &= testApplyRoutesAndPersists();
    ok &= testReapplyKeepsConnectionsAndHotplugReopens();
    ok &= testFailuresKeepPreviousConfiguration();
    ok &= testInAndOutAreSwitchedIndependently();
    ok &= testClockIsToldApartFromActivity();
    ok &= testValidation();
    if (!ok) return 1;
    std::puts("MidiPatchbayTests passed");
    return 0;
}
