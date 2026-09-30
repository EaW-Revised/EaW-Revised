#include "eawr/presentation/lighting/lighting.hpp"

#include <bit>
#include <cmath>

namespace eawr::presentation::lighting {
namespace {

constexpr float pi = 3.14159265358979323846F;

[[nodiscard]] float radians(const float degrees) noexcept { return degrees * pi / 180.0F; }

[[nodiscard]] Vec3 negate(const Vec3 value) noexcept { return {-value.x, -value.y, -value.z}; }

// Symmetric setter on a column-major matrix.
void set(Matrix4& matrix, const std::size_t row, const std::size_t column, const float value) noexcept {
    matrix[column * 4 + row] = value;
    matrix[row * 4 + column] = value;
}

// Little-endian float components of one candidate mini (at most a float3).
using Components = std::array<float, 3>;

// Judges the first occurrence of `id` by shape only and counts the rest.
[[nodiscard]] CandidateFieldResult read_field(std::span<const RawField> fields, const std::uint32_t id,
                                              const std::size_t expected_size, Components& components) noexcept {
    CandidateFieldResult result;
    result.id = id;
    result.expected_size = expected_size;
    for (std::size_t ordinal = 0; ordinal < fields.size(); ++ordinal) {
        if (fields[ordinal].id != id) continue;
        if (result.occurrence_count++ == 0) result.selected_ordinal = ordinal;
    }
    if (!result.selected_ordinal) return result;
    const std::span<const std::byte> bytes = fields[*result.selected_ordinal].bytes;
    result.observed_size = bytes.size();
    if (bytes.size() != result.expected_size) {
        result.status = CandidateFieldStatus::wrong_size;
        return result;
    }
    result.status = CandidateFieldStatus::decoded;
    for (std::size_t index = 0; index < result.expected_size / 4; ++index) {
        std::uint32_t bits = 0;
        for (std::size_t byte = 0; byte < 4; ++byte) {
            bits |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index * 4 + byte]))
                << (8U * byte);
        }
        components[index] = std::bit_cast<float>(bits);
        if (!std::isfinite(components[index])) result.status = CandidateFieldStatus::nonfinite;
    }
    return result;
}

} // namespace

Environment alo_viewer_default_environment() noexcept {
    // alo-viewer Config::GetDefaultEnvironment (MIT): the angles there are
    // written as (heading - 90) degrees and tilt degrees.
    Environment environment;
    environment.ambient = {0.1F, 0.1F, 0.1F, 1.0F};
    environment.specular = {1.0F, 1.0F, 1.0F, 1.0F};
    environment.shadow = {0.5F, 0.5F, 0.5F, 1.0F};
    environment.lights[0] = {direction_from_angles(radians(0.0F - 90.0F), radians(45.0F)),
                             {1.0F, 1.0F, 1.0F, 0.5F}};
    environment.lights[1] = {direction_from_angles(radians(210.0F - 90.0F), radians(-10.0F)),
                             {0.25F, 0.25F, 0.5F, 0.5F}};
    environment.lights[2] = {direction_from_angles(radians(120.0F - 90.0F), radians(-10.0F)),
                             {0.25F, 0.25F, 0.5F, 0.5F}};
    environment.source = EnvironmentSource::alo_viewer_default;
    return environment;
}

Vec3 direction_from_angles(const float heading_radians, const float tilt_radians) noexcept {
    return negate({std::cos(heading_radians) * std::cos(tilt_radians),
                   std::sin(heading_radians) * std::cos(tilt_radians),
                   std::sin(tilt_radians)});
}

Vec3 retail_light_direction(const float heading_radians, const float elevation_radians) noexcept {
    return negate({std::sin(heading_radians) * std::cos(elevation_radians),
                   -std::cos(heading_radians) * std::cos(elevation_radians),
                   std::sin(elevation_radians)});
}

std::optional<Environment> candidate_environment(const std::span<const RawField> fields) {
    return diagnose_candidate_environment(fields).environment;
}

CandidateEnvironmentDiagnostics diagnose_candidate_environment(const std::span<const RawField> fields) {
    CandidateEnvironmentDiagnostics diagnostics;
    std::array<Components, candidate_field_count> values{};
    bool complete = true;
    for (std::uint32_t id = 0; id < candidate_field_count; ++id) {
        diagnostics.fields[id] = read_field(fields, id, candidate_field_expected_size(id), values[id]);
        complete = complete && diagnostics.fields[id].status == CandidateFieldStatus::decoded;
    }
    // 0x17 starts each record at (0.5, 0.5, 0.5) (R-DEC-07). A present but
    // short or non-finite mini fails the record, as a short three-float read
    // fails the retail map load (R-DEC-04).
    Components shadow{0.5F, 0.5F, 0.5F};
    diagnostics.shadow_color = read_field(fields, shadow_color_field_id, shadow_color_field_size, shadow);
    complete = complete
        && (diagnostics.shadow_color.status == CandidateFieldStatus::decoded
            || diagnostics.shadow_color.status == CandidateFieldStatus::missing);
    if (!complete) return diagnostics;

    Environment environment = alo_viewer_default_environment();
    environment.source = EnvironmentSource::ted_candidate;
    for (std::uint32_t light = 0; light < 3; ++light) {
        const Components& color = values[0x00U + light];
        const float intensity = values[0x05U + light][0];
        const float heading = values[0x08U + light][0];
        const float elevation = values[0x0bU + light][0];
        environment.lights[light] = {retail_light_direction(heading, elevation),
                                     {color[0], color[1], color[2], intensity}};
    }
    const Components& specular = values[0x03U];
    const Components& ambient = values[0x04U];
    environment.specular = {specular[0], specular[1], specular[2], 1.0F};
    environment.ambient = {ambient[0], ambient[1], ambient[2], 1.0F};
    environment.shadow = {shadow[0], shadow[1], shadow[2], 1.0F};
    diagnostics.environment = environment;
    return diagnostics;
}

Coefficients eval_direction(const Vec3 d) noexcept {
    return {
        0.282094791773878F,
        -0.488602511902920F * d.y,
        0.488602511902920F * d.z,
        -0.488602511902920F * d.x,
        1.092548430592079F * d.x * d.y,
        -1.092548430592079F * d.y * d.z,
        0.315391565252520F * (3.0F * d.z * d.z - 1.0F),
        -1.092548430592079F * d.x * d.z,
        0.546274215296040F * (d.x * d.x - d.y * d.y),
    };
}

ColorCoefficients project_lights(const std::span<const DirectionalLight> lights) noexcept {
    ColorCoefficients result;
    for (const DirectionalLight& light : lights) {
        // alo-viewer negates Z "because of the wanted coord. system
        // handedness"; together with the D3DX odd-m signs this makes the
        // packed irradiance peak for normals facing the light (n = -d).
        const Coefficients basis = eval_direction({light.direction.x, light.direction.y, -light.direction.z});
        const std::array<float, 3> weight{light.color.r * light.color.a, light.color.g * light.color.a,
                                          light.color.b * light.color.a};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            for (std::size_t index = 0; index < 9; ++index) {
                result.rgb[channel][index] += weight[channel] * basis[index];
            }
        }
    }
    return result;
}

Coefficients project_constant(const float value) noexcept {
    Coefficients result{};
    result[0] = value * 2.0F * std::sqrt(pi);
    return result;
}

Matrix4 pack_irradiance(const Coefficients& c) noexcept {
    constexpr float c1 = 0.429043F, c2 = 0.511664F, c3 = 0.743125F, c4 = 0.886227F, c5 = 0.247708F;
    Matrix4 m{};
    set(m, 0, 0, c1 * c[8]);
    set(m, 0, 1, c1 * c[4]);
    set(m, 0, 2, c1 * c[7]);
    set(m, 0, 3, c2 * c[3]);
    set(m, 1, 1, -c1 * c[8]);
    set(m, 1, 2, c1 * c[5]);
    set(m, 1, 3, c2 * c[1]);
    set(m, 2, 2, c3 * c[6]);
    set(m, 2, 3, c2 * c[2]);
    set(m, 3, 3, c4 * c[0] - c5 * c[6]);
    return m;
}

IrradianceMatrices sh_matrices(const std::span<const DirectionalLight> lights, const Color& ambient) noexcept {
    const ColorCoefficients coefficients = project_lights(lights);
    IrradianceMatrices result;
    const std::array<float, 3> constant{ambient.r * ambient.a, ambient.g * ambient.a, ambient.b * ambient.a};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result.rgb[channel] = pack_irradiance(coefficients.rgb[channel]);
        result.rgb[channel][15] += constant[channel];
    }
    return result;
}

IrradianceMatrices sph_light_all(const Environment& environment) noexcept {
    return sh_matrices(std::span<const DirectionalLight>(environment.lights.data(), 3), environment.ambient);
}

IrradianceMatrices sph_light_fill(const Environment& environment) noexcept {
    return sh_matrices(std::span<const DirectionalLight>(environment.lights.data() + 1, 2), environment.ambient);
}

Matrix4 hemisphere_matrix(const float ambient, const float directional,
                          const std::array<float, 3>& light) noexcept {
    // Identical arithmetic to the P0 SphChannelMatrix builder
    // (src/presentation/godot/shader_adapter.hpp); a test holds them equal.
    Matrix4 m{};
    m[0 * 4 + 3] = directional * light[0] * 0.25F;
    m[1 * 4 + 3] = directional * light[1] * 0.25F;
    m[2 * 4 + 3] = directional * light[2] * 0.25F;
    m[3 * 4 + 0] = m[0 * 4 + 3];
    m[3 * 4 + 1] = m[1 * 4 + 3];
    m[3 * 4 + 2] = m[2 * 4 + 3];
    m[3 * 4 + 3] = ambient + directional * 0.5F;
    return m;
}

IrradianceMatrices hemisphere_matrices() noexcept {
    const auto& raw = hemisphere_light_direction;
    const float length = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]);
    const std::array<float, 3> light{raw[0] / length, raw[1] / length, raw[2] / length};
    return {{hemisphere_matrix(0.08F, hemisphere_directional[0], light),
             hemisphere_matrix(0.08F, hemisphere_directional[1], light),
             hemisphere_matrix(0.10F, hemisphere_directional[2], light)}};
}

IrradianceMatrices hemisphere_fill_matrices() noexcept {
    const auto& raw = hemisphere_light_direction;
    const float length = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]);
    const std::array<float, 3> light{raw[0] / length, raw[1] / length, raw[2] / length};
    return {{hemisphere_matrix(0.08F, 0.0F, light), hemisphere_matrix(0.08F, 0.0F, light),
             hemisphere_matrix(0.10F, 0.0F, light)}};
}

std::array<float, 3> sun_diffuse(const Environment& environment) noexcept {
    const Color& color = environment.lights[0].color;
    return {color.r * color.a, color.g * color.a, color.b * color.a};
}

std::array<float, 3> sun_specular(const Environment& environment) noexcept {
    const float scale = 2.0F * environment.lights[0].color.a;
    return {environment.specular.r * scale, environment.specular.g * scale, environment.specular.b * scale};
}

float evaluate(const Matrix4& matrix, const Vec3 normal) noexcept {
    const std::array<float, 4> n{normal.x, normal.y, normal.z, 1.0F};
    float result = 0.0F;
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) result += n[row] * matrix[column * 4 + row] * n[column];
    }
    return result;
}

Vec3 source_to_render(const Vec3 vector) noexcept { return {vector.x, vector.z, -vector.y}; }

Matrix4 source_to_render(const Matrix4& matrix) noexcept {
    // render axis r takes source axis p[r] with sign s[r]; w stays.
    constexpr std::array<std::size_t, 4> axis{0, 2, 1, 3};
    constexpr std::array<float, 4> sign{1.0F, 1.0F, -1.0F, 1.0F};
    Matrix4 result{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            result[column * 4 + row] = sign[row] * sign[column] * matrix[axis[column] * 4 + axis[row]];
        }
    }
    return result;
}

IrradianceMatrices source_to_render(const IrradianceMatrices& matrices) noexcept {
    return {{source_to_render(matrices.rgb[0]), source_to_render(matrices.rgb[1]),
             source_to_render(matrices.rgb[2])}};
}

std::optional<Policy> parse_policy(const std::string_view text) noexcept {
    if (text == "off") return Policy::off;
    if (text == "hemisphere") return Policy::hemisphere;
    if (text == "sh") return Policy::sh;
    return std::nullopt;
}

} // namespace eawr::presentation::lighting
