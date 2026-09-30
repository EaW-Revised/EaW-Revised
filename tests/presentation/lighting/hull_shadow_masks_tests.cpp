// CPU contracts for the real-hull shadow mask classifier (P1-04, #25).
// Synthetic geometry only by default. Opt-in: `--game-root <dir> --mod-root
// <dir> [--mask-output <dir>] [--sun-scan]` also prepares the pinned MC-50
// Hull fixture on the CPU, prints the single-Hull self-shadow diagnostic and
// the two-Hull candidate mask counts, and optionally writes the mask PGM.
// `--sun-scan` repeats the single-Hull diagnostic for 40 relative sun
// directions (equivalent to 40 ship orientations); `--list-meshes` prints the
// whole model's meshes; `--light-variants <dir>` writes unfiltered two-Hull
// classifications for the sun rotated +-20 degrees (diagnostic only).
#include "hull_asset_fixture.hpp"
#include "hull_shadow_masks.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace eawr::lighting_probe;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void add_quad(std::vector<Triangle>& out, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 normal,
    std::uint32_t group, bool flip = false) {
    if (flip) {
        out.push_back({a, c, b, normal, group});
        out.push_back({a, d, c, normal, group});
    } else {
        out.push_back({a, b, c, normal, group});
        out.push_back({a, c, d, normal, group});
    }
}

void add_box(std::vector<Triangle>& out, float x0, float x1, float z0, float z1, float h, std::uint32_t group) {
    add_quad(out, {x0, h, z0}, {x1, h, z0}, {x1, h, z1}, {x0, h, z1}, {0, 1, 0}, group);
    add_quad(out, {x0, 0, z1}, {x1, 0, z1}, {x1, h, z1}, {x0, h, z1}, {0, 0, 1}, group);
    add_quad(out, {x0, 0, z0}, {x1, 0, z0}, {x1, h, z0}, {x0, h, z0}, {0, 0, -1}, group);
    add_quad(out, {x1, 0, z0}, {x1, 0, z1}, {x1, h, z1}, {x1, h, z0}, {1, 0, 0}, group);
    add_quad(out, {x0, 0, z0}, {x0, 0, z1}, {x0, h, z1}, {x0, h, z0}, {-1, 0, 0}, group);
}

// Receiver (group 0): a deck plus its own fin at x in (6, 14). Caster
// (group 1): a separate tower at x in (-3, 3). Both are 10 high.
std::vector<Triangle> receiver(bool flip) {
    std::vector<Triangle> result;
    add_quad(result, {-20, 0, -20}, {20, 0, -20}, {20, 0, 20}, {-20, 0, 20}, {0, 1, 0}, 0, flip);
    add_box(result, 6, 14, -1, 1, 10, 0);
    return result;
}

constexpr float eye_height = 60.0F;
const Vec3 toward_sun{0.0F, 0.70710678F, 0.70710678F};

// Top-down camera over the deck: eye (0,60,0), up -Z, so screen right is +X
// and screen up is -Z.
Camera top_camera() { return {{0, eye_height, 0}, {0, 0, 0}, {0, 0, -1}, 45.0F, 201, 201}; }

// Pixel whose centre ray meets the deck (y = 0) at (x, z).
std::size_t pixel_at(const Camera& camera, float x, float z) {
    const float tangent = std::tan(22.5F * 3.14159265358979F / 180.0F);
    const float half = eye_height * tangent;
    const float px = (x / half + 1.0F) * 0.5F * static_cast<float>(camera.width) - 0.5F;
    const float py = (z / half + 1.0F) * 0.5F * static_cast<float>(camera.height) - 0.5F;
    return static_cast<std::size_t>(std::lround(py)) * camera.width
        + static_cast<std::size_t>(std::lround(px));
}

void test_pixel_ray() {
    const Camera camera = top_camera();
    const Vec3 centre = pixel_ray(camera, 100, 100);
    check(std::abs(centre.x) < 1e-6F && std::abs(centre.y + 1.0F) < 1e-6F && std::abs(centre.z) < 1e-6F,
        "centre pixel looks straight down");
    check(pixel_ray(camera, 200, 100).x > 0.0F, "right column points to +X");
    check(pixel_ray(camera, 100, 0).z < 0.0F, "top row points to -Z (camera up)");
    Camera wide = camera;
    wide.width = 402;
    const Vec3 edge = pixel_ray(wide, 401, 100);
    const Vec3 top = pixel_ray(camera, 100, 0);
    check(std::abs(edge.x / -edge.y - 2.0F * (-top.z / -top.y)) < 0.02F,
        "horizontal extent scales with aspect (vertical FOV kept)");
}

void test_bvh_matches_brute_force() {
    std::uint32_t state = 12345;
    const auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(state >> 8) / 16777216.0F * 20.0F - 10.0F;
    };
    std::vector<Triangle> triangles;
    for (int i = 0; i < 300; ++i) {
        const Vec3 a{next(), next(), next()};
        triangles.push_back({a, a + Vec3{next() * 0.2F, next() * 0.2F, next() * 0.2F},
            a + Vec3{next() * 0.2F, next() * 0.2F, next() * 0.2F}, {0, 1, 0}, 0});
    }
    const TriangleBvh bvh(triangles);
    int mismatches = 0;
    int hits = 0;
    for (int ray = 0; ray < 2000; ++ray) {
        const Vec3 origin{next(), next(), next()};
        const Vec3 direction = normalized({next(), next(), next()});
        const auto fast = bvh.closest(origin, direction, 0.0F);
        float best = -1.0F;
        for (const Triangle& t : bvh.triangles()) {
            const TriangleBvh single({t});
            if (const auto h = single.closest(origin, direction, 0.0F); h && (best < 0 || h->distance < best)) {
                best = h->distance;
            }
        }
        if (fast) ++hits;
        if (fast.has_value() != (best >= 0.0F) || (fast && std::abs(fast->distance - best) > 1e-4F)) ++mismatches;
    }
    check(mismatches == 0, "BVH closest hit matches brute force");
    check(hits > 50, "random rays exercise hits");
}

void test_caster_shadow(bool flip) {
    const std::vector<Triangle> own = receiver(flip);
    std::vector<Triangle> scene = own;
    add_box(scene, -3, 3, -1, 1, 10, 1);
    const TriangleBvh scene_bvh(scene);
    const TriangleBvh receiver_bvh(own);
    const Camera camera = top_camera();
    const MaskPlan plan = plan_masks(scene_bvh, &receiver_bvh, camera, toward_sun, {});
    const std::string suffix = flip ? " (flipped deck winding)" : "";
    check(plan.classes.size() == 201U * 201U, "mask size" + suffix);
    // Sun above +Z: the tower's shadow falls towards -Z, x in (-3, 3),
    // z in (-11, -1). Contact within 3 units of the tower is excluded.
    check(plan.classes[pixel_at(camera, 0.0F, -7.0F)] == PixelClass::shadowed, "deep caster shadow" + suffix);
    check(plan.classes[pixel_at(camera, -12.0F, -7.0F)] == PixelClass::lit, "open deck is lit" + suffix);
    check(plan.classes[pixel_at(camera, 0.0F, 8.0F)] == PixelClass::lit, "sunward deck is lit" + suffix);
    check(plan.classes[pixel_at(camera, 0.0F, -1.6F)] == PixelClass::excluded, "contact zone excluded" + suffix);
    check(plan.classes[pixel_at(camera, 3.0F, -7.0F)] == PixelClass::excluded, "shadow edge excluded" + suffix);
    check(plan.classes[pixel_at(camera, 0.0F, 0.0F)] == PixelClass::excluded, "caster surface excluded" + suffix);
    check(plan.classes[pixel_at(camera, 10.0F, -7.0F)] == PixelClass::excluded,
        "receiver self-shadow excluded" + suffix);
    check(plan.classes[pixel_at(camera, 0.0F, -19.9F)] == PixelClass::excluded, "deck edge excluded" + suffix);
    check(plan.shadowed > 30 && plan.lit > 1000, "both masks populated" + suffix);
    for (std::size_t index = 0; index < plan.classes.size(); ++index) {
        if (plan.classes[index] != PixelClass::shadowed) continue;
        const std::size_t x = index % camera.width;
        const std::size_t y = index / camera.width;
        const Vec3 ray = pixel_ray(camera, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        const Vec3 hit = camera.eye + ray * (eye_height / -ray.y);
        if (!(std::abs(hit.x) < 3.0F && hit.z < -1.0F && hit.z > -11.0F)) {
            check(false, "shadowed pixel outside the analytic caster shadow" + suffix);
            break;
        }
    }
    // Missing caster: every predicted receiver pixel must then be lit.
    const MaskPlan missing = plan_masks(receiver_bvh, &receiver_bvh, camera, toward_sun, {});
    check(missing.shadowed == 0, "no shadowed pixel without the caster" + suffix);
    std::size_t relit = 0;
    for (std::size_t index = 0; index < plan.classes.size(); ++index) {
        if (plan.classes[index] == PixelClass::shadowed && missing.classes[index] == PixelClass::lit) ++relit;
    }
    check(relit * 10 >= plan.shadowed * 9, "missing caster relights the receiver mask" + suffix);
}

// An open caster whose only face points away from the sun is culled by the
// renderer's shadow pass, so it must not predict a shadow unless the policy
// asks for double-sided casters.
void test_single_sided_casters() {
    const std::vector<Triangle> own = receiver(false);
    std::vector<Triangle> scene = own;
    add_quad(scene, {-3, 10, -4}, {3, 10, -4}, {3, 10, 4}, {-3, 10, 4}, {0, -1, 0}, 1);
    const TriangleBvh scene_bvh(scene);
    const TriangleBvh receiver_bvh(own);
    const Camera camera = top_camera();
    // The plate's shadow falls on z in (-14, -6).
    const std::size_t under = pixel_at(camera, 0.0F, -10.0F);
    const MaskPlan single = plan_masks(scene_bvh, &receiver_bvh, camera, toward_sun, {});
    check(single.classes[under] == PixelClass::excluded, "sun-averted open caster predicts no shadow");
    MaskPolicy both;
    both.single_sided_casters = false;
    const MaskPlan double_sided = plan_masks(scene_bvh, &receiver_bvh, camera, toward_sun, both);
    check(double_sided.classes[under] == PixelClass::shadowed, "double-sided policy counts the averted face");
    const Vec3 up{0.0F, 1.0F, 0.0F};
    check(scene_bvh.closest({0, 5, 0}, up, 0.0F, Faces::both).has_value()
        && !scene_bvh.closest({0, 5, 0}, up, 0.0F, Faces::exiting).has_value(), "exiting-face filter");
}

void test_negatives_and_encoding() {
    const TriangleBvh deck_only(receiver(false));
    MaskPolicy grazing;
    grazing.min_light_facing = 0.8F;
    const MaskPlan none = plan_masks(deck_only, nullptr, top_camera(), toward_sun, grazing);
    check(none.lit == 0 && none.shadowed == 0, "surfaces not facing the sun enough are excluded");
    const MaskPlan plain = plan_masks(deck_only, nullptr, top_camera(), toward_sun, {});
    check(plain.shadowed > 0, "without receiver_only the fin self-shadow counts as shadowed");
    const MaskPlan own = plan_masks(deck_only, &deck_only, top_camera(), toward_sun, {});
    check(own.shadowed == 0 && own.lit == plain.lit, "receiver_only removes only self-shadowed pixels");
    const auto pgm = encode_pgm(own);
    const std::string header = "P5\n201 201\n2\n";
    check(pgm.size() == header.size() + 201U * 201U
        && std::string(pgm.begin(), pgm.begin() + static_cast<std::ptrdiff_t>(header.size())) == header,
        "PGM header and payload size");
}

// The matched skinning control changes only the bone binding: the unskinned
// route bakes each rigid mesh's rest bind into its vertices (what #52's
// renderer does for the rigid route) and clears the bone, so both routes
// upload the same bind-space surface. A palette-skinned mesh or a bone the
// rest pose cannot resolve is rejected rather than silently uploaded.
void test_unskinned_upload_route() {
    eawr::assets::Model model;
    model.bones.resize(2);
    model.bones[0].relative_transform = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    model.bones[1].parent = 0;
    model.bones[1].relative_transform = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 2.0F};
    eawr::assets::Mesh mesh;
    mesh.bone = 1;
    eawr::assets::Submesh submesh;
    for (int i = 0; i < 3; ++i) {
        eawr::assets::Vertex vertex;
        vertex.position = {static_cast<float>(i == 1), static_cast<float>(i == 2), 0.5F};
        vertex.normal = {0.0F, 0.0F, 1.0F};
        vertex.bone_indices = {1, 0, 0, 0};
        vertex.bone_weights = {1.0F, 0.0F, 0.0F, 0.0F};
        submesh.vertices.push_back(vertex);
        submesh.indices.push_back(static_cast<std::uint16_t>(i));
    }
    mesh.submeshes.push_back(submesh);
    model.meshes.push_back(mesh);
    model.meshes.push_back(mesh);
    model.meshes.back().bone = 0;

    const auto plain = without_skinning(model);
    check(plain.has_value(), "unskinned upload bakes the rigid rest bind of a valid model");
    if (plain) {
        check(plain->bones.size() == model.bones.size(), "unskinned upload keeps the model bones");
        for (const eawr::assets::Mesh& m : plain->meshes) {
            check(m.bone == -1, "unskinned upload clears the rigid bone");
            for (const eawr::assets::Submesh& sm : m.submeshes) check(sm.skin_bones.empty(), "unskinned upload has no palette");
        }
        eawr::assets::Model expected = model;
        for (eawr::assets::Vertex& vertex : expected.meshes[0].submeshes[0].vertices) vertex.position.z += 2.0F;
        const auto& baked = plain->meshes[0].submeshes[0].vertices;
        const auto& want = expected.meshes[0].submeshes[0].vertices;
        bool positions = baked.size() == want.size();
        for (std::size_t i = 0; positions && i < baked.size(); ++i) {
            positions = baked[i].position.x == want[i].position.x && baked[i].position.y == want[i].position.y
                && baked[i].position.z == want[i].position.z && baked[i].normal.z == want[i].normal.z;
        }
        check(positions, "rigid vertices move through the bind bone's rest transform");
        check(surface_sha256(*plain) == surface_sha256(expected), "unskinned upload hashes the bind-space surface");
        check(surface_sha256(*plain) != surface_sha256(model), "bind-space surface differs from bone-space source");
    }

    eawr::assets::Model palette = model;
    palette.meshes.back().submeshes.back().skin_bones = {0, 1};
    check(!without_skinning(palette).has_value(), "palette-skinned mesh is rejected");
    eawr::assets::Model dangling = model;
    dangling.meshes[0].bone = 2;
    check(!without_skinning(dangling).has_value(), "rigid bone outside the rest pose is rejected");
    eawr::assets::Model cyclic = model;
    cyclic.bones[0].parent = 1;
    check(!without_skinning(cyclic).has_value(), "invalid bone hierarchy is rejected");

    eawr::assets::Model moved = model;
    moved.meshes[0].submeshes[0].vertices[0].position.x += 0.25F;
    check(surface_sha256(moved) != surface_sha256(model), "surface hash covers vertex positions");
    check(std::string(upload_route_name(UploadRoute::rigid_skinned)) == "rigid_skinned"
        && std::string(upload_route_name(UploadRoute::unskinned)) == "unskinned", "upload route names");
}

void print(const std::vector<CandidateResult>& candidates, std::size_t selected) {
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        std::cout << "  view (" << c.view_direction[0] << ',' << c.view_direction[1] << ','
                  << c.view_direction[2] << ") eligible " << c.eligible << " centre-blocked "
                  << c.centre_blocked << " raw-shadowed " << c.raw_shadowed << " lit " << c.lit
                  << " shadowed " << c.shadowed << (i == selected ? " SELECTED" : "") << '\n';
    }
}

int real_asset(const std::vector<std::string>& arguments) {
    std::string game, mod, out;
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == "--game-root") game = arguments[i + 1];
        if (arguments[i] == "--mod-root") mod = arguments[i + 1];
        if (arguments[i] == "--mask-output") out = arguments[i + 1];
    }
    if (game.empty() || mod.empty()) {
        std::cout << "real MC-50 Hull CPU fixture skipped (opt-in)\n";
        return 0;
    }
    const Prepared loaded = load_hull_fixture(game, mod);
    if (!loaded.fixture) {
        std::cerr << "FAIL: real Hull fixture: " << loaded.failure << '\n';
        return 1;
    }
    if (std::find(arguments.begin(), arguments.end(), "--list-meshes") != arguments.end()) {
        for (const auto& mesh : loaded.fixture->full_model.meshes) {
            std::cout << "mesh " << mesh.name << " visible " << mesh.visible << " bone " << mesh.bone << '\n';
            for (const auto& submesh : mesh.submeshes) {
                std::cout << "  " << submesh.shader << " vertices " << submesh.vertices.size()
                          << " indices " << submesh.indices.size() << '\n';
            }
        }
    }
    std::cout << "single Hull self-shadow diagnostic (source-backed sun):\n";
    print(diagnose_self_shadow(*loaded.fixture), static_cast<std::size_t>(-1));
    if (std::find(arguments.begin(), arguments.end(), "--sun-scan") != arguments.end()) {
        std::size_t best_raw = 0, best_eroded = 0;
        for (int elevation = -60; elevation <= 60; elevation += 30) {
            for (int heading = 0; heading < 360; heading += 45) {
                Fixture turned = *loaded.fixture;
                const float e = static_cast<float>(elevation) * 3.14159265F / 180.0F;
                const float h = static_cast<float>(heading) * 3.14159265F / 180.0F;
                turned.lighting.toward_light = {std::cos(e) * std::sin(h), std::sin(e), std::cos(e) * std::cos(h)};
                for (const auto& c : diagnose_self_shadow(turned)) {
                    best_raw = std::max(best_raw, c.raw_shadowed);
                    best_eroded = std::max(best_eroded, c.shadowed);
                }
            }
        }
        std::cout << "sun scan (40 directions x 5 views): max raw shadowed " << best_raw
                  << ", max eroded shadowed " << best_eroded << '\n';
    }
    std::string variants;
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == "--light-variants") variants = arguments[i + 1];
    }
    if (!variants.empty()) {
        // Diagnostic: unfiltered caster-shadow classification of the selected
        // view for sun directions rotated around the source-backed one.
        const Prepared base = prepare_hull_fixture(game, mod);
        if (!base.fixture) {
            std::cerr << "FAIL: " << base.failure << '\n';
            return 1;
        }
        const Fixture& f = *base.fixture;
        const std::vector<Triangle> own = render_triangles(f.model, nullptr);
        std::vector<Triangle> scene = own;
        for (Triangle t : own) {
            const Vec3 o{f.caster_translation[0], f.caster_translation[1], f.caster_translation[2]};
            t.a = t.a + o; t.b = t.b + o; t.c = t.c + o; t.group = 1;
            scene.push_back(t);
        }
        const TriangleBvh scene_bvh(scene);
        const TriangleBvh receiver_bvh(own);
        const Camera camera{{f.camera.eye[0], f.camera.eye[1], f.camera.eye[2]},
            {f.camera.target[0], f.camera.target[1], f.camera.target[2]},
            {f.camera.up[0], f.camera.up[1], f.camera.up[2]}, f.camera.vertical_fov_degrees,
            f.camera.width, f.camera.height};
        MaskPolicy loose;
        loose.min_light_facing = -1.0F;
        loose.min_view_facing = 0.0F;
        loose.light_footprint = 0.0F;
        loose.min_occluder_distance = 0.0F;
        loose.erosion_radius = 0;
        const Vec3 base_toward = normalized({f.lighting.toward_light[0], f.lighting.toward_light[1],
            f.lighting.toward_light[2]});
        const float base_elevation = std::asin(base_toward.y);
        const float base_heading = std::atan2(base_toward.x, base_toward.z);
        std::filesystem::create_directories(variants);
        for (int dh = -20; dh <= 20; dh += 5) {
            for (int de = -20; de <= 20; de += 5) {
                const float e = base_elevation + static_cast<float>(de) * 3.14159265F / 180.0F;
                const float h = base_heading + static_cast<float>(dh) * 3.14159265F / 180.0F;
                const Vec3 toward{std::cos(e) * std::sin(h), std::sin(e), std::cos(e) * std::cos(h)};
                const MaskPlan plan = plan_masks(scene_bvh, &receiver_bvh, camera, toward, loose);
                const auto pgm = encode_pgm(plan);
                std::ofstream file(std::filesystem::path(variants)
                        / ("h" + std::to_string(dh) + "_e" + std::to_string(de) + ".pgm"), std::ios::binary);
                file.write(reinterpret_cast<const char*>(pgm.data()), static_cast<std::streamsize>(pgm.size()));
            }
        }
        // Caster visibility (any class but 0) for a silhouette check.
        MaskPolicy caster_view = loose;
        caster_view.receiver_group = 1;
        const auto caster_pgm = encode_pgm(plan_masks(scene_bvh, nullptr, camera, base_toward, caster_view));
        std::ofstream caster_file(std::filesystem::path(variants) / "caster_visible.pgm", std::ios::binary);
        caster_file.write(reinterpret_cast<const char*>(caster_pgm.data()),
            static_cast<std::streamsize>(caster_pgm.size()));
        return 0;
    }
    const Prepared prepared = prepare_hull_fixture(game, mod);
    if (!prepared.fixture) {
        std::cerr << "FAIL: two-Hull fixture: " << prepared.failure << '\n';
        return 1;
    }
    const Fixture& f = *prepared.fixture;
    std::cout << "model " << f.model_path << " [" << f.model_layer << "] " << f.model_sha256 << '\n'
              << "texture " << f.texture_path << " [" << f.texture_layer << "] " << f.texture_sha256 << '\n'
              << "mesh " << f.mesh_name << ' ' << f.program << '/' << f.technique << '/' << f.pass_name
              << " vertices " << f.vertex_count << " triangles " << f.triangle_count
              << " dropped " << f.skipped_degenerate << '\n'
              << "caster translation (" << f.caster_translation[0] << ',' << f.caster_translation[1] << ','
              << f.caster_translation[2] << ")\ntwo-Hull candidates:\n";
    print(f.candidates, f.selected);
    std::cout << "shadow max distance " << f.shadow_max_distance << " far " << f.far_distance << '\n';
    if (!out.empty()) {
        std::filesystem::create_directories(out);
        const auto pgm = encode_pgm(f.masks);
        std::ofstream file(std::filesystem::path(out) / "masks.pgm", std::ios::binary);
        file.write(reinterpret_cast<const char*>(pgm.data()), static_cast<std::streamsize>(pgm.size()));
    }
    return f.masks.lit >= min_mask_pixels && f.masks.shadowed >= min_mask_pixels ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    test_pixel_ray();
    test_bvh_matches_brute_force();
    test_caster_shadow(false);
    test_caster_shadow(true);
    test_single_sided_casters();
    test_negatives_and_encoding();
    test_unskinned_upload_route();
    failures += real_asset(std::vector<std::string>(argv + 1, argv + argc));
    if (failures == 0) std::cout << "hull shadow mask contracts passed\n";
    return failures == 0 ? 0 : 1;
}
