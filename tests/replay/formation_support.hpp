#pragma once

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/pathfind.hpp"
#include "eawr/sim/tactical/session.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace formation_test_support {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;
using math::Vec2;
using math::Vec3;

extern int failures;

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr tactical::TypeId corvette_type = 2011;
constexpr tactical::TypeId frigate_type = 2012;

struct Run;

void expect(const bool condition, const std::string_view message);
[[nodiscard]] Fixed units(const std::int64_t value);
[[nodiscard]] Fixed decimal(const std::string_view text);
[[nodiscard]] Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z);
[[nodiscard]] bool near(const Fixed value, const Fixed expected, const Fixed tolerance);
[[nodiscard]] bool near(const Vec3& value, const Vec3& expected, const Fixed tolerance);
[[nodiscard]] tactical::MotionTable foc_table();
[[nodiscard]] tactical::FormationMember member(const eawr::sim::EntityId id, const tactical::TypeId type, const Vec3 position);
[[nodiscard]] const tactical::FormationSlot* slot_of(const std::vector<tactical::FormationSlot>& slots, const eawr::sim::EntityId id);
void test_occupation_radius();
void test_time_to_reach();
void test_column_slots();
void test_line_slots();
void test_mixed_layers();
void test_slot_avoids_layer();
void test_first_slot_open_position();
void test_immobile_member_and_zero_time();
[[nodiscard]] math::Quat yaw(const std::int64_t degrees);
[[nodiscard]] tactical::TacticalSetup setup(const std::vector<tactical::UnitState>& units_list);
[[nodiscard]] tactical::TacticalReplay group_replay();
[[nodiscard]] std::optional<Run> run(const tactical::TacticalReplay& replay, const std::size_t workers, const bool scramble,
    const tactical::MotionTable& table = foc_table());
[[nodiscard]] const tactical::UnitState* unit_of(const std::vector<tactical::UnitState>& list, const eawr::sim::EntityId id);
[[nodiscard]] Fixed distance(const Vec3& a, const Vec3& b);
void test_group_session(const char* trace);
void test_plan_searches_phase();
void test_single_lanes();
void test_search_budget();
void test_wait_replaced();
void test_disabled_engine_group_session();
void test_block_slots();
void test_block_approach();
void test_published_plan();

struct Run {
    std::vector<std::string> hashes;
    std::vector<std::vector<tactical::UnitState>> units;
    std::vector<std::optional<tactical::MotionState>> motion_at_40; // per unit ID 1..6
    std::vector<std::optional<tactical::MotionState>> motion_at_100; // per unit ID 1..16
};

} // namespace formation_test_support
