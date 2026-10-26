#pragma once
#include "native_audio_transport.h"
#include <chrono>
#include <cstddef>

enum class NativeChannelCode { Ok, Unavailable, AccessDenied, Removed, Cancelled, TimedOut, Failed };
struct NativeChannelResult {
    NativeChannelCode code=NativeChannelCode::Failed;
    std::uint32_t bytes=0,systemError=0;
};
// Injectable authenticated-channel boundary, NOT an authentication implementation.
// Connect Ok requires peer authentication and binding to the observed Windows
// identity. ConfirmLease performs the plan's separate data-channel authentication
// and binding to the SESSION lease before any PCM operation. No clear fallback.
// Each call has an absolute steady deadline. On timeout/cancel it must cancel and
// observe completion BEFORE returning: no buffer access or retained pointer may
// survive a return. Exchange returns exactly one complete bounded application
// response, handles framing internally, and permits only one request outstanding.
// RequestCancel is sticky, bounded/nonblocking and the only concurrent method.
// Close is worker-only, releases the connection after all operations completed,
// is bounded and cannot reset cancellation. Destructor must not race any method.
class INativeAudioAuthenticatedChannel {
public:
    using Deadline=std::chrono::steady_clock::time_point;
    virtual ~INativeAudioAuthenticatedChannel()=default;
    virtual NativeChannelResult Connect(Deadline) noexcept=0;
    virtual NativeChannelResult ConfirmLease(std::uint64_t lease,std::uint64_t generation,Deadline) noexcept=0;
    virtual NativeChannelResult Exchange(const void* request,std::size_t requestBytes,
        void* response,std::size_t responseCapacity,Deadline) noexcept=0;
    virtual void RequestCancel() noexcept=0;
    virtual void Close() noexcept=0;
};

// Engine worker adapter; not used by a graph callback. Fixed buffers are prepared
// at construction. Missing channel reports Unavailable; never selects the direct
// laboratory transport. Production channel provisioning remains a separate lot.
// The channel object's address/lifetime stays fixed until adapter destruction.
// Destructor cancels first and closes without issuing a graceful wire CLOSE.
std::unique_ptr<INativeAudioTransport> CreateBrokerNativeAudioTransport(
    std::unique_ptr<INativeAudioAuthenticatedChannel> channel={});
