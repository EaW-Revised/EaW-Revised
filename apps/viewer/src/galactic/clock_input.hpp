#pragma once

#include "eawr/sim/galactic/events.hpp"
#include <optional>
#include <span>

namespace eawr::viewer::galactic {

// Pause is nested in the session: a second toggle must wait for the first
// command's published state. Unrelated local-seat rejections cannot release it.
class PauseToggleGuard final {
public:
    [[nodiscard]] bool begin(bool paused, bool target) {
        if (pending_ || paused == target) return false;
        pending_ = target;
        return true;
    }
    void observe(bool paused, sim::SeatId seat, std::span<const sim::galactic::GalacticEvent> events) {
        if (!pending_) return;
        if (paused == *pending_) { pending_.reset(); return; }
        for (const auto& event : events) {
            using Kind = sim::galactic::GalacticEventKind;
            using Reason = sim::galactic::GalacticRejectReason;
            if (event.kind == Kind::command_rejected && event.producer == seat
                && (event.reason == Reason::inactive_seat || (!*pending_ && event.reason == Reason::not_paused))) {
                pending_.reset();
                return;
            }
        }
    }
private:
    std::optional<bool> pending_;
};

} // namespace eawr::viewer::galactic
