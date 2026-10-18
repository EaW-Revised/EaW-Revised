#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreAbilityLocks {
    NamedAbilityLocks named_ability_locks;
    // Derived from canonical unit types; sparse lifecycle hooks maintain it during a battle.
    using RadarRoster = std::map<EntityId, std::vector<std::size_t>>;
    RadarRoster radar_roster;
};

struct SessionStoreAbilityEffects {
    struct ConcentrateEffect {
        EntityId target{}, source{};
        std::uint32_t category{};
        math::Fixed defense{};
        math::Fixed speed{};
        AbilityKind kind{AbilityKind::concentrate_fire};
        math::Fixed cause{};
        friend bool operator==(const ConcentrateEffect&, const ConcentrateEffect&) = default;
    };
    // Derived source references; only activation recruits, service never scans recipients.
    std::vector<ConcentrateEffect> concentrate_effects;
    std::set<EntityId> concentrate_changed_targets;
    std::size_t concentrate_service_census{};
    EntityId concentrate_service_last_id{};
    mutable std::map<EntityId, std::vector<CombatBonuses>> impact_bonus_bases;
    mutable std::map<EntityId, TypeId> impact_bonus_containers;
    mutable bool impact_bonus_containers_ready{};
    std::vector<EntityId> beam_replans;
    std::vector<AbilitySpawnState> ability_spawns; // ascending shared projectile IDs; HABL
    std::vector<BuzzDroidState> buzz_droids; // WAB-71..74: ascending droid entity IDs; BUZZ
};

struct SessionStoreSpecialWeapons {
    std::map<PlayerId, ManualPlayerClock> manual_clocks; // WAD-40: shared across stations
    std::map<EntityId, SpecialWeaponState> special_weapons; // WSW-14/19: persistent planetary sources.
    std::set<PlayerId> special_disabled_by_hardpoint; // WSW-48: absence is the default enabled permission
    std::vector<EntityId> projectile_defence_order; // coordinator-reserved PDEF; empty implies static creation order
};

} // namespace eawr::sim::tactical::session_detail
