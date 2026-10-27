#pragma once

#include "wasapi_device_enumerator.h"
#include <Windows.h>
#include <cstdint>
#include <memory>

enum class WasapiStreamKind { Capture, Render, EndpointLoopback, ProcessLoopback };

struct WasapiOpenConfig {
    WasapiStreamKind kind = WasapiStreamKind::Capture;
    std::wstring endpointId;
    std::optional<WasapiDefaultRole> defaultRole;
    std::uint32_t processId = 0;
    std::uint64_t processCreationTime = 0;
    std::wstring processImagePath;
    std::int64_t bufferDuration100ns = 0; // 0 requests the shared engine default; maximum one second.
};

struct WasapiStreamInfo {
    std::wstring endpointId;
    WasapiStreamKind kind = WasapiStreamKind::Capture;
    WasapiMixFormat format;
    std::uint32_t bufferFrames = 0;
    std::int64_t streamLatency100ns = 0;
    std::int64_t defaultPeriod100ns = 0;
    std::int64_t minimumPeriod100ns = 0;
};

struct WasapiCapturePacket {
    const std::uint8_t* data = nullptr; // May be null for silence; never dereference a silent packet.
    std::uint32_t frames = 0;
    bool silent = false;
    bool discontinuity = false;
    bool timestampError = false;
    std::uint64_t devicePosition = 0;
    std::uint64_t qpcPosition100ns = 0; // WASAPI converts QPC ticks into 100 ns units.
};

enum class WasapiWaitKind { Audio, Stop, Timeout, Failed };
struct WasapiWaitResult {
    WasapiWaitKind kind = WasapiWaitKind::Failed;
    HRESULT status = E_FAIL;
};

// Bounds both native format and allocation size: <=256 channels, 8..384 kHz,
// <=one second and <=64 MiB of endpoint buffering. No allocation or COM calls.
HRESULT ValidateWasapiStreamFormat(const WasapiMixFormat& format, std::uint32_t bufferFrames) noexcept;

// Injectable packet interface. Every method, including destruction, belongs to
// ONE worker thread whose COM apartment remains initialized throughout the open
// lifetime. Never call from the graph callback. No concurrent Close/Wait or
// packet operations: signal the caller-owned stop event, join the worker first.
// The backend owns the audio event; the caller owns and keeps stopEvent alive.
// Streaming methods do not allocate C++ storage, throw, log or convert samples;
// COM and waits are worker operations, not realtime callback-safe operations.
class IWasapiBackend {
public:
    virtual ~IWasapiBackend() = default;
    // Device streams require exactly one endpointId/defaultRole; endpoint
    // loopback resolves RENDER. Process loopback requires a PID, creation time
    // and image path, verified before and after async activation. Start/read is
    // forbidden when either identity check fails. A second Open preserves the
    // existing stream (AUDCLNT_E_ALREADY_INITIALIZED).
    // Resolver failures map to E_FAIL because the enumerator returns bool;
    // OpenErrorDetail preserves its reason. Subsequent COM failures keep HRESULT.
    virtual HRESULT Open(const WasapiOpenConfig& config) noexcept = 0;
    // Setup diagnostic, read on the owning worker before Close/destruction.
    // Empty when no additional detail is available; reset by the next Open.
    virtual const char* OpenErrorDetail() const noexcept { return ""; }
    // Immutable effective native mix format and buffer metadata until Close.
    virtual const WasapiStreamInfo* Info() const noexcept = 0;
    virtual HRESULT Start() noexcept = 0;
    // Start/Stop reject an outstanding packet with AUDCLNT_E_OUT_OF_ORDER;
    // release or cancel it first. Stop is otherwise idempotent when stopped.
    virtual HRESULT Stop() noexcept = 0;
    virtual void Close() noexcept = 0; // Idempotent; abandons capture, silences held render packets.
    virtual WasapiWaitResult Wait(HANDLE stopEvent, DWORD timeoutMs) noexcept = 0;

    virtual HRESULT NextCapturePacketFrames(std::uint32_t& frames) noexcept = 0;
    // S_OK with frames>0 acquires a packet, valid until ReleaseCapture. Empty
    // capture returns AUDCLNT_S_BUFFER_EMPTY and no lease. Errors clear output.
    virtual HRESULT AcquireCapture(WasapiCapturePacket& packet) noexcept = 0;
    // Read the entire packet or zero frames (retry later), never a partial packet.
    // Invalid frame counts preserve the lease so callers can release correctly.
    virtual HRESULT ReleaseCapture(std::uint32_t framesRead) noexcept = 0;

    virtual HRESULT RenderWritableFrames(std::uint32_t& frames) noexcept = 0;
    virtual HRESULT AcquireRender(std::uint32_t frames, std::uint8_t*& data) noexcept = 0;
    // Release the requested frame count or zero (cancel). Silent submits native
    // silence without touching the buffer, including unsigned PCM encodings.
    // Invalid frame counts preserve the lease so callers can release correctly.
    virtual HRESULT ReleaseRender(std::uint32_t framesWritten, bool silent) noexcept = 0;
};

// Control/worker setup allocation only. Windows shared-mode implementation uses
// IAudioClient, native GetMixFormat, EVENTCALLBACK and endpoint LOOPBACK only.
// No automatic conversion, exclusive mode, IAudioClient3 or graph ownership.
std::unique_ptr<IWasapiBackend> CreateWasapiBackend();
