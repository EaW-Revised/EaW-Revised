#include "eawr/sim/tactical/space.hpp"

#include "../math/wide.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace eawr::sim::tactical {
namespace {

// 2^34 raw = 1024 source units per planar cell. The cell size only affects how many
// candidates the exact test sees, never which bodies match or their order.
constexpr int cell_shift = 34;

// |left - right| as an exact unsigned magnitude; the true difference is below 2^64.
[[nodiscard]] constexpr std::uint64_t distance_along(const std::int64_t left, const std::int64_t right) noexcept {
    return left >= right
        ? static_cast<std::uint64_t>(left) - static_cast<std::uint64_t>(right)
        : static_cast<std::uint64_t>(right) - static_cast<std::uint64_t>(left);
}

[[nodiscard]] constexpr std::int64_t saturating_add(const std::int64_t value, const std::int64_t offset) noexcept {
    return value > std::numeric_limits<std::int64_t>::max() - offset
        ? std::numeric_limits<std::int64_t>::max() : value + offset;
}

[[nodiscard]] constexpr std::int64_t saturating_subtract(const std::int64_t value, const std::int64_t offset) noexcept {
    return value < std::numeric_limits<std::int64_t>::min() + offset
        ? std::numeric_limits<std::int64_t>::min() : value - offset;
}

[[nodiscard]] constexpr std::int64_t cell_of(const math::Fixed value) noexcept {
    return value.raw() >> cell_shift;
}

} // namespace

bool within_range(
    const math::Vec3& from, const math::Vec3& to, const math::Fixed radius, const RangeMetric metric) noexcept {
    if (radius.raw() < 0) {
        return false;
    }
    const auto dx = distance_along(from.x.raw(), to.x.raw());
    const auto dy = distance_along(from.y.raw(), to.y.raw());
    // At most three squares below 2^128 each: the 192-bit sum cannot overflow.
    auto squared = math::detail::multiply_u64(dx, dx);
    static_cast<void>(math::detail::add_magnitude(squared, math::detail::multiply_u64(dy, dy)));
    if (metric == RangeMetric::spatial) {
        const auto dz = distance_along(from.z.raw(), to.z.raw());
        static_cast<void>(math::detail::add_magnitude(squared, math::detail::multiply_u64(dz, dz)));
    }
    const auto limit = static_cast<std::uint64_t>(radius.raw());
    return math::detail::compare(squared, math::detail::multiply_u64(limit, limit)) <= 0;
}

bool within_box(const math::Vec3& centre, const math::Vec3& half_extent, const math::Vec3& to) noexcept {
    if (half_extent.x.raw() < 0 || half_extent.y.raw() < 0 || half_extent.z.raw() < 0) {
        return false;
    }
    return distance_along(to.x.raw(), centre.x.raw()) <= static_cast<std::uint64_t>(half_extent.x.raw())
        && distance_along(to.y.raw(), centre.y.raw()) <= static_cast<std::uint64_t>(half_extent.y.raw())
        && distance_along(to.z.raw(), centre.z.raw()) <= static_cast<std::uint64_t>(half_extent.z.raw());
}

core::Result<SpaceIndex> SpaceIndex::build(const std::span<const SpaceBody> bodies) {
    if (bodies.size() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<SpaceIndex>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            "space index: " + std::to_string(bodies.size()) + " bodies exceed the index limit"));
    }
    SpaceIndex index;
    index.bodies_.assign(bodies.begin(), bodies.end());
    std::sort(index.bodies_.begin(), index.bodies_.end(),
        [](const SpaceBody& left, const SpaceBody& right) { return left.entity_id < right.entity_id; });
    for (std::size_t position = 0; position < index.bodies_.size(); ++position) {
        const auto id = index.bodies_[position].entity_id;
        if (id == invalid_entity_id || (position != 0 && index.bodies_[position - 1].entity_id == id)) {
            return core::Result<SpaceIndex>::failure(detail::diagnostic(diagnostic_codes::order,
                "space index: entity ID " + std::to_string(id) + " is zero or repeated"));
        }
    }
    index.cells_.reserve(index.bodies_.size());
    for (std::size_t position = 0; position < index.bodies_.size(); ++position) {
        const auto& body = index.bodies_[position];
        index.cells_.push_back(CellEntry{
            cell_of(body.position.y), cell_of(body.position.x), static_cast<std::uint32_t>(position)});
    }
    std::sort(index.cells_.begin(), index.cells_.end(), [](const CellEntry& left, const CellEntry& right) {
        return std::tie(left.y, left.x, left.body) < std::tie(right.y, right.x, right.body);
    });
    return core::Result<SpaceIndex>::success(std::move(index));
}

const SpaceBody* SpaceIndex::find(const EntityId entity_id) const noexcept {
    const auto found = std::lower_bound(bodies_.begin(), bodies_.end(), entity_id,
        [](const SpaceBody& body, const EntityId id) { return body.entity_id < id; });
    return found != bodies_.end() && found->entity_id == entity_id ? &*found : nullptr;
}

template <typename Match>
std::vector<EntityId> SpaceIndex::collect(
    const math::Vec3& centre,
    const math::Vec3& half_extent,
    const std::optional<PlayerId> owner,
    const Match& match) const {
    std::vector<EntityId> result;
    if (half_extent.x.raw() < 0 || half_extent.y.raw() < 0 || half_extent.z.raw() < 0) {
        return result;
    }
    const auto accept = [&](const SpaceBody& body) {
        return (!owner || body.owner == *owner) && match(body);
    };
    const auto x_low = cell_of(math::Fixed::from_raw(saturating_subtract(centre.x.raw(), half_extent.x.raw())));
    const auto x_high = cell_of(math::Fixed::from_raw(saturating_add(centre.x.raw(), half_extent.x.raw())));
    const auto y_low = cell_of(math::Fixed::from_raw(saturating_subtract(centre.y.raw(), half_extent.y.raw())));
    const auto y_high = cell_of(math::Fixed::from_raw(saturating_add(centre.y.raw(), half_extent.y.raw())));
    // Each span is at most 2^30 cells, so the product fits in 64 bits.
    const auto columns = static_cast<std::uint64_t>(x_high - x_low) + 1U;
    const auto rows = static_cast<std::uint64_t>(y_high - y_low) + 1U;
    if (columns * rows > cells_.size()) {
        for (const auto& body : bodies_) {
            if (accept(body)) {
                result.push_back(body.entity_id);
            }
        }
        return result;
    }
    std::vector<std::uint32_t> candidates;
    for (auto row = y_low; row <= y_high; ++row) {
        auto entry = std::lower_bound(cells_.begin(), cells_.end(), std::pair{row, x_low},
            [](const CellEntry& cell, const std::pair<std::int64_t, std::int64_t>& key) {
                return std::pair{cell.y, cell.x} < key;
            });
        for (; entry != cells_.end() && entry->y == row && entry->x <= x_high; ++entry) {
            candidates.push_back(entry->body);
        }
    }
    // Bodies are stored in ascending ID order, so ascending positions are ascending IDs.
    std::sort(candidates.begin(), candidates.end());
    for (const auto position : candidates) {
        if (accept(bodies_[position])) {
            result.push_back(bodies_[position].entity_id);
        }
    }
    return result;
}

std::vector<EntityId> SpaceIndex::box(
    const math::Vec3& centre, const math::Vec3& half_extent, const std::optional<PlayerId> owner) const {
    return collect(centre, half_extent, owner,
        [&](const SpaceBody& body) { return within_box(centre, half_extent, body.position); });
}

void SpaceIndex::box_positions(
    const math::Vec3& centre, const math::Vec3& half_extent, std::vector<std::uint32_t>& out) const {
    out.clear();
    if (half_extent.x.raw() < 0 || half_extent.y.raw() < 0 || half_extent.z.raw() < 0) {
        return;
    }
    const auto x_low = cell_of(math::Fixed::from_raw(saturating_subtract(centre.x.raw(), half_extent.x.raw())));
    const auto x_high = cell_of(math::Fixed::from_raw(saturating_add(centre.x.raw(), half_extent.x.raw())));
    const auto y_low = cell_of(math::Fixed::from_raw(saturating_subtract(centre.y.raw(), half_extent.y.raw())));
    const auto y_high = cell_of(math::Fixed::from_raw(saturating_add(centre.y.raw(), half_extent.y.raw())));
    const auto columns = static_cast<std::uint64_t>(x_high - x_low) + 1U;
    const auto rows = static_cast<std::uint64_t>(y_high - y_low) + 1U;
    if (columns * rows > cells_.size()) {
        for (std::size_t position = 0; position < bodies_.size(); ++position) {
            if (within_box(centre, half_extent, bodies_[position].position)) {
                out.push_back(static_cast<std::uint32_t>(position));
            }
        }
        return;
    }
    for (auto row = y_low; row <= y_high; ++row) {
        auto entry = std::lower_bound(cells_.begin(), cells_.end(), std::pair{row, x_low},
            [](const CellEntry& cell, const std::pair<std::int64_t, std::int64_t>& key) {
                return std::pair{cell.y, cell.x} < key;
            });
        for (; entry != cells_.end() && entry->y == row && entry->x <= x_high; ++entry) {
            if (within_box(centre, half_extent, bodies_[entry->body].position)) {
                out.push_back(entry->body);
            }
        }
    }
}

std::vector<EntityId> SpaceIndex::range(
    const math::Vec3& centre,
    const math::Fixed radius,
    const RangeMetric metric,
    const std::optional<PlayerId> owner) const {
    const math::Vec3 bounds{radius, radius, radius};
    return collect(centre, bounds, owner,
        [&](const SpaceBody& body) { return within_range(centre, body.position, radius, metric); });
}

} // namespace eawr::sim::tactical
