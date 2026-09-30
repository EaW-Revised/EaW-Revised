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

namespace {

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

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& letter : result) {
        if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter - 'A' + 'a');
    }
    return result;
}

[[nodiscard]] bool iequals(const std::string_view left, const std::string_view right) {
    return lower(left) == lower(right);
}

[[nodiscard]] std::string trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

[[nodiscard]] std::vector<std::string> split(const std::string_view text, const char separator) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (true) {
        const auto end = text.find(separator, begin);
        parts.emplace_back(text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return parts;
}

[[nodiscard]] std::optional<std::string> environment(const char* name) {
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

[[nodiscard]] std::optional<std::uint64_t> whole(const std::string_view text, const std::uint64_t low, const std::uint64_t high) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < low || value > high) return std::nullopt;
    return value;
}

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

// ---- The change a row makes to one value ----------------------------------------------------

[[nodiscard]] std::optional<double> decimal(const std::string& token) {
    if (token.empty()) return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

[[nodiscard]] std::string format_decimal(const double value) {
    std::ostringstream text;
    text.precision(6);
    text << std::fixed << value;
    auto result = text.str();
    while (result.size() > 1 && result.back() == '0') result.pop_back();
    if (!result.empty() && result.back() == '.') result.pop_back();
    return result;
}

// Tokens of a list value: comma or whitespace separated, as FoC's list tags are.
[[nodiscard]] std::vector<std::string> tokens(const std::string& text) {
    std::vector<std::string> result;
    std::string current;
    for (const char letter : text) {
        if (letter == ',' || letter == ' ' || letter == '\t' || letter == '\r' || letter == '\n') {
            if (!current.empty()) result.push_back(std::move(current));
            current.clear();
        } else {
            current += letter;
        }
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

[[nodiscard]] std::optional<std::string> flipped(const std::string& text) {
    const auto value = lower(trim(text));
    const bool upper = !text.empty() && std::isupper(static_cast<unsigned char>(trim(text).front()));
    const auto cased = [&](std::string word) {
        if (upper) word.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(word.front())));
        return word;
    };
    if (value == "yes") return cased("no");
    if (value == "no") return cased("yes");
    if (value == "true") return cased("false");
    if (value == "false") return cased("true");
    return std::nullopt;
}

struct Changed {
    std::vector<std::string> values;
    std::string how; // scaled, flipped, dropped, set
};

// The new occurrences of a value, or why this change does not apply to it. `auto` scales every
// number of every occurrence by 2 (a zero to 1; names between them stay); flips a boolean; drops the
// first entry of a list of names, or the last occurrence of a repeated tag. A single name (an
// enum or a reference) needs a `set:` change: which other value is valid is the registry's call.
[[nodiscard]] std::variant<Changed, std::string> change_values(const std::string& change,
    const std::vector<std::string>& current) {
    if (change.rfind("set:", 0) == 0) return Changed{{change.substr(4)}, "set"};
    if (current.empty()) return std::string("the object has no value to change");
    if (change.rfind("drop:", 0) == 0) {
        const auto member = change.substr(5);
        if (member.empty()) return std::string("drop:<member> requires a named member");
        auto values = current;
        bool removed = false;
        for (auto& value : values) {
            std::string kept;
            for (const auto& word : tokens(value)) {
                if (iequals(word, member)) { removed = true; continue; }
                kept += (kept.empty() ? "" : ", ") + word;
            }
            value = kept;
        }
        if (!removed) return std::string("the named member is absent: ") + member;
        return Changed{std::move(values), "dropped named member"};
    }
    const auto& text = current.back();
    const auto words = tokens(text);
    std::string kind = change;
    double factor = 2.0;
    if (change.rfind("scale:", 0) == 0) {
        const auto parsed = decimal(change.substr(6));
        if (!parsed) return "bad scale factor in " + change;
        factor = *parsed;
        kind = "scale";
    }
    // A number anywhere (a numeric list, or name-and-number tuples such as Damage_To_Armor_Mod's)
    // is scaled in every occurrence; the names stay.
    const bool numeric = std::any_of(current.begin(), current.end(), [](const std::string& value) {
        const auto parts = tokens(value);
        return std::any_of(parts.begin(), parts.end(), [](const std::string& word) { return decimal(word).has_value(); });
    });
    if (kind == "auto") {
        if (numeric) kind = "scale";
        else if (flipped(text)) kind = "flip";
        else if (current.size() > 1 || words.size() > 1) kind = "drop";
        else return std::string("a single name: the row needs a set:<value> change");
    }
    auto values = current;
    if (kind == "scale") {
        if (!numeric) return std::string("not a number: ") + text;
        for (auto& value : values) {
            std::string scaled;
            for (const auto& word : tokens(value)) {
                if (!scaled.empty()) scaled += ", ";
                const auto number = decimal(word);
                scaled += number ? format_decimal(*number == 0.0 ? 1.0 : *number * factor) : word;
            }
            value = scaled;
        }
        return Changed{values, "scaled"};
    }
    if (kind == "flip") {
        const auto other = flipped(text);
        if (!other) return std::string("not a boolean: ") + text;
        values.back() = *other;
        return Changed{values, "flipped"};
    }
    if (kind == "drop") {
        if (current.size() > 1) {
            values.pop_back();
            return Changed{values, "dropped"};
        }
        if (words.size() < 2) return std::string("a list of one entry: dropping it removes the tag, use set:");
        std::string rest;
        for (std::size_t index = 1; index < words.size(); ++index) rest += (index > 1 ? ", " : "") + words[index];
        values.back() = rest;
        return Changed{values, "dropped"};
    }
    return "unknown change " + change;
}

// ---- The loaded installation and its scene objects ----------------------------------------

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

    [[nodiscard]] units::LoadInput input(const data::Catalog& source) {
        units::LoadInput result;
        result.catalog = &source;
        result.filesystem = &*filesystem;
        result.model = [this](const std::string_view path) {
            const std::scoped_lock lock(cache_mutex);
            return cache->model(path);
        };
        return result;
    }
};

[[nodiscard]] std::string kind_name(const units::UnitKind kind) {
    switch (kind) {
    case units::UnitKind::station: return "station";
    case units::UnitKind::ship: return "ship";
    case units::UnitKind::squadron: return "squadron";
    case units::UnitKind::craft: return "craft";
    }
    return "other";
}

// The scene's object ids of a type, in table order.
[[nodiscard]] std::optional<std::vector<std::string>> objects_of(const units::UnitTables& tables,
    const std::vector<std::string>& factions, const std::string& type) {
    std::vector<std::string> ids;
    const auto add = [&](const std::string& id) {
        if (std::none_of(ids.begin(), ids.end(), [&](const std::string& seen) { return iequals(seen, id); })) {
            ids.push_back(id);
        }
    };
    if (type == "station" || type == "ship" || type == "squadron" || type == "craft") {
        for (const auto& unit : tables.units) if (kind_name(unit.kind) == type) add(unit.id);
    } else if (type == "hardpoint") {
        for (const auto& unit : tables.units) for (const auto& hardpoint : unit.hardpoints) add(hardpoint.id);
    } else if (type == "projectile") {
        for (const auto& projectile : tables.projectiles) add(projectile.id);
    } else if (type == "faction") {
        for (const auto& faction : factions) add(faction);
    } else {
        return std::nullopt;
    }
    return ids;
}

// The occurrences of a tag path in a resolved object (the last occurrence of each container),
// with the node of the last one.
struct Found {
    std::vector<std::string> values;
    const data::XmlNode* node{};
};

[[nodiscard]] Found find_values(const std::vector<const data::XmlNode*>& top, const std::string& tag) {
    Found found;
    const auto path = split(tag, '/');
    if (path.size() == 1) {
        for (const auto* value : top) {
            if (!iequals(value->name, path.front())) continue;
            found.values.push_back(value->raw_text);
            found.node = value;
        }
        return found;
    }
    const data::XmlNode* container = nullptr;
    for (const auto* value : top) if (iequals(value->name, path.front())) container = value;
    for (std::size_t index = 1; container != nullptr && index + 1 < path.size(); ++index) {
        const data::XmlNode* next = nullptr;
        for (const auto& child : container->children) if (iequals(child.name, path[index])) next = &child;
        container = next;
    }
    if (container == nullptr) return found;
    for (const auto& child : container->children) {
        if (!iequals(child.name, path.back())) continue;
        found.values.push_back(child.raw_text);
        found.node = &child;
    }
    return found;
}

[[nodiscard]] Found find_values(const data::EffectiveObject& object, const std::string& tag) {
    std::vector<const data::XmlNode*> top;
    for (const auto& value : object.values) top.push_back(&value.value);
    return find_values(top, tag);
}

[[nodiscard]] Found find_values(const data::XmlNode& root, const std::string& tag) {
    std::vector<const data::XmlNode*> top;
    for (const auto& child : root.children) top.push_back(&child);
    return find_values(top, tag);
}

// ---- One battle, observed ----------------------------------------------------------------

// The units' states at one completed tick, by entity id.
using Frame = std::vector<tactical::UnitState>;

struct Run {
    std::vector<std::string> hashes; // completed tick 1..n
    std::vector<Frame> frames;       // kept for the baseline only
    std::string error;
};

// Runs the battle for up to `ticks` ticks. With a baseline, it stops `after` ticks past the
// first differing hash and fills `observe` on every tick.
template <typename Observe>
Run run_battle(const soak::Loaded& loaded, const std::uint64_t seed, const std::size_t workers, const std::uint64_t ticks,
    const bool keep_frames, const std::vector<std::string>* baseline, const std::uint64_t after, Observe&& observe) {
    Run run;
    auto built = soak::build_battle(loaded, {.seed = seed, .anonymous_content = true});
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

[[nodiscard]] std::string row_json(const RowResult& result) {
    std::ostringstream json;
    json << "{\"id\": " << json_string(result.row.id) << ", \"class\": " << json_string(result.row.element)
         << ", \"tag\": " << json_string(result.row.tag) << ", \"type\": " << json_string(result.row.type)
         << ", \"change\": " << json_string(result.row.change) << ", \"verdict\": " << json_string(result.verdict)
         << ", \"reason\": " << json_string(result.reason) << ", \"objects\": [";
    for (std::size_t index = 0; index < result.objects.size(); ++index) {
        const auto& object = result.objects[index];
        const auto list = [](const std::vector<std::string>& values) {
            std::string text = "[";
            for (std::size_t at = 0; at < values.size(); ++at) text += (at ? ", " : "") + json_string(values[at]);
            return text + "]";
        };
        json << (index ? ", " : "") << "{\"id\": " << json_string(object.id) << ", \"from\": " << list(object.from)
             << ", \"to\": " << list(object.to) << ", \"read\": " << (object.traced ? (object.read ? "true" : "false") : "null") << "}";
    }
    json << "], \"without_tag\": " << result.skipped.size() << ", \"tables_changed\": "
         << (result.tables_changed ? "true" : "false") << ", \"first_tick\": "
         << (result.first_tick ? std::to_string(*result.first_tick) : std::string("null")) << ", \"kinds\": {";
    bool first = true;
    for (const auto& [kind, delta] : result.kinds) {
        if (!delta.first_tick) continue;
        json << (first ? "" : ", ") << json_string(kind) << ": {\"first_tick\": " << *delta.first_tick
             << ", \"units\": " << delta.units << ", \"max_position_delta\": " << json_number(delta.position)
             << ", \"max_height_delta\": " << json_number(delta.height) << "}";
        first = false;
    }
    json << "}, \"ticks_run\": " << result.ticks_run << ", \"workers_check\": "
         << (result.workers_check.empty() ? std::string("null") : json_string(result.workers_check))
         << ", \"seconds\": " << json_number(result.seconds) << "}";
    return json.str();
}

[[nodiscard]] double to_double(const eawr::sim::math::Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(eawr::sim::math::Fixed::scale);
}

[[nodiscard]] double distance(const eawr::sim::math::Vec3& left, const eawr::sim::math::Vec3& right) {
    const double x = to_double(left.x) - to_double(right.x);
    const double y = to_double(left.y) - to_double(right.y);
    const double z = to_double(left.z) - to_double(right.z);
    return std::sqrt(x * x + y * y + z * z);
}

struct Baseline {
    Run run;
    std::map<tactical::TypeId, std::string> kinds; // type id -> kind name
};

void check_row(Installation& installation, const Baseline& baseline, const Options& options, const bool compare,
    RowResult& result) {
    const auto& row = result.row;
    // The AI takes these constants through VFS bytes, outside DocumentOverrides.
    if (row.type == "constants" && lower(row.tag).starts_with("ai_")) {
        result.verdict = "not checkable";
        result.reason = "read as raw bytes: the AI XML bypasses document overrides";
        return;
    }
    const auto leaf = lower(split(row.tag, '/').back());
    const bool membership = leaf == "behavior" || leaf == "spacebehavior" || leaf == "attributes"
        || leaf == "categorymask" || leaf == "property_flags" || leaf.ends_with("_categories")
        || leaf.ends_with("_properties") || leaf.ends_with("_restrictions") || leaf.ends_with("_exclusions");
    if (membership && (row.change == "auto" || row.change == "drop")) {
        result.verdict = "not checkable";
        result.reason = "membership list: use drop:<member> or set:<value> to name the tested member";
        return;
    }
    const auto& base_catalog = installation.catalog->catalog;
    std::vector<data::ValueOverride> overrides;
    std::vector<data::DocumentOverride> documents;
    // One object's (or the document's) values: the change it gets, or false when the row is
    // not checkable.
    const auto take = [&](const std::string& id, const Found& found, auto&& add) {
        if (found.values.empty() && row.change.rfind("set:", 0) != 0) {
            result.skipped.push_back(id);
            return true;
        }
        auto changed = change_values(row.change, found.values);
        if (const auto* reason = std::get_if<std::string>(&changed)) {
            result.verdict = "not checkable";
            result.reason = id + ": " + *reason;
            return false;
        }
        auto& made = std::get<Changed>(changed);
        const auto trimmed = [](const std::vector<std::string>& values) {
            std::vector<std::string> result;
            for (const auto& value : values) result.push_back(trim(value));
            return result;
        };
        if (trimmed(made.values) == trimmed(found.values)) {
            result.skipped.push_back(id); // a set: value it already has
            return true;
        }
        ObjectChange object{id, found.values, made.values, false};
        object.traced = found.node != nullptr; // an unauthored tag has no baseline read evidence
        if (found.node != nullptr) {
            const auto& source = found.node->source;
            object.read = installation.read.contains({source.logical_path, source.line, source.column});
            object.traced = !installation.whole.contains(source.logical_path);
        }
        result.objects.push_back(std::move(object));
        add(std::move(made.values));
        return true;
    };
    if (row.type == "constants") {
        // GameConstants: the one document the start, the tables and the AI read it from.
        auto document = data::load_document(*installation.filesystem, constants_path);
        if (!document) {
            result.verdict = "not checkable";
            result.reason = "gameconstants.xml does not load: " + document.error().message;
            return;
        }
        if (row.element.empty() || iequals(document.value().root.name, row.element)) {
            const auto found = find_values(document.value().root, row.tag);
            if (!take(std::string(constants_path), found, [&](std::vector<std::string> values) {
                    documents.push_back({std::string(constants_path), row.tag, std::move(values)});
                })) {
                return;
            }
        }
    } else {
        const auto ids = objects_of(*installation.tables, installation.factions, row.type);
        if (!ids) {
            result.verdict = "not checkable";
            result.reason = "type " + row.type + " has no objects the headless battle builds";
            return;
        }
        for (const auto& id : *ids) {
            auto resolved = base_catalog.resolve(id);
            if (!resolved) continue;
            if (!row.element.empty() && !iequals(resolved.value().type_name, row.element)) continue;
            if (!take(id, find_values(resolved.value(), row.tag), [&](std::vector<std::string> values) {
                    overrides.push_back({id, row.tag, std::move(values)});
                })) {
                return;
            }
        }
    }
    if (overrides.empty() && documents.empty()) {
        result.verdict = "no change";
        result.reason = result.skipped.empty()
            ? "not exercised: the scene has no " + row.type + " of class " + (row.element.empty() ? "*" : row.element)
            : "not exercised: no " + row.type + " in the scene authors or inherits it, or the change is the value it has";
        return;
    }
    // The document change holds for this thread's loads until the row is done.
    const data::DocumentOverrides scope(documents);
    auto catalog = data::with_overrides(base_catalog, overrides);
    if (!catalog) {
        result.verdict = "not checkable";
        result.reason = "the override failed: " + catalog.error().message;
        return;
    }
    auto tables = units::load_unit_tables(installation.input(catalog.value()));
    if (!tables) {
        result.verdict = "not checkable";
        result.reason = "the unit tables do not load: " + tables.error().message;
        return;
    }
    result.tables_changed = tables.value() != *installation.tables;
    const soak::Loaded loaded{*installation.filesystem, catalog.value(), tables.value()};

    const auto& frames = baseline.run.frames;
    auto observe = [&](const std::uint64_t tick, const Frame& units) {
        if (tick >= frames.size()) return;
        const auto& before = frames[tick];
        std::size_t at = 0;
        const auto differ = [&](const tactical::UnitState& unit, const double moved) {
            const auto kind = baseline.kinds.contains(unit.type_id) ? baseline.kinds.at(unit.type_id) : std::string("other");
            auto& delta = result.kinds[kind];
            if (!delta.first_tick) delta.first_tick = tick + 1;
            ++delta.units;
            delta.position = std::max(delta.position, moved);
        };
        for (const auto& unit : units) {
            while (at < before.size() && before[at].entity_id < unit.entity_id) differ(before[at++], 0.0);
            if (at < before.size() && before[at].entity_id == unit.entity_id) {
                const auto& old = before[at++];
                if (!(old == unit)) {
                    differ(unit, distance(old.position, unit.position));
                    const auto kind = baseline.kinds.contains(unit.type_id) ? baseline.kinds.at(unit.type_id) : std::string("other");
                    auto& delta = result.kinds[kind];
                    delta.height = std::max(delta.height, std::abs(to_double(old.position.z) - to_double(unit.position.z)));
                }
            } else {
                differ(unit, 0.0); // a unit the baseline does not have
            }
        }
        while (at < before.size()) differ(before[at++], 0.0);
    };
    auto run = run_battle(loaded, options.seed, 1, options.ticks, false, &baseline.run.hashes, options.after, observe);
    result.ticks_run = run.hashes.size();
    for (std::size_t tick = 0; tick < run.hashes.size() && tick < baseline.run.hashes.size(); ++tick) {
        if (run.hashes[tick] != baseline.run.hashes[tick]) {
            result.first_tick = tick + 1;
            break;
        }
    }
    if (!result.first_tick && run.hashes.size() != baseline.run.hashes.size()) {
        result.first_tick = std::min(run.hashes.size(), baseline.run.hashes.size()) + 1; // one battle ended earlier
    }
    if (run.hashes.empty() && !run.error.empty()) {
        // The change made the data invalid (the battle does not build): that shows the value is
        // validated, not that it is applied. A doubled number gets one gentler try, halved.
        if (row.change == "auto" && !result.retried) {
            RowResult gentler;
            gentler.row = row;
            gentler.row.change = "scale:0.5";
            gentler.retried = true;
            check_row(installation, baseline, options, compare, gentler);
            gentler.row.change = row.change;
            gentler.reason = "doubled, the battle does not build (" + run.error + "); halved: " + gentler.reason;
            result = std::move(gentler);
            return;
        }
        result.verdict = "not checkable";
        result.reason = "the change makes the data invalid, the battle does not build: " + run.error
            + " (give the row a set: hint)";
    } else if (!run.error.empty()) {
        result.verdict = "changes";
        result.reason = "the changed battle fails at a later tick: " + run.error;
    } else if (result.first_tick) {
        result.verdict = "changes";
        result.reason = "the battle differs from completed tick " + std::to_string(*result.first_tick);
    } else {
        result.verdict = "no change";
        const bool read = std::any_of(result.objects.begin(), result.objects.end(),
            [](const auto& object) { return object.read || !object.traced; });
        const bool unauthored = std::any_of(result.objects.begin(), result.objects.end(),
            [](const auto& object) { return object.from.empty(); });
        if (unauthored) {
            result.reason = "not authored; read unknown: no baseline node for one or more changed objects; "
                "the battle never differs in " + std::to_string(result.ticks_run) + " ticks";
        } else if (!read) {
            result.reason = "not read: no loader of the headless battle reads it for these objects";
        } else if (!result.tables_changed) {
            result.reason = std::all_of(result.objects.begin(), result.objects.end(), [](const auto& object) { return object.traced; })
                ? "read and dropped: the full unit tables are identical with the change; other scenario consumers remain unproven"
                : "the unit tables are identical with the change and the battle never differs (its file is read whole: per-tag reads unknown)";
        } else {
            result.reason = "in the unit tables, the battle never differs in " + std::to_string(result.ticks_run)
                + " ticks: not applied, or not exercised by the scenario";
        }
    }
    if (compare && run.error.empty()) {
        auto again = run_battle(loaded, options.seed, options.check_workers, run.hashes.size(), false, nullptr, 0,
            [](std::uint64_t, const Frame&) {});
        if (!again.error.empty() || again.hashes != run.hashes) {
            result.diverged = true;
            std::size_t tick = 0;
            while (tick < again.hashes.size() && tick < run.hashes.size() && again.hashes[tick] == run.hashes[tick]) ++tick;
            result.workers_check = "diverged at " + std::to_string(options.check_workers) + " workers, tick "
                + std::to_string(tick + 1) + (again.error.empty() ? std::string() : ": " + again.error);
        } else {
            result.workers_check = "same at " + std::to_string(options.check_workers) + " workers";
        }
    }
}

} // namespace

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
        auto built = soak::build_battle(loaded, {.seed = options.seed, .anonymous_content = true});
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
