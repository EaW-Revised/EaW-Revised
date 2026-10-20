#pragma once

#include "../support/check.hpp"
#include "ability_cases.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ability_tests {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;



inline void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}


inline constexpr std::int64_t one = std::int64_t{1} << 24;


[[nodiscard]] inline Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }

[[nodiscard]] inline Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }

[[nodiscard]] inline math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}


// The M2 fleet's Unit_Ability data (spaceunitsfrigates.xml, spaceunitscorvettes.xml,
// spaceunitsfighters.xml), seconds truncated to frames (AB-04).
[[nodiscard]] inline tactical::AbilityProfile defend() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::defend;
    profile.expiration_frames = 450;
    profile.recharge_frames = 1800;
    profile.modifiers.weapon_delay = units(1);
    profile.modifiers.shield_regen = units(1);
    profile.modifiers.shield_regen_interval = decimal("0.1");
    profile.modifiers.energy_regen_interval = decimal("0.1");
    profile.modifiers.energy_regen = units(3);
    profile.modifiers.speed = decimal("0.8");
    profile.supports_autofire = true;
    return profile;
}

[[nodiscard]] inline tactical::AbilityProfile turbo() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::turbo;
    profile.expiration_frames = 600;
    profile.recharge_frames = 1500;
    profile.modifiers.weapon_delay = units(3);
    profile.modifiers.shield_regen = Fixed{};
    profile.modifiers.energy_regen = units(1);
    profile.modifiers.speed = units(2);
    return profile;
}


[[nodiscard]] inline tactical::UnitAbilityProfile unit_of(const tactical::TypeId type, const tactical::AbilityProfile& ability,
    const bool script = false) {
    return {type, {ability}, script};
}


inline const tactical::AbilityGate open_gate{true, true, false, true};


// --- Sessions ----------------------------------------------------------------------------------

inline constexpr tactical::TypeId corvette_type = 11;

inline constexpr tactical::TypeId frigate_type = 12;

inline constexpr tactical::TypeId plain_type = 13;


[[nodiscard]] inline tactical::MotionTable motion() {
    const tactical::MotionProfile corvette{corvette_type, decimal("3.72"), decimal("0.06"), decimal("0.06"),
        decimal("1.5"), units(2), decimal("0.24"), units(15)};
    const tactical::MotionProfile frigate{frigate_type, decimal("2.64"), decimal("0.048"), decimal("0.048"),
        decimal("0.84"), units(3), decimal("0.24"), units(5)};
    return {{units(15), units(300)}, {corvette, frigate}, std::nullopt, {}, {}};
}


[[nodiscard]] inline tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    tactical::DamageRules damage;
    damage.shield_recharge_frames = 90;
    damage.depleted_disable_seconds = units(5);
    damage.depleted_regen_cap = decimal("0.25");
    damage.damage_types = 1;
    damage.armor_types = 1;
    damage.armor_mods = {units(1)};
    damage.energy_recharge_frames = 150;
    damage.energy_to_shield = units(5);
    table.damage = damage;
    for (const auto type : {corvette_type, frigate_type, plain_type}) {
        tactical::DurabilityProfile profile;
        profile.type_id = type;
        profile.max_hull = units(3600);
        profile.max_shields = units(700);
        profile.shield_refresh = units(50);
        profile.armor_type = 0;
        profile.shield_armor_type = 0;
        profile.damage_tracking = true; // WCC-82: every M2 ship has DAMAGE_TRACKING
        table.profiles.push_back(profile);
    }
    return table;
}


[[nodiscard]] inline tactical::AbilityTable abilities(const std::vector<tactical::PlayerId>& humans) {
    tactical::AbilityTable table;
    table.profiles = {unit_of(corvette_type, turbo()), unit_of(frigate_type, defend(), true)};
    table.humans = humans;
    table.autofire_defaults = humans;
    return table;
}


[[nodiscard]] inline tactical::TacticalSetup setup() {
    tactical::TacticalSetup result;
    result.seed = 76;
    result.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    result.units = {
        {1, corvette_type, 1, at(-1800, -1500), math::identity_quat(), {}},
        {2, frigate_type, 1, at(0, 2000), math::identity_quat(), {}},
        {3, plain_type, 2, at(4000, 4000), math::identity_quat(), {}},
    };
    return result;
}


[[nodiscard]] inline tactical::PlayerCommand command(const std::uint64_t tick, const tactical::PlayerId player,
    const std::uint64_t sequence, const eawr::sim::EntityId unit, tactical::CommandPayload payload) {
    return {{tick, player, sequence}, {unit}, std::move(payload)};
}


struct Run {
    std::vector<std::string> hashes;
    std::vector<math::Vec3> corvette;           // position after each tick
    std::vector<Fixed> frigate_shield;          // after each tick
    std::vector<tactical::Event> events;
    std::optional<tactical::TacticalSession> session;
};


[[nodiscard]] inline Run run(const std::vector<tactical::PlayerCommand>& commands, const std::vector<tactical::PlayerId>& humans,
    const std::size_t workers, const std::uint64_t ticks) {
    Run result;
    auto created = tactical::TacticalSession::create(
        setup(), {}, durability(), motion(), std::nullopt, {}, {}, abilities(humans));
    expect(static_cast<bool>(created), "the ability session is created");
    if (!created) return result;
    auto session = std::move(created).value();
    for (const auto& entry : commands) expect(static_cast<bool>(session.submit(entry)), "a command is accepted");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "a step succeeds");
        if (!stepped) break;
        result.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->events()) result.events.push_back(event);
        for (const auto& unit : session.units()) {
            if (unit.entity_id == 1) result.corvette.push_back(unit.position);
        }
        const auto health = session.durability_state(2);
        result.frigate_shield.push_back(health ? health->shields : Fixed{});
    }
    result.session.emplace(std::move(session));
    return result;
}


// #614: a squadron a hangar launches mid-battle locks its S-foils like a tick-zero one. The
// button's order goes to the squadron's container (AB-40); the launched container and craft
// enter with the ability off and ready (AB-10), so the order reaches the flying craft.
inline constexpr tactical::TypeId station_type = 20;

inline constexpr tactical::TypeId xwing_type = 21;

inline constexpr tactical::TypeId xwing_squadron = 22;


[[nodiscard]] inline tactical::MotionTable hangar_motion() {
    tactical::MotionTable table;
    tactical::CraftProfile craft;
    craft.type_id = xwing_type;
    craft.max_speed = decimal("5.4");
    craft.min_speed = decimal("1.8");
    craft.rate_of_turn = units(6);
    craft.lift = units(6);
    craft.thrust = decimal("0.2");
    craft.roll_rate = units(6);
    craft.bank_angle = units(70);
    craft.strafe_distance = units(200);
    table.squadrons.craft = {craft};
    table.squadrons.squadrons = {
        {xwing_squadron, {xwing_type, xwing_type}, {at(0, 0), at(-10, 10)}, units(1000), units(200), units(300), units(20)}};
    tactical::SpawnerProfile spawner;
    spawner.type_id = station_type;
    spawner.entries = {{xwing_squadron, 1, 0}};
    spawner.delay_frames = 150;
    spawner.bays = {{0, at(20, 0, -10), at(1, 0, -1)}};
    table.squadrons.spawners = {spawner};
    return table;
}


[[nodiscard]] inline tactical::DurabilityTable hangar_durability() {
    tactical::DurabilityTable table;
    table.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    table.profiles = {tactical::DurabilityProfile{station_type, units(5000), std::nullopt, false, {}},
        tactical::DurabilityProfile{xwing_type, units(90), units(5), false, {}}};
    return table;
}

} // namespace ability_tests
