// Controlled A/B of the engine audio data path, not a hardware latency test.
// Real shared rings, routing, meters and interleaving; identical synthetic host
// workload on one thread. M1 scheduling and M2 hardware notification are excluded.
#include "audio_routing_runtime.h"
#include "asio_sample_conversion.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

bool measure(std::uint32_t frames, unsigned channels, ASIOSampleType type) {
    const auto pid = GetCurrentProcessId();
    // Use a private driver slot so a running production engine cannot discover
    // and consume this benchmark's shared rings during the measurement.
    constexpr std::uint32_t benchmarkDriver = 0xfffffffeu;
    auto owned = AudioClientMapping::CreateClient(benchmarkDriver, pid, 48000, frames);
    if (!owned) return false;
    AudioClientMapping::ChannelMask mask{};
    for (unsigned c = 0; c < channels; ++c) mask[c / 64] |= std::uint64_t{1} << (c % 64);
    if (!owned->SetActiveChannels(mask, mask) || !owned->SetEngineAttached(true)) return false;
    auto mapping = std::shared_ptr<AudioClientMapping>(std::move(owned));
    AudioRoutingLayout layout;
    layout.physicalDriverId = "benchmark";
    layout.sampleRate = 48000;
    layout.bufferFrames = frames;
    layout.physicalInputChannels = channels;
    layout.physicalOutputChannels = channels;
    // Runtime endpoint metadata still uses the public single-driver identity.
    layout.clients.push_back({{1, pid, L"AudioPathBenchmark.exe"}, mapping});
    for (unsigned c = 0; c < channels; ++c) {
        const auto ch = std::to_string(c + 1);
        const auto client = "virtual:TimoxVasio:" + std::to_string(pid);
        layout.routes.push_back({"in-" + ch, "physical:benchmark:input:" + ch,
            client + ":input:" + ch, -3.0, false});
        layout.routes.push_back({"out-" + ch, client + ":output:" + ch,
            "physical:benchmark:output:" + ch, -3.0, false});
    }
    std::string error;
    auto runtime = AudioRoutingRuntime::Create(std::move(layout), error);
    if (!runtime) { std::fprintf(stderr, "%s\n", error.c_str()); return false; }
    const auto bytes = AsioSampleConversion::BytesPerSample(type);
    std::vector<std::uint8_t> pcmInput(channels * frames * bytes), pcmOutput(pcmInput.size());
    std::vector<float> input(channels * frames), output(input.size()), clientInput(input.size()),
        clientOutput(input.size()), hostPlanes(input.size());
    std::vector<const float*> inputs(channels);
    std::vector<float*> outputs(channels);
    for (unsigned c = 0; c < channels; ++c) {
        inputs[c] = input.data() + c * frames;
        outputs[c] = output.data() + c * frames;
        for (std::uint32_t f = 0; f < frames; ++f) {
            const float value = float((f + c) % 61) / 128.0f;
            AsioSampleConversion::Encode(pcmInput.data() + (c * frames + f) * bytes, type, value);
            clientOutput[f * channels + c] = value;
        }
    }
    const auto decoder = AsioSampleConversion::SelectDecoder(type);
    const auto encoder = AsioSampleConversion::SelectEncoder(type);
    const int rounds = frames == 256 ? 2000 : 300;
    const auto run = [&](bool block) {
        const auto begin = std::chrono::steady_clock::now();
        for (int r = 0; r < rounds; ++r) {
            if (!mapping->WriteClientOutput(clientOutput.data(), frames)) return -1.0;
            for (unsigned c = 0; c < channels; ++c) {
                const auto* src = pcmInput.data() + c * frames * bytes;
                auto* dst = input.data() + c * frames;
                if (block) decoder(dst, src, frames);
                else for (std::uint32_t f = 0; f < frames; ++f)
                    dst[f] = AsioSampleConversion::Decode(src + f * bytes, type);
                std::fill_n(outputs[c], frames, 0.0f);
            }
            runtime->Process(inputs.data(), outputs.data(), frames, true);
            for (unsigned c = 0; c < channels; ++c) {
                auto* dst = pcmOutput.data() + c * frames * bytes;
                if (block) encoder(dst, outputs[c], frames);
                else for (std::uint32_t f = 0; f < frames; ++f)
                    AsioSampleConversion::Encode(dst + f * bytes, type, outputs[c][f]);
            }
            if (!mapping->WaitForClientSignal(0) ||
                mapping->ReadClientInput(clientInput.data(), frames) != frames) return -1.0;
            // DLL-side planar/interleaved copies, with a trivial identity host.
            for (unsigned c = 0; c < channels; ++c)
                for (std::uint32_t f = 0; f < frames; ++f)
                    hostPlanes[c * frames + f] = clientInput[f * channels + c];
            std::fill(clientOutput.begin(), clientOutput.end(), 0.0f);
            for (unsigned c = 0; c < channels; ++c)
                for (std::uint32_t f = 0; f < frames; ++f)
                    clientOutput[f * channels + c] = hostPlanes[c * frames + f];
        }
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / rounds;
    };
    if (run(false) < 0 || run(true) < 0) return false;
    const auto reference = pcmOutput;
    std::vector<double> scalar, block;
    for (int repeat = 0; repeat < 7; ++repeat) {
        // Alternate order to reduce systematic warm-cache and CPU-frequency bias.
        if (repeat % 2) { block.push_back(run(true)); scalar.push_back(run(false)); }
        else { scalar.push_back(run(false)); block.push_back(run(true)); }
        if (scalar.back() < 0 || block.back() < 0 || pcmOutput != reference) return false;
    }
    std::sort(scalar.begin(), scalar.end());
    std::sort(block.begin(), block.end());
    const auto before = scalar[3], after = block[3];
    const double periodUs = frames * 1000000.0 / 48000.0;
    std::printf("frames=%u channels=%u type=%ld before_us=%.3f after_us=%.3f reduction_pct=%.1f budget_before_pct=%.3f budget_after_pct=%.3f\n",
        frames, channels, type, before, after, 100.0 * (1 - after / before),
        100.0 * before / periodUs, 100.0 * after / periodUs);
    return true;
}

int main() {
    for (const auto frames : {256u, 2048u})
        for (const auto channels : {2u, 8u})
            for (const auto type : {ASIOSTInt24LSB, ASIOSTInt32LSB, ASIOSTFloat32LSB})
                if (!measure(frames, channels, type)) return 1;
    return 0;
}
