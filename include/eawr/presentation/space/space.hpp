#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/core/result.hpp"
#include "eawr/presentation/renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// E-space-primary-sky-v1 (P1-06, #27): the pure planning half of the
// environment-only space map preview. Everything here consumes values that
// the composition layer has already loaded; nothing opens a file or touches a
// graphics API. The plan is a modern diffuse preview of environment 0's
// primary sky only, plus the opt-in exact MeshGloss sky route below. It is
// not an implementation of the SKYDOME, PLANET or NEBULA effects, and a ready
// plan is never an environment-complete result.
namespace eawr::presentation::space {

inline constexpr std::string_view slice_id = "E-space-primary-sky-v1";
inline constexpr std::string_view adapter_id = "eawr-space-primary-sky-v1";
// Slice policy, not an original environment-selection rule.
inline constexpr std::size_t supported_environment_index = 0;

namespace diagnostic_codes {
inline constexpr std::string_view texture_unsupported = "EAWR-SPACE-0001";
inline constexpr std::string_view camera_invalid = "EAWR-SPACE-0002";
} // namespace diagnostic_codes

// -- plan ---------------------------------------------------------------------

enum class PlanStatus : std::uint8_t {
    ready,
    map_kind_unknown,
    not_space_map,
    terrain_present,
    map_not_semantically_complete,
    environment_absent,
    primary_sky_undeclared,
    catalog_unavailable,
    object_not_in_catalog,
    object_declares_no_model,
    model_not_in_vfs,
    model_failed_to_load,
    no_drawable_surface,
    surface_rejected,
};

enum class SurfaceStatus : std::uint8_t {
    accepted,
    // Skipped, never drawn and never counted as drawable.
    mesh_invisible,
    geometry_invalid,
    skinning_unsupported,
    hierarchy_unsupported,
    // A bone on the mesh's chain is stored hidden. No inherited visibility
    // rule is reviewed, so even a rigid chain cannot be baked and dropped.
    bone_visibility_unsupported,
    base_texture_missing,
    base_texture_duplicate,
    base_texture_wrong_type,
    multitexture_required,
    unconsumed_parameter,
    shader_not_qualified,
    texture_not_in_vfs,
    texture_failed_to_decode,
    texture_unsupported,
    // Material routes only (MeshGloss, and the opt-in MeshAdditive route): a
    // required authored field is absent, carries the wrong kind or appears
    // twice, or a value component is not finite. No default is ever
    // substituted.
    material_parameter_missing,
    material_parameter_invalid,
    material_value_nonfinite,
};

[[nodiscard]] std::string_view to_string(PlanStatus status) noexcept;
[[nodiscard]] std::string_view to_string(SurfaceStatus status) noexcept;

// A reviewed admission of one original shader identity into the opaque
// diffuse contract. Only BaseTexture is consumed; every other parameter the
// shader carries must be listed as reviewed-unconsumed, or the surface is
// rejected. A shader name alone never admits a Planet/Nebula/Skydome surface.
struct Qualification final {
    std::string shader;
    std::vector<std::string> reviewed_unconsumed_parameters;
    std::string provenance;
};

// The rows this slice ships. The only row is the synthetic fixture's invented
// shader identity; no original shader has a reviewed qualification yet.
[[nodiscard]] std::span<const Qualification> qualifications() noexcept;

// -- MeshGloss sky material route ----------------------------------------------
//
// eawr-space-sky-meshgloss-v1: an exact MeshGloss sph_t0/sph_t0_p0 material
// route for a sky surface, following docs/behaviour/meshgloss-programmable.md.
// Unlike the opaque diffuse contract it consumes the authored Emissive,
// Diffuse and Specular values as parsed from the ALO; nothing is defaulted,
// clamped or invented. It is opt-in: PlanInput::material_routes is empty by
// default, so the two-argument plan_surfaces (the sky ledger's verdict) is
// unchanged. It is not the SKYDOME, PLANET or NEBULA effect, and it does not
// admit a `.fxo` identity or MeshAdditive.
inline constexpr std::string_view meshgloss_route_id = "eawr-space-sky-meshgloss-v1";

enum class SkyMaterial : std::uint8_t {
    // The existing BaseTexture-only diffuse preview (qualifications()).
    opaque_diffuse,
    // The exact MeshGloss sph_t0_p0 arithmetic under a declared light policy.
    meshgloss,
    // The MeshAdditive t0_p0 arithmetic under declared TIME/LIGHT_SCALE
    // inputs. Opt-in synthetic route only (meshadditive_material_routes()).
    meshadditive,
};
[[nodiscard]] std::string_view to_string(SkyMaterial material) noexcept;

struct MaterialRouteRow final {
    std::string shader;
    SkyMaterial material{SkyMaterial::meshgloss};
    std::string provenance;
};

// The one reviewed row: exact identity MeshGloss.fx (case-insensitive, like
// every shader identity here).
[[nodiscard]] std::span<const MaterialRouteRow> material_routes() noexcept;

// How the route treats one authored field. `disposition` is one of
// consumed, consumed_rgb (xyz read, w recorded and not read by the selected
// arithmetic) or recorded_not_consumed.
struct FieldDisposition final {
    std::string name;
    std::string kind;
    std::string disposition;
    std::string rule;
};
[[nodiscard]] std::span<const FieldDisposition> meshgloss_fields() noexcept;

// Authored values, exactly as parsed. Present on every MeshGloss surface
// whose fields were all well-formed and finite.
struct MeshGlossMaterial final {
    assets::Vec4f emissive;
    assets::Vec4f diffuse;
    assets::Vec4f specular;
    float shininess{};
};

// The external (non-material) inputs of the MeshGloss arithmetic. `sph` holds
// the three per-channel symmetric irradiance matrices column-major (element
// [column * 4 + row]); `light_direction` is not normalised by the arithmetic.
struct SkyLightPolicy final {
    std::string id;
    std::array<std::array<float, 16>, 3> sph{};
    assets::Vec3f light_direction;
    assets::Vec3f light_specular;
    assets::Vec4f light_scale{1.0F, 1.0F, 1.0F, 1.0F};
    std::string cause;
};

// space-sky-unlit-v1: the space slice composes no light for the sky, so the
// irradiance matrices and the light-zero specular are zero and the light
// scale is (1, 1, 1, 1). Output is therefore 2 * Emissive.rgb * texture, and
// Diffuse/Specular are consumed with a zero light factor. This is a slice
// policy, not a recovered original sky light rig.
[[nodiscard]] SkyLightPolicy unlit_sky_policy();

// Engine-free reference of the arithmetic, for contracts only. The normal
// must already be the normalised world normal; `eye` is the camera eye.
struct MeshGlossVertex final {
    assets::Vec4f diffuse;
    assets::Vec3f specular;
};
[[nodiscard]] float sph_irradiance(const std::array<float, 16>& matrix, const assets::Vec3f& normal) noexcept;
[[nodiscard]] MeshGlossVertex meshgloss_vertex(const MeshGlossMaterial& material, const SkyLightPolicy& policy,
                                               const assets::Vec3f& normal, const assets::Vec3f& position,
                                               const assets::Vec3f& eye) noexcept;
// `texel` is the linear sampled texture (rgb, alpha).
[[nodiscard]] assets::Vec4f meshgloss_fragment(const MeshGlossVertex& vertex, const assets::Vec4f& texel) noexcept;

// -- MeshAdditive sky material route (opt-in, synthetic) -------------------------
//
// eawr-space-sky-meshadditive-t0-v1: the MeshAdditive preferred-pass (t0/t0_p0)
// arithmetic and declared states of docs/behaviour/meshadditive-sun-billboard.md
// A-01..A-10, for a NON-billboard rigid surface only. Every billboard bone,
// including mode 7 (`sun`), stays hierarchy_unsupported: no transform rule of
// B-01..B-09 is wired into the planner (the separate sun reference evaluator
// below is never called by it). The route is never part of material_routes(), so
// the shipped viewer, the sky ledger and every plan verdict are unchanged; it
// is reached only through meshadditive_material_routes(), which the viewer
// passes under the labelled `meshadditive-synthetic` control. It does not
// claim that the retail runtime selects t0, nor any original visual match.
inline constexpr std::string_view meshadditive_route_id = "eawr-space-sky-meshadditive-t0-v1";

// material_routes() plus the one MeshAdditive.fx row (exact identity,
// case-insensitive; `.fxo` and the MeshAdditive variants are never admitted).
[[nodiscard]] std::span<const MaterialRouteRow> meshadditive_material_routes() noexcept;
[[nodiscard]] std::span<const FieldDisposition> meshadditive_fields() noexcept;

// Authored values, exactly as parsed. `color` keeps its authored kind:
// a vector3 Color is stored with w = 0 and that w is never read.
struct MeshAdditiveMaterial final {
    assets::Vec4f color;
    assets::ParameterKind color_kind{assets::ParameterKind::vector4};
    assets::Vec4f uv_scroll_rate;
};

// The external inputs of the arithmetic, always declared, never borrowed from
// an engine clock or light rig. `time` is in seconds and only multiplies
// UVScrollRate.xy; `light_scale` is the LIGHT_SCALE rgba.
struct MeshAdditiveInputs final {
    std::string id;
    float time{};
    assets::Vec4f light_scale{1.0F, 1.0F, 1.0F, 1.0F};
    std::string cause;
};

// space-sky-meshadditive-inputs-v1: TIME frozen at 0 s and LIGHT_SCALE
// (1, 1, 1, 1). A declared slice policy matching the unlit sky policy's unit
// light scale; neither value is the recovered retail clock (gate G-04) nor the
// retail sky-object light scale (gate G-05).
[[nodiscard]] MeshAdditiveInputs meshadditive_default_inputs();
[[nodiscard]] bool finite_inputs(const MeshAdditiveInputs& inputs) noexcept;

// One declared render state of the adapter and the rule behind it.
struct RenderStatePolicy final {
    std::string state;
    std::string value;
    std::string rule;
};
inline constexpr std::string_view meshadditive_render_policy_id = "space-sky-meshadditive-state-v1";
[[nodiscard]] std::span<const RenderStatePolicy> meshadditive_render_policy() noexcept;

// Engine-free reference of the arithmetic, for contracts only.
// A-03: the unwrapped scrolled coordinate (interpolated as is).
[[nodiscard]] assets::Vec2f meshadditive_uv(const assets::Vec2f& uv, const assets::Vec4f& rate, float time) noexcept;
// A-09: the wrap addressing applied per sample, u - floor(u).
[[nodiscard]] assets::Vec2f wrap_uv(const assets::Vec2f& uv) noexcept;
// A-04..A-06: saturated Color.rgb * LIGHT_SCALE.rgb * LIGHT_SCALE.a, alpha 1.
[[nodiscard]] assets::Vec4f meshadditive_vertex_color(const MeshAdditiveMaterial& material,
                                                      const MeshAdditiveInputs& inputs) noexcept;
// A-07: texel * vertex colour, component-wise (alpha = texel alpha).
[[nodiscard]] assets::Vec4f meshadditive_fragment(const assets::Vec4f& vertex_color, const assets::Vec4f& texel) noexcept;
// A-08/A-10 into an RGBA UNORM target with separate alpha blending off:
// destination + fragment per channel, saturated.
[[nodiscard]] assets::Vec4f additive_blend(const assets::Vec4f& destination, const assets::Vec4f& fragment) noexcept;

// Placed space-environment surfaces, separate from the primary-sky planner.
// These routes are explicit t0 fallbacks; no retail technique choice is
// inferred. A caller must supply time and lighting for each draw.
inline constexpr std::string_view planet_route_id = "eawr-space-planet-t0-v1";
inline constexpr std::string_view nebula_route_id = "eawr-space-nebula-t0-v1";

enum class EnvironmentEffectStatus : std::uint8_t {
    ready, shader_unsupported, technique_unsupported, geometry_invalid,
    hierarchy_unsupported, field_missing, field_invalid, field_nonfinite,
    input_invalid,
};
[[nodiscard]] std::string_view to_string(EnvironmentEffectStatus status) noexcept;

struct EnvironmentEffectInputs final {
    std::string id;
    float time_seconds{};
    assets::Vec4f light_scale;
    assets::Vec3f light_direction;
    assets::Vec3f ambient_light;
    assets::Vec3f diffuse_light;
    assets::Vec3f specular_light;
};

struct EnvironmentEffectPlan final {
    EnvironmentEffectStatus status{EnvironmentEffectStatus::shader_unsupported};
    std::string detail;
    std::string route_id;
    std::string technique;
    std::string pass_name;
    // Exact logical names to resolve and bind, in sampler order. An absent
    // texture is a rejected plan; the caller must also fail on lookup errors.
    std::vector<std::pair<std::string, std::string>> textures;
    MaterialDescription material;
    std::vector<FieldDisposition> fields;
};

// The model overload checks the same rigid/visible hierarchy boundary as the
// primary-sky planner. Only exact Planet.fx and Nebula.fx are admitted.
[[nodiscard]] EnvironmentEffectPlan plan_environment_effect(
    const assets::Model& model, std::size_t mesh_index, std::size_t submesh_index,
    std::string_view technique, const EnvironmentEffectInputs& inputs);
// Shader sources paired with the modern_spatial descriptions above. The
// caller chooses the matching plan's route, not an arbitrary shader identity.
[[nodiscard]] std::string_view environment_effect_shader(std::string_view route_id, bool alpha_blended) noexcept;

// The effects' TIME clock (#185). Nebula.fx moves only through it: a vertex
// wave of period 1 / TFreq and a UV scroll of 3 * UVScrollRate per second of
// it. Retail's scene clock gains LogicalFPS / 1000 = 0.03 s for each 30 Hz
// sim frame a render step advanced (so it stands still while paused), and
// wraps at 28800 s. The viewer's 30 Hz presentation tick stands for the sim
// frame. `environment_effect_time` returns the clock at `tick`.
inline constexpr double effect_clock_seconds_per_tick = 0.03;
inline constexpr std::uint64_t effect_clock_wrap_ticks = 960'000; // 28800 s
[[nodiscard]] float environment_effect_time(std::uint64_t tick) noexcept;

// Declared environment content this slice does not render, kept with a cause
// so a partial preview can never read as a complete environment.
struct NotRendered final {
    std::string component;
    std::string declared;
    std::string status;
    std::string cause;
};

struct EnvironmentSelection final {
    PlanStatus status{PlanStatus::environment_absent};
    std::size_t environment_index{supported_environment_index};
    std::size_t environment_count{};
    std::optional<std::string> environment_name;
    std::string primary_sky;
    std::vector<NotRendered> not_rendered;
};

// Validates kind/terrain/completeness and selects environment 0's primary sky.
[[nodiscard]] EnvironmentSelection select_environment(const assets::Map& map);

struct ModelSelection final {
    PlanStatus status{PlanStatus::object_not_in_catalog};
    std::string declared_name;
    // Space_Model_Name, Land_Model_Name or Model_Name.
    std::string declared_tag;
    std::string object_source_path;
    std::string object_source_id;
    std::string object_layer_id;
    std::uint64_t object_line{};
};

// The existing viewer's fallback order (space, land, generic). It is recorded
// as that, not as a proven original sky model selection rule.
inline constexpr std::string_view model_selection_rule =
    "existing viewer fallback for a space map: Space_Model_Name, then Land_Model_Name, then Model_Name; "
    "not a proven original sky model selection rule";

[[nodiscard]] ModelSelection select_model(const assets::ObjectTypeRef* type, bool catalog_loaded);

struct TextureIdentity final {
    std::string logical_path;
    std::string source_id;
    std::string layer_id;
    std::string sha256;
    std::string format;
    std::string source_origin;
    bool has_alpha{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t mip_count{};
};

struct SurfacePlan final {
    std::size_t mesh_index{};
    std::size_t submesh_index{};
    std::string mesh_name;
    std::string original_shader;
    // Every parameter as "name:kind" in stored order.
    std::vector<std::string> parameters;
    std::vector<std::string> unconsumed_parameters;
    std::string base_texture;
    SurfaceStatus status{SurfaceStatus::accepted};
    // Every rejection cause found, in check order; status is the first.
    std::vector<SurfaceStatus> causes;
    std::string detail;
    std::uint64_t vertices{};
    std::uint64_t triangles{};
    // The route the surface's shader identity matched, if any.
    std::optional<SkyMaterial> material;
    // MeshGloss only: the authored values (absent when a field was missing,
    // ill-typed, duplicated or non-finite) and each field's disposition.
    std::optional<MeshGlossMaterial> meshgloss;
    // MeshAdditive only, under the same absent-when-invalid rule.
    std::optional<MeshAdditiveMaterial> meshadditive;
    std::vector<FieldDisposition> field_dispositions;
    TextureIdentity texture_identity;
    // Normalised (RGBA8 top-left, or a top-left block format) texture for an
    // accepted surface. Absent for every other status: there is no grey
    // placeholder texture in this slice.
    std::optional<assets::Texture> texture;
    // Single mesh, single submesh, no bones. A visible finite proper rigid
    // chain is baked into vertices in the source basis before renderer upload.
    // Present only for an accepted surface.
    std::optional<assets::Model> model;
};

struct ModelLookup final {
    enum class Status : std::uint8_t { resolved, not_in_vfs, failed_to_load };
    Status status{Status::not_in_vfs};
    std::string logical_path;
    std::string sha256;
    std::string failure;
    std::optional<assets::Model> model;
};

struct TextureLookup final {
    enum class Status : std::uint8_t { resolved, not_in_vfs, failed_to_decode };
    Status status{Status::not_in_vfs};
    std::string logical_path;
    std::string sha256;
    std::string failure;
    std::optional<assets::Texture> texture;
};

// Read-only visibility seam for the later #28 join. Absence is reported as
// explicitly unbound; no team or fog value is invented here.
struct VisibilityBinding final {
    std::string provider;
};

struct PlanInput final {
    const assets::Map* map{};
    const assets::ObjectTypeRef* sky_type{};
    bool catalog_loaded{};
    std::function<ModelLookup(std::string_view declared_model)> model;
    std::function<TextureLookup(std::string_view declared_texture)> texture;
    std::span<const Qualification> qualifications;
    // Empty keeps the diffuse-contract-only planner exactly as before.
    std::span<const MaterialRouteRow> material_routes;
    std::optional<VisibilityBinding> visibility;
};

struct SkyPlan final {
    PlanStatus status{PlanStatus::no_drawable_surface};
    std::string detail;
    EnvironmentSelection environment;
    ModelSelection model;
    std::string model_logical_path;
    std::string model_source_id;
    std::string model_layer_id;
    std::string model_sha256;
    std::size_t model_bones{};
    std::size_t model_meshes{};
    std::vector<SurfacePlan> surfaces;
    std::optional<VisibilityBinding> visibility;

    [[nodiscard]] std::size_t accepted_count() const noexcept;
};

// Surface-level planning of one decoded model against the qualification rows.
// Deterministic mesh-then-submesh ordinal order. Textures are not bound here.
[[nodiscard]] std::vector<SurfacePlan> plan_surfaces(
    const assets::Model& model, std::span<const Qualification> rows);
// The same, additionally matching `routes` for a shader no qualification row
// names. A qualification row wins over a route row for the same identity.
[[nodiscard]] std::vector<SurfacePlan> plan_surfaces(
    const assets::Model& model, std::span<const Qualification> rows, std::span<const MaterialRouteRow> routes);

// The whole plan. The texture lookup is called once per accepted surface's
// declared BaseTexture, in surface order.
[[nodiscard]] SkyPlan build_plan(const PlanInput& input);

// RGBA8 rows top-left for the uncompressed formats (BGRA/BGR swizzled,
// bottom-left rows flipped per mip, L8 expanded). Block formats pass through
// only when already top-left. Anything else is EAWR-SPACE-0001.
[[nodiscard]] core::Result<assets::Texture> normalize_texture(const assets::Texture& texture);

// -- fixed camera ---------------------------------------------------------------

enum class CameraStatus : std::uint8_t {
    valid,
    malformed,
    nonfinite,
    viewport_invalid,
    fov_invalid,
    near_far_invalid,
    direction_degenerate,
    up_collinear,
};

[[nodiscard]] std::string_view to_string(CameraStatus status) noexcept;
[[nodiscard]] CameraStatus validate_camera(const FixedCamera& camera) noexcept;

struct CameraParse final {
    CameraStatus status{CameraStatus::malformed};
    FixedCamera camera;
};

// Twelve comma-separated render-basis numbers: eye x,y,z, target x,y,z,
// up x,y,z, vertical fov degrees, near, far. The viewport size is supplied.
[[nodiscard]] CameraParse parse_camera(std::string_view text, std::uint32_t width, std::uint32_t height);

// -- mode-7 sun reference geometry (pure CPU, not wired) ------------------------
//
// eawr-sun-mode7-reference-av01-v1: the billboard mode-7 (`sun`) placement of
// docs/behaviour/meshadditive-sun-billboard.md B-01..B-09, as EAWR policy
// derived from the pinned MIT alo-viewer reference (AV-01). It is NOT recovered
// retail mode-7 behaviour (gate G-01). Nothing calls it: plan_surfaces,
// build_plan, the material routes, the renderer and the viewer are unchanged,
// and every billboard bone stays hierarchy_unsupported in the planner.
//
// Every vector is in the source (asset) basis: right-handed, Z up. The output
// is model space only; no sky-object world transform, camera centring, draw
// order or depth is implied (G-02). The toward-sun direction L is a caller
// input whose game source, sign and basis are open (G-03). Arithmetic is done
// in double and results are returned in double.
//
// Policy deviations from the reference (fail closed instead):
// - L vertical or near vertical (hypot(Lx, Ly) <= 1e-6 * |L|) and L zero,
//   where the reference result depends on signed zero;
// - any bone on the chain that is not a finite proper rigid transform, the
//   planner's own test (tolerance 1e-4);
// - any ancestor of the mesh bone with a billboard mode other than 0;
// - a mesh-bone mode other than exactly 7 (mode 6 included).
inline constexpr std::string_view sun_reference_policy_id = "eawr-sun-mode7-reference-av01-v1";
inline constexpr std::uint32_t sun_billboard_mode = 7;
// Thresholds. The camera ones are validate_camera's values.
inline constexpr double sun_camera_min_distance = 1.0e-6;
inline constexpr double sun_camera_min_up_length = 1.0e-6;
inline constexpr double sun_camera_min_up_sine = 1.0e-4;
inline constexpr double sun_direction_min_horizontal_ratio = 1.0e-6;

// Checks run in this declaration order after `placed`; the first failure wins.
// The mesh-bone index itself is checked first (as chain_invalid), because its
// mode cannot be read otherwise.
enum class SunPlacementStatus : std::uint8_t {
    placed,
    // The mesh bone's mode is not exactly 7.
    mode_not_sun,
    // Mesh-bone index out of range, a parent index below -1 or out of range,
    // or a chain that does not terminate.
    chain_invalid,
    // An ancestor (not the mesh bone) has a billboard mode other than 0.
    chain_billboard_ancestor,
    // A bone on the chain, the mesh bone included, is not a finite proper
    // rigid transform (scaled, sheared, reflected or non-finite).
    chain_not_proper_rigid,
    camera_nonfinite,
    // |target - eye| <= 1e-6.
    camera_direction_degenerate,
    // |up| <= 1e-6, or |cross(forward, up)| < 1e-4 for the unit vectors.
    camera_up_collinear,
    sun_direction_nonfinite,
    // Every component of L is +0 or -0.
    sun_direction_zero,
    // hypot(Lx, Ly) <= 1e-6 * |L|: the azimuth is undefined or discontinuous.
    sun_direction_vertical,
};
[[nodiscard]] std::string_view to_string(SunPlacementStatus status) noexcept;

using SunVec3 = std::array<double, 3>;

struct SunReferenceInput final {
    // The bind records exactly as parsed (assets::Bone::relative_transform),
    // composed parent after child as the planner does. Bone visibility is
    // not read here; it stays the planner's concern.
    std::span<const assets::Bone> bones;
    std::int32_t mesh_bone{-1};
    // The look-at camera in the source basis. The reference hard-codes up
    // (0, 0, 1); here it is explicit. Use source_from_render to convert a
    // render-basis FixedCamera.
    assets::Vec3f eye;
    assets::Vec3f target;
    assets::Vec3f up{0.0F, 0.0F, 1.0F};
    // L: toward the sun (the opposite of the light's travel direction). Not
    // normalised; only its angles are used.
    assets::Vec3f toward_sun;
};

// Every vector field is zero unless status is placed.
struct SunReferencePlacement final {
    SunPlacementStatus status{SunPlacementStatus::chain_invalid};
    // Names the failing bone or field; empty when placed.
    std::string detail;
    // o: the mesh bone's local origin under its bind-pose absolute transform.
    SunVec3 rest_origin{};
    // S(o): the mesh origin.
    SunVec3 sun_origin{};
    // R, U, B: camera right, up and backward (B points from target to eye).
    SunVec3 right{};
    SunVec3 view_up{};
    SunVec3 backward{};
    // S(+X), S(+Y), S(+Z); S(+X) is L normalised.
    std::array<SunVec3, 3> sun_axes{};
    // Elevation atan2(Lz, hypot(Lx, Ly)) and azimuth atan2(Ly, Lx), radians.
    double tilt{};
    double azimuth{};
};

[[nodiscard]] SunReferencePlacement sun_reference_placement(const SunReferenceInput& input);
// B-02: x*R + z*U - y*B + S(o) for a local (x, y, z). Empty unless the
// placement is `placed` and every component of `local` is finite.
[[nodiscard]] std::optional<SunVec3> sun_reference_vertex(
    const SunReferencePlacement& placement, const assets::Vec3f& local) noexcept;
// The inverse of the renderer's upload conversion (x, y, z) -> (x, z, -y):
// render (X, Y, Z) -> source (X, -Z, Y). Exact (a negation and a swap).
[[nodiscard]] assets::Vec3f source_from_render(const std::array<float, 3>& render) noexcept;

// -- predeclared evidence regions ----------------------------------------------

using Triangle = std::array<assets::Vec3f, 3>;

// Render-basis triangles of a source-basis submesh, via the documented single
// conversion (x, y, z) -> (x, z, -y) that the renderer applies at upload.
[[nodiscard]] std::vector<Triangle> render_triangles(const assets::Submesh& submesh);
// The same triangles grouped by the UV quadrant of each triangle's centroid:
// 0 = (u < .5, v < .5), 1 = (u >= .5, v < .5), 2 = (u < .5, v >= .5), 3 = both.
[[nodiscard]] std::array<std::vector<Triangle>, 4> render_triangles_by_uv_quadrant(
    const assets::Submesh& submesh);

struct ScreenMask final {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> bits;

    [[nodiscard]] std::uint64_t count() const noexcept;
    [[nodiscard]] bool at(std::uint32_t x, std::uint32_t y) const noexcept;
};

// Pixel-centre coverage of the triangles under the Godot look-at camera
// (vertical fov), with exact near/far plane clipping.
[[nodiscard]] ScreenMask rasterize(const FixedCamera& camera, std::span<const Triangle> triangles);
[[nodiscard]] ScreenMask erode(const ScreenMask& mask, std::uint32_t radius);
[[nodiscard]] ScreenMask dilate(const ScreenMask& mask, std::uint32_t radius);
[[nodiscard]] ScreenMask unite(const ScreenMask& left, const ScreenMask& right);

struct Rgb8Image final {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgb;
};

// A pixel differs when the summed absolute RGB difference exceeds this
// (about 0.04 of full scale, the land mode's threshold).
inline constexpr std::uint32_t changed_threshold = 10;
inline constexpr std::uint32_t mask_margin = 3;

struct SurfaceRegions final {
    ScreenMask mask;
    std::array<ScreenMask, 4> quadrants;
};

enum class PixelStatus : std::uint8_t {
    verified,
    not_projected,
    occluded,
    no_change,
    inconclusive,
    leaked_outside_region,
    not_submitted,
};
[[nodiscard]] std::string_view to_string(PixelStatus status) noexcept;

struct SurfacePixels final {
    PixelStatus status{PixelStatus::not_projected};
    std::uint64_t mask_pixels{};
    std::uint64_t interior_pixels{};
    std::uint64_t changed_interior{};
    std::uint64_t isolated_changed_inside{};
    std::uint64_t isolated_changed_outside{};
    std::uint64_t isolated_pixels_outside{};
    std::array<std::array<double, 3>, 4> quadrant_mean_rgb{};
    std::array<std::uint64_t, 4> quadrant_pixels{};
};

struct PixelEvaluation final {
    std::string status{"not_attempted"};
    std::string failure;
    std::vector<SurfacePixels> surfaces;
    std::uint64_t outside_pixels{};
    std::uint64_t changed_outside{};
    std::uint64_t occluder_pixels{};
    std::uint64_t occluder_changed{};
    std::string occlusion_status{"not_requested"};
};

// Evidence against the explicit sky-disabled control, never a corner colour.
// `isolated[i]` is the capture with only surface i submitted, or absent when
// surface i was not submitted. `occluder` is the foreground control's mask
// when one was composed.
[[nodiscard]] PixelEvaluation evaluate_pixels(
    std::span<const SurfaceRegions> regions,
    const Rgb8Image& configured,
    const Rgb8Image& disabled,
    std::span<const std::optional<Rgb8Image>> isolated,
    const std::optional<ScreenMask>& occluder);

} // namespace eawr::presentation::space
