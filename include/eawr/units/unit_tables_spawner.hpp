#pragma once

#include "eawr/units/unit_tables_common.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eawr::units {

struct SpawnEntry final {
    std::string squadron;
    std::uint32_t squadron_index{no_index};
    std::int32_t count{};
    bool operator==(const SpawnEntry&) const = default;
};

// SPAWN_SQUADRON lists. The six authored tiers are selected at hangar setup;
// the Tech_0 aliases remain available to existing content consumers.
struct Spawner final {
    std::vector<SpawnEntry> starting;
    std::optional<Fixed> delay_seconds;
    std::vector<SpawnEntry> reserves;
    bool reserves_used{false};
    std::array<std::vector<SpawnEntry>, 6> campaign_starting, campaign_reserves;
    bool operator==(const Spawner&) const = default;
};

struct SquadronMember final {
    std::string craft;
    std::uint32_t craft_index{no_index};
    std::optional<Vec3> offset; // Squadron_Offsets by member order
    bool operator==(const SquadronMember&) const = default;
};

} // namespace eawr::units
