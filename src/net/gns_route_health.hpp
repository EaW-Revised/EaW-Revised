#pragma once

#include "gns_link_stats.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace eawr::net {
// Decision logic of the in-match relay switch (docs: out/mp/turn-fallback/inmatch-switch-design.md), kept free of any transport,
// socket or lockstep code so it can be tested alone. NOT wired into anything: the switch itself is not implemented; this is the
// tests-first part (owner: "hint now, switch after tests"). It never acts; it answers "what should happen now" and counts.
//
// Healthy path (the owner's hard constraint): observe() on a pair that is not suffering is `GnsLossHint::observe` (two
// comparisons) plus one flag test; no evaluation, no probe, no allocation, nothing counted.
struct GnsSwitchLimits {
    std::array<std::chrono::seconds, 3> backoff{std::chrono::seconds(60), std::chrono::seconds(120), std::chrono::seconds(240)};
    unsigned max_attempts{3};                      // per match
    std::chrono::seconds pause_budget{10};         // a switch in flight that long is abandoned
    std::chrono::seconds probe_budget{3};          // the dry-run allocation on the other provider
};

// What the transport should do after an observation.
enum class GnsSwitchAction : std::uint8_t {
    none,
    probe_alternative, // dry-run Allocate on the other provider first (I1): never close a working-lossy pair before it answered
    switch_pair,       // the alternative answered: replace the pair (make-before-break) on the other provider
    abandon,           // a probe or switch overran its budget: stay on / return to the old route
};

struct GnsRouteHealthInput {
    bool relayed{};            // this pair is relayed (a direct pair is a different decision)
    std::optional<double> loss_pct; // receive loss, percent, from the stats tick; nullopt = unmeasured
    bool lockstep_waiting{};   // lockstep reports it is waiting for this peer (a quiet lobby or a paused game never triggers)
    bool tail_within_budget{true}; // the retained sealed tail is not near its budget (a switch must not start otherwise)
};

class GnsRouteHealth {
public:
    using Clock = std::chrono::steady_clock;
    explicit GnsRouteHealth(GnsSwitchLimits limits = {}) : limits_(limits) {}

    // Once per stats tick (about 1 s) on the I/O owner's thread.
    GnsSwitchAction observe(Clock::time_point now, const GnsRouteHealthInput& input) {
        hint_.observe(now, input.relayed, input.loss_pct);
        relayed_ = input.relayed;
        if (state_ == State::idle && !hint_.active()) return GnsSwitchAction::none; // healthy: nothing else runs
        if (state_ == State::probing) {
            if (now - started_ >= limits_.probe_budget) return give_up(now, false);
            return GnsSwitchAction::none;
        }
        if (state_ == State::switching) {
            if (now - started_ >= limits_.pause_budget) return give_up(now, true);
            return GnsSwitchAction::none;
        }
        if (!input.relayed || !hint_.active() || !input.lockstep_waiting || !input.tail_within_budget) return GnsSwitchAction::none;
        if (attempts_ >= limits_.max_attempts) return GnsSwitchAction::none;
        if (next_allowed_ && now < *next_allowed_) return GnsSwitchAction::none;
        ++evaluations_; ++attempts_; ++probes_;
        state_ = State::probing; started_ = now;
        return GnsSwitchAction::probe_alternative;
    }

    // The dry-run allocation finished. Only an answered one, in time, for a pair that is still relayed and still suffering, lets the
    // pair be replaced (I1). A late answer (past the probe budget) is a failed probe, whatever tick did or did not run; a pair that
    // went direct or recovered meanwhile is left alone: `abandon` without a failure and without a backoff.
    GnsSwitchAction alternative_result(Clock::time_point now, bool answered) {
        if (state_ != State::probing) return GnsSwitchAction::none;
        if (!answered || now - started_ >= limits_.probe_budget) return give_up(now, false);
        if (!relayed_ || !hint_.active()) { state_ = State::idle; return GnsSwitchAction::abandon; }
        state_ = State::switching; started_ = now; ++switches_;
        return GnsSwitchAction::switch_pair;
    }

    // The replacement finished (ok = the new pair is proven and the old one retired). A failed replacement answers `abandon`: the
    // transport drops the half-built pair and stays on (returns to) the old route. A success, even a late one, is kept: the new
    // pair works.
    GnsSwitchAction switch_result(Clock::time_point now, bool ok) {
        if (state_ != State::switching) return GnsSwitchAction::none;
        if (!ok) return give_up(now, true);
        state_ = State::idle; hint_.reset(); arm_backoff(now);
        return GnsSwitchAction::none;
    }

    bool in_flight() const { return state_ != State::idle; }
    bool suffering() const { return hint_.active(); }
    // The counters the healthy-run proof reads: all zero on a healthy match.
    unsigned evaluations() const { return evaluations_; }
    unsigned probes() const { return probes_; }
    unsigned switches() const { return switches_; }
    unsigned failures() const { return failures_; }
    unsigned attempts() const { return attempts_; }
    std::optional<Clock::time_point> next_allowed() const { return next_allowed_; }

private:
    enum class State : std::uint8_t { idle, probing, switching };
    GnsSwitchAction give_up(Clock::time_point now, bool was_switch) {
        state_ = State::idle; ++failures_; arm_backoff(now);
        static_cast<void>(was_switch);
        return GnsSwitchAction::abandon;
    }
    // The wait before the next attempt: the n-th attempt (1-based) waits backoff[n-1], the last entry repeating.
    void arm_backoff(Clock::time_point now) {
        const auto index = attempts_ == 0 ? 0 : std::min<std::size_t>(attempts_ - 1, limits_.backoff.size() - 1);
        next_allowed_ = now + limits_.backoff[index];
    }

    GnsSwitchLimits limits_;
    GnsLossHint hint_;
    bool relayed_{};
    State state_{State::idle};
    Clock::time_point started_{};
    std::optional<Clock::time_point> next_allowed_;
    unsigned attempts_{}, evaluations_{}, probes_{}, switches_{}, failures_{};
};
} // namespace eawr::net
