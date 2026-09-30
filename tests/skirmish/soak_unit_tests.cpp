// The soak driver's own contracts (#627), without the game: the command line rejects values that
// would make a run pass vacuously, and each invariant fails on a state built to break it and stays
// quiet on one that does not. The units, types and numbers here are invented.

#include "soak_invariants.hpp"
#include "soak_json.hpp"
#include "soak_options.hpp"

#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
namespace soak = eawr::soak;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

math::Fixed fx(const std::int64_t whole) {
    return math::Fixed::from_integer(whole).value();
}

const soak::Options* options_of(const std::variant<soak::Options, std::string>& parsed) {
    return std::get_if<soak::Options>(&parsed);
}

std::string error_of(const std::variant<soak::Options, std::string>& parsed) {
    const auto* text = std::get_if<std::string>(&parsed);
    return text == nullptr ? std::string() : *text;
}

void options_contract() {
    const auto defaults = soak::parse_options({});
    expect(options_of(defaults) != nullptr, "no arguments give the defaults");
    if (options_of(defaults) != nullptr) {
        expect(options_of(defaults)->ticks == 9000 && options_of(defaults)->seeds.size() == 1
                && options_of(defaults)->workers == 1 && options_of(defaults)->hash_workers == 0
                && !options_of(defaults)->strict,
            "the defaults are 1 seed, 9000 ticks, 1 worker, no comparison, not strict");
    }

    const auto full = soak::parse_options({"--seeds", "12,18,40-42", "--ticks", "2500", "--battles", "3", "--workers", "2",
        "--hash-workers", "4", "--hash-every", "5", "--out", "results", "--strict", "--time-budget", "3600.5",
        "--check-every", "2", "--require-decision", "--spawn-window", "300", "--hull-penetration", "0.25",
        "--hull-ticks", "45", "--slot-radius", "6", "--slot-ticks", "600", "--map-margin", "500", "--speed-factor", "3",
        "--craft-speed-factor", "20", "--no-invariants"});
    expect(options_of(full) != nullptr, "every flag is accepted: " + error_of(full));
    if (options_of(full) != nullptr) {
        const auto& options = *options_of(full);
        expect((options.seeds == std::vector<std::uint64_t>{12, 18, 40, 41, 42}), "the seed list has its ranges expanded");
        expect(options.ticks == 2500 && options.battles == 3 && options.workers == 2 && options.hash_workers == 4
                && options.hash_every == 5 && options.check_every == 2,
            "the counts are read");
        expect(options.strict && options.require_decision && options.out.has_value() && !options.invariants
                && options.limits.craft_speed_factor == 20.0,
            "the switches are read");
        expect(std::abs(options.time_budget - 3600.5) < 1e-9, "the time budget is read");
        expect(options.limits.spawn_window == 300 && options.limits.hull_ticks == 45 && options.limits.slot_ticks == 600
                && std::abs(options.limits.hull_penetration - 0.25) < 1e-9 && std::abs(options.limits.slot_radius - 6.0) < 1e-9
                && std::abs(options.limits.map_margin - 500.0) < 1e-9 && std::abs(options.limits.speed_factor - 3.0) < 1e-9,
            "the limits are read");
        // The reproduction flags parse back to the same run.
        const auto flags = soak::reproduction_flags(options, 18);
        std::vector<std::string> words;
        std::string word;
        for (const char letter : flags + ' ') {
            if (letter == ' ') {
                if (!word.empty()) words.push_back(word);
                word.clear();
            } else {
                word += letter;
            }
        }
        const auto again = soak::parse_options(words);
        expect(options_of(again) != nullptr, "the reproduction flags are valid: " + error_of(again));
        if (options_of(again) != nullptr) {
            const auto& repeated = *options_of(again);
            expect(repeated.seeds == std::vector<std::uint64_t>{18} && repeated.ticks == options.ticks
                    && repeated.workers == options.workers && repeated.hash_workers == options.hash_workers
                    && repeated.limits == options.limits && repeated.require_decision == options.require_decision
                    && repeated.check_every == options.check_every && repeated.invariants == options.invariants,
                "the reproduction flags name the same run for one seed");
        }
    }

    // A run that would pass by doing nothing is an error.
    for (const auto& bad : std::vector<std::vector<std::string>>{
             {"--ticks", "0"}, {"--ticks", "abc"}, {"--ticks", "-5"}, {"--ticks", ""}, {"--ticks", "12x"},
             {"--ticks", "1000001"}, {"--ticks"}, {"--seeds", "abc"}, {"--seeds", ""}, {"--seeds", "5-3"},
             {"--seeds", "1,,2"}, {"--seeds", "1-9999999"}, {"--workers", "0"}, {"--workers", "257"},
             {"--battles", "0"}, {"--battles", "many"}, {"--hash-every", "0"}, {"--check-every", "0"},
             {"--hash-workers", "x"}, {"--hash-workers", "1"}, {"--time-budget", "0"}, {"--time-budget", "soon"},
             {"--hull-penetration", "0"}, {"--slot-radius", "-1"}, {"--speed-factor", "0"}, {"--map-margin", "-3"},
             {"--unknown", "1"}, {"12"}, {"12", "2500"}}) {
        std::string joined;
        for (const auto& item : bad) joined += item + ' ';
        expect(options_of(soak::parse_options(bad)) == nullptr, "rejected: " + joined);
    }
    expect(soak::parse_seed_list("7") == std::optional<std::vector<std::uint64_t>>(std::vector<std::uint64_t>{7}),
        "a single seed");
}

void json_contract() {
    expect(soak::json_string("a\"b\\c\nd\te") == "\"a\\\"b\\\\c\\nd\\te\"", "strings are escaped");
    expect(soak::json_string(std::string(1, '\x01')) == "\"\\u0001\"", "control characters are escaped");
    expect(soak::json_number(1.5) == "1.500000", "numbers have six decimals");
    expect(soak::json_number(std::nan("")) == "0", "a number that is not finite is written as 0");
}

void rectangle_contract() {
    // Two 10 x 4 rectangles side by side along X overlap by 6 when their centres are 14 apart.
    expect(std::abs(soak::rectangle_penetration(0, 0, 1, 0, 10, 4, 14, 0, 1, 0, 10, 4) - 6.0) < 1e-9,
        "aligned rectangles overlap by the shared depth");
    expect(soak::rectangle_penetration(0, 0, 1, 0, 10, 4, 21, 0, 1, 0, 10, 4) == 0.0, "rectangles apart do not overlap");
    // Turned a quarter, the second's long side stands across the first's short side.
    expect(soak::rectangle_penetration(0, 0, 1, 0, 10, 4, 0, 13, 0, 1, 10, 4) > 0.0
            && soak::rectangle_penetration(0, 0, 1, 0, 10, 4, 0, 15, 0, 1, 10, 4) == 0.0,
        "a quarter turn moves the reach of the sides");
}

constexpr tactical::TypeId capital_a = 101;
constexpr tactical::TypeId capital_b = 102;
constexpr tactical::TypeId frigate = 103;
constexpr tactical::TypeId fighter = 201;

tactical::MotionTable table() {
    tactical::MotionTable motion;
    for (const auto type : {capital_a, capital_b, frigate}) {
        tactical::MotionProfile profile;
        profile.type_id = type;
        profile.max_speed = fx(5);
        motion.profiles.push_back(profile);
    }
    motion.footprints.push_back({capital_a, tactical::SpaceLayer::capital, fx(50), fx(20), fx(50), false});
    motion.footprints.push_back({capital_b, tactical::SpaceLayer::capital, fx(50), fx(20), fx(50), false});
    motion.footprints.push_back({frigate, tactical::SpaceLayer::frigate, fx(50), fx(20), fx(50), false});
    tactical::CraftProfile craft;
    craft.type_id = fighter;
    craft.max_speed = fx(8);
    motion.squadrons.craft.push_back(craft);
    return motion;
}

tactical::FogRules map() {
    tactical::FogRules rules;
    rules.map_left = fx(-6500);
    rules.map_top = fx(6500);
    rules.cell_size = fx(100);
    rules.cells_wide = 130;
    rules.cells_tall = 130;
    return rules;
}

tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type, const tactical::PlayerId owner,
    const std::int64_t x, const std::int64_t y) {
    tactical::UnitState state;
    state.entity_id = id;
    state.type_id = type;
    state.owner = owner;
    state.position = {fx(x), fx(y), fx(0)};
    return state;
}

bool has(const soak::Invariants& invariants, const std::string& rule) {
    for (const auto& violation : invariants.violations()) {
        if (violation.rule == rule) return true;
    }
    return false;
}

std::size_t count(const soak::Invariants& invariants, const std::string& rule) {
    std::size_t total = 0;
    for (const auto& violation : invariants.violations()) total += violation.rule == rule ? 1 : 0;
    return total;
}

void hull_contract() {
    soak::Limits limits;
    limits.spawn_window = 100;
    limits.hull_ticks = 10;
    limits.speed_factor = 1000.0; // this test moves ships freely
    // Two capital ships on top of each other after the window fail once, after hull_ticks ticks.
    soak::Invariants stacked(table(), map(), limits);
    for (std::uint64_t tick = 1; tick <= 130; ++tick) {
        const std::vector<tactical::UnitState> units{unit(1, capital_a, 1, 0, 0), unit(2, capital_b, 2, 10, 0)};
        stacked.observe(tick, units, {});
    }
    expect(count(stacked, "hull-overlap") == 1, "stacked capital ships fail hull-overlap once");
    if (has(stacked, "hull-overlap")) {
        const auto& violation = stacked.violations().front();
        expect(violation.tick == 110, "the failure is at the tick the run reaches hull_ticks (after the window)");
        expect(violation.units.find("entity 1") != std::string::npos && violation.units.find("entity 2") != std::string::npos,
            "the failure names both ships");
        expect(!violation.guards.empty() && violation.message.find("%") != std::string::npos, "it names its rule and the overlap");
    }
    // The same overlap inside the spawn window is allowed.
    soak::Invariants early(table(), map(), limits);
    for (std::uint64_t tick = 1; tick <= 100; ++tick) {
        const std::vector<tactical::UnitState> units{unit(1, capital_a, 1, 0, 0), unit(2, capital_b, 2, 10, 0)};
        early.observe(tick, units, {});
    }
    expect(early.violations().empty(), "hulls may overlap inside the spawn window");
    // Ships of different layers pass through each other.
    soak::Invariants layers(table(), map(), limits);
    for (std::uint64_t tick = 101; tick <= 160; ++tick) {
        const std::vector<tactical::UnitState> units{unit(1, capital_a, 1, 0, 0), unit(2, frigate, 2, 10, 0)};
        layers.observe(tick, units, {});
    }
    expect(layers.violations().empty(), "ships of different layers may overlap");
    // A short crossing is not a failure.
    soak::Invariants brief(table(), map(), limits);
    for (std::uint64_t tick = 101; tick <= 160; ++tick) {
        const std::int64_t gap = (tick >= 120 && tick < 125) ? 10 : 400;
        const std::vector<tactical::UnitState> units{unit(1, capital_a, 1, 0, 0), unit(2, capital_b, 2, gap, 0)};
        brief.observe(tick, units, {});
    }
    expect(brief.violations().empty(), "a crossing shorter than hull_ticks passes");
    expect(brief.metrics().max_hull_run == 5, "the longest run is reported");
}

void bounds_and_speed_contract() {
    soak::Limits limits;
    soak::Invariants bounds(table(), map(), limits);
    bounds.observe(1, std::vector<tactical::UnitState>{unit(1, capital_a, 1, 6000, 0), unit(2, capital_b, 1, 7000, 0)}, {});
    expect(bounds.violations().empty(), "a unit inside the margin past the grid is allowed");
    bounds.observe(2, std::vector<tactical::UnitState>{unit(1, capital_a, 1, 6000, 0), unit(2, capital_b, 1, 8000, 0)}, {});
    expect(count(bounds, "out-of-bounds") == 1, "a unit past the margin fails out-of-bounds");
    bounds.observe(3, std::vector<tactical::UnitState>{unit(1, capital_a, 1, 6000, 0), unit(2, capital_b, 1, 8100, 0)}, {});
    expect(count(bounds, "out-of-bounds") == 1, "the same unit is reported once");

    soak::Invariants speed(table(), map(), limits);
    speed.observe(1, std::vector<tactical::UnitState>{unit(1, capital_a, 1, 0, 0), unit(3, fighter, 1, 0, 100)}, {});
    speed.observe(2, std::vector<tactical::UnitState>{unit(1, capital_a, 1, 5, 0), unit(3, fighter, 1, 10, 100)}, {});
    expect(speed.violations().empty(), "moving up to a quarter past the maximum passes");
    speed.observe(3, std::vector<tactical::UnitState>{unit(1, capital_a, 1, 400, 0), unit(3, fighter, 1, 10, 100)}, {});
    expect(count(speed, "speed") == 1, "a ship that jumps fails speed");
    speed.observe(4, std::vector<tactical::UnitState>{unit(1, capital_a, 1, 400, 0), unit(3, fighter, 1, 100, 100)}, {});
    expect(count(speed, "speed") == 1, "a craft catching up at 11 times its speed passes (FM-12 has no cap)");
    speed.observe(5, std::vector<tactical::UnitState>{unit(1, capital_a, 1, 400, 0), unit(3, fighter, 1, 1000, 100)}, {});
    expect(count(speed, "speed") == 2, "a craft that jumps fails speed"); 
    if (has(speed, "speed")) {
        expect(speed.violations().front().tick == 3 && speed.violations().front().units.find("entity 1") != std::string::npos,
            "it names the tick and the ship");
    }
    // Every second tick is checked: the move is spread over two ticks.
    soak::Invariants sparse(table(), map(), limits);
    sparse.observe(2, std::vector<tactical::UnitState>{unit(1, capital_a, 1, 0, 0)}, {});
    sparse.observe(4, std::vector<tactical::UnitState>{unit(1, capital_a, 1, 20, 0)}, {});
    expect(sparse.violations().empty(), "a move over two ticks is divided by the two");
}

void slot_contract() {
    soak::Limits limits;
    limits.slot_ticks = 5;
    limits.slot_radius = 4.0;
    limits.speed_factor = 1000.0;
    const std::vector<tactical::Squadron> squadrons{{10, {11, 12}}, {20, {21}}};
    soak::Invariants crowded(table(), map(), limits);
    for (std::uint64_t tick = 1; tick <= 8; ++tick) {
        const std::vector<tactical::UnitState> units{unit(10, 300, 1, 0, 0), unit(11, fighter, 1, 0, 0),
            unit(12, fighter, 1, 30, 0), unit(20, 300, 1, 0, 0), unit(21, fighter, 1, 2, 0)};
        crowded.observe(tick, units, squadrons);
    }
    expect(count(crowded, "slot-crowding") == 1, "craft of two squadrons of one side on each other fail slot-crowding once");
    if (has(crowded, "slot-crowding")) {
        const auto& violation = crowded.violations().front();
        expect(violation.tick == 5, "the failure is at the tick the run reaches slot_ticks");
        expect(violation.units.find("entity 11") != std::string::npos && violation.units.find("entity 21") != std::string::npos,
            "it names both craft");
    }
    // A dogfight (opposing sides) may overlap, and so may one squadron's own craft.
    soak::Invariants fight(table(), map(), limits);
    for (std::uint64_t tick = 1; tick <= 20; ++tick) {
        const std::vector<tactical::UnitState> units{unit(11, fighter, 1, 0, 0), unit(12, fighter, 1, 1, 0),
            unit(21, fighter, 2, 2, 0)};
        fight.observe(tick, units, squadrons);
    }
    expect(fight.violations().empty(), "opposing craft and one squadron's own craft may overlap");
    // Leaving ends the run: three ticks close, one apart, three close again is not a run of five.
    soak::Invariants apart(table(), map(), limits);
    for (std::uint64_t tick = 1; tick <= 7; ++tick) {
        const std::int64_t gap = tick == 4 ? 100 : 2;
        const std::vector<tactical::UnitState> units{unit(11, fighter, 1, 0, 0), unit(21, fighter, 1, gap, 0)};
        apart.observe(tick, units, squadrons);
    }
    expect(apart.violations().empty(), "a run that is broken starts again");
}

void cap_contract() {
    soak::Limits limits;
    limits.hull_ticks = 1;
    limits.spawn_window = 0;
    soak::Invariants many(table(), map(), limits);
    // 60 stacked pairs each fail once; only max_kept are kept, the rest are counted.
    for (std::uint64_t tick = 1; tick <= 2; ++tick) {
        std::vector<tactical::UnitState> units;
        for (eawr::sim::EntityId pair = 0; pair < 60; ++pair) {
            units.push_back(unit(1000 + pair * 2, capital_a, 1, static_cast<std::int64_t>(pair) * 2000, 0));
            units.push_back(unit(1001 + pair * 2, capital_b, 2, static_cast<std::int64_t>(pair) * 2000 + 10, 0));
        }
        many.observe(tick, units, {});
    }
    expect(many.violations().size() == soak::Invariants::max_kept, "kept violations are capped");
    expect(many.dropped() > 0, "the rest are counted");
}

} // namespace

int main() {
    options_contract();
    json_contract();
    rectangle_contract();
    hull_contract();
    bounds_and_speed_contract();
    slot_contract();
    cap_contract();
    if (failures != 0) {
        std::cerr << failures << " soak contract(s) failed\n";
        return 1;
    }
    std::cout << "soak contracts passed\n";
    return 0;
}
