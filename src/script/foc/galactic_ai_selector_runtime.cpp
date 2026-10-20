#include "eawr/core/gc_profile.hpp"
#include "ai_engine_internal.hpp"
#include "galactic_ai_funding.hpp"
#include "galactic_ai_actions.hpp"
#include "ai_learning.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_selection_content.hpp"
#include "galactic_ai_execution_work.hpp"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace eawr::script::foc::ai {
using namespace engine_internal;
namespace gc = sim::galactic;
namespace gg = galactic_goals;
namespace {
struct StageKey {
    const PlanDef* plan{};
    gc::PlanetId target{};
    std::string_view policy;
    bool operator<(const StageKey& other) const {
        if (plan != other.plan) return std::less<const PlanDef*>{}(plan, other.plan);
        return std::tie(target, policy) < std::tie(other.target, other.policy);
    }
};
struct ExecutionFacts;
thread_local ExecutionFacts* execution_facts{};
thread_local GalacticExecutionWork completed_execution_work;
// The scope belongs to one ordered player execution turn. Snapshot/input owners
// prevent pointer reuse, and any publication change discards the derived rows.
// Actor predicates, assignments and route arithmetic remain live per read.
struct ExecutionFacts {
    const Engine* engine;
    tactical::PlayerId player;
    ExecutionFacts* previous{execution_facts};
    GalacticExecutionWork work;
    std::shared_ptr<const gc::GalacticSnapshot> snapshot;
    std::shared_ptr<const galactic_ai::WorldInput> input;
    std::map<StageKey, std::shared_ptr<const std::vector<gc::PlanetId>>> stages;
    ExecutionFacts(const Engine* owner, tactical::PlayerId id) : engine(owner), player(id) { execution_facts = this; }
    ~ExecutionFacts() { completed_execution_work = work; execution_facts = previous; }
    void publication(std::shared_ptr<const gc::GalacticSnapshot> next_snapshot,
        std::shared_ptr<const galactic_ai::WorldInput> next_input) {
        if (snapshot != next_snapshot || input != next_input) {
            stages.clear(); snapshot = std::move(next_snapshot); input = std::move(next_input);
        }
    }
};
template<class T> T required(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void required(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
const sim::PartitionExecutor& executor(const sim::PartitionExecutor* supplied) {
    static const sim::InlineExecutor serial;
    return supplied ? *supplied : serial;
}
gg::ProductionEstimate estimate(const GalacticSelectionInput& input, const PotentialPlan& potential,
    bool magic, bool targeted, Real multiplier, bool linear, const sim::PartitionExecutor& workers,
    gc::PlayerId player) {
    if (!input.complete) return {false, gg::rejected_estimate(), {}, 0};
    std::vector<gg::ProductionRequest> requests;
    requests.reserve(potential.units.size());
    Real seconds{};
    for (std::size_t i = 0; i < potential.units.size(); ++i) {
        if (potential.sources[i] != 2 || magic) continue;
        const auto found = input.types.find(potential.units[i]);
        if (found == input.types.end()) return {false, gg::rejected_estimate(), {}, 0};
        const auto& type = found->second;
        requests.push_back({potential.units[i], type.channel, type.structure || type.base,
            targeted && (type.base || type.instance_allowed) && type.target_build_seconds.has_value(),
            targeted && (type.structure || type.base) ? type.target_build_seconds.value_or(Real{}) : type.build_seconds});
        seconds = to_single(seconds + to_single(type.build_seconds * multiplier));
    }
    if (linear) return {true, seconds, {}, 0};
    return required(gg::estimate_production(input.producers, player, requests, input.reachable, workers));
}
}

GalacticExecutionWork galactic_execution_work() noexcept { return completed_execution_work; }

std::shared_ptr<const GalacticSelectionInput> Engine::galactic_selection(const PlayerAi& player,
    const Goal& goal, const PlanDef& plan) const {
    if (!galactic_selection_) return {};
    const auto* at = target(goal.target);
    return required(galactic_selection_(player.player, at ? at->object : 0,
        *player.functions.at(goal.function).goal, plan));
}

gg::ProductionEstimate Engine::galactic_estimate(const PlayerAi& player, const Goal& goal,
    const PotentialPlan& potential, bool linear) const {
    const auto input = galactic_selection(player, goal, plans_.at(potential.plan));
    if (!input || !input->complete) return {false, gg::rejected_estimate(), {}, 0};
    return estimate(*input, potential, plans_[potential.plan].magic, goal.target != 0,
        player.difficulty.galactic_build_time_multiplier, linear, executor(perception_executor_), player.player);
}

gc::funding::GoalRequest Engine::galactic_funding_request(const PlayerAi& player, const Goal& goal) const {
    const auto& definition = *player.functions.at(goal.function).goal;
    const auto category = data_.goal_categories.find(definition.category);
    if (category == data_.goal_categories.end()) throw std::runtime_error("galactic goal category is not registered");
    gc::funding::GoalRequest out; out.id = goal.id; out.category = category->second;
    if (goal.potential && !goal.plan) {
        out.cost = required(numeric::to_fixed(goal.potential->cost));
        const auto estimate = galactic_estimate(player, goal, *goal.potential, false);
        if (!estimate.feasible) throw std::runtime_error("galactic funding production estimate unavailable");
        out.estimated_seconds = required(numeric::to_fixed(estimate.seconds));
    }
    if (definition.time_limit != real(-1)) out.time_limit = required(numeric::to_fixed(definition.time_limit)); // WGA-346 exact sentinel
    if (goal.plan && player.galactic_funding) {
        const auto ledger = player.galactic_funding->ledger(player.player);
        if (ledger && ledger->goals.contains(goal.id)) {
            const auto& held = ledger->goals.at(goal.id);
            // WGA-21/22: the existing plan uses its saved funding horizon.
            // Its accepted producers need not admit a second copy of its roster.
            out.cost = held.cost; out.estimated_seconds = held.remaining; out.unallocated_resources = held.unallocated;
        } else throw std::runtime_error("running galactic goal requires its retained funding budget");
    }
    // WGA-21/345: selection supplies the unattached goal's paid-roster budget.
    if (!goal.plan) out.proposal_budget = out.cost;
    return out;
}
bool Engine::galactic_fundable(const PlayerAi& player, const Goal& goal, bool activation) const {
    if (!player.galactic_funding || !goal.potential) return false;
    if (!galactic_estimate(player, goal, *goal.potential, false).feasible) return false;
    return required(player.galactic_funding->feasible({player.player, galactic_funding_request(player, goal), activation}));
}
void Engine::begin_galactic_maintenance(PlayerAi& player) {
    auto* owner = dynamic_cast<GalacticFunding*>(player.galactic_funding.get());
    if (!owner) throw std::runtime_error("galactic maintenance requires the retained funding owner");
    const auto current = owner->ledger(player.player);
    if (!current) throw std::runtime_error("galactic maintenance requires an initialized funding ledger");
    std::vector<EvaluatedFundingWeight> weights;
    for (const auto& [name, setting] : player.galactic_policy.equations) {
        const auto category = data_.goal_categories.find(name);
        if (category == data_.goal_categories.end()) throw std::runtime_error("galactic funding category is not registered");
        if (!current->categories.contains(category->second)) throw std::runtime_error("galactic maintenance requires an existing funding category");
        std::optional<math::Fixed> weight;
        if (!setting.value.empty()) {
            const auto value = evaluate(setting.value, player.player, nullptr);
            if (!value) player.count_refusal(GalacticRefusal::budget, category->second);
            // WGA-421: a present failed equation preserves its caller's current
            // share. An absent equation remains distinct and resets to zero.
            weight = required(funding_equation_weight(current.get(), category->second, value));
        }
        weights.push_back({category->second, weight});
    }
    required(owner->begin_maintenance(player.player, weights));
}

// WGA-58..60/66: proposal layout and synchronized choices commit in authored
// order. Each scoring pass partitions immutable candidate contributions; the
// ordered selected roster determines the next pass's counts and queue estimate.
bool Engine::select_galactic_units(PlayerAi& player, const Goal& goal, PotentialPlan& potential) {
    gc_profile::Scope profile_scope(gc_profile::Phase::selection);
    const auto native_record_write = native_record_scope();
    native_record_changed(player.player, 2, goal.id); native_record_changed(player.player, 3, goal.id);
    mark_native_state_changed();
    const auto& plan = plans_.at(potential.plan);
    const auto input = galactic_selection(player, goal, plan);
    potential.valid = false;
    if (!input || !input->complete || !input->reachable || plan.taskforces.empty()) return false;
    const auto contrast_factor = [&](const AiType& type, std::uint64_t category) {
        if (!plan.galactic_contrast_qualified) return average_contrast(*host_, type, category);
        const auto found = plan.galactic_contrast.find(category);
        if (found == plan.galactic_contrast.end()) return Real{};
        Real sum{}; std::uint32_t count{}; bool matched{};
        for (const auto& entry : found->second) {
            if (entry.type) { if (*entry.type == type.type_id) return entry.weight; continue; }
            if (!(entry.category_bits & type.category_bits)) continue;
            matched = true;
            if (entry.weight != real(1)) { sum += entry.weight; ++count; }
        }
        return count ? sum / real(count) : matched ? real(1) : Real{};
    };
    potential.units.clear(); potential.freestore.clear(); potential.sources.clear();
    potential.producers.clear(); potential.pool_tokens.clear(); potential.taskforce_of_unit.clear(); potential.team_of_unit.clear();
    potential.cost = {};
    potential.target_contrast.clear();
    const auto history = player.plan_outcomes.find({potential.plan, goal.target});
    const Real adjustment = to_single(real(recent_failures(history == player.plan_outcomes.end() ? nullptr : &history->second)) * plan.per_failure_contrast_adjust);
    const auto maximum = to_single(plan.max_contrast + adjustment);
    const auto minimum = to_single(plan.min_contrast + adjustment);
    potential.threshold = goal.target && !plan.ignore_target && Real{} < maximum ? to_single(real(1) - minimum / maximum) : Real{};
    if (goal.target && !plan.ignore_target) for (const auto& row : input->contrast) {
        const auto force = input->unscaled_domain_totals
            ? row.category ? row.force * ((plan.max_contrast + adjustment) * player.difficulty.galactic_contrast_multiplier) : row.force
            : to_single(row.force * to_single(maximum * player.difficulty.galactic_contrast_multiplier));
        potential.target_contrast.push_back({row.category, force});
    }
    potential.current_contrast = potential.target_contrast;
    struct Slot { std::size_t force{}, team{}; std::int32_t count{}; Real cost{}; };
    std::vector<Slot> slots;
    std::vector<std::vector<gc::TypeId>> slot_types;
    std::vector<gc::TypeId> proposals;
    const auto proposal_type = [&](gc::TypeId type) {
        const auto found = input->proposal_substitutions.find(type);
        return found == input->proposal_substitutions.end() ? type : found->second;
    };
    for (std::size_t f = 0; f < plan.taskforces.size(); ++f) for (std::size_t t = 0; t < plan.taskforces[f].teams.size(); ++t) {
        slots.push_back({f, t, 0});
        auto& types = slot_types.emplace_back();
        for (const auto authored : plan.taskforces[f].teams[t].types) {
            const auto type = proposal_type(authored);
            types.push_back(type);
            if (input->types.contains(type) && std::find(proposals.begin(), proposals.end(), type) == proposals.end()) proposals.push_back(type);
        }
    }
    std::vector<std::vector<std::size_t>> teams(proposals.size());
    std::vector<std::size_t> starts(proposals.size());
    for (std::size_t i = 0; i < proposals.size(); ++i) {
        for (std::size_t s = 0; s < slots.size(); ++s) {
            const auto& types = slot_types[s];
            if (std::find(types.begin(), types.end(), proposals[i]) != types.end()) teams[i].push_back(s);
        }
        if (teams[i].size() > 1) starts[i] = static_cast<std::size_t>(stream(player).range(0, static_cast<std::int32_t>(teams[i].size()) - 1));
    }
    std::vector<std::int32_t> force_size(plan.taskforces.size());
    std::vector<Real> force_power(plan.taskforces.size());
    std::vector<std::uint64_t> force_categories(plan.taskforces.size());
    Real current_cost{};
    const auto needs = [&] {
        if (current_cost < input->minimum_cost) return true;
        for (const auto& slot : slots) if (slot.count < plan.taskforces[slot.force].teams[slot.team].min_count) return true;
        for (std::size_t f = 0; f < force_size.size(); ++f)
            if (force_size[f] < plan.taskforces[f].minimum_size || force_power[f] < plan.taskforces[f].minimum_force) return true;
        for (auto category : plan.required_categories)
            if (std::none_of(force_categories.begin(), force_categories.end(), [&](auto bits) { return (bits & category) != 0; })) return true;
        return false;
    };
    // WGA-252: classified breakdown is a weighted residual sum per domain;
    // category zero is checked separately by the zero-weight fallback.
    const auto breakdown_met = [&](bool escort, gc::ProductionDomain channel) {
        Real total{}, unmatched{};
        for (std::size_t row = 0; row < input->contrast.size(); ++row)
            if (input->contrast[row].escort == escort && input->contrast[row].channel == channel && input->contrast[row].category)
                total = to_single(total + potential.target_contrast[row].force);
        if (Real{} < total) for (std::size_t row = 0; row < input->contrast.size(); ++row) {
            if (input->contrast[row].escort != escort || input->contrast[row].channel != channel || !input->contrast[row].category) continue;
            const auto target_force = potential.target_contrast[row].force;
            const auto current = potential.current_contrast[row].force;
            if (Real{} < target_force && Real{} < current)
                unmatched = to_single(unmatched + to_single(to_single(current / target_force) * to_single(target_force / total)));
        }
        return !(potential.threshold < unmatched);
    };
    const auto deficits = [&] {
        for (const bool escort : {false, true}) for (const auto channel : {gc::ProductionDomain::ground, gc::ProductionDomain::space})
            if (!breakdown_met(escort, channel)) return true;
        return false;
    };
    std::set<sim::EntityId> used;
    std::vector<bool> erased(proposals.size());
    Real remaining = input->remaining_population;
    auto structure_slots = input->remaining_structure_slots_by_channel;
    std::map<gc::TypeId, std::uint32_t> selected_structures;
    const auto ledger = player.galactic_funding ? player.galactic_funding->ledger(player.player) : nullptr;
    const auto category = data_.goal_categories.find(player.functions.at(goal.function).goal->category);
    if (!ledger || category == data_.goal_categories.end() || !ledger->categories.contains(category->second)) return false;
    const auto& budget = ledger->categories.at(category->second);
    const Real cash = numeric::from_fixed(budget.available_cash);
    const Real income = numeric::from_fixed(budget.available_income);
    const auto time_limit = player.functions.at(goal.function).goal->time_limit;
    const auto resources = [&](Real seconds) {
        if (!(time_limit < Real{})) seconds = std::min(seconds, time_limit);
        return std::max(to_single(cash + to_single(income * std::max(seconds, Real{}))), Real{});
    };
    Real current_budget = resources(Real{});
    Real current_seconds{};
    Real max_path{};
    struct FreeCursor {
        std::vector<const GalacticSelectionObject*> rows;
        std::size_t next{};
    };
    std::map<gc::TypeId, FreeCursor> free_by_type;
    for (const auto& row : input->freestore) {
        max_path = std::max(max_path, row.path_cost);
        if (!plan.allow_free_store) continue;
        const auto* unit = host_->view->find(row.object);
        if (!unit || unit->owner != player.player || player.assigned.contains(row.object)) continue;
        const auto held = player.reserved.find(row.object);
        if (held != player.reserved.end() && held->second != goal.id) continue;
        // WGA-312: proposals match the retained original pool type.
        free_by_type[row.type].rows.push_back(&row);
    }
    for (auto& [type, cursor] : free_by_type) {
        static_cast<void>(type);
        // WGA-60: equal costs keep the publication's ascending identity order.
        std::stable_sort(cursor.rows.begin(), cursor.rows.end(), [](const auto* a, const auto* b) {
            return a->path_cost < b->path_cost;
        });
    }
    std::vector<gg::ProductionRequest> production_requests;
    std::map<gc::TypeId, std::uint32_t> selected_counts;
    const auto production_request = [&](gc::TypeId type) {
        const auto& facts = input->types.at(type);
        return gg::ProductionRequest{type, facts.channel, facts.structure || facts.base,
            goal.target && (facts.base || facts.instance_allowed) && facts.target_build_seconds.has_value(),
            goal.target && (facts.structure || facts.base) ? facts.target_build_seconds.value_or(Real{}) : facts.build_seconds};
    };
    std::vector<std::vector<std::size_t>> garrison_rows(proposals.size());
    for (std::size_t i = 0; i < proposals.size(); ++i) garrison_rows[i].resize(input->types.at(proposals[i]).garrisons.size());
    for (;;) {
        std::vector<gg::SelectionCandidate> candidates(proposals.size());
        std::vector<sim::EntityId> objects(proposals.size()), factories(proposals.size());
        std::vector<std::size_t> allocations(proposals.size(), slots.size()), contrast_rows(proposals.size(), input->contrast.size());
        std::vector<Real> excess;
        std::vector<std::size_t> production_rows;
        for (std::size_t i = 0; i < proposals.size(); ++i) {
            auto& candidate = candidates[i]; candidate.type = proposals[i];
            candidate.selected_count = selected_counts[proposals[i]];
            auto stage = [&](gg::SelectionStage which, Real weight, bool permanent = false) {
                candidate.stages[static_cast<std::size_t>(which)] = {weight, permanent};
            };
            if (erased[i]) { stage(gg::SelectionStage::freestore, gg::rejected_estimate(), true); continue; }
            const auto& facts = input->types.at(proposals[i]);
            const auto* info = host_->type(proposals[i]);
            if (!info) { stage(gg::SelectionStage::freestore, gg::rejected_estimate(), true); continue; }
            const GalacticSelectionObject* best{};
            if (const auto found = free_by_type.find(proposals[i]); found != free_by_type.end()) {
                auto& cursor = found->second;
                while (cursor.next < cursor.rows.size() && used.contains(cursor.rows[cursor.next]->object)) ++cursor.next;
                if (cursor.next < cursor.rows.size()) best = cursor.rows[cursor.next];
            }
            if (best) {
                objects[i] = best->object;
                stage(gg::SelectionStage::freestore, gg::freestore_preference(best->path_cost, max_path, best->garrison, player.reserved.contains(best->object)));
            }
            // WGA-60: no tactical reserve pool. Ordinary production uses C's
            // feasibility resources; activation uses available resources later.
            if (!best && !plan.magic) {
                if ((facts.structure || facts.base) && goal.target && (!facts.target_cost || !facts.target_build_seconds)) {
                    stage(gg::SelectionStage::budget, gg::rejected_estimate(), true); continue;
                }
                Real available = current_budget;
                if (current_seconds == Real{}) {
                    Real horizon = (facts.structure || facts.base) && goal.target ? *facts.target_build_seconds
                        : to_single(facts.build_seconds / real(2));
                    if (!(time_limit < Real{})) horizon = std::min(horizon, time_limit);
                    available = to_single(available + to_single(income * horizon));
                }
                const auto candidate_cost = (facts.structure || facts.base) && goal.target ? *facts.target_cost : facts.cost;
                if (available < to_single(potential.cost + candidate_cost)) stage(gg::SelectionStage::budget, gg::rejected_estimate());
            }
            if (candidate.stages[static_cast<std::size_t>(gg::SelectionStage::budget)].weight == gg::rejected_estimate()) continue;
            stage(gg::SelectionStage::tech, !best && facts.tech > input->player_tech
                ? gg::rejected_estimate() : to_single(real(2) * real(facts.tech) / real(5)), !best && facts.tech > input->player_tech);
            if (candidate.stages[static_cast<std::size_t>(gg::SelectionStage::tech)].weight == gg::rejected_estimate()) continue;
            Real team_weight = gg::rejected_estimate();
            for (std::size_t step = 0; step < teams[i].size(); ++step) {
                const auto s = teams[i][(starts[i] + step) % teams[i].size()];
                const auto& slot = slots[s]; const auto& team = plan.taskforces[slot.force].teams[slot.team];
                if (slot.force < input->attachment_allowed.size()) {
                    const auto allowed = input->attachment_allowed[slot.force];
                    if ((facts.hero && !allowed.first) || (facts.special_weapon && !allowed.second)) continue;
                }
                if (team.percentage_based) {
                    // WGA-60: ignored-target percentage teams use cost and the
                    // target budget; ordinary targeted teams contribute one.
                    if (plan.ignore_target && !(to_single(slot.cost + facts.cost)
                        < to_single(resources(Real{}) * real(team.percentage) / real(100)))) continue;
                    team_weight = real(1); allocations[i] = s; break;
                }
                if (slot.count < team.max_count && team.max_count > 0) {
                    team_weight = to_single(real(1) - real(slot.count) / real(team.max_count)); allocations[i] = s; break;
                }
            }
            stage(gg::SelectionStage::team, team_weight);
            if (team_weight == gg::rejected_estimate()) continue;
            if (best) stage(gg::SelectionStage::production, real(1) / real(2));
            else if ((facts.structure && (!goal.target || !facts.instance_allowed || selected_structures[proposals[i]] >= facts.instance_remaining
                    || !structure_slots[static_cast<std::size_t>(facts.channel)])) || (plan.magic && !facts.magic_allowed)
                || (!plan.magic && input->population_check && remaining < facts.population))
                stage(gg::SelectionStage::production, gg::rejected_estimate(), true);
            else if (plan.magic) stage(gg::SelectionStage::production, stream(player).uniform(Real{}, real(1)));
            else {
                production_requests.push_back(production_request(proposals[i]));
                const auto production = required(gg::estimate_production(input->producers, player.player,
                    production_requests, input->reachable, executor(perception_executor_)));
                production_requests.pop_back();
                if (!production.feasible) stage(gg::SelectionStage::production, gg::rejected_estimate(), true);
                else {
                    factories[i] = facts.structure || facts.base ? target(goal.target)->object : production.producers.back();
                    const auto mean = facts.structure || facts.base ? facts.build_seconds
                        : required(gg::estimate_single_type(input->producers, player.player, proposals[i], facts.channel, executor(nullptr)));
                    // WGA-482: admitted base production starts at zero excess,
                    // then subtracts the type's raw build time before normalization.
                    excess.push_back(facts.base ? -facts.build_seconds : facts.structure ? facts.build_seconds
                        : to_single(mean - facts.build_seconds)); production_rows.push_back(i);
                }
            }
            if (candidate.stages[static_cast<std::size_t>(gg::SelectionStage::production)].weight == gg::rejected_estimate()) continue;
            if (facts.hero) stage(gg::SelectionStage::hero,
                !facts.hero_containment_allowed || to_single(facts.hero_weight + facts.escort_weight) < real(-100)
                || (Real{} < facts.hero_maximum_cost && facts.hero_maximum_cost < current_cost)
                || (Real{} < facts.hero_minimum_cost && current_cost < facts.hero_minimum_cost)
                ? gg::rejected_estimate() : to_single(facts.hero_weight + facts.escort_weight), true);
            if (candidate.stages[static_cast<std::size_t>(gg::SelectionStage::hero)].weight == gg::rejected_estimate()) continue;
            stage(gg::SelectionStage::terrain, facts.terrain_allowed ? facts.terrain_effectiveness : gg::rejected_estimate(), !facts.terrain_allowed);
            if (!facts.terrain_allowed) continue;
            Real contrast_weight{};
            if (!facts.hero && plan.ignore_target) contrast_weight = stream(player).uniform(Real{}, to_single(info->combat_power / real(500)));
            else if (!facts.hero) {
                const auto* unit = best ? host_->view->find(best->object) : nullptr;
                const Real power = to_single(info->combat_power * (unit ? unit->health : real(1)));
                for (std::size_t row = 0; row < potential.current_contrast.size(); ++row) {
                    if (input->contrast[row].channel != facts.channel
                        || input->contrast[row].escort != plan.taskforces[slots[allocations[i]].force].escort) continue;
                    const Real deficit = potential.current_contrast[row].force;
                    if (!(Real{} < deficit) || (!best && !(potential.target_contrast[row].force * potential.threshold < deficit))) continue;
                    const auto effectiveness = contrast_factor(*info, input->contrast[row].category);
                    const auto applied = to_single(power * effectiveness);
                    if (!(Real{} < effectiveness) || !(Real{} < applied)) continue;
                    if (!best && input->population_check && Real{} < facts.population
                        && potential.target_contrast[row].force * potential.threshold < deficit - applied * real(truncate(remaining / facts.population))) continue;
                    const auto ratio = to_single(std::max(to_single(deficit - applied), Real{}) / std::max(deficit, applied));
                    const auto weight = to_single(std::max(to_single(real(1) - to_single(ratio * ratio)), Real{}) * effectiveness);
                    if (contrast_weight < weight) { contrast_weight = weight; contrast_rows[i] = row; }
                }
                // WGA-251: each garrison contributes its own positive best
                // contrast score; the freestore garrison bonus is independent.
                for (std::size_t garrison = 0; garrison < facts.garrisons.size(); ++garrison) {
                    const auto& spawned = facts.garrisons[garrison];
                    const auto* spawned_type = host_->type(spawned.type);
                    if (!spawned_type) throw std::runtime_error("qualified galactic garrison type absent");
                    Real preference{};
                    garrison_rows[i][garrison] = input->contrast.size();
                    for (std::size_t row = 0; row < input->contrast.size(); ++row) {
                        if (input->contrast[row].escort || (spawned.all_main_domains
                            ? !input->contrast[row].category : input->contrast[row].channel != spawned.channel)) continue;
                        const auto deficit = spawned.all_main_domains ? to_single(potential.current_contrast[row].force) : potential.current_contrast[row].force;
                        const auto limit = spawned.all_main_domains ? to_single(to_single(potential.target_contrast[row].force) * potential.threshold)
                            : potential.target_contrast[row].force * potential.threshold;
                        if (!(Real{} < deficit) || (!best && !(limit < deficit))) continue;
                        const auto average = contrast_factor(*spawned_type, input->contrast[row].category);
                        const auto effectiveness = spawned.all_main_domains
                            ? to_single(to_single(average) * to_single(spawned.terrain_effectiveness)) : average;
                        const auto applied = to_single(spawned.power * effectiveness);
                        if (!(Real{} < effectiveness) || !(Real{} < applied)) continue;
                        if (spawned.all_main_domains && !best && input->population_check && Real{} < spawned.population
                            && limit < to_single(deficit - to_single(applied * real(truncate(remaining / spawned.population))))) continue;
                        const auto ratio = to_single(std::max(to_single(deficit - applied), Real{}) / std::max(deficit, applied));
                        const auto weight = to_single(std::max(to_single(real(1) - to_single(ratio * ratio)), Real{}) * effectiveness);
                        if (preference < weight) { preference = weight; garrison_rows[i][garrison] = row; }
                    }
                    contrast_weight = to_single(contrast_weight + preference);
                }
                if (!(Real{} < contrast_weight)) {
                    const auto& allocated = slots[allocations[i]];
                    const auto& force = plan.taskforces[allocated.force];
                    const auto& team = force.teams[allocated.team];
                    const auto eligible_unclassified = breakdown_met(force.escort, facts.channel)
                        && std::any_of(input->contrast.begin(), input->contrast.end(), [&](const auto& row) {
                            const auto index = static_cast<std::size_t>(&row - input->contrast.data());
                            return !row.category && row.channel == facts.channel && row.escort == force.escort && Real{} < potential.current_contrast[index].force;
                        });
                    const auto missing_category = std::any_of(plan.required_categories.begin(), plan.required_categories.end(), [&](auto category_bits) {
                        return (info->category_bits & category_bits) && std::none_of(force_categories.begin(), force_categories.end(), [&](auto present) { return (present & category_bits) != 0; });
                    });
                    if (!(current_cost < input->minimum_cost) && !eligible_unclassified && !missing_category
                        && !(!team.percentage_based && allocated.count < team.min_count)
                        && !(force_size[allocated.force] < force.minimum_size) && !(force_power[allocated.force] < force.minimum_force))
                        contrast_weight = gg::rejected_estimate();
                }
            }
            stage(gg::SelectionStage::contrast, contrast_weight);
            stage(gg::SelectionStage::survival, facts.survival_type.empty() ? facts.survival : learning::survival(player, facts.survival_type));
        }
        const auto preferences = gg::production_preferences(excess);
        for (std::size_t i = 0; i < preferences.size(); ++i)
            candidates[production_rows[i]].stages[static_cast<std::size_t>(gg::SelectionStage::production)].weight = preferences[i];
        // These short folds belong to the ordered decision pass. Bulk producer
        // estimates above remain independent worker jobs over copied inputs.
        const auto scores = required(gg::score_candidates(candidates, static_cast<std::uint32_t>(potential.units.size()), executor(nullptr)));
        for (std::size_t i = 0; i < erased.size(); ++i) erased[i] = erased[i] || scores.candidates[i].removed;
        if (!scores.best) break;
        const auto chosen = *scores.best; const auto type = proposals[chosen]; const auto& facts = input->types.at(type);
        if (allocations[chosen] == slots.size()) break;
        auto& slot = slots[allocations[chosen]]; ++slot.count; slot.cost = to_single(slot.cost + facts.cost);
        current_cost = to_single(current_cost + facts.cost);
        ++selected_counts[type];
        if (!objects[chosen] && !plan.magic) production_requests.push_back(production_request(type));
        potential.units.push_back(type); potential.freestore.push_back(objects[chosen]);
        potential.sources.push_back(objects[chosen] ? 0 : 2); potential.producers.push_back(factories[chosen]); potential.pool_tokens.push_back(0);
        potential.taskforce_of_unit.push_back(slot.force); potential.team_of_unit.push_back(slot.team);
        ++force_size[slot.force]; const auto* info = host_->type(type);
        const auto* unit = objects[chosen] ? host_->view->find(objects[chosen]) : nullptr;
        const auto power = to_single(info->combat_power * (unit ? unit->health : real(1)));
        force_power[slot.force] = to_single(force_power[slot.force] + power); force_categories[slot.force] |= info->category_bits;
        if (objects[chosen]) used.insert(objects[chosen]);
        else if (!plan.magic) { potential.cost = to_single(potential.cost + facts.cost); remaining = to_single(remaining - facts.population); }
        if (!objects[chosen] && facts.structure) {
            ++selected_structures[type];
            --structure_slots[static_cast<std::size_t>(facts.channel)];
        }
        const auto production = required(gg::estimate_production(input->producers, player.player,
            production_requests, input->reachable, executor(perception_executor_)));
        if (production.feasible) {
            current_seconds = production.seconds;
            if (Real{} < current_seconds) current_budget = resources(current_seconds);
        }
        if (contrast_rows[chosen] < input->contrast.size()) {
            const auto row = contrast_rows[chosen];
            const auto applied = input->contrast[row].category == 0 ? power
                : input->unscaled_domain_totals
                    ? to_single(power * to_single(to_single(contrast_factor(*info, input->contrast[row].category))
                        * (input->contrast[row].channel == gc::ProductionDomain::ground ? to_single(facts.terrain_effectiveness) : real(1))))
                    : to_single(to_single(power * contrast_factor(*info, input->contrast[row].category))
                        * (facts.channel == gc::ProductionDomain::ground ? facts.terrain_effectiveness : real(1)));
            const auto residual = potential.current_contrast[row].force - applied;
            potential.current_contrast[row].force = input->unscaled_domain_totals ? residual : to_single(residual);
            if (input->contrast[row].category != 0) for (std::size_t total = 0; total < input->contrast.size(); ++total)
                if (!input->contrast[total].category && input->contrast[total].channel == facts.channel
                    && input->contrast[total].escort == input->contrast[row].escort)
                    potential.current_contrast[total].force = input->unscaled_domain_totals
                        ? potential.current_contrast[total].force - power : to_single(potential.current_contrast[total].force - power);
        } else if (input->unscaled_domain_totals) {
            for (std::size_t row = 0; row < input->contrast.size(); ++row)
                if (!input->contrast[row].category && !input->contrast[row].escort
                    && input->contrast[row].channel == facts.contrast_fallback_channel.value_or(facts.channel)) {
                    potential.current_contrast[row].force -= power; break;
                }
        }
        for (std::size_t garrison = 0; garrison < facts.garrisons.size(); ++garrison) {
            const auto& spawned = facts.garrisons[garrison]; const auto* spawned_type = host_->type(spawned.type);
            const auto row = garrison_rows[chosen][garrison];
            if (row >= input->contrast.size()) {
                if (spawned.all_main_domains) for (std::size_t total = 0; total < input->contrast.size(); ++total)
                    if (!input->contrast[total].category && !input->contrast[total].escort && input->contrast[total].channel == spawned.channel) {
                        potential.current_contrast[total].force -= spawned.power; break;
                    }
                continue;
            }
            const auto applied = !input->contrast[row].category ? spawned.power
                : spawned.all_main_domains
                    ? to_single(spawned.power * to_single(to_single(contrast_factor(*spawned_type, input->contrast[row].category))
                        * (input->contrast[row].channel == gc::ProductionDomain::ground ? to_single(spawned.terrain_effectiveness) : real(1))))
                    : to_single(to_single(spawned.power * contrast_factor(*spawned_type, input->contrast[row].category))
                        * (spawned.channel == gc::ProductionDomain::ground ? spawned.terrain_effectiveness : real(1)));
            const auto residual = potential.current_contrast[row].force - applied;
            potential.current_contrast[row].force = spawned.all_main_domains ? residual : to_single(residual);
            if (input->contrast[row].category) for (std::size_t total = 0; total < input->contrast.size(); ++total)
                if (!input->contrast[total].category && !input->contrast[total].escort
                    && input->contrast[total].channel == (spawned.all_main_domains ? input->contrast[row].channel : spawned.channel))
                    potential.current_contrast[total].force = spawned.all_main_domains
                        ? potential.current_contrast[total].force - spawned.power : to_single(potential.current_contrast[total].force - spawned.power);
        }
    }
    potential.valid = !needs() && (plan.ignore_target || !deficits())
        && (!(Real{} < input->maximum_cost) || !(input->maximum_cost < current_cost))
        && (!potential.units.empty() || std::any_of(plan.taskforces.begin(), plan.taskforces.end(), [](const auto& force) { return force.required; }));
    return potential.valid;
}

void Engine::service_galactic_execution(PlayerAi& player) {
    ExecutionFacts facts(this, player.player);
    auto taskforces_ = all_taskforces();
    auto blocks_ = all_blocks();
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {4, 13, 14, 15, 16, 17, 21, 22, 23, 32});
    mark_native_state_changed();
    std::map<std::uint64_t, std::vector<std::size_t>> touched;
    std::size_t task_index{};
    for (auto& task : player.tasks) {
        touched[task.block].push_back(task_index++);
        ++facts.work.indexed_tasks;
        if (task.finished) continue;
        // Native purchases and other task kinds may publish changed production
        // facts. Their ordered admission must precede the next preparation.
        if (task.source != 0) facts.stages.clear();
        auto force = taskforces_.find(task.taskforce);
        const auto* running = force == taskforces_.end() ? nullptr : this->plan(force->second.plan);
        if (!running || !galactic_goal(player.player, running->goal)) {
            // Assembly has a registered request/phase to terminate as well.
            if (task.galactic_assembly == BuildTask::GalacticAssembly::none) task.finished = task.failed = true;
            continue;
        }
        if (task.source == 2) {
            if (task.completion == 0) {
                galactic_ai::ActionRequest request;
                request.kind = plans_[running->definition].magic ? galactic_ai::ActionKind::magic : galactic_ai::ActionKind::produce;
                request.player = player.player; request.plan = running->id;
                request.taskforce = task.taskforce; request.block = task.block;
                request.type = task.type; request.target = task.producer;
                if (!request.target) request.target = running->target_object;
                if (!request.target || !host_->galactic_execution->supports(request.kind)) { task.finished = task.failed = true; continue; }
                const auto admitted = required(host_->galactic_execution->admit(request));
                if (!admitted.admitted || !admitted.request) { task.finished = task.failed = true; continue; }
                task.completion = admitted.request; task.issued = static_cast<std::uint64_t>(frame_);
                task.state_entered = task.issued;
                mark_galactic_tasks_changed();
                if (auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get()); actions && request.kind == galactic_ai::ActionKind::produce) {
                    const auto purchase = actions->purchases().find(task.completion);
                    if (purchase == actions->purchases().end()) throw std::runtime_error("accepted task has no producer association");
                    task.associated_producer = purchase->second.producer;
                }
                task.acknowledged = true; // admission, never a completion signal
                record(running, player.player, "build_admitted", std::to_string(admitted.request));
            }
            if (plans_[running->definition].magic) {
                task.object = task.completion; task.finished = true;
            } else {
                const auto status = required(galactic_build_observer_(player.player, task.completion));
                if (status.state == GalacticBuildStatus::State::waiting) continue;
                task.finished = true;
                task.state_entered = static_cast<std::uint64_t>(frame_);
                mark_galactic_tasks_changed();
                task.failed = status.state == GalacticBuildStatus::State::failed;
                task.object = status.object;
                if (!task.failed && task.object) if (const auto* actions = dynamic_cast<const galactic_ai::NativeActions*>(host_->galactic_execution.get())) {
                    task.object = required(actions->build_actor(player.player, task.completion));
                    if (const auto fleet = actions->containing_fleet(player.player, task.object)) force->second.galactic_fleet = *fleet;
                }
            }
        } else if (task.source == 0) {
            if (task.galactic_assembly != BuildTask::GalacticAssembly::none) continue;
            const auto* goal = find_goal(player, running->goal);
            const auto input = goal ? galactic_selection(player, *goal, plans_[running->definition]) : nullptr;
            if (!input) throw std::runtime_error("galactic assignment facts unavailable");
            sim::EntityId fallback{};
            bool waiting{};
            for (const auto& row : input->freestore) {
                const auto reservation = player.reserved.find(row.object);
                if (row.in_transit && row.type == task.type && !player.assigned.contains(row.object)
                    && (reservation == player.reserved.end() || reservation->second == running->goal)) {
                    waiting |= reservation != player.reserved.end() && reservation->second == running->goal;
                    continue;
                }
                const auto* unit = host_->view->find(row.object);
                if (!unit && row.type == task.type && !player.assigned.contains(row.object)
                    && reservation != player.reserved.end() && reservation->second == running->goal) {
                    waiting = true; continue; // WGA-164/307: retained identity awaits its live view.
                }
                if (!unit || unit->owner != player.player || row.type != task.type || player.assigned.contains(row.object)) continue; // WGA-312
                const auto reserved = player.reserved.find(row.object);
                if (reserved != player.reserved.end() && reserved->second == running->goal) { task.object = row.object; break; }
                if (!fallback && reserved == player.reserved.end()) fallback = row.object;
            }
            // WGA-164/305/307: a refused unreserved association cannot exhaust
            // this task while its reserved actor awaits transit or view publication.
            if (!task.object && waiting) continue;
            if (!task.object) task.object = fallback;
            auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
            if (task.object && actions && actions->qualified_freestore_available()) {
                const auto assigned = actions->qualified_freestore().assign(player.player, task.object, goal && goal->potential ? goal->potential->id : 0);
                if (assigned == galactic_ai::FreestoreAssignment::waiting) { task.object = 0; continue; }
                if (assigned == galactic_ai::FreestoreAssignment::failed) task.object = 0;
            }
            task.finished = true; task.failed = task.object == 0;
            if (task.object && task.staging) {
                if (!actions) throw std::runtime_error("staged freestore assignment requires its native assembly owner");
                if (!task.synchronized_staging)
                    task.failed = !required(actions->prepare_assembly_actor(player.player, task.object)).admitted;
                task.finished = task.failed;
                task.galactic_assembly = task.failed ? BuildTask::GalacticAssembly::failed
                    : task.synchronized_staging ? BuildTask::GalacticAssembly::waiting : BuildTask::GalacticAssembly::assigned;
                // Reserve the association while synchronized actors await departure.
                if (!task.failed) player.assigned[task.object] = task.taskforce;
                task.state_entered = static_cast<std::uint64_t>(frame_);
                mark_native_state_changed();
                mark_galactic_tasks_changed();
            }
        } else { task.finished = task.failed = true; }
        if ((task.finished || task.galactic_assembly == BuildTask::GalacticAssembly::assigned) && !task.failed && task.object) {
            if (player.assigned.contains(task.object) && player.assigned.at(task.object) != task.taskforce)
                throw std::runtime_error("galactic completion object belongs to another taskforce");
            if (std::find(force->second.units.begin(), force->second.units.end(), task.object) == force->second.units.end()) force->second.units.push_back(task.object);
            force->second.had_units = true;
            player.assigned[task.object] = task.taskforce;
            task.joined = true;
        }
    }
    service_galactic_assembly(player);
    for (const auto& [id, tasks] : touched) {
        const auto block = blocks_.find(id); if (block == blocks_.end()) continue;
        bool done = true, failed = false;
        for (const auto index : tasks) {
            const auto& task = player.tasks[index];
            ++facts.work.reduced_tasks;
            done = done && task.finished; failed = failed || task.failed;
        }
        if (done) {
            const bool newly_failed = failed && !block->second.finished;
            block->second.finished = true; block->second.result = !failed;
            if (newly_failed) for (const auto index : tasks) {
                const auto& task = player.tasks[index];
                ++facts.work.failure_tasks;
                if (task.galactic_assembly != BuildTask::GalacticAssembly::failed) continue;
                const auto* force = taskforce(task.taskforce);
                const auto* running = force ? plan(force->plan) : nullptr;
                if (running) emit_call(*running, *force, "Production_Failed", {Value{authoritative::Handle{handle_type, task.type}}});
                break;
            }
        }
    }
}

core::Result<std::pair<Real, bool>> Engine::galactic_freestore_selection_facts(
    tactical::PlayerId id, gc::PlanetId target_id, sim::EntityId object, const PlanDef& definition) const {
    return galactic_freestore_selection_batch(id, target_id, definition)(object);
}

Engine::GalacticFreestoreActorFacts Engine::galactic_freestore_selection_batch(
    tactical::PlayerId id, gc::PlanetId target_id, const PlanDef& definition) const {
    using Result = core::Result<std::pair<Real, bool>>;
    // Everything here depends on (player, target, plan, completed publication), never on the actor. Each piece is
    // resolved lazily at the position the per-actor read first needed it, so the first refusal and its order are
    // exactly those of an independent read of that actor.
    struct Batch {
        const galactic_ai::NativeActions* actions{};
        const GalacticWorld* world{};
        const PlayerAi* player{};
        std::shared_ptr<galactic_ai::QualifiedFreestore> store;
        bool snapshot_read{};
        std::shared_ptr<const gc::GalacticSnapshot> snapshot;
        bool goals_scanned{}, ambiguous{};
        const GoalType* goal_type{};
        bool stage_ready{};
        std::optional<core::Diagnostic> stage_failure;
        ExecutionFacts* memo{};
        std::shared_ptr<const galactic_ai::WorldInput> input;
        std::shared_ptr<const std::vector<gc::PlanetId>> stages;
        std::vector<const galactic_ai::TargetInput*> positions; // one per stage; null when the stage has no target row
        bool prefix{};
        Real tolerance{};
    };
    auto batch = std::make_shared<Batch>();
    batch->actions = dynamic_cast<const galactic_ai::NativeActions*>(host_->galactic_execution.get());
    batch->world = dynamic_cast<const GalacticWorld*>(host_->ai_world.get());
    batch->player = player_ai(id);
    return [this, batch, id, target_id, &definition](sim::EntityId object) -> Result {
        const auto refuse = [](std::string message) { return Result::failure(detail::make_error(authoritative::codes::missing_api, std::move(message))); };
        auto& b = *batch;
        const auto* world = b.world;
        if (!b.actions || !world || !b.player || !b.player->galactic) return refuse("BTP-U04: native stage/garrison owners unavailable");
        if (!b.store) b.store = b.actions->freestore_owner().lock();
        const auto* facts = b.store ? b.store->facts(object) : nullptr;
        if (!facts || !facts->qualified || facts->object.owner != id || !galactic_stage_garrison_)
            return refuse("BTP-U04: retained per-object garrison flag unavailable");
        if (!b.snapshot_read) { b.snapshot = b.actions->snapshot(); b.snapshot_read = true; }
        const auto& snapshot = b.snapshot; if (!snapshot) return refuse("BTP-U04: completed stage world unavailable");
        const auto garrison = galactic_stage_garrison_(id, *snapshot, object);
        if (!garrison) return Result::failure(garrison.error());
        if (!target_id) return Result::success({Real{}, garrison.value()}); // BTP-S01: global proposal has zero stage cost
        if (!galactic_stage_combat_skip_) return refuse("BTP-U04: retained actor combat-skip predicate unavailable");
        const auto skip = galactic_stage_combat_skip_(id, *snapshot, object);
        if (!skip) return Result::failure(skip.error());
        if (skip.value()) return Result::success({Real{}, garrison.value()});
        if (!b.goals_scanned) {
            b.goals_scanned = true;
            for (const auto* rows : {&b.player->active, &b.player->proposed}) for (const auto& goal : *rows) {
                const auto* destination = target(goal.target);
                const auto* running = plan(goal.plan);
                const auto index = goal.potential ? std::optional{goal.potential->plan}
                    : running ? std::optional{running->definition} : std::nullopt;
                if (!destination || destination->object != target_id || !index || plans_.at(*index).name != definition.name) continue;
                const auto* candidate = b.player->functions.at(goal.function).goal;
                if (b.goal_type && b.goal_type->reachability != candidate->reachability) { b.ambiguous = true; break; }
                b.goal_type = candidate;
            }
        }
        if (b.ambiguous) return refuse("BTP-U04: ambiguous authored staging policy");
        if (!b.goal_type || !galactic_stage_catalog_) return refuse("BTP-U04: retained goal/stage catalog unavailable");
        const auto* goal_type = b.goal_type;
        if (!b.stage_ready) {
            b.stage_ready = true;
            const auto constraints = galactic_stage_catalog_->constraints.find(definition.name);
            b.input = world->copy_input();
            if (constraints == galactic_stage_catalog_->constraints.end() || !b.input || !snapshot || !host_->view) {
                b.stage_failure = detail::make_error(authoritative::codes::missing_api, "BTP-U04: authored stage constraints/completed world unavailable");
            } else {
                const auto& input = b.input;
                auto* memo = execution_facts;
                if (memo && (memo->engine != this || memo->player != id)) memo = nullptr;
                b.memo = memo;
                if (memo) memo->publication(snapshot, input);
                const StageKey key{&definition, target_id, goal_type->reachability};
                if (memo) {
                    const auto cached = memo->stages.find(key);
                    if (cached != memo->stages.end()) { b.stages = cached->second; ++memo->work.stage_reuses; }
                }
                if (!b.stages) {
                    if (memo) ++memo->work.stage_preparations;
                    std::vector<gc::PlanetId> sites;
                    const AllowedProductionSites allowed_sites(input, id);
                    std::size_t site_index{};
                    for (const auto& site : input->targets) {
                        if (memo) ++memo->work.production_sites;
                        if (const auto allowed = allowed_sites.at(site_index++); !allowed || !allowed.value()) continue;
                        const auto reach = world->goal_reachable(id, target_id, site.planet, goal_type->reachability);
                        if (!reach) { b.stage_failure = detail::make_error(authoritative::codes::missing_api, "BTP-U04: stage production-site relation unavailable"); break; }
                        if (*reach) sites.push_back(site.planet);
                    }
                    if (!b.stage_failure) {
                        auto computed = galactic_force_stages(definition, constraints->second.forces, target_id, goal_type->reachability, sites);
                        if (!computed) b.stage_failure = computed.error();
                        else {
                            b.stages = std::make_shared<const std::vector<gc::PlanetId>>(std::move(computed).value());
                            if (memo) memo->stages.emplace(key, b.stages);
                        }
                    }
                }
                if (!b.stage_failure) {
                    b.positions.reserve(b.stages->size());
                    for (const auto stage : *b.stages) {
                        const auto position = std::find_if(input->targets.begin(), input->targets.end(), [&](const auto& row) { return row.planet == stage; });
                        b.positions.push_back(position == input->targets.end() ? nullptr : &*position);
                    }
                    b.prefix = upper_case(goal_type->reachability) == "SINGLE_HOP_DISCONNECTED";
                    b.tolerance = parse_real("0.0001").value();
                }
            }
        } else if (b.memo && !b.stage_failure) ++b.memo->work.stage_reuses; // the same retained stage list, read for this actor
        if (b.stage_failure) return Result::failure(*b.stage_failure);
        const auto& stages = b.stages;
        const auto* actor = host_->view->find(object);
        if (!actor || actor->owner != id) return refuse("BTP-U04: live stage actor unavailable");
        auto source = facts->object.planet;
        if (!source && facts->parent && facts->enroute) {
            const auto fleet = std::lower_bound(snapshot->fleets().begin(), snapshot->fleets().end(), facts->parent,
                [](const auto& row, auto key) { return row.id < key; });
            if (fleet != snapshot->fleets().end() && fleet->id == facts->parent && fleet->motion.transit) {
                const auto& transit = *fleet->motion.transit;
                if (transit.hop + 1 < transit.path.size()) source = transit.path[transit.hop + 1];
            }
        }
        if (!source) return Result::success({gg::rejected_estimate(), garrison.value()});
        if (!galactic_stage_usability_) return refuse("BTP-U04: source planet AI-usability unavailable");
        const auto usable = galactic_stage_usability_(id, *snapshot, source);
        if (!usable) return Result::failure(usable.error());
        if (!usable.value()) return Result::success({gg::rejected_estimate(), garrison.value()});
        if (!world->execution_available()) return refuse("BTP-U04: stage reach helper/pathfinder unavailable");
        Real cost{}; bool reachable{};
        for (std::size_t index = 0; index < stages->size(); ++index) {
            const auto stage = (*stages)[index]; const auto* position = b.positions[index];
            if (!position) return refuse("BTP-U04: stage target position unavailable");
            Real helper = gg::rejected_estimate();
            if (source == stage) helper = {};
            else if (const auto* route = world->execution_route(id, source, stage)) {
                if (!route->empty && (b.prefix ? route->prefix_threat : route->threat) <= b.tolerance) helper = route->distance;
            }
            if (helper != gg::rejected_estimate()) { cost = to_single(cost + to_single(helper * helper)); reachable = true; }
            const auto x = to_single(numeric::from_fixed(actor->position.x) - numeric::from_fixed(position->position.x));
            const auto y = to_single(numeric::from_fixed(actor->position.y) - numeric::from_fixed(position->position.y));
            cost = to_single(cost + to_single(to_single(x * x) + to_single(y * y)));
        }
        if (!reachable || stages->empty()) return Result::success({gg::rejected_estimate(), garrison.value()});
        return Result::success({to_single(cost / real(stages->size())), garrison.value()}); // BTP-S02/03
    };
}

core::Result<Engine::GalacticBuildPublication> Engine::galactic_build_budgets(
    const galactic_ai::NativeActions& actions, const GalacticFunding& funding) const {
    using Result = core::Result<GalacticBuildPublication>;
    const auto funding_revision = funding.state_revision(); const auto actions_revision = actions.build_state_revision();
    // S9a: inside a player turn the revision stacks the committed counter and the staged changes of every
    // turn so far (the pass is still sequential), and the memo is the turn's own.
    auto* turn = staged();
    const std::uint64_t task_revision = galactic_build_tasks_revision();
    const std::array key{task_revision, funding_revision, actions_revision};
    auto& memo = turn ? turn->build_cache : galactic_build_cache_;
    auto& memo_key = turn ? turn->build_cache_key : galactic_build_cache_key_;
    if (memo && memo_key == key) return Result::success(memo);
    std::vector<GalacticBudgetBuild> rows;
    std::set<sim::EntityId> associated;
    for (const auto& player : players_) {
        if (!player.galactic) return Result::failure(detail::make_error(authoritative::codes::invalid_request, "build publication requires a galactic owner"));
        const auto ledger = funding.state().find(player.player);
        for (const auto& task : player.tasks) {
            if (task.source != 2 || !task.completion || !task.associated_producer) continue;
            const auto purchase = actions.purchases().find(task.completion);
            const auto* force = taskforce(task.taskforce); const auto* owner = force ? plan(force->plan) : nullptr;
            if (purchase == actions.purchases().end() || !owner || owner->player != player.player
                || purchase->second.player != owner->player || purchase->second.goal != owner->goal
                || purchase->second.requested_type != task.type || purchase->second.producer != task.associated_producer)
                return Result::failure(detail::make_error(authoritative::codes::invalid_request, "build publication task/plan/producer association disagrees"));
            const auto head = actions.build_head(player.player, task.completion);
            if (!head) return Result::failure(head.error());
            if (!head.value().entry || head.value().entry->build_id != task.completion) continue;
            if (!associated.insert(task.completion).second) return Result::failure(detail::make_error(authoritative::codes::invalid_request, "build publication duplicates an actual head"));
            GalacticBudgetBuild row; row.planet = task.associated_producer; row.type = task.type;
            row.builder = player.player; row.channel = head.value().channel; row.build = task.completion;
            row.status = task.finished ? (task.salvage ? GalacticBudgetBuild::Status::salvage : GalacticBudgetBuild::Status::finished)
                : GalacticBudgetBuild::Status::goal;
            if (!task.finished) {
                if (ledger == funding.state().end() || !ledger->second.goals.contains(owner->goal))
                    return Result::failure(detail::make_error(authoritative::codes::invalid_request, "unfinished build publication requires its live goal budget"));
                const auto& goal = ledger->second.goals.at(owner->goal);
                const auto held = math::add(goal.capital, goal.accumulated);
                if (!held) return Result::failure(held.error());
                row.cost_to_completion = numeric::from_fixed(goal.cost);
                row.spendable = numeric::from_fixed(std::min(held.value(), goal.cost));
                row.budget_seconds = numeric::from_fixed(std::max(goal.remaining, math::Fixed{}));
            }
            rows.push_back(row);
        }
    }
    memo = std::make_shared<const std::vector<GalacticBudgetBuild>>(std::move(rows));
    memo_key = key;
    return Result::success(memo);
}

core::Result<math::Fixed> Engine::galactic_build_remaining_cost(tactical::PlayerId id, const BuildTask& task) const {
    using Result = core::Result<math::Fixed>;
    const auto* force = taskforce(task.taskforce); const auto* owner = force ? plan(force->plan) : nullptr;
    if (!owner || owner->player != id) return Result::failure(detail::make_error(authoritative::codes::invalid_request, "remaining cost requires its owning TaskForce/plan/player"));
    if (task.object) return Result::success({});
    const auto* actions = dynamic_cast<const galactic_ai::NativeActions*>(host_->galactic_execution.get());
    if (!actions) return Result::failure(detail::make_error(authoritative::codes::missing_api, "remaining cost requires the production owner"));
    if (!task.associated_producer) return actions->generic_build_cost(task.type);
    return actions->build_remaining_cost(id, task.completion);
}

void Engine::salvage_galactic_goal(PlayerAi& player, std::uint64_t goal_id) {
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {4, 13, 14, 15, 16, 17});
    auto* funding = dynamic_cast<GalacticFunding*>(player.galactic_funding.get());
    auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
    if (!funding) throw std::runtime_error("goal salvage requires the retained funding owner");
    const auto ledger = funding->state().find(player.player);
    if (ledger == funding->state().end() || !ledger->second.goals.contains(goal_id)) return;
    const auto& goal = ledger->second.goals.at(goal_id);
    const auto pool = required(math::add(goal.capital, goal.accumulated));
    std::vector<BuildTask*> candidates;
    for (auto& task : player.tasks) {
        const auto* force = taskforce(task.taskforce); const auto* owner = force ? plan(force->plan) : nullptr;
        if (!owner || owner->goal != goal_id || task.source != 2 || !task.completion || !task.associated_producer) continue;
        // The production owner is only needed once a task has a purchase to salvage.
        if (!actions) throw std::runtime_error("goal salvage requires the retained production owner");
        const auto purchase = actions->purchases().find(task.completion);
        if (purchase == actions->purchases().end()) throw std::runtime_error("salvage task association is unavailable");
        const auto head = required(actions->build_head(player.player, task.completion));
        if (!head.entry || !head.entry->pay_as_you_go) continue;
        const auto cost = required(actions->build_remaining_cost(player.player, task.completion));
        if (cost.raw() > 0 && cost <= pool) candidates.push_back(&task);
    }
    // Equal age/power order is a deterministic project policy, not a retail promise.
    std::stable_sort(candidates.begin(), candidates.end(), [&](const auto* a, const auto* b) {
        if (a->state_entered != b->state_entered) return a->state_entered < b->state_entered;
        const auto* at = host_->type(a->type); const auto* bt = host_->type(b->type);
        if (!at || !bt) throw std::runtime_error("salvage generic combat power is unavailable");
        return at->combat_power > bt->combat_power;
    });
    auto remaining = pool; math::Fixed selected{};
    for (auto* task : candidates) {
        const auto cost = required(actions->build_remaining_cost(player.player, task->completion));
        if (cost.raw() <= 0 || remaining < cost) continue;
        remaining = required(math::subtract(remaining, cost)); selected = required(math::add(selected, cost));
        task->salvage = true;
    }
    required(funding->remove_goal_salvage(player.player, goal_id, selected));
    mark_galactic_tasks_changed();
    mark_native_state_changed();
}
} // namespace eawr::script::foc::ai
