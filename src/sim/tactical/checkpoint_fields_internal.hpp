#pragma once

#include "checkpoint_access_internal.hpp"

// Child-state v7 / raw domain 9 field order. Changes require a section version change.
namespace eawr::sim::tactical::checkpoint_detail {

template <> struct Fields<AbilitySlot> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.active, v.autofire, v.started_tick, v.expires_tick, v.ready_tick, v.target, v.target_hardpoint, v.position);
    }
};

template <> struct Fields<AbilitySpawnState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.type, v.kind, v.owner, v.source, v.position, v.rotation, v.due, v.detonated, v.recipients);
    }
};

template <> struct Fields<AbilityState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.slots, v.replan_due, v.special, v.concentrate_recruits, v.stealth_on, v.stealth_detected, v.stealth_end, v.self_destruct_script, v.pending_buzz_position, v.charged_blast, v.mc30_script);
    }
};

template <> struct Fields<session_detail::Approach> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.prediction_frame);
    }
};

template <> struct Fields<session_detail::PositionHistory> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.previous, v.before_previous);
    }
};

template <> struct Fields<session_detail::Sighting> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.target, v.point);
    }
};

template <> struct Fields<ArrivalState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.frame, v.exit, v.direction);
    }
};

template <> struct Fields<BattleEconomyCue> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.type, v.tick, v.kind, v.position, v.visible_to);
    }
};

template <> struct Fields<BattleLoss> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.type, v.killer, v.count, v.tick);
    }
};

template <> struct Fields<BattleOutcome> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.condition, v.winner, v.winner_team, v.decided_tick, v.deciding_unit, v.end_tick);
    }
};

template <> struct Fields<BattleProduction> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.type, v.tick);
    }
};

template <> struct Fields<BlastProfile> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.damage, v.radius, v.dropoff, v.tiers, v.max_victims, v.immune_faction, v.max_delay);
    }
};

template <> struct Fields<detail::BlastRecipient> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.amount, v.delay, v.hardpoint);
    }
};

template <> struct Fields<BuzzDroidState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.droid, v.source_type, v.start, v.active_from);
    }
};

template <> struct Fields<CarriedObject> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.entity_id, v.type_id, v.parent, v.named_hero, v.generic_hero, v.limbo, v.model_visible, v.collidable, v.selected, v.movement_coordinated, v.combat_preserved, v.members);
    }
};

template <> struct Fields<CombatCell> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.x, v.y);
    }
};

template <> struct Fields<CombatState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.attack_target, v.direct, v.attack_hardpoint, v.next_scan_frame, v.weapons);
    }
};

template <> struct Fields<Access::CommandSource> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.host, v.profile, v.targets);
    }
};

template <> struct Fields<CompletedBuild> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.station, v.object, v.income_modifiers, v.level_up_service_frame);
    }
};

template <> struct Fields<Access::ConcentrateEffect> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.target, v.source, v.category, v.defense, v.speed, v.kind, v.cause);
    }
};

template <> struct Fields<ConstructionState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.parent, v.builder, v.finish_frame, v.increment, v.start_frame);
    }
};

template <> struct Fields<CorruptSystemsState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.source_type, v.started_tick, v.expires_tick, v.drain_per_frame, v.inaccuracy);
    }
};

template <> struct Fields<CraftState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.roll, v.pitch, v.yaw, v.velocity, v.flipping, v.chase, v.chase_until, v.relaxation);
    }
};

template <> struct Fields<DeathSpin> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.unit, v.type, v.owner, v.position, v.roll, v.pitch, v.yaw, v.velocity, v.spin_roll, v.path, v.points, v.lengths, v.travelled);
    }
};

template <> struct Fields<DurabilityProfile> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type_id, v.max_hull, v.max_speed, v.destroyed_with_hardpoints, v.hardpoints, v.max_shields, v.shield_refresh, v.armor_type, v.shield_armor_type, v.allow_diminishing_firepower, v.powered, v.max_energy, v.energy_refresh, v.ion_stun_effect, v.damage_tracking, v.scenario_invulnerable);
    }
};

template <> struct Fields<DurabilityState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.hull, v.hardpoints, v.shields, v.depleted_frame, v.last_hit_frame, v.energy, v.next_shield_frame, v.next_energy_frame, v.disabled, v.repairing_players, v.engines_disabled_until, v.ion_storm_contact, v.window_damage, v.damage_rate);
    }
};

template <> struct Fields<FlightProfile> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.kind, v.target_radius, v.lifetime, v.authored_distance, v.curve_distance, v.curve_offset, v.straight_distance);
    }
};

template <> struct Fields<FlightState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.profile, v.origin, v.aim, v.age_frames, v.path_distance, v.path_initialized, v.shield_redirected, v.shield_allowance, v.path);
    }
};

template <> struct Fields<Footprint> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type_id, v.layer, v.x_extent, v.y_extent, v.radius, v.obstacle, v.asteroid_field, v.ion_storm, v.nebula, v.impassable_asteroid, v.obstacle_offset, v.asteroid_damage, v.locomotor);
    }
};

template <> struct Fields<session_detail::FormationWait> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.frame, v.destination, v.max_speed, v.order, v.rank, v.sliced, v.failure_frame);
    }
};

template <> struct Fields<FreeGarrisonSetup> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.delay_frames, v.templates, v.registered);
    }
};

template <> struct Fields<Access::FreeGarrisonState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.setup, v.registered, v.due_frame, v.pending);
    }
};

template <> struct Fields<HardpointProfile> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.role, v.destroyable, v.max_health, v.repair_amount_per_frame, v.repair_cost_per_frame, v.allows_special_weapon_use, v.special_ability_name);
    }
};

template <> struct Fields<IncomeModifierState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.initialized, v.next_scan_frame, v.attached);
    }
};

template <> struct Fields<IonStunShot> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.frames, v.speed_reduction, v.rate_reduction, v.stack);
    }
};

template <> struct Fields<IonStunState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.end_frame, v.speed_reduction, v.rate_reduction);
    }
};

template <> struct Fields<session_detail::LiveUnit> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.state, v.durability, v.motion, v.speed, v.roll, v.combat, v.formation, v.abilities, v.approach, v.ion_stun, v.arrival_vulnerable_until, v.upgrade_bonuses, v.upgraded_durability, v.asteroid_contact, v.nebula, v.corrupt, v.waypoints, v.garrison_unit, v.sighting, v.position_history);
    }
};

template <> struct Fields<ManualPlayerClock> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.last_fired_frame, v.cooldown_frames);
    }
};

template <> struct Fields<ManualWeaponState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.target, v.requesting_player, v.assigned_frame, v.yaw, v.pitch, v.desired_yaw, v.desired_pitch);
    }
};

template <> struct Fields<math::Mat3x4> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.rows);
    }
};

template <> struct Fields<MotionProfile> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type_id, v.max_speed, v.acceleration, v.deceleration, v.rate_of_turn, v.turn_in_place_slowdown, v.roll_rate, v.bank_angle);
    }
};

template <> struct Fields<MotionState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.kind, v.start_tick, v.start_position, v.start_yaw, v.start_speed, v.target, v.nodes);
    }
};

template <> struct Fields<session_detail::NebulaContact> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.frame, v.present);
    }
};

template <> struct Fields<OpportunityState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.target, v.last_scan_frame);
    }
};

template <> struct Fields<Order> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.kind, v.issued_tick, v.destination, v.target, v.hardpoint, v.through_hazards);
    }
};

template <> struct Fields<PadState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.target, v.progress, v.under_construction, v.constructed, v.cooldown_until, v.cooldown_start, v.contents_locked);
    }
};

template <> struct Fields<Access::PathInputs> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.position, v.yaw, v.speed, v.limits);
    }
};

template <> struct Fields<PathNode> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.frame, v.position, v.yaw, v.speed);
    }
};

template <> struct Fields<Access::PendingBlastDamage> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.due, v.owner, v.damage_type, v.internal_damage_misc, v.recipient, v.amount, v.contacted_mesh);
    }
};

template <> struct Fields<session_detail::PlacementCache> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.position, v.rotation, v.matrix);
    }
};

template <> struct Fields<PlayerEconomy> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.credits, v.queues, v.pool, v.completed, v.income_residuals, v.pool_version, v.pool_additions, v.pool_addition_frame, v.lifetime, v.tech_level, v.pool_tokens, v.next_pool_token, v.next_queue_entry_id, v.campaign_extra_releases, v.collected_bounty);
    }
};

template <> struct Fields<PlayerQuit> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.tick);
    }
};

template <> struct Fields<session_detail::PopulationShare> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.share);
    }
};

template <> struct Fields<Projectile> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.shooter, v.owner, v.weapon, v.target, v.target_hardpoint, v.position, v.step, v.speed, v.travelled, v.max_travel, v.damage, v.damage_type, v.shield_damage, v.hitpoint_damage, v.allow_diminishing_firepower, v.internal_damage_misc, v.homing, v.locked, v.turn_rate, v.yaw, v.pitch, v.offset, v.energy_damage, v.ion_stun, v.blast, v.explosion_requested, v.source_damage_factor, v.disable_engines_frames, v.flight, v.muzzle_delay_until, v.damage_delay, v.profile_key, v.reflected);
    }
};

template <> struct Fields<ProjectileProfileKey> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.weapon, v.kind);
    }
};

template <> struct Fields<math::Quat> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.x, v.y, v.z, v.w);
    }
};

template <> struct Fields<QueueEntry> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.station, v.paid, v.frames, v.complete_frame, v.entry_id);
    }
};

template <> struct Fields<TacticalSession::ReinforcementSearch> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.token, v.attempt);
    }
};

template <> struct Fields<TacticalSession::ReinforcementSearchResult> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.next_attempt, v.candidates, v.predictions, v.position, v.valid);
    }
};

template <> struct Fields<session_detail::RespawnBatch> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.objects);
    }
};

template <> struct Fields<RespawnState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.owner, v.position, v.rotation);
    }
};

template <> struct Fields<RocketSegment> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.a, v.b, v.c, v.d, v.length);
    }
};

template <> struct Fields<SelfDestructScript> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.last_service, v.nearby, v.threat, v.armed);
    }
};

template <> struct Fields<SpawnEntryState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.alive, v.remaining);
    }
};

template <> struct Fields<SpawnerState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.next_service_frame, v.next_spawn_frame, v.ready, v.entries);
    }
};

template <> struct Fields<SpecialAbilitySlot> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.enabled, v.cancelled, v.despawn_success, v.next_service_frame, v.context, v.targets);
    }
};

template <> struct Fields<SpecialAbilityState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.service_cancelled, v.slots);
    }
};

template <> struct Fields<SpecialActivationContext> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.target, v.mode, v.owner_exists, v.type_exists, v.death_clone, v.map_editor);
    }
};

template <> struct Fields<SpecialWeaponState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.recharge_started, v.recharge_until, v.next_pulse_frame, v.target, v.pulses_left, v.instances);
    }
};

template <> struct Fields<Squadron> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.container, v.members);
    }
};

template <> struct Fields<SquadronFormationState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.base_position, v.base_target, v.complete, v.has_reached_done, v.attack_override, v.attack_target, v.attack_hardpoint);
    }
};

template <> struct Fields<SquadronLane> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.formation, v.origin, v.direction, v.ahead, v.aside);
    }
};

template <> struct Fields<SquadronState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.container, v.squadron_type, v.spawner, v.entry, v.roster, v.mode, v.escorted, v.anchor, v.target, v.target_hardpoint, v.next_scan_frame, v.move_origin, v.approach, v.diversion, v.cell, v.joined, v.idle_cell, v.lane, v.formation, v.garrison_unit, v.attack_path);
    }
};

template <> struct Fields<SquadronPathTracker> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.segment, v.started, v.turn_scale, v.speed_scale);
    }
};

template <> struct Fields<SquadronAttackRoute> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.identity, v.nodes, v.trackers, v.aside, v.start_facing, v.ahead, v.node_speeds);
    }
};

template <> struct Fields<SquadronAttackPath> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.target, v.cached_position, v.next_refresh_frame, v.collision_identity, v.stack_changed_frame,
            v.escorted_destination, v.initialization_pending, v.failure_frame, v.stack);
    }
};

template <> struct Fields<StarbaseEntry> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.unit, v.owner);
    }
};

template <> struct Fields<TrackedLeaf> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.entity, v.start, v.end, v.facing, v.x_extent, v.y_extent, v.reach, v.collision);
    }
};

template <> struct Fields<TrackingLayerView> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.start_frame, v.windows);
    }
};

template <> struct Fields<UnitState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.entity_id, v.type_id, v.owner, v.position, v.rotation, v.order, v.purchase_type, v.contained, v.purchase_token, v.barrage_source, v.garrison_enabled);
    }
};

template <> struct Fields<math::Vec2> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.x, v.y);
    }
};

template <> struct Fields<math::Vec3> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.x, v.y, v.z);
    }
};

template <> struct Fields<WeakenRecipient> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.target, v.expires);
    }
};

template <> struct Fields<WeaponState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.opportunity, v.countdown, v.pulses_left, v.next_muzzle, v.manual, v.turret);
    }
};

template <> struct Fields<ChargedBlastState> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.charge_remaining, v.shots_remaining, v.charged_presentation); }
};

template <> struct Fields<Mc30Script> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.last_service, v.nearby, v.threat, v.human, v.armed); }
};

template <> struct Fields<WeaponState::TurretState> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.target, v.yaw, v.pitch, v.desired_yaw, v.desired_pitch, v.ready, v.rest_started_frame, v.returning_to_rest); }
};

template <> struct Fields<MovePayload> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.destination, v.through_hazards, v.append); }
};

template <> struct Fields<session_detail::Waypoint> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.move, v.key, v.repeat); }
};

template <> struct Fields<session_detail::WaypointRoute> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.coordinator, v.points, v.active); }
};

template <> struct Fields<DepartureControl> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.player, v.controller, v.tick, v.basic_ai); }
};

template <> struct Fields<BeaconState> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.player, v.type, v.position, v.placed_tick, v.expires_tick, v.sequence); }
};

template <> struct Fields<SnapshotPlayer> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.player_id, v.team_id, v.neutral, v.radar_enabled); }
};

} // namespace eawr::sim::tactical::checkpoint_detail
