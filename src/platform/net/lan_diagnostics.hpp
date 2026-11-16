#pragma once
#include "lan_match.hpp"

#include <array>
#include <sstream>
#include <map>

namespace eawr::presentation::godot_backend {
// The route of a GNS pair as the report names it: direct, relayed by the project's own relay or by the Cloudflare one (plain
// "relay" while the provider is not known), and the transport's "lossy for a while" flag for a relayed pair.
[[nodiscard]] inline presentation::ui::NetRouteInfo net_route_of(const net::GnsPairDiagnostic& pair) {
    using Route = presentation::ui::NetRoute;
    presentation::ui::NetRouteInfo info;
    switch (pair.route) {
    case net::GnsRoute::direct: info.route = Route::direct; break;
    case net::GnsRoute::relayed:
        info.route = pair.relay_provider == net::GnsRelayProvider::own ? Route::relay_own
            : pair.relay_provider == net::GnsRelayProvider::cloudflare ? Route::relay_cloudflare : Route::relay;
        info.lossy = pair.route_lossy;
        break;
    case net::GnsRoute::unknown: break;
    }
    return info;
}

// Owned by setup's main thread, then exclusively by the live I/O driver. Main-thread
// cancellation waits for that driver before reuse; detached drains drop this observer.
// No addresses, credentials, payloads or hashes enter this log.
class LanDiagnostics {
public:
    std::function<void(std::string_view)> sink;
    net::PeerIndex seat{};
    std::uint64_t tick{};
    // #4043: counts every lockstep wait, however short (stall() above only reports waits of a second or more).
    presentation::ui::NetStallLedger ledger;
    void emit(std::string_view event, std::string_view reason, net::PeerIndex peer,
            std::string_view detail = {}) const {
        if (!sink) return;
        std::ostringstream line;
        line << "EAWR net event=" << event << " seat=" << seat << " peer=" << peer
             << " tick=" << tick << " reason=" << reason;
        if (!detail.empty()) line << ' ' << detail;
        // A diagnostic consumer cannot terminate a live match.
        try { sink(line.str()); } catch (...) {}
    }
    template<class Reason>
    void links(const net::MessageTransport& transport, Reason&& reason) {
        seat = transport.self();
        for (net::PeerIndex peer = 0; peer < transport.peer_count(); ++peer) {
            if (peer == seat) continue;
            const auto state = transport.link(peer);
            const auto previous = links_.find(peer);
            if (previous != links_.end() && previous->second == state) continue;
            const auto code = reason(peer);
            if (previous != links_.end() && previous->second == net::LinkState::open && state == net::LinkState::closed)
                emit("link-lost", code, peer);
            constexpr std::array names{"connecting", "open", "closed"};
            emit("link", code, peer, std::string("state=") + names[static_cast<std::size_t>(state)]);
            links_[peer] = state;
        }
    }
    // Connect-time route evidence: one line when a peer's route first becomes known and again when it changes
    // (direct or relayed, and by which relay). An unknown route is not logged.
    void route(net::PeerIndex peer, const presentation::ui::NetRouteInfo info, const double loss_pct = 0.0) {
        if (info.route == presentation::ui::NetRoute::unknown) {
            // A flagged peer whose route is gone (link closed): say it recovered, on the route it was flagged on.
            if (const auto flagged = lossy_.find(peer); flagged != lossy_.end() && flagged->second) {
                flagged->second = false;
                const auto last = routes_.find(peer);
                emit("route-recovered", presentation::ui::net_route_text(last != routes_.end() ? last->second : info.route), peer,
                    "loss_pct=" + std::to_string(static_cast<int>(loss_pct + 0.5)));
            }
            return;
        }
        const auto [at, inserted] = routes_.try_emplace(peer, info.route);
        if (inserted || at->second != info.route) {
            at->second = info.route;
            emit("route", presentation::ui::net_route_text(info.route), peer);
        }
        // The transport's degraded-route flag: one line when it sets and one when it clears (observation only).
        if (info.lossy != lossy_[peer]) {
            lossy_[peer] = info.lossy;
            emit(info.lossy ? "route-degraded" : "route-recovered", presentation::ui::net_route_text(info.route), peer,
                "loss_pct=" + std::to_string(static_cast<int>(loss_pct + 0.5)));
        }
    }
    void view(const LanView& value) {
        if (value.recovery != recovery_) {
            constexpr std::array names{"none", "waiting", "held", "catching-up", "proving", "rejoin-failed", "desync", "resyncing", "recovered", "failed"};
            emit("recovery", names[static_cast<std::size_t>(value.recovery)], value.reconnect_seat,
                "epoch=" + std::to_string(value.recovery_epoch));
            recovery_ = value.recovery;
        }
        if (value.phase == LanPhase::ended && phase_ != LanPhase::ended) {
            constexpr std::array reasons{"none", "version", "build", "identity", "refused", "timeout", "desync", "peer-left", "cancelled"};
            emit("battle-end", lan_connection_loss(value.diagnostic_code) ? "connection-loss" : reasons[static_cast<std::size_t>(value.reason)], seat,
                "code=" + (value.diagnostic_code.empty() ? std::string("none") : value.diagnostic_code));
        }
        phase_ = value.phase;
    }
    // Ignore subsecond completeness gaps. Announced intervals always get an end;
    // a quick end waits for the next poll so stall lines stay at most one per second.
    bool stall(bool waiting, std::uint64_t now, std::span<const net::PeerIndex> peers = {}) {
        if (!waiting && since_) {
            if (announced_) { end_ = "start_tick=" + std::to_string(start_tick_) + " end_tick=" + std::to_string(tick) + " duration_ms=" + std::to_string(now - *since_); end_peer_ = waiting_peer_; }
            since_.reset(); announced_ = false;
        }
        if (end_ && due(now)) {
            emit("stall-end", "resumed-or-ended", end_peer_, *end_);
            last_stall_ = now; end_.reset();
        }
        if (!waiting) return false;
        if (!since_) { since_ = now; start_tick_ = tick; waiting_peer_ = peers.empty() ? seat : peers.front(); }
        if (!announced_ && !end_ && now - *since_ >= 1000 && due(now)) {
            std::string detail = "start_tick=" + std::to_string(start_tick_) + " missing=";
            for (const auto peer : peers) detail += std::to_string(peer) + ",";
            emit("stall-start", "lockstep-wait", waiting_peer_, detail);
            announced_ = true; last_stall_ = now;
            return true;
        }
        return false;
    }
private:
    bool due(std::uint64_t now) const { return !last_stall_ || now - *last_stall_ >= 1000; }
    std::map<net::PeerIndex, net::LinkState> links_;
    std::map<net::PeerIndex, presentation::ui::NetRoute> routes_;
    std::map<net::PeerIndex, bool> lossy_;
    LanPhase phase_{LanPhase::idle};
    LanRecovery recovery_{LanRecovery::none};
    std::optional<std::uint64_t> since_, last_stall_;
    std::optional<std::string> end_;
    std::uint64_t start_tick_{};
    net::PeerIndex waiting_peer_{}, end_peer_{};
    bool announced_{};
};
}
