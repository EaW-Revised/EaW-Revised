#include "eawr/core/load_profile.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/sim/tactical/session.hpp"

#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/pathfind.hpp"
#include "eawr/sim/math/trig.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "blast_internal.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "orders_internal.hpp"
#include "staging.hpp"
#include "session_cmd.hpp"
#include "session_services.hpp"
#include "tactical_internal.hpp"

#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <array>
#include "diagnostic_timer.hpp"
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>


namespace eawr::sim::tactical {

// A plan submits the unit's new prediction: its layer rebuilds from this frame (AV-02,
// AV-15). `clip`: a waiting group member's plan frame, the end of its prediction (FM-10).
core::Result<void> session_detail::Tick::PlanLanes::submit(const EntityId unit_id, const LiveUnit& live, const std::size_t layer, const std::optional<std::uint64_t> clip) {
    const auto& frame = phase.tracked_.value().frame.value();
    auto& rebuilt = phase.tracked_.value().rebuilt.value();
    auto& tracking = phase.tracked_.value().tracking.value();
    core::tick_profile::Scope submit_scope("serial.commands.submit");
    rebuilt[layer] = true;
    if (!tracking) return core::Result<void>::success();
    tracking->views[layer].reset();
    tracking->samples.erase(unit_id);
    if (live.motion->kind != MotionKind::none) {
        const auto yaw = yaw_degrees(live.state.rotation);
        if (!yaw) return core::Result<void>::failure(yaw.error());
        auto fresh = sample_windows(*live.motion, frame, tracking->interval, tracking->windows,
            live.state.position, yaw.value(), clip);
        if (!fresh) return core::Result<void>::failure(fresh.error());
        TrackSamples samples;
        samples.current = fresh.value();
        samples.fresh = std::move(fresh).value();
        tracking->samples.emplace(unit_id, std::move(samples));
    }
    return core::Result<void>::success();
}

// PC-09: `live` waits for `search`, which has ended: its layer predicts the search's plan
// from the landing on (sample_ahead). The layer's windows keep their anchor: the landing,
// not the search's end, is the unit's submission (PC-08), so when the search ends (which
// depends on the slice size) changes only what a later search reads before the landing.
core::Result<void> session_detail::Tick::PlanLanes::publish(const EntityId unit_id, const LiveUnit& live, const Impl::SlicedSearch& search) {
    const auto& frame = phase.tracked_.value().frame.value();
    auto& tracking = phase.tracked_.value().tracking.value();
    const auto& table = phase.impl_->motion;
    core::tick_profile::Scope publish_scope("serial.commands.publish");
    if (!tracking) return core::Result<void>::success();
    const auto* footprint = tracked_footprint(table, live.state.type_id);
    const auto layer = footprint != nullptr ? dynamic_layer_index(footprint->layer) : std::nullopt;
    if (!layer) return core::Result<void>::success();
    tracking->views[*layer].reset();
    tracking->samples.erase(unit_id);
    const auto yaw = yaw_degrees(live.state.rotation);
    if (!yaw) return core::Result<void>::failure(yaw.error());
    const MotionState* current = live.motion && live.motion->kind != MotionKind::none ? &*live.motion : nullptr;
    const auto sample = [&](const std::uint64_t from) {
        return sample_ahead(current, search.frame, *search.plan, from, tracking->interval, tracking->windows,
            live.state.position, yaw.value());
    };
    const auto start = tracking->current_start[*layer];
    auto from_anchor = sample(start);
    if (!from_anchor) return core::Result<void>::failure(from_anchor.error());
    TrackSamples samples;
    samples.current = std::move(from_anchor).value();
    if (start != frame) {
        auto fresh = sample(frame);
        if (!fresh) return core::Result<void>::failure(fresh.error());
        samples.fresh = std::move(fresh).value();
    }
    tracking->samples.emplace(unit_id, std::move(samples));
    return core::Result<void>::success();
}

// The collision world a move in dynamic layer `layer` plans against (AV-01).
core::Result<void> session_detail::Tick::PlanLanes::world_for(const std::size_t layer, CollisionWorld& collisions) {
    auto& rebuilt = phase.tracked_.value().rebuilt.value();
    auto& tracking = phase.tracked_.value().tracking.value();
    auto& staged = phase.tracked_.value().staged.value();
    const auto& table = phase.impl_->motion;
    core::tick_profile::Scope collision_scope("serial.commands.collision-view");
    if (auto ensured = ensure_view(*tracking, table, staged, rebuilt, layer); !ensured) return ensured;
    if (auto statics = ensure_statics(*tracking, table, staged); !statics) return statics;
    collisions.interval = tracking->interval;
    collisions.layers[layer] = &*tracking->views[layer];
    collisions.statics = &*tracking->statics;
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::PlanLanes::unit_failure(const EntityId unit_id, const core::Diagnostic& error) {

    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
        "tick " + std::to_string(phase.tick) + " unit " + std::to_string(unit_id) + ": " + error.message));
}

// PC-08: `job`'s search runs in slices from now on; its unit waits for the landing.
core::Result<void> session_detail::Tick::PlanLanes::start_sliced(const PlanJob& job) {
    const auto& frame = phase.tracked_.value().frame.value();
    auto& staged = phase.tracked_.value().staged.value();
    const auto& table = phase.impl_->motion;
    core::tick_profile::Scope sliced_scope("serial.commands.start-sliced");
    auto& live = staged.at(job.unit);
    CollisionWorld world;
    if (auto built = world_for(job.layer, world); !built) return built;
    const auto landing = frame + table.avoidance->search_delay;
    auto sliced = phase.impl_->start_sliced(live, job.destination, job.max_speed, frame, landing, world);
    if (!sliced) return core::Result<void>::failure(sliced.error());
    // The wait's maximum speed is hashed only: a unit's own caps nothing it would not cap itself.
    auto max_speed = job.max_speed;
    if (!max_speed) max_speed = sliced.value().inputs.limits.max_speed;
    phase.impl_->searches.insert_or_assign(job.unit, std::move(sliced).value());
    live.formation = FormationWait{landing, job.destination, *max_speed, job.order, job.rank, true};
    return submit(job.unit, live, job.layer, landing);
}

// PC-08: the unit takes its sliced search's plan, while it still plans from what the search
// planned from; otherwise (a change the prediction could not see) it plans now.
core::Result<void> session_detail::Tick::PlanLanes::land(const PlanJob& job) {
    const auto& frame = phase.tracked_.value().frame.value();
    auto& staged = phase.tracked_.value().staged.value();
    core::tick_profile::Scope landing_scope("serial.commands.land");
    auto& live = staged.at(job.unit);
    auto found = phase.impl_->searches.find(job.unit);
    if (found == phase.impl_->searches.end()) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure, "no sliced search to land"));
    }
    const auto inputs = phase.impl_->path_inputs(live, found->second.max_speed);
    if (!inputs) return core::Result<void>::failure(inputs.error());
    if (inputs.value() == found->second.inputs && found->second.search.ended()) {
        auto plan = found->second.search.result();
        if (!plan) return core::Result<void>::failure(plan.error());
        if (found->second.search.exhausted()) {
            Impl::retain_path_failure(live, job.destination, frame, inputs.value().limits.max_speed);
        } else {
            Impl::take_plan(live, std::move(plan).value());
        }
    } else {
        CollisionWorld world;
        if (auto built = world_for(job.layer, world); !built) return built;
        if (auto planned = phase.impl_->plan_path(live, found->second.target, frame, &world, found->second.max_speed, nullptr);
            !planned) {
            return planned;
        }
    }
    if (live.formation && live.formation->failure_frame) {
        live.formation->order = job.order;
        live.formation->rank = job.rank;
    }
    phase.impl_->searches.erase(found);
    return submit(job.unit, live, job.layer, std::nullopt);
}

// The tick's path searches (AV-15, FM-07, FM-12; PC-05, PC-07, PC-08): queued in their
// planning order and planned when something else is about to read or change a layer (a
// group's slot mapping, a face, stop or other plan, a non-move command) and after the
// commands. A search reads only its own layer and the static layer (AV-01), so each dynamic
// layer is a lane: its jobs run in order, each submitting before the next, and the lanes run
// side by side, the next search of every lane together in the partitioned `plan-searches`
// phase. That is exactly the serial result. A job is one of three:
// - a sliced search landing this frame (`landing`): the unit takes its plan (PC-08);
// - a search, while its layer has spent fewer expansions this tick than the avoidance rules'
//   search_budget (PC-07): it may spend the rest of the budget, and a search that reaches it
//   is given up and starts again sliced;
// - once the budget is spent, a sliced search from the start: it lands search_delay frames
//   later, and the unit keeps its current plan until then (PC-04), predicted up to that frame.
// The first failed job in planning order answers.
std::optional<std::pair<EntityId, core::Diagnostic>> session_detail::Tick::PlanLanes::plan_pending() {
    const auto& frame = phase.tracked_.value().frame.value();
    auto& staged = phase.tracked_.value().staged.value();
    const auto& table = phase.impl_->motion;
    core::tick_profile::Scope pending_scope("serial.commands.plan-pending");
    const auto budget = table.avoidance->search_budget;
    std::array<std::vector<std::size_t>, 4> lanes;
    for (std::size_t index = 0; index < pending.size(); ++index) lanes[pending[index].layer].push_back(index);
    std::array<std::size_t, 4> next{};
    while (true) {
        std::vector<std::size_t> round;
        for (std::size_t layer = 0; layer < lanes.size(); ++layer) {
            if (next[layer] < lanes[layer].size()) round.push_back(lanes[layer][next[layer]++]);
        }
        if (round.empty()) break;
        std::sort(round.begin(), round.end());
        const std::size_t count = round.size();
        // Which jobs search now, each within the rest of its layer's budget.
        std::vector<char> searches(count);
        std::vector<std::size_t> searching;
        for (std::size_t index = 0; index < count; ++index) {
            const auto& job = pending[round[index]];
            searches[index] = !job.landing && spent[job.layer] < budget ? 1 : 0;
            if (searches[index] != 0) searching.push_back(index);
        }
        std::vector<CollisionWorld> worlds(count);
        std::vector<LiveUnit*> units(count);
        std::vector<PathSearchStats> work(count);
        std::vector<char> planned(count);
        std::vector<std::optional<core::Diagnostic>> errors(count);
        for (const auto index : searching) {
            const auto& job = pending[round[index]];
            units[index] = &staged.at(job.unit);
            if (auto built = world_for(job.layer, worlds[index]); !built) errors[index] = built.error();
        }
        const auto search = [&](const std::size_t index) {
            if (errors[index]) return;
            const auto& job = pending[round[index]];
            core::tick_profile::JobScope path_job_scope("job.pending-search", job.unit);
            auto result = phase.impl_->plan_path_within(*units[index], job.destination, frame, &worlds[index], job.max_speed,
                &work[index], budget - spent[job.layer]);
            if (!result) {
                errors[index] = result.error();
                return;
            }
            planned[index] = result.value() ? 1 : 0;
        };
        if (searching.size() == 1) {
            core::tick_profile::Scope inline_scope("serial.commands.inline-search");
            search(searching.front());
        } else if (!searching.empty()) {
            core::tick_profile::prepare_jobs(searching.size());
            const auto searched = phase.executor.execute_phase("plan-searches", tick_partition_count, [&](const std::size_t partition) {
                const auto range = partition_range(partition, searching.size());
                for (auto index = range.begin; index < range.end; ++index) search(searching[index]);
            });
            if (!searched) return std::pair{pending[round[searching.front()]].unit, searched.error()};
        }
        for (std::size_t index = 0; index < count; ++index) {
            const auto& job = pending[round[index]];
            if (errors[index]) return std::pair{job.unit, std::move(*errors[index])};
            if (job.landing) {
                if (auto landed = land(job); !landed) return std::pair{job.unit, landed.error()};
                continue;
            }
            if (searches[index] != 0) {
                spent[job.layer] += work[index].expansions;
                if (planned[index] != 0) {
                    auto& live = *units[index];
                    if (live.formation && live.formation->failure_frame) {
                        live.formation->order = job.order;
                        live.formation->rank = job.rank;
                    }
                    if (auto submitted = submit(job.unit, *units[index], job.layer, std::nullopt); !submitted) {
                        return std::pair{job.unit, submitted.error()};
                    }
                    continue;
                }
            }
            if (auto sliced = start_sliced(job); !sliced) return std::pair{job.unit, sliced.error()};
        }
    }
    pending.clear();
    return std::nullopt;
}

// FO-07 to FO-10 (#552, #599): squadrons one command sends to the same point take their
// formation's slots around it, each at its own layer height, and its lanes. Serial, once per
// command.
core::Result<void> session_detail::Tick::PlanLanes::spread_squadrons(const std::vector<EntityId>& squadrons) {
    auto& minds = phase.craft_prep_.value().minds.value();
    auto& staged = phase.tracked_.value().staged.value();
    const auto& squadron_table = phase.impl_->motion.squadrons;
    core::tick_profile::Scope squadron_scope("serial.commands.squadron-slots");
    std::vector<GroupSquadron> members;
    members.reserve(squadrons.size());
    for (const auto id : squadrons) {
        const auto& state = minds.at(id);
        GroupSquadron member;
        member.position = staged.at(id).state.position;
        member.type = state.squadron_type;
        if (const auto* squadron = squadron_table.find_squadron(state.squadron_type);
            squadron != nullptr && !squadron->members.empty()) {
            if (const auto* craft = squadron_table.find_craft(squadron->members.front())) {
                member.max_speed = craft->max_speed;
                member.min_speed = craft->min_speed;
                member.attack_distance = craft->attack_distance;
            }
            // FO-08: the farthest slot plus the craft's soft radius.
            math::Fixed farthest{};
            for (const auto& offset : squadron->offsets) {
                auto reach = math::length(offset);
                if (!reach) return core::Result<void>::failure(reach.error());
                farthest = std::max(farthest, reach.value());
            }
            const auto* footprint = phase.impl_->motion.footprint(squadron->members.front());
            member.radius = math::Fixed::from_raw(farthest.raw() + (footprint != nullptr ? footprint->radius.raw() : 0));
        }
        members.push_back(member);
    }
    // Every squadron of the command flies to the same point; the slots keep its x and y.
    const auto destination = minds.at(squadrons.front()).anchor;
    auto slots = squadron_group_slots(members, destination);
    if (!slots) return core::Result<void>::failure(slots.error());
    for (std::size_t index = 0; index < squadrons.size(); ++index) {
        auto& state = minds.at(squadrons[index]);
        const auto& slot = slots.value()[index];
        state.anchor = {slot.point.x, slot.point.y, state.anchor.z};
        if (slot.lane) {
            // FO-10: the squadron flies its formation's path and its move ends at the line
            // through its slot square to that path (FO-02).
            state.lane = *slot.lane;
            state.lane->formation = squadrons[static_cast<std::size_t>(slot.lane->formation)];
            state.move_origin = {math::Fixed::from_raw(state.anchor.x.raw() - slot.lane->direction.x.raw()),
                math::Fixed::from_raw(state.anchor.y.raw() - slot.lane->direction.y.raw()), state.anchor.z};
        }
    }
    return core::Result<void>::success();
}

// FM-01 to FM-10: the tracked ships of one move command (in command order). One plans as a
// single move; two or more move as a group: each gets its slot and planning speed, the
// front ship of each layer plans now and the others wait, their predictions clipped.
core::Result<void> session_detail::Tick::PlanLanes::plan_group(const PlayerCommand& command, const std::vector<EntityId>& group) {
    const auto& frame = phase.tracked_.value().frame.value();
    auto& staged = phase.tracked_.value().staged.value();
    const auto& table = phase.impl_->motion;
    core::tick_profile::Scope group_scope("serial.commands.group");
    const auto destination = *point_move(command.payload);
    const auto layer_of = [&](const LiveUnit& live) {
        return *dynamic_layer_index(tracked_footprint(table, live.state.type_id)->layer);
    };
    if (group.size() == 1) {
        pending.push_back({group.front(), destination, std::nullopt, layer_of(staged.at(group.front())), command.key, 0,
            false});
        return core::Result<void>::success();
    }
    // The slot mapping reads the layers: the queued searches plan first.
    if (auto failed = plan_pending()) return core::Result<void>::failure(std::move(failed->second));
    std::vector<FormationMember> members;
    members.reserve(group.size());
    CollisionWorld mapping;
    for (const auto unit_id : group) {
        auto& live = staged.at(unit_id);
        const auto* footprint = tracked_footprint(table, live.state.type_id);
        const auto limits = phase.impl_->limits_of(live);
        if (!limits) return core::Result<void>::failure(limits.error());
        if (limits.value().max_speed.raw() <= 0) {
            // FM-09a: no slot for a ship with no speed. Its order is accepted (MV-03) and
            // its old path cleared, so it stays as a stationary obstacle.
            if (auto planned = phase.impl_->plan_path(live, destination, frame, nullptr, math::Fixed{}, nullptr); !planned) {
                return planned;
            }
            if (auto submitted = submit(unit_id, live, layer_of(live), std::nullopt); !submitted) {
                return submitted;
            }
            continue;
        }
        const auto yaw = yaw_degrees(live.state.rotation);
        if (!yaw) return core::Result<void>::failure(yaw.error());
        const auto radius = occupation_radius(
            *footprint, limits.value().max_speed, limits.value().rate_of_turn, *table.avoidance);
        if (!radius) return core::Result<void>::failure(radius.error());
        members.push_back(FormationMember{unit_id, live.state.position, yaw.value(), footprint->layer,
            radius.value(), limits.value().max_speed, limits.value().rate_of_turn, footprint->radius,
            footprint->asteroid_damage, live.state.order.through_hazards});
        if (auto built = world_for(layer_of(live), mapping); !built) return built;
    }
    const auto slots = [&] {
        core::tick_profile::Scope mapping_scope("serial.commands.group-mapping");
        return map_group_move(members, destination, mapping, frame, *table.avoidance);
    }();
    if (!slots) return core::Result<void>::failure(slots.error());
    for (std::size_t rank = 0; rank < slots.value().size(); ++rank) {
        const auto& slot = slots.value()[rank];
        if (slot.delay == 0) continue;
        auto& live = staged.at(slot.entity);
        live.formation = FormationWait{frame + slot.delay, slot.destination, slot.max_speed, command.key,
            static_cast<std::uint32_t>(rank), false};
        if (auto submitted = submit(slot.entity, live, layer_of(live), live.formation->frame); !submitted) {
            return submitted;
        }
    }
    for (std::size_t rank = 0; rank < slots.value().size(); ++rank) {
        const auto& slot = slots.value()[rank];
        if (slot.delay != 0) continue;
        pending.push_back({slot.entity, slot.destination, slot.max_speed, layer_of(staged.at(slot.entity)), command.key,
            static_cast<std::uint32_t>(rank), false});
    }
    return core::Result<void>::success();
}

// OR-05, OR-06: a unit plans towards the approach slot the orders phase mapped, serially in
// ascending ID. A unit a hit destroyed this tick is gone.
// Plans `live` towards the slot from this frame and keeps the mapping's prediction frame. A
// tracked unit's search is a job of its lane (PC-07, PC-08), queued in the same order.
core::Result<void> session_detail::Tick::PlanLanes::plan_approach(const EntityId unit_id, LiveUnit& live, const Impl::ApproachPlan& plan) {
    const auto& frame = phase.tracked_.value().frame.value();
    auto& tracking = phase.tracked_.value().tracking.value();
    const auto& table = phase.impl_->motion;
    if (live.formation && live.formation->failure_frame) return core::Result<void>::success();
    live.formation.reset();
    const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
    std::optional<std::size_t> layer;
    if (footprint != nullptr) layer = dynamic_layer_index(footprint->layer);
    live.approach = Approach{plan.prediction_frame};
    if (tracking && layer) {
        pending.push_back({unit_id, plan.slot, std::nullopt, layer.value_or(0), CommandKey{}, 0, false});
        return core::Result<void>::success();
    }
    if (auto planned = phase.impl_->plan_path(live, plan.slot, frame, nullptr, std::nullopt, nullptr); !planned) return planned;
    if (layer && live.motion) return submit(unit_id, live, *layer, std::nullopt);
    return core::Result<void>::success();
}

// AB-24: a ship whose speed multiplier changed plans its move again from this frame, towards
// the same point, as a move order would (a group member still waiting keeps its wait).
core::Result<void> session_detail::Tick::PlanLanes::replan(const EntityId unit_id) {
    const auto& frame = phase.tracked_.value().frame.value();
    auto& tracking = phase.tracked_.value().tracking.value();
    auto& staged = phase.tracked_.value().staged.value();
    const auto& table = phase.impl_->motion;
    const auto found = staged.find(unit_id);
    if (found == staged.end()) return core::Result<void>::success();
    auto& live = found->second;
    if (!live.motion || live.motion->kind != MotionKind::path || live.formation) return core::Result<void>::success();
    const auto target = live.motion->target;
    const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
    const auto layer = footprint != nullptr ? dynamic_layer_index(footprint->layer) : std::nullopt;
    const bool layered = layer.has_value();
    const auto layer_index = layer.value_or(0);
    // A tracked unit's search is a job of its lane (PC-07, PC-08).
    if (tracking && layered) {
        pending.push_back({unit_id, target, std::nullopt, layer_index, CommandKey{}, 0, false});
        return core::Result<void>::success();
    }
    if (auto planned = phase.impl_->plan_path(live, target, frame, nullptr, std::nullopt, nullptr); !planned) return planned;
    if (layered) return submit(unit_id, live, layer_index, std::nullopt);
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::PlanLanes::queue_targeted_autofire() {
    auto& arrivals = phase.staging_.value().arrivals.value();
    auto& staged = phase.tracked_.value().staged.value();
    if (std::none_of(phase.impl_->abilities.profiles.begin(), phase.impl_->abilities.profiles.end(), [](const auto& profile) {
            return std::any_of(profile.special.begin(), profile.special.end(), [](const auto& nested) {
                return nested.style == SpecialActivationStyle::user_input && attack_target_ability(nested.kind).has_value();
            });
        })) return core::Result<void>::success();
    std::vector<const LiveUnit*> holders;
    for (const auto& [id, unit] : staged) {
        if (unit.abilities && unit.combat && unit.combat->attack_target != invalid_entity_id
            && !arrivals.contains(id)) holders.push_back(&unit);
    }
    if (holders.empty()) return core::Result<void>::success();
    std::vector<std::vector<PlayerCommand>> candidates(holders.size());
    const auto prepared = phase.executor.execute_phase("targeted-autofire", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, holders.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& unit = *holders[i];
            const auto id = unit.state.entity_id;
            const bool departed = std::any_of(phase.impl_->quits.begin(), phase.impl_->quits.end(),
                [&](const PlayerQuit& quit) { return quit.player == unit.state.owner; });
            const bool takeover = phase.impl_->setup.departure_policy
                && std::binary_search(phase.impl_->basic_ai.begin(), phase.impl_->basic_ai.end(), unit.state.owner);
            if (departed && !takeover) continue;
            const auto* profile = phase.impl_->abilities.find(unit.state.type_id);
            const auto target = staged.find(unit.combat->attack_target);
            if (profile == nullptr || target == staged.end() || target->first == id) continue;
            const bool human = phase.impl_->abilities.human(unit.state.owner);
            for (const auto& candidate : profile->special) {
                const auto served = candidate.style == SpecialActivationStyle::user_input
                    ? attack_target_ability(candidate.kind) : std::nullopt;
                if (!served) continue;
                const auto slot = ability_slot(*profile, *served);
                if (!slot) continue;
                const auto& state = unit.abilities->slots[*slot];
                if (state.active || (human && !state.autofire)) continue;
                if (*served != AbilityKind::concentrate_fire) {
                    const auto* beam = phase.impl_->beam_profile(unit, *served);
                    if (beam != &candidate || !phase.impl_->beam_usable(unit, *served, phase.tick)
                        || !phase.impl_->beam_target_valid(unit, target->second, *beam)
                        || !phase.impl_->beam_in_range(unit, target->second, *beam)) continue;
                } else {
                    if (phase.impl_->concentrate_profile(unit) != &candidate) continue;
                    const auto& special = unit.abilities->special.slots[static_cast<std::size_t>(&candidate - profile->special.data())];
                    const auto* target_profile = phase.impl_->combat.find(target->second.state.type_id);
                    if (special.cancelled || unit.abilities->special.service_cancelled || candidate.initially_enabled == false
                        || !ability_ready(profile->abilities[*slot], state, phase.impl_->ability_gate(unit, phase.tick), phase.tick)
                        || !players_hostile(phase.impl_->snapshot_players, unit.state.owner, target->second.state.owner)
                        || target_profile == nullptr
                        || !special_type_matches(candidate.filter, target->second.state.type_id, target_profile->category_bits)) continue;
                }
                // CF and beam candidates of one holder share this key; successful
                // switch-on consumes the pass by unit ID, never by command key (WAB-36).
                candidates[i].push_back(PlayerCommand{synthesised_command_key(phase.tick, unit.state.owner, id), {id},
                    AbilityPayload{*served, AbilityAction::activate, target->first}});
            }
        }
    });
    if (!prepared) return prepared;
    for (auto& holder : candidates) for (auto& candidate : holder)
        autofire_commands.push_back(std::move(candidate));
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::PlanLanes::prepare_paths() {
    const auto& combat_turns = phase.targets_.value().combat_turns.value();
    const auto& approaches = phase.orders_.value().approaches.value();
    auto& replans = phase.tracked_.value().replans.value();
    const auto& frame = phase.tracked_.value().frame.value();
    auto& tracking = phase.tracked_.value().tracking.value();
    auto& staged = phase.tracked_.value().staged.value();
    const auto& table = phase.impl_->motion;
    // A-04: a unit that turns toward its ordered target plans a turn in place as a face order of
    // this tick would, before this tick's commands (which may replace it). A unit a hit
    // destroyed this tick is gone.
    for (const auto& [unit_id, point] : combat_turns) {
        const auto found = staged.find(unit_id);
        if (found == staged.end()) continue;
        auto& live = found->second;
        if (auto planned = phase.impl_->plan_order(live, FacePayload{point}, frame); !planned) {
            return unit_failure(unit_id, planned.error());
        }
        const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
        const auto layer = footprint != nullptr ? dynamic_layer_index(footprint->layer) : std::nullopt;
        if (layer && live.motion) {
            if (auto submitted = submit(unit_id, live, *layer, std::nullopt); !submitted) {
                return unit_failure(unit_id, submitted.error());
            }
        }
    }

    for (const auto& [unit_id, plan] : approaches) {
        const auto found = staged.find(unit_id);
        if (found == staged.end()) continue;
        if (auto planned = plan_approach(unit_id, found->second, plan); !planned) {
            return unit_failure(unit_id, planned.error());
        }
    }

    // PC-08: every sliced search runs one slice, side by side in the `plan-searches` phase; a
    // search that lands this frame runs to its end.
    if (!phase.impl_->searches.empty()) {
        std::vector<std::pair<const EntityId, Impl::SlicedSearch>*> running;
        for (auto& entry : phase.impl_->searches) running.push_back(&entry);
        const auto slice = table.avoidance->search_slice;
        core::tick_profile::prepare_jobs(running.size());
        const auto sliced = phase.executor.execute_phase("plan-searches", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, running.size());
            for (auto index = range.begin; index < range.end; ++index) {
                auto& search = running[index]->second;
                core::tick_profile::JobWork job_work;
                const auto limit = search.frame <= frame ? std::numeric_limits<std::uint64_t>::max() : slice;
                if (core::tick_profile::active_jobs) {
                    job_work.before = search.search.expansions(); job_work.deadline = search.frame; job_work.limit = limit;
                }
                core::tick_profile::JobScope sliced_job_scope(search.frame <= frame ? "job.sliced-landing" : "job.sliced-slice",
                    running[index]->first, &job_work);
                PathSearchStats job_stats;
                static_cast<void>(search.search.run(limit, core::tick_profile::active_jobs ? &job_stats : nullptr));
                if (core::tick_profile::active_jobs) {
                    job_work.after = search.search.expansions(); job_work.children = job_stats.children;
                    job_work.queries = job_stats.queries; job_work.leaves = job_stats.leaves; job_work.narrow = job_stats.narrow;
                }
            }
        });
        if (!sliced) return core::Result<void>::failure(sliced.error());
        // PC-09 (#613): a search that has ended publishes its plan to its unit's layer at once,
        // as FoC's plan would be there from the frame it was made (PC-02); its unit still takes
        // it at the landing. In ascending unit ID.
        for (auto& [unit_id, search] : phase.impl_->searches) {
            if (search.plan || !search.search.ended()) continue;
            const auto found = staged.find(unit_id);
            if (found == staged.end() || !found->second.formation || !found->second.formation->sliced) continue;
            auto plan = search.search.result();
            if (!plan) continue; // the landing reports it
            search.plan = std::move(plan).value();
            if (auto published = publish(unit_id, found->second, search); !published) {
                return unit_failure(unit_id, published.error());
            }
        }
    }

    // FM-08, FM-10, PC-08: waiting ships whose plan is due (staggered group members, and sliced
    // searches that land) plan first, in the order of their commands and then each group's
    // planning order.
    if (tracking) {
        std::vector<std::tuple<CommandKey, std::uint32_t, EntityId>> due;
        for (const auto& [id, live] : staged) {
            if (live.formation && live.formation->frame <= frame) due.emplace_back(live.formation->order, live.formation->rank, id);
        }
        std::sort(due.begin(), due.end());
        for (const auto& entry : due) {
            auto& live = staged.at(std::get<2>(entry));
            const auto wait = *live.formation;
            live.formation.reset();
            const auto sliced = wait.sliced ? phase.impl_->searches.find(std::get<2>(entry)) : phase.impl_->searches.end();
            const bool landing = sliced != phase.impl_->searches.end() && sliced->second.frame == frame;
            pending.push_back({std::get<2>(entry), wait.destination, wait.max_speed,
                *dynamic_layer_index(tracked_footprint(table, live.state.type_id)->layer), wait.order, wait.rank, landing});
        }
    }

    // The replans read the layers and the due waits' new plans: the queued searches plan first.
    if (!replans.empty() && !pending.empty()) {
        if (auto failed = plan_pending()) return unit_failure(failed->first, failed->second);
    }
    for (const auto unit_id : replans) {
        if (auto planned = replan(unit_id); !planned) return unit_failure(unit_id, planned.error());
    }

    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::PlanLanes::prepare_commands() {
    auto& reinforcement_work = phase.gather_.value().reinforcement_work.value();
    auto& ledgers = phase.staging_.value().ledgers.value();
    auto& shares = phase.staging_.value().shares.value();
    auto& tracking = phase.tracked_.value().tracking.value();
    auto& staged = phase.tracked_.value().staged.value();
    const auto& table = phase.impl_->motion;
    // Commands phase: serial, in canonical (tick, player, sequence) order.
    phase.commands_.emplace();
    phase.beacons_ = phase.impl_->beacons;
    // HCB-12d: deadline seam at the agreed command tick, independent of worker count.
    std::erase_if(phase.beacons_, [this](const BeaconState& beacon) { return beacon.expires_tick <= phase.tick; });
    // SAE-10: workers test a whole ring against shared immutable staged collision views.
    // The ordinary command below still rechecks after earlier ordered commands.
    auto& searches = phase.commands_->searches;
    for (const auto& [key, request] : phase.impl_->reinforcement_searches) {
        if (key.tick != phase.tick) continue;
        searches.push_back(Commands::Search{key, request, {}, {}, {}});
    }
    std::array<CollisionWorld, 4> search_worlds;
    std::vector<UnitState> prevention_units;
    if (!searches.empty()) {
        for (const auto& part : phase.gather_->prevention_ids) {
            for (const auto id : part) {
                const auto found = staged.find(id);
                if (found != staged.end()) prevention_units.push_back(found->second.state);
            }
        }
        std::array<bool, 4> prepared{};
        for (const auto& search : searches) {
            const auto& payload = std::get<ReinforcePayload>(phase.impl_->pending.at(search.key).payload);
            const auto* footprint = table.footprint(payload.type);
            auto layer = footprint != nullptr ? footprint->layer : SpaceLayer::none;
            if (layer == SpaceLayer::none && table.squadrons.find_squadron(payload.type)) layer = SpaceLayer::corvette;
            const auto selected = dynamic_layer_index(layer);
            if (tracking && selected && !prepared[*selected]) {
                if (auto built = world_for(*selected, search_worlds[*selected]); !built) return built;
                prepared[*selected] = true;
            }
        }
        const auto searched = phase.executor.execute_phase("reinforcement-search", tick_partition_count,
            [&](const std::size_t partition) {
                const auto range = partition_range(partition, searches.size());
                for (auto index = range.begin; index < range.end; ++index) {
                    auto& search = searches[index];
                    const auto& payload = std::get<ReinforcePayload>(phase.impl_->pending.at(search.key).payload);
                    const auto player = search.key.player_id;
                    const auto* account = phase.impl_->economy.player(player);
                    search.result.next_attempt = search.request.attempt;
                    search.result.position = payload.position;
                    // SAE-03/10: wait for pool/population admission before spending a ring.
                    // These are sparse ledger/share reads; the ordered command rechecks them.
                    const auto ledger = std::find_if(ledgers.begin(), ledgers.end(),
                        [player](const PlayerEconomy& entry) { return entry.player == player; });
                    if (account == nullptr || ledger == ledgers.end() || phase.impl_->outcome
                        || phase.impl_->economy.disabled_types.contains(payload.type)
                        || std::find(ledger->pool.begin(), ledger->pool.end(), payload.type) == ledger->pool.end()) continue;
                    std::int64_t owned = 0;
                    for (const auto& [id, share] : shares) {
                        static_cast<void>(id);
                        if (share.owner == player) owned += share.share;
                    }
                    const auto population = population_count(owned);
                    if (population > account->population_cap
                        || phase.impl_->population_of(payload.type) > account->population_cap - population) continue;
                    const auto* footprint = table.footprint(payload.type);
                    auto layer = footprint != nullptr ? footprint->layer : SpaceLayer::none;
                    if (layer == SpaceLayer::none && table.squadrons.find_squadron(payload.type)) layer = SpaceLayer::corvette;
                    const auto selected = dynamic_layer_index(layer);
                    const auto* collisions = selected && prepared[*selected] ? &search_worlds[*selected] : nullptr;
                    auto attempt = search.request.attempt;
                    const DiagnosticTimer search_timer;
                    if (attempt == 0) {
                        // SAE-10: prevention alone selects the starting radius, even in fog.
                        for (const auto& unit : prevention_units) {
                            if (phase.impl_->allied(unit.owner, player)) continue;
                            const auto* prevention = phase.impl_->economy.prevention_of(unit.type_id);
                            const auto dx = math::detail::unsigned_magnitude(unit.position.x.raw() - payload.position.x.raw());
                            const auto dy = math::detail::unsigned_magnitude(unit.position.y.raw() - payload.position.y.raw());
                            auto distance = math::detail::multiply_u64(dx, dx);
                            static_cast<void>(math::detail::add_magnitude(distance, math::detail::multiply_u64(dy, dy)));
                            const auto radius = static_cast<std::uint64_t>(prevention->radius.raw());
                            if (math::detail::compare(distance, math::detail::multiply_u64(radius, radius)) < 0) {
                                attempt = 11; // first angle at radius 1000
                                break;
                            }
                        }
                    }
                    const auto count = attempt == 0 ? 1U : 10U;
                    search.result.next_attempt = attempt == 0 ? 1U : attempt + 10;
                    search.result.position = payload.position;
                    for (std::uint32_t angle = 0; angle < count; ++angle) {
                        const auto candidate = reinforcement_search_candidate(payload.position, attempt + angle,
                            payload.facing_yaw.value_or(account != nullptr ? account->reinforcement_yaw : math::Fixed{}), phase.impl_->economy.bounds);
                        if (!candidate) break;
                        ++search.result.candidates;
                        search.result.position = *candidate;
                        const auto valid = phase.impl_->placement_valid(player, payload.type, *candidate, staged, phase.tick + 1,
                            collisions, &search.work, &prevention_units, payload.facing_yaw);
                        if (!valid) { search.error = valid.error(); break; }
                        if (valid.value()) {
                            search.result.valid = true;
                            search.result.next_attempt = attempt;
                            break;
                        }
                    }
                    search.work.nanoseconds = search_timer.elapsed_nanoseconds();
                    search.result.predictions = search.work.predictions;
                    search.result.nanoseconds = search.work.nanoseconds;
                }
            });
        if (!searched) return searched;
        for (const auto& search : searches) {
            if (search.error) return core::Result<void>::failure(*search.error);
            reinforcement_work.predictions += search.work.predictions;
            reinforcement_work.collision_queries += search.work.collision_queries;
            reinforcement_work.nanoseconds += search.work.nanoseconds;
            for (std::size_t gate = 0; gate < search.work.rejections.size(); ++gate) {
                reinforcement_work.rejections[gate] += search.work.rejections[gate];
            }
        }
    }
    phase.commands_.value().diagnostics.emplace();
    // HCB-59: ready routes re-enter the existing group planner in stable order.
    // These are retained simulation transitions, not new player commands or replay events.
    if (!phase.orders_->waypoint_ready.empty()) {
        waypoint_legs.emplace();
        std::map<std::pair<CommandKey, CommandKey>, PlayerCommand> next_legs;
        for (const auto id : phase.orders_->waypoint_ready) {
            const auto found = staged.find(id);
            if (found == staged.end() || !found->second.waypoints) continue;
            auto& route = *found->second.waypoints;
            if (route.active && !route.points.empty()) {
                const auto completed = route.points.front();
                route.points.erase(route.points.begin());
                if (completed.repeat) route.points.push_back(completed);
            }
            if (route.points.empty()) { route.active = false; continue; }
            route.active = true;
            const auto& next = route.points.front();
            auto [leg, inserted] = next_legs.try_emplace(std::pair{route.coordinator.value_or(next.key), next.key},
                PlayerCommand{{phase.tick, found->second.state.owner, next.key.sequence}, {}, next.move});
            static_cast<void>(inserted);
            leg->second.units.push_back(id);
        }
        for (auto& [key, leg] : next_legs) {
            static_cast<void>(key);
            waypoint_legs->push_back(std::move(leg));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::PlanLanes::finish() {
    auto& minds = phase.craft_prep_.value().minds.value();
    auto& crafts = phase.staging_.value().crafts.value();
    auto& arrivals = phase.staging_.value().arrivals.value();
    auto& ledgers = phase.staging_.value().ledgers.value();
    auto& pad_requests = phase.staging_.value().pad_requests.value();
    auto& shares = phase.staging_.value().shares.value();
    auto& arrived_squadrons = phase.staging_.value().arrived_squadrons.value();
    auto& earners = phase.staging_.value().earners.value();
    auto& next_id = phase.staging_.value().next_id.value();
    const auto& frame = phase.tracked_.value().frame.value();
    auto& tracking = phase.tracked_.value().tracking.value();
    auto& staged = phase.tracked_.value().staged.value();
    auto& events = phase.impacts_.value().events.value();
    const auto& squadron_table = phase.impl_->motion.squadrons;
    auto& pads = phase.impl_->pad_stage.values;
    auto& construction = phase.impl_->construction_stage.values;
    const auto& table = phase.impl_->motion;
    const bool has_hunt = std::any_of(phase.impl_->abilities.profiles.begin(), phase.impl_->abilities.profiles.end(),
        [](const UnitAbilityProfile& profile) { return ability_slot(profile, AbilityKind::hunt).has_value(); });
    if (has_hunt) {
        // WAB-50..56: immutable inputs, partitioned destination decisions, ordered move commit.
        // The serial pass captures pointers only; all per-hunter service work runs in the workers.
        std::vector<const LiveUnit*> hunt_inputs;
        hunt_inputs.reserve(staged.size());
        for (const auto& [id, live] : staged) { static_cast<void>(id); hunt_inputs.push_back(&live); }
        struct HuntMove { EntityId unit{}; math::Vec3 destination{}; };
        std::vector<std::optional<HuntMove>> hunt_moves(hunt_inputs.size());
        std::vector<std::optional<core::Diagnostic>> hunt_errors(hunt_inputs.size());
        const auto& membership = phase.craft_prep_->craft_squadron.value();
        const auto selected = phase.executor.execute_phase("hunt", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, hunt_inputs.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& hunter = *hunt_inputs[index];
                if (!hunter.abilities) continue;
                const auto* profile = phase.impl_->abilities.find(hunter.state.type_id);
                const auto slot = ability_slot(*profile, AbilityKind::hunt);
                if (!slot || !hunter.abilities->slots[*slot].active) continue;
                const auto& active = hunter.abilities->slots[*slot];
                if ((phase.tick - active.started_tick) % 30 != 0) continue;
                EntityId mover = hunter.state.entity_id;
                const auto member = membership.find(mover);
                const auto container = member != membership.end() ? member->second : mover;
                const auto mind = minds.find(container);
                if (mind != minds.end()) {
                    EntityId leader{};
                    for (const auto id : mind->second.roster) {
                        if (staged.find(id) != staged.end()) { leader = id; break; }
                    }
                    if (mover != leader) continue;
                    // WAB-53 / FM-24: an arrived escort holds a fighter-idle cell;
                    // the order's escort mode alone does not mean it is moving.
                    const bool idle = mind->second.mode == SquadronMode::idle
                        || (mind->second.mode == SquadronMode::escort && mind->second.idle_cell.has_value());
                    if (!idle || mind->second.target != invalid_entity_id) continue;
                    mover = container;
                } else if (!hunter.motion || hunter.motion->kind != MotionKind::none || hunter.formation) continue;
                const auto flight = crafts.find(hunter.state.entity_id);
                if (flight != crafts.end() && (flight->second.chase != invalid_entity_id || flight->second.flipping)) continue;
                if ((hunter.combat && hunter.combat->attack_target != invalid_entity_id)
                    || phase.impl_->ability_gate(hunter, phase.tick).in_nebula || std::any_of(arrived_squadrons.begin(), arrived_squadrons.end(), [&](const Squadron& value) { return value.container == container; })
                    || arrivals.contains(container)) continue;
                const auto player = std::find_if(phase.impl_->setup.players.begin(), phase.impl_->setup.players.end(),
                    [&](const Player& value) { return value.player_id == hunter.state.owner; });
                const auto player_index = static_cast<std::size_t>(player - phase.impl_->setup.players.begin());
                const auto fogged = [&](const math::Vec3 point) {
                    return phase.impl_->fog && !phase.impl_->fog->revealed(player_index, point);
                };
                std::vector<HuntEnemy> enemies;
                for (const auto* candidate : hunt_inputs) {
                    if (!players_hostile(phase.impl_->snapshot_players, hunter.state.owner, candidate->state.owner)) continue;
                    const auto* footprint = phase.impl_->motion.footprint(candidate->state.type_id);
                    if (!candidate->motion && crafts.find(candidate->state.entity_id) == crafts.end()
                        && !(footprint && footprint->locomotor)) continue;
                    const auto* enemy_profile = phase.impl_->abilities.find(candidate->state.type_id);
                    enemies.push_back({candidate->state.position, fogged(candidate->state.position), enemy_profile && enemy_profile->force_sensitive});
                    if (enemies.size() == 128) break;
                }
                // WAB-54: a craft's authored range applies even when it has no REVEAL service.
                const auto reveal_range = profile->hunt_reveal_range;
                CombatRandom random(phase.impl_->setup.seed, phase.tick, hunter.state.entity_id, 0xfffe0006U); // coordinator-reserved, WAB-54
                auto destination = hunt_destination(hunter.state.position, phase.impl_->economy.bounds, reveal_range,
                    profile->force_sensitive, enemies, fogged, random);
                if (!destination) hunt_errors[index] = destination.error();
                else hunt_moves[index] = HuntMove{mover, destination.value()};
            }
        });
        if (!selected) return selected;
        for (std::size_t index = 0; index < hunt_moves.size(); ++index) {
            if (hunt_errors[index]) return unit_failure(hunt_inputs[index]->state.entity_id, *hunt_errors[index]);
            if (!hunt_moves[index]) continue;
            const auto& move = *hunt_moves[index];
            auto& live = staged.at(move.unit);
            const CommandPayload payload = AttackMovePayload{move.destination};
            live.state.order = order_for(payload, phase.tick);
            if (minds.find(move.unit) != minds.end()) {
                apply_squadron_order(minds.at(move.unit), payload, live.state.position, squadron_table);
                if (auto spread = spread_squadrons({move.unit}); !spread) return spread;
            } else {
                const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
                std::optional<std::size_t> layer;
                if (footprint) layer = dynamic_layer_index(footprint->layer);
                if (tracking && layer) {
                    pending.push_back({move.unit, move.destination, std::nullopt, *layer, CommandKey{}, 0, false});
                } else {
                    if (auto planned = phase.impl_->plan_order(live, MovePayload{move.destination}, frame); !planned) return planned;
                    if (layer && live.motion) {
                        if (auto submitted = submit(move.unit, live, *layer, std::nullopt); !submitted) return submitted;
                    }
                }
            }
        }
    }

    // WLU-07: an expired LURE switches off like the button (WAB-11): its squadron's move stops.
    {
        const auto& membership = phase.craft_prep_->craft_squadron.value();
        std::vector<EntityId> stops;
        for (const auto id : phase.tracked_.value().lure_ends.value()) {
            const auto member = membership.find(id);
            stops.push_back(member != membership.end() ? member->second : id);
        }
        std::sort(stops.begin(), stops.end());
        stops.erase(std::unique(stops.begin(), stops.end()), stops.end());
        for (const auto container_id : stops) {
            const auto found = staged.find(container_id);
            if (found == staged.end()) continue;
            auto& live = found->second;
            live.state.order = order_for(StopPayload{}, phase.tick);
            if (minds.find(container_id) != minds.end()) {
                apply_squadron_order(minds.at(container_id), StopPayload{}, live.state.position, squadron_table);
            } else if (live.motion) {
                if (auto stopped = phase.impl_->plan_order(live, StopPayload{}, frame); !stopped) return stopped;
                const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
                if (footprint) {
                    if (const auto layer = dynamic_layer_index(footprint->layer)) {
                        if (auto submitted = submit(container_id, live, *layer, std::nullopt); !submitted) return submitted;
                    }
                }
            }
        }
    }

    const bool has_lure = std::any_of(phase.impl_->abilities.profiles.begin(), phase.impl_->abilities.profiles.end(),
        [](const UnitAbilityProfile& profile) { return ability_slot(profile, AbilityKind::lure).has_value(); });
    if (has_lure) {
        // WLU-03..WLU-06: each luring craft's pulse reads the start-of-phase world and lists the
        // hostile top-level objects it lures (partitioned, one disjoint slot per craft); the orders
        // are committed serially in ascending craft order, so the last craft to pulse wins (WLU-06).
        std::vector<const LiveUnit*> lure_inputs;
        lure_inputs.reserve(staged.size());
        for (const auto& [id, live] : staged) { static_cast<void>(id); lure_inputs.push_back(&live); }
        std::vector<std::vector<EntityId>> lured(lure_inputs.size());
        const auto& membership = phase.craft_prep_->craft_squadron.value();
        const auto top_level = [&](const EntityId id) {
            const auto member = membership.find(id);
            return member != membership.end() ? member->second : id;
        };
        const auto pulsed = phase.executor.execute_phase("lure", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, lure_inputs.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& lurer = *lure_inputs[index];
                if (!lurer.abilities) continue;
                const auto& profile = *phase.impl_->abilities.find(lurer.state.type_id);
                const auto slot = ability_slot(profile, AbilityKind::lure);
                if (!slot || !lurer.abilities->slots[*slot].active) continue;
                const auto& lure = *profile.abilities[*slot].lure;
                if (!lure_pulses(lure, lurer.abilities->slots[*slot].started_tick, phase.tick)) continue;
                auto& out = lured[index];
                for (const auto* candidate : lure_inputs) {
                    // WLU-04: listed exact types within the radius of the craft (3D, inclusive).
                    if (!std::binary_search(lure.target_types.begin(), lure.target_types.end(), candidate->state.type_id)
                        || !within_range(lurer.state.position, candidate->state.position, lure.radius, RangeMetric::spatial)) continue;
                    // WLU-05: a team member stands for its team; only enemies of the craft's owner.
                    const auto target = staged.find(top_level(candidate->state.entity_id));
                    if (target == staged.end()
                        || !players_hostile(phase.impl_->snapshot_players, lurer.state.owner, target->second.state.owner)) continue;
                    out.push_back(target->first);
                }
                std::sort(out.begin(), out.end());
                out.erase(std::unique(out.begin(), out.end()), out.end());
            }
        });
        if (!pulsed) return pulsed;
        for (std::size_t index = 0; index < lured.size(); ++index) {
            const auto lurer = lure_inputs[index]->state.entity_id;
            const auto lurer_team = top_level(lurer);
            const auto* team_profile = staged.find(lurer_team) != staged.end()
                ? phase.impl_->combat.find(staged.at(lurer_team).state.type_id) : nullptr;
            // WCC-25: an invalid target type is never assigned.
            if (team_profile != nullptr && !team_profile->valid_target) continue;
            for (const auto target_id : lured[index]) {
                auto& target = staged.at(target_id);
                const auto mind = minds.find(target_id);
                // WLU-06: an uninterruptible move (a hyperspace arrival) and a team already
                // attacking this craft are left alone. A lone listed object outside a team is not
                // lured (none in the M2 rosters; WLU-10).
                if (arrivals.contains(target_id) || mind == minds.end()
                    || (target.state.order.kind == OrderKind::attack && target.state.order.target == lurer)) continue;
                const CommandPayload attack = AttackPayload{lurer};
                target.state.order = order_for(attack, phase.tick);
                // WSQ-47: the team's destination names the luring craft's team.
                apply_squadron_order(minds.at(target_id), AttackPayload{lurer_team}, target.state.position, squadron_table);
            }
        }
    }

    if (!pending.empty()) {
        if (auto failed = plan_pending()) return unit_failure(failed->first, failed->second);
    }
    const auto first_pad_added = next_id;
    for (const auto& request : pad_requests) {
        Impl::EconomyStage stage{ledgers, arrivals, shares, crafts, minds, arrived_squadrons,
            phase.staging_.value().born_spawners, earners, next_id,
            pads, construction, pad_requests, phase.impl_->pad_stage, phase.impl_->construction_stage, phase.pending_economy_cues_};
        const auto result = phase.impl_->execute_economy(request, phase.tick, staged, stage, events, nullptr, true);
        if (!result) return core::Result<void>::failure(result.error());
    }
    phase.sync_radar_creations(staged, first_pad_added, next_id);
    // PC-08: a sliced search whose unit is gone or no longer waits for it (a new move, face or
    // stop, FM-10) is dropped with its copied views.
    for (auto iterator = phase.impl_->searches.begin(); iterator != phase.impl_->searches.end();) {
        const auto unit = staged.find(iterator->first);
        const bool waits = unit != staged.end() && unit->second.formation && unit->second.formation->sliced
            && unit->second.formation->frame == iterator->second.frame;
        iterator = waits ? std::next(iterator) : phase.impl_->searches.erase(iterator);
    }

    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::PlanLanes::scripted_damage(const PlayerCommand& command, const DamagePayload* damage, const EntityId unit_id, const DurabilityProfile* profile) {
    const auto& damage_rules = phase.flights_.value().damage_rules.value();
    auto& rebuilt = phase.tracked_.value().rebuilt.value();
    auto& tracking = phase.tracked_.value().tracking.value();
    auto& staged = phase.tracked_.value().staged.value();
    auto& events = phase.impacts_.value().events.value();
    auto& killed = phase.impacts_.value().killed.value();
    const auto& table = phase.impl_->motion;
    const auto unit = staged.find(unit_id);
    if (phase.damage_blocked()) return core::Result<void>::success();
    // With damage rules the shield absorbs scripted damage first (DG-20); without, it is raw.
    // WR-33/WHE-51: privileged Lua damage bypasses arrival immunity, but keeps the take mode.
    // WHE-64: LuaDebugDamage distributes raw shares first, then also damages the leader.
    Hit routed_hit{damage->amount, no_type_index, false, true, true, hull_target};
    const auto routed = phase.redirect_damage(unit_id, routed_hit, [&](const EntityId recipient_id, Hit hit) -> core::Result<void> {
        if (phase.damage_blocked()) return core::Result<void>::success();
        const auto recipient = staged.find(recipient_id);
        if (recipient == staged.end() || !recipient->second.durability) return core::Result<void>::success();
        const auto* recipient_profile = phase.impl_->base_profile(recipient->second.state.owner, recipient->second.state.type_id);
        if (!recipient_profile) return core::Result<void>::success();
        auto& live = recipient->second;
        const auto hull_before = live.durability->hull;
        const auto shields_before = live.durability->shields;
        hit.take_damage_multiplier = live.take_damage_mode;
        DamageOutcome redirected;
        if (damage_rules) {
            const auto applied = apply_hit(*recipient_profile, *damage_rules, *live.durability, hit, phase.tick);
            if (!applied) return core::Result<void>::failure(applied.error());
            redirected = applied.value().damage;
            phase.impl_->end_depleted_defend(live, phase.tick, applied.value().storm_shield_branch);
        } else {
            const auto amount = math::multiply(hit.amount, hit.take_damage_multiplier);
            if (!amount) return core::Result<void>::failure(amount.error());
            redirected = apply_damage(*recipient_profile, *live.durability, hull_target, amount.value());
        }
        phase.impl_->track_damage(live, hull_before, shields_before);
        if (redirected.unit_destroyed) {
            events.push_back(phase.destruction_event(phase.tick, EventKind::unit_destroyed, live.state, 0, command.key.player_id));
            killed.push_back(live.state);
            if (const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr) {
                if (const auto layer = dynamic_layer_index(footprint->layer)) {
                    rebuilt[*layer] = true;
                    if (tracking) tracking->views[*layer].reset();
                } else if (footprint->layer == SpaceLayer::static_object && tracking) tracking->statics.reset();
                if (tracking) tracking->samples.erase(recipient_id);
            }
            phase.impl_->adjust_owned(live.state, false);
            staged.erase(recipient);
        }
        return core::Result<void>::success();
    });
    if (!routed) return core::Result<void>::failure(routed.error());
    if (phase.damage_blocked()) return core::Result<void>::success();
    auto outcome = DamageOutcome{};
    const auto hull_before = unit->second.durability->hull;
    const auto shields_before = unit->second.durability->shields;
    if (damage_rules != nullptr) {
        Hit hit{damage->amount, no_type_index, false, true, true, damage->hardpoint};
        hit.take_damage_multiplier = unit->second.take_damage_mode;
        auto applied = apply_hit(*profile, *damage_rules, *unit->second.durability, hit, phase.tick);
        if (!applied) {
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                command_context(command.key) + " unit " + std::to_string(unit_id) + ": "
                    + applied.error().message));
        }
        phase.impl_->end_depleted_defend(unit->second, phase.tick, applied.value().storm_shield_branch);
        outcome = applied.value().damage;
    } else {
        const auto amount = math::multiply(damage->amount, unit->second.take_damage_mode);
        if (!amount) return core::Result<void>::failure(amount.error());
        outcome = apply_damage(*profile, *unit->second.durability, damage->hardpoint, amount.value());
    }
    // WCC-54/82: raw scripted damage is tracked even without the optional damage rules.
    phase.impl_->track_damage(unit->second, hull_before, shields_before);
    if (outcome.destroyed_hardpoint) {
        events.push_back(phase.destruction_event(
            phase.tick, EventKind::hardpoint_destroyed, unit->second.state, *outcome.destroyed_hardpoint));
    }
    if (outcome.unit_destroyed) {
        events.push_back(phase.destruction_event(phase.tick, EventKind::unit_destroyed, unit->second.state, 0, command.key.player_id));
        killed.push_back(unit->second.state);
        // A tracked unit leaving its layer rebuilds it (AV-03): a later order this tick
        // plans without it (AV-15).
        if (const auto* footprint = table.avoidance ? tracked_footprint(table, unit->second.state.type_id) : nullptr) {
            if (const auto layer = dynamic_layer_index(footprint->layer)) {
                rebuilt[*layer] = true;
                if (tracking) tracking->views[*layer].reset();
            } else if (footprint->layer == SpaceLayer::static_object && tracking) {
                tracking->statics.reset();
            }
            if (tracking) tracking->samples.erase(unit_id);
        }
        phase.impl_->adjust_owned(unit->second.state, false);
        staged.erase(unit);
    }
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::PlanLanes::apply_cinematic(const PlayerCommand& command, const RetreatCinematicPayload* cinematic, const PlayerId issuer) {
    auto& events = phase.impacts_.value().events.value();
    const bool allowed = phase.impl_->retreat && phase.pending_retreat_.phase == RetreatPhase::escaping
        && phase.pending_retreat_.player == issuer && !phase.pending_outcome_ && cinematic->frame == 0;
    if (allowed) {
        // Reports never advance time, even when several arrive in one tactical frame.
        phase.pending_retreat_.playback_reported = phase.pending_retreat_.playback_reported || cinematic->playing;
        phase.pending_retreat_.playback_playing = cinematic->playing;
    }
    events.push_back(Event{phase.tick, allowed ? EventKind::order_accepted : EventKind::order_rejected,
        issuer, command.key.sequence, 0, OrderKind::retreat, allowed ? RejectReason::none : RejectReason::ability_unavailable});
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::PlanLanes::apply_retreat(const PlayerCommand& command, const RetreatPayload* retreat, const PlayerId issuer) {
    auto& events = phase.impacts_.value().events.value();
    auto& diagnostics = phase.commands_.value().diagnostics.value();
    auto refusal = RetreatRefusal::wrong_mode;
    if (phase.impl_->retreat) {
        auto gate = phase.impl_->retreat_gate(issuer); gate.battle_decided = phase.pending_outcome_.has_value();
        if (retreat->action == RetreatAction::auto_resolve) diagnostics.push_back(detail::diagnostic(
            diagnostic_codes::command_rejected, "mid-battle solve requires the native interleaved death service (WAR-41/45)", {}, core::Severity::warning));
        else if (phase.impl_->retreat->flight_qualified || retreat->action == RetreatAction::cancel)
            refusal = request_retreat(phase.impl_->retreat->rules, phase.pending_retreat_, issuer, retreat->action,
            phase.tick, gate, phase.impl_->retreat_actors());
    }
    events.push_back(Event{phase.tick, refusal == RetreatRefusal::none ? EventKind::order_accepted : EventKind::order_rejected,
        issuer, command.key.sequence, 0, OrderKind::retreat,
        refusal == RetreatRefusal::none ? RejectReason::none : RejectReason::ability_unavailable});
    if (refusal != RetreatRefusal::none) diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
        "campaign retreat refused: " + std::to_string(static_cast<unsigned>(refusal)), {}, core::Severity::warning));
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::PlanLanes::apply_beacon(const PlayerCommand& command, const BeaconPayload* beacon, const PlayerId issuer) {
    auto& events = phase.impacts_.value().events.value();
    const auto rule = std::lower_bound(phase.impl_->setup.beacon_rules.begin(), phase.impl_->setup.beacon_rules.end(), issuer,
        [](const auto& entry, const PlayerId player) { return entry.player < player; });
    const bool allowed = rule != phase.impl_->setup.beacon_rules.end() && rule->player == issuer;
    if (allowed) {
        std::erase_if(phase.beacons_, [issuer](const auto& previous) { return previous.player == issuer; });
        const auto at = std::lower_bound(phase.beacons_.begin(), phase.beacons_.end(), issuer,
            [](const auto& entry, const PlayerId player) { return entry.player < player; });
        phase.beacons_.insert(at, BeaconState{issuer, rule->type, beacon->position, phase.tick,
            phase.tick + rule->lifetime_frames, command.key.sequence});
    }
    events.push_back(Event{phase.tick, allowed ? EventKind::order_accepted : EventKind::order_rejected,
        issuer, command.key.sequence, invalid_entity_id, OrderKind::beacon,
        allowed ? RejectReason::none : RejectReason::beacon_unavailable});
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::PlanLanes::apply_reveal(const PlayerCommand& command, const RevealAllPayload* reveal, const PlayerId issuer) {
    auto& events = phase.impacts_.value().events.value();
    phase.pending_reveals_.push_back(reveal->player);
    events.push_back(Event{phase.tick, EventKind::order_accepted, issuer, command.key.sequence,
        invalid_entity_id, OrderKind::reveal_all});
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
