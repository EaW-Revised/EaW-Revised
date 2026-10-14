#include "relay_https.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace eawr::net::relay {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_head = 16 * 1024, max_response = 64 * 1024;
core::Diagnostic failure(const char* why) { return error(1556, why); }
struct Socket {
    socket::Handle handle;
    ~Socket() { socket::close(handle); }
};
bool clean(std::string_view text) {
    return std::none_of(text.begin(), text.end(), [](unsigned char ch) { return ch < 33 || ch > 126; });
}
bool request_valid(const HttpsRequest& r) {
    return !r.host.empty() && r.host.size() <= 253 && clean(r.host) && r.port != 0 && r.path.starts_with('/')
        && clean(r.path) && (r.method == "POST" || r.method == "GET") && clean(r.bearer) && r.body.size() <= 4096
        && r.timeout.count() > 0;
}
std::string head(const HttpsRequest& r) {
    std::string text = r.method + " " + r.path + " HTTP/1.1\r\nHost: " + r.host + "\r\nUser-Agent: eawr-relay\r\nAccept: application/json\r\n";
    if (!r.bearer.empty()) text += "Authorization: Bearer " + r.bearer + "\r\n";
    if (r.method == "POST") {
        if (!r.body.empty()) text += "Content-Type: application/json\r\n";
        text += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    }
    return text + "Connection: close\r\n\r\n" + r.body;
}
std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return result;
}
// getaddrinfo cannot be interrupted, so a lookup that outlives its deadline is left to finish on a
// detached helper that only owns shared state; the caller moves on at the deadline or on stop.
struct Lookup {
    std::mutex mutex;
    std::condition_variable finished;
    bool done{};
    std::vector<socket::Address> addresses;
};
std::vector<socket::Address> resolve_bounded(const std::string& host, std::uint16_t port, Clock::time_point deadline, std::stop_token stop) {
    if (socket::numeric_ip(host, false) || socket::numeric_ip(host, true)) return socket::resolve(host, port);
    auto lookup = std::make_shared<Lookup>();
    std::thread([lookup, host, port] {
        auto found = socket::resolve(host, port);
        std::lock_guard lock(lookup->mutex);
        lookup->addresses = std::move(found); lookup->done = true;
        lookup->finished.notify_all();
    }).detach();
    std::unique_lock lock(lookup->mutex);
    while (!lookup->finished.wait_for(lock, std::chrono::milliseconds(10), [&] { return lookup->done; }))
        if (stop.stop_requested() || Clock::now() >= deadline) return {};
    return std::move(lookup->addresses);
}
enum class Parse { more, complete, bad };
// Decodes whatever has arrived. `eof` tells a close-delimited body that it is whole.
Parse parse(const std::string& raw, bool eof, HttpsResponse& out) {
    const auto end = raw.find("\r\n\r\n");
    if (end == std::string::npos) return raw.size() > max_head || eof ? Parse::bad : Parse::more;
    if (end > max_head || !raw.starts_with("HTTP/1.")) return Parse::bad;
    const auto line_end = raw.find("\r\n");
    const auto first = raw.substr(0, line_end);
    const auto space = first.find(' ');
    int status{};
    if (space == std::string::npos || first.size() < space + 4) return Parse::bad;
    const auto number = std::from_chars(first.data() + space + 1, first.data() + space + 4, status);
    if (number.ec != std::errc{} || status < 100 || status > 599) return Parse::bad;
    bool chunked = false;
    std::optional<std::size_t> length;
    for (std::size_t at = line_end + 2; at < end;) {
        const auto next = raw.find("\r\n", at);
        const auto line = raw.substr(at, next - at);
        at = next + 2;
        const auto colon = line.find(':');
        if (colon == std::string::npos) return Parse::bad;
        const auto name = lower(line.substr(0, colon));
        auto value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        if (name == "content-length") {
            std::size_t parsed{};
            const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || parsed > max_response) return Parse::bad;
            length = parsed;
        } else if (name == "transfer-encoding") chunked = lower(value).find("chunked") != std::string::npos;
    }
    out.status = status;
    out.body.clear();
    const auto body = std::string_view(raw).substr(end + 4);
    if (status == 204 || status == 304 || status < 200) return Parse::complete;
    if (chunked) {
        std::size_t at = 0;
        for (;;) {
            const auto line = body.find("\r\n", at);
            if (line == std::string_view::npos) return eof ? Parse::bad : Parse::more;
            std::size_t size{};
            const auto converted = std::from_chars(body.data() + at, body.data() + line, size, 16);
            if (converted.ec != std::errc{} || (converted.ptr != body.data() + line && *converted.ptr != ';')
                || size > max_response || out.body.size() + size > max_response) return Parse::bad;
            if (size == 0) return Parse::complete;
            if (body.size() < line + 2 + size + 2) return eof ? Parse::bad : Parse::more;
            out.body.append(body.substr(line + 2, size));
            if (body.substr(line + 2 + size, 2) != "\r\n") return Parse::bad;
            at = line + 2 + size + 2;
        }
    }
    if (length) {
        if (body.size() < *length) return eof ? Parse::bad : Parse::more;
        out.body.assign(body.substr(0, *length));
        return Parse::complete;
    }
    if (body.size() > max_response) return Parse::bad;
    out.body.assign(body);
    return eof ? Parse::complete : Parse::more;
}
}
core::Result<HttpsResponse> https_request(const HttpsRequest& request, std::stop_token stop) {
    using Out = core::Result<HttpsResponse>;
    if (!request_valid(request)) return Out::failure(failure("Invalid outbound HTTPS request"));
    const auto deadline = Clock::now() + request.timeout;
    const auto expired = [&] { return stop.stop_requested() || Clock::now() >= deadline; };
    auto addresses = resolve_bounded(request.connect_address.empty() ? request.host : request.connect_address, request.port, deadline, stop);
    if (addresses.size() > 4) addresses.resize(4);
    if (addresses.empty()) return Out::failure(failure("Outbound HTTPS host did not resolve"));
    Socket connection{socket::invalid};
    for (std::size_t index = 0; index < addresses.size() && connection.handle == socket::invalid; ++index) {
        const auto budget = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()) / static_cast<long>(addresses.size() - index);
        const auto attempt_end = Clock::now() + budget;
        const auto handle = socket::connect(addresses[index]);
        if (handle == socket::invalid) continue;
        Socket candidate{handle};
        for (;;) {
            const auto state = socket::connected(handle);
            if (state == socket::Status::ready) { connection.handle = handle; candidate.handle = socket::invalid; break; }
            if (state != socket::Status::pending || expired() || Clock::now() >= attempt_end) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (connection.handle == socket::invalid) return Out::failure(failure("Outbound HTTPS connection failed"));
    TlsChannelOptions options;
    options.server_name = request.host; options.ca_file = request.ca_file;
    auto channel = open_tls_channel(connection.handle, options);
    if (!channel) return Out::failure(failure("Outbound HTTPS TLS setup failed"));
    const auto drive = [&](auto&& step) -> core::Result<TlsStep> {
        for (;;) {
            const auto result = step();
            if (result != TlsStep::wants_io) return core::Result<TlsStep>::success(result);
            if (expired()) return core::Result<TlsStep>::failure(failure("Outbound HTTPS request timed out"));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    const auto handshake = drive([&] { return channel->handshake(); });
    if (!handshake) return Out::failure(handshake.error());
    if (handshake.value() != TlsStep::done) return Out::failure(failure("Outbound HTTPS TLS verification failed"));
    const auto text = head(request);
    for (std::size_t offset = 0; offset < text.size();) {
        std::size_t written{};
        const auto sent = drive([&] {
            return channel->write(std::span(reinterpret_cast<const std::uint8_t*>(text.data()) + offset, text.size() - offset), written);
        });
        if (!sent) return Out::failure(sent.error());
        if (sent.value() != TlsStep::done || written == 0) return Out::failure(failure("Outbound HTTPS write failed"));
        offset += written;
    }
    std::string raw;
    HttpsResponse response;
    for (bool eof = false;;) {
        const auto state = parse(raw, eof, response);
        if (state == Parse::complete) return Out::success(std::move(response));
        if (state == Parse::bad || eof || raw.size() > max_head + max_response + 4096)
            return Out::failure(failure("Outbound HTTPS response was malformed or truncated"));
        std::array<std::uint8_t, 4096> buffer{};
        std::size_t count{};
        const auto received = drive([&] { return channel->read(buffer, count); });
        if (!received) return Out::failure(received.error());
        if (received.value() == TlsStep::failed) return Out::failure(failure("Outbound HTTPS read failed"));
        if (received.value() == TlsStep::closed) eof = true;
        else raw.append(reinterpret_cast<const char*>(buffer.data()), count);
    }
}
std::string numeric_turn_uris(std::string_view list, const TurnHostResolver& custom, std::chrono::milliseconds budget, std::stop_token stop) {
    const auto deadline = Clock::now() + budget;
    const TurnHostResolver system = [&](const std::string& host, std::uint16_t port) {
        std::vector<std::string> result;
        for (const auto& address : resolve_bounded(host, port, deadline, stop)) result.push_back(socket::numeric_host(address));
        return result;
    };
    const auto& resolver = custom ? custom : system;
    std::vector<std::string> entries;
    const auto add = [&](std::string entry) {
        if (entries.size() < 8 && std::find(entries.begin(), entries.end(), entry) == entries.end()) entries.push_back(std::move(entry));
    };
    const auto parse_port = [](std::string_view digits, std::uint16_t& port) {
        unsigned parsed{};
        const auto converted = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
        if (digits.empty() || converted.ec != std::errc{} || converted.ptr != digits.data() + digits.size() || parsed == 0 || parsed > 65535) return false;
        port = static_cast<std::uint16_t>(parsed); return true;
    };
    for (unsigned considered = 0; !list.empty() && considered < 4 && !stop.stop_requested(); ++considered) {
        const auto comma = list.find(',');
        auto uri = list.substr(0, comma);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        if (!uri.starts_with("turn:")) continue;
        uri.remove_prefix(5);
        std::uint16_t port = 3478;
        if (uri.starts_with('[')) {
            // Bracketed IPv6 literal with an optional port.
            const auto close = uri.find(']');
            if (close == std::string_view::npos) continue;
            const auto rest = uri.substr(close + 1);
            if (!socket::numeric_ip(std::string(uri.substr(1, close - 1)), true)
                || (!rest.empty() && (rest.front() != ':' || !parse_port(rest.substr(1), port)))) continue;
            add("turn:[" + std::string(uri.substr(1, close - 1)) + "]:" + std::to_string(port));
            continue;
        }
        std::string host(uri);
        if (const auto colon = uri.rfind(':'); colon != std::string_view::npos) {
            if (!parse_port(uri.substr(colon + 1), port)) continue;
            host = std::string(uri.substr(0, colon));
        }
        if (host.empty()) continue;
        if (std::all_of(host.begin(), host.end(), [](char ch) { return (ch >= '0' && ch <= '9') || ch == '.'; })) {
            // Digits and dots are an address or nothing; never a hostname for the resolver.
            if (socket::numeric_ip(host, false)) add("turn:" + host + ":" + std::to_string(port));
            continue;
        }
        if (!clean(host) || host.find_first_of("[]:%/?@") != std::string::npos) continue;
        std::vector<std::string> v4, v6;
        for (auto& address : resolver(host, port)) {
            if (address.empty() || address.find('%') != std::string::npos) continue;
            if (address.find(':') == std::string::npos ? socket::numeric_ip(address, false) : socket::numeric_ip(address, true))
                (address.find(':') == std::string::npos ? v4 : v6).push_back(std::move(address));
        }
        const bool use_v4 = !v4.empty();
        auto& chosen = use_v4 ? v4 : v6;
        for (std::size_t i = 0; i < chosen.size() && i < 2; ++i)
            add("turn:" + (use_v4 ? chosen[i] : "[" + chosen[i] + "]") + ":" + std::to_string(port));
    }
    std::string joined;
    for (const auto& entry : entries) joined += (joined.empty() ? "" : ",") + entry;
    return joined;
}
} // namespace eawr::net::relay
