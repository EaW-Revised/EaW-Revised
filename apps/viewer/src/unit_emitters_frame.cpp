#include "unit_emitters.hpp"
#include "frame_timer.hpp"

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

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace unit_emitters_detail;

namespace {
// BP-65: a hidden engine emitter drains for at most this many clock samples (10 s) before it is cut short.
constexpr std::uint64_t engine_drain_limit_samples = 300;

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
        if (ability.kind == sim::tactical::AbilityKind::invulnerability) modes.invulnerability = true;
    }
    return modes;
}

[[nodiscard]] double to_double(const sim::math::Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
}
} // namespace

void UnitEmitters::stop_all(Ship& ship, const char* reason) {
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
        if (((turbo_swap && running->engine) || (stun_ended && running->ion_stun)
             || (power_ended && running->power_to_weapons)
             || (ship.planned && !modes.invulnerability && running->invulnerability))
            && registry_->stop_emission(running->handle)) {
            running->draining = true;
            running->drain_from = samples_;
            if (running->engine) ++engine_drains_started_;
            if (running->ion_stun) ++ion_stun_drains_started_;
            if (running->power_to_weapons) ++power_to_weapons_drains_started_;
            if (running->invulnerability) ++invulnerability_drains_started_;
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
            {wanted.proxy, handle.value(), wanted.local, wanted.mesh_local, wanted.engine, wanted.ion_stun,
             wanted.power_to_weapons, born, wanted.invulnerability, log});
        ++started_[wanted.proxy_name];
    }
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
    auto step = proxy.life->prepare_step(visible, frame, mesh, 1.0F / 30.0F);
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
    for (const Ship& ship : ships_) {
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
    for (Ship& ship : ships_) {
        for (CloneProxy& proxy : ship.clone) complete(proxy);
        std::erase_if(ship.clone, [](const CloneProxy& proxy) { return !proxy.life; });
    }
    for (CloneProxy& proxy : clone_orphans_) complete(proxy);
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
            if (running->ion_stun) {
                ion_stun_max_particles_ = std::max(ion_stun_max_particles_,
                    static_cast<std::uint64_t>(advanced.particles));
                ion_stun_dropped_at_capacity_ += advanced.advance.dropped_at_capacity;
            }
            // A drain that has no particle left, or has drained for the bound, is released.
            const bool finished = running->draining && advanced.finished;
            if (running->draining && (finished || samples_ - running->drain_from >= engine_drain_limit_samples)) {
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
                         const FadeOpacity& fade_opacity, const std::span<const sim::EntityId> unfogged_props) {
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
    // FW-19: retain already running emitters while their host eases out of raw visibility.
    // A newly revealed host with no running stream still starts at its first visible sample.
    std::vector<sim::EntityId> fading_emitters;
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
    for (; samples_ + 1U < due; ++samples_) {
        ++caught_up_;
        const double tick = *clock_start_ + static_cast<double>(samples_) + 1.0;
        const auto base = static_cast<std::uint64_t>(std::max(0.0, std::floor(tick + 1.0e-9)));
        const auto before = snapshot_at(base);
        const auto after = snapshot_at(base + 1U);
        std::vector<space::LiveUnitPose> units;
        if (before && after) {
            const double share = std::max(0.0, tick - static_cast<double>(base));
            units = space::interpolate_units(*before, *after, share, viewer, reveal, fading_emitters);
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
        // BP-45/FW-19: engine brightness and shared unit opacity multiply at presentation;
        // damage, fire and other unit emitters carry the same opacity as their hull.
        for (auto running = ship.running.begin(); running != ship.running.end();) {
            const float fade = fade_opacity ? fade_opacity(view->entity) : 1.0F;
            if (registry_->set_brightness(running->handle, (running->engine ? ship.engine_brightness : 1.0F) * fade)) {
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

} // namespace eawr::presentation::godot_backend
