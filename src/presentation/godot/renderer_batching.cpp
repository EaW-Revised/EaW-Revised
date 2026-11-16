#include "renderer_internal.hpp"
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <unordered_set>

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

GodotRenderer::Impl::BatchReject GodotRenderer::Impl::batch_reject(const sim::EntityId entity, const Instance& instance) const {
    if (!instance.visible) return BatchReject::hidden;
    if (instance.reserved_proxy) return BatchReject::reserved_proxy;
    if (!instance.rid.is_valid()) return BatchReject::no_rid;
    const auto found = resources_.find(instance.asset_id);
    if (found == resources_.end()) return BatchReject::no_resource;
    const Resource& resource = found->second;
    if (instance.translated_parameters || batch_translated_asset(instance.asset_id)) return BatchReject::translated;
    if (!resource.mesh.is_valid() || !resource.batch_bounds) return BatchReject::no_mesh;
    if (resource.pass != RenderPass::opaque) return BatchReject::non_opaque;
    if (!resource.billboard_modes.empty()) return BatchReject::billboard;
    if (opacities_.contains(entity)) return BatchReject::opacity;
    // A skeleton-free piece (its whole model on one bone) carries the bone in its group slot (object x bone), so a moved bone
    // does not keep it out of a group; every other piece must be at rest.
    if (instance.rigid_bone < 0 && !rest_pose(entity, instance, resource)) return BatchReject::posed;
    return BatchReject::eligible;
}

std::optional<Key> GodotRenderer::Impl::batch_key(const sim::EntityId entity, const Instance& instance, bool* pose_exit) const {
    if (pose_exit) *pose_exit = false;
    if (!batch_active()) return std::nullopt;
    const BatchReject reject = batch_reject(entity, instance);
    if (reject == BatchReject::posed) {
        if (pose_exit) *pose_exit = true;
        return std::nullopt;
    }
    if (reject != BatchReject::eligible) return std::nullopt;
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
    const Transform3D world = world_transform(instance);
    store_transform(target.transforms.data() + static_cast<std::size_t>(instance.batch_slot) * 12, world);
    target.spheres[instance.batch_slot] = world_sphere(target.local_sphere, world);
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
                rendering.instance_set_transform(instance->second.rid, world_transform(instance->second));
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

// B stage 0: why each retained piece does or does not draw through a hull group. Read-only; runs on the render
// counters' cadence (the source reader), never on the product path. The reason order is batch_reject's, so
// the census cannot drift from the key.
void GodotRenderer::Impl::batch_census(render_categories::BatchCensus& census) const {
    using Reason = render_categories::BatchReason;
    census = {};
    census.valid = true;
    census.skin_writes = skin_census_;
    census.submit_serial = batch_serial_;
    const bool batching = batch_active();
    std::unordered_set<sim::AssetId> assets, posed_assets;
    std::unordered_set<std::uint64_t> bone_sets;
    for (const auto& [entity, instance] : instances_) {
        const auto found = resources_.find(instance.asset_id);
        const Resource* const resource = found == resources_.end() ? nullptr : &found->second;
        if (instance.rigid_bone >= 0) ++census.skeleton_free_instances;
        if (instance.skeleton.is_valid()) {
            ++census.skeleton_instances;
            if (instance.shared_idle_skeleton) ++census.shared_idle_instances;
            if (resource) census.skeleton_bones += resource->bone_count;
        }
        const std::size_t role = std::min(static_cast<std::size_t>(instance.category_role), render_categories::role_count - 1);
        Reason reason = Reason::hidden;
        if (instance.visible) {
            const auto state = batch_registry_.state_of(entity, batch_serial_);
            if (batching && state == hull_batching::Registry::State::grouped) reason = Reason::batched;
            else switch (batch_reject(entity, instance)) {
            case BatchReject::hidden: reason = Reason::hidden; break;
            case BatchReject::reserved_proxy: reason = Reason::reserved_proxy; break;
            case BatchReject::no_rid: reason = Reason::no_rid; break;
            case BatchReject::no_resource: reason = Reason::no_resource; break;
            case BatchReject::no_mesh: reason = Reason::no_mesh; break;
            case BatchReject::translated: reason = Reason::translated; break;
            case BatchReject::non_opaque: reason = Reason::non_opaque; break;
            case BatchReject::billboard: reason = Reason::billboard; break;
            case BatchReject::opacity: reason = Reason::opacity; break;
            case BatchReject::posed: reason = Reason::posed; break;
            case BatchReject::eligible:
                if (!batching) reason = Reason::batch_off;
                else switch (state) {
                case hull_batching::Registry::State::blocked: reason = Reason::sticky; break;
                case hull_batching::Registry::State::alone: reason = Reason::alone; break;
                case hull_batching::Registry::State::maturing: reason = Reason::maturing; break;
                case hull_batching::Registry::State::absent:
                case hull_batching::Registry::State::no_key:
                case hull_batching::Registry::State::grouped: reason = Reason::unobserved; break;
                }
                break;
            }
        }
        const auto index = static_cast<std::size_t>(reason);
        ++census.pieces[index];
        census.surfaces[index] += instance.category_surfaces;
        ++census.by_role[index][role];
        if (!instance.visible) continue;
        ++census.visible;
        if (!resource) continue;
        const bool translated = instance.translated_parameters || batch_translated_asset(instance.asset_id);
        const bool non_opaque = resource->pass != RenderPass::opaque;
        const bool billboard = !resource->billboard_modes.empty();
        const bool opacity = opacities_.contains(entity);
        census.flag_translated += translated;
        census.flag_non_opaque += non_opaque;
        census.flag_billboard += billboard;
        census.flag_opacity += opacity;
        census.flag_proxy += instance.reserved_proxy;
        census.flag_skinned += resource->skinned;
        census.flag_palette_surfaces += resource->census_palette_surfaces > 0;
        census.surfaces_rigid += resource->census_rigid_surfaces;
        census.surfaces_palette += resource->census_palette_surfaces;
        census.surfaces_plain += resource->census_plain_surfaces;
        census.rigid_bone_sets += resource->census_rigid_bones;
        assets.insert(instance.asset_id);
        // Bones that left bind pose (the rest_pose() test, but counted).
        std::uint64_t moved = 0, set_hash = 1469598103934665603ULL ^ instance.asset_id;
        const auto pose = skin_poses_.find(entity);
        if (resource->skinned && pose != skin_poses_.end() && pose->second.asset_id == instance.asset_id) {
            const auto& palette = pose->second.active_palette();
            if (palette.size() == resource->bone_count) {
                const Transform3D identity;
                for (std::size_t bone = 0; bone < palette.size(); ++bone) {
                    if (bone >= resource->skin_used_bones.size() || !resource->skin_used_bones[bone]) continue;
                    if (transform_from(palette[bone]).is_equal_approx(identity)) continue;
                    ++moved;
                    set_hash = (set_hash ^ (bone + 1)) * 1099511628211ULL;
                }
            }
        }
        if (moved == 0) continue;
        ++census.flag_posed;
        ++census.moved_bones[render_categories::census_bucket(moved)];
        posed_assets.insert(instance.asset_id);
        bone_sets.insert(set_hash);
        if (resource->census_palette_surfaces == 0) ++census.posed_rigid_only;
        else ++census.posed_with_palette_surfaces;
        if (resource->census_palette_surfaces == 0 && !non_opaque && !billboard && !translated && !opacity) {
            ++census.posed_groupable;
            census.posed_groupable_surfaces += instance.category_surfaces;
        }
    }
    census.distinct_assets = assets.size();
    census.distinct_posed_assets = posed_assets.size();
    census.distinct_posed_bone_sets = bone_sets.size();
    std::unordered_set<std::uint64_t> group_assets;
    batch_registry_.each_group([&](std::uint32_t, const Key& key, std::size_t size) {
        const std::size_t bucket = size <= 2 ? 0 : size <= 4 ? 1 : size <= 8 ? 2 : size <= 16 ? 3 : 4;
        ++census.group_sizes[bucket];
        census.group_size_members[bucket] += size;
        group_assets.insert(key.asset);
        census.groups_with_colour += key.has_colour;
    });
    census.groups_distinct_assets = group_assets.size();
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
