#pragma once

// Private support for the retail sun contract runner (sun_retail_tests.cpp), which
// keeps the helper definitions; the cases are grouped into
// sun_retail_{geometry,routing}_tests.cpp.

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

namespace eawr_sun_retail_test {

namespace space = eawr::presentation::space;
using eawr::assets::Vec3f;
using space::SunRetailPlacement;
using space::SunRetailStatus;
using space::SunVec3;
using Rigid12 = std::array<float, 12>;

extern int failures;

void expect(const bool condition, const std::string& message);

constexpr Rigid12 identity12{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

Rigid12 translation(const float x, const float y, const float z);

eawr::assets::Bone record(const std::string& name, const std::int32_t parent, const std::uint32_t billboard,
                          const Rigid12& transform);

bool near3(const SunVec3& a, const SunVec3& b, const double tolerance = 1.0e-3);

std::string show(const SunVec3& value);

void expect_near3(const SunVec3& actual, const SunVec3& expected, const std::string& label,
                  const double tolerance = 1.0e-3);

SunVec3 minus(const SunVec3& a, const SunVec3& b);
SunVec3 cross3(const SunVec3& a, const SunVec3& b);
double dot3(const SunVec3& a, const SunVec3& b);
double norm3(const SunVec3& a);
SunVec3 unit3(const SunVec3& a);
SunVec3 widen(const Vec3f& value);

// The RT-01 fixture: an identity root and a mode-7 child whose own translation
// is `own`, under the camera eye (0, -10, 0) -> (0, 0, 0), up +Z, and L = +Y.
struct RetailCase final {
    std::vector<eawr::assets::Bone> bones;
    std::int32_t mesh_bone{1};
    Vec3f eye{0.0F, -10.0F, 0.0F};
    Vec3f target{0.0F, 0.0F, 0.0F};
    Vec3f up{0.0F, 0.0F, 1.0F};
    Vec3f toward_light{0.0F, 1.0F, 0.0F};

    explicit RetailCase(const Vec3f own = {1000.0F, 0.0F, 0.0F}) {
        bones.push_back(record("Root", -1, 0, identity12));
        bones.push_back(record("Sun", 0, 7, translation(own.x, own.y, own.z)));
    }
    [[nodiscard]] space::SunRetailInput input() const {
        space::SunRetailInput value;
        value.bones = bones;
        value.mesh_bone = mesh_bone;
        value.eye = eye;
        value.target = target;
        value.up = up;
        value.toward_light = toward_light;
        return value;
    }
    [[nodiscard]] SunRetailPlacement place() const { return space::sun_retail_placement(input()); }
};

bool all_zero(const SunRetailPlacement& placement);

bool same(const SunRetailPlacement& a, const SunRetailPlacement& b);

void expect_status(const RetailCase& sun, const SunRetailStatus status, const std::string& label);

void expect_vertex(const RetailCase& sun, const Vec3f& local, const SunVec3& expected, const std::string& label,
                   const double tolerance = 1.0e-3);

void expect_frame(const SunRetailPlacement& placement, const SunVec3& x, const SunVec3& n, const SunVec3& z,
                  const std::string& label);

std::optional<SunVec3> reference_vertex(const RetailCase& sun, const Vec3f o, const Vec3f local);

RetailCase general_case();

// sun_retail_geometry_tests.cpp
void test_identity();
void test_retail_oracles();
void test_winding_and_frame_samples();
void test_camera_translation();
void test_camera_rotation();
void test_roll_from_camera_up();
void test_chain_facts();
void test_stored_scale();

// sun_retail_routing_tests.cpp
void test_degenerate_guards();
void test_precedence();
void test_statelessness();
void test_admission_closed();

} // namespace eawr_sun_retail_test
