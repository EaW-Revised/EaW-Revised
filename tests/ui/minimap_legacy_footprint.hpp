#pragma once

// Frozen skirmish footprint oracle from fe336eef, before the shared-helper extraction.
// Test-only: keep this independent of the replacement so a refactor cannot change
// the old camera rectangle unnoticed at standard or ultrawide viewport sizes.
#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/ui/selection.hpp"

#include <array>
#include <cmath>
#include <optional>

namespace eawr::test::skirmish_footprint_before {
using eawr::presentation::FixedCamera;
namespace ui = eawr::presentation::ui;
using Vec3 = std::array<float, 3>;

[[nodiscard]] inline Vec3 subtract(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
[[nodiscard]] inline float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
[[nodiscard]] inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] inline std::optional<Vec3> normalized(const Vec3& value) {
    const float length = std::sqrt(dot(value, value));
    if (!std::isfinite(length) || length < 1.0e-6F) return std::nullopt;
    return Vec3{value[0] / length, value[1] / length, value[2] / length};
}

// The render basis is source (x, z, -y).
[[nodiscard]] inline Vec3 source_from_render(const Vec3& render) { return {render[0], -render[2], render[1]}; }

// The camera's view basis: forward, right and up, render basis.
struct ViewBasis final {
    Vec3 forward{};
    Vec3 right{};
    Vec3 up{};
    float tan_half{};
};

[[nodiscard]] inline std::optional<ViewBasis> basis(const FixedCamera& frame) {
    const auto forward = normalized(subtract(frame.target, frame.eye));
    if (!forward) return std::nullopt;
    const auto right = normalized(cross(*forward, frame.up));
    if (!right) return std::nullopt;
    constexpr float degrees = 3.14159265358979323846F / 180.0F;
    const float tan_half = std::tan(frame.vertical_fov_degrees * 0.5F * degrees);
    if (!std::isfinite(tan_half) || tan_half <= 0.0F) return std::nullopt;
    return ViewBasis{*forward, *right, cross(*right, *forward), tan_half};
}


class Footprint final {
public:
    explicit Footprint(const FixedCamera& frame) : frame_(&frame),
        viewport_{static_cast<float>(frame.width), static_cast<float>(frame.height)} {}
std::optional<ui::PickRay> ray(const float x, const float y) const {
    if (!frame_ || viewport_[0] <= 0.0F || viewport_[1] <= 0.0F) return std::nullopt;
    const auto view = basis(*frame_);
    if (!view) return std::nullopt;
    const float aspect = viewport_[0] / viewport_[1];
    const float ndc_x = 2.0F * x / viewport_[0] - 1.0F;
    const float ndc_y = 1.0F - 2.0F * y / viewport_[1];
    Vec3 direction{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        direction[axis] = view->forward[axis] + view->right[axis] * ndc_x * view->tan_half * aspect
            + view->up[axis] * ndc_y * view->tan_half;
    }
    return ui::PickRay{source_from_render(frame_->eye), source_from_render(direction)};
}

std::optional<std::array<std::array<double, 2>, 4>> ground_corners(const double height) const {
    if (!frame_ || viewport_[0] <= 0.0F || viewport_[1] <= 0.0F) return std::nullopt;
    const std::array<std::array<float, 2>, 4> corners{
        {{0.0F, 0.0F}, {viewport_[0], 0.0F}, {viewport_[0], viewport_[1]}, {0.0F, viewport_[1]}}};
    std::array<std::array<double, 2>, 4> out{};
    std::array<std::array<double, 3>, 4> near_points{}, far_points{};
    std::array<double, 4> depths{};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        const auto corner_ray = ray(corners[index][0], corners[index][1]);
        if (!corner_ray) return std::nullopt;
        const auto& origin = corner_ray->origin;
        const auto& direction = corner_ray->direction;
        // ray() has forward component one, so this parameter is camera depth.
        depths[index] = std::abs(direction[2]) > 1.0e-9F
            ? (height - origin[2]) / direction[2] : -1.0;
        out[index] = {origin[0] + depths[index] * direction[0], origin[1] + depths[index] * direction[1]};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            near_points[index][axis] = origin[axis] + frame_->near_plane * direction[axis];
            far_points[index][axis] = origin[axis] + frame_->far_plane * direction[axis];
        }
    }
    const auto plane_cut = [height](const auto& lower, const auto& upper) {
        const double dz = upper[2] - lower[2];
        if (std::abs(dz) < 1.0e-9) return std::array<double, 2>{};
        const double fraction = (height - lower[2]) / dz;
        return std::array<double, 2>{lower[0] + fraction * (upper[0] - lower[0]),
                                     lower[1] + fraction * (upper[1] - lower[1])};
    };
    // MM-09: an upper ray outside the frustum meets the far face's vertical
    // edge instead. A lower ray behind the near face replaces both edges.
    if (depths[0] < frame_->near_plane || depths[1] < frame_->near_plane
        || depths[0] > frame_->far_plane || depths[1] > frame_->far_plane) {
        out[0] = plane_cut(far_points[3], far_points[0]);
        out[1] = plane_cut(far_points[2], far_points[1]);
    }
    if (depths[2] < frame_->near_plane || depths[3] < frame_->near_plane) {
        out[0] = plane_cut(far_points[3], far_points[0]);
        out[1] = plane_cut(far_points[2], far_points[1]);
        out[2] = plane_cut(near_points[2], near_points[1]);
        out[3] = plane_cut(near_points[3], near_points[0]);
    }
    return out;
}

private:
    const FixedCamera* frame_;
    std::array<float, 2> viewport_;
};
} // namespace eawr::test::skirmish_footprint_before
