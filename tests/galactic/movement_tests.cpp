#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "../../src/script/foc/galactic_ai_actions.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/fleets.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/movement.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using eawr::sim::math::Fixed;
Fixed f(std::int64_t value) { return Fixed::from_integer(value).value(); }
using eawr::testing::result_diagnostic::take;
gc::GalacticSetup setup() {
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}};
    s.seats = {{gc::SeatId{1}, 1, gc::SeatKind::human, 0, true, 1}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 1}, {3, {f(24), {}, {}}, 2}};
    s.routes = {{4, 1, 2, f(2)}};
    s.fleets = {{10, 1, 1, {{20, 1}}}, {11, 1, 1, {{21, 1}}}, {12, 2, 2, {{22, 1}}}};
    s.rules.maximum_fleet_movement_distance = f(12);
    s.rules.fiscal_cycle_frames = 5;
    s.fleet_control.emplace(); s.fleet_control->types = {{1, false, true, false}, {2, false, true, true}, {3, false, false, false}};
    s.movement.emplace();
    auto& m = *s.movement;
    m.mode_modifier = f(1); // synthetic fixture input, not an initialized retail default
    m.types = {{1, f(2), f(100), 0, true, false, false, false},
        {2, {}, f(50), 0, false, true, false, false}, {3, {}, f(10), 0, false, false, false, false}};
    m.players = {{1, f(1), f(3), false}, {2, f(1), f(3), false}};
    m.planets = {{1, f(1), false}, {2, f(1), false}, {3, f(1), false}};
    for (const auto& p : s.players) for (const auto& q : s.planets) m.access.push_back({p.player_id, q.id, true, false});
    return s;
}
gc::GalacticReplay replay() {
    gc::GalacticReplay r; r.setup = setup(); r.final_tick_count = 22;
    r.commands = {{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2, 3}, {}}},
        {{1, gc::SeatId{1}, 2}, gc::MoveFleet{10, {1, 2}, {}}},
        {{2, gc::SeatId{1}, 3}, gc::PauseGalaxy{}}, {{4, gc::SeatId{1}, 4}, gc::ResumeGalaxy{}},
        {{5, gc::SeatId{1}, 5}, gc::SetSpeed{false, f(8)}},
        {{10, gc::SeatId{1}, 6}, gc::MoveFleet{11, {1, 2}, std::uint8_t{2}}}};
    return r;
}
const gc::Fleet& fleet(const gc::GalacticSnapshot& s, gc::FleetId id = 10) {
    const auto rows = s.fleets(); const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; });
    if (found == rows.end()) throw std::runtime_error("test fleet absent");
    return *found;
}
bool event(const gc::GalacticSnapshot& s, gc::GalacticEventKind kind, gc::FleetId id = 10) {
    return std::any_of(s.events().begin(), s.events().end(), [&](const auto& e) { return e.kind == kind && e.subject == id; });
}
void staged_arrival_recheck_contracts() {
    namespace ga = eawr::script::foc::galactic_ai;
    using eawr::sim::math::Fixed;
    const auto f = [](std::int64_t value) { return Fixed::from_integer(value).value(); };
    gc::GalacticSetup setup;
    setup.players = {{1, 1, 0}, {2, 2, 0}};
    setup.seats = {{{1}, 1, gc::SeatKind::ai, 0, true, 0}, {{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    setup.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 1}};
    setup.routes = {{3, 1, 2, f(2)}};
    setup.fleets = {{10, 1, 1, {{20, 1}}}};
    setup.rules.maximum_fleet_movement_distance = f(12); setup.rules.fiscal_cycle_frames = 10000;
    setup.fleet_control.emplace(); setup.fleet_control->types = {{1, false, true, false}};
    setup.player_economy.emplace(); setup.fiscal.emplace(); setup.production.emplace(); setup.census.emplace();
    setup.production->maximum_human_queue = 1; setup.census->types = {{1, {}}};
    for (const auto& player : setup.players) {
        gc::GalacticPlayerEconomy account; account.player = player.player_id;
        setup.player_economy->players.push_back(account);
        setup.fiscal->players.push_back({player.player_id, false, f(1)});
        setup.production->players.push_back({player.player_id, false, true, false, {}, {}, {}, {}, {}});
        setup.census->players.push_back({player.player_id, 0, false, {}});
    }
    for (const auto& planet : setup.planets) {
        setup.fiscal->planets.push_back({planet.id, {}});
        gc::PlanetProduction row; row.planet = planet.id; setup.production->planets.push_back(row);
    }
    setup.census->objects = {
        {10, 1, 1, 1, 1, false, false, 1, 0, gc::ActorCombatRole::fleet, {}, {}},
        {20, 1, 1, 1, 10, false, false, 1, 0, gc::ActorCombatRole::ordinary, {}, {}}};
    setup.movement.emplace(); auto& movement = *setup.movement;
    movement.mode_modifier = f(1); movement.types = {{1, f(2), f(100), 0, true, false, false, false}};
    movement.players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}};
    movement.planets = {{1, f(1), false}, {2, f(1), false}};
    for (const auto& player : setup.players) for (const auto& planet : setup.planets)
        movement.access.push_back({player.player_id, planet.id, true, false});
    const auto bind = [](auto& actions, auto& world) {
        actions.bind_admission_gate([] { return false; });
        actions.bind_movement([](auto, const auto& snapshot) {
            ga::NativeActions::MovementInput input; input.ai_player = true;
            for (const auto& planet : snapshot.planets()) {
                gc::PathPlanet row; row.id = planet.id; row.relation = gc::PathRelation::allied;
                row.ai_classification_succeeded = false; row.ai_enemy_space_force = false;
                input.access.push_back(row); input.orbital_occupants[planet.id] = 0;
            }
            return eawr::core::Result<ga::NativeActions::MovementInput>::success(std::move(input));
        }, [] { return false; }, [&world](auto player, auto actor) { return world.recheck_native_orbit(player, actor); });
    };
    const auto mutable_world = [](eawr::campaign::GameSession& current) -> gc::GalacticSession& {
        gc::GalacticSession* world{};
        take(current.configure_native_replay_owners([&](auto& owner) {
            world = &owner; return eawr::core::Result<void>::success();
        }));
        return *world;
    };
    const std::array<gc::PlayerId, 1> players{1};
    std::string baseline;
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers), resumed_executor(workers == 8 ? 1 : workers * 2);
        auto game = take(eawr::campaign::GameSession::create(setup));
        auto oracle = take(eawr::campaign::GameSession::create(setup));
        ga::NativeActions actions(mutable_world(game), std::make_shared<eawr::script::foc::ai::GalacticFunding>(), {}, {});
        ga::NativeActions direct(mutable_world(oracle), std::make_shared<eawr::script::foc::ai::GalacticFunding>(), {}, {});
        bind(actions, mutable_world(game)); bind(direct, mutable_world(oracle));
        ga::ActionRequest request; request.kind = ga::ActionKind::move; request.player = 1; request.object = 10; request.target = 2;
        const auto admitted = take(direct.admit(request));
        auto barrier = take(mutable_world(game).begin_native_barrier(players, 16)); take(barrier->select_player(1));
        const auto staged = take(actions.admit(request));
        expect(admitted.admitted && staged.request == admitted.request && staged.admitted, "ordinary native movement departs through a real staged admission");
        take(barrier->commit()); barrier.reset();
        take(game.step(executor)); take(oracle.step(executor));
        expect(game.galaxy().snapshot()->fleets().front().motion.transit.has_value(), "checkpoint is saved in actual fleet transit");
        const auto native = take(actions.capture());
        const auto identities = eawr::checkpoint::game_identities(setup);
        const auto bytes = take(eawr::checkpoint::save_game_checkpoint(game, identities));
        auto restored = take(eawr::checkpoint::load_game_checkpoint(bytes, identities, {}, resumed_executor)).session;
        ga::NativeActions resumed(mutable_world(restored), std::make_shared<eawr::script::foc::ai::GalacticFunding>(), {}, {});
        bind(resumed, mutable_world(restored)); take(resumed.restore(native));
        bool finished{}; std::string trace;
        for (unsigned tick = 0; tick < 64 && !finished; ++tick) {
            const auto expected = take(direct.poll_movement(admitted.request));
            const auto poll = [&](auto& current, auto& native_actions) {
                const auto profiling = eawr::gc_profile::enabled.exchange(true);
                const auto index = static_cast<std::size_t>(eawr::gc_profile::Phase::make_snapshot);
                const auto before = eawr::gc_profile::snapshot()[index].calls;
                const auto committed = current.galaxy().canonical_state_bytes();
                auto pending = take(mutable_world(current).begin_native_barrier(players, 16)); take(pending->select_player(1));
                const auto result = take(native_actions.poll_movement(admitted.request));
                expect(result.finished == expected.finished && result.actor == expected.actor,
                    "staged arrival poll retains the ordinary terminal/no-hop result");
                expect(current.galaxy().canonical_state_bytes() == committed,
                    "arrival recheck leaves the committed movement owner unchanged until the barrier commit");
                expect(current.galaxy().snapshot()->canonical_bytes() == oracle.galaxy().snapshot()->canonical_bytes(),
                    "arrival recheck is visible to later reads in its own chain");
                take(pending->commit());
                const auto calls = eawr::gc_profile::snapshot()[index].calls - before;
                eawr::gc_profile::enabled.store(profiling);
                expect(calls == (result.finished ? 1U : 0U), "arrival recheck has one full snapshot per publishing barrier");
                expect(current.galaxy().canonical_state_bytes() == oracle.galaxy().canonical_state_bytes()
                    && galactic_test::record_follows(current.galaxy(), oracle.galaxy()), "staged and restored arrival state and record bytes match the unbatched oracle");
            };
            poll(game, actions); poll(restored, resumed);
            expect(take(actions.capture()).components == take(direct.capture()).components
                && take(resumed.capture()).components == take(direct.capture()).components,
                "native movement retention matches the oracle after restore on a different worker count");
            trace += game.state_sha256(); finished = expected.finished;
            if (!finished) { take(game.step(executor)); take(oracle.step(executor)); take(restored.step(resumed_executor)); }
        }
        expect(finished && game.galaxy().snapshot()->fleets().front().orbit == 2,
            "real movement arrives and executes its terminal no-hop recheck inside the barrier");
        if (baseline.empty()) baseline = trace; else expect(trace == baseline, "arrival recheck and restored continuation match on 1/2/4/8 workers");
    }
}

void native_movement_layers() {
    for (const bool packing : {false, true}) {
        std::vector<std::string> reference;
        for (const unsigned workers : {1U, 2U, 4U, 8U}) {
            auto source = setup();
            if (packing) {
                source.movement->types[0].squadron_capacity = 1;
                source.fleets[0].members = {{20, 1}, {30, 2}, {31, 2}};
                source.movement->contents = {{30, {{40, 3}}}, {31, {{41, 3}}}};
            }
            auto staged = take(gc::GalacticSession::create(source));
            auto direct = take(gc::GalacticSession::create(source));
            const auto initial = staged.snapshot(); const auto initial_hash = staged.state_sha256();
            auto lower = take(staged.begin_native_actions(1, 16));
            const auto apply = [&](gc::GalacticSession::NativeActionShadow& layer, gc::PlayerId player, const gc::CommandPayload& payload) {
                auto prepared = layer.admit_fleet(player, payload, false);
                auto applied = direct.admit_native_fleet(player, payload, false);
                expect(prepared && applied && prepared.value().admitted == applied.value().admitted
                    && prepared.value().object == applied.value().object && prepared.value().reason == applied.value().reason,
                    "native movement layer uses the live admission and refusal rules");
                expect(layer.snapshot()->canonical_bytes() == direct.snapshot()->canonical_bytes(),
                    "movement layer exposes exact staged motion, packing and event bytes before commit");
            };
            const auto remnant = lower.next_entity_id();
            apply(lower, 1, gc::MoveFleet{10, {1, 2}, {}});
            expect(lower.fleet(10) && lower.fleet(10)->motion.transit.has_value() && !fleet(*initial, 10).motion.transit,
                "staged departure is visible locally while the completed publication remains immutable");
            if (packing) expect(lower.fleet(remnant) && lower.fleet(remnant)->members == std::vector<gc::FleetMember>{{30, 2}},
                "packing stages the same remnant identity and membership as live admission");
            apply(lower, 1, gc::MoveFleet{10, {1, 2}, {}}); // a moving fleet cannot redirect
            apply(lower, 1, gc::SetFleetOrbitSlot{11, 1});
            auto higher = take(lower.for_player(2, 16));
            expect(higher.fleet(10) && higher.fleet(10)->motion.transit.has_value(),
                "higher player sees the lower player's staged departure");
            apply(higher, 2, gc::MoveFleet{12, {2, 3}, {}});
            apply(higher, 2, gc::MoveFleet{11, {1, 2}, {}}); // ownership refusal remains ordered
            expect(staged.snapshot() == initial && staged.state_sha256() == initial_hash,
                "movement decisions do not mutate the live galaxy before ordered commit");
            const std::array layers{lower, higher};
            expect(bool(staged.commit_native_actions(layers)) && staged.record() == direct.record()
                && staged.canonical_state_bytes() == direct.canonical_state_bytes(),
                "ordered movement layer commit preserves state and native replay bytes");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned tick = 0; tick < 24; ++tick) {
                auto a = staged.step(executor), b = direct.step(executor);
                expect(a && b && a.value().state_hash.get() == b.value().state_hash.get(),
                    "staged departures and remnants continue identically through arrival");
                if (!a || !b) return;
                hashes.push_back(a.value().state_hash.get());
            }
            if (reference.empty()) reference = hashes;
            else expect(reference == hashes, "native movement layers remain equal at 1/2/4/8 workers");
        }
    }
}
void departure() {
    auto s = setup(); auto& m = *s.movement;
    gc::Fleet candidate{100, 1, 1, {{101, 1}, {102, 3}}};
    auto d = gc::prepare_fleet_departure(candidate, m);
    expect(d && d.value().base_speed == f(2) && d.value().left_behind.empty(), "WGM-92 zero holds retains ineligible member");
    candidate.members = {{102, 3}}; expect(!gc::prepare_fleet_departure(candidate, m), "no eligible speed cannot depart");
    m.types[0].squadron_capacity = 1;
    candidate.members = {{101, 1}, {102, 2}, {103, 2}};
    m.contents = {{102, {{201, 3}}}, {103, {{202, 3}}}};
    d = gc::prepare_fleet_departure(candidate, m);
    expect(d && d.value().packed == std::vector<eawr::sim::EntityId>{103}
        && d.value().left_behind == std::vector<eawr::sim::EntityId>{102},
        "WGM-118 equal-cost packing consumes reverse encounter order without refusing");
    std::swap(candidate.members[1], candidate.members[2]);
    d = gc::prepare_fleet_departure(candidate, m);
    expect(d && d.value().packed == std::vector<eawr::sim::EntityId>{102},
        "equal-cost packing follows reversed membership, not identity order");
    std::swap(candidate.members[1], candidate.members[2]);
    m.types.push_back({4, {}, f(60), 0, false, true, false, false}); candidate.members[2].type = 4;
    d = gc::prepare_fleet_departure(candidate, m);
    expect(d && d.value().packed == std::vector<eawr::sim::EntityId>{103} && d.value().left_behind == std::vector<eawr::sim::EntityId>{102}, "descending-cost nonhyperspace packing and identity remnant");
    m.types[0].squadron_capacity = 0;
    d = gc::prepare_fleet_departure(candidate, m);
    expect(d && d.value().packed.empty() && d.value().left_behind.empty(), "zero holds bypasses entire remnant block");
    m.types[0].squadron_capacity = 1;
    m.types.push_back({5, f(1), f(10), 0, true, false, false, false});
    m.types[1].hyperspace_eligible = true; m.types[3].hyperspace_eligible = true;
    m.types[1].hyperspace_speed = f(1); m.types[3].hyperspace_speed = f(2);
    m.contents = {{102, {{201, 5}}}, {103, {{202, 1}}}};
    d = gc::prepare_fleet_departure(candidate, m);
    expect(d && d.value().packed == std::vector<eawr::sim::EntityId>{102} && d.value().base_speed == f(2), "eligible squadrons packed slowest first, packed container omitted from minimum");
    m.types[1].squadron_capacity = 9; m.types[0].squadron_capacity = 0;
    d = gc::prepare_fleet_departure(candidate, m);
    expect(d && d.value().packed.empty() && d.value().base_speed == f(1), "WGM-87 nested container's own holds never count");
    m.players[0].hyperspace_factor = Fixed::from_ratio(1, 2).value(); m.mode_modifier = f(3);
    expect(gc::prepare_fleet_departure(candidate, m).value().base_speed == Fixed::from_ratio(3, 2).value(), "minimum times authored faction and mode factors");
}
void mixed_squadron_packing() {
    auto s = setup(); auto& m = *s.movement;
    m.types[0].squadron_capacity = 1;
    // Squadron 30 has a fast first fighter and a slow second fighter, but a
    // faster template than squadron 31. Template eligibility deliberately differs.
    m.types[1].hyperspace_speed = f(9);
    m.types.push_back({4, f(3), f(60), 0, false, true, false, false});
    m.types.push_back({5, f(8), {}, 0, true, false, false, false});
    m.types.push_back({6, f(1), {}, 0, true, false, false, false});
    s.fleet_control->types.push_back({4, false, true, true});
    s.fleet_control->types.push_back({5, false, true, false});
    s.fleet_control->types.push_back({6, false, true, false});
    s.fleets[0].members = {{20, 1}, {30, 2}, {31, 4}};
    m.contents = {{30, {{40, 5}, {41, 6}}}, {31, {{42, 5}}}};
    const auto prepared = gc::prepare_fleet_departure(s.fleets[0], m);
    expect(prepared && prepared.value().packed == std::vector<eawr::sim::EntityId>{31}
        && prepared.value().left_behind.empty() && prepared.value().base_speed == f(1),
        "WGM-118/151 first fighter classifies, template speed ranks, unpacked craft set fleet speed");
    auto nonhyperspace = m;
    nonhyperspace.types[1].hyperspace_eligible = true;
    nonhyperspace.contents[0].children = {{40, 3}, {41, 6}};
    const auto classified = gc::prepare_fleet_departure(s.fleets[0], nonhyperspace);
    expect(classified && classified.value().packed == std::vector<eawr::sim::EntityId>{30},
        "first ineligible fighter enters cost packing despite eligible template and later fighter");
    nonhyperspace.contents[0].children.clear();
    expect(!gc::prepare_fleet_departure(s.fleets[0], nonhyperspace), "empty squadron refuses without a classification fallback");
    std::optional<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = eawr::campaign::GameSession::create(s).value();
        expect(bool(game.submit({{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, {}}})), "mixed squadron departure submits");
        const auto tick = game.step(executor).value();
        expect(fleet(*tick.galaxy).motion.transit && fleet(*tick.galaxy).motion.transit->packed == std::vector<eawr::sim::EntityId>{31},
            "live departure retains template-ranked packing");
        if (!reference) reference = tick.state_sha256;
        else expect(tick.state_sha256 == *reference, "mixed squadron packing equals on 1/2/4/8 workers");
    }
}
void move_after_merge() {
    auto s = setup();
    // Two live squadron fleets join a ship fleet. The template's ability flag
    // is false; WGM-83/101 reads the current surviving craft instead.
    s.fleet_control->types[1].can_hyperspace_without_ability = false;
    s.fleets[0].members = {{20, 1}, {30, 2}};
    s.fleets[1].members = {{31, 2}, {32, 2}};
    s.movement->types[1].hyperspace_speed = f(2);
    s.movement->contents = {{30, {{40, 1}}}, {31, {{41, 1}}}, {32, {{42, 1}}}};
    for (const bool destination_slot : {false, true}) for (const std::uint32_t holds : {0U, 1U}) {
        s.movement->types[0].squadron_capacity = holds;
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto game = eawr::campaign::GameSession::create(s).value();
            std::vector<std::string> hashes;
            const auto step = [&] {
                const auto tick = game.step(executor).value();
                hashes.push_back(tick.state_sha256 + ":" + tick.galaxy->sha256());
                expect(std::none_of(tick.galaxy->events().begin(), tick.galaxy->events().end(), [](const auto& row) {
                    return row.kind == gc::GalacticEventKind::command_rejected;
                }), "merged live-squadron fleet commands are not refused");
                return tick.galaxy;
            };
            expect(bool(game.submit({{0, {1}, 1}, gc::MergeFleets{11, 10}})), "merge before movement submits");
            auto snapshot = step();
            expect(std::none_of(snapshot->fleets().begin(), snapshot->fleets().end(), [](const auto& row) { return row.id == 11; })
                && fleet(*snapshot).members == std::vector<gc::FleetMember>{{20, 1}, {30, 2}, {32, 2}, {31, 2}},
                "merge destroys only the source shell and preserves live squadron identities");
            expect(bool(game.submit({{1, {1}, 2}, gc::SetFleetOrbitSlot{10, 2}})), "merged slot change submits");
            snapshot = step();
            expect(fleet(*snapshot).orbit_slot == 2 && event(*snapshot, gc::GalacticEventKind::fleet_slot_changed),
                "merged fighter fleet moves to an empty orbit slot");
            const auto ids = eawr::checkpoint::game_identities(s);
            const auto saved = eawr::checkpoint::save_game_checkpoint(game, ids).value();
            game = eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor).value().session;
            expect(bool(game.submit({{2, {1}, 3}, gc::MoveFleet{10, {1, 2},
                destination_slot ? std::optional{std::uint8_t{1}} : std::nullopt}})), "merged planet move submits");
            snapshot = step();
            expect(fleet(*snapshot).motion.transit && event(*snapshot, gc::GalacticEventKind::movement_begin),
                "merged fleet departs by its surviving identity after restore");
            if (holds && fleet(*snapshot).motion.transit)
                expect(fleet(*snapshot).motion.transit->packed == std::vector<eawr::sim::EntityId>{30},
                    "equal-speed scarce holds pack the first encountered squadron after a merge");
            for (unsigned tick = 0; tick < 4; ++tick) snapshot = step();
            expect(fleet(*snapshot).orbit == 2 && !fleet(*snapshot).motion.transit
                && fleet(*snapshot).members.size() == 4, "merged fleet and its squadrons arrive on the other planet");
            if (destination_slot) expect(fleet(*snapshot).orbit_slot == 1, "merged fleet consumes the requested arrival slot");
            if (workers == 1) reference = hashes;
            else expect(reference == hashes, "merge, slot change, restore and planet movement equal on 1/2/4/8 workers");
        }
    }
}
void roundtrip_and_parity() {
    const auto r = replay(); const auto bytes = gc::write_replay(r).value();
    expect(gc::parse_replay(bytes).value() == r, "movement inputs and command replay roundtrip");
    for (std::size_t size = 0; size < bytes.size(); size += 17) expect(!gc::parse_replay(std::span(bytes).first(size)), "truncated movement replay refuses");
    auto invalid = r.setup; invalid.movement->access.pop_back(); expect(!gc::validate_setup(invalid), "complete movement access required");
    invalid = r.setup; invalid.movement->players[0].hyperspace_factor = f(-1);
    expect(bool(gc::validate_setup(invalid)) && invalid.movement->players[0].hyperspace_factor == f(-1), "negative authored factor loads unchanged; departure decides whether it can move");
    invalid = r.setup; invalid.movement->types[0].hyperspace_speed = f(-1);
    expect(bool(gc::validate_setup(invalid)) && invalid.movement->types[0].hyperspace_speed == f(-1), "negative authored speed loads unchanged; departure decides whether it can move");
    invalid = r.setup; invalid.movement->contents = {{20, {{21, 1}}}}; expect(!gc::validate_setup(invalid), "child identity collision refused");
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = eawr::campaign::GameSession::create(r.setup).value();
        std::vector<std::string> hashes; std::size_t command = 0;
        for (std::uint64_t tick = 0; tick < r.final_tick_count; ++tick) {
            while (command < r.commands.size() && r.commands[command].key.tick == tick) expect(bool(game.submit(r.commands[command++])), "movement submit");
            const auto step = game.step(executor).value(); hashes.push_back(step.state_hash.get() + ":" + step.galaxy->sha256());
            expect(game.galaxy().state_sha256() == game.galaxy().cached_state_hash().get(), "cached movement section hash equals full encoding");
            expect(game.state_sha256() == step.state_hash.get(), "campaign movement digest includes the mode timeline");
            if (tick == 0) {
                expect(fleet(*step.galaxy).orbit == 0 && fleet(*step.galaxy).motion.position.x == f(1), "departure unlinks and installs source bounds offset");
                expect(event(*step.galaxy, gc::GalacticEventKind::movement_begin), "movement begins before first facing-only frame");
                expect(fleet(*step.galaxy).motion.transit->speed == f(4), "route speed multiplier applies to per-frame speed");
            }
            if (tick == 1) expect(event(*step.galaxy, gc::GalacticEventKind::movement_canceled) && fleet(*step.galaxy).motion.position.x == f(13), "enroute redirect blocked; intermediate arrival continues");
            if (tick == 2 || tick == 3) expect(fleet(*step.galaxy).motion.position.x == f(13), "nested clock pause holds in-transit position");
            if (tick == 4) expect(fleet(*step.galaxy).motion.transit->hop == 1 && fleet(*step.galaxy).motion.position.x == f(15), "intermediate arrival continues with next endpoint offset");
            if (tick == 5) expect(fleet(*step.galaxy).motion.transit->speed == f(2), "ordinary strategic speed changes pacing, not travel displacement");
            if (tick == 7) expect(fleet(*step.galaxy).orbit == 3 && !fleet(*step.galaxy).motion.transit && event(*step.galaxy, gc::GalacticEventKind::movement_finished), "final arrival clears transit and emits finished");
            if (tick == 7) expect(fleet(*step.galaxy).motion.position == eawr::sim::math::Vec3{f(42), f(10), {}}
                && fleet(*step.galaxy).motion.facing == eawr::sim::math::Vec3{{}, f(1), {}},
                "WGM-32/153 lone non-conflicting final arrival uses the same orbit offset and facing");
            if (tick == 1 || tick == 3 || tick == 4) {
                const auto ids = eawr::checkpoint::game_identities(r.setup);
                const auto saved = eawr::checkpoint::save_game_checkpoint(game, ids).value();
                auto restored = eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor).value().session;
                expect(restored.state_sha256() == game.state_sha256() && restored.galaxy().snapshot()->canonical_bytes() == game.galaxy().snapshot()->canonical_bytes(), "checkpoint retains transit path, hop, speed, facing, pause and events");
                game = std::move(restored);
            }
            if (tick == 5) game.scramble_storage_for_testing();
        }
        if (workers == 1) reference = hashes; else expect(reference == hashes, "movement/state/events/snapshots equal on 1/2/4/8 workers");
        expect(fleet(*game.galaxy().snapshot(), 11).orbit_slot == 2, "arrival consumes explicit destination slot");
    }
}
gc::GalacticSnapshot run(gc::GalacticSetup s, std::vector<gc::GameCommand> commands, std::uint64_t ticks) {
    auto game = gc::GalacticSession::create(s).value(); const eawr::sim::InlineExecutor executor;
    for (const auto& c : commands) expect(bool(game.submit(c)), "scenario command submits");
    for (std::uint64_t i = 0; i < ticks; ++i) expect(bool(game.step(executor)), "scenario movement step");
    return *game.snapshot();
}
void arrival() {
    const gc::GameCommand invalid_slot{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, std::uint8_t{3}}};
    auto refused_slot = eawr::campaign::GameSession::create(setup()).value();
    expect(!refused_slot.submit(invalid_slot), "in-process invalid destination slot refuses at input validation");
    const eawr::sim::InlineExecutor executor;
    expect(bool(refused_slot.step(executor)), "invalid input leaves session healthy");
    const auto ids = eawr::checkpoint::game_identities(setup());
    const auto saved_slot = eawr::checkpoint::save_game_checkpoint(refused_slot, ids).value();
    expect(bool(eawr::checkpoint::load_game_checkpoint(saved_slot, ids, {}, executor)),
        "refused destination slot never enters checkpoint history");
    const gc::GameCommand retreat{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 1}, {}}};
    expect(gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::move_fleet, gc::encode_payload(retreat.payload)).has_value(), "WGM-99 source/source fallback survives command decoding");
    const auto canceled = run(setup(), {retreat}, 1);
    expect(fleet(canceled).orbit == 1 && !fleet(canceled).motion.transit && event(canceled, gc::GalacticEventKind::movement_canceled), "source/source fallback cancels departure without unlinking");
    const gc::GameCommand move{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2, 3}, {}}};
    auto s = setup(); s.movement->access[2].accessible = false;
    auto snap = run(s, {move}, 6);
    expect(event(snap, gc::GalacticEventKind::movement_bounced) && fleet(snap).orbit == 0 && fleet(snap).motion.transit->path == std::vector<gc::PlanetId>{3, 2}, "inaccessible final bounces to prior planet, without linking");
    snap = run(s, {move}, 13); expect(fleet(snap).orbit == 2 && !fleet(snap).motion.transit, "bounced travel arrives at previous planet");
    s = setup(); s.movement->access[1].accessible = false;
    snap = run(s, {move}, 3); expect(fleet(snap).orbit == 0 && fleet(snap).motion.transit->hop == 1, "inaccessible intermediate skipped without orbit linkage");
    s = setup(); s.movement->access[1].potential_conflict = true;
    snap = run(s, {move}, 2); expect(fleet(snap).orbit == 2 && !fleet(snap).motion.transit && event(snap, gc::GalacticEventKind::movement_conflict), "intermediate conflict stops and publishes admission seam");
    {
        // GST-U01 / WGM-32/104/163: a stealth-only fleet (every member skips combat) neither stops at nor fights in a
        // hostile intermediate hop, and still arrives at its endpoint.
        auto stealth = s; stealth.movement->types[0].skips_combat = true;
        const auto passing = run(stealth, {move}, 3);
        expect(fleet(passing).orbit == 0 && fleet(passing).motion.transit->hop == 1
            && !event(passing, gc::GalacticEventKind::movement_conflict), "stealth-only fleet passes a hostile intermediate hop without a conflict");
        const auto arrived = run(stealth, {move}, 30);
        expect(fleet(arrived).orbit == 3 && !fleet(arrived).motion.transit, "stealth-only fleet reaches its endpoint through the hostile hop");
    }
    s.movement->players[0].benefits_from_corruption = true;
    snap = run(s, {move}, 3); expect(fleet(snap).orbit == 0 && fleet(snap).motion.transit->hop == 1, "corruption faction suppresses intermediate conflict without testing endpoint corruption");
    s.movement->access[2].potential_conflict = true;
    snap = run(s, {move}, 6); expect(fleet(snap).orbit == 3 && event(snap, gc::GalacticEventKind::movement_conflict), "final conflict never uses intermediate corruption suppression");
    s = setup(); s.movement->planets[0].corrupted = true; s.movement->planets[1].corrupted = true;
    s.movement->players[0].benefits_from_corruption = true;
    snap = run(s, {move}, 1); expect(fleet(snap).motion.transit->speed == f(6), "corruption/route speed bonuses use maximum, never product");
    s = setup(); s.movement->access[2].potential_conflict = true;
    s.fleets[0].orbit = 2;
    s.fleets[1].motion.facing = {f(1), {}, {}};
    const gc::GameCommand first{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {2, 3}, {}}};
    const gc::GameCommand far{{0, gc::SeatId{1}, 2}, gc::MoveFleet{11, {1, 2, 3}, {}}};
    const gc::GameCommand defender{{0, gc::SeatId{2}, 1}, gc::MoveFleet{12, {2, 3}, {}}};
    snap = run(s, {first, far, defender}, 5);
    expect(fleet(snap).orbit == 3 && fleet(snap, 11).orbit == 3 && fleet(snap, 12).orbit == 3, "conflict gathering matches final/current hop and attacker/planet owner without distance threshold");
    expect(fleet(snap, 11).motion.position.x == f(42) && fleet(snap, 12).motion.position.x == f(42),
        "WGM-153 gathered fleets use planet-relative orbit position instead of hop endpoints");
    s = setup(); s.planets[1].owner = 2; s.movement->access[1].potential_conflict = true;
    s.fleets[0].motion.facing = {f(1), {}, {}};
    snap = run(s, {{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, {}}},
        {{0, gc::SeatId{1}, 2}, gc::MoveFleet{11, {1, 2}, {}}}}, 1);
    expect(fleet(snap, 11).orbit == 2 && !fleet(snap, 11).motion.transit
        && fleet(snap, 11).motion.position.x == f(30), "WGM-36 gather links other inbound fleet at planet-2 offset");
    s = setup(); s.fleets[1].orbit = 2; s.fleets[1].orbit_slot = 2;
    const gc::GameCommand slot{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, std::uint8_t{2}}};
    snap = run(s, {slot}, 3); expect(snap.fleets().size() == 2 && fleet(snap, 11).members.size() == 2, "explicit nonzero slot merges into first equal owner fleet");
    s.fleets[1].owner = 2;
    snap = run(s, {slot}, 3); expect(snap.fleets().size() == 3 && fleet(snap).orbit == 2, "slot merge refuses different owner identity");
    s = setup(); s.fleets[1].orbit = 2;
    snap = run(s, {{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, std::uint8_t{0}}}}, 3);
    expect(snap.fleets().size() == 3, "explicit slot zero never selects automatic merge target");
    s = setup(); s.fleets[1].orbit = 2;
    snap = run(s, {{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, {}}}}, 2);
    expect(fleet(snap).orbit_slot == 1, "default chooser avoids equal-owner occupant in previous slot");
    s.fleets[1].owner = 2;
    snap = run(s, {{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, {}}}}, 2);
    expect(fleet(snap).orbit_slot == 0, "different owner does not block default slot preference");
    s = setup(); s.movement->types[0].squadron_capacity = 1;
    s.movement->types.push_back({4, {}, f(60), 0, false, true, false, false});
    s.fleet_control->types.push_back({4, false, true, true});
    s.fleets[0].members = {{20, 1}, {23, 2}, {24, 4}};
    s.movement->contents = {{23, {{100, 3}}}, {24, {{101, 3}}}};
    snap = run(s, {{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, {}}}}, 1);
    expect(fleet(snap).members == std::vector<gc::FleetMember>{{20, 1}, {24, 4}}
        && fleet(snap, 102).members == std::vector<gc::FleetMember>{{23, 2}}
        && fleet(snap, 102).orbit == 1 && fleet(snap).motion.transit->packed == std::vector<eawr::sim::EntityId>{24},
        "departure commits packed and left-behind existing identities; allocator covers live children");
    // Strict threshold: after first displacement remainder equals 2*speed, so no snap.
    s = setup(); s.routes.clear(); s.planets[1].position.x = f(8);
    snap = run(s, {{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, {}}}}, 2);
    expect(fleet(snap).orbit == 0 && fleet(snap).motion.position.x == f(3), "arrival comparison is strict less-than, not less-or-equal");
    snap = run(s, {{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, {}}}}, 3);
    expect(fleet(snap).orbit == 2, "strict threshold arrives on next displacement");
}
void orbit_placement_and_recheck() {
    std::vector<std::string> reference; std::optional<std::string> native_reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto s = setup();
        for (auto& planet : s.planets) { planet.position.y = f(7); planet.position.z = f(9); }
        s.movement->planets[1].bounds_half_z = f(3); s.movement->planets[2].bounds_half_z = f(4);
        s.movement->access[2].potential_conflict = true;
        s.fleets[0].orbit = 2;
        s.fleets[1].motion.facing = {f(1), {}, {}};
        auto game = take(eawr::campaign::GameSession::create(s));
        expect(bool(game.submit({{0, {1}, 1}, gc::MoveFleet{10, {2, 3}, {}}})), "distinct-endpoint arrival submits");
        expect(bool(game.submit({{0, {1}, 2}, gc::MoveFleet{11, {1, 2, 3}, {}}})), "gathered fleet earlier hop submits");
        std::vector<std::string> hashes;
        auto tick = take(game.step(executor)); hashes.push_back(tick.state_sha256);
        expect(fleet(*tick.galaxy, 11).motion.transit && fleet(*tick.galaxy, 11).motion.transit->desired.x == f(20),
            "gather candidate advances from distinct earlier endpoint to its matching final hop");
        tick = take(game.step(executor)); hashes.push_back(tick.state_sha256);
        const eawr::sim::math::Vec3 position{f(42), f(17), f(9)}, facing{{}, f(1), {}};
        for (const auto id : {10U, 11U}) {
            const auto& actor = fleet(*tick.galaxy, id);
            expect(actor.orbit == 3 && actor.motion.position == position && actor.motion.facing == facing
                && !actor.motion.transit, "WGM-153 orbit position, facing and cleared velocity/path differ from planet and hop endpoints");
            expect(event(*tick.galaxy, gc::GalacticEventKind::movement_finished, id), "gathered movement finishes once");
        }
        const auto ids = eawr::checkpoint::game_identities(s);
        const auto saved = take(eawr::checkpoint::save_game_checkpoint(game, ids));
        auto restored = take(eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor)).session;
        expect(restored.state_sha256() == game.state_sha256()
            && fleet(*restored.galaxy().snapshot(), 11).motion == fleet(*game.galaxy().snapshot(), 11).motion,
            "checkpoint retains gathered fleet position/facing and absent transit");
        game = std::move(restored);
        tick = take(game.step(executor)); hashes.push_back(tick.state_sha256);
        expect(fleet(*tick.galaxy, 11).motion.position == position, "discarded gathered staging cannot restore old lane motion");
        if (reference.empty()) reference = hashes; else expect(reference == hashes, "gathered arrival and restore equal on 1/2/4/8 workers");

        s = setup(); s.seats.push_back({{99}, 1, gc::SeatKind::script, 0, true, 0});
        s.fleets[0].orbit = 3; s.fleets[0].motion.position = {f(100), {}, {}};
        s.fleets[1].orbit = 2;
        s.players.push_back({3, 3, 0}); s.seats.push_back({{3}, 3, gc::SeatKind::ai, 2, true, 0});
        s.planets.push_back({4, {f(36), {}, {}}, 3}); s.routes.clear();
        std::sort(s.seats.begin(), s.seats.end(), [](const auto& a, const auto& b) { return a.seat_id < b.seat_id; });
        s.movement->players.push_back({3, f(1), f(1), false}); s.movement->planets.push_back({4, f(1), false});
        s.movement->access.clear();
        for (const auto& player : s.players) for (const auto& planet : s.planets)
            s.movement->access.push_back({player.player_id, planet.id, true, planet.id == 3});
        s.fleets.push_back({13, 3, 2, {{23, 1}}});
        s.fleets.push_back({14, 1, 2, {{24, 1}}});
        s.fleets.push_back({15, 1, 1, {{25, 1}}});
        auto world = take(gc::GalacticSession::create(s));
        expect(bool(world.submit({{0, {1}, 1}, gc::MoveFleet{11, {2, 3}, {}}})), "recheck inbound fleet submits");
        expect(bool(world.submit({{0, {2}, 1}, gc::MoveFleet{12, {2, 3}, {}}})), "defender gather candidate submits");
        expect(bool(world.submit({{0, {3}, 1}, gc::MoveFleet{13, {2, 3}, {}}})), "third-owner gather control submits");
        expect(bool(world.submit({{0, {1}, 2}, gc::MoveFleet{14, {2, 3, 4}, {}}})), "different-final gather control submits");
        expect(bool(world.submit({{0, {1}, 3}, gc::MoveFleet{15, {1, 2, 3}, {}}})), "different-current-hop gather control submits");
        expect(bool(world.step(executor)), "recheck candidate stays enroute before the forced test");
        expect(!world.recheck_native_orbit(2, 10) && !world.recheck_native_orbit(1, 11),
            "native recheck refuses wrong owner and a lane actor without planet parent");
        auto staged = take(gc::GalacticSession::from_replay(world.record()));
        (void)take(staged.step(executor));
        const auto initial = staged.snapshot(); const auto owner_bytes = staged.canonical_state_bytes();
        auto stale = take(world.begin_native_actions(1, 16));
        std::vector<gc::GalacticEvent> direct_notices, staged_notices;
        world.set_native_movement_listener([&](const auto& notice) { direct_notices.push_back(notice); });
        staged.set_native_movement_listener([&](const auto& notice) {
            staged_notices.push_back(notice);
            expect(staged.snapshot()->canonical_bytes() == initial->canonical_bytes(), "recheck listeners observe the preceding publication until relink and gather finish");
        });
        expect(bool(world.recheck_native_orbit(1, 10)), "native AI no-hop provider relinks and gathers at an existing planet");
        const auto stale_commit = world.commit_native_actions(stale);
        expect(!stale_commit && stale_commit.error().message.find("stale, unordered") != std::string::npos,
            "a genuinely replaced base snapshot is still rejected before replay");
        const auto profiling = eawr::gc_profile::enabled.exchange(true);
        const auto index = static_cast<std::size_t>(eawr::gc_profile::Phase::make_snapshot);
        const auto before_calls = eawr::gc_profile::snapshot()[index].calls;
        const std::array<gc::PlayerId, 2> players{1, 2};
        auto barrier = take(staged.begin_native_barrier(players, 16));
        expect(bool(barrier->select_player(1)), "select orbit-recheck player");
        expect(!staged.recheck_native_orbit(2, 10) && !staged.recheck_native_orbit(1, 11),
            "staged recheck preserves owner and planet-parent admission");
        expect(bool(staged.recheck_native_orbit(1, 10)), "staged orbit recheck gathers inbound attacker and defender");
        expect(staged.snapshot()->canonical_bytes() == world.snapshot()->canonical_bytes()
            && staged.canonical_state_bytes() == owner_bytes && staged_notices == direct_notices,
            "same-chain recheck reads and listener order match the oracle while the live owner stays unchanged");
        const auto lower = staged.snapshot();
        expect(bool(barrier->select_player(2)) && staged.snapshot()->canonical_bytes() == lower->canonical_bytes(),
            "later player inherits the staged recheck");
        expect(bool(barrier->commit()), "orbit recheck cannot replace a live barrier base or cause stale-base refusal");
        const auto calls = eawr::gc_profile::snapshot()[index].calls - before_calls;
        eawr::gc_profile::enabled.store(profiling);
        expect(calls == 1 && staged_notices == direct_notices,
            "orbit rechecks build one full snapshot per barrier and never redeliver movement listeners at commit");
        expect(staged.canonical_state_bytes() == world.canonical_state_bytes() && staged.record() == world.record(),
            "staged recheck committed state and record bytes equal the unbatched oracle");
        barrier.reset();
        expect(fleet(*world.snapshot(), 11).orbit == 3 && event(*world.snapshot(), gc::GalacticEventKind::movement_conflict)
            && fleet(*world.snapshot()).motion.position.x == f(42), "native and ordinary arrival share gathered orbit placement");
        expect(fleet(*world.snapshot(), 12).orbit == 3 && !fleet(*world.snapshot(), 12).motion.transit
            && fleet(*world.snapshot(), 13).motion.transit && fleet(*world.snapshot(), 14).motion.transit
            && fleet(*world.snapshot(), 15).motion.transit, "recheck gathering admits planet owner but excludes third owner, different final and different current hop");
        if (!native_reference) native_reference = world.state_sha256();
        else expect(*native_reference == world.state_sha256(), "native recheck/filter state equal on 1/2/4/8 workers");
        auto replayed = take(gc::GalacticSession::from_replay(world.record()));
        expect(bool(replayed.step(executor)) && bool(replayed.replay_native_barrier(executor)), "native orbit journal reconstructs its barrier");
        expect(replayed.state_sha256() == world.state_sha256(), "native gather journal replay matches the world");

        // Planet dispatch selects orbit-list order, not fleet identity. A negative
        // predicate still relinks but does not finish a matching inbound candidate.
        s = setup(); s.seats.push_back({{99}, 1, gc::SeatKind::script, 0, true, 0});
        s.fleets[0].orbit = 3; s.fleets[1].orbit = 2;
        s.fleets[2].orbit = 3;
        s.movement->access[5].potential_conflict = false;
        world = take(gc::GalacticSession::create(s));
        expect(bool(world.recheck_native_orbit(1, 10)), "relink first fleet behind later orbit entry without injecting runtime order into setup");
        expect(bool(world.submit({{0, {1}, 1}, gc::MoveFleet{11, {2, 3}, {}}})), "negative recheck candidate submits");
        expect(bool(world.submit({{0, {99}, 1}, gc::RecheckOrbit{3}})), "planet recheck submits through script seat");
        expect(bool(world.step(executor)), "planet recheck executes in the command phase");
        expect(fleet(*world.snapshot(), 12).motion.position.x == f(42) && fleet(*world.snapshot(), 11).motion.transit
            && !event(*world.snapshot(), gc::GalacticEventKind::movement_conflict, 12),
            "first orbit entry rechecks and negative result does not gather inbound fleets");
        expect(bool(world.submit({{1, {1}, 2}, gc::RecheckOrbit{10}})), "human recheck structurally submits");
        expect(bool(world.step(executor)) && event(*world.snapshot(), gc::GalacticEventKind::command_rejected), "orbit recheck command is script-seat only");
        expect(bool(world.submit({{2, {99}, 2}, gc::RecheckOrbit{11}})), "lane recheck submits");
        expect(bool(world.step(executor)) && fleet(*world.snapshot(), 11).motion.transit
            && !event(*world.snapshot(), gc::GalacticEventKind::movement_conflict, 11), "lane recheck cannot invent an encounter point");
        expect(bool(world.submit({{3, {99}, 3}, gc::RecheckOrbit{1}}))
            && bool(world.submit({{3, {99}, 4}, gc::RecheckOrbit{20}})), "empty orbit and fleetless member controls submit");
        expect(bool(world.step(executor)) && !event(*world.snapshot(), gc::GalacticEventKind::movement_conflict)
            && event(*world.snapshot(), gc::GalacticEventKind::command_rejected, 20), "empty orbit has no encounter and an ordinary member is not a fleet receiver");
    }
    const gc::CommandPayload command = gc::RecheckOrbit{777};
    const auto bytes = gc::encode_payload(command);
    expect(gc::layer_of(command) == gc::CommandLayer::galactic && gc::opcode_of(command) == gc::galactic_opcodes::recheck_orbit
        && gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::recheck_orbit, bytes) == command, "orbit recheck opcode roundtrips");
    expect(!gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::recheck_orbit, std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 1)), "truncated orbit request refuses");
    expect(!gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::recheck_orbit, gc::encode_payload(gc::RecheckOrbit{})), "zero orbit target refuses");
}

void transports() {
    auto s = setup();
    s.fleet_control->types.push_back({4, true, true, false});
    s.movement->types.push_back({4, f(3), {}, 1, true, false, false, true, true});
    s.fleets[0].members = {{20, 4}};
    s.movement->contents = {{20, {{100, 1}, {101, 3}}}};
    auto departure = gc::prepare_fleet_departure(s.fleets[0], *s.movement);
    expect(departure && departure.value().base_speed == f(3), "transport travels at own speed, ignoring carried troops' speed and eligibility");
    auto invalid = s; invalid.movement->types.back().fleet_container = true;
    expect(!gc::validate_setup(invalid), "transport and fleet roles are mutually exclusive");
    invalid = s; invalid.movement->contents.clear();
    expect(!gc::validate_setup(invalid), "transport requires explicit live contents, including empty list");
    // #3188: troops on a transport qualify its fleet for an explicit destination slot (WGM-83 breakdown) and for a
    // slot change; a transport carrying only ability-dependent units does not.
    const gc::GameCommand slotted{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, std::uint8_t{2}}};
    auto snap = run(s, {slotted}, 8);
    expect(fleet(snap).orbit == 2 && fleet(snap).orbit_slot == 2 && !event(snap, gc::GalacticEventKind::command_rejected),
        "loaded transport arrives in the requested destination slot");
    snap = run(s, {{{0, gc::SeatId{1}, 1}, gc::SetFleetOrbitSlot{10, 2}}}, 1);
    expect(fleet(snap).orbit_slot == 2 && !event(snap, gc::GalacticEventKind::command_rejected),
        "loaded transport changes its orbit slot");
    auto unloaded = s; unloaded.movement->contents[0].children = {{101, 3}};
    snap = run(unloaded, {slotted}, 1);
    expect(fleet(snap).orbit == 1 && !fleet(snap).motion.transit && event(snap, gc::GalacticEventKind::command_rejected),
        "transport carrying only ability-dependent units refuses an explicit destination slot");
    s.movement->access[1].potential_conflict = true;
    const gc::GameCommand move{{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, {}}};
    snap = run(s, {move}, 2);
    expect(event(snap, gc::GalacticEventKind::movement_conflict), "transport's own skip flag cannot hide ordinary carried combatants");
    s.movement->types[0].skips_combat = true; s.movement->types[2].skips_combat = true;
    snap = run(s, {move}, 2);
    expect(!event(snap, gc::GalacticEventKind::movement_conflict), "all skipping carried troops suppress transport conflict");
    s.movement->contents[0].children.clear();
    s.fleets[0].members.push_back({102, 1});
    snap = run(s, {move}, 2);
    expect(event(snap, gc::GalacticEventKind::movement_conflict), "empty transport does not skip combat");
    // Tick-zero checkpoint and replay continuation retain both codec forms.
    for (const bool with_transport : {false, true}) {
        gc::GalacticReplay r; r.setup = with_transport ? s : setup(); r.final_tick_count = 10; r.commands = {move};
        const auto bytes = gc::write_replay(r).value();
        expect(gc::parse_replay(bytes).value() == r, "movement replay round-trip with and without transport role tail");
        if (with_transport) {
            // Find the length-delimited movement payload in the public replay envelope.
            const auto integer = [&](std::size_t offset, std::size_t width) {
                std::uint32_t value{};
                for (std::size_t i = 0; i < width; ++i) value |= std::uint32_t{bytes.at(offset + i)} << (8 * i);
                return value;
            };
            std::size_t offset = 100;
            for (std::uint32_t i = 0; i < integer(96, 4); ++i) {
                const auto tag = integer(offset, 2), size = integer(offset + 2, 4);
                if (tag == 4) {
                    auto unordered = bytes;
                    // Mode modifier and type count precede the fixed-width type rows.
                    const auto first_type = offset + 6 + 12;
                    for (std::size_t byte = 0; byte < 8; ++byte) {
                        unordered[first_type + byte] = bytes[first_type + 32 + byte];
                        unordered[first_type + 32 + byte] = bytes[first_type + byte];
                    }
                    expect(!gc::parse_replay(unordered), "unordered movement types refuse before transport-tail lookup");
                    const auto tail = offset + 6 + size - 14; // one transport ID, then absent raid policy
                    expect(bytes[tail] == 1 && integer(tail + 1, 4) == 1 && bytes[offset + 6 + size - 1] == 0,
                        "located the movement transport table before the raid-policy flag");
                    auto corrupt = bytes; corrupt[tail] = 2;
                    expect(!gc::parse_replay(corrupt), "unknown movement transport-tail version refuses");
                    corrupt = bytes; corrupt[tail + 1] = 0;
                    expect(!gc::parse_replay(corrupt), "transport count omitting the encoded ID refuses");
                    corrupt = bytes; corrupt[tail + 5] = 99;
                    expect(!gc::parse_replay(corrupt), "unknown movement transport type refuses");
                    corrupt = bytes; corrupt[offset + 6 + size - 1] = 2;
                    expect(!gc::parse_replay(corrupt), "non-boolean movement raid-policy flag refuses");
                }
                offset += 6 + size;
            }
        }
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto game = eawr::campaign::GameSession::create(r.setup).value();
            const auto ids = eawr::checkpoint::game_identities(r.setup);
            const auto saved = eawr::checkpoint::save_game_checkpoint(game, ids).value();
            auto restored = eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor).value().session;
            expect(restored.state_sha256() == game.state_sha256(), "tick-zero checkpoint retains transport roles and live contents");
            expect(bool(game.submit(move)) && bool(restored.submit(move)), "restored transport move submits");
            std::vector<std::string> hashes;
            for (std::uint64_t tick = 0; tick < r.final_tick_count; ++tick) {
                hashes.push_back(game.step(executor).value().state_hash.get());
                expect(restored.step(executor).value().state_hash.get() == hashes.back(), "transport checkpoint continuation matches uninterrupted run");
            }
            if (workers == 1) reference = hashes;
            else expect(reference == hashes, "transport roles and contents retain 1/2/4/8 worker equality");
        }
    }
}
void generate(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto r = replay(); auto game = eawr::campaign::GameSession::create(r.setup).value();
    for (const auto& c : r.commands) if (!game.submit(c)) throw std::runtime_error("generator submit");
    std::ofstream hashes(directory / "fleet-travel.hashes.csv", std::ios::binary); hashes << "tick,sha256\n";
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t i = 0; i < r.final_tick_count; ++i) { const auto step = game.step(executor).value(); hashes << step.completed_tick << ',' << step.state_hash.get() << '\n'; }
    const auto bytes = gc::write_replay(game.record()).value();
    std::ofstream out(directory / "fleet-travel.eawr-gcreplay", std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out || !hashes) throw std::runtime_error("generator write");
}
void adapter() {
    eawr::units::GalacticTypeTable types;
    eawr::units::GalacticType ship; ship.name = "Ship"; ship.file = "units.xml"; ship.line = 9;
    ship.hyperspace_speed = f(7); ship.squadron_capacity = 3;
    eawr::units::GalacticType squadron; squadron.name = "Squadron"; squadron.build_cost_credits = f(100); squadron.hyperspace_speed = f(9);
    types.types = {ship, squadron};
    eawr::data::galactic::GalacticData data;
    data.constants = {{"Object_Max_Speed_Multiplier_Galactic", "", f(5)}, {"Corruption_Hyperspace_Bonus", "", f(3)}};
    eawr::data::galactic::Faction faction; faction.name = "Faction";
    faction.hyperspace_speed_factor = Fixed::from_ratio(1, 10).value(); faction.benefits_from_corruption = true;
    data.factions = {faction};
    const std::vector<eawr::campaign::MovementTypeBinding> bindings{{1, "ship", true, false, false, false}, {2, "Squadron", false, true, false, false}};
    const std::vector<eawr::campaign::MovementPlayerBinding> players{{1, "faction"}};
    auto built = eawr::campaign::build_movement_inputs(types, data, bindings, players);
    expect(built && built.value().base_mode_modifier == f(5) && built.value().types[0].hyperspace_speed == f(7)
        && built.value().types[0].squadron_capacity == 3 && built.value().types[1].build_cost == f(100)
        && built.value().types[1].hyperspace_speed == f(9)
        && built.value().players[0].corruption_bonus == f(3), "authored original-type/faction/initialized-mode inputs, no copied stock values");
    auto signed_types = types;
    signed_types.types[0].hyperspace_speed = f(-7);
    signed_types.types[1].build_cost_credits = f(-1);
    auto signed_data = data;
    signed_data.constants[0].number = f(-5);
    signed_data.constants[1].number = f(-3);
    signed_data.factions[0].hyperspace_speed_factor = f(-2);
    const auto signed_inputs = eawr::campaign::build_movement_inputs(signed_types, signed_data, bindings, players);
    expect(signed_inputs && signed_inputs.value().types[0].hyperspace_speed == f(-7)
        && signed_inputs.value().types[1].build_cost == f(-1) && signed_inputs.value().base_mode_modifier == f(-5)
        && signed_inputs.value().players[0].hyperspace_factor == f(-2)
        && signed_inputs.value().players[0].corruption_bonus == f(-3), "all signed movement authoring reaches campaign inputs unchanged");
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto config = setup();
        config.rules.maximum_fleet_movement_distance = f(-1);
        config.movement->mode_modifier = f(-1);
        config.movement->types[0].hyperspace_speed = f(-2);
        config.movement->types[1].build_cost = f(-1);
        config.movement->players[0].hyperspace_factor = f(-1);
        config.movement->players[0].corruption_bonus = f(-3);
        auto game = gc::GalacticSession::create(config).value();
        expect(bool(game.submit({{0, gc::SeatId{1}, 1}, gc::MoveFleet{10, {1, 2}, {}}})), "signed speed move reaches command admission");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(bool(game.step(executor)), "signed speed campaign steps without a negative-duration or speed failure");
        expect(!fleet(*game.snapshot()).motion.transit, "nonpositive movement factors reject departure at the consumer");
    }
    auto omitted = data; omitted.constants.erase(omitted.constants.begin());
    expect(!eawr::campaign::build_movement_inputs(types, omitted, bindings, players), "missing initialized mode tag never defaults to one");
    types.types[0].squadron_capacity.reset(); built = eawr::campaign::build_movement_inputs(types, data, bindings, players);
    expect(built && built.value().types[0].squadron_capacity == 0, "WGS-92 omitted capacity uses the shared zero default");
    types.types[1].build_cost_credits.reset();
    built = eawr::campaign::build_movement_inputs(types, data, bindings, players);
    expect(built && built.value().types[1].build_cost == Fixed{} && !types.types[1].build_cost_credits,
        "WGEP-92 squadron omission consumes zero without changing authored cost");
    auto transport_binding = bindings; transport_binding[0].transport = true;
    built = eawr::campaign::build_movement_inputs(types, data, transport_binding, players);
    expect(built && built.value().types[0].transport && built.value().types[0].hyperspace_speed == f(7),
        "explicit transport binding retains its own authored travel quantities");
}
void installed() {
    std::optional<std::string> root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) { eawr::testing::skip_installed("SKIPPED: stock movement inputs need EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}}) {
        auto mount = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(bool(mount), "stock movement manifest mount"); if (!mount) return;
        mounts.push_back(std::move(mount).value().mount);
    }
    const auto filesystem = eawr::vfs::Vfs::mount(mounts).value();
    const auto catalog = eawr::data::load_catalog(filesystem, eawr::data::Profile::foc).value();
    const auto data = eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog).value();
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    std::vector<eawr::campaign::MovementTypeBinding> bindings;
    for (const auto& t : types.types) if (t.object_class == "SpaceUnit" && t.hyperspace_speed && t.squadron_capacity)
        bindings.push_back({bindings.size() + 1, t.name, true, false, false, false});
    const std::vector<eawr::campaign::MovementPlayerBinding> players{{1, "Empire"}, {2, "Rebel"}, {3, "Underworld"}};
    const auto inputs = eawr::campaign::build_movement_inputs(types, data, bindings, players);
    expect(inputs && !bindings.empty(), "stock original-type carrier inputs and all playable faction factors load");
    if (!inputs) { std::cerr << eawr::core::format_diagnostic(inputs.error()) << '\n'; return; }
    expect(inputs.value().base_mode_modifier == *data.constant("Object_Max_Speed_Multiplier_Galactic")->number,
        "stock initialized modifier follows loaded constant");
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto* original = types.find(bindings[i].original_type);
        expect(inputs.value().types[i].hyperspace_speed == *original->hyperspace_speed &&
            inputs.value().types[i].squadron_capacity == static_cast<std::uint32_t>(*original->squadron_capacity), "stock carrier movement reads original values");
    }
    std::cout << "stock movement types=" << bindings.size() << " initialized-modifier=" << inputs.value().base_mode_modifier.raw() << '\n';

    // #5093: exercise the actual stock hero and live combat-skip initialization,
    // without AI decisions or a viewer supplying movement commands.
    namespace cp = eawr::campaign;
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Empire";
    auto referenced_types = cp::campaign_referenced_types(data, campaign);
    referenced_types.push_back("Boba_Fett_Team");
    const auto stock_types = eawr::units::galactic_type_table(catalog.catalog, false, referenced_types);
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        const bool local = faction.name == "Empire";
        options.players.push_back({{id, id, 0}, faction.name, true});
        if (local) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({{id}, id,
            local ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, local ? id : 0});
    }
    std::vector<std::string> names;
    for (const auto& type : stock_types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
    const auto unseeded_plan = take(cp::plan_live_campaign_setup(filesystem, data, stock_types, campaign, options, "Normal_Default"));
    const auto unseeded_services = take(cp::prepare_live_campaign_services(filesystem, catalog.catalog, data,
        stock_types, unseeded_plan, "Normal_Default"));
    auto unseeded_root = take(cp::CampaignRoot::create(unseeded_plan, unseeded_services));
    const auto& owned_planets = unseeded_root.game()->galaxy().setup().planets;
    const auto owned_planet = std::find_if(owned_planets.begin(), owned_planets.end(),
        [&](const auto& row) { return row.owner == *options.solo_reference_human; });
    expect(owned_planet != owned_planets.end(), "stock Empire campaign initializes an owned planet");
    if (owned_planet == owned_planets.end()) return;
    const auto site = std::find_if(unseeded_plan.locations().begin(), unseeded_plan.locations().end(),
        [&](const auto& row) { return row.planet && row.planet->id == owned_planet->id; });
    expect(site != unseeded_plan.locations().end(), "stock Empire campaign has an owned hero construction site");
    if (site == unseeded_plan.locations().end()) return;
    // Equal Footing has no named stealth hero. Seed the authored team through
    // the stock constructor, which qualifies its real payload and abilities.
    auto hero_data = data;
    const auto source = std::find_if(hero_data.campaigns.begin(), hero_data.campaigns.end(),
        [&](const auto& row) { return row.name == campaign; });
    expect(source != hero_data.campaigns.end(), "stock Empire campaign source resolves");
    if (source == hero_data.campaigns.end()) return;
    source->starting_forces.push_back({"Empire", site->original_type, "Boba_Fett_Team"});
    const auto plan = take(cp::plan_live_campaign_setup(filesystem, hero_data, stock_types, campaign, options, "Normal_Default"));
    const auto services = take(cp::prepare_live_campaign_services(filesystem, catalog.catalog, hero_data, stock_types, plan, "Normal_Default"));
    auto root_owner = take(cp::CampaignRoot::create(plan, services));
    const auto stock = root_owner.game()->galaxy().setup();
    const auto boba = std::find_if(options.types.begin(), options.types.end(), [](const auto& row) { return row.original_type == "Boba_Fett"; });
    expect(boba != options.types.end(), "stock Boba type resolves"); if (boba == options.types.end()) return;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto game = take(gc::GalacticSession::create(stock));
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto initial = game.snapshot();
        const auto hero = std::find_if(initial->actor_records().begin(), initial->actor_records().end(),
            [&](const auto& row) { return row.original_type == boba->id && row.owner == *options.solo_reference_human && !row.dead; });
        expect(hero != initial->actor_records().end(), "stock Empire campaign contains Boba");
        if (hero == initial->actor_records().end()) return;
        auto member = hero->id; auto parent = hero->container; gc::FleetId id{};
        for (std::size_t remaining = initial->actor_records().size(); parent && remaining; --remaining) {
            if (std::any_of(initial->planets().begin(), initial->planets().end(), [&](const auto& row) { return row.id == parent; })) break;
            const auto at = std::find_if(initial->fleets().begin(), initial->fleets().end(), [&](const auto& row) { return row.id == parent; });
            if (at != initial->fleets().end()) { id = at->id; break; }
            const auto actor = std::find_if(initial->actor_records().begin(), initial->actor_records().end(), [&](const auto& row) { return row.id == parent; });
            if (actor == initial->actor_records().end()) break;
            member = parent; parent = actor->container;
        }
        if (!id) {
            const auto launched = take(game.launch_native_transport(*options.solo_reference_human, member));
            expect(launched.admitted, "landed Boba launches through the ordinary transport owner");
            if (!launched.admitted) return;
            id = launched.object;
        }
        if (fleet(*game.snapshot(), id).members.size() > 1) {
            const auto detached = take(game.admit_native_fleet(*options.solo_reference_human,
                gc::TransferFleetMembers{{{id, member}}, 0, 2}, false));
            expect(detached.admitted, "Boba separates from ordinary combatants"); if (!detached.admitted) return;
            id = detached.object;
        }
        const auto skip = game.snapshot()->fleet_combat_skip(id);
        expect(skip && skip.value(), "stock Boba fleet has live recursive combat skipping"); if (!skip || !skip.value()) return;
        const auto origin = fleet(*game.snapshot(), id).orbit;
        const auto maximum = stock.rules.maximum_fleet_movement_distance;
        expect(maximum.has_value(), "stock campaign supplies its movement graph distance"); if (!maximum) return;
        const auto graph = take(gc::PlanetGraph::build(stock.planets, stock.routes, *maximum));
        std::vector<gc::PathPlanet> access;
        for (const auto& planet : stock.planets) { gc::PathPlanet row; row.id = planet.id; access.push_back(row); }
        gc::PathOptions path_options; path_options.multiplayer = true;
        path_options.allow_neutral = path_options.allow_hostile = path_options.allow_defended = path_options.allow_bad_final = true;
        std::vector<gc::PlanetId> path;
        for (const auto& target : stock.planets) {
            const auto admitted = std::find_if(stock.movement->access.begin(), stock.movement->access.end(),
                [&](const auto& row) { return row.player == *options.solo_reference_human && row.planet == target.id; });
            if (target.owner == *options.solo_reference_human || admitted == stock.movement->access.end()
                || !admitted->accessible || !admitted->potential_conflict) continue;
            auto candidate = take(gc::find_planet_path(graph, origin, target.id, access, path_options)).planets;
            if (!candidate.empty() && (path.empty() || candidate.size() < path.size())) path = std::move(candidate);
        }
        expect(path.size() >= 2, "stock Boba has a reachable hostile defended destination"); if (path.size() < 2) return;
        std::vector<std::string> hashes;
        const auto seat = gc::SeatId{*options.solo_reference_human};
        for (const auto sequence : {1ULL, 2ULL}) {
            take(game.submit({{game.completed_tick(), seat, sequence}, gc::MoveFleet{id, path, {}}}));
            (void)take(game.step(executor));
            expect(fleet(*game.snapshot(), id).motion.transit.has_value(), "stock Boba move begins, including the second move");
            for (unsigned tick = 0; tick < 20000 && fleet(*game.snapshot(), id).motion.transit; ++tick)
                (void)take(game.step(executor));
            const auto snapshot = game.snapshot(); const auto& arrived = fleet(*snapshot, id);
            std::cout << "5093 workers=" << workers << " move=" << sequence << " fleet=" << id
                << " orbit=" << arrived.orbit << " transit=" << bool(arrived.motion.transit)
                << " ability_preparations=" << (game.ability_state() ? game.ability_state()->preparations.size() : 0)
                << " raid=" << arrived.raid_destination.value_or(0) << '\n';
            expect(arrived.orbit == path.back() && !arrived.motion.transit, "stock Boba arrives and clears transit");
            expect(!game.ability_state() || game.ability_state()->preparations.empty(), "ordinary stealth travel leaves no pending hero mission");
            hashes.push_back(game.state_sha256());
            std::reverse(path.begin(), path.end());
        }
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "stock stealth round trip is identical on 1/2/4/8 workers");
    }
}
[[maybe_unused]] void pathfinder_movement() { // Retained for a future registered pathfinder scenario.
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto world = gc::GalacticSession::create(setup()).value();
        expect(bool(world.submit({{0, gc::SeatId{1}, 1}, gc::SetFleetPathfinder{10, 20}})), "movement pathfinder assigns");
        expect(bool(world.submit({{0, gc::SeatId{1}, 2}, gc::MoveFleet{10, {1, 2}, {}}})), "pathfinder fleet receives movement");
        expect(bool(world.submit({{0, gc::SeatId{1}, 3}, gc::SetFleetPathfinder{10, 0}})), "transit clear reaches admission");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (unsigned tick = 0; tick < 12; ++tick) {
            auto stepped = world.step(executor);
            expect(bool(stepped), "pathfinder fleet movement steps"); if (!stepped) return;
            expect(fleet(*world.snapshot()).members.front().pathfinder, "pathfinder flag survives transit and arrival");
            if (tick == 0) expect(event(*world.snapshot(), gc::GalacticEventKind::command_rejected)
                && fleet(*world.snapshot()).motion.transit.has_value(), "in-transit slot change refuses before changing the selection");
        }
    }
}

} // namespace
int case_galactic_movement_tests(int argc, const char* const argv[]) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--generate") { generate(argv[2]); return 0; }
        if (argc == 2 && std::string_view(argv[1]) == "--installed") { installed(); return galactic_test::failures == 0 ? 0 : 1; }
        staged_arrival_recheck_contracts(); native_movement_layers(); departure(); mixed_squadron_packing(); move_after_merge(); roundtrip_and_parity(); arrival(); orbit_placement_and_recheck(); transports(); adapter();
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return galactic_test::failures == 0 ? 0 : 1;
}
