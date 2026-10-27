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

namespace {

// #1702: the first lookup of a proposal trace that had no value, as "TOKENS: note" (the evaluator's
// own note, when it gave one). Lookups are traced as " {note} [TOKENS parameter=value...=value]".
std::string unavailable_blocker(const std::string& trace) {
    const auto end = trace.find("=unavailable]");
    if (end == std::string::npos) return {};
    const auto open = trace.rfind(" [", end);
    if (open == std::string::npos) return {};
    std::string lookup = trace.substr(open + 2, end - open - 2);
    lookup = lookup.substr(0, lookup.find(' '));
    const auto close = trace.rfind('}', open);
    if (close != std::string::npos && close + 1 == open) {
        const auto brace = trace.rfind(" {", close);
        if (brace != std::string::npos) lookup += ": " + trace.substr(brace + 2, close - brace - 2);
    }
    return lookup;
}

} // namespace

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

// Release only selected pool tokens still owned by this goal, whether or not
// Produce_Force has transferred them to a TaskForce yet.
void release_pool_reservations(PlayerAi& player, const Goal& goal) {
    if (!goal.potential) return;
    const auto& potential = *goal.potential;
    for (std::size_t i = 0; i < potential.sources.size(); ++i) {
        if (potential.sources[i] != 1) continue;
        const auto held = player.reserved_pool_tokens.find(potential.pool_tokens[i]);
        if (held == player.reserved_pool_tokens.end() || held->second != goal.id) continue;
        auto& count = player.reserved_pool[potential.units[i]];
        if (count != 0) --count;
        player.reserved_pool_tokens.erase(held);
    }
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
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {5});
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
    for (const AiPlayer& entry : host_->setup.players)
        humans += entry.human || (entry.ai && !host_->ai_admitted(entry.player)) ? 1 : 0;
    const std::int64_t others = std::max<std::int64_t>(1, static_cast<std::int64_t>(host_->setup.players.size()) - humans);
    const Real share = to_single(real(count) / to_single(real(fps) * real(5)));
    std::int64_t per_frame = truncate(share);
    if (real(per_frame) < share) ++per_frame;
    const std::int64_t cap = truncate(to_single(to_single(real(20) / real(others)) + to_single(real(1) / real(2))));
    return std::max<std::int64_t>(1, std::min(per_frame, cap));
}

void Engine::initialize_goals(PlayerAi& player) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {1});
    mark_native_state_changed();
    const auto targets = std::max<std::int64_t>(1, static_cast<std::int64_t>(player.targets.size()));
    player.per_frame = proposal_budget(targets * static_cast<std::int64_t>(player.functions.size()));
}

// GS-02 to GS-05.
void Engine::service_goals(PlayerAi& player) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {1});
    mark_native_state_changed();
    if (player.sleep_frames >= 1) {
        --player.sleep_frames;
        return;
    }
    propose(player);
    if (!player.maintenance_due) return;
    if (player.galactic) begin_galactic_maintenance(player);
    ++work().maintenances;
    maintain(player);
    player.maintenance_due = false;
    player.per_frame = proposal_budget(player.nontrivial);
    player.nontrivial = 0;
    player.sleep_frames = truncate(to_single(real(fps) * (player.galactic
        ? player.difficulty.galactic_goal_cycle_sleep : player.difficulty.space_goal_cycle_sleep)));
}

// GS-04: the proposal pass, a per-frame budget of (function, target) evaluations.
void Engine::propose(PlayerAi& player) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {1});
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
        const bool trace_all = host_->setup.journal && host_->setup.trace_proposals && !player.galactic;
        const bool trace_proposal = trace_all || (host_->setup.journal && !player.galactic
            && (function.goal->application.contains("GLOBAL") || function.goal_name == "DEFEND_SPACE_STATION"));
        std::string proposal_trace;
        const auto decision = [&](std::string event, std::string reason) {
            if (trace_all) {
                // #1702: folded by AiJournal::fold_proposal: "<reason>[ | blocker]\n<trace>".
                if (reason == "authored perception unavailable")
                    if (const auto blocker = unavailable_blocker(proposal_trace); !blocker.empty()) reason += " | " + blocker;
                journal_plan(PlanEvent{static_cast<std::uint64_t>(frame_), player.player,
                    std::string(proposal_trace_plan), function.goal_name, target_name(target), std::move(event),
                    std::move(reason) + '\n' + proposal_trace});
                return;
            }
            if (!host_->setup.journal || (function.goal_name != "PURCHASE_SPACE_UPGRADES_GENERIC"
                && function.goal_name != "SKIRMISH_UPGRADE_SPACE_STATION"
                && function.goal_name != "TACTICAL_MULTIPLAYER_BUILD_SPACE_UNITS_GENERIC"
                && function.goal_name != "GROUND_TO_SPACE_DAMAGE"
                && function.goal_name != "GROUND_TO_SPACE_DISABLE"
                && !trace_proposal)) return;
            if (trace_proposal) reason += proposal_trace;
            journal_plan(PlanEvent{static_cast<std::uint64_t>(frame_),
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
            ++work().goals_evaluated;
            const auto function_index = player.next_function;
            const auto allocated = allocate_galactic_goal(player.player, function_index, target ? target->id : 0);
            if (!allocated) throw std::runtime_error(allocated.error().message);
            if (global) { ++player.next_function; player.next_target = 0; } else ++player.next_target;
            if (allocated.value()) {
                auto& retained = player.proposed.back();
                const auto refused = !select_units(player, retained, *retained.potential) ? std::optional{GalacticRefusal::units}
                    : !production_time_allowed(player, retained) ? std::optional{GalacticRefusal::time}
                    : !galactic_fundable(player, retained, false) ? std::optional{GalacticRefusal::funds} : std::nullopt;
                if (refused) { player.count_refusal(*refused, function_index); player.proposed.pop_back(); }
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
        ++work().goals_evaluated;
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
        native_record_changed(player.player, 2, goal.id);
        player.proposed.push_back(std::move(goal));
    }
}

// PL-01: a plan is drawn for the goal among the plans of its type whose goal category the
// template turns on, weighted by the plan's success rate plus 1.
bool Engine::plan_goal(PlayerAi& player, Goal& goal) {
    const auto native_record_write = native_record_scope();
    native_record_changed(player.player, 2, goal.id); native_record_changed(player.player, 3, goal.id);
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
        return stream(player).uniform(Real{}, total);
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
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {13, 15, 16, 17});
    native_record_changed(player.player, 2, goal.id); native_record_changed(player.player, 3, goal.id);
    native_records_changed(native_all_players, {32});
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
            journal_plan(PlanEvent{static_cast<std::uint64_t>(frame_), player.player,
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
            set_pad_claim(goal.potential->producers[i], PadReservation{player.player, goal.id, goal.potential->units[i]});
    }
    goal.potential->reserved = true;
}

void Engine::change_wallet(const tactical::PlayerId player, const Real amount) {
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {31});
    mark_native_state_changed();
    if (amount == Real{}) return;
    const auto value = numeric::to_fixed(amount);
    if (!value) throw std::runtime_error("AI wallet amount: " + value.error().message);
    auto delta = value.value();
    if (delta.raw() > 0 && host_->ai_world->available()) {
        if (const auto* account = host_->ai_world->economy().player(player);
            account != nullptr && account->ai && host_->ai_admitted(player)) {
            const auto adjusted = math::multiply(delta, account->credit_multiplier);
            if (!adjusted) throw std::runtime_error("AI wallet refund: " + adjusted.error().message);
            delta = adjusted.value();
        }
    }
    // The balance check runs on the stacked wallet delta; a player turn adds its own delta to its effects.
    const auto changed = math::add(stacked_credits(player), delta);
    if (!changed) throw std::runtime_error("AI wallet balance: " + changed.error().message);
    if (auto* turn = staged()) {
        const auto own = math::add(turn->credits[player], delta);
        if (!own) throw std::runtime_error("AI wallet balance: " + own.error().message);
        turn->credits[player] = own.value();
    } else {
        host_->credit_changes[player] = changed.value();
    }
    authoritative::ScriptCommand order;
    order.verb = std::string(amount < Real{} ? verb_reservation_debit : verb_credit_grant);
    order.arguments = {Value::number(real(player)), Value{authoritative::Handle{handle_game_object, 0}},
        Value::number(amount < Real{} ? -amount : amount)};
    queue_order(std::move(order));
}

void Engine::refund_funds(PlayerAi& player, const std::uint64_t goal) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {17});
    native_records_changed(0, {31});
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
        journal_plan(PlanEvent{static_cast<std::uint64_t>(frame_), player.player,
            {}, {}, {}, "funds_refunded", "goal=" + std::to_string(goal) + " amount=" + show(funds->second)
                + " credits_before=" + show(fixed(host_->credits(player.player)))});
    change_wallet(player.player, funds->second); // WAS-26: only unused work, once
    player.reserved_credits.erase(funds);
}

void Engine::release(PlayerAi& player, Goal& goal, const bool refund) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {13, 15, 16, 17});
    native_record_changed(player.player, 2, goal.id); native_record_changed(player.player, 3, goal.id);
    native_records_changed(native_all_players, {32});
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
    release_pad_claims(player.player, goal.id);
    if (goal.potential && goal.potential->reserved && goal.plan == 0) {
        release_pool_reservations(player, goal);
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
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {2, 3});
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
        std::optional<Real> value{};
        if (function.equation) value = run_equation(*function.equation, context);
        if (observe) {
            trace += " goal_id=" + std::to_string(goal.id) + " running_plan=" + std::to_string(goal.plan)
                + " previous=" + numeric::decimal::format_lua_number(goal.desire.repr)
                + " raw=";
            if (value.has_value()) {
                trace += numeric::decimal::format_lua_number(value->repr);
            } else {
                trace += "unavailable";
            }
            journal_plan(PlanEvent{static_cast<std::uint64_t>(frame_),
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
    const std::int64_t extension = player.type ? player.type->goal_set_extension : PlayerType{}.goal_set_extension;
    std::map<std::string, std::size_t> seen;
    std::vector<Goal> culled;
    for (Goal& goal : player.proposed) {
        const std::string& category = player.functions[goal.function].goal->category;
        std::size_t active_count = 0;
        for (const Goal& active : player.active) {
            active_count += player.functions[active.function].goal->category == category ? 1 : 0;
        }
        const auto limit = static_cast<std::int64_t>(active_count) + extension;
        if (static_cast<std::int64_t>(seen[category]++) < limit) culled.push_back(std::move(goal));
    }
    player.proposed = std::move(culled);
    std::vector<Goal> kept;
    for (const auto& [budget, category] : ordered) maintain_category(player, category, kept);
    player.proposed.clear();
    // Kept goals become the active set; each gets its plan.
    player.active = std::move(kept);
    for (Goal& goal : player.active) {
        if (goal.plan != 0 || !goal.potential) continue;
        if (player.galactic && staged()) {
            // S9a: a galactic turn attaches at its service boundary (settle_galactic_attaches), in this order.
            staged()->attaches.push_back(PendingAttach{player.player, goal.id});
        } else if (staggered() && !player.galactic) {
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
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {28});
    mark_native_state_changed();
    // A goal already waiting keeps its place: the committed queue, then this turn's own intents. Intents of
    // other players carry their own goals, so reading them could not change the answer.
    for (const PendingAttach& pending : pending_attach_) {
        if (pending.player == player.player && pending.goal == goal.id) return;
    }
    if (auto* turn = staged()) {
        for (const PendingAttach& pending : turn->attaches) {
            if (pending.player == player.player && pending.goal == goal.id) return;
        }
        turn->attaches.push_back(PendingAttach{player.player, goal.id});
        return;
    }
    pending_attach_.push_back(PendingAttach{player.player, goal.id});
}

// SCH-04: attaches the queue's first `attach_per_tick` goals that still need a plan: a goal a
// later pass dropped, finished, or that got its plan some other way leaves the queue unattached.
core::Result<void> Engine::drain_attaches(authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {28});
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
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {2, 3, 5, 6, 7, 8, 9, 10, 11, 12});
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
    const auto finish = [&](Goal& goal, std::string_view reason = "completed") {
        if (goal.plan != 0) record(plan(goal.plan), player.player, "maintenance_end", std::string(reason));
        if (player.galactic && goal.plan != 0 && player.running.contains(goal.plan)) {
            finish_plan(player, goal.plan, *scripts_, reason != "completed");
            goal.exclusions_registered = false;
        } else {
            release(player, goal);
            if (goal.plan != 0) finish_plan(player, goal.plan, *scripts_, reason != "completed");
        }
    };
    const auto budget_running = [&](Goal& goal) {
        // GS-31: trimming applies even without proposals; protected plans keep zero desire.
        if (!player.galactic && !(Real{} < goal.desire)) {
            const Plan* running = goal.plan != 0 ? plan(goal.plan) : nullptr;
            if (running == nullptr || running->removable) { finish(goal, "nonpositive_desire"); return false; }
        }
        // GS-22/31: budget trimming rechecks a retained tactical allocation too,
        // including categories without proposals. Protection only exempts desire culling.
        if (!player.galactic && goal.plan != 0 && goal.potential
            && !plans_[goal.potential->plan].magic && !test_target_contrast(player, goal)) {
            finish(goal, "target_contrast");
            return false;
        }
        // WGA-12/22/75: maintenance cleared every old budget's capital and
        // income reservation. A running galactic plan must be budgeted again
        // too, using its retained completion cost/resources, not a new roster.
        if (player.galactic && goal.plan != 0) {
            const auto ledger = player.galactic_funding->ledger(player.player);
            if (!ledger || !ledger->goals.contains(goal.id)) { finish(goal, "funding"); return false; }
            if (ledger->goals.at(goal.id).reassessed) return true;
            const auto request = galactic_funding_request(player, goal);
            const auto feasible = player.galactic_funding->feasible({player.player, request, true});
            if (!feasible) throw std::runtime_error(feasible.error().message);
            if (!feasible.value()) { finish(goal, "funding"); return false; }
            const auto reserved = player.galactic_funding->reserve({player.player, request, true});
            if (!reserved) throw std::runtime_error(reserved.error().message);
        }
        return true;
    };
    if (proposed_count == 0) {
        for (Goal& goal : active) {
            if (goal.finished || (goal.plan != 0 && !player.running.contains(goal.plan))) {
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
    const auto extension = player.type ? player.type->goal_set_extension : PlayerType{}.goal_set_extension;
    const auto signed_limit = static_cast<std::int64_t>(active.size()) + extension;
    const auto limit = static_cast<std::size_t>(std::max<std::int64_t>(0, signed_limit));
    Real threshold = real(-1);
    for (const Goal& goal : candidates) threshold = std::max(threshold, maintenance_priority(player, goal));
    std::vector<Goal> keep;
    std::vector<Goal> abandon;
    std::map<std::uint64_t, const char*> fate; // #3716 diagnostic: how the pass first classified each active goal
    for (Goal& goal : active) {
        if (goal.finished || (goal.plan != 0 && !player.running.contains(goal.plan))) {
            fate[goal.id] = "finished";
            finish(goal);
            continue;
        }
        const Plan* running = goal.plan != 0 ? plan(goal.plan) : nullptr;
        const bool cullable = running == nullptr || running->removable;
        if (!(maintenance_priority(player, goal) < threshold) || !cullable) {
            if (!goal.potential) {
                keep.push_back(goal);
            } else if (!plans_[goal.potential->plan].magic && !test_target_contrast(player, goal)) {
                fate[goal.id] = "contrast-fail";
                finish(goal, "target_contrast");
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
            fate[goal.id] = "contrast-fail";
            finish(goal, "target_contrast");
        } else {
            fate[goal.id] = "below-threshold";
            abandon.push_back(goal);
        }
    }
    const auto by_desire = [&](const Goal& a, const Goal& b) {
        return maintenance_priority(player, b) < maintenance_priority(player, a);
    };
    std::stable_sort(keep.begin(), keep.end(), by_desire);
    std::stable_sort(abandon.begin(), abandon.end(), by_desire);
    // WGA-75: retained active plans consume their budget before proposals.
    std::erase_if(keep, [&](Goal& goal) { return !budget_running(goal); });
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
        return stream(player).uniform(Real{}, total);
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
            if (goal.active) finish(goal, "activation");
            register_activation(player, goal, false);
            continue;
        }
        activate(goal);
    }
    // What stays in the pool and was active is dropped.
    for (Goal& goal : pool) {
        if (goal.active) finish(goal, "replacement");
    }
    if (host_->setup.journal && !player.galactic && !active.empty()) {
        // Write-only diagnostic (#3716): each active goal's priority, the threshold and where it came from, the
        // candidates it competed with, and whether the pass kept it.
        const auto describe = [&](const Goal& goal) {
            std::string text = player.functions[goal.function].goal_name;
            if (const Target* found = goal.target != 0 ? this->target(goal.target) : nullptr; found != nullptr && found->object != 0) {
                if (const auto* view = host_->view ? host_->view->find(found->object) : nullptr) {
                    if (const auto* info = host_->type(view->type)) text += ":" + info->name;
                }
            }
            return text + "@" + numeric::decimal::format_lua_number(maintenance_priority(player, goal).repr);
        };
        std::string candidate_text;
        const Goal* best = nullptr;
        std::size_t listed = 0;
        for (const Goal& candidate : candidates) {
            if (best == nullptr || maintenance_priority(player, *best) < maintenance_priority(player, candidate)) best = &candidate;
            if (listed++ < 6) candidate_text += describe(candidate) + ";";
        }
        const std::string common = " | cat=" + category + " threshold=" + numeric::decimal::format_lua_number(threshold.repr)
            + " from=" + (best != nullptr ? describe(*best) : std::string("-")) + " active=" + std::to_string(active.size())
            + " limit=" + std::to_string(limit) + " candidates=" + std::to_string(candidates.size()) + " [" + candidate_text + "]";
        for (const Goal& goal : active) {
            const auto found = fate.find(goal.id);
            const bool kept = std::any_of(keep.begin(), keep.end(), [&](const Goal& other) { return other.id == goal.id; });
            const std::string name = found != fate.end() ? found->second : "above-threshold";
            const Target* target_info = goal.target != 0 ? this->target(goal.target) : nullptr;
            journal_plan(PlanEvent{static_cast<std::uint64_t>(frame_), player.player,
                goal.plan != 0 && player.running.contains(goal.plan) ? plans_[player.running.at(goal.plan).definition].name : std::string{},
                player.functions[goal.function].goal_name, target_name(target_info), "maintenance_goal",
                describe(goal) + " " + name + (kept ? "/kept" : "/dropped") + common}, true);
        }
    }
    for (Goal& goal : keep) if (budget_running(goal)) kept_all.push_back(goal);
}

// ---- Planning (PL-40 to PL-44) --------------------------------------------------------------

core::Result<void> Engine::attach_plan(PlayerAi& player, Goal& goal, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {2, 3, 4, 13, 14, 15, 16, 17});
    native_records_changed(native_all_players, {21, 22, 23, 24, 28, 29, 32});
    mark_native_state_changed();
    if (player.galactic && (!goal.potential || !goal.potential->valid || !goal.potential->reserved || !goal.potential->funded))
        return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request,
            "galactic plan attachment requires successful selection and reservation"));
    static_cast<void>(sequence);
    // Instance and identity allocation and the scheduler's instance creation are serial boundaries (S-T3b).
    if (staged()) throw std::logic_error("AI plan attachment cannot run inside a player turn");
    const PlanDef& definition = plans_[goal.potential->plan];
    // S9-early: refuse before any side effect when the player's galactic ranges cannot hold this plan.
    if (player.galactic && (player.next_instance_local >= galactic_instance_counter_limit
            || player.next_plan_local >= galactic_id_counter_limit
            || galactic_id_counter_limit - player.next_taskforce_local < definition.taskforces.size()))
        return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request,
            "galactic plan identity range exhausted"));
    const auto selection = player.galactic ? galactic_selection(player, goal, definition) : nullptr;
    const std::uint64_t instance = allocate_instance_id(player).value();
    if (auto created = scripts.create_instance(instance, definition.module); !created) {
        record(nullptr, player.player, "failed", definition.name + ": " + created.error().message);
        return created;
    }
    Plan plan;
    plan.id = allocate_plan_id(player).value();
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
        tf.id = allocate_taskforce_id(player).value();
        tf.thread = thread++;
        assign(tf_def.name, Value{authoritative::Handle{handle_taskforce, tf.id}});
        plan.taskforces.push_back(tf.id);
        const auto id = tf.id;
        if (player.taskforces.emplace(id, std::move(tf)).second)
            index_runtime(runtime_index_.taskforces, id, player.player);
    }
    assign("PlayerObject", Value{authoritative::Handle{handle_player, player.player}});
    for (const std::uint64_t tf_id : plan.taskforces) {
        events_.push_back(event(ScriptEvent::Kind::start_thread, player.taskforces.at(tf_id).name + "_Thread"));
    }
    goal.plan = plan.id;
    if (player.plan_of_instance.emplace(instance, plan.id).second)
        index_runtime(runtime_index_.instances, instance, player.player);
    ++work_.plans_attached;
    const std::uint64_t id = plan.id;
    if (player.running.emplace(id, std::move(plan)).second) {
        index_runtime(runtime_index_.plans, id, player.player);
    }
    std::string started = std::to_string(goal.potential->units.size()) + " units, desire " + show(goal.desire);
    if (host_->setup.journal && !player.galactic && target != nullptr && target->object != 0) {
        // Observe retained selection and current view fields only. Re-evaluating an
        // equation here could change the threat-grid request registry.
        const auto type_name = [&](const tactical::TypeId type) {
            const auto* info = host_->type(type);
            return info ? info->name : std::to_string(type);
        };
        if (const auto* view = host_->view ? host_->view->find(target->object) : nullptr) {
            started += " | target=" + type_name(view->type) + " owner=" + std::to_string(view->owner)
                + " health=" + numeric::decimal::format_lua_number(view->health.repr)
                + " shield=" + numeric::decimal::format_lua_number(view->shield.repr);
        }
        started += " | force=";
        for (const auto type : goal.potential->units) started += type_name(type) + ";";
    }
    record(&player.running.at(id), player.player, "started", std::move(started));
    return core::Result<void>::success();
}

// PL-42: every 0.1 s each plan's threads are pumped; a plan whose script exited or has no live
// thread is finished.
core::Result<void> Engine::service_plans(PlayerAi& player, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {3, 21, 22});
    native_records_changed(0, {29});
    mark_native_state_changed();
    static_cast<void>(sequence);
    // PL-45: Purge_Goals abandons every other running plan of the player that the goal system may
    // remove, in plan order.
    std::vector<std::uint64_t> purged;
    for (auto& [id, plan] : player.running) {
        if (plan.player != player.player || !plan.purge) continue;
        plan.purge = false;
        for (const auto& [other_id, other] : player.running) {
            if (other.player == player.player && other_id != id && other.removable && !other.exited &&
                std::find(purged.begin(), purged.end(), other_id) == purged.end()) {
                purged.push_back(other_id);
            }
        }
    }
    for (const std::uint64_t id : purged) {
        const std::uint64_t goal_id = player.running.at(id).goal;
        finish_plan(player, id, scripts, true);
        for (Goal& goal : player.active) {
            if (goal.id == goal_id) goal.finished = true;
        }
    }
    std::vector<std::uint64_t> finished;
    for (auto& [id, plan] : player.running) {
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
            if (auto found = player.taskforces.find(tf_id); found != player.taskforces.end()) found->second.damaged_pending = false;
        }
        ScriptEvent pump;
        pump.key = authoritative::EventKey{event_tick_, producer_foc_engine, plan.instance, 0};
        pump.target = plan.instance;
        pump.kind = ScriptEvent::Kind::pump;
        queue_event(std::move(pump));
        ++work().plans_pumped;
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
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 21, 22, 23, 24, 32});
    native_records_changed(0, {29});
    mark_native_state_changed();
    const auto found = player.running.find(plan_id);
    if (found == player.running.end()) return;
    Plan& plan = found->second;
    const PlanDef& definition = plans_[plan.definition];
    // EX-55: a retired goal cannot retain proposal holders.
    if (const auto blocker = player.proposal_blockers.find(plan.goal); blocker != player.proposal_blockers.end()) {
        player.proposal_blocks -= blocker->second;
        player.proposal_blockers.erase(blocker);
    }
    Goal* goal = nullptr;
    for (Goal& entry : player.active) {
        if (entry.id == plan.goal) goal = &entry;
    }
    record(&plan, player.player, abandoned ? "abandoned" : "finished", plan.result ? "success" : "failure");
    if (player.galactic) {
        auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
        if (goal) release(player, *goal); // WGA-348/349: choose salvage while plan and heads remain attached
        for (auto& task : player.tasks) if (task.galactic_assembly != BuildTask::GalacticAssembly::none
            && std::find(plan.taskforces.begin(), plan.taskforces.end(), task.taskforce) != plan.taskforces.end()) {
            if (!actions) throw std::runtime_error("galactic assembly finish requires its native movement owner");
            if (task.galactic_assembly_request) {
                const auto retired = actions->retire_movement(task.galactic_assembly_request);
                if (!retired) throw std::runtime_error(retired.error().message);
                task.galactic_assembly_request = 0;
            }
            if (task.object && !task.joined) {
                if (const auto* tf = taskforce(task.taskforce)) return_galactic_actor(*tf, task.object);
                player.assigned.erase(task.object);
            }
        }
        for (auto& task : player.tasks) if (task.source == 2 && task.completion
            && std::find(plan.taskforces.begin(), plan.taskforces.end(), task.taskforce) != plan.taskforces.end()) {
            if (!actions) throw std::runtime_error("galactic task finish requires its native producer owner");
            const auto finished = actions->finish_task(task.completion, task.salvage);
            if (!finished) throw std::runtime_error(finished.error().message);
            task.finished = true; task.state_entered = static_cast<std::uint64_t>(frame_);
        }
    }
    for (const std::uint64_t tf_id : plan.taskforces) {
        const auto tf = player.taskforces.find(tf_id);
        if (tf == player.taskforces.end()) continue;
        for (const sim::EntityId unit : tf->second.units) {
            return_galactic_actor(tf->second, unit);
            player.assigned.erase(unit);
        }
        for (const auto& purchase : tf->second.pooled) {
            auto& count = player.reserved_pool[purchase.type];
            if (count != 0) --count;
            player.reserved_pool_tokens.erase(purchase.token);
        }
        retire_runtime(RuntimeTable::taskforces, tf_id);
        player.taskforces.erase(tf);
    }
    // The pooled loop releases transferred tokens first. This sweep handles
    // the remaining selections and skips tokens already consumed or released.
    if (goal) release_pool_reservations(player, *goal);
    player.tasks.erase(std::remove_if(player.tasks.begin(), player.tasks.end(),
                           [&](const BuildTask& task) {
                               return std::find(plan.taskforces.begin(), plan.taskforces.end(), task.taskforce) != plan.taskforces.end();
                           }),
        player.tasks.end());
    if (player.galactic) mark_galactic_tasks_changed();
    for (auto iterator = player.blocks.begin(); iterator != player.blocks.end();) {
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
            retire_runtime(RuntimeTable::blocks, iterator->first);
            iterator = player.blocks.erase(iterator);
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
    stage_removal(scripts, plan.instance);
    refund_funds(player, plan.goal);
    release_pad_claims(player.player, plan.goal);
    static_cast<void>(definition);
    retire_runtime(RuntimeTable::instances, plan.instance);
    retire_runtime(RuntimeTable::plans, plan.id);
    player.plan_of_instance.erase(plan.instance);
    player.running.erase(found);
}

} // namespace eawr::script::foc::ai
