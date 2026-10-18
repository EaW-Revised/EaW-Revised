#include "replay_parse_internal.hpp"

namespace eawr::sim::tactical::replay_parse_detail {
using namespace replay_detail;

namespace {
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

core::Result<void> parse_players(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t player_count) {
    using Parsed = void;
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
    return core::Result<void>::success();
}

core::Result<void> parse_units(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint64_t unit_count,
    std::vector<EntityId>& disabled_garrisons,
    std::vector<UnitState>& carriers) {
    using Parsed = void;
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
    auto disabled = disabled_garrisons.begin();
    for (auto& carrier : carriers) {
        const auto unit = std::find_if(replay.setup.units.begin(), replay.setup.units.end(),
            [&](const auto& value) { return value.entity_id == carrier.entity_id; });
        if (unit == replay.setup.units.end())
            return fail<Parsed>(diagnostic_codes::malformed, "carrier references an absent initial entity", logical_path);
        unit->purchase_type = carrier.purchase_type;
        unit->contained = std::move(carrier.contained);
    }
    for (auto& unit : replay.setup.units) {
        if (disabled != disabled_garrisons.end() && unit.entity_id == *disabled) {
            unit.garrison_enabled = false;
            ++disabled;
        }
    }
    if (disabled != disabled_garrisons.end()) {
        return fail<Parsed>(diagnostic_codes::malformed, "GSPN references an absent initial entity", logical_path);
    }
    return core::Result<void>::success();
}

core::Result<void> parse_squadrons(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t header_reserved,
    const bool squadrons) {
    using Parsed = void;
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
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical::replay_parse_detail
