#include "eawr/core/load_profile.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/core/tick_profile.hpp"

#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/pathfind.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "blast_internal.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "orders_internal.hpp"
#include "staging.hpp"
#include "session_tick.hpp"
#include "session_services.hpp"
#include "tactical_internal.hpp"

#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>


namespace eawr::sim::tactical {

session_detail::Tick::Tick(TacticalSession& session, Impl* impl, const PartitionExecutor& executor_source)
    : session_(session), impl_(impl), executor(executor_source), tick(impl->completed_tick),
      pending_starbases_(impl->starbases), pending_outcome_(impl->outcome),
      pending_retreat_(impl->retreat_state), pending_midbattle_solve_(impl->midbattle_solve),
      pending_rng_(impl->rng_state),
      manual_clocks_(impl->manual_clocks), special_weapons_(impl->special_weapons),
      special_disabled_by_hardpoint_(impl->special_disabled_by_hardpoint),
      named_ability_locks_(impl->named_ability_locks),
      projectile_defence_order_(impl->projectile_defence_order) {
    // FC-R02 / WR-58: one active-mode prefix sample; save-only/paused parent barriers never call step().
    if (pending_retreat_.phase == RetreatPhase::escaping
        && pending_retreat_.cinematic_frame < pending_retreat_.cinematic_duration)
        ++pending_retreat_.cinematic_frame;
}

void session_detail::Tick::mark(const std::string_view section, const bool begin) const {
    if (impl_->commit_observer) impl_->commit_observer(section, begin);
}

core::Result<TacticalTick> TacticalSession::step(const PartitionExecutor& executor) {
    if (executor.worker_count() == 0) {
        return core::Result<TacticalTick>::failure(detail::diagnostic(
            diagnostic_codes::worker_failure, "executor reports no workers"));
    }
    if (impl_->object_turn_running) {
        return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
            "step during an object-turn provider call"));
    }
    const auto tick = impl_->completed_tick;
    // Stopping at the replay tick limit keeps record() a valid replay v2.
    if (tick >= max_ticks) {
        return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            "tick " + std::to_string(tick) + " reached the tactical tick limit"));
    }
    auto context = [&] {
        core::tick_profile::Scope scope("serial.tactical.setup");
        return session_detail::Tick(*this, impl_.get(), executor);
    }();
    return context.run();
}

namespace {

// A failure from deep inside a tick carries only its arithmetic message; the phase names where it came from.
core::Result<TacticalTick> phase_failure(const std::string_view phase, core::Diagnostic error) {
    error.message = std::string(phase) + ": " + error.message;
    return core::Result<TacticalTick>::failure(std::move(error));
}

} // namespace

core::Result<TacticalTick> session_detail::Tick::run() {
    core::tick_profile::Scope tail("serial.tactical.tail");
    const auto timed = [](const std::string_view name, auto&& run) {
        core::tick_profile::Scope scope(name);
        return run();
    };
    metrics_.emplace();
    metrics_.value().initial_emplacements.emplace(impl_->registry_emplacement_count);

    if (const auto phase = timed("serial.tactical.gather", [&] { return gather(); }); !phase) return phase_failure("gather", phase.error());
    if (const auto phase = timed("serial.tactical.prepare_craft", [&] { return prepare_craft(); }); !phase) return phase_failure("prepare_craft", phase.error());
    if (const auto phase = timed("serial.tactical.movement", [&] { return movement(); }); !phase) return phase_failure("movement", phase.error());
    if (const auto phase = timed("serial.tactical.open_staging", [&] { return open_staging(); }); !phase) return phase_failure("open_staging", phase.error());
    if (const auto phase = timed("serial.tactical.commit_moves", [&] { return commit_moves(); }); !phase) return phase_failure("commit_moves", phase.error());
    if (const auto phase = timed("serial.tactical.nebulas", [&] { return nebulas(); }); !phase) return phase_failure("nebulas", phase.error());
    if (const auto phase = timed("serial.tactical.prepare_targets", [&] { return prepare_targets(); }); !phase) return phase_failure("prepare_targets", phase.error());
    if (const auto phase = timed("serial.tactical.projectile_inputs", [&] { return projectile_inputs(); }); !phase) return phase_failure("projectile_inputs", phase.error());
    tracked_.emplace();
    tracked_->replans.emplace();
    tracked_->staged.emplace(std::move(gather_->moving.value()));
    if (const auto phase = timed("serial.tactical.impacts", [&] { return impacts(); }); !phase) return phase_failure("impacts", phase.error());
    gather_->moving = tracked_->staged->release();
    auto& moving = gather_->moving.value();
    if (std::any_of(moving.begin(), moving.end(), [](const LiveUnit& unit) {
        return unit.abilities && unit.abilities->charged_blast;
    })) {
        // FS-34, FD-53/55: the special-handler tail precedes hull constraints and hardpoint attempts.
        // FD-56's broader registration-order damage timing stays on the published deferred seam.
        const auto blast_polled = executor.execute_phase("blast-charge", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto index = range.begin; index < range.end; ++index) impl_->service_blast(moving[index], tick);
        });
        if (!blast_polled) return core::Result<TacticalTick>::failure(blast_polled.error());
    }
    if (const auto phase = timed("serial.tactical.hull_service", [&] { return hull_service(); }); !phase) return phase_failure("hull_service", phase.error());
    if (const auto phase = timed("serial.tactical.targets", [&] { return targets(); }); !phase) return phase_failure("targets", phase.error());
    if (const auto phase = timed("serial.tactical.ion_team", [&] { return ion_team(); }); !phase) return phase_failure("ion_team", phase.error());
    if (const auto phase = timed("serial.tactical.orders", [&] { return orders(); }); !phase) return phase_failure("orders", phase.error());
    if (const auto phase = timed("serial.tactical.launch_shots", [&] { return launch_shots(); }); !phase) return phase_failure("launch_shots", phase.error());
    impacts_->projectiles->insert(impacts_->projectiles->end(), flights_->launched->begin(), flights_->launched->end());
    impacts_->events->insert(impacts_->events->end(), manual_feedback_.begin(), manual_feedback_.end());
    if (const auto phase = timed("serial.tactical.abilities_tracking", [&] { return abilities_tracking(); }); !phase) return phase_failure("abilities_tracking", phase.error());
    if (const auto phase = timed("serial.tactical.late_impacts", [&] { return late_impacts(); }); !phase) return phase_failure("late_impacts", phase.error());
    if (const auto phase = timed("serial.tactical.retaliation", [&] { return retaliation(); }); !phase) return phase_failure("retaliation", phase.error());
    if (const auto phase = timed("serial.tactical.commands", [&] { return commands(); }); !phase) return phase_failure("commands", phase.error());
    if (const auto phase = timed("serial.tactical.special_weapons", [&] { return special_weapons(); }); !phase) return phase_failure("special_weapons", phase.error());
    if (const auto phase = timed("serial.tactical.systems", [&] { return systems(); }); !phase) return phase_failure("systems", phase.error());
    if (const auto phase = timed("serial.tactical.commit_survivors", [&] { return commit_survivors(); }); !phase) return phase_failure("commit_survivors", phase.error());
    if (const auto phase = timed("serial.tactical.retreat_service", [&] { return retreat_service(); }); !phase) return phase_failure("retreat_service", phase.error());
    if (const auto phase = timed("serial.tactical.pad_lifecycle", [&] { return pad_lifecycle(); }); !phase) return phase_failure("pad_lifecycle", phase.error());
    if (const auto phase = timed("serial.tactical.fighters", [&] { return fighters(); }); !phase) return phase_failure("fighters", phase.error());
    if (const auto phase = timed("serial.tactical.station_upgrades", [&] { return station_upgrades(); }); !phase) return phase_failure("station_upgrades", phase.error());
    if (const auto phase = timed("serial.tactical.economy", [&] { return economy(); }); !phase) return phase_failure("economy", phase.error());
    if (const auto phase = timed("serial.tactical.hangars", [&] { return hangars(); }); !phase) return phase_failure("hangars", phase.error());
    if (const auto phase = timed("serial.tactical.bonuses", [&] { return bonuses(); }); !phase) return phase_failure("bonuses", phase.error());
    if (const auto phase = timed("serial.tactical.finalize", [&] { return finalize(); }); !phase) return phase_failure("finalize", phase.error());
    if (const auto phase = timed("serial.tactical.commit", [&] { return commit(); }); !phase) return phase_failure("commit", phase.error());
    return timed("serial.tactical.finish", [&] { return finish(); });
}

} // namespace eawr::sim::tactical
