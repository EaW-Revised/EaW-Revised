// Contracts for the pure half of E-space-environment-v1 (P1 #27): environment
// light 0, the object transform, the render-basis conjugation, the family
// hook, billboards and the default camera. Synthetic inputs only.
#include "eawr/presentation/space/environment_scene.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

namespace {

namespace space = eawr::presentation::space;
namespace assets = eawr::assets;

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] bool near(const float a, const float b, const float tolerance = 1.0e-4F) {
    return std::abs(a - b) <= tolerance;
}

[[nodiscard]] bool near(const assets::Vec3f& a, const assets::Vec3f& b, const float tolerance = 1.0e-4F) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

[[nodiscard]] assets::RawField field(const std::uint32_t id, std::initializer_list<float> values) {
    assets::RawField result;
    result.id = id;
    for (const float value : values) {
        std::uint32_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        for (int byte = 0; byte < 4; ++byte) result.bytes.push_back(static_cast<std::byte>((raw >> (8 * byte)) & 0xFFU));
    }
    return result;
}

void test_environment_light() {
    assets::EnvironmentDescriptor environment;
    environment.fields = {field(0x00, {1.0F, 0.5F, 0.25F}), field(0x03, {0.2F, 0.4F, 0.6F}),
                          field(0x04, {0.1F, 0.1F, 0.1F}), field(0x05, {0.5F}), field(0x08, {0.0F}),
                          field(0x0b, {0.0F})};
    auto light = space::environment_light(environment);
    expect(light.has_value(), "a complete light-0 record decodes");
    // R-LIT-01: heading 0, elevation 0 points toward source -Y.
    expect(light && near(light->toward_light, {0.0F, -1.0F, 0.0F}), "heading 0 lies toward -Y");
    expect(light && near(light->diffuse, {0.5F, 0.25F, 0.125F}), "diffuse is colour times intensity");
    expect(light && near(light->specular, {0.2F, 0.4F, 0.6F}), "specular is colour times 2 times intensity");
    // Heading increases toward +X; the last complete occurrence wins.
    environment.fields.push_back(field(0x08, {1.5707964F}));
    environment.fields.push_back(field(0x0b, {0.0F}));
    environment.fields.push_back(assets::RawField{0x0b, 0, {std::byte{0}}});
    light = space::environment_light(environment);
    expect(light && near(light->toward_light, {1.0F, 0.0F, 0.0F}), "heading pi/2 lies toward +X; a short mini is ignored");
    environment.fields.push_back(field(0x0b, {1.5707964F}));
    light = space::environment_light(environment);
    expect(light && near(light->toward_light, {0.0F, 0.0F, 1.0F}), "elevation pi/2 points up");
    assets::EnvironmentDescriptor missing;
    missing.fields = {field(0x00, {1.0F, 1.0F, 1.0F})};
    expect(!space::environment_light(missing), "a record without the light fields fails closed");
    assets::EnvironmentDescriptor overflow;
    overflow.fields = {field(0x00, {1.0F, 1.0F, 1.0F}), field(0x03, {1.0F, 1.0F, 1.0F}), field(0x04, {0.1F, 0.1F, 0.1F}),
                       field(0x05, {2.0e38F}), field(0x08, {0.0F}), field(0x0b, {0.0F})};
    expect(!space::environment_light(overflow), "finite fields whose scaled light overflows fail closed");
}

void test_transforms() {
    // R-ROT-01 with a zero triple is the fixed quarter turn: +X -> +Y.
    const space::Affine turn = space::object_transform({10.0F, 20.0F, 30.0F}, {0.0F, 0.0F, 0.0F}, 2.0F);
    expect(near(space::transform_point(turn, {1.0F, 0.0F, 0.0F}), {10.0F, 22.0F, 30.0F}),
           "the object rule turns model +X to +Y, scales and translates");
    // Yaw 90 then the quarter turn: +X -> -X.
    const space::Affine yawed = space::object_transform({}, {0.0F, 0.0F, 90.0F}, 1.0F);
    expect(near(space::transform_direction(yawed, {1.0F, 0.0F, 0.0F}), {-1.0F, 0.0F, 0.0F}), "yaw composes after the turn");
    // Roll about world X after the turn: model +X (now +Y) goes to +Z.
    const space::Affine rolled = space::object_transform({}, {90.0F, 0.0F, 0.0F}, 1.0F);
    expect(near(space::transform_direction(rolled, {1.0F, 0.0F, 0.0F}), {0.0F, 0.0F, 1.0F}), "roll is about world X");
    // The render conjugation agrees with converting points one by one.
    const space::Affine source = space::object_transform({5.0F, -7.0F, 3.0F}, {10.0F, 329.0F, 254.0F}, 3.0F);
    const space::Affine render = space::render_affine(source);
    const assets::Vec3f point{1.5F, -2.0F, 0.75F};
    expect(near(space::transform_point(render, space::render_from_source(point)),
                space::render_from_source(space::transform_point(source, point)), 1.0e-3F),
           "render_affine is the basis change of the source transform");
}

[[nodiscard]] assets::Model quad_model(const std::string& shader, const std::uint32_t billboard) {
    assets::Model model;
    assets::Bone root;
    root.relative_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    assets::Bone bone = root;
    bone.parent = 0;
    bone.billboard = billboard;
    bone.relative_transform[3] = 100.0F;
    model.bones = {root, bone};
    assets::Mesh mesh;
    mesh.name = "quad";
    mesh.bone = 1;
    assets::Submesh submesh;
    submesh.shader = shader;
    for (const auto& [x, z] : {std::pair{-1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, 1.0F}, {-1.0F, 1.0F}}) {
        assets::Vertex vertex;
        vertex.position = {x, 0.0F, z};
        vertex.normal = {0.0F, -1.0F, 0.0F};
        submesh.vertices.push_back(vertex);
    }
    submesh.indices = {0, 1, 2, 0, 2, 3};
    submesh.parameters.push_back({"BaseTexture", assets::ParameterKind::texture, std::string("glow.tga")});
    submesh.parameters.push_back({"Color", assets::ParameterKind::vector4, assets::Vec4f{1.0F, 1.0F, 1.0F, 1.0F}});
    mesh.submeshes.push_back(submesh);
    model.meshes.push_back(mesh);
    return model;
}

void test_surfaces_and_billboards() {
    const assets::Model plain = quad_model("MeshAdditive.fx", 0);
    auto surfaces = space::scene_surfaces(plain);
    expect(surfaces.size() == 1 && surfaces[0].problem.empty() && surfaces[0].route == space::SceneRoute::meshadditive,
           "a plain MeshAdditive surface is drawable");
    expect(!surfaces.empty() && near(surfaces[0].model.meshes[0].submeshes[0].vertices[0].position, {99.0F, 0.0F, -1.0F}),
           "the rigid chain is baked into a plain surface");
    expect(space::environment_family(plain) == space::EnvironmentFamily::none, "a MeshAdditive model is no family");
    expect(space::environment_family(quad_model("Nebula.fx", 0)) == space::EnvironmentFamily::nebula, "Nebula.fx is a nebula");
    expect(space::environment_family(quad_model("planet.FX", 0)) == space::EnvironmentFamily::planet,
           "Planet.fx is a planet, case-insensitively");
    expect(space::scene_surfaces(quad_model("MeshShadowVolume.fx", 0))[0].route == space::SceneRoute::unsupported,
             "an unrouted shader is listed as unsupported");
    const auto companion = space::scene_surfaces(quad_model("MeshAlpha.fx", 0));
    expect(companion.size() == 1 && companion[0].problem.empty()
            && companion[0].route == space::SceneRoute::legacy_mesh,
        "a planet's alpha ring remains in the environment model with its ordinary scene selector");

    const assets::Model glow = quad_model("MeshAdditive.fx", 6);
    surfaces = space::scene_surfaces(glow);
    expect(surfaces.size() == 1 && surfaces[0].billboard == 6 && near(surfaces[0].bone_origin, {0.0F, 0.0F, 0.0F}),
           "a billboard keeps its local quad and its parent's origin");
    const space::Affine object = space::object_transform({0.0F, 1000.0F, 0.0F}, {}, 10.0F);
    const auto facing = space::facing_billboard(surfaces[0], object, 10.0F, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, -100.0F);
    expect(facing.problem.empty(), "the glow is placed");
    if (facing.problem.empty()) {
        const auto& vertices = facing.model.meshes[0].submeshes[0].vertices;
        // Pushed 100 behind its anchor with the size grown by 1100 / 1000.
        expect(near(vertices[0].position.y, 1100.0F, 1.0e-2F), "the glow sits behind the anchor");
        expect(near(std::abs(vertices[0].position.x), 11.0F, 1.0e-3F), "the glow keeps its angular size");
    }
    const auto inside = space::facing_billboard(surfaces[0], object, 10.0F, {0.0F, 999.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, 5.0F);
    expect(!inside.problem.empty(), "an eye inside the clearance is refused");
    for (const std::uint32_t mode : {1U, 2U, 3U}) {
        const auto admitted = space::scene_surfaces(quad_model("MeshAdditive.fx", mode));
        expect(admitted.size() == 1 && admitted[0].problem.empty() && admitted[0].billboard == mode,
               "camera billboard surface is admitted for the renderer's live bone pose");
    }
    for (const std::uint32_t mode : {4U, 5U}) {
        const auto deferred = space::scene_surfaces(quad_model("MeshAdditive.fx", mode));
        expect(deferred.size() == 1 && !deferred[0].problem.empty(),
               "light and wind billboards await their external direction");
    }
}

void test_sunlight_glow_offset() {
    const auto side = space::sunlight_glow_offset({1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 0.656168F, 2.0F);
    expect(side && near(*side, {1.312336F, 0.0F, 0.0F}),
           "mode 6 shifts toward the projected light by authored bone distance and face scale");
    const auto diagonal = space::sunlight_glow_offset({1.0F, -1.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 2.0F, 1.0F);
    expect(diagonal && near(*diagonal, {1.41421356F, 0.0F, 0.0F}),
           "mode 6 discards the depth component without renormalizing its lateral light component");
    const auto head_on = space::sunlight_glow_offset({0.0F, -1.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 2.0F, 1.0F);
    expect(head_on && near(*head_on, {0.0F, 0.0F, 0.0F}), "head-on light causes no lateral glow shift");
    const auto off_axis = space::sunlight_glow_offset({1.0F, 0.0F, 0.0F}, {1.0F, -1.0F, 0.0F}, 2.0F, 1.0F);
    expect(off_axis && near(*off_axis, {1.0F, 1.0F, 0.0F}),
           "an orbit changes the camera-facing plane and the glow's lit-side mask");
    expect(!space::sunlight_glow_offset({}, {0.0F, -1.0F, 0.0F}, 2.0F, 1.0F),
           "a missing light direction fails closed");
}

void test_default_camera() {
    assets::Map map;
    assets::SourceVolume volume;
    volume.minimum = {-1000.0F, -1000.0F, -500.0F};
    volume.maximum = {1000.0F, 1000.0F, 500.0F};
    map.volumes.push_back(volume);
    space::TacticalDefaults tactical;
    auto camera = space::default_space_camera(map, tactical, 1280, 720);
    expect(camera.target_kind == "volume_centre", "without markers the target is the volume centre");
    expect(space::validate_camera(camera.camera) == space::CameraStatus::valid, "the default camera is valid");

    const auto marker = [](const std::string& type, const std::uint32_t record, const assets::Vec3f position) {
        assets::Placement placement;
        placement.key.record_ordinal = record;
        placement.type_resolution = assets::TypeResolution::unique;
        placement.type_candidates.push_back({type, {}, {}, {}, {}});
        placement.position = position;
        return placement;
    };
    map.placements.push_back(marker("Team_00_Spawn_Point_Marker", 0, {0.0F, -800.0F, 0.0F}));
    map.placements.push_back(marker("Team_01_Base_Position_Marker", 5, {0.0F, 800.0F, 0.0F}));
    map.placements.push_back(marker("Team_01_Spawn_Point_Marker", 3, {-800.0F, 0.0F, 0.0F}));
    map.placements.push_back(marker("Player_Spawn_Point_Marker", 1, {5.0F, 5.0F, 0.0F}));
    camera = space::default_space_camera(map, tactical, 1280, 720);
    expect(camera.target_kind == "start_marker" && camera.target_record == 3u,
           "the target is local player 1's first spawn marker");
    // The FoC Space_Mode default yaw controls the eye independently of the map centre.
    const auto& c = camera.camera;
    expect(near(c.target[0], -800.0F) && near(c.target[2], 0.0F), "the target is the marker, in the render basis");
    const float horizontal = tactical.distance * std::cos(tactical.pitch_degrees * 3.14159265F / 180.0F);
    expect(near(c.eye[0], -800.0F, 1.0e-2F) && near(c.eye[2], horizontal, 1.0e-2F),
           "the eye uses the Space_Mode default yaw");
    expect(near(c.eye[1], tactical.distance * std::sin(tactical.pitch_degrees * 3.14159265F / 180.0F), 1.0e-2F),
           "the eye rises by the pitch");
    expect(c.far_plane == space::environment_far_plane && c.near_plane == tactical.near_plane
               && near(c.vertical_fov_degrees, 2.0F * std::atan(0.75F * std::tan(tactical.fov_degrees * 3.14159265F / 360.0F))
                                                   * 180.0F / 3.14159265F, 1.0e-3F),
           "planes and fov come from the tactical values (#515: the XML angle's 4:3 vertical one) and the "
           "environment far plane");
}

void test_camera_relative_sky() {
    eawr::presentation::FixedCamera camera;
    camera.far_plane = 7000.0F;
    camera.near_plane = 10.0F;
    const auto environment = space::environment_view_camera(camera);
    expect(environment.far_plane > space::environment_sky_radius && environment.near_plane == camera.near_plane,
           "the live Space_Mode camera sees the entire sky shell");

    const assets::Vec3f initial_eye{0.0F, 0.0F, 1000.0F};
    const auto original = space::object_transform(initial_eye, {17.0F, 0.0F, 128.0F}, 12.0F);
    for (const assets::Vec3f eye : {initial_eye, assets::Vec3f{0.0F, 0.0F, 200.0F},
                                   assets::Vec3f{0.0F, 0.0F, 1900.0F},
                                   assets::Vec3f{1900.0F, 0.0F, 0.0F}, assets::Vec3f{-900.0F, 900.0F, 700.0F}}) {
        const auto moved = space::camera_relative_sky_transform(original, initial_eye, eye);
        expect(near(moved[3], eye.x) && near(moved[7], eye.y) && near(moved[11], eye.z),
               "the sky follows default, zoomed in, maximum zoom and rotated camera eyes");
        for (const std::size_t index : {0U, 1U, 2U, 4U, 5U, 6U, 8U, 9U, 10U}) {
            expect(near(moved[index], original[index]), "camera motion preserves sky orientation and scale");
        }
    }
}

void test_config_free_laser_camera() {
    eawr::presentation::FixedCamera fixed;
    fixed.eye = {0.0F, 707.10678F, 2207.10678F};
    fixed.target = {0.0F, 0.0F, 1500.0F};
    fixed.vertical_fov_degrees = 60.0F;
    const auto render = space::environment_view_camera(fixed);
    eawr::presentation::camera::Constants tactical;
    tactical.near_clip = 10.0F;
    tactical.far_clip = 7000.0F;
    const auto laser = space::environment_laser_camera(render, tactical);
    expect(render.far_plane == 60000.0F && laser.near_plane == 10.0F && laser.far_plane == 7000.0F,
           "config-free laser camera uses tactical planes while render keeps the sky range");
    expect(laser.eye == render.eye && laser.target == render.target && laser.up == render.up
               && laser.vertical_fov_degrees == render.vertical_fov_degrees
               && laser.width == render.width && laser.height == render.height,
           "laser clips preserve the fixed drawn pose, FOV and viewport without a bridge");
    tactical.near_clip = 20.0F;
    tactical.far_clip = 9000.0F;
    const auto overridden = space::environment_laser_camera(render, tactical);
    expect(overridden.near_plane == 20.0F && overridden.far_plane == 9000.0F,
           "laser planes follow supplied XML or bridge override constants");
}

} // namespace

int main() {
    test_environment_light();
    test_transforms();
    test_surfaces_and_billboards();
    test_sunlight_glow_offset();
    test_default_camera();
    test_camera_relative_sky();
    test_config_free_laser_camera();
    if (failures != 0) {
        std::cerr << failures << " environment scene contract failure(s)\n";
        return 1;
    }
    std::cout << "environment scene contracts passed\n";
    return 0;
}
