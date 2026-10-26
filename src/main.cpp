#include "audio_controller.h"
#include "physical_asio_host.h"
#include "control_api_server.h"
#include "vasio_client_manager.h"
#include "engine_diagnostics.h"
#include "engine_instance_guard.h"
#include "native_audio_installed_transport_win32.h"
#include "audio_configuration_store.h"
#include "application_profile_store.h"

#include <windows.h>
#if defined(VASIO_ENABLE_CRASH_DUMP)
#include <dbghelp.h>
#endif

#include <cstdio>
#include <iostream>
#include <string>

namespace {
HANDLE gStopEvent = nullptr;

#if defined(VASIO_ENABLE_CRASH_DUMP)
LONG WINAPI writeCrashDump(EXCEPTION_POINTERS* exceptionPointers) {
    wchar_t dumpPath[MAX_PATH]{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, dumpPath, MAX_PATH);
    if (pathLength > 0 && pathLength < MAX_PATH) {
        wchar_t* fileName = wcsrchr(dumpPath, L'\\');
        const std::size_t prefixLength = fileName
            ? static_cast<std::size_t>(fileName + 1 - dumpPath) : 0;
        if (prefixLength < MAX_PATH) {
            swprintf_s(dumpPath + prefixLength, MAX_PATH - prefixLength,
                L"TimoxVirtualAsioEngine-%lu.dmp", GetCurrentProcessId());
            HANDLE dumpFile = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (dumpFile != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{
                    GetCurrentThreadId(), exceptionPointers, FALSE};
                const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                    dumpFile, static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo |
                        MiniDumpWithIndirectlyReferencedMemory),
                    exceptionPointers ? &exceptionInfo : nullptr, nullptr, nullptr);
                CloseHandle(dumpFile);
                if (written) std::fwprintf(stderr, L"Crash dump written: %ls\n", dumpPath);
                else std::fprintf(stderr, "MiniDumpWriteDump failed: %lu\n", GetLastError());
            } else {
                std::fprintf(stderr, "Could not create engine crash dump: %lu\n", GetLastError());
            }
        }
    }
    if (exceptionPointers && exceptionPointers->ExceptionRecord) {
        std::fprintf(stderr, "Unhandled engine exception 0x%08lx at %p\n",
            exceptionPointers->ExceptionRecord->ExceptionCode,
            exceptionPointers->ExceptionRecord->ExceptionAddress);
        std::fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

BOOL WINAPI consoleControlHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_SHUTDOWN_EVENT) {
        if (gStopEvent) SetEvent(gStopEvent);
        return TRUE;
    }
    return FALSE;
}

int listAsioDrivers() {
    PhysicalAsioHost host;
    const auto drivers = host.enumerate();
    for (const auto& driver : drivers) {
        std::wcout << driver.id << L"\t" << std::wstring(driver.name.begin(), driver.name.end()) << L"\n";
    }
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
#if defined(VASIO_ENABLE_CRASH_DUMP)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(writeCrashDump);
#endif
    if (argc == 2 && std::wstring(argv[1]) == L"--list-asio") return listAsioDrivers();
    if (argc > 1) {
        std::fwprintf(stderr, L"Usage: TimoxVirtualAsioEngine.exe [--list-asio]\n");
        return 2;
    }

    EngineInstanceGuard instance;
    if (!instance.acquired()) {
        if (instance.error() == ERROR_ALREADY_EXISTS)
            std::fprintf(stderr, "A TimoxVasio engine is already running; no audio device was opened.\n");
        else
            std::fprintf(stderr, "Cannot establish exclusive engine ownership (Windows error %lu); no audio device was opened.\n", instance.error());
        return 3;
    }

    EngineDiagnostics diagnostics;
    std::string diagnosticsError;
    const auto diagnosticsDirectory = EngineDiagnostics::DefaultDirectory();
    if (diagnosticsDirectory.empty() || !diagnostics.Initialize(diagnosticsDirectory, diagnosticsError)) {
        std::fprintf(stderr, "Impossible d’initialiser les journaux moteur: %s\n", diagnosticsError.c_str());
        return 1;
    }
    diagnostics.Write(EngineDiagnostics::Level::Info, "engine", "Engine process started");

    VasioClientManager clients;
    if (!clients.Start()) {
        diagnostics.Write(EngineDiagnostics::Level::Error, "clients", "Unable to start VASIO client discovery");
        std::fprintf(stderr, "Impossible de démarrer la découverte des clients VASIO.\n");
        return 1;
    }
    AudioController controller(clients,
        AudioConfigurationStore::DefaultPath(),ApplicationProfileStore::DefaultPath(),
        {},{},{},[] {return CreateInstalledNativeAudioTransport();});
    if (!controller.Start()) {
        diagnostics.Write(EngineDiagnostics::Level::Error, "audio", "Unable to start the audio controller");
        clients.Stop();
        std::fprintf(stderr, "Impossible de démarrer le contrôleur audio.\n");
        return 1;
    }
    ControlApiServer api(clients, controller, diagnostics, [] {
        if (gStopEvent) SetEvent(gStopEvent);
    });
    std::uint16_t port = 0;
    std::string apiError;
    if (!api.Start(52525, port, apiError)) {
        diagnostics.Write(EngineDiagnostics::Level::Error, "api", apiError);
        controller.Stop();
        clients.Stop();
        std::fprintf(stderr, "Impossible de démarrer l’API locale: %s\n", apiError.c_str());
        return 1;
    }
    gStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!gStopEvent || !SetConsoleCtrlHandler(consoleControlHandler, TRUE)) {
        diagnostics.Write(EngineDiagnostics::Level::Error, "engine", "Unable to initialize the orderly shutdown signal");
        if (gStopEvent) CloseHandle(gStopEvent);
        api.Stop();
        controller.Stop();
        clients.Stop();
        std::fprintf(stderr, "Impossible d’initialiser l’arrêt du moteur.\n");
        return 1;
    }

    std::printf("{\"event\":\"api.ready\",\"port\":%u}\n", static_cast<unsigned>(port));
    diagnostics.Write(EngineDiagnostics::Level::Info, "api", "Local control API listening on port " + std::to_string(port));
    std::fflush(stdout);
    WaitForSingleObject(gStopEvent, INFINITE);
    SetConsoleCtrlHandler(consoleControlHandler, FALSE);
    api.Stop();
    controller.Stop();
    clients.Stop();
    diagnostics.Write(EngineDiagnostics::Level::Info, "engine", "Engine process stopped");
    CloseHandle(gStopEvent);
    gStopEvent = nullptr;
    return 0;
}
