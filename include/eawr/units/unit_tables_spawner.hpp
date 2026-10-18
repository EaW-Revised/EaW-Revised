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

// SPAWN_SQUADRON lists. SK-23 launches `starting` once after
// `delay_seconds`; SK-36 keeps `reserves` read but unused (reserves_used is
// always false in M2).
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
