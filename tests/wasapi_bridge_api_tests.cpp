#include <httplib.h>
#include <nlohmann/json.hpp>
#include "audio_controller.h"
#include "control_api_server.h"
#include "engine_diagnostics.h"
#include "vasio_client_manager.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

// Disabled bridges exercise the actual HTTP/WebSocket/controller transaction
// without opening a Windows audio endpoint or depending on installed hardware.
int wmain(int argc, wchar_t** argv) {
    using Json = nlohmann::json;
    if (argc != 1 && (argc != 3 || std::wstring(argv[1]) != L"--snapshot-file")) return 2;
    Json messages = Json::array();
    wchar_t directory[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory)) return 2;
    const auto base = std::wstring(directory) + L"TimoxVasio-wasapi-api-" + std::to_wstring(GetCurrentProcessId());
    // Keep PWAR HTTP writes away from the real user's driver configuration.
    const auto pwarProfile = base + L"-pwar-profile";
    if (!CreateDirectoryW(pwarProfile.c_str(), nullptr)) return 2;
    struct ProfileCleanup {
        std::wstring path;
        ~ProfileCleanup() { DeleteFileW((path + L"\\pwarASIO.cfg").c_str()); RemoveDirectoryW(path.c_str()); }
    } profileCleanup{pwarProfile};
    if (!SetEnvironmentVariableW(L"USERPROFILE", pwarProfile.c_str())) return 2;
    const auto config = base + L"-config.json", profiles = base + L"-profiles.json";
    DeleteFileW(config.c_str()); DeleteFileW(profiles.c_str());
    VasioClientManager clients;
    AudioController controller(clients, config, profiles);
    EngineDiagnostics diagnostics;
    std::string error;
    if (!controller.Start() || !diagnostics.Initialize(base + L"-logs", error)) return 3;
    ControlApiServer api(clients, controller, diagnostics, [] {});
    std::uint16_t port = 0;
    if (!api.Start(0, port, error)) return 4;
    httplib::Client http("127.0.0.1", port);
    bool passed = true;
    auto check = [&](bool condition, const char* message) {
        if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); passed = false; }
    };
    auto state = [&]() {
        const auto response = http.Get("/api/v1/state");
        return response && response->status == 200 ? Json::parse(response->body) : Json::object();
    };
    auto initial = state();
    const auto pwarDefaults = http.Get("/api/v1/pwar");
    check(pwarDefaults && pwarDefaults->status == 200 && !Json::parse(pwarDefaults->body).value("exists", true),
        "PWAR GET supplies defaults without writing a file");
    const auto pwarSave = http.Put("/api/v1/pwar", R"({"address":"127.0.0.1","port":18321})", "application/json");
    check(pwarSave && pwarSave->status == 200 && Json::parse(pwarSave->body).value("reopenRequired", false),
        "PWAR PUT saves settings and explicitly requires reopen");
    for (const auto* invalid : {R"({"address":"256.1.1.1","port":8321})", R"({"address":"127.0.0.1","port":0})",
        R"({"address":"127.0.0.1","port":8321,"path":"other"})", R"({"address":"127.0.0.1","port":1.5})"}) {
        const auto rejected = http.Put("/api/v1/pwar", invalid, "application/json");
        check(rejected && rejected->status == 400, "PWAR rejects malformed settings");
    }
    const auto pwarRead = http.Get("/api/v1/pwar");
    check(pwarRead && pwarRead->status == 200 && Json::parse(pwarRead->body).value("port", 0) == 18321,
        "PWAR rejected writes preserve saved settings");
    check(state()["engine"] == initial["engine"], "PWAR settings do not open or reset audio");
    messages.push_back(initial);
    check(initial.contains("nativeAudio") && initial["nativeAudio"].value("state", "") == "disabled" &&
        !initial["nativeAudio"].value("enabled", true) && !initial["nativeAudio"].value("attached", true) &&
        initial["nativeAudio"].value("graphFormat", Json{}) == nullptr,
        "state exposes disabled native backend without inventing an active graph format");
    check(initial.value("apiVersion", "") == "1.1" && initial.contains("wasapiInventoryError") &&
        initial.value("configuredWasapiBridges", Json{}) == Json::array(), "API 1.1 exposes empty bridge configuration and inventory status");
    httplib::Headers headers{{"Sec-WebSocket-Protocol", "vasio.api.v1"}};
    httplib::ws::WebSocketClient ws("ws://127.0.0.1:" + std::to_string(port) + "/api/v1/ws", headers);
    ws.set_read_timeout(2);
    const bool connected = static_cast<bool>(ws.connect());
    check(connected, "API 1.1 retains v1 WebSocket subprotocol");
    unsigned serial = 0;
    auto apply = [&](const Json& payload) {
        const auto id = "bridge-test-" + std::to_string(++serial);
        if (!ws.send(Json{{"id", id}, {"command", "configuration.apply"}, {"payload", payload}}.dump())) return Json::object();
        std::string message;
        for (int i = 0; i < 40; ++i) {
            if (ws.read(message) != httplib::ws::Text) break;
            auto reply = Json::parse(message);
            if (reply.value("id", "") == id) return reply;
        }
        return Json::object();
    };
    if (connected) {
        Json payload{{"physicalDriverId", nullptr}, {"sampleRate", 48000}, {"bufferFrames", 256},
            {"clockSource", {{"kind", "software"}}}, {"routes", Json::array()},
            {"wasapiBridges", Json::array({{{"id", "capture_a"}, {"enabled", false}, {"kind", "capture"}, {"defaultRole", "communications"}},
                {{"id", "loopback_b"}, {"enabled", false}, {"kind", "endpointLoopback"}, {"endpointId", "test-render-endpoint"}, {"bufferDuration100ns", 100000}}})}};
        check(apply(payload).value("success", false), "API accepts disabled default and explicit loopback configurations");
        payload["nativeAudio"] = {{"enabled", true}};
        check(!apply(payload).value("success", true), "native activation remains rejected before authenticated API integration");
        payload.erase("nativeAudio");
        const auto configured = state();
        messages.push_back(configured);
        const auto bridges = configured.value("configuredWasapiBridges", Json{});
        check(bridges.is_array() && bridges.size() == 2, "state preserves both bridge selectors");
        const auto effective = configured.value("wasapiBridges", Json::array());
        check(effective.size() == 2 && effective[0]["state"] == "disabled" && effective[0]["generationId"].is_string() &&
            effective[0]["effectiveInfo"].is_null(), "disabled bridge has status without invented device format");
        check(effective[0]["health"].contains("controlBacklogUpdates") &&
            effective[0]["health"]["controlBacklogUpdates"] == 0,
            "API exposes cumulative control backlog diagnostic even for a disabled bridge");
        payload.erase("wasapiBridges"); payload.erase("clockSource");
        payload["expectedConfigurationRevision"] = configured.at("configurationRevision");
        check(apply(payload).value("success", false), "omitted bridge field is accepted");
        check(state().value("configuredWasapiBridges", Json{}) == bridges, "omitted bridge field preserves configuration transactionally");
        payload["wasapiBridges"] = Json::array();
        check(!apply(payload).value("success", true), "stale revision cannot remove bridges");
        check(state().value("configuredWasapiBridges", Json{}) == bridges, "stale rejection leaves bridge configuration intact");
        payload.erase("expectedConfigurationRevision");
        payload["wasapiBridges"] = Json::array({bridges[0], bridges[0]});
        check(!apply(payload).value("success", true), "duplicate bridge ids rejected by real API");
        payload["wasapiBridges"] = Json::array({{{"id", "invalid"}, {"enabled", false}, {"kind", "capture"}, {"endpointId", ""}}});
        check(!apply(payload).value("success", true), "empty explicit endpoint rejected by real API");
        check(state().value("configuredWasapiBridges", Json{}) == bridges, "malformed bridge updates leave configuration intact");
        payload["wasapiBridges"] = Json::array();
        check(apply(payload).value("success", false), "explicit empty bridges removes all bridges");
        const auto cleared = state();
        messages.push_back(cleared);
        check(cleared.value("configuredWasapiBridges", Json{}) == Json::array() && cleared.value("wasapiBridges", Json{}) == Json::array(),
            "removal clears desired and effective state");
        ws.close();
    }
    const auto inventory = http.Get("/api/v1/wasapi-devices");
    check(inventory && inventory->status == 200 && Json::parse(inventory->body).contains("wasapiInventoryError"),
        "inventory refresh returns explicit success or error state");
    if (inventory && inventory->status == 200) messages.push_back(Json::parse(inventory->body));
    api.Stop(); controller.Stop();
    DeleteFileW(config.c_str()); DeleteFileW(profiles.c_str());
    if (argc == 3) {
        std::ofstream output(std::filesystem::path(argv[2]), std::ios::binary);
        output << messages.dump(2);
        output.flush();
        check(output.good(), "write actual HTTP snapshots for JSON Schema validation");
    }
    return passed ? 0 : 1;
}
