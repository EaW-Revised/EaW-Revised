#include "eawr/net/port_mapping.hpp"
#include "../net/platform_socket.hpp"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fstream>
#include <sstream>
#endif
#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <limits>
#include <optional>
#include <thread>

namespace eawr::net::mapping {
namespace {
template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic d; d.code = "EAWR-NET-1107"; d.message = std::move(message);
    return core::Result<T>::failure(std::move(d));
}
struct Socket {
    socket::Handle fd{socket::invalid};
    ~Socket() { socket::close(fd); }
};
using Clock = std::chrono::steady_clock;
constexpr auto timeout = std::chrono::milliseconds(1500);
bool pending(std::stop_token stop, Clock::time_point end) {
    return !stop.stop_requested() && Clock::now() < end;
}
void pause() { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
class NativeIo final : public RouterIo {
public:
    core::Result<Route> route() override {
        // udp_open initializes Winsock and yields a nonblocking socket for source selection.
        Socket probe{socket::udp_open("0.0.0.0", 0).handle};
        if (probe.fd == socket::invalid) return fail<Route>("Could not open router discovery socket.");
        std::uint32_t gateway = 0;
#ifdef _WIN32
        ULONG length = 0;
        if (GetIpForwardTable(nullptr, &length, FALSE) != ERROR_INSUFFICIENT_BUFFER)
            return fail<Route>("Could not read the IPv4 routing table.");
        // DWORD backing keeps the routing table aligned.
        std::vector<DWORD> storage((length + sizeof(DWORD) - 1) / sizeof(DWORD));
        auto* table = reinterpret_cast<MIB_IPFORWARDTABLE*>(storage.data());
        if (GetIpForwardTable(table, &length, FALSE) != NO_ERROR) return fail<Route>("Could not read the IPv4 routing table.");
        DWORD metric = (std::numeric_limits<DWORD>::max)();
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto& row = table->table[i];
            if (row.dwForwardDest == 0 && row.dwForwardMask == 0 && row.dwForwardNextHop != 0 && row.dwForwardMetric1 < metric) {
                gateway = ntohl(row.dwForwardNextHop); metric = row.dwForwardMetric1;
            }
        }
#else
        // Linux route discovery. Other POSIX systems fail non-fatally rather than guessing a router.
        std::ifstream routes("/proc/net/route"); std::string line; unsigned best = (std::numeric_limits<unsigned>::max)();
        std::getline(routes, line);
        while (std::getline(routes, line)) {
            std::istringstream row(line); std::string interface;
            unsigned dest{}, hop{}, flags{}, ref{}, use{}, metric{}, mask{};
            if (!(row >> interface >> std::hex >> dest >> hop >> flags >> std::dec >> ref >> use >> metric >> std::hex >> mask)) continue;
            if (dest == 0 && mask == 0 && hop != 0 && (flags & 3U) == 3U && metric < best) {
                gateway = ((hop & 255U) << 24U) | ((hop & 0xFF00U) << 8U) | ((hop & 0xFF0000U) >> 8U) | (hop >> 24U); best = metric;
            }
        }
#endif
        if (!gateway) return fail<Route>("No IPv4 default gateway; automatic port mapping is unavailable.");
        sockaddr_in target{}; target.sin_family = AF_INET; target.sin_port = htons(5351); target.sin_addr.s_addr = htonl(gateway);
#ifdef _WIN32
        const auto fd = static_cast<SOCKET>(probe.fd); int size = sizeof(target);
#else
        const auto fd = static_cast<int>(probe.fd); socklen_t size = sizeof(target);
#endif
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0
            || getsockname(fd, reinterpret_cast<sockaddr*>(&target), &size) != 0)
            return fail<Route>("Could not determine the local address for the gateway.");
        return core::Result<Route>::success({gateway, ntohl(target.sin_addr.s_addr)});
    }
    core::Result<Bytes> udp(std::uint32_t address, std::uint16_t port,
        std::span<const std::uint8_t> request, std::stop_token stop, bool discovery) override {
        Socket stream{socket::udp_open("0.0.0.0", 0).handle};
        if (stream.fd == socket::invalid) return fail<Bytes>("Could not open router UDP socket.");
        const auto end = Clock::now() + timeout;
        // PCP/NAT-PMP use connected UDP: the kernel filters source address AND source port.
        if (!discovery) {
            sockaddr_in target{}; target.sin_family = AF_INET; target.sin_port = htons(port); target.sin_addr.s_addr = htonl(address);
#ifdef _WIN32
            const auto fd = static_cast<SOCKET>(stream.fd);
#else
            const auto fd = static_cast<int>(stream.fd);
#endif
            if (::connect(fd, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0) return fail<Bytes>("Router UDP connection failed.");
        }
        // Cleanup is best effort: do not leave duplicate zero-lifetime requests
        // in flight that could erase a replacement NAT-PMP mapping after an ACK.
        const bool removing = !discovery && ((request.size() == 12 && request[0] == 0 && request[1] == 2
            && std::all_of(request.begin() + 8, request.end(), [](auto byte) { return byte == 0; }))
            || (request.size() == 60 && request[0] == 2 && request[1] == 1
                && std::all_of(request.begin() + 4, request.begin() + 8, [](auto byte) { return byte == 0; })));
        auto next_send = Clock::now(); std::array<std::uint8_t, 8192> buffer{};
        while (pending(stop, end)) {
            if (Clock::now() >= next_send) {
                if (!socket::udp_send(stream.fd, discovery ? 0xEFFFFFFAU : address, port, request))
                    return fail<Bytes>("Router UDP request failed.");
                next_send = removing ? end : Clock::now() + std::chrono::milliseconds(250);
            }
            const auto received = socket::udp_receive(stream.fd, buffer);
            if (received.status == socket::Status::ready && received.count && received.from == address)
                return core::Result<Bytes>::success(Bytes(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received.count)));
            if (received.status == socket::Status::error) return fail<Bytes>("Router did not accept UDP discovery.");
            pause();
        }
        return fail<Bytes>("Router discovery timed out or was cancelled.");
    }
    core::Result<std::string> http(const Url& url, std::string_view action,
        std::string_view body, std::stop_token stop) override {
        Socket stream{socket::connect(ipv4(url.address), url.port)};
        if (stream.fd == socket::invalid) return fail<std::string>("Could not connect to the UPnP gateway.");
        const auto end = Clock::now() + timeout;
        auto state = socket::Status::pending;
        while (pending(stop, end) && (state = socket::connected(stream.fd)) == socket::Status::pending) pause();
        if (state != socket::Status::ready) return fail<std::string>("UPnP connection timed out or failed.");
        const bool get = action.empty();
        std::string request = std::string(get ? "GET " : "POST ") + url.path + " HTTP/1.1\r\nHost: " + ipv4(url.address)
            + ":" + std::to_string(url.port) + "\r\nConnection: close\r\n";
        if (!get) request += "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + std::string(action)
            + "\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        request += "\r\n"; request += body;
        std::size_t sent = 0;
        while (sent < request.size() && pending(stop, end)) {
            const auto written = socket::write(stream.fd, {reinterpret_cast<const std::uint8_t*>(request.data() + sent), request.size() - sent});
            if (written.status == socket::Status::error || written.status == socket::Status::closed) return fail<std::string>("UPnP request failed.");
            sent += written.count; if (!written.count) pause();
        }
        if (sent != request.size()) return fail<std::string>("UPnP request timed out.");
        std::string response; std::array<std::uint8_t, 4096> buffer{};
        std::size_t header_end = std::string::npos; std::optional<std::size_t> length;
        bool closed = false;
        while (pending(stop, end) && response.size() <= 73728) {
            const auto read = socket::read(stream.fd, buffer);
            if (read.status == socket::Status::error) return fail<std::string>("UPnP response failed.");
            if (read.status == socket::Status::closed) { closed = true; break; }
            response.append(reinterpret_cast<const char*>(buffer.data()), read.count);
            if (header_end == std::string::npos) {
                header_end = response.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    auto headers = response.substr(0, header_end);
                    std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    // Close-delimited and Content-Length supported. Reject transfer encodings explicitly.
                    if (headers.find("transfer-encoding:") != headers.npos) return fail<std::string>("UPnP gateway uses an unsupported HTTP transfer encoding.");
                    if (const auto pos = headers.find("\r\ncontent-length:"); pos != headers.npos) {
                        auto value = std::string_view(headers).substr(pos + 17);
                        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
                        std::size_t count{}; auto n = std::from_chars(value.data(), value.data() + value.size(), count);
                        if (n.ec != std::errc{} || count > 65536) return fail<std::string>("Invalid UPnP HTTP length.");
                        length = count;
                    }
                }
            }
            if (length && header_end != response.npos && response.size() >= header_end + 4 + *length) break;
            if (!read.count) pause();
        }
        if (header_end == response.npos || header_end > 8192 || response.size() > 73728
            || (!closed && (!length || response.size() < header_end + 4 + *length))
            || !(response.starts_with("HTTP/1.1 200 ") || response.starts_with("HTTP/1.0 200 ")
                || response.starts_with("HTTP/1.1 500 ") || response.starts_with("HTTP/1.0 500 ")))
            return fail<std::string>("UPnP HTTP action was refused, incomplete or too large.");
        return core::Result<std::string>::success(response.substr(header_end + 4, length.value_or(response.size() - header_end - 4)));
    }
};
}
std::unique_ptr<RouterIo> make_router_io() { return std::make_unique<NativeIo>(); }
} // namespace eawr::net::mapping
