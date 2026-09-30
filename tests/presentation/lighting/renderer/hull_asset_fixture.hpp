#pragma once

// Engine-free preparation of the real MC-50 Hull shadow fixture (P1-04, #25).
// It reuses the viewer's pinned Hull plan (model_preview): the Remake MC-50
// model and its Hull BaseTexture must match their SHA-256 pins, the Hull must
// be the MeshBumpColorize.fx submesh, and its rest hierarchy must agree with
// the renderer's rest placement. It then predeclares, on the CPU only, the
// camera and the shadowed/lit masks the GPU captures are measured against.
// Private asset bytes never leave the caller's process; only hashes, paths
// and counts are reported.

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/renderer.hpp"
#include "hull_shadow_masks.hpp"
#include "model_preview.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace eawr::lighting_probe {

// Render-space directions from the Hull towards the eye, tried in order.
// Declared before any capture; the selection rule below uses CPU masks only.
inline constexpr std::array<std::array<float, 3>, 5> hull_view_candidates{{
    {1.0F, 0.45F, 0.35F},   // the exploratory preview's starboard view
    {0.6F, 1.0F, -0.6F},    // above, starboard quarter, from aft
    {-0.6F, 1.0F, -0.6F},   // above, port quarter, from aft
    {0.0F, 1.0F, -0.35F},   // near top-down from aft
    {0.8F, 0.6F, -0.9F},    // starboard, low, from aft
}};
inline constexpr std::uint32_t capture_width = 640;
inline constexpr std::uint32_t capture_height = 480;
inline constexpr float fit_margin = 1.08F;
// Minimum eroded pixels in each mask for a candidate to qualify.
inline constexpr std::size_t min_mask_pixels = 1500;
// Godot's directional shadow fades out from 0.8 of its maximum distance
// (engine default; the adapter does not set it). The whole Hull is kept
// inside that fade-free range with 10% margin.
inline constexpr float assumed_fade_start = 0.8F;
inline constexpr std::int32_t shadow_atlas_size = 4096;
// The Hull alone barely self-occludes (see diagnose_self_shadow), so a second
// instance of the same pinned Hull flies on the sun line: this far along the
// source-backed toward-light direction, then shifted to starboard (render +X)
// so its shadow covers only part of the receiver and leaves a lit control.
inline constexpr float caster_sun_distance = 120.0F;
inline constexpr float caster_lateral_offset = 70.0F;

struct Lighting final {
    std::array<std::array<float, 16>, 3> sph{};
    std::array<float, 3> toward_light{};
    std::array<float, 3> specular{};
    std::array<float, 3> shadow_floor{};
};

// alo_viewer_default_environment converted to render basis exactly as the
// synthetic probe does.
[[nodiscard]] Lighting source_backed_lighting();

struct CandidateResult final {
    std::array<float, 3> view_direction{};
    presentation::FixedCamera camera;
    float far_distance{};
    std::size_t lit{}, shadowed{}, surface{};
    std::size_t eligible{}, centre_blocked{}, raw_shadowed{};
    bool qualifies{};
};

struct FixtureOptions final {
    float caster_sun_distance{lighting_probe::caster_sun_distance};
    float caster_lateral_offset{lighting_probe::caster_lateral_offset};
};

struct Fixture final {
    // Provenance (logical paths, layer ids and SHA-256 only).
    std::string model_path;
    std::string model_layer;
    std::string model_sha256;
    std::string texture_path;
    std::string texture_layer;
    std::string texture_sha256;
    std::string mesh_name;
    std::string program, technique, pass_name;
    std::size_t vertex_count{}, index_count{}, triangle_count{};
    std::size_t skipped_degenerate{};
    std::vector<std::string> bone_chain;
    float rest_placement_delta{}, rest_tolerance{};
    viewer::model_preview::Bounds rest_asset_bounds;
    // What the renderer uploads: the Hull mesh reduced to its selected
    // submesh, with the model's bones, as the viewer does.
    assets::Model model;
    // The whole loaded model, for diagnostics only.
    assets::Model full_model;
    assets::Texture texture;
    presentation::MaterialDescription material;
    // Scene: receiver Hull at identity, caster Hull translated (render space).
    FixtureOptions options;
    std::array<float, 3> caster_translation{};
    viewer::model_preview::Bounds scene_bounds;
    // Camera and shadow coverage.
    Lighting lighting;
    std::vector<CandidateResult> candidates;
    std::size_t selected{};
    presentation::FixedCamera camera;
    float far_distance{};
    float shadow_max_distance{};
    MaskPolicy policy;
    MaskPlan masks;
};

struct Prepared final {
    std::optional<Fixture> fixture;
    std::string failure;
};

// Asset half only: mounts, pins, Hull selection, upload model/texture/material
// and lighting; no view or masks.
[[nodiscard]] Prepared load_hull_fixture(
    const std::filesystem::path& game_root, const std::filesystem::path& mod_root);

// Mounts <mod_root>/Data, <game_root>/corruption/Data, <game_root>/GameData/Data
// (highest precedence first) as the viewer does and prepares the fixture.
[[nodiscard]] Prepared prepare_hull_fixture(const std::filesystem::path& game_root,
    const std::filesystem::path& mod_root, const FixtureOptions& options = {});

// Candidate views of the receiver Hull alone, with no caster: the falsifiable
// diagnostic for why a single Hull cannot carry the acceptance masks.
[[nodiscard]] std::vector<CandidateResult> diagnose_self_shadow(const Fixture& fixture);

// Render-space triangles of the uploaded Hull (renderer axis convention
// (x, z, -y), identity instance). Degenerate triangles are dropped.
[[nodiscard]] std::vector<Triangle> render_triangles(const assets::Model& model, std::size_t* skipped);

// Matched upload-route control. `rigid_skinned` is the viewer's upload: the
// Hull mesh keeps its bone, so the renderer uploads ARRAY_BONES/WEIGHTS and
// attaches a skeleton with the identity palette. `unskinned` is the same
// model with the rigid bone's rest transform baked into its vertices before
// removing the bone binding (mesh bone -1, no palette). The renderer then
// uploads the same bind-space surface on both routes and attaches no skeleton
// on the unskinned route.
enum class UploadRoute { rigid_skinned, unskinned };
[[nodiscard]] const char* upload_route_name(UploadRoute route) noexcept;
[[nodiscard]] std::optional<assets::Model> without_skinning(const assets::Model& model);
// SHA-256 over the bind-space surface both routes upload: every visible surface's
// position, normal, tangent, binormal, first UV and index, in upload order.
// Skin data (mesh bone, palette, bone indices and weights) is excluded.
[[nodiscard]] std::string surface_sha256(const assets::Model& model);

} // namespace eawr::lighting_probe
