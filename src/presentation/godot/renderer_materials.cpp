#include "eawr/core/load_profile.hpp"
#include "renderer_upload_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {
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
