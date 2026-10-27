#pragma once

#include "galactic_lan_match.hpp"

#include <algorithm>
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


// Clock messages expire on the presentation clock even when the agreed frontier is stopped.
// Persistent driver receipts remain available to release pending input hints independently.
class LanNoticeModel final {
public:
    static constexpr std::uint64_t clock_notice_ms = 5000;

    [[nodiscard]] std::string notice(const presentation::godot_backend::GalacticLanView& view,
        std::span<const LanPlayer> players, std::uint64_t now_ms) {
        auto text = lan_notice(view, players);
        if (view.phase == presentation::godot_backend::GalacticLanPhase::ended) return text;
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
                append(name(refusal.seat) + " speed change refused: 6 per 45 session seconds");
        }
        if (!action_ || action_->tick != view.clock_change_tick || action_->seat != view.clock_actor
            || action_->text != view.clock_action)
            action_ = Action{view.clock_change_tick, now_ms, view.clock_actor, view.clock_action};
        // An agreed resume or release of a system hold clears a stale paused line immediately.
        if (view.clock_action == "paused" && !view.clock_paused) action_->cleared = true;
        if (view.clock_paused && view.clock_action == "paused" && !action_->cleared) append("Paused by " + name(view.clock_actor));
        else if (!view.clock_action.empty() && !action_->cleared && visible(action_->since, now_ms))
            append(name(view.clock_actor) + " " + view.clock_action);
        return text;
    }
private:
    struct Stamp { std::uint64_t tick{}, since{}; };
    struct Action { std::uint64_t tick{}, since{}; sim::SeatId seat{}; std::string text; bool cleared{}; };
    [[nodiscard]] static bool visible(std::uint64_t since, std::uint64_t now) {
        return now >= since && now - since < clock_notice_ms;
    }
    std::optional<Action> action_;
    std::map<sim::SeatId, Stamp> refusals_;
};

} // namespace eawr::viewer::galactic
