#include "audio_controller.h"

#include "audio_routing_runtime.h"
#include "physical_asio_host.h"
#include "software_clock_host.h"
#include "wasapi_device_notifications.h"
#include "persistent_routes.h"
#include "vasio_client_manager.h"

#include <windows.h>

#include <cmath>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <stdexcept>
#include <utility>

namespace {
constexpr wchar_t kHostWindowClass[] = L"VirtualASIO.AudioController.MessageWindow";

template<class Counters>
std::optional<PerformanceDuration> durationFromTicks(const Counters& value,
    std::uint64_t frequency) noexcept {
    if (!value.samples || !frequency) return std::nullopt;
    const auto nanos = [frequency](std::uint64_t ticks) {
        return static_cast<std::uint64_t>(
            static_cast<long double>(ticks) * 1000000000.0L / frequency);
    };
    return PerformanceDuration{value.samples, static_cast<std::uint64_t>(
        static_cast<long double>(value.sumTicks) * 1000000000.0L /
        frequency / value.samples),
        nanos(value.worstTicks)};
}

bool supportedBufferFrames(const PhysicalAsioCapabilities& capabilities, std::uint32_t frames) {
    if (!frames || capabilities.minBufferFrames < 0 || capabilities.maxBufferFrames < 0 ||
        frames < static_cast<std::uint32_t>(capabilities.minBufferFrames) ||
        frames > static_cast<std::uint32_t>(capabilities.maxBufferFrames)) return false;
    if (capabilities.bufferGranularity > 0)
        return (frames - static_cast<std::uint32_t>(capabilities.minBufferFrames)) %
            static_cast<std::uint32_t>(capabilities.bufferGranularity) == 0;
    if (capabilities.bufferGranularity == -1) return (frames & (frames - 1)) == 0;
    return capabilities.bufferGranularity == 0;
}

void configureVasioClients(VasioClientManager& clients, std::uint32_t bufferFrames,
                           std::uint32_t sampleRate) {
    clients.SetDriverConfiguration(bufferFrames, sampleRate);
}

LRESULT CALLBACK hostWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND createHostWindow() {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = hostWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kHostWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    return CreateWindowExW(0, kHostWindowClass, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, instance, nullptr);
}

bool sameRoutes(const std::vector<AudioRoute>& left, const std::vector<AudioRoute>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].id != right[index].id ||
            left[index].sourceEndpointId != right[index].sourceEndpointId ||
            left[index].destinationEndpointId != right[index].destinationEndpointId ||
            left[index].gainDb != right[index].gainDb || left[index].mute != right[index].mute)
            return false;
    }
    return true;
}

bool hasAttachedClient(const std::vector<VasioClientSnapshot>& clients) noexcept {
    return std::any_of(clients.begin(), clients.end(), [](const VasioClientSnapshot& client) {
        return client.info.driverId == 1 && client.mapping && client.mapping->EngineAttached();
    });
}

// Reserve only valid desired native/physical pairs, even before native IO exists.
// These remain dormant graph routes; opening ASIO channels does not activate them.
// Channel vectors contain global zero-based indices, not compact plane indices.
void reserveNativePhysicalChannels(const std::vector<AudioRoute>& routes,
    const std::optional<std::string>& driver,const PhysicalAsioCapabilities& caps,
    std::vector<std::uint32_t>& inputs,std::vector<std::uint32_t>& outputs) {
    if(!driver)return;
    const auto add=[&](const std::string& id,const char* direction,long count,
                       std::vector<std::uint32_t>& channels) {
        const auto prefix="physical:"+*driver+":"+direction+":";
        if(count<=0 || id.compare(0,prefix.size(),prefix)!=0)return;
        const auto text=id.substr(prefix.size());std::uint32_t channel=0;
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),channel);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || !channel ||
           channel>static_cast<unsigned long>(count) || text!=std::to_string(channel))return;
        channels.push_back(channel-1);
    };
    for(const auto& route:routes) {
        if(PersistentRoutes::NativeEndpointType(route.destinationEndpointId)==RoutingEndpointType::NativeDestination)
            add(route.sourceEndpointId,"input",caps.inputChannels,inputs);
        if(PersistentRoutes::NativeEndpointType(route.sourceEndpointId)==RoutingEndpointType::NativeSource)
            add(route.destinationEndpointId,"output",caps.outputChannels,outputs);
    }
    for(auto* channels:{&inputs,&outputs}) {
        std::sort(channels->begin(),channels->end());
        channels->erase(std::unique(channels->begin(),channels->end()),channels->end());
    }
}

void acknowledgePublishedGenerations(const std::vector<VasioClientSnapshot>& clients,
    std::uint32_t sampleRate, std::uint32_t bufferFrames) noexcept {
    for (const auto& client : clients) {
        if (client.mapping && client.mapping->AudioEpoch())
            client.mapping->AckAudioGeneration(sampleRate, bufferFrames);
    }
}
}

AudioController::AudioController(VasioClientManager& clients, std::wstring configurationPath,
                                 std::wstring applicationProfilesPath,
                                 ClockHostFactory clockHostFactory,
                                 WasapiBridge::BackendFactory wasapiBackendFactory,
                                 WasapiBridgeManager::EndpointResolver wasapiEndpointResolver, NativeAudioManager::TransportFactory nativeTransportFactory)
    : clients_(clients), configurationStore_(std::move(configurationPath)),
      applicationProfileStore_(std::move(applicationProfilesPath)),
      clockHostFactory_(std::move(clockHostFactory)),
      callbackSlot_(&AudioRoutingRuntime::PhysicalCallback),
      wasapiBackendFactory_(std::move(wasapiBackendFactory)),
      wasapiEndpointResolver_(std::move(wasapiEndpointResolver)), nativeTransportFactory_(std::move(nativeTransportFactory)) {}
AudioController::~AudioController() { Stop(); }

void AudioController::StopPhysicalRuntime() noexcept {
    ResetPerformanceMeasurementsWindow();
    callbackSlot_.Publish(nullptr);
    nativePublishedEpoch_=0;
    if (physicalHost_) physicalHost_->stop();
    callbackSlot_.WaitForQuiescence();
    runtime_.reset();
    if(nativeManager_)nativeManager_->StopAfterQuiescence(NativeAudioManager::Clock::now());
    nativeManager_.reset();
    wasapiManager_.reset(); // Graph retired first; session destruction joins workers off RT.
    hostPhysicalInputs_.clear();
    hostPhysicalOutputs_.clear();
    if (physicalHost_) physicalHost_->close();
}

bool AudioController::TryHotSwapRoutes(const std::vector<AudioRoute>& routes,
    const std::vector<VasioClientSnapshot>& clients,
    const std::shared_ptr<WasapiBridgeManager>& requestedManager) {
    const auto current = Snapshot();
    const auto manager=requestedManager?requestedManager:wasapiManager_;
    const auto bindings=manager?manager->Bindings():std::vector<WasapiRoutingBinding>{};
    const auto nativeBindings=NativeBindings();
    const auto nativeEpoch=nativeBindings.empty()?0:nativeBindings.front().generation->epoch;
    const auto activeRoutes = PersistentRoutes::ResolveForClock(routes, clients, current.physicalDriverId,bindings,nativeBindings);
    if (current.state != "running" || !runtime_ || !physicalHost_ ||
        !current.sampleRate || !current.bufferFrames ||
        current.clockSource != desiredConfiguration_.clockSource ||
        current.physicalDriverId != desiredConfiguration_.physicalDriverId ||
        (desiredConfiguration_.sampleRate && current.sampleRate != desiredConfiguration_.sampleRate) ||
        (desiredConfiguration_.bufferFrames && current.bufferFrames != desiredConfiguration_.bufferFrames))
        return false;

    AudioRoutingLayout layout;
    layout.physicalDriverId = current.physicalDriverId.value_or("");
    layout.sampleRate = *current.sampleRate;
    layout.bufferFrames = *current.bufferFrames;
    layout.physicalInputChannels = current.physicalCapabilities.inputChannels;
    layout.physicalOutputChannels = current.physicalCapabilities.outputChannels;
    layout.clients = clients;
    layout.routes = activeRoutes;
    layout.wasapiBindings=bindings;layout.nativeBindings=nativeBindings;
    std::string error;
    auto replacement = AudioRoutingRuntime::Create(std::move(layout), error);
    if (!replacement ||
        !std::includes(hostPhysicalInputs_.begin(), hostPhysicalInputs_.end(),
            replacement->RoutedPhysicalInputChannels().begin(),
            replacement->RoutedPhysicalInputChannels().end()) ||
        !std::includes(hostPhysicalOutputs_.begin(), hostPhysicalOutputs_.end(),
            replacement->RoutedPhysicalOutputChannels().begin(),
            replacement->RoutedPhysicalOutputChannels().end()))
        return false;

    ResetPerformanceMeasurementsWindow();
    const bool measurementsEnabled = ReadPerformanceMeasurements().enabled;
    replacement->SetStageMeasurementsEnabled(measurementsEnabled);
    for (const auto& client : clients)
        if (client.mapping) client.mapping->SetStageMeasurementsEnabled(measurementsEnabled);

    auto retired = std::move(runtime_);
    runtime_ = std::move(replacement);
    callbackSlot_.Publish(runtime_.get());
    nativePublishedEpoch_=nativeEpoch;
    callbackSlot_.WaitForQuiescence();
    retired.reset();
    wasapiManager_=manager;
    acknowledgePublishedGenerations(clients, *current.sampleRate, *current.bufferFrames);
    auto updated = current;
    updated.routes = activeRoutes;
    updated.configuredRoutes = desiredConfiguration_.routes;
    if(updated.lastError.find("WASAPI reconnection rejected")==0)updated.lastError.clear();
    SetStatus(updated);
    return true;
}

bool AudioController::Start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) return true;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        accepting_ = true;
        stopping_ = false;
    }
    queueEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    startupEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!queueEvent_ || !startupEvent_) {
        started_.store(false);
        std::lock_guard<std::mutex> lock(queueMutex_);
        accepting_ = false;
        if (queueEvent_) CloseHandle(queueEvent_);
        if (startupEvent_) CloseHandle(startupEvent_);
        queueEvent_ = nullptr;
        startupEvent_ = nullptr;
        return false;
    }
    try {
        worker_ = std::thread(&AudioController::Run, this);
    } catch (...) {
        started_.store(false);
        std::lock_guard<std::mutex> lock(queueMutex_);
        accepting_ = false;
        CloseHandle(queueEvent_);
        CloseHandle(startupEvent_);
        queueEvent_ = nullptr;
        startupEvent_ = nullptr;
        return false;
    }
    const DWORD startupResult = WaitForSingleObject(startupEvent_, INFINITE);
    return startupResult == WAIT_OBJECT_0 && started_.load();
}

void AudioController::Stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!worker_.joinable()) return;
        accepting_ = false;
        stopping_ = true;
    }
    if (queueEvent_) SetEvent(queueEvent_);
    if (worker_.get_id() != std::this_thread::get_id()) worker_.join();
    if (queueEvent_) {
        CloseHandle(queueEvent_);
        queueEvent_ = nullptr;
    }
    if (startupEvent_) {
        CloseHandle(startupEvent_);
        startupEvent_ = nullptr;
    }
}

std::vector<PhysicalAsioDriverInfo> AudioController::EnumeratePhysicalDrivers() {
    return Invoke([this] {
        if (hostClockSource_ == AudioClockSource::Asio) return physicalHost_->enumerate();
        auto catalog = clockHostFactory_ ? clockHostFactory_() : std::make_unique<PhysicalAsioHost>();
        return catalog ? catalog->enumerate() : std::vector<PhysicalAsioDriverInfo>{};
    });
}

std::vector<WasapiDeviceInfo> AudioController::EnumerateWasapiDevices() {
    return Invoke([this] {
        wasapiInventoryObserved_=true;
        RefreshWasapiInventoryOnWorker();
        return wasapiDevices_;
    });
}

std::vector<WasapiProcessInfo> AudioController::EnumerateWasapiProcesses() {
    return Invoke([this] {
        const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct Apartment { HRESULT result; ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); } } release{apartment};
        wasapiProcesses_.clear(); wasapiProcessInventoryError_.clear();
        if (SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE) {
            try { WasapiDeviceEnumerator{}.enumerateActiveRenderProcesses(wasapiProcesses_, wasapiProcessInventoryError_); }
            catch (...) { wasapiProcesses_.clear(); wasapiProcessInventoryError_ = "Cannot refresh Windows audio application inventory"; }
        } else wasapiProcessInventoryError_ = "Cannot initialize COM for Windows audio application inventory";
        SetStatus(Snapshot());
        return wasapiProcesses_;
    });
}

void AudioController::RefreshWasapiInventoryOnWorker() {
    const auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct Apartment {HRESULT result;~Apartment(){if(SUCCEEDED(result))CoUninitialize();}} release{apartment};
    wasapiDevices_.clear();wasapiInventoryError_.clear();
    if(SUCCEEDED(apartment) || apartment==RPC_E_CHANGED_MODE) {
        try { WasapiDeviceEnumerator{}.enumerate(wasapiDevices_,wasapiInventoryError_); }
        catch(...) {wasapiDevices_.clear();wasapiInventoryError_="Cannot refresh WASAPI inventory";}
    } else wasapiInventoryError_="Cannot initialize COM for WASAPI inventory";
    SetStatus(Snapshot());
}

void AudioController::ReconcileWasapiOnWorker() {
    {std::lock_guard<std::mutex> lock(queueMutex_);if(stopping_)return;}
    if(!wasapiManager_ || Snapshot().state!="running")return;
    try {
        auto candidate=wasapiManager_->Reconcile(WasapiBridgeManager::Clock::now());
        if(candidate && !TryHotSwapRoutes(desiredConfiguration_.routes,clients_.GetClientSnapshots(),candidate)) {
            wasapiManager_->DeferPublication(WasapiBridgeManager::Clock::now());
            auto status=Snapshot();
            status.lastError="WASAPI reconnection rejected by graph constraints; retry deferred";
            SetStatus(status);
        }
    } catch(...) {
        // A slave preparation/allocation failure must not terminate the master.
        // The existing manager retains the failed session for the next retry.
    }
}

std::vector<NativeRoutingBinding> AudioController::NativeBindings() const {
    if(!nativeManager_)return {};
    const auto status=nativeManager_->Snapshot(NativeAudioManager::Clock::now());
    if(status.state!=NativeAudioManagerState::Prepared && status.state!=NativeAudioManagerState::Attached)return {};
    const auto generation=nativeManager_->Generation();
    return generation?std::vector<NativeRoutingBinding>{{"windows",generation}}:std::vector<NativeRoutingBinding>{};
}

void AudioController::ReconcileNativeOnWorker() noexcept {
    try {
        {std::lock_guard<std::mutex> lock(queueMutex_);if(stopping_)return;}
        const auto current=Snapshot();
        if(audioStoppedByCommand_ || current.state!="running" || !current.sampleRate || !current.bufferFrames)return;
        bool changed=!nativeManager_;
        if(!nativeManager_)nativeManager_=std::make_shared<NativeAudioManager>(nativeTransportFactory_);
        const auto before=nativeManager_->Snapshot(NativeAudioManager::Clock::now());
        changed=changed || before.enabled!=desiredConfiguration_.nativeAudio.enabled || !before.masterRunning ||
            before.desiredRate!=*current.sampleRate || before.desiredQuantum!=*current.bufferFrames;
        nativeManager_->SetDesired(desiredConfiguration_.nativeAudio.enabled,true,*current.sampleRate,*current.bufferFrames);
        const auto now=NativeAudioManager::Clock::now();
        auto action=nativeManager_->Inspect(now);
        changed=changed || action!=NativeAudioAction::None;
        if(action==NativeAudioAction::Detach) {
            // NativeBindings excludes detaching sessions. Keep the sole owner
            // until a graph without its IO has been published and quiesced.
            if(!TryHotSwapRoutes(desiredConfiguration_.routes,clients_.GetClientSnapshots()))return;
            nativeManager_->StopAfterQuiescence(now);
            action=nativeManager_->Inspect(now);
        }
        if(action==NativeAudioAction::AttemptOpen) {
            const auto opened=nativeManager_->AttemptOpen(now);
            if(opened.code==NativeTransferCode::Ok) {
                if(TryHotSwapRoutes(desiredConfiguration_.routes,clients_.GetClientSnapshots()))
                    nativeManager_->MarkAttached();
                else nativeManager_->StopAfterQuiescence(now,{NativeTransferCode::InvalidPacket});
            } else if(nativeManager_->Inspect(now)==NativeAudioAction::Detach) {
                // Failed preparation was never published, so no graph borrower.
                nativeManager_->StopAfterQuiescence(now);
            }
        }
        if(changed)SetStatus(Snapshot());
    } catch(...) {
        // An optional native preparation/publication must not stop the master.
        // Publication can succeed before a later snapshot allocation throws.
        // attached is an acknowledgement, not proof that no graph borrows IO.
        if(nativeManager_) {
            const auto generation=nativeManager_->Generation();
            if(generation && generation->epoch==nativePublishedEpoch_)nativeManager_->MarkAttached();
            else nativeManager_->StopAfterQuiescence(NativeAudioManager::Clock::now(),{NativeTransferCode::Failed});
        }
    }
}

std::optional<AudioControllerResult> AudioController::ApplyPreparedLive(
    const AudioControllerConfiguration& configuration,const AudioControllerConfiguration* durable) {
    const auto current=Snapshot();
    if(current.state!="running" || !runtime_ || !physicalHost_ || !current.sampleRate || !current.bufferFrames ||
       current.clockSource!=configuration.clockSource || current.physicalDriverId!=configuration.physicalDriverId ||
       (configuration.sampleRate && configuration.sampleRate!=current.sampleRate) ||
       (configuration.bufferFrames && configuration.bufferFrames!=current.bufferFrames))return std::nullopt;
    std::string error;
    auto candidate=WasapiBridgeManager::Prepare(configuration.wasapiBridges,*current.sampleRate,
        *current.bufferFrames,wasapiManager_,wasapiBackendFactory_,error,false,wasapiEndpointResolver_);
    if(!candidate)return AudioControllerResult{false,error};
    auto clients=clients_.GetClientSnapshots();
    auto bindings=candidate->Bindings();
    const auto nativeBindings=configuration.nativeAudio.enabled?NativeBindings():std::vector<NativeRoutingBinding>{};
    const auto nativeEpoch=nativeBindings.empty()?0:nativeBindings.front().generation->epoch;
    const auto routes=PersistentRoutes::ResolveForClock(configuration.routes,clients,current.physicalDriverId,bindings,
        nativeBindings);
    AudioRoutingLayout layout;
    layout.physicalDriverId=current.physicalDriverId.value_or("");
    layout.sampleRate=*current.sampleRate;layout.bufferFrames=*current.bufferFrames;
    layout.physicalInputChannels=current.physicalCapabilities.inputChannels;
    layout.physicalOutputChannels=current.physicalCapabilities.outputChannels;
    layout.clients=clients;layout.routes=routes;layout.wasapiBindings=std::move(bindings);
    layout.nativeBindings=nativeBindings;
    auto replacement=AudioRoutingRuntime::Create(std::move(layout),error);
    if(!replacement)return AudioControllerResult{false,error};
    auto requiredInputs=replacement->RoutedPhysicalInputChannels();
    auto requiredOutputs=replacement->RoutedPhysicalOutputChannels();
    reserveNativePhysicalChannels(configuration.routes,current.physicalDriverId,
        current.physicalCapabilities,requiredInputs,requiredOutputs);
    if(!std::includes(hostPhysicalInputs_.begin(),hostPhysicalInputs_.end(),
          requiredInputs.begin(),requiredInputs.end()) ||
       !std::includes(hostPhysicalOutputs_.begin(),hostPhysicalOutputs_.end(),
          requiredOutputs.begin(),requiredOutputs.end()))
        return std::nullopt;
    // All fallible preparation precedes persistence and callback publication.
    auto updated=current;updated.routes=routes;updated.configuredRoutes=configuration.routes;
    updated.configuredWasapiBridges=configuration.wasapiBridges;
    updated.configuredNativeAudio=configuration.nativeAudio;
    replacement->SetStageMeasurementsEnabled(ReadPerformanceMeasurements().enabled);
    if(durable && !configurationStore_.Save(*durable,error))return AudioControllerResult{false,error};
    ResetPerformanceMeasurementsWindow();
    auto retired=std::move(runtime_);runtime_=std::move(replacement);
    callbackSlot_.Publish(runtime_.get());nativePublishedEpoch_=nativeEpoch;
    callbackSlot_.WaitForQuiescence();retired.reset();
    wasapiManager_=std::move(candidate);
    acknowledgePublishedGenerations(clients,*current.sampleRate,*current.bufferFrames);
    SetStatus(updated);
    return AudioControllerResult{true,{}};
}

ApplicationProfilesSnapshot AudioController::GetApplicationProfiles() {
    return Invoke([this] {
        return ApplicationProfilesSnapshot{applicationProfiles_, applicationProfilesError_};
    });
}

ApplicationProfilesUpdateResult AudioController::ReplaceApplicationProfiles(
    std::vector<ApplicationProfile> profiles) {
    try {
        return Invoke([this, profiles = std::move(profiles)]() mutable {
            ApplicationProfilesUpdateResult result;
            if (!ApplicationProfileStore::NormalizeAndValidate(profiles, result.error)) return result;

            const auto previous = applicationProfiles_;
            bool unchanged = previous.size() == profiles.size();
            for (std::size_t index = 0; unchanged && index < profiles.size(); ++index) {
                unchanged = previous[index].processName == profiles[index].processName &&
                    previous[index].inputChannels == profiles[index].inputChannels &&
                    previous[index].outputChannels == profiles[index].outputChannels;
            }
            if ((!unchanged || !applicationProfilesError_.empty()) &&
                !applicationProfileStore_.Save(profiles, result.error)) return result;

            for (const auto& client : clients_.GetClientSnapshots()) {
                const auto oldProfile = ApplicationProfileStore::EffectiveProfileForExecutable(
                    client.info.processName, previous);
                const auto newProfile = ApplicationProfileStore::EffectiveProfileForExecutable(
                    client.info.processName, profiles);
                if (oldProfile.inputChannels != newProfile.inputChannels ||
                    oldProfile.outputChannels != newProfile.outputChannels) {
                    result.restartRequiredClients.push_back({client.info.processId,
                        client.info.processName});
                }
            }
            applicationProfiles_ = std::move(profiles);
            applicationProfilesError_.clear();
            result.profiles = applicationProfiles_;
            result.success = true;
            return result;
        });
    } catch (const std::exception& exception) {
        return {false, exception.what(), {}, {}};
    }
}

AudioControllerSnapshot AudioController::Snapshot() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return state_;
}

std::vector<AudioControllerSnapshot> AudioController::EventsSince(std::uint64_t revision) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::vector<AudioControllerSnapshot> result;
    for (const auto& event : events_) if (event.revision > revision) result.push_back(event);
    return result;
}

std::vector<AudioMeterSnapshot> AudioController::ReadMeters() {
    return Invoke([this] {
        return runtime_ ? runtime_->ReadMeters() : std::vector<AudioMeterSnapshot>{};
    });
}

AudioBufferHealthSnapshot AudioController::ReadBufferHealth() const {
    std::lock_guard<std::mutex> lock(bufferHealthMutex_);
    return bufferHealth_;
}

PerformanceMeasurementsSnapshot AudioController::ReadPerformanceMeasurements() const {
    std::lock_guard<std::mutex> lock(performanceMeasurementsMutex_);
    return performanceMeasurements_;
}

void AudioController::ResetPerformanceMeasurementsWindow() noexcept {
    ++performanceMeasurementsGeneration_;
    std::lock_guard<std::mutex> lock(performanceMeasurementsMutex_);
    performanceMeasurements_.window.reset();
}

AudioControllerResult AudioController::SetPerformanceMeasurementsEnabled(bool enabled) {
    try {
        return Invoke([this, enabled] {
            {
                std::lock_guard<std::mutex> lock(performanceMeasurementsMutex_);
                if (performanceMeasurements_.enabled == enabled) return AudioControllerResult{true, {}};
                performanceMeasurements_.enabled = enabled;
                performanceMeasurements_.window.reset();
            }
            ++performanceMeasurementsGeneration_;
            if (runtime_) runtime_->SetStageMeasurementsEnabled(enabled);
            if (physicalHost_) physicalHost_->setStageMeasurementsEnabled(enabled);
            for (const auto& client : clients_.GetClientSnapshots())
                if (client.mapping) client.mapping->SetStageMeasurementsEnabled(enabled);
            return AudioControllerResult{true, {}};
        });
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

AudioControllerResult AudioController::ApplyConfiguration(
    const AudioControllerConfiguration& configuration,
    std::optional<std::uint64_t> expectedConfigurationRevision, bool preserveClockSource,bool preserveWasapiBridges,
    bool preserveNativeAudio) {
    try {
        return Invoke([this, configuration, expectedConfigurationRevision, preserveClockSource,preserveWasapiBridges,preserveNativeAudio] {
            if (expectedConfigurationRevision &&
                *expectedConfigurationRevision != configurationRevision_)
                return AudioControllerResult{false,
                    "Saved configuration changed in another application; refresh and retry", true};
            auto effective = configuration;
            if (preserveClockSource) effective.clockSource = desiredConfiguration_.clockSource;
            if (preserveWasapiBridges) effective.wasapiBridges=desiredConfiguration_.wasapiBridges;
            if (preserveNativeAudio) effective.nativeAudio=desiredConfiguration_.nativeAudio;
            return ApplyOnWorker(effective);
        });
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

AudioControllerResult AudioController::StartAudio() {
    try {
        return Invoke([this] {
            if (!audioStoppedByCommand_ && Snapshot().state == "running")
                return AudioControllerResult{true, {}};
            const auto result = ApplyConfigurationCore(desiredConfiguration_);
            if (result.success) {audioStoppedByCommand_ = false;ReconcileNativeOnWorker();}
            return result;
        });
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

AudioControllerResult AudioController::ResetAudio() {
    try {
        return Invoke([this] {
            const auto result = ApplyConfigurationCore(desiredConfiguration_);
            if (result.success) {audioStoppedByCommand_ = false;ReconcileNativeOnWorker();}
            return result;
        });
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

AudioControllerResult AudioController::StopAudio() {
    try {
        return Invoke([this] {
            if (audioStoppedByCommand_) return AudioControllerResult{true, {}};
            auto status = Snapshot();
            status.state = "reconfiguring";
            SetStatus(status);
            if (runtime_) runtime_->StopCorrelation();
            StopPhysicalRuntime();
            ++bufferHealthGeneration_;
            bufferHealthPolicy_.Reset();
            { std::lock_guard<std::mutex> lock(bufferHealthMutex_); bufferHealth_ = {}; }
            status.state = "stopped";
            status.lastError.clear();
            SetStatus(status);
            audioStoppedByCommand_ = true;
            return AudioControllerResult{true, {}};
        });
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

AudioControllerResult AudioController::StartCorrelation(const std::string& leftEndpointId,
                                                          const std::string& rightEndpointId) {
    try {
        return Invoke([this, leftEndpointId, rightEndpointId] {
            if (Snapshot().state != "running" || !runtime_)
                return AudioControllerResult{false, "Audio engine is not running"};
            if (!runtime_->SetCorrelationPair(leftEndpointId, rightEndpointId))
                return AudioControllerResult{false, "Stereo pair is not an active routed client output"};
            return AudioControllerResult{true, {}};
        });
    } catch (const std::exception& exception) {
        return {false, exception.what()};
    }
}

void AudioController::StopCorrelation() {
    try {
        Invoke([this] { if (runtime_) runtime_->StopCorrelation(); });
    } catch (...) {}
}

AudioCorrelationSnapshot AudioController::ReadCorrelation() {
    try {
        return Invoke([this] {
            return runtime_ ? runtime_->ReadCorrelation() : AudioCorrelationSnapshot{};
        });
    } catch (...) {
        return {};
    }
}

void AudioController::SetStatus(const AudioControllerSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto revision = state_.revision + 1;
    state_ = snapshot;
    if(nativeManager_)state_.nativeAudio=nativeManager_->Snapshot(NativeAudioManager::Clock::now());
    else {
        state_.nativeAudio={};state_.nativeAudio.enabled=state_.configuredNativeAudio.enabled;
        state_.nativeAudio.masterRunning=state_.state=="running";
        state_.nativeAudio.state=state_.configuredNativeAudio.enabled?NativeAudioManagerState::WaitingForMaster:NativeAudioManagerState::Disabled;
    }
    state_.wasapiBridges=wasapiManager_?wasapiManager_->Snapshots():std::vector<WasapiBridgeSnapshot>{};
    if(!wasapiManager_)for(const auto& config:state_.configuredWasapiBridges) {
        WasapiBridgeSnapshot bridge;bridge.id=config.id;bridge.state=config.enabled?"stopped":"disabled";
        bridge.health.state=WasapiBridgeState::Stopped;state_.wasapiBridges.push_back(std::move(bridge));
    }
    state_.wasapiDevices=wasapiDevices_;
    state_.wasapiInventoryError=wasapiInventoryError_;
    state_.wasapiProcesses=wasapiProcesses_;
    state_.wasapiProcessInventoryError=wasapiProcessInventoryError_;
    state_.revision = revision;
    state_.configurationRevision = configurationRevision_;
    events_.push_back(state_);
    if (events_.size() > 64) events_.pop_front();
}

void AudioController::Run() noexcept {
    // Keep COM alive through observer unregistration AND ASIO host destruction.
    // The controller owns a message pump; STA also matches the ASIO host model.
    const auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct Apartment {HRESULT result;~Apartment(){if(SUCCEEDED(result))CoUninitialize();}} release{apartment};
    WasapiDeviceNotifications deviceNotifications;
    try {
        hostWindow_ = createHostWindow();
        if (!hostWindow_) throw std::runtime_error("Unable to create ASIO message window");
        physicalHost_ = clockHostFactory_ ? clockHostFactory_() : std::make_unique<PhysicalAsioHost>();
        hostClockSource_ = AudioClockSource::Asio;
        if (!physicalHost_) throw std::runtime_error("Audio clock host factory returned null");
        RestoreApplicationProfilesOnWorker();
        RestoreConfigurationOnWorker();
        const bool notificationsAvailable=SUCCEEDED(deviceNotifications.Start());
        if (startupEvent_) SetEvent(startupEvent_);
        auto nextInventoryRefresh=std::chrono::steady_clock::time_point{};
        auto nextRouteReconcile = std::chrono::steady_clock::time_point{};
        auto nextWasapiHealth=std::chrono::steady_clock::time_point{};
        auto bufferWindowStart = std::chrono::steady_clock::time_point{};
        auto observedBufferHealthGeneration = bufferHealthGeneration_;
        auto observedPerformanceGeneration = performanceMeasurementsGeneration_;
        auto performanceWindowStart = std::chrono::steady_clock::time_point{};
        for (;;) {
            const DWORD waitResult = MsgWaitForMultipleObjectsEx(1, &queueEvent_, 20,
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (waitResult == WAIT_FAILED) throw std::runtime_error("Audio controller message wait failed");
            if (waitResult == WAIT_OBJECT_0) {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::lock_guard<std::mutex> lock(queueMutex_);
                        if (queue_.empty()) break;
                        task = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    task();
                }
            }
            if (waitResult == WAIT_OBJECT_0 + 1) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            const auto inventoryNow=std::chrono::steady_clock::now();
            if(wasapiInventoryObserved_ && inventoryNow>=nextInventoryRefresh &&
               (!notificationsAvailable || !wasapiInventoryError_.empty() || deviceNotifications.ConsumeChange())) {
                // Consume before enumeration so an event during the scan remains
                // pending. Coalesce storms; retry errors/registration failure at 5s.
                RefreshWasapiInventoryOnWorker();
                nextInventoryRefresh=std::chrono::steady_clock::now()+
                    ((notificationsAvailable&&wasapiInventoryError_.empty()) ? std::chrono::milliseconds(500) : std::chrono::milliseconds(5000));
            }
            const auto clockError = physicalHost_ ? physicalHost_->consumeErrorCode() : 0;
            if (clockError) {
                auto failed = Snapshot();
                StopPhysicalRuntime();
                audioStoppedByCommand_ = true;
                failed.state = "error";
                failed.clockCapabilities = {};
                failed.physicalCapabilities = {};
                failed.lastError = "Audio clock backend failed (Windows error " +
                    std::to_string(clockError) + "); explicitly start or reset audio to retry";
                SetStatus(failed);
            }
            if (!clockError && physicalHost_ && physicalHost_->consumeSampleRateChange()) {
                const auto previous = Snapshot();
                StopPhysicalRuntime();
                AudioControllerSnapshot failed = previous;
                failed.state = "error";
                failed.physicalCapabilities = {};
                failed.lastError = "Physical ASIO driver reported a sample-rate change; reapply the configuration to resynchronize TimoxVasio";
                SetStatus(failed);
            }
            if (!audioStoppedByCommand_ && (desiredConfiguration_.physicalDriverId ||
                    desiredConfiguration_.clockSource == AudioClockSource::Software)) {
                ReconcileWasapiOnWorker();
                ReconcileNativeOnWorker();
                if((wasapiManager_ || nativeManager_) && std::chrono::steady_clock::now()>=nextWasapiHealth) {
                    SetStatus(Snapshot());nextWasapiHealth=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
                }
                const auto clients = clients_.GetClientSnapshots();
                const auto resolved = PersistentRoutes::ResolveForClock(desiredConfiguration_.routes,
                    clients, desiredConfiguration_.physicalDriverId,
                    wasapiManager_?wasapiManager_->Bindings():std::vector<WasapiRoutingBinding>{},NativeBindings());
                const auto current = Snapshot();
                const bool attachedClient = hasAttachedClient(clients);
                const bool routesChanged = !sameRoutes(resolved, current.routes);
                const bool generationChanged = runtime_ &&
                    !runtime_->MatchesClientGenerations(clients);
                const bool callbackNeeded = attachedClient && current.state != "running";
                if (routesChanged || generationChanged || callbackNeeded) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= nextRouteReconcile) {
                        const bool swapped = TryHotSwapRoutes(resolved, clients);
                        const auto applied = swapped ? AudioControllerResult{true, {}} :
                            ApplyConfigurationCore(desiredConfiguration_);
                        nextRouteReconcile = now + (applied.success
                            ? std::chrono::milliseconds(250) : std::chrono::seconds(2));
                    }
                } else {
                    nextRouteReconcile = std::chrono::steady_clock::time_point{};
                }
                if (resolved.size() == desiredConfiguration_.routes.size()) {
                    std::string migrationError;
                    auto durable = PersistentRoutes::Encode(desiredConfiguration_.routes,
                        clients, migrationError);
                    if (migrationError.empty() &&
                        !sameRoutes(durable, desiredConfiguration_.routes)) {
                        auto updated = desiredConfiguration_;
                        updated.routes = std::move(durable);
                        if (configurationStore_.Save(updated, migrationError)) {
                            desiredConfiguration_ = std::move(updated);
                            auto status = Snapshot();
                            status.configuredRoutes = desiredConfiguration_.routes;
                            SetStatus(status);
                        }
                    }
                }
            }
            if (observedBufferHealthGeneration != bufferHealthGeneration_) {
                observedBufferHealthGeneration = bufferHealthGeneration_;
                bufferWindowStart = {};
            }
            const auto currentStatus = Snapshot();
            if (currentStatus.state == "running" && currentStatus.sampleRate &&
                currentStatus.bufferFrames && physicalHost_) {
                const auto now = std::chrono::steady_clock::now();
                if (bufferWindowStart == std::chrono::steady_clock::time_point{})
                    bufferWindowStart = now;
                const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - bufferWindowStart).count();
                if (durationMs >= 5000) {
                    const auto counters = physicalHost_->consumeTimingCounters();
                    AsioBufferWindow window{*currentStatus.sampleRate, *currentStatus.bufferFrames,
                        static_cast<std::uint64_t>(durationMs), counters.callbacks,
                        counters.delayedStarts, counters.processingOverruns,
                        counters.worstProcessingNs};
                    const auto assessment = bufferHealthPolicy_.Assess(window);
                    { std::lock_guard<std::mutex> lock(bufferHealthMutex_);
                      bufferHealth_ = {true, window, assessment}; }
                    bufferWindowStart = now;
                }
            } else {
                bufferWindowStart = {};
                bufferHealthPolicy_.Reset();
                { std::lock_guard<std::mutex> lock(bufferHealthMutex_); bufferHealth_ = {}; }
            }
            if (observedPerformanceGeneration != performanceMeasurementsGeneration_) {
                observedPerformanceGeneration = performanceMeasurementsGeneration_;
                performanceWindowStart = {};
                if (runtime_) runtime_->ConsumeStageCounters();
                if (physicalHost_) physicalHost_->consumeOutputCopyStageCounters();
                for (const auto& client : clients_.GetClientSnapshots())
                    if (client.mapping) client.mapping->ConsumeClientStageCounters();
            }
            const bool measuring = ReadPerformanceMeasurements().enabled;
            if (measuring && currentStatus.state == "running" && runtime_ && physicalHost_) {
                const auto now = std::chrono::steady_clock::now();
                if (performanceWindowStart == std::chrono::steady_clock::time_point{})
                    performanceWindowStart = now;
                if (now - performanceWindowStart >= std::chrono::seconds(5)) {
                    LARGE_INTEGER frequencyValue{};
                    QueryPerformanceFrequency(&frequencyValue);
                    const auto frequency = frequencyValue.QuadPart > 0
                        ? static_cast<std::uint64_t>(frequencyValue.QuadPart) : 0;
                    PerformanceMeasurementsWindow window;
                    const auto runtimeCounters = runtime_->ConsumeStageCounters();
                    window.physical.clientRead = durationFromTicks(runtimeCounters.clientRead, frequency);
                    window.physical.routing = durationFromTicks(runtimeCounters.routing, frequency);
                    window.physical.outputCopy = durationFromTicks(
                        physicalHost_->consumeOutputCopyStageCounters(), frequency);
                    for (const auto& client : clients_.GetClientSnapshots()) {
                        PerformanceClientWindow item;
                        item.processId = client.info.processId;
                        item.driverId = client.info.driverId == 1
                            ? "TimoxVasio" : std::to_string(client.info.driverId);
                        if (client.mapping) {
                            const auto counters = client.mapping->ConsumeClientStageCounters();
                            if (client.mapping->EngineAttached() && counters.epoch &&
                                counters.epoch == client.mapping->AudioEpoch() &&
                                counters.epoch == client.mapping->PublishedAudioEpoch()) {
                                item.hostCallback = durationFromTicks(counters.hostCallback, frequency);
                                item.ringWrite = durationFromTicks(counters.ringWrite, frequency);
                                item.ringWait = durationFromTicks(counters.ringWait, frequency);
                                item.ringWaitDiscardedBlocks = counters.ringWaitDiscardedBlocks;
                            }
                        }
                        window.clients.push_back(std::move(item));
                    }
                    { std::lock_guard<std::mutex> lock(performanceMeasurementsMutex_);
                      performanceMeasurements_.window = std::move(window); }
                    performanceWindowStart = now;
                }
            } else {
                performanceWindowStart = {};
            }
            bool shouldStop = false;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                shouldStop = stopping_ && queue_.empty();
            }
            if (shouldStop) break;
        }
        StopPhysicalRuntime();
        physicalHost_.reset();
        if (hostWindow_) DestroyWindow(hostWindow_);
        hostWindow_ = nullptr;
    } catch (...) {
        StopPhysicalRuntime();
        physicalHost_.reset();
        if (hostWindow_) DestroyWindow(hostWindow_);
        hostWindow_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            accepting_ = false;
            stopping_ = true;
        }
        AudioControllerSnapshot failed;
        failed.state = "error";
        failed.lastError = "Audio controller worker failed";
        SetStatus(failed);
        started_.store(false);
        if (startupEvent_) SetEvent(startupEvent_);
    }
    started_.store(false);
}

void AudioController::RestoreApplicationProfilesOnWorker() {
    std::vector<ApplicationProfile> storedProfiles;
    std::string error;
    if (!applicationProfileStore_.Load(storedProfiles, error)) {
        applicationProfiles_ = ApplicationProfileStore::DefaultProfiles();
        applicationProfilesError_ = std::move(error);
    } else {
        applicationProfiles_ = std::move(storedProfiles);
        applicationProfilesError_.clear();
    }
}

AudioControllerResult AudioController::ApplyOnWorker(
    const AudioControllerConfiguration& configuration, bool persist) {
    const auto previous=Snapshot();
    const auto oldConfiguration=desiredConfiguration_;
    // Keep the exclusive native owner alive across a master reconstruction.
    // It is detached from the member before Core tears down the old runtime;
    // failed persistence can then reattach the same IO/epoch without reopening.
    auto retainedNative=nativeManager_;
    bool coreStarted=false,committed=false;
    const auto restore=[&] {
        if(!coreStarted || committed)return;
        if(previous.state=="running" && Snapshot().revision!=previous.revision) {
            const auto result=ApplyConfigurationCore(oldConfiguration,true,retainedNative);
            if(!result.success) {
                StopPhysicalRuntime();
                auto failed=Snapshot();failed.lastError="Configuration rollback failed: "+result.error;
                SetStatus(failed);
            }
        }else nativeManager_=retainedNative;
    };
    try {
        std::string routeError;
        AudioControllerConfiguration durable = configuration;
        durable.routes = PersistentRoutes::Encode(configuration.routes,
            clients_.GetClientSnapshots(), routeError);
        if (!routeError.empty()) return {false, routeError};
        const auto live=ApplyPreparedLive(configuration,persist?&durable:nullptr);
        AudioControllerResult applied;
        if(live)applied=*live;
        else {
            coreStarted=true;nativeManager_.reset();
            // An unavailable configured endpoint must not prevent applying
            // unrelated routes or bridges. Keep an unchanged existing bridge
            // in retry state; a newly selected invalid endpoint still fails.
            std::vector<std::string> unavailableBridgeIds;
            for(const auto& bridge:configuration.wasapiBridges) {
                if(!bridge.enabled)continue;
                const auto old=std::find_if(oldConfiguration.wasapiBridges.begin(),oldConfiguration.wasapiBridges.end(),
                    [&](const auto& candidate){return candidate.id==bridge.id;});
                if(old==oldConfiguration.wasapiBridges.end() || !old->enabled)continue;
                const auto& a=old->open;const auto& b=bridge.open;
                if(a.kind==b.kind && a.endpointId==b.endpointId && a.defaultRole==b.defaultRole &&
                    a.processId==b.processId && a.processCreationTime==b.processCreationTime &&
                    a.processImagePath==b.processImagePath && a.bufferDuration100ns==b.bufferDuration100ns)
                    unavailableBridgeIds.push_back(bridge.id);
            }
            applied=ApplyConfigurationCore(configuration,false,{},unavailableBridgeIds);
        }
        if (!applied.success) {restore();return applied;}
        if(!persist)return applied;
        std::string error;
        if (!live && !configurationStore_.Save(durable, error)) {
            if(previous.state=="running")restore();
            else {StopPhysicalRuntime();auto failed=previous;failed.lastError=error;SetStatus(failed);}
            return {false, error};
        }
        committed=true;
        desiredConfiguration_ = std::move(durable);
        ++configurationRevision_;
        if(coreStarted && retainedNative) {
            // Core already retired every old graph borrower. Only now, after
            // durable commit, release the old owner before a replacement Open.
            retainedNative->StopAfterQuiescence(NativeAudioManager::Clock::now());
            retainedNative.reset();
        }
        auto status = Snapshot();
        status.configuredRoutes = desiredConfiguration_.routes;
        status.configuredWasapiBridges=desiredConfiguration_.wasapiBridges;
        status.configuredNativeAudio=desiredConfiguration_.nativeAudio;
        SetStatus(status);
        audioStoppedByCommand_ = false;
        ReconcileNativeOnWorker();
        return applied;
    } catch (const std::exception& exception) {
        restore();return {false, exception.what()};
    } catch (...) {
        restore();return {false,"Unknown audio controller failure"};
    }
}
void AudioController::RestoreConfigurationOnWorker() {
    AudioControllerConfiguration configuration;
    std::string error;
    const auto status = configurationStore_.Load(configuration, error);
    if (status == ConfigurationLoadStatus::Missing) return;
    if (status == ConfigurationLoadStatus::Error) {
        AudioControllerSnapshot failed;
        failed.state = "stopped";
        failed.lastError = std::move(error);
        SetStatus(failed);
        return;
    }
    desiredConfiguration_ = configuration;
    const auto restored = ApplyConfigurationCore(configuration,true);
    if (restored.success) {
        ++configurationRevision_;
        SetStatus(Snapshot());
        ReconcileNativeOnWorker();
    }
    if (!restored.success) {
        StopPhysicalRuntime();
        auto failed = Snapshot();
        failed.state = "stopped";
        failed.physicalCapabilities = {};
        failed.lastError = restored.error;
        SetStatus(failed);
    }
}

AudioControllerResult AudioController::ApplyConfigurationCore(
    const AudioControllerConfiguration& configuration,bool allowUnavailable,const std::shared_ptr<NativeAudioManager>& retainedNative,
    const std::vector<std::string>& unavailableBridgeIds) {
    const bool software = configuration.clockSource == AudioClockSource::Software;
    if (configuration.clockSource != AudioClockSource::Asio && !software)
        return {false, "Unsupported audio clock source"};
    if (software && (configuration.physicalDriverId || !configuration.sampleRate ||
            *configuration.sampleRate < 8000 || *configuration.sampleRate > 384000 ||
            !configuration.bufferFrames || *configuration.bufferFrames < 16 ||
            *configuration.bufferFrames > 8192))
        return {false, "Software clock requires no physical driver, a sample rate from 8000 to 384000 and a buffer size from 16 to 8192"};
    if (!software && !configuration.physicalDriverId &&
            (!configuration.routes.empty() || configuration.sampleRate || configuration.bufferFrames))
        return {false, "A physical ASIO clock is required for sample-rate, buffer and routing configuration"};
    std::string preparationError;
    if(!ValidateWasapiBridgeConfigurations(configuration.wasapiBridges,preparationError))return {false,preparationError};
    std::shared_ptr<WasapiBridgeManager> candidate;
    if(configuration.sampleRate && configuration.bufferFrames) {
        candidate=WasapiBridgeManager::Prepare(configuration.wasapiBridges,*configuration.sampleRate,
            *configuration.bufferFrames,{},wasapiBackendFactory_,preparationError,allowUnavailable,wasapiEndpointResolver_,unavailableBridgeIds);
        if(!candidate)return {false,preparationError};
    }
    ++bufferHealthGeneration_;
    bufferHealthPolicy_.Reset();
    { std::lock_guard<std::mutex> lock(bufferHealthMutex_); bufferHealth_ = {}; }
    AudioControllerSnapshot next;
    next.clockSource = configuration.clockSource;
    next.state = "reconfiguring";
    next.configuredRoutes = configuration.routes;
    next.configuredWasapiBridges=configuration.wasapiBridges;
    next.configuredNativeAudio=configuration.nativeAudio;
    SetStatus(next);
    StopPhysicalRuntime();
    if (hostClockSource_ != configuration.clockSource) {
        std::unique_ptr<AudioClockHost> replacement;
        if (software) replacement = std::make_unique<SoftwareClockHost>();
        else replacement = clockHostFactory_ ? clockHostFactory_() : std::make_unique<PhysicalAsioHost>();
        if (!replacement) throw std::runtime_error("Audio clock host factory returned null");
        physicalHost_ = std::move(replacement);
        hostClockSource_ = configuration.clockSource;
    }
    if (!software && !configuration.physicalDriverId) {
        next.state = "stopped";
        SetStatus(next);
        return {true, {}};
    }

    const auto selectedId = configuration.physicalDriverId.value_or("software");
    const std::wstring driverId(selectedId.begin(), selectedId.end());
    std::string error;
    PhysicalAsioCapabilities capabilities;
    if (!physicalHost_->open(driverId, hostWindow_, capabilities, error)) {
        next.state = "error";
        next.physicalDriverId = configuration.physicalDriverId;
        next.lastError = std::move(error);
        SetStatus(next);
        return {false, next.lastError};
    }

    const double requestedRate = configuration.sampleRate.value_or(
        static_cast<std::uint32_t>(capabilities.currentSampleRate));
    if (!std::isfinite(requestedRate) || requestedRate < 1.0 || std::floor(requestedRate) != requestedRate ||
        requestedRate > UINT32_MAX || !physicalHost_->setSampleRate(requestedRate, error) ||
        !physicalHost_->refreshCapabilities(capabilities, error) ||
        capabilities.currentSampleRate != requestedRate) {
        physicalHost_->close();
        next.state = "error";
        next.physicalDriverId = configuration.physicalDriverId;
        next.lastError = error.empty() ? "Physical driver did not confirm the requested sample rate" : std::move(error);
        SetStatus(next);
        return {false, next.lastError};
    }
    const auto sampleRate = static_cast<std::uint32_t>(requestedRate);
    const auto physicalBufferFrames = configuration.bufferFrames.value_or(
        capabilities.preferredBufferFrames > 0 ? static_cast<std::uint32_t>(capabilities.preferredBufferFrames) : 0u);
    if (!supportedBufferFrames(capabilities, physicalBufferFrames)) {
        physicalHost_->close();
        next.state = "error";
        next.physicalDriverId = configuration.physicalDriverId;
        next.lastError = "Requested buffer size is not supported by the selected physical driver at this rate";
        SetStatus(next);
        return {false, next.lastError};
    }

    if(!candidate)candidate=WasapiBridgeManager::Prepare(configuration.wasapiBridges,sampleRate,
        physicalBufferFrames,{},wasapiBackendFactory_,error,allowUnavailable,wasapiEndpointResolver_,unavailableBridgeIds);
    if(!candidate) {
        physicalHost_->close();next.state="error";next.lastError=error;SetStatus(next);return {false,error};
    }
    nativeManager_=retainedNative;
    const auto bindings=candidate->Bindings();
    const auto nativeBindings=NativeBindings();
    const auto nativeEpoch=nativeBindings.empty()?0:nativeBindings.front().generation->epoch;
    const auto activeRoutes=PersistentRoutes::ResolveForClock(configuration.routes,
        clients_.GetClientSnapshots(),configuration.physicalDriverId,bindings,nativeBindings);
    configureVasioClients(clients_, physicalBufferFrames, sampleRate);
    AudioRoutingLayout layout;
    layout.physicalDriverId = configuration.physicalDriverId.value_or("");
    layout.sampleRate = sampleRate;
    layout.bufferFrames = physicalBufferFrames;
    layout.physicalInputChannels = capabilities.inputChannels;
    layout.physicalOutputChannels = capabilities.outputChannels;
    layout.clients = clients_.GetClientSnapshots();
    const auto generationClients = layout.clients;
    layout.routes = activeRoutes;
    layout.wasapiBindings=bindings;layout.nativeBindings=nativeBindings;
    runtime_ = AudioRoutingRuntime::Create(std::move(layout), error);
    if (!runtime_) {
        physicalHost_->close();
        next.state = "error";
        next.physicalDriverId = configuration.physicalDriverId;
        next.lastError = std::move(error);
        SetStatus(next);
        return {false, next.lastError};
    }
    wasapiManager_=std::move(candidate);
    runtime_->SetStageMeasurementsEnabled(ReadPerformanceMeasurements().enabled);
    for (const auto& client : generationClients)
        if (client.mapping) client.mapping->SetStageMeasurementsEnabled(
            ReadPerformanceMeasurements().enabled);

    // Without an attached client the physical driver remains configured for
    // endpoint discovery. An attached client needs the clock-only callback to
    // exchange compact frames and receive its signal even without routes.
    // An enabled slave needs a live selected master even while its routes are
    // dormant, so WASAPI or native recovery can resume routed flow. Explicit
    // StopAudio destroys this manager and prevents retries until StartAudio.
    const bool enabledBridge=std::any_of(configuration.wasapiBridges.begin(),configuration.wasapiBridges.end(),
        [](const WasapiBridgeConfiguration& bridge){return bridge.enabled;});
    if (!software && activeRoutes.empty() && !hasAttachedClient(generationClients) &&
        !enabledBridge && !configuration.nativeAudio.enabled) {
        next.state = "stopped";
        next.physicalDriverId = configuration.physicalDriverId;
        next.sampleRate = sampleRate;
        next.bufferFrames = physicalBufferFrames;
        next.routes = activeRoutes;
        next.clockCapabilities = capabilities;
        next.physicalCapabilities = std::move(capabilities);
        SetStatus(next);
        return {true, {}};
    }

    auto reservedInputs=runtime_->RoutedPhysicalInputChannels();
    auto reservedOutputs=runtime_->RoutedPhysicalOutputChannels();
    reserveNativePhysicalChannels(configuration.routes,configuration.physicalDriverId,
        capabilities,reservedInputs,reservedOutputs);
    callbackSlot_.Publish(runtime_.get());
    nativePublishedEpoch_=nativeEpoch;
    if (!physicalHost_->start(physicalBufferFrames, reservedInputs,
                               reservedOutputs,
                               &RealtimeCallbackSlot::Dispatch, &callbackSlot_, error)) {
        StopPhysicalRuntime();
        next.state = "error";
        next.physicalDriverId = configuration.physicalDriverId;
        next.sampleRate = sampleRate;
        next.bufferFrames = physicalBufferFrames;
        next.lastError = std::move(error);
        SetStatus(next);
        return {false, next.lastError};
    }
    hostPhysicalInputs_ = std::move(reservedInputs);
    hostPhysicalOutputs_ = std::move(reservedOutputs);
    physicalHost_->setStageMeasurementsEnabled(ReadPerformanceMeasurements().enabled);
    acknowledgePublishedGenerations(generationClients, sampleRate, physicalBufferFrames);
    next.state = "running";
    next.physicalDriverId = configuration.physicalDriverId;
    next.sampleRate = sampleRate;
    next.bufferFrames = physicalBufferFrames;
    next.routes = activeRoutes;
    next.clockCapabilities = capabilities;
    if (!software) next.physicalCapabilities = std::move(capabilities);
    SetStatus(next);
    return {true, {}};
}
