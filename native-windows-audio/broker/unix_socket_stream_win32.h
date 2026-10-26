#pragma once
#include "native_audio_broker_transport.h"
#include "byte_stream.h"

namespace timox_broker {
// Raw AF_UNIX SOCK_STREAM transport, not authentication. Host owns the protected
// directory, endpoint discovery and removal of its socket path after shutdown.
// Never unlinks or takes over an existing pathname. No network fallback.
// Worker-only except RequestCancel. Cancellation is sticky; destruction/Close
// must not race calls. Nonblocking I/O retains no caller buffer after return.
// Waits poll cancellation every <=10 ms, subject to OS scheduling; not for RT.
class UnixSocketStream final : public ByteStream {
public:
    using Deadline=INativeAudioAuthenticatedChannel::Deadline;
    UnixSocketStream();
    ~UnixSocketStream() override;
    // Object + Impl, excluding Winsock/kernel allocations and caller buffers.
    static std::size_t FixedStorageBytes() noexcept;
    UnixSocketStream(const UnixSocketStream&)=delete;
    UnixSocketStream& operator=(const UnixSocketStream&)=delete;
    NativeChannelResult Listen(const char* path) noexcept;
    // Listener remains open; destination must be unused/closed. To interrupt
    // Accept, cancel the listener; destination cancellation is checked on entry
    // and after accept, not while waiting for a connection.
    NativeChannelResult Accept(UnixSocketStream& destination,Deadline) noexcept;
    NativeChannelResult Connect(const char* path,Deadline) noexcept;
    // Kernel-reported peer PID; output unchanged on failure. This is an
    // observation, NOT authorization: it does not pin a process lifetime/token
    // or establish which process currently owns a transferred socket handle.
    NativeChannelResult GetPeerProcessId(std::uint32_t& output) noexcept;
    NativeChannelResult ReadSome(void*,std::size_t,Deadline) noexcept override;
    // Failure bytes is the prefix already sent; never retry the whole message.
    NativeChannelResult WriteAll(const void*,std::size_t,Deadline) noexcept override;
    void RequestCancel() noexcept override;
    void Close() noexcept override;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
