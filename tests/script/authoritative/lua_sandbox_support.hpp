#pragma once

#include "eawr/core/sha256.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/clock_rng.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/numeric/binary64.hpp"
#include "eawr/sim/world.hpp"
#include "harness.hpp"
#include "scenario.hpp"
#include "sflua_sandbox.hpp"
#include <algorithm>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace lua_sandbox_test_support {

using namespace eawr::script::authoritative::test;




void expect_runs(const std::string& script, const std::string& label, std::vector<std::string> expected_commands = {});
void run_scheduler();
void run_workers();


} // namespace lua_sandbox_test_support
