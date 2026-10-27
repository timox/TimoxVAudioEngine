#include "channel_binding.h"
#include <algorithm>
namespace timox_broker {
namespace {
template<std::size_t N> bool Zero(const std::array<std::uint8_t,N>& a) noexcept {
    return std::all_of(a.begin(),a.end(),[](std::uint8_t v){return v==0;});
}
bool ValidSid(const std::array<std::uint8_t,68>& sid,std::uint16_t bytes) noexcept {
    return bytes>=8&&bytes<=sid.size()&&sid[0]==1&&sid[1]<=15&&bytes==8u+4u*sid[1];
}
bool ValidIdentity(const ChannelIdentity& i) noexcept {
    return ValidSid(i.userSid,i.userSidBytes)&&ValidSid(i.logonSid,i.logonSidBytes)&&
        i.logonSidBytes==20&&i.logonSid[1]==3&&
        i.logonSid[2]==0&&i.logonSid[3]==0&&i.logonSid[4]==0&&i.logonSid[5]==0&&
        i.logonSid[6]==0&&i.logonSid[7]==5&&i.logonSid[8]==5&&i.logonSid[9]==0&&
        i.logonSid[10]==0&&i.logonSid[11]==0&&i.authenticationId!=0&&
        i.sessionId!=0&&i.sessionId!=0xffffffffu;
}
bool EmptyIdentity(const ChannelIdentity& i) noexcept {
    return i.userSidBytes==0&&i.logonSidBytes==0&&i.authenticationId==0&&i.sessionId==0&&
        Zero(i.userSid)&&Zero(i.logonSid);
}
bool Valid(const ChannelBinding& b) noexcept {
    switch(b.type){
    case BindingType::BindRequest:case BindingType::BindReply:
        return b.lease==0&&b.generation==0&&Zero(b.nonce)&&ValidIdentity(b.identity);
    case BindingType::DataOffer:
        return b.lease!=0&&b.generation!=0&&!Zero(b.nonce)&&EmptyIdentity(b.identity);
    case BindingType::DataBindRequest:case BindingType::DataBindReply:
        return b.lease!=0&&b.generation!=0&&!Zero(b.nonce)&&ValidIdentity(b.identity);
    default:return false;
    }
}
void Put(std::uint8_t* p,std::uint64_t value,unsigned count) noexcept {
    for(unsigned n=0;n<count;++n){p[n]=static_cast<std::uint8_t>(value&0xff);value>>=8;}
}
std::uint64_t Get(const std::uint8_t* p,unsigned count) noexcept {
    std::uint64_t value=0;for(unsigned n=0;n<count;++n)value|=std::uint64_t{p[n]}<<(8*n);return value;
}
}
bool EncodeBinding(const ChannelBinding& b,BindingBuffer& output,std::size_t& size) noexcept {
    if(!Valid(b))return false;
    BindingBuffer wire{};
    const bool offer=b.type==BindingType::DataOffer;
    const std::size_t length=offer?48:64u+b.identity.userSidBytes+b.identity.logonSidBytes;
    wire[0]='T';wire[1]='N';wire[2]='A';wire[3]='C';Put(wire.data()+4,1,2);
    Put(wire.data()+6,static_cast<std::uint16_t>(b.type),2);Put(wire.data()+8,length-16,4);
    Put(wire.data()+16,b.lease,8);Put(wire.data()+24,b.generation,8);
    if(!offer){
        Put(wire.data()+32,b.identity.authenticationId,8);Put(wire.data()+40,b.identity.sessionId,4);
        Put(wire.data()+44,b.identity.userSidBytes,2);Put(wire.data()+46,b.identity.logonSidBytes,2);
        std::copy_n(b.identity.userSid.data(),b.identity.userSidBytes,wire.data()+48);
        std::copy_n(b.identity.logonSid.data(),b.identity.logonSidBytes,wire.data()+48+b.identity.userSidBytes);
    }
    std::copy(b.nonce.begin(),b.nonce.end(),wire.data()+length-16);
    output=wire;size=length;return true;
}
bool DecodeBinding(const void* input,std::size_t size,ChannelBinding& output) noexcept {
    if(!input||size<16||size>MaxBindingBytes)return false;
    const auto* wire=static_cast<const std::uint8_t*>(input);
    if(wire[0]!='T'||wire[1]!='N'||wire[2]!='A'||wire[3]!='C'||Get(wire+4,2)!=1||
       Get(wire+8,4)!=size-16||Get(wire+12,4)!=0)return false;
    const auto type=Get(wire+6,2);if(type<1||type>5)return false;
    ChannelBinding b;b.type=static_cast<BindingType>(type);
    if(b.type==BindingType::DataOffer){if(size!=48)return false;}
    else {
        if(size<64)return false;
        auto& i=b.identity;
        i.userSidBytes=static_cast<std::uint16_t>(Get(wire+44,2));
        i.logonSidBytes=static_cast<std::uint16_t>(Get(wire+46,2));
        if(i.userSidBytes>i.userSid.size()||i.logonSidBytes>i.logonSid.size()||
           size!=64u+i.userSidBytes+i.logonSidBytes)return false;
        i.authenticationId=Get(wire+32,8);i.sessionId=static_cast<std::uint32_t>(Get(wire+40,4));
        std::copy_n(wire+48,i.userSidBytes,i.userSid.data());
        std::copy_n(wire+48+i.userSidBytes,i.logonSidBytes,i.logonSid.data());
    }
    b.lease=Get(wire+16,8);b.generation=Get(wire+24,8);
    std::copy_n(wire+size-16,b.nonce.size(),b.nonce.data());
    if(!Valid(b))return false;
    output=b;return true;
}
bool SameChannelIdentity(const ChannelIdentity& a,const ChannelIdentity& b) noexcept {
    return ValidIdentity(a)&&ValidIdentity(b)&&a.authenticationId==b.authenticationId&&a.sessionId==b.sessionId&&
        a.userSidBytes==b.userSidBytes&&a.logonSidBytes==b.logonSidBytes&&
        std::equal(a.userSid.begin(),a.userSid.begin()+a.userSidBytes,b.userSid.begin())&&
        std::equal(a.logonSid.begin(),a.logonSid.begin()+a.logonSidBytes,b.logonSid.begin());
}
bool MatchesBinding(const ChannelBinding& a,const ChannelBinding& b) noexcept {
    return Valid(a)&&Valid(b)&&a.type==b.type&&a.lease==b.lease&&a.generation==b.generation&&a.nonce==b.nonce&&
        (a.type==BindingType::DataOffer||SameChannelIdentity(a.identity,b.identity));
}
}
