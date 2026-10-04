#include "eawr/core/load_profile.hpp"
#include "eawr/sim/tactical/session.hpp"

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
      manual_clocks_(impl->manual_clocks) {}

void session_detail::Tick::mark(const std::string_view section, const bool begin) const {
    if (impl_->commit_observer) impl_->commit_observer(section, begin);
}

core::Result<TacticalTick> TacticalSession::step(const PartitionExecutor& executor) {
    if (executor.worker_count() == 0) {
        return core::Result<TacticalTick>::failure(detail::diagnostic(
            diagnostic_codes::worker_failure, "executor reports no workers"));
    }
    const auto tick = impl_->completed_tick;
    // Stopping at the replay tick limit keeps record() a valid replay v2.
    if (tick >= max_ticks) {
        return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            "tick " + std::to_string(tick) + " reached the tactical tick limit"));
    }
    session_detail::Tick context(*this, impl_.get(), executor);
    return context.run();
}

core::Result<TacticalTick> session_detail::Tick::run() {
    metrics_.emplace();
    metrics_.value().initial_emplacements.emplace(impl_->registry_emplacement_count);

    if (const auto phase = gather(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = prepare_craft(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = movement(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = open_staging(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = commit_moves(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = nebulas(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = targets(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = ion_team(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = orders(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = projectile_inputs(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = abilities_tracking(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = impacts(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = commands(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = systems(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = commit_survivors(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = pad_lifecycle(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = fighters(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = economy(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = hangars(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = bonuses(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = finalize(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    if (const auto phase = commit(); !phase) return core::Result<TacticalTick>::failure(phase.error());
    return finish();
}

} // namespace eawr::sim::tactical
