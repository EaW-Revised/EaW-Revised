#pragma once

// Engine-independent model/mesh selection and preview framing for the viewer's
// fixed-scene path. It keeps the frozen P1-01 Hangar draw (default run or an
// explicit `--eawr-mesh Hangar`) separate from the opt-in exploratory MC-50
// Hull preview (`--eawr-mesh Hull`). The Hull preview selects its own legacy
// material, BaseTexture, texture pin and bounds-fitted camera; it is never an
// acceptance capture. Nothing here touches Godot, so the rules are unit tested
// directly.

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::viewer::model_preview {

inline constexpr std::string_view pinned_model_path =
    "data/art/models/rebel_mon_calamari_mc_50.alo";
inline constexpr std::string_view pinned_model_sha256 =
    "9fd06b06d1626d8a11bea184fe73768745a6774062d44c9c39636e16202b62fe";

inline constexpr std::string_view hangar_mesh = "Hangar";
inline constexpr std::string_view hangar_program = "MeshGloss.fx";
inline constexpr std::string_view hangar_texture_path = "data/art/textures/hangar_3.dds";
inline constexpr std::string_view hangar_texture_sha256 =
    "24cfdf7a9a6d9156e9d218b2d2f6f4a1545dae63096534471dbb08da9af9572b";

inline constexpr std::string_view hull_mesh = "Hull";
inline constexpr std::string_view hull_program = "MeshBumpColorize.fx";
inline constexpr std::string_view hull_base_texture = "Rebel_Mon_Calamari_Tide.dds";
inline constexpr std::string_view hull_texture_sha256 =
    "456e88d85c9569d173c2b68bc4cc59a51e5a201aa2bf4cac8ee23afc26fab7cb";

// Render-space direction from the Hull towards the preview eye: starboard,
// above and slightly towards the bow, so the full length reads side-on.
inline constexpr std::array<float, 3> hull_view_direction{1.0F, 0.45F, 0.35F};
// The fitted corners reach at most 1/margin of the viewport half-extent.
inline constexpr float hull_fit_margin = 1.08F;

enum class Kind : std::uint8_t {
    // Pinned MC-50, no mesh override: the frozen #22 fixed-capture draw.
    frozen_hangar,
    // Pinned MC-50 with `--eawr-mesh Hangar`: the same draw, stated explicitly.
    explicit_hangar,
    // Pinned MC-50 with `--eawr-mesh Hull`: opt-in exploratory preview.
    exploratory_hull,
    // Any other `--eawr-model`: the pre-existing unpinned inspection path.
    unpinned_model,
};

enum class CameraPolicy : std::uint8_t {
    // Keep the frozen FixedCamera defaults untouched.
    frozen_fixed,
    // Fit the whole rest-pose Hull into the viewport and clip planes.
    hull_bounds_fit,
    // The pre-existing mesh-header-bounds camera for unpinned models.
    legacy_mesh_bounds,
};

[[nodiscard]] std::string_view to_string(Kind kind) noexcept;
[[nodiscard]] std::string_view to_string(CameraPolicy policy) noexcept;

[[nodiscard]] bool iequals(std::string_view left, std::string_view right) noexcept;

struct LegacySelection final {
    std::string technique;
    std::string pass;
};

[[nodiscard]] std::optional<LegacySelection> legacy_selection(std::string_view program);
[[nodiscard]] std::optional<std::string> base_texture_name(const assets::Submesh& submesh);
[[nodiscard]] std::string texture_logical_path(std::string_view name);

struct Request final {
    std::string_view model_path;
    std::string_view mesh_name;
    std::string_view texture_override;
    bool animation_requested{};
};

struct Plan final {
    Kind kind{Kind::unpinned_model};
    // Empty means any mesh / any implemented legacy program.
    std::string required_mesh;
    std::string required_program;
    // Hull only: the BaseTexture material parameter must name exactly this.
    std::string required_base_texture;
    // Empty means derive from the selected submesh's BaseTexture.
    std::string texture_path;
    // Empty means unpinned.
    std::string expected_model_sha256;
    std::string expected_texture_sha256;
    CameraPolicy camera{CameraPolicy::legacy_mesh_bounds};
    bool exploratory{};
};

// Either a value or a fail-closed reason; exactly one is set.
template <typename T>
struct Checked final {
    std::optional<T> value;
    std::string failure;
    [[nodiscard]] explicit operator bool() const noexcept { return value.has_value(); }
    [[nodiscard]] static Checked ok(T result) { return {std::move(result), {}}; }
    [[nodiscard]] static Checked fail(std::string reason) { return {std::nullopt, std::move(reason)}; }
};

[[nodiscard]] Checked<Plan> plan_for(const Request& request);

struct Selection final {
    std::size_t mesh_index{};
    std::size_t submesh_index{};
    LegacySelection material;
};

// Mirrors the frozen first-match rule: meshes in file order, first submesh
// whose program has an implemented legacy selector (and matches the plan).
[[nodiscard]] Checked<Selection> select_submesh(const assets::Model& model, const Plan& plan);

// Logical path of the texture the plan draws for the selected submesh.
[[nodiscard]] Checked<std::string> texture_path_for(
    const Plan& plan, const assets::Submesh& submesh);

// Empty expected hash means unpinned.
[[nodiscard]] bool hash_matches(std::string_view actual, std::string_view expected) noexcept;

struct Bounds final {
    std::array<float, 3> min{};
    std::array<float, 3> max{};
};

struct RestPlacement final {
    // Vertex bounds exactly as the renderer places them at rest (asset space):
    // a palette-skinned submesh draws as stored (identity bind palette); a
    // rigid one is stored in its bone's space and drawn at that bone's rest
    // transform.
    Bounds asset;
    // Bone chain from the root to the mesh's bone (names), empty if unbound.
    std::vector<std::string> chain;
    // Largest distance the rest hierarchy moves a stored rigid vertex.
    float max_placement_delta{};
    float tolerance{};
    std::size_t vertex_count{};
};

// Validates the hierarchy (parent-first, acyclic, finite), the mesh's bone,
// its vertices against the header bounds (both in stored space), and the
// finiteness of the rest placement. Only then are the bounds trusted for
// framing.
[[nodiscard]] Checked<RestPlacement> rest_placement(
    const assets::Model& model, std::size_t mesh_index, std::size_t submesh_index);

// Asset-space bounds -> renderer axis convention (x, z, -y) -> the column
// major instance transform the renderer applies. Returns the world AABB of
// the eight transformed corners.
[[nodiscard]] Bounds world_bounds(const Bounds& asset, const std::array<float, 16>& instance);

struct Fit final {
    presentation::FixedCamera camera;
    std::array<float, 3> view_direction{};
    // NDC extent of the eight world-bounds corners (all within [-1, 1]).
    float ndc_min_x{}, ndc_max_x{}, ndc_min_y{}, ndc_max_y{};
    // View depth range of the corners (strictly within near/far).
    float depth_min{}, depth_max{};
};

// Frames the whole AABB from `view_direction`: the smallest distance at which
// every corner lies within 1/margin of the viewport half-extent, clip planes
// around those corners, then re-projects every corner and fails closed unless
// each is inside the viewport and the clip range.
[[nodiscard]] Checked<Fit> fit_camera(
    const Bounds& world, const presentation::FixedCamera& base,
    const std::array<float, 3>& view_direction, float margin);

// Re-projects the corners of `world` through `camera` (perspective, square
// pixels). Exposed so tests can assert the fit independently.
struct Projection final {
    bool finite{};
    float ndc_min_x{}, ndc_max_x{}, ndc_min_y{}, ndc_max_y{};
    float depth_min{}, depth_max{};
};
[[nodiscard]] Projection project_corners(const Bounds& world, const presentation::FixedCamera& camera);

// --- Whole-unit animation preview (`--eawr-unit`, P1-03) -------------------
// Every drawable mesh of one model under one clip, sampled at several times.

// LOD and ALT tags parsed as the pinned MIT viewer does: the first "_ALT"
// and then the first "_LOD" (case-sensitive), each followed by decimal
// digits; -1 when a tag is absent or has no digits.
struct MeshLevels final {
    int alt{-1};
    int lod{-1};
};
[[nodiscard]] MeshLevels mesh_levels(std::string_view name);

// The levels a unit draws, as the reference viewer opens a model: ALT 0 (the
// undamaged state) and the highest tagged LOD (the most detailed), or the
// requested LOD clamped to the tagged range.
struct UnitLevels final {
    int alt{};
    int lod{};
    int max_alt{-1};
    int max_lod{-1};
};
[[nodiscard]] UnitLevels unit_levels(const assets::Model& model, std::optional<int> lod = std::nullopt);

struct UnitSurface final {
    std::size_t mesh{};
    std::size_t submesh{};
};
// Every submesh of a visible mesh whose tags are absent or equal `levels`,
// in file order. Shader support is the caller's filter.
[[nodiscard]] std::vector<UnitSurface> unit_surfaces(const assets::Model& model, const UnitLevels& levels);

// The reference hides a mesh whose connected bone is hidden in the pose; an
// unbound mesh is always shown.
[[nodiscard]] bool surface_visible(const assets::Model& model, const UnitSurface& surface,
    std::span<const presentation::animation::BonePose> pose);

// Asset-space bounds of the visible `surfaces` as drawn under `pose` (one
// entry per model bone): palette-skinned vertices by their weighted skin
// matrices, rigid ones by their bone's model transform (they are stored in
// its space), unbound ones as stored. Fails closed on a palette index or bone
// out of range, or a non-finite result; an empty result is a failure too.
[[nodiscard]] Checked<Bounds> posed_bounds(const assets::Model& model,
    std::span<const UnitSurface> surfaces, std::span<const presentation::animation::BonePose> pose);

// Comma-separated finite decimals, for example "0,0.25,0.5".
[[nodiscard]] Checked<std::vector<float>> parse_floats(std::string_view text);

// One frame is written at full size; several are tiled row-major, three per
// row, each at half the viewport size.
struct StripLayout final {
    std::uint32_t columns{};
    std::uint32_t rows{};
    std::uint32_t tile_width{};
    std::uint32_t tile_height{};
};
[[nodiscard]] StripLayout strip_layout(std::size_t frames, std::uint32_t width, std::uint32_t height);

} // namespace eawr::viewer::model_preview
