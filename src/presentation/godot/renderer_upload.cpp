#include "eawr/core/load_profile.hpp"
#include "renderer_upload_internal.hpp"
#include "vram_probe.hpp"

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
    auto recipe = std::make_shared<Resource::Recipe>();
    recipe->model = model;
    recipe->texture = keep_texture(texture);
    for (const auto& item : binding_textures) recipe->bindings.emplace_back(item.binding, keep_texture(item.texture));
    resource.recipe = std::move(recipe);
    auto& budget = detail::texture_budget();
    if (!budget.probed()) {
        const auto device = detail::probe_device_memory();
        budget.set_device(device.bytes, device.source);
    }
    std::uint64_t incoming{};
    std::set<std::pair<std::uint64_t, std::uint64_t>> counted;
    const auto count_texture = [&](const assets::Texture& value) {
        const auto item = detail::texture_item(value);
        const auto key = std::pair{item.key, item.check};
        if (detail::texture_sharing() && (!counted.insert(key).second || texture_pool_.contains(key))) return;
        const auto drop = detail::TextureBudget::usable_drop(detail::TextureBudget::mip_edges(value), budget.drop());
        for (std::size_t level = drop; level < value.mips.size(); ++level) {
            const auto bytes = static_cast<std::uint64_t>(value.mips[level].bytes.size());
            incoming += value.format == assets::PixelFormat::bgr8 ? bytes / 3U * 4U : bytes;
        }
    };
    count_texture(texture);
    for (const auto& item : binding_textures) count_texture(item.texture);
    for (const auto& mesh : model.meshes) {
        if (!mesh.visible) continue;
        for (const auto& surface : mesh.submeshes) incoming += detail::surface_gpu_bytes(
            surface.vertices.size(), surface.indices.size(), mesh.bone >= 0 || !surface.skin_bones.empty(),
            legacy::authored_binormals(description));
    }
    std::uint64_t held{};
    for (const auto& category : detail::gpu_memory_ledger().totals()) held += category.live_bytes;
    const auto state = budget.state();
    // Before frames draw, the ledger supplies the allocations the device counter cannot yet see.
    held = std::max(held, static_cast<std::uint64_t>(rendering->get_rendering_info(
        RenderingServer::RENDERING_INFO_VIDEO_MEM_USED)));
    const auto usable = state.device_bytes / detail::TextureBudget::usable_denominator
        * detail::TextureBudget::usable_numerator;
    const bool resident = residency_.eager(held, incoming, usable);
    if (!upload_identity_pool_) upload_identity_pool_ = std::make_unique<detail::UploadIdentityPool>();
    resource.identity = upload_identity_pool_->submit(
        detail::upload_identity_bytes(model, texture, description, named));
    MaterialRefusal refusal;
    bool texture_uploaded = !resident || acquire_texture(*rendering, texture, resource.texture, resource.gpu_items);
    for (const GodotRenderer::BindingTexture& item : binding_textures) {
        if (!texture_uploaded) break;
        RID bound;
        texture_uploaded = !resident || acquire_texture(*rendering, item.texture, bound, resource.gpu_items);
        if (bound.is_valid()) resource.binding_textures.emplace_back(item.binding, bound);
    }
    const MaterialUpload material_uploaded = texture_uploaded
        ? upload_material(*rendering, description, resource, refusal)
        : MaterialUpload::failed;
    if (!texture_uploaded || material_uploaded != MaterialUpload::success
        || !upload_mesh(*rendering, model, resource, legacy::authored_binormals(description), resident)) {
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
    residency_.add(asset_id, resident);
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
    residency_.remove(asset_id);
    ++upload_generation_;
    return core::Result<void>::success();
}

std::shared_ptr<const assets::Texture> GodotRenderer::Impl::keep_texture(const assets::Texture& texture) {
    const auto item = detail::texture_item(texture);
    auto& entry = texture_recipes_[{item.key, item.check}];
    if (auto existing = entry.lock()) return existing;
    auto kept = std::make_shared<const assets::Texture>(texture);
    entry = kept;
    return kept;
}

bool GodotRenderer::Impl::admit_resource(RenderingServer& rendering, const sim::AssetId asset, Resource& resource) {
    if (resource.mesh.is_valid()) return true;
    const auto& recipe = *resource.recipe;
    bool ready = acquire_texture(rendering, *recipe.texture, resource.texture, resource.gpu_items);
    for (const auto& [name, texture] : recipe.bindings) {
        if (!ready) break;
        RID bound;
        ready = acquire_texture(rendering, *texture, bound, resource.gpu_items);
        if (bound.is_valid()) resource.binding_textures.emplace_back(name, bound);
    }
    if (ready) ready = upload_mesh(rendering, recipe.model, resource, legacy::authored_binormals(resource.description));
    if (!ready) {
        evict_resource(rendering, asset, resource);
        residency_.admission_failed(asset);
        fail(diagnostic_codes::upload_failed, "Godot rejected demanded renderer asset " + std::to_string(asset));
        return false;
    }
    configure_material(rendering, resource.description, resource.material, resource.texture, resource.binding_textures);
    if (resource.priority) rendering.material_set_render_priority(resource.material, *resource.priority);
    if (const auto fog = fog_consumers_.find(asset); fog != fog_consumers_.end()) {
        if (const auto problem = attach_fog_consumer(asset, fog->second)) diagnostics_.push(*problem);
    }
    residency_.resident(asset, true);
    ++upload_generation_;
    return true;
}

void GodotRenderer::Impl::evict_resource(RenderingServer& rendering, const sim::AssetId asset, Resource& resource) {
    if (const auto fog = fog_consumers_.find(asset); fog != fog_consumers_.end()) detach_fog_consumer(asset, fog->second);
    for (auto instance = instances_.begin(); instance != instances_.end();) {
        if (instance->second.asset_id == asset) instance = remove_instance(&rendering, instance);
        else ++instance;
    }
    if (resource.mesh.is_valid()) rendering.free_rid(resource.mesh);
    resource.mesh = {};
    for (const auto& item : resource.gpu_items) {
        if (item.category == detail::GpuCategory::texture) release_texture(rendering, item);
        else detail::gpu_memory_ledger().remove(item);
    }
    resource.gpu_items.clear();
    resource.texture = {};
    resource.binding_textures.clear();
    configure_material(rendering, resource.description, resource.material, {}, {});
    residency_.resident(asset, false);
    ++upload_generation_;
}

void GodotRenderer::Impl::reconcile_residency(RenderingServer& rendering,
    const std::span<const sim::RenderInstance> instances) {
    std::vector<std::uint64_t> demand;
    demand.reserve(instances.size());
    for (const auto& instance : instances) demand.push_back(instance.asset_id);
    const auto budget = detail::texture_budget().state();
    residency_.budget(budget.device_bytes, budget.drop);
    const auto changes = residency_.plan(demand);
    // Release unused resources before admitting the newly visible slots.
    for (const auto asset : changes.evict) evict_resource(rendering, asset, resources_.at(asset));
    for (const auto asset : changes.admit) static_cast<void>(admit_resource(rendering, asset, resources_.at(asset)));
}


void GodotRenderer::Impl::free_resource(RenderingServer& rendering, const Resource& resource) {
    if (resource.mesh.is_valid()) rendering.free_rid(resource.mesh);
    if (resource.material.is_valid()) rendering.free_rid(resource.material);
    if (resource.shader.is_valid() && !resource.shared_shader) rendering.free_rid(resource.shader);
    // The texture RIDs are borrowed from the pool; the resource's texture
    // items hold its references.
    for (const detail::GpuItem& item : resource.gpu_items) {
        if (item.category == detail::GpuCategory::texture) release_texture(rendering, item);
        else detail::gpu_memory_ledger().remove(item);
    }
}

} // namespace eawr::presentation::godot_backend
