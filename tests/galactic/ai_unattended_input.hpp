#pragma once

#include "../support/result.hpp"
#include "../support/check.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/platform/live_game_session.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

namespace galactic_campaign_input {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
// Test-input policy only: retain Human perception while answering ordinary prompts.
// Read accepted pending AND executed commands so a repeated boundary or a restored
// queued answer cannot claim another sequence or enqueue the same answer twice.
inline std::optional<gc::GameCommand> unattended_human_input(cp::GameSession& game) {
    const auto hold = game.battle_summary_hold();
    const auto pending = game.pending_conflict_view();
    if (!hold && (!pending || !pending->choice_ready)) return {};
    const auto recording = eawr::testing::result_code_space::take(game.checkpoint_recording());
    const auto interactive = recording.commands.setup.interactive_seat;
    eawr::testing::require(interactive.value || !hold, "noninteractive campaign harness acquired a summary hold");
    std::optional<eawr::platform::LiveGameOrder> order;
    if (hold) order = eawr::platform::LiveGameOrder{interactive, gc::BattleSummaryAcknowledge{hold}, {}};
    else {
        eawr::platform::LiveGameFrame frame;
        frame.latest = game.galaxy().snapshot(); frame.session_tick = game.completed_tick();
        frame.pending_conflict = std::make_shared<cp::PendingConflictView>(*pending);
        for (const auto& seat : frame.latest->seats()) {
            order = eawr::platform::battle_choice_order(frame, seat.seat_id, pending->conflict.id, false);
            if (order) break;
        }
    }
    if (!order) return {}; // Forced or unavailable choices remain honest refusals.
    std::uint64_t sequence{};
    for (const auto& accepted : recording.commands.commands) {
        if (accepted.key.seat != order->seat) continue;
        sequence = std::max(sequence, accepted.key.sequence);
        if (accepted.payload == order->payload) return {};
    }
    eawr::testing::require(sequence < UINT64_MAX, "unattended human command sequence exhausted");
    gc::GameCommand command{{game.completed_tick(), order->seat, sequence + 1}, order->payload};
    const auto submitted = game.submit(command);
    eawr::testing::require(static_cast<bool>(submitted), "unattended human command submission refused");
    return command;
}
} // namespace galactic_campaign_input
