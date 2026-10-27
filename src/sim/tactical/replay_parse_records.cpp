#include "replay_parse_internal.hpp"

namespace eawr::sim::tactical::replay_parse_detail {
using namespace replay_detail;

core::Result<void> parse_commands(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint64_t command_count) {
    using Parsed = void;
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
        std::size_t roster_bytes{};
        if (opcode == detail::opcode_battle_control) {
            BattleControlPayload control;
            std::uint32_t version{}, count{};
            if (!reader.read_u32(control.departed.value) || !reader.read_u32(version)
                || version != 4 || !reader.read_u32(count) || count == 0 || count > max_controller_seats
                || 16ULL * count + 16ULL > body_size - detail::command_common_size)
                return fail<Parsed>(diagnostic_codes::malformed, "invalid battle-control roster" + at, logical_path);
            for (std::uint32_t seat = 0; seat < count; ++seat) {
                ControllerBinding row;
                std::uint32_t kind{}, active{};
                if (!reader.read_u32(row.seat.value) || !reader.read_u32(row.player)
                    || !reader.read_u32(kind) || kind > 255 || !reader.read_u32(active) || active > 1)
                    return fail<Parsed>(diagnostic_codes::malformed, "truncated battle-control seat" + at, logical_path);
                row.kind = static_cast<SeatKind>(kind); row.active = active != 0;
                control.policy.seats.push_back(row);
            }
            roster_bytes = 16U * count;
            if (!reader.read_u32(count) || count != 1)
                return fail<Parsed>(diagnostic_codes::malformed, "invalid battle-control faction count" + at, logical_path);
            ControllerFaction faction;
            std::uint32_t mode{};
            if (!reader.read_u32(faction.player) || !reader.read_u32(mode))
                return fail<Parsed>(diagnostic_codes::malformed, "truncated battle-control faction" + at, logical_path);
            faction.mode = static_cast<FactionController>(mode);
            control.policy.factions.push_back(faction);
            roster_bytes += 8U;
            command.payload = std::move(control);
        } else if (opcode == detail::opcode_gift) {
            GiftPayload gift;
            std::uint32_t gift_reserved{};
            if (!reader.read_u32(gift.recipient.value) || !reader.read_u32(gift_reserved) || gift_reserved != 0)
                return fail<Parsed>(diagnostic_codes::invalid_command, "invalid gift payload" + at, logical_path);
            command.payload = gift;
        } else if (opcode == detail::opcode_beacon) {
            BeaconPayload beacon;
            if (!read_vec3(reader, beacon.position))
                return fail<Parsed>(diagnostic_codes::malformed, "truncated beacon position" + at, logical_path);
            command.payload = beacon;
        } else if (opcode == detail::opcode_reveal_all) {
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
        } else if (opcode == detail::opcode_repair_hardpoint) {
            RepairHardpointPayload repair;
            std::uint32_t repair_reserved{};
            if (!reader.read_u32(repair.hardpoint) || !reader.read_u32(repair_reserved)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated repair payload" + at, logical_path);
            }
            if (repair_reserved != 0) {
                return fail<Parsed>(diagnostic_codes::version, "nonzero reserved repair field" + at, logical_path);
            }
            command.payload = repair;
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
        } else if (opcode == detail::opcode_retreat_cinematic) {
            std::uint32_t playing{}, frame{};
            if (!reader.read_u32(playing) || !reader.read_u32(frame) || playing > 1)
                return fail<Parsed>(diagnostic_codes::malformed, "invalid retreat cinematic payload" + at, logical_path);
            command.payload = RetreatCinematicPayload{playing != 0, frame};
        } else if (opcode == detail::opcode_retreat) {
            std::uint32_t action{}, reserved_retreat{};
            if (!reader.read_u32(action) || !reader.read_u32(reserved_retreat) || reserved_retreat
                || action < 1 || action > 3)
                return fail<Parsed>(diagnostic_codes::malformed, "invalid retreat payload" + at, logical_path);
            command.payload = RetreatPayload{static_cast<RetreatAction>(action)};
        } else if (opcode == detail::opcode_special_fire) {
            SpecialFirePayload special;
            std::uint32_t admission{}, reserved_special{};
            if (!reader.read_u64(special.target) || !reader.read_u32(admission)
                || !reader.read_u32(reserved_special)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated special fire payload" + at, logical_path);
            }
            if (reserved_special != 0 || admission > static_cast<std::uint32_t>(SpecialFireAdmission::ai_taskforce)) {
                return fail<Parsed>(diagnostic_codes::version, "unsupported special fire field" + at, logical_path);
            }
            special.admission = static_cast<SpecialFireAdmission>(admission);
            command.payload = special;
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
        } else if (opcode == 9 || opcode == detail::opcode_prepaid_buy) {
            BuyPayload buy;
            buy.prepaid = opcode == detail::opcode_prepaid_buy;
            if (!reader.read_u64(buy.type)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated buy payload" + at, logical_path);
            }
            command.payload = buy;
        } else if (opcode == detail::opcode_campaign_credit_release) {
            std::int64_t request{};
            if (!reader.read_i64(request)) return fail<Parsed>(diagnostic_codes::malformed, "truncated campaign release" + at, logical_path);
            command.payload = CampaignCreditReleasePayload{math::Fixed::from_raw(request)};
        } else if (opcode == detail::opcode_credit_grant) {
            std::int64_t amount{};
            if (!reader.read_i64(amount)) return fail<Parsed>(diagnostic_codes::malformed, "truncated credit grant" + at, logical_path);
            command.payload = CreditGrantPayload{math::Fixed::from_raw(amount)};
        } else if (opcode == detail::opcode_ai_reservation_debit) {
            std::int64_t amount{};
            if (!reader.read_i64(amount)) return fail<Parsed>(diagnostic_codes::malformed, "truncated reservation debit" + at, logical_path);
            command.payload = AiReservationDebitPayload{math::Fixed::from_raw(amount)};
        } else if (opcode == detail::opcode_cancel_entry) {
            CancelPayload cancel;
            if (!reader.read_u32(cancel.queue) || !reader.read_u32(cancel.index)
                || !reader.read_u64(cancel.entry_id) || cancel.index != 0 || cancel.entry_id == 0
                || !replay.setup.queue_identities) {
                return fail<Parsed>(diagnostic_codes::malformed, "invalid cancel entry payload or missing QIDS extension" + at, logical_path);
            }
            command.payload = cancel;
        } else if (opcode == 10) {
            CancelPayload cancel;
            if (!reader.read_u32(cancel.queue) || !reader.read_u32(cancel.index)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated cancel payload" + at, logical_path);
            }
            command.payload = cancel;
        } else if (opcode == 11 || opcode == detail::opcode_reserved_reinforce || opcode == detail::opcode_reinforce_facing) {
            ReinforcePayload reinforce;
            if (!reader.read_u64(reinforce.type) || !read_vec3(reader, reinforce.position)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated reinforce payload" + at, logical_path);
            }
            if (opcode == detail::opcode_reserved_reinforce
                && (!reader.read_u64(reinforce.pool_token) || reinforce.pool_token == 0)) {
                return fail<Parsed>(diagnostic_codes::malformed, "invalid reinforcement purchase token" + at, logical_path);
            }
            if (opcode == detail::opcode_reinforce_facing) {
                std::int64_t yaw{};
                if (!reader.read_u64(reinforce.pool_token) || !reader.read_i64(yaw)) {
                    return fail<Parsed>(diagnostic_codes::malformed, "truncated reinforcement facing" + at, logical_path);
                }
                reinforce.facing_yaw = math::Fixed::from_raw(yaw);
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
        const auto expected = detail::command_common_size + prefix + target_bytes + roster_bytes + detail::unit_list_header_size
            + 8U * static_cast<std::size_t>(listed);
        const auto tail_size = body_size >= expected ? body_size - expected : 0U;
        const bool provenance_tail = tail_size == 16U || tail_size == 24U;
        const bool append_tail = (opcode == 2 || opcode == detail::opcode_hazard_move)
            && (tail_size == 8U || tail_size == 24U);
        if (body_size < expected || tail_size != (append_tail ? 8U : 0U) + (provenance_tail ? 16U : 0U)) {
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
        if (append_tail) {
            std::uint32_t version{}, append_flags{};
            if (!reader.read_u32(version) || !reader.read_u32(append_flags)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated waypoint tail" + at, logical_path);
            }
            if (version != 1 || append_flags != 1) {
                return fail<Parsed>(diagnostic_codes::malformed, "unsupported waypoint tail" + at, logical_path);
            }
            std::get<MovePayload>(command.payload).append = true;
        }
        if (provenance_tail) {
            std::uint32_t tag{}, version{}, origin{};
            if (!reader.read_u32(tag) || !reader.read_u32(version)
                || !reader.read_u32(command.issuing_seat.value) || !reader.read_u32(origin))
                return fail<Parsed>(diagnostic_codes::malformed, "truncated controller provenance" + at, logical_path);
            if (tag != 0x4c525443U || version != 1)
                return fail<Parsed>(diagnostic_codes::version, "unsupported controller provenance tail" + at, logical_path);
            command.origin = static_cast<CommandOrigin>(origin);
            if ((command.origin != CommandOrigin::seat && command.origin != CommandOrigin::engine)
                || ((command.origin == CommandOrigin::seat) != (command.issuing_seat.value != 0)))
                return fail<Parsed>(diagnostic_codes::invalid_issuer, "invalid controller provenance shape" + at, logical_path);
        }
        replay.commands.push_back(std::move(command));
    }
    return core::Result<void>::success();
}

core::Result<void> parse_object_turns(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t object_turn_count) {
    using Parsed = void;
    if (object_turn_count > reader.remaining() / replay_object_turn_record_size) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "truncated object-turn table for declared count " + std::to_string(object_turn_count), logical_path);
    }
    replay.object_turns.reserve(object_turn_count);
    for (std::uint32_t index = 0; index < object_turn_count; ++index) {
        ObjectTurnRecord record;
        std::uint8_t ability{}, action{};
        std::uint16_t flags{};
        std::uint32_t reserved{};
        math::Vec3 position;
        if (!reader.read_u64(record.tick) || !reader.read_u64(record.use.unit) || !reader.read_u64(record.use.sequence)
            || !reader.read_u8(ability) || !reader.read_u8(action) || !reader.read_u16(flags)
            || !reader.read_u32(reserved) || !reader.read_u64(record.use.target) || !read_vec3(reader, position))
            return fail<Parsed>(diagnostic_codes::malformed,
                "truncated object-turn record " + std::to_string(index), logical_path);
        if (reserved != 0 || (flags & ~1U) != 0 || ((flags & 1U) == 0 && position != math::Vec3{}))
            return fail<Parsed>(diagnostic_codes::version,
                "unsupported object-turn flags or reserved bits at record " + std::to_string(index), logical_path);
        record.use.ability = static_cast<AbilityKind>(ability);
        record.use.action = static_cast<AbilityAction>(action);
        if ((flags & 1U) != 0) record.use.position = position;
        replay.object_turns.push_back(record);
    }
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical::replay_parse_detail

namespace eawr::sim::tactical {
std::uint8_t command_body_opcode(const PlayerCommand& command) noexcept { return detail::command_opcode(command); }
core::Result<PlayerCommand> decode_command_body(const std::span<const std::uint8_t> bytes, const TacticalSetup& setup,
    const std::optional<PlayerId> authenticated_player) {
    using namespace replay_detail;
    if (bytes.size() > 16384U)
        return fail<PlayerCommand>(diagnostic_codes::resource_limit, "command body exceeds routing limit", {});
    std::vector<std::uint8_t> framed;
    sim::detail::append_u32(framed, static_cast<std::uint32_t>(bytes.size()));
    framed.insert(framed.end(), bytes.begin(), bytes.end());
    Reader reader(framed);
    TacticalReplay replay;
    if (auto parsed = replay_parse_detail::parse_commands(reader, {}, replay, 1); !parsed)
        return core::Result<PlayerCommand>::failure(parsed.error());
    if (reader.remaining() != 0)
        return fail<PlayerCommand>(diagnostic_codes::malformed, "trailing command body bytes", {});
    auto command = std::move(replay.commands.front());
    if (authenticated_player) command.key.player_id = *authenticated_player;
    if (std::holds_alternative<GiftPayload>(command.payload) && !setup.controllers)
        return fail<PlayerCommand>(diagnostic_codes::invalid_command, "gift requires a controller policy", {});
    if (auto shape = detail::validate_command_shape(command, setup.players, "command body", {}); !shape)
        return core::Result<PlayerCommand>::failure(shape.error());
    if (command.origin != CommandOrigin::ordinary || std::holds_alternative<GiftPayload>(command.payload)) {
        if (auto valid = validate_command_provenance(command, setup); !valid)
            return core::Result<PlayerCommand>::failure(valid.error());
    }
    if (const auto* cancel = std::get_if<CancelPayload>(&command.payload);
        cancel && cancel->entry_id != 0 && !setup.queue_identities)
        return fail<PlayerCommand>(diagnostic_codes::invalid_command, "cancel entry requires QIDS extension", {});
    return core::Result<PlayerCommand>::success(std::move(command));
}
core::Result<std::vector<std::uint8_t>> encode_command_body(const PlayerCommand& command, const TacticalSetup& setup) {
    if (auto shape = detail::validate_command_shape(command, setup.players, "command body", {}); !shape)
        return core::Result<std::vector<std::uint8_t>>::failure(shape.error());
    if (auto provenance = validate_command_provenance(command, setup); !provenance)
        return core::Result<std::vector<std::uint8_t>>::failure(provenance.error());
    if (const auto* cancel = std::get_if<CancelPayload>(&command.payload);
        cancel && cancel->entry_id != 0 && !setup.queue_identities)
        return replay_detail::fail<std::vector<std::uint8_t>>(diagnostic_codes::invalid_command, "cancel entry requires QIDS extension", {});
    std::vector<std::uint8_t> bytes;
    detail::append_command(bytes, command);
    bytes.erase(bytes.begin(), bytes.begin() + 4);
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}
} // namespace eawr::sim::tactical
