#include "battle_source_internal.hpp"
#include "battle_source_draw.hpp"
#include "live_session_internal.hpp"
#include "skirmish_prepare.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/platform/live_ai.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/presentation/space/live_units.hpp"
#include <set>
#include <algorithm>
#include <utility>

namespace eawr::presentation::godot_backend {
namespace tactical = sim::tactical;
using namespace live_session_detail;

BattleSourceState::BattleSourceState(const live_session::Options& options)
    : options_(options), draw_bindings_(std::make_unique<BattleDrawBindings>()) {}
BattleSourceState::~BattleSourceState() = default;

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

bool BattleSourceState::prepare_local(const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string& failure, const LocalStartKind kind) {
    auto& placed_ships_ = draw_bindings_->placed_ships;
    skirmish::Fixture fixture = skirmish::m2_fixture();
    const bool attract = kind == LocalStartKind::attract;
    if (kind == LocalStartKind::selected) {
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
    if (kind == LocalStartKind::selected) {
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
    auto start = prepared ? core::Result<battle::BattleStart>::success(std::move(prepared->start))
        : skirmish::build_start(fixture, inputs.value());
    if (!start) { failure = "live session start: " + core::format_diagnostic(start.error()); return false; }
    // #76: the ability table's human players are the start's human lobby players, as the victory rules'.
    std::vector<tactical::PlayerId> humans;
    for (const battle::StartPlayer& player : start.value().players) {
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
        [](const battle::StartPlayer& player) { return player.human; });
    // MAT-10: a demo map has no local player; the view is only a spectator of its first player.
    const auto first = start_->players.empty() ? 1U : start_->players.front().player.player_id;
    player_ = options_.player.value_or(human != start_->players.end() ? human->player.player_id
                                                                      : (attract ? first : 1U));
    if (std::none_of(start_->players.begin(), start_->players.end(),
            [&](const battle::StartPlayer& player) { return player.player.player_id == player_; })) {
        failure = "--eawr-live-player " + std::to_string(player_) + " is not a player of the start";
        return false;
    }
    for (const battle::StartPlayer& player : start_->players) {
        team_of_player_[player.player.player_id] = player.player.team_id;
    }
    for (const battle::StartUnit& unit : start_->units) {
        SpacePopulation::Options::PlacedShip ship;
        ship.object_id = unit.type;
        ship.position = {to_float(unit.state.position.x), to_float(unit.state.position.y), to_float(unit.state.position.z)};
        ship.yaw_degrees = to_float(unit.yaw_degrees);
        ship.live_entity = unit.state.entity_id;
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const battle::StartPlayer& player) { return player.player.player_id == unit.state.owner; });
        if (owner != start_->players.end() && owner->colour) ship.team_colour = owner->colour->rgb;
        if (unit.role == battle::UnitRole::placed) {
            // MAT-08: a demo map's force is the map's own record, owned by the session now.
            session_records_.push_back(unit.record);
            ship.colour_record = unit.record;
        } else if (unit.role == battle::UnitRole::map_object) {
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
    select_arrivals();
    return true;
}

LocalSkirmishSource::LocalSkirmishSource(const LocalStartKind kind) : kind_(kind) {}

bool LocalSkirmishSource::prepare(BattlePreparation& prepared, const vfs::Vfs& files,
    const data::Catalog& catalog, std::string_view, std::string& failure) const {
    auto& state = prepared.state();
    return state.prepare_local(files, catalog, failure, kind_);
}

bool AttractSource::prepare(BattlePreparation& prepared, const vfs::Vfs& files,
    const data::Catalog& catalog, std::string_view, std::string& failure) const {
    return prepared.state().prepare_local(files, catalog, failure, LocalStartKind::attract);
}

BattleAiCapability LocalSkirmishSource::ai_capability() const noexcept { return BattleAiCapability::configurable; }
BattleAiCapability AttractSource::ai_capability() const noexcept { return BattleAiCapability::configurable; }

} // namespace eawr::presentation::godot_backend
