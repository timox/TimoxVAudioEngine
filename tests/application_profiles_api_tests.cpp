#include <httplib.h>
#include <nlohmann/json.hpp>

#include "audio_controller.h"
#include "control_api_server.h"
#include "vasio_client_manager.h"
#include "engine_diagnostics.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

namespace {
// Discovery-only ASIO replacement: no physical device or callback is needed to
// exercise the real controller, API validation and saved-route restoration.
class OneChannelClock final : public AudioClockHost {
public:
    std::vector<AudioClockDeviceInfo> enumerate() const override {
        return {{L"replacement", "One-channel replacement"}};
    }
    bool open(const std::wstring& id, void*, AudioClockCapabilities& caps, std::string& error) override {
        if (id != L"replacement") { error = "Unknown fake device"; return false; }
        caps_ = {};
        caps_.inputChannels = caps_.outputChannels = 1;
        caps_.minBufferFrames = 16;
        caps_.maxBufferFrames = 512;
        caps_.preferredBufferFrames = 64;
        caps_.bufferGranularity = -1;
        caps_.currentSampleRate = 48000;
        caps_.supportedSampleRates = {48000, 96000};
        caps = caps_;
        return true;
    }
    bool setSampleRate(double rate, std::string&) override { caps_.currentSampleRate = rate; return true; }
    bool refreshCapabilities(AudioClockCapabilities& caps, std::string&) override { caps = caps_; return true; }
    bool consumeSampleRateChange() noexcept override { return false; }
    AudioClockTimingCounters consumeTimingCounters() noexcept override { return {}; }
    void setStageMeasurementsEnabled(bool) noexcept override {}
    StageTimingCounters consumeOutputCopyStageCounters() noexcept override { return {}; }
    bool start(std::uint32_t, const std::vector<std::uint32_t>&,
        const std::vector<std::uint32_t>&, AudioClockProcessCallback, void*, std::string& error) override {
        error = "Dormant routes must not start the discovery-only ASIO host";
        return false;
    }
    void stop() noexcept override {}
    void close() noexcept override {}
private:
    AudioClockCapabilities caps_;
};

bool require(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

std::wstring tempPath(const wchar_t* suffix) {
    wchar_t directory[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory)) return {};
    return std::wstring(directory) + L"TimoxVasio-profile-api-test-" +
        std::to_wstring(GetCurrentProcessId()) + suffix;
}
}

int wmain() {
    const auto configPath = tempPath(L"-configuration.json");
    const auto profilesPath = tempPath(L"-profiles.json");
    if (configPath.empty() || profilesPath.empty()) return 2;
    DeleteFileW(configPath.c_str());
    DeleteFileW(profilesPath.c_str());

    VasioClientManager clients;
    AudioController controller(clients, configPath, profilesPath,
        [] { return std::make_unique<OneChannelClock>(); });
    if (!controller.Start()) return 3;

    EngineDiagnostics diagnostics;
    std::string error;
    if (!diagnostics.Initialize(tempPath(L"-logs"), error)) return 3;

    std::atomic<bool> stopRequested{false};
    ControlApiServer api(clients, controller, diagnostics, [&] { stopRequested.store(true); });
    std::uint16_t port = 0;
    if (!api.Start(0, port, error)) {
        std::fprintf(stderr, "API server failed to start: %s\n", error.c_str());
        controller.Stop();
        return 4;
    }

    httplib::Client client("127.0.0.1", port);
    bool passed = true;
    const auto initialState = client.Get("/api/v1/state");
    passed = require(initialState && initialState->status == 200 &&
        nlohmann::json::parse(initialState->body).contains("configuredRoutes"),
        "GET state exposes saved routes separately from active routes") && passed;
    passed = require(initialState && initialState->status == 200 &&
        nlohmann::json::parse(initialState->body).at("bufferHealth").is_null(),
        "GET state reports no physical buffer observation before callbacks") && passed;
    passed = require(initialState && initialState->status == 200 &&
        nlohmann::json::parse(initialState->body).at("performanceMeasurements") ==
            nlohmann::json{{"enabled", false}, {"sampleEveryCallbacks", 16}, {"window", nullptr}},
        "GET state exposes disabled measurements with a null window") && passed;
    const auto diagnosticState = client.Get("/api/v1/diagnostics?limit=10");
    passed = require(diagnosticState && diagnosticState->status == 200 &&
        nlohmann::json::parse(diagnosticState->body)["level"] == "info",
        "GET diagnostics returns the current level and entries") && passed;
    const auto debugLevel = client.Put("/api/v1/diagnostics", R"({"level":"debug"})", "application/json");
    passed = require(debugLevel && debugLevel->status == 200 &&
        nlohmann::json::parse(debugLevel->body)["level"] == "debug",
        "PUT diagnostics persists debug level") && passed;
    const auto invalidLimit = client.Get("/api/v1/diagnostics?limit=501");
    passed = require(invalidLimit && invalidLimit->status == 400,
        "GET diagnostics rejects an out of range limit") && passed;
    const auto initial = client.Get("/api/v1/application-profiles");
    passed = require(initial && initial->status == 200,
        "GET application profiles returns the configured profile list") && passed;
    if (initial && initial->status == 200) {
        const auto body = nlohmann::json::parse(initial->body);
        passed = require(body["profiles"].size() == 1 &&
            body["profiles"][0]["processName"] == "mixxx.exe" &&
            body["profiles"][0]["inputChannels"] == 255 &&
            body["profiles"][0]["outputChannels"] == 255,
            "GET exposes the default Mixxx compatibility profile") && passed;
    }

    const auto update = client.Put("/api/v1/application-profiles", R"({"profiles":[{"processName":"Mixxx.exe","inputChannels":256,"outputChannels":192}]})",
        "application/json");
    passed = require(update && update->status == 200,
        "PUT replaces application profiles") && passed;
    if (update && update->status == 200) {
        const auto body = nlohmann::json::parse(update->body);
        passed = require(body["profiles"].size() == 1 &&
            body["profiles"][0]["processName"] == "mixxx.exe" &&
            body["profiles"][0]["inputChannels"] == 256 &&
            body["profiles"][0]["outputChannels"] == 192 &&
            body["restartRequiredClients"].empty(),
            "PUT returns normalized profiles and clients requiring restart") && passed;
    }

    const auto invalid = client.Put("/api/v1/application-profiles", R"({"profiles":[{"processName":"mixxx.exe","inputChannels":257,"outputChannels":255}]})",
        "application/json");
    passed = require(invalid && invalid->status == 400,
        "PUT rejects channel counts above 256") && passed;

    const auto reset = client.Put("/api/v1/application-profiles", R"({"profiles":[]})",
        "application/json");
    passed = require(reset && reset->status == 200,
        "PUT with an empty list resets all application overrides") && passed;
    const auto afterReset = client.Get("/api/v1/application-profiles");
    if (!afterReset || afterReset->status != 200) passed = false;
    else {
        const auto body = nlohmann::json::parse(afterReset->body);
        passed = require(body["profiles"].empty(),
            "an explicitly empty profile list survives API readback") && passed;
    }

    httplib::Headers websocketHeaders{{"Sec-WebSocket-Protocol", "vasio.api.v1"}};
    httplib::ws::WebSocketClient websocket("ws://127.0.0.1:" + std::to_string(port) + "/api/v1/ws", websocketHeaders);
    websocket.set_read_timeout(2);
    passed = require(static_cast<bool>(websocket.connect()), "WebSocket accepts the documented subprotocol") && passed;
    std::string websocketMessage;
    const auto acknowledged = [&](const char* id) {
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (websocket.read(websocketMessage) != httplib::ws::Text) break;
            const auto response = nlohmann::json::parse(websocketMessage);
            if (response.value("id", std::string{}) == id) return response.value("success", false);
        }
        return false;
    };
    if (passed) {
        passed = require(websocket.send(R"({"id":"measure-on","command":"performance.measurements.set","payload":{"enabled":true}})"),
            "WebSocket accepts stage measurement enable") && passed;
        passed = require(acknowledged("measure-on"),
            "stage measurement enable is acknowledged") && passed;
        const auto measuringState = client.Get("/api/v1/state");
        passed = require(measuringState && measuringState->status == 200 &&
            nlohmann::json::parse(measuringState->body)["performanceMeasurements"]["enabled"] == true &&
            nlohmann::json::parse(measuringState->body)["performanceMeasurements"]["window"].is_null(),
            "stage measurement enable does not invent a physical observation") && passed;
        passed = require(websocket.send(R"({"id":"measure-repeat","command":"performance.measurements.set","payload":{"enabled":true}})"),
            "WebSocket accepts repeated stage measurement enable") && passed;
        passed = require(acknowledged("measure-repeat"),
            "repeated stage measurement enable is acknowledged") && passed;
        passed = require(websocket.send(R"({"id":"measure-off","command":"performance.measurements.set","payload":{"enabled":false}})"),
            "WebSocket accepts stage measurement disable") && passed;
        passed = require(acknowledged("measure-off"),
            "stage measurement disable is acknowledged") && passed;
        passed = require(websocket.send(R"({"id":"reset-test","command":"engine.reset"})"),
            "WebSocket accepts a driver reset request") && passed;
        bool resetAcknowledged = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (websocket.read(websocketMessage) != httplib::ws::Text) break;
            const auto response = nlohmann::json::parse(websocketMessage);
            if (response.value("id", std::string{}) == "reset-test") {
                resetAcknowledged = response.value("success", false);
                break;
            }
        }
        passed = require(resetAcknowledged,
            "driver reset is acknowledged without changing saved configuration") && passed;
        passed = require(websocket.send(R"({"id":"stop-test","command":"engine.stop"})"),
            "WebSocket accepts an engine.stop request") && passed;
        bool stopAcknowledged = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (websocket.read(websocketMessage) != httplib::ws::Text) break;
            const auto response = nlohmann::json::parse(websocketMessage);
            if (response.value("id", std::string{}) == "stop-test") {
                stopAcknowledged = response.value("success", false);
                break;
            }
        }
        passed = require(stopAcknowledged,
            "engine.stop stops audio while no client is attached") && passed;
        const auto stillServing = client.Get("/api/v1/state");
        passed = require(stillServing && stillServing->status == 200,
            "engine.stop keeps the control API available") && passed;
        passed = require(websocket.send(R"({"id":"start-test","command":"engine.start"})"),
            "WebSocket accepts an engine.start request") && passed;
        bool startAcknowledged = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (websocket.read(websocketMessage) != httplib::ws::Text) break;
            const auto response = nlohmann::json::parse(websocketMessage);
            if (response.value("id", std::string{}) == "start-test") {
                startAcknowledged = response.value("success", false);
                break;
            }
        }
        passed = require(startAcknowledged,
            "engine.start is acknowledged after audio stop") && passed;
        passed = require(!stopRequested.load(), "engine.stop does not shut down the API process") && passed;
        const auto clockCommand = [&](const nlohmann::json& request) {
            if (!websocket.send(request.dump())) return nlohmann::json{};
            for (int attempt = 0; attempt < 30; ++attempt) {
                if (websocket.read(websocketMessage) != httplib::ws::Text) break;
                const auto reply = nlohmann::json::parse(websocketMessage);
                if (reply.value("id", std::string{}) == request["id"].get<std::string>()) return reply;
            }
            return nlohmann::json{};
        };
        nlohmann::json softwarePayload{{"physicalDriverId", nullptr}, {"sampleRate", 48000},
            {"bufferFrames", 256}, {"routes", nlohmann::json::array()},
            {"clockSource", {{"kind", "software"}}}};
        const auto softwareReply = clockCommand({{"id", "software-on"},
            {"command", "configuration.apply"}, {"payload", softwarePayload}});
        passed = require(softwareReply.value("success", false), "API accepts software clock without hardware") && passed;
        const auto softwareState = client.Get("/api/v1/state");
        const auto clockState = nlohmann::json::parse(softwareState->body);
        passed = require(clockState["engine"].value("clockSource", nlohmann::json{}) ==
            nlohmann::json{{"kind", "software"}} && clockState["engine"]["state"] == "running" &&
            clockState["engine"]["physicalDriverId"].is_null(), "state identifies a running software clock") && passed;
        softwarePayload.erase("clockSource");
        softwarePayload["expectedConfigurationRevision"] = clockState["configurationRevision"];
        passed = require(clockCommand({{"id", "software-preserve"}, {"command", "configuration.apply"},
            {"payload", softwarePayload}}).value("success", false), "old client preserves selected clock") && passed;
        passed = require(!clockCommand({{"id", "software-stale"}, {"command", "configuration.apply"},
            {"payload", softwarePayload}}).value("success", true), "stale clock configuration is rejected") && passed;
        softwarePayload.erase("expectedConfigurationRevision");
        softwarePayload["clockSource"] = {{"kind", "unknown"}};
        passed = require(!clockCommand({{"id", "bad-clock"}, {"command", "configuration.apply"},
            {"payload", softwarePayload}}).value("success", true), "unknown clock source is rejected") && passed;
        AudioControllerConfiguration dormant;
        dormant.clockSource = AudioClockSource::Software;
        dormant.sampleRate = 96000;
        dormant.bufferFrames = 128;
        dormant.routes.push_back({"saved-hardware", "physical:unplugged:input:2",
            "virtual:TimoxVasio:app:absent.exe:input:1", 0.0, false});
        passed = require(controller.ApplyConfiguration(dormant).success, "fixture retains absent hardware route") && passed;
        softwarePayload["clockSource"] = {{"kind", "software"}};
        softwarePayload["sampleRate"] = 96000;
        softwarePayload["bufferFrames"] = 128;
        softwarePayload["routes"] = nlohmann::json::array({{{"id", "saved-hardware"},
            {"sourceEndpointId", "physical:unplugged:input:2"},
            {"destinationEndpointId", "virtual:TimoxVasio:app:absent.exe:input:1"},
            {"gainDb", 0.0}, {"mute", false}}});
        passed = require(clockCommand({{"id", "preserve-hardware"}, {"command", "configuration.apply"},
            {"payload", softwarePayload}}).value("success", false), "API preserves saved dormant hardware routes") && passed;
        // Mirror Control's transfer preparation: retain A's dormant routes while
        // discovering B. B lacks channel 2, so recovery must restore software
        // through the API even though A is absent from the physical inventory.
        auto replacementPayload = softwarePayload;
        replacementPayload["clockSource"] = {{"kind", "asio"}};
        replacementPayload["physicalDriverId"] = "replacement";
        replacementPayload["sampleRate"] = 48000;
        replacementPayload["bufferFrames"] = 64;
        passed = require(clockCommand({{"id", "discover-replacement"}, {"command", "configuration.apply"},
            {"payload", replacementPayload}}).value("success", false),
            "API opens replacement while retaining absent original hardware routes") && passed;
        const auto replacementState = client.Get("/api/v1/state");
        passed = require(replacementState && replacementState->status == 200,
            "replacement state is available") && passed;
        if (replacementState && replacementState->status == 200) {
            const auto body = nlohmann::json::parse(replacementState->body);
            passed = require(body["engine"]["clockSource"] == nlohmann::json{{"kind", "asio"}} &&
                body["engine"]["physicalDriverId"] == "replacement" &&
                body["configuredRoutes"] == softwarePayload["routes"] && body["routes"].empty(),
                "ASIO preparation preserves original routes as dormant") && passed;
            passed = require(body["physicalDrivers"].size() == 1 &&
                body["physicalDrivers"][0]["id"] == "replacement" &&
                body["physicalDrivers"][0]["inputEndpoints"].size() == 1,
                "original card is absent and replacement cannot map its second channel") && passed;
        }
        passed = require(clockCommand({{"id", "restore-software"}, {"command", "configuration.apply"},
            {"payload", softwarePayload}}).value("success", false),
            "API restores software without UNKNOWN_ENDPOINT for the absent original card") && passed;
        const auto restoredSoftware = client.Get("/api/v1/state");
        passed = require(restoredSoftware && restoredSoftware->status == 200,
            "restored software state is available") && passed;
        if (restoredSoftware && restoredSoftware->status == 200) {
            const auto body = nlohmann::json::parse(restoredSoftware->body);
            passed = require(body["engine"]["clockSource"] == softwarePayload["clockSource"] &&
                body["engine"]["physicalDriverId"].is_null() && body["engine"]["state"] == "running" &&
                body["engine"]["sampleRate"] == 96000 && body["engine"]["bufferFrames"] == 128 &&
                body["configuredRoutes"] == softwarePayload["routes"] && body["routes"].empty(),
                "software recovery confirms the original source, format and every dormant route") && passed;
        }
        passed = require(clockCommand({{"id", "software-stop"}, {"command", "engine.stop"}}).value("success", false),
            "software engine stops through API") && passed;
        websocket.close();
    }

    api.Stop();
    controller.Stop();
    DeleteFileW(configPath.c_str());
    DeleteFileW(profilesPath.c_str());
    if (!passed) return 1;
    std::puts("PASS: application profile API is reachable over local HTTP.");
    return 0;
}
