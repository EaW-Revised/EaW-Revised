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
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace dogfight_test_support {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;
using eawr::sim::EntityId;

extern int failures;

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr std::uint64_t seed = 4570;
constexpr tactical::PlayerId empire = 1;
constexpr tactical::PlayerId rebel = 2;
constexpr tactical::TypeId craft_type = 200;
constexpr tactical::TypeId trio_type = 320;   // three craft
constexpr tactical::TypeId single_type = 330; // one craft
constexpr tactical::TypeId frigate_type = 400;

struct Order;
struct Session;

void expect(const bool condition, const std::string_view message);
[[nodiscard]] Fixed units(const std::int64_t value);
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0);
[[nodiscard]] double as_double(const Fixed value);
[[nodiscard]] double distance(const math::Vec3& a, const math::Vec3& b);
[[nodiscard]] tactical::MotionTable motion(const bool frigate_layer = true);
[[nodiscard]] tactical::DurabilityTable durability(const std::int64_t craft_hull);
[[nodiscard]] tactical::CombatTable combat();
[[nodiscard]] tactical::UnitState unit(const EntityId id, const tactical::TypeId type, const tactical::PlayerId owner,
    const math::Vec3& position, const std::int64_t yaw_degrees = 0);
[[nodiscard]] std::vector<tactical::Player> players();
[[nodiscard]] std::optional<math::Vec3> position_of(const tactical::TacticalSnapshot& snapshot, const EntityId id);
[[nodiscard]] std::vector<std::string> run(const Session& fixture, const std::size_t workers, const int ticks,
    const std::function<void(const tactical::TacticalSession&, const tactical::TacticalTick&)>& observe = {});
void expect_workers(const Session& fixture, const int ticks, const std::vector<std::string>& hashes, const std::string& label);
[[nodiscard]] Session trio_fight();
void test_pairing();
void test_chase_timer();
void test_chase_work_counts();
void test_can_follow();
[[nodiscard]] Session fight_to_the_end(const bool third);
void test_fight_end();
void test_spin_in_dogfight();
void test_avoidance();
void test_intercepted();
void test_retarget_clears_the_hardpoint();
void test_group_move();

struct Order {
    tactical::PlayerId player{};
    EntityId unit{};
    tactical::CommandPayload payload;
    std::vector<EntityId> more{}; // further units of the same command
    std::uint64_t tick{1};
};

struct Session {
    tactical::TacticalSetup setup;
    tactical::MotionTable motion;
    tactical::DurabilityTable durability;
    tactical::CombatTable weapons = combat();
    std::vector<Order> orders;
};

} // namespace dogfight_test_support
