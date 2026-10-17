#include "galactic_ai.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

// Per-player policy snapshots shared by service inputs (GC AI barrier cost).
// Contract: an unchanged policy costs zero copies per barrier; a policy write
// and a restore each produce one fresh immutable snapshot; content always equals
// the live policy, at every worker count.
namespace {
using namespace eawr;
namespace a = script::foc::galactic_ai;
namespace auth = script::authoritative;
int failures{};
void expect(bool ok, std::string_view message) { if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
template<class T> T must(core::Result<T> result) { if (!result) throw std::runtime_error(result.error().code + " " + result.error().message); return std::move(result).value(); }
void must(core::Result<void> result) { if (!result) throw std::runtime_error(result.error().code + " " + result.error().message); }

a::LifecycleSetup fixture(bool two_players = false) {
    a::LifecycleSetup setup;
    setup.players = {{2, "ORDINARY", "NORMAL", 1, 1}};
    if (two_players) setup.players.push_back({3, "ORDINARY", "NORMAL", 1, 1});
    setup.categories = {{"OFFENSIVE", 0}, {"DEFENSIVE", 1}};
    setup.plans = {{"ATTACK", {"CONQUER"}}, {"DEFEND", {"DEFENCE"}}};
    setup.xml = {
        {"data/xml/ai/players/fixture.xml", R"(<AIPlayerType><Name>Ordinary</Name><GalacticFreeStoreScript>GalacticFreeStore</GalacticFreeStoreScript><GoalProposalFunctionSets>fixture</GoalProposalFunctionSets><Templates><Galactic>Default A B</Galactic></Templates><Difficulty_Adjustments><Normal>Normal</Normal></Difficulty_Adjustments></AIPlayerType>)"},
        {"data/xml/ai/templates/fixture.xml", R"(<AITemplates>
<Default><Priority>1</Priority><Trigger>Zero</Trigger><Budget><Offensive>One</Offensive><Defensive>Zero</Defensive></Budget><Turn_On><Goals><Category>Offensive</Category><Category>Defensive</Category></Goals><Plans><Goal_Category>Offensive</Goal_Category><Goal_Category>Defensive</Goal_Category></Plans></Turn_On></Default>
<A><Priority>4</Priority><Trigger>TriggerA</Trigger><Un_Trigger>LeaveA</Un_Trigger><Budget><Offensive>WeightA</Offensive></Budget><Turn_Off><Goals><Goal_Type>Auxiliary</Goal_Type></Goals></Turn_Off></A>
<B><Priority>3</Priority><Trigger>TriggerB</Trigger><Un_Trigger>LeaveB</Un_Trigger><Budget><Offensive>WeightB</Offensive></Budget></B>
</AITemplates>)"},
        {"data/xml/ai/goals/fixture.xml", R"(<Goals><Conquer><GameMode>Galactic</GameMode><Category>Offensive</Category></Conquer><Defence><GameMode>Galactic</GameMode><Category>Defensive</Category></Defence><Auxiliary><GameMode>Galactic</GameMode><Category>Defensive</Category></Auxiliary></Goals>)"},
        {"data/xml/ai/goalfunctions/fixture.xml", R"(<FunctionSet><ConquerEntry><Goal>Conquer</Goal><Function>One</Function></ConquerEntry></FunctionSet>)"},
        {"data/xml/ai/perceptualequations/fixture.xml", R"(<PerceptualEquations><One>1</One><Zero>0</Zero><TriggerA>0</TriggerA><TriggerB>0</TriggerB><LeaveA>0</LeaveA><LeaveB>0</LeaveB><WeightA>2</WeightA><WeightB>3</WeightB></PerceptualEquations>)"},
        {"data/xml/difficultyadjustments.xml", R"(<Difficulty_Adjustments><Difficulty_Adjustment Name="Normal"><Galactic_AI_Contrast_Multiplier>1.1</Galactic_AI_Contrast_Multiplier><Galactic_Build_Time_Multiplier>0.8</Galactic_Build_Time_Multiplier><Galactic_AI_Goal_Cycle_Sleep_Duration>0</Galactic_AI_Goal_Cycle_Sleep_Duration></Difficulty_Adjustment></Difficulty_Adjustments>)"}
    };
    for (const auto& [path, bytes] : setup.xml) { static_cast<void>(bytes); setup.xml_paths.push_back(path); }
    return setup;
}
struct Queries final : a::Queries {
    a::WorldInput world;
    std::map<std::string, a::Number, std::less<>> values;
    std::shared_ptr<const a::WorldInput> copy_world() const override { return std::make_shared<const a::WorldInput>(world); }
    // "Name#<player>" overrides "Name" for that player only.
    core::Result<a::Number> equation(std::string_view name, const a::QueryContext& context) const override {
        auto found = values.find(std::string(name) + '#' + std::to_string(context.self));
        if (found == values.end()) found = values.find(std::string(name));
        return core::Result<a::Number>::success(found == values.end() ? a::Number(0) : found->second);
    }
    std::optional<a::gc::PlanetId> containing_planet(eawr::sim::EntityId) const override { return {}; }
};
struct Services final : a::Services {
    std::vector<a::ServiceInput> calls;
    core::Result<void> initialize(a::gc::PlayerId) override { return core::Result<void>::success(); }
    core::Result<void> clear_equations(a::gc::PlayerId) override { return core::Result<void>::success(); }
    core::Result<void> apply_funding_writes(a::gc::PlayerId, const a::TemplatePolicy&) override { return core::Result<void>::success(); }
    core::Result<void> apply_policy(a::gc::PlayerId, const a::TemplatePolicy&, a::PolicyWrite) override { return core::Result<void>::success(); }
    core::Result<void> run(const a::ServiceInput& input, auth::ScriptScheduler&) override { calls.push_back(input); return core::Result<void>::success(); }
};
auth::ScriptScheduler scheduler() { auth::SessionConfig config; config.galactic_clock = true; config.tick_duration = {1, 30}; return must(auth::ScriptScheduler::create(config, {})); }
struct Rig {
    std::shared_ptr<Queries> queries = std::make_shared<Queries>();
    std::shared_ptr<Services> services = std::make_shared<Services>();
    std::shared_ptr<a::Manager> manager;
    auth::ScriptScheduler scripts = scheduler();
    std::map<a::gc::PlayerId, a::TemplatePolicy> previous; // live policy at the end of the previous barrier
    explicit Rig(bool two_players = false) { manager = must(a::Manager::create(fixture(two_players), {queries, {}, {}, services, {}, {}, {}})); }
    // One barrier; returns the policy copies it made and checks every input in it.
    std::uint64_t step(std::uint64_t tick) {
        const auto before = manager->policy_snapshot_stats();
        const auto first_call = services->calls.size();
        queries->world.clocks = {tick, tick};
        must(manager->service({tick, tick}, {}, scripts));
        // The templates service runs last and is the only writer inside a barrier: it sees the
        // live policy after its own write, every earlier input of that player the policy the
        // barrier started with (the previous barrier's live policy; defaults on the first).
        const auto lifecycle = manager->lifecycle();
        std::map<a::gc::PlayerId, const a::TemplatePolicy*> first;
        for (auto call = first_call; call < services->calls.size(); ++call) {
            const auto& input = services->calls[call];
            if (!input.policy) { expect(false, "every service input carries a policy snapshot"); continue; }
            const auto first_input = first.try_emplace(input.player, input.policy.get()).first->second;
            if (input.service != a::Service::templates) expect(*input.policy == *first_input, "inputs before the templates service keep the barrier's earlier policy");
            const auto expected = std::find_if(lifecycle.players.begin(), lifecycle.players.end(), [&](const auto& p) { return p.selected.player == input.player; });
            if (input.service == a::Service::templates) expect(expected != lifecycle.players.end() && *input.policy == expected->policy, "the templates input holds that player's live policy");
        }
        for (const auto& player : lifecycle.players) {
            const auto latest = std::find_if(services->calls.rbegin(), services->calls.rend() - static_cast<std::ptrdiff_t>(first_call), [&](const auto& c) { return c.player == player.selected.player; });
            expect(latest != services->calls.rend() - static_cast<std::ptrdiff_t>(first_call) && *latest->policy == player.policy, "the last service input of a barrier holds that player's live policy");
            const auto prior = previous.find(player.selected.player);
            const auto earliest = first.find(player.selected.player);
            if (prior != previous.end() && earliest != first.end()) {
                expect(*earliest->second == prior->second, "the first input of a barrier holds the previous barrier's live policy");
            }
            previous[player.selected.player] = player.policy;
        }
        const auto after = manager->policy_snapshot_stats();
        expect(after.builds + after.reuses - before.builds - before.reuses == services->calls.size() - first_call, "one snapshot request per service input");
        return after.builds - before.builds;
    }
    const a::TemplatePolicy* last() const { return services->calls.back().policy.get(); }
};

void unchanged_policy_costs_no_copies() {
    Rig rig;
    expect(rig.step(0) == 1, "first barrier builds one snapshot for all six services");
    const auto* first = rig.last();
    std::set<const a::TemplatePolicy*> seen;
    for (const auto& call : rig.services->calls) seen.insert(call.policy.get());
    expect(seen.size() == 1, "all inputs of one barrier share one snapshot");
    for (std::uint64_t tick = 1; tick <= 12; ++tick) {
        expect(rig.step(tick) == 0, "unchanged policy: zero policy copies per barrier");
        expect(rig.last() == first, "unchanged policy keeps the same snapshot");
    }
}
void policy_change_produces_fresh_snapshot() {
    Rig rig;
    rig.step(0); rig.step(1);
    const auto earlier = rig.services->calls.back().policy;
    const auto earlier_copy = *earlier;
    rig.queries->values["TriggerA"] = 1;
    // The template cursor visits one template per barrier: A activates within three.
    std::uint64_t builds{};
    for (std::uint64_t tick = 2; tick <= 4; ++tick) builds += rig.step(tick);
    expect(builds == 1, "one template activation builds exactly one fresh snapshot");
    expect(rig.last() != earlier.get() && *rig.last() != earlier_copy, "fresh snapshot carries the new policy");
    expect(*earlier == earlier_copy, "an earlier snapshot is never mutated");
    const auto* changed = rig.last();
    for (std::uint64_t tick = 5; tick <= 8; ++tick) expect(rig.step(tick) == 0 && rig.last() == changed, "snapshot is reused after the change");
    rig.queries->values["TriggerA"] = 0; rig.queries->values["LeaveA"] = 1; // departure replays survivors
    expect(rig.step(9) == 1 && rig.last() != changed && *changed != *rig.last(), "template departure builds a fresh snapshot");
    for (std::uint64_t tick = 10; tick <= 14; ++tick) expect(rig.step(tick) == 0, "no further copies once the policy settles");
}
void restore_produces_fresh_snapshot() {
    Rig original;
    original.queries->values["TriggerA"] = 1;
    for (std::uint64_t tick = 0; tick <= 3; ++tick) original.step(tick);
    const auto saved = original.manager->lifecycle();
    const auto* before_restore = original.last();
    // Restoring the same boundary into the same manager still drops the snapshot.
    must(original.manager->restore_lifecycle(saved));
    original.previous.clear();
    expect(original.step(4) == 1 && original.last() != before_restore, "restore produces a fresh snapshot");
    expect(original.step(5) == 0, "restored snapshot is then reused");
    // A manager that already built snapshots restores another boundary.
    Rig other;
    for (std::uint64_t tick = 0; tick <= 2; ++tick) other.step(tick);
    must(other.manager->restore_lifecycle(saved));
    other.previous.clear();
    expect(other.step(4) == 1 && *other.last() == saved.players.front().policy, "restore into a warm manager rebuilds from the restored policy");
    // A rejected restore changes nothing, so the snapshot survives.
    auto bad = saved; bad.players.front().activation_order.push_back(0);
    const auto* kept = other.last();
    expect(!other.manager->restore_lifecycle(bad) && other.step(5) == 0 && other.last() == kept, "a rejected restore keeps the current snapshot");
}
void players_keep_separate_snapshots() {
    Rig rig(true);
    expect(rig.step(0) == 2, "first barrier builds one snapshot per player");
    // Only player 2 activates template A, only player 3 activates template B.
    rig.queries->values["TriggerA#2"] = 1;
    rig.queries->values["TriggerB#3"] = 1;
    std::uint64_t builds{};
    for (std::uint64_t tick = 1; tick <= 8; ++tick) builds += rig.step(tick);
    expect(builds == 2, "one activation per player builds exactly one fresh snapshot each");
    const auto& players = rig.manager->lifecycle().players;
    expect(players.size() == 2 && players[0].policy != players[1].policy, "the two players hold different live policies");
    for (std::uint64_t tick = 9; tick <= 12; ++tick) expect(rig.step(tick) == 0, "settled players cost no copies");
}
void profile_scope_counts_builds() {
    const auto was = eawr::gc_profile::enabled.exchange(true);
    const auto begin = eawr::gc_profile::snapshot();
    Rig rig;
    rig.step(0);
    rig.queries->values["TriggerA"] = 1;
    for (std::uint64_t tick = 1; tick < 6; ++tick) rig.step(tick);
    const auto delta = eawr::gc_profile::difference(begin, eawr::gc_profile::snapshot());
    eawr::gc_profile::enabled.store(was);
    expect(delta[static_cast<std::size_t>(eawr::gc_profile::Phase::policy_snapshot)].calls == rig.manager->policy_snapshot_stats().builds
        && rig.manager->policy_snapshot_stats().builds == 2, "profile scope counts exactly the snapshot builds");
}
void worker_equality() {
    std::string reference;
    a::PolicySnapshotStats stats;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        Rig rig;
        for (std::uint64_t tick = 0; tick < 12; ++tick) {
            if (tick == 4) rig.queries->values["TriggerA"] = 1;
            if (tick == 8) { rig.queries->values["TriggerA"] = 0; rig.queries->values["LeaveA"] = 1; }
            rig.step(tick);
            auth::ServiceOptions options; options.host_paced = [](std::uint64_t) { return true; };
            must(rig.scripts.service(executor, options));
        }
        const auto digest = must(rig.manager->lifecycle_hash()) + must(rig.scripts.state_hash());
        if (reference.empty()) { reference = digest; stats = rig.manager->policy_snapshot_stats(); }
        else expect(digest == reference && rig.manager->policy_snapshot_stats() == stats, "1/2/4/8 workers: identical lifecycle state and snapshot work");
    }
}
} // namespace

int main() {
    try { unchanged_policy_costs_no_copies(); policy_change_produces_fresh_snapshot(); restore_produces_fresh_snapshot(); players_keep_separate_snapshots(); profile_scope_counts_builds(); worker_equality(); }
    catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    return failures ? 1 : 0;
}
