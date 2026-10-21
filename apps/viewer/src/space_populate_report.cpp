#include "eawr/core/json_text.hpp"
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

[[nodiscard]] std::string json(const std::string_view value) {
    return eawr::core::json_quote_ascii(value);
}

[[nodiscard]] std::string strings(const std::vector<std::string>& values) {
    std::string text{"["};
    for (std::size_t index = 0; index < values.size(); ++index) text += (index == 0 ? "" : ", ") + json(values[index]);
    return text + "]";
}


} // namespace

void SpacePopulation::write_report(std::ostream& output) const {
    std::map<scene::SpaceRole, std::size_t> roles;
    for (const auto& decision : decisions_) ++roles[decision.role];
    // Objects not drawn, or drawn with something left out, grouped by object
    // and reason so the list stays readable.
    std::map<std::pair<std::string, std::string>, std::vector<std::uint64_t>> groups;
    for (const auto& decision : decisions_) {
        if (decision.role == scene::SpaceRole::drawn && decision.missing.empty()) continue;
        std::string reason(scene::to_string(decision.role));
        for (const std::string& item : decision.missing) reason += "; " + item;
        groups[{decision.object_id.empty() ? std::string("(uncatalogued)") : decision.object_id, reason}]
            .push_back(decision.scene_ordinal);
    }
    output << "  \"populate\": {\"requested\": true, \"declared_placements\": "
           << map_placements_ << ", \"composed\": " << roles[scene::SpaceRole::drawn]
           << ", \"scene_sha256\": " << json(scene_ ? scene_->scene_sha256 : "");
    if (!options_.placed_ships.empty()) output << ", \"placed_ships\": " << options_.placed_ships.size();
    if (!options_.placed_ships.empty()) output << ", \"backdrop_ships\": " << std::count(backdrop_ships_.begin(), backdrop_ships_.end(), true);
    if (!live_decisions_.empty() && std::any_of(options_.placed_ships.begin(), options_.placed_ships.end(),
            [](const Options::PlacedShip& ship) { return ship.live_entity != 0; })) {
        // Auxiliary models are live ships too, counted apart from
        // the session's units.
        std::size_t drawn = 0;
        std::size_t clones_drawn = 0;
        std::size_t breakoffs_drawn = 0;
        std::size_t launch_slots_drawn = 0;
        std::size_t projectile_slots_drawn = 0;
        std::size_t beacon_models_drawn = 0;
        for (std::size_t ship = 0; ship < live_decisions_.size(); ++ship) {
            if (!live_decisions_[ship]) continue;
            const Options::PlacedShip& placed = options_.placed_ships[ship];
            ++(placed.death_clone ? clones_drawn
               : placed.breakoff  ? breakoffs_drawn
               : placed.launch_slot ? launch_slots_drawn
               : placed.projectile_slot ? projectile_slots_drawn
               : placed.beacon_model ? beacon_models_drawn
                                    : drawn);
        }
        output << ", \"live_units\": {\"drawn\": " << drawn << ", \"not_drawn\": " << strings(live_undrawn_)
               << ", \"session_records_skipped\": " << session_records_skipped_
               << ", \"clones_drawn\": " << clones_drawn << ", \"breakoffs_drawn\": " << breakoffs_drawn
               << ", \"launch_slots_drawn\": " << launch_slots_drawn
               << ", \"launch_slots_not_drawn\": " << strings(launch_slots_undrawn_)
               << ", \"projectile_slots_drawn\": " << projectile_slots_drawn
               << ", \"projectile_slots_not_drawn\": " << strings(projectile_slots_undrawn_)
               << ", \"beacon_models_drawn\": " << beacon_models_drawn
               << ", \"ships_retired\": " << std::count(live_retired_.begin(), live_retired_.end(), true)
               << ", \"released_assets\": " << live_released_assets_;
        output << ", \"suppressed_records\": [";
        for (std::size_t index = 0; index < skipped_session_records_.size(); ++index)
            output << (index == 0 ? "" : ", ") << skipped_session_records_[index];
        output << "], \"surface_assets_by_unit\": [";
        bool first_unit = true;
        for (std::size_t ship = 0; ship < live_decisions_.size(); ++ship) {
            if (!live_decisions_[ship]) continue;
            const auto& placed = options_.placed_ships[ship];
            if (!placed.team_colour || placed.death_clone || placed.breakoff || placed.launch_slot
                || placed.projectile_slot || placed.beacon_model) continue;
            output << (first_unit ? "" : ", ") << "{\"entity\": " << placed.live_entity
                   << ", \"type\": " << json(placed.object_id) << ", \"rgb\": ["
                   << static_cast<unsigned>((*placed.team_colour)[0]) << ", "
                   << static_cast<unsigned>((*placed.team_colour)[1]) << ", "
                   << static_cast<unsigned>((*placed.team_colour)[2]) << "], \"assets\": [";
            bool first_hull_asset = true;
            for (const auto asset : hull_surface_assets_[ship]) {
                output << (first_hull_asset ? "" : ", ") << asset;
                first_hull_asset = false;
            }
            output << "]}";
            first_unit = false;
        }
        output << ']';
        if (!live_clip_status_.empty()) {
            output << ", \"clips\": {";
            bool first = true;
            for (const auto& [key, status] : live_clip_status_) {
                output << (first ? "" : ", ") << json(key) << ": " << json(status);
                first = false;
            }
            output << "}, \"clip_poses\": " << live_clip_poses_ << ", \"clip_hidden_bones\": " << live_clip_hidden_bones_;
        }
        // #427: each DEFEND type's shield shell and the most shells one frame drew.
        output << ", \"shield_shells\": {\"types\": {";
        bool first_shell = true;
        for (const auto& [object, status] : shield_shells_) {
            output << (first_shell ? "" : ", ") << json(object) << ": " << json(status);
            first_shell = false;
        }
        output << "}, \"max_shown\": " << shield_shells_shown_ << "}";
        // WST-12: each STEALTH type's cloak sub-object and the most cloaks one frame drew.
        output << ", \"stealth_shells\": {\"types\": {";
        bool first_cloak = true;
        for (const auto& [object, status] : stealth_shells_) {
            output << (first_cloak ? "" : ", ") << json(object) << ": " << json(status);
            first_cloak = false;
        }
        output << "}, \"max_shown\": " << stealth_shells_shown_ << "}";
        output << "}";
    }
    if (options_.debug_ship) {
        output << ", \"debug_ship\": {\"status\": " << json(debug_ship_status_)
               << ", \"object\": " << json(options_.debug_ship->object_id)
               << ", \"spawn_record\": " << options_.debug_ship->spawn_record
               << ", \"yaw_degrees\": " << debug_ship_yaw_ << ", \"pose\": \"idle_bind\", \"source\": "
               << json("FoC " + debug_ship_type_ + " XML")
               << ", \"hardpoint_states\": {";
        // The last value given for an id is the one applied.
        const auto& states = options_.debug_ship->hardpoint_states;
        bool first_state = true;
        for (std::size_t index = 0; index < states.size(); ++index) {
            if (std::any_of(states.begin() + static_cast<std::ptrdiff_t>(index) + 1, states.end(),
                            [&](const auto& later) { return later.first == states[index].first; })) continue;
            output << (first_state ? "" : ", ") << json(states[index].first) << ": "
                   << json(scene::to_string(states[index].second));
            first_state = false;
        }
        output << "}}";
    }
    output << ", \"hardpoint_turrets\": [";
    bool first_turret = true;
    for (std::size_t ship = 0; ship < hardpoint_turret_bindings_.size(); ++ship) {
        if (!live_transforms_[ship] || !live_shown_[ship] || live_retired_[ship]) continue;
        for (const auto& [hardpoint, binding] : hardpoint_turret_bindings_[ship]) {
            if (!binding.applied || !binding.muzzle) continue;
            const auto frame = sim::math::compose(*live_transforms_[ship], binding.attachment_frame);
            if (!frame) continue;
            const auto& muzzle = binding.pose.bones[*binding.muzzle].model_asset;
            output << (first_turret ? "" : ", ") << "{\"entity\": " << options_.placed_ships[ship].live_entity
                   << ", \"hardpoint\": " << hardpoint
                   << ", \"yaw\": " << static_cast<double>(binding.applied->yaw.raw()) / sim::math::Fixed::scale
                   << ", \"pitch\": " << static_cast<double>(binding.applied->pitch.raw()) / sim::math::Fixed::scale;
            // Actual uploaded skeleton, placed through the same attachment/ship frame.
            for (const bool direction : {false, true}) {
                output << (direction ? ", \"forward\": [" : ", \"muzzle\": [");
                for (std::size_t row = 0; row < 3; ++row) {
                    double value = direction ? 0.0 : static_cast<double>(frame.value().rows[row][3].raw())
                        / sim::math::Fixed::scale;
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        value += static_cast<double>(frame.value().rows[row][axis].raw()) / sim::math::Fixed::scale
                            * muzzle[(direction ? 0U : 12U) + axis];
                    output << (row == 0 ? "" : ", ") << value;
                }
                output << "]";
            }
            output << "}";
            first_turret = false;
        }
    }
    output << "]";
    output
           << ",\n    \"classification\": \"scene::classify_space_placements: catalog markers (Is_Marker or the Marker "
              "element) are never drawn; nebula and background objects (Is_Nebula, In_Background) are left to the "
              "space environment; scene::classify_live_unit ignores those environment tags for composed units. "
              "Every other placement with a supported visible surface is drawn. Each HardPoints "
              "entry's state selects its art (scene::hardpoint_art): an intact hardpoint draws its Model_To_Attach "
              "at the bind frame of its Attachment_Bone and hides its Damage_Decal mesh and the damage emitters at "
              "and below its Damage_Particles bone. MeshShadowVolume surfaces are stencil-shadow geometry and are "
              "not drawn in colour\""
           << ",\n    \"roles\": {\"drawn\": " << roles[scene::SpaceRole::drawn]
           << ", \"environment\": " << roles[scene::SpaceRole::environment]
           << ", \"marker\": " << roles[scene::SpaceRole::marker]
           << ", \"not_drawable\": " << roles[scene::SpaceRole::not_drawable] << "}"
           << ", \"surfaces_uploaded\": " << surfaces_uploaded_ << ", \"instances\": " << instance_count_
           << ", \"hardpoints_attached\": " << attachments_drawn_
           << ", \"blast_mesh_submissions\": {\"buildup\": "
           << (blast_meshes_shown_.contains("buildup") ? blast_meshes_shown_.at("buildup") : 0)
           << ", \"charged\": "
           << (blast_meshes_shown_.contains("charged") ? blast_meshes_shown_.at("charged") : 0) << "}"
           << ", \"skinned_instances\": " << skinned_instances_
            << ",\n    \"team_colour\": {\"colour_variants\": " << team_colour_variants_
            << ", \"meaning\": \"per-instance colour assignments\""
            << ", \"source\": \"TED owner to catalog faction <Color>\", \"drawn_by_status\": {";
    bool first_status = true;
    for (const auto& [status, count] : team_colour_status_) {
        output << (first_status ? "" : ", ") << json(status) << ": " << count;
        first_status = false;
    }
    output << "}}, \"unit_animation\": {\"clips\": " << clips_.size()
            << ", \"placements_animated\": " << animated_placements_
            << ", \"instances_animated\": " << animated_instances_.size()
            << ", \"sample_at_capture\": " << (animation_sample_ ? std::to_string(*animation_sample_) : "null")
            << ", \"clock\": " << json(options_.live_clock ? "live" : "held")
            << ", \"idle_offset\": " << options_.idle_offset
            << ", \"idle_sample_failures\": " << idle_sample_failures_
            << ", \"idle_rule\": " << json(idle_rule)
            << ", \"by_status\": {";
    first_status = true;
    for (const auto& [status, count] : animation_status_) {
        output << (first_status ? "" : ", ") << json(status) << ": " << count;
        first_status = false;
    }
    output << "}, \"bound_idle_hulls\": [";
    bool first_hull = true;
    for (const auto& [object, clip] : bound_idle_hulls_) {
        output << (first_hull ? "" : ", ") << "{\"object\": " << json(object)
               << ", \"clip\": " << json(clip);
        if (const auto playback = idle_playbacks_.find(object); playback != idle_playbacks_.end()) {
            const animation::IdlePlayback& idle = playback->second;
            output << ", \"loop\": " << (idle.loop ? "true" : "false")
                   << ", \"restarts\": " << (idle.restarts ? "true" : "false")
                   << ", \"random_start\": " << (idle.random_start ? "true" : "false")
                   << ", \"rate_mod\": [" << idle.rate.numerator << ", " << idle.rate.denominator << ']';
        }
        if (const auto rejected = idle_rate_rejected_.find(object); rejected != idle_rate_rejected_.end())
            output << ", \"rate_mod_rejected\": " << json(rejected->second);
        output << '}';
        first_hull = false;
    }
    output << "], \"unbound_skinned_hulls\": [";
    first_hull = true;
    for (const auto& [object, reason] : static_hulls_) {
        output << (first_hull ? "" : ", ") << "{\"object\": " << json(object)
               << ", \"reason\": " << json(reason) << '}';
        first_hull = false;
    }
    output << "]}, \"attached_effects\": {\"composed\": " << (attached_effects_.composed ? "true" : "false")
           << ", \"records\": " << attached_effects_.records
           << ", \"hidden_by_hardpoint_state\": " << attached_effects_.hardpoint_hidden
           << ", \"admitted\": " << attached_effects_.admitted
           << ", \"capacity_exhausted\": " << attached_effects_.capacity_exhausted
           << ", \"report\": " << (attached_effects_.composed ? "\"map_particles\"" : "null")
           << ", \"cause\": " << json(attached_effects_.cause) << '}'
           << ",\n    \"upload_failures\": " << strings(upload_failures_) << ",\n    \"not_drawn_or_partial\": [";
    bool first = true;
    for (const auto& [key, ordinals] : groups) {
        std::string list;
        for (std::size_t index = 0; index < ordinals.size(); ++index) {
            list += (index == 0 ? "" : ",") + std::to_string(ordinals[index]);
        }
        output << (first ? "\n      " : ",\n      ") << "{\"object\": " << json(key.first)
               << ", \"placements\": " << ordinals.size() << ", \"ordinals\": " << json(list)
               << ", \"reason\": " << json(key.second) << "}";
        first = false;
    }
    output << (groups.empty() ? "" : "\n    ") << "],\n    \"placements\": [";
    for (std::size_t index = 0; index < decisions_.size(); ++index) {
        const auto& decision = decisions_[index];
        const scene::Placement& placement = scene_->placements[decision.scene_ordinal];
        output << (index == 0 ? "\n      " : ",\n      ") << "{\"ordinal\": " << decision.scene_ordinal
               << ", \"object\": " << json(decision.object_id) << ", \"type\": " << json(decision.type_name)
               << ", \"role\": " << json(scene::to_string(decision.role)) << ", \"model\": " << json(placement.model_path)
               << ", \"drawn_surfaces\": " << decision.drawn_surfaces.size()
               << ", \"stencil_shadow_surfaces\": " << decision.shadow_volume_surfaces
               << ", \"hidden_damage_decal_surfaces\": " << hidden_decals(index)
               << ", \"hardpoints\": " << decision.hardpoints.size()
               << ", \"missing\": " << strings(decision.missing) << "}";
    }
    output << (decisions_.empty() ? "" : "\n    ") << "]},\n";

    output << "  \"lighting\": {\"policy\": " << json(lighting::to_string(options_.policy))
           << ", \"composed\": " << (options_.policy != lighting::Policy::off ? "true" : "false")
           << ", \"shadows\": " << (options_.shadows ? "true" : "false")
           << ", \"environment\": " << json(options_.environment_source)
           << ", \"shadow_max_distance\": " << shadow_max_distance_;
    if (options_.policy != lighting::Policy::off) {
        output << ", \"shadow_mode\": " << json(to_string(lighting_.shadow_layout))
               << ", \"shadow_atlas_size\": " << lighting_.shadow_atlas_size
               << ", \"shadow_filter\": " << json(to_string(lighting_.shadow_filter))
               << ", \"shadow_stabilization\": \"godot_sphere_texel_snap\""
               << ", \"shadow_split_offsets\": [" << lighting_.shadow_split_offsets[0] << ", "
               << lighting_.shadow_split_offsets[1] << ", " << lighting_.shadow_split_offsets[2] << ']'
               << ", \"shadow_bias\": " << lighting_.shadow_bias.value_or(0.0F)
               << ", \"shadow_normal_bias\": " << lighting_.shadow_normal_bias.value_or(0.0F)
               << ", \"shadow_blur\": " << lighting_.shadow_blur.value_or(0.0F)
               << ", \"shadow_floor\": [" << lighting_.shadow_floor[0] << ", " << lighting_.shadow_floor[1] << ", "
               << lighting_.shadow_floor[2] << ']';
    }
    if (options_.debug_ship) output << ", \"shadow_scope\": \"scene directional light (debug evidence only)\"";
    output << ", \"shadow_receiving_materials\": " << shadow_receiving_
           << ", \"shadow_variant_failures\": " << shadow_variant_failures_
           << ", \"cause\": " << json(options_.policy == lighting::Policy::off
               ? "no lighting policy requested: hulls keep the P0 legacy material constants"
               : "the land lighting policy applied to the populated space scene: one directional sun from the "
                 "environment and its SH irradiance on every hull; the sky adapter stays unshaded and casts no shadow")
           << "},\n";
}
} // namespace eawr::presentation::godot_backend
