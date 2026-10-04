#pragma once

// The soak driver's command line (#627). Every flag is validated: a zero or non-numeric value
// is an error, never a run that passes because it did nothing.
//
//   foc_soak_tests [--seeds LIST] [--ticks N] [--battles N] [--workers N] [--hash-workers N]
//                  [--hash-every N] [--out DIR] [--strict] [--time-budget SECONDS]
//                  [--check-every N] [--spawn-window N] [--hull-penetration F] [--hull-ticks N]
//                  [--slot-radius F] [--slot-ticks N] [--map-margin F] [--speed-factor F] [--craft-speed-factor F]
//                  [--require-decision] [--no-invariants]
//                  [--ai-schedule faithful|staggered] [--ai-attach-cap N]
//                  [--tick-trace DIR] [--check-schedule]

#include "soak_invariants.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace eawr::soak {

struct Options {
    std::vector<std::uint64_t> seeds{1};    // seeds and inclusive ranges: 12,18,40-47
    std::uint64_t ticks{9000};              // the cap: five minutes of battle
    std::size_t battles{1};                 // battles running at once, each on its own workers
    std::size_t workers{1};                 // simulation workers of each battle
    std::size_t hash_workers{0};            // 0: off; else a second run at this many workers, compared every tick
    std::size_t hash_every{1};              // the hash comparison runs on every Nth seed of the list
    std::optional<std::filesystem::path> out; // summary.json, seed-N.json and a replay per failing seed
    bool strict{false};                     // a missing game root is a failure, not a skip
    double time_budget{0.0};                // seconds after which no new seed starts; 0: none
    std::size_t check_every{1};             // invariants are checked every Nth tick
    bool require_decision{false};           // a seed that reaches the cap without a decided battle fails
    bool invariants{true};                  // false: only sim failures (and the worker comparison) fail a seed
    // #957: the AI's schedule. Unset: the project's (staggered, one plan attach a tick).
    std::optional<bool> ai_faithful;        // true: FoC's own schedule, for the before/after measurement
    std::optional<std::uint32_t> ai_attach_cap;
    std::optional<std::filesystem::path> tick_trace; // seed-N-ticks.csv per seed: each tick's cost and AI work
    bool check_schedule{false};             // #957: per seed, count the AI's work per tick under both schedules and check the caps
    Limits limits;
};

// The options of `arguments` (argv without the program name), or the reason they are invalid.
[[nodiscard]] std::variant<Options, std::string> parse_options(const std::vector<std::string>& arguments);

// "12,18,40-47": the seeds in order; nothing on a malformed list, an empty one, or a range over
// 100000 seeds.
[[nodiscard]] std::optional<std::vector<std::uint64_t>> parse_seed_list(const std::string& text);

// The flags that reproduce `options` for one seed, "--seeds 12 --ticks 2500 --workers 1 ..." (the
// invariant limits only when they differ from the defaults).
[[nodiscard]] std::string reproduction_flags(const Options& options, std::uint64_t seed);

} // namespace eawr::soak
