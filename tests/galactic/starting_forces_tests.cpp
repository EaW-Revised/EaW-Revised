#include "galactic_support.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/campaign/tactical_return_live.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../../src/sim/galactic/encoding.hpp"
#include "../../src/sim/galactic/native_actor_change_checkpoint.hpp"

#include <algorithm>
#include <limits>
#include <cstdlib>
#include <tuple>

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using Fixed = eawr::sim::math::Fixed;
Fixed f(std::int64_t n) { return Fixed::from_integer(n).value(); }
void native_actor_change_rollback() {
    using Change = gc::GalacticSession::NativeActorChange;
    using Checkpoint = gc::session_detail::NativeActorChangeCheckpoint;
    const Checkpoint::Changes prior{{Change::Kind::changed, 41, 0, false, {}, true},
        {Change::Kind::created, 42, 17, {}, false, {}}, {Change::Kind::lost, 43, 0, true, true, false}};
    const auto equal = [](const auto& left, const auto& right) {
        return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](const auto& a, const auto& b) {
            return std::tie(a.kind, a.actor, a.source, a.dead, a.deleting, a.death_clone)
                == std::tie(b.kind, b.actor, b.source, b.dead, b.deleting, b.death_clone);
        });
    };
    for (const bool prepared : {false, true}) {
        auto pending = prior;
        const Checkpoint checkpoint(pending, prepared);
        pending.erase(pending.begin() + 1); // coalescing can erase inside the saved frontier
        const auto erased = pending;
        checkpoint.restore();
        expect(equal(pending, prepared ? prior : erased), "prepared rollback restores erased entries and every prior field in order");
        pending.assign({{Change::Kind::created, 99, 0, {}, {}, {}}});
        pending.insert(pending.end(), prior.begin(), prior.end());
        const auto appended = pending;
        checkpoint.restore();
        expect(equal(pending, prepared ? prior : appended), "prepared rollback removes duplicate births after in-place replacement");
    }
    Checkpoint::Changes empty;
    const Checkpoint checkpoint(empty, true);
    empty.push_back(prior.front()); checkpoint.restore();
    expect(empty.empty(), "prepared rollback retains an empty notification frontier");
}
struct Fixture {
    cp::StartingForceInputs inputs;
    cp::CampaignPlacementContext context;
    gc::GalacticSetup galaxy;
    Fixture() {
        const auto add = [&](gc::TypeId id, const char* name, const char* behavior) {
            eawr::units::GalacticType type; type.name = name; type.behavior = {behavior};
            inputs.types.types.push_back(type); inputs.bindings.push_back({id, name});
            inputs.runtime_types.push_back({id, false, false, {}});
        };
        add(7, "Ship", "DUMMY_STARSHIP"); add(8, "Troop", "LAND_UNIT");
        add(9, "Carrier", "TRANSPORT"); add(10, "Company", "DUMMY_GROUND_COMPANY");
        add(11, "Squadron", "DUMMY_SPACE_FIGHTER_SQUADRON");
        add(12, "GroundStructure", "DUMMY_GROUND_STRUCTURE"); add(13, "Starbase", "DUMMY_STAR_BASE");
        add(14, "SpaceStructure", "DUMMY_ORBITAL_STRUCTURE"); add(15, "GroundBase", "DUMMY_GROUND_BASE");
        add(16, "GalacticFleet", "FLEET"); add(17, "GalacticSquadron", "SQUADRON");
        source(10).company_transport = "Carrier"; source(10).company_units = {"Troop", "Troop"};
        source(11).squadron_units = {"Ship", "Ship"}; source(12).political_control = f(2);
        source(13).base_level = 3; source(14).political_control = f(1);
        inputs.fleet_type = 16; inputs.squadron_type = 17;
        source(7).object_class = "SpaceUnit"; source(8).object_class = "GroundUnit";
        source(9).object_class = "TransportUnit"; source(10).object_class = "GroundCompany";
        source(11).object_class = "Squadron"; source(16).object_class = "Container";
        context.control.max_ground_forces = 2;
        for (gc::PlayerId a : {1U, 2U, 3U}) for (gc::PlayerId b : {1U, 2U, 3U})
            context.control.relations.push_back({a, b, a == b});
        for (gc::PlayerId a : {1U, 2U, 3U}) for (gc::PlayerId b : {1U, 2U, 3U})
            inputs.relations.push_back({a, b, a != b && a != 3 && b != 3});
        gc::ControlPlanet control; control.planet = 1; control.surface_accessible = true;
        context.control.planets.push_back(control);
        gc::PlanetProduction production; production.planet = 1; production.ground_slots = 3; production.space_slots = 3;
        context.production.planets.push_back(production);
        galaxy.control = context.control; galaxy.movement = context.movement;
    }
    eawr::units::GalacticType& source(gc::TypeId id) { return inputs.types.types[id - 7]; }
    cp::StartingForceRuntimeType& live(gc::TypeId id) { return inputs.runtime_types[id - 7]; }
    cp::StartingForceRequest request(gc::TypeId id) const {
        return {1, 1, id, inputs.types.types[id - 7].name, true, f(1)};
    }
    auto place(gc::TypeId type, eawr::sim::EntityId next = 100) const {
        return cp::place_starting_force(inputs, request(type), next, galaxy, context);
    }
};
void reconciled_placement_contracts() {
    Fixture fixture;
    auto inputs = std::make_shared<const cp::StartingForceInputs>(fixture.inputs);
    const auto creation = cp::prepare_nested_creation(*inputs);
    expect(bool(creation), "reconciliation fixture has qualified constructor facts");
    if (!creation) return;
    const gc::Planet planet{1, {}, 1};
    const auto view = [&] {
        return gc::GalacticSession::NativeForceView{planet, fixture.context.control.planets[0], fixture.context.production.planets[0],
            creation.value(), fixture.context.control.max_ground_forces, fixture.galaxy.fleets, fixture.context.movement.contents, {}};
    };
    const gc::GalacticSession::NativeProductionRequest request{1, 1, 12, {}};
    const auto legacy = cp::stock_native_force_shadow_builder(inputs, request);
    const auto reconcile = cp::stock_native_force_reconciliation_builder(inputs, request);
    auto old = legacy(view(), 100); auto accepted = reconcile(view(), 100);
    expect(old && accepted && accepted.value() && accepted.value()->next_id == old.value().next_id
        && accepted.value()->structures == old.value().structures && accepted.value()->originals == old.value().originals,
        "typed reconciliation placement preserves successful constructor output");
    fixture.context.production.planets[0].ground_slots = 0;
    const auto excess_before = legacy(view(), 100);
    const auto ordinary = reconcile(view(), 100);
    const auto excess_after = legacy(view(), 100);
    expect(excess_before && excess_after && ordinary && ordinary.value()
        && ordinary.value()->next_id == 101
        && ordinary.value()->structures == excess_before.value().structures
        && excess_after.value().structures == excess_before.value().structures,
        "CGS-20: direct and reconciled creation retain the same excess structure without mutating inputs");
    const auto missing = cp::stock_native_force_reconciliation_builder(inputs, {1, 1, 99999, {}})(view(), 100);
    expect(!missing && missing.error().code == std::string(gc::diagnostic_codes::invalid_setup),
        "unbound constructor content remains an error rather than an ordinary refusal");
    fixture.context.production.planets[0].ground_slots = 3;
    const auto base = cp::stock_native_force_reconciliation_builder(inputs, {1, 1, 13, {}})(view(), 100);
    expect(base && base.value() && base.value()->next_id == 100 && base.value()->starbase,
        "a zero-allocation base write remains an admitted placement");
}
void allocation_and_identity() {
    Fixture fixture;
    auto ship = fixture.place(7);
    expect(ship && ship.value().next_id == 102 && ship.value().fleets[0].id == 100
        && ship.value().fleets[0].members[0].id == 101, "ship allocates fleet before its member");
    auto squadron = fixture.place(11);
    expect(squadron && squadron.value().next_id == 104 && squadron.value().contents[0].container == 101
        && squadron.value().contents[0].children[0].id == 102 && squadron.value().contents[0].children[1].id == 103
        && squadron.value().fleets[0].members[0].type == 11, "squadron keeps logical template and ordered live craft");
    expect(squadron && squadron.value().originals[0].current_type == 16
        && squadron.value().originals[1].current_type == 17
        && squadron.value().originals[1].original_type == 11
        && squadron.value().originals[2].current_type == 7
        && squadron.value().originals[2].generated_source == 0,
        "current squadron container differs from strategic template; production children use ordinary births");
    auto company = fixture.place(10);
    expect(company && company.value().next_id == 103 && company.value().fleets.empty()
        && company.value().landed[0] == gc::SurfaceForce{100, 9, 1}
        && company.value().originals[0].original_type == 10 && company.value().originals[0].container == 1
        && company.value().originals[1].container == 100, "landed company retains original company on its actual carrier");
    expect(company && company.value().originals[0].current_type == 9
        && company.value().originals[1].current_type == 8,
        "company transport and team-member identities use their actual constructed types");
    fixture.context.control.planets[0].surface_accessible = false;
    auto orbit = fixture.place(10);
    expect(orbit && orbit.value().next_id == 104 && orbit.value().fleets[0].id == 103
        && orbit.value().fleets[0].members[0].id == 100 && orbit.value().fleets[0].orbit_slot == 0,
        "orbiting company creates outer fleet after carrier and ordered children");
    expect(!fixture.place(7, std::numeric_limits<eawr::sim::EntityId>::max() - 1), "exhausted frontier refuses atomically");
    expect(fixture.galaxy.fleets.empty() && fixture.context.census.objects.empty(), "pure placement never mutates input state");
}
void guards_and_hostility() {
    Fixture fixture; fixture.live(8).always_spawn_in_orbit = true;
    expect(fixture.place(10).value().landed.empty(), "ordinary always-orbit member stays in orbit without hostile presence");
    fixture.galaxy.fleets = {{50, 2, 1, {{51, 7}}, 1, {}}};
    expect(fixture.place(10).value().landed.size() == 1, "hostile non-stealth fleet allows ordinary always-orbit company to land");
    fixture.galaxy.fleets[0].owner = 3;
    expect(fixture.place(10).value().fleets.size() == 1, "neutral non-ally is not a hostile orbital owner");
    fixture.galaxy.fleets[0].owner = 2;
    fixture.live(8).skips_combat = true;
    auto guard = fixture.place(10);
    expect(guard && guard.value().fleets[0].orbit_slot == 2, "stealth member selects first globally empty guard slot");
    fixture.galaxy.fleets.push_back({52, 3, 1, {{53, 7}}, 2, {}});
    expect(fixture.place(10).value().fleets[0].orbit_slot == 0, "full guard slots fall back to normal orbit");
    fixture.live(8).skips_combat = false; fixture.live(7).skips_combat = true;
    expect(fixture.place(10).value().fleets.size() == 1, "hostile fleet that skips combat does not prevent always-orbit placement");
    fixture.galaxy.fleets[0].members.push_back({54, 9});
    expect(fixture.place(10).value().landed.size() == 1, "an empty transport prevents a hostile fleet from skipping combat");
    fixture.inputs.relations.clear();
    expect(!fixture.place(10), "missing enemy relation cannot default to allied or hostile");
    for (gc::PlayerId a : {1U, 2U, 3U}) for (gc::PlayerId b : {1U, 2U, 3U})
        fixture.inputs.relations.push_back({a, b, a != b && a != 3 && b != 3});
    fixture.live(8).always_spawn_in_orbit = false;
    fixture.context.control.planets[0].landed = {{60, 9, 1}, {61, 9, 1}};
    expect(fixture.place(10).value().fleets.size() == 1, "full surface list forces orbit placement");
}
void duplicate_authored_starbases() {
    // WGS-180: renamed types and changing owners distinguish last-wins from
    // first-wins, highest-level and owner-matched selection.
    for (const std::int64_t second_level : {2, 3, 4}) {
        Fixture fixture;
        auto second = fixture.source(13); second.name = "OtherStation"; second.base_level = second_level;
        fixture.inputs.types.types.push_back(second);
        fixture.inputs.bindings.push_back({18, second.name});
        fixture.inputs.runtime_types.push_back({18, false, false, {}});
        const auto first = fixture.place(13);
        expect(first && cp::apply_starting_force_placement(fixture.inputs, fixture.request(13), first.value(),
            fixture.galaxy, fixture.context), "first authored station commits");
        const cp::StartingForceRequest request{1, 2, 18, second.name, false, f(1)};
        auto placed = cp::place_starting_force(fixture.inputs, request, 100, fixture.galaxy, fixture.context);
        expect(placed && placed.value().starbase == gc::ControlBase{2, static_cast<std::uint32_t>(second_level)}
            && placed.value().starbase_type == 18 && !placed.value().created && placed.value().next_id == 100,
            "second authored station replaces type, level and supplied owner without allocation");
        if (!placed) continue;
        expect(bool(cp::apply_starting_force_placement(fixture.inputs, request, placed.value(),
            fixture.galaxy, fixture.context)), "second authored station commits");
        const auto& production = fixture.context.production.planets.front();
        expect(fixture.context.control.planets.front().starbase == placed.value().starbase
            && production.starbase_type == 18 && production.starbase_owner == 2
            && production.space_level == static_cast<std::uint32_t>(second_level),
            "control and production retain the same last authored station");
    }
    Fixture invalid;
    invalid.source(13).base_level.reset(); expect(!invalid.place(13), "missing station level refuses");
    invalid.source(13).base_level = -1; expect(!invalid.place(13), "negative station level refuses");
    invalid.source(13).base_level = 0; expect(!invalid.place(13), "zero station level refuses");
    invalid.source(13).base_level = 6; expect(!invalid.place(13), "out-of-range station level refuses");
}
void level_one_starbase_clears_space_structures() {
    // CGS-10: a base below level two clears the planet's space special structures; ground structures stay.
    Fixture fixture;
    fixture.source(13).base_level = 1;
    const auto commit = [&](const gc::TypeId type, const eawr::sim::EntityId next) {
        auto placed = fixture.place(type, next);
        expect(bool(placed), "authored structure or base places");
        if (!placed) return placed;
        expect(bool(cp::apply_starting_force_placement(fixture.inputs, fixture.request(type), placed.value(), fixture.galaxy,
            fixture.context)), "authored structure or base commits");
        return placed;
    };
    commit(12, 100);
    const auto space = commit(14, 101);
    auto& production = fixture.context.production.planets.front();
    expect(production.objects.size() == 2, "a ground and a space structure are installed before the base");
    const auto base = commit(13, 102);
    expect(base && base.value().clear_space_structures && base.value().special_control == std::vector<Fixed>{f(2)},
        "a level-one base clears space structures and keeps the ground structure's control");
    expect(production.objects.size() == 1 && production.objects.front().domain == gc::ProductionDomain::ground,
        "only the ground structure remains in production");
    expect(std::none_of(fixture.context.census.objects.begin(), fixture.context.census.objects.end(),
        [&](const auto& object) { return space && object.id == space.value().primary; }), "the cleared structure leaves the census");
    expect(fixture.context.control.planets.front().special_control == std::vector<Fixed>{f(2)},
        "planet special control is recomputed from the remaining structures");
    // A chain whose space structure is followed by a level-one base clears the structure it placed itself: the combined
    // result is applied once, so the placement must not carry the cleared structure.
    Fixture chained;
    chained.source(13).base_level = 1; chained.source(14).additional_creation_type = "Starbase";
    auto together = chained.place(14);
    expect(together && together.value().clear_space_structures && together.value().structures.empty()
        && together.value().starbase == gc::ControlBase{1, 1}, "a chained level-one base clears the space structure placed before it");
    if (together) {
        expect(together.value().cleared_structures == std::vector<eawr::sim::EntityId>{100}
            && together.value().originals.size() == 1, "the cleared chain structure keeps its allocation row but is listed as cleared");
        expect(bool(cp::apply_starting_force_placement(chained.inputs, chained.request(14), together.value(), chained.galaxy,
            chained.context)), "the chained placement commits");
        expect(chained.context.production.planets.front().objects.empty() && chained.context.census.objects.empty(),
            "the real planet keeps no cleared structure and no census row for it");
    }
    Fixture raised;
    raised.source(13).base_level = 2;
    const auto kept = raised.place(13);
    expect(kept && !kept.value().clear_space_structures, "a level-two base clears nothing");
}
void bases_structures_and_chains() {
    Fixture fixture;
    auto base = fixture.place(13);
    expect(base && !base.value().created && base.value().next_id == 100 && base.value().starbase == gc::ControlBase{1, 3},
        "starbase updates typed level without allocating an object");
    expect(!fixture.place(15), "direct debug ground-base assertion remains explicit refusal");
    auto ground = fixture.place(12);
    expect(ground && ground.value().structures.size() == 1 && ground.value().ground_base == gc::ControlBase{1, 1}
        && ground.value().special_control == std::vector<Fixed>{f(2)}, "ground special installs base level and authored control");
    if (ground) expect(bool(cp::apply_starting_force_placement(fixture.inputs, fixture.request(12), ground.value(),
        fixture.galaxy, fixture.context)), "typed ground structure commits");
    auto space = fixture.place(14, 101);
    expect(space && space.value().special_control == std::vector<Fixed>{f(2), f(1)},
        "special control retains ground then space category order");
    fixture.context.production.planets[0].ground_slots = 1;
    expect(bool(fixture.place(12, 102)), "CGS-20: exhausted special slots retain direct creation");
    fixture.context.control.planets[0].surface_accessible = false;
    auto skipped = fixture.place(12, 101);
    expect(skipped && !skipped.value().created && skipped.value().next_id == 101, "inaccessible ground structure creates nothing");
    Fixture chain; chain.source(13).additional_creation_type = "Ship";
    auto additional = chain.place(13);
    expect(additional && additional.value().starbase && additional.value().next_id == 102
        && additional.value().fleets.size() == 1 && additional.value().primary == 0,
        "additional creation follows primary dispatch without replacing a null primary");
    Fixture primary_chain; primary_chain.source(7).additional_creation_type = "Squadron";
    auto primary_extra = primary_chain.place(7);
    expect(primary_extra && primary_extra.value().primary == 100 && primary_extra.value().fleets.size() == 2,
        "recursive additional fleet cannot replace the primary ship fleet");
    chain.source(7).additional_creation_type = "Starbase";
    expect(!chain.place(13), "cyclic additional creation refuses without partial commit");
    Fixture unknown; unknown.inputs.runtime_types.erase(unknown.inputs.runtime_types.begin() + 1);
    expect(!unknown.place(10), "missing nested runtime predicate refuses");
    unknown.source(10).company_transport = "Absent";
    expect(!unknown.place(10), "unbound company carrier refuses");
}
void excess_special_structures() {
    // CGS-20: use renamed types, repeated records, both domains and supplied
    // owners unrelated to the existing controller. Capacity is not placement admission.
    for (const bool ground : {false, true}) for (const std::int64_t slots : {0, 1}) {
        Fixture fixture;
        const gc::TypeId type = ground ? 12U : 14U;
        fixture.source(type).name = "AuthoredFacility";
        fixture.source(type).political_control = Fixed{};
        fixture.inputs.bindings[type - 7].original_type = "AuthoredFacility";
        auto& production = fixture.context.production.planets.front();
        production.ground_slots = slots; production.space_slots = slots;
        fixture.context.control.planets.front().ground_base = gc::ControlBase{3, 0};
        fixture.galaxy.control = fixture.context.control;
        for (eawr::sim::EntityId i = 0; i < 7; ++i) {
            auto request = fixture.request(type); request.owner = static_cast<gc::PlayerId>(i % 3 + 1);
            request.human = i % 2 == 0;
            const auto placed = cp::place_starting_force(fixture.inputs, request, 100 + i, fixture.galaxy, fixture.context);
            expect(placed && placed.value().created && placed.value().next_id == 101 + i
                && placed.value().primary == 100 + i && placed.value().structures.size() == 1,
                "CGS-20: every excess authored record allocates exactly one retained structure");
            if (!placed) break;
            expect(bool(cp::apply_starting_force_placement(fixture.inputs, request, placed.value(), fixture.galaxy,
                fixture.context)), "CGS-20: excess structure commits to the ordered planet list");
            expect(production.objects.size() == i + 1 && fixture.context.census.objects.size() == i + 1,
                "CGS-20: no earlier structure is displaced or skipped");
            if (production.objects.size() != i + 1 || fixture.context.census.objects.size() != i + 1) break;
            const auto& object = production.objects.back();
            const auto& actor = fixture.context.census.objects.back();
            expect(object.id == 100 + i && object.owner == request.owner && object.type == type
                && object.domain == (ground ? gc::ProductionDomain::ground : gc::ProductionDomain::space)
                && actor.id == object.id && actor.owner == request.owner && actor.original_type == type && actor.container == 1,
                "CGS-20: ordered duplicates retain supplied faction and literal census identity");
        }
        expect(production.ground_slots == slots && production.space_slots == slots,
            "CGS-20: retaining excess structures never inflates the authored purchase capacities");
        expect(!ground || fixture.context.control.planets.front().ground_base == gc::ControlBase{1, 5},
            "CGS-20: ground base level remains bounded while all seven structures remain present");
        const auto inputs = std::make_shared<const cp::StartingForceInputs>(fixture.inputs);
        const auto creation = cp::prepare_nested_creation(*inputs);
        expect(bool(creation), "excess native constructor inputs prepare");
        if (!creation) continue;
        const gc::Planet planet{1, {}, 3};
        const gc::GalacticSession::NativeForceView view{planet, fixture.context.control.planets.front(), production,
            creation.value(), fixture.context.control.max_ground_forces, fixture.galaxy.fleets, fixture.context.movement.contents, {}};
        const gc::GalacticSession::NativeProductionRequest request{2, 1, type, {}};
        const auto legacy = cp::stock_native_force_shadow_builder(inputs, request)(view, 107);
        const auto reconciled = cp::stock_native_force_reconciliation_builder(inputs, request)(view, 107);
        expect(legacy && reconciled && reconciled.value() && legacy.value().next_id == 108
            && reconciled.value()->structures == legacy.value().structures && legacy.value().structures.front().owner == 2,
            "CGS-20: both native direct-creation adapters retain over-capacity objects too");
    }
    Fixture chain;
    chain.context.production.planets.front().ground_slots = 0;
    chain.context.production.planets.front().space_slots = 0;
    chain.source(12).additional_creation_type = "SpaceStructure";
    const auto placed = chain.place(12);
    expect(placed && placed.value().primary == 100 && placed.value().next_id == 102
        && placed.value().structures.size() == 2 && placed.value().structures[0].domain == gc::ProductionDomain::ground
        && placed.value().structures[1].domain == gc::ProductionDomain::space,
        "CGS-20: additional creation continues across both full domains and retains the primary identity");
}
std::vector<std::uint8_t> census_section(const gc::GalacticSession& world) {
    const auto bytes = world.canonical_state_bytes();
    const auto read = [&](std::size_t at, unsigned count) {
        std::uint64_t value{};
        for (unsigned i = 0; i < count; ++i) value |= std::uint64_t{bytes.at(at + i)} << (i * 8);
        return value;
    };
    std::size_t at = 16;
    for (std::uint64_t n = 0; n < read(12, 4); ++n) {
        const auto id = read(at, 2), size = read(at + 4, 8); at += 12;
        if (id == static_cast<unsigned>(gc::StateSection::census))
            return {bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + size)};
        at += static_cast<std::size_t>(size);
    }
    return {};
}
void representation_contracts(const gc::GalacticSetup& setup, std::shared_ptr<const cp::StartingForceInputs> inputs,
    const eawr::sim::PartitionExecutor& executor) {
    auto created = gc::GalacticSession::create(setup); expect(bool(created), "representation world constructs"); if (!created) return;
    auto world = std::move(created).value(); world.set_hash_verification(true);
    const auto rows = world.snapshot()->actor_records();
    const auto company = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.original_type == 10; });
    expect(company != rows.end(), "representation fixture contains company"); if (company == rows.end()) return;
    const auto actor = company->id;
    auto duplicate = setup;
    duplicate.census->objects.insert(duplicate.census->objects.begin(), duplicate.census->objects.front());
    expect(!gc::validate_setup(duplicate) && !gc::GalacticSession::create(duplicate),
        "duplicate initial actor identity refuses before constructing the record map");
    std::vector<gc::GalacticSession::NativeActorChange> changes; unsigned deliveries{};
    world.set_native_actor_changes_handler([&](const gc::GalacticSnapshot& view, auto payload) {
        ++deliveries;
        for (const auto& change : payload) {
            if (change.kind != gc::GalacticSession::NativeActorChange::Kind::lost)
                expect(bool(view.current_type(change.actor)), "changed actors have completed literal identity");
            changes.push_back(change);
        }
        return eawr::core::Result<void>::success();
    });
    const auto retained = world.snapshot(); const auto hash = world.state_sha256();
    expect(!world.snapshot()->current_type(0) && !world.change_actor_current_type(actor, 0)
        && !world.change_actor_current_type(actor, 99999), "missing and unbound current identity refuse");
    expect(world.state_sha256() == hash && deliveries == 0, "unqualified type changes are atomic");
    expect(bool(world.change_actor_current_type(actor, 8)) && world.snapshot()->current_type(actor).value() == 8
        && retained->current_type(actor).value() == 9, "literal type commit keeps immutable prior snapshot");
    expect(world.snapshot()->actor_records()[static_cast<std::size_t>(company - rows.begin())].original_type == 10
        && changes.size() == 1 && deliveries == 1 && world.cached_state_hash().get() == world.state_sha256(),
        "current change preserves original and invalidates only committed hash state");
    auto game = cp::GameSession::create(setup); expect(bool(game), "representation save owner constructs"); if (!game) return;
    auto section = census_section(world);
    expect(bool(game.value().restore_actor_representations(section)), "saved current facts apply after world reference reconstruction");
    const auto ids = eawr::checkpoint::game_identities(setup);
    auto saved = eawr::checkpoint::save_game_checkpoint(game.value(), ids);
    auto restored = saved ? eawr::checkpoint::load_game_checkpoint(saved.value(), ids, {}, executor)
        : eawr::core::Result<eawr::checkpoint::GameRestore>::failure(saved.error());
    expect(restored && restored.value().session.galaxy().snapshot()->current_type(actor).value() == 8,
        "disk restore preserves current literal distinct from its constructor type");
    if (restored) {
        const auto a = game.value().step(executor), b = restored.value().session.step(executor);
        expect(a && b && a.value().state_hash.get() == b.value().state_hash.get(), "current-type load continuation equals uninterrupted owner");
    }
    const auto before_restore = world.state_sha256(); const auto notification_count = deliveries;
    const auto u32 = [&](std::size_t at) {
        std::uint32_t value{};
        for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t{section.at(at + i)} << (i * 8);
        return value;
    };
    const auto census_size = u32(0);
    auto census = gc::detail::decode_census(std::span<const std::uint8_t>(section).subspan(4, census_size));
    expect(census && census->objects.size() >= 2, "duplicate restore fixture contains two actor rows");
    if (!census || census->objects.size() < 2) return;
    expect(census->objects[0].id != census->objects[1].id,
        "duplicate restore fixture starts with distinct actor identities");
    census->objects[1].id = census->objects[0].id;
    std::vector<std::uint8_t> encoded;
    gc::detail::encode_census(encoded, *census);
    std::vector<std::uint8_t> duplicate_section;
    gc::detail::put_u32(duplicate_section, static_cast<std::uint32_t>(encoded.size()));
    duplicate_section.insert(duplicate_section.end(), encoded.begin(), encoded.end());
    duplicate_section.insert(duplicate_section.end(), section.begin() + 4 + census_size, section.end());
    section = std::move(duplicate_section);
    expect(!world.restore_actor_representations(section) && world.state_sha256() == before_restore
        && deliveries == notification_count, "duplicate saved actor identity refuses atomically");
    section.resize(3);
    expect(!world.restore_actor_representations(section) && world.state_sha256() == before_restore
        && deliveries == notification_count, "malformed restore refuses without publication or callbacks");
    const gc::GalacticSession::NativeProductionRequest request{1, 1, 10, {}};
    auto private_world = gc::GalacticSession::create(setup); if (!private_world) return;
    auto& private_owner = private_world.value(); unsigned births{};
    private_owner.set_native_actor_changes_handler([&](const auto&, auto payload) {
        births += static_cast<unsigned>(payload.size()); return eawr::core::Result<void>::success();
    });
    auto shadow = private_owner.begin_native_actions(4).value(); const auto frontier = shadow.next_entity_id();
    auto builder = cp::stock_native_force_shadow_builder(inputs, request);
    auto unqualified = [&](const auto& view, auto first) {
        auto placed = builder(view, first);
        if (placed) placed.value().originals.front().current_type = 99999;
        return placed;
    };
    expect(!shadow.create_force(request, unqualified, executor) && shadow.next_entity_id() == frontier
        && shadow.actor_changes().empty(), "unqualified private current type cannot consume identity or birth frontier");
    auto cycle = [&](const auto& view, auto first) {
        auto placed = builder(view, first);
        if (placed && placed.value().originals.size() > 2) {
            placed.value().originals[1].generated_source = placed.value().originals[2].id;
            placed.value().originals[2].generated_source = placed.value().originals[1].id;
        }
        return placed;
    };
    expect(!shadow.create_force(request, cycle, executor) && shadow.next_entity_id() == frontier,
        "cyclic generated-source inheritance refuses before either owner commits");
    auto generated = [&](const auto& view, auto first) {
        auto placed = builder(view, first);
        if (placed) for (auto& row : placed.value().originals)
            if (row.container == first && row.id != first) row.generated_source = first;
        return placed;
    };
    auto made = shadow.create_force(request, generated, executor);
    expect(made && shadow.created_object(frontier) && shadow.created_object(frontier)->current_type == 9,
        "private constructor exposes transport current identity before publication");
    expect(!private_owner.snapshot()->current_type(frontier) && births == 0, "private creation has no world callbacks");
    bool source_seen{};
    for (const auto& change : shadow.actor_changes()) {
        if (change.actor == frontier) source_seen = true;
        if (change.source) expect(source_seen && shadow.created_object(change.actor)->generated_source == frontier,
            "qualified generated source precedes child and is readable in same private pass");
    }
    expect(made && bool(private_owner.commit_native_actions(shadow, true)) && births == 0
        && private_owner.snapshot()->current_type(frontier).value() == 9,
        "prepared private commit suppresses exactly its duplicate birth frontier");
    auto second = private_owner.begin_native_actions(4).value(); const auto second_id = second.next_entity_id();
    auto ordinary = second.create_force(request, builder, executor);
    expect(ordinary && bool(private_owner.commit_native_actions(second)) && births == second.actor_changes().size(),
        "ordinary commit delivers its sparse births exactly once");
    const auto stable = private_owner.snapshot()->actor_records().data(); const auto before_idle = births;
    expect(bool(private_owner.step(executor)) && private_owner.snapshot()->actor_records().data() == stable && births == before_idle,
        "idle tick reuses identity publication with no actor callback work");
    const std::vector<eawr::sim::EntityId> loss{second_id};
    expect(bool(private_owner.apply_battle_losses(loss)) && !private_owner.snapshot()->current_type(second_id)
        && births > before_idle, "actual loss retires identity and emits a separate sparse loss");
    auto transitions = gc::GalacticSession::create(setup); if (!transitions) return;
    auto& moving = transitions.value(); moving.set_hash_verification(true);
    unsigned transition_batches{};
    moving.set_native_actor_changes_handler([&](const auto&, auto) {
        ++transition_batches; return eawr::core::Result<void>::success();
    });
    const auto launch = moving.launch_native_transport(1, actor);
    expect(launch && launch.value().admitted && moving.snapshot()->current_type(actor).value() == 9
        && transition_batches == 1, "boarding publishes one completed batch and keeps current transport type");
    if (launch && launch.value().admitted) {
        const auto fleet = launch.value().object;
        const auto records = moving.snapshot()->actor_records();
        const auto parent = std::find_if(records.begin(), records.end(), [&](const auto& row) { return row.id == actor; });
        expect(parent != records.end() && parent->container == fleet && moving.snapshot()->current_type(fleet).value() == 16,
            "boarding records typed fleet shell and literal immediate parent");
        const auto landed = moving.land_native_transports(1, fleet);
        expect(landed && landed.value().admitted && moving.snapshot()->current_type(actor).value() == 9
            && !moving.snapshot()->current_type(fleet) && transition_batches == 2,
            "landing preserves current transport while retiring empty fleet in one completed batch");
        const auto returned = moving.snapshot()->actor_records();
        const auto at = std::find_if(returned.begin(), returned.end(), [&](const auto& row) { return row.id == actor; });
        expect(at != returned.end() && at->container == 1 && moving.cached_state_hash().get() == moving.state_sha256(),
            "landing parent and independent full-state hash agree");
    }
}
void representation_continuation_contracts(gc::GalacticSetup setup,
    std::shared_ptr<const cp::StartingForceInputs> inputs, const cp::TacticalReturnState& pending,
    const eawr::checkpoint::GameLoadOptions& load, const eawr::sim::PartitionExecutor& executor,
    std::vector<std::string>& reference) {
    setup.debug_commands = true; setup.rules.maximum_fleet_movement_distance = f(30);
    auto created = cp::GameSession::create(setup);
    expect(bool(created), "representation continuation owner constructs"); if (!created) return;
    auto game = std::move(created).value();
    expect(bool(cp::bind_stock_hero_returns(game, inputs))
        && bool(game.restore_return_state(cp::encode_tactical_return_state(pending))), "continuation owner binds due constructor");
    for (unsigned i = 0; i < 4; ++i) if (!game.step(executor)) { expect(false, "continuation births advance"); return; }
    const auto fleet = game.galaxy().snapshot()->fleets().front().id;
    const auto actor = game.galaxy().snapshot()->fleets().front().members.front().id;
    expect(bool(game.submit({{game.completed_tick(), {1}, 1}, gc::MoveFleet{fleet, {2, 1}, {}}}))
        && bool(game.step(executor)), "representation transit command advances");
    const auto fleets = game.galaxy().snapshot()->fleets();
    const auto moving = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == fleet; });
    expect(moving != fleets.end() && moving->motion.transit.has_value(), "save boundary is during actual transit");
    if (moving == fleets.end() || !moving->motion.transit) return;
    const auto identities = eawr::checkpoint::game_identities(setup);
    const auto restore = [&] {
        auto saved = eawr::checkpoint::save_game_checkpoint(game, identities);
        return saved ? eawr::checkpoint::load_game_checkpoint(saved.value(), identities, load, executor)
            : eawr::core::Result<eawr::checkpoint::GameRestore>::failure(saved.error());
    };
    auto transit = restore();
    if (!transit) std::cerr << transit.error().message << '\n';
    expect(transit && transit.value().session.state_sha256() == game.state_sha256()
        && transit.value().session.galaxy().snapshot()->current_type(actor).value() == 7,
        "mid-transit checkpoint preserves literal actor facts and full state");
    if (!transit) return;
    for (auto* owner : {&game, &transit.value().session})
        expect(bool(owner->submit({{owner->completed_tick(), {1}, 2}, gc::DebugSetPlanetOwner{3, 1}})),
            "owner change submits to both continuation owners");
    auto changed = game.step(executor), changed_restore = transit.value().session.step(executor);
    expect(changed && changed_restore && changed.value().state_hash.get() == changed_restore.value().state_hash.get(),
        "owner change after transit restore equals uninterrupted play");
    const auto actors = game.galaxy().snapshot()->actor_records();
    const auto planet = std::find_if(actors.begin(), actors.end(), [](const auto& row) { return row.id == 3; });
    expect(planet != actors.end() && planet->owner == 1, "owner commit updates saved typed planet record");
    auto owned = restore();
    if (!owned) std::cerr << owned.error().message << '\n';
    expect(owned && owned.value().session.state_sha256() == game.state_sha256(), "post-owner-change checkpoint restores exactly");
    if (!owned) return;
    std::vector<std::string> hashes;
    for (unsigned i = 0; i < 32; ++i) {
        auto a = game.step(executor), b = transit.value().session.step(executor), c = owned.value().session.step(executor);
        expect(a && b && c && a.value().state_hash.get() == b.value().state_hash.get()
            && a.value().state_hash.get() == c.value().state_hash.get(),
            "transit and ownership reloads continue with uninterrupted full hashes");
        if (!a || !b || !c) return;
        hashes.push_back(a.value().state_hash.get());
    }
    const auto records = game.galaxy().snapshot()->actor_records();
    const auto arrived = std::find_if(records.begin(), records.end(), [&](const auto& row) { return row.id == actor; });
    expect(arrived != records.end() && arrived->planet == 1 && arrived->current_type == 7,
        "continued arrival commits current identity at destination after both reloads");
    if (reference.empty()) reference = hashes;
    else expect(hashes == reference, "transit and owner-change continuation has 1/2/4/8 full-hash parity");
}
std::string actor_state_contracts(gc::GalacticSetup setup, const eawr::sim::PartitionExecutor& executor) {
    if (!setup.census) return {};
    auto& records = setup.census->objects;
    const auto chosen = std::find_if(records.begin(), records.end(), [](const auto& row) {
        return row.original_type == 7 && row.combat_role == gc::ActorCombatRole::ordinary;
    });
    expect(chosen != records.end(), "stealth contract has an actual ordinary craft"); if (chosen == records.end()) return {};
    const auto id = chosen->id; chosen->abilities.present = true;
    auto created = gc::GalacticSession::create(setup); expect(bool(created), "live actor owner constructs"); if (!created) return {};
    auto world = std::move(created).value(); world.set_hash_verification(true);
    unsigned notifications{};
    world.set_native_actor_changes_handler([&](const auto&, auto changes) {
        notifications += static_cast<unsigned>(changes.size()); return eawr::core::Result<void>::success();
    });
    const auto before = world.snapshot(); const auto original_hash = world.state_sha256();
    expect(!world.apply_actor_stealth(0, 0, f(1), true) && !world.apply_actor_stealth(id, id + 1, f(1), true)
        && !world.apply_actor_stealth(99999, 0, f(1), true), "only existing self/null ability targets admit");
    expect(world.state_sha256() == original_hash && !notifications, "unqualified effects are atomic");
    expect(bool(world.apply_actor_stealth(id, 0, f(2), true)) && world.snapshot()->actor_combat_skip(id).value()
        && !before->actor_combat_skip(id).value(), "effect activates one actor and preserves old immutable view");
    const auto applied = world.snapshot()->actor_records();
    const auto actor = std::find_if(applied.begin(), applied.end(), [&](const auto& row) { return row.id == id; });
    expect(actor->abilities.evade_chance == f(1) && actor->abilities.self_target, "apply clamps chance and records self target");
    expect(bool(world.apply_actor_stealth(id, id, f(2), true)) && notifications == 1, "identical scalar write sends no notification");
    expect(bool(world.apply_actor_stealth(id, id, f(-1), true)) && bool(world.apply_actor_stealth(id, 0, f(1), false))
        && !world.snapshot()->actor_combat_skip(id).value(), "one removal clears successive scalar writes");
    expect(bool(world.apply_actor_stealth(id, 0, f(1), true)) && bool(world.set_actor_garrison(id, true))
        && world.snapshot()->actor_garrison(id).value(), "separate retained garrison owner commits");
    expect(!world.snapshot()->actor_garrison(99999) && !world.snapshot()->actor_combat_skip(99999), "missing facts refuse");
    const auto stable = world.snapshot(); const auto count = notifications;
    for (unsigned tick = 0; tick < 3; ++tick) {
        auto step = world.step(executor);
        expect(bool(step), "ordinary tick retains live ability state");
        if (step) expect(tick == 0 ? step.value().work.control_stealth_fleets == setup.fleets.size()
            : step.value().work.control_stealth_fleets == 0,
            "control rebuilds live skip inputs once and does no recursive stealth work on idle ticks");
    }
    expect(world.snapshot()->actor_records().data() == stable->actor_records().data() && notifications == count
        && world.snapshot()->actor_combat_skip(id).value(), "idle ticks reuse publication without automatic clear or actor notifications");
    auto game = cp::GameSession::create(setup); expect(bool(game), "actor checkpoint owner constructs"); if (!game) return {};
    expect(bool(game.value().restore_actor_representations(census_section(world))), "archive restores live fields without construction reset");
    const auto identity = eawr::checkpoint::game_identities(setup);
    auto bytes = eawr::checkpoint::save_game_checkpoint(game.value(), identity);
    expect(bool(bytes), "active stealth/garrison archive writes"); if (!bytes) return {};
    auto loaded = eawr::checkpoint::load_game_checkpoint(bytes.value(), identity, {}, executor);
    expect(bool(loaded), "active stealth/garrison archive loads"); if (!loaded) return {};
    expect(loaded.value().session.galaxy().snapshot()->actor_combat_skip(id).value()
        && loaded.value().session.galaxy().snapshot()->actor_garrison(id).value(), "saved authoritative facts survive fresh owner restore");
    auto live = game.value().step(executor), resumed = loaded.value().session.step(executor);
    expect(live && resumed && live.value().state_hash.get() == resumed.value().state_hash.get(), "load then continue equals uninterrupted owner");
    expect(bool(world.apply_actor_stealth(id, 0, {}, false)) && bool(world.set_actor_garrison(id, false)), "explicit removal and clear commit");
    const auto cleared = world.snapshot()->actor_records();
    const auto removed = std::find_if(cleared.begin(), cleared.end(), [&](const auto& row) { return row.id == id; });
    expect(!removed->abilities.self_target && removed->abilities.evade_chance == Fixed{}
        && !world.snapshot()->actor_combat_skip(id).value() && !world.snapshot()->actor_garrison(id).value(), "removal clears all live scalar fields");
    return live ? live.value().state_hash.get() : std::string{};
}
std::string nested_split_stealth(gc::GalacticSetup setup, const eawr::sim::PartitionExecutor& executor) {
    // The stock constructor can represent a squadron with FLEET behaviour;
    // its actual children remain in MovementContents, outside the root fleet table.
    if (!setup.census || !setup.production || !setup.production->nested_creation) return {};
    for (auto& type : setup.production->nested_creation->types)
        if (type.type == 17) type.combat_role = gc::ActorCombatRole::fleet;
    for (auto& actor : setup.census->objects) {
        if (actor.current_type == 17) actor.combat_role = gc::ActorCombatRole::fleet;
        actor.abilities.present = true;
    }
    const auto nested = std::find_if(setup.census->objects.begin(), setup.census->objects.end(),
        [](const auto& actor) { return actor.current_type == 17; });
    expect(nested != setup.census->objects.end(), "constructed squadron supplies nested fleet actor");
    if (nested == setup.census->objects.end() || setup.fleets.size() != 2) return {};
    const auto nested_id = nested->id;
    const auto parent = std::find_if(setup.fleets.begin(), setup.fleets.end(), [&](const auto& fleet) {
        return std::any_of(fleet.members.begin(), fleet.members.end(), [&](const auto& member) { return member.id == nested_id; });
    });
    if (parent == setup.fleets.end()) return {};
    const auto root = parent->id;
    const auto other = setup.fleets.front().id == root ? setup.fleets.back().id : setup.fleets.front().id;
    const auto contents = std::find_if(setup.movement->contents.begin(), setup.movement->contents.end(),
        [&](const auto& row) { return row.container == nested_id; });
    expect(contents != setup.movement->contents.end() && !contents->children.empty(), "nested fleet has real constructor children");
    if (contents == setup.movement->contents.end() || contents->children.empty()) return {};
    const auto child = contents->children.front().id;
    setup.production->fleet_blockers.emplace();
    for (const auto& fleet : setup.fleets)
        setup.production->fleet_blockers->push_back({fleet.id, false, false, true});
    auto made = gc::GalacticSession::create(setup); expect(bool(made), "split stealth world creates"); if (!made) return {};
    auto world = std::move(made).value(); world.set_hash_verification(true);
    for (const auto& actor : setup.census->objects)
        if (actor.combat_role == gc::ActorCombatRole::ordinary)
            expect(bool(world.apply_actor_stealth(actor.id, 0, f(1), true)), "ordinary constructed child activates");
    const auto admit = [&](const gc::CommandPayload& payload) {
        auto result = world.admit_native_fleet(1, payload);
        expect(result && result.value().admitted, "real membership operation admits");
        return result && result.value().admitted;
    };
    if (!admit(gc::MergeFleets{other, root})) return {};
    expect(world.snapshot()->fleet_combat_skip(root).value(), "merged nested fleet reads all live children");
    expect(bool(world.apply_actor_stealth(child, 0, {}, false)) && !world.snapshot()->fleet_combat_skip(root).value(),
        "nested child removal reaches actual root fleet");
    if (world.snapshot()->production()->fleet_blockers) {
        const auto& blockers = *world.snapshot()->production()->fleet_blockers;
        const auto blocker = std::find_if(blockers.begin(), blockers.end(), [&](const auto& row) { return row.fleet == root; });
        expect(blocker != blockers.end() && !blocker->skips_space_combat, "production root blocker follows nested child removal");
    }
    expect(bool(world.apply_actor_stealth(child, 0, f(1), true)), "nested child reapplies");
    if (!admit(gc::RemoveFleetMembers{root, {nested_id}})) return {};
    const auto split = world.next_entity_id();
    if (!admit(gc::CreateFleet{1, {nested_id}, 0})) return {};
    expect(world.snapshot()->fleet_combat_skip(root).value() && world.snapshot()->fleet_combat_skip(split).value(),
        "split root memberships preserve actual nested breakdown");
    expect(bool(world.apply_actor_stealth(child, 0, {}, false)) && world.snapshot()->fleet_combat_skip(root).value()
        && !world.snapshot()->fleet_combat_skip(split).value(), "child write affects only its new split root");
    if (!admit(gc::MergeFleets{split, root})) return {};
    expect(!world.snapshot()->fleet_combat_skip(root).value() && !world.snapshot()->fleet_combat_skip(split),
        "merge retires source and reflects retained mixed children");
    expect(bool(world.apply_actor_stealth(child, 0, f(1), true)) && world.snapshot()->fleet_combat_skip(root).value(),
        "post-merge child writer follows actual parent chain");
    auto first = world.step(executor), idle = world.step(executor);
    expect(first && idle && first.value().work.control_stealth_fleets == 1 && idle.value().work.control_stealth_fleets == 0,
        "split/merge invalidates control inputs once and idle reuses them");
    return idle ? idle.value().state_hash.get() : std::string{};
}
void nested_live_stealth() {
    std::vector<gc::CensusObject> records(4);
    for (std::size_t i = 0; i < records.size(); ++i) {
        records[i].id = i + 1; records[i].combat_role = gc::ActorCombatRole::ordinary;
        records[i].abilities = {true, true, true, f(1)}; records[i].garrison = false;
    }
    records[0].combat_role = gc::ActorCombatRole::transport; records[1].combat_role = gc::ActorCombatRole::squadron;
    gc::MovementSetup movement; movement.contents = {{1, {{2, 0}}}, {2, {{3, 0}, {4, 0}}}};
    gc::Fleet fleet{10, 1, 1, {{1, 0}}};
    expect(gc::fleet_combat_skip(fleet, records, &movement).value(), "recursive all-stealth transport/squadron skips");
    records[1].combat_role = gc::ActorCombatRole::fleet;
    expect(gc::fleet_combat_skip(fleet, records, &movement).value(),
        "nested fleet constructor uses its explicit payload breakdown outside the outer fleet table");
    records[3].abilities.combat_skip = false;
    expect(!gc::fleet_combat_skip(fleet, records, &movement).value(), "one ordinary payload defeats container's own skip");
    records[3].delete_pending = true;
    expect(gc::fleet_combat_skip(fleet, records, &movement).value(), "deletion pending payload skips before ability dispatch");
    movement.contents[0].children.clear();
    expect(!gc::fleet_combat_skip(fleet, records, &movement).value(), "empty transport never skips with its own flag");
    fleet.members.clear(); expect(!gc::fleet_combat_skip(fleet, records, &movement).value(), "empty fleet never skips");
    fleet.members = {{99, 0}}; expect(!gc::fleet_combat_skip(fleet, records, &movement), "unqualified member refuses");
    fleet.members = {{1, 0}}; movement.contents[0].children = {{1, 0}};
    expect(!gc::fleet_combat_skip(fleet, records, &movement), "cyclic payload refuses boundedly");
}
void root_checkpoint_and_parity(bool positive_planet = false, bool due_heroes = false) {
    Fixture fixture;
    fixture.source(10).population_value = f(3); fixture.source(11).population_value = f(2);
    for (const auto id : {10U, 11U}) {
        fixture.source(id).build_cost_credits = f(1); fixture.source(id).build_time_seconds = f(1);
    }
    eawr::data::galactic::GalacticData data;
    for (const auto& name : {"REBEL", "EMPIRE", "NEUTRAL"}) {
        eawr::data::galactic::Faction faction; faction.name = name;
        faction.is_neutral = std::string_view(name) == "NEUTRAL"; data.factions.push_back(faction);
    }
    eawr::data::galactic::Planet planet; planet.name = "A"; planet.behaviors = {"PLANET"};
    planet.galactic_position = eawr::data::galactic::Position{f(0), f(0), f(0)}; planet.planet_credit_value = f(10);
    planet.population_value = positive_planet ? 4 : 0;
    data.planets.push_back(planet);
    data.constants = {{"MaximumFleetMovementDistance", "10", f(10)}, {"Fiscal_Cycle_Time_In_Secs", "45", f(45)},
        {"Credit_Cap_Per_Planet", "100", f(100)}};
    eawr::data::galactic::Campaign campaign; campaign.name = "Fixture"; campaign.planets = {"A"}; campaign.locations = {"A"};
    campaign.starting_forces = {{"REBEL", "A", "GroundStructure"}, {"REBEL", "A", "Starbase"},
        {"REBEL", "A", "Company"}, {"REBEL", "A", "Squadron"}, {"REBEL", "A", "Ship"}};
    if (due_heroes) {
        data.planets[0].galactic_position = eawr::data::galactic::Position{f(20), {}, {}};
        auto near = data.planets[0]; near.name = "B"; near.galactic_position = eawr::data::galactic::Position{f(2), {}, {}};
        auto lost = data.planets[0]; lost.name = "C"; lost.galactic_position = eawr::data::galactic::Position{};
        data.planets.push_back(near); data.planets.push_back(lost);
        campaign.planets = campaign.locations = {"A", "B", "C"};
        campaign.starting_forces = {{"REBEL", "A", "GroundStructure"}, {"REBEL", "B", "GroundStructure"},
            {"EMPIRE", "C", "GroundStructure"}};
    }
    campaign.starting_credits = {{"REBEL", 10}};
    data.campaigns.push_back(campaign);
    cp::CampaignSetupOptions options; options.seed = 42; options.types = fixture.inputs.bindings;
    options.players = {{{1, 1, 0}, "REBEL", true}, {{2, 2, 0}, "EMPIRE", true}, {{3, 3, 0}, "NEUTRAL", true}};
    options.seats = {{gc::SeatId{1}, 1, gc::SeatKind::human, 0, true, 1}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    options.fiscal_players = std::vector<gc::FiscalPlayer>{{1, true, f(1)}, {2, true, f(1)}, {3, false, f(1)}};
    const auto planned = cp::plan_campaign_setup(data, fixture.inputs.types, "Fixture", options);
    expect(bool(planned), "nested starting fixture plans"); if (!planned) return;
    auto& context = fixture.context;
    context.control.change_seconds = f(7); context.control.planets[0].type = 100;
    context.control.planets[0].retains_residual = true;
    context.movement.mode_modifier = f(1);
    context.movement.planets = {{1, f(0), false}}; // explicit synthetic geometry
    for (gc::PlayerId id : {1U, 2U, 3U}) {
        context.control.players.push_back({id, id == 3, id != 3, id});
        context.movement.players.push_back({id, f(1), f(0), false});
        context.movement.access.push_back({id, 1, true, false}); // explicit fixture admission
        context.census.players.push_back({id, 100, false, {}});
        gc::ProductionPlayer producer; producer.player = id; producer.human = id == 1;
        producer.pay_as_you_go = id != 1; producer.uses_population_cap = id == 1;
        context.production.players.push_back(producer);
    }
    context.production.maximum_human_queue = 5;
    if (due_heroes) context.production.planets.reserve(3);
    auto& producer = context.production.planets[0]; producer.enabled = true; producer.surface_accessible = true;
    producer.maximum_space_level = 5; producer.maximum_ground_forces = 2;
    for (const auto& binding : fixture.inputs.bindings) {
        const auto& source = fixture.source(binding.id);
        gc::MovementType movement; movement.type = binding.id; movement.hyperspace_speed = f(2);
        movement.hyperspace_eligible = binding.id == 7 || binding.id == 9;
        movement.transport = binding.id == 9; movement.squadron = binding.id == 11; movement.fleet_container = binding.id == 16;
        context.movement.types.push_back(movement);
        context.control.types.push_back({binding.id, static_cast<std::uint8_t>(binding.id == 7 ? 1 : 0), false, binding.id == 8});
        context.control.ground_capability_types.push_back({binding.id, binding.id == 8 ? f(1) : f(0), binding.id == 9, false});
        auto original = cp::prepare_census_type(source, binding.id, fixture.inputs.bindings);
        expect(bool(original), "fixture binds original census types"); if (!original) return;
        context.census.types.push_back(original.value());
    }
    context.census.types.push_back({100, positive_planet ? f(4) : f(0), 0});
    context.location_originals.push_back({1, 100, 0, 1, 0, false, false, 0, 0, gc::ActorCombatRole::unavailable, {}, {}});
    if (due_heroes) for (const gc::PlanetId id : {2ULL, 3ULL}) {
        auto control = context.control.planets[0]; control.planet = id; context.control.planets.push_back(control);
        auto production = context.production.planets[0]; production.planet = id; context.production.planets.push_back(production);
        context.movement.planets.push_back({id, f(0), false});
        for (gc::PlayerId owner : {1U, 2U, 3U}) context.movement.access.push_back({owner, id, true, false});
        context.location_originals.push_back({id, 100, 0, id, 0, false, false, 0, 0, gc::ActorCombatRole::unavailable, {}, {}});
    }
    if (due_heroes) std::sort(context.movement.access.begin(), context.movement.access.end(), [](const auto& a, const auto& b) {
        return std::pair{a.player, a.planet} < std::pair{b.player, b.planet};
    });
    for (const auto id : {10U, 11U}) {
        gc::ProductionOffer offer; offer.type = id; offer.affiliated = offer.direct_build_allowed = true;
        offer.domain = id == 10 ? gc::ProductionDomain::ground : gc::ProductionDomain::space;
        offer.population = *fixture.source(id).population_value;
        offer.price = *fixture.source(id).build_cost_credits; offer.seconds = *fixture.source(id).build_time_seconds;
        producer.offers.push_back(offer);
    }
    cp::CampaignSetupServices services;
    services.stock_starting_forces = std::make_shared<const cp::StartingForceInputs>(fixture.inputs);
    services.placement_context = [context](const auto&) { return eawr::core::Result<cp::CampaignPlacementContext>::success(context); };
    services.location = [](const auto&) { return eawr::core::Result<void>::success(); };
    services.home = [](const auto&) { return eawr::core::Result<void>::success(); };
    services.credit_cap = [](const auto&, const auto&) { return eawr::core::Result<Fixed>::success(f(100)); };
    services.assign_tech = [](const auto& tech, const auto&) { return eawr::core::Result<std::int64_t>::success(tech.requested_current); };
    std::vector<std::string> reference, continuation_reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto root = cp::CampaignRoot::create(planned.value(), services);
        if (!root) std::cerr << root.error().message << '\n';
        expect(bool(root), "stock allocator creates full persistent CampaignRoot"); if (!root) return;
        const auto& setup = root.value().game()->galaxy().setup();
        if (due_heroes) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto& game = *root.value().game();
            auto inputs = services.stock_starting_forces;
            cp::TacticalReturnState pending;
            pending.heroes = {{901, 7, 0, 1, 2, 3, 1}, {902, 10, 12, 1, 2, 3, 1},
                {903, 11, 0, 1, 2, 3, 1}, {904, 10, 12, 1, 2, 3, 1}, {905, 10, 12, 1, 2, 3, 1}};
            expect(bool(cp::bind_stock_hero_returns(game, inputs)), "stock due owner binds to the live campaign");
            expect(bool(game.restore_return_state(cp::encode_tactical_return_state(pending))), "fixture schedules due records");
            const auto identities = eawr::checkpoint::game_identities(setup);
            eawr::checkpoint::GameLoadOptions load;
            // Fixture scheduling is replayed by its owner; retail schedules come from recorded defeat.
            load.configure_session = [inputs, pending](cp::GameSession& fresh) {
                if (auto bound = cp::bind_stock_hero_returns(fresh, inputs); !bound) return bound;
                return fresh.restore_return_state(cp::encode_tactical_return_state(pending));
            };
            representation_continuation_contracts(setup, inputs, pending, load, executor, continuation_reference);
            const auto saved = eawr::checkpoint::save_game_checkpoint(game, identities);
            expect(bool(saved), "pending due heroes save before their deadline"); if (!saved) return;
            auto loaded = eawr::checkpoint::load_game_checkpoint(saved.value(), identities, load, executor);
            if (!loaded) std::cerr << loaded.error().message << '\n';
            expect(bool(loaded), "fresh checkpoint owner rebinds stock due placement"); if (!loaded) return;
            std::vector<std::string> hashes;
            for (unsigned tick = 0; tick < 4; ++tick) {
                game.scramble_storage_for_testing(); loaded.value().session.scramble_storage_for_testing();
                auto stepped = game.step(executor); auto restored = loaded.value().session.step(executor);
                if (!stepped) std::cerr << stepped.error().message << '\n';
                if (!restored) std::cerr << restored.error().message << '\n';
                expect(stepped && restored, "live due production commits succeed"); if (!stepped || !restored) return;
                expect(stepped.value().state_hash.get() == restored.value().state_hash.get(), "pending due checkpoint continuation equals live control");
                hashes.push_back(stepped.value().state_hash.get());
            }
            const auto snapshot = game.galaxy().snapshot();
            expect(game.return_state().heroes.empty() && snapshot->fleets().size() == 2,
                "ordinary, company and squadron heroes consume only successful primary placement");
            expect(snapshot->fleets().front().orbit == 2 && snapshot->fleets().front().members.front().type == 7
                && snapshot->fleets().front().motion.position == setup.planets[1].position,
                "lost-planet hero lands at nearest owned planet center, rather than first-owned planet");
            expect(snapshot->control()->planets[1].landed.size() == 2
                && snapshot->movement()->contents.size() == 3
                && snapshot->movement()->contents.front().children.size() == 2
                && snapshot->fleets().back().members.front().type == 11
                && snapshot->movement()->contents.back().children.size() == 2,
                "nearest backing site receives two companies; third cancels against newly committed exact-original census");
            const auto frontier = game.galaxy().next_entity_id();
            auto after = eawr::checkpoint::save_game_checkpoint(game, identities);
            expect(bool(after), "placed hero trees save after success"); if (!after) return;
            auto resumed = eawr::checkpoint::load_game_checkpoint(after.value(), identities, load, executor);
            if (!resumed) std::cerr << resumed.error().message << '\n';
            expect(resumed && resumed.value().session.state_sha256() == game.state_sha256(), "placed trees restore with primary record erased");
            if (!resumed) return;
            auto next = game.step(executor); auto restored_next = resumed.value().session.step(executor);
            expect(next && restored_next && next.value().state_hash.get() == restored_next.value().state_hash.get()
                && game.galaxy().next_entity_id() == frontier, "restored success never creates a second hero");
            // Direct creation may commit an additional result while reporting a null primary.
            auto extra_world = gc::GalacticSession::create(setup);
            expect(bool(extra_world), "secondary-only placement gets a separate persistent owner"); if (!extra_world) return;
            auto chain_inputs = std::make_shared<cp::StartingForceInputs>(*inputs);
            chain_inputs->types.types[6].additional_creation_type = "Ship"; // authored Starbase row
            const gc::GalacticSession::NativeProductionRequest extra_request{1, 2, 13, {}};
            auto secondary = extra_world.value().place_due_hero(extra_request, cp::stock_native_force_builder(chain_inputs, extra_request), executor);
            if (!secondary) std::cerr << secondary.error().message << '\n';
            expect(secondary && secondary.value().primary == 0 && secondary.value().objects.size() == 2
                && extra_world.value().snapshot()->fleets().size() == 1, "secondary tree commits without claiming primary success");
            if (reference.empty()) reference = hashes;
            else expect(reference == hashes, "live due placement hashes equal on 1/2/4/8 workers");
            continue;
        }
        expect(setup.planets[0].owner == 1 && setup.control->state[0].current_player == 1 && setup.fleets.size() == 2
            && setup.control->planets[0].landed.size() == 1 && setup.production->planets[0].objects.size() == 1,
            "initial no-trade political ownership follows all nested starting forces");
        expect(root.value().game()->galaxy().next_entity_id() == 12, "all nested allocations share the root frontier");
        if (workers == 1) {
            auto malformed = setup; malformed.production->nested_creation->types[3].transport = 0;
            expect(!gc::validate_setup(malformed), "unbound completion transport refuses before campaign creation");
            malformed = setup; malformed.production->nested_creation->types[4].members = {0};
            expect(!gc::validate_setup(malformed), "unbound completion craft refuses before campaign creation");
            malformed = setup; malformed.production->nested_creation->relations.pop_back();
            expect(!gc::validate_setup(malformed), "incomplete completion enemy relations refuse before campaign creation");
        }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        if (!positive_planet) {
            representation_contracts(setup, services.stock_starting_forces, executor);
            const auto actor_hash = actor_state_contracts(setup, executor);
            static std::string first_actor_hash;
            if (first_actor_hash.empty()) first_actor_hash = actor_hash;
            else expect(first_actor_hash == actor_hash, "live actor hashes equal on 1/2/4/8 workers");
            const auto split_hash = nested_split_stealth(setup, executor);
            static std::string first_split_hash;
            if (first_split_hash.empty()) first_split_hash = split_hash;
            else expect(first_split_hash == split_hash, "nested split/merge and cached control equal on 1/2/4/8 workers");
        }
        const auto identities = eawr::checkpoint::game_identities(setup);
        auto saved = eawr::checkpoint::save_game_checkpoint(*root.value().game(), identities);
        expect(bool(saved), "nested root saves at tick zero"); if (!saved) return;
        auto loaded = eawr::checkpoint::load_game_checkpoint(saved.value(), identities, {}, executor);
        expect(loaded && loaded.value().session.state_sha256() == root.value().game()->state_sha256(),
            "nested root has canonical tick-zero checkpoint equality");
        if (!loaded) return;
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 12; ++tick) {
            auto stepped = root.value().game()->step(executor); auto restored = loaded.value().session.step(executor);
            expect(stepped && restored && stepped.value().state_hash.get() == restored.value().state_hash.get(),
                "nested root checkpoint continuation matches");
            if (!stepped || !restored) return;
            hashes.push_back(stepped.value().state_hash.get());
        }
        auto& game = *root.value().game();
        const auto buy_tick = game.completed_tick();
        expect(bool(game.submit({{buy_tick, {1}, 1}, gc::ProductionCommand{1, 10, gc::ProductionAction::buy, gc::ProductionDomain::ground, 0, {}}})), "company buy submitted");
        expect(bool(game.submit({{buy_tick, {1}, 2}, gc::ProductionCommand{1, 11, gc::ProductionAction::buy, gc::ProductionDomain::space, 0, {}}})), "squadron buy submitted");
        for (int tick = 0; tick < 5; ++tick) expect(bool(game.step(executor)), "nested production queue advances");
        const auto midway = eawr::checkpoint::save_game_checkpoint(game, identities);
        expect(bool(midway), "mid-queue nested production saves"); if (!midway) return;
        auto restored_queue = eawr::checkpoint::load_game_checkpoint(midway.value(), identities, {}, executor);
        if (!restored_queue) std::cerr << restored_queue.error().message << '\n';
        expect(bool(restored_queue), "mid-queue nested production reconstructs"); if (!restored_queue) return;
        expect(game.galaxy().snapshot()->production()->planets[0].queues[0].size() == 1
            && game.galaxy().snapshot()->production()->planets[0].queues[1].size() == 1, "both nested purchases queued");
        for (int tick = 0; tick < 28; ++tick) {
            auto stepped = game.step(executor); auto continued = restored_queue.value().session.step(executor);
            if (!stepped) std::cerr << stepped.error().message << '\n';
            expect(stepped && continued && stepped.value().state_hash.get() == continued.value().state_hash.get(),
                "nested completion matches after mid-queue checkpoint");
            if (!stepped || !continued) return;
            expect(game.galaxy().state_sha256() == game.galaxy().cached_state_hash().get(),
                "nested completion cached hashes equal a fresh encoding of all live sections");
            expect(game.state_sha256() == stepped.value().state_hash.get(),
                "nested completion root hash includes the retained mode timeline");
            hashes.push_back(stepped.value().state_hash.get());
        }
        const auto snapshot = game.galaxy().snapshot();
        expect(snapshot->control()->planets[0].landed.back() == gc::SurfaceForce{14, 9, 1}
            && snapshot->fleets().back().id == 17 && snapshot->fleets().back().members[0] == gc::FleetMember{18, 11}
            && game.galaxy().next_entity_id() == 21, "purchases share startup nested ID ordering and placement");
        expect(snapshot->movement()->contents.back().children == std::vector<gc::FleetMember>{{19, 7}, {20, 7}},
            "purchased squadron retains ordered repeated craft");
        expect(snapshot->current_type(14) && snapshot->current_type(14).value() == 9
            && snapshot->current_type(18) && snapshot->current_type(18).value() == 17,
            "paid completion publishes current transport and squadron identities");
        expect(restored_queue.value().session.galaxy().snapshot()->actor_records().size() == snapshot->actor_records().size()
            && restored_queue.value().session.galaxy().snapshot()->current_type(18).value() == 17,
            "restored completion publishes the same current representations");
        expect(snapshot->production()->planets[0].queues[0].empty() && snapshot->production()->planets[0].queues[1].empty(),
            "nested purchases finish once");
        const auto& counts = snapshot->production()->players[0].counts;
        for (const auto id : {10U, 11U}) {
            const auto at = std::find_if(counts.begin(), counts.end(), [&](const auto& row) { return row.type == id; });
            expect(at != counts.end() && at->historical == 1 && at->owned >= 2, "nested completion updates owned and historical census");
        }
        expect(snapshot->population()[0].used == f(positive_planet ? 8 : 10) && snapshot->population()[0].reserved == f(0),
            "nested population moves reservation into owned original usage");
        expect(bool(game.submit({{game.completed_tick(), {1}, 3}, gc::ProductionCommand{1, 10, gc::ProductionAction::buy, gc::ProductionDomain::ground, 0, {}}})),
            "company purchase with full landed capacity submitted");
        for (int tick = 0; tick < 32; ++tick) {
            auto stepped = game.step(executor); expect(bool(stepped), "full-capacity company production advances");
            if (!stepped) return;
            hashes.push_back(stepped.value().state_hash.get());
        }
        const auto full = game.galaxy().snapshot();
        expect(full->control()->planets[0].landed.size() == 2 && full->fleets().back().id == 25
            && full->fleets().back().members[0] == gc::FleetMember{22, 9}
            && full->movement()->contents.back().children == std::vector<gc::FleetMember>{{23, 8}, {24, 8}}
            && game.galaxy().next_entity_id() == 26, "full ground capacity purchases the same orbiting transport structure as startup");
        expect(full->population()[0].used == f(positive_planet ? 11 : 13), "orbiting purchased company retains its logical population type");
        expect(full->player_economy()->players[0].credits == f(7), "three priced nested purchases debit the funded campaign once each");
        const auto finished_save = eawr::checkpoint::save_game_checkpoint(game, identities);
        auto finished_load = finished_save ? eawr::checkpoint::load_game_checkpoint(finished_save.value(), identities, {}, executor)
            : eawr::core::Result<eawr::checkpoint::GameRestore>::failure(finished_save.error());
        expect(finished_load && finished_load.value().session.state_sha256() == game.state_sha256(),
            "post-completion reconstruction preserves live journal without rewriting tick-zero setup");
        auto invasion_replay = game.record(); invasion_replay.setup.debug_commands = true;
        auto invasion_world = gc::GalacticSession::from_replay(invasion_replay);
        expect(bool(invasion_world), "purchased-company invasion fixture reconstructs"); if (!invasion_world) return;
        auto& invasion = invasion_world.value();
        for (std::uint64_t tick = 0; tick < game.completed_tick(); ++tick)
            if (!invasion.step(executor)) { expect(false, "invasion fixture advances through purchases"); return; }
        expect(std::none_of(invasion.setup().movement->contents.begin(), invasion.setup().movement->contents.end(),
            [](const auto& row) { return row.container == 22; }), "purchased company is absent from immutable tick-zero contents");
        // Stage an empty hostile surface through the trusted battle-return and debug boundaries.
        gc::ControlBattleReturn cleared; cleared.planet = 1; cleared.invader = 2; cleared.defender = 1;
        cleared.winner = 2; cleared.land = true;
        expect(bool(invasion.apply_control_battle(cleared)), "enemy land victory clears the prior surface presence");
        const auto invasion_tick = invasion.completed_tick();
        expect(bool(invasion.submit({{invasion_tick, {1}, 4}, gc::DebugSetPlanetOwner{1, 2}}))
            && bool(invasion.submit({{invasion_tick, {1}, 5}, gc::LandFleet{25, 1}})), "purchased company hostile landing submitted");
        const auto landed = invasion.step(executor); expect(bool(landed), "purchased company hostile landing advances");
        if (!landed) return;
        expect(std::any_of(landed.value().snapshot->events().begin(), landed.value().snapshot->events().end(), [](const auto& event) {
            return event.kind == gc::GalacticEventKind::command_executed && event.sequence == 5 && event.subject == 25;
        }) && landed.value().snapshot->control()->planets[0].landed == std::vector<gc::SurfaceForce>{{22, 9, 1}},
            "purchased company qualifies from live payload contents and lands on an empty hostile surface");
        expect(invasion.state_sha256() == landed.value().state_hash.get(), "purchased landing cached hash equals fresh encoding");
        hashes.push_back(landed.value().state_hash.get());
        auto casualty_world = gc::GalacticSession::from_replay(game.record());
        expect(bool(casualty_world), "mutable return-boundary fixture reconstructs recorded purchases"); if (!casualty_world) return;
        auto& world = casualty_world.value();
        for (std::uint64_t tick = 0; tick < game.completed_tick(); ++tick)
            if (!world.step(executor)) { expect(false, "return-boundary fixture advances to purchased company"); return; }
        expect(world.state_sha256() == game.galaxy().state_sha256(), "return-boundary fixture equals live campaign before casualties");
        const std::vector<eawr::sim::EntityId> casualties{22, 23, 24, 25};
        expect(bool(world.apply_battle_losses(casualties)), "purchased orbital company casualties commit through owning return boundary");
        const auto loss_tick = world.step(executor); expect(bool(loss_tick), "post-casualty population refresh advances");
        if (!loss_tick) return;
        expect(world.snapshot()->population()[0].used == f(positive_planet ? 8 : 10)
            && std::none_of(world.snapshot()->movement()->contents.begin(), world.snapshot()->movement()->contents.end(),
                [](const auto& row) { return row.container == 22; }), "purchased carrier and payload deaths leave no live census contribution or contents");
        expect(world.state_sha256() == loss_tick.value().state_hash.get(), "casualty journal cleanup invalidates cached census hash");
        hashes.push_back(loss_tick.value().state_hash.get());
        if (workers == 1) reference = hashes; else expect(hashes == reference, "nested starting forces and purchases have 1/2/4/8 parity");
    }
    auto conflict = services; conflict.unify_human_guard_fleets = [](auto, auto, const auto& setup) {
        return eawr::core::Result<std::vector<gc::Fleet>>::success(setup.fleets);
    };
    expect(!cp::CampaignRoot::create(planned.value(), conflict), "stock debug allocator rejects a competing guard-unification owner");
}
void initial_ability_predicates() {
    Fixture fixture;
    fixture.source(8).initial_stealth = {{{}, {}, {}}}; // specific omission defaults
    const auto prepare = [&] { return cp::prepare_initial_starting_force_inputs(fixture.inputs.types,
        fixture.inputs.bindings, fixture.inputs.relations, 16, 17); };
    auto initial = prepare();
    expect(initial && initial.value().runtime_types[1].skips_combat && !initial.value().runtime_types[0].skips_combat,
        "fresh automatic galactic stealth activates while an ordinary object does not skip");
    fixture.source(8).initial_stealth[0].initially_enabled = false;
    expect(prepare() && !prepare().value().runtime_types[1].skips_combat, "disabled initial galactic stealth does not activate");
    fixture.source(8).initial_stealth[0].initially_enabled.reset();
    fixture.source(8).initial_stealth[0].activation_style = "User_Input";
    expect(prepare() && prepare().value().runtime_types[1].skips_combat, "specific stealth validation corrects manual style to automatic");
    fixture.source(8).initial_stealth[0].activation_style = "Space_Automatic";
    expect(prepare() && prepare().value().runtime_types[1].skips_combat, "specific stealth validation corrects tactical style to galactic");
    fixture.source(8).initial_stealth[0].activation_style = "Unknown";
    expect(prepare() && prepare().value().runtime_types[1].skips_combat, "specific stealth validation corrects invalid style");
    fixture.source(8).initial_stealth[0].activation_style.clear();
    fixture.inputs.types.issues.push_back({eawr::data::galactic::IssueKind::bad_value, {}, 0, "Troop",
        "Abilities/Galactic_Stealth_Ability/Initially_Enabled", "Unknown", {}});
    expect(!prepare(), "malformed authored initial enabled value cannot become a constructor default");
}
void initial_movement_roles() {
    Fixture fixture; fixture.source(7).hyperspace_capable = true; fixture.source(9).hyperspace_capable = true;
    fixture.source(8).initial_stealth = {{{}, {}, {}}};
    auto initial = cp::prepare_initial_starting_force_inputs(fixture.inputs.types, fixture.inputs.bindings, fixture.inputs.relations, 16, 17);
    eawr::data::galactic::GalacticData data;
    eawr::data::galactic::Constant speed; speed.tag = "Object_Max_Speed_Multiplier_Galactic"; speed.number = f(1);
    data.constants.push_back(speed);
    auto movement = initial ? cp::prepare_initial_movement_inputs(initial.value(), data, {}) : eawr::core::Result<cp::MovementInputs>::failure(initial.error());
    expect(movement && movement.value().types[0].hyperspace_eligible && movement.value().types[2].transport
        && movement.value().types[2].hyperspace_eligible && movement.value().types[4].squadron
        && movement.value().types[9].fleet_container && movement.value().types[1].skips_combat,
        "fresh movement roles preserve current carrier dispatch, logical squadron containers and activated payload stealth");
}
void initial_relations() {
    eawr::data::galactic::GalacticData data;
    for (const auto* name : {"A", "B", "Neutral"}) {
        eawr::data::galactic::Faction faction; faction.name = name; data.factions.push_back(faction);
    }
    data.factions[0].allies = {"b", "Neutral"}; data.factions[0].enemies = {"B", "A", "Neutral"};
    data.factions[2].is_neutral = true;
    std::vector<cp::CampaignPlayerBinding> players(4);
    for (std::size_t i = 0; i < players.size(); ++i) {
        players[i].player.player_id = static_cast<gc::PlayerId>(i + 1);
        players[i].faction = i == 1 ? "B" : i == 2 ? "Neutral" : "A";
    }
    auto relations = cp::prepare_initial_campaign_relations(data, players);
    expect(relations && relations.value().control.size() == 16 && relations.value().placement.size() == 16,
        "fresh relations cover every directed player pair");
    if (!relations) return;
    const auto allied = [&](std::size_t a, std::size_t b) { return relations.value().control[a * 4 + b].allied; };
    const auto hostile = [&](std::size_t a, std::size_t b) { return relations.value().placement[a * 4 + b].enemy; };
    expect(allied(0, 0) && allied(0, 3) && !hostile(0, 3), "self and same faction bypass hostile authored lists");
    expect(!allied(0, 1) && hostile(0, 1) && !allied(1, 0) && !hostile(1, 0),
        "enemies override allies case insensitively, while the reverse direction remains neutral");
    expect(!allied(0, 2) && !hostile(0, 2) && !allied(2, 0) && !hostile(2, 0) && allied(2, 2),
        "either neutral faction precedes authored lists except for self");
    data.factions[0].enemies.clear(); relations = cp::prepare_initial_campaign_relations(data, players);
    expect(relations && allied(0, 1) && !hostile(0, 1), "authored ally applies without an enemy override");
    players[0].faction = "Missing";
    expect(!cp::prepare_initial_campaign_relations(data, players), "missing faction input explicitly refuses");
}
void initial_geometry() {
    eawr::assets::Model model;
    auto bounds = cp::prepare_initial_movement_planet(1, model, f(3));
    expect(bounds && bounds.value().bounds_half_z == f(3) && !bounds.value().corrupted,
        "initial movement geometry scales the collision-empty unit box and starts uncorrupted");
    eawr::assets::Mesh mesh; mesh.collidable = true; mesh.bounds_min = {-10, -20, -7}; mesh.bounds_max = {10, 20, 5};
    model.meshes.push_back(mesh);
    bounds = cp::prepare_initial_movement_planet(1, model, f(3));
    expect(bounds && bounds.value().bounds_half_z == f(18), "movement Z half-extent uses collision bounds times scale");
    expect(!cp::prepare_initial_movement_planet(1, model, {}) && !cp::prepare_initial_movement_planet(0, model, f(3)),
        "missing scale or planet identity refuse");
}
} // namespace
int case_galactic_starting_forces_tests() {
#if defined(_MSC_VER)
    expect(_putenv_s("EAWR_VERIFY_GALACTIC_HASH", "1") == 0, "full galactic hash verifier enabled");
#else
    expect(setenv("EAWR_VERIFY_GALACTIC_HASH", "1", 1) == 0, "full galactic hash verifier enabled");
#endif
    native_actor_change_rollback();
    reconciled_placement_contracts();
    duplicate_authored_starbases();
    level_one_starbase_clears_space_structures();
    excess_special_structures();
    allocation_and_identity(); nested_live_stealth(); guards_and_hostility(); bases_structures_and_chains(); root_checkpoint_and_parity(); root_checkpoint_and_parity(true); root_checkpoint_and_parity(false, true); initial_ability_predicates(); initial_relations(); initial_geometry(); initial_movement_roles();
    return galactic_test::failures ? 1 : 0;
}
