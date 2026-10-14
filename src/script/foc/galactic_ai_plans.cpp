#include "galactic_ai_plans.hpp"
#include "tactical_ai_internal.hpp"

#include <algorithm>

namespace eawr::script::foc::ai::galactic_goals {
std::vector<PlanDef> goal_plan_registrations(const PlanCatalog& catalog) {
    std::vector<PlanDef> result;
    for (const auto& source : catalog.plans) {
        PlanDef row;
        row.name = source.name; row.module = source.module; row.goals = source.goals;
        row.ignore_target = source.ignore_target.value_or(false);
        row.magic = source.magic.value_or(false); row.allow_free_store = source.allow_freestore.value_or(false);
        row.allow_engaged = source.allow_engaged.value_or(true);
        row.per_failure_contrast_adjust = source.failure_adjust.value_or(Real{});
        row.min_contrast = source.minimum_contrast.value_or(Real{});
        row.max_contrast = source.maximum_contrast.value_or(Real{});
        result.push_back(std::move(row));
    }
    return result;
}
namespace {
namespace auth = authoritative;
using CatalogResult = core::Result<PlanCatalog>;

core::Diagnostic error(std::string message) {
    return {"galactic-ai.plan-catalog", core::Severity::error, std::move(message), {}, {}, {}, {}};
}
bool top_level_plan(std::string_view path) {
    const auto canonical = upper_case(path);
    constexpr std::string_view prefix = "DATA/SCRIPTS/AI/";
    return canonical.starts_with(prefix) && canonical.ends_with(".LUA")
        && canonical.substr(prefix.size()).find('/') == std::string::npos;
}
std::string stem(std::string_view path) {
    const auto start = path.find_last_of('/') + 1;
    return upper_case(path.substr(start, path.size() - start - 4));
}
std::string lua_string(std::string_view text) {
    std::string out{"\""};
    for (const auto character : text) {
        if (character == '\n') out += "\\n";
        else if (character == '\r') out += "\\r";
        else if (character == '\t') out += "\\t";
        else {
            if (character == '"' || character == '\\') out += '\\';
            out += character;
        }
    }
    return out + '"';
}
template<class T> std::optional<T> field(const auth::ValueList& values, std::size_t index) {
    if (index < values.size()) if (const auto* value = std::get_if<T>(&values[index].data)) return *value;
    return {};
}
core::Result<PlanDefinitionData> definition(std::string module, const auth::ValueList& values) {
    using Result = core::Result<PlanDefinitionData>;
    const auto categories = field<std::string>(values, 0);
    const auto teams = field<std::vector<auth::Value>>(values, 1);
    if (!categories || !teams || teams->empty()) return Result::failure(error(module + ": missing Category/TaskForce"));
    for (const auto index : {2u, 3u, 4u, 5u, 10u, 14u}) {
        if (index < values.size() && !std::holds_alternative<std::monostate>(values[index].data)
            && !std::holds_alternative<bool>(values[index].data))
            return Result::failure(error(module + ": invalid Boolean override"));
    }
    for (const auto index : {6u, 7u, 8u, 11u, 12u, 13u}) {
        if (index < values.size() && !std::holds_alternative<std::monostate>(values[index].data)
            && !std::holds_alternative<Real>(values[index].data))
            return Result::failure(error(module + ": invalid numeric override"));
    }
    if (values.size() > 9 && !std::holds_alternative<std::monostate>(values[9].data)
        && !std::holds_alternative<std::vector<auth::Value>>(values[9].data))
        return Result::failure(error(module + ": invalid required categories"));
    PlanDefinitionData out;
    out.name = stem(module); out.module = std::move(module);
    for (const auto& goal : split_names(*categories, "|")) out.goals.push_back(upper_case(goal));
    if (out.goals.empty()) return Result::failure(error(out.module + ": empty Category"));
    for (const auto& value : *teams) {
        const auto* entries = std::get_if<std::vector<auth::Value>>(&value.data);
        if (!entries || entries->empty()) return Result::failure(error(out.module + ": malformed TaskForce"));
        const auto name = field<std::string>(*entries, 0);
        if (!name || name->empty()) return Result::failure(error(out.module + ": unnamed TaskForce"));
        PlanTeamData team; team.name = *name;
        for (std::size_t index = 1; index < entries->size(); ++index) {
            const auto declaration = field<std::string>(*entries, index);
            if (!declaration) return Result::failure(error(out.module + ": non-text team declaration"));
            team.declarations.push_back(*declaration);
        }
        out.teams.push_back(std::move(team));
    }
    out.ignore_target = field<bool>(values, 2); out.magic = field<bool>(values, 3);
    out.allow_freestore = field<bool>(values, 4); out.allow_engaged = field<bool>(values, 5);
    out.failure_adjust = field<Real>(values, 6); out.minimum_contrast = field<Real>(values, 7);
    out.maximum_contrast = field<Real>(values, 8);
    if (const auto required = field<std::vector<auth::Value>>(values, 9)) {
        for (const auto& entry : *required) {
            const auto* name = std::get_if<std::string>(&entry.data);
            if (!name) return Result::failure(error(out.module + ": non-text required category"));
            out.required_categories.push_back(*name);
        }
    }
    out.magic_stealing = field<bool>(values, 10); out.minimum_cost = field<Real>(values, 11);
    out.additional_cost = field<Real>(values, 12); out.pool_count = field<Real>(values, 13);
    out.allow_special_weapons = field<bool>(values, 14);
    return Result::success(std::move(out));
}
}

CatalogResult load_plan_catalog(const AiSetup& setup, const std::map<std::string, std::string>& modules) {
    auto host = std::make_shared<detail::Host>();
    host->setup = setup;
    host->view = std::make_shared<detail::WorldView>();
    for (const auto& type : host->setup.content.types) {
        host->types.emplace(type.type_id, &type); host->types_by_name.emplace(type.name, &type);
    }
    for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
    auth::ModuleManifest manifest;
    std::vector<std::string> paths;
    for (const auto& [path, bytes] : modules) {
        if (const auto added = manifest.add(path, bytes); !added) return CatalogResult::failure(added.error());
        if (top_level_plan(path)) paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) { return upper_case(a) < upper_case(b); });
    if (paths.empty()) return CatalogResult::failure(error("No top-level galactic plan modules"));
    for (const auto& path : paths) {
        // Host glue only. Run stock definition and pause functions; never copy
        // their bodies or hardcode a retail difficulty table into production.
        const auto loader = "require(" + lua_string(path.substr(0, path.size()-4)) + ")\n"
            "PlanDefinitionLoad = true\nBase_Definitions()\n"
            "_EAWR_GalacticDefinition(Category, TaskForce, IgnoreTarget, MagicPlan, AllowFreeStoreUnits, AllowEngagedUnits,\n"
            " PerFailureContrastAdjust, MinContrastScale, MaxContrastScale, RequiredCategories, MagicPlanStealing,\n"
            " MinimumCost, AdditionalCost, ScriptPoolCount, AllowSpecialWeapons)\n"
            "_EAWR_GalacticPacing(DifficultyBasedMinPause(\"Easy\"), DifficultyBasedMinPause(\"Normal\"), DifficultyBasedMinPause(\"Hard\"))\n";
        if (const auto added = manifest.add("EAWR/GALACTIC_PLAN/" + stem(path) + ".LUA", loader); !added)
            return CatalogResult::failure(added.error());
    }
    auth::SessionConfig config;
    config.seed = setup.seed;
    config.tick_duration = {sim::tactical::tick_numerator, sim::tactical::tick_denominator};
    config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/AI/"};
    auto created = auth::ScriptScheduler::create(std::move(config), std::move(manifest));
    if (!created) return CatalogResult::failure(created.error());
    auto& scripts = created.value();
    std::vector<core::Diagnostic> errors;
    tactical_ai_detail::register_globals(scripts, host, errors);
    tactical_ai_detail::register_methods(scripts, host, errors);
    auto definitions = std::make_shared<std::map<std::uint64_t, auth::ValueList>>();
    auto pacing = std::make_shared<std::map<std::uint64_t, auth::ValueList>>();
    const auto capture = [](const auto& destination) -> auth::Binding {
        return [destination](auth::BindingContext& context, const auth::ValueList& values) {
            (*destination)[context.instance()] = values;
            return core::Result<auth::ValueList>::success({});
        };
    };
    for (auto result : {scripts.register_binding("_EAWR_GalacticDefinition", capture(definitions)),
        scripts.register_binding("_EAWR_GalacticPacing", capture(pacing))}) if (!result) errors.push_back(result.error());
    if (!errors.empty()) return CatalogResult::failure(errors.front());
    PlanCatalog out;
    std::uint64_t instance{};
    for (const auto& path : paths) {
        ++instance;
        if (const auto loaded = scripts.create_instance(instance, "EAWR/GALACTIC_PLAN/" + stem(path) + ".LUA"); !loaded)
            return CatalogResult::failure(loaded.error());
        const auto found = definitions->find(instance);
        if (found == definitions->end()) return CatalogResult::failure(error(path + ": no definition output"));
        auto plan = definition(path, found->second);
        if (!plan) return CatalogResult::failure(plan.error());
        out.plans.push_back(std::move(plan).value());
        const auto pause = pacing->find(instance);
        if (pause == pacing->end()) return CatalogResult::failure(error(path + ": no pacing output"));
        const std::array<std::string, 3> levels{"Easy", "Normal", "Hard"};
        for (std::size_t index = 0; index < levels.size(); ++index) {
            const auto value = field<Real>(pause->second, index);
            if (!value) return CatalogResult::failure(error(path + ": nonnumeric difficulty pause"));
            const auto [existing, inserted] = out.minimum_pause_seconds.try_emplace(levels[index], *value);
            if (!inserted && existing->second != *value) return CatalogResult::failure(error(path + ": inconsistent pause library"));
        }
    }
    return CatalogResult::success(std::move(out));
}

bool activation_allowed(const PlanDefinitionData& plan, const GoalType& goal,
    const foc::galactic_ai::TemplatePolicy& policy, const ProposalCounts& proposals) {
    const auto name = upper_case(goal.name);
    if (goal.mode != "GALACTIC" || !proposable(proposals, name)
        || std::find(plan.goals.begin(), plan.goals.end(), name) == plan.goals.end()) return false;
    const auto enabled_goal = policy.goal_types.find(name);
    const auto setting = policy.plan_names.find(plan.name);
    return enabled_goal != policy.goal_types.end() && enabled_goal->second.value
        && setting != policy.plan_names.end() && setting->second.value;
}

} // namespace eawr::script::foc::ai::galactic_goals
