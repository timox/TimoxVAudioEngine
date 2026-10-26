#include "wasapi_device_enumerator.h"
#include <Windows.h>
#include <mmreg.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static WAVEFORMATEX Format(WORD tag, WORD bits) {
    WAVEFORMATEX f{};
    f.wFormatTag = tag; f.nChannels = 2; f.nSamplesPerSec = 48000;
    f.wBitsPerSample = bits; f.nBlockAlign = 2 * bits / 8;
    f.nAvgBytesPerSec = 48000 * f.nBlockAlign;
    return f;
}
static WAVEFORMATEXTENSIBLE Extended(WORD tag, WORD bits, WORD valid) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format = Format(WAVE_FORMAT_EXTENSIBLE, bits);
    f.Format.cbSize = 22; f.Samples.wValidBitsPerSample = valid;
    f.dwChannelMask = 3;
    f.SubFormat = {tag, 0, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    return f;
}
static void ParserTests() {
    WasapiMixFormat parsed;
    std::string error;
    for (WORD bits : {WORD{8}, WORD{16}, WORD{24}, WORD{32}}) {
        const auto f = Format(WAVE_FORMAT_PCM, bits);
        Require(ParseWasapiMixFormat(&f, sizeof(f), parsed, error), "PCM rejected");
        Require(parsed.sampleRate == 48000 && parsed.channels == 2 && parsed.containerBits == bits &&
            parsed.validBits == bits && parsed.blockAlign == bits / 4 &&
            parsed.encoding == WasapiSampleEncoding::PcmInteger && error.empty(), "PCM fields");
    }
    for (WORD bits : {WORD{32}, WORD{64}}) {
        const auto f = Format(WAVE_FORMAT_IEEE_FLOAT, bits);
        Require(ParseWasapiMixFormat(&f, sizeof(f), parsed, error) &&
            parsed.encoding == WasapiSampleEncoding::IeeeFloat && parsed.containerBits == bits, "float rejected");
    }
    auto ext = Extended(WAVE_FORMAT_PCM, 32, 24);
    Require(ParseWasapiMixFormat(&ext, sizeof(ext), parsed, error) && parsed.validBits == 24 &&
        parsed.containerBits == 32 && parsed.channelMask == 3, "24 valid bits in 32-bit container");
    ext = Extended(WAVE_FORMAT_IEEE_FLOAT, 32, 32);
    Require(ParseWasapiMixFormat(&ext, sizeof(ext), parsed, error) &&
        parsed.encoding == WasapiSampleEncoding::IeeeFloat, "extensible float");
    unsigned char unaligned[sizeof(ext) + 1]{};
    std::memcpy(unaligned + 1, &ext, sizeof(ext));
    Require(ParseWasapiMixFormat(unaligned + 1, sizeof(ext), parsed, error), "unaligned input");
    for (std::size_t n = 0; n < sizeof(ext); ++n)
        Require(!ParseWasapiMixFormat(&ext, n, parsed, error), "truncated format accepted");
    Require(!ParseWasapiMixFormat(nullptr, sizeof(ext), parsed, error), "null input accepted");
    const auto reject = [&](const auto& invalid) {
        Require(!ParseWasapiMixFormat(&invalid, sizeof(invalid), parsed, error) &&
            parsed.sampleRate == 0 && !error.empty(), "invalid format accepted or stale output");
    };
    auto f = Format(WAVE_FORMAT_PCM, 16);
    f.nChannels = 0; reject(f);
    f = Format(WAVE_FORMAT_PCM, 16); f.nSamplesPerSec = 0; reject(f);
    f = Format(WAVE_FORMAT_PCM, 16); f.nBlockAlign = 3; reject(f);
    f = Format(WAVE_FORMAT_PCM, 16); f.nAvgBytesPerSec = 1; reject(f);
    f = Format(WAVE_FORMAT_PCM, 12); reject(f);
    f = Format(WAVE_FORMAT_IEEE_FLOAT, 16); reject(f);
    f = Format(WAVE_FORMAT_ADPCM, 16); reject(f);
    f = Format(WAVE_FORMAT_PCM, 16); f.cbSize = 1; reject(f);
    f = Format(WAVE_FORMAT_PCM, 16); f.nSamplesPerSec = 0xffffffffu; reject(f);
    ext = Extended(WAVE_FORMAT_PCM, 32, 33); reject(ext);
    ext = Extended(WAVE_FORMAT_PCM, 32, 0); reject(ext);
    ext = Extended(WAVE_FORMAT_IEEE_FLOAT, 32, 24); reject(ext);
    ext = Extended(WAVE_FORMAT_PCM, 16, 16); ext.Format.cbSize = 21; reject(ext);
    ext = Extended(WAVE_FORMAT_PCM, 16, 16); ext.SubFormat.Data4[0] = 0; reject(ext);
    ext = Extended(WAVE_FORMAT_PCM, 16, 16); ext.dwChannelMask = 1; reject(ext);
    ext.dwChannelMask = 0;
    Require(ParseWasapiMixFormat(&ext, sizeof(ext), parsed, error), "unspecified channel mask rejected");
}

int main(int argc, char** argv) {
    try {
        ParserTests();
        WasapiDeviceEnumerator enumerator;
        std::wstring id = L"stale";
        std::string error;
        Require(!enumerator.resolve(WasapiDataFlow::Render, L"", std::nullopt, id, error) && id.empty(),
            "missing selector must fail without COM");
        Require(!enumerator.resolve(WasapiDataFlow::Render, L"id", WasapiDefaultRole::Console, id, error),
            "ambiguous selector must fail");
        Require(!enumerator.resolve(static_cast<WasapiDataFlow>(99), L"id", std::nullopt, id, error),
            "invalid flow must fail without COM");
        Require(!enumerator.resolve(WasapiDataFlow::Render, L"", static_cast<WasapiDefaultRole>(99), id, error),
            "invalid role must fail without COM");
        std::vector<WasapiDeviceInfo> stale(1);
        Require(!enumerator.enumerate(stale, error) && stale.empty() && !error.empty(),
            "uninitialized caller COM must fail and clear stale inventory");
        if (argc > 1 && std::strcmp(argv[1], "--probe") == 0) {
            // Explicit opt-in environmental observation, not a device-presence test.
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Require(SUCCEEDED(hr), "probe COM initialization failed");
            struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
            std::vector<WasapiDeviceInfo> devices;
            if (!enumerator.enumerate(devices, error)) {
                std::cout << "Inventory unavailable: " << error << '\n';
            } else {
                std::size_t capture = 0, render = 0, active = 0, formats = 0;
                for (const auto& device : devices) {
                    (device.flow == WasapiDataFlow::Capture ? capture : render)++;
                    if (device.state & 1) ++active;
                    if (device.mixFormat) ++formats;
                }
                std::cout << "Observed endpoints: " << devices.size() << "; capture=" << capture
                    << "; render=" << render << "; active=" << active << "; mixFormats=" << formats << '\n';
                for (auto flow : {WasapiDataFlow::Capture, WasapiDataFlow::Render}) {
                    for (auto role : {WasapiDefaultRole::Console, WasapiDefaultRole::Multimedia, WasapiDefaultRole::Communications}) {
                        if (enumerator.resolve(flow, L"", role, id, error)) {
                            std::wstring explicitResult;
                            const bool available = enumerator.resolve(flow, id, std::nullopt, explicitResult, error);
                            std::cout << "Default role " << static_cast<int>(role) << " flow " << static_cast<int>(flow)
                                << ": resolved; explicit ID " << (available && explicitResult == id ? "confirmed" : "changed/unavailable") << '\n';
                        } else std::cout << "Default unavailable: " << error << '\n';
                    }
                }
            }
        }
        std::cout << "WASAPI enumerator deterministic tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
