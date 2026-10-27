#include "skirmish_prepare.hpp"
#include "lan_match.hpp"
#include <iomanip>
#include <locale>
#include "eawr/core/load_profile.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
namespace eawr::presentation::godot_backend {
core::Result<std::vector<std::uint32_t>> skirmish_player_ids(std::string_view text) {
    using Result = core::Result<std::vector<std::uint32_t>>;
    std::vector<std::uint32_t> ids;
    std::uint32_t previous{};
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto part = text.substr(0, comma);
        std::uint32_t id{};
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), id);
        if (part.empty() || parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()
            || id <= previous || id > sim::tactical::max_players || ids.size() >= skirmish::local_setup_rows) break;
        ids.push_back(id); previous = id;
        if (comma == std::string_view::npos) { text = {}; break; }
        text.remove_prefix(comma + 1);
        if (text.empty()) text = ","; // Preserve trailing-comma refusal.
    }
    if (!text.empty() || ids.size() < 2) {
        core::Diagnostic error;
        error.code = "EAWR-NET-1700";
        error.message = "--eawr-skirmish-players requires two to " + std::to_string(skirmish::local_setup_rows)
            + " increasing slot IDs";
        return Result::failure(std::move(error));
    }
    return Result::success(std::move(ids));
}

namespace { std::string lower_path(std::string_view value) {
    std::string result(value); for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return result;
} }
// The mounted FoC view's unit tables (the M2 types), or nothing with `failure` set.
[[nodiscard]] std::optional<units::UnitTables> load_start_tables(const vfs::Vfs& filesystem, const data::Catalog& catalog,
                                                           std::string& failure, const std::string& space_map,
                                                           const std::set<std::string>& additional_types) {
    core::load_profile::Scope load_scope(core::load_profile::Phase::unit_tables);
    scene::VfsAssetCache cache(filesystem);
    units::LoadInput input;
    input.catalog = &catalog;
    input.filesystem = &filesystem;
    input.space_map = space_map;
    input.model = cache.access().model;
    if (!additional_types.empty()) {
        for (const auto type : units::pinned_m2_types()) input.types.emplace_back(type);
        input.types.insert(input.types.end(), additional_types.begin(), additional_types.end());
        for (const auto type : units::pinned_m2_obstacles()) input.obstacles.emplace_back(type);
    }
    auto tables = units::load_unit_tables(input);
    if (!tables) {
        failure = "live session unit tables: " + core::format_diagnostic(tables.error());
        return std::nullopt;
    }
    return std::move(tables).value();
}

[[nodiscard]] std::set<std::string> missing_start_types(const skirmish::Fixture& fixture,
                                                       const skirmish::StartInputs& inputs,
                                                       const units::UnitTables& tables) {
    std::set<std::string> missing;
    const auto need = [&](const std::string& type) {
        // RG-04: skip gated starting types before their unsupported data is loaded.
        if (!skirmish::roster_disabled_types().contains(skirmish::type_id(type)) && !tables.find(type)) missing.insert(type);
    };
    for (const auto& slot : fixture.slots) {
        for (const auto& type : slot.fleet) need(type);
        for (const auto& forces : inputs.faction_forces) {
            if (lower_path(forces.faction) != lower_path(slot.faction)) continue;
            for (const auto& type : forces.space_skirmish_default_forces) need(type);
        }
        const std::string station = "team_" + std::string(slot.team < 10 ? "0" : "")
            + std::to_string(slot.team) + "_space_station";
        for (const auto& placement : inputs.placements) {
            if (!placement.marker || lower_path(placement.type) != station) continue;
            for (const auto& candidate : placement.marker_for) {
                std::string affiliations = candidate.affiliation;
                std::replace(affiliations.begin(), affiliations.end(), ',', ' ');
                std::istringstream names(affiliations);
                std::string faction;
                while (names >> faction) if (lower_path(faction) == lower_path(slot.faction)) need(candidate.type);
            }
        }
    }
    return missing;
}


core::Result<PreparedSkirmish> prepare_skirmish(const skirmish::Fixture& fixture,
    const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    std::string failure;
    auto tables = load_start_tables(filesystem, catalog, failure, fixture.map);
    if (!tables) { core::Diagnostic error; error.code = "EAWR-NET-0600"; error.message = failure;
        return core::Result<PreparedSkirmish>::failure(std::move(error)); }
    auto inputs = skirmish::read_start_inputs(fixture, filesystem, catalog, *tables);
    if (!inputs) {
        auto error = inputs.error(); error.message = "live session start inputs: " + core::format_diagnostic(error);
        return core::Result<PreparedSkirmish>::failure(std::move(error));
    }
    // SC-01: follow the selected starting types' craft, hangar, hardpoint and projectile references.
    // Keep the existing closure when every selected type is already loaded.
    const auto missing = missing_start_types(fixture, inputs.value(), *tables);
    if (!missing.empty()) {
        tables = load_start_tables(filesystem, catalog, failure, fixture.map, missing);
        if (!tables) { core::Diagnostic error; error.code = "EAWR-NET-0600"; error.message = failure;
            return core::Result<PreparedSkirmish>::failure(std::move(error)); }
        inputs.value().tables = &*tables;
    }
    return prepare_skirmish(fixture, std::move(*tables), std::move(inputs).value());
}
core::Result<PreparedSkirmish> prepare_skirmish(const skirmish::Fixture& fixture, units::UnitTables tables, skirmish::StartInputs inputs) {
    inputs.tables = &tables;
    auto start = skirmish::build_start(fixture, inputs);
    if (!start) {
        auto error = start.error(); error.message = "live session start: " + core::format_diagnostic(error);
        return core::Result<PreparedSkirmish>::failure(std::move(error));
    }
    return core::Result<PreparedSkirmish>::success({std::move(tables), std::move(inputs), std::move(start).value()});
}
bool lan_map_supports_rows(const skirmish::SetupMap& map, std::span<const skirmish::LobbySlot> occupied) {
    const auto rows = skirmish::setup_row_count(map, false);
    return rows >= 2 && std::all_of(occupied.begin(), occupied.end(),
        [rows](const auto& row) { return row.slot != 0 && row.slot <= rows; });
}
core::Result<skirmish::FixtureOptions> validated_lan_options(const skirmish::SetupSelection& selection, std::span<const skirmish::SetupMap> maps) {
    auto result = skirmish::setup_options(selection, maps, false);
    if (result) result.value().open_slots = selection.open_slots;
    return result;
}
core::Result<void> append_lan_ai_row(skirmish::SetupSelection& selection, const skirmish::SetupMap& map,
    std::uint32_t slot, std::size_t palette_size, bool prefer_unused_team) {
    auto failure = net::lobby_diagnostic(net::LobbyReason::invalid_choice, "This map has no valid AI starting team.");
    std::set<std::uint32_t> used;
    for (const auto& row : selection.slots) used.insert(row.colour_index.value_or(row.slot - 1));
    std::uint32_t colour = 0;
    while (colour < palette_size && used.contains(colour)) ++colour;
    if (colour >= palette_size) return core::Result<void>::failure(
        net::lobby_diagnostic(net::LobbyReason::invalid_choice, "There is no unused multiplayer colour for this row."));
    const auto host_team = selection.slots.empty() ? 0U : selection.slots.front().team;
    // Project UI default: prefer a free authored marker before placing a new AI on
    // an overflowing team. WSS-54 allows overflow; this preference is never a Start gate.
    for (const bool free_marker : {true, false}) for (const bool unused_team : {true, false}) {
        if (!prefer_unused_team && unused_team) continue;
        for (const bool opposing : {true, false}) for (const auto& team : map.teams) {
            if (team.factions.empty() || (team.team != host_team) != opposing) continue;
            const auto occupied = static_cast<std::size_t>(std::count_if(selection.slots.begin(), selection.slots.end(),
                [&](const auto& row) { return row.team == team.team; }));
            const bool has_free_marker = !team.spawn_capacity || occupied < *team.spawn_capacity;
            if (has_free_marker != free_marker) continue;
            skirmish::LobbySlot added; added.slot = slot; added.team = team.team; added.colour_index = colour;
            const auto teammate = std::find_if(selection.slots.begin(), selection.slots.end(), [&](const auto& row) { return row.team == team.team; });
            if (prefer_unused_team && unused_team != (teammate == selection.slots.end())) continue;
            added.faction = teammate == selection.slots.end() ? team.factions.front() : teammate->faction;
            auto candidate = selection; candidate.slots.push_back(std::move(added));
            std::sort(candidate.slots.begin(), candidate.slots.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
            auto valid = skirmish::validate_setup_slots(candidate.slots, map.metadata, false, false);
            if (valid) { selection = std::move(candidate); return core::Result<void>::success(); }
            failure = valid.error();
        }
    }
    return core::Result<void>::failure(std::move(failure));
}
const char* lan_ai_label(sim::tactical::AiLevel level) {
    return level == sim::tactical::AiLevel::easy ? "TEXT_EASY_AI_PLAYER"
        : level == sim::tactical::AiLevel::hard ? "TEXT_HARD_AI_PLAYER" : "TEXT_MEDIUM_AI_PLAYER";
}
LanSeatKind lan_seat_kind(const skirmish::FixtureOptions& options, std::uint32_t slot) {
    if (options.slots) for (const auto& row : *options.slots) if (row.slot == slot)
        return row.human ? LanSeatKind::human : LanSeatKind::ai;
    return std::find(options.open_slots.begin(), options.open_slots.end(), slot) != options.open_slots.end()
        ? LanSeatKind::open : LanSeatKind::closed;
}
core::Result<void> select_lan_seat_kind(skirmish::SetupSelection& selection, const skirmish::SetupMap& map,
    std::uint32_t slot, std::size_t palette_size, LanSeatKind kind, sim::tactical::AiLevel level) {
    const auto bad = [] { return core::Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice,
        "Choose Open, Closed or an AI level for a row without a human.")); };
    if (slot == 0 || slot > skirmish::setup_row_count(map, false)
        || (kind != LanSeatKind::closed && kind != LanSeatKind::open && kind != LanSeatKind::ai)
        || static_cast<unsigned>(level) > static_cast<unsigned>(sim::tactical::AiLevel::hard)) return bad();
    auto candidate = selection;
    auto row = std::find_if(candidate.slots.begin(), candidate.slots.end(), [&](const auto& value) { return value.slot == slot; });
    if (row != candidate.slots.end() && row->human) return bad();
    std::erase(candidate.open_slots, slot);
    if (kind == LanSeatKind::ai) {
        if (row == candidate.slots.end()) {
            auto added = append_lan_ai_row(candidate, map, slot, palette_size);
            if (!added) return added;
            row = std::find_if(candidate.slots.begin(), candidate.slots.end(), [&](const auto& value) { return value.slot == slot; });
        }
        row->difficulty = level;
    } else {
        if (row != candidate.slots.end()) candidate.slots.erase(row);
        if (kind == LanSeatKind::open) candidate.open_slots.push_back(slot);
        std::sort(candidate.open_slots.begin(), candidate.open_slots.end());
    }
    selection = std::move(candidate);
    return core::Result<void>::success();
}
core::Result<void> select_lan_ai_level(skirmish::SetupSelection& selection, const skirmish::SetupMap& map,
    std::uint32_t slot, std::size_t palette_size, int item) {
    if (item < 2 || item > 4) return core::Result<void>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice,
        "Choose a valid AI level."));
    return select_lan_seat_kind(selection, map, slot, palette_size, LanSeatKind::ai,
        item == 2 ? sim::tactical::AiLevel::easy : item == 4 ? sim::tactical::AiLevel::hard : sim::tactical::AiLevel::normal);
}
sim::tactical::TacticalSetup lan_recording_setup(const skirmish::Fixture& fixture, const skirmish::SkirmishStart& start) {
    auto setup = skirmish::recording_setup(fixture, start);
    setup.queue_identities = true;
    if (setup.skirmish) {
        setup.departure_policy.emplace();
        for (const auto& slot : setup.skirmish->slots) if (slot.human) setup.departure_policy->humans.push_back(slot.player);
    }
    return setup;
}
core::Result<net::LobbyGame> encode_lan_lobby_game(const skirmish::FixtureOptions& options, std::uint64_t input_delay) {
    if (!options.map || !options.slots || options.slots->empty() || !options.match || options.slots->size() + options.open_slots.size() > net::max_match_seats || input_delay == 0 || input_delay > net::max_input_delay) {
        return core::Result<net::LobbyGame>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice, "Incomplete staging options."));
    }
    std::ostringstream out; out.imbue(std::locale::classic());
    const auto& m = *options.match;
    out << 3 << ' ' << input_delay << ' ' << options.seed.has_value() << ' ' << options.seed.value_or(0)
        << ' ' << options.victory_condition.has_value() << ' ' << static_cast<unsigned>(options.victory_condition.value_or(sim::tactical::VictoryCondition::none))
        << ' ' << m.allow_heroes << ' ' << m.allow_superweapons << ' ' << m.free_starting_units << ' ' << m.pre_built_base
        << ' ' << m.allow_random_events << ' ' << m.credits.raw() << ' ' << m.start_tech << ' ' << m.max_tech
        << ' ' << m.game_timer << ' ' << m.win_integer << ' ' << m.auto_resolve << ' ' << m.win_float.raw()
        << ' ' << std::quoted(m.win_condition) << ' ' << std::quoted(m.tactical_win_condition) << ' ' << options.slots->size();
    std::set<std::uint32_t> rows;
    std::uint32_t previous{};
    for (const auto& slot : *options.slots) {
        if (slot.slot == 0 || slot.slot <= previous || slot.slot > net::max_match_seats
            || !rows.insert(slot.slot).second || static_cast<unsigned>(slot.difficulty) > static_cast<unsigned>(sim::tactical::AiLevel::hard)
            || (slot.human && slot.difficulty != sim::tactical::AiLevel::normal))
            return core::Result<net::LobbyGame>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice, "Invalid AI level."));
        out << ' ' << slot.slot << ' ' << std::quoted(slot.faction) << ' ' << slot.team << ' ' << static_cast<unsigned>(lan_seat_kind(options, slot.slot))
            << ' ' << static_cast<unsigned>(slot.difficulty) << ' ' << slot.colour_index.value_or(slot.slot - 1) << ' ' << slot.fleet.size();
        for (const auto& type : slot.fleet) out << ' ' << std::quoted(type);
        previous = slot.slot;
    }
    out << ' ' << options.open_slots.size();
    previous = 0;
    for (const auto slot : options.open_slots) {
        if (slot == 0 || slot <= previous || slot > net::max_match_seats || !rows.insert(slot).second)
            return core::Result<net::LobbyGame>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice, "Invalid Open staging row."));
        out << ' ' << slot; previous = slot;
    }
    const auto bytes = out.str();
    if (bytes.size() > net::max_lobby_payload_bytes) return core::Result<net::LobbyGame>::failure(
        net::lobby_diagnostic(net::LobbyReason::resource_limit, "Staging options exceed the room budget."));
    return core::Result<net::LobbyGame>::success({*options.map, {bytes.begin(), bytes.end()}});
}
core::Result<skirmish::FixtureOptions> decode_lan_lobby_game(const net::LobbySnapshot& snapshot, std::uint64_t* input_delay) {
    using Result = core::Result<skirmish::FixtureOptions>;
    const auto bad = [] { return Result::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice, "Invalid canonical staging options.")); };
    if (snapshot.game.options.empty() || snapshot.game.options.size() > net::max_lobby_payload_bytes) return bad();
    const std::string bytes(snapshot.game.options.begin(), snapshot.game.options.end());
    if (net::sanitize_lobby_text(bytes, bytes.size()) != bytes) return bad();
    std::istringstream in(bytes); in.imbue(std::locale::classic());
    unsigned version{}, victory{}; bool seed_given{}, victory_given{}; std::uint64_t seed{}, delay{}; std::int64_t credits{}, win{}; std::size_t count{};
    skirmish::FixtureOptions result; result.map = snapshot.game.map; result.match.emplace();
    auto& m = *result.match;
    if (!(in >> version)) return bad();
    if (version != 3) return Result::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice,
        "Unsupported skirmish staging options version; all peers must use the same build."));
    if (!(in >> delay >> seed_given >> seed >> victory_given >> victory >> m.allow_heroes >> m.allow_superweapons >> m.free_starting_units >> m.pre_built_base
        >> m.allow_random_events >> credits >> m.start_tech >> m.max_tech >> m.game_timer >> m.win_integer >> m.auto_resolve >> win
        >> std::quoted(m.win_condition) >> std::quoted(m.tactical_win_condition) >> count)
        || delay == 0 || delay > net::max_input_delay || victory > static_cast<unsigned>(sim::tactical::VictoryCondition::all_enemy_units_destroyed)
        || count == 0 || count > net::max_match_seats) return bad();
    if (seed_given) result.seed = seed;
    if (victory_given) result.victory_condition = static_cast<sim::tactical::VictoryCondition>(victory);
    m.credits = sim::math::Fixed::from_raw(credits); m.win_float = sim::math::Fixed::from_raw(win);
    result.slots.emplace();
    for (std::size_t index = 0; index < count; ++index) {
        skirmish::LobbySlot slot; std::uint32_t colour{}; std::size_t fleet{}; unsigned level{}, kind{};
        if (!(in >> slot.slot >> std::quoted(slot.faction) >> slot.team >> kind >> level >> colour >> fleet)
            || (kind != static_cast<unsigned>(LanSeatKind::human) && kind != static_cast<unsigned>(LanSeatKind::ai))
            || level > static_cast<unsigned>(sim::tactical::AiLevel::hard) || fleet > 4096) return bad();
        slot.human = kind == static_cast<unsigned>(LanSeatKind::human);
        slot.difficulty = static_cast<sim::tactical::AiLevel>(level);
        slot.colour_index = colour;
        for (std::size_t n = 0; n < fleet; ++n) { std::string type; if (!(in >> std::quoted(type)) || type.size() > 1024) return bad(); slot.fleet.push_back(std::move(type)); }
        result.slots->push_back(std::move(slot));
    }
    std::size_t vacancies{};
    if (!(in >> vacancies) || vacancies > net::max_match_seats - count) return bad();
    for (std::size_t n = 0; n < vacancies; ++n) {
        std::uint32_t slot{}; if (!(in >> slot)) return bad(); result.open_slots.push_back(slot);
    }
    auto canonical = encode_lan_lobby_game(result, delay);
    if (!canonical || canonical.value() != snapshot.game) return bad();
    // A departed or relocated human leaves Open. An Open vacancy is never an AI.
    // An empty roster is the caller's explicit request for authored staging rows.
    if (!snapshot.seats.empty()) {
        std::erase_if(*result.slots, [&](const auto& row) {
            if (!row.human || std::any_of(snapshot.seats.begin(), snapshot.seats.end(), [&](const auto& seat) { return seat.selection.slot == row.slot; })) return false;
            result.open_slots.push_back(row.slot); return true;
        });
    }
    // Seat requests override their owner row; shared humans must agree, checked by the loaded validator.
    for (const auto& seat : snapshot.seats) {
        auto row = std::find_if(result.slots->begin(), result.slots->end(), [&](const auto& value) { return value.slot == seat.selection.slot; });
        if (row == result.slots->end()) {
            if (lan_seat_kind(result, seat.selection.slot) != LanSeatKind::open) return bad();
            skirmish::LobbySlot added; added.slot = seat.selection.slot;
            result.slots->push_back(std::move(added)); row = std::prev(result.slots->end());
        } else if (!row->human) return bad();
        std::erase(result.open_slots, seat.selection.slot);
        row->faction = seat.selection.faction; row->team = seat.selection.team; row->colour_index = seat.selection.colour; row->human = true;
        row->difficulty = sim::tactical::AiLevel::normal;
    }
    std::sort(result.slots->begin(), result.slots->end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
    std::sort(result.open_slots.begin(), result.open_slots.end());
    if (input_delay) *input_delay = delay;
    return Result::success(std::move(result));
}

}
