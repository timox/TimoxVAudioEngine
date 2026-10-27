#include "routing_graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
struct Fixture {
    std::vector<RoutingEndpoint> endpoints{
        {"physical:driver-1:input:1", RoutingEndpointType::PhysicalInput, 48000},
        {"physical:driver-1:output:1", RoutingEndpointType::PhysicalOutput, 48000},
        {"virtual:TimoxVasio:111:output:1", RoutingEndpointType::VirtualOutput, 48000},
        {"virtual:TimoxVasio:222:input:1", RoutingEndpointType::VirtualInput, 48000},
        {"virtual:TimoxVasio:333:input:1", RoutingEndpointType::VirtualInput, 48000},
        {"virtual:TimoxVasio:444:output:1", RoutingEndpointType::VirtualOutput, 48000},
    };
};

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 1.0e-5f;
}

bool testSupportedRouteDirectionsAndSamples() {
    Fixture fixture;
    std::vector<AudioRoute> routes{
        {"app-link", "virtual:TimoxVasio:111:output:1", "virtual:TimoxVasio:222:input:1", 0.0, false},
        {"hardware-output", "virtual:TimoxVasio:111:output:1", "physical:driver-1:output:1", 0.0, false},
        {"hardware-input", "physical:driver-1:input:1", "virtual:TimoxVasio:222:input:1", 0.0, false},
    };
    std::string error;
    auto graph = RoutingGraph::Create(fixture.endpoints, routes, 48000, error);
    if (!graph) { std::fprintf(stderr, "route graph rejected valid links: %s\n", error.c_str()); return false; }

    std::vector<float> virtualOut{0.25f, -0.5f};
    std::vector<float> physicalIn{0.75f, 0.125f};
    std::vector<float> virtualIn(2), physicalOut(2);
    std::vector<const float*> source(fixture.endpoints.size(), nullptr);
    std::vector<float*> destination(fixture.endpoints.size(), nullptr);
    source[0] = physicalIn.data();
    source[2] = virtualOut.data();
    destination[1] = physicalOut.data();
    destination[3] = virtualIn.data();
    graph->Process(source.data(), destination.data(), 2);

    for (std::size_t frame = 0; frame < 2; ++frame) {
        if (!near(physicalOut[frame], virtualOut[frame]) ||
            !near(virtualIn[frame], virtualOut[frame] + physicalIn[frame])) return false;
    }
    return true;
}

bool testGainMixAndMute() {
    Fixture fixture;
    std::vector<AudioRoute> routes{
        {"mix-a", "virtual:TimoxVasio:111:output:1", "virtual:TimoxVasio:222:input:1", -6.0205999, false},
        {"mix-b", "virtual:TimoxVasio:444:output:1", "virtual:TimoxVasio:222:input:1", -6.0205999, false},
        {"muted", "physical:driver-1:input:1", "virtual:TimoxVasio:333:input:1", 0.0, true},
    };
    std::string error;
    auto graph = RoutingGraph::Create(fixture.endpoints, routes, 48000, error);
    if (!graph) { std::fprintf(stderr, "route graph rejected valid mix: %s\n", error.c_str()); return false; }
    std::vector<float> a{0.2f, -0.4f};
    std::vector<float> b{0.6f, 0.8f};
    std::vector<float> c{0.9f, 0.7f};
    std::vector<float> mixed(2, 9.0f), muted(2, 9.0f);
    std::vector<const float*> source(fixture.endpoints.size(), nullptr);
    std::vector<float*> destination(fixture.endpoints.size(), nullptr);
    source[2] = a.data(); source[5] = b.data(); source[0] = c.data();
    destination[3] = mixed.data(); destination[4] = muted.data();
    graph->Process(source.data(), destination.data(), 2);
    return near(mixed[0], 0.4f) && near(mixed[1], 0.2f) && near(muted[0], 0.0f) && near(muted[1], 0.0f);
}

bool testVectorAndTailMix() {
    Fixture fixture;
    std::vector<AudioRoute> routes{
        {"mix-a", "virtual:TimoxVasio:111:output:1", "virtual:TimoxVasio:222:input:1", -6.0205999, false},
        {"mix-b", "virtual:TimoxVasio:444:output:1", "virtual:TimoxVasio:222:input:1", 3.0, false},
    };
    std::string error;
    auto graph = RoutingGraph::Create(fixture.endpoints, routes, 48000, error);
    if (!graph) return false;
    constexpr std::size_t frames = 513;
    std::vector<float> a(frames), b(frames), mixed(frames, 7.0f);
    std::vector<const float*> source(fixture.endpoints.size(), nullptr);
    std::vector<float*> destination(fixture.endpoints.size(), nullptr);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        a[frame] = static_cast<float>(frame % 17) / 32.0f;
        b[frame] = -static_cast<float>(frame % 23) / 64.0f;
    }
    source[2] = a.data(); source[5] = b.data(); destination[3] = mixed.data();
    graph->Process(source.data(), destination.data(), static_cast<std::uint32_t>(frames));
    const float gainA = static_cast<float>(std::pow(10.0, routes[0].gainDb / 20.0));
    const float gainB = static_cast<float>(std::pow(10.0, routes[1].gainDb / 20.0));
    for (std::size_t frame = 0; frame < frames; ++frame)
        if (!near(mixed[frame], a[frame] * gainA + b[frame] * gainB)) return false;
    return true;
}

bool testNoRouteClearsDestinations() {
    Fixture fixture;
    std::string error;
    auto graph = RoutingGraph::Create(fixture.endpoints, {}, 48000, error);
    if (!graph) return false;
    std::vector<float> physicalOut(2, 0.0f);
    std::vector<float> virtualIn(2, 1.0f);
    std::vector<float*> destination(fixture.endpoints.size(), nullptr);
    destination[1] = physicalOut.data();
    destination[3] = virtualIn.data();
    graph->Process(nullptr, destination.data(), 2, true);
    if (!near(physicalOut[0], 0.0f) || !near(physicalOut[1], 0.0f) ||
        !near(virtualIn[0], 0.0f) || !near(virtualIn[1], 0.0f)) return false;

    std::fill(physicalOut.begin(), physicalOut.end(), 1.0f);
    std::fill(virtualIn.begin(), virtualIn.end(), 1.0f);
    graph->Process(nullptr, destination.data(), 2);
    return near(physicalOut[0], 0.0f) && near(physicalOut[1], 0.0f) &&
        near(virtualIn[0], 0.0f) && near(virtualIn[1], 0.0f);
}

bool testInvalidRoutesAreRejected() {
    Fixture fixture;
    std::string error;
    const AudioRoute valid{"route-1", "virtual:TimoxVasio:111:output:1", "virtual:TimoxVasio:222:input:1", 0.0, false};
    if (RoutingGraph::Create(fixture.endpoints, {{"unknown", "missing", valid.destinationEndpointId, 0.0, false}}, 48000, error)) return false;
    if (RoutingGraph::Create(fixture.endpoints, {{"direction", "virtual:TimoxVasio:222:input:1", valid.destinationEndpointId, 0.0, false}}, 48000, error)) return false;
    if (RoutingGraph::Create(fixture.endpoints, {valid, valid}, 48000, error)) return false;
    auto nonFinite = valid;
    nonFinite.gainDb = std::numeric_limits<double>::quiet_NaN();
    if (RoutingGraph::Create(fixture.endpoints, {nonFinite}, 48000, error)) return false;
    auto wrongRange = valid;
    wrongRange.gainDb = 25.0;
    if (RoutingGraph::Create(fixture.endpoints, {wrongRange}, 48000, error)) return false;
    return true;
}
}

int main() {
    if (!testSupportedRouteDirectionsAndSamples()) return 1;
    if (!testGainMixAndMute()) return 1;
    if (!testVectorAndTailMix()) return 1;
    if (!testNoRouteClearsDestinations()) return 1;
    if (!testInvalidRoutesAreRejected()) return 1;
    std::puts("PASS: supported route directions, float mixing, gain, mute and invalid-route rejection.");
    return 0;
}
