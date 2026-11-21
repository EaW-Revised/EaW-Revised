#pragma once

#include "eawr/sim/galactic/events.hpp"
#include <cstdint>
#include <optional>
#include <span>

namespace eawr::viewer::galactic {

// A seat's pause/resume request in flight. Pause is nested in a solo session, so a second toggle waits for the first
// command's published state; in a LAN match the commands are absolute and idempotent, so a press on what the button
// shows (the request in flight, else the agreed state) may reverse the request. The latch never outlives its answer:
// it ends when the agreed state equals the request, when this seat's request is refused (the adapter's debounce or a
// dropped order), or after one answer window of session ticks. A seat that asked for a pause can always ask for the
// resume (and the reverse) itself; it never has to wait for another seat to flip the agreed state (#7973).
// Presentation only: it never reaches the simulation.
class PauseToggleGuard final {
public:
    static constexpr std::uint64_t release_ticks = 60; // two debounce windows; a request answers in a handful of ticks

    // What the pause button shows and what its next press acts on: the request in flight, else the agreed state.
    [[nodiscard]] bool shown(bool paused) const noexcept { return pending_ ? pending_->target : paused; }
    [[nodiscard]] bool begin(bool paused, bool target, std::uint64_t tick = 0) {
        expire(tick);
        if (shown(paused) == target) return false;
        pending_ = Pending{target, tick};
        return true;
    }
    void observe(bool paused, sim::SeatId seat, std::span<const sim::galactic::GalacticEvent> events, std::uint64_t tick = 0) {
        if (!pending_) return;
        if (paused == pending_->target) { pending_.reset(); return; }
        for (const auto& event : events) {
            using Kind = sim::galactic::GalacticEventKind;
            using Reason = sim::galactic::GalacticRejectReason;
            if (event.kind == Kind::command_rejected && event.producer == seat
                && (event.reason == Reason::inactive_seat || (!pending_->target && event.reason == Reason::not_paused))) {
                pending_.reset();
                return;
            }
        }
        expire(tick);
    }
    // The adapter refused this seat's clock request (the pause debounce, the speed budget): nothing is in flight.
    void reset() noexcept { pending_.reset(); }
    [[nodiscard]] std::optional<bool> target() const noexcept {
        return pending_ ? std::optional{pending_->target} : std::nullopt;
    }
private:
    void expire(std::uint64_t tick) noexcept {
        if (pending_ && tick >= pending_->since + release_ticks) pending_.reset();
    }
    struct Pending { bool target{}; std::uint64_t since{}; };
    std::optional<Pending> pending_;
};

// #5355: in a LAN match the shared speed budget is small (2 changes per 10 session seconds), and the agreed snapshot
// shows a request only after the input delay. One strategic speed request is in flight per seat: further presses wait
// until the published speed equals the request, this seat's speed event or a sim refusal of its SetSpeed is published,
// the adapter's budget refusal arrives (reset()), or one speed window of session ticks has passed (never stuck).
// Speeds are Q24 raw factors; ticks are agreed session ticks. A request back to normal (1x) is never held behind a
// faster one in flight: it replaces it, so the seat that asked for fast forward can always end it with its own button
// (owner online test 2026-11-17). Presentation only.
class SpeedToggleGuard final {
public:
    static constexpr std::uint64_t release_ticks = 300; // one ClockThrottle window
    static constexpr std::int64_t normal = std::int64_t{1} << 24; // 1x as a Q24 raw factor

    [[nodiscard]] bool begin(std::int64_t current, std::int64_t target, std::uint64_t tick) {
        if (pending_) {
            if (target != normal || pending_->target == target) return false;
        } else if (current == target) {
            return false;
        }
        pending_ = Pending{target, tick};
        return true;
    }
    // What the button shows and what its next press acts on: the request in flight, else the current speed.
    [[nodiscard]] std::int64_t shown(std::int64_t current) const noexcept { return pending_ ? pending_->target : current; }
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
