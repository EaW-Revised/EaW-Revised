#pragma once

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"


#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace combat_test_support {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;

extern int failures;

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr tactical::TypeId shooter_type = 1;
constexpr tactical::TypeId fighter_type = 2;
constexpr tactical::TypeId bomber_type = 3;
constexpr tactical::TypeId transport_type = 4;
constexpr tactical::TypeId gunship_type = 5; // object weapon, ship-level targeting only
constexpr std::uint64_t fighter_bit = 1U;
constexpr std::uint64_t bomber_bit = 2U;
constexpr std::uint64_t transport_bit = 4U;
struct Shot {
    std::uint64_t tick{};
    tactical::CombatEventKind kind{};
    eawr::sim::EntityId shooter{};
    eawr::sim::EntityId target{};
    std::uint32_t hardpoint{tactical::no_hardpoint};
};


void expect(bool condition, std::string_view message);
[[nodiscard]] Fixed units(std::int64_t value);
[[nodiscard]] math::Vec3 at(std::int64_t x, std::int64_t y, std::int64_t z = 0);
[[nodiscard]] tactical::WeaponProfile ion(std::uint64_t restrictions = 0);
[[nodiscard]] tactical::CombatTable table(std::uint64_t restrictions = 0);
[[nodiscard]] std::vector<tactical::SensorProfile> sensors(std::int64_t range = 2000);
[[nodiscard]] tactical::UnitState unit(eawr::sim::EntityId id, tactical::TypeId type,
    tactical::PlayerId owner, math::Vec3 position, bool facing_west = false);
[[nodiscard]] tactical::TacticalSetup setup(std::vector<tactical::UnitState> units_in);
[[nodiscard]] tactical::TacticalSession session(const tactical::TacticalSetup& value,
    const tactical::CombatTable& combat = table(), std::int64_t sensor_range = 2000);
std::vector<Shot> run(tactical::TacticalSession& value, std::uint64_t ticks);
[[nodiscard]] eawr::sim::EntityId opportunity_target(const tactical::TacticalSession& value, eawr::sim::EntityId id);
[[nodiscard]] tactical::MotionTable turning_motion_all();
void test_note_cases(const std::filesystem::path& path);
void test_fire_reveal();
void test_priority_beats_distance();
void test_retention_and_replacement();
void test_fire_cycle();
void test_attack_order();
void test_ship_level_choice();
void test_ship_level_suitability();
void test_restrictions_and_fog();
void test_noncollidable_opportunity_target();
void test_zero_cone();
void test_fire_bone_cone();
void test_launcher_cone();
void test_fire_bone_pole();
void test_object_weapon_cone();
void test_lead();
void test_attack_turn();
void test_attack_turn_edges();
void test_order_without_attack_distance();
void test_orders_approach();
void test_orders_attack_move();
void test_orders_guard();
void test_orders_phase();
void test_orders_workers_and_replay();
void test_hardpoint_orders();
void test_hardpoint_orders_replay();

} // namespace combat_test_support
