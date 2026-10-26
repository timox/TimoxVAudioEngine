#include "vasio_client_manager.h"

#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace {
constexpr DWORD kScanPeriodMs = 250;

int driverIdFromModule(const wchar_t* name) {
    return _wcsicmp(name, L"TimoxVasio.dll") == 0 ? 1 : 0;
}

std::uint64_t processCreationTime(std::uint32_t pid) noexcept {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool valid = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    ULARGE_INTEGER time{};
    time.LowPart = created.dwLowDateTime;
    time.HighPart = created.dwHighDateTime;
    return valid ? time.QuadPart : 0;
}
}

VasioClientManager::~VasioClientManager() { Stop(); }

bool VasioClientManager::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return true;
    try {
        thread_ = std::thread(&VasioClientManager::Run, this);
    } catch (...) {
        running_.store(false);
        return false;
    }
    return true;
}

void VasioClientManager::Stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
    } else if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& client : clients_) {
        client.control->SetEngineAttached(false);
        client.control->SignalClient();
    }
    clients_.clear();
}

std::vector<VasioClientSnapshot> VasioClientManager::GetClientSnapshots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VasioClientSnapshot> result;
    result.reserve(clients_.size());
    for (const auto& client : clients_) result.push_back({client.info, client.mapping});
    return result;
}

bool VasioClientManager::TryReserveShutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdownReserved_ || !clients_.empty()) return false;
    shutdownReserved_ = true;
    return true;
}

void VasioClientManager::SetDriverConfiguration(
    std::uint32_t preferredBufferFrames, std::uint32_t sampleRate) {
    std::lock_guard<std::mutex> lock(mutex_);
    preferredBufferFrames_ = preferredBufferFrames;
    sampleRate_ = sampleRate;
    for (auto& client : clients_) {
        client.control->SetPreferredBufferFrames(preferredBufferFrames_);
        client.control->SetBlockFrames(preferredBufferFrames_);
        if (sampleRate_) client.control->SetSampleRate(sampleRate_);
    }
}

void VasioClientManager::Run() noexcept {
    while (running_.load()) {
        try { Scan(); } catch (...) { /* Discovery retries on the next interval. */ }
        for (DWORD elapsed = 0; elapsed < kScanPeriodMs && running_.load(); elapsed += 10) Sleep(10);
    }
}

void VasioClientManager::Scan() {
    std::set<std::pair<std::uint32_t, std::uint32_t>> found;
    std::set<std::pair<std::uint32_t, std::uint32_t>> existingClients;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& client : clients_)
            existingClients.emplace(client.info.driverId, client.info.processId);
    }
    std::vector<PROCESSENTRY32W> processes;
    HANDLE processSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    if (Process32FirstW(processSnapshot, &process)) {
        do { processes.push_back(process); }
        while (Process32NextW(processSnapshot, &process));
    }
    CloseHandle(processSnapshot);

    // A recycled PID must not inherit the previous process's control attachment.
    // Query only known clients; discovery runs off the audio callback thread.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdownReserved_) return;
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [&](Client& client) {
            const auto present = std::find_if(processes.begin(), processes.end(), [&](const auto& entry) {
                return entry.th32ProcessID == client.info.processId;
            });
            if (present == processes.end()) return false;
            const auto creation = processCreationTime(client.info.processId);
            if (!creation || creation == client.control->ProcessCreationTime()) return false;
            const auto key = std::make_pair(client.info.driverId, client.info.processId);
            existingClients.erase(key);
            client.control->SetEngineAttached(false);
            client.control->SignalClient();
            return true;
        }), clients_.end());
    }

    // Client-created mappings are the fastest authoritative signal of an initialized
    // TimoxVasio instance. Discover and attach it before the slower per-process module scan,
    // so a host's init timeout does not expire while unrelated processes are inspected.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::unique_ptr<AudioClientMapping>>
        fastMappings;
    for (const auto& entry : processes) {
        if (entry.th32ProcessID == GetCurrentProcessId()) continue;
        constexpr std::uint32_t driver = 1;
        const auto key = std::make_pair(driver, entry.th32ProcessID);
        if (existingClients.find(key) != existingClients.end()) continue;
        auto mapping = AudioClientMapping::OpenEngine(driver, entry.th32ProcessID, 0);
        if (!mapping) continue;
        found.insert(key);
        fastMappings.emplace(key, std::move(mapping));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdownReserved_) return;
        for (auto& entry : fastMappings) {
            const auto& key = entry.first;
            const auto existing = std::find_if(clients_.begin(), clients_.end(), [&](const Client& client) {
                return client.info.driverId == key.first && client.info.processId == key.second;
            });
            if (existing != clients_.end()) continue;
            auto& mapping = entry.second;
            mapping->SetPreferredBufferFrames(preferredBufferFrames_);
            mapping->SetBlockFrames(preferredBufferFrames_);
            if (sampleRate_) mapping->SetSampleRate(sampleRate_);
            mapping->SetEngineAttached(true);
            std::wstring processName;
            const auto processEntry = std::find_if(processes.begin(), processes.end(), [&](const auto& item) {
                return item.th32ProcessID == key.second;
            });
            if (processEntry != processes.end()) processName = processEntry->szExeFile;
            auto control = std::shared_ptr<AudioClientMapping>(std::move(mapping));
            clients_.push_back({{key.first, key.second, std::move(processName)},
                control, control});
        }
        // Refresh known clients before the slower module inventory. OpenEngine
        // returns an independent view of the newly published immutable epoch.
        for (auto& client : clients_) {
            const auto present = std::find_if(processes.begin(), processes.end(), [&](const auto& entry) {
                return entry.th32ProcessID == client.info.processId;
            });
            if (present == processes.end()) continue;
            const auto published = client.control->PublishedAudioEpoch();
            if (published == client.mapping->AudioEpoch()) continue;
            auto next = AudioClientMapping::OpenEngine(
                client.info.driverId, client.info.processId, 0);
            if (!next || next->AudioEpoch() != published) continue;
            client.mapping = std::shared_ptr<AudioClientMapping>(std::move(next));
        }
    }

    // Keep the module scan for timely client removal and hosts whose driver module is
    // visible before their mapping has been created.
    for (const auto& entry : processes) {
        if (entry.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE moduleSnapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, entry.th32ProcessID);
        if (moduleSnapshot == INVALID_HANDLE_VALUE) {
            for (const auto& key : existingClients)
                if (key.second == entry.th32ProcessID) found.insert(key);
            continue;
        }
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (Module32FirstW(moduleSnapshot, &module)) {
            do {
                const int driverId = driverIdFromModule(module.szModule);
                if (driverId) found.emplace(static_cast<std::uint32_t>(driverId), entry.th32ProcessID);
            } while (Module32NextW(moduleSnapshot, &module));
        }
        CloseHandle(moduleSnapshot);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdownReserved_) return;
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [&](Client& client) {
        const auto key = std::make_pair(client.info.driverId, client.info.processId);
        if (found.find(key) != found.end()) return false;
        client.control->SetEngineAttached(false);
        client.control->SignalClient();
        return true;
    }), clients_.end());

    for (const auto& key : found) {
        const auto existing = std::find_if(clients_.begin(), clients_.end(), [&](const Client& client) {
            return client.info.driverId == key.first && client.info.processId == key.second;
        });
        if (existing != clients_.end()) continue;
        auto mapping = AudioClientMapping::OpenEngine(key.first, key.second, 0);
        if (!mapping) continue;
        mapping->SetPreferredBufferFrames(preferredBufferFrames_);
        mapping->SetBlockFrames(preferredBufferFrames_);
        if (sampleRate_) mapping->SetSampleRate(sampleRate_);
        mapping->SetEngineAttached(true);
        auto control = std::shared_ptr<AudioClientMapping>(std::move(mapping));
        clients_.push_back({{key.first, key.second, L""}, control, control});
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, key.second);
        if (process) {
            wchar_t path[MAX_PATH]{};
            DWORD length = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &length)) {
                const wchar_t* base = wcsrchr(path, L'\\');
                clients_.back().info.processName = base ? base + 1 : path;
            }
            CloseHandle(process);
        }
    }
}
