#include "eawr/sim/galactic/replay.hpp"

#include "../replay_internal.hpp"
#include "encoding.hpp"
#include "eawr/sim/galactic/victory.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>

namespace eawr::sim::galactic {

namespace {

inline constexpr std::array<std::uint8_t, 8> replay_magic{'E', 'A', 'W', 'R', 'G', 'C', 'R', 0};
inline constexpr std::uint16_t flag_debug_commands = 1;
inline constexpr std::uint16_t flag_header_extensions = 2;
inline constexpr std::uint16_t graph_rules_tag = 1;
inline constexpr std::uint16_t fleet_control_tag = 2; // coordinator-reserved
inline constexpr std::uint16_t player_economy_tag = 3; // coordinator-reserved
inline constexpr std::uint16_t control_tag = 8; // coordinator-reserved
inline constexpr std::uint16_t movement_tag = 4; // coordinator-reserved
inline constexpr std::uint16_t seat_control_tag = 5; // coordinator-reserved
inline constexpr std::uint16_t fiscal_tag = 6; // coordinator-reserved
inline constexpr std::uint16_t census_tag = 10; // coordinator-reserved
inline constexpr std::uint16_t production_tag = 7; // coordinator-reserved
inline constexpr std::uint16_t victory_tag = 14; // coordinator-reserved
inline constexpr std::uint16_t outcomes_tag = 11; // coordinator-reserved trusted returns
inline constexpr std::uint16_t observer_tag = 12; // coordinator-reserved
inline constexpr std::uint16_t economy_corruption_tag = 15; // coordinator-reserved
inline constexpr std::uint16_t native_effects_tag = 16; // coordinator-reserved
inline constexpr std::uint16_t ability_host_tag = 17; // coordinator-reserved
inline constexpr std::uint16_t interactive_seat_tag = 18; // solo summary modal policy
inline constexpr std::uint16_t income_tag = 13; // coordinator-reserved
// Keep count admission and unknown-tag admission on the same supported table.
// New extension consumers append their tag here; no separate literal bound.
inline constexpr std::array header_extension_tags{
    graph_rules_tag, fleet_control_tag, player_economy_tag, movement_tag,
    seat_control_tag, fiscal_tag, production_tag, control_tag, census_tag, outcomes_tag, observer_tag, income_tag, victory_tag, economy_corruption_tag, native_effects_tag, ability_host_tag, interactive_seat_tag};

template <typename T>
[[nodiscard]] core::Result<T> fail(const std::string_view code, std::string message, const std::string_view path = {}) {
    auto diagnostic = detail::diagnostic(code, std::move(message));
    if (!path.empty()) diagnostic.logical_path = std::string(path);
    return core::Result<T>::failure(std::move(diagnostic));
}

[[nodiscard]] std::string key_text(const CommandOrderKey& key) {
    return "command (tick " + std::to_string(key.tick) + ", seat " + std::to_string(key.seat.value) + ", sequence "
        + std::to_string(key.sequence) + ")";
}

} // namespace

core::Result<void> validate_replay(const GalacticReplay& replay) {
    if (auto valid = validate_setup(replay.setup); !valid) return valid;
    if (replay.final_tick_count > galactic_max_ticks) {
        return fail<void>(diagnostic_codes::replay_resource_limit, "final tick exceeds the galactic tick limit");
    }
    if (replay.commands.size() > galactic_max_commands) {
        return fail<void>(diagnostic_codes::replay_resource_limit, "too many commands");
    }
    const auto& seats = replay.setup.seats;
    if (replay.setup.interactive_seat.value && std::none_of(seats.begin(), seats.end(), [&](const auto& seat) {
        return seat.seat_id == replay.setup.interactive_seat && seat.active && seat.kind == SeatKind::human;
    })) return fail<void>(diagnostic_codes::replay_malformed, "interactive seat must be an active human");
    // Check the final encoded size before building a potentially oversized byte vector.
    std::uint64_t encoded_size = 128 + replay.setup.players.size() * 12 + replay.setup.seats.size() * 18
        + replay.setup.planets.size() * 36 + replay.setup.routes.size() * 24 + replay.setup.fleets.size() * 24;
    if (replay.setup.rules.maximum_fleet_movement_distance) encoded_size += 22 + replay.setup.routes.size() * 16;
    if (!replay.setup.outcomes.empty()) {
        encoded_size += 10;
        for (const auto& entry : replay.setup.outcomes) encoded_size += 20 + entry.payload.size();
    }
    if (replay.setup.interactive_seat.value) encoded_size += 10;
    if (replay.setup.fleet_control) encoded_size += 20 + replay.setup.fleet_control->types.size() * 11
        + replay.setup.fleet_control->detached_units.size() * 28 + replay.setup.fleets.size() * 9;
    if (replay.setup.fiscal) {
        std::vector<std::uint8_t> payload; detail::encode_fiscal(payload, *replay.setup.fiscal);
        encoded_size += 6 + payload.size();
    }
    if (replay.setup.player_economy) encoded_size += 14 + replay.setup.player_economy->players.size() * 36;
    if (replay.setup.observer) {
        std::vector<std::uint8_t> payload; detail::encode_observer(payload, *replay.setup.observer);
        encoded_size += 6 + payload.size();
    }
    if (replay.setup.victory) {
        std::vector<std::uint8_t> payload; detail::encode_victory(payload, *replay.setup.victory);
        encoded_size += 6 + payload.size();
    }
    if (replay.setup.production) {
        std::vector<std::uint8_t> payload; detail::encode_production(payload, *replay.setup.production);
        encoded_size += 6 + payload.size();
    }
    if (replay.setup.movement) {
        std::vector<std::uint8_t> payload; detail::encode_movement(payload, *replay.setup.movement, replay.setup.fleets);
        encoded_size += 6 + payload.size();
    }
    if (replay.setup.seat_control) {
        encoded_size += 18 + replay.setup.seat_control->teams.size() * 9;
        for (const auto& controller : replay.setup.seat_control->controllers)
            encoded_size += 14 + controller.configuration.size() + controller.state.size();
    }
    if (replay.setup.census) {
        std::vector<std::uint8_t> payload; detail::encode_census(payload, *replay.setup.census);
        encoded_size += 6 + payload.size();
    }
    if (replay.setup.economy_corruption) {
        std::vector<std::uint8_t> payload; detail::encode_economy_corruption(payload, *replay.setup.economy_corruption);
        encoded_size += 6 + payload.size();
    }
    if (replay.setup.ability_host) {
        std::vector<std::uint8_t> payload; detail::encode_ability_setup(payload, *replay.setup.ability_host);
        encoded_size += 6 + payload.size();
    }
    if (replay.setup.income) {
        std::vector<std::uint8_t> payload; detail::encode_income(payload, *replay.setup.income);
        encoded_size += 6 + payload.size();
    }
    if (encoded_size > galactic_replay_max_bytes) return fail<void>(diagnostic_codes::replay_resource_limit, "the replay exceeds the size limit");
    if (replay.setup.control) {
        std::vector<std::uint8_t> payload; detail::encode_control(payload, *replay.setup.control);
        encoded_size += 6 + payload.size();
    }
    for (const auto& fleet : replay.setup.fleets) encoded_size += fleet.members.size() * 16;
    for (std::size_t index = 0; index < replay.commands.size(); ++index) {
        const auto& command = replay.commands[index];
        const auto context = key_text(command.key);
        if (index > 0 && !(replay.commands[index - 1].key < command.key)) {
            return fail<void>(diagnostic_codes::replay_order, context + ": not after the previous command in (tick, seat, sequence) order");
        }
        if (command.key.tick >= replay.final_tick_count) {
            return fail<void>(diagnostic_codes::replay_order, context + ": at or after the final tick");
        }
        const auto seat = std::lower_bound(seats.begin(), seats.end(), command.key.seat,
            [](const Seat& row, const SeatId value) { return row.seat_id < value; });
        if (seat == seats.end() || seat->seat_id != command.key.seat) {
            return fail<void>(diagnostic_codes::replay_command, context + ": no such seat");
        }
        const auto layer = layer_of(command.payload);
        if (std::holds_alternative<PostLoadFixup>(command.payload))
            return fail<void>(diagnostic_codes::replay_command, context + ": a load prologue is separate from player input");
        if (const auto* tactical = std::get_if<TacticalLayerCommand>(&command.payload);
            tactical != nullptr && tactical->body.size() > galactic_replay_max_payload - 8) {
            return fail<void>(diagnostic_codes::replay_command, context + ": malformed payload");
        }
        const auto bytes = encode_payload(command.payload);
        if (bytes.size() > galactic_replay_max_payload
            || decode_payload(layer, opcode_of(command.payload), bytes) != std::optional(command.payload)) {
            return fail<void>(diagnostic_codes::replay_command, context + ": malformed payload");
        }
        encoded_size += 27 + bytes.size();
        if (encoded_size > galactic_replay_max_bytes) {
            return fail<void>(diagnostic_codes::replay_resource_limit, "the replay exceeds the size limit");
        }
        const auto opcode = opcode_of(command.payload);
        const bool debug = layer == CommandLayer::session
            && (opcode == session_opcodes::debug_set_planet_owner || opcode == session_opcodes::debug_set_fleet_orbit);
        if (debug && !replay.setup.debug_commands) {
            return fail<void>(diagnostic_codes::replay_command, context + ": a debug command in a setup without debug commands");
        }
        if (const auto* tactical = std::get_if<TacticalLayerCommand>(&command.payload);
            tactical != nullptr && tactical->conflict == invalid_entity_id) {
            return fail<void>(diagnostic_codes::replay_command, context + ": a tactical command names no conflict");
        }
    }
    if (replay.native_effects.size() > galactic_max_commands - replay.commands.size())
        return fail<void>(diagnostic_codes::replay_resource_limit, "too many native journal effects");
    if (!replay.native_effects.empty()) encoded_size += 14;
    for (std::size_t i = 0; i < replay.native_effects.size(); ++i) {
        const auto& effect = replay.native_effects[i];
        if (effect.sequence != i + 1 || effect.frontier > replay.final_tick_count
            || effect.phase > NativeEffectPhase::world_service || effect.kind < NativeEffectKind::fleet
            || effect.kind > NativeEffectKind::story_reward
            || (effect.phase == NativeEffectPhase::world_service && effect.frontier == replay.final_tick_count)
            || effect.payload.size() > galactic_replay_max_payload)
            return fail<void>(diagnostic_codes::replay_order, "invalid native effect framing, phase or frontier");
        if (i && (effect.frontier < replay.native_effects[i - 1].frontier
            || (effect.frontier == replay.native_effects[i - 1].frontier && effect.phase < replay.native_effects[i - 1].phase)))
            return fail<void>(diagnostic_codes::replay_order, "native journal effects are not in owner execution order");
        encoded_size += 23 + effect.payload.size();
        if (encoded_size > galactic_replay_max_bytes)
            return fail<void>(diagnostic_codes::replay_resource_limit, "native journal exceeds the replay byte limit");
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> write_replay(const GalacticReplay& replay) {
    using Bytes = std::vector<std::uint8_t>;
    if (auto valid = validate_replay(replay); !valid) return core::Result<Bytes>::failure(valid.error());
    const auto& setup = replay.setup;
    Bytes bytes(replay_magic.begin(), replay_magic.end());
    detail::put_u16(bytes, galactic_replay_format_version);
    detail::put_u16(bytes, static_cast<std::uint16_t>((setup.debug_commands ? flag_debug_commands : 0)
        | ((setup.rules.maximum_fleet_movement_distance || setup.player_economy || setup.seat_control || setup.movement || setup.fiscal || setup.control || setup.production || setup.census || !setup.outcomes.empty() || setup.observer || setup.income || setup.victory || setup.economy_corruption || setup.ability_host || setup.interactive_seat.value || !replay.native_effects.empty()) ? flag_header_extensions : 0)));
    detail::put_u32(bytes, galactic_rules_version);
    detail::put_bytes(bytes, setup.content_identity);
    detail::put_bytes(bytes, setup.campaign_identity);
    detail::put_u64(bytes, setup.seed);
    detail::put_u64(bytes, replay.final_tick_count);
    if (setup.rules.maximum_fleet_movement_distance || setup.player_economy || setup.seat_control || setup.movement || setup.fiscal || setup.control || setup.production || setup.census || !setup.outcomes.empty() || setup.observer || setup.income || setup.victory || setup.economy_corruption || setup.ability_host || setup.interactive_seat.value || !replay.native_effects.empty()) {
        detail::put_u32(bytes, (setup.rules.maximum_fleet_movement_distance ? 1U : 0U)
            + (setup.fleet_control ? 1U : 0U) + (setup.player_economy ? 1U : 0U) + (setup.seat_control ? 1U : 0U) + (setup.movement ? 1U : 0U) + (setup.fiscal ? 1U : 0U) + (setup.control ? 1U : 0U) + (setup.production ? 1U : 0U) + (setup.census ? 1U : 0U) + (!setup.outcomes.empty() ? 1U : 0U) + (setup.observer ? 1U : 0U) + (setup.income ? 1U : 0U) + (setup.victory ? 1U : 0U) + (setup.economy_corruption ? 1U : 0U) + (setup.ability_host ? 1U : 0U) + (setup.interactive_seat.value ? 1U : 0U) + (!replay.native_effects.empty() ? 1U : 0U));
    }
    if (setup.rules.maximum_fleet_movement_distance) {
        detail::put_u16(bytes, graph_rules_tag);
        detail::put_u32(bytes, static_cast<std::uint32_t>(12 + setup.routes.size() * 16));
        detail::put_i64(bytes, setup.rules.maximum_fleet_movement_distance->raw());
        detail::put_u32(bytes, static_cast<std::uint32_t>(setup.routes.size()));
        for (const auto& route : setup.routes) {
            detail::put_u64(bytes, route.id);
            detail::put_i64(bytes, route.hs_speed_factor.raw());
        }
    }
    if (setup.fleet_control) {
        Bytes payload; detail::encode_fleet_control(payload, *setup.fleet_control, setup.fleets);
        detail::put_u16(bytes, fleet_control_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.player_economy) {
        Bytes payload; detail::encode_player_economy(payload, *setup.player_economy);
        detail::put_u16(bytes, player_economy_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.movement) {
        Bytes payload; detail::encode_movement(payload, *setup.movement, setup.fleets);
        detail::put_u16(bytes, movement_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.seat_control) {
        Bytes payload; detail::encode_seat_control(payload, *setup.seat_control);
        detail::put_u16(bytes, seat_control_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.fiscal) {
        Bytes payload; detail::encode_fiscal(payload, *setup.fiscal);
        detail::put_u16(bytes, fiscal_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.production) {
        Bytes payload; detail::encode_production(payload, *setup.production);
        detail::put_u16(bytes, production_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.control) {
        Bytes payload; detail::encode_control(payload, *setup.control);
        detail::put_u16(bytes, control_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.census) {
        Bytes payload; detail::encode_census(payload, *setup.census);
        detail::put_u16(bytes, census_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (!setup.outcomes.empty()) {
        Bytes payload; detail::put_u32(payload, static_cast<std::uint32_t>(setup.outcomes.size()));
        for (const auto& entry : setup.outcomes) {
            detail::put_u64(payload, entry.completed_tick); detail::put_u64(payload, entry.conflict);
            detail::put_u32(payload, static_cast<std::uint32_t>(entry.payload.size())); detail::put_bytes(payload, entry.payload);
        }
        detail::put_u16(bytes, outcomes_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.observer) {
        Bytes payload; detail::encode_observer(payload, *setup.observer);
        detail::put_u16(bytes, observer_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.income) {
        Bytes payload; detail::encode_income(payload, *setup.income);
        detail::put_u16(bytes, income_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.victory) {
        Bytes payload; detail::encode_victory(payload, *setup.victory);
        detail::put_u16(bytes, victory_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.economy_corruption) {
        Bytes payload; detail::encode_economy_corruption(payload, *setup.economy_corruption);
        detail::put_u16(bytes, economy_corruption_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (!replay.native_effects.empty()) {
        Bytes payload;
        detail::put_u64(payload, replay.native_effects.size());
        for (const auto& effect : replay.native_effects) {
            detail::put_u64(payload, effect.frontier); detail::put_u64(payload, effect.sequence);
            detail::put_u8(payload, static_cast<std::uint8_t>(effect.phase));
            detail::put_u16(payload, static_cast<std::uint16_t>(effect.kind));
            detail::put_u32(payload, static_cast<std::uint32_t>(effect.payload.size())); detail::put_bytes(payload, effect.payload);
        }
        detail::put_u16(bytes, native_effects_tag);
        detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.ability_host) {
        Bytes payload; detail::encode_ability_setup(payload, *setup.ability_host);
        detail::put_u16(bytes, ability_host_tag); detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (setup.interactive_seat.value) {
        detail::put_u16(bytes, interactive_seat_tag); detail::put_u32(bytes, 4);
        detail::put_u32(bytes, setup.interactive_seat.value);
    }
    detail::encode_players(bytes, setup.players);
    detail::put_u32(bytes, static_cast<std::uint32_t>(setup.seats.size()));
    for (const auto& seat : setup.seats) {
        detail::put_u32(bytes, seat.seat_id.value);
        detail::put_u32(bytes, seat.player);
        detail::put_u8(bytes, static_cast<std::uint8_t>(seat.kind));
        detail::put_u32(bytes, seat.lobby_slot);
        detail::put_u8(bytes, seat.active ? 1U : 0U);
        detail::put_u32(bytes, seat.human_side_id);
    }
    detail::put_u32(bytes, setup.rules.fiscal_cycle_frames);
    detail::put_u32(bytes, static_cast<std::uint32_t>(setup.planets.size()));
    for (const auto& planet : setup.planets) detail::encode_planet(bytes, planet);
    detail::put_u32(bytes, static_cast<std::uint32_t>(setup.routes.size()));
    for (const auto& route : setup.routes) detail::encode_route(bytes, route);
    detail::put_u32(bytes, static_cast<std::uint32_t>(setup.fleets.size()));
    for (const auto& fleet : setup.fleets) detail::encode_fleet(bytes, fleet);
    detail::put_u64(bytes, replay.commands.size());
    for (const auto& command : replay.commands) {
        detail::put_u64(bytes, command.key.tick);
        detail::put_u32(bytes, command.key.seat.value);
        detail::put_u64(bytes, command.key.sequence);
        detail::put_u8(bytes, static_cast<std::uint8_t>(layer_of(command.payload)));
        detail::put_u16(bytes, opcode_of(command.payload));
        const auto payload = encode_payload(command.payload);
        detail::put_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        detail::put_bytes(bytes, payload);
    }
    if (bytes.size() > galactic_replay_max_bytes) {
        return fail<Bytes>(diagnostic_codes::replay_resource_limit, "the replay exceeds the size limit");
    }
    return core::Result<Bytes>::success(std::move(bytes));
}

namespace {

core::Result<void> parse_native_effects_extension(sim::detail::Reader& input, GalacticReplay& replay,
    const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    std::uint64_t effects{};
    if (!input.read_u64(effects) || effects > galactic_max_commands || effects * 23 > input.remaining())
        return malformed("bad native effect count");
    for (std::uint64_t i = 0; i < effects; ++i) {
        NativeEffect effect; std::uint8_t phase{}; std::uint16_t kind{}; std::uint32_t effect_length{};
        if (!input.read_u64(effect.frontier) || !input.read_u64(effect.sequence) || !input.read_u8(phase)
            || !input.read_u16(kind) || !input.read_u32(effect_length) || effect_length > galactic_replay_max_payload || effect_length > input.remaining())
            return malformed("truncated native effect");
        effect.phase = static_cast<NativeEffectPhase>(phase); effect.kind = static_cast<NativeEffectKind>(kind);
        effect.payload.resize(effect_length); if (!input.read_bytes(effect.payload)) return malformed("truncated native payload");
        replay.native_effects.push_back(std::move(effect));
    }
    if (input.remaining() || replay.native_effects.empty()) return malformed("invalid native journal extension");
    return core::Result<void>::success();
}

core::Result<void> parse_outcomes_extension(sim::detail::Reader& input, GalacticSetup& setup,
    const auto& count, const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    std::uint32_t entries{};
    if (!count(entries, 20) || !entries) return malformed("invalid outcome count");
    for (std::uint32_t i = 0; i < entries; ++i) {
        OutcomeJournalEntry entry; std::uint32_t size{};
        if (!input.read_u64(entry.completed_tick) || !input.read_u64(entry.conflict)
            || !input.read_u32(size) || !size || size > 1'048'576 || size > input.remaining())
            return malformed("invalid outcome entry");
        entry.payload.resize(size); if (!input.read_bytes(entry.payload)) return malformed("truncated outcome payload");
        setup.outcomes.push_back(std::move(entry));
    }
    return core::Result<void>::success();
}

core::Result<void> parse_graph_rules_extension(sim::detail::Reader& input, GalacticSetup& setup,
    std::vector<std::pair<RouteId, math::Fixed>>& route_factors, const auto& count,
    const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    std::int64_t maximum{}; std::uint32_t routes{};
    if (!input.read_i64(maximum) || !count(routes, 16)) return malformed("bad graph rules extension");
    setup.rules.maximum_fleet_movement_distance = math::Fixed::from_raw(maximum);
    for (std::uint32_t i = 0; i < routes; ++i) {
        RouteId id{}; std::int64_t factor{};
        if (!input.read_u64(id) || !input.read_i64(factor)) return malformed("truncated graph factors");
        route_factors.emplace_back(id, math::Fixed::from_raw(factor));
    }
    return core::Result<void>::success();
}

core::Result<void> parse_seat_control_extension(sim::detail::Reader& input, GalacticSetup& setup,
    const auto& count, const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    setup.seat_control.emplace(); auto& control = *setup.seat_control;
    std::uint32_t teams{}, controllers{};
    if (!count(teams, 9) || teams > 2) return malformed("bad team policy count");
    for (std::uint32_t i = 0; i < teams; ++i) {
        TeamLead team; std::uint8_t perspective{};
        if (!input.read_u32(team.side) || !input.read_u32(team.lead.value)
            || !input.read_u8(perspective) || perspective > 1) return malformed("bad team policy");
        team.observer_ai = perspective != 0; control.teams.push_back(team);
    }
    if (!count(controllers, 14) || controllers > galactic_max_players) return malformed("bad controller count");
    for (std::uint32_t i = 0; i < controllers; ++i) {
        FactionController row; std::uint8_t kind{}, dropout{};
        if (!input.read_u32(row.player) || !input.read_u8(kind) || !input.read_u8(dropout))
            return malformed("bad controller policy");
        row.kind = static_cast<FactionControl>(kind); row.dropout = static_cast<FactionControl>(dropout);
        for (auto* value : {&row.configuration, &row.state}) {
            std::uint32_t size{};
            if (!input.read_u32(size) || size > 16384 || size > input.remaining()) return malformed("bad controller payload");
            value->resize(size); if (!input.read_bytes(*value)) return malformed("truncated controller payload");
        }
        control.controllers.push_back(std::move(row));
    }
    return core::Result<void>::success();
}

core::Result<void> parse_fleet_control_extension(sim::detail::Reader& input, GalacticSetup& setup,
    std::vector<std::pair<FleetId, std::uint8_t>>& fleet_slots, const auto& count,
    const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    setup.fleet_control.emplace(); auto& control = *setup.fleet_control;
    const auto flag = [&](bool& value) {
        std::uint8_t raw{}; if (!input.read_u8(raw) || raw > 1) return false;
        value = raw != 0; return true;
    };
    if (!flag(control.gates.queued_campaign_conflict) || !flag(control.gates.active_speech)) return malformed("bad fleet gates");
    std::uint32_t types{}, units{}, slots{};
    if (!count(types, 11)) return malformed("bad fleet types");
    for (std::uint32_t i = 0; i < types; ++i) {
        FleetTypeTraits row;
        if (!input.read_u64(row.type) || !flag(row.transport) || !flag(row.can_hyperspace_without_ability) || !flag(row.requires_live_children)) return malformed("bad fleet type");
        control.types.push_back(row);
    }
    if (!count(units, 28)) return malformed("bad detached unit count");
    for (std::uint32_t i = 0; i < units; ++i) {
        DetachedUnit row;
        if (!input.read_u64(row.member.id) || !input.read_u64(row.member.type) || !input.read_u32(row.owner)
            || !input.read_u64(row.planet)) return malformed("truncated detached unit");
        control.detached_units.push_back(row);
    }
    if (!count(slots, 9)) return malformed("bad fleet slot count");
    for (std::uint32_t i = 0; i < slots; ++i) {
        FleetId id{}; std::uint8_t slot{};
        if (!input.read_u64(id) || !input.read_u8(slot) || slot > 2) return malformed("bad fleet slot");
        fleet_slots.emplace_back(id, slot);
    }
    return core::Result<void>::success();
}

core::Result<void> parse_fiscal_contributions(sim::detail::Reader& input, FiscalSetup& fiscal, std::uint32_t& rows,
    const auto& count, const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    std::uint8_t tail_version{}; std::int64_t bonus{}, penalty{};
    if (!input.read_u8(tail_version) || tail_version != 1 || !input.read_i64(bonus) || !input.read_i64(penalty))
        return malformed("bad fiscal contribution tail tail_version or constants");
    auto& contribution_rules = fiscal.contributions.emplace();
    contribution_rules.maximum_alignment_bonus = math::Fixed::from_raw(bonus);
    contribution_rules.maximum_alignment_penalty = math::Fixed::from_raw(penalty);
    if (!count(rows, 16)) return malformed("bad fiscal political curve count");
    for (std::uint32_t i = 0; i < rows; ++i) {
        std::int64_t x{}, y{};
        if (!input.read_i64(x) || !input.read_i64(y)) return malformed("bad fiscal curve point");
        contribution_rules.political_curve.push_back({math::Fixed::from_raw(x), math::Fixed::from_raw(y)});
    }
    if (!count(rows, 5) || rows != fiscal.players.size()) return malformed("bad fiscal faction count");
    for (std::uint32_t i = 0; i < rows; ++i) {
        IncomeFaction row; std::uint8_t alignment{};
        if (!input.read_u32(row.player) || !input.read_u8(alignment) || alignment > 2) return malformed("bad fiscal faction alignment");
        row.alignment = static_cast<IncomeAlignment>(alignment); contribution_rules.factions.push_back(row);
    }
    if (!count(rows, 17) || rows != fiscal.planets.size()) return malformed("bad fiscal alignment planet count");
    for (std::uint32_t i = 0; i < rows; ++i) {
        IncomePlanet row; std::uint8_t alignment{}; std::int64_t strength{};
        if (!input.read_u64(row.planet) || !input.read_u8(alignment) || alignment > 2 || !input.read_i64(strength))
            return malformed("bad fiscal planet alignment");
        row.alignment = static_cast<IncomeAlignment>(alignment); row.strength = math::Fixed::from_raw(strength); contribution_rules.planets.push_back(row);
    }
    if (!count(rows, 16)) return malformed("bad fiscal trade count");
    for (std::uint32_t i = 0; i < rows; ++i) {
        IncomeRoute row; std::int64_t factor{};
        if (!input.read_u64(row.route) || !input.read_i64(factor)) return malformed("bad fiscal trade factor");
        row.factor = math::Fixed::from_raw(factor); contribution_rules.routes.push_back(row);
    }
    return core::Result<void>::success();
}

core::Result<void> parse_fiscal_extension(sim::detail::Reader& input, GalacticSetup& setup,
    const auto& count, const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    setup.fiscal.emplace(); auto& fiscal = *setup.fiscal;
    std::int64_t cap{}; std::uint32_t rows{};
    if (!input.read_i64(cap) || !count(rows, 13) || rows > galactic_max_players) return malformed("bad fiscal players");
    fiscal.credit_cap_per_planet = math::Fixed::from_raw(cap);
    for (std::uint32_t i = 0; i < rows; ++i) {
        FiscalPlayer row; std::uint8_t eligible{}; std::int64_t multiplier{};
        if (!input.read_u32(row.player) || !input.read_u8(eligible) || eligible > 1 || !input.read_i64(multiplier)) return malformed("bad fiscal player");
        row.earns_planet_income = eligible != 0; row.positive_credit_multiplier = math::Fixed::from_raw(multiplier);
        fiscal.players.push_back(row);
    }
    if (!count(rows, 16)) return malformed("bad fiscal planets");
    for (std::uint32_t i = 0; i < rows; ++i) {
        PlanetBaseIncome row; std::int64_t credits{};
        if (!input.read_u64(row.planet) || !input.read_i64(credits)) return malformed("bad fiscal planet");
        row.credits = math::Fixed::from_raw(credits); fiscal.planets.push_back(row);
    }
    if (input.remaining()) {
        if (auto parsed = parse_fiscal_contributions(input, fiscal, rows, count, logical_path); !parsed)
            return core::Result<Out>::failure(parsed.error());
    }
    return core::Result<void>::success();
}

core::Result<void> parse_player_economy_extension(sim::detail::Reader& input, GalacticSetup& setup,
    const auto& count, const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    std::uint32_t rows{};
    if (!count(rows, 36) || rows > galactic_max_players) return malformed("bad player economy count");
    setup.player_economy.emplace();
    for (std::uint32_t i = 0; i < rows; ++i) {
        GalacticPlayerEconomy row; std::int64_t credits{}, cap{};
        if (!input.read_u32(row.player) || !input.read_i64(credits) || !input.read_i64(cap)
            || !input.read_i64(row.current_tech) || !input.read_i64(row.maximum_tech))
            return malformed("truncated player economy row");
        row.credits = math::Fixed::from_raw(credits); row.credit_cap = math::Fixed::from_raw(cap);
        setup.player_economy->players.push_back(row);
    }
    return core::Result<void>::success();
}

core::Result<void> parse_replay_extensions(sim::detail::Reader& reader, GalacticReplay& replay,
    std::vector<std::pair<RouteId, math::Fixed>>& route_factors,
    std::vector<std::pair<FleetId, std::uint8_t>>& fleet_slots,
    std::vector<std::pair<FleetId, FleetMotion>>& fleet_motions,
    const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    auto& setup = replay.setup;
    std::uint32_t extensions{};
    if (!reader.read_u32(extensions) || extensions == 0 || extensions > header_extension_tags.size()) return malformed("bad extension count");
    std::uint16_t previous_tag{};
    for (std::uint32_t index = 0; index < extensions; ++index) {
        std::uint32_t length{}; std::uint16_t tag{};
        if (!reader.read_u16(tag) || tag <= previous_tag
            || std::find(header_extension_tags.begin(), header_extension_tags.end(), tag) == header_extension_tags.end()
            || !reader.read_u32(length) || length > reader.remaining()) return malformed("unknown or invalid header extension");
        previous_tag = tag;
        std::vector<std::uint8_t> payload(length);
        if (!reader.read_bytes(payload)) return malformed("truncated extension");
        sim::detail::Reader input(payload);
        const auto count = [&](std::uint32_t& value, const std::size_t width) {
            return input.read_u32(value) && value <= galactic_max_objects && std::size_t{value} * width <= input.remaining();
        };
        if (tag == interactive_seat_tag) {
            if (!input.read_u32(setup.interactive_seat.value) || !setup.interactive_seat.value || input.remaining())
                return malformed("invalid interactive seat extension");
            continue;
        }
        if (tag == native_effects_tag) {
            if (auto parsed = parse_native_effects_extension(input, replay, logical_path); !parsed)
                return core::Result<Out>::failure(parsed.error());
            continue;
        }
        if (tag == ability_host_tag) {
            auto decoded = detail::decode_ability_setup(payload);
            if (!decoded) return malformed("invalid live ability host extension");
            setup.ability_host = std::move(*decoded); continue;
        }
        if (tag == economy_corruption_tag) {
            auto decoded = detail::decode_economy_corruption(payload);
            if (!decoded) return malformed("invalid corruption economy extension");
            setup.economy_corruption = std::move(*decoded);
            continue;
        }
        if (tag == income_tag) {
            auto decoded = detail::decode_income(payload);
            if (!decoded) return malformed("invalid income extension");
            setup.income = std::move(*decoded);
            continue;
        }
        if (tag == observer_tag) {
            auto decoded = detail::decode_observer(payload);
            if (!decoded) return malformed("invalid observer extension");
            setup.observer = std::move(*decoded);
            continue;
        }
        if (tag == census_tag) {
            auto decoded = detail::decode_census(payload);
            if (!decoded) return malformed("invalid census extension");
            setup.census = std::move(*decoded);
            continue;
        } else if (tag == victory_tag) {
            auto value = detail::decode_victory(payload);
            if (!value) return malformed("bad victory extension");
            setup.victory = std::move(*value);
            continue;
        } else if (tag == outcomes_tag) {
            if (auto parsed = parse_outcomes_extension(input, setup, count, logical_path); !parsed)
                return core::Result<Out>::failure(parsed.error());
        } else if (tag == production_tag) {
            auto decoded = detail::decode_production(payload);
            if (!decoded) return malformed("invalid production extension");
            setup.production = std::move(*decoded);
            continue;
        } else if (tag == graph_rules_tag) {
            if (auto parsed = parse_graph_rules_extension(input, setup, route_factors, count, logical_path); !parsed)
                return core::Result<Out>::failure(parsed.error());
        } else if (tag == control_tag) {
            auto decoded = detail::decode_control(payload);
            if (!decoded) return malformed(decoded.error().message);
            setup.control = std::move(decoded).value();
            continue;
        } else if (tag == movement_tag) {
            auto decoded = detail::decode_movement(payload);
            if (!decoded) return malformed(decoded.error().message);
            setup.movement = std::move(decoded.value().inputs);
            fleet_motions = std::move(decoded.value().fleets);
            continue;
        } else if (tag == seat_control_tag) {
            if (auto parsed = parse_seat_control_extension(input, setup, count, logical_path); !parsed)
                return core::Result<Out>::failure(parsed.error());
        } else if (tag == fleet_control_tag) {
            if (auto parsed = parse_fleet_control_extension(input, setup, fleet_slots, count, logical_path); !parsed)
                return core::Result<Out>::failure(parsed.error());
        } else if (tag == fiscal_tag) {
            if (auto parsed = parse_fiscal_extension(input, setup, count, logical_path); !parsed)
                return core::Result<Out>::failure(parsed.error());
        } else {
            if (auto parsed = parse_player_economy_extension(input, setup, count, logical_path); !parsed)
                return core::Result<Out>::failure(parsed.error());
        }
        if (input.remaining() != 0) return malformed("extension has trailing bytes");
    }
    return core::Result<void>::success();
}

core::Result<void> parse_replay_participants(sim::detail::Reader& reader, GalacticSetup& setup,
    const auto& count, const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    std::uint32_t players{};
    if (!count(players, galactic_max_players, 12)) return malformed("bad player count");
    for (std::uint32_t index = 0; index < players; ++index) {
        GalacticPlayer player;
        if (!reader.read_u32(player.player_id) || !reader.read_u32(player.faction_id) || !reader.read_u32(player.flags)) {
            return malformed("truncated player");
        }
        setup.players.push_back(player);
    }
    std::uint32_t seats{};
    if (!count(seats, galactic_max_seats, 18)) return malformed("bad seat count");
    for (std::uint32_t index = 0; index < seats; ++index) {
        Seat seat;
        std::uint8_t kind{};
        std::uint8_t active{};
        if (!reader.read_u32(seat.seat_id.value) || !reader.read_u32(seat.player) || !reader.read_u8(kind)
            || !reader.read_u32(seat.lobby_slot) || !reader.read_u8(active) || active > 1
            || !reader.read_u32(seat.human_side_id)) {
            return malformed("truncated or bad seat");
        }
        seat.kind = static_cast<SeatKind>(kind);
        seat.active = active == 1;
        setup.seats.push_back(seat);
    }
    if (!reader.read_u32(setup.rules.fiscal_cycle_frames)) return malformed("truncated rules");
    return core::Result<void>::success();
}

core::Result<void> parse_replay_objects(sim::detail::Reader& reader, GalacticSetup& setup,
    std::vector<std::pair<RouteId, math::Fixed>>& route_factors,
    std::vector<std::pair<FleetId, std::uint8_t>>& fleet_slots,
    std::vector<std::pair<FleetId, FleetMotion>>& fleet_motions,
    const auto& count, const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    std::uint32_t planets{};
    if (!count(planets, galactic_max_objects, 36)) return malformed("bad planet count");
    for (std::uint32_t index = 0; index < planets; ++index) {
        Planet planet;
        std::int64_t x{};
        std::int64_t y{};
        std::int64_t z{};
        if (!reader.read_u64(planet.id) || !reader.read_i64(x) || !reader.read_i64(y) || !reader.read_i64(z)
            || !reader.read_u32(planet.owner)) {
            return malformed("truncated planet");
        }
        planet.position = {math::Fixed::from_raw(x), math::Fixed::from_raw(y), math::Fixed::from_raw(z)};
        setup.planets.push_back(planet);
    }
    std::uint32_t routes{};
    if (!count(routes, galactic_max_objects, 24)) return malformed("bad route count");
    if (setup.rules.maximum_fleet_movement_distance && route_factors.size() != routes) return malformed("graph route count differs");
    std::size_t objects = std::size_t{planets} + routes;
    if (objects > galactic_max_objects) {
        return fail<Out>(diagnostic_codes::replay_resource_limit, "too many galaxy objects", logical_path);
    }
    for (std::uint32_t index = 0; index < routes; ++index) {
        Route route;
        if (!reader.read_u64(route.id) || !reader.read_u64(route.from) || !reader.read_u64(route.to)) return malformed("truncated route");
        if (setup.rules.maximum_fleet_movement_distance) {
            if (route_factors.size() != routes || route_factors[index].first != route.id)
                return malformed("graph factors must match route IDs and order");
            route.hs_speed_factor = route_factors[index].second;
        }
        setup.routes.push_back(route);
    }
    std::uint32_t fleets{};
    if (!count(fleets, galactic_max_objects, 24)) return malformed("bad fleet count");
    objects += fleets;
    if (objects > galactic_max_objects) {
        return fail<Out>(diagnostic_codes::replay_resource_limit, "too many galaxy objects", logical_path);
    }
    for (std::uint32_t index = 0; index < fleets; ++index) {
        Fleet fleet;
        std::uint32_t members{};
        if (!reader.read_u64(fleet.id) || !reader.read_u32(fleet.owner) || !reader.read_u64(fleet.orbit)
            || !count(members, galactic_max_objects, 16)) {
            return malformed("truncated fleet");
        }
        objects += members;
        if (objects > galactic_max_objects) {
            return fail<Out>(diagnostic_codes::replay_resource_limit, "too many galaxy objects", logical_path);
        }
        for (std::uint32_t member = 0; member < members; ++member) {
            FleetMember row;
            if (!reader.read_u64(row.id) || !reader.read_u64(row.type)) return malformed("truncated fleet member");
            fleet.members.push_back(row);
        }
        setup.fleets.push_back(std::move(fleet));
    }
    if (setup.fleet_control) {
        if (fleet_slots.size() != setup.fleets.size()) return malformed("fleet slots differ from fleet count");
        for (std::size_t i = 0; i < fleet_slots.size(); ++i) {
            if (fleet_slots[i].first != setup.fleets[i].id) return malformed("fleet slots must match IDs and order");
            setup.fleets[i].orbit_slot = fleet_slots[i].second;
        }
    }
    if (setup.movement) {
        if (fleet_motions.size() != setup.fleets.size()) return malformed("movement fleets differ from fleet count");
        for (std::size_t i = 0; i < fleet_motions.size(); ++i) {
            if (fleet_motions[i].first != setup.fleets[i].id) return malformed("movement fleets must match IDs and order");
            setup.fleets[i].motion = std::move(fleet_motions[i].second);
        }
    }
    return core::Result<void>::success();
}

core::Result<void> parse_replay_commands(sim::detail::Reader& reader, GalacticReplay& replay,
    const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    std::uint64_t commands{};
    if (!reader.read_u64(commands) || commands > galactic_max_commands || commands * 27 > reader.remaining()) {
        return malformed("bad command count");
    }
    for (std::uint64_t index = 0; index < commands; ++index) {
        CommandOrderKey key;
        std::uint8_t layer{};
        std::uint16_t opcode{};
        std::uint32_t length{};
        if (!reader.read_u64(key.tick) || !reader.read_u32(key.seat.value) || !reader.read_u64(key.sequence)
            || !reader.read_u8(layer) || !reader.read_u16(opcode) || !reader.read_u32(length)
            || length > galactic_replay_max_payload || length > reader.remaining()) {
            return malformed("truncated command " + std::to_string(index));
        }
        std::vector<std::uint8_t> body(length);
        if (!reader.read_bytes(body)) return malformed("truncated command " + std::to_string(index));
        if (layer < 1 || layer > 3) {
            return fail<Out>(diagnostic_codes::replay_command, key_text(key) + ": unknown layer " + std::to_string(layer), logical_path);
        }
        auto payload = decode_payload(static_cast<CommandLayer>(layer), opcode, body);
        if (!payload) {
            return fail<Out>(diagnostic_codes::replay_command, key_text(key) + ": layer " + std::to_string(layer) + " opcode "
                    + std::to_string(opcode) + " is not an implemented command or its payload is malformed", logical_path);
        }
        replay.commands.push_back({key, std::move(*payload)});
    }
    return core::Result<void>::success();
}

core::Result<void> parse_replay_preamble(sim::detail::Reader& reader, std::uint16_t& version, std::uint16_t& flags,
    std::uint32_t& rules, const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    if (!reader.read_u16(version) || !reader.read_u16(flags) || !reader.read_u32(rules)) return malformed("truncated header");
    if (version != galactic_replay_format_version) {
        return fail<Out>(diagnostic_codes::replay_version, "unsupported galactic replay version " + std::to_string(version), logical_path);
    }
    if (rules != galactic_rules_version) {
        return fail<Out>(diagnostic_codes::replay_version, "recorded under galactic rules version " + std::to_string(rules)
                + ", this build runs " + std::to_string(galactic_rules_version), logical_path);
    }
    if ((flags & ~(flag_debug_commands | flag_header_extensions)) != 0) return malformed("unknown header flags");
    return core::Result<void>::success();
}

core::Result<void> parse_replay_header(sim::detail::Reader& reader, GalacticReplay& replay,
    const std::uint16_t flags, const std::string_view logical_path) {
    using Out = void;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    auto& setup = replay.setup;
    setup.debug_commands = (flags & flag_debug_commands) != 0;
    if (!reader.read_bytes(setup.content_identity) || !reader.read_bytes(setup.campaign_identity)
        || !reader.read_u64(setup.seed) || !reader.read_u64(replay.final_tick_count)) {
        return malformed("truncated header");
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<GalacticReplay> parse_replay(const std::span<const std::uint8_t> bytes, const std::string_view logical_path) {
    using Out = GalacticReplay;
    const auto malformed = [&](const std::string& what) {
        return fail<Out>(diagnostic_codes::replay_malformed, what, logical_path);
    };
    if (bytes.size() > galactic_replay_max_bytes) {
        return fail<Out>(diagnostic_codes::replay_resource_limit, "the replay exceeds the size limit", logical_path);
    }
    sim::detail::Reader reader(bytes);
    std::array<std::uint8_t, 8> magic{};
    if (!reader.read_bytes(magic) || magic != replay_magic) return malformed("not a galactic replay (EAWRGCR)");
    std::uint16_t version{};
    std::uint16_t flags{};
    std::uint32_t rules{};
    if (auto parsed = parse_replay_preamble(reader, version, flags, rules, logical_path); !parsed)
        return core::Result<Out>::failure(parsed.error());
    Out replay;
    auto& setup = replay.setup;
    if (auto parsed = parse_replay_header(reader, replay, flags, logical_path); !parsed)
        return core::Result<Out>::failure(parsed.error());
    std::vector<std::pair<RouteId, math::Fixed>> route_factors;
    std::vector<std::pair<FleetId, std::uint8_t>> fleet_slots;
    std::vector<std::pair<FleetId, FleetMotion>> fleet_motions;
    if ((flags & flag_header_extensions) != 0) {
        if (auto parsed = parse_replay_extensions(reader, replay, route_factors, fleet_slots, fleet_motions, logical_path); !parsed)
            return core::Result<Out>::failure(parsed.error());
    }
    // Counts are bounded before anything is reserved, so a hostile count cannot allocate.
    const auto count = [&](std::uint32_t& value, const std::size_t limit, const std::size_t row_bytes) {
        return reader.read_u32(value) && value <= limit && std::size_t{value} * row_bytes <= reader.remaining();
    };
    if (auto parsed = parse_replay_participants(reader, setup, count, logical_path); !parsed)
        return core::Result<Out>::failure(parsed.error());
    if (auto parsed = parse_replay_objects(reader, setup, route_factors, fleet_slots, fleet_motions, count, logical_path); !parsed)
        return core::Result<Out>::failure(parsed.error());
    if (auto parsed = parse_replay_commands(reader, replay, logical_path); !parsed)
        return core::Result<Out>::failure(parsed.error());
    if (reader.remaining() != 0) return malformed("trailing bytes after the last command");
    if (setup.fiscal && setup.fiscal->contributions
        && setup.fiscal->contributions->routes.size() != setup.routes.size()) return malformed("fiscal contribution route count differs from galaxy");
    if (auto valid = validate_replay(replay); !valid) {
        auto diagnostic = valid.error();
        diagnostic.logical_path = std::string(logical_path);
        return core::Result<Out>::failure(std::move(diagnostic));
    }
    return core::Result<Out>::success(std::move(replay));
}

} // namespace eawr::sim::galactic
