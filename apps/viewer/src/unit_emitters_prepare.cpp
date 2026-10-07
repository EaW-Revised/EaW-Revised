#include "unit_emitters.hpp"

#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/presentation/particles/map_effect_plan.hpp"
#include "eawr/presentation/particles/prewarmed_capacity.hpp"
#include "eawr/presentation/particles/proxy_binding.hpp"
#include "eawr/presentation/space/debris.hpp"
#include "eawr/presentation/space/live_units.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

#include "unit_emitters_internal.hpp"
#include "space_populate_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace unit_emitters_detail;

namespace {

// The per-emitter particle allocation of the static space map's attached plan
// (--eawr-map-attached-capacity default), raised for independent continuous emitters.
constexpr std::size_t fallback_capacity = 256;
// The plan's seed: every ship's emitters draw from their own record seed.
constexpr std::uint64_t plan_seed = 394;

// The model's emitter types are proxy-name prefixes, compared case-insensitively over the
// prefix's length (BP-42, BP-43).
[[nodiscard]] bool has_prefix(const std::string_view name, const std::string_view prefix) {
    if (name.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(name[index])) != prefix[index]) return false;
    }
    return true;
}
// Hidden when the object is created: turbo engines, power to weapons, missile shield.
[[nodiscard]] bool hidden_at_creation(const std::string_view name) {
    return has_prefix(name, "pte") || has_prefix(name, "pptw") || has_prefix(name, "pgw") || has_prefix(name, "pem");
}
[[nodiscard]] bool engine_emitter(const std::string_view name) { return has_prefix(name, "pe"); }
[[nodiscard]] bool turbo_emitter(const std::string_view name) { return has_prefix(name, "pte"); }
[[nodiscard]] bool power_to_weapons_emitter(const std::string_view name) { return has_prefix(name, "pptw"); }
[[nodiscard]] bool ion_stun_emitter(const std::string_view name) { return has_prefix(name, "pi"); }
[[nodiscard]] bool invulnerability_emitter(const std::string_view name) { return has_prefix(name, "pem"); }

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(character == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}
} // namespace

const assets::Texture* UnitEmitters::resolve_texture(const std::string_view authored) {
    const std::size_t slash = authored.find_last_of("/\\");
    const std::string name = lower(slash == std::string_view::npos ? authored : authored.substr(slash + 1));
    auto found = textures_.find(name);
    if (found == textures_.end()) {
        std::optional<assets::Texture> decoded;
        std::string stem = name;
        for (const std::string_view suffix : {std::string_view(".tga"), std::string_view(".dds")}) {
            if (stem.ends_with(suffix)) stem.resize(stem.size() - suffix.size());
        }
        for (const std::string& candidate : {"data/art/textures/" + name, "data/art/textures/" + stem + ".tga",
                                             "data/art/textures/" + stem + ".dds"}) {
            if (!filesystem_->stat(candidate)) continue;
            if (auto loaded = assets::load_texture(*filesystem_, candidate)) {
                decoded = std::move(loaded.value());
                break;
            }
        }
        found = textures_.emplace(name, std::move(decoded)).first;
    }
    return found->second ? &*found->second : nullptr;
}

const UnitEmitters::EffectSystem& UnitEmitters::effect_system(const std::string& path) {
    auto found = systems_.find(path);
    if (found != systems_.end()) return found->second;
    EffectSystem entry;
    if (auto bytes = filesystem_->open(path); !bytes) {
        entry.cause = core::format_diagnostic(bytes.error());
    } else if (auto system = particles::load_alo(bytes.value(), path); !system) {
        entry.cause = core::format_diagnostic(system.error());
    } else if (system.value().emitters.empty()) {
        entry.cause = "particle system declares no emitters";
    } else {
        entry.capacity = particles::continuous_capacity(system.value(), fallback_capacity);
        entry.system = std::move(system).value();
    }
    return systems_.emplace(path, std::move(entry)).first->second;
}

const UnitEmitters::ModelFrames* UnitEmitters::model_frames(const scene::Placement& placement,
                                                            const assets::Model*& model) {
    model = cache_.model(placement.model_path);
    if (model == nullptr) return nullptr;
    auto found = frames_.find(placement.model_path);
    if (found != frames_.end()) return &found->second;
    // The bind pose, as the static attached plan reads it: capital ships run no idle clip that
    // moves their proxy bones.
    ModelFrames frames;
    frames.bones.resize(model->bones.size());
    if (auto player = animation::Player::create(*model)) {
        if (auto pose = player.value().sample({}); pose && pose.value().bones.size() == model->bones.size()) {
            frames.bind_pose = true;
            for (std::size_t bone = 0; bone < model->bones.size(); ++bone) {
                frames.bones[bone] = particles::fixed_model_frame(pose.value().bones[bone].model_asset,
                                                                  scene::fixed_from_binary32);
            }
        }
    }
    return &frames_.emplace(placement.model_path, std::move(frames)).first->second;
}

void UnitEmitters::plan(Ship& ship, const SpacePopulation::LiveShipEmitterView& view,
                        const std::span<const scene::HardpointState> states, const Modes modes) {
    ship.entity = view.entity;
    ship.planned = true;
    ship.states.assign(states.begin(), states.end());
    ship.modes = modes;
    ship.wanted.clear();
    ++plans_;
    plan_model(ship, view, *view.placement, sim::math::identity_matrix(), 0, view.hardpoints, states, modes);
    // BP-67: attached models participate in the same emitter visibility switches as the root.
    // Resolve and bind once per ship; state replans reuse the model-local frames and references.
    if (!ship.attachments_planned) {
        ship.attachments_planned = true;
        const assets::Model* owner = nullptr;
        const ModelFrames* owner_frames = model_frames(*view.placement, owner);
        if (owner != nullptr && owner_frames != nullptr) {
            std::size_t proxy_base = owner->proxies.size();
            for (std::size_t index = 0; index < view.hardpoints.size(); ++index) {
                const auto& hardpoint = view.hardpoints[index];
                if (!hardpoint.resolved || hardpoint.model.empty()) continue;
                const auto bone = std::find_if(owner->bones.begin(), owner->bones.end(),
                    [&](const assets::Bone& entry) { return ieq(entry.name, hardpoint.bone); });
                if (bone == owner->bones.end()) continue;
                const std::size_t bone_index = static_cast<std::size_t>(bone - owner->bones.begin());
                if (!bone->visible || bone_index >= owner_frames->bones.size() || !owner_frames->bones[bone_index]) continue;
                scene::Placement placement;
                placement.model_path = probe(cache_, "data/art/models/", hardpoint.model, model_suffixes);
                const assets::Model* attached = cache_.model(placement.model_path);
                if (attached == nullptr || attached->proxies.empty()) continue;
                placement.effects = scene::model_proxy_effects(cache_.access(), *attached);
                placement.map_logical_path = view.placement->map_logical_path;
                placement.scene_ordinal = view.placement->scene_ordinal;
                placement.record_ordinal = index + 1;
                ship.attachments.push_back({index, proxy_base, std::move(placement), *owner_frames->bones[bone_index]});
                // Identity is independent of which hardpoints are currently destroyed.
                proxy_base += attached->proxies.size();
            }
        }
    }
    for (const auto& attached : ship.attachments) {
        const auto state = attached.hardpoint < states.size() ? states[attached.hardpoint] : scene::HardpointState::intact;
        if (!scene::hardpoint_art(state).attached_model) continue;
        plan_model(ship, view, attached.placement, attached.local, attached.proxy_base, {}, {}, modes);
    }
}

void UnitEmitters::plan_model(Ship& ship, const SpacePopulation::LiveShipEmitterView& view,
                              const scene::Placement& placement, const sim::math::Mat3x4& attachment,
                              const std::size_t proxy_base, const std::span<const scene::HardpointAttachment> hardpoints,
                              const std::span<const scene::HardpointState> states, const Modes modes) {
    const assets::Model* model = nullptr;
    const ModelFrames* frames = model_frames(placement, model);
    // BP-70: root particle projectiles have no mesh or proxy bone; their emitter
    // uses the projectile's model transform, including Scale_Factor.
    if (model == nullptr && view.projectile && cache_.particle_system(placement.model_path)) {
        const EffectSystem& system = effect_system(placement.model_path);
        Wanted wanted{proxy_base, placement.model_declared, placement.model_path,
            attachment, static_cast<std::uint32_t>(plan_seed) + static_cast<std::uint32_t>(view.entity), system.capacity};
        wanted.size_scale = static_cast<float>(placement.scale_raw) / static_cast<float>(sim::math::Fixed::scale);
        if (!system.system) {
            wanted.failed = true;
            ++start_failed_[wanted.effect + ": " + system.cause];
        }
        ship.wanted.push_back(std::move(wanted));
        return;
    }
    if (model == nullptr || frames == nullptr || model->proxies.empty()) return;
    std::vector<particles::EffectEvidence> effects(model->proxies.size());
    for (std::size_t ordinal = 0; ordinal < model->proxies.size() && ordinal < placement.effects.size(); ++ordinal) {
        const std::string& path = placement.effects[ordinal].resolved;
        if (path.empty()) continue;
        const EffectSystem& system = effect_system(path);
        effects[ordinal] = {system.system ? particles::EffectKind::particle
                                : cache_.model(path) ? particles::EffectKind::non_particle
                                                     : particles::EffectKind::unknown,
                            system.capacity};
    }
    // BP-41 to BP-43: the hardpoint states decide which damage and engine emitters run.
    const std::vector<std::uint8_t> hidden = scene::hidden_hardpoint_proxies(*model, hardpoints, states);
    // BP-43, AB-31/32, IS-09: the emitter-type switch clears the same code-hidden flag
    // the model's authored visibility sets. The Tartan's hidden pptw_ptwsa must therefore
    // run while POWER_TO_WEAPONS is active, just like its hidden ion-stun proxy.
    std::vector<std::uint8_t> shown(model->proxies.size(), 0U);
    for (std::size_t ordinal = 0; ordinal < shown.size(); ++ordinal) {
        const std::string& name = model->proxies[ordinal].name;
        shown[ordinal] = ((modes.ion_stunned && ion_stun_emitter(name))
            || (modes.power_to_weapons && power_to_weapons_emitter(name))
            || (modes.invulnerability && invulnerability_emitter(name))
            || (modes.turbo && modes.engines_online && turbo_emitter(name))) ? 1U : 0U;
    }
    // Planned in the ship's model space: the identity placement makes each record's frame the
    // proxy bone's bind frame; the ship's pose is composed with it every sample.
    scene::Placement local = placement;
    local.transform = scene::Transform{};
    local.transform->matrix = attachment;
    local.scale_raw = sim::math::Fixed::scale;
    const particles::MapEffectPlacementInput input{model, &local, frames->bones, effects,
        frames->bind_pose ? particles::VisibilityEvidence::bind_pose : particles::VisibilityEvidence::unknown,
        std::nullopt, std::nullopt, hidden, shown};
    const particles::MapEffectPlan planned = particles::plan_map_effects({&input, 1}, plan_seed, SIZE_MAX / 2);
    for (const particles::MapEffectRecord& record : planned.records) {
        if (record.status != particles::MapEffectStatus::admitted || !record.emitter_frame) {
            if (record.cause != particles::MapEffectCause::hardpoint_state) {
                ++not_admitted_[record.proxy_name + ": " + record.detail];
            }
            continue;
        }
        // BP-43: hidden by type when the object is created, until an ability shows the type:
        // TURBO and SPOILER_LOCK the turbo engines, POWER_TO_WEAPONS its effect (AB-31, AB-32).
        // The authored-hidden flag is overridden by the same type switch above (BP-43).
        const bool shown_by_ability = (modes.turbo && modes.engines_online && turbo_emitter(record.proxy_name))
            || (modes.power_to_weapons && power_to_weapons_emitter(record.proxy_name))
            || (modes.invulnerability && invulnerability_emitter(record.proxy_name));
        if (hidden_at_creation(record.proxy_name) && !shown_by_ability) {
            ++not_admitted_[record.proxy_name + ": emitter type hidden at creation"];
            continue;
        }
        // BP-42: engine emitters stop once the engines are permanently off-line, and while the
        // turbo engines stand in for them (AB-31).
        if ((!modes.engines_online || modes.turbo) && engine_emitter(record.proxy_name)) continue;
        Wanted wanted{proxy_base + record.proxy_ordinal, record.proxy_name, record.effect_logical_path, *record.emitter_frame,
                      record.seed + static_cast<std::uint32_t>(view.entity), record.capacity};
        // BP-45: the brightness goes to the engine type in use.
        wanted.engine = engine_emitter(record.proxy_name) || turbo_emitter(record.proxy_name);
        wanted.ion_stun = ion_stun_emitter(record.proxy_name);
        wanted.power_to_weapons = power_to_weapons_emitter(record.proxy_name);
        wanted.invulnerability = invulnerability_emitter(record.proxy_name);
        const EffectSystem& system = effect_system(wanted.effect);
        if (!system.system) {
            wanted.failed = true;
            ++start_failed_[wanted.effect + ": " + system.cause];
        } else if (std::any_of(system.system->emitters.begin(), system.system->emitters.end(), [&](const auto& emitter) {
                       return system.system->version == particles::AloParticleVersion::legacy_v1 && emitter.cpu_ready
                           && emitter.creator_id == 35;
                   })) {
            // The static attached plan's mesh binding: the first mesh on the proxy bone's parent,
            // found by the proxy's index, as a model may name several proxies alike (the X-wing's
            // and Z-95's twin pe_Z95engines).
            auto binding = particles::bind_proxy_mesh(*model, record.proxy_ordinal);
            if (binding && binding.value().owner_bone < frames->bones.size() && frames->bones[binding.value().owner_bone]) {
                wanted.mesh = std::move(binding.value().binding);
                const auto frame = sim::math::compose(attachment, *frames->bones[binding.value().owner_bone]);
                if (frame) wanted.mesh_local = frame.value();
                else wanted.failed = true;
            } else {
                wanted.failed = true;
                ++start_failed_[wanted.effect + ": mesh binding: "
                                + (binding ? std::string("owner bone has no bind frame")
                                           : core::format_diagnostic(binding.error()))];
            }
        }
        ship.wanted.push_back(std::move(wanted));
    }
}

void UnitEmitters::plan_clone(Ship& ship, const SpacePopulation::LiveShipEmitterView& view) {
    ship.clone_planned = true;
    ++clone_ships_;
    const assets::Model* model = cache_.model(view.placement->model_path);
    if (model == nullptr) {
        ++clone_not_run_[view.placement->model_path + ": model not loaded"];
        return;
    }
    for (std::size_t ordinal = 0; ordinal < model->proxies.size(); ++ordinal) {
        const assets::Proxy& proxy = model->proxies[ordinal];
        const auto skip = [&](const std::string& cause) { ++clone_not_run_[proxy.name + ": " + cause]; };
        // FoC hides a proxy with its hidden flag set for good (BP-46, the debug build).
        if (!proxy.visible) {
            skip("hidden by its proxy flag");
            continue;
        }
        const std::string upper = [&] {
            std::string text = proxy.name;
            for (char& character : text) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            return text;
        }();
        if (upper.find("_ALT") != std::string::npos || upper.find("_LOD") != std::string::npos) {
            skip("alternate or LOD proxy (not modelled on a death clone)");
            continue;
        }
        if (proxy.bone >= model->bones.size()) {
            skip("bone out of range");
            continue;
        }
        const std::string path = ordinal < view.placement->effects.size() ? view.placement->effects[ordinal].resolved
                                                                             : std::string{};
        if (path.empty()) {
            skip("effect unresolved");
            continue;
        }
        const EffectSystem& system = effect_system(path);
        if (!system.system) {
            skip(system.cause);
            continue;
        }
        // Every clone draws from its own seeds: the unit, then the proxy's index.
        const auto seed = static_cast<std::uint32_t>((plan_seed * 0x9e3779b9ULL) ^ (view.entity * 0x85ebca6bULL)
                                                     ^ (ordinal * 0xc2b2ae35ULL)) | 1U;
        CloneProxy entry{ordinal, proxy.name, proxy.bone, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
        const particles::SystemDefinition* definition = &*system.system;
        if (std::any_of(system.system->emitters.begin(), system.system->emitters.end(), [&](const auto& emitter) {
                return system.system->version == particles::AloParticleVersion::legacy_v1 && emitter.cpu_ready
                    && emitter.creator_id == 35;
            })) {
            // An EnhancedMesh system emits from the first mesh on the proxy bone's parent, bound as
            // the engine emitters bind theirs (BP-42), by the proxy's index.
            auto binding = particles::bind_proxy_mesh(*model, ordinal);
            if (!binding || binding.value().owner_bone >= model->bones.size()) {
                skip("mesh binding: " + (binding ? std::string("owner bone out of range")
                                                 : core::format_diagnostic(binding.error())));
                continue;
            }
            entry.mesh_bone = binding.value().owner_bone;
            entry.life.emplace(*registry_,
                [definition, capacity = system.capacity, mesh = std::move(binding.value().binding)](
                    particles::EffectRegistry& registry, const std::uint32_t draw) {
                    return registry.spawn(*definition, draw, capacity, mesh);
                },
                seed, clone_reappearance);
        } else {
            entry.life.emplace(*registry_,
                [definition, capacity = system.capacity](particles::EffectRegistry& registry, const std::uint32_t draw) {
                    return registry.spawn(*definition, draw, capacity);
                },
                seed, clone_reappearance);
        }
        ship.clone.push_back(std::move(entry));
    }
}

} // namespace eawr::presentation::godot_backend
