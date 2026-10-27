#include "unit_emitters.hpp"
#include "eawr/assets/geometry_stripped.hpp"

#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/presentation/particles/map_effect_plan.hpp"
#include "eawr/presentation/particles/prewarmed_capacity.hpp"
#include "eawr/presentation/particles/proxy_binding.hpp"
#include "eawr/presentation/space/debris.hpp"
#include "eawr/presentation/live/live_units.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <set>
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

// #4218: a reused source slot seeds its emitters from the slot the session's reservation policy bound the unit to
// (view.identity is the ship's own placement unless the slot was reused early). Attached models keep their own
// record ordinal; only the scene ordinal they inherit from the ship follows the identity.
void apply_seed_identity(scene::Placement& local, const SpacePopulation::LiveShipEmitterView& view, const bool root) {
    if (view.identity == nullptr) return;
    local.scene_ordinal = view.identity->scene_ordinal;
    if (root) local.record_ordinal = view.identity->record_ordinal;
}

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
    return has_prefix(name, "pte") || has_prefix(name, "pptw") || has_prefix(name, "prs") || has_prefix(name, "pem");
}
[[nodiscard]] bool engine_emitter(const std::string_view name) { return has_prefix(name, "pe"); }
[[nodiscard]] bool turbo_emitter(const std::string_view name) { return has_prefix(name, "pte"); }
[[nodiscard]] bool power_to_weapons_emitter(const std::string_view name) { return has_prefix(name, "pptw"); }
[[nodiscard]] bool ion_stun_emitter(const std::string_view name) { return has_prefix(name, "pi"); }
[[nodiscard]] bool invulnerability_emitter(const std::string_view name) { return has_prefix(name, "pem"); }
[[nodiscard]] bool gravity_well_emitter(const std::string_view name) { return has_prefix(name, "pgw"); }
[[nodiscard]] bool missile_shield_emitter(const std::string_view name) { return has_prefix(name, "prs"); }

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(character == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

[[nodiscard]] const char* clone_proxy_skip(const assets::Proxy& proxy, const assets::Model& model) {
    if (!proxy.visible) return "hidden by its proxy flag";
    std::string upper = proxy.name;
    for (char& character : upper) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    if (upper.find("_ALT") != std::string::npos || upper.find("_LOD") != std::string::npos) {
        return "alternate or LOD proxy (not modelled on a death clone)";
    }
    if (proxy.bone >= model.bones.size()) return "bone out of range";
    return nullptr;
}

// An EnhancedMesh system (legacy v1, CPU ready, creator 35) emits from a bound mesh.
template<class System>
[[nodiscard]] bool mesh_emitter(const System& system) {
    return system.system && std::any_of(system.system->emitters.begin(), system.system->emitters.end(), [&](const auto& emitter) {
        return system.system->version == particles::AloParticleVersion::legacy_v1 && emitter.cpu_ready
            && emitter.creator_id == 35;
    });
}
} // namespace

void UnitEmitters::prepare_assets(const SpacePopulation& population) {
    if (assets_prepared_) return;
    const auto began = std::chrono::steady_clock::now();
    std::set<std::string> models, frames, systems, sources, attached_models;
    std::map<std::string, std::string> attached_paths;
    const auto source = [&](const std::string& path) {
        if (sources.insert(path).second) {
            if (const auto entry = filesystem_->stat(path)) prepared_source_bytes_ += entry.value().size;
        }
    };
    const auto prepare_system = [&](const std::string& path) {
        if (path.empty() || !systems.insert(path).second) return;
        ++prepared_system_requests_;
        source(path);
        if (effect_system(path).system) ++prepared_systems_;
    };
    const auto prepare_model = [&](const scene::Placement& placement, const bool death_clone) {
        const auto& path = placement.model_path;
        if (path.empty()) return;
        const assets::Model* model = emitter_model(path);
        if (models.insert(path).second) {
            ++prepared_model_requests_;
            source(path);
            if (model) ++prepared_models_;
        }
        if (!death_clone && model && frames.insert(path).second) static_cast<void>(model_frames(placement, model));
        if (!model && !death_clone) prepare_system(path); // A particle-system model can own a projectile slot.
        for (std::size_t ordinal = 0; ordinal < placement.effects.size(); ++ordinal) {
            if (death_clone && (!model || ordinal >= model->proxies.size()
                || clone_proxy_skip(model->proxies[ordinal], *model))) continue;
            const auto& effect = placement.effects[ordinal];
            prepare_system(effect.resolved);
            // EnhancedMesh emitters bind a mesh at plan time; decode it here, never on the frame path.
            if (model && ordinal < model->proxies.size() && mesh_emitter(effect_system(effect.resolved))) {
                static_cast<void>(emitter_binding(path, ordinal));
            }
            // Non-particle model proxies use the same cached classification as plan_model.
            if (!effect.resolved.empty() && !effect_system(effect.resolved).system) {
                if (models.insert(effect.resolved).second) {
                    ++prepared_model_requests_;
                    source(effect.resolved);
                    if (emitter_model(effect.resolved)) ++prepared_models_;
                }
            }
        }
    };
    for (std::size_t ship = 0; ship < population.live_ship_count(); ++ship) {
        const auto view = population.live_ship_emitter_assets(ship);
        if (!view || !view->placement) continue;
        prepare_model(*view->placement, view->death_clone);
        if (view->death_clone) continue; // Clone planning reads its own model proxies only.
        for (const auto& hardpoint : view->hardpoints) {
            if (!hardpoint.resolved || hardpoint.model.empty()) continue;
            scene::Placement attached;
            auto [path, inserted] = attached_paths.try_emplace(hardpoint.model);
            if (inserted) path->second = probe(cache_, "data/art/models/", hardpoint.model, model_suffixes);
            attached.model_path = path->second;
            if (attached.model_path.empty() || !attached_models.insert(attached.model_path).second) continue;
            if (const auto* model = emitter_model(attached.model_path)) {
                attached.effects = scene::model_proxy_effects(cache_.access(), *model);
            }
            prepare_model(attached, false);
        }
    }
    asset_prepare_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    assets_prepared_ = true;
}

const assets::Model* UnitEmitters::emitter_model(const std::string& path) {
    if (model_paths_.insert(path).second && assets_prepared_) ++unprepared_models_[path];
    const auto found = emitter_models_.find(path);
    if (found != emitter_models_.end()) return found->second.get();
    std::unique_ptr<const assets::Model> metadata;
    if (auto loaded = cache_.shared_model(path)) {
        metadata = std::make_unique<const assets::Model>(assets::geometry_stripped_copy(*loaded.value()));
    }
    return emitter_models_.emplace(path, std::move(metadata)).first->second.get();
}

const core::Result<particles::ProxyMeshBinding>& UnitEmitters::emitter_binding(const std::string& path,
                                                                                const std::size_t ordinal) {
    auto key = std::make_pair(path, ordinal);
    const auto found = emitter_bindings_.find(key);
    if (found != emitter_bindings_.end()) return found->second;
    using Binding = core::Result<particles::ProxyMeshBinding>;
    auto loaded = cache_.shared_model(path);
    Binding binding = loaded ? particles::bind_proxy_mesh(*loaded.value(), ordinal)
                             : Binding::failure(loaded.error());
    return emitter_bindings_.emplace(std::move(key), std::move(binding)).first->second;
}

const core::Result<particles::ProxyMeshBinding>* UnitEmitters::planned_binding(const std::string& path,
                                                                                const std::size_t ordinal) {
    // Prepared bindings only once preparation is done: a late miss is counted and skipped, not decoded.
    if (!assets_prepared_) return &emitter_binding(path, ordinal);
    const auto found = emitter_bindings_.find(std::make_pair(path, ordinal));
    if (found != emitter_bindings_.end()) return &found->second;
    ++unprepared_bindings_[path + "#" + std::to_string(ordinal)];
    return nullptr;
}

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
    if (assets_prepared_) ++unprepared_systems_[path];
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
    model = emitter_model(placement.model_path);
    if (model == nullptr) return nullptr;
    auto found = frames_.find(placement.model_path);
    if (found != frames_.end()) return &found->second;
    if (assets_prepared_) ++unprepared_frames_[placement.model_path];
    // The bind pose, as the static attached plan reads it: capital ships run no idle clip that
    // moves their proxy bones.
    ModelFrames frames;
    frames.bones.resize(model->bones.size());
    // BP-74/PS-03: a proxy can be on a child bone of the exact charge mesh's bone.
    // Cache that ancestry once per model; phase replanning only reads the proxy ordinal.
    frames.blast_phases.resize(model->proxies.size());
    for (const auto& mesh : model->meshes) {
        const auto name = lower(mesh.name);
        if ((name != "buildup" && name != "charged") || mesh.bone < 0) continue;
        for (std::size_t ordinal = 0; ordinal < model->proxies.size(); ++ordinal) {
            auto bone = static_cast<std::int32_t>(model->proxies[ordinal].bone);
            for (std::size_t depth = 0; bone >= 0 && depth < model->bones.size(); ++depth) {
                if (bone == mesh.bone) { frames.blast_phases[ordinal] = name == "charged"; break; }
                bone = model->bones[static_cast<std::size_t>(bone)].parent;
            }
        }
    }
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
                const assets::Model* attached = emitter_model(placement.model_path);
                if (attached == nullptr || attached->proxies.empty()) continue;
                placement.effects = scene::model_proxy_effects(cache_.access(), *attached);
                placement.map_logical_path = view.placement->map_logical_path;
                placement.scene_ordinal = view.placement->scene_ordinal;
                placement.record_ordinal = static_cast<std::uint32_t>(index + 1);
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
                                : emitter_model(path) ? particles::EffectKind::non_particle
                                                     : particles::EffectKind::unknown,
                            system.capacity};
    }
    // BP-41 to BP-43: the hardpoint states decide which damage and engine emitters run.
    std::vector<std::uint8_t> hidden = scene::hidden_hardpoint_proxies(*model, hardpoints, states);
    // BP-74: these are exact charge-state mesh/bone names, not emitter type prefixes.
    // Their ordinary descendant proxies inherit the owning mesh's visibility (WHE-35/36, FD-55).
    for (std::size_t ordinal = 0; ordinal < model->proxies.size(); ++ordinal) {
        const auto phase = frames->blast_phases[ordinal];
        if (phase && !(*phase ? modes.blast_charged : modes.blast_charging)) {
            hidden[ordinal] = 1U;
        }
    }
    // BP-43, AB-31/32, IS-09: the emitter-type switch clears the same code-hidden flag
    // the model's authored visibility sets. The Tartan's hidden pptw_ptwsa must therefore
    // run while POWER_TO_WEAPONS is active, just like its hidden ion-stun proxy.
    std::vector<std::uint8_t> shown(model->proxies.size(), 0U);
    for (std::size_t ordinal = 0; ordinal < shown.size(); ++ordinal) {
        const std::string& name = model->proxies[ordinal].name;
        shown[ordinal] = ((modes.ion_stunned && ion_stun_emitter(name))
            || (modes.power_to_weapons && power_to_weapons_emitter(name))
            || (modes.invulnerability && invulnerability_emitter(name))
            || (modes.gravity_well && gravity_well_emitter(name))
            || (modes.missile_shield && missile_shield_emitter(name))
            || (modes.turbo && modes.engines_online && turbo_emitter(name))) ? 1U : 0U;
    }
    // Planned in the ship's model space: the identity placement makes each record's frame the
    // proxy bone's bind frame; the ship's pose is composed with it every sample.
    scene::Placement local = placement;
    apply_seed_identity(local, view, &placement == view.placement);
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
        // IS-09: the ion-stun behaviour initializes its emitters hidden, even when
        // the model authors them visible. Only a live stun shows them; expiry hides them again.
        if (modes.ion_stun_effect && !modes.ion_stunned && ion_stun_emitter(record.proxy_name)) {
            ++not_admitted_[record.proxy_name + ": ion stun inactive"];
            continue;
        }
        // BP-43: hidden by type when the object is created, until an ability shows the type:
        // TURBO and SPOILER_LOCK the turbo engines, POWER_TO_WEAPONS its effect (AB-31, AB-32).
        // The authored-hidden flag is overridden by the same type switch above (BP-43).
        const bool shown_by_ability = (modes.turbo && modes.engines_online && turbo_emitter(record.proxy_name))
            || (modes.power_to_weapons && power_to_weapons_emitter(record.proxy_name))
            || (modes.invulnerability && invulnerability_emitter(record.proxy_name))
            || (modes.gravity_well && gravity_well_emitter(record.proxy_name))
            || (modes.missile_shield && missile_shield_emitter(record.proxy_name));
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
        wanted.gravity_well = gravity_well_emitter(record.proxy_name);
        wanted.missile_shield = missile_shield_emitter(record.proxy_name);
        wanted.blast_effect = frames->blast_phases[record.proxy_ordinal].has_value();
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
            const auto* found_binding = planned_binding(placement.model_path, record.proxy_ordinal);
            if (found_binding == nullptr) {
                wanted.failed = true;
                ++start_failed_[wanted.effect + ": mesh binding: not prepared"];
                ship.wanted.push_back(std::move(wanted));
                continue;
            }
            auto binding = *found_binding;
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
    const assets::Model* model = emitter_model(view.placement->model_path);
    if (model == nullptr) {
        ++clone_not_run_[view.placement->model_path + ": model not loaded"];
        return;
    }
    for (std::size_t ordinal = 0; ordinal < model->proxies.size(); ++ordinal) {
        const assets::Proxy& proxy = model->proxies[ordinal];
        const auto skip = [&](const std::string& cause) { ++clone_not_run_[proxy.name + ": " + cause]; };
        // FoC hides a proxy with its hidden flag set for good (BP-46, the debug build).
        if (const char* cause = clone_proxy_skip(proxy, *model)) {
            skip(cause);
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
            const auto* found_binding = planned_binding(view.placement->model_path, ordinal);
            if (found_binding == nullptr) {
                skip("mesh binding: not prepared");
                continue;
            }
            auto binding = *found_binding;
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
