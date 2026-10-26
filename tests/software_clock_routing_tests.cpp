#include "audio_routing_runtime.h"
#include "software_clock_host.h"
#include "persistent_routes.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        constexpr std::uint32_t frames = 64;
        const auto pid = GetCurrentProcessId();
        auto source = AudioClientMapping::CreateClient(1, pid, 48000, 256);
        auto destination = AudioClientMapping::CreateClient(1, pid + 1, 48000, 256);
        require(source && destination, "create synthetic application mappings");
        AudioClientMapping::ChannelMask inputs{}, outputs{};
        inputs[0] = outputs[0] = 1;
        for (auto* mapping : {source.get(), destination.get()}) {
            mapping->SetEngineAttached(true);
            require(mapping->SetActiveChannels(inputs, outputs), "activate application channels");
        }
        auto sender = std::shared_ptr<AudioClientMapping>(std::move(source));
        auto receiver = std::shared_ptr<AudioClientMapping>(std::move(destination));
        AudioRoutingLayout layout;
        layout.sampleRate = 48000;
        layout.bufferFrames = frames;
        layout.clients = {{{1, pid, L"source.exe"}, sender},
            {{1, pid + 1, L"destination.exe"}, receiver}};
        layout.routes = {{"application-loop", "virtual:TimoxVasio:" + std::to_string(pid) + ":output:1",
            "virtual:TimoxVasio:" + std::to_string(pid + 1) + ":input:1", 0.0, false}};
        auto savedRoutes = layout.routes;
        savedRoutes.push_back({"hardware", "physical:fake:input:1",
            "virtual:TimoxVasio:" + std::to_string(pid + 1) + ":input:1", 0.0, false});
        const auto softwareRoutes = PersistentRoutes::ResolveForClock(savedRoutes, layout.clients, std::nullopt);
        require(softwareRoutes.size() == 1 && softwareRoutes.front().id == "application-loop",
            "software graph keeps application route while physical route stays dormant");
        require(PersistentRoutes::ResolveForClock(savedRoutes, layout.clients, std::string("other")).size() == 1,
            "different ASIO device does not activate saved physical route");
        require(PersistentRoutes::ResolveForClock(savedRoutes, layout.clients, std::string("fake")).size() == 2 &&
            savedRoutes.size() == 2, "return to selected ASIO device reactivates preserved route");
        std::string error;
        auto runtime = AudioRoutingRuntime::Create(layout, error);
        require(!!runtime, "create graph without physical endpoints");
        require(runtime->RoutedPhysicalInputChannels().empty() &&
            runtime->RoutedPhysicalOutputChannels().empty(), "no physical buffers requested");
        float output[frames];
        for (auto& sample : output) sample = 0.375f;
        require(sender->WriteClientOutput(output, frames), "queue source application block");
        SoftwareClockHost clock;
        AudioClockCapabilities capabilities;
        require(clock.open(L"software", nullptr, capabilities, error), "open software cadence");
        require(clock.start(frames, {}, {}, &AudioRoutingRuntime::PhysicalCallback, runtime.get(), error),
            "start software routing callback");
        require(receiver->WaitForClientSignal(1000), "destination receives software clock signal");
        clock.stop();
        float input[frames]{};
        require(receiver->ReadClientInput(input, frames) == frames, "destination receives complete block");
        for (float sample : input)
            require(std::fabs(sample - 0.375f) < 1e-6f, "software graph routes actual application samples");
        require(sender->WaitForClientSignal(0), "source also receives callback signal");

        while (receiver->ReadClientInput(input, frames) != 0) {}
        layout.routes.clear();
        runtime = AudioRoutingRuntime::Create(layout, error);
        require(!!runtime, "create software graph with unrouted clients");
        receiver->WaitForClientSignal(0);
        require(clock.start(frames, {}, {}, &AudioRoutingRuntime::PhysicalCallback, runtime.get(), error),
            "restart software callback without routes");
        require(receiver->WaitForClientSignal(1000), "unrouted client still receives clock signal");
        clock.stop();
        require(receiver->ReadClientInput(input, frames) == frames, "unrouted client receives input block");
        for (float sample : input) require(sample == 0.0f, "unrouted software input is silence");
        clock.close();
        std::puts("PASS: software cadence routes application samples and signals unrouted clients");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
