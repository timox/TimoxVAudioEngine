#include "audio_transport.h"
#include "audio_transport_layout.h"

#include <cstddef>
#include <string>

namespace {
constexpr std::uint32_t kMagic = 0x56534154; // VSAT
constexpr std::uint32_t kStatusMagic = 0x54535356; // VSST

std::wstring mapName(std::uint32_t pid, std::uint32_t driver,
    std::uint64_t creation) {
    return L"Local\\VASIO-Map-v9-" +
        std::to_wstring(pid) + L"-" + std::to_wstring(driver) + L"-" +
        std::to_wstring(creation);
}

std::uint64_t processCreationTime(std::uint32_t pid) noexcept {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return ok ? value.QuadPart : 0;
}
}

bool AudioClientMapping::ReadStatusReadOnly(std::uint32_t driver, std::uint32_t pid,
    StatusSnapshot& snapshot) {
    snapshot = {};
    if (!pid) return false;
    const auto creation = processCreationTime(pid);
    if (!creation) return false;
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, mapName(pid, driver, creation).c_str());
    if (!map) return false;
    void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(Shared));
    if (!view) { CloseHandle(map); return false; }
    const auto* shared = static_cast<const Shared*>(view);
    const bool valid = shared->magic == kMagic && shared->driverId == driver &&
        shared->processId == pid && shared->version == 9 &&
        shared->size == sizeof(Shared) && shared->processCreationTime == creation;
    if (valid) {
        snapshot.layoutVersion = shared->version;
        snapshot.processId = shared->processId;
        snapshot.driverId = shared->driverId;
        snapshot.confirmedSampleRate = static_cast<std::uint32_t>(shared->sampleRate);
        snapshot.confirmedBufferFrames = static_cast<std::uint32_t>(shared->blockFrames);
        snapshot.inputUnderruns = static_cast<std::uint64_t>(shared->inputUnderruns);
        snapshot.outputOverruns = static_cast<std::uint64_t>(shared->outputOverruns);
        snapshot.inputOverruns = static_cast<std::uint64_t>(shared->inputOverruns);
        snapshot.outputUnderruns = static_cast<std::uint64_t>(shared->outputUnderruns);
        snapshot.stableChannels = false;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const auto before = shared->activeChannelsGeneration;
            if (before & 1) continue;
            for (std::size_t word = 0; word < kChannelMaskWords; ++word) {
                snapshot.activeInputs[word] = static_cast<std::uint64_t>(shared->activeInputs[word]);
                snapshot.activeOutputs[word] = static_cast<std::uint64_t>(shared->activeOutputs[word]);
            }
            MemoryBarrier();
            if (before == shared->activeChannelsGeneration) {
                snapshot.stableChannels = true;
                break;
            }
        }
        {
            const auto& status = shared->status;
            if (status.magic == kStatusMagic && status.schemaVersion == 2 &&
                status.size == sizeof(ClientStatusV2) && status.ready == 1) {
                MemoryBarrier();
                snapshot.available = true;
                for (std::size_t index = 0; index < 4; ++index)
                    snapshot.dllVersion[index] = status.dllVersion[index];
                std::size_t pathLength = 0;
                while (pathLength < std::size(status.dllPath) && status.dllPath[pathLength])
                    ++pathLength;
                snapshot.dllPath.assign(status.dllPath, status.dllPath + pathLength);
                snapshot.state = static_cast<ClientState>(status.state);
                snapshot.requestedSampleRate = static_cast<std::uint32_t>(status.requestedSampleRate);
                snapshot.requestedBufferFrames = static_cast<std::uint32_t>(status.requestedBufferFrames);
                snapshot.qpcFrequency = static_cast<std::uint64_t>(status.qpcFrequency);
                snapshot.stableExchange = false;
                for (int attempt = 0; attempt < 16; ++attempt) {
                    const auto before = status.exchangeGeneration;
                    if (before & 1) continue;
                    snapshot.firstCallbackQpc = static_cast<std::uint64_t>(status.firstCallbackQpc);
                    snapshot.lastCallbackQpc = static_cast<std::uint64_t>(status.lastCallbackQpc);
                    snapshot.lastSuccessfulExchangeQpc = static_cast<std::uint64_t>(status.lastSuccessfulExchangeQpc);
                    snapshot.lastIntervalQpc = static_cast<std::uint64_t>(status.lastIntervalQpc);
                    snapshot.callbackCount = static_cast<std::uint64_t>(status.callbackCount);
                    snapshot.successfulExchangeCount = static_cast<std::uint64_t>(status.successfulExchangeCount);
                    snapshot.exchangedFrames = static_cast<std::uint64_t>(status.exchangedFrames);
                    MemoryBarrier();
                    if (before == status.exchangeGeneration) {
                        snapshot.stableExchange = true;
                        break;
                    }
                }
                snapshot.resetRequests = static_cast<std::uint64_t>(status.resetRequests);
            }
        }
    }
    UnmapViewOfFile(view);
    CloseHandle(map);
    return valid;
}
