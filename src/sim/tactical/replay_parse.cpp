#include "replay_internal.hpp"

namespace eawr::sim::tactical {
using namespace replay_detail;

namespace {
[[nodiscard]] bool read_skirmish(Reader& reader, ReplaySkirmishSetup& setup) {
    const auto text = [&](std::string& value) {
        std::uint16_t length{};
        if (!reader.read_u16(length) || length > 1024U || length > reader.remaining()) return false;
        value.resize(length);
        return reader.read_bytes(std::span(reinterpret_cast<std::uint8_t*>(value.data()), value.size()));
    };
    std::uint32_t flags{}, slots{};
    std::int64_t credits{}, win_float{};
    auto& match = setup.match;
    if (!text(setup.map) || !text(setup.map_sha256) || !reader.read_u32(flags) || (flags & ~31U) != 0
        || !reader.read_i64(credits)) return false;
    for (auto* value : {&match.start_tech, &match.max_tech, &match.game_timer, &match.win_integer, &match.auto_resolve}) {
        std::int64_t decoded{};
        if (!reader.read_i64(decoded) || decoded < std::numeric_limits<std::int32_t>::min()
            || decoded > std::numeric_limits<std::int32_t>::max()) return false;
        *value = static_cast<std::int32_t>(decoded);
    }
    if (!reader.read_i64(win_float) || !text(match.win_condition) || !text(match.space_win_condition)
        || !reader.read_u32(setup.victory_condition) || !reader.read_u32(slots) || slots > max_players) return false;
    match.allow_heroes = (flags & 1U) == 0;
    match.allow_superweapons = (flags & 2U) == 0;
    match.free_starting_units = (flags & 4U) == 0;
    match.pre_built_base = (flags & 8U) == 0;
    match.allow_random_events = (flags & 16U) != 0;
    match.credits = math::Fixed::from_raw(credits);
    match.win_float = math::Fixed::from_raw(win_float);
    for (std::uint32_t index = 0; index < slots; ++index) {
        ReplayLobbySlot slot;
        std::uint32_t human{}, colour{}, fleet{};
        if (!reader.read_u32(slot.player) || !reader.read_u32(human) || human > 1U
            || !reader.read_u32(colour) || !reader.read_u32(fleet) || fleet > 1024U) return false;
        slot.human = human != 0;
        if (colour != std::numeric_limits<std::uint32_t>::max()) slot.colour_index = colour;
        for (std::uint32_t type = 0; type < fleet; ++type) {
            std::string name;
            if (!text(name)) return false;
            slot.fleet.push_back(std::move(name));
        }
        setup.slots.push_back(std::move(slot));
    }
    return reader.remaining() == 0;
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


} // namespace

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
    const bool extensions = format == replay_format_version_extensions
        || format == replay_format_version_squadron_extensions;
    const bool squadrons = format == replay_format_version_squadrons
        || format == replay_format_version_squadron_extensions;
    if ((format != replay_format_version && !squadrons && !extensions) || rules != tactical_rules_version
        || decoded_math != math_version || decoded_bits != fractional_bits) {
        return fail<Parsed>(diagnostic_codes::version,
            "unsupported replay-v2 contract versions (format=" + std::to_string(format)
                + ", rules=" + std::to_string(rules) + ", math=" + std::to_string(decoded_math)
                + ", fractional_bits=" + std::to_string(decoded_bits) + ")",
            logical_path);
    }
    if ((!extensions && header_size != replay_header_size)
        || (extensions && (header_size < replay_header_size + 4U || header_size > bytes.size()))) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "invalid replay-v2 header size " + std::to_string(header_size), logical_path);
    }
    if (numerator != tick_numerator || denominator != tick_denominator) {
        return fail<Parsed>(diagnostic_codes::version,
            "tactical rules v1 require a 1/30 s tick, found " + std::to_string(numerator) + "/"
                + std::to_string(denominator),
            logical_path);
    }
    if (extensions) {
        std::uint32_t count{};
        if (!reader.read_u32(count) || count == 0 || count > (header_size - reader.offset()) / 4U) {
            return fail<Parsed>(diagnostic_codes::malformed, "invalid replay header extension count", logical_path);
        }
        std::uint16_t previous_tag{};
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint16_t tag{}, length{};
            if (reader.offset() > header_size || header_size - reader.offset() < 4U
                || !reader.read_u16(tag) || !reader.read_u16(length)
                || length > header_size - reader.offset()) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated replay header extension", logical_path);
            }
            if (tag <= previous_tag) {
                return fail<Parsed>(diagnostic_codes::malformed, "replay header extension tags must increase", logical_path);
            }
            previous_tag = tag;
            if (tag == replay_extension_skirmish_setup) {
                if (replay.setup.skirmish || length > 65000U) {
                    return fail<Parsed>(diagnostic_codes::malformed, "duplicate or oversized SKSU extension", logical_path);
                }
                Reader body(bytes.subspan(reader.offset(), length));
                ReplaySkirmishSetup metadata;
                if (!read_skirmish(body, metadata)) {
                    return fail<Parsed>(diagnostic_codes::malformed, "invalid SKSU extension body", logical_path);
                }
                std::vector<std::uint8_t> consumed(length);
                static_cast<void>(reader.read_bytes(consumed));
                replay.setup.skirmish = std::move(metadata);
                continue;
            }
            if (tag != replay_extension_match_policy) {
                return fail<Parsed>(diagnostic_codes::version,
                    "unknown replay header extension tag " + std::to_string(tag), logical_path);
            }
            if (replay.setup.match_policy || length != 4) {
                return fail<Parsed>(diagnostic_codes::malformed,
                    "duplicate or invalid match-policy header extension", logical_path);
            }
            std::uint32_t flags{};
            if (!reader.read_u32(flags) || flags == 0 || (flags & ~15U) != 0) {
                return fail<Parsed>(diagnostic_codes::malformed,
                    "match-policy extension requires nonzero supported disabled flags", logical_path);
            }
            replay.setup.match_policy = SkirmishMatchPolicy{
                (flags & 1U) == 0, (flags & 2U) == 0, (flags & 4U) == 0, (flags & 8U) == 0};
        }
        if (reader.offset() != header_size || (!replay.setup.match_policy && !replay.setup.skirmish)) {
            return fail<Parsed>(diagnostic_codes::malformed, "invalid replay header extension length", logical_path);
        }
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

    // WBF-48: intentional quit has neither a payload prefix nor a unit list.
    constexpr std::size_t minimum_command_record_size =
        4 + detail::command_common_size + detail::unit_list_header_size;
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
        if (opcode == 0 || opcode > detail::max_command_opcode) {
            return fail<Parsed>(diagnostic_codes::version,
                "unsupported opcode " + std::to_string(opcode) + at, logical_path);
        }
        if (body_size < detail::command_common_size + prefix + detail::unit_list_header_size) {
            return fail<Parsed>(diagnostic_codes::malformed,
                "opcode " + std::to_string(opcode) + " body length " + std::to_string(body_size)
                    + " is shorter than its fixed payload" + at,
                logical_path);
        }
        if (opcode == detail::opcode_reveal_all) {
            RevealAllPayload reveal;
            std::uint32_t reveal_reserved{};
            if (!reader.read_u32(reveal.player) || !reader.read_u32(reveal_reserved) || reveal_reserved != 0) {
                return fail<Parsed>(diagnostic_codes::invalid_command, "invalid reveal-all payload" + at, logical_path);
            }
            command.payload = reveal;
        } else if (opcode == detail::opcode_pad_sell) {
            command.payload = PadSellPayload{};
        } else if (opcode == 1) {
            command.payload = StopPayload{};
        } else if (opcode == detail::opcode_intentional_quit) {
            command.payload = QuitPayload{};
        } else if (opcode == 2 || opcode == detail::opcode_hazard_move) {
            MovePayload move;
            move.through_hazards = opcode == detail::opcode_hazard_move;
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
        } else if (opcode == detail::opcode_attack_hardpoint) {
            AttackPayload attack;
            std::uint32_t hardpoint_reserved{};
            if (!reader.read_u64(attack.target) || !reader.read_u32(attack.hardpoint)
                || !reader.read_u32(hardpoint_reserved)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated hardpoint attack payload" + at,
                    logical_path);
            }
            if (hardpoint_reserved != 0) {
                return fail<Parsed>(diagnostic_codes::version, "nonzero reserved hardpoint attack field" + at,
                    logical_path);
            }
            if (attack.target == 0) {
                return fail<Parsed>(diagnostic_codes::invalid_command, "a hardpoint attack names no target" + at,
                    logical_path);
            }
            if (attack.hardpoint == attack_hull) {
                return fail<Parsed>(diagnostic_codes::invalid_command,
                    "a hardpoint attack names the hull (write it as opcode 3)" + at, logical_path);
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
            } else if (reserved16 == 2 && reserved32 == 0) {
                std::int64_t x{}, y{}, z{};
                if (!reader.read_i64(x) || !reader.read_i64(y) || !reader.read_i64(z))
                    return fail<Parsed>(diagnostic_codes::malformed, "truncated ability position" + at, logical_path);
                payload.position = math::Vec3{math::Fixed::from_raw(x), math::Fixed::from_raw(y), math::Fixed::from_raw(z)};
            } else if (reserved16 != 0 || reserved32 != 0) {
                return fail<Parsed>(diagnostic_codes::version, "nonzero reserved ability field" + at, logical_path);
            }
            command.payload = payload;
        } else if (opcode == detail::opcode_manual_target) {
            ManualTargetPayload manual;
            std::uint32_t reserved_manual{};
            if (!reader.read_u64(manual.target) || !reader.read_u32(manual.hardpoint)
                || !reader.read_u32(reserved_manual)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated manual target payload" + at, logical_path);
            }
            if (reserved_manual != 0) {
                return fail<Parsed>(diagnostic_codes::version, "nonzero reserved manual target field" + at, logical_path);
            }
            command.payload = manual;
        } else if (opcode == detail::opcode_area_ability) {
            std::uint32_t kind{}, reserved_area{};
            AreaAbilityPayload area;
            if (!reader.read_u32(kind) || !reader.read_u32(reserved_area) || !read_vec3(reader, area.point)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated area ability payload" + at, logical_path);
            }
            if (reserved_area != 0 || kind != static_cast<std::uint32_t>(AbilityKind::barrage)) {
                return fail<Parsed>(diagnostic_codes::version, "unsupported area ability field" + at, logical_path);
            }
            area.ability = AbilityKind::barrage;
            command.payload = area;
        } else if (opcode == detail::opcode_pad_build) {
            PadBuildPayload pad;
            if (!reader.read_u64(pad.type)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated pad build payload" + at, logical_path);
            }
            command.payload = pad;
        } else if (opcode == 9) {
            BuyPayload buy;
            if (!reader.read_u64(buy.type)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated buy payload" + at, logical_path);
            }
            command.payload = buy;
        } else if (opcode == detail::opcode_credit_grant) {
            std::int64_t amount{};
            if (!reader.read_i64(amount)) return fail<Parsed>(diagnostic_codes::malformed, "truncated credit grant" + at, logical_path);
            command.payload = CreditGrantPayload{math::Fixed::from_raw(amount)};
        } else if (opcode == 10) {
            CancelPayload cancel;
            if (!reader.read_u32(cancel.queue) || !reader.read_u32(cancel.index)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated cancel payload" + at, logical_path);
            }
            command.payload = cancel;
        } else if (opcode == 11 || opcode == detail::opcode_reserved_reinforce) {
            ReinforcePayload reinforce;
            if (!reader.read_u64(reinforce.type) || !read_vec3(reader, reinforce.position)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated reinforce payload" + at, logical_path);
            }
            if (opcode == detail::opcode_reserved_reinforce
                && (!reader.read_u64(reinforce.pool_token) || reinforce.pool_token == 0)) {
                return fail<Parsed>(diagnostic_codes::malformed, "invalid reinforcement purchase token" + at, logical_path);
            }
            command.payload = reinforce;
        } else if (opcode == 4) {
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
        } else {
            return fail<Parsed>(diagnostic_codes::version, "unsupported opcode " + std::to_string(opcode) + at,
                logical_path);
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
        const std::size_t target_bytes = aimed != nullptr ? (aimed->position ? 24U : aimed->target != invalid_entity_id ? 8U : 0U) : 0U;
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


} // namespace eawr::sim::tactical
