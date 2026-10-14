#include "platform_socket.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <limits>
#include <string>
#include <cstring>

namespace eawr::net::socket {
std::string peer_address(Handle stream) {
    sockaddr_storage address{};
    int length = sizeof(address);
    std::array<char, NI_MAXHOST> name{};
    if (getpeername(static_cast<SOCKET>(stream), reinterpret_cast<sockaddr*>(&address), &length) != 0) return {};
    // A dual-stack accept reports IPv4 peers as mapped IPv6. Export their IPv4
    // address so later pair dials do not require an IPv6-only socket to route IPv4.
    if (address.ss_family == AF_INET6) {
        const auto& v6 = reinterpret_cast<const sockaddr_in6&>(address);
        if (IN6_IS_ADDR_V4MAPPED(&v6.sin6_addr)) {
            sockaddr_in v4{}; v4.sin_family = AF_INET; v4.sin_port = v6.sin6_port;
            std::memcpy(&v4.sin_addr, reinterpret_cast<const std::uint8_t*>(&v6.sin6_addr) + 12, sizeof(v4.sin_addr));
            std::memcpy(&address, &v4, sizeof(v4)); length = sizeof(v4);
        }
    }
    if (getnameinfo(reinterpret_cast<sockaddr*>(&address), length, name.data(), static_cast<DWORD>(name.size()),
            nullptr, 0, NI_NUMERICHOST) != 0) return {};
    return name.data();
}
namespace {
struct Runtime {
    bool ready{};
    Runtime() { WSADATA data{}; ready = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~Runtime() { if (ready) WSACleanup(); }
};
bool initialize() { static Runtime runtime; return runtime.ready; }
SOCKET native(Handle handle) { return static_cast<SOCKET>(handle); }
bool configure(SOCKET fd) {
    u_long nonblocking = 1;
    const int yes = 1;
    return ioctlsocket(fd, FIONBIO, &nonblocking) == 0
        && setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes)) == 0;
}
bool endpoint(std::string_view address, std::uint16_t port, sockaddr_in& target) {
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    return InetPtonA(AF_INET, std::string(address).c_str(), &target.sin_addr) == 1;
}
Io result(int count) {
    if (count > 0) return {Status::ready, static_cast<std::size_t>(count)};
    if (count == 0) return {Status::closed, 0};
    const int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK || error == WSAEINTR) return {Status::pending, 0};
    return {Status::error, 0};
}
int size(std::size_t count) { return static_cast<int>(std::min(count, static_cast<std::size_t>((std::numeric_limits<int>::max)()))); }
}
std::vector<Address> resolve(std::string_view address, std::uint16_t port) {
    if (!initialize()) return {};
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* list = nullptr;
    if (getaddrinfo(std::string(address).c_str(), std::to_string(port).c_str(), &hints, &list) != 0) return {};
    struct Guard { addrinfo* value; ~Guard() { freeaddrinfo(value); } } guard{list};
    std::vector<Address> result;
    for (auto* item = list; item; item = item->ai_next) {
        Address target;
        if ((item->ai_family != AF_INET && item->ai_family != AF_INET6) || item->ai_addrlen > target.bytes.size()) continue;
        target.family = item->ai_family; target.size = static_cast<std::size_t>(item->ai_addrlen);
        std::memcpy(target.bytes.data(), item->ai_addr, target.size); result.push_back(target);
    }
    return result;
}
std::string numeric_host(const Address& address) {
    if (!initialize()) return {};
    std::array<char, 64> text{};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(address.bytes.data()), static_cast<int>(address.size),
            text.data(), static_cast<DWORD>(text.size()), nullptr, 0, NI_NUMERICHOST) != 0) return {};
    return text.data();
}
bool numeric_ip(std::string_view text, bool ipv6) {
    if (!initialize()) return false;
    std::array<std::uint8_t, 16> buffer{};
    return inet_pton(ipv6 ? AF_INET6 : AF_INET, std::string(text).c_str(), buffer.data()) == 1;
}
Open listen(std::string_view address, std::uint16_t port) {
    if (!initialize()) return {};
    // Wildcard hosts prefer one dual-stack socket, falling back to IPv4.
    for (const int family : {AF_INET6, AF_INET}) {
        sockaddr_storage target{};
        std::size_t size{};
        if (family == AF_INET6) {
            auto& v6 = reinterpret_cast<sockaddr_in6&>(target);
            v6.sin6_family = AF_INET6; v6.sin6_port = htons(port); size = sizeof(v6);
            const auto numeric = address == "0.0.0.0" ? "::" : std::string(address);
            if (inet_pton(AF_INET6, numeric.c_str(), &v6.sin6_addr) != 1) continue;
        } else {
            auto& v4 = reinterpret_cast<sockaddr_in&>(target); size = sizeof(v4);
            if (!endpoint(address, port, v4)) continue;
        }
        const SOCKET fd = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET) continue;
        const int zero = 0, yes = 1;
        if (!configure(fd) || (family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&zero), sizeof(zero)) != 0)
            || setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0
            || ::bind(fd, reinterpret_cast<sockaddr*>(&target), static_cast<int>(size)) != 0 || ::listen(fd, 8) != 0) { closesocket(fd); continue; }
        int count = static_cast<int>(size);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&target), &count) != 0) { closesocket(fd); continue; }
        const auto bound = family == AF_INET6 ? reinterpret_cast<sockaddr_in6&>(target).sin6_port : reinterpret_cast<sockaddr_in&>(target).sin_port;
        return {static_cast<Handle>(fd), ntohs(bound)};
    }
    return {};
}
Handle connect(const Address& address) {
    if (!initialize()) return invalid;
    const SOCKET fd = ::socket(address.family, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return invalid;
    if (!configure(fd)) { closesocket(fd); return invalid; }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(address.bytes.data()), static_cast<int>(address.size)) != 0
        && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(fd); return invalid; }
    return static_cast<Handle>(fd);
}
Handle connect(std::string_view address, std::uint16_t port) {
    // Numeric-only helper for raw socket callers; the transport resolves asynchronously.
    Address target;
    auto& v4 = *reinterpret_cast<sockaddr_in*>(target.bytes.data());
    if (endpoint(address, port, v4)) { target.family = AF_INET; target.size = sizeof(v4); }
    else {
        auto& v6 = *reinterpret_cast<sockaddr_in6*>(target.bytes.data());
        v6.sin6_family = AF_INET6; v6.sin6_port = htons(port);
        if (inet_pton(AF_INET6, std::string(address).c_str(), &v6.sin6_addr) != 1) return invalid;
        target.family = AF_INET6; target.size = sizeof(v6);
    }
    return connect(target);
}
Handle accept(Handle listener) {
    const SOCKET fd = ::accept(native(listener), nullptr, nullptr);
    if (fd == INVALID_SOCKET) return invalid;
    if (!configure(fd)) { closesocket(fd); return invalid; }
    return static_cast<Handle>(fd);
}
Status connected(Handle handle) {
    WSAPOLLFD descriptor{native(handle), POLLWRNORM, 0};
    const int ready = WSAPoll(&descriptor, 1, 0);
    if (ready == 0) return Status::pending;
    if (ready < 0) return Status::error;
    int error = 0;
    int length = sizeof(error);
    return getsockopt(native(handle), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0
        ? Status::ready : Status::error;
}
Io read(Handle handle, std::span<std::uint8_t> bytes) {
    return result(recv(native(handle), reinterpret_cast<char*>(bytes.data()), size(bytes.size()), 0));
}
Io write(Handle handle, std::span<const std::uint8_t> bytes) {
    return result(::send(native(handle), reinterpret_cast<const char*>(bytes.data()), size(bytes.size()), 0));
}
void close(Handle handle) { if (handle != invalid) closesocket(native(handle)); }
bool shutdown_write(Handle handle) { return ::shutdown(native(handle), SD_SEND) == 0; }
Open udp_open(std::string_view address, std::uint16_t port, bool reuse) {
    sockaddr_in target{};
    if (!initialize() || !endpoint(address, port, target)) return {};
    const SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) return {};
    u_long nonblocking = 1;
    const int yes = 1;
    // A send to a port nobody listens on makes later receives fail with a connection reset; ask
    // the stack not to report it for this socket.
    DWORD returned = 0;
    BOOL no_reset = FALSE;
    constexpr DWORD udp_connection_reset = 0x9800000CU;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0
        || setsockopt(fd, SOL_SOCKET, reuse ? SO_REUSEADDR : SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0
        || setsockopt(fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0
        || WSAIoctl(fd, udp_connection_reset, &no_reset, sizeof(no_reset), nullptr, 0, &returned, nullptr, nullptr) != 0
        || ::bind(fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) { closesocket(fd); return {}; }
    int length = sizeof(target);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&target), &length) != 0) { closesocket(fd); return {}; }
    return {static_cast<Handle>(fd), ntohs(target.sin_port)};
}
bool udp_send(Handle handle, std::uint32_t address, std::uint16_t port, std::span<const std::uint8_t> bytes) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    target.sin_addr.s_addr = htonl(address);
    const int sent = ::sendto(native(handle), reinterpret_cast<const char*>(bytes.data()), size(bytes.size()), 0,
        reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    return sent == static_cast<int>(bytes.size());
}
Datagram udp_receive(Handle handle, std::span<std::uint8_t> bytes) {
    sockaddr_in from{};
    int length = sizeof(from);
    const int count = ::recvfrom(native(handle), reinterpret_cast<char*>(bytes.data()), size(bytes.size()), 0,
        reinterpret_cast<sockaddr*>(&from), &length);
    if (count >= 0) return {Status::ready, static_cast<std::size_t>(count), ntohl(from.sin_addr.s_addr)};
    const int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK || error == WSAEINTR) return {Status::pending, 0, 0};
    // An oversize datagram is cut by the stack; it is not one of ours, so it is dropped.
    if (error == WSAEMSGSIZE) return {Status::ready, 0, ntohl(from.sin_addr.s_addr)};
    return {Status::error, 0, 0};
}
} // namespace eawr::net::socket
