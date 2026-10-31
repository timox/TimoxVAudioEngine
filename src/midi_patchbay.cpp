#include "midi_patchbay.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaximumVirtualPorts = 64;
constexpr std::size_t kMaximumRoutes = 512;
constexpr std::uintmax_t kMaximumStoreBytes = 1024 * 1024;

struct CategoryName { const char* name; std::uint32_t bit; };
constexpr CategoryName kCategories[] = {
    {"notes", midi_ump::Notes},
    {"polyPressure", midi_ump::PolyPressure},
    {"controlChange", midi_ump::ControlChange},
    {"programChange", midi_ump::ProgramChange},
    {"channelPressure", midi_ump::ChannelPressure},
    {"pitchBend", midi_ump::PitchBend},
    {"systemRealtime", midi_ump::SystemRealtime},
    {"systemCommon", midi_ump::SystemCommon},
    {"sysex", midi_ump::SysEx},
};

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool validVirtualId(const std::string& id) {
    // Becomes the loopback unique ids "tx-<id>-in" / "tx-<id>-out" (32 max).
    if (id.empty() || id.size() > 24) return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

bool validDisplayName(const std::string& name) {
    // MIDI 1.0 port names are limited to 31 characters once Windows appends
    // the function block name, so names stay short.
    if (name.empty() || name.size() > 24 || name.front() == ' ' || name.back() == ' ') return false;
    for (const unsigned char c : name)
        if (c < 0x20 || c > 0x7E) return false;
    return true;
}

bool decodeMask(const Json& value, std::uint16_t& mask, const char* field, std::string& error) {
    if (!value.is_array() || value.empty() || value.size() > 16) {
        error = std::string(field) + " must list 1 to 16 values between 1 and 16";
        return false;
    }
    mask = 0;
    for (const auto& item : value) {
        if (!item.is_number_integer() || item.get<std::int64_t>() < 1 || item.get<std::int64_t>() > 16) {
            error = std::string(field) + " values must be integers between 1 and 16";
            return false;
        }
        mask |= static_cast<std::uint16_t>(1u << (item.get<int>() - 1));
    }
    return true;
}

Json encodeMask(std::uint16_t mask) {
    Json result = Json::array();
    for (int i = 0; i < 16; ++i)
        if ((mask >> i) & 1u) result.push_back(i + 1);
    return result;
}

bool decodeOptionalNumber(const Json& value, std::optional<std::uint8_t>& result, const char* field,
                          std::string& error) {
    if (value.is_null()) { result.reset(); return true; }
    if (!value.is_number_integer() || value.get<std::int64_t>() < 1 || value.get<std::int64_t>() > 16) {
        error = std::string(field) + " must be null or an integer between 1 and 16";
        return false;
    }
    result = static_cast<std::uint8_t>(value.get<int>() - 1);
    return true;
}

bool decodeRoute(const Json& item, MidiRoute& route, std::string& error) {
    static const std::set<std::string> allowed{"id", "source", "destination", "enabled",
        "channels", "messages", "outputChannel", "transpose", "reclock"};
    if (!item.is_object()) { error = "Each MIDI route must be an object"; return false; }
    for (const auto& entry : item.items())
        if (!allowed.count(entry.key())) { error = "Unknown MIDI route field " + entry.key(); return false; }
    for (const char* field : {"id", "source", "destination"})
        if (!item.contains(field) || !item[field].is_string()) {
            error = std::string("MIDI route field ") + field + " must be a string";
            return false;
        }
    route = MidiRoute{};
    route.id = item["id"].get<std::string>();
    route.sourcePortId = item["source"].get<std::string>();
    route.destinationPortId = item["destination"].get<std::string>();
    if (item.contains("enabled")) {
        if (!item["enabled"].is_boolean()) { error = "enabled must be a boolean"; return false; }
        route.enabled = item["enabled"].get<bool>();
    }
    if (item.contains("channels") && !decodeMask(item["channels"], route.channelMask, "channels", error)) return false;
    if (item.contains("messages")) {
        const auto& messages = item["messages"];
        if (!messages.is_array() || messages.empty()) { error = "messages must be a non-empty array"; return false; }
        route.messages = 0;
        for (const auto& name : messages) {
            bool known = false;
            if (name.is_string())
                for (const auto& category : kCategories)
                    if (name.get<std::string>() == category.name) { route.messages |= category.bit; known = true; }
            if (!known) { error = "Unknown MIDI message category " + name.dump(); return false; }
        }
    }
    if (item.contains("outputChannel") && !decodeOptionalNumber(item["outputChannel"], route.outputChannel, "outputChannel", error))
        return false;
    if (item.contains("reclock")) {
        if (!item["reclock"].is_boolean()) { error = "reclock must be a boolean"; return false; }
        route.reclock = item["reclock"].get<bool>();
    }
    if (item.contains("transpose")) {
        if (!item["transpose"].is_number_integer() || item["transpose"].get<std::int64_t>() < -127 ||
            item["transpose"].get<std::int64_t>() > 127) {
            error = "transpose must be an integer between -127 and 127";
            return false;
        }
        route.transpose = item["transpose"].get<int>();
    }
    return true;
}

Json encodeRoute(const MidiRoute& route) {
    Json messages = Json::array();
    for (const auto& category : kCategories)
        if (route.messages & category.bit) messages.push_back(category.name);
    return Json{{"id", route.id}, {"source", route.sourcePortId}, {"destination", route.destinationPortId},
        {"enabled", route.enabled},
        {"channels", encodeMask(route.channelMask)}, {"messages", std::move(messages)},
        {"outputChannel", route.outputChannel ? Json(*route.outputChannel + 1) : Json(nullptr)},
        {"transpose", route.transpose}, {"reclock", route.reclock}};
}

std::int64_t nowUnixMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t nowSteadyNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr std::int64_t kClockTimeoutNs = 1'000'000'000;  // no tick for 1 s: clock stopped

enum Transport : int { TransportNone = 0, TransportStart, TransportContinue, TransportStop };

bool isClockTick(std::uint32_t word0) noexcept {
    return midi_ump::Type(word0) == 0x1 && ((word0 >> 16) & 0xFF) == 0xF8;
}
}

struct MidiPatchbay::PortCounters {
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::int64_t> lastReceivedUnixMs{0};
    std::atomic<std::int64_t> lastSentUnixMs{0};
    std::atomic<std::uint64_t> activity{0};
    std::atomic<std::int64_t> lastActivityUnixMs{0};
    std::atomic<std::uint64_t> clockTicks{0};
    std::atomic<std::int64_t> lastClockUnixMs{0};
    std::atomic<std::int64_t> lastClockSteadyNs{0};
    // Last tick arrivals in sound card seconds; the tempo is a linear fit over
    // them (two beats), which removes most arrival jitter.
    static constexpr std::size_t kTempoHistory = 48;
    std::array<std::atomic<double>, kTempoHistory> tickCardSeconds{};
    std::atomic<std::uint64_t> tickRun{0};  // ticks since the clock (re)started

    // Reader side (any thread): tempo from the recorded ticks, 0 if unknown.
    double MeasuredBpm() const noexcept {
        const auto run = tickRun.load(std::memory_order_acquire);
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(run, kTempoHistory));
        if (n < 3) return 0;
        const auto first = run - n;
        const double origin = tickCardSeconds[first % kTempoHistory].load(std::memory_order_relaxed);
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(i);
            const double y = tickCardSeconds[(first + i) % kTempoHistory].load(std::memory_order_relaxed) - origin;
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double denominator = static_cast<double>(n) * sxx - sx * sx;
        const double period = denominator > 0 ? (static_cast<double>(n) * sxy - sx * sy) / denominator : 0;
        return period > 0 ? 60.0 / (24.0 * period) : 0;  // 24 ticks per quarter note
    }
    std::atomic<std::int64_t> lastClockSentSteadyNs{0};
    std::atomic<int> transport{TransportNone};
    std::atomic<std::int64_t> transportUnixMs{0};
    std::atomic<std::uint64_t> activeSensing{0};
    std::atomic<std::int64_t> lastActiveSensingUnixMs{0};

    // One port's messages arrive in order on one receive path, so relaxed
    // atomics are enough; a rare race only skews one tempo sample.
    // cardSeconds: arrival time on the sound card timebase, so the measured
    // tempo is the tempo the audio hears.
    void Observe(std::uint32_t word0, std::int64_t steadyNs, std::int64_t unixMs, double cardSeconds) noexcept {
        received.fetch_add(1, std::memory_order_relaxed);
        lastReceivedUnixMs.store(unixMs, std::memory_order_relaxed);
        const auto status = midi_ump::Type(word0) == 0x1 ? static_cast<int>((word0 >> 16) & 0xFF) : 0;
        if (status == 0xF8) {
            clockTicks.fetch_add(1, std::memory_order_relaxed);
            lastClockUnixMs.store(unixMs, std::memory_order_relaxed);
            const auto previous = lastClockSteadyNs.exchange(steadyNs, std::memory_order_relaxed);
            auto run = tickRun.load(std::memory_order_relaxed);
            if (previous == 0 || steadyNs - previous > kClockTimeoutNs) run = 0;  // (re)starting: tempo unknown
            tickCardSeconds[run % kTempoHistory].store(cardSeconds, std::memory_order_relaxed);
            tickRun.store(run + 1, std::memory_order_release);
            return;
        }
        if (status == 0xFE) {
            activeSensing.fetch_add(1, std::memory_order_relaxed);
            lastActiveSensingUnixMs.store(unixMs, std::memory_order_relaxed);
            return;
        }
        if (status == 0xFA || status == 0xFB || status == 0xFC) {
            transport.store(status == 0xFA ? TransportStart : status == 0xFB ? TransportContinue : TransportStop,
                std::memory_order_relaxed);
            transportUnixMs.store(unixMs, std::memory_order_relaxed);
        }
        activity.fetch_add(1, std::memory_order_relaxed);
        lastActivityUnixMs.store(unixMs, std::memory_order_relaxed);
    }
};

// Re-emits an incoming clock on a regular grid in sound card time. A linear
// fit over the last ticks predicts where each tick belongs; the tick is sent
// at that point (plus a small margin) instead of when it happened to arrive.
// One tick out for each tick in: the tempo and the beat position of the source
// are kept, only Windows and transport jitter is removed.
struct MidiPatchbay::Reclocker {
    static constexpr std::size_t kHistory = 48;      // two beats
    static constexpr std::size_t kMinimumHistory = 8;
    static constexpr double kMarginSeconds = 0.002;  // scheduling margin
    static constexpr double kGapSeconds = 1.0;       // longer pause: the clock restarted

    std::mutex mutex;
    std::array<double, kHistory> arrivals{};
    std::uint64_t count = 0;
    double lastArrival = -1;
    double lastOutput = -1;
    std::atomic<std::uint64_t> emitted{0};

    void Reset() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        count = 0;
        lastArrival = lastOutput = -1;
    }

    // arrival and result in card seconds.
    double OnTick(double arrival) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (lastArrival >= 0 && (arrival - lastArrival > kGapSeconds || arrival < lastArrival)) {
            count = 0;
            lastOutput = -1;
        }
        double output = arrival;
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(count, kHistory));
        if (n >= kMinimumHistory) {
            const auto first = count - n;
            const double origin = arrivals[first % kHistory];
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const double x = static_cast<double>(i), y = arrivals[(first + i) % kHistory] - origin;
                sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            const double denominator = n * sxx - sx * sx;
            const double period = denominator > 0 ? (n * sxy - sx * sy) / denominator : 0;
            const double intercept = (sy - period * sx) / static_cast<double>(n);
            if (period > 0) {
                const double predicted = origin + intercept + period * static_cast<double>(n);
                output = std::max(arrival, predicted + kMarginSeconds);
                if (lastOutput >= 0) output = std::max(output, lastOutput + period / 2);
            }
        }
        arrivals[count % kHistory] = arrival;
        ++count;
        lastArrival = arrival;
        lastOutput = output;
        emitted.fetch_add(1, std::memory_order_relaxed);
        return output;
    }
};

namespace {
template <class Counters>
struct PortTarget {
    std::uint64_t cookie = 0;
    std::uint8_t group = 0;
    bool destination = false;
    Counters* counters = nullptr;
};

// Sends to the destination port's connection, on that port's group.
template <class Counters>
class BackendSink final : public MidiOutputSink {
public:
    // atHostNs: 0 sends now, otherwise schedules for that host time.
    BackendSink(MidiBackend& backend, const std::vector<PortTarget<Counters>>& targets,
                std::int64_t steadyNs, std::int64_t unixMs, std::int64_t atHostNs = 0)
        : backend_(backend), targets_(targets), steadyNs_(steadyNs), unixMs_(unixMs), atHostNs_(atHostNs) {}
    void Send(std::size_t destinationIndex, const std::uint32_t* words, std::size_t count) noexcept override {
        if (destinationIndex >= targets_.size() || !targets_[destinationIndex].destination || count == 0 || count > 4)
            return;
        std::uint32_t message[4];
        for (std::size_t i = 0; i < count; ++i) message[i] = words[i];
        message[0] = midi_ump::WithGroup(message[0], targets_[destinationIndex].group);
        const auto cookie = targets_[destinationIndex].cookie;
        const bool sent = atHostNs_ > 0 ? backend_.SendAt(cookie, message, count, atHostNs_)
                                        : backend_.Send(cookie, message, count);
        if (sent && targets_[destinationIndex].counters) {
            targets_[destinationIndex].counters->sent.fetch_add(1, std::memory_order_relaxed);
            targets_[destinationIndex].counters->lastSentUnixMs.store(unixMs_, std::memory_order_relaxed);
            // Remembered to tell a real clock source from clock the engine
            // sends to an application that echoes it back (MIDI thru).
            if (isClockTick(message[0]))
                targets_[destinationIndex].counters->lastClockSentSteadyNs.store(steadyNs_, std::memory_order_relaxed);
        }
    }
private:
    MidiBackend& backend_;
    const std::vector<PortTarget<Counters>>& targets_;
    std::int64_t steadyNs_;
    std::int64_t unixMs_;
    std::int64_t atHostNs_;
};

std::uint64_t sourceKey(std::uint64_t cookie, std::uint8_t group) {
    return (cookie << 4) | (group & 0x0F);
}

std::string endpointKey(const std::string& endpointId) { return "endpoint:" + endpointId; }

std::string virtualKey(const std::string& id, bool toEngine) {
    return std::string("midi:virtual:") + id + (toEngine ? ":in" : ":out");
}

struct PresentPort {
    std::string endpointId;
    std::uint8_t group = 0;
    bool source = false;
    bool destination = false;
};
}

namespace midi_patchbay_json {
bool Decode(const std::string& text, MidiPatchbayConfiguration& configuration, std::string& error) {
    Json body;
    try {
        body = Json::parse(text);
    } catch (const Json::exception&) {
        error = "MIDI configuration is not valid JSON";
        return false;
    }
    if (!body.is_object()) { error = "MIDI configuration must be an object"; return false; }
    for (const auto& entry : body.items())
        if (entry.key() != "version" && entry.key() != "virtualPorts" && entry.key() != "routes" &&
            entry.key() != "engineClock") {
            error = "Unknown MIDI configuration field " + entry.key();
            return false;
        }
    if (body.contains("version") && body["version"] != 1) { error = "Unsupported MIDI configuration version"; return false; }
    if (!body.contains("virtualPorts") || !body["virtualPorts"].is_array() ||
        !body.contains("routes") || !body["routes"].is_array()) {
        error = "MIDI configuration requires virtualPorts and routes arrays";
        return false;
    }
    MidiPatchbayConfiguration decoded;
    for (const auto& item : body["virtualPorts"]) {
        bool valid = item.is_object() && item.contains("id") && item["id"].is_string() &&
            item.contains("name") && item["name"].is_string();
        if (valid)
            for (const auto& entry : item.items())
                if (entry.key() != "id" && entry.key() != "name" &&
                    !((entry.key() == "input" || entry.key() == "output") && entry.value().is_boolean()))
                    valid = false;
        if (!valid) {
            error = "Each virtual MIDI port requires id and name strings and accepts input and output booleans";
            return false;
        }
        decoded.virtualPorts.push_back({item["id"].get<std::string>(), item["name"].get<std::string>(),
            item.value("input", true), item.value("output", true)});
    }
    for (const auto& item : body["routes"]) {
        MidiRoute route;
        if (!decodeRoute(item, route, error)) return false;
        decoded.routes.push_back(std::move(route));
    }
    if (body.contains("engineClock")) {
        const auto& clock = body["engineClock"];
        if (!clock.is_object() || clock.size() != 1 || !clock.contains("bpm") || !clock["bpm"].is_number()) {
            error = "engineClock requires only a numeric bpm";
            return false;
        }
        decoded.engineClockBpm = clock["bpm"].get<double>();
    }
    configuration = std::move(decoded);
    return true;
}

std::string Encode(const MidiPatchbayConfiguration& configuration) {
    Json ports = Json::array();
    for (const auto& port : configuration.virtualPorts)
        ports.push_back({{"id", port.id}, {"name", port.name}, {"input", port.input}, {"output", port.output}});
    Json routes = Json::array();
    for (const auto& route : configuration.routes) routes.push_back(encodeRoute(route));
    return Json{{"version", 1}, {"virtualPorts", std::move(ports)}, {"routes", std::move(routes)},
        {"engineClock", {{"bpm", configuration.engineClockBpm}}}}.dump(2);
}
}

struct MidiPatchbay::Runtime {
    std::unique_ptr<MidiRoutingGraph> graph;
    std::unordered_map<std::uint64_t, std::size_t> sourceIndices;  // by sourceKey(cookie, group)
    std::vector<PortTarget<PortCounters>> targets;                  // by port index
    std::vector<std::shared_ptr<PortCounters>> counters;            // by port index
    std::vector<std::string> routeIds;
    std::size_t engineClockIndex = 0;
    struct ReclockTarget { std::size_t destination; std::shared_ptr<Reclocker> reclocker; };
    std::vector<std::vector<ReclockTarget>> reclocks;               // by source port index
};

bool MidiPatchbay::ParsePortId(const std::string& portId, std::uint8_t& group, std::string& endpointId) {
    if (!startsWith(portId, kPortPrefix)) return false;
    const auto start = std::char_traits<char>::length(kPortPrefix);
    const auto colon = portId.find(':', start);
    if (colon == std::string::npos || colon == start || colon - start > 2 || colon + 1 >= portId.size()) return false;
    int value = 0;
    for (auto i = start; i < colon; ++i) {
        if (portId[i] < '0' || portId[i] > '9') return false;
        value = value * 10 + (portId[i] - '0');
    }
    if (value < 1 || value > 16 || (colon - start == 2 && portId[start] == '0')) return false;
    group = static_cast<std::uint8_t>(value - 1);
    endpointId = portId.substr(colon + 1);
    return true;
}

std::string MidiPatchbay::PortId(std::uint8_t group, const std::string& endpointId) {
    return kPortPrefix + std::to_string(group + 1) + ":" + endpointId;
}

MidiPatchbay::MidiPatchbay(std::unique_ptr<MidiBackend> backend, std::filesystem::path storePath,
    std::string unavailableReason)
    : backend_(std::move(backend)), storePath_(std::move(storePath)),
      unavailableReason_(std::move(unavailableReason)), steadyNs_(nowSteadyNs), unixMs_(nowUnixMs) {
    if (backend_)
        backend_->SetReceiveHandler([this](std::uint64_t cookie, const std::uint32_t* words, std::size_t count) {
            OnReceive(cookie, words, count);
        });
    else if (unavailableReason_.empty())
        unavailableReason_ = "No MIDI backend is available";
}

MidiPatchbay::~MidiPatchbay() { Stop(); }

void MidiPatchbay::SetTimeSourcesForTesting(TimeSource steadyNs, TimeSource unixMs) noexcept {
    steadyNs_ = steadyNs;
    unixMs_ = unixMs;
    clockThreadEnabled_ = false;
}

bool MidiPatchbay::Validate(const MidiPatchbayConfiguration& configuration, std::string& error) {
    if (configuration.virtualPorts.size() > kMaximumVirtualPorts) {
        error = "At most 64 virtual MIDI ports are supported";
        return false;
    }
    if (configuration.routes.size() > kMaximumRoutes) {
        error = "At most 512 MIDI routes are supported";
        return false;
    }
    std::set<std::string> virtualIds, names, routeIds;
    for (const auto& port : configuration.virtualPorts) {
        if (!validVirtualId(port.id)) {
            error = "Virtual MIDI port ids use 1 to 24 lowercase letters, digits or hyphens";
            return false;
        }
        if (!port.input && !port.output) {
            error = "Virtual MIDI port " + port.id + " needs its In port, its Out port or both";
            return false;
        }
        if (!validDisplayName(port.name)) {
            error = "Virtual MIDI port names use 1 to 24 printable ASCII characters without edge spaces";
            return false;
        }
        if (!virtualIds.insert(port.id).second || !names.insert(port.name).second) {
            error = "Virtual MIDI port ids and names must be unique";
            return false;
        }
    }
    if (!(configuration.engineClockBpm >= 20.0 && configuration.engineClockBpm <= 300.0)) {
        error = "The engine clock tempo must be between 20 and 300 BPM";
        return false;
    }
    for (const auto& route : configuration.routes) {
        if (!MidiRoutingGraph::ValidateRoute(route, error)) return false;
        if (!routeIds.insert(route.id).second) { error = "Duplicate MIDI route id " + route.id; return false; }
        if (route.destinationPortId == kEngineClockPort) {
            error = "MIDI route " + route.id + " cannot send to the engine clock";
            return false;
        }
        for (const auto* portId : {&route.sourcePortId, &route.destinationPortId}) {
            if (*portId == kEngineClockPort) continue;
            if (startsWith(*portId, kVirtualPrefix)) {
                if (!virtualIds.count(portId->substr(std::char_traits<char>::length(kVirtualPrefix)))) {
                    error = "MIDI route " + route.id + " references undefined virtual port " + *portId;
                    return false;
                }
            } else {
                std::uint8_t group = 0;
                std::string endpointId;
                if (!ParsePortId(*portId, group, endpointId)) {
                    error = "MIDI route " + route.id + " port ids must be midi:virtual:<id>, midi:port:<1-16>:<endpoint id> or midi:clock:engine";
                    return false;
                }
            }
        }
    }
    return true;
}

MidiApplyResult MidiPatchbay::Start() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (!backend_) return {false, unavailableReason_};
    endpoints_.clear();
    enumerationError_.clear();
    if (!backend_->Enumerate(endpoints_, enumerationError_)) endpoints_.clear();

    MidiPatchbayConfiguration loaded;
    std::error_code code;
    if (!storePath_.empty() && std::filesystem::exists(storePath_, code)) {
        const auto size = std::filesystem::file_size(storePath_, code);
        if (code || size > kMaximumStoreBytes) return {false, "The MIDI patchbay file has an invalid size"};
        std::ifstream input(storePath_, std::ios::binary);
        std::stringstream buffer;
        buffer << input.rdbuf();
        std::string error;
        if (!input.good() && !input.eof()) return {false, "Unable to read the MIDI patchbay file"};
        if (!midi_patchbay_json::Decode(buffer.str(), loaded, error) || !Validate(loaded, error))
            return {false, "Ignored invalid MIDI patchbay file: " + error};
    }
    auto result = ApplyLocked(loaded);
    if (!result.success) configuration_ = loaded;  // keep it visible so it can be corrected
    if (clockThreadEnabled_ && !clockThread_.joinable()) {
        { std::lock_guard<std::mutex> wakeLock(clockThreadMutex_); clockThreadStop_ = false; }
        clockThread_ = std::thread([this] { ClockThread(); });
    }
    return result;
}

void MidiPatchbay::ClockThread() {
    // 2 ms wake-ups with a 20 ms look-ahead: ticks are scheduled ahead with a
    // timestamp, so their timing does not depend on when this thread wakes.
    std::unique_lock<std::mutex> lock(clockThreadMutex_);
    while (!clockThreadStop_) {
        lock.unlock();
        PumpClock(steadyNs_());
        lock.lock();
        clockThreadWake_.wait_for(lock, std::chrono::milliseconds(2), [this] { return clockThreadStop_; });
    }
}

void MidiPatchbay::Stop() noexcept {
    if (clockThread_.joinable()) {
        { std::lock_guard<std::mutex> wakeLock(clockThreadMutex_); clockThreadStop_ = true; }
        clockThreadWake_.notify_all();
        clockThread_.join();
    }
    { std::lock_guard<std::mutex> clockLock(clockMutex_); clockRunning_ = false; }
    std::lock_guard<std::mutex> lock(controlMutex_);
    {
        std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
        runtime_.reset();
    }
    if (backend_)
        for (const auto& entry : connections_) backend_->Close(entry.second.cookie);
    connections_.clear();
}

MidiApplyResult MidiPatchbay::Apply(const MidiPatchbayConfiguration& configuration, bool persist) {
    std::string error;
    if (!Validate(configuration, error)) return {false, error};
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (!backend_) return {false, unavailableReason_};
    auto result = ApplyLocked(configuration);
    if (result.success && persist && !Save(configuration, error))
        return {false, "MIDI routes are active but could not be saved: " + error};
    return result;
}

void MidiPatchbay::Refresh() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (!backend_) return;
    std::vector<MidiEndpointInfo> endpoints;
    std::string error;
    if (!backend_->Enumerate(endpoints, error)) {
        enumerationError_ = error;
        return;
    }
    endpoints_ = std::move(endpoints);
    enumerationError_.clear();
    ApplyLocked(configuration_);
}

MidiApplyResult MidiPatchbay::ApplyLocked(const MidiPatchbayConfiguration& configuration) {
    std::string error;
    if (!Validate(configuration, error)) return {false, error};

    std::map<std::string, PresentPort> present;
    for (const auto& endpoint : endpoints_)
        for (const auto& port : endpoint.ports)
            present[PortId(port.group, endpoint.endpointId)] = {endpoint.endpointId, port.group, port.source, port.destination};
    std::map<std::string, const MidiVirtualPortConfig*> virtualPorts;
    for (const auto& port : configuration.virtualPorts) virtualPorts[kVirtualPrefix + port.id] = &port;
    const auto usable = [&](const std::string& portId, bool asSource) {
        if (portId == kEngineClockPort) return asSource;
        const auto application = virtualPorts.find(portId);
        if (application != virtualPorts.end()) return asSource ? application->second->input : application->second->output;
        const auto found = present.find(portId);
        return found != present.end() && (asSource ? found->second.source : found->second.destination);
    };

    // A route is live only when its source can send and its destination can
    // receive; otherwise it stays configured but inactive.
    auto routes = configuration.routes;
    std::set<std::string> wantedEndpoints;
    for (auto& route : routes) {
        if (route.enabled && (!usable(route.sourcePortId, true) || !usable(route.destinationPortId, false)))
            route.enabled = false;
        if (!route.enabled) continue;
        for (const auto* portId : {&route.sourcePortId, &route.destinationPortId})
            if (!startsWith(*portId, kVirtualPrefix) && *portId != kEngineClockPort)
                wantedEndpoints.insert(present.at(*portId).endpointId);
    }

    std::map<std::string, OpenConnection> next;
    std::vector<std::uint64_t> created;
    std::map<std::string, std::string> errors;
    const auto rollback = [&] { for (const auto cookie : created) backend_->Close(cookie); };

    // Application ports: each enabled direction is its own loopback. Unchanged
    // ones stay open; turning one direction on or off leaves the other alone.
    for (const auto& port : configuration.virtualPorts) {
        for (const auto direction : {MidiVirtualDirection::ToEngine, MidiVirtualDirection::FromEngine}) {
            const bool toEngine = direction == MidiVirtualDirection::ToEngine;
            if (!(toEngine ? port.input : port.output)) continue;
            const auto key = virtualKey(port.id, toEngine);
            const auto existing = connections_.find(key);
            if (existing != connections_.end() && existing->second.displayName == port.name) {
                next[key] = existing->second;
                continue;
            }
            OpenConnection connection;
            connection.cookie = nextCookie_++;
            connection.displayName = port.name;
            if (!backend_->CreateVirtualPort(connection.cookie, port.id, port.name, direction,
                    connection.endpointId, error)) {
                rollback();
                return {false, "Unable to create MIDI port " + port.name + (toEngine ? " In: " : " Out: ") + error};
            }
            created.push_back(connection.cookie);
            next[key] = connection;
        }
    }
    // Endpoints: one connection carries all of its MIDI 1.0 ports. A failure
    // only deactivates the routes using it.
    for (const auto& endpointId : wantedEndpoints) {
        const auto key = endpointKey(endpointId);
        const auto existing = connections_.find(key);
        if (existing != connections_.end()) { next[key] = existing->second; continue; }
        OpenConnection connection;
        connection.cookie = nextCookie_++;
        if (!backend_->OpenEndpoint(connection.cookie, endpointId, error)) {
            errors[endpointId] = error;
            continue;
        }
        created.push_back(connection.cookie);
        next[key] = connection;
    }

    auto runtime = std::make_shared<Runtime>();
    std::vector<std::string> ports;
    // The engine clock is a source only; nothing is received from it.
    runtime->engineClockIndex = ports.size();
    runtime->targets.push_back({0, 0, false, nullptr});
    ports.push_back(kEngineClockPort);
    for (const auto& port : configuration.virtualPorts) {
        // A MIDI 1.0 loopback carries one port; accept whatever group it uses.
        if (port.input) {
            const auto cookie = next.at(virtualKey(port.id, true)).cookie;
            for (std::uint8_t group = 0; group < 16; ++group)
                runtime->sourceIndices.emplace(sourceKey(cookie, group), ports.size());
        }
        runtime->targets.push_back({port.output ? next.at(virtualKey(port.id, false)).cookie : 0, 0, port.output, nullptr});
        ports.push_back(kVirtualPrefix + port.id);
    }
    for (const auto& entry : present) {
        const auto connection = next.find(endpointKey(entry.second.endpointId));
        if (connection == next.end()) continue;
        if (entry.second.source)
            runtime->sourceIndices.emplace(sourceKey(connection->second.cookie, entry.second.group), ports.size());
        runtime->targets.push_back({connection->second.cookie, entry.second.group, entry.second.destination, nullptr});
        ports.push_back(entry.first);
    }
    for (auto& route : routes)
        if (route.enabled && (std::find(ports.begin(), ports.end(), route.sourcePortId) == ports.end() ||
                              std::find(ports.begin(), ports.end(), route.destinationPortId) == ports.end()))
            route.enabled = false;
    for (std::size_t i = 0; i < ports.size(); ++i) {
        auto& counters = counters_[ports[i]];
        if (!counters) counters = std::make_shared<PortCounters>();
        runtime->counters.push_back(counters);
        runtime->targets[i].counters = counters.get();
    }
    runtime->graph = MidiRoutingGraph::Create(ports, routes, error);
    if (!runtime->graph) {
        rollback();
        return {false, error};
    }
    for (const auto& route : configuration.routes) runtime->routeIds.push_back(route.id);
    // Reclocked routes: their clock ticks are re-emitted on the card grid.
    runtime->reclocks.resize(ports.size());
    const auto indexOf = [&](const std::string& portId) {
        return static_cast<std::size_t>(std::find(ports.begin(), ports.end(), portId) - ports.begin());
    };
    std::set<std::string> reclockIds;
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const auto& route = routes[i];
        if (!route.enabled || !route.reclock || !(route.messages & midi_ump::SystemRealtime) ||
            route.sourcePortId == kEngineClockPort) continue;  // the engine clock is already on the card grid
        auto& reclocker = reclockers_[route.id];
        if (!reclocker) reclocker = std::make_shared<Reclocker>();
        runtime->reclocks[indexOf(route.sourcePortId)].push_back({indexOf(route.destinationPortId), reclocker});
        reclockIds.insert(route.id);
    }
    for (auto it = reclockers_.begin(); it != reclockers_.end();)
        it = reclockIds.count(it->first) ? std::next(it) : reclockers_.erase(it);

    {
        std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
        runtime_ = std::move(runtime);
    }
    for (const auto& entry : connections_) {
        const auto kept = next.find(entry.first);
        if (kept == next.end() || kept->second.cookie != entry.second.cookie) backend_->Close(entry.second.cookie);
    }
    connections_ = std::move(next);
    endpointErrors_ = std::move(errors);
    configuration_ = configuration;
    { std::lock_guard<std::mutex> clockLock(clockMutex_); clockBpm_ = configuration.engineClockBpm; }
    return {true, {}};
}

void MidiPatchbay::OnReceive(std::uint64_t cookie, const std::uint32_t* words, std::size_t count) noexcept {
    std::shared_ptr<const Runtime> runtime;
    {
        std::lock_guard<std::mutex> lock(runtimeMutex_);
        runtime = runtime_;
    }
    if (!runtime || !backend_ || !words) return;
    std::size_t offset = 0;
    while (offset < count) {
        const auto length = midi_ump::WordCount(words[offset]);
        if (offset + length > count) return;
        // MIDI 1.0 only: anything else (MIDI 2.0, utility, stream) is dropped.
        if (midi_ump::Category(words[offset]) != 0) Route(*runtime, cookie, words + offset, length);
        offset += length;
    }
}

void MidiPatchbay::Route(const Runtime& runtime, std::uint64_t cookie, const std::uint32_t* words,
                         std::size_t count) noexcept {
    const auto source = runtime.sourceIndices.find(sourceKey(cookie, midi_ump::Group(words[0])));
    if (source == runtime.sourceIndices.end()) return;
    // Every message from a port is observed, routed or not, on the card timebase.
    const auto steady = steadyNs_(), wall = unixMs_();
    const auto mapping = timebase_->Current();
    const double card = mapping.CardSeconds(steady);
    runtime.counters[source->second]->Observe(words[0], steady, wall, card);
    BackendSink<PortCounters> sink(*backend_, runtime.targets, steady, wall);
    runtime.graph->Process(source->second, words, count, sink);

    const auto& reclocks = runtime.reclocks[source->second];
    if (reclocks.empty() || midi_ump::Type(words[0]) != 0x1) return;
    if (!mapping.cardLocked) {
        // No master clock to reclock on: pass ticks through as they arrive.
        if (((words[0] >> 16) & 0xFF) == 0xF8)
            for (const auto& target : reclocks) sink.Send(target.destination, words, 1);
        return;
    }
    const auto status = (words[0] >> 16) & 0xFF;
    if (status == 0xFA || status == 0xFB || status == 0xFC) {
        for (const auto& target : reclocks) target.reclocker->Reset();
        return;
    }
    if (status != 0xF8) return;
    for (const auto& target : reclocks) {
        const auto at = mapping.HostNs(target.reclocker->OnTick(card));
        BackendSink<PortCounters> timed(*backend_, runtime.targets, steady, wall, at > steady ? at : 0);
        timed.Send(target.destination, words, 1);
    }
}

void MidiPatchbay::EmitEngineClock(const Runtime& runtime, std::uint32_t word, std::int64_t atHostNs) noexcept {
    BackendSink<PortCounters> sink(*backend_, runtime.targets, steadyNs_(), unixMs_(), atHostNs);
    runtime.graph->Process(runtime.engineClockIndex, &word, 1, sink);
}

void MidiPatchbay::PumpClock(std::int64_t hostNs) {
    constexpr std::int64_t kLookaheadNs = 20'000'000;
    const auto mapping = timebase_->Update();
    std::shared_ptr<const Runtime> runtime;
    {
        std::lock_guard<std::mutex> lock(runtimeMutex_);
        runtime = runtime_;
    }
    std::lock_guard<std::mutex> clockLock(clockMutex_);
    if (!clockRunning_ || !runtime || !backend_) return;
    // Ticks are only ever placed on the master clock's samples. Without a
    // running master the clock waits; Windows timers are never used instead.
    clockWaiting_ = !mapping.cardLocked;
    if (clockWaiting_) return;
    const double now = mapping.CardSeconds(hostNs);
    if (mapping.generation != clockGeneration_) {  // the master (re)started: new sample count
        clockGeneration_ = mapping.generation;
        nextTickCardSeconds_ = now;
    }
    const double horizon = mapping.CardSeconds(hostNs + kLookaheadNs);
    const double period = 60.0 / (clockBpm_ * 24.0);  // 24 ticks per quarter note, in card seconds
    if (nextTickCardSeconds_ < now - 0.05) nextTickCardSeconds_ = now;  // resume after a stall or a card restart
    while (nextTickCardSeconds_ <= horizon) {
        const auto at = mapping.HostNs(nextTickCardSeconds_);
        EmitEngineClock(*runtime, 0x10F80000u, at > hostNs ? at : 0);
        ++clockTicks_;
        nextTickCardSeconds_ += period;
    }
}

MidiApplyResult MidiPatchbay::SetEngineClock(std::optional<double> bpm, const std::string& transport) {
    if (bpm && !(*bpm >= 20.0 && *bpm <= 300.0)) return {false, "The engine clock tempo must be between 20 and 300 BPM"};
    if (!transport.empty() && transport != "start" && transport != "stop" && transport != "continue")
        return {false, "transport must be start, stop or continue"};
    if ((transport == "start" || transport == "continue") && !timebase_->Current().cardLocked)
        return {false, "No master clock is running: start audio on an ASIO driver or on the software clock"};
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (!backend_) return {false, unavailableReason_};
    if (bpm) {
        auto next = configuration_;
        next.engineClockBpm = *bpm;
        std::string error;
        if (!Save(next, error)) return {false, "The tempo could not be saved: " + error};
        configuration_ = next;
    }
    std::shared_ptr<const Runtime> runtime;
    {
        std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
        runtime = runtime_;
    }
    const auto steady = steadyNs_();
    {
        std::lock_guard<std::mutex> clockLock(clockMutex_);
        if (bpm) clockBpm_ = *bpm;
        if (!transport.empty() && runtime) {
            const std::uint32_t word = transport == "start" ? 0x10FA0000u : transport == "continue" ? 0x10FB0000u : 0x10FC0000u;
            EmitEngineClock(*runtime, word, 0);
            clockTransport_ = transport;
            clockRunning_ = transport != "stop";
            // The first tick follows start/continue by 1 ms, on the master's samples.
            if (clockRunning_) {
                const auto mapping = timebase_->Current();
                nextTickCardSeconds_ = mapping.CardSeconds(steady) + 0.001;
                clockGeneration_ = mapping.generation;
                clockWaiting_ = false;
            }
        }
    }
    clockThreadWake_.notify_all();
    return {true, {}};
}

bool MidiPatchbay::Save(const MidiPatchbayConfiguration& configuration, std::string& error) const {
    if (storePath_.empty()) { error = "No MIDI patchbay file location"; return false; }
    std::error_code code;
    std::filesystem::create_directories(storePath_.parent_path(), code);
    if (code) { error = "Unable to create the configuration directory"; return false; }
    auto temporary = storePath_;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << midi_patchbay_json::Encode(configuration);
        output.flush();
        if (!output) { error = "Unable to write the MIDI patchbay file"; return false; }
    }
    std::filesystem::rename(temporary, storePath_, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        error = "Unable to replace the MIDI patchbay file";
        return false;
    }
    return true;
}

MidiPatchbaySnapshot MidiPatchbay::Snapshot() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    MidiPatchbaySnapshot snapshot;
    snapshot.available = backend_ != nullptr;
    snapshot.backend = backend_ ? backend_->Name() : std::string{};
    snapshot.error = backend_ ? enumerationError_ : unavailableReason_;
    snapshot.configuration = configuration_;

    if (backend_) {
        MidiPortStatus clock;
        clock.id = kEngineClockPort;
        clock.name = "Engine clock";
        clock.kind = "engineClock";
        clock.source = true;
        clock.online = clock.open = true;
        snapshot.ports.push_back(std::move(clock));
    }
    for (const auto& port : configuration_.virtualPorts) {
        MidiPortStatus status;
        status.id = kVirtualPrefix + port.id;
        status.name = port.name;
        status.kind = "virtual";
        status.source = port.input;
        status.destination = port.output;
        const auto input = connections_.find(virtualKey(port.id, true));
        const auto output = connections_.find(virtualKey(port.id, false));
        if (input != connections_.end()) status.inputEndpointId = input->second.endpointId;
        if (output != connections_.end()) status.outputEndpointId = output->second.endpointId;
        status.online = status.open = (!port.input || input != connections_.end()) &&
                                      (!port.output || output != connections_.end());
        snapshot.ports.push_back(std::move(status));
    }
    std::set<std::string> listed;
    for (const auto& endpoint : endpoints_)
        for (const auto& port : endpoint.ports) {
            MidiPortStatus status;
            status.id = PortId(port.group, endpoint.endpointId);
            if (!listed.insert(status.id).second) continue;
            status.name = port.name.empty() ? endpoint.name : port.name;
            status.kind = "port";
            status.endpointId = endpoint.endpointId;
            status.endpointName = endpoint.name;
            status.group = port.group + 1;
            status.source = port.source;
            status.destination = port.destination;
            status.online = true;
            status.open = connections_.count(endpointKey(endpoint.endpointId)) != 0;
            const auto error = endpointErrors_.find(endpoint.endpointId);
            if (error != endpointErrors_.end()) status.error = error->second;
            snapshot.ports.push_back(std::move(status));
        }
    // Routed ports that are currently absent (unplugged device, stopped app).
    for (const auto& route : configuration_.routes)
        for (const auto* portId : {&route.sourcePortId, &route.destinationPortId}) {
            std::uint8_t group = 0;
            std::string endpointId;
            if (!ParsePortId(*portId, group, endpointId) || !listed.insert(*portId).second) continue;
            MidiPortStatus status;
            status.id = *portId;
            status.kind = "port";
            status.endpointId = endpointId;
            status.group = group + 1;
            snapshot.ports.push_back(std::move(status));
        }

    const auto now = steadyNs_();
    for (auto& status : snapshot.ports) {
        const auto counters = counters_.find(status.id);
        if (counters == counters_.end()) continue;
        status.received = counters->second->received.load(std::memory_order_relaxed);
        status.sent = counters->second->sent.load(std::memory_order_relaxed);
        status.lastReceivedUnixMs = counters->second->lastReceivedUnixMs.load(std::memory_order_relaxed);
        status.lastSentUnixMs = counters->second->lastSentUnixMs.load(std::memory_order_relaxed);
        const auto& c = *counters->second;
        status.activity = c.activity.load(std::memory_order_relaxed);
        status.lastActivityUnixMs = c.lastActivityUnixMs.load(std::memory_order_relaxed);
        status.clockTicks = c.clockTicks.load(std::memory_order_relaxed);
        status.lastClockUnixMs = c.lastClockUnixMs.load(std::memory_order_relaxed);
        const auto lastTick = c.lastClockSteadyNs.load(std::memory_order_relaxed);
        status.clockRunning = lastTick != 0 && now - lastTick <= kClockTimeoutNs;
        // Measured in sound card time.
        if (status.clockRunning) status.clockBpm = std::round(c.MeasuredBpm() * 100.0) / 100.0;
        const auto lastClockSent = c.lastClockSentSteadyNs.load(std::memory_order_relaxed);
        status.clockEchoSuspected = status.clockRunning && lastClockSent != 0 && now - lastClockSent <= kClockTimeoutNs;
        switch (c.transport.load(std::memory_order_relaxed)) {
        case TransportStart: status.transport = "start"; break;
        case TransportContinue: status.transport = "continue"; break;
        case TransportStop: status.transport = "stop"; break;
        default: break;
        }
        status.transportUnixMs = c.transportUnixMs.load(std::memory_order_relaxed);
        status.activeSensing = c.activeSensing.load(std::memory_order_relaxed);
        status.lastActiveSensingUnixMs = c.lastActiveSensingUnixMs.load(std::memory_order_relaxed);
    }

    const auto mapping = timebase_->Current();
    snapshot.timebase.cardLocked = mapping.cardLocked;
    snapshot.timebase.reference = !mapping.cardLocked ? "none"
        : mapping.source == AudioTimebaseSource::Software ? "software" : "asio";
    snapshot.timebase.nominalRate = mapping.nominalRate;
    snapshot.timebase.measuredRate = mapping.measuredRate;
    snapshot.timebase.driftPpm = mapping.spanSeconds > 0 ? mapping.driftPpm : 0;
    snapshot.timebase.spanSeconds = mapping.spanSeconds;
    {
        std::lock_guard<std::mutex> clockLock(clockMutex_);
        snapshot.engineClock.bpm = clockBpm_;
        snapshot.engineClock.running = clockRunning_;
        snapshot.engineClock.waitingForMasterClock = clockRunning_ && (clockWaiting_ || !mapping.cardLocked);
        snapshot.engineClock.ticks = clockTicks_;
        snapshot.engineClock.transport = clockTransport_;
    }
    if (backend_) {
        // The engine clock port reports the clock it generates, like a received clock.
        auto& clock = snapshot.ports.front();
        clock.clockTicks = snapshot.engineClock.ticks;
        clock.clockRunning = snapshot.engineClock.running && !snapshot.engineClock.waitingForMasterClock;
        clock.clockBpm = clock.clockRunning ? snapshot.engineClock.bpm : 0;
        clock.transport = snapshot.engineClock.transport;
    }

    std::shared_ptr<const Runtime> runtime;
    {
        std::lock_guard<std::mutex> runtimeLock(runtimeMutex_);
        runtime = runtime_;
    }
    for (std::size_t i = 0; i < configuration_.routes.size(); ++i) {
        MidiRouteStatus status;
        status.id = configuration_.routes[i].id;
        if (runtime && i < runtime->routeIds.size() && runtime->routeIds[i] == status.id) {
            status.active = runtime->graph->RouteActive(i);
            status.forwarded = runtime->graph->Forwarded(i);
            status.filtered = runtime->graph->Filtered(i);
        }
        const auto reclocker = reclockers_.find(status.id);
        if (reclocker != reclockers_.end()) status.reclocked = reclocker->second->emitted.load(std::memory_order_relaxed);
        snapshot.routes.push_back(std::move(status));
    }
    return snapshot;
}
