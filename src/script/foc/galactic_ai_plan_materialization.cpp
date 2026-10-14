#include "galactic_ai_plans.hpp"

#include <algorithm>
#include <type_traits>

namespace eawr::script::foc::ai::galactic_goals {
namespace {
using authoritative::Value;
template<class T> Value optional_value(const std::optional<T>& value) {
    if (!value) return {};
    if constexpr (std::is_same_v<T, bool>) return Value{*value};
    else return Value::number(*value);
}
core::Diagnostic invalid(std::string message) {
    return {"galactic-ai.plan-materialization", core::Severity::error, std::move(message), {}, {}, {}, {}};
}
}
core::Result<MaterializedPlanCatalog> materialize_plan_catalog(const detail::Host& host, const PlanCatalog& catalog) {
    using Result = core::Result<MaterializedPlanCatalog>;
    MaterializedPlanCatalog out;
    for (const auto& source : catalog.plans) {
        std::string goals;
        for (const auto& goal : source.goals) { if (!goals.empty()) goals += '|'; goals += goal; }
        std::vector<Value> teams;
        GalacticPlanConstraints constraints;
        constraints.minimum_cost = source.minimum_cost; constraints.additional_cost = source.additional_cost;
        constraints.magic_stealing = source.magic_stealing; constraints.pool_count = source.pool_count;
        for (const auto& force : source.teams) {
            std::vector<Value> declarations{Value::text(force.name)};
            GalacticForceConstraints options;
            for (const auto& declaration : force.declarations) {
                const auto text = trimmed(declaration); const auto name = upper_case(trimmed(text.substr(0, text.find('='))));
                if (name == "DENYHEROATTACH") options.deny_heroes = true;
                else if (name == "DENYSPECIALWEAPONATTACH") options.deny_special_weapons = true;
                else if (name == "REQUIREUNIQUESTAGE") options.unique_stage = true;
                else if (name == "REQUIREMATCHINGSTAGE") options.matching_stage = true;
                else if (!name.empty() && name.front() == '-') {
                    for (const auto& excluded : split_names(name.substr(1), "|")) {
                        const auto type = host.types_by_name.find(upper_case(trimmed(excluded)));
                        if (type == host.types_by_name.end()) return Result::failure(invalid(source.name + ": unresolved excluded original type " + excluded));
                        options.excluded_types.push_back(type->second->type_id);
                    }
                }
                declarations.push_back(Value::text(declaration));
            }
            teams.push_back(Value{std::move(declarations)}); constraints.forces.push_back(std::move(options));
        }
        std::vector<Value> required;
        for (const auto& category : source.required_categories) required.push_back(Value::text(category));
        authoritative::ValueList values{Value::text(goals), Value{std::move(teams)}, optional_value(source.ignore_target),
            optional_value(source.magic), optional_value(source.allow_freestore), optional_value(source.allow_engaged),
            optional_value(source.failure_adjust), optional_value(source.minimum_contrast), optional_value(source.maximum_contrast),
            Value{std::move(required)}, optional_value(source.allow_special_weapons)};
        std::vector<std::string> notes;
        auto plan = build_plan(host, source.name, source.module, values, notes);
        if (!plan) return Result::failure(plan.error());
        if (!notes.empty()) return Result::failure(invalid(source.name + ": " + notes.front()));
        for (std::size_t index = 0; index < plan.value().taskforces.size(); ++index) {
            const auto& options = constraints.forces[index];
            for (auto& team : plan.value().taskforces[index].teams)
                std::erase_if(team.types, [&](auto type) { return std::find(options.excluded_types.begin(), options.excluded_types.end(), type) != options.excluded_types.end(); });
        }
        out.constraints.emplace(source.name, std::move(constraints)); out.plans.push_back(std::move(plan).value());
    }
    return Result::success(std::move(out));
}
} // namespace eawr::script::foc::ai::galactic_goals
