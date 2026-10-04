#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/world.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <set>
#include <vector>

// Retail space fog cells (#274; docs/behaviour/space-visibility.md V-11 to V-17): the logical
// fog that targeting reads when a session is bound to a map's fog grid.
namespace eawr::sim::tactical {

// A map's space fog grid and its regrow constants: content the setup's content identity names,
// like the sensor table, and neither replay data nor state. Column c covers world X from
// map_left + c * cell_size; row r covers world Y down from map_top - r * cell_size.
struct FogRules {
    math::Fixed map_left{};  // MapLeft
    math::Fixed map_top{};   // MapTop
    math::Fixed cell_size{}; // DesiredSpaceFOWCellSize (100)
    std::uint32_t cells_wide{};
    std::uint32_t cells_tall{};
    // A player's grid is serviced on the ticks t with t % service_period == player ID %
    // service_period. Each service lowers every cell no revealer holds by ramp_down_step.
    std::uint32_t service_period{16};
    std::uint32_t ramp_down_step{21};
    friend constexpr bool operator==(const FogRules&, const FogRules&) noexcept = default;
};

// Retail's ramp-down step for a regrow time (SpaceFOWRegrowTime): floor(238 * 16 / (seconds *
// 30)), exact. 6 s gives 21. Fails with EAWR-SIM-0305 for a time <= 0 or a step of 0.
[[nodiscard]] core::Result<std::uint32_t> fog_ramp_down_step(math::Fixed regrow_seconds);

// Fails with EAWR-SIM-0305 unless the cell size is positive, each side has 1 to 4,096 cells,
// the grid's far edges fit in Q24, the service period is 1 to 65,536 and the step 1 to 238.
[[nodiscard]] core::Result<void> validate_fog_rules(const FogRules& rules);

// One tick's revealer: a live unit with a sensor profile.
struct FogRevealer {
    EntityId id{};
    PlayerId owner{};
    math::Vec3 position{};
    math::Fixed range{};
};

// V-19: a unit that fired this tick while it reveals shows itself to the player it fired at: the
// cell of its position, clamped into the grid, takes the held value in that player's grid alone
// without being held, so it regrows like a released cell (V-15).
struct FogFlash {
    PlayerId to{};
    math::Vec3 position{};
};

// Per-player cell grids, the value copy retail keeps per player, plus each revealer's
// last marked circle. Copies share immutable row buffers; advance stages only rows that
// change, then commits them after every partition succeeds.
class FogCells final {
public:
    FogCells() = default;
    // players in strictly increasing ID order (validated by the session setup).
    FogCells(const FogRules& rules, std::span<const Player> players);

    // One tick, in retail order: services the grids whose phase is `tick` (when `service`),
    // then releases the circles of revealers that are gone and re-marks every revealer that
    // has no circle yet or has moved at least one cell width since its last mark, then applies
    // the tick's flashes (V-19). revealers are in strictly increasing ID. Per-row work is
    // partitioned; the result does not depend on the executor.
    [[nodiscard]] core::Result<void> advance(std::uint64_t tick, bool service,
        std::span<const FogRevealer> revealers, const PartitionExecutor& executor,
        std::span<const FogFlash> flashes = {});

    // V-20: holds the entire grid for this player across ordinary refresh. Transactional,
    // with row assignments partitioned; retained snapshots keep their previous rows.
    [[nodiscard]] core::Result<void> reveal_all(PlayerId player, const PartitionExecutor& executor);

    // Whether a unit at `position` is unfogged for the player_index-th player: its cell is in
    // the grid and its value is above zero.
    [[nodiscard]] bool revealed(std::size_t player_index, const math::Vec3& position) const noexcept;
    // The player_index-th player's cell values, row by row (255 held, 0 fogged).
    [[nodiscard]] std::span<const std::uint8_t> values(std::size_t player_index) const;
    // Immutable presentation rows: retaining them never copies cell bytes.
    [[nodiscard]] std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> value_rows(std::size_t player_index) const;
    // Deterministic work counter, excluding pointer metadata and canonical serialization.
    [[nodiscard]] std::size_t copied_grid_bytes() const noexcept { return copied_grid_bytes_; }
    [[nodiscard]] const FogRules& rules() const noexcept { return rules_; }
    // Hashed state: every anchor in ascending ID, then every player's values.
    void append_state(std::vector<std::uint8_t>& bytes) const;

private:
    struct Anchor {
        std::int32_t column{};
        std::int32_t row{};
        std::int32_t radius{};
        math::Fixed x{};
        math::Fixed y{};
        PlayerId owner{};
        friend constexpr bool operator==(const Anchor&, const Anchor&) noexcept = default;
    };

    FogRules rules_{};
    std::vector<Player> players_;
    std::shared_ptr<const std::map<EntityId, Anchor>> anchors_ = std::make_shared<const std::map<EntityId, Anchor>>();
    using ValueRow = std::shared_ptr<std::vector<std::uint8_t>>;
    using HoldRow = std::shared_ptr<std::vector<std::uint32_t>>;
    std::vector<std::vector<ValueRow>> values_; // player, row, column
    std::vector<std::vector<HoldRow>> holds_; // revealers holding each cell
    mutable std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> flat_values_;
    std::size_t copied_grid_bytes_{};
    std::set<PlayerId> full_reveals_; // V-20; canonical only when nonempty
};

} // namespace eawr::sim::tactical
