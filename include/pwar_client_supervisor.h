#pragma once

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

class PwarClientSupervisor final {
public:
    PwarClientSupervisor() = default;
    ~PwarClientSupervisor();
    PwarClientSupervisor(const PwarClientSupervisor&) = delete;
    PwarClientSupervisor& operator=(const PwarClientSupervisor&) = delete;

    bool Start(const std::string& peerAddress, std::uint32_t maxQueueMs, std::string& error);
    bool Stop(std::string& error) noexcept;
    bool RequestStatus(std::string& error);
    nlohmann::json Snapshot() const;
    std::uint64_t Revision() const noexcept;

private:
    bool startLocked(const std::string& peerAddress, std::uint32_t maxQueueMs, std::string& error);
    bool stopLocked(std::string& error) noexcept;
    bool sendCommandLocked(const std::string& command, std::string& error);
    void readOutput(HANDLE pipe, bool standardError) noexcept;
    void processLine(std::string line, bool standardError) noexcept;
    void waitForExit() noexcept;
    void closeChild() noexcept;

    mutable std::mutex stateMutex_;
    std::mutex operationMutex_;
    std::condition_variable stateChanged_;
    HANDLE process_ = nullptr;
    HANDLE stdinWrite_ = nullptr;
    HANDLE stdoutRead_ = nullptr;
    HANDLE stderrRead_ = nullptr;
    DWORD processId_ = 0;
    DWORD exitCode_ = STILL_ACTIVE;
    bool processExited_ = true;
    bool startAccepted_ = false;
    bool stopRequested_ = false;
    std::string peerAddress_;
    std::uint32_t maxQueueMs_ = 100;
    std::string state_ = "stopped";
    std::string error_;
    std::string stderrTail_;
    nlohmann::json clientStatus_ = nlohmann::json::object();
    std::map<std::string, nlohmann::json> responses_;
    std::uint64_t requestSequence_ = 0;
    std::uint64_t revision_ = 0;
    std::thread stdoutThread_;
    std::thread stderrThread_;
    std::thread waitThread_;
};
