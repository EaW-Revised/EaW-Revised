#pragma once

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/visibility.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sim_bench {

namespace tactical = eawr::sim::tactical;
using eawr::sim::math::Fixed;
using Clock = std::chrono::steady_clock;

struct Options {
    std::size_t units = 1200;
    std::uint64_t ticks = 300;
    std::uint64_t warmup = 30;
    std::size_t repeat = 3;
    std::uint64_t seed = 0x5eed;
    std::vector<std::size_t> workers;
    bool dispatch_cost = false;
};
constexpr tactical::PlayerId player_count = 4;

struct Battle {
    tactical::TacticalReplay replay;
    std::vector<tactical::SensorProfile> sensors;
    tactical::DurabilityTable durability;
    tactical::CombatTable combat;
};

[[nodiscard]] Battle generate(const Options& options);

} // namespace sim_bench
