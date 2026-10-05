#include "space_populate.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/presentation/space/space.hpp"
#include "frame_timer.hpp"
#include "render_profile_viewport.hpp"

#include "family_textures.hpp"
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

void SpacePopulation::pose_units(GodotRenderer& renderer, const std::uint32_t sample) {
    idle_frame_ms_ = idle_sample_ms_ = idle_upload_ms_ = 0.0;
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
        const IdlePlacement& idle = idle_placements_[index];
        auto pose = animation::sample_idle(*clips_[idle.clip], idle.playback, idle.start_frame, tick,
                                           particles::map_owner_ticks_per_second);
        if (pose) poses[index] = std::move(pose.value());
        else ++idle_sample_failures_;
    }
    sample_timer.finish();
    FrameTimer upload_timer(trace_frames_ ? &idle_upload_ms_ : nullptr);
    for (const AnimatedInstance& instance : animated_instances_) {
        if (!poses[instance.idle]) continue;
        const auto posed = renderer.set_skin_pose(instance.entity, instance.asset, poses[instance.idle]->bones);
        if (!posed) upload_failures_.push_back("entity " + std::to_string(instance.entity) + " idle pose: "
                                               + core::format_diagnostic(posed.error()));
    }
}

bool SpacePopulation::live_ship_drawn(const std::size_t ship) const noexcept {
    return ship < live_decisions_.size() && live_decisions_[ship].has_value();
}

const animation::Player* SpacePopulation::live_clip(const std::size_t ship, const bool alternate) const noexcept {
    const auto& bound = alternate ? live_alternate_clips_ : live_clips_;
    return ship < bound.size() && bound[ship] ? clips_[*bound[ship]].get() : nullptr;
}

bool SpacePopulation::pose_live_clips(GodotRenderer& renderer, const std::span<const LiveClipPose> poses) {
    for (const LiveClipPose& request : poses) {
        const animation::Player* player = live_clip(request.ship, request.alternate);
        if (player == nullptr) continue;
        auto pose = animation::sample_death_frame(*player, {request.position, request.blend_from});
        if (!pose) {
            failure_ = "live clip of ship " + std::to_string(request.ship) + ": " + core::format_diagnostic(pose.error());
            return false;
        }
        // Bone visibility is part of a clip (a death clip hides the pieces it
        // breaks off); the palette has no visibility, so a hidden bone's
        // geometry collapses to the bone's origin.
        std::vector<animation::BonePose> bones = std::move(pose.value().bones);
        for (animation::BonePose& bone : bones) {
            if (bone.visible) continue;
            ++live_clip_hidden_bones_;
            for (animation::Matrix* matrix : {&bone.skin_asset, &bone.model_asset}) {
                for (std::size_t index = 0; index < 12; ++index) (*matrix)[index] = 0.0F;
            }
        }
        for (const std::size_t index : piece_index_.clips(request.ship)) {
            const LiveClipInstance& instance = live_clip_instances_[index];
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

std::optional<SpacePopulation::LiveShipEmitterView> SpacePopulation::live_ship_emitter_view(const std::size_t ship) const {
    if (!live_ship_drawn(ship) || live_retired_[ship]) return std::nullopt;
    const std::size_t decision = *live_decisions_[ship];
    LiveShipEmitterView view;
    view.placement = &scene_->placements[decisions_[decision].scene_ordinal];
    view.hardpoints = decisions_[decision].hardpoints;
    if (decision < hardpoint_states_.size()) view.states = hardpoint_states_[decision];
    if (live_shown_[ship] && ship < live_transforms_.size()) view.model_to_world = live_transforms_[ship];
    view.death_clone = options_.placed_ships[ship].death_clone;
    view.projectile = options_.placed_ships[ship].projectile_slot;
    view.entity = options_.placed_ships[ship].live_entity;
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

bool SpacePopulation::pose_live(const std::span<const LivePose> poses, const particles::StepExecutor* workers) {
    std::fill(live_shown_.begin(), live_shown_.end(), false);
    std::fill(live_defend_.begin(), live_defend_.end(), false);
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
        live_shown_[pose.ship] = true;
        live_defend_[pose.ship] = pose.defend_active;
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
                                         const std::optional<std::array<float, 3>> neutral_colour) {
    if (!live_ship_drawn(ship)) return true;
    auto& saved = fog_models_[entity];
    std::vector<const GatedInstance*> shown;
    for (const auto index : piece_index_.pieces(ship)) {
        const auto& piece = pieces_[index];
        if (piece.shield) continue;
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
    for (const auto& old : saved) {
        if (reused.contains(old.instance.entity_id)) continue;
        renderer.forget_instance_pose(old.instance.entity_id);
        static_cast<void>(renderer.release(old.instance.asset_id));
    }
    saved = std::move(next);
    return true;
}

void SpacePopulation::draw_fog_models(GodotRenderer& renderer, const space::FogGhosts& memory,
                                      std::vector<sim::RenderInstance>& output) {
    for (auto entry = fog_models_.begin(); entry != fog_models_.end();) {
        const auto state = memory.states().find(entry->first);
        if (state == memory.states().end()) {
            for (const auto& piece : entry->second) {
                renderer.forget_instance_pose(piece.instance.entity_id);
                static_cast<void>(renderer.release(piece.instance.asset_id));
            }
            entry = fog_models_.erase(entry);
        } else {
            if (state->second.ghost) {
                for (const auto& piece : entry->second) output.push_back(piece.instance);
            }
            ++entry;
        }
    }
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

void SpacePopulation::retire_live_ship(GodotRenderer& renderer, const std::size_t ship) {
    if (ship >= live_retired_.size() || live_retired_[ship]) return;
    live_retired_[ship] = true;
    std::set<sim::AssetId> dropped;
    std::set<sim::EntityId> entities;
    for (const std::size_t index : piece_index_.pieces(ship)) {
        const GatedInstance& piece = pieces_[index];
        renderer.clear_skin_pose(piece.instance.entity_id);
        dropped.insert(piece.instance.asset_id);
        entities.insert(piece.instance.entity_id);
    }
    std::erase_if(pieces_, [&](const GatedInstance& piece) { return piece.live == ship; });
    std::erase_if(live_clip_instances_, [&](const LiveClipInstance& instance) { return instance.ship == ship; });
    std::erase_if(animated_instances_, [&](const AnimatedInstance& instance) { return entities.contains(instance.entity); });
    if (live_clips_[ship]) clips_[*live_clips_[ship]].reset();
    live_clips_[ship].reset();
    if (live_alternate_clips_[ship]) clips_[*live_alternate_clips_[ship]].reset();
    live_alternate_clips_[ship].reset();
    live_shown_[ship] = false;
    // Uploads are shared by model, surface and colour: another clone of the same type keeps them.
    for (const GatedInstance& piece : pieces_) dropped.erase(piece.instance.asset_id);
    for (const sim::AssetId asset : dropped) {
        static_cast<void>(renderer.release(asset));
        std::erase(uploaded_, asset);
        passes_.erase(asset);
        ++live_released_assets_;
    }
    rebuild_piece_index();
    refresh_instances();
}


} // namespace eawr::presentation::godot_backend
