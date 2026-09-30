#include "renderer_internal.hpp"

namespace eawr::presentation::godot_backend {

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
    PendingPose pose{.asset_id = asset_id};
    pose.palette.reserve(bones.size());
    pose.model_transforms.reserve(bones.size());
    for (const animation::BonePose& bone : bones) {
        if (!std::all_of(bone.skin_asset.begin(), bone.skin_asset.end(),
                [](const float value) { return std::isfinite(value); })
            || !std::all_of(bone.model_asset.begin(), bone.model_asset.end(),
                [](const float value) { return std::isfinite(value); })) {
            return failure(diagnostic_codes::invalid_skin_pose,
                "skin pose contains a non-finite palette matrix");
        }
        pose.palette.push_back(animation::Player::asset_to_render_transform(bone.skin_asset));
        pose.model_transforms.push_back(transform_from(
            animation::Player::asset_to_render_transform(bone.model_asset)));
    }
    skin_poses_[entity_id] = std::move(pose);
    const auto instance = instances_.find(entity_id);
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (instance != instances_.end() && instance->second.asset_id == asset_id && rendering) {
        if (!apply_skin_pose(*rendering, instance->second, skin_poses_.at(entity_id))) {
            return failure(diagnostic_codes::invalid_skin_pose,
                "Godot skeleton palette binding failed");
        }
        if (!resource->second.billboard_modes.empty()) {
            refresh_billboards(*rendering, entity_id, instance->second, resource->second);
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

// #535: the fog fade's alpha (space-fog-presentation.md FW-16 to FW-18): the unit's own opacity, an
// instance shader parameter the ship hull adapters dither with (the same surfaces the unit light
// scale reaches; a shader without the uniform is unchanged). Godot's per-instance geometry
// transparency is not used: it sends the instance through the transparent pass, where the sky's
// transparent layers (drawn later, with no depth to test against) paint over a hull seen against
// open space.
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
    rendering.instance_geometry_set_shader_parameter(instance.rid, StringName("eawr_unit_opacity"), alpha);
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

[[nodiscard]] std::vector<GodotRenderer::SkinBindingEvidence> GodotRenderer::Impl::skin_bindings() const {
    std::vector<GodotRenderer::SkinBindingEvidence> result;
    for (const auto& [entity_id, instance] : instances_) {
        if (!instance.skeleton.is_valid()) continue;
        const auto pose = skin_poses_.find(entity_id);
        if (pose == skin_poses_.end() || pose->second.asset_id != instance.asset_id) continue;
        result.push_back({entity_id, instance.asset_id, pose->second.palette.size()});
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

void GodotRenderer::Impl::submit(std::shared_ptr<const sim::RenderSnapshot> snapshot) {
    submission_evidence_.clear();
    if (!snapshot) return;
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !scenario_.is_valid()) return;
    const std::vector<PresentationTransform> transforms = adapt_snapshot(*snapshot);
    // This is the project-owned submission dependency. It combines with
    // material priority below; Godot remains responsible for the final
    // depth and blend ordering within each class.
    std::vector<detail::RoutedTransform> routed;
    routed.reserve(transforms.size());
    for (const PresentationTransform& transform : transforms) {
        const auto resource = resources_.find(transform.asset_id);
        routed.push_back({transform, resource == resources_.end()
            ? RenderPass::post : resource->second.pass});
    }
    detail::order_pass_submissions(routed);
    submission_evidence_.reserve(routed.size());
    for (const detail::RoutedTransform& submission : routed) {
        const auto resource = resources_.find(submission.transform.asset_id);
        if (resource == resources_.end()) continue;
        submission_evidence_.push_back({submission.transform.entity_id,
            submission.transform.asset_id, submission.pass});
    }
    std::unordered_map<sim::EntityId, bool> live;
    live.reserve(routed.size());
    missing_waits_.begin();
    for (const detail::RoutedTransform& submission : routed) {
        const PresentationTransform& transform = submission.transform;
        const auto resource = resources_.find(transform.asset_id);
        auto instance = instances_.find(transform.entity_id);
        const std::optional<sim::AssetId> current_asset = instance == instances_.end()
            ? std::nullopt
            : std::optional<sim::AssetId>(instance->second.asset_id);
        const std::optional<sim::AssetId> requested_asset = resource == resources_.end()
            ? std::nullopt
            : std::optional<sim::AssetId>(transform.asset_id);
        const detail::InstanceTransition transition =
            detail::reconcile_instance(current_asset, requested_asset);
        if (resource == resources_.end()) {
            if (missing_waits_.wait(transform.entity_id, transform.asset_id)) {
                fail(diagnostic_codes::missing_asset,
                    "snapshot references unloaded renderer asset "
                        + std::to_string(transform.asset_id));
            }
            if (transition == detail::InstanceTransition::remove) {
                static_cast<void>(remove_instance(rendering, instance));
            }
            continue;
        }
        live.emplace(transform.entity_id, true);
        if (transition == detail::InstanceTransition::create) {
            const RID rid = rendering->instance_create();
            rendering->instance_set_base(rid, resource->second.mesh);
            rendering->instance_set_scenario(rid, scenario_);
            if (non_casting_.contains(transform.asset_id)) {
                rendering->instance_geometry_set_cast_shadows_setting(
                    rid, RenderingServer::SHADOW_CASTING_SETTING_OFF);
            }
            instance = instances_.emplace(
                transform.entity_id, Instance{transform.asset_id, rid, {}}).first;
            const auto pending = skin_poses_.find(transform.entity_id);
            bind_instance_skin(*rendering, instance->second, resource->second,
                pending == skin_poses_.end() ? nullptr : &pending->second);
            if (light_scales_.contains(transform.entity_id)) {
                apply_light_scale(*rendering, transform.entity_id, instance->second);
            }
            if (opacities_.contains(transform.entity_id)) {
                apply_unit_opacity(*rendering, transform.entity_id, instance->second);
            }
        } else if (transition == detail::InstanceTransition::replace) {
            if (instance->second.skeleton.is_valid()) {
                rendering->free_rid(instance->second.skeleton);
                instance->second.skeleton = {};
            }
            rendering->instance_set_base(instance->second.rid, resource->second.mesh);
            instance->second.asset_id = transform.asset_id;
            const auto pending = skin_poses_.find(transform.entity_id);
            bind_instance_skin(*rendering, instance->second, resource->second,
                pending == skin_poses_.end() ? nullptr : &pending->second);
        }
        rendering->instance_set_transform(
            instance->second.rid, transform_from(transform.column_major));
        instance->second.object_transform = transform_from(transform.column_major);
        if (!resource->second.billboard_modes.empty()) {
            refresh_billboards(*rendering, transform.entity_id, instance->second, resource->second);
        }
    }
    missing_waits_.end();
    for (auto current = instances_.begin(); current != instances_.end();) {
        current = live.contains(current->first)
            ? std::next(current) : remove_instance(rendering, current);
    }
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
    if (rendering && instance->second.rid.is_valid()) rendering->free_rid(instance->second.rid);
    if (rendering && instance->second.skeleton.is_valid()) rendering->free_rid(instance->second.skeleton);
    return instances_.erase(instance);
}

[[nodiscard]] bool GodotRenderer::Impl::apply_skin_pose(
    RenderingServer& rendering, const Instance& instance, const PendingPose& pose) {
    if (!instance.skeleton.is_valid()) return false;
    for (std::size_t index = 0; index < pose.palette.size(); ++index) {
        rendering.skeleton_bone_set_transform(instance.skeleton,
            static_cast<std::int32_t>(index), transform_from(pose.palette[index]));
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
    std::vector<Transform3D> models = pose ? pose->model_transforms : resource.bind_models;
    std::vector<Transform3D> palettes(resource.bone_count);
    for (std::size_t bone = 0; bone < resource.bone_count; ++bone) {
        palettes[bone] = pose ? transform_from(pose->palette[bone]) : Transform3D();
    }
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
        for (std::size_t child = 0; child < resource.bone_count; ++child) {
            std::int32_t ancestor = static_cast<std::int32_t>(child);
            while (ancestor >= 0 && ancestor != static_cast<std::int32_t>(bone)) {
                ancestor = resource.bone_parents[static_cast<std::size_t>(ancestor)];
            }
            if (ancestor == static_cast<std::int32_t>(bone)) {
                palettes[child] = delta * palettes[child];
                models[child] = delta * models[child];
            }
        }
    }
    for (std::size_t bone = 0; bone < resource.bone_count; ++bone) {
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
    rendering.skeleton_allocate_data(instance.skeleton,
        static_cast<std::int32_t>(resource.bone_count), false);
    rendering.instance_attach_skeleton(instance.rid, instance.skeleton);
    const Transform3D identity;
    for (std::size_t index = 0; index < resource.bone_count; ++index) {
        rendering.skeleton_bone_set_transform(instance.skeleton,
            static_cast<std::int32_t>(index), identity);
    }
    if (pose && pose->asset_id == instance.asset_id
        && pose->palette.size() == resource.bone_count) {
        static_cast<void>(apply_skin_pose(rendering, instance, *pose));
    }
}

} // namespace eawr::presentation::godot_backend
