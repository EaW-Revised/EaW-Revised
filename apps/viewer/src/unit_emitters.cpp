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

namespace eawr::presentation::godot_backend {
namespace {

// The per-emitter particle allocation of the static space map's attached plan
// (--eawr-map-attached-capacity default), raised for prewarmed continuous emitters.
constexpr std::size_t fallback_capacity = 256;
// The plan's seed: every ship's emitters draw from their own record seed.
constexpr std::uint64_t plan_seed = 394;
// BP-65: a hidden engine emitter drains for at most this many clock samples (10 s) before it is cut short.
constexpr std::uint64_t engine_drain_limit_samples = 300;
// #429 (BP-48): a death clone's proxy whose piece shows again is reset, as the debug
// build resets the group when its host shows again: its old drain goes.
constexpr auto clone_reappearance = particles::ReappearancePolicy::reset;

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
    return has_prefix(name, "pte") || has_prefix(name, "pptw") || has_prefix(name, "pgw");
}
[[nodiscard]] bool engine_emitter(const std::string_view name) { return has_prefix(name, "pe"); }
[[nodiscard]] bool turbo_emitter(const std::string_view name) { return has_prefix(name, "pte"); }
[[nodiscard]] bool power_to_weapons_emitter(const std::string_view name) { return has_prefix(name, "pptw"); }
[[nodiscard]] bool ion_stun_emitter(const std::string_view name) { return has_prefix(name, "pi"); }

// The emitter modes a unit's snapshot instance sets (BP-42, BP-43, space-abilities AB-31, AB-32).
[[nodiscard]] UnitEmitters::Modes modes_of(const sim::tactical::TacticalInstance* instance) {
    UnitEmitters::Modes modes;
    if (instance == nullptr) return modes;
    modes.engines_online = !instance->durability || instance->durability->engines_online;
    // IS-09: the stun shows its emitters from the hit until its end frame (IS-03).
    modes.ion_stunned = instance->ion_stun_frames > 0;
    for (const auto& ability : instance->abilities) {
        if (!ability.active) continue;
        if (ability.kind == sim::tactical::AbilityKind::turbo || ability.kind == sim::tactical::AbilityKind::spoiler_lock) {
            modes.turbo = true;
        }
        if (ability.kind == sim::tactical::AbilityKind::power_to_weapons) modes.power_to_weapons = true;
    }
    return modes;
}

[[nodiscard]] double to_double(const sim::math::Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
}

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(character == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

[[nodiscard]] std::string json(const std::string_view text) {
    std::string result{"\""};
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            result += '\\';
            result += character;
        } else if (static_cast<unsigned char>(character) < 0x20U) {
            result += ' ';
        } else {
            result += character;
        }
    }
    return result + "\"";
}

void counts(std::ostream& output, const char* name, const std::map<std::string, std::uint64_t>& values) {
    output << ", \"" << name << "\": {";
    bool first = true;
    for (const auto& [key, count] : values) {
        output << (first ? "" : ", ") << json(key) << ": " << count;
        first = false;
    }
    output << "}";
}

} // namespace

UnitEmitters::UnitEmitters(godot::Node3D& host, const vfs::Vfs& filesystem)
    : host_(&host), filesystem_(&filesystem), cache_(filesystem),
      backend_(std::make_unique<GodotParticleBackend>(
          host, [this](const std::string_view name) { return resolve_texture(name); })),
      registry_(std::make_unique<particles::EffectRegistry>(*backend_)) {
    // #638: nothing here reads the streams' hashes, so the frames do not compute them.
    registry_->set_stream_hashes(false);
}

UnitEmitters::~UnitEmitters() { release(); }

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
        entry.capacity = particles::prewarmed_capacity(system.value(), fallback_capacity);
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
    ship.planned = true;
    ship.states.assign(states.begin(), states.end());
    ship.modes = modes;
    ship.wanted.clear();
    ++plans_;
    const assets::Model* model = nullptr;
    const ModelFrames* frames = model_frames(*view.placement, model);
    if (model == nullptr || frames == nullptr || model->proxies.empty()) return;
    std::vector<particles::EffectEvidence> effects(model->proxies.size());
    for (std::size_t ordinal = 0; ordinal < model->proxies.size() && ordinal < view.placement->effects.size(); ++ordinal) {
        const std::string& path = view.placement->effects[ordinal].resolved;
        if (path.empty()) continue;
        const EffectSystem& system = effect_system(path);
        effects[ordinal] = {system.system ? particles::EffectKind::particle
                                : cache_.model(path) ? particles::EffectKind::non_particle
                                                     : particles::EffectKind::unknown,
                            system.capacity};
    }
    // BP-41 to BP-43: the hardpoint states decide which damage and engine emitters run.
    const std::vector<std::uint8_t> hidden = scene::hidden_hardpoint_proxies(*model, view.hardpoints, states);
    // IS-09: a stun clears the code-hidden flag of every ion-stun proxy, the flag an authored
    // hidden proxy is loaded with, so the Tartan's authored-hidden pi_damage_elec_cap00 runs.
    std::vector<std::uint8_t> shown(model->proxies.size(), 0U);
    if (modes.ion_stunned) {
        for (std::size_t ordinal = 0; ordinal < shown.size(); ++ordinal) {
            shown[ordinal] = ion_stun_emitter(model->proxies[ordinal].name) ? 1U : 0U;
        }
    }
    // Planned in the ship's model space: the identity placement makes each record's frame the
    // proxy bone's bind frame; the ship's pose is composed with it every sample.
    scene::Placement local = *view.placement;
    local.transform = scene::Transform{};
    local.transform->matrix = sim::math::identity_matrix();
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
        // An authored-hidden proxy stays hidden: the type switch clears only the code flag.
        const bool shown_by_ability = (modes.turbo && modes.engines_online && turbo_emitter(record.proxy_name))
            || (modes.power_to_weapons && power_to_weapons_emitter(record.proxy_name));
        if (hidden_at_creation(record.proxy_name) && !shown_by_ability) {
            ++not_admitted_[record.proxy_name + ": emitter type hidden at creation"];
            continue;
        }
        // BP-42: engine emitters stop once the engines are permanently off-line, and while the
        // turbo engines stand in for them (AB-31).
        if ((!modes.engines_online || modes.turbo) && engine_emitter(record.proxy_name)) continue;
        Wanted wanted{record.proxy_ordinal, record.proxy_name, record.effect_logical_path, *record.emitter_frame,
                      record.seed + static_cast<std::uint32_t>(view.entity), record.capacity};
        // BP-45: the brightness goes to the engine type in use.
        wanted.engine = engine_emitter(record.proxy_name) || turbo_emitter(record.proxy_name);
        wanted.ion_stun = ion_stun_emitter(record.proxy_name);
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
                wanted.mesh_local = frames->bones[binding.value().owner_bone];
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

void UnitEmitters::stop_all(Ship& ship, const char* reason) {
    for (const Running& running : ship.running) static_cast<void>(registry_->release(running.handle));
    if (!ship.running.empty()) stopped_[reason] += ship.running.size();
    ship.running.clear();
}

void UnitEmitters::update(Ship& ship, const SpacePopulation::LiveShipEmitterView& view,
                          const std::span<const scene::HardpointState> states, const Modes modes,
                          const std::uint64_t tick, const std::uint64_t born) {
    // BP-65: only the turbo swap's hidden engine emitters drain (the recording shows that one).
    const bool turbo_swap = ship.planned && ship.modes.turbo != modes.turbo;
    // IS-09: the stun's end hides its emitters as the swap hides the engines' (BP-65): their
    // residual particles drain.
    const bool stun_ended = ship.planned && ship.modes.ion_stunned && !modes.ion_stunned;
    if (!ship.planned || ship.modes != modes
        || !std::equal(ship.states.begin(), ship.states.end(), states.begin(), states.end())) {
        plan(ship, view, states, modes);
    }
    // Stop what the plan no longer admits (BP-43), start what it newly admits (BP-41, BP-42).
    for (auto running = ship.running.begin(); running != ship.running.end();) {
        const bool kept = std::any_of(ship.wanted.begin(), ship.wanted.end(),
                                      [&](const Wanted& wanted) { return wanted.proxy == running->proxy; });
        if (kept || running->draining) {
            ++running;
            continue;
        }
        // BP-65: an engine emitter that the turbo swap hides stops emitting and its residual
        // particles drain, as a hidden proxy's do in FoC (rig recording, #559).
        if (((turbo_swap && running->engine) || (stun_ended && running->ion_stun))
            && registry_->stop_emission(running->handle)) {
            running->draining = true;
            running->drain_from = samples_;
            ++(running->engine ? engine_drains_started_ : ion_stun_drains_started_);
            ++running;
            continue;
        }
        static_cast<void>(registry_->release(running->handle));
        ++stopped_["hardpoint_state"];
        running = ship.running.erase(running);
    }
    for (Wanted& wanted : ship.wanted) {
        if (wanted.failed) continue;
        const bool present = std::any_of(ship.running.begin(), ship.running.end(), [&](const Running& running) {
            return running.proxy == wanted.proxy && !running.draining;
        });
        if (present) continue;
        const EffectSystem& system = effect_system(wanted.effect);
        auto handle = wanted.mesh ? registry_->spawn(*system.system, wanted.seed, wanted.capacity, *wanted.mesh)
                                  : registry_->spawn(*system.system, wanted.seed, wanted.capacity);
        if (!handle) {
            wanted.failed = true;
            ++start_failed_[wanted.effect + ": " + core::format_diagnostic(handle.error())];
            continue;
        }
        std::size_t log = start_log_limit;
        if (start_log_.size() < start_log_limit) {
            log = start_log_.size();
            start_log_.push_back({view.entity, wanted.proxy_name, tick, born, std::nullopt, std::nullopt, 0});
        }
        ship.running.push_back(
            {wanted.proxy, handle.value(), wanted.local, wanted.mesh_local, wanted.engine, wanted.ion_stun, born, log});
        ++started_[wanted.proxy_name];
    }
}

void UnitEmitters::place(const std::size_t index, const sim::math::Mat3x4& pose) {
    Ship& ship = ships_[index];
    for (auto running = ship.running.begin(); running != ship.running.end();) {
        std::optional<particles::EmitterFrame> frame;
        std::optional<particles::MeshFrame> mesh;
        if (auto world = sim::math::compose(pose, running->local)) frame = particles::source_emitter_frame(world.value());
        if (running->mesh_local) {
            if (auto world = sim::math::compose(pose, *running->mesh_local)) {
                const particles::EmitterFrame owner = particles::source_emitter_frame(world.value());
                mesh = particles::MeshFrame{owner.origin, owner.basis};
            }
        }
        const bool placed = frame && (!running->mesh_local || mesh) && registry_->set_frame(running->handle, *frame)
            && (!mesh || registry_->set_mesh_frame(running->handle, *mesh));
        if (!placed) {
            static_cast<void>(registry_->release(running->handle));
            ++stopped_["frame_overflow"];
            running = ship.running.erase(running);
            continue;
        }
        if (running->log < start_log_.size() && !start_log_[running->log].origin) {
            start_log_[running->log].origin = std::array<float, 3>{frame->origin.x, frame->origin.y, frame->origin.z};
        }
        ++running;
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

bool UnitEmitters::step_clone_proxy(CloneProxy& proxy, const bool visible, const particles::EmitterFrame& frame,
                                    const particles::MeshFrame* mesh, const sim::EntityId entity,
                                    const std::uint64_t tick, const std::uint64_t born) {
    auto step = proxy.life->step(visible, frame, mesh, 1.0F / 30.0F, camera_frame_);
    if (!step) {
        // A clone emitter that fails is reported and let go; the battle view goes on.
        ++clone_failed_[proxy.name + ": " + core::format_diagnostic(step.error())];
        static_cast<void>(proxy.life->release_all());
        proxy.life.reset();
        return false;
    }
    proxy.last = frame;
    if (mesh != nullptr) proxy.last_mesh = *mesh;
    if (step.value().spawned) {
        ++clone_started_[proxy.name];
        if (clone_log_.size() < start_log_limit) clone_log_.push_back({entity, proxy.name, tick, born});
    }
    if (step.value().detached) ++clone_hidden_[proxy.name];
    clone_drains_released_ += step.value().drains_released;
    clone_drains_cut_short_ += step.value().drains_cut_short;
    clone_drains_reset_ += step.value().drains_reset;
    clone_particles_now_ += step.value().stats.particles;
    return true;
}

void UnitEmitters::step_clone(const std::size_t index, const SpacePopulation::LiveShipEmitterView& view,
                              const std::optional<ClonePose>& pose, const std::uint64_t tick, const std::uint64_t born) {
    Ship& ship = ships_[index];
    if (!pose) {
        orphan_clone(ship);
        return;
    }
    if (!ship.clone_planned) plan_clone(ship, view);
    // A bone's frame on the clip pose, in world space.
    const auto bone_frame = [&](const std::size_t bone) -> std::optional<particles::EmitterFrame> {
        if (bone >= pose->bones.size()) return std::nullopt;
        const auto local = particles::fixed_model_frame(pose->bones[bone].model_asset, scene::fixed_from_binary32);
        if (!local) return std::nullopt;
        auto world = sim::math::compose(pose->model_to_world, *local);
        if (!world) return std::nullopt;
        return particles::source_emitter_frame(world.value());
    };
    for (CloneProxy& proxy : ship.clone) {
        if (!proxy.life) continue;
        // The proxy stands on its bone of the clip pose (BP-46, the debug build), and runs
        // while the clip shows that bone; a hidden bone's instance drains where it stands.
        std::optional<particles::EmitterFrame> frame = bone_frame(proxy.bone);
        std::optional<particles::MeshFrame> mesh;
        if (proxy.mesh_bone) {
            if (const auto owner = bone_frame(*proxy.mesh_bone)) mesh = particles::MeshFrame{owner->origin, owner->basis};
        }
        const bool placed = frame && (!proxy.mesh_bone || mesh);
        const bool visible = placed && pose->bones[proxy.bone].visible;
        if (!placed) {
            frame = proxy.last;
            mesh = proxy.last_mesh;
        }
        if (!frame || (proxy.mesh_bone && !mesh)) continue;  // never placed: nothing runs yet
        static_cast<void>(step_clone_proxy(proxy, visible, *frame, mesh ? &*mesh : nullptr, view.entity, tick, born));
    }
    std::erase_if(ship.clone, [](const CloneProxy& proxy) { return !proxy.life; });
}

void UnitEmitters::orphan_clone(Ship& ship) {
    for (CloneProxy& proxy : ship.clone) {
        if (proxy.life && proxy.last && (proxy.life->active() || !proxy.life->draining().empty())) {
            clone_orphans_.push_back(std::move(proxy));
        }
    }
    ship.clone.clear();
}

bool UnitEmitters::advance_sample() {
    // #421: a clone's proxies that outlived it stop emitting and drain where they last stood.
    for (CloneProxy& proxy : clone_orphans_) {
        static_cast<void>(step_clone_proxy(proxy, false, *proxy.last, proxy.last_mesh ? &*proxy.last_mesh : nullptr,
                                           sim::EntityId{}, 0U, samples_));
    }
    std::erase_if(clone_orphans_, [](const CloneProxy& proxy) {
        return !proxy.life || (!proxy.life->active() && proxy.life->draining().empty());
    });
    std::uint64_t clone_live = 0;
    for (const Ship& ship : ships_) {
        for (const CloneProxy& proxy : ship.clone) {
            if (proxy.life) clone_live += proxy.life->draining().size() + (proxy.life->active() ? 1U : 0U);
        }
    }
    for (const CloneProxy& proxy : clone_orphans_) clone_live += proxy.life->draining().size() + (proxy.life->active() ? 1U : 0U);
    clone_max_live_ = std::max(clone_max_live_, clone_live);
    clone_max_particles_ = std::max(clone_max_particles_, clone_particles_now_);
    clone_particles_now_ = 0;
    std::uint64_t particles_now = 0;
    // #638: every running emitter steps in one batch on the particle workers; the results are
    // then taken in the running order, as one advance after another gave them.
    batch_handles_.clear();
    for (const Ship& ship : ships_) {
        for (const Running& running : ship.running) batch_handles_.push_back(running.handle);
    }
    if (auto advanced = registry_->advance_all(batch_handles_, 1.0F / 30.0F, camera_frame_, batch_stats_); !advanced) {
        failure_ = "unit emitter: " + core::format_diagnostic(advanced.error());
        return false;
    }
    std::size_t next = 0;
    for (Ship& ship : ships_) {
        for (auto running = ship.running.begin(); running != ship.running.end();) {
            const particles::EffectFrameStats& advanced = batch_stats_[next++];
            particles_now += advanced.particles;
            // A drain that has no particle left, or has drained for the bound, is released.
            const bool finished = running->draining && advanced.finished;
            if (running->draining && (finished || samples_ - running->drain_from >= engine_drain_limit_samples)) {
                static_cast<void>(registry_->release(running->handle));
                if (running->engine) ++(finished ? engine_drains_finished_ : engine_drains_cut_short_);
                if (running->ion_stun) ++(finished ? ion_stun_drains_finished_ : ion_stun_drains_cut_short_);
                running = ship.running.erase(running);
                continue;
            }
            ++running;
        }
    }
    max_particles_ = std::max(max_particles_, particles_now);
    particles_ = particles_now;
    return true;
}

bool UnitEmitters::present_running() {
    // #638: one batch on the particle workers, as advance_sample.
    batch_handles_.clear();
    for (const Ship& ship : ships_) {
        for (const Running& running : ship.running) batch_handles_.push_back(running.handle);
    }
    if (auto presented = registry_->present_all(batch_handles_, camera_frame_); !presented) {
        failure_ = "unit emitter: " + core::format_diagnostic(presented.error());
        return false;
    }
    std::uint64_t effects = 0;
    for (const Ship& ship : ships_) {
        for (const Running& running : ship.running) {
            ++effects;
            if (running.log < start_log_.size()) ++start_log_[running.log].presented;
        }
    }
    presented_effects_ += effects;
    if (effects != 0) ++presented_frames_;
    return true;
}

bool UnitEmitters::frame(const SpacePopulation& population, const std::span<const platform::LiveTickEvents> reached,
                         const SnapshotAt& snapshot_at, const sim::tactical::PlayerId viewer,
                         const sim::tactical::TacticalSnapshot& previous, const sim::tactical::TacticalSnapshot& latest,
                         const ClonePoseAt& clone_pose_at, const ProjectilePoseAt& projectile_pose_at,
                         const FixedCamera& camera, const double presented_tick, const bool reveal,
                         const FadeOpacity& fade_opacity) {
    if (released_) return true;
    const auto instance_of = [](const sim::tactical::TacticalSnapshot& snapshot, const sim::EntityId entity)
        -> const sim::tactical::TacticalInstance* {
        const auto instances = snapshot.instances();
        const auto found = std::lower_bound(instances.begin(), instances.end(), entity,
            [](const sim::tactical::TacticalInstance& instance, const sim::EntityId id) { return instance.entity_id < id; });
        return found != instances.end() && found->entity_id == entity ? &*found : nullptr;
    };
    ++frames_count_;
    // #406: the clock starts at the first frame, or earlier at the birth of the oldest tick that
    // frame reaches, as the battle effects' and the breakoff props' do.
    if (!clock_start_) {
        clock_start_ = space::debris_clock_start(
            presented_tick, reached.empty() ? std::nullopt : std::optional<std::uint64_t>(reached.front().tick));
    }
    camera_frame_ = particles::camera_frame_from_render(camera.eye, camera.target, camera.up);
    const auto due = static_cast<std::uint64_t>(std::max(0.0, std::floor(presented_tick - *clock_start_ + 1.0e-9)));
    ships_.resize(std::max(ships_.size(), population.live_ship_count()));
    // #406: a frame due more than one sample (a stall) first runs the samples before its own,
    // each at its own presented tick (sample n stands where a paced frame presenting clock
    // start + n + 1 draws it): the pose and visibility the snapshots give that tick, and the
    // hardpoint and engine states of its newer snapshot, as a paced frame reads them. So a
    // hardpoint's damage emitters are born at its death and a moving ship leaves its trail. The
    // brightness is not set here: the particle renderer multiplies it in only when it builds the
    // stream to draw (as FoC's renderer does, BP-45), and only the frame's own
    // last sample is drawn.
    for (; samples_ + 1U < due; ++samples_) {
        ++caught_up_;
        const double tick = *clock_start_ + static_cast<double>(samples_) + 1.0;
        const auto base = static_cast<std::uint64_t>(std::max(0.0, std::floor(tick + 1.0e-9)));
        const auto before = snapshot_at(base);
        const auto after = snapshot_at(base + 1U);
        std::vector<space::LiveUnitPose> units;
        if (before && after) {
            const double share = std::max(0.0, tick - static_cast<double>(base));
            units = space::interpolate_units(*before, *after, share, viewer, reveal);
            // #447: a craft spinning away keeps running its model's emitters on its spin pose.
            const auto spinning = space::interpolate_spinning(*before, *after, share, viewer, reveal);
            units.insert(units.end(), spinning.begin(), spinning.end());
            std::sort(units.begin(), units.end(),
                      [](const space::LiveUnitPose& left, const space::LiveUnitPose& right) { return left.entity < right.entity; });
        } else {
            // The tick has left the snapshot history: where the ships stood is unknown, so none
            // of them runs its emitters (least visible, as a hidden ship's, BP-44).
            ++unknown_samples_;
        }
        for (std::size_t index = 0; index < ships_.size(); ++index) {
            Ship& ship = ships_[index];
            const auto view = population.live_ship_emitter_view(index);
            // #421: a death clone runs its own proxies on its clip pose at this sample's tick.
            if (view && view->death_clone && view->placement != nullptr) {
                stop_all(ship, "death_clone");
                step_clone(index, *view, clone_pose_at ? clone_pose_at(index, tick) : std::nullopt, base + 1U, samples_);
                continue;
            }
            if (!view) orphan_clone(ship);
            if (!view || view->death_clone || view->placement == nullptr) {
                stop_all(ship, !view ? "retired" : view->death_clone ? "death_clone" : "not_shown");
                continue;
            }
            // #456: a model projectile's proxies (the missile's trail) ride its flight at this
            // sample's tick; a slot whose projectile was not in flight then runs nothing.
            if (view->projectile) {
                const auto pose = projectile_pose_at ? projectile_pose_at(index, tick) : std::nullopt;
                if (!pose) {
                    stop_all(ship, "not_shown");
                    continue;
                }
                update(ship, *view, view->states, Modes{}, base + 1U, samples_);
                place(index, *pose);
                continue;
            }
            if (!before || !after) {
                stop_all(ship, "pose_unknown");
                continue;
            }
            const auto unit = std::lower_bound(units.begin(), units.end(), view->entity,
                [](const space::LiveUnitPose& pose, const sim::EntityId id) { return pose.entity < id; });
            if (unit == units.end() || unit->entity != view->entity) {
                stop_all(ship, "not_shown");
                continue;
            }
            // The model transform SpacePopulation::pose_live gives the same pose.
            std::array<sim::math::Fixed, 6> values{};
            const std::array<double, 6> source{unit->position[0], unit->position[1], unit->position[2],
                                               unit->yaw_degrees, unit->roll_degrees, unit->pitch_degrees};
            bool finite = true;
            for (std::size_t axis = 0; axis < values.size(); ++axis) {
                auto fixed = scene::fixed_from_binary32(static_cast<float>(source[axis]));
                finite = finite && static_cast<bool>(fixed);
                if (fixed) values[axis] = fixed.value();
            }
            std::optional<sim::math::Mat3x4> pose;
            if (finite) {
                if (auto placed = scene::placement_transform(values[0], values[1], values[2], values[3],
                        values[5], values[4], sim::math::Fixed::from_raw(view->placement->scale_raw))) {
                    pose = placed.value();
                }
            }
            if (!pose) {
                stop_all(ship, "frame_overflow");
                continue;
            }
            std::vector<scene::HardpointState> states(view->states.begin(), view->states.end());
            const sim::tactical::TacticalInstance* instance = unit->instance;
            const Modes modes = modes_of(instance);
            if (instance != nullptr && instance->durability) {
                const auto& hardpoints = instance->durability->hardpoints;
                for (std::size_t slot = 0; slot < hardpoints.size() && slot < states.size(); ++slot) {
                    states[slot] = static_cast<scene::HardpointState>(hardpoints[slot].state);
                }
            }
            update(ship, *view, states, modes, base + 1U, samples_);
            place(index, *pose);
        }
        if (!advance_sample()) return false;
    }
    // The frame's own sample: the poses just drawn and the latest states.
    for (std::size_t index = 0; index < ships_.size(); ++index) {
        Ship& ship = ships_[index];
        const auto view = population.live_ship_emitter_view(index);
        // #421: a death clone runs its own proxies on its clip pose, once per sample due.
        if (view && view->death_clone && view->placement != nullptr) {
            stop_all(ship, "death_clone");
            if (samples_ < due) {
                step_clone(index, *view, clone_pose_at ? clone_pose_at(index, presented_tick) : std::nullopt,
                           latest.completed_tick(), samples_);
            }
            continue;
        }
        if (!view) orphan_clone(ship);
        // BP-44: a ship that is not shown this frame (fogged, or gone from the session) runs
        // nothing; its emitters stop at once.
        if (!view || view->death_clone || !view->model_to_world || view->placement == nullptr) {
            stop_all(ship, !view ? "retired" : view->death_clone ? "death_clone" : "not_shown");
            continue;
        }
        const sim::tactical::TacticalInstance* now = instance_of(latest, view->entity);
        const Modes modes = modes_of(now);
        const bool engines_online = modes.engines_online;
        update(ship, *view, view->states, modes, latest.completed_tick(), samples_);
        // BP-45: the brightness FoC gives the engine emitters this tick.
        const sim::tactical::TacticalInstance* before = instance_of(previous, view->entity);
        if (now != nullptr && before != nullptr && now->durability && now->durability->max_speed
            && now->durability->max_speed->raw() > 0) {
            double step = 0.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double delta = to_double(now->fixed_transform.rows[axis][3])
                    - to_double(before->fixed_transform.rows[axis][3]);
                step += delta * delta;
            }
            engine_brightness_[view->entity] = engines_online
                ? std::clamp(std::sqrt(step) * 0.8 / to_double(*now->durability->max_speed) + 0.2, 0.0, 1.0)
                : 0.0;
            ship.engine_brightness = static_cast<float>(engine_brightness_[view->entity]);
        }
        // BP-40: every running emitter stands at its bone on the ship's drawn pose for the
        // samples this frame is due; world-space particles it already emitted stay where they
        // were born.
        place(index, *view->model_to_world);
        // BP-45: FoC multiplies each engine particle's colour by the brightness as it renders it.
        for (auto running = ship.running.begin(); running != ship.running.end();) {
            // #535: the engine glow eases in and out with its ship (FW-16), FoC's brightness scale
            // unchanged otherwise.
            const float fade = fade_opacity ? fade_opacity(view->entity) : 1.0F;
            if (!running->engine || registry_->set_brightness(running->handle, ship.engine_brightness * fade)) {
                ++running;
                continue;
            }
            static_cast<void>(registry_->release(running->handle));
            ++stopped_["frame_overflow"];
            running = ship.running.erase(running);
        }
    }
    std::uint64_t running_count = 0;
    for (const Ship& ship : ships_) running_count += ship.running.size();
    max_running_ = std::max(max_running_, running_count);
    // #433: a frame drawn between samples still draws each emitter where the model stands now:
    // its particles that live in the emitter's frame follow it (FoC's linked particles,
    // BP-40, placed by the emitter's transform as it renders) and its stream is
    // built again for this frame's camera and brightness, without advancing the clock.
    if (samples_ >= due && !present_running()) return false;
    for (; samples_ < due; ++samples_) {
        if (!advance_sample()) return false;
    }
    // What this frame draws: each new emitter's age now is the one it is first seen at.
    for (const Ship& ship : ships_) {
        for (const Running& running : ship.running) {
            if (running.log < start_log_.size() && !start_log_[running.log].first_age) {
                start_log_[running.log].first_age = samples_ - std::min(samples_, running.born);
            }
        }
    }
    return true;
}

void UnitEmitters::release() {
    if (released_) return;
    released_ = true;
    for (Ship& ship : ships_) {
        for (const Running& running : ship.running) static_cast<void>(registry_->release(running.handle));
        ship.running.clear();
        orphan_clone(ship);
    }
    for (CloneProxy& proxy : clone_orphans_) {
        if (proxy.life) static_cast<void>(proxy.life->release_all());
    }
    clone_orphans_.clear();
}

void UnitEmitters::write_report(std::ostream& output) const {
    std::uint64_t running = 0;
    for (const Ship& ship : ships_) running += ship.running.size();
    output << "  \"unit_emitters\": {\"frames\": " << frames_count_ << ", \"samples\": " << samples_
           << ", \"ships\": " << ships_.size() << ", \"plans\": " << plans_ << ", \"running\": " << running
           << ", \"max_running\": " << max_running_ << ", \"max_particles\": " << max_particles_;
    counts(output, "started", started_);
    counts(output, "stopped", stopped_);
    counts(output, "not_admitted", not_admitted_);
    counts(output, "start_failed", start_failed_);
    output << ", \"engine_drains\": {\"started\": " << engine_drains_started_ << ", \"finished\": "
           << engine_drains_finished_ << ", \"cut_short\": " << engine_drains_cut_short_ << "}";
    output << ", \"ion_stun_drains\": {\"started\": " << ion_stun_drains_started_ << ", \"finished\": "
           << ion_stun_drains_finished_ << ", \"cut_short\": " << ion_stun_drains_cut_short_ << "}";
    output << ", \"caught_up\": " << caught_up_ << ", \"unknown_samples\": " << unknown_samples_
           << ", \"presented_frames\": " << presented_frames_ << ", \"presented_effects\": " << presented_effects_;
    output << ", \"start_log\": [";
    for (std::size_t index = 0; index < start_log_.size(); ++index) {
        const StartRow& row = start_log_[index];
        output << (index ? ", " : "") << "{\"unit\": " << row.entity << ", \"proxy\": " << json(row.proxy)
               << ", \"tick\": " << row.tick << ", \"born\": " << row.born << ", \"origin\": ";
        if (row.origin) {
            output << "[" << (*row.origin)[0] << ", " << (*row.origin)[1] << ", " << (*row.origin)[2] << "]";
        } else {
            output << "null";
        }
        output << ", \"first_age\": " << (row.first_age ? std::to_string(*row.first_age) : std::string("null"))
               << ", \"presented\": " << row.presented << "}";
    }
    output << "], \"start_log_full\": " << (start_log_.size() >= start_log_limit ? "true" : "false");
    output << ", \"engine_brightness\": {";
    bool first = true;
    for (const auto& [entity, brightness] : engine_brightness_) {
        output << (first ? "" : ", ") << "\"" << entity << "\": " << brightness;
        first = false;
    }
    output << "}";
    // #421: the death clones' own proxies.
    std::uint64_t clone_live = 0;
    for (const Ship& ship : ships_) {
        for (const CloneProxy& proxy : ship.clone) {
            if (proxy.life) clone_live += proxy.life->draining().size() + (proxy.life->active() ? 1U : 0U);
        }
    }
    for (const CloneProxy& proxy : clone_orphans_) {
        if (proxy.life) clone_live += proxy.life->draining().size() + (proxy.life->active() ? 1U : 0U);
    }
    output << ", \"death_clones\": {\"ships\": " << clone_ships_ << ", \"live\": " << clone_live
           << ", \"max_live\": " << clone_max_live_ << ", \"orphans\": " << clone_orphans_.size()
           << ", \"max_particles\": " << clone_max_particles_ << ", \"drains_released\": " << clone_drains_released_
           << ", \"drains_cut_short\": " << clone_drains_cut_short_ << ", \"drains_reset\": " << clone_drains_reset_
           << ", \"reappearance\": " << json(particles::to_string(clone_reappearance));
    counts(output, "started", clone_started_);
    counts(output, "hidden", clone_hidden_);
    counts(output, "not_run", clone_not_run_);
    counts(output, "failed", clone_failed_);
    output << ", \"start_log\": [";
    for (std::size_t index = 0; index < clone_log_.size(); ++index) {
        const CloneStartRow& row = clone_log_[index];
        output << (index ? ", " : "") << "{\"unit\": " << row.entity << ", \"proxy\": " << json(row.proxy)
               << ", \"tick\": " << row.tick << ", \"born\": " << row.born << "}";
    }
    output << "], \"start_log_full\": " << (clone_log_.size() >= start_log_limit ? "true" : "false") << "}";
    output << ", \"failure\": " << (failure_.empty() ? std::string("null") : json(failure_)) << "},\n";
}

} // namespace eawr::presentation::godot_backend
