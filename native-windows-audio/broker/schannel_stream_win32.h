#pragma once
#include "byte_stream.h"
#include <Windows.h>
#include <wincrypt.h>
#include <array>

namespace timox_broker {
namespace detail {
// Completion accounting, separate from scheduling: a timeout may lose its
// cancellation race and still consume pipe bytes. Retain those bytes before
// returning a retryable timeout. The channel never reports ciphertext as bytes
// of plaintext delivered to its caller.
inline bool RetainCiphertextCompletion(NativeChannelResult& result,
    std::size_t capacity,std::size_t& buffered) noexcept {
    if(buffered>capacity || result.bytes>capacity-buffered)return false;
    if(result.code==NativeChannelCode::Ok || result.code==NativeChannelCode::TimedOut)
        buffered+=result.bytes;
    if(result.code!=NativeChannelCode::Ok)result.bytes=0;
    return true;
}
}
// TLS on an already-connected byte stream. This does NOT bind the certificate
// to the observed Windows logon; channel binding remains a subsequent gate.
// Worker-only except sticky RequestCancel; callers must quiesce before destruction.
// Own certificate context is duplicated; its backing key must remain available.
class SchannelStream final {
public:
    using Deadline=ByteStream::Deadline;
    using Pin=std::array<unsigned char,32>;
    // TLS 1.2 records: <=16 KiB plaintext, <=18 KiB ciphertext + 5-byte
    // header. Provisioned handshake flights must fit the 32 KiB profile.
    // Larger chains/tokens are rejected; buffers never grow dynamically.
    static constexpr std::size_t CipherBufferBytes=32768;
    static constexpr std::size_t PlainBufferBytes=16384;
    static constexpr std::size_t OutputBufferBytes=18437;
    static constexpr std::size_t MaxHandshakeTokenBytes=CipherBufferBytes;
    // Includes this object and Impl (not OS/Schannel allocations or transport).
    static std::size_t RetainedApplicationBytes() noexcept;
    SchannelStream(std::unique_ptr<ByteStream>,bool server,PCCERT_CONTEXT ownCertificate,const Pin& expectedPeer);
    ~SchannelStream();
    SchannelStream(const SchannelStream&)=delete;
    SchannelStream& operator=(const SchannelStream&)=delete;
    NativeChannelResult Handshake(Deadline) noexcept;
    NativeChannelResult ReadSome(void*,std::size_t capacity,Deadline) noexcept;
    NativeChannelResult WriteAll(const void*,std::size_t bytes,Deadline) noexcept;
    void RequestCancel() noexcept;
    void Close() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
