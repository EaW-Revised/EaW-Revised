#include "galactic_ai_plans.hpp"
#include "tactical_ai_internal.hpp"

#include <algorithm>
#include <array>
#include <set>

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
// A plan file the debug build does not take up: its script does not load, or it names no task force or no goal type.
// The definition manager logs it and goes on with the next file, so the catalog skips it and lists it.
constexpr std::string_view unloadable_code = "galactic-ai.plan-unloadable";
core::Diagnostic unloadable(std::string message) {
    auto result = error(std::move(message));
    result.code = std::string(unloadable_code);
    return result;
}
// RGC-163: the debug build reads `*.lua` from these five directories, in this order, none of them recursively. The
// directory does not decide which mode a plan serves; its Category goal types do.
constexpr std::array<std::string_view, 5> plan_directories{"DATA/SCRIPTS/AI/", "DATA/SCRIPTS/AI/SPACEMODE/", "DATA/SCRIPTS/AI/LANDMODE/",
    "DATA/SCRIPTS/AI/STORYARC/", "DATA/SCRIPTS/INTERVENTIONS/"};
}
std::optional<std::size_t> plan_directory_index(std::string_view path) {
    const auto canonical = upper_case(path);
    if (!canonical.ends_with(".LUA")) return {};
    for (std::size_t index = 0; index < plan_directories.size(); ++index)
        if (canonical.starts_with(plan_directories[index]) && canonical.substr(plan_directories[index].size()).find('/') == std::string::npos)
            return index;
    return {};
}
namespace {
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
template<class T> bool index_is(const auth::ValueList& values, std::size_t index) {
    return index < values.size() && std::holds_alternative<T>(values[index].data);
}
// RGC-162: the plan reader skips what it cannot use and keeps the plan. A file is refused only when no task force, no goal
// type or no usable task force is left (gates 1, 4, 6); every other malformed item is dropped and noted once in `notes`.
core::Result<PlanDefinitionData> definition(std::string module, const auth::ValueList& values, std::vector<std::string>& notes) {
    using Result = core::Result<PlanDefinitionData>;
    const auto categories = field<std::string>(values, 0);
    const auto teams = field<std::vector<auth::Value>>(values, 1);
    if (!categories || !teams || teams->empty()) return Result::failure(unloadable(module + ": missing Category/TaskForce"));
    PlanDefinitionData out;
    out.name = stem(module); out.module = std::move(module);
    const auto note = [&](const std::string& message) { notes.push_back(out.module + ": " + message); };
    const auto wrong_type = [&](std::size_t index, std::string_view name, bool right) {
        if (index < values.size() && !std::holds_alternative<std::monostate>(values[index].data) && !right)
            note("wrong-typed " + std::string(name) + " override ignored (RGC-162)");
    };
    wrong_type(2, "IgnoreTarget", index_is<bool>(values, 2)); wrong_type(3, "MagicPlan", index_is<bool>(values, 3));
    wrong_type(4, "AllowFreeStoreUnits", index_is<bool>(values, 4)); wrong_type(5, "AllowEngagedUnits", index_is<bool>(values, 5));
    wrong_type(6, "PerFailureContrastAdjust", index_is<Real>(values, 6)); wrong_type(7, "MinContrastScale", index_is<Real>(values, 7));
    wrong_type(8, "MaxContrastScale", index_is<Real>(values, 8)); wrong_type(9, "RequiredCategories", index_is<std::vector<auth::Value>>(values, 9));
    wrong_type(10, "MagicPlanStealing", index_is<bool>(values, 10)); wrong_type(11, "MinimumCost", index_is<Real>(values, 11));
    wrong_type(12, "AdditionalCost", index_is<Real>(values, 12)); wrong_type(13, "ScriptPoolCount", index_is<Real>(values, 13));
    wrong_type(14, "AllowSpecialWeapons", index_is<bool>(values, 14));
    for (const auto& goal : split_names(*categories, "|")) out.goals.push_back(upper_case(goal));
    if (out.goals.empty()) return Result::failure(unloadable(out.module + ": empty Category"));
    std::size_t entry_number{};
    for (const auto& value : *teams) {
        ++entry_number;
        const auto* entries = std::get_if<std::vector<auth::Value>>(&value.data);
        if (!entries || entries->empty()) {
            note("TaskForce entry " + std::to_string(entry_number) + " is not a table; skipped (RGC-162)");
            continue;
        }
        const auto name = field<std::string>(*entries, 0);
        if (!name || name->empty()) {
            note("TaskForce entry " + std::to_string(entry_number) + " has no name; skipped");
            continue;
        }
        PlanTeamData team; team.name = *name;
        for (std::size_t index = 1; index < entries->size(); ++index) {
            const auto declaration = field<std::string>(*entries, index);
            if (!declaration) {
                note("TaskForce " + *name + " declaration " + std::to_string(index) + " is not text; skipped");
                continue;
            }
            team.declarations.push_back(*declaration);
        }
        out.teams.push_back(std::move(team));
    }
    if (out.teams.empty()) return Result::failure(unloadable(out.module + ": no usable TaskForce entry"));
    out.ignore_target = field<bool>(values, 2); out.magic = field<bool>(values, 3);
    out.allow_freestore = field<bool>(values, 4); out.allow_engaged = field<bool>(values, 5);
    out.failure_adjust = field<Real>(values, 6); out.minimum_contrast = field<Real>(values, 7);
    out.maximum_contrast = field<Real>(values, 8);
    if (const auto required = field<std::vector<auth::Value>>(values, 9)) {
        for (const auto& entry : *required) {
            const auto* name = std::get_if<std::string>(&entry.data);
            if (!name) {
                note("a RequiredCategories entry is not text; skipped (RGC-162)");
                continue;
            }
            out.required_categories.push_back(*name);
        }
    }
    out.magic_stealing = field<bool>(values, 10); out.minimum_cost = field<Real>(values, 11);
    out.additional_cost = field<Real>(values, 12); out.pool_count = field<Real>(values, 13);
    out.allow_special_weapons = field<bool>(values, 14);
    return Result::success(std::move(out));
}
}

CatalogResult load_plan_catalog(const AiSetup& setup, const std::map<std::string, std::string>& modules, const PlanAdmission& admits) {
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
        if (plan_directory_index(path)) paths.push_back(path);
    }
    // Directory order first, as the debug build enumerates, then name within each directory.
    std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) {
        const auto left = *plan_directory_index(a), right = *plan_directory_index(b);
        return left != right ? left < right : upper_case(a) < upper_case(b);
    });
    // The definition scripts are chunks named by plan name; a repeated name in a later directory gets a distinct chunk.
    std::map<std::string, unsigned> stem_uses;
    std::vector<std::string> chunks;
    for (const auto& path : paths) {
        const auto uses = stem_uses[stem(path)]++;
        chunks.push_back("EAWR/GALACTIC_PLAN/" + stem(path) + (uses ? "~" + std::to_string(uses) : std::string{}) + ".LUA");
    }
    for (const auto& path : paths) {
        // Host glue only. Run stock definition and pause functions; never copy
        // their bodies or hardcode a retail difficulty table into production.
        const auto loader = "require(" + lua_string(path.substr(0, path.size()-4)) + ")\n"
            "PlanDefinitionLoad = true\nBase_Definitions()\n"
            "_EAWR_GalacticDefinition(Category, TaskForce, IgnoreTarget, MagicPlan, AllowFreeStoreUnits, AllowEngagedUnits,\n"
            " PerFailureContrastAdjust, MinContrastScale, MaxContrastScale, RequiredCategories, MagicPlanStealing,\n"
            " MinimumCost, AdditionalCost, ScriptPoolCount, AllowSpecialWeapons)\n"
            "if EnemyContrastTypes then\n for index, enemy in pairs(EnemyContrastTypes) do\n"
            " _EAWR_GalacticContrast(enemy, FriendlyContrastTypes[index])\n end\nend\n"
            "_EAWR_GalacticPacing(DifficultyBasedMinPause(\"Easy\"), DifficultyBasedMinPause(\"Normal\"), DifficultyBasedMinPause(\"Hard\"))\n";
        if (const auto added = manifest.add(chunks[static_cast<std::size_t>(&path - paths.data())], loader); !added)
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
    tactical_ai_detail::register_globals(scripts, host, errors, false);
    tactical_ai_detail::register_methods(scripts, host, errors, false);
    using WeightedList = std::vector<std::pair<std::string, Real>>;
    using ContrastLists = std::vector<std::pair<std::string, WeightedList>>;
    auto lists = std::make_shared<std::vector<WeightedList>>();
    auto contrast = std::make_shared<std::map<std::uint64_t, ContrastLists>>();
    const auto check = [&](core::Result<void> result) { if (!result) errors.push_back(result.error()); };
    check(scripts.register_binding("WeightedTypeList.Create", [lists](auth::BindingContext&, const auth::ValueList&) {
        lists->emplace_back();
        return detail::one(detail::handle(handle_type_list, lists->size()));
    }));
    check(scripts.register_method(handle_type_list, "Parse", [lists](auth::BindingContext&, const auth::ValueList& values) -> core::Result<auth::ValueList> {
        const auto* handle = values.empty() ? nullptr : detail::as_handle(values[0], handle_type_list);
        const auto names = field<std::vector<auth::Value>>(values, 1);
        const auto weights = field<std::vector<auth::Value>>(values, 2);
        if (!handle || !handle->id || handle->id > lists->size() || !names || !weights || names->size() != weights->size())
            return detail::fail("plan weighted list requires names and weights of equal length");
        WeightedList parsed;
        for (std::size_t index = 0; index < names->size(); ++index) {
            const auto* name = std::get_if<std::string>(&(*names)[index].data);
            const auto* weight = std::get_if<Real>(&(*weights)[index].data);
            if (!name || !weight) return detail::fail("plan weighted list requires text names and numeric weights");
            parsed.emplace_back(upper_case(*name), *weight);
        }
        (*lists)[handle->id - 1] = std::move(parsed);
        return detail::none();
    }));
    check(scripts.register_binding("_EAWR_GalacticContrast", [lists, contrast](auth::BindingContext& context, const auth::ValueList& values) -> core::Result<auth::ValueList> {
        const auto name = field<std::string>(values, 0);
        const auto* handle = values.size() == 2 ? detail::as_handle(values[1], handle_type_list) : nullptr;
        if (!name || !handle || !handle->id || handle->id > lists->size()) return detail::fail("plan contrast requires an enemy category and a weighted list");
        (*contrast)[context.instance()].emplace_back(upper_case(*name), (*lists)[handle->id - 1]);
        return detail::none();
    }));
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
    std::set<std::string> registered;
    for (const auto& path : paths) {
        ++instance;
        const auto skip = [&](const core::Diagnostic& why) { out.skipped_plans.push_back(path + ": " + core::format_diagnostic(why)); };
        if (const auto loaded = scripts.create_instance(instance, chunks[instance - 1]); !loaded) {
            skip(loaded.error());
            continue;
        }
        const auto found = definitions->find(instance);
        if (found == definitions->end()) {
            skip(unloadable("no definition output"));
            continue;
        }
        auto plan = definition(path, found->second, out.item_diagnostics);
        if (!plan) {
            skip(plan.error());
            continue;
        }
        // RGC-162 gate 2: the earlier definition of a name is kept and the later file is refused.
        if (!registered.insert(plan.value().name).second) {
            out.item_diagnostics.push_back(path + ": plan " + plan.value().name + " is already defined by an earlier directory entry; the earlier definition is kept (RGC-162)");
            continue;
        }
        if (admits && *plan_directory_index(path) != 0 && !admits(plan.value())) {
            ++out.unused_plans;
            continue;
        }
        if (const auto rows = contrast->find(instance); rows != contrast->end()) plan.value().contrast = rows->second;
        out.plans.push_back(std::move(plan).value());
        const auto pause = pacing->find(instance);
        if (pause == pacing->end()) return CatalogResult::failure(error(path + ": no pacing output"));
        const std::array<std::string, 3> levels{"Easy", "Normal", "Hard"};
        for (std::size_t index = 0; index < levels.size(); ++index) {
            const auto value = field<Real>(pause->second, index);
            if (!value) return CatalogResult::failure(error(path + ": nonnumeric difficulty pause"));
            const auto [existing, inserted] = out.minimum_pause_seconds.try_emplace(levels[index], *value);
            if (!inserted && existing->second != *value) {
                out.pause_differences.push_back(path + ": " + levels[index] + " pause differs from the first plan's library");
                break;
            }
        }
    }
    // RGC-164: a plan stage with nothing loaded is a logged failure, not a refused startup. The catalog is empty, so no goal
    // finds a plan: the AI runs, proposes no goals and builds, moves and attacks nothing.
    if (out.plans.empty()) out.item_diagnostics.push_back("no galactic plan loads (" + std::to_string(paths.size()) + " file(s) read, "
        + std::to_string(out.skipped_plans.size()) + " skipped); the AI starts with no plans and proposes no goals (RGC-164)"
        + (out.skipped_plans.empty() ? std::string{} : "; first skipped: " + out.skipped_plans.front()));
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
