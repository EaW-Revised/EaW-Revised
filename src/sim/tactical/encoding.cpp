#include "tactical_internal.hpp"

#include "../replay_internal.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace eawr::sim::tactical {

std::string_view to_string(const OrderKind kind) noexcept {
    switch (kind) {
    case OrderKind::none:
        return "none";
    case OrderKind::stop:
        return "stop";
    case OrderKind::move:
        return "move";
    case OrderKind::attack:
        return "attack";
    case OrderKind::damage:
        return "damage";
    case OrderKind::face:
        return "face";
    case OrderKind::attack_move:
        return "attack_move";
    case OrderKind::guard:
        return "guard";
    case OrderKind::ability:
        return "ability";
    case OrderKind::buy:
        return "buy";
    case OrderKind::cancel:
        return "cancel";
    case OrderKind::reinforce:
        return "reinforce";
    case OrderKind::pad_build:
        return "pad_build";
    case OrderKind::pad_sell:
        return "pad_sell";
    case OrderKind::credit_grant:
        return "credit_grant";
    case OrderKind::intentional_quit: return "intentional_quit";
    case OrderKind::area_ability:
        return "area_ability";
    case OrderKind::manual_target:
        return "manual_target";
    case OrderKind::reveal_all:
        return "reveal_all";
    }
    return "unknown";
}

std::string_view to_string(const EventKind kind) noexcept {
    switch (kind) {
    case EventKind::order_accepted:
        return "order_accepted";
    case EventKind::order_rejected:
        return "order_rejected";
    case EventKind::hardpoint_destroyed:
        return "hardpoint_destroyed";
    case EventKind::unit_destroyed:
        return "unit_destroyed";
    case EventKind::victory:
        return "victory";
    case EventKind::spin_away_started:
        return "spin_away_started";
    case EventKind::spin_away_ended:
        return "spin_away_ended";
    case EventKind::reinforcement_unloaded:
        return "reinforcement_unloaded";
    case EventKind::station_replaced:
        return "station_replaced";
    case EventKind::pad_captured: return "pad_captured";
    case EventKind::pad_construction_started: return "pad_construction_started";
    case EventKind::pad_construction_completed: return "pad_construction_completed";
    case EventKind::player_quit: return "player_quit";
    case EventKind::pad_structure_sold: return "pad_structure_sold";
    case EventKind::ability_cancelled: return "ability_cancelled";
    case EventKind::ability_ready: return "ability_ready";
    case EventKind::manual_target_timeout: return "manual_target_timeout";
    }
    return "unknown";
}

std::string_view to_string(const RejectReason reason) noexcept {
    switch (reason) {
    case RejectReason::none:
        return "none";
    case RejectReason::unit_not_live:
        return "unit_not_live";
    case RejectReason::unit_not_owned:
        return "unit_not_owned";
    case RejectReason::target_not_live:
        return "target_not_live";
    case RejectReason::target_not_hostile:
        return "target_not_hostile";
    case RejectReason::not_damageable:
        return "not_damageable";
    case RejectReason::hardpoint_invalid:
        return "hardpoint_invalid";
    case RejectReason::target_is_unit:
        return "target_is_unit";
    case RejectReason::ability_unavailable:
        return "ability_unavailable";
    case RejectReason::cannot_produce:
        return "cannot_produce";
    case RejectReason::queue_full:
        return "queue_full";
    case RejectReason::insufficient_credits:
        return "insufficient_credits";
    case RejectReason::no_queue_entry:
        return "no_queue_entry";
    case RejectReason::not_in_pool:
        return "not_in_pool";
    case RejectReason::no_population_room:
        return "no_population_room";
    case RejectReason::invalid_position:
        return "invalid_position";
    case RejectReason::no_economy:
        return "no_economy";
    case RejectReason::arriving:
        return "arriving";
    case RejectReason::battle_decided:
        return "battle_decided";
    }
    return "unknown";
}

namespace detail {

core::Diagnostic diagnostic(
    const std::string_view code,
    std::string message,
    const std::string_view logical_path,
    const core::Severity severity) {
    return core::Diagnostic{
        .code = std::string(code),
        .severity = severity,
        .message = std::move(message),
        .logical_path = logical_path.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{std::string(logical_path)},
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("tactical-v1"),
    };
}

std::size_t payload_prefix_size(const std::uint8_t opcode) noexcept {
    switch (opcode) {
    case 1:
        return 0;
    case 2:
    case opcode_hazard_move:
        return 24;
    case 3:
        return 8;
    case 4:
        return 16;
    case 5:
        return 24;
    case 6:
    case 7:
        return 32;
    case 8:
        return 8;
    case opcode_attack_hardpoint:
    case opcode_manual_target:
        return 16;
    case 9:  // #530 buy: type
    case opcode_reveal_all: // V-20: player ID and zero reserved word
    case opcode_pad_build: // WBP-09: UC type
    case opcode_credit_grant: // SAE-07: Q24 credits
    case 10: // #530 cancel: queue, index
        return 8;
    case 11: // #530 reinforce: type, position
    case opcode_area_ability: // ability kind, reserved word, world point
        return 32;
    case opcode_reserved_reinforce: // SAE-11: type, position, purchase token
        return 40;
    default:
        return 0;
    }
}

std::uint8_t command_opcode(const PlayerCommand& command) noexcept {
    if (std::holds_alternative<RevealAllPayload>(command.payload)) return opcode_reveal_all;
    if (const auto* move = std::get_if<MovePayload>(&command.payload);
        move != nullptr && move->through_hazards) return opcode_hazard_move;
    if (const auto* reinforce = std::get_if<ReinforcePayload>(&command.payload);
        reinforce != nullptr && reinforce->pool_token != 0) return opcode_reserved_reinforce;
    if (std::holds_alternative<QuitPayload>(command.payload)) return opcode_intentional_quit;
    if (std::holds_alternative<PadSellPayload>(command.payload)) return opcode_pad_sell;
    if (std::holds_alternative<AreaAbilityPayload>(command.payload)) return opcode_area_ability;
    if (std::holds_alternative<ManualTargetPayload>(command.payload)) return opcode_manual_target;
    if (std::holds_alternative<CreditGrantPayload>(command.payload)) return opcode_credit_grant;
    if (std::holds_alternative<PadBuildPayload>(command.payload)) return opcode_pad_build;
    // #531: an attack on one hardpoint has its own opcode, so attacks on a unit keep opcode 3 and their bytes.
    if (const auto* attack = std::get_if<AttackPayload>(&command.payload);
        attack != nullptr && attack->hardpoint != attack_hull) {
        return opcode_attack_hardpoint;
    }
    return static_cast<std::uint8_t>(order_kind(command.payload));
}

std::size_t command_body_size(const PlayerCommand& command) noexcept {
    // A targeted ability command (#561) appends its uint64 target.
    const auto* ability = std::get_if<AbilityPayload>(&command.payload);
    const std::size_t target = ability != nullptr ? (ability->position ? 24U : ability->target != invalid_entity_id ? 8U : 0U) : 0U;
    return command_common_size
        + payload_prefix_size(command_opcode(command)) + target
        + unit_list_header_size + 8U * command.units.size();
}

void append_player(std::vector<std::uint8_t>& bytes, const Player& player) {
    sim::detail::append_u32(bytes, player.player_id);
    sim::detail::append_u32(bytes, player.team_id);
    sim::detail::append_u64(bytes, player.faction_id);
    sim::detail::append_u32(bytes, player.flags);
    sim::detail::append_u32(bytes, 0);
}

void append_unit_record(std::vector<std::uint8_t>& bytes, const UnitState& unit) {
    sim::detail::append_u64(bytes, unit.entity_id);
    sim::detail::append_u64(bytes, unit.type_id);
    sim::detail::append_u32(bytes, unit.owner);
    sim::detail::append_u32(bytes, 0);
    sim::detail::append_i64(bytes, unit.position.x.raw());
    sim::detail::append_i64(bytes, unit.position.y.raw());
    sim::detail::append_i64(bytes, unit.position.z.raw());
    sim::detail::append_i64(bytes, unit.rotation.x.raw());
    sim::detail::append_i64(bytes, unit.rotation.y.raw());
    sim::detail::append_i64(bytes, unit.rotation.z.raw());
    sim::detail::append_i64(bytes, unit.rotation.w.raw());
}

void append_order(std::vector<std::uint8_t>& bytes, const Order& order) {
    sim::detail::append_u64(bytes, order.issued_tick);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(order.kind));
    // #531: the reserved word holds the ordered hardpoint's index plus one, zero for none.
    sim::detail::append_u32(bytes, order.hardpoint == attack_hull ? 0U : order.hardpoint + 1U);
    sim::detail::append_i64(bytes, order.destination.x.raw());
    sim::detail::append_i64(bytes, order.destination.y.raw());
    sim::detail::append_i64(bytes, order.destination.z.raw());
    sim::detail::append_u64(bytes, order.target);
}

void append_command(std::vector<std::uint8_t>& bytes, const PlayerCommand& command) {
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(command_body_size(command)));
    sim::detail::append_u64(bytes, command.key.tick);
    sim::detail::append_u32(bytes, command.key.player_id);
    sim::detail::append_u64(bytes, command.key.sequence);
    bytes.push_back(command_opcode(command));
    bytes.push_back(0);
    sim::detail::append_u16(bytes, 0);
    if (const auto* reveal = std::get_if<RevealAllPayload>(&command.payload)) {
        sim::detail::append_u32(bytes, reveal->player);
        sim::detail::append_u32(bytes, 0);
    } else if (const auto* manual = std::get_if<ManualTargetPayload>(&command.payload)) {
        sim::detail::append_u64(bytes, manual->target);
        sim::detail::append_u32(bytes, manual->hardpoint);
        sim::detail::append_u32(bytes, 0);
    } else if (const auto* area = std::get_if<AreaAbilityPayload>(&command.payload)) {
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(area->ability));
        sim::detail::append_u32(bytes, 0);
        sim::detail::append_i64(bytes, area->point.x.raw());
        sim::detail::append_i64(bytes, area->point.y.raw());
        sim::detail::append_i64(bytes, area->point.z.raw());
    } else if (const auto* move = std::get_if<MovePayload>(&command.payload)) {
        sim::detail::append_i64(bytes, move->destination.x.raw());
        sim::detail::append_i64(bytes, move->destination.y.raw());
        sim::detail::append_i64(bytes, move->destination.z.raw());
    } else if (const auto* attack = std::get_if<AttackPayload>(&command.payload)) {
        sim::detail::append_u64(bytes, attack->target);
        if (attack->hardpoint != attack_hull) {
            sim::detail::append_u32(bytes, attack->hardpoint);
            sim::detail::append_u32(bytes, 0);
        }
    } else if (const auto* damage = std::get_if<DamagePayload>(&command.payload)) {
        sim::detail::append_i64(bytes, damage->amount.raw());
        sim::detail::append_u32(bytes, damage->hardpoint);
        sim::detail::append_u32(bytes, 0);
    } else if (const auto* face = std::get_if<FacePayload>(&command.payload)) {
        sim::detail::append_i64(bytes, face->target.x.raw());
        sim::detail::append_i64(bytes, face->target.y.raw());
        sim::detail::append_i64(bytes, face->target.z.raw());
    } else if (const auto* attack_move = std::get_if<AttackMovePayload>(&command.payload)) {
        sim::detail::append_i64(bytes, attack_move->destination.x.raw());
        sim::detail::append_i64(bytes, attack_move->destination.y.raw());
        sim::detail::append_i64(bytes, attack_move->destination.z.raw());
        sim::detail::append_u64(bytes, attack_move->target);
    } else if (const auto* guard = std::get_if<GuardPayload>(&command.payload)) {
        sim::detail::append_i64(bytes, guard->destination.x.raw());
        sim::detail::append_i64(bytes, guard->destination.y.raw());
        sim::detail::append_i64(bytes, guard->destination.z.raw());
        sim::detail::append_u64(bytes, guard->target);
    } else if (const auto* ability = std::get_if<AbilityPayload>(&command.payload)) {
        // #561: a targeted ability sets the first reserved field to 1 and carries its target
        // hardpoint and target; any other ability command encodes exactly as before.
        const bool targeted = ability->target != invalid_entity_id;
        bytes.push_back(static_cast<std::uint8_t>(ability->ability));
        bytes.push_back(static_cast<std::uint8_t>(ability->action));
        sim::detail::append_u16(bytes, ability->position ? 2U : targeted ? 1U : 0U);
        sim::detail::append_u32(bytes, targeted ? ability->target_hardpoint : 0U);
        if (targeted) sim::detail::append_u64(bytes, ability->target);
        if (ability->position) {
            sim::detail::append_i64(bytes, ability->position->x.raw());
            sim::detail::append_i64(bytes, ability->position->y.raw());
            sim::detail::append_i64(bytes, ability->position->z.raw());
        }
    } else if (const auto* buy = std::get_if<BuyPayload>(&command.payload)) {
        sim::detail::append_u64(bytes, buy->type);
    } else if (const auto* grant = std::get_if<CreditGrantPayload>(&command.payload)) {
        sim::detail::append_i64(bytes, grant->amount.raw());
    } else if (const auto* pad = std::get_if<PadBuildPayload>(&command.payload)) {
        sim::detail::append_u64(bytes, pad->type);
    } else if (const auto* cancel = std::get_if<CancelPayload>(&command.payload)) {
        sim::detail::append_u32(bytes, cancel->queue);
        sim::detail::append_u32(bytes, cancel->index);
    } else if (const auto* reinforce = std::get_if<ReinforcePayload>(&command.payload)) {
        sim::detail::append_u64(bytes, reinforce->type);
        sim::detail::append_i64(bytes, reinforce->position.x.raw());
        sim::detail::append_i64(bytes, reinforce->position.y.raw());
        sim::detail::append_i64(bytes, reinforce->position.z.raw());
        if (reinforce->pool_token != 0) sim::detail::append_u64(bytes, reinforce->pool_token);
    }
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(command.units.size()));
    sim::detail::append_u32(bytes, 0);
    for (const auto unit : command.units) {
        sim::detail::append_u64(bytes, unit);
    }
}

void append_event(std::vector<std::uint8_t>& bytes, const Event& event) {
    sim::detail::append_u64(bytes, event.tick);
    sim::detail::append_u32(bytes, event.player);
    bytes.push_back(static_cast<std::uint8_t>(event.kind));
    bytes.push_back(static_cast<std::uint8_t>(event.order));
    bytes.push_back(static_cast<std::uint8_t>(event.reason));
    bytes.push_back(event.hardpoint);
    sim::detail::append_u64(bytes, event.sequence);
    sim::detail::append_u64(bytes, event.unit);
}

core::Result<void> validate_command_shape(
    const PlayerCommand& command,
    const std::vector<Player>& players,
    const std::string_view context,
    const std::string_view logical_path) {
    const auto issuer = std::find_if(players.begin(), players.end(),
        [&command](const Player& player) { return player.player_id == command.key.player_id; });
    if (issuer == players.end()) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_issuer,
            std::string(context) + ": issuer is not a declared player", logical_path));
    }
    if (!issuer->commandable()) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_issuer,
            std::string(context) + ": issuer is not a commandable player", logical_path));
    }
    if (command.units.size() > max_units_per_command) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::resource_limit,
            std::string(context) + ": unit list exceeds the per-command limit", logical_path));
    }
    // #530: a buy lists exactly its station; a cancel or reinforce lists no unit.
    if ((std::holds_alternative<BuyPayload>(command.payload) || std::holds_alternative<PadBuildPayload>(command.payload)
            || std::holds_alternative<PadSellPayload>(command.payload))
        && command.units.size() != 1) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": a buy lists exactly one station", logical_path));
    }
    if ((std::holds_alternative<CancelPayload>(command.payload)
            || std::holds_alternative<ReinforcePayload>(command.payload) || std::holds_alternative<CreditGrantPayload>(command.payload)
            || std::holds_alternative<QuitPayload>(command.payload) || std::holds_alternative<RevealAllPayload>(command.payload))
        && !command.units.empty()) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": a cancel or reinforce lists no unit", logical_path));
    }
    if (command.units.empty() && !economy_command(command.payload) && !std::holds_alternative<QuitPayload>(command.payload)
        && !std::holds_alternative<RevealAllPayload>(command.payload)) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": unit list is empty", logical_path));
    }
    if (const auto* cancel = std::get_if<CancelPayload>(&command.payload);
        cancel != nullptr && cancel->queue >= build_queue_count) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": cancel names no build queue", logical_path));
    }
    if (const auto* reveal = std::get_if<RevealAllPayload>(&command.payload);
        reveal != nullptr && std::none_of(players.begin(), players.end(),
            [&](const Player& player) { return player.player_id == reveal->player; })) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": reveal recipient is not a declared player", logical_path));
    }
    for (std::size_t index = 0; index < command.units.size(); ++index) {
        if (command.units[index] == invalid_entity_id
            || (index != 0 && command.units[index] <= command.units[index - 1])) {
            return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
                std::string(context) + ": unit IDs must be nonzero and strictly increasing at index "
                    + std::to_string(index),
                logical_path));
        }
    }
    if (const auto* attack = std::get_if<AttackPayload>(&command.payload);
        attack != nullptr && attack->target == invalid_entity_id) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": attack target is entity ID zero", logical_path));
    }
    if (const auto* manual = std::get_if<ManualTargetPayload>(&command.payload);
        manual != nullptr && (manual->target == invalid_entity_id || manual->hardpoint >= 255)) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": manual target requires a nonzero entity and hardpoint below 255", logical_path));
    }
    if (const auto* damage = std::get_if<DamagePayload>(&command.payload);
        damage != nullptr && damage->amount.raw() < 0) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": damage amount is negative", logical_path));
    }
    if (const auto* grant = std::get_if<CreditGrantPayload>(&command.payload);
        grant != nullptr && grant->amount.raw() <= 0) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": credit grant must be positive", logical_path));
    }
    if (const auto* ability = std::get_if<AbilityPayload>(&command.payload)) {
        const auto kind = static_cast<std::uint8_t>(ability->ability);
        const auto action = static_cast<std::uint8_t>(ability->action);
        if (kind == 0 || to_string(ability->ability) == "NONE" || action == 0
            || action > static_cast<std::uint8_t>(AbilityAction::autofire_off)) {
            return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
                std::string(context) + ": unknown ability or ability action", logical_path));
        }
        // AB-61: only switching ION_CANNON_SHOT on takes a target, and it always does.
        const bool aims = (ability->ability == AbilityKind::ion_cannon_shot
            || ability->ability == AbilityKind::concentrate_fire || ability->ability == AbilityKind::energy_weapon
            || ability->ability == AbilityKind::tractor_beam) && ability->action == AbilityAction::activate;
        if (aims != (ability->target != invalid_entity_id)
            || (ability->target == invalid_entity_id && ability->target_hardpoint != 0xffffffffU)) {
            return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
                std::string(context) + ": an ability target is required exactly when a targeted ability switches on",
                logical_path));
        }
        const bool positioned = ability->ability == AbilityKind::weaken_enemy && ability->action == AbilityAction::activate;
        if (positioned != ability->position.has_value())
            return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
                std::string(context) + ": a position is required exactly for weaken activation", logical_path));
        if (ability->position) {
            constexpr auto limit = std::int64_t{1} << 42; // existing Q24 command coordinate bound
            for (const auto value : {ability->position->x, ability->position->y, ability->position->z})
                if (value.raw() < -limit || value.raw() > limit)
                    return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
                        std::string(context) + ": ability position outside the command coordinate bound", logical_path));
        }
    }
    if (const auto* area = std::get_if<AreaAbilityPayload>(&command.payload);
        area != nullptr && area->ability != AbilityKind::barrage) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": area ability requires BARRAGE", logical_path));
    }
    return core::Result<void>::success();
}

} // namespace detail
} // namespace eawr::sim::tactical
