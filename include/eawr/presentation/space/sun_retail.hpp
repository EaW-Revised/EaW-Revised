#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/space/space.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// -- mode-7 sun retail placement policy (pure CPU, not wired) ---------------------
//
// eawr-sun-mode7-retail-v1 (WP-16, #27): the billboard mode-7 (`sun`) placement of
// docs/behaviour/meshadditive-sun-mode7-retail.md R-M7-01..R-M7-11, as EAWR policy
// following that note's implementation handoff. It is a separate identity from
// eawr-sun-mode7-reference-av01-v1 (space.hpp), which stays unchanged and is
// rejected by the note as retail behaviour (B-02, B-03, B-05, B-07).
//
// The rule is camera-centred. The mesh origin is E + d*L, where E is the pass
// camera's eye, d is the length of the mesh bone's own authored translation and
// L is the world vector of directional light 0 pointing toward the light (the
// vector bound as DIR_LIGHT_VEC_0). Local +Y maps to n = -L/|L|, toward the eye;
// local +X maps to X = normalise(U x n), with U the camera up axis; local +Z maps
// to Z = n x X. The frame (X, n, Z) is a reflection (determinant -1). Neither the
// sky-object world transform, nor the parent chain, nor the direction of the own
// translation, nor any animated pose moves or turns the mesh.
//
// Its one caller is the approximate sun of the P1 environment view
// (environment_scene.hpp; rescope 2026-09-24 put exact mode-7 geometry on the
// fidelity list). plan_surfaces, build_plan, the material routes, the renderer
// and the sky ledger are unchanged, every billboard bone stays
// hierarchy_unsupported in the planner, and sun_retail_admissible() (exact
// admission) is false while any row of sun_retail_open_gates() is open.
//
// Every vector is in the source (asset) basis: right-handed, Z up. L is a caller
// input; how a map supplies it is gate G-03. Arithmetic is done in double.
//
// Policy choices (fail closed where the retail engine collapses or is unknown):
// - the mesh bone's authored mode must be exactly 7 (stricter than R-M7-01);
// - the chain must be well formed and finite proper rigid (the planner's own
//   test, tolerance 1e-4) with no billboard ancestor. That per-bone tolerance
//   does not make s exactly 1: small scales compound along a chain, so s is
//   derived from the composed stored matrix (R-M7-05), with the render-object
//   world transform taken as identity (no world input, G-12) and the bind pose
//   (no animation input, G-07);
// - d <= 1e-6, a non-unit L, and L parallel to the camera up axis are rejected
//   instead of drawing the collapsed mesh of R-M7-07;
// - the evaluator keeps no state between passes, so no prior degenerate pass can
//   shrink a later one (R-M7-05's retained s is not reproduced).
namespace eawr::presentation::space {

inline constexpr std::string_view sun_retail_policy_id = "eawr-sun-mode7-retail-v1";
// d <= this is sun_distance_zero.
inline constexpr double sun_retail_min_distance = 1.0e-6;
// ||L| - 1| > this is sun_direction_not_unit.
inline constexpr double sun_retail_unit_tolerance = 1.0e-4;
// |U x n| < this is sun_along_camera_up (the camera up-sine threshold).
inline constexpr double sun_retail_min_up_sine = 1.0e-4;

// R-M7-01: the draw-time mode is the authored value's low four bits.
[[nodiscard]] constexpr std::uint32_t sun_retail_draw_mode(const std::uint32_t authored) noexcept {
    return authored & 0xFU;
}
// R-M7-01: load zeroes a bone's rest translation for its descendants only when
// the authored mode is exactly 6 or 7. The bone's own d is always taken from the
// authored record, before that zeroing; EAWR's parser keeps the authored record.
[[nodiscard]] constexpr bool sun_retail_rest_translation_zeroed(const std::uint32_t authored) noexcept {
    return authored == 6U || authored == 7U;
}

// Checks run in this declaration order after `placed`; the first failure wins.
// The mesh-bone index itself is checked first (as chain_invalid), because its
// mode cannot be read otherwise.
enum class SunRetailStatus : std::uint8_t {
    placed,
    // The mesh bone's draw mode is not 7.
    mode_not_sun,
    // The draw mode is 7 but the authored value is not exactly 7 (for example
    // 23): retail draws it as mode 7 yet keeps its rest translation for
    // descendants. Outside this policy.
    mode_sun_alias,
    // Mesh-bone index out of range, a parent index below -1 or out of range,
    // or a chain that does not terminate.
    chain_invalid,
    // An ancestor (not the mesh bone) has an authored billboard mode other than 0.
    chain_billboard_ancestor,
    // A bone on the chain, the mesh bone included, is not a finite proper
    // rigid transform (scaled, sheared, reflected or non-finite).
    chain_not_proper_rigid,
    // d <= 1e-6: the mesh bone's own translation is zero or nearly so.
    sun_distance_zero,
    camera_nonfinite,
    // |target - eye| <= 1e-6.
    camera_direction_degenerate,
    // |up| <= 1e-6, or |cross(forward, up)| < 1e-4 for the unit vectors.
    camera_up_collinear,
    sun_direction_nonfinite,
    // Every component of L is +0 or -0.
    sun_direction_zero,
    // ||L| - 1| > 1e-4. The only retail producer is unit length; normalising
    // would invent semantics.
    sun_direction_not_unit,
    // |U x n| < 1e-4: the quad's roll is undefined.
    sun_along_camera_up,
};
[[nodiscard]] std::string_view to_string(SunRetailStatus status) noexcept;

struct SunRetailInput final {
    // The bind records exactly as parsed (assets::Bone::relative_transform).
    // d is the length of the mesh bone's own translation (elements 3, 7, 11).
    // Parents are checked, never composed into placement. Bone visibility is
    // not read; it stays the planner's concern.
    std::span<const assets::Bone> bones;
    std::int32_t mesh_bone{-1};
    // The look-at camera of the pass, in the source basis. Use
    // source_from_render to convert a render-basis FixedCamera.
    assets::Vec3f eye;
    assets::Vec3f target;
    assets::Vec3f up{0.0F, 0.0F, 1.0F};
    // L: directional light 0 toward the light, unit length. Used as given for
    // the distance and normalised for the facing axis, as R-M7-02/03 state.
    assets::Vec3f toward_light;
};

// Every numeric field is zero unless status is placed.
struct SunRetailPlacement final {
    SunRetailStatus status{SunRetailStatus::chain_invalid};
    // Names the failing bone or field; empty when placed.
    std::string detail;
    // d; and s (R-M7-05), the length of the third rotation column of the mesh
    // bone's stored matrix: the identity world composed with every chain record
    // below the root, the mesh bone's own included. The root's authored record
    // is not used, because the root's stored matrix is the world transform
    // (EV-RET-02); a parentless mesh bone therefore has s = 1. Translations do
    // not reach the column.
    double distance{};
    double scale{};
    // E + d*L.
    SunVec3 origin{};
    // Camera right, up and backward (B points from target to eye), for
    // diagnostics; U also sets the roll.
    SunVec3 right{};
    SunVec3 view_up{};
    SunVec3 backward{};
    // Images of local +X, +Y and +Z: X, n and Z (unit, before s).
    std::array<SunVec3, 3> axes{};
};

[[nodiscard]] SunRetailPlacement sun_retail_placement(const SunRetailInput& input);
// R-M7-06 with an identity attached local offset: origin + s*(x*X + y*n + z*Z)
// for a local (x, y, z). Empty unless the placement is `placed` and every
// component of `local` is finite.
[[nodiscard]] std::optional<SunVec3> sun_retail_vertex(
    const SunRetailPlacement& placement, const assets::Vec3f& local) noexcept;

// A gate that must close before any planner, renderer or viewer admission.
struct SunRetailGate final {
    std::string_view id;
    std::string_view blocks;
};
[[nodiscard]] std::span<const SunRetailGate> sun_retail_open_gates() noexcept;
// True only for a placed result once no gate is open; false in this increment.
[[nodiscard]] bool sun_retail_admissible(const SunRetailPlacement& placement) noexcept;

} // namespace eawr::presentation::space
