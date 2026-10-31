#pragma once
#include <nlohmann/json.hpp>
#include <map>
#include <string>

namespace supervision {
using Json = nlohmann::json;
inline Json field(const Json& object, const char* key) { return object.contains(key) ? object[key] : Json(nullptr); }
inline std::string processId(const Json& pid, const Json& creation) {
    return "process:" + (creation.is_string() ? std::string{} : "runtime:") + pid.dump() +
        (creation.is_string() ? ":" + creation.get<std::string>() : "");
}
inline std::string asioAdherent(const std::string& driver, const Json& client) {
    const auto creation = field(client, "processCreationTime");
    return processId(client["pid"], creation) + (creation.is_string() ? "" : ":asio:" + driver);
}
inline std::string asioNode(const std::string& driver, const Json& client) {
    return "asio:" + driver + ":" + processId(client["pid"], field(client, "processCreationTime"));
}
}
inline nlohmann::json SupervisionDeviceTopology(nlohmann::json devices) {
    for (auto& driver : devices["virtualDrivers"])
        for (auto& client : driver["clients"])
            for (const auto* key : {"activity", "processCpuPercent", "processCpuWindowMs"}) client.erase(key);
    return devices;
}
inline nlohmann::json SupervisionClientObservations(const nlohmann::json& devices) {
    using namespace supervision;
    Json result = Json::array();
    for (const auto& driver : devices.value("virtualDrivers", Json::array()))
        for (const auto& client : driver.value("clients", Json::array())) {
            const auto creation = field(client, "processCreationTime");
            result.push_back({{"driverId", driver["id"]}, {"processId", client["pid"]},
                {"processCreationTime", creation}, {"nodeId", asioNode(driver["id"], client)},
                {"adherentId", asioAdherent(driver["id"], client)}, {"activity", field(client, "activity")},
                {"processCpuPercent", field(client, "processCpuPercent")}, {"processCpuWindowMs", field(client, "processCpuWindowMs")}});
        }
    return result;
}
namespace supervision {
// MIDI 1.0 patchbay projection. midi is the GET /api/v1/midi document.
// An application port is the engine's dedicated loopback pair for one
// application: it makes that application's traffic observable, but the
// loopback does not reveal which process opened it, so no process identity is
// claimed. Device ports are grouped per Windows MIDI Services endpoint.
inline std::string midiApplicationAdherent(const std::string& id) { return "midi:application:" + id; }
inline std::string midiApplicationNode(const std::string& id) { return "midi-application:" + id; }
inline std::string midiDeviceAdherent(const std::string& endpointId) { return "device:midi:" + endpointId; }
inline std::string midiDeviceNode(const std::string& endpointId) { return "midi-device:" + endpointId; }
inline Json midiTime(const Json& port, const char* key) {
    return port.contains(key) && port[key].is_number_integer() && port[key].get<std::int64_t>() > 0 ? port[key] : Json(nullptr);
}
inline std::pair<std::string, std::string> midiOwner(const Json& port) {
    if (port.value("kind", std::string{}) == "virtual") {
        const auto id = port["id"].get<std::string>().substr(std::string("midi:virtual:").size());
        return {midiApplicationNode(id), midiApplicationAdherent(id)};
    }
    const auto endpoint = port.value("endpointId", std::string{});
    return {midiDeviceNode(endpoint), midiDeviceAdherent(endpoint)};
}
}
// Traffic counters per MIDI port and per MIDI link, attributed to nodes.
inline nlohmann::json SupervisionMidiObservations(const nlohmann::json& midi) {
    using namespace supervision;
    if (!midi.is_object()) return nullptr;
    Json ports = Json::array(), links = Json::array(), clockSources = Json::array();
    for (const auto& port : midi.value("ports", Json::array())) {
        const auto owner = midiOwner(port);
        ports.push_back({{"portId", port["id"]}, {"nodeId", owner.first}, {"adherentId", owner.second},
            {"online", field(port, "online")}, {"open", field(port, "open")},
            {"received", port.value("received", 0)}, {"sent", port.value("sent", 0)},
            {"lastReceivedUnixMs", midiTime(port, "lastReceivedUnixMs")}, {"lastSentUnixMs", midiTime(port, "lastSentUnixMs")},
            {"activity", field(port, "activity")}, {"clock", field(port, "clock")}, {"activeSensing", field(port, "activeSensing")},
            {"error", field(port, "error")}});
        // A clock source is a port where ticks arrive: each application has its
        // own In loopback, so the emitter is that application. Clock the engine
        // itself forwards to the port and that comes back is only an echo.
        const auto clock = field(port, "clock");
        if (clock.is_object() && clock.value("running", false))
            clockSources.push_back({{"portId", port["id"]}, {"nodeId", owner.first}, {"adherentId", owner.second},
                {"bpm", field(clock, "bpm")}, {"transport", field(clock, "transport")},
                {"echoSuspected", clock.value("echoSuspected", false)}});
    }
    for (const auto& route : midi.value("routes", Json::array()))
        links.push_back({{"linkId", "midi:" + route["id"].get<std::string>()}, {"active", field(route, "active")},
            {"forwarded", field(route, "forwarded")}, {"filtered", field(route, "filtered")}});
    return {{"available", field(midi, "available")}, {"backend", field(midi, "backend")}, {"error", field(midi, "error")},
        {"ports", std::move(ports)}, {"links", std::move(links)}, {"clockSources", std::move(clockSources)}};
}
inline nlohmann::json SupervisionMeter(nlohmann::json meter, const nlohmann::json& devices) {
    using namespace supervision;
    const auto endpoint = meter.value("endpointId", std::string{});
    bool matchedClient = false;
    meter["availability"] = "observed";
    meter["unavailableReason"] = nullptr;
    for (const auto& driver : devices.value("virtualDrivers", Json::array()))
        for (const auto& client : driver.value("clients", Json::array())) {
            const auto prefix = "virtual:" + driver["id"].get<std::string>() + ":" + client["pid"].dump() + ":";
            if (endpoint.rfind(prefix, 0) != 0) continue;
            matchedClient = true;
            const auto activity = field(client, "activity");
            const auto status = activity.is_object() && activity.contains("state") && activity["state"].is_string()
                ? activity["state"].get<std::string>() : std::string{};
            const bool unavailable = !activity.is_object() || !activity.value("available", false);
            const bool source = endpoint.rfind(prefix + "output:", 0) == 0;
            const bool noExchange = activity.is_object() && activity.contains("successfulExchangeCount") &&
                activity["successfulExchangeCount"].is_number() && activity["successfulExchangeCount"] == 0;
            if (source && (unavailable || status == "stopped" || status == "detached" ||
                status == "initialized" || status == "buffers-ready" || noExchange)) {
                meter["peakDbfs"] = nullptr; meter["shortTermLufs"] = nullptr;
                meter["availability"] = "unavailable";
                meter["unavailableReason"] = unavailable ? "client-status-unavailable" : noExchange ? "client-no-exchange" : "client-" + status;
            }
        }
    if (endpoint.rfind("virtual:", 0) == 0 && !matchedClient) {
        meter["peakDbfs"] = nullptr; meter["shortTermLufs"] = nullptr;
        meter["availability"] = "unavailable"; meter["unavailableReason"] = "client-not-attached";
    }
    return meter;
}
inline nlohmann::json SupervisionSnapshot(const nlohmann::json& state, const nlohmann::json& meters,
        const nlohmann::json& extras, std::int64_t readAtUnixMs) {
    using namespace supervision;
    Json adherents = Json::array(), nodes = Json::array(), links = Json::array();
    std::map<std::string, std::size_t> adherentIndices;
    std::map<std::string, std::pair<std::string, std::string>> endpointOwners;
    const auto addNode = [&](const std::string& aid, const std::string& kind, const Json& label,
            const Json& identity, const Json& capabilities, Json node) {
        if (!adherentIndices.count(aid)) {
            adherentIndices[aid] = adherents.size();
            adherents.push_back({{"id", aid}, {"kind", kind}, {"label", label}, {"identity", identity},
                {"capabilities", capabilities}, {"nodeIds", Json::array()}});
        }
        node["adherentId"] = aid;
        adherents[adherentIndices.at(aid)]["nodeIds"].push_back(node["id"]);
        for (const auto* direction : {"sources", "sinks"})
            for (const auto& endpoint : node["ports"][direction]) endpointOwners[endpoint["id"]] = {node["id"], aid};
        nodes.push_back(std::move(node));
    };
    const Json unknownCapabilities{{"processing", nullptr}, {"midi", nullptr}};
    for (const auto& driver : state.value("virtualDrivers", Json::array())) {
        const auto did = driver["id"].get<std::string>();
        for (const auto& client : driver.value("clients", Json::array())) {
            const auto creation = field(client, "processCreationTime");
            const auto aid = asioAdherent(did, client);
            Json ports{{"sources", Json::array()}, {"sinks", Json::array()}};
            const auto prefix = "virtual:" + did + ":" + client["pid"].dump() + ":";
            for (const auto& pair : {std::make_pair("outputEndpoints", "sources"), std::make_pair("inputEndpoints", "sinks")})
                for (const auto& endpoint : driver.value(pair.first, Json::array()))
                    if (endpoint["id"].get<std::string>().rfind(prefix, 0) == 0) ports[pair.second].push_back(endpoint);
            addNode(aid, "process", field(client, "processName"),
                {{"scope", creation.is_string() ? "processInstance" : "runtimePid"}, {"processId", client["pid"]}, {"processCreationTime", creation}},
                unknownCapabilities, {{"id", asioNode(did, client)}, {"kind", "asioClient"}, {"resourceId", did},
                    {"state", "attached"}, {"ports", ports}, {"configuration", nullptr}, {"effectiveInfo", {{"sampleRate", field(driver, "sampleRate")}, {"bufferFrames", field(driver, "bufferFrames")}}}});
        }
    }
    const auto engine = state.value("engine", Json::object());
    for (const auto& driver : state.value("physicalDrivers", Json::array())) {
        if (field(engine, "physicalDriverId") != driver["id"]) continue;
        const auto did = driver["id"].get<std::string>();
        auto capabilities = unknownCapabilities;
        for (const auto* key : {"sampleRates", "bufferSizes", "capabilitiesKnown"}) capabilities[key] = field(driver, key);
        addNode("device:asio:" + did, "device", field(driver, "name"),
            {{"scope", "driverAccess"}, {"driverId", did}, {"hardwareIdentity", nullptr}}, capabilities,
            {{"id", "asio-device:" + did}, {"kind", "asioDevice"}, {"resourceId", did}, {"state", field(engine, "state")},
             {"ports", {{"sources", driver.value("inputEndpoints", Json::array())}, {"sinks", driver.value("outputEndpoints", Json::array())}}},
             {"configuration", nullptr}, {"effectiveInfo", {{"sampleRate", field(engine, "sampleRate")}, {"bufferFrames", field(engine, "bufferFrames")}}}});
    }
    const auto native = state.value("nativeAudio", Json::object());
    if (native.value("enabled", false)) {
        Json ports{{"sources", Json::array()}, {"sinks", Json::array()}};
        if (native.value("attached", false) && native.value("state", std::string{}) == "running" && field(native, "graphFormat").is_object()) {
            for (const auto& pair : {std::make_pair("input", "sources"), std::make_pair("output", "sinks")})
                for (int channel = 1; channel <= 2; ++channel)
                    ports[pair.second].push_back({{"id", "native:windows:" + std::string(pair.first) + ":" + std::to_string(channel)},
                        {"name", "Windows " + std::string(pair.first) + " " + std::to_string(channel)}, {"direction", pair.first}, {"channel", channel}});
        }
        addNode("device:native:windows", "device", "Windows native audio", {{"scope", "driverAccess"}, {"driverId", "native:windows"}, {"hardwareIdentity", nullptr}},
            unknownCapabilities, {{"id", "native:windows"}, {"kind", "nativeDevice"}, {"resourceId", "windows"}, {"state", field(native, "state")},
                {"ports", ports}, {"configuration", nullptr}, {"effectiveInfo", {{"graphFormat", field(native, "graphFormat")}, {"nativeFormat", field(native, "nativeFormat")},
                    {"epoch", field(native, "epoch")}, {"wireGeneration", field(native, "wireGeneration")}}}});
    }
    Json bridgeObservations = Json::array();
    for (const auto& config : state.value("configuredWasapiBridges", Json::array())) {
        Json runtime = Json::object();
        for (const auto& candidate : state.value("wasapiBridges", Json::array())) if (candidate["id"] == config["id"]) runtime = candidate;
        Json effective = field(runtime, "effectiveInfo");
        Json endpoint = effective.is_object() ? field(effective, "endpointId") : field(config, "endpointId");
        if (!endpoint.is_string() || endpoint.get<std::string>().empty()) endpoint = field(config, "endpointId");
        if (!runtime.empty()) bridgeObservations.push_back({{"nodeId", nullptr}, {"adherentId", nullptr},
            {"bridgeId", config["id"]}, {"state", field(runtime, "state")}, {"health", field(runtime, "health")},
            {"generationId", field(runtime, "generationId")}, {"retryAttempt", field(runtime, "retryAttempt")},
            {"retryInMs", field(runtime, "retryInMs")}, {"graphFormat", field(runtime, "graphFormat")}});
        std::string aid, kind; Json identity, label;
        if (config.contains("processId")) {
            const auto creation = field(config, "processCreationTime");
            aid = processId(config["processId"], creation) + (creation.is_string() ? "" : ":wasapi:" + config["id"].get<std::string>()); kind = "process"; label = field(config, "processImagePath");
            identity = {{"scope", creation.is_string() ? "processInstance" : "runtimePid"}, {"processId", config["processId"]}, {"processCreationTime", creation}};
        } else if (endpoint.is_string() && !endpoint.get<std::string>().empty()) {
            aid = "device:wasapi:" + endpoint.get<std::string>(); kind = "device"; label = endpoint;
            identity = {{"scope", "wasapiEndpoint"}, {"endpointId", endpoint}, {"hardwareIdentity", nullptr}};
        } else continue; // Unresolved intent is configuration, not a fabricated participant.
        const auto nid = "wasapi:" + config["id"].get<std::string>() + ":" + aid;
        addNode(aid, kind, label, identity, unknownCapabilities,
            {{"id", nid}, {"kind", "wasapiBridge"}, {"resourceId", config["id"]},
             {"state", runtime.contains("state") ? runtime["state"] : Json(config.value("enabled", false) ? "configured" : "disabled")},
             {"ports", {{"sources", runtime.value("inputEndpoints", Json::array())}, {"sinks", runtime.value("outputEndpoints", Json::array())}}},
             {"configuration", config}, {"effectiveInfo", effective}});
        if (!runtime.empty()) {
            bridgeObservations.back()["nodeId"] = nid;
            bridgeObservations.back()["adherentId"] = aid;
        }
    }
    const auto midi = field(extras, "midi");
    if (midi.is_object()) {
        std::map<std::string, Json> virtualConfig;
        for (const auto& port : field(midi, "configuration").value("virtualPorts", Json::array())) virtualConfig[port["id"]] = port;
        std::map<std::string, std::pair<Json, Json>> devicePorts;  // endpointId -> sources, sinks
        std::map<std::string, Json> deviceNames, deviceStates;
        for (const auto& port : midi.value("ports", Json::array())) {
            const auto kind = port.value("kind", std::string{});
            if (kind == "virtual") {
                const auto id = port["id"].get<std::string>().substr(std::string("midi:virtual:").size());
                const auto config = virtualConfig.count(id) ? virtualConfig.at(id) : Json(nullptr);
                const auto name = port.value("name", std::string{});
                Json ports{{"sources", Json::array()}, {"sinks", Json::array()}};
                if (port.value("source", false)) ports["sources"].push_back({{"id", port["id"]}, {"name", name + " In"},
                    {"direction", "input"}, {"medium", "midi"}, {"endpointId", field(port, "inputEndpointId")}});
                if (port.value("destination", false)) ports["sinks"].push_back({{"id", port["id"]}, {"name", name + " Out"},
                    {"direction", "output"}, {"medium", "midi"}, {"endpointId", field(port, "outputEndpointId")}});
                addNode(midiApplicationAdherent(id), "application", name,
                    {{"scope", "midiApplicationPort"}, {"portId", port["id"]}},
                    {{"processing", nullptr}, {"midi", {{"protocol", "midi1"}, {"input", port.value("source", false)},
                        {"output", port.value("destination", false)}}}},
                    {{"id", midiApplicationNode(id)}, {"kind", "midiApplicationPort"}, {"resourceId", id},
                     {"state", port.value("open", false) ? "open" : "unavailable"}, {"ports", ports}, {"configuration", config},
                     {"effectiveInfo", {{"inputEndpointId", field(port, "inputEndpointId")}, {"outputEndpointId", field(port, "outputEndpointId")}}}});
            } else if (kind == "port") {
                const auto endpoint = port.value("endpointId", std::string{});
                auto& entry = devicePorts[endpoint];
                if (entry.first.is_null()) { entry.first = Json::array(); entry.second = Json::array(); }
                const Json projected{{"id", port["id"]}, {"name", field(port, "name")}, {"group", field(port, "group")}, {"medium", "midi"}};
                if (port.value("source", false)) { auto p = projected; p["direction"] = "input"; entry.first.push_back(p); }
                if (port.value("destination", false)) { auto p = projected; p["direction"] = "output"; entry.second.push_back(p); }
                if (!port.value("source", false) && !port.value("destination", false)) {
                    // Routed but absent: keep it attributable without inventing a direction.
                    auto p = projected; p["direction"] = nullptr; entry.first.push_back(p);
                }
                if (!deviceNames.count(endpoint) || deviceNames[endpoint].is_null()) deviceNames[endpoint] = field(port, "endpointName");
                const auto state = !port.value("online", false) ? "offline" : port.value("open", false) ? "open" : "available";
                if (!deviceStates.count(endpoint) || deviceStates[endpoint] != "open") deviceStates[endpoint] = state;
            }
        }
        for (const auto& entry : devicePorts)
            addNode(midiDeviceAdherent(entry.first), "device", deviceNames[entry.first].is_null() ? Json(entry.first) : deviceNames[entry.first],
                {{"scope", "midiEndpoint"}, {"endpointId", entry.first}, {"hardwareIdentity", nullptr}},
                {{"processing", nullptr}, {"midi", {{"protocol", "midi1"}}}},
                {{"id", midiDeviceNode(entry.first)}, {"kind", "midiDevice"}, {"resourceId", entry.first}, {"state", deviceStates[entry.first]},
                 {"ports", {{"sources", entry.second.first}, {"sinks", entry.second.second}}}, {"configuration", nullptr}, {"effectiveInfo", nullptr}});
    }
    std::map<std::string, Json> requested, applied;
    for (const auto& route : state.value("configuredRoutes", Json::array())) requested[route["id"]] = route;
    for (const auto& route : state.value("routes", Json::array())) applied[route["id"]] = route;
    auto all = requested; for (const auto& route : applied) all[route.first] = route.second;
    for (const auto& entry : all) {
        const auto& route = entry.second;
        Json link{{"id", entry.first}, {"owner", "engine"}, {"requested", requested.count(entry.first) ? requested.at(entry.first) : Json(nullptr)},
            {"applied", applied.count(entry.first) ? applied.at(entry.first) : Json(nullptr)}, {"state", applied.count(entry.first) ? "applied" : "configured"}};
        for (const auto& pair : {std::make_pair("sourceEndpointId", "sourceNodeId"), std::make_pair("destinationEndpointId", "destinationNodeId")}) {
            link[pair.first] = field(route, pair.first);
            const auto found = endpointOwners.find(route.value(pair.first, std::string{}));
            link[pair.second] = found == endpointOwners.end() ? Json(nullptr) : Json(found->second.first);
        }
        links.push_back(std::move(link));
    }
    if (midi.is_object()) {
        std::map<std::string, Json> status;
        for (const auto& route : midi.value("routes", Json::array())) status[route["id"]] = route;
        for (const auto& route : field(midi, "configuration").value("routes", Json::array())) {
            const auto id = route["id"].get<std::string>();
            const bool active = status.count(id) && status.at(id).value("active", false);
            Json link{{"id", "midi:" + id}, {"owner", "engine"}, {"medium", "midi"}, {"requested", route},
                {"applied", active ? route : Json(nullptr)}, {"state", active ? "applied" : "configured"},
                {"sourceEndpointId", route["source"]}, {"destinationEndpointId", route["destination"]}};
            for (const auto& pair : {std::make_pair("source", "sourceNodeId"), std::make_pair("destination", "destinationNodeId")}) {
                const auto found = endpointOwners.find(route.value(pair.first, std::string{}));
                link[pair.second] = found == endpointOwners.end() ? Json(nullptr) : Json(found->second.first);
            }
            links.push_back(std::move(link));
        }
    }
    Json channelMeasurements = Json::array();
    for (auto meter : meters) {
        meter = SupervisionMeter(std::move(meter), state);
        const auto found = endpointOwners.find(meter.value("endpointId", std::string{}));
        meter["nodeId"] = found == endpointOwners.end() ? Json(nullptr) : Json(found->second.first);
        meter["adherentId"] = found == endpointOwners.end() ? Json(nullptr) : Json(found->second.second);
        channelMeasurements.push_back(std::move(meter));
    }
    auto projectedEngine = engine;
    projectedEngine["configuration"] = {{"revision", field(state, "configurationRevision")},
        {"configuredRoutes", state.value("configuredRoutes", Json::array())}, {"configuredWasapiBridges", state.value("configuredWasapiBridges", Json::array())},
        {"nativeAudioEnabled", native.value("enabled", false)},
        {"midi", midi.is_object() ? field(midi, "configuration") : Json(nullptr)}};
    projectedEngine["observations"] = {{"owner", "engine"}, {"readAtUnixMs", readAtUnixMs},
        {"bufferHealth", field(state, "bufferHealth")}, {"performanceMeasurements", field(state, "performanceMeasurements")},
        {"meters", channelMeasurements}, {"clients", SupervisionClientObservations(state)}, {"wasapiBridges", bridgeObservations},
        {"nativeAudio", field(state, "nativeAudio")}, {"pwarClient", field(extras, "pwarClient")},
        {"midi", SupervisionMidiObservations(midi)}};
    projectedEngine["channelPairs"] = state.value("stereoPairs", Json::array());
    projectedEngine["resources"] = {{"applicationProfiles", "/api/v1/application-profiles"}, {"pwarSettings", "/api/v1/pwar"}, {"diagnostics", "/api/v1/diagnostics"}, {"midi", "/api/v1/midi"}};
    const auto cachedProcesses = field(extras, "windowsAudioApplications");
    const auto processInventoryError = field(extras, "windowsAudioApplicationInventoryError");
    const bool hasCachedProcesses = cachedProcesses.is_array() && !cachedProcesses.empty();
    return {{"schemaVersion", 1}, {"readAtUnixMs", readAtUnixMs}, {"engine", projectedEngine}, {"adherents", adherents}, {"nodes", nodes}, {"links", links},
        {"inventory", {{"physicalDrivers", state.value("physicalDrivers", Json::array())}, {"wasapiDevices", state.value("wasapiDevices", Json::array())},
            {"windowsAudioApplications", hasCachedProcesses ? cachedProcesses : Json(nullptr)},
            {"windowsAudioApplicationsState", !processInventoryError.is_null() ? "unavailable" : hasCachedProcesses ? "cached" : "unknown"},
            {"windowsAudioApplicationInventoryError", processInventoryError}, {"wasapiError", field(state, "wasapiInventoryError")}}}};
}
