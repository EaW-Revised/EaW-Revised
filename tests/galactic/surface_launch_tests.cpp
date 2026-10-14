#include "galactic_support.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/replay.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// #2880 / WGM-13, WGUI-55: the seat's surface-to-orbit launch (galactic opcode 18). It reuses the
// native launch gates and the ordinary fleet admission, so these cases pin the command's wire form,
// its all-or-nothing admission and refusals, equality with the native owner, the observer's surface
// references, and checkpoint/replay continuation on 1/2/4/8 workers.
namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::checkpoint;
using galactic_test::expect;
using eawr::sim::math::Fixed;
using EntityId = eawr::sim::EntityId;
Fixed f(std::int64_t value) { return Fixed::from_integer(value).value(); }
template <class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
    return std::move(result).value();
}
void done(eawr::core::Result<void> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
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
    o.local_player = 1; o.reveal_delay_seconds = f(1);
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
        && gc::galactic_opcodes::transfer_fleet_members == 20 && gc::galactic_opcodes::recheck_orbit == 21 && gc::galactic_opcodes::first_reserved == 22,
        "launch, sale and transfer occupy 18/19/20 before orbit recheck 21 and the held opcode 22");
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
} // namespace

int main() {
    try {
        wire_format();
        creates_and_extends_a_fleet();
        refusals_leave_the_surface_intact();
        movement_created_fleet_adds_no_shell_record();
        native_owner_equality();
        checkpoint_replay_and_workers();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::failures == 0 ? 0 : 1;
}
