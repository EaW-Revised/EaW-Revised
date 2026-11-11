#include "eawr/skirmish/land_fog.hpp"

#include "skirmish_internal.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace eawr::skirmish {
namespace {

using sim::math::Fixed;
using Result = core::Result<sim::tactical::FogRules>;

Result refuse(std::string text) { return Result::failure(detail::error(diagnostic_codes::input, std::move(text))); }

// ceil(extent / cell) in whole cells, capped. Exact integer arithmetic on Q24 raw values.
std::int64_t cell_count(const std::int64_t extent, const std::int64_t cell) {
    const auto exact = extent / cell + (extent % cell != 0 ? 1 : 0);
    return std::min<std::int64_t>(exact, land_fog_max_cells);
}

// extent / count, rounded to the nearest Q24 raw value (half up).
std::int64_t shrunk_cell(const std::int64_t extent, const std::int64_t count) {
    return extent / count + ((extent % count) * 2 >= count ? 1 : 0);
}

} // namespace

core::Result<sim::tactical::FogRules> land_fog_rules(const std::uint32_t samples_wide, const std::uint32_t samples_tall,
    const Fixed sample_spacing, const Fixed desired_cell_size, const Fixed regrow_seconds) {
    if (samples_wide == 0 || samples_tall == 0 || sample_spacing.raw() <= 0)
        return refuse("land fog: the terrain has no extent");
    if (desired_cell_size.raw() <= 0) return refuse("land fog: DesiredLandFOWCellSize is missing or not a positive number");
    const auto spacing = sample_spacing.raw();
    if (samples_wide > std::numeric_limits<std::int64_t>::max() / spacing
        || samples_tall > std::numeric_limits<std::int64_t>::max() / spacing)
        return refuse("land fog: the terrain extent exceeds the coordinate range");
    const std::int64_t extent_x = static_cast<std::int64_t>(samples_wide) * spacing;
    const std::int64_t extent_y = static_cast<std::int64_t>(samples_tall) * spacing;
    const auto requested = desired_cell_size.raw();
    const auto count_x = cell_count(extent_x, requested);
    const auto count_y = cell_count(extent_y, requested);
    const auto cell_x = shrunk_cell(extent_x, count_x);
    const auto cell_y = shrunk_cell(extent_y, count_y);
    if (cell_x <= 0 || cell_y <= 0) return refuse("land fog: the terrain is too small for a fog cell");
    if (cell_x != cell_y)
        return refuse("land fog: a " + std::to_string(samples_wide) + " x " + std::to_string(samples_tall)
            + " sample terrain gets fog cells of different width and height (Q24 raw " + std::to_string(cell_x) + " and "
            + std::to_string(cell_y) + "); the shared reveal needs square cells");
    auto step = sim::tactical::fog_ramp_down_step(regrow_seconds);
    if (!step) return Result::failure(step.error());
    sim::tactical::FogRules rules;
    rules.map_left = Fixed{};
    rules.map_top = Fixed::from_raw(extent_y);
    rules.cell_size = Fixed::from_raw(cell_x);
    rules.cells_wide = static_cast<std::uint32_t>(count_x);
    rules.cells_tall = static_cast<std::uint32_t>(count_y);
    rules.ramp_down_step = step.value();
    if (auto valid = sim::tactical::validate_fog_rules(rules); !valid) return Result::failure(valid.error());
    return Result::success(std::move(rules));
}

} // namespace eawr::skirmish
