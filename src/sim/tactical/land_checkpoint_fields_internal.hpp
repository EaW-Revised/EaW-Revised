#pragma once

#include "checkpoint_archive_internal.hpp"
#include "eawr/sim/tactical/land_movement.hpp"
#include "session_land_internal.hpp"

namespace eawr::sim::tactical::checkpoint_detail {

template <> struct Fields<session_detail::LandCombatState> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.targets); }
};

template <> struct Fields<LandOccupation> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.object, v.primary_passability, v.secondary_passability, v.primary_cells, v.secondary_cells);
    }
};
template <> struct Fields<LandOccupancy::Checkpoint> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.occupations, v.changed, v.revision); }
};
template <> struct Fields<LandPathRequest> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.entity, v.mask, v.start, v.end, v.radius, v.width, v.detour_ratio, v.trivial, v.smooth,
            v.blocked_cells, v.collision_cells, v.allow_collision_edges);
    }
};
template <> struct Fields<LandPath> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.status, v.nodes, v.length); }
};
template <> struct Fields<LandPathStats> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.entity, v.work, v.pops, v.expansions, v.pushes, v.evaluations, v.width_cells, v.sight_cells, v.restarts);
    }
};
template <> struct Fields<LandSearch::Node> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.cell, v.g, v.direction, v.closed); }
};
template <> struct Fields<LandSearch::Entry> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.f, v.g, v.cell); }
};
template <> struct Fields<LandSearch::Checkpoint> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.request, v.phase, v.pass, v.needs_validation, v.restarts, v.waited, v.stats, v.nodes, v.open,
            v.cells, v.kept, v.smooth_anchor, v.smooth_low, v.smooth_high, v.smooth_step, v.check_index, v.smooth_mode, v.result);
    }
};
template <> struct Fields<units::LandCompanyMember> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.kind, v.locomotor, v.movement_class, v.create_team, v.named_hero, v.generic_hero,
            v.unique_ground_unit, v.max_speed);
    }
};
template <> struct Fields<LandLocomotionProfile> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.maximum_speed, v.acceleration, v.deceleration, v.turn_rate, v.radius, v.walk_transition, v.collision_class);
    }
};
template <> struct Fields<LandMotionGates> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.start_almost_finished, v.stop_point, v.stop_almost_finished, v.turn_almost_finished,
            v.animation_prevents_movement, v.deployment_almost_finished, v.turret_aligned);
    }
};
template <> struct Fields<LandMovingMember> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.object, v.squad, v.source, v.profile, v.mask, v.position, v.slot, v.facing, v.speed, v.stop_velocity,
            v.transition_frame, v.deployment_frame, v.state, v.gates, v.collision_class, v.pass_admitted,
            v.path_around_admitted, v.offset_compatible, v.terrain_blocked, v.assigned_speed, v.collision_speed,
            v.collision_lock, v.terrain_target, v.escape_aligned_admitted, v.escape_opposed_admitted, v.progress,
            v.next_node, v.temporary_next, v.detour_path, v.detour_next, v.own_terrain_block);
    }
};
template <> struct Fields<LandMovingSquad> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.object, v.position, v.facing, v.maximum_speed, v.speed, v.rear_extent, v.path, v.next_node, v.order,
            v.following_start, v.progress, v.yielding_to, v.blocker_progress, v.has_formation, v.formation_length,
            v.temporary_path, v.temporary_next, v.temporary_first, v.temporary_second, v.generic_team, v.turning);
    }
};
template <> struct Fields<LandContactSettings> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.complexity_quota, v.reset_interval, v.prediction_seconds, v.minimum_prediction_distance);
    }
};
template <> struct Fields<LandContact> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.first, v.second, v.primary, v.route_actor, v.recorded_member, v.kind, v.state, v.search, v.route,
            v.score, v.terrain_target, v.first_speed_limit, v.second_speed_limit, v.deadlock_remaining,
            v.search_passes, v.first_progress, v.second_progress, v.charged_nodes, v.first_stopped,
            v.second_stopped, v.search_succeeded, v.installed, v.motion_observed, v.escape_retry_pending);
    }
};
template <> struct Fields<LandMovement::Checkpoint> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.tick, v.squads, v.members, v.contact_settings, v.contacts, v.contact_complexity,
            v.last_contact_frame, v.contacts_serviced);
    }
};

} // namespace eawr::sim::tactical::checkpoint_detail
