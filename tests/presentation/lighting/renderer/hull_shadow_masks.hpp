#pragma once

// Engine-free CPU prediction of where a real hull must self-shadow (P1-04,
// #25). The same render-space triangles the production renderer uploads are
// ray traced on the CPU: from the fixed camera to the visible surface, then
// from that surface towards the sun. Pixels whose surface faces the sun yet is
// blocked by other hull geometry form the receiver mask; pixels with an
// unobstructed path to the sun form the lit control mask. Everything near a
// silhouette, a shadow edge, or an occluder in contact with the receiver is
// excluded, so the GPU shadow map's filter and bias cannot decide the result.
// Nothing here touches Godot, so the classifier is unit tested directly.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace eawr::lighting_probe {

struct Vec3 final {
    float x{}, y{}, z{};
};

[[nodiscard]] Vec3 operator+(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] Vec3 operator-(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] Vec3 operator*(Vec3 a, float s) noexcept;
[[nodiscard]] float dot(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] Vec3 cross(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] Vec3 normalized(Vec3 a) noexcept;

// One render-space triangle, the mean of its three vertex normals (which
// decides which side the renderer treats as the outside surface) and the
// submitted instance it belongs to.
struct Triangle final {
    Vec3 a, b, c;
    Vec3 vertex_normal;
    std::uint32_t group{};
};

// Which sides of a triangle a ray may hit. `exiting` accepts only triangles
// whose outward (vertex-normal) side faces along the ray: seen from the sun,
// those are the front faces a cull_back shadow pass rasterises.
enum class Faces : std::uint8_t { both, exiting };

struct Hit final {
    float distance{};
    std::uint32_t triangle{};
};

// Median-split bounding volume hierarchy over the triangles, both sides
// intersecting (a shadow ray is blocked by any surface).
class TriangleBvh final {
public:
    explicit TriangleBvh(std::vector<Triangle> triangles);
    [[nodiscard]] std::optional<Hit> closest(
        Vec3 origin, Vec3 direction, float min_distance, Faces faces = Faces::both) const;
    [[nodiscard]] const std::vector<Triangle>& triangles() const noexcept { return triangles_; }

private:
    struct Node final {
        Vec3 min, max;
        std::uint32_t first{}, count{}; // leaf when count > 0
        std::uint32_t left{}, right{};
    };
    std::uint32_t build(std::uint32_t first, std::uint32_t count);
    std::vector<Triangle> triangles_;
    std::vector<Node> nodes_;
};

// Perspective camera exactly as GodotRenderer::set_camera configures it:
// vertical field of view (Godot keeps height), looking_at(target, up).
struct Camera final {
    Vec3 eye, target, up{0.0F, 1.0F, 0.0F};
    float vertical_fov_degrees{45.0F};
    std::uint32_t width{}, height{};
};

// Unit direction through the centre of pixel (x, y); row 0 is the top row.
[[nodiscard]] Vec3 pixel_ray(const Camera& camera, std::uint32_t x, std::uint32_t y) noexcept;

enum class PixelClass : std::uint8_t { excluded = 0, lit = 1, shadowed = 2 };

struct MaskPolicy final {
    // Surface must face the sun (geometric normal . toward_light) ...
    float min_light_facing{0.35F};
    // ... and the camera, so it is not a grazing or culled surface.
    float min_view_facing{0.2F};
    // Geometric and vertex normals must agree this well, else the side the
    // renderer draws is ambiguous.
    float min_normal_agreement{0.5F};
    // Shadow rays leave the surface this far along its normal.
    float surface_offset{0.05F};
    // Four extra shadow rays are offset this far perpendicular to the sun
    // (then moved along the sun onto the surface's tangent plane); all five
    // must agree, so the pixel is away from any shadow edge.
    float light_footprint{1.5F};
    // A blocking hit nearer than this is contact, where bias decides; such a
    // pixel is excluded rather than called shadowed.
    float min_occluder_distance{3.0F};
    // A shadowed pixel needs a sun-facing (exiting) caster face on every
    // shadow ray, as GodotRenderer's shadow pass draws casters with their
    // material's back-face culling. A lit pixel needs no hit of either side.
    bool single_sided_casters{true};
    // Screen-space erosion: every pixel within this Chebyshev radius must have
    // the same class and a depth within `depth_tolerance` (relative).
    std::uint32_t erosion_radius{2};
    float depth_tolerance{0.02F};
    // Hits deeper than this (from the eye) are excluded; the caller keeps it
    // inside the shadow fade-free range.
    float max_depth{1.0e30F};
    // Only pixels whose visible surface belongs to this group are classified.
    std::uint32_t receiver_group{};
};

struct MaskPlan final {
    std::uint32_t width{}, height{};
    std::vector<PixelClass> classes; // row-major, row 0 at the top
    std::size_t lit{}, shadowed{}, surface{};
    // Diagnostics before erosion: eligible surface pixels (facing both the
    // camera and the sun), those whose centre shadow ray is blocked at any
    // distance, and the unanimous lit/shadowed classes.
    std::size_t eligible{}, centre_blocked{}, raw_lit{}, raw_shadowed{};
};

// `scene` holds every submitted triangle. When `receiver_only` is given, a
// shadowed pixel additionally needs all five shadow rays clear against it:
// the shadow is cast by other geometry, so removing that geometry must light
// the pixel (the missing-caster prediction), and receiver self-shadow is
// excluded.
[[nodiscard]] MaskPlan plan_masks(const TriangleBvh& scene, const TriangleBvh* receiver_only,
    const Camera& camera, Vec3 toward_light, const MaskPolicy& policy);

// Binary PGM (P5, maxval 2) of the class values; the verifier reads it back.
[[nodiscard]] std::vector<std::uint8_t> encode_pgm(const MaskPlan& plan);

} // namespace eawr::lighting_probe
