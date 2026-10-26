#include "audio_transport.h"
#include "audio_transport_layout.h"
#include "driver_version.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace {
constexpr std::uint32_t kMagic = 0x56534154; // VSAT
constexpr std::uint32_t kVersion = 9;
constexpr std::uint32_t kStatusMagic = 0x54535356; // VSST
constexpr std::uint64_t kCapacity = AudioClientMapping::kRingCapacityFrames;

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

std::wstring objectName(const wchar_t* kind, std::uint32_t pid, std::uint32_t driver,
    std::uint64_t creation) {
    return L"Local\\VASIO-" + std::wstring(kind) + L"-v9-" + std::to_wstring(pid) + L"-" +
        std::to_wstring(driver) + L"-" + std::to_wstring(creation);
}

std::wstring audioName(std::uint32_t pid, std::uint32_t driver,
    std::uint64_t creation, std::uint64_t epoch) {
    return objectName(L"Audio", pid, driver, creation) + L"-" + std::to_wstring(epoch);
}

void moduleMarker() noexcept {}

void recordTicks(volatile LONG64* samples, volatile LONG64* sum,
    volatile LONG64* worst, std::uint64_t ticks) noexcept {
    InterlockedIncrement64(samples);
    InterlockedExchangeAdd64(sum, static_cast<LONG64>(ticks));
    auto previous = InterlockedCompareExchange64(worst, 0, 0);
    while (ticks > static_cast<std::uint64_t>(previous)) {
        const auto observed = InterlockedCompareExchange64(worst, static_cast<LONG64>(ticks), previous);
        if (observed == previous) break;
        previous = observed;
    }
}

StageCounterBank* enterStageBank(volatile LONG64* generation, StageCounterBank (&banks)[2]) noexcept {
    const auto observed = InterlockedCompareExchange64(generation, 0, 0);
    auto* bank = &banks[observed & 1];
    InterlockedIncrement(&bank->inFlight);
    if (InterlockedCompareExchange64(generation, 0, 0) == observed) return bank;
    InterlockedDecrement(&bank->inFlight);
    return nullptr;
}

void discardStageBlock(volatile LONG64* generation, StageCounterBank (&banks)[2]) noexcept {
    if (auto* bank = enterStageBank(generation, banks)) {
        InterlockedIncrement64(&bank->ringWaitDiscardedBlocks);
        InterlockedDecrement(&bank->inFlight);
    }
}

AudioClientMapping::StageDurationCounters takeTicks(volatile LONG64* samples,
    volatile LONG64* sum, volatile LONG64* worst) noexcept {
    return {static_cast<std::uint64_t>(InterlockedExchange64(samples, 0)),
        static_cast<std::uint64_t>(InterlockedExchange64(sum, 0)),
        static_cast<std::uint64_t>(InterlockedExchange64(worst, 0))};
}

std::wstring ownModulePath() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleMarker), &module)) return {};
    wchar_t path[1024]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return {};
    return std::wstring(path, length);
}
}

AudioClientMapping::AudioClientMapping(HANDLE mapping, HANDLE eventHandle, Shared* shared, bool engineOwner) noexcept
    : mapping_(mapping), event_(eventHandle), shared_(shared), engineOwner_(engineOwner) {}

AudioClientMapping::~AudioClientMapping() {
    if (shared_) UnmapViewOfFile(shared_);
    if (audio_) UnmapViewOfFile(audio_);
    if (audioMapping_) CloseHandle(audioMapping_);
    if (event_) CloseHandle(event_);
    if (mapping_) CloseHandle(mapping_);
}

std::unique_ptr<AudioClientMapping> AudioClientMapping::CreateClient(std::uint32_t driver,
    std::uint32_t pid, std::uint32_t rate, std::uint32_t block) {
    return Open(true, driver, pid, rate, block);
}

std::unique_ptr<AudioClientMapping> AudioClientMapping::OpenEngine(std::uint32_t driver,
    std::uint32_t pid, std::uint32_t rate) {
    return Open(false, driver, pid, rate, 0);
}

std::unique_ptr<AudioClientMapping> AudioClientMapping::Open(bool create, std::uint32_t driver,
    std::uint32_t pid, std::uint32_t rate, std::uint32_t block) {
    if (!pid || (create && (!rate || !block || block > kRingCapacityFrames))) return {};
    const auto creation = processCreationTime(pid);
    if (!creation) return {};
    const auto mapName = objectName(L"Map", pid, driver, creation);
    const auto eventName = objectName(L"Event", pid, driver, creation);
    HANDLE map = create
        ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), mapName.c_str())
        : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapName.c_str());
    if (!map) return {};
    const DWORD mapError = GetLastError();
    const bool reusingClientMapping = create && mapError == ERROR_ALREADY_EXISTS;
    HANDLE eventHandle = create ? CreateEventW(nullptr, FALSE, FALSE, eventName.c_str())
                                : OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, eventName.c_str());
    if (!eventHandle) { CloseHandle(map); return {}; }
    void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!view) { CloseHandle(eventHandle); CloseHandle(map); return {}; }
    auto* shared = static_cast<Shared*>(view);
    if (create && !reusingClientMapping) {
        std::memset(shared, 0, sizeof(Shared));
        shared->magic = kMagic;
        shared->version = kVersion;
        shared->size = sizeof(Shared);
        shared->driverId = driver;
        shared->processId = pid;
        shared->processCreationTime = creation;
        InterlockedExchange(&shared->sampleRate, static_cast<LONG>(rate));
        InterlockedExchange(&shared->blockFrames, static_cast<LONG>(block));
        InterlockedExchange(&shared->preferredBufferFrames, static_cast<LONG>(block));
        InterlockedExchange64(&shared->generation, 1);
        auto& status = shared->status;
        status.magic = kStatusMagic;
        status.schemaVersion = 2;
        status.size = sizeof(ClientStatusV2);
        status.dllVersion[0] = TIMOX_VERSION_MAJOR;
        status.dllVersion[1] = TIMOX_VERSION_MINOR;
        status.dllVersion[2] = TIMOX_VERSION_PATCH;
        status.dllVersion[3] = TIMOX_VERSION_BUILD;
        const auto path = ownModulePath();
        const auto count = (std::min)(path.size(), std::size(status.dllPath) - 1);
        std::copy_n(path.data(), count, status.dllPath);
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency))
            InterlockedExchange64(&status.qpcFrequency, frequency.QuadPart);
        InterlockedExchange(&status.state, static_cast<LONG>(ClientState::Initialized));
        InterlockedExchange(&status.requestedSampleRate, static_cast<LONG>(rate));
        InterlockedExchange(&status.requestedBufferFrames, static_cast<LONG>(block));
        InterlockedExchange(&status.ready, 1);
    } else if (shared->magic != kMagic || shared->version !=
        kVersion || shared->size != sizeof(Shared) ||
        shared->driverId != driver || shared->processId != pid ||
        shared->processCreationTime != creation ||
        (rate && !reusingClientMapping &&
            static_cast<std::uint32_t>(InterlockedCompareExchange(&shared->sampleRate, 0, 0)) != rate) ||
        shared->status.magic != kStatusMagic || shared->status.size != sizeof(ClientStatusV2)) {
        UnmapViewOfFile(view); CloseHandle(eventHandle); CloseHandle(map); return {};
    }
    auto result = std::unique_ptr<AudioClientMapping>(new AudioClientMapping(map, eventHandle, shared, !create));
    if (!create && result->PublishedAudioEpoch() && !result->RefreshAudioGeneration()) return {};
    return result;
}

bool AudioClientMapping::SetEngineAttached(bool attached) noexcept {
    ownsEngineAttachment_ = attached;
    const LONG64 state = attached ? 1 : 0;
    InterlockedExchange(&shared_->engineProcessId, attached ? static_cast<LONG>(GetCurrentProcessId()) : 0);
    if (InterlockedExchange64(&shared_->engineAttached, state) != state)
        InterlockedIncrement64(&shared_->generation);
    return true;
}

bool AudioClientMapping::EngineAttached() const noexcept {
    return InterlockedCompareExchange64(const_cast<volatile LONG64*>(&shared_->engineAttached), 0, 0) != 0;
}

void AudioClientMapping::SetStageMeasurementsEnabled(bool enabled) noexcept {
    if (!shared_ || !engineOwner_) return;
    const LONG next = enabled ? 1 : 0;
    if (InterlockedExchange(&shared_->stageMeasurementsEnabled, next) == next) return;
    InterlockedIncrement64(&shared_->stageMeasurementGeneration);
    if (!enabled && audio_) {
        const auto tail = InterlockedCompareExchange64(&audio_->sampledWriteIndex, 0, 0);
        InterlockedExchange64(&audio_->sampledReadIndex, tail);
    }
    (void)ConsumeClientStageCounters();
}

bool AudioClientMapping::StageMeasurementsEnabled() const noexcept {
    return shared_ && InterlockedCompareExchange(
        const_cast<volatile LONG*>(&shared_->stageMeasurementsEnabled), 0, 0) != 0;
}

AudioClientMapping::ClientStageCounters AudioClientMapping::ConsumeClientStageCounters() noexcept {
    ClientStageCounters result{};
    if (!shared_) return result;
    while (InterlockedCompareExchange(&shared_->stageCounterConsumer, 1, 0) != 0) Sleep(0);
    const auto old = InterlockedIncrement64(&shared_->stageCounterGeneration) - 1;
    auto& bank = shared_->stageCounterBanks[old & 1];
    while (InterlockedCompareExchange(&bank.inFlight, 0, 0) != 0) Sleep(0);
    result.epoch = static_cast<std::uint64_t>(InterlockedCompareExchange64(
        &shared_->stageCounterEpoch, 0, 0));
    result.hostCallback = takeTicks(&bank.hostCallbackSamples,
        &bank.hostCallbackSumTicks, &bank.hostCallbackWorstTicks);
    result.ringWrite = takeTicks(&bank.ringWriteSamples,
        &bank.ringWriteSumTicks, &bank.ringWriteWorstTicks);
    result.ringWait = takeTicks(&bank.ringWaitSamples,
        &bank.ringWaitSumTicks, &bank.ringWaitWorstTicks);
    result.ringWaitDiscardedBlocks = static_cast<std::uint64_t>(
        InterlockedExchange64(&bank.ringWaitDiscardedBlocks, 0));
    InterlockedExchange(&shared_->stageCounterConsumer, 0);
    return result;
}

bool AudioClientMapping::EngineProcessAlive() const noexcept {
    if (!EngineAttached()) return false;
    const DWORD pid = static_cast<DWORD>(InterlockedCompareExchange(
        const_cast<volatile LONG*>(&shared_->engineProcessId), 0, 0));
    if (!pid) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

std::uint32_t AudioClientMapping::SampleRate() const noexcept {
    return static_cast<std::uint32_t>(InterlockedCompareExchange(
        const_cast<volatile LONG*>(&shared_->sampleRate), 0, 0));
}

std::uint32_t AudioClientMapping::BlockFrames() const noexcept {
    return static_cast<std::uint32_t>(InterlockedCompareExchange(
        const_cast<volatile LONG*>(&shared_->blockFrames), 0, 0));
}

std::uint32_t AudioClientMapping::PreferredBufferFrames() const noexcept {
    return static_cast<std::uint32_t>(InterlockedCompareExchange(
        const_cast<volatile LONG*>(&shared_->preferredBufferFrames), 0, 0));
}

bool AudioClientMapping::SetSampleRate(std::uint32_t rate) noexcept {
    if (!engineOwner_ || !rate) return false;
    if (InterlockedExchange(&shared_->sampleRate, static_cast<LONG>(rate)) != static_cast<LONG>(rate))
        InterlockedIncrement64(&shared_->generation);
    return true;
}

bool AudioClientMapping::SetBlockFrames(std::uint32_t frames) noexcept {
    if (!engineOwner_ || !frames || frames > kRingCapacityFrames) return false;
    if (InterlockedExchange(&shared_->blockFrames, static_cast<LONG>(frames)) != static_cast<LONG>(frames))
        InterlockedIncrement64(&shared_->generation);
    return true;
}

bool AudioClientMapping::SetPreferredBufferFrames(std::uint32_t frames) noexcept {
    if (!engineOwner_ || frames < 64 || frames > 8192 || (frames & (frames - 1)) != 0)
        return false;
    if (InterlockedExchange(&shared_->preferredBufferFrames, static_cast<LONG>(frames)) != static_cast<LONG>(frames))
        InterlockedIncrement64(&shared_->generation);
    return true;
}

bool AudioClientMapping::SetActiveChannels(const ChannelMask& inputs, const ChannelMask& outputs) noexcept {
    if (engineOwner_ || !shared_ ||
        InterlockedCompareExchange(&shared_->status.state, 0, 0) ==
            static_cast<LONG>(ClientState::Running)) return false;
    const auto epoch = static_cast<std::uint64_t>(InterlockedIncrement64(&shared_->nextAudioEpoch));
    const auto name = audioName(shared_->processId, shared_->driverId,
        shared_->processCreationTime, epoch);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(Audio), name.c_str());
    if (!map) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(map); return false; }
    auto* next = static_cast<Audio*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Audio)));
    if (!next) { CloseHandle(map); return false; }
    std::memset(next, 0, sizeof(Audio));
    next->magic = kMagic;
    next->version = kVersion;
    next->size = sizeof(Audio);
    next->driverId = shared_->driverId;
    next->processId = shared_->processId;
    next->processCreationTime = shared_->processCreationTime;
    next->epoch = epoch;
    next->capacityFrames = kRingCapacityFrames;
    for (std::uint32_t channel = 0; channel < kChannelCount; ++channel) {
        const auto bit = std::uint64_t{1} << (channel % 64);
        if (inputs[channel / 64] & bit) next->inputChannels[next->inputStride++] = static_cast<std::uint16_t>(channel);
        if (outputs[channel / 64] & bit) next->outputChannels[next->outputStride++] = static_cast<std::uint16_t>(channel);
    }
    InterlockedIncrement64(&shared_->activeChannelsGeneration);
    for (std::size_t word = 0; word < kChannelMaskWords; ++word) {
        InterlockedExchange64(&shared_->activeInputs[word], static_cast<LONG64>(inputs[word]));
        InterlockedExchange64(&shared_->activeOutputs[word], static_cast<LONG64>(outputs[word]));
    }
    InterlockedIncrement64(&shared_->activeChannelsGeneration);
    (void)ConsumeClientStageCounters();
    (void)ConsumeClientStageCounters();
    InterlockedExchange64(&shared_->stageCounterEpoch, static_cast<LONG64>(epoch));
    // Release publication: all immutable audio header, lists and zero indices precede this store.
    InterlockedExchange64(&shared_->publishedAudioEpoch, static_cast<LONG64>(epoch));
    if (audio_) UnmapViewOfFile(audio_);
    if (audioMapping_) CloseHandle(audioMapping_);
    audioMapping_ = map;
    audio_ = next;
    inputSlots_.fill(-1);
    outputSlots_.fill(-1);
    for (std::uint32_t i = 0; i < next->inputStride; ++i) inputSlots_[next->inputChannels[i]] = static_cast<std::int16_t>(i);
    for (std::uint32_t i = 0; i < next->outputStride; ++i) outputSlots_[next->outputChannels[i]] = static_cast<std::int16_t>(i);
    InterlockedIncrement64(&shared_->generation);
    return true;
}

bool AudioClientMapping::RetireAudioGeneration() noexcept {
    if (engineOwner_ || !shared_ ||
        InterlockedCompareExchange(&shared_->status.state, 0, 0) ==
            static_cast<LONG>(ClientState::Running)) return false;
    InterlockedIncrement64(&shared_->activeChannelsGeneration);
    for (std::size_t word = 0; word < kChannelMaskWords; ++word) {
        InterlockedExchange64(&shared_->activeInputs[word], 0);
        InterlockedExchange64(&shared_->activeOutputs[word], 0);
    }
    InterlockedIncrement64(&shared_->activeChannelsGeneration);
    // Existing views retain their mapping until their owners retire.
    InterlockedExchange64(&shared_->publishedAudioEpoch, 0);
    InterlockedExchange64(&shared_->acknowledgedAudioEpoch, 0);
    (void)ConsumeClientStageCounters();
    (void)ConsumeClientStageCounters();
    InterlockedExchange64(&shared_->stageCounterEpoch, 0);
    InterlockedIncrement64(&shared_->generation);
    return true;
}

std::uint64_t AudioClientMapping::PublishedAudioEpoch() const noexcept {
    return static_cast<std::uint64_t>(InterlockedCompareExchange64(
        const_cast<volatile LONG64*>(&shared_->publishedAudioEpoch), 0, 0));
}
std::uint64_t AudioClientMapping::AudioEpoch() const noexcept { return audio_ ? audio_->epoch : 0; }
std::uint64_t AudioClientMapping::ProcessCreationTime() const noexcept {
    return shared_ ? shared_->processCreationTime : 0;
}
bool AudioClientMapping::RefreshAudioGeneration() noexcept {
    const auto epoch = PublishedAudioEpoch();
    if (!epoch) return audio_ == nullptr;
    if (audio_) return audio_->epoch == epoch;
    if (!engineOwner_) return false;
    const auto name = audioName(shared_->processId, shared_->driverId,
        shared_->processCreationTime, epoch);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (!map) return false;
    auto* next = static_cast<Audio*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Audio)));
    if (!next) { CloseHandle(map); return false; }
    bool valid = next->magic == kMagic && next->version == kVersion &&
        next->size == sizeof(Audio) && next->driverId == shared_->driverId &&
        next->processId == shared_->processId &&
        next->processCreationTime == shared_->processCreationTime &&
        next->epoch == epoch && next->capacityFrames == kRingCapacityFrames &&
        next->inputStride <= kChannelCount && next->outputStride <= kChannelCount;
    for (std::uint32_t i = 0; valid && i < next->inputStride; ++i)
        valid = next->inputChannels[i] < kChannelCount &&
            (i == 0 || next->inputChannels[i - 1] < next->inputChannels[i]);
    for (std::uint32_t i = 0; valid && i < next->outputStride; ++i)
        valid = next->outputChannels[i] < kChannelCount &&
            (i == 0 || next->outputChannels[i - 1] < next->outputChannels[i]);
    if (!valid || PublishedAudioEpoch() != epoch) {
        UnmapViewOfFile(next); CloseHandle(map); return false;
    }
    audioMapping_ = map;
    audio_ = next;
    inputSlots_.fill(-1);
    outputSlots_.fill(-1);
    for (std::uint32_t i = 0; i < next->inputStride; ++i) inputSlots_[next->inputChannels[i]] = static_cast<std::int16_t>(i);
    for (std::uint32_t i = 0; i < next->outputStride; ++i) outputSlots_[next->outputChannels[i]] = static_cast<std::int16_t>(i);
    return true;
}
bool AudioClientMapping::AckAudioGeneration(std::uint32_t expectedRate,
    std::uint32_t expectedFrames) noexcept {
    if (!engineOwner_ || !audio_ || PublishedAudioEpoch() != audio_->epoch ||
        !expectedRate || !expectedFrames || SampleRate() != expectedRate ||
        BlockFrames() != expectedFrames) return false;
    InterlockedExchange(&shared_->acknowledgedSampleRate, static_cast<LONG>(expectedRate));
    InterlockedExchange(&shared_->acknowledgedBlockFrames, static_cast<LONG>(expectedFrames));
    if (PublishedAudioEpoch() != audio_->epoch || SampleRate() != expectedRate ||
        BlockFrames() != expectedFrames) return false;
    InterlockedExchange64(&shared_->acknowledgedAudioEpoch, static_cast<LONG64>(audio_->epoch));
    return true;
}
bool AudioClientMapping::AudioGenerationAcknowledged() const noexcept {
    return audio_ && PublishedAudioEpoch() == audio_->epoch &&
        static_cast<std::uint64_t>(InterlockedCompareExchange64(
            const_cast<volatile LONG64*>(&shared_->acknowledgedAudioEpoch), 0, 0)) == audio_->epoch &&
        InterlockedCompareExchange(const_cast<volatile LONG*>(&shared_->acknowledgedSampleRate), 0, 0) == static_cast<LONG>(SampleRate()) &&
        InterlockedCompareExchange(const_cast<volatile LONG*>(&shared_->acknowledgedBlockFrames), 0, 0) == static_cast<LONG>(BlockFrames());
}
std::uint32_t AudioClientMapping::InputStride() const noexcept { return audio_ ? audio_->inputStride : 0; }
std::uint32_t AudioClientMapping::OutputStride() const noexcept { return audio_ ? audio_->outputStride : 0; }
int AudioClientMapping::InputChannelAt(std::uint32_t slot) const noexcept { return audio_ && slot < audio_->inputStride ? audio_->inputChannels[slot] : -1; }
int AudioClientMapping::OutputChannelAt(std::uint32_t slot) const noexcept { return audio_ && slot < audio_->outputStride ? audio_->outputChannels[slot] : -1; }
int AudioClientMapping::InputSlot(std::uint32_t channel) const noexcept { return audio_ && channel < kChannelCount ? inputSlots_[channel] : -1; }
int AudioClientMapping::OutputSlot(std::uint32_t channel) const noexcept { return audio_ && channel < kChannelCount ? outputSlots_[channel] : -1; }

bool AudioClientMapping::GetActiveChannels(ChannelMask& inputs, ChannelMask& outputs) const noexcept {
    if (audio_) {
        inputs = {}; outputs = {};
        for (std::uint32_t i = 0; i < audio_->inputStride; ++i) {
            const auto channel = audio_->inputChannels[i];
            inputs[channel / 64] |= std::uint64_t{1} << (channel % 64);
        }
        for (std::uint32_t i = 0; i < audio_->outputStride; ++i) {
            const auto channel = audio_->outputChannels[i];
            outputs[channel / 64] |= std::uint64_t{1} << (channel % 64);
        }
        return true;
    }
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto before = InterlockedCompareExchange64(
            const_cast<volatile LONG64*>(&shared_->activeChannelsGeneration), 0, 0);
        if (before & 1) continue;
        ChannelMask inputSnapshot{};
        ChannelMask outputSnapshot{};
        for (std::size_t word = 0; word < kChannelMaskWords; ++word) {
            inputSnapshot[word] = static_cast<std::uint64_t>(InterlockedCompareExchange64(
                const_cast<volatile LONG64*>(&shared_->activeInputs[word]), 0, 0));
            outputSnapshot[word] = static_cast<std::uint64_t>(InterlockedCompareExchange64(
                const_cast<volatile LONG64*>(&shared_->activeOutputs[word]), 0, 0));
        }
        const auto after = InterlockedCompareExchange64(
            const_cast<volatile LONG64*>(&shared_->activeChannelsGeneration), 0, 0);
        if (before == after && !(after & 1)) {
            inputs = inputSnapshot;
            outputs = outputSnapshot;
            return true;
        }
    }
    return false;
}

bool AudioClientMapping::WaitForClientSignal(DWORD timeoutMs) noexcept {
    return WaitForSingleObject(event_, timeoutMs) == WAIT_OBJECT_0;
}

std::uint32_t AudioClientMapping::Available(volatile LONG64* write, volatile LONG64* read) const noexcept {
    const auto w = static_cast<std::uint64_t>(InterlockedCompareExchange64(write, 0, 0));
    const auto r = static_cast<std::uint64_t>(InterlockedCompareExchange64(read, 0, 0));
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(w - r, kCapacity));
}

std::uint32_t AudioClientMapping::ClientOutputAvailable() const noexcept {
    return audio_ ? Available(&audio_->clientWriteFrame, &audio_->engineReadFrame) : 0;
}
std::uint32_t AudioClientMapping::ClientInputAvailable() const noexcept {
    return audio_ ? Available(&audio_->engineWriteFrame, &audio_->clientReadFrame) : 0;
}

bool AudioClientMapping::Write(const float* src, std::uint32_t frames, volatile LONG64* write,
    volatile LONG64* read, float* ring, volatile LONG64* overruns, std::uint32_t stride) noexcept {
    if ((!src && stride) || frames > kCapacity) return false;
    const auto w = static_cast<std::uint64_t>(InterlockedCompareExchange64(write, 0, 0));
    const auto r = static_cast<std::uint64_t>(InterlockedCompareExchange64(read, 0, 0));
    if (w - r + frames > kCapacity) { InterlockedIncrement64(overruns); return false; }
    const auto firstFrames = (std::min)(static_cast<std::uint64_t>(frames),
        kCapacity - w % kCapacity);
    if (firstFrames && stride) {
        std::memcpy(ring + (w % kCapacity) * stride, src,
            static_cast<std::size_t>(firstFrames) * stride * sizeof(float));
    }
    if (firstFrames < frames && stride) {
        std::memcpy(ring, src + static_cast<std::size_t>(firstFrames) * stride,
            static_cast<std::size_t>(frames - firstFrames) * stride * sizeof(float));
    }
    InterlockedExchange64(write, static_cast<LONG64>(w + frames));
    return true;
}

std::uint32_t AudioClientMapping::Read(float* dst, std::uint32_t frames, volatile LONG64* read,
    volatile LONG64* write, const float* ring, volatile LONG64* underruns,
    std::uint32_t stride) noexcept {
    if (!dst && stride) return 0;
    const auto r = static_cast<std::uint64_t>(InterlockedCompareExchange64(read, 0, 0));
    const auto w = static_cast<std::uint64_t>(InterlockedCompareExchange64(write, 0, 0));
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, w - r));
    const auto firstFrames = (std::min)(static_cast<std::uint64_t>(count),
        kCapacity - r % kCapacity);
    if (firstFrames && stride) {
        std::memcpy(dst, ring + (r % kCapacity) * stride,
            static_cast<std::size_t>(firstFrames) * stride * sizeof(float));
    }
    if (firstFrames < count && stride) {
        std::memcpy(dst + static_cast<std::size_t>(firstFrames) * stride, ring,
            static_cast<std::size_t>(count - firstFrames) * stride * sizeof(float));
    }
    if (count < frames) {
        if (stride) std::fill(dst + static_cast<std::size_t>(count) * stride,
                  dst + static_cast<std::size_t>(frames) * stride, 0.0f);
        if (underruns) InterlockedIncrement64(underruns);
    }
    InterlockedExchange64(read, static_cast<LONG64>(r + count));
    return count;
}

bool AudioClientMapping::WriteClientOutput(const float* data, std::uint32_t frames) noexcept {
    return audio_ && Write(data, frames, &audio_->clientWriteFrame, &audio_->engineReadFrame,
        audio_->clientToEngine, &shared_->outputOverruns, audio_->outputStride);
}
bool AudioClientMapping::WriteClientOutputMeasured(const float* data, std::uint32_t frames,
    std::uint64_t callbackTicks, std::uint64_t copyTicks, std::uint64_t publishQpc) noexcept {
    if (!audio_ || !StageMeasurementsEnabled()) return WriteClientOutput(data, frames);
    if ((!data && audio_->outputStride) || frames > kCapacity) return false;
    const auto w = static_cast<std::uint64_t>(InterlockedCompareExchange64(&audio_->clientWriteFrame, 0, 0));
    const auto r = static_cast<std::uint64_t>(InterlockedCompareExchange64(&audio_->engineReadFrame, 0, 0));
    if (w - r + frames > kCapacity) {
        InterlockedIncrement64(&shared_->outputOverruns);
        discardStageBlock(&shared_->stageCounterGeneration, shared_->stageCounterBanks);
        return false;
    }
    LARGE_INTEGER copyStart{}, published{};
    QueryPerformanceCounter(&copyStart);
    const auto first = (std::min)(static_cast<std::uint64_t>(frames), kCapacity - w % kCapacity);
    if (first && audio_->outputStride)
        std::memcpy(audio_->clientToEngine + (w % kCapacity) * audio_->outputStride, data,
            static_cast<std::size_t>(first) * audio_->outputStride * sizeof(float));
    if (first < frames && audio_->outputStride)
        std::memcpy(audio_->clientToEngine, data + static_cast<std::size_t>(first) * audio_->outputStride,
            static_cast<std::size_t>(frames - first) * audio_->outputStride * sizeof(float));
    QueryPerformanceCounter(&published);
    const auto generation = InterlockedCompareExchange64(&shared_->stageMeasurementGeneration, 0, 0);
    const auto sampleIndex = static_cast<std::uint64_t>(InterlockedCompareExchange64(
        &audio_->sampledWriteIndex, 0, 0));
    auto& sample = audio_->sampledOutput[sampleIndex % kCapacity];
    InterlockedExchange64(&sample.firstFrame, static_cast<LONG64>(w));
    InterlockedExchange64(&sample.epoch, static_cast<LONG64>(audio_->epoch));
    InterlockedExchange64(&sample.generation, generation);
    InterlockedExchange(&sample.frames, static_cast<LONG>(frames));
    InterlockedExchange64(&sample.publishQpc,
        static_cast<LONG64>(publishQpc ? publishQpc : published.QuadPart));
    InterlockedExchange64(&audio_->sampledWriteIndex, static_cast<LONG64>(sampleIndex + 1));
    InterlockedExchange64(&audio_->clientWriteFrame, static_cast<LONG64>(w + frames));
    if (StageMeasurementsEnabled() && generation ==
        InterlockedCompareExchange64(&shared_->stageMeasurementGeneration, 0, 0)) {
        if (auto* bank = enterStageBank(&shared_->stageCounterGeneration, shared_->stageCounterBanks)) {
            recordTicks(&bank->hostCallbackSamples, &bank->hostCallbackSumTicks,
                &bank->hostCallbackWorstTicks, callbackTicks);
            const auto ringTicks = copyTicks + static_cast<std::uint64_t>(published.QuadPart - copyStart.QuadPart);
            recordTicks(&bank->ringWriteSamples, &bank->ringWriteSumTicks,
                &bank->ringWriteWorstTicks, ringTicks);
            InterlockedDecrement(&bank->inFlight);
        }
    }
    return true;
}
std::uint32_t AudioClientMapping::ReadClientOutput(float* data, std::uint32_t frames) noexcept {
    if (!audio_) return 0;
    bool matched = false;
    std::uint64_t published = 0;
    std::uint64_t sampledGeneration = 0;
    if (StageMeasurementsEnabled() && (data || !audio_->outputStride)) {
        const auto r = static_cast<std::uint64_t>(InterlockedCompareExchange64(&audio_->engineReadFrame, 0, 0));
        const auto w = static_cast<std::uint64_t>(InterlockedCompareExchange64(&audio_->clientWriteFrame, 0, 0));
        const auto count = (std::min)(static_cast<std::uint64_t>(frames), w - r);
        const auto generation = InterlockedCompareExchange64(&shared_->stageMeasurementGeneration, 0, 0);
        sampledGeneration = static_cast<std::uint64_t>(generation);
        auto head = static_cast<std::uint64_t>(InterlockedCompareExchange64(
            &audio_->sampledReadIndex, 0, 0));
        const auto tail = static_cast<std::uint64_t>(InterlockedCompareExchange64(
            &audio_->sampledWriteIndex, 0, 0));
        while (count && head < tail) {
            auto& sample = audio_->sampledOutput[head % kCapacity];
            const auto firstFrame = static_cast<std::uint64_t>(
                InterlockedCompareExchange64(&sample.firstFrame, 0, 0));
            if (firstFrame >= r + count) break;
            const auto stamp = InterlockedExchange64(&sample.publishQpc, 0);
            const bool whole = firstFrame == r && count == frames &&
                InterlockedCompareExchange(&sample.frames, 0, 0) == static_cast<LONG>(frames) &&
                static_cast<std::uint64_t>(InterlockedCompareExchange64(&sample.epoch, 0, 0)) == audio_->epoch &&
                InterlockedCompareExchange64(&sample.generation, 0, 0) == generation &&
                PublishedAudioEpoch() == audio_->epoch && StageMeasurementsEnabled();
            if (stamp && whole) { matched = true; published = static_cast<std::uint64_t>(stamp); }
            else if (stamp && PublishedAudioEpoch() == audio_->epoch && StageMeasurementsEnabled() &&
                InterlockedCompareExchange64(&shared_->stageMeasurementGeneration, 0, 0) == generation)
                discardStageBlock(&shared_->stageCounterGeneration, shared_->stageCounterBanks);
            ++head;
        }
        auto observed = InterlockedCompareExchange64(&audio_->sampledReadIndex, 0, 0);
        while (head > static_cast<std::uint64_t>(observed)) {
            const auto prior = InterlockedCompareExchange64(&audio_->sampledReadIndex,
                static_cast<LONG64>(head), observed);
            if (prior == observed) break;
            observed = prior;
        }
    }
    LARGE_INTEGER readQpc{};
    if (matched) QueryPerformanceCounter(&readQpc);
    const auto count = Read(data, frames, &audio_->engineReadFrame, &audio_->clientWriteFrame,
        audio_->clientToEngine, &shared_->outputUnderruns, audio_->outputStride);
    if (matched) {
        if (count == frames && readQpc.QuadPart >= static_cast<LONG64>(published) &&
            StageMeasurementsEnabled() && PublishedAudioEpoch() == audio_->epoch &&
            static_cast<std::uint64_t>(InterlockedCompareExchange64(
                &shared_->stageMeasurementGeneration, 0, 0)) == sampledGeneration) {
            if (auto* bank = enterStageBank(&shared_->stageCounterGeneration, shared_->stageCounterBanks)) {
                recordTicks(&bank->ringWaitSamples, &bank->ringWaitSumTicks,
                    &bank->ringWaitWorstTicks, readQpc.QuadPart - published);
                InterlockedDecrement(&bank->inFlight);
            }
        } else if (PublishedAudioEpoch() == audio_->epoch && StageMeasurementsEnabled() &&
            static_cast<std::uint64_t>(InterlockedCompareExchange64(
                &shared_->stageMeasurementGeneration, 0, 0)) == sampledGeneration) {
            discardStageBlock(&shared_->stageCounterGeneration, shared_->stageCounterBanks);
        }
    }
    return count;
}
bool AudioClientMapping::WriteEngineInput(const float* data, std::uint32_t frames) noexcept {
    return audio_ && Write(data, frames, &audio_->engineWriteFrame, &audio_->clientReadFrame,
        audio_->engineToClient, &shared_->inputOverruns, audio_->inputStride);
}
std::uint32_t AudioClientMapping::ReadClientInput(float* data, std::uint32_t frames) noexcept {
    return audio_ ? Read(data, frames, &audio_->clientReadFrame, &audio_->engineWriteFrame,
        audio_->engineToClient, &shared_->inputUnderruns, audio_->inputStride) : 0;
}
bool AudioClientMapping::SignalClient() noexcept { return SetEvent(event_) != FALSE; }
std::uint64_t AudioClientMapping::ClientOutputOverruns() const noexcept {
    return static_cast<std::uint64_t>(InterlockedCompareExchange64(&shared_->outputOverruns, 0, 0));
}
std::uint64_t AudioClientMapping::ClientInputUnderruns() const noexcept {
    return static_cast<std::uint64_t>(InterlockedCompareExchange64(&shared_->inputUnderruns, 0, 0));
}
std::uint64_t AudioClientMapping::ClientInputOverruns() const noexcept {
    return static_cast<std::uint64_t>(InterlockedCompareExchange64(&shared_->inputOverruns, 0, 0));
}
std::uint64_t AudioClientMapping::ClientOutputUnderruns() const noexcept {
    return static_cast<std::uint64_t>(InterlockedCompareExchange64(&shared_->outputUnderruns, 0, 0));
}

void AudioClientMapping::PublishClientState(ClientState state) noexcept {
    if (!shared_ || engineOwner_ || shared_->version != kVersion) return;
    if (state == ClientState::Running) {
        InterlockedIncrement64(&shared_->status.exchangeGeneration);
        InterlockedExchange64(&shared_->status.firstCallbackQpc, 0);
        InterlockedExchange64(&shared_->status.lastCallbackQpc, 0);
        InterlockedExchange64(&shared_->status.lastSuccessfulExchangeQpc, 0);
        InterlockedExchange64(&shared_->status.lastIntervalQpc, 0);
        InterlockedExchange64(&shared_->status.callbackCount, 0);
        InterlockedExchange64(&shared_->status.successfulExchangeCount, 0);
        InterlockedExchange64(&shared_->status.exchangedFrames, 0);
        InterlockedIncrement64(&shared_->status.exchangeGeneration);
    }
    InterlockedExchange(&shared_->status.state, static_cast<LONG>(state));
}

void AudioClientMapping::PublishClientRequestedFormat(std::uint32_t sampleRate,
    std::uint32_t bufferFrames) noexcept {
    if (!shared_ || engineOwner_ || shared_->version != kVersion) return;
    if (sampleRate)
        InterlockedExchange(&shared_->status.requestedSampleRate, static_cast<LONG>(sampleRate));
    if (bufferFrames)
        InterlockedExchange(&shared_->status.requestedBufferFrames, static_cast<LONG>(bufferFrames));
}

void AudioClientMapping::PublishResetRequest() noexcept {
    if (!shared_ || engineOwner_ || shared_->version != kVersion) return;
    InterlockedIncrement64(&shared_->status.resetRequests);
}

void AudioClientMapping::PublishClientExchange(std::uint32_t frames) noexcept {
    if (!shared_ || engineOwner_ || shared_->version != kVersion) return;
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) return;
    auto& status = shared_->status;
    InterlockedIncrement64(&status.exchangeGeneration);
    InterlockedCompareExchange64(&status.firstCallbackQpc, now.QuadPart, 0);
    const auto previous = InterlockedExchange64(&status.lastCallbackQpc, now.QuadPart);
    if (previous > 0 && now.QuadPart >= previous)
        InterlockedExchange64(&status.lastIntervalQpc, now.QuadPart - previous);
    InterlockedIncrement64(&status.callbackCount);
    if (frames) {
        InterlockedExchange64(&status.lastSuccessfulExchangeQpc, now.QuadPart);
        InterlockedIncrement64(&status.successfulExchangeCount);
    }
    InterlockedExchangeAdd64(&status.exchangedFrames, static_cast<LONG64>(frames));
    InterlockedIncrement64(&status.exchangeGeneration);
}
