#include "routing_graph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
#include <emmintrin.h>
#define TIMOX_ROUTING_SSE2 1
#endif

bool RoutingGraph::SupportsLink(RoutingEndpointType source, RoutingEndpointType destination) noexcept {
    if (source == RoutingEndpointType::NativeSource)
        return destination == RoutingEndpointType::VirtualInput || destination == RoutingEndpointType::PhysicalOutput ||
            destination == RoutingEndpointType::WasapiRender || destination == RoutingEndpointType::NativeDestination;
    if (destination == RoutingEndpointType::NativeDestination)
        return source == RoutingEndpointType::VirtualOutput || source == RoutingEndpointType::PhysicalInput ||
            source == RoutingEndpointType::WasapiCapture;
    if (source == RoutingEndpointType::WasapiCapture)
        return destination == RoutingEndpointType::VirtualInput ||
            destination == RoutingEndpointType::PhysicalOutput || destination == RoutingEndpointType::WasapiRender;
    if (destination == RoutingEndpointType::WasapiRender)
        return source == RoutingEndpointType::VirtualOutput || source == RoutingEndpointType::PhysicalInput;
    return (source == RoutingEndpointType::VirtualOutput && destination == RoutingEndpointType::VirtualInput) ||
           (source == RoutingEndpointType::VirtualOutput && destination == RoutingEndpointType::PhysicalOutput) ||
           (source == RoutingEndpointType::PhysicalInput && destination == RoutingEndpointType::VirtualInput);
}

std::unique_ptr<RoutingGraph> RoutingGraph::Create(const std::vector<RoutingEndpoint>& endpoints,
    const std::vector<AudioRoute>& routes, std::uint32_t sampleRate, std::string& error) {
    error.clear();
    if (sampleRate == 0) {
        error = "Routing sample rate must be positive";
        return {};
    }

    std::unordered_map<std::string, std::size_t> indices;
    indices.reserve(endpoints.size());
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        const auto& endpoint = endpoints[index];
        if (endpoint.id.empty() || endpoint.sampleRate != sampleRate ||
            !indices.emplace(endpoint.id, index).second) {
            error = "Endpoint IDs must be unique and all endpoint rates must match the graph";
            return {};
        }
    }

    auto graph = std::unique_ptr<RoutingGraph>(new RoutingGraph(endpoints.size()));
    graph->endpointTypes_.reserve(endpoints.size());
    for (const auto& endpoint : endpoints) graph->endpointTypes_.push_back(endpoint.type);
    graph->routes_.reserve(routes.size());
    std::unordered_set<std::string> routeIds;
    routeIds.reserve(routes.size());
    for (const auto& route : routes) {
        if (route.id.empty() || !routeIds.emplace(route.id).second) {
            error = "Route IDs must be non-empty and unique";
            return {};
        }
        if (!std::isfinite(route.gainDb) || route.gainDb < -120.0 || route.gainDb > 24.0) {
            error = "Route gain must be finite and between -120 and +24 dB";
            return {};
        }
        const auto source = indices.find(route.sourceEndpointId);
        const auto destination = indices.find(route.destinationEndpointId);
        if (source == indices.end() || destination == indices.end()) {
            error = "Route references an endpoint that is not present in the inventory";
            return {};
        }
        if (!SupportsLink(endpoints[source->second].type, endpoints[destination->second].type)) {
            error = "Route endpoint directions are not supported";
            return {};
        }
        const float linearGain = static_cast<float>(std::pow(10.0, route.gainDb / 20.0));
        graph->routes_.push_back({source->second, destination->second, linearGain, route.mute});
    }
    return graph;
}

void RoutingGraph::Process(const float* const* sourceBlocks, float* const* destinationBlocks,
                           std::uint32_t frames, bool physicalOutputsPrecleared) const noexcept {
    if (!destinationBlocks) return;
    for (std::size_t endpoint = 0; endpoint < endpointCount_; ++endpoint) {
        if (destinationBlocks[endpoint] &&
            !(physicalOutputsPrecleared && endpointTypes_[endpoint] == RoutingEndpointType::PhysicalOutput))
            std::fill_n(destinationBlocks[endpoint], frames, 0.0f);
    }
    if (!sourceBlocks || frames == 0) return;
    for (const auto& route : routes_) {
        if (route.mute) continue;
        const float* source = sourceBlocks[route.sourceIndex];
        float* destination = destinationBlocks[route.destinationIndex];
        if (!source || !destination) continue;
        std::uint32_t frame = 0;
#if defined(TIMOX_ROUTING_SSE2)
        const __m128 gain = _mm_set1_ps(route.linearGain);
        for (; frame + 4 <= frames; frame += 4) {
            const __m128 mixed = _mm_add_ps(_mm_loadu_ps(destination + frame),
                _mm_mul_ps(_mm_loadu_ps(source + frame), gain));
            _mm_storeu_ps(destination + frame, mixed);
        }
#endif
        for (; frame < frames; ++frame)
            destination[frame] += source[frame] * route.linearGain;
    }
}
