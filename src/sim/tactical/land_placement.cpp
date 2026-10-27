#include "eawr/sim/tactical/land_placement.hpp"
#include "eawr/sim/math/trig.hpp"

#include <algorithm>

namespace eawr::sim::tactical {
namespace {
using math::Fixed;

constexpr std::int32_t step_x[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr std::int32_t step_y[8] = {0, 1, 1, 1, 0, -1, -1, -1};
} // namespace

std::optional<std::uint32_t> land_common_mask(const std::span<const std::uint32_t> member_masks) noexcept {
    if (member_masks.empty()) return std::nullopt;
    auto common = member_masks.front();
    for (const auto mask : member_masks) common &= mask;
    return common;
}

bool land_is_authored_class(const LandTerrain& terrain, const std::uint32_t mask) noexcept {
    for (const auto& authored : terrain.movement_masks()) if (authored.bits == mask) return true;
    return false;
}

LandZones LandZones::build(const LandOccupancy& occupancy, const std::uint32_t mask) {
    LandZones zones;
    zones.rebuild(occupancy, mask);
    return zones;
}

void LandZones::rebuild(const LandOccupancy& occupancy, const std::uint32_t mask) {
    const auto& terrain = occupancy.terrain();
    columns_ = terrain.width();
    const auto rows = terrain.height();
    const auto grid = occupancy.cells();
    mask_ = mask;
    revision_ = occupancy.revision();
    count_ = 0;
    zones_.assign(grid.size(), 0);
    const auto open = [&](const std::int64_t x, const std::int64_t y) {
        return x >= 0 && y >= 0 && x < columns_ && y < rows
            && (grid[static_cast<std::size_t>(y) * columns_ + static_cast<std::size_t>(x)] & ~mask) == 0;
    };
    for (std::size_t first = 0; first < grid.size(); ++first) {
        if (zones_[first] != 0 || (grid[first] & ~mask) != 0) continue;
        const auto label = ++count_;
        queue_.clear();
        queue_.push_back(static_cast<std::uint32_t>(first));
        zones_[first] = label;
        for (std::size_t head = 0; head < queue_.size(); ++head) {
            const std::int64_t x = queue_[head] % columns_, y = queue_[head] / columns_;
            for (int direction = 0; direction < 8; ++direction) {
                const auto nx = x + step_x[direction], ny = y + step_y[direction];
                if (!open(nx, ny)) continue;
                // The search's corner rule (LP-05): a diagonal join needs both orthogonal neighbours.
                if ((direction & 1) != 0 && !(open(x + step_x[direction], y) && open(x, y + step_y[direction]))) continue;
                const auto cell = static_cast<std::uint32_t>(ny * columns_ + nx);
                if (zones_[cell] != 0) continue;
                zones_[cell] = label;
                queue_.push_back(cell);
            }
        }
    }
}

void LandZoneCache::prepare(const LandOccupancy& occupancy, const std::span<const std::uint32_t> masks) {
    for (const auto mask : masks) {
        auto found = std::find_if(zones_.begin(), zones_.end(), [mask](const LandZones& zones) { return zones.mask() == mask; });
        if (found == zones_.end()) zones_.push_back(LandZones::build(occupancy, mask));
        else if (found->revision() != occupancy.revision() || found->cell_count() != occupancy.cells().size()) found->rebuild(occupancy, mask);
    }
}

const LandZones* LandZoneCache::find(const std::uint32_t mask) const noexcept {
    const auto found = std::find_if(zones_.begin(), zones_.end(), [mask](const LandZones& zones) { return zones.mask() == mask; });
    return found == zones_.end() ? nullptr : &*found;
}

Fixed land_placement_padding(const Fixed radius, const Fixed formation_spacing) noexcept {
    // 0.3 * (spacing * 20) = spacing * 6, exactly in raw units.
    const auto quarter = std::max<std::int64_t>(radius.raw(), 0) / 4;
    const auto spaced = std::max<std::int64_t>(formation_spacing.raw(), 0) * 6;
    return Fixed::from_raw(std::max(quarter, spaced));
}

LandPlacement land_find_open_position(const LandOccupancy& occupancy, const LandPlacementQuery& query) {
    const auto& terrain = occupancy.terrain();
    const auto spacing = terrain.spacing();
    const auto columns = terrain.width(), rows = terrain.height();
    LandPlacement placement;
    placement.position = query.desired;

    Fixed tested_radius;
    if (!math::try_add(query.radius, land_placement_padding(query.radius, query.formation_spacing), tested_radius)) return placement;
    const auto footprint = land_footprint(tested_radius, spacing);
    std::uint32_t origin_zone = 0;
    if (query.zones != nullptr) {
        const auto column = land_cell_of(query.from.x, spacing, columns), row = land_cell_of(query.from.y, spacing, rows);
        if (column && row) origin_zone = query.zones->zone_of(*row * columns + *column);
    }
    const auto legal = [&](const math::Vec2 at) {
        const auto column = land_cell_of(at.x, spacing, columns), row = land_cell_of(at.y, spacing, rows);
        if (!column || !row) return false;
        // WLPA-05: a centre at or past the playable boundary is unplayable; a map without bounds has none.
        const auto playable = terrain.playable_centre(at.x, at.y);
        if (playable && !*playable) return false;
        if (query.zones != nullptr && origin_zone != 0 && query.zones->zone_of(*row * columns + *column) != origin_zone) return false;
        const std::int64_t low_x = static_cast<std::int64_t>(*column) + footprint.low, high_x = static_cast<std::int64_t>(*column) + footprint.high;
        const std::int64_t low_y = static_cast<std::int64_t>(*row) + footprint.low, high_y = static_cast<std::int64_t>(*row) + footprint.high;
        if (low_x < 0 || low_y < 0 || high_x >= columns || high_y >= rows) return false;
        if (!occupancy.admits_footprint(static_cast<std::uint32_t>(low_x), static_cast<std::uint32_t>(low_y),
                static_cast<std::uint32_t>(high_x), static_cast<std::uint32_t>(high_y), query.mask)) return false;
        return query.filter == nullptr || !query.filter->rejects(at);
    };

    // WLPA-25: the ring increment grows by 0.8 per try and is capped by the smaller of 10 and 1.5 hard radii
    // (5 for a zero radius).
    const std::int64_t unit = Fixed::scale;
    const std::int64_t cap = query.radius.raw() > 0 ? std::min(10 * unit, query.radius.raw() * 3 / 2) : 5 * unit;
    // The bias toward the mover, as the space planner's open-position search has it: the first sample of every
    // ring lies on the mover's side and the rest alternate either side of it.
    Fixed base_angle;
    if (query.from != query.desired) {
        Fixed dx, dy;
        if (math::try_subtract(query.from.x, query.desired.x, dx) && math::try_subtract(query.from.y, query.desired.y, dy)) {
            if (auto angle = math::atan2_turn(dy, dx)) base_angle = angle.value();
        }
    }
    std::int64_t ring = 0;
    for (std::uint32_t attempt = 0; attempt < land_placement_tries; ++attempt) {
        placement.tries = attempt + 1;
        if (attempt == 0) {
            if (legal(query.desired)) {
                placement.status = LandPlacementStatus::exact;
                return placement;
            }
            continue;
        }
        ring += std::min(cap, static_cast<std::int64_t>(attempt) * unit * 4 / 5);
        const std::int64_t samples = 8 * (1 + ring / (2 * spacing.raw()));
        for (std::int64_t sample = 0; sample < samples; ++sample) {
            const std::int64_t offset = sample == 0 ? 0 : (sample & 1) != 0 ? (sample + 1) / 2 : -(sample / 2);
            Fixed angle;
            if (!math::try_add(base_angle, Fixed::from_raw(offset * unit / samples), angle)) continue;
            const auto direction = math::sin_cos_turn(angle);
            Fixed along_x, along_y, x, y;
            if (!math::try_multiply(Fixed::from_raw(ring), direction.cosine, along_x)
                || !math::try_multiply(Fixed::from_raw(ring), direction.sine, along_y)
                || !math::try_add(query.desired.x, along_x, x) || !math::try_add(query.desired.y, along_y, y)) continue;
            if (legal({x, y})) {
                placement.status = LandPlacementStatus::adjusted;
                placement.position = {x, y};
                return placement;
            }
        }
    }
    return placement;
}

Fixed land_formation_width(const std::uint32_t count, const Fixed largest_extent, const Fixed area_sum) noexcept {
    if (count == 0) return {};
    if (count == 1) return largest_extent;
    std::uint64_t rows = 1;
    while (rows * rows < count) ++rows;
    // ceil(sqrt(N)) rows of the largest member plus 0.25, against sqrt(max(1, 1.8 * area_sum)); code constants.
    Fixed tall;
    if (!math::try_multiply(Fixed::from_raw(static_cast<std::int64_t>(rows) * Fixed::scale), largest_extent, tall)
        || !math::try_add(tall, Fixed::from_raw(Fixed::scale / 4), tall)) return largest_extent;
    auto scaled = Fixed::from_raw(std::max<std::int64_t>(area_sum.raw(), 0) / 5 * 9);
    if (scaled.raw() < Fixed::scale) scaled = Fixed::from_raw(Fixed::scale);
    Fixed area_width;
    if (!math::try_sqrt(scaled, area_width)) return tall;
    return std::max(tall, area_width);
}

} // namespace eawr::sim::tactical
