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
#include "__RESTORED_HEADER__"
#include "__HANGAR_HEADER__"
#include <iostream>
#include <map>
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
    check(!campaign.complete && campaign.cyclic && campaign.plan.rows.size() == 5,
        "cyclic mod hangars retain a bounded prefix and report partial coverage");
    seeds = {{1, "carrier", std::numeric_limits<std::size_t>::max(), 0}};
    campaign = campaign_warm_arrivals(seeds, types);
    check(!campaign.complete && campaign.overflow, "nested demand multiplication cannot wrap into a small allocation");
    // A restored battle: the carriers' selected tier is unknown, so the largest roster up to the owner's tech counts.
    const std::vector<RosterCraft> tiers{{{"fighter", 4}}, {{"fighter", 6}, {"bomber", 2}}, {{"interceptor", 8}}, {{"fighter", 99}}};
    const auto up_to_one = largest_roster_up_to(tiers, 1);
    check(up_to_one.size() == 2 && up_to_one[0] == RosterCraft::value_type{"bomber", 2}
        && up_to_one[1] == RosterCraft::value_type{"fighter", 6}, "tiers up to the owner's tech keep the largest count per craft");
    check(largest_roster_up_to(tiers, 2).size() == 3 && largest_roster_up_to(tiers, 0).size() == 1
        && largest_roster_up_to(tiers, 9)[1].second == 99, "higher tiers join only when the owner's tech reaches them");
    battle::ArrivalPlan planned, restored;
    planned.add(1, "fighter", 4, Source::launch, 7);
    planned.add(1, "fighter", 2, Source::purchase, 7);
    planned.add(1, "bomber", 3, Source::launch, 7);
    restored.add(1, "fighter", 6, Source::launch, 0);
    restored.add(1, "bomber", 2, Source::launch, 0);
    restored.add(1, "interceptor", 5, Source::launch, 0);
    restored.add(2, "fighter", 1, Source::launch, 0);
    // Live craft count on top of the carriers' hangars, but only for types the plan or the carriers name.
    const auto with_alive = with_alive_demand(planned, restored, {{{1, "fighter"}, 5}, {{1, "bomber"}, 1}, {{1, "frigate"}, 9}});
    check(with_alive.rows.size() == restored.rows.size() + 2 && with_alive.rows[restored.rows.size()].type == "bomber"
        && with_alive.rows.back().type == "fighter" && with_alive.rows.back().simultaneous_maximum == 10,
        "live craft join the wanted capacity for named types only");
    const auto delta = restored_roster_delta(planned, restored);
    check(delta.additions.size() == 3 && delta.additions[0].owner == 1 && delta.additions[0].type == "fighter"
        && delta.additions[0].simultaneous_maximum == 2 && delta.additions[1].type == "interceptor"
        && delta.additions[1].simultaneous_maximum == 5 && delta.additions[2].owner == 2,
        "only the shortfall over the planned launch rows is added; purchases do not count as launch capacity");
    check(delta.fresh.size() == 2 && delta.fresh[0].second == "interceptor" && delta.fresh[1].first == 2,
        "only types the plan never named need a recipe read at load");
    // #5097 PR 2: the hangars of ships queued, pooled or called in, planned once per owner, type and tech level.
    {
        std::map<std::string, CampaignWarmType> table{
            {"carrier", {1, "carrier", {{"fighter", 12}, {"bomber", 6}, {"tender", 1}}}},
            {"tender", {1, "tender", {{"drone", 4}}}}};
        const auto lookup = [&](const sim::tactical::PlayerId owner, const std::string& type, const std::uint32_t tech)
            -> std::optional<CampaignWarmType> {
            const auto found = table.find(type);
            if (found == table.end()) return std::nullopt;
            auto hangar = found->second;
            hangar.owner = owner;
            if (tech >= 3) for (auto& [craft, count] : hangar.hangar) if (craft == "fighter") count = 20;
            return hangar;
        };
        const std::map<std::pair<sim::tactical::PlayerId, std::string>, std::size_t> none;
        auto warm = plan_hangar_warm({1, "carrier", 0}, lookup, none);
        check(warm.complete && warm.wants.size() == 4 && warm.wants[0].craft == "fighter" && warm.wants[0].slots == 12
            && warm.wants[1].craft == "bomber" && warm.wants[1].slots == 6 && warm.wants[2].craft == "tender"
            && warm.wants[3].craft == "drone" && warm.wants[3].slots == 4 && warm.wants[3].owner == 1,
            "a ship's hangar expands to every craft it launches, nested hangars included, in authored order");
        const std::map<std::pair<sim::tactical::PlayerId, std::string>, std::size_t> some{{{1, "fighter"}, 12}, {{1, "bomber"}, 2}, {{2, "drone"}, 9}};
        warm = plan_hangar_warm({1, "carrier", 0}, lookup, some);
        check(warm.wants.size() == 3 && warm.wants[0].craft == "bomber" && warm.wants[0].have == 2 && warm.wants[0].slots == 6
            && warm.wants[2].craft == "drone" && warm.wants[2].have == 0,
            "slots that exist are not planned again, and another owner's slots do not count");
        warm = plan_hangar_warm({1, "carrier", 3}, lookup, none);
        check(warm.wants[0].craft == "fighter" && warm.wants[0].slots == 20, "the roster follows the owner's tech level");
        warm = plan_hangar_warm({1, "carrier", 0}, lookup, none, 8);
        check(warm.wants[0].slots == 8 && warm.wants[1].slots == 6, "one ship claims at most the slot cap per craft");
        table["tender"].hangar = {{"carrier", 1}};
        warm = plan_hangar_warm({1, "carrier", 0}, lookup, none);
        check(!warm.complete && warm.wants.size() >= 3, "a cyclic hangar keeps its bounded prefix and says so");
        warm = plan_hangar_warm({1, "frigate", 0}, lookup, none);
        check(warm.complete && warm.wants.empty(), "a ship without a hangar plans nothing");
        // A planning failure abandons the jobs: their tickets must go back, or the next recipe waits behind them for good.
        struct FakeJob { std::uint64_t ticket{}; };
        struct FakeScene {   // the scene's read-ahead: one ticket reads at a time, the oldest first
            std::map<std::uint64_t, bool> tickets;
            std::size_t released{};
            bool ready(std::uint64_t ticket) const { return !tickets.empty() && tickets.begin()->first == ticket; }
            void release(std::uint64_t ticket) { released += tickets.erase(ticket); }
        } scene;
        std::deque<FakeJob> jobs;
        for (std::uint64_t ticket = 1; ticket <= 3; ++ticket) { scene.tickets[ticket] = true; jobs.push_back({ticket}); }
        jobs.push_back({});   // a job that never started holds no ticket
        scene.tickets[4] = true;
        check(!scene.ready(4), "a later ticket waits while an older one is outstanding");
        abandon_hangar_jobs(jobs, [&](std::uint64_t ticket) { scene.release(ticket); });
        check(jobs.empty() && scene.released == 3 && scene.tickets.size() == 1, "abandoned jobs release exactly their tickets");
        check(scene.ready(4), "the next recipe's ticket is ready once the abandoned ones are released");
        std::deque<FakeJob> none_left;
        abandon_hangar_jobs(none_left, [&](std::uint64_t) { check(false, "no job, no release"); });
        HangarWarmSeen seen;
        check(seen.note(1, 5, 0) && !seen.note(1, 5, 0) && seen.note(1, 5, 1) && seen.note(2, 5, 0) && seen.size() == 3,
            "a ship type is planned once per owner and tech level");
    }
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
            restored_header = (ROOT / 'apps/viewer/src/restored_roster_warm.hpp').as_posix()
            hangar_header = (ROOT / 'apps/viewer/src/hangar_warm_plan.hpp').as_posix()
            source.write_text(HARNESS.replace('__HEADER__', header).replace('__CAMPAIGN_HEADER__', campaign_header).replace('__RESTORED_HEADER__', restored_header).replace('__HANGAR_HEADER__', hangar_header), encoding='utf-8')
            compile_fixture(toolchain, source, executable)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_planning_failure_hands_back_the_tickets(self):
        # The production catch path must use the helper the harness exercises (a bare clear() leaks the scene's oldest ticket).
        source = (ROOT / 'apps/viewer/src/live_session_fighters.cpp').read_text(encoding='utf-8')
        catch = source[source.index('} catch (const std::bad_alloc&) {', source.index('void LiveSessionView::pump_hangar_warm')):]
        catch = catch[:catch.index('return;')]
        self.assertIn('abandon_hangar_jobs(hangar_warm_jobs_', catch)
        self.assertNotIn('hangar_warm_jobs_.clear()', catch)


if __name__ == '__main__':
    unittest.main()
