#include "software_clock_host.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

static void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct Probe {
    std::atomic<unsigned> calls{0}, active{0}, violations{0};
    std::uint32_t expectedFrames = 256;
    std::int64_t busyTicks = 0;
    std::int64_t last = 0;
    static void Process(const float* const* in, float* const* out,
        std::uint32_t frames, void* context) noexcept {
        auto& p = *static_cast<Probe*>(context);
        if (p.active.fetch_add(1) != 0) p.violations.fetch_add(1);
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (in || out || frames != p.expectedFrames || now.QuadPart <= p.last)
            p.violations.fetch_add(1);
        p.last = now.QuadPart;
        if (p.busyTicks) {
            LARGE_INTEGER end{};
            do { QueryPerformanceCounter(&end); } while (end.QuadPart - now.QuadPart < p.busyTicks);
        }
        p.calls.fetch_add(1);
        p.active.fetch_sub(1);
    }
};
int main() {
    try {
        // Truncating each period causes drift; reusing missed deadlines bursts.
        SoftwareClockDeadline clock(100, 1000, 3000, 16);
        Require(clock.deadline() == 106, "fractional first deadline");
        Require(clock.advancePast(106) == 0 && clock.deadline() == 111, "ordinary advance");
        Require(clock.advancePast(140) == 5 && clock.deadline() == 143, "skip missed deadlines");
        Require(clock.advancePast(141) == 0 && clock.deadline() == 148, "irregular time stays monotone");
        Require(clock.advancePast(1000) == 159 && clock.deadline() == 1002, "large stall bounded advance");
        SoftwareClockDeadline drift(0, 10000000, 44100, 256);
        for (unsigned i = 0; i < 44099; ++i) drift.advancePast(drift.deadline());
        Require(drift.deadline() == 2560000000LL, "fractional periods accumulate drift");
        SoftwareClockHost host;
        AudioClockCapabilities caps;
        std::string error;
        Require(host.enumerate().empty(), "software is not a physical device");
        Require(!host.open(L"physical", nullptr, caps, error), "reject foreign id");
        Require(host.open(L"software", nullptr, caps, error), "open software");
        Require(caps.inputChannels == 0 && caps.outputChannels == 0 &&
            caps.currentSampleRate == 48000 && caps.preferredBufferFrames == 256,
            "zero physical channels and default format");
        for (double rate : {0., 7999., 384001., std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()})
            Require(!host.setSampleRate(rate, error), "reject invalid rate");
        for (double rate : {8000., 44100.5, 384000., 48000.})
        {
            Require(host.setSampleRate(rate, error), "accept supported finite rate");
            Require(host.refreshCapabilities(caps, error) && caps.currentSampleRate == rate,
                "report confirmed rate");
        }
        Probe probe;
        for (unsigned frames : {0u, 15u, 8193u})
            Require(!host.start(frames, {}, {}, &Probe::Process, &probe, error), "reject quantum");
        Require(!host.start(256, {0}, {}, &Probe::Process, &probe, error), "reject input channel");
        Require(!host.start(256, {}, {0}, &Probe::Process, &probe, error), "reject output channel");
        Require(!host.start(256, {}, {}, nullptr, &probe, error), "reject null callback");
        for (unsigned run = 0; run < 3; ++run) {
            const auto before = probe.calls.load();
            Require(host.start(256, {}, {}, &Probe::Process, &probe, error), error.c_str());
            Require(!host.setSampleRate(44100, error), "rate immutable while running");
            Require(!host.start(256, {}, {}, &Probe::Process, &probe, error), "reject concurrent start");
            for (unsigned wait = 0; wait < 200 && probe.calls.load() < before + 4; ++wait) Sleep(5);
            host.stop();
            const auto stopped = probe.calls.load();
            Require(stopped >= before + 4, "worker must produce callbacks");
            Sleep(20);
            Require(probe.calls.load() == stopped && probe.active.load() == 0,
                "stop must quiesce callbacks");
            Require(probe.violations.load() == 0, "serialized monotone zero-channel callbacks");
            Require(host.consumeTimingCounters().callbacks >= 4, "callback counters");
            Require(host.consumeTimingCounters().callbacks == 0, "consume resets counters");
        }
        for (unsigned frames : {16u, 8192u}) {
            Probe boundary;
            boundary.expectedFrames = frames;
            Require(host.start(frames, {}, {}, &Probe::Process, &boundary, error), "valid quantum boundaries");
            for (unsigned wait = 0; wait < 200 && !boundary.calls.load(); ++wait) Sleep(5);
            host.stop();
            Require(boundary.calls.load() > 0 && boundary.violations.load() == 0, "boundary quantum delivered");
        }
        Probe slow;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        slow.busyTicks = frequency.QuadPart / 50; // Deliberate 20ms test workload exceeds 5.33ms quantum.
        host.consumeTimingCounters();
        Require(host.start(256, {}, {}, &Probe::Process, &slow, error), "start slow callback");
        for (unsigned wait = 0; wait < 200 && slow.calls.load() < 2; ++wait) Sleep(5);
        host.stop();
        const auto timing = host.consumeTimingCounters();
        Require(timing.processingOverruns >= 2 && timing.worstProcessingNs >= 19000000,
            "report callback overruns");
        Require(host.start(256, {}, {}, &Probe::Process, &slow, error), "restart after overruns");
        host.close(); // Closing a running host must join too.
        const auto closedCount = slow.calls.load();
        Sleep(20);
        Require(closedCount >= 2 && slow.calls.load() == closedCount && slow.violations.load() == 0,
            "close quiesces an overrunning callback");
        host.close(); host.close();
        Require(!host.start(256, {}, {}, &Probe::Process, &probe, error), "closed start rejected");
        std::cout << "Software clock tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Software clock test failed: " << e.what() << '\n';
        return 1;
    }
}
