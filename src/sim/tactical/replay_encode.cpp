#include "replay_internal.hpp"

namespace eawr::sim::tactical {
using namespace replay_detail;

namespace {
[[nodiscard]] std::vector<std::uint8_t> skirmish_body(const ReplaySkirmishSetup& setup) {
    std::vector<std::uint8_t> bytes;
    const auto text = [&](const std::string& value) {
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    };
    text(setup.map);
    text(setup.map_sha256);
    const auto& match = setup.match;
    const SkirmishMatchPolicy policy{match.allow_heroes, match.allow_superweapons,
        match.free_starting_units, match.pre_built_base};
    sim::detail::append_u32(bytes, policy.disabled_flags() | (match.allow_random_events ? 16U : 0U));
    sim::detail::append_i64(bytes, match.credits.raw());
    for (const auto value : {match.start_tech, match.max_tech, match.game_timer, match.win_integer, match.auto_resolve})
        sim::detail::append_i64(bytes, value);
    sim::detail::append_i64(bytes, match.win_float.raw());
    text(match.win_condition);
    text(match.space_win_condition);
    sim::detail::append_u32(bytes, setup.victory_condition);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(setup.slots.size()));
    for (const auto& slot : setup.slots) {
        sim::detail::append_u32(bytes, slot.player);
        sim::detail::append_u32(bytes, slot.human ? 1U : 0U);
        sim::detail::append_u32(bytes, slot.colour_index.value_or(std::numeric_limits<std::uint32_t>::max()));
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(slot.fleet.size()));
        for (const auto& type : slot.fleet) text(type);
    }
    return bytes;
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

core::Result<std::vector<std::uint8_t>> write_replay(const TacticalReplay& replay) {
    using Bytes = std::vector<std::uint8_t>;
    const auto validated = validate_replay(replay);
    if (!validated) {
        return core::Result<Bytes>::failure(validated.error());
    }
    const auto metadata = replay.setup.skirmish ? skirmish_body(*replay.setup.skirmish) : std::vector<std::uint8_t>{};
    const bool extensions = replay.setup.match_policy.has_value() || replay.setup.skirmish.has_value();
    const auto header_size = replay_header_size + (extensions ? 4U : 0U)
        + (replay.setup.match_policy ? 8U : 0U) + (replay.setup.skirmish ? 4U + metadata.size() : 0U);
    std::size_t size = header_size + replay.setup.players.size() * detail::player_record_size
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
    const auto format = extensions
        ? (squadrons.empty() ? replay_format_version_extensions : replay_format_version_squadron_extensions)
        : (squadrons.empty() ? replay_format_version : replay_format_version_squadrons);
    sim::detail::append_u16(bytes, format);
    sim::detail::append_u16(bytes, static_cast<std::uint16_t>(header_size));
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
    if (extensions) {
        sim::detail::append_u32(bytes, (replay.setup.match_policy ? 1U : 0U) + (replay.setup.skirmish ? 1U : 0U));
    }
    if (replay.setup.match_policy) {
        sim::detail::append_u16(bytes, replay_extension_match_policy);
        sim::detail::append_u16(bytes, 4); // body length, excluding tag/length
        sim::detail::append_u32(bytes, replay.setup.match_policy->disabled_flags());
    }
    if (replay.setup.skirmish) {
        sim::detail::append_u16(bytes, replay_extension_skirmish_setup);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(metadata.size()));
        bytes.insert(bytes.end(), metadata.begin(), metadata.end());
    }
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
