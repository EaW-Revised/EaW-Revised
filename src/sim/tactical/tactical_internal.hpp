#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
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

[[nodiscard]] core::Diagnostic diagnostic(
    std::string_view code,
    std::string message,
    std::string_view logical_path = {},
    core::Severity severity = core::Severity::error);

// Payload bytes that follow the unit list header, by opcode (0 for an unknown opcode).
[[nodiscard]] std::size_t payload_prefix_size(std::uint8_t opcode) noexcept;
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
