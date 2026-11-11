#include "renderer_internal.hpp"
#include "eawr/assets/geometry_stripped.hpp"
#include "eawr/presentation/shader_material_route.hpp"
#include "eawr/presentation/shader_material_activation.hpp"
#include <bit>
#include <cmath>

namespace eawr::presentation::godot_backend {
namespace {
namespace route = shader_material::routing;
using Engine = shader_bindings::EngineInput;
using Matrix = route::Matrix;
struct Current final {
    std::array<std::array<std::uint32_t, 16U>, 12U> words{};
    std::array<shader_evaluation::EngineInput, 12U> inputs{};
    std::size_t count{};
    void add(Engine input, std::span<const float> value) {
        if (count == inputs.size() || value.size() > words[0U].size()
            || std::any_of(value.begin(), value.end(), [](float item) { return !std::isfinite(item); })) return;
        auto& storage = words[count];
        for (std::size_t i = 0U; i < value.size(); ++i) storage[i] = std::bit_cast<std::uint32_t>(value[i]);
        inputs[count] = {input, {assets::EffectValueKind::floating, {storage.data(), value.size()}}}; ++count;
    }
    std::span<const shader_evaluation::EngineInput> values() const { return {inputs.data(), count}; }
};
Matrix matrix(const Transform3D& transform) {
    Matrix value{};
    for (int column = 0; column < 3; ++column) {
        const auto axis = transform.basis.get_column(column);
        for (int row = 0; row < 3; ++row) value[static_cast<std::size_t>(column * 4 + row)] = static_cast<float>(axis[row]);
    }
    value[12U] = static_cast<float>(transform.origin.x); value[13U] = static_cast<float>(transform.origin.y);
    value[14U] = static_cast<float>(transform.origin.z); value[15U] = 1.0F; return value;
}
bool invertible(const Transform3D& value) {
    const auto determinant = value.basis.determinant(); return determinant != 0 && std::isfinite(determinant);
}
void current(Current& output, const FixedCamera& camera, const Transform3D& camera_render, const Transform3D& object_render) {
    Basis source_to_render;
    source_to_render.set_column(0, Vector3(1, 0, 0)); source_to_render.set_column(1, Vector3(0, 0, -1));
    source_to_render.set_column(2, Vector3(0, 1, 0));
    const Transform3D basis(source_to_render, Vector3());
    const Transform3D camera_basis(Basis(Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, -1)), Vector3());
    const auto world = basis.affine_inverse() * object_render * basis;
    output.add(Engine::world, matrix(world));
    if (invertible(world)) output.add(Engine::world_inverse, matrix(world.affine_inverse()));
    const auto eye = basis.affine_inverse().xform(camera_render.origin);
    const std::array eye_value{static_cast<float>(eye.x), static_cast<float>(eye.y), static_cast<float>(eye.z), 1.0F};
    output.add(Engine::eye_position, eye_value);
    if (invertible(world)) {
        const auto local_eye = world.affine_inverse().xform(eye);
        const std::array value{static_cast<float>(local_eye.x), static_cast<float>(local_eye.y), static_cast<float>(local_eye.z), 1.0F};
        output.add(Engine::eye_object_position, value);
    }
    if (!invertible(camera_render)) return;
    const auto view = camera_basis * camera_render.affine_inverse() * basis;
    const auto world_view = view * world;
    output.add(Engine::view, matrix(view)); output.add(Engine::view_inverse, matrix(view.affine_inverse()));
    output.add(Engine::world_view, matrix(world_view));
    if (invertible(world_view)) output.add(Engine::world_view_inverse, matrix(world_view.affine_inverse()));
    const auto projection = route::source_projection(camera);
    if (!projection) return;
    output.add(Engine::projection, *projection);
    output.add(Engine::view_projection, route::multiply(*projection, matrix(view)));
    output.add(Engine::world_view_projection, route::multiply(*projection, matrix(world_view)));
    // The source uses row-vector matrices. Column-major render coefficients
    // are the logical row order of their transpose; P5 owns CTAB packing.
    // Unsourced lighting/fog/clock/skin inputs remain absent and refuse.
}
Variant numeric_value(const assets::ParameterValue& source) {
    Variant result;
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::int32_t> || std::is_same_v<T, float>) result = value;
        else if constexpr (std::is_same_v<T, assets::Vec3f>) result = Vector3(value.x, value.y, value.z);
        else if constexpr (std::is_same_v<T, assets::Vec4f>) result = Vector4(value.x, value.y, value.z, value.w);
    }, source); return result;
}
bool same_surface(const route::Surface& left, const route::Surface& right) {
    if (left.program != right.program || left.overrides.size() != right.overrides.size()
        || left.textures.size() != right.textures.size()) return false;
    for (std::size_t i = 0U; i < left.overrides.size(); ++i) {
        const auto& a = left.overrides[i]; const auto& b = right.overrides[i];
        if (a.parameter != b.parameter || a.count != b.count || a.kind != b.kind || a.words != b.words) return false;
    }
    for (std::size_t i = 0U; i < left.textures.size(); ++i)
        if (left.textures[i].uniform != right.textures[i].uniform || left.textures[i].resource != right.textures[i].resource) return false;
    return true;
}
} // namespace
struct GodotRenderer::Impl::TranslationState final {
    struct Prepared final {
        std::shared_ptr<route::Surface> surface;
        std::vector<StringName> material_names, instance_names;
    };
    std::shared_ptr<const shader_activation::Generation> generation;
    ShaderTextureResolver textures;
    FixedCamera camera;
    std::map<sim::AssetId, Prepared> surfaces;
    TranslationWork work;
};
void GodotRenderer::set_shader_generation(std::shared_ptr<const shader_activation::Generation> generation,
    ShaderTextureResolver textures) {
    if (!impl_->resources_.empty()) {
        impl_->diagnostics_.push({.code = "EAWR-SHADER-ROUTE-0002", .message = "shader generation must be installed before surface uploads", .logical_path = {}, .line = {}, .column = {}, .source_id = {}});
        return;
    }
    if (!generation) { impl_->translation_.reset(); return; }
    auto state = std::make_shared<Impl::TranslationState>(); state->generation = std::move(generation);
    state->textures = std::move(textures); impl_->translation_ = std::move(state);
}
bool GodotRenderer::admitted_shader(std::string_view shader) const {
    if (!impl_->translation_) return false;
    const auto identity = route::effect_identity(shader);
    return identity && shader_material::find(*impl_->translation_->generation, identity.value());
}
GodotRenderer::TranslationWork GodotRenderer::translation_work() const {
    return impl_->translation_ ? impl_->translation_->work : TranslationWork{};
}
bool GodotRenderer::usable_shader(const assets::Submesh& surface) {
    if (!impl_->translation_) return false;
    std::vector<BindingTexture> textures;
    auto prepared = impl_->prepare_translation(surface, textures);
    return prepared && bool(prepared.value());
}
bool GodotRenderer::shader_translation_enabled() const { return bool(impl_->translation_); }
bool GodotRenderer::Impl::batch_translated_asset(const sim::AssetId asset) const {
    return translation_ && translation_->surfaces.contains(asset);
}
bool GodotRenderer::translated_asset(const sim::AssetId asset) const {
    return impl_->batch_translated_asset(asset);
}
void GodotRenderer::Impl::set_translation_camera(const FixedCamera& camera) { translation_->camera = camera; }
void GodotRenderer::Impl::retire_translation(const sim::AssetId asset) {
    if (translation_->surfaces.erase(asset)) batch_mark_asset(asset);
}
core::Result<std::unique_ptr<route::Surface>> GodotRenderer::Impl::prepare_translation(
    const assets::Submesh& surface, std::vector<GodotRenderer::BindingTexture>& resolved,
    std::span<const MaterialBinding> overrides) {
    using Result = core::Result<std::unique_ptr<route::Surface>>;
    const auto identity = route::effect_identity(surface.shader);
    if (!identity) return Result::failure(identity.error());
    const auto* resident = shader_material::find(*translation_->generation, identity.value());
    if (!resident) return Result::success({});
    const auto refusal = route::provider_refusal(resident->program, bool(fog_), !surface.skin_bones.empty());
    if (refusal == route::ProviderRefusal::fog_variant) return Result::failure({.code = "EAWR-SHADER-ROUTE-0003",
        .message = "current fog route has no admitted native material variant", .logical_path = {}, .line = {}, .column = {}, .source_id = {}});
    if (refusal == route::ProviderRefusal::vertex_attributes) return Result::failure({.code = "EAWR-SHADER-ROUTE-0005",
        .message = "authored vertex vectors, colour or secondary UV payload is not qualified for native materials", .logical_path = {}, .line = {}, .column = {}, .source_id = {}});
    if (refusal == route::ProviderRefusal::skinning) return Result::failure({.code = "EAWR-SHADER-ROUTE-0006",
        .message = "source palette skinning and its vertex arithmetic order are not qualified for native materials", .logical_path = {}, .line = {}, .column = {}, .source_id = {}});
    if (refusal == route::ProviderRefusal::colorization)
        return core::Result<std::unique_ptr<route::Surface>>::failure({.code = "EAWR-SHADER-ROUTE-0004",
            .message = "per-entity colorization parameter provider is not qualified for native materials", .logical_path = {}, .line = {}, .column = {}, .source_id = {}});
    Current initial; current(initial, translation_->camera, view_transform_, Transform3D());
    return route::bind(translation_->generation, surface.shader, surface.parameters, initial.values(),
        [&](std::string_view declared) -> std::optional<std::string> {
            auto loaded = translation_->textures ? translation_->textures(declared) : std::nullopt;
            if (!loaded) return {};
            const auto texture_identity = loaded->first;
            resolved.push_back({{}, std::move(loaded->second)}); return texture_identity;
        }, {}, overrides);
}
core::Result<bool> GodotRenderer::upload_shader_material(const sim::AssetId asset, const assets::Model& model) {
    if (!impl_->translation_ || assets::geometry_stripped(model)) return core::Result<bool>::success(false);
    // Every sampled native image is carried by a named binding. This unused
    // base slot cannot replace an absent authored or engine texture.
    assets::Texture unused;
    unused.width = 1U; unused.height = 1U;
    unused.mips.push_back({1U, 1U, 4U, std::vector<std::byte>(4U, std::byte{255U})});
    return impl_->upload_translation(asset, model, unused, {});
}
core::Result<bool> GodotRenderer::Impl::upload_translation(const sim::AssetId asset, const ModelView& model,
    const assets::Texture& texture, ModelUpload retained_model, const MaterialDescription* overrides) {
    using Result = core::Result<bool>;
    if (!translation_) return Result::success(false);
    const assets::Submesh* selected{};
    std::size_t surfaces{};
    for (const auto& mesh : model.meshes) if (mesh.visible) for (const auto& surface : mesh.submeshes) {
        selected = &surface; ++surfaces;
    }
    if (surfaces == 1U) {
        std::vector<GodotRenderer::BindingTexture> resolved;
        auto prepared = prepare_translation(*selected, resolved,
            overrides ? std::span<const MaterialBinding>{overrides->bindings} : std::span<const MaterialBinding>{});
        if (prepared && prepared.value()) {
            auto surface = std::move(prepared).value();
            for (std::size_t i = 0U; i < resolved.size(); ++i) resolved[i].binding = surface->textures[i].uniform;
            const auto prior = translation_->surfaces.find(asset);
            if (prior != translation_->surfaces.end()) {
                if (!same_surface(*surface, *prior->second.surface))
                    return Result::failure({.code = std::string(diagnostic_codes::duplicate_asset),
                        .message = "translated surface parameters changed under a live asset ID", .logical_path = {}, .line = {}, .column = {}, .source_id = {}});
                // View/projection inputs can change between repeated uploads.
                // Compare geometry and decoded images against the original
                // upload while checking authored overrides separately above.
                const auto existing = resources_.find(asset);
                if (existing == resources_.end())
                    return Result::failure({.code = std::string(diagnostic_codes::duplicate_asset),
                        .message = "translated asset retention has no renderer resource", .logical_path = {}, .line = {}, .column = {}, .source_id = {}});
                auto retained = upload_resource(asset, model, texture, existing->second.description, resolved, std::move(retained_model));
                return retained ? Result::success(true) : Result::failure(retained.error());
            }
            auto uploaded = upload_resource(asset, model, texture, surface->instance.material, resolved, retained_model);
            if (uploaded) {
                TranslationState::Prepared retained;
                for (const auto& binding : surface->instance.material.bindings) retained.material_names.emplace_back(binding.name.c_str());
                for (const auto& binding : surface->instance.parameters) retained.instance_names.emplace_back(binding.name.c_str());
                retained.surface = std::move(surface);
                translation_->surfaces[asset] = std::move(retained); ++translation_->work.uploaded;
                batch_mark_asset(asset);
                return Result::success(true);
            }
            diagnostics_.push(uploaded.error());
        } else if (!prepared) diagnostics_.push(prepared.error());
    }
    return Result::success(false);
}
core::Result<void> GodotRenderer::Impl::upload(const sim::AssetId asset, const ModelView& model,
    const assets::Texture& texture, const MaterialDescription& description,
    std::span<const GodotRenderer::BindingTexture> binding_textures, ModelUpload retained_model) {
    if (!translation_) return upload_resource(asset, model, texture, description, binding_textures, std::move(retained_model));
    auto native = upload_translation(asset, model, texture, retained_model, &description);
    if (!native) return core::Result<void>::failure(native.error());
    if (native.value()) return core::Result<void>::success();
    // The caller's existing adapter is the only possible fallback. An empty
    // unknown-effect description fails ordinary validation; no generic shader
    // is selected here, and no compiler/cache is reachable from this module.
    auto fallback = upload_resource(asset, model, texture, description, binding_textures, std::move(retained_model));
    if (!fallback) ++translation_->work.refused;
    else if (description.program == "eawr_missing_effect") ++translation_->work.missing_green;
    else ++translation_->work.approximated;
    return fallback;
}
void GodotRenderer::Impl::update_translation(RenderingServer& rendering, const sim::EntityId entity,
    Instance& instance, Resource& resource) {
    const auto found = translation_->surfaces.find(instance.asset_id);
    if (found == translation_->surfaces.end()) return;
    auto& surface = *found->second.surface;
    Current inputs; current(inputs, translation_->camera, view_transform_, instance.object_transform);
    const bool previously_valid = surface.instance.valid;
    if (auto updated = surface.update(inputs.values()); !updated) {
        if (previously_valid) diagnostics_.push(updated.error());
        rendering.instance_set_visible(instance.rid, false);
        instance.applied_visible = false; return;
    }
    for (std::size_t index = 0U; index < surface.instance.material.bindings.size(); ++index) {
        const auto& binding = surface.instance.material.bindings[index];
        if (!std::holds_alternative<std::string>(binding.value)) rendering.material_set_param(resource.material,
            found->second.material_names[index], numeric_value(binding.value));
    }
    for (std::size_t index = 0U; index < surface.instance.parameters.size(); ++index)
        rendering.instance_geometry_set_shader_parameter(instance.rid, found->second.instance_names[index],
            numeric_value(surface.instance.parameters[index].value));
    static_cast<void>(entity);
    apply_visibility(rendering, instance, instance.visible);
}
} // namespace eawr::presentation::godot_backend
