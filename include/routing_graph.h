#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class RoutingEndpointType {
    PhysicalInput,
    PhysicalOutput,
    VirtualInput,
    VirtualOutput,
    WasapiCapture,
    WasapiRender,
    NativeSource,
    NativeDestination,
};

struct RoutingEndpoint {
    std::string id;
    RoutingEndpointType type;
    std::uint32_t sampleRate;
};

struct AudioRoute {
    std::string id;
    std::string sourceEndpointId;
    std::string destinationEndpointId;
    double gainDb;
    bool mute;
};

// Immutable after Create(); Process() performs no allocation or locking.
class RoutingGraph final {
public:
    static bool SupportsLink(RoutingEndpointType source, RoutingEndpointType destination) noexcept;
    static std::unique_ptr<RoutingGraph> Create(const std::vector<RoutingEndpoint>& endpoints,
        const std::vector<AudioRoute>& routes, std::uint32_t sampleRate, std::string& error);

    RoutingGraph(const RoutingGraph&) = delete;
    RoutingGraph& operator=(const RoutingGraph&) = delete;

    // Precleared physical outputs are supplied only by the physical ASIO callback.
    void Process(const float* const* sourceBlocks, float* const* destinationBlocks,
                 std::uint32_t frames, bool physicalOutputsPrecleared = false) const noexcept;
    std::size_t EndpointCount() const noexcept { return endpointCount_; }

private:
    struct CompiledRoute {
        std::size_t sourceIndex;
        std::size_t destinationIndex;
        float linearGain;
        bool mute;
    };

    explicit RoutingGraph(std::size_t endpointCount) : endpointCount_(endpointCount) {}
    std::size_t endpointCount_;
    std::vector<RoutingEndpointType> endpointTypes_;
    std::vector<CompiledRoute> routes_;
};
