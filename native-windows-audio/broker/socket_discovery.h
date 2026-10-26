#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace timox_broker {
// PUBLIC metadata, not credentials. Decoding does NOT establish trust: the host
// must verify the publication's owner, ACL and path chain before consuming it.
// epoch is host metadata, not a freshness proof or a field in the TNAC handshake.
struct SocketDiscovery {
    std::uint64_t epoch = 0;
    std::array<std::uint8_t,16> socketId{};
    std::array<std::uint8_t,32> serverPin{};
};
constexpr std::size_t SocketDiscoveryBytes = 72;
using SocketDiscoveryBuffer = std::array<std::uint8_t,SocketDiscoveryBytes>;
using SocketBasename = std::array<char,38>; // 32 hex + .sock + NUL
// Fixed little-endian TNSD v1. Exact size, zero flags/reserved fields,
// nonzero epoch/id/pin. No native structure serialization; failures preserve output.
bool EncodeSocketDiscovery(const SocketDiscovery&, SocketDiscoveryBuffer&) noexcept;
bool DecodeSocketDiscovery(const void*, std::size_t, SocketDiscovery&) noexcept;
// Only a basename can come from discovery. The trusted host supplies the root,
// checks the complete AF_UNIX path length and refuses truncation/reparse points.
bool SocketDiscoveryBasename(const SocketDiscovery&, SocketBasename&) noexcept;
}
