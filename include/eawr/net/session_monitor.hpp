#pragma once

#include "eawr/net/transport.hpp"
#include <functional>
#include <string_view>

namespace eawr::net {

// Number of reachable sealed ticks, including the first missing tick used as a
// departure cutoff. The final accepted batch is one tick before this boundary.
inline constexpr std::uint64_t sealed_input_horizon(std::uint64_t input_delay) noexcept {
    return 2 * input_delay + 2;
}

// LAN-4 records tick zero and every completed tick; views are valid only during record().
// Inputs use the qualified loopback's replay encoding (empty batch: empty bytes).
class SessionMonitor {
public:
    virtual ~SessionMonitor() = default;
    virtual bool needs_encoding(std::uint64_t tick, bool match_end = false) const = 0;
    virtual core::Result<void> record(std::uint64_t completed_tick, std::string_view hash,
        std::span<const std::uint8_t> canonical_state, std::span<const std::uint8_t> canonical_input,
        bool match_end = false) = 0;
    virtual core::Result<void> receive(const Received& received) = 0; // Message kinds 32..39.
    // Agreed active delay plus a bounded transition envelope for immutable old inputs.
    virtual core::Result<void> on_input_delay(std::uint64_t delay, std::uint64_t retained_delay) {
        static_cast<void>(delay); static_cast<void>(retained_delay);
        return core::Result<void>::success();
    }
    // LAN-6: exclude this peer from pending/future exchanges at the agreed boundary.
    virtual core::Result<void> on_departure(PeerIndex peer, std::uint64_t tick) {
        static_cast<void>(peer); static_cast<void>(tick);
        return core::Result<void>::success();
    }
    // Required summary/detail voters, including a freeze or terminal exchange.
    // Closed streams remain errors there; a real freeze never runs liveness expiry.
    virtual std::vector<PeerIndex> waiting_for_peers() const { return {}; }
    virtual bool frozen() const { return false; }
    virtual bool ready_for_resync() const { return false; }
    virtual core::Result<void> on_resync(std::uint64_t tick, std::string_view hash) {
        static_cast<void>(tick); static_cast<void>(hash);
        core::Diagnostic error;
        error.code = "EAWR-NET-1207";
        error.message = "monitor does not support a resync barrier";
        return core::Result<void>::failure(std::move(error));
    }
    // Authenticated stream replacement (and bounded retries while a mesh hash
    // gate waits). A self call enables retry semantics before the zero barrier
    // without sending. Ownership identifies peers whose failed sends belong to
    // the mesh decision. Resend retained evidence, never reset the comparison.
    virtual core::Result<void> on_link_repair(PeerIndex, std::function<bool(PeerIndex)> = {}) {
        core::Diagnostic error;
        error.code = "EAWR-NET-1790"; error.message = "Monitor does not support authenticated link repair";
        return core::Result<void>::failure(std::move(error));
    }
    virtual bool may_step() const = 0; // False while an exchange is pending or the match is frozen.
    // All active peers have independently proved this exact completed state.
    // Unknown monitor implementations fail closed instead of losing diagnostics.
    virtual core::Result<void> on_reconnect(std::uint64_t tick, std::string_view hash,
        std::span<const PeerIndex> inactive) {
        static_cast<void>(tick); static_cast<void>(hash); static_cast<void>(inactive);
        core::Diagnostic error;
        error.code = "EAWR-NET-1010";
        error.message = "monitor does not support a reconnect barrier";
        return core::Result<void>::failure(std::move(error));
    }
};

} // namespace eawr::net
