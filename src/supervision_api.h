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
        {"nativeAudioEnabled", native.value("enabled", false)}};
    projectedEngine["observations"] = {{"owner", "engine"}, {"readAtUnixMs", readAtUnixMs},
        {"bufferHealth", field(state, "bufferHealth")}, {"performanceMeasurements", field(state, "performanceMeasurements")},
        {"meters", channelMeasurements}, {"clients", SupervisionClientObservations(state)}, {"wasapiBridges", bridgeObservations},
        {"nativeAudio", field(state, "nativeAudio")}, {"pwarClient", field(extras, "pwarClient")}};
    projectedEngine["channelPairs"] = state.value("stereoPairs", Json::array());
    projectedEngine["resources"] = {{"applicationProfiles", "/api/v1/application-profiles"}, {"pwarSettings", "/api/v1/pwar"}, {"diagnostics", "/api/v1/diagnostics"}};
    const auto cachedProcesses = field(extras, "windowsAudioApplications");
    const auto processInventoryError = field(extras, "windowsAudioApplicationInventoryError");
    const bool hasCachedProcesses = cachedProcesses.is_array() && !cachedProcesses.empty();
    return {{"schemaVersion", 1}, {"readAtUnixMs", readAtUnixMs}, {"engine", projectedEngine}, {"adherents", adherents}, {"nodes", nodes}, {"links", links},
        {"inventory", {{"physicalDrivers", state.value("physicalDrivers", Json::array())}, {"wasapiDevices", state.value("wasapiDevices", Json::array())},
            {"windowsAudioApplications", hasCachedProcesses ? cachedProcesses : Json(nullptr)},
            {"windowsAudioApplicationsState", !processInventoryError.is_null() ? "unavailable" : hasCachedProcesses ? "cached" : "unknown"},
            {"windowsAudioApplicationInventoryError", processInventoryError}, {"wasapiError", field(state, "wasapiInventoryError")}}}};
}
