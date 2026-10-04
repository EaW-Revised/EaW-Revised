#include "cli_internal.hpp"

namespace sim_headless::cli {

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

} // namespace sim_headless::cli
