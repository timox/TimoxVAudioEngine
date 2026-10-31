#include "midi_patchbay.h"

#include <nlohmann/json.hpp>

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
    {"flexData", midi_ump::FlexData},
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
    static const std::set<std::string> allowed{"id", "source", "destination", "enabled", "groups",
        "channels", "messages", "outputGroup", "outputChannel", "transpose"};
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
    if (item.contains("groups") && !decodeMask(item["groups"], route.groupMask, "groups", error)) return false;
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
    if (item.contains("outputGroup") && !decodeOptionalNumber(item["outputGroup"], route.outputGroup, "outputGroup", error))
        return false;
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
        {"enabled", route.enabled}, {"groups", encodeMask(route.groupMask)},
        {"channels", encodeMask(route.channelMask)}, {"messages", std::move(messages)},
        {"outputGroup", route.outputGroup ? Json(*route.outputGroup + 1) : Json(nullptr)},
        {"outputChannel", route.outputChannel ? Json(*route.outputChannel + 1) : Json(nullptr)},
        {"transpose", route.transpose}};
}

class BackendSink final : public MidiOutputSink {
public:
    BackendSink(MidiBackend& backend, const std::vector<std::uint64_t>& cookies)
        : backend_(backend), cookies_(cookies) {}
    void Send(std::size_t destinationIndex, const std::uint32_t* words, std::size_t count) noexcept override {
        if (destinationIndex < cookies_.size()) backend_.Send(cookies_[destinationIndex], words, count);
    }
private:
    MidiBackend& backend_;
    const std::vector<std::uint64_t>& cookies_;
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
    std::unordered_map<std::uint64_t, std::size_t> sourceIndices;
    std::vector<std::uint64_t> cookies;
    std::vector<std::string> routeIds;
};

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
            } else if (!startsWith(*portId, kEndpointPrefix) ||
                       portId->size() == std::char_traits<char>::length(kEndpointPrefix)) {
                error = "MIDI route " + route.id + " port ids must start with midi:virtual: or midi:endpoint:";
                return false;
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

    std::set<std::string> presentEndpoints;
    for (const auto& endpoint : endpoints_) presentEndpoints.insert(kEndpointPrefix + endpoint.endpointId);
    std::set<std::string> wantedEndpoints;
    for (const auto& route : configuration.routes) {
        if (!route.enabled) continue;
        for (const auto* portId : {&route.sourcePortId, &route.destinationPortId})
            if (startsWith(*portId, kEndpointPrefix) && presentEndpoints.count(*portId)) wantedEndpoints.insert(*portId);
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
    // Endpoints: open on demand; a failure only deactivates its routes.
    for (const auto& portId : wantedEndpoints) {
        const auto existing = connections_.find(portId);
        if (existing != connections_.end()) { next[portId] = existing->second; continue; }
        OpenConnection connection;
        connection.cookie = nextCookie_++;
        const auto endpointId = portId.substr(std::char_traits<char>::length(kEndpointPrefix));
        if (!backend_->OpenEndpoint(connection.cookie, endpointId, error)) {
            errors[portId] = error;
            continue;
        }
        created.push_back(connection.cookie);
        next[portId] = connection;
    }

    auto runtime = std::make_shared<Runtime>();
    std::vector<std::string> ports;
    for (const auto& entry : next) {
        runtime->sourceIndices.emplace(entry.second.cookie, ports.size());
        runtime->cookies.push_back(entry.second.cookie);
        ports.push_back(entry.first);
    }
    runtime->graph = MidiRoutingGraph::Create(ports, configuration.routes, error);
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
    portErrors_ = std::move(errors);
    configuration_ = configuration;
    return {true, {}};
}

void MidiPatchbay::OnReceive(std::uint64_t cookie, const std::uint32_t* words, std::size_t count) noexcept {
    std::shared_ptr<const Runtime> runtime;
    {
        std::lock_guard<std::mutex> lock(runtimeMutex_);
        runtime = runtime_;
    }
    if (!runtime || !backend_) return;
    const auto source = runtime->sourceIndices.find(cookie);
    if (source == runtime->sourceIndices.end()) return;
    BackendSink sink(*backend_, runtime->cookies);
    runtime->graph->Process(source->second, words, count, sink);
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
        status.transport = "APP";
        const auto connection = connections_.find(status.id);
        status.online = status.open = connection != connections_.end();
        if (status.open) status.clientEndpointId = connection->second.clientEndpointId;
        snapshot.ports.push_back(std::move(status));
    }
    std::set<std::string> listed;
    for (const auto& endpoint : endpoints_) {
        MidiPortStatus status;
        status.id = kEndpointPrefix + endpoint.endpointId;
        status.name = endpoint.name;
        status.kind = "endpoint";
        status.transport = endpoint.transport;
        status.online = true;
        status.open = connections_.count(status.id) != 0;
        const auto error = portErrors_.find(status.id);
        if (error != portErrors_.end()) status.error = error->second;
        listed.insert(status.id);
        snapshot.ports.push_back(std::move(status));
    }
    // Routed endpoints that are currently absent (unplugged device, stopped app).
    for (const auto& route : configuration_.routes)
        for (const auto* portId : {&route.sourcePortId, &route.destinationPortId})
            if (startsWith(*portId, kEndpointPrefix) && listed.insert(*portId).second) {
                MidiPortStatus status;
                status.id = *portId;
                status.kind = "endpoint";
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
