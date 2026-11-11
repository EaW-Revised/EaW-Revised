#include "../support/result.hpp"
#include "../support/reverse_partition_executor.hpp"
#include "galactic_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/replay.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_economic_replay.hpp"
#include "galactic_ai_intent_commit.hpp"
#include "galactic_ai_intent_refusal.hpp"
#include "galactic_ai_outbox.hpp"
#include "galactic_ai_outbox_state.hpp"

#include <atomic>
#include <functional>
#include <semaphore>
#include <thread>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Fixtures below are the native control/force fixtures of control_tests.cpp, kept in step by hand.
namespace {
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using eawr::sim::math::Fixed;
Fixed f(std::int64_t value) { return Fixed::from_integer(value).value(); }
gc::GalacticSetup setup() {
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{gc::SeatId{1}, 1, gc::SeatKind::human, 0, true, 1}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 3}, {3, {f(24), {}, {}}, 3}};
    s.routes = {{4, 1, 2, f(2)}}; s.rules.maximum_fleet_movement_distance = f(12); s.rules.fiscal_cycle_frames = 7;
    s.fleets = {{10, 1, 1, {{20, 1}, {21, 2}}}, {11, 2, 3, {{22, 1}}}};
    s.fleet_control.emplace(); s.fleet_control->types = {{1, false, true, false}, {2, true, true, false}, {3, false, true, false}};
    s.movement.emplace(); auto& m = *s.movement; m.mode_modifier = f(1);
    m.types = {{1, f(2), f(100), 0, true, false, false, false}, {2, f(2), f(50), 0, true, false, false, false}, {3, f(2), f(50), 0, true, false, false, true}};
    m.players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}, {3, f(1), f(1), false}};
    m.planets = {{1, f(1), false}, {2, f(1), false}, {3, f(1), false}};
    for (const auto& player : s.players) for (const auto& p : s.planets) m.access.push_back({player.player_id, p.id, true, false});
    s.control.emplace(); auto& c = *s.control;
    c.change_seconds = f(7); c.max_ground_forces = 10;
    c.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : c.players) for (const auto& b : c.players) c.relations.push_back({a.player, b.player, a.player == b.player});
    c.types = {{1, 1, false, false}, {2, 0, false, true}, {3, 1, true, false}};
    for (const auto& p : s.planets) {
        gc::ControlPlanet row; row.planet = p.id; row.type = 100 + p.id;
        row.surface_accessible = true; row.capture_reward = f(250); c.planets.push_back(row);
    }
    c.planets[0].ground_base = {1, 2}; c.routes = {{4, f(1)}};
    return s;
}
void enable_production(gc::GalacticSetup& s) {
    s.production.emplace(); s.production->maximum_human_queue = 5;
    for (const auto& player : s.players) {
        gc::ProductionPlayer row; row.player = player.player_id;
        row.human = player.player_id == 1; row.pay_as_you_go = !row.human;
        row.uses_population_cap = row.human; row.population_capacity = f(100);
        s.production->players.push_back(row);
    }
    for (std::size_t i = 0; i < s.planets.size(); ++i) {
        gc::PlanetProduction row; row.planet = s.planets[i].id;
        row.target_owner = s.planets[i].owner; row.enabled = true; row.surface_accessible = true;
        row.maximum_space_level = 5; row.ground_slots = 3; row.space_slots = 3;
        row.maximum_ground_forces = s.control->max_ground_forces;
        row.ground_occupier = s.control->planets[i].ground_base.owner;
        row.ground_level = s.control->planets[i].ground_base.level;
        row.starbase_owner = s.control->planets[i].starbase.owner;
        row.space_level = s.control->planets[i].starbase.level;
        s.production->planets.push_back(row);
    }
}
gc::GalacticSetup native_control_fixture() {
    auto config = setup(); config.planets[1].owner = 1;
    config.movement->types[1].transport = true;
    config.fleets[0].orbit = 2; config.fleets[0].members = {{20, 1}, {21, 2}, {31, 2}};
    config.fleets[1].members = {{22, 2}}; config.control->planets[1].ground_base = {1, 1};
    config.player_economy.emplace(); config.fiscal.emplace(); config.fiscal->credit_cap_per_planet = f(1000);
    config.census.emplace(); config.census->types = {{1, f(1), 0}, {2, {}, 0}, {3, f(1), 0}};
    for (const auto& player : config.players) {
        config.player_economy->players.push_back({player.player_id, f(100), f(1000), 0, 5});
        config.fiscal->players.push_back({player.player_id, true, f(1)});
        config.census->players.push_back({player.player_id, 100, true, {}});
    }
    for (const auto& planet : config.planets) config.fiscal->planets.push_back({planet.id, f(1)});
    enable_production(config);
    config.control->planets[1].landed = {{50, 2, 2}, {51, 2, 1}, {52, 2, 3}};
    for (const auto& force : config.control->planets[1].landed) {
        gc::CensusObject actor; actor.id = force.id; actor.original_type = actor.current_type = force.type;
        actor.owner = force.owner; actor.planet = actor.container = 2; actor.combat_role = gc::ActorCombatRole::transport;
        config.census->objects.push_back(actor); config.movement->contents.push_back({actor.id, {}});
    }
    for (const auto& fleet : config.fleets) for (const auto& member : fleet.members) {
        gc::CensusObject actor; actor.id = member.id; actor.original_type = actor.current_type = member.type;
        actor.owner = fleet.owner; actor.planet = fleet.orbit; actor.container = fleet.id;
        actor.combat_role = member.type == 2 ? gc::ActorCombatRole::transport : gc::ActorCombatRole::ordinary;
        config.census->objects.push_back(actor);
        if (member.type == 2) config.movement->contents.push_back({member.id, {}});
    }
    std::sort(config.census->objects.begin(), config.census->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(config.movement->contents.begin(), config.movement->contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
    gc::ObserverSetup observer; observer.human_observers = {1}; observer.solo_reference_human = 1; observer.reveal_delay_seconds = f(1); observer.automatic_reveal_range = f(1);
    observer.players = {{1, false, false, 0, 0, {}}, {2, false, false, 0, 0, {}}, {3, false, false, 0, 0, {}}};
    for (const auto& planet : config.planets) {
        observer.planets.push_back({planet.id, planet.owner, planet.owner, 0, 0, 0, 0, false, false, false, false, {}});
        observer.planets.back().current_owner = observer.planets.back().current_controller = planet.owner;
        for (const auto& surface : config.control->planets) if (surface.planet == planet.id)
            for (const auto& force : surface.landed) observer.planets.back().landed_transports.push_back({force.id, force.owner, false, false});
    }
    observer.types = {{1, false, false, false}, {2, false, false, false}, {3, false, false, false}};
    for (const auto& player : config.players) for (const auto& planet : config.planets)
        observer.records.push_back({player.player_id, planet.id, false, 0, 0, 0, {}});
    config.observer = std::move(observer);
    return config;
}
gc::GalacticSetup native_force_fixture() {
    auto config = native_control_fixture();
    config.production->nested_creation.emplace(); auto& nested = *config.production->nested_creation;
    nested.fleet_type = 1;
    for (const auto id : {1U, 2U, 3U}) {
        gc::NestedCreationType type; type.type = id; type.combat_role = gc::ActorCombatRole::ordinary;
        nested.types.push_back(type);
    }
    for (const auto& relation : config.control->relations) nested.relations.push_back({relation.player, relation.other, !relation.allied});
    return config;
}

// ---------------------------------------------------------------------------------------------
// GFS-A22 / S9b-2: the ordered AI-intent commit. These cases drive commit_ordered_intents with
// accepted and refused intents of every kind and check that the outcomes, the world and the
// journal are identical at 1/2/4/8 workers, when the intents arrive in reverse registration and
// submission order, and under a partition order that runs the highest partition first.
// ---------------------------------------------------------------------------------------------
namespace ga = eawr::script::foc::galactic_ai;
using Shadow = gc::GalacticSession::NativeActionShadow;
using Placement = gc::GalacticSession::NativeForcePlacement;
using OptionalPlacement = eawr::core::Result<std::optional<Placement>>;
using eawr::testing::take;

// Movement fixture and path inputs of ai_movement_tests.cpp, kept in step by hand.
gc::GalacticSetup movement_fixture() {
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{gc::SeatId{1}, 1, gc::SeatKind::ai, 0, true, 0}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 1}, {3, {f(24), {}, {}}, 1}};
    s.rules.maximum_fleet_movement_distance = f(12); s.rules.fiscal_cycle_frames = 100;
    s.fleets = {{10, 1, 1, {{20, 1}, {21, 2}, {22, 2}}}, {11, 1, 1, {{23, 1}}}};
    s.fleet_control.emplace(); s.fleet_control->types = {{1, false, true, false}, {2, true, true, false}};
    s.movement.emplace(); auto& m = *s.movement;
    m.mode_modifier = f(1); m.types = {{1, f(1), f(100), 0, true, false, false, false}, {2, f(1), f(50), 0, true, false, false, false, true}};
    m.players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}, {3, f(1), f(1), false}};
    m.planets = {{1, f(1), false}, {2, f(1), false}, {3, f(1), false}};
    m.contents = {{21, {}}, {22, {}}};
    for (const auto& p : s.players) for (const auto& q : s.planets) m.access.push_back({p.player_id, q.id, true, false});
    s.control.emplace(); auto& c = *s.control; c.change_seconds = f(7); c.max_ground_forces = 2;
    c.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : c.players) for (const auto& b : c.players) c.relations.push_back({a.player, b.player, a.player == b.player});
    c.types = {{1, 1, false, false}, {2, 0, false, true}};
    for (const auto& p : s.planets) {
        gc::ControlPlanet row; row.planet = p.id; row.type = 100 + p.id; row.surface_accessible = true; row.ground_base = {1, 1};
        c.planets.push_back(row);
    }
    return s;
}
ga::NativeActions::MovementInput movement_input(gc::PlayerId player, const gc::GalacticSnapshot& snapshot) {
    ga::NativeActions::MovementInput input; input.ai_player = true;
    const auto* control = snapshot.control();
    if (!control || !snapshot.movement()) throw std::runtime_error("movement control inputs missing");
    const auto allied = [&](gc::PlayerId other) {
        return std::any_of(control->relations.begin(), control->relations.end(), [&](const auto& r) { return r.player == player && r.other == other && r.allied; });
    };
    const auto neutral = [&](gc::PlayerId other) {
        return std::any_of(control->players.begin(), control->players.end(), [&](const auto& r) { return r.player == other && r.neutral; });
    };
    for (const auto& p : snapshot.planets()) {
        gc::PathPlanet row; row.id = p.id;
        row.relation = allied(p.owner) ? gc::PathRelation::allied : neutral(p.owner) ? gc::PathRelation::neutral : gc::PathRelation::hostile;
        row.ai_classification_succeeded = false; // WGM-19's explicit unavailable-classifier input
        for (const auto& q : control->planets) if (q.planet == p.id && q.starbase.level)
            row.starbase_relation = allied(q.starbase.owner) ? gc::PathRelation::allied : gc::PathRelation::hostile;
        for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == p.id) {
            row.fleet_presence_visible = true; row.nonallied_fleet = row.nonallied_fleet || !allied(fleet.owner);
        }
        // Harness observations are complete; absent defense is an explicit
        // observation, not the conservative unavailable-AI-perception default.
        row.ai_enemy_space_force = row.nonallied_fleet;
        for (const auto& q : snapshot.movement()->planets) if (q.planet == p.id) row.corrupted = q.corrupted;
        input.access.push_back(row); input.orbital_occupants[p.id] = 0;
    }
    for (const auto& state : control->state) input.orbital_occupants[state.planet] = state.orbital_player;
    for (const auto& p : snapshot.movement()->players) if (p.player == player) input.benefits_from_corruption = p.benefits_from_corruption;
    input.multiplayer = std::count_if(snapshot.seats().begin(), snapshot.seats().end(), [](const auto& seat) { return seat.kind == gc::SeatKind::human && seat.active; }) > 1;
    return input;
}
// Control fixture with a pay-as-you-go player 1 that can queue one ship at planet 2.
gc::GalacticSetup commit_fixture() {
    auto config = native_force_fixture();
    config.production->players[0].human = false; config.production->players[0].pay_as_you_go = true;
    config.seats[0].kind = gc::SeatKind::ai; config.seats[0].human_side_id = 0;
    config.production->maximum_human_queue = 1;
    gc::ProductionOffer offer; offer.type = 1; offer.domain = gc::ProductionDomain::space;
    offer.population = f(1);
    offer.price = f(4); offer.seconds = f(10); offer.affiliated = true; offer.direct_build_allowed = true;
    config.production->planets[1].offers = {offer};
    return config;
}
const gc::GalacticSession::NativeProductionRequest production_request{1, 2, 1, f(4)};
const gc::GalacticSession::NativeProductionRequest force_request{1, 2, 1, {}};
gc::GalacticSession::NativeProductionPayment no_payment() { return {}; }

Placement placement_at(eawr::sim::EntityId first, const std::vector<Fixed>& special) {
    Placement result; result.next_id = first + 1; result.special_control = special;
    result.structures.push_back({first, 1, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure});
    gc::CensusObject actor; actor.id = first; actor.original_type = actor.current_type = 1;
    actor.owner = 1; actor.planet = actor.container = 2; actor.combat_role = gc::ActorCombatRole::ordinary;
    result.originals.push_back(actor); return result;
}

// What each captured intent is expected to do on the commit world.
struct Expected { ga::IntentKind kind; ga::IntentFate fate; };

using Streams = std::map<gc::PlayerId, std::vector<Shadow::IntentRecord>>;

Shadow::IntentRecord forged(const Shadow::IntentRecord& previous, Shadow::Intent request) {
    // A retained non-allocating record continues the player's identity chain.
    return Shadow::IntentRecord{std::move(request), previous.next_id, previous.next_id, previous.next_invasion, previous.next_invasion};
}

// Both players decide over the same frozen root, each in its own identity block.
Streams capture(gc::GalacticSession& world, const eawr::sim::PartitionExecutor& executor) {
    Streams out;
    auto root = take(world.begin_native_actions(1, 64));
    const auto base = world.next_entity_id() + 4 * gc::galactic_max_objects;
    const auto sink = [&](gc::PlayerId player) {
        return [&out, player](auto, auto intent) { out[player].push_back(std::move(intent)); };
    };
    auto one = take(root.fork_player(1, base, 64, {}, sink(1)));
    auto two = take(root.fork_player(2, base + gc::galactic_max_objects, 64, {}, sink(2)));
    // player 1
    static_cast<void>(take(one.add_credits(1, f(50), f(1))));
    expect(take(one.launch_transport(1, 51, 0, false)).admitted, "player 1 projects a surface launch");
    expect(take(one.admit_fleet(1, gc::RemoveFleetMembers{10, {31}}, false)).admitted, "player 1 projects a member removal");
    expect(take(one.admit_fleet(1, gc::RemoveFleetMembers{10, {21}}, false)).admitted, "player 1 projects a second removal");
    static_cast<void>(take(one.create_force(force_request, [&](const auto& view, auto first) {
        return eawr::core::Result<Placement>::success(placement_at(first, view.presence.special_control));
    }, executor)));
    static_cast<void>(take(one.admit_production(production_request, executor, [](const auto&) {
        return eawr::core::Result<gc::GalacticSession::NativeProductionPayment>::success(no_payment());
    })));
    // A derived journal row of the launch (dropped when the launch is refused) and one of the build.
    out[1].insert(out[1].begin() + 2, forged(out[1][1],
        Shadow::JournalRequest{gc::NativeEffectKind::launch, {1, 2, 3}, gc::NativeEffectPhase::barrier}));
    out[1].push_back(forged(out[1].back(), Shadow::JournalRequest{gc::NativeEffectKind::production, {4, 5, 6}, gc::NativeEffectPhase::barrier}));
    // A finish of a build that never reached the queue, and a derived row behind it.
    out[1].push_back(forged(out[1].back(), Shadow::FinishRequest{987654, false, f(1)}));
    out[1].push_back(forged(out[1].back(), Shadow::JournalRequest{gc::NativeEffectKind::production_finish, {7}, gc::NativeEffectPhase::barrier}));
    // player 2
    static_cast<void>(take(two.add_credits(2, f(7), f(1))));
    take(two.recheck_orbit(2, 11));
    expect(take(two.admit_fleet(2, gc::RemoveFleetMembers{11, {22}}, false)).admitted, "player 2 projects a member removal");
    expect(take(two.admit_fleet(2, gc::CreateFleet{3, {22}, 0}, false)).admitted, "player 2 projects a fleet creation");
    out[2].push_back(forged(out[2].back(), Shadow::OrbitRecheckRequest{2, 987654}));
    return out;
}

// Earlier canonical work on the commit world: it consumes what some captured intents wanted.
void earlier_work(gc::GalacticSession& world) {
    expect(take(world.launch_native_transport(1, 51, 10)).admitted, "earlier work consumes the surface actor");
    expect(take(world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {31}}, false)).admitted, "earlier work removes a member");
    static_cast<void>(take(world.add_credits(1, f(5000), f(1))));
}

const std::map<gc::PlayerId, std::vector<Expected>>& expectations() {
    using K = ga::IntentKind; using F = ga::IntentFate;
    static const std::map<gc::PlayerId, std::vector<Expected>> table{
        {1, {{K::cash, F::adjusted}, {K::control, F::refused}, {K::journal, F::dropped}, {K::fleet, F::refused},
             {K::fleet, F::admitted}, {K::force, F::admitted}, {K::production, F::admitted}, {K::journal, F::journaled},
             {K::finish, F::refused}, {K::journal, F::dropped}}},
        {2, {{K::cash, F::admitted}, {K::orbit_recheck, F::admitted}, {K::fleet, F::admitted}, {K::fleet, F::admitted},
             {K::orbit_recheck, F::refused}}}};
    return table;
}

struct Run {
    ga::IntentCommitReport report;
    std::vector<std::uint8_t> state;
    gc::GalacticReplay record;
    std::vector<std::string> log;
};

enum class Arrival { ascending, descending, partitions };

Run commit_run(const eawr::sim::PartitionExecutor& executor, const std::array<gc::PlayerId, 2>& registration, Arrival arrival,
    bool preflight_ok = true) {
    auto capture_world = take(gc::GalacticSession::create(commit_fixture()));
    const auto streams = capture(capture_world, executor);
    auto world = take(gc::GalacticSession::create(commit_fixture()));
    earlier_work(world);
    world.set_native_production_handler([](const auto&) {
        return eawr::core::Result<gc::GalacticSession::NativeProductionPayment>::success(no_payment());
    });
    Run run;
    ga::IntentLedger ledger(registration);
    // Closures are created at the player's turn, one per retained intent.
    for (const auto player : registration) ledger.bind_hooks(player, [&run, player, preflight_ok](const auto& record, std::size_t index) {
        ga::IntentHooks hooks;
        const auto label = [player, index](const char* what, const ga::IntentOutcome& outcome) {
            return std::string(what) + ":" + std::to_string(player) + "/" + std::to_string(index) + "/"
                + ga::intent_kind_name(outcome.kind);
        };
        hooks.on_refused = [&run, label](const ga::IntentOutcome& outcome) {
            run.log.push_back(label("refused", outcome)); return eawr::core::Result<bool>::success(false);
        };
        hooks.on_admitted = [&run, label](const ga::IntentOutcome& outcome) {
            run.log.push_back(label("admitted", outcome)); return eawr::core::Result<void>::success();
        };
        if (ga::intent_kind(record) == ga::IntentKind::force)
            hooks.force_builder = [](const auto& view, auto first) {
                return OptionalPlacement::success(placement_at(first, view.presence.special_control));
            };
        if (ga::intent_kind(record) == ga::IntentKind::production)
            hooks.production_preflight = [preflight_ok](const auto&) { return eawr::core::Result<bool>::success(preflight_ok); };
        return hooks;
    });
    const auto submit = [&](gc::PlayerId player) {
        for (const auto& record : streams.at(player)) ledger.submit(player, record);
    };
    if (arrival == Arrival::ascending) { submit(1); submit(2); }
    else if (arrival == Arrival::descending) { submit(2); submit(1); }
    else {
        const std::array<gc::PlayerId, 2> players{registration[1], registration[0]};
        take(executor.execute_phase("test-intent-submit", eawr::sim::tick_partition_count, [&](std::size_t partition) {
            const auto range = eawr::sim::partition_range(partition, players.size());
            for (auto index = range.begin; index < range.end; ++index) submit(players[index]);
        }));
    }
    run.report = take(ga::commit_ordered_intents(world, ledger, executor));
    run.state = world.canonical_state_bytes();
    run.record = world.record();
    return run;
}

void ordered_commit_matrix() {
    const eawr::sim::InlineExecutor serial;
    const auto reference = commit_run(serial, {1, 2}, Arrival::ascending);
    // Every kind, accepted and refused, in the player's own order.
    for (const auto& [player, expected] : expectations()) {
        std::vector<ga::IntentOutcome> own;
        for (const auto& outcome : reference.report.outcomes) if (outcome.player == player) own.push_back(outcome);
        expect(own.size() == expected.size(), "every captured intent of the player has exactly one outcome");
        for (std::size_t i = 0; i < std::min(own.size(), expected.size()); ++i) {
            expect(own[i].index == i && own[i].kind == expected[i].kind && own[i].fate == expected[i].fate,
                "outcome " + std::to_string(player) + "/" + std::to_string(i) + " is " + ga::intent_kind_name(expected[i].kind)
                + " with the expected fate (got " + ga::intent_kind_name(own[i].kind) + ", fate " + std::to_string(static_cast<int>(own[i].fate)) + ")");
        }
    }
    bool ascending = true;
    for (std::size_t i = 1; i < reference.report.outcomes.size(); ++i)
        ascending = ascending && reference.report.outcomes[i - 1].player <= reference.report.outcomes[i].player;
    expect(ascending, "outcomes are in ascending player order");
    expect(reference.report.refused == 4 && reference.report.adjusted == 1 && reference.report.journaled == 1,
        "the report counts refused, adjusted and journaled intents");
    // Each hook ran once, for its own outcome, in commit order.
    std::size_t hook_calls{};
    for (const auto& outcome : reference.report.outcomes) if (outcome.kind != ga::IntentKind::journal) ++hook_calls;
    expect(reference.log.size() == hook_calls, "every non-journal intent runs exactly one hook");
    expect(!reference.log.empty() && reference.log.front().rfind("admitted:1/0/cash", 0) == 0,
        "player 1's first closure runs before any of player 2's");

    // A refused production keeps the speculative build out of the queue and out of the journal.
    const auto refused_build = commit_run(serial, {1, 2}, Arrival::ascending, false);
    std::size_t production_refused{};
    for (const auto& outcome : refused_build.report.outcomes)
        if (outcome.kind == ga::IntentKind::production && outcome.fate == ga::IntentFate::refused) ++production_refused;
    expect(production_refused == 1, "a failing preflight refuses the production intent");
    const auto journaled_row = [](const Run& run) {
        return std::count_if(run.report.outcomes.begin(), run.report.outcomes.end(),
            [](const auto& outcome) { return outcome.fate == ga::IntentFate::journaled; });
    };
    expect(journaled_row(refused_build) == 0 && journaled_row(reference) == 1,
        "the derived row of a refused build is dropped, the one of an admitted build is appended");

    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (const auto& registration : {std::array<gc::PlayerId, 2>{1, 2}, std::array<gc::PlayerId, 2>{2, 1}})
            for (const auto arrival : {Arrival::ascending, Arrival::descending, Arrival::partitions}) {
                const auto run = commit_run(executor, registration, arrival);
                expect(run.report == reference.report && run.state == reference.state && run.record == reference.record
                    && run.log == reference.log,
                    "outcomes, world, journal and hook order equal the reference at " + std::to_string(workers)
                    + " workers for any registration and arrival order");
            }
    }
    const eawr::test_support::ReverseOrderExecutor reverse(8);
    const auto flipped = commit_run(reverse, {2, 1}, Arrival::partitions);
    expect(flipped.report == reference.report && flipped.state == reference.state && flipped.record == reference.record,
        "a partition order that runs the highest partition first gives the same commit");

    // Intents from a player the barrier does not know fail the commit instead of vanishing.
    auto world = take(gc::GalacticSession::create(commit_fixture()));
    const std::array<gc::PlayerId, 1> only{1};
    ga::IntentLedger ledger(only);
    ledger.submit(9, Shadow::IntentRecord{Shadow::CashRequest{9, f(1), f(1), f(1)}, 1, 1, 1, 1});
    expect(!ga::commit_ordered_intents(world, ledger, serial), "an unknown player's intent fails the commit");
    // A force intent cannot be readmitted without its placement owner.
    std::vector<ga::PlayerIntent> missing;
    auto capture_world = take(gc::GalacticSession::create(commit_fixture()));
    auto streams = capture(capture_world, serial);
    for (const auto& record : streams.at(1)) if (ga::intent_kind(record) == ga::IntentKind::force) missing.push_back({1, record, {}});
    auto fresh = take(gc::GalacticSession::create(commit_fixture()));
    expect(!ga::commit_ordered_intents(fresh, std::move(missing), serial), "a force intent without a placement builder is refused loudly");
}

// The independent-players barrier feeds the ledger through its IntentSink from the worker threads.
std::vector<std::uint8_t> barrier_run(const eawr::sim::PartitionExecutor& executor, const std::array<gc::PlayerId, 2>& registration,
    gc::GalacticReplay& record, ga::IntentCommitReport& report) {
    const auto require = [](const auto& result) { if (!result) throw std::runtime_error(result.error().message); };
    const std::array<gc::PlayerId, 2> players{1, 2};
    auto world = take(gc::GalacticSession::create(native_control_fixture()));
    auto barrier = take(world.begin_native_barrier(players, 64));
    const auto prelude = take(world.add_credits(1, f(5), f(1)));
    require(world.append_native_effect(gc::NativeEffectKind::cash, ga::economic_replay::cash(1, f(5), f(1), prelude)));
    ga::IntentLedger ledger(registration);
    require(barrier->begin_independent_players(executor, ledger.sink()));
    require(executor.execute_phase("test-native-player-decisions", eawr::sim::tick_partition_count, [&](std::size_t partition) {
        const auto range = eawr::sim::partition_range(partition, players.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto scope = take(barrier->player_scope(players[index]));
            static_cast<void>(take(world.add_credits(players[index], f(static_cast<int>((index + 1) * 10)), f(1))));
            const auto fleet = index ? 11U : 10U, actor = index ? 22U : 20U, planet = index ? 3U : 2U;
            if (!take(world.admit_native_fleet(players[index], gc::RemoveFleetMembers{fleet, {actor}}, false)).admitted)
                throw std::runtime_error("independent member removal refused");
            if (!take(world.admit_native_fleet(players[index], gc::CreateFleet{planet, {actor}, 0}, false)).admitted)
                throw std::runtime_error("independent fleet creation refused");
        }
    }));
    require(barrier->finish_independent_players([&]() -> eawr::core::Result<void> {
        auto committed = ga::commit_ordered_intents(world, ledger, executor);
        if (!committed) return eawr::core::Result<void>::failure(committed.error());
        report = std::move(committed).value();
        return eawr::core::Result<void>::success();
    }));
    require(barrier->commit());
    record = world.record();
    return world.canonical_state_bytes();
}

void barrier_commit() {
    const eawr::sim::InlineExecutor serial;
    gc::GalacticReplay reference_record; ga::IntentCommitReport reference_report;
    const auto reference = barrier_run(serial, {1, 2}, reference_record, reference_report);
    expect(reference_report.refused == 0 && reference_report.admitted == 6 && reference_report.dropped == 4,
        "the barrier fixture admits every cash and fleet intent and drops the world's derived fleet rows");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (const auto& registration : {std::array<gc::PlayerId, 2>{1, 2}, std::array<gc::PlayerId, 2>{2, 1}}) {
            gc::GalacticReplay record; ga::IntentCommitReport report;
            const auto state = barrier_run(executor, registration, record, report);
            expect(state == reference && record == reference_record && report == reference_report,
                "the independent barrier commits the same world at " + std::to_string(workers) + " workers in any registration order");
        }
    }
}

// #6388: a refusal that arrives after the purchase completed (a late production or finish intent) must not turn the row into
// "failed" with its object still set; the restore of a save made after that refuses such a row.
void refusal_after_completion() {
    const eawr::sim::InlineExecutor executor;
    auto world = take(gc::GalacticSession::create(commit_fixture()));
    auto funding = std::make_shared<eawr::script::foc::ai::GalacticFunding>();
    const std::array policy{eawr::script::foc::ai::EvaluatedFundingTemplate{1, {{1, f(1)}}, {}}};
    take(funding->initialize(1, policy, f(200))); take(funding->reserve(1, {7, 1, f(100), {}, {}, {}}));
    ga::NativeActions actions(world, funding, {}, {{1, f(1)}}, {}, &executor,
        [](auto) { return eawr::core::Result<Fixed>::success(f(4)); });
    actions.bind_handlers();
    ga::ActionRequest request; request.kind = ga::ActionKind::produce; request.player = 1; request.target = 2; request.type = 1;
    const auto accepted = take(actions.admit_paid(request, 7));
    expect(accepted.admitted && actions.purchases().contains(accepted.request), "the build is retained by the native owner");
    if (!accepted.admitted) return;
    const auto build = accepted.request;
    gc::GalacticSession::NativeProductionOutcome done; done.player = 1; done.build = build; done.object = 777; done.completed = true;
    take(actions.apply_delivery(ga::NativeActions::Delivery{done}));
    expect(actions.purchases().at(build).completed && actions.purchases().at(build).object == 777, "the outcome completes the purchase with its object");
    const Shadow::IntentRecord production{Shadow::ProductionRequest{production_request,
        {gc::ProductionAdmission::allowed, build, f(4)}}, build, build + 1, 1, 1};
    const Shadow::IntentRecord finish{Shadow::FinishRequest{build, false, f(1)}, build + 1, build + 1, 1, 1};
    ga::IntentOutcome outcome; outcome.player = 1; outcome.fate = ga::IntentFate::refused;
    const auto revision = actions.build_state_revision();
    expect(!take(ga::refusal::production(actions, production)(outcome)) && !take(ga::refusal::finish(actions, finish)(outcome)),
        "a late refusal has no consequence for a completed purchase");
    const auto& purchase = actions.purchases().at(build);
    expect(purchase.completed && !purchase.failed && purchase.object == 777 && actions.build_state_revision() == revision,
        "the completed purchase keeps its object and is not marked failed");
}

// The refusal consequences map onto fields the native owner already persists.
void refusal_consequences() {
    const eawr::sim::InlineExecutor executor;
    auto world = take(gc::GalacticSession::create(commit_fixture()));
    auto funding = std::make_shared<eawr::script::foc::ai::GalacticFunding>();
    const std::array policy{eawr::script::foc::ai::EvaluatedFundingTemplate{1, {{1, f(1)}}, {}}};
    take(funding->initialize(1, policy, f(200))); take(funding->reserve(1, {7, 1, f(100), {}, {}, {}}));
    ga::NativeActions actions(world, funding, {}, {{1, f(1)}}, {}, &executor,
        [](auto) { return eawr::core::Result<Fixed>::success(f(4)); });
    actions.bind_handlers();
    ga::ActionRequest request; request.kind = ga::ActionKind::produce; request.player = 1; request.target = 2; request.type = 1;
    const auto accepted = take(actions.admit_paid(request, 7));
    expect(accepted.admitted && actions.purchases().contains(accepted.request), "the speculative build is retained by the native owner");
    if (!accepted.admitted) return;
    const auto build = accepted.request;
    const Shadow::IntentRecord production{Shadow::ProductionRequest{production_request,
        {gc::ProductionAdmission::allowed, build, f(4)}}, build, build + 1, 1, 1};
    const Shadow::IntentRecord finish{Shadow::FinishRequest{build, false, f(1)}, build + 1, build + 1, 1, 1};
    ga::IntentOutcome outcome; outcome.player = 1; outcome.fate = ga::IntentFate::refused;
    const auto revision = actions.build_state_revision();
    expect(!actions.purchases().at(build).failed, "an accepted build is not failed");
    expect(take(ga::refusal::production(actions, production)(outcome)) && actions.purchases().at(build).failed
        && !actions.purchases().at(build).completed && actions.build_state_revision() != revision,
        "a refused production fails its retained purchase so the goal refunds the mark");
    expect(!take(ga::refusal::production(actions, production)(outcome)) && !take(ga::refusal::finish(actions, finish)(outcome)),
        "refusal consequences are idempotent");
    ga::IntentOutcome foreign = outcome; foreign.player = 2;
    expect(!take(ga::refusal::finish(actions, finish)(foreign)), "another player's refusal cannot fail the purchase");

    // A refused hop ends the player's movement block for that fleet.
    auto mover = take(gc::GalacticSession::create(movement_fixture()));
    ga::NativeActions moves(mover, std::make_shared<eawr::script::foc::ai::GalacticFunding>(), ga::NativeActions::Placement{},
        std::map<gc::PlayerId, Fixed>{});
    moves.bind_admission_gate([] { return false; });
    moves.bind_movement([](gc::PlayerId player, const gc::GalacticSnapshot& snapshot) {
        return eawr::core::Result<ga::NativeActions::MovementInput>::success(movement_input(player, snapshot));
    }, [] { return false; }, [&](auto player, auto actor) { return mover.recheck_native_orbit(player, actor); });
    ga::ActionRequest move; move.kind = ga::ActionKind::move; move.player = 1; move.object = 10; move.target = 3;
    move.plan = 7; move.taskforce = 8; move.block = 9; move.members = {20, 21, 22};
    const auto moved = take(moves.admit(move));
    expect(moved.admitted && !moves.movements().at(moved.request).finished, "the speculative move is a retained unfinished block");
    if (!moved.admitted) return;
    const Shadow::IntentRecord hop{Shadow::FleetRequest{1, gc::MoveFleet{10, {1, 3}, {}}, false, {true, 0, {}}},
        production.next_id + 1, production.next_id + 1, 1, 1};
    expect(take(ga::refusal::fleet(moves, hop)(outcome)) && moves.movements().at(moved.request).finished,
        "a refused hop finishes the player's movement block");
    expect(!take(ga::refusal::fleet(moves, hop)(outcome)), "a finished movement block is left alone");
    const Shadow::IntentRecord merge{Shadow::FleetRequest{1, gc::MergeFleets{10, 11}, false, {true, 0, {}}}, hop.first_id, hop.first_id, 1, 1};
    expect(!take(ga::refusal::fleet(moves, merge)(outcome)), "a refused merge has no persisted consequence");
    expect(!take(ga::refusal::for_intent(moves, hop)(outcome)), "for_intent selects the fleet handler");
    const auto hooks = ga::refusal::standard_hooks(actions, production);
    expect(static_cast<bool>(hooks.on_refused) && !hooks.force_builder, "standard hooks carry only the refusal handler");
}


// Async AI A2.2: the outbox keeps a batch's intents per virtual tick and releases them at their apply tick.
// The async-lane determinism counts: the standard set plus 10 sim workers, as every W1 lane step proves.
std::vector<std::size_t> lane_worker_counts() {
    auto counts = eawr::platform::determinism_worker_counts();
    if (std::find(counts.begin(), counts.end(), std::size_t{10}) == counts.end()) counts.push_back(10);
    return counts;
}

Shadow::IntentRecord cash_record(gc::PlayerId player, eawr::sim::EntityId id, std::uint64_t invasion) {
    return Shadow::IntentRecord{Shadow::CashRequest{player, f(1), f(1), f(1)}, id, id, invasion, invasion};
}

void outbox_slices() {
    const auto gap = ga::async_identity_gap;
    expect(ga::identity_gap(ga::synchronous_reference_schedule) == 0 && ga::identity_gap(ga::standard_async_schedule) == gap,
        "only a deferred schedule leaves an identity gap");
    ga::IntentOutbox outbox(ga::standard_async_schedule);
    std::size_t hooks{};
    outbox.bind_hooks([&](gc::PlayerId, const Shadow::IntentRecord&) { ++hooks; return ga::IntentHooks{}; });
    expect(!outbox.next_apply_tick() && outbox.slices() == 0 && outbox.pending_intents() == 0, "a new outbox is empty");

    const auto start = outbox.begin_batch(100, {50, 7});
    expect(start.id == 50 + gap && start.invasion == 7 + gap && outbox.open(), "a deferred batch starts above the live frontier plus the gap");
    const auto sink = outbox.sink();
    sink(1, cash_record(1, start.id, start.invasion)); // outside a virtual tick: nothing to apply
    expect(outbox.slices() == 0, "an intent recorded outside a virtual tick is not retained");
    outbox.begin_virtual_tick(1);
    sink(1, cash_record(1, start.id, start.invasion));
    sink(2, cash_record(2, start.id + 3, start.invasion + 1));
    outbox.begin_virtual_tick(3);
    sink(1, cash_record(1, start.id + 5, start.invasion + 1));
    expect(outbox.slices() == 2 && outbox.pending_intents() == 3 && hooks == 3, "slices open per virtual tick and every intent gets its hooks");
    expect(outbox.take_due(1000).empty(), "an open batch is never released");
    outbox.end_batch();
    const auto block = ga::async_identity_block;
    expect(!outbox.open() && outbox.frontier() == ga::LaneIdentities{start.id + block, start.invasion + block},
        "the lane frontier is the end of the batch's fixed identity block, known from the batch start");
    expect(ga::identity_block(ga::synchronous_reference_schedule) == 0 && ga::identity_block(ga::standard_async_schedule) == block,
        "only a deferred schedule owns an identity block");
    expect(outbox.next_apply_tick() == std::optional<std::uint64_t>{130}, "the first slice applies at snapshot + delay");
    expect(outbox.take_due(129).empty(), "nothing is due before its apply tick");
    auto due = outbox.take_due(130);
    expect(due.size() == 1 && due[0].virtual_tick == 1 && due[0].apply_tick == 130 && due[0].snapshot_tick == 100
        && due[0].intents.size() == 2 && due[0].intents[0].player == 1 && due[0].intents[1].player == 2,
        "the first virtual tick applies at 130 with its two intents in stream order");
    expect(outbox.take_due(131).empty() && outbox.slices() == 1, "the third virtual tick is not due at 131");
    due = outbox.take_due(900);
    expect(due.size() == 1 && due[0].virtual_tick == 3 && due[0].apply_tick == 132, "the third virtual tick applies at 132");
    expect(outbox.slices() == 0 && outbox.pending_intents() == 0 && !outbox.next_apply_tick(), "released slices leave the outbox");

    // The lane frontier is monotone: a later batch never restarts below what the lane already used.
    const auto second = outbox.begin_batch(115, {10, 1});
    expect(second.id == start.id + block && second.invasion == start.invasion + block, "a later batch continues above the previous block");
    outbox.begin_virtual_tick(2);
    sink(1, cash_record(1, second.id, second.invasion));
    outbox.abandon_batch();
    expect(!outbox.open() && outbox.slices() == 0 && outbox.frontier() == ga::LaneIdentities{start.id + block, start.invasion + block},
        "an abandoned batch leaves neither slices nor identities behind");
    const auto third = outbox.begin_batch(115, {start.id + block + 10, 9});
    expect(third.id == start.id + block + 10 + gap, "the live frontier plus the gap wins when the world is ahead");
    outbox.end_batch();

    // The synchronous reference decides and applies in the same tick, with no gap.
    ga::IntentOutbox reference(ga::synchronous_reference_schedule);
    const auto now = reference.begin_batch(5, {50, 7});
    expect(now.id == 50 && now.invasion == 7, "the reference allocates the identities the unbatched barrier did");
    reference.begin_virtual_tick(1);
    reference.sink()(1, cash_record(1, now.id, now.invasion));
    reference.end_batch();
    expect(reference.next_apply_tick() == std::optional<std::uint64_t>{5} && reference.take_due(5).size() == 1,
        "the reference slice applies in its own tick");
}

// A lane decides in a serial barrier over the live world, keeps its intents and discards the barrier;
// the slices commit later through the ordered readmit.
struct LaneRun {
    ga::IntentCommitReport report;
    std::vector<std::uint8_t> before, discarded, after;
    eawr::sim::EntityId lane_first{}, live_next{}, reserved{}, final_next{};
    std::size_t reservations{};
    bool replay_matches{};
};

// bump: the world reserves the whole identity block when the batch begins and the sim allocates between two slices.
LaneRun lane_run(const eawr::sim::PartitionExecutor& executor, const ga::AsyncSchedule& schedule, bool world_allocates, bool bump = false, bool reload = false) {
    const std::array<gc::PlayerId, 2> players{1, 2};
    LaneRun run;
    auto world = take(gc::GalacticSession::create(native_control_fixture()));
    run.before = world.canonical_state_bytes();
    ga::IntentOutbox outbox(schedule);
    const auto start = outbox.begin_batch(100, {world.next_entity_id(), world.next_native_invasion_id()});
    run.lane_first = start.id;
    {
        gc::GalacticSession::NativeBarrierOptions options;
        options.first_id = start.id; options.first_invasion = start.invasion; options.intent_sink = outbox.sink(); options.lane = true;
        if (schedule.apply_delay != 0) { options.id_end = outbox.frontier().id; options.invasion_end = outbox.frontier().invasion; }
        // The block is reserved when the window begins, not after it.
        if (bump) take(world.reserve_native_frontiers(world.next_entity_id(), outbox.frontier().id,
            world.next_native_invasion_id(), outbox.frontier().invasion));
        auto barrier = take(world.begin_native_barrier(players, 64, std::move(options)));
        take(barrier->begin_players());
        // Virtual tick 1: both players, then the tail.
        outbox.begin_virtual_tick(1);
        take(barrier->select_player(1));
        expect(take(world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {20}}, false)).admitted, "the lane removes a member");
        expect(take(world.admit_native_fleet(1, gc::CreateFleet{2, {20}, 0}, false)).admitted, "the lane creates a fleet");
        take(barrier->select_player(2));
        expect(take(world.admit_native_fleet(2, gc::RemoveFleetMembers{11, {22}}, false)).admitted, "the lane removes a second member");
        take(barrier->begin_tail());
        // Virtual tick 2: a new turn over what tick 1 decided; the players take it in the opposite order.
        outbox.begin_virtual_tick(2);
        take(barrier->begin_turn());
        take(barrier->select_player(2));
        expect(take(world.admit_native_fleet(2, gc::CreateFleet{3, {22}, 0}, false)).admitted, "the lane creates a second fleet from its own earlier removal");
        take(barrier->select_player(1));
        expect(take(world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {21}}, false)).admitted, "player 1 acts again after player 2");
        take(barrier->discard());
    }
    outbox.end_batch();
    run.discarded = world.canonical_state_bytes();
    if (world_allocates) {
        // The world allocates an identity between the snapshot and the apply tick.
        expect(take(world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {31}}, false)).admitted, "the world removes a member");
        expect(take(world.admit_native_fleet(1, gc::CreateFleet{2, {31}, 0}, false)).admitted, "the world creates a fleet");
    }
    run.live_next = world.next_entity_id(); run.reserved = outbox.frontier().id;
    // reload: the pending work goes through its saved form first, as after a save in the middle of the window.
    ga::IntentOutbox reloaded(schedule);
    ga::IntentOutbox* pending = &outbox;
    if (reload) {
        auto decoded = take(ga::decode_outbox_state(take(ga::encode_outbox_state(outbox, 0))));
        reloaded.restore(decoded.frontier, decoded.floor, std::move(decoded.slices));
        pending = &reloaded;
    }
    ga::IntentCommitOptions options; options.refuse_stale_identities = true;
    if (bump) { options.identity_floor = pending->lane_floor(); options.invasion_floor = pending->lane_invasion_floor(); }
    bool first_slice = true;
    for (auto& slice : pending->take_due(1000)) {
        auto committed = take(ga::commit_ordered_intents(world, std::move(slice.intents), executor, options));
        if (bump && first_slice) {
            first_slice = false;
            // The sim's own allocation between two apply ticks: it must land above the whole lane range.
            expect(world.next_entity_id() == run.reserved, "a readmitted slice leaves the world counter above the lane range");
            expect(take(world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {31}}, false)).admitted, "the sim removes a member between slices");
            expect(take(world.admit_native_fleet(1, gc::CreateFleet{2, {31}, 0}, false)).admitted, "the sim creates a fleet between slices");
            expect(world.next_entity_id() == run.reserved + 1, "the sim allocated above the lane range");
        }
        run.report.admitted += committed.admitted; run.report.refused += committed.refused;
        run.report.dropped += committed.dropped; run.report.stale += committed.stale;
        run.report.outcomes.insert(run.report.outcomes.end(), committed.outcomes.begin(), committed.outcomes.end());
    }
    run.after = world.canonical_state_bytes();
    run.final_next = world.next_entity_id();
    if (bump) {
        // The journal reproduces every counter move: reservation, rewinds, returns and the sim's own allocation.
        auto replay = take(gc::GalacticSession::from_replay(world.record()));
        take(replay.replay_native_barrier(executor));
        run.replay_matches = replay.canonical_state_bytes() == run.after && replay.next_entity_id() == run.final_next;
    }
    for (const auto& effect : world.native_effects()) if (effect.kind == gc::NativeEffectKind::identity_reservation) ++run.reservations;
    return run;
}

// The saved form of the pending work: every request kind, commands, timing validation, no closure saved.
void outbox_state() {
    const auto& A = ga::standard_async_schedule;
    ga::IntentOutbox outbox(A);
    std::size_t derived{};
    outbox.bind_hooks([&](gc::PlayerId, const Shadow::IntentRecord&) { ++derived; return ga::IntentHooks{}; });
    const auto start = outbox.begin_batch(40, {1000, 7});
    const auto sink = outbox.sink();
    const gc::GalacticSession::NativeProductionRequest production{2, 5, 9, f(3)};
    auto id = start.id; auto inv = start.invasion;
    const auto record = [&](Shadow::Intent request, int ids, int invasions) {
        Shadow::IntentRecord row{std::move(request), id, id + ids, inv, inv + invasions};
        id += ids; inv += invasions; return row;
    };
    outbox.begin_virtual_tick(1);
    sink(1, record(Shadow::FleetRequest{1, gc::MoveFleet{10, {1, 3}, {}}, true, {true, 77, gc::GalacticRejectReason::none}}, 0, 0));
    sink(1, record(Shadow::ControlRequest{Shadow::ControlRequest::Kind::actor_land, 1, 51, 3, false, 4, {false, 0, gc::GalacticRejectReason::unknown_subject}, true}, 0, 0));
    sink(1, record(Shadow::CashRequest{1, f(5), f(2), f(4)}, 0, 0));
    sink(2, record(Shadow::ForceRequest{std::make_shared<const Shadow::Force>(Shadow::Force{production, {}}), {41, 42}}, 2, 0));
    sink(2, record(Shadow::ProductionRequest{production, {gc::ProductionAdmission::allowed, 55, f(3)}}, 1, 0));
    sink(2, record(Shadow::FinishRequest{55, true, f(1)}, 0, 0));
    outbox.begin_virtual_tick(2);
    sink(3, record(Shadow::InvasionRequest{false, 3, 12, {61, 62}, false, {true, 8, true, gc::GalacticRejectReason::none}, 8}, 0, 1));
    sink(3, record(Shadow::JournalRequest{gc::NativeEffectKind::cash, {1, 2, 3}, gc::NativeEffectPhase::world_service}, 0, 0));
    sink(3, record(Shadow::OrbitRecheckRequest{3, 12}, 0, 0));
    {
        std::vector<eawr::script::authoritative::ScriptCommand> commands;
        eawr::script::authoritative::ScriptCommand command;
        command.tick = 41; command.issuer = 7; command.sequence = 3; command.verb = "Spawn";
        command.arguments = {eawr::script::authoritative::Value::text("Unit"), eawr::script::authoritative::Value{true},
            eawr::script::authoritative::Value{eawr::script::authoritative::Handle{2, 9}},
            eawr::script::authoritative::Value{eawr::script::authoritative::ValueList{eawr::script::authoritative::Value{}}}};
        commands.push_back(command);
        expect(outbox.add_commands(commands) && commands.empty(), "the open virtual tick keeps the commands");
    }
    outbox.end_batch();
    derived = 0;
    const auto bytes = take(ga::encode_outbox_state(outbox, 123));
    const auto again = take(ga::encode_outbox_state(outbox, 123));
    expect(bytes == again && take(ga::encode_outbox_state(outbox, 124)) != bytes, "the saved form is canonical and holds the batch counter");

    const auto decoded = take(ga::decode_outbox_state(bytes));
    expect(decoded.schedule == A && decoded.ai_ticks == 123 && decoded.frontier == outbox.frontier() && decoded.slices.size() == 2,
        "the header, the batch counter, the frontier and both slices survive");
    expect(decoded.slices[0].intents.size() == 6 && decoded.slices[1].intents.size() == 3 && decoded.slices[1].commands.size() == 1
        && decoded.slices[0].apply_tick == 70 && decoded.slices[1].apply_tick == 71, "intents, commands and apply ticks survive");
    ga::IntentOutbox loaded(A);
    loaded.bind_hooks([&](gc::PlayerId, const Shadow::IntentRecord&) { ++derived; return ga::IntentHooks{}; });
    expect(decoded.floor == 1000, "the lane floor survives");
    loaded.restore(decoded.frontier, decoded.floor, decoded.slices);
    expect(outbox.lane_invasion_floor() == start.invasion && loaded.lane_invasion_floor() == start.invasion,
        "the invasion floor is the lowest pending handle, the same after a load");
    expect(derived == 9, "every retained intent gets its hooks derived again on load");
    expect(take(ga::encode_outbox_state(loaded, 123)) == bytes, "a loaded outbox saves the same bytes");
    expect(loaded.pending_slices()[0].intents[3].player == 2
        && std::get<Shadow::ForceRequest>(loaded.pending_slices()[0].intents[3].record.request).actors == std::vector<eawr::sim::EntityId>({41, 42}),
        "a force intent keeps its request and actors");

    auto cut = bytes; cut.pop_back();
    expect(!ga::decode_outbox_state(cut), "a truncated state is refused");
    auto trailing = bytes; trailing.push_back(0);
    expect(!ga::decode_outbox_state(trailing), "trailing bytes are refused");
    auto wrong = bytes; wrong[0] = 9;
    expect(!ga::decode_outbox_state(wrong), "an unknown version is refused");
    auto timing = bytes; timing[8] = 31; // apply_delay 30 -> 31: the slice apply ticks no longer follow the schedule
    expect(!ga::decode_outbox_state(timing), "slice timing that differs from the schedule is refused");
}

void deferred_lane() {
    const eawr::sim::InlineExecutor serial;
    const auto quiet = lane_run(serial, ga::standard_async_schedule, false);
    expect(quiet.discarded == quiet.before, "a discarded lane barrier leaves the live world untouched");
    expect(quiet.report.admitted == 5 && quiet.report.refused == 0 && quiet.report.stale == 0 && quiet.after != quiet.before,
        "the deferred slices commit all five fleet intents of two turns on an unchanged world");
    expect(quiet.lane_first == quiet.live_next + ga::async_identity_gap, "the lane identities start above the gap");

    const auto busy = lane_run(serial, ga::standard_async_schedule, true);
    expect(busy.report.admitted == 5 && busy.report.refused == 0 && busy.report.stale == 0,
        "identities the world allocated meanwhile stay below the lane's gap: nothing is refused");
    expect(busy.after != quiet.after, "the world's own work is part of the result");

    // The world reserves the lane range once the batch has ended: the sim allocates between two apply ticks and no
    // slice is refused, every slice is readmitted at its own identities.
    const auto bumped = lane_run(serial, ga::standard_async_schedule, false, true);
    expect(bumped.report.admitted == 5 && bumped.report.refused == 0 && bumped.report.stale == 0,
        "a reserved lane range: the sim allocates between slices and nothing is refused or stale");
    expect(bumped.reserved > bumped.lane_first && bumped.final_next == bumped.reserved + 1,
        "the world counter ends above the lane range plus the sim's one identity");
    expect(bumped.reservations >= 3, "the reservation and every rewind are journaled");
    expect(bumped.replay_matches, "replaying the journal reproduces the identities and the world of the rewind path");
    const auto reloaded = lane_run(serial, ga::standard_async_schedule, false, true, true);
    expect(reloaded.after == bumped.after && reloaded.report.outcomes == bumped.report.outcomes && reloaded.replay_matches,
        "pending slices saved and loaded in the middle of the window apply to the same world");
    for (const auto workers : lane_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto again = lane_run(executor, ga::standard_async_schedule, false, true);
        expect(again.after == bumped.after && again.report.outcomes == bumped.report.outcomes,
            "the reserved lane range gives the same world at " + std::to_string(workers) + " workers");
    }

    // Without the gap the world's allocation reaches the lane's first identity: a refusal, not a world error.
    const auto reached = lane_run(serial, ga::synchronous_reference_schedule, true);
    expect(reached.report.stale >= 1 && reached.report.refused >= reached.report.stale,
        "an intent the world's identities have reached is refused as stale");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto again = lane_run(executor, ga::standard_async_schedule, true);
        expect(again.after == busy.after && again.report.outcomes == busy.report.outcomes,
            "the deferred commit gives the same world at " + std::to_string(workers) + " workers");
    }
}

// A readmitted span that ends exactly at the value the counter was rewound from closes the rewind: the flag must not stay set
// (a later reservation would be refused), and the journal must replay to the same world and flag.
void rewind_closes_at_counter() {
    const eawr::sim::InlineExecutor serial;
    const std::array<gc::PlayerId, 2> players{1, 2};
    auto world = take(gc::GalacticSession::create(native_control_fixture()));
    ga::IntentOutbox outbox(ga::standard_async_schedule);
    const auto start = outbox.begin_batch(100, {world.next_entity_id(), world.next_native_invasion_id()});
    {
        gc::GalacticSession::NativeBarrierOptions options;
        options.first_id = start.id; options.first_invasion = start.invasion; options.intent_sink = outbox.sink(); options.lane = true;
        auto barrier = take(world.begin_native_barrier(players, 64, std::move(options)));
        take(barrier->begin_players());
        outbox.begin_virtual_tick(1);
        take(barrier->select_player(1));
        expect(take(world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {20}}, false)).admitted, "close: the lane removes a member");
        expect(take(world.admit_native_fleet(1, gc::CreateFleet{2, {20}, 0}, false)).admitted, "close: the lane creates a fleet");
        take(barrier->discard());
    }
    outbox.end_batch();
    // The world's counter sits exactly at the end of the last lane span (not at the end of the whole lane range).
    take(world.reserve_native_identity_frontier(world.next_entity_id(), start.id + 1));
    ga::IntentCommitOptions options; options.refuse_stale_identities = true; options.identity_floor = outbox.lane_floor();
    for (auto& slice : outbox.take_due(1000)) take(ga::commit_ordered_intents(world, std::move(slice.intents), serial, options));
    expect(world.next_entity_id() == start.id + 1, "close: the counter ends where it was reserved");
    expect(static_cast<bool>(world.reserve_native_identity_frontier(world.next_entity_id(), world.next_entity_id() + 4)),
        "close: a later identity reservation is not refused by a rewind flag left set");
    auto replay = take(gc::GalacticSession::from_replay(world.record()));
    take(replay.replay_native_barrier(serial));
    expect(replay.canonical_state_bytes() == world.canonical_state_bytes() && replay.next_entity_id() == world.next_entity_id(),
        "close: the journal replays to the same world");
}

// A window owns a fixed identity block: allocating past its end refuses the command, deterministically, and the world stays usable.
struct BlockRun { bool first{}, second{}, third{}; std::vector<std::uint8_t> after; eawr::sim::EntityId live_after_window{}, end{}; };
BlockRun block_run() {
    const std::array<gc::PlayerId, 2> players{1, 2};
    BlockRun run;
    auto world = take(gc::GalacticSession::create(native_control_fixture()));
    ga::IntentOutbox outbox(ga::standard_async_schedule);
    const auto start = outbox.begin_batch(100, {world.next_entity_id(), world.next_native_invasion_id()});
    gc::GalacticSession::NativeBarrierOptions options;
    options.first_id = start.id; options.first_invasion = start.invasion; options.intent_sink = outbox.sink(); options.lane = true;
    options.id_end = start.id + 2; options.invasion_end = start.invasion + 2; // room for exactly two identities
    run.end = options.id_end;
    take(world.reserve_native_identity_frontier(world.next_entity_id(), run.end));
    auto barrier = take(world.begin_native_barrier(players, 64, std::move(options)));
    take(barrier->begin_players());
    outbox.begin_virtual_tick(1);
    take(barrier->select_player(1));
    expect(take(world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {20, 21, 31}}, false)).admitted, "block: detach player 1 members");
    run.first = take(world.admit_native_fleet(1, gc::CreateFleet{2, {20}, 0}, false)).admitted;
    run.second = take(world.admit_native_fleet(1, gc::CreateFleet{2, {21}, 0}, false)).admitted;
    take(barrier->select_player(2));
    expect(take(world.admit_native_fleet(2, gc::RemoveFleetMembers{11, {22}}, false)).admitted, "block: detach player 2 member");
    run.third = take(world.admit_native_fleet(2, gc::CreateFleet{3, {22}, 0}, false)).admitted;
    take(barrier->discard());
    outbox.end_batch();
    run.live_after_window = world.next_entity_id();
    run.after = world.canonical_state_bytes();
    return run;
}

// The invasion block: a window reserves its invasion handles at its start too, so a human invasion between the window and the
// slice's apply tick takes a handle above the block, and the AI invasion is still readmitted at its own lane handle.
struct InvasionRun {
    ga::IntentCommitReport report;
    std::vector<std::uint8_t> after;
    std::uint64_t lane_handle{}, block_end{}, human_handle{}, final_next{};
    bool reserved{}, replay_matches{}, lane_held{}, human_held{}, held_after_apply{};
};
InvasionRun invasion_block_run(const eawr::sim::PartitionExecutor& executor, bool reload) {
    const std::array<gc::PlayerId, 2> players{1, 2};
    InvasionRun run;
    auto config = native_control_fixture(); config.control->planets[1].ground_base.owner = 2;
    auto world = take(gc::GalacticSession::create(config)); world.enable_native_invasions(true);
    ga::IntentOutbox outbox(ga::standard_async_schedule);
    const auto start = outbox.begin_batch(100, {world.next_entity_id(), world.next_native_invasion_id()});
    run.lane_handle = start.invasion; run.block_end = outbox.frontier().invasion;
    {
        gc::GalacticSession::NativeBarrierOptions options;
        options.first_id = start.id; options.first_invasion = start.invasion; options.intent_sink = outbox.sink(); options.lane = true;
        options.id_end = outbox.frontier().id; options.invasion_end = outbox.frontier().invasion;
        take(world.reserve_native_frontiers(world.next_entity_id(), options.id_end, world.next_native_invasion_id(), options.invasion_end));
        auto barrier = take(world.begin_native_barrier(players, 64, std::move(options)));
        take(barrier->begin_players());
        outbox.begin_virtual_tick(1);
        take(barrier->select_player(1));
        const std::array<eawr::sim::EntityId, 1> lane_transports{31};
        const auto staged = take(world.admit_native_invasion(1, 10, lane_transports, false));
        expect(staged.admitted && staged.request == run.lane_handle, "invasion block: the lane invasion takes the block's first handle");
        take(barrier->discard());
    }
    outbox.end_batch();
    run.reserved = world.next_native_invasion_id() == run.block_end;
    // A human invasion between the window and the apply tick.
    const std::array<eawr::sim::EntityId, 1> human_transports{21};
    const auto human = take(world.admit_native_invasion(1, 10, human_transports, false));
    expect(human.admitted, "invasion block: the human invasion is admitted");
    run.human_handle = human.request;
    ga::IntentOutbox reloaded(ga::standard_async_schedule);
    ga::IntentOutbox* pending = &outbox;
    if (reload) {
        auto decoded = take(ga::decode_outbox_state(take(ga::encode_outbox_state(outbox, 0))));
        reloaded.restore(decoded.frontier, decoded.floor, std::move(decoded.slices));
        pending = &reloaded;
    }
    ga::IntentCommitOptions options; options.refuse_stale_identities = true;
    options.identity_floor = pending->lane_floor(); options.invasion_floor = pending->lane_invasion_floor();
    run.lane_held = pending->holds_invasion(run.lane_handle); run.human_held = pending->holds_invasion(run.human_handle);
    for (auto& slice : pending->take_due(1000)) {
        auto committed = take(ga::commit_ordered_intents(world, std::move(slice.intents), executor, options));
        run.report.admitted += committed.admitted; run.report.refused += committed.refused; run.report.stale += committed.stale;
        run.report.outcomes.insert(run.report.outcomes.end(), committed.outcomes.begin(), committed.outcomes.end());
    }
    run.held_after_apply = pending->holds_invasion(run.lane_handle);
    run.after = world.canonical_state_bytes();
    run.final_next = world.next_native_invasion_id();
    auto replay = take(gc::GalacticSession::from_replay(world.record()));
    ga::NativeActions::bind_economic_replay_owners(replay);
    take(replay.replay_native_barrier(executor));
    run.replay_matches = replay.canonical_state_bytes() == run.after && replay.next_native_invasion_id() == run.final_next;
    return run;
}

void invasion_block() {
    const eawr::sim::InlineExecutor serial;
    const auto run = invasion_block_run(serial, false);
    expect(run.reserved, "the world invasion counter is reserved to the block end when the window begins");
    expect(run.human_handle == run.block_end, "a human invasion between the window and its apply lands above the lane block");
    // The world counter already stands at the block end, so only the outbox can say the lane handle is pending (goal-state check).
    expect(run.lane_held && !run.human_held && !run.held_after_apply,
        "the outbox holds the lane invasion handle until its slice applies, and never the human's");
    // The lane may also hand over non-allocating records (journal, finish); only the invasion intent is judged here.
    std::size_t invasions{};
    const ga::IntentOutcome* invasion{};
    for (const auto& outcome : run.report.outcomes)
        if (outcome.kind == ga::IntentKind::invasion) { ++invasions; invasion = &outcome; }
    const auto described = "outcomes=" + std::to_string(run.report.outcomes.size()) + " invasions=" + std::to_string(invasions)
        + " stale=" + std::to_string(run.report.stale) + " fate=" + std::to_string(invasion ? static_cast<int>(invasion->fate) : -1)
        + " reason=" + std::to_string(invasion ? static_cast<int>(invasion->reason) : -1)
        + " request=" + std::to_string(invasion ? invasion->request : 0) + " lane=" + std::to_string(run.lane_handle)
        + " human=" + std::to_string(run.human_handle) + " end=" + std::to_string(run.block_end);
    expect(invasions == 1 && run.report.stale == 0 && invasion->fate == ga::IntentFate::admitted && invasion->request == run.lane_handle,
        "the AI invasion is readmitted at its own lane handle, not refused as stale (" + described + ")");
    expect(run.final_next == run.block_end + 1, "the world invasion counter returns above the human's handle");
    expect(run.replay_matches, "the journal replays the reservation, the rewind and the return to the same world");
    const auto reloaded = invasion_block_run(serial, true);
    expect(reloaded.after == run.after && reloaded.report.outcomes == run.report.outcomes && reloaded.replay_matches
        && reloaded.lane_held && !reloaded.human_held && !reloaded.held_after_apply,
        "the pending invasion saved and loaded in the middle of the window applies to the same world");
    for (const auto workers : lane_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto again = invasion_block_run(executor, false);
        expect(again.after == run.after && again.report.outcomes == run.report.outcomes,
            "the invasion block gives the same world at " + std::to_string(workers) + " workers");
    }
}

void identity_block_exhaustion() {
    const auto a = block_run(); const auto b = block_run();
    expect(a.first && a.second && !a.third, "the window takes identities up to the end of its block and refuses the next command");
    expect(a.live_after_window == a.end, "the world counter was reserved to the block end when the window began");
    expect(a.first == b.first && a.second == b.second && a.third == b.third && a.after == b.after, "block exhaustion is deterministic");
}

// The window overlay (T2): a lane window starts from the world plus the still-pending slices of earlier windows, so a decision that
// a pending slice already consumed is refused by the lane itself instead of being admitted and refused at the real apply.
struct OverlayWindow {
    bool second_admitted{}, player2_admitted{}, create_admitted{}, fresh_admitted{}; std::size_t records{}, replayed{}, refused{};
};
OverlayWindow overlay_window(const eawr::sim::PartitionExecutor& executor, bool with_overlay, bool reload = false) {
    const std::array<gc::PlayerId, 2> players{1, 2};
    OverlayWindow out;
    auto world = take(gc::GalacticSession::create(native_control_fixture()));
    ga::IntentOutbox outbox(ga::standard_async_schedule);
    {   // Window 1 removes member 20 of fleet 10.
        const auto start = outbox.begin_batch(100, {world.next_entity_id(), world.next_native_invasion_id()});
        gc::GalacticSession::NativeBarrierOptions options;
        options.first_id = start.id; options.first_invasion = start.invasion; options.intent_sink = outbox.sink(); options.lane = true;
        auto barrier = take(world.begin_native_barrier(players, 64, std::move(options)));
        take(barrier->begin_players());
        outbox.begin_virtual_tick(1);
        take(barrier->select_player(1));
        expect(take(world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {20}}, false)).admitted, "window 1 removes a member");
        take(barrier->select_player(2));
        expect(take(world.admit_native_fleet(2, gc::RemoveFleetMembers{11, {22}}, false)).admitted, "window 1: player 2 removes its own member");
        take(barrier->begin_tail());
        take(barrier->discard());
        outbox.end_batch();
    }
    // Case (f): the pending slices go through their saved form (a save in the middle of the window) before the overlay is built.
    ga::IntentOutbox reloaded(ga::standard_async_schedule);
    ga::IntentOutbox* pending = &outbox;
    if (reload) {
        auto decoded = take(ga::decode_outbox_state(take(ga::encode_outbox_state(outbox, 0))));
        reloaded.restore(decoded.frontier, decoded.floor, std::move(decoded.slices));
        pending = &reloaded;
    }
    std::optional<gc::GalacticSession::NativeActionShadow> overlay;
    if (with_overlay) {
        ga::IntentCommitOptions commit; commit.refuse_stale_identities = true; commit.identity_floor = pending->lane_floor();
        std::vector<gc::GalacticSession::NativeOverlayRecord> records;
        for (const auto& slice : pending->pending_slices())
            for (const auto& step : ga::plan_slice(slice.intents)) {
                if (ga::slice_step_stale(world, step.intent->record, commit)) continue;
                records.push_back({step.intent->player, &step.intent->record});
            }
        gc::GalacticSession::NativeOverlayStats stats;
        overlay.emplace(take(world.build_native_overlay(1, records, executor, stats)));
        out.records = stats.records; out.replayed = stats.replayed; out.refused = stats.refused;
    }
    {   // Window 2 tries the same removal over the same world.
        const auto start = outbox.begin_batch(115, {world.next_entity_id(), world.next_native_invasion_id()});
        gc::GalacticSession::NativeBarrierOptions options;
        options.first_id = start.id; options.first_invasion = start.invasion; options.intent_sink = outbox.sink(); options.lane = true;
        options.overlay = std::move(overlay);
        auto barrier = take(world.begin_native_barrier(players, 64, std::move(options)));
        take(barrier->begin_players());
        outbox.begin_virtual_tick(1);
        take(barrier->select_player(1));
        auto again = world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {20}}, false);
        out.second_admitted = again && again.value().admitted;
        // T3 (the lost-actor pattern): the freed member is seen as freed (creation from it differs), and a refusal leaves nothing:
        // a different member of the same fleet is still free (case (c)).
        auto created = world.admit_native_fleet(1, gc::CreateFleet{2, {20}, 0}, false);
        out.create_admitted = created && created.value().admitted;
        auto fresh = world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {21}}, false);
        out.fresh_admitted = fresh && fresh.value().admitted;
        // Mixed players on one overlay chain (the b19-b21 launch-layer regression): player 2's consumed member.
        take(barrier->select_player(2));
        auto other = world.admit_native_fleet(2, gc::RemoveFleetMembers{11, {22}}, false);
        out.player2_admitted = other && other.value().admitted;
        take(barrier->discard());
        outbox.abandon_batch();
    }
    return out;
}

void overlay_stale_fleet() {
    const eawr::sim::InlineExecutor serial;
    const auto bare = overlay_window(serial, false);
    expect(bare.second_admitted, "negative control: without the overlay the lane re-admits a removal a pending slice already made");
    const auto overlaid = overlay_window(serial, true);
    expect(overlaid.records >= 1 && overlaid.replayed >= 1, "the overlay replays the pending record(s): records=" + std::to_string(overlaid.records) + " replayed=" + std::to_string(overlaid.replayed));
    expect(!overlaid.second_admitted, "with the overlay the lane refuses the removal itself");
    // T3 finding (b29): creating a fleet from the freed member is admitted with and without the overlay (the member is free in both),
    // so only the removal and the second player are decisive; create_admitted is kept to show the refusal path leaves it usable.
    expect(bare.player2_admitted && !overlaid.player2_admitted, "the second player's pending removal is part of the overlay (mixed players on one chain)");
    expect(bare.fresh_admitted && overlaid.fresh_admitted, "a refusal leaves no rows: an untouched member is still removable");
    expect(overlaid.refused == 0, "every pending record replays");
    const auto saved = overlay_window(serial, true, true);
    expect(saved.second_admitted == overlaid.second_admitted && saved.create_admitted == overlaid.create_admitted
        && saved.player2_admitted == overlaid.player2_admitted && saved.records == overlaid.records,
        "a save in the middle of the window gives the same overlay decisions");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto again = overlay_window(executor, true);
        expect(again.second_admitted == overlaid.second_admitted && again.records == overlaid.records,
            "the overlay gives the same decision at " + std::to_string(workers) + " workers");
    }
}

// T1: the overlaid root equals the really-applied world. Projection compared: every fleet row (id, owner, orbit, members, slot,
// motion, raid destination). Excluded and why: payments/ledger rows (the overlay replays shadow verbs with a no-op payment, the world
// charges), journal/native-effect rows (the overlay root clears its logs), identity counters (the barrier resets them) and
// everything the fixture does not write. Negative control: dropping the last record (player 2's removal) must make them differ.
struct ProjectionRun { bool fleets_equal{}; bool control_differs{}; std::size_t fleets{}; std::size_t replayed{}; };
ProjectionRun overlay_projection(const eawr::sim::PartitionExecutor& executor) {
    const std::array<gc::PlayerId, 2> players{1, 2};
    ProjectionRun out;
    auto world = take(gc::GalacticSession::create(native_control_fixture()));
    ga::IntentOutbox outbox(ga::standard_async_schedule);
    {
        const auto start = outbox.begin_batch(100, {world.next_entity_id(), world.next_native_invasion_id()});
        gc::GalacticSession::NativeBarrierOptions options;
        options.first_id = start.id; options.first_invasion = start.invasion; options.intent_sink = outbox.sink(); options.lane = true;
        auto barrier = take(world.begin_native_barrier(players, 64, std::move(options)));
        take(barrier->begin_players());
        outbox.begin_virtual_tick(1);
        take(barrier->select_player(1));
        expect(take(world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {20}}, false)).admitted, "T1 window removes a member");
        expect(take(world.admit_native_fleet(1, gc::CreateFleet{2, {20}, 0}, false)).admitted, "T1 window creates a fleet");
        take(barrier->select_player(2));
        expect(take(world.admit_native_fleet(2, gc::RemoveFleetMembers{11, {22}}, false)).admitted, "T1 player 2 removes its member");
        take(barrier->begin_tail());
        take(barrier->discard());
        outbox.end_batch();
    }
    ga::IntentCommitOptions commit; commit.refuse_stale_identities = true; commit.identity_floor = outbox.lane_floor();
    std::vector<gc::GalacticSession::NativeOverlayRecord> records;
    for (const auto& slice : outbox.pending_slices())
        for (const auto& step : ga::plan_slice(slice.intents)) {
            if (ga::slice_step_stale(world, step.intent->record, commit)) continue;
            records.push_back({step.intent->player, &step.intent->record});
        }
    gc::GalacticSession::NativeOverlayStats stats;
    auto overlay = take(world.build_native_overlay(1, records, executor, stats));
    auto control_records = records;
    // Drop the last record that really changes fleets (the tail of the stream may be journal/orbit records the overlay only counts).
    for (auto at = control_records.size(); at-- > 0;)
        if (std::holds_alternative<Shadow::FleetRequest>(control_records[at].record->request)) { control_records.erase(control_records.begin() + static_cast<std::ptrdiff_t>(at)); break; }
    gc::GalacticSession::NativeOverlayStats control_stats;
    auto control = take(world.build_native_overlay(1, control_records, executor, control_stats));
    // The really-applied world: the same slices through the ordered readmit on an identical fresh world.
    auto applied = take(gc::GalacticSession::create(native_control_fixture()));
    for (const auto& slice : outbox.pending_slices()) {
        auto intents = slice.intents;
        take(ga::commit_ordered_intents(applied, std::move(intents), executor, commit));
    }
    out.replayed = stats.replayed;
    const auto applied_snapshot = applied.snapshot();
    const auto overlay_snapshot = overlay.snapshot();
    const auto control_snapshot = control.snapshot();
    const auto expected = applied_snapshot->fleets();
    const auto got = overlay_snapshot->fleets();
    out.fleets = expected.size();
    out.fleets_equal = std::vector<gc::Fleet>(expected.begin(), expected.end()) == std::vector<gc::Fleet>(got.begin(), got.end());
    const auto wrong = control_snapshot->fleets();
    out.control_differs = std::vector<gc::Fleet>(expected.begin(), expected.end()) != std::vector<gc::Fleet>(wrong.begin(), wrong.end());
    return out;
}

void overlay_projection_equals_applied() {
    const eawr::sim::InlineExecutor serial;
    const auto run = overlay_projection(serial);
    expect(run.fleets >= 2, "T1 the applied world has fleets to compare");
    expect(run.replayed >= 2, "T1 the overlay really replayed the pending records");
    expect(run.fleets_equal, "T1 the overlaid root's fleets equal the really-applied world's");
    expect(run.control_differs, "T1 negative control: without the last record the overlay differs from the applied world");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto again = overlay_projection(executor);
        expect(again.fleets_equal && again.control_differs, "T1 holds at " + std::to_string(workers) + " workers");
    }
}

// L1 (A2.5): a lane barrier on the detached shell never sees what the live session does meanwhile. The lane script runs on a second
// thread with a gate after every step; between gates the main thread mutates the live session. The recorded intents (the canonical
// saved form of the outbox) must equal those of the same script run alone on an identically refreshed shell.
std::vector<std::uint8_t> run_lane_script(gc::GalacticSession& lane, const std::function<void()>& gate) {
    const std::array<gc::PlayerId, 2> players{1, 2};
    ga::IntentOutbox outbox(ga::standard_async_schedule);
    const auto start = outbox.begin_batch(100, {lane.next_entity_id(), lane.next_native_invasion_id()});
    gc::GalacticSession::NativeBarrierOptions options;
    options.first_id = start.id; options.first_invasion = start.invasion; options.intent_sink = outbox.sink(); options.lane = true;
    auto barrier = take(lane.begin_native_barrier(players, 64, std::move(options)));
    take(barrier->begin_players()); gate();
    outbox.begin_virtual_tick(1);
    take(barrier->select_player(1));
    (void)take(lane.admit_native_fleet(1, gc::RemoveFleetMembers{10, {20}}, false)); gate();
    (void)take(lane.admit_native_fleet(1, gc::CreateFleet{2, {20}, 0}, false)); gate();
    take(barrier->select_player(2));
    (void)take(lane.admit_native_fleet(2, gc::RemoveFleetMembers{11, {22}}, false)); gate();
    take(barrier->begin_tail()); gate();
    outbox.begin_virtual_tick(2);
    take(barrier->begin_turn());
    take(barrier->select_player(2));
    (void)take(lane.admit_native_fleet(2, gc::CreateFleet{3, {22}, 0}, false)); gate();
    take(barrier->select_player(1));
    (void)take(lane.admit_native_fleet(1, gc::RemoveFleetMembers{10, {21}}, false)); gate();
    take(barrier->discard());
    outbox.end_batch();
    return take(ga::encode_outbox_state(outbox, 0));
}

void lane_isolation() {
    auto live = take(gc::GalacticSession::create(native_control_fixture()));
    auto shell = take(gc::GalacticSession::create_lane_shell(live));
    expect(!shell.refresh_lane(shell), "a session cannot refresh itself as the lane shell");
    take(shell.refresh_lane(live));
    expect(shell.next_entity_id() == live.next_entity_id() && shell.completed_tick() == live.completed_tick(),
        "the refreshed shell reads the live frontier and tick");
    const auto reference = run_lane_script(shell, [] {});
    take(shell.refresh_lane(live));
    expect(run_lane_script(shell, [] {}) == reference, "the shell gives the same intents every time it is refreshed from the same world");

    for (int pattern = 0; pattern < 3; ++pattern) {
        // Every pattern starts from a fresh world and pattern_shell.
        auto pattern_live = take(gc::GalacticSession::create(native_control_fixture()));
        auto pattern_shell = take(gc::GalacticSession::create_lane_shell(pattern_live));
        take(pattern_shell.refresh_lane(pattern_live));
        const auto before = pattern_live.canonical_state_bytes();
        std::counting_semaphore<> lane_ready(0), main_go(0);
        std::atomic<bool> done{false};
        std::exception_ptr failure;
        std::vector<std::uint8_t> interleaved;
        std::thread lane([&] {
            try { interleaved = run_lane_script(pattern_shell, [&] { lane_ready.release(); main_go.acquire(); }); }
            catch (...) { failure = std::current_exception(); }
            done = true; lane_ready.release();
        });
        int round = 0;
        for (;;) {
            lane_ready.acquire();
            if (done) break;
            // The sim thread keeps working on the pattern_live session while the lane is paused: every mutation S shares.
            if ((round + pattern) % 3 == 0) (void)pattern_live.admit_native_fleet(1, gc::RemoveFleetMembers{10, {21}}, false);
            if ((round + pattern) % 3 == 1) (void)pattern_live.admit_native_fleet(1, gc::CreateFleet{2, {20}, 0}, false);
            if ((round + pattern) % 3 == 2) (void)pattern_live.admit_native_fleet(2, gc::RemoveFleetMembers{11, {22}}, false);
            ++round;
            main_go.release();
        }
        lane.join();
        if (failure) std::rethrow_exception(failure);
        expect(pattern_live.canonical_state_bytes() != before, "the pattern_live session really changed while the lane was paused (pattern " + std::to_string(pattern) + ")");
        expect(interleaved == reference, "the interleaved lane decides exactly what the quiescent lane decided (pattern " + std::to_string(pattern) + ")");
        // The pattern_live session stays usable: its own barrier opens and closes while the pattern_shell is idle.
        const std::array<gc::PlayerId, 2> players{1, 2};
        auto own = take(pattern_live.begin_native_barrier(players, 64));
        take(own->begin_players());
        take(own->discard());
    }
}

// An immediate Lua batch inside the lane (a magic force, a cash write) is part of the player's decisions: its force and cash
// must reach the outbox as intents, because the lane barrier is discarded and the world takes only intents. Without them the AI
// keeps an actor (and a funding change) the world never got.
void lane_immediate_effects() {
    const eawr::sim::InlineExecutor executor;
    const std::array<gc::PlayerId, 2> players{1, 2};
    auto world = take(gc::GalacticSession::create(commit_fixture()));
    ga::IntentOutbox outbox(ga::standard_async_schedule);
    outbox.bind_hooks([](gc::PlayerId, const Shadow::IntentRecord& record) {
        ga::IntentHooks hooks;
        if (ga::intent_kind(record) == ga::IntentKind::force)
            hooks.force_builder = [](const auto& view, auto first) {
                return OptionalPlacement::success(placement_at(first, view.presence.special_control));
            };
        return hooks;
    });
    const auto start = outbox.begin_batch(100, {world.next_entity_id(), world.next_native_invasion_id()});
    {
        gc::GalacticSession::NativeBarrierOptions options;
        options.first_id = start.id; options.first_invasion = start.invasion; options.intent_sink = outbox.sink(); options.lane = true;
        options.id_end = outbox.frontier().id; options.invasion_end = outbox.frontier().invasion;
        auto barrier = take(world.begin_native_barrier(players, 64, std::move(options)));
        take(barrier->begin_players());
        outbox.begin_virtual_tick(1);
        take(barrier->select_player(1));
        auto batch = take(world.begin_native_actions(64));
        static_cast<void>(take(batch.add_credits(1, f(50), f(1))));
        static_cast<void>(take(batch.create_force(force_request, [&](const auto& view, auto first) {
            return eawr::core::Result<Placement>::success(placement_at(first, view.presence.special_control));
        }, executor)));
        take(world.commit_native_actions(batch, false, true));
        take(barrier->begin_tail());
        take(barrier->discard());
    }
    outbox.end_batch();
    std::size_t forces{}, cash{};
    eawr::sim::EntityId born{}; // the actor id the lane gave the force at decision time
    for (const auto& slice : outbox.pending_slices())
        for (const auto& intent : slice.intents) {
            forces += ga::intent_kind(intent.record) == ga::IntentKind::force;
            cash += ga::intent_kind(intent.record) == ga::IntentKind::cash;
            if (ga::intent_kind(intent.record) == ga::IntentKind::force) born = intent.record.first_id;
        }
    expect(forces == 1, "the immediate magic force reaches the outbox as one force intent");
    expect(cash == 1, "the immediate cash write reaches the outbox as one cash intent");
    // The world takes them through the ordered readmit and gets the actor the AI holds.
    auto applied = take(gc::GalacticSession::create(commit_fixture()));
    const auto before = applied.snapshot()->actor_records().size();
    ga::IntentCommitOptions commit; commit.refuse_stale_identities = true;
    std::size_t admitted{};
    std::vector<eawr::sim::EntityId> made;
    for (auto& slice : outbox.take_due(1000)) {
        const auto report = take(ga::commit_ordered_intents(applied, std::move(slice.intents), executor, commit));
        admitted += report.admitted;
        for (const auto& outcome : report.outcomes)
            if (outcome.kind == ga::IntentKind::force) made.insert(made.end(), outcome.actors.begin(), outcome.actors.end());
    }
    expect(admitted >= 1 && applied.snapshot()->actor_records().size() == before + 1,
        "the readmitted force creates the actor the lane's AI holds");
    expect(born != 0 && made.size() == 1 && made.front() == born,
        "the readmitted actor has the id the lane gave it at decision time");
    const auto rows = applied.snapshot()->actor_records();
    expect(std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.id == born; }),
        "the world holds the actor under the lane's id");
}

// The lane's stage delivers what it decided; the readmission is told not to deliver it again, but a LOSS the world causes while it
// readmits (a landing consumes its fleet) was never in the stage and must still reach the retained owner.
void readmit_keeps_losses() {
    auto world = take(gc::GalacticSession::create(commit_fixture()));
    std::vector<gc::GalacticSession::NativeActorChange> seen;
    world.set_native_actor_changes_handler([&seen](const auto&, auto changes) {
        seen.insert(seen.end(), changes.begin(), changes.end());
        return eawr::core::Result<void>::success();
    });
    {
        gc::GalacticSession::PreparedNotifications prepared(world);
        const std::array<eawr::sim::EntityId, 1> destroyed{22};
        take(world.apply_battle_losses(destroyed));
    }
    std::size_t lost{}, other{};
    for (const auto& change : seen) { if (change.kind == gc::GalacticSession::NativeActorChange::Kind::lost) ++lost; else ++other; }
    expect(lost >= 1 && other == 0, "a loss caused during the readmission reaches the owner, and nothing else is delivered twice");
    expect(std::any_of(seen.begin(), seen.end(), [](const auto& change) { return change.actor == 22 && change.kind == gc::GalacticSession::NativeActorChange::Kind::lost; }),
        "the lost actor is the destroyed one");
    seen.clear();
    const std::array<eawr::sim::EntityId, 1> later{20};
    take(world.apply_battle_losses(later));
    expect(std::any_of(seen.begin(), seen.end(), [](const auto& change) { return change.actor == 20; }),
        "the owner's own handler is back once the readmission scope ends");
}

}
int case_galactic_ai_intent_commit_tests() {
    try {
        ordered_commit_matrix();
        barrier_commit();
        refusal_consequences();
        refusal_after_completion();
        outbox_slices();
        identity_block_exhaustion();
        invasion_block();
        outbox_state();
        deferred_lane();
        rewind_closes_at_counter();
        overlay_stale_fleet();
        overlay_projection_equals_applied();
        lane_isolation();
        lane_immediate_effects();
        readmit_keeps_losses();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures == 0 ? 0 : 1;
}
