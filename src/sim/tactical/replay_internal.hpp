#pragma once

#include "eawr/sim/tactical/replay.hpp"

#include "../replay_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace eawr::sim::tactical::replay_detail {

using sim::detail::Reader;

constexpr std::array<std::uint8_t, 8> replay_magic{'E', 'A', 'W', 'R', 'P', 'L', 'Y', 0};
constexpr std::uint32_t math_version = 1;
constexpr std::uint32_t fractional_bits = 24;

template <typename T>
[[nodiscard]] core::Result<T> fail(
    const std::string_view code,
    std::string message,
    const std::string_view logical_path) {
    return core::Result<T>::failure(detail::diagnostic(code, std::move(message), logical_path));
}
[[nodiscard]] inline std::string command_label(const std::size_t index, const PlayerCommand& command) {
    return "command " + std::to_string(index) + " (tick " + std::to_string(command.key.tick)
        + ", player " + std::to_string(command.key.player_id) + ", sequence "
        + std::to_string(command.key.sequence) + ")";
}


} // namespace eawr::sim::tactical::replay_detail
