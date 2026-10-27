#include "audio_clock_host.h"
#include "audio_controller.h"
#include "vasio_client_manager.h"

#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct HostState {
    std::atomic<std::uint32_t> failureCode{0};
    bool refuseRate = false;
    bool opened = false;
    bool workerLifetime = true;
    DWORD caller = GetCurrentThreadId();
    unsigned stops = 0;
    unsigned closes = 0;
    unsigned creations = 0;
    unsigned destructions = 0;
};

class FakeClock final : public AudioClockHost {
public:
    explicit FakeClock(HostState& state) : state_(state), thread_(GetCurrentThreadId()) {
        ++state_.creations;
        state_.workerLifetime = thread_ != state_.caller;
    }
    ~FakeClock() override {
        state_.workerLifetime = state_.workerLifetime && GetCurrentThreadId() == thread_;
        ++state_.destructions;
    }
    std::vector<AudioClockDeviceInfo> enumerate() const override {
        checkThread();
        return {{L"fake", "Test clock"}};
    }
    bool open(const std::wstring& id, void*, AudioClockCapabilities& caps, std::string&) override {
        checkThread();
        require(id == L"fake", "selected device reaches host");
        caps_ = {};
        caps_.inputChannels = caps_.outputChannels = 1;
        caps_.minBufferFrames = 16;
        caps_.maxBufferFrames = 256;
        caps_.preferredBufferFrames = 64;
        caps_.bufferGranularity = -1;
        caps_.currentSampleRate = 48000;
        caps_.supportedSampleRates = {48000, 96000};
        caps = caps_;
        state_.opened = true;
        return true;
    }
    bool setSampleRate(double rate, std::string&) override {
        checkThread();
        if (!state_.refuseRate) caps_.currentSampleRate = rate;
        return true;
    }
    bool refreshCapabilities(AudioClockCapabilities& caps, std::string&) override {
        checkThread(); caps = caps_; return true;
    }
    bool consumeSampleRateChange() noexcept override { return false; }
    std::uint32_t consumeErrorCode() noexcept override { return state_.failureCode.exchange(0); }
    AudioClockTimingCounters consumeTimingCounters() noexcept override { return {}; }
    void setStageMeasurementsEnabled(bool) noexcept override {}
    StageTimingCounters consumeOutputCopyStageCounters() noexcept override { return {}; }
    bool start(std::uint32_t, const std::vector<std::uint32_t>&,
        const std::vector<std::uint32_t>&, AudioClockProcessCallback,
        void*, std::string&) override {
        throw std::runtime_error("discovery-only controller must not start callbacks");
    }
    void stop() noexcept override { ++state_.stops; }
    void close() noexcept override { state_.opened = false; ++state_.closes; }
private:
    void checkThread() const { require(GetCurrentThreadId() == thread_, "host stays on controller worker"); }
    HostState& state_;
    DWORD thread_;
    AudioClockCapabilities caps_;
};
}

int main() {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 2;
    const auto path = std::wstring(temp) + L"TimoxVasio-clock-test-" +
        std::to_wstring(GetCurrentProcessId()) + L".json";
    const auto profiles = path + L".profiles";
    DeleteFileW(path.c_str());
    int result = 0;
    try {
        HostState state;
        VasioClientManager clients;
        AudioController controller(clients, path, profiles,
            [&] { return std::make_unique<FakeClock>(state); });
        require(controller.Start(), "injected controller starts");
        require(controller.EnumeratePhysicalDrivers().front().id == L"fake", "injected discovery is visible");
        AudioControllerConfiguration config;
        config.physicalDriverId = "fake";
        config.sampleRate = 96000;
        config.bufferFrames = 64;
        require(controller.ApplyConfiguration(config).success, "device-only configuration applies");
        auto snapshot = controller.Snapshot();
        require(snapshot.state == "stopped" && snapshot.sampleRate == 96000 &&
            snapshot.bufferFrames == 64, "discovery keeps confirmed format without starting callbacks");
        const auto stops = state.stops;
        const auto closes = state.closes;
        require(state.opened && controller.StopAudio().success && !state.opened &&
            state.stops == stops + 1 && state.closes == closes + 1,
            "audio stop stops and closes the configured host");
        require(controller.StartAudio().success && controller.Snapshot().sampleRate == 96000,
            "audio restart restores confirmed configuration");
        config.bufferFrames = 63;
        require(!controller.ApplyConfiguration(config).success && !state.opened,
            "unsupported quantum fails with stopped host");
        config.bufferFrames = 64;
        state.refuseRate = true;
        require(!controller.ApplyConfiguration(config).success && controller.Snapshot().state == "error",
            "unconfirmed rate fails instead of publishing requested format");
        controller.Stop();
        require(state.creations == 1 && state.destructions == 1 && state.workerLifetime,
            "worker distinct from caller owns host lifetime");
        DeleteFileW(path.c_str());
        {
            HostState catalogState;
            AudioController software(clients, path, profiles,
                [&] { return std::make_unique<FakeClock>(catalogState); });
            require(software.Start(), "software controller starts");
            AudioControllerConfiguration settings;
            settings.clockSource = AudioClockSource::Software;
            settings.sampleRate = 48000;
            settings.bufferFrames = 64;
            require(software.ApplyConfiguration(settings).success, "software clock applies without hardware");
            const auto running = software.Snapshot();
            require(running.state == "running" && running.clockSource == AudioClockSource::Software &&
                !running.physicalDriverId && running.physicalCapabilities.maxBufferFrames == 0 &&
                running.clockCapabilities.currentSampleRate == 48000,
                "software clock runs with separate clock and physical capabilities");
            require(software.EnumeratePhysicalDrivers().front().id == L"fake",
                "physical inventory remains ASIO while software clock runs");
            settings.physicalDriverId = "fake";
            require(!software.ApplyConfiguration(settings).success &&
                software.Snapshot().revision == running.revision,
                "invalid software driver combination leaves running graph untouched");
            settings.physicalDriverId.reset();
            settings.bufferFrames = 0;
            require(!software.ApplyConfiguration(settings).success &&
                software.Snapshot().revision == running.revision,
                "invalid software quantum leaves running graph untouched");
            settings.bufferFrames = 64;
            settings.clockSource = AudioClockSource::Asio;
            require(!software.ApplyConfiguration(settings).success &&
                software.Snapshot().revision == running.revision && software.Snapshot().state == "running",
                "ASIO without driver cannot stop the running software clock");
            require(software.ApplyConfiguration(settings, running.configurationRevision, true).success &&
                software.Snapshot().clockSource == AudioClockSource::Software,
                "omitted clock source preserves saved choice in transaction");
            const auto preserved = software.Snapshot();
            require(software.ApplyConfiguration(settings, running.configurationRevision, true).conflict &&
                software.Snapshot().revision == preserved.revision,
                "stale clock transaction cannot mutate running configuration");
            require(software.StopAudio().success && software.Snapshot().state == "stopped",
                "software audio stops");
            require(software.StartAudio().success && software.Snapshot().state == "running",
                "software audio restarts");
            require(software.ResetAudio().success && software.Snapshot().state == "running",
                "software audio resets");
            settings.physicalDriverId = "fake";
            require(software.ApplyConfiguration(settings).success &&
                software.Snapshot().clockSource == AudioClockSource::Asio && catalogState.opened,
                "explicit transition returns ownership to ASIO host");
            catalogState.failureCode.store(1234);
            for (unsigned attempt = 0; attempt < 100 && software.Snapshot().state != "error"; ++attempt)
                Sleep(10);
            require(software.Snapshot().state == "error" && !catalogState.opened,
                "backend failure stops and closes host with visible error");
            const auto failedRevision = software.Snapshot().revision;
            Sleep(300);
            require(software.Snapshot().revision == failedRevision,
                "backend failure cannot automatically restart audio");
            require(software.StartAudio().success && catalogState.opened,
                "explicit start recovers failed backend");
            settings.clockSource = AudioClockSource::Software;
            settings.physicalDriverId.reset();
            settings.routes.push_back({"saved-hardware", "physical:fake:input:1",
                "virtual:TimoxVasio:app:absent.exe:input:1", 0.0, false});
            require(software.ApplyConfiguration(settings).success && !catalogState.opened,
                "ASIO closes before software clock starts");
            require(software.Snapshot().routes.empty() && software.Snapshot().configuredRoutes.size() == 1,
                "software snapshot preserves configured hardware route as dormant");
            software.Stop();
            require(software.Start() && software.Snapshot().clockSource == AudioClockSource::Software &&
                software.Snapshot().state == "running" && software.Snapshot().configuredRoutes.size() == 1,
                "saved software mode and dormant hardware route restore");
            software.Stop();
        }
        DeleteFileW(path.c_str());
        AudioController nullController(clients, path, profiles,
            [] { return std::unique_ptr<AudioClockHost>{}; });
        require(!nullController.Start() && nullController.Snapshot().state == "error",
            "null factory fails startup cleanly");
        nullController.Stop();
        AudioController throwingController(clients, path, profiles,
            []() -> std::unique_ptr<AudioClockHost> { throw std::runtime_error("factory failure"); });
        for (unsigned attempt = 0; attempt < 50; ++attempt) {
            require(!throwingController.Start() && throwingController.Snapshot().state == "error",
                "throwing factory consistently reports failed startup");
            throwingController.Stop();
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        result = 1;
    }
    DeleteFileW(path.c_str());
    DeleteFileW(profiles.c_str());
    if (!result) std::puts("PASS: injected clock lifecycle and format validation");
    return result;
}
