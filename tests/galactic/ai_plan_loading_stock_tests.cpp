#include <array>
#include <cstdint>
#include <exception>
#include <map>
#include <vector>
#include <cstddef>
#include <string_view>
#include <string>
#include "../support/env.hpp"
#include "../support/check.hpp"
#include "../support/result.hpp"
// WGA-25/38/42: stock definition/pacing data, independent of live world actions.
#if defined(_MSC_VER) && __has_include(<hash_map>)
#define _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS
#include <hash_map>
#define EAWR_CATEGORY_MAP_ORACLE 1
#endif

#include "galactic_ai_plans.hpp"
#include "galactic_ai.hpp"
#include "ai_goal_state_stock.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <utility>
using eawr::testing::must;

int galactic_learning_contracts();

namespace {
namespace ai = eawr::script::foc::ai;
namespace g = ai::galactic_goals;
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool ok, const char* message) {
    if (!ok) local_checker.check_before_print(ok, message, std::cerr, "FAIL: ");
}

const g::PlanDefinitionData& plan(const g::PlanCatalog& catalog, std::string_view name) {
    const auto found = std::find_if(catalog.plans.begin(), catalog.plans.end(),
        [&](const auto& row) { return row.name == name; });
    if (found == catalog.plans.end()) throw std::runtime_error("Missing plan " + std::string(name));
    return *found;
}
void category_order_contracts() {
    const std::array<std::uint64_t, 7> collisions{16, 1, 8, 32, 2, 64, 16};
    expect(g::galactic_contrast_category_order(collisions) == std::vector<std::uint64_t>{8, 16, 32, 64, 1, 2},
        "native category collisions sort within first-encounter buckets and duplicates retain the first key");
    constexpr auto wide = UINT64_C(0x8000000000000001);
    const std::array<std::uint64_t, 9> growth{9, wide, 8, 16, 2, 3, 4, 32, 1};
    expect(g::galactic_contrast_category_order(growth) == std::vector<std::uint64_t>{9, 1, wide, 8, 16, 32, 2, 3, 4},
        "ninth unique category regroups the retained map and compares full unsigned 64-bit keys");
    expect(g::galactic_contrast_category_order({}).empty(), "empty authored category map remains empty");
#ifdef EAWR_CATEGORY_MAP_ORACLE
    // A separate library implementation checks the portable ordering at
    // collisions and multiple growth boundaries. The original policy itself
    // is qualified by WGA-448, not inferred from this compiler's library.
    std::vector<std::uint64_t> extended(growth.begin(), growth.end());
    for (std::uint64_t index = 0; index < 617; ++index) extended.push_back((index * 37) % 617);
    for (const auto& keys : {std::vector<std::uint64_t>(collisions.begin(), collisions.end()), extended}) {
        stdext::hash_map<std::uint64_t, bool> reference;
        for (const auto key : keys) reference.emplace(key, true);
        std::vector<std::uint64_t> expected;
        for (const auto& [key, value] : reference) { (void)value; expected.push_back(key); }
        expect(g::galactic_contrast_category_order(keys) == expected,
            "portable category iteration agrees with an independent native container oracle across growth boundaries");
    }
#endif
    eawr::script::foc::detail::Host host;
    eawr::script::foc::AiType unit; unit.type_id = 10; unit.name = "UNIT";
    host.setup.content.types = {unit}; host.types_by_name.emplace("UNIT", &host.setup.content.types.front());
    g::PlanDefinitionData authored; authored.name = "ORDERED"; authored.module = "ordered.lua";
    authored.goals = {"ONE"}; authored.teams = {{"Main", {"Unit = 1"}}};
    for (const auto key : collisions) {
        const auto name = "K" + std::to_string(key); host.setup.content.categories.emplace(name, key);
        authored.contrast.push_back({name, {{"UNIT", ai::real(authored.contrast.empty() ? 1 : 9)}}});
    }
    g::PlanCatalog catalog; catalog.plans = {authored};
    const auto materialized = must(g::materialize_plan_catalog(host, catalog));
    expect(materialized.plans.front().galactic_contrast_order == g::galactic_contrast_category_order(collisions)
        && materialized.plans.front().galactic_contrast.at(16).front().weight == ai::real(1),
        "materialization resolves authored keys to retained map order and preserves the first duplicate's weighted list");
}
void fixture() {
    category_order_contracts();
    goal_state_contracts();
    const std::string script = R"LUA(
function Base_Definitions()
 Category = "One | Two"
 TaskForce = {{"Main", "Infantry = 0,2", "Vehicle = 50%", "-Excluded", "DenyHeroAttach"}}
 MagicPlan = false
 MinimumCost = 17
 AdditionalCost = 3
 MinPlanCost = 99
 MaxPlanCost = 99
 EnemyContrastTypes = {"Ground"}
 FriendlyContrastTypes = {WeightedTypeList.Create()}
 FriendlyContrastTypes[1].Parse({"Infantry", "Unit"}, {1, 3})
end
function DifficultyBasedMinPause(difficulty)
 if difficulty == "Easy" then return 11 end
 if difficulty == "Normal" then return 9 end
 return 7
end
function main() error("definition loading must not run a plan thread") end
)LUA";
    std::map<std::string, std::string> modules{{"data/scripts/ai/example.lua", script}};
    const auto catalog = must(g::load_plan_catalog({}, modules));
    const auto& definition = plan(catalog, "EXAMPLE");
    expect(definition.goals == std::vector<std::string>{"ONE", "TWO"}, "goal names normalized without merging categories");
    expect(definition.teams.front().declarations == std::vector<std::string>{"Infantry = 0,2", "Vehicle = 50%", "-Excluded", "DenyHeroAttach"}, "team ranges, percentages, exclusions, flags retained");
    expect(definition.minimum_cost == ai::real(17) && definition.additional_cost == ai::real(3),
        "source plan globals MinimumCost and AdditionalCost are retained; invented MinPlanCost/MaxPlanCost globals are ignored");
    expect(definition.magic == false && !definition.allow_freestore && !definition.minimum_contrast, "absent differs from false/default data");
    expect(definition.contrast.size() == 1 && definition.contrast.front().first == "GROUND"
        && definition.contrast.front().second == std::vector<std::pair<std::string, ai::Real>>{{"INFANTRY", ai::real(1)}, {"UNIT", ai::real(3)}},
        "definition retains its own ordered weighted list including exact-type overrides");
    auto varied = modules;
    varied.emplace("data/scripts/ai/other.lua", script + "\nlocal base = Base_Definitions\nfunction Base_Definitions() base(); FriendlyContrastTypes[1].Parse({'Vehicle'}, {7}) end");
    const auto independent = must(g::load_plan_catalog({}, varied));
    expect(plan(independent, "EXAMPLE").contrast == definition.contrast
        && plan(independent, "OTHER").contrast.front().second == std::vector<std::pair<std::string, ai::Real>>{{"VEHICLE", ai::real(7)}},
        "separate definition VMs preserve each plan's custom matrix without global fallback");
    expect(catalog.minimum_pause_seconds.at("Easy") == ai::real(11)
        && catalog.minimum_pause_seconds.at("Normal") == ai::real(9)
        && catalog.minimum_pause_seconds.at("Hard") == ai::real(7), "pause table comes from executed Lua, not production literals");
    ai::GoalType goal; goal.name = "One"; goal.mode = "GALACTIC";
    g::ProposalCounts counts; counts.ordinary["ONE"] = 1;
    eawr::script::foc::galactic_ai::TemplatePolicy policy;
    expect(!g::activation_allowed(definition, goal, policy, counts), "missing materialized policy does not activate");
    policy.plan_names["EXAMPLE"] = {true, 1};
    policy.goal_types["ONE"] = {true, 1};
    expect(g::activation_allowed(definition, goal, policy, counts), "matching goal and materialized policy admit activation");
    policy.plan_names["EXAMPLE"] = {false, 4};
    expect(!g::activation_allowed(definition, goal, policy, counts), "explicit plan-name disable");
    policy.plan_names["EXAMPLE"] = {true, 1}; counts.exclusions["ONE"] = 1;
    expect(!g::activation_allowed(definition, goal, policy, counts), "global exclusion blocks activation");
    counts.exclusions["ONE"] = 0; goal.mode = "SPACE";
    expect(!g::activation_allowed(definition, goal, policy, counts), "wrong manager mode rejected");
    modules["data/scripts/ai/bad.lua"] = "function Base_Definitions() end\nfunction DifficultyBasedMinPause(d) return 1 end";
    expect(!g::load_plan_catalog({}, modules), "malformed plan fails whole catalog instead of silently dropping it");
    modules.erase("data/scripts/ai/bad.lua");
    modules["data/scripts/ai/example.lua"] = script + "\nlocal original = Base_Definitions\nfunction Base_Definitions() original(); MagicPlan = 'wrong' end";
    expect(!g::load_plan_catalog({}, modules), "malformed override is not silently treated as absent");
    modules["data/scripts/ai/example.lua"] = "require('MissingLibrary')";
    expect(!g::load_plan_catalog({}, modules), "missing stock dependency fails");
    expect(!g::load_plan_catalog({}, {}), "empty module set fails");
}
std::string game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("Stock loading requires EAWR_EAW_GAME_ROOT");
    if (root->empty()) throw std::runtime_error("Stock loading requires a nonempty game root");
    return std::move(*root);
}
std::map<std::string, std::string> modules(const eawr::vfs::Vfs& fs) {
    std::map<std::string, std::string> out;
    for (const auto& asset : must(fs.enumerate("data/scripts", ".lua"))) {
        if (!asset.canonical_path.starts_with("data/scripts/library/")
            && !(asset.canonical_path.starts_with("data/scripts/ai/")
                && asset.canonical_path.substr(16).find('/') == std::string::npos)) continue;
        const auto bytes = must(fs.open(asset.canonical_path));
        std::string text(bytes.size(), '\0');
        if (!text.empty()) std::memcpy(text.data(), bytes.data(), text.size());
        out.emplace(asset.canonical_path, std::move(text));
    }
    return out;
}
void installed() {
    const auto root = std::filesystem::path(game_root());
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    auto fs = must(eawr::vfs::Vfs::mount(mounts));
    const auto stock_modules = modules(fs);
    const auto catalog = must(g::load_plan_catalog({}, stock_modules));
    namespace lifecycle = eawr::script::foc::galactic_ai;
    lifecycle::LifecycleSetup selected;
    for (const auto& path : lifecycle::required_xml()) {
        const auto bytes = must(fs.open(path));
        std::string text(bytes.size(), '\0');
        if (!text.empty()) std::memcpy(text.data(), bytes.data(), text.size());
        selected.xml.emplace(path, std::move(text));
    }
    const auto data = must(lifecycle::load_data(selected));
    // Exact mounted closure identity, with unambiguous length framing.
    std::vector<std::uint8_t> closure;
    const auto append = [&](std::string_view value) {
        for (unsigned i = 0; i < 8; ++i) closure.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(value.size()) >> (i * 8U)));
        closure.insert(closure.end(), value.begin(), value.end());
    };
    for (const auto& [path, text] : selected.xml) { append(path); append(text); }
    for (const auto& [path, text] : stock_modules) { append(path); append(text); }
    installed_goal_state(fs, data, g::goal_plan_registrations(catalog), eawr::core::sha256_hex(closure));
    const auto galactic_goals = std::count_if(data.goals.begin(), data.goals.end(),
        [](const auto& entry) { return entry.second.mode == "GALACTIC"; });
    expect(galactic_goals == 61, "selected stock galactic closure loads 61 goals through production data loader");
    expect(catalog.plans.size() == 55, "all 55 top-level stock definitions load, including disabled plans");
    // WGA-25 plan catalog, plus its two previously omitted AlwaysOff entries.
    const auto names = ai::split_names(R"NAMES(
AI_PLAN_EXPANSIONGENERIC_GENERATEMAGICCASHDROP AI_PLAN_EXPANSIONGENERIC_REMOVECORRUPTION
AI_PLAN_UNDERWORLD_BLACKMARKET AI_PLAN_UNDERWORLD_BUILDUNDERWORLDBARRACKS
AI_PLAN_UNDERWORLD_BUILDUNDERWORLDDROIDWORKS AI_PLAN_UNDERWORLD_BUILDUNDERWORLDPALACE
AI_PLAN_UNDERWORLD_BUILDUNDERWORLDVEHICLEFACTORY AI_PLAN_UNDERWORLD_DEPLOYSABOTEUR AI_PLAN_UNDERWORLD_HACKSUPERWEAPON
BUILDBASECOMPONENTADVANCEDVEHICLEFACTORY BUILDBASECOMPONENTBARRACKS BUILDBASECOMPONENTCOMMARRAY
BUILDBASECOMPONENTHEAVYVEHICLEFACTORY BUILDBASECOMPONENTLIGHTVEHICLEFACTORY BUILDBASECOMPONENTOFFICERACADEMY
BUILDBASECOMPONENTRESEARCHFACILITY BUILDBASESHIELDPLAN BUILDCANTINAPLAN BUILDECONOMICSTRUCTUREPLAN
BUILDGROUNDDEFENSIVESTRUCTUREPLAN BUILDGROUNDFORCESPLAN BUILDHUTTPALACEPLAN BUILDINFILTRATORFACILITY
BUILDJAMMINGSTATIONPLAN BUILDSCANNERPLAN BUILDSPACEDEFENSIVESTRUCTUREPLAN BUILDSPACEFORCESPLAN BUILDTURBOLASERS
BUILDWEAPONSTRUCTUREPLAN CONQUEROPPONENTPLAN CONQUERPIRATEPLAN CONTRASTTORECONNECT CRUSHPLAN DEATHSTARUSEPLAN
DEPLOYUNITONTARGET EMPIREADVANCETECHPLAN FLUSHMAJORITEMBUDGET GENERATEMAGICGROUNDDEFENSE
GENERATEMAGICGROUNDDEFENSEEASY GENERATEMAGICGROUNDDEFENSEHARD GENERATEMAGICGROUNDSTRUCTURE
GENERATEMAGICSPACEDEFENSE GENERATEMAGICSPACEDEFENSEEASY GENERATEMAGICSPACEDEFENSEHARD LIFTBLOCKADEPLAN
PATHBONUSFORDISTANTTARGETS PLANTSPYPLAN RAIDPLAN REBELADVANCETECHPLAN SABOTAGEPOLITICALCONTROLPLAN
SIPHONCREDITSPLAN UNRESTRICTEDGRABLAND UNRESTRICTEDGRABSPACE UPGRADEGROUNDBASEPLAN UPGRADESTARBASEPLAN
)NAMES", " \t\r\n");
    std::set<std::string> actual;
    for (const auto& definition : catalog.plans) actual.insert(definition.name);
    expect(actual == std::set<std::string>(names.begin(), names.end()), "stock loaded set matches the complete catalog");
    const auto& conquest = plan(catalog, "CONQUEROPPONENTPLAN");
    expect(conquest.goals == std::vector<std::string>{"CONQUER_OPPONENT"}
        && conquest.teams.size() == 2 && conquest.teams[0].name == "SpaceForce"
        && conquest.teams[1].name == "GroundForce", "stock conquest retains separate domain teams");
    expect(conquest.minimum_contrast == ai::parse_real("1.25")
        && conquest.maximum_contrast == ai::parse_real("1.75")
        && conquest.failure_adjust == ai::parse_real("0.5"), "stock conquest authored contrast overrides");
    const auto& reserve = plan(catalog, "BUILDSPACEFORCESPLAN");
    expect(reserve.allow_freestore == false && reserve.ignore_target == true, "stock reserve production declines free store");
    expect(std::find(reserve.teams[0].declarations.begin(), reserve.teams[0].declarations.end(), "DenyHeroAttach")
        != reserve.teams[0].declarations.end(), "stock reserve hero exclusion retained");
    const auto& cash = plan(catalog, "AI_PLAN_EXPANSIONGENERIC_GENERATEMAGICCASHDROP");
    expect(cash.magic == true && cash.magic_stealing == false && cash.allow_freestore == false, "stock magic flags retained independently");
    for (const auto name : {"BUILDGROUNDDEFENSIVESTRUCTUREPLAN", "BUILDSPACEDEFENSIVESTRUCTUREPLAN"})
        expect(plan(catalog, name).goals == std::vector<std::string>{"ALWAYSOFF"}, "two omitted defensive plans stay disabled");
    expect(catalog.minimum_pause_seconds.at("Easy") == ai::real(300)
        && catalog.minimum_pause_seconds.at("Normal") == ai::real(200)
        && catalog.minimum_pause_seconds.at("Hard") == ai::real(45), "WGA-42 stock difficulty pause branches execute");
    std::vector<lifecycle::PlanInput> registrations;
    for (const auto& definition : catalog.plans) registrations.push_back({definition.name, definition.goals});
    const auto& empire = data.players.at("BASICEMPIRE");
    expect(!empire.galactic_templates.empty(), "stock selected type retains galactic template list");
    const std::array<std::string, 3> levels{"EASY", "NORMAL", "HARD"};
    const std::array<std::string, 3> contrast{"0.67", "1.1", "1.15"};
    const std::array<std::string, 3> build{"1.5", "0.8", "0.6"};
    for (std::size_t index = 0; index < levels.size(); ++index) {
        const auto& difficulty = data.difficulties.at(empire.difficulty.at(levels[index]));
        expect(difficulty.galactic_contrast_multiplier == *ai::parse_real(contrast[index])
            && difficulty.galactic_build_time_multiplier == *ai::parse_real(build[index]),
            "WGA-26/48 XML difficulty adjustments are independent of Lua pauses");
    }
    const auto policy = lifecycle::apply_template(data, registrations, {}, data.templates.at(empire.galactic_templates.front()));
    g::ProposalCounts counts;
    for (const auto& [name, setting] : policy.goal_types) counts.ordinary[name] = setting.value ? 1 : 0;
    const auto& rebel_tech = plan(catalog, "REBELADVANCETECHPLAN");
    expect(!g::activation_allowed(rebel_tech, data.goals.at("ADVANCE_TECH_REBEL"), policy, counts),
        "stock Empire default keeps Rebel tech plan disabled");
    for (const auto& definition : catalog.plans) {
        for (const auto& name : definition.goals) {
            const auto goal = data.goals.find(name);
            if (goal == data.goals.end() || goal->second.mode != "GALACTIC") continue;
            if (name == "ALWAYSOFF") expect(!g::activation_allowed(definition, goal->second, policy, counts),
                "stock AlwaysOff registration cannot activate through default policy");
        }
    }
    std::cout << "Stock plan catalog: " << catalog.plans.size() << " definitions; Easy/Normal/Hard pauses 300/200/45\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") installed();
        else fixture();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures == 0 && galactic_goal_state_failures() == 0 && galactic_learning_contracts() == 0 ? 0 : 1;
}
