#pragma once

#include "audio_meter.h"
#include "asio_buffer_health.h"
#include "application_profile.h"
#include "application_profile_store.h"
#include "audio_configuration_store.h"
#include "audio_clock_host.h"
#include "realtime_callback_slot.h"
#include "routing_graph.h"
#include "wasapi_bridge_configuration.h"
#include "wasapi_bridge_manager.h"
#include "native_audio_configuration.h"
#include "native_audio_manager.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class AudioRoutingRuntime;
class VasioClientManager;
struct VasioClientSnapshot;

enum class AudioClockSource { Asio, Software };

struct AudioControllerConfiguration {
    AudioClockSource clockSource = AudioClockSource::Asio;
    std::optional<std::string> physicalDriverId;
    std::optional<std::uint32_t> sampleRate;
    std::optional<std::uint32_t> bufferFrames;
    std::vector<AudioRoute> routes;
    std::vector<WasapiBridgeConfiguration> wasapiBridges;
    NativeAudioConfiguration nativeAudio;
};

struct AudioControllerResult {
    bool success = false;
    std::string error;
    bool conflict = false;
};

struct AudioControllerSnapshot {
    AudioClockSource clockSource = AudioClockSource::Asio;
    AudioClockCapabilities clockCapabilities;
    std::uint64_t revision = 0;
    std::uint64_t configurationRevision = 0;
    std::string state = "stopped";
    std::optional<std::string> physicalDriverId;
    std::optional<std::uint32_t> sampleRate;
    std::optional<std::uint32_t> bufferFrames;
    std::string lastError;
    std::vector<AudioRoute> routes;
    std::vector<AudioRoute> configuredRoutes;
    std::vector<WasapiBridgeConfiguration> configuredWasapiBridges;
    NativeAudioConfiguration configuredNativeAudio;
    NativeAudioManagerSnapshot nativeAudio;
    std::vector<WasapiBridgeSnapshot> wasapiBridges;
    std::vector<WasapiDeviceInfo> wasapiDevices;
    std::string wasapiInventoryError;
    std::vector<WasapiProcessInfo> wasapiProcesses;
    std::string wasapiProcessInventoryError;
    PhysicalAsioCapabilities physicalCapabilities;
};

struct AudioBufferHealthSnapshot {
    bool available = false;
    AsioBufferWindow window{};
    AsioBufferAssessment assessment{};
};

struct PerformanceDuration {
    std::uint64_t samples = 0;
    std::uint64_t meanNs = 0;
    std::uint64_t worstNs = 0;
};

struct PerformanceClientWindow {
    std::uint32_t processId = 0;
    std::string driverId;
    std::optional<PerformanceDuration> hostCallback;
    std::optional<PerformanceDuration> ringWrite;
    std::optional<PerformanceDuration> ringWait;
    std::uint64_t ringWaitDiscardedBlocks = 0;
};

struct PerformancePhysicalWindow {
    std::optional<PerformanceDuration> clientRead;
    std::optional<PerformanceDuration> routing;
    std::optional<PerformanceDuration> outputCopy;
};

struct PerformanceMeasurementsWindow {
    std::uint32_t durationMs = 5000;
    std::vector<PerformanceClientWindow> clients;
    PerformancePhysicalWindow physical;
};

struct PerformanceMeasurementsSnapshot {
    bool enabled = false;
    std::optional<PerformanceMeasurementsWindow> window;
};

class AudioController final {
public:
    // Constructed and destroyed on the controller worker, including after restart.
    using ClockHostFactory = std::function<std::unique_ptr<AudioClockHost>()>;
    explicit AudioController(VasioClientManager& clients,
        std::wstring configurationPath = AudioConfigurationStore::DefaultPath(),
        std::wstring applicationProfilesPath = ApplicationProfileStore::DefaultPath(),
        ClockHostFactory clockHostFactory = {},
        WasapiBridge::BackendFactory wasapiBackendFactory = {},
        WasapiBridgeManager::EndpointResolver wasapiEndpointResolver = {},
        NativeAudioManager::TransportFactory nativeTransportFactory = {});
    ~AudioController();
    AudioController(const AudioController&) = delete;
    AudioController& operator=(const AudioController&) = delete;

    bool Start();
    void Stop() noexcept;
    std::vector<PhysicalAsioDriverInfo> EnumeratePhysicalDrivers();
    std::vector<WasapiDeviceInfo> EnumerateWasapiDevices();
    std::vector<WasapiProcessInfo> EnumerateWasapiProcesses();
    // Legacy callers cannot implicitly clear native desire. A caller that owns
    // that field must explicitly pass preserveNativeAudio=false to replace it.
    AudioControllerResult ApplyConfiguration(const AudioControllerConfiguration& configuration,
        std::optional<std::uint64_t> expectedConfigurationRevision = std::nullopt,
        bool preserveClockSource = false, bool preserveWasapiBridges = false,
        bool preserveNativeAudio = true);
    AudioControllerResult StartAudio();
    AudioControllerResult ResetAudio();
    AudioControllerResult StopAudio();
    AudioControllerResult StartCorrelation(const std::string& leftEndpointId,
                                           const std::string& rightEndpointId);
    void StopCorrelation();
    AudioCorrelationSnapshot ReadCorrelation();
    ApplicationProfilesSnapshot GetApplicationProfiles();
    ApplicationProfilesUpdateResult ReplaceApplicationProfiles(
        std::vector<ApplicationProfile> profiles);
    AudioControllerSnapshot Snapshot() const;
    std::vector<AudioControllerSnapshot> EventsSince(std::uint64_t revision) const;
    std::vector<AudioMeterSnapshot> ReadMeters();
    AudioBufferHealthSnapshot ReadBufferHealth() const;
    AudioControllerResult SetPerformanceMeasurementsEnabled(bool enabled);
    PerformanceMeasurementsSnapshot ReadPerformanceMeasurements() const;

private:
    template<class Function>
    auto Invoke(Function&& function) -> decltype(function()) {
        using Result = decltype(function());
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(function));
        auto future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (!accepting_) throw std::runtime_error("Audio controller is not running");
            queue_.emplace_back([task] { (*task)(); });
        }
        SetEvent(queueEvent_);
        return future.get();
    }

    void Run() noexcept;
    AudioControllerResult ApplyOnWorker(const AudioControllerConfiguration& configuration,
                                        bool persist = true);
    void RestoreConfigurationOnWorker();
    void RestoreApplicationProfilesOnWorker();
    void SetStatus(const AudioControllerSnapshot& snapshot);

    VasioClientManager& clients_;
    AudioConfigurationStore configurationStore_;
    ApplicationProfileStore applicationProfileStore_;
    AudioControllerConfiguration desiredConfiguration_;
    std::uint64_t configurationRevision_ = 0; // Controller worker only.
    bool audioStoppedByCommand_ = false;
    std::vector<ApplicationProfile> applicationProfiles_ = ApplicationProfileStore::DefaultProfiles();
    std::string applicationProfilesError_;
    std::atomic<bool> started_{false};
    std::thread worker_;
    mutable std::mutex queueMutex_;
    std::deque<std::function<void()>> queue_;
    bool accepting_ = false;
    bool stopping_ = false;
    HANDLE queueEvent_ = nullptr;
    HANDLE startupEvent_ = nullptr;
    HWND hostWindow_ = nullptr;
    ClockHostFactory clockHostFactory_;
    std::unique_ptr<AudioClockHost> physicalHost_;
    AudioClockSource hostClockSource_ = AudioClockSource::Asio;
    std::unique_ptr<AudioRoutingRuntime> runtime_;
    mutable std::mutex stateMutex_;
    AudioControllerSnapshot state_;
    std::deque<AudioControllerSnapshot> events_;
    mutable std::mutex bufferHealthMutex_;
    AudioBufferHealthSnapshot bufferHealth_;
    AsioBufferHealthPolicy bufferHealthPolicy_;
    std::uint64_t bufferHealthGeneration_ = 0; // Controller worker only.
    mutable std::mutex performanceMeasurementsMutex_;
    PerformanceMeasurementsSnapshot performanceMeasurements_;
    std::uint64_t performanceMeasurementsGeneration_ = 0; // Controller worker only.
    void ResetPerformanceMeasurementsWindow() noexcept;
    AudioControllerResult ApplyConfigurationCore(const AudioControllerConfiguration& configuration,
        bool allowUnavailable = false,const std::shared_ptr<NativeAudioManager>& retainedNative = {},
        const std::vector<std::string>& unavailableBridgeIds = {});
    std::optional<AudioControllerResult> ApplyPreparedLive(const AudioControllerConfiguration& configuration,
        const AudioControllerConfiguration* durable);
    void ReconcileWasapiOnWorker();
    void ReconcileNativeOnWorker() noexcept;
    std::vector<NativeRoutingBinding> NativeBindings() const;
    bool TryHotSwapRoutes(const std::vector<AudioRoute>& routes,
        const std::vector<VasioClientSnapshot>& clients,
        const std::shared_ptr<WasapiBridgeManager>& manager = {});
    void StopPhysicalRuntime() noexcept;
    RealtimeCallbackSlot callbackSlot_;
    std::vector<std::uint32_t> hostPhysicalInputs_;
    std::vector<std::uint32_t> hostPhysicalOutputs_;
    WasapiBridge::BackendFactory wasapiBackendFactory_;
    WasapiBridgeManager::EndpointResolver wasapiEndpointResolver_;
    std::shared_ptr<WasapiBridgeManager> wasapiManager_;
    NativeAudioManager::TransportFactory nativeTransportFactory_;
    std::shared_ptr<NativeAudioManager> nativeManager_;
    // Actual slot publication, independent of fallible snapshot/acknowledgement.
    std::uint64_t nativePublishedEpoch_=0;
    std::vector<WasapiDeviceInfo> wasapiDevices_;
    std::string wasapiInventoryError_;
    std::vector<WasapiProcessInfo> wasapiProcesses_;
    std::string wasapiProcessInventoryError_;
    bool wasapiInventoryObserved_=false; // Worker only; subscribe after first inventory request.
    void RefreshWasapiInventoryOnWorker();
};
