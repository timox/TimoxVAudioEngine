#include "application_profile_store.h"

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
    return std::wstring(directory) + L"TimoxVasio-application-profiles-test-" +
        std::to_wstring(GetCurrentProcessId()) + L".json";
}
}

int wmain() {
    const auto path = testPath();
    if (path.empty()) return 2;
    DeleteFileW(path.c_str());
    ApplicationProfileStore store(path);
    std::vector<ApplicationProfile> profiles;
    std::string error;
    bool passed = require(store.Load(profiles, error), "missing profile file loads defaults") &&
        require(profiles.size() == 1 && profiles[0].processName == L"mixxx.exe" &&
            profiles[0].inputChannels == 255 && profiles[0].outputChannels == 255,
            "default Mixxx profile caps both directions at 255");

    const auto defaultMixxx = ApplicationProfileStore::EffectiveProfileForExecutable(
        L"C:\\Program Files\\Mixxx\\MIXXX.EXE", profiles);
    const auto defaultOther = ApplicationProfileStore::EffectiveProfileForExecutable(
        L"C:\\Audio\\Other.exe", profiles);
    passed = require(defaultMixxx.inputChannels == 255 && defaultMixxx.outputChannels == 255,
        "profile lookup matches the executable basename without case sensitivity") && passed;
    passed = require(defaultOther.inputChannels == 256 && defaultOther.outputChannels == 256,
        "unlisted applications retain 256 channels") && passed;

    std::vector<ApplicationProfile> custom{{L"Mixxx.exe", 256, 192}, {L"other.exe", 64, 128}};
    passed = require(store.Save(custom, error), "valid application profiles save atomically") && passed;
    profiles.clear();
    passed = require(store.Load(profiles, error), "saved application profiles reload") && passed;
    const auto customMixxx = ApplicationProfileStore::EffectiveProfileForExecutable(
        L"Mixxx.exe", profiles);
    const auto customOther = ApplicationProfileStore::EffectiveProfileForExecutable(
        L"Other.exe", profiles);
    passed = require(customMixxx.inputChannels == 256 && customMixxx.outputChannels == 192,
        "API profile can override the built-in Mixxx limit") && passed;
    passed = require(customOther.inputChannels == 64 && customOther.outputChannels == 128,
        "input and output channel limits are independently preserved") && passed;

    std::vector<ApplicationProfile> duplicate{{L"Mixxx.exe", 255, 255}, {L"mixxx.EXE", 128, 128}};
    passed = require(!store.Save(duplicate, error), "duplicate case-insensitive executable names are rejected") && passed;
    std::vector<ApplicationProfile> invalid{{L"..\\Mixxx.exe", 255, 255}};
    passed = require(!store.Save(invalid, error), "profile paths are rejected") && passed;
    invalid = {{L"Mixxx.exe", 257, 255}};
    passed = require(!store.Save(invalid, error), "channel counts above 256 are rejected") && passed;

    DeleteFileW(path.c_str());
    HANDLE malformed = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (malformed == INVALID_HANDLE_VALUE) passed = false;
    else {
        constexpr char invalidJson[] = "{";
        DWORD written = 0;
        passed = WriteFile(malformed, invalidJson, sizeof(invalidJson) - 1, &written, nullptr) &&
            written == sizeof(invalidJson) - 1 && passed;
        CloseHandle(malformed);
        passed = require(!store.Load(profiles, error), "malformed profile files report an error") && passed;
    }
    DeleteFileW(path.c_str());
    if (!passed) return 1;
    std::puts("PASS: per-application profiles default, persist, normalize and validate correctly.");
    return 0;
}
