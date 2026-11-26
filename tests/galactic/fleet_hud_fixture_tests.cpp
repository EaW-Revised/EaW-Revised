#include "../../apps/viewer/src/galactic/planet_detail_layout.hpp"
#include "../../apps/viewer/src/galactic/fleet_traits.hpp"
#include "galactic_support.hpp"
#include "../presentation/galactic/fleet_hud_fixture.hpp"
#include "../presentation/galactic/ground_hud_fixture.hpp"
#include "galactic/ground_roster.hpp"
#include "../presentation/galactic/hud_clock_fixture.hpp"
#include "galactic/galaxy_view.hpp"
#include "galactic/fleet_projection_cache.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <cstdlib>
#include <string>
#include <string_view>

namespace {

// #8428: the fleet HUD's incremental projection (the developer verify mode's comparison) equals a full projection of
// every publication across a split, a merge, a move (departure, transit, arrival), a sight change and a presentation
// index change, and reprojects only the planets whose rows changed. The live view built from the shared projection
// equals the one that projects by itself.
void incremental_fleet_projection_matches_full(const std::string_view fixture) {
    namespace gc = eawr::sim::galactic;
    namespace view = eawr::viewer::galactic;
    const std::string label = "#8428 " + std::string(fixture) + ": ";
    auto setup = view::synthetic_live_setup();
    view::configure_synthetic_hud_clock(setup);
    std::vector<gc::FleetDisplayType> traits;
    std::vector<gc::FleetDisplayVisibility> visibility;
    std::map<gc::TypeId, std::string> names;
    view::configure_fleet_hud_fixture(setup, 1, fixture, traits, visibility, names);
    auto created = gc::GalacticSession::create(setup);
    if (!created) { galactic_test::expect(false, label + created.error().message); return; }
    auto session = std::move(created).value();
    const eawr::platform::ThreadWorkerAdapter executor(1);
    const auto metadata = view::synthetic_galaxy();
    std::vector<std::pair<eawr::sim::EntityId, std::int64_t>> indices;
    std::vector<eawr::sim::EntityId> departing;
    // Rank callbacks stand in for the authored art: any stable answer per type.
    const auto inputs = [&] {
        return view::FleetProjectionInputs{1, traits, 8, visibility,
            [](const gc::TypeId type) -> std::optional<std::uint32_t> { return type % 7; }, indices,
            [](const gc::TypeId type) -> std::optional<std::int64_t> {
                return type == 1003 ? std::optional<std::int64_t>{1} : std::nullopt; },
            departing, 0};
    };
    view::IncrementalFleetProjection cache;
    std::size_t partial_planets{}, reused{}, checks{};
    const auto check = [&](const std::string& what) {
        const auto snapshot = session.snapshot();
        const auto before = cache.receipts();
        const auto& incremental = cache.update(snapshot, inputs());
        const auto full = view::project_observed_fleets(*snapshot, inputs());
        galactic_test::expect(view::same_fleet_projection(incremental, full), label + what + ": incremental equals full");
        if (cache.receipts().partial > before.partial) partial_planets += cache.last_changed().size();
        if (cache.receipts().reused > before.reused) ++reused;
        ++checks;
        const view::FleetMapVisibility map{visibility, {}, false, false};
        const auto shared = view::build_live_view(*snapshot, metadata, 1, view::synthetic_neutral_owner, {}, map, &incremental);
        const auto own = view::build_live_view(*snapshot, metadata, 1, view::synthetic_neutral_owner, {}, map);
        galactic_test::expect(shared.view && own.view && shared.view->fleets == own.view->fleets,
            label + what + ": the live view from the shared projection equals its own");
    };
    std::uint64_t sequence{};
    const auto order = [&](const auto& command, const std::string& what) {
        const auto submitted = session.submit({{session.completed_tick(), gc::SeatId{1}, ++sequence}, command});
        galactic_test::expect(bool(submitted), label + what + " is admitted");
    };
    const auto step = [&](const std::string& what) {
        const auto stepped = session.step(executor);
        if (!stepped) { galactic_test::expect(false, label + what + ": " + stepped.error().message); return false; }
        check(what);
        return true;
    };
    if (!step("startup")) return;
    galactic_test::expect(cache.last_full(), label + "the first publication projects every planet");
    if (!step("quiet tick")) return;
    galactic_test::expect(!cache.last_full(), label + "a tick without publication-wide changes is not a full projection");
    order(gc::TransferFleetMembers{{{601, 7003}}, 0, 2}, "split");
    if (!step("split")) return;
    order(gc::MergeFleets{602, 601}, "merge");
    if (!step("merge")) return;
    if (setup.movement) {
        order(gc::MoveFleet{601, {101, 102}, {}}, "move");
        bool arrived = false;
        for (int tick = 0; tick < 600 && !arrived; ++tick) {
            if (!step("move tick " + std::to_string(tick))) return;
            const auto fleets = session.snapshot()->fleets();
            arrived = std::any_of(fleets.begin(), fleets.end(), [](const auto& fleet) {
                return fleet.id == 601 && fleet.orbit == 102 && !fleet.motion.transit; });
        }
        galactic_test::expect(arrived, label + "the moved fleet arrives at its destination");
    }
    // A sight change reprojects that planet alone, on the same publication.
    for (auto& row : visibility) if (row.planet == 103) row.current_visible = true;
    check("sight change");
    galactic_test::expect(!cache.last_full() && cache.last_changed().size() == 1 && cache.last_changed().front() == 103,
        label + "a sight change reprojects only its planet");
    // A presentation index belongs to every card: everything is reprojected.
    indices = {{7001, 0}};
    check("presentation index");
    galactic_test::expect(cache.last_full(), label + "a presentation index change projects every planet");
    if (!step("after index")) return;
    galactic_test::expect(checks > 4 && partial_planets > 0, label + "partial updates happened");
    static_cast<void>(reused);
}

} // namespace

int main() {
    for (const auto fixture : {"fleet-contents", "fleet-slot"}) incremental_fleet_projection_matches_full(fixture);
    namespace gc = eawr::sim::galactic;
    namespace view = eawr::viewer::galactic;
    for (const auto name : {"fleet-presence", "fleet-count", "fleet-contents", "fleet-heroes"}) {
        for (const gc::PlayerId player : {1U, 2U, 3U}) {
            auto setup = view::synthetic_live_setup();
            view::configure_synthetic_hud_clock(setup);
            std::vector<gc::FleetDisplayType> traits;
            std::vector<gc::FleetDisplayVisibility> visibility;
            std::map<gc::TypeId, std::string> names;
            view::configure_fleet_hud_fixture(setup, player, name, traits, visibility, names);
            auto created = gc::GalacticSession::create(setup);
            if (!created) {
                std::cerr << "FAIL: " << name << " player " << player << ": " << created.error().message << '\n';
                return 1;
            }
            auto session = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(1);
            const auto stepped = session.step(executor);
            if (!stepped) { std::cerr << "FAIL: " << stepped.error().message << '\n'; return 1; }
            const auto projected = gc::observed_fleets(*session.snapshot(), player, traits, 35, visibility);
            galactic_test::expect(projected.cards.size() >= 2, "fixture publishes both own orbit slots after session admission");
            galactic_test::expect(std::any_of(projected.cards.begin(), projected.cards.end(),
                [](const auto& card) { return card.fleet == 601; }), "own published fleet identity survives startup");
        }
    }
    // GHUD-S68: the focused planet's structure row. Capacity is authored data; the built land
    // structures need structure knowledge and arrive in creation order.
    for (const auto name : {"ground-structures", "ground-structures-hidden", "ground-contents", "ground-launch-move"}) {
        auto setup = view::synthetic_live_setup();
        view::configure_synthetic_hud_clock(setup);
        std::vector<gc::FleetDisplayType> traits;
        std::vector<gc::FleetDisplayVisibility> visibility;
        std::map<gc::TypeId, std::string> names;
        view::configure_ground_fixture(setup, 1, name, traits, visibility, names);
        auto created = gc::GalacticSession::create(setup);
        if (!created) { std::cerr << "FAIL: " << name << ": " << created.error().message << '\n'; return 1; }
        auto session = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(1);
        const auto stepped = session.step(executor);
        if (!stepped) { std::cerr << "FAIL: " << name << ": " << stepped.error().message << '\n'; return 1; }
        const auto roster = view::ground_roster(*session.snapshot(), 1, 101);
        const std::string_view fixture = name;
        if (fixture == "ground-contents") {
            // Base Layout text: a planet whose ground contents are visible shows the caption alone.
            galactic_test::expect(roster.contents_known && roster.size_known && !roster.destroyed
                && view::base_layout_text(roster) == view::BaseLayoutText{true, false},
                "visible ground force: BASE LAYOUT caption without the UNKNOWN line");
        }
        if (fixture == "ground-launch-move") {
            const auto submitted = session.submit({{1, gc::SeatId{1}, 1}, gc::LaunchFleet{{8001}, {101, 102}, {}}});
            if (!submitted) { std::cerr << "FAIL: " << submitted.error().message << '\n'; return 1; }
            const auto moved = session.step(executor);
            if (!moved) { std::cerr << "FAIL: " << moved.error().message << '\n'; return 1; }
            galactic_test::expect(std::any_of(session.snapshot()->fleets().begin(), session.snapshot()->fleets().end(),
                [](const auto& fleet) { return fleet.motion.transit && fleet.motion.transit->path.back() == 102
                    && std::any_of(fleet.members.begin(), fleet.members.end(), [](const auto& member) { return member.id == 8001; }); }),
                "native launch fixture admits the real combined command and publishes its moving member");
        }
        if (fixture == "ground-structures") {
            galactic_test::expect(roster.accessible && roster.structures_known && roster.structure_capacity == 5,
                "known structures: capacity is the authored land position count");
            galactic_test::expect(roster.structures.size() == 3 && roster.structures[0].id == 9101
                && roster.structures[1].id == 9102 && roster.structures[2].id == 9103,
                "known structures: built land structures keep creation order");
            galactic_test::expect(roster.structures[0].type == 1004 && roster.structures[1].type == 1005
                && roster.structures[2].owner == 2, "known structures: type and owner come with each row");
            const auto other = view::ground_roster(*session.snapshot(), 1, 102);
            galactic_test::expect(other.structure_capacity == 3 && other.structures.empty(),
                "a planet with nothing built keeps its authored capacity");
        } else if (fixture == "ground-structures-hidden") {
            galactic_test::expect(roster.structure_capacity == 5 && !roster.structures_known && roster.structures.empty(),
                "unknown structures: capacity only, no contents");
        } else {
            galactic_test::expect(roster.structure_capacity == 0 && roster.structures.empty(),
                "a fixture without a production table has no structure capacity");
        }
    }
    // GHUD-S68: the focused planet's structure row. Capacity is authored data; the built land
    // structures need structure knowledge and arrive in creation order.
    for (const auto name : {"ground-structures", "ground-structures-hidden", "ground-contents"}) {
        auto setup = view::synthetic_live_setup();
        view::configure_synthetic_hud_clock(setup);
        std::vector<gc::FleetDisplayType> traits;
        std::vector<gc::FleetDisplayVisibility> visibility;
        std::map<gc::TypeId, std::string> names;
        view::configure_ground_fixture(setup, 1, name, traits, visibility, names);
        auto created = gc::GalacticSession::create(setup);
        if (!created) {
            std::cerr << "FAIL: " << name << ": " << created.error().message << '\n';
            return 1;
        }
        auto session = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(1);
        const auto stepped = session.step(executor);
        if (!stepped) { std::cerr << "FAIL: " << name << ": " << stepped.error().message << '\n'; return 1; }
        const auto roster = view::ground_roster(*session.snapshot(), 1, 101);
        const std::string_view fixture = name;
        if (fixture == "ground-structures") {
            galactic_test::expect(roster.accessible && roster.structures_known && roster.structure_capacity == 5,
                "known structures: capacity is the authored land position count");
            galactic_test::expect(roster.structures.size() == 3 && roster.structures[0].id == 9101
                && roster.structures[1].id == 9102 && roster.structures[2].id == 9103,
                "known structures: built land structures keep creation order");
            galactic_test::expect(roster.structures.size() == 3 && roster.structures[0].type == 1004
                && roster.structures[1].type == 1005 && roster.structures[2].owner == 2,
                "known structures: type and owner come with each row");
            const auto other = view::ground_roster(*session.snapshot(), 1, 102);
            galactic_test::expect(other.structure_capacity == 3 && other.structures.empty(),
                "a planet with nothing built keeps its authored capacity");
        } else if (fixture == "ground-structures-hidden") {
            galactic_test::expect(roster.structure_capacity == 5 && !roster.structures_known && roster.structures.empty(),
                "unknown structures: capacity only, no contents");
        } else {
            galactic_test::expect(roster.structure_capacity == 0 && roster.structures.empty(),
                "a fixture without a production table has no structure capacity");
        }
    }
    // The Base Layout text lines: caption unless the planet is destroyed; UNKNOWN while the force size is unseen.
    {
        view::GroundRoster seen, unseen, destroyed;
        seen.size_known = true;
        destroyed.destroyed = true; destroyed.size_known = false;
        galactic_test::expect(view::base_layout_text(seen) == view::BaseLayoutText{true, false}, "size seen: caption only");
        galactic_test::expect(view::base_layout_text(unseen) == view::BaseLayoutText{true, true}, "size unseen: caption and UNKNOWN");
        galactic_test::expect(view::base_layout_text(destroyed) == view::BaseLayoutText{false, false},
            "destroyed planet: no text at all, whatever the knowledge");
        galactic_test::expect(view::base_layout_text({}) == view::BaseLayoutText{true, true}, "empty roster reads as unknown");
    }
    // GHUD-S69: pad placement after the debug build's slot cleanup.
    {
        const std::vector<view::GroundCompany> companies{{11, 1000, true, 1}, {12, 1000, true, 1}, {13, 1000, true, 1}};
        auto layout = view::place_pads(companies, {}, 6);
        galactic_test::expect(layout[0] == 0U && layout[1] == 1U && layout[2] == 2U && !layout[3],
            "pads fill in roster order without remembered slots");
        layout = view::place_pads(companies, {{12, 4}, {13, 4}, {11, 9}}, 6);
        galactic_test::expect(layout[4] == 1U && layout[0] == 0U && layout[1] == 2U,
            "a remembered slot is kept; a clash or an out-of-range slot takes the first free pad");
        layout = view::place_pads(companies, {}, 6);
        galactic_test::expect(view::move_pad(layout, 0, 4) && !layout[0] && layout[4] == 0U && layout[1] == 1U,
            "a drop on an empty pad takes it and frees the old one");
        layout = view::place_pads(companies, {}, 6);
        galactic_test::expect(view::move_pad(layout, 0, 2) && layout[0] == 1U && layout[1] == 2U && layout[2] == 0U,
            "a drop on an occupied pad shifts the neighbours down into the old pad");
        layout = view::place_pads(companies, {}, 6);
        galactic_test::expect(view::move_pad(layout, 2, 0) && layout[0] == 2U && layout[1] == 0U && layout[2] == 1U,
            "a drop on the first pad pushes the others up to the next free pad");
        const std::vector<view::GroundCompany> full{{1, 1, true, 1}, {2, 1, true, 1}, {3, 1, true, 1}};
        layout = view::place_pads(full, {}, 3);
        galactic_test::expect(view::move_pad(layout, 0, 2) && layout[0] == 1U && layout[1] == 2U && layout[2] == 0U,
            "a full row still rotates through the moved company's own pad");
    }
    {
        // Live fleet projection uses published transport traits, rather than guessing from display art.
        std::vector<eawr::sim::galactic::FleetDisplayType> display(2);
        display[0].type = 7; display[1].type = 9;
        std::vector<eawr::sim::galactic::FleetTypeTraits> types(2);
        types[0].type = 7; types[0].transport = true; types[1].type = 9;
        view::apply_fleet_transport_traits(display, types);
        galactic_test::expect(display[0].transport && !display[1].transport, "published transport traits feed the fleet projection");
        // A disclosed local member may have no display metadata; classify it without revealing foreign groups.
        gc::ObservedFleet local; local.local = true; local.owner = 1;
        gc::ObservedFleetGroup missing; missing.type = 42; missing.fleet = 10; missing.representative = 11;
        local.groups.push_back(missing);
        gc::ObservedFleet other = local; other.local = false;
        gc::Fleet fleet; fleet.id = 10; fleet.owner = 1; fleet.members.push_back({11, 7});
        std::vector<gc::ObservedFleet> disclosed{local, other};
        view::apply_local_transport_traits(disclosed, std::vector<gc::Fleet>{fleet}, types);
        galactic_test::expect(disclosed[0].groups[0].transport && !disclosed[1].groups[0].transport,
            "local transport uses published member traits even without a display row");
        galactic_test::expect(view::land_tray_enabled(true, 9, 10) && !view::land_tray_enabled(true, 10, 10)
            && !view::land_tray_enabled(false, 1, 10) && !view::land_tray_enabled(true, {}, 10),
            "tray disables at capacity, for view-only and for unknown counts");
        // GPD-14 groups local transport types across all orbit slots.
        using namespace eawr::sim::galactic;
        ObservedFleet one; one.planet = 3; one.local = true; one.orbit_slot = std::uint8_t{0};
        ObservedFleetGroup transport; transport.type = 7; transport.count = 2; transport.transport = true;
        transport.representative = 70; transport.fleet = 5;
        ObservedFleetGroup ship; ship.type = 9; ship.count = 4;
        one.groups = {transport, ship};
        ObservedFleet two = one; two.orbit_slot = std::uint8_t{2}; two.groups = {transport}; two.groups[0].count = 1;
        ObservedFleet middle = one; middle.orbit_slot = std::uint8_t{1}; middle.groups = {transport};
        ObservedFleet foreign = one; foreign.local = false;
        const auto tray = view::land_tray_entries({one, middle, two, foreign}, 3, false);
        galactic_test::expect(tray.size() == 1 && tray[0].type == 7 && tray[0].count == 5 && !tray[0].enabled,
            "tray combines ground types, excludes ships and foreign fleets, and respects disabled state");
        galactic_test::expect(view::land_tray_entry(tray, 0) && !view::land_tray_entry(tray, 1), "unused tray slots hide");
        galactic_test::expect(tray[0].representative == 70 && tray[0].fleet == 5,
            "a tray entry names the disclosed unit and fleet that a drop lands");
        galactic_test::expect(!view::land_tray_enabled(true, 10, 10) && !view::land_tray_enabled(false, 1, 10)
            && !view::land_tray_enabled(true, {}, 10), "landed cap, view-only, and inaccessible count disable the tray");
        galactic_test::expect(view::land_tray_entries({one}, 4, true).empty(), "tray uses selected planet");
        galactic_test::expect(view::land_tray_slot_name(3) == "land_fleet_03", "tray slot names");
    }
    {
        using view::PlanetDetailAccess;
        galactic_test::expect(view::planet_detail_access(true, false, false, false, true) == PlanetDetailAccess::edit,
            "owner edits");
        galactic_test::expect(view::planet_detail_access(false, true, false, false, true) == PlanetDetailAccess::view_only
            && view::planet_detail_access(false, false, true, true, true) == PlanetDetailAccess::view_only,
            "ground intelligence or corruption benefit permits viewing");
        galactic_test::expect(view::planet_detail_access(false, false, true, false, true) == PlanetDetailAccess::closed,
            "other viewers are closed");
        galactic_test::expect(view::planet_detail_access(true, true, true, true, false) == PlanetDetailAccess::closed,
            "inaccessible surface closes even for owner with intelligence");
        const auto inaccessible = view::planet_detail_access(true, false, false, false, false);
        const auto accessible = view::planet_detail_access(true, false, false, false, true);
        galactic_test::expect(!view::detail_pads_visible(inaccessible) && !view::detail_can_edit(inaccessible)
            && view::detail_pads_visible(accessible) && view::detail_can_edit(accessible),
            "inaccessible owner hides pads and cannot submit layout; accessible owner can edit");
        // GPD-17/20: reserve remembered positions before filling free pads.
        using eawr::assets::UiMapMarker;
        using eawr::assets::UiMarkerKind;
        const auto marker = [](std::uint32_t id, UiMarkerKind kind, std::string types = {}) {
            UiMapMarker row; row.record = id; row.kind = kind; row.specific_types = std::move(types); return row;
        };
        const std::vector<UiMapMarker> markers{marker(1, UiMarkerKind::in_base), marker(2, UiMarkerKind::out_base),
            marker(3, UiMarkerKind::specific, "Tower, Shield"), marker(4, UiMarkerKind::defender), marker(5, UiMarkerKind::defender)};
        std::vector<view::DetailStructure> structures(3);
        structures[0].id = 10; structures[0].type = 10; structures[0].name = "Ordinary"; structures[0].base_position = "IN_BASE";
        structures[1].id = 11; structures[1].type = 11; structures[1].name = "Reserved"; structures[1].remembered = 1;
        structures[2].id = 12; structures[2].type = 12; structures[2].name = "Tower";
        std::vector<view::DetailUnit> units{{20, 20, {}}, {21, 21, 4}};
        const auto detail = view::planet_detail_layout(markers, structures, units);
        galactic_test::expect(detail.builds[0].object == 11 && detail.builds[1].object == 10
            && detail.builds[2].object == 12 && detail.builds[2].fixed, "remembered, fallback and fixed specific placement");
        galactic_test::expect(detail.units[0].object == 21 && detail.units[1].object == 20, "unit remembered reservation");
        const auto swap = view::detail_move(detail, true, 0, 1, true);
        galactic_test::expect(swap.size() == 2 && swap[0].object == 11 && swap[0].marker == 2U
            && swap[1].object == 10 && swap[1].marker == 1U, "building swap is one atomic change");
        galactic_test::expect(view::detail_move(detail, true, 0, 1, view::detail_can_edit(inaccessible)).empty()
            && !view::detail_move(detail, true, 0, 1, view::detail_can_edit(accessible)).empty(),
            "surface access prevents or permits a layout drag");
        galactic_test::expect(view::detail_move(detail, true, 0, 2, true).empty()
            && view::detail_move(detail, false, 0, 1, false).empty(), "specific and view-only drops are refused");
        const auto unit_swap = view::detail_move(detail, false, 0, 1, true);
        galactic_test::expect(unit_swap.size() == 2 && unit_swap[0].object == 21 && unit_swap[1].object == 20,
            "unit swap changes both markers");
        const auto defaults = view::detail_defaults(detail, {{11, 1}, {21, 4}});
        galactic_test::expect(defaults.size() == 2 && defaults[0].object == 10 && defaults[1].object == 20,
            "default write-back includes only unassigned movable objects");
        structures[2].name = "Source"; structures[2].every_marker_type = "Tower";
        const auto every = view::planet_detail_layout(markers, structures, {});
        galactic_test::expect(every.builds[2].specific_type == "Tower", "every-marker target uses specific pad");
    }
    // GPD-22/23: a tray drop is checked as a whole before anything is submitted. The plan is the exact command
    // list the viewer submits, so an empty plan means nothing was submitted.
    {
        namespace gc = eawr::sim::galactic;
        using view::TrayDropResult;
        auto base = view::synthetic_live_setup();
        view::configure_synthetic_hud_clock(base);
        std::vector<gc::FleetDisplayType> traits;
        std::vector<gc::FleetDisplayVisibility> visibility;
        std::map<gc::TypeId, std::string> names;
        view::configure_ground_fixture(base, 1, "ground-launch-move", traits, visibility, names);
        // Every transport member needs its explicit (empty) payload row, kept in container order.
        const auto add_empty_transport = [](gc::GalacticSetup& setup, eawr::sim::EntityId id) {
            setup.movement->contents.push_back({id, {}});
            std::sort(setup.movement->contents.begin(), setup.movement->contents.end(),
                [](const auto& a, const auto& b) { return a.container < b.container; });
        };
        const auto build = [&](const auto& mutate) {
            auto copy = base;
            mutate(copy);
            auto created = gc::GalacticSession::create(copy);
            if (!created) { std::cerr << "FAIL: tray fixture: " << created.error().message << '\n'; std::exit(1); }
            auto session = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(1);
            if (!session.step(executor)) { std::cerr << "FAIL: tray fixture step\n"; std::exit(1); }
            return session;
        };
        const view::LandTrayEntry entry{.type = 1000, .count = 2, .enabled = true, .representative = 7001, .fleet = 601};
        const auto pad_with = [](std::optional<eawr::sim::EntityId> occupant, std::uint32_t marker) {
            view::DetailPad pad; pad.marker.record = marker; pad.object = occupant; pad.type = 1000; return pad;
        };
        const view::TrayDropFacts open{true, true, true};
        const auto planet = [](const gc::GalacticSnapshot& snapshot) -> const gc::ControlPlanet& {
            const auto& rows = snapshot.control()->planets;
            return *std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.planet == 101; });
        };
        auto world = build([](auto&) {});
        const auto snapshot = world.snapshot();
        const auto occupied = view::plan_tray_landing(*snapshot, 1, 101, entry, pad_with(8001, 5), open);
        galactic_test::expect(occupied.result == TrayDropResult::ok && occupied.commands.size() == 3
            && std::get_if<gc::LaunchFromSurface>(&occupied.commands[0])
            && std::get_if<gc::LandUnits>(&occupied.commands[1])
            && std::get_if<gc::SetGroundLayout>(&occupied.commands[2]),
            "a drop on an occupied pad is launch, land, marker in that order");
        if (occupied.commands.size() == 3) {
            const auto& launch = *std::get_if<gc::LaunchFromSurface>(&occupied.commands[0]);
            const auto& land = *std::get_if<gc::LandUnits>(&occupied.commands[1]);
            const auto& layout = *std::get_if<gc::SetGroundLayout>(&occupied.commands[2]);
            galactic_test::expect(launch.units == std::vector<eawr::sim::EntityId>{8001} && launch.target == 0 && launch.slot == 0
                && land.planet == 101 && land.units == std::vector<eawr::sim::EntityId>{7001}
                && layout.planet == 101 && layout.entries.size() == 1 && layout.entries[0].object == 7001
                && layout.entries[0].marker == 5U, "the plan names the occupant, the dropped unit and the pad marker");
            // The simulation admits the whole list in one tick, in order.
            std::uint64_t sequence{};
            for (const auto& command : occupied.commands)
                world.submit({{world.completed_tick(), gc::SeatId{1}, ++sequence}, command}).value();
            const eawr::platform::ThreadWorkerAdapter executor(1);
            const auto result = world.step(executor).value();
            const auto& surface = planet(*result.snapshot);
            const auto has = [&](eawr::sim::EntityId id) {
                return std::any_of(surface.landed.begin(), surface.landed.end(), [id](const auto& row) { return row.id == id; });
            };
            galactic_test::expect(has(7001) && !has(8001) && surface.landed.size() == 3,
                "the planned drop lands the unit and sends the occupant to orbit");
            galactic_test::expect(std::any_of(surface.ground_layout.begin(), surface.ground_layout.end(),
                [](const auto& row) { return row.object == 7001 && row.marker == 5; })
                && !std::any_of(surface.ground_layout.begin(), surface.ground_layout.end(),
                    [](const auto& row) { return row.object == 8001; }), "the dropped unit remembers the pad marker");
            galactic_test::expect(std::any_of(result.snapshot->fleets().begin(), result.snapshot->fleets().end(),
                [](const auto& fleet) { return fleet.orbit == 101 && std::any_of(fleet.members.begin(), fleet.members.end(),
                    [](const auto& member) { return member.id == 8001; }); }), "the occupant joins an orbit fleet");
        }
        const auto free_pad = view::plan_tray_landing(*snapshot, 1, 101, entry, pad_with({}, 6), open);
        galactic_test::expect(free_pad.result == TrayDropResult::ok && free_pad.commands.size() == 2
            && std::get_if<gc::LandUnits>(&free_pad.commands[0]), "a drop on a free pad is land and marker only");
        auto noncombat = build([](auto& setup) {
            setup.control->ground_capability_types = {{1000, {}, true, false}, {1001, {}, false, true},
                {1002, {}, false, false}, {1003, {}, false, false}};
            for (auto& row : setup.movement->contents) if (row.container == 7001) row.children = {{9001, 1001}};
        });
        const auto noncombat_snapshot = noncombat.snapshot();
        galactic_test::expect(!gc::has_ground_capability(*noncombat_snapshot->control(), {7001, 1000},
                noncombat_snapshot->movement()).value(), "the tray regression carries a noncombat passenger");
        const auto noncombat_plan = view::plan_tray_landing(*noncombat_snapshot, 1, 101, entry, pad_with({}, 6), open);
        galactic_test::expect(noncombat_plan.result == TrayDropResult::ok,
            "GGS-11: the advanced-map tray admits a transport with a stay-in-transport passenger");
        std::uint64_t noncombat_sequence{};
        for (const auto& command : noncombat_plan.commands)
            noncombat.submit({{noncombat.completed_tick(), gc::SeatId{1}, ++noncombat_sequence}, command}).value();
        const eawr::platform::ThreadWorkerAdapter noncombat_executor(1);
        const auto noncombat_result = noncombat.step(noncombat_executor).value();
        galactic_test::expect(std::any_of(planet(*noncombat_result.snapshot).landed.begin(),
                planet(*noncombat_result.snapshot).landed.end(), [](const auto& row) { return row.id == 7001; }),
            "the planned noncombat tray landing executes on the same admission rule");
        const auto refused = [&](const view::TrayLanding& plan, TrayDropResult why, const char* message) {
            galactic_test::expect(plan.result == why && plan.commands.empty(), message);
        };
        refused(view::plan_tray_landing(*snapshot, 1, 101, entry, pad_with(8001, 5), {false, true, true}),
            TrayDropResult::view_only, "view-only submits nothing");
        refused(view::plan_tray_landing(*snapshot, 1, 101, entry, pad_with(8001, 5), {true, false, true}),
            TrayDropResult::paused, "a paused surface deploy submits nothing, not even the launch");
        refused(view::plan_tray_landing(*snapshot, 1, 101, entry, pad_with(8001, 5), {true, true, false}),
            TrayDropResult::paused, "a paused layout change submits nothing, not even the launch");
        auto fixed = pad_with(8001, 5); fixed.fixed = true;
        refused(view::plan_tray_landing(*snapshot, 1, 101, entry, fixed, open), TrayDropResult::fixed_pad, "a fixed pad is refused");
        auto elsewhere = entry; elsewhere.fleet = 999;
        refused(view::plan_tray_landing(*snapshot, 1, 101, elsewhere, pad_with(8001, 5), open),
            TrayDropResult::not_in_orbit, "a unit that is not in a local orbit fleet is refused");
        refused(view::plan_tray_landing(*snapshot, 1, 102, entry, pad_with(8001, 5), open),
            TrayDropResult::not_in_orbit, "a unit orbiting another planet is refused");
        auto cargo = entry; cargo.representative = 7003;
        refused(view::plan_tray_landing(*snapshot, 1, 101, cargo, pad_with(8001, 5), open),
            TrayDropResult::not_a_transport, "a member that cannot land is refused");
        refused(view::plan_tray_landing(*snapshot, 1, 101, entry, pad_with(9999, 5), open),
            TrayDropResult::occupant_missing, "a pad occupant that is not on the surface is refused before any launch");
        refused(view::plan_tray_landing(*snapshot, 2, 101, entry, pad_with(8001, 5), open),
            TrayDropResult::not_in_orbit, "another player's fleet is refused");
        // The rest of the sim's refusals that a snapshot can answer: no unit, closed surface, planet not ours.
        auto nameless = entry; nameless.representative = 0;
        refused(view::plan_tray_landing(*snapshot, 1, 101, nameless, pad_with(8001, 5), open),
            TrayDropResult::no_unit, "a tray entry with no disclosed unit is refused");
        const auto closed = build([](auto& setup) {
            for (auto& row : setup.control->planets) if (row.planet == 101) row.surface_accessible = false;
        });
        const auto closed_snapshot = closed.snapshot();
        refused(view::plan_tray_landing(*closed_snapshot, 1, 101, entry, pad_with(8001, 5), open),
            TrayDropResult::surface_closed, "an inaccessible surface is refused before any launch");
        // The layout command needs the planet's owner: a landable unit orbiting a planet the local player does not own is refused.
        const auto abroad = build([&](auto& setup) {
            // Planet 107 belongs to player 2 in the synthetic galaxy.
            setup.fleets.push_back({650, 1, 107, {{7050, 1000}}, 0});
            add_empty_transport(setup, 7050);
        });
        const auto abroad_snapshot = abroad.snapshot();
        const view::LandTrayEntry visiting{.type = 1000, .count = 1, .enabled = true, .representative = 7050, .fleet = 650};
        refused(view::plan_tray_landing(*abroad_snapshot, 1, 107, visiting, pad_with({}, 6), open),
            TrayDropResult::not_planet_owner, "a planet the local player does not own is refused (the layout command needs the owner)");
        // A blockaded planet refuses the occupant's launch, so the whole drop is refused up front.
        const auto blockaded = build([&](auto& setup) {
            // The first fleet of the orbit list holds the blockade, so the enemy fleet goes in front of the local one.
            // A warship (not an empty transport, which has no live breakdown and so holds no orbit).
            setup.fleets.insert(setup.fleets.begin(), {599, 2, 101, {{7099, 1001}}, 0});
        });
        const auto blockaded_snapshot = blockaded.snapshot();
        const auto& states = blockaded_snapshot->control()->state;
        galactic_test::expect(std::any_of(states.begin(), states.end(), [](const auto& row) { return row.planet == 101 && row.blockaded; }),
            "the fixture's enemy orbit fleet blockades planet 101");
        refused(view::plan_tray_landing(*blockaded_snapshot, 1, 101, entry, pad_with(8001, 5), open),
            TrayDropResult::blockaded, "a blockaded planet refuses a drop that would launch an occupant");
        // The landing is refused when the ground owner is not allied, and the owner is read after the occupant has gone.
        const auto enemy_base = build([](auto& setup) {
            for (auto& row : setup.control->planets) if (row.planet == 101) row.ground_base = {2, 1};
        });
        const auto enemy_base_snapshot = enemy_base.snapshot();
        refused(view::plan_tray_landing(*enemy_base_snapshot, 1, 101, entry, pad_with(8001, 5), open),
            TrayDropResult::not_planet_owner, "an enemy ground base makes the planet theirs: nothing is launched or landed");
        auto mixed = build([](auto& setup) {
            for (auto& row : setup.control->planets) if (row.planet == 101) row.landed[1].owner = 2;
        });
        const auto mixed_snapshot = mixed.snapshot();
        refused(view::plan_tray_landing(*mixed_snapshot, 1, 101, entry, pad_with(8001, 5), open),
            TrayDropResult::alliance_blocked, "the ground owner after the launch (the next landed unit) decides, not the occupant");
        galactic_test::expect(view::plan_tray_landing(*mixed_snapshot, 1, 101, entry, pad_with({}, 6), open).result == TrayDropResult::ok,
            "with no occupant leaving, the same surface admits the drop");
        // What the check prevents: the same three commands, submitted blind, launch the occupant and then lose the landing.
        {
            std::uint64_t sequence{};
            for (const gc::CommandPayload& command : std::vector<gc::CommandPayload>{
                    gc::LaunchFromSurface{{8001}, 0, 0}, gc::LandUnits{101, {7001}}, gc::SetGroundLayout{101, {{7001, 5}}}})
                mixed.submit({{mixed.completed_tick(), gc::SeatId{1}, ++sequence}, command}).value();
            const eawr::platform::ThreadWorkerAdapter blind_executor(1);
            const auto blind = mixed.step(blind_executor).value();
            const auto& surface = planet(*blind.snapshot);
            const auto on_surface = [&](eawr::sim::EntityId id) {
                return std::any_of(surface.landed.begin(), surface.landed.end(), [id](const auto& row) { return row.id == id; });
            };
            galactic_test::expect(!on_surface(8001) && !on_surface(7001),
                "unchecked, the occupant is launched and the landing is refused: the half-apply the pre-check exists to stop");
        }
        // Capacity is counted after the occupant's launch: full surface refuses a free pad, accepts an occupied one.
        const auto full = build([](auto& setup) { setup.control->max_ground_forces = 3; });
        const auto full_snapshot = full.snapshot();
        refused(view::plan_tray_landing(*full_snapshot, 1, 101, entry, pad_with({}, 6), open),
            TrayDropResult::no_capacity, "a full surface refuses a drop on a free pad and submits nothing");
        galactic_test::expect(view::plan_tray_landing(*full_snapshot, 1, 101, entry, pad_with(8001, 5), open).result == TrayDropResult::ok,
            "the occupant's launch frees the slot the landing needs");
        // A marker held by another object would refuse the third command after the launch; it is caught first.
        const auto remembered = build([](auto& setup) {
            for (auto& row : setup.control->planets) if (row.planet == 101) row.ground_layout = {{8002, 5}};
        });
        const auto remembered_snapshot = remembered.snapshot();
        refused(view::plan_tray_landing(*remembered_snapshot, 1, 101, entry, pad_with(8001, 5), open),
            TrayDropResult::marker_taken, "a marker kept by another object is refused before the launch");
        galactic_test::expect(view::plan_tray_landing(*remembered_snapshot, 1, 101, entry, pad_with(8002, 5), open).result
            == TrayDropResult::ok, "the occupant's own marker entry is replaced");
    }
    return galactic_test::failures ? 1 : 0;
}
