#include "hull_asset_fixture.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"
#include "model_preview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace eawr::lighting_probe {
namespace {

namespace preview = viewer::model_preview;
namespace lighting = presentation::lighting;

[[nodiscard]] std::string hash(const std::vector<std::byte>& bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

[[nodiscard]] std::filesystem::path data_root(const std::filesystem::path& root) {
    std::string name = root.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "data") return root;
    return std::filesystem::is_directory(root / "Data") ? root / "Data" : root;
}

[[nodiscard]] Vec3 render(const assets::Vec3f& v) noexcept { return {v.x, v.z, -v.y}; }

[[nodiscard]] Prepared fail(std::string reason) { return {std::nullopt, std::move(reason)}; }

} // namespace

Lighting source_backed_lighting() {
    const auto environment = lighting::alo_viewer_default_environment();
    const auto matrices = lighting::source_to_render(lighting::sph_light_all(environment));
    const auto travel = lighting::source_to_render(environment.lights[0].direction);
    Lighting result;
    result.sph = matrices.rgb;
    result.toward_light = {-travel.x, -travel.y, -travel.z};
    result.specular = {environment.specular.r, environment.specular.g, environment.specular.b};
    result.shadow_floor = {environment.shadow.r, environment.shadow.g, environment.shadow.b};
    return result;
}

std::vector<Triangle> render_triangles(const assets::Model& model, std::size_t* skipped) {
    std::vector<Triangle> result;
    std::size_t dropped = 0;
    for (const assets::Mesh& mesh : model.meshes) {
        if (!mesh.visible) continue;
        for (const assets::Submesh& submesh : mesh.submeshes) {
            for (std::size_t index = 0; index + 2 < submesh.indices.size(); index += 3) {
                const auto& a = submesh.vertices.at(submesh.indices[index]);
                const auto& b = submesh.vertices.at(submesh.indices[index + 1]);
                const auto& c = submesh.vertices.at(submesh.indices[index + 2]);
                Triangle t{render(a.position), render(b.position), render(c.position),
                    render(a.normal) + render(b.normal) + render(c.normal)};
                const Vec3 area = cross(t.b - t.a, t.c - t.a);
                if (dot(area, area) < 1.0e-12F) {
                    ++dropped;
                    continue;
                }
                result.push_back(t);
            }
        }
    }
    if (skipped) *skipped = dropped;
    return result;
}

const char* upload_route_name(const UploadRoute route) noexcept {
    return route == UploadRoute::unskinned ? "unskinned" : "rigid_skinned";
}

std::optional<assets::Model> without_skinning(const assets::Model& model) {
    // Match GodotRenderer's rigid upload after #52: the skin palette starts at
    // identity, but rigid source vertices first move through their bind bone.
    auto player = presentation::animation::Player::create(model);
    if (!player) return std::nullopt;
    auto rest = player.value().sample({});
    if (!rest) return std::nullopt;
    assets::Model result = model;
    for (assets::Mesh& mesh : result.meshes) {
        if (mesh.bone >= 0 && static_cast<std::size_t>(mesh.bone) >= rest.value().bones.size()) {
            return std::nullopt;
        }
        for (assets::Submesh& submesh : mesh.submeshes) {
            if (!submesh.skin_bones.empty()) return std::nullopt;
            if (mesh.bone >= 0) {
                const auto& bind = rest.value().bones[static_cast<std::size_t>(mesh.bone)].model_asset;
                for (assets::Vertex& vertex : submesh.vertices) {
                    vertex = presentation::animation::Player::rigid_vertex_to_bind(bind, vertex);
                }
            }
        }
        mesh.bone = -1;
    }
    return result;
}

std::string surface_sha256(const assets::Model& model) {
    std::vector<std::uint8_t> bytes;
    const auto append = [&bytes](const void* data, std::size_t size) {
        const auto* first = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), first, first + size);
    };
    const auto vec3 = [&append](const assets::Vec3f& v) {
        const std::array<float, 3> values{v.x, v.y, v.z};
        append(values.data(), sizeof(values));
    };
    for (const assets::Mesh& mesh : model.meshes) {
        if (!mesh.visible) continue;
        for (const assets::Submesh& submesh : mesh.submeshes) {
            const std::array<std::uint64_t, 2> counts{submesh.vertices.size(), submesh.indices.size()};
            append(counts.data(), sizeof(counts));
            for (const assets::Vertex& vertex : submesh.vertices) {
                vec3(vertex.position);
                vec3(vertex.normal);
                vec3(vertex.tangent);
                vec3(vertex.binormal);
                const std::array<float, 2> uv{vertex.texcoord[0].x, vertex.texcoord[0].y};
                append(uv.data(), sizeof(uv));
            }
            append(submesh.indices.data(), submesh.indices.size() * sizeof(std::uint16_t));
        }
    }
    return sim::sha256_hex(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

Prepared load_hull_fixture(
    const std::filesystem::path& game_root, const std::filesystem::path& mod_root) {
    const std::filesystem::path expansion = game_root / "corruption" / "Data";
    const std::filesystem::path base = game_root / "GameData" / "Data";
    if (mod_root.empty() || !std::filesystem::is_directory(data_root(mod_root))
        || !std::filesystem::is_directory(expansion) || !std::filesystem::is_directory(base)) {
        return fail("requires the Remake mod root and game GameData/Data + corruption/Data");
    }
    const std::array<std::pair<std::string, std::filesystem::path>, 3> roots{{
        {"mod", data_root(mod_root)}, {"expansion", expansion}, {"base", base}}};
    std::vector<vfs::MountSpec> specs;
    for (const auto& [id, root] : roots) {
        auto manifest = vfs::resolve_manifest_mount(id, root);
        if (!manifest) return fail(core::format_diagnostic(manifest.error()));
        specs.push_back(std::move(manifest.value().mount));
    }
    auto filesystem = vfs::Vfs::mount(specs);
    if (!filesystem) return fail(core::format_diagnostic(filesystem.error()));

    auto planned = preview::plan_for({preview::pinned_model_path, preview::hull_mesh, {}, false});
    if (!planned) return fail(planned.failure);
    const preview::Plan& plan = *planned.value;
    Fixture fixture;
    fixture.model_path = std::string(preview::pinned_model_path);
    auto model_bytes = filesystem.value().open(fixture.model_path);
    auto model_record = filesystem.value().stat(fixture.model_path);
    if (!model_bytes || !model_record) return fail("pinned MC-50 model is missing");
    fixture.model_sha256 = hash(model_bytes.value());
    fixture.model_layer = model_record.value().layer_id;
    if (!preview::hash_matches(fixture.model_sha256, plan.expected_model_sha256)) {
        return fail("MC-50 model SHA-256 mismatch: " + fixture.model_sha256);
    }
    auto loaded = assets::load_model(filesystem.value(), fixture.model_path);
    if (!loaded) return fail(core::format_diagnostic(loaded.error()));
    const auto selection = preview::select_submesh(loaded.value(), plan);
    if (!selection) return fail(selection.failure);
    auto rest = preview::rest_placement(
        loaded.value(), selection.value->mesh_index, selection.value->submesh_index);
    if (!rest) return fail(rest.failure);
    const assets::Mesh& source_mesh = loaded.value().meshes[selection.value->mesh_index];
    const assets::Submesh& submesh = source_mesh.submeshes[selection.value->submesh_index];
    fixture.mesh_name = source_mesh.name;
    fixture.program = submesh.shader;
    fixture.technique = selection.value->material.technique;
    fixture.pass_name = selection.value->material.pass;
    fixture.vertex_count = submesh.vertices.size();
    fixture.index_count = submesh.indices.size();
    fixture.bone_chain = rest.value->chain;
    fixture.rest_placement_delta = rest.value->max_placement_delta;
    fixture.rest_tolerance = rest.value->tolerance;

    auto texture_path = preview::texture_path_for(plan, submesh);
    if (!texture_path) return fail(texture_path.failure);
    fixture.texture_path = *texture_path.value;
    auto texture_bytes = filesystem.value().open(fixture.texture_path);
    auto texture_record = filesystem.value().stat(fixture.texture_path);
    if (!texture_bytes || !texture_record) return fail("Hull BaseTexture is missing");
    fixture.texture_sha256 = hash(texture_bytes.value());
    fixture.texture_layer = texture_record.value().layer_id;
    if (!preview::hash_matches(fixture.texture_sha256, plan.expected_texture_sha256)) {
        return fail("Hull BaseTexture SHA-256 mismatch: " + fixture.texture_sha256);
    }
    auto texture = assets::load_texture(filesystem.value(), fixture.texture_path);
    if (!texture) return fail(core::format_diagnostic(texture.error()));
    fixture.texture = std::move(texture.value());

    // The viewer's upload: the selected mesh reduced to its one submesh, with
    // the model's bones so the renderer's rigid Hull bone binding holds.
    fixture.model.source = loaded.value().source;
    fixture.model.bones = loaded.value().bones;
    {
        assets::Mesh mesh = source_mesh;
        mesh.submeshes = {submesh};
        fixture.model.meshes.push_back(std::move(mesh));
    }
    fixture.material = presentation::MaterialDescription{
        .schema_version = presentation::MaterialDescription::current_schema_version,
        .route = presentation::MaterialRoute::legacy_effect,
        .pass = presentation::RenderPass::opaque,
        .program = submesh.shader,
        .technique = fixture.technique,
        .pass_name = fixture.pass_name,
        .bindings = {},
    };
    for (const assets::MaterialParameter& parameter : submesh.parameters) {
        fixture.material.bindings.push_back({parameter.name, parameter.value});
    }

    fixture.lighting = source_backed_lighting();
    fixture.full_model = loaded.value();
    fixture.rest_asset_bounds = rest.value->asset;
    return {std::move(fixture), {}};
}

namespace {

std::vector<Triangle> translated(std::vector<Triangle> triangles, Vec3 offset, std::uint32_t group) {
    for (Triangle& t : triangles) {
        t.a = t.a + offset;
        t.b = t.b + offset;
        t.c = t.c + offset;
        t.group = group;
    }
    return triangles;
}

// `framed` is fitted into the viewport; `covered` (which contains it) bounds
// the distances the shadow must reach.
std::vector<CandidateResult> evaluate_views(const Fixture& fixture, const TriangleBvh& scene,
    const TriangleBvh* receiver_only, const viewer::model_preview::Bounds& framed,
    const viewer::model_preview::Bounds& covered, std::vector<MaskPlan>* plans, std::string* failure) {
    const Vec3 toward{fixture.lighting.toward_light[0], fixture.lighting.toward_light[1],
        fixture.lighting.toward_light[2]};
    std::vector<CandidateResult> result;
    for (const auto& direction : hull_view_candidates) {
        presentation::FixedCamera framing;
        framing.width = capture_width;
        framing.height = capture_height;
        auto fit = preview::fit_camera(framed, framing, direction, fit_margin);
        if (!fit) {
            if (failure) *failure = "candidate view fit failed: " + fit.failure;
            return {};
        }
        CandidateResult candidate;
        candidate.view_direction = direction;
        candidate.camera = fit.value->camera;
        const Vec3 eye{candidate.camera.eye[0], candidate.camera.eye[1], candidate.camera.eye[2]};
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 p{(corner & 1) ? covered.max[0] : covered.min[0],
                (corner & 2) ? covered.max[1] : covered.min[1], (corner & 4) ? covered.max[2] : covered.min[2]};
            const Vec3 d = p - eye;
            candidate.far_distance = std::max(candidate.far_distance, std::sqrt(dot(d, d)));
        }
        const Camera camera{eye,
            {candidate.camera.target[0], candidate.camera.target[1], candidate.camera.target[2]},
            {candidate.camera.up[0], candidate.camera.up[1], candidate.camera.up[2]},
            candidate.camera.vertical_fov_degrees, candidate.camera.width, candidate.camera.height};
        MaskPolicy policy;
        policy.max_depth = candidate.far_distance;
        MaskPlan masks = plan_masks(scene, receiver_only, camera, toward, policy);
        candidate.lit = masks.lit;
        candidate.shadowed = masks.shadowed;
        candidate.surface = masks.surface;
        candidate.eligible = masks.eligible;
        candidate.centre_blocked = masks.centre_blocked;
        candidate.raw_shadowed = masks.raw_shadowed;
        candidate.qualifies = masks.lit >= min_mask_pixels && masks.shadowed >= min_mask_pixels;
        result.push_back(candidate);
        if (plans) plans->push_back(std::move(masks));
    }
    return result;
}

} // namespace

std::vector<CandidateResult> diagnose_self_shadow(const Fixture& fixture) {
    const TriangleBvh hull(render_triangles(fixture.model, nullptr));
    constexpr std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const preview::Bounds world = preview::world_bounds(fixture.rest_asset_bounds, identity);
    return evaluate_views(fixture, hull, nullptr, world, world, nullptr, nullptr);
}

Prepared prepare_hull_fixture(const std::filesystem::path& game_root,
    const std::filesystem::path& mod_root, const FixtureOptions& options) {
    Prepared loaded = load_hull_fixture(game_root, mod_root);
    if (!loaded.fixture) return loaded;
    Fixture& fixture = *loaded.fixture;
    fixture.options = options;
    const std::vector<Triangle> receiver = render_triangles(fixture.model, &fixture.skipped_degenerate);
    fixture.triangle_count = receiver.size();
    const Vec3 toward = normalized({fixture.lighting.toward_light[0], fixture.lighting.toward_light[1],
        fixture.lighting.toward_light[2]});
    const Vec3 offset = toward * options.caster_sun_distance + Vec3{options.caster_lateral_offset, 0.0F, 0.0F};
    fixture.caster_translation = {offset.x, offset.y, offset.z};
    std::vector<Triangle> scene = receiver;
    const std::vector<Triangle> caster = translated(receiver, offset, 1);
    scene.insert(scene.end(), caster.begin(), caster.end());
    const TriangleBvh scene_bvh(std::move(scene));
    const TriangleBvh receiver_bvh(receiver);
    constexpr std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<float, 16> moved = identity;
    moved[12] = offset.x;
    moved[13] = offset.y;
    moved[14] = offset.z;
    const preview::Bounds receiver_world = preview::world_bounds(fixture.rest_asset_bounds, identity);
    const preview::Bounds caster_world = preview::world_bounds(fixture.rest_asset_bounds, moved);
    preview::Bounds world = receiver_world;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        world.min[axis] = std::min(receiver_world.min[axis], caster_world.min[axis]);
        world.max[axis] = std::max(receiver_world.max[axis], caster_world.max[axis]);
    }
    fixture.scene_bounds = world;
    std::vector<MaskPlan> plans;
    std::string failure;
    // Frame the receiver Hull; the caster may leave the frame but not the
    // shadow's coverage distance.
    fixture.candidates = evaluate_views(
        fixture, scene_bvh, &receiver_bvh, receiver_world, world, &plans, &failure);
    if (fixture.candidates.empty()) return fail(failure);
    std::size_t best_score = 0;
    bool any = false;
    for (std::size_t index = 0; index < fixture.candidates.size(); ++index) {
        const CandidateResult& candidate = fixture.candidates[index];
        const std::size_t score = std::min(candidate.lit, candidate.shadowed);
        if (candidate.qualifies && (!any || score > best_score)) {
            any = true;
            best_score = score;
            fixture.selected = index;
        }
    }
    if (!any) {
        std::string counts;
        for (const CandidateResult& candidate : fixture.candidates) {
            counts += " [eligible " + std::to_string(candidate.eligible) + ", lit "
                + std::to_string(candidate.lit) + ", shadowed " + std::to_string(candidate.shadowed) + "]";
        }
        return fail("no predeclared view gives both masks the minimum pixel count:" + counts);
    }
    const CandidateResult& chosen = fixture.candidates[fixture.selected];
    fixture.camera = chosen.camera;
    fixture.far_distance = chosen.far_distance;
    fixture.policy.max_depth = chosen.far_distance;
    fixture.masks = std::move(plans[fixture.selected]);
    fixture.shadow_max_distance = std::ceil(1.1F * chosen.far_distance / assumed_fade_start);
    return loaded;
}

} // namespace eawr::lighting_probe
