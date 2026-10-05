// Plan definitions of the FoC space plans (#449, docs/behaviour/foc-tactical-ai.md "Plans and
// TaskForces" PL-10 to PL-12): what a plan script's definition load leaves in its globals,
// turned into the goal names, TaskForce definitions and flags the goal system reads.

#include "ai_engine.hpp"

#include <algorithm>

namespace eawr::script::foc::ai {
namespace {

using authoritative::Value;

std::optional<bool> flag_of(const Value& value) {
    const auto* found = std::get_if<bool>(&value.data);
    if (found == nullptr) return std::nullopt;
    return *found;
}

std::optional<Real> number_value(const Value& value) {
    const auto* found = std::get_if<numeric::LuaNumber>(&value.data);
    if (found == nullptr) return std::nullopt;
    return *found;
}

std::optional<std::int64_t> whole(std::string_view text) {
    const auto value = parse_real(trimmed(text));
    if (!value) return std::nullopt;
    return truncate(*value);
}

// PL-11: "X = a, b" (minimum a, maximum b; b 0 means a), "X = a" and "X = p%"; X is a keyword,
// a '|' list of categories (every type of those categories that is not a squadron member or a
// star base) or a '|' list of type names.
core::Result<void> parse_entry(const Host& host, std::string_view text, TaskForceDef& taskforce, std::vector<std::string>& notes) {
    const std::size_t equals = text.find('=');
    const std::string left = trimmed(text.substr(0, equals));
    std::int64_t minimum = 0;
    std::int64_t maximum = 0;
    std::int64_t percentage = 0;
    bool percentage_based = false;
    if (equals != std::string_view::npos) {
        const std::string right = trimmed(text.substr(equals + 1));
        if (!right.empty() && right.back() == '%') {
            percentage = whole(right.substr(0, right.size() - 1)).value_or(0);
            percentage_based = true;
        } else {
            const std::size_t comma = right.find(',');
            minimum = whole(right.substr(0, comma)).value_or(0);
            maximum = comma == std::string::npos ? minimum : whole(right.substr(comma + 1)).value_or(0);
        }
    }
    const std::string key = upper_case(left);
    if (key == "ESCORTFORCE") {
        taskforce.escort = true;
        return core::Result<void>::success();
    }
    if (key == "TASKFORCEREQUIRED") {
        taskforce.required = true;
        return core::Result<void>::success();
    }
    if (key == "MINIMUMTOTALSIZE") {
        taskforce.minimum_size = static_cast<std::int32_t>(minimum);
        return core::Result<void>::success();
    }
    if (key == "MINIMUMTOTALFORCE") {
        taskforce.minimum_force = to_single(real(minimum));
        return core::Result<void>::success();
    }
    if (key == "DENYHEROATTACH" || key == "DENYSPECIALWEAPONATTACH" || key == "REQUIREUNIQUESTAGE" ||
        key == "REQUIREMATCHINGSTAGE" || (!key.empty() && key.front() == '-')) {
        return core::Result<void>::success();
    }
    TeamDef team;
    team.min_count = static_cast<std::int32_t>(minimum);
    team.max_count = static_cast<std::int32_t>(maximum == 0 ? minimum : maximum);
    team.percentage = static_cast<std::int32_t>(percentage);
    team.percentage_based = percentage_based;
    const std::vector<std::string> names = split_names(left, "|");
    std::uint64_t bits = 0;
    bool categories = !names.empty();
    for (const std::string& name : names) {
        const auto found = host.setup.content.categories.find(upper_case(name));
        if (found == host.setup.content.categories.end()) {
            categories = false;
            break;
        }
        bits |= found->second;
    }
    bool unloaded = false;
    if (categories && bits != 0) {
        for (const AiType& type : host.setup.content.types) {
            if (type.craft || type.star_base || (type.category_bits & bits) == 0) continue;
            team.types.push_back(type.type_id);
        }
    } else {
        for (const std::string& name : names) {
            const auto found = host.types_by_name.find(upper_case(name));
            if (found == host.types_by_name.end()) {
                notes.push_back("unknown type " + name + " in " + taskforce.name);
                unloaded = true;
                continue;
            }
            if (found->second->craft) continue;
            team.types.push_back(found->second->type_id);
        }
        std::sort(team.types.begin(), team.types.end());
    }
    // A team with no possible type is dropped. The host loads only the M2 types, but every category
    // and type name the FoC space plans use has FoC types (PL-11), so a team naming only types
    // outside the M2 set stays, and no unit can fill it.
    if (!team.types.empty() || (categories && bits != 0) || unloaded) taskforce.teams.push_back(std::move(team));
    return core::Result<void>::success();
}

} // namespace

core::Result<PlanDef> build_plan(const Host& host, std::string name, std::string module, const authoritative::ValueList& globals,
    std::vector<std::string>& notes) {
    using PlanResult = core::Result<PlanDef>;
    const auto invalid = [&](std::string message) {
        core::Diagnostic diagnostic;
        diagnostic.code = "EAWR-AI-0103";
        diagnostic.message = name + ": " + std::move(message);
        return PlanResult::failure(std::move(diagnostic));
    };
    if (globals.size() < 10) return invalid("incomplete definition");
    PlanDef plan;
    plan.name = std::move(name);
    plan.module = std::move(module);
    // PL-10: Category names the goal types, '|' separated.
    if (const auto* category = std::get_if<std::string>(&globals[0].data)) {
        for (const std::string& goal : split_names(*category, "|")) plan.goals.push_back(upper_case(goal));
    }
    if (plan.goals.empty()) return invalid("no goal category");
    const auto* taskforces = std::get_if<std::vector<Value>>(&globals[1].data);
    if (taskforces == nullptr || taskforces->empty()) return invalid("no TaskForce definition");
    for (const Value& entry : *taskforces) {
        const auto* table = std::get_if<std::vector<Value>>(&entry.data);
        if (table == nullptr || table->empty()) continue;
        const auto* tf_name = std::get_if<std::string>(&table->front().data);
        if (tf_name == nullptr) continue;
        TaskForceDef taskforce;
        taskforce.name = *tf_name;
        for (std::size_t index = 1; index < table->size(); ++index) {
            if (const auto* text = std::get_if<std::string>(&(*table)[index].data)) {
                if (auto parsed = parse_entry(host, *text, taskforce, notes); !parsed) return PlanResult::failure(parsed.error());
            } else {
                notes.push_back(taskforce.name + ": a table entry is not hosted");
            }
        }
        plan.taskforces.push_back(std::move(taskforce));
    }
    if (plan.taskforces.empty()) return invalid("no TaskForce definition");
    plan.ignore_target = flag_of(globals[2]).value_or(false);
    plan.magic = flag_of(globals[3]).value_or(false);
    plan.allow_free_store = flag_of(globals[4]).value_or(true);
    plan.allow_engaged = flag_of(globals[5]).value_or(true);
    plan.per_failure_contrast_adjust = to_single(number_value(globals[6]).value_or(Real{}));
    plan.min_contrast = to_single(number_value(globals[7]).value_or(Real{}));
    plan.max_contrast = to_single(number_value(globals[8]).value_or(Real{}));
    if (const auto* required = std::get_if<std::vector<Value>>(&globals[9].data)) {
        for (const Value& value : *required) {
            const auto* text = std::get_if<std::string>(&value.data);
            if (text == nullptr) continue;
            // PL-14: each string is one union requirement, not one category name.
            std::uint64_t bits = 0;
            bool recognized = true;
            for (const auto& category : split_names(*text, "| ,\t\n")) {
                const auto found = host.setup.content.categories.find(upper_case(category));
                if (found == host.setup.content.categories.end()) {
                    recognized = false;
                    break;
                }
                bits |= found->second;
            }
            if (recognized) plan.required_categories.push_back(bits);
            else notes.push_back("unknown required category " + *text + " in " + plan.name);
        }
    }
    return PlanResult::success(std::move(plan));
}

} // namespace eawr::script::foc::ai
