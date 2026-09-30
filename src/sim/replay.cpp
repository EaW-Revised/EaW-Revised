#include "eawr/sim/replay.hpp"
#include "eawr/core/sha256.hpp"

#include "replay_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace eawr::sim {
namespace {

constexpr std::array<std::uint8_t, 8> replay_magic{'E', 'A', 'W', 'R', 'P', 'L', 'Y', 0};
constexpr std::uint32_t math_version = 1;
constexpr std::uint32_t fractional_bits = 24;
constexpr std::size_t entity_record_size = 64;
constexpr std::size_t command_common_size = 24;

[[nodiscard]] core::Diagnostic diagnostic(
    const std::string_view code,
    std::string message,
    const std::string_view logical_path = {}) {
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = logical_path.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{std::string(logical_path)},
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("replay-v1"),
    };
}

using detail::Reader;

[[nodiscard]] bool read_vec3(Reader& reader, math::Vec3& value) noexcept {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t z{};
    if (!reader.read_i64(x) || !reader.read_i64(y) || !reader.read_i64(z)) {
        return false;
    }
    value = {math::Fixed::from_raw(x), math::Fixed::from_raw(y), math::Fixed::from_raw(z)};
    return true;
}

[[nodiscard]] bool read_entity(Reader& reader, EntityState& entity) noexcept {
    return reader.read_u64(entity.entity_id)
        && reader.read_u64(entity.asset_id)
        && read_vec3(reader, entity.position)
        && read_vec3(reader, entity.velocity);
}

[[nodiscard]] std::size_t expected_command_body_size(const std::uint8_t opcode) noexcept {
    switch (opcode) {
    case 1:
        return 88;
    case 2:
        return 32;
    case 3:
    case 4:
        return 56;
    default:
        return 0;
    }
}


} // namespace

namespace detail {

void append_u16(std::vector<std::uint8_t>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8U * index)));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, const std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8U * index)));
    }
}

void append_i64(std::vector<std::uint8_t>& bytes, const std::int64_t value) {
    std::uint64_t encoded{};
    if (value >= 0) {
        encoded = static_cast<std::uint64_t>(value);
    } else {
        const auto magnitude = value == std::numeric_limits<std::int64_t>::min()
            ? (std::uint64_t{1} << 63U)
            : static_cast<std::uint64_t>(-value);
        encoded = (~magnitude) + 1U;
    }
    append_u64(bytes, encoded);
}

void append_entity(std::vector<std::uint8_t>& bytes, const EntityState& entity) {
    append_u64(bytes, entity.entity_id);
    append_u64(bytes, entity.asset_id);
    append_i64(bytes, entity.position.x.raw());
    append_i64(bytes, entity.position.y.raw());
    append_i64(bytes, entity.position.z.raw());
    append_i64(bytes, entity.velocity.x.raw());
    append_i64(bytes, entity.velocity.y.raw());
    append_i64(bytes, entity.velocity.z.raw());
}

void append_command(std::vector<std::uint8_t>& bytes, const Command& command) {
    const auto body_size = std::visit([](const auto& payload) -> std::uint32_t {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, CreateCommand>) {
            return 88;
        } else if constexpr (std::is_same_v<Payload, DestroyCommand>) {
            return 32;
        } else {
            return 56;
        }
    }, command.payload);
    append_u32(bytes, body_size);
    append_u64(bytes, command.key.tick);
    append_u32(bytes, command.key.player_id);
    append_u64(bytes, command.key.sequence);
    bytes.push_back(static_cast<std::uint8_t>(command.payload.index() + 1U));
    bytes.push_back(0);
    append_u16(bytes, 0);
    std::visit([&bytes](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, CreateCommand>) {
            append_entity(bytes, payload.entity);
        } else if constexpr (std::is_same_v<Payload, DestroyCommand>) {
            append_u64(bytes, payload.entity_id);
        } else if constexpr (std::is_same_v<Payload, SetPositionCommand>) {
            append_u64(bytes, payload.entity_id);
            append_i64(bytes, payload.position.x.raw());
            append_i64(bytes, payload.position.y.raw());
            append_i64(bytes, payload.position.z.raw());
        } else {
            append_u64(bytes, payload.entity_id);
            append_i64(bytes, payload.velocity.x.raw());
            append_i64(bytes, payload.velocity.y.raw());
            append_i64(bytes, payload.velocity.z.raw());
        }
    }, command.payload);
}

core::Result<void> validate_replay_contract(
    const Replay& replay,
    const std::string_view logical_path) {
    if (replay.tick_numerator == 0 || replay.tick_denominator == 0
        || std::gcd(replay.tick_numerator, replay.tick_denominator) != 1U) {
        return core::Result<void>::failure(diagnostic(
            diagnostic_codes::replay_malformed,
            "tick duration must be a positive reduced rational",
            logical_path));
    }
    if (replay.final_tick_count > replay_max_ticks
        || replay.initial_entities.size() > replay_max_entities
        || replay.commands.size() > replay_max_commands) {
        return core::Result<void>::failure(diagnostic(
            diagnostic_codes::replay_resource_limit,
            "tick/entity/command count exceeds replay-v1 resource limits",
            logical_path));
    }
    EntityId previous_id{};
    for (std::size_t index = 0; index < replay.initial_entities.size(); ++index) {
        const auto id = replay.initial_entities[index].entity_id;
        if (id == invalid_entity_id || (index != 0 && id <= previous_id)) {
            return core::Result<void>::failure(diagnostic(
                diagnostic_codes::replay_order,
                "initial entity IDs must be nonzero and strictly increasing at index "
                    + std::to_string(index),
                logical_path));
        }
        previous_id = id;
    }
    for (std::size_t index = 0; index < replay.commands.size(); ++index) {
        const auto& command = replay.commands[index];
        if (command.key.tick >= replay.final_tick_count) {
            return core::Result<void>::failure(diagnostic(
                diagnostic_codes::replay_malformed,
                "command " + std::to_string(index) + " tick " + std::to_string(command.key.tick)
                    + " is not less than final tick count",
                logical_path));
        }
        if (index != 0 && !(replay.commands[index - 1].key < command.key)) {
            return core::Result<void>::failure(diagnostic(
                diagnostic_codes::replay_order,
                "command ordering is not strictly increasing at index " + std::to_string(index),
                logical_path));
        }
        const EntityId referenced_id = std::visit([](const auto& payload) {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, CreateCommand>) {
                return payload.entity.entity_id;
            } else {
                return payload.entity_id;
            }
        }, command.payload);
        if (referenced_id == invalid_entity_id) {
            return core::Result<void>::failure(diagnostic(
                diagnostic_codes::replay_malformed,
                "command " + std::to_string(index) + " contains invalid entity ID zero",
                logical_path));
        }
    }
    return core::Result<void>::success();
}

} // namespace detail

core::Result<Replay> parse_replay(
    const std::span<const std::uint8_t> bytes,
    const std::string_view logical_path) {
    if (bytes.size() > replay_max_bytes) {
        return core::Result<Replay>::failure(diagnostic(
            diagnostic_codes::replay_resource_limit,
            "replay exceeds the 256 MiB input limit",
            logical_path));
    }
    if (bytes.size() < replay_header_size) {
        return core::Result<Replay>::failure(diagnostic(
            diagnostic_codes::replay_malformed,
            "truncated replay header: expected 96 bytes, found " + std::to_string(bytes.size()),
            logical_path));
    }

    Reader reader(bytes);
    std::array<std::uint8_t, 8> magic{};
    std::uint16_t format{};
    std::uint16_t header_size{};
    std::uint32_t rules{};
    std::uint32_t decoded_math_version{};
    std::uint32_t decoded_fractional_bits{};
    Replay replay;
    std::uint64_t initial_count{};
    std::uint64_t command_count{};
    if (!reader.read_bytes(magic) || !reader.read_u16(format) || !reader.read_u16(header_size)
        || !reader.read_u32(rules) || !reader.read_u32(decoded_math_version)
        || !reader.read_u32(decoded_fractional_bits) || !reader.read_u32(replay.tick_numerator)
        || !reader.read_u32(replay.tick_denominator) || !reader.read_u64(replay.seed)
        || !reader.read_u64(replay.final_tick_count) || !reader.read_u64(initial_count)
        || !reader.read_u64(command_count) || !reader.read_bytes(replay.content_identity)) {
        return core::Result<Replay>::failure(diagnostic(
            diagnostic_codes::replay_malformed, "truncated replay header", logical_path));
    }
    if (magic != replay_magic) {
        return core::Result<Replay>::failure(diagnostic(
            diagnostic_codes::replay_malformed, "invalid replay magic", logical_path));
    }
    if (format != replay_format_version || rules != simulation_rules_version
        || decoded_math_version != math_version || decoded_fractional_bits != fractional_bits) {
        return core::Result<Replay>::failure(diagnostic(
            diagnostic_codes::replay_version,
            "unsupported replay contract versions (format=" + std::to_string(format)
                + ", rules=" + std::to_string(rules)
                + ", math=" + std::to_string(decoded_math_version)
                + ", fractional_bits=" + std::to_string(decoded_fractional_bits) + ")",
            logical_path));
    }
    if (header_size != replay_header_size) {
        return core::Result<Replay>::failure(diagnostic(
            diagnostic_codes::replay_malformed,
            "invalid replay header size " + std::to_string(header_size), logical_path));
    }
    if (initial_count > replay_max_entities || command_count > replay_max_commands
        || replay.final_tick_count > replay_max_ticks) {
        return core::Result<Replay>::failure(diagnostic(
            diagnostic_codes::replay_resource_limit,
            "tick/entity/command count exceeds replay-v1 resource limits",
            logical_path));
    }
    if (initial_count > reader.remaining() / entity_record_size) {
        return core::Result<Replay>::failure(diagnostic(
            diagnostic_codes::replay_malformed,
            "truncated initial entity table for declared count " + std::to_string(initial_count),
            logical_path));
    }

    replay.initial_entities.reserve(static_cast<std::size_t>(initial_count));
    for (std::uint64_t index = 0; index < initial_count; ++index) {
        EntityState entity;
        if (!read_entity(reader, entity)) {
            return core::Result<Replay>::failure(diagnostic(
                diagnostic_codes::replay_malformed,
                "truncated initial entity record at index " + std::to_string(index),
                logical_path));
        }
        replay.initial_entities.push_back(entity);
    }

    constexpr std::size_t minimum_command_record_size = 4 + 32;
    if (command_count > reader.remaining() / minimum_command_record_size) {
        return core::Result<Replay>::failure(diagnostic(
            diagnostic_codes::replay_malformed,
            "declared command count " + std::to_string(command_count)
                + " exceeds the remaining bounded command table",
            logical_path));
    }

    replay.commands.reserve(static_cast<std::size_t>(command_count));
    for (std::uint64_t index = 0; index < command_count; ++index) {
        const auto prefix_offset = reader.offset();
        std::uint32_t body_size{};
        if (!reader.read_u32(body_size)) {
            return core::Result<Replay>::failure(diagnostic(
                diagnostic_codes::replay_malformed,
                "missing command length prefix at index " + std::to_string(index)
                    + " offset " + std::to_string(prefix_offset),
                logical_path));
        }
        if (body_size < command_common_size || body_size > reader.remaining()) {
            return core::Result<Replay>::failure(diagnostic(
                diagnostic_codes::replay_malformed,
                "invalid command body length " + std::to_string(body_size)
                    + " at index " + std::to_string(index)
                    + " offset " + std::to_string(prefix_offset),
                logical_path));
        }

        Command command;
        std::uint8_t opcode{};
        std::uint8_t flags{};
        std::uint16_t reserved{};
        if (!reader.read_u64(command.key.tick) || !reader.read_u32(command.key.player_id)
            || !reader.read_u64(command.key.sequence) || !reader.read_u8(opcode)
            || !reader.read_u8(flags) || !reader.read_u16(reserved)) {
            return core::Result<Replay>::failure(diagnostic(
                diagnostic_codes::replay_malformed,
                "truncated command common body at index " + std::to_string(index),
                logical_path));
        }
        if (flags != 0 || reserved != 0) {
            return core::Result<Replay>::failure(diagnostic(
                diagnostic_codes::replay_version,
                "unsupported command flags/reserved bits at index " + std::to_string(index),
                logical_path));
        }
        const auto expected_size = expected_command_body_size(opcode);
        if (expected_size == 0) {
            return core::Result<Replay>::failure(diagnostic(
                diagnostic_codes::replay_version,
                "unsupported opcode " + std::to_string(opcode) + " at command " + std::to_string(index),
                logical_path));
        }
        if (body_size != expected_size) {
            return core::Result<Replay>::failure(diagnostic(
                diagnostic_codes::replay_malformed,
                "opcode " + std::to_string(opcode) + " requires body length "
                    + std::to_string(expected_size) + ", found " + std::to_string(body_size)
                    + " at command " + std::to_string(index),
                logical_path));
        }

        if (opcode == 1) {
            EntityState entity;
            if (!read_entity(reader, entity)) {
                return core::Result<Replay>::failure(diagnostic(
                    diagnostic_codes::replay_malformed,
                    "truncated create payload at command " + std::to_string(index), logical_path));
            }
            command.payload = CreateCommand{entity};
        } else if (opcode == 2) {
            EntityId id{};
            if (!reader.read_u64(id)) {
                return core::Result<Replay>::failure(diagnostic(
                    diagnostic_codes::replay_malformed,
                    "truncated destroy payload at command " + std::to_string(index), logical_path));
            }
            command.payload = DestroyCommand{id};
        } else {
            EntityId id{};
            math::Vec3 value{};
            if (!reader.read_u64(id) || !read_vec3(reader, value)) {
                return core::Result<Replay>::failure(diagnostic(
                    diagnostic_codes::replay_malformed,
                    "truncated set payload at command " + std::to_string(index), logical_path));
            }
            if (opcode == 3) {
                command.payload = SetPositionCommand{id, value};
            } else {
                command.payload = SetVelocityCommand{id, value};
            }
        }
        replay.commands.push_back(std::move(command));
    }
    if (reader.remaining() != 0) {
        return core::Result<Replay>::failure(diagnostic(
            diagnostic_codes::replay_malformed,
            "trailing bytes after command table at offset " + std::to_string(reader.offset())
                + ": " + std::to_string(reader.remaining()),
            logical_path));
    }
    const auto validated = detail::validate_replay_contract(replay, logical_path);
    if (!validated) {
        return core::Result<Replay>::failure(validated.error());
    }
    return core::Result<Replay>::success(std::move(replay));
}

core::Result<std::vector<std::uint8_t>> write_replay(const Replay& replay) {
    const auto validated = detail::validate_replay_contract(replay, {});
    if (!validated) {
        return core::Result<std::vector<std::uint8_t>>::failure(validated.error());
    }
    std::size_t size = replay_header_size + replay.initial_entities.size() * entity_record_size;
    for (const auto& command : replay.commands) {
        const auto body_size = std::visit([](const auto& payload) -> std::size_t {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, CreateCommand>) {
                return 88;
            } else if constexpr (std::is_same_v<Payload, DestroyCommand>) {
                return 32;
            } else {
                return 56;
            }
        }, command.payload);
        if (size > replay_max_bytes - (4 + body_size)) {
            return core::Result<std::vector<std::uint8_t>>::failure(diagnostic(
                diagnostic_codes::replay_resource_limit,
                "encoded replay exceeds the 256 MiB output limit"));
        }
        size += 4 + body_size;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(size);
    bytes.insert(bytes.end(), replay_magic.begin(), replay_magic.end());
    detail::append_u16(bytes, replay_format_version);
    detail::append_u16(bytes, static_cast<std::uint16_t>(replay_header_size));
    detail::append_u32(bytes, simulation_rules_version);
    detail::append_u32(bytes, math_version);
    detail::append_u32(bytes, fractional_bits);
    detail::append_u32(bytes, replay.tick_numerator);
    detail::append_u32(bytes, replay.tick_denominator);
    detail::append_u64(bytes, replay.seed);
    detail::append_u64(bytes, replay.final_tick_count);
    detail::append_u64(bytes, replay.initial_entities.size());
    detail::append_u64(bytes, replay.commands.size());
    bytes.insert(bytes.end(), replay.content_identity.begin(), replay.content_identity.end());
    for (const auto& entity : replay.initial_entities) {
        detail::append_entity(bytes, entity);
    }
    for (const auto& command : replay.commands) {
        detail::append_command(bytes, command);
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

std::array<std::uint8_t, 32> sha256(const std::span<const std::uint8_t> bytes) noexcept {
    return core::sha256(bytes);
}

std::string sha256_hex(const std::span<const std::uint8_t> bytes) {
    return core::sha256_hex(bytes);
}

} // namespace eawr::sim
