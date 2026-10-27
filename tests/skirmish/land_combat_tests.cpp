#include "land_test_support.hpp"
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/skirmish/land_content.hpp"
#include "eawr/scene/scene.hpp"
#include "../support/env.hpp"
#include "../support/reverse_partition_executor.hpp"
#include "../../src/sim/tactical/session_land_combat_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
using Bytes = std::vector<std::uint8_t>;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
template<class T> T take(core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(value).value();
}
void must(core::Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }

struct Scenario { TacticalSetup setup; TacticalContent content; };
Scenario scenario(bool tank = false, std::size_t count = 4) {
    Scenario result;
    auto field = make_field(std::vector<std::string>(40, std::string(64, '.')));
    auto& content = result.content;
    content.domain = BattleDomain::land; content.land_terrain = field.terrain;
    content.land.clear_bit = clear; content.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    DamageRules rules;
    rules.shield_recharge_frames = 30; rules.damage_types = 2; rules.armor_types = 2;
    rules.armor_mods = {whole(1), Fixed::from_ratio(1, 2).value(), whole(2), whole(1)};
    content.durability.damage = rules;
    for (const TypeId body : {12U, 22U}) {
        DurabilityProfile health;
        health.type_id = body; health.max_hull = whole(body == 12 ? 12 : 48);
        health.armor_type = body == 12 ? 0 : 1; health.allow_diminishing_firepower = false;
        content.durability.profiles.push_back(health);
        CombatProfile gun;
        gun.type_id = body; gun.category_bits = body == 12 ? 1 : 2;
        gun.max_attack_distance = whole(150); gun.living_projectile_collision = true;
        gun.collision = CollisionBox{{whole(-2), whole(-2), whole(-2)}, {whole(2), whole(2), whole(4)}};
        WeaponProfile weapon;
        weapon.range = whole(150); weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 100;
        weapon.pulse_count = 3; weapon.pulse_delay_frames = 8;
        ShotProfile shot;
        shot.damage = whole(body == 12 ? 4 : 8); shot.damage_type = body == 12 ? 0 : 1;
        shot.speed = whole(4); shot.max_travel = whole(500);
        weapon.shot = shot; gun.weapons.push_back(weapon);
        content.combat.profiles.push_back(gun);
        content.combat.footprints.push_back({body, whole(2), whole(2), whole(2)});
        LandCompanyBinding company;
        company.type = body - 1; company.company.id = body == 12 ? "synthetic-infantry" : "synthetic-tank";
        company.company.team_max_speed = whole(40); company.effective_team_max_speed = whole(40);
        const auto number = body == 22 ? 1U : count;
        company.company.max_squad_size = static_cast<std::uint32_t>(number);
        company.formation_spacing = Fixed::from_ratio(1, 5).value();
        for (std::size_t i = 0; i < number; ++i) {
            units::LandCompanyMember member;
            member.type = body == 12 ? "synthetic-infantry-member" : "synthetic-tank-member";
            member.kind = body == 12 ? units::LandObjectKind::infantry : units::LandObjectKind::vehicle;
            member.create_team = true; member.movement_class = "walker"; member.max_speed = whole(40);
            member.locomotor = "synthetic-walking";
            company.company.members.push_back(member);
            company.members.push_back({body, {whole(40), whole(120), whole(150), whole(2), whole(1), false,
                body == 12 ? units::LandCollisionGroup::infantry : units::LandCollisionGroup::none}, 0, {}, {}, {}});
        }
        content.land.companies.push_back(std::move(company));
    }
    auto& setup = result.setup;
    setup.domain = BattleDomain::land; setup.seed = 49; setup.content_identity[0] = 91;
    setup.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}};
    UnitState first; first.entity_id = 1; first.type_id = tank ? 21U : 11U; first.owner = 1;
    first.position = {whole(100), whole(100), {}};
    UnitState second = first; second.entity_id = 2; second.type_id = 11; second.owner = 2;
    second.position.x = whole(190);
    setup.units = {first, second};
    return result;
}

Bytes save(const TacticalSession& session) {
    return take(checkpoint::save_tactical_checkpoint(session, session.pending_commands(),
        checkpoint::default_seats(session.record().setup), checkpoint::tactical_identities(session.record().setup.content_identity)));
}
TacticalSession restore(const TacticalSession& session, const TacticalContent& content, const sim::PartitionExecutor& executor) {
    const auto bytes = save(session);
    auto loaded = take(checkpoint::load_tactical_checkpoint(bytes,
        checkpoint::tactical_identities(session.record().setup.content_identity),
        [&content](const TacticalReplay& replay) { return TacticalSession::from_replay(replay, content); }, executor));
    expect(loaded.work.resimulated_ticks == 0 && save(loaded.session) == bytes, "combat checkpoint restores directly and re-encodes identically");
    return std::move(loaded.session);
}

struct Fight { std::vector<std::string> hashes; Bytes final; std::size_t fired{}, hits{}, deaths{}; bool midflight{}; };
Fight fight(const Scenario& input, const sim::PartitionExecutor& executor, bool resumes) {
    auto session = take(TacticalSession::create(input.setup, input.content));
    must(session.submit({{0, 1, 1}, {1}, AttackPayload{2}}));
    must(session.submit({{0, 2, 1}, {2}, AttackPayload{1}}));
    Fight result;
    for (int tick = 0; tick < 220; ++tick) {
        const auto output = take(session.step(executor));
        expect(output.diagnostics.empty(), "in-range land attacks have no diagnostics");
        result.hashes.push_back(output.state_sha256);
        for (const auto& event : output.snapshot->combat_events()) {
            result.fired += event.kind == CombatEventKind::weapon_fired;
            result.hits += event.kind == CombatEventKind::projectile_hit;
        }
        for (const auto& event : output.snapshot->events()) if (event.kind == EventKind::unit_destroyed) {
            ++result.deaths;
            expect(!session.unit_state(event.unit), "destroyed land object leaves the live state immediately");
            expect(std::none_of(session.land_movement()->members().begin(), session.land_movement()->members().end(),
                [&](const auto& member) { return member.object == event.unit; }), "destroyed member leaves movement and its contact index");
            expect(std::none_of(session.land_occupancy()->occupations().begin(), session.land_occupancy()->occupations().end(),
                [&](const auto& occupation) { return occupation.object == event.unit; }), "destroyed member frees its occupation immediately");
        }
        bool pulse = false;
        for (const auto& member : session.land_movement()->members()) if (const auto state = session.combat_state(member.object))
            for (const auto& weapon : state->weapons) pulse |= weapon.pulses_left > 0 && weapon.pulses_left < 3;
        if (!result.midflight && !output.snapshot->projectiles().empty() && pulse) {
            result.midflight = true;
            if (resumes) session = restore(session, input.content, executor);
        }
        if (resumes && tick == 120) session = restore(session, input.content, executor);
    }
    result.final = save(session);
    expect(session.land_movement()->squads().size() < 2, "an empty squad root is removed after its last member dies");
    auto playback = take(TacticalSession::from_replay(take(parse_replay(take(write_replay(session.record())))), input.content));
    for (const auto& hash : result.hashes) expect(take(playback.step(executor)).state_sha256 == hash,
        "land combat command recording reproduces the digest at every tick");
    expect(save(playback) == result.final, "combat replay reproduces the final complete checkpoint");
    expect(result.fired > 0 && result.hits > 0 && result.deaths > 0, "land fight launches, hits, damages and kills");
    expect(result.midflight, "checkpoint witness contains a live projectile and a pending pulse");
    return result;
}

void deterministic() {
    sim::InlineExecutor serial;
    for (const bool tank : {false, true}) {
        const auto input = scenario(tank);
        const auto baseline = fight(input, serial, false);
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            platform::ThreadWorkerAdapter executor(workers);
            test_support::ReverseOrderExecutor reverse(workers);
            for (const auto* runner : std::array<const sim::PartitionExecutor*, 2>{&executor, &reverse}) {
                const auto candidate = fight(input, *runner, true);
                expect(candidate.hashes == baseline.hashes && candidate.final == baseline.final,
                    "infantry and tank fights are equal on 1/2/4/8 workers and reverse partitions, including restored flight/pulse state");
            }
        }
    }
}

void commands_and_armor() {
    auto input = scenario(false, 1);
    // Give the one target an explicit half-damage armor row and no gun.
    input.content.durability.profiles[0].max_hull = whole(100);
    input.content.durability.profiles[0].armor_type = 1;
    auto session = take(TacticalSession::create(input.setup, input.content));
    const auto members = session.land_movement()->members();
    const auto attacker = members[0].object, victim = members[1].object;
    sim::InlineExecutor executor;
    must(session.submit({{0, 1, 1}, {1}, AttackPayload{2}}));
    take(session.step(executor));
    expect(session.unit_state(1)->order.target == 2 && session.unit_state(attacker)->order.target == 2
        && session.combat_state(attacker)->direct && session.combat_state(attacker)->attack_target == victim,
        "attack is owned by the root, fans out to members, and fires at the ordered hostile squad");
    must(session.submit({{1, 1, 2}, {1}, StopPayload{}}));
    take(session.step(executor));
    expect(session.unit_state(attacker)->order.kind == OrderKind::stop && !session.combat_state(attacker)->direct
        && session.combat_state(attacker)->attack_target == 0, "stop clears the ordered target on every member");
    const auto order = session.unit_state(1)->order;
    must(session.submit({{2, 1, 3}, {1}, AttackPayload{99999}}));
    const auto refused = take(session.step(executor));
    expect(refused.diagnostics.size() == 1 && session.unit_state(1)->order == order, "invalid target reports a diagnostic and preserves the accepted order");
    expect(!session.submit({{3, 1, 4}, {1}, AttackMovePayload{}}), "land attack-move is explicitly rejected");
    expect(!session.submit({{3, 1, 5}, {1}, GuardPayload{}}), "land guard is explicitly rejected");
    // Idle acquisition resumes at the named cadence; the first hit uses authored damage x armor.
    bool hit = false;
    for (int i = 0; i < 100 && !hit; ++i) {
        const auto output = take(session.step(executor));
        for (const auto& event : output.snapshot->combat_events()) if (event.kind == CombatEventKind::projectile_hit && event.target == victim) {
            expect(session.durability_state(victim)->hull == whole(98), "authored projectile damage 4 times armor 0.5 removes exactly 2 hull");
            hit = true;
        }
    }
    expect(hit, "idle land members acquire and fire without an attack order");
    auto invalid = scenario(true, 1);
    invalid.content.combat.profiles[0].valid_target = false;
    auto invalid_session = take(TacticalSession::create(invalid.setup, invalid.content));
    must(invalid_session.submit({{0, 1, 1}, {1}, AttackPayload{2}}));
    expect(take(invalid_session.step(executor)).diagnostics.size() == 1
        && invalid_session.unit_state(1)->order.kind == OrderKind::none,
        "an enemy squad with no targetable living members rejects attack without changing the order");
}

void terrain_contacts() {
    auto field = make_field(std::vector<std::string>(4, std::string(4, '.')));
    sim::tactical::detail::ProjectileStep flight;
    flight.from = {whole(10), whole(10), whole(10)};
    flight.projectile.position = {whole(30), whole(10), whole(-10)};
    auto ground = take(sim::tactical::session_detail::land_projectile_contact(*field.terrain, flight));
    expect(ground.expired && !ground.hit && ground.expiry_reason == ProjectileExpiryReason::ground_contact
        && ground.projectile.position == sim::math::Vec3{whole(20), whole(10), whole(0)}, "ground terminal is at the exact segment contact");
    flight.hit = 99; flight.contact = {whole(20), whole(10), whole(0)};
    auto tie = take(sim::tactical::session_detail::land_projectile_contact(*field.terrain, flight));
    expect(tie.hit == 99 && !tie.expired, "LR2-02 object contact wins an exact terrain tie");
    flight.contact = {whole(25), whole(10), whole(-5)};
    ground = take(sim::tactical::session_detail::land_projectile_contact(*field.terrain, flight));
    // The two-thirds crossing is rounded to the fixed grid, so the ground height is zero to within a few dozen raw units.
    const auto ground_error = ground.projectile.position.z.raw() < 0 ? -ground.projectile.position.z.raw() : ground.projectile.position.z.raw();
    expect(!ground.hit && ground.expired && ground_error <= 64 && ground.expiry_reason == ProjectileExpiryReason::ground_contact,
        "terrain before an object spends the shot at ground");
}

void range_policy() {
    auto input = scenario(false, 1);
    input.setup.units[1].position.x = whole(251); // 151 from a 150-range weapon, inside target extent 2.
    auto session = take(TacticalSession::create(input.setup, input.content));
    sim::InlineExecutor executor;
    must(session.submit({{0, 1, 1}, {1}, AttackPayload{2}}));
    bool fired = false;
    for (int i = 0; i < 60; ++i) {
        const auto output = take(session.step(executor));
        expect(output.diagnostics.empty(), "target radius extends the authored planar attack range");
        for (const auto& event : output.snapshot->combat_events()) fired |= event.kind == CombatEventKind::weapon_fired;
    }
    expect(fired, "a target just beyond centre range is still fired on inside its authored footprint");
    LandTerrainInput raised;
    raised.width = 64; raised.height = 40; raised.spacing = whole(20);
    raised.heights.resize(64 * 40); raised.surfaces.resize(64 * 40); raised.passability.assign(64 * 40, clear);
    raised.movement_masks = {{"walker", walker}};
    for (std::size_t row = 0; row < 40; ++row) for (std::size_t column = 12; column < 64; ++column)
        raised.heights[row * 64 + column] = whole(500);
    auto hill = input;
    hill.content.land_terrain = std::make_shared<const LandTerrain>(take(LandTerrain::build(std::move(raised))));
    auto elevated = take(TacticalSession::create(hill.setup, hill.content));
    bool planar = false;
    for (int i = 0; i < 60; ++i) for (const auto& event : take(elevated.step(executor)).snapshot->combat_events())
        planar |= event.kind == CombatEventKind::weapon_fired;
    expect(planar, "idle acquisition uses planar range even when terrain height exceeds the weapon range");
    input.setup.units[1].position.x = whole(600);
    auto distant = take(TacticalSession::create(input.setup, input.content));
    const auto position = distant.land_movement()->squads()[0].position;
    must(distant.submit({{0, 1, 1}, {1}, AttackPayload{2}}));
    const auto output = take(distant.step(executor));
    expect(output.diagnostics.size() == 1 && distant.unit_state(1)->order.kind == OrderKind::attack
        && distant.land_movement()->squads()[0].position == position,
        "out-of-range attack retains its target and visibly diagnoses the unqualified no-chase fallback");
}

void ground_blast_once() {
    auto input = scenario(true, 1);
    input.content.durability.profiles[0].max_hull = whole(100);
    input.content.combat.profiles[0].weapons.clear();
    auto& weapon = input.content.combat.profiles[1].weapons[0];
    weapon.pulse_count = 1; weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 1000;
    weapon.shot->damage = {}; weapon.shot->blast.damage = whole(6); weapon.shot->blast.radius = whole(100);
    weapon.shot->blast.max_delay = {};
    LandTerrainInput terrain;
    terrain.width = 32; terrain.height = 16; terrain.spacing = whole(20);
    terrain.heights.resize(32 * 16); terrain.surfaces.resize(32 * 16); terrain.passability.assign(32 * 16, clear);
    terrain.movement_masks = {{"walker", walker}};
    for (std::size_t row = 0; row < 16; ++row) terrain.heights[row * 32 + 7] = whole(30);
    input.content.land_terrain = std::make_shared<const LandTerrain>(take(LandTerrain::build(std::move(terrain))));
    auto session = take(TacticalSession::create(input.setup, input.content));
    const auto members = session.land_movement()->members();
    const auto victim = members[1].object;
    must(session.submit({{0, 1, 1}, {1}, AttackPayload{2}}));
    sim::InlineExecutor executor;
    bool terminal = false;
    for (int i = 0; i < 400 && !terminal; ++i) {
        const auto output = take(session.step(executor));
        for (const auto& event : output.snapshot->combat_events())
            terminal |= event.kind == CombatEventKind::projectile_expired
                && event.outcome == static_cast<std::uint32_t>(ProjectileExpiryReason::ground_contact);
    }
    expect(terminal && session.durability_state(victim)->hull == whole(88),
        "a ridge terminal delivers one authored splash through shared armor damage");
    take(session.step(executor));
    expect(session.durability_state(victim)->hull == whole(88), "a spent ground shot has no second lifetime blast");
}

void retained_contact_death() {
    const auto input = scenario(false, 2);
    auto session = take(TacticalSession::create(input.setup, input.content));
    auto saved = session.land_movement()->checkpoint();
    const auto first = saved.members[0].object, blocker = saved.members[2].object;
    const auto survivor = saved.members[3];
    LandContact contact;
    contact.first = first; contact.second = blocker; contact.kind = LandContactKind::stopped_stopped;
    saved.contacts.push_back(contact);
    auto movement = LandMovement::from_checkpoint(std::move(saved));
    expect(movement.has_value(), "death fixture retains a valid stopped-pair blocker contact");
    if (!movement) return;
    expect(movement->remove_member(blocker) && movement->contacts().empty(), "a dead blocker leaves the retained contact index");
    const auto remaining = std::find_if(movement->members().begin(), movement->members().end(),
        [&](const auto& row) { return row.object == survivor.object; });
    expect(remaining != movement->members().end() && remaining->slot == survivor.slot,
        "a casualty preserves the other member's formation slot");
    expect(movement->remove_member(survivor.object) && movement->squads().size() == 1,
        "the final member removes its squad and compacts the remaining squad bindings");
    expect(LandMovement::from_checkpoint(movement->checkpoint()).has_value(), "the casualty-compacted movement state remains restorable");
}

void direct_body_wrapper() {
    auto input = scenario(false, 1);
    input.content.land.companies[0].type = 12;
    input.content.land.companies[0].company.members[0].create_team = false;
    for (auto& root : input.setup.units) root.type_id = 12;
    auto session = take(TacticalSession::create(input.setup, input.content));
    expect(!session.combat_state(1) && !session.combat_state(2)
        && !session.durability_state(1) && !session.durability_state(2),
        "a direct object's command wrapper does not duplicate its body's health or gun");
    must(session.submit({{0, 1, 1}, {1}, AttackPayload{2}}));
    sim::InlineExecutor executor;
    std::size_t fired{}, hits{};
    for (int i = 0; i < 100; ++i) {
        const auto output = take(session.step(executor));
        for (const auto& event : output.snapshot->combat_events()) {
            if (event.kind == CombatEventKind::weapon_fired) {
                ++fired; expect(event.shooter > 2, "only direct object bodies launch shots");
            }
            if (event.kind == CombatEventKind::projectile_hit) {
                ++hits; expect(event.target > 2, "a same-type command wrapper cannot absorb a projectile");
            }
        }
    }
    expect(fired > 0 && hits > 0, "direct ground objects still exchange physical combat through their command wrappers");
}

int stress() {
    auto input = scenario(false, 9);
    input.setup.units.clear();
    // 88 roots + 792 members = 880 objects, with two parallel opposing lines.
    for (sim::EntityId id = 1; id <= 88; ++id) {
        UnitState root;
        root.entity_id = id; root.type_id = 11; root.owner = id <= 44 ? 1U : 2U;
        root.position = {whole(root.owner == 1 ? 100 : 190), whole(40 + static_cast<std::int64_t>((id - 1) % 44) * 40), {}};
        input.setup.units.push_back(root);
    }
    input.content.land_terrain = make_field(std::vector<std::string>(112, std::string(64, '.'))).terrain;
    input.content.durability.profiles[0].max_hull = whole(10000);
    auto session = take(TacticalSession::create(input.setup, input.content));
    expect(session.land_movement()->members().size() + session.land_movement()->squads().size() == 880,
        "combat stress fixture starts at 880 live land objects");
    platform::ThreadWorkerAdapter executor(6);
    for (int i = 0; i < 12; ++i) take(session.step(executor));
    core::tick_profile::Recording recording;
    const auto start = std::chrono::steady_clock::now();
    {
        core::tick_profile::Capture capture(recording);
        for (int i = 0; i < 48; ++i) take(session.step(executor));
    }
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 48;
    std::cout << "land combat stress objects=880 workers=6 ms/tick=" << ms << " budget=16.7\n";
    for (const auto name : {"land-targets", "land-fire", "land-projectiles", "land-impacts", "land-canonical-state", "land-state-hash"}) {
        double total{};
        for (const auto& cost : recording.costs) if (cost.name == name) total += cost.ms;
        std::cout << name << " ms/tick=" << total / 48 << '\n';
    }
    // Timing is reported from the remote host; deterministic work/state contracts above are the merge gates.
    expect(!session.snapshot()->projectiles().empty(), "stress harness exercises active projectile combat");
    return checks.finish();
}

int installed() {
    const auto root = testing::environment("EAWR_EAW_GAME_ROOT");
    if (!root || root->empty()) { std::cout << "SKIPPED: no game data\n"; return 77; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"})
        mounts.push_back(take(vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data")).mount);
    const auto filesystem = take(vfs::Vfs::mount(mounts));
    const auto loaded = take(data::load_catalog(filesystem, data::Profile::foc));
    scene::VfsAssetCache cache(filesystem);
    skirmish::Fixture fixture;
    fixture.map = "data/art/maps/_mp_land_naboo.ted";
    fixture.slots = {{1, "Rebel", 0, true, {}}, {2, "Empire", 1, true, {}}};
    const auto discovered = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, {}));
    fixture.map_sha256 = discovered.common.map_sha256;
    units::LoadInput load;
    load.domain = BattleDomain::land; load.catalog = &loaded.catalog; load.filesystem = &filesystem; load.model = cache.access().model;
    std::set<std::string> requested;
    for (const auto& faction : discovered.factions) requested.insert(faction.forces.begin(), faction.forces.end());
    load.types.assign(requested.begin(), requested.end());
    auto tables = take(units::load_unit_tables(load));
    for (const auto& company : tables.land_companies) for (const auto& member : company.members) requested.insert(member.type);
    load.types.assign(requested.begin(), requested.end()); tables = take(units::load_unit_tables(load));
    const auto inputs = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, tables));
    auto gates = take(skirmish::authored_land_content_inputs(tables));
    gates.last_known_capacity = {{1, 50}, {2, 50}};
    const auto built = take(skirmish::build_land_start(fixture, inputs, gates));
    auto initial = built.start.setup;
    // Retain two authored companies; place the witness on decoded open ground, outside weapon range,
    // then use real land paths to close. Stock names here identify test data only.
    std::vector<UnitState> pair;
    for (const auto owner : {1U, 2U}) {
        const auto company = std::find_if(initial.units.begin(), initial.units.end(), [&](const auto& unit) {
            return unit.owner == owner && std::any_of(built.content.land.companies.begin(), built.content.land.companies.end(),
                [&](const auto& row) { return row.type == unit.type_id && !row.members.empty()
                    && built.content.combat.find(row.members.front().type); });
        });
        if (company == initial.units.end()) throw std::runtime_error("stock witness has no armed company for a side");
        pair.push_back(*company);
    }
    std::sort(pair.begin(), pair.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
    auto content = built.content;
    content.land.static_objects.clear(); content.land.occupations.clear();
    // Start from authored spawn terrain; destinations are opposite sides of their midpoint.
    auto a = pair[0].position, b = pair[1].position;
    const sim::math::Vec3 midpoint{Fixed::from_raw(a.x.raw() / 2 + b.x.raw() / 2),
        Fixed::from_raw(a.y.raw() / 2 + b.y.raw() / 2), {}};
    initial.units = pair;
    auto session = take(TacticalSession::create(initial, content));
    const auto count = session.land_movement()->members().size();
    for (const auto& unit : pair) must(session.submit({{0, unit.owner, 1}, {unit.entity_id}, MovePayload{midpoint}}));
    platform::ThreadWorkerAdapter executor(4);
    std::array<bool, 2> shots{};
    bool died = false, moved = false;
    for (int i = 0; i < 3600 && !died; ++i) {
        const auto output = take(session.step(executor));
        for (const auto& event : output.snapshot->combat_events()) if (event.kind == CombatEventKind::weapon_fired) {
            const auto shooter = session.unit_state(event.shooter);
            if (shooter && shooter->owner >= 1 && shooter->owner <= 2) shots[shooter->owner - 1] = true;
        }
        for (const auto& squad : session.land_movement()->squads()) {
            const auto before = std::find_if(pair.begin(), pair.end(), [&](const auto& row) { return row.entity_id == squad.object; });
            if (before != pair.end()) moved |= squad.position != sim::math::Vec2{before->position.x, before->position.y};
        }
        died = session.land_movement()->members().size() < count;
    }
    expect(moved && shots[0] && shots[1] && died, "two stock companies on Naboo walk into range, exchange authored fire and lose a member");
    std::cout << "stock combat map=" << fixture.map << " moved=" << moved << " both-fire=" << (shots[0] && shots[1]) << " death=" << died << '\n';
    return checks.finish();
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
        if (argc == 2 && std::string_view(argv[1]) == "--stress") return stress();
        terrain_contacts(); range_policy(); ground_blast_once(); retained_contact_death(); direct_body_wrapper(); commands_and_armor(); deterministic(); return checks.finish();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
