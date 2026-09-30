#include "hull_shadow_masks.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace eawr::lighting_probe {

Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalized(Vec3 a) noexcept {
    const float length = std::sqrt(dot(a, a));
    return length > 0.0F ? a * (1.0F / length) : Vec3{};
}

namespace {

[[nodiscard]] float axis(Vec3 v, int index) noexcept {
    return index == 0 ? v.x : index == 1 ? v.y : v.z;
}

[[nodiscard]] Vec3 centroid(const Triangle& t) noexcept {
    return (t.a + t.b + t.c) * (1.0F / 3.0F);
}

// Moller-Trumbore, both sides.
[[nodiscard]] std::optional<float> intersect(const Triangle& t, Vec3 origin, Vec3 direction) noexcept {
    const Vec3 edge1 = t.b - t.a;
    const Vec3 edge2 = t.c - t.a;
    const Vec3 p = cross(direction, edge2);
    const float determinant = dot(edge1, p);
    if (std::abs(determinant) < 1.0e-12F) return std::nullopt;
    const float inverse = 1.0F / determinant;
    const Vec3 s = origin - t.a;
    const float u = dot(s, p) * inverse;
    if (u < 0.0F || u > 1.0F) return std::nullopt;
    const Vec3 q = cross(s, edge1);
    const float v = dot(direction, q) * inverse;
    if (v < 0.0F || u + v > 1.0F) return std::nullopt;
    return dot(edge2, q) * inverse;
}

[[nodiscard]] bool box_hit(Vec3 min, Vec3 max, Vec3 origin, Vec3 inverse, float limit) noexcept {
    float near = 0.0F;
    float far = limit;
    for (int index = 0; index < 3; ++index) {
        float t0 = (axis(min, index) - axis(origin, index)) * axis(inverse, index);
        float t1 = (axis(max, index) - axis(origin, index)) * axis(inverse, index);
        if (t0 > t1) std::swap(t0, t1);
        near = std::max(near, t0);
        far = std::min(far, t1);
        if (near > far) return false;
    }
    return true;
}

} // namespace

TriangleBvh::TriangleBvh(std::vector<Triangle> triangles) : triangles_(std::move(triangles)) {
    if (!triangles_.empty()) {
        nodes_.reserve(triangles_.size() * 2);
        static_cast<void>(build(0, static_cast<std::uint32_t>(triangles_.size())));
    }
}

std::uint32_t TriangleBvh::build(const std::uint32_t first, const std::uint32_t count) {
    Node node;
    node.min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    node.max = node.min * -1.0F;
    Vec3 centre_min = node.min;
    Vec3 centre_max = node.max;
    for (std::uint32_t index = first; index < first + count; ++index) {
        const Triangle& t = triangles_[index];
        for (const Vec3& p : {t.a, t.b, t.c}) {
            node.min = {std::min(node.min.x, p.x), std::min(node.min.y, p.y), std::min(node.min.z, p.z)};
            node.max = {std::max(node.max.x, p.x), std::max(node.max.y, p.y), std::max(node.max.z, p.z)};
        }
        const Vec3 c = centroid(t);
        centre_min = {std::min(centre_min.x, c.x), std::min(centre_min.y, c.y), std::min(centre_min.z, c.z)};
        centre_max = {std::max(centre_max.x, c.x), std::max(centre_max.y, c.y), std::max(centre_max.z, c.z)};
    }
    const auto self = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(node);
    if (count <= 4) {
        nodes_[self].first = first;
        nodes_[self].count = count;
        return self;
    }
    const Vec3 extent = centre_max - centre_min;
    const int split = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
    const auto begin = triangles_.begin() + first;
    const auto middle = begin + count / 2;
    std::nth_element(begin, middle, begin + count, [split](const Triangle& l, const Triangle& r) {
        return axis(centroid(l), split) < axis(centroid(r), split);
    });
    const std::uint32_t left = build(first, count / 2);
    const std::uint32_t right = build(first + count / 2, count - count / 2);
    nodes_[self].left = left;
    nodes_[self].right = right;
    return self;
}

std::optional<Hit> TriangleBvh::closest(
    Vec3 origin, Vec3 direction, float min_distance, Faces faces) const {
    if (nodes_.empty()) return std::nullopt;
    const auto reciprocal = [](float value) { return 1.0F / (value == 0.0F ? 1.0e-30F : value); };
    const Vec3 inverse{reciprocal(direction.x), reciprocal(direction.y), reciprocal(direction.z)};
    std::optional<Hit> best;
    float limit = std::numeric_limits<float>::max();
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const Node& node = nodes_[stack.back()];
        stack.pop_back();
        if (!box_hit(node.min, node.max, origin, inverse, limit)) continue;
        if (node.count > 0) {
            for (std::uint32_t index = node.first; index < node.first + node.count; ++index) {
                const Triangle& triangle = triangles_[index];
                const auto t = intersect(triangle, origin, direction);
                if (t && faces == Faces::exiting) {
                    Vec3 outward = cross(triangle.b - triangle.a, triangle.c - triangle.a);
                    if (dot(outward, triangle.vertex_normal) < 0.0F) outward = outward * -1.0F;
                    if (dot(outward, direction) <= 0.0F) continue;
                }
                if (t && *t > min_distance && *t < limit) {
                    limit = *t;
                    best = Hit{*t, index};
                }
            }
        } else {
            stack.push_back(node.left);
            stack.push_back(node.right);
        }
    }
    return best;
}

Vec3 pixel_ray(const Camera& camera, const std::uint32_t x, const std::uint32_t y) noexcept {
    const Vec3 forward = normalized(camera.target - camera.eye);
    const Vec3 right = normalized(cross(forward, camera.up));
    const Vec3 up = cross(right, forward);
    const float tangent = std::tan(camera.vertical_fov_degrees * 0.5F * 3.14159265358979F / 180.0F);
    const float aspect = static_cast<float>(camera.width) / static_cast<float>(camera.height);
    const float ndc_x = 2.0F * (static_cast<float>(x) + 0.5F) / static_cast<float>(camera.width) - 1.0F;
    const float ndc_y = 1.0F - 2.0F * (static_cast<float>(y) + 0.5F) / static_cast<float>(camera.height);
    return normalized(forward + right * (ndc_x * tangent * aspect) + up * (ndc_y * tangent));
}

MaskPlan plan_masks(const TriangleBvh& scene, const TriangleBvh* receiver_only,
    const Camera& camera, Vec3 toward_light, const MaskPolicy& policy) {
    MaskPlan plan;
    plan.width = camera.width;
    plan.height = camera.height;
    const std::size_t total = static_cast<std::size_t>(camera.width) * camera.height;
    std::vector<PixelClass> raw(total, PixelClass::excluded);
    std::vector<float> depth(total, -1.0F);
    const Vec3 light = normalized(toward_light);
    const Vec3 helper = std::abs(light.y) < 0.9F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    const Vec3 u = normalized(cross(light, helper));
    const Vec3 v = cross(light, u);
    const std::array<Vec3, 5> offsets{{{}, u * policy.light_footprint, u * -policy.light_footprint,
        v * policy.light_footprint, v * -policy.light_footprint}};
    const Vec3 forward = normalized(camera.target - camera.eye);
    for (std::uint32_t y = 0; y < camera.height; ++y) {
        for (std::uint32_t x = 0; x < camera.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * camera.width + x;
            const Vec3 direction = pixel_ray(camera, x, y);
            const auto hit = scene.closest(camera.eye, direction, 0.0F);
            if (!hit) continue;
            depth[index] = hit->distance * dot(direction, forward);
            const Triangle& t = scene.triangles()[hit->triangle];
            if (t.group != policy.receiver_group) continue;
            ++plan.surface;
            Vec3 normal = normalized(cross(t.b - t.a, t.c - t.a));
            const Vec3 vertex_normal = normalized(t.vertex_normal);
            const float agreement = dot(normal, vertex_normal);
            if (agreement < 0.0F) normal = normal * -1.0F;
            if (std::abs(agreement) < policy.min_normal_agreement) continue;
            if (dot(normal, direction * -1.0F) < policy.min_view_facing) continue;
            if (dot(normal, light) < policy.min_light_facing) continue;
            if (depth[index] > policy.max_depth) continue;
            ++plan.eligible;
            const Vec3 surface = camera.eye + direction * hit->distance + normal * policy.surface_offset;
            bool all_clear = true;
            bool all_blocked = true;
            if (scene.closest(surface, light, 0.0F)) ++plan.centre_blocked;
            for (const Vec3& offset : offsets) {
                // Slide the offset along the sun direction back onto the
                // surface's tangent plane: the same shadow line, but a start
                // point outside a thin or curved surface.
                const Vec3 on_plane = offset - light * (dot(offset, normal) / dot(light, normal));
                const Vec3 origin = surface + on_plane;
                const auto blocker = scene.closest(origin, light, 0.0F);
                if (blocker) all_clear = false;
                const auto caster = policy.single_sided_casters
                    ? scene.closest(origin, light, 0.0F, Faces::exiting) : blocker;
                if (!caster || caster->distance < policy.min_occluder_distance
                    || (blocker && blocker->distance < policy.min_occluder_distance)) {
                    all_blocked = false;
                }
                if (receiver_only && receiver_only->closest(origin, light, 0.0F)) all_blocked = false;
            }
            if (all_clear) {
                raw[index] = PixelClass::lit;
                ++plan.raw_lit;
            } else if (all_blocked) {
                raw[index] = PixelClass::shadowed;
                ++plan.raw_shadowed;
            }
        }
    }
    plan.classes.assign(total, PixelClass::excluded);
    const auto radius = static_cast<std::int64_t>(policy.erosion_radius);
    for (std::uint32_t y = 0; y < camera.height; ++y) {
        for (std::uint32_t x = 0; x < camera.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * camera.width + x;
            const PixelClass value = raw[index];
            if (value == PixelClass::excluded) continue;
            bool uniform = true;
            for (std::int64_t dy = -radius; dy <= radius && uniform; ++dy) {
                for (std::int64_t dx = -radius; dx <= radius && uniform; ++dx) {
                    const std::int64_t nx = static_cast<std::int64_t>(x) + dx;
                    const std::int64_t ny = static_cast<std::int64_t>(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= camera.width || ny >= camera.height) {
                        uniform = false;
                        break;
                    }
                    const std::size_t other = static_cast<std::size_t>(ny) * camera.width
                        + static_cast<std::size_t>(nx);
                    uniform = raw[other] == value
                        && std::abs(depth[other] - depth[index]) <= policy.depth_tolerance * depth[index];
                }
            }
            if (!uniform) continue;
            plan.classes[index] = value;
            if (value == PixelClass::lit) ++plan.lit;
            else ++plan.shadowed;
        }
    }
    return plan;
}

std::vector<std::uint8_t> encode_pgm(const MaskPlan& plan) {
    const std::string header = "P5\n" + std::to_string(plan.width) + " "
        + std::to_string(plan.height) + "\n2\n";
    std::vector<std::uint8_t> bytes(header.begin(), header.end());
    for (const PixelClass value : plan.classes) bytes.push_back(static_cast<std::uint8_t>(value));
    return bytes;
}

} // namespace eawr::lighting_probe
