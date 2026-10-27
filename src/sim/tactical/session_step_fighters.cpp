#include "eawr/core/load_profile.hpp"
#include "eawr/sim/tactical/session.hpp"

#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/pathfind.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "blast_internal.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "orders_internal.hpp"
#include "staging.hpp"
#include "session_tick.hpp"
#include "session_services.hpp"
#include "tactical_internal.hpp"

#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
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


bool session_detail::Tick::squadron_refresh_skips(const LiveUnit*& target, const CraftProfile*& craft, const SquadronState& state,
    detail::MapStage<SquadronState>& minds, SquadronOutcome& outcome, const EntityId& target_id,
    const std::uint64_t& frame, const LiveUnit*& leader, const detail::CombatWorld& combat_world,
    std::vector<const LiveUnit*>& attack_leaders, std::size_t& index, const LiveUnit*& container,
    detail::MapStage<CraftState>& crafts, auto& finish_base) {
    if (target != nullptr && craft != nullptr && (state.attack_path || state.approach)) {
        const auto& retained = outcome.attack_path ? outcome.attack_path : state.attack_path;
        if (retained && retained->target == target_id && frame < retained->next_refresh_frame
            && !squadron_attack_initialization_due(*retained, frame)) {
            finish_base();
            return true;
        }
        // FSC-72/73: copied post-object observations and staged path work.
        // No worker writes the shared squadron state or re-runs a flight.
        SquadronAttackPath path = outcome.attack_path ? *outcome.attack_path
            : state.attack_path.value_or(SquadronAttackPath{});
        if (!state.attack_path) {
            // FC-44: constructor clock, independent of target scans.
            CombatRandom draw(impl_->setup.seed, frame, state.container, 0xfffe000fU);
            const auto fps = logical_frames_per_second;
            path.next_refresh_frame = frame + fps + draw.uniform(1, fps);
        }
        if (path.target != target_id) {
            path.target = target_id;
            path.initialization_pending = true;
        }
        // CombatWorld retains copied geometry/visibility, but its
        // health pointers belonged to the earlier movement stage.
        // Rebind them to immutable survivors before this later service.
        std::optional<detail::CombatUnit> target_geometry;
        if (const auto* prior = combat_world.find(target_id)) {
            target_geometry = *prior;
            target_geometry->position = target->state.position;
            auto transform = unit_transform(*target);
            if (!transform) { outcome.error = transform.error(); return true; }
            target_geometry->transform = transform.value();
            target_geometry->durability_profile = impl_->health_profile(*target);
            target_geometry->durability = target->durability ? &*target->durability : nullptr;
            target_geometry->combat = target->combat ? &*target->combat : nullptr;
        }
        const auto* target_view = target_geometry ? &*target_geometry : nullptr;
        const auto* leader_view = combat_world.find(leader->state.entity_id);
        auto attack_point = target->state.position;
        // OR-24 initializes a never-chosen destination on the order tick.
        // A destroyed retained index reads as the centre until replacement.
        auto* attachment = target_view != nullptr
            ? detail::standing_hardpoint(*target_view, state.formation->attack_hardpoint) : nullptr;
        if (target_view != nullptr && state.formation->attack_hardpoint == attack_hull) {
            const auto* selector = attack_leaders[index] != nullptr ? attack_leaders[index] : leader;
            attachment = detail::best_hardpoint(&impl_->combat, impl_->combat.find(selector->state.type_id),
                *target_view, selector->state.position);
            if (attachment != nullptr) outcome.attack_hardpoint = attachment->hardpoint;
        }
        if (attachment != nullptr) {
            auto point = math::transform_point(target_view->transform, attachment->position);
            if (!point) { outcome.error = point.error(); return true; }
            attack_point = point.value();
        }
        auto reach = core::Result<math::Fixed>::success(impl_->approach_range(*leader, false, target_view));
        if (target_view != nullptr) {
            reach = detail::target_attack_distance(&impl_->motion, *target_view, leader->state.position,
                impl_->attack_distance(*leader).value_or(math::Fixed{}),
                detail::has_aim_hardpoint(*target_view));
        }
        if (!reach) { outcome.error = reach.error(); return true; }
        SquadronAttackRefresh observation;
        observation.target_position = attack_point;
        observation.effective_reach = reach.value();
        // WMV-18: an idle lost-base identity is provenance only;
        // it is not a live escort input to attack-path refresh.
        observation.escorted_destination = state.mode == SquadronMode::escort
            ? state.escorted : invalid_entity_id;
        observation.in_attack_reach = within_range(leader->state.position, attack_point,
            reach.value(), RangeMetric::planar);
        const auto flight = crafts.find(leader->state.entity_id);
        observation.pursuing = flight != crafts.end() && flight->second.chase_until > frame;
        observation.completed_or_stopping = outcome.attack_approach == false
            || (flight != crafts.end() && flight->second.velocity == math::Vec3{});
        if (const auto theirs = minds.find(target_id); theirs != minds.end())
            observation.shared_cell = state.cell && theirs->second.cell && *state.cell == *theirs->second.cell;
        if (target_view != nullptr && leader_view != nullptr && target_view->team != leader_view->team)
            observation.visible = !target_view->in_limbo && !target_view->stealth_invisible
                && (target_view->visible_to & (std::uint64_t{1} << leader_view->player_index)) != 0;
        observation.repath_permitted = !leader->abilities
            || !ability_movement_disabled(*impl_->abilities.find(leader->state.type_id), *leader->abilities);
        observation.protected_destination = path.collision_identity != 0
            && path.collision_identity != state.container;
        // Full moving-escort copying/guard threshold, per-member mapping
        // and exceptional attack-positioning producers remain UFS-34/36.
        const bool refresh = refresh_squadron_attack_path(path, frame, observation);
        const bool initialize = squadron_attack_initialization_due(path, frame);
        const bool search_permitted = observation.visible && observation.repath_permitted
            && !observation.shared_cell && !observation.protected_destination
            && (!observation.in_attack_reach || observation.pursuing);
        if ((!initialize && (!refresh || path.initialization_pending)) || !search_permitted) {
            if (!state.attack_path || path != *state.attack_path) outcome.attack_path = std::move(path);
            finish_base();
            return true;
        }
        // FS-19/40: search reads the currently published container
        // pose, not the new member bounds/facing staged for commit.
        auto yaw = yaw_degrees(container->state.rotation);
        if (!yaw) { outcome.error = yaw.error(); return true; }
        auto speed = flight != crafts.end() ? math::length(flight->second.velocity)
            : core::Result<math::Fixed>::success(math::Fixed{});
        if (!speed) { outcome.error = speed.error(); return true; }
        auto route = plan_squadron_attack_route(*craft, impl_->motion.rules, state.container,
            outcome.members, frame, container->state.position, yaw.value(), speed.value(), attack_point,
            math::Vec3{container->placement_cache->matrix.rows[0][0], container->placement_cache->matrix.rows[1][0],
                container->placement_cache->matrix.rows[2][0]});
        if (!route) { outcome.error = route.error(); return true; }
        if (route.value().nodes.size() < 2) {
            // FC-68: an exhausted/empty attempt retains the accepted
            // destination and failure history, never marks it arrived.
            if (initialize) path.failure_frame = frame;
        } else if (install_squadron_attack_path(path, std::move(route).value(), frame)
            == SquadronPathAdmission::active) {
            path.cached_position = attack_point;
            path.escorted_destination = observation.escorted_destination;
            path.initialization_pending = false;
            outcome.attack_approach = true;
        }
        outcome.attack_path = std::move(path);
    }
    return false;
}


void session_detail::Tick::prepare_squadron_outcomes_partition(const std::size_t& partition,
    std::vector<const Squadron*>& squadron_inputs, std::vector<SquadronOutcome>& outcomes, auto& find,
    detail::MapStage<SquadronState>& minds, std::vector<std::pair<std::size_t, PlayerId>>& final_killers,
    const SquadronTable& squadron_table, std::vector<std::uint8_t>& completed_bases,
    const std::vector<std::pair<EntityId, math::Vec3>>& movement_starts,
    const std::vector<std::optional<CraftStep>>& movement_steps, const detail::CombatWorld& combat_world,
    std::vector<const LiveUnit*>& attack_leaders, detail::MapStage<CraftState>& crafts,
    const std::uint64_t& frame) {
    const auto range = partition_range(partition, squadron_inputs.size());
    for (auto index = range.begin; index < range.end; ++index) {
        auto& outcome = outcomes[index];
        const auto* container = find(squadron_inputs[index]->container);
        outcome.container_live = container != nullptr;
        math::Vec3 low{};
        math::Vec3 high{};
        for (const auto member : squadron_inputs[index]->members) {
            const auto* craft = find(member);
            if (craft == nullptr) {
                if (const auto death = loss_killers_.find(member); death != loss_killers_.end()
                    && death->second.first > final_killers[index].first) final_killers[index] = death->second;
                continue;
            }
            const auto& at = craft->state.position;
            if (outcome.members.empty()) {
                low = at;
                high = at;
            } else {
                low = {std::min(low.x, at.x), std::min(low.y, at.y), std::min(low.z, at.z)};
                high = {std::max(high.x, at.x), std::max(high.y, at.y), std::max(high.z, at.z)};
            }
            outcome.members.push_back(member);
        }
        if (container == nullptr || outcome.members.empty()) {
            continue;
        }
        outcome.centre = {math::Fixed::from_raw(midpoint(low.x.raw(), high.x.raw())),
            math::Fixed::from_raw(midpoint(low.y.raw(), high.y.raw())),
            math::Fixed::from_raw(midpoint(low.z.raw(), high.z.raw()))};
        // FM-26 (#687, walk 1 WSQ-50): a squadron holding an idle cell has its container
        // on the cell's point.
        const bool solo = squadron_inputs[index]->members.size() == 1
            && squadron_inputs[index]->members.front() == squadron_inputs[index]->container;
        if (const auto mind = minds.find(squadron_inputs[index]->container);
            !solo && mind != minds.end() && mind->second.idle_cell) {
            outcome.centre = mind->second.anchor;
        }
        outcome.rotation = container->state.rotation;
        outcome.transform = container->placement_cache->matrix;
        // WSQ-53, FM-14 (#741): the container takes its leader's whole facing every
        // frame (the first live craft of the roster, WSQ-51).
        if (!solo) {
            const auto* leader = find(outcome.members.front());
            outcome.rotation = leader->state.rotation;
            if (leader->placement_cache && leader->placement_cache->rotation == outcome.rotation) {
                outcome.transform = leader->placement_cache->matrix;
            } else if (outcome.rotation != container->state.rotation) {
                auto turned = math::to_matrix(outcome.rotation, outcome.centre);
                if (!turned) {
                    outcome.error = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + " squadron " + std::to_string(squadron_inputs[index]->container)
                            + ": " + turned.error().message);
                    continue;
                }
                outcome.transform = turned.value();
            }
        }
        outcome.transform.rows[0][3] = outcome.centre.x;
        outcome.transform.rows[1][3] = outcome.centre.y;
        outcome.transform.rows[2][3] = outcome.centre.z;
        const auto mind = minds.find(squadron_inputs[index]->container);
        if (mind != minds.end() && mind->second.formation) {
            const auto& state = mind->second;
            const auto target_id = state.formation->attack_target;
            const auto* target = find(target_id);
            const auto* leader = find(outcome.members.front());
            const auto* craft = squadron_table.find_craft(leader->state.type_id);
            const auto finish_base = [&] {
                // WMV-11/18: done records history only for the sole base,
                // after ordinary flight; an override cannot complete it.
                if ((state.formation->complete && state.formation->has_reached_done)
                    || state.formation->attack_override || state.mode == SquadronMode::move
                    || outcome.attack_approach.value_or(state.approach)) return;
                // WMV-18 seam: FM-24's arrived escort hold stands in for
                // its base path. A distant escort is not done at birth.
                if (state.mode == SquadronMode::escort && !state.idle_cell) return;
                const auto& path = outcome.attack_path ? outcome.attack_path : state.attack_path;
                if (path) {
                    for (const auto& route : path->stack) {
                        for (const auto member : outcome.members) {
                            const auto tracker = route.trackers.find(member);
                            if (tracker != route.trackers.end() && (route.nodes.size() < 2
                                || tracker->second.segment < route.nodes.size() - 1)) return;
                        }
                    }
                }
                if (state.mode == SquadronMode::idle
                    && state.formation->base_target == invalid_entity_id && !state.idle_cell) {
                    // A lost escort still has an unfinished hold. Losing
                    // its carrier does not make the current position arrived.
                    const auto reach = math::Fixed::from_raw(idle_cell_size * math::Fixed::scale);
                    if (!within_range(leader->state.position, state.anchor, reach, RangeMetric::planar)) return;
                    completed_bases[index] = 2; // ordered FM-24 cell claim before completion
                } else completed_bases[index] = 1;
            };
            if (state.attack_path && !state.attack_path->stack.empty()) {
                const auto& route = state.attack_path->stack.back();
                const auto leader_id = outcome.members.front();
                const auto tracker = route.trackers.find(leader_id);
                std::vector<EntityId> admitted;
                bool new_start = false;
                for (const auto member : outcome.members) {
                    const auto moved = std::lower_bound(movement_starts.begin(), movement_starts.end(), member,
                        [](const auto& start, const EntityId id) { return start.first < id; });
                    if (moved == movement_starts.end() || moved->first != member
                        || !movement_steps[static_cast<std::size_t>(moved - movement_starts.begin())]) continue;
                    admitted.push_back(member);
                    const auto own = route.trackers.find(member);
                    new_start = new_start || (own != route.trackers.end() && !own->second.started);
                }
                const auto segment = std::find(admitted.begin(), admitted.end(), leader_id) != admitted.end()
                    ? squadron_attack_segment(*state.attack_path, leader_id, leader->state.position)
                    : (tracker != route.trackers.end() ? tracker->second.segment : 0);
                if (tracker != route.trackers.end() && (new_start || tracker->second.segment != segment)) {
                    outcome.attack_path = state.attack_path;
                    advance_squadron_attack_path(*outcome.attack_path, leader_id, leader->state.position, admitted);
                    auto& installed = outcome.attack_path->stack.back();
                    if (std::find(admitted.begin(), admitted.end(), leader_id) != admitted.end())
                        installed.trackers.at(state.container).started = true;
                }
                if (route.nodes.size() < 2 || segment >= route.nodes.size() - 1)
                    outcome.attack_approach = false;
            }
            if (squadron_refresh_skips(target, craft, state, minds, outcome, target_id, frame, leader, combat_world, attack_leaders, index, container, crafts, finish_base)) continue;
            finish_base();
        }
    }
}


core::Result<void> session_detail::Tick::service_death_projectiles_and_spins(const std::vector<UnitState>& killed, const SquadronTable& squadron_table,
    const std::uint64_t& frame, detail::MapStage<CraftState>& crafts, std::vector<Event>& events) {
    // WNO-29/WCC-70: visit the ordinary-death journal once, in destruction order.
    // Replacements and generic removal never enter this journal.
    if (impl_->durability.damage) for (const auto& dead : killed) {
        const auto* profile = impl_->combat.find(dead.type_id);
        if (profile == nullptr || profile->death_projectiles.empty()) continue;
        CombatRandom random(impl_->setup.seed, tick, dead.entity_id, death_projectile_selection_slot);
        const auto choice = random.uniform(0, static_cast<std::uint32_t>(profile->death_projectiles.size() - 1));
        auto origin = dead.position;
        auto height = math::add(origin.z, profile->ranged_target_z_adjust);
        if (!height) return core::Result<void>::failure(height.error());
        origin.z = height.value();
        auto matrix = math::to_matrix(dead.rotation, origin);
        if (!matrix) return core::Result<void>::failure(matrix.error());
        auto aim = math::transform_point(matrix.value(), {math::Fixed::from_raw(math::Fixed::scale), {}, {}});
        if (!aim) return core::Result<void>::failure(aim.error());
        CombatEvent shot;
        shot.tick = tick; shot.shooter = dead.entity_id;
        shot.weapon = death_projectile_slot_flag | choice;
        shot.target_hardpoint = no_hardpoint; shot.origin = origin; shot.aim = aim.value();
        auto& next = flights_.value().next_projectile.value();
        auto projectile = detail::launch_projectile(shot, profile->death_projectiles[choice], dead.owner,
            true, next++, {}, {});
        if (!projectile) return core::Result<void>::failure(projectile.error());
        impacts_.value().projectiles.value().push_back(std::move(projectile).value());
    }
    // Spin phase (#447, space-fighter-deaths SP-04 to SP-08): workers service each spinning
    // craft's copy in its own slot; the serial commit in ascending ID drops those that ended and
    // publishes their spin_away_ended. The craft killed this tick then draw whether they spin away
    // (SP-02, SP-03), serially in destruction order: one keyed draw per death, so this walks the
    // tick's deaths, never every unit, and their order is the events' order.
    fighters_.emplace();
    auto& spins = fighters_.value().spins.emplace(impl_->spins.size());
    if (!spins.empty()) {
        std::vector<std::uint8_t> ended(spins.size());
        std::vector<std::optional<core::Diagnostic>> spin_errors(spins.size());
        const auto spun = executor.execute_phase("spins", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, spins.size());
            for (auto index = range.begin; index < range.end; ++index) {
                spins[index] = impl_->spins[index];
                const auto* craft = squadron_table.find_craft(spins[index].type);
                if (craft == nullptr || !craft->spin_away) {
                    ended[index] = 1;
                    continue;
                }
                const auto stepped = step_spin(spins[index], craft->rate_of_turn, *craft->spin_away,
                    impl_->setup.seed, frame);
                if (!stepped) {
                    spin_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + ": " + stepped.error().message);
                    continue;
                }
                ended[index] = stepped.value() ? 1 : 0;
            }
        });
        if (!spun) {
            return core::Result<void>::failure(spun.error());
        }
        std::vector<DeathSpin> running;
        for (std::size_t index = 0; index < spins.size(); ++index) {
            if (spin_errors[index]) {
                return core::Result<void>::failure(std::move(*spin_errors[index]));
            }
            if (ended[index] != 0) {
                events.push_back(Event{tick, EventKind::spin_away_ended, spins[index].owner, 0, spins[index].unit});
                continue;
            }
            running.push_back(std::move(spins[index]));
        }
        spins = std::move(running);
    }
    for (const auto& state : killed) {
        const auto* craft = squadron_table.find_craft(state.type_id);
        if (craft == nullptr || !craft->spin_away
            || !spins_away(*craft->spin_away, impl_->setup.seed, frame, state.entity_id)) {
            continue;
        }
        // SP-03: a craft without a flight state spins from rest.
        const auto flight = crafts.find(state.entity_id);
        spins.push_back(start_spin(state.entity_id, state.type_id, state.owner, state.position,
            flight != crafts.end() ? flight->second : CraftState{}));
        events.push_back(Event{tick, EventKind::spin_away_started, state.owner, 0, state.entity_id});
    }
    std::sort(spins.begin(), spins.end(), [](const DeathSpin& left, const DeathSpin& right) { return left.unit < right.unit; });

    return core::Result<void>::success();
}


core::Result<void> session_detail::Tick::commit_squadron_outcomes(std::vector<const Squadron*>& squadron_inputs, std::vector<SquadronOutcome>& outcomes,
    detail::MapStage<SquadronState>& minds, auto& find,
    std::vector<std::pair<std::size_t, PlayerId>>& final_killers, std::vector<std::uint8_t>& completed_bases,
    std::vector<LiveUnit>& survivors, std::vector<TacticalInstance>& instances,
    std::vector<Squadron>& squadrons) {
    mark("squadron_commit", true);
    std::vector<EntityId> emptied;
    std::vector<Squadron> kept;
    for (std::size_t index = 0; index < squadron_inputs.size(); ++index) {
        auto& outcome = outcomes[index];
        if (outcome.error) {
            return core::Result<void>::failure(std::move(*outcome.error));
        }
        if (!outcome.container_live) {
            continue; // the container itself died: its craft fly on without a sensor
        }
        const auto container = squadron_inputs[index]->container;
        if (outcome.members.empty()) {
            // WBF-31: the relevant parent container's last member also reevaluates elimination.
            track_victory_change(&find(container)->state, nullptr, true);
            if (loss_ids_.insert(container).second) {
                const auto& state = find(container)->state;
                loss_killers_.emplace(container, std::pair{pending_losses_.size() + 1U, final_killers[index].second});
                pending_losses_.push_back({state.owner, state.type_id, final_killers[index].second, 1, tick});
                if (impl_->setup.attract_pool) {
                    // MAT-15: the emptied container is the team's Destroy; its company type returns.
                    const auto mind = minds.find(container);
                    note_pool_return(state, mind != minds.end() ? mind->second.garrison_unit
                                                                : find(container)->garrison_unit, false);
                }
            }
            emptied.push_back(container);
            continue;
        }
        if (completed_bases[index] == 2) {
            auto& state = minds.at(container);
            // FM-24 is ordered: a successful claim changes the next team's
            // available cells. FM-26 publishes the newly claimed hold point.
            claim_idle(container, state, state.anchor);
            if (state.idle_cell) {
                outcome.centre = state.anchor;
                outcome.transform.rows[0][3] = outcome.centre.x;
                outcome.transform.rows[1][3] = outcome.centre.y;
                outcome.transform.rows[2][3] = outcome.centre.z;
            }
        }
        const auto slot = static_cast<std::size_t>(find(container) - survivors.data());
        // WWP-51: aggregate publication can be this target's first displacement.
        if (!survivors[slot].position_history && survivors[slot].state.position != outcome.centre
            && impl_->combat.find_target_type(survivors[slot].state.type_id) != nullptr) {
            survivors[slot].position_history = PositionHistory{survivors[slot].state.position,
                survivors[slot].state.position};
        }
        survivors[slot].state.position = outcome.centre;
        survivors[slot].state.rotation = outcome.rotation;
        instances[slot].fixed_transform = outcome.transform;
        // FD-67/FSC-73: installation follows ordinary turns; it cannot move
        // or fire a member again. The next movement reads this ordered commit.
        if (outcome.attack_path) minds.at(container).attack_path = std::move(outcome.attack_path);
        if (outcome.attack_approach) minds.at(container).approach = *outcome.attack_approach;
        if (outcome.attack_hardpoint && minds.at(container).formation)
            minds.at(container).formation->attack_hardpoint = *outcome.attack_hardpoint;
        if (completed_bases[index] != 0) {
            auto& state = minds.at(container);
            if (completed_bases[index] == 1 || state.idle_cell) {
                state.formation->complete = true;
                state.formation->has_reached_done = true;
                if (state.mode == SquadronMode::idle) state.escorted = invalid_entity_id;
            }
        }
        kept.push_back(Squadron{container, std::move(outcome.members)});
    }
    // Containers without craft leave the session. They have no hull, so no event is
    // published; presentation sees them leave the snapshot.
    if (!emptied.empty()) {
        std::vector<LiveUnit> remaining;
        std::vector<TacticalInstance> remaining_instances;
        for (std::size_t index = 0; index < survivors.size(); ++index) {
            if (std::binary_search(emptied.begin(), emptied.end(), survivors[index].state.entity_id)) {
                continue;
            }
            remaining.push_back(std::move(survivors[index]));
            remaining_instances.push_back(std::move(instances[index]));
        }
        survivors = std::move(remaining);
        instances = std::move(remaining_instances);
    }
    squadrons = std::move(kept);
    mark("squadron_commit", false);
    return core::Result<void>::success();
}


core::Result<void> session_detail::Tick::prepare_squadron_outcomes(detail::MapStage<SquadronState>& minds, std::vector<LiveUnit>& survivors,
    const std::vector<Squadron>& arrived_squadrons, detail::MapStage<CraftState>& crafts,
    const SquadronTable& squadron_table, const std::vector<std::pair<EntityId, math::Vec3>>& movement_starts,
    const std::vector<std::optional<CraftStep>>& movement_steps, const detail::CombatWorld& combat_world,
    const std::uint64_t& frame, std::vector<TacticalInstance>& instances) {
    // Squadron phase (#271, V-03): workers read the survivors and fill one slot per squadron
    // with its live craft and their bounding-box centre. The serial commit then moves each
    // container there, or removes a container whose last craft has died.
    std::vector<const Squadron*> squadron_inputs;
    squadron_inputs.reserve(impl_->squadrons.size() + arrived_squadrons.size());
    std::set<EntityId> replaced;
    std::set<EntityId> regrouped;
    for (const auto& squadron : arrived_squadrons) replaced.insert(squadron.container);
    for (const auto& squadron : arrived_squadrons) for (const auto member : squadron.members) regrouped.insert(member);
    for (const auto& squadron : impl_->squadrons)
        if (!replaced.contains(squadron.container) && !(squadron.members.size() == 1
            && squadron.members.front() == squadron.container && regrouped.contains(squadron.container)))
            squadron_inputs.push_back(&squadron);
    for (const auto& squadron : arrived_squadrons) squadron_inputs.push_back(&squadron);
    if (!replaced.empty()) std::sort(squadron_inputs.begin(), squadron_inputs.end(),
        [](const auto* left, const auto* right) { return left->container < right->container; });
    auto& squadrons = fighters_.value().squadrons.emplace();
    if (!squadron_inputs.empty()) {
        const auto find = [&survivors](const EntityId id) -> const LiveUnit* {
            const auto found = std::lower_bound(survivors.begin(), survivors.end(), id,
                [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
            return found != survivors.end() && found->state.entity_id == id ? &*found : nullptr;
        };
        // Capture only stable selector handles: commands can create a new destination after
        // movement. Its first path must use the same cohort choice as subsequent flight.
        using AttackCohortKey = std::tuple<std::optional<CommandKey>, EntityId, EntityId>;
        std::map<AttackCohortKey, const LiveUnit*> cohort_leaders;
        std::vector<const LiveUnit*> attack_leaders(squadron_inputs.size());
        for (std::size_t index = 0; index < squadron_inputs.size(); ++index) {
            const auto id = squadron_inputs[index]->container;
            const auto mind = minds.find(id);
            const auto* container = find(id);
            if (mind == minds.end() || !mind->second.formation || container == nullptr) continue;
            const auto target = mind->second.formation->attack_target;
            if (target == invalid_entity_id) continue;
            const LiveUnit* leader = nullptr;
            for (const auto member : squadron_inputs[index]->members) {
                if ((leader = find(member)) != nullptr) break;
            }
            if (leader == nullptr) continue;
            const auto coordinator = container->waypoints ? container->waypoints->coordinator : std::nullopt;
            const AttackCohortKey key{coordinator, coordinator ? invalid_entity_id : id, target};
            attack_leaders[index] = cohort_leaders.try_emplace(key, leader).first->second;
        }
        std::vector<SquadronOutcome> outcomes(squadron_inputs.size());
        std::vector<std::uint8_t> completed_bases(squadron_inputs.size());
        std::vector<std::pair<std::size_t, PlayerId>> final_killers(squadron_inputs.size());
        const auto grouped = executor.execute_phase("squadrons", tick_partition_count, [&](const std::size_t partition) {
            prepare_squadron_outcomes_partition(partition, squadron_inputs, outcomes, find, minds, final_killers, squadron_table, completed_bases, movement_starts, movement_steps, combat_world, attack_leaders, crafts, frame);
        });
        if (!grouped) {
            return core::Result<void>::failure(grouped.error());
        }
        if (auto result = commit_squadron_outcomes(squadron_inputs, outcomes, minds, find, final_killers, completed_bases, survivors, instances, squadrons); !result) return result;
    }

    return core::Result<void>::success();
}


core::Result<void> session_detail::Tick::retire_fighter_state(detail::MapStage<SquadronState>& minds, detail::MapStage<CraftState>& crafts,
    const SquadronTable& squadron_table, const std::uint64_t& frame, std::vector<Squadron>& squadrons) {
    // #75: a squadron that left releases its spawner entry (FL-08); dead craft, squadrons and
    // spawners drop their state. Serial, in ascending ID.
    mark("minds_cleanup", true);
    auto& spawners = fighters_.value().spawners.emplace(impl_->spawners);
    // FL-12: commit only this tick's carrier births through the starting-unit initializer.
    for (const auto id : staging_.value().born_spawners) {
        if (survivor(id) != nullptr) spawners.emplace(id, initial_spawner(impl_->setup.seed, frame, id));
    }
    for (auto iterator = minds.begin(); iterator != minds.end();) {
        const auto container = iterator->first;
        const auto found = std::lower_bound(squadrons.begin(), squadrons.end(), container,
            [](const Squadron& squadron, EntityId id) { return squadron.container < id; });
        const bool kept = found != squadrons.end() && found->container == container;
        if (kept) {
            ++iterator;
            continue;
        }
        const auto& state = iterator->second;
        const auto hangar = spawners.find(state.spawner);
        const auto* spawner = survivor(state.spawner);
        if (hangar != spawners.end() && spawner != nullptr) {
            squadron_lost(*squadron_table.find_spawner(spawner->state.type_id, spawner->state.owner), spawners.at(hangar->first), state.entry, frame);
        }
        iterator = minds.erase(iterator);
    }
    crafts.erase_if([&](const auto& entry) {
        if (survivor(entry.first) != nullptr) return false;
        impl_->craft_trig.erase(entry.first);
        return true;
    });
    spawners.erase_if([&](const auto& entry) { return survivor(entry.first) == nullptr; });
    auto& garrisons = fighters_.value().free_garrisons.emplace(impl_->free_garrisons);
    if (!garrisons.empty()) {
        const auto serviced = executor.execute_phase("free-garrisons", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, garrisons.size());
            for (auto index = range.begin; index < range.end; ++index) {
                auto& state = garrisons[index];
                const auto player = state.setup.player;
                const bool quit = std::any_of(impl_->quits.begin(), impl_->quits.end(),
                    [player](const auto& value) { return value.player == player; })
                    || std::any_of(pending_quits_.begin(), pending_quits_.end(),
                        [player](const auto& value) { return value.player == player; });
                const bool held = !state.registered.empty();
                std::erase_if(state.registered, [&](const auto id) {
                    const auto* unit = survivor(id);
                    return unit == nullptr || unit->state.owner != player;
                });
                if (quit) {
                    state.pending.clear();
                    state.due_frame.reset();
                    continue;
                }
                // FL-14: only the loss of the last registered actual object starts a delay.
                if (held && state.registered.empty() && state.pending.empty() && !state.due_frame)
                    state.due_frame = frame + state.setup.delay_frames;
            }
        });
        if (!serviced) return core::Result<void>::failure(serviced.error());
    }

    mark("minds_cleanup", false);
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::fighters() {
    if (impl_->setup.domain != BattleDomain::space) {
        // Finish/commit consume these stages even when the space services do not run.
        fighters_.emplace();
        fighters_->spins.emplace(impl_->spins);
        fighters_->squadrons.emplace(impl_->squadrons);
        fighters_->spawners.emplace(impl_->spawners);
        fighters_->free_garrisons.emplace(impl_->free_garrisons);
        return core::Result<void>::success();
    }
    auto& minds = craft_prep_.value().minds.value();
    auto& crafts = staging_.value().crafts.value();
    const auto& arrived_squadrons = staging_.value().arrived_squadrons.value();
    const auto& frame = tracked_.value().frame.value();
    auto& events = impacts_.value().events.value();
    const auto& killed = impacts_.value().killed.value();
    auto& instances = systems_.value().instances.value();
    auto& survivors = surviving_.value().survivors.value();
    const auto& squadron_table = impl_->motion.squadrons;
    const auto& movement_steps = movement_->craft_steps.value();
    // The live movement vector has entered later staging by this phase. The
    // immutable start journal retains the same ascending slots as craft_steps.
    const auto& movement_starts = gather_->starts.value();
    const auto& combat_world = targets_->world.value().value();
    if (auto result = service_death_projectiles_and_spins(killed, squadron_table, frame, crafts, events); !result) return result;
    if (auto result = prepare_squadron_outcomes(minds, survivors, arrived_squadrons, crafts, squadron_table, movement_starts, movement_steps, combat_world, frame, instances); !result) return result;
    auto& squadrons = fighters_.value().squadrons.value();
    if (auto result = retire_fighter_state(minds, crafts, squadron_table, frame, squadrons); !result) return result;
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::hangars() {
    if (impl_->setup.domain != BattleDomain::space) return core::Result<void>::success();
    auto& minds = craft_prep_.value().minds.value();
    auto& crafts = staging_.value().crafts.value();
    auto& respawns = staging_.value().respawns.value();
    auto& earners = staging_.value().earners.value();
    auto& next_id = staging_.value().next_id.value();
    const auto& frame = tracked_.value().frame.value();
    auto& instances = systems_.value().instances.value();
    auto& survivors = surviving_.value().survivors.value();
    auto& squadrons = fighters_.value().squadrons.value();
    auto& spawners = fighters_.value().spawners.value();
    const auto& squadron_table = impl_->motion.squadrons;
    auto& pads = impl_->pad_stage.values;
    auto& arrivals = staging_.value().arrivals.value();
    auto& garrisons = fighters_.value().free_garrisons.value();
    const auto allied_pending = [&](const PlayerId owner) {
        const auto station_player = std::lower_bound(impl_->snapshot_players.begin(), impl_->snapshot_players.end(), owner,
            [](const auto& player, const auto id) { return player.player_id < id; });
        return std::find_if(garrisons.begin(), garrisons.end(), [&](const auto& state) {
            if (state.pending.empty() || station_player == impl_->snapshot_players.end()
                || station_player->player_id != owner || station_player->neutral) return false;
            const auto player = std::lower_bound(impl_->snapshot_players.begin(), impl_->snapshot_players.end(), state.setup.player,
                [](const auto& value, const auto id) { return value.player_id < id; });
            return player != impl_->snapshot_players.end() && !player->neutral && player->team_id == station_player->team_id;
        });
    };
    // Hangar phase (#75, FL-01 to FL-06): workers service each spawner's hangar copy in its own
    // slot; the serial commit launches the squadrons in ascending spawner ID with the next stable
    // IDs, craft first and their team container last (FL-06).
    if (!spawners.empty()) {
        std::vector<std::pair<EntityId, SpawnerState>> hangars(spawners.begin(), spawners.end());
        std::vector<std::optional<SpawnDecision>> decisions(hangars.size());
        std::vector<std::optional<std::uint32_t>> free_bays(hangars.size());
        const auto serviced_hangars = executor.execute_phase("hangars", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, hangars.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& unit = *survivor(hangars[index].first);
                const auto& profile = *squadron_table.find_spawner(unit.state.type_id, unit.state.owner);
                const bool service_due = frame == hangars[index].second.next_service_frame;
                const bool no_controller = impl_->setup.controllers && service_due
                    && std::any_of(impl_->setup.controllers->factions.begin(), impl_->setup.controllers->factions.end(),
                        [&](const auto& row) { return row.player == unit.state.owner; })
                    && impl_->output_controller(unit.state.owner, unit.state.controller_seat, 0).value == 0;
                std::vector<bool> intact;
                const auto* hull = impl_->health_profile(unit);
                for (const auto& bay : profile.bays) {
                    // FL-03: space hangar admission checks destruction only;
                    // a disabled bay restored by an upgrade can still launch.
                    intact.push_back(!unit.durability || hull == nullptr || bay.hardpoint >= hull->hardpoints.size()
                        || !hardpoint_destroyed(*hull, *unit.durability, bay.hardpoint));
                }
                // WSL-19: a launched single ship that left the session frees its entry slot like a lost squadron.
                std::erase_if(hangars[index].second.ships, [&](const SpawnedShip& ship) {
                    if (survivor(ship.id) != nullptr) return false;
                    squadron_lost(profile, hangars[index].second, ship.entry, frame);
                    return true;
                });
                decisions[index] = service_spawner(profile, hangars[index].second, impl_->setup.seed, frame,
                    hangars[index].first, intact, arrivals.contains(hangars[index].first) || !unit.state.garrison_enabled || no_controller);
                if (!decisions[index] && service_due && profile.starbase
                    && !arrivals.contains(hangars[index].first) && hangars[index].second.next_spawn_frame <= frame) {
                    const auto pending = allied_pending(unit.state.owner);
                    if (pending != garrisons.end()) {
                        const bool eligible = !impl_->setup.controllers
                            || std::none_of(impl_->setup.controllers->factions.begin(), impl_->setup.controllers->factions.end(),
                                [&](const auto& row) { return row.player == pending->setup.player; })
                            || impl_->output_controller(pending->setup.player, {}, 0).value != 0;
                        if (eligible) free_bays[index] = free_garrison_bay(profile, impl_->setup.seed, frame, hangars[index].first, intact);
                    }
                }
            }
        });
        if (!serviced_hangars) {
            return core::Result<void>::failure(serviced_hangars.error());
        }
        for (std::size_t index = 0; index < hangars.size(); ++index) {
            spawners.assign(hangars[index].first, std::move(hangars[index].second));
            if (!decisions[index] && !free_bays[index]) continue;
            const auto spawner = *survivor(hangars[index].first); // copied: launches grow `survivors`
            const auto& profile = *squadron_table.find_spawner(spawner.state.type_id, spawner.state.owner);
            const auto first_added = survivors.size();
            auto pending = garrisons.end();
            if (!decisions[index]) {
                pending = allied_pending(spawner.state.owner);
                if (pending == garrisons.end()) continue; // an earlier station claimed the pending front
            }
            const auto decision = decisions[index].value_or(SpawnDecision{0, free_bays[index].value_or(0)});
            const auto first_id = next_id;
            const auto output_owner = pending != garrisons.end() ? pending->setup.player : spawner.state.owner;
            const auto controller = impl_->output_controller(output_owner, spawner.state.controller_seat,
                spawners.at(hangars[index].first).controller_output_ordinal);
            const auto flown_out = impl_->launch(spawner, profile, decision, frame, next_id, survivors, instances,
                squadrons, crafts, minds, pending != garrisons.end() ? pending->pending.front() : 0,
                pending != garrisons.end() ? pending->setup.player : 0,
                pending != garrisons.end() ? &pending->registered : nullptr, &arrivals, controller);
            if (!flown_out) {
                return core::Result<void>::failure(flown_out.error());
            }
            if (impl_->setup.controllers && spawner.state.control == UnitControl::shared_structure
                && survivors.size() != first_added)
                ++spawners.at(hangars[index].first).controller_output_ordinal;
            if (decisions[index] && profile.entries[decision.entry].ship) {
                auto& hangar = spawners.at(hangars[index].first);
                if (survivors.size() == first_added) {
                    // WSL-16: no free spot beside the bay: the launch did not happen, so the slot is returned.
                    auto& entry = hangar.entries[decision.entry];
                    if (entry.alive > 0) --entry.alive;
                    if (entry.remaining != unlimited_reserve) ++entry.remaining;
                } else {
                    hangar.ships.push_back({first_id, decision.entry});
                }
            }
            if (pending != garrisons.end()) {
                pending->pending.erase(pending->pending.begin());
                spawners.at(hangars[index].first).next_spawn_frame = frame + profile.delay_frames;
            }
            for (auto added = first_added; added < survivors.size(); ++added) {
                track_victory_change(nullptr, &survivors[added].state, false);
                if (pending != garrisons.end()) {
                    const auto& born = survivors[added].state;
                    if (const auto* born_profile = squadron_table.find_spawner(born.type_id, born.owner);
                        born_profile != nullptr && (!born_profile->entries.empty() || born_profile->starbase))
                        spawners.emplace(born.entity_id, initial_spawner(impl_->setup.seed, frame + 1, born.entity_id));
                    if (impl_->economy.stream(born.type_id) != nullptr) earners.emplace_back(born.entity_id, born.owner);
                }
            }
        }
    }

    while (!respawns.empty() && respawns.begin()->first <= tick) {
        for (const auto& request : respawns.begin()->second.objects) {
            if (next_id == invalid_entity_id) return core::Result<void>::failure(
                detail::diagnostic(diagnostic_codes::resource_limit, "respawn exhausted stable IDs"));
            const auto id = next_id;
            UnitState state{id, request.type, request.owner, request.position, request.rotation, {}};
            if (auto assigned = impl_->attribute_output(state, request.controller_seat, id); !assigned) return assigned;
            auto unit = impl_->new_unit(state, tick);
            const auto transform = unit_transform(unit);
            if (!transform) return core::Result<void>::failure(transform.error());
            instances.push_back(impl_->instance_for(unit, transform.value(), tick + 1));
            track_victory_change(nullptr, &unit.state, false);
            survivors.push_back(std::move(unit));
            if (impl_->economy.pads.point(request.type) != nullptr) {
                impl_->pad_stage.touch(id);
                pads.emplace(id, PadState{request.owner});
            }
            if (impl_->economy.stream(request.type) != nullptr) earners.emplace_back(id, request.owner);
            next_id = id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : id + 1;
        }
        respawns.erase(respawns.begin());
    }

    // FL-14: player timers mature after object/hangar service. A queue made
    // ready this frame is available only to a subsequent due hangar pass.
    if (!garrisons.empty()) {
        const auto matured = executor.execute_phase("free-garrison-timers", tick_partition_count,
            [&](const std::size_t partition) {
                const auto range = partition_range(partition, garrisons.size());
                for (auto index = range.begin; index < range.end; ++index) {
                    auto& state = garrisons[index];
                    if (state.due_frame && *state.due_frame <= frame) {
                        state.pending = state.setup.templates;
                        state.due_frame.reset();
                    }
                }
            });
        if (!matured) return core::Result<void>::failure(matured.error());
    }
    return core::Result<void>::success();
}

const LiveUnit* session_detail::Tick::survivor(const EntityId id) {
    const auto& survivors = surviving_.value().survivors.value();

        const auto found = std::lower_bound(survivors.begin(), survivors.end(), id,
            [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
        return found != survivors.end() && found->state.entity_id == id ? &*found : nullptr;
}

std::int64_t session_detail::Tick::midpoint(const std::int64_t low, const std::int64_t high) noexcept {
    const auto half = (static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low)) / 2U;
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + half);
}

} // namespace eawr::sim::tactical
