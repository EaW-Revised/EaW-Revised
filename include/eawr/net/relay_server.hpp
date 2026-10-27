#pragma once

#include "eawr/core/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace eawr::net {
class TurnRefresh; // Internal credential-refresh step (src/net/relay_turn_refresh.hpp).
// Where relay credentials come from. coturn: HMAC REST credentials from turn_uri and
// turn_secret. cloudflare: one outbound HTTPS call per pairing to Cloudflare Realtime TURN.
enum class TurnProvider : std::uint8_t { coturn, cloudflare };
struct RelayServerOptions {
    std::string listen_address{"127.0.0.1"};
    std::uint16_t port{27020};
    std::string certificate_file;
    std::string key_file;
    std::string access_token;
    bool require_access_token{};
    bool room_aliases{};
    std::size_t failed_guesses_per_minute{100};
    std::string turn_uri;
    std::string turn_secret;
    TurnProvider turn_provider{TurnProvider::coturn};
    // Secrets, used only by the cloudflare provider; never logged or placed in an error.
    std::string cloudflare_key_id;
    std::string cloudflare_api_token;
    std::size_t cloudflare_calls_per_minute{120};
    std::chrono::milliseconds cloudflare_timeout{4000};
    // Contract seams for a local fake HTTPS endpoint; the host name stays rtc.live.cloudflare.com
    // and its certificate is still verified. Production leaves these at their defaults.
    std::string cloudflare_connect_address;
    std::string cloudflare_ca_file;
    std::uint16_t cloudflare_port{443};
    // Receives sanitized warnings (e.g. EAWR-NET-1556 when Cloudflare is unreachable).
    // Called on the poll owner thread only.
    std::function<void(const core::Diagnostic&)> diagnostics;
    std::size_t max_connections{1024};
    std::size_t max_sessions{512};
    // 18 = 2 x max_match_seats: nine room members behind one NAT, each with a directory and a room connection.
    std::size_t connections_per_ip{18};
    std::size_t unpaired_sessions_per_ip{2};
    std::size_t accepts_per_minute{30};
    std::size_t operations_per_minute{120};
    std::size_t bytes_per_minute{1024 * 1024};
    std::size_t directory_operations_per_minute{2048};
    std::size_t directory_bytes_per_minute{8 * 1024 * 1024};
    std::size_t queue_bytes{256 * 1024};
    std::size_t queue_frames{64};
    std::chrono::milliseconds admission_timeout{10000};
    std::chrono::milliseconds max_join_lifetime{900000};
    std::chrono::milliseconds idle_timeout{60000};
    std::chrono::milliseconds session_lifetime{7200000};
    // Ticket E: paired peers renew the lease before it ends. Each renewal adds session_lifetime, never past
    // max_session_lifetime counted from pairing. A peer may renew once the lease has renewal_window left; the
    // service also warns both peers then (the effective window is at most half of session_lifetime).
    std::chrono::milliseconds max_session_lifetime{86400000};
    std::chrono::milliseconds renewal_window{600000};
    // Menu channel (#3758): the room-less presence list and chat of the online room browser. A prefix may carry up to
    // connections_per_ip menu connections (players behind one NAT); 30 polls a minute per client leave room for chat.
    std::size_t menu_operations_per_minute{1024};
    std::size_t menu_bytes_per_minute{4 * 1024 * 1024};
    std::size_t menu_max_members{512};
    std::size_t menu_recent_lines{50};
    std::chrono::milliseconds menu_line_lifetime{600000};
    // N-peer rooms (slice N3). Room members have budgets of their own, apart from the legacy per-session and
    // per-prefix ones (players share a NAT): a member, a whole room (capacity x member) and a prefix
    // (connections_per_ip x member). Provisional until the introduced-path test measures the signal volume.
    std::size_t room_operations_per_member_per_minute{480};
    std::size_t room_bytes_per_member_per_minute{2 * 1024 * 1024};
    std::chrono::milliseconds room_reservation_timeout{30000}; // an admitted guest must attach within this
    std::chrono::milliseconds room_hold_timeout{300000};       // a started match holds a lost seat this long
};

// One owner thread calls poll. Uses the existing native socket layer and TLS;
// contains no game, simulation, GNS or viewer dependency.
class RelayServer final {
public:
    using Clock = std::chrono::steady_clock;
    using Now = Clock::time_point (*)();
    // Hash provider seam for deterministic worker-boundary tests. Null selects
    // the standard salted PBKDF2 implementation; callbacks run on the hash worker.
    using PasswordHash = core::Result<std::string> (*)(std::string_view, std::string_view);
    // Credential refresh seam for deterministic renewal tests; null issues the HMAC (coturn) credentials.
    static core::Result<std::unique_ptr<RelayServer>> listen(RelayServerOptions options, Now now = Clock::now, PasswordHash hash = nullptr,
        std::shared_ptr<TurnRefresh> refresh = nullptr);
    ~RelayServer();
    void poll();
    std::uint16_t port() const;
    std::size_t connections() const;
    std::size_t sessions() const;
    std::size_t rooms() const;
    // Operator moderation, called on the same owner thread as poll().
    bool remove_chat_member(const std::string& room, std::uint32_t seat);
private:
    struct Impl;
    explicit RelayServer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace eawr::net
