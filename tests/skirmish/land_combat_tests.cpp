#include "land_test_support.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/skirmish/land_content.hpp"
#include "eawr/skirmish/land_fog.hpp"
#include "eawr/scene/scene.hpp"
#include "../support/env.hpp"
#include "../support/reverse_partition_executor.hpp"
#include "../../src/sim/tactical/session_land_combat_internal.hpp"
#include "../../src/sim/tactical/combat_algorithms.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <map>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <ctime>
#endif
#include <string>

namespace {
using namespace eawr;
using namespace sim::tactical;
using sim::EntityId;
using sim::invalid_entity_id;
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
        if (body == 12) company.company.team_collision_class = units::LandCollisionGroup::infantry; // WLPA-82: the container's class
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
    // WLPA-82: a contact names squad bodies; the casualties are soldiers of the blocking squad.
    const auto first = saved.squads[0].object, blocker_squad = saved.squads[1].object;
    const auto blocker = saved.members[2].object;
    const auto survivor = saved.members[3];
    LandContact contact;
    contact.first = first; contact.second = blocker_squad; contact.kind = LandContactKind::stopped_stopped;
    saved.contacts.push_back(contact);
    auto detached_movement = LandMovement::from_checkpoint(saved);
    expect(detached_movement.has_value(), "containment fixture starts with a valid movement roster");
    if (detached_movement) {
        auto detached = detached_movement->detach_squad(blocker_squad);
        expect(detached && detached->members.size() == 2 && detached_movement->squads().size() == 1
            && detached_movement->contacts().empty(), "detaching removes a whole squad and its contacts");
        if (detached) {
            const auto initial = detached->squad.position;
            const sim::math::Vec2 destination{Fixed::from_raw(initial.x.raw() + whole(20).raw()), initial.y};
            expect(detached_movement->restore_squad(std::move(*detached), destination)
                && detached_movement->squads().size() == 2 && detached_movement->members().size() == 4
                && LandMovement::from_checkpoint(detached_movement->checkpoint()).has_value(),
                "restoring a detached squad recovers its roster and valid movement state");
            expect(detached_movement->squads().back().position == destination,
                "restoring a detached squad moves its root to the chosen exit position");
        }
    }
    auto movement = LandMovement::from_checkpoint(std::move(saved));
    expect(movement.has_value(), "death fixture retains a valid stopped-pair blocker contact");
    if (!movement) return;
    expect(movement->remove_member(blocker) && movement->contacts().size() == 1,
        "a dead soldier does not end its squad's retained contact: the squad is the body");
    const auto remaining = std::find_if(movement->members().begin(), movement->members().end(),
        [&](const auto& row) { return row.object == survivor.object; });
    expect(remaining != movement->members().end() && remaining->slot == survivor.slot,
        "a casualty preserves the other member's formation slot");
    expect(movement->remove_member(survivor.object) && movement->squads().size() == 1 && movement->contacts().empty(),
        "the final member removes its squad, its body and the retained contact, and compacts the remaining squad bindings");
    expect(LandMovement::from_checkpoint(movement->checkpoint()).has_value(), "the casualty-compacted movement state remains restorable");
}

// LC-28/LC-29 (#5895): which squad member an idle attacker holds, for priority sets and each Is_Better_Target
// branch. Entity 1 is the attacker (infantry, player 1). Entity 2 is an enemy infantry squad, entity 3 an enemy
// tank squad that may walk in; both can shoot back, so their hulls are large unless the case needs a death.
// The scan judges candidates in ascending ID and the comparator is not transitive (a worse-ranked but nearer
// candidate beats the best so far), so a candidate wins on rank only when it comes after the held one: the
// walking squad is the tank, which has the higher ID.
struct Replacement {
    std::optional<Fixed> infantry_priority, tank_priority; // nullopt: the set excludes the type
    std::int64_t infantry_x{230};       // the enemy infantry stands at (infantry_x, 100): 130 from the attacker
    std::int64_t tank_start_y{500};     // the enemy tank starts at (100, tank_start_y)
    std::int64_t tank_y{190};           // and ends at (100, tank_y); the same value means it stands still
    std::int64_t leave_tick{};          // at this tick the infantry walks on to x = 1000 (0: never)
    bool infantry_unhittable{};         // the attacker's first weapon cannot hit infantry (its second can)
    bool attack_infantry{};             // an attack order on the infantry squad
    bool fragile_tank{};                // the tank dies to a few hits
};
Fixed half_steps(const std::int64_t halves) { return Fixed::from_ratio(halves, 2).value(); }

Scenario replacement_scenario(const Replacement& plan) {
    auto result = scenario(false, 1);
    auto& content = result.content;
    for (auto& health : content.durability.profiles) health.max_hull = whole(10000);
    if (plan.fragile_tank) content.durability.profiles[1].max_hull = whole(8);
    PrioritySet set;
    set.unlisted = whole(100);
    set.rows = {{12, plan.infantry_priority}, {22, plan.tank_priority}};
    content.combat.priority_sets.push_back(set);
    content.combat.profiles[0].priority_set = 0;
    if (plan.infantry_unhittable) {
        // Two object weapons: the first cannot hit the infantry category, the second can, so the infantry stays
        // suitable and only the comparator's hit test tells it from the tank.
        auto& weapons = content.combat.profiles[0].weapons;
        weapons.push_back(weapons[0]);
        weapons[0].category_restrictions = 1;
    }
    auto& units = result.setup.units;
    units.resize(3);
    units[0].entity_id = 1; units[0].type_id = 11; units[0].owner = 1;
    units[0].position = {whole(100), whole(100), {}};
    units[1] = units[0]; units[1].entity_id = 2; units[1].owner = 2;
    units[1].position = {whole(plan.infantry_x), whole(100), {}};
    units[2] = units[0]; units[2].entity_id = 3; units[2].type_id = 21; units[2].owner = 2;
    units[2].position = {whole(100), whole(plan.tank_start_y), {}};
    return result;
}

EntityId member_of(const TacticalSession& session, const EntityId root) {
    for (const auto& member : session.land_movement()->members())
        if (session.land_movement()->squads()[member.squad].object == root) return member.object;
    return invalid_entity_id;
}

// The attacker's held targets in order of acquisition, repeats and gaps dropped; the last tick's target is
// returned apart.
std::vector<EntityId> hold_trace(const Replacement& plan, const sim::PartitionExecutor& executor, const int ticks,
    Fight* record = nullptr, const bool resumes = false, EntityId* final_target = nullptr) {
    auto input = replacement_scenario(plan);
    auto session = take(TacticalSession::create(input.setup, input.content));
    const auto attacker = member_of(session, 1), infantry_id = member_of(session, 2), tank = member_of(session, 3);
    expect(attacker != invalid_entity_id && infantry_id != invalid_entity_id && tank != invalid_entity_id,
        "the replacement fixture has one member per squad");
    std::uint64_t sequence = 1;
    const bool tank_walks = plan.tank_start_y != plan.tank_y;
    if (tank_walks) must(session.submit({{0, 2, sequence++}, {3}, MovePayload{{whole(100), whole(plan.tank_y), {}}}}));
    if (plan.attack_infantry) must(session.submit({{0, 1, sequence++}, {1}, AttackPayload{2}}));
    if (plan.leave_tick > 0)
        must(session.submit({{static_cast<std::uint64_t>(plan.leave_tick), 2, sequence++}, {2},
            MovePayload{{whole(1000), whole(100), {}}}}));
    const auto root_position = [&session](const EntityId root) {
        for (const auto& squad : session.land_movement()->squads()) if (squad.object == root) return squad.position;
        return sim::math::Vec2{};
    };
    const auto tank_before = root_position(3), infantry_before = root_position(2);
    std::vector<EntityId> trace;
    for (int tick = 0; tick < ticks; ++tick) {
        const auto output = take(session.step(executor));
        expect(output.diagnostics.empty(), "idle land targeting has no diagnostics");
        if (record != nullptr) record->hashes.push_back(output.state_sha256);
        if (resumes && tick == 160) session = restore(session, input.content, executor);
        const auto state = session.combat_state(attacker);
        const EntityId held = state ? state->attack_target : invalid_entity_id;
        if (final_target != nullptr) *final_target = held;
        // Only the targets held, in order; the ticks with none between two of them are not recorded.
        if (held != invalid_entity_id && (trace.empty() || trace.back() != held)) trace.push_back(held);
    }
    if (tank_walks && tank != invalid_entity_id) expect(root_position(3) != tank_before, "the walking enemy tank squad moved");
    if (plan.leave_tick > 0 && infantry_id != invalid_entity_id)
        expect(root_position(2) != infantry_before, "the leaving enemy infantry squad moved");
    if (record != nullptr) record->final = save(session);
    return trace;
}

// The shared comparator on hand-built units, for the health and stickiness rules that need damage a session cannot
// stage (the land session accepts no scripted damage): a scanner at the origin, a held target and a candidate.
void comparator_health_and_stickiness() {
    using detail::combat_detail::UnitCombat;
    DurabilityRules rules;
    rules.damaged_fraction = Fixed::from_ratio(33, 100).value();
    CombatTable table;
    detail::CombatWorld world;
    world.table = &table; world.rules = &rules;
    CombatProfile scanner_profile;
    scanner_profile.type_id = 1; scanner_profile.stickiness = whole(30);
    CombatState scanner_state;
    detail::CombatUnit scanner;
    scanner.id = 1; scanner.owner = 1; scanner.profile = &scanner_profile; scanner.combat = &scanner_state;
    CombatProfile target_profile;
    target_profile.type_id = 2;
    DurabilityProfile health;
    health.type_id = 2; health.max_hull = whole(100); health.damage_tracking = true;
    DurabilityState full, low, tracked;
    full.hull = whole(100); low.hull = whole(20);
    tracked.hull = whole(100); tracked.damage_rate = whole(5); // 20 seconds to live
    const auto unit = [&](const EntityId id, const std::int64_t x, const DurabilityState& state) {
        detail::CombatUnit target;
        target.id = id; target.owner = 2; target.profile = &target_profile;
        target.durability_profile = &health; target.durability = &state;
        target.position = {whole(x), {}, {}};
        return target;
    };
    UnitCombat judge(world, scanner);
    judge.land_release_direct();
    const auto near_healthy = unit(2, 50, full), far_healthy = unit(3, 90, full);
    const auto far_low = unit(4, 90, low), near_low = unit(5, 50, low);
    const auto better = [&judge](const detail::CombatUnit& candidate, const Fixed candidate_rank,
        const detail::CombatUnit& held, const Fixed held_rank) { return judge.better(candidate, candidate_rank, &held, held_rank); };
    const Fixed good = whole(1), poor = whole(3);
    expect(better(far_low, poor, near_healthy, good),
        "LC-29: a low-health candidate beats a healthy held target on worse rank and distance");
    expect(!better(near_healthy, good, far_low, poor),
        "LC-29: a healthy candidate of better rank and distance does not replace a low-health held target");
    expect(better(near_low, good, far_low, poor) && !better(far_low, poor, near_low, good),
        "LC-29: with equal health the rank and then the distance decide");
    expect(better(near_low, poor, far_low, poor) && !better(far_low, poor, near_low, poor),
        "LC-29: with equal health and rank the strictly nearer candidate wins");
    expect(!better(near_low, poor, near_low, poor), "LC-29: equal distance keeps the held target");
    expect(!judge.better(near_healthy, std::nullopt, &far_healthy, good), "LC-29: a candidate with no rank is never chosen");
    expect(judge.better(near_healthy, poor, &far_healthy, std::nullopt) && judge.better(near_healthy, poor, nullptr, good),
        "LC-29: any ranked candidate beats a held target with no rank or none at all");
    const auto dying = unit(6, 50, tracked), healthy = unit(7, 50, full);
    expect(judge.sticky(dying), "WCC-15: a damage-tracked target that dies within the threshold is sticky");
    scanner_profile.stickiness = whole(10);
    expect(!judge.sticky(dying), "WCC-15: not when it outlives the threshold");
    scanner_profile.stickiness = whole(30);
    expect(!judge.sticky(healthy), "WCC-15: a target taking no damage is never sticky");
    health.damage_tracking = false;
    expect(!judge.sticky(dying), "WCC-15: a target that is not damage-tracked is never sticky");
}

void target_priorities_and_replacement() {
    sim::InlineExecutor executor;
    const auto run = [&executor](const Replacement& tried, const int ticks = 420) { return hold_trace(tried, executor, ticks); };
    using Trace = std::vector<EntityId>;
    Replacement plan;
    plan.infantry_x = 190; plan.tank_start_y = plan.tank_y = 230; // infantry 90 away, tank 130 away, both there at once
    const auto session_ids = [&plan]() {
        auto input = replacement_scenario(plan);
        auto session = take(TacticalSession::create(input.setup, input.content));
        return std::pair<EntityId, EntityId>{member_of(session, 2), member_of(session, 3)};
    };
    const auto ids = session_ids();
    const EntityId infantry_id = ids.first, tank = ids.second;
    // LC-28 at a scan with no held target: the set's rank decides before distance; an exclusion is no target.
    plan.infantry_priority = half_steps(4); plan.tank_priority = half_steps(3);
    expect(run(plan, 120) == Trace{tank}, "LC-28: a better-ranked tank 130 away beats a worse-ranked infantry 90 away");
    plan.infantry_priority = plan.tank_priority = half_steps(4);
    expect(run(plan, 120) == Trace{infantry_id}, "LC-29: equal rank falls to the strictly nearer candidate");
    plan.tank_priority.reset(); plan.tank_start_y = plan.tank_y = 160; // excluded, and nearer than the infantry
    expect(run(plan, 120) == Trace{infantry_id}, "LC-28: a candidate the set excludes is never chosen");
    plan.infantry_priority.reset();
    expect(run(plan, 120).empty(), "LC-28: with every candidate excluded there is no target");

    // The infantry is held at rank 1.5, 130 away; the enemy tank then walks into range.
    Replacement walk;
    walk.infantry_priority = half_steps(3); walk.tank_priority = half_steps(4);
    expect(run(walk) == Trace{infantry_id, tank}, "LC-29: a worse-ranked candidate that ends strictly nearer replaces the held target");
    walk.tank_y = 245; // 145 away
    expect(run(walk) == Trace{infantry_id}, "LC-29: a worse-ranked candidate that is not nearer leaves the held target");
    walk.infantry_priority = half_steps(4); walk.tank_priority = half_steps(2);
    expect(run(walk) == Trace{infantry_id, tank}, "LC-29: a strictly better-ranked candidate replaces the held target, though farther");
    walk.infantry_priority = half_steps(3); walk.tank_priority = half_steps(4);
    walk.infantry_unhittable = true;
    expect(run(walk) == Trace{infantry_id, tank}, "LC-29: a candidate the weapon can hit replaces one it cannot, though worse and farther");
    walk.infantry_unhittable = false; walk.attack_infantry = true; walk.tank_priority = half_steps(2); walk.tank_y = 190;
    expect(run(walk) == Trace{infantry_id}, "LC-29: an ordered target is protected from a better-ranked, nearer candidate");
    // WCC-14: a held target at rank exactly 1.0 is kept without any scan.
    walk.attack_infantry = false; walk.infantry_priority = half_steps(2); walk.tank_priority = half_steps(1);
    expect(run(walk) == Trace{infantry_id}, "WCC-14: a held target at rank exactly 1.0 is not replaced");

    // A held target that leaves attack range is dropped, as before this slice (range stands in for the chase and the
    // fog clear that are not hosted): a rank 1.0 infantry walks away while the rank 2.0 tank stands in range.
    Replacement leave;
    leave.infantry_x = 190; leave.tank_start_y = leave.tank_y = 230; leave.leave_tick = 60;
    leave.infantry_priority = half_steps(2); leave.tank_priority = half_steps(4);
    EntityId last = invalid_entity_id;
    expect(hold_trace(leave, executor, 420, nullptr, false, &last) == Trace{infantry_id, tank} && last == tank,
        "LC-29: a held target that walks out of range is dropped and the squad turns to the target in range");
    // A held target that dies is replaced by whatever is in range, whatever its rank; with nothing in range the squad
    // holds nothing.
    Replacement dies;
    dies.infantry_x = 190; dies.tank_start_y = dies.tank_y = 230; dies.fragile_tank = true;
    dies.infantry_priority = half_steps(4); dies.tank_priority = half_steps(3);
    last = tank;
    expect(hold_trace(dies, executor, 300, nullptr, false, &last) == Trace{tank, infantry_id} && last == infantry_id,
        "LC-29: a held target that dies is replaced by the candidate in range, though it ranks worse");
    dies.infantry_x = 400;
    expect(hold_trace(dies, executor, 300, nullptr, false, &last) == Trace{tank} && last == invalid_entity_id,
        "LC-29: a held target that dies with nothing else in range leaves the squad without a target");
    comparator_health_and_stickiness();
}

void replacement_deterministic() {
    Replacement plan;
    plan.infantry_priority = half_steps(3); plan.tank_priority = half_steps(4);
    sim::InlineExecutor serial;
    Fight baseline;
    const auto expected = hold_trace(plan, serial, 300, &baseline);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        test_support::ReverseOrderExecutor reverse(workers);
        for (const auto* runner : std::array<const sim::PartitionExecutor*, 2>{&executor, &reverse}) {
            Fight candidate;
            const auto trace = hold_trace(plan, *runner, 300, &candidate, true);
            expect(trace == expected && candidate.hashes == baseline.hashes && candidate.final == baseline.final,
                "LC-29 replacement is equal on 1/2/4/8 workers and reverse partitions, including a checkpoint restore");
        }
    }
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

// LC-90/LC-91 (legacy #5898, fog PR B): land targeting honours the fog. A tank (player 1, body 22) reveals a circle of
// the planned range; the infantry (player 2, body 12) has no sensor row, so it sees only what a shot of the tank
// shows it. Hulls are large so nothing dies; the fog alone decides who may fire.
struct FogPlan {
    std::int64_t reveal{130};
    bool endless{}; // the tank fires every second at range 300 instead of every 15 seconds at range 150
};
Scenario fog_scenario(const FogPlan& plan) {
    auto input = scenario(true, 1);
    const auto field = make_field(std::vector<std::string>(64, std::string(64, '.')));
    auto& content = input.content;
    content.land_terrain = field.terrain;
    content.fog = take(skirmish::land_fog_rules(field.terrain->width(), field.terrain->height(), field.terrain->spacing(),
        whole(15), whole(6)));
    SensorProfile sensor;
    sensor.type_id = 22; sensor.reveal_range = whole(plan.reveal); sensor.flash_radius = whole(30);
    content.sensors.push_back(sensor);
    for (auto& health : content.durability.profiles) health.max_hull = whole(100000);
    auto& tank = content.combat.profiles[1];
    auto& weapon = tank.weapons[0];
    weapon.pulse_count = 1;
    if (plan.endless) {
        tank.max_attack_distance = whole(300); weapon.range = whole(300);
        weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 100;
    } else {
        // A new weapon waits a random part of its recharge before the first shot, so a slow weapon fires first at
        // some tick in [0, 480] and then every 480 ticks: long gaps in which no reveal of a shot may remain.
        weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 1500;
    }
    return input;
}

double planar_distance(const UnitState& a, const UnitState& b) {
    const auto dx = static_cast<double>(a.position.x.raw() - b.position.x.raw()) / static_cast<double>(Fixed::scale);
    const auto dy = static_cast<double>(a.position.y.raw() - b.position.y.raw()) / static_cast<double>(Fixed::scale);
    return std::sqrt(dx * dx + dy * dy);
}

struct FogRun {
    std::vector<std::string> hashes; Bytes final;
    std::vector<int> tank_shots, infantry_shots; // the tick of each shot
    std::vector<double> tank_shot_distance;      // the distance between the bodies on the tick of each tank shot
    double last_distance{};
};
FogRun fog_run(const Scenario& input, const sim::PartitionExecutor& executor, const bool resumes, const bool ordered,
    const bool retreats, const int ticks) {
    auto session = take(TacticalSession::create(input.setup, input.content));
    if (ordered) must(session.submit({{0, 2, 1}, {2}, AttackPayload{1}}));
    if (retreats) must(session.submit({{0, 2, 2}, {2}, MovePayload{{whole(300), whole(100), {}}, false, false}}));
    const auto tank = member_of(session, 1), infantry_id = member_of(session, 2);
    FogRun result;
    for (int tick = 0; tick < ticks; ++tick) {
        const auto output = take(session.step(executor));
        result.hashes.push_back(output.state_sha256);
        const auto tank_state = session.unit_state(tank), infantry_state = session.unit_state(infantry_id);
        const auto gap = tank_state && infantry_state ? planar_distance(*tank_state, *infantry_state) : 0.0;
        result.last_distance = gap;
        for (const auto& event : output.snapshot->combat_events()) {
            if (event.kind != CombatEventKind::weapon_fired) continue;
            if (event.shooter == tank) { result.tank_shots.push_back(tick); result.tank_shot_distance.push_back(gap); }
            else if (event.shooter == infantry_id) result.infantry_shots.push_back(tick);
        }
        if (resumes && tick == 120) session = restore(session, input.content, executor);
    }
    result.final = save(session);
    return result;
}

// LC-90: the suitability clause itself, apart from the weapon's own visibility gate. A tank (player 1, no sensor row)
// faces two unarmed enemies: A is nearer and fogged, B is farther and inside the reveal circle of a player 1
// spotter (infantry, range 60). Without the clause the squad picks (or keeps the order on) A and never fires,
// because the weapon refuses a fogged target; with it the squad passes A over and fires at B.
struct TwoEnemies { std::vector<EntityId> targets; EntityId a{}, b{}; };
TwoEnemies two_enemy_fight(const bool ordered, const sim::PartitionExecutor& executor) {
    auto input = fog_scenario({60, true});
    input.content.sensors.clear();
    SensorProfile spotter;
    spotter.type_id = 12; spotter.reveal_range = whole(60);
    input.content.sensors.push_back(spotter);
    input.content.combat.profiles[0].weapons.clear();
    UnitState friend_spotter = input.setup.units[1];
    friend_spotter.entity_id = 3; friend_spotter.owner = 1; friend_spotter.position.x = whole(290);
    UnitState far_enemy = input.setup.units[1];
    far_enemy.entity_id = 4; far_enemy.position.x = whole(240);
    input.setup.units.push_back(friend_spotter);
    input.setup.units.push_back(far_enemy);
    auto session = take(TacticalSession::create(input.setup, input.content));
    if (ordered) must(session.submit({{0, 1, 1}, {1}, AttackPayload{2}}));
    TwoEnemies result;
    result.a = member_of(session, 2); result.b = member_of(session, 4);
    for (int tick = 0; tick < 100; ++tick) {
        const auto output = take(session.step(executor));
        for (const auto& event : output.snapshot->combat_events())
            if (event.kind == CombatEventKind::weapon_fired) result.targets.push_back(event.target);
    }
    return result;
}

void fog_two_enemies() {
    sim::InlineExecutor serial;
    for (const bool ordered : {false, true}) {
        const auto fight_result = two_enemy_fight(ordered, serial);
        expect(!fight_result.targets.empty() && std::all_of(fight_result.targets.begin(), fight_result.targets.end(),
                [&](const EntityId target) { return target == fight_result.b; }),
            ordered ? "LC-90: an ordered target that is fogged is passed over for a visible enemy"
                    : "LC-90: a nearer fogged enemy is not held; the squad fires at the visible one");
    }
}

void fog_targeting() {
    sim::InlineExecutor serial;
    // R6: an enemy inside weapon range but outside every reveal circle is not suitable, ordered or not.
    for (const bool ordered : {false, true}) {
        const auto blind = fog_run(fog_scenario({40, false}), serial, false, ordered, false, 1100);
        expect(blind.tank_shots.empty(), "LC-90: an enemy inside weapon range but outside every reveal circle is not acquired");
        expect(blind.infantry_shots.empty(), ordered
            ? "LC-90: an attack order on a fogged unit does not fire at it"
            : "LC-90: a unit that sees nothing acquires nothing");
    }
    // The same fight with the circle covering the enemy: the tank acquires and fires.
    const auto covered = fog_run(fog_scenario({130, false}), serial, false, false, false, 1100);
    expect(!covered.tank_shots.empty(), "LC-90: an enemy inside a reveal circle is acquired and fired at");
    // R9 and R7: the shot shows the tank to the infantry's owner; the infantry fires back while the reveal lasts,
    // never before it, and drops its held target once the reveal has regrown to fog.
    for (const bool ordered : {false, true}) {
        const auto seen = fog_run(fog_scenario({130, false}), serial, false, ordered, false, 1300);
        expect(!seen.tank_shots.empty(), "LC-91: the revealing tank fires");
        if (seen.tank_shots.empty()) continue;
        expect(!seen.infantry_shots.empty() && seen.infantry_shots.front() > seen.tank_shots.front(),
            ordered ? "LC-91: an ordered attacker fires at its shooter only after the shot's reveal, not before"
                    : "LC-91: a unit with no sensor acquires its shooter after the shot's reveal, not before");
        // Every infantry shot follows a tank shot by less than 300 ticks (the reveal regrows in about 210): between
        // the tank's shots the reveal is gone and the held (or ordered) target is dropped with it.
        bool shown = true;
        for (const auto tick : seen.infantry_shots) {
            bool recent = false;
            for (const auto shot : seen.tank_shots) recent = recent || (shot < tick && tick - shot < 300);
            shown = shown && recent;
        }
        expect(!seen.infantry_shots.empty() && shown,
            "LC-90: the shot reveal regrows to fog and the held (or ordered) target is dropped with it");
    }
    // R7: a held target that leaves the circle is dropped at once, although it stays inside weapon range.
    const auto retreat = fog_run(fog_scenario({130, true}), serial, false, false, true, 500);
    expect(!retreat.tank_shots.empty() && retreat.last_distance > 190,
        "LC-90: the retreating enemy ends well outside the reveal circle and inside the tank's range of 300");
    expect(std::all_of(retreat.tank_shot_distance.begin(), retreat.tank_shot_distance.end(),
            [](const double shot_gap) { return shot_gap <= 160.0; }),
        "LC-90: the tank never fires at a target beyond its reveal circle (circle cell edge included)");
    // Equal on 1/2/4/8 workers and under reverse partitions, with a checkpoint restore in the middle.
    const auto reference_shot = fog_run(fog_scenario({130, false}), serial, false, true, false, 800);
    const auto reference_retreat = fog_run(fog_scenario({130, true}), serial, false, false, true, 400);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        test_support::ReverseOrderExecutor reverse(workers);
        for (const auto* runner : std::array<const sim::PartitionExecutor*, 2>{&threads, &reverse}) {
            const auto shot = fog_run(fog_scenario({130, false}), *runner, true, true, false, 800);
            expect(shot.hashes == reference_shot.hashes && shot.final == reference_shot.final
                    && shot.infantry_shots == reference_shot.infantry_shots,
                "LC-90/91: the shot reveal fight is equal on 1/2/4/8 workers and reverse partitions, with a restore");
            const auto moving = fog_run(fog_scenario({130, true}), *runner, true, false, true, 400);
            expect(moving.hashes == reference_retreat.hashes && moving.final == reference_retreat.final
                    && moving.tank_shots == reference_retreat.tank_shots,
                "LC-90: the retreat fight is equal on 1/2/4/8 workers and reverse partitions, with a restore");
        }
    }
}

double process_cpu_ms() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    const auto ticks = [](const FILETIME& t) { return static_cast<double>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime); };
    return (ticks(kernel) + ticks(user)) / 10000.0;
#else
    timespec now{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    return static_cast<double>(now.tv_sec) * 1000.0 + static_cast<double>(now.tv_nsec) / 1.0e6;
#endif
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
    // Production hashes on the platform hasher thread; EAWR_STRESS_SERIAL_HASH=1 measures the in-tick hash instead.
    if (const auto serial = testing::environment("EAWR_STRESS_SERIAL_HASH"); !serial || serial->empty())
        session.set_state_hasher(std::make_shared<platform::ThreadStateHasher>());
    platform::ThreadWorkerAdapter executor(6);
    for (int i = 0; i < 12; ++i) take(session.step(executor));
    core::tick_profile::Recording recording;
    const auto cpu_start = process_cpu_ms();
    const auto start = std::chrono::steady_clock::now();
    {
        core::tick_profile::Capture capture(recording);
        for (int i = 0; i < 48; ++i) take(session.step(executor));
    }
    const auto wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const auto cpu_ms = process_cpu_ms() - cpu_start;
    const auto ms = wall_ms / 48;
    const auto logical = std::max(1U, std::thread::hardware_concurrency());
    std::cout << "land combat stress objects=880 workers=6 ms/tick=" << ms << " budget=16.7\n";
    std::cout << "land combat stress cpu%=" << (wall_ms > 0 ? 100.0 * cpu_ms / (wall_ms * logical) : 0.0)
              << " logical=" << logical << " barriers ms/tick=" << recording.barriers_ms / 48 << '\n';
    // Compact per-phase summary: serial (exclusive) cost per tick, largest first, phases above 0.1 ms.
    std::map<std::string, double> phases;
    for (const auto& cost : recording.costs) if (!cost.name.empty()) phases[std::string(cost.name)] += cost.ms;
    std::vector<std::pair<std::string, double>> ordered(phases.begin(), phases.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [name, total] : ordered) if (total / 48 >= 0.1) std::cout << name << " ms/tick=" << total / 48 << '\n';
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
    content.land.static_objects.clear(); content.land.occupations.clear(); content.land.initial_pad_children.clear();
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
        // Diagnostic trace of the two bodies, so a red run shows where the walk stopped.
        if (i % 240 == 0 || i == 60 || i == 120) {
            for (const auto& squad : session.land_movement()->squads()) {
                const auto scale = static_cast<double>(Fixed::scale);
                std::cout << "stock combat tick=" << i << " squad=" << squad.object << " pos=" << static_cast<double>(squad.position.x.raw()) / scale
                          << "," << static_cast<double>(squad.position.y.raw()) / scale << " facing=" << static_cast<double>(squad.facing.raw()) / scale
                          << " speed=" << static_cast<double>(squad.speed.raw()) / scale << " order=" << static_cast<int>(squad.order)
                          << " node=" << squad.next_node << "/" << squad.path.nodes.size() << " turning=" << squad.turning
                          << " progress=" << squad.progress << " contacts=" << session.land_movement()->contacts().size()
                          << " tmp=" << squad.temporary_next << "/" << squad.temporary_path.nodes.size() << " tmp_first=" << squad.temporary_first
                          << " assigned=" << static_cast<double>(squad.assigned_speed.raw()) / scale
                          << " collision=" << static_cast<double>(squad.collision_speed.raw()) / scale
                          << " straggler_wait=" << static_cast<int>(squad.straggler_wait_remaining) << '\n';
            }
            for (const auto& member : session.land_movement()->members()) {
                const auto scale = static_cast<double>(Fixed::scale);
                std::cout << "stock combat tick=" << i << " member=" << member.object << " squad=" << member.squad << " type=" << member.source.type
                          << " state=" << static_cast<int>(member.state) << " speed=" << static_cast<double>(member.speed.raw()) / scale
                          << " walk_transition=" << member.profile.walk_transition << " clock=" << member.profile.start_clock
                          << " clip_ticks=" << member.profile.start_clip_ticks
                          << " frame=" << static_cast<double>(member.transition_frame.raw()) / scale << '\n';
            }
            for (const auto& c : session.land_movement()->contacts())
                std::cout << "stock combat tick=" << i << " contact " << c.first << "/" << c.second << " kind=" << static_cast<int>(c.kind)
                          << " state=" << static_cast<int>(c.state) << " search=" << static_cast<int>(c.search) << " route=" << static_cast<int>(c.route)
                          << " stopped=" << c.first_stopped << "/" << c.second_stopped << " installed=" << c.installed
                          << " retry=" << static_cast<int>(c.retry_remaining) << " deadlock=" << static_cast<int>(c.deadlock_remaining) << '\n';
        }
    }
    expect(moved && shots[0] && shots[1] && died, "two stock companies on Naboo walk into range, exchange authored fire and lose a member");
    std::cout << "stock combat map=" << fixture.map << " moved=" << moved << " both-fire=" << (shots[0] && shots[1]) << " death=" << died << '\n';
    return checks.finish();
}
}

// #6145 (LR13-17): a stock walker (AT-ST, Walk_Transition) ordered from a standstill must leave its place. Nothing published
// the start-clip gate in a live session, so such a unit stood for ever after its first order. The Empire side is given the
// stock "Imperial_Heavy_Scout_Squad" (four AT-ST walkers, one object per squad); the Rebel side only supplies the witness
// the walker is sent towards.
int installed_walker() {
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
    auto discovered = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, {}));
    fixture.map_sha256 = discovered.common.map_sha256;
    const std::string walkers = "Imperial_Heavy_Scout_Squad";
    for (auto& faction : discovered.factions) if (faction.faction == "Empire") faction.forces = {walkers};
    units::LoadInput load;
    load.domain = BattleDomain::land; load.catalog = &loaded.catalog; load.filesystem = &filesystem; load.model = cache.access().model;
    std::set<std::string> requested;
    for (const auto& faction : discovered.factions) requested.insert(faction.forces.begin(), faction.forces.end());
    load.types.assign(requested.begin(), requested.end());
    auto tables = take(units::load_unit_tables(load));
    for (const auto& company : tables.land_companies) for (const auto& member : company.members) requested.insert(member.type);
    load.types.assign(requested.begin(), requested.end()); tables = take(units::load_unit_tables(load));
    auto inputs = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, tables));
    for (auto& faction : inputs.factions) if (faction.faction == "Empire") { faction.forces = {walkers}; faction.forces_authored = true; }
    auto gates = take(skirmish::authored_land_content_inputs(tables));
    gates.last_known_capacity = {{1, 50}, {2, 50}};
    const auto built = take(skirmish::build_land_start(fixture, inputs, gates));
    auto initial = built.start.setup;
    // The walker: an Empire unit whose company has a Walk_Transition member whose start clock the adapter filled.
    const auto walker_row = std::find_if(built.content.land.companies.begin(), built.content.land.companies.end(), [&](const auto& row) {
        return !row.members.empty() && std::all_of(row.members.begin(), row.members.end(), [](const auto& member) {
            return member.locomotion.walk_transition && member.locomotion.start_clock; });
    });
    if (walker_row == built.content.land.companies.end()) throw std::runtime_error("the stock walker company has no Walk_Transition start clock");
    const auto walker_at = std::find_if(initial.units.begin(), initial.units.end(), [&](const auto& unit) {
        return unit.owner == 2 && unit.type_id == walker_row->type; });
    // The witness must be a company too (the static roots were cleared above, so a unit without a company binding would be refused).
    const auto witness = std::find_if(initial.units.begin(), initial.units.end(), [&](const auto& unit) {
        return unit.owner == 1 && std::any_of(built.content.land.companies.begin(), built.content.land.companies.end(),
            [&](const auto& row) { return row.type == unit.type_id; }); });
    if (walker_at == initial.units.end() || witness == initial.units.end()) throw std::runtime_error("the start has no walker or no witness");
    const auto walker_unit = *walker_at, witness_unit = *witness;
    auto content = built.content;
    content.land.static_objects.clear(); content.land.occupations.clear(); content.land.initial_pad_children.clear();
    initial.units = {walker_unit, witness_unit};
    // The session wants the units in ascending entity ID order.
    std::sort(initial.units.begin(), initial.units.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
    const sim::math::Vec3 target{Fixed::from_raw(walker_unit.position.x.raw() / 2 + witness_unit.position.x.raw() / 2),
        Fixed::from_raw(walker_unit.position.y.raw() / 2 + witness_unit.position.y.raw() / 2), {}};
    auto session = take(TacticalSession::create(initial, content));
    must(session.submit({{0, walker_unit.owner, 1}, {walker_unit.entity_id}, MovePayload{target}}));
    platform::ThreadWorkerAdapter executor(4);
    const auto squad_index = [&]() -> std::size_t {
        const auto& squads = session.land_movement()->squads();
        for (std::size_t i = 0; i < squads.size(); ++i) if (squads[i].object == walker_unit.entity_id) return i;
        throw std::runtime_error("the walker has no movement squad");
    }();
    const auto lead_index = [&]() -> std::size_t {
        const auto& members = session.land_movement()->members();
        for (std::size_t i = 0; i < members.size(); ++i) if (members[i].squad == squad_index) return i;
        throw std::runtime_error("the walker squad has no member");
    }();
    const auto start = session.land_movement()->squads()[squad_index].position;
    int first_unit = 0, first_speed = 0, first_starting = 0, first_moving = 0;
    double travelled = 0;
    std::uint32_t wait_ticks = 0;
    for (const auto& member : session.land_movement()->members())
        if (member.profile.walk_transition) wait_ticks = std::max(wait_ticks, member.profile.start_clip_ticks);
    for (int i = 1; i <= 900; ++i) {
        static_cast<void>(take(session.step(executor)));
        const auto& squad = session.land_movement()->squads()[squad_index];
        const auto& lead = session.land_movement()->members()[lead_index];
        const double dx = static_cast<double>(squad.position.x.raw() - start.x.raw()) / static_cast<double>(Fixed::scale);
        const double dy = static_cast<double>(squad.position.y.raw() - start.y.raw()) / static_cast<double>(Fixed::scale);
        travelled = std::max(travelled, std::hypot(dx, dy));
        if (first_unit == 0 && travelled >= 1.0) first_unit = i;
        if (first_speed == 0 && squad.speed > Fixed{}) first_speed = i;
        if (first_starting == 0 && lead.state == sim::tactical::LandWalkState::starting) first_starting = i;
        if (first_moving == 0 && lead.state == sim::tactical::LandWalkState::moving) first_moving = i;
    }
    // One short line: the owner's test output keeps only 1 KB per test.
    std::cout << "stock walker ticks: wait=" << wait_ticks << " starting=" << first_starting << " moving=" << first_moving
              << " speed=" << first_speed << " one_unit=" << first_unit << " travelled=" << static_cast<int>(travelled) << '\n';
    for (const auto& company : tables.land_companies) {
        if (company.id != walker_row->company.id || company.locomotion.empty() || company.members.empty()) continue;
        const auto& loco = company.locomotion.front();
        std::cout << "stock walker clip read=" << loco.start_clip_read << " frames=" << loco.start_clip_frames
                  << " fps=" << static_cast<double>(loco.start_clip_fps.raw()) / static_cast<double>(Fixed::scale)
                  << " max_speed=" << (company.members.front().max_speed ? static_cast<double>(company.members.front().max_speed->raw()) / static_cast<double>(Fixed::scale) : -1.0)
                  << " movement_animation_speed=" << (loco.movement_animation_speed ? static_cast<double>(loco.movement_animation_speed->raw()) / static_cast<double>(Fixed::scale) : -1.0) << '\n';
    }
    expect(first_unit > 0 && first_unit <= static_cast<int>(wait_ticks) + 90, "a stock walker leaves its place once its start clip has played");
    expect(wait_ticks > 0 && first_starting > 0 && first_moving - first_starting >= static_cast<int>(wait_ticks),
        "the stock AT-ST waits its MOVESTART clip in starting before it walks");
    expect(travelled > 60.0, "a stock walker ordered across the stock map walks tens of units, it does not stand for ever");
    return checks.finish();
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
        if (argc == 2 && std::string_view(argv[1]) == "--installed-walker") return installed_walker();
        if (argc == 2 && std::string_view(argv[1]) == "--stress") return stress();
        terrain_contacts(); range_policy(); ground_blast_once(); retained_contact_death(); direct_body_wrapper(); commands_and_armor(); target_priorities_and_replacement(); replacement_deterministic(); fog_targeting(); fog_two_enemies(); deterministic(); return checks.finish();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
