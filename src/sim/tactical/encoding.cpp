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
    default:
        return 0;
    }
}

std::size_t command_body_size(const PlayerCommand& command) noexcept {
    // A targeted ability command (#561) appends its uint64 target.
    const auto* ability = std::get_if<AbilityPayload>(&command.payload);
    const std::size_t target = ability != nullptr && ability->target != invalid_entity_id ? 8U : 0U;
    return command_common_size
        + payload_prefix_size(static_cast<std::uint8_t>(order_kind(command.payload))) + target
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
    sim::detail::append_u32(bytes, 0);
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
    bytes.push_back(static_cast<std::uint8_t>(order_kind(command.payload)));
    bytes.push_back(0);
    sim::detail::append_u16(bytes, 0);
    if (const auto* move = std::get_if<MovePayload>(&command.payload)) {
        sim::detail::append_i64(bytes, move->destination.x.raw());
        sim::detail::append_i64(bytes, move->destination.y.raw());
        sim::detail::append_i64(bytes, move->destination.z.raw());
    } else if (const auto* attack = std::get_if<AttackPayload>(&command.payload)) {
        sim::detail::append_u64(bytes, attack->target);
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
        sim::detail::append_u16(bytes, targeted ? 1U : 0U);
        sim::detail::append_u32(bytes, targeted ? ability->target_hardpoint : 0U);
        if (targeted) sim::detail::append_u64(bytes, ability->target);
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
    if (command.units.empty()) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": unit list is empty", logical_path));
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
    if (const auto* damage = std::get_if<DamagePayload>(&command.payload);
        damage != nullptr && damage->amount.raw() < 0) {
        return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
            std::string(context) + ": damage amount is negative", logical_path));
    }
    if (const auto* ability = std::get_if<AbilityPayload>(&command.payload)) {
        const auto kind = static_cast<std::uint8_t>(ability->ability);
        const auto action = static_cast<std::uint8_t>(ability->action);
        if (kind == 0 || kind > static_cast<std::uint8_t>(AbilityKind::ion_cannon_shot) || action == 0
            || action > static_cast<std::uint8_t>(AbilityAction::autofire_off)) {
            return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
                std::string(context) + ": unknown ability or ability action", logical_path));
        }
        // AB-61: only switching ION_CANNON_SHOT on takes a target, and it always does.
        const bool aims = ability->ability == AbilityKind::ion_cannon_shot && ability->action == AbilityAction::activate;
        if (aims != (ability->target != invalid_entity_id)
            || (ability->target == invalid_entity_id && ability->target_hardpoint != 0xffffffffU)) {
            return core::Result<void>::failure(diagnostic(diagnostic_codes::invalid_command,
                std::string(context) + ": an ability target is required exactly when ION_CANNON_SHOT switches on",
                logical_path));
        }
    }
    return core::Result<void>::success();
}

} // namespace detail
} // namespace eawr::sim::tactical
