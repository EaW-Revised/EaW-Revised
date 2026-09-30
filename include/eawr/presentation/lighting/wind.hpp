#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/assets/map.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

// Foliage wind (#147): the scene wind and clock that bend Tree.fx and Grass.fx
// geometry, from docs/behaviour/vegetation-effects.md W-01..W-09 and
// docs/behaviour/p1-effective-environment.md R-WX-01. Vectors are in the
// TED/ALO source basis (right-handed, X right, Y forward, Z up); the Godot
// adapters convert once. Presentation-only float arithmetic: nothing here
// re-enters the simulation, opens a file or touches a graphics API.
namespace eawr::presentation::lighting::wind {

// W-02: the scene's bend period and spatial phase grid.
inline constexpr float bend_period_seconds = 3.0F;
inline constexpr float bend_phase_dimension = 1000.0F;
// W-01: the scene clock advances by at most 1 s per frame and wraps at 8 h.
inline constexpr float clock_step_limit_seconds = 1.0F;
inline constexpr float clock_wrap_seconds = 28800.0F;
// R-WX-01 and the environment record defaults (minis 0x2b and 0x2c).
inline constexpr std::uint32_t heading_mini = 0x2bU;
inline constexpr std::uint32_t speed_mini = 0x2cU;
inline constexpr float default_heading_degrees = 0.0F;
inline constexpr float default_speed = 2.0F;
// W-06..W-08: Grass.fx's wind constants.
inline constexpr float grass_fast_wind_speed = 10.0F;
inline constexpr float grass_min_time_scale = 0.125F;
inline constexpr float grass_max_time_scale = 1.0F;
inline constexpr float grass_spatial_wavelength = 20.0F;
inline constexpr float grass_bend_bias = 10.0F;
inline constexpr float grass_bend_min = 10.0F;
inline constexpr float grass_bend_max = 20.5F;

struct EnvironmentWind final {
    float heading_degrees{default_heading_degrees};
    float speed{default_speed};
    // speed * (cos h, sin h, 0): heading 0 is +X and 90 degrees is +Y.
    assets::Vec3f vector;
};

// R-WX-01 over R-DEC-02/03: the last occurrence of a mini long enough to read
// wins, a longer mini is read as its prefix and a shorter or non-finite one
// keeps the record default.
[[nodiscard]] EnvironmentWind environment_wind(const assets::EnvironmentDescriptor& environment) noexcept;

// W-01: the clock after a frame of `delta` seconds. A negative or non-finite
// delta does not move it.
[[nodiscard]] float advance_clock(float seconds, float delta) noexcept;
// W-01: the scene clock reading after `seconds` of steady time, wrapped like
// advance_clock (in (0, 28800], or 0 at the start), for fixed capture times.
[[nodiscard]] float clock_at(double seconds) noexcept;

// W-02: WIND_BEND_VECTOR.xyz of a placed model whose world box centre is
// (centre_x, centre_y): wind * 0.5 * (1 + sin(2 pi t / 3 + 2 pi sin(2 pi
// x / 1000) sin(2 pi y / 1000))).
[[nodiscard]] assets::Vec3f bend_vector(const assets::Vec3f& wind, float centre_x, float centre_y,
                                        float seconds) noexcept;

// Axis-aligned box as the engine keeps it: centre and half extents.
struct Box final {
    assets::Vec3f centre;
    assets::Vec3f half_extent;
};

// Column-major 4x4 affine matrix (element [column * 4 + row]), the
// animation module's bone matrix layout.
using Matrix = std::array<float, 16>;

// W-04: the box of `box` under `matrix` (centre transformed, half extents
// summed through the absolute rotation-scale part).
[[nodiscard]] Box transform_box(const Box& box, const Matrix& matrix) noexcept;

// W-04: the model's object-space bend box, the union of every mesh's authored
// box (hidden and collision meshes included) under its bone's reference
// matrix. A mesh without a bone, or with a bone outside `bone_matrices`, uses
// its authored box unchanged. Empty when the model has no mesh.
[[nodiscard]] std::optional<Box> model_bend_box(const assets::Model& model,
                                                std::span<const Matrix> bone_matrices) noexcept;

// The inverse of an affine matrix, or nullopt when it is singular.
[[nodiscard]] std::optional<Matrix> invert_affine(const Matrix& matrix) noexcept;

// W-03: Tree.fx's world offset of a vertex at mesh-space height `mesh_z`,
// for a model whose world box is `world_height` high. Zero when the height
// is not positive.
[[nodiscard]] assets::Vec3f tree_offset(float bend_scale, const assets::Vec3f& bend, float mesh_z,
                                        float world_height) noexcept;

// W-06/W-07: Grass.fx's per-material wind terms.
struct GrassWave final {
    float normalized_speed{};
    float time_scale{};
    float bend_bias{};
    float bend_scale{};
};
[[nodiscard]] GrassWave grass_wave(float bend_scale, float wind_speed) noexcept;
// W-06: the wave value in [0, 1] at mesh-space (x, y).
[[nodiscard]] float grass_anim(float time_scale, float seconds, float mesh_x, float mesh_y) noexcept;
// W-07: the world offset of a grass vertex with first-UV v.
[[nodiscard]] assets::Vec3f grass_offset(float bend_scale, const assets::Vec3f& wind, float seconds,
                                         float mesh_x, float mesh_y, float v) noexcept;

} // namespace eawr::presentation::lighting::wind
