#include "renderer_internal.hpp"
#include <godot_cpp/variant/utility_functions.hpp>

#include <bit>
#include <cstring>

#include <godot_cpp/variant/packed_float32_array.hpp>

// Instanced hull submission (opt-in, default off): rest-pose, opaque, identically stated pieces of
// one asset share one multimesh draw instead of one mesh instance each. The retained per-entity
// Instance stays (hidden while batched), so every per-entity path keeps working and leaving a
// group is a visibility flip. Presentation only; nothing here reads or writes simulation state.

namespace eawr::presentation::godot_backend {

namespace {
using hull_batching::Key;
using Change = hull_batching::Registry::Change;

constexpr std::uint32_t first_capacity = 8;
constexpr std::uint64_t audit_period = 64;

std::uint32_t bits(const float value) { return std::bit_cast<std::uint32_t>(value); }

void store_transform(float* destination, const Transform3D& transform) {
    for (int row = 0; row < 3; ++row) {
        destination[row * 4 + 0] = static_cast<float>(transform.basis.rows[row][0]);
        destination[row * 4 + 1] = static_cast<float>(transform.basis.rows[row][1]);
        destination[row * 4 + 2] = static_cast<float>(transform.basis.rows[row][2]);
    }
    destination[3] = static_cast<float>(transform.origin.x);
    destination[7] = static_cast<float>(transform.origin.y);
    destination[11] = static_cast<float>(transform.origin.z);
}

std::array<float, 4> world_sphere(const std::array<float, 4>& local, const Transform3D& transform) {
    const Vector3 centre = transform.xform(Vector3(local[0], local[1], local[2]));
    const float scale = static_cast<float>(std::max({transform.basis.get_column(0).length(),
        transform.basis.get_column(1).length(), transform.basis.get_column(2).length()}));
    return {static_cast<float>(centre.x), static_cast<float>(centre.y), static_cast<float>(centre.z), local[3] * scale};
}
} // namespace

bool GodotRenderer::Impl::batch_active() const noexcept {
    return hull_batching::enabled && (render_categories::ablation == "off" || render_categories::ablation == "none");
}

void GodotRenderer::Impl::batch_mark(const sim::EntityId entity) {
    if (!batch_active()) return;
    const auto instance = instances_.find(entity);
    if (instance == instances_.end() || instance->second.batch_dirty) return;
    instance->second.batch_dirty = true;
    batch_dirty_.push_back(entity);
}

void GodotRenderer::Impl::batch_mark_asset(const sim::AssetId asset) {
    if (!batch_active()) return;
    for (const auto& [entity, instance] : instances_)
        if (instance.asset_id == asset) batch_mark(entity);
}

bool GodotRenderer::Impl::rest_pose(const sim::EntityId entity, const Instance& instance, const Resource& resource) const {
    // Without an applied pose bind_instance_skin leaves every bone at identity: the bind-pose mesh.
    const auto pose = skin_poses_.find(entity);
    if (pose == skin_poses_.end() || !resource.skinned || pose->second.asset_id != instance.asset_id) return true;
    const auto& palette = pose->second.active_palette();
    if (palette.size() != resource.bone_count) return true;
    const Transform3D identity;
    for (std::size_t bone = 0; bone < palette.size(); ++bone) {
        if (bone >= resource.skin_used_bones.size() || !resource.skin_used_bones[bone]) continue;
        if (!transform_from(palette[bone]).is_equal_approx(identity)) return false;
    }
    return true;
}

std::optional<Key> GodotRenderer::Impl::batch_key(const sim::EntityId entity, const Instance& instance, bool* pose_exit) const {
    if (pose_exit) *pose_exit = false;
    if (!batch_active() || instance.reserved_proxy || !instance.visible || !instance.rid.is_valid()) return std::nullopt;
    const auto found = resources_.find(instance.asset_id);
    if (found == resources_.end()) return std::nullopt;
    const Resource& resource = found->second;
    if (instance.translated_parameters || batch_translated_asset(instance.asset_id)) return std::nullopt;
    if (!resource.mesh.is_valid() || !resource.batch_bounds || resource.pass != RenderPass::opaque
        || !resource.billboard_modes.empty() || opacities_.contains(entity)) return std::nullopt;
    if (!rest_pose(entity, instance, resource)) {
        if (pose_exit) *pose_exit = true;
        return std::nullopt;
    }
    ++batch_counters_.key_builds;
    Key key;
    key.asset = instance.asset_id;
    key.casts = instance.created_casts && render_categories::engine_casts(true);
    if (const auto colour = colorizations_.find(entity); colour != colorizations_.end()) {
        key.has_colour = true;
        key.colour = {bits(colour->second[0]), bits(colour->second[1]), bits(colour->second[2])};
    }
    if (const auto light = light_scales_.find(entity); light != light_scales_.end()) {
        key.has_light = true;
        key.light = {bits(light->second[0]), bits(light->second[1]), bits(light->second[2])};
    }
    return key;
}

void GodotRenderer::Impl::batch_write_transform(const sim::EntityId entity, const Instance& instance) {
    static_cast<void>(entity);
    const auto group = batch_groups_.find(instance.batch_group);
    if (group == batch_groups_.end() || instance.batch_slot >= group->second.members) return;
    auto& target = group->second;
    store_transform(target.transforms.data() + static_cast<std::size_t>(instance.batch_slot) * 12, instance.object_transform);
    target.spheres[instance.batch_slot] = world_sphere(target.local_sphere, instance.object_transform);
    target.dirty = true;
}

void GodotRenderer::Impl::batch_apply(RenderingServer& rendering, std::vector<Change> changes) {
    for (const Change& change : changes) {
        switch (change.kind) {
        case Change::Kind::create: {
            const auto resource = resources_.find(change.key.asset);
            if (resource == resources_.end()) break;
            BatchGroup group;
            group.asset = change.key.asset;
            group.casts = change.key.casts;
            group.capacity = first_capacity;
            const AABB& bounds = *resource->second.batch_bounds;
            const Vector3 centre = bounds.get_center();
            group.local_sphere = {static_cast<float>(centre.x), static_cast<float>(centre.y), static_cast<float>(centre.z),
                static_cast<float>(bounds.size.length() * 0.5)};
            group.transforms.assign(static_cast<std::size_t>(group.capacity) * 12, 0.0F);
            group.spheres.assign(group.capacity, {});
            group.multimesh = rendering.multimesh_create();
            rendering.multimesh_allocate_data(group.multimesh, static_cast<int32_t>(group.capacity),
                RenderingServer::MULTIMESH_TRANSFORM_3D, false, false);
            rendering.multimesh_set_mesh(group.multimesh, resource->second.mesh);
            rendering.multimesh_set_visible_instances(group.multimesh, 0);
            group.instance = rendering.instance_create();
            rendering.instance_set_base(group.instance, group.multimesh);
            rendering.instance_set_scenario(group.instance, scenario_);
            if (!change.key.casts)
                rendering.instance_geometry_set_cast_shadows_setting(group.instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
            if (change.key.has_colour)
                rendering.instance_geometry_set_shader_parameter(group.instance, StringName("eawr_unit_colorization"),
                    Vector4(std::bit_cast<float>(change.key.colour[0]), std::bit_cast<float>(change.key.colour[1]),
                        std::bit_cast<float>(change.key.colour[2]), 1.0F));
            if (change.key.has_light)
                rendering.instance_geometry_set_shader_parameter(group.instance, StringName("eawr_unit_light_scale"),
                    Vector3(std::bit_cast<float>(change.key.light[0]), std::bit_cast<float>(change.key.light[1]),
                        std::bit_cast<float>(change.key.light[2])));
            batch_groups_.emplace(change.group, std::move(group));
            break;
        }
        case Change::Kind::join: {
            const auto group = batch_groups_.find(change.group);
            const auto instance = instances_.find(change.entity);
            if (group == batch_groups_.end() || instance == instances_.end()) break;
            auto& target = group->second;
            if (change.slot >= target.capacity) {
                target.capacity = std::max(target.capacity * 2, change.slot + 1);
                target.transforms.resize(static_cast<std::size_t>(target.capacity) * 12, 0.0F);
                target.spheres.resize(target.capacity);
                rendering.multimesh_allocate_data(target.multimesh, static_cast<int32_t>(target.capacity),
                    RenderingServer::MULTIMESH_TRANSFORM_3D, false, false);
            }
            ++target.members;
            instance->second.batch_group = change.group;
            instance->second.batch_slot = change.slot;
            instance->second.batch_hidden = true;
            apply_visibility(rendering, instance->second, instance->second.visible);
            batch_write_transform(change.entity, instance->second);
            target.surfaces = std::max<std::uint64_t>(target.surfaces, instance->second.category_surfaces);
            break;
        }
        case Change::Kind::leave: {
            const auto instance = instances_.find(change.entity);
            if (instance != instances_.end() && !change.forgotten) {
                instance->second.batch_group = 0;
                instance->second.batch_hidden = false;
                rendering.instance_set_transform(instance->second.rid, instance->second.object_transform);
                apply_visibility(rendering, instance->second, instance->second.visible);
            } else if (instance != instances_.end()) {
                instance->second.batch_group = 0;
                instance->second.batch_hidden = false;
            }
            const auto group = batch_groups_.find(change.group);
            if (group == batch_groups_.end()) break;
            auto& target = group->second;
            const std::uint32_t last = target.members - 1;
            if (change.moved) {
                std::memcpy(target.transforms.data() + static_cast<std::size_t>(change.slot) * 12,
                    target.transforms.data() + static_cast<std::size_t>(last) * 12, 12 * sizeof(float));
                target.spheres[change.slot] = target.spheres[last];
                const auto moved = instances_.find(*change.moved);
                if (moved != instances_.end()) moved->second.batch_slot = change.slot;
            }
            --target.members;
            target.dirty = true;
            break;
        }
        case Change::Kind::drop: {
            const auto group = batch_groups_.find(change.group);
            if (group == batch_groups_.end()) break;
            if (group->second.instance.is_valid()) rendering.free_rid(group->second.instance);
            if (group->second.multimesh.is_valid()) rendering.free_rid(group->second.multimesh);
            batch_groups_.erase(group);
            break;
        }
        }
    }
}

void GodotRenderer::Impl::batch_update(RenderingServer& rendering, const bool advance) {
    if (!batch_active()) return;
    if (advance) ++batch_serial_;
    std::vector<sim::EntityId> dirty;
    dirty.swap(batch_dirty_);
    for (const auto entity : dirty) {
        const auto instance = instances_.find(entity);
        if (instance == instances_.end()) continue;
        instance->second.batch_dirty = false;
        bool pose_exit = false;
        const auto key = batch_key(entity, instance->second, &pose_exit);
        batch_registry_.observe(entity, key, batch_serial_, pose_exit);
    }
    // Missed-hook safety net: a group's members must still match its key; repair and count.
    if (advance && batch_serial_ % audit_period == 0) {
        ++batch_counters_.audits;
        struct Repair { sim::EntityId entity; std::optional<Key> key; bool pose_exit; };
        std::vector<Repair> repairs;
        batch_registry_.each_group([&](std::uint32_t id, const Key& key, std::size_t) {
            const auto* members = batch_registry_.member_list(id);
            for (const auto entity : *members) {
                const auto instance = instances_.find(entity);
                if (instance == instances_.end()) continue;
                bool pose_exit = false;
                const auto current = batch_key(entity, instance->second, &pose_exit);
                if (!current || !(*current == key)) repairs.push_back({entity, current, pose_exit});
            }
        });
        for (const auto& [entity, key, pose_exit] : repairs) {
            ++batch_counters_.repairs;
            batch_registry_.observe(entity, key, batch_serial_, pose_exit);
        }
    }
    batch_apply(rendering, batch_registry_.reconcile(batch_serial_));
    if (advance && batch_serial_ == 120) {
        const auto message = std::string("EAWR instanced hulls: renderer active, members=")
            + std::to_string(batch_registry_.member_total());
        godot::UtilityFunctions::print(godot::String::utf8(message.c_str()));
    }
    for (auto& [id, group] : batch_groups_) {
        static_cast<void>(id);
        if (!group.dirty) continue;
        group.dirty = false;
        ++batch_counters_.flushes;
        PackedFloat32Array buffer;
        buffer.resize(static_cast<int64_t>(group.capacity) * 12);
        std::memcpy(buffer.ptrw(), group.transforms.data(), group.transforms.size() * sizeof(float));
        rendering.multimesh_set_buffer(group.multimesh, buffer);
        rendering.multimesh_set_visible_instances(group.multimesh, static_cast<int32_t>(group.members));
        if (group.members) {
            Vector3 low(1e30F, 1e30F, 1e30F), high(-1e30F, -1e30F, -1e30F);
            for (std::uint32_t slot = 0; slot < group.members; ++slot) {
                const auto& sphere = group.spheres[slot];
                low = Vector3(std::min<real_t>(low.x, sphere[0] - sphere[3]), std::min<real_t>(low.y, sphere[1] - sphere[3]),
                    std::min<real_t>(low.z, sphere[2] - sphere[3]));
                high = Vector3(std::max<real_t>(high.x, sphere[0] + sphere[3]), std::max<real_t>(high.y, sphere[1] + sphere[3]),
                    std::max<real_t>(high.z, sphere[2] + sphere[3]));
            }
            rendering.multimesh_set_custom_aabb(group.multimesh, AABB(low, high - low));
        }
        ++batch_counters_.uploads;
    }
}

void GodotRenderer::Impl::batch_forget(RenderingServer* rendering, const sim::EntityId entity, Instance& instance) {
    if (!hull_batching::enabled) return;
    batch_registry_.forget(entity);
    instance.batch_dirty = false;
    if (rendering) batch_apply(*rendering, batch_registry_.take_changes());
    else static_cast<void>(batch_registry_.take_changes());
}

void GodotRenderer::Impl::batch_shutdown(RenderingServer& rendering) {
    for (auto& [id, group] : batch_groups_) {
        static_cast<void>(id);
        if (group.instance.is_valid()) rendering.free_rid(group.instance);
        if (group.multimesh.is_valid()) rendering.free_rid(group.multimesh);
    }
    batch_groups_.clear();
}

void GodotRenderer::Impl::batch_fill_sample(render_categories::Sample& sample) const {
    if (!hull_batching::enabled) return;
    sample.batch_enabled = true;
    sample.batch_active = batch_active();
    std::uint64_t collapsed = 0;
    for (const auto& [id, group] : batch_groups_) {
        static_cast<void>(id);
        sample.batch_groups += 1;
        sample.batch_members += group.members;
        if (group.members) collapsed += static_cast<std::uint64_t>(group.members - 1) * group.surfaces;
    }
    sample.batch_collapsed_surfaces += collapsed;
    const auto& stats = batch_registry_.stats();
    sample.batch_joins += stats.joins;
    sample.batch_leaves += stats.leaves;
    sample.batch_sticky += stats.sticky_blocks;
    sample.batch_flushes += batch_counters_.flushes;
    sample.batch_repairs += batch_counters_.repairs;
    sample.batch_key_builds += batch_counters_.key_builds;
}

} // namespace eawr::presentation::godot_backend
