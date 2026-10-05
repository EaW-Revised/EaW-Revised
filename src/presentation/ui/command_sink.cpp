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

core::Result<tactical::CommandPayload> command_payload(const TacticalIntent& intent) {
    using Payload = core::Result<tactical::CommandPayload>;
    switch (intent.verb) {
    case TacticalVerb::stop: return Payload::success(tactical::StopPayload{});
    case TacticalVerb::move: return Payload::success(tactical::MovePayload{intent.destination, intent.through_hazards});
    case TacticalVerb::attack:
        if (intent.target == sim::invalid_entity_id) {
            return Payload::failure(failure(diagnostic_codes::invalid_intent, "an attack needs a target unit"));
        }
        return Payload::success(tactical::AttackPayload{intent.target, intent.hardpoint});
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
            tactical::AbilityPayload payload{intent.unit_ability, intent.ability_action};
            if (intent.unit_ability == tactical::AbilityKind::weaken_enemy
                && intent.ability_action == tactical::AbilityAction::activate) payload.position = intent.destination;
            // AB-61, WHE-24/57: targeted activations carry the world click's object identity.
            if ((intent.unit_ability == tactical::AbilityKind::ion_cannon_shot
                || intent.unit_ability == tactical::AbilityKind::concentrate_fire
                || intent.unit_ability == tactical::AbilityKind::energy_weapon
                || intent.unit_ability == tactical::AbilityKind::tractor_beam)
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
    case TacticalVerb::buy: return Payload::success(tactical::BuyPayload{intent.type});
    case TacticalVerb::pad_build: return Payload::success(tactical::PadBuildPayload{intent.type});
    case TacticalVerb::intentional_quit: return Payload::success(tactical::QuitPayload{});
    case TacticalVerb::pad_sell: return Payload::success(tactical::PadSellPayload{});
    case TacticalVerb::cancel:
        return Payload::success(tactical::CancelPayload{static_cast<std::uint32_t>(intent.queue), intent.index});
    case TacticalVerb::reinforce: return Payload::success(tactical::ReinforcePayload{intent.type, intent.destination});
    }
    return Payload::failure(failure(diagnostic_codes::invalid_intent, "unknown order verb"));
}

CommandScheduler::CommandScheduler(
    const tactical::PlayerId player, const std::uint64_t first_sequence, const std::uint64_t first_tick) noexcept
    : player_(player), next_sequence_(first_sequence), open_tick_(first_tick) {}

core::Result<void> CommandScheduler::issue(const TacticalIntent& intent) {
    std::vector<sim::EntityId> units = intent.units;
    std::sort(units.begin(), units.end());
    units.erase(std::unique(units.begin(), units.end()), units.end());
    // #530: a buy names its one station; a cancel or reinforce names no unit.
    const bool economy = intent.verb == TacticalVerb::buy || intent.verb == TacticalVerb::cancel
        || intent.verb == TacticalVerb::reinforce || intent.verb == TacticalVerb::pad_build || intent.verb == TacticalVerb::pad_sell;
    const bool quitting = intent.verb == TacticalVerb::intentional_quit;
    if (quitting && !units.empty()) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_intent, "intentional quit names no unit"));
    }
    if ((intent.verb == TacticalVerb::buy || intent.verb == TacticalVerb::pad_build || intent.verb == TacticalVerb::pad_sell) && units.size() != 1) {
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
    queue_.push_back(tactical::PlayerCommand{
        sim::CommandKey{open_tick_, player_, next_sequence_++}, std::move(units), std::move(payload).value()});
    return core::Result<void>::success();
}

std::vector<tactical::PlayerCommand> CommandScheduler::take(const std::uint64_t next_tick) {
    std::vector<tactical::PlayerCommand> commands;
    {
        const std::lock_guard lock(mutex_);
        open_tick_ = std::max(open_tick_, next_tick + 1U);
        const auto due = std::find_if(queue_.begin(), queue_.end(),
            [next_tick](const tactical::PlayerCommand& command) { return command.key.tick > next_tick; });
        commands.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(due));
        queue_.erase(queue_.begin(), due);
    }
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
    return open_tick_;
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
    intent.through_hazards = intent.verb == TacticalVerb::move && modifiers.through_hazards;
    if (auto sent = send(std::move(intent)); !sent) return core::Result<bool>::failure(sent.error());
    mode_ = OrderMode::none;
    return core::Result<bool>::success(true);
}

} // namespace eawr::presentation::ui
