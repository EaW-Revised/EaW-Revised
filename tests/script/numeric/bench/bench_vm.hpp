#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eawr::script {

namespace sflua {
// Runs `ticks` calls of Tick(tick, steps_per_tick); returns nanoseconds per tick.
std::vector<std::int64_t> run_tick_workload(int ticks, int steps_per_tick, std::string& last_message);
} // namespace sflua

namespace hwlua_bench {
std::vector<std::int64_t> run_tick_workload(int ticks, int steps_per_tick, std::string& last_message);
} // namespace hwlua_bench

} // namespace eawr::script
