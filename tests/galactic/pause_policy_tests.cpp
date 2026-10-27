// WGM-07 deviation: the pause admission policy (docs/behaviour/walks/gc-movement-control.md). Retail refuses
// fleet management, surface deployment and fleet movement while the galaxy is paused; the project admits each
// of those three kinds during a PLAYER pause behind a typed, replayed policy in the fleet-control setup.
// These cases pin the kind x policy x paused matrix, that an admitted paused command advances no time, the
// system holds that always refuse (pending conflict, battle summary), non-human issuers, the atomic combined
// launch, the replay and setup wire form, and replay/checkpoint continuation on 1/2/4/8 workers.

#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/replay.hpp"
#include "../../src/sim/galactic/encoding.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using galactic_test::expect;
using eawr::sim::math::Fixed;
using eawr::testing::result_diagnostic::take;
using Admission = gc::PausedAdmission;
using Reason = gc::GalacticRejectReason;
Fixed f(std::int64_t value) { return Fixed::from_integer(value).value(); }
void done(eawr::core::Result<void> result) { eawr::testing::result_diagnostic::must(std::move(result)); }

// Planet 1 (own): transports 30, 31 and company 32 on the surface, own fleets 10 (company 20) and 14
// (transport 60) in orbit. Planet 2 (own): fleet 13 in orbit. Seat 1 is the one human; seat 2 is an AI seat
// for player 2, whose fleet 12 waits at planet 3.
gc::GalacticSetup fixture(const gc::PausePolicy policy) {
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{gc::SeatId{1}, 1, gc::SeatKind::human, 0, true, 1}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 1}, {3, {f(24), {}, {}}, 1}};
    s.rules.maximum_fleet_movement_distance = f(12); s.rules.fiscal_cycle_frames = 100;
    s.fleets = {{10, 1, 1, {{20, 1}}}, {12, 2, 3, {{24, 1}}}, {13, 1, 2, {{25, 1}}}, {14, 1, 1, {{60, 2}}}};
    s.fleet_control.emplace(); s.fleet_control->types = {{1, false, true, false}, {2, true, true, false}};
    s.fleet_control->pause = policy;
    s.movement.emplace(); auto& m = *s.movement;
    m.mode_modifier = f(1);
    m.types = {{1, f(1), f(100), 0, true, false, false, false}, {2, f(1), f(50), 0, true, false, false, false, true}};
    m.players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}, {3, f(1), f(1), false}};
    for (const auto& q : s.planets) m.planets.push_back({q.id, f(1), false});
    m.contents = {{30, {}}, {31, {}}, {60, {}}};
    for (const auto& p : s.players) for (const auto& q : s.planets) m.access.push_back({p.player_id, q.id, true, false});
    s.control.emplace(); auto& c = *s.control;
    c.change_seconds = f(7); c.max_ground_forces = 10;
    c.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : c.players) for (const auto& b : c.players) c.relations.push_back({a.player, b.player, a.player == b.player});
    c.types = {{1, 1, false, false}, {2, 0, false, true}};
    for (const auto& p : s.planets) {
        gc::ControlPlanet row; row.planet = p.id; row.type = 100 + p.id; row.surface_accessible = true;
        if (p.id == 1) row.landed = {{30, 2, 1}, {31, 2, 1}, {32, 1, 1}};
        c.planets.push_back(row);
    }
    s.observer.emplace(); auto& o = *s.observer;
    o.human_observers = {1}; o.solo_reference_human = 1; o.reveal_delay_seconds = f(1);
    o.types = {{1, false, false, false}, {2, false, false, false}};
    const gc::IntelligenceMask mask = gc::bit(gc::Intelligence::ground_count) | gc::bit(gc::Intelligence::ground_contents)
        | gc::bit(gc::Intelligence::ownership) | gc::bit(gc::Intelligence::fleet_presence) | gc::bit(gc::Intelligence::fleet_count);
    for (const auto& player : s.players) o.players.push_back({player.player_id, false, false, mask, 1, {}});
    for (const auto& planet : s.planets) {
        gc::ObserverPlanet row; row.planet = planet.id;
        row.allegiance = row.target_owner = row.space_owner = row.ground_owner = planet.owner;
        row.current_owner = row.current_controller = planet.owner;
        for (const auto& force : c.planets[static_cast<std::size_t>(planet.id - 1)].landed)
            row.landed_transports.push_back({force.id, force.owner, false, false});
        o.planets.push_back(row);
    }
    for (const auto& player : s.players) for (const auto& planet : s.planets)
        o.records.push_back({player.player_id, planet.id, true, 0, 0, mask, {}});
    return s;
}

constexpr gc::PausePolicy retail{};
constexpr gc::PausePolicy admit_all = gc::PausePolicy::admit_all();
// Exactly one kind admits.
gc::PausePolicy only(const int kind) {
    gc::PausePolicy p;
    if (kind == 0) p.fleet_management = Admission::admit;
    if (kind == 1) p.surface_deploy = Admission::admit;
    if (kind == 2) p.fleet_move = Admission::admit;
    return p;
}

// One command per kind. The kinds map to the policy entries; the combined launch needs two of them.
const gc::CommandPayload manage = gc::SetFleetOrbitSlot{10, 1};
const gc::CommandPayload lift = gc::LaunchFromSurface{{30}, 0, 0};
const gc::CommandPayload land = gc::LandFleet{14, 1};
const gc::CommandPayload depart = gc::MoveFleet{10, {1, 2}, {}};
const gc::CommandPayload lift_and_depart = gc::LaunchFleet{{30}, {1, 2}, {}};

std::uint64_t sequence_counter = 0;
struct Outcome { bool rejected{}; Reason reason{}; };
void pause(gc::GalacticSession& world, const eawr::sim::PartitionExecutor& executor, std::uint32_t seat = 1) {
    done(world.submit({{world.completed_tick(), gc::SeatId{seat}, ++sequence_counter}, gc::PauseGalaxy{}}));
    (void)take(world.step(executor));
}
void resume(gc::GalacticSession& world, const eawr::sim::PartitionExecutor& executor, std::uint32_t seat = 1) {
    done(world.submit({{world.completed_tick(), gc::SeatId{seat}, ++sequence_counter}, gc::ResumeGalaxy{}}));
    (void)take(world.step(executor));
}
// Issues one command at the next tick and steps. `pending` is the step's pending-conflict input, `hold` the
// battle-summary system hold derived from the conflict owner's state.
Outcome issue(gc::GalacticSession& world, const eawr::sim::PartitionExecutor& executor, const gc::CommandPayload& payload,
    const std::uint32_t seat = 1, const bool pending = false, const bool hold = false) {
    const auto sequence = ++sequence_counter;
    done(world.submit({{world.completed_tick(), gc::SeatId{seat}, sequence}, payload}));
    if (hold) world.set_system_hold(true);
    const auto tick = take(world.step(executor, pending));
    Outcome out;
    for (const auto& event : tick.snapshot->events())
        if (event.kind == gc::GalacticEventKind::command_rejected && event.sequence == sequence) { out.rejected = true; out.reason = event.reason; }
    return out;
}
bool paused_refusal(const Outcome& o) { return o.rejected && o.reason == Reason::paused; }
const gc::Fleet* fleet_by_id(const gc::GalacticSnapshot& snapshot, gc::FleetId id) {
    for (const auto& fleet : snapshot.fleets()) if (fleet.id == id) return &fleet;
    return nullptr;
}
const gc::Fleet* fleet_of(const gc::GalacticSnapshot& snapshot, eawr::sim::EntityId member) {
    for (const auto& fleet : snapshot.fleets())
        for (const auto& row : fleet.members) if (row.id == member) return &fleet;
    return nullptr;
}
bool on_surface(const gc::GalacticSnapshot& snapshot, eawr::sim::EntityId id) {
    for (const auto& planet : snapshot.control()->planets)
        for (const auto& unit : planet.landed) if (unit.id == id) return true;
    return false;
}

// Kind x policy x {paused, unpaused}: a refusal reads `paused` only when the galaxy is paused and the
// policy for that command's kind says refuse. A policy for another kind never leaks into this one.
void matrix() {
    eawr::platform::ThreadWorkerAdapter executor(1);
    struct Row { const char* name; const gc::CommandPayload* payload; int kind; };
    const Row rows[] = {{"fleet management", &manage, 0}, {"surface lift", &lift, 1}, {"surface landing", &land, 1}, {"fleet move", &depart, 2}};
    for (const auto& row : rows) {
        for (const bool paused : {false, true}) {
            for (int policy = 0; policy < 5; ++policy) {
                // 0: retail, 1..3: only kind 0..2 admits, 4: all admit.
                const auto value = policy == 0 ? retail : policy == 4 ? admit_all : only(policy - 1);
                const bool admits = policy == 4 || policy - 1 == row.kind;
                auto world = take(gc::GalacticSession::create(fixture(value)));
                if (paused) pause(world, executor);
                const auto outcome = issue(world, executor, *row.payload);
                const std::string what = std::string(row.name) + (paused ? " paused" : " unpaused") + " policy " + std::to_string(policy);
                if (paused && !admits) expect(paused_refusal(outcome), what + ": refused as paused");
                else expect(!outcome.rejected, what + ": admitted");
            }
        }
    }
    // Hand-built setups keep retail without naming the policy.
    expect(gc::FleetControlSetup{}.pause == retail, "the sim default policy is retail refusal");
}

// An admitted paused command changes state at once and advances no time; a move starts on the resume.
void paused_admission_advances_no_time() {
    eawr::platform::ThreadWorkerAdapter executor(1);
    // Reference: the same departure issued unpaused. Frames counted from the issuing step to the arrival.
    const auto arrival_frames = [&](const bool paused_first) {
        auto world = take(gc::GalacticSession::create(fixture(admit_all)));
        if (paused_first) pause(world, executor);
        const auto before = world.clock().galactic_frame;
        const auto outcome = issue(world, executor, depart);
        expect(!outcome.rejected, "the departure is admitted");
        const auto* departed = fleet_by_id(*world.snapshot(), 10);
        expect(departed && departed->motion.transit, "the departure sets the transit at admission");
        if (paused_first) {
            expect(world.clock().galactic_frame == before, "the paused admission runs no frame service");
            const auto frozen = *departed;
            for (int i = 0; i < 40; ++i) (void)take(world.step(executor));
            expect(world.clock().galactic_frame == before && *fleet_by_id(*world.snapshot(), 10) == frozen,
                "a paused galaxy does not move the departed fleet");
            resume(world, executor);
        }
        for (int guard = 0; guard < 2000; ++guard) {
            const auto* fleet = fleet_by_id(*world.snapshot(), 10);
            if (fleet && fleet->orbit == 2 && !fleet->motion.transit) return world.clock().galactic_frame - before;
            (void)take(world.step(executor));
        }
        expect(false, "the departed fleet arrives");
        return std::uint64_t{};
    };
    const auto unpaused = arrival_frames(false), paused = arrival_frames(true);
    expect(unpaused > 0 && paused == unpaused, "a fleet ordered during a pause travels as long as one ordered unpaused, starting on the resume");

    // Deployment edits apply at once.
    auto world = take(gc::GalacticSession::create(fixture(admit_all)));
    pause(world, executor);
    const auto frame = world.clock().galactic_frame;
    expect(!issue(world, executor, manage).rejected && fleet_by_id(*world.snapshot(), 10)->orbit_slot == 1, "a paused slot change applies at once");
    expect(!issue(world, executor, lift).rejected && fleet_of(*world.snapshot(), 30) && !on_surface(*world.snapshot(), 30),
        "a paused lift puts the transport in a new orbit fleet");
    expect(!issue(world, executor, land).rejected && !fleet_of(*world.snapshot(), 60) && on_surface(*world.snapshot(), 60),
        "a paused landing puts the transport on the surface");
    expect(world.clock().galactic_frame == frame && world.clock().pause_depth == 1, "paused deployment advances no frame and keeps the pause");
    // Open check: a paused landing publishes no timed side effect (no pending conflict, no invasion request).
    expect(!world.pending_native_invasion(), "a paused landing queues no invasion work");
}

// System holds refuse whatever the policy says; so does a non-human issuer and invasion from orbit.
void holds_and_issuers_always_refuse() {
    eawr::platform::ThreadWorkerAdapter executor(1);
    const gc::CommandPayload* kinds[] = {&manage, &lift, &land, &depart};
    for (const auto* payload : kinds) {
        {   // A pending conflict arrives as the step's input.
            auto world = take(gc::GalacticSession::create(fixture(admit_all)));
            pause(world, executor);
            expect(paused_refusal(issue(world, executor, *payload, 1, true)), "a pending conflict refuses despite the policy");
        }
        {   // The battle-summary hold is on the pause counter without a pending conflict (open check).
            auto world = take(gc::GalacticSession::create(fixture(admit_all)));
            pause(world, executor);
            expect(paused_refusal(issue(world, executor, *payload, 1, false, true)), "a battle-summary hold refuses despite the policy");
            expect(paused_refusal(issue(world, executor, *payload)), "a system hold persists between command batches");
            world.set_system_hold(false);
            expect(!issue(world, executor, *payload).rejected, "releasing system ownership admits a later player-pause command again");
        }
        {   // Nested: a system level plus the player's own pause still refuses.
            auto world = take(gc::GalacticSession::create(fixture(admit_all)));
            done(world.adjust_conflict_pause(true)); pause(world, executor);
            expect(world.clock().pause_depth == 2, "one system level plus one player pause");
            expect(paused_refusal(issue(world, executor, *payload, 1, true)), "nested system and player pause refuses");
        }
    }
    {   // An AI plan or script never acts during a pause (fleet 12 belongs to the AI seat's player).
        auto world = take(gc::GalacticSession::create(fixture(admit_all)));
        pause(world, executor);
        expect(paused_refusal(issue(world, executor, gc::SetFleetOrbitSlot{12, 1}, 2)), "a non-human seat is refused during a pause");
        expect(!world.pending_native_invasion(), "no invasion work either");
    }
    {   // Invasion from orbit starts a battle: refused during a pause, whatever the policy.
        auto world = take(gc::GalacticSession::create(fixture(admit_all)));
        world.enable_native_invasions(true);
        const auto unpaused = take(world.admit_native_invasion(1, 10, false));
        expect(unpaused.reason != Reason::paused, "invasion is not refused as paused when the galaxy runs");
        pause(world, executor);
        const auto refused = take(world.admit_native_invasion(1, 10, false));
        expect(!refused.admitted && refused.reason == Reason::paused, "invasion from orbit is refused during a player pause");
    }
}

// The combined launch is all-or-nothing under the policy: a refused move leg leaves the surface untouched.
void combined_launch_is_atomic() {
    eawr::platform::ThreadWorkerAdapter executor(1);
    struct Case { gc::PausePolicy policy; bool admitted; };
    gc::PausePolicy lift_only = only(1), move_only = only(2);
    for (const auto& c : {Case{retail, false}, Case{lift_only, false}, Case{move_only, false}, Case{admit_all, true}}) {
        auto world = take(gc::GalacticSession::create(fixture(c.policy)));
        pause(world, executor);
        const auto before = world.snapshot();
        const auto outcome = issue(world, executor, lift_and_depart);
        const auto after = world.snapshot();
        if (c.admitted) {
            const auto* launched = fleet_of(*after, 30);
            expect(!outcome.rejected && launched && launched->motion.transit && !on_surface(*after, 30), "both kinds admit: lifted and departed");
        } else {
            expect(paused_refusal(outcome) && on_surface(*after, 30) && !fleet_of(*after, 30)
                && after->fleets().size() == before->fleets().size() && after->fleet_control()->detached_units.empty(),
                "a refused leg leaves no half-launched fleet or detached unit");
        }
    }
}

// The setup wire form: three bytes after the gates, unknown values refused, rules version 5.
void wire_form() {
    // The pause policy introduced rules version 5; later bumps (6: explicit pathfinder selection, 6500f9254) keep it.
    expect(gc::galactic_rules_version >= 5, "the pause policy bumps the galactic rules version");
    gc::PausePolicy mixed; mixed.fleet_management = Admission::admit; mixed.fleet_move = Admission::admit;
    gc::GalacticReplay replay{fixture(mixed), 3, {}};
    auto bytes = take(gc::write_replay(replay));
    expect(take(gc::parse_replay(bytes)) == replay && take(gc::parse_replay(bytes)).setup.fleet_control->pause == mixed,
        "the policy round trips through the replay header");
    std::vector<std::uint8_t> payload;
    gc::detail::encode_fleet_control(payload, *replay.setup.fleet_control, replay.setup.fleets);
    expect(payload.size() > 5 && payload[2] == 1 && payload[3] == 0 && payload[4] == 1, "the three policy bytes follow the two gate bytes");
    const auto at = std::search(bytes.begin(), bytes.end(), payload.begin(), payload.end());
    expect(at != bytes.end(), "the fleet-control extension carries the encoded payload");
    if (at != bytes.end()) {
        for (std::size_t k = 0; k < 3; ++k) {
            auto bad = bytes; bad[static_cast<std::size_t>(at - bytes.begin()) + 2 + k] = 2;
            expect(!gc::parse_replay(bad), "a policy byte of 2 is refused on parse");
        }
    }
    auto old = bytes; old[8 + 2 + 2] = 4;
    const auto parsed = gc::parse_replay(old);
    expect(!parsed && parsed.error().message.find("rules version") != std::string::npos, "a rules version 4 replay is refused with the version diagnostic");
}

gc::GalacticReplay paused_replay(const gc::PausePolicy policy) {
    gc::GalacticReplay r{fixture(policy), 80, {}};
    std::uint64_t n = 0;
    const auto add = [&](std::uint64_t tick, const gc::CommandPayload& payload) { r.commands.push_back({{tick, gc::SeatId{1}, ++n}, payload}); };
    add(2, gc::PauseGalaxy{}); add(3, manage); add(4, lift); add(5, land); add(6, depart);
    add(9, gc::SetFleetOrbitSlot{13, 2}); add(30, gc::ResumeGalaxy{});
    return r;
}

// Replay round trip and 1/2/4/8 worker equality with paused admissions; the paused edits and departure took effect.
void replay_and_workers() {
    std::vector<std::string> reference;
    const auto replay = paused_replay(admit_all);
    expect(take(gc::parse_replay(take(gc::write_replay(replay)))) == replay, "a session with paused admissions is a canonical replay");
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto world = take(gc::GalacticSession::from_replay(replay));
        std::vector<std::string> hashes;
        for (std::uint64_t i = 0; i < replay.final_tick_count; ++i) hashes.push_back(take(world.step(executor)).state_hash.get());
        const auto snapshot = world.snapshot();
        expect(fleet_by_id(*snapshot, 13)->orbit_slot == 2 && !on_surface(*snapshot, 30) && on_surface(*snapshot, 60),
            "the paused edits stood after the replay");
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "paused admissions replay equally on 1/2/4/8 workers");
        // The same commands under retail are refused: the hash trace differs from the admitting run.
        auto retail_world = take(gc::GalacticSession::from_replay(paused_replay(retail)));
        std::string last;
        for (std::uint64_t i = 0; i < replay.final_tick_count; ++i) last = take(retail_world.step(executor)).state_hash.get();
        expect(last != hashes.back(), "retail policy refuses the same commands and ends in a different state");
    }
}

// A native journal reconstructs before its campaign owner is imported. Its input must retain the
// effective system gate even when the replay world has only the recorded player pause.
void native_hold_journal() {
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto world = take(gc::GalacticSession::create(fixture(admit_all)));
        pause(world, executor);
        world.set_system_hold(true);
        const auto refused = take(world.admit_native_fleet(1, manage));
        expect(!refused.admitted && refused.reason == Reason::paused, "native journal records a summary-only refusal");
        const auto replay = take(gc::parse_replay(take(gc::write_replay(world.record()))));
        auto restored = take(gc::GalacticSession::from_replay(replay));
        while (restored.completed_tick() < replay.final_tick_count) (void)take(restored.step(executor));
        done(restored.replay_native_barrier(executor));
        done(restored.finish_native_replay());
        expect(restored.state_sha256() == world.state_sha256() && restored.record().native_effects == replay.native_effects,
            "native refusal receipt replays without ambient campaign ownership on every worker count");
    }
}

// Save while paused after paused edits; the restored game continues exactly like the uninterrupted one.
void checkpoint_while_paused() {
    std::vector<std::string> reference;
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto setup = fixture(admit_all);
        auto game = take(eawr::campaign::GameSession::create(setup));
        const auto identities = cp::game_identities(setup);
        std::uint64_t n = 0;
        const auto send = [&](const gc::CommandPayload& payload) {
            done(game.submit({{game.completed_tick(), gc::SeatId{1}, ++n}, payload})); (void)take(game.step(executor));
        };
        send(gc::PauseGalaxy{}); send(manage); send(lift); send(depart);
        expect(game.galaxy().clock().pause_depth == 1 && game.galaxy().snapshot()->fleets().size() > 4, "paused edits applied before the save");
        const auto saved = take(cp::save_game_checkpoint(game, identities));
        galactic_test::mark_saved(game);
        auto restored = take(cp::load_game_checkpoint(saved, identities, {}, executor));
        expect(restored.session.state_sha256() == game.state_sha256() && restored.session.galaxy().clock().pause_depth == 1
            && restored.session.galaxy().setup().fleet_control->pause == admit_all, "restore equals the paused state and keeps the policy");
        std::vector<std::string> hashes;
        for (int i = 0; i < 3; ++i) {
            const auto live = take(game.step(executor)).state_hash.get();
            expect(take(restored.session.step(executor)).state_hash.get() == live, "restored paused continuation matches");
            hashes.push_back(live);
        }
        const auto resumed = [&](eawr::campaign::GameSession& session) {
            done(session.submit({{session.completed_tick(), gc::SeatId{1}, 100}, gc::ResumeGalaxy{}}));
            std::string last = take(session.step(executor)).state_hash.get();
            for (int i = 0; i < 20; ++i) last = take(session.step(executor)).state_hash.get();
            return last;
        };
        const auto live_final = resumed(game);
        expect(resumed(restored.session) == live_final, "the resumed departure continues identically after restore");
        hashes.push_back(live_final);
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "paused checkpoint continuation is equal on 1/2/4/8 workers");
    }
}
} // namespace

int main() {
    try {
        matrix();
        paused_admission_advances_no_time();
        holds_and_issuers_always_refuse();
        combined_launch_is_atomic();
        wire_form();
        replay_and_workers();
        native_hold_journal();
        checkpoint_while_paused();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}
