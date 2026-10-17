#include "galactic_ai.hpp"
#include "galactic_ai_funding.hpp"
#include "galactic_ai_goal_state.hpp"
#include "galactic_ai_world.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

namespace eawr::script::foc::ai {
struct EngineContractAccess {
    static void observe(Engine& engine, std::uint64_t tick) {
        engine.frame_ = static_cast<std::int64_t>(tick);
        engine.record(nullptr, 2, "observation", std::string(96, 'x'));
    }
    static void clear_diagnostics(Engine& engine) { engine.records_.clear(); engine.costs_.clear(); }
    static auto choice(Engine& engine) { return engine.sync_.range(0, 1000); }
};
}

namespace {
using namespace eawr;
namespace a = script::foc::galactic_ai;
namespace ai = script::foc::ai;
namespace auth = script::authoritative;
int failures{};
void expect(bool ok, std::string_view message) { if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
template<class T> T must(core::Result<T> result) { if (!result) throw std::runtime_error(result.error().code + " " + result.error().message); return std::move(result).value(); }
void must(core::Result<void> result) { if (!result) throw std::runtime_error(result.error().code + " " + result.error().message); }

a::LifecycleSetup fixture() {
    a::LifecycleSetup setup;
    setup.players = {{2, "ORDINARY", "NORMAL", 1, 1}};
    setup.categories = {{"OFFENSIVE", 0}, {"DEFENSIVE", 1}};
    setup.plans = {{"ATTACK", {"CONQUER"}}, {"DEFEND", {"DEFENCE"}}};
    setup.xml = {
        {"data/xml/ai/players/fixture.xml", R"(<AIPlayerType><Name>Ordinary</Name><GalacticFreeStoreScript>GalacticFreeStore</GalacticFreeStoreScript><GoalProposalFunctionSets>fixture</GoalProposalFunctionSets><Templates><Galactic>Default A B C</Galactic><Space>SpaceOnly</Space></Templates><Difficulty_Adjustments><Normal>Normal</Normal></Difficulty_Adjustments></AIPlayerType>)"},
        {"data/xml/ai/templates/fixture.xml", R"(<AITemplates>
<Default><Priority>1</Priority><Trigger>Zero</Trigger><Budget><Offensive>One</Offensive><Defensive>Zero</Defensive></Budget><Tactical_Budget_Allowance><Offensive>0.1</Offensive></Tactical_Budget_Allowance><Turn_On><Goals><Category>Offensive</Category><Category>Defensive</Category></Goals><Plans><Goal_Category>Offensive</Goal_Category><Goal_Category>Defensive</Goal_Category></Plans></Turn_On></Default>
<A><Priority>4</Priority><Trigger>TriggerA</Trigger><Un_Trigger>LeaveA</Un_Trigger><Budget><Offensive>WeightA</Offensive></Budget><Tactical_Budget_Allowance><Offensive>0.5</Offensive></Tactical_Budget_Allowance><Turn_Off><Goals><Category>Offensive</Category><Goal_Type>Auxiliary</Goal_Type></Goals><Plans><Name>Attack</Name></Plans></Turn_Off><Turn_On><Goals><Category>Defensive</Category><Goal_Type>Conquer</Goal_Type></Goals><Plans><Goal_Category>Offensive</Goal_Category></Plans></Turn_On></A>
<B><Priority>4</Priority><Trigger>TriggerB</Trigger><Un_Trigger>LeaveB</Un_Trigger><Budget><Offensive>WeightB</Offensive></Budget><Turn_Off><Goals><Goal_Type>Conquer</Goal_Type></Goals></Turn_Off></B>
<C><Priority>3</Priority><Trigger>TriggerC</Trigger><Un_Trigger>LeaveC</Un_Trigger><Turn_Off><Goals><Goal_Type>Defence</Goal_Type></Goals></Turn_Off></C>
</AITemplates>)"},
        {"data/xml/ai/goals/fixture.xml", R"(<Goals><Conquer><GameMode>Galactic</GameMode><Category>Offensive</Category><Global_Exclusions>Raid, Conquer</Global_Exclusions></Conquer><Defence><GameMode>Galactic</GameMode><Category>Defensive</Category></Defence><Auxiliary><GameMode>Galactic</GameMode><Category>Defensive</Category></Auxiliary><TacticalOnly><GameMode>Space</GameMode><Category>Offensive</Category></TacticalOnly></Goals>)"},
        {"data/xml/ai/goalfunctions/fixture.xml", R"(<FunctionSet><ConquerEntry><Goal>Conquer</Goal><Function>Zero</Function><Function>One</Function></ConquerEntry><TacticalEntry><Goal>TacticalOnly</Goal><Function>One</Function></TacticalEntry></FunctionSet>)"},
        {"data/xml/ai/perceptualequations/fixture.xml", R"(<PerceptualEquations><One>1</One><Zero>0</Zero><TriggerA>0</TriggerA><TriggerB>0</TriggerB><TriggerC>0</TriggerC><LeaveA>0</LeaveA><LeaveB>0</LeaveB><LeaveC>0</LeaveC><WeightA>2</WeightA><WeightB>3</WeightB></PerceptualEquations>)"},
        {"data/xml/difficultyadjustments.xml", R"(<Difficulty_Adjustments><Difficulty_Adjustment Name="Normal"><Galactic_AI_Contrast_Multiplier>1.1</Galactic_AI_Contrast_Multiplier><Galactic_Build_Time_Multiplier>0.8</Galactic_Build_Time_Multiplier><Galactic_AI_Goal_Cycle_Sleep_Duration>0</Galactic_AI_Goal_Cycle_Sleep_Duration></Difficulty_Adjustment></Difficulty_Adjustments>)"}
    };
    for (const auto& [path, bytes] : setup.xml) { static_cast<void>(bytes); setup.xml_paths.push_back(path); }
    return setup;
}
struct Queries final : a::Queries {
    a::WorldInput world;
    std::map<std::string, a::Number, std::less<>> values;
    std::set<std::string, std::less<>> failed_equations;
    std::shared_ptr<const a::WorldInput> copy_world() const override { return std::make_shared<const a::WorldInput>(world); }
    core::Result<a::Number> equation(std::string_view name, const a::QueryContext&) const override {
        if (failed_equations.contains(name)) {
            core::Diagnostic error; error.code = auth::codes::missing_api; error.message = "unbound equation context";
            return core::Result<a::Number>::failure(std::move(error));
        }
        const auto found = values.find(std::string(name));
        return core::Result<a::Number>::success(found == values.end() ? a::Number(0) : found->second);
    }
    std::optional<a::gc::PlanetId> containing_planet(sim::EntityId) const override { return {}; }
};
struct Services : a::Services {
    std::vector<a::ServiceInput> calls;
    std::vector<std::string> initialization;
    std::size_t native_landing_services{};
    core::Result<void> initialize(a::gc::PlayerId) override { initialization.push_back("ledger"); return core::Result<void>::success(); }
    core::Result<void> clear_equations(a::gc::PlayerId) override { return core::Result<void>::success(); }
    core::Result<void> apply_funding_writes(a::gc::PlayerId, const a::TemplatePolicy&) override { return core::Result<void>::success(); }
    core::Result<void> apply_policy(a::gc::PlayerId, const a::TemplatePolicy&, a::PolicyWrite write) override { if (write == a::PolicyWrite::initial) initialization.push_back("policy"); return core::Result<void>::success(); }
    core::Result<void> run(const a::ServiceInput& input, auth::ScriptScheduler&) override {
        calls.push_back(input);
        if (input.service == a::Service::execution) ++native_landing_services;
        return core::Result<void>::success();
    }
};
struct FundingServices final : Services {
    ai::GalacticFunding funding;
    std::vector<ai::EvaluatedFundingTemplate> initial;
    std::vector<sim::galactic::funding::Ledger> clears;
    std::vector<a::PolicyWrite> policy_writes;
    std::size_t funding_writes{};
    static std::vector<ai::EvaluatedFundingTemplate> evaluated(const a::TemplatePolicy& policy) {
        // Explicit synthetic equation values; the actual retained funding
        // consumer below owns the clear, priorities, funds and changed marker.
        const std::map<std::string, int> values{{"One", 1}, {"Zero", 0}, {"WeightA", 2}, {"WeightB", 3}};
        std::map<int, ai::EvaluatedFundingTemplate> grouped;
        const auto category = [](std::string_view name) { return name == "OFFENSIVE" ? 0U : 1U; };
        for (const auto& [name, entry] : policy.equations) {
            auto& input = grouped[entry.priority]; input.priority = entry.priority;
            const auto weight = entry.value.empty() ? std::optional<ai::funding::Amount>{} : must(ai::funding::Amount::from_integer(values.at(entry.value)));
            input.equations.push_back({category(name), weight});
        }
        for (const auto& [name, entry] : policy.allowances) {
            auto& input = grouped[entry.priority]; input.priority = entry.priority;
            input.allowances.push_back({category(name), must(script::numeric::to_fixed(entry.value))});
        }
        std::vector<ai::EvaluatedFundingTemplate> result;
        for (auto& [priority, input] : grouped) { static_cast<void>(priority); result.push_back(std::move(input)); }
        return result;
    }
    core::Result<void> initialize(a::gc::PlayerId player) override { return funding.initialize(player, initial, must(ai::funding::Amount::from_integer(1000))); }
    core::Result<void> clear_equations(a::gc::PlayerId player) override {
        auto result = funding.clear_equations(player);
        if (result) clears.push_back(*funding.ledger(player));
        return result;
    }
    core::Result<void> apply_funding_writes(a::gc::PlayerId player, const a::TemplatePolicy& writes) override {
        ++funding_writes;
        for (const auto& input : evaluated(writes)) if (auto result = funding.apply_template(player, input); !result) return result;
        return core::Result<void>::success();
    }
    core::Result<void> apply_policy(a::gc::PlayerId player, const a::TemplatePolicy& policy, a::PolicyWrite write) override {
        policy_writes.push_back(write);
        if (write == a::PolicyWrite::initial) initial = evaluated(policy);
        else if (write == a::PolicyWrite::reversal) {
            const auto inputs = evaluated(policy);
            for (const auto& input : inputs) if (auto result = funding.apply_template(player, input); !result) return result;
        }
        return core::Result<void>::success();
    }
};
auth::ScriptScheduler scheduler() { auth::SessionConfig config; config.galactic_clock = true; config.tick_duration = {1, 30}; return must(auth::ScriptScheduler::create(config, {})); }
struct Rig {
    std::shared_ptr<Queries> queries = std::make_shared<Queries>();
    std::shared_ptr<Services> services = std::make_shared<Services>();
    std::shared_ptr<a::Manager> manager;
    auth::ScriptScheduler scripts = scheduler();
    explicit Rig(a::LifecycleSetup setup = fixture()) {
        manager = must(a::Manager::create(std::move(setup), {queries, {}, {}, services, {}, {}, {}}));
    }
    void step(std::uint64_t sync, std::uint64_t mode, a::Admission admission = {}) {
        queries->world.clocks = {sync, mode};
        must(manager->service({sync, mode}, admission, scripts));
    }
};
void cadence() {
    Rig rig;
    rig.step(0, 0);
    expect(rig.manager->trace().size() == 6, "GFSA-02 six first services");
    for (std::size_t index = 0; index < 6; ++index) expect(rig.manager->trace()[index].service == static_cast<a::Service>(index), "GFS-A02 ordered service identity");
    expect(rig.services->initialization == std::vector<std::string>{"policy", "ledger"}, "WGA-67 template before initial funding");
    expect(rig.manager->lifecycle().players.front().deadlines == std::array<std::uint64_t, 6>{300, 1, 3, 3, 300, 1}, "GFC-A01..06 exact initial deadlines");
    rig.step(1, 1); rig.step(2, 2);
    expect(rig.manager->trace().size() == 10, "one-frame goal/template floor");
    rig.step(3, 3);
    expect(rig.manager->trace().size() == 14, "inclusive three-frame planning/execution deadline");
    expect(rig.manager->goal_functions(2) == std::vector<std::pair<std::string, std::string>>{{"CONQUER", "One"}}, "WGA-02 mode filter/WGA-93 later Function overrides Zero");
    Rig suppressed;
    suppressed.step(0, 0, {a::Mode::galactic, false, false});
    expect(suppressed.manager->trace().empty(), "suppression does not consume deadlines");
    suppressed.step(1, 0, {a::Mode::galactic, false, true});
    expect(suppressed.manager->trace().size() == 1 && suppressed.manager->trace().front().service == a::Service::planning, "WGA-04 planning-only auto-resolve");
    suppressed.step(2, 0, {a::Mode::space, true, false});
    expect(suppressed.manager->lifecycle().clocks == a::Clocks{1, 0}, "parent clocks remain frozen while another manager is selected");
    suppressed.step(4, 0, {a::Mode::galactic, false, true});
    expect(suppressed.manager->trace().size() == 2 && suppressed.manager->lifecycle().players.front().deadlines[0] == 0, "only planning rearms at frozen mode time");
    expect(suppressed.services->native_landing_services == 0, "WGA-415 suppressed auto-resolve planning never admits the native landing execution owner");
    suppressed.step(5, 1, {a::Mode::galactic, true, false});
    expect(suppressed.services->native_landing_services == 1, "ordinary admitted due execution services the native landing owner after suppression ends");
}
void configured_cadence() {
    auto setup = fixture();
    setup.service_periods = {300, 3, 10, 10, 300, 5};
    Rig original(setup);
    original.step(0, 0);
    expect(original.manager->lifecycle().players.front().deadlines == setup.service_periods,
        "configured periods rearm all six services in synchronization frames");
    for (std::uint64_t frame = 1; frame <= 7; ++frame) original.step(frame, frame);
    const auto saved = must(original.manager->capture());
    Rig restored(setup);
    must(restored.manager->restore(saved));
    expect(restored.manager->lifecycle() == original.manager->lifecycle(),
        "configured periods and partially elapsed deadlines survive native restore");
    for (std::uint64_t frame = 8; frame <= 610; ++frame) {
        original.step(frame, frame); restored.step(frame, frame);
        expect(original.manager->lifecycle() == restored.manager->lifecycle(),
            "configured resumed services equal uninterrupted across two perception deadlines");
    }
    Rig other_config;
    must(other_config.manager->restore(saved));
    expect(other_config.manager->lifecycle().service_periods == setup.service_periods,
        "saved session periods replace fresh defaults on restore");
    auto malformed = restored.manager->lifecycle();
    malformed.service_periods[2] = 0;
    expect(!restored.manager->restore_lifecycle(malformed), "zero service period cannot enter restored state");
    setup.service_periods[2] = 0;
    auto queries = std::make_shared<Queries>(); auto services = std::make_shared<Services>();
    expect(!a::Manager::create(setup, {queries, {}, {}, services, {}, {}, {}}),
        "zero service period is rejected at campaign setup");
}
void template_failure_consumers() {
    Rig failed;
    failed.queries->failed_equations.insert("TriggerA");
    failed.step(0, 0);
    expect(!failed.manager->lifecycle().players.front().active_templates[1]
        && !failed.queries->equation("TriggerA", {}),
        "WGA-420 failed trigger keeps initial zero while the owning query still reports failure");

    Rig explicit_leave;
    explicit_leave.queries->values["TriggerA"] = 1;
    explicit_leave.step(0, 0);
    explicit_leave.queries->failed_equations.insert("LeaveA");
    explicit_leave.step(1, 1);
    expect(explicit_leave.manager->lifecycle().players.front().active_templates[1],
        "WGA-420 failed explicit untrigger keeps initial zero and retains the active template");

    auto setup = fixture();
    for (auto& [path, xml] : setup.xml) {
        static_cast<void>(path);
        const std::string untrigger = "<Un_Trigger>LeaveA</Un_Trigger>";
        const auto at = xml.find(untrigger);
        if (at != std::string::npos) xml.erase(at, untrigger.size());
    }
    Rig implicit_leave(std::move(setup));
    implicit_leave.queries->values["TriggerA"] = 1;
    implicit_leave.step(0, 0);
    expect(implicit_leave.manager->lifecycle().players.front().active_templates[1],
        "implicit untrigger retains a positive trigger");
    implicit_leave.queries->failed_equations.insert("TriggerA");
    implicit_leave.step(1, 1);
    expect(!implicit_leave.manager->lifecycle().players.front().active_templates[1]
        && implicit_leave.manager->lifecycle().players.front().activation_order == std::vector<std::size_t>{0},
        "WGA-420 failed implicit trigger test removes the template and reapplies survivors");
}
void templates() {
    Rig rig;
    rig.queries->values["TriggerB"] = 1;
    rig.step(0, 0);
    expect(!rig.manager->goal_enabled(2, "Offensive", "Conquer"), "later equal-priority category/type setting controls one goal entry");
    rig.queries->values["TriggerA"] = 1;
    rig.step(1, 1); // rotating index1 triggers A after B
    auto state = rig.manager->lifecycle();
    expect(state.players.front().activation_order == std::vector<std::size_t>{0, 2, 1}, "activation list differs from authored vector");
    expect(state.players.front().policy.equations.at("OFFENSIVE").value == "WeightA", "equal priority later activation wins");
    expect(rig.manager->goal_enabled(2, "Offensive", "Conquer") && !rig.manager->plan_enabled(2, "Offensive", "Conquer", "Attack"), "off-before-on and specific plan name switch");
    expect(!rig.manager->goal_enabled(2, "Defensive", "Auxiliary"), "WGA-108 type-off follows category-on inside one template");
    const auto earlier_policy = rig.services->calls.back().policy;
    rig.step(2, 2);
    rig.queries->values["TriggerC"] = 1; rig.queries->values["LeaveC"] = 1;
    rig.step(3, 3); // C triggers and leaves in this same final service.
    state = rig.manager->lifecycle();
    expect(state.players.front().activation_order == std::vector<std::size_t>{0, 2, 1}, "same-service trigger/untrigger removes once");
    expect(state.players.front().policy.equations.at("OFFENSIVE").value == "WeightA", "survivors replay in activation order");
    expect(rig.manager->goal_enabled(2, "Defensive", "Defence"), "departing matched switches reset disabled1 before default reapplication");
    rig.queries->values["LeaveA"] = 1;
    rig.step(4, 4);
    state = rig.manager->lifecycle();
    expect(state.players.front().policy.equations.at("OFFENSIVE").value == "WeightB", "reversal restores survivor equation");
    expect(state.players.front().policy.allowances.at("OFFENSIVE").value == *ai::parse_real("0.5") && state.players.front().policy.allowances.at("OFFENSIVE").priority == 4, "allowance value AND priority survive untrigger");
    expect(earlier_policy->equations.at("OFFENSIVE").value == "WeightA", "template input remains an immutable snapshot");
}
void restore() {
    Rig original; original.step(0, 0); original.step(1, 1);
    const auto boundary = original.manager->lifecycle();
    const auto hash = must(original.manager->lifecycle_hash());
    Rig restored; must(restored.manager->restore_lifecycle(boundary));
    expect(must(restored.manager->lifecycle_hash()) == hash, "native lifecycle state/hash at same boundary");
    original.step(4, 4); restored.step(4, 4);
    expect(must(original.manager->lifecycle_hash()) == must(restored.manager->lifecycle_hash()) && restored.services->initialization.empty(), "first resumed consumer retains deadlines without reinitialization");
    auto bad = boundary; bad.players.front().activation_order.push_back(0);
    const auto before = restored.manager->lifecycle();
    expect(!restored.manager->restore_lifecycle(bad) && restored.manager->lifecycle() == before, "corrupt template reference rejected atomically");
    bad = boundary; bad.players.front().initialized = false;
    expect(!restored.manager->restore_lifecycle(bad) && restored.manager->lifecycle() == before, "serviced state cannot request fresh initialization on restore");
}
void category_width() {
    auto setup = fixture();
    setup.xml["data/xml/enum/aigoalcategorytype.xml"] = "<AIGoalCategoryType><Offensive>0</Offensive><Defensive>1</Defensive><MajorItem>9</MajorItem></AIGoalCategoryType>";
    setup.xml["data/xml/enum/gameobjectcategorytype.xml"] = "<GameObjectCategoryType><All>0xFFFFFFFFFFFFFFFF</All><Upper>0x100000000</Upper><Fighter>1</Fighter></GameObjectCategoryType>";
    auto& equations = setup.xml.at("data/xml/ai/perceptualequations/fixture.xml");
    equations.insert(equations.find("</PerceptualEquations>"), "<AllMask>GameObjectCategoryType[All]</AllMask><UpperMask>GameObjectCategoryType[Upper | Fighter]</UpperMask><MajorBudget>AIGoalCategoryType[MajorItem]</MajorBudget>");
    const auto data = must(a::load_data(setup));
    const auto* all = data.equations.find("AllMask");
    const auto* upper = data.equations.find("UpperMask");
    const auto* major = data.equations.find("MajorBudget");
    expect(all && all->program.size() == 1 && all->program.front().value == a::Number(-1), "stock-width All mask preserves signed converter semantics");
    expect(upper && upper->program.size() == 1 && upper->program.front().value == a::Number(4294967297LL), "category OR retains bits above uint32 width");
    expect(major && major->program.size() == 1 && major->program.front().value == a::Number(9), "registered goal category converter uses the parsed category id");
}
void reversal_funding() {
    for (const bool departing : {false, true}) {
        auto queries = std::make_shared<Queries>();
        auto services = std::make_shared<FundingServices>();
        auto manager = must(a::Manager::create(fixture(), {queries, {}, {}, services, {}, {}, {}}));
        auto scripts = scheduler();
        const auto step = [&](std::uint64_t tick) { queries->world.clocks = {tick, tick}; must(manager->service({tick, tick}, {}, scripts)); };
        step(0); step(1); step(2);
        ai::funding::GoalRequest goal;
        goal.id = 7; goal.category = 0; goal.cost = must(ai::funding::Amount::from_integer(100));
        must(services->funding.reserve(2, goal));
        const auto before = services->funding.ledger(2);
        const auto policy = manager->lifecycle().players.front().policy;
        expect(!before->equations_changed, "funding starts with unchanged equations");
        queries->values["TriggerC"] = 1; queries->values["LeaveC"] = departing ? 1 : 0;
        step(3);
        expect(services->clears.size() == (departing ? 1U : 0U), "WGA-103 one clear on departure; trigger-only never clears");
        if (!departing) { expect(!services->funding.ledger(2)->equations_changed && services->funding_writes == 0 && services->policy_writes.back() == a::PolicyWrite::ordinary, "switch-only activation leaves funding marker alone"); continue; }
        const auto after = services->funding.ledger(2);
        expect(manager->lifecycle().players.front().policy == policy && after->equations_changed, "identical survivor policy still marks the real funding consumer changed");
        expect(services->policy_writes.back() == a::PolicyWrite::reversal, "header-faithful consumer receives explicit reversal without hidden clear state");
        expect(after->goals == before->goals && after->dead_goals == before->dead_goals, "reversal retains live/dead goal reservations");
        for (const auto& [id, category] : before->categories) {
            const auto& cleared = services->clears.front().categories.at(id);
            expect(!cleared.evaluated_weight && cleared.equation_priority == 1, "real clear removes every evaluator and resets its priority");
            const auto& replayed = after->categories.at(id);
            expect(replayed.evaluated_weight == category.evaluated_weight && replayed.cash == category.cash && replayed.available_cash == category.available_cash &&
                replayed.allowance == category.allowance && replayed.allowance_priority == category.allowance_priority, "replay retains funds/allowances and reinstalls evaluator");
        }
    }
}
void equal_funding_write() {
    auto setup = fixture();
    auto& player = setup.xml.at("data/xml/ai/players/fixture.xml");
    const auto vector = player.find("Default A B C"); player.replace(vector, 13, "Default Equal");
    auto& templates = setup.xml.at("data/xml/ai/templates/fixture.xml");
    templates.insert(templates.find("</AITemplates>"), "<Equal><Priority>1</Priority><Trigger>TriggerA</Trigger><Budget><Offensive>One</Offensive></Budget></Equal>");
    auto queries = std::make_shared<Queries>(); auto services = std::make_shared<FundingServices>();
    auto manager = must(a::Manager::create(std::move(setup), {queries, {}, {}, services, {}, {}, {}}));
    auto scripts = scheduler();
    queries->world.clocks = {0, 0}; must(manager->service({0, 0}, {}, scripts));
    const auto before = manager->lifecycle().players.front().policy;
    queries->values["TriggerA"] = 1; queries->world.clocks = {1, 1}; must(manager->service({1, 1}, {}, scripts));
    expect(manager->lifecycle().players.front().policy == before && services->funding_writes == 1 && services->clears.empty() &&
        services->funding.ledger(2)->equations_changed, "WGA-68 identical authored setter is delivered without reversal");
}

void worker_equality() {
    std::string reference;
    std::vector<a::ServiceTrace> trace;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        Rig rig;
        for (std::uint64_t tick = 0; tick < 8; ++tick) {
            rig.step(tick, tick);
            auth::ServiceOptions options; options.host_paced = [](std::uint64_t) { return true; };
            must(rig.scripts.service(executor, options));
        }
        const auto digest = must(rig.manager->lifecycle_hash()) + must(rig.scripts.state_hash());
        if (reference.empty()) { reference = digest; trace = rig.manager->trace(); }
        else expect(digest == reference && rig.manager->trace() == trace, "1/2/4/8 lifecycle/scheduler state and ordered service traces");
    }
}

void contributor_state() {
    static_assert(std::is_base_of_v<a::StateContributor, a::Manager>);
    Rig original; original.queries->values["TriggerA"] = 1;
    original.step(0, 0); original.step(1, 1);
    const auto saved = must(original.manager->capture());
    const auto hash = must(original.manager->state_hash());
    const auto wire = must(a::encode_native_state(saved));
    expect(hash == must(a::native_state_hash(saved)), "manager hash uses shared canonical native envelope");
    Rig resumed; resumed.queries->values = original.queries->values;
    must(resumed.manager->restore(must(a::decode_native_state(wire))));
    expect(must(resumed.manager->state_hash()) == hash, "real contributor captures/restores the same boundary");
    original.step(4, 4); resumed.step(4, 4);
    expect(original.manager->lifecycle() == resumed.manager->lifecycle() && resumed.services->initialization.empty(), "contributor first resumed consumer preserves activation order, policy and deadlines");
    expect(must(a::encode_native_state(saved)) == wire, "held native snapshot owns unchanged bytes across later services");
    auto raw = resumed.manager->lifecycle();
    raw.players.front().policy.allowances.at("OFFENSIVE").value = a::Number::from_repr(0x8000000000000000ULL);
    must(resumed.manager->restore_lifecycle(raw));
    const auto exact = must(resumed.manager->capture());
    Rig exact_copy; must(exact_copy.manager->restore(exact));
    expect(exact_copy.manager->lifecycle().players.front().policy.allowances.at("OFFENSIVE").value.repr == 0x8000000000000000ULL, "native state preserves raw Number signed zero");
    const auto before = resumed.manager->lifecycle();
    const auto reject = [&](a::NativeState bad, std::string_view why) {
        expect(!resumed.manager->restore(bad) && resumed.manager->lifecycle() == before, why);
    };
    auto bad = saved; bad.version = 2; reject(bad, "unsupported native schema rejected atomically");
    bad = saved; bad.components.clear(); reject(bad, "missing owned component rejected atomically");
    bad = saved; bad.components.at(std::string(a::lifecycle_component)).pop_back(); reject(bad, "truncated component rejected atomically");
    bad = saved; bad.components.at(std::string(a::lifecycle_component)).push_back(0); reject(bad, "trailing component bytes rejected atomically");
    bad = saved;
    auto& flags = bad.components.at(std::string(a::lifecycle_component));
    const std::string lifecycle_domain = "eawr-galactic-ai-lifecycle-v2";
    const auto frame = std::search(flags.begin(), flags.end(), lifecycle_domain.begin(), lifecycle_domain.end());
    expect(frame != flags.end(), "lifecycle domain exists in native fixture");
    if (frame != flags.end()) {
        const auto processing = static_cast<std::size_t>(frame - flags.begin()) + lifecycle_domain.size() + 32;
        flags[processing] = 2;
        reject(bad, "noncanonical flag rejected atomically");
    }
    bad = saved;
    auto& bytes = bad.components.at(std::string(a::lifecycle_component));
    const std::string category = "OFFENSIVE";
    const auto location = std::search(bytes.begin(), bytes.end(), category.begin(), category.end());
    expect(location != bytes.end(), "funding row exists in native fixture");
    if (location != bytes.end()) {
        const auto offset = static_cast<std::size_t>(location - bytes.begin()) + category.size();
        for (std::size_t n = 0; n < 8; ++n) bytes[offset + n] = n == 7 ? 0x7f : 0xff;
        reject(bad, "out-of-range priority rejected atomically");
    }
    auto changed = fixture();
    auto& player = changed.xml.at("data/xml/ai/players/fixture.xml");
    const auto vector = player.find("Default A B C"); player.replace(vector, 13, "Default B A C");
    auto mismatch = must(a::Manager::create(std::move(changed), {std::make_shared<Queries>(), {}, {}, std::make_shared<Services>(), {}, {}, {}}));
    const auto fresh = mismatch->lifecycle();
    expect(!mismatch->restore(saved) && mismatch->lifecycle() == fresh, "same-size changed template vector cannot alias saved indices");
    auto aggregate = saved; aggregate.components["other-owner"] = {1};
    Rig joined; must(joined.manager->restore(aggregate));
    expect(must(joined.manager->state_hash()) == hash, "contributor accepts independently owned aggregate components");
    Rig failed;
    expect(!failed.manager->service({1, 1}, {}, failed.scripts) && !failed.manager->capture() && !failed.manager->state_hash(),
        "failed incomplete service cannot become a checkpoint boundary");
    must(failed.manager->restore(saved));
    expect(must(failed.manager->state_hash()) == hash, "explicit restore recovers only a validated completed state");
}

// This is a fast-forward of observer traffic through the actual append owners,
// not a million-tick retail campaign. Native learning remains authored state.
void bounded_diagnostics(std::size_t workers) {
    constexpr std::uint64_t ticks = 1000000;
    constexpr auto capacity = script::foc::ai_diagnostic_capacity;
    struct WindowServices final : Services {
        std::uint64_t count{};
        core::Result<void> run(const a::ServiceInput& input, auth::ScriptScheduler& scripts) override {
            ++count;
            calls.clear(); // the test observer must not itself grow with time
            return Services::run(input, scripts);
        }
    };
    const auto rig = [] {
        auto result = std::make_unique<Rig>();
        result->services = std::make_shared<WindowServices>();
        result->manager = must(a::Manager::create(fixture(), {result->queries, {}, {}, result->services, {}, {}, {}}));
        return result;
    };
    auto live = rig(), reference = rig();
    platform::ThreadWorkerAdapter executor(workers);
    const auto host = [&] {
        auto result = std::make_shared<script::foc::detail::Host>();
        auto world = std::make_shared<ai::GalacticWorld>(ai::GalacticPerceptionSetup{});
        const auto snapshot = std::make_shared<a::gc::GalacticSnapshot>(0, a::gc::GalacticClock{},
            std::vector<a::gc::GalacticPlayer>{}, std::vector<a::gc::Planet>{}, std::vector<a::gc::Route>{},
            std::vector<a::gc::Fleet>{}, std::vector<a::gc::GalacticEvent>{}, std::nullopt, std::nullopt, nullptr);
        must(world->bind(snapshot, {}, executor, 0, 1000));
        result->ai_world = world;
        result->view = world->view();
        result->setup.journal = std::make_shared<script::foc::AiJournal>();
        return result;
    };
    auto live_host = host(), reference_host = host();
    auto engine = std::make_shared<ai::Engine>(live_host, ai::AiData{}, std::vector<ai::PlanDef>{});
    auto reference_engine = std::make_shared<ai::Engine>(reference_host, ai::AiData{}, std::vector<ai::PlanDef>{});
    const std::string identity(64, '0');
    ai::GoalStateContributor native(engine, identity), reference_native(reference_engine, identity);
    std::array<std::size_t, 6> plateau{};
    const auto storage = [&] {
        const auto& journal = *live_host->setup.journal;
        return std::array<std::size_t, 6>{live->manager->trace().capacity(), engine->records().capacity(),
            engine->costs().capacity(), journal.plans.capacity(), journal.costs.capacity(), journal.services.capacity()};
    };
    for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
        for (auto* current : {live.get(), reference.get()}) {
            // Exercise template activation/reversal decisions while logs wrap.
            current->queries->values["TriggerA"] = a::Number((tick / 64) % 2);
            current->queries->values["LeaveA"] = a::Number(1 - (tick / 64) % 2);
            current->step(tick, tick);
        }
        expect(live->services->calls.back().service == reference->services->calls.back().service
            && *live->services->calls.back().policy == *reference->services->calls.back().policy,
            "diagnostic retention leaves the service decision/policy unchanged");
        expect(ai::EngineContractAccess::choice(*engine) == ai::EngineContractAccess::choice(*reference_engine),
            "diagnostic retention leaves the next synchronized choice unchanged");
        for (const auto& current : {engine, reference_engine}) {
            ai::EngineContractAccess::observe(*current, tick);
            auth::ServiceReport report; report.tick = tick;
            must(current->after_service(report));
            expect(report.commands.empty(), "diagnostics do not introduce commands");
        }
        for (const auto& current : {live_host, reference_host})
            current->setup.journal->append_service({tick, 2, true, true, true});
        const auto& journal = *live_host->setup.journal;
        expect(engine->records().size() + engine->records_dropped() == tick
            && engine->costs().size() + engine->costs_dropped() == tick
            && journal.plans.size() + journal.plans_dropped == tick
            && journal.costs.size() + journal.costs_dropped == tick
            && journal.services.size() + journal.services_dropped == tick,
            "every retained or dropped diagnostic is accounted for");
        expect(engine->costs().back().tick == tick && journal.costs.back().tick == tick
            && journal.plans.back().tick == tick && journal.services.back().frame == tick,
            "the newest diagnostic remains available after compaction");
        if (tick == 2 * capacity) plateau = storage();
        if (tick >= 2 * capacity) expect(storage() == plateau, "all six vector allocations stay flat after warm-up");
        if (tick % 1024 == 0 || tick == ticks) {
            std::array<std::string, 2> lifecycle_hashes, native_hashes;
            must(executor.execute_phase("bounded-ai-diagnostic-checkpoint", 2, [&](std::size_t index) {
                lifecycle_hashes[index] = must((index == 0 ? live : reference)->manager->state_hash());
                native_hashes[index] = must((index == 0 ? native : reference_native).state_hash());
            }));
            expect(lifecycle_hashes[0] == lifecycle_hashes[1] && native_hashes[0] == native_hashes[1],
                "different retained diagnostic histories leave canonical hashes unchanged");
            expect(must(native.capture()).components == must(reference_native.capture()).components,
                "diagnostics and dropped counters remain outside saved native bytes");
            const auto& trace = live->manager->trace();
            expect(trace.size() <= capacity && trace.size() + live->manager->trace_dropped()
                == static_cast<WindowServices&>(*live->services).count, "trace retention/drop accounting");
            expect(std::is_sorted(trace.begin(), trace.end(), [](const auto& left, const auto& right) {
                return left.clocks.synchronization < right.clocks.synchronization;
            }), "compaction preserves chronological service order");
            // Independent reference: discard observations, retain all native state.
            must(reference->manager->restore_lifecycle(reference->manager->lifecycle()));
            ai::EngineContractAccess::clear_diagnostics(*reference_engine);
            reference_host->setup.journal->plans.clear(); reference_host->setup.journal->costs.clear();
            reference_host->setup.journal->services.clear();
        }
    }
    expect(reference->manager->trace_dropped() == 0 && reference_engine->records_dropped() == 0
        && reference_engine->costs_dropped() == 0, "the drained reference never overflows");
    expect(std::all_of(plateau.begin(), plateau.end(), [](auto value) { return value >= capacity; }),
        "the million-tick test actually saturates every diagnostic container");
    for (const auto size : storage()) expect(size <= 2 * capacity, "bounded diagnostic storage envelope");
    const auto checkpoint = must(native.capture());
    auto restored = std::make_shared<ai::Engine>(host(), ai::AiData{}, std::vector<ai::PlanDef>{});
    ai::GoalStateContributor restored_native(restored, identity);
    must(restored_native.restore(checkpoint));
    expect(must(restored_native.state_hash()) == must(native.state_hash())
        && ai::EngineContractAccess::choice(*restored) == ai::EngineContractAccess::choice(*engine),
        "fresh-owner restore continues the same native hash and synchronized choices");
    std::cout << "bounded diagnostics: " << ticks << " ticks, " << workers
        << " workers, six flat storage capacities, unchanged native/lifecycle hashes\n";
}

std::optional<std::string> game_root() {
#if defined(_MSC_VER)
    char* value{}; std::size_t size{};
    if (_dupenv_s(&value, &size, "EAWR_EAW_GAME_ROOT") || !value) return {};
    const std::string root(value); std::free(value); return root;
#else
    const auto* value = std::getenv("EAWR_EAW_GAME_ROOT"); return value ? std::optional<std::string>{value} : std::nullopt;
#endif
}
void installed_loading() {
    const auto root = game_root();
    if (!root || root->empty()) throw std::runtime_error("installed galactic AI loading requires EAWR_EAW_GAME_ROOT; unavailable data is not success");
    std::vector<vfs::MountSpec> mounts;
    for (const auto folder : {"corruption", "GameData"}) mounts.push_back(must(vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data")).mount);
    auto files = must(vfs::Vfs::mount(mounts));
    const auto read = [&](std::string_view path) {
        const auto bytes = must(files.open(path));
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    a::LifecycleSetup setup;
    for (const auto& path : a::required_xml()) setup.xml[path] = read(path);
    const auto data = must(a::load_data(setup));
    expect(data.players.at("BASICEMPIRE").galactic_templates.front() == "BASIC_EMPIRE_DEFAULT", "stock selected Empire template");
    expect(data.players.at("BASICEMPIRENOGALACTICAI").galactic_templates == std::vector<std::string>{"NONE_TEMPLATE"} && !data.players.at("BASICEMPIRENOGALACTICAI").freestore.contains("GALACTIC"), "stock restricted Empire has no ordinary freestore");
    expect(data.players.at("BASICREBELNOGALACTICAI").galactic_templates == std::vector<std::string>{"NONE_TEMPLATE"}, "stock restricted Rebel templates");
    expect(data.players.at("AI_PLAYER_UNDERWORLDSTORYOPPONENT").galactic_templates == std::vector<std::string>{"REMOVE_CORRUPTION_ONLY_TEMPLATE"}, "stock story opponent retains its distinct template");
    expect(data.templates.at("BASIC_EMPIRE_DEFAULT").tactical_allowances.empty(), "commented ordinary allowances are absent");
    expect(data.difficulties.at("NORMAL_DEFAULT").galactic_build_time_multiplier == *ai::parse_real("0.8") && data.difficulties.at("EASY_DEFAULT").galactic_contrast_multiplier == *ai::parse_real("0.67"), "authored galactic difficulty distinct from tactical");
    bool effective_magic{};
    for (const auto& function : data.function_sets.at("BASICDEFENSIVEGALACTICSET")) if (function.goal == "GENERATE_MAGIC_SPACE_DEFENSE") effective_magic = function.function == "Needs_Magic_Space_Defense";
    expect(effective_magic, "stock later Function overrides initial Zero");

    // Supporting loading proof: actual stock chunk/Base_Definitions through the
    // production FoC bindings. This does not claim live freestore action coverage.
    auth::ModuleManifest manifest;
    for (const auto path : {"Data/Scripts/FreeStore/GalacticFreeStore.lua", "Data/Scripts/FreeStore/GalacticHeroFreeStore.lua", "Data/Scripts/Library/PGCommands.lua", "Data/Scripts/Library/PGBaseDefinitions.lua", "Data/Scripts/Library/PGBase.lua", "Data/Scripts/Library/PGDebug.lua", "Data/Scripts/Library/PGAICommands.lua"}) must(manifest.add(path, read(path)));
    std::string reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auth::SessionConfig config; config.galactic_clock = true; config.tick_duration = {1, 30}; config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/FreeStore/"};
        auto scripts = must(auth::ScriptScheduler::create(config, manifest));
        auto host = std::make_shared<script::foc::detail::Host>();
        host->view = std::make_shared<const script::foc::detail::WorldView>();
        std::vector<core::Diagnostic> errors;
        script::foc::tactical_ai_detail::register_globals(scripts, host, errors);
        script::foc::tactical_ai_detail::register_methods(scripts, host, errors);
        if (!errors.empty()) throw std::runtime_error(errors.front().message);
        must(scripts.create_instance(1002, "Data/Scripts/FreeStore/GalacticFreeStore.lua"));
        auth::ScriptEvent definitions;
        definitions.key = {1, script::foc::producer_foc_engine, 1002, 0}; definitions.target = 1002;
        definitions.kind = auth::ScriptEvent::Kind::call; definitions.name = "Base_Definitions";
        must(scripts.submit_event(definitions));
        platform::ThreadWorkerAdapter executor(workers);
        auth::ServiceOptions options; options.host_paced = [](std::uint64_t) { return true; };
        const auto report = must(scripts.service(executor, options));
        expect(report.diagnostics.empty(), "stock Base_Definitions has no missing API faults");
        const auto rate = must(scripts.read_global(1002, "ServiceRate"));
        const auto unit_rate = must(scripts.read_global(1002, "UnitServiceRate"));
        const auto misspelled = must(scripts.read_global(1002, "UnitServciceRate"));
        expect(rate && std::get<a::Number>(rate->data) == a::Number(8) && unit_rate && std::holds_alternative<std::monostate>(unit_rate->data) && misspelled && std::get<a::Number>(misspelled->data) == a::Number(8), "actual stock main rate and absent numeric unit callback rate");
        const auto digest = must(scripts.state_hash());
        if (reference.empty()) reference = digest; else expect(reference == digest, "1/2/4/8 stock definition graph equality");
    }
}
} // namespace

int main(int argc, char** argv) {
    try { if (argc > 1 && std::string_view(argv[1]) == "--bounded") bounded_diagnostics(argc > 2 ? static_cast<std::size_t>(std::stoul(argv[2])) : 1); else if (argc > 1 && std::string_view(argv[1]) == "--state") contributor_state(); else if (argc > 1) installed_loading(); else { category_width(); cadence(); configured_cadence(); templates(); template_failure_consumers(); restore(); reversal_funding(); equal_funding_write(); worker_equality(); } }
    catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    return failures ? 1 : 0;
}
