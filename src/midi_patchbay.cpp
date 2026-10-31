#include "midi_patchbay.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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
    if (id.empty() || id.size() > 32) return false;
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
        "channels", "messages", "outputChannel", "transpose"};
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
        {"transpose", route.transpose}};
}

struct PortTarget {
    std::uint64_t cookie = 0;
    std::uint8_t group = 0;
    bool destination = false;
};

// Sends to the destination port's connection, on that port's group.
class BackendSink final : public MidiOutputSink {
public:
    BackendSink(MidiBackend& backend, const std::vector<PortTarget>& targets)
        : backend_(backend), targets_(targets) {}
    void Send(std::size_t destinationIndex, const std::uint32_t* words, std::size_t count) noexcept override {
        if (destinationIndex >= targets_.size() || !targets_[destinationIndex].destination || count == 0 || count > 4)
            return;
        std::uint32_t message[4];
        for (std::size_t i = 0; i < count; ++i) message[i] = words[i];
        message[0] = midi_ump::WithGroup(message[0], targets_[destinationIndex].group);
        backend_.Send(targets_[destinationIndex].cookie, message, count);
    }
private:
    MidiBackend& backend_;
    const std::vector<PortTarget>& targets_;
};

std::uint64_t sourceKey(std::uint64_t cookie, std::uint8_t group) {
    return (cookie << 4) | (group & 0x0F);
}

std::string endpointKey(const std::string& endpointId) { return "endpoint:" + endpointId; }

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
        if (entry.key() != "version" && entry.key() != "virtualPorts" && entry.key() != "routes") {
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
        if (!item.is_object() || item.size() != 2 || !item.contains("id") || !item["id"].is_string() ||
            !item.contains("name") || !item["name"].is_string()) {
            error = "Each virtual MIDI port requires only id and name strings";
            return false;
        }
        decoded.virtualPorts.push_back({item["id"].get<std::string>(), item["name"].get<std::string>()});
    }
    for (const auto& item : body["routes"]) {
        MidiRoute route;
        if (!decodeRoute(item, route, error)) return false;
        decoded.routes.push_back(std::move(route));
    }
    configuration = std::move(decoded);
    return true;
}

std::string Encode(const MidiPatchbayConfiguration& configuration) {
    Json ports = Json::array();
    for (const auto& port : configuration.virtualPorts) ports.push_back({{"id", port.id}, {"name", port.name}});
    Json routes = Json::array();
    for (const auto& route : configuration.routes) routes.push_back(encodeRoute(route));
    return Json{{"version", 1}, {"virtualPorts", std::move(ports)}, {"routes", std::move(routes)}}.dump(2);
}
}

struct MidiPatchbay::Runtime {
    std::unique_ptr<MidiRoutingGraph> graph;
    std::unordered_map<std::uint64_t, std::size_t> sourceIndices;  // by sourceKey(cookie, group)
    std::vector<PortTarget> targets;                                // by port index
    std::vector<std::string> routeIds;
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
      unavailableReason_(std::move(unavailableReason)) {
    if (backend_)
        backend_->SetReceiveHandler([this](std::uint64_t cookie, const std::uint32_t* words, std::size_t count) {
            OnReceive(cookie, words, count);
        });
    else if (unavailableReason_.empty())
        unavailableReason_ = "No MIDI backend is available";
}

MidiPatchbay::~MidiPatchbay() { Stop(); }

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
            error = "Virtual MIDI port ids use 1 to 32 lowercase letters, digits or hyphens";
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
    for (const auto& route : configuration.routes) {
        if (!MidiRoutingGraph::ValidateRoute(route, error)) return false;
        if (!routeIds.insert(route.id).second) { error = "Duplicate MIDI route id " + route.id; return false; }
        for (const auto* portId : {&route.sourcePortId, &route.destinationPortId}) {
            if (startsWith(*portId, kVirtualPrefix)) {
                if (!virtualIds.count(portId->substr(std::char_traits<char>::length(kVirtualPrefix)))) {
                    error = "MIDI route " + route.id + " references undefined virtual port " + *portId;
                    return false;
                }
            } else {
                std::uint8_t group = 0;
                std::string endpointId;
                if (!ParsePortId(*portId, group, endpointId)) {
                    error = "MIDI route " + route.id + " port ids must be midi:virtual:<id> or midi:port:<1-16>:<endpoint id>";
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
    return result;
}

void MidiPatchbay::Stop() noexcept {
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
    const auto usable = [&](const std::string& portId, bool asSource) {
        if (startsWith(portId, kVirtualPrefix)) return true;
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
            if (!startsWith(*portId, kVirtualPrefix)) wantedEndpoints.insert(present.at(*portId).endpointId);
    }

    std::map<std::string, OpenConnection> next;
    std::vector<std::uint64_t> created;
    std::map<std::string, std::string> errors;
    const auto rollback = [&] { for (const auto cookie : created) backend_->Close(cookie); };

    // Virtual ports: keep unchanged ones, create new or renamed ones.
    for (const auto& port : configuration.virtualPorts) {
        const auto portId = kVirtualPrefix + port.id;
        const auto existing = connections_.find(portId);
        if (existing != connections_.end() && existing->second.displayName == port.name) {
            next[portId] = existing->second;
            continue;
        }
        OpenConnection connection;
        connection.cookie = nextCookie_++;
        connection.displayName = port.name;
        if (!backend_->CreateVirtualPort(connection.cookie, port.id, port.name, connection.clientEndpointId, error)) {
            rollback();
            return {false, "Unable to create virtual MIDI port " + port.name + ": " + error};
        }
        created.push_back(connection.cookie);
        next[portId] = connection;
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
    for (const auto& port : configuration.virtualPorts) {
        const auto& connection = next.at(kVirtualPrefix + port.id);
        runtime->sourceIndices.emplace(sourceKey(connection.cookie, 0), ports.size());
        runtime->targets.push_back({connection.cookie, 0, true});
        ports.push_back(kVirtualPrefix + port.id);
    }
    for (const auto& entry : present) {
        const auto connection = next.find(endpointKey(entry.second.endpointId));
        if (connection == next.end()) continue;
        if (entry.second.source)
            runtime->sourceIndices.emplace(sourceKey(connection->second.cookie, entry.second.group), ports.size());
        runtime->targets.push_back({connection->second.cookie, entry.second.group, entry.second.destination});
        ports.push_back(entry.first);
    }
    for (auto& route : routes)
        if (route.enabled && (std::find(ports.begin(), ports.end(), route.sourcePortId) == ports.end() ||
                              std::find(ports.begin(), ports.end(), route.destinationPortId) == ports.end()))
            route.enabled = false;
    runtime->graph = MidiRoutingGraph::Create(ports, routes, error);
    if (!runtime->graph) {
        rollback();
        return {false, error};
    }
    for (const auto& route : configuration.routes) runtime->routeIds.push_back(route.id);

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
    BackendSink sink(*backend_, runtime.targets);
    runtime.graph->Process(source->second, words, count, sink);
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

    for (const auto& port : configuration_.virtualPorts) {
        MidiPortStatus status;
        status.id = kVirtualPrefix + port.id;
        status.name = port.name;
        status.kind = "virtual";
        status.source = status.destination = true;
        const auto connection = connections_.find(status.id);
        status.online = status.open = connection != connections_.end();
        if (status.open) status.clientEndpointId = connection->second.clientEndpointId;
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
        snapshot.routes.push_back(std::move(status));
    }
    return snapshot;
}
