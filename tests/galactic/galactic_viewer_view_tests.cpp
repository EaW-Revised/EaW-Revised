// The galactic mode's view model (apps/viewer/src/galactic/galaxy_view.cpp):
// my seat and my faction stay separate (ADR-020), ownership colours follow the
// faction, fleets group per faction in orbit, the overview frames the galaxy and
// panning stays inside the WGUI-58 box.
#include "galactic/galaxy_snapshot_stub.hpp"
#include "galactic/galaxy_view.hpp"
#include "eawr/presentation/ui/fleet_pathfinder.hpp"
#include "galactic/corruption_visuals.hpp"
#include "../presentation/galactic/fleet_hud_fixture.hpp"
#include "galactic/galactic_reveal.hpp"
#include "galactic/map_input.hpp"
#include "galactic/native_ai_flag.hpp"
#include "galactic/planet_hud.hpp"
#include "galactic/ground_stack.hpp"
#include "eawr/sim/galactic/ground_intelligence.hpp"
#include "eawr/sim/galactic/session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../presentation/galactic/ground_hud_fixture.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/presentation/galactic_pause.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>

using namespace eawr::viewer::galactic;

namespace {

[[nodiscard]] bool same(float a, float b);

void ggs_02_known_ground_counts_and_ggs_04_local_launches() {
    namespace gc = eawr::sim::galactic;
    for (const auto fixture : {"ground-contents", "ground-count", "ground-foreign", "ground-inaccessible"}) {
        auto setup = synthetic_live_setup();
        std::vector<gc::FleetDisplayType> traits;
        std::vector<gc::FleetDisplayVisibility> visibility;
        std::map<gc::TypeId, std::string> names;
        configure_ground_fixture(setup, 1, fixture, traits, visibility, names);
        const gc::GalacticSnapshot snapshot(0, {}, setup.players, setup.planets, setup.routes, setup.fleets,
            {}, {}, setup.fleet_control, {}, {}, {}, {}, {}, setup.control, {}, {}, setup.observer);
        const auto before = snapshot.canonical_bytes();
        const auto stack = ground_stack(snapshot, 1, 101, 6);
        assert(!ground_stack(snapshot, 1, 999, 6));
        assert(!ground_stack(snapshot, 99, 101, 6));
        assert(!ground_stack(snapshot, 1, 102, 6)); // Empty surface never draws a stack.
        if (std::string_view(fixture) == "ground-inaccessible") { assert(!stack); continue; }
        assert(stack && stack->count == 2 && stack->texture == 1); // Pending deletion omitted.
        if (std::string_view(fixture) == "ground-count") {
            // GGS-02 permits a known enemy count. GGS-03 withholds its typed contents;
            // GGS-04 never turns this count-only enemy badge into a local launch source.
            assert(!stack->contents_known && stack->companies.empty());
            assert(stack->contents.size() == 1 && !stack->contents.front().type);
            assert(ground_stack_units(*stack).empty());
        } else if (std::string_view(fixture) == "ground-foreign") {
            assert(stack->contents_known && ground_stack_units(*stack).empty());
        } else {
            assert(stack->contents_known && stack->contents.size() == 1 && stack->contents[0].count == 2);
            const auto units = ground_stack_units(*stack);
            assert((units == std::vector<ObjectId>{8001, 8002}));
            const auto launch = ground_stack_launch(snapshot, 1, 101, units, 601, 0);
            assert(launch && launch->kind == DropKind::launch && launch->members == units && launch->target == 601);
            MapInput input;
            const std::vector<InputPlanet> planets{{101, {}, 1, true}, {102, {}, 1, true}};
            const auto order = input.command(*launch, planets);
            assert(order && std::holds_alternative<gc::LaunchFromSurface>(*order));
            assert(std::get<gc::LaunchFromSurface>(*order).units == units);
            assert(ground_stack_launch(snapshot, 1, 101, units, 0, 2));
            assert(!ground_stack_launch(snapshot, 1, 102, units, 0, 0));
            assert(!ground_stack_launch(snapshot, 1, 101, units, 999, 0));
            assert(!ground_stack_launch(snapshot, 1, 101, units, 601, 1));
            assert(!ground_stack_launch(snapshot, 1, 101, units, 0, 3));
            assert(!ground_stack_launch(snapshot, 1, 101, std::vector<ObjectId>{8001, 8001}, 0, 0));
            assert(!ground_stack_launch(snapshot, 1, 101, std::vector<ObjectId>{8001, 8003}, 0, 0));
            const auto travel = ground_stack_launch(snapshot, 1, 101, units, 0, 2, {101, 102}, std::uint8_t{1});
            assert(travel && travel->path.back() == 102 && travel->destination_slot == 1);
            const auto move = input.command(*travel, planets);
            assert(move && std::holds_alternative<gc::LaunchFleet>(*move));
            assert(std::get<gc::LaunchFleet>(*move).units == units);
            const auto transfer = ground_stack_launch(snapshot, 1, 101, units, 0, 2, {101, 102}, {}, true);
            assert(transfer && transfer->land_on_arrival);
            const auto combined = input.command(*transfer, planets);
            assert(combined && std::holds_alternative<gc::TransferSurface>(*combined));
            assert((std::get<gc::TransferSurface>(*combined) == gc::TransferSurface{units, {101, 102}}));
            assert(!ground_stack_launch(snapshot, 1, 101, units, 0, 2, {}, {}, true));
            assert(!ground_stack_launch(snapshot, 1, 101, units, 0, 2, {101, 102}, std::uint8_t{1}, true));
            assert(!ground_stack_launch(snapshot, 1, 101, units, 0, 2, {101, 999}, {}, true));
        }
        assert(snapshot.canonical_bytes() == before);
    }
}

void empty_ground_slot_returns_only_while_ground_units_are_held() {
    namespace gc = eawr::sim::galactic;
    auto setup = synthetic_live_setup();
    configure_synthetic_hud_clock(setup);
    std::vector<gc::FleetDisplayType> traits;
    std::vector<gc::FleetDisplayVisibility> visibility;
    std::map<gc::TypeId, std::string> names;
    configure_ground_fixture(setup, 1, "ground-slot-return", traits, visibility, names);
    auto count_only = setup;
    for (auto& planet : count_only.control->planets) if (planet.planet == 101) planet.landed.clear();
    for (auto& planet : count_only.observer->planets) if (planet.planet == 101) planet.landed_transports.clear();
    for (auto& record : count_only.observer->records) if (record.player == 1 && record.planet == 101)
        record.mask &= ~gc::bit(gc::Intelligence::ground_contents);
    const gc::GalacticSnapshot empty_count(0, {}, count_only.players, count_only.planets, count_only.routes,
        count_only.fleets, {}, {}, count_only.fleet_control, {}, {}, {}, {}, {}, count_only.control,
        {}, {}, count_only.observer);
    const auto anonymous_target = ground_stack(empty_count, 1, 101, 6, true);
    assert(anonymous_target && !anonymous_target->contents_known && anonymous_target->contents.empty());
    eawr::platform::LiveGameSession::Options options;
    options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
    auto started = eawr::platform::LiveGameSession::start(setup, options);
    assert(started);
    auto game = std::move(started).value();
    const auto submit = [&](auto order, std::uint64_t tick) {
        game->submit(eawr::platform::LiveGameOrder{gc::SeatId{1}, order, {}});
        game->advance_to(tick);
        assert(game->wait_for(tick, std::chrono::seconds(5)));
        assert(game->rejected_orders().empty());
    };
    submit(gc::LaunchFromSurface{{8001, 8002}, 601, 0}, 1);
    const auto orbit = game->frame(gc::SeatId{1}).latest;
    assert(!ground_stack(*orbit, 1, 101, 6));
    assert(!ground_drop_stack(*orbit, 1, 101, 6, 0));
    assert(!ground_drop_stack(*orbit, 1, 101, 6, 602)); // Space-only fleet.
    const auto roster = ground_roster(*orbit, 1, 101);
    assert(roster.accessible);
    assert(roster.allied);
    assert(roster.contents_known || roster.count_known);
    assert(fleet_can_land(*orbit, 1, 601));
    const auto target = ground_drop_stack(*orbit, 1, 101, 6, 601);
    assert(target);
    assert(target->count == 0);
    assert(target->texture == 0);
    assert(target->contents.empty());
    assert(ground_stack_units(*target).empty()); // Receiver never becomes a launch source.
    const std::array<gc::FleetMemberRef, 1> space_member{{{601, 7003}}};
    const std::array<gc::FleetMemberRef, 1> ground_member{{{601, 8001}}};
    assert(!ground_drop_stack(*orbit, 1, 101, 6, 601, {}, false, space_member));
    assert(ground_drop_stack(*orbit, 1, 101, 6, 601, {}, false, ground_member));
    assert(!ground_drop_stack(*orbit, 1, 102, 6, 601)); // Same-planet landing only.
    assert(ground_drop_stack(*orbit, 1, 106, 6, 0, {}, true)); // Surface transfer.
    MapInput input;
    const std::vector<InputPlanet> planets{{101, {}, 1, true}};
    const DropIntent drop{DropKind::land, 601, 0, 101, {}, 0, {}, {}};
    const auto order = input.command(drop, planets);
    assert(order && fleet_stack_orders(*orbit, 1, drop, {}, *order, {}));
    submit(std::get<gc::LandFleet>(*order), 2);
    const auto returned = ground_stack(*game->frame(gc::SeatId{1}).latest, 1, 101, 6);
    assert(returned && returned->count >= 2);
    const auto units = ground_stack_units(*returned);
    assert(std::find(units.begin(), units.end(), 8001) != units.end());
    assert(std::find(units.begin(), units.end(), 8002) != units.end());
    game->stop();
}

void ggs_09_native_fixture_qualifies_all_live_combat_queries() {
    namespace gc = eawr::sim::galactic;
    auto setup = synthetic_live_setup();
    configure_synthetic_hud_clock(setup);
    std::vector<gc::FleetDisplayType> traits;
    std::vector<gc::FleetDisplayVisibility> visibility;
    std::map<gc::TypeId, std::string> names;
    configure_ground_fixture(setup, 1, "ground-representative", traits, visibility, names);
    auto created = gc::GalacticSession::create(setup);
    assert(created);
    const auto snapshot = created.value().snapshot();
    for (const auto& fleet : snapshot->fleets()) {
        assert(snapshot->fleet_combat_skip(fleet.id));
        for (const auto& member : fleet.members) assert(snapshot->actor_combat_skip(member.id));
    }
    for (const auto& planet : snapshot->control()->planets)
        for (const auto& force : planet.landed) assert(snapshot->actor_combat_skip(force.id));
    const auto stack = ground_stack(*snapshot, 1, 101, 6);
    assert(stack && stack->count == 2 && !stack->contents_known && stack->companies.empty());
}

// GGS-09: count-only art is a narrow permission, never a typed contents row.
void ggs_09_sanitized_ground_representatives() {
    namespace gc = eawr::sim::galactic;
    auto setup = synthetic_live_setup();
    std::vector<gc::FleetDisplayType> fleet_traits;
    std::vector<gc::FleetDisplayVisibility> visibility;
    std::map<gc::TypeId, std::string> names;
    configure_ground_fixture(setup, 1, "ground-count", fleet_traits, visibility, names);
    // Synthetic mod faction IDs deliberately share neither stock names nor ordinals.
    for (auto& player : setup.players) player.faction_id = player.player_id == 2 ? 707U : 909U;
    auto movement = std::make_shared<gc::MovementSetup>();
    movement->contents = {{8001, {{8102, 2002}, {8101, 2001}, {8103, 2003}}},
        {8002, {{8201, 2004}}}};
    auto actors = std::make_shared<std::vector<gc::CensusObject>>();
    actors->push_back({8001, 1000, 2, 101, 101, false, false, 1000});
    actors->push_back({8002, 1000, 2, 101, 101, false, false, 1000});
    actors->push_back({8101, 2001, 2, 101, 8001, false, false, 2001});
    actors->push_back({8102, 2002, 2, 101, 8001, false, false, 2002});
    actors->push_back({8103, 2003, 2, 101, 8001, false, true, 2003});
    actors->push_back({8201, 2004, 2, 101, 8002, false, false, 2004});
    std::map<gc::TypeId, gc::GroundDisplayType> traits{
        {1000, {1, false, false, true, 25}},
        {2001, {100, false, false, true, 25}},
        {2002, {200, false, false, true, 25}},
        {2003, {900, true, false, true, 0}},
        {2004, {150, false, false, true, 25}}};
    const auto display_traits = [&](gc::TypeId type) -> std::optional<gc::GroundDisplayType> {
        const auto at = traits.find(type);
        return at == traits.end() ? std::nullopt : std::optional{at->second};
    };
    const auto snapshot = [&] {
        gc::NativeSnapshotChanges changes;
        changes.movement = movement; changes.actors = actors;
        auto base = std::make_shared<const gc::GalacticSnapshot>(0, gc::GalacticClock{}, setup.players,
            setup.planets, setup.routes, setup.fleets, std::vector<gc::GalacticEvent>{},
            std::nullopt, setup.fleet_control, nullptr, std::nullopt, std::vector<gc::Seat>{},
            std::nullopt, std::nullopt, setup.control, std::vector<gc::PoliticalControlEffect>{},
            std::vector<gc::PopulationTotal>{}, setup.observer);
        return gc::GalacticSnapshot::native_view(base, std::move(changes));
    };
    const auto check = [&](gc::TypeId strongest, std::optional<gc::TypeId> hero) {
        const auto published = snapshot();
        const auto before = published->canonical_bytes();
        const auto rows = gc::observed_ground_display(*published, 1, display_traits);
        assert(rows.size() == 1 && rows.front().count == 2 && rows.front().owner == 2);
        assert(rows.front().strongest == strongest && rows.front().hero == hero);
        assert(rows.front().image(false) == strongest);
        assert(rows.front().image(true) == (hero ? hero : std::optional{strongest}));
        const auto stack = ground_stack(*published, 1, 101, 6);
        assert(stack && !stack->contents_known && stack->companies.empty());
        assert(stack->contents.size() == 1 && !stack->contents.front().type);
        assert(ground_stack_units(*stack).empty());
        assert(gc::observed_ground_display(*published, 2, display_traits).empty());
        assert(gc::observed_ground_display(*published, 99, display_traits).empty());
        assert(published->canonical_bytes() == before);
        return rows;
    };
    // A copied membership without its live actor row cannot disclose its type.
    const auto unpublished = actors->at(3);
    actors->erase(actors->begin() + 3);
    check(2004, {});
    actors->insert(actors->begin() + 3, unpublished);
    const auto roots_and_payload = *actors;
    actors->clear();
    const auto count_only = gc::observed_ground_display(*snapshot(), 1, display_traits);
    assert(count_only.size() == 1 && count_only.front().count == 2);
    assert(!count_only.front().strongest && !count_only.front().hero);
    *actors = roots_and_payload;
    const auto expected = check(2002, {}); // Live payload health, ignoring deleted high-health hero.
    const auto immutable = snapshot();
    const auto canonical = immutable->canonical_bytes();
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        std::vector<std::vector<gc::ObservedGroundDisplay>> results(16);
        assert(pool.execute(results.size(), [&](std::size_t index) {
            results[index] = gc::observed_ground_display(*immutable, 1, display_traits);
        }));
        for (const auto& result : results) assert(result == expected);
        assert(immutable->canonical_bytes() == canonical);
    }
    traits[2001].named_hero = true; traits[2001].hero_rank = 9;
    traits[2004].named_hero = true; traits[2004].hero_rank = 2;
    check(2004, 2004); // First transport's named hero takes its hero branch, not its heavy payload.
    traits[2001].health = 250;
    check(2001, 2004); // Filter preference differs from the health winner.
    traits[2004].hero_rank = 9;
    check(2001, 2001); // Equal rank retains the first transport's hero.
    traits[2004].health = 250;
    check(2004, 2001); // Equal hero health replaces, independently of rank.
    traits[2002].named_hero = true; traits[2002].hero_rank = 20;
    check(2004, 2004); // Within a transport membership order, not actor ID/rank, chooses the named hero.
    traits[2002].named_hero = false;
    traits[2001].named_hero = traits[2004].named_hero = false;
    traits[1000].generic_hero = true; traits[1000].health = 300;
    check(1000, 1000);
    traits[1000].generic_hero = false;
    traits[3000] = {400, true, false, true, -3};
    actors->push_back({8301, 3000, 2, 101, 8101, false, false, 3000});
    movement->contents.push_back({8101, {{8301, 3000}}});
    check(3000, 3000); // Recursively contained named hero, not a template member.
    actors->back().delete_pending = true;
    check(2001, {}); // Deleted nested hero is excluded; ordinary equal health retains first.
    (*actors)[0].combat_role = gc::ActorCombatRole::ordinary;
    (*actors)[0].current_type = 4000;
    traits[4000] = {500, false, false, true, 25};
    check(4000, {}); // Direct ordinary actors use their current type, not original company art.
    (*actors)[0].combat_role = gc::ActorCombatRole::fleet;
    check(1000, {}); // A fleet container's health branch uses its original type.
    for (auto& player : setup.observer->players) player.default_intelligence = 0;
    for (auto& row : setup.observer->records) row.mask = 0;
    assert(gc::observed_ground_display(*snapshot(), 1, display_traits).empty());
    for (auto& player : setup.observer->players) player.default_intelligence = gc::bit(gc::Intelligence::ground_count);
    for (auto& row : setup.observer->records) row.mask = gc::bit(gc::Intelligence::ground_count);
    for (auto& row : setup.control->planets) row.surface_accessible = false;
    assert(gc::observed_ground_display(*snapshot(), 1, display_traits).empty());
}

void pause_preview_matches_published_policy_and_holds() {
    namespace gc = eawr::sim::galactic;
    using Action = eawr::presentation::GalacticPausedAction;
    const auto setup = synthetic_live_setup();
    for (unsigned mask = 0; mask != 8; ++mask) for (const auto depth : {0U, 1U, 2U}) {
        gc::FleetControlSetup control;
        control.pause = {mask & 1 ? gc::PausedAdmission::admit : gc::PausedAdmission::refuse,
            mask & 2 ? gc::PausedAdmission::admit : gc::PausedAdmission::refuse,
            mask & 4 ? gc::PausedAdmission::admit : gc::PausedAdmission::refuse};
        gc::GalacticClock clock; clock.pause_depth = depth;
        const std::vector<gc::Seat> seats{{gc::SeatId{1}, 1}, {gc::SeatId{2}, 1, gc::SeatKind::ai},
            {gc::SeatId{3}, 1, gc::SeatKind::human, 0, false}};
        const gc::GalacticSnapshot snapshot(0, clock, setup.players, setup.planets, setup.routes,
            setup.fleets, {}, {}, control, {}, {}, seats);
        const auto before = snapshot.canonical_bytes();
        for (const bool hold : {false, true}) for (unsigned kind = 0; kind != 4; ++kind) {
            const auto action = static_cast<Action>(kind);
            const bool expected = depth == 0 || (!hold && kind < 3 && (mask & (1U << kind)));
            assert(eawr::presentation::galactic_pause_allows(snapshot, gc::SeatId{1}, action, hold) == expected);
            for (const auto issuer : {2U, 3U, 99U})
                assert(eawr::presentation::galactic_pause_allows(snapshot, gc::SeatId{issuer}, action, hold) == (depth == 0));
        }
        assert(snapshot.canonical_bytes() == before); // Presentation never writes replay state.
    }
    gc::GalacticClock clock; clock.pause_depth = 1;
    const gc::GalacticSnapshot no_control(0, clock, setup.players, setup.planets, setup.routes, setup.fleets, {});
    assert(!eawr::presentation::galactic_pause_allows(no_control, gc::SeatId{1}, Action::fleet_move, false));
}

void paused_departure_is_transit_at_route_start_until_resume() {
    namespace gc = eawr::sim::galactic;
    auto setup = synthetic_live_setup();
    std::vector<gc::FleetDisplayType> traits;
    std::vector<gc::FleetDisplayVisibility> visibility;
    std::map<gc::TypeId, std::string> names;
    configure_fleet_hud_fixture(setup, 1, "fleet-drag", traits, visibility, names);
    setup.fleet_control->pause = gc::PausePolicy::admit_all();
    eawr::platform::LiveGameSession::Options options;
    options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
    auto started = eawr::platform::LiveGameSession::start(setup, options);
    assert(started);
    auto game = std::move(started).value();
    const auto submit = [&](auto order, std::uint64_t tick) {
        game->submit(eawr::platform::LiveGameOrder{gc::SeatId{1}, order, {}});
        game->advance_to(tick);
        assert(game->wait_for(tick, std::chrono::seconds(5)));
    };
    submit(gc::PauseGalaxy{}, 1);
    submit(gc::MoveFleet{601, {101,102}, std::nullopt}, 2);
    assert(game->rejected_orders().empty());
    const auto paused = game->frame().latest;
    const auto fleet = std::find_if(paused->fleets().begin(), paused->fleets().end(), [](const auto& row) { return row.id == 601; });
    assert(fleet != paused->fleets().end() && !fleet->orbit && fleet->motion.transit);
    assert(eawr::presentation::galactic_departure_ready(*paused, *fleet));
    assert(paused->clock().galactic_frame == 0);
    const auto view = build_live_view(*paused, synthetic_galaxy(), 1, synthetic_neutral_owner,
        {}, {visibility, {}, false});
    assert(view.view);
    const auto marker = std::find_if(view.view->fleets.begin(), view.view->fleets.end(), [](const auto& row) { return row.fleet == 601; });
    assert(marker != view.view->fleets.end() && marker->transit && !marker->planet && marker->destination == 102);
    submit(gc::ResumeGalaxy{}, 3);
    // WGM-29 may spend the first service setting its heading, without translation.
    game->advance_to(4);
    assert(game->wait_for(4, std::chrono::seconds(5)));
    const auto resumed = game->frame().latest;
    const auto moving = std::find_if(resumed->fleets().begin(), resumed->fleets().end(), [](const auto& row) { return row.id == 601; });
    assert(moving != resumed->fleets().end());
    assert(!eawr::presentation::galactic_departure_ready(*resumed, *moving));
    assert(moving->motion.position != fleet->motion.position);
    submit(gc::PauseGalaxy{}, 5);
    const auto repaused = game->frame().latest;
    const auto underway = std::find_if(repaused->fleets().begin(), repaused->fleets().end(), [](const auto& row) { return row.id == 601; });
    assert(underway != repaused->fleets().end());
    assert(!eawr::presentation::galactic_departure_ready(*repaused, *underway));
    game->stop();
}

void planet_bonus_metadata_survives_view_projection() {
    auto metadata = synthetic_galaxy();
    metadata.planets.front().name = "ArbitraryModPlanet";
    metadata.planets.front().ability = {"KEY_A", "KEY_B", "mod_bonus.tga", "Authored title",
        "Authored body", "Popup title", "Popup body"};
    const auto view = build_view(metadata, 1);
    assert(view.view);
    const auto planet = std::find_if(view.view->planets.begin(), view.view->planets.end(),
        [&](const auto& row) { return row.id == metadata.planets.front().id; });
    assert(planet != view.view->planets.end());
    assert(planet->name == "ArbitraryModPlanet" && planet->ability.name_key == "KEY_A");
    assert(planet->ability.icon == "mod_bonus.tga" && planet->ability.title == "Popup title"
        && planet->ability.description == "Popup body");
    const auto setup = synthetic_live_setup();
    const eawr::sim::galactic::GalacticSnapshot snapshot(1, {}, setup.players, setup.planets,
        setup.routes, setup.fleets, {});
    const auto live = build_live_view(snapshot, metadata, 1, synthetic_neutral_owner);
    assert(live.view);
    const auto projected = std::find_if(live.view->planets.begin(), live.view->planets.end(),
        [&](const auto& row) { return row.id == metadata.planets.front().id; });
    assert(projected != live.view->planets.end() && projected->ability.title == "Popup title"
        && projected->ability.description_key == "KEY_B");
}

void corruption_attachment_edges_and_events_follow_admitted_facts() {
    GalaxyView view;
    view.local.seat = 1;
    PlanetView a; a.id = 101; a.position = {1, 2, 3};
    PlanetView b; b.id = 102;
    view.planets = {a, b};
    RouteView route; route.from = a.id; route.to = b.id;
    view.routes = {route, route}; // No duplicate particles on duplicate routes.
    CorruptionPublication facts;
    facts.seat = 1;
    CorruptionPlanetFact first;
    first.planet = a.id; first.corrupted = true; first.particle_type = "particle-a";
    CorruptionPlanetFact second = first; second.planet = b.id;
    facts.planets = {first, second};
    CorruptionVisuals visuals;
    visuals.update(view, facts);
    assert(visuals.attachments.size() == 2 && visuals.connections.size() == 1);
    assert(!visuals.connections[0].path);
    const auto generation = visuals.attachments[0].generation;
    facts.planets[0].brightness = 0.3F;
    visuals.update(view, facts);
    assert(visuals.attachments[0].generation == generation);
    assert(visuals.attachments[0].fact.brightness == 0.3F);
    facts.planets[0].particle_type = "particle-b";
    visuals.update(view, facts);
    assert(visuals.attachments[0].generation != generation);
    assert((visuals.attachments[0].position == Vec3{1, 2, 3}));
    facts.planets[0].corrupted = false;
    facts.planets[0].transitioning = true;
    facts.planets[0].increasing = true;
    visuals.update(view, facts);
    assert(visuals.attachments.size() == 2 && visuals.connections.size() == 1);
    assert(visuals.connections[0].path);
    facts.planets[0].corrupted = true;
    visuals.update(view, facts);
    assert(visuals.connections.size() == 2); // Settled neighbour adds a line as well.
    facts.planets[0].transitioning = false;
    visuals.update(view, facts);
    assert(visuals.connections.size() == 1 && !visuals.connections[0].path);
    facts.planets[0].corrupted = false; facts.planets[0].increasing = false;
    visuals.update(view, facts);
    assert(visuals.attachments.size() == 1 && visuals.connections.empty());
    facts.effects = {{1, a.id, GalacticEffectKind::sabotage, "explosion", {}, {}, {}},
        {2, b.id, GalacticEffectKind::planet_destruction, "explosion", {}, {}, {}},
        {3, b.id, GalacticEffectKind::superweapon_hack, "hack", {}, {}, {}}};
    visuals.update(view, facts);
    assert(visuals.one_shots.size() == 3);
    visuals.update(view, facts);
    assert(visuals.one_shots.empty());
    facts.seat = 2;
    visuals.update(view, facts);
    assert(visuals.attachments.empty() && visuals.connections.empty() && visuals.one_shots.empty());
    facts.seat = 1; facts.planets.clear(); facts.effects.clear();
    visuals.update(view, facts);
    assert(visuals.attachments.empty()); // Unpublished state never supplies a guessed particle.
}

void corruption_parameters_and_retained_pairs() {
    using namespace eawr::sim::galactic;
    corruption_fixture::Transition transition;
    transition.start = 10; transition.end = 30;
    assert(corruption_fraction(transition, 5) == 0 && corruption_fraction(transition, 10) == 0);
    assert(corruption_fraction(transition, 20) == 0.5F && corruption_fraction(transition, 100) == 1);
    transition.end = 0; assert(corruption_fraction(transition, 20) == 1);
    transition.end = transition.start; assert(corruption_fraction(transition, 0) == 1);
    transition.end = 9; assert(corruption_fraction(transition, 100) == 0);
    CorruptionParameters parameters;
    parameters.brightness = {0.2F, 1.4F}; parameters.scale = {2, 4}; parameters.energy = {0, 0.8F};
    CorruptionPlanetFact fact; fact.target_nonempty = true; fact.fraction = 0.25F;
    configure_corruption(fact, parameters);
    assert(same(*fact.brightness, 0.5F) && same(*fact.scale, 2.5F) && same(*fact.energy, 0.2F));
    fact.source_nonempty = true; fact.target_nonempty = false; fact.fraction = 0.75F;
    configure_corruption(fact, parameters);
    assert(same(*fact.brightness, 0.5F) && same(*fact.scale, 2.5F));
    CorruptionStyle style; style.path_colour = {37, 53, 11, 0};
    fact.transitioning = true; fact.fraction = 0.5F;
    const auto colour = corruption_path_colour(style, fact);
    assert(same(colour[0], 18.0F/255) && same(colour[1], 26.0F/255) && same(colour[2], 5.0F/255) && colour[3] == 1);
    style.radius = 5; style.endpoint_offset = 17;
    const auto geometry = corruption_line_geometry({0,0,0}, {100,0,0}, style);
    assert(geometry && geometry->origin.x == 17 && geometry->height == 66 && geometry->z.x == 1);
    assert(!corruption_line_geometry({0,0,0}, {30,0,0}, style));
    assert(!corruption_line_geometry({0,0,0}, {0,0,0}, style));
    style.endpoint_offset = -17;
    const auto extended = corruption_line_geometry({0,0,0}, {100,0,0}, style);
    assert(extended && extended->origin.x == -17 && extended->height == 134);
    CorruptionLineGrowth growth;
    assert(growth.service(10, 7) == 0);
    assert(growth.service(10, 7) == 0); // A second drawn frame is not a logical update.
    assert(same(growth.service(11, 7), 1.0F/210));
    assert(same(growth.service(115, 7), 0.5F));
    assert(same(growth.service(1000, 7), 1));
    CorruptionLineGrowth overshoot;
    assert(overshoot.service(0, 0.1F) == 0);
    assert(same(overshoot.service(1, 0.1F), 1.0F/3));
    CorruptionLineGrowth short_growth;
    assert(short_growth.service(0, 0.02F) == 0);
    assert(short_growth.service(1, 0.02F) > 1); // Submit the preceding overshoot, then clamp.
    assert(short_growth.service(2, 0.02F) == 1);
    GalaxyView view; view.local.seat = 1;
    PlanetView a; a.id = 1; PlanetView b; b.id = 2; view.planets = {a, b};
    RouteView route; route.from = 2; route.to = 1; view.routes = {route};
    CorruptionPublication publication; publication.seat = 1;
    CorruptionPlanetFact one; one.planet = 1; one.corrupted = true; one.particle_type = "mod";
    auto two = one; two.planet = 2; publication.planets = {one, two};
    CorruptionVisuals visuals; visuals.update(view, publication);
    assert(visuals.connections.size() == 1);
    const auto line = visuals.connections.front();
    const auto attachment_generation = visuals.attachments.front().generation;
    view.local.player = 2; visuals.update(view, publication);
    assert(visuals.attachments.front().generation > attachment_generation);
    assert(visuals.connections.front().generation != line.generation);
    const auto retained = visuals.connections.front();
    publication.planets[0].transitioning = publication.planets[1].transitioning = true;
    visuals.update(view, publication);
    assert(visuals.connections.size() == 1 && visuals.connections.front() == retained);
    publication.planets[1].corrupted = false;
    visuals.update(view, publication);
    assert(visuals.connections.empty()); // Both reciprocal records remove the one emitter.
    visuals.reset(); visuals.update(view, publication);
    assert(visuals.connections.empty());
}

void corruption_projection_rejects_undiscovered_and_other_observers() {
    auto setup = synthetic_live_setup();
    setup.observer.emplace();
    auto& observer = *setup.observer;
    observer.human_observers = {1}; observer.solo_reference_human = 1;
    observer.players.push_back({1, false, false, 0, 0, {}});
    for (const auto& planet : setup.planets) {
        eawr::sim::galactic::ObserverPlanet row; row.planet = planet.id; row.corrupted = true;
        observer.planets.push_back(row);
        observer.records.push_back({1, planet.id, planet.id == 101});
    }
    const eawr::sim::galactic::GalacticSnapshot snapshot(10, {}, setup.players,
        setup.planets, setup.routes, setup.fleets, {}, {}, {}, {}, {}, {},
        {}, {}, {}, {}, {}, setup.observer);
    auto view = *build_view(synthetic_galaxy(), 1).view;
    const auto bytes = snapshot.canonical_bytes();
    const auto admitted = observed_corruption(snapshot, view, "authored-particle");
    assert(admitted.planets.size() == 1 && admitted.planets[0].planet == 101);
    assert(admitted.planets[0].corrupted && admitted.planets[0].particle_type == "authored-particle");
    view.local.player = 2;
    assert(observed_corruption(snapshot, view, "authored-particle").planets.empty());
    assert(snapshot.canonical_bytes() == bytes);
}

void corruption_connection_work_follows_edges() {
    for (const ObjectId count : {32U, 1024U}) {
        GalaxyView view; view.local.seat = 1;
        CorruptionPublication publication; publication.seat = 1;
        for (ObjectId id = 1; id <= count; ++id) {
            PlanetView planet; planet.id = id; view.planets.push_back(planet);
            CorruptionPlanetFact fact; fact.planet = id; fact.transitioning = fact.increasing = true;
            publication.planets.push_back(fact);
            if (id > 1) { RouteView route; route.from = id-1; route.to = id; view.routes.push_back(route); }
        }
        CorruptionVisuals visuals;
        for (int repeat = 0; repeat < 2; ++repeat) {
            visuals.update(view, publication);
            assert(visuals.connections.size() == count-1);
            assert(visuals.work().route_visits == view.routes.size());
            assert(visuals.work().neighbor_visits == 2*view.routes.size());
            assert(visuals.work().adjacency_rebuilds == (repeat == 0 ? 1U : 0U));
        }
        view.routes.pop_back();
        visuals.update(view, publication);
        assert(visuals.work().adjacency_rebuilds == 1 && visuals.connections.size() == count-2);
    }
}

void transit_fixture_moves_before_repausing() {
    auto setup = synthetic_live_setup();
    std::vector<eawr::sim::galactic::FleetDisplayType> traits;
    std::vector<eawr::sim::galactic::FleetDisplayVisibility> visibility;
    std::map<eawr::sim::galactic::TypeId, std::string> names;
    configure_fleet_hud_fixture(setup, 1, "fleet-transit", traits, visibility, names);
    eawr::platform::LiveGameSession::Options options;
    options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
    auto started = eawr::platform::LiveGameSession::start(setup, options);
    assert(started);
    auto game = std::move(started).value();
    game->submit(eawr::platform::LiveGameOrder{eawr::sim::SeatId{1}, eawr::sim::galactic::PauseGalaxy{}, {}});
    game->advance_to(1);
    assert(game->wait_for(1, std::chrono::seconds(5)));
    game->submit(eawr::platform::LiveGameOrder{eawr::sim::SeatId{1}, eawr::sim::galactic::ResumeGalaxy{}, {}});
    game->advance_to(2);
    assert(game->wait_for(2, std::chrono::seconds(5)));
    assert(game->frame().latest->clock().pause_depth == 0);
    const auto target = game->frame().session_tick + 1;
    game->submit(eawr::platform::LiveGameOrder{eawr::sim::SeatId{1},
        eawr::sim::galactic::MoveFleet{601, {101,102}, std::nullopt}, {}});
    game->advance_to(target);
    assert(game->wait_for(target, std::chrono::seconds(5)));
    game->submit(eawr::platform::LiveGameOrder{eawr::sim::SeatId{1}, eawr::sim::galactic::PauseGalaxy{}, {}});
    game->advance_to(target + 1);
    assert(game->wait_for(target + 1, std::chrono::seconds(5)));
    const auto frame = game->frame();
    assert(frame.session_tick == target + 1 && frame.latest->clock().pause_depth == 1);
    assert(game->rejected_orders().empty());
    const auto moving = std::find_if(frame.latest->fleets().begin(), frame.latest->fleets().end(),
        [](const auto& fleet) { return fleet.id == 601; });
    assert(moving != frame.latest->fleets().end() && moving->motion.transit);
    const auto view = build_live_view(*frame.latest, synthetic_galaxy(), 1, synthetic_neutral_owner,
        {}, {visibility, {}, false});
    assert(view.view && std::any_of(view.view->fleets.begin(), view.view->fleets.end(),
        [](const auto& fleet) { return fleet.fleet == 601 && fleet.transit && fleet.destination == 102; }));
    game->stop();
}

void live_transit_overrides_orbit_and_synthetic_interpolation() {
    auto setup = synthetic_live_setup();
    const auto whole = [](std::int64_t value) { return eawr::sim::math::Fixed::from_raw(value * eawr::sim::math::Fixed::scale); };
    auto& motion = setup.fleets.front().motion;
    motion.position = {whole(-37), whole(22), whole(180)};
    motion.facing = {whole(1), whole(2), whole(0)};
    motion.transit.emplace(); motion.transit->path = {101,102,104}; motion.transit->hop = 1;
    setup.observer.emplace();
    setup.observer->human_observers = {1}; setup.observer->solo_reference_human = 1;
    setup.observer->players.push_back({1,false,false,0,0,{}});
    setup.observer->types.push_back({1,false,false,false});
    for (const auto& planet : setup.planets) {
        eawr::sim::galactic::ObserverPlanet row; row.planet = planet.id;
        setup.observer->planets.push_back(row);
        setup.observer->records.push_back({1,planet.id,true});
    }
    const eawr::sim::galactic::GalacticSnapshot snapshot(10, {}, setup.players,
        setup.planets, setup.routes, setup.fleets, {},
        {}, {}, {}, {}, {}, // movement/control/seat publication inputs
        {}, {}, {}, {}, {}, setup.observer);
    const auto view = build_live_view(snapshot, synthetic_galaxy(), 1, synthetic_neutral_owner,
        {}, {live_fleet_visibility(snapshot), {}, false});
    assert(view.view);
    const auto moving = std::find_if(view.view->fleets.begin(), view.view->fleets.end(),
        [](const auto& fleet) { return fleet.fleet == 301; });
    assert(moving != view.view->fleets.end() && moving->transit && moving->planet == 0);
    assert(moving->destination == 104 && moving->position.x == -37 && moving->position.y == 22 && moving->position.z == 180);
    assert(moving->facing.x == 1 && moving->facing.y == 2 && moving->facing.z == 0);
    assert(std::any_of(view.view->fleets.begin(), view.view->fleets.end(),
        [](const auto& fleet) { return fleet.fleet == 302 && !fleet.transit && fleet.planet == 101; }));
}

void live_fleets_follow_observer_presence_and_transit_visibility() {
    namespace gc = eawr::sim::galactic;
    auto setup = synthetic_live_setup();
    std::vector<gc::FleetDisplayType> display;
    std::vector<gc::FleetDisplayVisibility> sight;
    std::map<gc::TypeId, std::string> names;
    configure_fleet_hud_fixture(setup, 1, "fleet-presence", display, sight, names);
    setup.fleets.push_back({605,2,103,{{7010,1000}}});
    const auto snapshot = [&] {
        return gc::GalacticSnapshot(10, {}, setup.players, setup.planets, setup.routes, setup.fleets,
            {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, setup.observer);
    };
    const auto has = [](const GalaxyView& view, ObjectId id) {
        return std::any_of(view.fleets.begin(), view.fleets.end(),
            [id](const auto& row) { return row.fleet == id; });
    };
    const auto orbit = snapshot();
    const auto before = orbit.canonical_bytes();
    const auto view = build_live_view(orbit, synthetic_galaxy(), 2, synthetic_neutral_owner,
        {}, {sight, {}, false}); // coop seat 2 commands player 1
    assert(view.view && has(*view.view, 601) && has(*view.view, 603));
    assert(!has(*view.view, 604) && !has(*view.view, 605));
    const auto enemy = std::find_if(view.view->fleets.begin(), view.view->fleets.end(),
        [](const auto& row) { return row.fleet == 603; });
    assert(enemy->members == 0); // presence alone never publishes a count
    assert(orbit.canonical_bytes() == before);
    const auto live_sight = live_fleet_visibility(orbit);
    const auto cards = gc::observed_fleets(orbit, 1, display, 8, live_sight);
    assert(std::any_of(cards.cards.begin(), cards.cards.end(), [](const auto& row) { return row.fleet == 601; }));
    assert(std::any_of(cards.presence.begin(), cards.presence.end(), [](const auto& row) { return row.planet == 103; }));
    // WGUI-06: undiscovered planet 103 is currently visible, independently of discovery.
    assert(build_live_view(orbit, synthetic_galaxy(), 1, synthetic_neutral_owner).view->fleets.empty());
    assert(build_live_view(orbit, synthetic_galaxy(), 3, synthetic_neutral_owner,
        {}, {live_sight, {}, false}).view->fleets.empty()); // no foreign observer projection

    for (auto& planet : setup.planets) if (planet.id == 102) planet.owner = 2;
    for (auto& fleet : setup.fleets) if (fleet.id == 603) fleet.members = {{7005,1000}};
    setup.fleets.push_back({606,2,101,{{7011,1003}}});
    setup.fleets.push_back({607,2,102,{{7012,1000}}});
    setup.observer->types.back().stealth_object = true; // superweapon bypasses stealth
    for (auto& fleet : setup.fleets) {
        fleet.motion.transit.emplace();
        fleet.motion.transit->path = fleet.id == 607 ? std::vector<gc::PlanetId>{102,101}
            : std::vector<gc::PlanetId>{101,102};
    }
    const auto moving = snapshot();
    const std::array<TransitVisibility,3> transit{{{603,true},{604,true},{605,false}}};
    const auto projected = build_live_view(moving, synthetic_galaxy(), 1, synthetic_neutral_owner,
        {}, {sight, transit, false});
    assert(projected.view && has(*projected.view, 601) && has(*projected.view, 602));
    assert(has(*projected.view, 603) && !has(*projected.view, 604) && !has(*projected.view, 605));
    assert(has(*projected.view, 606) && has(*projected.view, 607));
    assert(std::all_of(projected.view->fleets.begin(), projected.view->fleets.end(), [](const auto& row) { return row.transit; }));
    const auto missing = build_live_view(moving, synthetic_galaxy(), 1, synthetic_neutral_owner,
        {}, {sight, {}, false});
    assert(missing.view && !has(*missing.view, 603) && !has(*missing.view, 605));
    const auto all = build_live_view(moving, synthetic_galaxy(), 1, synthetic_neutral_owner,
        {}, {sight, {}, true});
    assert(all.view && has(*all.view, 604) && has(*all.view, 605));
}

// #3931: the reveal flag is a draw-only observer swap. It shows a stealthed and a fogged
// enemy fleet and their counts, and leaves the truth publication (what commands read and
// what the simulation, saves and digests are made of) untouched.
void galaxy_reveal_is_a_draw_only_omniscient_view() {
    namespace gc = eawr::sim::galactic;
    auto setup = synthetic_live_setup();
    std::vector<gc::FleetDisplayType> display;
    std::vector<gc::FleetDisplayVisibility> sight;
    std::map<gc::TypeId, std::string> names;
    configure_fleet_hud_fixture(setup, 1, "fleet-presence", display, sight, names);
    setup.fleets.push_back({605,2,103,{{7010,1000}}}); // enemy fleet in the undiscovered system
    // The AI faction (player 2) is building a Star Destroyer, half done, over planet 102.
    const auto whole = [](std::int64_t value) { return eawr::sim::math::Fixed::from_raw(value * eawr::sim::math::Fixed::scale); };
    gc::ProductionSetup production;
    production.maximum_human_queue = 5;
    gc::PlanetProduction enemy_production;
    enemy_production.planet = 102;
    enemy_production.surface_accessible = true;
    gc::ProductionEntry queued;
    queued.type = 1000; queued.builder = 2; queued.build_id = 9001;
    queued.full_seconds = whole(60); queued.last_remaining = whole(30);
    enemy_production.queues[1].push_back(queued);
    production.planets.push_back(enemy_production);
    const std::shared_ptr<const gc::GalacticSnapshot> truth(new gc::GalacticSnapshot(10, {}, setup.players,
        setup.planets, setup.routes, setup.fleets, {}, {}, {}, {}, {}, {}, {}, production, {}, {}, {}, setup.observer));
    const auto bytes = truth->canonical_bytes();
    const auto digest = truth->sha256();
    const auto observer_before = *truth->observer();
    const auto observations_before = std::vector<gc::ObservedPlanet>(truth->observations().begin(), truth->observations().end());
    const auto has = [](const GalaxyView& view, ObjectId id) {
        return std::any_of(view.fleets.begin(), view.fleets.end(), [id](const auto& row) { return row.fleet == id; });
    };
    const auto members = [](const GalaxyView& view, ObjectId id) {
        const auto row = std::find_if(view.fleets.begin(), view.fleets.end(), [id](const auto& item) { return item.fleet == id; });
        return row == view.fleets.end() ? std::uint32_t{999} : row->members;
    };

    // Retail fog: the enemy fleet in the unexplored system, the stealthed one and every enemy count stay hidden.
    const auto fogged = build_live_view(*truth, synthetic_galaxy(), 1, synthetic_neutral_owner, {}, {sight, {}, false});
    assert(fogged.view && has(*fogged.view, 603) && !has(*fogged.view, 604) && !has(*fogged.view, 605));
    assert(members(*fogged.view, 603) == 0);
    const auto fogged_cards = gc::observed_fleets(*truth, 1, display, 8, sight);
    assert(std::none_of(fogged_cards.cards.begin(), fogged_cards.cards.end(), [](const auto& row) { return row.planet == 103; }));
    assert(observer_discovery(*truth, 1, 103) == std::optional<bool>(false));

    const auto revealed = reveal_snapshot(truth);
    assert(revealed && revealed != truth && revealed->observer() && revealed->observer()->solo_reference_human == 1);
    const auto reveal_sight = live_fleet_visibility(*revealed);
    const auto shown = build_live_view(*revealed, synthetic_galaxy(), 1, synthetic_neutral_owner, {},
        {reveal_sight, {}, true, true});
    // Fleets of one owner at one planet draw as one marker (lowest fleet id): 603 stands for 603 + the stealthed 604.
    assert(shown.view && has(*shown.view, 603) && has(*shown.view, 605));
    assert(members(*shown.view, 603) == 5 && members(*shown.view, 605) == 1);
    // The fleet HUD and radar read the same view: the fogged system now has a card and is discovered.
    const auto cards = gc::observed_fleets(*revealed, 1, display, 8, reveal_sight);
    assert(std::any_of(cards.cards.begin(), cards.cards.end(),
        [](const auto& row) { return row.planet == 103 && row.owner == 2 && row.count == 1; }));
    assert(std::any_of(cards.presence.begin(), cards.presence.end(),
        [](const auto& row) { return row.planet == 103 && row.owner == 2; }));
    assert(observer_discovery(*revealed, 1, 103) == std::optional<bool>(true));
    const auto owner = std::find_if(revealed->observations().begin(), revealed->observations().end(),
        [](const auto& row) { return row.player == 1 && row.planet == 102; });
    assert(owner != revealed->observations().end() && owner->known_owner == std::optional<gc::PlayerId>(2));
    // An AI faction's build queue reaches the planet HUD only through the reveal, read-only.
    const auto row_of = [](const gc::GalacticSnapshot& snapshot) -> const gc::ObservedPlanet& {
        const auto row = std::find_if(snapshot.observations().begin(), snapshot.observations().end(),
            [](const auto& item) { return item.player == 1 && item.planet == 102; });
        assert(row != snapshot.observations().end());
        return *row;
    };
    const auto describe = [](gc::TypeId type) {
        return std::pair<std::string, std::string>{type == 1000 ? "Star_Destroyer" : "", "star_destroyer_icon"};
    };
    const auto& queue_planet = truth->production()->planets.front();
    auto fogged_facts = observed_hud_facts(row_of(*truth), nullptr, &queue_planet, 1);
    assert(!publish_production_queue(fogged_facts, row_of(*truth), &queue_planet, false, describe));
    assert(!planet_hud_view(&fogged_facts, true, 3, 3).queues_enabled);
    auto queue_facts = observed_hud_facts(row_of(*revealed), nullptr, &queue_planet, 1);
    assert(!queue_facts.locally_controlled);
    assert(!publish_production_queue(queue_facts, row_of(*revealed), &queue_planet, false, describe)); // flag off
    assert(publish_production_queue(queue_facts, row_of(*revealed), &queue_planet, true, describe));
    assert(queue_facts.queue_revealed && queue_facts.space_queue && queue_facts.space_queue->size() == 1);
    const auto& item = queue_facts.space_queue->front();
    assert(item.type == "Star_Destroyer" && item.type_id == 1000 && item.build_id == 9001 && item.progress_percent == 50);
    const auto queue_view = planet_hud_view(&queue_facts, true, 3, 3);
    assert(queue_view.queues_enabled && !queue_view.accepts_queue && queue_view.choices.empty());
    assert(std::count_if(queue_view.queues.begin(), queue_view.queues.end(), [](const auto& row) { return row.visible; }) == 1);
    assert(production_activity(queue_facts) && !production_activity(fogged_facts));

    // An enemy fleet in transit (origin, destination) is drawn only with the reveal.
    auto moving_planets = setup.planets;
    for (auto& planet : moving_planets) if (planet.id == 102) planet.owner = 2;
    auto moving_fleets = setup.fleets;
    for (auto& fleet : moving_fleets) if (fleet.id == 605) {
        fleet.motion.transit.emplace();
        fleet.motion.transit->path = {103, 102};
    }
    const std::shared_ptr<const gc::GalacticSnapshot> moving(new gc::GalacticSnapshot(10, {}, setup.players,
        moving_planets, setup.routes, moving_fleets, {}, {}, {}, {}, {}, {}, {}, production, {}, {}, {}, setup.observer));
    const auto moving_fogged = build_live_view(*moving, synthetic_galaxy(), 1, synthetic_neutral_owner, {},
        {live_fleet_visibility(*moving), {}, false});
    assert(moving_fogged.view && !has(*moving_fogged.view, 605));
    const auto moving_revealed = reveal_snapshot(moving);
    const auto moving_shown = build_live_view(*moving_revealed, synthetic_galaxy(), 1, synthetic_neutral_owner, {},
        {live_fleet_visibility(*moving_revealed), {}, true, true});
    assert(moving_shown.view && std::any_of(moving_shown.view->fleets.begin(), moving_shown.view->fleets.end(),
        [](const auto& row) { return row.fleet == 605 && row.transit && row.destination == 102; }));

    // Only the observer column moves: the world itself is the truth publication's.
    assert(std::equal(revealed->planets().begin(), revealed->planets().end(), truth->planets().begin(), truth->planets().end()));
    assert(std::equal(revealed->fleets().begin(), revealed->fleets().end(), truth->fleets().begin(), truth->fleets().end()));

    // Command contract: move/land intents are decided from the truth publication and the truth discovery,
    // so the reveal view yields the same intent as truth for every destination, and a fog-hidden planet is refused.
    std::vector<InputPlanet> command_planets;
    for (const auto& planet : truth->planets())
        command_planets.push_back({planet.id, {}, 0, observer_discovery(*truth, 1, planet.id).value_or(false)});
    assert(std::any_of(command_planets.begin(), command_planets.end(), [](const auto& row) { return row.id == 103 && !row.discovered; }));
    std::size_t local_fleets = 0;
    for (const auto& fleet : truth->fleets()) {
        if (fleet.owner != 1 || fleet.motion.transit) continue;
        ++local_fleets;
        for (const auto& planet : command_planets) {
            const auto from_truth = fleet_destination_intent(*truth, 1, fleet.id, planet.id, command_planets, {});
            const auto from_reveal = fleet_destination_intent(*revealed, 1, fleet.id, planet.id, command_planets, {});
            assert(from_truth.has_value() == from_reveal.has_value());
            if (from_truth) assert(from_truth->kind == from_reveal->kind && from_truth->planet == from_reveal->planet
                && from_truth->path == from_reveal->path);
            if (!planet.discovered) assert(!from_reveal);
        }
    }
    assert(local_fleets > 0);

    // The truth publication is unchanged, so commands and digests keep the player's real knowledge.
    assert(truth->canonical_bytes() == bytes && truth->sha256() == digest);
    assert(*truth->observer() == observer_before);
    assert(std::equal(truth->observations().begin(), truth->observations().end(),
        observations_before.begin(), observations_before.end()));
    assert(observer_discovery(*truth, 1, 103) == std::optional<bool>(false));
    // A publication without an observer has nothing to reveal.
    const std::shared_ptr<const gc::GalacticSnapshot> bare(new gc::GalacticSnapshot(1, {}, setup.players,
        setup.planets, setup.routes, setup.fleets, {}));
    assert(reveal_snapshot(bare) == bare);
    // The reveal is single-human only: one seat would see what the other humans hide.
    assert(!reveal_refusal(true, 0) && !reveal_refusal(true, 1));
    assert(reveal_refusal(true, 2) && reveal_refusal(true, 4));
    assert(!reveal_refusal(false, 2));
}

// The reveal runs on every published tick in the viewer. Driving the same seeded session with
// and without it must give the same simulation state, save bytes and digest.
void galaxy_reveal_leaves_a_seeded_session_untouched() {
    namespace gc = eawr::sim::galactic;
    const auto run = [](bool reveal) {
        auto setup = synthetic_live_setup();
        std::vector<gc::FleetDisplayType> display;
        std::vector<gc::FleetDisplayVisibility> sight;
        std::map<gc::TypeId, std::string> names;
        configure_fleet_hud_fixture(setup, 1, "fleet-transit", display, sight, names);
        eawr::platform::LiveGameSession::Options options;
        options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
        auto started = eawr::platform::LiveGameSession::start(setup, options);
        assert(started);
        auto game = std::move(started).value();
        game->submit(eawr::platform::LiveGameOrder{eawr::sim::SeatId{1},
            eawr::sim::galactic::MoveFleet{601, {101,102}, std::nullopt}, {}});
        std::size_t revealed_fleets = 0;
        for (std::uint64_t tick = 1; tick <= 90; ++tick) {
            game->advance_to(tick);
            assert(game->wait_for(tick, std::chrono::seconds(5)));
            if (!reveal) continue;
            const auto shown = reveal_snapshot(game->frame().latest);
            const auto built = build_live_view(*shown, synthetic_galaxy(), 1, synthetic_neutral_owner, {},
                {live_fleet_visibility(*shown), {}, true, true});
            assert(built.view);
            revealed_fleets += built.view->fleets.size();
        }
        assert(!reveal || revealed_fleets > 0);
        const auto latest = game->frame().latest;
        auto result = std::pair{latest->canonical_bytes(), latest->sha256()};
        game->stop();
        return result;
    };
    const auto plain = run(false);
    const auto drawn = run(true);
    assert(plain.first == drawn.first && plain.second == drawn.second);
}

void live_snapshots_replace_dynamic_fixture_fields() {
    auto setup = synthetic_live_setup();
    setup.debug_commands = true;
    assert(eawr::sim::galactic::validate_setup(setup));
    eawr::platform::LiveGameSession::Options options;
    options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
    auto started = eawr::platform::LiveGameSession::start(setup, options);
    assert(started);
    auto game = std::move(started).value();
    const auto before = game->frame().latest;
    game->submit(eawr::platform::LiveGameOrder{eawr::sim::SeatId{2}, eawr::sim::galactic::PauseGalaxy{}, {}});
    game->advance_to(1);
    assert(game->wait_for(1, std::chrono::seconds(5)));
    const auto paused = game->frame().latest;
    assert(paused->clock().pause_depth == 1 && paused->clock().galactic_frame == 0);
    assert(!paused->events().empty());
    game->submit(eawr::platform::LiveGameOrder{eawr::sim::SeatId{2},
        eawr::sim::galactic::DebugSetPlanetOwner{101, 2}, {}});
    assert(game->wait_for(2, std::chrono::seconds(5)));
    const auto changed = game->frame().latest;
    const auto metadata = synthetic_galaxy();
    const auto first = build_live_view(*changed, metadata, 1, synthetic_neutral_owner);
    const auto second = build_live_view(*changed, metadata, 2, synthetic_neutral_owner);
    assert(first.view && second.view && first.view->tick == 2);
    assert(first.view->planets.front().owner == 2U && first.view->planets.front().owner_faction == "Empire");
    assert(first.view->planets.front().relation == Relation::other);
    assert(first.view->planets.front().model == metadata.planets.front().model);
    assert(first.view->local.player == second.view->local.player);
    assert(std::none_of(first.view->fleets.begin(), first.view->fleets.end(), [](const auto& fleet) { return fleet.transit; }));
    assert(before->planets().front().owner == 1); // retained publication never mutates
    game->submit(eawr::platform::LiveGameOrder{eawr::sim::SeatId{2}, eawr::sim::galactic::ResumeGalaxy{}, {}});
    game->advance_to(12);
    assert(game->wait_for(12, std::chrono::seconds(5)));
    assert(game->frame().latest->clock().pause_depth == 0);
    assert(game->frame().latest->clock().galactic_frame > 0);
    game->stop();
}

[[nodiscard]] const PlanetView& planet(const GalaxyView& view, const ObjectId id) {
    const auto found = std::find_if(view.planets.begin(), view.planets.end(),
                                    [&](const PlanetView& item) { return item.id == id; });
    assert(found != view.planets.end());
    return *found;
}

[[nodiscard]] bool same(const float a, const float b) { return std::abs(a - b) < 1.0e-3F; }

void coop_seats_share_the_faction_view() {
    const GalaxySnapshotStub galaxy = synthetic_galaxy();
    const ViewResult first = build_view(galaxy, 1);
    const ViewResult second = build_view(galaxy, 2);
    assert(first.view && second.view);
    // Two seats, one faction: the seat differs, the faction and its seats do not.
    assert(first.view->local.seat == 1 && second.view->local.seat == 2);
    assert(first.view->local.player == 1 && second.view->local.player == 1);
    assert(first.view->local.faction == "Rebel" && second.view->local.faction == "Rebel");
    assert((first.view->local.faction_seats == std::vector<SeatId>{1, 2}));
    assert(first.view->local.faction_seats == second.view->local.faction_seats);
    // Everything faction-derived is identical for both seats.
    for (std::size_t index = 0; index < first.view->planets.size(); ++index) {
        assert(first.view->planets[index].colour == second.view->planets[index].colour);
        assert(first.view->planets[index].relation == second.view->planets[index].relation);
    }
    // Seat IDs are not player IDs: seats 3 and 5 command player 2, the other party.
    const ViewResult empire = build_view(galaxy, 5);
    assert(empire.view && empire.view->local.player == 2 && empire.view->local.faction == "Empire");
    assert((empire.view->local.faction_seats == std::vector<SeatId>{3, 5}));
    const ViewResult ai = build_view(galaxy, 4);
    assert(ai.view && ai.view->local.faction == "Underworld" && (ai.view->local.faction_seats == std::vector<SeatId>{4}));
    assert(planet(*empire.view, 107).relation == Relation::own);
    assert(planet(*first.view, 107).relation == Relation::other);
}

void colours_follow_the_owning_faction() {
    const ViewResult built = build_view(synthetic_galaxy(), 1);
    assert(built.view);
    const GalaxyView& view = *built.view;
    assert(view.planets.size() == 9);
    // Rebel 185,40,39; Empire 54,134,242; neutral 100,100,100.
    assert(same(planet(view, 101).colour.r, 185.0F / 255.0F) && same(planet(view, 101).colour.b, 39.0F / 255.0F));
    assert(planet(view, 101).relation == Relation::own && planet(view, 101).owner_faction == "Rebel");
    assert(same(planet(view, 107).colour.b, 242.0F / 255.0F));
    assert(!planet(view, 103).owner && planet(view, 103).relation == Relation::neutral);
    assert(same(planet(view, 103).colour.g, 100.0F / 255.0F));
    // WGP-11: a route is typed only between two planets of the same faction.
    for (const RouteView& route : view.routes) {
        const PlanetView& a = planet(view, route.from);
        const PlanetView& b = planet(view, route.to);
        assert(route.typed == (a.owner && b.owner && *a.owner == *b.owner));
        assert(route.colour == (route.typed ? a.colour : planet(view, 103).colour));
    }
    assert(std::count_if(view.routes.begin(), view.routes.end(), [](const RouteView& r) { return r.typed; }) == 3);
}

void fleets_group_per_faction_in_orbit() {
    const ViewResult built = build_view(synthetic_galaxy(), 1);
    assert(built.view);
    const auto& fleets = built.view->fleets;
    // Two Rebel fleets at planet 101 become one marker; two fleets in transit.
    assert(fleets.size() == 6);
    const auto rebel = std::find_if(fleets.begin(), fleets.end(),
                                    [](const FleetMarker& m) { return m.planet == 101 && m.owner == 1; });
    assert(rebel != fleets.end() && rebel->fleets == 2 && rebel->members == 6 && rebel->fleet == 301);
    assert(rebel->relation == Relation::own);
    assert(std::count_if(fleets.begin(), fleets.end(), [](const FleetMarker& m) { return m.transit; }) == 2);
    const auto moving = std::find_if(fleets.begin(), fleets.end(), [](const FleetMarker& m) { return m.fleet == 306; });
    assert(moving != fleets.end() && moving->transit);
    // 40% of the hop from 103 (-60, 60) to 105 (30, -40).
    assert(same(moving->position.x, -24.0F) && same(moving->position.y, 20.0F));
    for (const FleetMarker& marker : fleets) {
        if (marker.transit) continue;
        const PlanetView& at = planet(*built.view, marker.planet);
        const float dx = marker.position.x - at.position.x;
        const float dy = marker.position.y - at.position.y;
        assert(same(std::sqrt(dx * dx + dy * dy), skeleton_orbit_offset * at.scale));
    }
}

void bad_snapshots_and_seats_are_refused() {
    GalaxySnapshotStub galaxy = synthetic_galaxy();
    assert(!build_view(galaxy, 9).view);
    galaxy.seats[1].active = false;
    const ViewResult inactive = build_view(galaxy, 2);
    assert(!inactive.view && inactive.failure.find("not active") != std::string::npos);
    const ViewResult remaining = build_view(galaxy, 1);
    assert(remaining.view && (remaining.view->local.faction_seats == std::vector<SeatId>{1}));
    GalaxySnapshotStub dangling = synthetic_galaxy();
    dangling.routes.push_back({299, 101, 999});
    assert(!build_view(dangling, 1).view);
    GalaxySnapshotStub repeated = synthetic_galaxy();
    repeated.planets.push_back(repeated.planets.front());
    assert(!build_view(repeated, 1).view);
}

void the_camera_frames_and_clamps() {
    const ViewResult built = build_view(synthetic_galaxy(), 1);
    assert(built.view);
    const GalaxyView& view = *built.view;
    OrbitCamera camera = overview_camera(view, 16.0F / 9.0F, 45.0F);
    assert(camera.distance > orbit_min_distance && camera.distance < orbit_max_distance);
    // WGUI-58: the box pads each side by 0.15 x the half-size.
    const Bounds box = pan_bounds(view);
    assert(same(box.max.x - view.bounds.max.x, 0.15F * (view.bounds.max.x - view.bounds.min.x) * 0.5F));
    pan(camera, view, 100000.0F, -100000.0F);
    assert(same(camera.target.x, box.max.x) && same(camera.target.y, box.min.y));
    orbit(camera, 0.0F, 1000.0F);
    assert(same(camera.pitch_degrees, orbit_max_pitch));
    zoom(camera, 0.0F);
    zoom(camera, 1.0e9F);
    assert(same(camera.distance, orbit_max_distance));
    // Render basis (x, y, z) -> (x, z, -y): a top-down eye sits above the target.
    OrbitCamera top = overview_camera(view, 1.0F, 45.0F);
    top.pitch_degrees = 89.0F;
    const auto fixed = fixed_camera(top, 1280, 720, 45.0F);
    assert(fixed.width == 1280 && fixed.height == 720);
    assert(fixed.eye[1] > fixed.target[1]);
    assert(same(fixed.target[2], -top.target.y));
}

void planet_flights_follow_wall_time_and_stock_transforms() {
    const auto view = *build_view(synthetic_galaxy(), 1).view;
    const auto& selected = planet(view, 101);
    PlanetCamera camera;
    assert(same(camera.pose().target.z, 10.0F));
    assert(same(camera.pose().pitch_degrees, 30.0F) && same(camera.pose().distance, 700.0F));
    assert(!camera.zoom_in(selected, true, {false, false, false}));
    PlanetCamera locked_selection;
    assert(locked_selection.zoom_in(selected, true, {true, true, false}));
    assert(!camera.zoom_in(selected, true, {true, false, true}));
    assert(camera.zoom_in(selected, true));
    assert(camera.state() == CameraState::inward && camera.alternate_model());
    camera.service(10000); // First service, not command time, starts the clock.
    assert(same(camera.alpha(), 0.0F));
    assert(!camera.zoom_in(selected, true));
    camera.pan(view, 100.0F, 100.0F);
    camera.service(10130); // alpha .2 -> smoothstep .104, light linear .2.
    assert(same(camera.alpha(), 0.2F));
    const auto partial = camera.pose();
    const float radius = 4.92F * selected.scale;
    assert(same(partial.target.x, selected.position.x * 0.104F));
    assert(same(partial.distance, 700.0F + (3.0F * radius - 700.0F) * 0.104F));
    assert(same(camera.lighting().angles.y, 175.0F));
    camera.service(10325);
    assert(same(camera.alpha(), 0.5F));
    assert(same(camera.pose().pitch_degrees, 32.5F));
    camera.service(10650);
    assert(camera.state() == CameraState::focused && same(camera.alpha(), 1.0F));
    assert(same(camera.pose().distance, 3.0F * radius));
    assert(same(camera.pose().target.z, selected.position.z + radius * 1.05F));
    assert(camera.zoom_in(selected, true, {true, false, true}) && camera.advanced_map());
    assert(!camera.zoom_in(selected, true));
    assert(camera.zoom_out({true, false, true}) && !camera.advanced_map() && camera.state() == CameraState::focused);
    assert(!camera.zoom_out({true, false, true}) && camera.state() == CameraState::focused);
    assert(camera.refocus(planet(view, 107), true));
    assert(camera.planet() == 107);
    assert(camera.zoom_out());
    camera.service(20000);
    camera.service(20325);
    assert(camera.state() == CameraState::outward && same(camera.alpha(), 0.5F));
    camera.service(20650);
    assert(camera.state() == CameraState::overview && !camera.alternate_model() && camera.planet() == 0);
    assert(same(camera.pose().target.x, 0.0F) && same(camera.pose().distance, 700.0F));
    // Tick zero is a valid first timestamp; coop cameras hold independent state.
    PlanetCamera other;
    assert(other.toggle(selected, true) && same(other.radar_rate(), 1.0F));
    other.service(0);
    other.service(650);
    assert(other.state() == CameraState::focused && camera.state() == CameraState::overview);
    assert(other.zoom_in(selected, true) && other.advanced_map());
    assert(other.toggle(selected, true) && !other.advanced_map() && other.state() == CameraState::outward);
    assert(same(other.radar_rate(), 1.0F));
    PlanetCamera missing;
    assert(missing.zoom_in(selected, false));
    missing.service(0); missing.service(650);
    assert(same(missing.pose().distance, 0.0F)); // No model/renderable -> radius zero (WGUI-60).
}

void pan_drag_and_radar_are_separate_transforms() {
    const auto light = source_light_direction({99.0F, 90.0F, 0.0F});
    assert(same(light.x, 1.0F) && same(light.y, 0.0F) && same(light.z, 0.0F));
    const auto focused_light = source_light_direction(CameraConstants{}.in_angles);
    assert(focused_light.x < -0.77F && focused_light.y > 0.54F && focused_light.z > 0.30F);
    const auto view = *build_view(synthetic_galaxy(), 1).view;
    PlanetCamera regular;
    assert(!regular.right_drag(view, 0.01F, 0.02F, false, false));
    assert(regular.right_drag(view, 0.01F, 0.02F, true, false));
    assert(same(regular.pose().target.x, 10.0F) && same(regular.pose().target.y, 20.0F));
    PlanetCamera slow;
    assert(slow.right_drag(view, 0.01F, 0.02F, true, true));
    assert(same(slow.pose().target.x, 2.5F) && same(slow.pose().target.y, 5.0F));
    slow.pan(view, 100.0F, 100.0F, true);
    assert(same(slow.pose().target.x, 2.5F));
    slow.pan(view, 1.0e6F, 1.0e6F);
    const auto bounds = pan_bounds(view);
    assert(same(slow.pose().target.x, bounds.max.x) && same(slow.pose().target.y, bounds.min.y));
    assert(same(slow.pose().target.z, 10.0F));
    for (const auto size : {std::array<float, 2>{200.0F, 100.0F}, {640.0F, 480.0F}}) {
        const Vec3 local{size[0] * 0.5F, -size[1] * 0.5F, 123.0F};
        const auto world = radar_to_world(local, size[0], size[1]);
        assert(world && (*world == Vec3{380.0F, -460.0F, 10.0F}));
        const auto back = world_to_radar(*world, size[0], size[1]);
        assert(back && same(back->x, local.x) && same(back->y, local.y) && back->z == 0.0F);
    }
    assert(!radar_to_world({}, 0, 100));
    assert(!world_to_radar({}, 100, std::numeric_limits<float>::infinity()));
    CameraConstants changed;
    changed.zoom_seconds = 2.0F; changed.initial_pullback = 1000.0F; changed.focused_light = 0.9F;
    PlanetCamera custom(changed);
    assert(custom.zoom_in(planet(view, 101), true));
    custom.service(0); custom.service(1000);
    assert(same(custom.alpha(), 0.5F) && same(custom.lighting().level, 0.7F));
}

void campaign_start_uses_authored_home_transform() {
    // WGP-41/42: Equal Footing Underworld's authored Hypori position.
    PlanetCamera camera;
    camera.position({160.0F, -320.0F, 999.0F});
    const auto pose = camera.pose();
    assert(pose.target.x == 160.0F && pose.target.y == -320.0F && pose.target.z == 10.0F);
    assert(pose.yaw_degrees == 0.0F && pose.pitch_degrees == 30.0F && pose.distance == 700.0F);
    const auto rendered = fixed_camera(pose, 1280, 720, galactic_vertical_fov(1280, 720));
    assert(std::abs(rendered.eye[0] - 160.0F) < 0.001F);
    assert(std::abs(rendered.eye[1] - 360.0F) < 0.001F);
    assert(std::abs(rendered.eye[2] - (320.0F + std::sqrt(3.0F) * 350.0F)) < 0.001F);
    assert(rendered.near_plane == 10.0F && rendered.far_plane == 10000.0F);
    // WGP-43: resize preserves horizontal full angle, converting only the aspect.
    const float tangent = std::tan(std::atan(0.5F) * 0.5F);
    for (const auto width : {960U, 1280U}) {
        const float vertical = galactic_vertical_fov(width, 720);
        const float focal = 360.0F / std::tan(vertical * std::numbers::pi_v<float> / 360.0F);
        assert(std::abs(focal - static_cast<float>(width) * 0.5F / tangent) < 0.001F);
    }
    // Positioning does not apply the ordinary drag bounds or a distance clamp.
    CameraConstants authored;
    authored.initial_pullback = 1234.0F;
    authored.initial_pitch = -20.0F;
    authored.scroll_plane = 25.0F;
    PlanetCamera custom(authored);
    custom.position({9000.0F, -8000.0F, -99.0F});
    const auto changed = custom.pose();
    assert(changed.target.x == 9000.0F && changed.target.y == -8000.0F && changed.target.z == 25.0F);
    assert(changed.distance == 1234.0F && changed.pitch_degrees == 20.0F && changed.yaw_degrees == 0.0F);
}

void native_ai_is_on_by_default_and_off_disables_it() {
    NativeAiFlag flag;
    assert(flag.enabled && !flag.invalid);
    apply_native_ai_flag(flag, "off");
    assert(!flag.enabled && !flag.invalid);
    apply_native_ai_flag(flag, "on");
    assert(flag.enabled && !flag.invalid);
    apply_native_ai_flag(flag, "maybe");
    assert(flag.invalid);
}

} // namespace

void pathfinder_slot_contracts() {
    namespace gc = eawr::sim::galactic;
    namespace ui = eawr::presentation::ui;
    gc::FleetControlSetup control;
    control.types = {{10, false, true, false, true}, {11, false, true, false, false}, {12, true, true, false, true}};
    std::vector<gc::Fleet> fleets{{1, 1, 101, {{20,10}, {21,10}, {22,11}, {23,12}}, 0}, {2, 2, 101, {{30,10}}, 1}};
    std::vector<gc::Seat> seats{{eawr::sim::SeatId{1}, 1}};
    gc::GalacticClock clock;
    const auto snapshot = [&] { return gc::GalacticSnapshot(0, clock, {}, {}, {}, fleets, {}, {}, control, {}, {}, seats); };
    const auto order = [&](gc::FleetId fleet, eawr::sim::EntityId member) { return ui::fleet_pathfinder_order(snapshot(), eawr::sim::SeatId{1}, fleet, member); };
    const std::array<std::uint8_t, 1> slots{0};
    assert(!ui::fleet_pathfinder(snapshot(), 1, 101, slots));
    assert(order(1,20) && order(1,23) && order(1,0));
    assert(!order(1,22) && !order(1,999) && !order(2,30) && !order(999,0));
    seats[0].active = false; assert(!order(1,20)); seats[0].active = true;
    seats[0].kind = gc::SeatKind::ai; assert(!order(1,20)); seats[0].kind = gc::SeatKind::human;
    fleets[0].motion.transit.emplace(); assert(!order(1,20) && !order(1,0)); fleets[0].motion.transit.reset();
    fleets[0].orbit = 0; assert(!order(1,20)); fleets[0].orbit = 101;
    clock.pause_depth = 1; assert(!order(1,20));
    control.pause = gc::PausePolicy::admit_all(); assert(order(1,20));
    assert(!ui::fleet_pathfinder_order(snapshot(), eawr::sim::SeatId{1}, 1, 20, true));
    clock.pause_depth = 0;
    control.gates.queued_campaign_conflict = true;
    assert(!ui::fleet_pathfinder_order(snapshot(), eawr::sim::SeatId{1}, 1, 20, false, true));
    assert(!ui::fleet_pathfinder_order(snapshot(), eawr::sim::SeatId{1}, 1, 0, false, true));
    control.gates.queued_campaign_conflict = false; control.gates.active_speech = true;
    assert(!ui::fleet_pathfinder_order(snapshot(), eawr::sim::SeatId{1}, 1, 20, true, true));
    seats.push_back({eawr::sim::SeatId{2}, 1});
    assert(ui::fleet_pathfinder_order(snapshot(), eawr::sim::SeatId{1}, 1, 20, true, true));
    seats.pop_back(); control.gates.active_speech = false;
    fleets[0].members[0].pathfinder = true;
    const auto shown = snapshot(); const auto before = shown.canonical_bytes();
    const auto selected = ui::fleet_pathfinder(shown, 1, 101, slots);
    assert(selected && selected->fleet == 1 && selected->member.id == 20);
    assert(!ui::fleet_pathfinder(shown, 2, 101, slots) && !ui::fleet_pathfinder(shown, 1, 102, slots));
    gc::ObservedFleet card; card.local = true; card.owner = 1; card.planet = 101; card.orbit_slot = std::uint8_t{0}; card.count = 4;
    gc::ObservedFleetGroup group; group.count = 2; group.type = 10; group.representative = 20; group.fleet = 1;
    card.groups.push_back(group);
    ui::separate_pathfinder(card, shown, 20, {});
    assert(card.count == 4 && card.groups.size() == 1 && card.groups[0].count == 1 && card.groups[0].representative == 21);
    card.orbit_slot.reset(); card.groups = {group};
    const std::array<std::pair<eawr::sim::EntityId, std::int64_t>, 1> remembered{{{20, 7}}};
    ui::separate_pathfinder(card, shown, 20, remembered, slots);
    assert(card.groups[0].count == 1 && card.groups[0].representative == 21);
    assert(shown.canonical_bytes() == before); // UI-07: view/offer/packing leave publication bytes unchanged.
    fleets[0].members[0].pathfinder = false;
    assert(!ui::fleet_pathfinder(snapshot(), 1, 101, slots));
}

int main() {
    ggs_02_known_ground_counts_and_ggs_04_local_launches();
    empty_ground_slot_returns_only_while_ground_units_are_held();
    ggs_09_sanitized_ground_representatives();
    ggs_09_native_fixture_qualifies_all_live_combat_queries();
    pause_preview_matches_published_policy_and_holds();
    paused_departure_is_transit_at_route_start_until_resume();
    pathfinder_slot_contracts();
    native_ai_is_on_by_default_and_off_disables_it();
    planet_bonus_metadata_survives_view_projection();
    corruption_attachment_edges_and_events_follow_admitted_facts();
    corruption_parameters_and_retained_pairs();
    corruption_projection_rejects_undiscovered_and_other_observers();
    corruption_connection_work_follows_edges();
    live_fleets_follow_observer_presence_and_transit_visibility();
    galaxy_reveal_is_a_draw_only_omniscient_view();
    galaxy_reveal_leaves_a_seeded_session_untouched();
    transit_fixture_moves_before_repausing();
    live_transit_overrides_orbit_and_synthetic_interpolation();
    live_snapshots_replace_dynamic_fixture_fields();
    coop_seats_share_the_faction_view();
    colours_follow_the_owning_faction();
    fleets_group_per_faction_in_orbit();
    bad_snapshots_and_seats_are_refused();
    the_camera_frames_and_clamps();
    planet_flights_follow_wall_time_and_stock_transforms();
    pan_drag_and_radar_are_separate_transforms();
    campaign_start_uses_authored_home_transform();
    return 0;
}
