// GC AI S3a: the world reader's structural index, fog bits and double-buffered view, against an ordered-map
// reference and against a cold world, over a sequence of motion, credit, queue-clock and roster changes. Every
// check repeats at 1/2/4/8 workers and in both fog sources.
#include "galactic_support.hpp"
#include "galactic_ai_world.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {
namespace ai = eawr::script::foc::ai;
namespace foc = eawr::script::foc;
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using ai::real;
using eawr::sim::math::Fixed;
using eawr::sim::math::Vec3;
Fixed whole(std::int64_t value) { return Fixed::from_integer(value).value(); }
template<class T> T must(eawr::core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(value).value();
}
void must(eawr::core::Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }

ai::GalacticPerceptionSetup setup(bool fog_traits) {
    ai::GalacticPerceptionSetup out; out.seed = 7; out.uses_fog = true;
    out.maximum_ground_level = out.maximum_space_level = real(5);
    out.players = {{1, 1, "Empire", "Normal", false, true, false, 10},
        {2, 2, "Rebel", "Normal", false, false, true, 20}, {3, 3, "Pirates", "Normal", true, false, false, 0}};
    for (const std::uint64_t id : {10, 20, 30, 40, 50, 60}) {
        ai::GalacticPlanetFacts facts; facts.planet = id; facts.name = "P" + std::to_string(id); facts.type = id * 10;
        out.planets.push_back(facts);
    }
    for (const auto id : {100U, 200U, 300U, 400U, 500U}) {
        foc::AiType named; named.type_id = id; named.name = "T" + std::to_string(id); out.types.push_back(named);
        ai::GalacticForceType force; force.type = id; force.categories = id == 200 ? 2U : 1U; force.power = real(id / 10);
        if (fog_traits && id == 300) force.object_traits = ai::GalacticObjectTraits{true, false};
        if (fog_traits && id == 400) force.object_traits = ai::GalacticObjectTraits{true, true};
        out.force_types.push_back(force);
    }
    out.maximum_fleet_distance = real(1000);
    return out;
}

struct Scene {
    std::vector<gc::Planet> planets;
    std::vector<gc::Fleet> fleets;
    std::shared_ptr<gc::MovementSetup> movement = std::make_shared<gc::MovementSetup>();
    std::optional<gc::PoliticalControlSetup> control;
    gc::ProductionSetup production;
    gc::PlayerEconomySetup accounts;
    std::optional<gc::ObserverSetup> observer;
    std::optional<gc::IncomeState> income{gc::IncomeState{}};
    std::vector<gc::ObservedPlanet> observations;
    std::uint64_t observer_generation{};
    std::uint64_t frame{};
    std::shared_ptr<const gc::GalacticSnapshot> snapshot() {
        ++frame;
        gc::GalacticClock clock; clock.session_tick = clock.galactic_frame = frame;
        auto generation = std::make_shared<gc::EconomyInputGeneration>();
        generation->census = generation->accounts = frame;
        std::vector<gc::PopulationTotal> population{{1, whole(100), {}, {}, false}, {2, whole(100), {}, {}, false}};
        auto seen = std::make_shared<const std::vector<gc::ObservedPlanet>>(observations);
        return std::make_shared<gc::GalacticSnapshot>(frame, clock,
            std::vector<gc::GalacticPlayer>{{1, 1, 0}, {2, 2, 0}, {3, 3, 0}}, planets, std::vector<gc::Route>{{900, 10, 20, whole(1)}},
            fleets, std::vector<gc::GalacticEvent>{}, std::nullopt, std::nullopt, movement, std::nullopt, std::vector<gc::Seat>{},
            accounts, production, control, std::vector<gc::PoliticalControlEffect>{}, population, observer, income, std::nullopt,
            std::vector<gc::ObservedProductionOffer>{}, seen, std::nullopt, std::shared_ptr<const std::vector<gc::CensusObject>>{},
            std::nullopt, std::nullopt, std::nullopt, std::shared_ptr<const gc::PlanetAccessState>{}, observer_generation, generation);
    }
};

Scene scene(bool with_observer) {
    Scene out;
    out.planets = {{10, {}, 1}, {20, {whole(100), {}, {}}, 2}, {30, {whole(200), {}, {}}, 1}, {40, {whole(300), {}, whole(5)}, 3},
        {50, {whole(400), whole(50), {}}, 2}, {60, {whole(500), whole(60), {}}, 3}};
    const auto fleet = [](gc::FleetId id, gc::PlayerId owner, gc::PlanetId orbit, std::vector<gc::FleetMember> members) {
        gc::Fleet value; value.id = id; value.owner = owner; value.orbit = orbit; value.members = std::move(members); return value;
    };
    out.fleets = {fleet(1000, 1, 10, {{1001, 100}, {1002, 300}}), fleet(1010, 2, 20, {{1011, 200}}),
        fleet(1020, 1, 30, {{1021, 400}, {1022, 100}}), fleet(1030, 2, 50, {{1031, 500}})};
    auto& traveller = out.fleets[2];
    traveller.motion.transit.emplace(); traveller.motion.position = {whole(10), whole(20), {}};
    auto& second = out.fleets[3]; second.motion.transit.emplace(); second.motion.position = {whole(30), {}, whole(1)};
    out.movement->contents = {{1002, {{1003, 200}, {1004, 500}}}, {1003, {{1005, 100}}}, {1021, {{1023, 300}}}};
    gc::PoliticalControlSetup control;
    gc::ControlPlanet surface; surface.planet = 20; surface.landed = {{2001, 500, 2}, {2002, 100, 2}};
    gc::ControlPlanet other; other.planet = 30; other.landed = {{2003, 300, 1}};
    control.planets = {surface, other};
    control.relations = {{1, 2, false}, {2, 1, false}};
    out.control = control;
    gc::PlanetProduction first; first.planet = 10; first.target_owner = first.starbase_owner = 1;
    first.enabled = first.surface_accessible = true; first.ground_slots = 5;
    first.objects = {{3001, 100, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure},
        {3002, 200, 1, {}, gc::ProductionDomain::space, gc::ProductionKind::structure}};
    gc::ProductionOffer offer; offer.type = 100; offer.domain = gc::ProductionDomain::ground; offer.price = whole(100);
    offer.seconds = whole(10); offer.affiliated = offer.direct_build_allowed = true;
    first.offers = {offer};
    auto second_planet = first; second_planet.planet = 20; second_planet.target_owner = second_planet.starbase_owner = 2;
    second_planet.objects = {{3003, 200, 2, {}, gc::ProductionDomain::space, gc::ProductionKind::structure}};
    gc::ProductionEntry queued; queued.type = 100; queued.builder = 1; queued.last_remaining = whole(8);
    first.queues[0].push_back(queued);
    out.production.maximum_human_queue = 4;
    gc::ProductionPlayer p1; p1.player = 1; p1.population_capacity = whole(100);
    auto p2 = p1; p2.player = 2; p2.human = true;
    out.production.players = {p1, p2};
    out.production.planets = {first, second_planet};
    out.income->inputs.offers = {{10, 100, whole(100), whole(10)}, {20, 100, whole(100), whole(10)}};
    gc::EconomicType reducer; reducer.type = 100;
    out.income->inputs.types = {reducer}; out.income->inputs.planets = {{10}, {20}};
    out.accounts.players = {{1, whole(500), whole(9999), 3, 5}, {2, whole(500), whole(9999), 3, 5}};
    if (with_observer) {
        gc::ObserverSetup observer; observer.local_player = 1;
        gc::ObserverPlayer a; a.player = 1; a.allies = {1};
        gc::ObserverPlayer b; b.player = 2; b.allies = {2};
        gc::ObserverPlayer c; c.player = 3; c.allies = {3, 2};
        observer.players = {a, b, c};
        gc::ObserverType stealth; stealth.type = 400; stealth.stealth_hero = true;
        observer.types = {stealth};
        out.observer = observer; out.observer_generation = 1;
        for (const gc::PlayerId viewer : {1U, 2U, 3U}) for (const std::uint64_t planet : {10, 20, 30, 40, 50, 60}) {
            gc::ObservedPlanet seen; seen.player = viewer; seen.planet = planet;
            seen.knowledge.ownership = (viewer + planet) % 3 != 0; seen.knowledge.fleet_presence = (viewer + planet) % 2 == 0;
            seen.knowledge.fleet_contents = (viewer * planet) % 4 == 0; seen.knowledge.ground_contents = (planet / 10 + viewer) % 3 == 1;
            seen.knowledge.structures = planet % 20 == 0; seen.knowledge.strongest_ship = viewer == 2;
            out.observations.push_back(seen);
        }
    }
    return out;
}

// The ordered-map and ordered-set resolution the roster used before the structural index.
struct Reference {
    struct Row { eawr::sim::EntityId id{}; std::uint64_t type{}; std::uint32_t owner{}; Vec3 position; gc::PlanetId planet{}; gc::IntelligenceContainer container{}; bool structure{}; };
    std::vector<Row> rows; std::set<eawr::sim::EntityId> transit;
};
Reference reference(const gc::GalacticSnapshot& snapshot, const ai::GalacticPerceptionSetup& setup) {
    Reference out; std::set<eawr::sim::EntityId> identities; std::map<eawr::sim::EntityId, gc::PlanetId> containment;
    const auto add = [&](eawr::sim::EntityId id, std::uint64_t type, std::uint32_t owner, const Vec3& position, gc::PlanetId containing,
                         gc::IntelligenceContainer container = gc::IntelligenceContainer::none, bool structure = false) {
        if (!identities.insert(id).second) return;
        out.rows.push_back({id, type, owner, position, containing, container, structure});
        if (containing) containment.emplace(id, containing);
    };
    const auto facts_type = [&](gc::PlanetId id) { for (const auto& row : setup.planets) if (row.planet == id) return row.type; return std::uint64_t{}; };
    const auto find_planet = [&](gc::PlanetId id) -> const gc::Planet* { for (const auto& row : snapshot.planets()) if (row.id == id) return &row; return nullptr; };
    for (const auto& row : snapshot.planets()) add(row.id, facts_type(row.id), row.owner, row.position, row.id);
    for (const auto& fleet : snapshot.fleets()) {
        const auto* at = find_planet(fleet.orbit);
        const auto containing = fleet.motion.transit ? gc::PlanetId{} : fleet.orbit;
        const auto position = fleet.motion.transit || !at ? fleet.motion.position : at->position;
        add(fleet.id, 0, fleet.owner, position, containing, gc::IntelligenceContainer::orbit);
        for (const auto& member : fleet.members) {
            add(member.id, member.type, fleet.owner, position, containing, gc::IntelligenceContainer::orbit);
            if (fleet.motion.transit) out.transit.insert(member.id);
        }
        if (fleet.motion.transit) out.transit.insert(fleet.id);
    }
    if (snapshot.control()) for (const auto& control : snapshot.control()->planets) {
        const auto* at = find_planet(control.planet); if (!at) continue;
        for (const auto& force : control.landed) add(force.id, force.type, force.owner, at->position, control.planet, gc::IntelligenceContainer::ground);
    }
    if (snapshot.production()) for (const auto& row : snapshot.planets()) for (const auto& at : snapshot.production()->planets) if (at.planet == row.id)
        for (const auto& object : at.objects) add(object.id, object.type, object.owner, row.position, row.id,
            object.domain == gc::ProductionDomain::space ? gc::IntelligenceContainer::orbit : gc::IntelligenceContainer::ground,
            object.kind == gc::ProductionKind::structure);
    if (snapshot.movement()) {
        std::map<eawr::sim::EntityId, const gc::MovementContents*> nested;
        for (const auto& contents : snapshot.movement()->contents) nested.emplace(contents.container, &contents);
        for (std::size_t index = 0; index < out.rows.size(); ++index) {
            const auto parent = out.rows[index];
            const auto found = nested.find(parent.id); if (found == nested.end()) continue;
            for (const auto& member : found->second->children) {
                if (identities.contains(member.id)) continue;
                const auto where = containment.find(parent.id);
                add(member.id, member.type, parent.owner, parent.position, where == containment.end() ? 0 : where->second, parent.container);
                if (out.transit.contains(parent.id)) out.transit.insert(member.id);
            }
        }
    }
    std::sort(out.rows.begin(), out.rows.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return out;
}
// The per-viewer fog the world computed with whole-table searches.
std::uint64_t reference_fog(const Reference::Row& source, const gc::GalacticSnapshot& snapshot, const ai::GalacticPerceptionSetup& setup,
    const std::vector<std::pair<std::uint32_t, gc::PlanetId>>& legacy) {
    std::uint64_t bits{};
    const auto find_traits = [&](std::uint64_t type) -> const ai::GalacticForceType* {
        for (const auto& row : setup.force_types) if (row.type == type) return &row;
        return nullptr; };
    for (std::size_t index = 0; index < setup.players.size(); ++index) {
        const auto viewer = setup.players[index].player;
        if (!snapshot.observer()) {
            for (const auto& [player, planet] : legacy) if (player == viewer && planet == source.planet) { bits |= std::uint64_t{1} << index; break; }
            continue;
        }
        const gc::ObserverPlayer* row{};
        for (const auto& candidate : snapshot.observer()->players) if (candidate.player == viewer) row = &candidate;
        const bool allied = source.owner == viewer || (row && std::binary_search(row->allies.begin(), row->allies.end(), source.owner));
        const gc::ObservedPlanet* known{};
        for (const auto& candidate : snapshot.observations()) if (candidate.player == viewer && candidate.planet == source.planet) { known = &candidate; break; }
        bool visible = allied;
        if (known && !visible) {
            if (source.id == source.planet) visible = known->knowledge.ownership;
            else {
                const auto* type = find_traits(source.type);
                const gc::ObserverType* live{};
                for (const auto& candidate : snapshot.observer()->types) if (candidate.type == source.type) live = &candidate;
                if (source.type == 0 || source.structure || (type && type->object_traits)) {
                    gc::ObjectIntelligenceContext object; object.planetary_system = source.planet != 0; object.structure = source.structure;
                    object.container = source.container;
                    if (type && type->object_traits) { object.hero = type->object_traits->hero; object.named_hero = type->object_traits->named_hero; }
                    object.stealth_hero = live && live->stealth_hero;
                    visible = gc::object_details_known(object, known->knowledge);
                }
            }
        }
        if (visible) bits |= std::uint64_t{1} << index;
    }
    return bits;
}

void compare(const ai::GalacticWorld& world, const ai::GalacticWorld& cold, const gc::GalacticSnapshot& snapshot,
    const ai::GalacticPerceptionSetup& config, const std::vector<std::pair<std::uint32_t, gc::PlanetId>>& legacy, std::string_view context,
    bool delta_expected_positions) {
    const auto label = [&](std::string_view what) { return std::string(context) + ": " + std::string(what); };
    const auto view = world.view(), cold_view = cold.view();
    expect(view && cold_view && view->tick == snapshot.completed_tick(), label("view tick is the completed tick"));
    if (!view || !cold_view) return;
    const auto expected = reference(snapshot, config);
    expect(view->units.size() == expected.rows.size() && cold_view->units.size() == expected.rows.size(), label("roster size equals the ordered reference"));
    expect(view->players.size() == cold_view->players.size(), label("viewer list equals cold"));
    for (std::size_t index = 0; index < std::min(view->units.size(), expected.rows.size()); ++index) {
        const auto& unit = view->units[index]; const auto& row = expected.rows[index];
        expect(unit.id == row.id && unit.type == row.type && unit.owner == row.owner, label("identity, type and owner equal the reference"));
        if (delta_expected_positions) expect(unit.position == row.position, label("position equals the reference"));
        expect(unit.visible_to == reference_fog(row, snapshot, config, legacy), label("fog bits equal the whole-table reference"));
        const auto& cold_unit = cold_view->units[index];
        expect(unit.id == cold_unit.id && unit.team == cold_unit.team && unit.visible_to == cold_unit.visible_to
            && (!delta_expected_positions || unit.position == cold_unit.position), label("unit equals the cold world's"));
        expect(world.containing_planet(row.id) == (row.planet ? std::optional{row.planet} : std::nullopt), label("containment equals the reference"));
        expect(world.arriving(row.id) == expected.transit.contains(row.id), label("transit membership equals the reference"));
    }
    expect(world.containing_planet(999999) == std::nullopt && !world.arriving(999999), label("unknown identity has no parent and is not enroute"));
}

void run(unsigned workers, bool with_observer) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    const auto config = setup(true);
    ai::GalacticWorld warm(config);
    warm.enable_live_deltas(300, std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers));
    std::vector<std::pair<std::uint32_t, gc::PlanetId>> legacy;
    if (!with_observer) {
        legacy = {{1, 10}, {2, 20}, {1, 30}, {3, 50}, {2, 0}};
        for (const auto& [player, planet] : legacy) warm.visibility_changed(player, planet, true, 0);
        warm.visibility_changed(1, 60, false, 0);
    }
    auto state = scene(with_observer);
    std::uint64_t held{};
    std::shared_ptr<const eawr::script::foc::detail::WorldView> kept; // a retained reader pins its publication
    const auto step = [&](std::string_view name, auto&& change, bool moved_positions = true, bool retain = false) {
        change(state);
        const auto snapshot = state.snapshot();
        must(warm.bind(snapshot, {}, executor, snapshot->completed_tick(), 5000));
        ai::GalacticWorld cold(config);
        if (!with_observer) {
            for (const auto& [player, planet] : legacy) cold.visibility_changed(player, planet, true, 0);
            cold.visibility_changed(1, 60, false, 0);
        }
        must(cold.bind(snapshot, {}, executor, snapshot->completed_tick(), 5000));
        compare(warm, cold, *snapshot, config, legacy, std::string(name) + (with_observer ? "/observer/" : "/fixture/") + std::to_string(workers), moved_positions);
        const auto a = warm.copy_input(), b = cold.copy_input();
        expect(a && b && a->credits == b->credits && a->targets.size() == b->targets.size(), std::string(name) + ": input equals cold");
        const std::array types{gc::TypeId{100}};
        for (const gc::PlayerId player : {1U, 2U}) for (const gc::PlanetId planet : {10U, 20U}) {
            const auto warm_rows = a->producers.read(player, planet, types), cold_rows = b->producers.read(player, planet, types);
            expect(warm_rows && cold_rows && warm_rows.value().size() == cold_rows.value().size()
                && std::equal(warm_rows.value().begin(), warm_rows.value().end(), cold_rows.value().begin(),
                    [](const auto& l, const auto& r) { return std::tie(l.player, l.planet, l.type, l.admission, l.cost, l.queue_seconds)
                        == std::tie(r.player, r.planet, r.type, r.admission, r.cost, r.queue_seconds); }),
                std::string(name) + ": producer rows equal cold, including queue clocks and credits");
        }
        if (retain) { kept = warm.view(); ++held; }
    };
    step("baseline", [](Scene&) {});
    step("motion", [](Scene& s) { s.fleets[2].motion.position.x = whole(15); });
    step("motion again", [](Scene& s) { s.fleets[3].motion.position.y = whole(9); });
    step("credits only", [](Scene& s) { s.accounts.players[0].credits = whole(450); });
    step("queue clock only", [](Scene& s) { s.production.planets[0].queues[0][0].last_remaining = whole(7); });
    step("queue entry added", [](Scene& s) {
        gc::ProductionEntry entry; entry.type = 100; entry.builder = 2; entry.last_remaining = whole(3); s.production.planets[1].queues[0].push_back(entry); });
    step("fleet changes orbit", [](Scene& s) { s.fleets[1].orbit = 30; });
    step("motion after roster change", [](Scene& s) { s.fleets[2].motion.position.y = whole(40); }, true, true);
    step("motion while a reader holds the view", [](Scene& s) { s.fleets[2].motion.position.y = whole(41); });
    step("motion again", [](Scene& s) { s.fleets[2].motion.position.y = whole(42); });
    step("member joins", [](Scene& s) { s.fleets[0].members.push_back({1006, 200}); });
    step("transit arrives", [](Scene& s) { s.fleets[2].motion.transit.reset(); s.fleets[2].orbit = 40; });
    step("quiet", [](Scene&) {});
    step("payload added", [](Scene& s) { auto next = std::make_shared<gc::MovementSetup>(*s.movement); next->contents.push_back({1011, {{1012, 100}}}); s.movement = next; });
    step("owner changes", [](Scene& s) { s.planets[1].owner = 1; });
    if (with_observer) step("observation changes", [](Scene& s) { s.observations[3].knowledge.ownership = !s.observations[3].knowledge.ownership; ++s.observer_generation; });
    step("production object added", [](Scene& s) { s.production.planets[0].objects.push_back({3009, 300, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::unit}); });
    step("duplicate identity resolves to the first row", [](Scene& s) { s.fleets[1].members.push_back({1001, 100}); });
    expect(held == 1 && kept, "the pinned publication stays readable");
}

void identity_contracts() {
    // An identity that two sources claim resolves once, to the first claim, and transit membership follows the fleet.
    auto state = scene(true);
    state.fleets[2].members.push_back({1001, 100});
    const auto snapshot = state.snapshot();
    ai::GalacticWorld world(setup(false));
    eawr::platform::ThreadWorkerAdapter executor(2);
    must(world.bind(snapshot, {}, executor, 1, 5000));
    const auto expected = reference(*snapshot, setup(false));
    expect(world.view()->units.size() == expected.rows.size(), "duplicated identity appears once");
    expect(world.containing_planet(1001) == std::optional<gc::PlanetId>{10}, "the first claim's parent wins");
    expect(world.arriving(1001), "a later claim by an enroute fleet still marks the identity enroute (roster order is preserved)");
}
} // namespace

int main() {
    try {
        for (const unsigned workers : {1U, 2U, 4U, 8U}) for (const bool with_observer : {true, false}) run(workers, with_observer);
        identity_contracts();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (galactic_test::failures) { std::cerr << galactic_test::failures << " failure(s)\n"; return 1; }
    std::cout << "galactic world reader contracts passed\n";
    return 0;
}
