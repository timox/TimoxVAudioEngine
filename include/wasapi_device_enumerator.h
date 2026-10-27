#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class WasapiDataFlow { Capture, Render };
enum class WasapiDefaultRole { Console, Multimedia, Communications };
enum class WasapiSampleEncoding { PcmInteger, IeeeFloat };

struct WasapiMixFormat {
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::uint16_t containerBits = 0;
    std::uint16_t validBits = 0;
    std::uint16_t blockAlign = 0;
    std::uint32_t channelMask = 0; // Zero means unspecified speaker positions.
    WasapiSampleEncoding encoding = WasapiSampleEncoding::PcmInteger;
};

struct WasapiDeviceInfo {
    std::wstring id; // Opaque Windows endpoint ID; never derive an ID from its name.
    std::wstring name;
    std::wstring containerId; // Windows PKEY_Device_ContainerId; empty when the driver does not publish one.
    WasapiDataFlow flow = WasapiDataFlow::Render;
    std::uint32_t state = 0; // DEVICE_STATE_* flags from the Windows endpoint.
    std::optional<WasapiMixFormat> mixFormat;
    std::string formatError; // Inactive, unsupported or invalidated during discovery.
};

struct WasapiProcessInfo {
    std::uint32_t processId = 0;
    std::uint64_t creationTime = 0; // FILETIME ticks, serialized as decimal text.
    std::wstring imagePath;
    std::wstring displayName;
    std::vector<std::wstring> renderEndpoints;
};

// Bounded WAVEFORMATEX/WAVEFORMATEXTENSIBLE parsing, no COM or hardware access.
// Accepts integer PCM (8/16/24/32-bit containers), IEEE float (32/64-bit).
// The input may be unaligned. Output is cleared on failure.
bool ParseWasapiMixFormat(const void* bytes, std::size_t size,
    WasapiMixFormat& result, std::string& error);

// Control/worker thread ONLY: may allocate and make blocking COM calls.
// Caller owns a COM apartment initialized on this same thread throughout each
// call. All acquired interfaces and COM allocations are released before return.
// No COM interface is shared with a graph callback, and no stream is started.
class WasapiDeviceEnumerator {
public:
    // Includes capture/render endpoints in all states. A missing mix format is
    // an endpoint-level observation, not a failure of the entire inventory.
    bool enumerate(std::vector<WasapiDeviceInfo>& devices, std::string& error) const;
    // Active single-process Windows render sessions only. This reports use of
    // the Windows shared audio engine, not which API the application called.
    bool enumerateActiveRenderProcesses(std::vector<WasapiProcessInfo>& processes,
        std::string& error) const;
    // Exactly one of a nonempty explicitId or defaultRole must be supplied.
    // Resolution verifies flow and active state; never silently falls back.
    bool resolve(WasapiDataFlow flow, const std::wstring& explicitId,
        std::optional<WasapiDefaultRole> defaultRole, std::wstring& resolvedId,
        std::string& error) const;
};
