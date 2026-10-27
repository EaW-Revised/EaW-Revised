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

core::Result<void> session_detail::Tick::CommandOrders::apply(
    const PlayerCommand& command, const std::vector<EntityId>& selected_units,
    const EntityId unit_id, LiveUnit& live, const EntityId approach,
    const bool waypoint_resume, const bool attack_assignment_allowed,
    std::vector<EntityId>& group, std::vector<EntityId>& squadron_group) const {
    auto* impl = owner.impl_;
    const auto command_tick = owner.tick;
    auto& craft_prep = owner.craft_prep_;
    auto& staged = owner.tracked_->staged.value();
    auto& tracking = owner.tracked_->tracking.value();
    auto& minds = owner.craft_prep_->minds.value();
    const auto& table = impl->motion;
    const auto& squadron_table = table.squadrons;
    const auto& view = owner.targets_->world.value().value();
    const bool member_move = std::holds_alternative<MovePayload>(command.payload) && [&] {
        const auto& membership = craft_prep->craft_squadron.value();
        const auto member = membership.find(unit_id);
        return member != membership.end() && member->second != unit_id;
    }();
    const bool queued_wait = [&] {
        if (waypoint_resume) return false;
        if (const auto* move = std::get_if<MovePayload>(&command.payload); move && move->append && !member_move) {
            // HCB-58: admission retains the current leg and bounded chronological queue.
            if (table.rules.max_waypoints == 0) return true;
            if (!live.waypoints) { live.waypoints.emplace(); live.waypoints->coordinator = command.key; }
            auto& route = *live.waypoints;
            if (route.points.size() >= table.rules.max_waypoints) return true;
            const MovePayload leg{move->destination, move->through_hazards};
            if (!route.points.empty() && route.points.back().move == leg) return true;
            bool closed = false;
            // The newest entry is an exact duplicate gate, not a loop-close candidate.
            for (auto prior = route.points.size(); prior > 1; --prior) {
                const auto index = prior - 2;
                const auto radius = math::Fixed::from_raw(80 * math::Fixed::scale);
                if (within_range(route.points[index].move.destination, leg.destination, radius, RangeMetric::planar)
                    && route.points[index].move.destination != leg.destination) {
                    // HCB-58: reject the radius boundary; exact destination equality is handled below.
                    const auto dx = math::subtract(route.points[index].move.destination.x, leg.destination.x).value();
                    const auto dy = math::subtract(route.points[index].move.destination.y, leg.destination.y).value();
                    if (math::length(math::Vec3{dx, dy, {}}).value() >= radius) continue;
                    for (auto repeated = index; repeated < route.points.size(); ++repeated) route.points[repeated].repeat = true;
                    closed = true;
                    break;
                }
                if (route.points[index].move.destination == leg.destination) {
                    for (auto repeated = index; repeated < route.points.size(); ++repeated) route.points[repeated].repeat = true;
                    closed = true;
                    break;
                }
            }
            if (closed) return true;
            route.points.push_back({leg, command.key, false});
            const auto mind = minds.find(unit_id);
            const bool moving_team = mind != minds.end() && mind->second.mode != SquadronMode::idle;
            if (route.active || route.points.size() > 1 || live.formation || moving_team
                || (live.motion && live.motion->kind != MotionKind::none) || live.approach) return true;
            route.active = true;
        } else if (std::holds_alternative<MovePayload>(command.payload)
            || std::holds_alternative<StopPayload>(command.payload)
            || std::holds_alternative<AttackPayload>(command.payload)
            || std::holds_alternative<AttackMovePayload>(command.payload)
            || std::holds_alternative<GuardPayload>(command.payload)) {
            live.waypoints.reset();
        }
        return false;
    }();
    if (std::holds_alternative<StopPayload>(command.payload)) {
        // WAB-51: Stop switches Hunt off through the same path as its button,
        // including clearing chase targets and stopping the whole squadron.
        const auto& membership = craft_prep->craft_squadron.value();
        const auto member = membership.find(unit_id);
        const auto holder = member == membership.end() ? unit_id : member->second;
        const auto stopped = apply_ability(holder, AbilityPayload{AbilityKind::hunt, AbilityAction::deactivate});
        if (!stopped) return core::Result<void>::failure(stopped.error());
    }
    // WSQ-59 (#741): a move given to one craft of a squadron changes nothing on the
    // craft (its order, targets and flight stay); only the cancellations below apply.
    if (!member_move && !queued_wait) live.state.order = order_for(command.payload, command_tick);
    // WAB-40, AB-65: accepted move, attack and guard orders also cancel the
    // parent shot when addressed to a member. Ability/internal orders stay separate.
    const bool explicitly_addressed = std::find(selected_units.begin(), selected_units.end(), unit_id) != selected_units.end();
    if ((std::holds_alternative<MovePayload>(command.payload)
        || std::holds_alternative<AttackPayload>(command.payload)
        || std::holds_alternative<AttackMovePayload>(command.payload)
        || std::holds_alternative<GuardPayload>(command.payload)) && (waypoint_resume || explicitly_addressed)) {
        const auto& membership = craft_prep->craft_squadron.value();
        const auto member = membership.find(unit_id);
        const auto parent_id = member == membership.end() ? unit_id : member->second;
        const auto parent = staged.find(parent_id);
        // WAB-51: an accepted external order cancels every holder in the team.
        const auto cancel_hunt = [&](LiveUnit& holder) {
            if (!holder.abilities) return;
            const auto* profile = impl->abilities.find(holder.state.type_id);
            const auto index = ability_slot(*profile, AbilityKind::hunt);
            if (index) static_cast<void>(deactivate_ability(profile->abilities[*index], holder.abilities->slots[*index], command_tick));
        };
        cancel_hunt(live);
        if (parent != staged.end()) cancel_hunt(parent->second);
        if (const auto mind = minds.find(parent_id); mind != minds.end()) {
            for (const auto member_id : mind->second.roster) {
                if (const auto holder = staged.find(member_id); holder != staged.end()) cancel_hunt(holder->second);
            }
        }
        auto* team = parent != staged.end() ? impl->ion_slot(parent->second) : nullptr;
        if (team != nullptr && team->active && minds.find(parent_id) != minds.end()) {
            const auto squadron = std::find_if(impl->squadrons.begin(), impl->squadrons.end(),
                [parent_id](const Squadron& entry) { return entry.container == parent_id; });
            if (squadron != impl->squadrons.end()) {
                std::vector<AbilitySlot*> craft;
                for (const auto craft_id : squadron->members) {
                    const auto found = staged.find(craft_id);
                    if (auto* slot = found != staged.end() ? impl->ion_slot(found->second) : nullptr) {
                        craft.push_back(slot);
                    }
                }
                impl->finish_ion_shot(*team, craft, parent->second.state.type_id, command_tick);
            }
        }
    }
    if (member_move || queued_wait) return core::Result<void>::success();
    if (!waypoint_resume && (std::holds_alternative<MovePayload>(command.payload)
        || std::holds_alternative<AttackPayload>(command.payload)
        || std::holds_alternative<AttackMovePayload>(command.payload)
        || std::holds_alternative<GuardPayload>(command.payload)) && (live.motion || minds.find(unit_id) != minds.end())) {
        if (!live.waypoints) live.waypoints.emplace();
        live.waypoints->coordinator = command.key;
    }
    // FO-01, FO-03 (#424): a squadron takes the order as one unit through its team
    // container; its craft fly it in the next craft phase. Serial, in command order.
    if (const auto mind = minds.find(unit_id); mind != minds.end()) {
        if (std::holds_alternative<AttackPayload>(command.payload) && !attack_assignment_allowed) return core::Result<void>::success();
        auto squadron_payload = command.payload;
        const auto team_target = [&](const EntityId target) {
            const auto& membership = craft_prep.value().craft_squadron.value();
            const auto member = membership.find(target);
            return member != membership.end() ? member->second : target;
        };
        // WSQ-47/WMV-18: a space team's destination names a team, while the
        // submitted command and published order retain the requested craft ID.
        if (auto* attack = std::get_if<AttackPayload>(&squadron_payload)) attack->target = team_target(attack->target);
        else if (auto* attack_move = std::get_if<AttackMovePayload>(&squadron_payload)) attack_move->target = team_target(attack_move->target);
        else if (auto* guard = std::get_if<GuardPayload>(&squadron_payload)) guard->target = team_target(guard->target);
        // UFS-21: an accepted replacement ends the special approach, even when
        // the replacement is another move to the same point. Face preserves it.
        if (live.abilities && !std::holds_alternative<FacePayload>(squadron_payload))
            live.abilities->pending_buzz_position.reset();
        apply_squadron_order(minds.at(mind->first), squadron_payload, live.state.position, squadron_table);
        if (mind->second.mode == SquadronMode::move && !std::holds_alternative<FacePayload>(command.payload)) {
            squadron_group.push_back(unit_id);
        }
        return core::Result<void>::success();
    }
    // An attack order gives the unit and its hardpoints that target (#73); any other
    // order ends a previous attack order.
    if (live.combat) {
        if (const auto* attack = std::get_if<AttackPayload>(&command.payload)) {
            if (attack_assignment_allowed) {
                live.combat->attack_target = attack->target;
                live.combat->attack_hardpoint = attack->hardpoint;
                live.combat->direct = true;
            }
        } else if (live.combat->direct) {
            live.combat->attack_target = invalid_entity_id;
            live.combat->attack_hardpoint = no_hardpoint;
            live.combat->direct = false;
        }
    }
    const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
    std::optional<std::size_t> layer;
    if (footprint != nullptr) layer = dynamic_layer_index(footprint->layer);
    const auto failed = [&](const core::Diagnostic& error) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
            command_context(command.key) + " unit " + std::to_string(unit_id) + ": " + error.message));
    };
    const bool moves = point_move(command.payload).has_value();
    // A move, face or stop submits the unit's new prediction and replaces a group
    // move's wait (FM-10), and so does an order that approaches a unit (#452); other
    // orders keep both. Every order ends the previous approach mapping.
    const bool submits = moves || std::holds_alternative<FacePayload>(command.payload)
        || std::holds_alternative<StopPayload>(command.payload) || approach != invalid_entity_id;
    if (submits) live.formation.reset();
    live.approach.reset();
    live.sighting.reset();
    if (tracking && layer && moves && live.motion) {
        group.push_back(unit_id);
        return core::Result<void>::success();
    }
    if (layer && (moves || submits) && !pending_empty()) {
        if (auto queued = plan_pending()) return unit_failure(queued->first, queued->second);
    }
    if (approach != invalid_entity_id) {
        // OR-02, OR-05: the first mapping. A unit in range of the target holds where
        // it is; otherwise it plans towards its approach slot.
        if (!live.motion) return core::Result<void>::success();
        const auto& target = staged.at(approach);
        const bool guard = std::holds_alternative<GuardPayload>(command.payload);
        // The targeting view's health and combat pointers name the units before
        // staging moved them; point the target's entry at its staged health and that
        // health's profile (an upgraded unit keeps its profile in the unit itself).
        std::optional<detail::CombatUnit> target_view;
        if (const auto* entry = view.find(approach)) {
            target_view = *entry;
            target_view->durability = target.durability ? &*target.durability : nullptr;
            target_view->durability_profile = impl->health_profile(target);
            target_view->combat = target.combat ? &*target.combat : nullptr;
        }
        const auto* ordered_attack = std::get_if<AttackPayload>(&command.payload);
        const auto range = impl->approach_range(live, guard, target_view ? &*target_view : nullptr);
        auto reach = core::Result<math::Fixed>::success(range);
        if (!guard && target_view) {
            reach = detail::target_attack_distance(&impl->motion, *target_view, live.state.position,
                impl->attack_distance(live).value_or(math::Fixed{}),
                detail::has_aim_hardpoint(*target_view));
        }
        if (!reach) return failed(reach.error());
        const auto point = impl->approach_point(live, guard, target, target_view ? &*target_view : nullptr,
            ordered_attack != nullptr ? ordered_attack->hardpoint : attack_hull);
        if (within_range(live.state.position, point, reach.value(), RangeMetric::planar)) {
            // A path is dropped; a turn in place under way (A-04's) carries on (OR-05).
            if (live.motion->kind != MotionKind::path) return core::Result<void>::success();
            if (auto held = impl->plan_order(live, StopPayload{}, command_tick + 1); !held) return failed(held.error());
            if (layer) {
                if (auto submitted = submit(unit_id, live, *layer, std::nullopt); !submitted) {
                    return failed(submitted.error());
                }
            }
            return core::Result<void>::success();
        }
        auto mapped = impl->approach_mapping(live, target, range, command_tick + 1, point);
        if (!mapped) return failed(mapped.error());
        if (auto planned = plan_approach(unit_id, live, mapped.value()); !planned) return failed(planned.error());
        return core::Result<void>::success();
    }
    CollisionWorld collisions;
    const CollisionWorld* planning = nullptr;
    if (tracking && layer && moves) {
        if (auto built = world_for(*layer, collisions); !built) return failed(built.error());
        planning = &collisions;
    }
    // An attack-move or guard of a point plans as the move it is for a ship (OR-11, OR-16).
    const CommandPayload plan_payload =
        moves ? CommandPayload{MovePayload{*point_move(command.payload)}} : command.payload;
    auto planned = impl->plan_order(live, plan_payload, command_tick + 1, planning);
    if (!planned) {
        return failed(planned.error());
    }
    if (layer && live.motion && submits) {
        if (auto submitted = submit(unit_id, live, *layer, std::nullopt); !submitted) return failed(submitted.error());
    }
    return core::Result<void>::success();
}

Order session_detail::Tick::order_for(const CommandPayload& payload, const std::uint64_t tick) {
    Order order;
    order.kind = order_kind(payload);
    order.issued_tick = tick;
    if (const auto* move = std::get_if<MovePayload>(&payload)) {
        order.destination = move->destination;
        order.through_hazards = move->through_hazards;
    } else if (const auto* face = std::get_if<FacePayload>(&payload)) {
        order.destination = face->target;
    } else if (const auto* attack = std::get_if<AttackPayload>(&payload)) {
        order.target = attack->target;
        order.hardpoint = attack->hardpoint;
    } else if (const auto* attack_move = std::get_if<AttackMovePayload>(&payload)) {
        // OP-01: an order that names a unit keeps no point.
        order.target = attack_move->target;
        if (order.target == invalid_entity_id) order.destination = attack_move->destination;
    } else if (const auto* guard = std::get_if<GuardPayload>(&payload)) {
        order.target = guard->target;
        if (order.target == invalid_entity_id) order.destination = guard->destination;
    }
    return order;
}

std::optional<math::Vec3> session_detail::Tick::point_move(const CommandPayload& payload) {
    if (const auto* move = std::get_if<MovePayload>(&payload)) return move->destination;
    if (const auto* attack_move = std::get_if<AttackMovePayload>(&payload);
        attack_move != nullptr && attack_move->target == invalid_entity_id) {
        return attack_move->destination;
    }
    if (const auto* guard = std::get_if<GuardPayload>(&payload); guard != nullptr && guard->target == invalid_entity_id) {
        return guard->destination;
    }
    return std::nullopt;
}

EntityId session_detail::Tick::approach_target_of(const CommandPayload& payload) {
    if (const auto* attack = std::get_if<AttackPayload>(&payload)) return attack->target;
    if (const auto* attack_move = std::get_if<AttackMovePayload>(&payload)) return attack_move->target;
    if (const auto* guard = std::get_if<GuardPayload>(&payload)) return guard->target;
    return invalid_entity_id;
}

} // namespace eawr::sim::tactical
