#include "ai_engine_internal.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace eawr::script::foc::ai {

using namespace engine_internal;

// ---- Goal system ---------------------------------------------------------------------------

// GS-03: a goal type is proposable while no plan blocks proposal and the space template turns
// its category on (and not off).
bool Engine::proposable(const PlayerAi& player, const GoalFunctionEntry& function) const {
    if (player.proposal_blocks > 0) return false;
    if (player.space_template == nullptr) return true;
    const std::string& category = function.goal->category;
    return player.space_template->goals_on.contains(category) && !player.space_template->goals_off.contains(category);
}

// FT-01/FT-03: the best starts as none at score 0 and is replaced only by a strictly higher
// score, so ties keep the earlier candidate and nothing above 0 means no best. A replaced best
// and every other candidate enter the draw, but a weight of 0 or less does not; with nothing in
// the draw, or a zero fraction, the best (or nil) is the answer.
std::optional<std::size_t> choose_target(const std::vector<Real>& scores, Real fraction, const std::function<Real()>& unit_draw) {
    std::optional<std::size_t> best;
    Real best_score{};
    std::vector<std::pair<std::size_t, Real>> weighed;
    const auto weigh = [&weighed](std::optional<std::size_t> candidate, Real weight) {
        if (candidate && Real{} < weight) weighed.emplace_back(*candidate, weight);
    };
    for (std::size_t index = 0; index < scores.size(); ++index) {
        if (best_score < scores[index]) {
            weigh(best, to_single(best_score));
            best = index;
            best_score = scores[index];
        } else {
            weigh(index, to_single(scores[index]));
        }
    }
    if (weighed.empty() || fraction == Real{}) return best;
    Real total{};
    for (const auto& entry : weighed) total = to_single(total + entry.second);
    weigh(best, to_single(total / to_single(fraction)));
    Real sum{};
    for (const auto& entry : weighed) sum = to_single(sum + entry.second);
    const Real draw = unit_draw() * sum;
    Real running{};
    std::size_t answer = weighed.back().first;
    for (const auto& [index, weight] : weighed) {
        running = to_single(running + weight);
        if (draw < running) {
            answer = index;
            break;
        }
    }
    return answer;
}

// GS-11: which targets a goal applies to. A region is a TACTICAL_LOCATION; an object is a
// friendly (allied owner) or enemy unit, or a structure (a star base).
bool Engine::target_matches(tactical::PlayerId player, const std::set<std::string>& flags, const Target* target) const {
    if (target == nullptr) return false;
    if (target->object == 0) return flags.contains("TACTICAL_LOCATION");
    const ViewUnit* unit = host_->view->find(target->object);
    if (unit == nullptr) return false;
    const bool allied = unit->owner != 0 && host_->players.contains(unit->owner) && !host_->neutral(unit->owner) &&
        host_->allied(unit->owner, player);
    const AiType* type = host_->type(unit->type);
    // GS-11: capture ownership alone does not make a target a build pad.
    if (type != nullptr && type->build_pad)
        return flags.contains(allied ? "FRIENDLY_BUILD_PAD" : "ENEMY_BUILD_PAD");
    const bool structure = type != nullptr && (type->star_base || (type->capture_point && !type->locomotor));
    const std::string flag = std::string(allied ? "FRIENDLY_" : "ENEMY_") + (structure ? "STRUCTURE" : "UNIT");
    return flags.contains(flag);
}

bool Engine::applies(const PlayerAi& player, const GoalType& goal, const Target* target) const {
    return target_matches(player.player, goal.application, target);
}

// GS-12: goals are alike when they share a target and a goal type, or one names the other's
// type in Is_Like.
bool Engine::like(const PlayerAi& player, const Goal& a, const Goal& b) const {
    if (a.target != b.target) return false;
    const GoalFunctionEntry& left = player.functions[a.function];
    const GoalFunctionEntry& right = player.functions[b.function];
    if (left.goal_name == right.goal_name) return true;
    const auto& a_like = left.goal->is_like;
    const auto& b_like = right.goal->is_like;
    return std::find(a_like.begin(), a_like.end(), right.goal_name) != a_like.end() ||
        std::find(b_like.begin(), b_like.end(), left.goal_name) != b_like.end();
}

std::int64_t Engine::recent_failures(const History* history) const {
    if (history == nullptr) return 0;
    std::int64_t count = 0;
    for (const auto& [success, expiry] : history->entries) count += success ? 0 : 1;
    return count;
}

void Engine::register_activation(PlayerAi& player, const Goal& goal, bool success) {
    const GoalFunctionEntry& function = player.functions[goal.function];
    const Real duration = function.goal->activation_tracking_duration;
    const std::int64_t expiry = duration < Real{} ? -1 : frame_ + truncate(to_single(duration * real(fps)));
    player.activations[{function.goal_name, goal.target}].entries.emplace_back(success, expiry);
}

Real Engine::category_budget(const PlayerAi& player, const std::string& category) const {
    if (player.space_template == nullptr) return Real{};
    const auto found = player.space_template->budget.find(category);
    if (found == player.space_template->budget.end()) return Real{};
    const Equation* equation = data_.equations.find(found->second);
    if (equation == nullptr) return Real{};
    return run_equation(*equation, context_of(player, nullptr)).value_or(Real{});
}

// GS-05: initialization and later passes use the same rounded and capped budget.
std::int64_t Engine::proposal_budget(std::int64_t count) const {
    std::int64_t humans = 0;
    for (const AiPlayer& entry : host_->setup.players) humans += entry.human ? 1 : 0;
    const std::int64_t others = std::max<std::int64_t>(1, static_cast<std::int64_t>(host_->setup.players.size()) - humans);
    const Real share = to_single(real(count) / to_single(real(fps) * real(5)));
    std::int64_t per_frame = truncate(share);
    if (real(per_frame) < share) ++per_frame;
    const std::int64_t cap = truncate(to_single(to_single(real(20) / real(others)) + to_single(real(1) / real(2))));
    return std::max<std::int64_t>(1, std::min(per_frame, cap));
}

void Engine::initialize_goals(PlayerAi& player) {
    const auto targets = std::max<std::int64_t>(1, static_cast<std::int64_t>(player.targets.size()));
    player.per_frame = proposal_budget(targets * static_cast<std::int64_t>(player.functions.size()));
}

// GS-02 to GS-05.
void Engine::service_goals(PlayerAi& player) {
    if (player.sleep_frames >= 1) {
        --player.sleep_frames;
        return;
    }
    propose(player);
    if (!player.maintenance_due) return;
    ++work_.maintenances;
    maintain(player);
    player.maintenance_due = false;
    player.per_frame = proposal_budget(player.nontrivial);
    player.nontrivial = 0;
    player.sleep_frames = truncate(to_single(real(fps) * player.difficulty.space_goal_cycle_sleep));
}

// GS-04: the proposal pass, a per-frame budget of (function, target) evaluations.
void Engine::propose(PlayerAi& player) {
    if (player.maintenance_due) return;
    for (std::int64_t step = 0; step < player.per_frame; ++step) {
        if (player.next_function >= player.functions.size()) {
            player.next_function = 0;
            player.next_target = 0;
            player.maintenance_due = true;
            return;
        }
        const GoalFunctionEntry& function = player.functions[player.next_function];
        if (!proposable(player, function)) {
            ++player.next_function;
            player.next_target = 0;
            --step;
            continue;
        }
        const bool global = function.goal->application.contains("GLOBAL");
        const Target* target = nullptr;
        if (!global) {
            if (player.next_target >= player.targets.size()) {
                ++player.next_function;
                player.next_target = 0;
                --step;
                continue;
            }
            target = this->target(player.targets[player.next_target]);
            if (!applies(player, *function.goal, target)) {
                ++player.next_target;
                --step;
                continue;
            }
        }
        Goal goal;
        goal.id = player.next_goal_id++;
        goal.function = player.next_function;
        goal.target = target == nullptr ? 0 : target->id;
        const auto advance = [&] {
            if (global) {
                ++player.next_function;
                player.next_target = 0;
            } else {
                ++player.next_target;
            }
        };
        bool culled = false;
        for (const Goal& active : player.active) culled = culled || like(player, active, goal);
        if (culled) {
            advance();
            --step;
            continue;
        }
        ++player.nontrivial;
        ++work_.goals_evaluated;
        advance();
        if (function.equation == nullptr) continue;
        const auto value = run_equation(*function.equation, context_of(player, target));
        if (!value) continue;
        const auto goal_key = std::make_pair(function.goal_name, goal.target);
        const auto outcomes = player.goal_outcomes.find(goal_key);
        const auto activations = player.activations.find(goal_key);
        Real desire = *value;
        desire = desire + to_single(function.goal->per_failure_desire_adjust *
            real(recent_failures(outcomes == player.goal_outcomes.end() ? nullptr : &outcomes->second)));
        desire = desire + to_single(function.goal->per_activation_failure_desire_adjust *
            real(recent_failures(activations == player.activations.end() ? nullptr : &activations->second)));
        if (!(Real{} < desire)) continue;
        goal.desire = to_single(desire);
        if (!plan_goal(player, goal)) continue;
        player.proposed.push_back(std::move(goal));
    }
}

// PL-01: a plan is drawn for the goal among the plans of its type whose goal category the
// template turns on, weighted by the plan's success rate plus 1.
bool Engine::plan_goal(PlayerAi& player, Goal& goal) {
    const GoalFunctionEntry& function = player.functions[goal.function];
    std::vector<std::pair<std::size_t, Real>> usable;
    Real total{};
    for (std::size_t index = 0; index < plans_.size(); ++index) {
        const PlanDef& plan = plans_[index];
        if (std::find(plan.goals.begin(), plan.goals.end(), function.goal_name) == plan.goals.end()) continue;
        if (player.space_template != nullptr) {
            const std::string& category = function.goal->category;
            if (!player.space_template->plans_on.contains(category) || player.space_template->plans_off.contains(category)) {
                continue;
            }
        }
        Real rate = real(1);
        if (const auto found = player.plan_success.find(index); found != player.plan_success.end() && found->second.second > 0) {
            rate = to_single(real(found->second.first) / real(found->second.second));
        }
        const Real weight = to_single(rate + real(1));
        usable.emplace_back(index, weight);
        total = to_single(total + weight);
    }
    if (usable.empty()) return false;
    const Real draw = sync_.uniform(Real{}, total);
    std::size_t chosen = usable.back().first;
    Real sum{};
    for (const auto& [index, weight] : usable) {
        sum = to_single(sum + weight);
        if (draw < sum) {
            chosen = index;
            break;
        }
    }
    PotentialPlan potential;
    potential.plan = chosen;
    const bool valid = select_units(player, goal, potential);
    goal.potential = std::move(potential);
    return valid;
}

void Engine::reserve(PlayerAi& player, Goal& goal) {
    if (!goal.potential) return;
    for (const sim::EntityId object : goal.potential->freestore) {
        if (object != 0) player.reserved[object] = goal.id;
    }
    if (!goal.potential->reserved) {
        for (std::size_t i = 0; i < goal.potential->sources.size(); ++i) {
            if (goal.potential->sources[i] == 1) {
                ++player.reserved_pool[goal.potential->units[i]];
                player.reserved_pool_tokens[goal.potential->pool_tokens[i]] = goal.id;
            }
        }
        player.reserved_credits[goal.id] = goal.potential->cost;
    }
    goal.potential->reserved = true;
}

void Engine::release(PlayerAi& player, const Goal& goal) {
    player.reserved_credits.erase(goal.id);
    if (goal.potential && goal.potential->reserved && goal.plan == 0) {
        for (std::size_t i = 0; i < goal.potential->sources.size(); ++i) {
            if (goal.potential->sources[i] == 1) {
                auto& count = player.reserved_pool[goal.potential->units[i]];
                if (count != 0) --count;
                player.reserved_pool_tokens.erase(goal.potential->pool_tokens[i]);
            }
        }
    }
    for (auto iterator = player.reserved.begin(); iterator != player.reserved.end();) {
        if (iterator->second == goal.id) {
            iterator = player.reserved.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

// GS-30: goal set maintenance, category by category in budget order.
void Engine::maintain(PlayerAi& player) {
    const auto by_desire = [](const Goal& a, const Goal& b) { return b.desire < a.desire; };
    std::stable_sort(player.active.begin(), player.active.end(), by_desire);
    std::vector<std::string> categories;
    for (const auto* set : {&player.active, &player.proposed}) {
        for (const Goal& goal : *set) {
            const std::string& category = player.functions[goal.function].goal->category;
            if (std::find(categories.begin(), categories.end(), category) == categories.end()) categories.push_back(category);
        }
    }
    std::sort(categories.begin(), categories.end());
    std::vector<std::pair<Real, std::string>> ordered;
    for (const std::string& category : categories) ordered.emplace_back(category_budget(player, category), category);
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return b.first < a.first; });
    // Reservations are rebuilt by the pass.
    for (Goal& goal : player.active) {
        if (goal.potential && goal.plan == 0) {
            release(player, goal);
            goal.potential->reserved = false;
        }
    }
    // GS-31: at most the active count plus the extension of proposals per category.
    std::stable_sort(player.proposed.begin(), player.proposed.end(), by_desire);
    const std::size_t extension = 2; // GS-31: the player type's goal set extension (engine default)
    std::map<std::string, std::size_t> seen;
    std::vector<Goal> culled;
    for (Goal& goal : player.proposed) {
        const std::string& category = player.functions[goal.function].goal->category;
        std::size_t active_count = 0;
        for (const Goal& active : player.active) {
            active_count += player.functions[active.function].goal->category == category ? 1 : 0;
        }
        if (seen[category]++ < active_count + extension) culled.push_back(std::move(goal));
    }
    player.proposed = std::move(culled);
    std::vector<Goal> kept;
    for (const auto& [budget, category] : ordered) maintain_category(player, category, kept);
    player.proposed.clear();
    // Kept goals become the active set; each gets its plan.
    player.active = std::move(kept);
    for (Goal& goal : player.active) {
        if (goal.plan != 0 || !goal.potential) continue;
        if (staggered()) {
            queue_attach(player, goal);
        } else if (auto attached = attach_plan(player, goal, *scripts_, *sequence_); !attached) {
            goal.finished = true;
        }
    }
}

// SCH-04: a goal that has its units waits in the attach queue, in the order the passes made it
// (player, then goal order), until a tick has an attach budget. A goal already waiting keeps its
// place.
void Engine::queue_attach(PlayerAi& player, const Goal& goal) {
    for (const PendingAttach& pending : pending_attach_) {
        if (pending.player == player.player && pending.goal == goal.id) return;
    }
    pending_attach_.push_back(PendingAttach{player.player, goal.id});
}

// SCH-04: attaches the queue's first `attach_per_tick` goals that still need a plan: a goal a
// later pass dropped, finished, or that got its plan some other way leaves the queue unattached.
core::Result<void> Engine::drain_attaches(authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    std::size_t taken = 0;
    std::uint32_t attached = 0;
    while (taken < pending_attach_.size() && attached < host_->setup.schedule.attach_per_tick) {
        const PendingAttach pending = pending_attach_[taken++];
        PlayerAi* player = player_ai(pending.player);
        Goal* goal = player != nullptr ? find_goal(*player, pending.goal) : nullptr;
        if (goal == nullptr || goal->finished || goal->plan != 0 || !goal->potential) continue;
        ++attached;
        if (auto created = attach_plan(*player, *goal, scripts, sequence); !created) goal->finished = true;
    }
    pending_attach_.erase(pending_attach_.begin(), pending_attach_.begin() + static_cast<std::ptrdiff_t>(taken));
    return core::Result<void>::success();
}

void Engine::maintain_category(PlayerAi& player, const std::string& category, std::vector<Goal>& kept_all) {
    const auto in_category = [&](const Goal& goal) { return player.functions[goal.function].goal->category == category; };
    std::vector<Goal> active;
    for (Goal& goal : player.active) {
        if (in_category(goal)) active.push_back(goal);
    }
    std::vector<Goal> candidates;
    std::size_t proposed_count = 0;
    for (Goal& goal : player.proposed) {
        if (!in_category(goal)) continue;
        ++proposed_count;
        const bool global = player.functions[goal.function].goal->application.contains("GLOBAL");
        if (global || applies(player, *player.functions[goal.function].goal, target(goal.target))) candidates.push_back(goal);
    }
    const auto finish = [&](Goal& goal) {
        release(player, goal);
        if (goal.plan != 0) finish_plan(player, goal.plan, *scripts_);
    };
    if (proposed_count == 0) {
        for (Goal& goal : active) {
            if (goal.finished || (goal.plan != 0 && !running_.contains(goal.plan))) continue;
            if (goal.potential && goal.plan == 0) reserve(player, goal);
            kept_all.push_back(goal);
        }
        return;
    }
    const std::size_t limit = active.size() + 2;
    Real threshold = real(-1);
    for (const Goal& goal : candidates) threshold = std::max(threshold, goal.desire);
    std::vector<Goal> keep;
    std::vector<Goal> abandon;
    for (Goal& goal : active) {
        if (goal.finished || (goal.plan != 0 && !running_.contains(goal.plan))) {
            finish(goal);
            continue;
        }
        const Plan* running = goal.plan != 0 ? plan(goal.plan) : nullptr;
        const bool cullable = running == nullptr || running->removable;
        if (!(goal.desire < threshold) || !cullable) {
            if (!goal.potential) {
                keep.push_back(goal);
            } else if (!plans_[goal.potential->plan].magic && !test_target_contrast(player, goal)) {
                finish(goal);
            } else {
                if (goal.plan == 0) reserve(player, goal);
                keep.push_back(goal);
            }
        } else if (goal.potential && !test_target_contrast(player, goal)) {
            finish(goal);
        } else {
            abandon.push_back(goal);
        }
    }
    const auto by_desire = [](const Goal& a, const Goal& b) { return b.desire < a.desire; };
    std::stable_sort(keep.begin(), keep.end(), by_desire);
    std::stable_sort(abandon.begin(), abandon.end(), by_desire);
    std::optional<Goal> first_abandon;
    if (!abandon.empty()) first_abandon = abandon.front();
    std::vector<Goal> pool = abandon;
    pool.insert(pool.end(), candidates.begin(), candidates.end());
    std::stable_sort(pool.begin(), pool.end(), by_desire);
    const auto like_kept = [&](const Goal& goal) {
        for (const Goal& other : keep) {
            if (other.id != goal.id && like(player, other, goal)) return true;
        }
        for (const Goal& other : kept_all) {
            if (other.id != goal.id && like(player, other, goal)) return true;
        }
        return false;
    };
    const auto activate = [&](Goal& goal) {
        if (goal.plan == 0) reserve(player, goal);
        if (!goal.active) {
            goal.active = true;
            register_activation(player, goal, true);
        }
        keep.push_back(goal);
    };
    // Pass 1: in desire order until a goal cannot go in.
    while (!pool.empty() && keep.size() < limit) {
        Goal goal = pool.front();
        if (!proposable(player, player.functions[goal.function]) || like_kept(goal)) break;
        if (goal.potential && !test_valid(player, goal)) {
            register_activation(player, goal, false);
            break;
        }
        pool.erase(pool.begin());
        activate(goal);
        if (first_abandon && like(player, goal, *first_abandon)) break;
    }
    // Pass 2: desire-weighted draws among the rest.
    while (keep.size() < limit && !pool.empty()) {
        std::vector<std::size_t> eligible;
        Real total{};
        for (std::size_t index = 0; index < pool.size(); ++index) {
            if (!proposable(player, player.functions[pool[index].function]) || like_kept(pool[index])) continue;
            eligible.push_back(index);
            total = to_single(total + pool[index].desire);
        }
        if (eligible.empty()) break;
        const Real draw = sync_.uniform(Real{}, total);
        std::size_t chosen = eligible.back();
        Real sum{};
        for (const std::size_t index : eligible) {
            sum = to_single(sum + pool[index].desire);
            if (draw < sum) {
                chosen = index;
                break;
            }
        }
        Goal goal = pool[chosen];
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(chosen));
        if (goal.potential && !test_valid(player, goal)) {
            if (goal.active) finish(goal);
            register_activation(player, goal, false);
            continue;
        }
        activate(goal);
    }
    // What stays in the pool and was active is dropped.
    for (Goal& goal : pool) {
        if (goal.active) finish(goal);
    }
    for (Goal& goal : keep) kept_all.push_back(goal);
}

// ---- Planning (PL-40 to PL-44) --------------------------------------------------------------

core::Result<void> Engine::attach_plan(PlayerAi& player, Goal& goal, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    static_cast<void>(sequence);
    const PlanDef& definition = plans_[goal.potential->plan];
    const std::uint64_t instance = next_instance_++;
    if (auto created = scripts.create_instance(instance, definition.module); !created) {
        record(nullptr, player.player, "failed", definition.name + ": " + created.error().message);
        return created;
    }
    Plan plan;
    plan.id = next_plan_id_++;
    plan.instance = instance;
    plan.definition = goal.potential->plan;
    plan.goal = goal.id;
    plan.player = player.player;
    plan.target = goal.target;
    if (const Target* target = this->target(goal.target)) plan.target_object = target->object;
    plan.start_tick = static_cast<std::uint64_t>(frame_);
    plan.requires_production = std::find(goal.potential->sources.begin(), goal.potential->sources.end(), 2) != goal.potential->sources.end();
    for (const auto factory : goal.potential->producers) {
        if (host_->world != nullptr && host_->world->pads().contains(factory)) plan.reserved_pads.push_back(factory);
    }
    const auto event = [&](ScriptEvent::Kind kind, std::string name) {
        ScriptEvent out;
        out.key = authoritative::EventKey{event_tick_, producer_foc_engine, instance, (*sequence_)++};
        out.target = instance;
        out.kind = kind;
        out.name = std::move(name);
        return out;
    };
    const auto assign = [&](std::string name, std::optional<Value> value) {
        ScriptEvent out = event(ScriptEvent::Kind::assign, std::move(name));
        out.parameter = std::move(value);
        events_.push_back(std::move(out));
    };
    events_.push_back(event(ScriptEvent::Kind::call, "Base_Definitions"));
    const Target* target = this->target(goal.target);
    assign("Target", target != nullptr && target->object != 0
            ? std::optional<Value>(Value{authoritative::Handle{handle_game_object, target->object}})
            : std::nullopt);
    assign("AITarget", target != nullptr ? std::optional<Value>(Value{authoritative::Handle{handle_ai_target, target->id}})
                                         : std::nullopt);
    std::int64_t thread = 0;
    for (std::size_t index = 0; index < definition.taskforces.size(); ++index) {
        const TaskForceDef& tf_def = definition.taskforces[index];
        auto thread_function = scripts.read_global(instance, tf_def.name + "_Thread");
        const bool has_thread = thread_function && !thread_function.value();
        if (!has_thread) continue; // PL-41: a TaskForce without its thread function is dropped
        TaskForce tf;
        tf.plan = plan.id;
        tf.definition = index;
        tf.name = tf_def.name;
        // PL-40: TaskForce types follow authored team order, retaining selection
        // order within each team. Produce_Force queues purchases in this order.
        std::vector<std::size_t> selected;
        for (std::size_t unit = 0; unit < goal.potential->units.size(); ++unit) {
            if (goal.potential->taskforce_of_unit[unit] == index) selected.push_back(unit);
        }
        std::stable_sort(selected.begin(), selected.end(), [&](const auto left, const auto right) {
            return goal.potential->team_of_unit[left] < goal.potential->team_of_unit[right];
        });
        for (const std::size_t unit : selected) {
            tf.types.push_back(goal.potential->units[unit]);
            tf.sources.push_back(goal.potential->sources[unit]);
            tf.producers.push_back(goal.potential->producers[unit]);
            tf.pool_tokens.push_back(goal.potential->pool_tokens[unit]);
        }
        // PL-40: no selected live or reinforcement units means no optional TaskForce,
        // and therefore no coroutine that could abort the other forces' plan.
        if (tf.types.empty() && !tf_def.required) continue;
        tf.id = next_taskforce_id_++;
        tf.thread = thread++;
        assign(tf_def.name, Value{authoritative::Handle{handle_taskforce, tf.id}});
        plan.taskforces.push_back(tf.id);
        taskforces_.emplace(tf.id, std::move(tf));
    }
    assign("PlayerObject", Value{authoritative::Handle{handle_player, player.player}});
    for (const std::uint64_t tf_id : plan.taskforces) {
        events_.push_back(event(ScriptEvent::Kind::start_thread, taskforces_.at(tf_id).name + "_Thread"));
    }
    goal.plan = plan.id;
    plan_of_instance_.emplace(instance, plan.id);
    ++work_.plans_attached;
    const std::uint64_t id = plan.id;
    running_.emplace(id, std::move(plan));
    record(&running_.at(id), player.player, "started",
        std::to_string(goal.potential->units.size()) + " units, desire " + show(goal.desire));
    return core::Result<void>::success();
}

// PL-42: every 0.1 s each plan's threads are pumped; a plan whose script exited or has no live
// thread is finished.
core::Result<void> Engine::service_plans(PlayerAi& player, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    static_cast<void>(sequence);
    // PL-45: Purge_Goals abandons every other running plan of the player that the goal system may
    // remove, in plan order.
    std::vector<std::uint64_t> purged;
    for (auto& [id, plan] : running_) {
        if (plan.player != player.player || !plan.purge) continue;
        plan.purge = false;
        for (const auto& [other_id, other] : running_) {
            if (other.player == player.player && other_id != id && other.removable && !other.exited &&
                std::find(purged.begin(), purged.end(), other_id) == purged.end()) {
                purged.push_back(other_id);
            }
        }
    }
    for (const std::uint64_t id : purged) {
        const std::uint64_t goal_id = running_.at(id).goal;
        finish_plan(player, id, scripts, true);
        for (Goal& goal : player.active) {
            if (goal.id == goal_id) goal.finished = true;
        }
    }
    std::vector<std::uint64_t> finished;
    for (auto& [id, plan] : running_) {
        if (plan.player != player.player) continue;
        if (plan.exited) {
            finished.push_back(id);
            continue;
        }
        if (plan.start_tick < static_cast<std::uint64_t>(frame_)) {
            auto slots = scripts.thread_slots(plan.instance);
            bool live = false;
            if (slots) {
                for (const bool slot : slots.value()) live = live || slot;
            }
            if (!live) {
                finished.push_back(id);
                continue;
            }
        }
        for (const std::uint64_t tf_id : plan.taskforces) {
            if (auto found = taskforces_.find(tf_id); found != taskforces_.end()) found->second.damaged_pending = false;
        }
        ScriptEvent pump;
        pump.key = authoritative::EventKey{event_tick_, producer_foc_engine, plan.instance, (*sequence_)++};
        pump.target = plan.instance;
        pump.kind = ScriptEvent::Kind::pump;
        events_.push_back(std::move(pump));
        ++work_.plans_pumped;
    }
    for (const std::uint64_t id : finished) {
        finish_plan(player, id, scripts);
        for (Goal& goal : player.active) {
            if (goal.plan == id) goal.finished = true;
        }
    }
    return core::Result<void>::success();
}

// PL-43: a plan ends: its outcome is learned, its units go back to the free store, its build
// tasks and blocks go and its script instance is removed. PL-45: an abandoned plan learns only its
// goal's outcome, with the plan's result so far.
void Engine::finish_plan(PlayerAi& player, std::uint64_t plan_id, authoritative::ScriptScheduler& scripts, const bool abandoned) {
    const auto found = running_.find(plan_id);
    if (found == running_.end()) return;
    Plan& plan = found->second;
    const PlanDef& definition = plans_[plan.definition];
    const Goal* goal = nullptr;
    for (const Goal& entry : player.active) {
        if (entry.id == plan.goal) goal = &entry;
    }
    if (goal != nullptr) {
        // LS-01: goal and plan outcomes.
        const GoalFunctionEntry& function = player.functions[goal->function];
        const Real duration = function.goal->tracking_duration;
        const std::int64_t expiry = duration < Real{} ? -1 : frame_ + truncate(to_single(duration * real(fps)));
        player.goal_outcomes[{function.goal_name, goal->target}].entries.emplace_back(plan.result, expiry);
        if (!abandoned) player.plan_outcomes[{plan.definition, goal->target}].entries.emplace_back(plan.result, expiry);
    }
    if (!abandoned) {
        auto& success = player.plan_success[plan.definition];
        success.first += plan.result ? 1 : 0;
        success.second += 1;
    }
    record(&plan, player.player, abandoned ? "abandoned" : "finished", plan.result ? "success" : "failure");
    for (const std::uint64_t tf_id : plan.taskforces) {
        const auto tf = taskforces_.find(tf_id);
        if (tf == taskforces_.end()) continue;
        for (const sim::EntityId unit : tf->second.units) player.assigned.erase(unit);
        for (const auto& purchase : tf->second.pooled) {
            auto& count = player.reserved_pool[purchase.type];
            if (count != 0) --count;
            player.reserved_pool_tokens.erase(purchase.token);
        }
        taskforces_.erase(tf);
    }
    player.tasks.erase(std::remove_if(player.tasks.begin(), player.tasks.end(),
                           [&](const BuildTask& task) {
                               return std::find(plan.taskforces.begin(), plan.taskforces.end(), task.taskforce) != plan.taskforces.end();
                           }),
        player.tasks.end());
    for (auto iterator = blocks_.begin(); iterator != blocks_.end();) {
        if (iterator->second.instance == plan.instance) {
            iterator = blocks_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = player.reserved.begin(); iterator != player.reserved.end();) {
        if (iterator->second == plan.goal) {
            iterator = player.reserved.erase(iterator);
        } else {
            ++iterator;
        }
    }
    static_cast<void>(scripts.remove_instance(plan.instance));
    player.reserved_credits.erase(plan.goal);
    static_cast<void>(definition);
    plan_of_instance_.erase(plan.instance);
    running_.erase(found);
}

} // namespace eawr::script::foc::ai
