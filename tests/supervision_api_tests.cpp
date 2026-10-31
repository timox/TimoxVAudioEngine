#include "supervision_api.h"
#include <cstdio>
#include <stdexcept>
#include <fstream>
using Json = nlohmann::json;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

int main() {
    try {
        Json client{{"pid", 42}, {"processName", "DAW.exe"}, {"processCreationTime", "9007199254740993"},
            {"inputChannels", {1}}, {"outputChannels", {1}}, {"processCpuPercent", nullptr},
            {"processCpuWindowMs", nullptr}, {"activity", {{"available", true}, {"callbackCount", 1}}}};
        Json source{{"id", "virtual:TimoxVasio:42:output:1"}, {"name", "out"}, {"direction", "output"}, {"channel", 1}};
        Json sink{{"id", "virtual:TimoxVasio:42:input:1"}, {"name", "in"}, {"direction", "input"}, {"channel", 1}};
        Json driver{{"id", "TimoxVasio"}, {"clients", Json::array({client})},
            {"inputEndpoints", Json::array({sink})}, {"outputEndpoints", Json::array({source})}};
        Json devices{{"physicalDrivers", Json::array()}, {"virtualDrivers", Json::array({driver})}, {"stereoPairs", Json::array()}};
        auto changed = devices;
        changed["virtualDrivers"][0]["clients"][0]["activity"]["callbackCount"] = 2;
        changed["virtualDrivers"][0]["clients"][0]["processCpuPercent"] = 12.0;
        changed["virtualDrivers"][0]["clients"][0]["processCpuWindowMs"] = 1000;
        check(SupervisionDeviceTopology(devices) == SupervisionDeviceTopology(changed), "activity/CPU changes must not invalidate topology");
        changed["virtualDrivers"][0]["clients"][0]["outputChannels"] = {1, 2};
        check(SupervisionDeviceTopology(devices) != SupervisionDeviceTopology(changed), "allocated ports must invalidate topology");
        changed = devices;
        changed["virtualDrivers"][0]["clients"][0]["processCreationTime"] = "9007199254740994";
        check(SupervisionDeviceTopology(devices) != SupervisionDeviceTopology(changed), "PID reuse must invalidate identity");
        auto observations = SupervisionClientObservations(devices);
        check(observations.size() == 1 && observations[0]["processCpuPercent"].is_null(), "unknown CPU is not zero");
        check(observations[0]["adherentId"] == "process:42:9007199254740993", "process creation identity must remain a string");

        Json state = devices;
        state["engine"] = {{"state", "running"}, {"physicalDriverId", nullptr}, {"sampleRate", 48000}, {"bufferFrames", 128}};
        state["configurationRevision"] = 7;
        state["configuredRoutes"] = Json::array(); state["routes"] = Json::array();
        state["configuredWasapiBridges"] = Json::array({
            {{"id", "copy1"}, {"kind", "processLoopback"}, {"enabled", true}, {"processId", 42}, {"processCreationTime", "9007199254740993"}, {"processImagePath", "C:/DAW.exe"}},
            {{"id", "copy2"}, {"kind", "processLoopback"}, {"enabled", false}, {"processId", 42}, {"processCreationTime", "9007199254740993"}, {"processImagePath", "C:/DAW.exe"}}});
        state["wasapiBridges"] = Json::array(); state["wasapiDevices"] = Json::array();
        state["bufferHealth"] = nullptr;
        state["performanceMeasurements"] = {{"enabled", false}, {"window", nullptr}};
        state["nativeAudio"] = {{"enabled", false}, {"state", "disabled"}, {"attached", false}};
        Json extras{{"windowsAudioApplications", Json::array()}, {"pwarClient", {{"pid", 42}, {"available", true}}}};
        Json meter{{"endpointId", source["id"]}, {"peakDbfs", -12.0}, {"shortTermLufs", nullptr},
            {"lufsWindowSeconds", 3.0}, {"underruns", 0}, {"overruns", 0}};
        const auto model = SupervisionSnapshot(state, Json::array({meter}), extras, 123);
        check(model["adherents"].size() == 1, "ASIO and two process copies must share the same proven process identity");
        check(model["nodes"].size() == 3, "one process can have multiple connection nodes");
        check(model["nodes"][0]["ports"]["sources"][0]["id"] == source["id"], "ASIO output is a motor source");
        check(model["nodes"][0]["ports"]["sinks"][0]["id"] == sink["id"], "ASIO input is a motor sink");
        check(model["nodes"][1]["ports"]["sources"].empty(), "configured process copy must not fabricate live stereo ports");
        check(model["adherents"][0]["capabilities"]["processing"].is_null(), "DAW capacity is unknown");
        check(!model["adherents"][0].contains("meters") && !model["nodes"][0].contains("meters"), "levels belong only to engine observations");
        check(model["engine"]["observations"]["meters"][0]["nodeId"] == model["nodes"][0]["id"], "engine channel measurement references its node");
        check(model["engine"]["observations"]["meters"][0]["shortTermLufs"].is_null(), "unavailable LUFS is preserved");

        auto unknownIdentityState = state;
        unknownIdentityState["virtualDrivers"][0]["clients"][0]["processCreationTime"] = nullptr;
        for (auto& config : unknownIdentityState["configuredWasapiBridges"]) config["processCreationTime"] = nullptr;
        const auto unknownIdentity = SupervisionSnapshot(unknownIdentityState, Json::array(), extras, 123);
        check(unknownIdentity["adherents"].size() == 3, "PID alone cannot merge ASIO and WASAPI process identities");

        state["configuredWasapiBridges"] = Json::array({{{"id", "default"}, {"kind", "render"}, {"defaultRole", "console"}, {"enabled", true}}});
        state["physicalDrivers"] = Json::array({{{"id", "unused"}, {"name", "Unopened driver"}, {"inputEndpoints", Json::array()}, {"outputEndpoints", Json::array()}}});
        const auto unresolved = SupervisionSnapshot(state, Json::array(), extras, 124);
        check(unresolved["nodes"].size() == 1, "unresolved default endpoint and inventory-only driver are not invented participants");
        check(unresolved["engine"]["configuration"]["configuredWasapiBridges"].size() == 1, "unresolved intent remains visible");
        check(unresolved["engine"]["observations"]["meters"].empty(), "absence of meters is not fabricated silence");
        auto unresolvedRuntime = state;
        unresolvedRuntime["wasapiBridges"] = Json::array({{{"id", "default"}, {"state", "retrying"},
            {"effectiveInfo", nullptr}, {"health", {{"error", -1}}}, {"generationId", "0"},
            {"retryAttempt", 2}, {"retryInMs", 500}, {"graphFormat", nullptr}}});
        const auto unresolvedObserved = SupervisionSnapshot(unresolvedRuntime, Json::array(), extras, 124);
        const auto& unresolvedBridges = unresolvedObserved["engine"]["observations"]["wasapiBridges"];
        check(unresolvedBridges.size() == 1, "unresolved default bridge must retain runtime observations");
        check(unresolvedBridges[0]["nodeId"].is_null() && unresolvedBridges[0]["adherentId"].is_null(), "unresolved observations cannot fabricate identity");
        check(unresolvedBridges[0]["bridgeId"] == "default" && unresolvedBridges[0]["state"] == "retrying" &&
            unresolvedBridges[0]["health"]["error"] == -1 && unresolvedBridges[0]["retryAttempt"] == 2,
            "unresolved bridge preserves state, error and retry evidence");
        check(unresolvedObserved["inventory"]["windowsAudioApplications"].is_null() &&
            unresolvedObserved["inventory"]["windowsAudioApplicationsState"] == "unknown", "empty unrefreshed inventory remains unknown");
        unresolvedRuntime["engine"]["state"] = "error";
        unresolvedRuntime["engine"]["lastError"] = {{"code", "AUDIO_CONFIGURATION_FAILED"}, {"message", "test failure"}, {"operation", "apply"}};
        const auto failedEngine = SupervisionSnapshot(unresolvedRuntime, Json::array(), extras, 127);
        check(failedEngine["engine"]["state"] == "error" && failedEngine["engine"]["lastError"] == unresolvedRuntime["engine"]["lastError"], "engine state and error are preserved");
        check(failedEngine["readAtUnixMs"] == 127 && failedEngine["engine"]["observations"]["readAtUnixMs"] == 127, "observation read time is preserved");
        unresolvedRuntime["configuredWasapiBridges"][0]["endpointId"] = "configured-endpoint";
        unresolvedRuntime["wasapiBridges"][0]["effectiveInfo"] = Json::object();
        const auto explicitEndpoint = SupervisionSnapshot(unresolvedRuntime, Json::array(), extras, 128);
        check(explicitEndpoint["nodes"].size() == 2 && explicitEndpoint["nodes"][1]["adherentId"] == "device:wasapi:configured-endpoint",
            "partial effective info preserves explicit configured endpoint identity");
        check(explicitEndpoint["engine"]["observations"]["wasapiBridges"][0]["nodeId"] == explicitEndpoint["nodes"][1]["id"],
            "explicit endpoint observation references its node");
        unresolvedRuntime["configuredWasapiBridges"][0]["endpointId"] = "";
        unresolvedRuntime["wasapiBridges"][0]["effectiveInfo"]["endpointId"] = "";
        const auto emptyEndpoint = SupervisionSnapshot(unresolvedRuntime, Json::array(), extras, 129);
        check(emptyEndpoint["nodes"].size() == 1 && emptyEndpoint["engine"]["observations"]["wasapiBridges"][0]["nodeId"].is_null(),
            "empty endpoint identifiers do not fabricate a participant");
        check(SupervisionMeter(meter, Json{{"virtualDrivers", Json::array()}})["unavailableReason"] == "client-not-attached", "disappeared process cannot retain a current peak");
        auto bridgeState = state;
        bridgeState["configuredWasapiBridges"] = Json::array({{{"id", "capture"}, {"kind", "capture"}, {"endpointId", "endpoint-A"}, {"enabled", true}}});
        Json bridgeSource{{"id", "wasapi:capture:input:1"}, {"name", "capture"}, {"direction", "input"}, {"channel", 1}};
        bridgeState["wasapiBridges"] = Json::array({{{"id", "capture"}, {"state", "running"},
            {"inputEndpoints", Json::array({bridgeSource})}, {"outputEndpoints", Json::array()},
            {"effectiveInfo", {{"endpointId", "endpoint-A"}}}, {"health", {{"capturedFrames", 512}}}}});
        auto bridgeMeter = meter; bridgeMeter["endpointId"] = bridgeSource["id"];
        const auto bridgeModel = SupervisionSnapshot(bridgeState, Json::array({bridgeMeter}), extras, 124);
        check(bridgeModel["nodes"][1]["ports"]["sources"][0]["id"] == bridgeSource["id"], "WASAPI capture is a motor source");
        check(bridgeModel["engine"]["observations"]["meters"][0]["nodeId"] == bridgeModel["nodes"][1]["id"], "WASAPI meters resolve bridge node");
        check(bridgeModel["engine"]["observations"]["wasapiBridges"][0]["health"]["capturedFrames"] == 512, "WASAPI counters stay centralized");
        auto stoppedDevices = devices;
        stoppedDevices["virtualDrivers"][0]["clients"][0]["activity"]["state"] = "stopped";
        auto unavailable = SupervisionMeter(meter, stoppedDevices);
        check(unavailable["peakDbfs"].is_null() && unavailable["unavailableReason"] == "client-stopped", "stopped client cannot publish stale peak as current signal");
        stoppedDevices["virtualDrivers"][0]["clients"][0]["activity"]["state"] = "detached";
        check(SupervisionMeter(meter, stoppedDevices)["unavailableReason"] == "client-detached", "detached client invalidates levels");
        stoppedDevices["virtualDrivers"][0]["clients"][0]["activity"]["available"] = false;
        check(SupervisionMeter(meter, stoppedDevices)["unavailableReason"] == "client-status-unavailable", "missing status is unknown, not silence");
        auto sinkMeter = meter; sinkMeter["endpointId"] = sink["id"];
        check(SupervisionMeter(sinkMeter, stoppedDevices)["peakDbfs"] == -12.0, "client state must not erase motor-produced sink level");
        stoppedDevices["virtualDrivers"][0]["clients"][0]["activity"] = {{"available", true}, {"state", "buffers-ready"}, {"successfulExchangeCount", 0}};
        check(SupervisionMeter(meter, stoppedDevices)["peakDbfs"].is_null(), "unstarted source is not measured silence");
        check(SupervisionMeter(meter, devices)["peakDbfs"] == -12.0, "available live levels are unchanged");
        state["nativeAudio"] = {{"enabled", true}, {"attached", true}, {"state", "running"},
            {"graphFormat", {{"sampleRate", 48000}, {"maxQuantum", 128}, {"channels", 2}}},
            {"nativeFormat", {{"sampleRate", 48000}, {"channels", 2}, {"sampleType", "pcm32"}}}, {"epoch", "7"}, {"wireGeneration", "9"}};
        Json desired{{"id", "route1"}, {"sourceEndpointId", "saved:alias"}, {"destinationEndpointId", sink["id"]}, {"gainDb", -3.0}, {"mute", false}};
        auto applied = desired; applied["sourceEndpointId"] = "native:windows:input:1";
        state["configuredRoutes"] = Json::array({desired}); state["routes"] = Json::array({applied});
        auto nativeMeter = meter; nativeMeter["endpointId"] = "native:windows:input:1";
        const auto nativeModel = SupervisionSnapshot(state, Json::array({nativeMeter}), extras, 125);
        check(nativeModel["links"][0]["sourceNodeId"] == "native:windows", "applied link resolves native node");
        check(nativeModel["links"][0]["requested"]["sourceEndpointId"] == "saved:alias", "requested alias remains separate from applied endpoint");
        check(nativeModel["engine"]["observations"]["meters"][0]["nodeId"] == "native:windows", "native channel measurement resolves node");
        state["routes"] = Json::array(); state["nativeAudio"]["attached"] = false; state["nativeAudio"]["state"] = "unavailable";
        const auto offlineModel = SupervisionSnapshot(state, Json::array(), extras, 126);
        check(offlineModel["links"][0]["sourceNodeId"].is_null() && offlineModel["links"][0]["applied"].is_null(), "offline route intent does not fabricate applied endpoint");
        check(offlineModel["nodes"][1]["ports"]["sources"].empty(), "unattached native access has no live ports");
        check(offlineModel["engine"]["observations"]["midi"].is_null(), "no MIDI patchbay projects no MIDI observation");

        // MIDI 1.0 patchbay: application ports and device ports become nodes, routes become links.
        Json midi{{"available", true}, {"backend", "windows-midi-services"}, {"error", nullptr},
            {"configuration", {{"virtualPorts", Json::array({{{"id", "bitwig"}, {"name", "Bitwig"}, {"input", true}, {"output", false}}})},
                {"routes", Json::array({
                    {{"id", "to-synth"}, {"source", "midi:virtual:bitwig"}, {"destination", "midi:port:2:usb"}},
                    {{"id", "to-gone"}, {"source", "midi:virtual:bitwig"}, {"destination", "midi:port:1:gone"}}})}}},
            {"ports", Json::array({
                {{"id", "midi:virtual:bitwig"}, {"name", "Bitwig"}, {"kind", "virtual"}, {"source", true}, {"destination", false},
                 {"online", true}, {"open", true}, {"inputEndpointId", "loop-in"}, {"outputEndpointId", nullptr},
                 {"received", 5}, {"sent", 0}, {"lastReceivedUnixMs", 1000}, {"lastSentUnixMs", 0}, {"error", nullptr},
                 {"activity", {{"messages", 1}, {"lastUnixMs", 1000}}}, {"activeSensing", {{"messages", 0}, {"lastUnixMs", nullptr}}},
                 {"clock", {{"ticks", 4}, {"running", true}, {"bpm", 120.0}, {"lastTickUnixMs", 1000}, {"echoSuspected", false},
                     {"transport", "start"}, {"transportUnixMs", 990}}}},
                {{"id", "midi:port:2:usb"}, {"name", "UM-4 Port 2"}, {"kind", "port"}, {"endpointId", "usb"}, {"endpointName", "UM-4"},
                 {"group", 2}, {"source", true}, {"destination", true}, {"online", true}, {"open", true},
                 {"received", 0}, {"sent", 4}, {"lastReceivedUnixMs", 0}, {"lastSentUnixMs", 900}, {"error", nullptr}},
                {{"id", "midi:port:1:gone"}, {"name", ""}, {"kind", "port"}, {"endpointId", "gone"}, {"endpointName", nullptr},
                 {"group", 1}, {"source", false}, {"destination", false}, {"online", false}, {"open", false},
                 {"received", 0}, {"sent", 0}, {"lastReceivedUnixMs", 0}, {"lastSentUnixMs", 0}, {"error", nullptr}}})},
            {"routes", Json::array({{{"id", "to-synth"}, {"active", true}, {"forwarded", 4}, {"filtered", 1}},
                {{"id", "to-gone"}, {"active", false}, {"forwarded", 0}, {"filtered", 0}}})}};
        auto midiExtras = extras; midiExtras["midi"] = midi;
        const auto midiModel = SupervisionSnapshot(state, Json::array(), midiExtras, 130);
        Json app, device, offline;
        for (const auto& node : midiModel["nodes"]) {
            if (node["id"] == "midi-application:bitwig") app = node;
            if (node["id"] == "midi-device:usb") device = node;
            if (node["id"] == "midi-device:gone") offline = node;
        }
        check(app["kind"] == "midiApplicationPort" && app["ports"]["sources"].size() == 1 && app["ports"]["sinks"].empty(),
            "application port exposes only its enabled In");
        check(app["ports"]["sources"][0]["name"] == "Bitwig In" && app["adherentId"] == "midi:application:bitwig",
            "application port is its own adherent");
        check(device["state"] == "open" && device["ports"]["sources"][0]["group"] == 2 && device["ports"]["sinks"].size() == 1,
            "device ports keep their MIDI 1.0 port and direction");
        check(offline["state"] == "offline", "absent routed device stays visible as offline");
        Json synthLink, goneLink;
        for (const auto& link : midiModel["links"]) {
            if (link["id"] == "midi:to-synth") synthLink = link;
            if (link["id"] == "midi:to-gone") goneLink = link;
        }
        check(synthLink["medium"] == "midi" && synthLink["state"] == "applied" &&
              synthLink["sourceNodeId"] == "midi-application:bitwig" && synthLink["destinationNodeId"] == "midi-device:usb",
            "active MIDI route is an applied link between resolved nodes");
        check(goneLink["state"] == "configured" && goneLink["applied"].is_null() && goneLink["destinationNodeId"] == "midi-device:gone",
            "inactive MIDI route stays configured intent");
        const auto& midiObservations = midiModel["engine"]["observations"]["midi"];
        check(midiObservations["ports"][0]["received"] == 5 && midiObservations["ports"][0]["lastSentUnixMs"].is_null() &&
              midiObservations["ports"][0]["nodeId"] == "midi-application:bitwig", "port traffic is attributed to its node");
        check(midiObservations["clockSources"].size() == 1 && midiObservations["clockSources"][0]["nodeId"] == "midi-application:bitwig" &&
              midiObservations["clockSources"][0]["bpm"] == 120.0, "the port receiving running clock is reported as its source");
        check(midiObservations["ports"][0]["clock"]["ticks"] == 4 && midiObservations["ports"][0]["activity"]["messages"] == 1,
            "clock ticks are reported apart from activity");
        check(midiObservations["links"][0]["linkId"] == "midi:to-synth" && midiObservations["links"][0]["forwarded"] == 4,
            "link traffic is observed by the engine");
        check(midiModel["engine"]["configuration"]["midi"]["routes"].size() == 2 &&
              midiModel["engine"]["resources"]["midi"] == "/api/v1/midi", "MIDI configuration and resource are referenced");
        std::puts("PASS: topology, process identity, multi-node ownership, and motor channel measurements, MIDI 1.0 topology");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
