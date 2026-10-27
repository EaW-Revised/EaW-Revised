"""Exercise reserve admission against the production planner, without Godot."""

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
#include "__CAMPAIGN_HEADER__"
#include <iostream>
using namespace eawr;
using namespace presentation;
int main() {
    int failed = 0;
    const auto check = [&](bool pass, const char* name) {
        if (!pass) { std::cerr << name << '\n'; ++failed; }
    };
    using Source = battle::ArrivalSource;
    std::vector<battle::ArrivalDemand> demands{
        {1, "carrier", 2, Source::reinforcement_pool, 42},
        {1, "carrier", 3, Source::reinforcement_pool, 99},
        {2, "carrier", 2, Source::reinforcement_pool, 55},
        {1, "carrier", 999, Source::launch, 42},
        {1, "station", 999, Source::station_spawn, 42},
        {3, "empty", 0, Source::reinforcement_pool, 0}};
    std::vector<ReserveWarmCost> costs{{1, "carrier", 0, 10}, {2, "carrier", 0, 10}};
    auto plan = plan_reserve_warm(demands, costs, {true, 70});
    check(plan.complete() && plan.rows.size() == 2 && plan.additional_bytes == 70,
        "aggregate duplicate rows and ignore non-reserve sources by default");
    check(plan.rows[0].owner == 1 && plan.rows[0].anchor == 42 && plan.rows[0].requested == 5
        && plan.rows[0].admitted == 5 && plan.rows[1].owner == 2 && plan.rows[1].admitted == 2,
        "keep owner capacity separate and first contribution ordering");
    plan = plan_reserve_warm(demands, costs, {true, 60});
    check(!plan.complete() && plan.rows[0].admitted == 5 && plan.rows[1].admitted == 1
        && plan.additional_bytes == 60, "partial capacity is explicit");
    plan = plan_reserve_warm(demands, costs, {true, 9});
    check(!plan.complete() && plan.additional_bytes == 0 && plan.rows[0].admitted == 0
        && plan.rows[1].admitted == 0, "cannot reserve a fractional pair");
    costs[0].existing = 2;
    plan = plan_reserve_warm(demands, costs, {true, 50});
    check(plan.complete() && plan.additional_bytes == 50 && plan.rows[0].admitted == 5,
        "existing pairs are not charged again");
    costs[0].instance_bytes.reset();
    plan = plan_reserve_warm(demands, costs, {false, 0});
    check(!plan.complete() && plan.rows[0].admitted == 2 && !plan.rows[0].estimated
        && plan.rows[1].admitted == 2, "unknown estimate retains only existing capacity");
    check(!plan_reserve_warm(demands, {}, {false, 0}).complete(),
        "missing owner estimate is partial");
    demands = {{7, "free craft", std::numeric_limits<std::size_t>::max(), Source::reinforcement_pool, 0}};
    costs = {{7, "free craft", 0, 0}};
    plan = plan_reserve_warm(demands, costs, {true, 0});
    check(plan.complete() && plan.additional_bytes == 0,
        "zero marginal cost respects authored counts without arbitrary caps");
    demands.push_back({7, "free craft", 1, Source::reinforcement_pool, 0});
    plan = plan_reserve_warm(demands, costs, {false, 0});
    check(!plan.valid && !plan.complete(), "request sum overflow is rejected");
    demands = {{7, "free craft", 2, Source::reinforcement_pool, 0}};
    costs[0].instance_bytes = std::numeric_limits<std::uint64_t>::max();
    plan = plan_reserve_warm(demands, costs, {false, 0});
    check(plan.valid && !plan.complete() && plan.rows[0].admitted == 1
        && plan.additional_bytes == std::numeric_limits<std::uint64_t>::max(),
        "byte arithmetic cannot wrap an oversized pair into admission");
    costs.push_back(costs[0]);
    check(!plan_reserve_warm(demands, costs, {false, 0}).valid,
        "duplicate owner/type estimate is ambiguous");
    check(plan_reserve_warm({}, {}, {true, 0}).complete(), "empty plan is complete");
    demands = {{1, "fighter", 6, Source::launch, 7}, {1, "fighter", 4, Source::reinforcement_pool, 8}};
    costs = {{1, "fighter", 1, 2}};
    plan = plan_reserve_warm(demands, costs, {true, 18}, true);
    check(plan.complete() && plan.rows[0].requested == 10 && plan.rows[0].admitted == 10,
        "campaign hangars and directly carried craft add to the same owner/type capacity");
    plan = plan_reserve_warm(demands, costs, {true, 4}, true);
    check(!plan.complete() && plan.rows[0].admitted == 3,
        "campaign hangar warm-up respects measured recipe budget");
    std::vector<CampaignWarmType> types{
        {1, "carrier", {{"fighter", 5}, {"shuttle", 1}}},
        {1, "shuttle", {{"drone", 2}}},
        {2, "carrier", {{"fighter", 3}}}};
    std::vector<CampaignWarmSeed> seeds{{1, "carrier", 3, 44}, {2, "carrier", 2, 0}};
    auto campaign = campaign_warm_arrivals(seeds, types);
    check(campaign.complete && campaign.plan.rows.size() == 4,
        "reserve carriers expand hangars without needing a starting entity anchor");
    check(campaign.plan.rows[0].type == "fighter" && campaign.plan.rows[0].simultaneous_maximum == 15
        && campaign.plan.rows[0].anchor == 44 && campaign.plan.rows[1].type == "shuttle"
        && campaign.plan.rows[2].type == "drone" && campaign.plan.rows[2].simultaneous_maximum == 6
        && campaign.plan.rows[3].owner == 2 && campaign.plan.rows[3].simultaneous_maximum == 6,
        "same-ship repeats, nested hangars and selected owner profiles retain authored counts");
    types.push_back({1, "drone", {{"carrier", 1}}});
    campaign = campaign_warm_arrivals(seeds, types);
    check(!campaign.complete && campaign.plan.rows.size() == 5,
        "cyclic mod hangars retain a bounded prefix and report partial coverage");
    seeds = {{1, "carrier", std::numeric_limits<std::size_t>::max(), 0}};
    campaign = campaign_warm_arrivals(seeds, types);
    check(!campaign.complete, "nested demand multiplication cannot wrap into a small allocation");
    return failed;
}
'''


class ReserveWarmPlanner(unittest.TestCase):
    def test_authored_capacity_and_budget_admission(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        include_key = 'INCLUDE' if toolchain.msvc else 'CPLUS_INCLUDE_PATH'
        old = toolchain.environment.get(include_key, '')
        toolchain.environment[include_key] = str(ROOT / 'include') + (os.pathsep + old if old else '')
        with tempfile.TemporaryDirectory(prefix='eawr-reserve-warm-') as temp:
            directory = Path(temp)
            source = directory / 'reserve_warm.cpp'
            executable = directory / ('reserve_warm.exe' if os.name == 'nt' else 'reserve_warm')
            header = (ROOT / 'apps/viewer/src/reserve_warm_plan.hpp').as_posix()
            campaign_header = (ROOT / 'apps/viewer/src/campaign_warm_arrivals.hpp').as_posix()
            source.write_text(HARNESS.replace('__HEADER__', header).replace('__CAMPAIGN_HEADER__', campaign_header), encoding='utf-8')
            compile_fixture(toolchain, source, executable)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
