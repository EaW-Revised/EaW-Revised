// Path search cost in the M2 battle (#520, docs/simulation.md#path-search-cost): loads the FoC M2
// start with the FoC AI (needs the game data), gives the human player's move orders across the
// map, and prints the tick times after the order with the work and time of the path searches
// (tactical::PathSearchProbe). Exits 1 when two worker counts disagree on a tick's state hash.
//
//   path_bench [--game-root <dir>] [--selection owner|owner-group|all|large[,...]] [--order-tick <n>] [--ticks 300]
//              [--workers 1,4] [--destination x,y] [--timing on|off] [--csv <prefix>] [--list 1]
//              [--pin <file> [--update-pin 1]] [--replay-out <file>] [--ships <prefix>] [--search-budget <n>]
//
// "owner" (the default, the owner's benchmark) stages 20 capital ships of the four M2 types that
// move at tick 0 and orders them at tick 600: 12 as one group move and 8 as single moves in the
// same tick, across the map through the centre's stations and pads. "owner-group" stages the same
// 20 ships and gives them one player order at tick 600, a group move of all 20 to the owner
// group's target (#613: the eye-check clip's order; the 8 single moves of "owner" are no player
// order, and their ships stand at rest in the group's way when its front ships plan). "all" orders every ship and
// squadron the human player has at tick 3000 (the station's launches included); "large" does the
// same with 9 staged ships more. --timing off leaves the probe's clock out of the tick times.
// --csv writes <prefix>-<selection>-w<workers>.csv (per tick) and ...-searches.csv (per search).
// --replay-out writes the first run's replay (setup, staged ships and every command, the AI's
// included) for `--eawr-live-session replay` in the viewer or `sim_headless --replay`.
// --search-budget <n> replaces the per-tick search budget of the avoidance rules (PC-07; a large
// value plans every search whole in its frame, as FoC does).
// --ships writes <prefix>-<selection>-w<workers>.csv: the staged ships after the order of an owner
// selection (every tick for 16 ticks, every 10 up to 300, then every 100): position, yaw and plan
// (start tick, target, nodes, end frame).
// --pin (the regression test path_bench_owner_work): the first worker count must be 1, whose
// searches come in planning order; every search's (and slice's) work from the order on must
// match <file> (--update-pin 1 rewrites it), and every tick must stay in its budgets
// (space-movement PC-07, PC-08): no search starts in a layer that has spent its budget, none
// passes it by more than one parent, and no slice passes the slice size by more than one parent
// (a sliced search that has not ended at its landing would). Without the game data it prints
// SKIPPED and exits 0.
//
// --melee s|m|l runs the close-range battle benchmark instead (#601, melee.cpp); --profile-attach
// <pid> samples another process, such as the viewer playing the melee (sampler.hpp).

#include "melee.hpp"

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

namespace {

namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace foc = eawr::script::foc;
using eawr::sim::math::Fixed;
using Clock = std::chrono::steady_clock;

// The cheapest clock the host has: the time stamp counter on x64, else the steady clock.
[[nodiscard]] std::uint64_t clock_ticks() noexcept {
#if (defined(_MSC_VER) && defined(_M_X64)) || defined(__x86_64__)
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
#endif
}

struct Options {
    std::optional<std::string> game_root;
    std::optional<std::uint64_t> order_tick; // default: 600 for owner, 3000 otherwise
    std::uint64_t ticks = 300;
    std::vector<std::size_t> workers{1, 4};
    std::vector<std::string> selections{"owner"};
    bool timing = true;
    std::optional<std::string> csv;
    std::optional<std::pair<std::int64_t, std::int64_t>> destination; // whole units; default: the mirror of the ships' centre
    bool list = false;
    std::optional<std::string> pin;
    bool update_pin = false;
    std::optional<std::string> replay_out;
    std::optional<std::string> ships; // --ships: the owner fleet's positions and plans after the order
    std::optional<std::uint64_t> search_budget; // --search-budget: overrides the avoidance rules' (PC-07)
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
[[nodiscard]] std::optional<T> parse_number(const std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<Options> parse_options(const int argc, const char* const argv[]) {
    Options options;
    options.game_root = environment("EAWR_EAW_GAME_ROOT");
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 >= argc) return std::nullopt;
        const std::string_view value(argv[++index]);
        if (argument == "--game-root") {
            options.game_root = std::string(value);
        } else if (argument == "--order-tick") {
            const auto tick = parse_number<std::uint64_t>(value);
            if (!tick || *tick < 1 || *tick > 50'000) return std::nullopt;
            options.order_tick = *tick;
        } else if (argument == "--ticks") {
            const auto ticks = parse_number<std::uint64_t>(value);
            if (!ticks || *ticks < 1 || *ticks > 10'000) return std::nullopt;
            options.ticks = *ticks;
        } else if (argument == "--workers") {
            options.workers.clear();
            std::string_view rest = value;
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                const auto count = parse_number<std::size_t>(rest.substr(0, comma));
                if (!count || *count == 0 || *count > eawr::platform::ThreadWorkerAdapter::max_worker_count) {
                    return std::nullopt;
                }
                options.workers.push_back(*count);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
            if (options.workers.empty()) return std::nullopt;
        } else if (argument == "--selection") {
            options.selections.clear();
            std::string_view rest = value;
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                const auto item = rest.substr(0, comma);
                if (item != "owner" && item != "owner-group" && item != "all" && item != "large") return std::nullopt;
                options.selections.emplace_back(item);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
            if (options.selections.empty()) return std::nullopt;
        } else if (argument == "--timing") {
            if (value != "on" && value != "off") return std::nullopt;
            options.timing = value == "on";
        } else if (argument == "--csv") {
            options.csv = std::string(value);
        } else if (argument == "--destination") {
            const auto comma = value.find(',');
            if (comma == std::string_view::npos) return std::nullopt;
            const auto x = parse_number<std::int64_t>(value.substr(0, comma));
            const auto y = parse_number<std::int64_t>(value.substr(comma + 1));
            if (!x || !y || *x < -100'000 || *x > 100'000 || *y < -100'000 || *y > 100'000) return std::nullopt;
            options.destination = std::pair{*x, *y};
        } else if (argument == "--list") {
            options.list = value == "1";
        } else if (argument == "--pin") {
            options.pin = std::string(value);
        } else if (argument == "--update-pin") {
            options.update_pin = value == "1";
        } else if (argument == "--replay-out") {
            options.replay_out = std::string(value);
        } else if (argument == "--ships") {
            options.ships = std::string(value);
        } else if (argument == "--search-budget") {
            const auto budget = parse_number<std::uint64_t>(value);
            if (!budget || *budget < 1) return std::nullopt;
            options.search_budget = *budget;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

struct Content {
    skirmish::SkirmishStart start;
    skirmish::SessionContent content;
    tactical::VictoryRules victory;
    foc::AiSetup ai;
    std::map<std::string, std::string> modules;
    std::map<tactical::TypeId, Fixed> heights; // Layer_Z_Adjust per unit-table type (SK-05)
};

[[nodiscard]] std::optional<Content> load(const std::filesystem::path& root) {
    const auto fail = [](const std::string& what) {
        std::cerr << "path_bench: " << what << '\n';
        return std::optional<Content>{};
    };
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, root / folder / "Data");
        if (!manifest) return fail("the " + id + " layer does not mount");
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) return fail("the FoC vfs does not mount");
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    if (!catalog) return fail("the FoC catalog does not load");
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    auto tables = eawr::units::load_unit_tables(input);
    if (!tables) return fail("the FoC unit tables do not load");
    const auto& fixture = skirmish::m2_fixture();
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    if (!inputs) return fail("the M2 start inputs do not read");
    auto start = skirmish::build_start(fixture, inputs.value());
    if (!start) return fail("the M2 start does not build");
    auto content = skirmish::session_content(tables.value());
    if (!content) return fail("the session content does not build");
    Content out{start.value(), std::move(content).value(), skirmish::victory_rules(start.value(), tables.value()),
        skirmish::ai_setup(start.value(), inputs.value(), tables.value()), {}, {}};
    if (!skirmish::enable_goal_system(filesystem.value(), out.ai)) return fail("the goal system does not load");
    auto modules = skirmish::ai_modules(filesystem.value(), out.ai);
    if (!modules) return fail("the AI's Lua files do not load");
    out.modules = std::move(modules).value();
    for (const auto& type : tables.value().units) {
        out.heights.emplace(skirmish::type_id(type.id), type.movement.layer_z_adjust.value_or(Fixed{}));
    }
    return out;
}

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

// Forwards to the pool and times the plan-searches phase.
class TimingExecutor final : public eawr::sim::PartitionExecutor {
public:
    explicit TimingExecutor(const eawr::sim::PartitionExecutor& inner) : inner_(inner) {}
    [[nodiscard]] std::size_t worker_count() const noexcept override { return inner_.worker_count(); }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t partition_count, const std::function<void(std::size_t)>& partition) const override {
        return execute_phase("unnamed", partition_count, partition);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override {
        const auto start = Clock::now();
        auto result = inner_.execute_phase(phase, partition_count, partition);
        if (phase == "plan-searches") plan += Clock::now() - start;
        return result;
    }
    mutable Clock::duration plan{};

private:
    const eawr::sim::PartitionExecutor& inner_;
};

struct TickRecord {
    std::uint64_t tick{};
    double ms{};
    double plan_ms{};
    std::vector<tactical::PathSearchStats> calls;
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

// The ships staged for the large selection. The M2 tables give motion to four types only:
// the Corellian corvette and the Tartan cruiser (corvette layer), the Nebulon-B and the
// Acclamator (frigate layer).
constexpr std::pair<std::string_view, int> large_fleet[] = {{"Corellian_Corvette", 4}, {"Nebulon_B_Frigate", 5}};

// The owner's benchmark: 20 capital ships in a block of five columns by four rows, 400 apart,
// west of the rebel star base; the first 12 of them (both layers) as one group move and the
// other 8 as single moves, all in the order tick.
constexpr std::pair<std::string_view, int> owner_fleet[] = {
    {"Corellian_Corvette", 6}, {"Tartan_Patrol_Cruiser", 4}, {"Nebulon_B_Frigate", 6}, {"Acclamator_Assault_Ship", 4}};
constexpr std::size_t owner_group[] = {0, 1, 2, 6, 7, 10, 11, 12, 16, 17, 3, 13};
constexpr std::size_t owner_singles[] = {4, 5, 8, 9, 14, 15, 18, 19};
constexpr std::pair<std::int64_t, std::int64_t> owner_group_target{2500, -3000};
constexpr std::pair<std::int64_t, std::int64_t> owner_single_targets[] = {
    {-3000, -4500}, {-1500, -4800}, {0, -4500}, {1500, -4000}, {3000, -2500}, {4500, -1000}, {-4200, -3000}, {1000, -5200}};

[[nodiscard]] tactical::PlayerId human_player(const Content& content) {
    for (const auto& player : content.start.players) {
        if (player.human) return player.player.player_id;
    }
    return 1;
}

[[nodiscard]] Fixed whole(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

[[nodiscard]] eawr::sim::math::Vec3 point(const std::pair<std::int64_t, std::int64_t>& xy) {
    return {whole(xy.first), whole(xy.second), Fixed{}};
}

// The selections that stage the owner's 20 ships.
[[nodiscard]] bool owner_selection(const std::string& selection) {
    return selection == "owner" || selection == "owner-group";
}

[[nodiscard]] std::uint64_t order_tick_of(const Options& options, const std::string& selection) {
    return options.order_tick.value_or(owner_selection(selection) ? 600 : 3000);
}

// --ships: one row per staged ship of the owner selection at completed tick `tick`: where it is
// and what its plan is (the plan's start tick, its target, its node count and end frame).
void append_ship_rows(std::string& rows, const tactical::TacticalSession& world,
    const std::vector<eawr::sim::EntityId>& fleet, const bool all_grouped, const std::uint64_t tick) {
    if (rows.empty()) rows = "tick,index,entity,role,x,y,yaw,plan,plan_tick,target_x,target_y,nodes,end_frame\n";
    const auto units = world.units();
    const auto whole_of = [](const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(Fixed::scale); };
    std::ostringstream line;
    line << std::fixed << std::setprecision(1);
    for (std::size_t index = 0; index < fleet.size(); ++index) {
        const auto found = std::find_if(units.begin(), units.end(),
            [&](const tactical::UnitState& unit) { return unit.entity_id == fleet[index]; });
        if (found == units.end()) continue;
        const bool grouped
            = all_grouped || std::find(std::begin(owner_group), std::end(owner_group), index) != std::end(owner_group);
        const auto yaw = tactical::yaw_degrees(found->rotation);
        line << tick << ',' << index << ',' << fleet[index] << ',' << (grouped ? "group" : "single") << ','
             << whole_of(found->position.x) << ',' << whole_of(found->position.y) << ','
             << (yaw ? whole_of(yaw.value()) : 0.0) << ',';
        const auto state = world.motion_state(fleet[index]);
        if (state) {
            line << tactical::to_string(state->kind) << ',' << state->start_tick << ',' << whole_of(state->target.x) << ','
                 << whole_of(state->target.y) << ',' << state->nodes.size() << ','
                 << (state->nodes.empty() ? 0.0 : whole_of(state->nodes.back().frame)) << '\n';
        } else {
            line << "none,0,0,0,0,0\n";
        }
    }
    rows += line.str();
}

[[nodiscard]] std::optional<Run> run(const Content& content, const std::string& selection, const std::size_t workers,
    const Options& options, Probe& probe) {
    auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors,
        content.content.durability, content.content.motion, std::nullopt, content.content.combat, content.victory);
    if (!world) {
        std::cerr << "path_bench: " << world.error().message << '\n';
        return std::nullopt;
    }
    const auto human = human_player(content);
    const auto& motion = content.content.motion;
    // A staged ship stands on the plane of the ship it is staged beside, at its own type's height
    // (space-movement LZ-01).
    const auto height = [&](const tactical::TypeId type) {
        const auto found = content.heights.find(type);
        return found == content.heights.end() ? Fixed{} : found->second;
    };
    const auto staged_z = [&](const tactical::UnitState& origin, const tactical::TypeId type) {
        return Fixed::from_raw(origin.position.z.raw() - height(origin.type_id).raw() + height(type).raw());
    };
    std::vector<eawr::sim::EntityId> fleet; // the owner selection's staged ships, in staging order
    if (owner_selection(selection)) {
        tactical::UnitState origin;
        for (const auto& unit : world.value().units()) {
            if (unit.owner == human && motion.footprint(unit.type_id) != nullptr && motion.find(unit.type_id) != nullptr) {
                origin = unit;
                break;
            }
        }
        for (const auto& [name, count] : owner_fleet) {
            const auto type = skirmish::type_id(name);
            if (motion.find(type) == nullptr) {
                std::cerr << "path_bench: " << name << " has no motion profile\n";
                return std::nullopt;
            }
            for (int copy = 0; copy < count; ++copy) {
                const auto index = static_cast<std::int64_t>(fleet.size());
                tactical::UnitState unit;
                unit.type_id = type;
                unit.owner = human;
                unit.rotation = origin.rotation;
                unit.position = {whole(-5600 + 400 * (index % 5)), whole(4100 - 400 * (index / 5)), staged_z(origin, type)};
                auto staged = world.value().stage_spawn(unit);
                if (!staged) {
                    std::cerr << "path_bench: staging " << name << ": " << staged.error().message << '\n';
                    return std::nullopt;
                }
                fleet.push_back(staged.value());
            }
        }
    }
    if (selection == "large") {
        // In rows of four behind the human player's ships, 400 apart, facing like the first one.
        std::vector<tactical::UnitState> ships;
        for (const auto& unit : world.value().units()) {
            if (unit.owner == human && motion.find(unit.type_id) != nullptr && motion.footprint(unit.type_id) != nullptr) {
                ships.push_back(unit);
            }
        }
        if (ships.empty()) {
            std::cerr << "path_bench: the human player has no ship to stage beside\n";
            return std::nullopt;
        }
        const auto origin = ships.front();
        int placed = 0;
        for (const auto& [name, count] : large_fleet) {
            const auto type = skirmish::type_id(name);
            if (motion.find(type) == nullptr) {
                std::cerr << "path_bench: " << name << " has no motion profile; not staged\n";
                continue;
            }
            for (int copy = 0; copy < count; ++copy, ++placed) {
                tactical::UnitState unit;
                unit.type_id = type;
                unit.owner = human;
                unit.rotation = origin.rotation;
                unit.position = {Fixed::from_raw(origin.position.x.raw() + whole(400 * (placed % 4 - 1)).raw()),
                    Fixed::from_raw(origin.position.y.raw() + whole(400 * (placed / 4 + 1)).raw()), staged_z(origin, type)};
                if (auto staged = world.value().stage_spawn(unit); !staged) {
                    std::cerr << "path_bench: staging " << name << ": " << staged.error().message << '\n';
                    return std::nullopt;
                }
            }
        }
    }
    // Staged units are no replay commands (stage_spawn): the written replay puts them in its setup.
    std::vector<tactical::UnitState> staged_units;
    for (const auto& unit : world.value().units()) {
        const auto& setup = content.start.setup.units;
        if (std::none_of(setup.begin(), setup.end(), [&](const tactical::UnitState& known) { return known.entity_id == unit.entity_id; })) {
            staged_units.push_back(unit);
        }
    }
    auto session = foc::create_session(std::move(world).value(), content.ai, content.modules);
    if (!session) {
        std::cerr << "path_bench: " << session.error().message << '\n';
        return std::nullopt;
    }
    const eawr::platform::ThreadWorkerAdapter pool(workers);
    const TimingExecutor executor(pool);
    Run out;
    const auto clock_began = clock_ticks();
    const auto time_began = Clock::now();
    const auto order_tick = order_tick_of(options, selection);
    const auto end = order_tick + options.ticks;
    for (std::uint64_t tick = 0; tick < end; ++tick) {
        std::vector<tactical::PlayerCommand> input;
        if (tick == order_tick) {
            for (const auto& unit : session.value().world().units()) {
                const auto* footprint = motion.footprint(unit.type_id);
                if (footprint == nullptr) continue;
                if (const auto layer = tactical::dynamic_layer_index(footprint->layer)) out.layers[unit.entity_id] = *layer;
            }
        }
        if (tick == order_tick && selection == "owner") {
            const auto live = session.value().world().units();
            const auto alive = [&](const eawr::sim::EntityId id) {
                return std::any_of(live.begin(), live.end(), [&](const tactical::UnitState& unit) { return unit.entity_id == id; });
            };
            std::vector<eawr::sim::EntityId> group;
            for (const auto index : owner_group) {
                if (alive(fleet[index])) group.push_back(fleet[index]);
            }
            std::sort(group.begin(), group.end());
            std::uint64_t sequence = 1;
            input.push_back(tactical::PlayerCommand{{tick, human, sequence++}, group, tactical::MovePayload{point(owner_group_target)}});
            out.ships = group.size();
            for (std::size_t index = 0; index < std::size(owner_singles); ++index) {
                const auto id = fleet[owner_singles[index]];
                if (!alive(id)) continue;
                input.push_back(tactical::PlayerCommand{
                    {tick, human, sequence++}, {id}, tactical::MovePayload{point(owner_single_targets[index])}});
                ++out.ships;
            }
            std::cout << "  order at tick " << tick << ": a group move of " << group.size() << " ships and "
                      << input.size() - 1 << " single moves" << std::endl; // flushed: the order's moment
        } else if (tick == order_tick && selection == "owner-group") {
            const auto live = session.value().world().units();
            std::vector<eawr::sim::EntityId> group;
            for (const auto id : fleet) {
                if (std::any_of(live.begin(), live.end(), [&](const tactical::UnitState& unit) { return unit.entity_id == id; })) {
                    group.push_back(id);
                }
            }
            std::sort(group.begin(), group.end());
            input.push_back(tactical::PlayerCommand{{tick, human, 1}, group, tactical::MovePayload{point(owner_group_target)}});
            out.ships = group.size();
            std::cout << "  order at tick " << tick << ": a group move of " << group.size() << " ships" << std::endl;
        } else if (tick == order_tick) {
            // Every ship and squadron container of the human player, to the far side of the map.
            const auto& live = session.value().world();
            std::set<eawr::sim::EntityId> craft;
            std::vector<eawr::sim::EntityId> units;
            for (const auto& squadron : live.squadrons()) {
                craft.insert(squadron.members.begin(), squadron.members.end());
            }
            Fixed sum_x{};
            Fixed sum_y{};
            for (const auto& unit : live.units()) {
                if (unit.owner != human || craft.contains(unit.entity_id)) continue;
                const bool squadron = std::any_of(live.squadrons().begin(), live.squadrons().end(),
                    [&](const tactical::Squadron& entry) { return entry.container == unit.entity_id; });
                if (!squadron && (motion.find(unit.type_id) == nullptr || motion.footprint(unit.type_id) == nullptr)) continue;
                units.push_back(unit.entity_id);
                if (squadron) {
                    ++out.squadrons;
                } else {
                    ++out.ships;
                    sum_x = Fixed::from_raw(sum_x.raw() + unit.position.x.raw());
                    sum_y = Fixed::from_raw(sum_y.raw() + unit.position.y.raw());
                }
            }
            const auto ships = static_cast<std::int64_t>(std::max<std::size_t>(1, out.ships));
            eawr::sim::math::Vec3 destination{Fixed::from_raw(sum_x.raw() / ships), Fixed::from_raw(-sum_y.raw() / ships), Fixed{}};
            if (options.destination) destination = {whole(options.destination->first), whole(options.destination->second), Fixed{}};
            if (options.list) {
                for (const auto& unit : live.units()) {
                    const auto* footprint = motion.footprint(unit.type_id);
                    if (footprint == nullptr || footprint->layer == tactical::SpaceLayer::none || craft.contains(unit.entity_id)) continue;
                    std::cout << "    unit " << unit.entity_id << " owner " << unit.owner << " layer "
                              << static_cast<int>(footprint->layer) << (footprint->obstacle ? " obstacle" : "") << " radius "
                              << footprint->radius.raw() / Fixed::scale << " at (" << unit.position.x.raw() / Fixed::scale
                              << ", " << unit.position.y.raw() / Fixed::scale << ")\n";
                }
            }
            std::cout << "  order at tick " << tick << ": " << out.ships << " ships and " << out.squadrons
                      << " squadrons to (" << destination.x.raw() / Fixed::scale << ", " << destination.y.raw() / Fixed::scale
                      << ")\n";
            input.push_back(tactical::PlayerCommand{{tick, human, 1}, units, tactical::MovePayload{destination}});
        }
        static_cast<void>(probe.take());
        executor.plan = {};
        const auto began = Clock::now();
        auto stepped = session.value().step(executor, input);
        const auto elapsed = Clock::now() - began;
        if (!stepped) {
            std::cerr << "path_bench: step " << tick + 1 << ": " << stepped.error().message << '\n';
            return std::nullopt;
        }
        out.hashes.push_back(stepped.value().world.state_sha256);
        const auto since = tick - order_tick;
        if (options.ships && owner_selection(selection) && tick >= order_tick
            && (since < 16 || (since < 300 && since % 10 == 0) || since % 100 == 0)) {
            append_ship_rows(out.ships_csv, session.value().world(), fleet, selection == "owner-group", tick + 1);
        }
        const auto sliced = session.value().world().sliced_searches();
        out.most_sliced = std::max(out.most_sliced, sliced.searches);
        out.most_sliced_bytes = std::max(out.most_sliced_bytes, sliced.bytes);
        TickRecord record{tick, std::chrono::duration<double, std::milli>(elapsed).count(),
            std::chrono::duration<double, std::milli>(executor.plan).count(), probe.take()};
        (tick >= order_tick ? out.after : out.before).push_back(std::move(record));
    }
    if (options.replay_out) {
        auto replay = session.value().record();
        replay.setup.units.insert(replay.setup.units.end(), staged_units.begin(), staged_units.end());
        auto bytes = tactical::write_replay(replay);
        if (!bytes) {
            std::cerr << "path_bench: the replay does not write: " << bytes.error().message << '\n';
            return std::nullopt;
        }
        std::ofstream file(*options.replay_out, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
        if (!file) {
            std::cerr << "path_bench: cannot write " << *options.replay_out << '\n';
            return std::nullopt;
        }
    }
    const auto clock_spent = clock_ticks() - clock_began;
    const auto time_spent = std::chrono::duration<double, std::nano>(Clock::now() - time_began).count();
    out.ns_per_clock_tick = clock_spent == 0 ? 1.0 : time_spent / static_cast<double>(clock_spent);
    return out;
}

[[nodiscard]] double percentile(std::vector<double> values, const double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    // Nearest rank.
    auto rank = static_cast<std::size_t>(fraction * static_cast<double>(values.size()) + 0.999999);
    rank = std::clamp<std::size_t>(rank, 1, values.size());
    return values[rank - 1];
}

void report(const Run& run, const std::string& selection, const std::size_t workers) {
    std::vector<double> after;
    std::vector<double> before;
    for (const auto& record : run.after) after.push_back(record.ms);
    for (const auto& record : run.before) before.push_back(record.ms);
    std::uint64_t searches = 0;
    std::uint64_t slots = 0;
    tactical::PathSearchStats sum;
    std::uint64_t max_expansions = 0;
    std::uint64_t longest = 0;
    std::uint64_t slot_ticks = 0;
    std::uint64_t max_searches_in_tick = 0;
    std::uint64_t abandoned = 0;
    std::uint64_t sliced = 0;
    std::uint64_t slices = 0;
    for (const auto& record : run.after) {
        std::uint64_t in_tick = 0;
        for (const auto& call : record.calls) {
            if (call.slot) {
                ++slots;
                slot_ticks += call.total_ticks;
                continue;
            }
            abandoned += call.part == tactical::PathSearchPart::abandoned ? 1 : 0;
            sliced += call.part == tactical::PathSearchPart::last_slice ? 1 : 0;
            slices += call.part == tactical::PathSearchPart::slice || call.part == tactical::PathSearchPart::last_slice ? 1 : 0;
            ++searches;
            ++in_tick;
            sum.tries += call.tries;
            sum.expansions += call.expansions;
            sum.children += call.children;
            sum.queries += call.queries;
            sum.windows += call.windows;
            sum.leaves += call.leaves;
            sum.narrow += call.narrow;
            sum.total_ticks += call.total_ticks;
            sum.query_ticks += call.query_ticks;
            sum.set_ticks += call.set_ticks;
            max_expansions = std::max(max_expansions, call.expansions);
            longest = std::max(longest, call.total_ticks);
        }
        max_searches_in_tick = std::max(max_searches_in_tick, in_tick);
    }
    const auto ns = [&](const std::uint64_t ticks) { return static_cast<double>(ticks) * run.ns_per_clock_tick; };
    const auto per = [](const double value, const std::uint64_t count) { return count == 0 ? 0.0 : value / static_cast<double>(count); };
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  " << selection << ", " << workers << " worker(s): " << run.after.size() << " ticks from the order: p50 "
              << percentile(after, 0.5) << " ms, p99 " << percentile(after, 0.99) << " ms, max " << percentile(after, 1.0)
              << " ms; order tick " << (run.after.empty() ? 0.0 : run.after.front().ms) << " ms (plan-searches phase "
              << (run.after.empty() ? 0.0 : run.after.front().plan_ms) << " ms)\n";
    std::cout << "    before the order: p50 " << percentile(before, 0.5) << " ms, p99 " << percentile(before, 0.99) << " ms\n";
    std::cout << "    path searches " << searches << " (at most " << max_searches_in_tick << " in a tick), tries "
              << sum.tries << ", expansions mean " << per(static_cast<double>(sum.expansions), searches) << " max "
              << max_expansions << "; longest search " << ns(longest) / 1e6 << " ms; slot searches " << slots << " ("
              << ns(slot_ticks) / 1e6 << " ms)\n";
    std::cout << "    PC-08: " << abandoned << " searches given up at the budget, " << sliced << " sliced searches landed ("
              << slices << " slices); at most " << run.most_sliced << " in flight after a tick, holding "
              << static_cast<double>(run.most_sliced_bytes) / 1048576.0 << " MiB\n";
    const double total = ns(sum.total_ticks);
    const double query = ns(sum.query_ticks);
    const double set = ns(sum.set_ticks);
    std::cout << "    per expansion " << per(total, sum.expansions) << " ns = collision queries " << per(query, sum.expansions)
              << " + open/closed set " << per(set, sum.expansions) << " + rest " << per(total - query - set, sum.expansions)
              << "; per expansion " << per(static_cast<double>(sum.queries), sum.expansions) << " queries, "
              << per(static_cast<double>(sum.windows), sum.expansions) << " windows, "
              << per(static_cast<double>(sum.leaves), sum.expansions) << " leaves, "
              << per(static_cast<double>(sum.narrow), sum.expansions) << " exact tests\n";
}

void write_csv(const std::string& path, const Run& run) {
    std::ofstream out(path, std::ios::binary);
    out << "tick,ms,plan_ms,searches,slots,expansions,queries,leaves,narrow,search_ms,query_ms,set_ms\n";
    for (const auto* records : {&run.before, &run.after}) {
        for (const auto& record : *records) {
            std::uint64_t searches = 0;
            std::uint64_t slots = 0;
            tactical::PathSearchStats sum;
            for (const auto& call : record.calls) {
                ++(call.slot ? slots : searches);
                sum.expansions += call.expansions;
                sum.queries += call.queries;
                sum.leaves += call.leaves;
                sum.narrow += call.narrow;
                sum.total_ticks += call.total_ticks;
                sum.query_ticks += call.query_ticks;
                sum.set_ticks += call.set_ticks;
            }
            const auto ms = [&](const std::uint64_t ticks) { return static_cast<double>(ticks) * run.ns_per_clock_tick / 1e6; };
            out << record.tick << ',' << record.ms << ',' << record.plan_ms << ',' << searches << ',' << slots << ','
                << sum.expansions << ',' << sum.queries << ',' << sum.leaves << ',' << sum.narrow << ','
                << ms(sum.total_ticks) << ',' << ms(sum.query_ticks) << ',' << ms(sum.set_ticks) << '\n';
        }
    }
}

// Every path search from the order on: its tick, unit and work counts (in the order the searches
// finished, which with more than one worker is not the planning order).
void write_searches_csv(const std::string& path, const Run& run) {
    std::ofstream out(path, std::ios::binary);
    out << "tick,entity,tries,expansions,children,queries,leaves,narrow,search_ms\n";
    for (const auto& record : run.after) {
        for (const auto& call : record.calls) {
            if (call.slot) continue;
            out << record.tick << ',' << call.entity << ',' << call.tries << ',' << call.expansions << ',' << call.children
                << ',' << call.queries << ',' << call.leaves << ',' << call.narrow << ','
                << static_cast<double>(call.total_ticks) * run.ns_per_clock_tick / 1e6 << '\n';
        }
    }
}

// The pinned rows of a 1-worker run: every search from the order on, in planning order.
[[nodiscard]] std::vector<std::string> work_rows(const Run& run) {
    std::vector<std::string> rows;
    for (const auto& record : run.after) {
        for (const auto& call : record.calls) {
            if (call.slot) continue;
            const auto layer = run.layers.find(call.entity);
            constexpr std::string_view parts = "wasl"; // whole, abandoned, slice, last slice
            rows.push_back(std::to_string(record.tick) + ',' + std::to_string(call.entity) + ','
                + (layer != run.layers.end() ? std::to_string(layer->second) : std::string("-")) + ','
                + parts[static_cast<std::size_t>(call.part)] + ',' + std::to_string(call.tries) + ',' + std::to_string(call.expansions) + ',' + std::to_string(call.queries) + ','
                + std::to_string(call.leaves) + ',' + std::to_string(call.narrow));
        }
    }
    return rows;
}

// PC-07, PC-08 in a 1-worker run: a search starts only while its layer has spent less than
// the budget in its tick and stops within one parent (10 children) past it; a slice counts at
// most the slice size and one parent. Returns the violations.
[[nodiscard]] std::vector<std::string> budget_violations(const Run& run, const tactical::AvoidanceRules& rules) {
    constexpr std::uint64_t parent = 10;
    std::vector<std::string> violations;
    for (const auto& record : run.after) {
        std::map<std::size_t, std::uint64_t> spent;
        for (const auto& call : record.calls) {
            if (call.slot) continue;
            const auto where = "tick " + std::to_string(record.tick) + " unit " + std::to_string(call.entity);
            if (call.part == tactical::PathSearchPart::slice || call.part == tactical::PathSearchPart::last_slice) {
                if (call.expansions > rules.search_slice + parent) {
                    violations.push_back(where + ": a slice of " + std::to_string(call.expansions) + " expansions");
                }
                continue;
            }
            const auto layer = run.layers.find(call.entity);
            if (layer == run.layers.end()) continue;
            auto& used = spent[layer->second];
            if (used >= rules.search_budget) {
                violations.push_back(where + " searched after " + std::to_string(used) + " expansions");
            }
            used += call.expansions;
            if (used > rules.search_budget + parent) {
                violations.push_back(where + " took layer " + std::to_string(layer->second) + " to " + std::to_string(used)
                    + " expansions");
            }
        }
    }
    return violations;
}

} // namespace

int main(const int argc, const char* const argv[]) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--melee") return eawr::bench::melee_main(argc, argv);
        if (std::string_view(argv[index]) == "--profile-attach") return eawr::bench::attach_main(argc, argv);
    }
    const auto options = parse_options(argc, argv);
    if (!options) {
        std::cerr << "usage: path_bench [--game-root <dir>] [--order-tick <n>] [--ticks <n>] [--workers <n>[,<n>...]]\n"
                     "                  [--selection owner|owner-group|all|large[,...]] [--csv <prefix>]\n";
        return 2;
    }
    if (!options->game_root) {
        if (options->pin) {
            std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the path benchmark\n";
            return 0;
        }
        std::cerr << "path_bench: set EAWR_EAW_GAME_ROOT or pass --game-root\n";
        return 2;
    }
    if (options->pin && (options->workers.front() != 1 || options->selections.size() != 1)) {
        std::cerr << "path_bench: --pin needs one selection and 1 as the first worker count\n";
        return 2;
    }
    auto content = load(*options->game_root);
    if (!content) return 1;
    if (options->search_budget && content->content.motion.avoidance) {
        content->content.motion.avoidance->search_budget = *options->search_budget;
    }
    if (options->list) {
        for (const auto& unit : content->start.units) {
            const auto* footprint = content->content.motion.footprint(unit.state.type_id);
            std::cout << "start " << unit.state.entity_id << ' ' << unit.type << " role " << skirmish::to_string(unit.role)
                      << " owner " << unit.state.owner << " at (" << unit.state.position.x.raw() / Fixed::scale << ", "
                      << unit.state.position.y.raw() / Fixed::scale << ")"
                      << (footprint != nullptr ? " layer " + std::to_string(static_cast<int>(footprint->layer)) : std::string())
                      << '\n';
        }
        for (const auto name : {"Corellian_Corvette", "Corellian_Gunboat", "Nebulon_B_Frigate", "Alliance_Assault_Frigate",
                 "Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Broadside_Class_Cruiser", "Interdictor_Cruiser",
                 "Star_Destroyer", "Victory_Destroyer", "Calamari_Cruiser", "Marauder_Missile_Cruiser", "Home_One"}) {
            const auto type = skirmish::type_id(name);
            const auto* footprint = content->content.motion.footprint(type);
            std::cout << "type " << name << (content->content.motion.find(type) != nullptr ? " moves" : " no motion")
                      << (footprint != nullptr ? " layer " + std::to_string(static_cast<int>(footprint->layer)) : std::string())
                      << '\n';
        }
    }
    Probe probe(options->timing);
    tactical::set_path_search_probe(&probe);
    bool agree = true;
    for (const auto& selection : options->selections) {
        std::cout << "selection " << selection << ":\n";
        std::optional<std::vector<std::string>> reference;
        for (const auto workers : options->workers) {
            auto result = run(*content, selection, workers, *options, probe);
            if (!result) {
                tactical::set_path_search_probe(nullptr);
                return 1;
            }
            report(*result, selection, workers);
            std::string joined;
            for (const auto& hash : result->hashes) joined += hash;
            std::cout << "    hashes digest " << eawr::sim::sha256_hex(std::span(
                reinterpret_cast<const std::uint8_t*>(joined.data()), joined.size())) << '\n';
            if (options->csv) {
                const auto prefix = *options->csv + "-" + selection + "-w" + std::to_string(workers);
                write_csv(prefix + ".csv", *result);
                write_searches_csv(prefix + "-searches.csv", *result);
            }
            if (options->ships && !result->ships_csv.empty()) {
                std::ofstream ships(*options->ships + "-" + selection + "-w" + std::to_string(workers) + ".csv", std::ios::binary);
                ships << result->ships_csv;
            }
            if (options->pin && !reference) {
                const auto rows = work_rows(*result);
                for (const auto& violation : budget_violations(*result, *content->content.motion.avoidance)) {
                    std::cout << "    FAIL: " << violation << '\n';
                    agree = false;
                }
                if (options->update_pin) {
                    std::ofstream output(*options->pin, std::ios::binary);
                    output << "tick,entity,layer,part,tries,expansions,queries,leaves,narrow\n";
                    for (const auto& row : rows) output << row << '\n';
                    std::cout << "    wrote " << *options->pin << '\n';
                } else {
                    std::ifstream input(*options->pin, std::ios::binary);
                    std::string line;
                    std::getline(input, line);
                    std::vector<std::string> pinned;
                    while (std::getline(input, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        pinned.push_back(line);
                    }
                    if (pinned != rows) {
                        std::cout << "    FAIL: the searches' work differs from " << *options->pin << " (" << rows.size()
                                  << " searches, " << pinned.size() << " pinned)\n";
                        for (std::size_t index = 0; index < std::max(rows.size(), pinned.size()); ++index) {
                            const auto now = index < rows.size() ? rows[index] : std::string("-");
                            const auto was = index < pinned.size() ? pinned[index] : std::string("-");
                            if (now != was) std::cout << "      pinned " << was << ", now " << now << '\n';
                        }
                        agree = false;
                    }
                }
            }
            if (!reference) {
                reference = result->hashes;
            } else if (*reference != result->hashes) {
                std::cout << "    FAIL: the hashes differ from the first worker count\n";
                agree = false;
            }
        }
    }
    tactical::set_path_search_probe(nullptr);
    return agree ? 0 : 1;
}
