#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <cstdint>
#include <thread>
#include <vector>

struct AudioClockDeviceInfo {
    std::wstring id; // Opaque backend device identifier.
    std::string name;
};

struct AudioClockCapabilities {
    long inputChannels = 0;
    long outputChannels = 0;
    std::vector<std::wstring> inputChannelNames;
    std::vector<std::wstring> outputChannelNames;
    long minBufferFrames = 0;
    long maxBufferFrames = 0;
    long preferredBufferFrames = 0;
    long bufferGranularity = 0;
    double currentSampleRate = 0.0;
    std::vector<double> supportedSampleRates;
};

struct AudioClockTimingCounters {
    std::uint64_t callbacks = 0;
    std::uint64_t delayedStarts = 0;
    std::uint64_t processingOverruns = 0;
    std::uint64_t worstProcessingNs = 0;
};

struct StageTimingCounters {
    std::uint64_t samples = 0;
    std::uint64_t sumTicks = 0;
    std::uint64_t worstTicks = 0;
};

class StageTimingAccumulator final {
public:
    void Add(std::uint64_t ticks) noexcept {
        const auto generation = activeGeneration_.load(std::memory_order_seq_cst);
        const auto index = generation & 1u;
        auto& bank = banks_[index];
        bank.inFlight.fetch_add(1, std::memory_order_seq_cst);
        if (activeGeneration_.load(std::memory_order_seq_cst) != generation) {
            bank.inFlight.fetch_sub(1, std::memory_order_seq_cst);
            return;
        }
        bank.sumTicks.fetch_add(ticks, std::memory_order_relaxed);
        auto worst = bank.worstTicks.load(std::memory_order_relaxed);
        while (ticks > worst && !bank.worstTicks.compare_exchange_weak(worst, ticks,
            std::memory_order_relaxed, std::memory_order_relaxed)) {}
        bank.samples.fetch_add(1, std::memory_order_relaxed);
        bank.inFlight.fetch_sub(1, std::memory_order_seq_cst);
    }
    StageTimingCounters Consume() noexcept {
        const auto index = activeGeneration_.fetch_add(1, std::memory_order_seq_cst) & 1u;
        auto& bank = banks_[index];
        while (bank.inFlight.load(std::memory_order_seq_cst) != 0)
            std::this_thread::yield();
        return {bank.samples.exchange(0, std::memory_order_relaxed),
            bank.sumTicks.exchange(0, std::memory_order_relaxed),
            bank.worstTicks.exchange(0, std::memory_order_relaxed)};
    }
private:
    struct Bank {
        std::atomic<std::uint32_t> inFlight{0};
        std::atomic<std::uint64_t> samples{0};
        std::atomic<std::uint64_t> sumTicks{0};
        std::atomic<std::uint64_t> worstTicks{0};
    };
    std::atomic<std::uint64_t> activeGeneration_{0};
    Bank banks_[2];
};

using AudioClockProcessCallback = void (*)(const float* const* inputs,
    float* const* outputs, std::uint32_t frames, void* context) noexcept;

// Control methods run on the owning controller worker. The process callback runs
// on the backend audio thread; buffers and context remain valid for that call.
// stop() must quiesce callbacks before close() releases backend resources.
class AudioClockHost {
public:
    virtual ~AudioClockHost() = default;
    virtual std::vector<AudioClockDeviceInfo> enumerate() const = 0;
    virtual bool open(const std::wstring& driverId, void* systemHandle,
              AudioClockCapabilities& capabilities, std::string& error) = 0;
    virtual bool setSampleRate(double sampleRate, std::string& error) = 0;
    virtual bool refreshCapabilities(AudioClockCapabilities& capabilities, std::string& error) = 0;
    virtual bool consumeSampleRateChange() noexcept = 0;
    virtual std::uint32_t consumeErrorCode() noexcept { return 0; }
    virtual AudioClockTimingCounters consumeTimingCounters() noexcept = 0;
    virtual void setStageMeasurementsEnabled(bool enabled) noexcept = 0;
    virtual StageTimingCounters consumeOutputCopyStageCounters() noexcept = 0;
    virtual bool start(std::uint32_t bufferFrames, const std::vector<std::uint32_t>& inputChannels,
               const std::vector<std::uint32_t>& outputChannels,
               AudioClockProcessCallback callback, void* context, std::string& error) = 0;
    virtual void stop() noexcept = 0;
    virtual void close() noexcept = 0;

};

// Compatibility names for existing ASIO-facing API and diagnostics consumers.
using PhysicalAsioDriverInfo = AudioClockDeviceInfo;
using PhysicalAsioCapabilities = AudioClockCapabilities;
using PhysicalAsioTimingCounters = AudioClockTimingCounters;
using PhysicalAsioProcessCallback = AudioClockProcessCallback;
