#include "replay_parse_internal.hpp"

namespace eawr::sim::tactical {
using namespace replay_detail;
using namespace replay_parse_detail;

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
    std::uint32_t header_size{};
    std::uint32_t header_reserved{}, player_count{};
    std::uint64_t unit_count{}, command_count{};
    bool extensions{}, squadrons{};
    TacticalReplay replay;
    std::vector<EntityId> disabled_garrisons;
    std::vector<UnitState> carriers;
    std::uint32_t object_turn_count{};
    if (auto section = parse_fixed_header(bytes, reader, logical_path, replay, header_size, header_reserved, player_count, unit_count, command_count, squadrons, extensions); !section) {
        return core::Result<TacticalReplay>::failure(section.error());
    }
    if (auto section = parse_header_extensions(bytes, reader, logical_path, replay, header_size, extensions, disabled_garrisons, carriers, object_turn_count); !section) {
        return core::Result<TacticalReplay>::failure(section.error());
    }
    const auto format = peek_replay_format_version(bytes);
    const bool wide_header = format == replay_format_version_native || format == replay_format_version_squadron_native;
    if (wide_header != replay.setup.native_scene.has_value())
        return fail<Parsed>(diagnostic_codes::malformed, "native replay format requires native scene metadata", logical_path);
    if (auto section = validate_header_counts(logical_path, replay, header_reserved, player_count, unit_count, command_count, squadrons); !section) {
        return core::Result<TacticalReplay>::failure(section.error());
    }
    if (auto section = parse_players(reader, logical_path, replay, player_count); !section) {
        return core::Result<TacticalReplay>::failure(section.error());
    }
    if (auto section = parse_units(reader, logical_path, replay, unit_count, disabled_garrisons, carriers); !section) {
        return core::Result<TacticalReplay>::failure(section.error());
    }
    if (auto section = parse_squadrons(reader, logical_path, replay, header_reserved, squadrons); !section) {
        return core::Result<TacticalReplay>::failure(section.error());
    }
    if (auto section = parse_commands(reader, logical_path, replay, command_count); !section) {
        return core::Result<TacticalReplay>::failure(section.error());
    }
    if (auto section = parse_object_turns(reader, logical_path, replay, object_turn_count); !section) {
        return core::Result<TacticalReplay>::failure(section.error());
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
