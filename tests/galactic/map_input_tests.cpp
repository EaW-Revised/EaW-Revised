#include "galactic/map_input.hpp"
#include "eawr/sim/galactic/session.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <algorithm>
#include <array>

using namespace eawr::viewer::galactic;
namespace {
std::vector<InputPlanet> planets{{1, {1, 2, 3}, 1, true}, {2, {}, 2, false}, {3, {}, 1, true}};

void selection_and_caller_gates() {
    MapInput input;
    assert(!input.select(planets, 2, CameraState::overview).accepted);
    input.gates.action_menu = true;
    assert(!input.select(planets, 1, CameraState::overview).tutorial);
    input.gates.action_menu = false;
    input.gates.selection_locked = true;
    assert(!input.world_select(planets, 1, CameraState::overview).accepted);
    auto effect = input.select(planets, 1, CameraState::overview);
    assert(effect.changed && effect.tutorial); // Core setter does not own caller lock.
    effect = input.select(planets, 1, CameraState::overview);
    assert(effect.accepted && effect.tutorial && !effect.changed);
    effect = input.select(planets, 3, CameraState::inward);
    assert(effect.refocus && effect.changed);
    input.gates.selection_locked = false;
    assert(!input.world_select(planets, 1, CameraState::overview, true).zoom);
    assert(input.world_select(planets, 1, CameraState::overview, true).zoom);
    assert(input.release_widget([](auto) { assert(false); }));
    assert(!input.release_widget([](auto) { assert(false); }));
    assert(!input.cancel_world(planets, CameraState::focused).changed);
    assert(input.cancel_world(planets, CameraState::overview).changed && input.selected == 0);
    for (unsigned gate = 0; gate < 4; ++gate) {
        input.gates.active_mode = gate != 0; input.gates.demo = gate == 1;
        input.gates.cinematic = gate == 2; input.gates.victory_pending = gate == 3;
        assert(!input.world_select(planets, 1, CameraState::overview).accepted);
        assert(!input.wheel_zoom());
    }
}
void deferred_revalidation() {
    MapInput input;
    auto effect = input.select(planets, 2, CameraState::overview, true);
    assert(effect.deferred && effect.position && input.delay == 2 && input.selected == 0);
    assert(!input.service_deferred(planets, CameraState::overview).accepted);
    assert(!input.service_deferred(planets, CameraState::overview).accepted);
    assert(input.delay == 0 && input.deferred == 0 && input.selected == 0);
    assert(input.select(planets, 3, CameraState::overview, true).deferred);
    assert(!input.service_deferred(planets, CameraState::overview).accepted);
    input.gates.action_menu = true;
    assert(!input.service_deferred(planets, CameraState::overview).accepted);
    input.gates.action_menu = false;
    assert(input.select(planets, 3, CameraState::focused, true).changed && input.delay == 0);
    input.select(planets, 1, CameraState::overview, true);
    input.service_deferred(planets, CameraState::overview);
    assert(input.service_deferred(planets, CameraState::overview).changed && input.selected == 1);
}
void navigation_and_heroes() {
    MapInput input;
    assert(input.navigate(planets, 1, Navigation::next, CameraState::overview).target == 1);
    assert(input.navigate(planets, 1, Navigation::next, CameraState::overview).target == 3);
    assert(input.navigate(planets, 1, Navigation::next, CameraState::overview).target == 1);
    assert(input.navigate(planets, 1, Navigation::previous, CameraState::overview).target == 3);
    input.gates.selection_locked = true;
    assert(!input.navigate(planets, 1, Navigation::next_own, CameraState::overview).position);
    assert(input.navigate(planets, 1, Navigation::next_other, CameraState::overview).tutorial);
    assert(!input.hero(planets, true, false, 1, CameraState::overview, true).accepted);
    input.gates.selection_locked = false;
    input.selected = 0;
    input.gates.action_menu = true;
    auto effect = input.navigate(planets, 1, Navigation::next_own, CameraState::overview);
    assert(effect.position && !effect.accepted); // Own path intentionally ignores selection return.
    assert(!input.navigate(planets, 1, Navigation::next_other, CameraState::overview).position);
    input.gates.action_menu = false;
    assert(!input.display_index(planets, 1, CameraState::overview).accepted);
    assert(!input.display_index(planets, 9, CameraState::overview).accepted);
    assert(!input.go_home(planets, 0, CameraState::overview).accepted);
    assert(input.go_home(planets, 3, CameraState::overview).target == 3 && input.home == 3);
    assert(input.go_home(planets, 1, CameraState::overview).target == 3);
    assert(!input.hero(planets, false, false, 1, CameraState::overview, true).accepted);
    assert(!input.hero(planets, true, true, 1, CameraState::overview, true).accepted);
    assert(!input.hero(planets, true, false, 99, CameraState::overview, true).accepted);
    effect = input.hero(planets, true, false, 1, CameraState::overview, true);
    assert(effect.accepted && effect.position && effect.zoom && !effect.toggle_zoom);
}
void rectangle_and_widget_drop_order() {
    MapInput input;
    assert(!input.begin_box({0, 0}, false, true));
    assert(input.begin_box({0, 0}, true, true));
    input.move_box({12, 0}); assert(!input.box_end);
    input.move_box({12, 1}); assert(input.box_end);
    std::vector<InputWidget> widgets{
        {10, 100, 1, 0, 0, true, true, true, true, true},
        {11, 101, 1, 12, 1, true, true, true, true, true},
        {12, 102, 1, 12.1F, 1, true, true, true, true, true},
        {13, 103, 1, 2, 0, false, true, true, true, true},
        {14, 0, 1, 2, 0, true, true, true, true, true},
        {15, 105, 1, 2, 0, true, true, false, true, true},
        {16, 106, 1, 2, 0, true, true, true, false, true},
        {17, 107, 1, 2, 0, true, false, true, true, true}};
    assert(input.release_box(widgets));
    assert((input.multiselection == std::vector<ObjectId>{10, 11}));
    assert(!input.box_start && !input.box_end);
    assert(input.begin_box({12, 1}, true, true));
    input.move_box({0, 0}); assert(input.release_box(widgets));
    assert((input.multiselection == std::vector<ObjectId>{10, 11}));
    unsigned home_merges = 0;
    widgets[0].stacked_drag = false;
    assert(!input.begin_widget(widgets[0], true, true, [&] { ++home_merges; }));
    widgets[0].stacked_drag = true;
    assert(input.begin_widget(widgets[0], true, true, [&] { assert(input.dragged == 0); ++home_merges; }));
    assert(home_merges == 1);
    assert(input.begin_box({4, 5}, true, true)); // Existing drag consumes left press.
    assert(!input.box_start);
    std::vector<ObjectId> callbacks;
    assert(input.release_widget([&](auto id) { callbacks.push_back(id); }));
    assert((callbacks == std::vector<ObjectId>{10, 11}) && input.dragged == 0);
    input.held_scroll = input.held_rotate = input.hyperspace = input.double_release = input.tooltip = true;
    input.dragged = 11; input.box_start = PointerPoint{1, 2}; input.box_end = PointerPoint{20, 20};
    input.gates.tutorial_locked = true;
    assert(!input.hud_event());
    assert(!input.held_scroll && !input.held_rotate && !input.hyperspace && !input.double_release
        && !input.tooltip && !input.dragged && !input.box_start && !input.box_end);
}
void wheel_radar_and_typed_commands() {
    MapInput input;
    assert(input.wheel_zoom()); input.held_scroll = true; assert(!input.wheel_zoom());
    input.held_scroll = false; input.held_rotate = true; assert(!input.wheel_zoom());
    const auto point = input.radar({0, 0}, 100, 100); assert(point);
    input.gates.tutorial_scroll = false; assert(!input.radar({0, 0}, 100, 100));
    input.gates.tutorial_scroll = true; assert(!input.radar({0, 0}, 0, 100));
    DropIntent move{DropKind::move, 50, 0, 3, {1, 3}, 0, {}, std::nullopt};
    auto command = input.command(move, planets);
    assert(command && std::holds_alternative<eawr::sim::galactic::MoveFleet>(*command));
    assert((std::get<eawr::sim::galactic::MoveFleet>(*command).path == std::vector<ObjectId>{1, 3}));
    move.path = {1, 2, 3}; assert(!input.command(move, planets)); // Unrevealed intermediate.
    move.path = {1, 3}; move.planet = 2; assert(!input.command(move, planets));
    assert(input.command({DropKind::merge, 50, 51, 0, {}, 0, {}, std::nullopt}, planets));
    assert(!input.command({DropKind::merge, 50, 50, 0, {}, 0, {}, std::nullopt}, planets));
    assert(input.command({DropKind::land, 50, 0, 1, {}, 0, {}, std::nullopt}, planets));
    assert(!input.command({DropKind::land, 50, 0, 2, {}, 0, {}, std::nullopt}, planets));
    assert(input.command({DropKind::orbit_slot, 50, 0, 0, {}, 1, {}, std::nullopt}, planets));
    // #2880/WGM-13: a surface launch is a typed seat command; the dragged unit leads and the
    // simulation owns every admission, so only malformed intents and undiscovered planets refuse.
    {
        DropIntent launch{DropKind::launch, 50, 0, 1, {}, 2, {50, 52, 53}, std::nullopt};
        auto order = input.command(launch, planets);
        assert(order && std::holds_alternative<eawr::sim::galactic::LaunchFromSurface>(*order));
        const auto& typed = std::get<eawr::sim::galactic::LaunchFromSurface>(*order);
        assert((typed.units == std::vector<ObjectId>{50, 52, 53}) && typed.target == 0 && typed.slot == 2);
        launch = {DropKind::launch, 50, 51, 1, {}, 0, {}, std::nullopt};
        order = input.command(launch, planets);
        assert(order && std::get<eawr::sim::galactic::LaunchFromSurface>(*order).units == std::vector<ObjectId>{50}
            && std::get<eawr::sim::galactic::LaunchFromSurface>(*order).target == 51);
        launch.slot = 1; assert(!input.command(launch, planets)); // slot is creation-only
        launch = {DropKind::launch, 50, 0, 1, {}, 3, {}, std::nullopt}; assert(!input.command(launch, planets));
        launch = {DropKind::launch, 50, 0, 1, {}, 0, {51, 50}, std::nullopt}; assert(!input.command(launch, planets)); // primary first
        launch = {DropKind::launch, 50, 0, 1, {}, 0, {50, 50}, std::nullopt}; assert(!input.command(launch, planets)); // distinct
        launch = {DropKind::launch, 50, 0, 2, {}, 0, {}, std::nullopt}; assert(!input.command(launch, planets)); // undiscovered
    }
    assert(!input.command({DropKind::ability, 50, 0, 1, {}, 0, {}, std::nullopt}, planets));
    input.gates.tutorial_locked = true;
    assert(!input.command({DropKind::land, 50, 0, 1, {}, 0, {}, std::nullopt}, planets));
}
void fleet_destination_uses_live_routes() {
    namespace gc = eawr::sim::galactic;
    const auto fixed = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    gc::GalacticSetup setup;
    setup.players = {{1, 1, 0}, {2, 2, 0}};
    setup.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}};
    setup.planets = {{1, {}, 1}, {2, {fixed(100), {}, {}}, 1}, {3, {fixed(200), {}, {}}, 2}};
    setup.routes = {{4, 1, 2, fixed(2)}, {5, 2, 3, fixed(2)}};
    setup.rules.maximum_fleet_movement_distance = fixed(1);
    setup.fleets = {{10, 1, 1, {{20, 1}}}, {11, 2, 3, {{21, 1}}}};
    setup.fleet_control.emplace(); setup.fleet_control->types = {{1, false, true, false}};
    setup.movement.emplace();
    setup.movement->mode_modifier = fixed(1);
    setup.movement->types = {{1, fixed(2), fixed(100), 0, true, false, false, false}};
    setup.movement->players = {{1, fixed(1), fixed(1), false}, {2, fixed(1), fixed(1), false}};
    setup.movement->planets = {{1, fixed(1), false}, {2, fixed(1), false}, {3, fixed(1), false}};
    for (const auto& player : setup.players) for (const auto& planet : setup.planets)
        setup.movement->access.push_back({player.player_id, planet.id, true, planet.id == 3});
    auto created = gc::GalacticSession::create(setup); assert(created);
    auto snapshot = created.value().snapshot();
    std::vector<InputPlanet> visible{{1, {}, 1, true}, {2, {}, 1, true}, {3, {}, 2, true}};
    auto intent = fleet_destination_intent(*snapshot, 1, 10, 3, visible);
    assert(intent && intent->kind == DropKind::move && (intent->path == std::vector<ObjectId>{1, 2, 3}));
    MapInput input; const auto command = input.command(*intent, visible);
    assert(command && std::get<gc::MoveFleet>(*command).fleet == 10);
    assert(!fleet_destination_intent(*snapshot, 1, 11, 1, visible)); // Enemy selection cannot command.
    assert(!fleet_destination_intent(*snapshot, 1, 999, 3, visible)); // Stale selection.
    visible[1].discovered = false;
    assert(!fleet_destination_intent(*snapshot, 1, 10, 3, visible));
    visible[1].discovered = true;
    visible[2].discovered = false;
    assert(!fleet_destination_intent(*snapshot, 1, 10, 3, visible));
    visible[2].discovered = true;
    setup.movement->access[1].potential_conflict = true;
    created = gc::GalacticSession::create(setup); assert(created); snapshot = created.value().snapshot();
    assert(!fleet_destination_intent(*snapshot, 1, 10, 3, visible)); // Hostile intermediate stops route.
    intent = fleet_destination_intent(*snapshot, 1, 10, 2, visible);
    assert(intent && (intent->path == std::vector<ObjectId>{1, 2})); // Hostile destination is admitted.
    intent = fleet_destination_intent(*snapshot, 1, 10, 1, visible);
    assert(intent && intent->kind == DropKind::land && std::holds_alternative<gc::LandFleet>(*input.command(*intent, visible)));
    setup.fleets.front().motion.transit.emplace();
    // Transit state is inspected through a published value; no command can
    // redirect an already travelling fleet through this preview binding.
    gc::GalacticSnapshot travelling(0, {}, setup.players, setup.planets, setup.routes, setup.fleets, {});
    assert(!fleet_destination_intent(travelling, 1, 10, 3, visible));
}
// #3072: a dragged card's fleets merge into the dragged fleet, then one stack moves.
void dragged_card_moves_as_one_stack() {
    namespace gc = eawr::sim::galactic;
    const auto fixed = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    gc::GalacticSetup setup;
    setup.players = {{1, 1, 0}, {2, 2, 0}};
    setup.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}};
    setup.planets = {{1, {}, 1}, {2, {fixed(100), {}, {}}, 1}};
    setup.routes = {{4, 1, 2, fixed(2)}};
    setup.rules.maximum_fleet_movement_distance = fixed(1);
    // Fleets 10 and 12 share slot 0 (one card); 13 is slot 1.
    setup.fleets = {{10, 1, 1, {{20, 1}}}, {12, 1, 1, {{22, 1}, {23, 1}}}, {13, 1, 1, {{24, 1}}, 1}};
    setup.fleet_control.emplace(); setup.fleet_control->types = {{1, false, true, false}};
    setup.movement.emplace();
    setup.movement->mode_modifier = fixed(1);
    setup.movement->types = {{1, fixed(2), fixed(100), 0, true, false, false, false}};
    setup.movement->players = {{1, fixed(1), fixed(1), false}, {2, fixed(1), fixed(1), false}};
    setup.movement->planets = {{1, fixed(1), false}, {2, fixed(1), false}};
    for (const auto& player : setup.players) for (const auto& planet : setup.planets)
        setup.movement->access.push_back({player.player_id, planet.id, true, false});
    auto created = gc::GalacticSession::create(setup); assert(created);
    auto& session = created.value();
    const auto snapshot = session.snapshot();
    const std::array<std::uint8_t, 1> slot0{0};
    const std::array<std::uint8_t, 2> both{0, 2};
    assert((stack_fleets(*snapshot, 1, 1, slot0) == std::vector<ObjectId>{10, 12}));
    assert((stack_fleets(*snapshot, 1, 1, both) == std::vector<ObjectId>{10, 12}));
    assert((stack_fleets(*snapshot, 1, 2, slot0).empty()));
    assert((stack_fleets(*snapshot, 2, 1, slot0).empty())); // Only the local owner's fleets.
    const std::vector<InputPlanet> visible{{1, {}, 1, true}, {2, {}, 1, true}};
    auto intent = fleet_destination_intent(*snapshot, 1, 12, 2, visible);
    assert(intent && intent->kind == DropKind::move);
    intent->destination_slot = std::uint8_t{2};
    MapInput input;
    const auto order = input.command(*intent, visible);
    assert(order && std::get<gc::MoveFleet>(*order).destination_slot == std::optional{std::uint8_t{2}});
    const std::vector<ObjectId> stack{10, 12};
    const auto commands = stack_commands(*intent, stack, *order);
    assert(commands.size() == 2 && std::get<gc::MergeFleets>(commands[0]) == (gc::MergeFleets{10, 12}));
    for (const auto& command : commands) {
        const auto receipt = session.admit_native_fleet(1, command, false);
        assert(receipt && receipt.value().admitted);
    }
    const auto after = session.snapshot()->fleets();
    assert(std::none_of(after.begin(), after.end(), [](const auto& row) { return row.id == 10; }));
    const auto moved = std::find_if(after.begin(), after.end(), [](const auto& row) { return row.id == 12; });
    assert(moved != after.end() && moved->members.size() == 3 && moved->motion.transit);
    // A merge drop folds every stack fleet into the target; the drop itself merges the source.
    const DropIntent merge{DropKind::merge, 12, 13, 1, {}, 0, {}, std::nullopt};
    const auto merges = stack_commands(merge, std::vector<ObjectId>{10, 12}, gc::MergeFleets{12, 13});
    assert(merges.size() == 2 && std::get<gc::MergeFleets>(merges[0]) == (gc::MergeFleets{10, 13})
        && std::get<gc::MergeFleets>(merges[1]) == (gc::MergeFleets{12, 13}));
    intent->destination_slot = std::uint8_t{3}; assert(!input.command(*intent, visible));
}
void inward_refocus_immediately_focuses() {
    PlanetCamera camera;
    PlanetView first; first.id = 1; first.position = {10, 20, 0};
    PlanetView second; second.id = 3; second.position = {30, 40, 0};
    assert(camera.zoom_in(first, true));
    camera.service(100); camera.service(300);
    assert(camera.state() == CameraState::inward && camera.alpha() < 1.0F);
    assert(camera.refocus(second, true));
    assert(camera.state() == CameraState::focused && camera.planet() == 3 && camera.alpha() == 1.0F);
    camera.service(1000);
    assert(camera.state() == CameraState::focused && camera.planet() == 3);
}
void published_discovery_is_player_specific() {
    auto setup = synthetic_live_setup();
    auto without = eawr::sim::galactic::GalacticSession::create(setup);
    assert(without && !observer_discovery(*without.value().snapshot(), 1, 101).has_value());
    eawr::sim::galactic::ObserverSetup observer;
    observer.local_player = 1;
    observer.types.push_back({1, false, false, false});
    for (const auto& player : setup.players) {
        eawr::sim::galactic::ObserverPlayer row;
        row.player = player.player_id;
        observer.players.push_back(row);
    }
    for (const auto& planet : setup.planets) {
        eawr::sim::galactic::ObserverPlanet row;
        row.planet = planet.id;
        row.allegiance = row.target_owner = row.current_owner = row.current_controller = planet.owner;
        observer.planets.push_back(row);
    }
    for (const auto& player : setup.players) for (const auto& planet : setup.planets) {
        eawr::sim::galactic::ObserverRecord row;
        row.player = player.player_id; row.planet = planet.id;
        row.discovered = player.player_id == 2;
        observer.records.push_back(row);
    }
    setup.observer = std::move(observer);
    auto session = eawr::sim::galactic::GalacticSession::create(setup);
    assert(session);
    const auto snapshot = session.value().snapshot();
    assert(observer_discovery(*snapshot, 1, 101) == std::optional(false));
    assert(observer_discovery(*snapshot, 2, 101) == std::optional(true));
    assert(observer_discovery(*snapshot, 1, 999) == std::optional(false));
    assert(observer_discovery(*snapshot, 99, 101) == std::optional(false));
}
}
int main() {
    selection_and_caller_gates(); deferred_revalidation(); navigation_and_heroes();
    rectangle_and_widget_drop_order(); wheel_radar_and_typed_commands();
    inward_refocus_immediately_focuses();
    published_discovery_is_player_specific();
    fleet_destination_uses_live_routes();
    dragged_card_moves_as_one_stack();
}
