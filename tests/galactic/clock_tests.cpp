#include "galactic_support.hpp"

#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/replay.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

// The galactic clock (ADR-013): session tick and galactic frame, nested pause and resume
// (WGM-05/06), the speed settings as pacing-only state (WGM-U02), the fiscal window in frames with
// one cycle per service (WGM-02/03, WGEP-01/02) and the 30-frame periodic gate (WGM-04).
namespace {

using namespace galactic_test;
using eawr::platform::LiveGameSession;
using galactic::GalacticSession;
using galactic::GameCommand;
using Kind = galactic::GalacticEventKind;
namespace codes = galactic::diagnostic_codes;
namespace math = eawr::sim::math;

[[nodiscard]] GameCommand pause(const std::uint64_t tick, const galactic::SeatId seat, const std::uint64_t sequence) {
    return {{tick, seat, sequence}, galactic::PauseGalaxy{}};
}
[[nodiscard]] GameCommand resume(const std::uint64_t tick, const galactic::SeatId seat, const std::uint64_t sequence) {
    return {{tick, seat, sequence}, galactic::ResumeGalaxy{}};
}
[[nodiscard]] GameCommand speed(const std::uint64_t tick, const galactic::SeatId seat, const std::uint64_t sequence,
    const bool tactical, const std::int64_t raw) {
    return {{tick, seat, sequence}, galactic::SetSpeed{tactical, math::Fixed::from_raw(raw)}};
}

// The stock Fiscal_Cycle_Time_In_Secs (45.0) as a loaded constant table would carry it.
[[nodiscard]] galactic::GalacticRules stock_rules() {
    return {galactic::fiscal_cycle_frames(math::Fixed::from_decimal("45.0").value()).value()};
}

[[nodiscard]] std::vector<Kind> kinds(const galactic::GalacticSnapshot& snapshot) {
    std::vector<Kind> result;
    for (const auto& event : snapshot.events()) result.push_back(event.kind);
    return result;
}

void frame_conversion() {
    expect(stock_rules().fiscal_cycle_frames == 1350, "WGM-02: 45 s is 1350 frames at 30 frames a second");
    const auto frames = [](const std::int64_t raw) { return galactic::fiscal_cycle_frames(math::Fixed::from_raw(raw)); };
    expect(frames(std::int64_t{1} << 22).value() == 8, "WGEP-02: 0.25 s is 7.5 frames, a tie rounded away from zero");
    expect(frames((std::int64_t{1} << 22) - 1).value() == 7, "just below the tie rounds down");
    expect(!frames(1), "a duration below half a frame is refused");
    expect(!frames(0) && frames(0).error().code == codes::invalid_setup, "zero seconds are refused");
    expect(!frames(-(std::int64_t{1} << 24)), "negative seconds are refused");
}

void fiscal_window() {
    auto setup = galactic::synthetic_galaxy();
    setup.rules = stock_rules();
    auto session = GalacticSession::create(setup).value();
    const eawr::sim::InlineExecutor executor;
    const auto start = session.clock();
    expect(start.fiscal_start == 0 && start.fiscal_end == 1350 && start.fiscal_cycle == 0, "the first window is [0, 1350)");
    std::vector<std::uint64_t> boundaries;
    std::uint64_t periodic = 0;
    for (int index = 0; index < 2800; ++index) {
        const auto tick = session.step(executor).value();
        expect(tick.work.fiscal_cycles <= 1, "never more than one fiscal cycle in a service (no catch-up loop)");
        periodic += tick.work.periodic_checks;
        const auto& clock = tick.snapshot->clock();
        expect(clock.fiscal_end - clock.fiscal_start == 1350, "the window keeps its duration");
        for (const auto& event : tick.snapshot->events()) {
            if (event.kind == Kind::fiscal_cycle_completed) {
                boundaries.push_back(clock.galactic_frame);
                expect(event.value == clock.fiscal_cycle, "the event carries the new cycle number");
            }
        }
    }
    expect(boundaries == std::vector<std::uint64_t>{1350, 2700}, "fiscal boundaries at galactic frames 1350 and 2700");
    expect(periodic == 2800 / 30, "WGM-04: one periodic check every 30 galactic frames");
    expect(session.clock().fiscal_cycle == 2 && session.clock().fiscal_start == 2700 && session.clock().fiscal_end == 4050,
        "two cycles, the start moved to the previous end");

    // A one-frame cycle still processes exactly one cycle per service.
    setup.rules.fiscal_cycle_frames = 1;
    auto fast = GalacticSession::create(setup).value();
    for (int index = 0; index < 10; ++index) expect(fast.step(executor).value().work.fiscal_cycles == 1, "one cycle a frame");
    expect(fast.clock().fiscal_cycle == 10, "ten frames, ten cycles");
    // Without the constant the galaxy has no fiscal window.
    setup.rules = {};
    auto none = GalacticSession::create(setup).value();
    for (int index = 0; index < 2000; ++index) expect(none.step(executor).value().work.fiscal_cycles == 0, "no fiscal window");
}

void pause_nesting() {
    auto setup = galactic::synthetic_galaxy();
    setup.rules = stock_rules();
    auto session = GalacticSession::create(setup).value();
    const eawr::platform::ThreadWorkerAdapter pool(2);
    expect(static_cast<bool>(session.submit(pause(2, {1}, 1))), "pause");
    expect(static_cast<bool>(session.submit(pause(3, {2}, 1))), "nested pause by the other seat");
    expect(static_cast<bool>(session.submit(resume(5, {1}, 2))), "first resume");
    expect(static_cast<bool>(session.submit(resume(7, {2}, 2))), "second resume");
    expect(static_cast<bool>(session.submit(resume(8, {2}, 3))), "resume with nothing paused");
    std::vector<std::uint64_t> frames;
    for (std::uint64_t index = 0; index < 10; ++index) {
        const PhaseProbe probe(pool);
        const auto tick = session.step(probe).value();
        const auto& clock = tick.snapshot->clock();
        frames.push_back(clock.galactic_frame);
        expect(clock.session_tick == index + 1, "the session tick advances on every step");
        expect(tick.work.serviced == !probe.phases.empty(), "a paused galaxy runs no phase");
        const auto events = kinds(*tick.snapshot);
        if (index == 2) expect(events == std::vector<Kind>{Kind::command_executed, Kind::galaxy_paused}, "WGM-05: first pause");
        if (index == 3) expect(events == std::vector<Kind>{Kind::command_executed}, "WGM-05: a nested pause repeats nothing");
        if (index == 3) expect(clock.pause_depth == 2, "depth two");
        if (index == 5) expect(events == std::vector<Kind>{Kind::command_executed} && clock.pause_depth == 1, "still paused");
        if (index == 7) expect(events == std::vector<Kind>{Kind::command_executed, Kind::galaxy_resumed}, "WGM-06: resume at zero");
        if (index == 8) {
            expect(events.size() == 1 && events[0] == Kind::command_rejected
                    && tick.snapshot->events()[0].reason == galactic::GalacticRejectReason::not_paused,
                "WGM-06: a resume needs a positive counter");
        }
    }
    // Ticks 0, 1 serviced; 2..6 paused (the pause executes before the service); 7.. serviced again.
    expect(frames == std::vector<std::uint64_t>{1, 2, 2, 2, 2, 2, 2, 3, 4, 5}, "the paused galaxy's frame does not advance");
    // The fiscal boundary moves by the paused ticks: frame 1350 is session tick 1355.
    std::uint64_t boundary_tick = 0;
    while (boundary_tick == 0) {
        const auto tick = session.step(pool).value();
        for (const auto& event : tick.snapshot->events()) {
            if (event.kind == Kind::fiscal_cycle_completed) boundary_tick = tick.completed_tick;
        }
    }
    expect(boundary_tick == 1355, "the fiscal window counts galactic frames, not session ticks");
}

void seat_authorization() {
    galactic::SyntheticGalaxyOptions options;
    options.coop_seat = true;
    auto setup = galactic::synthetic_galaxy(options);
    setup.seats[1].kind = galactic::SeatKind::ai;
    setup.seats[2].active = false;
    auto session = GalacticSession::create(setup).value();
    const eawr::sim::InlineExecutor executor;
    expect(static_cast<bool>(session.submit(pause(0, {3}, 1))), "an inactive seat's pause is admitted for execution");
    expect(static_cast<bool>(session.submit(speed(0, {3}, 2, false, 2 << 24))), "and its speed");
    expect(has_code(session.submit(pause(0, {9}, 1)), codes::unknown_issuer), "a pause from no seat is refused");
    auto tick = session.step(executor).value();
    expect(tick.snapshot->events().size() == 2 && tick.snapshot->events()[0].reason == galactic::GalacticRejectReason::inactive_seat
            && tick.snapshot->events()[1].reason == galactic::GalacticRejectReason::inactive_seat,
        "an inactive seat may not pause or change the speed");
    expect(tick.work.serviced && session.clock().strategic_speed.raw() == (1 << 24), "nothing changed");
    // Any active seat may pause, an AI seat included, until the multiplayer vote rules land.
    expect(static_cast<bool>(session.submit(pause(1, {2}, 1))), "the AI seat pauses");
    tick = session.step(executor).value();
    expect(!tick.work.serviced && session.clock().pause_depth == 1, "the AI seat's pause holds");
    expect(static_cast<bool>(session.submit(resume(2, {1}, 1))), "the human seat resumes");
    tick = session.step(executor).value();
    expect(tick.work.serviced && session.clock().pause_depth == 0, "any active seat may resume");
}

void speed_is_pacing_only() {
    galactic::SyntheticGalaxyOptions options;
    options.coop_seat = true;
    auto setup = galactic::synthetic_galaxy(options);
    setup.rules = stock_rules();
    const eawr::sim::InlineExecutor executor;
    auto plain = GalacticSession::create(setup).value();
    auto paced = GalacticSession::create(setup).value();
    for (const auto& command : script(setup, 60)) {
        expect(static_cast<bool>(plain.submit(command)) && static_cast<bool>(paced.submit(command)), "scripted command");
    }
    // The coop seat (3) only changes the speeds in the paced run.
    const auto coop = setup.seats[2].seat_id;
    expect(static_cast<bool>(paced.submit(speed(5, coop, 100, false, 3 << 24))), "strategic speed 3");
    expect(static_cast<bool>(paced.submit(speed(9, coop, 101, true, 1 << 23))), "tactical speed 0.5");
    expect(static_cast<bool>(paced.submit(speed(9, coop, 102, false, 3 << 24))), "the same speed again");
    expect(has_code(paced.submit(speed(10, coop, 103, false, 0)), codes::invalid_command), "speed 0 is refused");
    expect(has_code(paced.submit(speed(10, coop, 104, false, (std::int64_t{16} << 24) + 1)), codes::invalid_command),
        "speed over 16 is refused");
    for (int index = 0; index < 60; ++index) {
        const auto left = plain.step(executor).value();
        const auto right = paced.step(executor).value();
        expect(left.state_sha256 == right.state_sha256, "tick " + std::to_string(left.completed_tick) + ": a speed change changes no hash");
        if (index == 9) {
            const auto changes = std::count_if(right.snapshot->events().begin(), right.snapshot->events().end(),
                [](const galactic::GalacticEvent& event) { return event.kind == Kind::speed_changed; });
            expect(changes == 1, "an unchanged speed raises no speed event");
        }
    }
    expect(paced.clock().strategic_speed.raw() == (3 << 24) && paced.clock().tactical_speed.raw() == (1 << 23),
        "the speeds are session state, published in the snapshot clock");
    expect(paced.snapshot()->clock().strategic_speed == paced.clock().strategic_speed, "snapshot clock = session clock");
}

// TM-03/04: the fast-forward factor and back is pacing only; with several active humans the sim refuses it.
void fast_forward() {
    const eawr::sim::InlineExecutor executor;
    const auto raw = galactic::fast_forward_factor_raw;
    // Pacing: 120 ticks a second at 4x against the nominal 30.
    expect(raw == (std::int64_t{4} << 24), "fast forward is 4x");
    expect(LiveGameSession::tick_interval(math::Fixed::from_raw(raw)) == std::chrono::nanoseconds(1'000'000'000 / 120),
        "4x paces 120 ticks a second");
    expect(LiveGameSession::tick_interval(math::Fixed::from_raw(1 << 24)) == std::chrono::nanoseconds(1'000'000'000 / 30),
        "1x paces 30 ticks a second");
    {
        galactic::SyntheticGalaxyOptions options;
        options.coop_seat = true;
        auto setup = galactic::synthetic_galaxy(options);
        setup.rules = stock_rules();
        setup.seats[1].kind = galactic::SeatKind::ai; // one active human seat: fast forward is allowed (TM-04)
        setup.seats[2].active = false;
        auto plain = GalacticSession::create(setup).value();
        auto paced = GalacticSession::create(setup).value();
        const auto human = setup.seats[0].seat_id;
        for (const auto& command : script(setup, 60)) {
            expect(static_cast<bool>(plain.submit(command)) && static_cast<bool>(paced.submit(command)), "scripted command");
        }
        for (int index = 0; index < 60; ++index) {
            expect(plain.step(executor).value().state_sha256 == paced.step(executor).value().state_sha256, "scripted ticks match");
        }
        expect(static_cast<bool>(paced.submit(speed(60, human, 105, false, raw))), "fast forward");
        expect(static_cast<bool>(paced.submit(speed(90, human, 106, false, 1 << 24))), "fast forward off");
        for (int index = 60; index < 120; ++index) {
            const auto left = plain.step(executor).value();
            const auto right = paced.step(executor).value();
            expect(left.state_sha256 == right.state_sha256, "tick " + std::to_string(left.completed_tick) + ": fast forward changes no hash");
            if (index == 70) expect(paced.clock().strategic_speed.raw() == raw, "fast forward is on");
        }
        expect(paced.clock().strategic_speed.raw() == (1 << 24), "toggled back to 1x");
    }
    {
        // TM-05: fast forward then pause with no snapshot in between (the HUD submits 1x first), then resume at 1x.
        galactic::SyntheticGalaxyOptions options;
        options.coop_seat = true;
        auto setup = galactic::synthetic_galaxy(options);
        setup.rules = stock_rules();
        setup.seats[1].kind = galactic::SeatKind::ai;
        setup.seats[2].active = false;
        auto session = GalacticSession::create(setup).value();
        const auto human = setup.seats[0].seat_id;
        expect(static_cast<bool>(session.submit(speed(0, human, 1, false, raw))), "fast forward");
        expect(static_cast<bool>(session.submit(speed(0, human, 2, false, 1 << 24))), "pause resets to 1x");
        expect(static_cast<bool>(session.submit(pause(0, human, 3))), "pause");
        (void)session.step(executor).value();
        expect(session.clock().pause_depth == 1 && session.clock().strategic_speed.raw() == (1 << 24), "paused at 1x");
        expect(static_cast<bool>(session.submit(resume(1, human, 4))), "resume");
        (void)session.step(executor).value();
        expect(session.clock().pause_depth == 0 && session.clock().strategic_speed.raw() == (1 << 24), "play resumes at 1x");
    }
    {
        // Two active humans: the sim refuses the fast-forward factor itself, not only the button.
        galactic::SyntheticGalaxyOptions options;
        options.coop_seat = true;
        auto setup = galactic::synthetic_galaxy(options);
        setup.rules = stock_rules();
        auto session = GalacticSession::create(setup).value();
        const auto coop = setup.seats[2].seat_id;
        expect(static_cast<bool>(session.submit(speed(0, coop, 1, false, raw))), "the command is admitted to the queue");
        const auto tick = session.step(executor).value();
        const auto rejected = std::count_if(tick.snapshot->events().begin(), tick.snapshot->events().end(),
            [](const galactic::GalacticEvent& event) { return event.kind == Kind::command_rejected
                && event.reason == galactic::GalacticRejectReason::not_authorized; });
        expect(rejected == 1 && session.clock().strategic_speed.raw() == (1 << 24), "two humans: fast forward is refused");
    }
}

void live_equals_replay() {
    galactic::SyntheticGalaxyOptions options;
    options.coop_seat = true;
    auto setup = galactic::synthetic_galaxy(options);
    setup.rules = {7}; // a short fiscal window, so the run crosses several boundaries
    auto commands = script(setup, 80);
    const auto coop = setup.seats[2].seat_id;
    for (const auto& extra : {pause(10, coop, 1), pause(12, coop, 2), resume(20, coop, 3), resume(25, coop, 4),
             speed(30, coop, 5, false, 2 << 24), resume(31, coop, 6), pause(50, coop, 7), resume(53, coop, 8)}) {
        commands.push_back(extra);
    }
    std::sort(commands.begin(), commands.end(), [](const GameCommand& left, const GameCommand& right) { return left.key < right.key; });
    const eawr::sim::InlineExecutor executor;
    auto live = GalacticSession::create(setup).value();
    std::vector<std::string> hashes;
    std::size_t next = 0;
    for (std::uint64_t tick = 0; tick < 80; ++tick) {
        while (next < commands.size() && commands[next].key.tick == tick) expect(static_cast<bool>(live.submit(commands[next++])), "live submit");
        hashes.push_back(live.step(executor).value().state_sha256);
    }
    const auto bytes = galactic::write_replay(live.record());
    expect(static_cast<bool>(bytes), "the clock run records");
    const auto parsed = galactic::parse_replay(bytes.value());
    expect(parsed && parsed.value() == live.record() && parsed.value().setup.rules.fiscal_cycle_frames == 7,
        "the replay carries the rules");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        auto replayed = GalacticSession::from_replay(parsed.value()).value();
        std::vector<std::string> again;
        for (std::uint64_t tick = 0; tick < 80; ++tick) {
            replayed.scramble_storage_for_testing();
            again.push_back(replayed.step(pool).value().state_hash.get());
        }
        expect(again == hashes, std::to_string(workers) + " workers, scrambled: the clock replays itself");
        expect(replayed.clock() == live.clock(), "and ends on the same clock");
    }
}

} // namespace

int case_galactic_clock_tests() {
    frame_conversion();
    fiscal_window();
    pause_nesting();
    seat_authorization();
    speed_is_pacing_only();
    fast_forward();
    live_equals_replay();
    if (failures != 0) {
        std::cerr << failures << " galactic clock check(s) failed\n";
        return 1;
    }
    std::cout << "galactic clock contracts passed\n";
    return 0;
}
