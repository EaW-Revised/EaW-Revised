#pragma once

#include "replay_internal.hpp"

namespace eawr::sim::tactical::replay_parse_detail {
using replay_detail::Reader;

[[nodiscard]] core::Result<void> parse_extension_object_turn(
    Reader& reader,
    const std::string_view logical_path,
    std::uint32_t& object_turn_count,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_native_scene(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t length);

[[nodiscard]] core::Result<void> parse_extension_attract_pool(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_beacon_rules(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_skirmish_setup(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_queue_identities(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_garrison_disabled(
    Reader& reader,
    const std::string_view logical_path,
    std::vector<EntityId>& disabled_garrisons,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_free_garrison(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_multiplayer_departure(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_first_free_id(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_campaign(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_carriers(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    std::vector<UnitState>& carriers,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_match_identity(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_cinematic_seats(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_extension_match_policy(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length);

[[nodiscard]] core::Result<void> parse_fixed_header(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    std::uint32_t& header_size,
    std::uint32_t& header_reserved,
    std::uint32_t& player_count,
    std::uint64_t& unit_count,
    std::uint64_t& command_count,
    bool& squadrons,
    bool& extensions);

[[nodiscard]] core::Result<void> parse_header_extensions(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t header_size,
    const bool extensions,
    std::vector<EntityId>& disabled_garrisons,
    std::vector<UnitState>& carriers,
    std::uint32_t& object_turn_count);

[[nodiscard]] core::Result<void> validate_header_counts(
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t header_reserved,
    const std::uint32_t player_count,
    const std::uint64_t unit_count,
    const std::uint64_t command_count,
    const bool squadrons);

[[nodiscard]] core::Result<void> parse_players(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t player_count);

[[nodiscard]] core::Result<void> parse_units(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint64_t unit_count,
    std::vector<EntityId>& disabled_garrisons,
    std::vector<UnitState>& carriers);

[[nodiscard]] core::Result<void> parse_squadrons(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t header_reserved,
    const bool squadrons);

[[nodiscard]] core::Result<void> parse_commands(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint64_t command_count);

[[nodiscard]] core::Result<void> parse_object_turns(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t object_turn_count);

[[nodiscard]] inline bool read_vec3(Reader& reader, math::Vec3& value) noexcept {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t z{};
    if (!reader.read_i64(x) || !reader.read_i64(y) || !reader.read_i64(z)) {
        return false;
    }
    value = {math::Fixed::from_raw(x), math::Fixed::from_raw(y), math::Fixed::from_raw(z)};
    return true;
}

} // namespace eawr::sim::tactical::replay_parse_detail
