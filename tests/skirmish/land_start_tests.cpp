#include "eawr/skirmish/land_start.hpp"
#include "eawr/skirmish/land_content.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/math/trig.hpp"
#include "eawr/sim/math/trig.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/checkpoint/tactical.hpp"
#include "../support/check.hpp"
#include "../support/env.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

namespace {
using namespace eawr;
using namespace sim::tactical;
using sim::math::Fixed;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
template<class T> T take(core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(value).value();
}
Fixed whole(std::int64_t n) { return Fixed::from_raw(n * Fixed::scale); }

units::CollisionMesh collision_fixture(std::vector<std::array<sim::math::Vec3, 3>> triangles) {
    std::vector<sim::math::Vec3> vertices;
    std::vector<units::IndexedCollisionTriangles::Indices> indices;
    for (const auto& triangle : triangles) {
        units::IndexedCollisionTriangles::Indices corners{};
        for (std::size_t corner = 0; corner < triangle.size(); ++corner) {
            corners[corner] = static_cast<std::uint32_t>(vertices.size());
            vertices.push_back(triangle[corner]);
        }
        indices.push_back(corners);
    }
    units::CollisionMesh result;
    result.triangles = units::IndexedCollisionTriangles(std::move(vertices), std::move(indices));
    return result;
}

skirmish::LandStartBindings explicit_bindings(const units::UnitTables& tables) {
    skirmish::LandStartBindings bindings;
    bindings.identity[0] = 71;
    bindings.land.clear_bit = 1;
    bindings.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    std::set<std::string> classes;
    for (const auto& company : tables.land_companies) {
        LandCompanyBinding binding;
        binding.type = skirmish::type_id(company.id); binding.company = company;
        binding.effective_team_max_speed = company.team_max_speed;
        binding.formation_spacing = Fixed::from_ratio(1, 5).value();
        for (const auto& member : company.members) {
            classes.insert(member.movement_class);
            // Explicit synthetic physical inputs: installed coverage tests L3's data/marker
            // path, not the pending L2b stock locomotion/collision adapter.
            binding.members.push_back({skirmish::type_id(member.type), {whole(40), whole(120), whole(150), whole(2), whole(1), false}});
        }
        bindings.land.companies.push_back(std::move(binding));
    }
    for (const auto& name : classes) bindings.movement_masks.push_back({name, 0xffffffffU});
    for (std::uint32_t player = 1; player <= 8; ++player) bindings.last_known_capacity[player] = 50;
    return bindings;
}

std::vector<std::uint8_t> run(const skirmish::LandStart& built, std::size_t workers, bool reverse = false) {
    auto session = take(TacticalSession::create(built.start.setup, built.content));
    const auto squads = session.land_movement()->squads();
    expect(!squads.empty(), "land start actually creates squads");
    bool ordered = false;
    sim::EntityId moving{};
    sim::math::Vec2 before{};
    const bool authored_forward = std::count_if(built.start.setup.players.begin(), built.start.setup.players.end(),
        [](const auto& player) { return player.commandable(); }) == 8;
    for (const auto& squad : squads) {
        const auto root = session.unit_state(squad.object);
        if (!root || root->owner != 1) continue;
        auto destination = sim::math::Vec3{Fixed::from_raw(squad.position.x.raw() + 40 * Fixed::scale), squad.position.y, {}};
        if (authored_forward) {
            const auto axis = sim::math::sin_cos_turn(squad.facing);
            Fixed dx, dy;
            if (!sim::math::try_multiply(whole(40), axis.cosine, dx)
                || !sim::math::try_multiply(whole(40), axis.sine, dy)
                || !sim::math::try_add(squad.position.x, dx, destination.x)
                || !sim::math::try_add(squad.position.y, dy, destination.y)) throw std::runtime_error("forward target overflow");
        }
        auto command = session.submit({{0, root->owner, squad.object}, {squad.object}, MovePayload{destination}});
        if (!command) throw std::runtime_error(command.error().message);
        moving = squad.object; before = squad.position; ordered = true; break;
    }
    expect(ordered, "human has a company to command");
    platform::ThreadWorkerAdapter executor(workers);
    test_support::ReverseOrderExecutor reversed(workers);
    const bool diagnose = workers == 1 && !reverse && authored_forward;
    if (diagnose) std::cout << "recorded expected failure #4840: the original Dantooine +X 90-degree turn "
        "stalls after 1.72 units against an idle squad member (dd77 diagnostic); "
        "this movement witness follows the authored forward axis and still requires more than ten units\n";
    const auto trace = [&] {
        if (!diagnose) return;
        const auto* motion = session.land_movement();
        const auto squad = std::find_if(motion->squads().begin(), motion->squads().end(),
            [&](const auto& row) { return row.object == moving; });
        if (squad == motion->squads().end()) return;
        const auto index = static_cast<std::size_t>(squad - motion->squads().begin());
        const auto real = [](Fixed value) { return static_cast<double>(value.raw()) / Fixed::scale; };
        std::cout << "Dantooine diagnostic tick=" << session.completed_tick() << " root=" << moving
            << " xy=" << real(squad->position.x) << ',' << real(squad->position.y)
            << " speed=" << real(squad->speed) << " order=" << static_cast<unsigned>(squad->order)
            << " path=" << static_cast<unsigned>(squad->path.status) << " next=" << squad->next_node
            << '/' << squad->path.nodes.size() << " rear=" << real(squad->rear_extent) << '\n';
        const auto axis = sim::math::sin_cos_turn(squad->facing);
        for (const auto& member : motion->members()) if (member.squad == index) {
            Fixed dx, dy, x, y, lag;
            if (!sim::math::try_subtract(squad->position.x, member.position.x, dx)
                || !sim::math::try_subtract(squad->position.y, member.position.y, dy)
                || !sim::math::try_multiply(dx, axis.cosine, x)
                || !sim::math::try_multiply(dy, axis.sine, y)
                || !sim::math::try_add(x, y, lag)) throw std::runtime_error("diagnostic lag overflow");
            std::cout << " member=" << member.object << " xy=" << real(member.position.x) << ',' << real(member.position.y)
                << " lag=" << real(lag) << " slot=" << real(member.slot.x) << ',' << real(member.slot.y)
                << " terrain=" << member.terrain_blocked << " lock=" << member.collision_lock << '\n';
        }
        for (const auto& node : squad->path.nodes) std::cout << " node=" << real(node.x) << ',' << real(node.y);
        std::cout << '\n';
        for (const auto& contact : motion->contacts()) {
            const auto relevant = [&](sim::EntityId id) { return std::any_of(motion->members().begin(), motion->members().end(),
                [&](const auto& member) { return member.object == id && member.squad == index; }); };
            if (relevant(contact.first) || relevant(contact.second)) std::cout << " contact=" << contact.first << ',' << contact.second
                << " kind=" << static_cast<unsigned>(contact.kind) << " state=" << static_cast<unsigned>(contact.state) << '\n';
        }
    };
    trace();
    for (int tick = 0; tick < 300; ++tick) {
        take(session.step(reverse ? static_cast<const sim::PartitionExecutor&>(reversed) : executor));
        if (tick == 11 || tick == 49 || tick == 109 || tick == 149 || tick == 299) trace();
        if (tick == 149) {
            const auto saved = take(checkpoint::save_tactical_checkpoint(session, session.pending_commands(),
                checkpoint::default_seats(session.record().setup), checkpoint::tactical_identities(built.start.setup.content_identity)));
            auto restored = take(checkpoint::load_tactical_checkpoint(saved, checkpoint::tactical_identities(built.start.setup.content_identity),
                [&](const TacticalReplay& replay) { return TacticalSession::from_replay(replay, built.content); }, executor));
            expect(restored.session.canonical_state_bytes() == session.canonical_state_bytes(), "authored land static state restores exactly");
            session = std::move(restored.session);
        }
    }
    const auto final = std::find_if(session.land_movement()->squads().begin(), session.land_movement()->squads().end(),
        [&](const auto& squad) { return squad.object == moving; });
    Fixed moved;
    const bool measured = final != session.land_movement()->squads().end()
        && sim::math::try_length({Fixed::from_raw(final->position.x.raw() - before.x.raw()),
            Fixed::from_raw(final->position.y.raw() - before.y.raw())}, moved);
    expect(measured && moved > whole(10), "session-created authored company moves more than ten units after a move order");
    auto replay = take(TacticalSession::from_replay(take(parse_replay(take(write_replay(session.record())))), built.content));
    for (int tick = 0; tick < 300; ++tick) take(replay.step(executor));
    expect(replay.canonical_state_bytes() == session.canonical_state_bytes(), "authored land replay preserves static occupants and movement");
    return session.canonical_state_bytes();
}

void compare(const skirmish::Fixture& fixture, const skirmish::LandStartInputs& inputs,
    const skirmish::LandStartBindings& bindings) {
    const auto built = take(skirmish::build_land_start(fixture, inputs, bindings));
    const auto repeated = take(skirmish::build_land_start(fixture, inputs, bindings));
    TacticalReplay first; first.setup = built.start.setup;
    TacticalReplay second; second.setup = repeated.start.setup;
    expect(take(write_replay(first)) == take(write_replay(second)), "same inputs produce identical land setup bytes");
    // Compare the complete start census with the existing serializer using a space
    // projection solely for serialization (the production land census is richer than setup).
    const auto census = [](const skirmish::SkirmishStart& start) {
        auto projection = start.setup; projection.domain = BattleDomain::space;
        projection.units.clear();
        for (const auto& unit : start.units) projection.units.push_back(unit.state);
        return take(skirmish::census_json(projection, &start));
    };
    expect(census(built.start) == census(repeated.start), "complete markers, named units and ownership census are deterministic");
    expect(built.notes == repeated.notes, "start notes are deterministic");
    expect(built.start.setup.domain == BattleDomain::land, "start has land battle domain");
    const auto baseline = run(built, 1);
    for (const std::size_t workers : {2U, 4U, 8U}) expect(run(built, workers) == baseline, "land moves are equal on 1/2/4/8 workers");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) expect(run(built, workers, true) == baseline, "300 land ticks equal with reversed partitions");
}

skirmish::LandContentInputs adapter_inputs(const units::UnitTables& tables) {
    auto inputs = skirmish::authored_land_content_inputs(tables);
    expect(inputs.has_value(), "authored land content inputs build");
    return inputs ? std::move(inputs.value()) : skirmish::LandContentInputs{};
}

void authored_open_ground_rear(const skirmish::LandStart& built) {
    const auto& authored = built.content;
    auto content = authored;
    LandTerrainInput field;
    field.width = field.height = 256; field.spacing = whole(20);
    field.heights.resize(256 * 256); field.surfaces.resize(256 * 256);
    field.passability.assign(256 * 256, authored.land.clear_bit);
    field.movement_masks.assign(authored.land_terrain->movement_masks().begin(), authored.land_terrain->movement_masks().end());
    content.land_terrain = std::make_shared<const LandTerrain>(take(LandTerrain::build(std::move(field))));
    content.land.static_objects.clear(); content.land.occupations.clear();
    const sim::InlineExecutor serial;
    for (const auto& company : authored.land.companies) {
        if (company.company.members.size() < 2) continue;
        // Place a stationary authored member first at the same requested origin.
        // The session's collision-aware spawn search must adjust the next parent;
        // its remaining members then exercise a nonzero ideal rear on clear terrain.
        content.land.companies = authored.land.companies;
        auto stationary = company;
        stationary.type = skirmish::type_id("mod-open-ground-spawn-body");
        stationary.company.id = "mod-open-ground-spawn-body";
        stationary.company.members.resize(1); stationary.members.resize(1);
        stationary.company.members.front().create_team = false;
        content.land.companies.push_back(std::move(stationary));
        auto setup = built.start.setup; setup.units.clear();
        UnitState root; root.entity_id = 1; root.owner = 1; root.type_id = content.land.companies.back().type;
        root.position = {whole(1000), whole(1000), {}}; setup.units.push_back(root);
        root.entity_id = 2; root.type_id = company.type; setup.units.push_back(root);
        auto session = take(TacticalSession::create(setup, content));
        const auto squads = session.land_movement()->squads();
        const auto found = std::find_if(squads.begin(), squads.end(), [](const auto& squad) { return squad.object == 2; });
        if (found == squads.end()) continue;
        const auto index = static_cast<std::size_t>(found - squads.begin());
        const auto& squad = *found;
        Fixed expected;
        for (const auto& member : session.land_movement()->members()) if (member.squad == index)
            expected = std::max(expected, Fixed::from_raw(-member.slot.x.raw()));
        // Select an authored formation which exercises the missing nonzero producer.
        // No rear extent or member slot is supplied by the test.
        if (expected <= Fixed{}) continue;
        expect(squad.rear_extent == expected, "session derives the ideal rear extent from authored formation slots");
        const auto before = squad.position;
        const auto submitted = session.submit({{0, 1, 1}, {squad.object},
            MovePayload{{Fixed::from_raw(before.x.raw() + 40 * Fixed::scale), before.y, {}}}});
        if (!submitted) throw std::runtime_error(submitted.error().message);
        for (int tick = 0; tick < 300; ++tick) take(session.step(serial));
        const auto after = session.land_movement()->squads()[index].position;
        Fixed moved;
        expect(sim::math::try_length({Fixed::from_raw(after.x.raw() - before.x.raw()),
            Fixed::from_raw(after.y.raw() - before.y.raw())}, moved) && moved > whole(10),
            "session-created authored squad with a nonzero rear box moves more than ten units on open ground");
        std::cout << "open-ground rear regression company=" << company.company.id
            << " rear_raw=" << expected.raw() << " moved_raw=" << moved.raw() << '\n';
        return;
    }
    expect(false, "installed authored companies supply a nonzero rear-extent regression witness");
}

// #3776 L3: a setup selection over a land map yields land options; a space map keeps its rows as chosen.
void setup_domain_contract() {
    skirmish::SetupMap map;
    map.path = "data/art/maps/_mp_land_example.ted";
    map.name = "Example";
    map.metadata.capacity = 9;
    map.metadata.new_markers = true;
    map.metadata.start_positions = std::vector<assets::Vec2f>(3);
    map.teams = {{0, {"Rebel", "Empire"}}, {1, {"Rebel", "Empire"}}};
    skirmish::SetupSelection selection;
    selection.map = map.path;
    selection.slots = {{1, "Rebel", 0, true, {}}, {2, "Empire", 1, false, {}}};
    selection.slots[1].difficulty = AiLevel::hard;
    const std::vector<skirmish::SetupMap> space_maps{map};
    const auto space = take(skirmish::setup_options(selection, space_maps));
    expect(space.domain == BattleDomain::space && !space.slots->at(1).human && space.slots->at(1).difficulty == AiLevel::hard,
        "a space map keeps the domain space and its AI row");
    map.domain = BattleDomain::land;
    const std::vector<skirmish::SetupMap> land_maps{map};
    const auto land = take(skirmish::setup_options(selection, land_maps));
    expect(land.domain == BattleDomain::land && land.slots->at(0).human && land.slots->at(1).human
            && land.slots->at(1).difficulty == AiLevel{},
        "a land map yields the land domain with no AI rows");
    skirmish::FixtureOptions untouched;
    expect(untouched.domain == BattleDomain::space, "options default to the space domain");
    expect(skirmish::land_skirmish_enabled == (EAWR_LAND_SKIRMISH != 0), "the flag header follows the build option");
}

void contracts() {
    setup_domain_contract();
    units::UnitTables tables; tables.domain = BattleDomain::land;
    units::LandCompany company;
    company.team_type_authored = true; company.team_locomotor = "LAND_TEAM_CONTAINER_LOCOMOTOR";
    company.id = "Mod_Aurora_Company"; company.team_max_speed = whole(40); company.max_squad_size = 2;
    units::LandCompanyMember member;
    member.type = "Mod_Aurora_Member"; member.kind = units::LandObjectKind::infantry;
    member.create_team = true; member.movement_class = "mod-movement"; member.max_speed = whole(40);
    member.locomotor = "MOD_LOCOMOTOR";
    units::LandLocomotionData authored;
    authored.rate_of_turn = whole(24); authored.scale_factor = whole(1); authored.soft_radius = whole(1);
    authored.hard_x = authored.hard_y = whole(1);
    authored.formation_spacing = Fixed::from_ratio(1, 5).value(); authored.collision_class = units::LandCollisionGroup::infantry;
    company.locomotion = {authored, authored}; company.formation_spacing = authored.formation_spacing;
    company.members = {member, member}; tables.land_companies.push_back(company);
    tables.land_movement_classes = {{"mod-movement", {"Clear"}}};
    tables.constants.scalars.push_back({"Object_Max_Speed_Multiplier_Land", Fixed::from_ratio(1, 32).value(), {}});
    skirmish::Fixture fixture;
    fixture.map = "data/custommaps/aurora.ted"; fixture.map_sha256 = std::string(64, 'a'); fixture.seed = 17;
    fixture.slots = {{1, "Mod_Dawn", 0, true, {}}, {2, "Mod_Dusk", 1, true, {}}};
    skirmish::LandStartInputs inputs;
    inputs.map.kind = assets::MapKind::land;
    inputs.map.lobby.capacity = 8; inputs.map.lobby.new_markers = true;
    inputs.map.lobby.start_positions = std::vector<assets::Vec2f>{{0, 0}, {1, 1}};
    inputs.map.terrain.emplace(); auto& terrain = *inputs.map.terrain;
    terrain.width = terrain.height = 48; terrain.materials.emplace_back();
    terrain.samples.resize(48 * 48);
    for (int i = 0; i < 48 * 48; ++i) for (unsigned b = 0; b < 4; ++b)
        inputs.map.raw_passability.push_back(static_cast<std::byte>(b == 0 ? 1 : 0));
    inputs.common.match_defaults.max_tech = 5;
    inputs.common.map = fixture.map; inputs.common.map_sha256 = fixture.map_sha256; inputs.common.tables = &tables;
    inputs.common.factions = {{"Mod_Dawn", true, true, false}, {"Mod_Dusk", true, true, false}, {"Mod_Neutral", false, true, true}, {"Mod_Absent", false, false, false}};
    for (std::uint32_t i = 0; i < 8; ++i) inputs.common.lobby_colours.push_back({"mod-colour", {static_cast<std::uint8_t>(i), 0, 0}});
    for (std::uint32_t team = 0; team < 2; ++team) for (std::uint32_t spawn = 0; spawn < 4; ++spawn) {
        skirmish::MapPlacement p;
        p.record = team * 4 + spawn; p.type = team == 0 ? "Team_00_Spawn_Point_Marker" : "Team_01_Spawn_Point_Marker";
        p.marker = true; p.position = {whole(150 + team * 400), whole(150 + spawn * 140), {}}; p.orientation_degrees = {};
        p.orientation_degrees.emplace(); inputs.common.placements.push_back(p);
    }
    for (std::uint32_t owner = 0; owner < 5; ++owner) {
        skirmish::MapPlacement p;
        p.record = 10 + owner; p.type = "mod-prop"; p.owner_index = static_cast<std::int32_t>(owner);
        p.position = {whole(100), whole(100), {}}; p.orientation_degrees.emplace();
        p.decoration = true; p.discardable = false; inputs.common.placements.push_back(p);
    }
    for (const auto& faction : {"Mod_Dawn", "Mod_Dusk"}) {
        skirmish::LandFactionStart row; row.faction = faction; row.forces = {company.id};
        row.company_cap = take(sim::galactic::parse_income_curve("0,12, 1,12, 8,5")); inputs.factions.push_back(row);
    }
    auto bindings = explicit_bindings(tables);
    const auto authored_inputs = adapter_inputs(tables);
    // Menu start (no bindings file) uses this default: Clear is bit 1, the build is deterministic, all 8 seats have capacity.
    expect(!authored_inputs.unverified_passability_bits.empty() && authored_inputs.unverified_passability_bits.front().name == "Clear"
        && authored_inputs.unverified_passability_bits.front().bits == 1, "default land content gates start with the Clear bit");
    expect(authored_inputs.last_known_capacity.size() == 8 && authored_inputs.last_known_capacity.at(8) == 50, "default land content covers eight seats");
    expect(adapter_inputs(tables).unverified_passability_bits.size() == authored_inputs.unverified_passability_bits.size(), "default land content is deterministic");
    const auto authored_start = take(skirmish::build_land_start(fixture, inputs, authored_inputs));
    auto static_inputs = inputs;
    units::LandStaticObject prop;
    prop.id = "mod-prop"; prop.xml_type = "Props_Generic"; prop.land_obstacle = true;
    prop.width = whole(60); prop.height = whole(100); prop.x_offset = whole(-30); prop.y_offset = whole(-50);
    prop.fine_tune = false; prop.scale_factor = whole(1);
    static_inputs.static_tables.land_static_objects.push_back(prop);
    auto static_gates = authored_inputs;
    static_gates.unverified_passability_bits.push_back({"ModObstacle", 2});
    skirmish::LandStaticGate prop_gate; prop_gate.type = prop.id; prop_gate.primary_passability = 2;
    static_gates.unverified_static_objects.push_back(prop_gate);
    const auto static_start = take(skirmish::build_land_start(fixture, static_inputs, static_gates));
    expect(!static_start.content.land.occupations.empty() && static_start.start.setup.units.size() > authored_start.start.setup.units.size(),
        "custom blocking prop types enter the authored simulation without a name list");
    const auto prop_session = take(TacticalSession::create(static_start.start.setup, static_start.content));
    expect(!prop_session.land_occupancy()->admits(5, 5, 1), "authored rectangular prop footprint blocks cells at session creation");
    auto missing_extent = static_inputs;
    missing_extent.static_tables.land_static_objects.front().width.reset();
    const auto bad_prop = skirmish::build_land_start(fixture, missing_extent, static_gates);
    expect(static_cast<bool>(bad_prop), "WLPA-62/65: missing model and width produce an empty raster without refusal");
    const auto cells_for = [&](units::LandStaticObject value) {
        auto variant = static_inputs; variant.static_tables.land_static_objects.front() = std::move(value);
        const auto built = take(skirmish::build_land_start(fixture, variant, static_gates));
        expect(!built.content.land.occupations.empty(), "ordinary static object retains an occupation even with no raster samples");
        return built.content.land.occupations.front().primary_cells;
    };
    auto endpoint_prop = prop;
    // Nearest-cell rounding maps 99/109 together, while the far sample 119
    // reaches the next cell. Keep the extra endpoint sample observable after dedup.
    endpoint_prop.x_offset = whole(-1); endpoint_prop.y_offset = whole(-1);
    endpoint_prop.width = whole(20); endpoint_prop.height = whole(20);
    const auto exact_cells = cells_for(endpoint_prop);
    // LSL-08: a negative authored width (retail parses any float) is kept; it rasterizes no cells and never fails the start.
    auto negative_prop = endpoint_prop; negative_prop.width = whole(-1);
    expect(cells_for(negative_prop).empty(), "LSL-08: a negative obstacle width starts with an empty raster");
    endpoint_prop.width = whole(21);
    const auto nonexact_cells = cells_for(endpoint_prop);
    expect(exact_cells.size() == 1 && nonexact_cells.size() == 2,
        "WLPA-65: exact far endpoint excluded, nonexact next sample included");
    endpoint_prop.width = Fixed{};
    expect(cells_for(endpoint_prop).empty(), "WLPA-65: zero width has no samples");
    endpoint_prop.width = whole(40); endpoint_prop.height = whole(40);
    endpoint_prop.model_present = true; endpoint_prop.fine_tune.reset();
    endpoint_prop.collision_meshes.push_back(collision_fixture({
        {sim::math::Vec3{whole(-2), whole(-2), {}}, {whole(19), whole(-2), {}}, {whole(-2), whole(19), {}}}}));
    const auto refined_cells = cells_for(endpoint_prop);
    endpoint_prop.fine_tune = false;
    expect(!refined_cells.empty() && refined_cells.size() < cells_for(endpoint_prop).size(),
        "WLPA-64: default refinement tests collision triangles, leaving the model's empty region unoccupied");
    endpoint_prop.fine_tune.reset(); endpoint_prop.model_present = false;
    expect(cells_for(endpoint_prop).empty(), "WLPA-64: refined ordinary object without model contributes no occupation samples");
    {
        const auto field = [](std::uint32_t id, std::uint32_t value) {
            assets::RawField row; row.id = id;
            for (unsigned byte = 0; byte < 4; ++byte) row.bytes.push_back(static_cast<std::byte>(value >> (8U * byte)));
            return row;
        };
        auto water_tables = tables; water_tables.land_movement_classes.front().passabilities.push_back("Water");
        auto water_inputs = static_inputs; water_inputs.common.tables = &water_tables;
        for (std::size_t byte = 0; byte < water_inputs.map.raw_passability.size(); byte += 4)
            water_inputs.map.raw_passability[byte] = static_cast<std::byte>(5); // Clear | explicit Water bit
        water_inputs.map.water_records.push_back({"1/257/0", 0, {field(0x16, 1), field(0x15, 0)}});
        auto& water_prop = water_inputs.static_tables.land_static_objects.front();
        water_prop.collision_bounds = units::CollisionBounds{{whole(-1), whole(-1), whole(-5)}, {whole(1), whole(1), whole(5)}};
        auto water_gates = static_gates; water_gates.unverified_passability_bits.push_back({"Water", 4});
        const auto equality = take(skirmish::build_land_start(fixture, water_inputs, water_gates));
        expect(!equality.content.land.occupations.front().primary_cells.empty(), "WLPA-09: water plus five equal to ground plus model Z extent contributes");
        water_prop.collision_bounds = units::CollisionBounds{{whole(-1), whole(-1), whole(-4)}, {whole(1), whole(1), whole(4)}};
        const auto submerged = take(skirmish::build_land_start(fixture, water_inputs, water_gates));
        expect(submerged.content.land.occupations.front().primary_cells.empty(), "WLPA-09: strictly submerged obstacle samples do not contribute");
    }
    auto changed_gate = static_gates; changed_gate.unverified_static_objects.front().primary_passability = 4;
    expect(take(skirmish::build_land_start(fixture, static_inputs, changed_gate)).start.setup.content_identity
        != static_start.start.setup.content_identity, "static passability gates bind match identity");
    expect(authored_start.content.land.companies.front().members.front().locomotion.maximum_speed
        == Fixed::from_decimal("37.5").value(), "L3 overload builds authored profiles from loaded land tables");
    const std::array<std::string, 1> selected{company.id};
    compare(fixture, inputs, take(skirmish::build_land_bindings(tables, selected, authored_inputs)));
    // LC-02/06: fail creation of the non-team first member. The first survivor forms
    // a team and must use the converted team speed, never its raw frame-unit value.
    auto partial_tables = tables;
    partial_tables.land_companies.front().members.front().create_team = false;
    partial_tables.land_companies.front().members.front().max_speed = whole(5);
    auto partial_inputs = inputs; partial_inputs.common.tables = &partial_tables;
    auto partial_bindings = take(skirmish::build_land_bindings(partial_tables, selected, authored_inputs));
    partial_bindings.land.companies.front().members.front().type = 0;
    const auto partial_start = take(skirmish::build_land_start(fixture, partial_inputs, partial_bindings));
    const auto partial_session = take(TacticalSession::create(partial_start.start.setup, partial_start.content));
    expect(partial_session.land_movement()->members().size() == 2
        && std::all_of(partial_session.land_movement()->squads().begin(), partial_session.land_movement()->squads().end(),
            [](const auto& squad) { return squad.maximum_speed == Fixed::from_decimal("37.5").value(); }),
        "LC-02/06: first created team-forming survivor uses converted team maximum speed");
    partial_bindings.land.companies.front().effective_team_max_speed.reset();
    expect(!skirmish::build_land_start(fixture, partial_inputs, partial_bindings),
        "a team-forming first survivor refuses an explicit binding without converted team speed");
    compare(fixture, inputs, bindings);
    auto built = take(skirmish::build_land_start(fixture, inputs, bindings));
    expect(built.start.removed.size() == 2 && built.start.units.size() == 5 && built.start.setup.units.size() == 2,
        "WLEN-53 playable placements removed; neutral, missing-player decoration and out-of-range owner retained in census");
    expect(built.content.economy.players[0].population_cap == 11 && built.content.economy.players[0].start_tech == 1,
        "LC-46 curve uses controllable players; WLEN-54 tech starts at one");
    bindings.last_known_capacity[1] = 3;
    built = take(skirmish::build_land_start(fixture, inputs, bindings));
    expect(built.content.economy.players[0].population_cap == 3, "last-known capacity clamps faction cap");
    auto missing = bindings; missing.land.companies.clear();
    expect(!skirmish::build_land_start(fixture, inputs, missing), "missing effective company binding is refused");
    auto disagree = bindings; disagree.land.companies[0].company.members[0].type = "wrong-member";
    expect(!skirmish::build_land_start(fixture, inputs, disagree), "binding must match loaded L2 company plan");
    auto space = inputs; space.map.kind = assets::MapKind::space;
    expect(!skirmish::build_land_start(fixture, space, bindings), "land builder refuses space");
    auto bad_tech = fixture;
    bad_tech.match = inputs.common.match_defaults; bad_tech.match->max_tech = 0;
    const auto refused_tech = skirmish::build_land_start(bad_tech, inputs, bindings);
    expect(!refused_tech && refused_tech.error().code == skirmish::diagnostic_codes::input,
        "an invalid authored maximum tech is refused without clamping");
    auto absent_forces = inputs;
    absent_forces.factions[0].forces.clear(); absent_forces.factions[0].forces_authored = false;
    const auto empty_start = take(skirmish::build_land_start(fixture, absent_forces, bindings));
    expect(std::any_of(empty_start.notes.begin(), empty_start.notes.end(), [](const auto& note) { return note.find("Mod_Dawn") != std::string::npos; })
        && empty_start.start.setup.units.size() == 1, "missing optional forces is explicitly reported with no default companies");
    absent_forces.factions[0].forces_authored = true;
    const auto empty_authored = take(skirmish::build_land_start(fixture, absent_forces, bindings));
    expect(std::none_of(empty_authored.notes.begin(), empty_authored.notes.end(), [](const auto& note) { return note.find("Mod_Dawn") != std::string::npos; }),
        "authored empty forces differs from absent forces");
    auto too_few = inputs;
    std::erase_if(too_few.common.placements, [](const auto& p) { return p.type == "Team_01_Spawn_Point_Marker"; });
    expect(!skirmish::build_land_start(fixture, too_few, bindings), "missing team spawn markers is refused");
    auto tilted = inputs;
    tilted.common.placements[3].orientation_degrees->x = whole(1);
    expect(!skirmish::build_land_start(fixture, tilted, bindings), "tilted selected spawn marker is refused");
    bindings.identity = {};
    expect(!skirmish::build_land_start(fixture, inputs, bindings), "effective content identity is required");
}

void live_stock_move(const vfs::Vfs&, const data::Catalog&, scene::VfsAssetCache&);
struct TurningResult {
    std::vector<std::uint8_t> state;
    Fixed parent_distance, member_progress;
    bool operator==(const TurningResult&) const = default;
};

TurningResult turning_move(const skirmish::LandStart& built, const sim::PartitionExecutor& executor,
    bool naboo, bool restore, bool trace, bool verify_replay, bool repulsion = true) {
    auto session = take(TacticalSession::create(built.start.setup, built.content));
    const_cast<LandMovement*>(session.land_movement())->set_straggler_repulsion(repulsion);
    const auto squads = session.land_movement()->squads();
    std::vector<UnitState> own;
    for (const auto& unit : built.start.setup.units)
        if (unit.owner == 1 && std::any_of(squads.begin(), squads.end(),
            [&](const auto& squad) { return squad.object == unit.entity_id; })) own.push_back(unit);
    if (own.size() < 2) throw std::runtime_error("turning reproduction needs two authored human companies");
    const auto root = own[naboo ? 1U : 0U].entity_id;
    const auto selected = std::find_if(squads.begin(), squads.end(), [&](const auto& s) { return s.object == root; });
    if (selected == squads.end()) throw std::runtime_error("turning reproduction has no moving squad");
    const auto before = selected->position;
    sim::math::Vec2 member_before;
    if (!naboo) {
        const auto members = session.land_movement()->members();
        const auto member = std::find_if(members.begin(), members.end(), [](const auto& m) { return m.object == 1103; });
        if (member == members.end()) throw std::runtime_error("Dantooine expected-limit fixture has no member 1103");
        member_before = member->position;
    }
    const auto target = naboo ? own[0].position : sim::math::Vec3{before.x, before.y, {}};
    const auto destination = sim::math::Vec3{Fixed::from_raw(target.x.raw() + (naboo ? 100 : 40) * Fixed::scale), target.y, {}};
    take(session.submit({{naboo ? 11U : 0U, 1, 0}, {root}, MovePayload{destination}}));
    if (naboo) take(session.submit({{111, 1, 1}, {root}, StopPayload{}}));
    for (int tick = 0; tick < 150; ++tick) {
        take(session.step(executor));
        if (restore && tick == 49) {
            const auto saved = take(checkpoint::save_tactical_checkpoint(session, session.pending_commands(),
                checkpoint::default_seats(session.record().setup), checkpoint::tactical_identities(built.start.setup.content_identity)));
            auto loaded = take(checkpoint::load_tactical_checkpoint(saved, checkpoint::tactical_identities(built.start.setup.content_identity),
                [&](const TacticalReplay& replay) { return TacticalSession::from_replay(replay, built.content); }, executor));
            expect(loaded.session.canonical_state_bytes() == session.canonical_state_bytes(), "turning contact checkpoint restores exactly");
            session = std::move(loaded.session);
            const_cast<LandMovement*>(session.land_movement())->set_straggler_repulsion(repulsion);
        }
        if (trace && (tick == 11 || tick == 49 || tick >= 139)) {
            const auto* motion = session.land_movement();
            const auto s = std::find_if(motion->squads().begin(), motion->squads().end(), [&](const auto& row) { return row.object == root; });
            const auto real = [](Fixed n) { return static_cast<double>(n.raw()) / Fixed::scale; };
            std::cout << "turning map=" << (naboo ? "Naboo" : "Dantooine") << " tick=" << tick + 1 << " root=" << root
                << " position=" << real(s->position.x) << ',' << real(s->position.y) << " speed=" << real(s->speed)
                << " temporary=" << s->temporary_first << " facing=" << real(s->facing)
                << " rear=" << real(s->rear_extent) << " maximum=" << real(s->maximum_speed) << '\n';
            const auto index = static_cast<std::size_t>(s - motion->squads().begin());
            for (const auto& member : motion->members()) if (member.squad == index) std::cout << " member=" << member.object
                << " position=" << real(member.position.x) << ',' << real(member.position.y) << " lock=" << member.collision_lock
                << " facing=" << real(member.facing) << " speed=" << real(member.speed) << " assigned=" << real(member.assigned_speed)
                << " collision=" << real(member.collision_speed) << " state=" << static_cast<int>(member.state)
                << " terrain=" << member.terrain_blocked << " radius=" << real(member.profile.radius)
                << " maximum=" << real(member.profile.maximum_speed) << " turn=" << real(member.profile.turn_rate) << '\n';
            for (const auto& contact : motion->contacts()) {
                const auto touches = [&](sim::EntityId id) {
                    return std::any_of(motion->members().begin(), motion->members().end(),
                        [&](const auto& member) { return member.object == id && member.squad == index; });
                };
                if (!touches(contact.first) && !touches(contact.second)) continue;
                std::cout << " contact=" << contact.first << ',' << contact.second << " kind=" << static_cast<int>(contact.kind)
                    << " state=" << static_cast<int>(contact.state) << " stopped=" << contact.first_stopped << ',' << contact.second_stopped
                    << " limit=" << (contact.first_speed_limit ? real(*contact.first_speed_limit) : -1.0) << ','
                    << (contact.second_speed_limit ? real(*contact.second_speed_limit) : -1.0) << '\n';
                for (const auto& peer : motion->members()) if (peer.squad != index
                    && (peer.object == contact.first || peer.object == contact.second))
                    std::cout << " peer=" << peer.object << " position=" << real(peer.position.x) << ',' << real(peer.position.y)
                        << " radius=" << real(peer.profile.radius) << '\n';
            }
        }
    }
    const auto final_squads = session.land_movement()->squads();
    const auto final = std::find_if(final_squads.begin(), final_squads.end(), [&](const auto& s) { return s.object == root; });
    Fixed distance;
    const bool measured = final != final_squads.end() && sim::math::try_length(
        {Fixed::from_raw(final->position.x.raw() - before.x.raw()), Fixed::from_raw(final->position.y.raw() - before.y.raw())}, distance);
    Fixed member_progress;
    if (naboo) {
        expect(measured && distance > whole(10), "stock turning squad moves more than ten units within 150 ticks beside idle companies");
    } else {
        const auto members = session.land_movement()->members();
        const auto member = std::find_if(members.begin(), members.end(), [](const auto& m) { return m.object == 1103; });
        const bool member_measured = member != members.end() && sim::math::try_length(
            {Fixed::from_raw(member->position.x.raw() - member_before.x.raw()),
             Fixed::from_raw(member->position.y.raw() - member_before.y.raw())}, member_progress);
        expect(member_measured, "#4840 Dantooine expected-limit member progress is measurable");
        if (repulsion) {
            expect(measured && distance > Fixed{}, "#4840 Dantooine expected-limit parent retains initial movement");
            const auto index = static_cast<std::size_t>(final - final_squads.begin());
            for (const auto& m : members) if (m.squad == index)
                expect(m.collision_lock == 0 && !m.terrain_blocked,
                    "#4840 Dantooine expected-limit company has no collision lock or terrain block");
        }
    }
    if (verify_replay) {
        auto replay = take(TacticalSession::from_replay(take(parse_replay(take(write_replay(session.record())))), built.content));
        const_cast<LandMovement*>(replay.land_movement())->set_straggler_repulsion(repulsion);
        for (int tick = 0; tick < 150; ++tick) take(replay.step(executor));
        expect(replay.canonical_state_bytes() == session.canonical_state_bytes(), "stock turning move replay matches uninterrupted state");
    }
    return {session.canonical_state_bytes(), distance, member_progress};
}

void compare_turning(const skirmish::LandStart& built, bool naboo) {
    sim::InlineExecutor serial;
    // Replay one uninterrupted witness per map; all worker/restore runs compare
    // against those same complete bytes without replaying the identical leg again.
    const auto main_progress = naboo ? Fixed{} : turning_move(built, serial, false, false, false, false, false).member_progress;
    const auto expected_limit = [&](const TurningResult& result) {
        if (!naboo) expect(result.member_progress > main_progress,
            "#4840 Dantooine expected-limit member 1103 advances farther than the main-equivalent policy");
    };
    const auto baseline = turning_move(built, serial, naboo, false, true, true);
    expected_limit(baseline);
    if (!naboo) {
        const auto real = [](Fixed n) { return static_cast<double>(n.raw()) / Fixed::scale; };
        std::cout << "expected-limit #4840 Dantooine: member1103 progress main-policy=" << real(main_progress)
            << " head=" << real(baseline.member_progress) << " parent=" << real(baseline.parent_distance)
            << "; collision-speed decay between two idle peers and the WLPA-30 parent wait remain open\n";
    }
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        const auto result = turning_move(built, executor, naboo, true, false, false);
        expect(result == baseline, "stock turning contact equals at 1/2/4/8 workers");
        expected_limit(result);
        test_support::ReverseOrderExecutor reversed(workers);
        const auto reverse = turning_move(built, reversed, naboo, true, false, false);
        expect(reverse == baseline, "stock turning contact equals with reverse partitions");
        expected_limit(reverse);
    }
}

void naboo_turning_move(const vfs::Vfs& filesystem, const data::Catalog& catalog, scene::VfsAssetCache& cache) {
    skirmish::Fixture fixture;
    fixture.map = "data/art/maps/_mp_land_naboo.ted";
    fixture.slots = {{1, "Rebel", 0, true, {}}, {2, "Empire", 1, true, {}}};
    units::UnitTables empty;
    const auto discovered = take(skirmish::read_land_start_inputs(fixture, filesystem, catalog, empty));
    fixture.map_sha256 = discovered.common.map_sha256;
    units::LoadInput load;
    load.domain = BattleDomain::land; load.catalog = &catalog; load.filesystem = &filesystem;
    load.model = cache.access().model;
    std::set<std::string> requested;
    for (const auto& faction : discovered.factions) requested.insert(faction.forces.begin(), faction.forces.end());
    load.types.assign(requested.begin(), requested.end());
    auto tables = take(units::load_unit_tables(load));
    for (const auto& company : tables.land_companies)
        for (const auto& member : company.members) requested.insert(member.type);
    load.types.assign(requested.begin(), requested.end());
    tables = take(units::load_unit_tables(load));
    const auto inputs = take(skirmish::read_land_start_inputs(fixture, filesystem, catalog, tables));
    auto gates = adapter_inputs(tables);
    gates.last_known_capacity = {{1, 50}, {2, 50}};
    const auto built = take(skirmish::build_land_start(fixture, inputs, gates));
    compare_turning(built, true);
    // LS shoot PR 1: the stock companies' combat and durability rows ride in the content, deterministically.
    const auto repeated = take(skirmish::build_land_start(fixture, inputs, gates));
    expect(!built.content.combat.profiles.empty() && !built.content.durability.profiles.empty(),
        "stock land start binds the land types' combat and durability rows");
    expect(built.content.combat == repeated.content.combat && built.start.setup.content_identity == repeated.start.setup.content_identity
            && built.start.identity == repeated.start.identity,
        "stock land start with combat content builds deterministically");
    const auto domain = std::find_if(built.start.identity.domains.begin(), built.start.identity.domains.end(),
        [](const auto& row) { return row.name == "land-content"; });
    expect(domain != built.start.identity.domains.end() && domain->version == 2,
        "the land-content identity domain is version 2 with combat rows");
}

// #3776 L3 witness: the setup screen's selection becomes land FixtureOptions, and those options build the land start.
void setup_to_land_start(const vfs::Vfs& filesystem, const data::Catalog& catalog, scene::VfsAssetCache& cache,
    const std::vector<skirmish::SetupMap>& maps) {
    const auto map = std::find_if(maps.begin(), maps.end(), [](const auto& entry) {
        return entry.path.find("_mp_land_naboo") != std::string::npos && entry.unavailable.empty() && entry.teams.size() >= 2;
    });
    expect(map != maps.end(), "setup lists the stock Naboo land map");
    if (map == maps.end()) return;
    expect(map->domain == BattleDomain::land, "a land_setup_maps entry carries the land domain");
    skirmish::SetupSelection selection;
    selection.map = map->path;
    selection.slots = {{1, "Rebel", map->teams[0].team, true, {}}, {2, "Empire", map->teams[1].team, false, {}}};
    selection.slots[1].difficulty = AiLevel::hard;
    const auto options = take(skirmish::setup_options(selection, maps));
    expect(options.domain == BattleDomain::land, "the setup options carry the land domain");
    expect(options.slots && options.slots->size() == 2 && options.slots->at(0).human && options.slots->at(1).human
            && options.slots->at(1).difficulty == AiLevel{},
        "land has no AI seats: every occupied row is an unattended human seat");
    expect(!skirmish::fixture_from_options(options, filesystem, catalog),
        "the space/network fixture path refuses a land setup by name");
    skirmish::Fixture fixture;
    fixture.map = *options.map; fixture.slots = *options.slots; fixture.seed = options.seed.value_or(0); fixture.match = options.match;
    units::UnitTables empty;
    const auto discovered = take(skirmish::read_land_start_inputs(fixture, filesystem, catalog, empty));
    fixture.map_sha256 = discovered.common.map_sha256;
    units::LoadInput load;
    load.domain = BattleDomain::land; load.catalog = &catalog; load.filesystem = &filesystem;
    load.model = cache.access().model;
    std::set<std::string> requested;
    for (const auto& faction : discovered.factions) requested.insert(faction.forces.begin(), faction.forces.end());
    load.types.assign(requested.begin(), requested.end());
    auto tables = take(units::load_unit_tables(load));
    for (const auto& company : tables.land_companies)
        for (const auto& member : company.members) requested.insert(member.type);
    load.types.assign(requested.begin(), requested.end());
    tables = take(units::load_unit_tables(load));
    const auto inputs = take(skirmish::read_land_start_inputs(fixture, filesystem, catalog, tables));
    auto gates = adapter_inputs(tables);
    gates.last_known_capacity = {{1, 50}, {2, 50}};
    const auto built = take(skirmish::build_land_start(fixture, inputs, gates));
    // The start also carries the map's non-seat players (the neutral owner and any hostile or passive
    // owner the TED places props or units for); only the lobby seats are commandable.
    std::vector<sim::tactical::PlayerId> seats;
    for (const auto& player : built.start.setup.players)
        if (player.commandable()) seats.push_back(player.player_id);
    std::sort(seats.begin(), seats.end());
    expect(built.start.setup.domain == BattleDomain::land && seats == std::vector<sim::tactical::PlayerId>{1, 2},
        "setup options start a two-seat land battle");
    if (seats != std::vector<sim::tactical::PlayerId>{1, 2})
        std::cout << "land setup start: domain=" << static_cast<int>(built.start.setup.domain)
                  << " players=" << built.start.setup.players.size() << " commandable=" << seats.size() << "\n";
}

// The flag decides only the list: off is exactly setup_maps, on appends the land maps behind the space maps.
void domain_lists(const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    const skirmish::SetupMapQuery query;
    const auto space = take(skirmish::setup_maps(filesystem, catalog, query));
    const auto off = take(skirmish::setup_domain_maps(filesystem, catalog, query, false));
    expect(off.size() == space.size() && std::equal(off.begin(), off.end(), space.begin(), [](const auto& a, const auto& b) {
        return a.path == b.path && a.name == b.name && a.domain == BattleDomain::space && b.domain == BattleDomain::space;
    }), "flag off: the setup list is the unchanged space list");
    const auto on = take(skirmish::setup_domain_maps(filesystem, catalog, query, true));
    const auto land = take(skirmish::land_setup_maps(filesystem, catalog, query));
    expect(!land.empty() && on.size() == space.size() + land.size(), "flag on: the land maps follow the space maps");
    expect(std::equal(space.begin(), space.end(), on.begin(), [](const auto& a, const auto& b) { return a.path == b.path; }),
        "flag on: the space maps keep their order and place");
    expect(std::all_of(land.begin(), land.end(), [](const auto& map) { return map.domain == BattleDomain::land; }),
        "land maps carry the land domain");
    skirmish::SetupMapQuery land_query;
    land_query.kind = assets::MapKind::land;
    const auto passthrough = take(skirmish::setup_domain_maps(filesystem, catalog, land_query, true));
    const auto plain = take(skirmish::setup_maps(filesystem, catalog, land_query));
    expect(passthrough.size() == plain.size(), "a non-space query is passed through unchanged");
}

int installed() {
    const auto root = testing::environment("EAWR_EAW_GAME_ROOT");
    if (!root || root->empty()) { std::cout << "SKIPPED: no game data\n"; return 77; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"})
        mounts.push_back(take(vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data")).mount);
    const auto filesystem = take(vfs::Vfs::mount(mounts));
    const auto loaded = take(data::load_catalog(filesystem, data::Profile::foc));
    const auto maps = take(skirmish::land_setup_maps(filesystem, loaded.catalog));
    scene::VfsAssetCache cache(filesystem); const auto access = cache.access();
    live_stock_move(filesystem, loaded.catalog, cache);
    units::LoadInput all_load; all_load.domain = BattleDomain::land; all_load.catalog = &loaded.catalog;
    all_load.filesystem = &filesystem; all_load.model = access.model;
    for (const auto& definition : loaded.catalog.definitions()) {
        const auto kind = core::to_lower(definition.type_name);
        if (definition.namespace_winner && definition.category == data::Category::game_object && kind.ends_with("company"))
            all_load.types.push_back(definition.id);
    }
    const auto all_tables = take(units::load_unit_tables(all_load));
    const auto all_gates = adapter_inputs(all_tables);
    std::size_t admitted{}, refused{};
    for (const auto& name : all_load.types) {
        const std::array<std::string, 1> selected{name};
        const auto binding = skirmish::build_land_bindings(all_tables, selected, all_gates);
        if (binding) ++admitted;
        else { ++refused; std::cout << "land binding refusal: " << binding.error().message << '\n'; }
    }
    expect(admitted > 0 && admitted + refused == all_load.types.size(), "every stock land company has a binding or a named refusal");
    std::cout << "stock land bindings valid=" << admitted << " refused=" << refused << '\n';
    for (const auto* name : {"Bothan_Civ_Company_Mission", "Empire_Prison_Survivors_01", "Empire_Prison_Survivors_02",
        "Geonosian_Resistors", "Bothan_Resistors"}) {
        const std::array<std::string, 1> selected{name};
        const auto binding = skirmish::build_land_bindings(all_tables, selected, all_gates);
        expect(binding && !binding.value().land.companies.front().effective_team_max_speed,
            "LC-83 stock generic-team company is admitted without a container speed cap");
        if (binding) std::cout << "stock generic-team admitted: " << name << '\n';
    }
    for (const std::uint32_t players : {2U, 8U}) {
        const auto map_name = players == 2 ? "_mp_land_alderaan" : "_mp_land_dantooine";
        const auto selected = std::find_if(maps.begin(), maps.end(), [&](const auto& map) {
            return map.path.find(map_name) != std::string::npos && map.unavailable.empty() && map.metadata.capacity == players && map.teams.size() >= 2
                && map.teams[0].spawn_capacity.value_or(0) >= players / 2 && map.teams[1].spawn_capacity.value_or(0) >= players / 2;
        });
        expect(selected != maps.end(), "stock land map exists for requested 1v1/4v4 capacity");
        if (selected == maps.end()) continue;
        skirmish::Fixture fixture; fixture.map = selected->path; fixture.seed = 91;
        for (std::uint32_t slot = 1; slot <= players; ++slot) {
            const auto team = slot <= players / 2 ? 0U : 1U;
            fixture.slots.push_back({slot, team == 0 ? "Rebel" : "Empire", team, true, {}});
        }
        units::UnitTables empty;
        const auto discovered = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, empty));
        fixture.map_sha256 = discovered.common.map_sha256;
        units::LoadInput load; load.domain = BattleDomain::land; load.catalog = &loaded.catalog;
        load.filesystem = &filesystem; load.model = access.model;
        for (const auto& faction : discovered.factions) for (const auto& type : faction.forces)
            if (std::find(load.types.begin(), load.types.end(), type) == load.types.end()) load.types.push_back(type);
        auto tables = take(units::load_unit_tables(load));
        tables.land_static_objects = discovered.static_tables.land_static_objects;
        tables.unresolved.insert(tables.unresolved.end(), discovered.static_tables.unresolved.begin(), discovered.static_tables.unresolved.end());
        const auto inputs = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, tables));
        auto gates = adapter_inputs(tables);
        std::size_t bound_static{}, gated_static{}, census_static{}, refused_static{};
        std::size_t bound_authored_offsets{}, bound_test_offsets{}, scale_only_refused{};
        for (const auto& object : tables.land_static_objects) {
            if (!object.land_obstacle) { ++census_static; std::cout << "static census-only: " << object.id << '\n'; continue; }
            skirmish::LandStaticGate gate; gate.type = object.id;
            // Raw enum binding remains explicit; geometry/refinement use sourced defaults.
            const auto obstacle = std::find_if(gates.unverified_passability_bits.begin(), gates.unverified_passability_bits.end(),
                [](const auto& row) { return row.name == "Obstacle"; });
            if (obstacle != gates.unverified_passability_bits.end()) gate.primary_passability = obstacle->bits;
            const auto binding = skirmish::build_land_static_binding(tables, object, gate, gates.unverified_passability_bits);
            if (binding) {
                ++bound_static; gates.unverified_static_objects.push_back(gate);
                if (object.x_offset && object.y_offset) ++bound_authored_offsets;
                else ++bound_test_offsets;
                std::cout << "static bound with raw-bit gate: " << object.id << " offsets="
                    << (object.x_offset && object.y_offset ? "authored" : "model-fallback") << '\n';
            }
            else {
                if (!object.scale_factor && binding.error().message.find("unverified-static-scale") != std::string::npos) {
                    auto witness = gate; witness.scale_factor = whole(1);
                    if (skirmish::build_land_static_binding(tables, object, witness, gates.unverified_passability_bits)) {
                        ++scale_only_refused;
                        std::cout << "static refused-only-absent-scale: " << object.id << '\n';
                    }
                }
                if (binding.error().message.find("gate") != std::string::npos
                    && object.width && object.height) ++gated_static;
                else ++refused_static;
                std::cout << "static named refusal/gate: " << binding.error().code << ' ' << binding.error().message << '\n';
            }
        }
        expect(bound_static + gated_static + census_static + refused_static == tables.land_static_objects.size(), "every stock scene type has a named audit result");
        std::cout << "stock static map=" << fixture.map << " bound=" << bound_static << " gated=" << gated_static
            << " census-only=" << census_static << " refused=" << refused_static
            << " bound-authored-offsets=" << bound_authored_offsets << " bound-model-fallback-offsets=" << bound_test_offsets
            << " scale-only-refused=" << scale_only_refused << " raw-passability-bits=explicit-unverified-input\n";
        const auto authored = take(skirmish::build_land_start(fixture, inputs, gates));
        if (players == 2) authored_open_ground_rear(authored);
        else compare_turning(authored, false);
        std::vector<std::string> selected_types;
        for (const auto& company : tables.land_companies) selected_types.push_back(company.id);
        const auto bindings = take(skirmish::build_land_bindings(tables, selected_types, gates));
        expect(authored.start.setup.content_identity == take(skirmish::build_land_start(fixture, inputs, bindings)).start.setup.content_identity,
            "stock L3 authored overload equals the explicit binding path");
        if (players == 2) {
            const std::vector<data::ValueOverride> overrides{{"Rebel", "Land_Skirmish_AI_Default_Forces", {}}};
            const auto omitted = take(data::with_overrides(loaded.catalog, overrides));
            const auto missing = take(skirmish::read_land_start_inputs(fixture, filesystem, omitted, tables));
            const auto empty_start = take(skirmish::build_land_start(fixture, missing, bindings));
            expect(std::any_of(empty_start.notes.begin(), empty_start.notes.end(), [](const auto& note) { return note.find("Faction Rebel") != std::string::npos; }),
                "missing mounted faction forces tag produces an explicit start note");
        }
        compare(fixture, inputs, bindings);
        std::cout << "installed land start players=" << players << " map=" << fixture.map << " authored locomotion; explicit unverified raw-bit/collision-order test gates\n";
    }
    naboo_turning_move(filesystem, loaded.catalog, cache);
    domain_lists(filesystem, loaded.catalog);
    setup_to_land_start(filesystem, loaded.catalog, cache, maps);
    return checks.finish();
}

void live_stock_move(const vfs::Vfs& filesystem, const data::Catalog& catalog, scene::VfsAssetCache& cache) {
    // Reproduce the L6 stock capture without a renderer or its input scheduler.
    skirmish::Fixture fixture;
    fixture.map = "data/art/maps/_mp_land_naboo.ted";
    fixture.slots = {{1, "Rebel", 0, true, {}}, {2, "Empire", 1, true, {}}};
    units::UnitTables empty;
    const auto discovered = take(skirmish::read_land_start_inputs(fixture, filesystem, catalog, empty));
    fixture.map_sha256 = discovered.common.map_sha256;
    units::LoadInput load;
    load.domain = BattleDomain::land; load.catalog = &catalog; load.filesystem = &filesystem;
    load.model = cache.access().model;
    std::set<std::string> requested;
    for (const auto& slot : fixture.slots) {
        const auto faction = std::find_if(discovered.factions.begin(), discovered.factions.end(),
            [&](const auto& row) { return row.faction == slot.faction; });
        if (faction != discovered.factions.end()) requested.insert(faction->forces.begin(), faction->forces.end());
    }
    load.types.assign(requested.begin(), requested.end());
    auto tables = take(units::load_unit_tables(load));
    for (const auto& company : tables.land_companies)
        for (const auto& member : company.members) requested.insert(member.type);
    load.types.assign(requested.begin(), requested.end());
    tables = take(units::load_unit_tables(load));
    const auto inputs = take(skirmish::read_land_start_inputs(fixture, filesystem, catalog, tables));
    auto gates = adapter_inputs(tables);
    gates.last_known_capacity = {{1, 50}, {2, 50}};
    const auto built = take(skirmish::build_land_start(fixture, inputs, gates));
    auto session = take(TacticalSession::create(built.start.setup, built.content));
    const auto before = session.land_movement()->checkpoint();
    const auto instances = session.snapshot()->instances();
    const LandMovingMember* selected_member = nullptr;
    for (const auto& member : before.members) {
        const auto owned = std::find_if(instances.begin(), instances.end(), [&](const auto& row) {
            return row.entity_id == member.object && row.owner == 1;
        });
        if (owned == instances.end()) continue;
        if (!selected_member || member.position.y < selected_member->position.y
            || (member.position.y == selected_member->position.y && member.object < selected_member->object))
            selected_member = &member;
    }
    if (!selected_member) throw std::runtime_error("stock live witness needs an authored Rebel member");
    const auto& parent = before.squads[selected_member->squad];
    const auto root = parent.object;
    // Restore the original turning witness now covered by the #4840 correction.
    Fixed destination_x;
    if (!sim::math::try_add(parent.position.x, whole(100), destination_x))
        throw std::runtime_error("stock live turning destination overflow");
    const auto destination = sim::math::Vec3{destination_x, parent.position.y, {}};
    const auto submitted = session.submit({{11, 1, 0}, {root}, MovePayload{destination}});
    if (!submitted) throw std::runtime_error(submitted.error().message);
    const auto stopped = session.submit({{111, 1, 1}, {root}, StopPayload{}});
    if (!stopped) throw std::runtime_error(stopped.error().message);
    const auto real = [](Fixed value) { return static_cast<double>(value.raw()) / Fixed::scale; };
    const auto trace = [&] {
        const auto* motion = session.land_movement();
        const auto squads = motion->squads();
        const auto found = std::find_if(squads.begin(), squads.end(), [&](const auto& row) { return row.object == root; });
        if (found == squads.end()) throw std::runtime_error("stock live reproduction lost its squad");
        const auto index = static_cast<std::size_t>(found - squads.begin());
        std::cout << "L6 headless tick=" << session.completed_tick() << " root=" << root
            << " position=" << real(found->position.x) << ',' << real(found->position.y)
            << " speed=" << real(found->speed) << " max=" << real(found->maximum_speed)
            << " order=" << static_cast<unsigned>(found->order) << " path=" << static_cast<unsigned>(found->path.status)
            << " nodes=" << found->path.nodes.size() << " next=" << found->next_node
            << " contacts=" << motion->contacts().size() << '\n';
        for (const auto& member : motion->members()) if (member.squad == index) {
            std::cout << "L6 member=" << member.object << " position=" << real(member.position.x) << ',' << real(member.position.y)
                << " state=" << static_cast<unsigned>(member.state) << " speed=" << real(member.speed)
                << " max=" << real(member.profile.maximum_speed) << " accel=" << real(member.profile.acceleration)
                << " walk=" << member.profile.walk_transition << " start_gate=" << member.gates.start_almost_finished
                << " animation_gate=" << member.gates.animation_prevents_movement << " mask=" << member.mask
                << " assigned=" << real(member.assigned_speed) << " collision=" << real(member.collision_speed)
                << " terrain_blocked=" << member.terrain_blocked << " lock=" << member.collision_lock << '\n';
        }
    };
    trace();
    const sim::InlineExecutor executor;
    for (int tick = 0; tick < 151; ++tick) {
        const auto result = take(session.step(executor));
        for (const auto& diagnostic : result.diagnostics) std::cout << "L6 diagnostic " << diagnostic.message << '\n';
        if (tick == 11 || tick == 49 || tick == 109 || tick == 149) trace();
    }
    bool moved = false;
    const auto squads = session.land_movement()->squads();
    const auto selected = std::find_if(squads.begin(), squads.end(), [&](const auto& row) { return row.object == root; });
    const auto index = static_cast<std::size_t>(selected - squads.begin());
    for (const auto& member : session.land_movement()->members()) if (member.squad == index) {
        const auto initial = std::find_if(before.members.begin(), before.members.end(),
            [&](const auto& row) { return row.object == member.object; });
        const double displacement_x = real(member.position.x) - real(initial->position.x);
        const double displacement_y = real(member.position.y) - real(initial->position.y);
        moved |= displacement_x * displacement_x + displacement_y * displacement_y > 100.0;
    }
    expect(moved, "stock Naboo authored squad moves after the same L6 move/stop command stream");
}

}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
        contracts(); return checks.finish();
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
