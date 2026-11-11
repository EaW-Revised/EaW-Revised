#pragma once

#include "galactic_lan_match.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::viewer::galactic {

// Lobby identities are presentation data; neither names nor notices enter the simulation.
struct LanPlayer {
    net::PeerIndex peer{};
    sim::SeatId seat{};
    std::string player;
    std::string faction;
    // The lobby supplies the configured fallback (including a remaining co-op human).
    std::string departure_control;
};

// A refused recovery names its reason in plain words (#6040). The codes come from net::galactic_recovery_reason.
[[nodiscard]] inline std::string lan_recovery_refusal_line(const std::string& code) {
    if (code.empty()) return {};
    struct Row { const char* code; const char* text; };
    static constexpr Row rows[]{
        {"checkpoint_unavailable", "no saved checkpoint is available"},
        {"divergent_epoch", "the campaigns have already split"},
        {"anchor_hash_mismatch", "the saved checkpoint does not match this campaign"},
        {"log_too_old", "too much play has passed since the checkpoint"},
        {"authenticated_seat_mismatch", "the returning player does not match the saved seat"},
        {"malformed_catchup", "the catch-up data was invalid"},
        {"catchup_limit", "the catch-up data is too large"},
        {"private_restore_failed", "the catch-up could not be restored"},
        {"private_replay_failed", "the catch-up could not be replayed"},
        {"catchup_state_hash_mismatch", "the catch-up state does not match"},
        {"stale_publication_barrier", "the catch-up took too long or was overtaken by the match"},
        {"catchup_in_progress", "another recovery is already in progress"},
        {"catchup_cancelled", "the recovery was cancelled"},
    };
    for (const auto& row : rows)
        if (code == row.code) return std::string("Recovery refused: ") + row.text + ".";
    return "Recovery refused (" + code + ").";
}

// #5355 (TP-MP06): the battle-speed notice of a LAN GC battle child, polled per HUD refresh. `events` is the match's
// lock-free clock event count and `since` its value when the battle opened; `known` is the count the notice last took.
// Only a count the notice has not taken reads the view (`read_view`: owner lock and copy), once, and then `line` gets
// "<name> set the battle speed to 2x" when that event is a battle speed change (empty for a pause or a galactic speed).
// Returns the count to remember. Events from before the battle never name anyone in it.
template <class ReadView>
[[nodiscard]] std::uint64_t battle_speed_notice(const std::uint64_t events, const std::uint64_t since, const std::uint64_t known,
    ReadView&& read_view, std::span<const LanPlayer> players, std::string& line) {
    if (events == known || events <= since) return known;
    const presentation::godot_backend::GalacticLanView view = read_view();
    if (!view.clock_action.starts_with("set the battle speed")) return events;
    const auto at = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.seat == view.clock_actor; });
    line = (at != players.end() && !at->player.empty() ? at->player : "Player " + std::to_string(view.clock_actor.value))
        + " " + view.clock_action;
    return events;
}

// #6276: a child HUD reads the owner snapshot and formats a pause line only when its pause revision changes.
// The first refresh includes an already paused match; the line stays until the agreed pause or gameplay state changes.
class BattlePauseNotice final {
public:
    template <class ReadView>
    [[nodiscard]] const std::string& refresh(const std::uint64_t revision, ReadView&& read_view,
        std::span<const LanPlayer> players) {
        if (revision_ && *revision_ == revision) return text_;
        const presentation::godot_backend::GalacticLanView view = read_view();
        revision_ = revision;
        text_.clear();
        if (!view.clock_paused || !presentation::godot_backend::galactic_lan_gameplay_enabled(view)) return text_;
        const auto at = std::find_if(players.begin(), players.end(), [&](const auto& player) { return player.seat == view.clock_actor; });
        text_ = "Paused by " + (at != players.end() ? at->player : "Player " + std::to_string(view.clock_actor.value));
        return text_;
    }
private:
    std::optional<std::uint64_t> revision_;
    std::string text_;
};

[[nodiscard]] inline std::string lan_notice(
    const presentation::godot_backend::GalacticLanView& view, std::span<const LanPlayer> players) {
    using namespace presentation::godot_backend;
    const auto seat_name = [&](sim::SeatId seat) {
        const auto at = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.seat == seat; });
        return at != players.end() && !at->player.empty() ? at->player : "Player " + std::to_string(seat.value);
    };
    if (view.phase == GalacticLanPhase::ended) {
        std::string reason;
        switch (view.reason) {
        case GalacticLanReason::completed: reason = "Galactic match completed."; break;
        case GalacticLanReason::local_left: reason = "You left the Galactic match."; break;
        case GalacticLanReason::desync: reason = "Galactic match ended: players' states diverged."; break;
        case GalacticLanReason::stalled: reason = "Galactic match ended: a peer stopped responding."; break;
        case GalacticLanReason::link_lost: reason = "Galactic match ended: connection lost."; break;
        default: reason = "Galactic match ended."; break;
        }
        if (!view.message.empty()) reason += "\n" + view.message;
        // Only when recovery itself ended the match: a plain quit (which also carries a message) or a completion does
        // not repeat a standing refusal.
        if (view.recovery_ended && !view.recovery_refusal.empty()) reason += "\n" + lan_recovery_refusal_line(view.recovery_refusal);
        return reason;
    }
    std::string text;
    if (view.phase == GalacticLanPhase::connecting) text = "Connecting to Galactic match";
    if (view.phase == GalacticLanPhase::waiting_for_peer) {
        text = "Waiting for ";
        bool first = true;
        for (const auto peer : view.waiting_for) {
            if (!first) text += ", ";
            first = false;
            const auto at = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.peer == peer; });
            text += at != players.end() && !at->player.empty() ? at->player : "peer " + std::to_string(peer + 1);
        }
    }
    for (const auto seat : view.departed) {
        if (!text.empty()) text += '\n';
        text += seat_name(seat) + " left";
        const auto at = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.seat == seat; });
        if (at != players.end() && !at->departure_control.empty()) text += "; " + at->departure_control;
        else text += "; configured faction control takes over";
    }
    if (!view.departed.empty()) {
        if (players.size() == 2)
            text += "\nA two-player link split continues as separate campaigns. You can continue or quit.";
        text += "\nRejoin is disabled for Galactic multiplayer.";
    }
    return text;
}


// A leaver must never stop the match: the shared pause belongs to nobody once the seat that took it has left. The lowest
// remaining human seat resumes it, as an ordinary seat-issued Resume in the agreed input (no new rule, no new wire frame).
[[nodiscard]] inline bool lan_releases_departed_pause(const presentation::godot_backend::GalacticLanView& view,
    std::span<const LanPlayer> players, sim::SeatId local) {
    using namespace presentation::godot_backend;
    if (view.phase == GalacticLanPhase::ended || !view.clock_paused) return false;
    const auto departed = [&](sim::SeatId seat) {
        return std::find(view.departed.begin(), view.departed.end(), seat) != view.departed.end();
    };
    if (!departed(view.clock_actor)) return false;
    const LanPlayer* lowest = nullptr;
    for (const auto& player : players)
        if (!departed(player.seat) && (!lowest || player.seat.value < lowest->seat.value)) lowest = &player;
    return lowest && lowest->seat == local;
}

// #5355: the clock bar's fast-forward tooltip in a LAN match. 2x is a request: lockstep runs at the slowest seat.
[[nodiscard]] inline std::string lan_speed_tooltip() {
    return "Shared clock: any active human may set 2x or back to 1x; " + std::to_string(net::ClockThrottle::speed_changes_per_window)
        + " speed changes per " + std::to_string(net::ClockThrottle::window_seconds)
        + " session seconds per player. 2x is a request: the slowest player's computer sets the real pace.";
}

// #5355: measures the agreed galactic tick rate that really runs, from the completed session tick against the
// presentation clock. Fixed storage takes at most one sample per 100 ms. A new requested speed, a pause, a battle
// or a gap in the samples starts a new measurement.
class LanPaceMeter final {
public:
    static constexpr std::uint64_t window_ms = 3000;
    static constexpr std::uint64_t min_span_ms = 1000;
    static constexpr std::uint64_t max_gap_ms = 1000;

    static constexpr std::uint64_t sample_ms = 100;
    static constexpr std::size_t capacity = window_ms / sample_ms + 2;

    void sample(std::uint64_t completed_tick, std::uint64_t now_ms, std::int64_t requested_raw, bool running) noexcept {
        if (count_ != 0) {
            const auto& last = at(count_ - 1);
            if (!running || requested_raw != requested_raw_ || now_ms < last.ms || now_ms - last.ms > max_gap_ms
                || completed_tick < last.tick) first_ = count_ = 0;
            else if (now_ms - last.ms < sample_ms) return;
        }
        requested_raw_ = requested_raw;
        if (!running) return;
        if (count_ == capacity) drop_first();
        ring_[(first_ + count_) % capacity] = {completed_tick, now_ms};
        ++count_;
        while (count_ > 2 && now_ms - at(1).ms >= window_ms) drop_first();
    }
    // Ticks per wall second over the window, once it spans at least min_span_ms.
    [[nodiscard]] std::optional<double> ticks_per_second() const noexcept {
        if (count_ < 2) return std::nullopt;
        const auto& first = at(0);
        const auto& last = at(count_ - 1);
        const auto span = last.ms - first.ms;
        if (span < min_span_ms) return std::nullopt;
        return static_cast<double>(last.tick - first.tick) * 1000.0 / static_cast<double>(span);
    }
    [[nodiscard]] std::size_t samples() const noexcept { return count_; }
private:
    struct Sample { std::uint64_t tick{}, ms{}; };
    [[nodiscard]] const Sample& at(std::size_t index) const noexcept { return ring_[(first_ + index) % capacity]; }
    void drop_first() noexcept { first_ = (first_ + 1) % capacity; --count_; }
    std::array<Sample, capacity> ring_{};
    std::size_t first_{}, count_{};
    std::int64_t requested_raw_{};
};

// #5355: the persistent clock line while the shared galactic speed is not 1x. It says what really runs: the requested
// factor, and the measured one when the slowest seat holds the match below it. Nominal pace is 30 ticks a second.
[[nodiscard]] inline std::string lan_speed_line(std::int64_t requested_raw, std::optional<double> ticks_per_second) {
    using presentation::godot_backend::speed_factor_text;
    constexpr std::int64_t scale = std::int64_t{1} << 24;
    if (requested_raw == scale || requested_raw <= 0) return {};
    const auto requested = speed_factor_text(requested_raw);
    if (!ticks_per_second) return "Galactic speed " + requested + " requested; measuring the real pace";
    const double requested_rate = static_cast<double>(requested_raw) * 30.0 / static_cast<double>(scale);
    if (*ticks_per_second >= requested_rate * 0.95) return "Galactic speed " + requested;
    // Tenths are enough to read; the slowest seat bounds every peer in lockstep (MP-31: no tick is skipped).
    const auto tenths = static_cast<std::int64_t>(*ticks_per_second / 3.0 + 0.5);
    const auto running = tenths < 1 ? std::string("under 0.1x") : speed_factor_text(tenths * scale / 10);
    return "Galactic speed " + requested + " requested, running at " + running + ": the slowest player's computer sets the pace";
}

// Clock messages expire on the presentation clock even when the agreed frontier is stopped.
// Persistent driver receipts remain available to release pending input hints independently.
class LanNoticeModel final {
public:
    static constexpr std::uint64_t clock_notice_ms = 5000;
    static constexpr std::uint64_t refusal_notice_ms = 10000;

    [[nodiscard]] std::string notice(const presentation::godot_backend::GalacticLanView& view,
        std::span<const LanPlayer> players, std::uint64_t now_ms) {
        auto text = lan_notice(view, players);
        if (view.phase == presentation::godot_backend::GalacticLanPhase::ended) return text;
        // A refusal is shown when it changes, for a few seconds. The value the model first sees is the match's standing
        // state (no checkpoint at start), not an event, so it is not announced.
        if (!refusal_seen_) refusal_seen_ = view.recovery_refusal;
        else if (*refusal_seen_ != view.recovery_refusal) {
            refusal_seen_ = view.recovery_refusal;
            refusal_since_ = now_ms;
        }
        const auto name = [&](sim::SeatId seat) {
            const auto at = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.seat == seat; });
            return at != players.end() && !at->player.empty() ? at->player : "Player " + std::to_string(seat.value);
        };
        const auto append = [&](const std::string& line) {
            if (!text.empty()) text += '\n';
            text += line;
        };
        std::erase_if(refusals_, [&](const auto& row) {
            return std::none_of(view.clock_refusals.begin(), view.clock_refusals.end(),
                [&](const auto& refusal) { return refusal.seat == row.first; });
        });
        for (const auto& refusal : view.clock_refusals) {
            auto [at, inserted] = refusals_.try_emplace(refusal.seat, Stamp{refusal.completed_tick, now_ms});
            if (!inserted && at->second.tick != refusal.completed_tick) at->second = {refusal.completed_tick, now_ms};
            if (refusal.completed_tick >= view.clock_change_tick && visible(at->second.since, now_ms))
                append(name(refusal.seat) + " speed change refused: " + std::to_string(net::ClockThrottle::speed_changes_per_window)
                    + " per " + std::to_string(net::ClockThrottle::window_seconds) + " session seconds");
        }
        if (!action_ || action_->tick != view.clock_change_tick || action_->seat != view.clock_actor
            || action_->text != view.clock_action)
            action_ = Action{view.clock_change_tick, now_ms, view.clock_actor, view.clock_action};
        // An agreed resume or release of a system hold clears a stale paused line immediately.
        if (view.clock_action == "paused" && !view.clock_paused) action_->cleared = true;
        if (view.clock_paused && view.clock_action == "paused" && !action_->cleared) append("Paused by " + name(view.clock_actor));
        else if (!view.clock_action.empty() && !action_->cleared && visible(action_->since, now_ms))
            append(name(view.clock_actor) + " " + view.clock_action);
        if (!view.recovery_refusal.empty() && refusal_since_ && now_ms >= *refusal_since_
            && now_ms - *refusal_since_ < refusal_notice_ms)
            append(lan_recovery_refusal_line(view.recovery_refusal));
        return text;
    }
private:
    struct Stamp { std::uint64_t tick{}, since{}; };
    struct Action { std::uint64_t tick{}, since{}; sim::SeatId seat{}; std::string text; bool cleared{}; };
    [[nodiscard]] static bool visible(std::uint64_t since, std::uint64_t now) {
        return now >= since && now - since < clock_notice_ms;
    }
    std::optional<Action> action_;
    std::optional<std::string> refusal_seen_;
    std::optional<std::uint64_t> refusal_since_;
    std::map<sim::SeatId, Stamp> refusals_;
};

} // namespace eawr::viewer::galactic
