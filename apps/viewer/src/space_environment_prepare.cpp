#include "space_environment.hpp"
#include "space_environment_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {

// Clean-room modern_spatial adapter for eawr-space-primary-sky-v1. It is a
// diffuse preview: it samples one BaseTexture per surface and nothing else,
// which is exactly what the plan admits. It is not the SKYDOME, PLANET or
// NEBULA effect. `cull_disabled, depth_draw_never` is the existing preview
// policy for a sky seen from inside; it is not a culling optimisation.
constexpr std::string_view sky_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never;
uniform sampler2D eawr_diffuse : filter_linear_mipmap, repeat_enable;
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb;
}
)GODOT";

// Negative control: passes the structural preflight, then must be rejected by
// Godot's own compiler (an undeclared identifier).
constexpr std::string_view broken_sky_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never;
uniform sampler2D eawr_diffuse : filter_linear_mipmap, repeat_enable;
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb * eawr_undeclared_control_symbol;
}
)GODOT";

// Foreground depth-occlusion control geometry. It writes depth like any
// opaque object; it is labelled as a control and never counted as sky.
constexpr std::string_view occluder_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled;
uniform sampler2D eawr_diffuse : filter_nearest, repeat_enable;
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb;
}
)GODOT";

// Sky-shadow caster controls only; never the sky preview's material. The
// shipped single-sided sky, seen from inside, turns its back face to a light
// behind it, and even with its cast flag enabled it leaves the lit receiver
// unchanged (observed on the pinned Godot backend). A positive control needs a
// surface that demonstrably casts: this variant writes depth and is uploaded
// with both windings (the surface's own geometry and UVs), so one copy faces
// the camera and the other faces the light.
constexpr std::string_view caster_sky_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_back;
uniform sampler2D eawr_diffuse : filter_linear_mipmap, repeat_enable;
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb;
}
)GODOT";

// Lit receiver of the sky-shadow controls. Unlike the sky it takes Godot's
// directional light and shadow; it is a labelled control and never sky.
// cull_back with both windings uploaded draws exactly the camera-facing copy
// with its authored normal, so the result never depends on which way the
// quad happens to be wound.
constexpr std::string_view receiver_shader = R"GODOT(
shader_type spatial;
render_mode cull_back, ambient_light_disabled;
uniform sampler2D eawr_diffuse : filter_nearest, repeat_enable;
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb;
}
void light() {
    DIFFUSE_LIGHT += vec3(ATTENUATION);
}
)GODOT";

constexpr std::uint32_t lifecycle_cycle_frames = 2;

[[nodiscard]] sim::math::Mat3x4 identity_transform() {
    using Fixed = sim::math::Fixed;
    const Fixed zero = Fixed::from_raw(0);
    const Fixed one = Fixed::from_raw(Fixed::scale);
    sim::math::Mat3x4 result{};
    result.rows[0] = {one, zero, zero, zero};
    result.rows[1] = {zero, one, zero, zero};
    result.rows[2] = {zero, zero, one, zero};
    return result;
}

[[nodiscard]] std::array<float, 3> centroid(const std::vector<space::Triangle>& triangles) {
    std::array<double, 3> sum{};
    std::size_t count = 0;
    for (const space::Triangle& triangle : triangles) {
        for (const assets::Vec3f& corner : triangle) {
            sum[0] += corner.x;
            sum[1] += corner.y;
            sum[2] += corner.z;
            ++count;
        }
    }
    if (count == 0) return {};
    return {static_cast<float>(sum[0] / static_cast<double>(count)),
            static_cast<float>(sum[1] / static_cast<double>(count)),
            static_cast<float>(sum[2] / static_cast<double>(count))};
}
} // namespace

namespace space_environment_detail {

[[nodiscard]] std::vector<MaterialBinding> meshgloss_bindings(
    const space::MeshGlossMaterial& material, const space::SkyLightPolicy& policy) {
    std::vector<MaterialBinding> bindings{
        {"BaseTexture", std::string{"space-sky-base-texture"}},
        {"Emissive", material.emissive},
        {"Diffuse", material.diffuse},
        {"Specular", material.specular},
    };
    constexpr std::array<char, 3> channels{'r', 'g', 'b'};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto& matrix = policy.sph[channel];
        for (std::size_t column = 0; column < 4; ++column) {
            bindings.push_back({"eawr_sky_sph_" + std::string(1, channels[channel]) + std::to_string(column),
                assets::Vec4f{matrix[column * 4], matrix[column * 4 + 1], matrix[column * 4 + 2], matrix[column * 4 + 3]}});
        }
    }
    bindings.push_back({"eawr_sky_light_direction", policy.light_direction});
    bindings.push_back({"eawr_sky_light_specular", policy.light_specular});
    bindings.push_back({"eawr_sky_light_scale", policy.light_scale});
    return bindings;
}

[[nodiscard]] std::vector<MaterialBinding> meshadditive_bindings(
    const space::MeshAdditiveMaterial& material, const space::MeshAdditiveInputs& inputs) {
    return {
        {"BaseTexture", std::string{"space-sky-base-texture"}},
        {"Color", material.color},
        {"UVScrollRate", material.uv_scroll_rate},
        {"eawr_sky_time", inputs.time},
        {"eawr_sky_light_scale", inputs.light_scale},
    };
}

// sky-shadow: the shipped sky, flag off (the policy under test).
// sky-shadow-cast: the shipped sky, flag on.
// sky-shadow-caster: the depth-writing caster variant, flag off.
// sky-shadow-caster-cast: the caster variant, flag on (positive control).
[[nodiscard]] bool shadow_control(const std::string_view control) {
    return control == "sky-shadow" || control == "sky-shadow-cast" || control == "sky-shadow-caster"
        || control == "sky-shadow-caster-cast";
}
[[nodiscard]] bool caster_control(const std::string_view control) {
    return control == "sky-shadow-caster" || control == "sky-shadow-caster-cast";
}


} // namespace space_environment_detail

void SpaceEnvironment::State::release_all() {
    if (!renderer) return;
    if (options.fog) {
        // Observed before release so the report keeps the bound state.
        if (!fog_final) fog_final = renderer->fog_status();
        if (fog_consumers.empty()) fog_consumers = renderer->fog_consumers();
        if (fog_units) fog_units->release(*renderer);
        renderer->disable_fog();
    }
    for (const Uploaded& item : uploaded) static_cast<void>(renderer->release(item.asset));
    if (occluder_uploaded) static_cast<void>(renderer->release(occluder_asset));
    occluder_uploaded = false;
    teardown_remaining = renderer->resources().size();
    teardown_instances = renderer->instance_count();
}

bool SpaceEnvironment::State::upload_all(const std::string& control_name) {
    uploaded.clear();
    sim::AssetId next_asset = 1;
    std::size_t accepted_seen = 0;
    const std::size_t accepted_total = plan.accepted_count();
    for (std::size_t index = 0; index < plan.surfaces.size(); ++index) {
        const space::SurfacePlan& surface = plan.surfaces[index];
        if (surface.status != space::SurfaceStatus::accepted || !surface.model || !surface.texture) continue;
        ++accepted_seen;
        // The broken-shader control breaks only the last surface, so earlier
        // uploads exist and the partial-failure release path is exercised.
        const bool broken = control_name == "broken-shader" && accepted_seen == accepted_total;
        // A MeshGloss surface draws through its own adapter with its authored
        // values; the broken-shader and caster controls keep their labelled
        // diffuse programs. The opaque pass requires light-scale alpha 1.
        const bool gloss = surface.material == space::SkyMaterial::meshgloss && !broken
            && !caster_control(control_name);
        if (gloss && (!surface.meshgloss || sky_light.light_scale.w != 1.0F)) {
            material_status = "upload_failed";
            upload_failure[index] = "a MeshGloss sky surface needs its authored values and light-scale alpha 1";
            release_all();
            uploaded.clear();
            return false;
        }
        // A MeshAdditive surface exists only under the labelled control, which
        // never combines with the broken-shader or caster controls.
        const bool additive_surface = surface.material == space::SkyMaterial::meshadditive;
        if (additive_surface && (!additive.enabled || !surface.meshadditive || broken || caster_control(control_name)
                                 || !space::finite_inputs(additive.inputs))) {
            material_status = "upload_failed";
            upload_failure[index] = "a MeshAdditive sky surface needs the meshadditive-synthetic control, its authored "
                                    "values and finite declared inputs";
            release_all();
            uploaded.clear();
            return false;
        }
        std::string_view program = sky_shader;
        if (broken) program = broken_sky_shader;
        else if (caster_control(control_name)) program = caster_sky_shader;
        else if (gloss) program = meshgloss_sky_shader;
        else if (additive_surface) program = meshadditive_sky_shader;
        const MaterialDescription material{
            .schema_version = MaterialDescription::current_schema_version,
            .route = MaterialRoute::modern_spatial,
            .pass = additive_surface ? RenderPass::transparent : RenderPass::opaque,
            .program = std::string(program),
            .technique = {},
            .pass_name = {},
            .bindings = gloss ? meshgloss_bindings(*surface.meshgloss, sky_light)
                : (additive_surface ? meshadditive_bindings(*surface.meshadditive, additive.inputs)
                                    : std::vector<MaterialBinding>{{"eawr_diffuse", std::string{"space-sky-diffuse"}}}),
        };
        const sim::AssetId asset = next_asset++;
        std::optional<assets::Model> caster;
        if (caster_control(control_name)) {
            // Control only: the same surface with its reversed triangles added.
            caster = *surface.model;
            std::vector<std::uint16_t>& indices = caster->meshes.front().submeshes.front().indices;
            const std::size_t count = indices.size();
            for (std::size_t first = 0; first + 2 < count; first += 3) {
                indices.insert(indices.end(), {indices[first], indices[first + 2], indices[first + 1]});
            }
        }
        const auto result = renderer->upload(asset, caster ? *caster : *surface.model, *surface.texture, material);
        if (!result) {
            compiler[index] = result.error().code == diagnostic_codes::shader_compile_failed ? "rejected" : "not_observed";
            upload_failure[index] = core::format_diagnostic(result.error());
            material_status = result.error().code == diagnostic_codes::shader_compile_failed
                ? "compile_failed" : "upload_failed";
            partial_failure = !uploaded.empty();
            release_all();
            partial_failure_remaining = teardown_remaining.value_or(0);
            uploaded.clear();
            return false;
        }
        compiler[index] = "compiled";
        // Before any instance exists: a sky that encloses the scene must not
        // cast. The renderer applies it when the instance is created. Only
        // the labelled *-cast shadow controls let it cast, to prove that the
        // lit receiver would detect a casting sky.
        sky_casts = control_name == "sky-shadow-cast" || control_name == "sky-shadow-caster-cast";
        renderer->set_casts_shadows(asset, sky_casts);
        uploaded.push_back({index, asset, static_cast<sim::EntityId>(asset), true});
    }
    material_status = "compiled";
    return true;
}

bool SpaceEnvironment::State::compose_occluder() {
    // A screen-aligned quad halfway between the eye and surface 0's centroid.
    // Its extent is predeclared from surface 0's own geometry: a partial cover
    // keeps surface 0 measurable, a full cover must invalidate its evidence.
    if (uploaded.empty()) return false;
    const space::SurfacePlan& first = plan.surfaces[uploaded.front().surface];
    const auto triangles = space::render_triangles(first.model->meshes.front().submeshes.front());
    const std::array<float, 3> centre = centroid(triangles);
    float radius = 0.0F;
    for (const space::Triangle& triangle : triangles) {
        for (const assets::Vec3f& corner : triangle) {
            const float dx = corner.x - centre[0];
            const float dy = corner.y - centre[1];
            const float dz = corner.z - centre[2];
            radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }
    const std::array<float, 3> eye = camera.eye;
    std::array<float, 3> forward{centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]};
    const float distance = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (!(distance > camera.near_plane * 4.0F) || !(radius > 0.0F)) return false;
    for (float& value : forward) value /= distance;
    // Screen axes of the look-at camera: right = up x back, up' = back x right.
    const std::array<float, 3> back{-forward[0], -forward[1], -forward[2]};
    const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    std::array<float, 3> right = cross(camera.up, back);
    const float right_length = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (!(right_length > 1.0e-6F)) return false;
    for (float& value : right) value /= right_length;
    const std::array<float, 3> up = cross(back, right);
    const bool full = control == "occluder-full";
    const float half = (full ? 1.5F : 0.3F) * radius * 0.5F;
    const std::array<float, 3> middle{eye[0] + forward[0] * distance * 0.5F, eye[1] + forward[1] * distance * 0.5F,
                                      eye[2] + forward[2] * distance * 0.5F};
    const auto corner = [&](const float sx, const float sy) {
        // Render basis -> source basis is the inverse of (x, y, z) -> (x, z, -y).
        const std::array<float, 3> render{middle[0] + (right[0] * sx + up[0] * sy) * half,
                                          middle[1] + (right[1] * sx + up[1] * sy) * half,
                                          middle[2] + (right[2] * sx + up[2] * sy) * half};
        return assets::Vec3f{render[0], -render[2], render[1]};
    };
    assets::Submesh submesh;
    submesh.shader = "eawr-space-occluder-control";
    for (const auto& [sx, sy, u, v] : {std::array<float, 4>{-1, 1, 0, 0}, std::array<float, 4>{1, 1, 1, 0},
                                       std::array<float, 4>{1, -1, 1, 1}, std::array<float, 4>{-1, -1, 0, 1}}) {
        assets::Vertex vertex;
        vertex.position = corner(sx, sy);
        vertex.normal = {0.0F, 0.0F, 1.0F};
        vertex.texcoord[0] = {u, v};
        submesh.vertices.push_back(vertex);
    }
    submesh.indices = {0, 1, 2, 0, 2, 3};
    assets::Model model;
    model.source.logical_path = "eawr-space-occluder-control";
    assets::Mesh mesh;
    mesh.name = "OccluderControl";
    mesh.submeshes.push_back(submesh);
    model.meshes.push_back(std::move(mesh));
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.format = assets::PixelFormat::rgba8;
    assets::MipLevel mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.bytes = {std::byte{40}, std::byte{200}, std::byte{60}, std::byte{255}};
    texture.mips.push_back(std::move(mip));
    const MaterialDescription material{
        .schema_version = MaterialDescription::current_schema_version,
        .route = MaterialRoute::modern_spatial,
        .pass = RenderPass::opaque,
        .program = std::string(occluder_shader),
        .technique = {},
        .pass_name = {},
        .bindings = {{"eawr_diffuse", std::string{"space-occluder-control"}}},
    };
    if (!renderer->upload(occluder_asset, model, texture, material)) return false;
    occluder_uploaded = true;
    occluder_mask = space::rasterize(camera, space::render_triangles(submesh));
    return true;
}

bool SpaceEnvironment::State::compose_receiver() {
    // A lit square below the line of sight to surface 0, lit by one fixed
    // directional light aimed from surface 0's centre through the receiver's
    // centre, so that a casting sky would shadow all of it. It is a control
    // fixture, not a source-backed space lighting environment.
    if (uploaded.empty()) return false;
    const space::SurfacePlan& first = plan.surfaces[uploaded.front().surface];
    const auto triangles = space::render_triangles(first.model->meshes.front().submeshes.front());
    const std::array<float, 3> centre = centroid(triangles);
    float radius = 0.0F;
    for (const space::Triangle& triangle : triangles) {
        for (const assets::Vec3f& corner : triangle) {
            const float dx = corner.x - centre[0];
            const float dy = corner.y - centre[1];
            const float dz = corner.z - centre[2];
            radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }
    const std::array<float, 3> eye = camera.eye;
    std::array<float, 3> forward{centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]};
    const float distance = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (!(distance > camera.near_plane * 4.0F) || !(radius > 0.0F)) return false;
    for (float& value : forward) value /= distance;
    const std::array<float, 3> back{-forward[0], -forward[1], -forward[2]};
    const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    std::array<float, 3> right = cross(camera.up, back);
    const float right_length = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (!(right_length > 1.0e-6F)) return false;
    for (float& value : right) value /= right_length;
    const std::array<float, 3> up = cross(back, right);
    // The eye-to-centre line has no component along `up`, so the plane
    // spanned by `right` and `forward`, dropped below that line, faces both
    // the camera (at 0.6 of the distance) and the light (through the centre).
    // Its half size keeps every parallel light ray inside surface 0.
    const float drop = distance * 0.3F;
    const float half = radius * 0.35F;
    std::array<float, 3> middle{};
    for (std::size_t axis = 0; axis < 3; ++axis) middle[axis] = eye[axis] + forward[axis] * distance * 0.6F - up[axis] * drop;
    std::array<float, 3> toward{centre[0] - middle[0], centre[1] - middle[1], centre[2] - middle[2]};
    const float toward_length = std::sqrt(toward[0] * toward[0] + toward[1] * toward[1] + toward[2] * toward[2]);
    for (float& value : toward) value /= toward_length;
    // Render basis -> source basis is the inverse of (x, y, z) -> (x, z, -y).
    const auto source = [](const std::array<float, 3>& render) { return assets::Vec3f{render[0], -render[2], render[1]}; };
    assets::Submesh submesh;
    submesh.shader = "eawr-space-lit-receiver-control";
    for (const auto& [sx, sy, u, v] : {std::array<float, 4>{-1, 1, 0, 0}, std::array<float, 4>{1, 1, 1, 0},
                                       std::array<float, 4>{1, -1, 1, 1}, std::array<float, 4>{-1, -1, 0, 1}}) {
        std::array<float, 3> render{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            render[axis] = middle[axis] + (right[axis] * sx + forward[axis] * sy) * half;
        }
        assets::Vertex vertex;
        vertex.position = source(render);
        vertex.normal = source(up);
        vertex.texcoord[0] = {u, v};
        submesh.vertices.push_back(vertex);
    }
    // Both windings: cull_back draws exactly the camera-facing copy.
    submesh.indices = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    assets::Model model;
    model.source.logical_path = "eawr-space-lit-receiver-control";
    assets::Mesh mesh;
    mesh.name = "LitReceiverControl";
    mesh.submeshes.push_back(submesh);
    model.meshes.push_back(std::move(mesh));
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.format = assets::PixelFormat::rgba8;
    assets::MipLevel mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.bytes = {std::byte{200}, std::byte{200}, std::byte{200}, std::byte{255}};
    texture.mips.push_back(std::move(mip));
    GodotRenderer::LightingState lighting;
    lighting.toward_light = toward;
    lighting.shadows = true;
    // Covers the receiver and surface 0 behind it.
    lighting.shadow_max_distance = distance * 2.0F;
    lighting.shadow_atlas_size = 4096;
    renderer->set_lighting(lighting);
    const MaterialDescription material{
        .schema_version = MaterialDescription::current_schema_version,
        .route = MaterialRoute::modern_spatial,
        .pass = RenderPass::opaque,
        .program = std::string(receiver_shader),
        .technique = {},
        .pass_name = {},
        .bindings = {{"eawr_diffuse", std::string{"space-lit-receiver-control"}}},
    };
    if (!renderer->upload(occluder_asset, model, texture, material)) return false;
    // The receiver must not shadow itself: only the sky's cast is measured.
    renderer->set_casts_shadows(occluder_asset, false);
    occluder_uploaded = true;
    foreground_role = "lit_receiver_control";
    occluder_mask = space::rasterize(camera, space::render_triangles(submesh));
    return true;
}

std::optional<int> SpaceEnvironment::State::lifecycle_step() {
    // The composed sky is submitted and drawn, so its instances exist when
    // its assets are released through the lease path and uploaded again.
    renderer->submit(configured_snapshot);
    if (++lifecycle_frames < lifecycle_cycle_frames) return std::nullopt;
    lifecycle_frames = 0;
    lifecycle_instances_before.push_back(renderer->instance_count());
    lifecycle_resources_before.push_back(renderer->resources().size());
    for (const Uploaded& item : uploaded) static_cast<void>(renderer->release(item.asset));
    lifecycle_instances_after.push_back(renderer->instance_count());
    lifecycle_released.push_back(renderer->resources().size());
    if (!upload_all(control)) {
        completed = true;
        static_cast<void>(give_up("sky surface re-upload failed in live lifecycle cycle "
            + std::to_string(lifecycle_released.size())));
        return 2;
    }
    lifecycle_reuploaded.push_back(renderer->resources().size());
    return std::nullopt;
}

bool SpaceEnvironment::ready(Node3D& host, const assets::Map& map, const vfs::Vfs& filesystem,
                             const assets::ObjectTypeCatalog& catalog, const bool catalog_loaded,
                             const std::string& catalog_failure) {
    State& state = *state_;
    // Only an explicit evidence control or fog selects the primary-sky harness.
    if (state.options.control.empty() && !state.options.fog) {
        state.view = std::make_unique<EnvironmentView>(state.options);
        return state.view->ready(host, map, filesystem, catalog, catalog_loaded, catalog_failure);
    }
    state.catalog_failure = catalog_failure;
    state.declared_placements = map.placements.size();
    if (!state.options.control.empty()) state.control = state.options.control;
    state.additive = parse_additive_control(state.control);
    if (state.additive.enabled) {
        if (!state.additive.failure.empty()) {
            return state.give_up("--eawr-space-control " + std::string(meshadditive_control) + ": "
                                 + state.additive.failure);
        }
        // The rest of the scene keeps its own controls: none or the occluder.
        // The report still names the labelled control (control_label()), and
        // the fixed-camera guard below still applies to it.
        state.control = state.additive.base;
    }
    constexpr std::array<std::string_view, 11> controls{"none", "drop-submission", "broken-shader", "occluder",
        "occluder-full", "reload-cycle", "sky-shadow", "sky-shadow-cast", "sky-shadow-caster", "sky-shadow-caster-cast",
        "meshgloss-light-probe"};
    if (std::find(controls.begin(), controls.end(), state.control) == controls.end()) {
        return state.give_up("--eawr-space-control must be none, drop-submission, broken-shader, occluder, "
                             "occluder-full, reload-cycle, sky-shadow, sky-shadow-cast, sky-shadow-caster, "
                             "sky-shadow-caster-cast or meshgloss-light-probe");
    }
    if (state.control == "meshgloss-light-probe") state.sky_light = light_probe_policy();
    if (state.options.fog) {
        // The fog join is evidenced on the fixed camera frame only, with the
        // shipped sky and no foreground control whose masks would overlap it.
        if (state.options.map_camera) {
            return state.give_up("space fog supports only the fixed --eawr-space-camera path, not a space map camera");
        }
        if (state.control != "none" || state.additive.enabled) {
            return state.give_up("space fog supports only --eawr-space-control none");
        }
        if (state.options.catalog == nullptr) {
            return state.give_up("space fog admission needs the XML catalog, which did not load: " + catalog_failure);
        }
    }
    // An unlocked space map camera draws from its authored pose; the fixed
    // camera, its controls and their evidence belong to --eawr-capture runs.
    const bool unlocked_camera = state.options.map_camera && state.options.capture_path.empty();
    if (unlocked_camera) {
        if (!state.options.camera.empty()) {
            return state.give_up("an unlocked space map camera takes its initial pose from --eawr-map-camera-config; "
                                 "--eawr-space-camera is the fixed capture camera and needs --eawr-capture");
        }
        if (state.control != "none" || state.additive.enabled) {
            return state.give_up("--eawr-space-control applies only to the fixed capture camera");
        }
    } else {
        if (state.options.camera.empty()) {
            state.camera_status = space::CameraStatus::malformed;
            return state.give_up("a space map needs --eawr-space-camera: no corpus camera is frozen for this slice, "
                                 "and none is inferred from source volumes or extents");
        }
        const FixedCamera defaults;
        const space::CameraParse parsed = space::parse_camera(state.options.camera, defaults.width, defaults.height);
        state.camera_status = parsed.status;
        state.camera = parsed.camera;
        if (parsed.status != space::CameraStatus::valid) {
            return state.give_up("--eawr-space-camera is " + std::string(space::to_string(parsed.status)));
        }
    }
    if (!unlocked_camera) {
        // The OS may give the window a different size from the fixed camera.
        // Every fixed-camera run reads the root viewport back and checks its
        // size, captured or not (the fog paint evidence writes no capture),
        // so pin that render target to the camera identity before the first
        // frame is drawn.
        pin_capture_viewport(*host.get_window(), state.camera.width, state.camera.height);
    }

    // Plan. All I/O stays here; the plan only sees loaded values.
    const assets::ObjectTypeRef* type = nullptr;
    if (!map.environments.empty() && map.environments.front().primary_sky) {
        type = assets::find_object_type(catalog, *map.environments.front().primary_sky);
    }
    std::map<std::string, space::TextureLookup> textures;
    space::PlanInput input;
    input.map = &map;
    input.sky_type = type;
    input.catalog_loaded = catalog_loaded;
    input.qualifications = space::qualifications();
    input.material_routes = space::material_routes();
    // Only the labelled meshadditive-synthetic control widens the routes.
    if (state.additive.enabled) input.material_routes = space::meshadditive_material_routes();
    input.model = [&](const std::string_view declared) {
        space::ModelLookup lookup;
        const auto path = probe_reference(filesystem, "data/art/models/", declared, model_suffixes);
        if (!path) {
            lookup.status = space::ModelLookup::Status::not_in_vfs;
            lookup.logical_path = std::string(declared);
            lookup.failure = "no data/art/models/ record for the declared model or its stem";
            return lookup;
        }
        lookup.logical_path = *path;
        auto bytes = filesystem.open(*path);
        auto model = assets::load_model(filesystem, *path);
        if (!bytes || !model) {
            lookup.status = space::ModelLookup::Status::failed_to_load;
            lookup.failure = !bytes ? core::format_diagnostic(bytes.error()) : core::format_diagnostic(model.error());
            return lookup;
        }
        lookup.sha256 = hash_bytes(bytes.value());
        lookup.status = space::ModelLookup::Status::resolved;
        lookup.model = std::move(model.value());
        return lookup;
    };
    input.texture = [&](const std::string_view declared) {
        const auto cached = textures.find(std::string(declared));
        if (cached != textures.end()) return cached->second;
        space::TextureLookup lookup;
        const auto path = probe_reference(filesystem, "data/art/textures/", declared, texture_suffixes);
        if (!path) {
            lookup.status = space::TextureLookup::Status::not_in_vfs;
            lookup.logical_path = std::string(declared);
            lookup.failure = "no data/art/textures/ record for the declared texture or its stem";
        } else {
            lookup.logical_path = *path;
            auto bytes = filesystem.open(*path);
            auto texture = assets::load_texture(filesystem, *path);
            if (!bytes || !texture) {
                lookup.status = space::TextureLookup::Status::failed_to_decode;
                lookup.failure = !bytes ? core::format_diagnostic(bytes.error())
                                        : core::format_diagnostic(texture.error());
            } else {
                lookup.status = space::TextureLookup::Status::resolved;
                lookup.sha256 = hash_bytes(bytes.value());
                lookup.texture = std::move(texture.value());
            }
        }
        textures.emplace(std::string(declared), lookup);
        return lookup;
    };
    state.plan = space::build_plan(input);
    state.plan_built = true;
    state.compiler.assign(state.plan.surfaces.size(), "not_attempted");
    state.upload_failure.assign(state.plan.surfaces.size(), "");
    const bool sky_blocked = state.plan.status != space::PlanStatus::ready;
    state.load_status = sky_blocked ? "blocked" : "loaded";
    if (sky_blocked) {
        return state.give_up("space primary sky plan is " + std::string(space::to_string(state.plan.status))
                                 + (state.plan.detail.empty() ? "" : ": " + state.plan.detail),
                             "space_blocked");
    }

    // One renderer on the host's own scenario; no second device and no
    // per-object scene nodes. The camera is live before any upload or frame.
    state.renderer = std::make_unique<GodotRenderer>(host, state.options.shaders);
    if (state.options.map_camera && !state.activate_camera(host)) return false;
    state.renderer->set_camera(state.camera);
    if (state.options.fog) {
        const auto enabled = state.renderer->enable_fog({{state.options.fog->stream(), state.options.fog->team()}});
        if (!enabled) return state.give_up(core::format_diagnostic(enabled.error()));
        state.fog_renderer_stream = state.options.fog->stream();
    }
    if (!sky_blocked && !state.upload_all(state.control)) {
        return state.give_up("sky surface upload failed: " + state.material_status);
    }
    const bool occluded = state.control == "occluder" || state.control == "occluder-full";
    if (occluded && !state.compose_occluder()) {
        return state.give_up("the foreground occluder control could not be composed");
    }
    if (shadow_control(state.control) && !state.compose_receiver()) {
        return state.give_up("the lit foreground receiver control could not be composed");
    }

    if (state.options.fog) {
        state.fog_units.emplace();
        if (!state.fog_units->compose(*state.renderer, map, state.options.map_sha256, filesystem,
                                      *state.options.catalog, state.options.fog_admit)) {
            return state.give_up(state.fog_units->failure());
        }
    }

    std::vector<sim::RenderInstance> base;
    if (state.occluder_uploaded) base.push_back({occluder_entity, occluder_asset, identity_transform()});
    std::vector<sim::RenderInstance> all = base;
    if (state.control == "drop-submission" && !state.uploaded.empty()) state.uploaded.back().submitted = false;
    for (const State::Uploaded& item : state.uploaded) {
        if (item.submitted) all.push_back({item.entity, item.asset, identity_transform()});
    }
    // With fog, every sky-only submission carries the selected immutable grid
    // set too, so the renderer binds it once and the sky evidence phases are
    // unchanged: the units are submitted only by the fog phases.
    const auto snapshot_of = [&](std::vector<sim::RenderInstance> instances) {
        return state.options.fog ? state.options.fog->snapshot(std::move(instances))
                                 : std::make_shared<const sim::RenderSnapshot>(0, std::move(instances));
    };
    state.sky_instances = all;
    state.configured_snapshot = snapshot_of(all);
    state.disabled_snapshot = snapshot_of(base);
    state.isolated_captures.assign(state.uploaded.size(), std::nullopt);
    for (std::size_t index = 0; index < state.uploaded.size(); ++index) {
        const State::Uploaded& item = state.uploaded[index];
        if (!item.submitted) continue;
        std::vector<sim::RenderInstance> alone = base;
        alone.push_back({item.entity, item.asset, identity_transform()});
        state.phases.push_back({"surface_" + std::to_string(index), snapshot_of(std::move(alone)), index});
    }
    state.phases.push_front({"sky_disabled", state.disabled_snapshot, std::nullopt});
    if (state.options.fog) {
        state.phases.push_back({"fog_units", nullptr, std::nullopt, 1});
        if (!state.options.fog_paint_evidence.empty()) {
            state.phases.push_back({"fog_units_painted", nullptr, std::nullopt, 2});
            state.phases.push_back({"fog_units_restored", nullptr, std::nullopt, 3});
        }
    }
    return true;
}
} // namespace eawr::presentation::godot_backend
