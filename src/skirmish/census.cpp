#include "eawr/skirmish/start.hpp"

#include "eawr/sim/tactical/session.hpp"

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace eawr::skirmish {
namespace {

// Q24 as a decimal rounded to six places (the raw value is listed beside it).
[[nodiscard]] std::string decimal(const Fixed value) {
    const std::int64_t raw = value.raw();
    const bool negative = raw < 0;
    const std::uint64_t magnitude =
        negative ? static_cast<std::uint64_t>(-(raw + 1)) + 1U : static_cast<std::uint64_t>(raw);
    std::uint64_t whole = magnitude >> 24U;
    std::uint64_t fraction = ((magnitude & 0xffffffU) * 1'000'000U + 0x800000U) >> 24U;
    if (fraction == 1'000'000U) {
        ++whole;
        fraction = 0;
    }
    std::string text = std::to_string(fraction);
    text = std::string(6 - text.size(), '0') + text;
    while (!text.empty() && text.back() == '0') text.pop_back();
    const bool zero = whole == 0 && text.empty();
    return (negative && !zero ? "-" : "") + std::to_string(whole) + (text.empty() ? "" : "." + text);
}

[[nodiscard]] std::string quoted(const std::string_view value) {
    std::string output = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            output.push_back('\\');
            output.push_back(static_cast<char>(character));
        } else if (character < 0x20U || character >= 0x7fU) {
            output += "\\u00";
            output.push_back(hex[character >> 4U]);
            output.push_back(hex[character & 15U]);
        } else {
            output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
    return output;
}

[[nodiscard]] std::string hex(const std::array<std::uint8_t, 32>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 15U]);
    }
    return text;
}

[[nodiscard]] std::string flag(const bool value) { return value ? "true" : "false"; }

[[nodiscard]] std::string vec3(const Vec3& value) {
    return "[" + decimal(value.x) + ", " + decimal(value.y) + ", " + decimal(value.z) + "]";
}

void player_row(std::ostringstream& json, const sim::tactical::Player& player, const StartPlayer* named) {
    json << "{\"player_id\": " << player.player_id << ", \"team\": " << player.team_id
         << ", \"faction_id\": " << player.faction_id << ", \"commandable\": " << flag(player.commandable());
    if (named != nullptr) {
        json << ", \"faction\": " << quoted(named->faction) << ", \"lobby\": " << flag(named->lobby);
        if (named->lobby) {
            json << ", \"human\": " << flag(named->human) << ", \"start_side\": " << quoted(named->start_side);
        }
        if (named->colour) {
            const auto& rgb = named->colour->rgb;
            json << ", \"colour\": {\"constant\": " << quoted(named->colour->constant) << ", \"rgb\": ["
                 << static_cast<int>(rgb[0]) << ", " << static_cast<int>(rgb[1]) << ", " << static_cast<int>(rgb[2]) << "]}";
        }
        if (named->owner_index) json << ", \"ted_owner_index\": " << *named->owner_index;
        if (named->lobby) {
            const auto all = sim::math::add(named->combat_power_tick_zero, named->combat_power_launches);
            json << ", \"credits\": " << named->credits << ", \"income\": " << flag(named->income)
                 << ", \"production_queue\": " << flag(named->production_queue)
                 << ", \"population_cap\": " << flag(named->population_cap)
                 << ", \"ai_combat_power\": {\"tick_zero\": " << decimal(named->combat_power_tick_zero)
                 << ", \"launches\": " << decimal(named->combat_power_launches)
                 << ", \"all_launched\": " << (all ? decimal(all.value()) : std::string("null")) << "}";
        }
    }
    json << "}";
}

void unit_row(std::ostringstream& json, const sim::tactical::UnitState& unit, const StartUnit* named) {
    json << "{\"entity_id\": " << unit.entity_id << ", \"type_id\": " << unit.type_id << ", \"owner\": " << unit.owner
         << ", \"position_raw\": [" << unit.position.x.raw() << ", " << unit.position.y.raw() << ", "
         << unit.position.z.raw() << "], \"rotation_raw\": [" << unit.rotation.x.raw() << ", " << unit.rotation.y.raw()
         << ", " << unit.rotation.z.raw() << ", " << unit.rotation.w.raw() << "]";
    if (named != nullptr) {
        json << ", \"type\": " << quoted(named->type) << ", \"role\": " << quoted(to_string(named->role))
             << ", \"record\": " << named->record << ", \"position\": " << vec3(unit.position)
             << ", \"yaw_degrees\": " << decimal(named->yaw_degrees);
        if (named->dropped_orientation_degrees) {
            json << ", \"dropped_orientation_degrees\": " << vec3(*named->dropped_orientation_degrees);
        }
        json << ", \"ai_combat_power\": "
             << (named->combat_power ? decimal(*named->combat_power) : std::string("null"))
             << ", \"reveal_range\": " << (named->reveal_range ? decimal(*named->reveal_range) : std::string("null"));
        if (named->craft != 0) json << ", \"craft\": " << named->craft << ", \"craft_type\": " << quoted(named->craft_type);
        json << ", \"hull\": " << (named->hull ? decimal(*named->hull) : std::string("null"))
             << ", \"victory_relevant\": " << flag(named->victory_relevant);
    }
    json << "}";
}

} // namespace

core::Result<std::string> census_json(const sim::tactical::TacticalSetup& setup, const SkirmishStart* start,
    const std::span<const sim::tactical::SensorProfile> sensors) {
    using Result = core::Result<std::string>;
    if (start != nullptr && (start->players.size() != setup.players.size() || start->units.size() != setup.units.size())) {
        return Result::failure(core::Diagnostic{.code = std::string(diagnostic_codes::input),
            .severity = core::Severity::error, .message = "census: the start does not match the setup",
            .logical_path = std::nullopt, .line = std::nullopt, .column = std::nullopt,
            .source_id = std::string("skirmish")});
    }
    auto session = sim::tactical::TacticalSession::create(setup, sensors);
    if (!session) return Result::failure(session.error());

    std::ostringstream json;
    json << "{\n  \"format\": \"eawr-skirmish-census\",\n  \"format_version\": 1,\n"
         << "  \"source\": " << (start != nullptr ? "\"fixture\"" : "\"replay\"") << ",\n";
    if (start != nullptr) {
        json << "  \"map\": {\"logical_path\": " << quoted(start->map) << ", \"sha256\": " << quoted(start->map_sha256)
             << "},\n";
    }
    json << "  \"tactical_rules_version\": " << sim::tactical::tactical_rules_version << ",\n"
         << "  \"seed\": " << setup.seed << ",\n"
         << "  \"content_identity\": " << quoted(hex(setup.content_identity)) << ",\n"
         << "  \"tick_zero\": {\"state_sha256\": " << quoted(session.value().state_sha256())
         << ", \"snapshot_sha256\": " << quoted(session.value().snapshot()->sha256())
         << ", \"next_entity_id\": " << session.value().next_entity_id()
         << ", \"sensor_profiles\": " << sensors.size() << "},\n";

    json << "  \"players\": [";
    for (std::size_t index = 0; index < setup.players.size(); ++index) {
        json << (index == 0 ? "\n    " : ",\n    ");
        player_row(json, setup.players[index], start != nullptr ? &start->players[index] : nullptr);
    }
    json << (setup.players.empty() ? "],\n" : "\n  ],\n");

    json << "  \"units\": [";
    for (std::size_t index = 0; index < setup.units.size(); ++index) {
        json << (index == 0 ? "\n    " : ",\n    ");
        unit_row(json, setup.units[index], start != nullptr ? &start->units[index] : nullptr);
    }
    json << (setup.units.empty() ? "]" : "\n  ]");

    if (start != nullptr) {
        json << ",\n  \"markers\": [";
        for (std::size_t index = 0; index < start->markers.size(); ++index) {
            const auto& marker = start->markers[index];
            json << (index == 0 ? "\n    " : ",\n    ") << "{\"record\": " << marker.record
                 << ", \"type\": " << quoted(marker.type) << ", \"use\": " << quoted(to_string(marker.use))
                 << ", \"player\": " << marker.player << ", \"position\": " << vec3(marker.position)
                 << ", \"yaw_degrees\": " << decimal(marker.yaw_degrees) << "}";
        }
        json << (start->markers.empty() ? "]" : "\n  ]");
        // Map objects the retail ownership pass deletes at the start.
        json << ",\n  \"removed_map_objects\": [";
        for (std::size_t index = 0; index < start->removed.size(); ++index) {
            const auto& removed = start->removed[index];
            json << (index == 0 ? "\n    " : ",\n    ") << "{\"record\": " << removed.record
                 << ", \"type\": " << quoted(removed.type) << ", \"faction\": " << quoted(removed.faction)
                 << ", \"reason\": " << quoted(to_string(removed.reason)) << "}";
        }
        json << (start->removed.empty() ? "]" : "\n  ]");
        // SK-23: launch data for #75; tick zero does not simulate it.
        json << ",\n  \"launches\": [";
        for (std::size_t index = 0; index < start->launches.size(); ++index) {
            const auto& launch = start->launches[index];
            json << (index == 0 ? "\n    " : ",\n    ") << "{\"spawner\": " << launch.spawner
                 << ", \"spawner_type\": " << quoted(launch.spawner_type) << ", \"owner\": " << launch.owner
                 << ", \"squadron\": " << quoted(launch.squadron) << ", \"count\": " << launch.count
                 << ", \"delay_seconds\": "
                 << (launch.delay_seconds ? decimal(*launch.delay_seconds) : std::string("null"))
                 << ", \"ai_combat_power\": " << decimal(launch.combat_power) << ", \"simulated\": false}";
        }
        json << (start->launches.empty() ? "]" : "\n  ]");
    }
    json << "\n}\n";
    return Result::success(json.str());
}

} // namespace eawr::skirmish
