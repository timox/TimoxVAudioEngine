#pragma once

#include "audio_clock_host.h"

// Enumerates the registered ASIO drivers without initializing or opening them.
class PhysicalAsioHost final : public AudioClockHost {
public:
    PhysicalAsioHost();
    ~PhysicalAsioHost() override;
    PhysicalAsioHost(const PhysicalAsioHost&) = delete;
    PhysicalAsioHost& operator=(const PhysicalAsioHost&) = delete;

    std::vector<PhysicalAsioDriverInfo> enumerate() const override;
    bool open(const std::wstring& driverId, void* systemHandle,
              PhysicalAsioCapabilities& capabilities, std::string& error) override;
    bool setSampleRate(double sampleRate, std::string& error) override;
    bool refreshCapabilities(PhysicalAsioCapabilities& capabilities, std::string& error) override;
    bool consumeSampleRateChange() noexcept override;
    PhysicalAsioTimingCounters consumeTimingCounters() noexcept override;
    void setStageMeasurementsEnabled(bool enabled) noexcept override;
    StageTimingCounters consumeOutputCopyStageCounters() noexcept override;
    bool start(std::uint32_t bufferFrames, const std::vector<std::uint32_t>& inputChannels,
               const std::vector<std::uint32_t>& outputChannels,
               PhysicalAsioProcessCallback callback, void* context, std::string& error) override;
    void stop() noexcept override;
    void close() noexcept override;

private:
    struct Session;
    std::unique_ptr<Session> session_;
};
