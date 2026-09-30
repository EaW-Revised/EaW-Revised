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

namespace eawr::sim::tactical {
namespace {

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

[[nodiscard]] bool read_quat(Reader& reader, math::Quat& value) noexcept {
    std::array<std::int64_t, 4> raw{};
    for (auto& component : raw) {
        if (!reader.read_i64(component)) {
            return false;
        }
    }
    value = {
        math::Fixed::from_raw(raw[0]), math::Fixed::from_raw(raw[1]),
        math::Fixed::from_raw(raw[2]), math::Fixed::from_raw(raw[3])};
    return true;
}

[[nodiscard]] std::string command_label(const std::size_t index, const PlayerCommand& command) {
    return "command " + std::to_string(index) + " (tick " + std::to_string(command.key.tick)
        + ", player " + std::to_string(command.key.player_id) + ", sequence "
        + std::to_string(command.key.sequence) + ")";
}

[[nodiscard]] const UnitState* setup_unit(const TacticalSetup& setup, const EntityId id) noexcept {
    const auto found = std::lower_bound(setup.units.begin(), setup.units.end(), id,
        [](const UnitState& unit, const EntityId value) { return unit.entity_id < value; });
    return found != setup.units.end() && found->entity_id == id ? &*found : nullptr;
}

// Squadron rules (#271): containers strictly increase, members strictly increase and are
// nonempty, every container and member is a setup unit of the container's owner, and no unit
// is listed twice (as a container or a member). Runs after the unit checks.
[[nodiscard]] core::Result<void> validate_squadrons(const TacticalSetup& setup) {
    constexpr std::string_view path{};
    if (setup.squadrons.size() > max_units) {
        return fail<void>(diagnostic_codes::resource_limit, "squadron count exceeds the unit limit", path);
    }
    std::vector<EntityId> listed;
    for (std::size_t index = 0; index < setup.squadrons.size(); ++index) {
        const auto& squadron = setup.squadrons[index];
        const auto label = "squadron " + std::to_string(squadron.container);
        if (index != 0 && squadron.container <= setup.squadrons[index - 1].container) {
            return fail<void>(diagnostic_codes::order,
                "squadron containers must strictly increase at index " + std::to_string(index), path);
        }
        const auto* container = setup_unit(setup, squadron.container);
        if (container == nullptr) {
            return fail<void>(diagnostic_codes::invalid_setup, label + ": the container is not a setup unit", path);
        }
        if (squadron.members.empty() || squadron.members.size() > max_units) {
            return fail<void>(diagnostic_codes::invalid_setup, label + ": a squadron lists 1 to 1,000,000 craft", path);
        }
        listed.push_back(squadron.container);
        for (std::size_t member = 0; member < squadron.members.size(); ++member) {
            const auto id = squadron.members[member];
            if (member != 0 && id <= squadron.members[member - 1]) {
                return fail<void>(diagnostic_codes::order, label + ": craft IDs must strictly increase", path);
            }
            const auto* craft = setup_unit(setup, id);
            if (craft == nullptr || craft->owner != container->owner) {
                return fail<void>(diagnostic_codes::invalid_setup,
                    label + ": craft " + std::to_string(id) + " is not a setup unit of the container's owner", path);
            }
            listed.push_back(id);
        }
    }
    std::sort(listed.begin(), listed.end());
    if (std::adjacent_find(listed.begin(), listed.end()) != listed.end()) {
        return fail<void>(diagnostic_codes::invalid_setup, "a unit belongs to more than one squadron", path);
    }
    return core::Result<void>::success();
}

} // namespace

std::optional<std::uint16_t> peek_replay_format_version(
    const std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() < replay_magic.size() + 2
        || !std::equal(replay_magic.begin(), replay_magic.end(), bytes.begin())) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(
        bytes[8] | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[9]) << 8U));
}

core::Result<void> validate_setup(const TacticalSetup& setup) {
    constexpr std::string_view path{};
    if (setup.players.size() > max_players || setup.units.size() > max_units) {
        return fail<void>(diagnostic_codes::resource_limit,
            "player/unit count exceeds tactical replay resource limits", path);
    }
    for (std::size_t index = 0; index < setup.players.size(); ++index) {
        const auto& player = setup.players[index];
        if (player.player_id == 0
            || (index != 0 && player.player_id <= setup.players[index - 1].player_id)) {
            return fail<void>(diagnostic_codes::order,
                "player IDs must be nonzero and strictly increasing at index " + std::to_string(index),
                path);
        }
        if ((player.flags & ~player_flag_commandable) != 0U) {
            return fail<void>(diagnostic_codes::version,
                "unsupported player flags at index " + std::to_string(index), path);
        }
    }
    for (std::size_t index = 0; index < setup.units.size(); ++index) {
        const auto& unit = setup.units[index];
        if (unit.entity_id == invalid_entity_id
            || (index != 0 && unit.entity_id <= setup.units[index - 1].entity_id)) {
            return fail<void>(diagnostic_codes::order,
                "unit IDs must be nonzero and strictly increasing at index " + std::to_string(index),
                path);
        }
        const auto owner = std::find_if(setup.players.begin(), setup.players.end(),
            [&unit](const Player& player) { return player.player_id == unit.owner; });
        if (owner == setup.players.end()) {
            return fail<void>(diagnostic_codes::invalid_setup,
                "unit " + std::to_string(unit.entity_id) + " owner " + std::to_string(unit.owner)
                    + " is not a declared player",
                path);
        }
        if (!math::to_matrix(unit.rotation, unit.position)) {
            return fail<void>(diagnostic_codes::invalid_setup,
                "unit " + std::to_string(unit.entity_id) + " rotation is not a unit quaternion",
                path);
        }
        if (unit.order != Order{}) {
            return fail<void>(diagnostic_codes::invalid_setup,
                "setup unit " + std::to_string(unit.entity_id) + " must not carry an order", path);
        }
    }
    return validate_squadrons(setup);
}

core::Result<void> validate_replay(const TacticalReplay& replay) {
    const auto setup = validate_setup(replay.setup);
    if (!setup) {
        return setup;
    }
    constexpr std::string_view path{};
    if (replay.final_tick_count > max_ticks || replay.commands.size() > max_commands) {
        return fail<void>(diagnostic_codes::resource_limit,
            "tick/command count exceeds tactical replay resource limits", path);
    }
    for (std::size_t index = 0; index < replay.commands.size(); ++index) {
        const auto& command = replay.commands[index];
        const auto label = command_label(index, command);
        if (command.key.tick >= replay.final_tick_count) {
            return fail<void>(diagnostic_codes::malformed,
                label + " tick is not less than final tick count", path);
        }
        if (index != 0 && !(replay.commands[index - 1].key < command.key)) {
            return fail<void>(diagnostic_codes::order,
                label + " does not strictly follow the previous (tick, player, sequence)", path);
        }
        const auto shape = detail::validate_command_shape(command, replay.setup.players, label, path);
        if (!shape) {
            return shape;
        }
    }
    return core::Result<void>::success();
}

core::Result<TacticalReplay> parse_replay(
    const std::span<const std::uint8_t> bytes,
    const std::string_view logical_path) {
    using Parsed = TacticalReplay;
    if (bytes.size() > replay_max_bytes) {
        return fail<Parsed>(diagnostic_codes::resource_limit,
            "replay exceeds the 256 MiB input limit", logical_path);
    }
    if (bytes.size() < replay_header_size) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "truncated replay-v2 header: expected 104 bytes, found " + std::to_string(bytes.size()),
            logical_path);
    }

    Reader reader(bytes);
    std::array<std::uint8_t, 8> magic{};
    std::uint16_t format{};
    std::uint16_t header_size{};
    std::uint32_t rules{};
    std::uint32_t decoded_math{};
    std::uint32_t decoded_bits{};
    std::uint32_t numerator{};
    std::uint32_t denominator{};
    std::uint32_t player_count{};
    std::uint32_t header_reserved{};
    std::uint64_t unit_count{};
    std::uint64_t command_count{};
    TacticalReplay replay;
    if (!reader.read_bytes(magic) || !reader.read_u16(format) || !reader.read_u16(header_size)
        || !reader.read_u32(rules) || !reader.read_u32(decoded_math) || !reader.read_u32(decoded_bits)
        || !reader.read_u32(numerator) || !reader.read_u32(denominator)
        || !reader.read_u64(replay.setup.seed) || !reader.read_u64(replay.final_tick_count)
        || !reader.read_u32(player_count) || !reader.read_u32(header_reserved)
        || !reader.read_u64(unit_count) || !reader.read_u64(command_count)
        || !reader.read_bytes(replay.setup.content_identity)) {
        return fail<Parsed>(diagnostic_codes::malformed, "truncated replay-v2 header", logical_path);
    }
    if (magic != replay_magic) {
        return fail<Parsed>(diagnostic_codes::malformed, "invalid replay magic", logical_path);
    }
    const bool squadrons = format == replay_format_version_squadrons;
    if ((format != replay_format_version && !squadrons) || rules != tactical_rules_version
        || decoded_math != math_version || decoded_bits != fractional_bits) {
        return fail<Parsed>(diagnostic_codes::version,
            "unsupported replay-v2 contract versions (format=" + std::to_string(format)
                + ", rules=" + std::to_string(rules) + ", math=" + std::to_string(decoded_math)
                + ", fractional_bits=" + std::to_string(decoded_bits) + ")",
            logical_path);
    }
    if (header_size != replay_header_size) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "invalid replay-v2 header size " + std::to_string(header_size), logical_path);
    }
    if (numerator != tick_numerator || denominator != tick_denominator) {
        return fail<Parsed>(diagnostic_codes::version,
            "tactical rules v1 require a 1/30 s tick, found " + std::to_string(numerator) + "/"
                + std::to_string(denominator),
            logical_path);
    }
    // Version 3 stores its squadron count in the version-2 reserved field; a version-3 file
    // without squadrons would be a second encoding of a version-2 replay.
    if (!squadrons && header_reserved != 0) {
        return fail<Parsed>(diagnostic_codes::version,
            "nonzero reserved replay-v2 header field", logical_path);
    }
    if (squadrons && (header_reserved == 0 || header_reserved > max_units)) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "replay-v3 squadron count must be 1 to 1,000,000, found " + std::to_string(header_reserved), logical_path);
    }
    if (player_count > max_players || unit_count > max_units || command_count > max_commands
        || replay.final_tick_count > max_ticks) {
        return fail<Parsed>(diagnostic_codes::resource_limit,
            "tick/player/unit/command count exceeds tactical replay resource limits", logical_path);
    }
    if (player_count > reader.remaining() / detail::player_record_size) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "truncated player table for declared count " + std::to_string(player_count), logical_path);
    }
    replay.setup.players.reserve(player_count);
    for (std::uint32_t index = 0; index < player_count; ++index) {
        Player player;
        std::uint32_t reserved{};
        if (!reader.read_u32(player.player_id) || !reader.read_u32(player.team_id)
            || !reader.read_u64(player.faction_id) || !reader.read_u32(player.flags)
            || !reader.read_u32(reserved)) {
            return fail<Parsed>(diagnostic_codes::malformed,
                "truncated player record at index " + std::to_string(index), logical_path);
        }
        if (reserved != 0) {
            return fail<Parsed>(diagnostic_codes::version,
                "nonzero reserved player field at index " + std::to_string(index), logical_path);
        }
        replay.setup.players.push_back(player);
    }
    if (unit_count > reader.remaining() / detail::unit_record_size) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "truncated unit table for declared count " + std::to_string(unit_count), logical_path);
    }
    replay.setup.units.reserve(static_cast<std::size_t>(unit_count));
    for (std::uint64_t index = 0; index < unit_count; ++index) {
        UnitState unit;
        std::uint32_t reserved{};
        if (!reader.read_u64(unit.entity_id) || !reader.read_u64(unit.type_id)
            || !reader.read_u32(unit.owner) || !reader.read_u32(reserved)
            || !read_vec3(reader, unit.position) || !read_quat(reader, unit.rotation)) {
            return fail<Parsed>(diagnostic_codes::malformed,
                "truncated unit record at index " + std::to_string(index), logical_path);
        }
        if (reserved != 0) {
            return fail<Parsed>(diagnostic_codes::version,
                "nonzero reserved unit field at index " + std::to_string(index), logical_path);
        }
        replay.setup.units.push_back(unit);
    }
    if (squadrons) {
        // Each record is at least 24 bytes: container, member count, reserved and one member.
        if (header_reserved > reader.remaining() / 24U) {
            return fail<Parsed>(diagnostic_codes::malformed,
                "truncated squadron table for declared count " + std::to_string(header_reserved), logical_path);
        }
        replay.setup.squadrons.reserve(header_reserved);
        for (std::uint32_t index = 0; index < header_reserved; ++index) {
            Squadron squadron;
            std::uint32_t members{};
            std::uint32_t reserved{};
            if (!reader.read_u64(squadron.container) || !reader.read_u32(members) || !reader.read_u32(reserved)
                || members > reader.remaining() / 8U) {
                return fail<Parsed>(diagnostic_codes::malformed,
                    "truncated squadron record at index " + std::to_string(index), logical_path);
            }
            if (reserved != 0) {
                return fail<Parsed>(diagnostic_codes::version,
                    "nonzero reserved squadron field at index " + std::to_string(index), logical_path);
            }
            squadron.members.resize(members);
            for (auto& member : squadron.members) {
                static_cast<void>(reader.read_u64(member));
            }
            replay.setup.squadrons.push_back(std::move(squadron));
        }
    }

    constexpr std::size_t minimum_command_record_size =
        4 + detail::command_common_size + detail::unit_list_header_size + 8;
    if (command_count > reader.remaining() / minimum_command_record_size) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "declared command count " + std::to_string(command_count)
                + " exceeds the remaining bounded command table",
            logical_path);
    }
    replay.commands.reserve(static_cast<std::size_t>(command_count));
    for (std::uint64_t index = 0; index < command_count; ++index) {
        const auto prefix_offset = reader.offset();
        const auto at = " at command " + std::to_string(index) + " offset " + std::to_string(prefix_offset);
        std::uint32_t body_size{};
        if (!reader.read_u32(body_size)) {
            return fail<Parsed>(diagnostic_codes::malformed, "missing command length prefix" + at,
                logical_path);
        }
        if (body_size < detail::command_common_size + detail::unit_list_header_size
            || body_size > reader.remaining()) {
            return fail<Parsed>(diagnostic_codes::malformed,
                "invalid command body length " + std::to_string(body_size) + at, logical_path);
        }
        PlayerCommand command;
        std::uint8_t opcode{};
        std::uint8_t flags{};
        std::uint16_t reserved{};
        if (!reader.read_u64(command.key.tick) || !reader.read_u32(command.key.player_id)
            || !reader.read_u64(command.key.sequence) || !reader.read_u8(opcode)
            || !reader.read_u8(flags) || !reader.read_u16(reserved)) {
            return fail<Parsed>(diagnostic_codes::malformed, "truncated command common body" + at,
                logical_path);
        }
        if (flags != 0 || reserved != 0) {
            return fail<Parsed>(diagnostic_codes::version, "unsupported command flags/reserved bits" + at,
                logical_path);
        }
        const auto prefix = detail::payload_prefix_size(opcode);
        if (opcode == 0 || opcode > 8) {
            return fail<Parsed>(diagnostic_codes::version,
                "unsupported opcode " + std::to_string(opcode) + at, logical_path);
        }
        if (body_size < detail::command_common_size + prefix + detail::unit_list_header_size) {
            return fail<Parsed>(diagnostic_codes::malformed,
                "opcode " + std::to_string(opcode) + " body length " + std::to_string(body_size)
                    + " is shorter than its fixed payload" + at,
                logical_path);
        }
        if (opcode == 1) {
            command.payload = StopPayload{};
        } else if (opcode == 2) {
            MovePayload move;
            if (!read_vec3(reader, move.destination)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated move payload" + at, logical_path);
            }
            command.payload = move;
        } else if (opcode == 3) {
            AttackPayload attack;
            if (!reader.read_u64(attack.target)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated attack payload" + at,
                    logical_path);
            }
            command.payload = attack;
        } else if (opcode == 5) {
            FacePayload face;
            if (!read_vec3(reader, face.target)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated face payload" + at, logical_path);
            }
            command.payload = face;
        } else if (opcode == 6 || opcode == 7) {
            math::Vec3 destination;
            EntityId target{};
            if (!read_vec3(reader, destination) || !reader.read_u64(target)) {
                return fail<Parsed>(diagnostic_codes::malformed,
                    std::string(opcode == 6 ? "truncated attack-move payload" : "truncated guard payload") + at,
                    logical_path);
            }
            if (opcode == 6) {
                command.payload = AttackMovePayload{destination, target};
            } else {
                command.payload = GuardPayload{destination, target};
            }
        } else if (opcode == 8) {
            std::uint8_t ability{};
            std::uint8_t action{};
            std::uint16_t reserved16{};
            std::uint32_t reserved32{};
            if (!reader.read_u8(ability) || !reader.read_u8(action) || !reader.read_u16(reserved16)
                || !reader.read_u32(reserved32)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated ability payload" + at, logical_path);
            }
            // #561: a first field of 1 marks a targeted ability, which carries its target hardpoint
            // and then its uint64 target.
            AbilityPayload payload{static_cast<AbilityKind>(ability), static_cast<AbilityAction>(action)};
            if (reserved16 == 1) {
                payload.target_hardpoint = reserved32;
                if (!reader.read_u64(payload.target) || payload.target == invalid_entity_id) {
                    return fail<Parsed>(diagnostic_codes::malformed, "truncated ability target" + at, logical_path);
                }
            } else if (reserved16 != 0 || reserved32 != 0) {
                return fail<Parsed>(diagnostic_codes::version, "nonzero reserved ability field" + at, logical_path);
            }
            command.payload = payload;
        } else {
            DamagePayload damage;
            std::int64_t amount{};
            std::uint32_t damage_reserved{};
            if (!reader.read_i64(amount) || !reader.read_u32(damage.hardpoint)
                || !reader.read_u32(damage_reserved)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated damage payload" + at,
                    logical_path);
            }
            if (damage_reserved != 0) {
                return fail<Parsed>(diagnostic_codes::version, "nonzero reserved damage field" + at,
                    logical_path);
            }
            damage.amount = math::Fixed::from_raw(amount);
            command.payload = damage;
        }
        std::uint32_t listed{};
        std::uint32_t list_reserved{};
        if (!reader.read_u32(listed) || !reader.read_u32(list_reserved)) {
            return fail<Parsed>(diagnostic_codes::malformed, "truncated unit list header" + at, logical_path);
        }
        if (list_reserved != 0) {
            return fail<Parsed>(diagnostic_codes::version, "nonzero reserved unit list field" + at,
                logical_path);
        }
        if (listed > max_units_per_command) {
            return fail<Parsed>(diagnostic_codes::resource_limit,
                "unit list of " + std::to_string(listed) + " exceeds the per-command limit" + at,
                logical_path);
        }
        const auto* aimed = std::get_if<AbilityPayload>(&command.payload);
        const std::size_t target_bytes = aimed != nullptr && aimed->target != invalid_entity_id ? 8U : 0U;
        const auto expected = detail::command_common_size + prefix + target_bytes + detail::unit_list_header_size
            + 8U * static_cast<std::size_t>(listed);
        if (body_size != expected) {
            return fail<Parsed>(diagnostic_codes::malformed,
                "opcode " + std::to_string(opcode) + " with " + std::to_string(listed)
                    + " units requires body length " + std::to_string(expected) + ", found "
                    + std::to_string(body_size) + at,
                logical_path);
        }
        command.units.resize(listed);
        for (auto& unit : command.units) {
            if (!reader.read_u64(unit)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated unit list" + at, logical_path);
            }
        }
        replay.commands.push_back(std::move(command));
    }
    if (reader.remaining() != 0) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "trailing bytes after command table at offset " + std::to_string(reader.offset()) + ": "
                + std::to_string(reader.remaining()),
            logical_path);
    }
    auto validated = validate_replay(replay);
    if (!validated) {
        auto error = validated.error();
        if (!logical_path.empty()) {
            error.logical_path = std::string(logical_path);
        }
        return core::Result<Parsed>::failure(std::move(error));
    }
    return core::Result<Parsed>::success(std::move(replay));
}

core::Result<std::vector<std::uint8_t>> write_replay(const TacticalReplay& replay) {
    using Bytes = std::vector<std::uint8_t>;
    const auto validated = validate_replay(replay);
    if (!validated) {
        return core::Result<Bytes>::failure(validated.error());
    }
    std::size_t size = replay_header_size + replay.setup.players.size() * detail::player_record_size
        + replay.setup.units.size() * detail::unit_record_size;
    const auto& squadrons = replay.setup.squadrons;
    for (const auto& squadron : squadrons) {
        size += 16U + 8U * squadron.members.size();
    }
    if (size > replay_max_bytes) {
        return fail<Bytes>(diagnostic_codes::resource_limit, "encoded replay exceeds the 256 MiB output limit", {});
    }
    for (const auto& command : replay.commands) {
        const auto record = 4 + detail::command_body_size(command);
        if (size > replay_max_bytes - record) {
            return fail<Bytes>(diagnostic_codes::resource_limit,
                "encoded replay exceeds the 256 MiB output limit", {});
        }
        size += record;
    }
    Bytes bytes;
    bytes.reserve(size);
    bytes.insert(bytes.end(), replay_magic.begin(), replay_magic.end());
    sim::detail::append_u16(bytes, squadrons.empty() ? replay_format_version : replay_format_version_squadrons);
    sim::detail::append_u16(bytes, static_cast<std::uint16_t>(replay_header_size));
    sim::detail::append_u32(bytes, tactical_rules_version);
    sim::detail::append_u32(bytes, math_version);
    sim::detail::append_u32(bytes, fractional_bits);
    sim::detail::append_u32(bytes, tick_numerator);
    sim::detail::append_u32(bytes, tick_denominator);
    sim::detail::append_u64(bytes, replay.setup.seed);
    sim::detail::append_u64(bytes, replay.final_tick_count);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(replay.setup.players.size()));
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(squadrons.size()));
    sim::detail::append_u64(bytes, replay.setup.units.size());
    sim::detail::append_u64(bytes, replay.commands.size());
    bytes.insert(bytes.end(), replay.setup.content_identity.begin(), replay.setup.content_identity.end());
    for (const auto& player : replay.setup.players) {
        detail::append_player(bytes, player);
    }
    for (const auto& unit : replay.setup.units) {
        detail::append_unit_record(bytes, unit);
    }
    for (const auto& squadron : squadrons) {
        sim::detail::append_u64(bytes, squadron.container);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(squadron.members.size()));
        sim::detail::append_u32(bytes, 0);
        for (const auto member : squadron.members) {
            sim::detail::append_u64(bytes, member);
        }
    }
    for (const auto& command : replay.commands) {
        detail::append_command(bytes, command);
    }
    return core::Result<Bytes>::success(std::move(bytes));
}

} // namespace eawr::sim::tactical
