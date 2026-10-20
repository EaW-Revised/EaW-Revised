#pragma once

#include "../sim/galactic/checkpoint_fields_internal.hpp"
#include "eawr/campaign/game_session.hpp"

namespace eawr::sim::galactic::checkpoint_detail {

template <> struct Fields<::eawr::campaign::Arbitration> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.route, v.forced_tactical, v.overrun);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveAllocation> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.killer, v.owner, v.final_blow_player, v.type, v.retained_base_type, v.killed, v.base_changed, v.base_destroyed, v.escaped_transport);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveCategory> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.name, v.ground);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveCombatant> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.type, v.planet, v.transport, v.named_hero, v.tactical, v.tech, v.hero_type, v.bonus_type, v.has_special_abilities, v.contains_superweapon_killer);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveFleet> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.fleet, v.squadron_type, v.space_capable, v.has_special_abilities);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveLearning> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.id, v.type, v.survived);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveObject> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.owner, v.original_type, v.current_type, v.kind, v.space_capable, v.land_capable, v.tactical, v.has_special_abilities, v.contents, v.fleet_members, v.current_type_id);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolvePlanet> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.owner, v.base_type, v.landed, v.ground_specials, v.built_structures, v.has_special_abilities);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveProgress> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.conflict, v.elapsed_mode_seconds, v.display_seconds, v.combatants, v.roster, v.initiated, v.withdraw_admitted, v.retreating_player);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveProgress::Combatant> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.original_type, v.current_type);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveProgress::SideRoster> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.current, v.escort, v.queue);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveRoster> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.planet, v.objects, v.fleets, v.defender_entries, v.invader_entries, v.orbit_fleets, v.raid_entries, v.tech);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveRoundOutcome> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.winner, v.retreating_player, v.remaining, v.allocations, v.learning, v.retained_queues, v.fix_land_structures, v.superweapon_victory, v.random_picks);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveRoundState> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.start_seconds, v.retreating_player, v.outcome);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveSide> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.owner, v.combatants);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveSideStrength> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.force, v.category_bonuses, v.weakest, v.superweapon, v.superweapon_killer);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveSides> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.sides, v.aggressor, v.planet_owner, v.bombing_side, v.bomber_type, v.terrain_index, v.built_structures, v.layer);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveStrength> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.gathered, v.categories, v.sides);
    }
};

template <> struct Fields<::eawr::campaign::AutoResolveTech> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.player, v.level);
    }
};

template <> struct Fields<::eawr::campaign::BattleSeat> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.seat, v.player);
    }
};

template <> struct Fields<::eawr::campaign::ConflictForce> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.type, v.owner, v.parent, v.role, v.destroy_after_tactical, v.hero, v.tactical_type, v.carrier, v.purchase_type);
    }
};

template <> struct Fields<::eawr::campaign::ConflictRecord> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.id, v.layer, v.planet, v.invader, v.defender, v.raid, v.corruption_mission, v.tutorial, v.ai_tactical_credits, v.forces, v.seats, v.station, v.superweapon, v.station_owner, v.station_type, v.station_level, v.terrain_index, v.map_identity, v.story_plot, v.linked_mission_options, v.structure_markers, v.upgrades, v.tactical, v.corruption_request);
    }
};

template <> struct Fields<::eawr::campaign::RecordedAdmission> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.entry_tick, v.conflict, v.arbitration);
    }
};

template <> struct Fields<::eawr::campaign::RecordedConflict> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.entry_tick, v.conflict);
    }
};

template <> struct Fields<::eawr::campaign::HistoryBoundary> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.tick, v.digest, v.last_key);
    }
};

template <> struct Fields<::eawr::campaign::HistoryBase> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.at, v.executed_dropped, v.native_dropped, v.child_attempts);
    }
};

template <> struct Fields<::eawr::campaign::ReturnedUpgrade> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.type, v.holder, v.owner);
    }
};

} // namespace eawr::sim::galactic::checkpoint_detail
