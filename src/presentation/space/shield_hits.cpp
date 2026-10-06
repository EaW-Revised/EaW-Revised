#include "eawr/presentation/space/shield_hits.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace eawr::presentation::space {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double degrees_per_radian = 180.0 / pi;

[[nodiscard]] Vec3d sub(const Vec3d& a, const Vec3d& b) noexcept { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
[[nodiscard]] Vec3d add(const Vec3d& a, const Vec3d& b) noexcept { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
[[nodiscard]] Vec3d scale(const Vec3d& a, const double s) noexcept { return {a[0] * s, a[1] * s, a[2] * s}; }
[[nodiscard]] double dot(const Vec3d& a, const Vec3d& b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
[[nodiscard]] Vec3d cross(const Vec3d& a, const Vec3d& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] Vec3d normalized_or_zero(const Vec3d& a) noexcept {
    const double size = std::sqrt(dot(a, a));
    if (!(size > 0.0) || !std::isfinite(size)) return {0.0, 0.0, 0.0};
    return scale(a, 1.0 / size);
}

// Rz(yaw) Ry(pitch) Rx(roll), column vectors (BP-10, the debug build: the rotation is about its local
// axes, so each call multiplies on the right).
[[nodiscard]] ShieldHitAxes facing_axes(const double yaw_degrees, const double pitch_degrees) noexcept {
    const double yaw = yaw_degrees / degrees_per_radian;
    const double pitch = pitch_degrees / degrees_per_radian;
    const double cy = std::cos(yaw);
    const double sy = std::sin(yaw);
    const double cp = std::cos(pitch);
    const double sp = std::sin(pitch);
    // Ry(p) sends +X to (cp, 0, -sp) and +Z to (sp, 0, cp); Rz(y) then turns them about +Z.
    ShieldHitAxes axes;
    axes.x = {cy * cp, sy * cp, -sp};
    axes.y = {-sy, cy, 0.0};
    axes.z = {cy * sp, sy * sp, cp};
    return axes;
}

// FoC's two-sided segment-triangle test (Collision3::Collision_Test): the fraction along the
// segment where it meets `triangle`, when that lies in (0, limit). `min_determinant` is FoC's
// 0.0001, rescaled when the test runs in a space other than the world's.
[[nodiscard]] std::optional<double> segment_meets(const ShieldTriangle& triangle, const Vec3d& start,
                                                  const Vec3d& delta, const double limit,
                                                  const double min_determinant) noexcept {
    const Vec3d edge0 = sub(triangle.b, triangle.a);
    const Vec3d edge1 = sub(triangle.c, triangle.a);
    const Vec3d p = cross(delta, edge1);
    const double determinant = dot(p, edge0);
    if (std::abs(determinant) < min_determinant) return std::nullopt;
    const double inverse = 1.0 / determinant;
    const Vec3d from_a = sub(start, triangle.a);
    const double u = dot(from_a, p) * inverse;
    if (u < 0.0 || u > 1.0) return std::nullopt;
    const Vec3d q = cross(from_a, edge0);
    const double v = dot(delta, q) * inverse;
    if (v < 0.0 || u + v > 1.0) return std::nullopt;
    const double fraction = dot(edge1, q) * inverse;
    // Past the start, and nearer than the best hit so far (the first of equals stays).
    if (fraction <= 0.0 || fraction >= limit) return std::nullopt;
    return fraction;
}

constexpr double foc_min_determinant = 1.0e-4;

// The model's rotation (quarter turn, roll, yaw) applied to a direction (live_model_point).
[[nodiscard]] Vec3d rotate_to_world(const Vec3d& value, const LivePose& pose) noexcept {
    return live_model_point(value, {0.0, 0.0, 0.0}, pose.yaw_degrees, pose.roll_degrees, 1.0);
}

// Its inverse: a world direction in model axes.
[[nodiscard]] Vec3d rotate_to_model(const Vec3d& value, const LivePose& pose) noexcept {
    const double yaw = pose.yaw_degrees / degrees_per_radian;
    const double cy = std::cos(yaw);
    const double sy = std::sin(yaw);
    const Vec3d unyawed{value[0] * cy + value[1] * sy, -value[0] * sy + value[1] * cy, value[2]};
    const double roll = pose.roll_degrees / degrees_per_radian;
    const double cr = std::cos(roll);
    const double sr = std::sin(roll);
    const Vec3d unrolled{unyawed[0], unyawed[1] * cr + unyawed[2] * sr, -unyawed[1] * sr + unyawed[2] * cr};
    return {unrolled[1], -unrolled[0], unrolled[2]};
}

// Whether the segment start + t delta, t in [0, limit], passes through the box.
[[nodiscard]] bool segment_meets_box(const Vec3d& min, const Vec3d& max, const Vec3d& start, const Vec3d& delta,
                                     const double limit) noexcept {
    double low = 0.0;
    double high = limit;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        // A small margin keeps a triangle lying in a box face inside its box.
        const double margin = 1.0e-9 * (1.0 + std::abs(min[axis]) + std::abs(max[axis]));
        const double lo = min[axis] - margin;
        const double hi = max[axis] + margin;
        if (delta[axis] == 0.0) {
            if (start[axis] < lo || start[axis] > hi) return false;
            continue;
        }
        double enter = (lo - start[axis]) / delta[axis];
        double leave = (hi - start[axis]) / delta[axis];
        if (enter > leave) std::swap(enter, leave);
        low = std::max(low, enter);
        high = std::min(high, leave);
        if (low > high) return false;
    }
    return true;
}

} // namespace

Vec3d shield_hit_direction(const std::optional<Vec3d>& collide_normal, const Vec3d& flight) noexcept {
    if (!collide_normal) return normalized_or_zero(scale(flight, -1.0));
    Vec3d direction = *collide_normal;
    if (dot(direction, flight) > 0.0) direction = scale(direction, -1.0);
    return direction;
}

ShieldHitAxes shield_hit_axes(const Vec3d& direction, const bool attached_to_collision) noexcept {
    // The debug build's facing from origin to direction (BP-10): yaw 0 when x and y match, else atan2 in [0, 360);
    // pitch 0 when z and x match (FoC's test, not the horizontal length), else the negated
    // elevation; roll 0.
    const double x = direction[0];
    const double y = direction[1];
    const double z = direction[2];
    double yaw = 0.0;
    if (!(x == 0.0 && y == 0.0)) {
        yaw = std::atan2(y, x) * degrees_per_radian;
        if (yaw < 0.0) yaw += 360.0;
    }
    double pitch = 0.0;
    if (!(z == 0.0 && x == 0.0)) pitch = -(std::atan2(z, std::sqrt(x * x + y * y)) * degrees_per_radian);
    ShieldHitAxes axes = facing_axes(yaw, pitch + 90.0);
    if (!attached_to_collision) {
        // The object transform's fixed quarter turn about the model's own +Z: model +X goes where
        // the facing sends +Y, model +Y where it sends -X.
        const ShieldHitAxes facing = axes;
        axes.x = facing.y;
        axes.y = scale(facing.x, -1.0);
    }
    return axes;
}

std::optional<ShieldSegmentHit> first_shield_hit(const std::span<const ShieldTriangle> triangles,
                                                 const Vec3d& start, const Vec3d& delta) noexcept {
    double limit = 1.0;
    const ShieldTriangle* met = nullptr;
    for (const ShieldTriangle& triangle : triangles) {
        const auto fraction = segment_meets(triangle, start, delta, limit, foc_min_determinant);
        if (!fraction) continue;
        limit = *fraction;
        met = &triangle;
    }
    if (met == nullptr) return std::nullopt;
    ShieldSegmentHit hit;
    hit.fraction = limit;
    hit.contact = add(start, scale(delta, limit));
    hit.normal = normalized_or_zero(cross(sub(met->b, met->a), sub(met->c, met->a)));
    hit.triangle = static_cast<std::size_t>(met - triangles.data());
    return hit;
}

Vec3d live_model_point(const Vec3d& model_point, const Vec3d& position, const double yaw_degrees,
                       const double roll_degrees, const double scale_factor) noexcept {
    // Quarter turn, then roll about +X, then yaw about +Z (scene::placement_transform).
    const Vec3d turned{-model_point[1], model_point[0], model_point[2]};
    const double roll = roll_degrees / degrees_per_radian;
    const double cr = std::cos(roll);
    const double sr = std::sin(roll);
    const Vec3d rolled{turned[0], turned[1] * cr - turned[2] * sr, turned[1] * sr + turned[2] * cr};
    const double yaw = yaw_degrees / degrees_per_radian;
    const double cy = std::cos(yaw);
    const double sy = std::sin(yaw);
    const Vec3d yawed{rolled[0] * cy - rolled[1] * sy, rolled[0] * sy + rolled[1] * cy, rolled[2]};
    return add(position, scale(yawed, scale_factor));
}

ShieldCollisionMesh make_shield_collision_mesh(std::vector<ShieldTriangle> triangles, std::size_t chunk_size) {
    ShieldCollisionMesh mesh;
    mesh.triangles = std::move(triangles);
    if (mesh.triangles.empty()) return mesh;
    chunk_size = std::max<std::size_t>(chunk_size, 1);
    Vec3d low = mesh.triangles.front().a;
    Vec3d high = low;
    for (std::size_t first = 0; first < mesh.triangles.size(); first += chunk_size) {
        ShieldCollisionMesh::Chunk chunk;
        chunk.first = first;
        chunk.count = std::min(chunk_size, mesh.triangles.size() - first);
        chunk.min = mesh.triangles[first].a;
        chunk.max = chunk.min;
        for (std::size_t index = first; index < first + chunk.count; ++index) {
            const ShieldTriangle& triangle = mesh.triangles[index];
            for (const Vec3d& point : {triangle.a, triangle.b, triangle.c}) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    chunk.min[axis] = std::min(chunk.min[axis], point[axis]);
                    chunk.max[axis] = std::max(chunk.max[axis], point[axis]);
                }
            }
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis], chunk.min[axis]);
            high[axis] = std::max(high[axis], chunk.max[axis]);
        }
        mesh.chunks.push_back(chunk);
    }
    mesh.centre = scale(add(low, high), 0.5);
    for (const ShieldTriangle& triangle : mesh.triangles) {
        for (const Vec3d& point : {triangle.a, triangle.b, triangle.c}) {
            const Vec3d offset = sub(point, mesh.centre);
            mesh.radius = std::max(mesh.radius, std::sqrt(dot(offset, offset)));
        }
    }
    return mesh;
}

std::optional<ShieldSegmentHit> first_shield_hit(const ShieldCollisionMesh& mesh, const LivePose& pose,
                                                 const Vec3d& start, const Vec3d& delta,
                                                 ShieldCastStats* const stats) noexcept {
    if (stats != nullptr) ++stats->casts;
    if (mesh.triangles.empty() || !(pose.scale > 0.0) || !std::isfinite(pose.scale)) return std::nullopt;
    // Into model space: the same fractions, one transform per cast instead of three per triangle.
    const double inverse_scale = 1.0 / pose.scale;
    const Vec3d model_start = rotate_to_model(scale(sub(start, pose.position), inverse_scale), pose);
    const Vec3d model_delta = rotate_to_model(scale(delta, inverse_scale), pose);
    // The mesh's sphere first.
    const double length_squared = dot(model_delta, model_delta);
    double nearest = 0.0;
    if (length_squared > 0.0) {
        nearest = std::clamp(dot(sub(mesh.centre, model_start), model_delta) / length_squared, 0.0, 1.0);
    }
    const Vec3d closest = sub(add(model_start, scale(model_delta, nearest)), mesh.centre);
    const double reach = mesh.radius * (1.0 + 1.0e-9) + 1.0e-9;
    if (dot(closest, closest) > reach * reach) {
        if (stats != nullptr) ++stats->sphere_rejects;
        return std::nullopt;
    }
    // FoC's determinant floor holds in world space; a model-space determinant is the world one
    // over the scale cubed.
    const double min_determinant = foc_min_determinant * inverse_scale * inverse_scale * inverse_scale;
    double limit = 1.0;
    const ShieldTriangle* met = nullptr;
    std::uint64_t tested = 0;
    for (const ShieldCollisionMesh::Chunk& chunk : mesh.chunks) {
        if (stats != nullptr) ++stats->chunks_tested;
        if (!segment_meets_box(chunk.min, chunk.max, model_start, model_delta, limit)) continue;
        for (std::size_t index = chunk.first; index < chunk.first + chunk.count; ++index) {
            ++tested;
            const auto fraction = segment_meets(mesh.triangles[index], model_start, model_delta, limit, min_determinant);
            if (!fraction) continue;
            limit = *fraction;
            met = &mesh.triangles[index];
        }
    }
    if (stats != nullptr) {
        stats->triangles_tested += tested;
        stats->max_triangles_per_cast = std::max(stats->max_triangles_per_cast, tested);
    }
    if (met == nullptr) return std::nullopt;
    ShieldSegmentHit hit;
    hit.fraction = limit;
    hit.contact = add(start, scale(delta, limit));
    hit.normal = rotate_to_world(normalized_or_zero(cross(sub(met->b, met->a), sub(met->c, met->a))), pose);
    hit.triangle = static_cast<std::size_t>(met - mesh.triangles.data());
    return hit;
}

std::optional<ShieldFlightHit> shield_flight_hit(const ShieldCollisionMesh& mesh, const LivePose& pose,
                                                 const Vec3d& step_start, const Vec3d& flight, const double step_length,
                                                 ShieldCastStats* const stats) noexcept {
    const Vec3d direction = normalized_or_zero(flight);
    if (dot(direction, direction) == 0.0) return std::nullopt;
    // The frame step, and on past the far side of the mesh's sphere: the nearest hit of the long
    // cast is the step's own whenever the step meets the mesh, so one cast serves both.
    const Vec3d centre = live_model_point(mesh.centre, pose.position, pose.yaw_degrees, pose.roll_degrees, pose.scale);
    const Vec3d to_centre = sub(centre, step_start);
    const double past = std::sqrt(dot(to_centre, to_centre)) + mesh.radius * pose.scale + 1.0;
    const double step = std::isfinite(step_length) ? std::max(step_length, 0.0) : 0.0;
    const double length = std::max(step, past);
    const auto hit = first_shield_hit(mesh, pose, step_start, scale(direction, length), stats);
    if (!hit) return std::nullopt;
    ShieldFlightHit result;
    result.hit = *hit;
    result.in_step = hit->fraction * length < step;
    return result;
}

} // namespace eawr::presentation::space
