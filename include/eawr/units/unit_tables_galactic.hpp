#pragma once

#include "eawr/units/unit_tables_abilities.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eawr::units {

// #530 (docs/behaviour/space-purchasing.md): a skirmish station's `Income_Stream_Ability`
// (PU-02, PU-03) and `Income_Stream_Mod_Ability` (PU-04) sub-objects, by Name.
struct IncomeStream final {
    std::string name;
    std::optional<Fixed> base_value;       // Base_Income_Value
    std::optional<Fixed> interval_seconds; // Base_Interval_In_Secs
    bool split_with_allies{};              // Split_Income_With_Allies
    bool full_amount_to_everyone{};        // Full_Amount_To_Everyone
    bool operator==(const IncomeStream&) const = default;
};

struct IncomeBonus final {
    std::string name;
    std::optional<Fixed> additive;   // Income_Additive_Value
    std::optional<Fixed> multiplier; // Income_Multiplier
    std::string target_source;       // Target_Stream_Source
    std::optional<Fixed> interval_multiplier;
    std::string activation_style;
    std::uint32_t stacking_category{};
    bool all_allied_sources{};
    bool reverse{};
    bool operator==(const IncomeBonus&) const = default;
};

// One faction's group of Tactical_Buildable_Objects_Multiplayer (PU-10), in authored order.
struct BuildGroup final {
    std::string faction;
    std::vector<std::string> types;
    bool operator==(const BuildGroup&) const = default;
};

// What a type builds and costs in a skirmish (#530, PU-10 to PU-21, PU-31).
struct Production final {
    // WGCH-164: authored source-owned tactical salvage modifier.
    struct Bounty {
        Fixed percentage{};
        std::int32_t category{};
        std::vector<std::string> factions{};
        bool all_allies{};
        bool operator==(const Bounty&) const = default;
    };
    std::vector<Bounty> bounties{};
    std::vector<BuildGroup> buildable;                   // Tactical_Buildable_Objects_Multiplayer
    std::optional<Fixed> build_cost_multiplayer;         // Tactical_Build_Cost_Multiplayer
    std::vector<BuildGroup> buildable_campaign;          // WGT-26
    std::optional<Fixed> build_cost_campaign;
    std::optional<Fixed> build_time_seconds;             // Tactical_Build_Time_Seconds
    std::string production_queue;                        // Tactical_Production_Queue
    std::optional<std::uint32_t> population_value;       // Population_Value
    std::optional<std::uint32_t> space_override_population_value; // negative authored sentinel becomes absent
    std::optional<Fixed> reinforcement_prevention_radius; // Reinforcement_Prevention_Radius
    std::vector<IncomeStream> income;
    std::vector<IncomeBonus> income_bonuses;
    std::optional<std::uint32_t> lifetime_player;
    std::optional<std::uint32_t> current_player;
    std::optional<std::uint32_t> lifetime_allies;
    std::optional<std::uint32_t> current_allies;
    std::vector<std::string> prerequisites;
    std::string next_level;
    bool upgrade_object{};
    bool level_up{};
    bool increments_tech{};
    std::string removes_previous;
    std::string next_upgrade; // WPR-63: menu metadata, outside content identity
    std::vector<CombatBonus> combat_bonuses;
    std::vector<StealthDurationBonus> stealth_duration_bonuses;
    std::vector<BattlefieldBonus> battlefield_bonuses;
    std::vector<AbilityEnable> ability_enables; // WRB-05
    bool operator==(const Production&) const = default;
};

// Explicit GC player type/name locks; the story/corruption producer is an input seam.
struct CampaignAbilityLock final {
    std::uint32_t player{};
    std::string type, name;
    bool enabled{};
    bool operator==(const CampaignAbilityLock&) const = default;
};

namespace table_rows {
struct CompanyMember {
    std::string type;
    bool named_hero{}, generic_hero{};
    bool attach_to_flagship{};
    std::string unique_space_unit;
    bool operator==(const CompanyMember&) const = default;
};
} // namespace table_rows

} // namespace eawr::units
