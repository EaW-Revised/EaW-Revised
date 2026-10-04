#include "eawr/scene/space_population.hpp"

#include <algorithm>

namespace eawr::scene {
namespace {

[[nodiscard]] char fold(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) noexcept {
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(),
                      [](const char a, const char b) { return fold(a) == fold(b); });
}

[[nodiscard]] std::string_view trimmed(std::string_view value) noexcept {
    const auto space = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    while (!value.empty() && space(value.back())) value.remove_suffix(1);
    return value;
}

[[nodiscard]] bool flag(const data::EffectiveObject& object, const std::string_view tag) {
    const data::EffectiveValue* value = object.value(tag);
    if (value == nullptr) return false;
    const std::string_view text = trimmed(value->value.raw_text);
    return ieq(text, "yes") || ieq(text, "true") || text == "1";
}

[[nodiscard]] std::string text_of(const data::EffectiveObject& object, const std::string_view tag) {
    const data::EffectiveValue* value = object.value(tag);
    return value == nullptr ? std::string() : std::string(trimmed(value->value.raw_text));
}

[[nodiscard]] bool shadow_volume_issue(const Issue& issue) noexcept {
    return issue.cause == Cause::shader_unsupported && is_shadow_volume_shader(issue.detail);
}

[[nodiscard]] std::string describe(const Issue& issue) {
    std::string text(to_string(issue.cause));
    if (!issue.detail.empty()) text += " " + issue.detail;
    return text;
}

} // namespace

SpaceObjectTags space_object_tags(const data::EffectiveObject& object, const ObjectResolver& resolve) {
    SpaceObjectTags tags;
    tags.type_name = object.type_name;
    tags.marker = flag(object, "Is_Marker") || ieq(object.type_name, "Marker");
    tags.nebula = flag(object, "Is_Nebula");
    tags.background = flag(object, "In_Background");
    tags.idle = idle_tags(object, assets::MapKind::space);
    if (const data::EffectiveValue* list = object.value("HardPoints")) {
        const std::string_view text = list->value.raw_text;
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t end = std::min(text.find_first_of(", \t\r\n", start), text.size());
            if (end > start) {
                HardpointAttachment attachment;
                attachment.hardpoint = std::string(text.substr(start, end - start));
                if (resolve) {
                    if (const auto hardpoint = resolve(attachment.hardpoint, data::Category::hardpoint)) {
                        attachment.resolved = true;
                        attachment.model = text_of(*hardpoint, "Model_To_Attach");
                        attachment.bone = text_of(*hardpoint, "Attachment_Bone");
                        attachment.damage_decal = text_of(*hardpoint, "Damage_Decal");
                        attachment.damage_particles = text_of(*hardpoint, "Damage_Particles");
                        attachment.engine_particles = text_of(*hardpoint, "Engine_Particles");
                        attachment.hide_engine_particles_on_death =
                            flag(*hardpoint, "Engine_Death_Hide_Engine_Particles");
                    }
                }
                tags.hardpoints.push_back(std::move(attachment));
            }
            start = end + 1;
        }
    }
    return tags;
}

std::string_view to_string(const HardpointState state) noexcept {
    switch (state) {
    case HardpointState::intact: return "intact";
    case HardpointState::damaged: return "damaged";
    case HardpointState::destroyed: return "destroyed";
    }
    return "unknown";
}

std::optional<HardpointState> parse_hardpoint_state(const std::string_view text) noexcept {
    for (const HardpointState state : {HardpointState::intact, HardpointState::damaged, HardpointState::destroyed}) {
        if (ieq(trimmed(text), to_string(state))) return state;
    }
    return std::nullopt;
}

HardpointArt hardpoint_art(const HardpointState state) noexcept {
    switch (state) {
    case HardpointState::intact: return {.attached_model = true, .damage_decal = false, .damage_particles = false};
    case HardpointState::damaged: return {.attached_model = true, .damage_decal = false, .damage_particles = false};
    case HardpointState::destroyed: return {.attached_model = false, .damage_decal = true, .damage_particles = true};
    }
    return {};
}

HardpointOwnerArt hardpoint_owner_art(const assets::Model& owner, const std::span<const HardpointAttachment> hardpoints) {
    HardpointOwnerArt art;
    art.mesh_hardpoint.assign(owner.meshes.size(), HardpointOwnerArt::none);
    art.proxy_hardpoint.assign(owner.proxies.size(), HardpointOwnerArt::none);
    art.engine_mesh_hardpoint.assign(owner.meshes.size(), HardpointOwnerArt::none);
    art.engine_proxy_hardpoint.assign(owner.proxies.size(), HardpointOwnerArt::none);
    // The hardpoint that owns each bone through its Damage_Particles bone.
    // Several bones may share a name; each one named is a root. As in FoC's
    // sub-object ancestry test, the walk stops at the model root (bone 0):
    // the root never matches, so nothing hangs off a root-named hardpoint.
    std::vector<std::size_t> bone_hardpoint(owner.bones.size(), HardpointOwnerArt::none);
    std::vector<std::size_t> engine_bone_hardpoint(owner.bones.size(), HardpointOwnerArt::none);
    for (std::size_t bone = 0; bone < owner.bones.size(); ++bone) {
        for (std::int32_t at = static_cast<std::int32_t>(bone), steps = 0;
             at > 0 && static_cast<std::size_t>(at) < owner.bones.size()
             && steps <= static_cast<std::int32_t>(owner.bones.size());
             at = owner.bones[static_cast<std::size_t>(at)].parent, ++steps) {
            const std::string_view name = owner.bones[static_cast<std::size_t>(at)].name;
            const auto found = std::find_if(hardpoints.begin(), hardpoints.end(), [&](const HardpointAttachment& hardpoint) {
                return !hardpoint.damage_particles.empty() && ieq(hardpoint.damage_particles, name);
            });
            if (found != hardpoints.end() && bone_hardpoint[bone] == HardpointOwnerArt::none) {
                bone_hardpoint[bone] = static_cast<std::size_t>(found - hardpoints.begin());
            }
            const auto engine = std::find_if(hardpoints.begin(), hardpoints.end(), [&](const HardpointAttachment& hardpoint) {
                return hardpoint.hide_engine_particles_on_death && !hardpoint.engine_particles.empty()
                    && ieq(hardpoint.engine_particles, name);
            });
            if (engine != hardpoints.end() && engine_bone_hardpoint[bone] == HardpointOwnerArt::none)
                engine_bone_hardpoint[bone] = static_cast<std::size_t>(engine - hardpoints.begin());
            if (bone_hardpoint[bone] != HardpointOwnerArt::none
                && engine_bone_hardpoint[bone] != HardpointOwnerArt::none) break;
        }
    }
    for (std::size_t mesh = 0; mesh < owner.meshes.size(); ++mesh) {
        const auto found = std::find_if(hardpoints.begin(), hardpoints.end(), [&](const HardpointAttachment& hardpoint) {
            return !hardpoint.damage_decal.empty() && ieq(hardpoint.damage_decal, owner.meshes[mesh].name);
        });
        if (found != hardpoints.end()) art.mesh_hardpoint[mesh] = static_cast<std::size_t>(found - hardpoints.begin());
        const std::int32_t bone = owner.meshes[mesh].bone;
        if (bone >= 0 && static_cast<std::size_t>(bone) < engine_bone_hardpoint.size())
            art.engine_mesh_hardpoint[mesh] = engine_bone_hardpoint[static_cast<std::size_t>(bone)];
    }
    for (std::size_t proxy = 0; proxy < owner.proxies.size(); ++proxy) {
        const std::uint32_t bone = owner.proxies[proxy].bone;
        if (bone < bone_hardpoint.size()) art.proxy_hardpoint[proxy] = bone_hardpoint[bone];
        if (bone < engine_bone_hardpoint.size()) art.engine_proxy_hardpoint[proxy] = engine_bone_hardpoint[bone];
    }
    return art;
}

std::vector<std::uint8_t> hidden_hardpoint_proxies(const assets::Model& owner,
                                                   const std::span<const HardpointAttachment> hardpoints,
                                                   const std::span<const HardpointState> states) {
    const HardpointOwnerArt art = hardpoint_owner_art(owner, hardpoints);
    std::vector<std::uint8_t> hidden(owner.proxies.size());
    for (std::size_t proxy = 0; proxy < hidden.size(); ++proxy) {
        const std::size_t hardpoint = art.proxy_hardpoint[proxy];
        if (hardpoint != HardpointOwnerArt::none) {
            const HardpointState state = hardpoint < states.size() ? states[hardpoint] : HardpointState::intact;
            hidden[proxy] = hardpoint_art(state).damage_particles ? 0U : 1U;
        }
        const std::size_t engine = art.engine_proxy_hardpoint[proxy];
        if (engine != HardpointOwnerArt::none && engine < states.size()
            && states[engine] == HardpointState::destroyed) hidden[proxy] = 1U;
    }
    return hidden;
}

std::string_view to_string(const SpaceRole role) noexcept {
    switch (role) {
    case SpaceRole::drawn: return "drawn";
    case SpaceRole::environment: return "environment";
    case SpaceRole::marker: return "marker";
    case SpaceRole::not_drawable: return "not_drawable";
    }
    return "unknown";
}

bool is_shadow_volume_shader(const std::string_view shader) noexcept {
    return ieq(shader, "MeshShadowVolume.fx");
}

bool drawable_projectile_effects(const Placement& placement) noexcept {
    if (placement.asset_id == 0 || !placement.transform || !placement.surfaces.empty()
        || placement.effects.empty()) return false;
    return std::all_of(placement.effects.begin(), placement.effects.end(),
               [](const AttachedEffect& effect) { return !effect.resolved.empty(); })
        && std::all_of(placement.issues.begin(), placement.issues.end(),
               [](const Issue& issue) { return issue.cause == Cause::model_has_no_surface; });
}

std::vector<SpacePlacementDecision> classify_space_placements(
    const Scene& scene,
    const std::function<std::optional<SpaceObjectTags>(std::string_view object_id)>& tags_of) {
    std::vector<SpacePlacementDecision> decisions;
    decisions.reserve(scene.placements.size());
    for (const Placement& placement : scene.placements) {
        SpacePlacementDecision decision;
        decision.scene_ordinal = placement.scene_ordinal;
        decision.object_id = placement.object_id;
        std::optional<SpaceObjectTags> tags;
        if (!placement.object_id.empty() && tags_of) tags = tags_of(placement.object_id);
        if (tags) decision.type_name = tags->type_name;
        const bool effect_projectile = tags && tags->type_name == "Projectile"
            && drawable_projectile_effects(placement);
        for (const Surface& surface : placement.surfaces) {
            if (is_shadow_volume_shader(surface.shader)) ++decision.shadow_volume_surfaces;
        }
        if (tags && tags->marker) {
            decision.role = SpaceRole::marker;
        } else if (tags && (tags->nebula || tags->background)) {
            decision.role = SpaceRole::environment;
        } else if (placement.drawable() || effect_projectile) {
            decision.role = SpaceRole::drawn;
            if (tags) decision.hardpoints = tags->hardpoints;
            const auto decal = [&](const std::string_view mesh) {
                return static_cast<std::size_t>(std::find_if(decision.hardpoints.begin(), decision.hardpoints.end(),
                    [&](const HardpointAttachment& hardpoint) {
                        return !hardpoint.damage_decal.empty() && ieq(hardpoint.damage_decal, mesh);
                    }) - decision.hardpoints.begin());
            };
            for (std::size_t index = 0; index < placement.surfaces.size(); ++index) {
                const Surface& surface = placement.surfaces[index];
                if (!surface.supported || find_legacy_selector(surface.shader) == nullptr) continue;
                if (const std::size_t hardpoint = decal(surface.mesh_name); hardpoint < decision.hardpoints.size()) {
                    ++decision.damage_decal_surfaces;
                    decision.damage_decals.push_back({index, hardpoint});
                    continue;
                }
                decision.drawn_surfaces.push_back(index);
            }
            for (const Issue& issue : placement.issues) {
                if (!shadow_volume_issue(issue)
                    && !(effect_projectile && issue.cause == Cause::model_has_no_surface)) {
                    decision.missing.push_back(describe(issue));
                }
            }
            if (decision.drawn_surfaces.empty() && !effect_projectile) {
                // Only damage decals were supported: nothing of the intact
                // object would draw.
                decision.role = SpaceRole::not_drawable;
                decision.hardpoints.clear();
                decision.damage_decals.clear();
                decision.missing.emplace_back("no_supported_surface besides damage decals");
            }
        } else {
            decision.role = SpaceRole::not_drawable;
            const bool blocked = std::any_of(placement.issues.begin(), placement.issues.end(),
                                             [](const Issue& issue) { return blocks_drawing(issue.cause); });
            for (const Issue& issue : placement.issues) {
                // Blocking causes explain it alone; otherwise it is the
                // surfaces that have no supported material.
                if (blocked ? blocks_drawing(issue.cause) : !shadow_volume_issue(issue)) {
                    decision.missing.push_back(describe(issue));
                }
            }
            if (decision.missing.empty()) decision.missing.emplace_back("no_supported_surface");
        }
        decisions.push_back(std::move(decision));
    }
    return decisions;
}

} // namespace eawr::scene
