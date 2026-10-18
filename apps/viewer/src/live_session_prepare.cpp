#include "eawr/core/load_profile.hpp"
#include "eawr/campaign/standalone_replay.hpp"
#include "live_session_view.hpp"
#include "skirmish_prepare.hpp"
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
// MAT-08: the table types a demo map's placed forces name that the tables do not hold yet.
[[nodiscard]] std::set<std::string> missing_placed_types(const skirmish::StartInputs& inputs,
                                                         const units::UnitTables& tables) {
    std::set<std::string> missing;
    for (const auto& type : skirmish::placed_force_types(inputs.placements)) {
        if (!tables.find(type)) missing.insert(type);
    }
    return missing;
}
} // namespace

bool LiveSessionView::prepare_m2(const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string& failure) {
    skirmish::Fixture fixture = skirmish::m2_fixture();
    const bool attract = options_.fixture == "attract";
    if (options_.fixture == "skirmish") {
        auto selected = skirmish::fixture_from_options(options_.skirmish, filesystem, catalog);
        if (!selected) {
            failure = "live skirmish options: " + core::format_diagnostic(selected.error());
            return false;
        }
        fixture = std::move(selected).value();
    } else if (attract) {
        // MAT-08, MAT-09: the demo map's own forces; no lobby, no victory condition.
        skirmish::PlacedForces forces;
        forces.ai = options_.attract_ai;
        auto selected = skirmish::placed_fixture(*options_.skirmish.map, std::move(forces),
                                                 options_.skirmish.seed.value_or(0U), filesystem, catalog);
        if (!selected) {
            failure = "live attract options: " + core::format_diagnostic(selected.error());
            return false;
        }
        fixture = std::move(selected).value();
    }
    std::optional<PreparedSkirmish> prepared;
    if (options_.fixture == "skirmish") {
        auto result = prepare_skirmish(fixture, filesystem, catalog);
        if (!result) { failure = result.error().message; return false; }
        prepared.emplace(std::move(result).value());
    }
    auto tables = prepared ? std::optional<units::UnitTables>(std::move(prepared->tables))
        : load_start_tables(filesystem, catalog, failure, fixture.map);
    if (!tables) return false;
    auto inputs = prepared ? core::Result<skirmish::StartInputs>::success(std::move(prepared->inputs))
        : skirmish::read_start_inputs(fixture, filesystem, catalog, *tables);
    if (!inputs) { failure = "live session start inputs: " + core::format_diagnostic(inputs.error()); return false; }
    inputs.value().tables = &*tables;
    if (attract) {
        const auto missing = missing_placed_types(inputs.value(), *tables);
        if (!missing.empty()) {
            auto extended = load_start_tables(filesystem, catalog, failure, fixture.map, missing);
            if (!extended) return false;
            tables = std::move(extended); inputs.value().tables = &*tables;
        }
    }
    auto start = prepared ? core::Result<skirmish::SkirmishStart>::success(std::move(prepared->start))
        : skirmish::build_start(fixture, inputs.value());
    if (!start) { failure = "live session start: " + core::format_diagnostic(start.error()); return false; }
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
    if (auto bound = skirmish::bind_difficulty(content.value(), start.value(), inputs.value()); !bound) {
        failure = "live session difficulty: " + core::format_diagnostic(bound.error());
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
        content.value().map_box = skirmish::map_box(inputs.value());
    }
    if (options_.ai.value_or(true)) {
        // #79: the retail freestore of each AI player, from the mounted FoC scripts.
        core::load_profile::Scope lua_scope(core::load_profile::Phase::lua);
        std::map<tactical::PlayerId, std::string> factions;
        for (const auto& player : start.value().players) factions.emplace(player.player.player_id, player.faction);
        auto scoring = BattleScoring::create(filesystem, catalog, *tables, std::move(factions), fixture.map);
        if (!scoring) {
            failure = "authoritative scoring: " + core::format_diagnostic(scoring.error());
            return false;
        }
        scoring_ = std::move(scoring).value();
        auto ai = platform::live_ai(start.value(), inputs.value(), *tables, filesystem, scoring_);
        if (!ai) {
            failure = "live session AI scripts: " + core::format_diagnostic(ai.error());
            return false;
        }
        ai_scripts_ = std::move(ai.value().scripts);
        ai_players_ = std::move(ai.value().players);
    }
    start_ = std::move(start).value();
    if (attract) {
        // MAT-08, MAT-17: a demo battle is not a recorded skirmish. Its players hold no lobby slots
        // and it has no victory condition, which the skirmish recording metadata (SKSU) refuses, so
        // the setup carries the match identity alone and the session is never recorded as a match.
        setup_ = start_->setup;
        setup_->match_identity = start_->identity;
    } else {
        setup_ = skirmish::recording_setup(fixture, *start_);
    }
    if (options_.lan && !attract) *setup_ = lan_recording_setup(fixture, *start_);
    setup_->queue_identities = true; // PU-17/63.
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
    // MAT-10: a demo map has no local player; the view is only a spectator of its first player.
    const auto first = start_->players.empty() ? 1U : start_->players.front().player.player_id;
    player_ = options_.player.value_or(human != start_->players.end() ? human->player.player_id
                                                                      : (attract ? first : 1U));
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
        if (unit.role == skirmish::UnitRole::placed) {
            // MAT-08: a demo map's force is the map's own record, owned by the session now.
            session_records_.push_back(unit.record);
            ship.colour_record = unit.record;
        } else if (unit.role == skirmish::UnitRole::map_object) {
            // The map draws this record's object; the session owns it now. Its team colour
            // stays the one the map's owner gives it.
            session_records_.push_back(unit.record);
            ship.colour_record = unit.record;
            // FW-24: map placements of any XML type without either fog behavior
            // draw independently of sensor contact, including placed derelict hulls.
            auto object = catalog.resolve(unit.type, data::Category::game_object);
            if (!object) { failure = core::format_diagnostic(object.error()); return false; }
            if (!space::map_prop_fog_bound(tag_text(object.value(), "Behavior"),
                                           tag_text(object.value(), "SpaceBehavior"))) {
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

bool LiveSessionView::prepare_borrowed(std::string& failure) {
    return prepare_native(*options_.borrowed_battle, failure);
}

bool LiveSessionView::prepare_native(const PreparedBattle& battle, std::string& failure) {
    const auto& setup = battle.replay.setup;
    if (units::content_identity(battle.tables) != setup.content_identity) {
        failure = "campaign battle presentation: prepared unit tables differ from the native setup";
        return false;
    }
    const auto local = options_.player.value_or(setup.players.empty() ? 1U : setup.players.front().player_id);
    if (std::none_of(setup.players.begin(), setup.players.end(),
            [&](const tactical::Player& player) { return player.player_id == local; })) {
        failure = "campaign battle presentation: local player is absent from the native setup";
        return false;
    }
    std::map<tactical::TypeId, std::string> names;
    for (const units::UnitType& type : battle.tables.units) names.emplace(skirmish::type_id(type.id), type.id);
    // Native start metadata also names map objects that are outside the unit tables.
    if (battle.start) for (const auto& unit : battle.start->units) names.emplace(unit.state.type_id, unit.type);
    for (const auto& unit : setup.units) {
        const auto name = names.find(unit.type_id);
        if (name == names.end()) {
            failure = "campaign battle presentation: unit " + std::to_string(unit.entity_id) + " has no prepared type";
            return false;
        }
        SpacePopulation::Options::PlacedShip ship;
        ship.object_id = name->second;
        ship.position = {to_float(unit.position.x), to_float(unit.position.y), to_float(unit.position.z)};
        auto transform = sim::math::to_matrix(unit.rotation, unit.position);
        ship.yaw_degrees = transform ? static_cast<float>(space::instance_yaw_degrees(transform.value())) : 0.0F;
        ship.live_entity = unit.entity_id;
        if (battle.start) {
            const auto owner = std::find_if(battle.start->players.begin(), battle.start->players.end(),
                [&](const auto& player) { return player.player.player_id == unit.owner; });
            if (owner != battle.start->players.end() && owner->colour) ship.team_colour = owner->colour->rgb;
        }
        ship_of_entity_.emplace(unit.entity_id, placed_ships_.size());
        owner_of_entity_.emplace(unit.entity_id, unit.owner);
        placed_ships_.push_back(std::move(ship));
    }
    for (const auto& player : setup.players) team_of_player_[player.player_id] = player.team_id;
    player_ = local;
    setup_ = setup;
    replay_ = battle.replay;
    tables_ = battle.tables;
    content_ = battle.content;
    victory_ = battle.victory;
    economy_ = battle.economy;
    start_ = battle.start;
    return true;
}

bool LiveSessionView::prepare_replay(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const std::string_view map_path, std::string& failure) {
    // Campaign maps need not declare a skirmish lobby capacity. The native child
    // already prepared their actors and rules; presentation must not rebuild them.
    if (options_.borrowed_battle) return prepare_borrowed(failure);
    std::optional<units::UnitTables> tables;
    auto replay = [&]() -> std::optional<tactical::TacticalReplay> {
        if (options_.borrowed_battle) return options_.borrowed_battle->replay;
        if (options_.fixture == "melee") {
            tables = load_start_tables(filesystem, catalog, failure, {});
            if (!tables) return std::nullopt;
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
    if (replay->setup.native_scene) {
        auto native = campaign::load_standalone_space_replay(*replay, filesystem, catalog);
        if (!native) { failure = "--eawr-live-replay: " + core::format_diagnostic(native.error()); return false; }
        PreparedBattle battle{std::move(*replay), std::move(native.value().content),
            std::move(native.value().victory), std::move(native.value().economy),
            std::move(native.value().tables), std::move(native.value().start)};
        colorization_enabled_ = false;
        return prepare_native(battle, failure);
    }
    if (!tables) tables = load_start_tables(filesystem, catalog, failure, {});
    if (!tables) return false;
    if (options_.borrowed_battle) tables = options_.borrowed_battle->tables;
    const tactical::TacticalSetup& setup = replay->setup;
    const std::string replay_map = setup.skirmish ? setup.skirmish->map : std::string(map_path);
    if (units::content_identity(*tables) != replay.value().setup.content_identity) {
        // WHZ-01: live recordings include the selected map's authored hazard
        // profiles. Pinned fixtures retain the narrower M2 closure above.
        auto selected = load_start_tables(filesystem, catalog, failure, replay_map);
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
        auto recorded = skirmish::replay_fixture(selected.value(), inputs.value(), replay->setup, false);
        if (!recorded) { failure = core::format_diagnostic(recorded.error()); return false; }
        inputs = skirmish::read_start_inputs(recorded.value(), filesystem, catalog, *tables);
        if (!inputs) { failure = core::format_diagnostic(inputs.error()); return false; }
        const auto missing = missing_start_types(recorded.value(), inputs.value(), *tables);
        if (!missing.empty()) {
            auto extended = load_start_tables(filesystem, catalog, failure, replay_map, missing);
            if (!extended) return false;
            tables = std::move(extended);
        }
    }
    if (setup.match_identity) {
        skirmish::FixtureOptions options;
        options.map = replay_map;
        auto selected = skirmish::fixture_from_options(options, filesystem, catalog);
        if (!selected) { failure = core::format_diagnostic(selected.error()); return false; }
        auto inputs = skirmish::read_start_inputs(selected.value(), filesystem, catalog, *tables);
        if (!inputs) { failure = core::format_diagnostic(inputs.error()); return false; }
        if (auto valid = skirmish::validate_replay_identity(setup, inputs.value()); !valid) {
            failure = core::format_diagnostic(valid.error());
            return false;
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
                content.value().map_box = skirmish::map_box(inputs.value());
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
                content.value().map_box = skirmish::map_box(inputs.value());
            }
        }
        auto start = skirmish::build_start(replay_fixture, inputs.value());
        if (!start) { failure = core::format_diagnostic(start.error()); return false; }
        // WTA-46 (#735): the recorded slot levels rebuild the live session's difficulty scales.
        if (auto bound = skirmish::bind_difficulty(content.value(), start.value(), inputs.value()); !bound) {
            failure = "--eawr-live-replay: " + core::format_diagnostic(bound.error());
            return false;
        }
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
        // Map records represented by replay actors belong to the live snapshot, just
        // as in prepare_skirmish. Leaving their static art admitted draws a second,
        // differently tinted model at the same position and makes the visible tint
        // depend on mesh/material draw ordering.
        if (start_) {
            const auto authored = std::find_if(start_->units.begin(), start_->units.end(),
                [&](const skirmish::StartUnit& entry) { return entry.state.entity_id == unit.entity_id; });
            if (authored != start_->units.end()
                && (authored->role == skirmish::UnitRole::placed || authored->role == skirmish::UnitRole::map_object)) {
                session_records_.push_back(authored->record);
                ship.colour_record = authored->record;
                if (authored->role == skirmish::UnitRole::map_object) {
                    auto object = catalog.resolve(authored->type, data::Category::game_object);
                    if (!object) { failure = core::format_diagnostic(object.error()); return false; }
                    if (!space::map_prop_fog_bound(tag_text(object.value(), "Behavior"),
                                                   tag_text(object.value(), "SpaceBehavior"))) {
                        unfogged_map_props_.push_back(unit.entity_id);
                    }
                }
            }
        }
        ship_of_entity_.emplace(unit.entity_id, placed_ships_.size());
        owner_of_entity_.emplace(unit.entity_id, unit.owner);
        placed_ships_.push_back(std::move(ship));
    }
    std::sort(unfogged_map_props_.begin(), unfogged_map_props_.end());
    setup_ = setup;
    victory_ = setup.skirmish && start_ ? skirmish::victory_rules(*start_, *tables)
        : skirmish::victory_rules(setup, *tables, humans, options_.skirmish.victory_condition.value_or(selected_victory));
    replay_ = std::move(replay).value();
    content_ = std::move(content).value();
    tables_ = std::move(*tables);
    if (options_.borrowed_battle) {
        content_ = options_.borrowed_battle->content;
        victory_ = options_.borrowed_battle->victory;
        economy_ = options_.borrowed_battle->economy;
        start_ = options_.borrowed_battle->start;
    }
    return true;
}

bool LiveSessionView::prepare(const vfs::Vfs& filesystem, const data::Catalog& catalog, const std::string_view map_path,
                              std::string& failure) {
    shield_flash_constants_ = presentation_constants::load_flash(filesystem);
    deployment_facing_.reset(); // WR-X01: per local player, per battle
    core::load_profile::Scope load_scope(core::load_profile::Phase::session);
    const auto minimap_settings = ui::minimap_settings(filesystem);
    nebula_colour_ = minimap_settings.nebula;
    if (const auto neutral = ui::faction_colour(minimap_settings, "Neutral")) {
        neutral_fog_colour_ = std::array<float, 3>{neutral->r / 255.0F, neutral->g / 255.0F, neutral->b / 255.0F};
    }
    faction_colours_ = minimap_settings;
    // #3076: FoC enables unit colorization for the skirmish game mode only; a campaign battle runs without it.
    colorization_enabled_ = !options_.borrowed_battle;
    const skirmish::Fixture& fixture = skirmish::m2_fixture();
    if (!options_.borrowed_battle && (options_.fixture == "replay") != !options_.replay_input.empty()) {
        failure = "--eawr-live-replay goes with --eawr-live-session replay, which needs it";
        return false;
    }
    if ((options_.fixture == "m2" || options_.fixture == "melee") && lower_path(map_path) != lower_path(fixture.map)) {
        failure = "--eawr-live-session " + options_.fixture + " runs on " + fixture.map;
        return false;
    }
    const bool demo = options_.fixture == "attract";
    if (options_.fixture != "skirmish" && !demo
        && (options_.skirmish.map || options_.skirmish.slots || options_.skirmish.seed)) {
        failure = "--eawr-skirmish-* options require --eawr-live-session skirmish";
        return false;
    }
    if (options_.skirmish.victory_condition && options_.fixture != "skirmish" && options_.fixture != "replay") {
        failure = "--eawr-skirmish-victory requires a skirmish start or replay";
        return false;
    }
    if (demo && options_.skirmish.slots) {
        failure = "a demo map's start has no lobby slots";
        return false;
    }
    if (options_.fixture == "skirmish" || demo) {
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
    if (options_.fixture != "m2" && options_.fixture != "skirmish" && !demo && options_.ai.has_value()) {
        ai_flag_ignored_ = true;
        godot::UtilityFunctions::printerr(godot::String(
            ("--eawr-live-ai has no effect with --eawr-live-session " + options_.fixture + "; it only runs the AI"
             + " of m2 or skirmish starts").c_str()));
    }
    if (!(options_.fixture == "replay" || options_.fixture == "melee" ? prepare_replay(filesystem, catalog, map_path, failure)
                                                                      : prepare_m2(filesystem, catalog, failure))) {
        return false;
    }
    if (!colorization_enabled_) {
        // #3076: a type's own nonzero No_Colorization_Color wins over its owner faction's.
        const auto read_type = [&](const std::string& id) {
            if (type_no_colorization_.contains(id)) return;
            const auto colour = ui::minimap_type_looks(id, &catalog).no_colorization;
            if (colour && *colour != data::ui::Rgba8{0, 0, 0, 0}) type_no_colorization_.emplace(id, std::array<std::uint8_t, 3>{colour->r, colour->g, colour->b});
        };
        for (const auto& type : tables_->units) read_type(type.id);
        for (auto& ship : placed_ships_) {
            read_type(ship.object_id);
            const auto owner = owner_of_entity_.find(ship.live_entity);
            if (owner != owner_of_entity_.end()) ship.team_colour = model_colour(owner->second, ship.object_id);
        }
    }
    // #427: a type with a DEFEND ability composes its SHIELD sub-object as its shield shell.
    std::map<tactical::PlayerId, std::string> scoring_factions;
    for (const auto& player : setup_->players) scoring_factions.emplace(player.player_id, player_faction(player.player_id));
    if (!scoring_) {
        auto scoring = BattleScoring::create(filesystem, catalog, *tables_, std::move(scoring_factions), std::string(map_path));
        if (scoring) scoring_ = std::move(scoring).value();
        else scoring_failure_ = core::format_diagnostic(scoring.error());
    }
    if (!scoring_failure_.empty()) godot::UtilityFunctions::printerr(godot::String(scoring_failure_.c_str()));
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
    // HCB-12: one reusable authored model per issuer, never a selectable unit.
    if (setup_) for (const auto& rule : setup_->beacon_rules) {
        const auto type = std::find_if(catalog.definitions().begin(), catalog.definitions().end(),
            [&](const auto& value) { return value.category == data::Category::game_object && skirmish::type_id(value.id) == rule.type; });
        if (type == catalog.definitions().end()) { failure = "beacon type is absent from the presentation catalog"; return false; }
        SpacePopulation::Options::PlacedShip ship;
        ship.object_id = type->id;
        ship.live_entity = std::numeric_limits<sim::EntityId>::max() / 16U + rule.player;
        ship.beacon_model = true;
        ship.team_colour = model_colour(rule.player, ship.object_id);
        beacon_ships_.emplace(rule.player, placed_ships_.size());
        placed_ships_.push_back(std::move(ship));
    }
    // BP-22: purchased reinforcements need the same shell as starting ships, including
    // heroes with DEFEND in their second slot. Include their newly prepared arrival slots.
    for (SpacePopulation::Options::PlacedShip& ship : placed_ships_) {
        const auto* type = tables_->find(ship.object_id);
        ship.defend_shell = type != nullptr && !ship.placement_preview
            && std::any_of(type->abilities.begin(), type->abilities.end(),
                           [](const units::Ability& ability) { return ability.type == "DEFEND"; });
        ship.stealth_shell = type != nullptr && !ship.placement_preview
            && std::any_of(type->abilities.begin(), type->abilities.end(),
                           [](const units::Ability& ability) { return ability.type == "STEALTH"; });
    }
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

std::shared_ptr<const LiveSessionView::PreparedBattle> LiveSessionView::prepared_battle() const {
    if (!setup_ || !content_ || !tables_) return {};
    return std::make_shared<const PreparedBattle>(PreparedBattle{
        {*setup_, 0, {}}, *content_, victory_, economy_, *tables_, start_});
}

bool LiveSessionView::start(std::string& failure) {
    core::load_profile::Scope load_scope(core::load_profile::Phase::session);
    if (!setup_ || !content_) {
        failure = "the live session was not prepared";
        return false;
    }
    platform::LiveSession::Options session_options;
    session_options.workers = options_.workers.value_or(platform::LiveSession::game_worker_count());
    session_options.measure_phases = trace_frames_;
    session_options.pacing = options_.real_time ? platform::LiveSession::Pacing::real_time
                                                : platform::LiveSession::Pacing::driven;
    session_options.target_rate = time_.target_rate(); // #459 TM-01
    session_options.initially_paused = true;
    // SND-40: only authored bases opt into the bounded durable presentation journal.
    for (const auto& type : tables_->units) {
        const auto base = [](const std::string& name) {
            const auto value = lower_path(name);
            return value == "dummy_star_base" || value == "dummy_orbital_structure";
        };
        const auto& behavior = type.footprint.hazard;
        if (std::any_of(behavior.behavior.begin(), behavior.behavior.end(), base)
            || std::any_of(behavior.space_behavior.begin(), behavior.space_behavior.end(), base)) {
            session_options.base_attack_types.push_back(skirmish::type_id(type.id));
        }
    }
    std::sort(session_options.base_attack_types.begin(), session_options.base_attack_types.end());
    // UI-07: the local player's one scheduler, taken on the simulation thread right before
    // each step with its own keys. The session starts at tick 0.
    // LAN assigns its agreed future tick after this handoff. A stalled admission
    // visit must not close the producer's current epoch (FD-01 phase order stays).
    std::function<std::uint64_t()> intent_epoch;
    if (options_.lan) intent_epoch = [this] { return session_ ? session_->completed_tick() : 0U; };
    scheduler_ = std::make_shared<ui::CommandScheduler>(player_, 0, 0, std::move(intent_epoch));
    if (options_.lan) options_.lan->set_input_epoch_provider([scheduler = std::weak_ptr(scheduler_)] {
        const auto producer = scheduler.lock();
        return producer ? producer->pending_intent_epoch() : std::nullopt;
    });
    if (options_.lan) options_.lan->set_submission_observer([scheduler = std::weak_ptr(scheduler_)]
        (const std::uint64_t sequence, const std::uint64_t tick) {
        if (const auto producer = scheduler.lock()) producer->note_execution(sequence, tick);
    });
    if (options_.lan) options_.lan->set_discard_observer([scheduler = std::weak_ptr(scheduler_)](std::uint64_t sequence) {
        if (const auto producer = scheduler.lock()) producer->present_result(sequence);
    });
    if (options_.lan_acceptance && options_.lan) scheduler_->set_issue_observer(
        [lan = options_.lan](std::uint64_t sequence, std::uint64_t epoch) { lan->note_issue(sequence, epoch); });
    if (options_.lan) input_sink_ = std::make_unique<LanCommandSink>(*scheduler_, options_.lan,
        [this](const core::Diagnostic& error) { recovery_input_feedback_ = error.message; });
    order_input_ = std::make_unique<ui::OrderInput>(input_sink_ ? *input_sink_ : *scheduler_);
    session_options.scripts = ai_scripts_;
    if (options_.lan) session_options.step_driver = options_.lan->driver(*setup_);
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
    if (options_.borrowed_feed) {
        session_ = options_.borrowed_feed;
        session_->configure_notifications(std::move(session_options.base_attack_types));
        session_->command_source(std::move(session_options.command_source));
        return true;
    }
    auto live = platform::LiveSession::start(*setup_, content_->sensors, content_->durability, content_->motion,
                                             content_->combat, session_options, victory_, content_->fog, content_->abilities,
                                             economy_, content_->map_box);
    if (!live) {
        failure = "live session: " + core::format_diagnostic(live.error());
        return false;
    }
    session_ = std::make_shared<platform::StandaloneTacticalFeed>(std::move(live).value());
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
            if ((order.ability == tactical::AbilityKind::fow_reveal_ping
                || order.ability == tactical::AbilityKind::weaken_enemy || order.ability == tactical::AbilityKind::buzz_droids
                    || order.ability == tactical::AbilityKind::corrupt_systems)
                && order.action == tactical::AbilityAction::activate)
                ability.position = order.point;
            live_order.payload = ability;
        }
        if (order.kind == tactical::OrderKind::attack_move) {
            live_order.payload = tactical::AttackMovePayload{order.point, order.target};
        }
        if (order.kind == tactical::OrderKind::guard) live_order.payload = tactical::GuardPayload{order.point, order.target};
        if (!session_->try_submit(std::move(live_order))) return false;
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
