#include "galactic_report.hpp"

#include "eawr/core/sha256.hpp"

#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace xml_scan {
namespace {

namespace galactic = eawr::data::galactic;
using galactic::Fixed;

std::string text(const std::string_view value) {
    std::ostringstream out;
    out << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20U) out << "\\u00" << hex[ch >> 4U] << hex[ch & 15U];
            else out << static_cast<char>(ch);
        }
    }
    out << '"';
    return out.str();
}

// A Q24 value as a display decimal string (six fractional digits, truncated toward zero).
std::string decimal(const Fixed value) {
    const std::int64_t raw = value.raw();
    const bool negative = raw < 0;
    const std::uint64_t magnitude = negative ? static_cast<std::uint64_t>(-(raw + 1)) + 1 : static_cast<std::uint64_t>(raw);
    const std::uint64_t scale = static_cast<std::uint64_t>(Fixed::scale);
    const std::uint64_t fraction = (magnitude % scale) * 1000000U / scale;
    std::ostringstream out;
    if (negative) out << '-';
    out << magnitude / scale << '.';
    const auto digits = std::to_string(fraction);
    out << std::string(6 - digits.size(), '0') << digits;
    return out.str();
}

std::string number(const std::optional<Fixed>& value) { return value ? text(decimal(*value)) : "null"; }
std::string whole(const std::optional<std::int64_t>& value) { return value ? std::to_string(*value) : "null"; }
std::string flag(const std::optional<bool>& value) { return !value ? "null" : (*value ? "true" : "false"); }

std::string names(const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) out += ',';
        out += text(values[index]);
    }
    return out + ']';
}

std::string faction_numbers(const std::vector<galactic::FactionNumber>& values) {
    std::string out = "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) out += ',';
        out += "{\"faction\":" + text(values[index].faction) + ",\"value\":" + std::to_string(values[index].value) + '}';
    }
    return out + ']';
}

std::string faction_names(const std::vector<galactic::FactionName>& values) {
    std::string out = "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) out += ',';
        out += "{\"faction\":" + text(values[index].faction) + ",\"name\":" + text(values[index].name) + '}';
    }
    return out + ']';
}

// The factions a campaign names in its records, first appearance first.
std::vector<std::string> campaign_factions(const galactic::Campaign& campaign) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    const auto add = [&](const std::string& name) {
        std::string key;
        for (const char ch : name) key.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch);
        if (seen.insert(key).second) result.push_back(name);
    };
    for (const auto& value : campaign.starting_credits) add(value.faction);
    for (const auto& value : campaign.home_locations) add(value.faction);
    for (const auto& value : campaign.starting_forces) add(value.faction);
    return result;
}

} // namespace

bool write_galactic_report(const std::filesystem::path& path, const galactic::GalacticData& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const auto identity = galactic::content_identity(data);
    std::string identity_hex;
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : identity) {
        identity_hex.push_back(hex[byte >> 4U]);
        identity_hex.push_back(hex[byte & 15U]);
    }
    std::map<std::string, std::size_t> by_kind;
    for (const auto& issue : data.issues) ++by_kind[std::string(galactic::to_string(issue.kind))];

    out << "{\n  \"schema\":\"eawr-galactic-report-v1\",\n  \"content_identity\":" << text(identity_hex)
        << ",\n  \"counts\":{\"campaigns\":" << data.campaigns.size() << ",\"planets\":" << data.planets.size()
        << ",\"trade_routes\":" << data.trade_routes.size() << ",\"factions\":" << data.factions.size()
        << ",\"constants\":" << data.constants.size() << ",\"issues\":" << data.issues.size() << ",\"issues_by_kind\":{";
    bool first = true;
    for (const auto& [kind, count] : by_kind) {
        if (!first) out << ',';
        first = false;
        out << text(kind) << ':' << count;
    }
    out << "}},\n  \"campaigns\":[";
    for (std::size_t index = 0; index < data.campaigns.size(); ++index) {
        const auto& campaign = data.campaigns[index];
        out << (index ? ",\n    " : "\n    ") << "{\"name\":" << text(campaign.name) << ",\"file\":" << text(campaign.file)
            << ",\"campaign_set\":" << text(campaign.campaign_set) << ",\"sort_order\":" << whole(campaign.sort_order)
            << ",\"is_multiplayer\":" << flag(campaign.is_multiplayer) << ",\"is_story_campaign\":"
            << flag(campaign.is_story_campaign) << ",\"tutorial\":" << flag(campaign.tutorial)
            << ",\"supports_custom_settings\":" << flag(campaign.supports_custom_settings)
            << ",\"factions\":" << names(campaign_factions(campaign)) << ",\"locations\":" << names(campaign.locations)
            << ",\"planets\":" << names(campaign.planets) << ",\"trade_routes\":" << names(campaign.trade_routes)
            << ",\"home_locations\":" << faction_names(campaign.home_locations)
            << ",\"starting_active_player\":" << text(campaign.starting_active_player)
            << ",\"starting_credits\":" << faction_numbers(campaign.starting_credits)
            << ",\"starting_tech_levels\":" << faction_numbers(campaign.starting_tech_levels)
            << ",\"max_tech_levels\":" << faction_numbers(campaign.max_tech_levels)
            << ",\"starting_force_records\":" << campaign.starting_forces.size()
            << ",\"special_case_production_records\":" << campaign.special_case_production.size() << '}';
    }
    out << "],\n  \"planets\":[";
    for (std::size_t index = 0; index < data.planets.size(); ++index) {
        const auto& planet = data.planets[index];
        out << (index ? ",\n    " : "\n    ") << "{\"name\":" << text(planet.name) << ",\"galactic_position\":";
        if (planet.galactic_position) {
            out << '[' << text(decimal(planet.galactic_position->x)) << ',' << text(decimal(planet.galactic_position->y))
                << ',' << text(decimal(planet.galactic_position->z)) << ']';
        } else {
            out << "null";
        }
        out << ",\"political_control\":" << number(planet.political_control) << ",\"planet_credit_value\":"
            << number(planet.planet_credit_value) << ",\"max_space_base\":" << whole(planet.max_space_base)
            << ",\"planet_surface_accessible\":" << flag(planet.planet_surface_accessible)
            << ",\"space_tactical_map\":" << text(planet.space_tactical_map) << '}';
    }
    out << "],\n  \"trade_routes\":[";
    for (std::size_t index = 0; index < data.trade_routes.size(); ++index) {
        const auto& route = data.trade_routes[index];
        out << (index ? "," : "") << "\n    {\"name\":" << text(route.name) << ",\"point_a\":" << text(route.point_a)
            << ",\"point_b\":" << text(route.point_b) << ",\"hs_speed_factor\":" << number(route.hs_speed_factor) << '}';
    }
    out << "],\n  \"factions\":[";
    for (std::size_t index = 0; index < data.factions.size(); ++index) {
        const auto& faction = data.factions[index];
        out << (index ? "," : "") << "\n    {\"name\":" << text(faction.name) << ",\"is_playable\":"
            << flag(faction.is_playable) << ",\"is_neutral\":" << flag(faction.is_neutral) << ",\"primary_enemy\":"
            << text(faction.primary_enemy) << ",\"hyperspace_speed_factor\":" << number(faction.hyperspace_speed_factor)
            << ",\"displayed_tech_level_adjustment\":" << whole(faction.displayed_tech_level_adjustment) << '}';
    }
    out << "],\n  \"constants\":[";
    for (std::size_t index = 0; index < data.constants.size(); ++index) {
        const auto& constant = data.constants[index];
        out << (index ? "," : "") << "\n    {\"tag\":" << text(constant.tag) << ",\"text\":" << text(constant.text)
            << ",\"number\":" << number(constant.number) << '}';
    }
    out << "],\n  \"issues\":[";
    for (std::size_t index = 0; index < data.issues.size(); ++index) {
        const auto& issue = data.issues[index];
        out << (index ? "," : "") << "\n    {\"kind\":" << text(galactic::to_string(issue.kind)) << ",\"file\":"
            << text(issue.file) << ",\"line\":" << issue.line << ",\"owner\":" << text(issue.owner) << ",\"tag\":"
            << text(issue.tag) << ",\"value\":" << text(issue.value) << ",\"reason\":" << text(issue.reason) << '}';
    }
    out << "],\n  \"inputs\":[";
    for (std::size_t index = 0; index < data.inputs.size(); ++index) {
        const auto& input = data.inputs[index];
        out << (index ? "," : "") << "\n    {\"logical_path\":" << text(input.logical_path) << ",\"source_id\":"
            << text(input.source_id) << ",\"sha256\":" << text(input.sha256) << '}';
    }
    out << "]\n}\n";
    return static_cast<bool>(out);
}

} // namespace xml_scan
