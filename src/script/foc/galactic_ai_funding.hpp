#pragma once

#include "eawr/sim/galactic/ai_funding.hpp"
#include "eawr/sim/galactic/session.hpp"
#include "eawr/script/foc/galactic_ai.hpp"

#include <map>
#include <span>
#include <vector>

namespace eawr::script::foc::ai {
namespace funding = sim::galactic::funding;

// A resolves registered category identities and evaluates equations in the
// selected self/enemy/human context. These copied values never parse XML or
// substitute static weights for retained cash (WGA-67..69/82).
struct EvaluatedFundingWeight {
    funding::CategoryId category{};
    std::optional<funding::Amount> weight;
};
struct FundingAllowance {
    funding::CategoryId category{};
    funding::Amount allowance{};
};
struct EvaluatedFundingTemplate {
    int priority{1};
    std::vector<EvaluatedFundingWeight> equations;
    std::vector<FundingAllowance> allowances;
};
// WGA-421: only a present equation uses this consumer's output convention.
// A null startup ledger or a fresh category has constructor share zero;
// maintenance requires its initialized ledger before invoking this adapter.
[[nodiscard]] core::Result<funding::Amount> funding_equation_weight(
    const funding::Ledger*, funding::CategoryId, std::optional<galactic_ai::Number> evaluated);

// One retained ledger per initialized galactic AI player. Bounded manager and
// money transactions commit in caller order; no new per-entity galaxy sweep.
// E owns actual producer billing; these methods never debit a player account.
class GalacticFunding final : public galactic_ai::Funding {
public:
    using State = funding::State;
    [[nodiscard]] const State& state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t state_revision() const noexcept { return revision_; }
    // Owned immutable copy: retain the shared pointer while traversing its fields.
    [[nodiscard]] std::shared_ptr<const funding::Ledger> ledger(sim::galactic::PlayerId player) const override;
    // F restores this complete DTO at the native AI boundary, with its own
    // version/hash and reference validation; no alternate save format here.
    [[nodiscard]] core::Result<void> restore(State state);
    // WGA-295: explicit producer copy. Running update/reset cadence is #2701.
    [[nodiscard]] core::Result<void> set_timeline(std::int32_t);
    // Validate touched rows before the native world publishes; untouched ledgers
    // retain their storage. A failed callback publishes no funding changes.
    [[nodiscard]] core::Result<void> commit_rows(State, const std::function<core::Result<void>()>& publish);
    [[nodiscard]] core::Result<void> initialize(sim::galactic::PlayerId player,
        std::span<const EvaluatedFundingTemplate> templates, funding::Amount credits);
    [[nodiscard]] core::Result<void> apply_template(sim::galactic::PlayerId player, const EvaluatedFundingTemplate&);
    // WGA-104/105/108/109:
    // untrigger clears every equation/priority, then A replays surviving active
    // templates. Allowances/priorities, cash and goal reservations stay intact.
    [[nodiscard]] core::Result<void> clear_equations(sim::galactic::PlayerId player);
    // Reevaluate existing equations without changing their template priority.
    [[nodiscard]] core::Result<void> begin_maintenance(sim::galactic::PlayerId player,
        std::span<const EvaluatedFundingWeight> weights);
    [[nodiscard]] core::Result<void> notify(const sim::galactic::GalacticSession::CreditNotification&);
    // E/F own canonical exactly-once EventKey routing and persistence. Only
    // fiscal, windfall, accepted marks and actual bills are consumed here;
    // cancellation/refund/completion require their producer-owned semantics.
    [[nodiscard]] core::Result<void> notify(const galactic_ai::FundingNotice&) override;
    [[nodiscard]] core::Result<bool> feasible(const galactic_ai::FundingTransaction&) const override;
    [[nodiscard]] core::Result<void> reserve(const galactic_ai::FundingTransaction&) override;
    [[nodiscard]] core::Result<bool> feasible(sim::galactic::PlayerId, const funding::GoalRequest&, bool activation) const;
    [[nodiscard]] core::Result<void> reserve(sim::galactic::PlayerId, const funding::GoalRequest&);
    [[nodiscard]] core::Result<void> mark_spending(sim::galactic::PlayerId, funding::GoalId, funding::Amount);
    [[nodiscard]] core::Result<void> spend(sim::galactic::PlayerId, funding::GoalId, funding::Amount);
    [[nodiscard]] core::Result<void> remove_goal(sim::galactic::PlayerId, funding::GoalId);
    [[nodiscard]] core::Result<void> allocate_resources(sim::galactic::PlayerId, funding::GoalId, funding::Amount);
    [[nodiscard]] core::Result<void> release_resources(sim::galactic::PlayerId, funding::GoalId, funding::Amount);
    [[nodiscard]] core::Result<void> remove_goal_salvage(sim::galactic::PlayerId, funding::GoalId, funding::Amount);
    [[nodiscard]] core::Result<funding::Amount> purchase_wait(sim::galactic::PlayerId, funding::GoalId, funding::Amount) const;
    [[nodiscard]] core::Result<void> flush_category(sim::galactic::PlayerId, funding::CategoryId);
    // F commits returned credits through its battle owner, exactly once. The
    // retained tactical balance and dead-goal obligations are part of State.
    [[nodiscard]] core::Result<funding::Amount> enter_tactical(sim::galactic::PlayerId, funding::TacticalTransferRequest);
    [[nodiscard]] core::Result<funding::Amount> release_tactical(sim::galactic::PlayerId, funding::TacticalReleaseRequest);
    [[nodiscard]] core::Result<funding::Amount> restore_strategic(sim::galactic::PlayerId, funding::Amount);

private:
    [[nodiscard]] const funding::Ledger* find_ledger(sim::galactic::PlayerId player) const noexcept;
    [[nodiscard]] core::Result<void> commit(sim::galactic::PlayerId, core::Result<funding::Ledger>);
    [[nodiscard]] core::Result<funding::Amount> commit_transfer(sim::galactic::PlayerId, core::Result<funding::TransferResult>);
    std::uint64_t revision_{1};
    State state_;
};
} // namespace eawr::script::foc::ai
