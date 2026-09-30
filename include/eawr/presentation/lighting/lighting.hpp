#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Engine-independent scene lighting (P1-04, #25).
//
// Two irradiance policies produce the same output shape: three symmetric 4x4
// matrices (red, green, blue) evaluated per normal as E(n) = n4' M n4 with
// n4 = (nx, ny, nz, 1). Every legacy material adapter already consumes that
// shape as `eawr_sph_r/g/b`, which is the SPH_LIGHT_ALL input of the effects.
//
//  - `sh`: second-order (9-coefficient) spherical-harmonics projection of the
//    scene's directional lights plus ambient, following the MIT alo-viewer
//    reimplementation (src/RenderEngine/SphericalHarmonics.cpp,
//    revision 9bb0053919cc5df8377610d4f91b11d956d6c2f4): D3DX-convention
//    direction evaluation with the direction's Z negated, Ramamoorthi and
//    Hanrahan's irradiance-matrix packing, and ambient added to the constant
//    term.
//  - `hemisphere`: the frozen P0 `meshgloss-hemisphere-v1` policy, kept so the
//    P0 regression captures stay reproducible.
//
// Numeric policy: presentation-only float arithmetic, like the camera and
// terrain modules. Nothing here re-enters the simulation. The module is pure:
// no engine type, no I/O, no global state.
namespace eawr::presentation::lighting {

struct Vec3 final {
    float x{}, y{}, z{};
};

// Linear RGB plus the alo-viewer intensity channel. A light's effective
// colour is rgb * a, exactly as alo-viewer applies it.
struct Color final {
    float r{}, g{}, b{}, a{1.0F};
};

// `direction` is the direction the light travels, in the TED/ALO source basis
// (right-handed, X-right, Y-forward, Z-up). A sun overhead has direction
// (0, 0, -1).
struct DirectionalLight final {
    Vec3 direction;
    Color color;
};

enum class LightSlot : std::uint8_t { sun = 0, fill1 = 1, fill2 = 2 };

enum class EnvironmentSource : std::uint8_t {
    // alo-viewer's Config::GetDefaultEnvironment (MIT), source-backed.
    alo_viewer_default,
    // A TED environment record read through the candidate field mapping
    // below. The mapping is inferred from field shapes and ranges, not from a
    // decoded editor; it is reported as unconfirmed.
    ted_candidate,
};

[[nodiscard]] constexpr std::string_view to_string(const EnvironmentSource source) noexcept {
    switch (source) {
    case EnvironmentSource::alo_viewer_default: return "alo_viewer_default";
    case EnvironmentSource::ted_candidate: return "ted_candidate_unconfirmed";
    }
    return "invalid";
}

struct Environment final {
    std::array<DirectionalLight, 3> lights{};
    Color ambient;
    Color specular;
    // The colour a shadowed surface is multiplied by, per channel: TED field
    // 0x17 of a candidate record, else alo-viewer's shadow quad colour. Both
    // default to 0.5 grey (#225, docs/rendering.md#shadows).
    Color shadow;
    EnvironmentSource source{EnvironmentSource::alo_viewer_default};
};

[[nodiscard]] Environment alo_viewer_default_environment() noexcept;

// alo-viewer's angle convention (General/3DTypes.cpp): heading about +Z and
// tilt above the XY plane give (cos t cos h, cos t sin h, sin t); a light's
// direction is the negation of that vector.
[[nodiscard]] Vec3 direction_from_angles(float heading_radians, float tilt_radians) noexcept;

// Retail's TED light angles (R-LIT-01 of
// docs/behaviour/p1-effective-environment.md): heading a and elevation e give
// the vector toward the light (sin a cos e, -cos a cos e, sin e), so heading 0
// lies toward -Y and heading pi/2 toward +X. Returns the travel direction, its
// negation. Equal to direction_from_angles(a - pi/2, e).
[[nodiscard]] Vec3 retail_light_direction(float heading_radians, float elevation_radians) noexcept;

// One raw TED environment mini-record field, as the map loader retains it.
struct RawField final {
    std::uint32_t id{};
    std::span<const std::byte> bytes;
};

// Candidate mapping of a TED environment record (unconfirmed; see
// docs/rendering.md#spherical-harmonics): per light i in {sun, fill1, fill2},
// colour = float3 at mini 0x00+i, intensity = float at 0x05+i, heading =
// float at 0x08+i and elevation = float at 0x0b+i (radians, turned into a
// direction by retail_light_direction, R-LIT-01); specular = float3 at 0x03;
// ambient = float3 at 0x04. The first occurrence of each ID is the one
// read (see diagnose_candidate_environment). Returns nullopt when any of those
// fields is absent, the wrong size or not finite. The shadow colour is the
// float3 at 0x17, which has a per-record default (0.5, 0.5, 0.5) and may be
// absent; present, it must be twelve bytes and finite like the colour fields
// (R-DEC-04, R-DEC-07 in docs/behaviour/p1-effective-environment.md).
[[nodiscard]] std::optional<Environment> candidate_environment(std::span<const RawField> fields);

// --- candidate field diagnostics ---------------------------------------------

// Per-field outcome of reading one candidate mini. Only shape is checked:
// presence, byte size and IEEE finiteness. No range, default or semantic check
// is applied, so `decoded` says nothing about whether the mapping is right.
enum class CandidateFieldStatus : std::uint8_t { decoded, missing, wrong_size, nonfinite };

[[nodiscard]] constexpr std::string_view to_string(const CandidateFieldStatus status) noexcept {
    switch (status) {
    case CandidateFieldStatus::decoded: return "decoded";
    case CandidateFieldStatus::missing: return "missing";
    case CandidateFieldStatus::wrong_size: return "wrong_size";
    case CandidateFieldStatus::nonfinite: return "nonfinite";
    }
    return "invalid";
}

// The candidate mapping reads mini IDs 0x00..0x0d: float3 at 0x00..0x04 and a
// float at 0x05..0x0d.
inline constexpr std::size_t candidate_field_count = 14;

[[nodiscard]] constexpr std::size_t candidate_field_expected_size(const std::uint32_t id) noexcept {
    return id < 0x05U ? 12U : (id < candidate_field_count ? 4U : 0U);
}

// The shadow colour mini (float3). Unlike the light fields it has a default.
inline constexpr std::uint32_t shadow_color_field_id = 0x17U;
inline constexpr std::size_t shadow_color_field_size = 12U;

// Owns no bytes and no span, so it stays valid after the input is released.
// When an ID occurs more than once, the FIRST occurrence in input order is the
// one judged and decoded, even if it is malformed and a later one is valid;
// later occurrences are only counted. (This differs on purpose from the map
// reference ledger, where the last valid mini supplies the resolved row.)
struct CandidateFieldResult final {
    std::uint32_t id{};
    CandidateFieldStatus status{CandidateFieldStatus::missing};
    std::size_t expected_size{};
    // Byte size of the selected occurrence; 0 when missing.
    std::size_t observed_size{};
    // Index into the input span of the selected occurrence; empty when missing.
    std::optional<std::size_t> selected_ordinal;
    // Every input field carrying this ID, the selected one included.
    std::size_t occurrence_count{};
};

struct CandidateEnvironmentDiagnostics final {
    // Present exactly when all fourteen fields are `decoded` and the shadow
    // colour is `decoded` or `missing` (the default); then equal to
    // candidate_environment(fields).
    std::optional<Environment> environment;
    // Indexed by mini ID: fields[i].id == i. IDs outside 0x00..0x0d are
    // ignored here.
    std::array<CandidateFieldResult, candidate_field_count> fields{};
    // Mini 0x17, judged like the fields above. `missing` keeps the default.
    CandidateFieldResult shadow_color;
};

[[nodiscard]] CandidateEnvironmentDiagnostics diagnose_candidate_environment(std::span<const RawField> fields);

// --- spherical harmonics ----------------------------------------------------

using Coefficients = std::array<float, 9>;

// Real SH basis up to band 2 in the D3DX order and sign convention
// (D3DXSHEvalDirection, order 3): Y00, Y1-1, Y10, Y11, Y2-2, Y2-1, Y20, Y21,
// Y22 with the published constants 0.282095, 0.488603, 1.092548, 0.315392 and
// 0.546274 and the Condon-Shortley sign on odd m.
[[nodiscard]] Coefficients eval_direction(Vec3 unit_direction) noexcept;

struct ColorCoefficients final {
    std::array<Coefficients, 3> rgb{};
};

// Projection of directional lights, alo-viewer style: each light contributes
// rgb * a * Y(direction with Z negated).
[[nodiscard]] ColorCoefficients project_lights(std::span<const DirectionalLight> lights) noexcept;

// Projection of a constant function f(w) = value over the sphere: only L00 is
// non-zero and equals value * 2 * sqrt(pi).
[[nodiscard]] Coefficients project_constant(float value) noexcept;

// Column-major 4x4, matching the Godot mat4 upload and the existing
// SphChannelMatrix layout. The matrices here are symmetric.
using Matrix4 = std::array<float, 16>;

struct IrradianceMatrices final {
    std::array<Matrix4, 3> rgb{};
};

// Ramamoorthi-Hanrahan irradiance-matrix packing of nine coefficients per
// channel (constants c1..c5 = 0.429043, 0.511664, 0.743125, 0.886227,
// 0.247708), as alo-viewer composes them.
[[nodiscard]] Matrix4 pack_irradiance(const Coefficients& coefficients) noexcept;

// alo-viewer's Calculate_Matrices: project `lights`, pack, then add
// ambient.rgb * ambient.a to the constant term.
[[nodiscard]] IrradianceMatrices sh_matrices(std::span<const DirectionalLight> lights,
                                             const Color& ambient) noexcept;

// SPH_LIGHT_ALL (sun + both fills) and SPH_LIGHT_FILL (both fills) for an
// environment, both in the source basis.
[[nodiscard]] IrradianceMatrices sph_light_all(const Environment& environment) noexcept;
[[nodiscard]] IrradianceMatrices sph_light_fill(const Environment& environment) noexcept;

// The frozen P0 hemisphere policy (meshgloss-hemisphere-v1), already in the
// render basis because that is where it was defined: ambient 0.08/0.08/0.10,
// directional 2.0/1.88/1.72, light direction normalize(0.35, 0.75, 0.56).
[[nodiscard]] IrradianceMatrices hemisphere_matrices() noexcept;
[[nodiscard]] Matrix4 hemisphere_matrix(float ambient, float directional,
                                        const std::array<float, 3>& unit_light) noexcept;
inline constexpr std::array<float, 3> hemisphere_light_direction{0.35F, 0.75F, 0.56F};
// The same policy split for adapters that light the sun per pixel (#199):
// the fill is the ambient term alone and the sun's diffuse colour is the
// directional term.
[[nodiscard]] IrradianceMatrices hemisphere_fill_matrices() noexcept;
inline constexpr std::array<float, 3> hemisphere_directional{2.0F, 1.88F, 1.72F};

// The sun's diffuse colour for a per-pixel sun: light 0's rgb * a, the
// weight its SPH_LIGHT_ALL projection uses.
[[nodiscard]] std::array<float, 3> sun_diffuse(const Environment& environment) noexcept;

// Retail DIR_LIGHT_SPECULAR_0: specular rgb * 2 * sun intensity (R-LIT-04).
[[nodiscard]] std::array<float, 3> sun_specular(const Environment& environment) noexcept;

// E(n) = n4' M n4.
[[nodiscard]] float evaluate(const Matrix4& matrix, Vec3 normal) noexcept;

// Source basis (X, Y, Z-up) to the renderer's basis (x, z, -y): M' = C M C'
// with C the signed permutation, so evaluate(M', C n) == evaluate(M, n).
[[nodiscard]] Matrix4 source_to_render(const Matrix4& matrix) noexcept;
[[nodiscard]] IrradianceMatrices source_to_render(const IrradianceMatrices& matrices) noexcept;
[[nodiscard]] Vec3 source_to_render(Vec3 vector) noexcept;

enum class Policy : std::uint8_t { off, hemisphere, sh };

[[nodiscard]] constexpr std::string_view to_string(const Policy policy) noexcept {
    switch (policy) {
    case Policy::off: return "off";
    case Policy::hemisphere: return "hemisphere";
    case Policy::sh: return "sh";
    }
    return "invalid";
}

[[nodiscard]] std::optional<Policy> parse_policy(std::string_view text) noexcept;

} // namespace eawr::presentation::lighting
