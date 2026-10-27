#pragma once

#include <cstdint>

struct AsioBufferWindow {
    std::uint32_t sampleRate;
    std::uint32_t bufferFrames;
    std::uint64_t durationMs;
    std::uint64_t callbacks;
    std::uint64_t delayedStarts;
    std::uint64_t processingOverruns;
    std::uint64_t worstProcessingNs;
};

enum class AsioBufferHealthState { Unavailable, Insufficient, Healthy, Warning };

struct AsioBufferAssessment {
    AsioBufferHealthState state = AsioBufferHealthState::Unavailable;
    std::uint64_t blockPeriodNs = 0;
    std::int64_t worstMarginNs = 0;
};

class AsioBufferHealthPolicy {
public:
    AsioBufferAssessment Assess(const AsioBufferWindow& window) noexcept {
        if (!window.sampleRate || !window.bufferFrames || !window.durationMs) {
            Reset();
            return {};
        }
        const auto period = static_cast<std::uint64_t>(window.bufferFrames) * 1000000000ULL /
            window.sampleRate;
        const auto margin = static_cast<std::int64_t>(period) -
            static_cast<std::int64_t>(window.worstProcessingNs);
        if (window.durationMs < 1000 || window.callbacks < 100) {
            incidentStreak_ = recoveryStreak_ = 0;
            return {AsioBufferHealthState::Insufficient, period, margin};
        }
        const auto incidents = window.delayedStarts > window.processingOverruns
            ? window.delayedStarts : window.processingOverruns;
        const auto threshold = window.callbacks / 100 + (window.callbacks % 100 != 0);
        if (incidents >= (threshold > 3 ? threshold : 3)) {
            if (incidentStreak_ < 3) ++incidentStreak_;
            recoveryStreak_ = 0;
            if (incidentStreak_ == 3) warning_ = true;
        } else {
            incidentStreak_ = 0;
            if (recoveryStreak_ < 3) ++recoveryStreak_;
            if (recoveryStreak_ == 3) warning_ = false;
        }
        return {warning_ ? AsioBufferHealthState::Warning : AsioBufferHealthState::Healthy,
            period, margin};
    }
    void Reset() noexcept { incidentStreak_ = recoveryStreak_ = 0; warning_ = false; }

private:
    unsigned incidentStreak_ = 0;
    unsigned recoveryStreak_ = 0;
    bool warning_ = false;
};
