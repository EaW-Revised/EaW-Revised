#pragma once

#include "checkpoint_archive_internal.hpp"
#include "eawr/sim/galactic/session.hpp"
#include "eawr/sim/galactic/story_engine.hpp"

namespace eawr::sim::galactic::checkpoint_detail {

template <> struct Fields<::eawr::sim::galactic::HistoryValue> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.bits);
    }
};

template <> struct Fields<::eawr::sim::galactic::HistorySample> {
    template <class A, class T> static void apply(A& a, T& v) {
        // GREF-26: keep the stored order and floating bit patterns.
        a(v.frame, v.economy, v.planets, v.technology, v.military);
    }
};

template <> struct Fields<::eawr::sim::SeatId> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.value);
    }
};

template <> struct Fields<::eawr::sim::galactic::AbilityActorState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.actor, v.type, v.enabled, v.cancelled, v.theft_enabled, v.theft_cancelled, v.corruption_enabled, v.corruption_cancelled, v.hack_enabled, v.hack_cancelled, v.destruction_enabled, v.destruction_cancelled, v.sabotage_enabled, v.sabotage_cancelled, v.neutralize_enabled, v.neutralize_cancelled, v.contested_check_disabled);
    }
};

template <> struct Fields<::eawr::sim::galactic::AbilityPreparation> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.actor, v.planet, v.offers, v.absence, v.open, v.kind, v.theft_choices, v.corruption_choices, v.target_choices);
    }
};

template <> struct Fields<::eawr::sim::galactic::AbilityResult> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.request, v.preparation, v.actor, v.planet, v.player, v.item, v.outcome, v.kind, v.technology);
    }
};

template <> struct Fields<::eawr::sim::galactic::ActorAbilityState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.present, v.combat_skip, v.self_target, v.evade_chance);
    }
};

template <> struct Fields<::eawr::sim::galactic::BattleRoleChoice> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.conflict, v.human_player, v.ai_player, v.choosing_side, v.observer_player, v.role);
    }
};

template <> struct Fields<::eawr::sim::galactic::BlackMarketBuyer> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.buyer, v.owner, v.present, v.opposing_planet, v.causes_despawn, v.price_modifier, v.absence_min, v.absence_max, v.despawn_requested, v.prepared_absence);
    }
};

template <> struct Fields<::eawr::sim::galactic::BlackMarketMenu> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.buyer, v.planet, v.offers, v.absence, v.active);
    }
};

template <> struct Fields<::eawr::sim::galactic::CampaignScoreLedger> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.builds, v.deaths, v.changes, v.quits);
    }
};

template <> struct Fields<::eawr::sim::galactic::CampaignScoreLedger::BuildKey> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.planet, v.type);
    }
};

template <> struct Fields<::eawr::sim::galactic::CampaignScoreLedger::ChangeKey> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.old_owner, v.new_owner);
    }
};

template <> struct Fields<::eawr::sim::galactic::CampaignScoreLedger::DeathKey> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.killer, v.type);
    }
};

template <> struct Fields<::eawr::sim::galactic::CapturedPlanetType> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.type);
    }
};

template <> struct Fields<::eawr::sim::galactic::CensusObject> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.original_type, v.owner, v.planet, v.container, v.dead, v.delete_pending, v.current_type, v.generated_source, v.combat_role, v.abilities, v.garrison, v.paid_cost);
    }
};

template <> struct Fields<::eawr::sim::galactic::CommandOrderKey> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.tick, v.seat, v.sequence);
    }
};

template <> struct Fields<::eawr::sim::galactic::ControlBase> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.level);
    }
};
template <> struct Fields<::eawr::sim::galactic::ControlAbility> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.protection, v.enabled, v.category, v.multiplier);
    }
};
template <> struct Fields<::eawr::sim::galactic::ControlAbilityType> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.abilities);
    }
};
template <> struct Fields<::eawr::sim::galactic::ControlAbilitySource> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.actor, v.type, v.ability, v.owner);
    }
};

template <> struct Fields<::eawr::sim::galactic::ControlCap> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.maximum);
    }
};

template <> struct Fields<::eawr::sim::galactic::ControlPlanet> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.type, v.surface_accessible, v.retains_residual, v.destroyed, v.ground_base, v.starbase, v.landed, v.production_owner, v.leaning_player, v.leaning, v.special_control, v.caps, v.transition_modifiers, v.capture_reward, v.built_structures);
    }
};

template <> struct Fields<::eawr::sim::galactic::ControlPlayer> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.neutral, v.playable, v.faction);
    }
};

template <> struct Fields<::eawr::sim::galactic::ControlRelation> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.other, v.allied);
    }
};

template <> struct Fields<::eawr::sim::galactic::ControlRoute> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.route, v.political_gain);
    }
};

template <> struct Fields<::eawr::sim::galactic::ControlTransitionModifier> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.category, v.source, v.multiplier, v.expires_frame);
    }
};

template <> struct Fields<::eawr::sim::galactic::ControlType> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.political_control, v.stealth, v.ground_capable);
    }
};

template <> struct Fields<::eawr::sim::galactic::CorruptionEconomyPlanet> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.corruption, v.influenced, v.destroyed, v.factor, v.piracy, v.market_modifier, v.siphon);
    }
};

template <> struct Fields<::eawr::sim::galactic::CorruptionEconomyPlayer> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.faction, v.benefits, v.playable, v.human, v.price, v.time, v.tech_purchase_multiplier, v.technology_price_modifiers, v.displayed_tech_adjustment, v.unlocks);
    }
};

template <> struct Fields<::eawr::sim::galactic::CorruptionUsage> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.uses);
    }
};

template <> struct Fields<::eawr::sim::galactic::CreditHaltAbility> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.seconds);
    }
};

template <> struct Fields<::eawr::sim::galactic::DetachedUnit> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.member, v.owner, v.planet);
    }
};

template <> struct Fields<::eawr::sim::galactic::EconomicPlanet> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.mining_colony, v.destroyed, v.intimidation);
    }
};

template <> struct Fields<::eawr::sim::galactic::EconomicReduction> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.category, v.reduction, v.price, v.galaxy_wide);
    }
};

template <> struct Fields<::eawr::sim::galactic::EconomicSource> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.type, v.planet, v.owner, v.planet_source, v.enabled);
    }
};

template <> struct Fields<::eawr::sim::galactic::EconomicType> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.mining, v.gambling, v.reductions, v.credit_halt, v.technology_price_reduction);
    }
};

template <> struct Fields<::eawr::sim::galactic::EconomyCorruptionState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.income_factors, v.factor_min, v.factor_max, v.tutorial, v.multiplayer, v.players, v.planets, v.items, v.buyers, v.menus, v.random_draws);
    }
};

template <> struct Fields<::eawr::sim::galactic::FactionController> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.kind, v.dropout, v.configuration, v.state);
    }
};

template <> struct Fields<::eawr::sim::galactic::Fleet> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.owner, v.orbit, v.members, v.orbit_slot, v.motion, v.raid_destination);
    }
};

template <> struct Fields<::eawr::sim::galactic::FleetMember> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.type);
    }
};

template <> struct Fields<::eawr::sim::galactic::FleetMotion> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.position, v.facing, v.transit, v.orbit_entry);
    }
};

template <> struct Fields<::eawr::sim::galactic::FleetProductionBlocker> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.fleet, v.pending_deletion, v.skips_space_combat, v.cancels_starbase_production);
    }
};

template <> struct Fields<::eawr::sim::galactic::FleetTransit> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.path, v.hop, v.desired, v.velocity, v.base_speed, v.speed, v.packed, v.destination_slot, v.producer, v.sequence, v.desired_facing, v.destination_actor);
    }
};

template <> struct Fields<::eawr::sim::galactic::GalacticAbilityState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.next_request, v.actors, v.preparations, v.results, v.transitions, v.missions, v.corruption_usage, v.planet_entitlements, v.sabotage_history, v.neutralization_history);
    }
};

template <> struct Fields<::eawr::sim::galactic::GalacticClock> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.session_tick, v.galactic_frame, v.tactical_frame, v.pause_depth, v.fiscal_start, v.fiscal_end, v.fiscal_cycle, v.strategic_speed, v.tactical_speed);
    }
};

template <> struct Fields<::eawr::sim::galactic::GalacticEvent> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.tick, v.kind, v.producer, v.subject, v.sequence, v.value, v.reason);
    }
};

template <> struct Fields<::eawr::sim::galactic::GalacticPlayerEconomy> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.credits, v.credit_cap, v.current_tech, v.maximum_tech);
    }
};

template <> struct Fields<::eawr::sim::galactic::GalacticSession::NativeActorChange> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.kind, v.actor, v.source, v.dead, v.deleting, v.death_clone);
    }
};

template <> struct Fields<::eawr::sim::galactic::GalacticVictoryCondition> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.kind, v.player, v.delay, v.parameters, v.occurred, v.suppress_text);
    }
};

template <> struct Fields<::eawr::sim::galactic::GamblingIncomeAbility> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.chance, v.minimum, v.maximum, v.interval_frames);
    }
};

template <> struct Fields<::eawr::sim::galactic::GamblingIncomeState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.source, v.planet, v.deadline, v.income, v.scheduled, v.planet_owner);
    }
};

template <> struct Fields<::eawr::sim::galactic::GroundCapabilityType> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.combat_power, v.transport, v.stays_in_transport);
    }
};

template <> struct Fields<::eawr::sim::galactic::IncomeBlocker> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.source, v.first_frame, v.first_expired_frame);
    }
};

template <> struct Fields<::eawr::sim::galactic::IncomeSetup> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.types, v.planets, v.sources, v.offers);
    }
};

template <> struct Fields<::eawr::sim::galactic::IncomeSetup::OfferBasis> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.type, v.price, v.seconds);
    }
};

template <> struct Fields<::eawr::sim::galactic::IncomeState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.inputs, v.gambling, v.random_draws, v.blockers);
    }
};

template <> struct Fields<::eawr::sim::galactic::IntelligenceSource> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.source, v.ability, v.text_identity, v.mask, v.final_frame);
    }
};

template <> struct Fields<::eawr::sim::galactic::LiveCorruptionMission> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.request, v.actor, v.planet, v.player, v.choice, v.state);
    }
};

template <> struct Fields<::eawr::sim::galactic::LiveCorruptionTransition> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.transition);
    }
};

template <> struct Fields<::eawr::sim::galactic::MovementAccess> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.planet, v.accessible, v.potential_conflict);
    }
};

template <> struct Fields<::eawr::sim::galactic::MovementContents> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.container, v.children);
    }
};

template <> struct Fields<::eawr::sim::galactic::MovementPlanet> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.bounds_half_z, v.corrupted);
    }
};

template <> struct Fields<::eawr::sim::galactic::MovementPlayer> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.hyperspace_factor, v.corruption_bonus, v.benefits_from_corruption);
    }
};

template <> struct Fields<::eawr::sim::galactic::RaidRules> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.required_faction, v.free_categories, v.limited_categories, v.max_heroes, v.max_limited);
    }
};
template <> struct Fields<::eawr::sim::galactic::RaidType> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.categories, v.hero, v.stays_in_transport);
    }
};
template <> struct Fields<::eawr::sim::galactic::RaidSetup> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.rules, v.types); }
};

template <> struct Fields<::eawr::sim::galactic::MovementSetup> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.types, v.contents, v.players, v.planets, v.access, v.mode_modifier, v.raid);
    }
};

template <> struct Fields<::eawr::sim::galactic::MovementType> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.hyperspace_speed, v.build_cost, v.squadron_capacity, v.hyperspace_eligible, v.squadron, v.fleet_container, v.skips_combat, v.transport);
    }
};

template <> struct Fields<::eawr::sim::galactic::NativeEffect> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.frontier, v.sequence, v.phase, v.kind, v.payload);
    }
};

template <> struct Fields<::eawr::sim::galactic::NativeInvasion> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.state, v.request, v.invader, v.defender, v.fleet, v.planet, v.transports, v.conflict, v.success, v.reader_released);
    }
};

template <> struct Fields<::eawr::sim::galactic::NestedCreationRelation> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.other, v.enemy);
    }
};

template <> struct Fields<::eawr::sim::galactic::NestedCreationSetup> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.fleet_type, v.types, v.relations, v.squadron_type);
    }
};

template <> struct Fields<::eawr::sim::galactic::NestedCreationType> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.transport, v.role, v.members, v.skips_combat, v.always_spawn_in_orbit, v.combat_role, v.abilities);
    }
};

template <> struct Fields<::eawr::sim::galactic::ObserverLandedTransport> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.owner, v.delete_pending, v.dead);
    }
};

template <> struct Fields<::eawr::sim::galactic::ObserverPlanet> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.allegiance, v.target_owner, v.space_owner, v.ground_owner, v.space_level, v.ground_level, v.corrupted, v.space_stealth, v.ground_stealth, v.automatic_reveal, v.landed_transports, v.political_control, v.current_owner, v.current_controller);
    }
};

template <> struct Fields<::eawr::sim::galactic::ObserverPlayer> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.ai, v.benefits_from_corruption, v.default_intelligence, v.minimum_visible_base_level, v.allies, v.canonical_empire);
    }
};

template <> struct Fields<::eawr::sim::galactic::ObserverRecord> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.planet, v.discovered, v.reveal_counter, v.refresh_frame, v.mask, v.sources, v.apparent_control, v.apparent_owner);
    }
};

template <> struct Fields<::eawr::sim::galactic::ObserverSetup> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.human_observers, v.solo_reference_human, v.reveal_delay_seconds, v.automatic_reveal_range, v.players, v.planets, v.types, v.records, v.logical_fps, v.planet_spies, v.history_players);
    }
};

template <> struct Fields<::eawr::sim::galactic::ObserverType> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.stealth_object, v.stealth_hero, v.superweapon);
    }
};

template <> struct Fields<::eawr::sim::galactic::Planet> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.position, v.owner);
    }
};

template <> struct Fields<::eawr::sim::galactic::PlanetAbilityEntitlements> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.player, v.automatic_upgrades, v.tactical_upgrades, v.build_unlocks);
    }
};

template <> struct Fields<::eawr::sim::galactic::PlanetAccess> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.restricted, v.local_revealed);
    }
};

template <> struct Fields<::eawr::sim::galactic::PlanetAccessState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.generation, v.planets);
    }
};

template <> struct Fields<::eawr::sim::galactic::PlanetControl> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.initialized, v.current_player, v.target_player, v.controller, v.orbital_player, v.current, v.target, v.contested, v.blockaded, v.political_income);
    }
};

template <> struct Fields<::eawr::sim::galactic::PlanetIncomeAbility> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.percentage, v.absolute, v.colony_multiplier);
    }
};

template <> struct Fields<::eawr::sim::galactic::PlanetProduction> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.target_owner, v.starbase_owner, v.ground_occupier, v.orbit_occupier, v.starbase_type, v.ground_level, v.space_level, v.maximum_space_level, v.ground_slots, v.space_slots, v.landed_transports, v.maximum_ground_forces, v.enabled, v.surface_accessible, v.starbase_blocked, v.offers, v.queues, v.objects);
    }
};

template <> struct Fields<::eawr::sim::galactic::PlanetSpy> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.ability, v.text_identity, v.mask, v.duration, v.influence_range, v.style, v.enabled, v.despawning);
    }
};

template <> struct Fields<::eawr::sim::galactic::PlayerEconomySetup> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.players);
    }
};

template <> struct Fields<::eawr::sim::galactic::PoliticalControlEffect> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.old_owner, v.new_owner, v.owner_changed, v.stop_production, v.refresh_visibility, v.first_capture, v.capture_reward);
    }
};

template <> struct Fields<::eawr::sim::galactic::PoliticalControlSetup> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.change_seconds, v.max_ground_forces, v.players, v.relations, v.types, v.planets, v.routes, v.state, v.captured_types, v.absent_forces, v.ground_capability_types, v.ability_types, v.ability_sources);
    }
};

template <> struct Fields<::eawr::sim::galactic::PopulationTotal> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.capacity, v.used, v.reserved, v.uses_cap);
    }
};

template <> struct Fields<::eawr::sim::galactic::PreparedCorruption> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.slot);
    }
};

template <> struct Fields<::eawr::sim::galactic::PreparedTarget> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.target, v.company, v.price, v.absence, v.target_absence, v.valid);
    }
};

template <> struct Fields<::eawr::sim::galactic::PreparedTheft> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.technology, v.price, v.chance, v.absence, v.advance);
    }
};

template <> struct Fields<::eawr::sim::galactic::ProductionCount> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.owned, v.historical);
    }
};

template <> struct Fields<::eawr::sim::galactic::ProductionEntry> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.builder, v.build_id, v.price, v.full_seconds, v.last_remaining, v.start_frame, v.duration_frames, v.pay_as_you_go);
    }
};

template <> struct Fields<::eawr::sim::galactic::ProductionObject> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.type, v.owner, v.paid_cost, v.domain, v.kind);
    }
};

template <> struct Fields<::eawr::sim::galactic::ProductionOffer> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.domain, v.kind, v.price, v.seconds, v.population, v.tech, v.required_ground_level, v.required_space_level, v.base_level, v.target_tech, v.planet_limit, v.current_limit, v.lifetime_limit, v.affiliated, v.cinematic, v.initially_locked, v.initially_placed, v.requires_initial_placement, v.direct_build_allowed, v.surface_exception, v.slicer_unlockable, v.required_structures, v.required_orbiting_units, v.authored_price, v.sellable);
    }
};

template <> struct Fields<::eawr::sim::galactic::ProductionPlayer> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.human, v.pay_as_you_go, v.uses_population_cap, v.population_capacity, v.population_used, v.counts, v.locked, v.unlocked);
    }
};

template <> struct Fields<::eawr::sim::galactic::ProductionSetup> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.maximum_human_queue, v.players, v.planets, v.fleet_blockers, v.nested_creation, v.technology_types);
    }
};

template <> struct Fields<::eawr::sim::galactic::Route> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.from, v.to, v.hs_speed_factor);
    }
};

template <> struct Fields<::eawr::sim::galactic::NeutralizationHistory> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.company, v.attacker, v.count);
    }
};

template <> struct Fields<::eawr::sim::galactic::SabotageHistory> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.count);
    }
};

template <> struct Fields<::eawr::sim::galactic::Seat> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.seat_id, v.player, v.kind, v.lobby_slot, v.active, v.human_side_id);
    }
};

template <> struct Fields<::eawr::sim::galactic::SeatControlSetup> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.teams, v.controllers);
    }
};

template <> struct Fields<::eawr::sim::galactic::SeatControlState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.control, v.battle);
    }
};

template <> struct Fields<::eawr::sim::galactic::SurfaceForce> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.type, v.owner);
    }
};

template <> struct Fields<::eawr::sim::galactic::TacticalBuiltStructure> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.position, v.enable_level, v.owner);
    }
};

template <> struct Fields<::eawr::sim::galactic::TeamLead> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.side, v.lead, v.observer_ai);
    }
};

template <> struct Fields<::eawr::sim::galactic::TechnologyPriceModifier> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.source, v.additive, v.first_expired_frame);
    }
};

template <> struct Fields<::eawr::sim::galactic::TechnologyType> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.tech, v.initially_locked, v.slicer_unlockable, v.affiliated_players);
    }
};

template <> struct Fields<::eawr::sim::galactic::VictoryContext> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.multiplayer, v.tutorial_delayed, v.fiscal_cycle, v.players, v.planets, v.relations, v.types, v.superweapon_killers, v.leaders);
    }
};

template <> struct Fields<::eawr::sim::galactic::VictoryLeader> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.company, v.unit, v.scheduled_return, v.dead, v.marked_for_death, v.deletion_pending, v.killer, v.object);
    }
};

template <> struct Fields<::eawr::sim::galactic::VictoryParameters> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.credits, v.cycles, v.control_fraction, v.factions, v.leaders, v.planets);
    }
};

template <> struct Fields<::eawr::sim::galactic::VictoryPlanet> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.type, v.current, v.target, v.owner, v.destroyed);
    }
};

template <> struct Fields<::eawr::sim::galactic::VictoryPlayer> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.faction, v.neutral, v.playable, v.human, v.ai, v.credits, v.primary_enemy, v.home, v.leader_company, v.leader_unit, v.planet_leader_unit, v.leader_unit_type, v.planet_leader_type);
    }
};

template <> struct Fields<::eawr::sim::galactic::VictoryState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.context, v.installed, v.pending, v.remaining, v.last_service, v.waits, v.use_retry, v.retry_waiting, v.result_dispatched, v.queued);
    }
};

template <> struct Fields<::eawr::sim::galactic::VictoryTest> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.kind, v.player);
    }
};

template <> struct Fields<::eawr::sim::galactic::VictoryType> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.name, v.type);
    }
};

template <> struct Fields<::eawr::sim::galactic::corruption_fixture::MarketItem> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.faction, v.tech, v.tutorial_tech, v.base_cost, v.tutorial_base_cost, v.uncommon_multiplier);
    }
};

template <> struct Fields<::eawr::sim::galactic::corruption_fixture::Offer> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.item, v.price, v.owned);
    }
};

template <> struct Fields<::eawr::sim::galactic::corruption_fixture::Siphon> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.active, v.recipient, v.fraction, v.end);
    }
};

template <> struct Fields<::eawr::sim::galactic::corruption_fixture::Transition> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.current, v.target, v.increasing, v.decreasing, v.start, v.end, v.last_change);
    }
};

template <> struct Fields<::eawr::sim::math::Vec3> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.x, v.y, v.z);
    }
};

template <> struct Fields<::eawr::sim::galactic::story::EventState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.name, v.triggered, v.active, v.disabled, v.start, v.clock, v.clock_last, v.tech_primed, v.tech_last, v.idle_stamped, v.idle_stamp);
    }
};

template <> struct Fields<::eawr::sim::galactic::story::SubPlotState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.name, v.owner, v.active, v.events);
    }
};

template <> struct Fields<::eawr::sim::galactic::story::State> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.started, v.now, v.sequence, v.subplots, v.flags);
    }
};

} // namespace eawr::sim::galactic::checkpoint_detail
