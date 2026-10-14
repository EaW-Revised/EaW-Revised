#pragma once

#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/units/population.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::units {

struct GalacticSpawn {
    std::string type;
    std::int32_t count{};
    friend bool operator==(const GalacticSpawn&, const GalacticSpawn&) = default;
};
struct GalacticStrengthBonus {
    std::vector<std::string> categories;
    std::optional<sim::math::Fixed> health, damage, enhancement;
    bool defense{};
    friend bool operator==(const GalacticStrengthBonus&, const GalacticStrengthBonus&) = default;
};
struct GalacticInitialStealth {
    std::optional<bool> initially_enabled;
    std::string activation_style;
    std::optional<sim::math::Fixed> evade_chance;
};

// GC original-type values, separate from UnitTables and its tactical identity (ADR-017).
// Absent or malformed values stay absent: consumers supply only walk-verified defaults.
struct GalacticType {
    std::string name;
    std::string object_class;
    std::string file; // diagnostic provenance; excluded from content identity
    std::uint64_t line{0};
    std::optional<sim::math::Fixed> hyperspace_speed; // WGM-26
    std::optional<sim::math::Fixed> population_value; // WGEP-49
    std::optional<sim::math::Fixed> space_override_population_value; // WGT-07: tactical-space getter only
    std::optional<sim::math::Fixed> political_control; // WGM-67/70
    std::optional<std::int64_t> squadron_capacity; // WGM-24/87
    std::optional<sim::math::Fixed> cheap_bribe_cost; // WGM-85
    std::optional<sim::math::Fixed> expensive_bribe_cost;
    std::optional<sim::math::Fixed> build_cost_credits; // WGEP-29
    std::optional<sim::math::Fixed> build_time_seconds; // WGEP-32
    std::optional<sim::math::Fixed> maintenance_cost; // WGA-291: perception only, not a fiscal debit
    std::optional<std::int64_t> tech_level; // WGEP-22
    std::optional<std::int64_t> required_ground_base_level;
    std::optional<std::int64_t> required_star_base_level;
    std::optional<std::int64_t> base_level; // WGEP-24
    std::optional<std::int64_t> additional_population_capacity; // WGEP-48
    std::optional<std::int64_t> build_limit_current_per_player; // WGEP-19/25
    std::optional<std::int64_t> build_limit_lifetime_per_player;
    std::optional<bool> can_hyperspace_without_ability{}; // WGM-83
    std::optional<bool> hyperspace_capable, always_spawn_in_orbit; // WGS-92
    std::optional<bool> build_initially_locked; // WGEP-18/19
    std::optional<bool> build_requires_initial_placement; // WGEP-35
    std::optional<bool> build_time_reduced_by_multiple_factories; // WGEP-34
    std::optional<std::int64_t> build_advances_tech_level; // WGEP-42: unsigned target level, zero means none
    std::string next_level_base; // WGEP-52
    std::string political_faction; // WGM-69
    std::string required_special_structures; // WGEP-21: keep the OR/group grammar for the consumer
    std::vector<std::string> required_orbiting_units;
    std::vector<std::string> required_planets;
    std::vector<std::string> affiliation;
    std::vector<std::string> squadron_units; // AU-82 append, WGM-26 container contents
    std::vector<std::string> company_units;
    std::optional<bool> ground_company_member, squadron_member; // WGA-363/364; unavailable timeline stays absent
    std::string company_transport; // WGEP-78: original composite count link
    std::string additional_creation_type; // WGEP-84
    std::optional<sim::math::Fixed> ai_combat_power; // WAR-19
    std::vector<std::string> category_mask; // WAR-21
    std::string unique_space_unit, unique_ground_unit; // WAR-20
    std::string land_bomber_type; // WAR-16
    std::optional<bool> named_hero, superweapon_killer; // WAR-24/25
    std::string sellable; // GHUD-S84: Sellable_Default, Sellable_Always or Sellable_Never.
    std::optional<bool> stealth_company; // authored metadata; runtime stealth remains an action fact
    std::optional<bool> can_participate_in_space; // WAR-15
    std::optional<bool> stays_in_transport; // WGM-106
    std::vector<std::string> behavior, space_behavior, land_behavior;
    std::array<std::vector<GalacticSpawn>, 6> starting_spawned_units; // WAR-23, authored tech rows
    std::vector<GalacticStrengthBonus> strength_bonuses; // WAR-26
    std::vector<GalacticInitialStealth> initial_stealth; // WGS-84: fresh galactic ability admission
};

struct GalacticTypeTable {
    std::vector<GalacticType> types; // catalog order, active winners only; Planet is in GalacticData
    std::vector<data::galactic::Issue> issues;

    [[nodiscard]] const GalacticType* find(std::string_view name) const noexcept;
};
// WGEP-158 / GHUD-S90: the authored ground-construction predicate selects
// the production domain; build categories and company representation do not.
[[nodiscard]] bool built_on_ground(const GalacticType&) noexcept;
// WGS-92: resolved inheritance precedes shared constructor defaults. Invalid
// consumed numeric quantities refuse instead of becoming omission defaults.
[[nodiscard]] core::Result<GalacticType> resolve_initial_galactic_type(
    const GalacticTypeTable&, std::string_view name);

struct GalacticReductionAbility {
    bool price{};
    std::optional<bool> initially_enabled{true};
    std::string activation_style{"Galactic_Automatic"};
    std::optional<bool> galaxy_wide;
    std::optional<std::int64_t> category;
    std::optional<sim::math::Fixed> reduction;
    std::vector<std::string> included_categories, excluded_categories, included_types, excluded_types;
};
struct GalacticTechnologyPriceAbility {
    std::string name;
    std::optional<bool> initially_enabled{true};
    std::string activation_style{"Galactic_Automatic"};
    std::optional<sim::math::Fixed> reduction{sim::math::Fixed{}};
};
struct GalacticEconomicType {
    std::string name, object_class;
    std::vector<data::galactic::SystemSpyDefinition> system_spies;
    std::optional<bool> mining_colony;
    std::optional<sim::math::Fixed> mining_percentage, mining_absolute, mining_multiplier;
    std::optional<sim::math::Fixed> gambling_chance, gambling_minimum, gambling_maximum, gambling_interval;
    bool mining{}, gambling{};
    // WGEP-163: Galactic_Sabotage_Ability halt profile. Both tags are mod-authored only
    // (the effective FoC data never sets them); an absent duration is unqualified, not zero seconds by rule.
    std::optional<bool> credit_halt;
    std::optional<sim::math::Fixed> credit_halt_seconds;
    GalacticInitialStealth mining_activation{true, "Galactic_Automatic", {}}; // WGEP-94 economic admission
    GalacticInitialStealth gambling_activation{true, "Galactic_Automatic", {}};
    std::vector<GalacticReductionAbility> reductions;
    std::vector<GalacticTechnologyPriceAbility> technology_prices;
};
struct GalacticEconomicTable {
    std::vector<GalacticEconomicType> types;
    std::vector<data::galactic::Issue> issues;
};
// WGEP-10/11/73: separate identity surface, including Planet definitions.
[[nodiscard]] GalacticEconomicTable galactic_economic_table(const data::Catalog&);

// No tactical table construction, models, game constants or simulation side effects.
[[nodiscard]] GalacticTypeTable galactic_type_table(const data::Catalog& catalog, bool include_planets = false);
[[nodiscard]] std::array<std::uint8_t, 32> galactic_content_identity(const GalacticTypeTable& table);

} // namespace eawr::units
