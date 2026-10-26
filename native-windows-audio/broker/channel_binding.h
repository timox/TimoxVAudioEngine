#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace timox_broker {
// Untrusted wire identity until compared with an independently observed token.
struct ChannelIdentity {
    std::array<std::uint8_t,68> userSid{},logonSid{};
    std::uint16_t userSidBytes=0,logonSidBytes=0;
    std::uint64_t authenticationId=0;
    std::uint32_t sessionId=0;
};
enum class BindingType:std::uint16_t {
    BindRequest=1,BindReply=2,DataOffer=3,DataBindRequest=4,DataBindReply=5
};
struct ChannelBinding {
    BindingType type=BindingType::BindRequest;
    std::uint64_t lease=0,generation=0;
    ChannelIdentity identity{};
    std::array<std::uint8_t,16> nonce{};
};
constexpr std::size_t MaxBindingBytes=1024;
using BindingBuffer=std::array<std::uint8_t,MaxBindingBytes>;
// Failures preserve outputs. Exact lengths, fixed storage; no native struct casts.
bool EncodeBinding(const ChannelBinding&,BindingBuffer&,std::size_t&) noexcept;
bool DecodeBinding(const void*,std::size_t,ChannelBinding&) noexcept;
// Compare ALL identity fields, including authentication LUID (SessionId is reused).
bool SameChannelIdentity(const ChannelIdentity&,const ChannelIdentity&) noexcept;
bool MatchesBinding(const ChannelBinding& received,const ChannelBinding& expected) noexcept;
}
