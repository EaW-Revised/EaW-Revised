#include "eawr/core/load_profile.hpp"
#include "renderer_upload_internal.hpp"
#include "eawr/presentation/shader_activation.hpp"
#include <cstring>

namespace eawr::presentation::godot_backend {
namespace {
struct ProgramPin final : shader_activation::Artifact {
    std::shared_ptr<const CachedShader> shader;
};
[[nodiscard]] Variant legacy_uniform_value(const legacy::UniformValue& value) {
    return std::visit([](const auto& item) -> Variant {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, float>) {
            return item;
        } else if constexpr (std::is_same_v<T, std::array<float, 2>>) {
            return Vector2(item[0], item[1]);
        } else if constexpr (std::is_same_v<T, std::array<float, 3>>) {
            return Vector3(item[0], item[1], item[2]);
        } else {
            return Vector4(item[0], item[1], item[2], item[3]);
        }
    }, value);
}

} // namespace

core::Result<std::shared_ptr<const shader_activation::Artifact>> GodotRenderer::prewarm_shader_material(
    const MaterialDescription& material) {
    using Result = core::Result<std::shared_ptr<const shader_activation::Artifact>>;
    const auto refuse = []() { return Result::failure({.code = std::string(diagnostic_codes::shader_compile_failed),
        .message = "translated material admission or offscreen program prewarm failed", .logical_path = std::nullopt, .line = std::nullopt, .column = std::nullopt, .source_id = std::nullopt}); };
    auto* rendering = RenderingServer::get_singleton();
    if (!rendering || !impl_ || material.route != MaterialRoute::modern_spatial || !validate_material(material)) return refuse();
    if (!impl_->shader_cache_) impl_->shader_cache_ = std::make_shared<GodotShaderCache>();
    Impl::Resource resource;
    struct Temporary final {
        RenderingServer& rendering;
        std::array<RID, 10U> rids{};
        ~Temporary() { for (const auto& rid : rids) if (rid.is_valid()) rendering.free_rid(rid); }
    } temporary{*rendering, {}};
    // This is an offscreen compiler exercise texture only. It is never
    // retained as an effect resource or substituted into a visible material.
    assets::Texture texture;
    texture.width = texture.height = 1U;
    texture.mips.push_back({1U, 1U, 4U, std::vector<std::byte>(4U, std::byte{255U})});
    resource.texture = Impl::create_texture(*rendering, texture);
    temporary.rids[0U] = resource.texture;
    if (!resource.texture.is_valid()) return refuse();
    Impl::MaterialRefusal refusal;
    const auto uploaded = impl_->upload_material(*rendering, material, resource, refusal);
    temporary.rids[1U] = resource.material;
    // CachedShader owns its shader; an uncached failure is ours to free.
    if (!resource.shared_shader && resource.shader.is_valid()) {
        rendering->free_rid(resource.shader);
        resource.shader = RID();
    }
    if (uploaded != Impl::MaterialUpload::success || !resource.shared_shader) return refuse();
    const auto mesh = rendering->mesh_create(); temporary.rids[2U] = mesh;
    const auto scenario = rendering->scenario_create(); temporary.rids[3U] = scenario;
    const auto camera = rendering->camera_create(); temporary.rids[4U] = camera;
    const auto viewport = rendering->viewport_create(); temporary.rids[5U] = viewport;
    const auto instance = rendering->instance_create(); temporary.rids[6U] = instance;
    const auto skinned_mesh = rendering->mesh_create(); temporary.rids[7U] = skinned_mesh;
    const auto skinned_instance = rendering->instance_create(); temporary.rids[8U] = skinned_instance;
    const auto skeleton = rendering->skeleton_create(); temporary.rids[9U] = skeleton;
    if (!mesh.is_valid() || !scenario.is_valid() || !camera.is_valid() || !viewport.is_valid() || !instance.is_valid()
        || !skinned_mesh.is_valid() || !skinned_instance.is_valid() || !skeleton.is_valid()) return refuse();
    Array arrays; arrays.resize(RenderingServer::ARRAY_MAX);
    PackedVector3Array positions, normals;
    PackedVector2Array uv;
    PackedColorArray colors;
    PackedFloat32Array tangents;
    positions.push_back(Vector3(-0.5F, -0.5F, -1.0F)); positions.push_back(Vector3(0.0F, 0.5F, -1.0F));
    positions.push_back(Vector3(0.5F, -0.5F, -1.0F));
    for (int i = 0; i < 3; ++i) {
        normals.push_back(Vector3(0.0F, 0.0F, 1.0F)); uv.push_back(Vector2()); colors.push_back(Color(1, 1, 1, 1));
        tangents.push_back(1.0F); tangents.push_back(0.0F); tangents.push_back(0.0F); tangents.push_back(1.0F);
    }
    arrays[RenderingServer::ARRAY_VERTEX] = positions; arrays[RenderingServer::ARRAY_NORMAL] = normals;
    arrays[RenderingServer::ARRAY_TANGENT] = tangents; arrays[RenderingServer::ARRAY_COLOR] = colors;
    arrays[RenderingServer::ARRAY_TEX_UV] = uv; arrays[RenderingServer::ARRAY_TEX_UV2] = uv;
    const std::array<float, 9U> coefficients{0, 0, 1, 0, 0, 1, 0, 0, 1};
    PackedByteArray binormals; binormals.resize(sizeof(coefficients));
    std::memcpy(binormals.ptrw(), coefficients.data(), sizeof(coefficients));
    arrays[RenderingServer::ARRAY_CUSTOM0] = binormals;
    const BitField<RenderingServer::ArrayFormat> format(static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGB_FLOAT)
        << RenderingServer::ARRAY_FORMAT_CUSTOM0_SHIFT);
    rendering->mesh_add_surface_from_arrays(mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays, {}, {}, format);
    // Source palette shaders remain outside the wrapper's scope. Ordinary
    // Godot-pre-skinned inputs still need their distinct vertex format warmed.
    PackedInt32Array bones; PackedFloat32Array weights;
    for (unsigned i = 0U; i < 12U; ++i) { bones.push_back(0); weights.push_back(i % 4U == 0U ? 1.0F : 0.0F); }
    arrays[RenderingServer::ARRAY_BONES] = bones; arrays[RenderingServer::ARRAY_WEIGHTS] = weights;
    rendering->mesh_add_surface_from_arrays(skinned_mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays, {}, {}, format);
    if (rendering->mesh_get_surface_count(mesh) != 1 || rendering->mesh_get_surface_count(skinned_mesh) != 1) return refuse();
    rendering->mesh_surface_set_material(mesh, 0, resource.material);
    rendering->mesh_surface_set_material(skinned_mesh, 0, resource.material);
    rendering->instance_set_base(instance, mesh); rendering->instance_set_scenario(instance, scenario);
    rendering->skeleton_allocate_data(skeleton, 1, false);
    rendering->skeleton_bone_set_transform(skeleton, 0, Transform3D());
    rendering->instance_set_base(skinned_instance, skinned_mesh); rendering->instance_set_scenario(skinned_instance, scenario);
    rendering->instance_attach_skeleton(skinned_instance, skeleton);
    rendering->camera_set_orthogonal(camera, 2.0F, 0.01F, 10.0F);
    rendering->viewport_set_size(viewport, 16, 16); rendering->viewport_set_scenario(viewport, scenario);
    rendering->viewport_attach_camera(viewport, camera); rendering->viewport_set_update_mode(viewport, RenderingServer::VIEWPORT_UPDATE_ALWAYS);
    rendering->viewport_set_active(viewport, true);
    bool drawn{};
    for (unsigned attempt = 0U; attempt < 2U && !drawn; ++attempt) {
        rendering->force_sync(); rendering->force_draw(false, 0.0); rendering->force_sync();
        drawn = rendering->viewport_get_render_info(viewport, RenderingServer::VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
            RenderingServer::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME) >= 2;
    }
    rendering->viewport_set_active(viewport, false);
    if (!drawn) return refuse();
    auto pin = std::make_shared<ProgramPin>(); pin->shader = std::move(resource.shared_shader);
    return Result::success(std::move(pin));
}

core::Result<void> GodotRenderer::set_translated_parameters(const sim::EntityId entity,
    const std::span<const MaterialBinding> parameters) {
    auto* rendering = RenderingServer::get_singleton();
    if (!rendering || !impl_) return core::Result<void>::failure({.code = std::string(diagnostic_codes::backend_unavailable),
        .message = "translated instance backend is unavailable", .logical_path = std::nullopt, .line = std::nullopt, .column = std::nullopt, .source_id = std::nullopt});
    const auto found = impl_->instances_.find(entity);
    if (found == impl_->instances_.end() || parameters.size() > 16U)
        return core::Result<void>::failure({.code = std::string(diagnostic_codes::invalid_material),
            .message = "translated instance or parameter budget is invalid", .logical_path = std::nullopt, .line = std::nullopt, .column = std::nullopt, .source_id = std::nullopt});
    for (const auto& parameter : parameters) if (std::holds_alternative<std::string>(parameter.value))
        return core::Result<void>::failure({.code = std::string(diagnostic_codes::invalid_material),
            .message = "translated instance parameters must be numeric", .logical_path = std::nullopt, .line = std::nullopt, .column = std::nullopt, .source_id = std::nullopt});
    for (const auto& parameter : parameters) {
        Variant value;
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::int32_t> || std::is_same_v<T, float>) value = item;
            else if constexpr (std::is_same_v<T, assets::Vec3f>) value = Vector3(item.x, item.y, item.z);
            else if constexpr (std::is_same_v<T, assets::Vec4f>) value = Vector4(item.x, item.y, item.z, item.w);
        }, parameter.value);
        rendering->instance_geometry_set_shader_parameter(found->second.rid, StringName(parameter.name.c_str()), value);
    }
    return core::Result<void>::success();
}

// Derives the shadow-receiving variant of a legacy adapter (P1-04). The
// adapters are unshaded because they evaluate their own irradiance; receiving
// a Godot directional shadow needs the lit pipeline, so `unshaded` is replaced
// by `ambient_light_disabled` and a light() function contributes exactly
// mix(shadow_floor, 1, ATTENUATION) per channel, making the lit result the
// unshaded result times the shadow term. An adapter without the expected
// render mode is reported rather than guessed at.
[[nodiscard]] std::optional<std::string> shadow_receiving_variant(const std::string_view source) {
    constexpr std::string_view unshaded = "render_mode unshaded,";
    const std::size_t position = source.find(unshaded);
    if (position == std::string_view::npos) return std::nullopt;
    std::string result(source);
    result.replace(position, unshaded.size(), "render_mode ambient_light_disabled,");
    result += "\nuniform vec3 eawr_shadow_floor = vec3(0.5);\n"
              "void light() {\n"
              "    DIFFUSE_LIGHT += mix(eawr_shadow_floor, vec3(1.0), ATTENUATION);\n"
              "}\n";
    return result;
}

// RenderingServer's reflected parameter list becomes empty when Godot's
// shading-language compiler rejects the source, so a uniform the source is
// known to declare reflects only after that compiler accepted it; neither
// engine stderr nor RID validity is consulted. The backend's per-variant
// GLSL compile by the driver happens later and is not observed here.
[[nodiscard]] bool GodotRenderer::Impl::shader_compiled(
    RenderingServer& rendering, const RID& shader, const StringName& declared) {
    core::load_profile::Scope scope(core::load_profile::Phase::shader_reflection);
    const TypedArray<Dictionary> parameters = rendering.get_shader_parameter_list(shader);
    for (int64_t index = 0; index < parameters.size(); ++index) {
        const Dictionary parameter = parameters[index];
        if (static_cast<StringName>(parameter.get("name", StringName())) == declared) return true;
    }
    return false;
}

// Modern sources declare nothing the renderer knows, so it injects an
// otherwise-unused eawr_compile_probe directly after the leading
// `shader_type spatial;`. Anchoring it there (not at the first `;`, which
// may sit in a comment or another shader type's declaration) means an
// observed probe also shows that compiler accepted the source as spatial.
[[nodiscard]] std::optional<std::string> GodotRenderer::Impl::with_compile_probe(std::string source) {
    const auto declaration_end = spatial_declaration_end(source);
    if (!declaration_end) return std::nullopt;
    source.insert(*declaration_end, "\nuniform float eawr_compile_probe;\n");
    return source;
}

// Legacy selectors reaching upload passed validate_material, so they name
// an adapter row; modern shader text never enters the message.
[[nodiscard]] std::string GodotRenderer::Impl::compile_failure_message(
    const MaterialDescription& source, const bool shadow_receiving, const sim::AssetId asset_id) {
    if (source.route == MaterialRoute::modern_spatial) {
        return "Godot rejected modern spatial shader compilation for asset "
            + std::to_string(asset_id);
    }
    return "Godot rejected the " + source.program + " " + source.technique + "/"
        + source.pass_name + (shadow_receiving ? " shadow-receiving" : "")
        + " adapter shader compilation for asset " + std::to_string(asset_id);
}

// The selector a refusal names; legacy selectors reaching upload passed
// validate_material, so they are known, bounded family names.
[[nodiscard]] std::string GodotRenderer::Impl::material_label(const MaterialDescription& source, const bool shadow_receiving) {
    if (source.route == MaterialRoute::modern_spatial) return "modern spatial";
    return source.program + " " + source.technique + "/" + source.pass_name
        + (shadow_receiving ? " shadow-receiving" : "");
}

[[nodiscard]] std::optional<std::string_view> GodotRenderer::Impl::legacy_shader_source(
    const MaterialDescription& source) {
    if (legacy::find_family(source) != nullptr) return legacy::shader_source(source);
    if (source.program == "MeshGloss.fx"
        && source.technique == "sph_t0" && source.pass_name == "sph_t0_p0") {
        return source.pass == RenderPass::transparent
            ? meshgloss_shader_alpha : meshgloss_shader_opaque;
    }
    // Fixed-function MESH/BATCHMESH passes (P1-11). The transparent-phase
    // effects are accepted only in the transparent pass and the opaque one
    // only in the opaque pass, so a caller cannot move either across.
    if ((source.program == "MeshAlpha.fx" || source.program == "MeshAlphaGloss.fx"
            || source.program == "BatchMeshAlpha.fx")
        && source.technique == "sph_t1" && source.pass_name == "sph_t1_p0") {
        return source.pass == RenderPass::transparent
            ? std::optional<std::string_view>{fixed_mesh_shader_alpha} : std::nullopt;
    }
    if (source.pass != RenderPass::opaque) return std::nullopt;
    if (source.program == "BatchMeshGloss.fx"
        && source.technique == "sph_t1" && source.pass_name == "sph_t1_p0") {
        return fixed_mesh_shader_opaque;
    }
    const bool rskin =
        (source.program == "RSkinBumpColorize.fx"
            && source.technique == "sph_t0" && source.pass_name == "sph_t0_p0")
        || (source.program == "RSkinGloss.fx"
            && source.technique == "sph_t1" && source.pass_name == "sph_t1_p0")
        || (source.program == "RSkinGlossColorize.fx"
            && source.technique == "sph_t0" && source.pass_name == "sph_t0_p0")
        || (source.program == "MeshBumpColorize.fx"
            && source.technique == "t0" && source.pass_name == "t0_p0");
    return rskin ? std::optional<std::string_view>{rskin_shader_opaque} : std::nullopt;
}

[[nodiscard]] GodotRenderer::Impl::MaterialUpload GodotRenderer::Impl::upload_material(
    RenderingServer& rendering,
    const MaterialDescription& source,
    Resource& target,
    MaterialRefusal& refusal) {
    core::load_profile::Scope load_scope(core::load_profile::Phase::material);
    std::string_view shader_source;
    std::optional<std::string> modern_source;
    std::string receiving_source;
    // Legacy adapters all declare BaseTexture; modern sources carry the
    // injected probe. Either must reflect before the material is built.
    StringName declared("BaseTexture");
    if (source.route == MaterialRoute::legacy_effect) {
        const auto selected = legacy_shader_source(source);
        // validate_material() is the public fail-closed gate, but keep the
        // upload selection independently exhaustive: a future selector
        // cannot silently inherit MeshGloss or the RSKIN adapter.
        if (!selected) return MaterialUpload::failed;
        shader_source = *selected;
        if (lighting_ && lighting_->shadows && legacy::receives_shadows(source)) {
            if (auto variant = shadow_receiving_variant(*selected)) {
                receiving_source = std::move(*variant);
                shader_source = receiving_source;
                target.shadow_receiving = true;
            } else {
                target.shadow_variant_failed = true;
            }
        }
    } else {
        modern_source = with_compile_probe(source.program.empty()
            ? std::string(modern_shader_smoke) : source.program);
        if (!modern_source) return MaterialUpload::shader_compile_failed;
        shader_source = *modern_source;
        declared = StringName("eawr_compile_probe");
    }
    const std::string uploaded = stored_output::backend_source(shader_source);
    if (shader_cache_) {
        const auto found = shader_cache_->impl_->entries.find(uploaded);
        if (found != shader_cache_->impl_->entries.end()) {
            target.shared_shader = found->second;
            target.shader = target.shared_shader->shader;
        }
    }
    std::vector<std::string> new_tokens;
    std::vector<ReflectedUniform> new_uniforms;
    if (!target.shared_shader) {
        {
            core::load_profile::Scope scope(core::load_profile::Phase::shader_compile);
            target.shader = rendering.shader_create();
            rendering.shader_set_code(target.shader, String::utf8(
                uploaded.data(), static_cast<int64_t>(uploaded.size())));
        }
        if (!shader_compiled(rendering, target.shader, declared)) {
            return MaterialUpload::shader_compile_failed;
        }
        {
            core::load_profile::Scope scope(core::load_profile::Phase::shader_reflection);
            new_tokens = shader_tokens(uploaded).value_or(std::vector<std::string>{});
            new_uniforms = reflected_uniforms(rendering, target.shader);
        }
        if (auto problem = portable_limit_problem(new_uniforms, new_tokens)) {
            refusal = {diagnostic_codes::shader_compile_failed, "shader", std::move(*problem)};
            return MaterialUpload::refused;
        }
    }
    // The exact immutable source already passed compiler/portable admission
    // on a hit. Its required probe must still match this upload's route.
    const auto& tokens = target.shared_shader ? target.shared_shader->tokens : new_tokens;
    const auto& uniforms = target.shared_shader ? target.shared_shader->uniforms : new_uniforms;
    const std::string required = utf8(String(declared));
    if (!std::any_of(uniforms.begin(), uniforms.end(), [&](const ReflectedUniform& uniform) {
            return uniform.name == required;
        })) {
        return MaterialUpload::shader_compile_failed;
    }
    // Bindings belong to this material, so admission runs on every upload,
    // including hits; no material RID exists yet. Invalid sources/refusals
    // never enter the cache.
    if (auto problem = binding_problem(source, uniforms, tokens)) {
        refusal = {diagnostic_codes::invalid_material, "material", std::move(*problem)};
        return MaterialUpload::refused;
    }
    if (shader_cache_ && !target.shared_shader) {
        auto& cache = *shader_cache_->impl_;
        if (cache.entries.size() < GodotShaderCache::entry_limit
            && uploaded.size() <= GodotShaderCache::source_byte_limit - cache.source_bytes) {
            auto accepted = std::make_shared<CachedShader>();
            accepted->shader = target.shader;
            accepted->uniforms = std::move(new_uniforms);
            accepted->tokens = std::move(new_tokens);
            cache.entries.emplace(uploaded, accepted);
            cache.source_bytes += uploaded.size();
            target.shared_shader = std::move(accepted);
        }
    }
    target.material = rendering.material_create();
    rendering.material_set_shader(target.material, target.shader);
    configure_material(rendering, source, target.material, target.texture, target.binding_textures);
    return target.shader.is_valid() && target.material.is_valid()
        ? MaterialUpload::success : MaterialUpload::failed;
}

// Everything a material needs besides its shader. The opt-in fog variant
// is configured by this same function, so both carry identical pass
// priority, bindings, legacy constants and lighting.
void GodotRenderer::Impl::configure_material(
    RenderingServer& rendering,
    const MaterialDescription& source,
    const RID& material,
    const RID& texture,
    const std::vector<std::pair<std::string, RID>>& binding_textures) const {
    core::load_profile::Scope scope(core::load_profile::Phase::material_binding);
    rendering.material_set_render_priority(material, render_pass_priority(source.pass));
    for (const MaterialBinding& binding : source.bindings) {
        Variant value;
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::int32_t> || std::is_same_v<T, float>) {
                value = item;
            } else if constexpr (std::is_same_v<T, assets::Vec3f>) {
                value = Vector3(item.x, item.y, item.z);
            } else if constexpr (std::is_same_v<T, assets::Vec4f>) {
                value = Vector4(item.x, item.y, item.z, item.w);
            } else if constexpr (std::is_same_v<T, std::string>) {
                const auto shared = shared_name(item)
                    ? shared_textures_.find(shared_key(item)) : shared_textures_.end();
                value = shared != shared_textures_.end() ? shared->second : texture;
                for (const auto& [name, bound] : binding_textures) {
                    if (name == binding.name) value = bound;
                }
            }
        }, binding.value);
        rendering.material_set_param(material, StringName(binding.name.c_str()), value);
    }
    if (source.route == MaterialRoute::legacy_effect) {
        rendering.material_set_param(material, StringName("BaseTexture"), texture);
        const Vector3 light_direction = Vector3(0.35F, 0.75F, 0.56F).normalized();
        const std::array<float, 3> light{
            static_cast<float>(light_direction.x), static_cast<float>(light_direction.y),
            static_cast<float>(light_direction.z)};
        rendering.material_set_param(material, StringName("eawr_sph_r"),
            godot_matrix(meshgloss_hemisphere_matrix(0.08F, 2.0F, light)));
        rendering.material_set_param(material, StringName("eawr_sph_g"),
            godot_matrix(meshgloss_hemisphere_matrix(0.08F, 1.88F, light)));
        rendering.material_set_param(material, StringName("eawr_sph_b"),
            godot_matrix(meshgloss_hemisphere_matrix(0.10F, 1.72F, light)));
        // Adapters that light the sun per pixel read the fill lights alone
        // (SPH_LIGHT_FILL) and the sun's diffuse colour: under the P0
        // constants the fill is the ambient term and the sun the directional.
        rendering.material_set_param(material, StringName("eawr_sph_fill_r"),
            godot_matrix(meshgloss_hemisphere_matrix(0.08F, 0.0F, light)));
        rendering.material_set_param(material, StringName("eawr_sph_fill_g"),
            godot_matrix(meshgloss_hemisphere_matrix(0.08F, 0.0F, light)));
        rendering.material_set_param(material, StringName("eawr_sph_fill_b"),
            godot_matrix(meshgloss_hemisphere_matrix(0.10F, 0.0F, light)));
        rendering.material_set_param(material, StringName("eawr_light_ambient"), Vector3(0.08F, 0.08F, 0.10F));
        rendering.material_set_param(material, StringName("eawr_light_diffuse"), Vector3(2.0F, 1.88F, 1.72F));
        rendering.material_set_param(material, StringName("eawr_eye_position"),
            Vector3(0.0F, 420.0F, 1050.0F));
        rendering.material_set_param(material, StringName("eawr_light_direction"), light_direction);
        rendering.material_set_param(material, StringName("eawr_light_specular"),
            Vector3(2.0F, 1.88F, 1.72F));
        rendering.material_set_param(material, StringName("eawr_light_scale"),
            Vector4(1.0F, 1.0F, 1.0F,
                source.pass == RenderPass::transparent && source.program == "MeshGloss.fx"
                    ? 0.5F : 1.0F));
        for (const legacy::Uniform& uniform : legacy::uniforms(source)) {
            rendering.material_set_param(material, StringName(String::utf8(uniform.name.data(),
                static_cast<int64_t>(uniform.name.size()))), legacy_uniform_value(uniform.value));
        }
    }
    // Without a lighting state the P0 constants above stand unchanged.
    apply_lighting_params(rendering, material);
    apply_wind_params(rendering, material, source);
}


} // namespace eawr::presentation::godot_backend
