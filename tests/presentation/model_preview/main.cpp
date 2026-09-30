// Viewer fixed-scene selection and exploratory Hull preview framing contracts.
//
// Exercises apps/viewer/src/model_preview.{hpp,cpp}, which is engine
// independent, without Godot. Models are synthetic: mesh/bone names and the
// Hull bounds mirror the pinned MC-50 structure, but no retail bytes are used.

#include "model_preview.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace preview = eawr::viewer::model_preview;
namespace assets = eawr::assets;

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void expect_close(const float actual, const float expected, const std::string_view message,
                  const float tolerance = 1e-3F) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::cerr << "FAILED: " << message << " (expected " << expected << ", got " << actual
                  << ")\n";
        ++failures;
    }
}

bool contains(const std::string& text, const std::string_view fragment) {
    return text.find(fragment) != std::string::npos;
}

template <typename T>
void expect_failure(const preview::Checked<T>& result, const std::string_view fragment,
                    const std::string_view message) {
    if (result) {
        std::cerr << "FAILED: " << message << " (unexpectedly succeeded)\n";
        ++failures;
    } else if (!contains(result.failure, fragment)) {
        std::cerr << "FAILED: " << message << " (reason was: " << result.failure << ")\n";
        ++failures;
    }
}

// Real MC-50 Hull header/vertex bounds (asset space, Z up, bow at -Y).
constexpr assets::Vec3f hull_min{-54.3327F, -138.071F, -21.5623F};
constexpr assets::Vec3f hull_max{54.3327F, 148.461F, 30.7542F};

std::array<float, 12> identity_bone() {
    return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
}

assets::Submesh submesh(std::string shader, std::string base_texture,
                        const assets::Vec3f low, const assets::Vec3f high) {
    assets::Submesh result;
    result.shader = std::move(shader);
    if (!base_texture.empty()) {
        result.parameters.push_back(
            {"BaseTexture", assets::ParameterKind::texture, std::move(base_texture)});
    }
    result.parameters.push_back({"Shininess", assets::ParameterKind::scalar, 16.0F});
    // Box corners plus a centre vertex; the bounds are exactly [low, high].
    for (int index = 0; index < 8; ++index) {
        assets::Vertex vertex;
        vertex.position = {(index & 1) != 0 ? high.x : low.x, (index & 2) != 0 ? high.y : low.y,
            (index & 4) != 0 ? high.z : low.z};
        result.vertices.push_back(vertex);
    }
    assets::Vertex centre;
    centre.position = {(low.x + high.x) * 0.5F, (low.y + high.y) * 0.5F, (low.z + high.z) * 0.5F};
    result.vertices.push_back(centre);
    result.indices = {0, 1, 2, 2, 1, 3};
    return result;
}

assets::Mesh mesh(std::string name, const std::int32_t bone, assets::Submesh part,
                  const assets::Vec3f low, const assets::Vec3f high) {
    assets::Mesh result;
    result.name = std::move(name);
    result.bone = bone;
    result.bounds_min = low;
    result.bounds_max = high;
    result.submeshes.push_back(std::move(part));
    return result;
}

// Hull, a shadow volume and the Hangar in MC-50 file order, each rigidly
// attached to its own bone under Root. The Hull bone carries the same
// sub-1e-6 rotation noise as the real asset.
assets::Model mc50_like() {
    assets::Model model;
    model.bones.push_back({"Root", -1, true, 0, identity_bone()});
    model.bones.push_back({"Hull", 0, true, 0,
        {1.0F, -6.36e-08F, 1.71e-14F, 0.0F, 6.36e-08F, 1.0F, -2.66e-07F, 0.0F, -1.22e-16F,
            2.66e-07F, 1.0F, 0.0F}});
    model.bones.push_back({"Shadow", 0, true, 0, identity_bone()});
    model.bones.push_back({"Hangar", 0, true, 0, identity_bone()});
    model.meshes.push_back(mesh("Hull", 1,
        submesh("MeshBumpColorize.fx", "Rebel_Mon_Calamari_Tide.dds", hull_min, hull_max),
        hull_min, hull_max));
    model.meshes.push_back(mesh("Shadow", 2,
        submesh("MeshShadowVolume.fx", "", hull_min, hull_max), hull_min, hull_max));
    const assets::Vec3f hangar_min{-15.5532F, 81.8341F, -4.83392F};
    const assets::Vec3f hangar_max{15.5532F, 127.482F, -0.6995F};
    model.meshes.push_back(mesh("Hangar", 3,
        submesh("MeshGloss.fx", "hangar_3.dds", hangar_min, hangar_max), hangar_min, hangar_max));
    return model;
}

preview::Plan plan(const std::string_view mesh_name, const std::string_view texture = {},
                   const bool animation = false,
                   const std::string_view model = preview::pinned_model_path) {
    auto planned = preview::plan_for({model, mesh_name, texture, animation});
    if (!planned) {
        std::cerr << "FAILED: plan for mesh '" << mesh_name << "': " << planned.failure << '\n';
        ++failures;
        return {};
    }
    return *planned.value;
}

std::array<float, 16> identity_instance() {
    return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F};
}

void test_plans() {
    const preview::Plan frozen = plan("");
    expect(frozen.kind == preview::Kind::frozen_hangar, "default MC-50 run is the frozen Hangar");
    expect(frozen.required_mesh == "Hangar" && frozen.required_program == "MeshGloss.fx",
        "frozen Hangar keeps the Hangar MeshGloss draw condition");
    expect(frozen.texture_path == "data/art/textures/hangar_3.dds",
        "frozen Hangar keeps the pinned Hangar texture path");
    expect(frozen.expected_model_sha256 == preview::pinned_model_sha256
            && frozen.expected_texture_sha256 == preview::hangar_texture_sha256,
        "frozen Hangar keeps both SHA-256 pins");
    expect(frozen.camera == preview::CameraPolicy::frozen_fixed && !frozen.exploratory,
        "frozen Hangar keeps the fixed camera and is not exploratory");
    expect(frozen.required_base_texture.empty(), "frozen Hangar does not add a BaseTexture gate");

    const preview::Plan explicit_hangar = plan("Hangar");
    expect(explicit_hangar.kind == preview::Kind::explicit_hangar, "explicit Hangar is named");
    expect(explicit_hangar.required_mesh == frozen.required_mesh
            && explicit_hangar.required_program == frozen.required_program
            && explicit_hangar.texture_path == frozen.texture_path
            && explicit_hangar.expected_model_sha256 == frozen.expected_model_sha256
            && explicit_hangar.expected_texture_sha256 == frozen.expected_texture_sha256
            && explicit_hangar.camera == frozen.camera && !explicit_hangar.exploratory,
        "explicit Hangar draws exactly the frozen Hangar");

    const preview::Plan overridden = plan("", "data/art/textures/other.dds");
    expect(overridden.texture_path == "data/art/textures/other.dds"
            && overridden.expected_texture_sha256 == preview::hangar_texture_sha256,
        "a Hangar texture override stays under the Hangar texture pin");

    const preview::Plan any_case = plan("", {}, false, "Data/Art/Models/Rebel_Mon_Calamari_MC_50.ALO");
    expect(any_case.kind == preview::Kind::frozen_hangar, "the model pin matches case-insensitively");

    const preview::Plan hull = plan("Hull");
    expect(hull.kind == preview::Kind::exploratory_hull && hull.exploratory,
        "--eawr-mesh Hull is the exploratory preview");
    expect(hull.required_mesh == "Hull" && hull.required_program == "MeshBumpColorize.fx",
        "Hull selects its own MeshBumpColorize.fx material");
    expect(hull.required_base_texture == "Rebel_Mon_Calamari_Tide.dds" && hull.texture_path.empty(),
        "Hull derives its texture from the required BaseTexture");
    expect(hull.expected_model_sha256 == preview::pinned_model_sha256,
        "Hull keeps the MC-50 model pin");
    expect(hull.expected_texture_sha256 == preview::hull_texture_sha256
            && hull.expected_texture_sha256 != preview::hangar_texture_sha256,
        "Hull carries its own texture pin");
    expect(hull.camera == preview::CameraPolicy::hull_bounds_fit, "Hull uses the bounds-fit camera");

    expect_failure(preview::plan_for({preview::pinned_model_path, "Hull", "data/art/textures/x.dds"}),
        "--eawr-texture is not accepted", "Hull rejects a texture override");
    expect_failure(preview::plan_for({preview::pinned_model_path, "Hull", {}, true}),
        "--eawr-animation is not accepted", "Hull rejects an animation");
    expect_failure(preview::plan_for({preview::pinned_model_path, "Engine_Exhaust", {}}),
        "requested mesh: Engine_Exhaust", "other MC-50 meshes fail closed");
    expect_failure(preview::plan_for({preview::pinned_model_path, "hull", {}}),
        "requested mesh: hull", "mesh names stay exact-case like the frozen rule");

    const preview::Plan unpinned = plan("Body", "data/art/textures/body.dds", true,
        "data/art/models/other.alo");
    expect(unpinned.kind == preview::Kind::unpinned_model && !unpinned.exploratory,
        "other models keep the unpinned path");
    expect(unpinned.expected_model_sha256.empty() && unpinned.expected_texture_sha256.empty(),
        "unpinned models carry no hash pins");
    expect(unpinned.required_mesh == "Body" && unpinned.required_program.empty()
            && unpinned.texture_path == "data/art/textures/body.dds"
            && unpinned.camera == preview::CameraPolicy::legacy_mesh_bounds,
        "unpinned models keep mesh, texture override and header-bounds camera");
}

void test_selection() {
    const assets::Model model = mc50_like();
    const auto frozen = preview::select_submesh(model, plan(""));
    expect(frozen && frozen.value->mesh_index == 2 && frozen.value->submesh_index == 0,
        "frozen Hangar selects the Hangar mesh, not the earlier Hull");
    expect(frozen && frozen.value->material.technique == "sph_t0"
            && frozen.value->material.pass == "sph_t0_p0",
        "frozen Hangar keeps the MeshGloss sph_t0 selector");
    const auto explicit_hangar = preview::select_submesh(model, plan("Hangar"));
    expect(explicit_hangar && explicit_hangar.value->mesh_index == 2,
        "explicit Hangar selects the same mesh");

    const auto hull = preview::select_submesh(model, plan("Hull"));
    expect(hull && hull.value->mesh_index == 0, "Hull selects the Hull mesh");
    expect(hull && hull.value->material.technique == "t0" && hull.value->material.pass == "t0_p0",
        "Hull uses the MeshBumpColorize t0/t0_p0 selector");
    const auto hull_texture = preview::texture_path_for(
        plan("Hull"), model.meshes[0].submeshes[0]);
    expect(hull_texture && *hull_texture.value == "data/art/textures/Rebel_Mon_Calamari_Tide.dds",
        "Hull texture comes from its BaseTexture");
    const auto hangar_texture = preview::texture_path_for(plan(""), model.meshes[2].submeshes[0]);
    expect(hangar_texture && *hangar_texture.value == "data/art/textures/hangar_3.dds",
        "frozen Hangar texture stays pinned by path");

    const auto unpinned = preview::select_submesh(model, plan("", {}, false, "data/art/models/x.alo"));
    expect(unpinned && unpinned.value->mesh_index == 0,
        "unpinned models keep the first-drawable rule");

    assets::Model missing = model;
    missing.meshes.erase(missing.meshes.begin());
    expect_failure(preview::select_submesh(missing, plan("Hull")), "requested mesh Hull is missing",
        "missing Hull fails closed");
    assets::Model no_hangar = model;
    no_hangar.meshes.pop_back();
    expect_failure(preview::select_submesh(no_hangar, plan("")),
        "selected model/mesh has no implemented drawable material",
        "frozen path keeps its failure when the Hangar is absent");
    assets::Model hangar_not_gloss = model;
    hangar_not_gloss.meshes[2].submeshes[0].shader = "MeshBumpColorize.fx";
    expect_failure(preview::select_submesh(hangar_not_gloss, plan("")),
        "selected model/mesh has no implemented drawable material",
        "frozen path still requires MeshGloss on the Hangar");

    assets::Model wrong_program = model;
    wrong_program.meshes[0].submeshes[0].shader = "MeshGloss.fx";
    expect_failure(preview::select_submesh(wrong_program, plan("Hull")),
        "has no MeshBumpColorize.fx submesh", "Hull with another material fails closed");

    assets::Model unknown_program = model;
    unknown_program.meshes[0].submeshes[0].shader = "MeshShield.fx";
    expect_failure(preview::select_submesh(unknown_program, plan("Hull")),
        "has no MeshBumpColorize.fx submesh", "Hull with an unimplemented material fails closed");

    assets::Model hidden = model;
    hidden.meshes[0].visible = false;
    expect_failure(preview::select_submesh(hidden, plan("Hull")), "is hidden",
        "hidden Hull fails closed");

    assets::Model wrong_texture = model;
    std::get<std::string>(wrong_texture.meshes[0].submeshes[0].parameters[0].value) =
        "Rebel_Mon_Calamari_Tide_B.dds";
    expect_failure(preview::select_submesh(wrong_texture, plan("Hull")),
        "expected Rebel_Mon_Calamari_Tide.dds", "Hull with another BaseTexture fails closed");

    assets::Model no_texture = model;
    no_texture.meshes[0].submeshes[0].parameters.erase(
        no_texture.meshes[0].submeshes[0].parameters.begin());
    expect_failure(preview::select_submesh(no_texture, plan("Hull")), "has no BaseTexture",
        "Hull without a BaseTexture fails closed");
    expect_failure(preview::texture_path_for(plan("", {}, false, "data/art/models/x.alo"),
                       no_texture.meshes[0].submeshes[0]),
        "no BaseTexture path", "unpinned texture derivation still fails without BaseTexture");

    expect(preview::hash_matches("abc", ""), "an empty pin is unpinned");
    expect(preview::hash_matches(preview::hull_texture_sha256, preview::hull_texture_sha256),
        "an equal hash matches");
    expect(!preview::hash_matches(preview::hangar_texture_sha256, preview::hull_texture_sha256),
        "the Hangar texture does not satisfy the Hull pin");
    expect(!preview::hash_matches("", preview::pinned_model_sha256), "a missing hash fails a pin");
}

void test_rest_placement() {
    const assets::Model model = mc50_like();
    const auto rest = preview::rest_placement(model, 0, 0);
    expect(static_cast<bool>(rest), "Hull rest placement is trusted: " + rest.failure);
    if (rest) {
        expect(rest.value->chain.size() == 2 && rest.value->chain[0] == "Root"
                && rest.value->chain[1] == "Hull",
            "Hull chain is Root -> Hull");
        expect_close(rest.value->asset.min[1], hull_min.y, "Hull bounds follow its vertices");
        expect_close(rest.value->asset.max[2], hull_max.z, "Hull bounds follow its vertices");
        expect(rest.value->max_placement_delta < 1e-3F, "Hull rest noise is far below tolerance");
        expect(rest.value->tolerance > rest.value->max_placement_delta, "tolerance is recorded");
        expect(rest.value->vertex_count == 9, "vertex count is recorded");
    }

    // Rigid vertices are stored in their bone's space, and the renderer draws
    // them at the bone's rest transform, so the framed bounds follow the bone.
    assets::Model rotated = model;
    rotated.bones[1].relative_transform = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, -1.0F, 0.0F, 0.0F};
    const auto turned = preview::rest_placement(rotated, 0, 0);
    expect(static_cast<bool>(turned), "a rotated Hull bone places the Hull: " + turned.failure);
    if (turned) {
        expect_close(turned.value->asset.min[1], hull_min.z, "rotated bone: stored z becomes y");
        expect_close(turned.value->asset.max[2], -hull_min.y, "rotated bone: stored y becomes -z");
        expect(turned.value->max_placement_delta > 100.0F, "rotated bone: the move is recorded");
    }
    assets::Model translated_parent = model;
    translated_parent.bones[0].relative_transform[7] = 50.0F;
    const auto shifted = preview::rest_placement(translated_parent, 0, 0);
    expect(static_cast<bool>(shifted), "a translated ancestor places the Hull: " + shifted.failure);
    if (shifted) {
        expect_close(shifted.value->asset.min[1], hull_min.y + 50.0F, "translated ancestor moves the bounds");
        expect_close(shifted.value->max_placement_delta, 50.0F, "translated ancestor: the move is recorded");
    }

    assets::Model cyclic = model;
    cyclic.bones[0].parent = 1;
    expect_failure(preview::rest_placement(cyclic, 0, 0), "parent-first and acyclic",
        "a forward parent reference fails closed");
    assets::Model non_finite_bone = model;
    non_finite_bone.bones[1].relative_transform[3] = std::numeric_limits<float>::quiet_NaN();
    expect_failure(preview::rest_placement(non_finite_bone, 0, 0), "non-finite: Hull",
        "a non-finite bone transform fails closed");
    assets::Model missing_bone = model;
    missing_bone.meshes[0].bone = 9;
    expect_failure(preview::rest_placement(missing_bone, 0, 0), "missing bone",
        "an out-of-range mesh bone fails closed");
    assets::Model outside_header = model;
    outside_header.meshes[0].bounds_max.y = 100.0F;
    expect_failure(preview::rest_placement(outside_header, 0, 0), "outside the mesh header bounds",
        "vertices outside the header bounds fail closed");
    assets::Model nan_vertex = model;
    nan_vertex.meshes[0].submeshes[0].vertices[4].position.x =
        std::numeric_limits<float>::infinity();
    expect_failure(preview::rest_placement(nan_vertex, 0, 0), "non-finite vertex",
        "a non-finite vertex fails closed");
    assets::Model empty = model;
    empty.meshes[0].submeshes[0].vertices.clear();
    expect_failure(preview::rest_placement(empty, 0, 0), "no vertices", "an empty Hull fails closed");
    assets::Model degenerate = model;
    for (assets::Vertex& vertex : degenerate.meshes[0].submeshes[0].vertices) {
        vertex.position = {1.0F, 2.0F, 3.0F};
    }
    expect_failure(preview::rest_placement(degenerate, 0, 0), "degenerate",
        "a zero-size Hull fails closed");
    expect_failure(preview::rest_placement(model, 7, 0), "out of range", "bad selection fails");

    // A palette-skinned submesh is stored and drawn in bind space, so a bone
    // rest transform does not move it.
    assets::Model skinned = rotated;
    skinned.meshes[0].submeshes[0].skin_bones = {1};
    const auto bind_space = preview::rest_placement(skinned, 0, 0);
    expect(bind_space && std::abs(bind_space.value->asset.min[1] - hull_min.y) < 1e-3F
            && bind_space.value->max_placement_delta == 0.0F,
        "palette-skinned vertices are not re-placed by the mesh bone");
}

void test_world_bounds() {
    const preview::Bounds asset{{-1.0F, -2.0F, -3.0F}, {4.0F, 5.0F, 6.0F}};
    const preview::Bounds world = preview::world_bounds(asset, identity_instance());
    // Renderer axis convention (x, z, -y).
    expect_close(world.min[0], -1.0F, "x stays x");
    expect_close(world.max[0], 4.0F, "x stays x");
    expect_close(world.min[1], -3.0F, "asset z becomes render y");
    expect_close(world.max[1], 6.0F, "asset z becomes render y");
    expect_close(world.min[2], -5.0F, "asset y becomes render -z");
    expect_close(world.max[2], 2.0F, "asset y becomes render -z");

    std::array<float, 16> moved = identity_instance();
    moved[12] = 100.0F;
    moved[14] = -40.0F;
    const preview::Bounds shifted = preview::world_bounds(asset, moved);
    expect_close(shifted.min[0], 99.0F, "instance translation moves x");
    expect_close(shifted.max[2], -38.0F, "instance translation moves z");

    // A quarter turn about render Y swaps the horizontal extents.
    std::array<float, 16> turned = identity_instance();
    turned[0] = 0.0F;
    turned[2] = -1.0F;
    turned[8] = 1.0F;
    turned[10] = 0.0F;
    const preview::Bounds rotated = preview::world_bounds(asset, turned);
    expect_close(rotated.max[0] - rotated.min[0], 7.0F, "instance rotation is applied to corners");
    expect_close(rotated.max[2] - rotated.min[2], 5.0F, "instance rotation is applied to corners");
}

void expect_fits(const preview::Fit& fit, const preview::Bounds& world, const std::string_view name) {
    const auto projection = preview::project_corners(world, fit.camera);
    expect(projection.finite, std::string(name) + ": corners project in front of the camera");
    expect(projection.ndc_min_x >= -1.0F && projection.ndc_max_x <= 1.0F
            && projection.ndc_min_y >= -1.0F && projection.ndc_max_y <= 1.0F,
        std::string(name) + ": every corner is inside the viewport");
    expect(projection.depth_min > fit.camera.near_plane && projection.depth_max < fit.camera.far_plane,
        std::string(name) + ": every corner is inside the clip planes");
    const float horizontal = std::max(std::abs(projection.ndc_min_x), std::abs(projection.ndc_max_x));
    const float vertical = std::max(std::abs(projection.ndc_min_y), std::abs(projection.ndc_max_y));
    // The binding axis reaches 1/margin of the half-extent: whole, not tiny.
    const float reach = std::max(horizontal, vertical);
    expect(reach >= 0.99F / preview::hull_fit_margin && reach <= 1.0F / preview::hull_fit_margin + 1e-3F,
        std::string(name) + ": the whole Hull fills the frame up to the fit margin");
    expect_close(fit.ndc_max_x, projection.ndc_max_x, std::string(name) + ": fit reports its projection");
}

void test_fit() {
    const assets::Model model = mc50_like();
    const auto rest = preview::rest_placement(model, 0, 0);
    if (!rest) {
        expect(false, "fit prerequisites: " + rest.failure);
        return;
    }
    const eawr::presentation::FixedCamera frozen_camera;
    const preview::Bounds world = preview::world_bounds(rest.value->asset, identity_instance());
    const auto fit = preview::fit_camera(
        world, frozen_camera, preview::hull_view_direction, preview::hull_fit_margin);
    expect(static_cast<bool>(fit), "Hull fits the 1280x720 viewport: " + fit.failure);
    if (fit) {
        expect_fits(*fit.value, world, "landscape");
        expect_close(fit.value->camera.target[2], -(hull_min.y + hull_max.y) * 0.5F,
            "the camera targets the Hull centre");
        expect(fit.value->camera.vertical_fov_degrees == frozen_camera.vertical_fov_degrees
                && fit.value->camera.width == frozen_camera.width,
            "the fit keeps the base field of view and viewport");
        expect(fit.value->camera.near_plane > 0.0F
                && fit.value->camera.far_plane < frozen_camera.far_plane,
            "the fit narrows the clip range around the Hull");
    }

    // The frozen fixed camera would show the Hull far smaller.
    const auto frozen_view = preview::project_corners(world, frozen_camera);
    expect(frozen_view.finite && frozen_view.ndc_max_x < 0.5F,
        "the frozen fixed camera leaves the Hull small, which is why Hull frames itself");

    eawr::presentation::FixedCamera portrait = frozen_camera;
    portrait.width = 720;
    portrait.height = 1280;
    const auto tall = preview::fit_camera(world, portrait, preview::hull_view_direction,
        preview::hull_fit_margin);
    expect(static_cast<bool>(tall), "Hull fits a portrait viewport too: " + tall.failure);
    if (tall) expect_fits(*tall.value, world, "portrait");

    std::array<float, 16> moved = identity_instance();
    moved[12] = 350.0F;
    moved[13] = -20.0F;
    moved[14] = 900.0F;
    const preview::Bounds far_world = preview::world_bounds(rest.value->asset, moved);
    const auto moved_fit = preview::fit_camera(far_world, frozen_camera,
        preview::hull_view_direction, preview::hull_fit_margin);
    expect(static_cast<bool>(moved_fit), "an offset instance still fits: " + moved_fit.failure);
    if (moved_fit) expect_fits(*moved_fit.value, far_world, "offset instance");

    expect_failure(preview::fit_camera(world, frozen_camera, {0.0F, 1.0F, 0.0F},
                       preview::hull_fit_margin),
        "parallel to the camera up vector", "a view straight down the up axis fails closed");
    expect_failure(preview::fit_camera(world, frozen_camera, {0.0F, 0.0F, 0.0F},
                       preview::hull_fit_margin),
        "view direction is invalid", "a zero view direction fails closed");
    preview::Bounds broken = world;
    broken.max[0] = std::numeric_limits<float>::quiet_NaN();
    expect_failure(preview::fit_camera(broken, frozen_camera, preview::hull_view_direction,
                       preview::hull_fit_margin),
        "world bounds are invalid", "non-finite bounds fail closed");
    const preview::Bounds point{{1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F}};
    expect_failure(preview::fit_camera(point, frozen_camera, preview::hull_view_direction,
                       preview::hull_fit_margin),
        "degenerate", "zero-size bounds fail closed");
    expect_failure(preview::fit_camera(world, frozen_camera, preview::hull_view_direction, 0.5F),
        "camera base is invalid", "a margin that would clip fails closed");

    // A camera inside the bounds cannot see every corner.
    eawr::presentation::FixedCamera inside = frozen_camera;
    inside.eye = {0.0F, 0.0F, 0.0F};
    inside.target = {0.0F, 0.0F, -1.0F};
    expect(!preview::project_corners(world, inside).finite,
        "a camera inside the Hull does not count as a fit");
}

} // namespace


void test_unit_preview() {
    expect(preview::mesh_levels("AT-ST_LOD2").lod == 2 && preview::mesh_levels("AT-ST_LOD2").alt == -1, "LOD tag parses");
    expect(preview::mesh_levels("Hull_ALT1_LOD0").alt == 1 && preview::mesh_levels("Hull_ALT1_LOD0").lod == 0, "ALT and LOD parse");
    expect(preview::mesh_levels("Hull_lod1").lod == -1 && preview::mesh_levels("X_LOD").lod == -1, "tags are case-sensitive and need digits");
    assets::Model model;
    model.bones.push_back({"Root", -1, true, 0, identity_bone()});
    model.bones.push_back({"Gun", 0, true, 0, {1.0F, 0.0F, 0.0F, 10.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}});
    const assets::Vec3f low{-1.0F, -1.0F, -1.0F};
    const assets::Vec3f high{1.0F, 1.0F, 1.0F};
    model.meshes.push_back(mesh("Body_LOD0", 0, submesh("RSkinGloss.fx", "a.dds", low, high), low, high));
    model.meshes.push_back(mesh("Body_LOD1", 0, submesh("RSkinGloss.fx", "a.dds", low, high), low, high));
    model.meshes.push_back(mesh("Gun", 1, submesh("MeshGloss.fx", "b.dds", low, high), low, high));
    model.meshes.back().visible = true;
    const preview::UnitLevels levels = preview::unit_levels(model);
    expect(levels.lod == 1 && levels.max_lod == 1 && levels.alt == 0, "the most detailed LOD is drawn by default");
    const auto surfaces = preview::unit_surfaces(model, levels);
    expect(surfaces.size() == 2 && surfaces[0].mesh == 1 && surfaces[1].mesh == 2, "untagged and matching meshes are drawn");
    expect(preview::unit_levels(model, 0).lod == 0 && preview::unit_levels(model, 7).lod == 1, "a requested LOD is clamped");
    std::vector<eawr::presentation::animation::BonePose> pose(2);
    for (auto& bone : pose) {
        bone.model_asset = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
        bone.skin_asset = bone.model_asset;
    }
    pose[1].model_asset[12] = 10.0F;
    const std::vector<preview::UnitSurface> gun{{2, 0}};
    const auto placed = preview::posed_bounds(model, gun, pose);
    expect(placed && std::abs(placed.value->min[0] - 9.0F) < 1e-4F && std::abs(placed.value->max[0] - 11.0F) < 1e-4F,
        "a rigid surface is framed at its bone");
    pose[1].visible = false;
    expect(!preview::surface_visible(model, gun[0], pose) && !preview::posed_bounds(model, gun, pose),
        "a hidden bone hides its mesh");
    const auto times = preview::parse_floats("0,0.25,1e-1");
    expect(times && times.value->size() == 3 && std::abs((*times.value)[2] - 0.1F) < 1e-6F, "times parse");
    expect(!preview::parse_floats("0,,1") && !preview::parse_floats("nan") && !preview::parse_floats(""), "bad times fail");
    const preview::StripLayout six = preview::strip_layout(6, 1280, 720);
    expect(six.columns == 3 && six.rows == 2 && six.tile_width == 640 && six.tile_height == 360, "six frames tile 3x2");
    const preview::StripLayout one = preview::strip_layout(1, 1280, 720);
    expect(one.columns == 1 && one.rows == 1 && one.tile_width == 1280, "one frame stays full size");
}

int main() {
    test_plans();
    test_selection();
    test_rest_placement();
    test_world_bounds();
    test_fit();
    test_unit_preview();
    if (failures != 0) {
        std::cerr << failures << " model preview contract(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "model preview contracts passed\n";
    return EXIT_SUCCESS;
}
