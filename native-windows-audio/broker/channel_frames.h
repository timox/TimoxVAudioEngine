#pragma once
#include "channel_binding.h"
#include "native_audio_broker_transport.h"
#include "native_audio_broker_protocol.h"
#include <cstring>

namespace timox_broker {
// Worker-only framing over SchannelStream. Stream is a template solely to let
// tests inject fragment boundaries; production supplies the authenticated TLS
// stream, never a raw pipe. No retained caller memory or audio allocation.
// Any failure closes the stream: retrying a partially consumed frame would
// desynchronize it. Semantic audio validation and admission remain the caller's.
namespace frames_detail {
template<class Stream>
NativeChannelResult Exact(Stream& stream,unsigned char* buffer,std::size_t bytes,
    INativeAudioAuthenticatedChannel::Deadline deadline) noexcept {
    std::size_t offset=0;
    while(offset<bytes){
        if(std::chrono::steady_clock::now()>=deadline)return {NativeChannelCode::TimedOut};
        auto result=stream.ReadSome(buffer+offset,bytes-offset,deadline);
        if(result.code!=NativeChannelCode::Ok){result.bytes=0;return result;}
        if(!result.bytes||result.bytes>bytes-offset)return {NativeChannelCode::Failed};
        offset+=result.bytes;
    }
    return {NativeChannelCode::Ok,static_cast<std::uint32_t>(bytes)};
}
inline std::uint64_t Read(const unsigned char* p,std::size_t at,unsigned width) noexcept {
    return timox_native::detail::ReadLittleEndian(p,at,width);
}
}
template<class Stream>
NativeChannelResult ReadAudioFrame(Stream& stream,void* output,std::size_t capacity,
    INativeAudioAuthenticatedChannel::Deadline deadline) noexcept {
    const bool bounded=output&&capacity<=MaxMessageBytes;
    const auto fail=[&](NativeChannelResult result){
        stream.Close();if(bounded)std::memset(output,0,capacity);result.bytes=0;return result;
    };
    if(!bounded||capacity<HeaderBytes)return fail({NativeChannelCode::Failed});
    auto* bytes=static_cast<unsigned char*>(output);
    auto result=frames_detail::Exact(stream,bytes,HeaderBytes,deadline);
    if(result.code!=NativeChannelCode::Ok)return fail(result);
    using frames_detail::Read;
    const auto payload=Read(bytes,12,4),opcode=Read(bytes,6,2)&0x7fff;
    if(Read(bytes,0,4)!=Magic||Read(bytes,4,2)!=1||Read(bytes,8,4)!=HeaderBytes||
       opcode<1||opcode>5||Read(bytes,36,4)||Read(bytes,40,8)||
       payload>MaxPayloadBytes||payload>capacity-HeaderBytes)return fail({NativeChannelCode::Failed});
    result=frames_detail::Exact(stream,bytes+HeaderBytes,static_cast<std::size_t>(payload),deadline);
    if(result.code!=NativeChannelCode::Ok)return fail(result);
    return {NativeChannelCode::Ok,static_cast<std::uint32_t>(HeaderBytes+payload)};
}
template<class Stream>
NativeChannelResult ReadBindingFrame(Stream& stream,ChannelBinding& output,
    INativeAudioAuthenticatedChannel::Deadline deadline) noexcept {
    BindingBuffer wire{};
    const auto fail=[&](NativeChannelResult result){stream.Close();result.bytes=0;return result;};
    auto result=frames_detail::Exact(stream,wire.data(),16,deadline);
    if(result.code!=NativeChannelCode::Ok)return fail(result);
    using frames_detail::Read;
    const auto payload=Read(wire.data(),8,4),type=Read(wire.data(),6,2);
    if(wire[0]!='T'||wire[1]!='N'||wire[2]!='A'||wire[3]!='C'||Read(wire.data(),4,2)!=1||
       type<1||type>5||Read(wire.data(),12,4)||payload>MaxBindingBytes-16)return fail({NativeChannelCode::Failed});
    result=frames_detail::Exact(stream,wire.data()+16,static_cast<std::size_t>(payload),deadline);
    if(result.code!=NativeChannelCode::Ok)return fail(result);
    if(!DecodeBinding(wire.data(),16+static_cast<std::size_t>(payload),output))return fail({NativeChannelCode::Failed});
    return {NativeChannelCode::Ok,static_cast<std::uint32_t>(16+payload)};
}
template<class Stream>
NativeChannelResult WriteBindingFrame(Stream& stream,const ChannelBinding& binding,
    INativeAudioAuthenticatedChannel::Deadline deadline) noexcept {
    BindingBuffer wire{};std::size_t bytes=0;
    if(!EncodeBinding(binding,wire,bytes)){stream.Close();return {NativeChannelCode::Failed};}
    if(std::chrono::steady_clock::now()>=deadline){stream.Close();return {NativeChannelCode::TimedOut};}
    auto result=stream.WriteAll(wire.data(),bytes,deadline);
    if(result.code!=NativeChannelCode::Ok||result.bytes!=bytes){
        stream.Close();if(result.code==NativeChannelCode::Ok)result.code=NativeChannelCode::Failed;result.bytes=0;
    }
    return result;
}
}
