#pragma once

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

namespace sim_headless::cli {

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
struct TraceObject {
    std::string label;
    eawr::sim::EntityId entity_id{};
};

struct LoadedTables {
    std::optional<eawr::vfs::Vfs> filesystem;
    std::optional<eawr::data::LoadResult> catalog;
    std::optional<eawr::units::UnitTables> tables;
};


void print_help(std::ostream& output);
int report_argument_error(std::string_view message);
int report_io_error(std::string_view path, std::string_view message, int code);
int report_leftover(std::string_view path, std::string_view message);
std::optional<Options> parse_options(int argc, const char* const argv[]);
std::filesystem::path trace_header_path(const std::string& trace_path);
bool paths_are_distinct(const Options& options);
std::vector<TraceObject> trace_objects(const eawr::sim::Replay& replay);
void append_trace_rows(
    std::ostringstream& trace,
    const std::uint64_t tick,
    const std::vector<TraceObject>& objects,
    const std::vector<eawr::sim::EntityState>& entities);
std::string trace_header(
    const eawr::sim::Replay& replay,
    const std::string_view replay_sha256,
    const std::string_view build_sha256);
int publish(const std::vector<eawr::platform::PublishedFile>& outputs);
eawr::core::Result<std::unique_ptr<LoadedTables>> load_tables(const Options& options);
int run_tactical(const Options& options, const std::vector<std::uint8_t>& bytes);
void list_start(std::ostream& output, const eawr::skirmish::SkirmishStart& start, const std::string& state_sha256);
int run_scenario_mode(const Options& options);
int run_skirmish(const Options& options);
int run_replay(const std::optional<Options>& options);

} // namespace sim_headless::cli
