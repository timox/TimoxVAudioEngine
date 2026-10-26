#pragma once
#include "native_audio_broker_transport.h"

namespace timox_broker {
// Connected raw stream beneath TLS, not an authentication boundary. Worker-only
// except sticky RequestCancel. No retained caller buffers after a call returns.
// Write failures report the prefix sent; read failures may report consumed bytes.
// Absolute deadlines; caller joins operations before Close/destruction.
class ByteStream {
public:
    using Deadline=INativeAudioAuthenticatedChannel::Deadline;
    static constexpr std::size_t MaxTransferBytes=256u*1024u;
    virtual ~ByteStream()=default;
    virtual NativeChannelResult ReadSome(void*,std::size_t,Deadline) noexcept=0;
    virtual NativeChannelResult WriteAll(const void*,std::size_t,Deadline) noexcept=0;
    virtual void RequestCancel() noexcept=0;
    virtual void Close() noexcept=0;
};
}
