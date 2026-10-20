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
        friend bool operator==(const ConcentrateEffect&, const ConcentrateEffect&) = default;
    };
    // Total order over every field: two effects from one source (tractor + concentrate fire) can share
    // (target, category, source), and which comes first reaches the checkpoint bytes and the change test.
    static void sort_concentrate_effects(std::vector<ConcentrateEffect>& effects) {
        std::sort(effects.begin(), effects.end(), [](const ConcentrateEffect& a, const ConcentrateEffect& b) {
            return std::tie(a.target, a.category, a.source, a.kind, a.cause, a.defense, a.speed)
                < std::tie(b.target, b.category, b.source, b.kind, b.cause, b.defense, b.speed);
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
    std::vector<EntityId> projectile_defence_order; // coordinator-reserved PDEF; empty implies static creation order
};

} // namespace eawr::sim::tactical::session_detail
