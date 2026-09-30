#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/pathfind.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// #520: the path search's work and the bounded search's paths (docs/behaviour/space-movement.md
// PC-06, docs/simulation.md#path-search-cost). Moves on Coruscant's static layer (the M2 start's
// stations, pads and satellites) with held fleets in the corvette and frigate layers, a dense
// obstacle field, orders onto the stations, and crowded layers:
// - every search's work (PathSearchStats counts) in the exact and the bounded search is pinned
//   in fixtures/path-cost.work.csv, so a change in how much work a search does fails here
//   (counts, never time); `--update` re-pins it;
// - the bounded search's paths stay within the pinned bounds of FoC's exact search (length,
//   arrival, largest distance from the exact path and obstacles passed on the other side).
//
//   tactical_path_cost_tests <fixtures directory> [--update] [--report] [--profile]
//
// --report prints every search against the exact one; --profile prints the time split of the
// bounded searches. Timings are printed, never checked.
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;
using math::Vec2;
using math::Vec3;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr tactical::TypeId corvette_type = 2011;
constexpr tactical::TypeId frigate_type = 2012;

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }
[[nodiscard]] Vec3 at(const std::int64_t x, const std::int64_t y) { return {units(x), units(y), Fixed{}}; }
[[nodiscard]] double real(const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(one); }

// The FoC limits and footprints of pathfind_tests.cpp (units::motion_table on the FoC data).
[[nodiscard]] tactical::MotionProfile corvette() {
    return {corvette_type, decimal("3.72"), decimal("0.06"), decimal("0.06"), decimal("1.5"), units(2)};
}
[[nodiscard]] tactical::MotionProfile frigate() {
    return {frigate_type, decimal("2.64"), decimal("0.048"), decimal("0.048"), decimal("0.6"), units(3)};
}

[[nodiscard]] tactical::MotionTable foc_table() {
    tactical::MotionTable table{{units(15), units(300)}, {corvette(), frigate()}, std::nullopt, {}, {}};
    table.avoidance = tactical::AvoidanceRules{units(24), decimal("0.2"), units(100), decimal("0.8"), units(15),
        decimal("0.66"), decimal("1.2"), decimal("0.25"), decimal("1.7"), decimal("0.5"), decimal("0.5"), 3500, 6, 90, 45,
        units(50)};
    table.footprints = {
        {corvette_type, tactical::SpaceLayer::corvette, decimal("17.755"), decimal("41.822"), decimal("41.822"), false},
        {frigate_type, tactical::SpaceLayer::frigate, decimal("23.48"), decimal("109.949"), decimal("109.949"), false},
    };
    return table;
}

[[nodiscard]] tactical::TrackedLeaf obstacle(const eawr::sim::EntityId id, const std::int64_t x, const std::int64_t y,
    const Fixed radius) {
    return {id, {units(x), units(y)}, {units(x), units(y)}, {units(1), Fixed{}}, radius, radius, radius,
        tactical::collision_static};
}

// The M2 start's static layer: the two star bases, the merchant dock, the gravity well station,
// seven defense satellite pads and six mining pads (whole-unit centres).
[[nodiscard]] std::vector<tactical::TrackedLeaf> coruscant_statics() {
    const Fixed pad = decimal("206.25");
    return {obstacle(1, -3810, 4460, units(300)), obstacle(7, 3685, -4170, units(250)), obstacle(12, 4791, 4732, units(414)),
        obstacle(13, -229, 52, units(500)), obstacle(14, 2327, 2529, units(100)), obstacle(15, 1880, 2058, units(100)),
        obstacle(16, -2541, -2011, units(100)), obstacle(17, -3021, -2467, units(100)), obstacle(18, 1938, -5047, pad),
        obstacle(19, -4345, -4063, pad), obstacle(20, -2107, 5112, pad), obstacle(21, -419, 694, pad),
        obstacle(22, 445, -237, pad), obstacle(23, -796, -428, pad), obstacle(31, 0, -932, units(100)),
        obstacle(32, 580, 683, units(100)), obstacle(33, -1359, 313, units(100))};
}

// Coruscant's static layer with a dense field of 40 obstacles (radius 60 to 140) west of the
// centre, from a fixed generator: an asteroid-field-like cluster to fly through.
[[nodiscard]] std::vector<tactical::TrackedLeaf> field_statics() {
    auto leaves = coruscant_statics();
    std::uint64_t state = 520;
    const auto next = [&state](const std::uint64_t bound) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::int64_t>((state >> 33U) % bound);
    };
    for (eawr::sim::EntityId id = 40; id < 80; ++id) {
        const auto x = -4000 + next(2400);
        const auto y = -1300 + next(2600);
        leaves.push_back(obstacle(id, x, y, units(60 + next(81))));
    }
    return leaves;
}

struct Ship {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t to_x{}; // where the ship's prediction takes it by the last window (== x, y: held)
    std::int64_t to_y{};
};

// A layer of ships (AV-03): a held ship is a static leaf in every window, facing +X; a moving
// one flies straight from its position to its end over the 45 windows.
[[nodiscard]] tactical::TrackingLayerView layer_of(const std::vector<Ship>& ships, const eawr::sim::EntityId first_id,
    const Fixed x_extent, const Fixed y_extent) {
    constexpr std::int64_t windows = 45;
    tactical::TrackingLayerView view;
    view.start_frame = 3000;
    view.windows.resize(windows);
    eawr::sim::EntityId id = first_id;
    for (const auto& ship : ships) {
        const bool still = ship.x == ship.to_x && ship.y == ship.to_y;
        const auto along = [](const std::int64_t k, const std::int64_t from, const std::int64_t to) {
            return Fixed::from_raw(units(from).raw() + (units(to).raw() - units(from).raw()) / windows * k);
        };
        for (std::int64_t window = 0; window < windows; ++window) {
            const Vec2 start{along(window, ship.x, ship.to_x), along(window, ship.y, ship.to_y)};
            const Vec2 end{along(window + 1, ship.x, ship.to_x), along(window + 1, ship.y, ship.to_y)};
            Vec2 facing{units(1), Fixed{}};
            if (!still) {
                const auto dx = Fixed::from_raw(end.x.raw() - start.x.raw());
                const auto dy = Fixed::from_raw(end.y.raw() - start.y.raw());
                const auto length = math::length(Vec2{dx, dy}).value();
                facing = {math::divide(dx, length).value(), math::divide(dy, length).value()};
            }
            view.windows[static_cast<std::size_t>(window)].push_back({id, start, end, facing, x_extent, y_extent,
                std::max(x_extent, y_extent), still ? tactical::collision_static : tactical::collision_moving});
        }
        ++id;
    }
    return view;
}

[[nodiscard]] std::vector<Ship> held(const std::vector<std::pair<std::int64_t, std::int64_t>>& points) {
    std::vector<Ship> ships;
    for (const auto& [x, y] : points) ships.push_back({x, y, x, y});
    return ships;
}

struct World {
    std::vector<tactical::TrackedLeaf> statics;
    tactical::TrackingLayerView corvettes;
    tactical::TrackingLayerView frigates;
    tactical::CollisionWorld collisions;
};

void bind(World& world) {
    world.collisions.interval = 90;
    world.collisions.layers[*tactical::dynamic_layer_index(tactical::SpaceLayer::corvette)] = &world.corvettes;
    world.collisions.layers[*tactical::dynamic_layer_index(tactical::SpaceLayer::frigate)] = &world.frigates;
    world.collisions.statics = &world.statics;
}

// The human fleet held near the rebel start and the empire ships at their base and mid-map.
void fleets(World& world) {
    world.corvettes = layer_of(held({{-4906, 4700}, {-4756, 4700}, {-5056, 4550}, {-4906, 4550}, {-4756, 4550},
                                  {1000, 999}, {4706, -4392}}),
        100, decimal("17.755"), decimal("41.822"));
    world.frigates = layer_of(held({{-5056, 4400}, {-4806, 4400}, {-4556, 4400}, {-5056, 4150}, {4706, -4392}}), 200,
        decimal("23.48"), decimal("109.949"));
}

// The owner's benchmark block (path_bench): ten ships of each layer, 400 apart, around the
// searching ship, and four ships of each layer crossing the map.
void crowded_fleets(World& world) {
    std::vector<Ship> corvettes;
    std::vector<Ship> frigates;
    for (std::int64_t index = 0; index < 20; ++index) {
        const auto x = -5600 + 400 * (index % 5);
        const auto y = 4100 - 400 * (index / 5);
        (index % 2 == 0 ? corvettes : frigates).push_back({x, y, x, y});
    }
    for (std::int64_t index = 0; index < 4; ++index) {
        corvettes.push_back({-4000 + 2000 * index, -4500, -3000 + 2000 * index, 4500});
        frigates.push_back({4500, -3000 + 2000 * index, -4500, -2000 + 2000 * index});
    }
    world.corvettes = layer_of(corvettes, 100, decimal("17.755"), decimal("41.822"));
    world.frigates = layer_of(frigates, 200, decimal("23.48"), decimal("109.949"));
}

struct Search {
    std::string name;
    const World* world{};
    bool frigate{};
    Vec3 start;
    Fixed yaw;
    Vec3 target;
};

struct Outcome {
    bool path{};
    tactical::MotionState state;
    tactical::PathSearchStats stats;
    double seconds{};
};

[[nodiscard]] Outcome run(const tactical::MotionTable& table, const Search& search, const tactical::PathSearchMode mode) {
    const auto type = search.frigate ? frigate_type : corvette_type;
    const auto limits = search.frigate ? frigate() : corvette();
    Outcome out;
    const auto began = std::chrono::steady_clock::now();
    auto planned = tactical::plan_space_move(table, limits, *table.footprint(type), search.world->collisions,
        search.frigate ? 999 : 998, 3000, search.start, search.yaw, Fixed{}, search.target, &out.stats, mode);
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    expect(static_cast<bool>(planned), search.name + " plans without an arithmetic failure");
    if (planned) {
        out.path = planned.value().kind == tactical::MotionKind::path;
        out.state = std::move(planned).value();
    }
    return out;
}

// PC-08 (#520): the bounded search run in slices of any size, or within a budget it does not
// reach, is plan_space_move's search: the same plan and the same work.
void check_slices(const tactical::MotionTable& table, const Search& search, const Outcome& bounded) {
    const auto type = search.frigate ? frigate_type : corvette_type;
    const auto limits = search.frigate ? frigate() : corvette();
    const auto entity = search.frigate ? 999 : 998;
    for (const std::uint64_t slice : {std::uint64_t{1}, std::uint64_t{97}, std::uint64_t{1500}}) {
        tactical::SlicedPathSearch sliced(table, limits, *table.footprint(type), search.world->collisions, entity, 3000,
            search.start, search.yaw, Fixed{}, search.target);
        std::uint64_t expansions = 0;
        std::uint64_t slices = 0;
        for (bool ended = false; !ended && slices < 100000; ++slices) {
            tactical::PathSearchStats stats;
            ended = sliced.run(slice, &stats);
            expansions += stats.expansions;
            expect(stats.part == (ended ? tactical::PathSearchPart::last_slice : tactical::PathSearchPart::slice),
                search.name + ": a slice reports itself");
            // A slice stops before the parent that would pass its count, so it overshoots by at
            // most one parent's children (at most 10 operations).
            expect(stats.expansions <= slice + 10, search.name + ": a slice counts at most its expansions and one parent");
        }
        const auto result = sliced.result();
        expect(result && result.value() == bounded.state,
            search.name + ": the search in slices of " + std::to_string(slice) + " is the whole search");
        expect(sliced.expansions() == bounded.stats.expansions && expansions == bounded.stats.expansions,
            search.name + ": the slices count the whole search's expansions");
        expect(sliced.held_bytes() > 0, search.name + ": a sliced search reports what it holds");
    }
    const auto within = [&](const std::uint64_t budget, tactical::PathSearchStats& stats) {
        return tactical::plan_space_move_within(table, limits, *table.footprint(type), search.world->collisions, entity, 3000,
            search.start, search.yaw, Fixed{}, search.target, budget, &stats);
    };
    tactical::PathSearchStats stats;
    const auto enough = within(bounded.stats.expansions + 1, stats);
    expect(enough && enough.value() && *enough.value() == bounded.state && stats.part == tactical::PathSearchPart::whole,
        search.name + ": a budget past the search's expansions gives the whole search");
    if (bounded.path && bounded.stats.expansions >= 2) {
        const auto short_of = within(bounded.stats.expansions / 2, stats);
        expect(short_of && !short_of.value() && stats.part == tactical::PathSearchPart::abandoned,
            search.name + ": a budget short of the search gives it up");
    }
}

// The path's positions every 10 ticks from its start to its end.
[[nodiscard]] std::vector<Vec2> track(const Search& search, const tactical::MotionState& state, std::uint64_t& arrival) {
    std::vector<Vec2> points;
    arrival = 3000;
    for (std::uint64_t tick = 3000;; tick += 10) {
        const auto sample = tactical::sample_motion(state, tick, search.start, search.yaw);
        if (!sample) break;
        points.push_back({sample.value().position.x, sample.value().position.y});
        if (sample.value().finished || tick > 3000 + 60000) {
            arrival = tick;
            break;
        }
    }
    return points;
}

[[nodiscard]] double path_length(const std::vector<Vec2>& points) {
    double total = 0;
    for (std::size_t index = 1; index < points.size(); ++index) {
        total += std::hypot(real(points[index].x) - real(points[index - 1].x), real(points[index].y) - real(points[index - 1].y));
    }
    return total;
}

// The largest distance from a point of `points` to the polyline `line`.
[[nodiscard]] double largest_offset(const std::vector<Vec2>& points, const std::vector<Vec2>& line) {
    double largest = 0;
    if (line.size() < 2) return largest;
    for (const auto& point : points) {
        double nearest = 1e30;
        const double px = real(point.x);
        const double py = real(point.y);
        for (std::size_t index = 0; index + 1 < line.size(); ++index) {
            const double ax = real(line[index].x);
            const double ay = real(line[index].y);
            const double dx = real(line[index + 1].x) - ax;
            const double dy = real(line[index + 1].y) - ay;
            const double squared = dx * dx + dy * dy;
            const double t = squared == 0 ? 0 : std::clamp(((px - ax) * dx + (py - ay) * dy) / squared, 0.0, 1.0);
            nearest = std::min(nearest, std::hypot(px - (ax + t * dx), py - (ay + t * dy)));
        }
        largest = std::max(largest, nearest);
    }
    return largest;
}

// Whether the two routes (same start, same end) pass the point on different sides: the loop of
// `first` and then `second` backwards winds around it.
[[nodiscard]] bool between(const std::vector<Vec2>& first, const std::vector<Vec2>& second, const Vec2 point) {
    std::vector<Vec2> loop = first;
    loop.insert(loop.end(), second.rbegin(), second.rend());
    const double px = real(point.x);
    const double py = real(point.y);
    int winding = 0;
    for (std::size_t index = 0; index < loop.size(); ++index) {
        const Vec2& a = loop[index];
        const Vec2& b = loop[(index + 1) % loop.size()];
        const double ay = real(a.y) - py;
        const double by = real(b.y) - py;
        const double cross = (real(a.x) - px) * by - (real(b.x) - px) * ay;
        if (ay <= 0 && by > 0 && cross > 0) ++winding;
        if (ay > 0 && by <= 0 && cross < 0) --winding;
    }
    return winding != 0;
}

// The obstacles and held ships a search of `layer` avoids: centres of the static leaves and of the
// first window's static leaves of the layer.
[[nodiscard]] std::vector<Vec2> avoided(const World& world, const bool frigate) {
    std::vector<Vec2> points;
    for (const auto& leaf : world.statics) points.push_back(leaf.start);
    const auto& layer = frigate ? world.frigates : world.corvettes;
    for (const auto& leaf : layer.windows.front()) {
        if (leaf.collision == tactical::collision_static) points.push_back(leaf.start);
    }
    return points;
}

// One fixture row: a search's work counts in one mode.
[[nodiscard]] std::string work_row(const std::string& name, const std::string_view mode, const tactical::PathSearchStats& stats) {
    return '"' + name + "\"," + std::string(mode) + ',' + std::to_string(stats.tries) + ',' + std::to_string(stats.expansions)
        + ',' + std::to_string(stats.children) + ',' + std::to_string(stats.queries) + ',' + std::to_string(stats.windows)
        + ',' + std::to_string(stats.leaves) + ',' + std::to_string(stats.narrow);
}

// The bounded search's pinned bounds against the exact search over these searches (PC-06).
constexpr std::size_t least_identical = 70;
constexpr std::size_t most_other_side = 2;
constexpr double worst_length_bound = 1.02;
constexpr double worst_arrival_bound = 1.035;
constexpr double largest_offset_bound = 1200.0;
constexpr std::uint64_t bounded_expansion_bound = 5000;

// --profile: the time split of each search (steady clock through PathSearchProbe).
class Probe final : public tactical::PathSearchProbe {
public:
    [[nodiscard]] std::uint64_t now() noexcept override {
        return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    void searched(const tactical::PathSearchStats& stats) noexcept override { last = stats; }
    tactical::PathSearchStats last;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: tactical_path_cost_tests <fixtures directory> [--update] [--report] [--profile]\n";
        return 2;
    }
    const std::filesystem::path golden = std::filesystem::path(argv[1]) / "path-cost.work.csv";
    bool update = false;
    bool report = false;
    bool profile = false;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        update = update || argument == "--update";
        report = report || argument == "--report";
        profile = profile || argument == "--profile";
    }
    const auto table = foc_table();
    World open;
    open.statics = coruscant_statics();
    fleets(open);
    bind(open);
    World field;
    field.statics = field_statics();
    fleets(field);
    bind(field);
    World crowded;
    crowded.statics = coruscant_statics();
    crowded_fleets(crowded);
    bind(crowded);

    std::vector<Search> searches;
    const auto add = [&](const std::string& name, const World& world, const bool frigate, const Vec3 start,
                         const std::int64_t yaw, const Vec3 target) {
        searches.push_back({name, &world, frigate, start, units(yaw), target});
    };
    const auto point = [](const std::int64_t x, const std::int64_t y) {
        return "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
    };
    for (const bool frigate : {false, true}) {
        const std::string kind = frigate ? "frigate" : "corvette";
        // Cross-map orders from three sides of the map to a 3 x 3 grid of targets.
        const std::vector<std::pair<Vec3, std::int64_t>> starts{
            {frigate ? at(-5056, 3900) : at(-5056, 4700), -90}, {at(4600, -3900), 90}, {at(-5200, 200), 0}};
        for (std::size_t start = 0; start < starts.size(); ++start) {
            const auto& [from, yaw] = starts[start];
            for (const std::int64_t x : {-4500, 0, 4500}) {
                for (const std::int64_t y : {-4500, 0, 4500}) {
                    if (std::abs(real(from.x) - static_cast<double>(x)) + std::abs(real(from.y) - static_cast<double>(y)) < 3000) {
                        continue;
                    }
                    add(kind + " from " + std::to_string(start) + " to " + point(x, y), open, frigate, from, yaw, at(x, y));
                }
            }
        }
        // Onto the stations and a pad (AV-19 moves the target out of them), and far-side targets with
        // an obstacle on the approach.
        for (const auto& [x, y] : std::vector<std::pair<std::int64_t, std::int64_t>>{
                 {-229, 52}, {3685, -4170}, {445, -237}, {-4227, -4861}, {2000, -2000}, {3000, -4000}}) {
            add(kind + " station or far side " + point(x, y), open, frigate, frigate ? at(-5056, 3900) : at(-5056, 4700), -90,
                at(x, y));
        }
        // Across the dense field.
        for (const auto& [x, y] : std::vector<std::pair<std::int64_t, std::int64_t>>{
                 {-1000, 0}, {-1200, 1000}, {-1200, -1000}, {3000, 200}, {-4500, -3000}}) {
            add(kind + " field to " + point(x, y), field, frigate, at(-5300, 100), 0, at(x, y));
        }
        // Out of the crowded block, across ships crossing the map.
        for (const auto& [x, y] : std::vector<std::pair<std::int64_t, std::int64_t>>{
                 {2500, -3000}, {-3000, -4500}, {0, -4500}, {4500, -1000}, {-4200, -3000}, {1000, -5200}}) {
            add(kind + " crowded to " + point(x, y), crowded, frigate, frigate ? at(-5200, 3700) : at(-5200, 3300), -90,
                at(x, y));
        }
    }

    Probe probe;
    struct Totals {
        std::uint64_t exact_expansions{};
        std::uint64_t bounded_expansions{};
        std::uint64_t exact_max{};
        std::uint64_t bounded_max{};
        double exact_seconds{};
        double bounded_seconds{};
        double worst_length{};
        double worst_arrival{};
        double worst_offset{};
        std::size_t identical{};
        std::size_t other_side{};
    } totals;
    std::vector<std::string> rows;
    for (const auto& search : searches) {
        const auto exact = run(table, search, tactical::PathSearchMode::exact);
        if (profile) tactical::set_path_search_probe(&probe);
        const auto bounded = run(table, search, tactical::PathSearchMode::bounded);
        tactical::set_path_search_probe(nullptr);
        check_slices(table, search, bounded);
        rows.push_back(work_row(search.name, "exact", exact.stats));
        rows.push_back(work_row(search.name, "bounded", bounded.stats));
        expect(bounded.path == exact.path, search.name + ": the bounded search finds a path where the exact one does");
        totals.exact_expansions += exact.stats.expansions;
        totals.bounded_expansions += bounded.stats.expansions;
        totals.exact_max = std::max(totals.exact_max, exact.stats.expansions);
        totals.bounded_max = std::max(totals.bounded_max, bounded.stats.expansions);
        totals.exact_seconds += exact.seconds;
        totals.bounded_seconds += bounded.seconds;
        if (!exact.path || !bounded.path) {
            if (report) std::cout << search.name << ": no path (exact " << exact.path << ", bounded " << bounded.path << ")\n";
            continue;
        }
        std::uint64_t exact_arrival = 0;
        std::uint64_t bounded_arrival = 0;
        const auto exact_track = track(search, exact.state, exact_arrival);
        const auto bounded_track = track(search, bounded.state, bounded_arrival);
        const double exact_length = path_length(exact_track);
        const double length = exact_length == 0 ? 1.0 : path_length(bounded_track) / exact_length;
        const double arrival = static_cast<double>(bounded_arrival - 3000)
            / static_cast<double>(std::max<std::uint64_t>(1, exact_arrival - 3000));
        const double offset = std::max(largest_offset(bounded_track, exact_track), largest_offset(exact_track, bounded_track));
        const bool same = bounded.state == exact.state;
        totals.identical += same ? 1 : 0;
        std::size_t sides = 0;
        for (const auto& obstacle_point : avoided(*search.world, search.frigate)) {
            sides += between(exact_track, bounded_track, obstacle_point) ? 1 : 0;
        }
        totals.other_side += sides != 0 ? 1 : 0;
        totals.worst_length = std::max(totals.worst_length, length);
        totals.worst_arrival = std::max(totals.worst_arrival, arrival);
        totals.worst_offset = std::max(totals.worst_offset, offset);
        if (report) {
            std::cout << std::fixed << std::setprecision(3) << search.name << ": exact " << exact.stats.tries << " tries "
                      << exact.stats.expansions << " expansions, bounded " << bounded.stats.tries << " tries "
                      << bounded.stats.expansions << " expansions; length x" << length << ", arrival x" << arrival << " ("
                      << exact_arrival - 3000 << " -> " << bounded_arrival - 3000 << " ticks), largest offset "
                      << std::setprecision(1) << offset << (same ? " (identical)" : "")
                      << (sides != 0 ? " OTHER SIDE of " + std::to_string(sides) : std::string()) << '\n';
        }
        if (profile) {
            const auto per = [&](const std::uint64_t ticks) {
                const double ns = static_cast<double>(ticks) * 1e9 * std::chrono::steady_clock::period::num
                    / std::chrono::steady_clock::period::den;
                return ns / static_cast<double>(std::max<std::uint64_t>(1, probe.last.expansions));
            };
            std::cout << "  per expansion: collision queries " << per(probe.last.query_ticks) << " ns, open/closed set "
                      << per(probe.last.set_ticks) << " ns, rest "
                      << per(probe.last.total_ticks - probe.last.query_ticks - probe.last.set_ticks) << " ns\n";
        }
    }
    std::cout << std::fixed << std::setprecision(3) << searches.size() << " searches (" << totals.identical
              << " identical to the exact search): expansions " << totals.exact_expansions << " exact (at most "
              << totals.exact_max << "), " << totals.bounded_expansions << " bounded (at most " << totals.bounded_max
              << "); " << totals.exact_seconds * 1e3 << " ms exact, " << totals.bounded_seconds * 1e3
              << " ms bounded; worst length x" << totals.worst_length << ", worst arrival x" << totals.worst_arrival
              << ", largest offset " << std::setprecision(1) << totals.worst_offset << " units; "
              << totals.other_side << " pass an obstacle or held ship on the other side\n";
    expect(totals.identical >= least_identical, "at least " + std::to_string(least_identical) + " bounded searches are the exact search");
    expect(totals.other_side <= most_other_side,
        "at most " + std::to_string(most_other_side) + " bounded searches pass an obstacle on the other side");
    expect(totals.worst_length <= worst_length_bound, "the bounded paths are at most x1.02 the exact length");
    expect(totals.worst_arrival <= worst_arrival_bound, "the bounded paths arrive at most x1.035 the exact time");
    expect(totals.worst_offset <= largest_offset_bound, "the bounded paths stay within 1200 units of the exact ones");
    expect(totals.bounded_max <= bounded_expansion_bound, "no bounded search expands more than 5000 nodes");
    expect(totals.bounded_expansions < totals.exact_expansions, "the bounded searches expand fewer nodes than the exact ones");
    if (update) {
        std::ofstream output(golden, std::ios::binary);
        output << "search,mode,tries,expansions,children,queries,windows,leaves,narrow\n";
        for (const auto& row : rows) output << row << '\n';
        std::cout << "wrote " << golden.string() << '\n';
    } else {
        std::ifstream input(golden, std::ios::binary);
        std::string line;
        std::getline(input, line);
        std::vector<std::string> pinned;
        while (std::getline(input, line)) {
            // A Windows checkout may give the fixture CRLF line ends.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pinned.push_back(line);
        }
        expect(pinned.size() == rows.size(), "path-cost.work.csv has a row per search and mode");
        for (std::size_t index = 0; index < std::min(pinned.size(), rows.size()); ++index) {
            expect(pinned[index] == rows[index], "work counts match path-cost.work.csv: pinned " + pinned[index] + ", now " + rows[index]);
        }
    }
    if (failures != 0) {
        std::cerr << failures << " path cost check(s) failed\n";
        return 1;
    }
    std::cout << "path cost contracts passed\n";
    return 0;
}
