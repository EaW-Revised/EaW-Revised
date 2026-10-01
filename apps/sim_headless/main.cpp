#include "eawr/core/diagnostic.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/executable.hpp"
#include "eawr/platform/publish_files.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include "scenario.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

void print_help(std::ostream& output) {
    output << "Usage: sim_headless --replay <file> --hash-out <file> [--workers <n|hardware>]\n"
              "                    [--events-out <file>] [--snapshot-out <file>]\n"
              "                    [--census-out <file.json>] [--trace-out <file.csv>]\n"
              "                    [--game-root <install> [--mod-root <leaf;parent;...>]]\n"
              "       sim_headless --skirmish m2 --game-root <install> [--mod-root <leaf;parent;...>] [--census-out <file.json>]\n"
              "                    [--replay-out <file>] [--ticks <n>] [--tag-trace-out <file.json>]\n"
              "       sim_headless --scenario <S-NN.json> --game-root <install> --trace-out <file.csv>\n"
              "                    [--hash-out <file>] [--replay-out <file>] [--combat-out <file.csv>]\n"
              "                    [--workers <1|2|4>]\n"
              "\n"
              "Runs replay-v1 or replay-v2 without a renderer and writes UTF-8 tick,state-hash CSV.\n"
              "A replay-v2 run can also write its event stream, per-tick snapshot digests and the\n"
              "tick-zero census of its setup. With --game-root it runs with the sensor, durability\n"
              "and motion tables of that installation's unit tables, which must be the content its\n"
              "setup names (a viewer live session records such replays). A replay-v1 run can also\n"
              "write a docs/traces.md trace\n"
              "of entity positions, with its JSON header beside it (same name, .json).\n"
              "--workers sizes the simulation worker pool: 1 to 256 threads, or hardware for\n"
              "one per hardware thread (default 1). No output depends on it.\n"
              "--skirmish m2 builds tick zero of the pinned FoC skirmish from the installation\n"
              "(docs/skirmish-start.md), lists it, and writes its census and a replay-v2 file that\n"
              "holds the setup alone and <n> ticks (default 0). --tag-trace-out records the XML its\n"
              "loaders read (docs/tag-coverage.md).\n"
              "--scenario stages a tests/fidelity scenario on the FoC unit tables and writes the remake's\n"
              "trace of it (docs/traces.md), its tick hashes and its replay. --combat-out adds its combat log:\n"
              "every shot, projectile hit and change of hull, shield and hardpoint health (docs/traces.md).\n"
              "All outputs are published or none.\n";
}

int report_argument_error(const std::string_view message) {
    const eawr::core::Diagnostic diagnostic{
        .code = std::string(eawr::core::diagnostic_codes::invalid_argument),
        .severity = eawr::core::Severity::error,
        .message = std::string(message),
        .logical_path = std::nullopt,
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("sim_headless"),
    };
    std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
    return 2;
}

int report_io_error(const std::string_view path, const std::string_view message, const int code) {
    const eawr::core::Diagnostic diagnostic{
        .code = "EAWR-SIM-CLI-0001",
        .severity = eawr::core::Severity::error,
        .message = std::string(message),
        .logical_path = std::string(path),
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("sim_headless"),
    };
    std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
    return code;
}

// Files a failed publication or its clean-up left behind; the path names each one.
int report_leftover(const std::string_view path, const std::string_view message) {
    const eawr::core::Diagnostic diagnostic{
        .code = "EAWR-SIM-CLI-0002",
        .severity = eawr::core::Severity::error,
        .message = std::string(message),
        .logical_path = std::string(path),
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("sim_headless"),
    };
    std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
    return 5;
}

struct Options {
    std::string replay_path;
    std::string hash_path;
    std::string events_path;
    std::string snapshot_path;
    std::string trace_path;
    std::string census_path;
    std::size_t workers{1};
    // --skirmish mode
    std::string skirmish;
    std::string game_root;
    std::filesystem::path mod_root;
    std::string replay_out_path;
    std::uint64_t ticks{};
    std::string tag_trace_path; // --tag-trace-out (#628)
    // --scenario mode
    std::string scenario_path;
    std::string combat_path; // --combat-out (#536)
};

[[nodiscard]] std::optional<Options> parse_options(const int argc, const char* const argv[]) {
    Options options;
    bool saw_replay = false;
    bool saw_hash = false;
    bool saw_trace = false;
    bool saw_workers = false;
    bool saw_events = false;
    bool saw_snapshot = false;
    bool saw_census = false;
    bool saw_skirmish = false;
    bool saw_game_root = false;
    bool saw_replay_out = false;
    bool saw_ticks = false;
    bool saw_scenario = false;
    bool saw_combat = false;
    bool saw_tag_trace = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if ((argument == "--replay" || argument == "--hash-out" || argument == "--trace-out"
             || argument == "--workers" || argument == "--events-out"
             || argument == "--snapshot-out" || argument == "--census-out" || argument == "--skirmish"
             || argument == "--game-root" || argument == "--mod-root" || argument == "--replay-out" || argument == "--ticks"
             || argument == "--scenario" || argument == "--combat-out" || argument == "--tag-trace-out")
            && index + 1 >= argc) {
            return std::nullopt;
        }
        if (argument == "--replay" && !saw_replay) {
            saw_replay = true;
            options.replay_path = argv[++index];
        } else if (argument == "--hash-out" && !saw_hash) {
            saw_hash = true;
            options.hash_path = argv[++index];
        } else if (argument == "--trace-out" && !saw_trace) {
            saw_trace = true;
            options.trace_path = argv[++index];
            if (std::filesystem::path(options.trace_path).extension() != ".csv") {
                return std::nullopt;
            }
        } else if (argument == "--workers" && !saw_workers) {
            saw_workers = true;
            const std::string_view value(argv[++index]);
            std::size_t parsed{};
            if (value == "hardware") {
                parsed = eawr::platform::ThreadWorkerAdapter::hardware_worker_count();
            } else {
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0
                    || parsed > eawr::platform::ThreadWorkerAdapter::max_worker_count) {
                    return std::nullopt;
                }
            }
            options.workers = parsed;
        } else if (argument == "--events-out" && !saw_events) {
            saw_events = true;
            options.events_path = argv[++index];
            if (options.events_path.empty()) {
                return std::nullopt;
            }
        } else if (argument == "--snapshot-out" && !saw_snapshot) {
            saw_snapshot = true;
            options.snapshot_path = argv[++index];
            if (options.snapshot_path.empty()) {
                return std::nullopt;
            }
        } else if (argument == "--census-out" && !saw_census) {
            saw_census = true;
            options.census_path = argv[++index];
            if (std::filesystem::path(options.census_path).extension() != ".json") {
                return std::nullopt;
            }
        } else if (argument == "--skirmish" && !saw_skirmish) {
            saw_skirmish = true;
            options.skirmish = argv[++index];
            if (options.skirmish != "m2") {
                return std::nullopt;
            }
        } else if (argument == "--game-root" && !saw_game_root) {
            saw_game_root = true;
            options.game_root = argv[++index];
            if (options.game_root.empty()) {
                return std::nullopt;
            }
        } else if (argument == "--mod-root" && options.mod_root.empty()) {
            options.mod_root = argv[++index];
            if (options.mod_root.empty()) return std::nullopt;
        } else if (argument == "--replay-out" && !saw_replay_out) {
            saw_replay_out = true;
            options.replay_out_path = argv[++index];
            if (options.replay_out_path.empty()) {
                return std::nullopt;
            }
        } else if (argument == "--scenario" && !saw_scenario) {
            saw_scenario = true;
            options.scenario_path = argv[++index];
            if (options.scenario_path.empty()) {
                return std::nullopt;
            }
        } else if (argument == "--combat-out" && !saw_combat) {
            saw_combat = true;
            options.combat_path = argv[++index];
            if (options.combat_path.empty()) {
                return std::nullopt;
            }
        } else if (argument == "--tag-trace-out" && !saw_tag_trace) {
            saw_tag_trace = true;
            options.tag_trace_path = argv[++index];
            if (std::filesystem::path(options.tag_trace_path).extension() != ".json") {
                return std::nullopt;
            }
        } else if (argument == "--ticks" && !saw_ticks) {
            saw_ticks = true;
            const std::string_view value(argv[++index]);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), options.ticks);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size()
                || options.ticks > eawr::sim::tactical::max_ticks) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }
    if ((saw_combat && !saw_scenario) || (saw_tag_trace && !saw_skirmish)) {
        return std::nullopt;
    }
    if (saw_scenario) {
        // A scenario run writes a trace, and optionally its tick hashes, replay and combat log.
        if (saw_skirmish || saw_replay || saw_events || saw_snapshot || saw_census || saw_ticks
            || !options.mod_root.empty() || !saw_game_root || !saw_trace) {
            return std::nullopt;
        }
        return options;
    }
    if (saw_skirmish) {
        // The skirmish start writes a census and/or a replay; it runs no ticks itself.
        if (saw_replay || saw_hash || saw_trace || saw_workers || saw_events || saw_snapshot
            || !saw_game_root || (!saw_census && !saw_replay_out && !saw_tag_trace) || (saw_ticks && !saw_replay_out)) {
            return std::nullopt;
        }
        return options;
    }
    if (saw_replay_out || saw_ticks || (!options.mod_root.empty() && !saw_game_root)) {
        return std::nullopt;
    }
    if (!saw_replay || !saw_hash || options.replay_path.empty() || options.hash_path.empty()) {
        return std::nullopt;
    }
    return options;
}

[[nodiscard]] std::filesystem::path trace_header_path(const std::string& trace_path) {
    return std::filesystem::path(trace_path).replace_extension(".json");
}

// A path resolved through the directories that exist, with ASCII case folded: two outputs
// whose names differ only in case are one file on Windows, so they are refused everywhere.
[[nodiscard]] std::filesystem::path::string_type file_key(const std::filesystem::path& path) {
    using Character = std::filesystem::path::value_type;
    std::error_code error;
    auto resolved = std::filesystem::absolute(path, error);
    if (!error) {
        auto canonical = std::filesystem::weakly_canonical(resolved, error);
        if (!error) {
            resolved = std::move(canonical);
        }
    }
    auto key = resolved.lexically_normal().native();
    std::transform(key.begin(), key.end(), key.begin(), [](const Character character) {
        return character >= Character{'A'} && character <= Character{'Z'}
                   ? static_cast<Character>(character - Character{'A'} + Character{'a'})
                   : character;
    });
    return key;
}

[[nodiscard]] bool same_file(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(left, right, error);
    return (!error && equivalent) || file_key(left) == file_key(right);
}

// The replay and every output (hash, event, snapshot and trace CSV, the trace's header, the
// census and the written replay) are read or written whole, so no two of them may be one file.
[[nodiscard]] bool paths_are_distinct(const Options& options) {
    std::vector<std::filesystem::path> paths;
    for (const auto* path : {&options.replay_path, &options.hash_path, &options.scenario_path}) {
        if (!path->empty()) {
            paths.emplace_back(*path);
        }
    }
    for (const auto* output : {&options.events_path, &options.snapshot_path, &options.census_path,
             &options.replay_out_path, &options.combat_path, &options.tag_trace_path}) {
        if (!output->empty()) {
            paths.emplace_back(*output);
        }
    }
    if (!options.trace_path.empty()) {
        paths.emplace_back(options.trace_path);
        paths.push_back(trace_header_path(options.trace_path));
    }
    for (std::size_t left = 0; left < paths.size(); ++left) {
        for (std::size_t right = left + 1; right < paths.size(); ++right) {
            if (same_file(paths[left], paths[right])) {
                return false;
            }
        }
    }
    return true;
}

// docs/traces.md: a replay has no scenario, so each stable ID is labelled entity.<id>.
struct TraceObject {
    std::string label;
    eawr::sim::EntityId entity_id{};
};

[[nodiscard]] std::vector<TraceObject> trace_objects(const eawr::sim::Replay& replay) {
    std::vector<TraceObject> objects;
    const auto add = [&objects](const eawr::sim::EntityId id) {
        objects.push_back({"entity." + std::to_string(id), id});
    };
    for (const auto& entity : replay.initial_entities) {
        add(entity.entity_id);
    }
    for (const auto& command : replay.commands) {
        if (const auto* create = std::get_if<eawr::sim::CreateCommand>(&command.payload)) {
            add(create->entity.entity_id);
        }
    }
    const auto by_label = [](const TraceObject& left, const TraceObject& right) {
        return left.label < right.label;
    };
    const auto same_label = [](const TraceObject& left, const TraceObject& right) {
        return left.label == right.label;
    };
    std::sort(objects.begin(), objects.end(), by_label);
    objects.erase(std::unique(objects.begin(), objects.end(), same_label), objects.end());
    return objects;
}

// Rows for one completed tick, sorted by object label, then field name.
void append_trace_rows(
    std::ostringstream& trace,
    const std::uint64_t tick,
    const std::vector<TraceObject>& objects,
    const std::vector<eawr::sim::EntityState>& entities) {
    for (const auto& object : objects) {
        const auto found = std::lower_bound(
            entities.begin(),
            entities.end(),
            object.entity_id,
            [](const eawr::sim::EntityState& entity, const eawr::sim::EntityId id) {
                return entity.entity_id < id;
            });
        const bool alive = found != entities.end() && found->entity_id == object.entity_id;
        trace << tick << ',' << object.label << ",alive," << (alive ? 1 : 0) << '\n';
        if (alive) {
            trace << tick << ',' << object.label << ",pos.x," << found->position.x.raw() << '\n'
                  << tick << ',' << object.label << ",pos.y," << found->position.y.raw() << '\n'
                  << tick << ',' << object.label << ",pos.z," << found->position.z.raw() << '\n';
        }
    }
}

[[nodiscard]] std::string lower_hex(const std::array<std::uint8_t, 32>& bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0x0fU]);
    }
    return text;
}

[[nodiscard]] std::string trace_header(
    const eawr::sim::Replay& replay,
    const std::string_view replay_sha256,
    const std::string_view build_sha256) {
    std::ostringstream json;
    json << "{\n"
         << "  \"format\": \"eawr-trace\",\n"
         << "  \"format_version\": 1,\n"
         << "  \"source\": \"remake\",\n"
         << "  \"content_identity\": \"" << lower_hex(replay.content_identity) << "\",\n"
         << "  \"tick_seconds\": {\"numerator\": " << replay.tick_numerator
         << ", \"denominator\": " << replay.tick_denominator << "},\n"
         << "  \"build_identity\": {\"kind\": \"executable-sha256\", \"value\": \"" << build_sha256
         << "\"},\n"
         << "  \"scenario_sha256\": null,\n"
         << "  \"replay_sha256\": \"" << replay_sha256 << "\"\n"
         << "}\n";
    return json.str();
}

// Exit 0 when every output was published, 4 when none was, 5 when files are left behind.
int publish(const std::vector<eawr::platform::PublishedFile>& outputs) {
    const auto report = eawr::platform::publish_files(outputs);
    int code = 0;
    if (report.failure) {
        code = report_io_error(report.failure->path, report.failure->message, 4);
    }
    for (const auto& leftover : report.leftovers) {
        code = report_leftover(leftover.path, leftover.message);
    }
    return code;
}

// The unit tables of an installation, mounted read-only as --skirmish m2 mounts it: the mod
// chain, then FoC over base EaW.
struct LoadedTables {
    std::optional<eawr::vfs::Vfs> filesystem;
    std::optional<eawr::data::LoadResult> catalog;
    std::optional<eawr::units::UnitTables> tables;
};

[[nodiscard]] eawr::core::Result<std::unique_ptr<LoadedTables>> load_tables(const Options& options) {
    using LoadResult = eawr::core::Result<std::unique_ptr<LoadedTables>>;
    auto loaded = std::make_unique<LoadedTables>();
    const std::filesystem::path root(options.game_root);
    std::vector<eawr::vfs::MountSpec> specs;
    auto roots = eawr::vfs::mod_chain_roots(options.mod_root);
    roots.emplace_back("expansion", root / "corruption" / "Data");
    roots.emplace_back("base", root / "GameData" / "Data");
    auto chain = eawr::vfs::resolve_manifest_chain(roots);
    if (!chain) return LoadResult::failure(chain.error());
    for (auto& manifest : chain.value()) specs.push_back(std::move(manifest.mount));
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) return LoadResult::failure(filesystem.error());
    loaded->filesystem.emplace(std::move(filesystem).value());
    auto catalog = eawr::data::load_catalog(*loaded->filesystem,
        options.mod_root.empty() ? eawr::data::Profile::foc : eawr::data::Profile::remake);
    if (!catalog) return LoadResult::failure(catalog.error());
    loaded->catalog.emplace(std::move(catalog).value());
    eawr::scene::VfsAssetCache cache(*loaded->filesystem);
    const auto access = cache.access();
    eawr::units::LoadInput unit_input;
    unit_input.catalog = &loaded->catalog->catalog;
    unit_input.filesystem = &*loaded->filesystem;
    unit_input.model = access.model;
    auto tables = eawr::units::load_unit_tables(unit_input);
    if (!tables) return LoadResult::failure(tables.error());
    loaded->tables.emplace(std::move(tables).value());
    return LoadResult::success(std::move(loaded));
}

// Replay-v2 runs the tactical session. Every output is staged until the whole run succeeds.
int run_tactical(const Options& options, const std::vector<std::uint8_t>& bytes) {
    namespace tactical = eawr::sim::tactical;
    const auto replay = tactical::parse_replay(bytes, options.replay_path);
    if (!replay) {
        std::cerr << eawr::core::format_diagnostic(replay.error()) << '\n';
        return 3;
    }
    // With --game-root the session runs with the installation's content tables, which must
    // be the content the setup names.
    eawr::skirmish::SessionContent content;
    tactical::VictoryRules victory;
    tactical::EconomyRules economy;
    if (!options.game_root.empty()) {
        auto loaded = load_tables(options);
        if (!loaded) {
            std::cerr << eawr::core::format_diagnostic(loaded.error()) << '\n';
            return 3;
        }
        if (eawr::units::content_identity(*loaded.value()->tables) != replay.value().setup.content_identity) {
            eawr::core::Diagnostic mismatch;
            mismatch.code = std::string(tactical::diagnostic_codes::invalid_setup);
            mismatch.message = "the unit tables of --game-root are not the content the replay's setup names";
            mismatch.logical_path = options.replay_path;
            std::cerr << eawr::core::format_diagnostic(mismatch) << '\n';
            return 3;
        }
        // #77, #76: the replay does not record which players are human; they are the pinned
        // fixture's human slots (the victory rules' and the ability table's).
        const auto humans = eawr::skirmish::human_slots(eawr::skirmish::m2_fixture());
        auto tables = eawr::skirmish::session_content(*loaded.value()->tables, humans);
        if (!tables) {
            std::cerr << eawr::core::format_diagnostic(tables.error()) << '\n';
            return 3;
        }
        content = std::move(tables).value();
        // #495: an M2 replay runs on the M2 map's fog grid, as the live session does.
        auto inputs = eawr::skirmish::read_start_inputs(eawr::skirmish::m2_fixture(), *loaded.value()->filesystem,
            loaded.value()->catalog->catalog, *loaded.value()->tables);
        if (!inputs) {
            std::cerr << eawr::core::format_diagnostic(inputs.error()) << '\n';
            return 3;
        }
        auto fog = eawr::skirmish::fog_rules(inputs.value());
        if (!fog) {
            std::cerr << eawr::core::format_diagnostic(fog.error()) << '\n';
            return 3;
        }
        content.fog = fog.value();
        victory = eawr::skirmish::victory_rules(replay.value().setup, *loaded.value()->tables, humans);
        // #530: a replay of the M2 start runs with its economy, as the live session does. The start
        // is rebuilt from the same inputs; a replay of another setup has none.
        auto start = eawr::skirmish::build_start(eawr::skirmish::m2_fixture(), inputs.value());
        if (start && start.value().setup.players == replay.value().setup.players
            && start.value().setup.units == replay.value().setup.units) {
            auto rules = eawr::skirmish::economy_rules(start.value(), inputs.value(), *loaded.value()->tables);
            if (!rules) {
                std::cerr << eawr::core::format_diagnostic(rules.error()) << '\n';
                return 3;
            }
            economy = std::move(rules).value();
        }
    }
    auto session_result = tactical::TacticalSession::from_replay(
        replay.value(), content.sensors, content.durability, content.motion, content.fog, content.combat, victory,
        content.abilities, economy);
    if (!session_result) {
        std::cerr << eawr::core::format_diagnostic(session_result.error()) << '\n';
        return 3;
    }
    auto session = std::move(session_result).value();
    const eawr::platform::ThreadWorkerAdapter executor(options.workers);
    std::ostringstream hashes;
    std::ostringstream events;
    std::ostringstream snapshots;
    std::string census;
    if (!options.census_path.empty()) {
        auto text = eawr::skirmish::census_json(replay.value().setup, nullptr);
        if (!text) {
            std::cerr << eawr::core::format_diagnostic(text.error()) << '\n';
            return 3;
        }
        census = std::move(text).value();
    }
    hashes << "tick,sha256\n";
    events << "tick,player,sequence,unit,event,order,reason\n";
    snapshots << "tick,sha256\n";
    snapshots << session.completed_tick() << ',' << session.snapshot()->sha256() << '\n';
    while (session.completed_tick() < replay.value().final_tick_count) {
        const auto tick = session.step(executor);
        if (!tick) {
            std::cerr << eawr::core::format_diagnostic(tick.error()) << '\n';
            return 3;
        }
        hashes << tick.value().completed_tick << ',' << tick.value().state_sha256 << '\n';
        snapshots << tick.value().completed_tick << ',' << tick.value().snapshot->sha256() << '\n';
        for (const auto& event : tick.value().snapshot->events()) {
            events << event.tick << ',' << event.player << ',' << event.sequence << ','
                   << event.unit << ',' << tactical::to_string(event.kind) << ','
                   << tactical::to_string(event.order) << ',' << tactical::to_string(event.reason)
                   << '\n';
        }
    }

    if (const auto& outcome = session.outcome()) {
        std::cout << "outcome " << tactical::to_string(outcome->condition) << " winner " << outcome->winner << " team "
                  << outcome->winner_team << " decided_tick " << outcome->decided_tick << " unit "
                  << outcome->deciding_unit << " end_tick " << outcome->end_tick << '\n';
    }
    std::vector<eawr::platform::PublishedFile> outputs;
    outputs.push_back({"hash output", options.hash_path, hashes.str()});
    if (!options.events_path.empty()) {
        outputs.push_back({"event output", options.events_path, events.str()});
    }
    if (!options.snapshot_path.empty()) {
        outputs.push_back({"snapshot output", options.snapshot_path, snapshots.str()});
    }
    if (!options.census_path.empty()) {
        outputs.push_back({"census output", options.census_path, census});
    }
    return publish(outputs);
}

// Human-readable tick-zero listing: players, then units, then the SK-23 launches.
void list_start(std::ostream& output, const eawr::skirmish::SkirmishStart& start, const std::string& state_sha256) {
    const auto whole = [](const eawr::sim::math::Fixed value) {
        return std::to_string(value.nearest_even_to_integer());
    };
    output << "map " << start.map << " sha256 " << start.map_sha256 << '\n';
    for (const auto& player : start.players) {
        output << "player " << player.player.player_id << ' ' << player.faction << " team " << player.player.team_id;
        if (player.lobby) {
            output << (player.human ? " human" : " ai") << " start " << player.start_side;
            if (player.colour) {
                output << " colour " << player.colour->constant << " (" << static_cast<int>(player.colour->rgb[0])
                       << ", " << static_cast<int>(player.colour->rgb[1]) << ", "
                       << static_cast<int>(player.colour->rgb[2]) << ')';
            }
            // SK-30, SK-31 (#530): the starting credits and whether the station earns and builds.
            output << " credits " << player.credits << " income " << (player.income ? "station" : "none")
                   << " production " << (player.production_queue ? "station" : "none") << " power "
                   << whole(player.combat_power_tick_zero);
        } else if (player.owner_index) {
            output << " non-playable (TED index " << *player.owner_index << ')';
        }
        output << '\n';
    }
    for (const auto& unit : start.units) {
        output << "unit " << unit.state.entity_id << ' ' << unit.type << " owner " << unit.state.owner << ' '
               << eawr::skirmish::to_string(unit.role) << " record " << unit.record << " at ("
               << whole(unit.state.position.x) << ", " << whole(unit.state.position.y) << ", "
               << whole(unit.state.position.z) << ") yaw " << whole(unit.yaw_degrees);
        if (unit.craft != 0) output << " craft " << unit.craft << " x " << unit.craft_type;
        output << '\n';
    }
    for (const auto& removed : start.removed) {
        output << "removed " << removed.type << " record " << removed.record << " (" << removed.faction << ", "
               << eawr::skirmish::to_string(removed.reason) << ")\n";
    }
    for (const auto& launch : start.launches) {
        output << "launch (not simulated) " << launch.count << " x " << launch.squadron << " from unit "
               << launch.spawner << ' ' << launch.spawner_type << '\n';
    }
    output << "tick 0 state " << state_sha256 << '\n';
}

// --scenario: the remake's trace of a tests/fidelity scenario (#70).
int run_scenario_mode(const Options& options) {
    const auto build = eawr::platform::current_executable_sha256();
    if (!build) {
        return report_io_error(options.trace_path, "could not hash the running executable", 4);
    }
    auto run = sim_headless::run_scenario(options.scenario_path, options.game_root, options.workers, *build);
    if (!run) {
        std::cerr << eawr::core::format_diagnostic(run.error()) << '\n';
        return 3;
    }
    for (const auto& warning : run.value().warnings) {
        std::cerr << "warning: " << warning << '\n';
    }
    std::vector<eawr::platform::PublishedFile> outputs;
    outputs.push_back({"trace output", options.trace_path, run.value().trace_csv});
    outputs.push_back({"trace header", trace_header_path(options.trace_path).string(), run.value().trace_header});
    if (!options.hash_path.empty()) {
        outputs.push_back({"hash output", options.hash_path, run.value().hashes_csv});
    }
    if (!options.combat_path.empty()) {
        outputs.push_back({"combat output", options.combat_path, run.value().combat_csv});
    }
    if (!options.replay_out_path.empty()) {
        if (run.value().staged) {
            std::cerr << "error: --replay-out: the scenario spawns or removes units, which a replay does not record\n";
            return 2;
        }
        outputs.push_back({"replay output", options.replay_out_path,
            std::string(run.value().replay.begin(), run.value().replay.end())});
    }
    return publish(outputs);
}

// --skirmish m2: tick zero from the FoC installation (FoC over base EaW, read-only).
int run_skirmish(const Options& options) {
    namespace skirmish = eawr::skirmish;
    const auto fail = [](const eawr::core::Diagnostic& diagnostic) {
        std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
        return 3;
    };
    // #628: the trace covers every load the skirmish start makes.
    std::optional<eawr::data::tag_trace::Recording> recording;
    if (!options.tag_trace_path.empty()) recording.emplace();
    auto loaded = load_tables(options);
    if (!loaded) return fail(loaded.error());
    const auto& filesystem = *loaded.value()->filesystem;
    const auto& catalog = loaded.value()->catalog->catalog;
    const auto& tables = *loaded.value()->tables;
    const auto& fixture = skirmish::m2_fixture();
    auto inputs = skirmish::read_start_inputs(fixture, filesystem, catalog, tables);
    if (!inputs) return fail(inputs.error());
    auto start = skirmish::build_start(fixture, inputs.value());
    if (!start) return fail(start.error());
    // The unit tables' sensor table (#68) drives the tick-zero snapshot's visibility.
    const auto sensors = skirmish::sensor_table(tables);
    auto session = eawr::sim::tactical::TacticalSession::create(start.value().setup, sensors);
    if (!session) return fail(session.error());

    std::vector<eawr::platform::PublishedFile> outputs;
    if (recording) {
        outputs.push_back({"tag trace output", options.tag_trace_path,
            eawr::data::tag_trace::to_json(recording->finish())});
    }
    if (!options.census_path.empty()) {
        auto census = skirmish::census_json(start.value().setup, &start.value(), sensors);
        if (!census) return fail(census.error());
        outputs.push_back({"census output", options.census_path, std::move(census).value()});
    }
    if (!options.replay_out_path.empty()) {
        eawr::sim::tactical::TacticalReplay replay;
        replay.setup = start.value().setup;
        replay.final_tick_count = options.ticks;
        auto bytes = eawr::sim::tactical::write_replay(replay);
        if (!bytes) return fail(bytes.error());
        outputs.push_back({"replay output", options.replay_out_path,
            std::string(bytes.value().begin(), bytes.value().end())});
    }
    list_start(std::cout, start.value(), session.value().state_sha256());
    return publish(outputs);
}

} // namespace

int main(const int argc, const char* const argv[]) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        print_help(std::cout);
        return 0;
    }
    const auto options = parse_options(argc, argv);
    if (!options) {
        return report_argument_error(
            "expected --replay <file> --hash-out <file> [--workers <n|hardware>] "
            "[--events-out <file>] [--snapshot-out <file>] [--census-out <file.json>] "
            "[--trace-out <file.csv>], or --skirmish m2 --game-root <install> "
            "[--census-out <file.json>] [--replay-out <file>] [--ticks <n>] [--tag-trace-out <file.json>], "
            "or --scenario <file.json> "
            "--game-root <install> --trace-out <file.csv> [--hash-out <file>] [--replay-out <file>] "
            "[--combat-out <file.csv>] [--workers <1|2|4>]");
    }
    if (!paths_are_distinct(*options)) {
        return report_argument_error(
            "--replay, --scenario, --hash-out, --events-out, --snapshot-out, --census-out, --replay-out, --combat-out, --tag-trace-out, "
            "--trace-out and the trace's .json header must name different files");
    }
    if (!options->scenario_path.empty()) {
        return run_scenario_mode(*options);
    }
    if (!options->skirmish.empty()) {
        return run_skirmish(*options);
    }

    std::ifstream input(options->replay_path, std::ios::binary | std::ios::ate);
    if (!input) {
        return report_io_error(options->replay_path, "could not open replay input", 3);
    }
    const auto end = input.tellg();
    const auto input_size = static_cast<std::streamoff>(end);
    if (input_size < 0 || static_cast<std::uint64_t>(input_size) > eawr::sim::replay_max_bytes) {
        return report_io_error(options->replay_path, "replay input exceeds the 256 MiB limit", 3);
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(input_size));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!input && !bytes.empty()) {
        return report_io_error(options->replay_path, "could not read complete replay input", 3);
    }

    if (const auto tactical_version = eawr::sim::tactical::peek_replay_format_version(bytes);
        tactical_version == eawr::sim::tactical::replay_format_version
        || tactical_version == eawr::sim::tactical::replay_format_version_squadrons) {
        if (!options->trace_path.empty()) {
            return report_argument_error("--trace-out requires a replay-v1 input");
        }
        return run_tactical(*options, bytes);
    }
    if (!options->game_root.empty()) {
        return report_argument_error("--game-root requires a replay-v2 input");
    }
    if (!options->events_path.empty() || !options->snapshot_path.empty() || !options->census_path.empty()) {
        return report_argument_error(
            "--events-out, --snapshot-out and --census-out require a replay-v2 input");
    }

    const auto replay = eawr::sim::parse_replay(bytes, options->replay_path);
    if (!replay) {
        std::cerr << eawr::core::format_diagnostic(replay.error()) << '\n';
        return 3;
    }
    auto world_result = eawr::sim::World::create(replay.value());
    if (!world_result) {
        std::cerr << eawr::core::format_diagnostic(world_result.error()) << '\n';
        return 3;
    }
    auto world = std::move(world_result).value();
    const eawr::platform::ThreadWorkerAdapter executor(options->workers);
    const bool tracing = !options->trace_path.empty();
    const auto objects = tracing ? trace_objects(replay.value()) : std::vector<TraceObject>{};
    std::ostringstream csv;
    std::ostringstream trace;
    csv << "tick,sha256\n";
    if (tracing) {
        trace << "tick,object,field,value\n";
        append_trace_rows(trace, world.completed_tick(), objects, world.entities());
    }
    while (world.completed_tick() < world.final_tick_count()) {
        const auto tick = world.step(executor);
        if (!tick) {
            std::cerr << eawr::core::format_diagnostic(tick.error()) << '\n';
            return 3;
        }
        csv << tick.value().completed_tick << ',' << tick.value().state_sha256 << '\n';
        if (tracing) {
            append_trace_rows(trace, tick.value().completed_tick, objects, world.entities());
        }
    }

    std::string header;
    if (tracing) {
        // The remake's build identity is the SHA-256 of the running sim_headless image.
        const auto build = eawr::platform::current_executable_sha256();
        if (!build) {
            return report_io_error(options->trace_path, "could not hash the running executable", 4);
        }
        header = trace_header(replay.value(), eawr::sim::sha256_hex(bytes), *build);
    }
    std::vector<eawr::platform::PublishedFile> outputs;
    outputs.push_back({"hash output", options->hash_path, csv.str()});
    if (tracing) {
        outputs.push_back({"trace output", options->trace_path, trace.str()});
        outputs.push_back({"trace header", trace_header_path(options->trace_path).string(), header});
    }
    return publish(outputs);
}
