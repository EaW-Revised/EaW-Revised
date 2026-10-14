// The galactic mode's view model (apps/viewer/src/galactic/galaxy_view.cpp):
// my seat and my faction stay separate (ADR-020), ownership colours follow the
// faction, fleets group per faction in orbit, the overview frames the galaxy and
// panning stays inside the WGUI-58 box.
#include "galactic/galaxy_snapshot_stub.hpp"
#include "galactic/galaxy_view.hpp"
#include "galactic/corruption_visuals.hpp"
#include "galactic/fleet_hud_fixture.hpp"
#include "eawr/platform/live_game_session.hpp"

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

void corruption_projection_rejects_undiscovered_and_other_observers() {
    auto setup = synthetic_live_setup();
    setup.observer.emplace();
    auto& observer = *setup.observer;
    observer.local_player = 1;
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
    setup.observer->local_player = 1;
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

} // namespace

int main() {
    planet_bonus_metadata_survives_view_projection();
    corruption_attachment_edges_and_events_follow_admitted_facts();
    corruption_projection_rejects_undiscovered_and_other_observers();
    live_fleets_follow_observer_presence_and_transit_visibility();
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
