#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace eawr::sim::galactic::restore_detail {

// Post-restore checks of the service tables a checkpoint carries. Each returns the first violation (nullopt when the
// table is sound) and never mutates: a valid save is read exactly as before. Every rule is one that holds for live
// state, not only at tick zero, so the setup-only rules (empty queues, initial gates) are deliberately absent.
[[nodiscard]] std::optional<std::string> production_problem(const GalacticSetup& setup, const ProductionSetup& state);
[[nodiscard]] std::optional<std::string> income_problem(const GalacticSetup& setup, const IncomeState& state,
    std::uint64_t blocker_next_expiry);
[[nodiscard]] std::optional<std::string> corruption_problem(const GalacticSetup& setup, const EconomyCorruptionState& state);
[[nodiscard]] std::optional<std::string> observer_problem(const GalacticSetup& setup, const ObserverSetup& state);

} // namespace eawr::sim::galactic::restore_detail
