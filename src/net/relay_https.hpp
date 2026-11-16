#pragma once

#include "eawr/net/gns_signalling.hpp"
#include "relay_rtt.hpp"
#include "relay_tls.hpp"

#include <chrono>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
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

// The same rewrite for credentials that may hold several providers (a Cloudflare hostname list plus the operator's own coturn):
// each listed entry keeps its own user and password through the hostname-to-address step, and `provider` first drops the
// entries of the other provider, judged by the host name the service sent. Several addresses of one entry share its
// credential. Equal users (or passwords) across the result collapse to one value, as a single-provider frame carries them. The
// result has an empty uri when no entry is left or none resolves; lists that do not fit their servers come back unchanged for
// the transport to refuse. `budget` is shared by all lookups.
struct TurnStrings {
    std::string uri, username, password;
    bool operator==(const TurnStrings&) const = default;
};
//
// Geography: with `automatic` and both providers listed, a non-empty `probe` measures the round-trip time to our own server
// and to Cloudflare (stun.cloudflare.com, the same anycast network as its TURN service; Cloudflare's TURN addresses ignore
// unauthenticated binding requests) and puts the lower first: our own server only when it is not slower than Cloudflare by more than
// `turn_own_bias`, so a player far from our single host is relayed through Cloudflare's nearest edge. A probe that gets no answer
// from either side keeps the configured order (own first); one that answers only from one side picks that side. Each probe takes
// at most `turn_probe_cap`. This chooses a route by distance, never by loss: the game's own fallback to the second server
// is failure-based.
// Round-trip time to a UDP endpoint that answers STUN, or nothing when it does not answer within `cap`. `ipv4` is a numeric
// address. Never called from the game thread. stun_rtt is the real one; tests inject a fake.
using RttProbe = std::function<std::optional<std::chrono::microseconds>(const std::string& ipv4, std::uint16_t port,
    std::chrono::milliseconds cap, std::stop_token stop)>;
struct TurnOrder {
    bool probed{};          // both providers were listed and a probe ran
    int own_ms{-1};         // median RTT, -1 = no answer
    int cloudflare_ms{-1};
    bool own_first{true};   // the order the result has
    bool classified{};      // own_uris is meaningful (credentials were rewritten)
    std::string own_uris;   // numeric entries of the operator's own provider (comma list), set whenever an order is requested
};
inline constexpr std::chrono::milliseconds turn_own_bias{15}, turn_probe_cap{100};
TurnStrings numeric_turn_credentials(const TurnStrings& listed, GnsTurnProvider provider, const TurnHostResolver& resolver = {},
    std::chrono::milliseconds budget = std::chrono::seconds(5), std::stop_token stop = {}, const RttProbe& probe = {},
    TurnOrder* order = nullptr);
// One log line (event "turn") with the chosen order and the measured RTTs; nothing without a probe result or a log.
using TurnLog = std::function<void(std::string_view event, std::string_view text)>;
void log_turn_order(const TurnOrder& order, const TurnLog& log);
} // namespace eawr::net::relay
