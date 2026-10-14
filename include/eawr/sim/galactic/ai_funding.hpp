#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/math/fixed.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace eawr::sim::galactic::funding {
using Amount = math::Fixed;
using CategoryId = std::uint32_t;
using GoalId = std::uint64_t;

namespace diagnostic_codes {
inline constexpr std::string_view overflow = math::diagnostic_codes::overflow;
inline constexpr std::string_view negative_input = "EAWR-GCFUND-0001";
inline constexpr std::string_view missing_category = "EAWR-GCFUND-0002";
inline constexpr std::string_view missing_goal = "EAWR-GCFUND-0003";
inline constexpr std::string_view insufficient_funding = "EAWR-GCFUND-0004";
inline constexpr std::string_view unverified_rule = "EAWR-GCFUND-0005";
inline constexpr std::string_view invalid_state = "EAWR-GCFUND-0006";
inline constexpr std::string_view unmarked_spend = "EAWR-GCFUND-0007";
inline constexpr std::string_view invalid_input = "EAWR-GCFUND-0008";
} // namespace diagnostic_codes

// Fixture boundary: callers resolve registered category IDs and evaluate equations
// with self/enemy/human context (WGA-68/69/82). No XML or live AI is read here.
struct Category {
    std::optional<Amount> evaluated_weight;
    int equation_priority{1};
    int allowance_priority{1};
    Amount allowance{}, share{}, previous_share{};
    Amount cash{}, available_cash{}, income{}, available_income{};
    bool operator==(const Category&) const = default;
};
struct Goal {
    CategoryId category{};
    Amount cost{}, marked{}, capital{}, accumulated{}, reserved_income{}, remaining{};
    bool reassessed{};
    Amount unallocated{}; // WGA-345: independent retained proposal resources
    bool operator==(const Goal&) const = default;
};
// WGA-23: a removed goal can still own marked, not-yet-billed purchases.
struct Obligation {
    CategoryId category{};
    Amount marked{}, capital{}, accumulated{};
    bool operator==(const Obligation&) const = default;
};
struct TacticalBalance {
    bool switched{};
    Amount saved{}, extra_released{};
    bool operator==(const TacticalBalance&) const = default;
};
struct Ledger {
    std::map<CategoryId, Category> categories;
    std::map<GoalId, Goal> goals;
    std::map<GoalId, Obligation> dead_goals;
    Amount dead_budget{}; // WGA-160: aggregate orphan spending, independent of keyed compatibility rows
    Amount last_interval{};
    Amount last_service_income{}; // WGA-293/294: retained global rate; fresh zero
    bool equations_changed{};
    TacticalBalance tactical;
    bool operator==(const Ledger&) const = default;
};
// WGA-294/295: explicit constructor state survives an empty, pre-manager save.
// Map-compatible ledger access keeps transaction code bounded to touched rows.
struct State : std::map<std::uint32_t, Ledger> {
    using std::map<std::uint32_t, Ledger>::map;
    Amount constructor_income{};
    std::int32_t timeline{};
    bool operator==(const State&) const = default;
};
struct GoalRequest {
    GoalId id{};
    CategoryId category{};
    Amount cost{}, estimated_seconds{};
    // Absent is the authored -1 sentinel, WGA-21. Zero is a real limit.
    std::optional<Amount> time_limit;
    // Existing-plan resource estimate supplied by its host, WGA-21.
    Amount unallocated_resources{};
    Amount proposal_budget{}; // WGA-345: computed before selected-roster allocation
};
struct TacticalTransferRequest { bool human{}; Amount strategic_credits{}; };
struct TacticalReleaseRequest { Amount requested{}, tactical_credits{}; };
struct TransferResult { Ledger ledger; Amount credits{}, transferred{}; };

// Value-in/value-out operations: failures return core::Result diagnostics, leaving the input
// unchanged. Fixed-point rounding and largest-share (ID tie) residual assignment are project
// policies, not claims of single-precision retail bit identity (WGA-11/69).
// Named unverified seam AllowanceOnlyReevaluation: a positive stored share on a
// category without an equation, mixed with positive evaluated weights, is rejected
// until its post-initialization treatment is qualified; its DTO funds remain intact.
[[nodiscard]] core::Result<Ledger> set_equation(Ledger, CategoryId, std::optional<Amount>, int priority);
[[nodiscard]] core::Result<Ledger> set_allowance(Ledger, CategoryId, Amount, int priority);
[[nodiscard]] core::Result<Ledger> initialize(Ledger, Amount credits);
[[nodiscard]] core::Result<Ledger> begin_maintenance(Ledger); // WGA-12/75, before ordered reassessment
// WGA-75: caller supplies category/goal desire order from the goal-maintenance host.
// Equal-desire order and GAI-U04 allocation scheduling remain external named seams.
[[nodiscard]] core::Result<std::vector<CategoryId>> category_order(const Ledger&);
[[nodiscard]] core::Result<bool> feasible(const Ledger&, const GoalRequest&, bool activation);
[[nodiscard]] core::Result<Ledger> reserve(Ledger, const GoalRequest&);
[[nodiscard]] core::Result<Ledger> mark_spending(Ledger, GoalId, Amount);
[[nodiscard]] core::Result<Ledger> spend(Ledger, GoalId, Amount);
[[nodiscard]] core::Result<Ledger> remove_goal(Ledger, GoalId);
[[nodiscard]] core::Result<Ledger> allocate_resources(Ledger, GoalId, Amount);
[[nodiscard]] core::Result<Ledger> release_resources(Ledger, GoalId, Amount);
// WGA-349: selected task costs, not all marked obligations, enter dead budget.
[[nodiscard]] core::Result<Ledger> remove_goal_salvage(Ledger, GoalId, Amount selected);
[[nodiscard]] core::Result<Amount> purchase_wait(const Ledger&, GoalId, Amount);
[[nodiscard]] core::Result<Ledger> flush_category(Ledger, CategoryId);
// WGA-13..15/83: supplied actual credits already include the fiscal payment;
// planet arithmetic, difficulty scaling/caps and cost-estimate updates are seams.
[[nodiscard]] core::Result<Ledger> fiscal(Ledger, Amount actual_credits, Amount income, Amount seconds);
[[nodiscard]] core::Result<Ledger> windfall(Ledger, Amount actual_delta); // WGA-85: total cash only
[[nodiscard]] core::Result<TransferResult> enter_tactical(Ledger, TacticalTransferRequest);
[[nodiscard]] core::Result<TransferResult> release_tactical(Ledger, TacticalReleaseRequest);
[[nodiscard]] core::Result<TransferResult> restore_strategic(Ledger, Amount tactical_credits);
} // namespace eawr::sim::galactic::funding
