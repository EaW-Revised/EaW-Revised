#include "ai_engine_internal.hpp"
#include "galactic_ai_goals.hpp"
#include "galactic_ai_actions.hpp"

#include <algorithm>
#include <limits>

namespace eawr::script::foc::ai {
namespace {
template<class T> core::Result<T> refuse(std::string message) {
    return core::Result<T>::failure(detail::make_error(authoritative::codes::invalid_request, std::move(message)));
}
const Goal* goal_in(const PlayerAi& player, std::uint64_t id) {
    for (const auto* goals : {&player.active, &player.proposed})
        for (const auto& goal : *goals) if (goal.id == id) return &goal;
    return nullptr;
}
GalacticGoalIdentity identity(const PlayerAi& player, const Goal& goal, const Engine& engine) {
    GalacticGoalIdentity row;
    row.player = player.player; row.goal = goal.id; row.target = goal.target;
    row.running_plan = goal.plan;
    if (goal.potential) {
        row.potential_plan = goal.potential->id;
        row.definition = engine.plans().at(goal.potential->plan).name;
    }
    if (const auto* plan = engine.plan(goal.plan)) { row.instance = plan->instance; row.definition = engine.plans().at(plan->definition).name; }
    return row;
}
}

core::Result<std::optional<GalacticGoalIdentity>> Engine::allocate_galactic_goal(
    tactical::PlayerId id, std::size_t function_index, std::uint64_t target_id) {
    mark_native_state_changed();
    using Answer = std::optional<GalacticGoalIdentity>;
    auto* player = player_ai(id);
    if (!initialized_ || !player || !player->galactic || function_index >= player->functions.size()
        || !host_->ai_world || host_->ai_world->goal_mode() != "GALACTIC") return refuse<Answer>("goal allocation requires an initialized galactic function");
    const auto& function = player->functions[function_index];
    const bool global = function.goal->application.contains("GLOBAL");
    const auto* selected = target(target_id);
    if ((global && target_id != 0) || (!global && (!selected
        || std::find(player->targets.begin(), player->targets.end(), target_id) == player->targets.end()
        || !applies(*player, *function.goal, selected)))) return refuse<Answer>("goal allocation target does not match its authored application");
    galactic_goals::ProposalCounts counts;
    counts.blockers = player->proposal_blocks;
    for (const auto& [name, count] : player->proposal_counts) counts.ordinary[name] = count;
    for (const auto& [name, count] : player->exclusion_counts) counts.exclusions[name] = count;
    if (!galactic_goals::proposable(counts, function.goal_name)) return core::Result<Answer>::success({});
    Goal goal; goal.function = function_index; goal.target = target_id;
    for (const auto* goals : {&player->active, &player->proposed}) for (const auto& retained : *goals)
        if (!retained.finished && like(*player, retained, goal)) return core::Result<Answer>::success({});
    const auto value = run_equation(*function.equation, context_of(*player, selected));
    if (!value) return core::Result<Answer>::success({}); // unqualified perception remains unavailable
    const auto key = std::pair{function.goal_name, target_id};
    const auto outcomes = player->goal_outcomes.find(key), activations = player->activations.find(key);
    goal.desire = to_single(*value
        + to_single(function.goal->per_failure_desire_adjust * real(recent_failures(outcomes == player->goal_outcomes.end() ? nullptr : &outcomes->second)))
        + to_single(function.goal->per_activation_failure_desire_adjust * real(recent_failures(activations == player->activations.end() ? nullptr : &activations->second))));
    if (!(Real{} < goal.desire)) return core::Result<Answer>::success({});
    std::vector<std::pair<std::size_t, Real>> candidates;
    Real total{};
    for (std::size_t index = 0; index < plans_.size(); ++index) {
        const auto& plan = plans_[index];
        const auto enabled = player->galactic_policy.plan_names.find(upper_case(plan.name));
        if (enabled == player->galactic_policy.plan_names.end() || !enabled->second.value
            || std::find(plan.goals.begin(), plan.goals.end(), function.goal_name) == plan.goals.end()) continue;
        Real rate = real(1);
        if (const auto found = player->plan_success.find(index); found != player->plan_success.end() && found->second.second > 0)
            rate = to_single(real(found->second.first) / real(found->second.second));
        const auto weight = to_single(rate + real(1));
        candidates.emplace_back(index, weight); total = to_single(total + weight);
    }
    if (candidates.empty()) return core::Result<Answer>::success({});
    // Both potential and running plan IDs share this allocator, preventing a
    // reservation from resolving to a different goal after plan attachment.
    if (player->next_goal_id == std::numeric_limits<std::uint64_t>::max()
        || next_plan_id_ == std::numeric_limits<std::uint64_t>::max()) return refuse<Answer>("galactic goal identity space exhausted");
    const auto draw = sync_.uniform(Real{}, total);
    auto chosen = candidates.back().first; Real sum{};
    for (const auto& [index, weight] : candidates) { sum = to_single(sum + weight); if (draw < sum) { chosen = index; break; } }
    goal.id = player->next_goal_id++;
    goal.potential.emplace(); goal.potential->id = next_plan_id_++; goal.potential->plan = chosen;
    const auto answer = identity(*player, goal, *this);
    player->proposed.push_back(std::move(goal));
    return core::Result<Answer>::success(answer);
}

std::optional<GalacticGoalIdentity> Engine::galactic_goal(tactical::PlayerId id, std::uint64_t goal) const {
    const auto* player = player_ai(id);
    if (!player || !player->galactic) return {};
    const auto* found = goal_in(*player, goal);
    return found && !found->finished ? std::optional{identity(*player, *found, *this)} : std::nullopt;
}
std::vector<GalacticGoalIdentity> Engine::galactic_goals(tactical::PlayerId id) const {
    std::vector<GalacticGoalIdentity> result;
    const auto* player = player_ai(id); if (!player || !player->galactic) return result;
    for (const auto* goals : {&player->active, &player->proposed}) for (const auto& goal : *goals)
        if (!goal.finished) result.push_back(identity(*player, goal, *this));
    return result;
}
std::optional<std::uint64_t> Engine::galactic_goal_for_plan(tactical::PlayerId id, std::uint64_t plan) const {
    if (plan == 0) return {};
    const auto* player = player_ai(id); if (!player || !player->galactic) return {};
    for (const auto* goals : {&player->active, &player->proposed}) for (const auto& goal : *goals)
        if (goal.plan == plan || (goal.potential && goal.potential->id == plan)) return goal.id;
    return {};
}
std::optional<std::uint64_t> Engine::galactic_potential_for_goal(tactical::PlayerId id, std::uint64_t goal) const {
    const auto* player = player_ai(id); if (!player || !player->galactic) return {};
    const auto* found = goal_in(*player, goal);
    return found && found->potential ? std::optional{found->potential->id} : std::nullopt;
}
bool Engine::galactic_reservation_valid(tactical::PlayerId id, sim::EntityId object, std::uint64_t potential) const {
    const auto goal = galactic_goal_for_plan(id, potential);
    const auto* player = player_ai(id);
    if (!goal || !player) return false;
    if (galactic_potential_for_goal(id, *goal) != potential) return false;
    const auto found = player->reserved.find(object);
    return found != player->reserved.end() && found->second == *goal;
}
core::Result<std::map<sim::EntityId, std::uint64_t>> Engine::galactic_execution_reservations(tactical::PlayerId id) const {
    using Result = core::Result<std::map<sim::EntityId, std::uint64_t>>;
    const auto* player = player_ai(id);
    if (!player || !player->galactic) return refuse<std::map<sim::EntityId, std::uint64_t>>("execution reservations require a retained galactic player");
    std::map<std::uint64_t, std::uint64_t> plans;
    for (const auto* rows : {&player->active, &player->proposed}) for (const auto& goal : *rows)
        if (goal.potential) plans.emplace(goal.id, goal.potential->id);
    std::map<sim::EntityId, std::uint64_t> result;
    // WGA-56: pending loss/cancel IDs remain owned until the engine releases
    // their goal. Presence is not a reservation lifetime predicate.
    for (const auto& [object, goal] : player->reserved) {
        const auto plan = plans.find(goal);
        if (!object || plan == plans.end()) return refuse<std::map<sim::EntityId, std::uint64_t>>("execution reservation has no retained potential plan");
        result.emplace(object, plan->second);
    }
    return Result::success(std::move(result));
}
core::Result<void> Engine::reserve_galactic_object(tactical::PlayerId id, std::uint64_t goal, sim::EntityId object) {
    mark_native_state_changed();
    auto* player = player_ai(id);
    if (!galactic_goal(id, goal) || !host_->view) return refuse<void>("reservation has no live galactic goal/view");
    const auto* unit = host_->view->find(object);
    if (!unit || unit->owner != id) return refuse<void>("reservation requires a present owned object");
    for (const auto& other : players_) {
        if (other.assigned.contains(object)) return refuse<void>("reservation object already assigned to a taskforce");
        if (const auto found = other.reserved.find(object); found != other.reserved.end()
            && (other.player != id || found->second != goal)) return refuse<void>("reservation object belongs to another goal");
    }
    auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
    const auto identity = galactic_goal(id, goal);
    if (actions && actions->qualified_freestore_available() && !player->reserved.contains(object)) {
        if (!identity->potential_plan) return refuse<void>("freestore reservation requires its actual potential plan");
        auto reserved = actions->qualified_freestore().reserve(id, object, identity->potential_plan);
        if (!reserved) return core::Result<void>::failure(reserved.error());
        if (!reserved.value()) return refuse<void>("freestore actor already has a reservation");
    }
    player->reserved[object] = goal;
    return core::Result<void>::success();
}
core::Result<void> Engine::release_galactic_goal(tactical::PlayerId id, std::uint64_t goal) {
    mark_native_state_changed();
    auto* player = player_ai(id);
    if (!player || !player->galactic) return refuse<void>("goal release requires a galactic player");
    const auto* found = goal_in(*player, goal);
    if (!found) return refuse<void>("goal release identity is unknown");
    if (found->active || found->plan || found->exclusions_registered || (found->potential && found->potential->funded))
        return refuse<void>("executing/funded goal release requires the composed maintenance consumer");
    auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
    if (actions && actions->qualified_freestore_available() && found->potential)
        actions->qualified_freestore().release(id, found->potential->id);
    std::erase_if(player->reserved, [&](const auto& row) { return row.second == goal; });
    std::erase_if(player->proposed, [&](const auto& row) { return row.id == goal; });
    return core::Result<void>::success();
}
} // namespace eawr::script::foc::ai
