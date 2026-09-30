#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/space/space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// E-space-environment-v1 (P1 #27, rescoped 2026-09-24): the pure half of the
// default space map view. Environment 0's primary and secondary sky, the map's
// planet and nebula placements and an approximate sun, seen through a default
// tactical camera. It is judged by eye against the original; every
// approximation below is a project policy, and the ones that matter are on the
// fidelity list of plan/phase-1/README.md. Nothing here opens a file or
// touches a graphics API. Vectors are in the TED/ALO source basis
// (right-handed, X right, Y forward, Z up) unless a name says render.
namespace eawr::presentation::space {

inline constexpr std::string_view environment_scene_id = "E-space-environment-v1";

// -- environment light 0 --------------------------------------------------------

// Light 0 (the sun) of one environment record, per
// docs/behaviour/p1-effective-environment.md R-LIT-01 and R-LIT-03..05.
struct EnvironmentLight final {
    // (sin a cos e, -cos a cos e, sin e) for heading a (0x08) and elevation
    // e (0x0b), radians. Unit length, pointing toward the light.
    assets::Vec3f toward_light;
    // Colour 0 (0x00) times intensity 0 (0x05).
    assets::Vec3f diffuse;
    // Specular colour (0x03) times 2 times intensity 0.
    assets::Vec3f specular;
    // Ambient colour (0x04), unscaled.
    assets::Vec3f ambient;
};

// Empty when one of those minis is absent, too short or not finite (R-DEC-07
// fails closed). For a repeated id the last complete occurrence wins (R-DEC-02).
[[nodiscard]] std::optional<EnvironmentLight> environment_light(const assets::EnvironmentDescriptor& environment);

// R-SKY-02: the sky object's orientation triple (first, 0, third) in degrees,
// from minis 0x1d/0x1f (primary) or 0x1e/0x20 (secondary). An absent or
// malformed mini reads 0, the record default.
[[nodiscard]] assets::Vec3f sky_orientation_degrees(const assets::EnvironmentDescriptor& environment, bool secondary);

// -- transforms -------------------------------------------------------------------

// Row-major 3x4 affine transform: rows [r0 r1 r2 t].
using Affine = std::array<float, 12>;
inline constexpr Affine identity_affine{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};

// R-ROT-01: v -> T + Rz(z) Ry(y) Rx(x) Rz(+90 degrees) (scale v) for an
// orientation triple (x, y, z) in degrees. R-ROT-02 (that TED mini 5 feeds this
// triple) is taken as the working reading; it is on the fidelity list.
[[nodiscard]] Affine object_transform(const assets::Vec3f& position, const assets::Vec3f& orientation_degrees,
                                      float scale) noexcept;
[[nodiscard]] assets::Vec3f transform_point(const Affine& transform, const assets::Vec3f& point) noexcept;
[[nodiscard]] assets::Vec3f transform_direction(const Affine& transform, const assets::Vec3f& direction) noexcept;
// The renderer's single basis change (x, y, z) -> (x, z, -y) applied to a
// source-basis transform, so that it can be handed to a render instance.
[[nodiscard]] Affine render_affine(const Affine& source) noexcept;
[[nodiscard]] assets::Vec3f render_from_source(const assets::Vec3f& source) noexcept;

// -- which placements are environment objects (the #32 hook) ----------------------

enum class EnvironmentFamily : std::uint8_t { none, planet, nebula };
[[nodiscard]] std::string_view to_string(EnvironmentFamily family) noexcept;

// A model with a visible Planet.fx surface is a planet; otherwise one with a
// visible Nebula.fx surface is a nebula. The environment composes exactly
// these placements, every surface of their model included; the populated scene
// composes everything else.
[[nodiscard]] EnvironmentFamily environment_family(const assets::Model& model) noexcept;

// -- surfaces of one environment model ---------------------------------------------

enum class SceneRoute : std::uint8_t {
    meshgloss,            // MeshGloss.fx: the sky MeshGloss route, unlit policy
    meshadditive,         // MeshAdditive.fx: the t0 additive route
    meshadditive_vcolor,  // MeshAdditiveVColor.fx: additive times vertex colour
    planet,               // Planet.fx: plan_environment_effect t0
    nebula,               // Nebula.fx: plan_environment_effect t0
    unsupported,
};
[[nodiscard]] std::string_view to_string(SceneRoute route) noexcept;

struct SceneSurface final {
    std::size_t mesh_index{};
    std::size_t submesh_index{};
    std::string mesh_name;
    std::string shader;
    SceneRoute route{SceneRoute::unsupported};
    // The mesh bone's authored billboard mode; 0 is none.
    std::uint32_t billboard{};
    std::int32_t mesh_bone{-1};
    // Empty when the surface can be drawn; otherwise why it is not.
    std::string problem;
    // One mesh, one submesh, no bones, in the model's own space. A plain
    // surface has its rigid bone chain baked in; a billboard keeps its local
    // geometry, and `bone_origin` is its parent's bind-pose origin in model
    // space.
    assets::Model model;
    assets::Vec3f bone_origin;
    std::string base_texture;
    // Authored values of the MeshGloss and additive routes (MeshAdditiveVColor
    // has no Color field and carries (1, 1, 1, 1)).
    std::optional<MeshGlossMaterial> meshgloss;
    std::optional<MeshAdditiveMaterial> meshadditive;
};

// Every submesh of every visible mesh, in mesh-then-submesh order. Hidden
// meshes are skipped.
[[nodiscard]] std::vector<SceneSurface> scene_surfaces(const assets::Model& model);
// The largest distance from the model origin of a drawable non-billboard
// vertex; 0 when there is none.
[[nodiscard]] float surface_radius(std::span<const SceneSurface> surfaces) noexcept;

// -- billboards ---------------------------------------------------------------------

struct Billboard final {
    // Empty when placed.
    std::string problem;
    // The surface's quad baked to world space: boneless, one mesh.
    assets::Model model;
};

// Approximate mode-7 sun (the exact geometry is on the fidelity list): the
// retail placement of sun_retail.hpp evaluated at the pass camera with L the
// environment's toward-light vector, then pulled toward the eye so that its
// origin is at most `max_distance` away. The quad shrinks by the same factor,
// so its direction and angular size are kept.
[[nodiscard]] Billboard sun_billboard(const assets::Model& sky_model, const SceneSurface& surface,
                                      const assets::Vec3f& eye, const assets::Vec3f& target, const assets::Vec3f& up,
                                      const assets::Vec3f& toward_light, float max_distance);

// Retail mode 6 shifts the camera-facing quad from its parent's origin toward
// light 0 in the viewing plane. `distance` is the length of the bone's own
// authored translation; `face_scale` is its retained facing-axis scale. Inputs
// share one coordinate basis. The projected light is not renormalized.
[[nodiscard]] std::optional<assets::Vec3f> sunlight_glow_offset(const assets::Vec3f& toward_light,
    const assets::Vec3f& toward_eye, float distance, float face_scale) noexcept;

// Legacy geometry helper: a quad
// facing the eye (local +Y toward it, local +X along up x that direction) at
// the parent's origin under `object`, sized by `scale`, then moved toward the
// eye by `clearance` world units (away from it when negative) with its size
// changed by the same factor, so its angular size is kept. The view passes
// minus the planet's radius: the glow disc sits behind the planet and only its
// soft edge shows around the limb. Drawn over the face instead, the additive
// glow saturates the lit planet to white.
[[nodiscard]] Billboard facing_billboard(const SceneSurface& surface, const Affine& object, float scale,
                                         const assets::Vec3f& eye, const assets::Vec3f& up, float clearance);

// -- default camera ------------------------------------------------------------------

// The render far plane of the environment view and the radius every sky is
// scaled to around the eye. Backdrop objects (the planet, the sun, nebulae)
// lie far beyond Space_Mode's Far_Clip, so the view uses its own far plane.
inline constexpr float environment_far_plane = 60000.0F;
inline constexpr float environment_sky_radius = 48000.0F;

// Space_Mode supplies the pose and near plane; the environment needs its own
// far plane to keep the camera-centred sky and distant backdrop in view.
[[nodiscard]] FixedCamera environment_view_camera(FixedCamera camera) noexcept;

// Move a sky or sky-owned sun instance by the eye displacement without changing
// its authored orientation, scale or local geometry.
[[nodiscard]] Affine camera_relative_sky_transform(const Affine& original,
    const assets::Vec3f& original_eye, const assets::Vec3f& current_eye) noexcept;

// The Space_Mode camera values the default view reads.
struct TacticalDefaults final {
    float distance{1000.0F};
    float pitch_degrees{50.0F};
    float fov_degrees{55.0F};
    float near_plane{10.0F};
    float yaw_degrees{0.0F};
};

inline constexpr std::string_view default_camera_policy =
    "target: the first Team_01_Spawn_Point_Marker placement (local player 1, lowest record), else the first "
    "Team_01_Base_Position_Marker, else the centre of the first source volume, else the source origin; "
    "yaw: Space_Mode Yaw_Default; distance, pitch, fov and near plane: Space_Mode "
    "Distance_Default, Pitch_Default, Fov_Default and Near_Clip; far plane: the environment far plane. A project "
    "policy for an opening skirmish view, not a recovered original camera";

struct DefaultCamera final {
    FixedCamera camera;
    // start_marker, volume_centre or origin.
    std::string target_kind;
    std::optional<std::uint32_t> target_record;
    std::string target_type;
    assets::Vec3f target_source;
    assets::Vec3f centre_source;
};

[[nodiscard]] DefaultCamera default_space_camera(const assets::Map& map, const TacticalDefaults& tactical,
                                                 std::uint32_t width, std::uint32_t height);

} // namespace eawr::presentation::space
