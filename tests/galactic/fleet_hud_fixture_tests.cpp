#include "../../apps/viewer/src/galactic/planet_detail_layout.hpp"
#include "../../apps/viewer/src/galactic/fleet_traits.hpp"
#include "galactic_support.hpp"
#include "../presentation/galactic/fleet_hud_fixture.hpp"
#include "../presentation/galactic/ground_hud_fixture.hpp"
#include "galactic/ground_roster.hpp"
#include "../presentation/galactic/hud_clock_fixture.hpp"
#include "galactic/galaxy_view.hpp"
#include "eawr/platform/sim_workers.hpp"

int main() {
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
        ObservedFleetGroup ship; ship.type = 9; ship.count = 4;
        one.groups = {transport, ship};
        ObservedFleet two = one; two.orbit_slot = std::uint8_t{2}; two.groups = {transport}; two.groups[0].count = 1;
        ObservedFleet middle = one; middle.orbit_slot = std::uint8_t{1}; middle.groups = {transport};
        ObservedFleet foreign = one; foreign.local = false;
        const auto tray = view::land_tray_entries({one, middle, two, foreign}, 3, false);
        galactic_test::expect(tray.size() == 1 && tray[0].type == 7 && tray[0].count == 5 && !tray[0].enabled,
            "tray combines ground types, excludes ships and foreign fleets, and respects disabled state");
        galactic_test::expect(view::land_tray_entry(tray, 0) && !view::land_tray_entry(tray, 1), "unused tray slots hide");
        galactic_test::expect(!view::land_tray_enabled(true, 10, 10) && !view::land_tray_enabled(false, 1, 10)
            && !view::land_tray_enabled(true, {}, 10), "landed cap, view-only, and inaccessible count disable the tray");
        galactic_test::expect(view::land_tray_entries({one}, 4, true).empty(), "tray uses selected planet");
        galactic_test::expect(view::land_tray_slot_name(3) == "land_fleet_03" && !view::land_tray_takes_pointer,
            "tray slots remain read-only");
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
    return galactic_test::failures ? 1 : 0;
}
