#pragma once

#include "eawr/sim/galactic/events.hpp"
#include <cstdint>
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

// #5355: in a LAN match the shared speed budget is small (2 changes per 10 session seconds), and the agreed snapshot
// shows a request only after the input delay. One strategic speed request is in flight per seat: further presses wait
// until the published speed equals the request, this seat's speed event or a sim refusal of its SetSpeed is published,
// the adapter's budget refusal arrives (reset()), or one speed window of session ticks has passed (never stuck).
// Speeds are Q24 raw factors; ticks are agreed session ticks. Presentation only.
class SpeedToggleGuard final {
public:
    static constexpr std::uint64_t release_ticks = 300; // one ClockThrottle window

    [[nodiscard]] bool begin(std::int64_t current, std::int64_t target, std::uint64_t tick) {
        if (pending_ || current == target) return false;
        pending_ = Pending{target, tick};
        return true;
    }
    void observe(std::int64_t current, sim::SeatId seat, std::span<const sim::galactic::GalacticEvent> events, std::uint64_t tick) {
        if (!pending_) return;
        if (current == pending_->target || tick >= pending_->since + release_ticks) { pending_.reset(); return; }
        for (const auto& event : events) {
            using Kind = sim::galactic::GalacticEventKind;
            using Reason = sim::galactic::GalacticRejectReason;
            // Only events executed at or after the press answer it; an older published event belongs to an earlier request.
            if (event.producer != seat || event.tick < pending_->since) continue;
            if ((event.kind == Kind::speed_changed && event.subject == 0U)
                || (event.kind == Kind::command_rejected
                    && (event.reason == Reason::not_authorized || event.reason == Reason::inactive_seat))) {
                pending_.reset();
                return;
            }
        }
    }
    void reset() noexcept { pending_.reset(); }
    // The requested factor while a request is in flight; the button shows it until the agreed state answers.
    [[nodiscard]] std::optional<std::int64_t> target() const noexcept {
        return pending_ ? std::optional{pending_->target} : std::nullopt;
    }
private:
    struct Pending { std::int64_t target{}; std::uint64_t since{}; };
    std::optional<Pending> pending_;
};

} // namespace eawr::viewer::galactic
