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
    constexpr char hex[] = "0123456789abcdef";
    std::string result{"\""};
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(character));
        } else if (character < 0x20U || character >= 0x7fU) {
            result += "\\u00";
            result.push_back(hex[character >> 4U]);
            result.push_back(hex[character & 15U]);
        } else {
            result.push_back(static_cast<char>(character));
        }
    }
    return result + "\"";
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
    if (!live_decisions_.empty() && std::any_of(options_.placed_ships.begin(), options_.placed_ships.end(),
            [](const Options::PlacedShip& ship) { return ship.live_entity != 0; })) {
        // #81, #391: death clones and breakoff props are live ships too, counted apart from
        // the session's units.
        std::size_t drawn = 0;
        std::size_t clones_drawn = 0;
        std::size_t breakoffs_drawn = 0;
        std::size_t launch_slots_drawn = 0;
        std::size_t projectile_slots_drawn = 0;
        for (std::size_t ship = 0; ship < live_decisions_.size(); ++ship) {
            if (!live_decisions_[ship]) continue;
            const Options::PlacedShip& placed = options_.placed_ships[ship];
            ++(placed.death_clone ? clones_drawn
               : placed.breakoff  ? breakoffs_drawn
               : placed.launch_slot ? launch_slots_drawn
               : placed.projectile_slot ? projectile_slots_drawn
                                    : drawn);
        }
        output << ", \"live_units\": {\"drawn\": " << drawn << ", \"not_drawn\": " << strings(live_undrawn_)
               << ", \"session_records_skipped\": " << session_records_skipped_
               << ", \"clones_drawn\": " << clones_drawn << ", \"breakoffs_drawn\": " << breakoffs_drawn
               << ", \"launch_slots_drawn\": " << launch_slots_drawn
               << ", \"launch_slots_not_drawn\": " << strings(launch_slots_undrawn_)
               << ", \"projectile_slots_drawn\": " << projectile_slots_drawn
               << ", \"projectile_slots_not_drawn\": " << strings(projectile_slots_undrawn_)
               << ", \"ships_retired\": " << std::count(live_retired_.begin(), live_retired_.end(), true)
               << ", \"released_assets\": " << live_released_assets_;
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
    output
           << ",\n    \"classification\": \"scene::classify_space_placements: catalog markers (Is_Marker or the Marker "
              "element) are never drawn; nebula and background objects (Is_Nebula, In_Background) are left to the "
              "space environment; every other placement with a supported visible surface is drawn. Each HardPoints "
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
           << ", \"skinned_instances\": " << skinned_instances_
            << ",\n    \"team_colour\": {\"colour_variants\": " << team_colour_variants_
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
