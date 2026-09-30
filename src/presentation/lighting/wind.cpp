#include "eawr/presentation/lighting/wind.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace eawr::presentation::lighting::wind {
namespace {

// The effects' and the engine's 2 pi.
constexpr float two_pi = 6.2831855F;
constexpr float degrees_to_radians = 0.017453292F;

// Little-endian binary32 at the start of `bytes`, independent of the host byte order.
[[nodiscard]] float read_f32(const std::vector<std::byte>& bytes) noexcept {
    std::uint32_t raw = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        raw |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index])) << (8U * index);
    }
    float value = 0.0F;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

[[nodiscard]] float scalar_or(const assets::EnvironmentDescriptor& environment, const std::uint32_t id,
                              const float fallback) noexcept {
    float value = fallback;
    for (const assets::RawField& field : environment.fields) {
        if (field.id != id || field.bytes.size() < 4) continue;
        const float read = read_f32(field.bytes);
        if (std::isfinite(read)) value = read;
    }
    return value;
}

[[nodiscard]] float fraction(const float value) noexcept {
    return value - std::floor(value);
}

[[nodiscard]] assets::Vec3f scaled(const assets::Vec3f& value, const float factor) noexcept {
    return {value.x * factor, value.y * factor, value.z * factor};
}

[[nodiscard]] float element(const Matrix& matrix, const int row, const int column) noexcept {
    return matrix[static_cast<std::size_t>(column * 4 + row)];
}

constexpr Matrix identity{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                          0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};

} // namespace

EnvironmentWind environment_wind(const assets::EnvironmentDescriptor& environment) noexcept {
    EnvironmentWind wind;
    wind.heading_degrees = scalar_or(environment, heading_mini, default_heading_degrees);
    wind.speed = scalar_or(environment, speed_mini, default_speed);
    const float heading = wind.heading_degrees * degrees_to_radians;
    wind.vector = {wind.speed * std::cos(heading), wind.speed * std::sin(heading), 0.0F};
    return wind;
}

float advance_clock(const float seconds, const float delta) noexcept {
    if (!std::isfinite(delta) || !(delta > 0.0F)) return seconds;
    float advanced = seconds + std::min(delta, clock_step_limit_seconds);
    if (advanced > clock_wrap_seconds) advanced -= clock_wrap_seconds;
    return advanced;
}

float clock_at(const double seconds) noexcept {
    if (!std::isfinite(seconds) || !(seconds > 0.0)) return 0.0F;
    const double wrap = static_cast<double>(clock_wrap_seconds);
    const double wrapped = std::fmod(seconds, wrap);
    return static_cast<float>(wrapped == 0.0 ? wrap : wrapped);
}

assets::Vec3f bend_vector(const assets::Vec3f& wind, const float centre_x, const float centre_y,
                          const float seconds) noexcept {
    const float phase = two_pi * std::sin(two_pi * centre_x / bend_phase_dimension)
        * std::sin(two_pi * centre_y / bend_phase_dimension);
    // sin(2 pi t / period + phase), with the period taken out first so a
    // late clock keeps its precision.
    const float wave = std::sin(two_pi * fraction(seconds / bend_period_seconds) + phase);
    return scaled(wind, 0.5F * (wave + 1.0F));
}

Box transform_box(const Box& box, const Matrix& matrix) noexcept {
    const std::array<float, 3> centre{box.centre.x, box.centre.y, box.centre.z};
    const std::array<float, 3> half{box.half_extent.x, box.half_extent.y, box.half_extent.z};
    std::array<float, 3> moved{};
    std::array<float, 3> extent{};
    for (int row = 0; row < 3; ++row) {
        moved[static_cast<std::size_t>(row)] = element(matrix, row, 3);
        for (int column = 0; column < 3; ++column) {
            const float m = element(matrix, row, column);
            moved[static_cast<std::size_t>(row)] += m * centre[static_cast<std::size_t>(column)];
            extent[static_cast<std::size_t>(row)] += std::abs(m * half[static_cast<std::size_t>(column)]);
        }
    }
    return {{moved[0], moved[1], moved[2]}, {extent[0], extent[1], extent[2]}};
}

std::optional<Box> model_bend_box(const assets::Model& model, const std::span<const Matrix> bone_matrices) noexcept {
    std::array<float, 3> low{1e30F, 1e30F, 1e30F};
    std::array<float, 3> high{-1e30F, -1e30F, -1e30F};
    bool any = false;
    for (const assets::Mesh& mesh : model.meshes) {
        const assets::Vec3f& a = mesh.bounds_min;
        const assets::Vec3f& b = mesh.bounds_max;
        if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z)
            || !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z)) {
            continue;
        }
        const Box authored{{0.5F * (a.x + b.x), 0.5F * (a.y + b.y), 0.5F * (a.z + b.z)},
                           {0.5F * (b.x - a.x), 0.5F * (b.y - a.y), 0.5F * (b.z - a.z)}};
        const bool bound = mesh.bone >= 0 && static_cast<std::size_t>(mesh.bone) < bone_matrices.size();
        const Box placed = transform_box(authored, bound ? bone_matrices[static_cast<std::size_t>(mesh.bone)] : identity);
        const std::array<float, 3> centre{placed.centre.x, placed.centre.y, placed.centre.z};
        const std::array<float, 3> half{placed.half_extent.x, placed.half_extent.y, placed.half_extent.z};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis], centre[axis] - half[axis]);
            high[axis] = std::max(high[axis], centre[axis] + half[axis]);
        }
        any = true;
    }
    if (!any) return std::nullopt;
    return Box{{0.5F * (low[0] + high[0]), 0.5F * (low[1] + high[1]), 0.5F * (low[2] + high[2])},
               {0.5F * (high[0] - low[0]), 0.5F * (high[1] - low[1]), 0.5F * (high[2] - low[2])}};
}

std::optional<Matrix> invert_affine(const Matrix& matrix) noexcept {
    const auto m = [&matrix](const int row, const int column) { return element(matrix, row, column); };
    const float c00 = m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1);
    const float c01 = m(1, 2) * m(2, 0) - m(1, 0) * m(2, 2);
    const float c02 = m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0);
    const float determinant = m(0, 0) * c00 + m(0, 1) * c01 + m(0, 2) * c02;
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12F) return std::nullopt;
    const float s = 1.0F / determinant;
    // inverse(row, column) = cofactor(column, row) / determinant.
    std::array<std::array<float, 3>, 3> inverse{};
    inverse[0] = {c00 * s, (m(0, 2) * m(2, 1) - m(0, 1) * m(2, 2)) * s, (m(0, 1) * m(1, 2) - m(0, 2) * m(1, 1)) * s};
    inverse[1] = {c01 * s, (m(0, 0) * m(2, 2) - m(0, 2) * m(2, 0)) * s, (m(0, 2) * m(1, 0) - m(0, 0) * m(1, 2)) * s};
    inverse[2] = {c02 * s, (m(0, 1) * m(2, 0) - m(0, 0) * m(2, 1)) * s, (m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0)) * s};
    Matrix result = identity;
    for (int row = 0; row < 3; ++row) {
        float translation = 0.0F;
        for (int column = 0; column < 3; ++column) {
            const float value = inverse[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)];
            result[static_cast<std::size_t>(column * 4 + row)] = value;
            translation -= value * m(column, 3);
        }
        result[static_cast<std::size_t>(12 + row)] = translation;
    }
    return result;
}

assets::Vec3f tree_offset(const float bend_scale, const assets::Vec3f& bend, const float mesh_z,
                          const float world_height) noexcept {
    if (!(world_height > 0.0F)) return {};
    return scaled(bend, bend_scale * mesh_z * mesh_z / (world_height * world_height));
}

GrassWave grass_wave(const float bend_scale, const float wind_speed) noexcept {
    GrassWave wave;
    wave.normalized_speed = bend_scale * wind_speed / grass_fast_wind_speed;
    wave.time_scale = grass_min_time_scale + (grass_max_time_scale - grass_min_time_scale) * wave.normalized_speed;
    wave.bend_bias = grass_bend_bias * wave.normalized_speed;
    wave.bend_scale = grass_bend_min + (grass_bend_max - grass_bend_min) * wave.normalized_speed;
    return wave;
}

float grass_anim(const float time_scale, const float seconds, const float mesh_x, const float mesh_y) noexcept {
    const float phase = time_scale * seconds + mesh_x / grass_spatial_wavelength + mesh_y / grass_spatial_wavelength;
    return 0.5F + 0.5F * std::sin(6.28F * fraction(phase));
}

assets::Vec3f grass_offset(const float bend_scale, const assets::Vec3f& wind, const float seconds,
                           const float mesh_x, const float mesh_y, const float v) noexcept {
    const float speed = std::sqrt(wind.x * wind.x + wind.y * wind.y + wind.z * wind.z);
    if (!(speed > 0.0F)) return {};
    const GrassWave wave = grass_wave(bend_scale, speed);
    const float anim = grass_anim(wave.time_scale, seconds, mesh_x, mesh_y);
    return scaled(wind, (1.0F - v) * (wave.bend_bias + wave.bend_scale * anim) / speed);
}

} // namespace eawr::presentation::lighting::wind
