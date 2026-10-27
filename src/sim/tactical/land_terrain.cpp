#include "eawr/sim/tactical/land_terrain.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace eawr::sim::tactical {
namespace {
using math::Fixed;
// LR2-U1 (unqualified): whether a segment that only touches the surface (distance exactly zero, no
// crossing) is a contact. Until a surveyed capture settles it, touching is not a contact; a start
// exactly on the surface moving up or along it is therefore clear.
constexpr bool touching_is_contact = false;
// LR2-U1/U2 (unqualified): exact triangle-edge, cell-corner and plane/terrain ties. A cell edge and
// the diagonal belong to both neighbouring triangles with equal height, so the result does not
// depend on which side wins; the diagonal picks (00,10,11) when the segment runs along it. A
// terrain contact at the same fraction as the water plane wins over the plane (LR2-U2 placeholder).
constexpr bool terrain_wins_plane_tie = true;
[[nodiscard]] bool below(const Fixed distance) noexcept {
    if constexpr (touching_is_contact) return distance.raw() <= 0;
    else return distance.raw() < 0;
}
// Fixed arithmetic that remembers an overflow so a long expression is checked once.
struct Num {
    Fixed value;
    bool ok{true};
    Num() = default;
    Num(const Fixed v) : value(v) {}
};
Num operator+(const Num a, const Num b) {
    Num r; r.ok = a.ok && b.ok && math::try_add(a.value, b.value, r.value); return r;
}
Num operator-(const Num a, const Num b) {
    Num r; r.ok = a.ok && b.ok && math::try_subtract(a.value, b.value, r.value); return r;
}
Num operator*(const Num a, const Num b) {
    Num r; r.ok = a.ok && b.ok && math::try_multiply(a.value, b.value, r.value); return r;
}
Num operator/(const Num a, const Num b) {
    Num r; r.ok = a.ok && b.ok && math::try_divide(a.value, b.value, r.value); return r;
}
// Surface id of a water plane render mode (WLEN-33), shared with LandTerrain::surface_at.
std::optional<std::int32_t> plane_surface(const std::uint32_t mode) {
    if (mode == 1) return 4;
    if (mode == 2) return 8;
    if (mode == 3) return 7;
    return std::nullopt;
}
// One cell of the contact mesh. Triangle A is (00,10,11) where u >= v, triangle B is (00,11,01).
struct ContactCell {
    std::uint32_t column{}, row{};
    Fixed h00, h10, h01, h11;
};
core::Result<LandTerrain> invalid(const char* message) {
    core::Diagnostic diagnostic;
    diagnostic.code = diagnostic_codes::invalid_land_terrain;
    diagnostic.message = message;
    return core::Result<LandTerrain>::failure(std::move(diagnostic));
}
std::optional<Fixed> interpolate(const Fixed a, const Fixed b, const Fixed fraction) {
    Fixed difference, product, result;
    if (!math::try_subtract(b, a, difference) || !math::try_multiply(difference, fraction, product)
        || !math::try_add(a, product, result)) return std::nullopt;
    return result;
}
}

core::Result<LandTerrain> LandTerrain::build(LandTerrainInput input) {
    const auto count = static_cast<std::uint64_t>(input.width) * input.height;
    if (input.width < 2 || input.height < 2 || count > std::numeric_limits<std::size_t>::max()
        || input.heights.size() != count || input.surfaces.size() != count || input.passability.size() != count
        || input.spacing.raw() <= 0) return invalid("land terrain dimensions or planes are invalid");
    const auto spacing = input.spacing.raw();
    if (spacing > std::numeric_limits<std::int64_t>::max() / input.width
        || spacing > std::numeric_limits<std::int64_t>::max() / input.height)
        return invalid("land terrain extent exceeds the fixed-point range");
    std::sort(input.movement_masks.begin(), input.movement_masks.end(),
        [](const auto& a, const auto& b) { return a.name < b.name; });
    for (std::size_t i = 0; i < input.movement_masks.size(); ++i) {
        if (input.movement_masks[i].name.empty()
            || (i && input.movement_masks[i - 1].name == input.movement_masks[i].name))
            return invalid("land movement masks need unique nonempty names");
    }
    auto bounds = input.map_bounds;
    // WLEN-39: use the whole rectangle when either inset dimension would be empty.
    constexpr auto inset = 400 * Fixed::scale;
    if (bounds) {
        Fixed extent_x, extent_y;
        if (!math::try_subtract(bounds->max_x, bounds->min_x, extent_x)
            || !math::try_subtract(bounds->max_y, bounds->min_y, extent_y)
            || extent_x.raw() <= 0 || extent_y.raw() <= 0) return invalid("land map bounds are invalid");
        if (extent_x.raw() > 2 * inset && extent_y.raw() > 2 * inset) {
            bounds->min_x = Fixed::from_raw(bounds->min_x.raw() + inset);
            bounds->min_y = Fixed::from_raw(bounds->min_y.raw() + inset);
            bounds->max_x = Fixed::from_raw(bounds->max_x.raw() - inset);
            bounds->max_y = Fixed::from_raw(bounds->max_y.raw() - inset);
        }
    }
    return core::Result<LandTerrain>::success(LandTerrain(std::move(input), bounds));
}

std::optional<std::uint32_t> LandTerrain::movement_mask(const std::string_view name) const noexcept {
    const auto found = std::lower_bound(input_.movement_masks.begin(), input_.movement_masks.end(), name,
        [](const LandMovementMask& mask, const std::string_view key) { return mask.name < key; });
    if (found == input_.movement_masks.end() || found->name != name) return std::nullopt;
    return found->bits;
}

std::optional<Fixed> LandTerrain::height_at(const Fixed x, const Fixed y) const {
    const auto spacing = input_.spacing.raw();
    if (x.raw() < 0 || y.raw() < 0 || x.raw() > spacing * (input_.width - 1)
        || y.raw() > spacing * (input_.height - 1)) return std::nullopt;
    const auto column = std::min(static_cast<std::uint32_t>(x.raw() / spacing), input_.width - 2);
    const auto row = std::min(static_cast<std::uint32_t>(y.raw() / spacing), input_.height - 2);
    const auto fx = math::divide(Fixed::from_raw(x.raw() - column * spacing), input_.spacing);
    const auto fy = math::divide(Fixed::from_raw(y.raw() - row * spacing), input_.spacing);
    if (!fx || !fy) return std::nullopt;
    const auto index = static_cast<std::size_t>(row) * input_.width + column;
    const auto low = interpolate(input_.heights[index], input_.heights[index + 1], fx.value());
    const auto high = interpolate(input_.heights[index + input_.width], input_.heights[index + input_.width + 1], fx.value());
    if (!low || !high) return std::nullopt;
    return interpolate(*low, *high, fy.value());
}

std::optional<std::int32_t> LandTerrain::surface_at(const Fixed x, const Fixed y) const {
    const auto height = height_at(x, y);
    if (!height) return std::nullopt;
    if (input_.water && input_.water->mode != 0) {
        Fixed threshold;
        if (math::try_subtract(input_.water->height, Fixed::from_raw(Fixed::scale / 2), threshold)
            && *height < threshold) {
            // WLEN-33: render mode selects water, lava or ice.
            if (const auto plane = plane_surface(input_.water->mode)) return *plane;
        }
    }
    return nearest_vertex_surface(x, y);
}

std::int32_t LandTerrain::nearest_vertex_surface(const Fixed x, const Fixed y) const {
    const auto nearest = [&](const std::int64_t raw) {
        const auto whole = raw / input_.spacing.raw();
        const auto remainder = raw % input_.spacing.raw();
        return static_cast<std::uint32_t>(whole + (remainder >= input_.spacing.raw() - remainder ? 1 : 0));
    };
    return input_.surfaces[static_cast<std::size_t>(nearest(y.raw())) * input_.width + nearest(x.raw())];
}

bool LandTerrain::admits(const std::uint32_t x, const std::uint32_t y, const std::uint32_t mask) const noexcept {
    if (x >= input_.width || y >= input_.height) return false;
    const auto value = input_.passability[static_cast<std::size_t>(y) * input_.width + x];
    return (value & mask) == value; // WLPA-01, including mixed restrictions.
}

std::optional<bool> LandTerrain::playable_centre(const Fixed x, const Fixed y) const noexcept {
    if (!bounds_) return std::nullopt;
    // WLPA-05; callers supply the centre until R2/R3 pins grid alignment.
    return x > bounds_->min_x && x < bounds_->max_x && y > bounds_->min_y && y < bounds_->max_y;
}

bool LandTerrain::admits_footprint(const std::uint32_t min_x, const std::uint32_t min_y,
    const std::uint32_t max_x, const std::uint32_t max_y, const std::uint32_t mask) const noexcept {
    if (min_x > max_x || min_y > max_y || max_x >= input_.width || max_y >= input_.height) return false;
    for (auto y = min_y; y <= max_y; ++y)
        for (auto x = min_x; x <= max_x; ++x) if (!admits(x, y, mask)) return false;
    return true;
}

std::array<std::uint8_t, 32> LandTerrain::identity() const {
    std::vector<std::uint8_t> bytes;
    const auto word = [&](const std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    constexpr std::string_view domain = "land_terrain";
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    word(1);
    word(input_.width); word(input_.height); word(static_cast<std::uint64_t>(input_.spacing.raw()));
    for (std::size_t i = 0; i < input_.heights.size(); ++i) {
        word(static_cast<std::uint64_t>(input_.heights[i].raw()));
        word(static_cast<std::uint32_t>(input_.surfaces[i])); word(input_.passability[i]);
    }
    word(input_.water.has_value());
    if (input_.water) { word(static_cast<std::uint64_t>(input_.water->height.raw())); word(input_.water->mode); }
    word(input_.map_bounds.has_value());
    if (input_.map_bounds) {
        word(static_cast<std::uint64_t>(input_.map_bounds->min_x.raw()));
        word(static_cast<std::uint64_t>(input_.map_bounds->min_y.raw()));
        word(static_cast<std::uint64_t>(input_.map_bounds->max_x.raw()));
        word(static_cast<std::uint64_t>(input_.map_bounds->max_y.raw()));
    }
    word(input_.movement_masks.size());
    for (const auto& mask : input_.movement_masks) {
        word(mask.name.size()); bytes.insert(bytes.end(), mask.name.begin(), mask.name.end()); word(mask.bits);
    }
    return core::sha256(bytes);
}

LandOccupancy::LandOccupancy(std::shared_ptr<const LandTerrain> terrain, const std::uint32_t clear_bit)
    : terrain_(std::move(terrain)), clear_bit_(clear_bit),
      current_(terrain_->passability().begin(), terrain_->passability().end()), staging_(current_.size()), offsets_(current_.size()), cursors_(current_.size()) {
    for (auto& value : current_) if ((value & ~clear_bit_) != 0) value &= ~clear_bit_;
    std::copy(current_.begin(), current_.end(), staging_.begin());
}

core::Result<LandOccupancy> LandOccupancy::create(std::shared_ptr<const LandTerrain> terrain,
    const std::uint32_t clear_bit) {
    if (!terrain || clear_bit == 0 || (clear_bit & (clear_bit - 1)) != 0
        || terrain->passability().size() > std::numeric_limits<std::uint32_t>::max()) {
        core::Diagnostic diagnostic;
        diagnostic.code = diagnostic_codes::invalid_land_terrain;
        diagnostic.message = "land occupancy needs terrain, a single Clear bit and addressable cells";
        return core::Result<LandOccupancy>::failure(std::move(diagnostic));
    }
    return core::Result<LandOccupancy>::success(LandOccupancy(std::move(terrain), clear_bit));
}

core::Result<void> LandOccupancy::commit(const std::vector<LandOccupation>& occupations,
    const PartitionExecutor& executor) {
    work_ = {};
    const auto previous = this->occupations();
    if (std::equal(occupations.begin(), occupations.end(), previous.begin(), previous.end())) {
        changed_.clear();
        return core::Result<void>::success();
    }
    const auto failure = [] {
        core::Diagnostic diagnostic;
        diagnostic.code = diagnostic_codes::invalid_land_terrain;
        diagnostic.message = "land occupation has an invalid object ID or cell index";
        return core::Result<void>::failure(std::move(diagnostic));
    };
    // Retain the high-water capacities across frames, including the inactive
    // objects' cell buffers. No scratch is allocated/freed for a warmed workload.
    order_.resize(occupations.size());
    valid_.resize(occupations.size());
    if (pending_occupations_.size() < occupations.size()) pending_occupations_.resize(occupations.size());
    for (std::size_t i = 0; i < order_.size(); ++i) order_[i] = i;
    std::sort(order_.begin(), order_.end(), [&](const auto a, const auto b) {
        return occupations[a].object < occupations[b].object;
    });
    for (std::size_t i = 0; i < order_.size(); ++i) {
        const auto id = occupations[order_[i]].object;
        if (id == 0 || (i && occupations[order_[i - 1]].object == id)) return failure();
    }
    const auto prepared = executor.execute(tick_partition_count, [this, &occupations](const std::size_t partition) {
        const auto range = partition_range(partition, occupations.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& source = occupations[order_[i]];
            auto& target = pending_occupations_[i];
            target.object = source.object;
            target.primary_passability = source.primary_passability;
            target.secondary_passability = source.secondary_passability;
            const auto copy_cells = [&](auto& destination, const auto& input) {
                destination.assign(input.begin(), input.end());
                std::sort(destination.begin(), destination.end());
                destination.erase(std::unique(destination.begin(), destination.end()), destination.end());
                return destination.empty() || destination.back() < current_.size();
            };
            valid_[i] = static_cast<std::uint8_t>(copy_cells(target.primary_cells, source.primary_cells)
                && copy_cells(target.secondary_cells, source.secondary_cells));
        }
    });
    if (!prepared) return prepared;
    if (std::find(valid_.begin(), valid_.end(), 0) != valid_.end()) return failure();
    const auto pending = std::span<const LandOccupation>(pending_occupations_).first(occupations.size());
    work_.objects_prepared = occupations.size();
    if (std::equal(pending.begin(), pending.end(), previous.begin(), previous.end())) {
        changed_.clear();
        return core::Result<void>::success();
    }
    // Only cells mentioned by the old/new snapshots can change. The untouched
    // cells of both buffers are identical, so no full-grid prefix or scan is needed.
    touched_.clear();
    const auto touch = [&](const auto& snapshot) {
        for (const auto& object : snapshot) {
            touched_.insert(touched_.end(), object.primary_cells.begin(), object.primary_cells.end());
            touched_.insert(touched_.end(), object.secondary_cells.begin(), object.secondary_cells.end());
        }
    };
    touch(previous); touch(pending);
    std::sort(touched_.begin(), touched_.end());
    touched_.erase(std::unique(touched_.begin(), touched_.end()), touched_.end());
    for (const auto cell : touched_) cursors_[cell] = 0;
    for (const auto& object : pending) {
        for (const auto cell : object.primary_cells) ++cursors_[cell];
        for (const auto cell : object.secondary_cells) ++cursors_[cell];
    }
    std::size_t references = 0;
    for (const auto cell : touched_) {
        const auto count = cursors_[cell];
        offsets_[cell] = cursors_[cell] = references;
        references += count;
    }
    bucket_bits_.resize(references);
    // Ordered commit into flat cell buckets, ascending object ID (ADR-009).
    for (const auto& object : pending) {
        for (const auto cell : object.primary_cells) bucket_bits_[cursors_[cell]++] = object.primary_passability;
        for (const auto cell : object.secondary_cells) bucket_bits_[cursors_[cell]++] = object.secondary_passability;
    }
    changed_flags_.resize(touched_.size());
    changed_.reserve(touched_.size());
    const auto executed = executor.execute(tick_partition_count, [this](const std::size_t partition) {
        const auto base = terrain_->passability();
        const auto range = partition_range(partition, touched_.size());
        for (auto index = range.begin; index < range.end; ++index) {
            const auto cell = touched_[index];
            auto value = base[cell];
            for (auto i = offsets_[cell]; i < cursors_[cell]; ++i) value |= bucket_bits_[i];
            // WLPA-03/04: preserve all sources, remove Clear with any restriction.
            staging_[cell] = (value & ~clear_bit_) != 0 ? value & ~clear_bit_ : value;
            changed_flags_[index] = static_cast<std::uint8_t>(current_[cell] != staging_[cell]);
        }
    });
    if (!executed) {
        for (const auto cell : touched_) staging_[cell] = current_[cell];
        return executed;
    }
    changed_.clear();
    current_.swap(staging_);
    for (std::size_t i = 0; i < touched_.size(); ++i) {
        const auto cell = touched_[i];
        if (changed_flags_[i]) changed_.push_back(cell);
        staging_[cell] = current_[cell]; // restore the back buffer's untouched-cell invariant
    }
    if (!changed_.empty()) ++revision_;
    occupations_.swap(pending_occupations_);
    occupation_count_ = occupations.size();
    work_.cells_reduced = touched_.size();
    work_.references_bucketed = references;
    return core::Result<void>::success();
}

LandOccupancy::Checkpoint LandOccupancy::checkpoint() const {
    const auto rows = occupations();
    return {std::vector<LandOccupation>(rows.begin(), rows.end()), changed_, revision_};
}

core::Result<LandOccupancy> LandOccupancy::from_checkpoint(std::shared_ptr<const LandTerrain> terrain,
    const std::uint32_t clear_bit, const Checkpoint& saved) {
    auto result = create(std::move(terrain), clear_bit);
    if (!result) return result;
    auto& grid = result.value();
    if (!std::is_sorted(saved.changed.begin(), saved.changed.end())
        || std::adjacent_find(saved.changed.begin(), saved.changed.end()) != saved.changed.end()
        || (!saved.changed.empty() && saved.changed.back() >= grid.current_.size())) {
        core::Diagnostic error;
        error.code = diagnostic_codes::invalid_setup;
        error.message = "invalid land occupancy checkpoint changes";
        return core::Result<LandOccupancy>::failure(std::move(error));
    }
    auto committed = grid.commit(saved.occupations, InlineExecutor{});
    if (!committed) return core::Result<LandOccupancy>::failure(committed.error());
    grid.revision_ = saved.revision;
    grid.changed_ = saved.changed;
    return result;
}

bool LandOccupancy::admits(const std::uint32_t x, const std::uint32_t y, const std::uint32_t mask) const noexcept {
    if (x >= terrain_->width() || y >= terrain_->height()) return false;
    const auto value = current_[static_cast<std::size_t>(y) * terrain_->width() + x];
    return (value & mask) == value;
}
bool LandOccupancy::admits_footprint(const std::uint32_t min_x, const std::uint32_t min_y,
    const std::uint32_t max_x, const std::uint32_t max_y, const std::uint32_t mask) const noexcept {
    if (min_x > max_x || min_y > max_y || max_x >= terrain_->width() || max_y >= terrain_->height()) return false;
    for (auto y = min_y; y <= max_y; ++y)
        for (auto x = min_x; x <= max_x; ++x) if (!admits(x, y, mask)) return false;
    return true;
}

LandSegmentResult LandTerrain::segment_intersection(
    const math::Vec3& start, const math::Vec3& end, const LandSegmentOptions options) const {
    LandSegmentResult failure;
    failure.status = LandSegmentStatus::overflow;
    const Num ax = start.x, ay = start.y, az = start.z;
    const Num dx = Num(end.x) - ax, dy = Num(end.y) - ay, dz = Num(end.z) - az;
    if (!dx.ok || !dy.ok || !dz.ok) return failure;
    const Fixed one = Fixed::from_raw(Fixed::scale);
    const Fixed spacing = input_.spacing;
    const auto max_x = Fixed::from_raw(spacing.raw() * (input_.width - 1));
    const auto max_y = Fixed::from_raw(spacing.raw() * (input_.height - 1));

    // Clip the segment to the sampled grid; what lies outside has no terrain to hit.
    Fixed lo, hi = one;
    for (const auto& [a, d, extent] : {std::tuple{ax, dx, max_x}, std::tuple{ay, dy, max_y}}) {
        if (d.value.raw() == 0) {
            if (a.value.raw() < 0 || a.value > extent) return {};
            continue;
        }
        const Num t0 = (Num(Fixed{}) - a) / d, t1 = (Num(extent) - a) / d;
        if (!t0.ok || !t1.ok) return failure;
        lo = std::max(lo, std::min(t0.value, t1.value));
        hi = std::min(hi, std::max(t0.value, t1.value));
    }
    if (lo > hi) return {};

    // Breakpoints: where the segment crosses a grid line. Between two the segment stays in one cell.
    std::vector<Fixed> ts{lo, hi};
    const auto grid_lines = [&](const Num a, const Num d, const std::uint32_t count) {
        if (d.value.raw() == 0 || count < 3) return true;
        const Num pa = a + Num(lo) * d, pb = a + Num(hi) * d;
        if (!pa.ok || !pb.ok) return false;
        const auto first = std::min(pa.value.raw(), pb.value.raw()) / spacing.raw() + 1;
        const auto last = std::max(pa.value.raw(), pb.value.raw()) / spacing.raw();
        for (auto k = std::max<std::int64_t>(first, 1); k <= last && k <= static_cast<std::int64_t>(count) - 2; ++k) {
            const Num t = (Num(Fixed::from_raw(k * spacing.raw())) - a) / d;
            if (!t.ok) return false;
            if (t.value > lo && t.value < hi) ts.push_back(t.value);
        }
        return true;
    };
    if (!grid_lines(ax, dx, input_.width) || !grid_lines(ay, dy, input_.height)) return failure;
    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
    if (ts.size() == 1) ts.push_back(ts.front()); // a point query: the segment is one position

    const auto cell_at = [&](const std::uint32_t column, const std::uint32_t row) {
        const auto index = static_cast<std::size_t>(row) * input_.width + column;
        return ContactCell{column, row, input_.heights[index], input_.heights[index + 1],
            input_.heights[index + input_.width], input_.heights[index + input_.width + 1]};
    };
    const auto point_at = [&](const Fixed t, Num& x, Num& y, Num& z) {
        x = ax + Num(t) * dx; y = ay + Num(t) * dy; z = az + Num(t) * dz;
        return x.ok && y.ok && z.ok;
    };
    const auto middle = [](const Fixed a, const Fixed b) { return Fixed::from_raw(a.raw() + (b.raw() - a.raw()) / 2); };
    // Offsets of a position inside the cell, in world units.
    const auto local_u = [&](const ContactCell& cell, const Num x) {
        return x - Num(Fixed::from_raw(cell.column * spacing.raw()));
    };
    const auto local_v = [&](const ContactCell& cell, const Num y) {
        return y - Num(Fixed::from_raw(cell.row * spacing.raw()));
    };
    // Height of the chosen triangle's plane over (x, y); the plane gradient is per world unit.
    const auto plane_height = [&](const ContactCell& cell, const bool triangle_a, const Num x, const Num y) {
        const Num along_u = Num(triangle_a ? cell.h10 : cell.h11) - Num(triangle_a ? cell.h00 : cell.h01);
        const Num along_v = Num(triangle_a ? cell.h11 : cell.h01) - Num(triangle_a ? cell.h10 : cell.h00);
        return Num(cell.h00) + (local_u(cell, x) * along_u + local_v(cell, y) * along_v) / Num(spacing);
    };

    struct Terrain { bool hit{}; Fixed t; ContactCell cell; bool triangle_a{true}; };
    Terrain terrain;
    for (std::size_t i = 0; i + 1 < ts.size() && !terrain.hit; ++i) {
        const Fixed ta = ts[i], tb = ts[i + 1];
        Num mx, my, mz;
        if (!point_at(middle(ta, tb), mx, my, mz)) return failure;
        const auto column = std::min(static_cast<std::uint32_t>(mx.value.raw() / spacing.raw()), input_.width - 2);
        const auto row = std::min(static_cast<std::uint32_t>(my.value.raw() / spacing.raw()), input_.height - 2);
        const auto cell = cell_at(column, row);
        Num xa, ya, za, xb, yb, zb;
        if (!point_at(ta, xa, ya, za) || !point_at(tb, xb, yb, zb)) return failure;
        const Num da = local_u(cell, xa) - local_v(cell, ya), db = local_u(cell, xb) - local_v(cell, yb);
        if (!da.ok || !db.ok) return failure;
        // The diagonal u == v splits the cell: height is continuous across it but the plane changes.
        std::array<Fixed, 3> bounds{ta, tb, tb};
        std::size_t pieces = 1;
        if ((da.value.raw() < 0 && db.value.raw() > 0) || (da.value.raw() > 0 && db.value.raw() < 0)) {
            const Num lead = Num(Fixed::from_raw(std::abs(da.value.raw())));
            const Num span = lead + Num(Fixed::from_raw(std::abs(db.value.raw())));
            const Num split = Num(ta) + (Num(tb) - Num(ta)) * (lead / span);
            if (!span.ok || !split.ok) return failure;
            bounds = {ta, std::clamp(split.value, ta, tb), tb};
            pieces = 2;
        }
        for (std::size_t piece = 0; piece < pieces && !terrain.hit; ++piece) {
            const Fixed t0 = bounds[piece], t1 = bounds[piece + 1];
            Num x0, y0, z0, x1, y1, z1, xm, ym, zm;
            if (!point_at(t0, x0, y0, z0) || !point_at(t1, x1, y1, z1)
                || !point_at(middle(t0, t1), xm, ym, zm)) return failure;
            const Num side = local_u(cell, xm) - local_v(cell, ym);
            const bool triangle_a = side.value.raw() >= 0;
            const Num f0 = z0 - plane_height(cell, triangle_a, x0, y0);
            const Num f1 = z1 - plane_height(cell, triangle_a, x1, y1);
            if (!side.ok || !f0.ok || !f1.ok) return failure;
            if (below(f0.value)) { // start below the surface (or the grid edge): contact at that start
                terrain = {true, t0, cell, triangle_a};
            } else if (below(f1.value)) {
                const Num span = f0 - f1;
                const Num t = Num(t0) + (Num(t1) - Num(t0)) * (f0 / span);
                if (!span.ok || !t.ok) return failure;
                terrain = {true, std::clamp(t.value, t0, t1), cell, triangle_a};
            }
        }
    }

    // LR2-16: the horizontal plane, only on a downward crossing from at or above it.
    bool plane_hit = false;
    Fixed plane_t;
    std::int32_t plane_type{};
    if (options.water_admitted && input_.water && plane_surface(input_.water->mode)) {
        Num x, y, z0, z1;
        if (!point_at(lo, x, y, z0) || !point_at(hi, x, y, z1)) return failure;
        const Num f0 = z0 - Num(input_.water->height), f1 = z1 - Num(input_.water->height);
        if (!f0.ok || !f1.ok) return failure;
        if (f0.value.raw() >= 0 && below(f1.value)) {
            const Num span = f0 - f1;
            const Num t = Num(lo) + (Num(hi) - Num(lo)) * (f0 / span);
            if (!span.ok || !t.ok) return failure;
            plane_hit = true; plane_t = std::clamp(t.value, lo, hi);
            plane_type = *plane_surface(input_.water->mode);
        }
    }

    if (!terrain.hit && !plane_hit) return {};
    const bool use_plane = plane_hit && (!terrain.hit || plane_t < terrain.t
        || (plane_t == terrain.t && !terrain_wins_plane_tie));
    const Fixed t = use_plane ? plane_t : terrain.t;
    Num x, y, z;
    if (!point_at(t, x, y, z)) return failure;

    LandSegmentResult result;
    result.status = LandSegmentStatus::hit;
    result.fraction = t;
    result.point = math::Vec3{x.value, y.value, z.value};
    if (use_plane) {
        result.source = LandContactSource::water_plane;
        result.normal = {Fixed{}, Fixed{}, one};
        result.surface = plane_type;
        return result;
    }
    const auto& cell = terrain.cell;
    const bool a = terrain.triangle_a;
    const Num slope_x = (Num(a ? cell.h10 : cell.h11) - Num(a ? cell.h00 : cell.h01)) / Num(spacing);
    const Num slope_y = (Num(a ? cell.h11 : cell.h01) - Num(a ? cell.h10 : cell.h00)) / Num(spacing);
    const Num nx = Num(Fixed{}) - slope_x, ny = Num(Fixed{}) - slope_y;
    if (!slope_x.ok || !slope_y.ok || !nx.ok || !ny.ok) return failure;
    auto normal = math::normalize(math::Vec3{nx.value, ny.value, one});
    if (!normal) return failure;
    result.source = LandContactSource::terrain;
    result.normal = normal.value();
    result.surface = nearest_vertex_surface(x.value, y.value);
    return result;
}
} // namespace eawr::sim::tactical
