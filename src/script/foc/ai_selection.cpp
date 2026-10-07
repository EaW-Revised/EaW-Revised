#include "ai_engine_internal.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace eawr::script::foc::ai {

using namespace engine_internal;

// EX-01: the free store objects a potential plan may take, with their cost (the XY distance
// squared to the goal target, 0 without one).
std::vector<const ViewUnit*> Engine::freestore_list(const PlayerAi& player, const Goal& goal, const PlanDef& plan) const {
    std::vector<const ViewUnit*> out;
    for (const ViewUnit& unit : host_->view->units) {
        if (!in_freestore(unit, player.player)) continue;
        if (const auto found = player.reserved.find(unit.id); found != player.reserved.end() && found->second != goal.id) continue;
        if (unit.health < data_.constants.health_low_threshold) continue;
        if (!plan.allow_engaged) {
            const AiType* type = host_->type(unit.type);
            const auto fighter = host_->setup.content.categories.find("FIGHTER");
            if (type != nullptr && unit.container && fighter != host_->setup.content.categories.end() &&
                (type->category_bits & fighter->second) != 0) {
                bool engaged = unit.attack_target != 0;
                for (const sim::EntityId member : unit.members) {
                    if (const ViewUnit* craft = host_->view->find(member)) engaged = engaged || craft->attack_target != 0;
                }
                if (engaged) continue;
            }
        }
        out.push_back(&unit);
    }
    return out;
}

// PL-31: the target's contrast list: entry 0 the target force, then one entry per enemy
// contrast category, each times the plan's maximum contrast factor.
std::vector<Contrast> Engine::contrast_list(const PlayerAi& player, const Goal& goal, const PlanDef& plan, Real max_factor) const {
    static_cast<void>(plan);
    std::vector<Contrast> out;
    const Target* target = this->target(goal.target);
    if (target == nullptr) return out;
    const auto force = [&](std::uint64_t category) -> Real {
        if (target->object != 0) {
            const ViewUnit* unit = host_->view->find(target->object);
            if (unit == nullptr) return Real{};
            const AiType* type = host_->type(unit->type);
            if (type == nullptr || (type->category_bits & category) == 0) return Real{};
            return type->combat_power;
        }
        return grid_.force(*host_, *host_->view, target->region, category, player.player, false, Real{}, 0);
    };
    const Real total = force(~std::uint64_t{0});
    out.push_back(Contrast{0, to_single(total * max_factor)});
    for (const std::uint64_t category : host_->contrast_order) {
        out.push_back(Contrast{category, total <= Real{} ? Real{} : to_single(force(category) * max_factor)});
    }
    if (total <= Real{}) {
        for (Contrast& entry : out) entry.force = Real{};
    }
    return out;
}

namespace {

// PL-33: the share of the target force the selection leaves, overall and by category.
bool contrast_met(const std::vector<Contrast>& target, const std::vector<Contrast>& current, Real threshold) {
    if (target.empty() || target.front().force <= Real{}) return true;
    const Real total = target.front().force;
    const Real left = std::max(current.front().force, Real{});
    if (threshold < left / total) return false;
    Real breakdown{};
    for (std::size_t index = 1; index < target.size(); ++index) {
        if (target[index].force <= Real{}) continue;
        const Real share = std::max(current[index].force, Real{}) / target[index].force;
        breakdown = breakdown + share * (target[index].force / total);
    }
    return !(threshold < breakdown);
}

} // namespace

// PL-20 to PL-35: the marshallers select units for the goal's plan one at a time.
bool Engine::select_units(PlayerAi& player, const Goal& goal, PotentialPlan& potential) {
    const PlanDef& plan = plans_[potential.plan];
    potential.units.clear();
    potential.freestore.clear();
    potential.sources.clear();
    potential.producers.clear();
    potential.pool_tokens.clear();
    potential.cost = Real{};
    potential.taskforce_of_unit.clear();
    potential.team_of_unit.clear();
    potential.valid = false;
    const Target* target = this->target(goal.target);
    Real target_x{}, target_y{};
    bool has_target = false;
    if (target != nullptr) {
        if (target->object != 0) {
            if (const ViewUnit* unit = host_->view->find(target->object)) {
                target_x = fixed(unit->position.x);
                target_y = fixed(unit->position.y);
                has_target = true;
            }
        } else {
            target_x = target->x;
            target_y = target->y;
            has_target = true;
        }
    }
    // Free store marshaller (PL-21).
    const std::vector<const ViewUnit*> store = plan.allow_free_store ? freestore_list(player, goal, plan) : std::vector<const ViewUnit*>{};
    std::vector<Real> cost(store.size());
    Real max_cost{};
    for (std::size_t index = 0; index < store.size(); ++index) {
        cost[index] = has_target ? xy_distance_squared(store[index]->position, target_x, target_y) : Real{};
        max_cost = std::max(max_cost, cost[index]);
    }
    std::vector<bool> allocated(store.size());
    // Plan marshaller (PL-22): the team layout and the proposal types.
    struct Slot {
        std::size_t taskforce{};
        std::size_t team{};
        std::int32_t count{};
    };
    std::vector<Slot> layout;
    std::vector<tactical::TypeId> proposals;
    const auto* account = host_->economy(player.player);
    Real credits = fixed(host_->credits(player.player));
    // WAS-25: a funded goal can restore its own allocation without charging again.
    if (const auto own = player.reserved_credits.find(goal.id); own != player.reserved_credits.end())
        credits = credits + own->second;
    const auto pooled_count = [&](tactical::TypeId type) {
        std::int64_t count = 0;
        if (account != nullptr) for (std::size_t index = 0; index < account->pool_tokens.size(); ++index) {
            const auto reserved = player.reserved_pool_tokens.find(account->pool_tokens[index]);
            if (index < account->pool.size() && account->pool[index] == type
                && (reserved == player.reserved_pool_tokens.end() || reserved->second == goal.id)) ++count;
        }
        for (std::size_t i = 0; i < potential.units.size(); ++i) {
            if (potential.units[i] == type && potential.sources[i] == 1) --count;
        }
        return count;
    };
    const auto pooled_token = [&](tactical::TypeId type) -> std::uint64_t {
        if (account == nullptr) return 0;
        for (std::size_t index = 0; index < account->pool_tokens.size(); ++index) {
            const auto token = account->pool_tokens[index];
            const auto reserved = player.reserved_pool_tokens.find(token);
            if (index < account->pool.size() && account->pool[index] == type
                && (reserved == player.reserved_pool_tokens.end() || reserved->second == goal.id)
                && std::find(potential.pool_tokens.begin(), potential.pool_tokens.end(), token) == potential.pool_tokens.end()) return token;
        }
        return 0;
    };
    for (std::size_t tf = 0; tf < plan.taskforces.size(); ++tf) {
        for (std::size_t team = 0; team < plan.taskforces[tf].teams.size(); ++team) {
            layout.push_back(Slot{tf, team, 0});
            for (const tactical::TypeId type : plan.taskforces[tf].teams[team].types) {
                if (std::find(proposals.begin(), proposals.end(), type) != proposals.end()) continue;
                bool present = false;
                for (const ViewUnit* unit : store) present = present || unit->type == type;
                if (!host_->setup.perception.campaign_game && account != nullptr) {
                    if (plan.allow_free_store) present = present || pooled_count(type) > 0;
                    else if (!plan.magic) present = present || producer(player.player, type, target != nullptr ? target->object : 0) != 0;
                }
                if (present) proposals.push_back(type);
            }
        }
    }
    // PL-22: each proposed type draws its own start among only its eligible teams.
    // A singleton list consumes no random draw (PE-11).
    std::vector<std::vector<std::size_t>> teams_of_type(proposals.size());
    std::vector<std::size_t> starts(proposals.size());
    for (std::size_t index = 0; index < proposals.size(); ++index) {
        auto& teams = teams_of_type[index];
        for (std::size_t slot = 0; slot < layout.size(); ++slot) {
            const TeamDef& team = plan.taskforces[layout[slot].taskforce].teams[layout[slot].team];
            if (std::find(team.types.begin(), team.types.end(), proposals[index]) != team.types.end()) teams.push_back(slot);
        }
        if (!teams.empty()) starts[index] = static_cast<std::size_t>(sync_.range(0, static_cast<std::int32_t>(teams.size()) - 1));
    }
    std::vector<std::int32_t> tf_size(plan.taskforces.size());
    std::vector<Real> tf_force(plan.taskforces.size());
    std::vector<std::uint64_t> tf_categories(plan.taskforces.size());
    // Contrast marshaller (PL-30).
    const auto plan_history = player.plan_outcomes.find({potential.plan, goal.target});
    const Real failures = real(recent_failures(plan_history == player.plan_outcomes.end() ? nullptr : &plan_history->second));
    const Real adjust = to_single(failures * plan.per_failure_contrast_adjust);
    const Real max_scale = to_single(plan.max_contrast + adjust);
    const Real min_scale = to_single(plan.min_contrast + adjust);
    const bool contrast = target != nullptr && !plan.ignore_target;
    potential.threshold = contrast && Real{} < max_scale ? to_single(real(1) - min_scale / max_scale) : Real{};
    potential.target_contrast = contrast
        ? contrast_list(player, goal, plan, to_single(max_scale * player.difficulty.space_contrast_multiplier))
        : std::vector<Contrast>{};
    potential.current_contrast = potential.target_contrast;
    const auto still_needs = [&] {
        for (const Slot& slot : layout) {
            if (slot.count < plan.taskforces[slot.taskforce].teams[slot.team].min_count) return true;
        }
        for (std::size_t tf = 0; tf < plan.taskforces.size(); ++tf) {
            if (tf_force[tf] < plan.taskforces[tf].minimum_force || tf_size[tf] < plan.taskforces[tf].minimum_size) return true;
        }
        for (const std::uint64_t category : plan.required_categories) {
            bool matched = false;
            for (const std::uint64_t bits : tf_categories) matched = matched || (bits & category) != 0;
            if (!matched) return true;
        }
        return false;
    };
    const auto type_power = [&](tactical::TypeId id) {
        const AiType* type = host_->type(id);
        return type == nullptr ? Real{} : type->combat_power;
    };
    std::vector<std::size_t> contrast_index(proposals.size());
    std::vector<bool> erased(proposals.size());
    for (;;) {
        std::vector<Real> weights(proposals.size(), -huge);
        std::vector<std::size_t> allocation(proposals.size(), store.size());
        for (std::size_t index = 0; index < proposals.size(); ++index) {
            if (erased[index]) continue;
            const tactical::TypeId type = proposals[index];
            const AiType* info = host_->type(type);
            Real weight{};
            bool reject = false;
            // Free store: the nearest unallocated object of the type.
            std::size_t best = store.size();
            for (std::size_t object = 0; object < store.size(); ++object) {
                if (allocated[object] || store[object]->type != type) continue;
                if (best == store.size() || cost[object] < cost[best]) best = object;
            }
            allocation[index] = best;
            if (best != store.size() && Real{} < max_cost) weight = weight + to_single(real(1) - cost[best] / max_cost);
            // Tech tree (tactical): Tech_Level / 5 * 2.
            if (info != nullptr) weight = weight + to_single(to_single(real(info->tech_level) / real(5)) * real(2));
            // Plan: the first team of the type with room.
            Real plan_weight = huge;
            for (const Slot& slot : layout) {
                const TeamDef& team = plan.taskforces[slot.taskforce].teams[slot.team];
                if (std::find(team.types.begin(), team.types.end(), type) == team.types.end()) continue;
                if (team.percentage_based) {
                    plan_weight = real(1);
                    break;
                }
                if (slot.count < team.max_count) {
                    plan_weight = to_single(real(1) - real(slot.count) / real(team.max_count));
                    break;
                }
            }
            if (plan_weight == huge) reject = true;
            weight = weight + (reject ? Real{} : plan_weight);
            // SAE-03: pooled combat units or affordable new production, after the plan gate.
            if (!reject && best == store.size() && !plan.magic) {
                if (plan.allow_free_store) {
                    if (pooled_count(type) <= 0 || account == nullptr || account->population >= account->population_cap) {
                        erased[index] = true;
                        continue;
                    }
                } else {
                    const auto factory = producer(player.player, type, target != nullptr ? target->object : 0);
                    const auto* option = build_option(player.player, type, factory);
                    const auto allowed = option != nullptr && tactical::production_allowed(*option, true, [&](tactical::TypeId counted) {
                        auto counts = host_->world->production_counts(player.player, counted);
                        for (std::size_t selected = 0; selected < potential.units.size(); ++selected) {
                            if (potential.units[selected] == counted && potential.sources[selected] == 2) {
                                ++counts.current_player; ++counts.current_allies;
                                ++counts.lifetime_player; ++counts.lifetime_allies;
                            }
                        }
                        return counts;
                    });
                    if (option == nullptr || credits < potential.cost + fixed(option->price)
                        || !allowed
                        || (option->kind == tactical::BuildKind::structure
                            && std::find(potential.producers.begin(), potential.producers.end(), factory) != potential.producers.end())) {
                        erased[index] = true;
                        continue;
                    }
                    // Debug build SAE-E04: feasible tactical production contributes 1.
                    weight = weight + real(1);
                }
            }
            // Contrast.
            if (!reject) {
                Real contrast_weight{};
                if (plan.ignore_target) {
                    contrast_weight = sync_.uniform(Real{}, to_single(type_power(type) / real(500)));
                } else if (contrast && info != nullptr) {
                    Real top{};
                    std::size_t top_index = 0;
                    for (std::size_t entry = 1; entry < potential.current_contrast.size(); ++entry) {
                        const Real force = potential.current_contrast[entry].force;
                        if (!(Real{} < force)) continue;
                        const Real factor = average_contrast(*host_, *info, potential.current_contrast[entry].category);
                        const Real applied = to_single(type_power(type) * factor);
                        const Real ratio = std::max(to_single(force - applied), Real{}) / std::max(force, applied);
                        const Real value = to_single(std::max(to_single(real(1) - ratio * ratio), Real{}) * factor);
                        if (top < value) {
                            top = value;
                            top_index = entry;
                        }
                    }
                    contrast_index[index] = top_index;
                    if (Real{} < top) top = to_single(top * real(2));
                    if (contrast_met(potential.target_contrast, potential.current_contrast, potential.threshold)) {
                        if (still_needs()) {
                            contrast_weight = Real{};
                        } else {
                            reject = true;
                        }
                    } else {
                        contrast_weight = top;
                    }
                }
                weight = weight + contrast_weight;
            }
            weights[index] = reject ? -huge : to_single(weight);
        }
        // PL-24: unit variety. A type holding half the selection yields while another can go in.
        if (!potential.units.empty()) {
            std::vector<Real> varied = weights;
            for (std::size_t index = 0; index < proposals.size(); ++index) {
                if (!(-huge < weights[index])) continue;
                bool other = false;
                for (std::size_t alternative = 0; alternative < proposals.size(); ++alternative) {
                    other = other || (alternative != index && -huge < weights[alternative]);
                }
                const auto count = std::count(potential.units.begin(), potential.units.end(), proposals[index]);
                if (other && static_cast<std::size_t>(count) * 2 >= potential.units.size()) varied[index] = -huge;
            }
            weights = std::move(varied);
        }
        std::size_t pick = proposals.size();
        for (std::size_t index = 0; index < proposals.size(); ++index) {
            if (!(-huge < weights[index])) continue;
            if (pick == proposals.size() || weights[pick] < weights[index]) pick = index;
        }
        if (pick == proposals.size()) break;
        const tactical::TypeId type = proposals[pick];
        const std::size_t object = allocation[pick];
        potential.units.push_back(type);
        potential.freestore.push_back(object == store.size() ? 0 : store[object]->id);
        const std::uint8_t source = object != store.size() ? 0 : (plan.allow_free_store ? 1 : 2);
        potential.sources.push_back(source);
        potential.producers.push_back(source == 2 ? producer(player.player, type, target != nullptr ? target->object : 0) : 0);
        potential.pool_tokens.push_back(source == 1 ? pooled_token(type) : 0);
        if (source == 2) {
            const auto* option = build_option(player.player, type, producer(player.player, type));
            if (option != nullptr) potential.cost = to_single(potential.cost + fixed(option->price));
        }
        if (object != store.size()) allocated[object] = true;
        // The team: rotate this type's eligible teams from its own drawn start.
        const auto& teams = teams_of_type[pick];
        std::size_t chosen = layout.size();
        for (std::size_t step = 0; step < teams.size(); ++step) {
            const std::size_t slot = teams[(starts[pick] + step) % teams.size()];
            const TeamDef& team = plan.taskforces[layout[slot].taskforce].teams[layout[slot].team];
            if (team.percentage_based || layout[slot].count < team.max_count) {
                chosen = slot;
                break;
            }
        }
        if (chosen == layout.size()) {
            if (!teams.empty()) chosen = teams[starts[pick]];
        }
        const std::size_t tf = chosen == layout.size() ? 0 : layout[chosen].taskforce;
        if (chosen != layout.size()) ++layout[chosen].count;
        potential.taskforce_of_unit.push_back(tf);
        potential.team_of_unit.push_back(chosen == layout.size() ? 0 : layout[chosen].team);
        ++tf_size[tf];
        tf_force[tf] = to_single(tf_force[tf] + type_power(type));
        if (const AiType* info = host_->type(type)) tf_categories[tf] |= info->category_bits;
        // PL-32: the unit's contrast leaves the target's.
        if (!potential.current_contrast.empty()) {
            const Real unit_power = type_power(type);
            const std::size_t entry = contrast_index[pick];
            if (entry < 1) {
                potential.current_contrast.front().force = to_single(potential.current_contrast.front().force - unit_power);
            } else if (const AiType* info = host_->type(type)) {
                const Real factor = average_contrast(*host_, *info, potential.current_contrast[entry].category);
                potential.current_contrast[entry].force = to_single(potential.current_contrast[entry].force - unit_power * factor);
                potential.current_contrast.front().force = to_single(potential.current_contrast.front().force - unit_power);
            }
        }
    }
    // PL-25: the plan's validity.
    bool valid = true;
    if (potential.units.empty()) {
        valid = false;
        for (const TaskForceDef& tf : plan.taskforces) valid = valid || tf.required;
    }
    if (still_needs()) valid = false;
    if (contrast && !contrast_met(potential.target_contrast, potential.current_contrast, potential.threshold)) valid = false;
    potential.valid = valid;
    return valid;
}

bool Engine::production_time_allowed(const PlayerAi&, const Goal&) const {
    // SAE-09: tactical selection resets its activation estimate to zero; only galactic
    // selection computes production travel/queue time. SAE-05 rejects only an estimate
    // exceeding a positive limit, so zero never rejects here. Feasibility remains SAE-03.
    return true;
}

// GS-21: a potential plan is still valid when its units are still there for it.
bool Engine::test_valid(PlayerAi& player, Goal& goal) {
    if (!goal.potential || !goal.potential->valid) return false;
    if (goal.plan != 0) return running_.contains(goal.plan);
    if (!goal.potential->reserved && !goal.potential->funded)
        return select_units(player, goal, *goal.potential) && production_time_allowed(player, goal);
    for (const sim::EntityId object : goal.potential->freestore) {
        if (object == 0) continue;
        const ViewUnit* unit = host_->view->find(object);
        if (unit == nullptr || !in_freestore(*unit, player.player)) return false;
        if (const auto found = player.reserved.find(object); found != player.reserved.end() && found->second != goal.id) return false;
    }
    return true;
}

// GS-22: the contrast test of an active goal.
bool Engine::test_target_contrast(PlayerAi& player, Goal& goal) {
    if (!goal.potential || !goal.potential->valid) return false;
    if (goal.plan != 0) {
        // A running plan keeps its goal while its target lives (fidelity list: the retail test
        // recomputes the contrast of the TaskForce units).
        const Target* target = this->target(goal.target);
        return target == nullptr || target->object == 0 || host_->view->find(target->object) != nullptr;
    }
    if (!goal.potential->reserved) return test_valid(player, goal);
    if (!test_valid(player, goal)) return false;
    const PlanDef& plan = plans_[goal.potential->plan];
    if (plan.ignore_target || goal.target == 0) return true;
    if (goal.potential->funded) return true; // WAS-25: restoration retains the original allocation
    return select_units(player, goal, *goal.potential);
}

} // namespace eawr::script::foc::ai
