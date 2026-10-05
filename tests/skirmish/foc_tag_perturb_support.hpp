#pragma once

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

namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace units = eawr::units;
namespace data = eawr::data;
namespace soak = eawr::soak;
using Clock = std::chrono::steady_clock;
using soak::json_number;
using soak::json_string;

struct Options {
    std::filesystem::path plan;
    std::filesystem::path out;
    std::string game_root;
    std::uint64_t ticks{1500};
    std::uint64_t after{300}; // ticks run past the first difference, for the observables
    std::size_t jobs{1};
    std::uint64_t seed{1};
    std::size_t check_workers{4};
    std::size_t check_every{10}; // every Nth row also runs at check_workers; 0 never
};

struct PlanRow {
    std::string id;
    std::string element; // the class, or empty for any
    std::string tag;     // the path below the element
    std::string type;
    std::string change;
};
struct Changed {
    std::vector<std::string> values;
    std::string how; // scaled, flipped, dropped, set
};

constexpr std::string_view constants_path = "data/xml/gameconstants.xml";

struct Installation {
    std::optional<eawr::vfs::Vfs> filesystem;
    std::optional<data::LoadResult> catalog;
    std::unique_ptr<eawr::scene::VfsAssetCache> cache;
    std::mutex cache_mutex; // the cache memoises; the rows load their tables in parallel
    std::optional<units::UnitTables> tables;
    std::set<std::tuple<std::string, std::uint64_t, std::uint64_t>> read; // the baseline's tag trace
    std::vector<std::string> factions; // the M2 start's factions
    std::set<std::string> whole;       // files a loader takes whole (tag_trace::document)

    [[nodiscard]] units::LoadInput input(const data::Catalog& source);
};
// The occurrences of a tag path in a resolved object (the last occurrence of each container),
// with the node of the last one.
struct Found {
    std::vector<std::string> values;
    const data::XmlNode* node{};
};
// The units' states at one completed tick, by entity id.
using Frame = std::vector<tactical::UnitState>;

struct Run {
    std::vector<std::string> hashes; // completed tick 1..n
    std::vector<Frame> frames;       // kept for the baseline only
    std::string error;
};
// first differing hash and fills `observe` on every tick.
template <typename Observe>
Run run_battle(const soak::Loaded& loaded, const std::uint64_t seed, const std::size_t workers, const std::uint64_t ticks,
    const bool keep_frames, const std::vector<std::string>* baseline, const std::uint64_t after, Observe&& observe) {
    Run run;
    auto built = soak::build_battle(loaded, {.seed = seed, .anonymous_content = true, .schedule = std::nullopt, .journal = nullptr});
    if (const auto* reason = std::get_if<std::string>(&built)) {
        run.error = *reason;
        return run;
    }
    auto& battle = *std::get<std::unique_ptr<soak::Battle>>(built);
    auto& session = *battle.session;
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::optional<std::uint64_t> differs;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        if (!stepped) {
            run.error = "step " + std::to_string(tick + 1) + ": " + stepped.error().code + ' ' + stepped.error().message;
            return run;
        }
        run.hashes.push_back(stepped.value().state_sha256);
        auto units = session.world().units();
        std::sort(units.begin(), units.end(), [](const auto& left, const auto& right) { return left.entity_id < right.entity_id; });
        observe(tick, units);
        if (keep_frames) run.frames.push_back(std::move(units));
        if (baseline != nullptr && !differs && (tick >= baseline->size() || (*baseline)[tick] != run.hashes.back())) {
            differs = tick;
        }
        if (differs && tick >= *differs + after) break;
        const auto& decided = session.world().outcome();
        if (decided && tick + 1 >= decided->end_tick) break;
    }
    return run;
}

struct KindDelta {
    std::optional<std::uint64_t> first_tick; // the first completed tick a unit of the kind differs
    double position{};                        // the largest distance from its baseline position
    double height{};                          // the largest |z| difference
    std::size_t units{};                      // units of the kind that differed
};

// ---- A row --------------------------------------------------------------------------------

struct ObjectChange {
    std::string id;
    std::vector<std::string> from;
    std::vector<std::string> to;
    bool read{};
    bool traced{true}; // false: the loaders take its file whole, so a read of one value is not traced
};

struct RowResult {
    PlanRow row;
    std::string verdict; // changes, no change, not checkable
    std::string reason;
    std::vector<ObjectChange> objects;
    std::vector<std::string> skipped; // objects of the type without the tag
    bool tables_changed{};
    std::optional<std::uint64_t> first_tick;
    std::map<std::string, KindDelta> kinds;
    std::uint64_t ticks_run{};
    std::string workers_check; // empty: not checked
    bool diverged{};
    bool retried{}; // the gentler change after a doubled one made the data invalid
    double seconds{};
};
struct Baseline {
    Run run;
    std::map<tactical::TypeId, std::string> kinds; // type id -> kind name
};

[[nodiscard]] std::string lower(std::string_view text);
[[nodiscard]] bool iequals(const std::string_view left, const std::string_view right);
[[nodiscard]] std::string trim(std::string_view text);
[[nodiscard]] std::vector<std::string> split(const std::string_view text, const char separator);
[[nodiscard]] std::optional<std::string> environment(const char* name);
[[nodiscard]] std::optional<std::uint64_t> whole(const std::string_view text, const std::uint64_t low, const std::uint64_t high);
[[nodiscard]] std::variant<Changed, std::string> change_values(const std::string& change,
    const std::vector<std::string>& current);
[[nodiscard]] std::string kind_name(const units::UnitKind kind);
[[nodiscard]] std::optional<std::vector<std::string>> objects_of(const units::UnitTables& tables,
    const std::vector<std::string>& factions, const std::string& type);
[[nodiscard]] Found find_values(const std::vector<const data::XmlNode*>& top, const std::string& tag);
[[nodiscard]] Found find_values(const data::EffectiveObject& object, const std::string& tag);
[[nodiscard]] Found find_values(const data::XmlNode& root, const std::string& tag);
[[nodiscard]] std::string row_json(const RowResult& result);
void check_row(Installation& installation, const Baseline& baseline, const Options& options, const bool compare,
    RowResult& result);

} // namespace foc_tag_perturb_test_support
