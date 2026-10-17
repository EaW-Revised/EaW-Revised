#include "ai_engine_internal.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_world.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace eawr::script::foc::ai {
using namespace engine_internal;
namespace {
template<class T> T required(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void required(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
} // namespace

void Engine::service_galactic_assembly(PlayerAi& player) {
    auto taskforces_ = all_taskforces();
    const auto native_record_write = native_record_scope();
    using Phase = BuildTask::GalacticAssembly;
    using Kind = galactic_ai::ActionKind;
    auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
    const auto now = static_cast<std::uint64_t>(frame_);
    const auto change = [&](BuildTask& task, Phase phase) {
        native_records_changed(player.player, {4});
        task.galactic_assembly = phase; task.state_entered = now;
        task.finished = phase == Phase::complete || phase == Phase::failed;
        task.failed = phase == Phase::failed;
        mark_native_state_changed();
        mark_galactic_tasks_changed();
    };
    const auto fail = [&](BuildTask& task) {
        if (task.galactic_assembly_request) {
            required(actions->retire_movement(task.galactic_assembly_request));
            task.galactic_assembly_request = 0;
        }
        change(task, Phase::failed);
    };
    // WGA-485 / GFC-A11: this is work inside the admitted execution turn.
    // Readiness precedes launch/separation and the countdown belongs to a
    // TaskForce, not a planet, type, player-global timer or arrival deadline.
    std::map<std::uint64_t, std::vector<BuildTask*>> synchronized;
    for (const auto& task : player.tasks) if (task.galactic_assembly == Phase::waiting)
        synchronized.try_emplace(task.taskforce);
    if (!synchronized.empty()) for (auto& task : player.tasks) if (!task.finished)
        if (const auto group = synchronized.find(task.taskforce); group != synchronized.end()) group->second.push_back(&task);
    for (auto& [force_id, tasks] : synchronized) {
        (void)force_id;
        if (!std::all_of(tasks.begin(), tasks.end(), [](const auto* task) {
            return task->galactic_assembly == Phase::waiting;
        })) continue;
        const auto* force = taskforce(tasks.front()->taskforce);
        const auto* running = force ? plan(force->plan) : nullptr;
        if (!running || !galactic_goal(player.player, running->goal)) continue;
        if (!actions || !galactic_stage_speed_) throw std::runtime_error("synchronized staging requires its data-defined speed reader");
        const auto* world = dynamic_cast<const GalacticWorld*>(host_->ai_world.get());
        const auto snapshot = actions->snapshot();
        if (!world || !world->execution_available() || !snapshot)
            throw std::runtime_error("synchronized staging requires the cached execution path owner");
        Real longest{};
        bool valid = true;
        for (auto* task : tasks) {
            const auto location = actions->assembly_location(player.player, task->object);
            const auto speed = required(galactic_stage_speed_(player.player, *snapshot, task->type));
            if (!location || location->transit || !(Real{} < speed)) { valid = false; break; }
            Real distance{};
            if (location->planet != task->staging) {
                const auto* route = world->execution_route(player.player, location->planet, task->staging);
                if (!route) throw std::runtime_error("synchronized staging cached route unavailable");
                // WGA-136: an explicitly empty route still has a decoded cost;
                // this estimate never manufactures an ordinary movement route.
                distance = route->distance;
            }
            task->galactic_stage_seconds = to_single(distance / speed);
            longest = std::max(longest, task->galactic_stage_seconds);
        }
        if (!valid) { for (auto* task : tasks) fail(*task); continue; }
        // Retail raises, rather than overwrites, every retained same-force copy.
        for (auto& task : player.tasks) if (task.synchronized_staging && !task.finished && task.taskforce == tasks.front()->taskforce)
            task.galactic_stage_remaining = std::max(task.galactic_stage_remaining, longest);
        for (auto* task : tasks) {
            task->galactic_stage_tick = now;
            change(*task, Phase::countdown);
        }
        record(running, player.player, "assembly_sync_started", std::to_string(tasks.front()->block));
    }
    std::map<std::uint64_t, std::vector<BuildTask*>> groups;
    for (auto& task : player.tasks) {
        if (task.galactic_assembly == Phase::none) continue;
        if (!actions) throw std::runtime_error("galactic assembly lost its native owner");
        if (task.finished && task.synchronized_staging && !task.failed) continue;
        groups[task.block].push_back(&task);
        if (task.finished) continue;
        const auto* force = taskforce(task.taskforce);
        const auto* running = force ? plan(force->plan) : nullptr;
        if (!running || !galactic_goal(player.player, running->goal)) { fail(task); continue; }
        if (task.galactic_assembly == Phase::waiting) continue;
        if (task.galactic_assembly == Phase::countdown) {
            if (task.galactic_stage_tick == now) continue;
            // WGA-485: the debug-build execution delay is three logical ticks (0.1 s).
            // One admitted service subtracts once, even after a long gap; no
            // elapsed-wall-time catch-up and no render-rate dependency.
            task.galactic_stage_remaining = to_single(task.galactic_stage_remaining - to_single(real(3) / real(fps)));
            task.galactic_stage_tick = now;
            native_records_changed(player.player, {4}); mark_native_state_changed();
            if (task.galactic_stage_seconds < task.galactic_stage_remaining) continue;
            record(running, player.player, "assembly_sync_released", std::to_string(task.object));
            if (task.galactic_stage_remaining <= Real{})
                record(running, player.player, "assembly_sync_expired", std::to_string(task.object));
            if (!required(actions->prepare_assembly_actor(player.player, task.object)).admitted) { fail(task); continue; }
            auto& retained = taskforces_.at(task.taskforce);
            if (std::find(retained.units.begin(), retained.units.end(), task.object) == retained.units.end()) retained.units.push_back(task.object);
            retained.had_units = task.joined = true;
            native_record_changed(player.player, 21, retained.id);
            change(task, Phase::assigned);
            continue;
        }
        if (task.galactic_assembly == Phase::assigned && task.state_entered < now) {
            const auto location = actions->assembly_location(player.player, task.object);
            if (!location || location->transit || !location->fleet) { change(task, Phase::failed); continue; }
            if (location->planet == task.staging) { change(task, Phase::staged); continue; }
            const auto snapshot = actions->snapshot();
            const auto stealth = galactic_stage_combat_skip_
                ? required(galactic_stage_combat_skip_(player.player, *snapshot, task.object))
                : required(snapshot->actor_combat_skip(task.object));
            galactic_ai::ActionRequest move;
            move.kind = Kind::move; move.player = player.player; move.plan = running->id;
            move.taskforce = task.taskforce; move.block = task.block;
            move.object = location->fleet; move.target = task.staging; move.members = {task.object};
            move.reach = stealth ? galactic_ai::MovementReach::any : task.synchronized_staging
                ? galactic_ai::MovementReach::enemy_undefended : galactic_ai::MovementReach::friendly_only;
            const auto admitted = required(actions->admit(move));
            if (!admitted.admitted || !admitted.request) { change(task, Phase::failed); continue; }
            task.galactic_assembly_request = admitted.request;
            change(task, Phase::moving);
        } else if (task.galactic_assembly == Phase::moving) {
            const auto moved = required(actions->poll_movement(task.galactic_assembly_request, !force->units.empty()));
            if (!moved.finished) {
                const auto location = actions->assembly_location(player.player, task.object);
                // A conflict return can leave this block with no resumable hop.
                // In-flight hops and live conflict waits stay pending.
                if (!moved.advance_requested && !moved.conflict_location
                    && (!location || !location->transit)) fail(task);
                continue;
            }
            required(actions->retire_movement(task.galactic_assembly_request));
            task.galactic_assembly_request = 0;
            const auto location = actions->assembly_location(player.player, task.object);
            if (!location || location->transit || location->planet != task.staging) {
                change(task, task.synchronized_staging ? Phase::complete : Phase::failed); continue;
            }
            // Retail arrival can land a transport before the later coordinated
            // all-task wait launches it again for final orbital formation.
            required(actions->land_assembly_arrival(player.player, task.object));
            change(task, Phase::staged);
        }
    }
    for (auto& [block_id, tasks] : groups) {
        if (std::all_of(tasks.begin(), tasks.end(), [](const auto* task) { return task->finished; })) continue;
        auto& force = taskforces_.at(tasks.front()->taskforce);
        const auto* running = plan(force.plan);
        const auto fail_group = [&] {
            for (auto* task : tasks) if (!task->finished) fail(*task);
        };
        if (!running || std::any_of(player.tasks.begin(), player.tasks.end(), [&](const auto& task) { return task.block == block_id && task.failed; })) {
            fail_group(); continue;
        }
        // The stock existing-force route is all source0. A caller with other
        // producers must wait for their real results before force formation.
        if (std::any_of(player.tasks.begin(), player.tasks.end(), [&](const auto& task) {
            return task.block == block_id && task.galactic_assembly == Phase::none && !task.finished;
        })) continue;
        const bool staged = std::all_of(tasks.begin(), tasks.end(), [&](const auto* task) {
            return task->galactic_assembly == Phase::staged && task->state_entered < now;
        });
        const bool orbital = std::all_of(tasks.begin(), tasks.end(), [&](const auto* task) {
            return task->galactic_assembly == Phase::orbital && task->state_entered < now;
        });
        if (!staged && !orbital) continue;
        bool ready = true;
        for (const auto* task : tasks) {
            const auto member = task->object;
            const auto location = actions->assembly_location(player.player, member);
            if (!location || location->transit || location->planet != tasks.front()->staging) { ready = false; break; }
            if (staged && location->landed) {
                const auto launched = required(actions->prepare_assembly_actor(player.player, member, false));
                if (!launched.admitted) { ready = false; break; }
            } else if (orbital && !location->fleet) { ready = false; break; }
        }
        if (!ready) { fail_group(); continue; }
        if (staged) {
            for (auto* task : tasks) change(*task, Phase::orbital);
            continue;
        }
        galactic_ai::ActionRequest form;
        // WGA-227 formation visits the full retained roster; readiness/launch
        // above belongs to this production block's actors only.
        form.kind = Kind::regroup; form.player = player.player; form.plan = running->id;
        form.taskforce = force.id; form.block = block_id; form.members = force.units;
        const auto formed = required(actions->admit(form));
        native_record_changed(player.player, 21, force.id);
        force.galactic_fleet = formed.admitted ? formed.request : 0;
        if (!force.galactic_fleet) for (const auto member : force.units)
            if (const auto fleet = actions->containing_fleet(player.player, member)) { force.galactic_fleet = *fleet; break; }
        if (!formed.admitted || !formed.request) { fail_group(); continue; }
        for (auto* task : tasks) change(*task, Phase::complete);
        record(running, player.player, "assembled", force.name + " fleet " + std::to_string(force.galactic_fleet));
    }
}

} // namespace eawr::script::foc::ai
