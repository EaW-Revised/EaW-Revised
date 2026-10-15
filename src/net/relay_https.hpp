#pragma once

#include "relay_tls.hpp"

#include <chrono>
#include <functional>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace eawr::net::relay {
// Bounded DNS and TCP connection on a caller-owned I/O worker. IPv4 first,
// at most two addresses; the returned socket belongs to the caller.
socket::Handle connect_service(const std::string& host, std::uint16_t port,
    std::chrono::steady_clock::time_point deadline, std::stop_token stop = {});
// Plain TLS byte channel over a nonblocking native socket, used by the outbound
// HTTPS client (and by tests as a fake HTTPS endpoint). It never owns the socket.
// Every call is nonblocking: wants_io means retry after the socket may have moved.
struct TlsChannelOptions {
    bool server{};
    // Client: SNI and certificate name. Always verified; there is no insecure mode.
    std::string server_name;
    // Client: when set, only this PEM bundle is trusted (tests, private CAs).
    // Empty selects the operating system trust store.
    std::string ca_file;
    // Server: PEM certificate chain and private key.
    std::string certificate, key;
};
enum class TlsStep { done, wants_io, closed, failed };
class TlsChannel {
public:
    virtual ~TlsChannel() = default;
    virtual TlsStep handshake() = 0;
    virtual TlsStep write(std::span<const std::uint8_t> bytes, std::size_t& written) = 0;
    virtual TlsStep read(std::span<std::uint8_t> bytes, std::size_t& count) = 0;
};
// Returns null when the options or credentials are unusable.
std::unique_ptr<TlsChannel> open_tls_channel(socket::Handle handle, const TlsChannelOptions& options);

struct HttpsRequest {
    std::string host;            // DNS name: Host header, SNI and certificate check
    std::string connect_address; // optional numeric address replacing DNS (tests)
    std::uint16_t port{443};
    std::string method{"POST"}, path, bearer, body;
    std::string ca_file;         // empty selects the system trust store
    std::chrono::milliseconds timeout{5000};
};
struct HttpsResponse { int status{}; std::string body; };
// One bounded request/response exchange on a fresh connection. Blocks the calling
// worker thread up to `timeout` and stops early on `stop`. Failures are typed
// EAWR-NET-1556 and never contain the request, bearer token or response body.
core::Result<HttpsResponse> https_request(const HttpsRequest& request, std::stop_token stop);

// Client side of a provider's `turn:` list: GNS takes numeric addresses only, so
// hostnames (turn.cloudflare.com) become numeric `turn:<ip>:<port>` entries; numeric
// URIs (coturn) pass through unchanged. IPv4 is preferred, at most two addresses per
// hostname and eight entries in total; unresolved hostnames are dropped. Resolution
// is bounded by `budget`, but still call this from a worker thread. The default resolver is the system one.
// Input is capped at four entries before any lookup; every system lookup shares `budget` and honours `stop`.
using TurnHostResolver = std::function<std::vector<std::string>(const std::string& host, std::uint16_t port)>;
std::string numeric_turn_uris(std::string_view list, const TurnHostResolver& resolver = {},
    std::chrono::milliseconds budget = std::chrono::seconds(5), std::stop_token stop = {});
} // namespace eawr::net::relay
