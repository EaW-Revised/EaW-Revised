#include "galactic_ai_funding.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"
#include "ai_equations.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"

#include <algorithm>
#include <stdexcept>
#include <set>
#include <utility>

namespace eawr::script::foc::ai {
GalacticFunding::GalacticFunding() { root_.record_digest_ = galactic_ai::funding_records(root_.state_); }
std::string GalacticFunding::record_sync_hash(bool cold) const {
    if (cold) return galactic_ai::funding_records(bound().state_).digest("funding");
    auto sums = bound().record_digest_.shards();
    for (const auto& [player, slice] : bound().player_records_) {
        static_cast<void>(player); core::RecordDigest::merge_shards(sums, slice.records.shards());
    }
    return core::RecordDigest::digest_of("funding", sums);
}
std::size_t GalacticFunding::record_byte_upper_bound() const noexcept {
    auto bytes = bound().record_digest_.byte_size() + bound().state_.size() * 32 + 1024;
    for (const auto& [player, slice] : bound().player_records_) { static_cast<void>(player); bytes += slice.records.byte_size(); }
    return bytes;
}
std::uint64_t GalacticFunding::state_revision() const noexcept {
    auto total = bound().revision_;
    for (const auto& [player, slice] : bound().player_records_) { static_cast<void>(player); total += slice.revision; }
    return total;
}
std::uint64_t GalacticFunding::state_revision(sim::galactic::PlayerId player) const {
    return bound().revision_ + bound().player_records_.at(player).revision;
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
thread_local const GalacticFunding* GalacticFunding::frontier_owner_ = nullptr;
thread_local GalacticFunding::BoundState* GalacticFunding::active_frontier_ = nullptr;
GalacticFunding::BoundState& GalacticFunding::bound() noexcept {
    return frontier_owner_ == this ? *active_frontier_ : root_;
}
const GalacticFunding::BoundState& GalacticFunding::bound() const noexcept {
    return frontier_owner_ == this ? *active_frontier_ : root_;
}
GalacticFunding::PlayerFrontier::PlayerFrontier(GalacticFunding& owner, sim::galactic::PlayerId player)
    : previous_owner_(frontier_owner_), previous_frontier_(active_frontier_) {
    if (!std::binary_search(owner.prepared_players_.begin(), owner.prepared_players_.end(), player))
        throw std::logic_error("funding player frontier was not prepared at the barrier");
    auto& selected = owner.frontiers_.at(player);
    lock_ = std::unique_lock<std::recursive_mutex>(selected.mutex);
    active_frontier_ = &selected.state; frontier_owner_ = &owner;
}
GalacticFunding::PlayerFrontier::~PlayerFrontier() {
    frontier_owner_ = previous_owner_; active_frontier_ = previous_frontier_;
}
GalacticFunding::SuspendIntents::SuspendIntents(GalacticFunding& owner) {
    if (frontier_owner_ == &owner) { selected_ = &owner.bound(); ++selected_->mutation_depth; }
}
GalacticFunding::SuspendIntents::~SuspendIntents() {
    if (selected_) --selected_->mutation_depth;
}
core::Result<void> GalacticFunding::prepare_player_frontiers(const sim::PartitionExecutor& executor,
    std::span<const sim::galactic::PlayerId> players, ReplaySink sink) {
    if (frontier_owner_ == this || !std::is_sorted(players.begin(), players.end())
        || std::adjacent_find(players.begin(), players.end()) != players.end()
        || std::any_of(players.begin(), players.end(), [&](auto player) { return !player || !root_.state_.contains(player); }))
        return invalid<void>("funding player frontiers require ordered initialized owners and the serial barrier");
    prepared_players_.clear(); prepared_hash_.clear();
    for (const auto player : players) frontiers_.try_emplace(player);
    auto prepared = executor.execute_phase("galactic-funding-frontiers", sim::tick_partition_count, [&](std::size_t partition) {
        const auto range = sim::partition_range(partition, players.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& frontier = frontiers_.at(players[index]);
            const std::lock_guard lock(frontier.mutex);
            frontier.state = root_;
            if (++frontier.generation == 0) std::terminate();
            frontier.state.private_generation = frontier.generation;
            if (sink) frontier.state.sink = [sink, player = players[index]](Replay replay) { sink(player, std::move(replay)); };
        }
    });
    if (!prepared) return prepared;
    prepared_players_.assign(players.begin(), players.end());
    prepared_hash_ = record_sync_hash(); prepared_revision_ = state_revision();
    return core::Result<void>::success();
}
core::Result<std::size_t> GalacticFunding::commit_player_frontiers() {
    if (frontier_owner_ == this || prepared_hash_.empty()
        || record_sync_hash() != prepared_hash_ || state_revision() != prepared_revision_)
        return invalid<std::size_t>("funding frontier commit requires its unchanged frozen root");
    for (const auto player : prepared_players_) if (frontiers_.at(player).state.sink)
        return invalid<std::size_t>("funding intents belong to their joint decision journal");
    // Hold every physical row until the ordered publication has consumed all
    // intents. Refused operations retain the preceding root; none are retried.
    std::vector<std::unique_lock<std::recursive_mutex>> locks;
    for (const auto player : prepared_players_) locks.emplace_back(frontiers_.at(player).mutex);
    std::size_t refused{};
    for (const auto player : prepared_players_) {
        auto& operations = frontiers_.at(player).state.operations;
        for (const auto& replay : operations) if (!replay(*this)) ++refused;
        operations.clear();
    }
    prepared_players_.clear(); prepared_hash_.clear();
    return core::Result<std::size_t>::success(refused);
}
const funding::Ledger* GalacticFunding::find_ledger(sim::galactic::PlayerId player) const noexcept {
    const auto at = bound().state_.find(player);
    return at == bound().state_.end() ? nullptr : &at->second;
}
std::shared_ptr<const funding::Ledger> GalacticFunding::ledger(sim::galactic::PlayerId player) const {
    const auto* value = find_ledger(player);
    return value ? std::make_shared<const funding::Ledger>(*value) : nullptr;
}
core::Result<void> GalacticFunding::commit_rows(State changes, const std::function<core::Result<void>()>& publish) {
    // A world callback may capture a stack transaction. Never retain it, or
    // replay replacement rows that were calculated against another frontier.
    if (frontier_owner_ == this) return invalid<void>("native funding rows require typed frontier publication");
    if (!publish) return invalid<void>("native funding publication requires its world owner");
    for (const auto& [player, ledger] : changes) {
        if (!bound().state_.contains(player)) return invalid<void>("native funding publication names an unknown player");
        auto valid = funding::category_order(ledger);
        if (!valid) return core::Result<void>::failure(valid.error());
    }
    if (auto published = publish(); !published) return published;
    if (++bound().revision_ == 0) std::terminate();
    for (auto& [player, ledger] : changes) {
        auto previous = std::move(bound().state_.at(player)); bound().state_.at(player) = std::move(ledger);
        galactic_ai::update_funding_records(bound().player_records_.at(player).records, player, &previous, &bound().state_.at(player));
    }
    return core::Result<void>::success();
}
core::Result<GalacticFunding::PrivateRows> GalacticFunding::prepare_private_rows(State changes) const {
    if (!has_player_frontier()) return invalid<PrivateRows>("private funding rows require a selected player frontier");
    for (const auto& [player, ledger] : changes) {
        if (!bound().state_.contains(player)) return invalid<PrivateRows>("private funding rows name an unknown player");
        auto valid = funding::category_order(ledger);
        if (!valid) return core::Result<PrivateRows>::failure(valid.error());
    }
    return core::Result<PrivateRows>::success(PrivateRows(active_frontier_, bound().private_generation, state_revision(), std::move(changes)));
}
core::Result<void> GalacticFunding::publish_private_rows(PrivateRows prepared) {
    if (!has_player_frontier() || prepared.selected_ != active_frontier_
        || prepared.generation_ != bound().private_generation || prepared.revision_ != state_revision())
        return invalid<void>("private funding rows require their unchanged originating frontier");
    // The typed native intent already represents these callback-derived rows.
    // Neither replacement ledgers nor stack publication callbacks enter replay.
    if (++bound().revision_ == 0) std::terminate();
    for (auto& [player, ledger] : prepared.changes_) {
        auto previous = std::move(bound().state_.at(player)); bound().state_.at(player) = std::move(ledger);
        galactic_ai::update_funding_records(bound().player_records_.at(player).records, player, &previous, &bound().state_.at(player));
    }
    return core::Result<void>::success();
}
core::Result<void> GalacticFunding::restore(State state) {
    if (frontier_owner_ == this) return invalid<void>("funding restore requires the serial boundary");
    if (state.constructor_income != funding::Amount{} || state.timeline < 0 || state.timeline > 5)
        return invalid<void>("invalid retained funding constructor state");
    for (const auto& [player, value] : state) {
        if (!player) return invalid<void>("funding state has an invalid player");
        auto valid = funding::category_order(value);
        if (!valid) return core::Result<void>::failure(valid.error());
    }
    bound().revision_ = state_revision() + 1;
    if (bound().revision_ == 0) std::terminate();
    bound().state_ = std::move(state); bound().player_records_.clear(); bound().record_digest_ = {};
    galactic_ai::update_funding_metadata(bound().record_digest_, bound().state_);
    for (const auto& [player, ledger] : bound().state_)
        galactic_ai::update_funding_records(bound().player_records_[player].records, player, nullptr, &ledger);
    return core::Result<void>::success();
}
core::Result<void> GalacticFunding::commit(sim::galactic::PlayerId player, core::Result<funding::Ledger> result) {
    if (!result) return core::Result<void>::failure(result.error());
    if (++bound().player_records_.at(player).revision == 0) std::terminate();
    auto previous = std::move(bound().state_.at(player)); bound().state_.at(player) = std::move(result).value();
    galactic_ai::update_funding_records(bound().player_records_.at(player).records, player, &previous, &bound().state_.at(player));
    return core::Result<void>::success();
}
core::Result<funding::Amount> GalacticFunding::commit_transfer(sim::galactic::PlayerId player,
    core::Result<funding::TransferResult> result) {
    if (!result) return core::Result<funding::Amount>::failure(result.error());
    auto value = std::move(result).value();
    if (++bound().player_records_.at(player).revision == 0) std::terminate();
    auto previous = std::move(bound().state_.at(player)); bound().state_.at(player) = std::move(value.ledger);
    galactic_ai::update_funding_records(bound().player_records_.at(player).records, player, &previous, &bound().state_.at(player));
    return core::Result<funding::Amount>::success(value.credits);
}
core::Result<void> GalacticFunding::set_timeline(std::int32_t timeline) {
    return mutate([&] { return set_timeline_impl(timeline); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.set_timeline(timeline);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::set_timeline_impl(std::int32_t timeline) {
    if (timeline < 0 || timeline > 5) return invalid<void>("invalid retained timeline input");
    if (++bound().revision_ == 0) std::terminate();
    bound().state_.timeline = timeline; galactic_ai::update_funding_metadata(bound().record_digest_, bound().state_);
    return core::Result<void>::success();
}
core::Result<void> GalacticFunding::initialize(sim::galactic::PlayerId player,
    std::span<const EvaluatedFundingTemplate> templates, funding::Amount credits) {
    return mutate([&] { return initialize_impl(player, templates, credits); }, [&] {
        auto owned = std::vector<EvaluatedFundingTemplate>(templates.begin(), templates.end());
        return [=](GalacticFunding& target) {
            auto result = target.initialize(player, owned, credits);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::initialize_impl(sim::galactic::PlayerId player,
    std::span<const EvaluatedFundingTemplate> templates, funding::Amount credits) {
    if (!player || bound().state_.contains(player)) return invalid<void>("funding player already initialized or invalid");
    funding::Ledger value;
    for (const auto& input : templates) {
        auto changed = apply(std::move(value), input);
        if (!changed) return core::Result<void>::failure(changed.error());
        value = std::move(changed).value();
    }
    auto initialized = funding::initialize(std::move(value), credits);
    if (!initialized) return core::Result<void>::failure(initialized.error());
    auto& slice = bound().player_records_[player];
    if (++slice.revision == 0) std::terminate();
    bound().state_.emplace(player, std::move(initialized).value());
    galactic_ai::update_funding_records(slice.records, player, nullptr, &bound().state_.at(player));
    return core::Result<void>::success();
}
core::Result<void> GalacticFunding::apply_template(sim::galactic::PlayerId player, const EvaluatedFundingTemplate& input) {
    return mutate([&] { return apply_template_impl(player, input); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.apply_template(player, input);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::apply_template_impl(sim::galactic::PlayerId player, const EvaluatedFundingTemplate& input) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, apply(*value, input));
}
core::Result<void> GalacticFunding::begin_maintenance(sim::galactic::PlayerId player,
    std::span<const EvaluatedFundingWeight> weights) {
    return mutate([&] { return begin_maintenance_impl(player, weights); }, [&] {
        auto owned = std::vector<EvaluatedFundingWeight>(weights.begin(), weights.end());
        return [=](GalacticFunding& target) {
            auto result = target.begin_maintenance(player, owned);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::begin_maintenance_impl(sim::galactic::PlayerId player,
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
    return mutate([&] { return clear_equations_impl(player); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.clear_equations(player);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::clear_equations_impl(sim::galactic::PlayerId player) {
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
    return mutate([&] { return notify_impl(input); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.notify(input);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::notify_impl(const sim::galactic::GalacticSession::CreditNotification& input) {
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
    return mutate([&] { return reserve_impl(player, request); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.reserve(player, request);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::reserve_impl(sim::galactic::PlayerId player, const funding::GoalRequest& request) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::reserve(*value, request));
}
core::Result<void> GalacticFunding::mark_spending(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) {
    return mutate([&] { return mark_spending_impl(player, goal, amount); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.mark_spending(player, goal, amount);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::mark_spending_impl(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::mark_spending(*value, goal, amount));
}
core::Result<void> GalacticFunding::spend(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) {
    return mutate([&] { return spend_impl(player, goal, amount); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.spend(player, goal, amount);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::spend_impl(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::spend(*value, goal, amount));
}
core::Result<void> GalacticFunding::remove_goal(sim::galactic::PlayerId player, funding::GoalId goal) {
    return mutate([&] { return remove_goal_impl(player, goal); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.remove_goal(player, goal);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::remove_goal_impl(sim::galactic::PlayerId player, funding::GoalId goal) {
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
    return mutate([&] { return allocate_resources_impl(player, goal, amount); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.allocate_resources(player, goal, amount);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::allocate_resources_impl(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::allocate_resources(*value, goal, amount));
}
core::Result<void> GalacticFunding::release_resources(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) {
    return mutate([&] { return release_resources_impl(player, goal, amount); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.release_resources(player, goal, amount);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::release_resources_impl(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount amount) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::release_resources(*value, goal, amount));
}
core::Result<void> GalacticFunding::remove_goal_salvage(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount selected) {
    return mutate([&] { return remove_goal_salvage_impl(player, goal, selected); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.remove_goal_salvage(player, goal, selected);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::remove_goal_salvage_impl(sim::galactic::PlayerId player, funding::GoalId goal, funding::Amount selected) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::remove_goal_salvage(*value, goal, selected));
}
core::Result<void> GalacticFunding::flush_category(sim::galactic::PlayerId player, funding::CategoryId category) {
    return mutate([&] { return flush_category_impl(player, category); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.flush_category(player, category);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<void> GalacticFunding::flush_category_impl(sim::galactic::PlayerId player, funding::CategoryId category) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<void>("funding player is not initialized");
    return commit(player, funding::flush_category(*value, category));
}
core::Result<funding::Amount> GalacticFunding::enter_tactical(sim::galactic::PlayerId player, funding::TacticalTransferRequest request) {
    return mutate([&] { return enter_tactical_impl(player, request); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.enter_tactical(player, request);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<funding::Amount> GalacticFunding::enter_tactical_impl(sim::galactic::PlayerId player, funding::TacticalTransferRequest request) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<funding::Amount>("funding player is not initialized");
    return commit_transfer(player, funding::enter_tactical(*value, request));
}
core::Result<funding::Amount> GalacticFunding::release_tactical(sim::galactic::PlayerId player, funding::TacticalReleaseRequest request) {
    return mutate([&] { return release_tactical_impl(player, request); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.release_tactical(player, request);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<funding::Amount> GalacticFunding::release_tactical_impl(sim::galactic::PlayerId player, funding::TacticalReleaseRequest request) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<funding::Amount>("funding player is not initialized");
    return commit_transfer(player, funding::release_tactical(*value, request));
}
core::Result<funding::Amount> GalacticFunding::restore_strategic(sim::galactic::PlayerId player, funding::Amount credits) {
    return mutate([&] { return restore_strategic_impl(player, credits); }, [&] {
        return [=](GalacticFunding& target) {
            auto result = target.restore_strategic(player, credits);
            return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
        };
    });
}
core::Result<funding::Amount> GalacticFunding::restore_strategic_impl(sim::galactic::PlayerId player, funding::Amount credits) {
    const auto* value = find_ledger(player);
    if (!value) return invalid<funding::Amount>("funding player is not initialized");
    return commit_transfer(player, funding::restore_strategic(*value, credits));
}
} // namespace eawr::script::foc::ai
