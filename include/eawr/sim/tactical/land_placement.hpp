#pragma once

#include "eawr/sim/tactical/land_path.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Land destinations (docs/behaviour/land-pathfinding.md, LP-18..LP-23): the movement class a group shares,
// the connected zones of a class, the nearest open position around a blocked destination and the width of a
// formation. Pure functions of the terrain, the committed occupancy and their inputs.
namespace eawr::sim::tactical {

// WLPA-24/27: the multiplier 20 on a company's formation spacing is a code constant of the original, not the terrain pitch.
inline constexpr std::int64_t land_formation_spacing_scale = 20;
// WLPA-25: at most this many tries (rings) around a destination.
inline constexpr std::uint32_t land_placement_tries = 20;

// WLPA-12: a group moves with the intersection of its members' movement masks. Empty input has no mask.
[[nodiscard]] std::optional<std::uint32_t> land_common_mask(std::span<const std::uint32_t> member_masks) noexcept;
// WLPA-12: the intersection must still be an authored movement class; otherwise the group has no class (a
// distinct failure, never the least restrictive member's mask).
[[nodiscard]] bool land_is_authored_class(const LandTerrain& terrain, std::uint32_t mask) noexcept;

// WLPA-13, WLPA-25: the 8-connected components of the cells a mask admits, with the search's corner rule, so
// two cells share a zone exactly when a footprint-free search can join them. Zone 0 is "not admitted".
class LandZones final {
public:
    [[nodiscard]] static LandZones build(const LandOccupancy& occupancy, std::uint32_t mask);
    // Rebuilds in place, keeping the buffers' capacity.
    void rebuild(const LandOccupancy& occupancy, std::uint32_t mask);
    [[nodiscard]] std::uint32_t mask() const noexcept { return mask_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::uint32_t zone_of(std::uint32_t cell) const noexcept {
        return cell < zones_.size() ? zones_[cell] : 0;
    }
    [[nodiscard]] std::uint32_t zone_count() const noexcept { return count_; }
    [[nodiscard]] std::size_t cell_count() const noexcept { return zones_.size(); }
    [[nodiscard]] bool connected(std::uint32_t a, std::uint32_t b) const noexcept {
        const auto zone = zone_of(a);
        return zone != 0 && zone == zone_of(b);
    }
private:
    std::uint32_t mask_{}, count_{}, columns_{};
    std::uint64_t revision_{};
    std::vector<std::uint32_t> zones_, queue_;
};

// Serial preparation, parallel reads: prepare() runs at the commit point (it rebuilds the zones of a class
// whose occupancy revision moved); partitioned workers only call find().
class LandZoneCache final {
public:
    void prepare(const LandOccupancy& occupancy, std::span<const std::uint32_t> masks);
    [[nodiscard]] const LandZones* find(std::uint32_t mask) const noexcept;
private:
    std::vector<LandZones> zones_;
};

// WLPA-25: further rejections the caller owns (object collisions and predicted-segment overlap are land
// local movement's), asked for a candidate position after the grid gates passed.
class LandPlacementFilter {
public:
    virtual ~LandPlacementFilter() = default;
    [[nodiscard]] virtual bool rejects(math::Vec2 position) const = 0;
};

struct LandPlacementQuery {
    std::uint32_t mask{};
    math::Vec2 from{};     // the mover: its zone is the connectivity origin and candidates are tried nearest it first
    math::Vec2 desired{};
    math::Fixed radius{};  // the hard radius of the object (WLPA-24 padding is added to it)
    math::Fixed formation_spacing{};  // the authored spacing; zero when the type has none
    // Candidates must share the mover's zone (WLPA-25). Without zones the gate is off; a mover on ground its mask does not admit has no zone, which also switches the gate off.
    const LandZones* zones{};
    const LandPlacementFilter* filter{};
};
enum class LandPlacementStatus : std::uint8_t {
    exact,     // the desired position itself is legal
    adjusted,  // a position on a later ring
    none,      // no legal position within the tries: the desired position is unchanged (a distinct result)
};
struct LandPlacement {
    LandPlacementStatus status{LandPlacementStatus::none};
    math::Vec2 position{};
    std::uint32_t tries{};  // rings looked at, the first being the desired position itself
};
// WLPA-24: the larger of a quarter of the hard radius and 0.3 times the spacing times the scale; zero radius
// and spacing give no padding.
[[nodiscard]] math::Fixed land_placement_padding(math::Fixed radius, math::Fixed formation_spacing) noexcept;
[[nodiscard]] LandPlacement land_find_open_position(const LandOccupancy& occupancy, const LandPlacementQuery& query);

// WLPA-29: the base width of a formation of `count` members: the larger of ceil(sqrt(count)) rows of the
// largest member plus a quarter, and sqrt(max(1, 1.8 * area_sum)). One member is just its extent. The
// extents and the area sum come from the members' authored geometry.
[[nodiscard]] math::Fixed land_formation_width(std::uint32_t count, math::Fixed largest_extent, math::Fixed area_sum) noexcept;

} // namespace eawr::sim::tactical
