// The land path benchmark (docs/behaviour/land-pathfinding.md): path_bench --land loads a stock land map,
// derives the open ground from the map's own passability (the cell values that cover at least 2% of the
// grid: the bit assignments of the movement classes are content, not built in), and runs a fixed set of
// searches between open cells.
//
//   path_bench --land [--game-root <dir>] [--map <name part>|all] [--searches 200] [--workers 1,4]
//              [--slice 400] [--quota 4000] [--radius 0] [--width 0] [--timing on|off]
//
// It prints the work per search (deterministic: this is what a regression pins), the host time per search,
// and a throttled sliced run of the same searches through LandSliceScheduler at every worker count. Exits 1
// when a worker count's results differ from the synchronous plans. Without the game data it prints SKIPPED
// and exits 0.

#include "melee.hpp"

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/land_path.hpp"
#include "eawr/skirmish/land_terrain.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::bench {
namespace {
namespace tactical = eawr::sim::tactical;
using sim::math::Fixed;
using Clock = std::chrono::steady_clock;

struct Options {
    std::optional<std::string> game_root;
    std::string map = "";  // empty: the first land map; "all": every land map; otherwise a name part
    std::size_t searches = 200;
    std::vector<std::size_t> workers{1, 4};
    std::uint64_t slice = 400;
    std::uint64_t quota = 4000;
    std::int64_t radius = 0;
    std::int64_t width = 0;
    bool timing = true;
};

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

template <typename T>
std::optional<T> number(const std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

std::optional<Options> parse(const int argc, const char* const argv[]) {
    Options options;
    options.game_root = environment("EAWR_EAW_GAME_ROOT");
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--land") continue;
        if (index + 1 >= argc) return std::nullopt;
        const std::string_view value(argv[++index]);
        if (argument == "--game-root") options.game_root = std::string(value);
        else if (argument == "--map") options.map = std::string(value);
        else if (argument == "--searches") {
            const auto count = number<std::size_t>(value);
            if (!count || *count == 0 || *count > 100'000) return std::nullopt;
            options.searches = *count;
        } else if (argument == "--workers") {
            options.workers.clear();
            std::string_view rest = value;
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                const auto count = number<std::size_t>(rest.substr(0, comma));
                if (!count || *count == 0 || *count > platform::ThreadWorkerAdapter::max_worker_count) return std::nullopt;
                options.workers.push_back(*count);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
            if (options.workers.empty()) return std::nullopt;
        } else if (argument == "--slice" || argument == "--quota") {
            const auto count = number<std::uint64_t>(value);
            if (!count || *count == 0) return std::nullopt;
            (argument == "--slice" ? options.slice : options.quota) = *count;
        } else if (argument == "--radius" || argument == "--width") {
            const auto units = number<std::int64_t>(value);
            if (!units || *units < 0 || *units > 10'000) return std::nullopt;
            (argument == "--radius" ? options.radius : options.width) = *units;
        } else if (argument == "--timing") {
            if (value != "on" && value != "off") return std::nullopt;
            options.timing = value == "on";
        } else return std::nullopt;
    }
    return options;
}

std::uint64_t fold(std::uint64_t hash, const std::uint64_t value) {
    hash ^= value;
    return hash * 1099511628211ULL;
}
std::uint64_t digest(const tactical::LandPath& path) {
    auto hash = fold(14695981039346656037ULL, static_cast<std::uint64_t>(path.status));
    hash = fold(hash, static_cast<std::uint64_t>(path.length.raw()));
    for (const auto& node : path.nodes) {
        hash = fold(hash, static_cast<std::uint64_t>(node.x.raw()));
        hash = fold(hash, static_cast<std::uint64_t>(node.y.raw()));
    }
    return hash;
}

int bench_map(const std::string& name, const assets::Map& map, const Options& options) {
    std::map<std::uint32_t, std::size_t> histogram;
    const auto& raw = map.raw_passability;
    for (std::size_t i = 0; i + 3 < raw.size(); i += 4) {
        std::uint32_t bits = 0;
        for (unsigned b = 0; b < 4; ++b) bits |= std::to_integer<std::uint32_t>(raw[i + b]) << (8U * b);
        ++histogram[bits];
    }
    const auto cells = raw.size() / 4;
    std::uint32_t open_bits = 0;
    for (const auto& [bits, count] : histogram) if (count * 50 >= cells) open_bits |= bits;
    auto loaded = skirmish::load_land_terrain(map, {{"open_ground", open_bits}});
    if (!loaded) { std::cerr << name << ": " << loaded.error().message << '\n'; return 1; }
    auto terrain = std::make_shared<const tactical::LandTerrain>(std::move(loaded).value());
    const std::uint32_t clear_bit = open_bits & (~open_bits + 1);
    if (clear_bit == 0) return 0;
    auto created = tactical::LandOccupancy::create(terrain, clear_bit);
    if (!created) return 0;
    const auto occupancy = std::move(created).value();
    const auto mask = *terrain->movement_mask("open_ground");
    std::vector<std::uint32_t> open_cells;
    for (std::uint32_t cell = 0; cell < occupancy.cells().size(); ++cell) {
        if ((occupancy.cells()[cell] & ~mask) == 0) open_cells.push_back(cell);
    }
    if (open_cells.size() < 100) return 0;
    std::uint64_t state = 0x9E3779B97F4A7C15ULL ^ open_cells.size();
    const auto next = [&] { state = state * 6364136223846793005ULL + 1442695040888963407ULL; return state >> 33; };
    std::vector<tactical::LandPathRequest> requests;
    for (std::size_t i = 0; i < options.searches; ++i) {
        tactical::LandPathRequest request;
        request.entity = static_cast<sim::EntityId>(i + 1);
        request.mask = mask;
        request.radius = Fixed::from_integer(options.radius).value();
        request.width = Fixed::from_integer(options.width).value();
        const auto from = open_cells[next() % open_cells.size()], to = open_cells[next() % open_cells.size()];
        const auto spacing = terrain->spacing();
        request.start = {tactical::land_cell_centre(from % terrain->width(), spacing), tactical::land_cell_centre(from / terrain->width(), spacing)};
        request.end = {tactical::land_cell_centre(to % terrain->width(), spacing), tactical::land_cell_centre(to / terrain->width(), spacing)};
        requests.push_back(request);
    }

    // Synchronous plans: the deterministic work and the host time per search.
    tactical::LandPathArena arena;
    std::vector<tactical::LandPath> plans;
    std::vector<std::uint64_t> works;
    std::uint64_t found = 0, expansions = 0, evaluations = 0, nanoseconds = 0;
    for (const auto& request : requests) {
        tactical::LandPathStats stats;
        const auto start = Clock::now();
        auto plan = tactical::LandPathBackend::plan(occupancy, arena, request, &stats);
        if (options.timing) nanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        found += plan.status == tactical::LandPathStatus::found;
        expansions += stats.expansions;
        evaluations += stats.evaluations;
        works.push_back(stats.work);
        plans.push_back(std::move(plan));
    }
    auto sorted = works;
    std::sort(sorted.begin(), sorted.end());
    std::uint64_t total = 0;
    for (const auto work : works) total += work;
    std::cout << "map " << name << ": " << terrain->width() << "x" << terrain->height() << " cells, " << open_cells.size()
              << " open, mask bits 0x" << std::hex << mask << std::dec << ", radius " << options.radius << ", width " << options.width << '\n';
    std::cout << "  searches " << requests.size() << ", routed " << found << ", work/search mean " << total / requests.size()
              << " p50 " << sorted[sorted.size() / 2] << " p95 " << sorted[sorted.size() * 95 / 100] << " max " << sorted.back()
              << ", expansions/search " << expansions / requests.size() << ", footprint tests/search " << evaluations / requests.size() << '\n';
    if (options.timing) {
        std::cout << "  host time per search " << std::fixed << std::setprecision(1) << static_cast<double>(nanoseconds) / requests.size() / 1000.0
                  << " us (synchronous, one thread)\n";
    }
    std::uint64_t reference = 14695981039346656037ULL;
    for (const auto& plan : plans) reference = fold(reference, digest(plan));
    std::cout << "  plans digest " << std::hex << reference << std::dec << '\n';

    // The same searches through the throttled scheduler, per worker count.
    bool agree = true;
    for (const auto workers : options.workers) {
        platform::ThreadWorkerAdapter executor(workers);
        tactical::LandPathArena sliced_arena;
        std::vector<std::unique_ptr<tactical::LandSearch>> searches;
        std::vector<tactical::LandSearch*> pointers;
        for (const auto& request : requests) {
            searches.push_back(std::make_unique<tactical::LandSearch>(occupancy, sliced_arena, request));
            pointers.push_back(searches.back().get());
        }
        tactical::LandPathAdmission admission(options.quota, 1);
        tactical::LandSliceScheduler scheduler;
        std::uint64_t frames = 0, denied = 0;
        const auto start = Clock::now();
        for (; frames < 1'000'000; ++frames) {
            admission.begin_frame(frames);
            auto report = scheduler.run(executor, pointers, admission, options.slice);
            if (!report) { std::cerr << report.error().message << '\n'; return 1; }
            denied += report.value().denied;
            if (report.value().admitted == 0 && report.value().denied == 0) break;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto& search : searches) hash = fold(hash, digest(search->result()));
        std::cout << "  workers " << workers << ": slice " << options.slice << ", quota " << options.quota << ": " << frames
                  << " ticks to finish, " << denied << " refused slices";
        if (options.timing) std::cout << ", " << std::fixed << std::setprecision(1) << static_cast<double>(elapsed) / 1000.0 << " ms total";
        std::cout << ", digest " << std::hex << hash << std::dec << '\n';
        if (hash != reference) { std::cout << "    FAIL: the sliced results differ from the synchronous plans\n"; agree = false; }
    }
    return agree ? 0 : 1;
}
} // namespace

int land_main(const int argc, const char* const argv[]) {
    const auto options = parse(argc, argv);
    if (!options) {
        std::cerr << "usage: path_bench --land [--game-root <dir>] [--map <name part>|all] [--searches <n>] [--workers <n>[,<n>...]]\n"
                     "                  [--slice <work>] [--quota <work>] [--radius <units>] [--width <units>] [--timing on|off]\n";
        return 2;
    }
    if (!options->game_root) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the land path benchmark\n";
        return 0;
    }
    std::vector<vfs::MountSpec> mounts;
    for (const auto& folder : {"corruption", "GameData"}) {
        auto manifest = vfs::resolve_manifest_mount(folder, std::filesystem::path(*options->game_root) / folder / "Data");
        if (!manifest) { std::cerr << manifest.error().message << '\n'; return 1; }
        mounts.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = vfs::Vfs::mount(mounts);
    if (!filesystem) return 1;
    auto records = filesystem.value().enumerate("data/art/maps/", ".ted");
    if (!records) return 1;
    int status = 0;
    bool any = false;
    for (const auto& record : records.value()) {
        if (!options->map.empty() && options->map != "all" && record.canonical_path.find(options->map) == std::string::npos) continue;
        auto map = assets::load_map(filesystem.value(), record.canonical_path);
        if (!map || map.value().kind != assets::MapKind::land) continue;
        any = true;
        status |= bench_map(record.canonical_path, map.value(), *options);
        if (options->map.empty()) break;
    }
    if (!any) { std::cerr << "path_bench --land: no land map matched\n"; return 1; }
    return status;
}

} // namespace eawr::bench
