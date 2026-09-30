#include "eawr/sim/tactical/combat.hpp"

#include "../math/wide.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <numeric>
#include <string>

// Projectile collision with a type's meshes (#536, docs/behaviour/space-damage.md DG-36): a tree
// of boxes over each mesh's triangles, and an exact segment test in the unit's frame.
namespace eawr::sim::tactical {
namespace {

using math::Fixed;

constexpr std::uint32_t leaf_size = 4;

[[nodiscard]] std::int64_t component(const math::Vec3& point, const std::size_t axis) noexcept {
    return axis == 0 ? point.x.raw() : axis == 1 ? point.y.raw() : point.z.raw();
}

struct Build {
    std::vector<CollisionTriangle> source;
    std::vector<std::uint32_t> order;
    std::vector<CollisionNode> nodes;

    // The box around triangles order[first, first + count).
    void bound(CollisionNode& node, const std::uint32_t first, const std::uint32_t count) const {
        std::array<std::int64_t, 3> low{};
        std::array<std::int64_t, 3> high{};
        low.fill(std::numeric_limits<std::int64_t>::max());
        high.fill(std::numeric_limits<std::int64_t>::min());
        for (std::uint32_t index = first; index < first + count; ++index) {
            const auto& triangle = source[order[index]];
            for (const auto* corner : {&triangle.a, &triangle.b, &triangle.c}) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis], component(*corner, axis));
                    high[axis] = std::max(high[axis], component(*corner, axis));
                }
            }
        }
        node.min = {Fixed::from_raw(low[0]), Fixed::from_raw(low[1]), Fixed::from_raw(low[2])};
        node.max = {Fixed::from_raw(high[0]), Fixed::from_raw(high[1]), Fixed::from_raw(high[2])};
    }

    [[nodiscard]] std::int64_t centre(const std::uint32_t triangle, const std::size_t axis) const noexcept {
        const auto& t = source[triangle];
        return component(t.a, axis) + component(t.b, axis) + component(t.c, axis); // three times the centre
    }

    // Builds the node for order[first, first + count) at `slot`; its children follow it.
    void split(const std::uint32_t slot, const std::uint32_t first, const std::uint32_t count) {
        bound(nodes[slot], first, count);
        if (count <= leaf_size) {
            nodes[slot].first = first;
            nodes[slot].count = count;
            return;
        }
        std::size_t axis = 0;
        std::int64_t longest = -1;
        for (std::size_t candidate = 0; candidate < 3; ++candidate) {
            const auto extent = component(nodes[slot].max, candidate) - component(nodes[slot].min, candidate);
            if (extent > longest) {
                longest = extent;
                axis = candidate;
            }
        }
        const auto begin = order.begin() + first;
        std::stable_sort(begin, begin + count, [&](const std::uint32_t left, const std::uint32_t right) {
            return centre(left, axis) < centre(right, axis);
        });
        const auto half = count / 2;
        const auto left = static_cast<std::uint32_t>(nodes.size());
        nodes.emplace_back();
        split(left, first, half);
        const auto right = static_cast<std::uint32_t>(nodes.size());
        nodes.emplace_back();
        split(right, first + half, count - half);
        nodes[slot].first = left;
        nodes[slot].second = right;
        nodes[slot].count = 0;
    }
};

// The segment test works on 1/32 units in the unit's frame: coordinates below 2^13 units stay
// below 2^18, so every product of the triangle test fits in 64 bits.
constexpr int grid_shift = math::Fixed::fractional_bits - 5;
constexpr std::int64_t max_local = std::int64_t{2 * max_mesh_extent} << 5;

[[nodiscard]] std::int64_t grid(const std::int64_t raw) noexcept {
    constexpr std::int64_t half = std::int64_t{1} << (grid_shift - 1);
    return raw >= 0 ? (raw + half) >> grid_shift : -((-raw + half) >> grid_shift);
}

struct Point {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t z{};
};

[[nodiscard]] Point grid(const math::Vec3& value) noexcept {
    return {grid(value.x.raw()), grid(value.y.raw()), grid(value.z.raw())};
}
[[nodiscard]] Point minus(const Point& a, const Point& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] Point cross(const Point& a, const Point& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] std::int64_t dot(const Point& a, const Point& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] std::int64_t get(const Point& p, const std::size_t axis) noexcept {
    return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
}

// A fraction numerator / denominator with 0 <= numerator and 0 < denominator.
struct Fraction {
    std::int64_t numerator{};
    std::int64_t denominator{1};
};

// Exact: a < b.
[[nodiscard]] bool less(const Fraction& a, const Fraction& b) noexcept {
    namespace wide = math::detail;
    const auto left = wide::multiply_u64(static_cast<std::uint64_t>(a.numerator), static_cast<std::uint64_t>(b.denominator));
    const auto right = wide::multiply_u64(static_cast<std::uint64_t>(b.numerator), static_cast<std::uint64_t>(a.denominator));
    return wide::compare(left, right) < 0;
}

// Whether the segment p + s d, s in [0, 1], meets the box; exact on the grid.
[[nodiscard]] bool meets_box(const Point& p, const Point& d, const Point& low, const Point& high) noexcept {
    // enter and leave as fractions with positive denominators; the numerators may be negative.
    std::int64_t enter_n = 0;
    std::int64_t enter_d = 1;
    std::int64_t leave_n = 1;
    std::int64_t leave_d = 1;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto start = get(p, axis);
        const auto delta = get(d, axis);
        const auto lo = get(low, axis);
        const auto hi = get(high, axis);
        if (delta == 0) {
            if (start < lo || start > hi) return false;
            continue;
        }
        std::int64_t near_n = delta > 0 ? lo - start : start - hi;
        std::int64_t far_n = delta > 0 ? hi - start : start - lo;
        const auto den = delta > 0 ? delta : -delta;
        // enter = max(enter, near), leave = min(leave, far): |numerators| and denominators < 2^20.
        if (near_n * enter_d > enter_n * den) {
            enter_n = near_n;
            enter_d = den;
        }
        if (far_n * leave_d < leave_n * den) {
            leave_n = far_n;
            leave_d = den;
        }
        if (enter_n * leave_d > leave_n * enter_d) return false;
    }
    return true;
}

// The segment's entry into the triangle, both faces; nothing when it misses or runs parallel.
[[nodiscard]] std::optional<Fraction> meets_triangle(const Point& p, const Point& d, const Point& a, const Point& b,
    const Point& c) noexcept {
    const auto e1 = minus(b, a);
    const auto e2 = minus(c, a);
    const auto pvec = cross(d, e2);
    auto det = dot(e1, pvec);
    if (det == 0) return std::nullopt;
    const auto s = minus(p, a);
    auto u = dot(s, pvec);
    const auto qvec = cross(s, e1);
    auto v = dot(d, qvec);
    auto t = dot(e2, qvec);
    if (det < 0) {
        det = -det;
        u = -u;
        v = -v;
        t = -t;
    }
    if (u < 0 || v < 0 || u > det - v || t < 0 || t > det) return std::nullopt;
    return Fraction{t, det};
}

// floor(fraction x 2^24) for 0 <= numerator <= denominator < 2^62.
[[nodiscard]] Fixed to_fixed(const Fraction& fraction) noexcept {
    if (fraction.numerator >= fraction.denominator) return Fixed::from_raw(Fixed::scale);
    std::uint64_t remainder = static_cast<std::uint64_t>(fraction.numerator);
    const auto denominator = static_cast<std::uint64_t>(fraction.denominator);
    std::int64_t result = 0;
    for (int bit = 0; bit < Fixed::fractional_bits; ++bit) {
        remainder <<= 1;
        result <<= 1;
        if (remainder >= denominator) {
            remainder -= denominator;
            result |= 1;
        }
    }
    return Fixed::from_raw(result);
}

} // namespace

CollisionMesh collision_mesh(std::vector<CollisionTriangle> triangles, const std::uint32_t hardpoint,
    const std::uint32_t source_hardpoint, const bool shield) {
    CollisionMesh mesh;
    mesh.hardpoint = hardpoint;
    mesh.source_hardpoint = source_hardpoint;
    mesh.shield = shield;
    if (triangles.empty()) return mesh;
    Build build;
    build.source = std::move(triangles);
    build.order.resize(build.source.size());
    std::iota(build.order.begin(), build.order.end(), 0U);
    build.nodes.emplace_back();
    build.split(0, 0, static_cast<std::uint32_t>(build.source.size()));
    mesh.nodes = std::move(build.nodes);
    mesh.triangles.reserve(build.order.size());
    for (const auto index : build.order) mesh.triangles.push_back(build.source[index]);
    return mesh;
}

core::Result<std::optional<MeshHit>> segment_hits_meshes(std::span<const CollisionMesh> meshes,
    const std::function<bool(std::size_t)>& enabled, const math::Mat3x4& transform, const math::Vec3& from,
    const math::Vec3& to) {
    using Out = core::Result<std::optional<MeshHit>>;
    const auto& m = transform.rows;
    const math::Vec3 origin{m[0][3], m[1][3], m[2][3]};
    std::optional<core::Diagnostic> error;
    // Model-space coordinates: the rotation's columns are the model axes in the world.
    const auto local = [&](const math::Vec3& point) {
        const auto sub = [&](const Fixed a, const Fixed b) {
            auto value = math::subtract(a, b);
            if (!value) {
                if (!error) error = value.error();
                return Fixed{};
            }
            return value.value();
        };
        const math::Vec3 delta{sub(point.x, origin.x), sub(point.y, origin.y), sub(point.z, origin.z)};
        std::array<Fixed, 3> result{};
        for (std::size_t column = 0; column < 3; ++column) {
            auto value = math::dot(math::Vec3{m[0][column], m[1][column], m[2][column]}, delta);
            if (!value) {
                if (!error) error = value.error();
            } else {
                result[column] = value.value();
            }
        }
        return math::Vec3{result[0], result[1], result[2]};
    };
    const auto start = grid(local(from));
    const auto end = grid(local(to));
    if (error) return Out::failure(*error);
    for (const auto value : {start.x, start.y, start.z, end.x, end.y, end.z}) {
        // Beyond twice the mesh extent the segment cannot meet a mesh the table validated.
        if (value > max_local || value < -max_local) return Out::success(std::nullopt);
    }
    const auto delta = minus(end, start);
    std::optional<Fraction> best;
    std::size_t best_mesh = 0;
    std::vector<std::uint32_t> stack;
    for (std::size_t index = 0; index < meshes.size(); ++index) {
        const auto& mesh = meshes[index];
        if (mesh.nodes.empty() || !enabled(index)) continue;
        stack.assign(1, 0);
        while (!stack.empty()) {
            const auto& node = mesh.nodes[stack.back()];
            stack.pop_back();
            if (!meets_box(start, delta, grid(node.min), grid(node.max))) continue;
            if (node.count == 0) {
                stack.push_back(node.second);
                stack.push_back(node.first);
                continue;
            }
            for (std::uint32_t t = node.first; t < node.first + node.count; ++t) {
                const auto& triangle = mesh.triangles[t];
                const auto hit = meets_triangle(start, delta, grid(triangle.a), grid(triangle.b), grid(triangle.c));
                // The first along the segment; on a tie the earlier mesh, then the triangle met first.
                if (hit && (!best || less(*hit, *best))) {
                    best = hit;
                    best_mesh = index;
                }
            }
        }
    }
    if (!best) return Out::success(std::nullopt);
    return Out::success(MeshHit{to_fixed(*best), best_mesh});
}

} // namespace eawr::sim::tactical
