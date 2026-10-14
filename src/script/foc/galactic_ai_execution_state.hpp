#pragma once

#include "galactic_ai_execution.hpp"

namespace eawr::script::foc::galactic_ai { class LiveExecutionStateContributor; }
namespace eawr::campaign { class GalacticAiUsabilityProvider; }
namespace eawr::script::foc::ai {
// Restore-barrier capability: only E and B can reconstruct archived inputs.
class DerivedExecutionRestore {
    DerivedExecutionRestore() = default;
    friend class galactic_ai::LiveExecutionStateContributor;
    friend class eawr::campaign::GalacticAiUsabilityProvider;
public:
    DerivedExecutionRestore(const DerivedExecutionRestore&) = delete;
    DerivedExecutionRestore& operator=(const DerivedExecutionRestore&) = delete;
};
}
namespace eawr::script::foc::galactic_ai {

// WGA-29/139: retained configuration and ownership, not the derived route matrix.
struct ExecutionRetainedPlayer {
    bool initialized{}, finder_present{};
    std::uint64_t next_refresh{};
    std::vector<gc::PlanetId> node_order;
    Number maximum_distance{};
    std::map<sim::EntityId, std::uint64_t> reservations;
    bool operator==(const ExecutionRetainedPlayer&) const = default;
};
using ExecutionRetainedState = std::map<gc::PlayerId, ExecutionRetainedPlayer>;

// The campaign owner updates this at the completed barrier and supplies the
// required native reference validator on restore after world, child and Lua
// reconstruction. IDs do not remap. Known pending loss/cancel identities remain
// valid; absence from the current live roster alone must not drop reservations.
class ExecutionStateContributor final : public TrackedStateContributor {
public:
    using Validator = std::function<core::Result<void>(const ExecutionRetainedState&)>;
    static constexpr std::string_view component = "galactic-execution";
    static constexpr std::uint32_t version = 1;
    explicit ExecutionStateContributor(Validator validator = {}) : validator_(std::move(validator)) {}
    [[nodiscard]] const ExecutionRetainedState& state() const noexcept { return state_; }
    [[nodiscard]] core::Result<void> replace(ExecutionRetainedState);
    [[nodiscard]] core::Result<NativeState> capture() const override;
    [[nodiscard]] core::Result<void> restore(const NativeState&) override;
    [[nodiscard]] core::Result<std::string> state_hash() const override;
private:
    ExecutionRetainedState state_;
    Validator validator_;
};
// F binds these reconstructed native owners before restoring this contributor.
// Each restored matrix is derived from B's current facts in the saved query
// order. All matrices and pool ownership stage before any state is published.
class LiveExecutionStateContributor final : public TrackedStateContributor {
public:
    // Borrowed state is untracked until its runtime binds every writer to this
    // owner. Debug/full verification checks the opt-in promise at each hash.
    void enable_revision_tracking() noexcept { mark_state_changed(); tracked_ = true; }
    [[nodiscard]] std::optional<std::uint64_t> state_revision() const noexcept override {
        return tracked_ ? TrackedStateContributor::state_revision() : std::nullopt;
    }
    [[nodiscard]] core::Result<void> replace_retained(ExecutionRetainedState);
    using Inputs = std::function<core::Result<ExecutionRouteInput>(gc::PlayerId, const ai::DerivedExecutionRestore*)>;
    LiveExecutionStateContributor(ExecutionRetainedState& retained, std::map<gc::PlayerId, ExecutionRoutes>& routes,
        std::shared_ptr<FreestoreInput> pool, Inputs inputs, ExecutionStateContributor::Validator validator,
        const sim::PartitionExecutor& executor)
        : retained_(retained), routes_(routes), pool_(std::move(pool)), inputs_(std::move(inputs)),
          validator_(std::move(validator)), executor_(executor) {}
    [[nodiscard]] core::Result<NativeState> capture() const override;
    [[nodiscard]] core::Result<void> restore(const NativeState&) override;
    [[nodiscard]] core::Result<std::string> state_hash() const override;
private:
    bool tracked_{};
    ExecutionRetainedState& retained_;
    std::map<gc::PlayerId, ExecutionRoutes>& routes_;
    std::shared_ptr<FreestoreInput> pool_;
    Inputs inputs_;
    ExecutionStateContributor::Validator validator_;
    const sim::PartitionExecutor& executor_;
};
} // namespace eawr::script::foc::galactic_ai
