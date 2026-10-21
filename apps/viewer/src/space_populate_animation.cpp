#include "space_populate.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/presentation/space/space.hpp"
#include "frame_timer.hpp"
#include "perf_trace.hpp"
#include "render_profile_viewport.hpp"

#include "family_textures.hpp"
#include "effect_clock_assets.hpp"
#include "space_populate_internal.hpp"
#include "shield_shell.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/animation/unit_clips.hpp"
#include "eawr/presentation/ui/pads.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <variant>

namespace eawr::presentation::godot_backend {
namespace {

// A live unit's model transform from the simulation's position, facing yaw
// and bank roll (#351), and a breakoff prop's pitch (#391). It is the
// placement conversion every placed object goes through
// (scene::placement_transform), which carries FoC's fixed +90 degree model
// turn (#288), so live units and placed objects turn together and the
// simulation's facing needs no compensation.
[[nodiscard]] core::Result<sim::math::Mat3x4> live_unit_transform(const sim::math::Vec3& position,
    const sim::math::Fixed yaw_degrees, const sim::math::Fixed pitch_degrees, const sim::math::Fixed roll_degrees,
    const sim::math::Fixed scale) {
    return scene::placement_transform(position.x, position.y, position.z, yaw_degrees, pitch_degrees, roll_degrees,
                                      scale);
}


} // namespace

void SpacePopulation::pose_pending_units(GodotRenderer& renderer) {
    if (!pending_unit_sample_) return;
    const auto sample = *pending_unit_sample_;
    pending_unit_sample_.reset();
    pose_units(renderer, sample);
}

void SpacePopulation::schedule_arrival_sample() noexcept {
    // Preserve an already pending frame (including attract's external clock).
    if (!pending_unit_sample_ && last_unit_sample_) pending_unit_sample_ = last_unit_sample_;
    // Appended idle/turret palettes and additive materials have not seen this sample yet.
    animation_sample_.reset();
    effect_sample_.reset();
}

void SpacePopulation::pose_units(GodotRenderer& renderer, const std::uint32_t sample) {
    last_unit_sample_ = sample;
    idle_frame_ms_ = idle_sample_ms_ = idle_upload_ms_ = 0.0;
    idle_skin_work_ = {};
    FrameTimer frame_timer(trace_frames_ ? &idle_frame_ms_ : nullptr);
    const std::uint32_t held_sample = options_.live_clock ? sample : std::min(sample, options_.animation_frames - 1U);
    if (!effect_clock_assets_.empty() && effect_sample_ != held_sample) {
        effect_sample_ = held_sample;
        const float time = space::environment_effect_time(static_cast<std::uint64_t>(held_sample) + options_.idle_offset);
        for (const sim::AssetId asset : effect_clock_assets_) {
            if (auto set = renderer.set_material_scalar(asset, "eawr_effect_time", time); !set) {
                upload_failures_.push_back("additive effect clock: " + core::format_diagnostic(set.error()));
            }
        }
    }
    if (idle_placements_.empty() || animation_sample_ == held_sample) return;
    animation_sample_ = held_sample;
    const std::uint64_t tick = static_cast<std::uint64_t>(held_sample) + options_.idle_offset;
    std::vector<std::optional<animation::Pose>> poses(idle_placements_.size());
    FrameTimer sample_timer(trace_frames_ ? &idle_sample_ms_ : nullptr);
    for (std::size_t index = 0; index < idle_placements_.size(); ++index) {
        const auto live = idle_live_ships_.find(index);
        if (live != idle_live_ships_.end() && live_retired_[live->second]) continue;
        const IdlePlacement& idle = idle_placements_[index];
        auto pose = animation::sample_idle(*clips_[idle.clip], idle.playback, idle.start_frame, tick,
                                           particles::map_owner_ticks_per_second);
        if (pose) poses[index] = std::move(pose.value());
        else ++idle_sample_failures_;
    }
    sample_timer.finish();
    for (const auto& [idle, ship] : idle_live_ships_) {
        if (poses[idle]) contact_bones_[ship] = poses[idle]->bones;
    }
    FrameTimer upload_timer(trace_frames_ ? &idle_upload_ms_ : nullptr);
    renderer.begin_skin_batch(trace_frames_ && PerfTrace::fine_particle_timings());
    for (const AnimatedInstance& instance : animated_instances_) {
        if (!poses[instance.idle]) continue;
        const auto posed = renderer.set_skin_pose(instance.entity, instance.asset, poses[instance.idle]->bones);
        if (!posed) upload_failures_.push_back("entity " + std::to_string(instance.entity) + " idle pose: "
                                               + core::format_diagnostic(posed.error()));
    }
    idle_skin_work_ = renderer.end_skin_batch();
}

bool SpacePopulation::live_ship_drawn(const std::size_t ship) const noexcept {
    return ship < live_decisions_.size() && live_decisions_[ship].has_value();
}

bool SpacePopulation::live_ship_emitter_visit(const std::size_t ship) const noexcept {
    if (ship >= live_shown_.size() || ship >= live_death_clone_.size()) return true;
    if (!live_shown_[ship] && !live_death_clone_[ship]) return false;
    return live_ship_drawn(ship) && !live_retired_[ship];
}

bool SpacePopulation::pose_live_turrets(GodotRenderer& renderer, const std::span<const LivePose> poses,
    const std::span<const LiveClipPose> clips) {
    const auto rotation = [](const sim::math::Fixed degrees, const bool pitch) {
        auto matrix = animation::Player::identity_matrix();
        const auto radians = static_cast<float>(degrees.raw()) / static_cast<float>(sim::math::Fixed::scale)
            * 0.017453292519943295F;
        const auto c = std::cos(radians), s = std::sin(radians);
        if (pitch) { matrix[0] = c; matrix[2] = -s; matrix[8] = s; matrix[10] = c; }
        else { matrix[0] = c; matrix[1] = s; matrix[4] = -s; matrix[5] = c; }
        return matrix;
    };
    const auto apply_angles = [&](TurretBinding& binding, const LivePose::TurretPose& angles) {
        const bool barrel = angles.barrel && binding.barrel.has_value();
        if (angles.barrel && !binding.barrel) {
            failure_ = "turret profile requires a missing barrel bone";
            return false;
        }
        auto turret = rotation(angles.yaw, false);
        if (!barrel && !angles.xy_only)
            turret = animation::Player::multiply(turret, rotation(angles.pitch, true));
        if (!binding.player->apply_bone_transform(binding.pose, binding.turret, turret)) return false;
        if (barrel && !angles.xy_only
            && !binding.player->apply_bone_transform(binding.pose, *binding.barrel, rotation(angles.pitch, true))) return false;
        return true;
    };
    for (const auto& request : poses) {
        if (!request.turret || request.ship >= turret_bindings_.size() || !turret_bindings_[request.ship]) continue;
        auto& binding = *turret_bindings_[request.ship];
        if (request.ship >= contact_bones_.size() || contact_bones_[request.ship].size() != binding.pose.bones.size()) {
            failure_ = "ordinary turret pose storage does not match the bound skeleton";
            return false;
        }
        // UA-05: the validated sim profile owns XY-only and usable-barrel admission.
        const bool active_clip = std::any_of(clips.begin(), clips.end(),
            [&](const LiveClipPose& clip) { return clip.ship == request.ship; });
        if (active_clip) {
            // The live clip already sampled this same model skeleton; preserve its local transforms.
            std::copy(contact_bones_[request.ship].begin(), contact_bones_[request.ship].end(), binding.pose.bones.begin());
        } else if (binding.idle) {
            const auto& idle = idle_placements_[*binding.idle];
            auto sampled = animation::sample_idle(*binding.player, idle.playback, idle.start_frame,
                static_cast<std::uint64_t>(animation_sample_.value_or(0U)) + options_.idle_offset,
                particles::map_owner_ticks_per_second, binding.pose);
            if (!sampled) { failure_ = core::format_diagnostic(sampled.error()); return false; }
        } else {
            std::copy(binding.bind_bones.begin(), binding.bind_bones.end(), binding.pose.bones.begin());
        }
        if (!apply_angles(binding, *request.turret)) return false;
        // UA-05: storage and bone bindings are prepared once at composition, then reused each frame.
        std::copy(binding.pose.bones.begin(), binding.pose.bones.end(), contact_bones_[request.ship].begin());
        for (auto& bone : binding.pose.bones) {
            if (bone.visible) continue;
            for (animation::Matrix* matrix : {&bone.skin_asset, &bone.model_asset})
                for (std::size_t index = 0; index < 12; ++index) (*matrix)[index] = 0.0F;
        }
        for (const auto index : piece_index_.pieces(request.ship)) {
            const auto& piece = pieces_[index];
            if (piece.gate || piece.shield || piece.cloak) continue;
            if (auto posed = renderer.set_skin_pose(piece.instance.entity_id, piece.instance.asset_id, binding.pose.bones); !posed) {
                failure_ = core::format_diagnostic(posed.error()); return false;
            }
        }
    }
    for (const auto& request : poses) {
        if (request.ship >= hardpoint_turret_bindings_.size()) continue;
        auto& bindings = hardpoint_turret_bindings_[request.ship];
        for (const auto& turret : request.hardpoint_turrets) {
            const auto found = bindings.find(turret.hardpoint);
            if (found == bindings.end()) continue;
            auto& binding = found->second;
            std::copy(binding.bind_bones.begin(), binding.bind_bones.end(), binding.pose.bones.begin());
            if (!apply_angles(binding, turret.angles)) return false;
            for (auto& bone : binding.pose.bones) {
                if (bone.visible) continue;
                for (animation::Matrix* matrix : {&bone.skin_asset, &bone.model_asset})
                    for (std::size_t index = 0; index < 12; ++index) (*matrix)[index] = 0.0F;
            }
            for (const auto index : piece_index_.pieces(request.ship)) {
                const auto& piece = pieces_[index];
                if (!piece.gate || piece.gate->hardpoint != turret.hardpoint
                    || piece.gate->art != HardpointGate::Art::attached_model) continue;
                if (auto posed = renderer.set_skin_pose(piece.instance.entity_id, piece.instance.asset_id, binding.pose.bones); !posed) {
                    failure_ = core::format_diagnostic(posed.error());
                    return false;
                }
                binding.applied = turret.angles;
            }
        }
    }
    return true;
}

const animation::Player* SpacePopulation::live_clip(const std::size_t ship, const bool alternate) const noexcept {
    const auto& bound = alternate ? live_alternate_clips_ : live_clips_;
    return ship < bound.size() && bound[ship] ? clips_[*bound[ship]].get() : nullptr;
}

bool SpacePopulation::live_clip_terminal_hidden(const std::size_t ship) const noexcept {
    const auto held = held_clips_.find(ship);
    return held != held_clips_.end() && held->second.hidden;
}

std::span<const animation::BonePose> SpacePopulation::terminal_clip_bones(const std::size_t ship) const noexcept {
    const auto held = held_clips_.find(ship);
    return held == held_clips_.end() ? std::span<const animation::BonePose>{} : held->second.bones;
}

std::span<const animation::BonePose> SpacePopulation::held_clip_bones(const std::size_t ship,
    const animation::ClipPosition position, const float blend_from) const noexcept {
    const auto held = held_clips_.find(ship);
    if (blend_from != 0.0F || held == held_clips_.end() || held->second.position != position) return {};
    return held->second.bones;
}

void SpacePopulation::prepare_live_clip(const std::size_t index) {
    const auto& request = pending_clip_poses_[index];
    auto& row = prepared_clips_[index];
    if (!row.player || row.cached || request.ship >= live_retired_.size() || live_retired_[request.ship]) return;
    auto pose = animation::sample_death_frame(*row.player, {request.position, request.blend_from});
    if (!pose) { row.failure = core::format_diagnostic(pose.error()); return; }
    row.pose = std::move(pose.value());
    // A visible unused root does not keep a wreck alive. Every piece must
    // instead prove all of its actually weighted geometry bones hidden.
    row.hidden = row.terminal && !row.geometry.empty();
    for (const auto& used : row.geometry) {
        if (used.size() != row.pose->bones.size()
            || std::none_of(used.begin(), used.end(), [](const auto value) { return value != 0; })) {
            row.hidden = false;
            break;
        }
        for (std::size_t bone = 0; bone < used.size(); ++bone)
            if (used[bone] && row.pose->bones[bone].visible) row.hidden = false;
    }
}

bool SpacePopulation::pose_live_clips(GodotRenderer& renderer, const std::span<const LiveClipPose> poses,
    const particles::StepExecutor* workers) {
    if (poses.empty()) return true;
    prepared_clips_.resize(poses.size());
    auto& prepared = prepared_clips_;
    pending_clip_poses_ = poses;
    // Snapshot immutable upload metadata before dispatch. Workers never call Godot.
    for (std::size_t index = 0; index < poses.size(); ++index) {
        const auto& request = poses[index];
        auto& row = prepared[index];
        row = PreparedLiveClip{};
        row.player = live_clip(request.ship, request.alternate);
        if (!row.player || request.ship >= live_retired_.size() || live_retired_[request.ship]) continue;
        row.terminal = live_death_clone_[request.ship] && !request.alternate && request.blend_from == 0.0F
            && request.position.subdivisions != 0 && request.position.position
                >= static_cast<std::uint64_t>(row.player->playable_frames()) * request.position.subdivisions;
        const auto held = held_clips_.find(request.ship);
        row.cached = row.terminal && held != held_clips_.end() && held->second.position == request.position;
        if (!row.terminal || row.cached) continue;
        for (const auto piece : piece_index_.pieces(request.ship)) {
            const auto& geometry = pieces_[piece];
            const auto clips = piece_index_.clips(request.ship);
            const bool posed = std::any_of(clips.begin(), clips.end(), [&](const auto clip) {
                return live_clip_instances_[clip].entity == geometry.instance.entity_id;
            });
            row.geometry.push_back(posed ? renderer.skin_geometry_bones(geometry.instance.asset_id)
                                          : std::vector<std::uint8_t>{});
        }
    }
    constexpr std::size_t parallel_threshold = 256;
    if (workers && poses.size() >= parallel_threshold) {
        if (!workers->run(poses.size(), live_clip_task_)) {
            pending_clip_poses_ = {};
            failure_ = "live clip workers failed";
            return false;
        }
    } else {
        for (std::size_t index = 0; index < poses.size(); ++index) prepare_live_clip(index);
    }
    pending_clip_poses_ = {};
    // Stable request order controls diagnostics, palette commits and retire requests.
    for (std::size_t index = 0; index < poses.size(); ++index) {
        const auto& request = poses[index];
        auto& row = prepared[index];
        if (!row.failure.empty()) {
            failure_ = "live clip of ship " + std::to_string(request.ship) + ": " + row.failure;
            return false;
        }
        if (row.cached || !row.pose) continue;
        auto bones = std::move(row.pose->bones);
        contact_bones_[request.ship] = bones;
        if (row.terminal) held_clips_.insert_or_assign(request.ship,
            HeldClip{request.position, bones, row.hidden});
        else held_clips_.erase(request.ship);
        for (auto& bone : bones) {
            if (bone.visible) continue;
            ++live_clip_hidden_bones_;
            for (animation::Matrix* matrix : {&bone.skin_asset, &bone.model_asset})
                for (std::size_t entry = 0; entry < 12; ++entry) (*matrix)[entry] = 0.0F;
        }
        for (const auto clip : piece_index_.clips(request.ship)) {
            const auto& instance = live_clip_instances_[clip];
            const auto posed = renderer.set_skin_pose(instance.entity, instance.asset, bones);
            if (!posed) {
                failure_ = "entity " + std::to_string(instance.entity) + " live clip pose: "
                    + core::format_diagnostic(posed.error());
                return false;
            }
        }
        ++live_clip_poses_;
    }
    return true;
}

std::optional<SpacePopulation::LiveShipBox> SpacePopulation::live_ship_box(const std::size_t ship) const {
    if (ship >= live_bounds_.size() || ship >= live_transforms_.size() || !live_bounds_[ship] || !live_transforms_[ship]
        || !live_ship_drawn(ship)) {
        return std::nullopt;
    }
    return LiveShipBox{*live_transforms_[ship], live_bounds_[ship]->first, live_bounds_[ship]->second};
}

std::optional<SpacePopulation::LiveShipEmitterView> SpacePopulation::live_ship_emitter_assets(const std::size_t ship) const {
    if (!scene_ || ship >= live_decisions_.size() || !live_decisions_[ship]
        || ship >= options_.placed_ships.size()) return std::nullopt;
    const std::size_t decision = *live_decisions_[ship];
    if (decision >= decisions_.size() || decisions_[decision].scene_ordinal >= scene_->placements.size()) return std::nullopt;
    LiveShipEmitterView view;
    view.placement = &scene_->placements[decisions_[decision].scene_ordinal];
    view.hardpoints = decisions_[decision].hardpoints;
    view.death_clone = options_.placed_ships[ship].death_clone;
    view.projectile = options_.placed_ships[ship].projectile_slot;
    view.entity = options_.placed_ships[ship].live_entity;
    view.identity = view.placement;
    if (ship < live_identity_.size() && live_identity_[ship] != ship) {
        if (const scene::Placement* identity = live_placement(live_identity_[ship])) view.identity = identity;
    }
    return view;
}

const scene::Placement* SpacePopulation::live_placement(const std::size_t ship) const noexcept {
    if (!scene_ || ship >= live_decisions_.size() || !live_decisions_[ship]) return nullptr;
    const std::size_t decision = *live_decisions_[ship];
    if (decision >= decisions_.size() || decisions_[decision].scene_ordinal >= scene_->placements.size()) return nullptr;
    return &scene_->placements[decisions_[decision].scene_ordinal];
}

void SpacePopulation::bind_live_identity(const std::size_t ship, const std::size_t identity_ship) {
    if (ship >= live_decisions_.size()) return;
    if (ship >= live_identity_.size()) {
        const std::size_t known = live_identity_.size();
        live_identity_.resize(live_decisions_.size());
        for (std::size_t index = known; index < live_identity_.size(); ++index) live_identity_[index] = index;
    }
    // Unchanged: a spin-away craft rebinds the same slot every frame. UnitEmitters replans only when the entity
    // changes (as for a physical rebind in BASE), so an identity change under the same entity keeps its seeds.
    if (live_identity_[ship] == identity_ship) return;
    live_identity_[ship] = identity_ship;
    // An animated hull starts its idle clip at a frame hashed from its slot's placement: the identity slot's.
    const scene::Placement* identity = live_placement(identity_ship);
    if (identity == nullptr) return;
    for (const auto& [idle_index, idle_ship] : idle_live_ships_) {
        if (idle_ship != ship) continue;
        IdlePlacement& idle = idle_placements_[idle_index];
        const auto start = placement_start_frame(*identity, clips_[idle.clip]->playable_frames());
        if (idle.start_frame == start) continue;
        idle.start_frame = start;
        schedule_arrival_sample();
    }
}

std::optional<SpacePopulation::LiveShipEmitterView> SpacePopulation::live_ship_emitter_view(const std::size_t ship) const {
    if (!live_ship_drawn(ship) || live_retired_[ship]) return std::nullopt;
    const std::size_t decision = *live_decisions_[ship];
    auto assets = live_ship_emitter_assets(ship);
    if (!assets) return std::nullopt;
    LiveShipEmitterView& view = *assets;
    if (decision < hardpoint_states_.size()) view.states = hardpoint_states_[decision];
    if (live_shown_[ship] && ship < live_transforms_.size()) view.model_to_world = live_transforms_[ship];
    return view;
}

std::optional<sim::math::Mat3x4> SpacePopulation::live_transform(const LivePose& pose) const {
    if (!live_ship_drawn(pose.ship) || live_retired_[pose.ship]) return std::nullopt;
    const scene::Placement& placement = scene_->placements[decisions_[*live_decisions_[pose.ship]].scene_ordinal];
    auto transform = live_unit_transform(pose.position, pose.yaw_degrees, pose.pitch_degrees, pose.roll_degrees,
                                         sim::math::Fixed::from_raw(placement.scale_raw));
    if (!transform) return std::nullopt;
    return transform.value();
}

std::optional<SpacePopulation::LiveBoneFrame> SpacePopulation::live_bone_frame(
    const sim::EntityId entity, const std::uint32_t bone) const {
    const auto found = contact_live_ships_.find(entity);
    if (found == contact_live_ships_.end()) return std::nullopt;
    const std::size_t ship = found->second;
    if (!live_ship_drawn(ship) || live_retired_[ship] || !live_transforms_[ship]
        || options_.placed_ships[ship].live_entity != entity
        || bone >= contact_bones_[ship].size()) return std::nullopt;
    const auto& posed = contact_bones_[ship][bone];
    const auto local = particles::fixed_model_frame(posed.model_asset, scene::fixed_from_binary32);
    if (!local) return std::nullopt;
    const auto world = sim::math::compose(*live_transforms_[ship], *local);
    if (!world) return std::nullopt;
    return LiveBoneFrame{world.value(), posed.visible};
}

bool SpacePopulation::pose_live(const std::span<const LivePose> poses, const particles::StepExecutor* workers) {
    std::fill(live_shown_.begin(), live_shown_.end(), false);
    std::fill(live_defend_.begin(), live_defend_.end(), false);
    std::fill(live_stealth_.begin(), live_stealth_.end(), false);
    std::fill(live_blast_charging_.begin(), live_blast_charging_.end(), false);
    std::fill(live_blast_charged_.begin(), live_blast_charged_.end(), false);
    std::fill(live_transforms_.begin(), live_transforms_.end(), std::nullopt);
    for (const LivePose& pose : poses) {
        if (!live_ship_drawn(pose.ship) || live_retired_[pose.ship]) continue;
        // BP-43/IS-09: emitter state and fog opacity belong to the occupying unit,
        // rather than the placeholder identity used when the model slot was composed.
        if (pose.entity != sim::invalid_entity_id) options_.placed_ships[pose.ship].live_entity = pose.entity;
        const std::size_t decision = *live_decisions_[pose.ship];
        const auto transform = live_transform(pose);
        if (!transform) {
            failure_ = "live unit " + std::to_string(options_.placed_ships[pose.ship].live_entity)
                + ": its transform leaves the Q24 range";
            return false;
        }
        live_transforms_[pose.ship] = *transform;
        contact_live_ships_.insert_or_assign(pose.entity, pose.ship);
        live_shown_[pose.ship] = true;
        live_defend_[pose.ship] = pose.defend_active;
        live_stealth_[pose.ship] = pose.stealth_active;
        live_blast_charging_[pose.ship] = pose.blast_charging;
        live_blast_charged_[pose.ship] = pose.blast_charged;
        if (pose.construction_hull) {
            live_alternates_[pose.ship] = presentation::ui::construction_alternate(*pose.construction_hull, live_alternate_counts_[pose.ship]);
        }
        auto& states = hardpoint_states_[decision];
        for (std::size_t index = 0; index < pose.hardpoints.size() && index < states.size(); ++index) {
            states[index] = pose.hardpoints[index];
        }
    }
    FrameTimer compose_timer(trace_frames_ ? &compose_ms_ : nullptr);
    if (!space::compose_population_pieces(std::span<GatedInstance>(pieces_), live_transforms_,
            compose_errors_, workers, source_to_render)) {
        failure_ = "live piece pose workers failed";
        return false;
    }
    // Ordered error selection keeps diagnostics independent of worker order.
    for (std::size_t index = 0; index < compose_errors_.size(); ++index) {
        if (compose_errors_[index] == 0) continue;
        failure_ = "live unit " + std::to_string(options_.placed_ships[*pieces_[index].live].live_entity)
            + ": a piece transform leaves the Q24 range";
        return false;
    }
    compose_timer.finish();
    refresh_instances();
    return true;
}

std::span<const sim::EntityId> SpacePopulation::live_hull_entities(const std::size_t ship) const {
    return piece_index_.entities(ship, true);
}

std::span<const sim::EntityId> SpacePopulation::live_ship_entities(const std::size_t ship) const {
    return piece_index_.entities(ship);
}

void SpacePopulation::set_live_opacity(GodotRenderer& renderer, const std::size_t ship, const float opacity) {
    if (!piece_index_.opacity_changed(ship, opacity)) return;
    for (const sim::EntityId entity : piece_index_.entities(ship)) renderer.set_unit_opacity(entity, opacity);
}

void SpacePopulation::prepare_fog_model_capture() {
    fog_drawn_entities_.clear();
    fog_drawn_entities_.reserve(instances_.size());
    for (const auto& instance : instances_) fog_drawn_entities_.push_back(instance.entity_id);
    std::sort(fog_drawn_entities_.begin(), fog_drawn_entities_.end());
}

bool SpacePopulation::remember_fog_model(GodotRenderer& renderer, const sim::EntityId entity,
                                         const std::size_t ship, const std::span<const animation::BonePose> neutral_pose,
                                         const std::optional<std::array<float, 3>> neutral_colour, const bool initial) {
    if (!live_ship_drawn(ship)) return true;
    const auto& placement = scene_->placements[decisions_[*live_decisions_[ship]].scene_ordinal];
    if (initial && !placement.transform) return true;
    auto& saved = fog_models_[entity];
    std::vector<const GatedInstance*> shown;
    for (const auto index : piece_index_.pieces(ship)) {
        const auto& piece = pieces_[index];
        if (piece.shield) continue;
        if (initial) {
            // Battle-start knowledge cannot expose a later hidden damage, cloak,
            // construction or ability state. Ordinary loss-of-sight copies below
            // still retain exactly the previously submitted art.
            if (piece.cloak || piece.blast_charged || (piece.alternate && *piece.alternate != 0)
                || (piece.gate && piece.gate->art != HardpointGate::Art::attached_model)) continue;
            shown.push_back(&piece);
            continue;
        }
        // Copy the submitted alternate and damage art, never an unshown piece.
        if (std::binary_search(fog_drawn_entities_.begin(), fog_drawn_entities_.end(),
                piece.instance.entity_id)) shown.push_back(&piece);
    }
    std::vector<FogPiece> next;
    next.reserve(shown.size());
    std::set<sim::EntityId> reused;
    const auto discard_new = [&] {
        for (const auto& piece : next) {
            if (reused.contains(piece.instance.entity_id)) continue;
            renderer.forget_instance_pose(piece.instance.entity_id);
            static_cast<void>(renderer.release(piece.instance.asset_id));
        }
    };
    for (const auto* piece : shown) {
        auto old = std::find_if(saved.begin(), saved.end(), [&](const FogPiece& copy) {
            return copy.source == piece->instance.entity_id && copy.instance.asset_id == piece->instance.asset_id;
        });
        sim::RenderInstance copy = piece->instance;
        if (initial) {
            const auto transform = sim::math::compose(placement.transform->matrix, piece->local);
            if (!transform) {
                failure_ = "initial fog model: piece transform leaves the Q24 range";
                discard_new();
                return false;
            }
            copy.fixed_transform = source_to_render(transform.value());
        }
        if (old != saved.end()) {
            copy.entity_id = old->instance.entity_id;
            reused.insert(copy.entity_id);
        } else {
            if (auto retained = renderer.retain(copy.asset_id); !retained) {
                failure_ = core::format_diagnostic(retained.error());
                discard_new();
                return false;
            }
            copy.entity_id = next_fog_piece_++;
        }
        renderer.copy_instance_pose(piece->instance.entity_id, copy.entity_id, 0.5F); // FW-26
        // The copy's native light scale replaces transient live lighting such as nebula tint.
        renderer.set_light_scale(copy.entity_id, {0.5F, 0.5F, 0.5F});
        next.push_back({piece->instance.entity_id, copy});
        if (neutral_colour) renderer.set_unit_colorization(copy.entity_id, *neutral_colour);
        if (!neutral_pose.empty()
            && std::any_of(animated_instances_.begin(), animated_instances_.end(), [&](const auto& animated) {
                return animated.entity == piece->instance.entity_id;
            })) {
            if (auto posed = renderer.set_skin_pose(copy.entity_id, copy.asset_id, neutral_pose); !posed) {
                failure_ = core::format_diagnostic(posed.error());
                discard_new();
                return false;
            }
        }
    }
    bool released_old = false;
    for (const auto& old : saved) {
        if (reused.contains(old.instance.entity_id)) continue;
        renderer.forget_instance_pose(old.instance.entity_id);
        static_cast<void>(renderer.release(old.instance.asset_id));
        released_old = true;
    }
    if (released_old) prune_effect_clock_assets(effect_clock_assets_, renderer.resources());
    saved = std::move(next);
    return true;
}

void SpacePopulation::draw_fog_models(GodotRenderer& renderer, const space::FogGhosts& memory,
                                      std::vector<sim::RenderInstance>& output) {
    bool released = false;
    for (auto entry = fog_models_.begin(); entry != fog_models_.end();) {
        const auto state = memory.states().find(entry->first);
        if (state == memory.states().end()) {
            for (const auto& piece : entry->second) {
                renderer.forget_instance_pose(piece.instance.entity_id);
                static_cast<void>(renderer.release(piece.instance.asset_id));
                released = true;
            }
            entry = fog_models_.erase(entry);
        } else {
            if (state->second.ghost) {
                for (const auto& piece : entry->second) output.push_back(piece.instance);
            }
            ++entry;
        }
    }
    if (released) prune_effect_clock_assets(effect_clock_assets_, renderer.resources());
}

std::size_t SpacePopulation::fog_model_pieces(const sim::EntityId entity) const noexcept {
    const auto entry = fog_models_.find(entity);
    return entry == fog_models_.end() ? 0 : entry->second.size();
}

void SpacePopulation::set_shield_time(GodotRenderer& renderer, const float seconds) {
    for (const sim::AssetId asset : shield_assets_) {
        if (std::find(uploaded_.begin(), uploaded_.end(), asset) == uploaded_.end()) continue;
        if (auto set = renderer.set_material_scalar(asset, meshshield_time_binding, seconds); !set) {
            upload_failures_.push_back("shield shell " + std::to_string(asset) + " clock: "
                                       + core::format_diagnostic(set.error()));
        }
    }
}

void SpacePopulation::retire_live_ship(GodotRenderer& renderer, const std::size_t ship, const bool reusable) {
    if (ship >= live_retired_.size() || live_retired_[ship]) return;
    live_retired_[ship] = true;
    std::set<sim::AssetId> dropped;
    std::set<sim::EntityId> entities;
    for (const std::size_t index : piece_index_.pieces(ship)) {
        const GatedInstance& piece = pieces_[index];
        renderer.forget_instance_pose(piece.instance.entity_id);
        dropped.insert(piece.instance.asset_id);
        entities.insert(piece.instance.entity_id);
    }
    if (reusable) {
        auto& parked = parked_ships_[ship];
        for (const auto index : piece_index_.pieces(ship)) parked.pieces.push_back(pieces_[index]);
        for (const auto index : piece_index_.clips(ship)) parked.clips.push_back(live_clip_instances_[index]);
        for (const auto& animated : animated_instances_)
            if (entities.contains(animated.entity)) parked.animated.push_back(animated);
    }
    std::erase_if(pieces_, [&](const GatedInstance& piece) { return piece.live == ship; });
    std::erase_if(live_clip_instances_, [&](const LiveClipInstance& instance) { return instance.ship == ship; });
    std::erase_if(animated_instances_, [&](const AnimatedInstance& instance) { return entities.contains(instance.entity); });
    if (!reusable) {
        if (live_clips_[ship]) clips_[*live_clips_[ship]].reset();
        live_clips_[ship].reset();
        if (live_alternate_clips_[ship]) clips_[*live_alternate_clips_[ship]].reset();
        live_alternate_clips_[ship].reset();
    }
    held_clips_.erase(ship);
    std::vector<animation::BonePose>{}.swap(contact_bones_[ship]);
    live_transforms_[ship].reset();
    std::erase_if(contact_live_ships_, [&](const auto& entry) { return entry.second == ship; });
    live_shown_[ship] = false;
    piece_index_.reset_opacity(ship);
    // Uploads are shared by model and surface: another clone of the same type keeps them.
    for (const GatedInstance& piece : pieces_) dropped.erase(piece.instance.asset_id);
    for (const auto& [slot, parked] : parked_ships_) {
        static_cast<void>(slot);
        for (const auto& piece : parked.pieces) dropped.erase(piece.instance.asset_id);
    }
    for (const sim::AssetId asset : dropped) {
        static_cast<void>(renderer.release(asset));
        std::erase(uploaded_, asset);
        passes_.erase(asset);
        ++live_released_assets_;
    }
    if (!dropped.empty()) prune_effect_clock_assets(effect_clock_assets_, renderer.resources());
    rebuild_piece_index();
    refresh_instances();
}

void SpacePopulation::restore_live_ship(GodotRenderer& renderer, const std::size_t ship) {
    const auto parked = parked_ships_.find(ship);
    if (parked == parked_ships_.end()) return;
    for (const auto& piece : parked->second.pieces) {
        if (!piece.colour) continue;
        const auto& rgb = *piece.colour;
        renderer.set_unit_colorization(piece.instance.entity_id, {static_cast<float>(rgb[0]) / 255.0F,
            static_cast<float>(rgb[1]) / 255.0F, static_cast<float>(rgb[2]) / 255.0F});
    }
    pieces_.insert(pieces_.end(), parked->second.pieces.begin(), parked->second.pieces.end());
    live_clip_instances_.insert(live_clip_instances_.end(), parked->second.clips.begin(), parked->second.clips.end());
    animated_instances_.insert(animated_instances_.end(), parked->second.animated.begin(), parked->second.animated.end());
    const bool idle = !parked->second.animated.empty();
    parked_ships_.erase(parked);
    std::sort(animated_instances_.begin(), animated_instances_.end(), [](const auto& left, const auto& right) {
        return left.entity < right.entity;
    });
    // Reuse preserves the original compose identities and submission order.
    std::sort(pieces_.begin(), pieces_.end(), [](const auto& left, const auto& right) {
        return left.instance.entity_id < right.instance.entity_id;
    });
    std::sort(live_clip_instances_.begin(), live_clip_instances_.end(), [](const auto& left, const auto& right) {
        return left.entity < right.entity;
    });
    live_retired_[ship] = false;
    live_shown_[ship] = false;
    held_clips_.erase(ship);
    if (idle) schedule_arrival_sample();
    rebuild_piece_index();
}


} // namespace eawr::presentation::godot_backend
