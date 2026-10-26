#include "audio_routing_runtime.h"
#include "native_audio_backend.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace {
std::array<double, 5> makeHighShelf(double sampleRate) {
    constexpr double pi = 3.14159265358979323846;
    constexpr double frequency = 1681.974450955533;
    constexpr double gainDb = 3.999843853973347;
    constexpr double q = 0.7071752369554196;
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w = 2.0 * pi * frequency / sampleRate;
    const double cosine = std::cos(w);
    const double alpha = std::sin(w) / (2.0 * q);
    const double sqrtA = std::sqrt(a);
    const double a0 = (a + 1.0) - (a - 1.0) * cosine + 2.0 * sqrtA * alpha;
    return {{
        a * ((a + 1.0) + (a - 1.0) * cosine + 2.0 * sqrtA * alpha) / a0,
        -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine) / a0,
        a * ((a + 1.0) + (a - 1.0) * cosine - 2.0 * sqrtA * alpha) / a0,
        2.0 * ((a - 1.0) - (a + 1.0) * cosine) / a0,
        ((a + 1.0) - (a - 1.0) * cosine - 2.0 * sqrtA * alpha) / a0
    }};
}

std::array<double, 5> makeLufsHighPass(double sampleRate) {
    constexpr double pi = 3.14159265358979323846;
    constexpr double frequency = 38.13547087602444;
    constexpr double q = 0.5003270373238773;
    const double w = 2.0 * pi * frequency / sampleRate;
    const double cosine = std::cos(w);
    const double alpha = std::sin(w) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    return {{
        ((1.0 + cosine) * 0.5) / a0,
        (-(1.0 + cosine)) / a0,
        ((1.0 + cosine) * 0.5) / a0,
        (-2.0 * cosine) / a0,
        (1.0 - alpha) / a0
    }};
}

double processBiquad(double input, const std::array<double, 5>& coefficients,
                     double& z1, double& z2) noexcept {
    const double output = coefficients[0] * input + z1;
    z1 = coefficients[1] * input - coefficients[3] * output + z2;
    z2 = coefficients[2] * input - coefficients[4] * output;
    return output;
}

std::string virtualPrefix(std::uint32_t driverId, std::uint32_t processId) {
    (void)driverId;
    return "virtual:TimoxVasio:" + std::to_string(processId) + ":";
}

bool physicalChannel(const std::string& endpoint, const std::string& prefix, std::uint32_t& channel) {
    if (endpoint.rfind(prefix, 0) != 0) return false;
    const auto text = endpoint.data() + prefix.size();
    const auto end = endpoint.data() + endpoint.size();
    std::uint32_t oneBased = 0;
    const auto result = std::from_chars(text, end, oneBased);
    if (result.ec != std::errc{} || result.ptr != end || oneBased == 0) return false;
    channel = oneBased - 1;
    return true;
}
}

AudioRoutingRuntime::AudioRoutingRuntime(std::uint32_t frames, std::uint32_t sampleRate,
    std::size_t endpointCount,
    long inputChannels, long outputChannels)
    : bufferFrames_(frames), sampleRate_(sampleRate), endpointCount_(endpointCount),
      physicalInputChannels_(inputChannels), physicalOutputChannels_(outputChannels),
      physicalOutputBase_(static_cast<std::size_t>(inputChannels)),
      sourceSamples_(endpointCount * frames, 0.0f),
      destinationSamples_(endpointCount * frames, 0.0f),
      sourceBlocks_(endpointCount, nullptr), destinationBlocks_(endpointCount, nullptr),
      peakLinear_(std::make_unique<std::atomic<float>[]>(endpointCount)),
      shortTermLufs_(std::make_unique<std::atomic<float>[]>(endpointCount)),
      shortTermLufsReady_(std::make_unique<std::atomic<bool>[]>(endpointCount)),
      meterFilterStates_(endpointCount),
      lufsBucketFrames_((std::max)(1u, sampleRate / 10u)) {
    static_assert(std::atomic<float>::is_always_lock_free,
        "Realtime peak meters require lock-free float atomics");
    lufsShelf_ = makeHighShelf(static_cast<double>(sampleRate_));
    lufsHighPass_ = makeLufsHighPass(static_cast<double>(sampleRate_));
    for (std::size_t index = 0; index < endpointCount_; ++index) {
        peakLinear_[index].store(0.0f, std::memory_order_relaxed);
        shortTermLufs_[index].store(-120.0f, std::memory_order_relaxed);
        shortTermLufsReady_[index].store(false, std::memory_order_relaxed);
    }
}

std::unique_ptr<AudioRoutingRuntime> AudioRoutingRuntime::Create(AudioRoutingLayout layout,
    std::string& error) {
    error.clear();
    if (layout.sampleRate == 0 || layout.bufferFrames == 0 ||
        layout.physicalInputChannels < 0 || layout.physicalOutputChannels < 0) {
        error = "Audio runtime requires positive rate and buffer size and non-negative channel counts";
        return {};
    }

    std::vector<RoutingEndpoint> endpoints;
    endpoints.reserve(static_cast<std::size_t>(layout.physicalInputChannels) +
        static_cast<std::size_t>(layout.physicalOutputChannels));
    for (long channel = 0; channel < layout.physicalInputChannels; ++channel) {
        endpoints.push_back({"physical:" + layout.physicalDriverId + ":input:" +
            std::to_string(channel + 1), RoutingEndpointType::PhysicalInput, layout.sampleRate});
    }
    for (long channel = 0; channel < layout.physicalOutputChannels; ++channel) {
        endpoints.push_back({"physical:" + layout.physicalDriverId + ":output:" +
            std::to_string(channel + 1), RoutingEndpointType::PhysicalOutput, layout.sampleRate});
    }

    struct ClientIndices {
        std::array<std::size_t, AudioClientMapping::kChannelCount> inputs;
        std::array<std::size_t, AudioClientMapping::kChannelCount> outputs;
    };
    std::vector<ClientIndices> clientIndices;
    clientIndices.reserve(layout.clients.size());
    std::vector<VasioClientSnapshot> activeClients;
    activeClients.reserve(layout.clients.size());
    for (auto& client : layout.clients) {
        if (client.info.driverId != 1 || !client.mapping) continue;
        if (client.mapping->SampleRate() != layout.sampleRate) {
            error = "VASIO client sample rate differs from the selected physical clock";
            return {};
        }
        if (!client.mapping->EngineAttached()) {
            error = "VASIO client is no longer attached to this engine";
            return {};
        }
        AudioClientMapping::ChannelMask activeInputs{};
        AudioClientMapping::ChannelMask activeOutputs{};
        if (!client.mapping->GetActiveChannels(activeInputs, activeOutputs)) {
            error = "Unable to read a consistent TimoxVasio channel snapshot";
            return {};
        }
        ClientIndices indices{};
        constexpr auto inactiveEndpoint = static_cast<std::size_t>(-1);
        indices.inputs.fill(inactiveEndpoint);
        indices.outputs.fill(inactiveEndpoint);
        const auto prefix = virtualPrefix(client.info.driverId, client.info.processId);
        for (std::uint32_t channel = 0; channel < AudioClientMapping::kChannelCount; ++channel) {
            const auto apiChannel = channel + 1;
            const auto active = std::uint64_t{1} << (channel % 64);
            if (activeInputs[channel / 64] & active) {
                indices.inputs[channel] = endpoints.size();
                endpoints.push_back({prefix + "input:" + std::to_string(apiChannel),
                    RoutingEndpointType::VirtualInput, layout.sampleRate});
            }
            if (activeOutputs[channel / 64] & active) {
                indices.outputs[channel] = endpoints.size();
                endpoints.push_back({prefix + "output:" + std::to_string(apiChannel),
                    RoutingEndpointType::VirtualOutput, layout.sampleRate});
            }
        }
        clientIndices.push_back(std::move(indices));
        activeClients.push_back(std::move(client));
    }

    std::unordered_set<std::string> bridgeIds;
    std::unordered_set<const WasapiBridgeIo*> bridgePointers;
    std::unordered_map<std::string, const WasapiRoutingBinding*> bridgeEndpoints;
    std::vector<std::size_t> bridgeBases;
    for (const auto& binding : layout.wasapiBindings) {
        const bool validId = !binding.bridgeId.empty() &&
            std::all_of(binding.bridgeId.begin(), binding.bridgeId.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
            });
        if (!validId || !binding.io || binding.resolvedEndpointId.empty() ||
            !bridgeIds.insert(binding.bridgeId).second || !bridgePointers.insert(binding.io.get()).second) {
            error = "WASAPI bindings require unique stable IDs, unique IO generations and resolved endpoints";
            return {};
        }
        const auto& format = binding.io->Format();
        const bool capture = format.direction == WasapiBridgeDirection::Capture;
        if (format.graphSampleRate != layout.sampleRate || format.maxQuantum < layout.bufferFrames ||
            !format.channels || (binding.endpointLoopback && !capture)) {
            error = "WASAPI generation format or loopback direction does not match the graph";
            return {};
        }
        bridgeBases.push_back(endpoints.size());
        for (std::size_t channel = 0; channel < format.channels; ++channel) {
            const auto id = "wasapi:" + binding.bridgeId + (capture ? ":input:" : ":output:") +
                std::to_string(channel + 1);
            endpoints.push_back({id, capture ? RoutingEndpointType::WasapiCapture : RoutingEndpointType::WasapiRender,
                layout.sampleRate});
            bridgeEndpoints.emplace(id, &binding);
        }
    }
    for (const auto& route : layout.routes) {
        const auto source = bridgeEndpoints.find(route.sourceEndpointId);
        const auto destination = bridgeEndpoints.find(route.destinationEndpointId);
        if (source != bridgeEndpoints.end() && destination != bridgeEndpoints.end() &&
            source->second->endpointLoopback &&
            destination->second->io->Format().direction == WasapiBridgeDirection::Render &&
            source->second->resolvedEndpointId == destination->second->resolvedEndpointId) {
            error = "Direct endpoint loopback to its own render device is not supported";
            return {};
        }
    }
    std::vector<std::size_t> nativeBases;
    std::unordered_set<std::string> nativeIds;
    for (const auto& binding : layout.nativeBindings) {
        const auto& generation = binding.generation;
        if (binding.id != "windows" || !nativeIds.insert(binding.id).second || !generation ||
            !generation->epoch || !generation->wireGeneration) {
            error = "Native bindings require the unique windows identity and a live generation";
            return {};
        }
        nativeBases.push_back(endpoints.size());
        for (const bool source : {true, false}) {
            const auto& io = source ? generation->windowsRenderSource : generation->windowsCaptureDestination;
            if (!io || !bridgePointers.insert(io.get()).second) {
                error = "Native IO must be present and unique across all native and WASAPI bindings";
                return {};
            }
            const auto& format = io->Format();
            if (format.generationId != generation->epoch || format.channels != 2 ||
                format.graphSampleRate != layout.sampleRate || format.maxQuantum < layout.bufferFrames ||
                format.direction != (source ? WasapiBridgeDirection::Capture : WasapiBridgeDirection::Render)) {
                error = "Native IO epoch, stereo format, rate, quantum or direction does not match the graph";
                return {};
            }
            for (unsigned channel = 1; channel <= 2; ++channel)
                endpoints.push_back({"native:" + binding.id + (source ? ":input:" : ":output:") +
                    std::to_string(channel), source ? RoutingEndpointType::NativeSource : RoutingEndpointType::NativeDestination,
                    layout.sampleRate});
        }
        if (generation->windowsRenderSource->Format().maxQuantum !=
            generation->windowsCaptureDestination->Format().maxQuantum) {
            error = "Native duplex IO must share the same maximum quantum";
            return {};
        }
    }
    // Native metadata cannot identify feedback through Windows/WASAPI. Preserve
    // the explicit WASAPI loopback guard above; do not claim cross-backend loop detection.
    const auto actualEndpointCount = endpoints.size();
    if (actualEndpointCount > (std::numeric_limits<std::size_t>::max)() / layout.bufferFrames / sizeof(float)) {
        error = "Audio runtime sample storage dimensions overflow";
        return {};
    }
    auto runtime = std::unique_ptr<AudioRoutingRuntime>(new AudioRoutingRuntime(layout.bufferFrames,
        layout.sampleRate,
        actualEndpointCount, layout.physicalInputChannels, layout.physicalOutputChannels));
    std::string graphError;
    runtime->graph_ = RoutingGraph::Create(endpoints, layout.routes, layout.sampleRate, graphError);
    if (!runtime->graph_) {
        error = std::move(graphError);
        return {};
    }
    runtime->bridges_.reserve(layout.wasapiBindings.size() + layout.nativeBindings.size() * 2);
    for (std::size_t index = 0; index < layout.wasapiBindings.size(); ++index) {
        BridgeBuffers buffers;
        buffers.owner = std::move(layout.wasapiBindings[index].io);
        buffers.io = buffers.owner.get();
        buffers.endpointBase = bridgeBases[index];
        buffers.capture = buffers.io->Format().direction == WasapiBridgeDirection::Capture;
        for (std::size_t channel = 0; channel < buffers.io->Format().channels; ++channel) {
            const auto offset = (buffers.endpointBase + channel) * layout.bufferFrames;
            if (buffers.capture) buffers.capturePlanes.push_back(runtime->sourceSamples_.data() + offset);
            else buffers.renderPlanes.push_back(runtime->destinationSamples_.data() + offset);
        }
        runtime->bridges_.push_back(std::move(buffers));
    }
    runtime->nativeGenerations_.reserve(layout.nativeBindings.size());
    for (std::size_t index = 0; index < layout.nativeBindings.size(); ++index) {
        auto generation = std::move(layout.nativeBindings[index].generation);
        for (const bool source : {true, false}) {
            BridgeBuffers buffers;
            buffers.owner = source ? generation->windowsRenderSource : generation->windowsCaptureDestination;
            buffers.io = buffers.owner.get();
            buffers.endpointBase = nativeBases[index] + (source ? 0 : 2);
            buffers.capture = source;
            for (unsigned channel = 0; channel < 2; ++channel) {
                const auto offset = (buffers.endpointBase + channel) * layout.bufferFrames;
                if (source) buffers.capturePlanes.push_back(runtime->sourceSamples_.data() + offset);
                else buffers.renderPlanes.push_back(runtime->destinationSamples_.data() + offset);
            }
            runtime->bridges_.push_back(std::move(buffers));
        }
        runtime->nativeGenerations_.push_back(std::move(generation));
    }
    std::set<std::uint32_t> routedInputs;
    std::set<std::uint32_t> routedOutputs;
    const auto physicalPrefix = "physical:" + layout.physicalDriverId + ":";
    for (const auto& route : layout.routes) {
        std::uint32_t channel = 0;
        const bool input = physicalChannel(route.sourceEndpointId, physicalPrefix + "input:", channel);
        if (input)
            routedInputs.insert(channel);
        const bool output = physicalChannel(route.destinationEndpointId, physicalPrefix + "output:", channel);
        if (output)
            routedOutputs.insert(channel);
    }
    runtime->routedPhysicalInputChannels_.assign(routedInputs.begin(), routedInputs.end());
    runtime->routedPhysicalOutputChannels_.assign(routedOutputs.begin(), routedOutputs.end());
    runtime->clients_.reserve(activeClients.size());
    for (std::size_t index = 0; index < activeClients.size(); ++index) {
        ClientBuffers buffers;
        buffers.client = std::move(activeClients[index]);
        const auto& mapping = *buffers.client.mapping;
        buffers.inputStride = mapping.InputStride();
        buffers.outputStride = mapping.OutputStride();
        buffers.inputEndpointIndices.reserve(buffers.inputStride);
        buffers.outputEndpointIndices.reserve(buffers.outputStride);
        for (std::uint32_t slot = 0; slot < buffers.inputStride; ++slot) {
            const int channel = mapping.InputChannelAt(slot);
            if (channel < 0 || channel >= static_cast<int>(AudioClientMapping::kChannelCount)) {
                error = "Invalid compact input channel in TimoxVasio transport";
                return {};
            }
            buffers.inputEndpointIndices.push_back(clientIndices[index].inputs[static_cast<std::size_t>(channel)]);
        }
        for (std::uint32_t slot = 0; slot < buffers.outputStride; ++slot) {
            const int channel = mapping.OutputChannelAt(slot);
            if (channel < 0 || channel >= static_cast<int>(AudioClientMapping::kChannelCount)) {
                error = "Invalid compact output channel in TimoxVasio transport";
                return {};
            }
            buffers.outputEndpointIndices.push_back(clientIndices[index].outputs[static_cast<std::size_t>(channel)]);
        }
        buffers.clientOutput.resize(static_cast<std::size_t>(buffers.outputStride) * layout.bufferFrames);
        buffers.clientInput.resize(static_cast<std::size_t>(buffers.inputStride) * layout.bufferFrames);
        runtime->clients_.push_back(std::move(buffers));
    }

    for (std::size_t endpointIndex = 0; endpointIndex < endpoints.size(); ++endpointIndex) {
        const auto& endpoint = endpoints[endpointIndex];
        const bool usedByRoute = std::any_of(layout.routes.begin(), layout.routes.end(),
            [&endpoint](const AudioRoute& route) {
                return route.sourceEndpointId == endpoint.id ||
                    route.destinationEndpointId == endpoint.id;
            });
        if (!usedByRoute) continue;

        MeterBinding binding;
        binding.endpointId = endpoint.id;
        binding.endpointIndex = endpointIndex;
        binding.clientOutput = endpoint.type == RoutingEndpointType::VirtualOutput;
        if (endpoint.type == RoutingEndpointType::VirtualInput || binding.clientOutput) {
            for (auto& client : runtime->clients_) {
                const auto prefix = virtualPrefix(client.client.info.driverId,
                    client.client.info.processId);
                if (endpoint.id.rfind(prefix, 0) == 0) {
                    binding.clientMapping = client.client.mapping.get();
                    break;
                }
            }
        }
        runtime->meterBindings_.push_back(std::move(binding));
    }
    return runtime;
}

std::vector<AudioMeterSnapshot> AudioRoutingRuntime::ReadMeters() {
    std::vector<AudioMeterSnapshot> result;
    result.reserve(meterBindings_.size());
    for (const auto& binding : meterBindings_) {
        AudioMeterSnapshot meter;
        meter.endpointId = binding.endpointId;
        meter.peakLinear = peakLinear_[binding.endpointIndex].load(std::memory_order_acquire);
        meter.shortTermLufs = shortTermLufs_[binding.endpointIndex].load(std::memory_order_acquire);
        meter.shortTermLufsReady = shortTermLufsReady_[binding.endpointIndex].load(std::memory_order_acquire);
        if (binding.clientMapping) {
            if (binding.clientOutput) {
                meter.underruns = binding.clientMapping->ClientOutputUnderruns();
                meter.overruns = binding.clientMapping->ClientOutputOverruns();
            } else {
                meter.underruns = binding.clientMapping->ClientInputUnderruns();
                meter.overruns = binding.clientMapping->ClientInputOverruns();
            }
        }
        result.push_back(std::move(meter));
    }
    return result;
}

bool AudioRoutingRuntime::SetCorrelationPair(const std::string& leftEndpointId,
                                             const std::string& rightEndpointId) noexcept {
    std::size_t left = static_cast<std::size_t>(-1);
    std::size_t right = static_cast<std::size_t>(-1);
    for (const auto& binding : meterBindings_) {
        if (!binding.clientOutput) continue;
        if (binding.endpointId == leftEndpointId) left = binding.endpointIndex;
        if (binding.endpointId == rightEndpointId) right = binding.endpointIndex;
    }
    if (left == static_cast<std::size_t>(-1) || right == static_cast<std::size_t>(-1) || left == right)
        return false;

    correlationGeneration_.fetch_add(1, std::memory_order_acq_rel);
    correlationLeftIndex_.store(left, std::memory_order_relaxed);
    correlationRightIndex_.store(right, std::memory_order_relaxed);
    correlationGeneration_.fetch_add(1, std::memory_order_release);
    return true;
}

void AudioRoutingRuntime::StopCorrelation() noexcept {
    correlationGeneration_.fetch_add(1, std::memory_order_acq_rel);
    correlationLeftIndex_.store(static_cast<std::size_t>(-1), std::memory_order_relaxed);
    correlationRightIndex_.store(static_cast<std::size_t>(-1), std::memory_order_relaxed);
    correlationGeneration_.fetch_add(1, std::memory_order_release);
}

AudioCorrelationSnapshot AudioRoutingRuntime::ReadCorrelation() const noexcept {
    const auto generation = correlationGeneration_.load(std::memory_order_acquire);
    if ((generation & 1u) || generation == 0 ||
        correlationLeftIndex_.load(std::memory_order_relaxed) == static_cast<std::size_t>(-1) ||
        correlationRightIndex_.load(std::memory_order_relaxed) == static_cast<std::size_t>(-1) ||
        correlationPublishedGeneration_.load(std::memory_order_acquire) != generation)
        return {};
    return {true, correlationHasSignal_.load(std::memory_order_relaxed),
        correlationValue_.load(std::memory_order_relaxed)};
}

void AudioRoutingRuntime::Process(const float* const* physicalInputs, float* const* physicalOutputs,
                                  std::uint32_t frames, bool physicalOutputsPrecleared) noexcept {
    if (frames != bufferFrames_ || !graph_) {
        if (physicalOutputs) {
            for (long channel = 0; channel < physicalOutputChannels_; ++channel)
                if (physicalOutputs[channel]) std::fill_n(physicalOutputs[channel], frames, 0.0f);
        }
        return;
    }

    const bool sampleStages = stageMeasurementsEnabled_.load(std::memory_order_relaxed) &&
        (++stageSampleIndex_ % 16u == 0);
    LARGE_INTEGER readStart{};
    if (sampleStages) QueryPerformanceCounter(&readStart);
    bool observedClient = false;

    std::fill(sourceBlocks_.begin(), sourceBlocks_.end(), nullptr);
    std::fill(destinationBlocks_.begin(), destinationBlocks_.end(), nullptr);
    for (long channel = 0; channel < physicalInputChannels_; ++channel)
        sourceBlocks_[static_cast<std::size_t>(channel)] = physicalInputs ? physicalInputs[channel] : nullptr;
    for (long channel = 0; channel < physicalOutputChannels_; ++channel)
        destinationBlocks_[physicalOutputBase_ + static_cast<std::size_t>(channel)] =
            physicalOutputs ? physicalOutputs[channel] : nullptr;

    for (auto& client : clients_) {
        auto& mapping = *client.client.mapping;
        const bool attached = mapping.EngineAttached();
        const bool currentGeneration = mapping.AudioEpoch() &&
            mapping.AudioEpoch() == mapping.PublishedAudioEpoch();
        if (attached && currentGeneration)
        {
            observedClient = true;
            mapping.ReadClientOutput(client.clientOutput.data(), frames);
        }
        else std::fill(client.clientOutput.begin(), client.clientOutput.end(), 0.0f);
        for (std::uint32_t slot = 0; slot < client.outputStride; ++slot) {
            const auto sourceIndex = client.outputEndpointIndices[slot];
            if (sourceIndex != static_cast<std::size_t>(-1)) {
                auto* plane = sourceSamples_.data() + sourceIndex * bufferFrames_;
                for (std::uint32_t frame = 0; frame < frames; ++frame)
                    plane[frame] = client.clientOutput[static_cast<std::size_t>(frame) *
                        client.outputStride + slot];
                sourceBlocks_[sourceIndex] = plane;
            }
        }
        for (std::uint32_t slot = 0; slot < client.inputStride; ++slot) {
            const auto destinationIndex = client.inputEndpointIndices[slot];
            if (destinationIndex == static_cast<std::size_t>(-1)) continue;
            auto* destination = destinationSamples_.data() + destinationIndex * bufferFrames_;
            destinationBlocks_[destinationIndex] = destination;
        }
    }

    LARGE_INTEGER readEnd{};
    if (sampleStages) QueryPerformanceCounter(&readEnd);
    if (sampleStages && observedClient && readEnd.QuadPart >= readStart.QuadPart)
        clientReadTiming_.Add(static_cast<std::uint64_t>(readEnd.QuadPart - readStart.QuadPart));

    for (auto& bridge : bridges_) {
        if (bridge.capture) {
            bridge.io->ReadCaptureBlock(bridge.capturePlanes.data(), frames, bridge.capturePlanes.size());
            for (std::size_t channel = 0; channel < bridge.capturePlanes.size(); ++channel)
                sourceBlocks_[bridge.endpointBase + channel] = bridge.capturePlanes[channel];
        } else {
            for (std::size_t channel = 0; channel < bridge.renderPlanes.size(); ++channel)
                destinationBlocks_[bridge.endpointBase + channel] =
                    destinationSamples_.data() + (bridge.endpointBase + channel) * bufferFrames_;
        }
    }
    // Preserve the existing client-read and graph-only measurements. Bridge IO
    // does not get attributed to either stage; separate bridge timing is future work.
    LARGE_INTEGER routingStart = readEnd;
    if (sampleStages && !bridges_.empty()) QueryPerformanceCounter(&routingStart);

    graph_->Process(sourceBlocks_.data(), destinationBlocks_.data(), frames,
        physicalOutputsPrecleared);
    if (sampleStages) {
        LARGE_INTEGER routingEnd{};
        QueryPerformanceCounter(&routingEnd);
        if ((observedClient || !bridges_.empty()) && routingEnd.QuadPart >= routingStart.QuadPart)
            routingTiming_.Add(static_cast<std::uint64_t>(routingEnd.QuadPart - routingStart.QuadPart));
    }
    for (auto& bridge : bridges_)
        if (!bridge.capture)
            bridge.io->WriteRenderBlock(bridge.renderPlanes.data(), frames, bridge.renderPlanes.size());

    const auto correlationGeneration = correlationGeneration_.load(std::memory_order_acquire);
    if ((correlationGeneration & 1u) != 0) {
        correlationSamples_ = 0;
        correlationSumLeft_ = correlationSumRight_ = 0.0;
        correlationSumLeftSquared_ = correlationSumRightSquared_ = correlationSumProduct_ = 0.0;
    } else if (correlationGeneration != callbackCorrelationGeneration_) {
        callbackCorrelationGeneration_ = correlationGeneration;
        correlationSamples_ = 0;
        correlationSumLeft_ = correlationSumRight_ = 0.0;
        correlationSumLeftSquared_ = correlationSumRightSquared_ = correlationSumProduct_ = 0.0;
    }
    const auto leftIndex = correlationLeftIndex_.load(std::memory_order_relaxed);
    const auto rightIndex = correlationRightIndex_.load(std::memory_order_relaxed);
    const auto confirmedCorrelationGeneration = correlationGeneration_.load(std::memory_order_acquire);
    if (confirmedCorrelationGeneration != correlationGeneration) {
        correlationSamples_ = 0;
        correlationSumLeft_ = correlationSumRight_ = 0.0;
        correlationSumLeftSquared_ = correlationSumRightSquared_ = correlationSumProduct_ = 0.0;
    }
    if (confirmedCorrelationGeneration == correlationGeneration &&
        correlationGeneration != 0 && (correlationGeneration & 1u) == 0 &&
        leftIndex != static_cast<std::size_t>(-1) && rightIndex != static_cast<std::size_t>(-1) &&
        leftIndex < endpointCount_ && rightIndex < endpointCount_) {
        const float* leftSamples = sourceBlocks_[leftIndex]
            ? sourceBlocks_[leftIndex] : destinationBlocks_[leftIndex];
        const float* rightSamples = sourceBlocks_[rightIndex]
            ? sourceBlocks_[rightIndex] : destinationBlocks_[rightIndex];
        if (leftSamples && rightSamples) {
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                const double left = std::isfinite(leftSamples[frame]) ? leftSamples[frame] : 0.0;
                const double right = std::isfinite(rightSamples[frame]) ? rightSamples[frame] : 0.0;
                correlationSumLeft_ += left;
                correlationSumRight_ += right;
                correlationSumLeftSquared_ += left * left;
                correlationSumRightSquared_ += right * right;
                correlationSumProduct_ += left * right;
            }
            correlationSamples_ += frames;
            const auto windowSamples = (std::max)(std::uint64_t{1},
                static_cast<std::uint64_t>(sampleRate_) / 10);
            if (correlationSamples_ >= windowSamples) {
                const double count = static_cast<double>(correlationSamples_);
                const double covariance = correlationSumProduct_ -
                    correlationSumLeft_ * correlationSumRight_ / count;
                const double leftEnergy = correlationSumLeftSquared_ -
                    correlationSumLeft_ * correlationSumLeft_ / count;
                const double rightEnergy = correlationSumRightSquared_ -
                    correlationSumRight_ * correlationSumRight_ / count;
                constexpr double minimumRms = 0.00003162277660168379; // -90 dBFS
                const bool hasSignal = leftEnergy > 0.0 && rightEnergy > 0.0 &&
                    std::sqrt(leftEnergy / count) >= minimumRms &&
                    std::sqrt(rightEnergy / count) >= minimumRms;
                const double denominator = hasSignal ? std::sqrt(leftEnergy * rightEnergy) : 1.0;
                const double coefficient = hasSignal ? covariance / denominator : 0.0;
                correlationValue_.store(static_cast<float>(std::clamp(coefficient, -1.0, 1.0)),
                    std::memory_order_relaxed);
                correlationHasSignal_.store(hasSignal, std::memory_order_relaxed);
                correlationPublishedGeneration_.store(correlationGeneration, std::memory_order_release);
                correlationSamples_ = 0;
                correlationSumLeft_ = correlationSumRight_ = 0.0;
                correlationSumLeftSquared_ = correlationSumRightSquared_ = correlationSumProduct_ = 0.0;
            }
        } else {
            correlationSamples_ = 0;
            correlationSumLeft_ = correlationSumRight_ = 0.0;
            correlationSumLeftSquared_ = correlationSumRightSquared_ = correlationSumProduct_ = 0.0;
            correlationHasSignal_.store(false, std::memory_order_relaxed);
            correlationPublishedGeneration_.store(correlationGeneration, std::memory_order_release);
        }
    }

    for (const auto& binding : meterBindings_) {
        const auto index = binding.endpointIndex;
        const float* samples = sourceBlocks_[index]
            ? sourceBlocks_[index] : destinationBlocks_[index];
        auto& filter = meterFilterStates_[index];
        if (!samples) {
            peakLinear_[index].store(0.0f, std::memory_order_release);
            filter = {};
            shortTermLufs_[index].store(-120.0f, std::memory_order_relaxed);
            shortTermLufsReady_[index].store(false, std::memory_order_release);
            continue;
        }
        float peak = 0.0f;
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const float rawSample = samples[frame];
            const double sample = std::isfinite(rawSample) ? rawSample : 0.0;
            peak = (std::max)(peak, static_cast<float>(std::abs(sample)));
            const double shelfSample = processBiquad(sample, lufsShelf_,
                filter.shelfZ1, filter.shelfZ2);
            const double weightedSample = processBiquad(shelfSample, lufsHighPass_,
                filter.highPassZ1, filter.highPassZ2);
            filter.bucketEnergy += weightedSample * weightedSample;
            if (++filter.bucketSamples >= lufsBucketFrames_) {
                const double bucketMeanEnergy = filter.bucketEnergy /
                    static_cast<double>(filter.bucketSamples);
                filter.windowEnergy -= filter.energyBuckets[filter.bucketWriteIndex];
                filter.energyBuckets[filter.bucketWriteIndex] = bucketMeanEnergy;
                filter.windowEnergy += bucketMeanEnergy;
                filter.bucketWriteIndex = (filter.bucketWriteIndex + 1) %
                    static_cast<std::uint32_t>(filter.energyBuckets.size());
                filter.bucketCount = (std::min)(filter.bucketCount + 1,
                    static_cast<std::uint32_t>(filter.energyBuckets.size()));
                filter.bucketEnergy = 0.0;
                filter.bucketSamples = 0;
                if (filter.bucketCount == filter.energyBuckets.size()) {
                    const double meanEnergy = filter.windowEnergy /
                        static_cast<double>(filter.bucketCount);
                    const float lufs = meanEnergy > 0.0
                        ? static_cast<float>(-0.691 + 10.0 * std::log10(meanEnergy))
                        : -120.0f;
                    shortTermLufs_[index].store((std::max)(-120.0f, lufs),
                        std::memory_order_relaxed);
                    shortTermLufsReady_[index].store(true, std::memory_order_release);
                }
            }
        }
        constexpr double denormalFloor = 1.0e-30;
        if (std::abs(filter.shelfZ1) < denormalFloor) filter.shelfZ1 = 0.0;
        if (std::abs(filter.shelfZ2) < denormalFloor) filter.shelfZ2 = 0.0;
        if (std::abs(filter.highPassZ1) < denormalFloor) filter.highPassZ1 = 0.0;
        if (std::abs(filter.highPassZ2) < denormalFloor) filter.highPassZ2 = 0.0;
        peak = (std::min)(peak, 1.0f);
        peakLinear_[index].store(peak, std::memory_order_release);
    }

    for (auto& client : clients_) {
        auto& mapping = *client.client.mapping;
        if (!mapping.EngineAttached()) continue;
        const bool currentGeneration = mapping.AudioEpoch() &&
            mapping.AudioEpoch() == mapping.PublishedAudioEpoch();
        std::fill(client.clientInput.begin(), client.clientInput.end(), 0.0f);
        for (std::uint32_t slot = 0; slot < client.inputStride; ++slot) {
            const auto sourceIndex = client.inputEndpointIndices[slot];
            if (sourceIndex == static_cast<std::size_t>(-1)) continue;
            const auto* source = destinationSamples_.data() + sourceIndex * bufferFrames_;
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                client.clientInput[static_cast<std::size_t>(frame) *
                    client.inputStride + slot] =
                    source[frame];
            }
        }
        if (currentGeneration) mapping.WriteEngineInput(client.clientInput.data(), frames);
        mapping.SignalClient();
    }
}

void AudioRoutingRuntime::SetStageMeasurementsEnabled(bool enabled) noexcept {
    stageMeasurementsEnabled_.store(enabled, std::memory_order_release);
    for (auto& client : clients_)
        if (client.client.mapping) client.client.mapping->SetStageMeasurementsEnabled(enabled);
    if (!enabled) ConsumeStageCounters();
}

AudioRoutingRuntime::StageCounters AudioRoutingRuntime::ConsumeStageCounters() noexcept {
    return {clientReadTiming_.Consume(), routingTiming_.Consume()};
}

bool AudioRoutingRuntime::MatchesClientGenerations(
    const std::vector<VasioClientSnapshot>& clients) const noexcept {
    std::size_t eligibleCount = 0;
    for (const auto& client : clients) {
        if (client.info.driverId != 1 || !client.mapping) continue;
        ++eligibleCount;
        const auto existing = std::find_if(clients_.begin(), clients_.end(),
            [&client](const ClientBuffers& current) {
                return current.client.info.driverId == client.info.driverId &&
                    current.client.info.processId == client.info.processId;
            });
        if (existing == clients_.end() ||
            existing->client.mapping.get() != client.mapping.get() ||
            existing->client.mapping->AudioEpoch() != client.mapping->AudioEpoch())
            return false;
    }
    return eligibleCount == clients_.size();
}

void AudioRoutingRuntime::PhysicalCallback(const float* const* inputs, float* const* outputs,
    std::uint32_t frames, void* context) noexcept {
    // PhysicalAsioHost cleared every selected physical output before dispatch.
    if (context) static_cast<AudioRoutingRuntime*>(context)->Process(inputs, outputs, frames, true);
}
