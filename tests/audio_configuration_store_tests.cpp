#include "audio_configuration_store.h"
#include "audio_controller.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {
bool require(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

std::wstring testPath() {
    wchar_t directory[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory)) return {};
    return std::wstring(directory) + L"TimoxVasio-configuration-store-test-" +
        std::to_wstring(GetCurrentProcessId()) + L".json";
}

bool writeConfiguration(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool success = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size();
    CloseHandle(file);
    return success;
}

std::string readConfiguration(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    const DWORD size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size > 1024 * 1024) { CloseHandle(file); return {}; }
    std::string contents(size, '\0');
    DWORD read = 0;
    const bool success = ReadFile(file, contents.data(), size, &read, nullptr) && read == size;
    CloseHandle(file);
    return success ? contents : std::string{};
}
}

int wmain() {
    const auto path = testPath();
    if (path.empty()) return 2;
    DeleteFileW(path.c_str());

    AudioConfigurationStore store(path);
    AudioControllerConfiguration restored;
    std::string error;
    const auto missing = store.Load(restored, error);
    bool passed = require(missing == ConfigurationLoadStatus::Missing,
        "a missing configuration is reported as missing");

    passed = require(writeConfiguration(path,
        R"({"version":1,"physicalDriverId":null,"sampleRate":null,"bufferFrames":null,"routes":[],"nativeAudio":{"enabled":true}})") &&
        store.Load(restored, error) == ConfigurationLoadStatus::Loaded,
        "native desired configuration loads without activating a backend") && passed;
    passed = require(restored.nativeAudio.enabled, "native enabled desire decoded") && passed;
    const std::string nativePrefix = R"({"version":1,"physicalDriverId":null,"sampleRate":null,"bufferFrames":null,"routes":[],"nativeAudio":)";
    for (const auto* invalid : {"null", "true", "{}", R"({"enabled":1})",
            R"({"enabled":"true"})", R"({"enabled":false,"transport":"direct"})"}) {
        restored.nativeAudio.enabled = true;
        passed = require(writeConfiguration(path, nativePrefix + invalid + "}") &&
            store.Load(restored, error) == ConfigurationLoadStatus::Error && restored.nativeAudio.enabled,
            "malformed native configuration rejected without mutating output") && passed;
    }
    passed = require(writeConfiguration(path,
        R"({"version":1,"physicalDriverId":null,"sampleRate":null,"bufferFrames":null,"routes":[]})") &&
        store.Load(restored, error) == ConfigurationLoadStatus::Loaded && !restored.nativeAudio.enabled,
        "legacy store resets native desire to disabled") && passed;
    for (const bool enabled : {true, false}) {
        AudioControllerConfiguration native;
        native.nativeAudio.enabled = enabled;
        passed = require(store.Save(native, error) && store.Load(restored, error) == ConfigurationLoadStatus::Loaded &&
            restored.nativeAudio.enabled == enabled,
            "native desired configuration round trips both boolean values") && passed;
    }

    AudioControllerConfiguration expected;
    expected.physicalDriverId = "{physical-driver}";
    expected.sampleRate = 48000;
    expected.bufferFrames = 512;
    expected.routes.push_back({"route-1", "virtual:TimoxVasio:123:output:1",
        "physical:driver:output:2", -3.5, false});
    passed = require(store.Save(expected, error), "a valid configuration is saved") && passed;
    const auto status = store.Load(restored, error);
    passed = require(status == ConfigurationLoadStatus::Loaded,
        "a saved configuration is loaded") && passed;
    if (status == ConfigurationLoadStatus::Loaded) {
        passed = require(restored.physicalDriverId == expected.physicalDriverId,
            "physical driver identity is preserved") && passed;
        passed = require(restored.sampleRate == expected.sampleRate &&
            restored.bufferFrames == expected.bufferFrames,
            "effective sample rate and buffer size are preserved") && passed;
        passed = require(restored.routes.size() == 1 &&
            restored.routes[0].id == expected.routes[0].id &&
            restored.routes[0].sourceEndpointId == expected.routes[0].sourceEndpointId &&
            restored.routes[0].destinationEndpointId == expected.routes[0].destinationEndpointId &&
            restored.routes[0].gainDb == expected.routes[0].gainDb &&
            restored.routes[0].mute == expected.routes[0].mute,
            "complete route data is preserved") && passed;
    }

    expected.routes[0].sourceEndpointId = "virtual:TimoxVasio:app:renoise.exe:output:1";
    passed = require(store.Save(expected, error), "an executable-bound route is saved") && passed;
    passed = require(store.Load(restored, error) == ConfigurationLoadStatus::Loaded &&
        restored.routes.size() == 1 &&
        restored.routes[0].sourceEndpointId == expected.routes[0].sourceEndpointId,
        "an executable-bound route survives a configuration reload") && passed;

    expected.clockSource = AudioClockSource::Software;
    expected.physicalDriverId.reset();
    passed = require(store.Save(expected, error) &&
        store.Load(restored, error) == ConfigurationLoadStatus::Loaded &&
        restored.clockSource == AudioClockSource::Software && !restored.physicalDriverId,
        "software clock selection survives reload") && passed;

    const auto savedClockBytes = readConfiguration(path);
    passed = require(!savedClockBytes.empty(), "clock preservation fixture is readable") && passed;
    const auto rejectClockSave = [&](const AudioControllerConfiguration& invalid) {
        return require(!store.Save(invalid, error) && !error.empty() &&
            readConfiguration(path) == savedClockBytes &&
            store.Load(restored, error) == ConfigurationLoadStatus::Loaded &&
            restored.clockSource == expected.clockSource && restored.physicalDriverId == expected.physicalDriverId &&
            restored.sampleRate == expected.sampleRate && restored.bufferFrames == expected.bufferFrames,
            "invalid clock save preserves previous file byte-for-byte and remains loadable");
    };
    auto badClock = expected;
    badClock.clockSource = static_cast<AudioClockSource>(99);
    passed = rejectClockSave(badClock) && passed;
    badClock = expected; badClock.physicalDriverId = "unexpected-hardware";
    passed = rejectClockSave(badClock) && passed;
    badClock = expected; badClock.sampleRate.reset();
    passed = rejectClockSave(badClock) && passed;
    badClock = expected; badClock.bufferFrames.reset();
    passed = rejectClockSave(badClock) && passed;
    for (const std::uint32_t rate : {0u, 7999u, 384001u}) {
        badClock = expected; badClock.sampleRate = rate;
        passed = rejectClockSave(badClock) && passed;
    }
    for (const std::uint32_t frames : {0u, 15u, 8193u}) {
        badClock = expected; badClock.bufferFrames = frames;
        passed = rejectClockSave(badClock) && passed;
    }
    badClock = expected; badClock.clockSource = AudioClockSource::Asio; badClock.physicalDriverId = "";
    passed = rejectClockSave(badClock) && passed;
    badClock.physicalDriverId = "card"; badClock.sampleRate = 0;
    passed = rejectClockSave(badClock) && passed;
    badClock.sampleRate = 48000; badClock.bufferFrames = 0;
    passed = rejectClockSave(badClock) && passed;

    WasapiBridgeConfiguration capture;
    capture.id = "Capture_1";
    capture.open.endpointId = L"{opaque-device}-\u00e9-\U0001f3a7";
    capture.open.bufferDuration100ns = 10000000;
    WasapiBridgeConfiguration render;
    render.id = "render-2"; render.enabled = false;
    render.open.kind = WasapiStreamKind::Render;
    render.open.defaultRole = WasapiDefaultRole::Communications;
    WasapiBridgeConfiguration loopback;
    loopback.id = "loopback"; loopback.open.kind = WasapiStreamKind::EndpointLoopback;
    loopback.open.defaultRole = WasapiDefaultRole::Multimedia;
    loopback.open.bufferDuration100ns = 100000;
    expected.wasapiBridges = {capture, render, loopback};
    passed = require(store.Save(expected, error) && store.Load(restored, error) == ConfigurationLoadStatus::Loaded &&
        restored.wasapiBridges.size() == 3, "WASAPI bridges save and reload") && passed;
    if (restored.wasapiBridges.size() == 3) {
        for (std::size_t i = 0; i < 3; ++i) {
            const auto& actual = restored.wasapiBridges[i];
            const auto& original = expected.wasapiBridges[i];
            passed = require(actual.id == original.id && actual.enabled == original.enabled &&
                actual.open.kind == original.open.kind && actual.open.endpointId == original.open.endpointId &&
                actual.open.defaultRole == original.open.defaultRole &&
                actual.open.bufferDuration100ns == original.open.bufferDuration100ns,
                "WASAPI identity, Unicode selector, enabled state, mode and duration round-trip") && passed;
        }
    }
    auto invalidSave = expected;
    invalidSave.wasapiBridges[0].open.endpointId = std::wstring(1, wchar_t{0xd800});
    passed = require(!store.Save(invalidSave, error) && !error.empty() &&
        store.Load(restored, error) == ConfigurationLoadStatus::Loaded &&
        restored.wasapiBridges.size() == 3 &&
        restored.wasapiBridges[0].open.endpointId == capture.open.endpointId,
        "invalid UTF-16 save leaves existing file intact") && passed;
    std::wstring decodedEndpoint = L"unchanged";
    std::string encodedEndpoint = "unchanged";
    passed = require(!WasapiEndpointFromUtf8(std::string("\xc0\xaf", 2), decodedEndpoint) &&
        decodedEndpoint == L"unchanged" &&
        !WasapiEndpointToUtf8(std::wstring(L"a\0b", 3), encodedEndpoint) && encodedEndpoint == "unchanged",
        "invalid UTF-8 and embedded UTF-16 NUL are rejected without output mutation") && passed;

    const std::string legacy = R"({"version":1,"physicalDriverId":null,"sampleRate":null,"bufferFrames":null,"routes":[]})";
    passed = require(writeConfiguration(path, legacy) &&
        store.Load(restored, error) == ConfigurationLoadStatus::Loaded &&
        restored.clockSource == AudioClockSource::Asio && restored.wasapiBridges.empty(),
        "legacy configuration defaults to ASIO even after loading software") && passed;
    const auto bridgeDocument = [&legacy](const std::string& bridges) {
        auto text = legacy;
        text.insert(1, "\"wasapiBridges\":" + bridges + ",");
        return text;
    };
    passed = require(writeConfiguration(path, bridgeDocument(
        R"([{"id":"default","kind":"capture","defaultRole":"console"}])")) &&
        store.Load(restored, error) == ConfigurationLoadStatus::Loaded && restored.wasapiBridges.size() == 1 &&
        restored.wasapiBridges[0].enabled && restored.wasapiBridges[0].open.bufferDuration100ns == 0 &&
        restored.wasapiBridges[0].open.defaultRole == WasapiDefaultRole::Console,
        "optional enabled/duration default without changing explicit default role") && passed;
    for (const std::string bridges : {std::string{"null"}, std::string{"{}"},
        std::string{R"([{"id":"","kind":"capture","endpointId":"a"}])"},
        std::string{R"([{"id":"bad:id","kind":"capture","endpointId":"a"}])"},
        std::string{R"([{"id":"a","kind":"capture","endpointId":""}])"},
        std::string{R"([{"id":"a","kind":"capture","endpointId":null}])"},
        std::string{R"([{"id":"a","kind":"capture","endpointId":"a\u0000b"}])"},
        std::string{R"([{"id":"a","kind":"capture","endpointId":"a\ud800"}])"},
        std::string{R"([{"id":"a","kind":"capture"}])"},
        std::string{R"([{"id":"a","kind":"capture","defaultRole":"console","endpointId":"a"}])"},
        std::string{R"([{"id":"a","kind":"capture","defaultRole":null}])"},
        std::string{R"([{"id":"a","kind":"capture","defaultRole":"unknown"}])"},
        std::string{R"([{"id":"a","kind":"processLoopback","endpointId":"a"}])"},
        std::string{R"([{"id":"a","kind":"future","endpointId":"a"}])"},
        std::string{R"([{"id":"a","kind":"capture","endpointId":"a","unknown":1}])"},
        std::string{R"([{"id":"a","kind":"capture","endpointId":"a","enabled":1}])"},
        std::string{R"([{"id":"a","kind":"capture","endpointId":"a","bufferDuration100ns":-1}])"},
        std::string{R"([{"id":"a","kind":"capture","endpointId":"a","bufferDuration100ns":10000001}])"},
        std::string{R"([{"id":"a","kind":"capture","endpointId":"a","bufferDuration100ns":18446744073709551615}])"},
        std::string{R"([{"id":"a","kind":"capture","endpointId":"a","bufferDuration100ns":0.5}])"},
        std::string{R"([{"id":"a","kind":"capture","endpointId":"a"},{"id":"a","kind":"render","endpointId":"b"}])"}}) {
        passed = require(writeConfiguration(path, bridgeDocument(bridges)) &&
            store.Load(restored, error) == ConfigurationLoadStatus::Error &&
            restored.wasapiBridges.size() == 1 && restored.wasapiBridges[0].id == "default",
            "malformed WASAPI configuration rejects without mutating last decoded configuration") && passed;
    }
    auto bounded = expected;
    bounded.wasapiBridges.clear();
    for (unsigned i = 0; i < 32; ++i) { auto item = capture; item.id = "bridge" + std::to_string(i); bounded.wasapiBridges.push_back(item); }
    passed = require(store.Save(bounded, error) && store.Load(restored, error) == ConfigurationLoadStatus::Loaded &&
        restored.wasapiBridges.size() == 32, "32 bridges accepted") && passed;
    auto extra = capture; extra.id = "bridge32"; bounded.wasapiBridges.push_back(extra);
    passed = require(!store.Save(bounded, error), "33 bridges rejected on save") && passed;
    std::string oversized = "[";
    for (unsigned i = 0; i < 33; ++i) { if (i) oversized += ','; oversized += "{\"id\":\"b" + std::to_string(i) + "\",\"kind\":\"capture\",\"endpointId\":\"a\"}"; }
    oversized += ']';
    passed = require(writeConfiguration(path, bridgeDocument(oversized)) &&
        store.Load(restored, error) == ConfigurationLoadStatus::Error, "33 bridges rejected on load") && passed;
    extra.id = std::string(129, 'a');
    passed = require(!ValidateWasapiBridgeConfigurations({extra}, error), "bridge ID maximum enforced") && passed;
    extra.id = std::string(128, 'a'); extra.open.endpointId = std::wstring(32768, L'a');
    passed = require(ValidateWasapiBridgeConfigurations({extra}, error), "ID and endpoint bounds accepted") && passed;
    extra.open.endpointId.push_back(L'a');
    passed = require(!ValidateWasapiBridgeConfigurations({extra}, error), "endpoint maximum enforced") && passed;
    passed = require(writeConfiguration(path, bridgeDocument("[]")) &&
        store.Load(restored, error) == ConfigurationLoadStatus::Loaded && restored.wasapiBridges.empty(),
        "explicit empty bridge list clears previously loaded bridges") && passed;
    for (const std::string selection : {R"({"kind":"future"})", R"(null)",
            R"({"kind":"software","extra":1})"}) {
        auto invalid = legacy;
        invalid.insert(1, "\"clockSource\":" + selection + ",");
        passed = require(writeConfiguration(path, invalid) &&
            store.Load(restored, error) == ConfigurationLoadStatus::Error,
            "malformed or unsupported clock selection is rejected") && passed;
    }

    HANDLE malformed = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (malformed == INVALID_HANDLE_VALUE) passed = false;
    else {
        constexpr char invalid[] = "{";
        DWORD written = 0;
        passed = WriteFile(malformed, invalid, sizeof(invalid) - 1, &written, nullptr) &&
            written == sizeof(invalid) - 1 && passed;
        CloseHandle(malformed);
        passed = require(store.Load(restored, error) == ConfigurationLoadStatus::Error,
            "malformed configuration is reported as an error") && passed;
    }

    DeleteFileW(path.c_str());
    if (!passed) return 1;
    std::puts("PASS: configuration store distinguishes missing/invalid files and round-trips the complete configuration.");
    return 0;
}
