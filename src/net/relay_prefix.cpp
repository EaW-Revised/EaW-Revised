#include "relay_protocol.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <array>
#include <cstring>

namespace eawr::net::relay {
std::string address_prefix(std::string_view address) {
    const std::string input(address.substr(0, address.find('%')));
    std::array<char, INET6_ADDRSTRLEN> text{};
    auto format = [&](int family, const void* bytes) {
        return inet_ntop(family, bytes, text.data(), static_cast<unsigned>(text.size()))
            ? std::string(text.data()) : std::string{};
    };
    in_addr v4{};
    if (inet_pton(AF_INET, input.c_str(), &v4) == 1) return format(AF_INET, &v4);
    in6_addr v6{};
    if (inet_pton(AF_INET6, input.c_str(), &v6) != 1) return {};
    if (IN6_IS_ADDR_V4MAPPED(&v6)) {
        std::memcpy(&v4, reinterpret_cast<const std::uint8_t*>(&v6) + 12, sizeof(v4));
        return format(AF_INET, &v4);
    }
    std::memset(reinterpret_cast<std::uint8_t*>(&v6) + 8, 0, 8);
    const auto prefix = format(AF_INET6, &v6);
    return prefix.empty() ? prefix : prefix + "/64";
}
} // namespace eawr::net::relay
