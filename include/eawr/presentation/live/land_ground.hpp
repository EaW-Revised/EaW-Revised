#pragma once

#include "eawr/presentation/ui/selection.hpp"
#include "eawr/sim/tactical/land_terrain.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace eawr::presentation::live {

// Intersect the L1 bilinear height field, visiting grid cells along the ray.
// Each cell has a quadratic height along a ray, so shallow rays and ridges
// do not depend on a fixed sampling step or on the space battle plane.
[[nodiscard]] inline std::optional<ui::Vec3f> land_ground_point(
    const sim::tactical::LandTerrain& terrain, const ui::PickRay& ray) noexcept {
    using sim::math::Fixed;
    const auto real = [](Fixed v) { return static_cast<double>(v.raw()) / Fixed::scale; };
    const double spacing = real(terrain.spacing());
    double begin = 0.0, end = std::numeric_limits<double>::infinity();
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const double limit = spacing * ((axis == 0 ? terrain.width() : terrain.height()) - 1);
        const double origin = ray.origin[axis], direction = ray.direction[axis];
        if (!std::isfinite(origin) || !std::isfinite(direction)) return std::nullopt;
        if (std::abs(direction) < 1.0e-12) {
            if (origin < 0 || origin > limit) return std::nullopt;
        } else {
            const double a = -origin / direction, b = (limit - origin) / direction;
            begin = std::max(begin, std::min(a, b)); end = std::min(end, std::max(a, b));
        }
    }
    if (!std::isfinite(ray.origin[2]) || !std::isfinite(ray.direction[2]) || end < begin) return std::nullopt;
    const auto point = [&](double t) {
        return ui::Vec3f{static_cast<float>(ray.origin[0] + t * ray.direction[0]),
            static_cast<float>(ray.origin[1] + t * ray.direction[1]),
            static_cast<float>(ray.origin[2] + t * ray.direction[2])};
    };
    if (!std::isfinite(end)) {
        if (std::abs(ray.direction[2]) < 1.0e-12) return std::nullopt;
        const auto height = terrain.height_at(Fixed::from_raw(static_cast<std::int64_t>(ray.origin[0] * Fixed::scale)),
            Fixed::from_raw(static_cast<std::int64_t>(ray.origin[1] * Fixed::scale)));
        if (!height) return std::nullopt;
        const double t = (real(*height) - ray.origin[2]) / ray.direction[2];
        return t >= 0 ? std::optional(point(t)) : std::nullopt;
    }
    const auto height = [&](std::uint32_t x, std::uint32_t y) {
        return real(*terrain.height_at(Fixed::from_raw(static_cast<std::int64_t>(x) * terrain.spacing().raw()),
            Fixed::from_raw(static_cast<std::int64_t>(y) * terrain.spacing().raw())));
    };
    // A line crosses at most width + height cells. A vertical grid boundary
    // belongs to either adjacent cell because their shared edge is identical.
    for (std::uint64_t visited = 0; visited < static_cast<std::uint64_t>(terrain.width()) + terrain.height(); ++visited) {
        const double probe = std::min(end, std::nextafter(begin, end));
        const auto cell = [&](std::size_t axis, std::uint32_t size) {
            double value = (ray.origin[axis] + probe * ray.direction[axis]) / spacing;
            if (ray.direction[axis] < 0) value = std::nextafter(value, -std::numeric_limits<double>::infinity());
            return static_cast<std::uint32_t>(std::clamp(std::floor(value), 0.0, static_cast<double>(size - 2)));
        };
        const auto x = cell(0, terrain.width()), y = cell(1, terrain.height());
        double next = end;
        for (std::size_t axis = 0; axis < 2; ++axis) {
            if (std::abs(ray.direction[axis]) < 1.0e-12) continue;
            const auto index = axis == 0 ? x : y;
            const double edge = spacing * (index + (ray.direction[axis] > 0 ? 1 : 0));
            const double crossing = (edge - ray.origin[axis]) / ray.direction[axis];
            if (crossing > begin) next = std::min(next, crossing);
        }
        const double h00 = height(x, y), h10 = height(x + 1, y), h01 = height(x, y + 1), h11 = height(x + 1, y + 1);
        const auto difference = [&](double u) {
            const double t = begin + (next - begin) * u;
            const double fx = (ray.origin[0] + t * ray.direction[0]) / spacing - x;
            const double fy = (ray.origin[1] + t * ray.direction[1]) / spacing - y;
            return ray.origin[2] + t * ray.direction[2]
                - (h00 + (h10 - h00) * fx + (h01 - h00) * fy + (h11 - h10 - h01 + h00) * fx * fy);
        };
        const double c = difference(0), f1 = difference(1), fm = difference(0.5);
        const double a = 2 * (f1 + c - 2 * fm), b = f1 - c - a;
        std::array<double, 2> roots{2, 2};
        if (std::abs(a) < 1.0e-10) {
            if (std::abs(b) > 1.0e-10) roots[0] = -c / b;
            else if (std::abs(c) < 1.0e-7) roots[0] = 0;
        } else {
            const double discriminant = b * b - 4 * a * c;
            if (discriminant >= 0) {
                roots = {(-b - std::sqrt(discriminant)) / (2 * a), (-b + std::sqrt(discriminant)) / (2 * a)};
                std::sort(roots.begin(), roots.end());
            }
        }
        for (const double root : roots) if (root >= -1.0e-8 && root <= 1.0 + 1.0e-8)
            return point(begin + (next - begin) * std::clamp(root, 0.0, 1.0));
        if (next >= end) break;
        begin = next;
    }
    return std::nullopt;
}

} // namespace eawr::presentation::live
