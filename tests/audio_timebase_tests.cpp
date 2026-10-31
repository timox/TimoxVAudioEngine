#include "audio_timebase.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); return false; } } while (0)

// Deterministic callback jitter in [-0.3 ms, +0.3 ms].
std::int64_t jitterNs(std::uint64_t index) {
    const std::uint64_t mixed = (index * 2654435761u) % 601u;
    return (static_cast<std::int64_t>(mixed) - 300) * 1000;
}

// A card running `ppm` fast relative to the host clock, 256-frame buffers.
void runCard(AudioTimebase& timebase, double nominal, double ppm, double seconds, std::int64_t startNs,
             std::uint64_t& frames, std::int64_t& nowNs) {
    const double actual = nominal * (1.0 + ppm * 1e-6);
    const std::uint64_t buffers = static_cast<std::uint64_t>(seconds * actual / 256.0);
    std::int64_t lastUpdate = startNs;
    for (std::uint64_t i = 0; i < buffers; ++i) {
        const auto ideal = startNs + static_cast<std::int64_t>(std::llround(static_cast<double>(frames) / actual * 1e9));
        nowNs = ideal + jitterNs(i);
        timebase.Publish(frames, nowNs);
        frames += 256;
        if (nowNs - lastUpdate >= 50'000'000) { timebase.Update(); lastUpdate = nowNs; }
    }
    timebase.Update();
}

bool testMeasuresCardDriftAgainstHostClock() {
    AudioTimebase timebase;
    CHECK(!timebase.Update().cardLocked);  // no card: host clock as is
    timebase.Begin(48000.0, AudioTimebaseSource::Asio);
    std::uint64_t frames = 0;
    std::int64_t now = 0;
    runCard(timebase, 48000.0, 40.0, 1.0, 5'000'000'000, frames, now);
    auto mapping = timebase.Current();
    CHECK(mapping.cardLocked && mapping.measuredRate == 48000.0 && mapping.driftPpm == 0);  // not enough history yet
    runCard(timebase, 48000.0, 40.0, 40.0, 6'000'000'000, frames, now);
    mapping = timebase.Current();
    CHECK(mapping.cardLocked);
    CHECK(std::fabs(mapping.driftPpm - 40.0) < 3.0);
    CHECK(mapping.spanSeconds > 25.0);

    // Card time advances 40 ppm faster than host time, and conversions round-trip.
    const double before = mapping.CardSeconds(now), after = mapping.CardSeconds(now + 10'000'000'000);
    CHECK(std::fabs((after - before) - 10.0 * (1.0 + 40e-6)) < 50e-6);
    CHECK(std::llabs(mapping.HostNs(mapping.CardSeconds(now)) - now) <= 2);
    return true;
}

bool testRestartForgetsThePreviousCard() {
    AudioTimebase timebase;
    timebase.Begin(48000.0, AudioTimebaseSource::Asio);
    std::uint64_t frames = 0;
    std::int64_t now = 0;
    runCard(timebase, 48000.0, -60.0, 20.0, 1'000'000'000, frames, now);
    CHECK(std::fabs(timebase.Current().driftPpm + 60.0) < 4.0);
    timebase.End();
    CHECK(!timebase.Update().cardLocked);
    timebase.Begin(44100.0, AudioTimebaseSource::Software);
    frames = 0;
    runCard(timebase, 44100.0, 25.0, 20.0, now + 1'000'000'000, frames, now);
    const auto mapping = timebase.Current();
    CHECK(mapping.nominalRate == 44100.0 && std::fabs(mapping.driftPpm - 25.0) < 4.0);
    CHECK(mapping.source == AudioTimebaseSource::Software);
    return true;
}

bool testDropoutsDoNotLookLikeDrift() {
    AudioTimebase timebase;
    timebase.Begin(48000.0, AudioTimebaseSource::Asio);
    std::uint64_t frames = 0;
    std::int64_t now = 0;
    runCard(timebase, 48000.0, 0.0, 10.0, 1'000'000'000, frames, now);
    // The frame counter jumps back (driver reset without End): the fit restarts.
    frames = 0;
    runCard(timebase, 48000.0, 0.0, 1.0, now + 10'000'000, frames, now);
    const auto mapping = timebase.Current();
    CHECK(mapping.cardLocked && std::fabs(mapping.driftPpm) < 1000.0);
    return true;
}
}

bool testConcurrentPublishUpdateAndRead() {
    AudioTimebase timebase;
    timebase.Begin(48000.0, AudioTimebaseSource::Asio);
    std::atomic<bool> done{false};
    std::atomic<bool> torn{false};
    std::thread audio([&] {  // the ASIO callback
        std::uint64_t frames = 0;
        for (std::int64_t ns = 0; ns < 3'000'000'000; ns += 5'333'333) { timebase.Publish(frames, ns); frames += 256; }
        done = true;
    });
    std::thread clock([&] { while (!done) timebase.Update(); timebase.Update(); });  // the MIDI clock thread
    std::thread reader([&] {  // API and receive threads
        while (!done) {
            const auto mapping = timebase.Current();
            if (mapping.cardLocked && mapping.nominalRate != 48000.0) torn = true;
        }
    });
    audio.join(); clock.join(); reader.join();
    CHECK(!torn && timebase.Current().cardLocked);
    return true;
}

int main() {
    bool ok = true;
    ok &= testMeasuresCardDriftAgainstHostClock();
    ok &= testRestartForgetsThePreviousCard();
    ok &= testDropoutsDoNotLookLikeDrift();
    ok &= testConcurrentPublishUpdateAndRead();
    if (!ok) return 1;
    std::puts("AudioTimebaseTests passed");
    return 0;
}
