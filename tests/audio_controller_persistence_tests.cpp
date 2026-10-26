#include "audio_controller.h"
#include "vasio_client_manager.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {
bool require(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

std::wstring configurationPath() {
    wchar_t directory[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory)) return {};
    return std::wstring(directory) + L"TimoxVasio-controller-persistence-" +
        std::to_wstring(GetCurrentProcessId()) + L".json";
}

bool writeMalformedConfiguration(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    constexpr char malformed[] = "{";
    DWORD written = 0;
    const bool success = WriteFile(file, malformed, sizeof(malformed) - 1, &written, nullptr) &&
        written == sizeof(malformed) - 1;
    CloseHandle(file);
    return success;
}
}

int wmain() {
    const auto path = configurationPath();
    if (path.empty()) return 2;
    DeleteFileW(path.c_str());
    bool passed = true;

    {
        VasioClientManager clients;
        AudioController controller(clients, path);
        passed = require(controller.Start(), "controller starts with no saved configuration") && passed;
        const auto initial = controller.Snapshot();
        passed = require(initial.state == "stopped" && initial.lastError.empty() &&
            initial.revision == 0, "missing configuration leaves a clean stopped state") && passed;
        const auto measurementsOff = controller.ReadPerformanceMeasurements();
        passed = require(!measurementsOff.enabled && !measurementsOff.window,
            "stage measurements default off without a window") && passed;
        passed = require(controller.SetPerformanceMeasurementsEnabled(true).success,
            "stage measurements enable") && passed;
        passed = require(controller.SetPerformanceMeasurementsEnabled(true).success,
            "repeated enable is accepted") && passed;
        const auto measurementsEnabled = controller.ReadPerformanceMeasurements();
        passed = require(measurementsEnabled.enabled && !measurementsEnabled.window,
            "enabled measurement has no invented stopped-driver window") && passed;
        passed = require(controller.SetPerformanceMeasurementsEnabled(false).success,
            "stage measurements disable") && passed;
        const auto measurementsDisabled = controller.ReadPerformanceMeasurements();
        passed = require(!measurementsDisabled.enabled && !measurementsDisabled.window,
            "disable clears measurement state") && passed;

        const AudioControllerConfiguration emptyConfiguration;
        const auto applied = controller.ApplyConfiguration(emptyConfiguration, 0);
        passed = require(applied.success, "an empty stopped configuration applies") && passed;
        const auto savedRevision = controller.Snapshot().configurationRevision;
        passed = require(savedRevision == 1, "saved configuration revision advances") && passed;
        const auto conflict = controller.ApplyConfiguration(emptyConfiguration, 0);
        passed = require(!conflict.success && conflict.conflict &&
            controller.Snapshot().configurationRevision == savedRevision,
            "stale configuration revision cannot overwrite another apply") && passed;
        passed = require(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
            "successful configuration.apply writes the persistence file") && passed;

        auto nativeConfiguration = emptyConfiguration;
        nativeConfiguration.nativeAudio.enabled = true;
        passed = require(controller.ApplyConfiguration(nativeConfiguration, std::nullopt, false, false, false).success &&
            controller.Snapshot().configuredNativeAudio.enabled && controller.Snapshot().state == "stopped",
            "explicit native desire persists without activating any audio backend") && passed;
        passed = require(controller.ApplyConfiguration(emptyConfiguration).success &&
            controller.Snapshot().configuredNativeAudio.enabled,
            "legacy callers preserve the native desired configuration") && passed;
        const auto nativeRevision = controller.Snapshot().configurationRevision;
        const auto nativeConflict = controller.ApplyConfiguration(emptyConfiguration, 0, false, false, false);
        passed = require(nativeConflict.conflict && controller.Snapshot().configurationRevision == nativeRevision &&
            controller.Snapshot().configuredNativeAudio.enabled,
            "revision conflict cannot replace native desire") && passed;
        const auto temporaryPath = path + L".tmp";
        HANDLE heldTemporary = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        passed = require(heldTemporary != INVALID_HANDLE_VALUE, "save failure fixture locks temporary file") && passed;
        if (heldTemporary != INVALID_HANDLE_VALUE) {
            const auto rejected = controller.ApplyConfiguration(emptyConfiguration, std::nullopt, false, false, false);
            passed = require(!rejected.success && controller.Snapshot().configuredNativeAudio.enabled &&
                controller.Snapshot().configurationRevision == nativeRevision,
                "failed save preserves native desire and revision") && passed;
            CloseHandle(heldTemporary);
            DeleteFileW(temporaryPath.c_str());
        }
        passed = require(controller.ApplyConfiguration(emptyConfiguration, std::nullopt, false, false, false).success &&
            !controller.Snapshot().configuredNativeAudio.enabled,
            "explicit replacement clears native desire") && passed;
        passed = require(controller.ApplyConfiguration(nativeConfiguration, std::nullopt, false, false, false).success,
            "native restore fixture saved") && passed;
        controller.Stop();
    }

    {
        VasioClientManager clients;
        AudioController controller(clients, path);
        passed = require(controller.Start(), "controller starts after restart with a saved configuration") && passed;
        const auto restored = controller.Snapshot();
        passed = require(restored.state == "stopped" && restored.lastError.empty() &&
            restored.revision >= 2 && restored.configuredNativeAudio.enabled,
            "startup replays the saved configuration before Start returns") && passed;
        controller.Stop();
    }

    passed = require(writeMalformedConfiguration(path), "malformed startup fixture is written") && passed;
    {
        VasioClientManager clients;
        AudioController controller(clients, path);
        passed = require(controller.Start(), "controller remains available after a restore error") && passed;
        const auto failedRestore = controller.Snapshot();
        passed = require(failedRestore.state == "stopped" && !failedRestore.lastError.empty(),
            "invalid saved configuration leaves the engine stopped with an API-visible error") && passed;
        controller.Stop();
    }

    DeleteFileW(path.c_str());
    if (!passed) return 1;
    std::puts("PASS: successful configuration.apply survives controller restart; restore errors stay stopped and visible.");
    return 0;
}
