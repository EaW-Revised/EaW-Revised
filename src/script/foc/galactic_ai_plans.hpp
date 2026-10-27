#pragma once

#include "ai_data.hpp"
#include "ai_engine.hpp"
#include "galactic_ai_goals.hpp"
#include "eawr/script/foc/galactic_ai.hpp"
#include "eawr/script/foc/tactical_ai.hpp"

#include <functional>
#include <span>

namespace eawr::script::foc::ai::galactic_goals {

struct PlanTeamData {
    std::string name;
    // WGA-25/38: preserve ranges, percentages, exclusions and attachment
    // flags verbatim as declaration data until the live selector resolves them.
    std::vector<std::string> declarations;
    bool operator==(const PlanTeamData&) const = default;
};
struct PlanDefinitionData {
    std::string name, module;
    std::vector<std::string> goals;
    std::vector<PlanTeamData> teams;
    std::optional<bool> ignore_target, magic, allow_freestore, allow_engaged, magic_stealing;
    std::optional<Real> failure_adjust, minimum_contrast, maximum_contrast, minimum_cost, additional_cost, pool_count;
    std::vector<std::string> required_categories;
    std::optional<bool> allow_special_weapons;
    // WGA-445: each definition owns its authored enemy/friendly lists.
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, Real>>>> contrast;
    bool operator==(const PlanDefinitionData&) const = default;
};
struct PlanCatalog {
    std::vector<PlanDefinitionData> plans;
    std::map<std::string, Real, std::less<>> minimum_pause_seconds;
    // Plans whose own pause library differs from the first one read: each plan sleeps by its own library at run
    // time, so the difference is reported, not refused.
    std::vector<std::string> pause_differences;
    // Plan files that do not load (script error, no task force, no goal type). The debug build logs such a file and
    // goes on without that plan; so does the catalog, and lists it here.
    std::vector<std::string> skipped_plans;
    // Items the plan reader dropped while keeping their plan (wrong-typed override, bad task force entry, repeated plan name)
    // and the empty-catalog notice: one line each, reported at load (RGC-162, RGC-164).
    std::vector<std::string> item_diagnostics;
    // Plans from the other four directories that no admitted goal type can use (see PlanAdmission); not registered.
    std::size_t unused_plans{};
    bool operator==(const PlanCatalog&) const = default;
};

// RGC-163: the index of the debug build's plan directory (of five, in its order) holding this `*.lua` path, if any.
[[nodiscard]] std::optional<std::size_t> plan_directory_index(std::string_view path);

// Production definition-load path: stock Lua executes in fresh authoritative
// instances through the existing FoC bindings. No main/taskforce thread runs.
// `admits` (galactic runtime): the first directory is the base registry and keeps every plan that loads; a plan from one of the
// other four directories is registered only when `admits` accepts it. The debug build's single registry serves every mode and
// the plan's goal types decide who can use it, so a plan no galactic goal names cannot reach the galactic AI either way; leaving
// it out avoids registering unrelated-mode plans. The caller retains the frozen module inventory for require() helpers.
using PlanAdmission = std::function<bool(const PlanDefinitionData&)>;
[[nodiscard]] core::Result<PlanCatalog> load_plan_catalog(const AiSetup&,
    const std::map<std::string, std::string>& modules, const PlanAdmission& admits = {});

// Allocation-only registration view of the loaded stock definitions. Raw team
// declarations still need the composed selector; no selected forces or valid
// potential plan is implied by this catalog projection.
[[nodiscard]] std::vector<PlanDef> goal_plan_registrations(const PlanCatalog&);

struct GalacticForceConstraints {
    bool deny_heroes{}, deny_special_weapons{}, unique_stage{}, matching_stage{};
    std::vector<sim::galactic::TypeId> excluded_types;
};
struct GalacticPlanConstraints {
    std::optional<Real> minimum_cost, additional_cost, pool_count;
    std::optional<bool> magic_stealing;
    std::vector<GalacticForceConstraints> forces;
};
struct MaterializedPlanCatalog {
    std::vector<PlanDef> plans;
    std::map<std::string, GalacticPlanConstraints, std::less<>> constraints;
    std::vector<std::string> diagnostics; // exclusions and inclusions naming types no file defines (RGC-61/62), reported at load
};
// PL-11/WGA-25/60: resolve authored declarations against original campaign
// identities. Attachment/stage options remain typed facts for their consumers.
[[nodiscard]] core::Result<MaterializedPlanCatalog> materialize_plan_catalog(
    const detail::Host&, const PlanCatalog&);

// WGA-448: retained native category-map iteration, independent of the host STL.
[[nodiscard]] std::vector<std::uint64_t> galactic_contrast_category_order(std::span<const std::uint64_t>);

// WGA-16/19/25: categories name goal types; A materializes plan-name policy
// from template category/type/name switches. Missing policy never enables a plan.
[[nodiscard]] bool activation_allowed(const PlanDefinitionData&, const GoalType&,
    const foc::galactic_ai::TemplatePolicy&, const ProposalCounts&);

} // namespace eawr::script::foc::ai::galactic_goals
