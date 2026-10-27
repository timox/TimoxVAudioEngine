#include "socket_discovery.h"
#include <algorithm>

namespace timox_broker {
namespace {
template<std::size_t N> bool Nonzero(const std::array<std::uint8_t,N>& bytes) noexcept {
    return std::any_of(bytes.begin(),bytes.end(),[](std::uint8_t b){return b!=0;});
}
bool Valid(const SocketDiscovery& d) noexcept {
    return d.epoch!=0 && Nonzero(d.socketId) && Nonzero(d.serverPin);
}
constexpr std::array<std::uint8_t,16> Header{'T','N','S','D',1,0,0,0,72,0,0,0,0,0,0,0};
}
bool EncodeSocketDiscovery(const SocketDiscovery& d, SocketDiscoveryBuffer& output) noexcept {
    if(!Valid(d)) return false;
    SocketDiscoveryBuffer wire{};
    std::copy(Header.begin(),Header.end(),wire.begin());
    for(unsigned i=0;i<8;++i) wire[16+i]=static_cast<std::uint8_t>(d.epoch>>(8*i));
    std::copy(d.socketId.begin(),d.socketId.end(),wire.begin()+24);
    std::copy(d.serverPin.begin(),d.serverPin.end(),wire.begin()+40);
    output=wire; return true;
}
bool DecodeSocketDiscovery(const void* data, std::size_t size, SocketDiscovery& output) noexcept {
    if(!data || size!=SocketDiscoveryBytes) return false;
    const auto* wire=static_cast<const std::uint8_t*>(data);
    if(!std::equal(Header.begin(),Header.end(),wire)) return false;
    SocketDiscovery d;
    for(unsigned i=0;i<8;++i) d.epoch|=std::uint64_t{wire[16+i]}<<(8*i);
    std::copy_n(wire+24,d.socketId.size(),d.socketId.begin());
    std::copy_n(wire+40,d.serverPin.size(),d.serverPin.begin());
    if(!Valid(d)) return false;
    output=d; return true;
}
bool SocketDiscoveryBasename(const SocketDiscovery& d, SocketBasename& output) noexcept {
    if(!Valid(d)) return false;
    constexpr char hex[]="0123456789abcdef";
    SocketBasename name{};
    for(std::size_t i=0;i<d.socketId.size();++i) {
        name[2*i]=hex[d.socketId[i]>>4]; name[2*i+1]=hex[d.socketId[i]&15];
    }
    constexpr char suffix[]=".sock";
    std::copy_n(suffix,sizeof(suffix),name.begin()+32);
    output=name; return true;
}
}
