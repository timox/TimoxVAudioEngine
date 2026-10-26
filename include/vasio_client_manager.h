#pragma once

#include "audio_transport.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct VasioClientInfo {
    std::uint32_t driverId;
    std::uint32_t processId;
    std::wstring processName;
};

struct VasioClientSnapshot {
    VasioClientInfo info;
    std::shared_ptr<AudioClientMapping> mapping;
};

// Attaches only to processes with the unique TimoxVasio.dll module and
// a matching versioned mapping. It does not start or control those processes.
class VasioClientManager final {
public:
    VasioClientManager() = default;
    ~VasioClientManager();
    VasioClientManager(const VasioClientManager&) = delete;
    VasioClientManager& operator=(const VasioClientManager&) = delete;

    bool Start();
    void Stop();
    // Pins the mapping lifetime while a controller builds or publishes an audio snapshot.
    std::vector<VasioClientSnapshot> GetClientSnapshots() const;
    bool TryReserveShutdown();
    void SetDriverConfiguration(std::uint32_t preferredBufferFrames, std::uint32_t sampleRate);

private:
    struct Client {
        VasioClientInfo info;
        // Owns the stable control attachment across audio epoch replacements.
        std::shared_ptr<AudioClientMapping> control;
        std::shared_ptr<AudioClientMapping> mapping;
    };

    void Run() noexcept;
    void Scan();

    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<Client> clients_;
    std::uint32_t preferredBufferFrames_{256};
    std::uint32_t sampleRate_{};
    bool shutdownReserved_ = false;
};
