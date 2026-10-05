#pragma once

#include "eawr/data/xml.hpp"
#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace foc_plan_test_support {

namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;
extern int failures;
void expect(bool condition, const std::string& message);
std::optional<std::string> environment(const char* name);

int run_foc_plan_cases(int argc, char** argv);


} // namespace foc_plan_test_support
