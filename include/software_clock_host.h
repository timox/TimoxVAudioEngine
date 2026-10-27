#pragma once
#include "audio_clock_host.h"

// Absolute QPC schedule. Fractional ticks are retained through the epoch and
// quantum index rather than discarded once per callback.
// Preconditions: positive QPC frequency, supported host sample rate/quantum,
// and monotone nonnegative QPC timestamps within int64 range.
class SoftwareClockDeadline final {
public:
    SoftwareClockDeadline(std::int64_t origin, std::int64_t frequency,
        double sampleRate, std::uint32_t frames) noexcept;
    std::int64_t deadline() const noexcept;
    std::uint64_t advancePast(std::int64_t now) noexcept;
private:
    std::int64_t origin_;
    long double ticksPerBlockNumerator_;
    long double sampleRate_;
    std::uint64_t index_ = 1;
};

class SoftwareClockHost final : public AudioClockHost {
public:
    SoftwareClockHost();
    ~SoftwareClockHost() override;
    SoftwareClockHost(const SoftwareClockHost&) = delete;
    SoftwareClockHost& operator=(const SoftwareClockHost&) = delete;
    std::vector<AudioClockDeviceInfo> enumerate() const override;
    bool open(const std::wstring&, void*, AudioClockCapabilities&, std::string&) override;
    bool setSampleRate(double, std::string&) override;
    bool refreshCapabilities(AudioClockCapabilities&, std::string&) override;
    bool consumeSampleRateChange() noexcept override;
    std::uint32_t consumeErrorCode() noexcept override;
    AudioClockTimingCounters consumeTimingCounters() noexcept override;
    void setStageMeasurementsEnabled(bool) noexcept override;
    StageTimingCounters consumeOutputCopyStageCounters() noexcept override;
    bool start(std::uint32_t, const std::vector<std::uint32_t>&,
        const std::vector<std::uint32_t>&, AudioClockProcessCallback, void*, std::string&) override;
    void stop() noexcept override;
    void close() noexcept override;
private:
    struct Session;
    std::unique_ptr<Session> session_;
};
