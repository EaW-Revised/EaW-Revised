#pragma once

// SKIPPED and exits 0.
//
// --melee s|m|l runs the close-range battle benchmark instead (#601, melee.cpp); --profile-attach
// <pid> samples another process, such as the viewer playing the melee (sampler.hpp).

#include "melee.hpp"
#include "sampler.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/pathfind.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace path_bench {


namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace foc = eawr::script::foc;
using eawr::sim::math::Fixed;
using Clock = std::chrono::steady_clock;
struct Options {
    std::optional<std::string> game_root;
    std::optional<std::uint64_t> order_tick; // default: 600 for owner, 3000 otherwise
    std::uint64_t ticks = 300;
    std::vector<std::size_t> workers{1, 4};
    std::vector<std::string> selections{"owner"};
    bool timing = true;
    bool live_execution = false; // --execution: legacy retains the historical path benchmark
    bool profile = false;
    unsigned profile_interval_us = 1000;
    std::optional<std::string> csv;
    std::optional<std::pair<std::int64_t, std::int64_t>> destination; // whole units; default: the mirror of the ships' centre
    bool list = false;
    std::optional<std::string> pin;
    bool update_pin = false;
    std::optional<std::string> replay_out;
    std::optional<std::string> ships; // --ships: the owner fleet's positions and plans after the order
    std::optional<std::uint64_t> search_budget; // --search-budget: overrides the avoidance rules' (PC-07)
};

struct Content {
    skirmish::SkirmishStart start;
    skirmish::SessionContent content;
    tactical::VictoryRules victory;
    foc::AiSetup ai;
    std::map<std::string, std::string> modules;
    std::map<tactical::TypeId, Fixed> heights; // Layer_Z_Adjust per unit-table type (SK-05)
};
[[nodiscard]] std::uint64_t clock_ticks() noexcept;

// Collects every call's stats on the searching threads; the main thread takes them per tick.
class Probe final : public tactical::PathSearchProbe {
public:
    explicit Probe(const bool timing) : timing_(timing) {}
    [[nodiscard]] std::uint64_t now() noexcept override { return timing_ ? clock_ticks() : 0; }
    void searched(const tactical::PathSearchStats& stats) noexcept override {
        const std::lock_guard lock(mutex_);
        pending_.push_back(stats);
    }
    [[nodiscard]] std::vector<tactical::PathSearchStats> take() {
        const std::lock_guard lock(mutex_);
        return std::exchange(pending_, {});
    }

private:
    bool timing_;
    std::mutex mutex_;
    std::vector<tactical::PathSearchStats> pending_;
};

struct TickRecord {
    std::uint64_t tick{};
    double ms{};
    double plan_ms{};
    std::vector<tactical::PathSearchStats> calls;
    double world_ms{};
    double engine_ms{};
    double service_ms{};
    std::map<std::string, double> phases;
};

struct Run {
    std::map<eawr::sim::EntityId, std::size_t> layers; // the dynamic layer of each unit at the order
    std::vector<std::string> hashes; // every tick's world hash
    std::vector<TickRecord> before;  // the ticks before the order (timing only)
    std::vector<TickRecord> after;   // the order tick and the ticks after it
    std::size_t ships{};
    std::size_t squadrons{};
    std::size_t most_sliced{};       // PC-08: the most sliced searches after a tick
    std::size_t most_sliced_bytes{}; // and the most memory they held
    double ns_per_clock_tick{1};
    std::string ships_csv; // --ships rows
};

[[nodiscard]] std::optional<Options> parse_options(const int argc, const char* const argv[]);
[[nodiscard]] std::optional<Content> load(const std::filesystem::path& root);
[[nodiscard]] std::optional<Run> run(const Content& content, const std::string& selection, const std::size_t workers,
    const Options& options, Probe& probe, eawr::bench::Sampler* sampler);
[[nodiscard]] double percentile(std::vector<double> values, const double fraction);
void report(const Run& run, const std::string& selection, const std::size_t workers);
void write_csv(const std::string& path, const Run& run);
void write_searches_csv(const std::string& path, const Run& run);
[[nodiscard]] std::vector<std::string> work_rows(const Run& run);
[[nodiscard]] std::vector<std::string> budget_violations(const Run& run, const tactical::AvoidanceRules& rules);

} // namespace path_bench
