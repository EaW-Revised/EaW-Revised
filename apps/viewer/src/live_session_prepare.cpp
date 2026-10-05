#include "eawr/core/load_profile.hpp"
#include "live_session_view.hpp"
#include "eawr/presentation/ui/pads.hpp"

#include "shutdown_trace.hpp"
#include "frame_timer.hpp"

#include "eawr/platform/live_ai.hpp"
#include "eawr/presentation/space/live_units.hpp"
#include "eawr/presentation/space/unit_fade.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/presentation/ui/minimap.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/units/unit_tables.hpp"

#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "live_session_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace tactical = sim::tactical;
using namespace live_session_detail;

namespace {
// How far a driven frame's presentation tick may sit from a capture tick it shows.
// The mounted FoC view's unit tables (the M2 types), or nothing with `failure` set.
[[nodiscard]] std::optional<units::UnitTables> load_tables(const vfs::Vfs& filesystem, const data::Catalog& catalog,
                                                           std::string& failure, const std::string& space_map,
                                                           const std::set<std::string>& additional_types = {}) {
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
} // namespace

bool LiveSessionView::prepare_m2(const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string& failure) {
    skirmish::Fixture fixture = skirmish::m2_fixture();
    if (options_.fixture == "skirmish") {
        auto selected = skirmish::fixture_from_options(options_.skirmish, filesystem, catalog);
        if (!selected) {
            failure = "live skirmish options: " + core::format_diagnostic(selected.error());
            return false;
        }
        fixture = std::move(selected).value();
    }
    auto tables = load_tables(filesystem, catalog, failure, fixture.map);
    if (!tables) return false;
    auto inputs = skirmish::read_start_inputs(fixture, filesystem, catalog, *tables);
    if (!inputs) {
        failure = "live session start inputs: " + core::format_diagnostic(inputs.error());
        return false;
    }
    if (options_.fixture == "skirmish") {
        // SC-01: the existing loader follows the selected types' craft, hangar,
        // hardpoint and projectile references. Keep its pinned inputs unchanged
        // when every selected type is already loaded (including the M2 default).
        const auto missing = missing_start_types(fixture, inputs.value(), *tables);
        if (!missing.empty()) {
            auto extended = load_tables(filesystem, catalog, failure, fixture.map, missing);
            if (!extended) return false;
            tables = std::move(extended);
            inputs.value().tables = &*tables;
        }
    }
    auto start = skirmish::build_start(fixture, inputs.value());
    if (!start) {
        failure = "live session start: " + core::format_diagnostic(start.error());
        return false;
    }
    // #76: the ability table's human players are the start's human lobby players, as the victory rules'.
    std::vector<tactical::PlayerId> humans;
    for (const skirmish::StartPlayer& player : start.value().players) {
        if (player.lobby && player.human) humans.push_back(player.player.player_id);
    }
    auto content = skirmish::session_content(*tables, humans);
    if (!content) {
        failure = "live session content: " + core::format_diagnostic(content.error());
        return false;
    }
    // #495: the map's fog grid. #507's live `--eawr-live-reveal` is a draw-time bypass only
    // (interpolate_units, LiveFogView, the minimap) and must never change what the session itself
    // binds: every session keeps its real sensor table on reveal, so skipping fog here
    // would silently swap visible_to's fog-cell test (V-11 to V-17)
    // for the exact-range one, changing hashes reveal on vs off.
    {
        auto fog = skirmish::fog_rules(inputs.value());
        if (!fog) {
            failure = "live session fog rules: " + core::format_diagnostic(fog.error());
            return false;
        }
        content.value().fog = fog.value();
    }
    if (options_.ai.value_or(true)) {
        // #79: the retail freestore of each AI player, from the mounted FoC scripts.
        core::load_profile::Scope lua_scope(core::load_profile::Phase::lua);
        auto ai = platform::live_ai(start.value(), inputs.value(), *tables, filesystem);
        if (!ai) {
            failure = "live session AI scripts: " + core::format_diagnostic(ai.error());
            return false;
        }
        ai_scripts_ = std::move(ai.value().scripts);
        ai_players_ = std::move(ai.value().players);
    }
    start_ = std::move(start).value();
    setup_ = skirmish::recording_setup(fixture, *start_);
    content_ = std::move(content).value();
    victory_ = skirmish::victory_rules(*start_, tables.value());
    // #530: the skirmish economy (credits, the station build queues and hyperspace arrival).
    auto economy = skirmish::economy_rules(*start_, inputs.value(), tables.value());
    if (!economy) {
        failure = "live session economy: " + core::format_diagnostic(economy.error());
        return false;
    }
    economy_ = std::move(economy).value();
    auto preview_colours = ui::reinforcement_colours(filesystem);
    if (!preview_colours) {
        failure = "reinforcement preview colours: " + core::format_diagnostic(preview_colours.error());
        return false;
    }
    preview_colours_ = preview_colours.value();
    tables_ = std::move(*tables);

    const auto human = std::find_if(start_->players.begin(), start_->players.end(),
        [](const skirmish::StartPlayer& player) { return player.human; });
    player_ = options_.player.value_or(human != start_->players.end() ? human->player.player_id : 1U);
    if (std::none_of(start_->players.begin(), start_->players.end(),
            [&](const skirmish::StartPlayer& player) { return player.player.player_id == player_; })) {
        failure = "--eawr-live-player " + std::to_string(player_) + " is not a player of the start";
        return false;
    }
    for (const skirmish::StartPlayer& player : start_->players) {
        team_of_player_[player.player.player_id] = player.player.team_id;
    }
    for (const skirmish::StartUnit& unit : start_->units) {
        SpacePopulation::Options::PlacedShip ship;
        ship.object_id = unit.type;
        ship.position = {to_float(unit.state.position.x), to_float(unit.state.position.y), to_float(unit.state.position.z)};
        ship.yaw_degrees = to_float(unit.yaw_degrees);
        ship.live_entity = unit.state.entity_id;
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const skirmish::StartPlayer& player) { return player.player.player_id == unit.state.owner; });
        if (owner != start_->players.end() && owner->colour) ship.team_colour = owner->colour->rgb;
        if (unit.role == skirmish::UnitRole::map_object) {
            // The map draws this record's object; the session owns it now. Its team colour
            // stays the one the map's owner gives it.
            session_records_.push_back(unit.record);
            ship.colour_record = unit.record;
            const auto obstacle = std::find_if(tables_->obstacles.begin(), tables_->obstacles.end(),
                [&](const units::ObstacleType& type) { return type.id == unit.type; });
            // FW-24: hazards remain simulated, but an environment prop without
            // either fog behavior draws independently of sensor contact.
            if (obstacle != tables_->obstacles.end() && obstacle->xml_type == "SpaceProp"
                && !space::map_prop_fog_bound(obstacle->footprint.hazard.behavior,
                                              obstacle->footprint.hazard.space_behavior)) {
                unfogged_map_props_.push_back(unit.state.entity_id);
            }
        }
        ship_of_entity_.emplace(unit.state.entity_id, placed_ships_.size());
        owner_of_entity_.emplace(unit.state.entity_id, unit.state.owner);
        placed_ships_.push_back(std::move(ship));
    }
    std::sort(unfogged_map_props_.begin(), unfogged_map_props_.end());
    return true;
}

bool LiveSessionView::prepare_replay(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const std::string_view map_path, std::string& failure) {
    auto tables = load_tables(filesystem, catalog, failure, {});
    if (!tables) return false;
    auto replay = [&]() -> std::optional<tactical::TacticalReplay> {
        if (options_.fixture == "melee") {
            // #601: the melee benchmark's fight, built here exactly as path_bench --melee builds it.
            auto inputs = skirmish::read_start_inputs(skirmish::m2_fixture(), filesystem, catalog, *tables);
            if (!inputs) {
                failure = "--eawr-live-session melee: " + core::format_diagnostic(inputs.error());
                return std::nullopt;
            }
            auto start = skirmish::build_start(skirmish::m2_fixture(), inputs.value());
            if (!start) {
                failure = "--eawr-live-session melee: " + core::format_diagnostic(start.error());
                return std::nullopt;
            }
            auto melee = skirmish::build_melee(*tables, start.value(), options_.melee_size.value_or(skirmish::MeleeSize::s),
                                               options_.melee_seed.value_or(601), 4500);
            if (!melee) {
                failure = "--eawr-live-session melee: " + core::format_diagnostic(melee.error());
                return std::nullopt;
            }
            return std::move(melee).value().replay;
        }
        std::ifstream file(options_.replay_input, std::ios::binary);
        if (!file) {
            failure = "--eawr-live-replay: cannot read " + ViewerPath::utf8(options_.replay_input);
            return std::nullopt;
        }
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto parsed = tactical::parse_replay(bytes, ViewerPath::utf8(options_.replay_input));
        if (!parsed) {
            failure = "--eawr-live-replay: " + core::format_diagnostic(parsed.error());
            return std::nullopt;
        }
        return std::move(parsed).value();
    }();
    if (!replay) return false;
    const tactical::TacticalSetup& setup = replay->setup;
    const std::string replay_map = setup.skirmish ? setup.skirmish->map : std::string(map_path);
    if (units::content_identity(*tables) != replay.value().setup.content_identity) {
        // WHZ-01: live recordings include the selected map's authored hazard
        // profiles. Pinned fixtures retain the narrower M2 closure above.
        auto selected = load_tables(filesystem, catalog, failure, replay_map);
        if (!selected) return false;
        tables = std::move(selected);
    }
    if (units::content_identity(*tables) != replay.value().setup.content_identity) {
        // SC-01: recordings of selected factions use the same starting-type closure
        // as live skirmishes, including station production and default fleet references.
        skirmish::FixtureOptions fixture_options;
        fixture_options.map = replay_map;
        auto selected = skirmish::fixture_from_options(fixture_options, filesystem, catalog);
        if (!selected) { failure = core::format_diagnostic(selected.error()); return false; }
        auto inputs = skirmish::read_start_inputs(selected.value(), filesystem, catalog, *tables);
        if (!inputs) { failure = core::format_diagnostic(inputs.error()); return false; }
        auto recorded = skirmish::replay_fixture(selected.value(), inputs.value(), replay->setup);
        if (!recorded) { failure = core::format_diagnostic(recorded.error()); return false; }
        inputs = skirmish::read_start_inputs(recorded.value(), filesystem, catalog, *tables);
        if (!inputs) { failure = core::format_diagnostic(inputs.error()); return false; }
        const auto missing = missing_start_types(recorded.value(), inputs.value(), *tables);
        if (!missing.empty()) {
            auto extended = load_tables(filesystem, catalog, failure, replay_map, missing);
            if (!extended) return false;
            tables = std::move(extended);
        }
    }
    if (units::content_identity(*tables) != replay.value().setup.content_identity) {
        failure = "--eawr-live-replay: the replay names other content than the mounted FoC unit tables";
        return false;
    }
    auto humans = skirmish::human_slots(skirmish::m2_fixture());
    if (setup.skirmish) {
        humans.clear();
        for (const auto& slot : setup.skirmish->slots) if (slot.human) humans.push_back(slot.player);
    }
    auto content = skirmish::session_content(*tables, humans);
    if (!content) {
        failure = "live session content: " + core::format_diagnostic(content.error());
        return false;
    }
    std::map<tactical::TypeId, std::string> names;
    for (const units::UnitType& type : tables->units) names.emplace(skirmish::type_id(type.id), type.id);
    // Team colours: the lobby colour of the M2 slot playing the player's faction.
    std::map<tactical::FactionId, std::array<std::uint8_t, 3>> faction_colours;
    auto selected_victory = tactical::VictoryCondition::enemy_starbase_destroyed;
    if (!setup.skirmish) {
        if (auto inputs = skirmish::read_start_inputs(skirmish::m2_fixture(), filesystem, catalog, *tables)) {
            selected_victory = inputs.value().space_victory_condition;
            // Display reveal bypasses drawing only; replay targeting retains the map's fog grid.
            {
                auto fog = skirmish::fog_rules(inputs.value());
                if (!fog) {
                    failure = "live session fog rules: " + core::format_diagnostic(fog.error());
                    return false;
                }
                content.value().fog = fog.value();
            }
            for (const skirmish::LobbySlot& slot : skirmish::m2_fixture().slots) {
                if (slot.slot >= 1U && slot.slot - 1U < inputs.value().lobby_colours.size()) {
                    faction_colours.emplace(skirmish::faction_id(slot.faction), inputs.value().lobby_colours[slot.slot - 1U].rgb);
                }
            }
            // #501: map-object units (e.g. Skirmish_Merchant_Dock) are not in the unit tables; the
            // live path places them straight from the map's placements (add_map_objects), so a
            // replay resolves their type the same way instead of a second lookup.
            for (const auto& [id, type] : skirmish::map_object_type_names(inputs.value().placements)) names.emplace(id, type);
            // #530: a replay of the M2 start runs with its economy, as sim_headless does.
            auto fixture = skirmish::m2_fixture();
            fixture.match = skirmish::replay_match_options(setup, inputs.value().match_defaults);
            auto start = skirmish::build_start(fixture, inputs.value());
            if (start && start.value().setup.players == setup.players && start.value().setup.units == setup.units) {
                auto economy = skirmish::economy_rules(start.value(), inputs.value(), *tables);
                if (!economy) {
                    failure = "live session economy: " + core::format_diagnostic(economy.error());
                    return false;
                }
                economy_ = std::move(economy).value();
            }
        }
    }
    // WBP-22/29, WHZ-01: selected-map replays need authored map-object names and model slots,
    // including ordinary recordings with no policy extension or pad-building command.
    const bool pad_replay = std::any_of(replay->commands.begin(), replay->commands.end(), [](const auto& command) {
        return std::holds_alternative<tactical::PadBuildPayload>(command.payload);
    });
    const bool selected_map_replay = lower_path(map_path) != lower_path(skirmish::m2_fixture().map);
    if (pad_replay || setup.match_policy || selected_map_replay || setup.skirmish) {
        skirmish::FixtureOptions fixture_options;
        fixture_options.map = replay_map;
        auto selected = skirmish::fixture_from_options(fixture_options, filesystem, catalog);
        if (!selected) { failure = core::format_diagnostic(selected.error()); return false; }
        const auto& fixture = selected.value();
        auto inputs = skirmish::read_start_inputs(fixture, filesystem, catalog, *tables);
        if (!inputs) { failure = core::format_diagnostic(inputs.error()); return false; }
        auto replay_fixture = fixture;
        replay_fixture.match = skirmish::replay_match_options(setup, inputs.value().match_defaults);
        if (setup.match_policy || selected_map_replay || setup.skirmish) {
            auto recorded = skirmish::replay_fixture(fixture, inputs.value(), setup);
            if (!recorded) { failure = core::format_diagnostic(recorded.error()); return false; }
            replay_fixture = std::move(recorded).value();
            inputs = skirmish::read_start_inputs(replay_fixture, filesystem, catalog, *tables);
            if (!inputs) { failure = core::format_diagnostic(inputs.error()); return false; }
            {
                auto fog = skirmish::fog_rules(inputs.value());
                if (!fog) { failure = core::format_diagnostic(fog.error()); return false; }
                content.value().fog = fog.value();
            }
        }
        auto start = skirmish::build_start(replay_fixture, inputs.value());
        if (!start) { failure = core::format_diagnostic(start.error()); return false; }
        if (setup.skirmish) {
            if (inputs.value().map_sha256 != setup.skirmish->map_sha256) {
                failure = "--eawr-live-replay: the recorded map identity differs from the mounted map";
                return false;
            }
            selected_victory = start.value().victory_condition;
        }
        for (const auto& [id, type] : skirmish::map_object_type_names(inputs.value().placements)) names.emplace(id, type);
        auto economy = skirmish::economy_rules(start.value(), inputs.value(), *tables);
        if (!economy) { failure = core::format_diagnostic(economy.error()); return false; }
        economy_ = std::move(economy).value();
        // WBP-29: focused fleets assign different IDs; map placement/type supplies model metadata.
        std::vector<skirmish::StartUnit> replay_units;
        replay_units.reserve(setup.units.size());
        for (const auto& state : setup.units) {
            auto authored = std::find_if(start.value().units.begin(), start.value().units.end(),
                [&](const auto& unit) {
                    return unit.state.type_id == state.type_id && unit.state.position == state.position;
                });
            if (authored == start.value().units.end()) {
                authored = std::find_if(start.value().units.begin(), start.value().units.end(),
                    [&](const auto& unit) { return unit.state.type_id == state.type_id; });
            }
            if (authored != start.value().units.end()) {
                replay_units.push_back(*authored);
            } else {
                const auto name = names.find(state.type_id);
                if (name == names.end()) {
                    failure = "pad replay has a starting type absent from the mounted tables";
                    return false;
                }
                skirmish::StartUnit visual;
                visual.type = name->second;
                visual.role = skirmish::UnitRole::fleet;
                replay_units.push_back(std::move(visual));
            }
            replay_units.back().state = state;
        }
        start.value().units = std::move(replay_units);
        start.value().setup = setup;
        start.value().launches.clear();
        start_ = std::move(start).value();
    }
    player_ = options_.player.value_or(setup.players.empty() ? 1U : setup.players.front().player_id);
    if (std::none_of(setup.players.begin(), setup.players.end(),
            [&](const tactical::Player& player) { return player.player_id == player_; })) {
        failure = "--eawr-live-player " + std::to_string(player_) + " is not a player of the replay";
        return false;
    }
    for (const tactical::Player& player : setup.players) team_of_player_[player.player_id] = player.team_id;
    for (const tactical::UnitState& unit : setup.units) {
        const auto name = names.find(unit.type_id);
        if (name == names.end()) {
            failure = "--eawr-live-replay: unit " + std::to_string(unit.entity_id) + " has a type the tables lack";
            return false;
        }
        SpacePopulation::Options::PlacedShip ship;
        ship.object_id = name->second;
        ship.position = {to_float(unit.position.x), to_float(unit.position.y), to_float(unit.position.z)};
        auto transform = sim::math::to_matrix(unit.rotation, unit.position);
        ship.yaw_degrees = transform ? static_cast<float>(space::instance_yaw_degrees(transform.value())) : 0.0F;
        ship.live_entity = unit.entity_id;
        const auto owner = std::find_if(setup.players.begin(), setup.players.end(),
            [&](const tactical::Player& player) { return player.player_id == unit.owner; });
        if (owner != setup.players.end()) {
            if (const auto colour = faction_colours.find(owner->faction_id); colour != faction_colours.end()) {
                ship.team_colour = colour->second;
            }
            if (setup.skirmish && start_) {
                const auto recorded = std::find_if(start_->players.begin(), start_->players.end(),
                    [&](const auto& player) { return player.player.player_id == unit.owner; });
                if (recorded != start_->players.end() && recorded->colour) ship.team_colour = recorded->colour->rgb;
            }
        }
        ship_of_entity_.emplace(unit.entity_id, placed_ships_.size());
        owner_of_entity_.emplace(unit.entity_id, unit.owner);
        placed_ships_.push_back(std::move(ship));
    }
    setup_ = setup;
    victory_ = setup.skirmish && start_ ? skirmish::victory_rules(*start_, *tables)
        : skirmish::victory_rules(setup, *tables, humans, options_.skirmish.victory_condition.value_or(selected_victory));
    replay_ = std::move(replay).value();
    content_ = std::move(content).value();
    tables_ = std::move(*tables);
    return true;
}

bool LiveSessionView::prepare(const vfs::Vfs& filesystem, const data::Catalog& catalog, const std::string_view map_path,
                              std::string& failure) {
    core::load_profile::Scope load_scope(core::load_profile::Phase::session);
    const auto minimap_settings = ui::minimap_settings(filesystem);
    nebula_colour_ = minimap_settings.nebula;
    if (const auto neutral = ui::faction_colour(minimap_settings, "Neutral")) {
        neutral_fog_colour_ = std::array<float, 3>{neutral->r / 255.0F, neutral->g / 255.0F, neutral->b / 255.0F};
    }
    const skirmish::Fixture& fixture = skirmish::m2_fixture();
    if ((options_.fixture == "replay") != !options_.replay_input.empty()) {
        failure = "--eawr-live-replay goes with --eawr-live-session replay, which needs it";
        return false;
    }
    if ((options_.fixture == "m2" || options_.fixture == "melee") && lower_path(map_path) != lower_path(fixture.map)) {
        failure = "--eawr-live-session " + options_.fixture + " runs on " + fixture.map;
        return false;
    }
    if (options_.fixture != "skirmish" && (options_.skirmish.map || options_.skirmish.slots || options_.skirmish.seed)) {
        failure = "--eawr-skirmish-* options require --eawr-live-session skirmish";
        return false;
    }
    if (options_.skirmish.victory_condition && options_.fixture != "skirmish" && options_.fixture != "replay") {
        failure = "--eawr-skirmish-victory requires a skirmish start or replay";
        return false;
    }
    if (options_.fixture == "skirmish") {
        if (options_.skirmish.map && *options_.skirmish.map != lower_path(map_path)) {
            failure = "--eawr-map and --eawr-skirmish-map must select the same map";
            return false;
        }
        options_.skirmish.map = lower_path(map_path);
    }
    if (options_.fixture != "melee" && (options_.melee_size || options_.melee_seed)) {
        failure = "--eawr-live-melee and --eawr-live-melee-seed go with --eawr-live-session melee";
        return false;
    }
    if (options_.fixture != "m2" && options_.fixture != "skirmish" && options_.ai.has_value()) {
        ai_flag_ignored_ = true;
        godot::UtilityFunctions::printerr(godot::String(
            ("--eawr-live-ai has no effect with --eawr-live-session " + options_.fixture + "; it only runs the AI"
             + " of m2 or skirmish starts").c_str()));
    }
    if (!(options_.fixture == "replay" || options_.fixture == "melee" ? prepare_replay(filesystem, catalog, map_path, failure)
                                                                      : prepare_m2(filesystem, catalog, failure))) {
        return false;
    }
    // #427: a type with a DEFEND ability composes its SHIELD sub-object as its shield shell.
    std::map<tactical::PlayerId, std::string> scoring_factions;
    for (const auto& player : setup_->players) scoring_factions.emplace(player.player_id, player_faction(player.player_id));
    auto scoring = BattleScoring::create(filesystem, catalog, *tables_, std::move(scoring_factions), std::string(map_path));
    if (scoring) scoring_ = std::move(scoring).value();
    else scoring_failure_ = core::format_diagnostic(scoring.error());
    if (!scoring_failure_.empty()) godot::UtilityFunctions::printerr(godot::String(scoring_failure_.c_str()));
    for (SpacePopulation::Options::PlacedShip& ship : placed_ships_) {
        const auto type = std::find_if(tables_->units.begin(), tables_->units.end(),
                                       [&](const units::UnitType& entry) { return entry.id == ship.object_id; });
        ship.defend_shell = type != tables_->units.end()
            && std::any_of(type->abilities.begin(), type->abilities.end(),
                           [](const units::Ability& ability) { return ability.type == "DEFEND"; });
    }
    // #76 AB-31 (UA-06): a type with SPOILER_LOCK plays its DEPLOY clip when the ability switches
    // on and UNDEPLOY when it switches off. Retail draws among several variants; the X-wing has one.
    const auto exists = [&](const std::string& path) { return static_cast<bool>(filesystem.stat(path)); };
    // FW-30: capture-point copies use neutral IDLE art, independent of their live owner.
    for (const auto& type : tables_->units) {
        if (!type.capture_point || !type.last_state_visible_under_fow) continue;
        if (!neutral_fog_colour_) { failure = "fog capture-point copy: neutral faction colour is unavailable"; return false; }
        auto object = catalog.resolve(type.id, data::Category::game_object);
        const auto clips = animation::model_clip_paths(type.model_path, animation::clip_types::idle, exists,
            object ? tag_text(object.value(), "Space_Model_Anim_Override_Name") : std::string{});
        const auto index = animation::death_clip_variant(clips.size(), type.neutral_fog_animation_index, 0);
        if (!index) continue; // No matching native clip: preserve the copied pose.
        auto model = assets::load_model(filesystem, type.model_path);
        auto clip = assets::load_animation(filesystem, clips[*index]);
        if (!model || !clip) { failure = "fog capture-point copy: neutral animation asset is unreadable"; return false; }
        auto player = animation::Player::create(model.value(), &clip.value());
        if (!player) { failure = core::format_diagnostic(player.error()); return false; }
        auto pose = player.value().sample({});
        if (!pose) { failure = core::format_diagnostic(pose.error()); return false; }
        for (auto& bone : pose.value().bones) {
            if (bone.visible) continue;
            for (auto* matrix : {&bone.skin_asset, &bone.model_asset}) {
                for (std::size_t component = 0; component < 12; ++component) (*matrix)[component] = 0.0F;
            }
        }
        neutral_fog_poses_.emplace(skirmish::type_id(type.id), std::move(pose.value().bones));
    }
    const auto prepare_sfoil_clips = [&] {
        for (SpacePopulation::Options::PlacedShip& ship : placed_ships_) {
            const auto type = std::find_if(tables_->units.begin(), tables_->units.end(),
                                           [&](const units::UnitType& entry) { return entry.id == ship.object_id; });
            if (!ship.clip.empty() || type == tables_->units.end()
                || std::none_of(type->abilities.begin(), type->abilities.end(),
                                [](const units::Ability& ability) { return ability.type == "SPOILER_LOCK"; })) {
                continue;
            }
            auto object = catalog.resolve(ship.object_id, data::Category::game_object);
            if (!object) continue;
            const std::string model = space_model_path(object.value());
            const std::string anim_override = tag_text(object.value(), "Space_Model_Anim_Override_Name");
            const auto deploy = animation::model_clip_paths(model, animation::clip_types::deploy, exists, anim_override);
            const auto undeploy = animation::model_clip_paths(model, animation::clip_types::undeploy, exists, anim_override);
            if (deploy.empty() || undeploy.empty()) continue;
            ship.clip = deploy.front();
            ship.alternate_clip = undeploy.front();
        }
    };
    prepare_sfoil_clips();
    prepare_clips(filesystem, catalog);
    prepare_launch_slots(filesystem, catalog);
    // #614: a squadron a hangar launches later flies in a launch slot, which the first pass
    // did not see; without the clips the slot's craft never plays DEPLOY or UNDEPLOY.
    prepare_sfoil_clips();
    // #424: the setup's squadrons select and order as one unit, their team container.
    if (setup_) {
        for (const tactical::Squadron& squadron : setup_->squadrons) register_squadron(squadron, 0);
    }
    for (const ScriptedInput& input : options_.inputs) {
        // #518: an icon may be a squadron a spawner launches later, which the start does not hold;
        // a target that never shows is logged as not on screen.
        if (input.unit && !input.icon && !ship_of_entity_.contains(*input.unit)) {
            failure = "--eawr-live-input: unit " + std::to_string(*input.unit) + " is not a unit of the start";
            return false;
        }
    }
    // --eawr-live-late-orders: a unit the start does not hold (bought or launched later) has an ID
    // above every start unit's; an order on one is given as the local player, whose purchases they are.
    sim::EntityId last_start_unit = 0;
    for (const auto& [entity, ship] : ship_of_entity_) last_start_unit = std::max(last_start_unit, entity);
    for (const ScheduledOrder& order : options_.orders) {
        const auto late = [&](const sim::EntityId id) {
            return ship_of_entity_.contains(id) || (options_.late_orders && id > last_start_unit);
        };
        const auto stranger = std::find_if(order.more.begin(), order.more.end(),
            [&](const sim::EntityId id) { return !late(id); });
        if (!late(order.unit) || stranger != order.more.end()) {
            failure = "--eawr-live-order: unit " + std::to_string(stranger != order.more.end() ? *stranger : order.unit)
                + " is not a unit of the start";
            return false;
        }
        if (order.target != 0 && !ship_of_entity_.contains(order.target)) {
            failure = "--eawr-live-order: target " + std::to_string(order.target) + " is not a unit of the start";
            return false;
        }
    }
    // A driven frame shows the tick frame * step; a capture tick between two frames would be
    // drawn at a later tick than its file name says (#316 review 3).
    if (options_.stall && options_.real_time) {
        failure = "--eawr-live-stall is for driven captures";
        return false;
    }
    if (!options_.real_time) {
        for (const std::uint64_t tick : options_.capture_ticks) {
            // Past the stall a frame shows its step's tick plus the skipped ticks.
            std::uint64_t stepped = tick;
            if (options_.stall && (options_.stall->start || tick > options_.stall->tick)) {
                // The first frame presents tick <ticks> after a start stall.
                if (options_.stall->start ? tick < options_.stall->ticks
                                          : tick <= options_.stall->tick + options_.stall->ticks) {
                    failure = "--eawr-live-capture-ticks: tick " + std::to_string(tick) + " falls in the --eawr-live-stall skip";
                    return false;
                }
                stepped = tick - options_.stall->ticks;
            }
            const double frames = std::round(static_cast<double>(stepped) / options_.ticks_per_frame);
            if (std::abs(frames * options_.ticks_per_frame - static_cast<double>(stepped)) > capture_tolerance) {
                std::ostringstream message;
                message << "--eawr-live-capture-ticks: tick " << tick << " falls between frames at --eawr-live-step "
                        << options_.ticks_per_frame << "; a capture tick must be a multiple of the step";
                failure = message.str();
                return false;
            }
        }
    }
    return true;
}

bool LiveSessionView::start(std::string& failure) {
    core::load_profile::Scope load_scope(core::load_profile::Phase::session);
    if (!setup_ || !content_) {
        failure = "the live session was not prepared";
        return false;
    }
    platform::LiveSession::Options session_options;
    session_options.workers = options_.workers.value_or(platform::LiveSession::game_worker_count());
    session_options.pacing = options_.real_time ? platform::LiveSession::Pacing::real_time
                                                : platform::LiveSession::Pacing::driven;
    session_options.target_rate = time_.target_rate(); // #459 TM-01
    session_options.initially_paused = true;
    // UI-07: the local player's one scheduler, taken on the simulation thread right before
    // each step with its own keys. The session starts at tick 0.
    scheduler_ = std::make_unique<ui::CommandScheduler>(player_, 0, 0);
    order_input_ = std::make_unique<ui::OrderInput>(*scheduler_);
    session_options.scripts = ai_scripts_;
    // #494: the local player's fog cells ride along with each tick for the fog in the world.
    if (content_->fog) session_options.fog_player = player_;
    session_options.command_source = [scheduler = scheduler_.get(), fault = options_.fault_tick,
                                      replay = replay_ ? &*replay_ : nullptr](const std::uint64_t next_tick) {
        if (fault && next_tick == *fault) {
            throw std::runtime_error("--eawr-live-fault-tick injected a fault before tick " + std::to_string(next_tick));
        }
        // A replay session's recorded commands run at their ticks with their keys, before the
        // local player's.
        std::vector<tactical::PlayerCommand> commands;
        if (replay != nullptr) {
            for (const tactical::PlayerCommand& command : replay->commands) {
                if (command.key.tick == next_tick) commands.push_back(command);
            }
        }
        auto taken = scheduler->take(next_tick);
        commands.insert(commands.end(), std::make_move_iterator(taken.begin()), std::make_move_iterator(taken.end()));
        return commands;
    };
    auto live = platform::LiveSession::start(*setup_, content_->sensors, content_->durability, content_->motion,
                                             content_->combat, session_options, victory_, content_->fog, content_->abilities,
                                             economy_);
    if (!live) {
        failure = "live session: " + core::format_diagnostic(live.error());
        return false;
    }
    session_ = std::move(live).value();
    // The debug hook: each order as its unit's owner would give it, at its tick.
    for (const ScheduledOrder& order : options_.orders) {
        const auto owner = owner_of_entity_.find(order.unit);
        platform::LiveOrder live_order{owner != owner_of_entity_.end() ? owner->second : player_, {order.unit},
                                       tactical::StopPayload{}, order.tick};
        live_order.units.insert(live_order.units.end(), order.more.begin(), order.more.end());
        if (order.kind == tactical::OrderKind::move) live_order.payload = tactical::MovePayload{order.point};
        if (order.kind == tactical::OrderKind::face) live_order.payload = tactical::FacePayload{order.point};
        if (order.kind == tactical::OrderKind::damage) {
            live_order.payload = tactical::DamagePayload{order.amount, order.hardpoint};
        }
        if (order.kind == tactical::OrderKind::attack) live_order.payload = tactical::AttackPayload{order.target};
        if (order.kind == tactical::OrderKind::ability) {
            tactical::AbilityPayload ability{order.ability, order.action};
            ability.target = order.target; // #561: none unless the order names one
            if (order.ability == tactical::AbilityKind::weaken_enemy && order.action == tactical::AbilityAction::activate)
                ability.position = order.point;
            live_order.payload = ability;
        }
        if (order.kind == tactical::OrderKind::attack_move) {
            live_order.payload = tactical::AttackMovePayload{order.point, order.target};
        }
        if (order.kind == tactical::OrderKind::guard) live_order.payload = tactical::GuardPayload{order.point, order.target};
        session_->submit(std::move(live_order));
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
