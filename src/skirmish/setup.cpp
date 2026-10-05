#include "eawr/skirmish/setup.hpp"
#include "skirmish_internal.hpp"
#include "eawr/data/ui/text_database.hpp"

#include <algorithm>
#include <charconv>
#include <set>

namespace eawr::skirmish {
namespace {

std::optional<std::uint32_t> marker_team(std::string_view name, std::string_view suffix) {
    if (!name.starts_with("Team_") || !name.ends_with(suffix)) return std::nullopt;
    const auto number = name.substr(5, name.size() - 5 - suffix.size());
    std::uint32_t team{};
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), team);
    if (number.empty() || parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()
        || team >= sim::tactical::max_players) return std::nullopt;
    return team;
}

std::string fallback_name(const std::string& path) {
    const auto begin = path.rfind('/') + 1;
    return path.substr(begin, path.size() - begin - 4);
}

} // namespace

bool setup_map_eligible(const assets::Map& map, const SetupMapQuery& query) {
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

core::Result<std::vector<SetupMap>> setup_maps(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const SetupMapQuery& query) {
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
        if (!map || !setup_map_eligible(map.value(), query)) continue;
        SetupMap entry;
        entry.path = record.canonical_path;
        entry.name = fallback_name(entry.path);
        entry.metadata = map.value().lobby;
        {
            // WSS-06: capacity prefixes the authored display name (including an
            // untranslated key); an absent display name keeps the bare map stem.
            if (map.value().display_name_key && !map.value().display_name_key->empty()) {
                const auto* text = texts ? texts.value().find(*map.value().display_name_key) : nullptr;
                entry.name = text ? data::ui::to_utf8(text->value) : *map.value().display_name_key;
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
                    if (!p->marker || !team || !seen.insert(*team).second || !spawns.contains(*team)
                        || !p->position || !p->orientation_degrees) continue;
                    SetupTeam side{*team, {}, spawns[*team]};
                    for (const auto& candidate : p->marker_for) {
                        for (const auto& faction : detail::tokens(candidate.affiliation)) {
                            if (faction != "Rebel" && faction != "Empire" && faction != "Underworld") continue;
                            if (std::find(side.factions.begin(), side.factions.end(), faction) == side.factions.end())
                                side.factions.push_back(faction);
                        }
                    }
                    if (!side.factions.empty() && entry.metadata.start_positions
                        && side.team < entry.metadata.start_positions->size()) entry.teams.push_back(std::move(side));
                }
                std::sort(entry.teams.begin(), entry.teams.end(), [](const auto& a, const auto& b) { return a.team < b.team; });
                if (entry.teams.size() < 2) entry.unavailable = "This map has fewer than two supported station and spawn teams.";
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

std::uint32_t setup_row_count(const SetupMap& map) noexcept {
    return std::min(map.metadata.capacity.value_or(0), local_setup_rows);
}

void repair_setup_selection(SetupSelection& selection, const SetupMap& map) {
    selection.map = map.path;
    const auto rows = setup_row_count(map);
    if (selection.slots.size() > rows) selection.slots.resize(rows);
    std::set<std::uint32_t> occupied;
    for (auto& slot : selection.slots) {
        if (slot.slot == 0 || slot.slot > rows || !occupied.insert(slot.slot).second) slot.slot = 0;
        if (map.metadata.new_markers == true && map.metadata.start_positions && !map.metadata.start_positions->empty())
            slot.team = std::min(slot.team, static_cast<std::uint32_t>(map.metadata.start_positions->size() - 1));
    }
    for (auto& slot : selection.slots) {
        if (slot.slot != 0) continue;
        for (std::uint32_t row = 1; row <= rows; ++row) {
            if (occupied.insert(row).second) { slot.slot = row; break; }
        }
    }
    std::sort(selection.slots.begin(), selection.slots.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
}

core::Result<void> validate_setup_slots(const std::span<const LobbySlot> slots,
    const assets::MapLobbyMetadata& metadata, const bool local_controls) {
    using Result = core::Result<void>;
    const auto fail = [](std::string message) {
        return Result::failure(detail::error(diagnostic_codes::fixture, std::move(message)));
    };
    if (!metadata.capacity) return fail("This map has no valid authored player capacity.");
    const auto rows = std::min(*metadata.capacity, local_setup_rows);
    if (slots.size() < 2 || slots.size() > rows) return fail("Choose at least two players within this map's local capacity.");
    if (metadata.new_markers != true) return fail("Legacy map starts are not supported yet.");
    if (!metadata.start_positions || metadata.start_positions->size() < 2)
        return fail("This map has missing or malformed authored start positions.");
    if (local_controls && (slots.front().slot != 1 || !slots.front().human))
        return fail("The local host must occupy player row 1.");
    std::map<std::uint32_t, std::string> factions;
    std::uint32_t previous{};
    for (const auto& slot : slots) {
        if (slot.slot <= previous || slot.slot > (local_controls ? rows : sim::tactical::max_players))
            return fail("Occupied player rows must be distinct, ordered and within range.");
        previous = slot.slot;
        if (local_controls && slot.slot != 1 && slot.human) return fail("Other local rows support AI or Open only.");
        if (slot.team >= metadata.start_positions->size()) return fail("The selected team has no authored start on this map.");
        const auto [team, inserted] = factions.emplace(slot.team, slot.faction);
        if (!inserted && !detail::iequals(team->second, slot.faction))
            return fail("Players sharing a team must choose the same faction.");
    }
    if (factions.size() < 2) return fail("Choose at least two different teams.");
    return Result::success();
}

core::Result<void> select_setup_colour(SetupSelection& selection, const std::uint32_t slot,
    const std::uint32_t colour, const std::size_t palette_size) {
    using Result = core::Result<void>;
    const auto fail = [](std::string message) {
        return Result::failure(detail::error(diagnostic_codes::fixture, std::move(message)));
    };
    const auto edited = std::find_if(selection.slots.begin(), selection.slots.end(),
        [&](const auto& record) { return record.slot == slot; });
    if (edited == selection.slots.end()) return fail("An Open row has no selected colour.");
    if (colour >= palette_size) return fail("The selected multiplayer colour is outside the authored palette.");
    std::set<std::uint32_t> used;
    for (const auto& record : selection.slots) {
        if (record.slot != slot) used.insert(record.colour_index.value_or(record.slot - 1));
    }
    auto chosen = colour;
    if (used.contains(chosen)) {
        chosen = 0;
        while (chosen < palette_size && used.contains(chosen)) ++chosen;
        if (chosen == palette_size) return fail("There is no unused multiplayer colour for this row.");
    }
    edited->colour_index = chosen;
    return Result::success();
}

core::Result<FixtureOptions> setup_options(const SetupSelection& selection, std::span<const SetupMap> maps) {
    using Result = core::Result<FixtureOptions>;
    const auto fail = [](std::string message) {
        return Result::failure(detail::error(diagnostic_codes::fixture, std::move(message)));
    };
    const auto map = std::find_if(maps.begin(), maps.end(), [&](const auto& entry) { return entry.path == selection.map; });
    if (map == maps.end()) return fail("Select an installed space skirmish map.");
    if (!map->unavailable.empty()) return fail(map->unavailable);
    if (auto valid = validate_setup_slots(selection.slots, map->metadata); !valid) return Result::failure(valid.error());
    std::set<std::uint32_t> colours;
    std::map<std::uint32_t, std::uint32_t> teammates;
    for (const auto& slot : selection.slots) {
        const auto colour = slot.colour_index.value_or(slot.slot - 1);
        if (colour >= multiplayer_colour_count || !colours.insert(colour).second)
            return fail("Choose distinct colours from the authored multiplayer palette.");
        const auto team = std::find_if(map->teams.begin(), map->teams.end(), [&](const auto& entry) { return entry.team == slot.team; });
        if (team == map->teams.end()) return fail("The selected team has no start on this map.");
        if (team->spawn_capacity && ++teammates[slot.team] > *team->spawn_capacity)
            return fail("Team " + std::to_string(slot.team + 1) + " has too few spawn markers for the selected players.");
        if (std::find(team->factions.begin(), team->factions.end(), slot.faction) == team->factions.end())
            return fail("This map has no starting station for " + slot.faction + " on the selected team.");
        if (!slot.fleet.empty()) return fail("Starting fleets are not configurable in this screen.");
    }
    FixtureOptions options;
    options.map = selection.map;
    options.slots = std::vector<LobbySlot>(selection.slots.begin(), selection.slots.end());
    options.match = selection.match;
    return Result::success(std::move(options));
}

SetupOptionsEdit begin_setup_options(const SetupSelection& selection, const MatchOptions& defaults) {
    return {selection.match.value_or(defaults), selection.custom_options};
}

void reset_setup_options(SetupOptionsEdit& edit, const MatchOptions& defaults) {
    edit = {defaults, false};
}

void accept_setup_options(SetupSelection& selection, const SetupOptionsEdit& edit) {
    selection.match = edit.value;
    selection.custom_options = edit.custom;
}

} // namespace eawr::skirmish
