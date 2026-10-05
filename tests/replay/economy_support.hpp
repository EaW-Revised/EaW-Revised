#pragma once

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace economy_test_support {

namespace sim = eawr::sim;
namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;

extern int failures;
extern const std::vector<tactical::SensorProfile> sensors;

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr tactical::PlayerId human = 1;
constexpr tactical::PlayerId ai = 2;
constexpr tactical::FactionId rebel = 100;
constexpr tactical::FactionId empire = 200;
constexpr tactical::TypeId ship_type = 10;     // a corvette-like buy: 500, 450 frames, population 2
constexpr tactical::TypeId station_type = 40;
constexpr tactical::TypeId craft_type = 60;
constexpr tactical::TypeId squadron_type = 70; // three craft: 550, 510 frames, population 1
constexpr tactical::TypeId upgrade_type = 90;  // listed, never built (PU-20)
constexpr sim::EntityId human_station = 1;
constexpr sim::EntityId ai_station = 2;


void expect(const bool condition, const std::string_view message);
[[nodiscard]] Fixed units(const std::int64_t value);
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0);
[[nodiscard]] double real(const Fixed value);
[[nodiscard]] tactical::TacticalSetup setup();
[[nodiscard]] tactical::EconomyRules rules(const std::int64_t credits = 6000, const std::uint32_t human_cap = 25);
[[nodiscard]] tactical::MotionTable motion();
[[nodiscard]] tactical::PlayerCommand buy(const std::uint64_t tick, const tactical::PlayerId player, const std::uint64_t sequence,
    const sim::EntityId station, const tactical::TypeId type);
[[nodiscard]] tactical::PlayerCommand cancel(const std::uint64_t tick, const tactical::PlayerId player,
    const std::uint64_t sequence, const std::uint32_t index);
[[nodiscard]] tactical::PlayerCommand reinforce(const std::uint64_t tick, const tactical::PlayerId player,
    const std::uint64_t sequence, const tactical::TypeId type, const math::Vec3 point);
[[nodiscard]] std::optional<tactical::TacticalSession> session(
    const tactical::EconomyRules& economy = rules(), const tactical::MotionTable& table = motion());
[[nodiscard]] const tactical::PlayerEconomy* ledger(const tactical::TacticalSession& world, const tactical::PlayerId player);
std::vector<tactical::Event> step_to(tactical::TacticalSession& world, const std::uint64_t tick);
[[nodiscard]] const tactical::TacticalInstance* instance(const tactical::TacticalSnapshot& snapshot, const sim::EntityId id);
void test_roster_gate();
void test_arrival_table();
void test_income();
void test_team_production();
void test_credit_grant();
void test_buy();
void test_refusals();
void test_cancel();
void test_station_lost();
void test_reinforce_ship();
void test_reinforced_carrier();
void test_arrival_hits();
void test_arrival_services();
void test_pending_victory_reinforcement();
[[nodiscard]] tactical::MotionTable arrival_lane_motion();
void test_arrival_lane_sweep();
void test_arrival_tracking_frames();
void test_reinforce_refusals();
void test_arrival_placement();


} // namespace economy_test_support
