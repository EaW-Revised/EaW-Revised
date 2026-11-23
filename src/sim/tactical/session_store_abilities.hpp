#pragma once

#include "session_types.hpp"

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

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
        // Projectile_Combat_Mod_*: a timed modifier (CombatBonuses index `stat`, `amount`, until `expires`);
        // `category` is the projectile type key, so one live modifier exists per target and key.
        bool modifier{};
        std::uint32_t stat{};
        math::Fixed amount{};
        std::uint64_t expires{};
        friend bool operator==(const ConcentrateEffect&, const ConcentrateEffect&) = default;
    };
    // Total order over every field: two effects from one source (tractor + concentrate fire) can share
    // (target, category, source), and which comes first reaches the checkpoint bytes and the change test.
    static void sort_concentrate_effects(std::vector<ConcentrateEffect>& effects) {
        std::sort(effects.begin(), effects.end(), [](const ConcentrateEffect& a, const ConcentrateEffect& b) {
            return std::tie(a.target, a.category, a.source, a.kind, a.cause, a.defense, a.speed, a.modifier, a.stat, a.amount, a.expires)
                < std::tie(b.target, b.category, b.source, b.kind, b.cause, b.defense, b.speed, b.modifier, b.stat, b.amount, b.expires);
        });
    }
    // Sorts `next` and, when it differs from the stored effects, marks both sets of targets changed.
    void commit_concentrate_effects(std::vector<ConcentrateEffect> next) {
        sort_concentrate_effects(next);
        if (next == concentrate_effects) return;
        for (const auto& effect : concentrate_effects) concentrate_changed_targets.insert(effect.target);
        for (const auto& effect : next) concentrate_changed_targets.insert(effect.target);
        concentrate_effects = std::move(next);
    }
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
    std::map<EntityId, TswState> tsw_states; // TSW-02: one record per tactical super weapon object; saved and hashed only with a TSW setup
    TswWorld tsw_world; // TSW-09/10: the victory hold, the destroyed targets and the planet fact; saved and hashed with the records
    std::vector<EntityId> projectile_defence_order; // coordinator-reserved PDEF; empty implies static creation order
};

} // namespace eawr::sim::tactical::session_detail
