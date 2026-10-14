#pragma once

#include "ai_engine.hpp"

#include <algorithm>

namespace eawr::script::foc::ai::learning {
// WGA-214/215: registration is immediate; zero duration has no recent row.
inline void append(History& history, bool result, Real duration, std::int64_t frame, std::uint32_t logical_fps) {
    if (duration == Real{}) return;
    const auto expiry = duration < Real{} ? -1 : frame + truncate(to_single(duration * real(logical_fps)));
    history.entries.emplace_back(result, expiry);
}
inline void activation(PlayerAi& player, const Goal& goal, bool result, std::int64_t frame, std::uint32_t logical_fps) {
    const auto& function = player.functions.at(goal.function);
    if (function.goal->activation_tracking_duration == Real{}) return;
    append(player.activations[{function.goal_name, goal.target}], result,
        function.goal->activation_tracking_duration, frame, logical_fps);
}
inline void increment(std::pair<std::int64_t, std::int64_t>& counts, bool result) {
    counts.first += result ? 1 : 0;
    ++counts.second;
}
// WGA-212/213: called once while the goal/plan association is still retained.
inline void outcome(PlayerAi& player, const Goal& goal, const Plan& plan, std::int64_t frame, std::uint32_t logical_fps) {
    const auto& function = player.functions.at(goal.function);
    increment(player.goal_success[function.goal_name], plan.result);
    increment(player.target_goal_success[{function.goal_name, goal.target}], plan.result);
    increment(player.plan_success[plan.definition], plan.result);
    increment(player.target_plan_success[{plan.definition, goal.target}], plan.result);
    if (function.goal->tracking_duration == Real{}) return;
    append(player.goal_outcomes[{function.goal_name, goal.target}], plan.result, function.goal->tracking_duration, frame, logical_fps);
    append(player.plan_outcomes[{plan.definition, goal.target}], plan.result, function.goal->tracking_duration, frame, logical_fps);
}
template<class Key> inline void prune(std::map<Key, History>& histories, std::int64_t frame) {
    for (auto& [key, history] : histories) {
        static_cast<void>(key);
        auto& entries = history.entries;
        const auto first = std::find_if(entries.begin(), entries.end(), [frame](const auto& row) {
            return row.second > 0 && row.second > frame;
        });
        entries.erase(entries.begin(), first);
    }
}
// WGA-216: preserve the entire suffix, including nonpositive expiry rows.
inline void sweep(PlayerAi& player, std::int64_t frame) {
    prune(player.plan_outcomes, frame);
    prune(player.goal_outcomes, frame);
    prune(player.activations, frame);
}
// WGA-217: ownership changes are not target deletion. Global rows survive.
inline void erase_target(PlayerAi& player, std::uint64_t target) {
    if (!target) return;
    const auto erase = [target](auto& table) { std::erase_if(table, [target](const auto& row) { return row.first.second == target; }); };
    erase(player.target_goal_success); erase(player.target_plan_success);
    erase(player.goal_outcomes); erase(player.plan_outcomes); erase(player.activations);
}
// WGA-221: only a qualified producer supplies a survival observation.
inline Real survival(const PlayerAi& player, std::string_view type) {
    const auto found = player.unit_survival.find(type);
    return found == player.unit_survival.end() || !found->second.second ? real(1)
        : to_single(real(found->second.first) / real(found->second.second));
}
} // namespace eawr::script::foc::ai::learning
