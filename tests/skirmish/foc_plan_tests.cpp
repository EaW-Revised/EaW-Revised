// The FoC goal system in the M2 battle (#449, docs/behaviour/foc-tactical-ai.md "Goal system",
// "Plans and TaskForces"): with the retail space AI XML loaded, the Empire AI's goal system
// proposes goals, draws plans and runs them beside the freestore. The run must start named
// plans that issue orders, give the same state hashes, commands and plan journal on 1, 2, 4 and
// 8 workers (ADR-009), replay headless from its record, and keep the Lua cost per tick in
// budget. The live session hosts the same AI. The bombing run's squadrons fly to their fogged
// target and fire at it (#633); their trace (target, distance, whether the Empire sees the
// object, every 240 ticks from the order) is printed.
//
//   foc_plan_tests [ticks [timeline.csv [costs.csv]]]   (needs EAWR_EAW_GAME_ROOT; skipped otherwise)
//
// timeline.csv is the one-worker run's plan journal (tick, player, plan, goal, target, event,
// detail); costs.csv its Lua cost per tick. Both feed the eye-check material.

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

#include "foc_plan_support.hpp"

namespace foc_plan_test_support {

int failures = 0;
void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

} // namespace foc_plan_test_support

int main(int argc, char** argv) {
    return foc_plan_test_support::run_foc_plan_cases(argc, argv);
}
