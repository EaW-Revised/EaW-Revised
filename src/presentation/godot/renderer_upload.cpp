#include "eawr/core/load_profile.hpp"
#include "renderer_upload_internal.hpp"

namespace eawr::presentation::godot_backend {
[[nodiscard]] core::Result<void> GodotRenderer::Impl::upload(
    const sim::AssetId asset_id,
    const assets::Model& model,
    const assets::Texture& texture,
    const MaterialDescription& description,
    const std::span<const GodotRenderer::BindingTexture> binding_textures) {
    // Each per-binding texture names a distinct texture-valued binding.
    std::vector<detail::NamedTexture> named;
    for (const GodotRenderer::BindingTexture& item : binding_textures) {
        const bool bound = std::any_of(description.bindings.begin(), description.bindings.end(),
            [&](const MaterialBinding& binding) {
                return binding.name == item.binding && std::holds_alternative<std::string>(binding.value);
            });
        const bool repeated = std::any_of(named.begin(), named.end(),
            [&](const detail::NamedTexture& seen) { return seen.binding == item.binding; });
        if (!bound || repeated) {
            return failure(diagnostic_codes::invalid_material,
                "per-binding texture '" + item.binding + "' of asset " + std::to_string(asset_id)
                    + (repeated ? " is given twice" : " names no texture-valued material binding"));
        }
        named.push_back({item.binding, &item.texture});
    }
    if (const auto existing = resources_.find(asset_id); existing != resources_.end()) {
        // Different content under a live asset ID would silently keep the
        // old RIDs; replacing an asset is an explicit release then upload.
        const detail::UploadIdentity identity = detail::upload_identity(model, texture, description, named);
        if (existing->second.identity.get() != identity) {
            return failure(diagnostic_codes::duplicate_asset,
                "renderer asset " + std::to_string(asset_id)
                    + " is already uploaded with different model, texture or material content");
        }
        static_cast<void>(leases_.retain(asset_id));
        return core::Result<void>::success();
    }
    if (auto valid = validate_material(description); !valid) {
        // Validation includes the modern spatial preflight. Preserve its
        // exact bounded core Diagnostic in the renderer-visible history.
        diagnostics_.push(valid.error());
        return valid;
    }
    // A legacy/ family's extra samplers read only their own texture; the
    // base texture is never substituted for one (gate MULTITEX-01).
    for (const legacy::TextureBinding& sampled : legacy::binding_textures(description)) {
        const std::string_view required = sampled.name;
        const bool carried = std::any_of(named.begin(), named.end(),
            [&](const detail::NamedTexture& item) { return item.binding == required; });
        if (!carried) {
            return failure(diagnostic_codes::invalid_material, material_label(description, false)
                + " material for asset " + std::to_string(asset_id) + " binds " + legacy::bindings::echoed(required)
                + ", but the upload carries no per-binding texture for it (gate MULTITEX-01)");
        }
    }
    for (const MaterialBinding& binding : description.bindings) {
        const auto* name = std::get_if<std::string>(&binding.value);
        if (name != nullptr && shared_name(*name) && !shared_textures_.contains(shared_key(*name))) {
            return failure(diagnostic_codes::upload_failed,
                "renderer asset " + std::to_string(asset_id) + " binds unregistered shared texture '"
                    + shared_key(*name) + "'");
        }
    }
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !scenario_.is_valid()) {
        return failure(diagnostic_codes::backend_unavailable,
            "Godot renderer was not initialized with a valid scenario");
    }

    Resource resource;
    if (!upload_identity_pool_) upload_identity_pool_ = std::make_unique<detail::UploadIdentityPool>();
    resource.identity = upload_identity_pool_->submit(
        detail::upload_identity_bytes(model, texture, description, named));
    MaterialRefusal refusal;
    bool texture_uploaded = upload_texture(*rendering, texture, resource.texture);
    for (const GodotRenderer::BindingTexture& item : binding_textures) {
        if (!texture_uploaded) break;
        RID bound;
        texture_uploaded = upload_texture(*rendering, item.texture, bound);
        if (bound.is_valid()) resource.binding_textures.emplace_back(item.binding, bound);
    }
    const MaterialUpload material_uploaded = texture_uploaded
        ? upload_material(*rendering, description, resource, refusal)
        : MaterialUpload::failed;
    if (!texture_uploaded || material_uploaded != MaterialUpload::success
        || !upload_mesh(*rendering, model, resource, legacy::authored_binormals(description))) {
        free_resource(*rendering, resource);
        if (material_uploaded == MaterialUpload::shader_compile_failed) {
            return failure(diagnostic_codes::shader_compile_failed,
                compile_failure_message(description, resource.shadow_receiving, asset_id));
        }
        if (material_uploaded == MaterialUpload::refused) {
            return failure(refusal.code, material_label(description, resource.shadow_receiving) + " "
                + std::string(refusal.subject) + " for asset " + std::to_string(asset_id) + " " + refusal.reason);
        }
        return failure(diagnostic_codes::upload_failed,
            "Godot rejected mesh, texture, or material resources for asset "
                + std::to_string(asset_id));
    }
    // Counted only for a resource that exists; a failed mesh upload after
    // a compiled shadow variant frees that variant and leaves no count.
    if (resource.shadow_receiving) ++shadow_receiving_;
    if (resource.shadow_variant_failed) ++shadow_variant_failures_;
    resource.pass = description.pass;
    resource.description = description;
    resources_.emplace(asset_id, std::move(resource));
    ++upload_generation_; // #888: the kept submission order resolves assets again
    static_cast<void>(leases_.upload(asset_id));
    return core::Result<void>::success();
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::retain(const sim::AssetId asset_id) {
    const auto found = resources_.find(asset_id);
    if (found == resources_.end()) {
        return failure(diagnostic_codes::missing_asset,
            "cannot retain missing renderer asset " + std::to_string(asset_id));
    }
    static_cast<void>(leases_.retain(asset_id));
    return core::Result<void>::success();
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::release(const sim::AssetId asset_id) {
    const auto found = resources_.find(asset_id);
    if (found == resources_.end()) {
        return failure(diagnostic_codes::missing_asset,
            "cannot release missing renderer asset " + std::to_string(asset_id));
    }
    const detail::ResourceLeaseLedger::Release disposition = leases_.release(asset_id);
    if (disposition == detail::ResourceLeaseLedger::Release::retained) {
        return core::Result<void>::success();
    }
    // Unregister the fog consumer before any of its material RIDs is freed.
    if (const auto consumer = fog_consumers_.find(asset_id); consumer != fog_consumers_.end()) {
        detach_fog_consumer(asset_id, consumer->second);
        fog_consumers_.erase(consumer);
    }
    fog_unsupported_.erase(asset_id);
    RenderingServer* rendering = RenderingServer::get_singleton();
    for (auto current = instances_.begin(); current != instances_.end();) {
        if (current->second.asset_id != asset_id) {
            ++current;
            continue;
        }
        current = remove_instance(rendering, current);
    }
    // A pose set for this asset on an entity without its instance (not
    // yet submitted, or waiting while the asset was missing) would
    // otherwise outlive the asset and bind to a later re-upload.
    std::erase_if(skin_poses_, [asset_id](const auto& pose) {
        return pose.second.asset_id == asset_id;
    });
    if (found->second.shadow_receiving) --shadow_receiving_;
    if (found->second.shadow_variant_failed) --shadow_variant_failures_;
    if (rendering) free_resource(*rendering, found->second);
    resources_.erase(found);
    ++upload_generation_;
    return core::Result<void>::success();
}


void GodotRenderer::Impl::free_resource(RenderingServer& rendering, const Resource& resource) {
    if (resource.mesh.is_valid()) rendering.free_rid(resource.mesh);
    if (resource.material.is_valid()) rendering.free_rid(resource.material);
    if (resource.shader.is_valid() && !resource.shared_shader) rendering.free_rid(resource.shader);
    if (resource.texture.is_valid()) rendering.free_rid(resource.texture);
    for (const auto& item : resource.binding_textures) {
        if (item.second.is_valid()) rendering.free_rid(item.second);
    }
}

} // namespace eawr::presentation::godot_backend
