#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/fog.hpp"
#include "eawr/sim/tactical/space.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Per-player space sensor visibility (P2-05, #68; docs/behaviour/space-visibility.md).
namespace eawr::sim::tactical {

class TacticalSnapshot;

// A unit type's space sensor: its FoC `Space_FOW_Reveal_Range` in Q24 source units, from
// the #65 unit tables. A type without a profile reveals nothing. The table is content
// that the setup's content identity names; it is neither replay data nor state.
struct SensorProfile {
    TypeId type_id{};
    math::Fixed reveal_range{};
    math::Fixed dense_multiplier{math::Fixed::from_raw(math::Fixed::scale / 2)};
    bool multisample{};
    bool reveals{true};
    math::Vec2 half_extents{}; // V-21: scaled hard coordination extents, yaw oriented
    math::Vec2 box_offset{};   // V-21: unscaled authored offset, yaw oriented
    math::Vec3 flash_offset{}; // V-19: model box centre, added without unit yaw
    math::Fixed flash_radius{};
    friend constexpr bool operator==(const SensorProfile&, const SensorProfile&) noexcept = default;
};

// Fails with EAWR-SIM-0305 unless type IDs strictly increase and every range is >= 0.
[[nodiscard]] core::Result<void> validate_sensors(std::span<const SensorProfile> sensors);

// Immutable sensor state of one tick. visible_to is const and safe to call concurrently.
class SensorField final {
public:
    SensorField() = default;

    // players in strictly increasing ID order (at most 64, each unit owner among them),
    // units in any order and a validated sensor table.
    [[nodiscard]] static core::Result<SensorField> build(
        std::span<const Player> players,
        std::span<const UnitState> units,
        std::span<const SensorProfile> sensors,
        std::span<const EntityId> disabled = {}); // WR-35/39: ascending IDs whose reveal service is disabled

    // Bit k is set when players[k] sees a unit of `owner` at `position`: players[k] is on
    // the owner's team, or a unit of players[k]'s team whose type has a sensor profile lies
    // within its reveal range, measured in the source XY plane, inclusive.
    [[nodiscard]] std::uint64_t visible_to(PlayerId owner, const math::Vec3& position) const;
    [[nodiscard]] std::optional<math::Fixed> reveal_range(TypeId type_id) const noexcept;
    [[nodiscard]] const SensorProfile* profile(TypeId type_id) const noexcept;

private:
    [[nodiscard]] std::uint64_t team_mask(PlayerId player) const noexcept;

    std::vector<std::pair<PlayerId, std::uint64_t>> team_masks_; // ascending player ID
    std::vector<SensorProfile> sensors_;
    SpaceIndex observers_;
    std::vector<math::Fixed> observer_ranges_; // aligned with observers_.bodies()
    math::Fixed max_range_{};
};

// Presentation layout of derived fog grids: the Q24 lower corner, cell size and cell
// count of FogGridV1 (docs/replay-format.md). FoC authors DesiredSpaceFOWCellSize 100.
struct FogLayout {
    math::Fixed origin_x{};
    math::Fixed origin_y{};
    math::Fixed cell_x{};
    math::Fixed cell_y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

// The source the fog presentation hook (presentation::fog::TextureCache) reads in place of
// the painted fog stub: one grid per team of the snapshot's players, in ascending team
// order, with revision completed tick + 1. A cell is 255 (unchanged) when its centre, the
// lower corner plus floor(cell / 2) raw on each axis, lies within the planar reveal range
// of an instance of that team, and 0 (dark) otherwise. Fails when the layout or the
// collection breaks a FogGridV1 rule.
[[nodiscard]] core::Result<fog::FogGridSet> fog_grids(const TacticalSnapshot& snapshot, const FogLayout& layout);

} // namespace eawr::sim::tactical
