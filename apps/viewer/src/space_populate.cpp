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

} // namespace

SpacePopulation::SpacePopulation(Options options) : options_(std::move(options)) {}

std::optional<lighting::Environment> SpacePopulation::environment(
    const assets::Map& map, const std::string_view choice, std::string& failure) {
    if (choice != "map") return lighting::alo_viewer_default_environment();
    if (map.environments.empty()) {
        failure = "--eawr-environment map: the map declares no environment record";
        return std::nullopt;
    }
    std::vector<lighting::RawField> fields;
    for (const auto& field : map.environments.front().fields) fields.push_back({field.id, field.bytes});
    auto candidate = lighting::candidate_environment(fields);
    if (!candidate) failure = "--eawr-environment map: environment 0 does not decode under the candidate mapping";
    return candidate;
}

std::optional<RenderPass> SpacePopulation::pass(const sim::AssetId asset) const {
    const auto found = passes_.find(asset);
    if (found == passes_.end()) return std::nullopt;
    return found->second;
}



std::size_t SpacePopulation::hidden_decals(const std::size_t decision) const {
    if (decision >= decisions_.size() || decision >= hardpoint_states_.size()) return 0;
    const auto& states = hardpoint_states_[decision];
    return static_cast<std::size_t>(std::count_if(decisions_[decision].damage_decals.begin(),
        decisions_[decision].damage_decals.end(), [&](const auto& decal) {
            return decal.hardpoint >= states.size() || !scene::hardpoint_art(states[decal.hardpoint]).damage_decal;
        }));
}

bool SpacePopulation::set_hardpoint_state(const std::uint64_t scene_ordinal, const std::string_view hardpoint,
                                          const scene::HardpointState state) {
    bool found = false;
    bool changed = false;
    for (std::size_t index = 0; index < decisions_.size() && index < hardpoint_states_.size(); ++index) {
        const scene::SpacePlacementDecision& decision = decisions_[index];
        if (decision.scene_ordinal != scene_ordinal) continue;
        for (std::size_t entry = 0; entry < decision.hardpoints.size() && entry < hardpoint_states_[index].size(); ++entry) {
            if (!ieq(decision.hardpoints[entry].hardpoint, hardpoint)) continue;
            changed = changed || hardpoint_states_[index][entry] != state;
            hardpoint_states_[index][entry] = state;
            found = true;
        }
    }
    if (!found) return false;
    refresh_instances();
    if (changed && composed_ && options_.attached_effects && !options_.attached_effects(*this)) {
        failure_ = "hardpoint " + std::string(hardpoint) + ": the attached effects could not follow its state";
        return false;
    }
    return true;
}

std::span<const scene::HardpointState> SpacePopulation::hardpoint_states(const std::uint64_t scene_ordinal) const {
    for (std::size_t index = 0; index < decisions_.size() && index < hardpoint_states_.size(); ++index) {
        if (decisions_[index].scene_ordinal == scene_ordinal) return hardpoint_states_[index];
    }
    return {};
}

bool SpacePopulation::is_marker(const std::uint64_t scene_ordinal) const {
    return std::any_of(decisions_.begin(), decisions_.end(), [&](const scene::SpacePlacementDecision& decision) {
        return decision.scene_ordinal == scene_ordinal && decision.role == scene::SpaceRole::marker;
    });
}

void SpacePopulation::rebuild_piece_index() {
    piece_index_.rebuild(live_decisions_.size(), pieces_, live_clip_instances_, [](const GatedInstance& piece) {
        return !piece.shield && (!piece.gate || piece.gate->art != HardpointGate::Art::attached_model);
    });
}

void SpacePopulation::refresh_instances() {
    FrameTimer timer(trace_frames_ ? &refresh_ms_ : nullptr);
    instances_.clear();
    std::uint64_t shells = 0;
    attachment_marks_.clear();
    for (const GatedInstance& piece : pieces_) {
        if (piece.live && *piece.live < live_shown_.size() && !live_shown_[*piece.live]) continue;
        if (piece.live && piece.alternate && *piece.alternate != live_alternates_[*piece.live]) continue;
        if (piece.shield) {
            if (!piece.live || *piece.live >= live_defend_.size() || !live_defend_[*piece.live]) continue;
            ++shells;
        }
        if (piece.gate) {
            const HardpointGate& gate = *piece.gate;
            const scene::HardpointArt art = scene::hardpoint_art(hardpoint_states_[gate.decision][gate.hardpoint]);
            const bool visible = gate.art == HardpointGate::Art::attached_model ? art.attached_model
                : gate.art == HardpointGate::Art::damage_decal ? art.damage_decal
                : hardpoint_states_[gate.decision][gate.hardpoint] != scene::HardpointState::destroyed;
            if (!visible) continue;
            if (gate.art == HardpointGate::Art::attached_model) attachment_marks_.mark(gate.decision, gate.hardpoint);
        }
        instances_.push_back(piece.instance);
    }
    instance_count_ = instances_.size() - shells;
    attachments_drawn_ = attachment_marks_.count();
    shield_shells_shown_ = std::max(shield_shells_shown_, shells);
}

void SpacePopulation::release(GodotRenderer& renderer) {
    composed_ = false;
    for (const auto& [entity, model] : fog_models_) {
        static_cast<void>(entity);
        for (const auto& piece : model) {
            renderer.forget_instance_pose(piece.instance.entity_id);
            static_cast<void>(renderer.release(piece.instance.asset_id));
        }
    }
    fog_models_.clear();
    fog_drawn_entities_.clear();
    pending_unit_sample_.reset();
    for (const GatedInstance& piece : pieces_) renderer.clear_skin_pose(piece.instance.entity_id);
    for (const sim::AssetId asset : uploaded_) static_cast<void>(renderer.release(asset));
    uploaded_.clear();
    effect_clock_assets_.clear();
    effect_sample_.reset();
    pieces_.clear();
    piece_index_.clear();
    instances_.clear();
    live_clip_instances_.clear();
}



} // namespace eawr::presentation::godot_backend
