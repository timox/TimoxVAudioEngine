#pragma once
#include <cstddef>
#include <cstdint>
#include "native_audio_packet_validation.h"

// Audio messages ONLY AFTER mutual authentication and identity/lease binding.
// This codec supplies no authentication, authorization, TLS, framing accumulator,
// session creation, packet-sequence tracking or replay-state mutation. The caller
// supplies trusted context and commits request history only after acceptance.
// Buffers must be stable/readable for the call; returned payload is borrowed.
namespace timox_broker {
constexpr std::uint32_t Magic = 0x42415654; // ASCII TVAB
constexpr std::size_t HeaderBytes = 48, MaxPayloadBytes = 32832;
constexpr std::size_t MaxMessageBytes = HeaderBytes + MaxPayloadBytes;
enum class Opcode : std::uint16_t { Session=1, ReadRender=2, WriteCapture=3, Status=4, Close=5 };
// Broker-domain wire values, deliberately independent of Win32 and N3 enums.
enum class Status : std::uint32_t { Ok=0, Empty=1, Full=2, Unavailable=3,
    AccessDenied=4, OwnedElsewhere=5, Removed=6, Cancelled=7,
    InvalidPacket=8, ProtocolMismatch=9, Protected=10, Failed=11 };
enum class Error { None, Truncated, Header, Version, Opcode, Size, Reserved,
    Status, RequestId, Lease, Context, Payload, Packet };
struct Context {
    bool response=false;
    Opcode opcode=Opcode::Session; // Expected operation for requests and responses.
    std::uint64_t requestId=0; // Exact outstanding ID for responses.
    std::uint64_t lastRequestId=0; // Requests must be strictly greater, no wrap.
    std::uint64_t leaseId=0;
    std::uint64_t generation=0;
    std::uint32_t maxFrames=4096;
};
struct MessageView {
    Opcode opcode=Opcode::Session;
    Status status=Status::Ok;
    std::uint64_t requestId=0,leaseId=0;
    const unsigned char* payload=nullptr;
    std::uint32_t payloadBytes=0;
};
struct Result { Error error; MessageView message; };

// v1 header offsets: magic u32@0, version u16@4, opcode u16@6 (bit15
// means response), headerBytes u32@8, payloadBytes u32@12, requestId u64@16,
// leaseId u64@24, status u32@32, zero u32@36, zero u64@40.
// Request/success-response payloads:
// SESSION: empty / generation u64; request lease=0, success lease!=0.
// READ_RENDER: generation u64,maxFrames u32,zero u32 / N0 render packet.
// WRITE_CAPTURE: N0 capture packet / empty.
// STATUS: generation u64 / three u64 counters (renderDropped,captureDropped,
// captureSilence). CLOSE: generation u64 / empty.
// Every error response has exactly systemError u32 (no strings). SESSION errors
// carry lease=0; all other errors echo the request lease, including Removed.
// Request status must be Ok. Non-Ok response statuses are syntax only: whether
// e.g. Empty is meaningful for an operation is the session handler's decision.
inline Result ValidateMessage(const void* buffer,std::size_t bytes,const Context& c) noexcept {
    if(!buffer || bytes<HeaderBytes)return {Error::Truncated,{}};
    const auto* b=static_cast<const unsigned char*>(buffer);
    const auto read=[b](std::size_t at,unsigned width) noexcept {
        return timox_native::detail::ReadLittleEndian(b,at,width);
    };
    if(read(0,4)!=Magic || read(8,4)!=HeaderBytes)return {Error::Header,{}};
    if(read(4,2)!=1)return {Error::Version,{}};
    const auto wireOp=read(6,2),op=wireOp&0x7fff;
    if(op<1 || op>5 || ((wireOp&0x8000)!=0)!=c.response || op!=static_cast<unsigned>(c.opcode))
        return {Error::Opcode,{}};
    const auto payloadBytes=read(12,4);
    if(payloadBytes>MaxPayloadBytes || bytes-HeaderBytes!=payloadBytes)return {Error::Size,{}};
    if(read(36,4) || read(40,8))return {Error::Reserved,{}};
    const auto status=read(32,4),id=read(16,8),lease=read(24,8);
    if(status>static_cast<unsigned>(Status::Failed) || (!c.response && status))return {Error::Status,{}};
    if(!id || (c.response ? (!c.requestId || id!=c.requestId) : id<=c.lastRequestId))return {Error::RequestId,{}};
    const bool session=op==static_cast<unsigned>(Opcode::Session);
    if(session) {
        if(c.response && !status ? !lease : lease!=0)return {Error::Lease,{}};
    } else {
        if(!c.leaseId || !c.generation || !c.maxFrames || c.maxFrames>4096)return {Error::Context,{}};
        if(lease!=c.leaseId)return {Error::Lease,{}};
    }
    if(c.response && status) {
        if(payloadBytes!=4)return {Error::Payload,{}};
    } else if(session) {
        if(c.response ? (payloadBytes!=8 || !read(48,8)) : payloadBytes!=0)return {Error::Payload,{}};
    } else if((c.response && c.opcode==Opcode::ReadRender) || (!c.response && c.opcode==Opcode::WriteCapture)) {
        const timox_native::PacketContext pc{c.generation,
            static_cast<timox_packet_u16>(c.response?TIMOX_NATIVE_FROM_WINDOWS_RENDER:TIMOX_NATIVE_TO_WINDOWS_CAPTURE),c.maxFrames};
        if(timox_native::ValidatePacket(b+HeaderBytes,static_cast<std::size_t>(payloadBytes),pc).error!=timox_native::PacketError::None)
            return {Error::Packet,{}};
    } else if(c.response) {
        if(payloadBytes!=(c.opcode==Opcode::Status?24u:0u))return {Error::Payload,{}};
    } else {
        if(payloadBytes!=(c.opcode==Opcode::ReadRender?16u:8u) || read(48,8)!=c.generation)return {Error::Payload,{}};
        if(c.opcode==Opcode::ReadRender && (!read(56,4) || read(56,4)>c.maxFrames || read(60,4)))return {Error::Payload,{}};
    }
    return {Error::None,{c.opcode,static_cast<Status>(status),id,lease,b+HeaderBytes,static_cast<std::uint32_t>(payloadBytes)}};
}
} // namespace timox_broker
