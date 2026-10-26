#include "audio_routing_runtime.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
bool closeTo(float left, float right) { return std::fabs(left - right) < 1.0e-5f; }
}

int main() {
    constexpr std::uint32_t frames = 2;
    const auto pid = GetCurrentProcessId();
    auto mapping = AudioClientMapping::CreateClient(1, pid, 48000, 256);
    if (!mapping) {
        std::fprintf(stderr, "Could not create test audio mapping.\n");
        return 1;
    }
    mapping->SetEngineAttached(true);
    AudioClientMapping::ChannelMask activeInputs{};
    AudioClientMapping::ChannelMask activeOutputs{};
    activeInputs[0] = (std::uint64_t{1} << 0) | (std::uint64_t{1} << 1);
    activeInputs[3] = std::uint64_t{1} << 63;
    activeOutputs[0] = std::uint64_t{3};
    activeOutputs[3] = std::uint64_t{1} << 63;
    if (!mapping->SetActiveChannels(activeInputs, activeOutputs)) return 6;
    auto pinned = std::shared_ptr<AudioClientMapping>(std::move(mapping));
    VasioClientSnapshot client{{1, pid, L"probe.exe"}, pinned};
    AudioRoutingLayout layout;
    layout.physicalDriverId = "{driver-1}";
    layout.sampleRate = 48000;
    layout.bufferFrames = frames;
    layout.physicalInputChannels = 10;
    layout.physicalOutputChannels = 10;
    layout.clients.push_back(client);
    layout.routes = {
        {"virtual-to-physical", "virtual:TimoxVasio:" + std::to_string(pid) + ":output:1",
            "physical:{driver-1}:output:1", 0.0, false},
        {"virtual-stereo-right", "virtual:TimoxVasio:" + std::to_string(pid) + ":output:2",
            "physical:{driver-1}:output:2", 0.0, false},
        {"physical-to-virtual", "physical:{driver-1}:input:1",
            "virtual:TimoxVasio:" + std::to_string(pid) + ":input:2", 0.0, false},
        {"virtual-loop", "virtual:TimoxVasio:" + std::to_string(pid) + ":output:1",
            "virtual:TimoxVasio:" + std::to_string(pid) + ":input:1", 0.0, false},
        {"high-output", "virtual:TimoxVasio:" + std::to_string(pid) + ":output:256",
            "physical:{driver-1}:output:1", 0.0, false},
        {"high-input", "physical:{driver-1}:input:1",
            "virtual:TimoxVasio:" + std::to_string(pid) + ":input:256", 0.0, false},
        {"physical-input-ten", "physical:{driver-1}:input:10",
            "virtual:TimoxVasio:" + std::to_string(pid) + ":input:2", 0.0, false},
        {"physical-output-ten", "virtual:TimoxVasio:" + std::to_string(pid) + ":output:1",
            "physical:{driver-1}:output:10", 0.0, false},
    };
    std::string error;
    auto runtime = AudioRoutingRuntime::Create(std::move(layout), error);
    if (!runtime) {
        std::fprintf(stderr, "Could not build test runtime: %s\n", error.c_str());
        return 2;
    }
    if (runtime->RoutedPhysicalInputChannels() != std::vector<std::uint32_t>{0, 9} ||
        runtime->RoutedPhysicalOutputChannels() != std::vector<std::uint32_t>{0, 1, 9}) {
        std::fprintf(stderr, "Physical buffers inputs:");
        for (const auto channel : runtime->RoutedPhysicalInputChannels()) std::fprintf(stderr, " %u", channel);
        std::fprintf(stderr, "; outputs:");
        for (const auto channel : runtime->RoutedPhysicalOutputChannels()) std::fprintf(stderr, " %u", channel);
        std::fprintf(stderr, "\n");
        return 7;
    }

    if (pinned->InputStride() != 3 || pinned->OutputStride() != 3 ||
        pinned->InputChannelAt(2) != 255 || pinned->OutputChannelAt(2) != 255) return 14;
    float clientOutput[frames * 3]{};
    clientOutput[0] = 0.25f; clientOutput[3] = -0.5f;
    clientOutput[1] = -0.5f; clientOutput[4] = 1.0f;
    clientOutput[2] = 0.375f; clientOutput[5] = -0.625f;
    if (!pinned->WriteClientOutput(clientOutput, frames)) return 3;
    const float physicalInputData[frames]{0.75f, 0.125f};
    const float physicalInputTenData[frames]{0.5f, -0.25f};
    const float* physicalInputs[10]{};
    physicalInputs[0] = physicalInputData;
    physicalInputs[9] = physicalInputTenData;
    float physicalOutputData[frames]{};
    float physicalOutputTenData[frames]{};
    float* physicalOutputs[10]{};
    physicalOutputs[0] = physicalOutputData;
    physicalOutputs[9] = physicalOutputTenData;
    runtime->Process(physicalInputs, physicalOutputs, frames);

    float clientInput[frames * 3]{};
    if (pinned->ReadClientInput(clientInput, frames) != frames) return 4;
    if (!closeTo(physicalOutputData[0], 0.625f) || !closeTo(physicalOutputData[1], -1.125f) ||
        !closeTo(clientInput[0], 0.25f) || !closeTo(clientInput[3], -0.5f) ||
        !closeTo(clientInput[1], 1.25f) || !closeTo(clientInput[4], -0.125f) ||
        !closeTo(physicalOutputTenData[0], 0.25f) || !closeTo(physicalOutputTenData[1], -0.5f) ||
        !closeTo(clientInput[2], 0.75f) || !closeTo(clientInput[5], 0.125f)) {
        std::fprintf(stderr, "Runtime sample mismatch: physical=%f,%f input0=%f,%f input1=%f,%f input255=%f,%f\n",
            physicalOutputData[0], physicalOutputData[1], clientInput[0], clientInput[3],
            clientInput[1], clientInput[4], clientInput[2], clientInput[5]);
        return 5;
    }

    const auto leftEndpoint = "virtual:TimoxVasio:" + std::to_string(pid) + ":output:1";
    const auto rightEndpoint = "virtual:TimoxVasio:" + std::to_string(pid) + ":output:2";
    if (!runtime->SetCorrelationPair(leftEndpoint, rightEndpoint)) return 8;
    for (std::uint32_t block = 0; block < 2400; ++block) {
        if (!pinned->WriteClientOutput(clientOutput, frames)) return 9;
        runtime->Process(physicalInputs, physicalOutputs, frames);
    }
    auto correlation = runtime->ReadCorrelation();
    if (!correlation.active || !correlation.hasSignal || !closeTo(correlation.correlation, -1.0f)) {
        std::fprintf(stderr, "Expected L/R anti-correlation -1, got active=%d signal=%d value=%f\n",
            correlation.active, correlation.hasSignal, correlation.correlation);
        return 10;
    }

    clientOutput[0] = clientOutput[3] = 0.0f;
    clientOutput[1] = clientOutput[4] = 0.0f;
    for (std::uint32_t block = 0; block < 2400; ++block) {
        if (!pinned->WriteClientOutput(clientOutput, frames)) return 11;
        runtime->Process(physicalInputs, physicalOutputs, frames);
    }
    correlation = runtime->ReadCorrelation();
    if (!correlation.active || correlation.hasSignal) {
        std::fprintf(stderr, "Expected explicit no-signal correlation state.\n");
        return 12;
    }
    runtime->StopCorrelation();
    if (runtime->ReadCorrelation().active) return 13;
    // A client without a route still advances on each physical clock cycle.
    // Its virtual inputs are silent and physical outputs stay silent.
    AudioRoutingLayout noRouteLayout;
    noRouteLayout.physicalDriverId = "{driver-1}";
    noRouteLayout.sampleRate = 48000;
    noRouteLayout.bufferFrames = frames;
    noRouteLayout.physicalInputChannels = 1;
    noRouteLayout.physicalOutputChannels = 1;
    noRouteLayout.clients.push_back(client);
    auto noRouteRuntime = AudioRoutingRuntime::Create(std::move(noRouteLayout), error);
    if (!noRouteRuntime || !noRouteRuntime->RoutedPhysicalInputChannels().empty() ||
        !noRouteRuntime->RoutedPhysicalOutputChannels().empty()) return 15;
    while (pinned->WaitForClientSignal(0)) {}
    float staleInput[frames * 3]{};
    while (pinned->ClientInputAvailable() >= frames)
        pinned->ReadClientInput(staleInput, frames);
    for (int cycle = 0; cycle < 2; ++cycle) {
        if (!pinned->WriteClientOutput(clientOutput, frames)) return 16;
        float output[frames]{1.0f, 1.0f};
        float* outputs[]{output};
        noRouteRuntime->Process(nullptr, outputs, frames);
        float input[frames * 3]{};
        if (!pinned->WaitForClientSignal(0) ||
            pinned->ClientOutputAvailable() != 0 ||
            pinned->ReadClientInput(input, frames) != frames ||
            !closeTo(output[0], 0.0f) || !closeTo(output[1], 0.0f)) return 17;
        for (float sample : input) if (!closeTo(sample, 0.0f)) {
            std::fprintf(stderr, "No-route input not silent: cycle=%d sample=%f\n", cycle, sample);
            return 18;
        }
    }
    auto stageCounters = noRouteRuntime->ConsumeStageCounters();
    if (stageCounters.clientRead.samples || stageCounters.routing.samples) return 19;
    for (int cycle = 0; cycle < 16; ++cycle)
        noRouteRuntime->Process(nullptr, nullptr, frames);
    stageCounters = noRouteRuntime->ConsumeStageCounters();
    if (stageCounters.clientRead.samples || stageCounters.routing.samples) return 20;
    noRouteRuntime->SetStageMeasurementsEnabled(true);
    for (int cycle = 0; cycle < 16; ++cycle)
        noRouteRuntime->Process(nullptr, nullptr, frames);
    stageCounters = noRouteRuntime->ConsumeStageCounters();
    if (stageCounters.clientRead.samples != 1 || stageCounters.routing.samples != 1) return 21;
    noRouteRuntime->SetStageMeasurementsEnabled(false);
    for (int cycle = 0; cycle < 16; ++cycle)
        noRouteRuntime->Process(nullptr, nullptr, frames);
    stageCounters = noRouteRuntime->ConsumeStageCounters();
    if (stageCounters.clientRead.samples || stageCounters.routing.samples) return 22;
    AudioRoutingLayout absentClientLayout;
    absentClientLayout.physicalDriverId = "{driver-1}";
    absentClientLayout.sampleRate = 48000;
    absentClientLayout.bufferFrames = frames;
    absentClientLayout.physicalInputChannels = 1;
    absentClientLayout.physicalOutputChannels = 1;
    auto absentClientRuntime = AudioRoutingRuntime::Create(std::move(absentClientLayout), error);
    if (!absentClientRuntime) return 23;
    absentClientRuntime->SetStageMeasurementsEnabled(true);
    for (int cycle = 0; cycle < 16; ++cycle)
        absentClientRuntime->Process(nullptr, nullptr, frames);
    stageCounters = absentClientRuntime->ConsumeStageCounters();
    if (stageCounters.clientRead.samples || stageCounters.routing.samples) return 24;
    auto engineMapping = AudioClientMapping::OpenEngine(1, pid, 48000);
    if (!engineMapping) return 25;
    auto enginePinned = std::shared_ptr<AudioClientMapping>(std::move(engineMapping));
    AudioRoutingLayout switchLayout;
    switchLayout.physicalDriverId = "{driver-1}";
    switchLayout.sampleRate = 48000;
    switchLayout.bufferFrames = frames;
    switchLayout.clients.push_back({{1, pid, L"probe.exe"}, enginePinned});
    auto switchRuntime = AudioRoutingRuntime::Create(std::move(switchLayout), error);
    if (!switchRuntime) return 26;
    switchRuntime->SetStageMeasurementsEnabled(true);
    if (!pinned->StageMeasurementsEnabled()) return 27;
    switchRuntime->SetStageMeasurementsEnabled(false);
    if (pinned->StageMeasurementsEnabled()) return 28;
    std::puts("PASS: physical/virtual routes transfer samples through the preallocated runtime.");
    return 0;
}
