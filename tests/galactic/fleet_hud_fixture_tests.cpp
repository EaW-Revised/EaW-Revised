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
        const std::vector<view::GroundCompany> companies{{11, 1000, true}, {12, 1000, true}, {13, 1000, true}};
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
        const std::vector<view::GroundCompany> full{{1, 1, true}, {2, 1, true}, {3, 1, true}};
        layout = view::place_pads(full, {}, 3);
        galactic_test::expect(view::move_pad(layout, 0, 2) && layout[0] == 1U && layout[1] == 2U && layout[2] == 0U,
            "a full row still rotates through the moved company's own pad");
    }
    return galactic_test::failures ? 1 : 0;
}
