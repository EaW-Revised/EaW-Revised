#include "unit_emitters.hpp"
#include "presentation_constants.hpp"
#include "frame_timer.hpp"

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
#include <utility>

#include "unit_emitters_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace unit_emitters_detail;

namespace {
// BP-65: a hidden engine emitter drains for at most this many clock samples (10 s) before it is cut short.
constexpr std::uint64_t engine_drain_limit_samples = 300;

// The emitter modes a unit's snapshot instance sets (BP-42, BP-43, space-abilities AB-31, AB-32).
[[nodiscard]] UnitEmitters::Modes modes_of(const sim::tactical::TacticalInstance* instance,
                                           const std::span<const tactical::TypeId> ion_stun_types) {
    UnitEmitters::Modes modes;
    if (instance == nullptr) return modes;
    modes.engines_online = !instance->durability || instance->durability->engines_online;
    modes.ion_stun_effect = std::binary_search(ion_stun_types.begin(), ion_stun_types.end(), instance->type_id);
    // IS-09: the stun shows its emitters from the hit until its end frame (IS-03).
    modes.ion_stunned = instance->ion_stun_frames > 0;
    for (const auto& ability : instance->abilities) {
        if (ability.kind == sim::tactical::AbilityKind::blast) {
            modes.blast_charging = ability.blast_charge_remaining != 0;
            modes.blast_charged = !modes.blast_charging && ability.blast_charged_presentation;
        }
        if (!ability.active) continue;
        if (ability.kind == sim::tactical::AbilityKind::turbo || ability.kind == sim::tactical::AbilityKind::spoiler_lock) {
            modes.turbo = true;
        }
        // WAB-80: FULL_SALVO shows the same power-to-weapons emitter type as POWER_TO_WEAPONS.
        if (ability.kind == sim::tactical::AbilityKind::power_to_weapons
            || ability.kind == sim::tactical::AbilityKind::full_salvo) {
            modes.power_to_weapons = true;
        }
        if (ability.kind == sim::tactical::AbilityKind::invulnerability) modes.invulnerability = true;
        if (ability.kind == sim::tactical::AbilityKind::interdict) modes.gravity_well = true;
        if (ability.kind == sim::tactical::AbilityKind::missile_shield) modes.missile_shield = true;
    }
    return modes;
}

[[nodiscard]] double to_double(const sim::math::Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
}
} // namespace

void UnitEmitters::stop_all(Ship& ship, const char* reason) {
    ship.reconciled = false;
    for (const Running& running : ship.running) static_cast<void>(registry_->release(running.handle));
    if (!ship.running.empty()) stopped_[reason] += ship.running.size();
    ship.running.clear();
}

void UnitEmitters::update(Ship& ship, const SpacePopulation::LiveShipEmitterView& view,
                          const std::span<const scene::HardpointState> states, const Modes modes,
                          const std::uint64_t tick, const std::uint64_t born) {
    if (ship.planned && ship.entity != view.entity) {
        stop_all(ship, "rebound");
        ship.planned = false;
        ship.engine_brightness = 1.0F;
    }
    // BP-65: only the turbo swap's hidden engine emitters drain (the recording shows that one).
    const bool turbo_swap = ship.planned && ship.modes.turbo != modes.turbo;
    // IS-09: the stun's end hides its emitters as the swap hides the engines' (BP-65): their
    // residual particles drain.
    const bool stun_ended = ship.planned && ship.modes.ion_stunned && !modes.ion_stunned;
    // AB-32, BP-48: hiding the glow at switch-off or expiration drains its existing particles.
    const bool power_ended = ship.planned && ship.modes.power_to_weapons && !modes.power_to_weapons;
    const bool blast_changed = ship.planned && (ship.modes.blast_charging != modes.blast_charging
        || ship.modes.blast_charged != modes.blast_charged);
    if (!ship.planned || ship.modes != modes
        || !std::equal(ship.states.begin(), ship.states.end(), states.begin(), states.end())) {
        plan(ship, view, states, modes);
        ship.reconciled = false;
    }
    // A reconciled ship (its running emitters already match its plan) changes only when it replans or
    // loses a running emitter, which clear the flag: skip walking the large wanted/running records.
    if (ship.reconciled) return;
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
        if (((turbo_swap && running->engine) || (stun_ended && running->ion_stun)
             || (power_ended && running->power_to_weapons)
             || (!modes.gravity_well && running->gravity_well)
             || (!modes.missile_shield && running->missile_shield)
             || (blast_changed && running->blast_effect)
             || (ship.planned && !modes.invulnerability && running->invulnerability))
            && registry_->stop_emission(running->handle)) {
            running->draining = true;
            running->drain_from = samples_;
            if (running->engine) ++engine_drains_started_;
            if (running->ion_stun) ++ion_stun_drains_started_;
            if (running->power_to_weapons) ++power_to_weapons_drains_started_;
            if (running->invulnerability) ++invulnerability_drains_started_;
            if (running->gravity_well) ++deployment_drains_started_["gravity_well"];
            if (running->missile_shield) ++deployment_drains_started_["missile_shield"];
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
        auto definition = *system.system;
        // BP-70/PS-28: object scale transforms emission positions; billboard
        // size stays authored and has its separate dynamic-size multiplier.
        auto handle = wanted.mesh ? registry_->spawn(std::move(definition), wanted.seed, wanted.capacity, *wanted.mesh)
                                  : registry_->spawn(std::move(definition), wanted.seed, wanted.capacity);
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
            {wanted.proxy, handle.value(), wanted.local, wanted.mesh_local, wanted.engine, wanted.ion_stun,
             wanted.power_to_weapons, born, wanted.invulnerability, log});
        ship.running.back().gravity_well = wanted.gravity_well;
        ship.running.back().missile_shield = wanted.missile_shield;
        ship.running.back().blast_effect = wanted.blast_effect;
        ship.running.back().elapsed_clock = view.projectile;
        ++started_[wanted.proxy_name];
    }
    ship.reconciled = true;
}

void UnitEmitters::place(const std::size_t index, const sim::math::Mat3x4& pose) {
    Ship& ship = ships_[index];
    for (auto running = ship.running.begin(); running != ship.running.end();) {
        FrameTimer timer(measure_preparation_ ? &emitter_prepare_ms_ : nullptr, true);
        std::optional<particles::EmitterFrame> frame;
        std::optional<particles::MeshFrame> mesh;
        if (auto world = sim::math::compose(pose, running->local)) frame = particles::source_emitter_frame(world.value());
        if (running->mesh_local) {
            if (auto world = sim::math::compose(pose, *running->mesh_local)) {
                const particles::EmitterFrame owner = particles::source_emitter_frame(world.value());
                mesh = particles::MeshFrame{owner.origin, owner.basis};
            }
        }
        timer.finish();
        const bool placed = frame && (!running->mesh_local || mesh) && registry_->set_frame(running->handle, *frame)
            && (!mesh || registry_->set_mesh_frame(running->handle, *mesh));
        if (!placed) {
            ship.reconciled = false;
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

bool UnitEmitters::step_clone_proxy(CloneProxy& proxy, const bool visible, const particles::EmitterFrame& frame,
                                    const particles::MeshFrame* mesh, const sim::EntityId entity,
                                    const std::uint64_t tick, const std::uint64_t born) {
    auto step = proxy.life->prepare_step(visible, frame, mesh, sample_seconds_);
    if (!step) {
        // A clone emitter that fails is reported and let go; the battle view goes on.
        ++clone_failed_[proxy.name + ": " + core::format_diagnostic(step.error())];
        static_cast<void>(proxy.life->release_all());
        proxy.life.reset();
        return false;
    }
    proxy.last = frame;
    if (mesh != nullptr) proxy.last_mesh = *mesh;
    if (step.value().result.spawned) {
        ++clone_started_[proxy.name];
        if (clone_log_.size() < start_log_limit) clone_log_.push_back({entity, proxy.name, tick, born});
    }
    if (step.value().result.detached) ++clone_hidden_[proxy.name];
    clone_drains_cut_short_ += step.value().result.drains_cut_short;
    clone_drains_reset_ += step.value().result.drains_reset;
    proxy.pending = std::move(step.value());
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
        FrameTimer timer(measure_preparation_ ? &clone_prepare_ms_ : nullptr, true);
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
        timer.finish();
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
    ship.clone_planned = false;
}

bool UnitEmitters::retire_clone(const std::size_t index, const std::span<const animation::BonePose> final_bones) {
    if (index >= ships_.size()) return true;
    Ship& ship = ships_[index];
    // A slot that never reached a sample cannot discard a future proxy birth.
    if (!ship.clone_planned) return false;
    for (const auto& proxy : ship.clone) {
        if (!proxy.life) continue;
        if (proxy.bone >= final_bones.size()) return false;
        // Hidden-bone effects keep the existing last-frame orphan/drain path;
        // only visible-bone effects must be exhausted to release their owner.
        if (!final_bones[proxy.bone].visible) continue;
        // A visible proxy can keep a permanently hidden wreck alive after its
        // finite system has ended. Empty right now is insufficient: delayed
        // emission and child chains must be exhausted too. An attachment that
        // has not spawned yet still needs its owner for the next sample.
        const auto active = proxy.life->active();
        if (!active || !registry_->exhausted(*active)) return false;
        for (const auto drain : proxy.life->draining()) {
            if (!registry_->exhausted(drain)) return false;
        }
    }
    // Selection above is read-only. Release only once every proxy agrees, so
    // a still-burning sibling leaves the entire clone unchanged.
    for (auto& proxy : ship.clone) {
        if (!proxy.life || !final_bones[proxy.bone].visible) continue;
        static_cast<void>(proxy.life->release_all());
        proxy.life.reset();
    }
    orphan_clone(ship);
    return true;
}

void UnitEmitters::rebuild_busy() {
    busy_.clear();
    for (std::size_t index = 0; index < ships_.size(); ++index) {
        if (!ships_[index].running.empty() || !ships_[index].clone.empty()) busy_.push_back(index);
    }
}

bool UnitEmitters::advance_sample() {
    // #421: a clone's proxies that outlived it stop emitting and drain where they last stood.
    for (CloneProxy& proxy : clone_orphans_) {
        static_cast<void>(step_clone_proxy(proxy, false, *proxy.last, proxy.last_mesh ? &*proxy.last_mesh : nullptr,
                                           sim::EntityId{}, 0U, samples_));
    }
    // Every lifecycle keeps its original visibility/spawn order. Only the CPU advance and
    // stream build run on workers; backend uploads and drain releases stay in owner order.
    batch_handles_.clear();
    batch_deltas_.clear();
    const auto collect = [&](const CloneProxy& proxy) {
        if (!proxy.pending) return;
        const auto& prepared = *proxy.pending;
        batch_handles_.insert(batch_handles_.end(), prepared.handles.begin(), prepared.handles.end());
        batch_deltas_.insert(batch_deltas_.end(), prepared.deltas.begin(), prepared.deltas.end());
    };
    for (const std::size_t slot : busy_) {
        const Ship& ship = ships_[slot];
        for (const CloneProxy& proxy : ship.clone) collect(proxy);
    }
    for (const CloneProxy& proxy : clone_orphans_) collect(proxy);
    if (auto advanced = registry_->advance_all(batch_handles_, batch_deltas_, camera_frame_, batch_stats_); !advanced) {
        failure_ = "clone emitter: " + core::format_diagnostic(advanced.error());
        return false;
    }
    std::size_t clone_next = 0;
    const auto complete = [&](CloneProxy& proxy) {
        if (!proxy.pending) return;
        const std::size_t count = proxy.pending->handles.size();
        const auto stats = std::span<const particles::EffectFrameStats>(batch_stats_).subspan(clone_next, count);
        clone_next += count;
        auto prepared = std::move(*proxy.pending);
        proxy.pending.reset();
        auto step = proxy.life->complete_step(std::move(prepared), stats);
        if (!step) {
            ++clone_failed_[proxy.name + ": " + core::format_diagnostic(step.error())];
            static_cast<void>(proxy.life->release_all());
            proxy.life.reset();
            return;
        }
        clone_drains_released_ += step.value().drains_released;
        clone_particles_now_ += step.value().stats.particles;
        clone_dropped_at_capacity_ += step.value().stats.advance.dropped_at_capacity;
    };
    for (const std::size_t slot : busy_) {
        Ship& ship = ships_[slot];
        for (CloneProxy& proxy : ship.clone) complete(proxy);
        std::erase_if(ship.clone, [](const CloneProxy& proxy) { return !proxy.life; });
    }
    for (CloneProxy& proxy : clone_orphans_) complete(proxy);
    std::erase_if(clone_orphans_, [](const CloneProxy& proxy) {
        return !proxy.life || (!proxy.life->active() && proxy.life->draining().empty());
    });
    std::uint64_t clone_live = 0;
    for (const std::size_t slot : busy_) {
        const Ship& ship = ships_[slot];
        for (const CloneProxy& proxy : ship.clone) {
            if (proxy.life) clone_live += proxy.life->draining().size() + (proxy.life->active() ? 1U : 0U);
        }
    }
    for (const CloneProxy& proxy : clone_orphans_) clone_live += proxy.life->draining().size() + (proxy.life->active() ? 1U : 0U);
    clone_max_live_ = std::max(clone_max_live_, clone_live);
    clone_max_particles_ = std::max(clone_max_particles_, clone_particles_now_);
    clone_particles_now_ = 0;
    return advance_running(sample_seconds_, false);
}

bool UnitEmitters::advance_running(const float delta_seconds, const bool projectiles) {
    std::uint64_t particles_now = projectiles ? sampled_particles_ : 0;
    // #638: every running emitter steps in one batch on the particle workers; the results are
    // then taken in the running order, as one advance after another gave them.
    batch_handles_.clear();
    batch_deltas_.clear();
    for (const std::size_t slot : busy_) {
        const Ship& ship = ships_[slot];
        for (const Running& running : ship.running) {
            if (running.elapsed_clock != projectiles) continue;
            batch_handles_.push_back(running.handle);
            // A surviving state discovered inside a folded interval starts at its
            // own event, rather than inheriting the whole frame's elapsed time.
            const float age = running.catchup_seconds;
            batch_deltas_.push_back(std::max(0.0F, delta_seconds + age));
        }
    }
    if (auto advanced = registry_->advance_all(batch_handles_, batch_deltas_, camera_frame_, batch_stats_); !advanced) {
        failure_ = "unit emitter: " + core::format_diagnostic(advanced.error());
        return false;
    }
    std::size_t next = 0;
    for (const std::size_t slot : busy_) {
        Ship& ship = ships_[slot];
        for (auto running = ship.running.begin(); running != ship.running.end();) {
            if (running->elapsed_clock != projectiles) { ++running; continue; }
            running->catchup_seconds = 0.0F;
            const particles::EffectFrameStats& advanced = batch_stats_[next++];
            if (collect_axis_diagnostics_ && running->log < start_log_.size()) {
                auto& measured = model_axis_[start_log_[running->log].proxy];
                for (const auto& emitter : advanced.emitters) {
                    measured.samples += emitter.axis_samples;
                    measured.reversed += emitter.axis_reversed;
                    measured.max_sine_error = std::max(measured.max_sine_error, emitter.axis_max_sine_error);
                }
            }
            particles_now += advanced.particles;
            if (running->log < start_log_.size()) {
                std::uint64_t quads = 0;
                for (const auto& emitter : advanced.emitters) {
                    if (emitter.drawn && emitter.maximum_alpha > 0.0F) quads += emitter.quads;
                }
                auto& row = start_log_[running->log];
                row.max_visible_quads = std::max(row.max_visible_quads, quads);
            }
            if (running->ion_stun) {
                ion_stun_max_particles_ = std::max(ion_stun_max_particles_,
                    static_cast<std::uint64_t>(advanced.particles));
                ion_stun_dropped_at_capacity_ += advanced.advance.dropped_at_capacity;
            }
            // A drain that has no particle left, or has drained for the bound, is released.
            const bool finished = running->draining && advanced.finished;
            if (running->draining && (finished || samples_ - running->drain_from >= engine_drain_limit_samples)) {
                ship.reconciled = false;
                static_cast<void>(registry_->release(running->handle));
                if (running->engine) ++(finished ? engine_drains_finished_ : engine_drains_cut_short_);
                if (running->ion_stun) ++(finished ? ion_stun_drains_finished_ : ion_stun_drains_cut_short_);
                if (running->power_to_weapons) {
                    ++(finished ? power_to_weapons_drains_finished_ : power_to_weapons_drains_cut_short_);
                }
                if (running->invulnerability)
                    ++(finished ? invulnerability_drains_finished_ : invulnerability_drains_cut_short_);
                running = ship.running.erase(running);
                continue;
            }
            ++running;
        }
    }
    if (!projectiles) sampled_particles_ = particles_now;
    max_particles_ = std::max(max_particles_, particles_now);
    particles_ = particles_now;
    return true;
}

bool UnitEmitters::advance_projectiles(const double presented_tick) {
    // PS-42: both linked and world particles consume the elapsed time of the
    // pose just supplied. Following only the linked particles between integer
    // samples separates a moving ring from its inherited-motion kite.
    if (!projectile_clock_tick_) projectile_clock_tick_ = clock_start_;
    const double elapsed = std::max(0.0, presented_tick - *projectile_clock_tick_);
    if (elapsed == 0.0) {
        batch_handles_.clear();
        for (const std::size_t slot : busy_) {
            const Ship& ship = ships_[slot];
            for (const Running& running : ship.running) {
                if (running.elapsed_clock) batch_handles_.push_back(running.handle);
            }
        }
        if (auto presented = registry_->present_all(batch_handles_, camera_frame_); !presented) {
            failure_ = "projectile emitter: " + core::format_diagnostic(presented.error());
            return false;
        }
        return true;
    }
    if (!advance_running(static_cast<float>(elapsed * presentation_constants::logical_frame_seconds), true)) return false;
    projectile_clock_tick_ = presented_tick;
    return true;
}

bool UnitEmitters::present_running() {
    // #638: one batch on the particle workers, as advance_sample.
    batch_handles_.clear();
    for (const std::size_t slot : busy_) {
        const Ship& ship = ships_[slot];
        for (const Running& running : ship.running) {
            if (!running.elapsed_clock) batch_handles_.push_back(running.handle);
        }
    }
    if (auto presented = registry_->present_all(batch_handles_, camera_frame_); !presented) {
        failure_ = "unit emitter: " + core::format_diagnostic(presented.error());
        return false;
    }
    std::uint64_t effects = 0;
    for (const std::size_t slot : busy_) {
        const Ship& ship = ships_[slot];
        for (const Running& running : ship.running) {
            if (running.elapsed_clock) continue;
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
                         const FadeOpacity& fade_opacity, const std::span<const sim::EntityId> unfogged_props,
                         const ProjectileOpacity& projectile_opacity, const BirthAt& clone_birth_at,
                         const ProjectileBirthAt& projectile_birth_at) {
    if (released_) return true;
    FrameTimer owner_setup(measure_owner_ ? &owner_timings_[0] : nullptr);
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
    camera_frame_.collect_axis_diagnostics = collect_axis_diagnostics_;
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
    // FW-19: retain already running emitters while their host eases out of raw visibility.
    // A newly revealed host with no running stream still starts at its first visible sample.
    owner_setup.finish();
    FrameTimer owner_fading(measure_owner_ ? &owner_timings_[1] : nullptr);
    std::vector<sim::EntityId> fading_emitters;
    // Only the catch-up samples below read the list, and most frames run none: scan only for them.
    if (samples_ + 1U < due) {
        // FW-24: historical samples retain unfogged map owners too, including
        // owners whose streams have not started yet on the first catch-up frame.
        if (!reveal) fading_emitters.assign(unfogged_props.begin(), unfogged_props.end());
        if (!reveal && fade_opacity) {
            for (std::size_t index = 0; index < ships_.size(); ++index) {
                const auto view = population.live_ship_emitter_view(index);
                if (view && !view->projectile && view->model_to_world && !ships_[index].running.empty()
                    && fade_opacity(view->entity) < 1.0F) fading_emitters.push_back(view->entity);
            }
            std::sort(fading_emitters.begin(), fading_emitters.end());
            fading_emitters.erase(std::unique(fading_emitters.begin(), fading_emitters.end()), fading_emitters.end());
        }
    }
    owner_fading.finish();
    FrameTimer owner_catchup(measure_owner_ ? &owner_timings_[2] : nullptr);
    sample_seconds_ = presentation_constants::logical_frame_seconds;
    const bool folded = particles::EffectRegistry::catchup_folding_default() && due > samples_ && due - samples_ > 2;
    // No deferred scope or registry scan on the ordinary one/two-sample path.
    const auto frame_uploads = registry_->defer_uploads(folded);
    const auto folded_start = samples_;
    const auto historical_end = folded ? samples_ + 1U : due;
    for (; samples_ + 1U < due && samples_ < historical_end; ++samples_) {
        ++caught_up_;
        const double tick = *clock_start_ + static_cast<double>(samples_) + 1.0;
        const auto base = static_cast<std::uint64_t>(std::max(0.0, std::floor(tick + 1.0e-9)));
        const auto before = snapshot_at(base);
        const auto after = snapshot_at(base + 1U);
        std::vector<live::LiveUnitPose> units;
        if (before && after) {
            const double share = std::max(0.0, tick - static_cast<double>(base));
            units = live::interpolate_units(*before, *after, share, viewer, reveal, fading_emitters);
            // #447: a craft spinning away keeps running its model's emitters on its spin pose.
            const auto spinning = live::interpolate_spinning(*before, *after, share, viewer, reveal);
            units.insert(units.end(), spinning.begin(), spinning.end());
            std::sort(units.begin(), units.end(),
                      [](const live::LiveUnitPose& left, const live::LiveUnitPose& right) { return left.entity < right.entity; });
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
                if (!folded) {
                    step_clone(index, *view, clone_pose_at ? clone_pose_at(index, tick) : std::nullopt, base + 1U, samples_);
                } else {
                    // Advance only previously visible generations at their retained frame.
                    // An undrawn historical pose must not admit a transient generation.
                    for (auto& proxy : ship.clone) if (proxy.life && proxy.last)
                        static_cast<void>(step_clone_proxy(proxy, proxy.life->active().has_value(), *proxy.last,
                            proxy.last_mesh ? &*proxy.last_mesh : nullptr, view->entity, base + 1U, samples_));
                }
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
                auto pose = projectile_pose_at ? projectile_pose_at(index, tick) : std::nullopt;
                if (!pose) {
                    stop_all(ship, "not_shown");
                    continue;
                }
                if (folded && projectile_pose_at && projectile_birth_at) {
                    const auto current = projectile_birth_at(index);
                    if (!current || current->second != ship.projectile_generation) {
                        // A previously drawn projectile retired or rebound in this gap.
                        // Locate its terminal pose with logarithmic history queries;
                        // advancing every missed pose is unnecessary for a retired stream.
                        auto low = samples_;
                        auto high = due - 2U;
                        while (low < high) {
                            const auto middle = low + (high - low + 1U) / 2U;
                            auto last = projectile_pose_at(index, *clock_start_ + static_cast<double>(middle) + 1.0);
                            if (last) { low = middle; pose = std::move(last); }
                            else high = middle - 1U;
                        }
                    }
                }
                if (!folded) update(ship, *view, view->states, Modes{}, base + 1U, samples_);
                place(index, *pose);
                continue;
            }
            if (!before || !after) {
                stop_all(ship, "pose_unknown");
                continue;
            }
            const auto unit = std::lower_bound(units.begin(), units.end(), view->entity,
                [](const live::LiveUnitPose& pose, const sim::EntityId id) { return pose.entity < id; });
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
            const Modes modes = modes_of(instance, ion_stun_types_);
            if (instance != nullptr && instance->durability) {
                const auto& hardpoints = instance->durability->hardpoints;
                for (std::size_t slot = 0; slot < hardpoints.size() && slot < states.size(); ++slot) {
                    states[slot] = static_cast<scene::HardpointState>(hardpoints[slot].state);
                }
            }
            if (!folded) update(ship, *view, states, modes, base + 1U, samples_);
            place(index, *pose);
        }
        rebuild_busy();
        if (!advance_projectiles(tick) || !advance_sample()) return false;
    }
    owner_catchup.finish();
    // At most one historical pose plus the drawn pose. Only state transitions
    // for newly visible streams are inspected inside the skipped interval.
    sample_seconds_ = static_cast<float>(due > samples_ ? due - samples_ : 1U)
        * presentation_constants::logical_frame_seconds;
    // The frame's own sample: the poses just drawn and the latest states.
    // A pooled slot that runs nothing and is neither posed nor a death clone has nothing to do here
    // (the walk below would only stop what it does not run): skip it.
    visit_.clear();
    for (std::size_t index = 0; index < ships_.size(); ++index) {
        if (ships_[index].running.empty() && ships_[index].clone.empty()
            && !population.live_ship_emitter_visit(index)) continue;
        visit_.push_back(index);
        FrameTimer owner_update(measure_owner_ ? &owner_timings_[3] : nullptr, true);
        Ship& ship = ships_[index];
        const auto view = population.live_ship_emitter_view(index);
        // #421: a death clone runs its own proxies on its clip pose, once per sample due.
        if (view && view->death_clone && view->placement != nullptr) {
            stop_all(ship, "death_clone");
            if (samples_ < due) {
                const auto pose = clone_pose_at ? clone_pose_at(index, presented_tick) : std::nullopt;
                step_clone(index, *view, pose, latest.completed_tick(), samples_);
                if (folded && pose && clone_pose_at) {
                    std::map<std::size_t, std::uint64_t> births;
                    for (std::size_t proxy = 0; proxy < ship.clone.size(); ++proxy)
                        if (ship.clone[proxy].pending && ship.clone[proxy].pending->result.spawned)
                            births[proxy] = folded_start;
                    const auto birth_tick = clone_birth_at ? clone_birth_at(index) : std::nullopt;
                    const auto earliest = birth_tick ? static_cast<std::uint64_t>(std::max(0.0,
                        std::ceil(*birth_tick - *clock_start_ - 1.0 - 1.0e-9))) : folded_start;
                    for (auto& entry : births) entry.second = std::max(folded_start, earliest);
                    // Only newly seen proxies need birth reconstruction, once per clone.
                    // Steady clones use the folded elapsed-time batch directly.
                    for (auto sample = std::max(folded_start, earliest); !births.empty() && sample + 1U < due; ++sample) {
                        const auto historical = clone_pose_at(index, *clock_start_ + static_cast<double>(sample) + 1.0);
                        for (auto& [proxy, born] : births) {
                            const auto bone = ship.clone[proxy].bone;
                            if (!historical || bone >= historical->bones.size() || !historical->bones[bone].visible)
                                born = sample + 1U;
                        }
                    }
                    for (const auto& [proxy, born] : births) {
                        auto& pending = *ship.clone[proxy].pending;
                        for (std::size_t effect = 0; effect < pending.handles.size(); ++effect) {
                            if (pending.handles[effect] == *pending.result.spawned)
                                pending.deltas[effect] = static_cast<float>(due > born + 1U ? due - born - 1U : 0U)
                                    * presentation_constants::logical_frame_seconds;
                        }
                        for (auto row = clone_log_.rbegin(); row != clone_log_.rend(); ++row) {
                            if (row->entity == view->entity && row->proxy == ship.clone[proxy].name && row->born == samples_) {
                                row->born = born;
                                row->tick = static_cast<std::uint64_t>(*clock_start_ + static_cast<double>(born)) + 2U;
                                break;
                            }
                        }
                    }
                }
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
        const Modes modes = modes_of(now, ion_stun_types_);
        const bool engines_online = modes.engines_online;
        const auto projectile_birth = view->projectile && projectile_birth_at ? projectile_birth_at(index) : std::nullopt;
        if (projectile_birth) {
            if (folded && ship.projectile_generation != projectile_birth->second) stop_all(ship, "projectile_rebound");
            ship.projectile_generation = projectile_birth->second;
        }
        update(ship, *view, view->states, modes, latest.completed_tick(), samples_);
        // Folded-interval births use the endpoint pose: during stalls / raised
        // sim speed this can displace the historical origin (~100 world units
        // in the moving-ship fixture). Exact birth poses are follow-up #5308.
        if (folded && !view->projectile) {
            std::vector<Running*> fresh;
            for (auto& running : ship.running) if (running.born == samples_ && !running.draining) fresh.push_back(&running);
            if (!fresh.empty()) {
                Ship history;
                history.attachments = ship.attachments;
                history.attachments_planned = ship.attachments_planned;
                const auto snapshot_for_sample = [&](const std::uint64_t sample)
                    -> std::shared_ptr<const sim::tactical::TacticalSnapshot> {
                    const double tick = *clock_start_ + static_cast<double>(sample) + 1.0;
                    const auto base = static_cast<std::uint64_t>(std::max(0.0, std::floor(tick + 1.0e-9)));
                    // Match the ordinary historical sample: both poses must be
                    // knowable, and its state comes from the newer snapshot.
                    if (!snapshot_at(base)) return nullptr;
                    return snapshot_at(base + 1U);
                };
                // The event log omits ticks without presented events. State
                // transitions therefore need the retained snapshot interval,
                // not just the sparse event records. Skip the unknown prefix
                // without walking an arbitrarily long first-frame stall.
                auto oldest = folded_start;
                auto end = due;
                while (oldest < end) {
                    const auto middle = oldest + (end - oldest) / 2U;
                    if (snapshot_for_sample(middle)) end = middle;
                    else oldest = middle + 1U;
                }
                std::map<std::size_t, std::uint64_t> births;
                for (const auto* running : fresh) births[running->proxy] = oldest;
                for (auto sample = oldest; sample < due; ++sample) {
                    const auto snapshot = snapshot_for_sample(sample);
                    const auto* instance = snapshot ? instance_of(*snapshot, view->entity) : nullptr;
                    std::vector<scene::HardpointState> states(view->states.begin(), view->states.end());
                    if (instance && instance->durability) {
                        const auto& hardpoints = instance->durability->hardpoints;
                        for (std::size_t slot = 0; slot < hardpoints.size() && slot < states.size(); ++slot)
                            states[slot] = static_cast<scene::HardpointState>(hardpoints[slot].state);
                    }
                    const auto historical_modes = modes_of(instance, ion_stun_types_);
                    if (instance && (!history.planned || history.modes != historical_modes || history.states != states))
                        plan(history, *view, states, historical_modes);
                    for (const auto* running : fresh) {
                        const bool active = instance && std::any_of(history.wanted.begin(), history.wanted.end(),
                            [&](const auto& wanted) { return wanted.proxy == running->proxy; });
                        if (!active) births[running->proxy] = sample + 1U;
                    }
                }
                for (auto* running : fresh) {
                    const auto born = std::min(due - 1U, births[running->proxy]);
                    // The final batch covers [samples_, due). Its delta is shortened
                    // for a late birth, or extended for an older unseen active state.
                    running->catchup_seconds = (static_cast<float>(born) - static_cast<float>(samples_))
                        * -presentation_constants::logical_frame_seconds;
                    running->born = born;
                    if (running->log < start_log_.size()) {
                        start_log_[running->log].born = born;
                        start_log_[running->log].tick = static_cast<std::uint64_t>(*clock_start_ + static_cast<double>(born)) + 2U;
                    }
                }
            }
        }
        if (folded && projectile_birth) {
            const double start = std::max(projectile_birth->first, *clock_start_ + static_cast<double>(folded_start));
            const float delta = static_cast<float>(std::max(0.0, presented_tick - start))
                * presentation_constants::logical_frame_seconds;
            const float frame_delta = static_cast<float>(std::max(0.0,
                presented_tick - projectile_clock_tick_.value_or(*clock_start_)))
                * presentation_constants::logical_frame_seconds;
            for (auto& running : ship.running) if (running.born == samples_ && !running.draining)
                running.catchup_seconds = delta - frame_delta;
        }
        // BP-45: the brightness FoC gives the engine emitters this tick.
        const sim::tactical::TacticalInstance* before =
            now != nullptr && now->durability && now->durability->max_speed ? instance_of(previous, view->entity) : nullptr;
        if (now != nullptr && before != nullptr && now->durability && now->durability->max_speed
            && now->durability->max_speed->raw() > 0) {
            double step = 0.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double delta = to_double(now->fixed_transform.rows[axis][3])
                    - to_double(before->fixed_transform.rows[axis][3]);
                step += delta * delta;
            }
            // The report's per-entity map keeps its node for the ship's entity: no lookup per frame.
            // The cached pointer is safe only because engine_brightness_ is never erased from
            // (std::map nodes stay put on insert).
            if (ship.brightness_slot == nullptr || ship.brightness_entity != view->entity) {
                ship.brightness_slot = &engine_brightness_[view->entity];
                ship.brightness_entity = view->entity;
            }
            *ship.brightness_slot = engines_online
                ? std::clamp(std::sqrt(step) * 0.8 / to_double(*now->durability->max_speed) + 0.2, 0.0, 1.0)
                : 0.0;
            ship.engine_brightness = static_cast<float>(*ship.brightness_slot);
        }
        // BP-40: every running emitter stands at its bone on the ship's drawn pose for the
        // samples this frame is due; world-space particles it already emitted stay where they
        // were born.
        owner_update.finish();
        FrameTimer owner_place(measure_owner_ ? &owner_timings_[4] : nullptr, true);
        place(index, *view->model_to_world);
        owner_place.finish();
        FrameTimer owner_brightness(measure_owner_ ? &owner_timings_[5] : nullptr, true);
        // BP-45/FW-19: engine brightness and shared unit opacity multiply at presentation;
        // damage, fire and other unit emitters carry the same opacity as their hull.
        // The opacity belongs to the ship, not to each of its emitters: look it up once.
        const float fade = ship.running.empty() ? 1.0F
            : view->projectile && projectile_opacity ? projectile_opacity(index)
            : fade_opacity ? fade_opacity(view->entity) : 1.0F;
        for (auto running = ship.running.begin(); running != ship.running.end();) {
            if (registry_->set_brightness(running->handle, (running->engine ? ship.engine_brightness : 1.0F) * fade)) {
                ++running;
                continue;
            }
            // A stopped emitter must be respawned: without this update() early-returns on the
            // reconciled ship until a replan.
            ship.reconciled = false;
            static_cast<void>(registry_->release(running->handle));
            ++stopped_["frame_overflow"];
            running = ship.running.erase(running);
        }
    }
    busy_.clear();
    for (const std::size_t index : visit_) {
        if (!ships_[index].running.empty() || !ships_[index].clone.empty()) busy_.push_back(index);
    }
    FrameTimer owner_registry(measure_owner_ ? &owner_timings_[6] : nullptr);
    std::uint64_t running_count = 0;
    for (const std::size_t slot : busy_) running_count += ships_[slot].running.size();
    max_running_ = std::max(max_running_, running_count);
    // #433: a frame drawn between samples still draws each emitter where the model stands now:
    // its particles that live in the emitter's frame follow it (FoC's linked particles,
    // BP-40, placed by the emitter's transform as it renders) and its stream is
    // built again for this frame's camera and brightness, without advancing the clock.
    if (samples_ >= due && !present_running()) return false;
    if (samples_ < due) {
        if (!advance_sample()) return false;
        samples_ = due;
    }
    if (!advance_projectiles(presented_tick)) return false;
    owner_registry.finish();
    FrameTimer owner_finish(measure_owner_ ? &owner_timings_[7] : nullptr);
    // What this frame draws: each new emitter's age now is the one it is first seen at.
    for (const std::size_t slot : busy_) {
        const Ship& ship = ships_[slot];
        for (const Running& running : ship.running) {
            if (running.log < start_log_.size() && !start_log_[running.log].first_age) {
                start_log_[running.log].first_age = samples_ - std::min(samples_, running.born);
            }
        }
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
