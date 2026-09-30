#include "eawr/presentation/space/space.hpp"

#include "space_internal.hpp"

#include "eawr/presentation/terrain/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace eawr::presentation::space {
namespace {

[[nodiscard]] char fold(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

constexpr Rigid identity_rigid{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

[[nodiscard]] Rigid compose(const Rigid& parent, const Rigid& child) noexcept {
    Rigid result{};
    for (int axis = 0; axis < 3; ++axis) {
        const auto column = direction(parent, {child[axis], child[4 + axis], child[8 + axis]});
        result[axis] = column.x;
        result[4 + axis] = column.y;
        result[8 + axis] = column.z;
    }
    const auto translation = point(parent, {child[3], child[7], child[11]});
    result[3] = translation.x; result[7] = translation.y; result[11] = translation.z;
    return result;
}

[[nodiscard]] std::string_view origin_name(const assets::ImageOrigin origin) noexcept {
    return origin == assets::ImageOrigin::top_left ? "top_left" : "bottom_left";
}

// Degenerate input is not an oracle of the effect; the reference returns zero.
[[nodiscard]] assets::Vec3f normalized(const assets::Vec3f& value) noexcept {
    const float length = std::sqrt(dot3(value, value));
    if (!(length > 0.0F)) return {};
    return {value.x / length, value.y / length, value.z / length};
}

[[nodiscard]] float saturate(const float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

} // namespace

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

[[nodiscard]] bool declared(const std::optional<std::string>& value) noexcept {
    if (!value) return false;
    return std::any_of(value->begin(), value->end(),
        [](const char character) { return static_cast<unsigned char>(character) > ' '; });
}

[[nodiscard]] std::string_view kind_name(const assets::ParameterKind kind) noexcept {
    switch (kind) {
    case assets::ParameterKind::integer: return "integer";
    case assets::ParameterKind::scalar: return "scalar";
    case assets::ParameterKind::vector3: return "vector3";
    case assets::ParameterKind::vector4: return "vector4";
    case assets::ParameterKind::texture: return "texture";
    }
    return "unknown";
}

[[nodiscard]] bool finite(const assets::Vec3f& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] assets::Vec3f direction(const Rigid& m, const assets::Vec3f v) noexcept {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z,
            m[4] * v.x + m[5] * v.y + m[6] * v.z,
            m[8] * v.x + m[9] * v.y + m[10] * v.z};
}
[[nodiscard]] assets::Vec3f point(const Rigid& m, const assets::Vec3f v) noexcept {
    const auto r = direction(m, v);
    return {r.x + m[3], r.y + m[7], r.z + m[11]};
}
[[nodiscard]] bool proper_rigid(const Rigid& m) noexcept {
    if (!std::all_of(m.begin(), m.end(), [](const float v) { return std::isfinite(v); })) return false;
    const assets::Vec3f axes[3]{{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[2], m[6], m[10]}};
    const auto dot = [](const assets::Vec3f a, const assets::Vec3f b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    constexpr float tolerance = 1.0e-4F;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dot(axes[i], axes[i]) - 1.0F) > tolerance) return false;
        for (int j = 0; j < i; ++j) if (std::abs(dot(axes[i], axes[j])) > tolerance) return false;
    }
    const auto& a = axes[0]; const auto& b = axes[1]; const auto& c = axes[2];
    const float determinant = a.x * (b.y * c.z - b.z * c.y)
        + a.y * (b.z * c.x - b.x * c.z) + a.z * (b.x * c.y - b.y * c.x);
    return std::abs(determinant - 1.0F) <= tolerance;
}

// Empty when every bone from `start` to the root is a finite proper rigid
// transform. The bounded walk also catches cycles in hand-built models.
[[nodiscard]] std::string hierarchy_problem(const assets::Model& model, const std::int32_t start) {
    if (start < -1) return "bone index " + std::to_string(start) + " is invalid";
    std::int32_t current = start;
    for (std::size_t steps = 0; current >= 0; ++steps) {
        if (steps > model.bones.size()) return "the bone chain does not terminate";
        if (static_cast<std::size_t>(current) >= model.bones.size()) {
            return "bone index " + std::to_string(current) + " is out of range";
        }
        const assets::Bone& bone = model.bones[static_cast<std::size_t>(current)];
        if (bone.billboard != 0) {
            return "bone " + std::to_string(current) + " '" + bone.name + "' is a billboard (mode "
                + std::to_string(bone.billboard) + ")";
        }
        if (!proper_rigid(bone.relative_transform)) {
            return "bone " + std::to_string(current) + " '" + bone.name
                + "' has a non-finite, scaled, sheared, or reflected relative transform";
        }
        if (bone.parent < -1) {
            return "bone " + std::to_string(current) + " '" + bone.name + "' has an invalid parent index";
        }
        current = bone.parent;
    }
    return {};
}

[[nodiscard]] Rigid model_rigid(const assets::Model& model, const std::int32_t start) noexcept {
    Rigid result = identity_rigid;
    for (std::int32_t current = start; current >= 0; current = model.bones[static_cast<std::size_t>(current)].parent) {
        result = compose(model.bones[static_cast<std::size_t>(current)].relative_transform, result);
    }
    return result;
}

// Empty when every bone from `start` to the root is stored visible; otherwise
// names the first hidden one. A malformed chain is hierarchy_problem's cause.
[[nodiscard]] std::string hidden_bone(const assets::Model& model, const std::int32_t start) {
    std::int32_t current = start;
    for (std::size_t steps = 0; current >= 0 && steps <= model.bones.size(); ++steps) {
        if (static_cast<std::size_t>(current) >= model.bones.size()) return {};
        const assets::Bone& bone = model.bones[static_cast<std::size_t>(current)];
        if (!bone.visible) {
            return "bone " + std::to_string(current) + " '" + bone.name + "' on the mesh's chain is stored hidden; "
                "no reviewed bone-visibility rule lets the chain be dropped";
        }
        current = bone.parent;
    }
    return {};
}

void add_cause(SurfacePlan& surface, const SurfaceStatus cause) {
    if (std::find(surface.causes.begin(), surface.causes.end(), cause) == surface.causes.end()) {
        surface.causes.push_back(cause);
    }
}

void settle(SurfacePlan& surface) {
    surface.status = surface.causes.empty() ? SurfaceStatus::accepted : surface.causes.front();
}

[[nodiscard]] bool finite4(const assets::Vec4f& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

[[nodiscard]] float dot3(const assets::Vec3f& a, const assets::Vec3f& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

std::string_view to_string(const PlanStatus status) noexcept {
    switch (status) {
    case PlanStatus::ready: return "ready";
    case PlanStatus::map_kind_unknown: return "map_kind_unknown";
    case PlanStatus::not_space_map: return "not_space_map";
    case PlanStatus::terrain_present: return "terrain_present";
    case PlanStatus::map_not_semantically_complete: return "map_not_semantically_complete";
    case PlanStatus::environment_absent: return "environment_absent";
    case PlanStatus::primary_sky_undeclared: return "primary_sky_undeclared";
    case PlanStatus::catalog_unavailable: return "catalog_unavailable";
    case PlanStatus::object_not_in_catalog: return "object_not_in_catalog";
    case PlanStatus::object_declares_no_model: return "object_declares_no_model";
    case PlanStatus::model_not_in_vfs: return "model_not_in_vfs";
    case PlanStatus::model_failed_to_load: return "model_failed_to_load";
    case PlanStatus::no_drawable_surface: return "no_drawable_surface";
    case PlanStatus::surface_rejected: return "surface_rejected";
    }
    return "invalid";
}

std::string_view to_string(const SurfaceStatus status) noexcept {
    switch (status) {
    case SurfaceStatus::accepted: return "accepted";
    case SurfaceStatus::mesh_invisible: return "mesh_invisible";
    case SurfaceStatus::geometry_invalid: return "geometry_invalid";
    case SurfaceStatus::skinning_unsupported: return "skinning_unsupported";
    case SurfaceStatus::hierarchy_unsupported: return "hierarchy_unsupported";
    case SurfaceStatus::bone_visibility_unsupported: return "bone_visibility_unsupported";
    case SurfaceStatus::base_texture_missing: return "base_texture_missing";
    case SurfaceStatus::base_texture_duplicate: return "base_texture_duplicate";
    case SurfaceStatus::base_texture_wrong_type: return "base_texture_wrong_type";
    case SurfaceStatus::multitexture_required: return "multitexture_required";
    case SurfaceStatus::unconsumed_parameter: return "unconsumed_parameter";
    case SurfaceStatus::shader_not_qualified: return "shader_not_qualified";
    case SurfaceStatus::texture_not_in_vfs: return "texture_not_in_vfs";
    case SurfaceStatus::texture_failed_to_decode: return "texture_failed_to_decode";
    case SurfaceStatus::texture_unsupported: return "texture_unsupported";
    case SurfaceStatus::material_parameter_missing: return "material_parameter_missing";
    case SurfaceStatus::material_parameter_invalid: return "material_parameter_invalid";
    case SurfaceStatus::material_value_nonfinite: return "material_value_nonfinite";
    }
    return "invalid";
}

std::string_view to_string(const SkyMaterial material) noexcept {
    switch (material) {
    case SkyMaterial::opaque_diffuse: return "opaque_diffuse";
    case SkyMaterial::meshgloss: return "meshgloss";
    case SkyMaterial::meshadditive: return "meshadditive";
    }
    return "invalid";
}

std::span<const MaterialRouteRow> material_routes() noexcept {
    static const std::vector<MaterialRouteRow> rows{
        MaterialRouteRow{
            .shader = "MeshGloss.fx",
            .material = SkyMaterial::meshgloss,
            .provenance = "MeshGloss sph_t0/sph_t0_p0 observable arithmetic, "
                          "docs/behaviour/meshgloss-programmable.md (MeshGloss.fx SHA-256 d44c399e...abd0); "
                          "authored field values are consumed as parsed, never defaulted",
        },
    };
    return rows;
}

std::span<const FieldDisposition> meshgloss_fields() noexcept {
    static const std::vector<FieldDisposition> fields{
        {"BaseTexture", "texture", "consumed",
         "sampled linear with mips, wrap U/V; rgb is multiplied by twice the interpolated vertex diffuse and "
         "alpha weights the interpolated vertex specular; output alpha is the light-scale alpha, not texture alpha"},
        {"Emissive", "vector4", "consumed_rgb",
         "rgb is added to the per-vertex diffuse; w is recorded and not read by sph_t0_p0"},
        {"Diffuse", "vector4", "consumed_rgb",
         "rgb multiplies the per-vertex SPH irradiance and the light-scale rgb; w is recorded and not read; "
         "the light factor is zero under space-sky-unlit-v1"},
        {"Specular", "vector4", "consumed_rgb",
         "rgb multiplies the light-zero specular rgb and the per-vertex half-vector cosine raised to a fixed 16; "
         "w is recorded and not read; the light factor is zero under space-sky-unlit-v1"},
        {"Shininess", "scalar", "recorded_not_consumed",
         "must be present and finite; the selected path's exponent is a fixed 16, so the value does not change "
         "the output"},
    };
    return fields;
}

SkyLightPolicy unlit_sky_policy() {
    SkyLightPolicy policy;
    policy.id = "space-sky-unlit-v1";
    policy.light_scale = {1.0F, 1.0F, 1.0F, 1.0F};
    policy.cause = "the space slice composes no light for the sky: zero irradiance matrices, zero light-zero "
                   "specular and unit light scale; a slice policy, not a recovered original sky light rig";
    return policy;
}

float sph_irradiance(const std::array<float, 16>& matrix, const assets::Vec3f& normal) noexcept {
    const std::array<float, 4> vector{normal.x, normal.y, normal.z, 1.0F};
    float result = 0.0F;
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) result += vector[row] * matrix[column * 4 + row] * vector[column];
    }
    return result;
}

MeshGlossVertex meshgloss_vertex(const MeshGlossMaterial& material, const SkyLightPolicy& policy,
                                 const assets::Vec3f& normal, const assets::Vec3f& position,
                                 const assets::Vec3f& eye) noexcept {
    const float red = sph_irradiance(policy.sph[0], normal);
    const float green = sph_irradiance(policy.sph[1], normal);
    const float blue = sph_irradiance(policy.sph[2], normal);
    const assets::Vec3f toward_eye = normalized({eye.x - position.x, eye.y - position.y, eye.z - position.z});
    const assets::Vec3f half = normalized({toward_eye.x + policy.light_direction.x,
        toward_eye.y + policy.light_direction.y, toward_eye.z + policy.light_direction.z});
    const float factor = std::pow(std::max(dot3(normal, half), 0.0F), 16.0F);
    const assets::Vec4f& d = material.diffuse;
    const assets::Vec4f& e = material.emissive;
    const assets::Vec4f& s = material.specular;
    const assets::Vec4f& l = policy.light_scale;
    return MeshGlossVertex{
        .diffuse = {d.x * red * l.x + e.x, d.y * green * l.y + e.y, d.z * blue * l.z + e.z, l.w},
        .specular = {s.x * factor * policy.light_specular.x, s.y * factor * policy.light_specular.y,
                     s.z * factor * policy.light_specular.z},
    };
}

assets::Vec4f meshgloss_fragment(const MeshGlossVertex& vertex, const assets::Vec4f& texel) noexcept {
    return {2.0F * vertex.diffuse.x * texel.x + vertex.specular.x * texel.w,
            2.0F * vertex.diffuse.y * texel.y + vertex.specular.y * texel.w,
            2.0F * vertex.diffuse.z * texel.z + vertex.specular.z * texel.w,
            vertex.diffuse.w};
}

std::span<const MaterialRouteRow> meshadditive_material_routes() noexcept {
    static const std::vector<MaterialRouteRow> rows = [] {
        std::vector<MaterialRouteRow> result(material_routes().begin(), material_routes().end());
        result.push_back(MaterialRouteRow{
            .shader = "MeshAdditive.fx",
            .material = SkyMaterial::meshadditive,
            .provenance = "MeshAdditive t0/t0_p0 preferred-pass arithmetic and declared states, "
                          "docs/behaviour/meshadditive-sun-billboard.md A-01..A-10 (MeshAdditive.fx SHA-256 "
                          "35d84c23...eb88c); opt-in synthetic route for non-billboard surfaces; no retail "
                          "technique selection, mode-7 transform or original visual match is claimed",
        });
        return result;
    }();
    return rows;
}

std::span<const FieldDisposition> meshadditive_fields() noexcept {
    static const std::vector<FieldDisposition> fields{
        {"BaseTexture", "texture", "consumed",
         "sampled with wrap U/V and linear min/mag/mip filtering at the authored first UV plus "
         "TIME * UVScrollRate.xy; rgb and alpha are multiplied by the saturated vertex colour, whose alpha is 1, "
         "so texture alpha reaches only the output alpha and never scales the ONE/ONE rgb contribution"},
        {"Color", "vector3|vector4", "consumed_rgb",
         "rgb * LIGHT_SCALE.rgb * LIGHT_SCALE.a, saturated to [0, 1] per vertex; a float4's w is recorded and "
         "not read"},
        {"UVScrollRate", "vector4", "consumed_xy",
         "xy * TIME is added to the first UV before interpolation; z and w are recorded and not read"},
    };
    return fields;
}

MeshAdditiveInputs meshadditive_default_inputs() {
    MeshAdditiveInputs inputs;
    inputs.id = "space-sky-meshadditive-inputs-v1";
    inputs.time = 0.0F;
    inputs.light_scale = {1.0F, 1.0F, 1.0F, 1.0F};
    inputs.cause = "declared slice inputs: TIME frozen at 0 s and LIGHT_SCALE (1, 1, 1, 1), matching the unlit "
                   "sky policy's unit light scale; not the retail clock (G-04) or sky light scale (G-05)";
    return inputs;
}

bool finite_inputs(const MeshAdditiveInputs& inputs) noexcept {
    return std::isfinite(inputs.time) && finite4(inputs.light_scale);
}

std::span<const RenderStatePolicy> meshadditive_render_policy() noexcept {
    static const std::vector<RenderStatePolicy> states{
        {"blend", "one_one_add",
         "A-08: rgb = destination + fragment (source ONE, destination ONE, ADD), saturated by the 8-bit UNORM "
         "target"},
        {"depth_write", "off", "A-08: the pass never writes depth"},
        {"depth_test", "on_engine_default",
         "policy: tested against depth that non-sky opaque controls write (sky surfaces never write depth); the "
         "engine comparator is used and the A-08 equal-depth tie is not asserted (G-06)"},
        {"fog", "disabled",
         "policy: A-07 writes a vertex fog factor of 1; the original pipeline fog state is open (G-06)"},
        {"cull", "disabled",
         "policy: the sky preview's seen-from-inside state; the effect declares no cull state (G-06)"},
        {"srgb", "stored_values_on_srgb_output",
         "policy (A-09 declared-no-decode): texels are sampled without sRGB decode and the product is written as "
         "a stored value, decoded once after the transparent pass for the sRGB output (the gl_compatibility "
         "fallback instead compensates its approximate ALBEDO decode), so arithmetic and the additive blend act on "
         "stored values"},
        {"alpha", "rgb_only",
         "policy: the adapter keeps fragment alpha 1 so the engine's additive blend is exactly ONE/ONE on rgb; "
         "texture alpha never scales rgb (A-10), and the A-10 destination-alpha add is not reproduced (G-06)"},
        {"pass", "transparent", "after the opaque sky; additive composition is order-independent"},
    };
    return states;
}

assets::Vec2f meshadditive_uv(const assets::Vec2f& uv, const assets::Vec4f& rate, const float time) noexcept {
    return {uv.x + time * rate.x, uv.y + time * rate.y};
}

assets::Vec2f wrap_uv(const assets::Vec2f& uv) noexcept {
    return {uv.x - std::floor(uv.x), uv.y - std::floor(uv.y)};
}

assets::Vec4f meshadditive_vertex_color(const MeshAdditiveMaterial& material, const MeshAdditiveInputs& inputs) noexcept {
    const assets::Vec4f& c = material.color;
    const assets::Vec4f& l = inputs.light_scale;
    return {saturate(c.x * l.x * l.w), saturate(c.y * l.y * l.w), saturate(c.z * l.z * l.w), 1.0F};
}

assets::Vec4f meshadditive_fragment(const assets::Vec4f& vertex_color, const assets::Vec4f& texel) noexcept {
    return {texel.x * vertex_color.x, texel.y * vertex_color.y, texel.z * vertex_color.z, texel.w * vertex_color.w};
}

assets::Vec4f additive_blend(const assets::Vec4f& destination, const assets::Vec4f& fragment) noexcept {
    return {saturate(destination.x + fragment.x), saturate(destination.y + fragment.y),
            saturate(destination.z + fragment.z), saturate(destination.w + fragment.w)};
}

std::span<const Qualification> qualifications() noexcept {
    static const std::vector<Qualification> rows{
        Qualification{
            .shader = "EawrSyntheticOpaqueDiffuse.fx",
            .reviewed_unconsumed_parameters = {},
            .provenance = "invented shader identity of the original synthetic fixture "
                          "tests/assets/fixtures/space_environment_fixture.py; not an original shader",
        },
    };
    return rows;
}

std::size_t SkyPlan::accepted_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(surfaces.begin(), surfaces.end(),
        [](const SurfacePlan& surface) { return surface.status == SurfaceStatus::accepted; }));
}

EnvironmentSelection select_environment(const assets::Map& map) {
    EnvironmentSelection result;
    result.environment_count = map.environments.size();
    if (!map.kind) {
        result.status = PlanStatus::map_kind_unknown;
        return result;
    }
    if (*map.kind != assets::MapKind::space) {
        result.status = PlanStatus::not_space_map;
        return result;
    }
    if (map.terrain) {
        result.status = PlanStatus::terrain_present;
        return result;
    }
    if (!map.semantic_complete) {
        result.status = PlanStatus::map_not_semantically_complete;
        return result;
    }
    if (map.environments.empty()) {
        result.status = PlanStatus::environment_absent;
        return result;
    }
    const assets::EnvironmentDescriptor& selected = map.environments[supported_environment_index];
    result.environment_name = selected.name;
    if (selected.secondary_sky) {
        result.not_rendered.push_back({"secondary_sky", *selected.secondary_sky, "declared_not_rendered",
            "secondary sky composition (order, blend, transform, time) has no clean evidence; "
            "this slice renders the primary sky only"});
    }
    if (selected.cloud_texture) {
        result.not_rendered.push_back({"cloud_texture", *selected.cloud_texture, "declared_not_rendered",
            "a texture reference whose geometry and composition are unknown; no cloud layer is drawn"});
    }
    result.not_rendered.push_back({"planet", "", "unexamined_blocked",
        "no typed TED field names Planet content and no clean evidence pins PLANET techniques, passes or "
        "parameters; placements are not classified as environment objects in this slice"});
    result.not_rendered.push_back({"nebula", "", "unexamined_blocked",
        "no typed TED field names Nebula content and no clean evidence pins NEBULA techniques, passes or "
        "parameters; nothing is drawn or accepted as unused"});
    for (std::size_t index = 0; index < map.environments.size(); ++index) {
        if (index == supported_environment_index) continue;
        result.not_rendered.push_back({"environment_" + std::to_string(index),
            map.environments[index].name.value_or(""), "not_selected",
            "only environment 0 is supported as a slice policy; the original environment selection is unknown"});
    }
    if (!declared(selected.primary_sky)) {
        result.status = PlanStatus::primary_sky_undeclared;
        return result;
    }
    result.primary_sky = *selected.primary_sky;
    result.status = PlanStatus::ready;
    return result;
}

ModelSelection select_model(const assets::ObjectTypeRef* type, const bool catalog_loaded) {
    ModelSelection result;
    if (type == nullptr) {
        result.status = catalog_loaded ? PlanStatus::object_not_in_catalog : PlanStatus::catalog_unavailable;
        return result;
    }
    result.object_source_path = type->source.logical_path;
    result.object_source_id = type->source.source_id;
    result.object_layer_id = type->source.layer_id;
    result.object_line = type->source.line;
    const std::pair<const std::optional<std::string>*, std::string_view> order[] = {
        {&type->space_model_name, "Space_Model_Name"},
        {&type->land_model_name, "Land_Model_Name"},
        {&type->model_name, "Model_Name"},
    };
    for (const auto& [name, tag] : order) {
        if (!declared(*name)) continue;
        result.declared_name = **name;
        result.declared_tag = std::string(tag);
        result.status = PlanStatus::ready;
        return result;
    }
    result.status = PlanStatus::object_declares_no_model;
    return result;
}

SkyPlan build_plan(const PlanInput& input) {
    SkyPlan plan;
    plan.visibility = input.visibility;
    if (input.map == nullptr) {
        plan.status = PlanStatus::map_kind_unknown;
        plan.detail = "no map was supplied";
        return plan;
    }
    plan.environment = select_environment(*input.map);
    if (plan.environment.status != PlanStatus::ready) {
        plan.status = plan.environment.status;
        return plan;
    }
    plan.model = select_model(input.sky_type, input.catalog_loaded);
    if (plan.model.status != PlanStatus::ready) {
        plan.status = plan.model.status;
        return plan;
    }
    if (!input.model) {
        plan.status = PlanStatus::model_not_in_vfs;
        plan.detail = "no model lookup was supplied";
        return plan;
    }
    ModelLookup lookup = input.model(plan.model.declared_name);
    plan.model_logical_path = lookup.logical_path;
    plan.model_sha256 = lookup.sha256;
    if (lookup.status == ModelLookup::Status::not_in_vfs) {
        plan.status = PlanStatus::model_not_in_vfs;
        plan.detail = lookup.failure;
        return plan;
    }
    if (lookup.status != ModelLookup::Status::resolved || !lookup.model) {
        plan.status = PlanStatus::model_failed_to_load;
        plan.detail = lookup.failure;
        return plan;
    }
    const assets::Model& model = *lookup.model;
    plan.model_source_id = model.source.source_id;
    plan.model_layer_id = model.source.layer_id;
    plan.model_bones = model.bones.size();
    plan.model_meshes = model.meshes.size();
    plan.surfaces = plan_surfaces(model, input.qualifications, input.material_routes);

    for (SurfacePlan& surface : plan.surfaces) {
        // Texture identity is recorded for every visible surface with one
        // well-typed BaseTexture, so a rejected real surface still pins what
        // it names; only an accepted surface keeps pixels and geometry.
        const bool texture_declared = surface.status != SurfaceStatus::mesh_invisible
            && !surface.base_texture.empty()
            && std::find(surface.causes.begin(), surface.causes.end(), SurfaceStatus::base_texture_duplicate)
                == surface.causes.end();
        if (!texture_declared) continue;
        if (!input.texture) {
            add_cause(surface, SurfaceStatus::texture_not_in_vfs);
            settle(surface);
            continue;
        }
        TextureLookup texture = input.texture(surface.base_texture);
        surface.texture_identity.logical_path = texture.logical_path;
        surface.texture_identity.sha256 = texture.sha256;
        if (texture.status == TextureLookup::Status::not_in_vfs) {
            add_cause(surface, SurfaceStatus::texture_not_in_vfs);
        } else if (texture.status != TextureLookup::Status::resolved || !texture.texture) {
            add_cause(surface, SurfaceStatus::texture_failed_to_decode);
            if (surface.detail.empty()) surface.detail = texture.failure;
        } else {
            const assets::Texture& decoded = *texture.texture;
            surface.texture_identity.source_id = decoded.source.source_id;
            surface.texture_identity.layer_id = decoded.source.layer_id;
            surface.texture_identity.format = std::string(assets::to_string(decoded.format));
            surface.texture_identity.source_origin = std::string(origin_name(decoded.source_origin));
            surface.texture_identity.has_alpha = decoded.has_alpha;
            surface.texture_identity.width = decoded.width;
            surface.texture_identity.height = decoded.height;
            surface.texture_identity.mip_count = decoded.mips.size();
            auto normalized = normalize_texture(decoded);
            if (!normalized) {
                add_cause(surface, SurfaceStatus::texture_unsupported);
                if (surface.detail.empty()) surface.detail = normalized.error().message;
            } else if (surface.causes.empty()) {
                surface.texture = std::move(normalized.value());
            }
        }
        settle(surface);
        if (surface.status != SurfaceStatus::accepted) {
            surface.texture.reset();
            continue;
        }
        assets::Model single;
        single.source = model.source;
        assets::Mesh mesh = model.meshes[surface.mesh_index];
        // Bake the visible rigid bind chain in source coordinates. The Godot
        // upload remains the sole source-to-render basis conversion.
        const Rigid transform = model_rigid(model, mesh.bone);
        mesh.bone = -1;
        mesh.submeshes = {model.meshes[surface.mesh_index].submeshes[surface.submesh_index]};
        for (assets::Vertex& vertex : mesh.submeshes.front().vertices) {
            vertex.position = point(transform, vertex.position);
            vertex.normal = direction(transform, vertex.normal);
            vertex.tangent = direction(transform, vertex.tangent);
            vertex.binormal = direction(transform, vertex.binormal);
        }
        const auto& vertices = mesh.submeshes.front().vertices;
        mesh.bounds_min = mesh.bounds_max = vertices.front().position;
        for (const assets::Vertex& vertex : vertices) {
            mesh.bounds_min.x = std::min(mesh.bounds_min.x, vertex.position.x);
            mesh.bounds_min.y = std::min(mesh.bounds_min.y, vertex.position.y);
            mesh.bounds_min.z = std::min(mesh.bounds_min.z, vertex.position.z);
            mesh.bounds_max.x = std::max(mesh.bounds_max.x, vertex.position.x);
            mesh.bounds_max.y = std::max(mesh.bounds_max.y, vertex.position.y);
            mesh.bounds_max.z = std::max(mesh.bounds_max.z, vertex.position.z);
        }
        single.meshes.push_back(std::move(mesh));
        surface.model = std::move(single);
    }

    std::size_t drawable = 0;
    const SurfacePlan* rejected = nullptr;
    for (const SurfacePlan& surface : plan.surfaces) {
        if (surface.status == SurfaceStatus::accepted) ++drawable;
        else if (surface.status != SurfaceStatus::mesh_invisible && rejected == nullptr) rejected = &surface;
    }
    if (rejected != nullptr) {
        plan.status = PlanStatus::surface_rejected;
        plan.detail = "mesh " + std::to_string(rejected->mesh_index) + " submesh "
            + std::to_string(rejected->submesh_index) + " (" + rejected->original_shader + "): "
            + std::string(to_string(rejected->status));
    } else if (drawable == 0) {
        plan.status = PlanStatus::no_drawable_surface;
        plan.detail = "the sky model has no visible surface";
    } else {
        plan.status = PlanStatus::ready;
    }
    return plan;
}

} // namespace eawr::presentation::space
