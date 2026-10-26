#pragma once

#include <cstdint>
#include <string>

struct AudioMeterSnapshot {
    std::string endpointId;
    float peakLinear = 0.0f;
    float shortTermLufs = -120.0f;
    bool shortTermLufsReady = false;
    std::uint64_t underruns = 0;
    std::uint64_t overruns = 0;
};

struct AudioCorrelationSnapshot {
    bool active = false;
    bool hasSignal = false;
    float correlation = 0.0f;
};
