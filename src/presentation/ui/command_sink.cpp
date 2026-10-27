#include "eawr/presentation/ui/command_sink.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

namespace eawr::presentation::ui {
namespace {

namespace tactical = sim::tactical;

[[nodiscard]] core::Diagnostic failure(const std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.message = std::move(message);
    return diagnostic;
}

} // namespace

namespace {
bool consumes(const TacticalIntent& pending, const TacticalIntent& request) {
    const bool overlap = std::any_of(request.units.begin(), request.units.end(), [&](const auto unit) {
        return std::find(pending.units.begin(), pending.units.end(), unit) != pending.units.end();
    });
    // A pending sale consumes the entity itself, including replacement orders
    // that a repeated guard-override gesture would otherwise issue for that child.
    if (pending.verb == TacticalVerb::pad_sell && overlap) return true;
    if (pending.verb != request.verb) return false;
    if (request.verb == TacticalVerb::cancel)
        return pending.queue == request.queue && pending.queue_entry_id == request.queue_entry_id;
    if (request.verb == TacticalVerb::intentional_quit) return true;
    if (!overlap) return false;
    switch (request.verb) {
    case TacticalVerb::ability: return pending.unit_ability == request.unit_ability;
    case TacticalVerb::repair_hardpoint: return pending.hardpoint == request.hardpoint;
    case TacticalVerb::special_fire:
    case TacticalVerb::pad_build:
    case TacticalVerb::pad_sell: return true;
    default: return false; // Orders replace orders; ordinary purchases reserve count and money.
    }
}
}

tactical::EconomyView pending_economy(tactical::EconomyView view,
    const std::span<const PendingCommand> pending, const PendingBuildOption& option_of,
    const std::optional<std::uint64_t> completed_tick) {
    for (const auto& row : pending) {
        if (completed_tick && row.execution_tick && *row.execution_tick < *completed_tick) continue;
        const auto& intent = row.intent;
        if (intent.verb == TacticalVerb::buy || intent.verb == TacticalVerb::pad_build) {
            const auto* option = option_of(intent.units.front(), intent.type);
            if (!option) continue;
            view.credits = sim::math::Fixed::from_raw(std::max<std::int64_t>(0, view.credits.raw() - option->price.raw()));
            if (intent.verb == TacticalVerb::buy) {
                auto& queue = view.queues[static_cast<std::size_t>(option->queue)];
                // Zero is deliberately not an authoritative cancellable queue identity.
                queue.push_back({intent.type, intent.units.front(), option->price, option->build_frames,
                    queue.empty() ? row.tick + option->build_frames : 0U, 0});
            }
        } else if (intent.verb == TacticalVerb::cancel && intent.queue_entry_id != 0) {
            auto& queue = view.queues[static_cast<std::size_t>(intent.queue)];
            const auto found = std::find_if(queue.begin(), queue.end(), [&](const auto& entry) {
                return entry.entry_id == intent.queue_entry_id;
            });
            if (found == queue.end()) continue;
            const auto refund = sim::math::add(view.credits, found->paid);
            if (refund) view.credits = refund.value();
            const bool front = found == queue.begin();
            queue.erase(found);
            if (front && !queue.empty()) queue.front().complete_frame = row.tick + queue.front().frames;
        } else if (intent.verb == TacticalVerb::reinforce) {
            // WGM-48/WGT-19: campaign offers carry persistent identity and original population.
            if (!view.reserves.empty()) {
                const auto reserve = std::find_if(view.reserves.begin(), view.reserves.end(), [&](const auto& entry) {
                    return entry.token == intent.reinforcement_token && entry.type == intent.type;
                });
                if (reserve == view.reserves.end() || reserve->restricted) continue;
                view.population += reserve->population;
                view.reserves.erase(reserve);
                const auto token = std::find(view.pool_tokens.begin(), view.pool_tokens.end(), intent.reinforcement_token);
                if (token != view.pool_tokens.end()) {
                    const auto index = static_cast<std::size_t>(token - view.pool_tokens.begin());
                    if (index < view.pool.size()) view.pool.erase(view.pool.begin() + static_cast<std::ptrdiff_t>(index));
                    view.pool_tokens.erase(token);
                }
                continue;
            }
            const auto found = std::find(view.pool.begin(), view.pool.end(), intent.type);
            if (found == view.pool.end()) continue;
            const auto index = static_cast<std::size_t>(found - view.pool.begin());
            view.pool.erase(found);
            if (index < view.pool_tokens.size()) view.pool_tokens.erase(view.pool_tokens.begin() + static_cast<std::ptrdiff_t>(index));
            const auto* option = option_of(sim::invalid_entity_id, intent.type);
            if (option) view.population += option->population;
        }
    }
    return view;
}

void CommandScheduler::present_snapshot(const std::uint64_t completed_tick) {
    const std::lock_guard lock(mutex_);
    reservations_enabled_ = true;
    if (std::erase_if(reservations_, [completed_tick](const auto& row) {
        return row.execution_tick && *row.execution_tick < completed_tick;
    }))
        ++reservation_revision_;
}
void CommandScheduler::present_result(const std::uint64_t sequence) {
    const std::lock_guard lock(mutex_);
    if (std::erase_if(reservations_, [sequence](const auto& row) { return row.sequence == sequence; })) ++reservation_revision_;
}
void CommandScheduler::note_execution(const std::uint64_t sequence, const std::uint64_t tick) {
    const std::lock_guard lock(mutex_);
    for (auto& row : reservations_) if (row.sequence == sequence) row.execution_tick = tick;
}
void CommandScheduler::present_event_gap(const std::uint64_t lost_through) {
    const std::lock_guard lock(mutex_);
    // Event-log ticks count completed steps; a command at tick N appears at N + 1.
    if (std::erase_if(reservations_, [lost_through](const auto& row) {
        return row.execution_tick && *row.execution_tick < lost_through;
    })) ++reservation_revision_;
}

tactical::ProductionCounts pending_production_counts(tactical::ProductionCounts counts,
    const tactical::EconomyView& ledger, const std::uint64_t completed_tick,
    const std::span<const PendingCommand> pending, const PendingBuildOption& option_of,
    const tactical::TypeId type) {
    std::vector<PendingCommand> unexecuted;
    for (const auto& row : pending) {
        if (!row.execution_tick || *row.execution_tick >= completed_tick) unexecuted.push_back(row);
    }
    const auto projected = pending_economy(ledger, unexecuted, option_of);
    std::uint64_t queued = 0;
    for (const auto& queue : projected.queues) queued += static_cast<std::uint64_t>(std::count_if(queue.begin(), queue.end(),
        [type](const auto& entry) { return entry.type == type; }));
    // Counts and ledger share a live read; only commands outside that world are projected.
    const auto before = counts.queued_player;
    const auto replace = [before, queued](const std::uint64_t value) { return value - std::min(value, before) + queued; };
    counts.current_player = replace(counts.current_player);
    counts.current_allies = replace(counts.current_allies);
    counts.queued_player = queued;
    counts.queued_allies = replace(counts.queued_allies);
    return counts;
}
std::vector<PendingCommand> CommandScheduler::reservations() const {
    const std::lock_guard lock(mutex_);
    return reservations_;
}
bool CommandScheduler::consumed(const TacticalIntent& intent) const {
    const std::lock_guard lock(mutex_);
    return std::any_of(reservations_.begin(), reservations_.end(), [&](const auto& row) { return consumes(row.intent, intent); });
}
std::uint64_t CommandScheduler::reservation_revision() const {
    const std::lock_guard lock(mutex_);
    return reservation_revision_;
}

core::Result<tactical::CommandPayload> command_payload(const TacticalIntent& intent) {
    using Payload = core::Result<tactical::CommandPayload>;
    switch (intent.verb) {
    case TacticalVerb::gift:
        if (intent.gift_recipient.value == 0)
            return Payload::failure(failure(diagnostic_codes::invalid_intent, "gift needs a recipient seat"));
        return Payload::success(tactical::GiftPayload{intent.gift_recipient});
    case TacticalVerb::stop: return Payload::success(tactical::StopPayload{});
    case TacticalVerb::beacon: return Payload::success(tactical::BeaconPayload{intent.destination});
    case TacticalVerb::move: return Payload::success(tactical::MovePayload{intent.destination, intent.through_hazards, intent.append});
    case TacticalVerb::attack:
        if (intent.target == sim::invalid_entity_id) {
            return Payload::failure(failure(diagnostic_codes::invalid_intent, "an attack needs a target unit"));
        }
        return Payload::success(tactical::AttackPayload{intent.target, intent.hardpoint});
    case TacticalVerb::special_fire:
        if (intent.target == sim::invalid_entity_id || intent.units.size() != 1)
            return Payload::failure(failure(diagnostic_codes::invalid_intent, "special fire needs one source and a target"));
        return Payload::success(tactical::SpecialFirePayload{intent.target, tactical::SpecialFireAdmission::player});
    case TacticalVerb::attack_move:
        return Payload::success(tactical::AttackMovePayload{intent.destination, intent.target});
    case TacticalVerb::guard:
        return Payload::success(tactical::GuardPayload{intent.destination, intent.target});
    case TacticalVerb::face:
        return Payload::failure(failure(diagnostic_codes::unsupported_intent,
            "face orders have no tactical command in rules v1"));
    case TacticalVerb::ability:
        // #76: a modelled unit ability becomes an ability command (space-abilities AB-50).
        if (intent.unit_ability != tactical::AbilityKind::none) {
            // WAD-38: BARRAGE activates on a world point through its area command.
            if (intent.unit_ability == tactical::AbilityKind::barrage
                && intent.ability_action == tactical::AbilityAction::activate) {
                return Payload::success(tactical::AreaAbilityPayload{intent.unit_ability, intent.destination});
            }
            tactical::AbilityPayload payload{intent.unit_ability, intent.ability_action};
            // WHE-61, WAB-70: WEAKEN_ENEMY and BUZZ_DROIDS switch on at the clicked world point.
            if ((intent.unit_ability == tactical::AbilityKind::weaken_enemy
                    || intent.unit_ability == tactical::AbilityKind::fow_reveal_ping
                    || intent.unit_ability == tactical::AbilityKind::buzz_droids
                    || intent.unit_ability == tactical::AbilityKind::corrupt_systems)
                && intent.ability_action == tactical::AbilityAction::activate) payload.position = intent.destination;
            // AB-61, WHE-24/57: targeted activations carry the world click's object identity.
            if ((intent.unit_ability == tactical::AbilityKind::ion_cannon_shot
                || intent.unit_ability == tactical::AbilityKind::concentrate_fire
                || intent.unit_ability == tactical::AbilityKind::energy_weapon
                || intent.unit_ability == tactical::AbilityKind::tractor_beam
                || intent.unit_ability == tactical::AbilityKind::leech_shields)
                && intent.ability_action == tactical::AbilityAction::activate) {
                if (intent.target == sim::invalid_entity_id) {
                    return Payload::failure(failure(diagnostic_codes::invalid_intent, "this ability needs a target unit"));
                }
                payload.target = intent.target;
                payload.target_hardpoint = intent.hardpoint;
            }
            return Payload::success(payload);
        }
        return Payload::failure(failure(diagnostic_codes::unsupported_intent,
            "targeted special abilities have no tactical command yet"));
    case TacticalVerb::repair_hardpoint:
        if (intent.hardpoint >= 255) return Payload::failure(failure(diagnostic_codes::invalid_intent, "invalid repair hardpoint"));
        return Payload::success(tactical::RepairHardpointPayload{intent.hardpoint});
    case TacticalVerb::buy: return Payload::success(tactical::BuyPayload{intent.type});
    case TacticalVerb::pad_build: return Payload::success(tactical::PadBuildPayload{intent.type});
    case TacticalVerb::intentional_quit: return Payload::success(tactical::QuitPayload{});
    case TacticalVerb::pad_sell: return Payload::success(tactical::PadSellPayload{});
    case TacticalVerb::cancel:
        if (intent.queue_entry_id == 0) return Payload::failure(failure(diagnostic_codes::invalid_intent,
            "cancel requires a displayed queue entry identity"));
        return Payload::success(tactical::CancelPayload{static_cast<std::uint32_t>(intent.queue), 0, intent.queue_entry_id});
    case TacticalVerb::reinforce: return Payload::success(tactical::ReinforcePayload{intent.type, intent.destination, intent.reinforcement_token, intent.reinforcement_facing});
    }
    return Payload::failure(failure(diagnostic_codes::invalid_intent, "unknown order verb"));
}

CommandScheduler::CommandScheduler(
    const tactical::PlayerId player, const std::uint64_t first_sequence, const std::uint64_t first_tick,
    std::function<std::uint64_t()> intent_epoch, const sim::SeatId controller_seat)
    : player_(player), controller_seat_(controller_seat), next_sequence_(first_sequence), open_tick_(first_tick), intent_epoch_(std::move(intent_epoch)),
      epoch_state_(intent_epoch_ ? std::make_shared<EpochState>() : nullptr) {}

CommandScheduler::IntentEpochScope::IntentEpochScope(IntentEpochScope&& other) noexcept
    : state_(std::move(other.state_)), previous_(other.previous_), epoch_(other.epoch_) {}

CommandScheduler::IntentEpochScope::~IntentEpochScope() {
    if (!state_) return;
    const std::lock_guard lock(state_->mutex);
    state_->epoch = previous_;
}

CommandScheduler::IntentEpochScope CommandScheduler::capture_intent_epoch() {
    if (!epoch_state_) return {};
    const std::lock_guard lock(epoch_state_->mutex);
    const auto previous = epoch_state_->epoch;
    const auto epoch = previous ? *previous : intent_epoch_();
    epoch_state_->epoch = epoch;
    return IntentEpochScope(epoch_state_, previous, epoch);
}

std::uint64_t CommandScheduler::producer_epoch() const {
    const std::lock_guard lock(epoch_state_->mutex);
    return epoch_state_->epoch ? *epoch_state_->epoch : intent_epoch_();
}

std::optional<std::uint64_t> CommandScheduler::pending_intent_epoch() const {
    if (!epoch_state_) return {};
    const std::lock_guard lock(mutex_);
    const std::lock_guard epoch_lock(epoch_state_->mutex);
    auto epoch = epoch_state_->epoch;
    if (!queue_.empty()) epoch = epoch ? std::min(*epoch, queue_.front().key.tick) : queue_.front().key.tick;
    return epoch;
}

core::Result<void> CommandScheduler::issue(const TacticalIntent& intent) {
    std::vector<sim::EntityId> units = intent.units;
    std::sort(units.begin(), units.end());
    units.erase(std::unique(units.begin(), units.end()), units.end());
    // #530: a buy names its one station; a cancel or reinforce names no unit.
    const bool economy = intent.verb == TacticalVerb::buy || intent.verb == TacticalVerb::cancel
        || intent.verb == TacticalVerb::reinforce || intent.verb == TacticalVerb::pad_build || intent.verb == TacticalVerb::pad_sell;
    const bool quitting = intent.verb == TacticalVerb::intentional_quit || intent.verb == TacticalVerb::beacon;
    if (quitting && !units.empty()) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_intent, "this player command names no unit"));
    }
    if ((intent.verb == TacticalVerb::buy || intent.verb == TacticalVerb::pad_build || intent.verb == TacticalVerb::pad_sell || intent.verb == TacticalVerb::repair_hardpoint) && units.size() != 1) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_intent, "a buy names one station"));
    }
    if (economy && intent.verb != TacticalVerb::buy && intent.verb != TacticalVerb::pad_build
        && intent.verb != TacticalVerb::pad_sell && !units.empty()) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_intent, "a cancel or reinforce names no unit"));
    }
    if (units.empty() && !economy && !quitting) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_intent, "an order needs at least one unit"));
    }
    if (!units.empty() && units.front() == sim::invalid_entity_id) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_intent, "an order lists unit ID zero"));
    }
    if (units.size() > tactical::max_units_per_command) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_intent,
            "an order lists " + std::to_string(units.size()) + " units; the limit is "
                + std::to_string(tactical::max_units_per_command)));
    }
    auto payload = command_payload(intent);
    if (!payload) return core::Result<void>::failure(payload.error());
    const std::lock_guard lock(mutex_);
    if (reservations_enabled_ && std::any_of(reservations_.begin(), reservations_.end(),
        [&](const auto& row) { return consumes(row.intent, intent); }))
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_intent, "action already pending in presentation"));
    if (intent_epoch_) open_tick_ = std::max(open_tick_, producer_epoch());
    queue_.push_back(tactical::PlayerCommand{
        sim::CommandKey{open_tick_, player_, next_sequence_++}, std::move(units), std::move(payload).value()});
    if (controller_seat_.value != 0) {
        queue_.back().origin = tactical::CommandOrigin::seat;
        queue_.back().issuing_seat = controller_seat_;
    }
    const bool reserves = economy || quitting || intent.verb == TacticalVerb::ability
        || intent.verb == TacticalVerb::repair_hardpoint || intent.verb == TacticalVerb::special_fire;
    if (reservations_enabled_ && reserves) {
        reservations_.push_back({open_tick_, queue_.back().key.sequence, intent});
        ++reservation_revision_;
    }
    if (issue_observer_) issue_observer_(queue_.back().key.sequence, open_tick_);
    return core::Result<void>::success();
}

void CommandScheduler::discard_unit_orders(const sim::EntityId entity) {
    const std::lock_guard lock(mutex_);
    std::erase_if(queue_, [entity](const auto& command) {
        return command.units.size() == 1 && command.units.front() == entity;
    });
    for (auto& command : queue_) std::erase(command.units, entity);
    for (auto& reservation : reservations_)
        if (!reservation.handed_off) std::erase(reservation.intent.units, entity);
    std::erase_if(reservations_, [&](const auto& row) {
        return !row.handed_off && std::none_of(queue_.begin(), queue_.end(),
            [&](const auto& command) { return command.key.sequence == row.sequence; });
    });
    ++reservation_revision_;
}

std::vector<tactical::PlayerCommand> CommandScheduler::take(const std::uint64_t next_tick) {
    std::vector<tactical::PlayerCommand> commands;
    {
        const std::lock_guard lock(mutex_);
        if (!intent_epoch_) open_tick_ = std::max(open_tick_, next_tick + 1U);
        const auto due = intent_epoch_ ? queue_.end() : std::find_if(queue_.begin(), queue_.end(),
            [next_tick](const tactical::PlayerCommand& command) { return command.key.tick > next_tick; });
        commands.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(due));
        queue_.erase(queue_.begin(), due);
        if (!intent_epoch_) for (const auto& command : commands) {
            for (auto& row : reservations_) if (row.sequence == command.key.sequence) {
                row.tick = next_tick; row.handed_off = true; row.execution_tick = next_tick;
            }
        }
    }
    if (!intent_epoch_)
        for (tactical::PlayerCommand& command : commands) command.key.tick = next_tick;
    return commands;
}

std::size_t CommandScheduler::pending() const {
    const std::lock_guard lock(mutex_);
    return queue_.size();
}

std::uint64_t CommandScheduler::next_sequence() const {
    const std::lock_guard lock(mutex_);
    return next_sequence_;
}

std::uint64_t CommandScheduler::open_tick() const {
    const std::lock_guard lock(mutex_);
    return intent_epoch_ ? std::max(open_tick_, producer_epoch()) : open_tick_;
}

void OrderInput::set_selection(std::vector<sim::EntityId> units) {
    std::sort(units.begin(), units.end());
    units.erase(std::unique(units.begin(), units.end()), units.end());
    std::erase(units, sim::invalid_entity_id);
    selection_ = std::move(units);
}

core::Result<void> OrderInput::send(TacticalIntent intent) {
    intent.units = selection_;
    return sink_->issue(intent);
}

core::Result<void> OrderInput::stop(const CommandOrigin origin) {
    if (selection_.empty()) return core::Result<void>::success();
    mode_ = OrderMode::none;
    TacticalIntent intent;
    intent.verb = TacticalVerb::stop;
    intent.origin = origin;
    return send(std::move(intent));
}

core::Result<void> OrderInput::ability(const std::uint32_t ability_crc, const WorldPick& pick, const CommandOrigin origin) {
    if (selection_.empty()) return core::Result<void>::success();
    TacticalIntent intent;
    intent.verb = TacticalVerb::ability;
    intent.destination = pick.point;
    intent.target = pick.entity;
    intent.ability = ability_crc;
    intent.origin = origin;
    return send(std::move(intent));
}

core::Result<bool> OrderInput::world_command(const WorldPick& pick, const CommandOrigin origin,
    const OrderModifiers modifiers) {
    if (selection_.empty()) return core::Result<bool>::success(false);
    TacticalIntent intent;
    intent.origin = origin;
    const bool hostile_unit = pick.entity != sim::invalid_entity_id && pick.hostile;
    // OR-01: without an armed mode Ctrl and Alt guard, Ctrl alone attack-moves.
    auto mode = mode_;
    if (mode == OrderMode::none && modifiers.ctrl) mode = modifiers.alt ? OrderMode::guard : OrderMode::attack_move;
    if (mode == OrderMode::waypoint && modifiers.ctrl && modifiers.alt) mode = OrderMode::guard;
    switch (mode) {
    case OrderMode::attack_move:
    case OrderMode::guard:
        if (hostile_unit) {
            intent.verb = TacticalVerb::attack;
            intent.target = pick.entity;
            intent.hardpoint = pick.hardpoint;
            break;
        }
        intent.verb = mode == OrderMode::guard ? TacticalVerb::guard : TacticalVerb::attack_move;
        // OP-01: an order that names a unit carries no point.
        if (mode == OrderMode::guard && pick.entity != sim::invalid_entity_id && pick.own && !pick.selected) {
            intent.target = pick.entity;
        } else {
            intent.destination = pick.point;
        }
        break;
    case OrderMode::attack:
        if (!hostile_unit) return core::Result<bool>::success(false);
        intent.verb = TacticalVerb::attack;
        intent.target = pick.entity;
        intent.hardpoint = pick.hardpoint;
        break;
    case OrderMode::waypoint:
    case OrderMode::move:
        intent.verb = TacticalVerb::move;
        intent.destination = pick.point;
        break;
    case OrderMode::none:
        if (hostile_unit) {
            intent.verb = TacticalVerb::attack;
            intent.target = pick.entity;
            intent.hardpoint = pick.hardpoint;
        } else {
            intent.verb = TacticalVerb::move;
            intent.destination = pick.point;
        }
        break;
    }
    intent.append = intent.verb == TacticalVerb::move && (mode == OrderMode::waypoint || (modifiers.alt && !modifiers.ctrl));
    intent.through_hazards = intent.verb == TacticalVerb::move && modifiers.through_hazards;
    if (auto sent = send(std::move(intent)); !sent) return core::Result<bool>::failure(sent.error());
    if (mode_ != OrderMode::waypoint) mode_ = OrderMode::none;
    return core::Result<bool>::success(true);
}

} // namespace eawr::presentation::ui
