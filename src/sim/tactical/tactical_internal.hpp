#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::sim::tactical::detail {

inline constexpr std::size_t player_record_size = 24;
inline constexpr std::size_t unit_record_size = 80;
inline constexpr std::size_t order_record_size = 48;
inline constexpr std::size_t command_common_size = 24;
inline constexpr std::size_t unit_list_header_size = 8;
inline constexpr std::size_t event_record_size = 32;
inline constexpr std::size_t instance_record_size = 144;
inline constexpr std::size_t snapshot_player_record_size = 8;

// An attack on one hardpoint of its target (#531). Opcodes 9 to 11 belong to #556/#574 (see the
// replay opcode registry).
inline constexpr std::uint8_t opcode_attack_hardpoint = 12;
// The highest command opcode the parser accepts. Whichever of #591 and #556/#574 lands second
// changes only this line; an opcode in the range that no branch handles fails as unsupported.
inline constexpr std::uint8_t opcode_pad_build = 13; // coordinator-reserved, WBP-09/10
inline constexpr std::uint8_t opcode_credit_grant = 14; // coordinator-reserved, SAE-07
inline constexpr std::uint8_t opcode_pad_sell = 15; // coordinator-reserved, WBP-30
inline constexpr std::uint8_t opcode_intentional_quit = 16; // coordinator-reserved, WBF-43/48
inline constexpr std::uint8_t opcode_area_ability = 17; // coordinator-reserved, WAD-38
inline constexpr std::uint8_t opcode_manual_target = 18; // coordinator-reserved, WAD-39
inline constexpr std::uint8_t opcode_reserved_reinforce = 19; // coordinator-reserved, SAE-11
inline constexpr std::uint8_t opcode_hazard_move = 20; // coordinator-reserved, WHZ-08a
inline constexpr std::uint8_t opcode_reveal_all = 21; // coordinator-reserved, V-20
inline constexpr std::uint8_t opcode_cancel_entry = 22; // coordinator-reserved, PU-17/63, WPR-31
inline constexpr std::uint8_t opcode_ai_reservation_debit = 23; // coordinator-reserved, WAS-25
inline constexpr std::uint8_t opcode_prepaid_buy = 24; // coordinator-reserved, WAS-26
inline constexpr std::uint8_t opcode_repair_hardpoint = 25; // coordinator-reserved: WSL-40
inline constexpr std::uint8_t opcode_reinforce_facing = 26; // coordinator-reserved: WR-X01
inline constexpr std::uint8_t max_command_opcode = opcode_reinforce_facing;

[[nodiscard]] core::Diagnostic diagnostic(
    std::string_view code,
    std::string message,
    std::string_view logical_path = {},
    core::Severity severity = core::Severity::error);

// Payload bytes that follow the unit list header, by opcode (0 for an unknown opcode).
[[nodiscard]] std::size_t payload_prefix_size(std::uint8_t opcode) noexcept;
// The opcode a command is written with: its order kind, or opcode_attack_hardpoint for an attack on one hardpoint (#531).
[[nodiscard]] std::uint8_t command_opcode(const PlayerCommand& command) noexcept;
[[nodiscard]] std::size_t command_body_size(const PlayerCommand& command) noexcept;

void append_player(std::vector<std::uint8_t>& bytes, const Player& player);
void append_unit_record(std::vector<std::uint8_t>& bytes, const UnitState& unit);
void append_order(std::vector<std::uint8_t>& bytes, const Order& order);
void append_command(std::vector<std::uint8_t>& bytes, const PlayerCommand& command);
void append_event(std::vector<std::uint8_t>& bytes, const Event& event);

// Checks that do not depend on session state: issuer declared and commandable, unit list
// size and order, attack target nonzero.
[[nodiscard]] core::Result<void> validate_command_shape(
    const PlayerCommand& command,
    const std::vector<Player>& players,
    std::string_view context,
    std::string_view logical_path);

} // namespace eawr::sim::tactical::detail
