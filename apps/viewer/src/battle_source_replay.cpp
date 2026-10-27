#include "battle_source_internal.hpp"
#include "battle_source_draw.hpp"
#include "live_session_internal.hpp"
#include "skirmish_prepare.hpp"
#include "viewer_path.hpp"
#include "eawr/campaign/standalone_replay.hpp"
#include "eawr/campaign/space_replay_rules.hpp"
#include <fstream>
#include <iterator>
#include <algorithm>
#include <iostream>
#include <utility>

namespace eawr::presentation::godot_backend {
namespace tactical = sim::tactical;
using namespace live_session_detail;

bool BattleSourceState::prepare_native(const live_session::PreparedBattle& battle, std::string& failure) {
    auto& placed_ships_ = draw_bindings_->placed_ships;
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
    for (const units::UnitType& type : battle.tables.units) names.emplace(battle::type_id(type.id), type.id);
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
        ship.yaw_degrees = transform ? static_cast<float>(live::instance_yaw_degrees(transform.value())) : 0.0F;
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
    select_arrivals();
    return true;
}

bool BattleSourceState::bind_replay(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const std::string_view map_path, std::optional<tactical::TacticalReplay> replay,
    std::optional<units::UnitTables> tables, std::string& failure) {
    auto& placed_ships_ = draw_bindings_->placed_ships;
    if (replay->setup.native_scene) {
        auto native = campaign::load_standalone_space_replay(*replay, filesystem, catalog);
        if (!native) { failure = "--eawr-live-replay: " + core::format_diagnostic(native.error()); return false; }
        live_session::PreparedBattle battle{std::move(*replay), std::move(native.value().content),
            std::move(native.value().victory), std::move(native.value().economy),
            std::move(native.value().tables), std::move(native.value().start)};
        colorization_enabled_ = false;
        return prepare_native(battle, failure);
    }
    if (!tables) tables = load_start_tables(filesystem, catalog, failure, {});
    if (!tables) return false;
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
    std::vector<core::Diagnostic> skipped; // #4589
    auto content = skirmish::session_content(*tables, humans, {}, units::ContentErrors::skip_object, &skipped);
    for (const auto& warning : skipped) {
        std::cerr << "EAWR live session content: " << core::format_diagnostic(warning) << std::endl;
    }
    if (!content) {
        failure = "live session content: " + core::format_diagnostic(content.error());
        return false;
    }
    // #4589: a recording that places a skipped type would replay it unarmed; refuse it like a live start.
    if (auto placed = skirmish::refuse_skipped_setup(setup, skipped); !placed) {
        failure = "live session start: " + core::format_diagnostic(placed.error());
        return false;
    }
    std::map<tactical::TypeId, std::string> names;
    for (const units::UnitType& type : tables->units) names.emplace(battle::type_id(type.id), type.id);
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
                    faction_colours.emplace(battle::faction_id(slot.faction), inputs.value().lobby_colours[slot.slot - 1U].rgb);
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
            if (!setup.campaign && start && start.value().setup.players == setup.players && start.value().setup.units == setup.units) {
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
    if (pad_replay || setup.match_policy || selected_map_replay || setup.skirmish || setup.campaign) {
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
        if (setup.campaign) {
            tactical::TacticalContent bound;
            inputs.value().tables = &*tables;
            if (auto rules = campaign::bind_space_rules(setup, inputs.value(), bound); !rules) {
                failure = "campaign replay rules: " + core::format_diagnostic(rules.error());
                return false;
            }
            economy_ = std::move(bound.economy);
            victory_ = std::move(bound.victory);
        } else {
            auto economy = skirmish::economy_rules(start.value(), inputs.value(), *tables);
            if (!economy) { failure = core::format_diagnostic(economy.error()); return false; }
            economy_ = std::move(economy).value();
        }
        // WBP-29: focused fleets assign different IDs; map placement/type supplies model metadata.
        std::vector<battle::StartUnit> replay_units;
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
                battle::StartUnit visual;
                visual.type = name->second;
                visual.role = battle::UnitRole::fleet;
                replay_units.push_back(std::move(visual));
            }
            replay_units.back().state = state;
        }
        start.value().units = std::move(replay_units);
        start.value().setup = setup;
        start.value().launches.clear();
        start.value().arrivals = battle::plan_start_arrivals(start.value(), *tables);
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
        ship.yaw_degrees = transform ? static_cast<float>(live::instance_yaw_degrees(transform.value())) : 0.0F;
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
                [&](const battle::StartUnit& entry) { return entry.state.entity_id == unit.entity_id; });
            if (authored != start_->units.end()
                && (authored->role == battle::UnitRole::placed || authored->role == battle::UnitRole::map_object)) {
                session_records_.push_back(authored->record);
                ship.colour_record = authored->record;
                if (authored->role == battle::UnitRole::map_object) {
                    auto object = catalog.resolve(authored->type, data::Category::game_object);
                    if (!object) { failure = core::format_diagnostic(object.error()); return false; }
                    if (!live::map_prop_fog_bound(tag_text(object.value(), "Behavior"),
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
    if (!setup.campaign) {
        victory_ = setup.skirmish && start_ ? skirmish::victory_rules(*start_, *tables)
            : skirmish::victory_rules(setup, *tables, humans, options_.skirmish.victory_condition.value_or(selected_victory));
    }
    replay_ = std::move(replay).value();
    content_ = std::move(content).value();
    tables_ = std::move(*tables);
    select_arrivals();
    return true;
}

std::optional<tactical::TacticalReplay> BattleSourceState::read_replay(std::string& failure) {
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
}

std::optional<tactical::TacticalReplay> BattleSourceState::build_melee(const vfs::Vfs& filesystem,
    const data::Catalog& catalog, std::optional<units::UnitTables>& tables, std::string& failure) {
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

bool ReplaySource::prepare(BattlePreparation& prepared, const vfs::Vfs& files,
    const data::Catalog& catalog, const std::string_view map_path, std::string& failure) const {
    auto replay = prepared.state().read_replay(failure);
    if (!replay) return false;
    return prepared.state().bind_replay(files, catalog, map_path, std::move(replay), {}, failure);
}

bool MeleeSource::prepare(BattlePreparation& prepared, const vfs::Vfs& files,
    const data::Catalog& catalog, const std::string_view map_path, std::string& failure) const {
    std::optional<units::UnitTables> tables;
    auto replay = prepared.state().build_melee(files, catalog, tables, failure);
    if (!replay) return false;
    return prepared.state().bind_replay(files, catalog, map_path, std::move(replay), std::move(tables), failure);
}

BattleAiCapability ReplaySource::ai_capability() const noexcept { return BattleAiCapability::recorded; }
BattleAiCapability MeleeSource::ai_capability() const noexcept { return BattleAiCapability::recorded; }

} // namespace eawr::presentation::godot_backend
