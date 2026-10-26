#pragma once

#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Private data plane shared by a VASIO client and its routing engine.
class AudioClientMapping final {
public:
    static constexpr std::uint32_t kChannelCount = 256;
    static constexpr std::uint32_t kRingCapacityFrames = 16384;
    static constexpr std::size_t kChannelMaskWords = kChannelCount / 64;
    using ChannelMask = std::array<std::uint64_t, kChannelMaskWords>;
    struct StageDurationCounters {
        std::uint64_t samples = 0, sumTicks = 0, worstTicks = 0;
    };
    struct ClientStageCounters {
        std::uint64_t epoch = 0;
        StageDurationCounters hostCallback, ringWrite, ringWait;
        std::uint64_t ringWaitDiscardedBlocks = 0;
    };

    enum class ClientState : std::uint32_t {
        Initialized = 1, BuffersReady = 2, Running = 3,
        Stopped = 4, Detached = 5
    };
    struct StatusSnapshot {
        bool available = false;
        std::uint32_t layoutVersion = 0;
        std::uint32_t processId = 0;
        std::uint32_t driverId = 0;
        std::array<std::uint32_t, 4> dllVersion{};
        std::wstring dllPath;
        ClientState state = ClientState::Detached;
        std::uint32_t requestedSampleRate = 0;
        std::uint32_t requestedBufferFrames = 0;
        std::uint32_t confirmedSampleRate = 0;
        std::uint32_t confirmedBufferFrames = 0;
        std::uint64_t qpcFrequency = 0;
        std::uint64_t firstCallbackQpc = 0;
        std::uint64_t lastCallbackQpc = 0;
        std::uint64_t lastSuccessfulExchangeQpc = 0;
        std::uint64_t lastIntervalQpc = 0;
        std::uint64_t callbackCount = 0;
        std::uint64_t successfulExchangeCount = 0;
        std::uint64_t exchangedFrames = 0;
        std::uint64_t resetRequests = 0;
        bool stableExchange = true;
        bool stableChannels = true;
        std::uint64_t inputUnderruns = 0;
        std::uint64_t outputOverruns = 0;
        std::uint64_t inputOverruns = 0;
        std::uint64_t outputUnderruns = 0;
        ChannelMask activeInputs{};
        ChannelMask activeOutputs{};
    };

    static bool ReadStatusReadOnly(std::uint32_t driverId, std::uint32_t processId,
        StatusSnapshot& snapshot);

    static std::unique_ptr<AudioClientMapping> CreateClient(std::uint32_t driverId,
        std::uint32_t processId, std::uint32_t sampleRate, std::uint32_t blockFrames);
    static std::unique_ptr<AudioClientMapping> OpenEngine(std::uint32_t driverId,
        std::uint32_t processId, std::uint32_t sampleRate);
    ~AudioClientMapping();

    AudioClientMapping(const AudioClientMapping&) = delete;
    AudioClientMapping& operator=(const AudioClientMapping&) = delete;

    bool SetEngineAttached(bool attached) noexcept;
    bool EngineAttached() const noexcept;
    void SetStageMeasurementsEnabled(bool enabled) noexcept;
    bool StageMeasurementsEnabled() const noexcept;
    ClientStageCounters ConsumeClientStageCounters() noexcept;
    bool EngineProcessAlive() const noexcept;
    std::uint32_t SampleRate() const noexcept;
    std::uint32_t BlockFrames() const noexcept;
    std::uint32_t PreferredBufferFrames() const noexcept;
    bool SetSampleRate(std::uint32_t sampleRate) noexcept;
    bool SetBlockFrames(std::uint32_t blockFrames) noexcept;
    bool SetPreferredBufferFrames(std::uint32_t frames) noexcept;
    bool SetActiveChannels(const ChannelMask& inputs, const ChannelMask& outputs) noexcept;
    bool RetireAudioGeneration() noexcept;
    bool GetActiveChannels(ChannelMask& inputs, ChannelMask& outputs) const noexcept;
    bool RefreshAudioGeneration() noexcept;
    std::uint64_t PublishedAudioEpoch() const noexcept;
    std::uint64_t AudioEpoch() const noexcept;
    std::uint64_t ProcessCreationTime() const noexcept;
    bool AckAudioGeneration(std::uint32_t expectedRate, std::uint32_t expectedFrames) noexcept;
    bool AudioGenerationAcknowledged() const noexcept;
    std::uint32_t InputStride() const noexcept;
    std::uint32_t OutputStride() const noexcept;
    int InputChannelAt(std::uint32_t slot) const noexcept;
    int OutputChannelAt(std::uint32_t slot) const noexcept;
    int InputSlot(std::uint32_t channel) const noexcept;
    int OutputSlot(std::uint32_t channel) const noexcept;
    bool WaitForClientSignal(DWORD timeoutMs) noexcept;
    std::uint32_t ClientOutputAvailable() const noexcept;
    std::uint32_t ClientInputAvailable() const noexcept;
    bool WriteClientOutput(const float* interleaved, std::uint32_t frames) noexcept;
    bool WriteClientOutputMeasured(const float* interleaved, std::uint32_t frames,
        std::uint64_t callbackTicks, std::uint64_t copyTicks, std::uint64_t publishQpc) noexcept;
    std::uint32_t ReadClientOutput(float* interleaved, std::uint32_t frames) noexcept;
    bool WriteEngineInput(const float* interleaved, std::uint32_t frames) noexcept;
    std::uint32_t ReadClientInput(float* interleaved, std::uint32_t frames) noexcept;
    bool SignalClient() noexcept;
    std::uint64_t ClientOutputOverruns() const noexcept;
    std::uint64_t ClientInputUnderruns() const noexcept;
    std::uint64_t ClientInputOverruns() const noexcept;
    std::uint64_t ClientOutputUnderruns() const noexcept;
    void PublishClientState(ClientState state) noexcept;
    void PublishClientRequestedFormat(std::uint32_t sampleRate,
        std::uint32_t bufferFrames) noexcept;
    void PublishClientExchange(std::uint32_t frames) noexcept;
    void PublishResetRequest() noexcept;

private:
    struct Shared;
    struct Audio;
    AudioClientMapping(HANDLE mapping, HANDLE eventHandle, Shared* shared, bool engineOwner) noexcept;
    static std::unique_ptr<AudioClientMapping> Open(bool create, std::uint32_t driverId,
        std::uint32_t processId, std::uint32_t sampleRate, std::uint32_t blockFrames);
    std::uint32_t Available(volatile LONG64* write, volatile LONG64* read) const noexcept;
    bool Write(const float* src, std::uint32_t frames, volatile LONG64* write,
        volatile LONG64* read, float* ring, volatile LONG64* overruns, std::uint32_t stride) noexcept;
    std::uint32_t Read(float* dst, std::uint32_t frames, volatile LONG64* read,
        volatile LONG64* write, const float* ring, volatile LONG64* underruns,
        std::uint32_t stride) noexcept;

    HANDLE mapping_ = nullptr;
    HANDLE event_ = nullptr;
    Shared* shared_ = nullptr;
    HANDLE audioMapping_ = nullptr;
    Audio* audio_ = nullptr;
    std::array<std::int16_t, kChannelCount> inputSlots_{};
    std::array<std::int16_t, kChannelCount> outputSlots_{};
    bool engineOwner_ = false;
    bool ownsEngineAttachment_ = false;
};
