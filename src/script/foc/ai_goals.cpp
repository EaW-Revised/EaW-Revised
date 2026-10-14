#include "eawr/core/tick_profile.hpp"
#include "ai_engine_internal.hpp"
#include "ai_learning.hpp"
#include "galactic_ai_funding.hpp"
#include "galactic_ai_actions.hpp"
#include "eawr/script/numeric/decimal.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace eawr::script::foc::ai {

using namespace engine_internal;

namespace {
// GS-31: active tactical goals have a tapering preference over nearby proposals.
Real maintenance_priority(const PlayerAi& player, const Goal& goal) {
    if (player.galactic || !goal.active || !(real(1) / real(100) < goal.desire)
        || !(goal.desire < real(40))) return goal.desire;
    const Real preference = (real(1) - (goal.desire - real(1) / real(100))
        / (real(3999) / real(100))) * (real(8) / real(5));
    return to_single(goal.desire + preference);
}
} // namespace

// ---- Goal system ---------------------------------------------------------------------------

// GS-03: a goal type is proposable while no plan blocks proposal and the space template turns
// its category on (and not off).
bool Engine::proposable(const PlayerAi& player, const GoalFunctionEntry& function) const {
    if (player.proposal_blocks > 0) return false;
    if (player.galactic) {
        const auto enabled = player.proposal_counts.find(function.goal_name);
        const auto excluded = player.exclusion_counts.find(function.goal_name);
        return enabled != player.proposal_counts.end() && enabled->second > 0
            && (excluded == player.exclusion_counts.end() || excluded->second == 0);
    }
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
    return host_->ai_world->target_matches(player, flags, target);
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
    mark_native_state_changed();
    learning::activation(player, goal, success, frame_, static_cast<std::uint32_t>(fps));
}

Real Engine::category_budget(const PlayerAi& player, const std::string& category) const {
    if (player.galactic) {
        const auto ledger = player.galactic_funding ? player.galactic_funding->ledger(player.player) : nullptr;
        const auto id = data_.goal_categories.find(category);
        if (!ledger || id == data_.goal_categories.end() || !ledger->categories.contains(id->second)) return {};
        return numeric::from_fixed(ledger->categories.at(id->second).available_cash);
    }
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
    mark_native_state_changed();
    const auto targets = std::max<std::int64_t>(1, static_cast<std::int64_t>(player.targets.size()));
    player.per_frame = proposal_budget(targets * static_cast<std::int64_t>(player.functions.size()));
}

// GS-02 to GS-05.
void Engine::service_goals(PlayerAi& player) {
    mark_native_state_changed();
    if (player.sleep_frames >= 1) {
        --player.sleep_frames;
        return;
    }
    propose(player);
    if (!player.maintenance_due) return;
    if (player.galactic) begin_galactic_maintenance(player);
    ++work_.maintenances;
    maintain(player);
    player.maintenance_due = false;
    player.per_frame = proposal_budget(player.nontrivial);
    player.nontrivial = 0;
    player.sleep_frames = truncate(to_single(real(fps) * (player.galactic
        ? player.difficulty.galactic_goal_cycle_sleep : player.difficulty.space_goal_cycle_sleep)));
}

// GS-04: the proposal pass, a per-frame budget of (function, target) evaluations.
void Engine::propose(PlayerAi& player) {
    core::tick_profile::Scope scope("serial.goal.proposal");
    mark_native_state_changed();
    if (player.maintenance_due) return;
    for (std::int64_t step = 0; step < player.per_frame; ++step) {
        if (player.next_function >= player.functions.size()) {
            player.next_function = 0;
            player.next_target = 0;
            player.maintenance_due = true;
            return;
        }
        const GoalFunctionEntry& function = player.functions[player.next_function];
        const Target* target = nullptr;
        // Journal the evaluation already being performed; never evaluate again to observe it.
        // Global goals include authored mod purchase/funding goals as well as stock ones.
        const bool trace_proposal = host_->setup.journal && !player.galactic
            && (function.goal->application.contains("GLOBAL") || function.goal_name == "DEFEND_SPACE_STATION");
        std::string proposal_trace;
        const auto decision = [&](std::string event, std::string reason) {
            if (!host_->setup.journal || (function.goal_name != "PURCHASE_SPACE_UPGRADES_GENERIC"
                && function.goal_name != "SKIRMISH_UPGRADE_SPACE_STATION"
                && function.goal_name != "TACTICAL_MULTIPLAYER_BUILD_SPACE_UNITS_GENERIC"
                && function.goal_name != "GROUND_TO_SPACE_DAMAGE"
                && function.goal_name != "GROUND_TO_SPACE_DISABLE"
                && !trace_proposal)) return;
            if (trace_proposal) reason += proposal_trace;
            host_->setup.journal->plans.push_back(PlanEvent{static_cast<std::uint64_t>(frame_),
                player.player, {}, function.goal_name, target_name(target), std::move(event), std::move(reason)});
        };
        if (!proposable(player, function)) {
            decision("rejected", "proposal blocked or goal category disabled");
            ++player.next_function;
            player.next_target = 0;
            --step;
            continue;
        }
        const bool global = function.goal->application.contains("GLOBAL");
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
        if (player.galactic) {
            ++player.nontrivial;
            ++work_.goals_evaluated;
            const auto allocated = allocate_galactic_goal(player.player, player.next_function, target ? target->id : 0);
            if (!allocated) throw std::runtime_error(allocated.error().message);
            if (global) { ++player.next_function; player.next_target = 0; } else ++player.next_target;
            if (allocated.value()) {
                auto& retained = player.proposed.back();
                if (!select_units(player, retained, *retained.potential) || !production_time_allowed(player, retained)
                    || !galactic_fundable(player, retained, false)) player.proposed.pop_back();
            }
            continue;
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
            decision("rejected", "equivalent goal already active");
            advance();
            --step;
            continue;
        }
        ++player.nontrivial;
        ++work_.goals_evaluated;
        advance();
        if (function.equation == nullptr) {
            decision("rejected", "authored equation missing");
            continue;
        }
        auto proposal_context = context_of(player, target);
        if (trace_proposal) proposal_context.proposal_trace = &proposal_trace;
        const auto value = run_equation(*function.equation, proposal_context);
        if (!value) {
            if (trace_proposal) proposal_trace += " raw=unavailable adjusted=unavailable";
            decision("rejected", "authored perception unavailable");
            continue;
        }
        const auto goal_key = std::make_pair(function.goal_name, goal.target);
        const auto outcomes = player.goal_outcomes.find(goal_key);
        const auto activations = player.activations.find(goal_key);
        Real desire = *value;
        desire = desire + to_single(function.goal->per_failure_desire_adjust *
            real(recent_failures(outcomes == player.goal_outcomes.end() ? nullptr : &outcomes->second)));
        desire = desire + to_single(function.goal->per_activation_failure_desire_adjust *
            real(recent_failures(activations == player.activations.end() ? nullptr : &activations->second)));
        if (trace_proposal) proposal_trace += " raw=" + numeric::decimal::format_lua_number(value->repr)
            + " adjusted=" + numeric::decimal::format_lua_number(desire.repr);
        if (!(Real{} < desire)) {
            decision("rejected", "nonpositive authored desire");
            continue;
        }
        goal.desire = to_single(desire);
        decision("proposed", "positive authored desire");
        if (!plan_goal(player, goal)) {
            if (trace_proposal && goal.potential) {
                const auto& selection = *goal.potential;
                proposal_trace += " plan=" + plans_[selection.plan].module + " units=" + std::to_string(selection.units.size())
                    + " contrast_threshold=" + numeric::decimal::format_lua_number(selection.threshold.repr);
            }
            decision("rejected", "no feasible plan selection");
            continue;
        }
        if (trace_proposal && goal.potential) {
            std::string selected = "quote=" + numeric::decimal::format_lua_number(goal.potential->cost.repr)
                + " credits=" + show(fixed(host_->credits(player.player))) + " units=";
            for (const auto type : goal.potential->units) {
                const auto* info = host_->type(type);
                selected += (info ? info->name : std::to_string(type)) + ";";
            }
            decision("selected", std::move(selected));
        }
        player.proposed.push_back(std::move(goal));
    }
}

// PL-01: a plan is drawn for the goal among the plans of its type whose goal category the
// template turns on, weighted by the plan's success rate plus 1.
bool Engine::plan_goal(PlayerAi& player, Goal& goal) {
    mark_native_state_changed();
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
    const Real draw = [&] {
        core::tick_profile::Scope scope("serial.goal.ordered-rng");
        return sync_.uniform(Real{}, total);
    }();
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
    core::tick_profile::Scope scope("serial.goal.reservations");
    mark_native_state_changed();
    if (!goal.potential) return;
    if (player.galactic) {
        auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
        std::optional<galactic_ai::QualifiedFreestore> staged_pool;
        if (actions && actions->qualified_freestore_available()) {
            staged_pool = actions->qualified_freestore().clone();
            for (const auto object : goal.potential->freestore) if (object) {
                const auto* held = staged_pool->reservations(player.player);
                if (held) if (const auto found = held->find(object); found != held->end() && found->second == goal.potential->id) continue;
                const auto reserved = staged_pool->reserve(player.player, object, goal.potential->id);
                if (!reserved || !reserved.value())
                    throw std::runtime_error(reserved ? "galactic actor is already reserved" : reserved.error().message);
            }
        }
        const auto result = player.galactic_funding->reserve({player.player, galactic_funding_request(player, goal), true});
        if (!result) throw std::runtime_error(result.error().message);
        if (staged_pool) actions->qualified_freestore().replace(*staged_pool);
        for (auto object : goal.potential->freestore) if (object) player.reserved[object] = goal.id;
        if (!goal.exclusions_registered) {
            for (const auto& name : player.functions.at(goal.function).goal->global_exclusions) ++player.exclusion_counts[name];
            goal.exclusions_registered = true;
        }
        goal.potential->reserved = goal.potential->funded = true;
        return;
    }
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
    }
    if (!goal.potential->funded) {
        // WAS-25: debit the real wallet once and keep only the refundable remainder.
        if (host_->setup.journal)
            host_->setup.journal->plans.push_back(PlanEvent{static_cast<std::uint64_t>(frame_), player.player,
                {}, goal.function < player.functions.size() ? player.functions[goal.function].goal_name : std::string(),
                target_name(target(goal.target)),
                "funds_reserved", "goal=" + std::to_string(goal.id) + " amount=" + show(goal.potential->cost)
                    + " credits_before=" + show(fixed(host_->credits(player.player)))});
        player.reserved_credits[goal.id] = goal.potential->cost;
        change_wallet(player.player, -goal.potential->cost);
        goal.potential->cost = Real{};
        goal.potential->funded = true;
    }
    for (std::size_t i = 0; i < goal.potential->sources.size(); ++i) {
        if (goal.potential->sources[i] == 2 && host_->ai_world->available()
            && host_->ai_world->pads().contains(goal.potential->producers[i]))
            pad_reservations_[goal.potential->producers[i]] = {player.player, goal.id, goal.potential->units[i]};
    }
    goal.potential->reserved = true;
}

void Engine::change_wallet(const tactical::PlayerId player, const Real amount) {
    mark_native_state_changed();
    if (amount == Real{}) return;
    const auto value = numeric::to_fixed(amount);
    if (!value) throw std::runtime_error("AI wallet amount: " + value.error().message);
    auto delta = value.value();
    if (delta.raw() > 0 && host_->ai_world->available()) {
        if (const auto* account = host_->ai_world->economy().player(player); account != nullptr && account->ai) {
            const auto adjusted = math::multiply(delta, account->credit_multiplier);
            if (!adjusted) throw std::runtime_error("AI wallet refund: " + adjusted.error().message);
            delta = adjusted.value();
        }
    }
    const auto changed = math::add(host_->credit_changes[player], delta);
    if (!changed) throw std::runtime_error("AI wallet balance: " + changed.error().message);
    host_->credit_changes[player] = changed.value();
    authoritative::ScriptCommand order;
    order.sequence = order_sequence_++;
    order.verb = std::string(amount < Real{} ? verb_reservation_debit : verb_credit_grant);
    order.arguments = {Value::number(real(player)), Value{authoritative::Handle{handle_game_object, 0}},
        Value::number(amount < Real{} ? -amount : amount)};
    orders_.push_back(std::move(order));
}

void Engine::refund_funds(PlayerAi& player, const std::uint64_t goal) {
    mark_native_state_changed();
    if (player.galactic) {
        auto* funding = dynamic_cast<GalacticFunding*>(player.galactic_funding.get());
        if (!funding) throw std::runtime_error("galactic release requires the retained funding owner");
        salvage_galactic_goal(player, goal);
        return; // WGA-349: category release and aggregate dead budget; no wallet grant.
    }
    const auto funds = player.reserved_credits.find(goal);
    if (funds == player.reserved_credits.end()) return;
    if (host_->setup.journal)
        host_->setup.journal->plans.push_back(PlanEvent{static_cast<std::uint64_t>(frame_), player.player,
            {}, {}, {}, "funds_refunded", "goal=" + std::to_string(goal) + " amount=" + show(funds->second)
                + " credits_before=" + show(fixed(host_->credits(player.player)))});
    change_wallet(player.player, funds->second); // WAS-26: only unused work, once
    player.reserved_credits.erase(funds);
}

void Engine::release(PlayerAi& player, Goal& goal, const bool refund) {
    core::tick_profile::Scope scope("serial.goal.reservations");
    mark_native_state_changed();
    if (player.galactic && goal.potential) {
        auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
        if (actions && actions->qualified_freestore_available())
            actions->qualified_freestore().release(player.player, goal.potential->id);
    }
    if (player.galactic && refund && goal.exclusions_registered) {
        for (const auto& name : player.functions.at(goal.function).goal->global_exclusions) {
            auto found = player.exclusion_counts.find(name);
            if (found != player.exclusion_counts.end() && found->second) --found->second;
        }
        goal.exclusions_registered = false;
    }
    if (refund) refund_funds(player, goal.id);
    std::erase_if(pad_reservations_, [&](const auto& entry) {
        return entry.second.player == player.player && entry.second.goal == goal.id;
    });
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
    core::tick_profile::Scope scope("serial.goal.maintenance");
    mark_native_state_changed();
    for (auto& goal : player.active) {
        const auto& function = player.functions.at(goal.function);
        // GS-30: refresh tactical active desire before sorting and replacement too.
        if (!player.galactic && !function.goal->application.contains("GLOBAL")
            && !applies(player, *function.goal, target(goal.target))) continue;
        auto context = context_of(player, target(goal.target));
        std::string trace;
        const bool observe = host_->setup.journal && !player.galactic
            && function.goal->application.contains("GLOBAL");
        if (observe) context.proposal_trace = &trace;
        const auto value = function.equation ? run_equation(*function.equation, context) : std::nullopt;
        if (observe) {
            trace += " goal_id=" + std::to_string(goal.id) + " running_plan=" + std::to_string(goal.plan)
                + " previous=" + numeric::decimal::format_lua_number(goal.desire.repr)
                + " raw=" + (value ? numeric::decimal::format_lua_number(value->repr) : "unavailable");
            host_->setup.journal->plans.push_back(PlanEvent{static_cast<std::uint64_t>(frame_),
                player.player, {}, function.goal_name, target_name(target(goal.target)),
                "maintenance_evaluation", std::move(trace)});
        }
        if (!value) {
            if (player.galactic) goal.finished = true;
            continue;
        }
        if (!player.galactic) {
            // GS-30: current tactical desire is a fresh raw equation value, not proposal history.
            goal.desire = to_single(std::clamp(to_single(*value), Real{}, huge));
            continue;
        }
        const auto key = std::make_pair(function.goal_name, goal.target);
        const auto failures = player.goal_outcomes.find(key), activations = player.activations.find(key);
        goal.desire = to_single(*value
            + to_single(function.goal->per_failure_desire_adjust * real(recent_failures(failures == player.goal_outcomes.end() ? nullptr : &failures->second)))
            + to_single(function.goal->per_activation_failure_desire_adjust * real(recent_failures(activations == player.activations.end() ? nullptr : &activations->second))));
        if (!(Real{} < goal.desire)) goal.finished = true;
    }
    const auto by_desire = [&](const Goal& a, const Goal& b) {
        return maintenance_priority(player, b) < maintenance_priority(player, a);
    };
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
    if (player.galactic) {
        const auto ledger = player.galactic_funding->ledger(player.player);
        const auto ids = funding::category_order(*ledger);
        if (!ids) throw std::runtime_error(ids.error().message);
        ordered.clear();
        for (auto id : ids.value()) for (const auto& category : categories)
            if (data_.goal_categories.at(category) == id) ordered.emplace_back(category_budget(player, category), category);
    }
    // Reservations are rebuilt by the pass.
    for (Goal& goal : player.active) {
        if (goal.potential && goal.plan == 0) {
            release(player, goal, false); // WAS-25: ignored/restored reservations retain funds
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
        if (staggered() && !player.galactic) {
            queue_attach(player, goal);
        } else if (auto attached = attach_plan(player, goal, *scripts_, sequence_); !attached) {
            goal.finished = true;
            if (player.galactic) release(player, goal);
        }
    }
}

// SCH-04: a goal that has its units waits in the attach queue, in the order the passes made it
// (player, then goal order), until a tick has an attach budget. A goal already waiting keeps its
// place.
void Engine::queue_attach(PlayerAi& player, const Goal& goal) {
    mark_native_state_changed();
    for (const PendingAttach& pending : pending_attach_) {
        if (pending.player == player.player && pending.goal == goal.id) return;
    }
    pending_attach_.push_back(PendingAttach{player.player, goal.id});
}

// SCH-04: attaches the queue's first `attach_per_tick` goals that still need a plan: a goal a
// later pass dropped, finished, or that got its plan some other way leaves the queue unattached.
core::Result<void> Engine::drain_attaches(authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    mark_native_state_changed();
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
    mark_native_state_changed();
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
        if (player.galactic && goal.plan != 0 && running_.contains(goal.plan)) {
            finish_plan(player, goal.plan, *scripts_);
            goal.exclusions_registered = false;
        } else {
            release(player, goal);
            if (goal.plan != 0) finish_plan(player, goal.plan, *scripts_);
        }
    };
    const auto budget_running = [&](Goal& goal) {
        // GS-31: trimming applies even without proposals; protected plans keep zero desire.
        if (!player.galactic && !(Real{} < goal.desire)) {
            const Plan* running = goal.plan != 0 ? plan(goal.plan) : nullptr;
            if (running == nullptr || running->removable) { finish(goal); return false; }
        }
        // WGA-12/22/75: maintenance cleared every old budget's capital and
        // income reservation. A running galactic plan must be budgeted again
        // too, using its retained completion cost/resources, not a new roster.
        if (player.galactic && goal.plan != 0) {
            const auto ledger = player.galactic_funding->ledger(player.player);
            if (!ledger || !ledger->goals.contains(goal.id)) { finish(goal); return false; }
            if (ledger->goals.at(goal.id).reassessed) return true;
            const auto request = galactic_funding_request(player, goal);
            const auto feasible = player.galactic_funding->feasible({player.player, request, true});
            if (!feasible) throw std::runtime_error(feasible.error().message);
            if (!feasible.value()) { finish(goal); return false; }
            const auto reserved = player.galactic_funding->reserve({player.player, request, true});
            if (!reserved) throw std::runtime_error(reserved.error().message);
        }
        return true;
    };
    if (proposed_count == 0) {
        for (Goal& goal : active) {
            if (goal.finished || (goal.plan != 0 && !running_.contains(goal.plan))) {
                finish(goal);
                continue;
            }
            if (goal.potential && goal.plan == 0) {
                if (player.galactic && !test_valid(player, goal)) {
                    // WGA-215: eligible active reassessment records admission failure too.
                    register_activation(player, goal, false); finish(goal); continue;
                }
                reserve(player, goal);
            }
            if (budget_running(goal)) kept_all.push_back(goal);
        }
        return;
    }
    const std::size_t limit = active.size() + 2;
    Real threshold = real(-1);
    for (const Goal& goal : candidates) threshold = std::max(threshold, maintenance_priority(player, goal));
    std::vector<Goal> keep;
    std::vector<Goal> abandon;
    for (Goal& goal : active) {
        if (goal.finished || (goal.plan != 0 && !running_.contains(goal.plan))) {
            finish(goal);
            continue;
        }
        const Plan* running = goal.plan != 0 ? plan(goal.plan) : nullptr;
        const bool cullable = running == nullptr || running->removable;
        if (!(maintenance_priority(player, goal) < threshold) || !cullable) {
            if (!goal.potential) {
                keep.push_back(goal);
            } else if (!plans_[goal.potential->plan].magic && !test_target_contrast(player, goal)) {
                finish(goal);
            } else {
                if (goal.plan == 0) {
                    if (player.galactic && !test_valid(player, goal)) {
                        // WGA-215: eligible active reassessment records admission failure too.
                        register_activation(player, goal, false); finish(goal); continue;
                    }
                    reserve(player, goal);
                }
                keep.push_back(goal);
            }
        } else if (goal.potential && !test_target_contrast(player, goal)) {
            finish(goal);
        } else {
            abandon.push_back(goal);
        }
    }
    const auto by_desire = [&](const Goal& a, const Goal& b) {
        return maintenance_priority(player, b) < maintenance_priority(player, a);
    };
    std::stable_sort(keep.begin(), keep.end(), by_desire);
    std::stable_sort(abandon.begin(), abandon.end(), by_desire);
    // WGA-75: retained active plans consume their budget before proposals.
    if (player.galactic) std::erase_if(keep, [&](Goal& goal) { return !budget_running(goal); });
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
        if (!budget_running(goal)) return false;
        if (goal.plan == 0) reserve(player, goal);
        if (!goal.active) {
            goal.active = true;
            register_activation(player, goal, true);
        }
        keep.push_back(goal);
        return true;
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
        if (!activate(goal)) continue;
        if (first_abandon && like(player, goal, *first_abandon)) break;
    }
    // Pass 2: desire-weighted draws among the rest.
    while (keep.size() < limit && !pool.empty()) {
        std::vector<std::size_t> eligible;
        Real total{};
        for (std::size_t index = 0; index < pool.size(); ++index) {
            if (!proposable(player, player.functions[pool[index].function]) || like_kept(pool[index])) continue;
            eligible.push_back(index);
            total = to_single(total + maintenance_priority(player, pool[index]));
        }
        if (eligible.empty()) break;
        const Real draw = [&] {
        core::tick_profile::Scope scope("serial.goal.ordered-rng");
        return sync_.uniform(Real{}, total);
    }();
        std::size_t chosen = eligible.back();
        Real sum{};
        for (const std::size_t index : eligible) {
            sum = to_single(sum + maintenance_priority(player, pool[index]));
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
    for (Goal& goal : keep) if (budget_running(goal)) kept_all.push_back(goal);
}

// ---- Planning (PL-40 to PL-44) --------------------------------------------------------------

core::Result<void> Engine::attach_plan(PlayerAi& player, Goal& goal, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    mark_native_state_changed();
    if (player.galactic && (!goal.potential || !goal.potential->valid || !goal.potential->reserved || !goal.potential->funded))
        return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request,
            "galactic plan attachment requires successful selection and reservation"));
    static_cast<void>(sequence);
    const PlanDef& definition = plans_[goal.potential->plan];
    const auto selection = player.galactic ? galactic_selection(player, goal, definition) : nullptr;
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
        if (host_->ai_world->available() && host_->ai_world->pads().contains(factory)) plan.reserved_pads.push_back(factory);
    }
    const auto event = [&](ScriptEvent::Kind kind, std::string name) {
        ScriptEvent out;
        out.key = authoritative::EventKey{event_tick_, producer_foc_engine, instance, sequence_++};
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
        if (selection && index < selection->force_stages.size()) tf.galactic_stage = selection->force_stages[index];
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
    mark_native_state_changed();
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
        pump.key = authoritative::EventKey{event_tick_, producer_foc_engine, plan.instance, sequence_++};
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

// WGA-212/213: first completion/abandonment records the retained result
// for both goal and plan before their live association is detached.
void Engine::finish_plan(PlayerAi& player, std::uint64_t plan_id, authoritative::ScriptScheduler& scripts, const bool abandoned) {
    mark_native_state_changed();
    const auto found = running_.find(plan_id);
    if (found == running_.end()) return;
    Plan& plan = found->second;
    const PlanDef& definition = plans_[plan.definition];
    Goal* goal = nullptr;
    for (Goal& entry : player.active) {
        if (entry.id == plan.goal) goal = &entry;
    }
    record(&plan, player.player, abandoned ? "abandoned" : "finished", plan.result ? "success" : "failure");
    if (player.galactic) {
        auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
        if (goal) release(player, *goal); // WGA-348/349: choose salvage while plan and heads remain attached
        for (auto& task : player.tasks) if (task.source == 2 && task.completion
            && std::find(plan.taskforces.begin(), plan.taskforces.end(), task.taskforce) != plan.taskforces.end()) {
            if (!actions) throw std::runtime_error("galactic task finish requires its native producer owner");
            const auto finished = actions->finish_task(task.completion, task.salvage);
            if (!finished) throw std::runtime_error(finished.error().message);
            task.finished = true; task.state_entered = static_cast<std::uint64_t>(frame_);
        }
    }
    for (const std::uint64_t tf_id : plan.taskforces) {
        const auto tf = taskforces_.find(tf_id);
        if (tf == taskforces_.end()) continue;
        for (const sim::EntityId unit : tf->second.units) {
            return_galactic_actor(tf->second, unit);
            player.assigned.erase(unit);
        }
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
    if (player.galactic) mark_galactic_tasks_changed();
    for (auto iterator = blocks_.begin(); iterator != blocks_.end();) {
        if (iterator->second.instance == plan.instance) {
            if (iterator->second.galactic_request && iterator->second.kind == Block::Kind::move) {
                auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
                if (!actions) throw std::runtime_error("galactic block release requires its native listener owner");
                const auto retired = actions->retire_movement(iterator->second.galactic_request);
                if (!retired) throw std::runtime_error(retired.error().message);
            }
            if (iterator->second.galactic_request && iterator->second.kind == Block::Kind::galactic_invade) {
                auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
                if (!actions) throw std::runtime_error("invasion block release requires its native queue owner");
                const auto retired = actions->retire_invasion(iterator->second.galactic_request);
                if (!retired) throw std::runtime_error(retired.error().message);
            }
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
    if (goal != nullptr) learning::outcome(player, *goal, plan, frame_, static_cast<std::uint32_t>(fps));
    static_cast<void>(scripts.remove_instance(plan.instance));
    refund_funds(player, plan.goal);
    std::erase_if(pad_reservations_, [&](const auto& entry) {
        return entry.second.player == player.player && entry.second.goal == plan.goal;
    });
    static_cast<void>(definition);
    plan_of_instance_.erase(plan.instance);
    running_.erase(found);
}

} // namespace eawr::script::foc::ai
