#include "galactic/map_input.hpp"
#include "galactic/space_proof_route.hpp"
#include "galactic/fleet_panels.hpp"
#include "galactic/fleet_traits.hpp"
#include "galactic/hero_map_bar.hpp"
#include "eawr/sim/galactic/session.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

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
    {
        const DropIntent launch_move{DropKind::launch, 50, 0, 1, {1, 3}, 0, {50, 52}, std::nullopt};
        const auto order = input.command(launch_move, planets);
        assert(order && std::holds_alternative<eawr::sim::galactic::LaunchFleet>(*order));
        const auto& typed = std::get<eawr::sim::galactic::LaunchFleet>(*order);
        assert(typed.units == launch_move.members && typed.path == launch_move.path && !typed.destination_slot);
        auto invalid = launch_move; invalid.path = {1, 2};
        assert(!input.command(invalid, planets)); // Undiscovered destination.
        invalid = launch_move; invalid.path = {3, 1};
        assert(!input.command(invalid, planets)); // Wrong source.
        invalid = launch_move; invalid.target = 51;
        assert(!input.command(invalid, planets)); // Launch/move creates its own fleet.
    }
    assert(!input.command({DropKind::ability, 50, 0, 1, {}, 0, {}, std::nullopt}, planets));
    input.gates.tutorial_locked = true;
    assert(!input.command({DropKind::land, 50, 0, 1, {}, 0, {}, std::nullopt}, planets));
}
void live_hero_containment_and_home() {
    namespace gc = eawr::sim::galactic;
    const auto base = std::make_shared<const gc::GalacticSnapshot>(0, gc::GalacticClock{},
        std::vector<gc::GalacticPlayer>{}, std::vector<gc::Planet>{{1, {}, 1}, {3, {}, 2}},
        std::vector<gc::Route>{}, std::vector<gc::Fleet>{{10, 1, 1, {{30, 8}}}}, std::vector<gc::GalacticEvent>{});
    std::vector<gc::CensusObject> actors{{20, 7, 2, 3, 3, false, false, 7},
        {21, 7, 1, 999, 30, false, false, 7}, {22, 7, 1, 3, 3, false, false, 7},
        {30, 8, 1, 1, 10, false, false, 8}};
    const auto snapshot = [&] {
        gc::NativeSnapshotChanges changes;
        changes.actors = std::make_shared<const std::vector<gc::CensusObject>>(actors);
        return gc::GalacticSnapshot::native_view(base, std::move(changes));
    };
    assert(first_hero_planet(*snapshot(), 7) == 3); // Foreign first identity still wins, like retail's type query.
    actors[0].current_type = 9;
    assert(first_hero_planet(*snapshot(), 7) == 1); // Nested parents resolve; stale .planet ignored.
    assert(actor_containing_planet(*snapshot(), 22) == 3); // Direct surface parent.
    assert(!first_hero_planet(*snapshot(), 0));
    assert(!first_hero_planet(*snapshot(), 99));
    actors[1].container = 999;
    assert(!first_hero_planet(*snapshot(), 7)); // Unresolved first live match does not pick the second.
    actors[1].container = 30; actors[3].container = 21;
    assert(!first_hero_planet(*snapshot(), 7)); // Cycle.
    actors[3].container = 10; actors[3].dead = true;
    assert(!first_hero_planet(*snapshot(), 7));
    actors[3].dead = false; actors[3].delete_pending = true;
    assert(!first_hero_planet(*snapshot(), 7));
    actors[3].delete_pending = false; actors[3].owner = 2;
    assert(first_hero_planet(*snapshot(), 7) == 1); // Ownership is not a containment predicate.
    actors[3].owner = 1;
    actors[1].dead = true; assert(first_hero_planet(*snapshot(), 7) == 3);
    actors[1].dead = false; actors[1].delete_pending = true; assert(first_hero_planet(*snapshot(), 7) == 3);
    actors[1].delete_pending = false; actors[1].current_type = 8;
    assert(first_hero_planet(*snapshot(), 7) == 3); // Current type, no original-type fallback.
    actors[1].current_type = 7;
    gc::NativeSnapshotChanges travelling;
    travelling.actors = std::make_shared<const std::vector<gc::CensusObject>>(actors);
    travelling.fleets = std::vector<gc::Fleet>{{10, 1, 1, {{30, 8}}}};
    travelling.fleets->front().motion.transit.emplace();
    assert(!first_hero_planet(*gc::GalacticSnapshot::native_view(base, std::move(travelling)), 7));
    MapInput input;
    assert(!input.go_home(planets, 999, CameraState::overview).accepted && input.home == 0);
    assert(!input.go_home(planets, 0, CameraState::overview).accepted && input.home == 0);
    const auto resolved = first_hero_planet(*snapshot(), 7);
    assert(input.go_home(planets, resolved, CameraState::overview).accepted && input.home == 1);
    assert(input.go_home(planets, 3, CameraState::overview).target == 1); // Valid cached fallback remains.
    input.gates.selection_locked = true;
    assert(!input.hero(planets, true, false, resolved, CameraState::overview, false).accepted);
    input.gates.selection_locked = false;
    assert(!input.hero(planets, true, true, resolved, CameraState::overview, false).accepted);
    assert(!input.hero(planets, false, false, resolved, CameraState::overview, false).accepted);
    const auto effect = input.hero(planets, true, false, resolved, CameraState::overview, true);
    assert(effect.accepted && effect.position && effect.zoom && effect.target == 1);
    input.selected = 0; // WGUI-47 accepts unchanged selection before checking discovery.
    auto hidden = planets; hidden.front().discovered = false;
    assert(!input.hero(hidden, true, false, resolved, CameraState::overview, false).accepted);
}
void queued_ability_drop_is_scoped_to_preparation_and_owner() {
    namespace gc = eawr::sim::galactic;
    auto owner = std::make_shared<int>(1);
    const std::weak_ptr<const void> lifetime = owner;
    gc::PreparedAbility capability; capability.actor = 50; capability.planet = 1; capability.preparation = 77;
    capability.kind = gc::AbilityKind::planet_destruction;
    PendingAbilityDrop pending;
    assert(!pending.matches(lifetime, capability));
    pending.arm(lifetime, capability);
    assert(pending.matches(lifetime, capability)); // Another release before a publication queues nothing.
    const auto next_publication = capability;
    pending.observe(&next_publication);
    assert(pending.matches(lifetime, capability)); // A new snapshot with the same capability is still pending.
    auto reopened = capability; ++reopened.preparation;
    assert(!pending.matches(lifetime, reopened));
    pending.observe(&reopened);
    assert(!pending.matches(lifetime, capability));
    pending.arm(lifetime, reopened); pending.observe(nullptr);
    assert(!pending.matches(lifetime, reopened)); // Retraction/settlement permits a later publication.
    pending.arm(lifetime, capability);
    auto replacement = std::make_shared<int>(2);
    const std::weak_ptr<const void> restored_lifetime = replacement;
    assert(!pending.matches(restored_lifetime, capability)); // Loaded owners may reuse numeric ids.
    owner.reset();
    assert(!pending.matches(restored_lifetime, capability));
}
// WGCH-151..155: the parked hero fleet's authored action at a hostile planet starts through the typed inbox.
void hero_fleet_drop_offers_authored_ability() {
    namespace gc = eawr::sim::galactic;
    using gc::AbilityKind;
    using eawr::sim::math::Fixed;
    std::vector<gc::Fleet> fleets{{10, 1, 2, {{30, 8}}}, {11, 1, 1, {{32, 8}}}};
    std::vector<gc::CensusObject> actors{{30, 8, 1, 2, 10, false, false, 8},
        {31, 9, 1, 2, 10, false, false, 9}, {32, 8, 1, 1, 11, false, false, 8}};
    gc::PoliticalControlSetup control;
    control.players = {{1, false, true}, {2, false, true}};
    control.relations = {{1, 1, true}, {1, 2, false}};
    control.planets = {{1}, {2}};
    control.state.resize(2);
    control.state[0].current_player = control.state[0].target_player = 1;
    control.state[1].current_player = control.state[1].target_player = 2;
    gc::EconomyCorruptionState economy;
    gc::CorruptionEconomyPlayer owner; owner.player = 2; owner.playable = true;
    economy.players = {owner}; economy.planets = {{1}, {2}};
    economy.planets[1].corruption = gc::corruption_fixture::Type::black_market;
    gc::ProductionSetup production;
    gc::ProductionPlayer producer; producer.player = 1; production.players = {producer};
    gc::PlanetProduction location; location.planet = 2;
    location.objects = {{70, 70, 2, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure}};
    production.planets = {location}; production.technology_types = {{70, 1, true, true, {1}}};
    gc::PlayerEconomySetup accounts;
    gc::GalacticPlayerEconomy account; account.player = 1; account.current_tech = 2; accounts.players = {account};
    gc::GalacticAbilitySetup host;
    gc::GalacticAbilityProfile profile; profile.type = 8; profile.has_market = false;
    profile.sabotage = gc::SabotageAbilityProfile{"S", true, {}};
    host.profiles = {profile}; host.activated_sabotage_abilities = {"S"};
    host.activated_destruction_abilities = {"D"}; host.activated_neutralize_abilities = {"N"};
    host.activated_corruption_abilities = {"C"}; host.activated_hack_abilities = {"H"};
    host.sabotage_costs = {{70, Fixed::from_integer(100).value()}};
    host.technologies = {{70}};
    host.theft_planets = {{2}};
    gc::CorruptionPlanet corruption; corruption.planet = 2; corruption.retains_influence = true;
    corruption.slots[0].type = gc::corruption_fixture::Type::piracy; host.corruption_planets = {corruption};
    gc::AbilityActorState service; service.actor = 30; service.type = 8; service.sabotage_enabled = true;
    std::shared_ptr<const gc::MovementSetup> movement;
    const auto snapshot = [&] {
        auto base = std::shared_ptr<const gc::GalacticSnapshot>(new gc::GalacticSnapshot(0, {}, {},
            {{1, {}, 1}, {2, {}, 2}}, {}, fleets, {}, std::nullopt, std::nullopt, movement,
            std::nullopt, {}, accounts, production, control, {}, {}, std::nullopt, std::nullopt, std::nullopt,
            {}, {}, std::nullopt, {}, std::nullopt, std::nullopt, economy));
        gc::NativeSnapshotChanges changes;
        changes.actors = std::make_shared<const std::vector<gc::CensusObject>>(actors);
        changes.ability_services = std::make_shared<const std::vector<gc::AbilityActorState>>(std::vector{service});
        return gc::GalacticSnapshot::native_view(base, std::move(changes));
    };
    auto drop = ability_drop_candidate(*snapshot(), 1, 10, host);
    assert(drop && drop->actor == 30 && drop->planet == 2 && drop->kind == AbilityKind::structure_sabotage);
    assert(std::get<gc::DeployGroundAbility>(ability_drop_order(*drop)) == (gc::DeployGroundAbility{30, 2}));
    assert(!ability_drop_candidate(*snapshot(), 1, 11, host));
    assert(!ability_drop_candidate(*snapshot(), 2, 10, host));
    assert(!ability_drop_candidate(*snapshot(), 1, 99, host));
    service.sabotage_cancelled = true; assert(!ability_drop_candidate(*snapshot(), 1, 10, host));
    service.sabotage_cancelled = false;
    host.profiles[0].lifecycle_only = true; assert(!ability_drop_candidate(*snapshot(), 1, 10, host));
    host.profiles[0].lifecycle_only = false;
    actors[0].dead = true; assert(!ability_drop_candidate(*snapshot(), 1, 10, host)); actors[0].dead = false;
    actors[0].owner = 2; assert(!ability_drop_candidate(*snapshot(), 1, 10, host)); actors[0].owner = 1;
    economy.planets[1].corruption = gc::corruption_fixture::Type::none;
    assert(!ability_drop_candidate(*snapshot(), 1, 10, host)); // Sabotage needs corruption.
    economy.planets[1].corruption = gc::corruption_fixture::Type::black_market;
    AbilityPreviewPolicy policy;
    policy.restricted = [](auto) { return true; };
    assert(!ability_drop_candidate(*snapshot(), 1, 10, host, policy)); policy.restricted = {};
    policy.space_override = [](auto, auto) { return true; };
    assert(!ability_drop_candidate(*snapshot(), 1, 10, host, policy)); policy.space_override = {};

    // Every supported kind uses its live service gate, authored routing and target rule.
    const std::array kinds{AbilityKind::planet_destruction, AbilityKind::technology_theft, AbilityKind::black_market,
        AbilityKind::structure_sabotage, AbilityKind::system_spy, AbilityKind::hero_neutralization, AbilityKind::corruption, AbilityKind::superweapon_hack};
    for (const auto kind : kinds) {
        auto& p = host.profiles[0]; p = {}; p.type = 8; p.has_market = false;
        service = {}; service.actor = 30; service.type = 8;
        if (kind == AbilityKind::planet_destruction) { p.destruction = gc::DestructionAbilityProfile{"D"}; service.destruction_enabled = true; }
        if (kind == AbilityKind::technology_theft) { p.theft.emplace(); service.theft_enabled = true; }
        if (kind == AbilityKind::black_market) { p.has_market = true; service.enabled = true; }
        if (kind == AbilityKind::structure_sabotage) { p.sabotage = gc::SabotageAbilityProfile{"S"}; service.sabotage_enabled = true; }
        if (kind == AbilityKind::system_spy) { p.spy.emplace(); p.spy->name = "SPY"; service.spy_enabled = true; host.activated_spy_abilities = {"SPY"}; }
        if (kind == AbilityKind::hero_neutralization) { p.neutralize.emplace(); p.neutralize->name = "N"; service.neutralize_enabled = true; }
        if (kind == AbilityKind::corruption) { p.corruption = gc::CorruptionAbilityProfile{"C"}; service.corruption_enabled = true; }
        if (kind == AbilityKind::superweapon_hack) { p.hack = gc::HackAbilityProfile{"H"}; service.hack_enabled = true; }
        economy.planets[1].corruption = kind == AbilityKind::corruption ? gc::corruption_fixture::Type::none : gc::corruption_fixture::Type::black_market;
        if (kind == AbilityKind::superweapon_hack) {
            host.hack_required_type = 99;
            fleets.push_back({12, 2, 2, {{40, 99}}}); actors.push_back({40, 99, 2, 2, 12, false, false, 99});
        }
        drop = ability_drop_candidate(*snapshot(), 1, 10, host);
        assert(drop && drop->kind == kind);
        if (kind == AbilityKind::superweapon_hack) {
            control.state[1].target_player = 1; control.planets[1].destroyed = true;
            assert(ability_drop_candidate(*snapshot(), 1, 10, host)); // Hack has neither opposing nor ruins gate.
            control.state[1].target_player = 2; control.planets[1].destroyed = false;
            fleets.pop_back(); actors.pop_back();
            assert(!ability_drop_candidate(*snapshot(), 1, 10, host));
        }
        policy.kind_restricted = [kind](auto, auto candidate) { return candidate == kind; };
        assert(!ability_drop_candidate(*snapshot(), 1, 10, host, policy)); policy.kind_restricted = {};
    }
    // Rank uses ORIGINAL type, includes ineligible entries, and retains stored order on ties.
    auto& p = host.profiles[0]; p = {}; p.type = 8; p.has_market = false;
    p.destruction = gc::DestructionAbilityProfile{"D"}; service = {}; service.actor = 30; service.type = 8; service.destruction_enabled = true;
    p.neutralize.emplace(); p.neutralize->name = "N"; service.neutralize_enabled = true;
    assert(ability_drop_candidate(*snapshot(), 1, 10, host)->kind == AbilityKind::planet_destruction);
    service.destruction_enabled = false;
    assert(ability_drop_candidate(*snapshot(), 1, 10, host)->kind == AbilityKind::hero_neutralization);
    service.destruction_enabled = true; p.neutralize.reset(); service.neutralize_enabled = false;
    fleets[0].members.push_back({31, 9});
    policy.rank = [](auto type) -> std::int64_t { return type == 8 ? 2000 : 3000; };
    assert(ability_drop_candidate(*snapshot(), 1, 10, host, policy)->actor == 30); // First candidate above 999 survives.
    policy.rank = [](auto type) -> std::int64_t { return type == 9 ? 0 : 100; };
    assert(!ability_drop_candidate(*snapshot(), 1, 10, host, policy)); // Lower-ranked actor has no ability; no fallback.
    actors[0].original_type = 9; actors[1].original_type = 8;
    assert(ability_drop_candidate(*snapshot(), 1, 10, host, policy)->actor == 30);
    actors[0].original_type = 8; actors[1].original_type = 9;
    policy.rank = [](auto) -> std::int64_t { return 25; };
    assert(ability_drop_candidate(*snapshot(), 1, 10, host, policy)->actor == 30);
    std::reverse(fleets[0].members.begin(), fleets[0].members.end());
    assert(!ability_drop_candidate(*snapshot(), 1, 10, host, policy));
    std::reverse(fleets[0].members.begin(), fleets[0].members.end());
    p.destruction.reset(); p.sabotage = gc::SabotageAbilityProfile{"S"}; service.sabotage_enabled = true;
    assert(!ability_drop_candidate(*snapshot(), 1, 10, host)); // Multiple supplied entries suppress non-destruction.
    fleets[0].members.pop_back();

    // Stored transport order previews a live bearer; deployment retains the supplied transport.
    auto contents = std::make_shared<gc::MovementSetup>();
    gc::MovementType transport; transport.type = 7; transport.transport = true; contents->types = {transport};
    contents->contents = {{30, {{31, 8}}}}; movement = contents;
    actors[0].current_type = 7; actors[1].container = 30; actors[1].current_type = 8;
    service.actor = 31; service.type = 8;
    drop = ability_drop_candidate(*snapshot(), 1, 10, host);
    assert(drop && drop->actor == 31 && drop->supplied == 30);
    assert(std::get<gc::DeployGroundAbility>(ability_drop_order(*drop)) == (gc::DeployGroundAbility{30, 2}));
    actors[1].container = 99; assert(!ability_drop_candidate(*snapshot(), 1, 10, host)); actors[1].container = 30;
    // Presentation services are not canonical GC state, including flattened native overlays.
    const auto before = snapshot();
    auto disabled = service; disabled.sabotage_cancelled = true;
    gc::NativeSnapshotChanges changed;
    changed.ability_services = std::make_shared<const std::vector<gc::AbilityActorState>>(std::vector{disabled});
    const auto after = gc::GalacticSnapshot::native_view(before, std::move(changed));
    assert(before->canonical_bytes() == after->canonical_bytes() && before->sha256() == after->sha256());
    assert(!ability_drop_candidate(*after, 1, 10, host));
    gc::NativeSnapshotChanges unrelated; unrelated.observer_generation = 42;
    const auto flattened = gc::GalacticSnapshot::native_view(after, std::move(unrelated));
    assert(flattened->ability_services()[0].sabotage_cancelled);
    assert(before->canonical_bytes() == flattened->canonical_bytes());
    gc::NativeSnapshotChanges other_seat; other_seat.observer.emplace();
    other_seat.observer->human_observers = {2}; other_seat.observer->solo_reference_human = 2;
    const auto foreign = gc::GalacticSnapshot::native_view(flattened, std::move(other_seat));
    assert(foreign->ability_services().empty()); // Inherited service gates never cross observer seats.
}

void typed_ability_intents_preserve_capabilities() {
    namespace gc = eawr::sim::galactic;
    MapInput input;
    DropIntent intent; intent.kind = DropKind::ability; intent.source = 50; intent.planet = 1;
    const gc::PrepareGalacticAbility prepare{50, 1, gc::AbilityKind::planet_destruction};
    intent.ability = prepare;
    auto command = input.command(intent, planets);
    assert(command && std::get<gc::PrepareGalacticAbility>(*command) == prepare);
    gc::PreparedAbility capability;
    capability.actor = 50; capability.planet = 1; capability.preparation = 77;
    capability.kind = gc::AbilityKind::technology_theft; capability.choice = 4; capability.item = 99;
    capability.price = eawr::sim::math::Fixed::from_integer(321).value(); capability.owned = true;
    intent.ability = gc::ExecutePreparedAbility{capability};
    command = input.command(intent, planets);
    assert(command && std::get<gc::ExecutePreparedAbility>(*command).prepared == capability);
    intent.source = 51; assert(!input.command(intent, planets)); intent.source = 50;
    intent.planet = 3; assert(!input.command(intent, planets)); intent.planet = 1;
    auto undiscovered = planets; undiscovered.front().discovered = false;
    assert(!input.command(intent, undiscovered));
    input.gates.selection_locked = true; assert(!input.command(intent, planets)); input.gates.selection_locked = false;
    input.gates.action_menu = true; assert(!input.command(intent, planets)); input.gates.action_menu = false;
    input.gates.hud_enabled = false; assert(!input.command(intent, planets)); input.gates.hud_enabled = true;
    input.gates.tutorial_locked = true; assert(!input.command(intent, planets)); input.gates.tutorial_locked = false;
    input.gates.active_mode = false; assert(!input.command(intent, planets)); input.gates.active_mode = true;
    input.gates.galactic = false; assert(!input.command(intent, planets)); input.gates.galactic = true;
    intent.ability = gc::CancelGalacticAbility{50, 77};
    command = input.command(intent, undiscovered); // Cancellation remains possible after discovery is lost.
    assert(command && std::get<gc::CancelGalacticAbility>(*command) == (gc::CancelGalacticAbility{50, 77}));
    intent.source = 51; assert(!input.command(intent, planets)); intent.source = 50;
    intent.ability = gc::CancelGalacticAbility{50, 0}; assert(!input.command(intent, planets));
    intent.ability.reset(); assert(!input.command(intent, planets));
}
// WGM-83/101 and WGUI-25: movement retains its destination actor, while an escort permits ordinary travel.
void probe_move_and_ground_admission() {
    namespace gc = eawr::sim::galactic;
    using eawr::sim::math::Fixed;
    gc::FleetControlSetup fleet_control;
    fleet_control.types = {{8, false, false, false}, {9, false, true, false}, {10, true, true, false}};
    gc::PoliticalControlSetup control;
    control.ground_capability_types = {{8, Fixed::from_integer(1).value(), false, true},
        {9, Fixed::from_integer(1).value(), false, false}, {10, {}, true, false}};
    auto movement = std::make_shared<gc::MovementSetup>();
    movement->types = {{8}, {9}, {10}}; movement->types[2].transport = true;
    movement->contents = {{30, {{31, 8}}}};
    std::vector<gc::Fleet> fleets{{10, 1, 1, {{30, 10}}}};
    const auto snapshot = [&] {
        return gc::GalacticSnapshot(0, {}, {}, {{1, {}, 1}, {2, {}, 2}}, {}, fleets, {},
            std::nullopt, fleet_control, movement, std::nullopt, {}, std::nullopt, std::nullopt, control);
    };
    const gc::MoveFleet move{10, {1, 2}};
    const AbilityDrop spy{31, 2, gc::AbilityKind::system_spy, 30};
    const auto prepared = ability_move_order(snapshot(), 1, move, spy);
    assert(prepared && prepared->destination_actor == 30 && prepared->path == move.path);
    assert(!ability_move_order(snapshot(), 1, move, std::nullopt));
    auto raid = move; raid.raid = true;
    assert(ability_move_order(snapshot(), 1, raid, std::nullopt) == raid);
    assert(!ability_move_order(snapshot(), 2, move, spy));
    auto wrong = spy; wrong.planet = 1;
    assert(!ability_move_order(snapshot(), 1, move, wrong));
    assert(!fleet_can_land(snapshot(), 1, 10));
    movement->contents[0].children.push_back({32, 9});
    const auto escorted = ability_move_order(snapshot(), 1, move, spy);
    assert(escorted && escorted->destination_actor == 0);
    assert(fleet_can_land(snapshot(), 1, 10));
    assert(!fleet_can_land(snapshot(), 2, 10));
    movement->contents[0].children.clear();
    assert(!fleet_can_land(snapshot(), 1, 10));
    movement->contents[0].children = {{31, 8}};
    movement->contents.push_back({40, {{41, 9}}});
    fleets.push_back({11, 1, 1, {{40, 10}}});
    const std::array<ObjectId, 2> stack{10, 11};
    const auto stacked = ability_move_order(snapshot(), 1, move, spy, stack);
    assert(stacked && stacked->destination_actor == 0);
    assert(fleet_can_land(snapshot(), 1, 10, stack));
    const DropIntent intent{DropKind::move, 10, 0, 2, {1, 2}};
    const auto orders = fleet_stack_orders(snapshot(), 1, intent, stack, move,
        [&](ObjectId, ObjectId) { return std::optional{spy}; });
    assert(orders && orders->size() == 2 && std::get<gc::MergeFleets>((*orders)[0]) == (gc::MergeFleets{11, 10}));
    assert(std::get<gc::MoveFleet>((*orders)[1]).destination_actor == 0);
    const auto own_orders = fleet_stack_orders(snapshot(), 1, intent, stack, move, {});
    assert(own_orders && std::get<gc::MoveFleet>(own_orders->back()).destination_actor == 0);
    fleets.back().owner = 2;
    assert(!ability_move_order(snapshot(), 1, move, spy, stack));
    assert(!fleet_can_land(snapshot(), 1, 10, stack));
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
    auto intent = fleet_destination_intent(*snapshot, 1, 10, 3, visible, {});
    assert(intent && intent->kind == DropKind::move && (intent->path == std::vector<ObjectId>{1, 2, 3}));
    MapInput input; const auto command = input.command(*intent, visible);
    assert(command && std::get<gc::MoveFleet>(*command).fleet == 10);
    assert(!fleet_destination_intent(*snapshot, 1, 11, 1, visible, {})); // Enemy selection cannot command.
    assert(!fleet_destination_intent(*snapshot, 1, 999, 3, visible, {})); // Stale selection.
    visible[1].discovered = false;
    assert(!fleet_destination_intent(*snapshot, 1, 10, 3, visible, {}));
    visible[1].discovered = true;
    visible[2].discovered = false;
    assert(!fleet_destination_intent(*snapshot, 1, 10, 3, visible, {}));
    visible[2].discovered = true;
    {
        // WGM-18/GCMP-24: the saved multiplayer flag omits the reveal gate, so a hidden hop no longer stops the route.
        auto multiplayer = setup; multiplayer.victory.emplace(); multiplayer.victory->context.multiplayer = true;
        for (const auto& player : setup.players) {
            gc::VictoryPlayer row; row.player = player.player_id; row.faction = "Faction"; row.playable = true;
            multiplayer.victory->context.players.push_back(row);
        }
        for (const auto& planet : setup.planets) {
            gc::VictoryPlanet row; row.planet = planet.id; row.type = planet.id; row.current = row.target = row.owner = planet.owner;
            multiplayer.victory->context.planets.push_back(row);
        }
        const auto mp_created = gc::GalacticSession::create(multiplayer); assert(mp_created);
        const auto mp_snapshot = mp_created.value().snapshot();
        visible[1].discovered = false;
        assert(!fleet_destination_intent(*snapshot, 1, 10, 3, visible, {})); // solo: hidden hop stops the route
        const auto through = fleet_destination_intent(*mp_snapshot, 1, 10, 3, visible, {});
        assert(through && (through->path == std::vector<ObjectId>{1, 2, 3}));
        visible[1].discovered = true;
    }
    setup.movement->access[1].potential_conflict = true;
    created = gc::GalacticSession::create(setup); assert(created); snapshot = created.value().snapshot();
    assert(!fleet_destination_intent(*snapshot, 1, 10, 3, visible, {})); // Hostile intermediate stops route.
    intent = fleet_destination_intent(*snapshot, 1, 10, 2, visible, {});
    assert(intent && (intent->path == std::vector<ObjectId>{1, 2})); // Hostile destination is admitted.
    intent = fleet_destination_intent(*snapshot, 1, 10, 1, visible, {});
    assert(intent && intent->kind == DropKind::land && std::holds_alternative<gc::LandFleet>(*input.command(*intent, visible)));
    setup.fleets.front().motion.transit.emplace();
    // Transit state is inspected through a published value; no command can
    // redirect an already travelling fleet through this preview binding.
    gc::GalacticSnapshot travelling(0, {}, setup.players, setup.planets, setup.routes, setup.fleets, {});
    assert(!fleet_destination_intent(travelling, 1, 10, 3, visible, {}));

    // #5093 / WGM-23: arrival at a defended planet must not strand a
    // combat-skipping fleet in the viewer's next route search.
    gc::NativeSnapshotChanges changes;
    changes.fleets = setup.fleets;
    changes.fleets->front().motion.transit.reset();
    changes.fleets->front().orbit = 3;
    gc::CensusObject hero;
    hero.id = 20; hero.current_type = hero.original_type = 1;
    hero.owner = 1; hero.planet = 3; hero.container = 10;
    hero.combat_role = gc::ActorCombatRole::ordinary;
    hero.abilities.present = hero.abilities.combat_skip = true;
    changes.actors = std::make_shared<const std::vector<gc::CensusObject>>(std::vector{hero});
    const auto arrived = gc::GalacticSnapshot::native_view(snapshot, changes);
    intent = fleet_destination_intent(*arrived, 1, 10, 1, visible, {});
    assert(intent && (intent->path == std::vector<ObjectId>{3, 2, 1}));
    const auto second_move = input.command(*intent, visible);
    assert(second_move && std::get<gc::MoveFleet>(*second_move).path == intent->path);
    auto sibling = hero; sibling.id = 40; sibling.container = 12;
    changes.fleets->push_back({12, 1, 3, {{40, 1}}});
    changes.actors = std::make_shared<const std::vector<gc::CensusObject>>(std::vector{hero, sibling});
    const auto together = gc::GalacticSnapshot::native_view(snapshot, changes);
    // Click-to-move selects the entire slot card, just like a card drag. Resolve
    // its native fleet identities before admission, never after the path search.
    const std::array<std::uint8_t, 1> slots{changes.fleets->front().orbit_slot};
    const auto stack = stack_fleets(*together, 1, 3, slots);
    assert((stack == std::vector<ObjectId>{10, 12}));
    assert(fleet_destination_intent(*together, 1, 10, 1, visible, stack));
    sibling.abilities.combat_skip = false;
    changes.actors = std::make_shared<const std::vector<gc::CensusObject>>(std::vector{hero, sibling});
    const auto mixed_card = gc::GalacticSnapshot::native_view(snapshot, changes);
    assert(fleet_destination_intent(*mixed_card, 1, 10, 1, visible, {})); // Individually selected Boba can leave.
    assert(!fleet_destination_intent(*mixed_card, 1, 10, 1, visible, stack)); // Whole card, preview and drop cannot.
    changes.fleets->pop_back();
    hero.abilities.combat_skip = false;
    changes.actors = std::make_shared<const std::vector<gc::CensusObject>>(std::vector{hero});
    assert(!fleet_destination_intent(*gc::GalacticSnapshot::native_view(snapshot, changes), 1, 10, 1, visible, {}));
    // A transport's own flag cannot grant skipping: its live cargo owns it.
    hero.combat_role = gc::ActorCombatRole::transport;
    auto cargo = hero; cargo.id = 30; cargo.container = 20;
    cargo.combat_role = gc::ActorCombatRole::ordinary;
    cargo.abilities.combat_skip = true;
    auto nested = std::make_shared<gc::MovementSetup>(*snapshot->movement());
    nested->contents = {{20, {{30, 1}}}};
    changes.movement = nested;
    changes.actors = std::make_shared<const std::vector<gc::CensusObject>>(std::vector{hero, cargo});
    assert(fleet_destination_intent(*gc::GalacticSnapshot::native_view(snapshot, changes), 1, 10, 1, visible, {}));
    auto companion = cargo; companion.id = 31; companion.abilities.combat_skip = false;
    nested->contents.front().children.push_back({31, 1});
    changes.actors = std::make_shared<const std::vector<gc::CensusObject>>(std::vector{hero, cargo, companion});
    assert(!fleet_destination_intent(*gc::GalacticSnapshot::native_view(snapshot, changes), 1, 10, 1, visible, {}));
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
    auto intent = fleet_destination_intent(*snapshot, 1, 12, 2, visible, {});
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
    observer.human_observers = {1}; observer.solo_reference_human = 1;
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
// WGUI-05/60/62: the per-planet alternate and the per-surface draw decision.
void undiscovered_planets_show_only_the_grid_shell() {
    PlanetShellInput art;
    art.galaxy_planet = true; art.has_shell = true; art.world_layer = true;
    art.camera_target = true; art.camera_alternate = 1;
    // Undiscovered: alternate 2 whatever the camera does, in the world layer and in a secondary scene layer.
    assert(planet_alternate(art) == 2);
    art.world_layer = false;
    assert(planet_alternate(art) == 2);
    // The shell surface draws, the camera alternates and the plain model surfaces do not; untagged surfaces always do.
    assert(surface_drawn(2, 2) && !surface_drawn(0, 2) && !surface_drawn(1, 2) && surface_drawn(-1, 2));
    // Discovered mid-game: the same planet falls back to the camera rule and the shell disappears.
    art.discovered = true;
    assert(planet_alternate(art) == 0 && !surface_drawn(2, planet_alternate(art)) && surface_drawn(0, planet_alternate(art)));
    art.world_layer = true;
    assert(planet_alternate(art) == 1 && surface_drawn(1, 1) && !surface_drawn(0, 1));
    art.camera_target = false;
    assert(planet_alternate(art) == 0);
    // A model with no alternate-2 surface (mod planet): undiscovered keeps the old rule, so it never vanishes.
    PlanetShellInput plain;
    plain.galaxy_planet = true; plain.world_layer = true;
    assert(planet_alternate(plain) == 0 && surface_drawn(0, planet_alternate(plain)));
    plain.camera_target = true; plain.camera_alternate = 1;
    assert(planet_alternate(plain) == 1 && surface_drawn(1, planet_alternate(plain)));
    // Scene-only art never takes the shell, and non-planet objects (fleets, cursor, station) stay on alternate 0.
    PlanetShellInput scene;
    scene.galaxy_planet = true; scene.scene_art = true; scene.has_shell = true; scene.world_layer = true;
    assert(planet_alternate(scene) == 0);
    assert(planet_alternate(PlanetShellInput{}) == 0);
}
// WGUI-05: the decision site of the grid shell, over a model's surface list and across publications.
struct TestSurface { std::string status; int alternate; };
void shell_decisions_follow_models_and_discovery() {
    const std::vector<TestSurface> stock{{"drawn", -1}, {"drawn", 0}, {"drawn", 1}, {"drawn", 2}};
    const std::vector<TestSurface> mod{{"drawn", 0}, {"drawn", 1}, {"drawn", -1}};
    const std::vector<TestSurface> skipped{{"drawn", 0}, {"skipped", 2}};
    assert(has_shell_surface(stock));
    assert(!has_shell_surface(mod));         // no alternate-2 surface at all
    assert(!has_shell_surface(skipped));     // an undrawn alternate-2 surface is no shell
    assert(!has_shell_surface(std::vector<TestSurface>{}));
    // A mod planet with no shell stays visible while undiscovered: every surface it authors still draws.
    PlanetShellInput plain;
    plain.galaxy_planet = true; plain.world_layer = true; plain.has_shell = has_shell_surface(mod);
    const int desired = planet_alternate(plain);
    assert(surface_drawn(0, desired) && surface_drawn(-1, desired));
    // Publication 1: planet 103 undiscovered (grid shell, and it is listed). Publication 2: it is discovered.
    const std::vector<ObjectId> galaxy{101, 102, 103, 900};
    const std::vector<ObjectId> scene_art{900};
    std::set<ObjectId> known{101, 102};
    const auto listed = [&] {
        return undiscovered_planets(galaxy, scene_art, [&](ObjectId id) { return known.contains(id); });
    };
    const auto before = listed();
    assert(before == std::vector<ObjectId>{103}); // scene art never takes the shell
    PlanetShellInput art;
    art.galaxy_planet = true; art.world_layer = true; art.has_shell = has_shell_surface(stock);
    art.discovered = known.contains(103);
    assert(planet_alternate(art) == 2);
    for (const auto& surface : stock) assert(surface_drawn(surface.alternate, 2) == (surface.alternate == 2 || surface.alternate < 0));
    known.insert(103); // discovered mid-game
    const auto after = listed();
    assert(after.empty() && after != before); // the changed list is what makes the HUD resubmit
    art.discovered = known.contains(103);
    assert(planet_alternate(art) == 0);
    for (const auto& surface : stock) assert(surface_drawn(surface.alternate, 0) == (surface.alternate == 0 || surface.alternate < 0));
    assert(!surface_drawn(2, planet_alternate(art))); // the shell surface is gone
}
// WGUI-175: the three orbit panels, friendly and with another owner's fleet present.
eawr::sim::galactic::ObservedFleet panel_card(eawr::sim::galactic::PlayerId owner, bool local,
    int slot, std::vector<std::pair<std::optional<std::uint64_t>, std::size_t>> groups,
    std::uint64_t fleet) {
    eawr::sim::galactic::ObservedFleet card;
    card.fleet = fleet; card.planet = 7; card.owner = owner; card.local = local;
    if (slot >= 0) card.orbit_slot = static_cast<std::uint8_t>(slot);
    for (const auto& [type, count] : groups) {
        eawr::sim::galactic::ObservedFleetGroup group;
        group.count = count; group.type = type; group.fleet = fleet;
        card.groups.push_back(group); card.count += count;
    }
    return card;
}
void fleet_panels_follow_the_retail_layouts() {
    using eawr::sim::galactic::ObservedFleet;
    const auto local0 = panel_card(1, true, 0, {{101, 3}, {102, 1}}, 10);
    const auto local1 = panel_card(1, true, 1, {{103, 2}}, 11);
    const auto local2 = panel_card(1, true, 2, {{101, 4}}, 12);
    const auto enemy = panel_card(2, false, -1, {{201, 11}, {202, 11}, {203, 11}}, 20);
    const auto second_enemy = panel_card(2, false, -1, {{201, 5}, {std::nullopt, 2}}, 21);
    const auto plan = [](std::initializer_list<const ObservedFleet*> cards, bool aware, std::size_t capacity = 12) {
        const std::vector<const ObservedFleet*> list(cards);
        return plan_fleet_panels(list, aware, capacity);
    };
    // Friendly layout: each local fleet in the panel of its orbit slot, the other panels empty.
    auto friendly = plan({&local0, &local1}, false);
    assert(friendly.mode == FleetPanelMode::friendly);
    assert(friendly.panel[0] && friendly.panel[0]->groups.size() == 2 && friendly.panel[1] && !friendly.panel[2]);
    assert(friendly.drop_slot[0] == 0 && friendly.drop_slot[1] == 1 && friendly.drop_slot[2] == 2);
    assert(!friendly.first_other && friendly.other_count == 0);
    // Another owner's aware fleet: it takes panel 1 (merged by type), slot 0 moves to panel 3, slot 1 stays.
    auto hostile = plan({&local0, &local1, &enemy}, false);
    assert(hostile.mode == FleetPanelMode::enemy);
    assert(hostile.panel[0] && !hostile.panel[0]->local && hostile.panel[0]->groups.size() == 3);
    assert(hostile.panel[0]->groups[0].count == 11 && hostile.panel[0]->groups[0].type == 201);
    assert(hostile.panel[1] && hostile.panel[1]->groups.size() == 1 && hostile.panel[1]->orbit_slot == 1);
    assert(hostile.panel[2] && hostile.panel[2]->orbit_slot == 0 && hostile.panel[2]->groups.size() == 2);
    assert(!hostile.drop_slot[0] && hostile.drop_slot[1] == 1 && hostile.drop_slot[2] == 0);
    assert(hostile.first_other == &enemy && hostile.other_count == 33);
    // Presence alone (no card) also flips the layout; the other panel stays empty.
    auto presence = plan({&local0}, true);
    assert(presence.mode == FleetPanelMode::enemy && !presence.panel[0] && presence.panel[2] && presence.panel[2]->orbit_slot == 0);
    // Several other fleets merge: same type sums, count-only groups stay separate.
    auto crowd = plan({&enemy, &second_enemy}, false);
    assert(crowd.panel[0]->groups.size() == 4 && crowd.panel[0]->groups[0].count == 16 && crowd.panel[0]->count == 40);
    assert(!crowd.panel[0]->groups.back().type && crowd.panel[0]->groups.back().count == 2);
    // Slots 0 and 2 share panel 3: types sum, and the card names no single slot (no drag from a mixed card).
    auto shared = plan({&local0, &local2, &enemy}, false);
    assert(shared.panel[2]->groups.size() == 2 && shared.panel[2]->groups[0].count == 7 && !shared.panel[2]->orbit_slot);
    // The panel capacity bounds the merged groups.
    auto capped = plan({&enemy}, false, 2);
    assert(capped.panel[0]->groups.size() == 2);
    assert(fleet_panel_for_slot(FleetPanelMode::friendly, 2) == 2 && fleet_panel_for_slot(FleetPanelMode::enemy, 0) == 2
        && fleet_panel_for_slot(FleetPanelMode::enemy, 1) == 1 && fleet_panel_for_slot(FleetPanelMode::enemy, 2) == 2);
}
}
// WGUI-72..74: the live campaign supplies one display trait per type from the authored object. Without a trait an
// enemy group is a bare count, so deleting this fill hides every enemy unit icon (owner capture 2026-10-16).
void live_fleet_traits_follow_the_authored_art() {
    struct Row { bool hero{}, named_hero{}, force_sensitive{}; std::optional<float> max_health; };
    const std::map<std::string, Row> art{{"Star_Destroyer", {false, false, false, 1000.0F}},
        {"Darth_Vader", {true, true, true, std::nullopt}}, {"Generic_Hero", {true, false, false, 250.0F}},
        {"Tartan_Patrol_Cruiser", {false, false, false, 400.0F}}};
    const std::map<eawr::sim::galactic::TypeId, std::string> names{{1, "Star_Destroyer"}, {2, "Darth_Vader"},
        {3, "Tartan_Patrol_Cruiser"}, {4, "Unknown_Type"}, {5, "Generic_Hero"}};
    const auto traits = eawr::viewer::galactic::live_fleet_display_traits(names, art);
    assert(traits.size() == 4); // one per type with authored art, in type order; a type without art gets none
    assert(traits[0].type == 1 && !traits[0].hero && !traits[0].named_hero && !traits[0].force_sensitive);
    assert(traits[0].damage && traits[0].damage->raw() == 1000 * eawr::sim::math::Fixed::scale);
    assert(traits[1].type == 2 && traits[1].hero && traits[1].named_hero && traits[1].force_sensitive && !traits[1].damage);
    assert(traits[2].type == 3 && traits[2].damage && traits[2].damage->raw() == 400 * eawr::sim::math::Fixed::scale);
    assert(traits[3].type == 5 && traits[3].hero && !traits[3].named_hero && !traits[3].force_sensitive);
    assert(eawr::viewer::galactic::live_fleet_display_traits(names, std::map<std::string, Row>{}).empty());
}
void hero_map_collection_is_local_stable_and_live() {
    namespace gc = eawr::sim::galactic;
    struct Look { bool named_hero{}; std::string icon; bool hero{named_hero}; std::int64_t hero_rank{25}; };
    const std::map<gc::TypeId, std::string> names{{7, "hero"}, {8, "ship"}, {9, "generic"},
        {10, "no_icon"}, {11, "second_hero"}};
    const std::map<std::string, Look> art{{"hero", {true, "hero.tga"}}, {"ship", {false, "ship.tga"}},
        {"generic", {false, "generic.tga", true}}, {"no_icon", {true, ""}}, {"second_hero", {true, "second.tga"}}};
    const auto base = std::make_shared<const gc::GalacticSnapshot>(0, gc::GalacticClock{},
        std::vector<gc::GalacticPlayer>{}, std::vector<gc::Planet>{{1, {}, 1}, {3, {}, 2}},
        std::vector<gc::Route>{}, std::vector<gc::Fleet>{{10, 1, 1, {{30, 8}}}}, std::vector<gc::GalacticEvent>{});
    std::vector<gc::CensusObject> actors{{20, 7, 2, 3, 3, false, false, 7},
        {21, 11, 1, 1, 1, false, false, 11}, {22, 7, 1, 1, 10, false, false, 7},
        {23, 7, 1, 1, 1, false, false, 7}, {24, 9, 1, 1, 1, false, false, 9},
        {25, 10, 1, 1, 1, false, false, 10}, {26, 11, 1, 1, 22, false, false, 11},
        {27, 11, 1, 1, 0, false, false, 11}, {28, 11, 1, 1, 24, false, false, 11}, {30, 8, 1, 1, 10, false, false, 8}};
    const auto snapshot = [&] {
        gc::NativeSnapshotChanges changes;
        changes.actors = std::make_shared<const std::vector<gc::CensusObject>>(actors);
        return gc::GalacticSnapshot::native_view(base, std::move(changes));
    };
    const auto collect = [&] { return collect_map_heroes(*snapshot(), 1, names, art); };
    auto entries = collect();
    assert((entries == std::vector<HeroMapEntry>{{7, 22, 1}, {11, 21, 1}}));
    assert(collect() == entries); // Stable type order, independent of first-seen type.
    const std::array pending{gc::HeroReturnView{7, 1, 1, 0, 100}};
    assert(collect_map_heroes(*snapshot(), 1, names, art, pending) == entries); // Live entry wins a duplicate schedule.
    auto ranked_art = art;
    ranked_art.at("hero").hero_rank = 10;
    ranked_art.at("second_hero").hero_rank = 90;
    const auto ranked = collect_map_heroes(*snapshot(), 1, names, ranked_art);
    assert((ranked == std::vector<HeroMapEntry>{{11, 21, 1, 90}, {7, 22, 1, 10}}));
    assert(collect_map_heroes(*snapshot(), 3, names, art).empty());
    actors[2].dead = true; actors[3].delete_pending = true;
    assert((collect() == std::vector<HeroMapEntry>{{11, 21, 1}}));
    actors[1].dead = true;
    assert(collect().empty()); // Children of named or generic heroes never supply another card.
    actors[2].dead = false; actors[3].delete_pending = false;
    gc::NativeSnapshotChanges travelling;
    travelling.actors = std::make_shared<const std::vector<gc::CensusObject>>(actors);
    travelling.fleets = std::vector<gc::Fleet>{{10, 1, 1, {{30, 8}}}};
    travelling.fleets->front().motion.transit.emplace();
    const auto transit = collect_map_heroes(*gc::GalacticSnapshot::native_view(base, std::move(travelling)), 1, names, art);
    assert((transit == std::vector<HeroMapEntry>{{7, 23, 1}})); // Parked duplicate remains; no transit destination invented.
}
void scheduled_hero_map_uses_published_frames() {
    namespace gc = eawr::sim::galactic;
    struct Look { bool named_hero{}; std::string icon; bool hero{named_hero}; std::int64_t hero_rank{25}; };
    const std::map<gc::TypeId, std::string> names{{7, "hero"}, {8, "foreign"}, {9, "generic"}, {10, "no_icon"}};
    const std::map<std::string, Look> art{{"hero", {true, "hero.tga", true, 90}},
        {"foreign", {true, "foreign.tga"}}, {"generic", {false, "generic.tga", true}}, {"no_icon", {true, ""}}};
    const std::vector<gc::HeroReturnView> returns{{7, 1, 1, 10, 50}, {7, 1, 1, 20, 70},
        {8, 2, 3, 10, 50}, {9, 1, 1, 10, 50}, {10, 1, 1, 10, 50}};
    const auto collect = [&](std::uint64_t frame) {
        gc::GalacticClock clock; clock.galactic_frame = frame;
        const gc::GalacticSnapshot snapshot(0, clock, {}, {{1, {}, 1}, {3, {}, 2}}, {}, {}, {});
        return collect_map_heroes(snapshot, 1, names, art, returns);
    };
    const auto entries = collect(20);
    assert(entries.size() == 1 && entries[0].scheduled && entries[0].actor == 0);
    assert(entries[0].rank == 90 && entries[0].start_frame == 10 && entries[0].deadline == 50);
    assert(entries[0].completion == 0.25);
    assert(collect(9)[0].completion == 0.0 && collect(40)[0].completion == 0.75);
    assert(collect(50)[0].completion == 1.0 && collect(51)[0].completion == 0.0);
}
void space_proof_selects_reachable_authored_defenders() {
    namespace gc = eawr::sim::galactic;
    const auto fixed = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    std::vector<gc::Planet> worlds{{1, {}, 1}, {2, {fixed(100), {}, {}}, 1},
        {3, {fixed(200), {}, {}}, 2}, {4, {fixed(100), fixed(100), {}}, 2},
        {5, {fixed(1000), {}, {}}, 2}};
    std::vector<gc::Route> routes{{101, 1, 2, fixed(1)}, {102, 2, 3, fixed(1)}, {103, 2, 4, fixed(1)}};
    std::vector<gc::Fleet> fleets{{11, 1, 1, {{21, 1}}}, {10, 1, 1, {{20, 1}}},
        {12, 2, 3, {{22, 1}}}, {13, 2, 5, {{23, 1}}}};
    gc::MovementSetup movement;
    for (const auto& world : worlds) movement.access.push_back({1, world.id, true, world.owner == 2});
    gc::ProductionSetup production;
    gc::PoliticalControlSetup control;
    gc::PlanetProduction station; station.planet = 4; station.starbase_owner = 2; station.starbase_type = 9;
    production.planets.push_back(station);
    const auto select = [&] {
        const gc::GalacticSnapshot snapshot(0, {}, {}, worlds, routes, fleets, {}, fixed(1), std::nullopt,
            std::make_shared<const gc::MovementSetup>(movement), std::nullopt, {}, std::nullopt, production, control);
        return space_proof_route(snapshot, 1, 3);
    };
    auto chosen = select();
    // Equal path costs: lowest destination, then fleet identity, independent of input fleet order.
    assert(chosen && chosen->fleet == 10 && (chosen->path == std::vector<gc::PlanetId>{1, 2, 3}));
    routes[2].hs_speed_factor = fixed(2);
    chosen = select();
    assert(chosen && chosen->path.back() == 4); // Cost beats the lower destination identity.
    routes[2].hs_speed_factor = fixed(1);
    fleets.erase(fleets.begin() + 2);
    chosen = select();
    assert(chosen && (chosen->path == std::vector<gc::PlanetId>{1, 2, 4})); // Station, no hostile fleet.
    production.planets[0].starbase_type = 0;
    gc::ProductionObject object; object.id = 30; object.owner = 2;
    object.domain = gc::ProductionDomain::space; object.kind = gc::ProductionKind::structure;
    production.planets[0].objects.push_back(object);
    assert(select()); // Authored orbital defender is also sufficient.
    production.planets[0].objects[0].domain = gc::ProductionDomain::ground;
    assert(!select()); // Ground defenders and the disconnected fleet do not yield space battles.
    production.planets[0].objects[0].domain = gc::ProductionDomain::space;
    production.planets[0].objects[0].kind = gc::ProductionKind::technology;
    assert(!select());
    production.planets[0].objects[0].kind = gc::ProductionKind::structure;
    movement.access[1].accessible = false;
    assert(!select());
    movement.access[1].accessible = true;
    movement.access[1].potential_conflict = true;
    assert(!select()); // The ordinary pathfinder cannot pass through a defended intermediate world.
    movement.access[1].potential_conflict = false;
    worlds[3].owner = 3; production.planets[0].objects[0].owner = 3;
    assert(!select()); // Neutral is not an attack target.
    worlds[3].owner = 2; production.planets[0].objects[0].owner = 2;
    fleets[0].motion.transit.emplace(); fleets[1].members.clear();
    assert(!select()); // Only parked, populated local fleets may be selected.
    fleets[0].motion.transit.reset(); fleets[1].members.push_back({20, 1});
    worlds[1].owner = 2; fleets.push_back({14, 2, 2, {{24, 1}}});
    chosen = select();
    assert(chosen && chosen->fleet == 11 && (chosen->path == std::vector<gc::PlanetId>{1, 2}));
    // The established adjacent-fleet case retains its original fleet/route ordering.
    control.relations.push_back({1, 2, true});
    chosen = select();
    assert(chosen && chosen->fleet == 11 && chosen->path.back() == 2); // Legacy predicate includes allies.
    worlds[1].owner = 0; fleets.back().owner = 0;
    chosen = select();
    assert(chosen && chosen->fleet == 11 && chosen->path.back() == 2); // And an owner-zero opposing fleet.
    fleets.pop_back(); worlds[1].owner = 1;
    assert(!select()); // Fallback still excludes allied destinations.
    control.relations.clear();
    movement.access.clear();
    assert(!select()); // Missing published access never invents presence or admits a route.
}
void surface_world_orbit_targets() {
    namespace gc = eawr::sim::galactic;
    std::vector<gc::Fleet> fleets{{10, 1, 1, {}}, {11, 2, 1, {}}, {12, 1, 3, {}}, {13, 1, 1, {}}};
    fleets.back().motion.transit.emplace();
    const gc::GalacticSnapshot snapshot(0, {}, {}, {}, {}, fleets, {});
    MapInput input;
    const auto target = surface_orbit_target(snapshot, 1, 1, 1, 1, 10, std::uint8_t{2});
    assert(target && target->fleet == 10 && target->slot == 0);
    const auto command = input.command({DropKind::launch, 50, target->fleet, 1, {}, target->slot}, planets);
    assert(command && std::get<gc::LaunchFromSurface>(*command).target == 10);
    for (std::uint8_t slot = 0; slot < 3; ++slot) {
        const auto empty = surface_orbit_target(snapshot, 1, 1, 1, 1, 0, slot);
        assert(empty && !empty->fleet && empty->slot == slot);
    }
    for (const auto fleet : {11U, 12U, 13U, 99U})
        assert(!surface_orbit_target(snapshot, 1, 1, 1, 1, fleet, std::uint8_t{0}));
    // WGM-199: another planet's badges/empty slots defer to world routing,
    // even when their fleet is foreign or stale. They never become source-orbit targets.
    for (const auto fleet : {0U, 11U, 12U, 99U}) {
        const auto neighbour = surface_orbit_target(snapshot, 1, 1, 3, 2, fleet, std::uint8_t{2});
        assert(neighbour && !neighbour->fleet && neighbour->slot == 0);
        const auto move = input.command({DropKind::launch, 50, neighbour->fleet, 1, {1, 3}, 2}, planets);
        assert(move && std::holds_alternative<gc::LaunchFleet>(*move));
        assert((std::get<gc::LaunchFleet>(*move).path == std::vector<ObjectId>{1, 3}));
    }
    assert(!surface_orbit_target(snapshot, 1, 1, 1, 2, 10, std::uint8_t{0}));
    assert(!surface_orbit_target(snapshot, 1, 1, 1, 1, 0, std::nullopt));
    assert(!surface_orbit_target(snapshot, 1, 1, 1, 1, 0, std::uint8_t{3}));
}
int main() {
    surface_world_orbit_targets();
    space_proof_selects_reachable_authored_defenders();
    scheduled_hero_map_uses_published_frames();
    hero_map_collection_is_local_stable_and_live();
    selection_and_caller_gates(); deferred_revalidation(); navigation_and_heroes();
    rectangle_and_widget_drop_order(); wheel_radar_and_typed_commands();
    inward_refocus_immediately_focuses();
    published_discovery_is_player_specific();
    undiscovered_planets_show_only_the_grid_shell();
    shell_decisions_follow_models_and_discovery();
    fleet_panels_follow_the_retail_layouts();
    live_fleet_traits_follow_the_authored_art();
    live_hero_containment_and_home();
    queued_ability_drop_is_scoped_to_preparation_and_owner();
    typed_ability_intents_preserve_capabilities();
    hero_fleet_drop_offers_authored_ability();
    probe_move_and_ground_admission();
    fleet_destination_uses_live_routes();
    dragged_card_moves_as_one_stack();
}
