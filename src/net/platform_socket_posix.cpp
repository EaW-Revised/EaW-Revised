#include "platform_socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <cstring>
#include <netdb.h>

namespace eawr::net::socket {
std::string peer_address(Handle stream) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    std::array<char, NI_MAXHOST> name{};
    if (getpeername(static_cast<int>(stream), reinterpret_cast<sockaddr*>(&address), &length) != 0) return {};
    if (address.ss_family == AF_INET6) {
        const auto& v6 = reinterpret_cast<const sockaddr_in6&>(address);
        if (IN6_IS_ADDR_V4MAPPED(&v6.sin6_addr)) {
            sockaddr_in v4{}; v4.sin_family = AF_INET; v4.sin_port = v6.sin6_port;
            std::memcpy(&v4.sin_addr, reinterpret_cast<const std::uint8_t*>(&v6.sin6_addr) + 12, sizeof(v4.sin_addr));
            std::memcpy(&address, &v4, sizeof(v4)); length = sizeof(v4);
        }
    }
    if (getnameinfo(reinterpret_cast<sockaddr*>(&address), length, name.data(), static_cast<socklen_t>(name.size()),
            nullptr, 0, NI_NUMERICHOST) != 0) return {};
    return name.data();
}
namespace {
int native(Handle handle) { return static_cast<int>(handle); }
bool configure(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    const int yes = 1;
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0
        && setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0;
}
bool endpoint(std::string_view address, std::uint16_t port, sockaddr_in& target) {
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    return inet_pton(AF_INET, std::string(address).c_str(), &target.sin_addr) == 1;
}
Io result(ssize_t count) {
    if (count > 0) return {Status::ready, static_cast<std::size_t>(count)};
    if (count == 0) return {Status::closed, 0};
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return {Status::pending, 0};
    return {Status::error, 0};
}
}
std::vector<Address> resolve(std::string_view address, std::uint16_t port) {
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
    std::array<char, 64> text{};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(address.bytes.data()), static_cast<socklen_t>(address.size),
            text.data(), static_cast<socklen_t>(text.size()), nullptr, 0, NI_NUMERICHOST) != 0) return {};
    return text.data();
}
bool numeric_ip(std::string_view text, bool ipv6) {
    std::array<std::uint8_t, 16> buffer{};
    return inet_pton(ipv6 ? AF_INET6 : AF_INET, std::string(text).c_str(), buffer.data()) == 1;
}
Open listen(std::string_view address, std::uint16_t port) {
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
        const int fd = ::socket(family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        const int zero = 0, yes = 1;
        if (!configure(fd) || (family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)) != 0)
            || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0
            || ::bind(fd, reinterpret_cast<sockaddr*>(&target), static_cast<socklen_t>(size)) != 0 || ::listen(fd, 8) != 0) { ::close(fd); continue; }
        socklen_t count = static_cast<socklen_t>(size);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&target), &count) != 0) { ::close(fd); continue; }
        const auto bound = family == AF_INET6 ? reinterpret_cast<sockaddr_in6&>(target).sin6_port : reinterpret_cast<sockaddr_in&>(target).sin_port;
        return {static_cast<Handle>(fd), ntohs(bound)};
    }
    return {};
}
Handle connect(const Address& address) {
    const int fd = ::socket(address.family, SOCK_STREAM, 0);
    if (fd < 0) return invalid;
    if (!configure(fd)) { ::close(fd); return invalid; }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(address.bytes.data()), static_cast<socklen_t>(address.size)) != 0
        && errno != EINPROGRESS) { ::close(fd); return invalid; }
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
    const int fd = ::accept(native(listener), nullptr, nullptr);
    if (fd < 0) return invalid;
    if (!configure(fd)) { ::close(fd); return invalid; }
    return fd;
}
Status connected(Handle handle) {
    pollfd descriptor{native(handle), POLLOUT, 0};
    const int ready = ::poll(&descriptor, 1, 0);
    if (ready == 0 || (ready < 0 && errno == EINTR)) return Status::pending;
    if (ready < 0) return Status::error;
    int error = 0;
    socklen_t size = sizeof(error);
    return getsockopt(native(handle), SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0
        ? Status::ready : Status::error;
}
Io read(Handle handle, std::span<std::uint8_t> bytes) {
    return result(::recv(native(handle), bytes.data(), bytes.size(), 0));
}
Io write(Handle handle, std::span<const std::uint8_t> bytes) {
    return result(::send(native(handle), bytes.data(), bytes.size(), MSG_NOSIGNAL));
}
void close(Handle handle) { if (handle != invalid) ::close(native(handle)); }
bool shutdown_write(Handle handle) { return ::shutdown(native(handle), SHUT_WR) == 0; }
Open udp_open(std::string_view address, std::uint16_t port, bool reuse) {
    sockaddr_in target{};
    if (!endpoint(address, port, target)) return {};
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return {};
    const int flags = fcntl(fd, F_GETFL, 0);
    const int yes = 1;
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0
        || (reuse && setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0)
#ifdef SO_REUSEPORT
        || (reuse && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) != 0)
#endif
        || setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) != 0
        || ::bind(fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) { ::close(fd); return {}; }
    socklen_t length = sizeof(target);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&target), &length) != 0) { ::close(fd); return {}; }
    return {fd, ntohs(target.sin_port)};
}
bool udp_send(Handle handle, std::uint32_t address, std::uint16_t port, std::span<const std::uint8_t> bytes) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    target.sin_addr.s_addr = htonl(address);
    const ssize_t sent = ::sendto(native(handle), bytes.data(), bytes.size(), MSG_NOSIGNAL,
        reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    return sent == static_cast<ssize_t>(bytes.size());
}
Datagram udp_receive(Handle handle, std::span<std::uint8_t> bytes) {
    sockaddr_in from{};
    socklen_t length = sizeof(from);
    const ssize_t count = ::recvfrom(native(handle), bytes.data(), bytes.size(), 0,
        reinterpret_cast<sockaddr*>(&from), &length);
    if (count >= 0) return {Status::ready, static_cast<std::size_t>(count), ntohl(from.sin_addr.s_addr)};
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return {Status::pending, 0, 0};
    return {Status::error, 0, 0};
}
} // namespace eawr::net::socket
