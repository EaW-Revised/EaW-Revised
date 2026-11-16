// Tactical super weapon power-up camera contracts (docs/behaviour/space-tactical-superweapon.md, TSW-16).
//
// Exercises include/eawr/presentation/camera/tsw_camera.hpp through its public API. The geometry is test data; the
// shot's numbers are the unverified least-visible choices the header names, checked here against independent arithmetic.

#include "../../support/check.hpp"
#include "eawr/presentation/camera/tsw_camera.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
namespace camera = eawr::presentation::camera;
using Vec3 = std::array<float, 3>;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAILED: ");
}

void expect_close(const float actual, const float expected, const std::string_view message, const float tolerance = 1e-2F) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::cerr << "FAILED: " << message << " (expected " << expected << ", got " << actual << ")\n";
        ++failures;
    }
}

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }

// A sphere of `radius` around `center` whose dish is on its surface along `axis` (axis a unit vector in these tests).
camera::TswCameraInput sphere(const Vec3& center, const Vec3& axis, const float radius) {
    camera::TswCameraInput input;
    input.dish = {center[0] + axis[0] * radius, center[1] + axis[1] * radius, center[2] + axis[2] * radius};
    input.dish_axis = axis;
    input.radius = radius;
    return input;
}

camera::TswCameraPose pose_at(const camera::TswCameraInput& input, const std::uint32_t k) {
    const auto pose = camera::tsw_camera(input, k);
    expect(pose.has_value(), "a valid input gives a pose");
    return pose ? pose.value() : camera::TswCameraPose{};
}

void test_the_shot_looks_at_the_dish() {
    const auto input = sphere({1000.0F, -500.0F, 20.0F}, {1.0F, 0.0F, 0.0F}, 800.0F);
    const auto pose = pose_at(input, 0);
    // The dish is on the forward axis at the model's surface.
    expect_close(pose.target[0], 1800.0F, "the camera looks at the dish centre, x");
    expect_close(pose.target[1], -500.0F, "the camera looks at the dish centre, y");
    expect_close(pose.target[2], 20.0F, "the camera looks at the dish centre, z");
    // 1.4 radii from the dish.
    expect_close(length(sub(pose.eye, pose.target)), 1.4F * 800.0F, "the camera stands 1.4 radii from the dish", 0.5F);
    // 45 degrees off the dish axis: the offset's component along the axis equals its sideways part.
    const Vec3 offset = sub(pose.eye, pose.target);
    expect_close(offset[0], 1.4F * 800.0F * std::cos(0.78539816F), "the camera is in front of the dish by cos 45 degrees", 0.5F);
    expect_close(std::abs(offset[1]), 1.4F * 800.0F * std::sin(0.78539816F), "and to the side by sin 45 degrees", 0.5F);
    expect_close(offset[2], 0.0F, "the camera stays level with the dish");
}

void test_the_dish_reads_on_the_left() {
    // For any horizontal heading the sphere's centre must lie to the right of the view direction and the dish on its
    // left half: right = forward x up, and the model centre is behind the dish along the axis.
    for (int heading = 0; heading < 360; heading += 30) {
        const float radians = static_cast<float>(heading) * 0.017453292519943295F;
        const Vec3 axis{std::cos(radians), std::sin(radians), 0.0F};
        const Vec3 center{-300.0F, 700.0F, 0.0F};
        const auto input = sphere(center, axis, 500.0F);
        const auto pose = pose_at(input, 100);
        Vec3 forward = sub(pose.target, pose.eye);
        const float l = length(forward);
        forward = {forward[0] / l, forward[1] / l, forward[2] / l};
        const Vec3 right = cross(forward, {0.0F, 0.0F, 1.0F});
        const float centre_side = dot(sub(center, pose.target), right);
        expect(centre_side > 0.0F, "the sphere's centre is to the right of the dish in the picture");
    }
}

void test_the_shot_is_held() {
    const auto input = sphere({0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 300.0F);
    const auto first = pose_at(input, 0);
    for (const std::uint32_t k : {1U, 30U, 270U, 345U, 569U}) expect(pose_at(input, k) == first, "every frame of the cinematic is the same shot");
}

void test_a_tilted_axis_keeps_the_distance() {
    // The object faces up at a target above it (TSW-05 gives it the full 3D facing).
    const Vec3 axis{0.6F, 0.0F, 0.8F};
    const auto input = sphere({0.0F, 0.0F, 0.0F}, axis, 400.0F);
    const auto pose = pose_at(input, 0);
    expect_close(length(sub(pose.eye, pose.target)), 1.4F * 400.0F, "a tilted axis keeps the standoff", 0.5F);
    expect_close(length(sub(pose.target, {0.0F, 0.0F, 0.0F})), 400.0F, "the dish stays where it was put", 0.5F);
    expect(std::isfinite(pose.eye[0]) && std::isfinite(pose.eye[1]) && std::isfinite(pose.eye[2]), "the pose is finite");
}

void test_a_vertical_axis_has_a_side() {
    for (const float z : {1.0F, -1.0F}) {
        const auto pose = pose_at(sphere({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, z}, 100.0F), 0);
        expect_close(length(sub(pose.eye, pose.target)), 140.0F, "a vertical axis still gives the standoff", 0.5F);
        expect(std::isfinite(pose.eye[0]) && std::isfinite(pose.eye[1]) && std::isfinite(pose.eye[2]), "a vertical axis gives a finite pose");
    }
}

void test_an_unnormalised_axis_is_normalised() {
    const auto a = pose_at(sphere({10.0F, 20.0F, 30.0F}, {1.0F, 0.0F, 0.0F}, 250.0F), 5);
    auto longer = sphere({10.0F, 20.0F, 30.0F}, {1.0F, 0.0F, 0.0F}, 250.0F);
    longer.dish_axis = {7.0F, 0.0F, 0.0F}; // the same dish, an axis that is not a unit vector
    const auto b = pose_at(longer, 5);
    for (std::size_t i = 0; i < 3; ++i) {
        expect_close(a.eye[i], b.eye[i], "the axis is normalised, eye", 1e-3F);
        expect_close(a.target[i], b.target[i], "the axis is normalised, target", 1e-3F);
    }
}

void test_clip_planes() {
    const auto pose = pose_at(sphere({}, {1.0F, 0.0F, 0.0F}, 100.0F), 0);
    expect_close(pose.near_clip, 1.0F, "the near plane is the other cinematic camera's");
    expect_close(pose.far_clip, 10000.0F, "the far plane is the other cinematic camera's");
}

void test_rejections() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const auto good = sphere({0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 100.0F);
    expect(camera::tsw_camera(good, 0).has_value(), "the base input is valid");
    auto bad = good;
    bad.dish[1] = nan;
    expect(!camera::tsw_camera(bad, 0).has_value(), "a non-finite dish is refused");
    bad = good;
    bad.dish_axis[2] = inf;
    expect(!camera::tsw_camera(bad, 0).has_value(), "a non-finite axis is refused");
    bad = good;
    bad.dish_axis = {0.0F, 0.0F, 0.0F};
    expect(!camera::tsw_camera(bad, 0).has_value(), "a zero-length axis is refused");
    for (const float radius : {0.0F, -5.0F, nan, inf}) {
        bad = good;
        bad.radius = radius;
        expect(!camera::tsw_camera(bad, 0).has_value(), "a radius that is not positive and finite is refused");
    }
    expect(!camera::tsw_camera(good, camera::tsw_rules::duration_frames).has_value(), "the frame past the end is refused");
    const auto refused = camera::tsw_camera(good, 100000);
    expect(!refused.has_value(), "a late frame is refused");
    if (!refused.has_value()) expect(refused.error().code == camera::diagnostic_codes::invalid_tsw_camera_input, "the refusal carries the camera code");
}

void test_the_timer() {
    expect(camera::tsw_cinematic_running(0), "the cinematic runs in its first frame");
    expect(camera::tsw_cinematic_running(569), "the cinematic runs to frame 569");
    expect(!camera::tsw_cinematic_running(570), "the cinematic is over at frame 570");
    // TSW-C4: the two played runs measured 571 and 568 frames from the command.
    expect(camera::tsw_rules::duration_frames >= 568 && camera::tsw_rules::duration_frames <= 571, "the length is the played runs'");
}

} // namespace

void test_the_dish_lands_on_the_hull() {
    // A fire bone floating a tenth of a radius above the hull on the line from the centre: the dish is on the hull.
    const Vec3 origin{100.0F, -200.0F, 50.0F};
    const Vec3 bone{100.0F + 880.0F, -200.0F, 50.0F};
    const Vec3 dish = camera::tsw_dish_on_hull(origin, bone, 800.0F);
    expect_close(dish[0], 900.0F, "the dish is one radius from the centre along the bone line, x");
    expect_close(dish[1], -200.0F, "the dish keeps the bone line, y");
    expect_close(dish[2], 50.0F, "the dish keeps the bone line, z");
    // An oblique bone: the point keeps the direction and takes the radius.
    const Vec3 oblique = camera::tsw_dish_on_hull(origin, {100.0F + 30.0F, -200.0F + 40.0F, 50.0F + 120.0F}, 650.0F);
    expect_close(length(sub(oblique, origin)), 650.0F, "an oblique bone lands one radius from the centre");
    expect_close(dot(sub(oblique, origin), {30.0F, 40.0F, 120.0F}), 650.0F * length({30.0F, 40.0F, 120.0F}),
        "an oblique bone keeps its direction", 1.0F);
    // A bone inside the hull is pushed out to it.
    expect_close(length(sub(camera::tsw_dish_on_hull(origin, {101.0F, -200.0F, 50.0F}, 800.0F), origin)), 800.0F, "an inner bone lands on the hull");
    // No direction, a bad radius or a non-finite input keeps the bone point.
    expect(camera::tsw_dish_on_hull(origin, origin, 800.0F) == origin, "a bone on the centre keeps its position");
    expect(camera::tsw_dish_on_hull(origin, bone, 0.0F) == bone, "a zero radius keeps the bone position");
    expect(camera::tsw_dish_on_hull(origin, bone, -5.0F) == bone, "a negative radius keeps the bone position");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    expect(camera::tsw_dish_on_hull({nan, 0.0F, 0.0F}, bone, 800.0F) == bone, "a non-finite centre keeps the bone position");
}

int main() {
    test_the_dish_lands_on_the_hull();
    test_the_shot_looks_at_the_dish();
    test_the_dish_reads_on_the_left();
    test_the_shot_is_held();
    test_a_tilted_axis_keeps_the_distance();
    test_a_vertical_axis_has_a_side();
    test_an_unnormalised_axis_is_normalised();
    test_clip_planes();
    test_rejections();
    test_the_timer();
    if (failures != 0) {
        std::cerr << failures << " tsw camera contract failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "tsw camera contracts passed\n";
    return EXIT_SUCCESS;
}
