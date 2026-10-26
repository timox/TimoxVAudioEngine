#include "asio_sample_conversion.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace {
bool near(float actual, float expected, float tolerance = 0.0001f) {
    return std::fabs(actual - expected) <= tolerance;
}

bool roundTrip(ASIOSampleType type, float value, float tolerance = 0.0001f) {
    std::array<unsigned char, 8> bytes{};
    AsioSampleConversion::Encode(bytes.data(), type, value);
    return near(AsioSampleConversion::Decode(bytes.data(), type), value, tolerance);
}
}

int main() {
    if (!roundTrip(ASIOSTInt16LSB, 0.5f) || !roundTrip(ASIOSTInt16MSB, -0.5f) ||
        !roundTrip(ASIOSTInt24LSB, 0.25f) || !roundTrip(ASIOSTInt24MSB, -0.25f) ||
        !roundTrip(ASIOSTInt32LSB, 0.75f) || !roundTrip(ASIOSTInt32MSB, -0.75f) ||
        !roundTrip(ASIOSTInt32LSB24, 0.5f) || !roundTrip(ASIOSTInt32MSB20, -0.5f) ||
        !roundTrip(ASIOSTInt16LSB, -1.0f) || !roundTrip(ASIOSTInt24MSB, -1.0f) ||
        !roundTrip(ASIOSTInt32LSB24, -1.0f) ||
        !roundTrip(ASIOSTFloat32LSB, 0.12345f) || !roundTrip(ASIOSTFloat32MSB, -0.12345f) ||
        !roundTrip(ASIOSTFloat64LSB, 0.12345f) || !roundTrip(ASIOSTFloat64MSB, -0.12345f)) {
        std::fprintf(stderr, "ASIO sample conversion round-trip failed.\n");
        return 1;
    }
    if (!AsioSampleConversion::IsSupported(ASIOSTInt32LSB24) ||
        AsioSampleConversion::IsSupported(ASIOSTDSDInt8LSB1)) {
        std::fprintf(stderr, "ASIO sample format support classification failed.\n");
        return 2;
    }
    std::array<unsigned char, 8> clipped{};
    AsioSampleConversion::Encode(clipped.data(), ASIOSTFloat64LSB, 1.5f);
    if (!near(AsioSampleConversion::Decode(clipped.data(), ASIOSTFloat64LSB), 1.0f)) return 3;
    std::puts("PASS: ASIO PCM conversion round-trips supported integer and float formats.");
    return 0;
}
