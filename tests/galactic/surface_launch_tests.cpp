// #2880 / WGM-13, WGUI-55: the seat's surface-to-orbit launch (galactic opcode 18). It reuses the
// native launch gates and the ordinary fleet admission, so these cases pin the command's wire form,
// its all-or-nothing admission and refusals, equality with the native owner, the observer's surface
// references, and checkpoint/replay continuation on 1/2/4/8 workers.

#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using galactic_test::expect;
using eawr::sim::math::Fixed;
using EntityId = eawr::sim::EntityId;
Fixed f(std::int64_t value) { return Fixed::from_integer(value).value(); }
using eawr::testing::result_diagnostic::take;
void done(eawr::core::Result<void> result) {
    eawr::testing::result_diagnostic::must(std::move(result));
}

// Planet 1 (local): own transports 30, 31 and a nontransport company 32 on the surface, own fleet 10 in
// orbit. Planet 2 (local): own transport 51 and own fleet 13 in orbit. Planet 3 (local): own transport 50
// under an enemy fleet 12 and no own orbital presence, so it is blockaded. Planet 4 (enemy): enemy
// company 40.
gc::GalacticSetup fixture() {
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{gc::SeatId{1}, 1, gc::SeatKind::human, 0, true, 1}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 1}, {3, {f(24), {}, {}}, 1}, {4, {f(36), {}, {}}, 2}};
    s.rules.maximum_fleet_movement_distance = f(12); s.rules.fiscal_cycle_frames = 100;
    s.fleets = {{10, 1, 1, {{20, 1}}}, {12, 2, 3, {{24, 1}}}, {13, 1, 2, {{25, 1}}}};
    s.fleet_control.emplace(); s.fleet_control->types = {{1, false, true, false}, {2, true, true, false}};
    s.movement.emplace(); auto& m = *s.movement;
    m.mode_modifier = f(1);
    m.types = {{1, f(1), f(100), 0, true, false, false, false}, {2, f(1), f(50), 0, true, false, false, false, true}};
    m.players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}, {3, f(1), f(1), false}};
    for (const auto& q : s.planets) m.planets.push_back({q.id, f(1), false});
    // Every transport, landed or in orbit, owns an explicit (empty) live contents row.
    m.contents = {{30, {}}, {31, {}}, {40, {}}, {50, {}}, {51, {}}};
    for (const auto& p : s.players) for (const auto& q : s.planets) m.access.push_back({p.player_id, q.id, true, false});
    s.control.emplace(); auto& c = *s.control;
    c.change_seconds = f(7); c.max_ground_forces = 10;
    c.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : c.players) for (const auto& b : c.players) c.relations.push_back({a.player, b.player, a.player == b.player});
    c.types = {{1, 1, false, false}, {2, 0, false, true}};
    for (const auto& p : s.planets) {
        gc::ControlPlanet row; row.planet = p.id; row.type = 100 + p.id; row.surface_accessible = true;
        if (p.id == 1) row.landed = {{30, 2, 1}, {31, 2, 1}, {32, 1, 1}};
        if (p.id == 2) row.landed = {{51, 2, 1}};
        if (p.id == 3) row.landed = {{50, 2, 1}};
        if (p.id == 4) row.landed = {{40, 2, 2}};
        c.planets.push_back(row);
    }
    // The observer's live surface references mirror the control lists (WGUI-41/103).
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

gc::GameCommand launch(std::uint64_t tick, std::uint64_t sequence, std::vector<EntityId> units,
    gc::FleetId target = 0, std::uint8_t slot = 0, std::uint32_t seat = 1) {
    return {{tick, gc::SeatId{seat}, sequence}, gc::LaunchFromSurface{std::move(units), target, slot}};
}
const gc::SurfaceForce* surface_unit(const gc::GalacticSnapshot& snapshot, EntityId id) {
    for (const auto& planet : snapshot.control()->planets)
        for (const auto& unit : planet.landed) if (unit.id == id) return &unit;
    return nullptr;
}
const gc::Fleet* fleet_of(const gc::GalacticSnapshot& snapshot, EntityId member) {
    for (const auto& fleet : snapshot.fleets())
        for (const auto& row : fleet.members) if (row.id == member) return &fleet;
    return nullptr;
}
bool observed_on_surface(const gc::GalacticSnapshot& snapshot, EntityId id) {
    for (const auto& planet : snapshot.observer()->planets)
        for (const auto& row : planet.landed_transports) if (row.id == id) return true;
    return false;
}
const gc::GalacticEvent* event_of(const gc::GalacticSnapshot& snapshot, gc::GalacticEventKind kind, std::uint64_t sequence) {
    for (const auto& event : snapshot.events()) if (event.kind == kind && event.sequence == sequence) return &event;
    return nullptr;
}
std::vector<std::tuple<gc::PlanetId, EntityId, gc::TypeId, gc::PlayerId>> surface_of(const gc::GalacticSnapshot& snapshot) {
    std::vector<std::tuple<gc::PlanetId, EntityId, gc::TypeId, gc::PlayerId>> all;
    for (const auto& planet : snapshot.control()->planets)
        for (const auto& unit : planet.landed) all.emplace_back(planet.planet, unit.id, unit.type, unit.owner);
    return all;
}
// Identity, owner, orbit, slot and ordered members; excludes motion bookkeeping.
std::vector<std::tuple<gc::FleetId, gc::PlayerId, gc::PlanetId, unsigned, std::vector<EntityId>>> fleets_of(
    const gc::GalacticSnapshot& snapshot) {
    std::vector<std::tuple<gc::FleetId, gc::PlayerId, gc::PlanetId, unsigned, std::vector<EntityId>>> all;
    for (const auto& fleet : snapshot.fleets()) {
        std::vector<EntityId> members;
        for (const auto& row : fleet.members) members.push_back(row.id);
        all.emplace_back(fleet.id, fleet.owner, fleet.orbit, static_cast<unsigned>(fleet.orbit_slot), std::move(members));
    }
    return all;
}

void wire_format() {
    expect(gc::galactic_opcodes::launch_from_surface == 18 && gc::galactic_opcodes::sell_object == 19
        && gc::galactic_opcodes::transfer_fleet_members == 20 && gc::galactic_opcodes::recheck_orbit == 21
        && gc::galactic_opcodes::launch_fleet == 22 && gc::galactic_opcodes::first_reserved == 23,
        "launch continuation occupies 22 after orbit recheck 21");
    const gc::CommandPayload typed = gc::LaunchFromSurface{{30, 31}, 0, 2};
    expect(gc::layer_of(typed) == gc::CommandLayer::galactic && gc::opcode_of(typed) == 18, "launch is a galactic-layer opcode 18");
    const auto bytes = gc::encode_payload(typed);
    expect(bytes.size() == 4 + 2 * 8 + 8 + 1, "count, units, target and slot are the whole body");
    expect(gc::decode_payload(gc::CommandLayer::galactic, 18, bytes) == std::optional<gc::CommandPayload>(typed),
        "launch decodes to the same typed payload");
    const gc::CommandPayload fleet_target = gc::LaunchFromSurface{{30}, 10, 0};
    expect(gc::decode_payload(gc::CommandLayer::galactic, 18, gc::encode_payload(fleet_target))
        == std::optional<gc::CommandPayload>(fleet_target), "target-fleet launch round trips");
    const auto refuses = [](const std::vector<std::uint8_t>& body) {
        return !gc::decode_payload(gc::CommandLayer::galactic, 18, body);
    };
    auto trailing = bytes; trailing.push_back(0);
    auto truncated = bytes; truncated.pop_back();
    expect(refuses(trailing) && refuses(truncated) && refuses({}), "trailing, truncated and empty bodies are malformed");
    expect(refuses(gc::encode_payload(gc::LaunchFromSurface{{}, 0, 0})), "an empty unit list is malformed");
    expect(refuses(gc::encode_payload(gc::LaunchFromSurface{{30, 30}, 0, 0})), "duplicate units are malformed");
    expect(refuses(gc::encode_payload(gc::LaunchFromSurface{{0}, 0, 0})), "a null unit is malformed");
    expect(refuses(gc::encode_payload(gc::LaunchFromSurface{{30}, 0, 3})), "slot above 2 is malformed");
    expect(refuses(gc::encode_payload(gc::LaunchFromSurface{{30}, 10, 1})), "a creation slot with a named target is non-canonical");
    expect(!gc::decode_payload(gc::CommandLayer::session, 18, bytes), "opcode 18 belongs to the galactic layer only");

    const gc::GalacticReplay replay{fixture(), 2, {launch(0, 1, {30, 31}, 0, 1), launch(1, 2, {32})}};
    const auto written = take(gc::write_replay(replay));
    expect(take(gc::parse_replay(written)) == replay, "launch commands are canonical replay records");
}

void creates_and_extends_a_fleet() {
    eawr::platform::ThreadWorkerAdapter executor(1);
    auto world = take(gc::GalacticSession::create(fixture()));
    done(world.submit(launch(0, 1, {30, 31}, 0, 2)));
    auto tick = take(world.step(executor));
    const auto& snapshot = *tick.snapshot;
    const auto* created = fleet_of(snapshot, 30);
    expect(created && created == fleet_of(snapshot, 31) && created->owner == 1 && created->orbit == 1 && created->orbit_slot == 2
        && created->members.size() == 2 && created->members[0].id == 30 && created->members[1].id == 31,
        "units join one new orbit fleet at their planet in the requested slot, in command order");
    expect(!surface_unit(snapshot, 30) && !surface_unit(snapshot, 31) && surface_unit(snapshot, 32),
        "only the launched surface links are removed");
    expect(!observed_on_surface(snapshot, 30) && !observed_on_surface(snapshot, 31) && observed_on_surface(snapshot, 32)
        && observed_on_surface(snapshot, 40), "the observer's live surface references follow the launch and nothing else");
    const auto* executed = event_of(snapshot, gc::GalacticEventKind::command_executed, 1);
    expect(executed && created && executed->subject == created->id && executed->producer == gc::SeatId{1}
        && !event_of(snapshot, gc::GalacticEventKind::command_rejected, 1), "one executed event names the new fleet");
    expect(snapshot.fleet_control()->detached_units.empty(), "no temporary detached link survives the command");

    // A second world: extend the retained fleet 10 and keep its members in command order.
    auto second = take(gc::GalacticSession::create(fixture()));
    done(second.submit(launch(0, 1, {31, 30}, 10)));
    tick = take(second.step(executor));
    const auto* retained = fleet_of(*tick.snapshot, 30);
    expect(retained && retained->id == 10 && retained->members.size() == 3 && retained->members[1].id == 31
        && retained->members[2].id == 30, "a named target fleet receives the units after its members, in order");
    expect(event_of(*tick.snapshot, gc::GalacticEventKind::command_executed, 1)
        && event_of(*tick.snapshot, gc::GalacticEventKind::command_executed, 1)->subject == 10,
        "the executed event names the target fleet");
}

// A census-less, nested-less session has no actor owner: a fleet the movement owner creates (surface
// retreat here; the departure remnant shares the helper) must not add a lone fleet-shell record, or every
// combat-skip reader keyed on an empty record map would leave its member-fallback path (#2880).
void movement_created_fleet_adds_no_shell_record() {
    auto world = take(gc::GalacticSession::create(fixture()));
    const auto fleets_before = world.snapshot()->fleets().size();
    expect(world.snapshot()->actor_records().empty(), "the fixture session has no actor records");
    const std::vector<gc::FleetId> none;
    done(world.apply_retreat_return(1, 1, 2, true, none));
    const auto snapshot = world.snapshot();
    expect(snapshot->fleets().size() == fleets_before + 1 && fleet_of(*snapshot, 30) && fleet_of(*snapshot, 30) == fleet_of(*snapshot, 32),
        "the surface retreat created one fleet from the landed units");
    expect(snapshot->actor_records().empty(),
        "a movement-created fleet records no shell actor without a census or nested creation facts");
}

void refusals_leave_the_surface_intact() {
    struct Case { const char* name; std::vector<gc::GameCommand> commands; gc::GalacticRejectReason reason; std::uint64_t at; bool settle; };
    using Reason = gc::GalacticRejectReason;
    const std::vector<Case> cases{
        {"unknown unit", {launch(0, 1, {999})}, Reason::unknown_subject, 1, false},
        {"enemy-owned unit", {launch(0, 1, {40})}, Reason::not_authorized, 1, false},
        {"another seat's unit", {launch(0, 1, {30}, 0, 0, 2)}, Reason::not_authorized, 1, false},
        {"nontransport company", {launch(0, 1, {32})}, Reason::invalid_membership, 1, false},
        {"blockaded surface", {launch(1, 1, {50})}, Reason::movement_blocked, 1, true},
        {"enemy target fleet", {launch(0, 1, {30}, 12)}, Reason::not_authorized, 1, false},
        {"target in another orbit", {launch(0, 1, {30}, 13)}, Reason::incompatible_location, 1, false},
        {"unknown target fleet", {launch(0, 1, {30}, 999)}, Reason::unknown_subject, 1, false},
        {"mixed ownership is all-or-nothing", {launch(0, 1, {30, 40})}, Reason::not_authorized, 1, false},
        {"units on different planets", {launch(0, 1, {30, 51})}, Reason::incompatible_location, 1, false},
        {"one bad unit refuses the group", {launch(0, 1, {30, 31, 32})}, Reason::invalid_membership, 1, false},
        {"paused galaxy", {{{0, gc::SeatId{1}, 1}, gc::PauseGalaxy{}}, launch(0, 2, {30})}, Reason::paused, 2, false},
    };
    for (const auto& item : cases) {
        eawr::platform::ThreadWorkerAdapter executor(1);
        auto world = take(gc::GalacticSession::create(fixture()));
        const auto before_surface = surface_of(*world.snapshot());
        const auto before_fleets = fleets_of(*world.snapshot());
        std::shared_ptr<const gc::GalacticSnapshot> last;
        std::uint64_t completed = 0;
        if (item.settle) {
            last = take(world.step(executor)).snapshot; ++completed;
            const auto& control = *last->control();
            const auto state = std::find_if(control.state.begin(), control.state.end(), [](const auto& row) { return row.planet == 3; });
            expect(state != control.state.end() && state->blockaded, "the blockade case starts from a real published blockade");
        }
        for (const auto& command : item.commands) done(world.submit(command));
        while (completed <= item.commands.back().key.tick) { last = take(world.step(executor)).snapshot; ++completed; }
        const auto* rejected = event_of(*last, gc::GalacticEventKind::command_rejected, item.at);
        expect(rejected && rejected->reason == item.reason && rejected->producer == item.commands.back().key.seat, item.name);
        expect(!event_of(*last, gc::GalacticEventKind::command_executed, item.at),
            std::string(item.name) + ": no executed event for a refused launch");
        expect(last->fleet_control()->detached_units.empty(), std::string(item.name) + ": temporary detached link is rolled back");
        expect(surface_of(*last) == before_surface && fleets_of(*last) == before_fleets,
            std::string(item.name) + ": surface and fleets are exactly as before");
        for (const auto& row : before_surface)
            expect(observed_on_surface(*last, std::get<1>(row)), std::string(item.name) + ": observer references are untouched");
    }
}

void native_owner_equality() {
    eawr::platform::ThreadWorkerAdapter executor(1);
    auto seat = take(gc::GalacticSession::create(fixture()));
    done(seat.submit(launch(0, 1, {30, 31})));
    (void)take(seat.step(executor));
    auto native = take(gc::GalacticSession::create(fixture()));
    const auto first = take(native.launch_native_transport(1, 30, 0));
    expect(first.admitted, "native launch admits the first unit");
    expect(take(native.launch_native_transport(1, 31, first.object)).admitted, "native launch adds the second unit");
    (void)take(native.step(executor));
    expect(fleets_of(*seat.snapshot()) == fleets_of(*native.snapshot())
        && surface_of(*seat.snapshot()) == surface_of(*native.snapshot()),
        "the seat command and the native launch produce the same fleet and surface state");
    expect(!observed_on_surface(*native.snapshot(), 30) && observed_on_surface(*native.snapshot(), 32),
        "the native launch also retires the observer's surface reference");
    (void)take(native.step(executor));
    const auto blocked = take(native.launch_native_transport(1, 50, 0));
    expect(!blocked.admitted && blocked.reason == gc::GalacticRejectReason::movement_blocked,
        "the native owner still refuses a blockaded launch after the shared extraction");
}

void combined_launch_wire_and_admission() {
    const gc::CommandPayload typed = gc::LaunchFleet{{30, 31}, {1, 2}, std::nullopt};
    const auto bytes = gc::encode_payload(typed);
    expect(gc::opcode_of(typed) == 22 && gc::layer_of(typed) == gc::CommandLayer::galactic,
        "combined launch has its own typed galactic opcode");
    expect(gc::decode_payload(gc::CommandLayer::galactic, 22, bytes) == std::optional<gc::CommandPayload>{typed},
        "combined launch wire round trip");
    for (std::size_t n = 0; n < bytes.size(); ++n)
        expect(!gc::decode_payload(gc::CommandLayer::galactic, 22,
            std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n))),
            "every truncated launch body is refused");
    auto trailing = bytes; trailing.push_back(0);
    expect(!gc::decode_payload(gc::CommandLayer::galactic, 22, trailing), "trailing launch bytes refused");
    gc::LaunchFleet oversized{{}, {1, 2}, {}};
    for (EntityId id = 1; id <= 8200; ++id) oversized.units.push_back(id);
    expect(!gc::decode_payload(gc::CommandLayer::galactic, 22, gc::encode_payload(oversized)),
        "live commands obey the replay payload bound too");
    for (const auto& invalid : std::vector<gc::LaunchFleet>{
        {{}, {1, 2}, {}}, {{30, 30}, {1, 2}, {}}, {{0}, {1, 2}, {}},
        {{30}, {}, {}}, {{30}, {1}, {}}, {{30}, {1, 0}, {}}, {{30}, {1, 1}, {}}, {{30}, {1, 2}, std::uint8_t{3}}})
        expect(!gc::decode_payload(gc::CommandLayer::galactic, 22, gc::encode_payload(invalid)), "noncanonical combined launch refused");
    const gc::CommandPayload slot = gc::LaunchFleet{{30}, {1, 2}, std::uint8_t{1}};
    expect(gc::decode_payload(gc::CommandLayer::galactic, 22, gc::encode_payload(slot)) == std::optional{slot},
        "explicit destination slot survives codec");
    for (unsigned mode = 0; mode < 4; ++mode) {
        auto setup = fixture();
        if (mode == 2) for (auto& row : setup.movement->access)
            if (row.player == 1 && row.planet == 1) row.potential_conflict = true;
        eawr::platform::ThreadWorkerAdapter pool(1);
        auto game = take(gc::GalacticSession::create(setup));
        const gc::LaunchFleet order{mode == 3 ? std::vector<EntityId>{40} : std::vector<EntityId>{30, 31},
            mode == 1 ? std::vector<gc::PlanetId>{1, 4} : std::vector<gc::PlanetId>{1, 2}, {}};
        done(game.submit({{0, gc::SeatId{1}, 1}, order}));
        const auto snapshot = take(game.step(pool)).snapshot;
        const auto* fleet = fleet_of(*snapshot, 30);
        if (mode == 3) {
            expect(!fleet && surface_unit(*snapshot, 40), "unauthorized launch creates no fleet or movement");
        } else {
            expect(fleet && fleet == fleet_of(*snapshot, 31) && !surface_unit(*snapshot, 30), "launch preserves identities in one fleet");
            if (mode == 0) expect(fleet && fleet->motion.transit && fleet->motion.transit->path == order.path,
                "combined launch starts movement with its generated fleet");
            else expect(fleet && fleet->orbit == 1 && !fleet->motion.transit,
                "launch remains in source orbit when movement refuses or conflicts");
            if (mode == 1) expect(event_of(*snapshot, gc::GalacticEventKind::command_rejected, 1)
                && event_of(*snapshot, gc::GalacticEventKind::command_rejected, 1)->reason == gc::GalacticRejectReason::invalid_path,
                "ordinary movement owns the invalid-hop refusal");
            if (mode == 2) expect(event_of(*snapshot, gc::GalacticEventKind::movement_conflict, 0)
                && !event_of(*snapshot, gc::GalacticEventKind::movement_begin, 1), "source conflict suppresses departure");
        }
    }
}

void combined_source_refusals() {
    for (unsigned mode = 0; mode < 2; ++mode) {
        eawr::platform::ThreadWorkerAdapter pool(1);
        auto game = take(gc::GalacticSession::create(fixture()));
        const auto before_surface = surface_of(*game.snapshot());
        const auto before_fleets = fleets_of(*game.snapshot());
        const gc::LaunchFleet order{mode == 0 ? std::vector<EntityId>{30, 31} : std::vector<EntityId>{30, 51},
            mode == 0 ? std::vector<gc::PlanetId>{2, 3} : std::vector<gc::PlanetId>{1, 2}, {}};
        done(game.submit({{0, gc::SeatId{1}, 1}, order}));
        const auto snapshot = take(game.step(pool)).snapshot;
        const auto* rejected = event_of(*snapshot, gc::GalacticEventKind::command_rejected, 1);
        expect(rejected && rejected->reason == (mode == 0 ? gc::GalacticRejectReason::invalid_path
            : gc::GalacticRejectReason::incompatible_location), "combined launch rejects wrong path source or mixed source planets");
        expect(!event_of(*snapshot, gc::GalacticEventKind::command_executed, 1), "source refusal precedes fleet creation");
        expect(surface_of(*snapshot) == before_surface && fleets_of(*snapshot) == before_fleets
            && snapshot->fleet_control()->detached_units.empty(), "source refusal leaves all memberships unchanged");
        expect(observed_on_surface(*snapshot, 30) && observed_on_surface(*snapshot, 31) && observed_on_surface(*snapshot, 51),
            "source refusal leaves the observer links intact");
        auto baseline = take(gc::GalacticSession::create(fixture()));
        (void)take(baseline.step(pool));
        done(game.submit(launch(1, 2, {30})));
        done(baseline.submit(launch(1, 2, {30})));
        (void)take(game.step(pool)); (void)take(baseline.step(pool));
        const auto* after_refusal = fleet_of(*game.snapshot(), 30);
        const auto* untouched = fleet_of(*baseline.snapshot(), 30);
        expect(after_refusal && untouched && after_refusal->id == untouched->id,
            "source refusal consumes no generated fleet identity");
    }
    auto setup = fixture(); setup.movement.reset();
    eawr::platform::ThreadWorkerAdapter pool(1);
    auto game = take(gc::GalacticSession::create(setup));
    done(game.submit({{0, gc::SeatId{1}, 1}, gc::LaunchFleet{{30}, {1, 2}, {}}}));
    const auto snapshot = take(game.step(pool)).snapshot;
    const auto* cancelled = event_of(*snapshot, gc::GalacticEventKind::movement_canceled, 1);
    expect(fleet_of(*snapshot, 30) && cancelled && cancelled->reason == gc::GalacticRejectReason::unknown_value,
        "missing movement owner cancels only the committed launch's continuation");
    expect(event_of(*snapshot, gc::GalacticEventKind::command_executed, 1)
        && !event_of(*snapshot, gc::GalacticEventKind::command_rejected, 1), "recheck failure does not reject an already executed launch");
}

void combined_entry_admission() {
    for (unsigned mode = 0; mode < 5; ++mode) {
        auto setup = fixture();
        const bool allowed = mode == 1, nonhuman = mode == 2, conflict = mode == 3;
        for (auto& row : setup.movement->access)
            if (row.player == 1 && row.planet == 2) row.accessible = allowed;
        if (nonhuman) { setup.seats[0].kind = gc::SeatKind::ai; setup.seats[0].human_side_id = 0; }
        if (conflict) for (auto& row : setup.movement->access)
            if (row.player == 1 && row.planet == 1) row.potential_conflict = true;
        if (mode == 4) setup.control->planets[0].surface_accessible = false;
        eawr::platform::ThreadWorkerAdapter pool(1);
        auto game = take(gc::GalacticSession::create(setup));
        const auto before_fleets = fleets_of(*game.snapshot());
        done(game.submit({{0, gc::SeatId{1}, 1}, gc::LaunchFleet{{31, 30}, {1, 2}, {}}}));
        const auto snapshot = take(game.step(pool)).snapshot;
        if (allowed || nonhuman) {
            const auto* fleet = fleet_of(*snapshot, 30);
            expect(fleet && fleet == fleet_of(*snapshot, 31) && fleet->motion.transit
                && !surface_unit(*snapshot, 30), "qualified admission or nonhuman owner continues ordinary movement");
        } else if (conflict) {
            expect(fleet_of(*snapshot, 30) && !surface_unit(*snapshot, 30)
                && event_of(*snapshot, gc::GalacticEventKind::movement_conflict, 0)
                && !event_of(*snapshot, gc::GalacticEventKind::movement_canceled, 1), "source conflict suppresses the destination entry check");
        } else {
            const auto* cancelled = event_of(*snapshot, gc::GalacticEventKind::movement_canceled, 1);
            expect(cancelled && cancelled->reason == gc::GalacticRejectReason::movement_blocked
                && !event_of(*snapshot, gc::GalacticEventKind::movement_begin, 1), "denied entry cancels departure");
            expect(fleets_of(*snapshot) == before_fleets && snapshot->fleet_control()->detached_units.empty(),
                "denied entry destroys only its temporary fleet and retires every detached link");
            const auto* a = surface_unit(*snapshot, 30); const auto* b = surface_unit(*snapshot, 31);
            expect(a && b && a->type == 2 && b->type == 2 && a->owner == 1 && b->owner == 1,
                "re-landing preserves transport identities, types and owners");
            const auto& landed = snapshot->control()->planets[0].landed;
            expect(landed.size() == 3 && landed[0].id == 32 && landed[1].id == 31 && landed[2].id == 30,
                "leading unit and followers append after the untouched surface roster in command order");
            expect(observed_on_surface(*snapshot, 30) && observed_on_surface(*snapshot, 31), "re-landing republishes the live observer links");
            const auto* created = event_of(*snapshot, gc::GalacticEventKind::fleet_created, 1);
            const auto* destroyed = event_of(*snapshot, gc::GalacticEventKind::fleet_destroyed, 1);
            expect(created && destroyed && created->subject == destroyed->subject, "the same generated fleet is created and destroyed");
            expect(std::count_if(snapshot->events().begin(), snapshot->events().end(), [](const auto& event) {
                return event.sequence == 1 && event.kind == gc::GalacticEventKind::command_executed;
            }) == 1, "rollback preserves one launch-executed event");
        }
    }
}

// #4039: a denied destination entry creates the generated fleet and destroys it inside one command. The
// native actor-change batch then names an actor the delivered snapshot no longer holds; the AI's qualified
// fact reader refuses such a name, ending the whole galactic session. Delivered batches must only name
// actors the snapshot holds (a loss may name any retired actor).
void denied_entry_batches_name_only_live_actors() {
    for (const bool denied : {false, true}) {
        auto setup = fixture();
        for (auto& row : setup.movement->access) if (row.player == 1 && row.planet == 2) row.accessible = !denied;
        // The generated fleet's shell record needs a census, which needs the production, fiscal and economy tables.
        setup.player_economy.emplace(); setup.fiscal.emplace(); setup.production.emplace(); setup.census.emplace();
        setup.production->maximum_human_queue = 1;
        setup.census->types = {{1, {}}, {2, {}}};
        for (const auto& player : setup.players) {
            const bool human = player.player_id == 1;
            gc::GalacticPlayerEconomy economy; economy.player = player.player_id;
            setup.player_economy->players.push_back(economy);
            setup.fiscal->players.push_back({player.player_id, false, f(1)});
            setup.production->players.push_back({player.player_id, human, !human, human, {}, {}, {}, {}, {}});
            setup.census->players.push_back({player.player_id, 0, false, {}});
        }
        for (const auto& planet : setup.planets) {
            setup.fiscal->planets.push_back({planet.id, {}});
            gc::PlanetProduction row; row.planet = planet.id; setup.production->planets.push_back(row);
        }
        for (const auto id : {20ULL, 24ULL, 25ULL, 30ULL, 31ULL, 32ULL, 40ULL, 50ULL, 51ULL}) {
            const gc::TypeId type = id == 20 || id == 24 || id == 25 || id == 32 ? 1 : 2;
            const gc::PlayerId owner = id == 24 || id == 40 ? 2 : 1;
            const gc::PlanetId planet = id == 20 || id == 30 || id == 31 || id == 32 ? 1 : id == 25 || id == 51 ? 2 : id == 40 ? 4 : 3;
            gc::CensusObject object{id, type, owner, planet, planet, false, false, type};
            object.combat_role = gc::ActorCombatRole::ordinary; // the movement checks read each member's role
            setup.census->objects.push_back(object);
        }
        eawr::platform::ThreadWorkerAdapter pool(1);
        auto game = take(gc::GalacticSession::create(setup));
        std::vector<std::string> stale;
        std::size_t batches = 0;
        bool saw_created{};
        game.set_native_actor_changes_handler([&](const gc::GalacticSnapshot& snapshot, auto changes) {
            ++batches;
            for (const auto& change : changes) {
                if (change.kind == gc::GalacticSession::NativeActorChange::Kind::created) saw_created = true;
                if (change.kind == gc::GalacticSession::NativeActorChange::Kind::lost) continue;
                const auto records = snapshot.actor_records();
                const auto at = std::find_if(records.begin(), records.end(), [&](const auto& row) { return row.id == change.actor; });
                if (at == records.end()) stale.push_back(std::to_string(change.actor));
            }
            return eawr::core::Result<void>::success();
        });
        done(game.submit({{0, gc::SeatId{1}, 1}, gc::LaunchFleet{{31, 30}, {1, 2}, {}}}));
        const auto snapshot = take(game.step(pool)).snapshot;
        if (denied) {
            expect(event_of(*snapshot, gc::GalacticEventKind::movement_canceled, 1) && surface_unit(*snapshot, 30) && surface_unit(*snapshot, 31),
                "the denied entry re-landed the launched units");
        } else {
            expect(fleet_of(*snapshot, 30) && fleet_of(*snapshot, 31), "the allowed entry created the launched fleet");
            expect(saw_created, "the allowed launch records a created entry before the denied-launch coalescing case");
        }
        expect(batches > 0, "the launch delivered actor changes");
        expect(stale.empty(), "no delivered change names an actor the snapshot no longer holds (the generated fleet was created and lost)");
    }
}

void nonhuman_unknown_destination_cancels_travel() {
    auto setup = fixture(); setup.seats[0].kind = gc::SeatKind::ai; setup.seats[0].human_side_id = 0;
    auto incomplete = setup;
    std::erase_if(incomplete.movement->access, [](const auto& row) { return row.player == 1 && row.planet == 2; });
    expect(!gc::GalacticSession::create(incomplete), "known destinations require complete access rows even for nonhuman owners");
    const gc::GalacticReplay replay{setup, 3, {
        {{0, gc::SeatId{1}, 1}, gc::LaunchFleet{{31, 30}, {1, 999}, {}}},
    }};
    const auto encoded = take(gc::write_replay(replay));
    const auto decoded = take(gc::parse_replay(encoded));
    expect(take(gc::write_replay(decoded)) == encoded, "unknown-destination AI launch round trips without inventing an access row");
    std::vector<std::string> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(eawr::campaign::GameSession::from_replay(decoded));
        std::vector<std::string> hashes{take(game.step(pool)).state_hash.get()};
        const auto snapshot = game.galaxy().snapshot();
        const auto* fleet = fleet_of(*snapshot, 30);
        const auto* cancelled = event_of(*snapshot, gc::GalacticEventKind::movement_canceled, 1);
        expect(fleet && fleet == fleet_of(*snapshot, 31) && fleet->orbit == 1 && !fleet->motion.transit
            && !surface_unit(*snapshot, 30), "unknown destination keeps the committed AI launch in source orbit");
        expect(cancelled && cancelled->reason == gc::GalacticRejectReason::unknown_value
            && event_of(*snapshot, gc::GalacticEventKind::command_executed, 1)
            && !event_of(*snapshot, gc::GalacticEventKind::command_rejected, 1)
            && !event_of(*snapshot, gc::GalacticEventKind::movement_begin, 1),
            "missing AI destination row cancels travel rather than rejecting an executed launch");
        const auto identities = cp::game_identities(setup);
        const auto saved = take(cp::save_game_checkpoint(game, identities));
        galactic_test::mark_saved(game);
        auto restored = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        for (std::uint64_t tick = 1; tick < replay.final_tick_count; ++tick) {
            hashes.push_back(take(game.step(pool)).state_hash.get());
            expect(take(restored.session.step(pool)).state_hash.get() == hashes.back(), "AI cancellation restores its committed launch exactly");
        }
        expect(take(cp::save_game_checkpoint(game, identities)) == take(cp::save_game_checkpoint(restored.session, identities)),
            "AI cancellation checkpoint continuation re-saves identically");
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "AI unknown-destination cancellation is equal on 1/2/4/8 workers");
    }
}

void checkpoint_replay_and_workers() {
    const gc::GalacticReplay replay{fixture(), 6, {
        launch(0, 1, {30, 31}, 0, 1),     // creates a fleet in slot 1
        launch(1, 2, {32}),               // refused: nontransport
        launch(2, 3, {40}),               // refused: enemy surface
        {{3, gc::SeatId{1}, 4}, gc::MoveFleet{10, {1, 2}, std::nullopt}}, // ordinary movement still composes
    }};
    std::vector<std::string> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(eawr::campaign::GameSession::from_replay(replay));
        const auto identities = cp::game_identities(replay.setup);
        std::vector<std::string> hashes;
        for (std::uint64_t i = 0; i < 2; ++i) hashes.push_back(take(game.step(pool)).state_hash.get());
        const auto saved = take(cp::save_game_checkpoint(game, identities));
        galactic_test::mark_saved(game);
        for (std::uint64_t i = 2; i < replay.final_tick_count; ++i) hashes.push_back(take(game.step(pool)).state_hash.get());
        auto restored = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        const auto* revived = fleet_of(*restored.session.galaxy().snapshot(), 30);
        expect(revived && revived->orbit_slot == 1 && revived->members.size() == 2
            && !observed_on_surface(*restored.session.galaxy().snapshot(), 30),
            "the restored galaxy publishes the launched fleet and the retired surface reference");
        for (std::uint64_t i = 2; i < replay.final_tick_count; ++i)
            expect(take(restored.session.step(pool)).state_hash.get() == hashes[static_cast<std::size_t>(i)],
                "checkpoint continuation equals the uninterrupted launch run");
        expect(take(cp::save_game_checkpoint(restored.session, identities)) == take(cp::save_game_checkpoint(game, identities)),
            "the restored session re-saves the identical canonical checkpoint");
        const auto* launched = fleet_of(*game.galaxy().snapshot(), 30);
        expect(launched && launched->orbit_slot == 1 && launched->members.size() == 2, "the launched fleet keeps its slot");
        expect(!observed_on_surface(*game.galaxy().snapshot(), 30) && observed_on_surface(*game.galaxy().snapshot(), 32),
            "observer references stay current through the whole run");
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "launch hashes are equal on 1/2/4/8 workers");
    }
}

void combined_checkpoint_replay_and_workers() {
    const gc::GalacticReplay replay{fixture(), 6, {
        {{0, gc::SeatId{1}, 1}, gc::LaunchFleet{{30, 31}, {1, 2}, {}}},
        {{1, gc::SeatId{1}, 2}, gc::LaunchFleet{{51}, {2, 4}, {}}}, // launch succeeds, travel refuses
        {{2, gc::SeatId{1}, 3}, gc::LaunchFleet{{40}, {4, 3}, {}}}, // enemy actor refuses
    }};
    const auto parsed = take(gc::parse_replay(take(gc::write_replay(replay))));
    expect(parsed == replay, "combined commands survive a canonical replay");
    std::vector<std::string> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(eawr::campaign::GameSession::from_replay(parsed));
        const auto identities = cp::game_identities(parsed.setup);
        std::vector<std::string> hashes;
        hashes.push_back(take(game.step(pool)).state_hash.get());
        const auto saved = take(cp::save_game_checkpoint(game, identities));
        galactic_test::mark_saved(game);
        for (std::uint64_t i = 1; i < parsed.final_tick_count; ++i) hashes.push_back(take(game.step(pool)).state_hash.get());
        auto restored = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        for (std::uint64_t i = 1; i < parsed.final_tick_count; ++i)
            expect(take(restored.session.step(pool)).state_hash.get() == hashes[static_cast<std::size_t>(i)],
                "combined launch checkpoint retains moving fleet and queued continuations");
        expect(take(cp::save_game_checkpoint(restored.session, identities)) == take(cp::save_game_checkpoint(game, identities)),
            "combined launch continuation re-saves identical checkpoint");
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "combined launch replay hashes equal on 1/2/4/8 workers");
    }
}

void restricted_checkpoint_replay_and_workers() {
    auto setup = fixture();
    for (auto& row : setup.movement->access) if (row.player == 1 && row.planet == 2) row.accessible = false;
    const gc::GalacticReplay replay{setup, 6, {
        {{0, gc::SeatId{1}, 1}, gc::LaunchFleet{{31, 30}, {1, 2}, {}}},
        {{1, gc::SeatId{1}, 2}, gc::LaunchFleet{{30, 51}, {1, 2}, {}}},
        {{2, gc::SeatId{1}, 3}, gc::LaunchFleet{{31, 30}, {2, 3}, {}}},
        {{3, gc::SeatId{1}, 4}, gc::LaunchFleet{{30, 31}, {1, 2}, {}}},
    }};
    const auto parsed = take(gc::parse_replay(take(gc::write_replay(replay))));
    expect(parsed == replay, "replay retains the prequalified per-player entry result");
    std::vector<std::string> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(eawr::campaign::GameSession::from_replay(parsed));
        const auto identities = cp::game_identities(parsed.setup);
        std::vector<std::string> hashes;
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(surface_unit(*game.galaxy().snapshot(), 30) && observed_on_surface(*game.galaxy().snapshot(), 31)
            && !fleet_of(*game.galaxy().snapshot(), 30), "denied replay launch has already re-landed before checkpoint");
        const auto saved = take(cp::save_game_checkpoint(game, identities));
        galactic_test::mark_saved(game);
        for (std::uint64_t i = 1; i < parsed.final_tick_count; ++i) hashes.push_back(take(game.step(pool)).state_hash.get());
        auto restored = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        for (std::uint64_t i = 1; i < parsed.final_tick_count; ++i)
            expect(take(restored.session.step(pool)).state_hash.get() == hashes[static_cast<std::size_t>(i)],
                "restricted launch checkpoint continuation matches the uninterrupted run");
        expect(take(cp::save_game_checkpoint(restored.session, identities)) == take(cp::save_game_checkpoint(game, identities)),
            "restricted launch checkpoint re-saves identical bytes");
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "restricted launch replay hashes equal on 1/2/4/8 workers");
    }
}
} // namespace

int case_galactic_surface_launch_tests() {
    try {
        wire_format();
        creates_and_extends_a_fleet();
        refusals_leave_the_surface_intact();
        movement_created_fleet_adds_no_shell_record();
        native_owner_equality();
        combined_launch_wire_and_admission();
        combined_source_refusals();
        combined_entry_admission();
        denied_entry_batches_name_only_live_actors();
        nonhuman_unknown_destination_cancels_travel();
        checkpoint_replay_and_workers();
        combined_checkpoint_replay_and_workers();
        restricted_checkpoint_replay_and_workers();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::failures == 0 ? 0 : 1;
}
