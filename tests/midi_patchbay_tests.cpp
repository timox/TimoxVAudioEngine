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

const char* kConfiguration = R"({
  "virtualPorts": [{"id": "daw", "name": "Timox DAW"}],
  "routes": [
    {"id": "to-synth", "source": "midi:virtual:daw", "destination": "midi:endpoint:usb-synth",
     "channels": [1], "outputChannel": 2},
    {"id": "from-keys", "source": "midi:endpoint:usb-keys", "destination": "midi:virtual:daw"},
    {"id": "to-absent", "source": "midi:virtual:daw", "destination": "midi:endpoint:unplugged"}
  ]
})";

bool testApplyRoutesAndPersists() {
    FakeBackend::State state;
    state.endpoints = {{"usb-synth", "Synth", "KS"}, {"usb-keys", "Keys", "KS"}, {"other", "Other", "KS"}};
    const auto store = tempStore("apply.json");
    MidiPatchbay patchbay(std::make_unique<FakeBackend>(state), store);
    CHECK(patchbay.Start().success);

    MidiPatchbayConfiguration configuration;
    std::string error;
    CHECK(midi_patchbay_json::Decode(kConfiguration, configuration, error));
    const auto applied = patchbay.Apply(configuration);
    CHECK(applied.success);
    // Only routed, present endpoints are opened.
    CHECK(state.open.size() == 3);
    const auto daw = cookieOf(state, "virtual:Timox DAW");
    const auto synth = cookieOf(state, "usb-synth");
    const auto keys = cookieOf(state, "usb-keys");
    CHECK(daw && synth && keys);

    const std::uint32_t ch1 = 0x20903C64u, ch2 = 0x20913C64u;
    state.handler(daw, &ch1, 1);
    state.handler(daw, &ch2, 1);
    CHECK(state.sent.size() == 1 && state.sent[0].first == synth && state.sent[0].second[0] == 0x20913C64u);
    state.handler(keys, &ch2, 1);
    CHECK(state.sent.size() == 2 && state.sent[1].first == daw);

    const auto snapshot = patchbay.Snapshot();
    CHECK(snapshot.available && snapshot.routes.size() == 3);
    CHECK(snapshot.routes[0].active && snapshot.routes[0].forwarded == 1 && snapshot.routes[0].filtered == 1);
    CHECK(!snapshot.routes[2].active);
    bool absentListed = false;
    for (const auto& port : snapshot.ports) {
        if (port.id == "midi:endpoint:unplugged") absentListed = !port.online;
        if (port.id == "midi:virtual:daw") CHECK(port.clientEndpointId == "client-Timox DAW");
    }
    CHECK(absentListed);

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
    state.endpoints = {{"usb-synth", "Synth", "KS"}};
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
    state.endpoints.push_back({"usb-keys", "Keys", "KS"});
    patchbay.Refresh();
    CHECK(cookieOf(state, "usb-keys") != 0);
    // Synth unplugged: its connection is released.
    state.endpoints.erase(state.endpoints.begin());
    patchbay.Refresh();
    CHECK(cookieOf(state, "usb-synth") == 0);

    // Renaming recreates the virtual port; removing it closes everything routed to it.
    configuration.virtualPorts[0].name = "Timox Live";
    CHECK(patchbay.Apply(configuration).success);
    CHECK(!cookieOf(state, "virtual:Timox DAW") && cookieOf(state, "virtual:Timox Live"));
    CHECK(patchbay.Apply(MidiPatchbayConfiguration{}).success);
    CHECK(state.open.empty());
    return true;
}

bool testFailuresKeepPreviousConfiguration() {
    FakeBackend::State state;
    state.endpoints = {{"usb-synth", "Synth", "KS"}, {"usb-keys", "Keys", "KS"}};
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
        if (port.id == "midi:endpoint:usb-keys") CHECK(port.error == "busy");

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
    CHECK(!midi_patchbay_json::Decode(R"({"virtualPorts":[],"routes":[{"id":"r","source":"a","destination":"b","channels":[17]}]})",
        configuration, error));
    CHECK(!midi_patchbay_json::Decode(R"({"virtualPorts":[],"routes":[{"id":"r","source":"a","destination":"b","messages":["noise"]}]})",
        configuration, error));
    CHECK(midi_patchbay_json::Decode(
        R"({"virtualPorts":[],"routes":[{"id":"r","source":"midi:virtual:missing","destination":"midi:endpoint:x"}]})",
        configuration, error));
    CHECK(!MidiPatchbay::Validate(configuration, error));  // undefined virtual port
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
