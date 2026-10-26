#include <cstdio>

#if __has_include("engine_diagnostics.h")
#include "engine_diagnostics.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path testDirectory() {
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) throw std::runtime_error("could not locate test executable");
    return std::filesystem::path(executable).parent_path() /
        (L"engine-diagnostics-test-" + std::to_wstring(GetCurrentProcessId()));
}

void testLevelFilteringAndPersistence(const std::filesystem::path& directory) {
    std::filesystem::remove_all(directory);
    EngineDiagnostics diagnostics;
    std::string error;
    require(diagnostics.Initialize(directory.wstring(), error), error.c_str());
    diagnostics.Write(EngineDiagnostics::Level::Info, "test", "normal entry");
    diagnostics.Write(EngineDiagnostics::Level::Debug, "test", "hidden entry");
    auto entries = diagnostics.ReadRecent(10);
    require(entries.size() == 1 && entries[0].message == "normal entry",
        "info level must omit debug entries");

    require(diagnostics.SetLevel(EngineDiagnostics::Level::Debug, error), error.c_str());
    diagnostics.Write(EngineDiagnostics::Level::Debug, "test", "visible entry");
    entries = diagnostics.ReadRecent(10);
    require(entries.size() == 2 && entries[1].message == "visible entry",
        "debug level must retain debug entries");

    EngineDiagnostics restored;
    require(restored.Initialize(directory.wstring(), error), error.c_str());
    require(restored.GetLevel() == EngineDiagnostics::Level::Debug,
        "debug level must persist across logger instances");
    std::filesystem::remove_all(directory);
}

void testBoundedReadAndRotation(const std::filesystem::path& directory) {
    std::filesystem::remove_all(directory);
    EngineDiagnostics diagnostics;
    std::string error;
    require(diagnostics.Initialize(directory.wstring(), error), error.c_str());
    const std::string payload(1024 * 1024, 'x');
    for (int index = 0; index < 7; ++index)
        diagnostics.Write(EngineDiagnostics::Level::Info, "rotation", payload);

    const auto entries = diagnostics.ReadRecent(2);
    require(entries.size() == 2, "ReadRecent must return no more than its requested limit");
    std::size_t archives = 0;
    for (const auto& item : std::filesystem::directory_iterator(directory))
        if (item.path().filename().wstring().find(L"engine.log.") == 0) ++archives;
    require(archives > 0 && archives <= 4, "rotation must retain between one and four archives");
    std::filesystem::remove_all(directory);
}

void testConcurrentWriteAndRead(const std::filesystem::path& directory) {
    std::filesystem::remove_all(directory);
    EngineDiagnostics diagnostics;
    std::string error;
    require(diagnostics.Initialize(directory.wstring(), error), error.c_str());
    std::thread writer([&] {
        for (int index = 0; index < 500; ++index)
            diagnostics.Write(EngineDiagnostics::Level::Info, "concurrent", std::to_string(index));
    });
    for (int index = 0; index < 100; ++index)
        require(diagnostics.ReadRecent(20).size() <= 20, "concurrent reads must remain bounded");
    writer.join();
    const auto entries = diagnostics.ReadRecent(500);
    require(entries.size() == 500 && entries.front().message == "0" && entries.back().message == "499",
        "concurrent reads must observe complete ordered entries");
    std::filesystem::remove_all(directory);
}
}

int main() {
    try {
        const auto directory = testDirectory();
        testLevelFilteringAndPersistence(directory / L"levels");
        testBoundedReadAndRotation(directory / L"rotation");
        testConcurrentWriteAndRead(directory / L"concurrent");
        std::puts("PASS: engine diagnostics logger tests");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

#else
int main() {
    std::fputs("FAIL: EngineDiagnostics is not implemented\n", stderr);
    return 1;
}
#endif
