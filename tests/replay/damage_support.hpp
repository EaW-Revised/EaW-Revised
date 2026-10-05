#pragma once

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace damage_test_support {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;

extern int failures;

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr tactical::TypeId shooter_type = 1;
constexpr tactical::TypeId frigate_type = 2; // shielded, hull 600, one destroyable weapon hardpoint
constexpr tactical::TypeId corvette_type = 3; // no shield, hull 300, one destroyable hardpoint
constexpr tactical::TypeId station_type = 4; // shielded, one shield generator
struct Tally {
    std::size_t shots{};
    std::size_t hits{};
    std::size_t absorbed{}; // hits the shield took whole (hit_outcome_shield_absorbed, #80)
    std::vector<tactical::Event> events;
};

void expect(bool condition, std::string_view message);
[[nodiscard]] Fixed units(std::int64_t value);
[[nodiscard]] Fixed decimal(std::string_view text);
[[nodiscard]] math::Vec3 at(std::int64_t x, std::int64_t y, std::int64_t z = 0);
[[nodiscard]] bool close_to(Fixed value, double expected, double tolerance);
[[nodiscard]] tactical::DamageRules rules();
[[nodiscard]] tactical::DurabilityProfile frigate();
[[nodiscard]] tactical::WeaponProfile laser(std::int64_t travel = 700, std::uint32_t hardpoint = 0);
[[nodiscard]] tactical::CombatTable combat(std::int64_t travel = 700);
[[nodiscard]] tactical::DurabilityTable durability();
[[nodiscard]] std::vector<tactical::SensorProfile> sensors();
[[nodiscard]] tactical::UnitState unit(eawr::sim::EntityId id, tactical::TypeId type,
    tactical::PlayerId owner, math::Vec3 position, bool facing_west = false);
[[nodiscard]] tactical::TacticalSetup setup(std::vector<tactical::UnitState> list);
[[nodiscard]] tactical::TacticalSession session(const tactical::TacticalSetup& value, std::int64_t travel = 700);
Tally run(tactical::TacticalSession& value, std::uint64_t ticks);
[[nodiscard]] std::vector<tactical::CollisionTriangle> plate(std::int64_t x, std::int64_t half);
[[nodiscard]] tactical::CombatTable missile_combat(bool homing);
void test_curve();
void test_hit_pipeline();
void test_diminishing_gates();
void test_out_of_combat();
void test_arrival_vulnerability();
void test_recharge();
void test_energy();
void test_arrival_in_battle();
void test_shield_loss();
void test_hardpoint_destroyed();
void test_energy_weapon();
void test_recharge_phase();
void test_segment();
void test_first_projectile_contact();
void test_zero_direct_blast();
void test_meshes();
void test_mesh_hits();
void test_craft_sphere();
void test_object_weapon_scatter();
void test_object_burst_clock();
void test_aimed_routes();
void test_ordered_hardpoint_route();
void test_out_of_range_miss();
void test_range_boundary();
void test_path();
void test_broad_phase_reach();
void test_broad_phase_sphere_reach();
void test_missile_homing();
void test_missile_lock_loss();

} // namespace damage_test_support
