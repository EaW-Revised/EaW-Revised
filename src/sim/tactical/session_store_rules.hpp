#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreRules {
    TacticalSetup setup;
    std::vector<SensorProfile> sensors;
    DurabilityTable durability;
    // Derived from durability.difficulty at creation, never hashed: one scaled copy of every
    // profile per player whose health or shield scale is not 1, in strictly increasing owner order.
    struct OwnerProfiles {
        PlayerId owner{};
        std::vector<DurabilityProfile> profiles; // parallel to durability.profiles
    };
    std::vector<OwnerProfiles> owner_profiles;
    MotionTable motion;
    CombatTable combat;
    std::map<std::pair<TypeId, std::uint32_t>, std::shared_ptr<const WeaponProfile>> manual_admission_profiles;
    VictoryRules victory;
    AbilityTable abilities;
    EconomyRules economy;                           // #530
    MapBox map_box;                                 // WDF-05: the fighter grids' low corner
    SessionStoreRules(const TacticalSetup& source, std::span<const SensorProfile> sensor_table,
        const DurabilityTable& table, const MotionTable& motion_table, const CombatTable& combat_table,
        const VictoryRules& victory_rules, const AbilityTable& ability_table, const EconomyRules& economy_rules,
        const MapBox& box)
        : setup(source), sensors(sensor_table.begin(), sensor_table.end()), durability(table), motion(motion_table),
          combat(combat_table), victory(victory_rules), abilities(ability_table), economy(economy_rules), map_box(box) {}
};

} // namespace eawr::sim::tactical::session_detail
