#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/commands.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::sim {

inline constexpr std::uint16_t replay_format_version = 1;
inline constexpr std::uint32_t simulation_rules_version = 1;
inline constexpr std::uint32_t state_encoding_version = 1;
inline constexpr std::size_t replay_header_size = 96;
inline constexpr std::size_t replay_max_bytes = 256U * 1024U * 1024U;
inline constexpr std::uint64_t replay_max_entities = 1'000'000;
inline constexpr std::uint64_t replay_max_commands = 1'000'000;
inline constexpr std::uint64_t replay_max_ticks = 1'000'000;

struct Replay {
    std::uint32_t tick_numerator{};
    std::uint32_t tick_denominator{};
    std::uint64_t seed{};
    std::uint64_t final_tick_count{};
    std::array<std::uint8_t, 32> content_identity{};
    std::vector<EntityState> initial_entities;
    std::vector<Command> commands;
};

[[nodiscard]] core::Result<Replay> parse_replay(
    std::span<const std::uint8_t> bytes,
    std::string_view logical_path = {});
[[nodiscard]] core::Result<std::vector<std::uint8_t>> write_replay(const Replay& replay);

[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes);

namespace diagnostic_codes {
inline constexpr std::string_view replay_malformed = "EAWR-SIM-0101";
inline constexpr std::string_view replay_version = "EAWR-SIM-0102";
inline constexpr std::string_view replay_resource_limit = "EAWR-SIM-0103";
inline constexpr std::string_view replay_order = "EAWR-SIM-0104";
inline constexpr std::string_view invalid_command = "EAWR-SIM-0105";
inline constexpr std::string_view id_exhausted = "EAWR-SIM-0106";
inline constexpr std::string_view movement_overflow = "EAWR-SIM-0107";
inline constexpr std::string_view simulation_complete = "EAWR-SIM-0108";
inline constexpr std::string_view worker_failure = "EAWR-SIM-0109";
} // namespace diagnostic_codes

} // namespace eawr::sim
