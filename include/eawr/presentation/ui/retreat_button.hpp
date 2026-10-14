#pragma once

#include "eawr/sim/tactical/retreat.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace eawr::presentation::ui {

// HCB-15b: the command bar's `b_retreat` state for one perspective. Without a published
// view (standalone skirmish, HCB-15a) the button stays a disabled glyph.
struct RetreatButtonView {
    bool enabled{};   // the press reaches the retreat toggle
    bool flashing{};  // the local retreat countdown is underway
    double dial{1.0}; // opening lockout progress; 1 draws no dial
    bool operator==(const RetreatButtonView&) const = default;
};

// HCB-15b: enabled unless the opening lockout, a tutorial/mode/participant gate, a decided
// battle or our escape phase refuses it; prevention, superweapon and another player's retreat
// leave it enabled (the press is then refused). During the lockout the dial shows the elapsed
// share of the lockout.
[[nodiscard]] inline RetreatButtonView retreat_button_view(
    const std::optional<sim::tactical::RetreatView>& view, const std::uint64_t completed_tick, const bool decided) {
    using sim::tactical::RetreatPhase;
    using sim::tactical::RetreatRefusal;
    RetreatButtonView out;
    if (!view || decided) return out;
    const bool escaping = view->state.phase == RetreatPhase::escaping || view->state.phase == RetreatPhase::escaped;
    const auto refusal = view->refusal;
    out.enabled = !escaping && (refusal == RetreatRefusal::none || refusal == RetreatRefusal::other_player
        || refusal == RetreatRefusal::prevention || refusal == RetreatRefusal::superweapon);
    out.flashing = out.enabled && view->state.phase == RetreatPhase::countdown && view->state.player == view->player;
    if (refusal == RetreatRefusal::lockout && view->lockout_frames > 0) {
        const auto remaining = view->lockout_frames > completed_tick ? view->lockout_frames - completed_tick : 0;
        out.dial = 1.0 - std::clamp(static_cast<double>(remaining) / view->lockout_frames, 0.0, 1.0);
    }
    return out;
}

} // namespace eawr::presentation::ui
