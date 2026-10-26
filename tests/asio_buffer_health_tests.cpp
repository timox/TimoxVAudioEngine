#include "asio_buffer_health.h"

#include <cstdio>

int main() {
    AsioBufferHealthPolicy policy;
    AsioBufferWindow sample{44100, 512, 5000, 430, 0, 0, 1000000};
    auto result = policy.Assess(sample);
    if (result.state != AsioBufferHealthState::Healthy ||
        result.blockPeriodNs != 11609977 || result.worstMarginNs != 10609977) return 1;

    sample.processingOverruns = 6;
    sample.worstProcessingNs = 12500000;
    if (policy.Assess(sample).state != AsioBufferHealthState::Healthy) return 2;
    if (policy.Assess(sample).state != AsioBufferHealthState::Healthy) return 3;
    if (policy.Assess(sample).state != AsioBufferHealthState::Warning) return 4;

    sample.processingOverruns = 0;
    sample.worstProcessingNs = 1000000;
    if (policy.Assess(sample).state != AsioBufferHealthState::Warning) return 5;
    if (policy.Assess(sample).state != AsioBufferHealthState::Warning) return 6;
    if (policy.Assess(sample).state != AsioBufferHealthState::Healthy) return 7;

    sample.callbacks = 10;
    if (policy.Assess(sample).state != AsioBufferHealthState::Insufficient) return 8;
    policy.Reset();
    sample.callbacks = 430;
    sample.delayedStarts = 6;
    if (policy.Assess(sample).state != AsioBufferHealthState::Healthy) return 9;
    if (policy.Assess(sample).state != AsioBufferHealthState::Healthy) return 10;
    if (policy.Assess(sample).state != AsioBufferHealthState::Warning) return 11;

    std::puts("PASS: buffer health requires repeated observed incidents and recovers with hysteresis.");
    return 0;
}
