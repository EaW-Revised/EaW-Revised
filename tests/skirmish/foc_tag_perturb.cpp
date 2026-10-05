#include "foc_tag_perturb_support.hpp"
// The tag perturbation check (docs/tag-applied-check.md): does a value the tag registry marks
// applied really change the game? For each row of a plan, the M2 battle (the AI on both sides,
// m2_battle.hpp) runs on the FoC data with that one tag changed in memory (data::with_overrides)
// for the scene's objects of one type, and is compared tick by tick with the unchanged battle.
//
//   foc_tag_perturb --plan <plan.tsv> --out <results.jsonl> [--ticks 1500] [--jobs N] [--seed N]
//                   [--check-workers 4] [--check-every 10] [--after 300] [--game-root <install>]
//
// The plan is tab-separated with a header: `id class tag type change`.
//   - class: the object's XML element (SpaceUnit, Squadron, HardPoint, Projectile, ...); `*` any.
//   - tag: the tag path from the element, as the tag registry writes it (SpaceUnit/Max_Speed); the
//     class may be omitted from the path.
//   - type: station, ship, squadron or craft (the unit tables' kinds), hardpoint, projectile,
//     faction (the M2 start's factions) or constants (gameconstants.xml, a document override).
//   - change: auto, scale:<factor>, flip, drop or set:<text> (see change_values).
// Each row writes one JSON line (see row_json). Exit 0 when every row ran, 1 when the baseline
// is not deterministic or a row's worker comparison diverged, 2 on a bad command line or when
// the data does not load.

#include "m2_battle.hpp"
#include "soak_json.hpp"

#include "eawr/data/tag_trace.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace foc_tag_perturb_test_support {




[[nodiscard]] std::variant<Options, std::string> parse_options(const std::vector<std::string>& arguments) {
    Options options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& name = arguments[index];
        if (index + 1 >= arguments.size()) return name + " needs a value";
        const std::string& value = arguments[++index];
        const auto number = [&](const std::uint64_t low, const std::uint64_t high) { return whole(value, low, high); };
        if (name == "--plan") {
            options.plan = value;
        } else if (name == "--out") {
            options.out = value;
        } else if (name == "--game-root") {
            options.game_root = value;
        } else if (name == "--ticks") {
            const auto parsed = number(1, 100000);
            if (!parsed) return "--ticks takes 1 to 100000";
            options.ticks = *parsed;
        } else if (name == "--after") {
            const auto parsed = number(0, 100000);
            if (!parsed) return "--after takes 0 to 100000";
            options.after = *parsed;
        } else if (name == "--jobs") {
            const auto parsed = number(1, 256);
            if (!parsed) return "--jobs takes 1 to 256";
            options.jobs = static_cast<std::size_t>(*parsed);
        } else if (name == "--seed") {
            const auto parsed = number(0, UINT64_MAX);
            if (!parsed) return "--seed takes a whole number";
            options.seed = *parsed;
        } else if (name == "--check-workers") {
            const auto parsed = number(1, 256);
            if (!parsed) return "--check-workers takes 1 to 256";
            options.check_workers = static_cast<std::size_t>(*parsed);
        } else if (name == "--check-every") {
            const auto parsed = number(0, 1000000);
            if (!parsed) return "--check-every takes 0 to 1000000";
            options.check_every = static_cast<std::size_t>(*parsed);
        } else {
            return "unknown argument " + name;
        }
    }
    if (options.plan.empty()) return "--plan is required";
    if (options.out.empty()) return "--out is required";
    if (options.game_root.empty()) {
        if (auto root = environment("EAWR_EAW_GAME_ROOT")) options.game_root = *root;
    }
    if (options.game_root.empty()) return "--game-root (or EAWR_EAW_GAME_ROOT) is required";
    return options;
}

[[nodiscard]] std::variant<std::vector<PlanRow>, std::string> read_plan(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "cannot read the plan " + path.string();
    std::vector<PlanRow> rows;
    std::string line;
    std::size_t number = 0;
    while (std::getline(file, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const auto fields = split(line, '\t');
        if (number == 1 && !fields.empty() && fields.front() == "id") continue; // the header
        if (fields.size() != 5) return "plan line " + std::to_string(number) + ": expected 5 tab-separated fields";
        PlanRow row{fields[0], fields[1] == "*" ? std::string{} : fields[1], fields[2], lower(fields[3]), fields[4]};
        // The registry writes the tag path from the element: drop the class from it.
        if (!row.element.empty()) {
            const auto prefix = row.element + "/";
            if (row.tag.size() > prefix.size() && iequals(row.tag.substr(0, prefix.size()), prefix)) {
                row.tag = row.tag.substr(prefix.size());
            }
        }
        if (row.id.empty() || row.tag.empty()) return "plan line " + std::to_string(number) + ": empty id or tag";
        rows.push_back(std::move(row));
    }
    return rows;
}


// ---- One battle, observed ----------------------------------------------------------------


// Runs the battle for up to `ticks` ticks. With a baseline, it stops `after` ticks past the
} // namespace

using namespace foc_tag_perturb_test_support;

int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    auto parsed = parse_options(arguments);
    if (const auto* reason = std::get_if<std::string>(&parsed)) {
        std::cerr << "foc_tag_perturb: " << *reason << "\nusage: foc_tag_perturb --plan <plan.tsv> --out <results.jsonl> "
                  << "[--ticks N] [--jobs N] [--seed N] [--check-workers N] [--check-every N] [--after N] "
                  << "[--game-root <install>]\n";
        return 2;
    }
    const auto options = std::get<Options>(std::move(parsed));
    auto plan = read_plan(options.plan);
    if (const auto* reason = std::get_if<std::string>(&plan)) {
        std::cerr << "foc_tag_perturb: " << *reason << '\n';
        return 2;
    }
    const auto rows = std::get<std::vector<PlanRow>>(std::move(plan));
    const auto began = Clock::now();

    Installation installation;
    {
        std::vector<eawr::vfs::MountSpec> specs;
        for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                         std::pair{std::string("base"), std::string("GameData")}}) {
            auto manifest = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(options.game_root) / folder / "Data");
            if (!manifest) {
                std::cerr << "foc_tag_perturb: the FoC " << folder << " layer does not mount: " << manifest.error().message << '\n';
                return 2;
            }
            specs.push_back(std::move(manifest).value().mount);
        }
        auto filesystem = eawr::vfs::Vfs::mount(specs);
        if (!filesystem) {
            std::cerr << "foc_tag_perturb: the FoC vfs does not mount: " << filesystem.error().message << '\n';
            return 2;
        }
        installation.filesystem.emplace(std::move(filesystem).value());
    }
    auto catalog = data::load_catalog(*installation.filesystem, data::Profile::foc);
    if (!catalog) {
        std::cerr << "foc_tag_perturb: the FoC catalog does not load: " << catalog.error().message << '\n';
        return 2;
    }
    installation.catalog.emplace(std::move(catalog).value());
    installation.cache = std::make_unique<eawr::scene::VfsAssetCache>(*installation.filesystem);

    // The baseline: its loads are traced (which values a loader reads), then it runs once at one
    // worker and once at --check-workers.
    Baseline baseline;
    {
        data::tag_trace::Recording recording;
        auto tables = units::load_unit_tables(installation.input(installation.catalog->catalog));
        if (!tables) {
            std::cerr << "foc_tag_perturb: the FoC unit tables do not load: " << tables.error().message << '\n';
            return 2;
        }
        installation.tables.emplace(std::move(tables).value());
        const soak::Loaded loaded{*installation.filesystem, installation.catalog->catalog, *installation.tables};
        auto built = soak::build_battle(loaded, {.seed = options.seed, .anonymous_content = true, .schedule = std::nullopt, .journal = nullptr});
        if (const auto* reason = std::get_if<std::string>(&built)) {
            std::cerr << "foc_tag_perturb: the baseline battle does not build: " << *reason << '\n';
            return 2;
        }
        auto inputs = skirmish::read_start_inputs(skirmish::m2_fixture(), *installation.filesystem,
            installation.catalog->catalog, *installation.tables);
        if (inputs) {
            for (const auto& faction : inputs.value().factions) installation.factions.push_back(faction.name);
        }
        for (const auto& entry : recording.finish()) {
            if (entry.kind == data::tag_trace::Kind::used) installation.read.insert({entry.logical_path, entry.line, entry.column});
            if (entry.kind == data::tag_trace::Kind::document) installation.whole.insert(entry.logical_path);
        }
    }
    for (const auto& unit : installation.tables->units) baseline.kinds[skirmish::type_id(unit.id)] = kind_name(unit.kind);
    const soak::Loaded loaded{*installation.filesystem, installation.catalog->catalog, *installation.tables};
    const auto loaded_seconds = std::chrono::duration<double>(Clock::now() - began).count();
    baseline.run = run_battle(loaded, options.seed, 1, options.ticks, true, nullptr, 0, [](std::uint64_t, const Frame&) {});
    if (!baseline.run.error.empty()) {
        std::cerr << "foc_tag_perturb: the baseline battle failed: " << baseline.run.error << '\n';
        return 2;
    }
    const auto check = run_battle(loaded, options.seed, options.check_workers, baseline.run.hashes.size(), false, nullptr, 0,
        [](std::uint64_t, const Frame&) {});
    const bool deterministic = check.error.empty() && check.hashes == baseline.run.hashes;
    const auto baseline_seconds = std::chrono::duration<double>(Clock::now() - began).count() - loaded_seconds;

    std::ofstream out(options.out, std::ios::binary);
    if (!out) {
        std::cerr << "foc_tag_perturb: cannot write " << options.out.string() << '\n';
        return 2;
    }
    std::mutex out_mutex;
    out << "{\"baseline\": {\"ticks\": " << baseline.run.hashes.size() << ", \"seed\": " << options.seed
        << ", \"final_hash\": " << json_string(baseline.run.hashes.back()) << ", \"deterministic\": "
        << (deterministic ? "true" : "false") << ", \"check_workers\": " << options.check_workers
        << ", \"load_seconds\": " << json_number(loaded_seconds) << ", \"seconds\": " << json_number(baseline_seconds)
        << ", \"read_values\": " << installation.read.size() << ", \"rows\": " << rows.size() << "}}\n";
    out.flush();
    std::cout << "baseline: " << baseline.run.hashes.size() << " ticks, " << (deterministic ? "deterministic" : "NOT deterministic")
              << " at " << options.check_workers << " workers (load " << json_number(loaded_seconds) << " s, runs "
              << json_number(baseline_seconds) << " s)" << std::endl;
    if (!deterministic) {
        std::cerr << "foc_tag_perturb: the baseline differs at " << options.check_workers << " workers; no row is checked\n";
        return 1;
    }

    std::atomic<std::size_t> next = 0;
    std::atomic<std::size_t> diverged = 0;
    std::vector<std::thread> pool;
    for (std::size_t job = 0; job < std::min(options.jobs, rows.size()); ++job) {
        pool.emplace_back([&] {
            for (auto at = next++; at < rows.size(); at = next++) {
                RowResult result;
                result.row = rows[at];
                const auto started = Clock::now();
                const bool compare = options.check_every != 0 && at % options.check_every == 0;
                try {
                    check_row(installation, baseline, options, compare, result);
                } catch (const std::exception& error) {
                    result.verdict = "not checkable";
                    result.reason = std::string("an exception: ") + error.what();
                } catch (...) {
                    result.verdict = "not checkable";
                    result.reason = "an unknown exception";
                }
                result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
                if (result.diverged) ++diverged;
                const std::scoped_lock lock(out_mutex);
                out << row_json(result) << '\n';
                out.flush();
                std::cout << result.row.id << ": " << result.verdict << " (" << result.reason << ")" << std::endl;
            }
        });
    }
    for (auto& thread : pool) thread.join();
    std::cout << rows.size() << " row(s) in " << json_number(std::chrono::duration<double>(Clock::now() - began).count())
              << " s" << std::endl;
    return diverged == 0 ? 0 : 1;
}
