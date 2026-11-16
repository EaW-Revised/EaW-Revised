#pragma once

#include "eawr/net/gns_transport.hpp"
#include "eawr/net/transport.hpp"

#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::net {
// GNS d534c19a, CConnectionTransportP2PICE::TransportPopulateConnectionInfo:
// TURN sets Relayed and clears the remote IP; direct ICE publishes that IP.
// SDR POPs, missing addresses and conflicting flags remain unknown. No status-text
// tokens are used: that revision's detailed printer omits the ICE transport kind.
inline GnsRoute classify_gns_route(bool connected, bool relayed, bool remote_ip, bool sdr_pop) {
    if (!connected || sdr_pop) return GnsRoute::unknown;
    if (relayed) return remote_ip ? GnsRoute::unknown : GnsRoute::relayed;
    return remote_ip ? GnsRoute::direct : GnsRoute::unknown;
}
// The TURN server named by GNS's "ICE: TURN server <addr> accepted a request (message 0x..)" line (the carried patch emits one per
// reply, so one shortly after every allocation and every refresh); empty for any other text.
inline std::string turn_server_from_spew(std::string_view line) {
    constexpr std::string_view head = "TURN server ", tail = " accepted a request";
    const auto start = line.find(head);
    if (start == std::string_view::npos) return {};
    const auto from = start + head.size();
    const auto end = line.find(tail, from);
    if (end == std::string_view::npos || end == from) return {};
    return std::string(line.substr(from, end - from));
}
// Which provider carries a relayed pair. `accepted` is the comma list of "host:port" TURN servers that accepted a request in this
// process lately (the log line cannot say which connection it belongs to), `own` the operator's own numeric entries (nullopt = the
// endpoint does not classify), `all` the full credential list. Entries are "turn:host:port" with an optional "?query". Every
// accepting server must be the operator's own for "own", and every one listed but not own for "cloudflare"; a mix or an unlisted
// server is unknown.
inline GnsRelayProvider classify_relay_provider(GnsRoute route, std::string_view accepted, const std::optional<std::string>& own,
        std::string_view all) {
    if (route != GnsRoute::relayed) return GnsRelayProvider::none;
    if (accepted.empty() || !own) return GnsRelayProvider::unknown;
    const auto listed = [](std::string_view list, std::string_view server) {
        for (std::size_t begin = 0; begin < list.size();) {
            auto comma = list.find(',', begin);
            auto entry = list.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
            begin = comma == std::string_view::npos ? list.size() : comma + 1;
            if (!entry.starts_with("turn:")) continue;
            entry.remove_prefix(5);
            entry = entry.substr(0, entry.find('?'));
            if (entry == server) return true;
        }
        return false;
    };
    bool any_own = false, any_other = false;
    for (std::size_t begin = 0; begin < accepted.size();) {
        auto comma = accepted.find(',', begin);
        const auto server = accepted.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
        begin = comma == std::string_view::npos ? accepted.size() : comma + 1;
        if (listed(*own, server)) any_own = true;
        else if (listed(all, server)) any_other = true;
        else return GnsRelayProvider::unknown;
    }
    if (any_own == any_other) return GnsRelayProvider::unknown;
    return any_own ? GnsRelayProvider::own : GnsRelayProvider::cloudflare;
}
// One word for the log and the overlay: "direct", "relay (own)", "relay (cloudflare)", "relay" or "unknown".
inline std::string route_text(GnsRoute route, GnsRelayProvider provider) {
    if (route == GnsRoute::direct) return "direct";
    if (route != GnsRoute::relayed) return "unknown";
    return provider == GnsRelayProvider::own ? "relay (own)" : provider == GnsRelayProvider::cloudflare ? "relay (cloudflare)" : "relay";
}
// A relayed pair whose receive loss stays high: raised at >= 15% for 20 s in a row, cleared at < 5% for 10 s in a row (or when the pair
// stops being relayed). Observation only: it picks no route and changes no traffic (the in-match relay switch is a separate,
// agreed design). Called once per stats tick (about 1 s) on the I/O owner's thread; a healthy pair costs two comparisons.
class GnsLossHint {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr double raise_pct = 15.0, clear_pct = 5.0;
    static constexpr std::chrono::seconds raise_after{20}, clear_after{10};
    // True when active() changed on this observation.
    bool observe(Clock::time_point now, bool relayed, std::optional<double> loss_pct) {
        const bool before = active_;
        if (!relayed || !loss_pct) { if (!relayed) reset(); return before != active_; }
        if (*loss_pct >= raise_pct) {
            calm_since_.reset();
            if (!high_since_) high_since_ = now;
            if (!active_ && now - *high_since_ >= raise_after) { active_ = true; peak_ = *loss_pct; }
            if (active_ && *loss_pct > peak_) peak_ = *loss_pct;
        } else {
            high_since_.reset();
            if (*loss_pct < clear_pct) {
                if (!calm_since_) calm_since_ = now;
                if (active_ && now - *calm_since_ >= clear_after) { active_ = false; peak_ = 0; }
            } else calm_since_.reset();
        }
        return before != active_;
    }
    bool active() const { return active_; }
    double peak_pct() const { return peak_; }
    void reset() { active_ = false; peak_ = 0; high_since_.reset(); calm_since_.reset(); }
private:
    bool active_{};
    double peak_{};
    std::optional<Clock::time_point> high_since_, calm_since_;
};
// Local cadence observations of the SDK's integer-millisecond estimate. These
// are not native probe counts, and getters never advance the sampler.
class GnsLinkStatsSampler {
public:
    using Clock = std::chrono::steady_clock;
    bool due(Clock::time_point now) const {
        return !attempted_ || now - *attempted_ >= std::chrono::seconds(1);
    }
    void observe(Clock::time_point now, std::optional<int> ping_ms, std::optional<double> loss_pct = std::nullopt) {
        if (!due(now)) return;
        attempted_ = now;
        if (!ping_ms || *ping_ms < 0) return;
        const auto sample = std::chrono::microseconds(static_cast<std::int64_t>(*ping_ms) * 1000);
        if (stats_.sample_count == 0) stats_.smoothed_rtt = sample;
        else {
            const auto delta = sample > stats_.last_rtt ? sample - stats_.last_rtt : stats_.last_rtt - sample;
            stats_.jitter += (delta - stats_.jitter) / 4;
            stats_.smoothed_rtt += (sample - stats_.smoothed_rtt) / 8;
        }
        stats_.last_rtt = sample;
        if (loss_pct) stats_.loss_pct = *loss_pct;
        if (stats_.sample_count != std::numeric_limits<std::uint64_t>::max()) ++stats_.sample_count;
    }
    std::optional<LinkStats> cached() const {
        if (stats_.sample_count == 0) return {};
        return stats_;
    }
    void reset() { attempted_.reset(); stats_ = {}; }
private:
    std::optional<Clock::time_point> attempted_;
    LinkStats stats_;
};
} // namespace eawr::net
