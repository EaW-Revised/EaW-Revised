#pragma once

#include "eawr/units/unit_tables_common.hpp"
#include "eawr/sim/tactical/abilities.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eawr::units {

struct ModMultiplier final {
    std::string modifier;
    Fixed value;
    bool operator==(const ModMultiplier&) const = default;
};

struct ModFlag final {
    std::string modifier;
    bool value{};
    bool operator==(const ModFlag&) const = default;
};

struct Ability final {
    std::string type;          // upper-case, as the engine's ability enum is spelled
    std::string authored_type; // as written (`power_to_weapons` on the Acclamator)
    std::optional<Fixed> expiration_seconds;
    std::optional<Fixed> recharge_seconds;
    std::vector<ModMultiplier> modifiers;
    std::vector<ModFlag> modifier_flags;
    bool supports_autofire{};
    // #561: the projectile the ability fires instead of the weapons' own (Projectile_Types_Override),
    // resolved to its index in the projectile table.
    std::string projectile_override;
    std::uint32_t projectile_index{no_index};
    std::optional<Fixed> effective_radius;
    std::string gui_activated_ability_name;
    std::optional<Fixed> fixed_inaccuracy{};
    std::optional<Fixed> target_z_offset{};
    std::string spawned_object;
    std::uint32_t spawned_projectile_index{no_index};
    std::optional<Fixed> bomb_countdown_seconds, target_position_z_offset;
    std::string replenish_particle;
    // WHE-78: presentation only; not part of the simulation content identity.
    std::string concentrate_particle;
    // WLU-01: LURE's pulse data as authored (Target_Types names, the pulse period and count, the
    // Is_Pulsing flag the service never reads, and the presentation particle).
    std::vector<std::string> target_types;
    std::optional<Fixed> pulse_frequency_seconds;
    std::optional<std::uint32_t> max_pulses;
    std::optional<bool> is_pulsing;
    std::string lure_particle;
    bool disable_movement{}; // WIN-02, WMS-02: authored movement lock while deployed
    std::optional<Fixed> targeting_max_attack_distance; // WRA-03: active targeting reach
    bool operator==(const Ability&) const = default;
};

// WAB-70/71: a nested Buzz_Droids_Ability as authored (the droid object it creates at a point,
// that object's arming delay, chase and damage radii and per-second damage).
struct BuzzDroids final {
    std::string name;
    std::optional<Fixed> min_range, max_range, chase_radius, damage_radius;
    std::optional<Fixed> enemy_damage_per_second, own_damage_per_second, activation_seconds;
    std::string object_type;
    std::string activate_sfx; // presentation: the droids' own arming cue (WAB-75)
    bool operator==(const BuzzDroids&) const = default;
};

// WCB-01..03: a named self-targeted ring launcher, independent of the unit ability's radius.
struct ClusterBomb final {
    std::string name, bomb_type;
    sim::tactical::SpecialActivationStyle style{};
    std::uint32_t projectile_index{no_index}, count{1};
    Fixed detonation_seconds{Fixed::from_raw(Fixed::scale)};
    bool operator==(const ClusterBomb&) const = default;
};

// WPR-51: automatic combat bonuses authored on an upgrade object.
struct CombatBonus final {
    std::uint32_t stacking_category{};
    std::vector<std::string> types;
    std::vector<std::string> categories;
    std::array<Fixed, 7> percentages{}; // health, damage, energy, shield, defense, speed, fire range
    std::string name;
    sim::tactical::SpecialAbilityFilter filter;
    std::string specific_faction;
    bool enabled{true};
    std::vector<sim::tactical::TypeId> excluded_containers;
    bool operator==(const CombatBonus&) const = default;
};

// WGT-49/50: source-owned player effects; independent from Combat_Bonus_Ability.
struct BattlefieldBonus final {
    std::string name;
    std::uint32_t category{};
    Fixed defence{}, reveal_delta{};
    bool all_allies{}, enabled{true};
    bool operator==(const BattlefieldBonus&) const = default;
};

// WST-03: a Space_Automatic Battlefield_Modifier_Ability's Stealth_Duration_Multiplier, for its owner
// (and its allies with Apply_To_All_Allies).
struct StealthDurationBonus final {
    std::uint32_t stacking_category{};
    Fixed multiplier{};
    bool all_allies{};
    bool operator==(const StealthDurationBonus&) const = default;
};

// WRB-05: an Enable_Ability authored on an upgrade object: it enables the named nested ability on
// the listed types (or categories) of its holder, and of the holder's allies when `all_allies`.
struct AbilityEnable final {
    std::string ability_name;
    std::vector<std::string> types;
    std::vector<std::string> categories;
    bool all_allies{};
    bool operator==(const AbilityEnable&) const = default;
};

namespace table_rows {
struct SpecialUnlock {
    bool enable{}, all_allies{}, reverse{};
    bool operator==(const SpecialUnlock&) const = default;
};
} // namespace table_rows

} // namespace eawr::units
