#pragma once

// Frozen eager algorithm, retained only to compare map-list values and diagnostics
// against the metadata-gated implementation. No game assets are embedded here.
#include "eawr/skirmish/setup.hpp"
#include "eawr/data/ui/text_database.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <set>
#include <sstream>

namespace setup_map_reference {
using namespace eawr;
using namespace eawr::skirmish;
namespace {

inline std::optional<std::uint32_t> marker_team(std::string_view name, std::string_view suffix) {
    if (!name.starts_with("Team_") || !name.ends_with(suffix)) return std::nullopt;
    const auto number = name.substr(5, name.size() - 5 - suffix.size());
    std::uint32_t team{};
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), team);
    if (number.empty() || parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()
        || team >= sim::tactical::max_players) return std::nullopt;
    return team;
}

inline std::string fallback_name(const std::string& path) {
    const auto begin = path.rfind('/') + 1;
    return path.substr(begin, path.size() - begin - 4);
}

inline std::vector<std::string> tokens(std::string text) {
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream stream(text);
    std::vector<std::string> values;
    for (std::string value; stream >> value;) values.push_back(std::move(value));
    return values;
}

} // namespace

inline bool reference_eligible(const assets::Map& map, const SetupMapQuery& query) {
    const auto& metadata = map.lobby;
    const auto folded = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        });
        return value;
    };
    return map.kind == query.kind && metadata.capacity && *metadata.capacity >= query.minimum_capacity
        && metadata.levels && *metadata.levels >= query.minimum_levels
        && metadata.custom && (!query.custom || metadata.custom == query.custom)
        && (!query.owner || metadata.owner == query.owner)
        && (!query.terrain || metadata.terrain == query.terrain)
        && (!query.new_markers_only || metadata.new_markers == true)
        && (query.game_type.empty() || (metadata.game_types
            && folded(*metadata.game_types).find(folded(query.game_type)) != std::string::npos));
}

inline core::Result<std::vector<SetupMap>> eager_maps(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const SetupMapQuery& query = {}, std::vector<Fixture>* eager_fixtures = nullptr) {
    using Result = core::Result<std::vector<SetupMap>>;
    auto records = filesystem.enumerate("data/art/maps/", ".ted");
    if (!records) return Result::failure(records.error());
    std::vector<SetupMap> maps;
    const units::UnitTables tables;
    const auto types = assets::object_type_catalog(catalog);
    auto texts = data::ui::load_language_text_database(filesystem, "ENGLISH");
    for (const auto& record : records.value()) {
        auto map = assets::load_map(filesystem, record.canonical_path, types);
        // WSS-03 supplies the authored eligibility filters. Excluding a map that
        // cannot be decoded is the project's fail-closed policy, not a retail fallback.
        if (!map || !reference_eligible(map.value(), query)) continue;
        SetupMap entry;
        entry.path = record.canonical_path;
        entry.name = fallback_name(entry.path);
        entry.metadata = map.value().lobby;
        entry.context_name = map.value().context_name;
        {
            // WSS-06: capacity prefixes the authored display name (including an
            // untranslated key); an absent display name keeps the bare map stem.
            if (map.value().display_name_key && !map.value().display_name_key->empty()) {
                const auto* text = texts ? texts.value().find(*map.value().display_name_key) : nullptr;
                entry.name = text ? data::ui::to_utf8(text->value) : *map.value().display_name_key;
                entry.display_name = entry.name;
                entry.name = "(" + std::to_string(*entry.metadata.capacity) + ") " + entry.name;
            }
            const auto slash = entry.path.rfind('/');
            const std::string stem = entry.path.substr(slash + 1, entry.path.size() - slash - 5);
            for (const auto suffix : {".tga", ".dds"}) {
                const auto path = "data/art/textures/" + stem + suffix;
                if (filesystem.stat(path)) { entry.preview_path = path; break; }
            }
            if (entry.preview_path.empty()) {
                for (const auto& chunk : map.value().chunks) {
                    if (chunk.id == 19 && !chunk.group) { entry.embedded_preview = chunk.payload; break; }
                }
            }
        }
        FixtureOptions options;
        options.map = entry.path;
        auto fixture = fixture_from_options(options, filesystem, catalog);
        if (!fixture) entry.unavailable = fixture.error().message;
        else {
            if (eager_fixtures) eager_fixtures->push_back(fixture.value());
            auto inputs = read_start_inputs(fixture.value(), filesystem, catalog, tables);
            if (!inputs) entry.unavailable = inputs.error().message;
            else {
                std::map<std::uint32_t, std::uint32_t> spawns;
                for (const auto& placement : inputs.value().placements) {
                    if (!placement.marker || !placement.position || !placement.orientation_degrees
                        || placement.orientation_degrees->x != Fixed{} || placement.orientation_degrees->y != Fixed{}) continue;
                    if (const auto team = marker_team(placement.type, "_Spawn_Point_Marker")) ++spawns[*team];
                }
                // SK-11: the first marker in reverse record order serves the first player on a team.
                std::set<std::uint32_t> seen;
                for (auto p = inputs.value().placements.rbegin(); p != inputs.value().placements.rend(); ++p) {
                    const auto team = marker_team(p->type, "_Space_Station");
                    if (!p->marker || !team || !seen.insert(*team).second
                        || !p->position || !p->orientation_degrees) continue;
                    SetupTeam side{*team, {}, spawns[*team]};
                    for (const auto& candidate : p->marker_for) {
                        for (const auto& faction : tokens(candidate.affiliation)) {
                            if (faction != "Rebel" && faction != "Empire" && faction != "Underworld") continue;
                            if (std::find(side.factions.begin(), side.factions.end(), faction) == side.factions.end())
                                side.factions.push_back(faction);
                        }
                    }
                    if (!side.factions.empty() && entry.metadata.start_positions
                        && side.team < entry.metadata.start_positions->size()) entry.teams.push_back(std::move(side));
                }
                std::sort(entry.teams.begin(), entry.teams.end(), [](const auto& a, const auto& b) { return a.team < b.team; });
                if (entry.teams.size() < 2) entry.unavailable = "This map has fewer than two supported station teams.";
                if (entry.metadata.new_markers != true)
                    entry.unavailable = "Legacy map starts are not supported yet.";
                else if (!entry.metadata.start_positions || entry.metadata.start_positions->size() < 2)
                    entry.unavailable = "This map has missing or malformed authored start positions.";
            }
        }
        maps.push_back(std::move(entry));
    }
    std::sort(maps.begin(), maps.end(), [](const auto& a, const auto& b) {
        return a.name != b.name ? a.name < b.name : a.path < b.path;
    });
    return Result::success(std::move(maps));
}

}

namespace setup_map_reference {
// Length-prefix every variable field, preserve float bits, and encode absent
// optionals separately. This covers every SetupMap field, including preview bytes.
inline std::string bytes(const std::vector<SetupMap>& maps) {
    std::string result;
    const auto integer = [&](std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) result.push_back(static_cast<char>((value >> (i * 8U)) & 255U));
    };
    const auto text = [&](const std::string& value) { integer(value.size()); result += value; };
    const auto optional_integer = [&](const auto& value) { integer(value.has_value()); if (value) integer(*value); };
    const auto optional_text = [&](const auto& value) { integer(value.has_value()); if (value) text(*value); };
    integer(maps.size());
    for (const auto& map : maps) {
        text(map.path); text(map.name); text(map.unavailable); text(map.preview_path); text(map.display_name);
        optional_text(map.context_name);
        integer(map.embedded_preview.size());
        for (const auto value : map.embedded_preview) integer(std::to_integer<std::uint8_t>(value));
        const auto& metadata = map.metadata;
        optional_integer(metadata.capacity); optional_integer(metadata.levels);
        optional_integer(metadata.owner); optional_integer(metadata.terrain);
        optional_integer(metadata.custom); optional_integer(metadata.new_markers); optional_text(metadata.game_types);
        integer(metadata.start_positions.has_value());
        if (metadata.start_positions) {
            integer(metadata.start_positions->size());
            for (const auto& point : *metadata.start_positions) {
                integer(std::bit_cast<std::uint32_t>(point.x)); integer(std::bit_cast<std::uint32_t>(point.y));
            }
        }
        integer(map.teams.size());
        for (const auto& team : map.teams) {
            integer(team.team); optional_integer(team.spawn_capacity); integer(team.factions.size());
            for (const auto& faction : team.factions) text(faction);
        }
    }
    return result;
}
inline bool same_fixture(const Fixture& a, const Fixture& b) {
    if (a.map != b.map || a.map_sha256 != b.map_sha256 || a.slots.size() != b.slots.size()
        || a.pre_built_base != b.pre_built_base || a.free_starting_units != b.free_starting_units
        || a.seed != b.seed || a.victory_condition != b.victory_condition || a.match != b.match || a.placed != b.placed) return false;
    for (std::size_t i = 0; i < a.slots.size(); ++i) {
        const auto& x = a.slots[i]; const auto& y = b.slots[i];
        if (x.slot != y.slot || x.faction != y.faction || x.team != y.team || x.human != y.human
            || x.fleet != y.fleet || x.colour_index != y.colour_index) return false;
    }
    return true;
}
}
