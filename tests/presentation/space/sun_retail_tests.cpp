#include "eawr/presentation/space/space.hpp"
#include "eawr/presentation/space/sun_retail.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sun_retail_test_support.hpp"

// Contracts for eawr-sun-mode7-retail-v1 (WP-16, #27): the retail mode-7 sun
// rule of docs/behaviour/meshadditive-sun-mode7-retail.md RT-01..RT-09, plus
// camera translation, rotation and roll, degenerate guards, chain facts,
// precedence and the closed admission. Every expected value is hard-coded from
// an independent double-precision derivation of the note's formulas (see
// docs/rendering.md#sun-policies); none is computed by the
// evaluator under test. All bones, cameras and lights are invented here.
namespace eawr_sun_retail_test {

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

Rigid12 translation(const float x, const float y, const float z) { return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z}; }

eawr::assets::Bone record(const std::string& name, const std::int32_t parent, const std::uint32_t billboard,
                          const Rigid12& transform) {
    eawr::assets::Bone bone;
    bone.name = name;
    bone.parent = parent;
    bone.billboard = billboard;
    bone.relative_transform = transform;
    return bone;
}

bool near3(const SunVec3& a, const SunVec3& b, const double tolerance) {
    return std::abs(a[0] - b[0]) <= tolerance && std::abs(a[1] - b[1]) <= tolerance
        && std::abs(a[2] - b[2]) <= tolerance;
}

std::string show(const SunVec3& value) {
    return "(" + std::to_string(value[0]) + ", " + std::to_string(value[1]) + ", " + std::to_string(value[2]) + ")";
}

void expect_near3(const SunVec3& actual, const SunVec3& expected, const std::string& label,
                  const double tolerance) {
    expect(near3(actual, expected, tolerance), label + ": expected " + show(expected) + ", got " + show(actual));
}

SunVec3 minus(const SunVec3& a, const SunVec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

SunVec3 cross3(const SunVec3& a, const SunVec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double dot3(const SunVec3& a, const SunVec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

double norm3(const SunVec3& a) { return std::sqrt(dot3(a, a)); }

SunVec3 unit3(const SunVec3& a) {
    const double length = norm3(a);
    return {a[0] / length, a[1] / length, a[2] / length};
}

SunVec3 widen(const Vec3f& value) { return {value.x, value.y, value.z}; }

bool all_zero(const SunRetailPlacement& placement) {
    const SunVec3 zero{};
    return placement.distance == 0.0 && placement.scale == 0.0 && placement.origin == zero
        && placement.right == zero && placement.view_up == zero && placement.backward == zero
        && placement.axes[0] == zero && placement.axes[1] == zero && placement.axes[2] == zero;
}

bool same(const SunRetailPlacement& a, const SunRetailPlacement& b) {
    return a.status == b.status && a.detail == b.detail && a.distance == b.distance && a.scale == b.scale
        && a.origin == b.origin && a.right == b.right && a.view_up == b.view_up && a.backward == b.backward
        && a.axes == b.axes;
}

void expect_status(const RetailCase& sun, const SunRetailStatus status, const std::string& label) {
    const auto placement = sun.place();
    expect(placement.status == status, label + ": expected " + std::string(space::to_string(status)) + ", got "
        + std::string(space::to_string(placement.status)) + " (" + placement.detail + ")");
    if (status == SunRetailStatus::placed) return;
    expect(!placement.detail.empty(), label + ": a failure names its cause");
    expect(all_zero(placement), label + ": a failure carries no geometry");
    expect(!space::sun_retail_vertex(placement, {0.0F, 0.0F, 0.0F}), label + ": a failure places no vertex");
    expect(!space::sun_retail_admissible(placement), label + ": a failure is never admissible");
}

void expect_vertex(const RetailCase& sun, const Vec3f& local, const SunVec3& expected, const std::string& label,
                   const double tolerance) {
    const auto placement = sun.place();
    const auto vertex = space::sun_retail_vertex(placement, local);
    expect(placement.status == SunRetailStatus::placed && vertex && near3(*vertex, expected, tolerance),
           label + ": expected " + show(expected) + ", got "
               + (vertex ? show(*vertex) : std::string(space::to_string(placement.status))));
}

void expect_frame(const SunRetailPlacement& placement, const SunVec3& x, const SunVec3& n, const SunVec3& z,
                  const std::string& label) {
    expect(placement.status == SunRetailStatus::placed, label + " is placed: " + placement.detail);
    expect_near3(placement.axes[0], x, label + " X");
    expect_near3(placement.axes[1], n, label + " n");
    expect_near3(placement.axes[2], z, label + " Z");
}

// The B-02 reference result for the same camera and L with rest origin `o`.
std::optional<SunVec3> reference_vertex(const RetailCase& sun, const Vec3f o, const Vec3f local) {
    const std::vector<eawr::assets::Bone> bones{record("Root", -1, 0, identity12),
                                                record("Sun", 0, 7, translation(o.x, o.y, o.z))};
    space::SunReferenceInput input;
    input.bones = bones;
    input.mesh_bone = 1;
    input.eye = sun.eye;
    input.target = sun.target;
    input.up = sun.up;
    input.toward_sun = sun.toward_light;
    return space::sun_reference_vertex(space::sun_reference_placement(input), local);
}
RetailCase general_case() {
    RetailCase sun;
    sun.bones = {record("Parent", -1, 0, {0, -1, 0, 20, 1, 0, 0, -40, 0, 0, 1, 5}),
                 record("Sun", 0, 7, {1, 0, 0, -300, 0, 0.8660254F, -0.5F, -100, 0, 0.5F, 0.8660254F, 50})};
    sun.eye = {10, 20, 5};
    sun.target = {-30, 70, -15};
    sun.toward_light = {-0.51214755F, 0.76822126F, 0.38411063F};
    return sun;
}
} // namespace eawr_sun_retail_test

int main() {
    using namespace eawr_sun_retail_test;
    test_identity();
    test_retail_oracles();
    test_winding_and_frame_samples();
    test_camera_translation();
    test_camera_rotation();
    test_roll_from_camera_up();
    test_chain_facts();
    test_degenerate_guards();
    test_precedence();
    test_stored_scale();
    test_statelessness();
    test_admission_closed();
    if (failures != 0) {
        std::cerr << failures << " sun retail contract failure(s)\n";
        return 1;
    }
    std::cout << "sun retail contracts passed\n";
    return 0;
}
