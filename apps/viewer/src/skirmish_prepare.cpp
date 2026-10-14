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
        error.message = "--eawr-skirmish-players requires two to eight increasing slot IDs";
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
core::Result<skirmish::FixtureOptions> validated_lan_options(const skirmish::SetupSelection& selection, std::span<const skirmish::SetupMap> maps) {
    // The shared local-dialog validator checks authored rows/teams/options. LAN human flags are independent command-seat ownership.
    auto local = selection;
    for (auto& row : local.slots) row.human = row.slot == 1;
    auto result = skirmish::setup_options(local, maps);
    if (result) result.value().slots = selection.slots;
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
    // WSS-15/54: prefer an opposing team only when its faction and authored spawns admit this row.
    for (const bool unused_team : {true, false}) {
        if (!prefer_unused_team && unused_team) continue;
        for (const bool opposing : {true, false}) for (const auto& team : map.teams) {
            if (team.factions.empty() || (team.team != host_team) != opposing) continue;
            skirmish::LobbySlot added; added.slot = slot; added.team = team.team; added.colour_index = colour;
            const auto teammate = std::find_if(selection.slots.begin(), selection.slots.end(), [&](const auto& row) { return row.team == team.team; });
            if (prefer_unused_team && unused_team != (teammate == selection.slots.end())) continue;
            added.faction = teammate == selection.slots.end() ? team.factions.front() : teammate->faction;
            auto candidate = selection; candidate.slots.push_back(std::move(added));
            std::sort(candidate.slots.begin(), candidate.slots.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
            auto valid = validated_lan_options(candidate, std::span<const skirmish::SetupMap>(&map, 1));
            if (valid) { selection = std::move(candidate); return core::Result<void>::success(); }
            failure = valid.error();
        }
    }
    return core::Result<void>::failure(std::move(failure));
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
    if (!options.map || !options.slots || !options.match || options.slots->size() > skirmish::local_setup_rows || input_delay == 0 || input_delay > net::max_input_delay) {
        return core::Result<net::LobbyGame>::failure(net::lobby_diagnostic(net::LobbyReason::invalid_choice, "Incomplete staging options."));
    }
    std::ostringstream out; out.imbue(std::locale::classic());
    const auto& m = *options.match;
    out << 1 << ' ' << input_delay << ' ' << options.seed.has_value() << ' ' << options.seed.value_or(0)
        << ' ' << options.victory_condition.has_value() << ' ' << static_cast<unsigned>(options.victory_condition.value_or(sim::tactical::VictoryCondition::none))
        << ' ' << m.allow_heroes << ' ' << m.allow_superweapons << ' ' << m.free_starting_units << ' ' << m.pre_built_base
        << ' ' << m.allow_random_events << ' ' << m.credits.raw() << ' ' << m.start_tech << ' ' << m.max_tech
        << ' ' << m.game_timer << ' ' << m.win_integer << ' ' << m.auto_resolve << ' ' << m.win_float.raw()
        << ' ' << std::quoted(m.win_condition) << ' ' << std::quoted(m.space_win_condition) << ' ' << options.slots->size();
    for (const auto& slot : *options.slots) {
        out << ' ' << slot.slot << ' ' << std::quoted(slot.faction) << ' ' << slot.team << ' ' << slot.human
            << ' ' << slot.colour_index.value_or(slot.slot - 1) << ' ' << slot.fleet.size();
        for (const auto& type : slot.fleet) out << ' ' << std::quoted(type);
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
    if (!(in >> version >> delay >> seed_given >> seed >> victory_given >> victory >> m.allow_heroes >> m.allow_superweapons >> m.free_starting_units >> m.pre_built_base
        >> m.allow_random_events >> credits >> m.start_tech >> m.max_tech >> m.game_timer >> m.win_integer >> m.auto_resolve >> win
        >> std::quoted(m.win_condition) >> std::quoted(m.space_win_condition) >> count)
        || version != 1 || delay == 0 || delay > net::max_input_delay || victory > static_cast<unsigned>(sim::tactical::VictoryCondition::all_enemy_units_destroyed)
        || count < 2 || count > skirmish::local_setup_rows) return bad();
    if (seed_given) result.seed = seed;
    if (victory_given) result.victory_condition = static_cast<sim::tactical::VictoryCondition>(victory);
    m.credits = sim::math::Fixed::from_raw(credits); m.win_float = sim::math::Fixed::from_raw(win);
    result.slots.emplace();
    for (std::size_t index = 0; index < count; ++index) {
        skirmish::LobbySlot slot; std::uint32_t colour{}; std::size_t fleet{};
        if (!(in >> slot.slot >> std::quoted(slot.faction) >> slot.team >> slot.human >> colour >> fleet) || fleet > 4096) return bad();
        slot.colour_index = colour;
        for (std::size_t n = 0; n < fleet; ++n) { std::string type; if (!(in >> std::quoted(type)) || type.size() > 1024) return bad(); slot.fleet.push_back(std::move(type)); }
        result.slots->push_back(std::move(slot));
    }
    auto canonical = encode_lan_lobby_game(result, delay);
    if (!canonical || canonical.value() != snapshot.game) return bad();
    if (!snapshot.seats.empty()) for (auto& slot : *result.slots) slot.human = false;
    // Seat requests override their owner row; shared humans must agree, checked by the loaded validator.
    for (const auto& seat : snapshot.seats) {
        const auto row = std::find_if(result.slots->begin(), result.slots->end(), [&](const auto& value) { return value.slot == seat.selection.slot; });
        if (row == result.slots->end()) return bad();
        row->faction = seat.selection.faction; row->team = seat.selection.team; row->colour_index = seat.selection.colour; row->human = true;
    }
    if (input_delay) *input_delay = delay;
    return Result::success(std::move(result));
}

}
