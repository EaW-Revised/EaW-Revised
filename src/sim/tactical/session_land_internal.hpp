#pragma once

#include "eawr/sim/tactical/content.hpp"

#include "eawr/sim/world.hpp"

#include <array>
#include <map>
#include <vector>

namespace eawr::sim::tactical::session_detail {

// Remake work policy, independent of machine speed and the retail graph's units.
inline constexpr std::uint64_t land_session_path_budget = 4096;
inline constexpr std::uint64_t land_session_path_slice = 256;

// Stable allocation: searches retain references to this occupancy and arena.
struct LandSession final {
    LandSessionContent content;
    LandOccupancy occupancy;
    LandPathArena arena;
    LandSliceScheduler scheduler;
    LandMovement movement;
    std::map<EntityId, LandSearch> searches;
    std::vector<LandSearch*> search_order;
    std::map<EntityId, LandPathRequest> requests;
    std::map<EntityId, std::pair<TypeId, PlayerId>> bindings; // derived from setup expansion, never archived
    std::map<EntityId, UnitState> static_roots; // immutable setup poses, rederived on restore
    // Retained canonical-encoding scratch, never state: per-partition record bytes (worker phase) and the
    // previous tail size. Cleared and refilled every tick; capacity persists so the tick does not allocate.
    using EncodedParts = std::array<std::vector<std::uint8_t>, tick_partition_count>;
    EncodedParts occupation_parts, squad_parts, member_parts;
    std::size_t canonical_size_hint{};
    explicit LandSession(LandSessionContent source, LandOccupancy field)
        : content(std::move(source)), occupancy(std::move(field)) {}
};

} // namespace eawr::sim::tactical::session_detail
