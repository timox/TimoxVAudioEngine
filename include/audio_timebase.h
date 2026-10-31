#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Reference clock timebase for MIDI. The reference clock is the clock that
// paces the audio graph: the physical ASIO driver, or the engine's software
// clock without a card. (Unrelated to MIDI clock master/slave roles.) At each
// buffer it publishes its sample position (the driver's own position for ASIO)
// and the host time (steady clock, ns). A linear fit over the last ~30 s gives
// its real sample rate, so MIDI timing follows the reference clock's samples
// and never drifts from the audio. There is no fallback to Windows timers:
// without a running reference clock, MIDI clock generation waits.
//
// Threads: Begin/Publish/End run on the audio callback thread only (lock-free,
// no allocation). Update runs on one consumer thread only (the MIDI clock
// thread). Current and the conversions can be called from any thread.
enum class AudioTimebaseSource : std::uint8_t { None = 0, Asio = 1, Software = 2 };

class AudioTimebase {
public:
    struct Mapping {
        bool referenceRunning = false;     // the reference clock is running and has published
        AudioTimebaseSource source = AudioTimebaseSource::None;
        double nominalRate = 0;      // rate the card was opened at
        double measuredRate = 0;     // frames per second of host time; nominal until measured
        double driftPpm = 0;         // (measured / nominal - 1) * 1e6
        double spanSeconds = 0;      // time covered by the fit
        std::uint64_t generation = 0;
        // Fit: frames = framesAtOrigin + measuredRate * (hostNs - originNs) / 1e9
        std::int64_t originNs = 0;
        double framesAtOrigin = 0;

        // Card time in seconds: frames / nominal rate. Without a card, host seconds.
        double CardSeconds(std::int64_t hostNs) const noexcept;
        std::int64_t HostNs(double cardSeconds) const noexcept;
    };

    static AudioTimebase& Shared() noexcept;

    void Begin(double nominalRate, AudioTimebaseSource source) noexcept;
    void Publish(std::uint64_t framePosition, std::int64_t hostNs) noexcept;
    void End() noexcept;

    // Consumes published anchors and refreshes the fit. Returns Current().
    Mapping Update() noexcept;
    Mapping Current() const noexcept;

    static constexpr std::size_t kRingSize = 512;
    static constexpr std::size_t kFitPoints = 300;
    static constexpr std::int64_t kFitSpacingNs = 100'000'000;  // one fit point per 100 ms
    static constexpr double kMinimumFitSeconds = 2.0;

private:
    struct Anchor { std::uint64_t frames; std::int64_t hostNs; std::uint64_t generation; };
    struct Point { double x; double y; };  // seconds and frames relative to the fit origin

    // Producer side (audio thread).
    std::array<Anchor, kRingSize> ring_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<double> nominalRate_{0};
    std::atomic<std::uint8_t> source_{0};

    // Consumer side (Update thread only).
    std::uint64_t fitGeneration_ = 0;
    std::array<Point, kFitPoints> points_{};
    std::size_t pointCount_ = 0;
    std::size_t pointNext_ = 0;
    std::int64_t fitOriginNs_ = 0;
    std::uint64_t fitOriginFrames_ = 0;
    std::int64_t lastPointNs_ = 0;
    std::uint64_t lastFrames_ = 0;
    std::int64_t lastHostNs_ = 0;

    // Published mapping (seqlock over atomics, readable from any thread).
    std::atomic<std::uint32_t> sequence_{0};
    std::atomic<bool> cardLocked_{false};
    std::atomic<double> mappedNominal_{0};
    std::atomic<double> measuredRate_{0};
    std::atomic<double> spanSeconds_{0};
    std::atomic<std::int64_t> originNs_{0};
    std::atomic<double> framesAtOrigin_{0};
    std::atomic<std::uint64_t> mappedGeneration_{0};
    std::atomic<std::uint8_t> mappedSource_{0};

    void Store(const Mapping& mapping) noexcept;
    void Reset(std::uint64_t generation) noexcept;
    void AddPoint(const Anchor& anchor) noexcept;
};
