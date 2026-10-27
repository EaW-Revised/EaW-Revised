#pragma once

#include "eawr/sim/tactical/land_path.hpp"
#include "../support/check.hpp"

#include <memory>
#include <string>
#include <vector>

// Synthetic land maps drawn as text, shared by the land path and placement tests.
namespace eawr::land_test {
using sim::math::Fixed;
using namespace sim::tactical;

inline Fixed whole(const std::int64_t value) { return Fixed::from_integer(value).value(); }
inline sim::math::Vec2 at(const std::int64_t x, const std::int64_t y) { return {whole(x), whole(y)}; }
inline sim::math::Vec2 centre(const std::int64_t column, const std::int64_t row) { return at(column * 20, row * 20); }

// Cell values for the synthetic maps: 1 clear, 2 wall, 4 water, 8 infantry-only. A cell is entered when
// every bit it holds is in the mover's mask (WLPA-01).
constexpr std::uint32_t clear = 1, wall = 2, water = 4, infantry_only = 8;
constexpr std::uint32_t walker = clear, hover = clear | water, infantry = clear | infantry_only;

struct Field {
    std::shared_ptr<const LandTerrain> terrain;
    LandOccupancy occupancy;
    std::uint32_t columns{}, rows{};
};
inline Field make_field(const std::vector<std::string>& picture) {
    for (const auto& row : picture) testing::require(row.size() == picture.front().size(), "fixture rows have equal widths");
    LandTerrainInput input;
    input.height = static_cast<std::uint32_t>(picture.size());
    input.width = static_cast<std::uint32_t>(picture.front().size());
    input.spacing = whole(20);
    input.movement_masks = {{"walker", walker}, {"hover", hover}, {"infantry", infantry}};
    for (const auto& row : picture) {
        for (const auto glyph : row) {
            input.heights.push_back({});
            input.surfaces.push_back(0);
            input.passability.push_back(glyph == '#' ? wall : glyph == '~' ? water : glyph == 'i' ? infantry_only : clear);
        }
    }
    auto terrain = std::make_shared<const LandTerrain>(LandTerrain::build(std::move(input)).value());
    auto occupancy = LandOccupancy::create(terrain, clear).value();
    return {terrain, std::move(occupancy), terrain->width(), terrain->height()};
}
inline LandPathRequest request(const sim::tactical::LandTerrain& terrain, const std::int64_t from_x, const std::int64_t from_y,
    const std::int64_t to_x, const std::int64_t to_y, const std::string_view mask = "walker") {
    LandPathRequest result;
    result.entity = 7;
    result.mask = terrain.movement_mask(mask).value();
    result.start = centre(from_x, from_y);
    result.end = centre(to_x, to_y);
    return result;
}
// Independent check of a route: every sample along every segment lies in a cell the mask admits.
inline bool walkable(const Field& field, const LandPath& path, const std::uint32_t mask) {
    for (std::size_t i = 1; i < path.nodes.size(); ++i) {
        const auto a = path.nodes[i - 1], b = path.nodes[i];
        for (int step = 0; step <= 40; ++step) {
            const auto x = Fixed::from_raw(a.x.raw() + (b.x.raw() - a.x.raw()) * step / 40);
            const auto y = Fixed::from_raw(a.y.raw() + (b.y.raw() - a.y.raw()) * step / 40);
            const auto column = land_cell_of(x, whole(20), field.columns), row = land_cell_of(y, whole(20), field.rows);
            if (!column || !row || !field.occupancy.admits(*column, *row, mask)) {
                // The start cell is exempt: a unit may leave a closed cell.
                if (i == 1 && step == 0) continue;
                return false;
            }
        }
    }
    return true;
}
inline LandPath plan(const Field& field, const LandPathRequest& req, LandPathStats* stats = nullptr) {
    LandPathArena arena;
    return LandPathBackend::plan(field.occupancy, arena, req, stats);
}
inline Fixed distance(const sim::math::Vec2 a, const sim::math::Vec2 b) {
    Fixed length;
    static_cast<void>(sim::math::try_length({Fixed::from_raw(b.x.raw() - a.x.raw()), Fixed::from_raw(b.y.raw() - a.y.raw())}, length));
    return length;
}

} // namespace eawr::land_test
