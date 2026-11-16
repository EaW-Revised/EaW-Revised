#include "../support/check.hpp"
#include "../../src/net/gns_route_health.hpp"

#include <chrono>
#include <iostream>

// The decision logic of the in-match relay switch, without any transport (docs: out/mp/turn-fallback/inmatch-switch-design.md).
// Invariants checked here: I1 never replace a pair before the other provider answered a dry-run allocation; I2 one attempt in
// flight; I5 a failed attempt backs off (60/120/240 s) and the match allows three at most. Plus the owner's hard constraint:
// a healthy match runs nothing extra (every counter stays zero).
namespace {
using namespace std::chrono_literals;
using namespace eawr::net;
using Clock = GnsRouteHealth::Clock;
void expect(bool value, const char* message) { eawr::testing::require(value, message); }

const Clock::time_point t0 = Clock::now();
Clock::time_point at(int seconds) { return t0 + std::chrono::seconds(seconds); }
GnsRouteHealthInput bad(bool waiting = true) { return {true, 25.0, waiting, true}; }

// Drives `health` once a second from `from` to `to` (inclusive) with `input`; returns the first non-none action and its time.
struct Step { GnsSwitchAction action{GnsSwitchAction::none}; int when{-1}; };
Step drive(GnsRouteHealth& health, int from, int to, const GnsRouteHealthInput& input) {
    for (int s = from; s <= to; ++s) {
        const auto action = health.observe(at(s), input);
        if (action != GnsSwitchAction::none) return {action, s};
    }
    return {};
}

void healthy_runs_nothing() {
    GnsRouteHealth health;
    for (int s = 0; s < 3600; ++s) {
        // An hour of healthy play, lockstep waiting now and then (normal jitter): nothing extra runs.
        const GnsRouteHealthInput input{true, 0.5, s % 7 == 0, true};
        expect(health.observe(at(s), input) == GnsSwitchAction::none, "A healthy pair never gets an action");
    }
    expect(health.evaluations() == 0 && health.probes() == 0 && health.switches() == 0 && health.failures() == 0 && !health.in_flight(),
        "Healthy run: evaluations=0 probes=0 switches=0");
    // Loss alone, or waiting alone, or a direct pair, or an unmeasured pair never triggers either.
    GnsRouteHealth lossy_not_waiting;
    expect(drive(lossy_not_waiting, 0, 600, bad(false)).action == GnsSwitchAction::none, "High loss while lockstep is not waiting: no trigger");
    GnsRouteHealth direct;
    expect(drive(direct, 0, 600, {false, 40.0, true, true}).action == GnsSwitchAction::none, "A direct pair is not this decision");
    GnsRouteHealth unmeasured;
    expect(drive(unmeasured, 0, 600, {true, std::nullopt, true, true}).action == GnsSwitchAction::none, "Unmeasured loss: no trigger");
    expect(lossy_not_waiting.evaluations() == 0 && direct.evaluations() == 0 && unmeasured.evaluations() == 0, "No evaluation without suffering");
}

void trigger_needs_loss_for_20_s_and_waiting() {
    GnsRouteHealth health;
    const auto step = drive(health, 0, 60, bad());
    expect(step.action == GnsSwitchAction::probe_alternative && step.when == 20, "20 s of >= 15% loss while lockstep waits starts with the dry run");
    expect(health.evaluations() == 1 && health.probes() == 1 && health.switches() == 0 && health.in_flight(), "One evaluation, one probe, no switch yet (I1)");
    // Loss that dips every 10 s never gets 20 s in a row.
    GnsRouteHealth flap;
    for (int s = 0; s < 300; ++s)
        expect(flap.observe(at(s), {true, s % 10 == 9 ? 2.0 : 40.0, true, true}) == GnsSwitchAction::none, "Flapping loss never triggers");
    // Lockstep not waiting at first, loss already sustained: it triggers the moment lockstep waits.
    GnsRouteHealth late;
    expect(drive(late, 0, 100, bad(false)).action == GnsSwitchAction::none, "Not waiting: nothing");
    expect(late.observe(at(101), bad(true)) == GnsSwitchAction::probe_alternative, "Waiting now: the sustained loss triggers at once");
    // The retained tail near its budget blocks the start.
    GnsRouteHealth tail;
    expect(drive(tail, 0, 100, {true, 30.0, true, false}).action == GnsSwitchAction::none, "A tail near its budget never starts a switch");
}

void dry_run_first_and_one_in_flight() {
    GnsRouteHealth health;
    expect(drive(health, 0, 30, bad()).when == 20, "Triggered at 20 s");
    // I2: while the probe is out nothing else starts, whatever the stats say.
    for (int s = 21; s < 23; ++s) expect(health.observe(at(s), bad()) == GnsSwitchAction::none, "One attempt in flight");
    // I1: a dry run that did not answer never leads to a switch; the pair is left alone.
    expect(health.alternative_result(at(23), false) == GnsSwitchAction::abandon && health.switches() == 0 && health.failures() == 1,
        "A dead alternative: abandon, no switch (I1)");
    expect(!health.in_flight(), "Nothing in flight afterwards");
    // Results out of turn are ignored.
    expect(health.alternative_result(at(24), true) == GnsSwitchAction::none, "An alternative result without a probe is ignored");
    health.switch_result(at(24), true);
    expect(health.switches() == 0 && !health.in_flight(), "A switch result without a switch is ignored");
}

// Review findings: a pair that went direct or recovered during the dry run is not replaced; a late dry-run answer is a failed probe;
// a failed replacement tells the transport to go back to the old route.
void guards_at_the_decision_points() {
    GnsRouteHealth direct;
    drive(direct, 0, 30, bad());
    direct.observe(at(21), {false, 25.0, true, true}); // the pair went direct while the probe was out
    expect(direct.alternative_result(at(22), true) == GnsSwitchAction::abandon && direct.switches() == 0 && !direct.in_flight() && direct.failures() == 0,
        "A pair that went direct is not replaced (no failure, no backoff)");
    GnsRouteHealth late;
    drive(late, 0, 30, bad());
    expect(late.alternative_result(at(24), true) == GnsSwitchAction::abandon && late.switches() == 0 && late.failures() == 1,
        "A dry-run answer after the 3 s probe budget is a failed probe even if no tick ran");
    GnsRouteHealth failed;
    drive(failed, 0, 30, bad());
    failed.alternative_result(at(21), true);
    expect(failed.switch_result(at(24), false) == GnsSwitchAction::abandon && failed.failures() == 1 && !failed.in_flight(),
        "A failed replacement answers abandon: back to the old route");
    GnsRouteHealth succeeded;
    drive(succeeded, 0, 30, bad());
    succeeded.alternative_result(at(21), true);
    expect(succeeded.switch_result(at(40), true) == GnsSwitchAction::none && succeeded.failures() == 0 && !succeeded.in_flight(),
        "A success is kept even when it arrives after the pause budget");
}

void success_path() {
    GnsRouteHealth health;
    drive(health, 0, 30, bad());
    expect(health.alternative_result(at(21), true) == GnsSwitchAction::switch_pair && health.switches() == 1 && health.in_flight(),
        "An answered dry run lets the pair be replaced");
    for (int s = 22; s < 26; ++s) expect(health.observe(at(s), bad()) == GnsSwitchAction::none, "Nothing else while the switch is in flight");
    health.switch_result(at(26), true);
    expect(!health.in_flight() && !health.suffering(), "A successful switch resets the loss observation");
    expect(health.next_allowed() && *health.next_allowed() == at(26) + 60s, "The first attempt backs off 60 s even after a success");
    // The new route is bad again right away: nothing before the backoff ends.
    expect(drive(health, 27, 85, bad()).action == GnsSwitchAction::none, "No second attempt inside the 60 s backoff");
    expect(drive(health, 86, 120, bad()).action == GnsSwitchAction::probe_alternative, "At the end of the backoff the next attempt starts (the loss never cleared)");
}

void backoff_schedule_and_cap() {
    GnsRouteHealth health;
    int now = 0;
    const int expected_wait[3] = {60, 120, 240};
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto start = drive(health, now, now + 400, bad());
        expect(start.action == GnsSwitchAction::probe_alternative, "Attempt starts");
        now = start.when;
        health.alternative_result(at(now), false); // the alternative never answers
        expect(health.next_allowed() && *health.next_allowed() == at(now) + std::chrono::seconds(expected_wait[attempt]),
            "Backoff is 60, then 120, then 240 s");
        ++now;
    }
    expect(health.attempts() == 3 && health.failures() == 3 && health.switches() == 0, "Three failed attempts, no switch");
    expect(drive(health, now, now + 3600, bad()).action == GnsSwitchAction::none, "A fourth attempt never starts in the same match");
}

void pause_budget() {
    GnsRouteHealth health;
    drive(health, 0, 30, bad());
    health.alternative_result(at(21), true);
    expect(drive(health, 22, 30, bad()).action == GnsSwitchAction::none, "Within the 10 s pause budget");
    const auto overrun = drive(health, 31, 40, bad());
    expect(overrun.action == GnsSwitchAction::abandon && overrun.when == 31 && !health.in_flight() && health.failures() == 1,
        "A switch that overruns its 10 s pause budget is abandoned (back to the old route)");
    // A dry run that never reports is abandoned after its own budget too.
    GnsRouteHealth slow;
    drive(slow, 0, 30, bad());
    expect(drive(slow, 21, 30, bad()).action == GnsSwitchAction::abandon && slow.failures() == 1, "A dry run that never answers is abandoned");
}

void hysteresis() {
    GnsRouteHealth health;
    drive(health, 0, 30, bad());
    health.alternative_result(at(21), false); // backoff keeps it from retrying; the hint stays raised
    // Loss between 5% and 15% does not clear the suffering state; below 5% for 10 s does.
    for (int s = 22; s < 40; ++s) health.observe(at(s), {true, 9.0, true, true});
    expect(health.suffering(), "Between 5% and 15% it stays raised");
    for (int s = 40; s < 50; ++s) health.observe(at(s), {true, 1.0, true, true});
    expect(health.suffering(), "Calm for under 10 s keeps it raised");
    health.observe(at(50), {true, 1.0, true, true});
    expect(!health.suffering(), "10 s below 5% clears it");
}
}

int main() {
    healthy_runs_nothing();
    trigger_needs_loss_for_20_s_and_waiting();
    dry_run_first_and_one_in_flight();
    guards_at_the_decision_points();
    success_path();
    backoff_schedule_and_cap();
    pause_budget();
    hysteresis();
    std::cout << "GNS route health contracts passed\n";
    return 0;
}
