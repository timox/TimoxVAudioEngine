#include "routing_graph.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    constexpr std::uint32_t frames = 512;
    std::vector<RoutingEndpoint> endpoints;
    std::vector<AudioRoute> routes;
    for (int index = 0; index < 8; ++index) {
        const auto id = "virtual:TimoxVasio:1:output:" + std::to_string(index + 1);
        endpoints.push_back({id, RoutingEndpointType::VirtualOutput, 48000});
        routes.push_back({"route-" + std::to_string(index), id,
            "physical:device:output:" + std::to_string(index % 2 + 1), -3.0, false});
    }
    for (int index = 0; index < 2; ++index)
        endpoints.push_back({"physical:device:output:" + std::to_string(index + 1),
            RoutingEndpointType::PhysicalOutput, 48000});
    std::string error;
    auto graph = RoutingGraph::Create(endpoints, routes, 48000, error);
    if (!graph) return 1;
    std::array<std::array<float, frames>, 8> input{};
    std::array<std::array<float, frames>, 2> output{};
    std::array<const float*, 10> sources{};
    std::array<float*, 10> destinations{};
    for (int channel = 0; channel < 8; ++channel) {
        for (std::uint32_t frame = 0; frame < frames; ++frame)
            input[channel][frame] = static_cast<float>((channel + 1) * (frame % 31)) / 256.0f;
        sources[channel] = input[channel].data();
    }
    for (int channel = 0; channel < 2; ++channel)
        destinations[8 + channel] = output[channel].data();
    constexpr int iterations = 100000;
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration)
        graph->Process(sources.data(), destinations.data(), frames);
    const auto end = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::printf("%.1f ns/block checksum=%.5f\n", static_cast<double>(ns) / iterations,
        output[0][17] + output[1][17]);
    return 0;
}
