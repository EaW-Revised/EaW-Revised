#include "galactic_ai_actions.hpp"
#include "galactic_ai_economic_replay.hpp"
#include "ai_engine.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace eawr::script::foc::galactic_ai {
namespace {
template<class T> core::Result<T> invalid(std::string message) {
    core::Diagnostic error; error.code = std::string(authoritative::codes::invalid_request);
    error.message = std::move(message); return core::Result<T>::failure(std::move(error));
}
constexpr std::string_view component = "galactic-production";
void word(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
template<class Row> void write_purchase(std::vector<std::uint8_t>& bytes, std::uint64_t id, const Row& row) {
    word(bytes, id); word(bytes, row.player); word(bytes, row.goal);
    word(bytes, static_cast<std::uint64_t>(row.price.raw())); word(bytes, static_cast<std::uint64_t>(row.billed.raw()));
    word(bytes, static_cast<std::uint64_t>(row.marked.raw())); word(bytes, static_cast<std::uint64_t>(row.remaining.raw())); word(bytes, row.orphan);
    word(bytes, row.completed); word(bytes, row.failed); word(bytes, row.object);
    word(bytes, row.producer); word(bytes, row.requested_type); word(bytes, static_cast<std::uint64_t>(row.channel));
    word(bytes, row.task_finished); word(bytes, row.salvage);
}
template<class Row> void write_movement(std::vector<std::uint8_t>& bytes, std::uint64_t id, const Row& row) {
    word(bytes, id); word(bytes, row.player); word(bytes, row.actor); word(bytes, row.destination);
    word(bytes, row.conflict_location); word(bytes, static_cast<std::uint64_t>(row.reach));
    word(bytes, row.taskforce); word(bytes, row.finished); word(bytes, row.advance_requested);
    word(bytes, row.observed_tick); word(bytes, row.event_cursor);
    word(bytes, row.plan_id); word(bytes, row.taskforce_id); word(bytes, row.block_id); word(bytes, row.registered);
    word(bytes, row.freestore_actor);
}

}
thread_local const NativeActions* NativeActions::executor_owner_{};
thread_local const sim::PartitionExecutor* NativeActions::scoped_executor_{};
NativeActions::ExecutorScope::ExecutorScope(const NativeActions* owner, const sim::PartitionExecutor* executor) noexcept
    : previous_owner_(executor_owner_), previous_executor_(scoped_executor_) {
    executor_owner_ = owner; scoped_executor_ = executor;
}
NativeActions::ExecutorScope::~ExecutorScope() {
    executor_owner_ = previous_owner_; scoped_executor_ = previous_executor_;
}
thread_local const NativeActions* NativeActions::frontier_owner_ = nullptr;
thread_local NativeActions::BoundState* NativeActions::active_frontier_ = nullptr;
NativeActions::PlayerFrontier::PlayerFrontier(NativeActions& owner, gc::PlayerId player)
    : previous_owner_(frontier_owner_), previous_frontier_(active_frontier_) {
    if (!std::binary_search(owner.prepared_players_.begin(), owner.prepared_players_.end(), player))
        throw std::logic_error("native action player frontier was not prepared at the barrier");
    auto& selected = owner.frontiers_.at(player);
    lock_ = std::unique_lock<std::recursive_mutex>(selected.mutex);
    active_frontier_ = &selected.state; frontier_owner_ = &owner;
}
NativeActions::PlayerFrontier::~PlayerFrontier() {
    frontier_owner_ = previous_owner_; active_frontier_ = previous_frontier_;
}
core::Result<void> NativeActions::prepare_player_frontiers(const sim::PartitionExecutor& executor,
    std::span<const gc::PlayerId> players) {
    if (frontier_owner_ == this || pending_admission() || !std::is_sorted(players.begin(), players.end())
        || std::adjacent_find(players.begin(), players.end()) != players.end()
        || std::any_of(players.begin(), players.end(), [&](auto player) { return !player || !root_.player_scratch_.contains(player); }))
        return invalid<void>("native action frontiers require ordered admitted owners and the serial barrier");
    prepared_players_.clear();
    for (const auto player : players) frontiers_.try_emplace(player);
    auto prepared = executor.execute_phase("galactic-action-frontiers", sim::tick_partition_count, [&](std::size_t partition) {
        const auto range = sim::partition_range(partition, players.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& selected = frontiers_.at(players[index]);
            const std::lock_guard lock(selected.mutex);
            selected.state = root_;
            selected.state.freestore_ = std::make_shared<QualifiedFreestore>(root_.freestore_->clone(
                [lifetime = lifetime_] { if (*lifetime) (*lifetime)->mark_state_changed(); }, true));
        }
    });
    if (!prepared) return prepared;
    prepared_players_.assign(players.begin(), players.end());
    return core::Result<void>::success();
}
NativeActions::NativeActions(gc::GalacticSession& world, std::shared_ptr<ai::GalacticFunding> funding,
    Placement placement, std::map<gc::PlayerId, sim::math::Fixed> multipliers, GoalResolver resolver, const sim::PartitionExecutor* executor, BaseCost base_cost)
    : world_(world), funding_(std::move(funding)), placement_(std::move(placement)), multipliers_(std::move(multipliers)), goal_resolver_(std::move(resolver)), base_cost_(std::move(base_cost)), executor_(executor), lifetime_(std::make_shared<NativeActions*>(this)) {
    bound().freestore_ = std::make_shared<QualifiedFreestore>([lifetime = lifetime_] { if (*lifetime) (*lifetime)->mark_state_changed(); });
    for (const auto& player : world_.setup().players) bound().player_scratch_.try_emplace(player.player_id);
    update_metadata_record();
}
bool NativeActions::pending_admission() const noexcept {
    return std::any_of(bound().player_scratch_.begin(), bound().player_scratch_.end(), [](const auto& row) { return row.second.pending.has_value(); });
}
std::span<const sim::EntityId> NativeActions::created(gc::PlayerId player) const noexcept {
    const auto at = bound().player_scratch_.find(player);
    return at == bound().player_scratch_.end() ? std::span<const sim::EntityId>{} : std::span<const sim::EntityId>{at->second.created};
}
NativeActions::~NativeActions() {
    // A retired provider cannot clear a newer restore's handlers. Old closures
    // fail closed and never retain/dereference its destroyed native owner.
    *lifetime_ = nullptr;
}
void NativeActions::bind_handlers() {
    if (actor_changes_) world_.set_native_actor_changes_handler([lifetime = lifetime_](const auto& snapshot, auto changes) {
        if (changes.empty()) return core::Result<void>::success();
        if (!*lifetime || !(*lifetime)->actor_changes_)
            return invalid<void>("native actor lifecycle owner must be rebound before service");
        return (*lifetime)->actor_changes_(**lifetime, snapshot, changes);
    });
    world_.set_native_production_outcome_handler([lifetime = lifetime_](const auto& input) {
        if (!*lifetime) return;
        const auto found = (*lifetime)->bound().purchases_.find(input.build);
        if (found == (*lifetime)->bound().purchases_.end() || found->second.player != input.player) return;
        (*lifetime)->mark_build_state_changed();
        found->second.completed = input.completed;
        found->second.failed = !input.completed;
        found->second.object = input.object; (*lifetime)->update_record(1, input.build);
    });
    world_.set_native_production_handler([lifetime = lifetime_](const auto& input) {
        return *lifetime ? (*lifetime)->notice(input) : invalid<gc::GalacticSession::NativeProductionPayment>("native producer owner must be rebound before service");
    });
    world_.set_credit_notification_handler([lifetime = lifetime_](const auto& input) {
        return *lifetime && (*lifetime)->funding_ ? (*lifetime)->funding_->notify(input) : invalid<void>("native cash requires the rebound funding owner");
    });
}
bool NativeActions::supports(ActionKind kind) const noexcept {
    return kind == ActionKind::produce || kind == ActionKind::magic || kind == ActionKind::give_money
        || ((kind == ActionKind::land || kind == ActionKind::launch || kind == ActionKind::regroup) && admission_pending_)
        || (kind == ActionKind::invade && admission_pending_ && world_.native_invasion_available())
        || (kind == ActionKind::move && movement_provider_ && conflict_pending_ && no_hop_ && admission_pending_);
}
const gc::GalacticSession::NativeInvasion* NativeActions::invasion_status(std::uint64_t request) const noexcept {
    return world_.native_invasion(request);
}
core::Result<void> NativeActions::retire_invasion(std::uint64_t request) { return world_.retire_native_invasion(request); }
core::Result<gc::GalacticSession::NativeProductionPayment> NativeActions::notice(const gc::GalacticSession::NativeProductionNotice& input) {
    auto result = notice_unrecorded(input);
    if (result && input.kind == gc::GalacticSession::NativeProductionNotice::Kind::billed) {
        const auto recorded = world_.append_native_effect(gc::NativeEffectKind::production_payment,
            economic_replay::payment(input, result.value()), gc::NativeEffectPhase::world_service);
        if (!recorded) return core::Result<gc::GalacticSession::NativeProductionPayment>::failure(recorded.error());
    }
    return result;
}
core::Result<gc::GalacticSession::NativeProductionPayment> NativeActions::notice_unrecorded(const gc::GalacticSession::NativeProductionNotice& input) {
    RecordWrite record_write{*this, 1, input.build};
    using Payment = gc::GalacticSession::NativeProductionPayment;
    using Result = core::Result<Payment>;
    if (!funding_) return invalid<Payment>("native billing requires the funding owner");
    if (input.kind == gc::GalacticSession::NativeProductionNotice::Kind::marked) {
        const auto scratch = bound().player_scratch_.find(input.player);
        if (scratch == bound().player_scratch_.end()) return invalid<Payment>("native mark has no unique goal/request mapping");
        const auto& pending_ = scratch->second.pending;
        const auto pending_goal_ = scratch->second.pending_goal;
        if (!pending_ || pending_->player != input.player || !pending_goal_ || bound().purchases_.contains(input.build))
            return invalid<Payment>("native mark has no unique goal/request mapping");
        const auto snapshot = world_.snapshot();
        const auto& production = snapshot->production();
        if (!production) return invalid<Payment>("accepted build requires its actual production owner");
        const auto location = std::lower_bound(production->planets.begin(), production->planets.end(), pending_->target,
            [](const auto& row, auto id) { return row.planet < id; });
        if (location == production->planets.end() || location->planet != pending_->target) return invalid<Payment>("accepted build producer is absent");
        const auto offer = std::lower_bound(location->offers.begin(), location->offers.end(), pending_->type,
            [](const auto& row, auto id) { return row.type < id; });
        if (offer == location->offers.end() || offer->type != pending_->type) return invalid<Payment>("accepted build type/channel is absent");
        if (auto marked = funding_->mark_spending(input.player, pending_goal_, input.amount); !marked) return Result::failure(marked.error());
        mark_build_state_changed();
        bound().purchases_.emplace(input.build, Purchase{input.player, pending_goal_, input.price, {}, input.amount, input.remaining, false,
            false, false, 0, pending_->target, pending_->type, offer->domain});
        return Result::success({true, input.player, {}});
    }
    auto at = bound().purchases_.find(input.build);
    if (at == bound().purchases_.end() || at->second.player != input.player || input.amount.raw() < 0)
        return invalid<Payment>("native bill has no reconstructed build/goal mapping");
    auto& row = at->second;
    const auto ledger = funding_->ledger(row.player);
    if (!ledger) return invalid<Payment>("native bill requires its ledger");
    auto charge = input.amount; auto payer = row.player;
    if (!row.orphan) {
        const auto goal = ledger->goals.find(row.goal);
        if (goal == ledger->goals.end()) return Result::success({false, payer, {}});
        const auto held = sim::math::add(goal->second.capital, goal->second.accumulated);
        if (!held) return Result::failure(held.error());
        const auto spendable = std::min(held.value(), goal->second.cost);
        const auto difference = sim::math::subtract(spendable, charge);
        if (!difference) return Result::failure(difference.error());
        if (difference.value().trunc_to_integer() < 0) {
            // WGA-157: a live plan quotes completion cost, independently of its
            // spendable capital/income. Only that quote's shortage adds a mark.
            if (goal->second.cost < charge) {
                const auto shortage = sim::math::subtract(charge, goal->second.cost);
                if (!shortage) return Result::failure(shortage.error());
                const auto marked = sim::math::add(row.marked, shortage.value());
                if (!marked) return Result::failure(marked.error());
                auto result = funding_->mark_spending(row.player, row.goal, shortage.value());
                if (!result) return Result::failure(result.error());
                mark_build_state_changed();
                row.marked = marked.value();
            }
            return Result::success({false, payer, {}});
        }
        charge = std::min(charge, spendable);
    } else {
        const auto players = world_.snapshot()->production()->players;
        const auto owner = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.player == input.planet_owner && !p.human; });
        if (owner == players.end() || !funding_->ledger(input.planet_owner)) return Result::success({false, input.planet_owner, {}});
        payer = input.planet_owner;
    }
    auto total = sim::math::add(row.billed, charge);
    if (!total || total.value() > row.price) return invalid<Payment>("native bill exceeds accepted price");
    if (auto spent = funding_->spend(payer, row.orphan ? 0 : row.goal, charge); !spent) return Result::failure(spent.error());
    mark_build_state_changed();
    row.billed = total.value(); row.remaining = input.remaining;
    return Result::success({true, payer, charge});
}
core::Result<void> NativeActions::finish_task(sim::EntityId build, bool salvage) {
    RecordWrite record_write{*this, 1, build};
    const auto at = bound().purchases_.find(build);
    if (at == bound().purchases_.end()) return invalid<void>("finished task requires its accepted build mapping");
    auto recipient = at->second.player;
    for (const auto& producer : world_.snapshot()->production()->planets) for (const auto& queue : producer.queues)
        if (std::any_of(queue.begin(), queue.end(), [&](const auto& row) { return row.build_id == build; }))
            for (const auto& planet : world_.snapshot()->planets()) if (planet.id == producer.planet) recipient = planet.owner;
    const auto multiplier = multipliers_.find(recipient);
    if (multiplier == multipliers_.end()) return invalid<void>("finished task requires its refund multiplier");
    auto result = world_.finish_native_production_task(build, salvage, multiplier->second);
    if (!result) return result;
    if (auto recorded = world_.append_native_effect(gc::NativeEffectKind::production_finish,
        economic_replay::finish(build, salvage, multiplier->second)); !recorded) return recorded;
    mark_build_state_changed();
    at->second.orphan = true; // task signal itself never clears a mark or refunds
    at->second.task_finished = true; at->second.salvage = salvage;
    return core::Result<void>::success();
}
core::Result<ai::GalacticBuildStatus> NativeActions::build_status(gc::PlayerId player, sim::EntityId build) const {
    const auto found = bound().purchases_.find(build);
    if (found == bound().purchases_.end() || found->second.player != player) return invalid<ai::GalacticBuildStatus>("unknown retained native build identity");
    const auto& row = found->second;
    return core::Result<ai::GalacticBuildStatus>::success({row.failed ? ai::GalacticBuildStatus::State::failed
        : row.completed ? ai::GalacticBuildStatus::State::completed : ai::GalacticBuildStatus::State::waiting, row.object});
}
core::Result<sim::math::Fixed> NativeActions::generic_build_cost(gc::TypeId type) const {
    return base_cost_ ? base_cost_(type) : invalid<sim::math::Fixed>("BTP-U03: generic integer build cost unavailable");
}
core::Result<sim::EntityId> NativeActions::build_actor(gc::PlayerId player, sim::EntityId build) const {
    const auto found = bound().purchases_.find(build);
    if (found == bound().purchases_.end() || found->second.player != player || !found->second.completed || !found->second.object)
        return invalid<sim::EntityId>("produced assignment requires its completed retained build result");
    const auto object = found->second.object;
    const auto snapshot = world_.snapshot();
    const auto fleet = std::lower_bound(snapshot->fleets().begin(), snapshot->fleets().end(), object,
        [](const auto& row, auto id) { return row.id < id; });
    if (fleet != snapshot->fleets().end() && fleet->id == object) {
        if (fleet->owner != player || fleet->members.empty()) return invalid<sim::EntityId>("produced fleet requires its recorded builder and first direct member");
        return core::Result<sim::EntityId>::success(fleet->members.front().id); // WGA-350: keep the company/squadron shell intact
    }
    return core::Result<sim::EntityId>::success(object);
}
core::Result<NativeActions::BuildHead> NativeActions::build_head(gc::PlayerId player, sim::EntityId build) const {
    const auto found = bound().purchases_.find(build);
    if (found == bound().purchases_.end() || found->second.player != player) return invalid<BuildHead>("build head requires its retained player/build association");
    const auto& purchase = found->second;
    const auto snapshot = world_.snapshot(); const auto& production = snapshot->production();
    if (!production) return invalid<BuildHead>("build head production support is unavailable");
    const auto planet = std::lower_bound(production->planets.begin(), production->planets.end(), purchase.producer,
        [](const auto& row, auto id) { return row.planet < id; });
    if (planet == production->planets.end() || planet->planet != purchase.producer) return invalid<BuildHead>("build head producer is unavailable");
    BuildHead result; result.producer = purchase.producer; result.channel = purchase.channel;
    const auto& queue = planet->queues[static_cast<std::size_t>(purchase.channel)];
    if (!queue.empty()) {
        const auto& entry = queue.front(); result.entry = entry;
        if (entry.full_seconds.raw() <= 0) return invalid<BuildHead>("build head progress duration is unqualified");
        result.progress = std::clamp(ai::to_single(ai::real(1) - ai::to_single(numeric::from_fixed(entry.last_remaining)
            / numeric::from_fixed(entry.full_seconds))), ai::Real{}, ai::real(1));
        const auto frame = snapshot->clock().galactic_frame;
        const auto elapsed = frame > entry.start_frame ? frame - entry.start_frame : 0;
        const auto frames = elapsed >= entry.duration_frames ? 0 : entry.duration_frames - elapsed;
        const auto seconds = sim::math::Fixed::from_ratio(static_cast<std::int64_t>(frames), gc::galactic_frames_per_second);
        if (!seconds) return core::Result<BuildHead>::failure(seconds.error());
        result.remaining_seconds = ai::to_single(numeric::from_fixed(seconds.value()));
    }
    return core::Result<BuildHead>::success(std::move(result));
}
core::Result<sim::math::Fixed> NativeActions::build_remaining_cost(gc::PlayerId player, sim::EntityId build) const {
    const auto found = bound().purchases_.find(build);
    if (found == bound().purchases_.end() || found->second.player != player) return invalid<sim::math::Fixed>("remaining cost requires its retained player/build association");
    if (found->second.object) return core::Result<sim::math::Fixed>::success({});
    if (!world_.snapshot()->production()) return generic_build_cost(found->second.requested_type); // WGA-344 support-missing fallback
    const auto head = build_head(player, build);
    if (!head) return core::Result<sim::math::Fixed>::failure(head.error());
    if (!head.value().entry) return core::Result<sim::math::Fixed>::success({});
    return numeric::to_fixed(ai::to_single(ai::to_single(ai::real(1) - head.value().progress)
        * ai::to_single(numeric::from_fixed(head.value().entry->price))));
}
core::Result<ActionReceipt> NativeActions::admit(const ActionRequest& request) {
    using Result = core::Result<ActionReceipt>;
    const auto scratch = bound().player_scratch_.find(request.player);
    if (scratch != bound().player_scratch_.end()) scratch->second.created.clear();
    if (!supports(request.kind)) {
        auto error = invalid<ActionReceipt>("native action requires its movement/landing owner").error();
        error.code = std::string(authoritative::codes::missing_api);
        return Result::failure(std::move(error));
    }
    if (scratch == bound().player_scratch_.end()) return invalid<ActionReceipt>("native action requires a selected player");
    auto& created_ = scratch->second.created;
    if (pending_admission()) return invalid<ActionReceipt>("native actions require ordered nonreentrant admission");
    if (request.kind == ActionKind::move) return admit_movement(request);
    if (request.kind == ActionKind::invade) {
        auto invaded = world_.admit_native_invasion(request.player, request.object, admission_pending_());
        if (!invaded) return Result::failure(invaded.error());
        return Result::success({invaded.value().admitted, invaded.value().request});
    }
    if (request.kind == ActionKind::land) {
        auto landed = world_.land_native_transports(request.player, request.object, admission_pending_());
        if (!landed) return Result::failure(landed.error());
        return Result::success({landed.value().admitted, landed.value().object});
    }
    if (request.kind == ActionKind::launch && !request.members.empty()) {
        gc::FleetId target = request.object;
        bool tracked{};
        for (const auto member : request.members) {
            const auto snapshot = world_.snapshot();
            if (!snapshot->control() || !snapshot->fleet_control()) return invalid<ActionReceipt>("TaskForce launch requires real containment inputs");
            const gc::SurfaceForce* landed{};
            for (const auto& planet : snapshot->control()->planets) for (const auto& force : planet.landed)
                if (force.id == member) landed = &force;
            if (!landed) continue;
            const auto& traits = snapshot->fleet_control()->types;
            const auto trait = std::find_if(traits.begin(), traits.end(), [&](const auto& row) { return row.type == landed->type; });
            if (landed->owner != request.player || trait == traits.end() || !trait->transport) continue;
            // WGA-486: each member launches at its own parent; the retained
            // result is not a target for another member's local launch.
            auto launched = world_.launch_native_transport(request.player, member, 0, admission_pending_());
            if (!launched) return Result::failure(launched.error());
            // WGA-227: a later blockade/refusal keeps earlier actual launches.
            if (!launched.value().admitted) return Result::success({false, 0});
            target = launched.value().object; tracked = true;
        }
        if (!target) for (const auto member : request.members)
            if (const auto fleet = containing_fleet(request.player, member)) { target = *fleet; break; }
        return Result::success({tracked || target != 0, target});
    }
    if (request.kind == ActionKind::launch) {
        auto launched = world_.launch_native_transport(request.player, request.object, request.target, admission_pending_());
        if (!launched) return Result::failure(launched.error());
        return Result::success({launched.value().admitted, launched.value().object});
    }
    if (request.kind == ActionKind::regroup && !request.members.empty()) {
        auto target = request.object;
        if (!target) for (const auto member : request.members)
            if (const auto fleet = containing_fleet(request.player, member)) { target = *fleet; break; }
        const auto snapshot = world_.snapshot();
        const auto retained = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& row) { return row.id == target; });
        if (retained == snapshot->fleets().end() || retained->owner != request.player || !retained->orbit
            || retained->motion.transit || retained->members.empty()) return Result::success({false, 0});
        std::vector<gc::FleetId> others;
        for (const auto member : request.members) if (const auto fleet = containing_fleet(request.player, member))
            if (*fleet != target && std::find(others.begin(), others.end(), *fleet) == others.end()) others.push_back(*fleet);
        for (const auto source : others) {
            auto merged = world_.admit_native_fleet(request.player, gc::MergeFleets{source, target}, admission_pending_());
            if (!merged) return Result::failure(merged.error());
            if (!merged.value().admitted) return Result::success({false, 0});
        }
        return Result::success({true, target});
    }
    if (request.kind == ActionKind::regroup) {
        auto merged = world_.admit_native_fleet(request.player, gc::MergeFleets{request.object, request.target}, admission_pending_());
        if (!merged) return Result::failure(merged.error());
        return Result::success({merged.value().admitted, merged.value().admitted ? request.target : 0});
    }
    if (request.kind == ActionKind::give_money) {
        const auto multiplier = multipliers_.find(request.player);
        if (multiplier == multipliers_.end()) return invalid<ActionReceipt>("cash requires the selected AI multiplier or explicit non-AI one");
        auto amount = numeric::to_fixed(request.amount);
        if (!amount) return Result::failure(amount.error());
        auto added = world_.add_credits(request.player, amount.value(), multiplier->second);
        if (!added) return Result::failure(added.error());
        if (auto recorded = world_.append_native_effect(gc::NativeEffectKind::cash,
            economic_replay::cash(request.player, amount.value(), multiplier->second, added.value())); !recorded) return Result::failure(recorded.error());
        return Result::success({true, 0});
    }
    const gc::GalacticSession::NativeProductionRequest input{request.player, request.target, request.type, {}};
    if (request.kind == ActionKind::produce) {
        const auto goal = goal_resolver_ ? goal_resolver_(request.player, request.plan) : std::nullopt;
        if (!goal) return invalid<ActionReceipt>("paid action requires D's live plan/goal identity resolver");
        return admit_paid(request, *goal);
    }
    if (!placement_) return invalid<ActionReceipt>("magic creation requires original placement inputs");
    auto made = world_.create_native_force(input, [&](const gc::GalacticSnapshot& snapshot, sim::EntityId frontier) {
        return placement_(request, snapshot, frontier);
    });
    if (!made) return Result::failure(made.error());
    if (auto recorded = world_.append_native_effect(gc::NativeEffectKind::magic,
        economic_replay::magic(request, made.value())); !recorded) return Result::failure(recorded.error());
    created_ = std::move(made).value();
    return Result::success({!created_.empty(), created_.empty() ? 0 : created_.front()});
}
core::Result<ActionReceipt> NativeActions::admit_paid(const ActionRequest& request, std::uint64_t goal) {
    using Result = core::Result<ActionReceipt>;
    if (pending_admission() || request.kind != ActionKind::produce || !funding_)
        return invalid<ActionReceipt>("paid admission requires an ordered request and its funding owner");
    const auto ledger = funding_->ledger(request.player);
    if (!ledger || !ledger->goals.contains(goal)) return invalid<ActionReceipt>("paid action requires the allocated goal identity");
    if (!base_cost_) return invalid<ActionReceipt>("paid mark requires the original null-location type cost provider");
    auto cost = base_cost_(request.type);
    if (!cost) return Result::failure(cost.error());
    const auto owner_scratch = bound().player_scratch_.find(request.player);
    if (owner_scratch == bound().player_scratch_.end()) return invalid<ActionReceipt>("paid action requires a selected player");
    auto& scratch = owner_scratch->second;
    auto& pending_ = scratch.pending;
    auto& pending_goal_ = scratch.pending_goal;
    pending_ = request; pending_goal_ = goal;
    const sim::InlineExecutor inline_executor;
    auto accepted = world_.admit_native_production({request.player, request.target, request.type, cost.value()}, current_executor() ? *current_executor() : inline_executor);
    pending_.reset(); pending_goal_ = 0;
    if (!accepted) return Result::failure(accepted.error());
    if (accepted.value().admission == gc::ProductionAdmission::allowed) {
        if (auto recorded = world_.append_native_effect(gc::NativeEffectKind::production,
            economic_replay::production({request.player, request.target, request.type, cost.value()}, accepted.value())); !recorded) return Result::failure(recorded.error());
    }
    return Result::success({accepted.value().admission == gc::ProductionAdmission::allowed, accepted.value().build});
}
bool NativeActions::movement_actor_exists(sim::EntityId actor) const {
    return std::any_of(world_.snapshot()->fleets().begin(), world_.snapshot()->fleets().end(), [&](const auto& fleet) { return fleet.id == actor; });
}
std::optional<gc::FleetId> NativeActions::containing_fleet(gc::PlayerId player, sim::EntityId actor) const {
    for (const auto& fleet : world_.snapshot()->fleets()) {
        if (fleet.owner != player) continue;
        if (fleet.id == actor || std::any_of(fleet.members.begin(), fleet.members.end(), [&](const auto& member) { return member.id == actor; }))
            return fleet.id;
    }
    return {};
}
std::optional<NativeActions::AssemblyLocation> NativeActions::assembly_location(gc::PlayerId player, sim::EntityId actor) const {
    const auto snapshot = world_.snapshot();
    for (const auto& fleet : snapshot->fleets()) if (fleet.owner == player
        && (fleet.id == actor || std::any_of(fleet.members.begin(), fleet.members.end(), [&](const auto& member) { return member.id == actor; })))
        return AssemblyLocation{fleet.orbit, fleet.id, false, fleet.motion.transit.has_value()};
    if (snapshot->control()) for (const auto& planet : snapshot->control()->planets)
        for (const auto& member : planet.landed) if (member.id == actor && member.owner == player)
            return AssemblyLocation{planet.planet, 0, true, false};
    return {};
}
core::Result<ActionReceipt> NativeActions::prepare_assembly_actor(gc::PlayerId player, sim::EntityId actor, bool separate) {
    using Result = core::Result<ActionReceipt>;
    if (!admission_pending_) return invalid<ActionReceipt>("assembly requires its native admission owner");
    auto at = assembly_location(player, actor);
    if (!at || at->transit || !at->planet) return Result::success({false, 0});
    if (at->landed) {
        auto launched = world_.launch_native_transport(player, actor, 0, admission_pending_());
        if (!launched) return Result::failure(launched.error());
        if (!launched.value().admitted) return Result::success({false, 0});
        at = assembly_location(player, actor);
    }
    if (!at || !at->fleet) return Result::success({false, 0});
    const auto snapshot = world_.snapshot();
    const auto fleet = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& row) { return row.id == at->fleet; });
    if (fleet == snapshot->fleets().end()) return Result::success({false, 0});
    if (separate && actor != fleet->id && fleet->members.size() > 1) {
        // Each ordinary existing actor leaves a multi-member fleet before its
        // common join/pathing route. Earlier real effects survive later refusal.
        auto removed = world_.admit_native_fleet(player, gc::RemoveFleetMembers{fleet->id, {actor}}, admission_pending_());
        if (!removed) return Result::failure(removed.error());
        if (!removed.value().admitted) return Result::success({false, 0});
        auto created = world_.admit_native_fleet(player, gc::CreateFleet{at->planet, {actor}, 0}, admission_pending_());
        if (!created) return Result::failure(created.error());
        if (!created.value().admitted) return Result::success({false, 0});
        at = assembly_location(player, actor);
    }
    if (!at || !at->fleet) return Result::success({false, 0});
    if (auto refreshed = refresh_freestore_movement(actor, at->fleet, false); !refreshed) return Result::failure(refreshed.error());
    return Result::success({true, at->fleet});
}
core::Result<void> NativeActions::land_assembly_arrival(gc::PlayerId player, sim::EntityId actor) {
    const auto at = assembly_location(player, actor);
    if (!at || !at->fleet || at->transit || !at->planet) return core::Result<void>::success();
    const auto snapshot = world_.snapshot();
    if (!snapshot->fleet_control()) return invalid<void>("assembly arrival requires real transport traits");
    const auto fleet = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& row) { return row.id == at->fleet; });
    if (fleet == snapshot->fleets().end()) return core::Result<void>::success();
    const auto member = std::find_if(fleet->members.begin(), fleet->members.end(), [&](const auto& row) { return row.id == actor; });
    if (member == fleet->members.end()) return core::Result<void>::success();
    const auto& traits = snapshot->fleet_control()->types;
    const auto type = std::find_if(traits.begin(), traits.end(), [&](const auto& row) { return row.type == member->type; });
    if (type == traits.end()) return invalid<void>("assembly arrival transport type is unavailable");
    if (!type->transport) return core::Result<void>::success();
    if (!snapshot->control() || !admission_pending_) return invalid<void>("assembly landing requires its control/admission owners");
    const auto planet = std::find_if(snapshot->planets().begin(), snapshot->planets().end(), [&](const auto& row) { return row.id == at->planet; });
    const auto& relations = snapshot->control()->relations;
    const bool allied = planet != snapshot->planets().end() && (planet->owner == player || std::any_of(relations.begin(), relations.end(),
        [&](const auto& row) { return row.player == player && row.other == planet->owner && row.allied; }));
    if (!allied) return core::Result<void>::success();
    const auto available = world_.native_transport_landing_available(player, at->planet);
    if (!available) return core::Result<void>::failure(available.error());
    if (available.value() != gc::GalacticSession::NativeTransportLandingGate::admitted) return core::Result<void>::success();
    auto removed = world_.admit_native_fleet(player, gc::RemoveFleetMembers{at->fleet, {actor}}, admission_pending_());
    if (!removed) return core::Result<void>::failure(removed.error());
    if (!removed.value().admitted) return core::Result<void>::success();
    auto landed = world_.land_native_transport(player, actor, at->planet, admission_pending_());
    if (!landed) return core::Result<void>::failure(landed.error());
    return core::Result<void>::success();
}
void NativeActions::bind_movement(MovementProvider provider, ConflictPending pending, NoHop no_hop, BribeHop bribe) {
    movement_provider_ = std::move(provider); conflict_pending_ = std::move(pending);
    no_hop_ = std::move(no_hop); bribe_hop_ = std::move(bribe);
    bind_movement_listener();
}
void NativeActions::bind_freestore_path(FreestorePathProvider provider, ConflictPending pending) {
    freestore_path_ = std::move(provider); conflict_pending_ = std::move(pending); bind_movement_listener();
}
void NativeActions::bind_movement_listener() {
    world_.set_native_movement_listener([token = lifetime_](const gc::GalacticEvent& event) {
        if (!*token) return;
        // Sparse listener records, at the ordered movement commit; no per-frame
        // scene walk or allocation, and no dependency on the poll's cadence.
        for (auto& [id, movement] : (*token)->bound().movements_) {
            (void)id;
            if (!movement.registered || movement.actor != event.subject) continue;
            const auto previous = movement;
            if (event.kind == gc::GalacticEventKind::movement_finished && (movement.freestore_actor || !movement.finished)) movement.advance_requested = true;
            if (event.kind == gc::GalacticEventKind::movement_canceled) {
                movement.finished = true; if (!movement.freestore_actor) movement.advance_requested = false;
            }
            if (movement != previous) { (*token)->mark_state_changed(); (*token)->update_record(2, id); }
        }
    });
}
core::Result<void> NativeActions::advance_movement(Movement& movement, bool taskforce_has_members) {
    if (movement.freestore_actor) return advance_freestore_movement(movement);
    if (!movement_provider_ || !conflict_pending_ || !no_hop_ || !admission_pending_)
        return invalid<void>("movement requires its rebuilt path, global conflict and orbit owners");
    const auto snapshot = world_.snapshot();
    const auto at = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& fleet) {
        return fleet.id == movement.actor;
    });
    const auto no_hop = [&]() -> core::Result<void> {
        return at != snapshot->fleets().end() && at->orbit && !at->motion.transit
            ? no_hop_(movement.player, movement.actor) : core::Result<void>::success();
    };
    // WGA-229: global conflict gate precedes actor/source/path terminal tests.
    if (conflict_pending_()) { movement.advance_requested = true; return no_hop(); }
    movement.advance_requested = false;
    if (movement.taskforce && !taskforce_has_members) { movement.finished = true; return no_hop(); }
    if (at == snapshot->fleets().end()) { movement.actor = 0; movement.finished = true; return core::Result<void>::success(); }
    if (at->owner != movement.player || at->members.empty() || !at->orbit || at->motion.transit
        || at->orbit == movement.destination) {
        movement.finished = true; return no_hop();
    }
    // GFD-A05/A07: path predicates read this player's preceding native effects.
    // Reuse the already materialized layer; no new world snapshot is requested.
    if (movement_frontier_) if (auto bound = movement_frontier_(movement.player, snapshot); !bound) return bound;
    auto copied = movement_provider_(movement.player, *snapshot);
    if (!copied) return core::Result<void>::failure(copied.error());
    if (!world_.graph()) return invalid<void>("movement has no ordinary world graph");
    const auto& input = copied.value();
    gc::PathOptions options;
    options.ai_player = input.ai_player; options.multiplayer = input.multiplayer;
    options.benefits_from_corruption = input.benefits_from_corruption;
    options.ai_restrictions = movement.taskforce;
    options.allow_hostile = options.allow_neutral = options.allow_defended = movement.reach == MovementReach::any;
    options.allow_bad_final = movement.reach == MovementReach::enemy_destination
        || movement.reach == MovementReach::enemy_undefended || movement.reach == MovementReach::single_hop_disconnected
        || movement.reach == MovementReach::any;
    options.allow_bad_if_no_force = movement.reach == MovementReach::enemy_undefended
        || movement.reach == MovementReach::single_hop_disconnected || movement.reach == MovementReach::any;
    auto path = gc::find_planet_path(*world_.graph(), at->orbit, movement.destination, input.access, options);
    if (!path) return core::Result<void>::failure(path.error());
    if (path.value().planets.size() < 2) { movement.finished = true; return no_hop(); }
    const auto next = path.value().planets[1];
    const auto occupant = input.orbital_occupants.find(next);
    if (occupant == input.orbital_occupants.end()) return invalid<void>("movement requires a complete orbital-occupant table");
    if (occupant->second && occupant->second != movement.player) {
        if (next != movement.destination && input.benefits_from_corruption) {
            // Known deviation (#3981): the AI bribe request and its billing have no world owner yet, so the move
            // ends before the occupied hop instead of faulting the simulation.
            if (!bribe_hop_) { movement.finished = true; return no_hop(); }
            if (auto bribe = bribe_hop_(movement.player, movement.actor, next); !bribe) return bribe;
        } else if (movement.reach == MovementReach::friendly_only || movement.reach == MovementReach::friendly_ignore_threat) {
            movement.finished = true; return no_hop();
        }
    }
    // WGA-228: one ordinary hop only. The next poll searches the new world again.
    auto admitted = world_.admit_native_fleet(movement.player, gc::MoveFleet{movement.actor, {at->orbit, next}, {}}, admission_pending_());
    if (!admitted) return core::Result<void>::failure(admitted.error());
    if (!admitted.value().admitted) movement.finished = true;
    return core::Result<void>::success();
}
core::Result<void> NativeActions::advance_freestore_movement(Movement& movement) {
    // WGA-385..388: cached execution route, fixed zero safety threshold and
    // exact next-orbit ownership; no ordinary path/bribe/no-hop callback.
    if (!freestore_path_ || !conflict_pending_ || !admission_pending_)
        return invalid<void>("direct freestore movement requires its rebuilt cache and conflict owners");
    if (conflict_pending_()) { movement.advance_requested = true; return core::Result<void>::success(); }
    movement.advance_requested = false;
    const auto snapshot = world_.snapshot();
    const auto at = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& fleet) { return fleet.id == movement.actor; });
    if (at == snapshot->fleets().end() || !at->orbit || at->motion.transit || at->orbit == movement.destination) {
        movement.finished = true; return core::Result<void>::success();
    }
    auto path = freestore_path_(movement.player, *snapshot, at->orbit, movement.destination);
    if (!path) return core::Result<void>::failure(path.error());
    const auto& input = path.value();
    if ((input.threat.repr & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL || input.threat < Number(0))
        return invalid<void>("cached freestore safety is unqualified");
    // WGA-134/386 clamps the range to 0..1000. WGA-385 initializes it to zero,
    // and WGA-389 omits it from the archive, preserving constructor-zero.
    if (input.threat > Number(0) || input.planets.size() < 2) {
        movement.finished = true; return core::Result<void>::success();
    }
    if (!input.next_occupant) {
        core::Diagnostic error; error.code = std::string(authoritative::codes::missing_api);
        error.message = std::string(freestore_move_reader_seam); return core::Result<void>::failure(std::move(error));
    }
    if (*input.next_occupant && *input.next_occupant != movement.player) {
        movement.finished = true; return core::Result<void>::success();
    }
    if (input.planets.front() != at->orbit || input.planets.back() != movement.destination
        || input.planets[1] == at->orbit || !world_.graph() || !world_.graph()->node(input.planets[1]))
        return invalid<void>("cached freestore route endpoints are unqualified");
    auto admitted = world_.admit_native_fleet(movement.player,
        gc::MoveFleet{movement.actor, {at->orbit, input.planets[1]}, {}}, admission_pending_());
    if (!admitted) return core::Result<void>::failure(admitted.error());
    if (!admitted.value().admitted) movement.finished = true;
    return core::Result<void>::success();
}
core::Result<ActionReceipt> NativeActions::admit_movement(const ActionRequest& request) {
    const auto snapshot = world_.snapshot();
    const auto at = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& fleet) { return fleet.id == request.object; });
    if (at == snapshot->fleets().end() || at->owner != request.player || at->members.empty()
        || !at->orbit || at->motion.transit || (request.taskforce && request.members.empty()))
        return core::Result<ActionReceipt>::success({false, 0});
    if (!world_.graph() || !world_.graph()->node(request.target) || request.reach > MovementReach::any
        || bound().next_movement_ == std::numeric_limits<std::uint64_t>::max())
        return invalid<ActionReceipt>("movement destination, policy or request identity is invalid");
    Movement movement; movement.player = request.player; movement.actor = request.object;
    movement.destination = request.target; movement.reach = request.reach; movement.taskforce = request.taskforce != 0;
    movement.plan_id = request.plan; movement.taskforce_id = request.taskforce; movement.block_id = request.block;
    movement.observed_tick = snapshot->completed_tick(); movement.event_cursor = snapshot->events().size();
    if (auto advanced = advance_movement(movement); !advanced) return core::Result<ActionReceipt>::failure(advanced.error());
    mark_state_changed();
    const auto id = bound().next_movement_++; bound().movements_.emplace(id, movement); update_record(2, id); update_metadata_record();
    return core::Result<ActionReceipt>::success({true, id});
}
core::Result<NativeActions::Movement> NativeActions::poll_movement(std::uint64_t request, bool taskforce_has_members) {
    RecordWrite record_write{*this, 2, request};
    const auto found = bound().movements_.find(request);
    if (found == bound().movements_.end() || !found->second.registered) return invalid<Movement>("movement poll has no registered request identity");
    auto& movement = found->second;
    // Bump only on retained writes, including a partial advance before refusal.
    struct RetainedWrite {
        NativeActions& owner; Movement& value; Movement previous;
        ~RetainedWrite() { if (value != previous) owner.mark_state_changed(); }
    } retained_write{*this, movement, movement};
    const auto snapshot = world_.snapshot();
    if (movement.observed_tick != snapshot->completed_tick()) {
        movement.observed_tick = snapshot->completed_tick(); movement.event_cursor = 0;
    }
    const auto events = snapshot->events();
    for (auto i = movement.event_cursor; i < events.size(); ++i) {
        const auto& event = events[static_cast<std::size_t>(i)];
        if (event.subject != movement.actor) continue;
        // Movement arrival/cancel are consumed exclusively by the ordered
        // listener. Replaying them here would undo a later conflict-begin clear.
        if (event.kind == gc::GalacticEventKind::fleet_destroyed) { movement.finished = true; movement.actor = 0; if (!movement.freestore_actor) movement.advance_requested = false; }
    }
    movement.event_cursor = events.size();
    if ((movement.freestore_actor || !movement.finished) && movement.advance_requested)
        if (auto advanced = advance_movement(movement, taskforce_has_members); !advanced) return core::Result<Movement>::failure(advanced.error());
    if (!movement.freestore_actor && movement.actor && conflict_pending_ && !conflict_pending_()
        && std::none_of(world_.snapshot()->fleets().begin(), world_.snapshot()->fleets().end(),
        [&](const auto& fleet) { return fleet.id == movement.actor; })) { movement.actor = 0; movement.finished = true; }
    return core::Result<Movement>::success(movement);
}
core::Result<void> NativeActions::movement_conflict(std::uint64_t request, gc::PlanetId location, bool begin) {
    RecordWrite record_write{*this, 2, request};
    const auto found = bound().movements_.find(request);
    if (found == bound().movements_.end() || !found->second.registered)
        return invalid<void>("movement conflict requires its registered request");
    if (found->second.freestore_actor) {
        auto& movement = found->second; const auto previous = movement;
        if (begin) movement.finished = movement.advance_requested = false;
        else movement.advance_requested = true; // WGA-388: space end has no location qualification
        if (movement != previous) mark_state_changed();
        return core::Result<void>::success();
    }
    if (!location || !found->second.actor)
        return invalid<void>("movement conflict requires its retained actor, request and signalled location");
    auto& movement = found->second;
    const auto previous = movement;
    if (begin) {
        movement.finished = movement.advance_requested = false; movement.conflict_location = location;
    } else {
        if (movement.conflict_location != location) return invalid<void>("movement conflict end does not match its listener location");
        movement.conflict_location = 0;
        for (const auto& fleet : world_.snapshot()->fleets())
            if (fleet.id == movement.actor && fleet.orbit == location) movement.advance_requested = true;
        // WGA-231: a retreat/delete return no longer owns a resumable actor at
        // the conflict location. Do not let its travel finish restart this block.
        if (!movement.advance_requested) { movement.finished = true; movement.actor = 0; }
    }
    if (movement != previous) mark_state_changed();
    return core::Result<void>::success();
}
core::Result<void> NativeActions::invalidate_freestore_actor(std::uint64_t request, gc::FleetId subject) {
    RecordWrite record_write{*this, 2, request};
    const auto found = bound().movements_.find(request);
    if (found == bound().movements_.end() || !found->second.registered || !found->second.freestore_actor
        || !subject || found->second.actor != subject)
        return invalid<void>("free actor notification requires its registered request and retained fleet subject");
    auto& movement = found->second; const auto previous = movement;
    movement.finished = true; movement.actor = 0; // WGA-388: preserve requested advance.
    if (movement != previous) mark_state_changed();
    return core::Result<void>::success();
}
core::Result<void> NativeActions::retire_movement(std::uint64_t request) {
    RecordWrite record_write{*this, 2, request};
    const auto found = bound().movements_.find(request);
    if (found == bound().movements_.end()) return invalid<void>("movement retirement has no retained request");
    if (found->second.registered) {
        found->second.registered = false; mark_state_changed();
        if (found->second.freestore_actor) {
            bound().freestore_requests_.erase(found->second.freestore_actor);
            if (found->second.actor) return refresh_freestore_movement(found->second.freestore_actor, found->second.actor, false);
            // WGA-307/388: retreat may clear the block's fleet while its
            // selected unit remains alive. Structural callbacks already own
            // its new parent/location; cleanup only clears the tracked bit.
            if (const auto* facts = bound().freestore_->facts(found->second.freestore_actor)) {
                auto row = *facts; row.tracked_movement = false;
                return bound().freestore_->update(std::move(row));
            }
            return core::Result<void>::success();
        }
    }
    return core::Result<void>::success();
}
core::Result<void> NativeActions::retire_freestore_movement(sim::EntityId actor, bool actor_lost) {
    const auto found = bound().freestore_requests_.find(actor);
    if (found == bound().freestore_requests_.end()) return core::Result<void>::success();
    const auto request = found->second;
    if (actor_lost && bound().movements_.at(request).actor)
        if (auto invalidated = invalidate_freestore_actor(request, bound().movements_.at(request).actor); !invalidated) return invalidated;
    return retire_movement(request);
}
core::Result<NativeActions::FreestoreMove> NativeActions::preflight_freestore_move(
    const QualifiedFreestore& store, gc::PlayerId player, sim::EntityId actor, gc::PlanetId destination) const {
    using Result = core::Result<FreestoreMove>;
    // WGA-313: management gate first, reservation is deliberately not read.
    if (world_.clock().pause_depth) return Result::success(FreestoreMove::refused);
    if (!admission_pending_) return invalid<FreestoreMove>("freestore movement requires its galactic conflict admission owner");
    if (admission_pending_()) return Result::success(FreestoreMove::refused);
    const auto* facts = store.facts(actor);
    if (!player || !actor || !destination || !world_.graph() || !world_.graph()->node(destination)
        || !facts || !facts->qualified || facts->object.owner != player || facts->dead || facts->deleting
        || !store.contains(player, actor)) return Result::success(FreestoreMove::refused);
    if (!freestore_move_) {
        core::Diagnostic error; error.code = std::string(authoritative::codes::missing_api);
        error.message = std::string(freestore_move_reader_seam); return Result::failure(std::move(error));
    }
    const auto copied = freestore_move_(player, *facts, *world_.snapshot());
    if (!copied) return Result::failure(copied.error());
    if (!copied.value().actor_valid || !copied.value().source_ai_usable || facts->object.transit)
        return Result::success(FreestoreMove::refused);
    if (facts->parent == destination || copied.value().grandparent == destination)
        return Result::success(FreestoreMove::already_there);
    if (!freestore_path_ || !conflict_pending_) {
        core::Diagnostic error; error.code = std::string(authoritative::codes::missing_api);
        error.message = std::string(freestore_move_reader_seam); return Result::failure(std::move(error));
    }
    return Result::success(FreestoreMove::move);
}
core::Result<void> NativeActions::refresh_freestore_movement(sim::EntityId actor, gc::FleetId fleet, bool tracked) {
    const auto* facts = bound().freestore_->facts(actor);
    if (!facts) return core::Result<void>::success(); // actual loss has its own notification
    auto row = *facts; row.tracked_movement = tracked;
    const auto snapshot = world_.snapshot();
    const auto at = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(),
        [&](const auto& value) { return value.id == fleet; });
    if (at != snapshot->fleets().end()) {
        row.parent = actor == fleet ? at->orbit : fleet;
        row.parent_is_planet = actor == fleet && at->orbit && !at->motion.transit;
        row.object.directly_parented_to_planet = row.parent_is_planet;
        row.object.planet = at->motion.transit ? 0 : at->orbit;
        row.enroute = at->motion.transit.has_value();
    } else { row.object.planet = 0; row.parent_is_planet = row.object.directly_parented_to_planet = false; row.enroute = false; }
    return bound().freestore_->update(row);
}
core::Result<bool> NativeActions::admit_freestore_move(gc::PlayerId player, sim::EntityId actor, gc::PlanetId destination) {
    const auto decision = preflight_freestore_move(*bound().freestore_, player, actor, destination);
    if (!decision) return core::Result<bool>::failure(decision.error());
    if (decision.value() != FreestoreMove::move) return core::Result<bool>::success(decision.value() == FreestoreMove::already_there);
    if (bound().next_movement_ == std::numeric_limits<std::uint64_t>::max()) return invalid<bool>("freestore request identity overflow");
    const auto facts = *bound().freestore_->facts(actor);
    auto fleet = containing_fleet(player, actor);
    // WGA-313: landed transport launch uses the actual blockade/control owner.
    if (facts.object.transport && facts.parent_is_planet) {
        auto launched = world_.launch_native_transport(player, actor, 0, admission_pending_());
        if (!launched) return core::Result<bool>::failure(launched.error());
        if (!launched.value().admitted) return core::Result<bool>::success(false);
        fleet = containing_fleet(player, actor);
        if (fleet) if (auto refreshed = refresh_freestore_movement(actor, *fleet, false); !refreshed)
            return core::Result<bool>::failure(refreshed.error());
    }
    if (!fleet) return core::Result<bool>::success(false);
    const auto snapshot = world_.snapshot();
    const auto at = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& value) { return value.id == *fleet; });
    if (at == snapshot->fleets().end()) return core::Result<bool>::success(false);
    if (at->members.size() > 1 && actor != *fleet) {
        const auto source = at->orbit;
        auto removed = world_.admit_native_fleet(player, gc::RemoveFleetMembers{*fleet, {actor}}, admission_pending_());
        if (!removed) return core::Result<bool>::failure(removed.error());
        if (!removed.value().admitted) return core::Result<bool>::success(false);
        auto detached = *bound().freestore_->facts(actor); detached.parent = detached.object.planet = source;
        detached.parent_is_planet = detached.object.directly_parented_to_planet = true;
        detached.enroute = detached.tracked_movement = false;
        if (auto updated = bound().freestore_->update(detached); !updated) return core::Result<bool>::failure(updated.error());
        // Earlier admitted detach/launch effects remain if a later step refuses.
        auto created = world_.admit_native_fleet(player, gc::CreateFleet{source, {actor}, 0}, admission_pending_());
        if (!created) return core::Result<bool>::failure(created.error());
        if (!created.value().admitted) return core::Result<bool>::success(false);
        fleet = created.value().object;
        if (auto refreshed = refresh_freestore_movement(actor, *fleet, false); !refreshed)
            return core::Result<bool>::failure(refreshed.error());
    }
    Movement movement; movement.player = player; movement.actor = *fleet; movement.destination = destination;
    movement.freestore_actor = actor; movement.observed_tick = world_.snapshot()->completed_tick();
    movement.event_cursor = world_.snapshot()->events().size();
    // WGA-385: advance before insertion; a known unsafe/no-hop first advance
    // still returns a successfully created block.
    if (auto advanced = advance_freestore_movement(movement); !advanced) return core::Result<bool>::failure(advanced.error());
    mark_state_changed(); const auto request = bound().next_movement_++; bound().movements_.emplace(request, movement); update_record(2, request); update_metadata_record();
    bound().freestore_requests_.emplace(actor, request);
    if (auto refreshed = refresh_freestore_movement(actor, *fleet, true); !refreshed)
        return core::Result<bool>::failure(refreshed.error());
    return core::Result<bool>::success(true);
}
core::Result<bool> NativeActions::commit_freestore_move(const authoritative::ScriptCommand& command) {
    if (command.verb != verb_freestore_move || command.arguments.size() != 3) return invalid<bool>("invalid freestore movement command");
    const auto* player = detail::as_handle(command.arguments[0], handle_galactic_freestore);
    const auto* actor = detail::as_handle(command.arguments[1], handle_game_object);
    const auto* destination = detail::as_handle(command.arguments[2], handle_game_object);
    if (!player || player->id > std::numeric_limits<gc::PlayerId>::max() || !actor || !destination)
        return invalid<bool>("freestore movement command requires retained handles");
    return admit_freestore_move(static_cast<gc::PlayerId>(player->id), actor->id, destination->id);
}
core::Result<void> NativeActions::service_freestore_movement(gc::PlayerId player, sim::EntityId actor) {
    // Bounded retained-request frontier, called by native unit service, never
    // an extra per-frame galaxy sweep or the authored Lua periodic callback.
    const auto found = bound().freestore_requests_.find(actor);
    if (found == bound().freestore_requests_.end() || bound().movements_.at(found->second).player != player) return core::Result<void>::success();
    const auto request = found->second;
    const auto* facts = bound().freestore_->facts(actor);
    if (!facts || facts->object.owner != player) return retire_movement(request);
    const auto polled = poll_movement(request);
    if (!polled) return core::Result<void>::failure(polled.error());
    if (polled.value().finished) return retire_movement(request);
    return refresh_freestore_movement(actor, polled.value().actor, true);
}
core::Result<void> NativeActions::service_freestore_landing(gc::PlayerId player,
    const FreestorePool& pool, const sim::PartitionExecutor& executor) {
    if (!pool.qualified || !pool.qualified_players.contains(player))
        return invalid<void>("automatic landing requires its initialized qualified frontier");
    const auto& seats = world_.setup().seats;
    const auto owner = std::find_if(seats.begin(), seats.end(), [player](const auto& seat) { return seat.player == player; });
    if (owner == seats.end()) return invalid<void>("automatic landing manager human status unavailable");
    if (owner->kind == gc::SeatKind::human) return core::Result<void>::success();
    auto& scratch = bound().player_scratch_.at(player);
    auto& landing_stage_ = scratch.landing_stage;
    auto& landing_surfaces_ = scratch.landing_surfaces;
    landing_stage_.resize(pool.objects.size());
    const auto prepared = executor.execute_phase("galactic-ai-automatic-landing", sim::tick_partition_count,
        [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, pool.objects.size());
            for (auto i = range.begin; i < range.end; ++i) {
                landing_stage_[i] = 0;
                const auto& actor = pool.objects[i];
                if (actor.owner != player || !bound().freestore_->contains(player, actor.id)) continue;
                const auto* facts = bound().freestore_->facts(actor.id);
                if (!facts) continue;
                const auto* parent = bound().freestore_->facts(facts->parent);
                const auto at = parent ? std::lower_bound(pool.surfaces.begin(), pool.surfaces.end(), parent->parent,
                    [](const auto& surface, auto id) { return surface.planet < id; }) : pool.surfaces.end();
                const auto* surface = at != pool.surfaces.end() && parent && at->planet == parent->parent ? &*at : nullptr;
                if (automatic_landing_gate(false, actor, *facts, parent, surface) == LandingGate::admitted)
                    landing_stage_[i] = surface->planet;
            }
        });
    if (!prepared) return prepared;
    landing_surfaces_.assign(pool.surfaces.begin(), pool.surfaces.end());
    // ADR-009 / WGA-416: revalidate capacity in the world owner as earlier
    // ordered links may consume the last slot. Reservations do not skip service.
    for (std::size_t i = 0; i < landing_stage_.size(); ++i) {
        const auto planet = landing_stage_[i]; if (!planet) continue;
        const auto actor = pool.objects[i].id;
        const auto* facts = bound().freestore_->facts(actor);
        if (!facts || !bound().freestore_->contains(player, actor)) continue;
        const auto surface = std::lower_bound(landing_surfaces_.begin(), landing_surfaces_.end(), planet,
            [](const auto& value, auto id) { return value.planet < id; });
        if (surface == landing_surfaces_.end() || surface->planet != planet) continue;
        const auto capacity = world_.native_transport_landing_available(player, planet, surface->landed);
        if (!capacity) return core::Result<void>::failure(capacity.error());
        if (capacity.value() != gc::GalacticSession::NativeTransportLandingGate::admitted) continue;
        if (!admission_pending_) return invalid<void>("automatic landing management admission owner unavailable");
        const auto removed = world_.admit_native_fleet(player, gc::RemoveFleetMembers{facts->parent, {actor}}, admission_pending_());
        if (!removed) return core::Result<void>::failure(removed.error());
        const auto landed = world_.land_native_transport(player, actor, planet, admission_pending_(), surface->landed);
        if (!landed) return core::Result<void>::failure(landed.error());
        // The event receipt is admission, not the link Boolean (WGA-413).
        if (landed.value().linked) {
            ++surface->landed;
            if (const auto* current = bound().freestore_->facts(actor)) {
                auto row = *current; row.parent = row.object.planet = planet;
                row.parent_is_planet = row.object.directly_parented_to_planet = true;
                row.enroute = false;
                if (auto updated = bound().freestore_->update(std::move(row)); !updated) return updated;
            }
        }
    }
    return core::Result<void>::success();
}
void NativeActions::update_metadata_record() {
    std::vector<std::uint8_t> bytes; word(bytes, bound().next_movement_); word(bytes, bound().freestore_move_refusals_); bound().records_.set(0, 0, 0, bytes);
}
void NativeActions::update_record(std::uint32_t kind, std::uint64_t id) {
    std::vector<std::uint8_t> bytes;
    if (kind == 1) {
        const auto row = bound().purchases_.find(id); if (row == bound().purchases_.end()) return;
        write_purchase(bytes, id, row->second); bound().records_.set(row->second.player, kind, id, bytes);
    } else {
        const auto row = bound().movements_.find(id); if (row == bound().movements_.end()) return;
        write_movement(bytes, id, row->second); bound().records_.set(row->second.player, kind, id, bytes);
    }
}
core::RecordDigest NativeActions::rebuild_records() const {
    core::RecordDigest rebuilt;
    std::vector<std::uint8_t> bytes; word(bytes, bound().next_movement_); word(bytes, bound().freestore_move_refusals_); rebuilt.set(0, 0, 0, bytes);
    for (const auto& [id, row] : bound().purchases_) { bytes.clear(); write_purchase(bytes, id, row); rebuilt.set(row.player, 1, id, bytes); }
    for (const auto& [id, row] : bound().movements_) { bytes.clear(); write_movement(bytes, id, row); rebuilt.set(row.player, 2, id, bytes); }
    return rebuilt;
}
core::Result<std::string> NativeActions::record_sync_hash(bool cold) const {
    if (pending_admission()) return invalid<std::string>("production digest requires the completed admission barrier");
    const auto records = cold ? rebuild_records().digest(component) : bound().records_.digest(component);
    return core::Result<std::string>::success(compose_native_sync_digests({{"records", records}, {"freestore", bound().freestore_->record_sync_hash(cold)}}));
}
core::Result<NativeState> NativeActions::capture() const {
    if (auto verified = verify_record_sync_hash(); !verified) return core::Result<NativeState>::failure(verified.error());
    if (pending_admission()) return invalid<NativeState>("production state requires the completed admission barrier");
    NativeState out; auto& bytes = out.components[std::string(component)];
    word(bytes, component.size()); bytes.insert(bytes.end(), component.begin(), component.end()); word(bytes, 10);
    word(bytes, bound().purchases_.size());
    for (const auto& [id, row] : bound().purchases_) write_purchase(bytes, id, row);
    word(bytes, bound().next_movement_); word(bytes, bound().movements_.size());
    for (const auto& [id, row] : bound().movements_) write_movement(bytes, id, row);
    word(bytes, bound().freestore_move_refusals_);
    const auto freestore = bound().freestore_->encode(); word(bytes, freestore.size()); bytes.insert(bytes.end(), freestore.begin(), freestore.end());
    return core::Result<NativeState>::success(std::move(out));
}
core::Result<void> NativeActions::restore(const NativeState& state) {
    if (pending_admission() || state.version != 1 || !funding_) return invalid<void>("production restore requires its completed native/funding boundary");
    const auto found = state.components.find(component);
    if (found == state.components.end()) return invalid<void>("production component is missing");
    const auto& bytes = found->second; std::size_t at{}; bool valid = true;
    const auto read = [&]() {
        std::uint64_t out{};
        if (bytes.size() - at < 8) { valid = false; return out; }
        for (unsigned i = 0; i < 8; ++i) out |= static_cast<std::uint64_t>(bytes[at++]) << (i * 8);
        return out;
    };
    if (read() != component.size() || bytes.size() - at < component.size()) return invalid<void>("production domain is invalid");
    for (const auto character : component) if (bytes[at++] != static_cast<std::uint8_t>(character)) valid = false;
    if (read() != 10) valid = false;
    const auto count = read();
    if (!valid || count > (bytes.size() - at) / 128) return invalid<void>("production component framing is invalid");
    std::map<sim::EntityId, Purchase> staged;
    sim::EntityId last{};
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto id = read(), player = read(), goal = read(), price = read(), billed = read(), marked = read(), remaining = read(), orphan = read();
        const auto completed = read(), failed = read(), object = read();
        const auto producer = read(), requested_type = read(), channel = read(), task_finished = read(), salvage = read();
        if (!producer || !world_.graph() || !world_.graph()->node(producer) || !requested_type
            || channel > 1 || task_finished > 1 || salvage > 1) return invalid<void>("production task association is invalid");
        if (completed > 1 || failed > 1 || (completed && failed) || (object && (!completed || object >= world_.next_entity_id())))
            return invalid<void>("production completion reference is invalid");
        if (!valid || !id || id <= last || id >= world_.next_entity_id() || !player || player > std::numeric_limits<gc::PlayerId>::max()
            || !goal || price > (std::uint64_t{1} << 48) || billed > price || marked > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || remaining > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || orphan > 1) return invalid<void>("production identities or amounts are invalid");
        const auto ledger = funding_->ledger(static_cast<gc::PlayerId>(player));
        // Completed and cancelled mappings remain visible; pending obligations
        // can outlive an active goal; orphan billing uses the aggregate dead budget.
        if (!ledger)
            return invalid<void>("production obligation has an unknown native goal");
        staged.emplace(id, Purchase{static_cast<gc::PlayerId>(player), goal,
            sim::math::Fixed::from_raw(static_cast<std::int64_t>(price)), sim::math::Fixed::from_raw(static_cast<std::int64_t>(billed)), sim::math::Fixed::from_raw(static_cast<std::int64_t>(marked)),
            sim::math::Fixed::from_raw(static_cast<std::int64_t>(remaining)), orphan != 0, completed != 0, failed != 0, object,
            producer, requested_type, static_cast<gc::ProductionDomain>(channel), task_finished != 0, salvage != 0});
        last = id;
    }
    const auto next_movement = read(), move_count = read();
    if (!valid || !next_movement || move_count > (bytes.size() - at) / 128) return invalid<void>("movement component framing is invalid");
    std::map<std::uint64_t, Movement> moves; last = 0;
    for (std::uint64_t i = 0; i < move_count; ++i) {
        const auto id = read(), player = read(), actor = read(), destination = read(), location = read(), reach = read();
        const auto taskforce = read(), finished = read(), advance = read(), tick = read(), cursor = read();
        const auto plan_id = read(), taskforce_id = read(), block_id = read(), registered = read();
        const auto freestore_actor = read();
        if (!valid || !id || id <= last || id >= next_movement || !player || player > std::numeric_limits<gc::PlayerId>::max()
            || actor >= world_.next_entity_id() || freestore_actor >= world_.next_entity_id()
            || (freestore_actor && (plan_id || taskforce_id || block_id || taskforce || reach || location)) || !world_.graph() || !world_.graph()->node(destination)
            || (location && !world_.graph()->node(location)) || reach > static_cast<std::uint64_t>(MovementReach::any)
            || registered > 1 || taskforce > 1 || (taskforce != (taskforce_id != 0)) || finished > 1 || advance > 1 || (!actor && !finished) || tick > world_.snapshot()->completed_tick()
            || (tick == world_.snapshot()->completed_tick() && cursor > world_.snapshot()->events().size())
            || std::none_of(world_.setup().players.begin(), world_.setup().players.end(), [&](const auto& p) { return p.player_id == player; }))
            return invalid<void>("movement identities, policy or listener state are invalid");
        moves.emplace(id, Movement{static_cast<gc::PlayerId>(player), actor, destination, location,
            static_cast<MovementReach>(reach), taskforce != 0, finished != 0, advance != 0, tick, cursor, plan_id, taskforce_id, block_id, registered != 0, freestore_actor}); last = id;
    }
    const auto freestore_refusals = read(); const auto freestore_size = read();
    if (!valid || freestore_size != bytes.size() - at) return invalid<void>("production freestore tail framing is invalid");
    const auto freestore_bytes = std::span(bytes).subspan(at, static_cast<std::size_t>(freestore_size));
    QualifiedFreestore staged_freestore;
    if (auto decoded = staged_freestore.restore(freestore_bytes, world_.next_entity_id(), world_.setup().players); !decoded) return decoded;
    if (!staged_freestore.inventory().empty()) {
        if (!freestore_validation_) return invalid<void>("freestore restore requires reconstructed content/lifecycle/reservation validation");
        if (auto checked = freestore_validation_(staged_freestore); !checked) return checked;
    }
    std::map<sim::EntityId, std::uint64_t> tracked_actors;
    for (const auto& [id, movement] : moves) {
        if (!movement.registered || !movement.freestore_actor) continue;
        const auto* facts = staged_freestore.facts(movement.freestore_actor);
        if (!facts || facts->object.owner != movement.player || !facts->tracked_movement
            || !tracked_actors.emplace(movement.freestore_actor, id).second)
            return invalid<void>("freestore movement requires its unique retained actor and tracked-block fact");
    }
    for (const auto& [id, movement] : moves) if (movement.plan_id || movement.taskforce_id || movement.block_id) {
        if (!movement_references_) return invalid<void>("associated movement restore requires its reconstructed plan/taskforce/block validator");
        if (auto reference = movement_references_(id, movement); !reference) return reference;
    }
    if (movement_inventory_) if (auto inventory = movement_inventory_(moves); !inventory) return inventory;
    std::map<std::pair<gc::PlayerId, std::uint64_t>, sim::math::Fixed> obligations;
    for (const auto& [id, row] : staged) {
        (void)id;
        if (row.orphan) continue; // WGA-349/160: aggregate dead budget, no live goal marks
        auto& total = obligations[{row.player, row.goal}];
        auto sum = sim::math::add(total, sim::math::subtract(row.marked, row.billed).value());
        if (!sum) return core::Result<void>::failure(sum.error());
        total = sum.value();
    }
    for (const auto& [key, total] : obligations) if (total.raw() > 0) {
        const auto ledger = funding_->ledger(key.first);
        const auto active = ledger->goals.find(key.second);
        const auto dead = ledger->dead_goals.find(key.second);
        const auto marked = active != ledger->goals.end() ? active->second.marked
            : dead != ledger->dead_goals.end() ? dead->second.marked : sim::math::Fixed{};
        if (total > marked) return invalid<void>("production obligations exceed restored ledger marks");
    }
    if (world_.snapshot()->production()) for (const auto& planet : world_.snapshot()->production()->planets)
        for (const auto& queue : planet.queues) for (const auto& entry : queue) {
            const auto player = std::find_if(world_.snapshot()->production()->players.begin(), world_.snapshot()->production()->players.end(),
                [&](const auto& p) { return p.player == entry.builder; });
            if (player == world_.snapshot()->production()->players.end()) return invalid<void>("production queue has an unknown player");
            if (player->human) continue;
            const auto purchase = staged.find(entry.build_id);
            if (purchase == staged.end() || purchase->second.completed || purchase->second.failed
                || purchase->second.player != entry.builder || purchase->second.price != entry.price
                || purchase->second.producer != planet.planet || purchase->second.requested_type != entry.type
                || static_cast<std::size_t>(purchase->second.channel) != static_cast<std::size_t>(&queue - planet.queues.data())
                || purchase->second.remaining != entry.last_remaining)
                return invalid<void>("production mapping disagrees with the reconstructed paid queue");
        }
    mark_build_state_changed();
    // Validation above finishes before any retained owner is replaced.
    if (auto decoded = bound().freestore_->restore(freestore_bytes, world_.next_entity_id(), world_.setup().players); !decoded) return decoded;
    bound().purchases_ = std::move(staged);
    bound().movements_ = std::move(moves); bound().next_movement_ = next_movement;
    bound().freestore_requests_ = std::move(tracked_actors);
    bound().freestore_move_refusals_ = freestore_refusals; bound().records_ = rebuild_records();
    return core::Result<void>::success();
}
core::Result<std::string> NativeActions::state_hash() const {
    auto saved = capture();
    if (!saved) return core::Result<std::string>::failure(saved.error());
    return core::Result<std::string>::success(core::sha256_hex(saved.value().components.at(std::string(component))));
}
struct NativeActions::ImmediateState final : authoritative::ImmediateEffectState {
    std::shared_ptr<NativeActions*> lifetime;
    gc::GalacticSession::NativeActionShadow world;
    ShadowPlacement placement;
    std::map<gc::PlayerId, std::shared_ptr<const gc::funding::Ledger>> ledgers;
    std::map<gc::PlayerId, std::shared_ptr<const gc::funding::Ledger>> originals;
    std::map<std::uint64_t, std::vector<authoritative::Value>> taskforce_units;
    std::vector<std::pair<gc::NativeEffectKind, economic_replay::Bytes>> effects;
    std::optional<QualifiedFreestore> freestore;
    struct StoreStep { std::size_t native_count{}; std::vector<QualifiedFreestore::Replay> operations; };
    std::vector<StoreStep> store_steps;
    std::vector<std::uint8_t> original_freestore;
    std::shared_ptr<const gc::GalacticSnapshot> base;
    std::size_t prepared_births{};
    std::uint64_t freestore_refusals{}; // private additive delta, no shared Lua write
    ImmediateState(std::shared_ptr<NativeActions*> token, gc::GalacticSession::NativeActionShadow shadow, ShadowPlacement builder)
        : lifetime(std::move(token)), world(std::move(shadow)), placement(std::move(builder)), base((*lifetime)->world_.snapshot()) {}
    std::unique_ptr<authoritative::ImmediateEffectState> clone() const override { return std::make_unique<ImmediateState>(*this); }
    static std::optional<std::uint64_t> id(const authoritative::Value& value, std::uint32_t kind) {
        const auto* handle = std::get_if<authoritative::Handle>(&value.data);
        return handle && handle->kind == kind && handle->id ? std::optional(handle->id) : std::nullopt;
    }
    core::Result<void> retain_store_step(const QualifiedFreestore::OperationCursor& before) {
        if (!world.intent_sink()) return core::Result<void>::success();
        auto delta = freestore->operations_since(before);
        if (!delta) return core::Result<void>::failure(delta.error());
        if (!delta.value().empty()) store_steps.push_back({world.intents().size(), std::move(delta).value()});
        return core::Result<void>::success();
    }
    core::Result<authoritative::ValueList> apply(std::string_view action, const authoritative::ValueList& args) override {
        using Result = core::Result<authoritative::ValueList>;
        auto* owner = *lifetime;
        if (!owner) return invalid<authoritative::ValueList>("immediate native owner must be rebound");
        if (action == "galactic.freestore_move_unbound") {
            if (!args.empty() || freestore_refusals == std::numeric_limits<std::uint64_t>::max())
                return invalid<authoritative::ValueList>("freestore refusal counter is invalid");
            ++freestore_refusals; return Result::success({});
        }
        if (action == "galactic.prefer_ground") {
            const auto actor = args.size() == 2 ? id(args[0], handle_game_object) : std::nullopt;
            const auto* prefer = args.size() == 2 ? std::get_if<bool>(&args[1].data) : nullptr;
            if (!actor || !prefer) return invalid<authoritative::ValueList>("Set_Prefer_Ground_Over_Space requires an actor and Boolean");
            if (!freestore) { freestore = owner->bound().freestore_->clone(); original_freestore = owner->bound().freestore_->encode(); }
            const auto before = freestore->operation_cursor();
            const auto result = freestore->set_prefer_ground(*actor, *prefer);
            if (!result) return Result::failure(result.error());
            if (auto retained = retain_store_step(before); !retained) return Result::failure(retained.error());
            return Result::success({});
        }
        if (action == "galactic.prevent_ai_usage") {
            const auto actor = args.size() == 2 ? id(args[0], handle_game_object) : std::nullopt;
            const auto* prevent = args.size() == 2 ? std::get_if<bool>(&args[1].data) : nullptr;
            if (!actor || !prevent) return invalid<authoritative::ValueList>("Prevent_AI_Usage requires an actor and Boolean");
            if (!freestore) { freestore = owner->bound().freestore_->clone(); original_freestore = owner->bound().freestore_->encode(); }
            const auto* facts = freestore->facts(*actor);
            if (!facts) return invalid<authoritative::ValueList>("Prevent_AI_Usage requires authoritative actor facts");
            const auto before = freestore->operation_cursor();
            const auto result = freestore->prevent_ai_usage(facts->object.owner, *actor, *prevent);
            if (!result) return Result::failure(result.error());
            if (auto retained = retain_store_step(before); !retained) return Result::failure(retained.error());
            return Result::success({});
        }
        if (action == "galactic.give_money") {
            if (args.size() != 2 || !id(args[0], handle_player) || !std::get_if<numeric::LuaNumber>(&args[1].data))
                return invalid<authoritative::ValueList>("Give_Money requires a player and numeric amount");
            const auto player_id = *id(args[0], handle_player);
            if (player_id > std::numeric_limits<gc::PlayerId>::max()) return invalid<authoritative::ValueList>("cash player exceeds identity range");
            const auto player = static_cast<gc::PlayerId>(player_id);
            const auto multiplier = owner->multipliers_.find(player);
            if (multiplier == owner->multipliers_.end() || !owner->funding_)
                return invalid<authoritative::ValueList>("cash requires selected multiplier and native funding owner");
            auto amount = numeric::to_fixed(std::get<numeric::LuaNumber>(args[1].data));
            if (!amount) return Result::failure(amount.error());
            auto pending_world = world;
            auto actual = pending_world.add_credits(player, amount.value(), multiplier->second);
            if (!actual) return Result::failure(actual.error());
            auto before = ledgers.contains(player) ? ledgers.at(player) : owner->funding_->ledger(player);
            if (before) {
                auto changed = gc::funding::windfall(*before, actual.value());
                if (!changed) return Result::failure(changed.error());
                if (!originals.contains(player)) originals[player] = before;
                ledgers[player] = std::make_shared<const gc::funding::Ledger>(std::move(changed).value());
            }
            world = std::move(pending_world);
            effects.emplace_back(gc::NativeEffectKind::cash, economic_replay::cash(player, amount.value(), multiplier->second, actual.value()));
            return Result::success({});
        }
        if (action != "galactic.magic_force" || (args.size() != 3 && args.size() != 4)
            || !id(args[0], handle_player) || !id(args[1], handle_game_object))
            return invalid<authoritative::ValueList>("magic requires its player, staging planet and requested original types");
        const auto player_id = *id(args[0], handle_player);
        if (player_id > std::numeric_limits<gc::PlayerId>::max()) return invalid<authoritative::ValueList>("magic player exceeds identity range");
        const auto* types = std::get_if<std::vector<authoritative::Value>>(&args[2].data);
        if (!types || !placement) return invalid<authoritative::ValueList>("magic requires a qualified local placement provider and types");
        const auto taskforce = args.size() == 4 ? id(args[3], ai::handle_taskforce) : std::nullopt;
        if (args.size() == 4 && !taskforce) return invalid<authoritative::ValueList>("magic taskforce handle is invalid");
        if (taskforce && taskforce_units.contains(*taskforce))
            return Result::success({authoritative::Value{taskforce_units.at(*taskforce)}});
        std::vector<authoritative::Value> actors;
        const sim::InlineExecutor inline_executor;
        const auto& executor = owner->current_executor() ? *owner->current_executor() : static_cast<const sim::PartitionExecutor&>(inline_executor);
        for (const auto& value : *types) {
            const auto type = id(value, handle_type);
            if (!type) return invalid<authoritative::ValueList>("magic type handle is invalid");
            ActionRequest request; request.kind = ActionKind::magic; request.player = static_cast<gc::PlayerId>(player_id);
            request.target = *id(args[1], handle_game_object); request.type = *type;
            const auto first_birth = world.actor_changes().size();
            auto made = world.create_force({request.player, request.target, request.type, {}}, [&](const auto& view, auto frontier) {
                return placement(request, view, frontier);
            }, executor);
            if (!made) return Result::failure(made.error());
            if (owner->freestore_births_) {
                if (!freestore) { freestore = owner->bound().freestore_->clone(); original_freestore = owner->bound().freestore_->encode(); }
                const auto births = world.actor_changes().subspan(first_birth);
                auto native_callback = freestore->suspend_intents();
                if (auto prepared = owner->freestore_births_(*freestore, *base, world, births, executor); !prepared)
                    return Result::failure(prepared.error());
                prepared_births = world.actor_changes().size();
            } else if (owner->qualified_freestore_available())
                return invalid<authoritative::ValueList>("qualified magic requires private actor birth preparation");
            effects.emplace_back(gc::NativeEffectKind::magic, economic_replay::magic(request, made.value()));
            for (const auto actor : made.value()) {
                if (!actor)
                    return invalid<authoritative::ValueList>("magic identity exceeds the Lua handle range");
                actors.push_back(authoritative::Value{authoritative::Handle{handle_game_object, actor}});
            }
        }
        if (taskforce) taskforce_units[*taskforce] = actors;
        return Result::success({authoritative::Value{std::move(actors)}});
    }
    core::Result<authoritative::ValueList> query(std::string_view name, const authoritative::ValueList& args) const override {
        using Result = core::Result<authoritative::ValueList>;
        if (!*lifetime) return invalid<authoritative::ValueList>("immediate native owner must be rebound");
        if (name == "galactic.freestore_move") {
            const auto player = args.size() == 3 ? id(args[0], handle_galactic_freestore) : std::nullopt;
            const auto actor = args.size() == 3 ? id(args[1], handle_game_object) : std::nullopt;
            const auto destination = args.size() == 3 ? id(args[2], handle_game_object) : std::nullopt;
            if (!player || *player > std::numeric_limits<gc::PlayerId>::max() || !actor || !destination)
                return Result::success({authoritative::Value::number(Number(0))});
            const auto& store = freestore ? *freestore : *(*lifetime)->bound().freestore_;
            const auto decision = (*lifetime)->preflight_freestore_move(store, static_cast<gc::PlayerId>(*player), *actor, *destination);
            if (!decision) return Result::failure(decision.error());
            return Result::success({authoritative::Value::number(Number(static_cast<std::int64_t>(decision.value())))});
        }
        if (name == "galactic.planet_ai_usable") {
            // WGA-438: the receiver must be a planet; the answer reads the completed snapshot this shadow was captured from.
            const auto planet = args.size() == 1 ? id(args[0], handle_game_object) : std::nullopt;
            if (!planet || !world.planet(*planet))
                return invalid<authoritative::ValueList>("Get_Is_Planet_AI_Usable -- object is not a planet.");
            const auto* owner = *lifetime;
            if (!owner || !owner->planet_usability_) {
                core::Diagnostic error; error.code = std::string(authoritative::codes::missing_api);
                error.message = "planetary AI usability reader unbound (WGA-127/438)"; return Result::failure(std::move(error));
            }
            const auto usable = owner->planet_usability_(*base, *planet);
            if (!usable) return Result::failure(usable.error());
            return Result::success({authoritative::Value{usable.value()}});
        }
        if (name == "galactic.freestore_count") {
            const auto player = !args.empty() ? id(args[0], ai::handle_freestore) : std::nullopt;
            const auto* nontransport = args.size() == 2 ? std::get_if<bool>(&args[1].data) : nullptr;
            const auto& store = freestore ? *freestore : *(*lifetime)->bound().freestore_;
            if (!player || *player > std::numeric_limits<gc::PlayerId>::max() || args.size() > 2 || (args.size() == 2 && !nontransport)
                || !store.initialized(static_cast<gc::PlayerId>(*player))) return invalid<authoritative::ValueList>("freestore count requires initialized owning manager");
            return Result::success({authoritative::Value::number(Number(static_cast<std::int64_t>(store.object_count(static_cast<gc::PlayerId>(*player), nontransport ? std::optional(*nontransport) : std::nullopt))))});
        }
        if (args.size() != 1) return invalid<authoritative::ValueList>("native query requires one handle");
        if (name == "galactic.credits") {
            const auto player = id(args[0], handle_player);
            if (!player || *player > std::numeric_limits<gc::PlayerId>::max()) return invalid<authoritative::ValueList>("invalid credit query player");
            const auto amount = world.credits(static_cast<gc::PlayerId>(*player));
            return amount ? Result::success({authoritative::Value::number(numeric::from_fixed(*amount))})
                : invalid<authoritative::ValueList>("credit query has no account");
        }
        if (name == "galactic.taskforce_units") {
            const auto taskforce = id(args[0], ai::handle_taskforce);
            if (!taskforce) return invalid<authoritative::ValueList>("invalid taskforce query");
            const auto at = taskforce_units.find(*taskforce);
            return at == taskforce_units.end() ? Result::success({}) : Result::success({authoritative::Value{at->second}});
        }
        const auto handle = id(args[0], handle_game_object);
        if (!handle) return Result::success({});
        const auto* object = world.created_object(*handle);
        if (!object) return Result::success({}); // existing/dead handles use the ordinary completed view
        if (name == "galactic.created_valid") return Result::success({authoritative::Value{true}});
        if (name == "galactic.created_type") return Result::success({authoritative::Value{authoritative::Handle{handle_type, object->original_type}}});
        if (name == "galactic.created_owner") return Result::success({authoritative::Value{authoritative::Handle{handle_player, object->owner}}});
        if (name == "galactic.created_position") {
            const auto* planet = world.planet(object->planet);
            if (planet) return Result::success({detail::position_value(planet->position)});
        }
        return invalid<authoritative::ValueList>("unknown native query");
    }
};
struct NativeActions::ImmediateHost final : authoritative::ImmediateEffectHost {
    std::shared_ptr<NativeActions*> lifetime;
    ShadowPlacement placement;
    std::size_t max_requests{};
    ImmediateHost(std::shared_ptr<NativeActions*> token, ShadowPlacement builder, std::size_t bound)
        : lifetime(std::move(token)), placement(std::move(builder)), max_requests(bound) {}
    core::Result<std::unique_ptr<authoritative::ImmediateEffectState>> capture() const override {
        using Result = core::Result<std::unique_ptr<authoritative::ImmediateEffectState>>;
        auto* owner = *lifetime;
        if (!owner) return invalid<std::unique_ptr<authoritative::ImmediateEffectState>>("immediate native owner must be rebound");
        auto shadow = owner->world_.begin_native_actions(max_requests);
        if (!shadow) return Result::failure(shadow.error());
        return Result::success(std::make_unique<ImmediateState>(lifetime, std::move(shadow).value(), placement));
    }
    core::Result<void> publish(std::unique_ptr<authoritative::ImmediateEffectState> state) override {
        auto* owner = *lifetime;
        const auto* native = dynamic_cast<const ImmediateState*>(state.get());
        if (!owner || !native || native->lifetime != lifetime)
            return invalid<void>("native publication requires its originating live owner");
        if (native->freestore && native->original_freestore != owner->bound().freestore_->encode())
            return invalid<void>("freestore changed during private shadow service");
        if (native->freestore_refusals > std::numeric_limits<std::uint64_t>::max() - owner->bound().freestore_move_refusals_)
            return invalid<void>("freestore refusal counter exceeds its retained range");
        ai::GalacticFunding::State changed;
        if (!native->ledgers.empty() && !owner->funding_) return invalid<void>("native cash requires its funding owner");
        for (const auto& [player, ledger] : native->ledgers) {
            const auto before = owner->funding_->ledger(player);
            if (!before || *before != *native->originals.at(player)) return invalid<void>("native funding changed during shadow service");
            changed.emplace(player, *ledger);
        }
        const bool prepared = owner->freestore_births_ && native->prepared_births == native->world.actor_changes().size();
        const auto& native_sink = native->world.intent_sink();
        const auto& store_sink = owner->bound().freestore_->intent_sink();
        if (!native->store_steps.empty() && (!native_sink || !store_sink))
            return invalid<void>("accepted immediate transaction requires its joint decision sinks");
        std::size_t previous_count{};
        for (const auto& step : native->store_steps) {
            if (step.native_count < previous_count || step.native_count > native->world.intents().size())
                return invalid<void>("immediate freestore step has an invalid native intent prefix");
            previous_count = step.native_count;
        }
        const auto publish_world = [&] { return owner->world_.commit_native_actions(native->world, prepared, !native_sink); };
        auto committed = [&]() -> core::Result<void> {
            if (!owner->funding_) return publish_world();
            if (!owner->funding_->has_player_frontier())
                return owner->funding_->commit_rows(std::move(changed), publish_world);
            auto rows = owner->funding_->prepare_private_rows(std::move(changed));
            if (!rows) return core::Result<void>::failure(rows.error());
            if (auto published = publish_world(); !published) return published;
            return owner->funding_->publish_private_rows(std::move(rows).value());
        }();
        if (committed) {
            if (native->freestore_refusals) {
                owner->bound().freestore_move_refusals_ += native->freestore_refusals; owner->mark_state_changed(); owner->update_metadata_record();
            }
            if (native->freestore) owner->bound().freestore_->replace(*native->freestore);
            owner->bound().immediate_work_ = native->world.work();
            if (native_sink) {
                const auto intents = native->world.intents();
                std::size_t next{};
                for (const auto& step : native->store_steps) {
                    while (next < step.native_count) native_sink(native->world.player(), intents[next++]);
                    for (const auto& replay : step.operations) store_sink(replay);
                }
                while (next < intents.size()) native_sink(native->world.player(), intents[next++]);
            }
            for (const auto& [kind, payload] : native->effects)
                if (auto recorded = owner->world_.append_native_effect(kind, payload); !recorded) return recorded;
        }
        return committed;
    }
};
std::shared_ptr<authoritative::ImmediateEffectHost> NativeActions::immediate_host(ShadowPlacement placement, std::size_t max_requests) {
    return std::make_shared<ImmediateHost>(lifetime_, std::move(placement), max_requests);
}

} // namespace eawr::script::foc::galactic_ai
