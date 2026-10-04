#pragma once

#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/world.hpp"
#include "harness.hpp"
#include "persist_scenario.hpp"
#include "scenario.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alloc_probe {
extern std::atomic<bool> armed;
extern std::atomic<std::size_t> largest;
extern std::atomic<std::size_t> total;
}

namespace lua_persistence_test_support {

using namespace eawr::script::authoritative::test;




auth::SessionConfig persistence_config();
Session persistence_session(auth::SessionConfig config = persistence_config());
void barrier_actions(Session& session, int tick);
std::string state_hash(Session& session);
Session loaded(const std::string& bytes);
void run_inflated_counts(const std::string& bytes);
void run_value_depth(const eawr::sim::PartitionExecutor& executor);


} // namespace lua_persistence_test_support
