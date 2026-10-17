#include "galactic_ai_funding.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"
#include "ai_equations.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"

#include <set>
#include <utility>

namespace eawr::script::foc::ai {
GalacticFunding::GalacticFunding() : record_digest_(galactic_ai::funding_records(state_)) {}
std::string GalacticFunding::record_sync_hash(bool cold) const {
    return cold ? galactic_ai::funding_records(state_).digest("funding") : record_digest_.digest("funding");
}
core::Result<funding::Amount> funding_equation_weight(const funding::Ledger* current,
    funding::CategoryId category, std::optional<galactic_ai::Number> evaluated) {
    if (evaluated) return numeric::to_fixed(to_single(*evaluated));
    return core::Result<funding::Amount>::success(current && current->categories.contains(category)
        ? current->categories.at(category).share : funding::Amount{});
}
namespace {
template<class T> core::Result<T> invalid(const char* message) {
    core::Diagnostic diagnostic;
    diagnostic.code = funding::diagnostic_codes::invalid_state;
    diagnostic.message = message;
    return core::Result<T>::failure(std::move(diagnostic));
}
core::Result<funding::Ledger> apply(funding::Ledger ledger, const EvaluatedFundingTemplate& input) {
    for (const auto& row : input.equations) {
        auto changed = funding::set_equation(std::move(ledger), row.category, row.weight, input.priority);
        if (!changed) return changed;
        ledger = std::move(changed).value();
    }
    for (const auto& row : input.allowances) {
        auto changed = funding::set_allowance(std::move(ledger), row.category, row.allowance, input.priority);
        if (!changed) return changed;
        ledger = std::move(changed).value();
    }
    return core::Result<funding::Ledger>::success(std::move(ledger));
}
}
const funding::Ledger* GalacticFunding::find_ledger(sim::galactic::PlayerId player) const noexcept {
    const auto at = state_.find(player);
    return at == state_.end() ? nullptr : &at->second;
}
std::shared_ptr<const funding::Ledger> GalacticFunding::ledger(sim::galactic::PlayerId player) const {
    const auto* value = find_ledger(player);
    return value ? std::make_shared<const funding::Ledger>(*value) : nullptr;
}
core::Result<void> GalacticFunding::commit_rows(State changes, const std::function<core::Result<void>()>& publish) {
    if (!publish) return invalid<void>("native funding publication requires its world owner");
    for (const auto& [player, ledger] : changes) {
        if (!state_.contains(player)) return invalid<void>("native funding publication names an unknown player");
        auto valid = funding::category_order(ledger);
        if (!valid) return core::Result<void>::failure(valid.error());
    }
    if (auto published = publish(); !published) return published;
    if (++revision_ == 0) std::terminate();
    for (auto& [player, ledger] : changes) {
        auto previous = std::move(state_.at(player)); state_.at(player) = std::move(ledger);
        galactic_ai::update_funding_records(record_digest_, player, &previous, &state_.at(player));
    }
    return core::Result<void>::success();
}
core::Result<void> GalacticFunding::restore(State state) {
    if (state.constructor_income != funding::Amount{} || state.timeline < 0 || state.timeline > 5)
        return invalid<void>("invalid retained funding constructor state");
    for (const auto& [player, value] : state) {
        if (!player) return invalid<void>("funding state has an invalid player");
        auto valid = funding::category_order(value);
        if (!valid) return core::Result<void>::failure(valid.error());
    }
    if (++revision_ == 0) std::terminate();
    state_ = std::move(state); record_digest_ = galactic_ai::funding_records(state_);
    return core::Result<void>::success();
}
core::Result<void> GalacticFunding::commit(sim::galactic::PlayerId player, core::Result<funding::Ledger> result) {
    if (!result) return core::Result<void>::failure(result.error());
    if (++revision_ == 0) std::terminate();
    auto previous = std::move(state_.at(player)); state_.at(player) = std::move(result).value();
    galactic_ai::update_funding_records(record_digest_, player, &previous, &state_.at(player));
    return core::Result<void>::success();
}
core::Result<funding::Amount> GalacticFunding::commit_transfer(sim::galactic::PlayerId player,
    core::Result<funding::TransferResult> result) {
    if (!result) return core::Result<funding::Amount>::failure(result.error());
    auto value = std::move(result).value();
    if (++revision_ == 0) std::terminate();
    auto previous = std::move(state_.at(player)); state_.at(player) = std::move(value.ledger);
    galactic_ai::update_funding_records(record_digest_, player, &previous, &state_.at(player));
    return core::Result<funding::Amount>::success(value.credits);
}
core::Result<void> GalacticFunding::set_timeline(std::int32_t timeline) {
    if (timeline < 0 || timeline > 5) return invalid<void>("invalid retained timeline input");
    if (++revision_ == 0) std::terminate();
    state_.timeline = timeline; galactic_ai::update_funding_metadata(record_digest_, state_);
    return core::Result<void>::success();
}
core::Result<void> GalacticFunding::initialize(sim::galactic::PlayerId player,
    std::span<const EvaluatedFundingTemplate> templates, funding::Amount credits) {
    if (!player || state_.contains(player)) return invalid<void>("funding player already initialized or invalid");
    funding::Ledger value;
    for (const auto& input : templates) {
        auto changed = apply(std::move(value), input);
        if (!changed) return core::Result<void>::failure(changed.error());
        value = std::move(changed).value();
    }
    auto initialized = funding::initialize(std::move(value), credits);
    if (!initialized) return core::Result<void>::failure(initialized.error());
    if (++revision_ == 0) std::terminate();
    state_.emplace(player, std::move(initialized).value());
    galactic_ai::update_funding_records(record_digest_, player, nullptr, &state_.at(player));
    return core::Result<void>::success();
}
core::Result<void> GalacticFunding::apply_template(sim::galactic::PlayerId player, const EvaluatedFundingTemplate& input) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, apply(*value, input));
}
core::Result<void> GalacticFunding::begin_maintenance(sim::galactic::PlayerId player,
    std::span<const EvaluatedFundingWeight> weights) {
    const auto* current = find_ledger(player);
    if (!current) return invalid<void>("funding player is not initialized");
    auto value = *current;
    std::set<funding::CategoryId> seen;
    for (const auto& row : weights) {
        const auto at = value.categories.find(row.category);
        if (at == value.categories.end() || !seen.insert(row.category).second)
            return invalid<void>("reevaluation requires unique existing funding categories");
        at->second.evaluated_weight = row.weight;
    }
    return commit(player, funding::begin_maintenance(std::move(value)));
}
core::Result<void> GalacticFunding::clear_equations(sim::galactic::PlayerId player) {
    const auto* current = find_ledger(player);
    if (!current) return invalid<void>("funding player is not initialized");
    auto value = *current;
    for (auto& [id, category] : value.categories) {
        (void)id;
        category.evaluated_weight.reset();
        category.equation_priority = 1;
    }
    value.equations_changed = true;
    return commit(player, core::Result<funding::Ledger>::success(std::move(value)));
}
core::Result<void> GalacticFunding::notify(const sim::galactic::GalacticSession::CreditNotification& input) {
    const auto* value = find_ledger(input.player);
    if (!value) return core::Result<void>::success(); // GFC-A09 initialized-manager gate
    if (input.kind == sim::galactic::GalacticSession::CreditNotification::Kind::windfall)
        return commit(input.player, funding::windfall(*value, input.amount)); // WGA-200: retained totals remain writable
    if (value->tactical.switched) return invalid<void>("parent fiscal service during a tactical credit switch");
    return commit(input.player, funding::fiscal(*value, input.credits, input.amount, input.seconds));
}
core::Result<bool> GalacticFunding::feasible(sim::galactic::PlayerId player, const funding::GoalRequest& request, bool activation) const {
    const auto* value = find_ledger(player);
    if (!value) return invalid<bool>("funding player is not initialized");
    return funding::feasible(*value, request, activation);
}
core::Result<void> GalacticFunding::notify(const galactic_ai::FundingNotice& input) {
    using Kind = galactic_ai::NoticeKind;
    using Credit = sim::galactic::GalacticSession::CreditNotification;
    switch (input.kind) {
    case Kind::fiscal:
        return notify(Credit{Credit::Kind::fiscal, input.player, input.actual_credits,
            input.amount, input.interval_seconds});
    case Kind::windfall:
        return notify(Credit{Credit::Kind::windfall, input.player, input.actual_credits, input.amount, {}});
    case Kind::marked: return mark_spending(input.player, input.goal, input.amount);
    case Kind::billed: return spend(input.player, input.goal, input.amount);
    default: return invalid<void>("funding notice requires the producer or typed battle transfer owner");
    }
}
core::Result<bool> GalacticFunding::feasible(const galactic_ai::FundingTransaction& input) const {
    return feasible(input.player, input.request, input.activation);
}
core::Result<void> GalacticFunding::reserve(const galactic_ai::FundingTransaction& input) {
    return reserve(input.player, input.request);
}
core::Result<void> GalacticFunding::reserve(sim::galactic::PlayerId player, const funding::GoalRequest& request) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::reserve(*value, request));
}
core::Result<void> GalacticFunding::mark_spending(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::mark_spending(*value, goal, amount));
}
core::Result<void> GalacticFunding::spend(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::spend(*value, goal, amount));
}
core::Result<void> GalacticFunding::remove_goal(sim::galactic::PlayerId player, funding::GoalId goal) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::remove_goal(*value, goal));
}
core::Result<funding::Amount> GalacticFunding::purchase_wait(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) const {
    const auto* value = find_ledger(player);
    if (!value) return invalid<funding::Amount>("funding player is not initialized");
    return funding::purchase_wait(*value, goal, amount);
}
core::Result<void> GalacticFunding::allocate_resources(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::allocate_resources(*value, goal, amount));
}
core::Result<void> GalacticFunding::release_resources(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::release_resources(*value, goal, amount));
}
core::Result<void> GalacticFunding::remove_goal_salvage(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount selected) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::remove_goal_salvage(*value, goal, selected));
}
core::Result<void> GalacticFunding::flush_category(sim::galactic::PlayerId player, funding::CategoryId category) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::flush_category(*value, category));
}
core::Result<funding::Amount> GalacticFunding::enter_tactical(sim::galactic::PlayerId player, funding::TacticalTransferRequest request) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<funding::Amount>("funding player is not initialized");
    return commit_transfer(player, funding::enter_tactical(*value, request));
}
core::Result<funding::Amount> GalacticFunding::release_tactical(sim::galactic::PlayerId player, funding::TacticalReleaseRequest request) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<funding::Amount>("funding player is not initialized");
    return commit_transfer(player, funding::release_tactical(*value, request));
}
core::Result<funding::Amount> GalacticFunding::restore_strategic(sim::galactic::PlayerId player, funding::Amount credits) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<funding::Amount>("funding player is not initialized");
    return commit_transfer(player, funding::restore_strategic(*value, credits));
}
} // namespace eawr::script::foc::ai
