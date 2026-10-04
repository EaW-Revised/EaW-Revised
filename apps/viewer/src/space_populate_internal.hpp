#pragma once

#include "space_populate.hpp"
#include "eawr/assets/assets.hpp"
#include "eawr/scene/scene.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>

// Private asset lookup helpers shared by the space population files.
namespace eawr::presentation::godot_backend {

// The documented conversion (x, y, z) -> (x, z, -y) is a signed permutation,
// so conjugating the source matrix by it moves raw Q24 values exactly (the
// same conversion the land populate path applies).
[[nodiscard]] inline sim::math::Mat3x4 source_to_render(const sim::math::Mat3x4& source) {
    using Fixed = sim::math::Fixed;
    constexpr std::array<std::size_t, 3> axis{0, 2, 1};
    constexpr std::array<std::int64_t, 3> sign{1, 1, -1};
    sim::math::Mat3x4 result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            result.rows[row][column] = Fixed::from_raw(
                sign[row] * sign[column] * source.rows[axis[row]][axis[column]].raw());
        }
        result.rows[row][3] = Fixed::from_raw(sign[row] * source.rows[axis[row]][3].raw());
    }
    return result;
}


struct SpacePopulation::SurfaceUpload final {
    sim::AssetId renderer_asset{};
    std::optional<std::vector<animation::BonePose>> pose;
    // Source-basis bounds of the posed surface in model space.
    std::array<float, 3> minimum{1e30F, 1e30F, 1e30F};
    std::array<float, 3> maximum{-1e30F, -1e30F, -1e30F};
};

namespace space_populate_detail {
[[nodiscard]] GodotRenderer::LightingState lighting_state(const SpacePopulation::Options& options, float max_distance);
} // namespace space_populate_detail

constexpr std::array<std::string_view, 2> texture_suffixes{".tga", ".dds"};
constexpr std::array<std::string_view, 1> model_suffixes{".alo"};

[[nodiscard]] bool ieq(std::string_view left, std::string_view right);
[[nodiscard]] std::string probe(scene::VfsAssetCache& cache, std::string_view root, std::string_view name,
                                std::span<const std::string_view> suffixes);
[[nodiscard]] assets::Texture placeholder_texture();

} // namespace eawr::presentation::godot_backend
