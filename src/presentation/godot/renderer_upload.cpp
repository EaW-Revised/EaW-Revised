#include "eawr/core/load_profile.hpp"
#include "eawr/diagnostics/owner_census.hpp"
#include "renderer_upload_internal.hpp"
#include "vram_probe.hpp"

#include <map>
#include <optional>
#include <sstream>

namespace eawr::presentation::godot_backend {
namespace detail {
struct RecipeMemoryProbe final {
    static void census(const GodotRenderer::Impl& impl, diagnostics::OwnerCensus& output) noexcept {
        for (const auto& [asset, resource] : impl.resources_) {
            if (!output.step(diagnostics::OwnerCensus::model_vertices)) { output.models_incomplete(); break; }
            static_cast<void>(asset);
            if (resource.recipe) output.model(resource.recipe->model.retained(), diagnostics::OwnerCensus::renderer);
        }
    }
    // One-shot texture owner census (--eawr-texture-census), main thread only. Owners are found here, at
    // snapshot time, from the resources' recipes, so uploading keeps no extra record.
    static void texture_census(const GodotRenderer::Impl& impl, TextureCensus& census) {
        struct Holder final {
            std::uint64_t first_asset{}, last_asset{}, shared_by{};
            std::string owner, role;
            const TextureRecipe* recipe{};
        };
        std::map<std::pair<std::uint64_t, std::uint64_t>, Holder> holders;
        for (const auto& [asset, resource] : impl.resources_) {
            if (!resource.recipe) continue;
            const std::string& model_path = resource.recipe->model.source().logical_path;
            const auto note = [&](const TextureRecipe* recipe, const std::string& role) {
                if (!recipe) return;
                const auto [at, inserted] = holders.try_emplace({recipe->item().key, recipe->item().check});
                Holder& holder = at->second;
                if (inserted) {
                    holder.first_asset = asset;
                    holder.owner = model_path;
                    holder.role = role;
                    holder.recipe = recipe;
                }
                if (inserted || holder.last_asset != asset) ++holder.shared_by;
                holder.last_asset = asset;
            };
            note(resource.recipe->texture.get(), "base");
            for (const auto& binding : resource.recipe->bindings) note(binding.second.get(), binding.first);
        }
        const auto describe = [](TextureCensusRow& row, const Holder& holder) {
            row.owner = holder.owner;
            row.role = holder.role;
            row.shared_by = holder.shared_by;
            const assets::Source& source = holder.recipe->source();
            row.path = source.logical_path;
            row.source_id = source.source_id;
            row.layer_id = source.layer_id;
            if (const assets::Texture* retained = holder.recipe->retained()) {
                row.cpu_snapshot_held = true;
                row.cpu_bytes = assets::payload_capacity(*retained).texture_payload_bytes;
            }
        };
        for (const auto& [key, pooled] : impl.texture_pool_) {
            TextureCensusRow row;
            row.key = key.first;
            row.check = key.second;
            row.format = pooled.item.format;
            row.width = pooled.item.width;
            row.height = pooled.item.height;
            row.mips = pooled.item.mips;
            row.device_bytes = pooled.item.bytes;
            row.compressed = pooled.item.compressed;
            const auto holder = holders.find(key);
            if (holder == holders.end()) {
                row.owner_kind = TextureOwnerKind::unowned;
                row.owner = "no model resource";
            } else {
                row.owner_kind = TextureOwnerKind::model;
                describe(row, holder->second);
            }
            census.add(std::move(row));
        }
        // Demand-only (dormant) resources hold a CPU snapshot but no GPU texture.
        for (const auto& [key, holder] : holders) {
            if (impl.texture_pool_.contains(key) || !holder.recipe->retained()) continue;
            TextureCensusRow row;
            row.key = key.first;
            row.check = key.second;
            const GpuItem& item = holder.recipe->item();
            row.format = item.format;
            row.width = item.width;
            row.height = item.height;
            row.mips = item.mips;
            row.compressed = item.compressed;
            row.owner_kind = TextureOwnerKind::model;
            row.resident = false;
            describe(row, holder);
            census.add(std::move(row));
        }
        TextureCensusRegistry::instance().append(census, &impl);
    }
    static assets::RetainedPayload read(const GodotRenderer::Impl& impl) {
        assets::PayloadProbe probe;
        for (const auto& [entity, bytes] : impl.proxy_.charges) {
            static_cast<void>(bytes);
            if (impl.instances_.contains(entity))
                probe.result.proxy_payload_bytes += sizeof(GodotRenderer::Impl::Instance);
            const auto pose = impl.skin_poses_.find(entity);
            if (pose != impl.skin_poses_.end()) {
                probe.result.proxy_payload_bytes += sizeof(GodotRenderer::Impl::PendingPose)
                    + assets::vector_capacity_bytes(pose->second.palette)
                    + assets::vector_capacity_bytes(pose->second.model_transforms);
            }
        }
        for (const auto& [asset, resource] : impl.resources_) {
            static_cast<void>(asset);
            if (!resource.recipe) continue;
            ++probe.result.recipes;
            probe.model(resource.recipe->model.retained());
            probe.texture(resource.recipe->texture->retained());
            for (const auto& binding : resource.recipe->bindings) probe.texture(binding.second->retained());
        }
        return probe.result;
    }
};
} // namespace detail

void GodotRenderer::set_texture_loader(detail::TextureLoader loader) {
    impl_->set_texture_loader(std::move(loader));
}

void GodotRenderer::set_model_loader(detail::ModelLoader loader) {
    impl_->set_model_loader(std::move(loader));
}

std::pair<std::size_t, std::size_t> GodotRenderer::model_recipe_counts() const {
    return impl_ ? std::pair{impl_->model_stats_.ever_released, impl_->model_stats_.ever_fallback} : std::pair<std::size_t, std::size_t>{};
}

std::size_t GodotRenderer::model_reload_failures() const { return impl_ ? impl_->model_reload_failures_ : 0; }

assets::RetainedPayload GodotRenderer::recipe_memory() const {
    return impl_ ? detail::RecipeMemoryProbe::read(*impl_) : assets::RetainedPayload{};
}

void GodotRenderer::owner_census(diagnostics::OwnerCensus& output) const {
    // Same main-thread ownership boundary as recipe_memory and upload/release.
    if (impl_) detail::RecipeMemoryProbe::census(*impl_, output);
}

void GodotRenderer::set_texture_census(const bool enabled) noexcept {
    detail::TextureCensusRegistry::enabled().store(enabled, std::memory_order_relaxed);
}

bool GodotRenderer::texture_census_enabled() noexcept {
    return detail::TextureCensusRegistry::enabled().load(std::memory_order_relaxed);
}

std::function<std::string(std::string_view)> GodotRenderer::texture_census_probe() const {
    // Same main-thread, lifetime-token contract as recipe_memory_probe. Returns "" with the switch off.
    return [lifetime = std::weak_ptr<const bool>(memory_probe_lifetime_), impl = impl_.get()](const std::string_view phase) {
        if (!texture_census_enabled() || lifetime.expired() || !impl) return std::string{};
        detail::TextureCensus census;
        detail::RecipeMemoryProbe::texture_census(*impl, census);
        const auto totals = detail::gpu_memory_ledger().totals();
        std::ostringstream output;
        census.write(output, phase, &totals[static_cast<std::size_t>(detail::GpuCategory::texture)]);
        return output.str();
    };
}

std::vector<std::string> GodotRenderer::texture_census_snapshots(const std::string_view label) {
    std::vector<std::string> result;
    if (!texture_census_enabled()) return result;
    const auto totals = detail::gpu_memory_ledger().totals();
    for (const void* live : detail::TextureCensusRenderers::instance().live()) {
        detail::TextureCensus census;
        detail::RecipeMemoryProbe::texture_census(*static_cast<const GodotRenderer::Impl*>(live), census);
        if (census.rows().empty()) continue;
        std::ostringstream output;
        census.write(output, label, &totals[static_cast<std::size_t>(detail::GpuCategory::texture)]);
        result.push_back(output.str());
    }
    return result;
}

std::function<assets::RetainedPayload()> GodotRenderer::recipe_memory_probe() const {
    // Invoked only on the main thread, like renderer destruction. The token is
    // destroyed before impl_, so an old startup callback never dereferences a
    // released renderer. Capturing the stable Impl address also follows moves;
    // its lifetime token moves alongside impl_ in the default move operations.
    return [lifetime = std::weak_ptr<const bool>(memory_probe_lifetime_), impl = impl_.get()] {
        return lifetime.expired() || !impl ? assets::RetainedPayload{} : detail::RecipeMemoryProbe::read(*impl);
    };
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::upload(
    const sim::AssetId asset_id,
    const ModelView& model,
    const assets::Texture& texture,
    const MaterialDescription& description,
    const std::span<const GodotRenderer::BindingTexture> binding_textures,
    ModelUpload retained_model) {
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
    const bool authoritative = retained_model.model != nullptr; // loader/cache model, not a derived copy
    recipe->model = detail::ModelRecipe(authoritative ? std::move(retained_model)
        : ModelUpload{std::make_shared<const assets::Model>(model.original), {}}, model_loader_, authoritative);
    // The texture bytes are hashed once per upload, inside upload_keep_texture: the recipes and the budget count below both
    // use the item (#5197).
    std::optional<detail::GpuItem> texture_gpu_item;
    std::vector<detail::GpuItem> binding_items;
    binding_items.reserve(binding_textures.size());
    {
        core::load_profile::DetailScope keep(core::load_profile::Phase::upload_keep_texture);
        texture_gpu_item = detail::texture_item(texture);
        recipe->texture = keep_texture(texture, *texture_gpu_item);
        for (const auto& item : binding_textures) {
            binding_items.push_back(detail::texture_item(item.texture));
            recipe->bindings.emplace_back(item.binding, keep_texture(item.texture, binding_items.back()));
        }
    }
    resource.recipe = std::move(recipe);
    auto& budget = detail::texture_budget();
    if (!budget.probed()) {
        if (const auto device = detail::probe_device_memory()) {
            budget.set_device(device->bytes, device->source);
        }
    }
    std::uint64_t incoming{};
    std::set<std::pair<std::uint64_t, std::uint64_t>> counted;
    const auto count_texture = [&](const assets::Texture& value, const detail::GpuItem& item) {
        const auto key = std::pair{item.key, item.check};
        if (detail::texture_sharing() && (!counted.insert(key).second || texture_pool_.contains(key))) return;
        const auto drop = detail::TextureBudget::usable_drop(detail::TextureBudget::mip_edges(value), budget.drop());
        for (std::size_t level = drop; level < value.mips.size(); ++level) {
            const auto bytes = static_cast<std::uint64_t>(value.mips[level].bytes.size());
            incoming += value.format == assets::PixelFormat::bgr8 ? bytes / 3U * 4U : bytes;
        }
    };
    {
        core::load_profile::DetailScope counting(core::load_profile::Phase::upload_count_texture);
        count_texture(texture, *texture_gpu_item);
        for (std::size_t index = 0; index < binding_textures.size(); ++index) {
            count_texture(binding_textures[index].texture, binding_items[index]);
        }
    }
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
    std::vector<std::uint8_t> identity_bytes;
    {
        core::load_profile::DetailScope copy(core::load_profile::Phase::upload_identity_copy);
        identity_bytes = detail::upload_identity_bytes(model, texture, description, named);
    }
    {
        core::load_profile::DetailScope submit(core::load_profile::Phase::upload_identity_submit);
        resource.identity = upload_identity_pool_->submit(std::move(identity_bytes));
    }
    core::load_profile::DetailScope gpu_create(core::load_profile::Phase::upload_gpu_create);
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
    if (resident) {
        resource.recipe->texture->uploaded();
        for (const auto& binding : resource.recipe->bindings) binding.second->uploaded();
        resource.recipe->model.uploaded(model_stats_, [this](const auto& model) { return model_digest(model); });
    }
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
        render_churn::record(render_churn::Owner::model_resource, render_churn::Event::retained,
            static_cast<std::uint64_t>(found->second.mesh.is_valid()) + static_cast<std::uint64_t>(found->second.material.is_valid()));
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

detail::UploadIdentity GodotRenderer::Impl::model_digest(const std::shared_ptr<const assets::Model>& model) {
    return model_digests_.identity(model);
}

std::shared_ptr<detail::TextureRecipe> GodotRenderer::Impl::keep_texture(
    const assets::Texture& texture, const detail::GpuItem& item) {
    auto& entry = texture_recipes_[{item.key, item.check}];
    if (auto existing = entry.lock()) return existing;
    auto kept = std::make_shared<detail::TextureRecipe>(texture, texture_loader_, item);
    entry = kept;
    return kept;
}

bool GodotRenderer::Impl::acquire_texture(RenderingServer& rendering, const detail::TextureRecipe& recipe,
    RID& target, std::vector<detail::GpuItem>& items) {
    const auto& item = recipe.item();
    if (detail::texture_sharing()) {
        const auto found = texture_pool_.find({item.key, item.check});
        if (found != texture_pool_.end()) {
            ++found->second.references;
            detail::gpu_memory_ledger().reuse(item);
            target = found->second.rid;
            items.push_back(item);
            return true;
        }
    }
    const auto texture = recipe.read();
    return texture && acquire_texture(rendering, *texture, target, items);
}

bool GodotRenderer::Impl::admit_resource(RenderingServer& rendering, const sim::AssetId asset, Resource& resource) {
    if (resource.mesh.is_valid()) return true;
    auto& recipe = *resource.recipe;
    bool ready = acquire_texture(rendering, *recipe.texture, resource.texture, resource.gpu_items);
    for (const auto& [name, texture] : recipe.bindings) {
        if (!ready) break;
        RID bound;
        ready = acquire_texture(rendering, *texture, bound, resource.gpu_items);
        if (bound.is_valid()) resource.binding_textures.emplace_back(name, bound);
    }
    std::optional<ModelUpload> model;
    if (ready) {
        model = recipe.model.read(admission_model_);
        if (!model) {
            ++model_reload_failures_;
            fail(diagnostic_codes::upload_failed, "renderer asset " + std::to_string(asset) + " model '"
                + recipe.model.source().logical_path + "' no longer reloads to its uploaded content digest");
        }
        ready = model.has_value()
            && upload_mesh(rendering, model->view(), resource, legacy::authored_binormals(resource.description));
    }
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
    recipe.texture->uploaded();
    for (const auto& binding : recipe.bindings) binding.second->uploaded();
    recipe.model.uploaded(model_stats_, [this](const auto& model) { return model_digest(model); });
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
    if (resource.mesh.is_valid()) {
        render_churn::record(render_churn::Owner::model_resource, render_churn::Event::freed);
        rendering.free_rid(resource.mesh);
    }
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
    admission_model_.clear(); // sibling surfaces share one transient decode only within this batch
}


void GodotRenderer::Impl::free_resource(RenderingServer& rendering, const Resource& resource) {
    render_churn::record(render_churn::Owner::model_resource, render_churn::Event::freed,
        static_cast<std::uint64_t>(resource.mesh.is_valid()) + static_cast<std::uint64_t>(resource.material.is_valid())
        + static_cast<std::uint64_t>(resource.shader.is_valid() && !resource.shared_shader));
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
