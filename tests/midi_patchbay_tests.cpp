#include "midi_patchbay.h"

#include <algorithm>
#include <cmath>
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
        std::vector<std::int64_t> sentAt;  // 0 = sent immediately
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
        state_.sentAt.push_back(0);
        return true;
    }
    bool SendAt(std::uint64_t cookie, const std::uint32_t* words, std::size_t count, std::int64_t at) noexcept override {
        state_.sent.push_back({cookie, std::vector<std::uint32_t>(words, words + count)});
        state_.sentAt.push_back(at);
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

// A card running 100 ppm fast against the host clock, published like the
// ASIO callback does (256-frame buffers at 48 kHz).
struct SimulatedCard {
    AudioTimebase timebase;
    double nominal = 48000.0;
    double actual = 48000.0 * (1.0 + 100e-6);
    std::int64_t startNs = 0;
    std::uint64_t frames = 0;
    void Begin(std::int64_t now) { timebase.Begin(nominal, AudioTimebaseSource::Asio); startNs = now; frames = 0; }
    // Publishes every buffer that has started by host time `now`.
    void Advance(std::int64_t now) {
        for (;;) {
            const auto at = startNs + static_cast<std::int64_t>(static_cast<double>(frames) / actual * 1e9);
            if (at > now) return;
            timebase.Publish(frames, at);
            frames += 256;
        }
    }
    double TrueCardSeconds(std::int64_t hostNs) const { return static_cast<double>(hostNs - startNs) * 1e-9 * actual / nominal; }
};

bool testEngineClockFollowsTheSoundCard() {
    gSteadyNs = 1'000'000'000;
    SimulatedCard card;
    card.Begin(gSteadyNs);
    FakeBackend::State state;
    state.endpoints = endpoints();
    MidiPatchbay patchbay(std::make_unique<FakeBackend>(state), tempStore("engine-clock.json"));
    patchbay.SetTimeSourcesForTesting(fakeSteadyNs, fakeUnixMs);
    patchbay.SetTimebaseForTesting(&card.timebase);
    CHECK(patchbay.Start().success);
    MidiPatchbayConfiguration configuration;
    std::string error;
    CHECK(midi_patchbay_json::Decode(R"({"virtualPorts":[],"routes":[
        {"id":"clock-to-synth","source":"midi:clock:engine","destination":"midi:port:1:usb-synth"},
        {"id":"keys-to-multi","source":"midi:port:1:usb-keys","destination":"midi:port:1:usb-multi"}]})", configuration, error));
    CHECK(patchbay.Apply(configuration).success);
    const auto synth = cookieOf(state, "usb-synth");

    // Let the timebase measure the card for 5 s, then start at 120 BPM.
    for (int ms = 0; ms < 5000; ++ms) { gSteadyNs += 1'000'000; card.Advance(gSteadyNs); if (ms % 2 == 0) patchbay.PumpClock(gSteadyNs); }
    CHECK(patchbay.SetEngineClock(120.0, "start").success);
    CHECK(state.sent.size() == 1 && state.sent[0].first == synth && state.sent[0].second[0] == 0x10FA0000u);
    std::vector<std::int64_t> ticks;
    for (int ms = 0; ms < 20000; ++ms) {
        gSteadyNs += 1'000'000;
        card.Advance(gSteadyNs);
        if (ms % 2 == 0) {
            const auto before = state.sent.size();
            patchbay.PumpClock(gSteadyNs);
            for (auto i = before; i < state.sent.size(); ++i) {
                CHECK(state.sent[i].second[0] == 0x10F80000u);
                // Scheduled ahead, within the 20 ms look-ahead.
                // Scheduled ahead within the 20 ms look-ahead, or sent now when already due.
                const auto at = state.sentAt[i] ? state.sentAt[i] : gSteadyNs;
                CHECK(at >= gSteadyNs - 1'000'000 && at <= gSteadyNs + 21'000'000);
                ticks.push_back(at);
            }
        }
    }
    // 20 s at 120 BPM = 960 ticks, on the card's clock, not Windows'.
    CHECK(ticks.size() >= 958 && ticks.size() <= 962);
    const double period = 60.0 / (120.0 * 24.0);
    double worst = 0;
    for (std::size_t i = 1; i < ticks.size(); ++i)
        worst = std::max(worst, std::fabs(card.TrueCardSeconds(ticks[i]) - card.TrueCardSeconds(ticks[i - 1]) - period));
    CHECK(worst < 20e-6);
    // Over the run the clock matches the card within a few ppm; on the Windows
    // clock it runs 100 ppm fast, exactly like the card.
    const double cardSpan = card.TrueCardSeconds(ticks.back()) - card.TrueCardSeconds(ticks.front());
    const double expected = period * static_cast<double>(ticks.size() - 1);
    CHECK(std::fabs(cardSpan / expected - 1.0) < 5e-6);
    const double hostSpan = static_cast<double>(ticks.back() - ticks.front()) * 1e-9;
    CHECK(std::fabs((expected / hostSpan - 1.0) * 1e6 - 100.0) < 5.0);

    // An incoming clock at exactly 120 BPM on the Windows clock is measured on
    // the card timebase: against a card running 100 ppm fast it is
    // 120 / 1.0001 = 119.988 BPM, the tempo the recorded audio will show.
    const auto keys = cookieOf(state, "usb-keys");
    const std::uint32_t keyTick = 0x10F80000u;
    for (int i = 0; i < 48; ++i) {
        gSteadyNs += 60'000'000'000LL / (120 * 24);
        card.Advance(gSteadyNs);
        patchbay.PumpClock(gSteadyNs);
        state.handler(keys, &keyTick, 1);
    }
    const auto snapshot = patchbay.Snapshot();
    const auto keysPort = findPort(snapshot, "midi:port:1:usb-keys");
    CHECK(keysPort && std::fabs(keysPort->clockBpm - 119.99) < 0.006);
    CHECK(snapshot.referenceClock.running && std::fabs(snapshot.referenceClock.driftPpm - 100.0) < 5.0);
    CHECK(snapshot.engineClock.running && snapshot.engineClock.bpm == 120.0 && snapshot.engineClock.ticks >= ticks.size());
    const auto clockPort = findPort(snapshot, "midi:clock:engine");
    CHECK(clockPort && clockPort->clockRunning && clockPort->clockBpm == 120.0 && clockPort->transport == "start");

    CHECK(patchbay.SetEngineClock(std::nullopt, "stop").success);
    CHECK(state.sent.back().second[0] == 0x10FC0000u);
    const auto stopped = state.sent.size();
    gSteadyNs += 100'000'000;
    patchbay.PumpClock(gSteadyNs);
    CHECK(state.sent.size() == stopped);
    CHECK(!patchbay.SetEngineClock(400.0, "").success);
    CHECK(!patchbay.SetEngineClock(std::nullopt, "rewind").success);
    // A clock cannot be sent to the engine clock.
    configuration.routes[0].sourcePortId = "midi:port:1:usb-keys";
    configuration.routes[0].destinationPortId = "midi:clock:engine";
    CHECK(!MidiPatchbay::Validate(configuration, error));
    return true;
}

bool testIncomingClockIsReclockedOnTheCardGrid() {
    gSteadyNs = 1'000'000'000;
    SimulatedCard card;
    card.Begin(gSteadyNs);
    FakeBackend::State state;
    state.endpoints = endpoints();
    MidiPatchbay patchbay(std::make_unique<FakeBackend>(state), tempStore("reclock.json"));
    patchbay.SetTimeSourcesForTesting(fakeSteadyNs, fakeUnixMs);
    patchbay.SetTimebaseForTesting(&card.timebase);
    CHECK(patchbay.Start().success);
    for (int ms = 0; ms < 5000; ++ms) { gSteadyNs += 1'000'000; card.Advance(gSteadyNs); if (ms % 2 == 0) patchbay.PumpClock(gSteadyNs); }
    MidiPatchbayConfiguration configuration;
    std::string error;
    CHECK(midi_patchbay_json::Decode(R"({"virtualPorts":[{"id":"daw","name":"Timox DAW"}],"routes":[
        {"id":"keys-to-daw","source":"midi:port:1:usb-keys","destination":"midi:virtual:daw","reclock":true},
        {"id":"keys-to-multi","source":"midi:port:1:usb-keys","destination":"midi:port:1:usb-multi"}]})", configuration, error));
    CHECK(patchbay.Apply(configuration).success);
    const auto keys = cookieOf(state, "usb-keys");
    const auto dawOut = cookieOf(state, "virtual:Timox DAW Out");
    const auto multi = cookieOf(state, "usb-multi");

    // A keyboard clock at exactly 120 BPM on the Windows clock, with ±1.5 ms
    // of USB and scheduling jitter on each tick.
    const std::uint32_t tick = 0x10F80000u;
    const std::int64_t period = 60'000'000'000LL / (120 * 24);
    const std::int64_t start = gSteadyNs;
    std::vector<std::int64_t> arrivals, outputs;
    for (int i = 0; i < 192; ++i) {
        const std::int64_t jitter = (static_cast<std::int64_t>((i * 7919) % 31) - 15) * 100'000;
        gSteadyNs = start + i * period + jitter;
        card.Advance(gSteadyNs);
        patchbay.PumpClock(gSteadyNs);
        const auto before = state.sent.size();
        state.handler(keys, &tick, 1);
        arrivals.push_back(gSteadyNs);
        for (auto j = before; j < state.sent.size(); ++j)
            if (state.sent[j].first == dawOut) outputs.push_back(state.sentAt[j] ? state.sentAt[j] : gSteadyNs);
    }
    // One tick out for each tick in; the plain route still forwards raw ticks.
    CHECK(outputs.size() == arrivals.size());
    std::size_t raw = 0;
    for (const auto& message : state.sent) raw += message.first == multi;
    CHECK(raw == arrivals.size());
    // After the first ticks the output is regular: jitter drops from ±1.5 ms
    // to a few microseconds, with a few milliseconds of delay at most.
    double worstOut = 0, worstIn = 0, worstDelay = 0;
    for (std::size_t i = 16; i < outputs.size(); ++i) {
        worstOut = std::max(worstOut, std::fabs(card.TrueCardSeconds(outputs[i]) - card.TrueCardSeconds(outputs[i - 1]) -
            card.TrueCardSeconds(start + period) + card.TrueCardSeconds(start)));
        worstIn = std::max(worstIn, std::fabs(static_cast<double>(arrivals[i] - arrivals[i - 1] - period)) * 1e-9);
        worstDelay = std::max(worstDelay, static_cast<double>(outputs[i] - arrivals[i]) * 1e-9);
        CHECK(outputs[i] >= arrivals[i]);
    }
    CHECK(worstIn > 1e-3);
    CHECK(worstOut < 0.3e-3);
    CHECK(worstDelay < 5e-3);

    // Despite ±1.5 ms jitter the measured tempo stays within 0.2 BPM.
    const auto snapshot = patchbay.Snapshot();
    const auto keysPort = findPort(snapshot, "midi:port:1:usb-keys");
    CHECK(keysPort && keysPort->clockRunning && std::fabs(keysPort->clockBpm - 120.0) < 0.2);
    CHECK(snapshot.routes[0].reclocked == arrivals.size());

    // Start resets the history: the next ticks are forwarded at once.
    const std::uint32_t startMessage = 0x10FA0000u;
    state.handler(keys, &startMessage, 1);
    gSteadyNs += period;
    const auto before = state.sent.size();
    state.handler(keys, &tick, 1);
    for (auto j = before; j < state.sent.size(); ++j)
        if (state.sent[j].first == dawOut) CHECK(state.sentAt[j] == 0);
    return true;
}

bool testEngineClockNeverFallsBackToWindowsTime() {
    gSteadyNs = 1'000'000'000;
    SimulatedCard card;
    FakeBackend::State state;
    state.endpoints = endpoints();
    MidiPatchbay patchbay(std::make_unique<FakeBackend>(state), tempStore("reference-clock.json"));
    patchbay.SetTimeSourcesForTesting(fakeSteadyNs, fakeUnixMs);
    patchbay.SetTimebaseForTesting(&card.timebase);
    CHECK(patchbay.Start().success);
    MidiPatchbayConfiguration configuration;
    std::string error;
    CHECK(midi_patchbay_json::Decode(R"({"virtualPorts":[{"id":"daw","name":"Timox DAW"}],"routes":[
        {"id":"clock","source":"midi:clock:engine","destination":"midi:port:1:usb-synth"},
        {"id":"keys","source":"midi:port:1:usb-keys","destination":"midi:virtual:daw","reclock":true}]})", configuration, error));
    CHECK(patchbay.Apply(configuration).success);
    patchbay.PumpClock(gSteadyNs);

    // No reference clock running: the engine clock cannot start, and an incoming
    // clock on a reclock route passes through untouched.
    CHECK(!patchbay.SetEngineClock(std::nullopt, "start").success);
    CHECK(patchbay.Snapshot().referenceClock.source == "none");
    const std::uint32_t tick = 0x10F80000u;
    state.handler(cookieOf(state, "usb-keys"), &tick, 1);
    CHECK(state.sent.size() == 1 && state.sent[0].first == cookieOf(state, "virtual:Timox DAW Out") && state.sentAt[0] == 0);

    // The software clock becomes the reference clock (no card): the engine clock follows it.
    card.actual = card.nominal;
    card.timebase.Begin(card.nominal, AudioTimebaseSource::Software);
    card.startNs = gSteadyNs;
    card.frames = 0;
    for (int ms = 0; ms < 3000; ++ms) { gSteadyNs += 1'000'000; card.Advance(gSteadyNs); if (ms % 2 == 0) patchbay.PumpClock(gSteadyNs); }
    CHECK(patchbay.Snapshot().referenceClock.source == "software");
    CHECK(patchbay.SetEngineClock(120.0, "start").success);
    const auto ticksBetween = [&](int milliseconds) {
        const auto before = state.sent.size();
        for (int ms = 0; ms < milliseconds; ++ms) { gSteadyNs += 1'000'000; card.Advance(gSteadyNs); if (ms % 2 == 0) patchbay.PumpClock(gSteadyNs); }
        std::size_t ticks = 0;
        for (auto i = before; i < state.sent.size(); ++i) ticks += state.sent[i].second[0] == 0x10F80000u;
        return ticks;
    };
    CHECK(ticksBetween(1000) >= 47);

    // The reference clock stops: no tick is sent on Windows time; the clock waits.
    card.timebase.End();
    CHECK(ticksBetween(1000) <= 1);  // at most one tick scheduled before the stop was seen
    auto snapshot = patchbay.Snapshot();
    CHECK(snapshot.engineClock.running && snapshot.engineClock.waitingForReferenceClock);
    CHECK(!findPort(snapshot, "midi:clock:engine")->clockRunning);

    // The card comes back (new session, new sample count): ticks resume on it.
    card.timebase.Begin(card.nominal, AudioTimebaseSource::Asio);
    card.startNs = gSteadyNs;
    card.frames = 0;
    CHECK(ticksBetween(1000) >= 47);
    snapshot = patchbay.Snapshot();
    CHECK(snapshot.referenceClock.source == "asio" && !snapshot.engineClock.waitingForReferenceClock);
    return true;
}

int main() {
    bool ok = true;
    ok &= testApplyRoutesAndPersists();
    ok &= testReapplyKeepsConnectionsAndHotplugReopens();
    ok &= testFailuresKeepPreviousConfiguration();
    ok &= testInAndOutAreSwitchedIndependently();
    ok &= testClockIsToldApartFromActivity();
    ok &= testEngineClockFollowsTheSoundCard();
    ok &= testIncomingClockIsReclockedOnTheCardGrid();
    ok &= testEngineClockNeverFallsBackToWindowsTime();
    ok &= testValidation();
    if (!ok) return 1;
    std::puts("MidiPatchbayTests passed");
    return 0;
}
