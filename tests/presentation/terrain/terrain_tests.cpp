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

#include "terrain_test_support.hpp"

namespace eawr_terrain_test {

int failures{};

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// A wholly original synthetic heightfield.  Nothing here is copied from an
// installed map; the values exist so each geometric claim has a case that would
// fail if the claim stopped holding.
eawr::assets::Map synthetic_map(
    const std::uint32_t width, const std::uint32_t height,
    const std::size_t material_count, const bool sloped) {
    eawr::assets::Terrain terrain;
    terrain.width = width;
    terrain.height = height;
    terrain.cell_count = width * height;
    terrain.slot_count = static_cast<std::uint32_t>(material_count);
    terrain.samples.reserve(static_cast<std::size_t>(width) * height);
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t column = 0; column < width; ++column) {
            eawr::assets::TerrainSample sample;
            // A pure ramp along source X, so the expected normal is a closed
            // form rather than something read back out of the builder.
            sample.height_sample = sloped ? static_cast<std::int16_t>(column * 8) : std::int16_t{0};
            sample.material_slot = material_count == 0
                ? std::uint8_t{0}
                : static_cast<std::uint8_t>((column + row) % material_count);
            sample.vertex_intensity = static_cast<std::uint8_t>((column * 7 + row * 11) % 256);
            terrain.samples.push_back(sample);
        }
    }
    for (std::size_t index = 0; index < material_count; ++index) {
        eawr::assets::TerrainMaterial material;
        material.primary_texture = "synthetic_primary_" + std::to_string(index) + ".tga";
        // Only the second slot declares a secondary texture, so the dual and
        // single effect selections are both exercised by one map.
        if (index == 1) material.secondary_texture = "synthetic_secondary.tga";
        terrain.materials.push_back(std::move(material));
    }
    eawr::assets::Map map;
    map.source = {"data/art/maps/synthetic.ted", "synthetic", "test",
                  eawr::vfs::AssetOrigin::loose, 0};
    map.format_version = 0x0201;
    map.kind = eawr::assets::MapKind::land;
    map.semantic_complete = true;
    map.terrain = std::move(terrain);
    return map;
}

eawr::assets::Vec3f cross(const eawr::assets::Vec3f left, const eawr::assets::Vec3f right) {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

eawr::assets::Vec3f difference(const eawr::assets::Vec3f left, const eawr::assets::Vec3f right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

bool near_value(const float left, const float right, const float tolerance) {
    return std::abs(left - right) <= tolerance;
}

std::vector<std::byte> f32(const float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return {std::byte{static_cast<std::uint8_t>(bits)},
            std::byte{static_cast<std::uint8_t>(bits >> 8)},
            std::byte{static_cast<std::uint8_t>(bits >> 16)},
            std::byte{static_cast<std::uint8_t>(bits >> 24)}};
}

void mini(std::vector<std::byte>& stream, const std::uint8_t id, const std::vector<std::byte>& bytes) {
    stream.push_back(std::byte{id});
    stream.push_back(std::byte{static_cast<std::uint8_t>(bytes.size())});
    stream.insert(stream.end(), bytes.begin(), bytes.end());
}

std::vector<std::byte> vec3(const float x, const float y, const float z) {
    auto bytes = f32(x);
    const auto y_bytes = f32(y);
    const auto z_bytes = f32(z);
    bytes.insert(bytes.end(), y_bytes.begin(), y_bytes.end());
    bytes.insert(bytes.end(), z_bytes.begin(), z_bytes.end());
    return bytes;
}

} // namespace eawr_terrain_test

int main() {
    using namespace eawr_terrain_test;

    run_geometry_tests();
    run_descriptor_tests();
    run_blend_tests();

    if (failures == 0) std::cout << "terrain tests passed\n";
    return failures == 0 ? 0 : 1;
}
