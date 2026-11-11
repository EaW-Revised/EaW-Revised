#include "eawr/skirmish/land_start.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/skirmish/land_content.hpp"
#include "eawr/core/text.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
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
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
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
            binding.members.push_back({skirmish::type_id(member.type), {whole(40), whole(120), whole(150), whole(2), whole(1), false}, 0, {}, {}, {}});
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
    if (diagnose) std::cout << "#4840 Dantooine movement witness follows the authored forward axis "
        "and requires more than ten units\n";
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
    content.land.static_objects.clear(); content.land.occupations.clear(); content.land.initial_pad_children.clear();
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
    // The stock gameconstants.xml values; a start never defaults them (land_fog_rules refuses zero).
    inputs.common.land_fog_cell_size = whole(15); inputs.common.land_fog_regrow_seconds = whole(6);
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
    {
        auto capture_tables = tables;
        units::UnitType active; active.id = "Influencing"; active.influences_capture = true;
        units::UnitType inactive; inactive.id = "NonInfluencing"; inactive.influences_capture = false;
        capture_tables.units = {active, inactive};
        auto capture_inputs = inputs; capture_inputs.common.tables = &capture_tables;
        const auto built = take(skirmish::build_land_start(fixture, capture_inputs, explicit_bindings(capture_tables)));
        const std::array<Player, 3> players{{{1, 1, 1, 0}, {2, 2, 2, 0}, {3, 3, 3, 0}}};
        const CaptureProfile point{900, whole(100), Fixed{}, {1, 2}};
        const std::array<CaptureCandidate, 2> candidates{{
            {{1, 1, {}}, skirmish::type_id(active.id)}, {{2, 2, {}}, skirmish::type_id(inactive.id)}}};
        PadState state{};
        expect(service_capture(point, state, 3, 3, players, candidates, built.content.economy.pads, 99, {}) == 1,
            "LC-53: a non-influencing enemy on the point does not contest the influencing unit");
        state = {};
        expect(service_capture(point, state, 3, 3, players, std::span{candidates}.last(1), built.content.economy.pads, 99, {}) == 3,
            "LC-53: a non-influencing unit alone cannot capture");
    }
    // LC-85/86: the start carries the land fog grid over the whole 48 x 48 sample terrain (960 units, 64 cells of 15,
    // row 0 at the top) and the sensor rows of the land types that reveal; the constants come from the start inputs.
    {
        const auto& grid = authored_start.content.fog;
        expect(grid && grid->cells_wide == 64 && grid->cells_tall == 64 && grid->cell_size == whole(15)
                && grid->map_left == Fixed{} && grid->map_top == whole(960) && grid->ramp_down_step == 21,
            "the land start binds a fog grid over the whole terrain from DesiredLandFOWCellSize and LandFOWRegrowTime");
        expect(authored_start.content.sensors.empty(), "land types without a reveal behaviour produce no sensor rows");
        auto coarse = inputs;
        coarse.common.land_fog_cell_size = whole(30); coarse.common.land_fog_regrow_seconds = whole(12);
        const auto coarse_start = take(skirmish::build_land_start(fixture, coarse, authored_inputs));
        const auto& coarse_grid = coarse_start.content.fog;
        expect(coarse_grid && coarse_grid->cells_wide == 32 && coarse_grid->cell_size == whole(30) && coarse_grid->ramp_down_step == 10,
            "the authored land fog constants decide the grid and the ramp");
        expect(coarse_start.start.identity.sha256 != authored_start.start.identity.sha256,
            "the fog constants are part of the land match identity");
        auto unread = inputs;
        unread.common.land_fog_cell_size = Fixed{};
        expect(!skirmish::build_land_start(fixture, unread, authored_inputs), "a land start without the fog constants is refused, not defaulted");
        auto lopsided = inputs;
        lopsided.map.terrain->height = 25; lopsided.map.terrain->samples.resize(48 * 25);
        lopsided.map.raw_passability.resize(48 * 25 * 4);
        const auto refused = skirmish::build_land_start(fixture, lopsided, authored_inputs);
        expect(!refused && refused.error().message.find("square") != std::string::npos,
            "a terrain whose axes get different cell sizes is refused with a named reason");
        auto sensor_tables = tables;
        units::UnitType revealer;
        revealer.id = "Mod_Aurora_Member"; revealer.kind = units::UnitKind::ship;
        revealer.reveal = true; revealer.fow_reveal_range = whole(130); revealer.multisample_fow = true;
        sensor_tables.units.push_back(revealer);
        auto sensor_inputs = inputs; sensor_inputs.common.tables = &sensor_tables;
        const auto sensed = take(skirmish::build_land_start(fixture, sensor_inputs, explicit_bindings(sensor_tables)));
        expect(sensed.content.sensors.size() == 1 && sensed.content.sensors[0].type_id == skirmish::type_id("Mod_Aurora_Member")
                && sensed.content.sensors[0].reveals && sensed.content.sensors[0].reveal_range == whole(130)
                && sensed.content.sensors[0].multisample && sensed.content.sensors[0].dense_multiplier == Fixed::from_raw(Fixed::scale / 2),
            "a land type's reveal inputs become its sensor row");
    }
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
    auto victory_inputs = static_inputs;
    victory_inputs.common.land_victory_condition = VictoryCondition::enemy_base_components_destroyed;
    auto& victory_prop = victory_inputs.static_tables.land_static_objects.front();
    victory_prop.behaviors = {"DUMMY_LAND_BASE_LEVEL_COMPONENT"};
    victory_prop.land_victory_relevant = true;
    const auto component_start = take(skirmish::build_land_start(fixture, victory_inputs, static_gates));
    expect(component_start.content.victory.starbase_types == std::vector<sim::tactical::TypeId>{skirmish::type_id(prop.id)},
        "LC-70 accepts the component behavior with land relevance");
    victory_prop.land_victory_relevant = false; victory_prop.victory_relevant = true;
    expect(!take(skirmish::build_land_start(fixture, victory_inputs, static_gates)).content.victory.starbase_types.empty(),
        "LC-70 accepts general relevance independently");
    victory_prop.victory_relevant = false;
    expect(take(skirmish::build_land_start(fixture, victory_inputs, static_gates)).content.victory.starbase_types.empty(),
        "LC-70 excludes an irrelevant tower despite its behavior");
    victory_prop.victory_relevant = true; victory_prop.behaviors.clear();
    expect(take(skirmish::build_land_start(fixture, victory_inputs, static_gates)).content.victory.starbase_types.empty(),
        "LC-70 excludes a relevant wall without component behavior");
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
    auto chosen_inputs = inputs;
    chosen_inputs.common.land_victory_condition = VictoryCondition::enemy_hq_destroyed;
    const auto chosen = take(skirmish::build_land_start(fixture, chosen_inputs, bindings));
    expect(chosen.start.victory_condition == VictoryCondition::enemy_hq_destroyed
        && chosen.content.victory.condition == VictoryCondition::enemy_hq_destroyed,
        "land start installs the authored condition");
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
    // One-switch vehicles-only start: the switch off leaves every start unchanged; on, human slots take the vehicle set.
    {
        const auto vehicle_company = [](std::string id) {
            units::LandCompany row; row.id = std::move(id); row.team_locomotor = "TEAM_LOCOMOTOR";
            units::LandCompanyMember tank; tank.type = row.id + "_Member"; tank.kind = units::LandObjectKind::vehicle;
            units::LandLocomotionData data; data.collision_class = units::LandCollisionGroup::vehicle;
            row.members = {tank}; row.locomotion = {data};
            return row;
        };
        const auto stock = skirmish::land_vehicle_start_forces("Rebel");
        expect(stock.size() == 2 && !skirmish::land_vehicle_start_forces("empire").empty()
            && !skirmish::land_vehicle_start_forces("Underworld").empty() && skirmish::land_vehicle_start_forces("Mod_Dawn").empty(),
            "stock factions have a vehicle set; a mod faction has none");
        expect(skirmish::is_land_vehicle_company(vehicle_company("Rebel_Light_Tank_Brigade"))
            && !skirmish::is_land_vehicle_company(company), "a generic-team vehicle company qualifies; an infantry container company does not");
        auto giant = vehicle_company("giant"); giant.locomotion[0].collision_class = units::LandCollisionGroup::giant_vehicle;
        auto flying = vehicle_company("flying"); flying.locomotion[0].collision_class = units::LandCollisionGroup::none;
        auto team = vehicle_company("team"); team.team_locomotor = "LAND_TEAM_CONTAINER_LOCOMOTOR";
        auto mixed = vehicle_company("mixed"); mixed.members.push_back(mixed.members[0]);
        mixed.locomotion.push_back(mixed.locomotion[0]); mixed.locomotion[1].collision_class = units::LandCollisionGroup::infantry;
        expect(skirmish::is_land_vehicle_company(giant) && !skirmish::is_land_vehicle_company(flying)
            && !skirmish::is_land_vehicle_company(team) && !skirmish::is_land_vehicle_company(mixed)
            && !skirmish::is_land_vehicle_company(units::LandCompany{}), "vehicle test needs the generic team and only vehicle collision classes");
        auto switched = inputs;
        skirmish::set_land_vehicles_only(switched, true);
        expect(switched.human_vehicles_only && switched.factions[0].vehicle_forces.empty(), "a mod faction gets no vehicle set");
        skirmish::set_land_vehicles_only(switched, false);
        expect(!switched.human_vehicles_only, "turning the switch off clears it");
        const auto plain = take(skirmish::build_land_start(fixture, inputs, bindings));
        auto off = inputs; off.factions[0].vehicle_forces = {"Mod_Aurora_Company"};
        const auto off_start = take(skirmish::build_land_start(fixture, off, bindings));
        expect(off_start.start.setup.units.size() == plain.start.setup.units.size(), "with the switch off the forces are unchanged");
        auto on = off; on.human_vehicles_only = true; on.factions[0].vehicle_forces = {"Mod_Aurora_Company"};
        expect(!skirmish::build_land_start(fixture, on, bindings), "an infantry company in the vehicle set is refused");
        auto fallback = inputs; fallback.human_vehicles_only = true;
        const auto fallback_start = take(skirmish::build_land_start(fixture, fallback, bindings));
        expect(fallback_start.start.setup.units.size() == plain.start.setup.units.size()
            && std::any_of(fallback_start.notes.begin(), fallback_start.notes.end(), [](const auto& note) { return note.find("vehicles-only") != std::string::npos; }),
            "a faction without a vehicle set keeps its defaults and says so");
        auto no_free = on; fixture.match = inputs.common.match_defaults; fixture.match->free_starting_units = false;
        expect(static_cast<bool>(skirmish::build_land_start(fixture, no_free, bindings)), "with free starting units off the vehicle set is not consulted");
        fixture.match.reset();
    }
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
    // #3207 G1-c (WLAI-56): a lobby AI's land difficulty reaches the durability scales and the economy account; humans and
    // Normal slots stay neutral. The fixture adjustments are synthetic; the multipliers are read from them.
    {
        std::map<std::string, std::string, std::less<>> xml;
        xml["data/xml/difficultyadjustments.xml"] = R"(<Difficulty_Adjustments>
<Difficulty_Adjustment Name="Mod_Hard"><Credit_Multiplier>1.25</Credit_Multiplier><Land_Build_Time_Multiplier>0.5</Land_Build_Time_Multiplier>
<Damage_Multiplier>1.5</Damage_Multiplier><Health_Multiplier>0.5</Health_Multiplier><Shield_Multiplier>0.25</Shield_Multiplier></Difficulty_Adjustment>
<Difficulty_Adjustment Name="Mod_Normal"><Credit_Multiplier>1.0</Credit_Multiplier></Difficulty_Adjustment>
</Difficulty_Adjustments>)";
        xml["data/xml/ai/players/dusk.xml"] = R"(<AIPlayerType><Name>Mod_Dusk_AI</Name><Difficulty_Adjustments>
<Normal>Mod_Normal</Normal><Hard>Mod_Hard</Hard></Difficulty_Adjustments></AIPlayerType>)";
        auto table = script::foc::AiDifficultyTable::load(xml);
        expect(static_cast<bool>(table), "G1-c: the fixture difficulty table loads");
        if (table) {
            auto ai_inputs = inputs;
            ai_inputs.common.difficulty = std::make_shared<const script::foc::AiDifficultyTable>(std::move(table).value());
            ai_inputs.common.factions[1].basic_ai = "Mod_Dusk_AI";
            const Fixed one = Fixed::from_raw(Fixed::scale);
            const auto ai_fixture = [&](const AiLevel level) {
                auto copy = fixture;
                copy.slots[1].human = false; copy.slots[1].difficulty = level;
                return copy;
            };
            const auto normal = take(skirmish::build_land_start(ai_fixture(AiLevel::normal), ai_inputs, bindings));
            expect(normal.content.durability.difficulty.empty() && normal.content.economy.players[1].credit_multiplier == one
                && normal.content.economy.players[1].ai_build_time_multiplier == one,
                "G1-c: a Normal land AI keeps neutral scales");
            const auto hard = take(skirmish::build_land_start(ai_fixture(AiLevel::hard), ai_inputs, bindings));
            const auto& scales = hard.content.durability.difficulty;
            expect(scales.size() == 1 && scales[0].player == 2 && scales[0].health == Fixed::from_ratio(1, 2).value()
                && scales[0].shield == Fixed::from_ratio(1, 4).value() && scales[0].damage == Fixed::from_ratio(3, 2).value(),
                "G1-c: a Hard land AI binds health, shield and damage scales for its seat only");
            expect(hard.content.economy.players[1].credit_multiplier == Fixed::from_ratio(5, 4).value()
                && hard.content.economy.players[1].ai_build_time_multiplier == Fixed::from_ratio(1, 2).value()
                && hard.content.economy.players[0].credit_multiplier == one && hard.content.economy.players[0].ai_build_time_multiplier == one,
                "G1-c: a Hard land AI carries credit and land build-time multipliers, the human stays neutral");
            const auto repeated = take(skirmish::build_land_start(ai_fixture(AiLevel::hard), ai_inputs, bindings));
            expect(repeated.start.identity.sha256 == hard.start.identity.sha256, "G1-c: the land difficulty binding is deterministic");
            auto no_table = ai_inputs; no_table.common.difficulty.reset();
            const auto plain = take(skirmish::build_land_start(ai_fixture(AiLevel::hard), no_table, bindings));
            expect(plain.content.durability.difficulty.empty() && plain.start.identity.sha256 != hard.start.identity.sha256,
                "G1-c: no mounted difficulty table leaves the land start neutral, and the applied scales bind into its identity");
        }
    }
    bindings.identity = {};
    expect(!skirmish::build_land_start(fixture, inputs, bindings), "effective content identity is required");
}

void live_stock_move(const vfs::Vfs&, const data::Catalog&, scene::VfsAssetCache&);
struct TurningResult {
    std::vector<std::uint8_t> state;
    Fixed parent_distance, member_progress;
    bool operator==(const TurningResult&) const = default;
};

// The authored Dantooine fixture names its member by identity. Members are numbered above the last roster root, so every
// reinforcement point and prebuilt child the map places moves the member's identity up by one.
sim::EntityId fixture_member(const skirmish::LandStart& built, const sim::EntityId authored) {
    const auto& points = built.content.land.point_objects;
    const auto shift = std::count_if(built.start.setup.units.begin(), built.start.setup.units.end(),
        [&](const auto& unit) { return std::binary_search(points.begin(), points.end(), unit.type_id); });
    return authored + static_cast<sim::EntityId>(shift + built.content.land.initial_pad_children.size());
}

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
        const auto member = std::find_if(members.begin(), members.end(), [&](const auto& m) { return m.object == fixture_member(built, 1103); });
        if (member == members.end()) throw std::runtime_error("Dantooine expected-limit fixture has no member 1103 (shifted by its points)");
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
        const auto member = std::find_if(members.begin(), members.end(), [&](const auto& m) { return m.object == fixture_member(built, 1103); });
        const bool member_measured = member != members.end() && sim::math::try_length(
            {Fixed::from_raw(member->position.x.raw() - member_before.x.raw()),
             Fixed::from_raw(member->position.y.raw() - member_before.y.raw())}, member_progress);
        expect(member_measured, "#4840 Dantooine member progress is measurable");
        // Strict pass: the squad turns in place, then completes the 90-degree move (same bar as Naboo).
        expect(measured && distance > whole(10), "#4840 Dantooine parent moves more than ten units within 150 ticks");
        expect(member_measured && member_progress > whole(10), "#4840 Dantooine member 1103 moves more than ten units within 150 ticks");
        const auto index = static_cast<std::size_t>(final - final_squads.begin());
        for (const auto& m : members) if (m.squad == index)
            expect(m.collision_lock == 0 && !m.terrain_blocked, "#4840 Dantooine company has no collision lock or terrain block");
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
    const auto baseline = turning_move(built, serial, naboo, false, true, true);
    if (!naboo) {
        const auto real = [](Fixed n) { return static_cast<double>(n.raw()) / Fixed::scale; };
        std::cout << "strict #4840 Dantooine: member1103 progress=" << real(baseline.member_progress)
            << " parent=" << real(baseline.parent_distance) << '\n';
    }
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        const auto result = turning_move(built, executor, naboo, true, false, false);
        expect(result == baseline, "stock turning contact equals at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reversed(workers);
        const auto reverse = turning_move(built, reversed, naboo, true, false, false);
        expect(reverse == baseline, "stock turning contact equals with reverse partitions");
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
    expect(domain != built.start.identity.domains.end() && domain->version == 5,
        "the land-content identity domain is version 5 with active combat, the land fog grid and the static sensor rows");
}

// LC-44..53 (#5894): the reinforcement points the stock land maps author reach the start through the loader, as data.
void reinforcement_points(const vfs::Vfs& filesystem, const data::Catalog& catalog, scene::VfsAssetCache& cache,
    const std::vector<skirmish::SetupMap>& maps) {
    std::size_t maps_with_points{}, chosen_count{};
    const skirmish::SetupMap* chosen = nullptr;
    for (const auto& map : maps) {
        if (!map.unavailable.empty() || map.teams.size() < 2) continue;
        skirmish::Fixture fixture; fixture.map = map.path;
        fixture.slots = {{1, "Rebel", map.teams[0].team, true, {}}, {2, "Empire", map.teams[1].team, true, {}}};
        units::UnitTables empty;
        const auto discovered = skirmish::read_land_start_inputs(fixture, filesystem, catalog, empty);
        if (!discovered) continue;
        std::size_t authored{};
        for (const auto& placement : discovered.value().common.placements) {
            // The placement flag comes from the type's own Behavior list; stock points are Marker objects.
            if (!placement.reinforcement_point) continue;
            const auto& rows = discovered.value().static_tables.land_static_objects;
            const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& value) { return core::iequals(value.id, placement.type); });
            expect(row != rows.end() && row->reinforcement_point, "every placed reinforcement point has a loaded static row");
            ++authored;
        }
        if (authored == 0) continue;
        ++maps_with_points;
        if (chosen == nullptr) { chosen = &map; chosen_count = authored; }
    }
    std::cout << "stock land maps with authored reinforcement points: " << maps_with_points << "\n";
    expect(maps_with_points > 0, "the stock land maps author reinforcement points (placed objects with the REINFORCEMENT_POINT behaviour)");
    if (chosen == nullptr) return;
    skirmish::Fixture fixture; fixture.map = chosen->path;
    fixture.slots = {{1, "Rebel", chosen->teams[0].team, true, {}}, {2, "Empire", chosen->teams[1].team, true, {}}};
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
    const auto& land = built.content.land;
    const auto is_point = [&](TypeId type) { return std::binary_search(land.point_objects.begin(), land.point_objects.end(), type); };
    const auto placed = static_cast<std::size_t>(std::count_if(built.start.setup.units.begin(), built.start.setup.units.end(),
        [&](const auto& unit) { return is_point(unit.type_id); }));
    expect(placed >= chosen_count, "every reinforcement point the map places is a unit of the land start");
    expect(land.reinforcement.enabled && land.reinforcement.multiplayer && land.reinforcement.caps.size() == 2
            && !land.reinforcement.points.empty() && !land.reinforcement.capacities.empty(),
        "the land start carries the enabling radii, the capacities and one company cap per seat");
    expect(std::all_of(land.reinforcement.points.begin(), land.reinforcement.points.end(), [](const auto& row) { return row.enabling_radius.raw() > 0; }),
        "every enabling row names a positive authored radius");
    expect(built.content.economy.pads.neutral != 0
            && std::any_of(built.content.economy.pads.capture.begin(), built.content.economy.pads.capture.end(), [&](const auto& row) {
                   return is_point(row.type) && row.captures; })
            && std::all_of(built.content.economy.pads.capture.begin(), built.content.economy.pads.capture.end(), [&](const auto& row) {
                   return !row.captures || (is_point(row.type) && row.radius.raw() > 0 && row.transition_seconds.raw() > 0 && !row.affiliation.empty()); }),
        "the capturable points carry their authored capture profile against the declared neutral player");
    // The session runs: the pass over the points and the roster is equal at 1 and at 8 workers.
    const sim::InlineExecutor serial;
    auto reference = take(TacticalSession::create(built.start.setup, built.content));
    for (int i = 0; i < 90; ++i) take(reference.step(serial));
    platform::ThreadWorkerAdapter threads(8);
    auto parallel = take(TacticalSession::create(built.start.setup, built.content));
    for (int i = 0; i < 90; ++i) take(parallel.step(threads));
    expect(parallel.canonical_state_bytes() == reference.canonical_state_bytes() && !reference.pads().empty(),
        "a stock land battle with reinforcement points steps to one state at 1 and 8 workers and registers the points as pads");
    std::cout << "stock land reinforcement points: map=" << chosen->path << " authored=" << chosen_count << " placed=" << placed << "\n";
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

// LC-85..87 on a stock map: the start's fog grid fills the terrain, the stock land types supply sensor rows, and a
// session built from it marks the spawned companies' circles in their owners' grids without fogging everything out.
std::size_t stock_sampling_structures{}; // multisampling structure types met over all stock maps (LC-89, #6183)
void stock_fog(const skirmish::LandStart& built, const skirmish::LandStartInputs& inputs, const std::uint32_t players) {
    expect(inputs.common.land_fog_cell_size == whole(15) && inputs.common.land_fog_regrow_seconds == whole(6),
        "the stock land constants are DesiredLandFOWCellSize 15 and LandFOWRegrowTime 6");
    const auto& terrain = *inputs.map.terrain;
    const auto cells = [&](const std::uint32_t samples) {
        const std::uint64_t cell = static_cast<std::uint64_t>(inputs.common.land_fog_cell_size.raw() / Fixed::scale);
        return std::min<std::uint64_t>((std::uint64_t{samples} * 20 + cell - 1) / cell, 512);
    };
    const auto& grid = built.content.fog;
    expect(grid && grid->cells_wide == cells(terrain.width) && grid->cells_tall == cells(terrain.height)
            && grid->cell_size.raw() <= whole(15).raw() && grid->map_top == whole(static_cast<std::int64_t>(terrain.height) * 20),
        "the stock land start binds a fog grid over the whole terrain");
    expect(std::any_of(built.content.sensors.begin(), built.content.sensors.end(), [](const auto& row) { return row.reveals; }),
        "stock land types supply reveal sensor rows");
    auto session = take(TacticalSession::create(built.start.setup, built.content));
    const auto* fog = session.fog_cells();
    expect(fog != nullptr, "a stock land session has fog cells");
    if (!fog) return;
    std::size_t players_with_cells = 0;
    const auto live = session.units();
    for (std::size_t index = 0; index < built.start.setup.players.size(); ++index) {
        const auto values = fog->values(index);
        const auto revealed = static_cast<std::size_t>(std::count_if(values.begin(), values.end(), [](const auto v) { return v != 0; }));
        if (revealed > 0) ++players_with_cells;
        // A team reveals what its revealers' circles cover and nothing else. Every live object of the team with a reveal row
        // contributes at most the bounding square of its circle (range floored at 10, radius in cells = range / cell + 0.5).
        // This replaces an arbitrary "under half the map" bound: stock structures (range 300, 22 or more per team) reveal
        // far more than the companies alone, as authored.
        std::uint64_t cover = 0;
        const auto& self = built.start.setup.players[index];
        for (const auto& unit : live) {
            const auto owner = std::find_if(built.start.setup.players.begin(), built.start.setup.players.end(),
                [&](const auto& player) { return player.player_id == unit.owner; });
            const auto sensor = std::find_if(built.content.sensors.begin(), built.content.sensors.end(),
                [&](const auto& row) { return row.type_id == unit.type_id && row.reveals; });
            if (owner == built.start.setup.players.end() || owner->team_id != self.team_id || sensor == built.content.sensors.end()) continue;
            const auto range = std::max(sensor->reveal_range.raw(), whole(10).raw());
            const auto radius = static_cast<std::uint64_t>((range * 2 + grid->cell_size.raw()) / (2 * grid->cell_size.raw()));
            cover += (2 * radius + 1) * (2 * radius + 1);
        }
        std::cout << "stock land fog player=" << self.player_id << " revealed=" << revealed << " of " << values.size()
            << " cover_bound=" << cover << "\n";
        // The neutral player may see everything: Dantooine places Secondary_Abandoned_Sensor (Land_FOW_Reveal_Range 10000). The
        // bound is for the players that play.
        if (self.commandable()) expect(revealed < values.size() / 2, "a stock land player starts with less than half the map revealed");
        expect(static_cast<std::uint64_t>(revealed) <= cover, "a stock land player never reveals more than its team's reveal circles can cover");
    }
    expect(players_with_cells >= players, "every commandable stock land player starts with a revealed circle");
    // LC-86 (#6183): a placed or pre-built structure of the session that reveals shows its own cell to its owner at tick 0.
    std::size_t revealing{}, shown{};
    const auto& statics = built.content.land.static_objects;
    for (const auto& unit : built.start.setup.units) {
        if (std::find(statics.begin(), statics.end(), unit.type_id) == statics.end()) continue;
        const auto sensor = std::find_if(built.content.sensors.begin(), built.content.sensors.end(),
            [&](const auto& row) { return row.type_id == unit.type_id && row.reveals; });
        const auto owner = std::find_if(built.start.setup.players.begin(), built.start.setup.players.end(),
            [&](const auto& player) { return player.player_id == unit.owner; });
        if (sensor == built.content.sensors.end() || owner == built.start.setup.players.end()) continue;
        ++revealing;
        if (fog->revealed(static_cast<std::size_t>(owner - built.start.setup.players.begin()), unit.position)) ++shown;
    }
    std::cout << "stock land structures that reveal: " << revealing << " shown to their owner at tick 0: " << shown << "\n";
    expect(revealing > 0, "a stock land start places structures of the session that carry a REVEAL sensor row");
    expect(shown == revealing, "every revealing stock structure shows its own cell to its owner at tick 0");
    // LC-89 (#6183): a multisampling stock structure authors no hard extents, so its fog box is its model's collision box.
    std::size_t sampling{}, boxed{};
    for (const auto type : statics) {
        const auto sensor = std::find_if(built.content.sensors.begin(), built.content.sensors.end(),
            [&](const auto& row) { return row.type_id == type && row.multisample; });
        if (sensor == built.content.sensors.end()) continue;
        ++sampling;
        if (sensor->half_extents.x > Fixed{} && sensor->half_extents.y > Fixed{}) ++boxed;
    }
    std::cout << "stock land multisampling structure types: " << sampling << " with a fog box: " << boxed << "\n";
    expect(boxed == sampling, "every multisampling stock structure of the session has a non-empty fog box");
    stock_sampling_structures += sampling; // not every map places one (Alderaan places none); the maps together must
    std::cout << "stock land fog " << grid->cells_wide << 'x' << grid->cells_tall << " cell_raw=" << grid->cell_size.raw() << '\n';
}

// LE-LOADER (installed data): the map's build pads, their menus and their prices come from the data; a pad without CAPTURE_POINT
// is an inert pad profile; the land economy rows win (LC-46); and a build on a base pad runs end to end: the pad is revealed to its
// owner (#6183), the request is accepted, the price is debited, the child grows and the constructed object replaces it.
void stock_pads(const skirmish::LandStart& built, const skirmish::LandStartInputs& inputs, const skirmish::Fixture& fixture,
    const skirmish::LandContentInputs& gates, const units::UnitTables* capture_pad) {
    const auto& economy = built.content.economy;
    const auto& pad_tables = inputs.pad_tables;
    expect(!pad_tables.units.empty() && !economy.pads.capture.empty(), "a stock land map loads its build pads and their pad rules");
    std::size_t pads{}, inert{}, menus{}, options{};
    for (const auto& pad : pad_tables.units) {
        if (!pad.build_pad) continue;
        ++pads;
        const auto* profile = economy.pads.point(skirmish::type_id(pad.id));
        // No land pad captures until the land capture service (LE-CAPTURE): a CAPTURE_POINT pad keeps its capture inputs but is inert too.
        expect(profile != nullptr && profile->build_pad && !profile->captures,
            "every stock pad has an inert pad profile (no land pad captures yet)");
        if (profile && !profile->captures) {
            ++inert;
            std::vector<FactionId> expected_affiliation;
            if (pad.capture_point) {
                auto names = pad.affiliation;
                std::replace(names.begin(), names.end(), ',', ' ');
                std::istringstream tokens(names);
                std::string name;
                while (tokens >> name) for (const auto& faction : inputs.common.factions)
                    if (core::iequals(name, faction.name)) expected_affiliation.push_back(battle::faction_id(name));
                std::sort(expected_affiliation.begin(), expected_affiliation.end());
            }
            expect(profile->radius == pad.capture_radius.value_or(Fixed{}) && profile->affiliation == expected_affiliation,
                "a stock pad carries its type radius and matching capture-point affiliations");
        }
        for (const auto& group : pad.production.buildable) {
            const auto menu = std::find_if(economy.menus.begin(), economy.menus.end(), [&](const auto& row) {
                return row.station == skirmish::type_id(pad.id) && row.faction == battle::faction_id(group.faction); });
            expect(menu != economy.menus.end() && !menu->station_producer && menu->options.size() == group.types.size(),
                "a stock pad has one build menu per authored faction group with the authored entries");
            if (menu == economy.menus.end()) continue;
            ++menus;
            for (std::size_t index = 0; index < group.types.size() && index < menu->options.size(); ++index) {
                const auto* child = pad_tables.find(group.types[index]);
                const auto& option = menu->options[index];
                if (!child) continue;
                ++options;
                const auto seconds = child->production.build_time_seconds ? child->production.build_time_seconds->trunc_to_integer() : 0;
                expect(option.type == skirmish::type_id(group.types[index]) && option.kind == BuildKind::structure
                        && option.price == child->production.build_cost_multiplayer.value_or(Fixed{})
                        && option.build_frames == static_cast<std::uint32_t>(seconds) * 30,
                    "a stock build option takes its price and time from the construction child's data");
            }
        }
    }
    std::cout << "stock pads map=" << fixture.map << " pads=" << pads << " inert=" << inert << " menus=" << menus
        << " options=" << options << '\n';
    expect(pads > 0 && inert > 0 && menus > 0 && options > 0, "the stock map has inert pads with menus and priced options");
    // Census of what the map places: every distinct start type with its count, whether it is a pad, and how many reached the session.
    {
        std::map<std::string, std::array<std::size_t, 3>> census; // placed, in session, pad
        for (const auto& unit : built.start.units) {
            auto& row = census[unit.type];
            ++row[0];
            row[2] = pad_tables.find(unit.type) && pad_tables.find(unit.type)->build_pad ? 1 : 0;
            if (std::any_of(built.start.setup.units.begin(), built.start.setup.units.end(),
                    [&](const auto& state) { return state.entity_id == unit.state.entity_id; })) ++row[1];
        }
        for (const auto& [type, row] : census)
            std::cout << "stock census map=" << fixture.map << " type=" << type << " placed=" << row[0] << " session=" << row[1]
                << (row[2] ? " pad" : "") << '\n';
    }
    // A map that places a capture-point pad (Skirmish_Build_Pad is one) still starts: no land pad captures yet, so no neutral player is needed.
    if (capture_pad != nullptr) {
        auto with_capture = inputs;
        skirmish::merge_land_pad_types(with_capture.pad_tables, *capture_pad);
        const auto started = take(skirmish::build_land_start(fixture, with_capture, gates));
        const auto* profile = started.content.economy.pads.point(skirmish::type_id("Skirmish_Build_Pad"));
        expect(profile != nullptr && profile->build_pad && !profile->captures,
            "a land start whose pads include the capture-point Skirmish_Build_Pad loads and the pad never captures");
    }
    // LC-46: the land rows keep the start credits and the land cap (the builder's own rows are not used).
    for (const auto& player : economy.players)
        expect(player.credits == built.start.match.credits && player.start_tech == 1,
            "the land economy rows keep the lobby credits and the land start tech");
    expect(built.start.match.pre_built_base, "the stock pad case enables prebuilt bases");
    if (built.start.match.pre_built_base) {
        expect(!built.content.land.initial_pad_children.empty(), "prebuilt stock structure pads have a starting child");
        auto prebuilt = take(TacticalSession::create(built.start.setup, built.content));
        expect(!prebuilt.outcome(), "the stock prebuilt start has no immediate victory outcome");
        for (const auto& ledger : prebuilt.ledgers())
            expect(ledger.credits == built.start.match.credits, "prebuilt children do not debit their owners");
        bool income_child = false;
        for (const auto& [parent, child] : built.content.land.initial_pad_children) {
            const auto state = prebuilt.pads().find(parent);
            const auto unit = prebuilt.unit_state(child);
            const auto parent_unit = prebuilt.unit_state(parent);
            expect(state != prebuilt.pads().end() && state->second.constructed == child && unit,
                "a starting child is linked to its pad");
            expect(unit && parent_unit && unit->owner == parent_unit->owner,
                "a starting child belongs to its pad owner");
            if (unit && unit->owner == 1 && economy.stream(unit->type_id)) income_child = true;
        }
        expect(income_child, "a prebuilt stock child supplies an authored income stream");
        const auto initial = prebuilt.ledgers().front().credits;
        std::int64_t own_rate{};
        for (const auto& unit : built.start.setup.units) {
            const auto* stream = economy.stream(unit.type_id);
            if (unit.owner == 1 && stream && stream->automatic && stream->initially_enabled)
                own_rate += stream->per_frame.raw();
        }
        const sim::InlineExecutor income_executor;
        for (int tick = 0; tick < 30; ++tick) take(prebuilt.step(income_executor));
        expect(prebuilt.ledgers().front().credits > initial, "the stock prebuilt source pays income");
        if (fixture.slots.size() == 2)
            expect(own_rate > 0 && prebuilt.ledgers().front().credits.raw() - initial.raw() == own_rate * 30,
                "stock income over 30 frames equals its authored per-frame stream rates");
        expect(!prebuilt.outcome(), "the idle stock prebuilt start retains an undecided victory outcome");
        auto without = fixture;
        without.match = built.start.match;
        without.match->pre_built_base = false;
        const auto off = take(skirmish::build_land_start(without, inputs, gates));
        expect(off.content.land.initial_pad_children.empty(), "prebuilt off leaves starting children absent");
        if (fixture.slots.size() == 2) {
            auto hq_fixture = fixture;
            hq_fixture.victory_condition = VictoryCondition::enemy_hq_destroyed;
            const auto hq_start = take(skirmish::build_land_start(hq_fixture, inputs, gates));
            auto setup = hq_start.start.setup;
            auto rules = hq_start.content;
            rules.fog.reset(); // Isolate the HQ victory rule from stock visibility and sensor ranges.
            std::array<sim::EntityId, 2> children{}, parents{};
            for (const auto& [parent, child] : rules.land.initial_pad_children) {
                const auto unit = std::find_if(setup.units.begin(), setup.units.end(),
                    [&](const auto& row) { return row.entity_id == child; });
                if (unit != setup.units.end() && unit->owner >= 1 && unit->owner <= 2
                    && std::binary_search(rules.victory.starbase_types.begin(), rules.victory.starbase_types.end(), unit->type_id)) {
                    children[unit->owner - 1] = child;
                    parents[unit->owner - 1] = parent;
                }
            }
            expect(children[0] != 0 && children[1] != 0, "both players have a prebuilt HQ victory child");
            if (children[0] && children[1]) {
                const auto enemy = std::find_if(setup.units.begin(), setup.units.end(),
                    [&](const auto& row) { return row.entity_id == children[1]; });
                const auto enemy_type = enemy->type_id;
                expect(std::none_of(off.start.setup.units.begin(), off.start.setup.units.end(), [&](const auto& row) {
                    return row.owner == 2 && row.type_id == enemy_type;
                }), "prebuilt off has no enemy HQ child");
                std::erase_if(setup.units, [&](const auto& row) {
                    return row.entity_id != children[0] && row.entity_id != children[1]
                        && row.entity_id != parents[0] && row.entity_id != parents[1];
                });
                std::erase_if(rules.land.initial_pad_children, [&](const auto& link) {
                    return link.second != children[0] && link.second != children[1];
                });
                std::erase_if(rules.land.occupations, [&](const auto& row) {
                    return row.object != children[0] && row.object != children[1]
                        && row.object != parents[0] && row.object != parents[1];
                });
                auto attacker = std::find_if(setup.units.begin(), setup.units.end(),
                    [&](const auto& row) { return row.entity_id == children[0]; });
                const auto target = std::find_if(setup.units.begin(), setup.units.end(),
                    [&](const auto& row) { return row.entity_id == children[1]; });
                attacker->position = target->position;
                attacker->position.x = Fixed::from_raw(target->position.x.raw() + 50 * Fixed::scale);
                rules.durability = {};
                DamageRules damage;
                damage.shield_recharge_frames = 30; damage.damage_types = 1; damage.armor_types = 1;
                damage.armor_mods = {whole(1)}; rules.durability.damage = damage;
                for (const auto type : {attacker->type_id, target->type_id}) {
                    if (std::none_of(rules.durability.profiles.begin(), rules.durability.profiles.end(),
                            [&](const auto& row) { return row.type_id == type; })) {
                        DurabilityProfile health; health.type_id = type; health.max_hull = whole(4);
                        health.allow_diminishing_firepower = false; rules.durability.profiles.push_back(health);
                    }
                }
                std::sort(rules.durability.profiles.begin(), rules.durability.profiles.end(),
                    [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
                rules.combat = {};
                CombatProfile gun;
                gun.type_id = attacker->type_id; gun.max_attack_distance = whole(150); gun.living_projectile_collision = true;
                gun.collision = CollisionBox{{whole(-2), whole(-2), whole(-2)}, {whole(2), whole(2), whole(4)}};
                WeaponProfile weapon;
                weapon.range = whole(150); weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 100;
                weapon.pulse_count = 1;
                ShotProfile shot; shot.damage = whole(4); shot.speed = whole(4); shot.max_travel = whole(500);
                weapon.shot = shot; gun.weapons.push_back(weapon); rules.combat.profiles.push_back(gun);
                if (target->type_id != attacker->type_id) {
                    auto body = gun; body.type_id = target->type_id; body.weapons.clear();
                    rules.combat.profiles.push_back(body);
                }
                std::sort(rules.combat.profiles.begin(), rules.combat.profiles.end(),
                    [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
                for (const auto& profile : rules.combat.profiles)
                    rules.combat.footprints.push_back({profile.type_id, whole(2), whole(2), whole(2)});
                auto battle = take(TacticalSession::create(setup, rules));
                expect(!battle.outcome(), "the prebuilt HQ start has no immediate winner");
                expect(static_cast<bool>(battle.submit({{0, 1, 1}, {children[0]}, AttackPayload{children[1]}})),
                    "the attack on the enemy HQ is submitted");
                bool destroyed = false;
                const sim::InlineExecutor executor;
                for (int frame = 0; frame < 180 && !battle.outcome(); ++frame) {
                    const auto tick = take(battle.step(executor));
                    for (const auto& event : tick.snapshot->events())
                        destroyed |= event.kind == EventKind::unit_destroyed && event.unit == children[1];
                }
                expect(destroyed && battle.outcome() && battle.outcome()->winner == 1
                        && battle.outcome()->deciding_unit == children[1],
                    "destroying the prebuilt enemy HQ decides the HQ victory for its opponent");
            }
        }
        std::size_t retained{}, removed{};
        for (const auto& unit : built.start.units) {
            const auto* type = inputs.pad_tables.find(unit.type);
            if (!type || !type->build_pad || std::none_of(built.start.markers.begin(), built.start.markers.end(),
                    [&](const auto& marker) { return marker.record == unit.record && marker.use == battle::MarkerUse::base_position; })) continue;
            const bool exists = std::any_of(off.start.units.begin(), off.start.units.end(), [&](const auto& other) {
                return other.record == unit.record && other.state.owner == unit.state.owner && other.type == unit.type; });
            if (!type->respects_prebuilt_base) {
                expect(exists, "prebuilt off retains every opt-out marker pad");
                if (exists) ++retained;
            } else {
                expect(!exists, "prebuilt off omits each prebuilt structure marker pad");
                if (!exists) ++removed;
            }
        }
        expect(retained > 0 && removed > 0, "the stock map exercises both prebuilt marker-pad cases");
        if (fixture.slots.size() == 2 && !built.content.land.initial_pad_children.empty()) {
            const auto parent_id = built.content.land.initial_pad_children.front().first;
            const auto parent = std::find_if(built.start.units.begin(), built.start.units.end(),
                [&](const auto& unit) { return unit.state.entity_id == parent_id; });
            auto multi = inputs;
            if (parent != built.start.units.end()) {
                const auto pad = std::find_if(multi.pad_tables.units.begin(), multi.pad_tables.units.end(),
                    [&](const auto& unit) { return core::iequals(unit.id, parent->type); });
                if (pad != multi.pad_tables.units.end()) {
                    const auto group = std::find_if(pad->production.buildable.begin(), pad->production.buildable.end(),
                        [&](const auto& row) { return core::iequals(row.faction, fixture.slots[parent->state.owner - 1].faction); });
                    if (group != pad->production.buildable.end() && group->types.size() == 1) {
                        const auto second = std::find_if(multi.pad_tables.units.begin(), multi.pad_tables.units.end(),
                            [&](const auto& unit) { return unit.under_construction && !core::iequals(unit.id, group->types.front()); });
                        expect(second != multi.pad_tables.units.end(), "the stock fixture supplies a second construction type");
                        if (second != multi.pad_tables.units.end()) {
                            group->types.push_back(second->id);
                            const auto multiple = take(skirmish::build_land_start(fixture, multi, gates));
                            expect(std::none_of(multiple.content.land.initial_pad_children.begin(), multiple.content.land.initial_pad_children.end(),
                                    [&](const auto& entry) { return entry.first == parent_id; }),
                                "a multi-entry pad does not assume a starting selection");
                        }
                    } else expect(false, "the selected stock starting pad has one faction entry");
                } else expect(false, "the selected stock starting pad has a loaded type");
            } else expect(false, "the selected stock starting pad exists in the start census");
        }
    }
    // End to end: player 1 builds on one of its own pads, the available option with the shortest build time.
    const PlayerId owner = 1;
    const auto faction = battle::faction_id(fixture.slots.front().faction);
    sim::EntityId pad_entity{};
    const BuildOption* option = nullptr;
    const StationMenu* own_menu = nullptr;
    for (const auto& unit : built.start.setup.units) {
        if (unit.owner != owner) continue;
        if (std::any_of(built.content.land.initial_pad_children.begin(), built.content.land.initial_pad_children.end(),
                [&](const auto& link) { return link.first == unit.entity_id; })) continue;
        for (const auto& menu : economy.menus) {
            if (menu.station != unit.type_id || menu.faction != faction) continue;
            for (const auto& candidate : menu.options)
                if (candidate.kind == BuildKind::structure && candidate.available && candidate.price <= built.start.match.credits
                    && std::any_of(economy.pads.construction.begin(), economy.pads.construction.end(), [&](const auto& child) {
                        return child.type == candidate.type && economy.stream(child.constructed) == nullptr; })
                    && (option == nullptr || candidate.build_frames < option->build_frames)) {
                    option = &candidate; own_menu = &menu; pad_entity = unit.entity_id;
                }
        }
        if (option) break;
    }
    expect(option != nullptr, "player 1 starts with a build pad that offers an affordable structure");
    if (!option) return;
    auto session = take(TacticalSession::create(built.start.setup, built.content));
    auto control = take(TacticalSession::create(built.start.setup, built.content));
    const sim::InlineExecutor serial;
    const auto allowed = session.pad_build_allowed(owner, pad_entity);
    expect(allowed && allowed.value(), "a stock base pad is revealed to its owner at tick 0, so a build on it is allowed");
    const auto credits = [&]() {
        for (const auto& row : session.ledgers()) if (row.player == owner) return row.credits;
        return Fixed{};
    };
    const auto price = option->price;
    const auto frames = option->build_frames;
    // A type that is not on the pad's menu for the builder's faction is refused: nothing grows, nothing is debited.
    const BuildOption* foreign = nullptr;
    for (const auto& menu : economy.menus)
        for (const auto& candidate : menu.options)
            if (foreign == nullptr && menu.faction != faction && candidate.kind == BuildKind::structure && own_menu->find(candidate.type) == nullptr)
                foreign = &candidate;
    expect(foreign != nullptr, "another faction's pad menu offers a structure that player 1's menu does not");
    if (foreign) {
        const auto refused = session.submit({{0, owner, 1}, {pad_entity}, PadBuildPayload{foreign->type}});
        expect(static_cast<bool>(refused), "the command of an unlisted structure is queued for admission");
        take(session.step(serial));
        take(control.step(serial));
        const auto idle = session.pads().find(pad_entity);
        const auto unchanged = std::find_if(control.ledgers().begin(), control.ledgers().end(),
            [&](const auto& row) { return row.player == owner; });
        expect(idle != session.pads().end() && idle->second.under_construction == 0 && idle->second.constructed == 0
                && unchanged != control.ledgers().end() && credits() == unchanged->credits,
            "a structure that is not on the pad's menu for the builder's faction is refused: nothing is built or debited");
    }
    const auto submitted = session.submit({{session.completed_tick(), owner, 2}, {pad_entity}, PadBuildPayload{option->type}});
    expect(static_cast<bool>(submitted), "the pad build request is accepted");
    if (!submitted) return;
    sim::EntityId child{};
    std::uint64_t grown_at{};
    std::optional<TacticalSession> restored_session;
    for (std::uint64_t tick = 0; tick < frames + 150; ++tick) {
        take(session.step(serial));
        take(control.step(serial));
        if (restored_session) take(restored_session->step(serial));
        const auto state = session.pads().find(pad_entity);
        if (state == session.pads().end()) break;
        if (!restored_session && state->second.under_construction != 0 && tick >= frames / 2) {
            const auto saved = take(checkpoint::save_tactical_checkpoint(session, session.pending_commands(),
                checkpoint::default_seats(session.record().setup), checkpoint::tactical_identities(built.start.setup.content_identity)));
            auto restored = take(checkpoint::load_tactical_checkpoint(saved, checkpoint::tactical_identities(built.start.setup.content_identity),
                [&](const TacticalReplay& replay) { return TacticalSession::from_replay(replay, built.content); }, serial));
            expect(restored.session.canonical_state_bytes() == session.canonical_state_bytes(),
                "a stock loaded pad restores during construction");
            restored_session.emplace(std::move(restored.session));
        }
        if (state->second.constructed != 0) { child = state->second.constructed; grown_at = tick; break; }
    }
    std::cout << "stock pad build: price=" << price.raw() / Fixed::scale << " frames=" << frames << " done at tick " << grown_at << '\n';
    const auto control_ledger = std::find_if(control.ledgers().begin(), control.ledgers().end(),
        [&](const auto& row) { return row.player == owner; });
    expect(control_ledger != control.ledgers().end() && control_ledger->credits.raw() - credits().raw() == price.raw(),
        "the price of the pad build is debited once alongside ordinary income");
    expect(child != 0 && session.unit_state(child) && session.unit_state(child)->owner == owner,
        "the constructed object replaces the construction child, owned by the builder");
    expect(grown_at + 1 >= frames && grown_at <= frames + 90, "the construction takes the authored time");
    expect(restored_session.has_value(), "the stock pad was saved while construction was pending");
    if (restored_session)
        expect(restored_session->canonical_state_bytes() == session.canonical_state_bytes(),
            "a stock pad restored mid-construction completes identically");
    std::vector<std::uint8_t> pad_baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto parallel = take(TacticalSession::create(built.start.setup, built.content));
        platform::ThreadWorkerAdapter executor(workers);
        expect(static_cast<bool>(parallel.submit({{0, owner, 1}, {pad_entity}, PadBuildPayload{option->type}})),
            "the stock pad digest case submits its build");
        for (int tick = 0; tick < 40; ++tick) take(parallel.step(executor));
        const auto pad = parallel.pads().find(pad_entity);
        expect(pad != parallel.pads().end() && pad->second.under_construction != 0,
            "the stock pad digest case exercises active construction");
        const auto state = parallel.canonical_state_bytes();
        if (pad_baseline.empty()) pad_baseline = state;
        else expect(state == pad_baseline, "stock pad construction agrees on 1/2/4/8 workers");
    }
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
    reinforcement_points(filesystem, loaded.catalog, cache, maps);
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
    // LE-LOADER: the stock capture-point pad, merged into one map's pad rules to prove a map that places it still starts.
    units::LoadInput capture_load; capture_load.domain = BattleDomain::land; capture_load.catalog = &loaded.catalog;
    capture_load.filesystem = &filesystem; capture_load.model = access.model; capture_load.types = {"Skirmish_Build_Pad"};
    const auto capture_pad = take(units::load_unit_tables(capture_load));
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
        fixture.match = discovered.common.match_defaults;
        fixture.match->pre_built_base = true;
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
        stock_fog(authored, inputs, players);
        stock_pads(authored, inputs, fixture, gates, players == 2 ? &capture_pad : nullptr);
        {
            // The live land path carries the option in the match, not in Fixture::pre_built_base: with it off, the completed
            // children's static rows (HQ relevance among them) must not load, while the prebuilt-on read has them.
            auto off_fixture = fixture;
            off_fixture.match->pre_built_base = false;
            const auto off_inputs = take(skirmish::read_land_start_inputs(off_fixture, filesystem, loaded.catalog, tables));
            const auto has_row = [](const skirmish::LandStartInputs& read, sim::tactical::TypeId type) {
                return std::any_of(read.static_tables.land_static_objects.begin(), read.static_tables.land_static_objects.end(),
                    [&](const auto& row) { return skirmish::type_id(row.id) == type; });
            };
            std::size_t children_with_rows{};
            for (const auto& [parent, child] : authored.content.land.initial_pad_children) {
                (void)parent;
                const auto unit = std::find_if(authored.start.units.begin(), authored.start.units.end(),
                    [&](const auto& row) { return row.state.entity_id == child; });
                if (unit == authored.start.units.end()) continue;
                if (has_row(inputs, unit->state.type_id) && !has_row(off_inputs, unit->state.type_id)) ++children_with_rows;
                expect(!has_row(off_inputs, unit->state.type_id), "prebuilt off loads no static row for a completed pad child");
            }
            expect(authored.content.land.initial_pad_children.empty() || children_with_rows > 0,
                "prebuilt on loads the completed children's static rows that prebuilt off omits");
        }
        if (players == 2) authored_open_ground_rear(authored);
        else compare_turning(authored, false);
        std::vector<std::string> selected_types;
        for (const auto& company : tables.land_companies) selected_types.push_back(company.id);
        // The authored overload also binds the map's pad types: bind the same merged tables on the explicit path.
        auto bound_tables = tables;
        skirmish::merge_land_pad_types(bound_tables, inputs.pad_tables);
        const auto bindings = take(skirmish::build_land_bindings(bound_tables, selected_types, gates));
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
    expect(stock_sampling_structures > 0, "the stock maps place multisampling structures, so the fog box check above is not vacuous");
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

    // #5204 (WLPA-84/85/86): one stock squad ordered 400 units from its stock spawn reaches the target. The baseline prints the
    // stock radius, hard extents, gap and slot spread of the squad so the layout figures are on record.
    {
        struct MoveRun { std::int64_t arrived{-1}; std::uint64_t ticks{}; LandMovement::Checkpoint state; };
        // One move of the stock squad on the given executor. Diagnostics print the parent, its members and the retained
        // contacts at fixed ticks, so a stalled move shows which gate holds it.
        const auto move_run = [&](const sim::PartitionExecutor& workers, int limit, bool diagnostics) {
            auto long_session = take(TacticalSession::create(built.start.setup, built.content));
            const auto start = long_session.land_movement()->checkpoint();
            const auto& start_squad = start.squads[selected_member->squad];
            Fixed far_x;
            if (!sim::math::try_add(start_squad.position.x, whole(400), far_x)) throw std::runtime_error("stock 400-unit destination overflow");
            const auto far_submitted = long_session.submit({{11, 1, 0}, {root}, MovePayload{sim::math::Vec3{far_x, start_squad.position.y, {}}}});
            if (!far_submitted) throw std::runtime_error(far_submitted.error().message);
            if (diagnostics) {
                Fixed widest, deepest;
                for (const auto& member : start.members) if (member.squad == selected_member->squad) {
                    std::cout << "L6 rows member=" << member.object << " radius=" << real(member.profile.radius) << " hard=" << real(member.hard_x)
                        << ',' << real(member.hard_y) << " gap=" << real(member.gap) << " slot=" << real(member.slot.x) << ',' << real(member.slot.y) << '\n';
                    widest = std::max(widest, Fixed::from_raw(member.slot.y.raw() < 0 ? -member.slot.y.raw() : member.slot.y.raw()));
                    deepest = std::max(deepest, Fixed::from_raw(-member.slot.x.raw()));
                }
                std::cout << "L6 rows squad=" << root << " rear=" << real(start_squad.rear_extent) << " half_width=" << real(widest)
                    << " depth=" << real(deepest) << " mapped=" << start_squad.has_mapped << '\n';
            }
            MoveRun run;
            for (int tick = 0; tick < limit && run.arrived < 0; ++tick) {
                take(long_session.step(workers));
                run.ticks = static_cast<std::uint64_t>(tick) + 1;
                if (long_session.land_movement()->squads()[selected_member->squad].order == sim::tactical::LandMoveOrder::reached) run.arrived = tick;
                if (diagnostics && (tick == 0 || tick == 1 || tick == 4 || tick == 29 || tick == 99 || tick == 299 || tick == 999 || tick == 2399)) {
                    const auto& movement = *long_session.land_movement();
                    const auto& parent_now = movement.squads()[selected_member->squad];
                    std::cout << "L6 diag tick=" << tick << " order=" << static_cast<int>(parent_now.order) << " speed=" << real(parent_now.speed)
                        << " facing=" << real(parent_now.facing) << " turning=" << parent_now.turning << " node=" << parent_now.next_node << '/'
                        << parent_now.path.nodes.size() << " pos=" << real(parent_now.position.x) << ',' << real(parent_now.position.y)
                        << " rear=" << real(parent_now.rear_extent) << " mapped_axis=" << real(parent_now.mapped_axis)
                        << " contacts=" << movement.contacts().size() << '\n';
                    for (const auto& row : movement.members()) if (row.squad == selected_member->squad)
                        std::cout << "L6 diag   member=" << row.object << " pos=" << real(row.position.x) << ',' << real(row.position.y)
                            << " slot=" << real(row.slot.x) << ',' << real(row.slot.y) << " speed=" << real(row.speed) << " state="
                            << static_cast<int>(row.state) << " collision_speed=" << real(row.collision_speed)
                            << " terrain_blocked=" << row.terrain_blocked << '\n';
                    for (const auto& c : movement.contacts()) std::cout << "L6 diag   contact " << c.first << '-' << c.second
                        << " kind=" << static_cast<int>(c.kind) << " state=" << static_cast<int>(c.state)
                        << " target=" << real(c.terrain_target.x) << ',' << real(c.terrain_target.y) << " search=" << static_cast<int>(c.search)
                        << " route=" << static_cast<int>(c.route) << " installed=" << c.installed << '\n';
                }
            }
            run.state = long_session.land_movement()->checkpoint();
            const auto& end_squad = run.state.squads[selected_member->squad];
            if (diagnostics) std::cout << "L6 400-unit move arrived_tick=" << run.arrived << " position=" << real(end_squad.position.x) << ','
                << real(end_squad.position.y) << " from=" << real(start_squad.position.x) << ',' << real(start_squad.position.y) << '\n';
            return run;
        };
        const sim::InlineExecutor serial_executor;
        const auto arrived = move_run(serial_executor, 2400, true);
        expect(arrived.arrived >= 0, "#5204 a stock squad ordered 400 units from its stock spawn reaches the target");
        // Worker matrix: the same move at 1/2/4/8 workers and with reversed partitions ends in the identical state.
        constexpr int matrix_ticks = 600;
        const auto reference = move_run(serial_executor, matrix_ticks, false);
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            platform::ThreadWorkerAdapter threads(workers);
            expect(move_run(threads, matrix_ticks, false).state == reference.state, "#5204 stock move equals at 1/2/4/8 workers");
        }
        test_support::ReverseOrderExecutor reversed(4);
        expect(move_run(reversed, matrix_ticks, false).state == reference.state, "#5204 stock move equals with reverse partitions");
    }
}

}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
        contracts(); return checks.finish();
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
