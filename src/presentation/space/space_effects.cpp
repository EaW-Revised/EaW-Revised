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
std::string_view to_string(const EnvironmentEffectStatus status) noexcept {
    switch (status) {
    case EnvironmentEffectStatus::ready: return "ready";
    case EnvironmentEffectStatus::shader_unsupported: return "shader_unsupported";
    case EnvironmentEffectStatus::technique_unsupported: return "technique_unsupported";
    case EnvironmentEffectStatus::geometry_invalid: return "geometry_invalid";
    case EnvironmentEffectStatus::hierarchy_unsupported: return "hierarchy_unsupported";
    case EnvironmentEffectStatus::field_missing: return "field_missing";
    case EnvironmentEffectStatus::field_invalid: return "field_invalid";
    case EnvironmentEffectStatus::field_nonfinite: return "field_nonfinite";
    case EnvironmentEffectStatus::input_invalid: return "input_invalid";
    }
    return "unknown";
}

namespace {

struct EffectField final {
    std::string_view name;
    assets::ParameterKind kind;
    bool required;
    bool used;
};

constexpr std::array<EffectField, 10> planet_effect_fields{{
    {"BaseTexture", assets::ParameterKind::texture, true, true},
    {"CloudTexture", assets::ParameterKind::texture, true, true},
    {"Emissive", assets::ParameterKind::vector3, true, true},
    {"Diffuse", assets::ParameterKind::vector3, true, true},
    {"Specular", assets::ParameterKind::vector3, true, true},
    {"Atmosphere", assets::ParameterKind::vector4, false, false},
    {"CityColor", assets::ParameterKind::vector3, false, false},
    {"AtmospherePower", assets::ParameterKind::scalar, false, false},
    {"CloudScrollRate", assets::ParameterKind::scalar, false, false},
    {"NormalTexture", assets::ParameterKind::texture, false, false},
}};
constexpr EffectField planet_cloud_normal{"CloudNormalTexture", assets::ParameterKind::texture, false, false};
constexpr std::array<EffectField, 5> nebula_effect_fields{{
    {"BaseTexture", assets::ParameterKind::texture, true, true},
    {"UVScrollRate", assets::ParameterKind::vector4, true, true},
    {"DistortionScale", assets::ParameterKind::scalar, true, true},
    {"SFreq", assets::ParameterKind::scalar, true, true},
    {"TFreq", assets::ParameterKind::scalar, true, true},
}};

[[nodiscard]] bool effect_value_valid(const assets::MaterialParameter& value, const EffectField& field) {
    if (value.kind == assets::ParameterKind::vector4 && field.kind == assets::ParameterKind::vector3) {
        const auto* v = std::get_if<assets::Vec4f>(&value.value);
        return v != nullptr && finite4(*v);
    }
    if (value.kind != field.kind) return false;
    switch (value.kind) {
    case assets::ParameterKind::texture: {
        const auto* v = std::get_if<std::string>(&value.value);
        return v != nullptr && declared(std::optional<std::string>(*v));
    }
    case assets::ParameterKind::scalar: {
        const auto* v = std::get_if<float>(&value.value);
        return v != nullptr && std::isfinite(*v);
    }
    case assets::ParameterKind::vector3: {
        const auto* v = std::get_if<assets::Vec3f>(&value.value);
        return v != nullptr && finite(*v);
    }
    case assets::ParameterKind::vector4: {
        const auto* v = std::get_if<assets::Vec4f>(&value.value);
        return v != nullptr && finite4(*v);
    }
    case assets::ParameterKind::integer: return false;
    }
    return false;
}

[[nodiscard]] bool effect_nonfinite(const assets::MaterialParameter& value) noexcept {
    if (const auto* v = std::get_if<float>(&value.value)) return !std::isfinite(*v);
    if (const auto* v = std::get_if<assets::Vec3f>(&value.value)) return !finite(*v);
    if (const auto* v = std::get_if<assets::Vec4f>(&value.value)) return !finite4(*v);
    return false;
}

[[nodiscard]] assets::Vec3f effect_rgb(const assets::MaterialParameter& value) {
    if (const auto* v = std::get_if<assets::Vec3f>(&value.value)) return *v;
    const auto& v = std::get<assets::Vec4f>(value.value);
    return {v.x, v.y, v.z};
}

} // namespace

EnvironmentEffectPlan plan_environment_effect(const assets::Model& model, const std::size_t mesh_index,
    const std::size_t submesh_index, const std::string_view technique, const EnvironmentEffectInputs& inputs) {
    EnvironmentEffectPlan plan;
    if (mesh_index >= model.meshes.size() || submesh_index >= model.meshes[mesh_index].submeshes.size()) {
        plan.status = EnvironmentEffectStatus::geometry_invalid;
        plan.detail = "surface index is out of range";
        return plan;
    }
    const assets::Mesh& mesh = model.meshes[mesh_index];
    const assets::Submesh& submesh = mesh.submeshes[submesh_index];
    const bool planet = ieq(submesh.shader, "Planet.fx");
    const bool nebula = ieq(submesh.shader, "Nebula.fx");
    if (!planet && !nebula) return plan;
    plan.route_id = std::string(planet ? planet_route_id : nebula_route_id);
    plan.technique = std::string(technique);
    if (technique != "t0") {
        plan.status = EnvironmentEffectStatus::technique_unsupported;
        plan.detail = "only the explicitly selected t0 fallback is implemented";
        return plan;
    }
    plan.pass_name = "t0_p0";
    if (!mesh.visible || !submesh.skin_bones.empty() || !hierarchy_problem(model, mesh.bone).empty()
        || !hidden_bone(model, mesh.bone).empty()) {
        plan.status = EnvironmentEffectStatus::hierarchy_unsupported;
        plan.detail = "surface is hidden, skinned, or has an unsupported bone chain";
        return plan;
    }
    if ((planet && submesh.vertex_format != "alD3dVertNU2U3U3")
        || (nebula && submesh.vertex_format != "alD3dVertNU2C")
        || submesh.vertices.empty() || submesh.indices.empty() || submesh.indices.size() % 3 != 0
        || std::any_of(submesh.indices.begin(), submesh.indices.end(),
            [&](const std::uint16_t index) { return index >= submesh.vertices.size(); })
        || std::any_of(submesh.vertices.begin(), submesh.vertices.end(), [&](const assets::Vertex& vertex) {
            return !finite(vertex.position) || !finite(vertex.normal)
                || !(dot3(vertex.normal, vertex.normal) > 1.0e-12F)
                || !std::isfinite(vertex.texcoord[0].x) || !std::isfinite(vertex.texcoord[0].y)
                || (nebula && !finite4(vertex.color));
        })) {
        plan.status = EnvironmentEffectStatus::geometry_invalid;
        plan.detail = "surface has an unsupported vertex format or invalid indices, positions, normals, UVs, or colour";
        return plan;
    }
    if (inputs.id.empty() || !std::isfinite(inputs.time_seconds) || !finite4(inputs.light_scale)
        || !finite(inputs.light_direction) || !finite(inputs.ambient_light)
        || !finite(inputs.diffuse_light) || !finite(inputs.specular_light)
        || (planet && !(dot3(inputs.light_direction, inputs.light_direction) > 1.0e-12F))) {
        plan.status = EnvironmentEffectStatus::input_invalid;
        plan.detail = "named finite time and lighting inputs are required";
        return plan;
    }
    const auto lookup = [&](const std::string_view name) -> const EffectField* {
        if (planet) {
            for (const EffectField& field : planet_effect_fields) if (ieq(name, field.name)) return &field;
            if (ieq(name, planet_cloud_normal.name)) return &planet_cloud_normal;
        } else {
            for (const EffectField& field : nebula_effect_fields) if (ieq(name, field.name)) return &field;
        }
        return nullptr;
    };
    const auto count = [&](const std::string_view name) {
        return std::count_if(submesh.parameters.begin(), submesh.parameters.end(),
            [&](const assets::MaterialParameter& parameter) { return ieq(parameter.name, name); });
    };
    for (const assets::MaterialParameter& parameter : submesh.parameters) {
        const EffectField* field = lookup(parameter.name);
        if (field == nullptr || count(parameter.name) != 1) {
            plan.status = EnvironmentEffectStatus::field_invalid;
            plan.detail = "unknown or duplicate field " + parameter.name;
            return plan;
        }
        if (!effect_value_valid(parameter, *field)) {
            plan.status = effect_nonfinite(parameter)
                ? EnvironmentEffectStatus::field_nonfinite : EnvironmentEffectStatus::field_invalid;
            plan.detail = "ill-typed, empty, or non-finite field " + parameter.name;
            return plan;
        }
        const std::string canonical_name(field->name);
        plan.fields.push_back({canonical_name, std::string(kind_name(parameter.kind)),
            field->used ? "consumed" : "recorded_not_consumed", "selected t0 pass"});
        if (field->used && field->kind == assets::ParameterKind::texture) {
            plan.textures.emplace_back(canonical_name, std::get<std::string>(parameter.value));
            plan.material.bindings.push_back({canonical_name, parameter.value});
        } else if (field->used && field->kind == assets::ParameterKind::vector3) {
            plan.material.bindings.push_back({canonical_name, effect_rgb(parameter)});
        } else if (field->used) {
            plan.material.bindings.push_back({canonical_name, parameter.value});
        }
    }
    const auto check_required = [&](const auto& fields) {
        for (const EffectField& field : fields) {
            if (field.required && count(field.name) == 0) {
                plan.status = EnvironmentEffectStatus::field_missing;
                plan.detail = "required authored field " + std::string(field.name) + " is missing";
                return false;
            }
        }
        return true;
    };
    if (!(planet ? check_required(planet_effect_fields) : check_required(nebula_effect_fields))) return plan;
    std::stable_sort(plan.textures.begin(), plan.textures.end(), [](const auto& left, const auto& right) {
        return ieq(left.first, "BaseTexture") && !ieq(right.first, "BaseTexture");
    });
    plan.material.route = MaterialRoute::modern_spatial;
    plan.material.pass = planet && inputs.light_scale.w >= 1.0F ? RenderPass::opaque : RenderPass::transparent;
    plan.material.program = std::string(environment_effect_shader(plan.route_id, planet && inputs.light_scale.w < 1.0F));
    plan.material.bindings.push_back({"eawr_effect_time", inputs.time_seconds});
    plan.material.bindings.push_back({"eawr_effect_light_scale", inputs.light_scale});
    if (planet) {
        plan.material.bindings.push_back({"eawr_effect_light_direction", inputs.light_direction});
        plan.material.bindings.push_back({"eawr_effect_ambient", inputs.ambient_light});
        plan.material.bindings.push_back({"eawr_effect_diffuse", inputs.diffuse_light});
        plan.material.bindings.push_back({"eawr_effect_specular", inputs.specular_light});
    }
    plan.status = EnvironmentEffectStatus::ready;
    return plan;
}

std::string_view environment_effect_shader(const std::string_view route_id, const bool alpha_blended) noexcept {
    // Both programs are modern_spatial; neither uses the legacy effect table.
    // The Planet fallback stages blend base/cloud by cloud alpha, then apply
    // twice the explicitly supplied vertex light. Its unused atmospheric and
    // normal-map fields are recorded by the planner.
    //
    // The Nebula program ports Nebula.fx t0 (vs_1_1 / ps_1_1, ONE/ONE, cull
    // none, no z-write). Its wave offset uses the source-basis world position
    // and is added along the source object axes; render is (x, z, -y) of
    // source. vs_1_1 has no pow: the compiled effect squares the view-space
    // normal z three times, so the shell's far side (z < 0) gets the same edge
    // factor as its near side. GLSL pow() is undefined for a negative base and
    // drivers disagree (GL on RDNA3 returns 0 there), so the port squares too.
    // Each vertex colour is clamped to [0, 1] as oD0 is; the output alpha is
    // unused by the additive blend.
    static const std::string planet_body = R"GODOT(
uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D CloudTexture : filter_linear_mipmap, repeat_enable;
uniform vec3 Emissive;
uniform vec3 Diffuse;
uniform vec3 Specular;
uniform float eawr_effect_time;
uniform vec4 eawr_effect_light_scale;
uniform vec3 eawr_effect_light_direction;
uniform vec3 eawr_effect_ambient;
uniform vec3 eawr_effect_diffuse;
uniform vec3 eawr_effect_specular;
varying vec3 eawr_vertex_light;
void vertex() {
    vec3 world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
    vec3 toward_eye = normalize(CAMERA_POSITION_WORLD - world_position);
    vec3 toward_light = normalize(eawr_effect_light_direction);
    float facing = max(dot(world_normal, toward_light), 0.0);
    vec3 half_vector = normalize(toward_eye + toward_light);
    float highlight = pow(max(dot(world_normal, half_vector), 0.0), 32.0);
    eawr_vertex_light = Emissive + Diffuse * eawr_effect_ambient
        + Diffuse * eawr_effect_light_scale.rgb * eawr_effect_diffuse * facing
        + Specular * eawr_effect_specular * highlight;
}
void fragment() {
    vec4 base = texture(BaseTexture, UV);
    vec4 cloud = texture(CloudTexture, UV);
    ALBEDO = 2.0 * mix(base.rgb, cloud.rgb, cloud.a) * eawr_vertex_light;
)GODOT";
    static const std::string planet_opaque =
        "shader_type spatial;\nrender_mode unshaded, fog_disabled, cull_back, depth_draw_always;\n"
        + planet_body + "}\n";
    static const std::string planet_blended =
        "shader_type spatial;\nrender_mode unshaded, fog_disabled, cull_back, depth_draw_always, blend_mix;\n"
        + planet_body + "    ALPHA = eawr_effect_light_scale.a;\n}\n";
    static constexpr std::string_view nebula = R"GODOT(shader_type spatial;
render_mode unshaded, fog_disabled, cull_disabled, depth_draw_never, blend_add;
uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 UVScrollRate;
uniform float DistortionScale;
uniform float SFreq;
uniform float TFreq;
uniform float eawr_effect_time;
uniform vec4 eawr_effect_light_scale;
varying vec2 eawr_uv;
varying vec3 eawr_color;
void vertex() {
    vec3 world_render = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 world = vec3(world_render.x, -world_render.z, world_render.y);
    vec3 wave = sin(vec3(6.28) * fract(vec3(SFreq) * world + vec3(TFreq * eawr_effect_time)));
    VERTEX += DistortionScale * vec3(wave.x, wave.z, -wave.y);
    eawr_uv = UV + 3.0 * eawr_effect_time * UVScrollRate.xy;
    vec3 view_normal = normalize((VIEW_MATRIX * MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
    float z2 = view_normal.z * view_normal.z;
    float z4 = z2 * z2;
    float edge = 2.0 * z4 * z4 + 0.1;
    eawr_color = clamp(COLOR.rgb * eawr_effect_light_scale.rgb
        * eawr_effect_light_scale.a * edge, vec3(0.0), vec3(1.0));
}
void fragment() {
    ALBEDO = texture(BaseTexture, eawr_uv).rgb * eawr_color;
}
)GODOT";
    if (route_id == planet_route_id) return alpha_blended ? planet_blended : planet_opaque;
    if (route_id == nebula_route_id && !alpha_blended) return nebula;
    return {};
}

float environment_effect_time(const std::uint64_t tick) noexcept {
    return static_cast<float>(static_cast<double>(tick % effect_clock_wrap_ticks) * effect_clock_seconds_per_tick);
}

} // namespace eawr::presentation::space
