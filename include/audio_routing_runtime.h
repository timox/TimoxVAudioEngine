#pragma once

#include "audio_meter.h"
#include "physical_asio_host.h"
#include "routing_graph.h"
#include "vasio_client_manager.h"
#include "wasapi_bridge_io.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Stable persisted identity: one or more ASCII letters, digits, '_' or '-'.
// Owners must retire runtimes off RT and keep backend workers quiescent before
// releasing their last generation reference. Process only borrows raw pointers.
struct WasapiRoutingBinding {
    std::string bridgeId;
    std::shared_ptr<WasapiBridgeIo> io;
    std::wstring resolvedEndpointId;
    bool endpointLoopback = false;
};

struct NativeAudioGeneration;
// Initial native device identity is exactly "windows". Route IDs never contain
// the process lifetime epoch or the driver's independently scoped generation.
// The control owner retains the backend until callbacks using this generation
// are quiescent. Runtime destruction and backend Stop/Join occur off RT.
struct NativeRoutingBinding {
    std::string id;
    std::shared_ptr<NativeAudioGeneration> generation;
};

struct AudioRoutingLayout {
    std::string physicalDriverId;
    std::uint32_t sampleRate = 0;
    std::uint32_t bufferFrames = 0;
    long physicalInputChannels = 0;
    long physicalOutputChannels = 0;
    std::vector<VasioClientSnapshot> clients;
    std::vector<AudioRoute> routes;
    std::vector<WasapiRoutingBinding> wasapiBindings;
    std::vector<NativeRoutingBinding> nativeBindings;
};

// All vectors and the immutable graph are built off the audio thread. Process()
// only reads/writes their fixed storage and the shared-memory rings.
class AudioRoutingRuntime final {
public:
    struct StageCounters {
        StageTimingCounters clientRead;
        StageTimingCounters routing;
    };
    static std::unique_ptr<AudioRoutingRuntime> Create(AudioRoutingLayout layout,
        std::string& error);
    AudioRoutingRuntime(const AudioRoutingRuntime&) = delete;
    AudioRoutingRuntime& operator=(const AudioRoutingRuntime&) = delete;

    void Process(const float* const* physicalInputs, float* const* physicalOutputs,
                 std::uint32_t frames, bool physicalOutputsPrecleared = false) noexcept;
    std::vector<AudioMeterSnapshot> ReadMeters();
    void SetStageMeasurementsEnabled(bool enabled) noexcept;
    StageCounters ConsumeStageCounters() noexcept;
    bool SetCorrelationPair(const std::string& leftEndpointId,
                            const std::string& rightEndpointId) noexcept;
    void StopCorrelation() noexcept;
    AudioCorrelationSnapshot ReadCorrelation() const noexcept;
    bool MatchesClientGenerations(const std::vector<VasioClientSnapshot>& clients) const noexcept;
    const std::vector<std::uint32_t>& RoutedPhysicalInputChannels() const noexcept {
        return routedPhysicalInputChannels_;
    }
    const std::vector<std::uint32_t>& RoutedPhysicalOutputChannels() const noexcept {
        return routedPhysicalOutputChannels_;
    }
    static void PhysicalCallback(const float* const* inputs, float* const* outputs,
        std::uint32_t frames, void* context) noexcept;

private:
    struct BridgeBuffers {
        std::shared_ptr<WasapiBridgeIo> owner;
        WasapiBridgeIo* io = nullptr;
        std::size_t endpointBase = 0;
        bool capture = false;
        std::vector<float*> capturePlanes;
        std::vector<const float*> renderPlanes;
    };
    struct ClientBuffers {
        VasioClientSnapshot client;
        std::uint32_t inputStride = 0;
        std::uint32_t outputStride = 0;
        std::vector<std::size_t> inputEndpointIndices;
        std::vector<std::size_t> outputEndpointIndices;
        std::vector<float> clientOutput;
        std::vector<float> clientInput;
    };
    struct MeterBinding {
        std::string endpointId;
        std::size_t endpointIndex = 0;
        AudioClientMapping* clientMapping = nullptr;
        bool clientOutput = false;
    };
    struct MeterFilterState {
        double shelfZ1 = 0.0, shelfZ2 = 0.0;
        double highPassZ1 = 0.0, highPassZ2 = 0.0;
        double bucketEnergy = 0.0;
        std::array<double, 30> energyBuckets{};
        double windowEnergy = 0.0;
        std::uint32_t bucketSamples = 0;
        std::uint32_t bucketCount = 0;
        std::uint32_t bucketWriteIndex = 0;
    };

    AudioRoutingRuntime(std::uint32_t frames, std::uint32_t sampleRate,
        std::size_t endpointCount,
        long inputChannels, long outputChannels);

    std::uint32_t bufferFrames_;
    std::uint32_t sampleRate_;
    std::size_t endpointCount_;
    long physicalInputChannels_;
    long physicalOutputChannels_;
    std::size_t physicalOutputBase_;
    std::vector<std::uint32_t> routedPhysicalInputChannels_;
    std::vector<std::uint32_t> routedPhysicalOutputChannels_;
    std::unique_ptr<RoutingGraph> graph_;
    std::vector<float> sourceSamples_;
    std::vector<float> destinationSamples_;
    std::vector<const float*> sourceBlocks_;
    std::vector<float*> destinationBlocks_;
    std::vector<ClientBuffers> clients_;
    std::vector<BridgeBuffers> bridges_;
    std::vector<std::shared_ptr<NativeAudioGeneration>> nativeGenerations_;
    std::atomic<bool> stageMeasurementsEnabled_{false};
    std::uint32_t stageSampleIndex_ = 0; // Physical callback thread only.
    StageTimingAccumulator clientReadTiming_;
    StageTimingAccumulator routingTiming_;
    std::unique_ptr<std::atomic<float>[]> peakLinear_;
    std::unique_ptr<std::atomic<float>[]> shortTermLufs_;
    std::unique_ptr<std::atomic<bool>[]> shortTermLufsReady_;
    std::vector<MeterFilterState> meterFilterStates_;
    std::array<double, 5> lufsShelf_{};
    std::array<double, 5> lufsHighPass_{};
    std::uint32_t lufsBucketFrames_ = 1;
    std::vector<MeterBinding> meterBindings_;
    std::atomic<std::size_t> correlationLeftIndex_{static_cast<std::size_t>(-1)};
    std::atomic<std::size_t> correlationRightIndex_{static_cast<std::size_t>(-1)};
    std::atomic<std::uint64_t> correlationGeneration_{0};
    std::atomic<std::uint64_t> correlationPublishedGeneration_{0};
    std::atomic<float> correlationValue_{0.0f};
    std::atomic<bool> correlationHasSignal_{false};
    std::uint64_t callbackCorrelationGeneration_ = 0;
    std::uint64_t correlationSamples_ = 0;
    double correlationSumLeft_ = 0.0;
    double correlationSumRight_ = 0.0;
    double correlationSumLeftSquared_ = 0.0;
    double correlationSumRightSquared_ = 0.0;
    double correlationSumProduct_ = 0.0;
};
