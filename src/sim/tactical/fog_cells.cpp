#include "eawr/sim/tactical/fog_cells.hpp"

#include "../replay_internal.hpp"
#include "../math/wide.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace eawr::sim::tactical {
namespace {

constexpr std::uint32_t max_cells_per_side = 4096;
constexpr std::uint32_t max_service_period = 65536;
constexpr std::uint8_t held_value = 255;
// Retail ramps a released cell to at most 238 at its first service (FogOfWar ramp table).
constexpr std::uint8_t first_ramp_value = 238;
// Retail reveals with at least 10 source units (the debug build's reveal call).
constexpr std::int64_t minimum_range_raw = std::int64_t{10} * math::Fixed::scale;

struct Span {
    std::int32_t row{};
    std::int32_t first{};
    std::int32_t last{};
};

// A revealer's circle change: the circle it held (if any) and the one it marks now (if any).
struct Change {
    PlayerId owner{};
    std::vector<Span> released;
    std::vector<Span> marked;
};

[[nodiscard]] constexpr std::uint64_t distance_along(const std::int64_t left, const std::int64_t right) noexcept {
    return left >= right
        ? static_cast<std::uint64_t>(left) - static_cast<std::uint64_t>(right)
        : static_cast<std::uint64_t>(right) - static_cast<std::uint64_t>(left);
}

// Cells from `origin` to `value` along an axis, floor((value - origin) / cell); nothing when
// value lies before the origin.
[[nodiscard]] std::optional<std::uint64_t> cells_from(
    const std::int64_t origin, const std::int64_t value, const std::int64_t cell) noexcept {
    if (value < origin) {
        return std::nullopt;
    }
    return distance_along(value, origin) / static_cast<std::uint64_t>(cell);
}

[[nodiscard]] std::int32_t clamp_cell(const std::optional<std::uint64_t> cell, const std::uint32_t count) noexcept {
    if (!cell) {
        return 0;
    }
    return static_cast<std::int32_t>(std::min<std::uint64_t>(*cell, count - 1U));
}

// int(range / cell + 0.5) for range >= 10 units (V-13), capped at the grid's width plus
// height: a larger circle around a cell of the grid covers it whole.
[[nodiscard]] std::int32_t radius_cells(const math::Fixed range, const FogRules& rules) noexcept {
    const auto magnitude = static_cast<std::uint64_t>(std::max(range.raw(), minimum_range_raw));
    const auto cell = static_cast<std::uint64_t>(rules.cell_size.raw());
    const auto rounded = magnitude / cell + (2U * (magnitude % cell) >= cell ? 1U : 0U);
    return static_cast<std::int32_t>(
        std::min<std::uint64_t>(rounded, std::uint64_t{rules.cells_wide} + rules.cells_tall));
}

// Retail's midpoint (Bresenham) circle, filled row by row between the extreme points the
// eight octants plot on each row (V-14). Rows and columns outside the grid are clipped.
[[nodiscard]] std::vector<Span> circle(
    const std::int32_t column, const std::int32_t row, const std::int32_t radius, const FogRules& rules) {
    const std::int64_t r = radius;
    const std::int64_t cx = column;
    const std::int64_t cy = row;
    std::vector<std::pair<std::int64_t, std::int64_t>> extent(static_cast<std::size_t>(2 * r + 1),
        {std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min()});
    const auto plot = [&](const std::int64_t at_row, const std::int64_t at_column) {
        auto& span = extent[static_cast<std::size_t>(at_row - (cy - r))];
        span.first = std::min(span.first, at_column);
        span.second = std::max(span.second, at_column);
    };
    const auto points = [&](const std::int64_t x, const std::int64_t y) {
        plot(cy + y, cx + x);
        plot(cy + x, cx + y);
        plot(cy - x, cx + y);
        plot(cy - y, cx + x);
        plot(cy - y, cx - x);
        plot(cy - x, cx - y);
        plot(cy + x, cx - y);
        plot(cy + y, cx - x);
    };
    std::int64_t x = 0;
    std::int64_t y = r;
    std::int64_t decision = 1 - r;
    std::int64_t delta_e = 3;
    std::int64_t delta_se = 5 - 2 * r;
    points(x, y);
    while (x < y) {
        if (decision < 0) {
            decision += delta_e;
            delta_se += 2;
        } else {
            decision += delta_se;
            delta_se += 4;
            --y;
        }
        delta_e += 2;
        ++x;
        points(x, y);
    }
    std::vector<Span> spans;
    const std::int64_t last_column = static_cast<std::int64_t>(rules.cells_wide) - 1;
    for (std::size_t index = 0; index < extent.size(); ++index) {
        const auto at_row = cy - r + static_cast<std::int64_t>(index);
        if (at_row < 0 || at_row >= static_cast<std::int64_t>(rules.cells_tall)) {
            continue;
        }
        const auto first = std::max<std::int64_t>(extent[index].first, 0);
        const auto last = std::min(extent[index].second, last_column);
        if (first <= last) {
            spans.push_back(Span{static_cast<std::int32_t>(at_row), static_cast<std::int32_t>(first),
                static_cast<std::int32_t>(last)});
        }
    }
    return spans;
}

[[nodiscard]] core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, std::move(message)));
}

} // namespace

core::Result<std::uint32_t> fog_ramp_down_step(const math::Fixed regrow_seconds) {
    // 238 / (seconds * fps / 16) = 238 * 16 * 2^24 / (raw * 30), exact in 192 bits.
    if (regrow_seconds.raw() <= 0) {
        return core::Result<std::uint32_t>::failure(
            detail::diagnostic(diagnostic_codes::invalid_setup, "fog regrow time must be positive"));
    }
    const auto numerator = math::detail::shift_left_u64(238U * 16U, math::Fixed::fractional_bits);
    const auto denominator = math::detail::multiply_u64(static_cast<std::uint64_t>(regrow_seconds.raw()),
        logical_frames_per_second);
    math::detail::UInt192 quotient{};
    math::detail::UInt192 remainder{};
    math::detail::divide(numerator, denominator, quotient, remainder);
    if (quotient.is_zero()) {
        return core::Result<std::uint32_t>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "fog regrow time is so long that cells never regrow"));
    }
    // The quotient is at most 238 * 16 * 2^24 / 30, well below 2^32.
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(quotient.limb[0]));
}

core::Result<void> validate_fog_rules(const FogRules& rules) {
    if (rules.cell_size.raw() <= 0) {
        return invalid("fog rules: the cell size must be positive");
    }
    if (rules.cells_wide == 0 || rules.cells_tall == 0 || rules.cells_wide > max_cells_per_side
        || rules.cells_tall > max_cells_per_side) {
        return invalid("fog rules: each side has 1 to 4096 cells");
    }
    const auto cell = static_cast<std::uint64_t>(rules.cell_size.raw());
    // The far edges must be representable: map_left + wide * cell and map_top - tall * cell.
    const auto width = math::detail::multiply_u64(cell, rules.cells_wide);
    const auto height = math::detail::multiply_u64(cell, rules.cells_tall);
    const auto room_right = distance_along(std::numeric_limits<std::int64_t>::max(), rules.map_left.raw());
    const auto room_down = distance_along(rules.map_top.raw(), std::numeric_limits<std::int64_t>::min());
    if (math::detail::compare(width, math::detail::from_u64(room_right)) > 0
        || math::detail::compare(height, math::detail::from_u64(room_down)) > 0) {
        return invalid("fog rules: the grid extends past the Q24 range");
    }
    if (rules.service_period == 0 || rules.service_period > max_service_period) {
        return invalid("fog rules: the service period is 1 to 65536 ticks");
    }
    if (rules.ramp_down_step == 0 || rules.ramp_down_step > first_ramp_value) {
        return invalid("fog rules: the ramp-down step is 1 to 238");
    }
    return core::Result<void>::success();
}

FogCells::FogCells(const FogRules& rules, const std::span<const Player> players)
    : rules_(rules), players_(players.begin(), players.end()) {
    const auto value_row = std::make_shared<std::vector<std::uint8_t>>(rules.cells_wide, 0U);
    const auto hold_row = std::make_shared<std::vector<std::uint32_t>>(rules.cells_wide, 0U);
    values_.assign(players_.size(), std::vector<ValueRow>(rules.cells_tall, value_row));
    holds_.assign(players_.size(), std::vector<HoldRow>(rules.cells_tall, hold_row));
    flat_values_.resize(players_.size());
}

core::Result<void> FogCells::advance(const std::uint64_t tick, const bool service,
    const std::span<const FogRevealer> revealers, const PartitionExecutor& executor,
    const std::span<const FogFlash> flashes) {
    // Re-mark decisions: each worker reads the committed anchors and writes its revealers'
    // slots (V-12). A revealer marks when it has no circle yet or has moved at least one cell
    // width in the plane since it last marked.
    std::vector<std::optional<Anchor>> marks(revealers.size());
    const auto cell = static_cast<std::uint64_t>(rules_.cell_size.raw());
    const auto one_cell = math::detail::multiply_u64(cell, cell);
    const auto decided = executor.execute_phase("fog-reveal", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, revealers.size());
        for (auto index = range.begin; index < range.end; ++index) {
            const auto& revealer = revealers[index];
            const auto found = anchors_->find(revealer.id);
            if (found != anchors_->end()) {
                const auto dx = distance_along(revealer.position.x.raw(), found->second.x.raw());
                const auto dy = distance_along(revealer.position.y.raw(), found->second.y.raw());
                auto moved = math::detail::multiply_u64(dx, dx);
                static_cast<void>(math::detail::add_magnitude(moved, math::detail::multiply_u64(dy, dy)));
                if (math::detail::compare(moved, one_cell) < 0) {
                    continue;
                }
            }
            marks[index] = Anchor{
                clamp_cell(cells_from(rules_.map_left.raw(), revealer.position.x.raw(), rules_.cell_size.raw()),
                    rules_.cells_wide),
                clamp_cell(cells_from(revealer.position.y.raw(), rules_.map_top.raw(), rules_.cell_size.raw()),
                    rules_.cells_tall),
                radius_cells(revealer.range, rules_),
                revealer.position.x,
                revealer.position.y,
                revealer.owner,
            };
        }
    });
    if (!decided) {
        return decided;
    }

    // Serial, in ascending ID: the circles to release (revealers gone or re-marking) and mark.
    auto anchors = *anchors_;
    std::vector<Change> changes;
    std::size_t next = 0;
    for (auto iterator = anchors.begin(); iterator != anchors.end();) {
        while (next < revealers.size() && revealers[next].id < iterator->first) {
            ++next;
        }
        if (next < revealers.size() && revealers[next].id == iterator->first) {
            ++iterator;
            continue;
        }
        const auto& gone = iterator->second;
        changes.push_back(Change{gone.owner, circle(gone.column, gone.row, gone.radius, rules_), {}});
        iterator = anchors.erase(iterator);
    }
    for (std::size_t index = 0; index < revealers.size(); ++index) {
        if (!marks[index]) {
            continue;
        }
        const auto& mark = *marks[index];
        Change change{mark.owner, {}, circle(mark.column, mark.row, mark.radius, rules_)};
        if (const auto held = anchors.find(revealers[index].id); held != anchors.end()) {
            change.released = circle(held->second.column, held->second.row, held->second.radius, rules_);
        }
        anchors.insert_or_assign(revealers[index].id, mark);
        changes.push_back(std::move(change));
    }

    // Which players each change reveals for: every player on its owner's team.
    std::vector<std::vector<std::size_t>> allies(changes.size());
    for (std::size_t index = 0; index < changes.size(); ++index) {
        const auto owner = std::find_if(players_.begin(), players_.end(),
            [&](const Player& player) { return player.player_id == changes[index].owner; });
        for (std::size_t player = 0; player < players_.size(); ++player) {
            if (owner != players_.end() && players_[player].team_id == owner->team_id) {
                allies[index].push_back(player);
            }
        }
    }
    // V-19: each flash's cell, in the order given; a player outside the table sees none.
    struct FlashCell {
        std::size_t player{};
        std::int32_t column{};
        std::int32_t row{};
    };
    std::vector<FlashCell> flash_cells;
    for (const auto& flash : flashes) {
        const auto to = std::find_if(players_.begin(), players_.end(),
            [&](const Player& player) { return player.player_id == flash.to; });
        if (to == players_.end()) {
            continue;
        }
        flash_cells.push_back(FlashCell{static_cast<std::size_t>(to - players_.begin()),
            clamp_cell(cells_from(rules_.map_left.raw(), flash.position.x.raw(), rules_.cell_size.raw()), rules_.cells_wide),
            clamp_cell(cells_from(flash.position.y.raw(), rules_.map_top.raw(), rules_.cell_size.raw()), rules_.cells_tall)});
    }
    std::vector<bool> serviced(players_.size(), false);
    for (std::size_t player = 0; player < players_.size(); ++player) {
        serviced[player] = service && tick % rules_.service_period == players_[player].player_id % rules_.service_period;
    }

    // Cells, by row bands: each worker owns whole rows of every grid. The service runs first
    // (V-15), then each change in ascending revealer ID releases and marks its circle (V-11).
    auto values = values_;
    auto holds = holds_;
    const auto wide = static_cast<std::size_t>(rules_.cells_wide);
    const auto step = rules_.ramp_down_step;
    // Row buffers are immutable inputs; each partition stages only its own changed rows.
    std::vector<std::size_t> copied(tick_partition_count);
    // Disjoint row flags share one allocation per grid, including empty partition bands.
    std::vector<std::uint8_t> value_staged(players_.size() * rules_.cells_tall);
    std::vector<std::uint8_t> hold_staged(players_.size() * rules_.cells_tall);
    const auto applied = executor.execute_phase("fog-cells", tick_partition_count, [&](const std::size_t partition) {
        const auto band = partition_range(partition, rules_.cells_tall);
        const auto in_band = [&](const Span& span) {
            return static_cast<std::size_t>(span.row) >= band.begin && static_cast<std::size_t>(span.row) < band.end;
        };
        const auto write_value = [&](const std::size_t player, const std::size_t row, const std::size_t column, const std::uint8_t value) {
            if ((*values[player][row])[column] == value) return;
            auto& staged = value_staged[player * rules_.cells_tall + row];
            if (staged == 0U) {
                values[player][row] = std::make_shared<std::vector<std::uint8_t>>(*values[player][row]);
                copied[partition] += wide * sizeof(std::uint8_t);
                staged = 1U;
            }
            (*values[player][row])[column] = value;
        };
        const auto write_hold = [&](const std::size_t player, const std::size_t row, const std::size_t column, const bool add) {
            auto& staged = hold_staged[player * rules_.cells_tall + row];
            if (staged == 0U) {
                holds[player][row] = std::make_shared<std::vector<std::uint32_t>>(*holds[player][row]);
                copied[partition] += wide * sizeof(std::uint32_t);
                staged = 1U;
            }
            auto& hold = (*holds[player][row])[column];
            if (add) ++hold; else --hold;
        };
        // Keep service, releases, marks and flashes in their original order (V-11, V-15, V-19).
        for (std::size_t player = 0; player < players_.size(); ++player) {
            if (!serviced[player]) continue;
            for (auto row = band.begin; row < band.end; ++row) {
                for (std::size_t column = 0; column < wide; ++column) {
                    const auto value = (*values[player][row])[column];
                    const auto held = (*holds[player][row])[column];
                    write_value(player, row, column, held != 0U ? held_value : value > first_ramp_value ? first_ramp_value
                        : value > step ? static_cast<std::uint8_t>(value - step) : 0U);
                }
            }
        }
        for (std::size_t index = 0; index < changes.size(); ++index) {
            for (const auto player : allies[index]) {
                for (const auto& span : changes[index].released) {
                    if (!in_band(span)) continue;
                    for (auto column = span.first; column <= span.last; ++column) {
                        write_hold(player, static_cast<std::size_t>(span.row), static_cast<std::size_t>(column), false);
                    }
                }
                for (const auto& span : changes[index].marked) {
                    if (!in_band(span)) continue;
                    for (auto column = span.first; column <= span.last; ++column) {
                        write_hold(player, static_cast<std::size_t>(span.row), static_cast<std::size_t>(column), true);
                        write_value(player, static_cast<std::size_t>(span.row), static_cast<std::size_t>(column), held_value);
                    }
                }
            }
        }
        for (const auto& flash : flash_cells) {
            if (static_cast<std::size_t>(flash.row) < band.begin || static_cast<std::size_t>(flash.row) >= band.end) continue;
            write_value(flash.player, static_cast<std::size_t>(flash.row), static_cast<std::size_t>(flash.column), held_value);
        }
    });
    if (!applied) {
        return applied;
    }
    anchors_ = std::make_shared<const std::map<EntityId, Anchor>>(std::move(anchors));
    copied_grid_bytes_ = 0;
    for (const auto bytes : copied) copied_grid_bytes_ += bytes;
    for (auto& cached : flat_values_) cached.reset();
    values_ = std::move(values);
    holds_ = std::move(holds);
    return core::Result<void>::success();
}

bool FogCells::revealed(const std::size_t player_index, const math::Vec3& position) const noexcept {
    const auto column = cells_from(rules_.map_left.raw(), position.x.raw(), rules_.cell_size.raw());
    const auto row = cells_from(position.y.raw(), rules_.map_top.raw(), rules_.cell_size.raw());
    if (!column || !row || *column >= rules_.cells_wide || *row >= rules_.cells_tall) {
        return false;
    }
    return (*values_[player_index][static_cast<std::size_t>(*row)])[static_cast<std::size_t>(*column)] != 0U;
}

std::span<const std::uint8_t> FogCells::values(const std::size_t player_index) const {
    auto& cached = flat_values_[player_index];
    if (!cached) {
        auto flat = std::make_shared<std::vector<std::uint8_t>>();
        flat->reserve(static_cast<std::size_t>(rules_.cells_wide) * rules_.cells_tall);
        for (const auto& row : values_[player_index]) flat->insert(flat->end(), row->begin(), row->end());
        cached = std::move(flat);
    }
    return *cached;
}

std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> FogCells::value_rows(const std::size_t player_index) const {
    return {values_[player_index].begin(), values_[player_index].end()};
}

void FogCells::append_state(std::vector<std::uint8_t>& bytes) const {
    sim::detail::append_u64(bytes, anchors_->size());
    for (const auto& [id, anchor] : *anchors_) {
        sim::detail::append_u64(bytes, id);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(anchor.column));
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(anchor.row));
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(anchor.radius));
        sim::detail::append_u32(bytes, anchor.owner);
        sim::detail::append_i64(bytes, anchor.x.raw());
        sim::detail::append_i64(bytes, anchor.y.raw());
    }
    sim::detail::append_u64(bytes, values_.size());
    for (const auto& grid : values_) {
        for (const auto& row : grid) bytes.insert(bytes.end(), row->begin(), row->end());
    }
}

} // namespace eawr::sim::tactical
