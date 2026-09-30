#pragma once

// Private support for the terrain contract runner; definitions live in
// terrain_tests.cpp and each test group in its own terrain_*_tests.cpp.

#include "eawr/presentation/terrain/terrain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace eawr_terrain_test {

extern int failures;

void expect(bool condition, const char* message);
eawr::assets::Map synthetic_map(
    std::uint32_t width, std::uint32_t height,
    std::size_t material_count = 2, bool sloped = false);
eawr::assets::Vec3f cross(eawr::assets::Vec3f left, eawr::assets::Vec3f right);
eawr::assets::Vec3f difference(eawr::assets::Vec3f left, eawr::assets::Vec3f right);
bool near_value(float left, float right, float tolerance = 1e-4F);
std::vector<std::byte> f32(float value);
void mini(std::vector<std::byte>& stream, std::uint8_t id, const std::vector<std::byte>& bytes);
std::vector<std::byte> vec3(float x, float y, float z);

void run_geometry_tests();
void run_descriptor_tests();
void run_blend_tests();

} // namespace eawr_terrain_test
