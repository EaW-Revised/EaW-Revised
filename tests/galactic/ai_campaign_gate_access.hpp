#pragma once

#include "../../src/campaign/galactic_ai_runtime.hpp"

#include <algorithm>
#include <tuple>

// Read-only installed-gate evidence. No extra draw, service or outcome writer.
namespace galactic_learning_gate {
namespace ai = eawr::script::foc::ai;
namespace ga = eawr::script::foc::galactic_ai;
using Recent = std::vector<std::pair<bool, std::int64_t>>;
struct Player {
    decltype(ai::PlayerAi::plan_success) plans;
    decltype(ai::PlayerAi::goal_success) goals, survival;
    decltype(ai::PlayerAi::target_goal_success) targeted_goals;
    decltype(ai::PlayerAi::target_plan_success) targeted_plans;
    std::map<std::pair<std::string, std::uint64_t>, Recent> activations, goal_outcomes;
    std::map<std::pair<std::size_t, std::uint64_t>, Recent> plan_outcomes;
    bool operator==(const Player&) const = default;
};
struct Choice {
    std::uint64_t goal{}, target{};
    std::size_t definition{};
    std::string goal_type;
    // WGA-219: actual enabled global lifetime reader inputs, in definition order.
    std::vector<std::pair<std::size_t, ai::Real>> weights;
    bool learned{};
    bool operator==(const Choice&) const = default;
};
struct Snapshot {
    std::int64_t frame{};
    std::uint32_t random{};
    std::uint64_t next_plan{};
    ga::LifecycleState lifecycle;
    std::vector<ga::ServiceTrace> services;
    std::map<eawr::sim::tactical::PlayerId, Player> players;
    std::map<std::pair<eawr::sim::tactical::PlayerId, std::uint64_t>, Choice> choices;
    std::vector<std::tuple<std::uint64_t, eawr::sim::tactical::PlayerId, std::string, std::string>> completions;
    bool operator==(const Snapshot&) const = default;
};
}

namespace eawr::script::foc::ai {
// This definition belongs only to the campaign gate executable. Other contract
// executables have their own access fixture and must not include this header.
struct EngineContractAccess {
    static std::vector<std::uint64_t> paid_reservations(const Engine& engine, tactical::PlayerId id) {
        std::vector<std::uint64_t> result;
        const auto* player = engine.player_ai(id);
        if (!player) return result;
        for (const auto* goals : {&player->active, &player->proposed}) for (const auto& goal : *goals) {
            const auto& potential = goal.potential;
            if (!goal.finished && Real{} < goal.desire && potential && potential->reserved
                && Real{} < potential->cost && !engine.plans_.at(potential->plan).magic)
                result.push_back(goal.id);
        }
        return result;
    }
    static bool unstaged_paid_task(const Engine& engine, tactical::PlayerId id,
        std::uint64_t goal_id, sim::EntityId build) {
        const auto* player = engine.player_ai(id);
        if (!player) return false;
        const Goal* retained{};
        for (const auto* goals : {&player->active, &player->proposed}) for (const auto& goal : *goals)
            if (goal.id == goal_id) retained = &goal;
        if (!retained || !retained->potential || !retained->potential->target_contrast_disabled) return false;
        return std::any_of(player->tasks.begin(), player->tasks.end(), [build](const auto& task) {
            return task.source == 2 && task.issued == build && !task.staging && !task.failed;
        });
    }
    static galactic_learning_gate::Snapshot learning_snapshot(const Engine& engine) {
        using namespace galactic_learning_gate;
        Snapshot result; result.frame = engine.frame_; result.random = engine.sync_.seed(); result.next_plan = engine.next_plan_id_;
        for (const auto& player : engine.players_) {
            if (!player.galactic) continue;
            Player tables; tables.plans = player.plan_success; tables.goals = player.goal_success;
            tables.survival = player.unit_survival; tables.targeted_goals = player.target_goal_success;
            tables.targeted_plans = player.target_plan_success;
            for (const auto& [key, rows] : player.activations) tables.activations.emplace(key, rows.entries);
            for (const auto& [key, rows] : player.goal_outcomes) tables.goal_outcomes.emplace(key, rows.entries);
            for (const auto& [key, rows] : player.plan_outcomes) tables.plan_outcomes.emplace(key, rows.entries);
            result.players.emplace(player.player, std::move(tables));
            for (const auto* goals : {&player.proposed, &player.active}) for (const auto& goal : *goals) {
                if (!goal.potential || !goal.potential->id || goal.function >= player.functions.size()) continue;
                Choice choice; choice.goal = goal.id; choice.target = goal.target;
                choice.definition = goal.potential->plan; choice.goal_type = player.functions[goal.function].goal_name;
                for (std::size_t index = 0; index < engine.plans_.size(); ++index) {
                    const auto& plan = engine.plans_[index];
                    const auto enabled = player.galactic_policy.plan_names.find(upper_case(plan.name));
                    if (enabled == player.galactic_policy.plan_names.end() || !enabled->second.value
                        || std::find(plan.goals.begin(), plan.goals.end(), choice.goal_type) == plan.goals.end()) continue;
                    Real rate = real(1);
                    if (const auto found = player.plan_success.find(index); found != player.plan_success.end() && found->second.second > 0) {
                        rate = to_single(real(found->second.first) / real(found->second.second)); choice.learned = true;
                    }
                    choice.weights.emplace_back(index, to_single(rate + real(1)));
                }
                result.choices.emplace(std::pair{player.player, goal.potential->id}, std::move(choice));
            }
        }
        for (const auto& row : engine.records_) if (row.event == "finished" || row.event == "abandoned")
            result.completions.emplace_back(row.tick, row.player, row.plan, row.event);
        return result;
    }
};
}

