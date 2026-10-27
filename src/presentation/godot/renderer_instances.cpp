#include "renderer_internal.hpp"
#include "eawr/presentation/skin_pose.hpp"

#include <limits>
#include <new>
#include <set>

namespace eawr::presentation::godot_backend {

bool GodotRenderer::set_proxy_budget(PresentationBudget* budget) {
    if (!impl_->proxy_.charges.empty()) return false;
    impl_->proxy_.budget = budget ? budget : &impl_->proxy_.fallback;
    return true;
}

ResourceBudgetSnapshot GodotRenderer::proxy_budget() const {
    return impl_->proxy_.budget->snapshot(ResourceKind::gpu_proxy);
}

std::optional<std::uint64_t> GodotRenderer::model_copy_bytes(
    const std::span<const sim::AssetId> assets) const {
    if (assets.empty()) return std::nullopt;
    std::uint64_t bytes{};
    const auto add = [&](const std::uint64_t count) {
        if (count > std::numeric_limits<std::uint64_t>::max() - bytes) return false;
        bytes += count;
        return true;
    };
    for (const auto asset : assets) {
        const auto resource = impl_->resources_.find(asset);
        if (resource == impl_->resources_.end()) return std::nullopt;
        if (!add(sizeof(Impl::Instance))) return std::nullopt;
        const auto bones = resource->second.skinned ? resource->second.bone_count : 0;
        if (bones > std::numeric_limits<std::uint64_t>::max() / sizeof(Transform3D)
            || !add(bones * sizeof(Transform3D))) return std::nullopt;
        // Charging each piece's full mesh is conservative when pieces share it.
        bool known_mesh = false;
        for (const auto& item : resource->second.gpu_items) {
            known_mesh = known_mesh || item.category == detail::GpuCategory::mesh;
            if ((item.category == detail::GpuCategory::mesh || !detail::texture_sharing())
                && !add(item.bytes)) return std::nullopt;
        }
        if (!known_mesh) return std::nullopt;
    }
    return bytes;
}

GodotRenderer::ProxyResult GodotRenderer::reserve_proxy_instances(
    const std::span<const ProxyInstance> requested, const std::function<bool()>& allocation_gate) {
    auto* rendering = RenderingServer::get_singleton();
    if (!rendering || !impl_->scenario_.is_valid())
        return {ProxyOutcome::allocation_failed, 0, "renderer_unavailable"};
    std::size_t bytes = 0;
    std::vector<std::size_t> charges;
    std::set<sim::EntityId> unique;
    try {
        charges.reserve(requested.size());
        for (const auto& proxy : requested) {
            const auto resource = impl_->resources_.find(proxy.asset);
            if (resource == impl_->resources_.end() || !resource->second.mesh.is_valid()
                || impl_->instances_.contains(proxy.entity) || impl_->proxy_.charges.contains(proxy.entity)
                || !unique.insert(proxy.entity).second)
                return {ProxyOutcome::invalid_recipe, bytes, "proxy_recipe_unavailable"};
            // Account retained instance payload and skeleton transform storage.
            // Driver/RID overhead is unknown until the GPU provider supplies it;
            // probe_available remains false, never a fabricated VRAM estimate.
            const auto bones = resource->second.skinned ? resource->second.bone_count : 0;
            constexpr auto maximum = std::numeric_limits<std::size_t>::max();
            if (bones > (maximum - sizeof(Impl::Instance)) / sizeof(Transform3D))
                return {ProxyOutcome::allocation_failed, bytes, "proxy_size_overflow"};
            const auto charge = sizeof(Impl::Instance) + bones * sizeof(Transform3D);
            if (charge > maximum - bytes) return {ProxyOutcome::allocation_failed, bytes, "proxy_size_overflow"};
            bytes += charge;
            charges.push_back(charge);
        }
    } catch (const std::bad_alloc&) {
        return {ProxyOutcome::allocation_failed, bytes, "proxy_plan_allocation"};
    }
    const auto reservation = impl_->proxy_.budget->reserve(ResourceKind::gpu_proxy, bytes);
    if (reservation != ReservationOutcome::reserved)
        return {reservation == ReservationOutcome::budget_pressure ? ProxyOutcome::refused_budget
            : ProxyOutcome::allocation_failed, bytes, reservation == ReservationOutcome::budget_pressure
            ? "budget_pressure" : "proxy_size_overflow"};
    std::size_t created = 0;
    const auto rollback = [&] {
        for (std::size_t index = 0; index < created; ++index) {
            const auto entity = requested[index].entity;
            impl_->proxy_.charges.erase(entity);
            impl_->forget_instance_pose(entity);
        }
        impl_->proxy_.budget->release(ResourceKind::gpu_proxy, bytes);
    };
    try {
        impl_->instances_.reserve(impl_->instances_.size() + requested.size());
        impl_->proxy_.charges.reserve(impl_->proxy_.charges.size() + requested.size());
        for (std::size_t index = 0; index < requested.size(); ++index) {
            if (allocation_gate && !allocation_gate()) {
                rollback();
                return {ProxyOutcome::allocation_failed, bytes, "proxy_instance_allocation"};
            }
            const auto& proxy = requested[index];
            // Publish an empty owner first: every later failing allocation has
            // an RAII-visible RID slot for rollback.
            auto& instance = impl_->instances_.try_emplace(proxy.entity).first->second;
            ++created;
            instance.asset_id = proxy.asset;
            instance.reserved_proxy = true;
            instance.rid = rendering->instance_create();
            if (!instance.rid.is_valid()) {
                rollback();
                return {ProxyOutcome::allocation_failed, bytes, "proxy_instance_allocation"};
            }
            const auto& resource = impl_->resources_.at(proxy.asset);
            rendering->instance_set_base(instance.rid, resource.mesh);
            rendering->instance_set_scenario(instance.rid, impl_->scenario_);
            rendering->instance_set_visible(instance.rid, false);
            instance.visible = false;
            if (impl_->non_casting_.contains(proxy.asset))
                rendering->instance_geometry_set_cast_shadows_setting(instance.rid, RenderingServer::SHADOW_CASTING_SETTING_OFF);
            if (render_categories::enabled)
                instance.category_surfaces = rendering->mesh_get_surface_count(resource.mesh);
            impl_->copy_instance_pose(proxy.source, proxy.entity, 1.0F);
            if (render_categories::enabled) {
                const auto role = impl_->category_roles_.find(proxy.entity);
                instance.category_role = role == impl_->category_roles_.end()
                    ? render_categories::Role::unknown : role->second;
                instance.category_casts = !impl_->non_casting_.contains(proxy.asset);
            }
            const auto pending = impl_->skin_poses_.find(proxy.entity);
            Impl::bind_instance_skin(*rendering, instance, resource,
                pending == impl_->skin_poses_.end() ? nullptr : &pending->second);
            if (resource.skinned && resource.bone_count && !instance.skeleton.is_valid()) {
                rollback();
                return {ProxyOutcome::allocation_failed, bytes, "proxy_skeleton_allocation"};
            }
            impl_->proxy_.charges.emplace(proxy.entity, charges[index]);
        }
    } catch (const std::bad_alloc&) {
        rollback();
        return {ProxyOutcome::allocation_failed, bytes, "proxy_instance_allocation"};
    }
    return {ProxyOutcome::reserved, bytes, {}};
}

void GodotRenderer::Impl::copy_instance_pose(const sim::EntityId source, const sim::EntityId target,
                                            const float light_factor) {
    if (source == target) return;
    if (render_categories::enabled) {
        const auto role = category_roles_.find(source);
        if (role != category_roles_.end()) category_roles_.insert_or_assign(target, role->second);
    }
    const auto pose = skin_poses_.find(source);
    if (pose == skin_poses_.end()) clear_skin_pose(target);
    else {
        const PendingPose saved = pose->second;
        skin_poses_[target] = saved;
        const auto instance = instances_.find(target);
        if (instance != instances_.end() && instance->second.asset_id == saved.asset_id) {
            if (auto* rendering = RenderingServer::get_singleton()) {
                static_cast<void>(apply_skin_pose(*rendering, instance->second, saved));
            }
        }
    }
    const auto light = light_scales_.find(source);
    auto rgb = light == light_scales_.end() ? std::array<float, 3>{1, 1, 1} : light->second;
    for (auto& channel : rgb) channel *= light_factor;
    set_light_scale(target, rgb);
    // FW-30: mutable capture colour is independent of the mesh upload.
    const auto color = colorizations_.find(source);
    if (color != colorizations_.end()) set_unit_colorization(target, color->second);
    else {
        colorizations_.erase(target);
        const auto instance = instances_.find(target);
        if (instance != instances_.end()) {
            if (auto* rendering = RenderingServer::get_singleton()) {
                rendering->instance_geometry_set_shader_parameter(instance->second.rid,
                    StringName("eawr_unit_colorization"), Vector4(0, 0, 0, -1.0F));
            }
        }
    }
    set_unit_opacity(target, 1.0F);
}

void GodotRenderer::Impl::forget_instance_pose(const sim::EntityId entity_id) {
    if (render_categories::enabled) category_roles_.erase(entity_id);
    // Retirement is explicit, unlike absence from a submitted snapshot. Drop the
    // instance and skeleton now, including when another entity retains its mesh.
    const auto instance = instances_.find(entity_id);
    if (instance != instances_.end()) static_cast<void>(remove_instance(RenderingServer::get_singleton(), instance));
    skin_poses_.erase(entity_id);
    light_scales_.erase(entity_id);
    colorizations_.erase(entity_id);
    opacities_.erase(entity_id);
}

void GodotRenderer::Impl::set_render_category(const sim::EntityId entity_id, const render_categories::Role role) {
    if (!render_categories::enabled) return;
    category_roles_.insert_or_assign(entity_id, role);
    const auto instance = instances_.find(entity_id);
    if (instance != instances_.end()) instance->second.category_role = role;
}

void GodotRenderer::set_render_category(const sim::EntityId entity_id, const render_categories::Role role) {
    impl_->set_render_category(entity_id, role);
}

void GodotRenderer::write_churn_census(std::ostream& output) const {
    impl_->write_churn_census(output);
}

void GodotRenderer::Impl::write_churn_census(std::ostream& output) const {
    if (!render_categories::enabled) { output << "{\"enabled\":false}"; return; }
    // Report phase only: no census map is built while preparing/submitting a frame.
    render_churn::Census census;
    for (const auto& [entity, instance] : instances_) {
        if (!instance.visible) continue;
        const auto found = resources_.find(instance.asset_id);
        if (found == resources_.end()) continue;
        const auto& resource = found->second;
        const auto colour = colorizations_.find(entity);
        const auto light = light_scales_.find(entity);
        const auto opacity = opacities_.find(entity);
        const auto fog = fog_consumers_.find(instance.asset_id);
        const RID material = fog != fog_consumers_.end() && fog->second.material.is_valid()
            ? fog->second.material : resource.material;
        const auto rgb = colour == colorizations_.end() ? std::array{1.0F, 1.0F, 1.0F} : colour->second;
        const auto scale = light == light_scales_.end() ? std::array{1.0F, 1.0F, 1.0F} : light->second;
        const float alpha = opacity == opacities_.end() ? 1.0F : opacity->second;
        render_churn::CohortKey key{instance.asset_id, static_cast<std::uint64_t>(material.get_id()),
            fog == fog_consumers_.end() ? 0 : fog->second.id, static_cast<std::uint32_t>(resource.pass),
            resource.priority.value_or(0), resource.skinned, !resource.billboard_modes.empty(),
            alpha >= 1.0F && !non_casting_.contains(instance.asset_id),
            {render_churn::bits(rgb[0]), render_churn::bits(rgb[1]), render_churn::bits(rgb[2]),
             render_churn::bits(alpha), render_churn::bits(scale[0]), render_churn::bits(scale[1]), render_churn::bits(scale[2])}};
        census.add(key);
    }
    output << "{\"enabled\":true,\"phase\":\"report\",\"active_instances\":" << census.active
        << ",\"rigid_instances\":" << census.rigid << ",\"excluded_skin\":" << census.excluded_skin
        << ",\"excluded_billboard\":" << census.excluded_billboard << ",\"excluded_any\":" << census.excluded_any
        << ",\"lifetime_rid_events\":";
    render_churn::write_json(output, render_churn::total);
    output << ",\"cohorts\":[";
    bool first = true;
    for (const auto& [key, count] : census.cohorts) {
        output << (first ? "" : ",") << "{\"asset\":" << key.asset << ",\"material\":" << key.material
            << ",\"pass\":" << key.pass << ",\"priority\":" << key.priority << ",\"fog\":" << key.fog
            << ",\"skin\":" << key.skinned << ",\"billboard\":" << key.billboard << ",\"casts\":" << key.casts
            << ",\"rigid\":" << key.rigid() << ",\"count\":" << count << ",\"colour_opacity_light_bits\":[";
        for (std::size_t i = 0; i < key.colour_opacity_light.size(); ++i)
            output << (i ? "," : "") << key.colour_opacity_light[i];
        output << "]}"; first = false;
    }
    output << "]}";
}

std::vector<std::uint8_t> GodotRenderer::Impl::skin_geometry_bones(const sim::AssetId asset_id) const {
    const auto resource = resources_.find(asset_id);
    if (resource == resources_.end() || !resource->second.skinned
        || !resource->second.hidden_geometry_collapses
        || resource->second.description.route == MaterialRoute::modern_spatial
        || legacy::reads_wind(resource->second.description)) return {};
    return resource->second.skin_used_bones;
}

std::span<const std::uint8_t> GodotRenderer::Impl::skin_geometry_bones_view(const sim::AssetId asset_id) const {
    const auto resource = resources_.find(asset_id);
    if (resource == resources_.end() || !resource->second.skinned
        || !resource->second.hidden_geometry_collapses
        || resource->second.description.route == MaterialRoute::modern_spatial
        || legacy::reads_wind(resource->second.description)) return {};
    return resource->second.skin_used_bones;
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::set_skin_pose(
    const sim::EntityId entity_id,
    const sim::AssetId asset_id,
    const std::span<const animation::BonePose> bones) {
    const auto resource = resources_.find(asset_id);
    if (resource == resources_.end()) {
        return failure(diagnostic_codes::missing_asset,
            "cannot bind a skin pose to missing renderer asset " + std::to_string(asset_id));
    }
    if (!resource->second.skinned || bones.size() != resource->second.bone_count) {
        return failure(diagnostic_codes::invalid_skin_pose,
            "skin pose bone count does not match the uploaded skinned asset");
    }
    const auto convert = [](const animation::Matrix& matrix) { return transform_from(matrix); };
    std::shared_ptr<const SkinPosePalette<Transform3D>> prepared;
    if (skin_batch_active_) {
        prepared = skin_batch_.prepare(bones, convert, skin_profile_ ? &skin_work_ : nullptr);
        if (!prepared) return failure(diagnostic_codes::invalid_skin_pose,
            "skin pose contains a non-finite palette matrix");
    } else {
        // A live turret/clip may follow a shared idle pose. Compare against the
        // palette actually submitted, preserving the ordinary retained buffers.
        const auto retained = skin_poses_.find(entity_id);
        if (retained != skin_poses_.end() && retained->second.prepared)
            retained->second.palette = retained->second.active_palette();
        if (!cache_skin_pose(skin_poses_, entity_id, asset_id, bones, convert, &skin_changes_)) {
            return failure(diagnostic_codes::invalid_skin_pose,
                "skin pose contains a non-finite palette matrix");
        }
        skin_poses_.at(entity_id).prepared.reset();
    }
    return bind_skin_pose(entity_id, asset_id, resource->second, std::move(prepared));
}

core::Result<void> GodotRenderer::Impl::bind_skin_pose(const sim::EntityId entity_id,
    const sim::AssetId asset_id, const Resource& resource, std::shared_ptr<const SkinPosePalette<Transform3D>> prepared) {
    SkinPoseTimer binding(skin_profile_ ? &skin_work_.binding_ms : nullptr);
    if (skin_profile_) ++skin_work_.bindings;
    if (prepared) {
        auto& pending = skin_poses_[entity_id];
        skin_palette_changes(pending.active_palette(), prepared->palette, skin_changes_, pending.asset_id != asset_id);
        pending.asset_id = asset_id;
        pending.prepared = std::move(prepared);
    }
    const PendingPose& pose = skin_poses_.at(entity_id);
    const auto instance = instances_.find(entity_id);
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (instance != instances_.end() && instance->second.asset_id == asset_id && rendering) {
        if (resource.billboard_modes.empty()
            && !filter_skin_palette_changes(skin_changes_, resource.skin_used_bones)) {
            return failure(diagnostic_codes::invalid_skin_pose, "skin palette usage does not match the skeleton");
        }
        // A billboard refresh may have replaced the base palette: restore it
        // completely before that refresh. Ordinary skeletons retain unchanged
        // bones, so only byte-different palette matrices cross the engine API.
        const auto changes = resource.billboard_modes.empty()
            ? std::span<const std::uint8_t>(skin_changes_) : std::span<const std::uint8_t>{};
        if (!apply_skin_pose(*rendering, instance->second, pose, changes, skin_profile_ ? &skin_work_ : nullptr)) {
            return failure(diagnostic_codes::invalid_skin_pose,
                "Godot skeleton palette binding failed");
        }
        if (!resource.billboard_modes.empty()) {
            refresh_billboards(*rendering, entity_id, instance->second, resource);
        }
    }
    return core::Result<void>::success();
}

void GodotRenderer::Impl::set_light_scale(const sim::EntityId entity_id, const std::array<float, 3>& rgb) {
    const bool unit = rgb == std::array<float, 3>{1.0F, 1.0F, 1.0F};
    const auto current = light_scales_.find(entity_id);
    if (unit ? current == light_scales_.end() : current != light_scales_.end() && current->second == rgb) return;
    if (unit) light_scales_.erase(current);
    else light_scales_[entity_id] = rgb;
    const auto instance = instances_.find(entity_id);
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (instance != instances_.end() && rendering) apply_light_scale(*rendering, entity_id, instance->second);
}

void GodotRenderer::Impl::apply_light_scale(
    RenderingServer& rendering, const sim::EntityId entity_id, const Instance& instance) const {
    const auto scale = light_scales_.find(entity_id);
    const std::array<float, 3> rgb = scale == light_scales_.end() ? std::array<float, 3>{1.0F, 1.0F, 1.0F} : scale->second;
    rendering.instance_geometry_set_shader_parameter(
        instance.rid, StringName("eawr_unit_light_scale"), Vector3(rgb[0], rgb[1], rgb[2]));
}

void GodotRenderer::Impl::set_unit_colorization(const sim::EntityId entity_id, const std::array<float, 3>& rgb) {
    const auto current = colorizations_.find(entity_id);
    if (current != colorizations_.end() && current->second == rgb) return;
    colorizations_.insert_or_assign(entity_id, rgb);
    const auto instance = instances_.find(entity_id);
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (instance != instances_.end() && rendering) apply_unit_colorization(*rendering, entity_id, instance->second);
}

void GodotRenderer::Impl::apply_unit_colorization(
    RenderingServer& rendering, const sim::EntityId entity_id, const Instance& instance) const {
    const auto colour = colorizations_.find(entity_id);
    if (colour == colorizations_.end()) return;
    const auto& rgb = colour->second;
    rendering.instance_geometry_set_shader_parameter(
        instance.rid, StringName("eawr_unit_colorization"), Vector4(rgb[0], rgb[1], rgb[2], 1.0F));
}

// #535: the fog fade's alpha (space-fog-presentation.md FW-16 to FW-19).
// Geometry transparency supplies alpha to opaque/shield shaders; explicit-alpha
// mesh adapters use the same instance parameter instead of overriding the fade.
// Environment transparents draw first (FW-19), so the backdrop cannot paint over a fading hull.
void GodotRenderer::Impl::set_unit_opacity(const sim::EntityId entity_id, const float alpha) {
    const bool opaque = alpha >= 1.0F;
    const auto current = opacities_.find(entity_id);
    if (opaque ? current == opacities_.end() : current != opacities_.end() && current->second == alpha) return;
    if (opaque) opacities_.erase(current);
    else opacities_[entity_id] = alpha;
    const auto instance = instances_.find(entity_id);
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (instance != instances_.end() && rendering) apply_unit_opacity(*rendering, entity_id, instance->second);
}

void GodotRenderer::Impl::apply_unit_opacity(
    RenderingServer& rendering, const sim::EntityId entity_id, const Instance& instance) const {
    const auto opacity = opacities_.find(entity_id);
    const float alpha = opacity == opacities_.end() ? 1.0F : opacity->second;
    const float clamped = std::clamp(alpha, 0.0F, 1.0F);
    rendering.instance_geometry_set_transparency(instance.rid, 1.0F - clamped);
    rendering.instance_geometry_set_shader_parameter(instance.rid, StringName("eawr_unit_opacity"), clamped);
    // Geometry transparency does not attenuate a shadow. Suppress the opaque
    // silhouette while fading, then restore this asset's original policy.
    rendering.instance_geometry_set_cast_shadows_setting(instance.rid,
        clamped < 1.0F || non_casting_.contains(instance.asset_id)
            ? RenderingServer::SHADOW_CASTING_SETTING_OFF : RenderingServer::SHADOW_CASTING_SETTING_ON);
    if (render_categories::enabled) instance.category_casts = clamped >= 1.0F && !non_casting_.contains(instance.asset_id);
}

core::Result<void> GodotRenderer::Impl::set_material_priority(const sim::AssetId asset_id, const std::int32_t priority) {
    const auto resource = resources_.find(asset_id);
    if (resource == resources_.end()) return failure(diagnostic_codes::missing_asset, "cannot order a missing renderer asset");
    if (priority < -128 || priority > 127) return failure(diagnostic_codes::invalid_material, "material priority exceeds the renderer range");
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return failure(diagnostic_codes::backend_unavailable, "RenderingServer is unavailable");
    resource->second.priority = priority;
    rendering->material_set_render_priority(resource->second.material, priority);
    const auto consumer = fog_consumers_.find(asset_id);
    if (consumer != fog_consumers_.end() && consumer->second.material.is_valid())
        rendering->material_set_render_priority(consumer->second.material, priority);
    return core::Result<void>::success();
}

void GodotRenderer::Impl::clear_skin_pose(const sim::EntityId entity_id) {
    const auto pose = skin_poses_.find(entity_id);
    if (pose == skin_poses_.end()) return;
    skin_poses_.erase(pose);
    // A live instance returns to the rest palette it had before any pose,
    // exactly as bind_instance_skin leaves an unposed skeleton.
    const auto instance = instances_.find(entity_id);
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (instance == instances_.end() || !rendering || !instance->second.skeleton.is_valid()) return;
    const auto resource = resources_.find(instance->second.asset_id);
    if (resource == resources_.end()) return;
    const Transform3D identity;
    for (std::size_t index = 0; index < resource->second.bone_count; ++index) {
        rendering->skeleton_bone_set_transform(instance->second.skeleton,
            static_cast<std::int32_t>(index), identity);
    }
    if (!resource->second.billboard_modes.empty()) {
        refresh_billboards(*rendering, entity_id, instance->second, resource->second);
    }
}

void GodotRenderer::Impl::set_billboard_light(const sim::AssetId asset_id, const std::array<float, 3>& toward_light) {
    const auto found = resources_.find(asset_id);
    const Vector3 direction(toward_light[0], toward_light[1], toward_light[2]);
    if (found != resources_.end() && direction.is_finite() && direction.length_squared() > 0.0F) {
        found->second.billboard_light = direction.normalized();
        ++billboard_generation_; // #888: the next submit turns the billboards again
    }
}

core::Result<void> GodotRenderer::Impl::set_material_scalar(const sim::AssetId asset_id, const std::string_view binding,
                                                            const float value) {
    const auto resource = resources_.find(asset_id);
    if (resource == resources_.end()) {
        return failure(diagnostic_codes::missing_asset,
            "cannot set a material value on missing renderer asset " + std::to_string(asset_id));
    }
    std::vector<MaterialBinding>& bindings = resource->second.description.bindings;
    const auto bound = std::find_if(bindings.begin(), bindings.end(),
        [&](const MaterialBinding& item) { return item.name == binding; });
    if (bound == bindings.end() || !std::holds_alternative<float>(bound->value) || !std::isfinite(value)) {
        return failure(diagnostic_codes::invalid_material, "material value " + std::string(binding)
            + " is not a finite value for a scalar binding of renderer asset " + std::to_string(asset_id));
    }
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return failure(diagnostic_codes::backend_unavailable, "RenderingServer is unavailable");
    // The stored description configures a later fog variant, so it follows.
    bound->value = value;
    const StringName name(bound->name.c_str());
    rendering->material_set_param(resource->second.material, name, value);
    const auto consumer = fog_consumers_.find(asset_id);
    if (consumer != fog_consumers_.end() && consumer->second.material.is_valid()
        && consumer->second.material != resource->second.material) {
        rendering->material_set_param(consumer->second.material, name, value);
    }
    return core::Result<void>::success();
}

core::Result<void> GodotRenderer::Impl::set_material_vector3(const sim::AssetId asset_id, const std::string_view binding,
                                                            const std::array<float, 3>& value) {
    const auto resource = resources_.find(asset_id);
    if (resource == resources_.end()) return failure(diagnostic_codes::missing_asset,
        "cannot set a material value on missing renderer asset " + std::to_string(asset_id));
    auto& bindings = resource->second.description.bindings;
    const auto bound = std::find_if(bindings.begin(), bindings.end(),
        [&](const MaterialBinding& item) { return item.name == binding; });
    if (bound == bindings.end() || !std::holds_alternative<assets::Vec3f>(bound->value)
        || !std::all_of(value.begin(), value.end(), [](const float component) { return std::isfinite(component); })) {
        return failure(diagnostic_codes::invalid_material, "material value " + std::string(binding)
            + " is not a finite value for a vector3 binding of renderer asset " + std::to_string(asset_id));
    }
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return failure(diagnostic_codes::backend_unavailable, "RenderingServer is unavailable");
    bound->value = assets::Vec3f{value[0], value[1], value[2]};
    const StringName name(bound->name.c_str());
    const Vector3 vector(value[0], value[1], value[2]);
    rendering->material_set_param(resource->second.material, name, vector);
    const auto consumer = fog_consumers_.find(asset_id);
    if (consumer != fog_consumers_.end() && consumer->second.material.is_valid()
        && consumer->second.material != resource->second.material)
        rendering->material_set_param(consumer->second.material, name, vector);
    return core::Result<void>::success();
}

[[nodiscard]] std::vector<GodotRenderer::SkinBindingEvidence> GodotRenderer::Impl::skin_bindings() const {
    std::vector<GodotRenderer::SkinBindingEvidence> result;
    for (const auto& [entity_id, instance] : instances_) {
        if (!instance.skeleton.is_valid()) continue;
        const auto pose = skin_poses_.find(entity_id);
        if (pose == skin_poses_.end() || pose->second.asset_id != instance.asset_id) continue;
        result.push_back({entity_id, instance.asset_id, pose->second.active_palette().size()});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.entity_id < right.entity_id;
    });
    return result;
}

[[nodiscard]] GodotRenderer::LifecycleCounts GodotRenderer::Impl::lifecycle_counts() const noexcept {
    GodotRenderer::LifecycleCounts counts{
        .assets = resources_.size(),
        .instances = instances_.size(),
        .skin_poses = skin_poses_.size(),
        .missing_asset_waits = missing_waits_.size(),
    };
    for (const auto& [entity, instance] : instances_) {
        static_cast<void>(entity);
        if (instance.skeleton.is_valid()) ++counts.skeletons;
    }
    return counts;
}

[[nodiscard]] std::vector<GodotRenderer::InstanceEvidence> GodotRenderer::Impl::instance_evidence() const {
    std::vector<GodotRenderer::InstanceEvidence> result;
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return result;
    const Transform3D identity;
    for (const auto& [entity_id, instance] : instances_) {
        GodotRenderer::InstanceEvidence evidence{.entity_id = entity_id, .asset_id = instance.asset_id};
        evidence.visible = instance.visible;
        evidence.mesh_surfaces = rendering->mesh_get_surface_count(resources_.at(instance.asset_id).mesh);
        if (instance.skeleton.is_valid()) {
            evidence.skeleton_bones = rendering->skeleton_get_bone_count(instance.skeleton);
            for (std::int64_t bone = 0; bone < evidence.skeleton_bones; ++bone) {
                const Transform3D transform = rendering->skeleton_bone_get_transform(
                    instance.skeleton, static_cast<std::int32_t>(bone));
                evidence.skeleton_posed = evidence.skeleton_posed || !transform.is_equal_approx(identity);
            }
        }
        result.push_back(evidence);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.entity_id < right.entity_id;
    });
    return result;
}

std::optional<std::array<float, 12>> GodotRenderer::Impl::bone_evidence(
    const sim::EntityId entity, const std::int32_t bone) const {
    RenderingServer* rendering = RenderingServer::get_singleton();
    const auto instance = instances_.find(entity);
    if (!rendering || instance == instances_.end() || !instance->second.skeleton.is_valid()
        || bone < 0 || bone >= rendering->skeleton_get_bone_count(instance->second.skeleton)) return std::nullopt;
    const auto transform = rendering->skeleton_bone_get_transform(instance->second.skeleton, bone);
    const auto x = transform.basis.get_column(0);
    const auto y = transform.basis.get_column(1);
    const auto z = transform.basis.get_column(2);
    return std::array<float, 12>{static_cast<float>(x.x), static_cast<float>(x.y), static_cast<float>(x.z),
        static_cast<float>(y.x), static_cast<float>(y.y), static_cast<float>(y.z),
        static_cast<float>(z.x), static_cast<float>(z.y), static_cast<float>(z.z),
        static_cast<float>(transform.origin.x), static_cast<float>(transform.origin.y), static_cast<float>(transform.origin.z)};
}

void GodotRenderer::Impl::set_instance_visible(const sim::EntityId entity, const bool visible) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    const auto instance = instances_.find(entity);
    if (!rendering || instance == instances_.end() || instance->second.visible == visible) return;
    rendering->instance_set_visible(instance->second.rid, visible);
    instance->second.visible = visible;
}

void GodotRenderer::Impl::submit(std::shared_ptr<const sim::RenderSnapshot> snapshot) {
    if (!snapshot) {
        submission_evidence_.clear();
        order_.invalidate();
        return;
    }
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !scenario_.is_valid()) {
        submission_evidence_.clear();
        order_.invalidate();
        return;
    }
    // GM-04: once frames are drawn the device's counter is live; a battle that loaded over
    // budget steps its textures down here (measured every few frames, see the budget).
    enforce_texture_budget(*rendering);
    // #888: only what changed reaches Godot. The pass order is kept while the
    // snapshot's (entity, asset) sequence and the uploads are unchanged, and a
    // piece is sent only when its fixed transform differs from the one it last
    // sent; the fixed-to-float bridge (adapt_snapshot's conversion) runs for
    // those pieces only.
    const std::span<const sim::RenderInstance> instances = snapshot->instances();
    if (residency_.active()) reconcile_residency(*rendering, instances);
    ++work_.submits;
    work_.pieces += instances.size();
    // This is the project-owned submission dependency. It combines with
    // material priority below; Godot remains responsible for the final
    // depth and blend ordering within each class.
    const bool reordered = order_.plan(instances, upload_generation_, [this](const sim::AssetId asset) {
        const auto resource = resources_.find(asset);
        return resource == resources_.end() ? std::nullopt : std::optional<RenderPass>(resource->second.pass);
    }, work_);
    const std::span<const detail::PlannedPiece> pieces = order_.pieces();
    if (reordered) {
        submission_evidence_.clear();
        submission_evidence_.reserve(pieces.size());
        order_resources_.assign(pieces.size(), nullptr);
        for (std::size_t index = 0; index < pieces.size(); ++index) {
            if (!pieces[index].uploaded) continue;
            const sim::RenderInstance& source = instances[pieces[index].source];
            auto& resource = resources_.at(source.asset_id);
            if (!resource.mesh.is_valid()) continue;
            order_resources_[index] = &resource;
            submission_evidence_.push_back({source.entity_id, source.asset_id, pieces[index].pass});
        }
    }
    // A billboard turns with its piece, its pose (set_skin_pose), the camera
    // (set_camera turns every one) and the light; the submit handles the
    // piece and a new light.
    const bool billboard_light_changed = billboard_generation_ != billboard_applied_generation_;
    billboard_applied_generation_ = billboard_generation_;
    detail::SubmissionPresence presence(++submit_serial_);
    missing_waits_.begin();
    const auto category_frame = render_categories::enabled
        ? static_cast<std::uint64_t>(Engine::get_singleton()->get_process_frames()) : 0;
    for (std::size_t index = 0; index < pieces.size(); ++index) {
        const sim::RenderInstance& source = instances[pieces[index].source];
        Resource* const resource = order_resources_[index];
        auto instance = instances_.find(source.entity_id);
        const std::optional<sim::AssetId> current_asset = instance == instances_.end()
            ? std::nullopt
            : std::optional<sim::AssetId>(instance->second.asset_id);
        const std::optional<sim::AssetId> requested_asset = resource == nullptr
            ? std::nullopt
            : std::optional<sim::AssetId>(source.asset_id);
        const detail::InstanceTransition transition =
            detail::reconcile_instance(current_asset, requested_asset);
        if (resource == nullptr) {
            if (missing_waits_.wait(source.entity_id, source.asset_id)) {
                fail(diagnostic_codes::missing_asset,
                    "snapshot references unloaded renderer asset "
                        + std::to_string(source.asset_id));
            }
            if (transition == detail::InstanceTransition::remove) {
                presence.remove(instance->second.placement);
                static_cast<void>(remove_instance(rendering, instance));
            }
            continue;
        }
        if (transition == detail::InstanceTransition::create) {
            const RID rid = rendering->instance_create();
            rendering->instance_set_base(rid, resource->mesh);
            rendering->instance_set_scenario(rid, scenario_);
            if (non_casting_.contains(source.asset_id)) {
                rendering->instance_geometry_set_cast_shadows_setting(
                    rid, RenderingServer::SHADOW_CASTING_SETTING_OFF);
            }
            instance = instances_.emplace(
                source.entity_id, Instance{.asset_id = source.asset_id, .rid = rid,
                    .skeleton = {}, .object_transform = {}, .placement = {}}).first;
            if (render_categories::enabled) {
                const auto role = category_roles_.find(source.entity_id);
                instance->second.category_role = role == category_roles_.end() ? render_categories::Role::unknown : role->second;
            }
            const auto pending = skin_poses_.find(source.entity_id);
            bind_instance_skin(*rendering, instance->second, *resource,
                pending == skin_poses_.end() ? nullptr : &pending->second);
            if (light_scales_.contains(source.entity_id)) {
                apply_light_scale(*rendering, source.entity_id, instance->second);
            }
            if (opacities_.contains(source.entity_id)) {
                apply_unit_opacity(*rendering, source.entity_id, instance->second);
            }
            apply_unit_colorization(*rendering, source.entity_id, instance->second);
        } else if (transition == detail::InstanceTransition::replace) {
            if (instance->second.skeleton.is_valid()) {
                render_churn::record(render_churn::owner(instance->second.category_role, true), render_churn::Event::freed);
                rendering->free_rid(instance->second.skeleton);
                instance->second.skeleton = {};
            }
            rendering->instance_set_base(instance->second.rid, resource->mesh);
            instance->second.asset_id = source.asset_id;
            const auto pending = skin_poses_.find(source.entity_id);
            bind_instance_skin(*rendering, instance->second, *resource,
                pending == skin_poses_.end() ? nullptr : &pending->second);
        }
        presence.retain(instance->second.placement);
        if (instance->second.reserved_proxy && !instance->second.visible) {
            rendering->instance_set_visible(instance->second.rid, true);
            instance->second.visible = true;
        }
        const bool rebound = transition != detail::InstanceTransition::retain;
        if (!rebound) {
            render_churn::record(render_churn::owner(instance->second.category_role), render_churn::Event::retained);
            if (instance->second.skeleton.is_valid())
                render_churn::record(render_churn::owner(instance->second.category_role, true), render_churn::Event::retained);
        }
        const bool moved = detail::place_piece(instance->second.placement, rebound, source.fixed_transform);
        if (moved) {
            instance->second.object_transform = transform_from(adapt_instance(source).column_major);
            rendering->instance_set_transform(instance->second.rid, instance->second.object_transform);
            ++work_.transforms_sent;
        }
        if (!resource->billboard_modes.empty() && (moved || billboard_light_changed)) {
            refresh_billboards(*rendering, source.entity_id, instance->second, *resource);
            ++work_.billboard_refreshes;
        }
        if (render_categories::enabled) {
            instance->second.category_submit_frame = category_frame;
            // Surfaces are immutable for this upload; query only on create/replacement.
            if (transition != detail::InstanceTransition::retain) {
                instance->second.category_surfaces = rendering->mesh_get_surface_count(resource->mesh);
                const auto role = category_roles_.find(source.entity_id);
                instance->second.category_role = role == category_roles_.end() ? render_categories::Role::unknown : role->second;
                const auto opacity = opacities_.find(source.entity_id);
                instance->second.category_casts = !non_casting_.contains(source.asset_id)
                    && (opacity == opacities_.end() || opacity->second >= 1.0F);
            }
        }
    }
    missing_waits_.end();
    // Every live instance this snapshot carried is stamped; only when some
    // were not does the scan for the ones that left run.
    presence.sweep(instances_, [&](const InstanceMap::iterator current) {
        if (current->second.reserved_proxy) {
            if (current->second.visible) {
                rendering->instance_set_visible(current->second.rid, false);
                current->second.visible = false;
            }
            return std::next(current);
        }
        return remove_instance(rendering, current);
    }, work_);
    if (fog_) {
        // Only the immutable grid set is kept (for a team switch without a
        // new snapshot); camera, transform and tick changes with identical
        // grids reach the cache as `unchanged` and upload nothing.
        fog_->grids = snapshot->fog_grids();
        fog_->submitted_tick = snapshot->completed_tick();
        apply_fog();
    }
    // The shared_ptr dies here. The backend never stores a mutable simulation
    // object or a snapshot beyond this submission boundary.
}

// Frees the entity's instance and skeleton RIDs. Its pose stays: a caller
// may pose an entity once and resubmit it after an absence (map mode's
// terrain-only comparison phase does), so absence cannot end a pose.
// Poses end with their asset (release), with clear_skin_pose, or are
// replaced by set_skin_pose.
GodotRenderer::Impl::InstanceMap::iterator GodotRenderer::Impl::remove_instance(RenderingServer* rendering, const InstanceMap::iterator instance) {
    // All retirement paths (including residency eviction and asset unload)
    // end their proxy reservation at this single removal point.
    if (const auto charge = proxy_.charges.find(instance->first); charge != proxy_.charges.end()) {
        proxy_.budget->release(ResourceKind::gpu_proxy, charge->second);
        proxy_.charges.erase(charge);
    }
    if (rendering && instance->second.rid.is_valid()) {
        render_churn::record(render_churn::owner(instance->second.category_role), render_churn::Event::freed);
        rendering->free_rid(instance->second.rid);
    }
    if (rendering && instance->second.skeleton.is_valid()) {
        render_churn::record(render_churn::owner(instance->second.category_role, true), render_churn::Event::freed);
        rendering->free_rid(instance->second.skeleton);
    }
    return instances_.erase(instance);
}

[[nodiscard]] bool GodotRenderer::Impl::apply_skin_pose(
    RenderingServer& rendering, const Instance& instance, const PendingPose& pose,
    const std::span<const std::uint8_t> changes, SkinPoseWork* work) {
    SkinPoseTimer upload(work ? &work->upload_ms : nullptr);
    if (!instance.skeleton.is_valid()) return false;
    const auto& palette = pose.active_palette();
    if (!changes.empty() && changes.size() != palette.size()) return false;
    bool uploaded = false;
    for (std::size_t index = 0; index < palette.size(); ++index) {
        if (!changes.empty() && changes[index] == 0) continue;
        if (work && !uploaded) ++work->palette_uploads;
        uploaded = true;
        if (work) ++work->bone_uploads;
        const auto transform = transform_from(palette[index]);
        rendering.skeleton_bone_set_transform(instance.skeleton,
            static_cast<std::int32_t>(index), transform);
    }
    return true;
}

void GodotRenderer::Impl::refresh_billboards(RenderingServer& rendering, const sim::EntityId entity,
                        const Instance& instance, const Resource& resource) {
    if (!instance.skeleton.is_valid() || resource.billboard_modes.empty()) return;
    const float determinant = instance.object_transform.basis.determinant();
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8F) return;
    const auto pending = skin_poses_.find(entity);
    const PendingPose* pose = pending != skin_poses_.end() && pending->second.asset_id == instance.asset_id
        ? &pending->second : nullptr;
    billboard_pose_.reset(pose ? pose->active_models() : resource.bind_models,
        pose ? std::span<const animation::Matrix>(pose->active_palette()) : std::span<const animation::Matrix>{},
        Transform3D(), [](const animation::Matrix& matrix) { return transform_from(matrix); });
    auto& models = billboard_pose_.models;
    auto& palettes = billboard_pose_.palettes;
    const Basis object_inverse = instance.object_transform.basis.inverse();
    const Vector3 eye = instance.object_transform.affine_inverse().xform(view_transform_.origin);
    const Vector3 view_up = object_inverse.xform(view_transform_.basis.get_column(1)).normalized();
    const Vector3 view_back = object_inverse.xform(view_transform_.basis.get_column(2)).normalized();
    for (std::size_t bone = 0; bone < resource.bone_count; ++bone) {
        const std::uint32_t mode = resource.billboard_modes[bone] & 15U;
        // Light-axis and wind-axis bones need external vectors; leave
        // their authored pose intact until those inputs are available.
        if (mode == 0 || mode == 4 || mode == 5 || mode > 7) continue;
        const Transform3D& current = models[bone];
        const std::int32_t parent = resource.bone_parents[bone];
        const Vector3 pivot = mode == 6
            ? (parent >= 0 ? models[static_cast<std::size_t>(parent)].origin : Vector3())
            : current.origin;
        const Vector3 toward = (eye - pivot).normalized();
        Vector3 up = view_up;
        Vector3 back = (mode == 2 || mode == 3 || mode == 6) ? toward : view_back;
        if (mode == 3) {
            // Z-axis modes retain the authored source Z axis (render Y).
            up = current.basis.get_column(1).normalized();
            back = (back - up * back.dot(up)).normalized();
        }
        Vector3 right = up.cross(back).normalized();
        if (!right.is_finite() || right.length_squared() < 1e-8F
            || !back.is_finite() || back.length_squared() < 1e-8F) continue;
        up = back.cross(right).normalized();
        Vector3 scale{current.basis.get_column(0).length(),
            current.basis.get_column(1).length(), current.basis.get_column(2).length()};
        Vector3 origin = pivot;
        if (mode == 6 && resource.billboard_light) {
            const Vector3 light = object_inverse.xform(*resource.billboard_light).normalized();
            const auto shift = space::sunlight_glow_offset(
                {static_cast<float>(light.x), static_cast<float>(light.y), static_cast<float>(light.z)},
                {static_cast<float>(back.x), static_cast<float>(back.y), static_cast<float>(back.z)},
                resource.billboard_distances[bone], static_cast<float>(scale.z));
            if (shift) origin += Vector3(shift->x, shift->y, shift->z);
        }
        const Transform3D desired(Basis(right * scale.x, up * scale.y, back * scale.z), origin);
        if (std::abs(current.basis.determinant()) < 1e-8F) continue;
        const Transform3D delta = desired * current.affine_inverse();
        // The palette is animated absolute * inverse bind. Move children
        // with the billboard parent, including rigid particle attachments.
        static_cast<void>(visit_skin_descendants(resource.bone_parents, bone, [&](const std::size_t child) {
            palettes[child] = delta * palettes[child];
            models[child] = delta * models[child];
        }));
    }
    SkinPoseTimer upload(skin_profile_ ? &skin_work_.upload_ms : nullptr);
    if (skin_profile_ && resource.bone_count) ++skin_work_.billboard_uploads;
    for (std::size_t bone = 0; bone < resource.bone_count; ++bone) {
        if (skin_profile_) ++skin_work_.bone_uploads;
        rendering.skeleton_bone_set_transform(instance.skeleton,
            static_cast<std::int32_t>(bone), palettes[bone]);
    }
}

void GodotRenderer::Impl::bind_instance_skin(
    RenderingServer& rendering,
    Instance& instance,
    const Resource& resource,
    const PendingPose* pose) {
    if (!resource.skinned || resource.bone_count == 0) return;
    instance.skeleton = rendering.skeleton_create();
    if (!instance.skeleton.is_valid()) return;
    rendering.skeleton_allocate_data(instance.skeleton,
        static_cast<std::int32_t>(resource.bone_count), false);
    // Proxy reservation needs a typed allocation failure; retain the existing
    // non-proxy skin path's behavior.
    if (instance.reserved_proxy
        && static_cast<std::size_t>(rendering.skeleton_get_bone_count(instance.skeleton)) != resource.bone_count) {
        rendering.free_rid(instance.skeleton);
        instance.skeleton = {};
        return;
    }
    rendering.instance_attach_skeleton(instance.rid, instance.skeleton);
    const Transform3D identity;
    for (std::size_t index = 0; index < resource.bone_count; ++index) {
        rendering.skeleton_bone_set_transform(instance.skeleton,
            static_cast<std::int32_t>(index), identity);
    }
    if (pose && pose->asset_id == instance.asset_id
        && pose->active_palette().size() == resource.bone_count) {
        static_cast<void>(apply_skin_pose(rendering, instance, *pose));
    }
}

} // namespace eawr::presentation::godot_backend
