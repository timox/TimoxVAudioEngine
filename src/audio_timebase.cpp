#include "audio_timebase.h"

#include <cmath>

namespace {
constexpr double kMaximumPlausiblePpm = 1000.0;  // beyond this the fit is broken (dropouts), not drift
}

double AudioTimebase::Mapping::CardSeconds(std::int64_t hostNs) const noexcept {
    if (!cardLocked || nominalRate <= 0 || measuredRate <= 0) return static_cast<double>(hostNs) * 1e-9;
    const double frames = framesAtOrigin + measuredRate * static_cast<double>(hostNs - originNs) * 1e-9;
    return frames / nominalRate;
}

std::int64_t AudioTimebase::Mapping::HostNs(double cardSeconds) const noexcept {
    if (!cardLocked || nominalRate <= 0 || measuredRate <= 0) return static_cast<std::int64_t>(std::llround(cardSeconds * 1e9));
    const double frames = cardSeconds * nominalRate;
    return originNs + static_cast<std::int64_t>(std::llround((frames - framesAtOrigin) / measuredRate * 1e9));
}

AudioTimebase& AudioTimebase::Shared() noexcept {
    static AudioTimebase instance;
    return instance;
}

void AudioTimebase::Begin(double nominalRate) noexcept {
    nominalRate_.store(nominalRate > 0 && std::isfinite(nominalRate) ? nominalRate : 0, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void AudioTimebase::End() noexcept {
    nominalRate_.store(0, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void AudioTimebase::Publish(std::uint64_t framePosition, std::int64_t hostNs) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) >= kRingSize) return;  // consumer behind: drop
    ring_[head % kRingSize] = {framePosition, hostNs, generation_.load(std::memory_order_relaxed)};
    head_.store(head + 1, std::memory_order_release);
}

void AudioTimebase::Reset(std::uint64_t generation) noexcept {
    fitGeneration_ = generation;
    pointCount_ = pointNext_ = 0;
    lastPointNs_ = 0;
    lastFrames_ = 0;
    lastHostNs_ = 0;
}

void AudioTimebase::AddPoint(const Anchor& anchor) noexcept {
    if (pointCount_ == 0) {
        fitOriginNs_ = anchor.hostNs;
        fitOriginFrames_ = anchor.frames;
    } else if (anchor.hostNs - lastPointNs_ < kFitSpacingNs) {
        lastFrames_ = anchor.frames;
        lastHostNs_ = anchor.hostNs;
        return;
    }
    points_[pointNext_] = {static_cast<double>(anchor.hostNs - fitOriginNs_) * 1e-9,
        static_cast<double>(anchor.frames - fitOriginFrames_)};
    pointNext_ = (pointNext_ + 1) % kFitPoints;
    if (pointCount_ < kFitPoints) ++pointCount_;
    lastPointNs_ = anchor.hostNs;
    lastFrames_ = anchor.frames;
    lastHostNs_ = anchor.hostNs;
}

AudioTimebase::Mapping AudioTimebase::Update() noexcept {
    const auto generation = generation_.load(std::memory_order_acquire);
    const double nominal = nominalRate_.load(std::memory_order_relaxed);
    if (generation != fitGeneration_) Reset(generation);
    for (auto tail = tail_.load(std::memory_order_relaxed), head = head_.load(std::memory_order_acquire);
         tail != head; ++tail) {
        const auto anchor = ring_[tail % kRingSize];
        tail_.store(tail + 1, std::memory_order_release);
        if (anchor.generation == generation) AddPoint(anchor);
    }

    Mapping mapping;
    mapping.generation = generation;
    if (nominal <= 0 || pointCount_ == 0) {
        Store(mapping);
        return mapping;
    }
    mapping.cardLocked = true;
    mapping.nominalRate = nominal;
    // Until enough history exists, extrapolate from the last buffer at the nominal rate.
    mapping.measuredRate = nominal;
    mapping.originNs = lastHostNs_;
    mapping.framesAtOrigin = static_cast<double>(lastFrames_);

    double sx = 0, sy = 0;
    double minX = points_[0].x, maxX = points_[0].x;
    for (std::size_t i = 0; i < pointCount_; ++i) {
        sx += points_[i].x; sy += points_[i].y;
        minX = std::fmin(minX, points_[i].x); maxX = std::fmax(maxX, points_[i].x);
    }
    const double span = maxX - minX;
    if (pointCount_ >= 8 && span >= kMinimumFitSeconds) {
        const double n = static_cast<double>(pointCount_), mx = sx / n, my = sy / n;
        double sxx = 0, sxy = 0;
        for (std::size_t i = 0; i < pointCount_; ++i) {
            const double dx = points_[i].x - mx;
            sxx += dx * dx;
            sxy += dx * (points_[i].y - my);
        }
        const double slope = sxx > 0 ? sxy / sxx : 0;
        const double ppm = (slope / nominal - 1.0) * 1e6;
        if (slope > 0 && std::fabs(ppm) <= kMaximumPlausiblePpm) {
            mapping.measuredRate = slope;
            mapping.driftPpm = ppm;
            mapping.spanSeconds = span;
            mapping.originNs = fitOriginNs_;
            mapping.framesAtOrigin = static_cast<double>(fitOriginFrames_) + (my - slope * mx);
        } else {
            Reset(generation);  // dropouts or a discontinuity: measure again
        }
    }
    Store(mapping);
    return mapping;
}

// Single-writer sequence lock without fences: the odd sequence is stored
// before any field, fields are released, readers acquire them and recheck.
void AudioTimebase::Store(const Mapping& mapping) noexcept {
    const auto sequence = sequence_.load(std::memory_order_relaxed);
    sequence_.store(sequence + 1, std::memory_order_relaxed);
    cardLocked_.store(mapping.cardLocked, std::memory_order_release);
    mappedNominal_.store(mapping.nominalRate, std::memory_order_release);
    measuredRate_.store(mapping.measuredRate, std::memory_order_release);
    spanSeconds_.store(mapping.spanSeconds, std::memory_order_release);
    originNs_.store(mapping.originNs, std::memory_order_release);
    framesAtOrigin_.store(mapping.framesAtOrigin, std::memory_order_release);
    mappedGeneration_.store(mapping.generation, std::memory_order_release);
    sequence_.store(sequence + 2, std::memory_order_release);
}

AudioTimebase::Mapping AudioTimebase::Current() const noexcept {
    Mapping mapping;
    for (;;) {
        const auto before = sequence_.load(std::memory_order_acquire);
        if (before & 1u) continue;
        mapping.cardLocked = cardLocked_.load(std::memory_order_acquire);
        mapping.nominalRate = mappedNominal_.load(std::memory_order_acquire);
        mapping.measuredRate = measuredRate_.load(std::memory_order_acquire);
        mapping.spanSeconds = spanSeconds_.load(std::memory_order_acquire);
        mapping.originNs = originNs_.load(std::memory_order_acquire);
        mapping.framesAtOrigin = framesAtOrigin_.load(std::memory_order_acquire);
        mapping.generation = mappedGeneration_.load(std::memory_order_acquire);
        if (sequence_.load(std::memory_order_acquire) == before) break;
    }
    mapping.driftPpm = mapping.cardLocked && mapping.nominalRate > 0 && mapping.spanSeconds > 0
        ? (mapping.measuredRate / mapping.nominalRate - 1.0) * 1e6 : 0;
    return mapping;
}
