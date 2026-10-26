#pragma once
#include <cstdint>
#include <memory>

enum class NativeTransferCode { Ok, Empty, Full, Unavailable, AccessDenied,
    OwnedElsewhere, Removed, Cancelled, InvalidPacket, Failed };
struct NativeTransferResult {
    NativeTransferCode code = NativeTransferCode::Failed;
    std::uint32_t bytes = 0;
    std::uint32_t systemError = 0;
};
struct NativeSessionInfo { std::uint64_t wireGeneration = 0; };
// AccessDenied retains operation context: Open may be denied by the device ACL;
// ReadRender may also be denied by N2 protected-audio policy. Win32 alone cannot
// distinguish the reason. Do not map denial to driver absence.

// Worker-owned duplex session, fixed N0 format: 48000 Hz, stereo signed PCM32.
// No method belongs in a graph callback. RequestCancel is the only concurrent
// operation; it is sticky for this instance, including an Open in progress.
// Close/Open may reuse an uncancelled instance. Create a new instance after
// cancellation. Caller keeps the object alive until worker calls have returned;
// destructor/Close must never race another call. Wire generations may repeat
// after driver replacement: the backend must also assign its own local epoch.
class INativeAudioTransport {
public:
    virtual ~INativeAudioTransport() = default;
    virtual NativeTransferResult Open(NativeSessionInfo&) noexcept = 0;
    virtual NativeTransferResult ReadRender(std::uint32_t maxFrames, void* packet, std::uint32_t capacity) noexcept = 0;
    virtual NativeTransferResult WriteCapture(const void* packet, std::uint32_t bytes) noexcept = 0;
    virtual NativeTransferResult ReadStatus(std::uint64_t (&counters)[3]) noexcept = 0;
    virtual void RequestCancel() noexcept = 0;
    virtual void Close() noexcept = 0;
};

// Explicit laboratory adapter only. Never elevates, changes ACLs, installs a
// driver, or falls back to another device. N4 production needs a broker adapter.
std::unique_ptr<INativeAudioTransport> CreateDirectNativeAudioLabTransport();
