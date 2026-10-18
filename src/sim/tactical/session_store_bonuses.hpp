#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreBonuses {
    // WPR-51: component-wise largest per stacking category, then sum distinct categories.
    using OwnershipKey = std::pair<PlayerId, TypeId>;
    std::vector<OwnershipKey> ownership_keys;
    std::vector<std::uint64_t> ownership_counts;
    std::vector<std::uint64_t> committed_ownership_counts;
    std::vector<std::uint32_t> ownership_slots; // count_owned scratch: one key slot per unit, workers write disjoint entries
    std::vector<std::uint32_t> bonus_categories;
    std::vector<CombatBonuses> bonus_profiles;
    std::vector<CombatBonuses> committed_bonus_profiles;
    std::vector<CombatBonuses> bonus_parts;
    std::vector<IncomeCategory> income_category_parts;
    struct CommandSource {
        EntityId id{}, host{};
        std::size_t profile{};
        std::vector<EntityId> targets;
        friend bool operator==(const CommandSource&, const CommandSource&) = default;
    };
    struct CommandLedger {
        std::vector<CommandSource> sources;
        std::map<EntityId, std::vector<std::size_t>> recipients;
        std::map<PlayerId, std::vector<std::pair<std::uint32_t, math::Fixed>>> battlefield_defence;
        std::vector<std::pair<PlayerId, math::Fixed>> reveal_multipliers;
    };
    CommandLedger command_ledger;
    // WGT-50: last multiplier consumed by fog, indexed by bounded setup player order.
    std::array<math::Fixed, max_players> last_fog_multipliers{};
    bool fog_modifiers_dirty{}; // derived event hint; no checkpoint field
};

} // namespace eawr::sim::tactical::session_detail
