#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Where and how a live battle draws a shield hit (#415, docs/behaviour/battle-presentation.md
// BP-10, BP-17 to BP-19): FoC's rule for the projectile's damage application, read in
// the debug build. Pure presentation math in the source basis (+Z up); nothing here touches
// simulation state.
namespace eawr::presentation::space {

using Vec3d = std::array<double, 3>;

// A particle's axes in the source basis: where its local +X, +Y and +Z point.
struct ShieldHitAxes final {
    Vec3d x{1.0, 0.0, 0.0};
    Vec3d y{0.0, 1.0, 0.0};
    Vec3d z{0.0, 0.0, 1.0};
};

// BP-17: the direction the shield-hit particle faces. A target model with a SHIELD sub-object is
// hit on that mesh: the particle faces the mesh's normal at the contact, turned round when it
// points along the flight. A model without one faces back along the flight. `flight` is the
// projectile's velocity (any length). A flight of zero length gives the zero direction (the
// remake's choice).
[[nodiscard]] Vec3d shield_hit_direction(const std::optional<Vec3d>& collide_normal, const Vec3d& flight) noexcept;

// BP-18: the particle's axes for `direction`. FoC builds a facing from the origin to the
// direction (yaw about +Z, pitch as the negated elevation, no roll), adds 90 degrees to the
// pitch and composes Rz(yaw) Ry(pitch) Rx(0), so the particle's local +Z lies along the
// direction and its local +Y is horizontal. A particle attached to the collision
// (Particle_Attach_To_Collision) takes that transform as its offset from the hit bone; a free
// one is placed like any object, with the model's fixed +90 degree turn about its own +Z after
// the facing (R-ROT-01).
[[nodiscard]] ShieldHitAxes shield_hit_axes(const Vec3d& direction, bool attached_to_collision) noexcept;

struct ShieldTriangle final {
    Vec3d a{};
    Vec3d b{};
    Vec3d c{};
};

struct ShieldSegmentHit final {
    double fraction{};  // along the segment, in (0, 1): a surface at either end is not met
    Vec3d contact{};
    Vec3d normal{};     // the triangle's unit face normal, as wound
    std::size_t triangle{}; // PS-02: retain the contact mesh's bone alongside this triangle.
};

// BP-19: FoC's segment test against a collision mesh (Collision3::Collision_Test on each
// triangle): two-sided, the nearest hit strictly between the segment's start and end wins, and the
// normal is the hit triangle's face normal. `delta` runs from `start` to the segment's end.
[[nodiscard]] std::optional<ShieldSegmentHit> first_shield_hit(std::span<const ShieldTriangle> triangles,
                                                               const Vec3d& start, const Vec3d& delta) noexcept;

// A model-space point of a live unit in the source basis: the model's fixed +90 degree turn about
// +Z, the bank roll about the forward axis, the yaw, the uniform scale and the position
// (R-ROT-01, scene::placement_transform).
[[nodiscard]] Vec3d live_model_point(const Vec3d& model_point, const Vec3d& position, double yaw_degrees,
                                     double roll_degrees, double scale) noexcept;

// Where a unit stands as it is drawn: the arguments of live_model_point.
struct LivePose final {
    Vec3d position{};
    double yaw_degrees{};
    double roll_degrees{};
    double scale{1.0};
};

// A unit type's collision triangles in model space, kept once per type, with the bounds a cast
// tests before any triangle: the whole mesh's sphere, then runs of consecutive triangles in boxes.
struct ShieldCollisionMesh final {
    struct Chunk final {
        Vec3d min{};
        Vec3d max{};
        std::size_t first{};
        std::size_t count{};
    };
    std::vector<ShieldTriangle> triangles;
    std::vector<Chunk> chunks;
    Vec3d centre{};
    double radius{};  // from `centre`; 0 for an empty mesh
};

// The mesh over `triangles` (model space), in their order, `chunk_size` triangles per box.
[[nodiscard]] ShieldCollisionMesh make_shield_collision_mesh(std::vector<ShieldTriangle> triangles,
                                                             std::size_t chunk_size = 32);

// What the casts cost: casts made, casts whose segment missed the mesh's sphere, and the chunk
// boxes and triangles tested.
struct ShieldCastStats final {
    std::uint64_t casts{};
    std::uint64_t sphere_rejects{};
    std::uint64_t chunks_tested{};
    std::uint64_t triangles_tested{};
    std::uint64_t max_triangles_per_cast{};
};

// first_shield_hit against `mesh` posed by `pose`, for a segment in world space: the segment is
// taken into model space once (fractions are the same in both), so the triangles are neither
// copied nor posed per hit. The contact and the unit normal come back in world space.
[[nodiscard]] std::optional<ShieldSegmentHit> first_shield_hit(const ShieldCollisionMesh& mesh, const LivePose& pose,
                                                               const Vec3d& start, const Vec3d& delta,
                                                               ShieldCastStats* stats = nullptr) noexcept;

struct ShieldFlightHit final {
    ShieldSegmentHit hit;  // `fraction` along the cast from the step's start past the mesh
    bool in_step{};        // met within the projectile's frame step
};

// BP-19 in the viewer: the surface a shot meets, cast as FoC's projectile step is, from the
// step's start `step_start` along `flight` (any length). The frame step is `step_length` long;
// when it meets no triangle (the remake's collision box stopped the shot first) the cast goes on
// from the same start past the whole mesh. It never starts behind the step, so a step that begins
// inside the SHIELD bubble meets the bubble where the shot leaves it.
[[nodiscard]] std::optional<ShieldFlightHit> shield_flight_hit(const ShieldCollisionMesh& mesh, const LivePose& pose,
                                                               const Vec3d& step_start, const Vec3d& flight,
                                                               double step_length,
                                                               ShieldCastStats* stats = nullptr) noexcept;

} // namespace eawr::presentation::space
