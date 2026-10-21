#pragma once

#include "eawr/sim/galactic/ai_funding.hpp"
#include "eawr/core/record_digest.hpp"
#include "eawr/sim/galactic/session.hpp"
#include "eawr/script/foc/galactic_ai.hpp"

#include <map>
#include <mutex>
#include <span>
#include <utility>
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
    struct BoundState;
public:
    using State = funding::State;
    using Replay = std::function<core::Result<void>(GalacticFunding&)>;
    using ReplaySink = std::function<void(sim::galactic::PlayerId, Replay)>;
    GalacticFunding();
    [[nodiscard]] std::string record_sync_hash(bool cold = false) const;
    [[nodiscard]] std::size_t record_byte_upper_bound() const noexcept;
    [[nodiscard]] const State& state() const noexcept { return bound().state_; }
    // Freeze every ledger once, then let each decision owner read and mutate its
    // own physical copy. Successful typed operations commit in player order.
    [[nodiscard]] core::Result<void> prepare_player_frontiers(const sim::PartitionExecutor&,
        std::span<const sim::galactic::PlayerId>, ReplaySink = {});
    [[nodiscard]] core::Result<std::size_t> commit_player_frontiers();
    class PlayerFrontier {
    public:
        PlayerFrontier(const PlayerFrontier&) = delete;
        PlayerFrontier& operator=(const PlayerFrontier&) = delete;
        ~PlayerFrontier();
    private:
        friend class GalacticFunding;
        PlayerFrontier(GalacticFunding&, sim::galactic::PlayerId);
        const GalacticFunding* previous_owner_{};
        BoundState* previous_frontier_{};
        std::unique_lock<std::recursive_mutex> lock_;
    };
    [[nodiscard]] PlayerFrontier player_frontier(sim::galactic::PlayerId player) { return PlayerFrontier(*this, player); }
    // A native action records one world intent. Its funding callbacks update
    // the private ledger here; the actual admission invokes them at commit.
    class SuspendIntents {
    public:
        SuspendIntents(const SuspendIntents&) = delete;
        SuspendIntents& operator=(const SuspendIntents&) = delete;
        ~SuspendIntents();
    private:
        friend class GalacticFunding;
        explicit SuspendIntents(GalacticFunding&);
        BoundState* selected_{};
    };
    [[nodiscard]] SuspendIntents suspend_player_intents() { return SuspendIntents(*this); }
    [[nodiscard]] std::uint64_t state_revision() const noexcept;
    [[nodiscard]] std::uint64_t state_revision(sim::galactic::PlayerId) const;
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
    // Accepted immediate transactions update their private projection only.
    // Prepare before publishing the private world; the token carries no callback
    // or canonical intent and can be consumed only in its unchanged frontier.
    class PrivateRows {
    public:
        PrivateRows(const PrivateRows&) = delete;
        PrivateRows& operator=(const PrivateRows&) = delete;
        PrivateRows(PrivateRows&& other) noexcept
            : selected_(std::exchange(other.selected_, nullptr)), generation_(other.generation_), revision_(other.revision_),
              changes_(std::move(other.changes_)) {}
    private:
        friend class GalacticFunding;
        PrivateRows(BoundState* selected, std::uint64_t generation, std::uint64_t revision, State changes)
            : selected_(selected), generation_(generation), revision_(revision), changes_(std::move(changes)) {}
        BoundState* selected_{};
        std::uint64_t generation_{}, revision_{};
        State changes_;
    };
    [[nodiscard]] bool has_player_frontier() const noexcept { return frontier_owner_ == this; }
    [[nodiscard]] core::Result<PrivateRows> prepare_private_rows(State) const;
    [[nodiscard]] core::Result<void> publish_private_rows(PrivateRows);
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
    [[nodiscard]] core::Result<void> set_timeline_impl(std::int32_t timeline);
    [[nodiscard]] core::Result<void> initialize_impl(sim::galactic::PlayerId player,
        std::span<const EvaluatedFundingTemplate> templates, funding::Amount credits);
    [[nodiscard]] core::Result<void> apply_template_impl(sim::galactic::PlayerId player, const EvaluatedFundingTemplate& input);
    [[nodiscard]] core::Result<void> begin_maintenance_impl(sim::galactic::PlayerId player,
        std::span<const EvaluatedFundingWeight> weights);
    [[nodiscard]] core::Result<void> clear_equations_impl(sim::galactic::PlayerId player);
    [[nodiscard]] core::Result<void> notify_impl(const sim::galactic::GalacticSession::CreditNotification& input);
    [[nodiscard]] core::Result<void> reserve_impl(sim::galactic::PlayerId player, const funding::GoalRequest& request);
    [[nodiscard]] core::Result<void> mark_spending_impl(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount);
    [[nodiscard]] core::Result<void> spend_impl(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount);
    [[nodiscard]] core::Result<void> remove_goal_impl(sim::galactic::PlayerId player, funding::GoalId goal);
    [[nodiscard]] core::Result<void> allocate_resources_impl(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount);
    [[nodiscard]] core::Result<void> release_resources_impl(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount);
    [[nodiscard]] core::Result<void> remove_goal_salvage_impl(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount selected);
    [[nodiscard]] core::Result<void> flush_category_impl(sim::galactic::PlayerId player, funding::CategoryId category);
    [[nodiscard]] core::Result<funding::Amount> enter_tactical_impl(sim::galactic::PlayerId player, funding::TacticalTransferRequest request);
    [[nodiscard]] core::Result<funding::Amount> release_tactical_impl(sim::galactic::PlayerId player, funding::TacticalReleaseRequest request);
    [[nodiscard]] core::Result<funding::Amount> restore_strategic_impl(sim::galactic::PlayerId player, funding::Amount credits);
    struct PlayerRecords {
        std::uint64_t revision{};
        core::RecordDigest records;
    };
    struct BoundState {
        std::uint64_t private_generation{};
        std::uint64_t revision_{1};
        State state_;
        std::map<sim::galactic::PlayerId, PlayerRecords> player_records_;
        core::RecordDigest record_digest_;
        std::vector<Replay> operations;
        std::function<void(Replay)> sink;
        std::size_t mutation_depth{};
    };
    [[nodiscard]] BoundState& bound() noexcept;
    [[nodiscard]] const BoundState& bound() const noexcept;
    template<class Body, class Factory> auto mutate(Body body, Factory factory) {
        if (frontier_owner_ != this) return body();
        auto& selected = bound();
        const bool capture = selected.mutation_depth == 0;
        Replay replay = capture ? factory() : Replay{};
        struct Depth { std::size_t& value; ~Depth() { --value; } } depth{selected.mutation_depth};
        ++selected.mutation_depth;
        auto result = body();
        if (capture && result) {
            if (selected.sink) selected.sink(std::move(replay));
            else selected.operations.push_back(std::move(replay));
        }
        return result;
    }
    [[nodiscard]] const funding::Ledger* find_ledger(sim::galactic::PlayerId player) const noexcept;
    [[nodiscard]] core::Result<void> commit(sim::galactic::PlayerId, core::Result<funding::Ledger>);
    [[nodiscard]] core::Result<funding::Amount> commit_transfer(sim::galactic::PlayerId, core::Result<funding::TransferResult>);
    BoundState root_;
    struct Frontier {
        BoundState state;
        std::uint64_t generation{};
        std::recursive_mutex mutex;
    };
    std::map<sim::galactic::PlayerId, Frontier> frontiers_;
    std::vector<sim::galactic::PlayerId> prepared_players_;
    std::string prepared_hash_;
    std::uint64_t prepared_revision_{};
    static thread_local const GalacticFunding* frontier_owner_;
    static thread_local BoundState* active_frontier_;
};
} // namespace eawr::script::foc::ai
