#include "ai_engine_internal.hpp"
#include "galactic_ai_funding.hpp"
#include "galactic_ai_actions.hpp"
#include "ai_learning.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_selection_content.hpp"

#include <algorithm>
#include <stdexcept>

namespace eawr::script::foc::ai {
using namespace engine_internal;
namespace gc = sim::galactic;
namespace gg = galactic_goals;
namespace {
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
        requests.push_back({potential.units[i], type.channel, type.structure,
            targeted && type.instance_allowed && type.target_build_seconds.has_value(),
            targeted && type.structure ? type.target_build_seconds.value_or(Real{}) : type.build_seconds});
        seconds = to_single(seconds + to_single(type.build_seconds * multiplier));
    }
    if (linear) return {true, seconds, {}, 0};
    return required(gg::estimate_production(input.producers, player, requests, input.reachable, workers));
}
}

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
    if (!goal.plan && player.galactic_funding) {
        const auto ledger = player.galactic_funding->ledger(player.player);
        if (ledger && ledger->categories.contains(out.category)) {
            const auto& category_budget = ledger->categories.at(out.category);
            const auto seconds = out.time_limit ? std::min(out.estimated_seconds, *out.time_limit) : out.estimated_seconds;
            out.proposal_budget = std::max(required(math::add(category_budget.available_cash,
                required(math::multiply(category_budget.available_income, seconds)))), math::Fixed{});
        }
    }
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
    mark_native_state_changed();
    const auto& plan = plans_.at(potential.plan);
    const auto input = galactic_selection(player, goal, plan);
    potential.valid = false;
    if (!input || !input->complete || !input->reachable || plan.taskforces.empty()) return false;
    potential.units.clear(); potential.freestore.clear(); potential.sources.clear();
    potential.producers.clear(); potential.pool_tokens.clear(); potential.taskforce_of_unit.clear(); potential.team_of_unit.clear();
    potential.cost = {};
    potential.target_contrast.clear();
    const auto history = player.plan_outcomes.find({potential.plan, goal.target});
    const Real adjustment = to_single(real(recent_failures(history == player.plan_outcomes.end() ? nullptr : &history->second)) * plan.per_failure_contrast_adjust);
    const auto maximum = to_single(plan.max_contrast + adjustment);
    const auto minimum = to_single(plan.min_contrast + adjustment);
    potential.threshold = goal.target && !plan.ignore_target && Real{} < maximum ? to_single(real(1) - minimum / maximum) : Real{};
    if (goal.target && !plan.ignore_target) for (const auto& row : input->contrast) potential.target_contrast.push_back({row.category,
        to_single(row.force * to_single(maximum * player.difficulty.galactic_contrast_multiplier))});
    potential.current_contrast = potential.target_contrast;
    struct Slot { std::size_t force{}, team{}; std::int32_t count{}; Real cost{}; };
    std::vector<Slot> slots;
    std::vector<gc::TypeId> proposals;
    for (std::size_t f = 0; f < plan.taskforces.size(); ++f) for (std::size_t t = 0; t < plan.taskforces[f].teams.size(); ++t) {
        slots.push_back({f, t, 0});
        for (auto type : plan.taskforces[f].teams[t].types)
            if (input->types.contains(type) && std::find(proposals.begin(), proposals.end(), type) == proposals.end()) proposals.push_back(type);
    }
    std::vector<std::vector<std::size_t>> teams(proposals.size());
    std::vector<std::size_t> starts(proposals.size());
    for (std::size_t i = 0; i < proposals.size(); ++i) {
        for (std::size_t s = 0; s < slots.size(); ++s) {
            const auto& types = plan.taskforces[slots[s].force].teams[slots[s].team].types;
            if (std::find(types.begin(), types.end(), proposals[i]) != types.end()) teams[i].push_back(s);
        }
        if (teams[i].size() > 1) starts[i] = static_cast<std::size_t>(sync_.range(0, static_cast<std::int32_t>(teams[i].size()) - 1));
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
    for (const auto& row : input->freestore) max_path = std::max(max_path, row.path_cost);
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
            candidate.selected_count = static_cast<std::uint32_t>(std::count(potential.units.begin(), potential.units.end(), proposals[i]));
            auto stage = [&](gg::SelectionStage which, Real weight, bool permanent = false) {
                candidate.stages[static_cast<std::size_t>(which)] = {weight, permanent};
            };
            if (erased[i]) { stage(gg::SelectionStage::freestore, gg::rejected_estimate(), true); continue; }
            const auto& facts = input->types.at(proposals[i]);
            const auto* info = host_->type(proposals[i]);
            if (!info) { stage(gg::SelectionStage::freestore, gg::rejected_estimate(), true); continue; }
            const GalacticSelectionObject* best{};
            if (plan.allow_free_store) for (const auto& object : input->freestore) {
                const auto* unit = host_->view->find(object.object);
                if (!unit || unit->type != proposals[i] || unit->owner != player.player || used.contains(object.object) || player.assigned.contains(object.object)) continue;
                const auto held = player.reserved.find(object.object);
                if (held != player.reserved.end() && held->second != goal.id) continue;
                if (!best || object.path_cost < best->path_cost) best = &object;
            }
            if (best) {
                objects[i] = best->object;
                stage(gg::SelectionStage::freestore, gg::freestore_preference(best->path_cost, max_path, best->garrison, player.reserved.contains(best->object)));
            }
            // WGA-60: no tactical reserve pool. Ordinary production uses C's
            // feasibility resources; activation uses available resources later.
            if (!best && !plan.magic) {
                if (facts.structure && goal.target && (!facts.target_cost || !facts.target_build_seconds)) {
                    stage(gg::SelectionStage::budget, gg::rejected_estimate(), true); continue;
                }
                Real available = current_budget;
                if (current_seconds == Real{}) {
                    Real horizon = facts.structure && goal.target ? *facts.target_build_seconds
                        : to_single(facts.build_seconds / real(2));
                    if (!(time_limit < Real{})) horizon = std::min(horizon, time_limit);
                    available = to_single(available + to_single(income * horizon));
                }
                const auto candidate_cost = facts.structure && goal.target ? *facts.target_cost : facts.cost;
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
            else if (plan.magic) stage(gg::SelectionStage::production, sync_.uniform(Real{}, real(1)));
            else {
                auto prospective = potential; prospective.units.push_back(proposals[i]); prospective.sources.push_back(2);
                const auto production = estimate(*input, prospective, plan.magic, goal.target != 0,
                    player.difficulty.galactic_build_time_multiplier, false, executor(perception_executor_), player.player);
                if (!production.feasible) stage(gg::SelectionStage::production, gg::rejected_estimate(), true);
                else {
                    factories[i] = facts.structure ? target(goal.target)->object : production.producers.back();
                    const auto mean = required(gg::estimate_single_type(input->producers, player.player, proposals[i], facts.channel, executor(nullptr)));
                    excess.push_back(facts.structure ? facts.build_seconds : to_single(mean - facts.build_seconds)); production_rows.push_back(i);
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
            if (!facts.hero && plan.ignore_target) contrast_weight = sync_.uniform(Real{}, to_single(info->combat_power / real(500)));
            else if (!facts.hero) {
                const auto* unit = best ? host_->view->find(best->object) : nullptr;
                const Real power = to_single(info->combat_power * (unit ? unit->health : real(1)));
                for (std::size_t row = 0; row < potential.current_contrast.size(); ++row) {
                    if (input->contrast[row].channel != facts.channel
                        || input->contrast[row].escort != plan.taskforces[slots[allocations[i]].force].escort) continue;
                    const Real deficit = potential.current_contrast[row].force;
                    if (!(Real{} < deficit) || (!best && !(potential.target_contrast[row].force * potential.threshold < deficit))) continue;
                    const auto effectiveness = average_contrast(*host_, *info, input->contrast[row].category);
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
                        if (input->contrast[row].escort || input->contrast[row].channel != spawned.channel) continue;
                        const auto deficit = potential.current_contrast[row].force;
                        if (!(Real{} < deficit) || (!best && !(potential.target_contrast[row].force * potential.threshold < deficit))) continue;
                        const auto effectiveness = average_contrast(*host_, *spawned_type, input->contrast[row].category);
                        const auto applied = to_single(spawned.power * effectiveness);
                        if (!(Real{} < effectiveness) || !(Real{} < applied)) continue;
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
        const auto production = estimate(*input, potential, plan.magic, goal.target != 0,
            player.difficulty.galactic_build_time_multiplier, false, executor(perception_executor_), player.player);
        if (production.feasible) {
            current_seconds = production.seconds;
            if (Real{} < current_seconds) current_budget = resources(current_seconds);
        }
        if (contrast_rows[chosen] < input->contrast.size()) {
            const auto row = contrast_rows[chosen];
            const auto applied = input->contrast[row].category == 0 ? power
                : to_single(to_single(power * average_contrast(*host_, *info, input->contrast[row].category))
                    * (facts.channel == gc::ProductionDomain::ground ? facts.terrain_effectiveness : real(1)));
            potential.current_contrast[row].force = to_single(potential.current_contrast[row].force - applied);
            if (input->contrast[row].category != 0) for (std::size_t total = 0; total < input->contrast.size(); ++total)
                if (!input->contrast[total].category && input->contrast[total].channel == facts.channel
                    && input->contrast[total].escort == input->contrast[row].escort)
                    potential.current_contrast[total].force = to_single(potential.current_contrast[total].force - power);
        }
        for (std::size_t garrison = 0; garrison < facts.garrisons.size(); ++garrison) {
            const auto row = garrison_rows[chosen][garrison]; if (row >= input->contrast.size()) continue;
            const auto& spawned = facts.garrisons[garrison]; const auto* spawned_type = host_->type(spawned.type);
            const auto applied = !input->contrast[row].category ? spawned.power
                : to_single(to_single(spawned.power * average_contrast(*host_, *spawned_type, input->contrast[row].category))
                    * (spawned.channel == gc::ProductionDomain::ground ? spawned.terrain_effectiveness : real(1)));
            potential.current_contrast[row].force = to_single(potential.current_contrast[row].force - applied);
            if (input->contrast[row].category) for (std::size_t total = 0; total < input->contrast.size(); ++total)
                if (!input->contrast[total].category && !input->contrast[total].escort && input->contrast[total].channel == spawned.channel)
                    potential.current_contrast[total].force = to_single(potential.current_contrast[total].force - spawned.power);
        }
    }
    potential.valid = !needs() && (plan.ignore_target || !deficits())
        && (!(Real{} < input->maximum_cost) || !(input->maximum_cost < current_cost))
        && (!potential.units.empty() || std::any_of(plan.taskforces.begin(), plan.taskforces.end(), [](const auto& force) { return force.required; }));
    return potential.valid;
}

void Engine::service_galactic_execution(PlayerAi& player) {
    mark_native_state_changed();
    std::set<std::uint64_t> touched;
    for (auto& task : player.tasks) {
        touched.insert(task.block);
        if (task.finished) continue;
        auto force = taskforces_.find(task.taskforce);
        const auto* running = force == taskforces_.end() ? nullptr : plan(force->second.plan);
        if (!running || !galactic_goal(player.player, running->goal)) { task.finished = task.failed = true; continue; }
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
            const auto* goal = find_goal(player, running->goal);
            const auto input = goal ? galactic_selection(player, *goal, plans_[running->definition]) : nullptr;
            if (!input) throw std::runtime_error("galactic assignment facts unavailable");
            sim::EntityId fallback{};
            bool waiting{};
            for (const auto& row : input->freestore) {
                const auto reservation = player.reserved.find(row.object);
                if (row.in_transit && row.type == task.type && !player.assigned.contains(row.object)
                    && (reservation == player.reserved.end() || reservation->second == running->goal)) {
                    waiting = true; continue;
                }
                const auto* unit = host_->view->find(row.object);
                if (!unit || unit->owner != player.player || unit->type != task.type || player.assigned.contains(row.object)) continue;
                const auto reserved = player.reserved.find(row.object);
                if (reserved != player.reserved.end() && reserved->second == running->goal) { task.object = row.object; break; }
                if (!fallback && reserved == player.reserved.end()) fallback = row.object;
            }
            if (!task.object) task.object = fallback;
            if (!task.object && waiting) continue; // WGA-77: retained transit is still pending
            auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
            if (task.object && actions && actions->qualified_freestore_available()) {
                const auto assigned = actions->qualified_freestore().assign(player.player, task.object, goal && goal->potential ? goal->potential->id : 0);
                if (assigned == galactic_ai::FreestoreAssignment::waiting) { task.object = 0; continue; }
                if (assigned == galactic_ai::FreestoreAssignment::failed) task.object = 0;
            }
            task.finished = true; task.failed = task.object == 0;
        } else { task.finished = task.failed = true; }
        if (task.finished && !task.failed && task.object) {
            if (player.assigned.contains(task.object) && player.assigned.at(task.object) != task.taskforce)
                throw std::runtime_error("galactic completion object belongs to another taskforce");
            if (std::find(force->second.units.begin(), force->second.units.end(), task.object) == force->second.units.end()) force->second.units.push_back(task.object);
            force->second.had_units = true;
            player.assigned[task.object] = task.taskforce;
            task.joined = true;
        }
    }
    for (auto id : touched) {
        const auto block = blocks_.find(id); if (block == blocks_.end()) continue;
        bool done = true, failed = false;
        for (const auto& task : player.tasks) if (task.block == id) { done = done && task.finished; failed = failed || task.failed; }
        if (done) { block->second.finished = true; block->second.result = !failed; }
    }
}

core::Result<std::pair<Real, bool>> Engine::galactic_freestore_selection_facts(
    tactical::PlayerId id, gc::PlanetId target_id, sim::EntityId object, const PlanDef& definition) const {
    using Result = core::Result<std::pair<Real, bool>>;
    const auto refuse = [](std::string message) { return Result::failure(detail::make_error(authoritative::codes::missing_api, std::move(message))); };
    const auto* actions = dynamic_cast<const galactic_ai::NativeActions*>(host_->galactic_execution.get());
    const auto* world = dynamic_cast<const GalacticWorld*>(host_->ai_world.get());
    const auto* player = player_ai(id);
    if (!actions || !world || !player || !player->galactic) return refuse("BTP-U04: native stage/garrison owners unavailable");
    const auto store = actions->freestore_owner().lock(); const auto* facts = store ? store->facts(object) : nullptr;
    if (!facts || !facts->qualified || facts->object.owner != id || !galactic_stage_garrison_)
        return refuse("BTP-U04: retained per-object garrison flag unavailable");
    const auto snapshot = actions->snapshot(); if (!snapshot) return refuse("BTP-U04: completed stage world unavailable");
    const auto garrison = galactic_stage_garrison_(id, *snapshot, object);
    if (!garrison) return Result::failure(garrison.error());
    if (!target_id) return Result::success({Real{}, garrison.value()}); // BTP-S01: global proposal has zero stage cost
    if (!galactic_stage_combat_skip_) return refuse("BTP-U04: retained actor combat-skip predicate unavailable");
    const auto skip = galactic_stage_combat_skip_(id, *snapshot, object);
    if (!skip) return Result::failure(skip.error());
    if (skip.value()) return Result::success({Real{}, garrison.value()});
    const GoalType* goal_type{};
    for (const auto* rows : {&player->active, &player->proposed}) for (const auto& goal : *rows) {
        const auto* destination = target(goal.target);
        const auto* running = plan(goal.plan);
        const auto index = goal.potential ? std::optional{goal.potential->plan}
            : running ? std::optional{running->definition} : std::nullopt;
        if (!destination || destination->object != target_id || !index || plans_.at(*index).name != definition.name) continue;
        const auto* candidate = player->functions.at(goal.function).goal;
        if (goal_type && goal_type->reachability != candidate->reachability) return refuse("BTP-U04: ambiguous authored staging policy");
        goal_type = candidate;
    }
    if (!goal_type || !galactic_stage_catalog_) return refuse("BTP-U04: retained goal/stage catalog unavailable");
    const auto constraints = galactic_stage_catalog_->constraints.find(definition.name);
    const auto input = world->copy_input();
    if (constraints == galactic_stage_catalog_->constraints.end() || !input || !snapshot || !host_->view)
        return refuse("BTP-U04: authored stage constraints/completed world unavailable");
    std::vector<gc::PlanetId> sites;
    for (const auto& site : input->targets) {
        if (!allowed_production_site(*input, id, site.planet)) continue;
        const auto reach = world->goal_reachable(id, target_id, site.planet, goal_type->reachability);
        if (!reach) return refuse("BTP-U04: stage production-site relation unavailable");
        if (*reach) sites.push_back(site.planet);
    }
    const auto stages = galactic_force_stages(definition, constraints->second.forces, target_id, goal_type->reachability, sites);
    if (!stages) return Result::failure(stages.error());
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
    const bool prefix = upper_case(goal_type->reachability) == "SINGLE_HOP_DISCONNECTED";
    const auto tolerance = parse_real("0.0001").value();
    for (const auto stage : stages.value()) {
        const auto position = std::find_if(input->targets.begin(), input->targets.end(), [&](const auto& row) { return row.planet == stage; });
        if (position == input->targets.end()) return refuse("BTP-U04: stage target position unavailable");
        Real helper = gg::rejected_estimate();
        if (source == stage) helper = {};
        else if (const auto* route = world->execution_route(id, source, stage)) {
            if (!route->empty && (prefix ? route->prefix_threat : route->threat) <= tolerance) helper = route->distance;
        }
        if (helper != gg::rejected_estimate()) { cost = to_single(cost + to_single(helper * helper)); reachable = true; }
        const auto x = to_single(numeric::from_fixed(actor->position.x) - numeric::from_fixed(position->position.x));
        const auto y = to_single(numeric::from_fixed(actor->position.y) - numeric::from_fixed(position->position.y));
        cost = to_single(cost + to_single(to_single(x * x) + to_single(y * y)));
    }
    if (!reachable || stages.value().empty()) return Result::success({gg::rejected_estimate(), garrison.value()});
    return Result::success({to_single(cost / real(stages.value().size())), garrison.value()}); // BTP-S02/03
}

core::Result<Engine::GalacticBuildPublication> Engine::galactic_build_budgets(
    const galactic_ai::NativeActions& actions, const GalacticFunding& funding) const {
    using Result = core::Result<GalacticBuildPublication>;
    const auto funding_revision = funding.state_revision(); const auto actions_revision = actions.build_state_revision();
    const std::array key{galactic_tasks_revision_, funding_revision, actions_revision};
    if (galactic_build_cache_ && galactic_build_cache_key_ == key)
        return Result::success(galactic_build_cache_);
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
    galactic_build_cache_ = std::make_shared<const std::vector<GalacticBudgetBuild>>(std::move(rows));
    galactic_build_cache_key_ = key;
    return Result::success(galactic_build_cache_);
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
