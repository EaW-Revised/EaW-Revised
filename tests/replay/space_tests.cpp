#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/space.hpp"
#include "eawr/sim/tactical/visibility.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// P2-05 (#68): deterministic space queries and per-player sensor visibility. Query
// results are checked against a brute-force filter and across insertion orders; the
// visibility fixture comes from the independent oracle generate_tactical_fixture.py.
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;

[[nodiscard]] math::Fixed units(const std::int64_t value) {
    return math::Fixed::from_raw(value * one);
}

[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}

[[nodiscard]] math::Vec3 raw(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {math::Fixed::from_raw(x), math::Fixed::from_raw(y), math::Fixed::from_raw(z)};
}

// SplitMix64: a fixed, portable pseudo-random stream for generated bodies and queries.
class Stream final {
public:
    explicit Stream(const std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        auto z = state_;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    // Uniform raw value in [-span, span] units, at full Q24 resolution.
    std::int64_t coordinate(const std::int64_t span) {
        const auto width = static_cast<std::uint64_t>(2 * span * one + 1);
        return static_cast<std::int64_t>(next() % width) - span * one;
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] tactical::SpaceIndex index_of(const std::vector<tactical::SpaceBody>& bodies) {
    auto built = tactical::SpaceIndex::build(bodies);
    expect(static_cast<bool>(built), "space index builds");
    return built ? std::move(built).value() : tactical::SpaceIndex{};
}

struct Query {
    math::Vec3 centre;
    math::Vec3 half_extent;
    math::Fixed radius;
    tactical::RangeMetric metric{tactical::RangeMetric::planar};
    std::optional<tactical::PlayerId> owner;
};

// Brute-force expectations: every body in ascending ID order, filtered by the exact tests.
[[nodiscard]] std::vector<eawr::sim::EntityId> brute_box(
    std::vector<tactical::SpaceBody> bodies, const Query& query) {
    std::sort(bodies.begin(), bodies.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
    std::vector<eawr::sim::EntityId> result;
    for (const auto& body : bodies) {
        if ((!query.owner || body.owner == *query.owner)
            && tactical::within_box(query.centre, query.half_extent, body.position)) {
            result.push_back(body.entity_id);
        }
    }
    return result;
}

[[nodiscard]] std::vector<eawr::sim::EntityId> brute_range(
    std::vector<tactical::SpaceBody> bodies, const Query& query) {
    std::sort(bodies.begin(), bodies.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
    std::vector<eawr::sim::EntityId> result;
    for (const auto& body : bodies) {
        if ((!query.owner || body.owner == *query.owner)
            && tactical::within_range(query.centre, body.position, query.radius, query.metric)) {
            result.push_back(body.entity_id);
        }
    }
    return result;
}

void test_exact_predicates() {
    // 3-4-5 triangles: exactly on the boundary is inside; one raw unit further is outside.
    expect(tactical::within_range(at(0, 0), at(720, 960), units(1200), tactical::RangeMetric::planar),
           "diagonal distance exactly 1200 is within 1200");
    expect(!tactical::within_range(at(0, 0), raw(720 * one, 960 * one + 1), units(1200), tactical::RangeMetric::planar),
           "one raw unit beyond 1200 is outside");
    expect(tactical::within_range(at(0, 0), at(300, 0, 400), units(500), tactical::RangeMetric::spatial),
           "spatial 3-4-5 on the boundary is inside");
    expect(!tactical::within_range(at(0, 0), raw(300 * one, 0, 400 * one + 1), units(500), tactical::RangeMetric::spatial),
           "spatial one raw unit beyond is outside");
    expect(tactical::within_range(at(0, 0), at(300, 0, 4000), units(300), tactical::RangeMetric::planar),
           "planar ignores Z");
    expect(!tactical::within_range(at(0, 0), at(0, 0), math::Fixed::from_raw(-1), tactical::RangeMetric::planar),
           "a negative radius matches nothing, not even the centre");
    expect(tactical::within_range(at(5, 5), at(5, 5), math::Fixed{}, tactical::RangeMetric::spatial),
           "radius zero matches the same point");

    // Extremes: differences reach 2^64 - 1 and squares 2^128; nothing overflows.
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    expect(tactical::within_range(raw(0, 0), raw(high, 0), math::Fixed::from_raw(high), tactical::RangeMetric::planar),
           "distance INT64_MAX is within radius INT64_MAX");
    expect(!tactical::within_range(raw(0, 0), raw(low, 0), math::Fixed::from_raw(high), tactical::RangeMetric::planar),
           "distance 2^63 is beyond radius INT64_MAX");
    expect(!tactical::within_range(raw(low, low, low), raw(high, high, high), math::Fixed::from_raw(high),
               tactical::RangeMetric::spatial),
           "opposite corners of the raw domain are far apart");
    expect(tactical::within_box(raw(low, 0, 0), raw(high, 0, 0), raw(-1, 0, 0)),
           "box half extent INT64_MAX from INT64_MIN reaches -1");
    expect(!tactical::within_box(raw(low, 0, 0), raw(high, 0, 0), raw(0, 0, 0)),
           "and not 0");

    expect(tactical::within_box(at(0, 0, 0), at(10, 20, 30), at(-10, 20, -30)), "box corners are inside");
    expect(!tactical::within_box(at(0, 0, 0), at(10, 20, 30), raw(0, 0, 30 * one + 1)), "one raw unit beyond a face");
    expect(!tactical::within_box(at(0, 0, 0), raw(one, -1, one), at(0, 0, 0)), "a negative half extent is empty");
}

void test_index_matches_brute_force() {
    Stream stream(0x5EED0068);
    std::vector<tactical::SpaceBody> bodies;
    // Sparse, non-contiguous IDs; positions over several 1024-unit cells, including
    // clusters on a few exact coordinates.
    for (std::uint64_t index = 0; index < 400; ++index) {
        const auto id = 3 + index * 7 + (stream.next() % 5);
        auto position = raw(stream.coordinate(6000), stream.coordinate(6000), stream.coordinate(400));
        if (index % 13 == 0) position = at(1024, -1024, 0);
        bodies.push_back({id, static_cast<tactical::PlayerId>(1 + stream.next() % 3), position});
    }
    const auto index = index_of(bodies);
    expect(index.size() == bodies.size(), "every body is indexed");

    std::vector<tactical::SpaceBody> reversed(bodies.rbegin(), bodies.rend());
    std::vector<tactical::SpaceBody> rotated(bodies.begin() + 137, bodies.end());
    rotated.insert(rotated.end(), bodies.begin(), bodies.begin() + 137);
    const auto from_reversed = index_of(reversed);
    const auto from_rotated = index_of(rotated);

    std::size_t checked = 0;
    std::size_t nonempty = 0;
    std::vector<std::uint32_t> positions; // reused across queries, as the projectile phase does
    for (int trial = 0; trial < 600; ++trial) {
        Query query;
        query.centre = raw(stream.coordinate(7000), stream.coordinate(7000), stream.coordinate(400));
        if (trial % 17 == 0) query.centre = at(1024, -1024, 0);
        // Mostly tactical sizes; every 25th query spans the whole domain (linear path).
        const auto span = trial % 25 == 0 ? std::int64_t{1} << 38 : static_cast<std::int64_t>(stream.next() % 2500);
        query.radius = math::Fixed::from_raw(span * one + static_cast<std::int64_t>(stream.next() % one));
        query.half_extent = raw(static_cast<std::int64_t>(stream.next() % 2000) * one,
            static_cast<std::int64_t>(stream.next() % 2000) * one, static_cast<std::int64_t>(stream.next() % 600) * one);
        query.metric = trial % 2 == 0 ? tactical::RangeMetric::planar : tactical::RangeMetric::spatial;
        if (trial % 3 == 0) query.owner = static_cast<tactical::PlayerId>(1 + trial % 4);

        const auto boxed = index.box(query.centre, query.half_extent, query.owner);
        const auto ranged = index.range(query.centre, query.radius, query.metric, query.owner);
        expect(boxed == brute_box(bodies, query), "box query equals the brute-force filter");
        // #636: box_positions is box() without the owner filter and the order.
        if (!query.owner) {
            index.box_positions(query.centre, query.half_extent, positions);
            std::sort(positions.begin(), positions.end());
            std::vector<eawr::sim::EntityId> ids;
            for (const auto position : positions) ids.push_back(index.bodies()[position].entity_id);
            expect(ids == boxed, "box_positions, sorted, are the box query's bodies");
        }
        expect(ranged == brute_range(bodies, query), "range query equals the brute-force filter");
        expect(std::is_sorted(ranged.begin(), ranged.end())
                   && std::adjacent_find(ranged.begin(), ranged.end()) == ranged.end(),
               "range results strictly increase by ID");
        expect(from_reversed.box(query.centre, query.half_extent, query.owner) == boxed
                   && from_rotated.box(query.centre, query.half_extent, query.owner) == boxed,
               "box results do not depend on insertion order");
        expect(from_reversed.range(query.centre, query.radius, query.metric, query.owner) == ranged
                   && from_rotated.range(query.centre, query.radius, query.metric, query.owner) == ranged,
               "range results do not depend on insertion order");
        ++checked;
        if (!ranged.empty()) ++nonempty;
    }
    expect(checked == 600 && nonempty > 100, "the generated queries hit bodies");

    // R-07 shape: a 1.1 * 900 = 990 half-extent box per hostile owner.
    const auto hostile = index.box(at(0, 0, 0), at(990, 990, 990), tactical::PlayerId{2});
    for (const auto id : hostile) {
        const auto* body = index.find(id);
        expect(body != nullptr && body->owner == 2, "owner filter keeps only that owner");
    }
    expect(index.find(1) == nullptr && index.find(bodies.front().entity_id) != nullptr, "find by stable ID");
}

void test_index_rejects_bad_ids() {
    const std::vector<tactical::SpaceBody> repeated{{4, 1, at(0, 0)}, {4, 2, at(1, 1)}};
    const auto duplicate = tactical::SpaceIndex::build(repeated);
    expect(!duplicate && duplicate.error().code == "EAWR-SIM-0304", "a repeated ID is rejected");
    const std::vector<tactical::SpaceBody> zero{{0, 1, at(0, 0)}};
    const auto zeroed = tactical::SpaceIndex::build(zero);
    expect(!zeroed && zeroed.error().code == "EAWR-SIM-0304", "ID zero is rejected");
    const auto empty = tactical::SpaceIndex::build({});
    expect(empty && empty.value().range(at(0, 0), units(100), tactical::RangeMetric::planar).empty(),
           "an empty index answers nothing");
}

[[nodiscard]] tactical::UnitState unit(
    const eawr::sim::EntityId id, const tactical::TypeId type, const tactical::PlayerId owner, const math::Vec3& position) {
    return tactical::UnitState{id, type, owner, position, math::identity_quat(), {}};
}

void test_team_sharing_and_sensor_table() {
    // Players 1 and 4 share team 0; player 2 is team 1. Bits follow ascending player ID.
    const std::vector<tactical::Player> players{{1, 0, 1, 1}, {2, 1, 2, 1}, {4, 0, 3, 1}};
    const std::vector<tactical::SensorProfile> sensors{{10, units(500)}, {20, units(1200)}};
    const std::vector<tactical::UnitState> states{
        unit(1, 10, 4, at(0, 0)),       // ally fighter of player 4, 500
        unit(2, 30, 1, at(-5000, 0)),   // player 1 type without a sensor
        unit(3, 20, 2, at(500, 0)),     // enemy corvette on the ally's boundary
        unit(4, 20, 2, at(3000, 0)),    // enemy corvette far away
    };
    auto built = tactical::SensorField::build(players, states, sensors);
    expect(static_cast<bool>(built), "sensor field builds");
    if (!built) return;
    const auto& field = built.value();
    expect(field.visible_to(2, at(500, 0)) == 0b111, "the ally's sensor reveals the enemy to both team-0 players");
    expect(field.visible_to(2, at(3000, 0)) == 0b010, "a distant enemy is seen only by its owner");
    expect(field.visible_to(1, at(-5000, 0)) == 0b101, "a team-0 unit is seen by both team-0 players");
    expect(field.visible_to(4, at(0, 0)) == 0b111, "the enemy corvette's 1200 reveals the ally fighter");
    expect(field.reveal_range(20) == units(1200) && !field.reveal_range(30), "sensor lookup by type");

    const std::vector<tactical::SensorProfile> unordered{{20, units(1)}, {10, units(1)}};
    const std::vector<tactical::SensorProfile> repeated{{10, units(1)}, {10, units(2)}};
    const std::vector<tactical::SensorProfile> negative{{10, math::Fixed::from_raw(-1)}};
    for (const auto& table : {unordered, repeated, negative}) {
        const auto valid = tactical::validate_sensors(table);
        expect(!valid && valid.error().code == "EAWR-SIM-0305", "a bad sensor table is an invalid setup");
    }
    tactical::TacticalSetup setup;
    setup.players = players;
    setup.units = states;
    const auto rejected = tactical::TacticalSession::create(setup, negative);
    expect(!rejected && rejected.error().code == "EAWR-SIM-0305", "a session refuses a bad sensor table");
    const auto accepted = tactical::TacticalSession::create(setup, sensors);
    expect(accepted && accepted.value().snapshot()->visible_entities(1) == std::vector<eawr::sim::EntityId>{1, 2, 3},
           "player 1 sees its own team's units and the enemy inside the ally's range");
    expect(accepted && accepted.value().snapshot()->visible_entities(99).empty(), "an unknown player sees nothing");
}

// --- Oracle fixture ------------------------------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::vector<std::vector<std::string>> read_csv(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<std::vector<std::string>> rows;
    std::string line;
    bool header = true;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (header) {
            header = false;
            continue;
        }
        std::vector<std::string> fields;
        std::stringstream split(line);
        std::string field;
        while (std::getline(split, field, ',')) fields.push_back(field);
        rows.push_back(std::move(fields));
    }
    return rows;
}

[[nodiscard]] std::string hex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    for (const auto byte : digest) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 15U]);
    }
    return text;
}

[[nodiscard]] std::string masks_of(const tactical::TacticalSnapshot& snapshot) {
    std::string text;
    for (const auto& instance : snapshot.instances()) {
        if (!text.empty()) text += ';';
        text += std::to_string(instance.entity_id) + ':' + std::to_string(instance.visible_to);
    }
    return text;
}

void test_visibility_fixture(const std::string& fixtures) {
    std::vector<tactical::SensorProfile> sensors;
    for (const auto& row : read_csv(fixtures + "/tactical-visibility.sensors.csv")) {
        sensors.push_back({std::stoull(row.at(0)), math::Fixed::from_raw(std::stoll(row.at(1)))});
    }
    const auto layout_rows = read_csv(fixtures + "/tactical-visibility.fog-layout.csv");
    expect(sensors.size() == 4 && layout_rows.size() == 1, "fixture sensor table and fog layout load");
    if (layout_rows.size() != 1) return;
    const auto& l = layout_rows.front();
    const tactical::FogLayout layout{
        math::Fixed::from_raw(std::stoll(l.at(0))), math::Fixed::from_raw(std::stoll(l.at(1))),
        math::Fixed::from_raw(std::stoll(l.at(2))), math::Fixed::from_raw(std::stoll(l.at(3))),
        static_cast<std::uint32_t>(std::stoul(l.at(4))), static_cast<std::uint32_t>(std::stoul(l.at(5)))};

    std::vector<std::unique_ptr<const eawr::platform::ThreadWorkerAdapter>> executors;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        executors.push_back(std::make_unique<const eawr::platform::ThreadWorkerAdapter>(workers));
    }

    std::string seen_by_rebel;
    std::string seen_by_empire;
    const auto frames = read_csv(fixtures + "/tactical-visibility.frames.csv");
    expect(frames.size() == 8, "eight oracle frames");
    for (const auto& frame : frames) {
        const auto label = "frame " + frame.at(0) + ": ";
        const auto parsed = tactical::parse_replay(read_bytes(fixtures + "/" + frame.at(1)), frame.at(1));
        expect(static_cast<bool>(parsed), label + "replay parses");
        if (!parsed) continue;
        auto created = tactical::TacticalSession::from_replay(parsed.value(), sensors);
        expect(static_cast<bool>(created), label + "session is created with the sensor table");
        if (!created) continue;
        const auto initial = created.value().snapshot();
        expect(initial->sha256() == frame.at(6), label + "tick-0 snapshot matches the oracle");
        expect(masks_of(*initial) == frame.at(5), label + "visibility masks match the oracle");
        const auto rebel = initial->visible_entities(1);
        const auto empire = initial->visible_entities(2);
        const bool rebel_sees = std::find(rebel.begin(), rebel.end(), 5U) != rebel.end();
        const bool empire_sees = std::find(empire.begin(), empire.end(), 1U) != empire.end();
        expect((rebel_sees ? "yes" : "no") == frame.at(3), label + "the rebel player's view of the fighter");
        expect((empire_sees ? "yes" : "no") == frame.at(4), label + "the empire player's view of the frigate");
        seen_by_rebel += rebel_sees ? '1' : '0';
        seen_by_empire += empire_sees ? '1' : '0';

        const auto grids = tactical::fog_grids(*initial, layout);
        expect(static_cast<bool>(grids) && grids.value().size() == 3, label + "one fog grid per team");
        if (grids) {
            for (std::size_t team = 0; team < grids.value().size(); ++team) {
                expect(hex(grids.value().grids()[team].sha256()) == frame.at(9 + team),
                       label + "fog grid of team " + std::to_string(grids.value().grids()[team].team_id())
                           + " matches the oracle");
            }
        }

        for (const auto& executor : executors) {
            for (const bool scramble : {false, true}) {
                auto session = tactical::TacticalSession::from_replay(parsed.value(), sensors);
                if (!session) continue;
                if (scramble) session.value().scramble_storage_for_testing();
                const auto tick = session.value().step(*executor);
                const auto context = label + std::to_string(executor->worker_count()) + " worker(s)"
                    + (scramble ? ", scrambled storage" : "");
                expect(static_cast<bool>(tick), context + ": step succeeds");
                if (!tick) continue;
                expect(tick.value().state_sha256 == frame.at(7), context + ": state hash matches the oracle");
                expect(tick.value().snapshot->sha256() == frame.at(8), context + ": snapshot matches the oracle");
                expect(masks_of(*tick.value().snapshot) == frame.at(5), context + ": masks unchanged by the step");
            }
        }
    }
    // The enemy fighter is hidden, becomes visible at 1200, stays visible inside, and is
    // hidden again one raw unit beyond the range; its own 500 reveals the frigate once.
    expect(seen_by_rebel == "00111100", "rebel view of the fighter across the frames: " + seen_by_rebel);
    expect(seen_by_empire == "00010000", "empire view of the frigate across the frames: " + seen_by_empire);
}

void test_session_visibility_across_workers() {
    // A larger generated battle: every worker count and storage order publishes the same
    // snapshot, masks included.
    Stream stream(0x0068FEED);
    tactical::TacticalSetup setup;
    setup.seed = 68;
    setup.players = {{1, 0, 1, 1}, {2, 1, 2, 1}, {3, 3, 3, 0}};
    const std::vector<tactical::SensorProfile> sensors{{1, units(500)}, {2, units(600)}, {3, units(1200)}, {4, units(2000)}};
    for (eawr::sim::EntityId id = 1; id <= 240; ++id) {
        setup.units.push_back(unit(id, 1 + stream.next() % 5, static_cast<tactical::PlayerId>(1 + stream.next() % 3),
            raw(stream.coordinate(5000), stream.coordinate(5000), stream.coordinate(100))));
    }
    std::optional<std::string> expected;
    std::optional<std::string> expected_masks;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        for (const bool scramble : {false, true}) {
            auto session = tactical::TacticalSession::create(setup, sensors);
            expect(static_cast<bool>(session), "generated session is created");
            if (!session) return;
            if (scramble) session.value().scramble_storage_for_testing();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::string digests = session.value().snapshot()->sha256();
            std::string masks;
            for (int step = 0; step < 3; ++step) {
                const auto tick = session.value().step(executor);
                expect(static_cast<bool>(tick), "generated step succeeds");
                if (!tick) return;
                digests += ',' + tick.value().state_sha256 + ',' + tick.value().snapshot->sha256();
                masks = masks_of(*tick.value().snapshot);
            }
            if (!expected) {
                expected = digests;
                expected_masks = masks;
            }
            expect(digests == *expected, "hashes and snapshots agree across worker counts and storage orders");
            expect(masks == *expected_masks, "visibility masks agree across worker counts and storage orders");
        }
    }
    // The masks come from real sensor contact, not only team membership.
    auto session = tactical::TacticalSession::create(setup, sensors);
    std::size_t cross_team = 0;
    for (const auto& instance : session.value().snapshot()->instances()) {
        const auto owner_bit = std::uint64_t{1} << (instance.owner - 1U);
        if ((instance.visible_to & ~owner_bit) != 0U) ++cross_team;
    }
    expect(cross_team > 20, "many generated units are seen by another team");
}

} // namespace

int main(const int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: tactical_space_tests <fixture directory>\n";
        return 2;
    }
    test_exact_predicates();
    test_index_matches_brute_force();
    test_index_rejects_bad_ids();
    test_team_sharing_and_sensor_table();
    test_visibility_fixture(argv[1]);
    test_session_visibility_across_workers();
    if (failures != 0) {
        std::cerr << failures << " space query/visibility contract(s) failed\n";
        return 1;
    }
    std::cout << "space query and visibility contracts passed\n";
    return 0;
}
