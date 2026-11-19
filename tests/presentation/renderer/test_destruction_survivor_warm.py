"""Bounded destruction-survivor owner/type warming; native fixture runs remotely."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from native_cpp_fixture import compile_fixture, select_toolchain


HARNESS = r'''
#include "__HEADER__"
#include <iostream>
#include <limits>
using namespace eawr;
using namespace presentation;
int main() {
    int failures = 0;
    const auto check = [&](bool pass, const char* message) {
        if (!pass) { std::cerr << message << '\n'; ++failures; }
    };
    const std::vector<SurvivorWarmPlayer> players{{1, 101}, {4, 404}, {7, 404}};
    std::vector<SurvivorWarmType> types{
        {"large", 1, 1, {{{404}, {"medium"}}}},
        {"medium", 2, 2, {{{404}, {"small", "small"}}}}};
    const std::vector<SurvivorWarmSeed> seeds{{1, "large", 1, 5}};
    const auto count = [](const SurvivorWarmPlan& plan, unsigned owner, const std::string& type) {
        std::size_t total = 0;
        for (const auto& row : plan.plan.rows) if (row.owner == owner && row.type == type) {
            if (row.source != battle::ArrivalSource::destruction_survivor) return std::size_t{999};
            total += row.simultaneous_maximum;
        }
        return total;
    };
    auto plan = plan_survivor_warm(players, types, seeds, 64);
    check(count(plan, 4, "medium") == 1 && count(plan, 4, "small") == 4,
        "first affiliated owner, recursive physical members and repeated company members");
    check(!plan.admitted.contains({1, "medium"}) && plan.admitted.contains({4, "medium"})
        && plan.admitted.contains({7, "medium"}), "current affiliated owner retains its identity");
    check(plan.plan.rows.front().anchor == 5, "descendants keep the source composition anchor");
    types[0].choices[0].affiliations = {999};
    plan = plan_survivor_warm(players, types, seeds, 64);
    check(count(plan, 1, "medium") == 1, "no affiliation match retains source owner");
    types[0].minimum = types[0].maximum = 0;
    plan = plan_survivor_warm(players, types, seeds, 64);
    check(plan.plan.rows.empty() && !plan.admitted.contains({1, "medium"}), "nonpositive counts admit no survivor type");
    types[0].minimum = 200000; types[0].maximum = -100000;
    check(plan_survivor_warm(players, types, seeds, 64).plan.rows.empty(), "signed helper width cap can keep all counts negative");
    types[0].minimum = 4; types[0].maximum = 2;
    plan = plan_survivor_warm(players, types, seeds, 64);
    check(count(plan, 1, "medium") == 4, "reversed bounds warm their inclusive maximum");
    types[0].ejection = true;
    check(plan_survivor_warm(players, types, seeds, 64).plan.rows.empty(), "ejection diverts ordinary survivor placement");
    types[0].ejection = false; types[0].minimum = types[0].maximum = std::numeric_limits<std::int32_t>::max();
    plan = plan_survivor_warm(players, types, seeds, 8);
    check(plan.bounded && count(plan, 1, "medium") == 8 && count(plan, 4, "small") == 8,
        "large counts use a bounded owner/type warm prefix");
    types = {{"large", 1, 1, {{{101}, {"large"}}}}};
    plan = plan_survivor_warm(players, types, seeds, 8);
    check(plan.bounded && count(plan, 1, "large") == 8, "cyclic authoring terminates at the warm prefix");
    plan = plan_survivor_warm(players, types, seeds, 0);
    check(count(plan, 1, "large") == 0 && plan.admitted.contains({1, "large"}),
        "disabled warming retains runtime admission");
    std::cout << "survivor warm planner: " << failures << " failures\n";
    return failures ? 1 : 0;
}
'''


class DestructionSurvivorWarm(unittest.TestCase):
    def test_owner_counts_and_bounded_chains(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        header = ROOT / 'apps/viewer/src/destruction_survivor_warm.hpp'
        with tempfile.TemporaryDirectory(prefix='eawr-survivor-warm-') as temp:
            directory = Path(temp)
            source = directory / 'survivor_warm.cpp'
            executable = directory / ('survivor_warm.exe' if os.name == 'nt' else 'survivor_warm')
            source.write_text(HARNESS.replace('__HEADER__', header.as_posix()), encoding='utf-8')
            compile_fixture(toolchain, source, executable, include_dirs=(ROOT / 'include',))
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
