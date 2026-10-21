#pragma once

#include "eawr/sim/world.hpp"

#include <string_view>

namespace eawr::script::foc::galactic_ai {

// The executor everything inside one galactic AI player's decision chain is handed (GFS-A02 service order,
// GFD-A05/A07 own-effects-visible). The chain is sequential by rule, and once each player is an executor
// partition a phase started inside it runs inline on that worker anyway (docs/simulation.md), so this
// executor does the same today: every partition runs on the calling thread, lowest first. Bulk work that
// does not depend on the chain's own writes belongs in a prepass over S_t before the chain starts
// (Runtime::prepare_path_tables); what still reaches this executor is player-owned and small.
class DecisionPassExecutor final : public sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 1; }
    [[nodiscard]] core::Result<void> execute(std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override {
        return inline_.execute(partition_count, partition);
    }
    [[nodiscard]] core::Result<void> execute_phase(std::string_view, std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override {
        return inline_.execute(partition_count, partition);
    }

private:
    sim::InlineExecutor inline_;
};

} // namespace eawr::script::foc::galactic_ai
