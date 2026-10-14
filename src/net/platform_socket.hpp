#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <string_view>
#include <array>
#include <vector>
#include <string>

namespace eawr::net::socket {
using Handle = std::intptr_t;
inline constexpr Handle invalid = -1;
enum class Status { ready, pending, closed, error };
struct Io { Status status{Status::error}; std::size_t count{}; };
struct Open { Handle handle{invalid}; std::uint16_t port{}; };
// Opaque native endpoint bytes. Resolution may block; run it on a resolver thread.
struct Address { alignas(std::max_align_t) std::array<std::uint8_t, 128> bytes{}; std::size_t size{}; int family{}; };
std::vector<Address> resolve(std::string_view address, std::uint16_t port);
Handle connect(const Address& address);
// Numeric text form of a resolved address (no scope id, never DNS); empty when it cannot be formatted.
std::string numeric_host(const Address& address);
// True when `text` is a literal IPv4 (or IPv6) address as inet_pton parses it; never DNS.
bool numeric_ip(std::string_view text, bool ipv6);
// Numeric listen/connect helpers never perform DNS.
Open listen(std::string_view address, std::uint16_t port);
Handle connect(std::string_view address, std::uint16_t port);
Handle accept(Handle listener);
std::string peer_address(Handle stream); // numeric only, never DNS
Status connected(Handle handle);
Io read(Handle handle, std::span<std::uint8_t> bytes);
Io write(Handle handle, std::span<const std::uint8_t> bytes);
bool shutdown_write(Handle handle);
void close(Handle handle);

// Non-blocking IPv4 datagram sockets for LAN discovery. Addresses are host-order integers
// (127.0.0.1 is 0x7F000001). The socket allows address reuse, so a second viewer on one machine
// can listen on the same port, and broadcast sends.
inline constexpr std::uint32_t ipv4_loopback = 0x7F000001U;
inline constexpr std::uint32_t ipv4_broadcast = 0xFFFFFFFFU;
struct Datagram { Status status{Status::error}; std::size_t count{}; std::uint32_t from{}; };
// `address` is a numeric IPv4 address; port 0 picks an ephemeral port (reported in Open::port).
// Set reuse=false for private IPC endpoints that must never share a binding.
Open udp_open(std::string_view address, std::uint16_t port, bool reuse = true);
bool udp_send(Handle handle, std::uint32_t address, std::uint16_t port, std::span<const std::uint8_t> bytes);
Datagram udp_receive(Handle handle, std::span<std::uint8_t> bytes);
} // namespace eawr::net::socket
