#include "eawr/presentation/space/space.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "space_test_support.hpp"

// Pure contracts for E-space-primary-sky-v1. Every value is invented here: a
// synthetic space map, a two-surface sky model with asymmetric placement and
// its own texture per surface. Nothing is copied from an installed asset.
namespace eawr_space_test {

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

eawr::assets::Source source(const std::string& path) {
    return {path, "synthetic", "test", eawr::vfs::AssetOrigin::loose, 0};
}

eawr::assets::Map space_map() {
    eawr::assets::Map map;
    map.source = source("data/art/maps/eawr_space_synthetic.ted");
    map.kind = eawr::assets::MapKind::space;
    map.semantic_complete = true;
    eawr::assets::EnvironmentDescriptor environment;
    environment.name = "EAWR_SPACE_ENVIRONMENT";
    environment.primary_sky = "EAWR_SPACE_PRIMARY_SKY";
    environment.secondary_sky = "EAWR_SPACE_SECONDARY_SKY";
    environment.cloud_texture = "eawr_space_cloud.tga";
    map.environments.push_back(environment);
    return map;
}

// A 2x2-cell quad (3x3 vertices) whose corners are given in the source basis.
// UV (0,0) sits at `origin`, u grows along `u_axis`, v along `v_axis`.
eawr::assets::Submesh quad(const Vec3f origin, const Vec3f u_axis, const Vec3f v_axis,
                           const std::string& shader, const std::string& texture) {
    eawr::assets::Submesh submesh;
    submesh.shader = shader;
    submesh.vertex_format = "alD3dVertNU2";
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const float u = static_cast<float>(column) * 0.5F;
            const float v = static_cast<float>(row) * 0.5F;
            eawr::assets::Vertex vertex;
            vertex.position = {origin.x + u_axis.x * u + v_axis.x * v,
                               origin.y + u_axis.y * u + v_axis.y * v,
                               origin.z + u_axis.z * u + v_axis.z * v};
            vertex.texcoord[0] = {u, v};
            submesh.vertices.push_back(vertex);
        }
    }
    for (std::uint16_t row = 0; row < 2; ++row) {
        for (std::uint16_t column = 0; column < 2; ++column) {
            const auto corner = static_cast<std::uint16_t>(row * 3 + column);
            submesh.indices.insert(submesh.indices.end(),
                {corner, static_cast<std::uint16_t>(corner + 1), static_cast<std::uint16_t>(corner + 4),
                 corner, static_cast<std::uint16_t>(corner + 4), static_cast<std::uint16_t>(corner + 3)});
        }
    }
    if (!texture.empty()) {
        submesh.parameters.push_back({"BaseTexture", eawr::assets::ParameterKind::texture, texture});
    }
    return submesh;
}

// Surface A lies on the source +X side, surface B on the source +Y (forward)
// side; each is a 200x200 wall 400 units from the origin.
eawr::assets::Model sky_model() {
    eawr::assets::Model model;
    model.source = source("data/art/models/eawr_space_sky.alo");
    eawr::assets::Bone root;
    root.name = "Root";
    root.relative_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    model.bones.push_back(root);
    eawr::assets::Mesh a;
    a.name = "SkyA";
    a.bone = 0;
    a.submeshes.push_back(quad({400, -100, 100}, {0, 200, 0}, {0, 0, -200}, qualified_shader, "eawr_space_a.tga"));
    eawr::assets::Mesh b;
    b.name = "SkyB";
    b.bone = 0;
    b.submeshes.push_back(quad({100, 400, 100}, {-200, 0, 0}, {0, 0, -200}, qualified_shader, "eawr_space_b.dds"));
    model.meshes.push_back(a);
    model.meshes.push_back(b);
    return model;
}

eawr::assets::Texture rgba_texture(const std::string& path, const std::uint8_t red) {
    eawr::assets::Texture texture;
    texture.source = source(path);
    texture.width = 2;
    texture.height = 2;
    texture.format = eawr::assets::PixelFormat::rgba8;
    eawr::assets::MipLevel mip;
    mip.width = 2;
    mip.height = 2;
    mip.row_pitch = 8;
    mip.bytes.resize(16);
    for (int index = 0; index < 4; ++index) {
        const std::size_t offset = static_cast<std::size_t>(index) * 4;
        mip.bytes[offset] = std::byte{red};
        mip.bytes[offset + 1] = std::byte{static_cast<unsigned char>(index)};
        mip.bytes[offset + 2] = std::byte{0};
        mip.bytes[offset + 3] = std::byte{255};
    }
    texture.mips.push_back(mip);
    return texture;
}
} // namespace eawr_space_test

int main(const int argc, const char* const argv[]) {
    using namespace eawr_space_test;
    if (argc == 2) {
        const std::string_view request(argv[1]);
        if (request == "--dump-planet-opaque") {
            std::cout << space::environment_effect_shader(space::planet_route_id, false);
            return 0;
        }
        if (request == "--dump-planet-blended") {
            std::cout << space::environment_effect_shader(space::planet_route_id, true);
            return 0;
        }
        if (request == "--dump-nebula") {
            std::cout << space::environment_effect_shader(space::nebula_route_id, false);
            return 0;
        }
    }
    test_environment_selection();
    test_model_selection();
    test_two_surface_plan();
    test_surface_rejections();
    test_texture_normalisation();
    test_camera();
    test_projection_and_basis();
    test_morphology();
    test_pixel_evaluation();
    test_meshgloss_route();
    test_meshgloss_arithmetic();
    test_meshadditive_route();
    test_meshadditive_arithmetic();
    test_sun_reference_geometry();
    test_environment_effect_routes();
    test_environment_effect_clock();
    if (failures != 0) {
        std::cerr << failures << " space contract failure(s)\n";
        return 1;
    }
    std::cout << "space contracts passed\n";
    return 0;
}
