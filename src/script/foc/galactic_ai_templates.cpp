#include "galactic_ai_internal.hpp"

#include <algorithm>

namespace eawr::script::foc::galactic_ai {
namespace {
template<class T> void setting(std::map<std::string, Setting<T>, std::less<>>& settings, const std::string& name, T value, int priority) {
    const auto found = settings.find(name);
    if (found == settings.end() || found->second.priority <= priority) settings[name] = {std::move(value), priority};
}
void switches(std::map<std::string, Setting<bool>, std::less<>>& settings,
    const std::set<std::string>& off, const std::set<std::string>& on, int priority) {
    for (const auto& name : off) setting(settings, name, false, priority);
    for (const auto& name : on) setting(settings, name, true, priority);
}
bool matches_goal(const ai::GoalType& goal, const std::set<std::string>& categories, const std::set<std::string>& types) {
    return categories.contains(goal.category) || types.contains(ai::upper_case(goal.name));
}
bool matches_plan(const ai::AiData& data, const PlanInput& plan, const std::set<std::string>& categories,
    const std::set<std::string>& types, const std::set<std::string>& names) {
    if (names.contains(plan.name)) return true;
    for (const auto& name : plan.goals) {
        const auto found = data.goals.find(name);
        if (found != data.goals.end() && matches_goal(found->second, categories, types)) return true;
    }
    return false;
}
} // namespace

TemplatePolicy apply_template(const ai::AiData& data, std::span<const PlanInput> plans, TemplatePolicy policy, const ai::Template& entry) {
    for (const auto& [category, equation] : entry.budget) setting(policy.equations, category, equation, entry.priority);
    for (const auto& [category, allowance] : entry.tactical_allowances) setting(policy.allowances, category, allowance, entry.priority);
    switches(policy.goal_categories, entry.goals_off, entry.goals_on, entry.priority);
    switches(policy.plan_categories, entry.plans_off, entry.plans_on, entry.priority);
    switches(policy.plan_types, entry.plan_types_off, entry.plan_types_on, entry.priority);
    // WGA-102..110: selectors update existing entries, sharing each entry's
    // priority across category/type/name selectors. Off precedes on.
    const auto goals = [&](const std::set<std::string>& categories, const std::set<std::string>& types, bool enabled) {
        for (const auto& [name, goal] : data.goals) if (goal.mode == "GALACTIC" && matches_goal(goal, categories, types)) setting(policy.goal_types, name, enabled, entry.priority);
    };
    goals(entry.goals_off, {}, false); goals(entry.goals_on, {}, true);
    goals({}, entry.goal_types_off, false); goals({}, entry.goal_types_on, true);
    const auto plan_switches = [&](const std::set<std::string>& categories, const std::set<std::string>& types, const std::set<std::string>& names, bool enabled) {
        for (const auto& plan : plans) if (matches_plan(data, plan, categories, types, names)) setting(policy.plan_names, plan.name, enabled, entry.priority);
    };
    plan_switches(entry.plans_off, {}, {}, false); plan_switches(entry.plans_on, {}, {}, true);
    plan_switches({}, entry.plan_types_off, {}, false); plan_switches({}, entry.plan_types_on, {}, true);
    plan_switches({}, {}, entry.plan_names_off, false); plan_switches({}, {}, entry.plan_names_on, true);
    return policy;
}

TemplatePolicy reset_template(const ai::AiData& data, std::span<const PlanInput> plans, TemplatePolicy policy, const ai::Template& entry) {
    for (auto& [category, equation] : policy.equations) { static_cast<void>(category); equation = {"", 1}; }
    for (const auto& [name, goal] : data.goals) {
        if (goal.mode == "GALACTIC" && (matches_goal(goal, entry.goals_off, entry.goal_types_off) || matches_goal(goal, entry.goals_on, entry.goal_types_on))) policy.goal_types[name] = {false, 1};
    }
    for (const auto& plan : plans) if (matches_plan(data, plan, entry.plans_off, entry.plan_types_off, entry.plan_names_off) || matches_plan(data, plan, entry.plans_on, entry.plan_types_on, entry.plan_names_on)) policy.plan_names[plan.name] = {false, 1};
    // Allowance values/priorities and untouched switches survive (WGA-105).
    return policy;
}

core::Result<bool> Manager::Impl::positive(const PlayerSelection& player, std::string_view equation) const {
    if (equation.empty()) return core::Result<bool>::success(false);
    if (!capabilities.queries) return core::Result<bool>::failure(lifecycle_error("template equation query capability is unavailable"));
    const QueryContext context{player.player, player.enemy, player.human, {}, {}, Mode::galactic};
    auto value = capabilities.queries->equation(equation, context);
    // WGA-418/420: failure preserves this consumer's initial zero.
    // Lua and proposal consumers retain their separate failure behavior.
    return core::Result<bool>::success(value && value.value() > Number(0));
}

core::Result<void> Manager::Impl::initialize(PlayerLifecycle& player) {
    const auto& type = data.players.at(player.selected.player_type);
    for (const auto& [name, goal] : data.goals) if (goal.mode == "GALACTIC") player.policy.goal_types[name] = {false, 1};
    for (const auto& plan : plans) player.policy.plan_names[plan.name] = {false, 1};
    if (!type.galactic_templates.empty()) {
        player.active_templates[0] = true; // WGA-09: default is unconditional.
        player.activation_order.push_back(0);
        player.policy = apply_template(data, plans, std::move(player.policy), data.templates.at(type.galactic_templates.front()));
    }
    for (std::size_t index = 1; index < type.galactic_templates.size(); ++index) {
        const auto& entry = data.templates.at(type.galactic_templates[index]);
        auto triggered = positive(player.selected, entry.trigger);
        if (!triggered) return core::Result<void>::failure(triggered.error());
        if (triggered.value()) {
            player.active_templates[index] = true;
            player.activation_order.push_back(index);
            player.policy = apply_template(data, plans, std::move(player.policy), entry);
        }
    }
    if (auto applied = capabilities.services->apply_policy(player.selected.player, player.policy, PolicyWrite::initial); !applied) return applied;
    if (auto initialized = capabilities.services->initialize(player.selected.player); !initialized) return initialized;
    player.initialized = true;
    return core::Result<void>::success();
}

core::Result<void> Manager::Impl::templates(PlayerLifecycle& player) {
    const auto& names = data.players.at(player.selected.player_type).galactic_templates;
    if (names.empty()) return core::Result<void>::success();
    auto active = player.active_templates;
    auto order = player.activation_order;
    auto policy = player.policy;
    const auto count = std::min(names.size(), std::max<std::size_t>(names.size() / (gc::galactic_frames_per_second * 10U), 1));
    auto cursor = player.template_cursor;
    for (std::size_t visit = 0; visit < count; ++visit) {
        const auto index = cursor;
        cursor = (cursor + 1) % names.size();
        if (active[index]) continue;
        auto triggered = positive(player.selected, data.templates.at(names[index]).trigger);
        if (!triggered) return core::Result<void>::failure(triggered.error());
        if (triggered.value()) {
            active[index] = true;
            order.push_back(index);
            const auto& entry = data.templates.at(names[index]);
            policy = apply_template(data, plans, std::move(policy), entry);
            if (!entry.budget.empty() || !entry.tactical_allowances.empty()) {
                TemplatePolicy writes;
                for (const auto& [category, equation] : entry.budget) writes.equations[category] = {equation, entry.priority};
                for (const auto& [category, allowance] : entry.tactical_allowances) writes.allowances[category] = {allowance, entry.priority};
                if (auto applied = capabilities.services->apply_funding_writes(player.selected.player, writes); !applied) return applied;
            }
        }
    }
    bool removed{};
    for (const auto index : order) {
        if (index == 0) continue;
        const auto& entry = data.templates.at(names[index]);
        auto untriggered = positive(player.selected, entry.untrigger.empty() ? entry.trigger : entry.untrigger);
        if (!untriggered) return core::Result<void>::failure(untriggered.error());
        // WGA-420: without an authored untrigger, invert the trigger test.
        if (entry.untrigger.empty() ? !untriggered.value() : untriggered.value()) {
            active[index] = false;
            removed = true;
            policy = reset_template(data, plans, std::move(policy), data.templates.at(names[index]));
        }
    }
    if (removed) {
        std::erase_if(order, [&](std::size_t index) { return !active[index]; });
        for (const auto index : order) policy = apply_template(data, plans, std::move(policy), data.templates.at(names[index]));
        if (auto cleared = capabilities.services->clear_equations(player.selected.player); !cleared) return cleared;
    }
    if (removed || policy != player.policy) if (auto applied = capabilities.services->apply_policy(player.selected.player, policy,
        removed ? PolicyWrite::reversal : PolicyWrite::ordinary); !applied) return applied;
    player.active_templates = std::move(active);
    player.activation_order = std::move(order);
    player.policy = std::move(policy);
    player.template_cursor = cursor;
    return core::Result<void>::success();
}
} // namespace eawr::script::foc::galactic_ai
