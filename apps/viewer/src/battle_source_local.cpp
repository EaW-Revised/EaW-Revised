#include "battle_source_internal.hpp"
#include "battle_source_draw.hpp"
#include "live_session_internal.hpp"
#include "skirmish_prepare.hpp"
#include "viewer_path.hpp"
#include "menu_timeline.hpp"
#include "eawr/scene/model_prefetch.hpp"
#include "eawr/units/simulation_model.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/platform/saved_games.hpp"
#include "eawr/platform/live_ai.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/presentation/live/live_units.hpp"
#include <set>
#include <system_error>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <sstream>
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

// What the attract preparation's stages 1-3 read from the options, copied on the main thread when the stages are built:
// the stages may run on the load worker, which must not read the view's options.
struct AttractInputs final {
    std::string map;
    std::uint64_t seed{};
    std::vector<std::pair<std::uint32_t, std::string>> ai;
    std::vector<std::filesystem::path> model_volumes;
};

// A diagnostics line of the menu timeline. The timeline prints through the engine, so on the load worker (channel.defer)
// the line is kept for the main thread, which prints it after the join; otherwise it is printed at once.
void timeline_note(LoadSideChannel& channel, const char* kind, std::string text) {
    if (channel.defer) channel.add(kind, std::move(text));
    else MenuTimeline::note(kind, text);
}

// The attract load's unit tables with their models decoded ahead (scene::ModelPrefetch, readers decode into the model store
// the loader asks, in the loader's own order).
[[nodiscard]] std::optional<units::UnitTables> load_attract_tables(const AttractInputs& options, LoadSideChannel& channel,
    const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string& failure, const std::string& map,
    const std::set<std::string>& additional_types, const char* stage) {
    const std::atomic<bool>* const cancel = channel.cancel.get();
    std::vector<units::DiskKind> kinds;
    for (const auto& root : options.model_volumes) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(root, error);
        kinds.push_back(error ? units::DiskKind::unknown : units::volume_disk_kind(absolute.root_name().wstring()));
    }
    scene::ModelPrefetchOptions prefetch_options;
    const auto disk = units::slowest_disk(kinds);
    prefetch_options.solid_state = disk == units::DiskKind::solid_state;
    scene::ModelPrefetch prefetch(filesystem, prefetch_options);
    auto tables = load_start_tables(filesystem, catalog, failure, map, additional_types, &prefetch, cancel);
    prefetch.finish();
    const auto stats = prefetch.stats();
    std::ostringstream out;
    out << "stage=" << stage << " disk=" << (disk == units::DiskKind::solid_state ? "solid_state"
        : disk == units::DiskKind::spinning ? "spinning" : "unknown")
        << " readers=" << stats.readers << " readers_peak=" << stats.readers_peak << " planned=" << stats.planned
        << " decoded=" << stats.decoded << " served_ready=" << stats.served_ready << " served_inflight=" << stats.served_inflight
        << " served_unstarted=" << stats.served_unstarted << " unplanned=" << stats.unplanned
        << " unrequested=" << stats.unrequested << " window_bytes=" << stats.window_bytes
        << " held_bytes_peak=" << stats.held_bytes_peak;
    timeline_note(channel, "menu_prefetch", out.str());
    return tables;
}
} // namespace

namespace {
// What the local preparation's stages hand to one another.
struct LocalPrep final {
    skirmish::Fixture fixture = skirmish::m2_fixture();
    std::optional<PreparedSkirmish> prepared;
    std::optional<units::UnitTables> tables;
    std::optional<skirmish::StartInputs> inputs;
    std::optional<battle::BattleStart> start;
    std::vector<core::Diagnostic> skipped;
    std::optional<skirmish::SessionContent> content;
    std::optional<AttractInputs> attract; // attract only; stages 1-3 read these instead of the view's options
};
} // namespace

// The preparation as ordered stages: run back to back they are the former single function; the attract load queues
// them as separate load steps. Each stage reports its own failure text.
std::vector<BattleSource::Stage> BattleSourceState::local_stages(const vfs::Vfs& filesystem, const data::Catalog& catalog,
                                                                const LocalStartKind kind) {
    auto local = std::make_shared<LocalPrep>();
    const bool attract = kind == LocalStartKind::attract;
    const auto channel = load_channel_;
    // Read here, on the calling (main) thread: the stages may run on the load worker.
    const bool timeline = attract && MenuTimeline::enabled();
    if (attract) {
        AttractInputs inputs;
        inputs.map = options_.skirmish.map.value_or(std::string{});
        inputs.seed = options_.skirmish.seed.value_or(0U);
        inputs.ai = options_.attract_ai;
        inputs.model_volumes = options_.model_volumes;
        local->attract = std::move(inputs);
    }
    std::vector<BattleSource::Stage> stages;
    // 1: the fixture and the unit tables.
    // Stages 1-3 of an attract start run on the load worker: they read only `local`, the channel and the filesystem and
    // catalog (immutable once mounted), and write no BattleSourceState member. Stage 1 keeps `this` for the lobby
    // (`selected`) start alone, which is never offloaded.
    stages.push_back([this, &filesystem, &catalog, kind, attract, local, channel](std::string& failure) -> bool {
        auto& fixture = local->fixture;
        // A saved battle the host's queue step already prepared for this very content and save (exact key): take its
        // recorded fixture, tables, inputs and start instead of building them all again.
        const auto& handed = options_.saved_preparation;
        if (kind == LocalStartKind::selected && handed && !handed->consumed && !options_.load_path.empty()
            && handed->filesystem == &filesystem && handed->catalog == &catalog
            && handed->save_path == ViewerPath::utf8(options_.load_path)) {
            handed->consumed = true;
            fixture = std::move(handed->fixture);
            local->prepared.emplace(std::move(handed->prepared));
        } else if (kind == LocalStartKind::selected) {
            auto selected = skirmish::fixture_from_options(options_.skirmish, filesystem, catalog);
            if (!selected) {
                failure = "live skirmish options: " + core::format_diagnostic(selected.error());
                return false;
            }
            fixture = std::move(selected).value();
            if (!options_.load_path.empty()) {
                std::ifstream input(options_.load_path, std::ios::binary);
                if (!input) { failure = "The battle save could not be read."; return false; }
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
                auto saved_setup = platform::skirmish_save_setup(bytes);
                if (!saved_setup) { failure = core::format_diagnostic(saved_setup.error()); return false; }
                std::string reason;
                auto tables = load_start_tables(filesystem, catalog, reason, fixture.map);
                if (!tables) { failure = reason; return false; }
                auto inputs = skirmish::read_start_inputs(fixture, filesystem, catalog, *tables);
                if (!inputs) { failure = core::format_diagnostic(inputs.error()); return false; }
                auto recorded = skirmish::replay_fixture(fixture, inputs.value(), saved_setup.value(), false);
                if (!recorded) { failure = core::format_diagnostic(recorded.error()); return false; }
                fixture = std::move(recorded).value();
            }
        } else if (attract) {
            // MAT-08, MAT-09: the demo map's own forces; no lobby, no victory condition.
            skirmish::PlacedForces forces;
            forces.ai = local->attract->ai;
            auto selected = skirmish::placed_fixture(local->attract->map, std::move(forces),
                                                     local->attract->seed, filesystem, catalog);
            if (!selected) {
                failure = "live attract options: " + core::format_diagnostic(selected.error());
                return false;
            }
            fixture = std::move(selected).value();
        }
        if (kind == LocalStartKind::selected && !local->prepared) {
            auto result = prepare_skirmish(fixture, filesystem, catalog);
            if (!result) { failure = result.error().message; return false; }
            local->prepared.emplace(std::move(result).value());
        }
        local->tables = local->prepared ? std::optional<units::UnitTables>(std::move(local->prepared->tables))
            : attract ? load_attract_tables(*local->attract, *channel, filesystem, catalog, failure, fixture.map, {}, "tables")
                      : load_start_tables(filesystem, catalog, failure, fixture.map);
        return local->tables.has_value();
    });
    // 2: the start inputs (and, for a demo map, the tables of the types its placed forces name).
    stages.push_back([&filesystem, &catalog, attract, local, channel](std::string& failure) -> bool {
        auto& tables = local->tables;
        if (local->prepared) {
            local->inputs = std::move(local->prepared->inputs);
        } else {
            auto inputs = skirmish::read_start_inputs(local->fixture, filesystem, catalog, *tables);
            if (!inputs) { failure = "live session start inputs: " + core::format_diagnostic(inputs.error()); return false; }
            local->inputs = std::move(inputs).value();
        }
        local->inputs->tables = &*tables;
        if (attract) {
            const auto missing = missing_placed_types(*local->inputs, *tables);
            if (!missing.empty()) {
                auto extended = load_attract_tables(*local->attract, *channel, filesystem, catalog, failure, local->fixture.map, missing, "placed");
                if (!extended) return false;
                tables = std::move(extended); local->inputs->tables = &*tables;
            }
        }
        return true;
    });
    // 3: the start, the session content and what binds to them.
    stages.push_back([timeline, local, channel](std::string& failure) -> bool {
        auto& inputs = *local->inputs;
        if (local->prepared) {
            local->start = std::move(local->prepared->start);
        } else {
            auto built = skirmish::build_start(local->fixture, inputs);
            if (!built) { failure = "live session start: " + core::format_diagnostic(built.error()); return false; }
            local->start = std::move(built).value();
        }
        auto& start = *local->start;
        // #76: the ability table's human players are the start's human lobby players, as the victory rules'.
        std::vector<tactical::PlayerId> humans;
        for (const battle::StartPlayer& player : start.players) {
            if (player.lobby && player.human) humans.push_back(player.player.player_id);
        }
        // #4589: FoC loads a game with a bad object in it; a bad unit type is skipped and reported here.
        auto& skipped = local->skipped;
        auto content = skirmish::session_content(*local->tables, humans, {}, units::ContentErrors::skip_object, &skipped);
        for (const auto& warning : skipped) {
            std::cerr << "EAWR live session content: " << core::format_diagnostic(warning) << std::endl;
        }
        if (!content) {
            failure = "live session content: " + core::format_diagnostic(content.error());
            return false;
        }
        if (!skipped.empty()) std::cerr << "EAWR live session content: " << skirmish::content_load_summary(skipped) << std::endl;
        {
            eawr::core::LoadNoteCollector collector;
            for (const auto& note : content.value().load_notes) collector.append(note);
            for (const auto& note : start.load_notes) collector.append(note);
            content.value().load_notes = collector.snapshot();
        }
        skirmish::drop_skipped_starts(start, skipped, &content.value().load_notes);
        skirmish::bind_skipped_types(start, skirmish::skipped_type_ids(skipped));
        if (auto bound = skirmish::bind_difficulty(content.value(), start, inputs); !bound) {
            failure = "live session difficulty: " + core::format_diagnostic(bound.error());
            return false;
        }
        // #495: the map's fog grid. #507's live `--eawr-live-reveal` is a draw-time bypass only
        // (interpolate_units, LiveFogView, the minimap) and must never change what the session itself
        // binds: every session keeps its real sensor table on reveal, so skipping fog here
        // would silently swap visible_to's fog-cell test (V-11 to V-17)
        // for the exact-range one, changing hashes reveal on vs off.
        {
            auto fog = skirmish::fog_rules(inputs);
            if (!fog) {
                failure = "live session fog rules: " + core::format_diagnostic(fog.error());
                return false;
            }
            content.value().fog = fog.value();
            content.value().map_box = skirmish::map_box(inputs);
        }
        if (timeline) {
            // The A/B identity of a load-only change: the tables' content identity and the start's digest.
            const auto hex = [](const std::array<std::uint8_t, 32>& digest) {
                static constexpr char digits[] = "0123456789abcdef";
                std::string text;
                for (const auto byte : digest) { text += digits[byte >> 4U]; text += digits[byte & 15U]; }
                return text;
            };
            timeline_note(*channel, "menu_identity", "tables=" + hex(units::content_identity(*local->tables)) + " units="
                + std::to_string(local->tables->units.size()) + " start=" + hex(start.identity.sha256) + " skipped="
                + std::to_string(skipped.size()));
        }
        local->content = std::move(content).value();
        return true;
    });
    // 4: the AI scripts and the authoritative scoring.
    stages.push_back([this, &filesystem, &catalog, local](std::string& failure) -> bool {
        if (!options_.ai.value_or(true)) return true;
        // #79: the retail freestore of each AI player, from the mounted FoC scripts.
        core::load_profile::Scope lua_scope(core::load_profile::Phase::lua);
        auto& start = *local->start;
        std::map<tactical::PlayerId, std::string> factions;
        for (const auto& player : start.players) factions.emplace(player.player.player_id, player.faction);
        auto scoring = BattleScoring::create(filesystem, catalog, *local->tables, std::move(factions), local->fixture.map);
        if (!scoring) {
            failure = "authoritative scoring: " + core::format_diagnostic(scoring.error());
            return false;
        }
        scoring_ = std::move(scoring).value();
        auto ai = platform::live_ai(start, *local->inputs, *local->tables, filesystem, scoring_);
        if (!ai) {
            failure = "live session AI scripts: " + core::format_diagnostic(ai.error());
            return false;
        }
        for (const auto& note : ai.value().load_notes) std::cerr << core::format_diagnostic(note) << std::endl;
        ai_scripts_ = std::move(ai.value().scripts);
        ai_players_ = std::move(ai.value().players);
        return true;
    });
    // 5: the setup, the rules and the placed ships.
    stages.push_back([this, &catalog, attract, local](std::string& failure) -> bool {
        auto& fixture = local->fixture;
        auto& tables = local->tables;
        auto& inputs = *local->inputs;
        auto& placed_ships_ = draw_bindings_->placed_ships;
        start_ = std::move(*local->start);
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
        content_ = std::move(local->content);
        victory_ = skirmish::victory_rules(*start_, tables.value());
        // #530: the skirmish economy (credits, the station build queues and hyperspace arrival).
        auto economy = skirmish::economy_rules(*start_, inputs, tables.value());
        if (!economy) {
            failure = "live session economy: " + core::format_diagnostic(economy.error());
            return false;
        }
        economy_ = std::move(economy).value();
        {
            std::vector<std::string> dropped;
            skirmish::drop_skipped_menu_options(economy_, skirmish::skipped_type_ids(local->skipped), &dropped);
            for (const auto& line : dropped) std::cerr << "EAWR live session content: " << line << std::endl;
        }
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
                if (!live::map_prop_fog_bound(tag_text(object.value(), "Behavior"),
                                               tag_text(object.value(), "SpaceBehavior"))) {
                    unfogged_map_props_.push_back(unit.state.entity_id);
                }
            }
            ship_of_entity_.emplace(unit.state.entity_id, placed_ships_.size());
            core::load_profile::slot_composed("initial", ship.object_id);
            core::load_profile::unit_spawned(ship.object_id);
            owner_of_entity_.emplace(unit.state.entity_id, unit.state.owner);
            placed_ships_.push_back(std::move(ship));
        }
        std::sort(unfogged_map_props_.begin(), unfogged_map_props_.end());
        select_arrivals();
        return true;
    });
    return stages;
}

bool BattleSourceState::prepare_local(const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string& failure, const LocalStartKind kind) {
    for (const auto& stage : local_stages(filesystem, catalog, kind)) {
        if (!stage(failure)) return false;
    }
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

std::vector<BattleSource::Stage> AttractSource::prepare_stages(BattlePreparation& prepared, const vfs::Vfs& files,
    const data::Catalog& catalog, std::string_view) const {
    return prepared.state().local_stages(files, catalog, LocalStartKind::attract);
}

BattleAiCapability LocalSkirmishSource::ai_capability() const noexcept { return BattleAiCapability::configurable; }
BattleAiCapability AttractSource::ai_capability() const noexcept { return BattleAiCapability::configurable; }

} // namespace eawr::presentation::godot_backend
