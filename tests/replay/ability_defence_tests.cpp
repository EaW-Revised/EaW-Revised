#include "ability_fixtures.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ability_tests {

namespace {


// --- LASER_DEFENSE (WLD-01 to WLD-12, the Crusader's point defence) --------------------------------

constexpr tactical::TypeId missile_shooter_type = 14;

constexpr tactical::TypeId crusader_type = 15;


// The Crusader's authored data (units_space_underworld_crusader_gunship.xml): LASER_DEFENSE with a 60 s
// recharge, and the nested handler's 35 s run, 500 reach, 100 minimum flight, 0.5 s between looks.
[[nodiscard]] tactical::AbilityProfile laser_defence_ability() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::laser_defense;
    profile.expiration_frames = 1050;
    profile.recharge_frames = 1800;
    profile.recharge_at_activation = true;
    profile.gui_activated_ability_name = "Crusader_Laser_Defense";
    return profile;
}


[[nodiscard]] tactical::SpecialAbilityProfile laser_defence_handler() {
    tactical::SpecialAbilityProfile handler;
    handler.name = "Crusader_Laser_Defense";
    handler.kind = tactical::SpecialAbilityKind::laser_defense;
    handler.style = tactical::SpecialActivationStyle::user_input;
    handler.service_interval = 1;
    handler.defence_duration_frames = 1050;
    handler.defence_look_frames = 15;
    handler.defence_beam_frames = 2;
    handler.defence_radius = units(500);
    handler.defence_min_travel = units(100);
    handler.defence_categories = tactical::defence_category_missile | tactical::defence_category_rocket
        | tactical::defence_category_mptl_rocket;
    return handler;
}


[[nodiscard]] tactical::AbilityTable laser_defence_abilities() {
    tactical::AbilityTable table;
    table.profiles = {{crusader_type, {laser_defence_ability()}, false, {laser_defence_handler()}}};
    return table;
}


// A shooter at the origin with one concussion-missile-like weapon: a missile a second at 7 units a frame.
[[nodiscard]] tactical::CombatTable laser_defence_combat(const bool homing) {
    tactical::CombatTable table;
    const tactical::CollisionBox box{at(-20, -10, -10), at(20, 10, 10)};
    tactical::WeaponProfile weapon;
    weapon.hardpoint = 0;
    weapon.range = units(1400);
    weapon.min_recharge_hundredths = 100;
    weapon.max_recharge_hundredths = 100;
    weapon.pulse_count = 1;
    weapon.cone_width = units(175);
    weapon.cone_height = units(160);
    weapon.opportunity_when_idle = true;
    weapon.opportunity_when_targeting = true;
    weapon.fire_a = at(20, 0);
    weapon.shot = tactical::ShotProfile{units(10), 0, units(7), units(1400), true, true, {}};
    weapon.shot->homing = homing;
    weapon.shot->turn_rate = homing ? units(3) : Fixed{};
    tactical::CombatProfile shooter;
    shooter.type_id = missile_shooter_type;
    shooter.category_bits = 1;
    shooter.max_attack_distance = units(1400);
    shooter.weapons = {weapon};
    shooter.collision = box;
    tactical::CombatProfile crusader;
    crusader.type_id = crusader_type;
    crusader.category_bits = 2;
    crusader.collision = box;
    table.profiles = {shooter, crusader};
    return table;
}


[[nodiscard]] tactical::DurabilityTable laser_defence_durability() {
    auto table = durability();
    auto profile = table.profiles.front();
    for (const auto type : {missile_shooter_type, crusader_type}) {
        profile.type_id = type;
        table.profiles.push_back(profile);
    }
    std::sort(table.profiles.begin(), table.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    return table;
}


[[nodiscard]] std::vector<tactical::SensorProfile> laser_defence_sensors() {
    return {{missile_shooter_type, units(3000)}, {crusader_type, units(3000)}};
}


// Unit 1 (player 1) fires missiles at unit 2 (player 2, a Crusader `distance` away); unit 3 is a second
// Crusader of the shooter's owner, close to the missile path.
[[nodiscard]] tactical::TacticalSetup laser_defence_setup(const std::int64_t distance) {
    tactical::TacticalSetup result;
    result.seed = 1876;
    result.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    result.units = {
        {1, missile_shooter_type, 1, at(0, 0), math::identity_quat(), {}},
        {2, crusader_type, 2, at(distance, 0), math::identity_quat(), {}},
        {3, crusader_type, 1, at(distance / 2, 90), math::identity_quat(), {}},
    };
    return result;
}


struct LaserRun {
    std::vector<std::string> hashes;
    std::vector<tactical::CombatEvent> zaps, expired, hits;
    std::vector<tactical::Event> events;
    std::vector<tactical::AbilityStatus> status; // unit 2's laser status after each tick
    std::optional<tactical::TacticalSession> session;
    std::optional<tactical::TacticalReplay> replay;
};


[[nodiscard]] LaserRun laser_run(const std::vector<tactical::PlayerCommand>& commands, const std::int64_t distance,
    const std::size_t workers, const std::uint64_t ticks, const bool homing = true) {
    LaserRun result;
    auto created = tactical::TacticalSession::create(laser_defence_setup(distance), laser_defence_sensors(),
        laser_defence_durability(), motion(), std::nullopt, laser_defence_combat(homing), {}, laser_defence_abilities());
    expect(static_cast<bool>(created), "the laser defence session is created");
    if (!created) return result;
    auto session = std::move(created).value();
    for (const auto& entry : commands) expect(static_cast<bool>(session.submit(entry)), "a laser defence command is accepted");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "a laser defence step succeeds");
        if (!stepped) break;
        result.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->events()) result.events.push_back(event);
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.kind == tactical::CombatEventKind::point_defence_zap) result.zaps.push_back(event);
            if (event.kind == tactical::CombatEventKind::projectile_expired) result.expired.push_back(event);
            if (event.kind == tactical::CombatEventKind::projectile_hit && event.target == 2) result.hits.push_back(event);
        }
        for (const auto& instance : stepped.value().snapshot->instances()) {
            if (instance.entity_id != 2) continue;
            for (const auto& status : instance.abilities) {
                if (status.kind == tactical::AbilityKind::laser_defense) result.status.push_back(status);
            }
        }
    }
    result.replay = session.record();
    result.session.emplace(std::move(session));
    return result;
}


[[nodiscard]] tactical::AbilityPayload laser(const tactical::AbilityAction action) {
    return {tactical::AbilityKind::laser_defense, action};
}



// WSD-01..10: SELF_DESTRUCT. The countdown runs 105 frames (Expiration_Seconds 3.5), the unit then leaves the world
// with its blast object (900 damage within 500, enemies only, the next frame), a cancel recharges for the share of
// the countdown that ran, and the Krayt object script's triggers are served for the AI and for a human's autofire.
constexpr tactical::TypeId blast_type = 0x5D5D5D5D5D5D5D5DULL;


[[nodiscard]] tactical::AbilityProfile self_destruct_ability() {
    tactical::AbilityProfile ability;
    ability.kind = tactical::AbilityKind::self_destruct;
    ability.expiration_frames = 105;
    ability.recharge_frames = 300;
    ability.supports_autofire = true;
    ability.spawned = tactical::SpawnedAbilityProfile{};
    ability.spawned->type = blast_type;
    ability.spawned->damage_type = 0;
    ability.spawned->blast.damage = units(900);
    ability.spawned->blast.radius = units(500);
    ability.spawned->blast.max_delay = {};
    return ability;
}


struct SelfDestructWorld {
    tactical::TacticalSetup start;
    tactical::DurabilityTable health;
    tactical::CombatTable combat;
    tactical::AbilityTable table;
    std::vector<tactical::SensorProfile> sensors;
};


// Unit 1 (plain type, player 1) holds the ability; 2 is a player-2 frigate in reach, 3 a player-2 corvette out of reach
// and 4 a corvette of the holder's own player.
[[nodiscard]] SelfDestructWorld self_destruct_world(const bool script, const std::vector<tactical::PlayerId>& humans) {
    SelfDestructWorld world;
    world.start = setup();
    world.start.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}},
        {2, frigate_type, 2, at(300, 0), math::identity_quat(), {}},
        {3, corvette_type, 2, at(800, 0), math::identity_quat(), {}},
        {4, corvette_type, 1, at(100, 0), math::identity_quat(), {}}};
    world.health = durability();
    for (auto& row : world.health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
    for (const auto type : {corvette_type, frigate_type, plain_type}) {
        tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = 2;
        profile.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
        world.combat.profiles.push_back(profile);
        world.sensors.push_back({type, units(1000)});
    }
    tactical::UnitAbilityProfile holder{plain_type, {self_destruct_ability()}, false};
    holder.self_destruct_script = script;
    world.table.profiles = {holder};
    world.table.humans = humans;
    world.table.autofire_defaults = humans;
    return world;
}


struct SelfDestructRun {
    std::vector<std::string> hashes;
    std::vector<bool> alive;                 // unit 1 after each tick
    std::vector<bool> active;                // its SELF_DESTRUCT is on after each tick
    std::vector<Fixed> enemy, far, ally;     // hulls of units 2, 3, 4 after each tick
    std::vector<Fixed> holder;               // hull of unit 1 after each tick
    std::vector<std::uint64_t> services;     // the ticks unit 1's Krayt script ran (WSD-09)
    std::vector<std::size_t> spawns;         // ability_spawns() per tick
    std::vector<tactical::Event> events;
    std::optional<tactical::TacticalSession> session;
};


[[nodiscard]] SelfDestructRun run_self_destruct(const SelfDestructWorld& world,
    const std::vector<tactical::PlayerCommand>& commands, const std::size_t workers, const std::uint64_t ticks) {
    SelfDestructRun result;
    auto created = tactical::TacticalSession::create(world.start, world.sensors, world.health, {}, std::nullopt,
        world.combat, {}, world.table);
    expect(static_cast<bool>(created), "WSD-01: the self-destruct session is created");
    if (!created) { std::cerr << created.error().message << '\n'; return result; }
    auto session = std::move(created).value();
    for (const auto& entry : commands) expect(static_cast<bool>(session.submit(entry)), "WSD-01: a command is accepted");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    const auto hull = [&](const eawr::sim::EntityId id) {
        const auto state = session.durability_state(id);
        return state ? state->hull : Fixed{};
    };
    std::optional<std::int64_t> last_service;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "WSD-01: a step succeeds");
        if (!stepped) break;
        result.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->events()) result.events.push_back(event);
        const auto live = session.units();
        result.alive.push_back(std::any_of(live.begin(), live.end(), [](const auto& unit) { return unit.entity_id == 1; }));
        const auto abilities_of_holder = session.ability_state(1);
        result.active.push_back(abilities_of_holder && abilities_of_holder->slots[0].active);
        result.enemy.push_back(hull(2));
        result.far.push_back(hull(3));
        result.ally.push_back(hull(4));
        result.holder.push_back(hull(1));
        const auto script = abilities_of_holder ? abilities_of_holder->self_destruct_script : std::nullopt;
        if (script && (!last_service || *last_service != script->last_service)) result.services.push_back(tick);
        if (script) last_service = script->last_service;
        result.spawns.push_back(stepped.value().snapshot->ability_spawns().size());
    }
    result.session.emplace(std::move(session));
    return result;
}


// --- REDIRECT_BLASTER (WRB-01 to WRB-12, the Underworld armour upgrades' deflection) ----------------

constexpr tactical::TypeId blaster_type = 16;

constexpr tactical::TypeId deflector_type = 17;

constexpr tactical::TypeId upgrade_station_type = 18;

constexpr tactical::TypeId armour_upgrade_type = 90;


struct RedirectScenario {
    bool initially_enabled{true};
    bool buy_upgrade{};
    bool upgrade_all_allies{};
    bool deflector_owned_by_ally{};   // the deflector belongs to a third player allied to the upgrade's holder
    const char* block{"1.0"};
    const char* redirect{"0.0"};
    const char* arc{"360"};
    bool blockable{true};
    std::uint64_t filter_categories{1};
    std::int64_t distance{400};
    bool duplicate{};
    bool delayed{};
    bool moving{};
    bool reflect_twice{};
};


[[nodiscard]] tactical::AbilityTable redirect_abilities(const RedirectScenario& scenario) {
    tactical::SpecialAbilityProfile handler;
    handler.name = "Magnetic_Armor_L2";
    handler.kind = tactical::SpecialAbilityKind::redirect_blaster;
    handler.style = tactical::SpecialActivationStyle::unspecified;
    if (!scenario.initially_enabled) handler.initially_enabled = false;
    handler.service_interval = 0;
    handler.redirect_chance = decimal(scenario.redirect);
    handler.block_chance = decimal(scenario.block);
    handler.reaction_arc = decimal(scenario.arc);
    handler.max_redirect_angle = decimal("30");
    handler.filter.applicable_categories = scenario.filter_categories;
    tactical::AbilityTable table;
    table.profiles = {{deflector_type, {}, false, {handler}}};
    if (scenario.duplicate) {
        handler.name = "Second_Magnetic_Armor";
        table.profiles[0].special.push_back(handler);
    }
    if (scenario.reflect_twice) {
        handler.filter.applicable_categories = 2;
        table.profiles.insert(table.profiles.begin(), {blaster_type, {}, false, {handler}});
    }
    return table;
}


[[nodiscard]] tactical::CombatTable redirect_combat(const RedirectScenario& scenario) {
    tactical::CombatTable table;
    const tactical::CollisionBox box{at(-20, -10, -10), at(20, 10, 10)};
    tactical::WeaponProfile weapon;
    weapon.hardpoint = 0;
    weapon.range = units(1400);
    weapon.min_recharge_hundredths = 100;
    weapon.max_recharge_hundredths = 100;
    weapon.pulse_count = 1;
    weapon.cone_width = units(175);
    weapon.cone_height = units(160);
    weapon.opportunity_when_idle = true;
    weapon.opportunity_when_targeting = true;
    weapon.fire_a = at(20, 0);
    weapon.shot = tactical::ShotProfile{units(10), 0, units(7), units(1400), true, true, {}};
    weapon.shot->redirect_blockable = scenario.blockable;
    if (scenario.delayed) weapon.shot->damage_delay = decimal("0.1");
    tactical::CombatProfile shooter;
    shooter.type_id = blaster_type;
    shooter.category_bits = 1;
    shooter.max_attack_distance = units(1400);
    shooter.weapons = {weapon};
    shooter.collision = box;
    tactical::CombatProfile deflector;
    deflector.type_id = deflector_type;
    deflector.category_bits = 2;
    deflector.collision = box;
    if (scenario.reflect_twice) {
        // A colliding slot is deliberately a different, nondeflectable projectile profile.
        weapon.opportunity_when_idle = weapon.opportunity_when_targeting = false;
        weapon.shot->speed = units(99);
        weapon.shot->redirect_blockable = false;
        deflector.weapons = {weapon};
    }
    tactical::CombatProfile station;
    station.type_id = upgrade_station_type;
    station.category_bits = 4;
    station.collision = box;
    table.profiles = {shooter, deflector, station};
    return table;
}


[[nodiscard]] tactical::DurabilityTable redirect_durability() {
    auto table = durability();
    auto profile = table.profiles.front();
    for (const auto type : {blaster_type, deflector_type, upgrade_station_type}) {
        profile.type_id = type;
        table.profiles.push_back(profile);
    }
    std::sort(table.profiles.begin(), table.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    return table;
}


struct RedirectRun {
    std::vector<std::string> hashes;
    std::uint64_t deflected{}, landed{}, fired_back{}, reflected_again{};
    bool fired_back_toward_shooter{};
    math::Fixed hull{};
    std::vector<std::string> events;
};


[[nodiscard]] RedirectRun redirect_run(const RedirectScenario& scenario, const std::size_t workers, const std::uint64_t ticks) {
    RedirectRun result;
    tactical::TacticalSetup initial;
    initial.seed = 1876;
    initial.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable},
        {3, 2, 2, tactical::player_flag_commandable}};
    const tactical::PlayerId owner = scenario.deflector_owned_by_ally ? 3 : 2;
    initial.units = {
        {1, blaster_type, 1, at(0, 0), math::identity_quat(), {}},
        {2, deflector_type, owner, at(scenario.distance, 0), math::identity_quat(), {}},
        {3, upgrade_station_type, 2, at(-3000, 3000), math::identity_quat(), {}},
    };
    tactical::EconomyRules economy;
    economy.players = {{2, units(100), 25, false, {}, 1, 3}};
    tactical::BuildOption option;
    option.type = armour_upgrade_type;
    option.kind = tactical::BuildKind::upgrade;
    option.queue = tactical::BuildQueue::upgrades;
    option.price = units(10);
    option.build_frames = 2; option.ai_build_frames = 2; option.available = true;
    economy.menus = {{upgrade_station_type, 2, {option}}};
    tactical::UpgradeProfile upgrade;
    upgrade.type = armour_upgrade_type;
    tactical::AbilityEnableRule rule;
    rule.ability_name = "magnetic_armor_l2";
    rule.applicable = {deflector_type};
    rule.all_allies = scenario.upgrade_all_allies;
    upgrade.ability_enables = {rule};
    economy.upgrades = {upgrade};
    const std::vector<tactical::SensorProfile> sensors{{blaster_type, units(3000)}, {deflector_type, units(3000)},
        {upgrade_station_type, units(3000)}};
    auto moving_table = motion();
    if (scenario.moving) {
        auto moving_profile = moving_table.profiles.front();
        moving_profile.type_id = deflector_type;
        moving_profile.max_speed = decimal("0.2");
        moving_table.profiles.push_back(moving_profile);
    }
    auto created = tactical::TacticalSession::create(initial, sensors, redirect_durability(), moving_table, std::nullopt,
        redirect_combat(scenario), {}, redirect_abilities(scenario), economy);
    expect(static_cast<bool>(created), "the redirect blaster session is created");
    if (!created) { std::cerr << created.error().message << '\n'; return result; }
    auto session = std::move(created).value();
    if (scenario.moving) expect(static_cast<bool>(session.submit(command(0, owner, 0, 2,
        tactical::MovePayload{at(scenario.distance, 1000)}))), "moving defender command");
    if (scenario.buy_upgrade) expect(static_cast<bool>(session.submit(command(0, 2, 0, 3,
        tactical::BuyPayload{armour_upgrade_type}))), "the armour upgrade is bought");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "a redirect blaster step succeeds");
        if (!stepped) { std::cerr << stepped.error().message << '\n'; break; }
        result.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.kind != tactical::CombatEventKind::projectile_hit || event.target != 2) continue;
            if ((event.outcome & tactical::hit_outcome_redirected) != 0U) ++result.deflected;
            else ++result.landed;
        }
        for (const auto& projectile : session.projectiles()) {
            if (projectile.reflected) {
                expect(projectile.profile_key == tactical::ProjectileProfileKey{
                    blaster_type, 0, tactical::ProjectileProfileKind::ordinary} && projectile.speed == units(7),
                    "WRB-10: reflected shots retain their launch type and profile independently of the new shooter's slot");
                if (projectile.shooter == 1) ++result.reflected_again;
            }
            if (projectile.owner != owner || projectile.shooter != 2) continue;
            ++result.fired_back;
            // Flying back towards the shooter at the origin: a step with a negative x part.
            if (projectile.step.x.raw() < 0) result.fired_back_toward_shooter = true;
        }
    }
    if (const auto state = session.durability_state(2)) result.hull = math::Fixed::from_raw(state->hull.raw() + state->shields.raw());
    return result;
}

} // namespace


void test_projectile_defence_producers() {
    namespace detail = tactical::detail;
    using Kind = tactical::AbilityKind;
    expect(tactical::ability_kind("MISSILE_SHIELD") == Kind::missile_shield, "WPJ-17: shield kind binds");
    expect(tactical::ability_kind("sensor_jamming") == Kind::sensor_jamming, "WHE-32: jammer kind binds");
    tactical::CombatProfile profile; profile.type_id = 1;
    tactical::CombatProfile passive = profile; passive.passive_missile_shield_radius = units(7);
    passive.ranged_target_z_adjust = units(2);
    tactical::CombatState combat;
    tactical::DurabilityState dead; dead.hull = {};
    tactical::MotionTable motion; tactical::MotionProfile mover; mover.type_id = 1;
    motion.profiles = {mover};
    const std::vector<tactical::SnapshotPlayer> players{{1, 1, false}, {2, 1, false}, {3, 3, false}, {4, 4, true}};
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        detail::CombatWorld world; world.relationships = players; world.motion = &motion;
        world.units.resize(10);
        for (std::size_t slot = 0; slot < world.units.size(); ++slot) {
            auto& unit = world.units[slot]; unit.id = slot + 1; unit.type_id = 1;
            unit.owner = 1; unit.profile = &profile; unit.combat = &combat;
        }
        world.units[0].profile = &passive;
        world.units[3].owner = 2; // another player on the same team
        world.units[4].owner = 4; // neutral, not allied
        world.units[5].owner = 3; // enemy
        world.units[6].position = at(0, 0, 11); // planar inclusion is insufficient
        world.units[7].combat = nullptr;
        world.units[8].type_id = 2; // no locomotor
        world.units[9].durability = &dead;
        world.units[3].position = at(0, 0, 10); // inclusive spatial boundary
        std::vector<detail::ProjectileDefenceAbilityInput> inputs(10);
        inputs[1] = {true, false, units(8), {}};
        inputs[2] = {false, true, {}, units(10)};
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto prepared = detail::prepare_projectile_defences(world, inputs, executor, std::vector<eawr::sim::EntityId>{3, 2, 1});
        expect(static_cast<bool>(prepared), "WPJ-17/WHE-32: production copied inputs prepare");
        if (!prepared) continue;
        const auto& sources = world.projectile_defences.sources;
        expect(sources.size() == 3 && sources[0].id == 3 && sources[1].id == 2 && sources[2].id == 1,
            "WPJ-17: source registration order survives spatial indexing");
        expect(world.units[0].sensor_jammed && world.units[2].sensor_jammed && world.units[3].sensor_jammed
            && world.units[4].sensor_jammed, "WHE-32: self, ally and nonenemy neutral receive inclusive 3D stamps");
        for (std::size_t slot = 5; slot < 10; ++slot)
            expect(!world.units[slot].sensor_jammed, "WHE-32: enemy, height, no combat, no locomotor and dead gates");
        tactical::CraftProfile craft; craft.type_id = 2;
        motion.squadrons.craft = {craft};
        prepared = detail::prepare_projectile_defences(world, inputs, executor, std::vector<eawr::sim::EntityId>{3, 2, 1});
        expect(prepared && world.units[8].sensor_jammed, "WHE-32: craft locomotor recipients are included");
        motion.squadrons.craft.clear();
        expect(sources.size() == 3, "WHE-32: ordinary recipients are stamped without registering as emitters");
        expect(sources.size() == 3 && sources[2].adjusted_position.z == units(2), "WPJ-17: copied source uses adjusted height");
        const auto previous = sources;
        inputs[1].active_shield = false;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.projectile_defences.sources.size() == 3
            && !world.projectile_defences.sources[1].active_shield && world.projectile_defences.sources[1].sensor_jammed,
            "WPJ-17: an inactive declared shield remains eligible while externally jammed");
        inputs[2].active_jamming = false;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.projectile_defences.sources.size() == 1
            && world.projectile_defences.sources[0].id == 1, "WPJ-17: deactivation preserves passive source");
        expect(std::none_of(world.units.begin(), world.units.end(), [](const auto& unit) { return unit.sensor_jammed; }),
            "WHE-32: deactivation clears every current-frame stamp");
        expect(previous.size() == 3 && previous[0].sensor_jammed && previous[1].active_shield,
            "WPJ-17: published records are immutable copies of activation state");
        inputs[2].active_jamming = true; world.units[2].in_nebula = true;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && !world.units[0].sensor_jammed && !world.units[2].sensor_jammed,
            "WHE-31: nebula suspends stamping");
        world.units[2].in_nebula = false; world.units[2].durability = &dead;
        world.units[0].durability = &dead;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.projectile_defences.sources.empty() && !world.units[3].sensor_jammed,
            "WPJ-17/WHE-32: source death removes registration and stamps");
        world.units[0].durability = nullptr; world.units[2].durability = nullptr;
        inputs[2].jamming_radius.reset(); world.units[2].profile = &passive;
        world.units[3].position = at(0, 0, 7);
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.units[3].sensor_jammed, "WHE-32: absent jamming radius falls back to passive radius");
        world.units[3].position = at(0, 0, 8);
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && !world.units[3].sensor_jammed, "WHE-32: fallback radius excludes beyond-boundary recipient");
    }
    tactical::AbilityProfile jammer; jammer.kind = Kind::sensor_jamming;
    jammer.expiration_frames = 3; jammer.recharge_frames = 6;
    auto profile_of = unit_of(1, jammer);
    auto state = tactical::initial_abilities(profile_of);
    expect(tactical::activate_ability(jammer, state.slots[0], {}, 10).changed, "WHE-31: existing activation slot starts jamming");
    static_cast<void>(tactical::expire_abilities(profile_of, state, 12));
    expect(state.slots[0].active, "WHE-31: timer stays active before authored expiry boundary");
    static_cast<void>(tactical::expire_abilities(profile_of, state, 13));
    expect(!state.slots[0].active && state.slots[0].ready_tick == 19, "WHE-31: exact expiry starts existing recharge");
    auto invalid = tactical::AbilityTable{}; jammer.effective_radius = units(-1);
    invalid.profiles = {unit_of(1, jammer)};
    expect(!tactical::validate_abilities(invalid), "WPJ-17: invalid authored defence radius is refused");
}


void test_projectile_defence_transaction() {
    using Kind = tactical::AbilityKind;
    const auto order_of = [](const tactical::TacticalSession& session) {
        const auto bytes = session.canonical_state_bytes();
        const std::vector<std::uint8_t> tag{'P', 'D', 'E', 'F'};
        auto position = std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end());
        std::vector<eawr::sim::EntityId> order;
        if (position == bytes.end()) return order;
        auto offset = static_cast<std::size_t>(position - bytes.begin()) + 4;
        const auto read = [&]() {
            std::uint64_t value = 0;
            for (unsigned shift = 0; shift < 64 && offset < bytes.size(); shift += 8)
                value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
            return value;
        };
        const auto count = read();
        if (count > 16) { expect(false, "PDEF test block has bounded source count"); return order; }
        for (std::uint64_t i = 0; i < count; ++i) order.push_back(read());
        return order;
    };
    class FailVisibility final : public eawr::sim::PartitionExecutor {
    public:
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            return eawr::sim::InlineExecutor{}.execute(count, partition);
        }
        eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            if (phase == "visibility") return eawr::core::Result<void>::failure({});
            return execute(count, partition);
        }
    };
    tactical::AbilityProfile jammer; jammer.kind = Kind::sensor_jamming;
    jammer.effective_radius = units(100); jammer.expiration_frames = 3;
    tactical::AbilityProfile shield; shield.kind = Kind::missile_shield; shield.effective_radius = units(100);
    tactical::AbilityTable abilities; abilities.profiles = {unit_of(corvette_type, jammer),
        unit_of(frigate_type, shield), unit_of(plain_type, jammer)};
    tactical::CombatTable combat;
    for (const auto type : {corvette_type, frigate_type, plain_type}) {
        tactical::CombatProfile profile; profile.type_id = type; combat.profiles.push_back(profile);
    }
    auto initial = setup();
    initial.units.push_back({4, plain_type, 1, at(0, 0), math::identity_quat(), {}});
    const std::vector<tactical::PlayerCommand> commands{
        command(0, 1, 0, 2, tactical::AbilityPayload{Kind::missile_shield, tactical::AbilityAction::activate}),
        command(0, 1, 1, 1, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
        command(1, 1, 0, 4, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
        command(2, 1, 0, 1, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::deactivate}),
        command(2, 1, 1, 1, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
        command(3, 1, 0, 1, tactical::DamagePayload{units(100000), tactical::hull_target})};
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(initial, {}, durability(), motion(), std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(created), "PDEF lifecycle session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        for (const auto& item : commands) expect(static_cast<bool>(session.submit(item)), "PDEF lifecycle command submits");
        expect(order_of(session).empty(), "PDEF is absent before any jammer activation");
        const auto before = session.canonical_state_bytes();
        expect(!session.step(FailVisibility{}), "PDEF failed frame reaches post-command failure seam");
        expect(session.canonical_state_bytes() == before && session.completed_tick() == 0,
            "PDEF failed frame rolls back registration and ability activation");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const std::vector<std::vector<eawr::sim::EntityId>> orders{{2, 1}, {2, 1, 4}, {2, 4, 1}, {2, 4}, {}, {}};
        for (std::size_t frame = 0; frame < orders.size(); ++frame) {
            session.scramble_storage_for_testing();
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "PDEF lifecycle frame succeeds after retry");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            expect(order_of(session) == orders[frame], "PDEF retains activation order, appends reactivation, prunes death/expiry");
        }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "PDEF transaction hashes equal at 1/2/4/8 workers");
        const auto encoded = tactical::write_replay(session.record());
        const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == session.record(), "WPJ-17/WHE-32: appended kinds round trip through opcode 8");
        if (!parsed) continue;
        auto playback = tactical::TacticalSession::from_replay(parsed.value(), {}, durability(), motion(), std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(playback), "PDEF lifecycle replay creates");
        if (playback) for (const auto& hash : hashes) {
            const auto step = playback.value().step(executor);
            expect(step && step.value().state_sha256 == hash, "PDEF replay reproduces source ordering at every frame");
        }
    }
    // Creation registers a declared shield immediately, even while inactive.
    // Exercise both orders in the command batch, including a post-command rollback.
    for (const bool reinforce_first : {true, false}) {
        std::vector<std::string> creation_reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto creation_setup = setup();
            tactical::EconomyRules economy;
            economy.players = {{1, units(100), 25, false, {}, 1, 3}};
            tactical::BuildOption option;
            option.type = frigate_type; option.price = units(10);
            option.build_frames = 1; option.ai_build_frames = 1; option.available = true;
            economy.menus = {{corvette_type, 1, {option}}};
            economy.footprints = {{frigate_type, std::nullopt, {}}};
            auto created = tactical::TacticalSession::create(creation_setup, {}, durability(), motion(),
                std::nullopt, combat, {}, abilities, economy);
            expect(static_cast<bool>(created), "PDEF reinforcement ordering session creates");
            if (!created) continue;
            auto session = std::move(created).value();
            expect(static_cast<bool>(session.submit(command(0, 1, 0, 1, tactical::BuyPayload{frigate_type}))),
                "PDEF reinforcement purchase submits");
            const auto activate_sequence = reinforce_first ? 1U : 0U;
            const auto reinforce_sequence = reinforce_first ? 0U : 1U;
            std::vector<tactical::PlayerCommand> batch{
                command(2, 1, activate_sequence, 1,
                    tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
                {{2, 1, reinforce_sequence}, {}, tactical::ReinforcePayload{frigate_type, at(0, 0)}}};
            std::sort(batch.begin(), batch.end(), [](const auto& a, const auto& b) {
                return a.key.sequence < b.key.sequence;
            });
            for (const auto& item : batch) expect(static_cast<bool>(session.submit(item)),
                "PDEF same-frame creation commands submit in sequence order");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 3; ++frame) {
                if (frame == 2) {
                    const auto before = session.canonical_state_bytes();
                    expect(!session.step(FailVisibility{}), "PDEF creation frame reaches failure seam");
                    expect(session.canonical_state_bytes() == before && session.units().size() == 3,
                        "PDEF creation rollback restores ledger, entity allocation and reinforcement pool");
                }
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "PDEF creation frame succeeds after retry");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
            }
            expect(session.units().size() == 4 && session.arrivals().size() == 1,
                "PDEF fixture creates one reinforcement in the activation frame");
            const std::vector<eawr::sim::EntityId> expected = reinforce_first
                ? std::vector<eawr::sim::EntityId>{2, 4, 1} : std::vector<eawr::sim::EntityId>{2, 1, 4};
            expect(order_of(session) == expected,
                "PDEF static creation registers at its command position before or after jammer activation");
            if (workers == 1) creation_reference = hashes;
            else expect(hashes == creation_reference, "PDEF creation order hashes equal at 1/2/4/8 workers");
        }
    }
}


// WLS-01..03: shield-only drain, the owner's damage bonus, admission and release.
void test_leech_shields() {
    using K = tactical::AbilityKind;
    expect(tactical::ability_kind("leech_shields") == K::leech_shields && static_cast<unsigned>(K::leech_shields) == 23,
        "WLS-01: shield leech binds its reserved kind");
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto start = setup();
            start.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, scenario == 4 ? 1U : 2U, at(scenario == 5 ? 5 : scenario == 6 ? 301 : 100, 0, 200), math::identity_quat(), {}}};
            auto health = durability();
            health.damage->shield_recharge_frames = 1000;
            for (auto& row : health.profiles) { row.shield_refresh = {}; row.max_shields = units(100); }
            health.profiles[1].max_shields = scenario == 0 ? units(1000) : scenario == 1 ? units(2) : units(100);
            if (scenario == 7) {
                health.damage->shield_recharge_frames = 1;
                health.profiles[1].shield_refresh = units(1);
            }
            tactical::AbilityProfile leech;
            leech.kind = K::leech_shields; leech.expiration_frames = scenario == 3 ? 12U : 4U; leech.recharge_frames = 12;
            leech.recharge_at_activation = true; leech.supports_autofire = true; leech.gui_activated_ability_name = "leech";
            auto holder = unit_of(corvette_type, leech);
            tactical::SpecialAbilityProfile nested;
            nested.kind = tactical::SpecialAbilityKind::leech_shields;
            nested.name = "leech"; nested.style = tactical::SpecialActivationStyle::user_input;
            nested.service_interval = 1; nested.beam_min_range = units(5); nested.beam_max_range = units(300);
            nested.damage_per_frame = units(3); nested.leech_damage_bonus = units(3); nested.leech_duration_frames = leech.expiration_frames;
            nested.filter.applicable_categories = 2;
            holder.special = {nested};
            tactical::AbilityTable table; table.profiles = {holder}; table.humans = {1, 2};
            tactical::CombatTable combat;
            tactical::CombatProfile source; source.type_id = corvette_type; source.category_bits = 1;
            tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
            if (scenario == 0) {
                tactical::WeaponProfile weapon;
                weapon.range = units(300); weapon.min_recharge_hundredths = 10; weapon.max_recharge_hundredths = 10;
                weapon.cone_width = units(360); weapon.cone_height = units(180);
                weapon.shot = tactical::ShotProfile{units(10), 0, units(1000), units(1000), true, true, {}};
                source.weapons = {weapon}; source.max_attack_distance = units(300);
                target.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
            }
            combat.profiles = {source, target};
            const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(1000)}, {frigate_type, units(1000)}};
            auto movement = motion();
            if (scenario == 3) {
                movement.profiles[1].max_speed = units(400);
                movement.profiles[1].acceleration = units(400);
                movement.profiles[1].deceleration = units(400);
                movement.profiles[1].rate_of_turn = units(360);
            }
            const auto create = [&]() { return tactical::TacticalSession::create(start, sensors, health, movement, std::nullopt, combat, {}, table); };
            auto created = create();
            expect(static_cast<bool>(created), "WLS-01: shield leech fixture binds");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 1,
                tactical::AbilityPayload{K::leech_shields, tactical::AbilityAction::activate, 2}))), "WLS-01: targeted command submits");
            if (scenario == 0) expect(static_cast<bool>(world.submit(command(0, 1, 1, 1, tactical::AttackPayload{2}))),
                "WLS-02: source fires while leeching");
            if (scenario == 2) expect(static_cast<bool>(world.submit(command(2, 1, 1, 1,
                tactical::AbilityPayload{K::leech_shields, tactical::AbilityAction::deactivate}))), "WLS-03: cancel submits");
            if (scenario == 3) expect(static_cast<bool>(world.submit(command(2, 2, 0, 2, tactical::MovePayload{at(1000, 0, 200)}))),
                "WLS-03: target starts leaving range");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            unsigned boosted_hits = 0, ordinary_hits = 0;
            for (unsigned frame = 0; frame < 20; ++frame) {
                const auto result = world.step(executor);
                expect(static_cast<bool>(result), "WLS-02: shield drain tick succeeds");
                if (!result) break;
                hashes.push_back(result.value().state_sha256);
                const auto state = world.ability_state(1)->slots.front();
                const bool admitted = scenario < 4 || scenario == 7;
                if (frame == 0) expect(state.active == admitted && (!admitted || state.ready_tick == 12),
                    "WLS-01/03: hostile shielded range gate; recharge starts at activation");
                if (frame == 1 && admitted) expect(world.durability_state(2)->shields == (scenario == 0 ? units(957)
                    : scenario == 1 ? Fixed{} : scenario == 7 ? units(98) : units(97)),
                    "WLS-02: first drain is raw three; ordinary ten-damage shot becomes forty");
                if (scenario == 0) {
                    for (const auto& event : result.value().snapshot->combat_events())
                        if (event.kind == tactical::CombatEventKind::projectile_hit)
                            ++(frame < 4 ? boosted_hits : ordinary_hits);
                    expect(world.durability_state(2)->shields == units(1000 - std::min(frame, 3U) * 3
                        - boosted_hits * 40 - ordinary_hits * 10), "WLS-02: each hit uses the current lock's damage bonus");
                }
                if (scenario == 7 && frame == 2) expect(world.durability_state(2)->shields == units(96),
                    "WLS-02: normal regeneration continues between shield drains");
                expect(world.durability_state(1)->shields == units(100), "WLS-02: source gains no shields");
                expect(world.durability_state(2)->hull == health.profiles[1].max_hull, "WLS-02: drain never overflows to hull");
                if (scenario == 1 && frame >= 1) expect(!state.active && state.ready_tick == 12,
                    "WLS-03: depletion releases immediately and retains activation recharge");
                if (scenario == 2 && frame >= 2) expect(!state.active && state.ready_tick == 12,
                    "WLS-03: cancel retains activation recharge");
                if (scenario == 3 && frame == 10) {
                    expect(!state.active && state.ready_tick == 12,
                        "WLS-03: movement outside the planar range releases before duration expiry");
                    const auto live = world.units();
                    const auto moved = std::find_if(live.begin(), live.end(), [](const auto& unit) { return unit.entity_id == 2; });
                    expect(moved != live.end() && moved->position.x > units(300),
                        "WLS-03: the movement fixture actually exits the beam range");
                }
                if (frame >= leech.expiration_frames && admitted) expect(!state.active && state.ready_tick == 12,
                    "WLS-03: duration expiry retains activation recharge");
            }
            if (scenario == 0) expect(boosted_hits > 0 && ordinary_hits > 0,
                "WLS-02: the fixture exercises weapon damage both during and after the lock");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WLS-02: every frame matches 1/2/4/8 workers");
            const auto record = world.record();
            const auto bytes = tactical::write_replay(record);
            const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == record, "WLS-01: targeted command round-trips");
            if (parsed) {
                auto replayed = tactical::TacticalSession::from_replay(parsed.value(), sensors, health, movement, std::nullopt, combat, {}, table);
                expect(static_cast<bool>(replayed), "WLS-02: replay binds");
                if (replayed) for (const auto& hash : hashes)
                    expect(replayed.value().step(executor).value().state_sha256 == hash, "WLS-02: replay reproduces drain and release");
            }
        }
    }
    // WLS-02: direct shield damage bypasses armor/modes and preserves independent pool/hull state.
    auto health = durability();
    auto state = tactical::full_durability(health.profiles.front());
    state.shields = units(2); state.energy = units(17);
    tactical::drain_shields(health.profiles.front(), *health.damage, state, units(5), 10);
    expect(state.shields == Fixed{} && state.energy == units(17) && state.hull == health.profiles.front().max_hull
        && state.depleted_frame == 10, "WLS-02: excess raw drain never reaches hull or energy and starts depletion");
    health.damage->depleted_increment_seconds = units(1);
    tactical::drain_shields(health.profiles.front(), *health.damage, state, units(5), 20);
    expect(state.depleted_frame == 20, "WLS-02: depleted direct damage extends the depletion window");
    state.shields = units(1);
    tactical::drain_shields(health.profiles.front(), *health.damage, state, units(5), 21);
    expect(state.shields == units(1) && state.depleted_frame == 21,
        "WLS-02: positive regenerated shields remain protected by an active depletion window");
    state.shields = units(50); state.depleted_frame.reset(); state.ion_storm_contact = 21;
    health.damage->ion_storm_disable_seconds = units(5);
    tactical::drain_shields(health.profiles.front(), *health.damage, state, units(5), 21);
    expect(state.shields == units(50), "WLS-02: an ion storm absorbs no direct shield damage");
}


// WAB-36: an autofired targeted ability locks onto its unit's own attack target with no ability command.
// Scenarios: 0 AI owner, its own targeting picks; 1 human, autofire off; 2 human, autofire switched on; 3 unshielded target;
// 4 attack target inside the nested minimum range; 5 no attack target.
void test_targeted_autofire() {
    using K = tactical::AbilityKind;
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto start = setup();
            // Two shielded enemies in beam range: the lock follows the unit's attack target, never a search.
            start.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(200, 0, 200), math::identity_quat(), {}},
                {3, frigate_type, 2, at(-120, 0, 200), math::identity_quat(), {}}};
            auto health = durability();
            health.damage->shield_recharge_frames = 1000;
            for (auto& row : health.profiles) { row.shield_refresh = {}; row.max_shields = units(1000); }
            if (scenario == 3) health.profiles[1].max_shields = Fixed{};
            tactical::AbilityProfile leech;
            leech.kind = K::leech_shields; leech.expiration_frames = 30; leech.recharge_frames = 60;
            leech.recharge_at_activation = true; leech.supports_autofire = true; leech.gui_activated_ability_name = "leech";
            auto holder = unit_of(corvette_type, leech);
            tactical::SpecialAbilityProfile nested;
            nested.kind = tactical::SpecialAbilityKind::leech_shields;
            nested.name = "leech"; nested.style = tactical::SpecialActivationStyle::user_input;
            nested.service_interval = 1; nested.beam_min_range = units(scenario == 4 ? 250 : 5); nested.beam_max_range = units(300);
            nested.damage_per_frame = units(3); nested.leech_damage_bonus = units(3); nested.leech_duration_frames = leech.expiration_frames;
            nested.filter.applicable_categories = 2;
            holder.special = {nested};
            tactical::AbilityTable table; table.profiles = {holder};
            table.humans = scenario == 1 || scenario == 2 ? std::vector<tactical::PlayerId>{1, 2} : std::vector<tactical::PlayerId>{2};
            tactical::CombatTable combat;
            tactical::CombatProfile source; source.type_id = corvette_type; source.category_bits = 1;
            tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
            if (scenario != 5) {
                tactical::WeaponProfile weapon;
                weapon.range = units(300); weapon.min_recharge_hundredths = 10; weapon.max_recharge_hundredths = 10;
                weapon.cone_width = units(360); weapon.cone_height = units(180);
                weapon.shot = tactical::ShotProfile{units(10), 0, units(1000), units(1000), true, true, {}};
                source.weapons = {weapon}; source.max_attack_distance = units(300);
            }
            target.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
            combat.profiles = {source, target};
            const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(1000)}, {frigate_type, units(1000)}};
            const auto movement = motion();
            auto created = tactical::TacticalSession::create(start, sensors, health, movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WAB-36: targeted autofire fixture binds");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            if (scenario != 0 && scenario != 5) expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, tactical::AttackPayload{2}))),
                "WAB-36: the source attacks the farther enemy");
            if (scenario == 2) expect(static_cast<bool>(world.submit(command(0, 1, 1, 1,
                tactical::AbilityPayload{K::leech_shields, tactical::AbilityAction::autofire_on}))), "WAB-36: autofire switches on");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            const bool locks = scenario == 0 || scenario == 2;
            std::vector<std::string> hashes;
            std::optional<unsigned> locked;
            eawr::sim::EntityId locked_target{};
            for (unsigned frame = 0; frame < 12; ++frame) {
                const auto result = world.step(executor);
                expect(static_cast<bool>(result), "WAB-36: targeted autofire tick succeeds");
                if (!result) break;
                // The ability phase reads this tick's targeting result; only tick 0 carries commands.
                const auto after = world.combat_state(1);
                const auto attack_target = after ? after->attack_target : eawr::sim::EntityId{};
                hashes.push_back(result.value().state_sha256);
                const auto state = world.ability_state(1)->slots.front();
                if (state.active && !locked) {
                    locked = frame;
                    locked_target = state.target;
                    expect(attack_target != 0 && state.target == attack_target && state.ready_tick == frame + 60U,
                        "WAB-36: the lock takes the unit's attack target and starts the recharge");
                    if (scenario == 2) expect(state.target == 2, "WAB-36: a human's ordered attack target is the one locked");
                }
                if (locked && state.active) expect(state.target == locked_target, "WAB-36: the lock keeps its target");
            }
            expect(locked.has_value() == locks, locks ? "WAB-36: an armed ready leech locks without a command"
                : "WAB-36: no lock without autofire, a shielded attack target in range, or an attack target");
            if (locks && locked) {
                expect(*locked <= 2, "WAB-36: the lock follows the attack target within the attack-state pass");
                expect(world.durability_state(locked_target)->shields < units(1000), "WLS-02: the autofired lock drains the target");
            }
            const auto record = world.record();
            expect(std::none_of(record.commands.begin(), record.commands.end(), [](const auto& entry) {
                const auto* ability = std::get_if<tactical::AbilityPayload>(&entry.payload);
                return ability && ability->action == tactical::AbilityAction::activate;
            }), "WAB-36: no activation command is recorded");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WAB-36: every frame matches 1/2/4/8 workers");
            const auto bytes = tactical::write_replay(record);
            const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == record, "WAB-36: the record round-trips");
            if (parsed) {
                auto replayed = tactical::TacticalSession::from_replay(parsed.value(), sensors, health, movement, std::nullopt, combat, {}, table);
                expect(static_cast<bool>(replayed), "WAB-36: replay binds");
                if (replayed) for (const auto& hash : hashes)
                    expect(replayed.value().step(executor).value().state_sha256 == hash, "WAB-36: replay reproduces the autofired lock");
            }
        }
    }
}


// WAB-80: FULL_SALVO changes only hardpoints that author the full-salvo delay.
void test_full_salvo() {
    expect(tactical::ability_kind("full_salvo") == tactical::AbilityKind::full_salvo, "WAB-80: FULL_SALVO binds");
    expect(tactical::to_string(tactical::AbilityKind::full_salvo) == "FULL_SALVO", "WAB-80: FULL_SALVO's name");
    expect(static_cast<unsigned>(tactical::AbilityKind::full_salvo) == 19U, "WAB-80: the reserved kind number");

    tactical::AbilityProfile salvo;
    salvo.kind = tactical::AbilityKind::full_salvo;
    salvo.expiration_frames = 210; // Expiration_Seconds 7
    salvo.recharge_frames = 1350;  // Recharge_Seconds 45
    const tactical::AbilityTable ability_data{{unit_of(corvette_type, salvo)}, {1}};
    tactical::CombatTable combat;
    tactical::CombatProfile ship;
    ship.type_id = corvette_type; ship.category_bits = 1;
    ship.hardpoints = {{0, at(5, 0), true}, {1, at(-5, 0), true}};
    tactical::ShotProfile shot;
    shot.damage = units(1); shot.damage_type = 0; shot.speed = units(40); shot.max_travel = units(1500);
    // Hardpoint 0 is the missile launcher: six pulses 0.5 s apart, a 4 to 5 s recharge, multiplier 0.18.
    tactical::WeaponProfile missile;
    missile.hardpoint = 0; missile.range = units(1500);
    missile.min_recharge_hundredths = 400; missile.max_recharge_hundredths = 500;
    missile.pulse_count = 6; missile.pulse_delay_frames = 15;
    missile.fire_a = at(5, 2); missile.fire_b = at(5, 2);
    missile.full_salvo_delay = decimal("0.18");
    missile.cone_width = units(90); missile.cone_height = units(90);
    missile.opportunity_when_idle = true; missile.opportunity_when_targeting = true;
    missile.shot = shot;
    // Hardpoint 1 authors no multiplier: it keeps its cadence while FULL_SALVO is on.
    tactical::WeaponProfile laser = missile;
    laser.hardpoint = 1; laser.pulse_count = 2; laser.pulse_delay_frames = 6;
    laser.min_recharge_hundredths = 100; laser.max_recharge_hundredths = 100;
    laser.fire_a = at(-5, 2); laser.fire_b = at(-5, 2);
    laser.full_salvo_delay = units(1);
    ship.weapons = {missile, laser};
    ship.max_attack_distance = units(1500);
    tactical::CombatProfile target;
    target.type_id = plain_type; target.category_bits = 1;
    combat.profiles = {ship, target};
    auto start = setup();
    start.units[0].position = at(0, 0);
    start.units[2].position = at(800, 0);
    const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(2000)}};
    constexpr std::uint64_t ticks = 520;
    struct Fire {
        std::vector<std::uint64_t> missile, laser;
        std::vector<std::string> hashes;
        std::uint64_t ready_tick{};
        bool active_at_end{};
    };
    const auto play = [&](const bool use, const std::size_t workers) {
        Fire result;
        auto created = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
            combat, {}, ability_data);
        expect(static_cast<bool>(created), "WAB-80: the session binds");
        if (!created) return result;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(command(0, 1, 1, 1, tactical::AttackPayload{3}))),
            "WAB-80: the attack order submits");
        if (use) {
            expect(static_cast<bool>(session.submit(command(0, 1, 2, 1,
                tactical::AbilityPayload{tactical::AbilityKind::full_salvo, tactical::AbilityAction::activate}))),
                "WAB-80: the activation submits");
        }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (std::uint64_t tick = 0; tick < ticks; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "WAB-80: a step succeeds");
            if (!stepped) return result;
            result.hashes.push_back(stepped.value().state_sha256);
            for (const auto& event : stepped.value().snapshot->combat_events()) {
                if (event.kind != tactical::CombatEventKind::weapon_fired || event.shooter != 1) continue;
                (event.weapon == 0 ? result.missile : result.laser).push_back(event.tick);
            }
        }
        if (const auto state = session.ability_state(1)) {
            result.ready_tick = state->slots[0].ready_tick;
            result.active_at_end = state->slots[0].active;
        }
        return result;
    };
    const auto plain = play(false, 1);
    const auto boosted = play(true, 1);
    expect(plain.missile.size() >= 6 && boosted.missile.size() >= 6, "WAB-80: the missile hardpoint fires volleys");
    // Pulses within a volley are 15 frames apart; with the multiplier round(15 x 0.18) = 3.
    const auto gaps = [](const std::vector<std::uint64_t>& frames, const std::uint64_t until) {
        std::vector<std::uint64_t> result;
        for (std::size_t index = 1; index < frames.size() && frames[index] <= until; ++index) {
            result.push_back(frames[index] - frames[index - 1]);
        }
        return result;
    };
    const auto plain_gaps = gaps(plain.missile, ticks);
    expect(!plain_gaps.empty() && *std::min_element(plain_gaps.begin(), plain_gaps.end()) == 15,
        "WAB-80: without FULL_SALVO the pulse gap is 15 frames");
    const auto boosted_gaps = gaps(boosted.missile, 200);
    expect(std::count(boosted_gaps.begin(), boosted_gaps.end(), 3) >= 5,
        "WAB-80: while FULL_SALVO is on the pulse gap is three frames");
    expect(!boosted_gaps.empty() && *std::min_element(boosted_gaps.begin(), boosted_gaps.end()) == 3,
        "WAB-80: no gap is shorter than the scaled pulse delay");
    // The recharge between volleys (4 to 5 s) is scaled too: round(120..150 x 0.18) = 22 to 27 frames.
    expect(std::any_of(boosted_gaps.begin(), boosted_gaps.end(), [](const std::uint64_t gap) {
        return gap >= 22 && gap <= 27;
    }), "WAB-80: the recharge between volleys shrinks to the scaled 4 to 5 s");
    const auto in_window = [](const std::vector<std::uint64_t>& frames, const std::uint64_t from, const std::uint64_t to) {
        return static_cast<std::size_t>(std::count_if(frames.begin(), frames.end(),
            [&](const std::uint64_t tick) { return tick >= from && tick < to; }));
    };
    expect(in_window(boosted.missile, 0, 211) >= 2 * in_window(plain.missile, 0, 211) + 6,
        "WAB-80: the seven seconds of FULL_SALVO fire far more missiles");
    // After the seven seconds the ordinary delays are back (the gap to the first shot after expiry is not
    // scaled below the pulse delay), and the recharge runs to 45 s from expiry.
    const auto late_gaps = gaps(boosted.missile, ticks);
    expect(late_gaps.size() > boosted_gaps.size()
        && *std::max_element(late_gaps.begin(), late_gaps.end()) >= 60,
        "WAB-80: after expiry the hardpoint is back on its ordinary recharge");
    expect(!boosted.active_at_end && boosted.ready_tick > ticks,
        "WAB-80: FULL_SALVO has expired and is recharging");
    // The hardpoint without a multiplier fires exactly as it does without FULL_SALVO.
    expect(plain.laser == boosted.laser && !plain.laser.empty(), "WAB-80: a hardpoint without a multiplier is unchanged");
    for (const std::size_t workers : {2U, 4U, 8U}) {
        const auto again = play(true, workers);
        expect(again.hashes == boosted.hashes && again.missile == boosted.missile,
            "WAB-80: FULL_SALVO hashes match with " + std::to_string(workers) + " workers");
    }
    // The replay round-trips and reproduces every tick.
    auto created = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
        combat, {}, ability_data);
    if (!created) return;
    auto session = std::move(created).value();
    static_cast<void>(session.submit(command(0, 1, 1, 1, tactical::AttackPayload{3})));
    static_cast<void>(session.submit(command(0, 1, 2, 1,
        tactical::AbilityPayload{tactical::AbilityKind::full_salvo, tactical::AbilityAction::activate})));
    const eawr::platform::ThreadWorkerAdapter executor(2);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) static_cast<void>(session.step(executor));
    const auto replay = session.record();
    auto bytes = tactical::write_replay(replay);
    auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
    expect(parsed && parsed.value() == replay, "WAB-80: the FULL_SALVO command survives the replay format");
    if (parsed) {
        auto again = tactical::TacticalSession::from_replay(parsed.value(), sensors, durability(), motion(),
            std::nullopt, combat, {}, ability_data);
        expect(static_cast<bool>(again), "WAB-80: the replay session is created");
        if (again) {
            std::vector<std::string> hashes;
            while (again.value().completed_tick() < replay.final_tick_count) {
                hashes.push_back(again.value().step(executor).value().state_sha256);
            }
            expect(hashes == boosted.hashes, "WAB-80: the replay reproduces every tick hash");
        }
    }
    // A multiplier outside [0, 64] is refused at creation.
    auto bad = combat;
    bad.profiles[0].weapons[0].full_salvo_delay = units(65);
    expect(!tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt, bad, {}, ability_data),
        "WAB-80: a full-salvo multiplier beyond 64 is rejected");
    bad.profiles[0].weapons[0].full_salvo_delay = units(-1);
    expect(!tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt, bad, {}, ability_data),
        "WAB-80: a negative full-salvo multiplier is rejected");
}


void test_laser_defence() {
    using Kind = tactical::AbilityKind;
    expect(tactical::ability_kind("LASER_DEFENSE") == Kind::laser_defense && static_cast<int>(Kind::laser_defense) == 18
        && tactical::to_string(Kind::laser_defense) == "LASER_DEFENSE", "WLD-01: LASER_DEFENSE is AbilityKind 18");
    expect(tactical::special_ability_kind("Laser_Defense_Ability") == tactical::SpecialAbilityKind::laser_defense
        && tactical::special_service_interval(tactical::SpecialAbilityKind::laser_defense) == 1,
        "WLD-03: the nested handler binds and services every frame");

    // WLD-04: the categories a shot in flight belongs to.
    tactical::Projectile shot;
    expect(tactical::projectile_defence_category(shot) == 0, "WLD-04: a laser bolt is not a targeted category");
    shot.homing = true;
    expect(tactical::projectile_defence_category(shot) == tactical::defence_category_missile, "WLD-04: a homing shot is a MISSILE");
    shot.homing = false;
    shot.flight = tactical::FlightState{};
    shot.flight->profile.kind = tactical::FlightKind::rocket;
    expect(tactical::projectile_defence_category(shot) == tactical::defence_category_rocket, "WLD-04: a rocket-path shot is a ROCKET");

    // WLD-02/WLD-06 table rules: the handler is required, the run is the handler's, the recharge starts at switch-on.
    {
        auto table = laser_defence_abilities();
        expect(static_cast<bool>(tactical::validate_abilities(table)), "WLD-02: the Crusader's table is valid");
        auto without = table;
        without.profiles[0].special.clear();
        expect(!tactical::validate_abilities(without), "WLD-02: LASER_DEFENSE without its nested handler is refused");
        auto mismatched = table;
        mismatched.profiles[0].abilities[0].expiration_frames = 900;
        expect(!tactical::validate_abilities(mismatched), "WLD-02: the time limit is the handler's duration");
        auto reach = table;
        reach.profiles[0].special[0].defence_radius = {};
        expect(!tactical::validate_abilities(reach), "WLD-04: a handler with no reach is refused");
        auto others = table;
        others.profiles[0].abilities[0].kind = Kind::turbo;
        expect(!tactical::validate_abilities(others), "WLD-06: only LASER_DEFENSE recharges from its switch-on");
        const auto ability = laser_defence_ability();
        tactical::AbilitySlot slot;
        expect(tactical::activate_ability(ability, slot, open_gate, 40).changed && slot.expires_tick == 1090
            && slot.ready_tick == 1840, "WLD-06: the 60 s countdown starts when the ability switches on");
        expect(tactical::deactivate_ability(ability, slot, 100).changed && !slot.active && slot.ready_tick == 1840,
            "WLD-06: switching off early changes nothing about the countdown");
        expect(!tactical::activate_ability(ability, slot, open_gate, 200).changed, "WLD-06: it cannot switch on again while counting down");
        tactical::AbilityState state;
        state.slots.resize(1);
        const auto profile = laser_defence_abilities().profiles[0];
        static_cast<void>(tactical::activate_ability(ability, state.slots[0], open_gate, 10));
        static_cast<void>(tactical::expire_abilities(profile, state, 1060));
        expect(!state.slots[0].active && state.slots[0].ready_tick == 1810,
            "WLD-06: running out after 35 s leaves the countdown where the switch-on put it");
    }

    const std::vector<tactical::PlayerCommand> none;
    const std::vector<tactical::PlayerCommand> armed{command(0, 2, 0, 2, laser(tactical::AbilityAction::activate)),
        command(0, 1, 1, 3, laser(tactical::AbilityAction::activate))};
    const auto control = laser_run(none, 800, 1, 400);
    expect(!control.hits.empty() && control.zaps.empty(), "WLD-05 control: without the ability the missiles hit the Crusader");

    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const auto active = laser_run(armed, 800, workers, 1900);
        if (!active.session) continue;
        if (workers == 1) reference = active.hashes;
        else expect(active.hashes == reference, "WLD-12: laser defence hashes equal at 1/2/4/8 workers");
        const auto started = active.status.empty() ? std::uint64_t{0} : active.status.front().started_tick;
        expect(!active.zaps.empty(), "WLD-05: the active Crusader shoots missiles down");
        bool inside = true, spaced = true, own_side_spared = true, expired_in_step = true;
        for (const auto& zap : active.zaps) {
            const math::Vec3 crusader = at(800, 0);
            inside = inside && tactical::within_range(crusader, zap.aim, units(500), tactical::RangeMetric::spatial)
                && zap.origin == crusader && zap.shooter == 2;
            // WLD-03: the first look is the service after the switch-on, then one every 16 services.
            spaced = spaced && zap.tick > started && (zap.tick - started - 1) % 16 == 0 && zap.tick <= started + 1050;
            own_side_spared = own_side_spared && zap.shooter != 3;
            const auto gone = std::find_if(active.expired.begin(), active.expired.end(), [&](const auto& event) {
                return event.target == zap.target && event.tick == zap.tick
                    && event.outcome == static_cast<std::uint32_t>(tactical::ProjectileExpiryReason::explicit_request);
            });
            expired_in_step = expired_in_step && gone != active.expired.end();
        }
        expect(inside, "WLD-04: a zapped shot was inside the 500 reach, from the Crusader");
        expect(spaced, "WLD-03: looks come on the 0.5 s counter after the switch-on, within the 35 s run");
        expect(own_side_spared, "WLD-04: another Crusader of the shooter's owner never shoots its own side's missile");
        expect(expired_in_step, "WLD-05: a zapped shot detonates where it is in the same frame");
        for (const auto& hit : active.hits) {
            expect(hit.tick > started + 1050 || hit.tick <= started, "WLD-05: no missile reaches the Crusader during its 35 s run");
        }
        expect(std::any_of(active.hits.begin(), active.hits.end(), [&](const auto& hit) { return hit.tick > started + 1050; }),
            "WLD-02: missiles reach the Crusader again once the run ends");
        // WLD-06 through the snapshot: the dial counts down from the switch-on.
        const auto& status = active.status;
        expect(status.size() == 1900 && status[0].active && status[0].remaining_frames > 0, "WLD-06: the status is published while on");
        expect(status.size() == 1900 && !status[1100].active && !status[1100].ready
            && status[1100].remaining_frames == started + 1800 - 1101, "WLD-06: after the run it still counts down from the switch-on");
        expect(status.size() == 1900 && status[1899].ready, "WLD-06: it is ready 60 s after the switch-on");
        const auto state = active.session->ability_state(2);
        expect(state && state->slots[0].ready_tick == started + 1800, "WLD-06: ready tick is switch-on plus the recharge");
        // The same record replays to the same hashes and event stream.
        const auto bytes = tactical::write_replay(*active.replay);
        const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == *active.replay, "WLD-12: the commands round trip through the replay format");
        if (!parsed) continue;
        auto playback = tactical::TacticalSession::from_replay(parsed.value(), laser_defence_sensors(), laser_defence_durability(),
            motion(), std::nullopt, laser_defence_combat(true), {}, laser_defence_abilities());
        expect(static_cast<bool>(playback), "WLD-12: the replay creates a session");
        if (playback) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            for (std::size_t tick = 0; tick < 400 && tick < active.hashes.size(); ++tick) {
                const auto step = playback.value().step(executor);
                expect(step && step.value().state_sha256 == active.hashes[tick], "WLD-12: the replay matches every live frame");
            }
        }
    }

    // WLD-04: a shot that has not flown 100 units yet is spared, and so is a Crusader that is switched off.
    const auto close = laser_run(armed, 90, 1, 120);
    expect(!close.hits.empty() && close.zaps.empty(), "WLD-04: a missile inside its first 100 units is not shot down");

    // WLD-06: switching off early keeps the countdown; a second switch-on before it ends is refused.
    const std::vector<tactical::PlayerCommand> toggled{command(0, 2, 0, 2, laser(tactical::AbilityAction::activate)),
        command(100, 2, 1, 2, laser(tactical::AbilityAction::deactivate)),
        command(200, 2, 2, 2, laser(tactical::AbilityAction::activate))};
    const auto early = laser_run(toggled, 800, 1, 300);
    const auto early_state = early.session ? early.session->ability_state(2) : std::nullopt;
    expect(early_state && !early_state->slots[0].active && early_state->slots[0].ready_tick == 1800,
        "WLD-06: an early switch-off leaves the switch-on's countdown");
    expect(std::any_of(early.events.begin(), early.events.end(), [](const auto& event) {
        return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::ability_unavailable;
    }), "WLD-06: a second switch-on during the countdown is rejected");
    expect(std::none_of(early.zaps.begin(), early.zaps.end(), [](const auto& zap) { return zap.tick > 101; }),
        "WLD-03: a Crusader switched off stops looking");
}


void test_self_destruct_projectile_order() {
    auto world = self_destruct_world(false, {1});
    for (auto& profile : world.combat.profiles) {
        if (profile.type_id != corvette_type) continue;
        // Leave ship acquisition disabled so only the explicit attack below starts firing.
        tactical::WeaponProfile weapon;
        weapon.range = units(1000); weapon.pulse_count = 1;
        // Zero recharge guarantees the first shot on the expiry tick regardless of the spawn draw.
        weapon.cone_width = units(360); weapon.cone_height = units(160);
        weapon.shot = tactical::ShotProfile{units(10), 0, units(1), units(1000), true, true, {}};
        profile.weapons = {weapon};
    }
    const std::vector<tactical::PlayerCommand> commands{
        command(0, 1, 0, 1, tactical::AbilityPayload{tactical::AbilityKind::self_destruct, tactical::AbilityAction::activate}),
        // Commands commit after this tick's launches; arm the attack for tick 105.
        command(104, 1, 1, 4, tactical::AttackPayload{3})};
    std::vector<std::string> first_hashes;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(world.start, world.sensors, world.health, {}, std::nullopt,
            world.combat, {}, world.table);
        expect(static_cast<bool>(created), "SELF_DESTRUCT order: the armed session is created");
        if (!created) continue;
        auto session = std::move(created).value();
        for (const auto& entry : commands)
            expect(static_cast<bool>(session.submit(entry)), "SELF_DESTRUCT order: a command is accepted");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::optional<std::uint64_t> shot_id, blast_id;
        for (std::uint64_t tick = 0; tick < 107; ++tick) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "SELF_DESTRUCT order: a step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto projectiles = session.projectiles();
            expect(std::adjacent_find(projectiles.begin(), projectiles.end(), [](const auto& left, const auto& right) {
                return left.id >= right.id;
            }) == projectiles.end(), "SELF_DESTRUCT order: projectile IDs stay strictly ascending");
            if (tick == 105) {
                expect(projectiles.size() == 2, "SELF_DESTRUCT order: expiry coincides with one ordinary launch");
                for (const auto& projectile : projectiles) {
                    expect(session.request_projectile_explosion(projectile.id),
                        "SELF_DESTRUCT order: lookup finds each ordinary shot and blast");
                    if (projectile.shooter == 4) shot_id = projectile.id;
                    if (projectile.shooter == 1) blast_id = projectile.id;
                }
                expect(shot_id && blast_id && *shot_id < *blast_id,
                    "SELF_DESTRUCT order: the ordinary launch precedes the newly allocated blast");
            }
            if (tick == 106) {
                expect(shot_id && !session.request_projectile_explosion(*shot_id),
                    "SELF_DESTRUCT order: the ordinary shot's requested explosion is serviced next tick");
                expect(blast_id && !session.request_projectile_explosion(*blast_id),
                    "SELF_DESTRUCT order: the blast detonates next tick");
                const auto enemy = session.durability_state(2);
                expect(enemy && enemy->hull == units(2700), "SELF_DESTRUCT order: the blast still delivers its damage");
            }
        }
        expect(hashes.size() == 107, "SELF_DESTRUCT order: every tick completes");
        if (workers == 1) first_hashes = hashes;
        else expect(hashes == first_hashes, "SELF_DESTRUCT order: hashes match with " + std::to_string(workers) + " workers");
    }
}


void test_self_destruct() {
    using K = tactical::AbilityKind;
    expect(tactical::ability_kind("self_destruct") == K::self_destruct && tactical::to_string(K::self_destruct) == "SELF_DESTRUCT"
        && static_cast<unsigned>(K::self_destruct) == 21U, "WSD-01: SELF_DESTRUCT binds as kind 21");
    const std::vector<tactical::PlayerCommand> activate{command(0, 1, 0, 1,
        tactical::AbilityPayload{K::self_destruct, tactical::AbilityAction::activate})};

    // WSD-03..07: the countdown, the kill, the blast.
    const auto world = self_destruct_world(false, {1});
    const auto first = run_self_destruct(world, activate, 1, 130);
    expect(first.hashes.size() == 130, "WSD-03: the battle runs");
    if (first.hashes.size() == 130) {
        expect(first.alive[0] && first.alive[50] && first.alive[103] && first.alive[104],
            "WSD-03: the unit keeps its place through the countdown");
        expect(!first.alive[105] && !first.alive[129], "WSD-04: the unit is gone once the 105 frames have run");
        const auto died = std::find_if(first.events.begin(), first.events.end(), [](const auto& event) {
            return event.kind == tactical::EventKind::unit_destroyed && event.unit == 1; });
        expect(died != first.events.end() && died->tick == 105, "WSD-05: the unit is destroyed in the tick its countdown runs out");
        expect(first.spawns[104] == 0 && first.spawns[105] == 1 && first.spawns[106] == 0,
            "WSD-06: the blast object exists for the frame between the kill and its detonation");
        expect(first.enemy[105] == units(3600) && first.enemy[106] == units(3600 - 900),
            "WSD-06: the enemy in reach takes the area damage the frame after the kill");
        expect(first.far[129] == units(3600), "WSD-06: an enemy beyond the blast radius takes nothing");
        expect(first.ally[129] == units(3600), "WSD-07: the blast never damages the owner's own units");
        expect(first.enemy[129] == units(2700), "WSD-06: the blast damages once");
    }
    // 1/2/4/8 workers and the replay agree.
    for (const std::size_t workers : {2U, 4U, 8U}) {
        const auto again = run_self_destruct(world, activate, workers, 130);
        expect(again.hashes == first.hashes, "WSD-03: self-destruct hashes match with " + std::to_string(workers) + " workers");
    }
    if (first.session) {
        const auto replay = first.session->record();
        const auto bytes = tactical::write_replay(replay);
        const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == replay, "WSD-03: the SELF_DESTRUCT command survives the replay format");
        if (parsed) {
            auto again = tactical::TacticalSession::from_replay(parsed.value(), world.sensors, world.health, {},
                std::nullopt, world.combat, {}, world.table);
            expect(static_cast<bool>(again), "WSD-03: the replay session is created");
            if (again) {
                const eawr::platform::ThreadWorkerAdapter executor(2);
                std::vector<std::string> hashes;
                while (again.value().completed_tick() < replay.final_tick_count) {
                    hashes.push_back(again.value().step(executor).value().state_sha256);
                }
                expect(hashes == first.hashes, "WSD-03: the replay reproduces every tick hash");
            }
        }
    }

    // WSD-02, WSD-08: a cancel switches it off and recharges for the share of the countdown that ran; the
    // ability is refused while it recharges; the unit lives on.
    std::vector<tactical::PlayerCommand> cancel = activate;
    cancel.push_back(command(50, 1, 1, 1, tactical::AbilityPayload{K::self_destruct, tactical::AbilityAction::deactivate}));
    cancel.push_back(command(100, 1, 2, 1, tactical::AbilityPayload{K::self_destruct, tactical::AbilityAction::activate}));
    cancel.push_back(command(193, 1, 3, 1, tactical::AbilityPayload{K::self_destruct, tactical::AbilityAction::activate}));
    const auto cancelled = run_self_destruct(world, cancel, 1, 260);
    if (cancelled.hashes.size() == 260) {
        expect(cancelled.alive[120] && cancelled.alive[200], "WSD-08: a cancelled countdown leaves the unit alive");
        expect(cancelled.enemy[120] == units(3600), "WSD-08: a cancelled countdown never explodes");
        const auto refused = std::count_if(cancelled.events.begin(), cancelled.events.end(), [](const auto& event) {
            return event.kind == tactical::EventKind::order_rejected && event.tick == 100; });
        expect(refused == 1, "WSD-08: a new countdown is refused while the cancel's recharge runs");
    }
    {
        // 50 of 105 frames ran: round(50 / 105 x 300) = 143 frames of recharge, ready at 193; the command at 193 acts.
        const auto state = cancelled.session ? cancelled.session->ability_state(1) : std::nullopt;
        expect(state && state->slots[0].active && state->slots[0].started_tick == 193 && state->slots[0].expires_tick == 298,
            "WSD-08: the recharge ended after 143 frames, so the command at 193 starts a countdown to 298");
    }

    // WSD-09: the script's AI and autofire triggers. A frigate of player 2 at (200, 0) shoots the holder once,
    // whatever the world; `crowd` places two player-2 corvettes.
    const auto scripted = [&](const bool human, const bool armed, const std::int64_t damage, const std::int64_t rating,
                              const std::array<math::Vec3, 2>& crowd, const std::vector<tactical::PlayerCommand>& extra,
                              const std::uint64_t ticks) {
        auto fixture = self_destruct_world(true, human ? std::vector<tactical::PlayerId>{1} : std::vector<tactical::PlayerId>{});
        if (human && !armed) fixture.table.autofire_defaults.clear();
        fixture.table.combat_ratings = {{frigate_type, units(rating)}};
        for (auto& row : fixture.health.profiles) if (row.type_id == plain_type) row.max_hull = units(100);
        fixture.start.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}},
            {2, frigate_type, 2, at(200, 0), math::identity_quat(), {}},
            {3, corvette_type, 2, crowd[0], math::identity_quat(), {}},
            {4, corvette_type, 2, crowd[1], math::identity_quat(), {}}};
        for (auto& profile : fixture.combat.profiles) {
            if (profile.type_id != frigate_type) continue;
            profile.max_attack_distance = units(300);
            tactical::WeaponProfile weapon; weapon.range = units(300); weapon.pulse_count = 1;
            weapon.min_recharge_hundredths = 2000; weapon.max_recharge_hundredths = 2000; // its first shot lands within 20 s
            weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
            weapon.cone_width = units(360); weapon.cone_height = units(160);
            weapon.shot = tactical::ShotProfile{units(damage), 0, units(40), units(300), true, true, {}};
            profile.weapons = {weapon};
        }
        std::vector<tactical::PlayerCommand> commands{command(0, 2, 0, 2, tactical::AttackPayload{1})};
        commands.insert(commands.end(), extra.begin(), extra.end());
        return run_self_destruct(fixture, commands, 1, ticks);
    };
    const std::array<math::Vec3, 2> near_crowd{at(250, 0), at(250, 50)};
    const std::array<math::Vec3, 2> far_crowd{at(900, 0), at(900, 50)};
    const auto shooting = [&](const bool human, const bool armed, const std::int64_t damage, const std::int64_t rating,
                              const bool crowd) {
        return scripted(human, armed, damage, rating, crowd ? near_crowd : far_crowd, {}, 800);
    };
    // The countdown has started (the unit is counting down or already gone).
    const auto triggered = [](const SelfDestructRun& run) {
        return std::any_of(run.active.begin(), run.active.end(), [](const bool on) { return on; });
    };
    const auto first_active = [](const SelfDestructRun& run) -> std::optional<std::uint64_t> {
        const auto on = std::find(run.active.begin(), run.active.end(), true);
        if (on == run.active.end()) return std::nullopt;
        return static_cast<std::uint64_t>(on - run.active.begin());
    };
    // The AI below half hull with the threat nearby (rating 2000 against 2 x hull share x 1500 = 1200).
    const auto ai = shooting(false, false, 60, 2000, false);
    expect(triggered(ai), "WSD-09: the AI's Krayt self-destructs below half hull when the nearby threat reaches the trigger");
    // A weak threat (rating 100 < 1200) or a healthy hull does nothing.
    expect(!triggered(shooting(false, false, 60, 100, false)), "WSD-09: the AI waits while the nearby threat is below the trigger");
    const auto healthy = shooting(false, false, 10, 2000, false);
    expect(!triggered(healthy), "WSD-09: the AI waits above half hull");
    // A human's unit needs autofire, at most 20 % hull and more than two neighbours in range.
    expect(triggered(shooting(true, true, 85, 0, true)), "WSD-09: autofire self-destructs a human's unit at 20 % hull among three enemies");
    expect(!triggered(shooting(true, false, 85, 0, true)), "WSD-09: a human's unit without autofire never self-destructs on its own");
    expect(!triggered(shooting(true, true, 85, 0, false)), "WSD-09: autofire waits for more than two neighbours");
    expect(!triggered(shooting(true, true, 60, 0, true)), "WSD-09: autofire waits for 20 % hull");

    // WFO-22, WSD-09: the script runs in the unit's first frame, then whenever the engine's single-precision
    // clock is more than 1 s past LastService; the first LastService is minus the entity's phase (1 frame for
    // unit 1). The float clock puts the services 29, 30 or 31 frames apart.
    const std::vector<std::uint64_t> services{0, 29, 59, 89, 120, 150, 181, 212, 242, 273, 304, 335, 366, 397};
    expect(healthy.services.size() >= services.size()
            && std::equal(services.begin(), services.end(), healthy.services.begin()),
        "WFO-22: the Krayt script runs at frame 0, then on the single-precision clock's 1 s service rate");
    // WSD-09: the AI acts in the first service after the hull falls below half (the frigate was in range at
    // every earlier service's proximity count).
    if (ai.holder.size() == 800) {
        const auto hit = std::find_if(ai.holder.begin(), ai.holder.end(), [](const Fixed hull) { return hull < units(100); });
        const auto hit_tick = static_cast<std::uint64_t>(hit - ai.holder.begin());
        const auto next = std::find_if(ai.services.begin(), ai.services.end(), [&](const std::uint64_t tick) { return tick > hit_tick; });
        expect(hit != ai.holder.end() && next != ai.services.end() && first_active(ai) == std::optional<std::uint64_t>(*next),
            "WSD-09: the AI's Krayt switches SELF_DESTRUCT on in the script's first service after the hull falls");
    }
    // WSD-10: the proximity query is a box of 400 around the unit in all three axes that touches each object's
    // box (20 wide here): corvettes at (350, +/-350) (planar 495) count, corvettes 405 out count by their box,
    // corvettes 600 above the plane or 460 out do not.
    expect(triggered(scripted(true, true, 85, 0, {at(350, 350), at(350, -350)}, {}, 800)),
        "WSD-10: the proximity box reaches the corners of the 400 square");
    expect(triggered(scripted(true, true, 85, 0, {at(405, 0), at(0, -405)}, {}, 800)),
        "WSD-10: an object whose box reaches into the 400 square counts");
    expect(!triggered(scripted(true, true, 85, 0, {at(250, 0, 600), at(250, 50, 600)}, {}, 800)),
        "WSD-10: the proximity box ends 400 above and below the unit");
    expect(!triggered(scripted(true, true, 85, 0, {at(460, 0), at(0, 460)}, {}, 800)),
        "WSD-10: boxes beyond the 400 square do not count");
    // WSD-09: autofire switched on later takes a service to arm the human script; it acts in the service after.
    // A run without autofire gives the frame the frigate's shot brings the holder to 15 % and the script's
    // services; the same battle then switches autofire on in the frame after that hit.
    {
        const auto calm = scripted(true, false, 85, 0, near_crowd, {}, 1300);
        const auto hit = std::find_if(calm.holder.begin(), calm.holder.end(), [](const Fixed hull) { return hull <= units(20); });
        expect(calm.holder.size() == 1300 && hit != calm.holder.end(), "WSD-09: the frigate brings the holder to 15 %");
        if (calm.holder.size() == 1300 && hit != calm.holder.end()) {
            const auto armed_at = static_cast<std::uint64_t>(hit - calm.holder.begin()) + 1;
            const auto died = std::find(calm.alive.begin(), calm.alive.end(), false);
            const auto death = static_cast<std::uint64_t>(died - calm.alive.begin());
            // The command commits after the frame's ability phase: the script sees autofire from the next frame.
            const auto arms = std::find_if(calm.services.begin(), calm.services.end(),
                [&](const std::uint64_t tick) { return tick > armed_at; });
            const auto acts = arms == calm.services.end() ? arms : std::next(arms);
            expect(acts != calm.services.end() && *acts < death, "WSD-09: the holder lives two services past the hit");
            if (acts != calm.services.end() && *acts < death) {
                const std::vector<tactical::PlayerCommand> arm{command(armed_at, 1, 0, 1,
                    tactical::AbilityPayload{K::self_destruct, tactical::AbilityAction::autofire_on})};
                const auto late = scripted(true, false, 85, 0, near_crowd, arm, 1300);
                expect(first_active(late) == std::optional<std::uint64_t>(*acts),
                    "WSD-09: autofire switched on after the hit arms the script at its next service and acts at the one after");
            }
        }
    }

    // WSD-01: the table refuses a self-destruct without its countdown or blast object.
    for (const int variant : {0, 1, 2}) {
        auto bad = self_destruct_world(false, {1});
        auto& ability = bad.table.profiles[0].abilities[0];
        if (variant == 0) ability.expiration_frames = 0;
        if (variant == 1) ability.spawned.reset();
        if (variant == 2) ability.spawned->blast = {};
        expect(!tactical::TacticalSession::create(bad.start, bad.sensors, bad.health, {}, std::nullopt, bad.combat, {}, bad.table),
            "WSD-01: a self-destruct needs an expiration and a blast object");
    }
}


void test_redirect_blaster() {
    // WRB-01: the nested handler binds by its authored name.
    expect(tactical::special_ability_kind("Redirect_Blaster_Ability") == tactical::SpecialAbilityKind::redirect_blaster
        && tactical::special_service_interval(tactical::SpecialAbilityKind::redirect_blaster) == 0,
        "WRB-01: the Redirect_Blaster handler binds and has no service");
    // WRB-04: the table refuses chances and angles outside the authored ranges.
    {
        auto table = redirect_abilities({});
        expect(static_cast<bool>(tactical::validate_abilities(table)), "WRB-04: the deflector table is valid");
        auto chance = table;
        chance.profiles[0].special[0].block_chance = decimal("1.5");
        expect(!tactical::validate_abilities(chance), "WRB-04: a block chance above 1 is refused");
        auto arc = table;
        arc.profiles[0].special[0].reaction_arc = decimal("361");
        expect(!tactical::validate_abilities(arc), "WRB-04: an arc above 360 degrees is refused");
        auto angle = table;
        angle.profiles[0].special[0].max_redirect_angle = decimal("181");
        expect(!tactical::validate_abilities(angle), "WRB-04: a redirection angle above 180 degrees is refused");
    }
    const auto control = redirect_run(RedirectScenario{true, false, false, false, "0.0"}, 1, 400);
    expect(control.landed > 0 && control.deflected == 0 && control.fired_back == 0,
        "WRB-06 control: with no chance every shot lands and nothing is flown back");

    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const auto blocked = redirect_run(RedirectScenario{}, workers, 400);
        if (workers == 1) reference = blocked.hashes;
        else expect(blocked.hashes == reference, "WRB-11: redirect blaster hashes equal at 1/2/4/8 workers");
        expect(blocked.deflected > 0 && blocked.landed == 0, "WRB-06: with block chance 1 every shot is deflected and none lands");
        expect(blocked.fired_back > 0 && blocked.fired_back_toward_shooter,
            "WRB-10: a block flies a fresh projectile of the deflector's owner back, turned around");
    }
    {
        const auto none = redirect_run(RedirectScenario{true, false, false, false, "0.0"}, 1, 400);
        const auto full = redirect_run(RedirectScenario{}, 1, 400);
        expect(full.hull.raw() > none.hull.raw(), "WRB-06: a deflected shot deals no direct damage (hull plus shields)");
    }
    // WRB-05: disabled until an upgrade names it; only then does it react.
    {
        const auto disabled = redirect_run(RedirectScenario{false, false}, 1, 400);
        expect(disabled.deflected == 0 && disabled.landed > 0, "WRB-05: Initially_Enabled No does nothing without the upgrade");
        const auto enabled = redirect_run(RedirectScenario{false, true}, 1, 400);
        expect(enabled.deflected > 0 && enabled.landed == 0, "WRB-05: the bought upgrade's Enable_Ability switches it on");
        // The deflector's owner is an ally of the holder: only an upgrade that affects all allies reaches it.
        const auto ally = redirect_run(RedirectScenario{false, true, false, true}, 1, 400);
        expect(ally.deflected == 0, "WRB-05: an upgrade without Affects_All_Allies does not enable an ally's unit");
        const auto shared = redirect_run(RedirectScenario{false, true, true, true}, 1, 400);
        expect(shared.deflected > 0, "WRB-05: an upgrade that affects all allies enables an ally's unit");
    }
    // WRB-06: shots the handler refuses (a missile, a bomb, a rocket and the like) land.
    {
        const auto missile = redirect_run(RedirectScenario{true, false, false, false, "1.0", "0.0", "360", false}, 1, 400);
        expect(missile.deflected == 0 && missile.landed > 0, "WRB-06: a shot the category list refuses is not deflected");
    }
    // WRB-06: Applicable_Unit_Categories names the shooters it reacts to.
    {
        const auto other = redirect_run(RedirectScenario{true, false, false, false, "1.0", "0.0", "360", true, 4}, 1, 400);
        expect(other.deflected == 0 && other.landed > 0, "WRB-06: a shooter outside the authored categories is not deflected");
    }
    // WRB-07: a narrow reaction arc ignores shots that travel along the deflector's own facing.
    {
        const auto narrow = redirect_run(RedirectScenario{true, false, false, false, "1.0", "0.0", "90"}, 1, 400);
        expect(narrow.deflected == 0 && narrow.landed > 0, "WRB-07: a 90 degree arc ignores a shot from behind");
    }
    // WRB-09, WRB-10: redirection is rolled first and aims at the shooter.
    {
        const auto redirected = redirect_run(RedirectScenario{true, false, false, false, "0.0", "1.0"}, 1, 400);
        expect(redirected.deflected > 0 && redirected.landed == 0 && redirected.fired_back > 0 && redirected.fired_back_toward_shooter,
            "WRB-09/WRB-10: a redirect chance of 1 sends a fresh projectile at the shooter");
    }
    // WRB-09: a middling chance deflects some and lets some land, the same at every worker count.
    {
        const auto half = redirect_run(RedirectScenario{true, false, false, false, "0.5"}, 1, 900);
        expect(half.deflected > 0 && half.landed > 0, "WRB-09: a block chance of one half deflects some shots and lets some land");
        const auto again = redirect_run(RedirectScenario{true, false, false, false, "0.5"}, 4, 900);
        expect(half.hashes == again.hashes && half.deflected == again.deflected && half.landed == again.landed,
            "WRB-11: the rolls are keyed by the shot, not by the worker count");
    }
    // WRB-03/08: creating the replacement is a side effect even when movement reports failure.
    {
        tactical::CombatEvent event; event.shooter = 1; event.weapon = 7; event.aim = at(100, 0);
        tactical::ShotProfile shot; shot.speed = units(7); shot.max_travel = units(1400); shot.redirect_blockable = true;
        auto original = tactical::detail::launch_projectile(event, shot, 1, true, 1, {}).value();
        tactical::detail::CombatUnit defender; defender.id = 2; defender.owner = 2;
        tactical::detail::CombatUnit attacker; attacker.id = 1; attacker.position = at(-100, 0);
        auto handler = redirect_abilities({}).profiles[0].special[0];
        for (const auto kind : {tactical::ProjectileProfileKind::ordinary, tactical::ProjectileProfileKind::ability,
                               tactical::ProjectileProfileKind::barrage}) {
            original.profile_key = tactical::ProjectileProfileKey{blaster_type, 7, kind};
            tactical::CombatRandom rolls(42, 1, 1, tactical::redirect_blaster_slot);
            tactical::CombatRandom directions(42, 1, 1, tactical::redirect_blaster_direction_slot);
            const auto result = tactical::detail::redirect_blaster_reaction(original, shot, handler, defender, attacker,
                2, rolls, directions, false);
            expect(result && result.value().created && result.value().created->profile_key == original.profile_key,
                "WRB-10: ordinary, ability and barrage profile identities survive reflection");
            expect(rolls.draws() == 2 && directions.draws() >= 2,
                "WRB-09: redirect/block rolls and angle/pitch use their separate reserved streams");
            if (!result || !result.value().created) continue;
            auto reflected = *result.value().created;
            std::vector<std::uint8_t> bytes, changed;
            tactical::detail::append_projectile(bytes, reflected);
            ++reflected.profile_key->type;
            tactical::detail::append_projectile(changed, reflected);
            expect(bytes != changed, "WRB-10: a reflected shot's retained type is canonical state");
            reflected = *result.value().created; ++reflected.profile_key->weapon; changed.clear();
            tactical::detail::append_projectile(changed, reflected);
            expect(bytes != changed, "WRB-10: a reflected shot's retained slot is canonical state");
            reflected = *result.value().created;
            reflected.profile_key->kind = kind == tactical::ProjectileProfileKind::ordinary
                ? tactical::ProjectileProfileKind::ability : tactical::ProjectileProfileKind::ordinary;
            changed.clear(); tactical::detail::append_projectile(changed, reflected);
            expect(bytes != changed, "WRB-10: a reflected shot's retained override kind is canonical state");
        }
    }
    for (const auto redirect : {false, true}) {
        RedirectScenario scenario;
        if (redirect) { scenario.redirect = "1.0"; scenario.block = "0.0"; }
        const auto stationary = redirect_run(scenario, 1, 400);
        scenario.moving = true;
        const auto moving = redirect_run(scenario, 1, 400);
        expect(stationary.deflected > 0 && stationary.landed == 0 && moving.deflected == 0
            && moving.landed > 0 && moving.fired_back > 0 && moving.hull < stationary.hull,
            "WRB-03/08: moving defenders launch the replacement but retain incoming damage and no block-sound event");
        for (const auto workers : {2U, 4U, 8U})
            expect(moving.hashes == redirect_run(scenario, workers, 400).hashes,
                "WRB-11: moving-defender side effects are worker independent");
    }
    {
        RedirectScenario scenario;
        scenario.block = "0.0"; scenario.redirect = "1.0"; scenario.reflect_twice = true;
        const auto twice = redirect_run(scenario, 1, 900);
        expect(twice.reflected_again > 0,
            "WRB-10: a subsequent handler uses the actual original profile despite a colliding nondeflectable slot");
        for (const auto workers : {2U, 4U, 8U})
            expect(twice.hashes == redirect_run(scenario, workers, 900).hashes,
                "WRB-11: repeated reflection is worker independent");
    }
    {
        RedirectScenario scenario; scenario.block = "0.5";
        const auto single = redirect_run(scenario, 1, 900);
        scenario.duplicate = true;
        const auto two = redirect_run(scenario, 1, 900);
        expect(two.deflected > single.deflected && two.landed < single.landed,
            "WRB-02/09: the next enabled handler receives new rolls after the first fails");
        for (const auto workers : {2U, 4U, 8U})
            expect(two.hashes == redirect_run(scenario, workers, 900).hashes,
                "WRB-11: two-handler draws are worker independent");
    }
}

} // namespace ability_tests
