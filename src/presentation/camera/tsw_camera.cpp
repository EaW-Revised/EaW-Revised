#include "eawr/presentation/camera/tsw_camera.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace eawr::presentation::camera {
namespace {

using Vec3 = std::array<float, 3>;

constexpr float degrees_to_radians = 0.017453292519943295F;
constexpr float degenerate_length = 1e-4F;

[[nodiscard]] core::Diagnostic failure(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::invalid_tsw_camera_input);
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] bool finite(const Vec3& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}
[[nodiscard]] Vec3 add(const Vec3& a, const Vec3& b) noexcept { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
[[nodiscard]] Vec3 scaled(const Vec3& a, const float s) noexcept { return {a[0] * s, a[1] * s, a[2] * s}; }
[[nodiscard]] Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] float length(const Vec3& a) noexcept { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

} // namespace

core::Result<TswCameraPose> tsw_camera(const TswCameraInput& input, const std::uint32_t k) {
    using Result = core::Result<TswCameraPose>;
    if (!finite(input.dish) || !finite(input.dish_axis) || !std::isfinite(input.radius) || !(input.radius > 0.0F))
        return Result::failure(failure("the shot needs a finite dish and axis and a positive radius"));
    const float axis_length = length(input.dish_axis);
    if (!(axis_length > degenerate_length)) return Result::failure(failure("the dish axis has no length"));
    if (!tsw_cinematic_running(k)) return Result::failure(failure("the cinematic frame is past its end"));
    const Vec3 axis = scaled(input.dish_axis, 1.0F / axis_length);

    // The side the camera stands on: up (+Z) crossed with the axis, so the dish sits on the left of the picture and
    // the sphere runs off to the right. A vertical axis has no horizontal side; +Y stands in for up there.
    Vec3 side = cross({0.0F, 0.0F, 1.0F}, axis);
    if (!(length(side) > degenerate_length)) side = cross({0.0F, 1.0F, 0.0F}, axis);
    side = scaled(side, 1.0F / length(side));

    const float angle = tsw_rules::view_angle_degrees * degrees_to_radians;
    const Vec3& dish = input.dish;
    const Vec3 offset = add(scaled(axis, std::cos(angle)), scaled(side, std::sin(angle)));

    TswCameraPose pose;
    pose.target = dish;
    pose.eye = add(dish, scaled(offset, tsw_rules::view_distance_radii * input.radius));
    return Result::success(pose);
}

std::array<float, 3> tsw_dish_on_hull(const std::array<float, 3>& origin, const std::array<float, 3>& bone, const float radius) noexcept {
    if (!finite(origin) || !finite(bone) || !std::isfinite(radius) || !(radius > 0.0F)) return bone;
    const Vec3 toward{bone[0] - origin[0], bone[1] - origin[1], bone[2] - origin[2]};
    const float distance = length(toward);
    if (!(distance > 0.0F)) return bone;
    return add(origin, scaled(toward, radius / distance));
}

} // namespace eawr::presentation::camera
