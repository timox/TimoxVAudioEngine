#include "audio_routing_runtime.h"
#include "realtime_callback_slot.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

int main() {
    constexpr std::uint32_t frames = 64;
    const auto pid = GetCurrentProcessId();
    auto mapping = AudioClientMapping::CreateClient(1, pid, 48000, frames);
    if (!mapping) return 1;
    mapping->SetEngineAttached(true);
    AudioClientMapping::ChannelMask inputs{};
    AudioClientMapping::ChannelMask outputs{};
    outputs[0] = 3;
    if (!mapping->SetActiveChannels(inputs, outputs)) return 2;
    auto pinned = std::shared_ptr<AudioClientMapping>(std::move(mapping));
    const auto prefix = "virtual:TimoxVasio:" + std::to_string(pid) + ":output:";

    auto makeRuntime = [&](int channel) {
        AudioRoutingLayout layout;
        layout.physicalDriverId = "{test}";
        layout.sampleRate = 48000;
        layout.bufferFrames = frames;
        layout.physicalOutputChannels = 1;
        layout.clients.push_back({{1, pid, L"hot-swap-test.exe"}, pinned});
        if (channel) layout.routes.push_back({"test-route", prefix + std::to_string(channel),
            "physical:{test}:output:1", 0.0, false});
        std::string error;
        auto runtime = AudioRoutingRuntime::Create(std::move(layout), error);
        if (!runtime) std::fprintf(stderr, "Runtime creation failed: %s\n", error.c_str());
        return runtime;
    };
    auto first = makeRuntime(1);
    auto second = makeRuntime(2);
    auto withoutRoute = makeRuntime(0);
    if (!first || !second || !withoutRoute) return 3;
    if (first->RoutedPhysicalOutputChannels() != second->RoutedPhysicalOutputChannels()) return 4;

    RealtimeCallbackSlot slot(&AudioRoutingRuntime::PhysicalCallback);
    std::vector<float> block(static_cast<std::size_t>(frames) * pinned->OutputStride());
    block[0] = 0.25f;
    block[1] = -0.5f;
    float physicalOutput[frames]{};
    float* physicalOutputs[1]{physicalOutput};

    slot.Publish(first.get());
    if (!pinned->WriteClientOutput(block.data(), frames)) return 5;
    RealtimeCallbackSlot::Dispatch(nullptr, physicalOutputs, frames, &slot);
    if (std::fabs(physicalOutput[0] - 0.25f) > 1.0e-6f) return 6;

    slot.Publish(second.get());
    slot.WaitForQuiescence();
    first.reset();
    if (!pinned->WriteClientOutput(block.data(), frames)) return 7;
    std::fill_n(physicalOutput, frames, 0.0f);
    RealtimeCallbackSlot::Dispatch(nullptr, physicalOutputs, frames, &slot);
    if (std::fabs(physicalOutput[0] + 0.5f) > 1.0e-6f) return 8;

    slot.Publish(withoutRoute.get());
    slot.WaitForQuiescence();
    second.reset();
    // The physical host owns this buffer and clears it before each callback.
    std::fill_n(physicalOutput, frames, 0.0f);
    if (!pinned->WriteClientOutput(block.data(), frames)) return 9;
    RealtimeCallbackSlot::Dispatch(nullptr, physicalOutputs, frames, &slot);
    if (pinned->ClientOutputAvailable() != 0 || physicalOutput[0] != 0.0f ||
        !pinned->WaitForClientSignal(0)) return 10;

    slot.Publish(nullptr);
    slot.WaitForQuiescence();
    withoutRoute.reset();
    std::puts("PASS: routing graph changed and its last route was removed at the callback boundary.");
    return 0;
}
