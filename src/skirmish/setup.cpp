#include "eawr/skirmish/setup.hpp"
#include "skirmish_internal.hpp"
#include "setup_workers.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/sim/seat.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/scene/scene.hpp"

#include <algorithm>
#include <charconv>
#include <thread>
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

std::vector<SetupFaction> setup_factions(const data::Catalog& catalog) {
    std::vector<SetupFaction> result;
    for (const std::string& name : scene::faction_order(catalog)) {
        const auto resolved = catalog.resolve(name, data::Category::faction);
        if (!resolved) continue;
        const auto* playable = resolved.value().value("Is_Playable");
        if (!playable || !detail::retail_flag(playable->value.raw_text, false)) continue;
        SetupFaction faction;
        faction.name = name;
        if (const auto* id = resolved.value().value("Text_ID")) faction.text_id = detail::trim(id->value.raw_text);
        result.push_back(std::move(faction));
    }
    return result;
}

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
    const SetupMapQuery& query, const unsigned requested_workers) {
    using Result = core::Result<std::vector<SetupMap>>;
    auto records = filesystem.enumerate("data/art/maps/", ".ted");
    if (!records) return Result::failure(records.error());
    auto custom_records = filesystem.enumerate("data/custommaps/", ".ted");
    if (!custom_records) return Result::failure(custom_records.error());
    // WSS-07: discovery does not classify a map; its authored header does.
    // Both directories use the VFS's effective loose/archive winners.
    records.value().insert(records.value().end(), custom_records.value().begin(), custom_records.value().end());
    std::vector<SetupMap> maps;
    const auto types = assets::object_type_catalog(catalog);
    const auto playable_factions = setup_factions(catalog);
    auto texts = data::ui::load_language_text_database(filesystem, "ENGLISH");
    const auto checks = detail::inspect_setup_content(filesystem, catalog);
    const auto build_entry = [&](const vfs::AssetRecord& record, const assets::Map& map,
        const std::span<const std::byte> bytes) {
        SetupMap entry;
        entry.path = record.canonical_path;
        entry.name = fallback_name(entry.path);
        entry.metadata = map.lobby;
        entry.context_name = map.context_name;
        {
            // WSS-06: capacity prefixes the authored display name (including an
            // untranslated key); an absent display name keeps the bare map stem.
            if (map.display_name_key && !map.display_name_key->empty()) {
                const auto* text = texts ? texts.value().find(*map.display_name_key) : nullptr;
                entry.name = text ? data::ui::to_utf8(text->value) : *map.display_name_key;
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
                for (const auto& chunk : map.chunks) {
                    if (chunk.id == 19 && !chunk.group) { entry.embedded_preview = chunk.payload; break; }
                }
            }
        }
        auto placements = detail::inspect_setup_placements(map, bytes, entry.path, catalog, checks);
        if (!placements) entry.unavailable = placements.error().message;
        else {
            std::map<std::uint32_t, std::uint32_t> spawns;
            for (const auto& placement : placements.value()) {
                // A tilted marker still serves its team (SK-20, start.cpp pose).
                if (!placement.marker || !placement.position || !placement.orientation_degrees) continue;
                if (const auto team = marker_team(placement.type, "_Spawn_Point_Marker")) ++spawns[*team];
            }
            // SK-11: the first marker in reverse record order serves the first player on a team.
            std::set<std::uint32_t> seen;
            for (auto p = placements.value().rbegin(); p != placements.value().rend(); ++p) {
                const auto team = marker_team(p->type, "_Space_Station");
                if (!p->marker || !team || !seen.insert(*team).second
                    || !p->position || !p->orientation_degrees) continue;
                SetupTeam side{*team, {}, spawns[*team]};
                for (const auto& candidate : p->marker_for) {
                    for (const auto& faction : detail::tokens(candidate.affiliation)) {
                        // Both setup and LAN validate against this playable-only, canonical list.
                        // Station support still never limits the dropdown itself (WSS-12/13).
                        const auto playable = std::find_if(playable_factions.begin(), playable_factions.end(),
                            [&](const auto& playable_entry) { return detail::iequals(playable_entry.name, faction); });
                        if (playable != playable_factions.end()
                            && std::find(side.factions.begin(), side.factions.end(), playable->name) == side.factions.end())
                            side.factions.push_back(playable->name);
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
        return entry;
    };
    // Retain only picker records. Full decoded maps and source bytes live on
    // their worker, bounding memory by concurrency even for large mod corpora.
    std::vector<std::optional<SetupMap>> candidates(records.value().size());
    const auto errors = [&] {
        // The load-profile map line reports elapsed parallel-batch time on the
        // recording's owning thread, rather than summing worker CPU times.
        core::load_profile::Scope map_scope(core::load_profile::Phase::map);
        return detail::run_setup_map_workers(candidates.size(), requested_workers, [&](const std::size_t index) {
            const auto& record = records.value()[index];
            // Keep load_map's allocation guard before opening a TED. Metadata
            // and body are read from the same effective winner and same bytes.
            if (record.size > 512U * 1024U * 1024U) return;
            auto bytes = filesystem.open(record.canonical_path);
            if (!bytes) return;
            auto header = assets::load_map_header(bytes.value(), assets::source_from(record));
            if (!header || !setup_map_eligible(header.value(), query)) return;
            // Custom-directory discovery admits only authored custom maps.
            // An official header here must not change the stock picker list.
            if (record.canonical_path.starts_with("data/custommaps/") && header.value().lobby.custom != true) return;
            auto map = assets::load_map(bytes.value(), assets::source_from(record), types);
            // A header match still requires a completely valid body.
            if (map) candidates[index] = build_entry(record, map.value(), bytes.value());
        });
    }();
    for (std::size_t index = 0; index < errors.size(); ++index) {
        if (!errors[index]) continue;
        try { std::rethrow_exception(errors[index]); }
        catch (const std::exception& error) {
            return Result::failure(detail::error(diagnostic_codes::fixture,
                "Could not prepare map " + records.value()[index].canonical_path + ": " + error.what()));
        } catch (...) {
            return Result::failure(detail::error(diagnostic_codes::fixture,
                "Could not prepare map " + records.value()[index].canonical_path + ": unknown worker exception"));
        }
    }
    // Worker scheduling never chooses list order or a marker's reverse-record priority.
    for (auto& candidate : candidates) if (candidate) maps.push_back(std::move(*candidate));
    // Picker collision policy: original-directory custom maps win same-stem
    // collisions within the custom tab. Official maps in the other tab must
    // not hide a mod's custom map. VFS paths are already case-folded.
    std::set<std::string> original_custom_names;
    for (const auto& map : maps) {
        if (map.metadata.custom == true && map.path.starts_with("data/art/maps/"))
            original_custom_names.insert(map.path.substr(map.path.rfind('/') + 1));
    }
    std::erase_if(maps, [&](const auto& map) {
        return map.path.starts_with("data/custommaps/")
            && original_custom_names.contains(map.path.substr(map.path.rfind('/') + 1));
    });
    std::sort(maps.begin(), maps.end(), [](const auto& a, const auto& b) {
        return a.name != b.name ? a.name < b.name : a.path < b.path;
    });
    return Result::success(std::move(maps));
}

core::Result<std::vector<SetupMap>> setup_maps(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const SetupMapQuery& query) {
    return setup_maps(filesystem, catalog, query, std::thread::hardware_concurrency());
}

const std::vector<SetupMap>* SetupMapCache::find(const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const SetupMapQuery& query) const {
    return filesystem_.get() == &filesystem && catalog_.get() == &catalog
        && generation_ == catalog.generation() && query_ == query ? &maps_ : nullptr;
}

core::Result<void> SetupMapCache::load(std::shared_ptr<const vfs::Vfs> filesystem,
    std::shared_ptr<const data::Catalog> catalog, const SetupMapQuery& query) {
    if (find(*filesystem, *catalog, query)) return core::Result<void>::success();
    land_failure_.clear();
    auto maps = setup_domain_maps(*filesystem, *catalog, query, land_skirmish_enabled, &land_failure_);
    if (!maps) return core::Result<void>::failure(maps.error());
    maps_ = std::move(maps).value();
    filesystem_ = std::move(filesystem);
    catalog_ = std::move(catalog);
    generation_ = catalog_->generation();
    query_ = query;
    return core::Result<void>::success();
}

std::uint32_t setup_row_count(const SetupMap& map, const bool local_controls) noexcept {
    return std::min(map.metadata.capacity.value_or(0), local_controls ? local_setup_rows
        : static_cast<std::uint32_t>(sim::max_match_seats));
}

void repair_setup_selection(SetupSelection& selection, const SetupMap& map, const bool local_controls) {
    selection.map = map.path;
    const auto rows = setup_row_count(map, local_controls);
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
    const assets::MapLobbyMetadata& metadata, const bool local_controls, const bool complete_teams) {
    using Result = core::Result<void>;
    const auto fail = [](std::string message) {
        return Result::failure(detail::error(diagnostic_codes::fixture, std::move(message)));
    };
    if (!metadata.capacity) return fail("This map has no valid authored player capacity.");
    const auto rows = std::min(*metadata.capacity, local_controls ? local_setup_rows
        : static_cast<std::uint32_t>(sim::max_match_seats));
    if (slots.size() < (complete_teams ? 2U : 1U) || slots.size() > rows) return fail("Choose at least two players within this map's local capacity.");
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
        if (static_cast<unsigned>(slot.difficulty) > static_cast<unsigned>(sim::tactical::AiLevel::hard)) return fail("Choose a valid AI difficulty level.");
        if (slot.human && slot.difficulty != sim::tactical::AiLevel::normal) return fail("A human row has no AI difficulty level.");
        if (slot.team >= metadata.start_positions->size()) return fail("The selected team has no authored start on this map.");
        const auto [team, inserted] = factions.emplace(slot.team, slot.faction);
        if (complete_teams && !inserted && !detail::iequals(team->second, slot.faction))
            return fail("Players sharing a team must choose the same faction.");
    }
    if (complete_teams && factions.size() < 2) return fail("Choose at least two different teams.");
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

core::Result<FixtureOptions> setup_options(const SetupSelection& selection, std::span<const SetupMap> maps,
    const bool local_controls) {
    using Result = core::Result<FixtureOptions>;
    const auto fail = [](std::string message) {
        return Result::failure(detail::error(diagnostic_codes::fixture, std::move(message)));
    };
    const auto map = std::find_if(maps.begin(), maps.end(), [&](const auto& entry) { return entry.path == selection.map; });
    if (map == maps.end()) return fail("Select an installed space skirmish map.");
    if (!map->unavailable.empty()) return fail(map->unavailable);
    if (auto valid = validate_setup_slots(selection.slots, map->metadata, local_controls); !valid) return Result::failure(valid.error());
    std::set<std::uint32_t> colours;
    for (const auto& slot : selection.slots) {
        const auto colour = slot.colour_index.value_or(slot.slot - 1);
        if (colour >= multiplayer_colour_count || !colours.insert(colour).second)
            return fail("Choose distinct colours from the authored multiplayer palette.");
        const auto team = std::find_if(map->teams.begin(), map->teams.end(), [&](const auto& entry) { return entry.team == slot.team; });
        if (team == map->teams.end()) return fail("The selected team has no start on this map.");
        // WSS-54: retail validates total capacity and teams, not per-team spawn counts.
        // Project limit, not retail parity: only a faction the map's station candidates name can start here.
        if (std::none_of(team->factions.begin(), team->factions.end(),
                [&](const auto& name) { return detail::iequals(name, slot.faction); }))
            return fail("Project limit: this map has no starting station for " + slot.faction + " on the selected team.");
        if (!slot.fleet.empty()) return fail("Starting fleets are not configurable in this screen.");
    }
    FixtureOptions options;
    options.map = selection.map;
    options.slots = std::vector<LobbySlot>(selection.slots.begin(), selection.slots.end());
    options.match = selection.match;
    options.domain = map->domain;
    // Land has no AI seats yet (the land start admits human lobby slots only): every occupied row starts as an
    // unattended human seat, so the opponent rows of the setup screen still start a battle.
    if (options.domain == sim::tactical::BattleDomain::land)
        for (auto& slot : *options.slots) { slot.human = true; slot.difficulty = sim::tactical::AiLevel{}; }
    // WSS-31: the copied record's space victory choice, when it names one of the two evidenced conditions;
    // any other text leaves the data's own default (the start reads it, WBF-08).
    if (selection.match) {
        if (auto condition = sim::tactical::parse_victory_condition(selection.match->tactical_win_condition))
            options.victory_condition = condition.value();
    }
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
