#pragma once

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fighter_test_support {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;

extern int failures;

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr std::uint64_t seed = 12345;
constexpr tactical::PlayerId empire = 1;
constexpr tactical::PlayerId rebel = 2;
constexpr tactical::TypeId carrier_type = 100; // SPAWN_SQUADRON station
constexpr tactical::TypeId craft_type = 200;   // TIE-fighter-like craft
constexpr tactical::TypeId squadron_a = 300;   // two craft
constexpr tactical::TypeId squadron_b = 310;   // one craft
constexpr tactical::TypeId frigate_type = 400; // priority 1
constexpr tactical::TypeId corvette_type = 410; // priority 3


void expect(const bool condition, const std::string_view message);
[[nodiscard]] Fixed units(const std::int64_t value);
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0);
[[nodiscard]] tactical::SpawnerProfile spawner();
[[nodiscard]] tactical::MotionTable motion();
[[nodiscard]] tactical::DurabilityTable durability();
[[nodiscard]] tactical::CombatTable combat();
[[nodiscard]] std::vector<tactical::SensorProfile> sensors();
[[nodiscard]] tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type,
    const tactical::PlayerId owner, const math::Vec3& position);
[[nodiscard]] std::vector<tactical::Player> players();
[[nodiscard]] const tactical::TacticalInstance* instance(const tactical::TacticalSnapshot& snapshot,
    const eawr::sim::EntityId id);
[[nodiscard]] math::Vec3 position(const tactical::TacticalInstance& found);
void test_hangar();
void test_destroyed_bay();
void test_validation();
void test_finite_reserve_boundary();
void test_formation_catch_up();
void test_coordinate_limit();
void test_out_of_combat();
void test_approach();
void test_launch_state();
void test_launch();
void test_launched_squadron();
void test_priority_scan();
void test_launch_idle();
void test_worker_equality();
void test_squadron_orders();
void test_squadron_attack_move();
void test_idle_cell_claim();
void test_idle_grid();
void test_squadron_guard();
void test_attack_order_approach();
void test_team_attack();
void test_team_retarget();
void test_squadron_hardpoint_attack();
void test_empty_table();
void test_staged_flight_rollback();


} // namespace fighter_test_support
