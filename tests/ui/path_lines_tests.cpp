// PL-05..PL-08: the selected-ship path line builder (docs/behaviour/space-path-lines.md). Counted work,
// no clock: the rebuild limit takes its clock as an argument.

#include "eawr/presentation/ui/path_lines.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "ui_test_support.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
namespace tactical = sim::tactical;
using sim::math::Fixed;
using test::ui::expect;

Fixed units(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

tactical::PredictedPathNode node(const std::int64_t frame, const std::int64_t x, const std::int64_t y,
    const std::int64_t yaw, const std::int64_t speed) {
    return {units(frame), {units(x), units(y), units(-20)}, units(yaw), units(speed)};
}

std::shared_ptr<const tactical::PredictedPath> plan(std::vector<tactical::PredictedPathNode> nodes) {
    auto path = std::make_shared<tactical::PredictedPath>();
    path->nodes = std::move(nodes);
    return path;
}

// A straight leg of `length` units at 5 units a frame, starting at frame `start`.
std::shared_ptr<const tactical::PredictedPath> straight(const std::int64_t length, const std::int64_t start = 0,
    const std::int64_t y = 0) {
    return plan({node(start, 0, y, 0, 5), node(start + length / 5, length, y, 0, 5)});
}

bool close(const float a, const double b, const double tolerance = 1.0e-3) { return std::abs(a - b) <= tolerance; }

void known_plan() {
    ui::PathLineBuilder builder;
    const std::vector<ui::PathLineShip> ships{{1, straight(100)}};
    expect(builder.update(true, ships, 0.0), "PL-05: a selected ship with a plan builds a line");
    const auto vertices = builder.vertices();
    expect(vertices.size() == 10 && builder.stats().segments == 5 && builder.stats().ships == 1,
        "PL-05: 100 units at a 20-unit step is five segments");
    expect(close(vertices.front().x, 0.0) && close(vertices.back().x, 100.0) && close(vertices.front().z, -20.0),
        "PL-05: the line runs from the plan start to the plan end");
    bool chained = true;
    for (std::size_t segment = 1; segment < 5; ++segment)
        chained = chained && vertices[segment * 2].x == vertices[segment * 2 - 1].x;
    expect(chained, "PL-05: segments share their end points");
    expect(close(vertices[1].x, 20.0) && close(vertices[3].x, 40.0), "PL-05: a segment ends once the track has moved 20 units");
    expect(vertices.front().frame == 0.0F && close(vertices.back().frame, 20.0), "PL-06: vertices carry the plan frame they are reached");

    ui::PathLineBuilder shorter;
    const std::vector<ui::PathLineShip> near_ship{{1, straight(10)}};
    shorter.update(true, near_ship, 0.0);
    expect(shorter.vertices().size() == 2 && close(shorter.vertices()[1].x, 10.0),
        "PL-05: a plan shorter than the step still ends with its last piece");

    ui::PathLineBuilder odd;
    const std::vector<ui::PathLineShip> uneven{{1, straight(110)}};
    odd.update(true, uneven, 0.0);
    expect(odd.stats().segments == 6 && close(odd.vertices().back().x, 110.0) && close(odd.vertices()[10].x, 100.0),
        "PL-05: the last piece is shorter than the step");
}

// The builder samples the plan with the movement's own Hermite rule: compare with the sim.
void agrees_with_the_movement() {
    const auto curve = plan({node(0, 0, 0, 0, 5), node(60, 250, 100, 45, 5), node(120, 300, 300, 90, 5)});
    ui::PathLineBuilder builder;
    const std::vector<ui::PathLineShip> ships{{1, curve}};
    builder.update(true, ships, 0.0);
    tactical::MotionState state;
    state.kind = tactical::MotionKind::path;
    for (const auto& n : curve->nodes) state.nodes.push_back({n.frame, n.position, n.yaw, n.speed});
    bool all = !builder.vertices().empty();
    for (const auto& vertex : builder.vertices()) {
        const auto frame = static_cast<std::uint64_t>(std::llround(vertex.frame + builder.base_frame()));
        if (std::abs(vertex.frame + builder.base_frame() - static_cast<double>(frame)) > 1.0e-4) continue; // between ticks
        if (frame >= 120) continue; // the movement does not snap to the last node (MV-32); the line ends on it
        const auto sample = tactical::sample_motion(state, frame, state.nodes.front().position, Fixed{});
        if (!sample) { all = false; continue; }
        const double sx = static_cast<double>(sample.value().position.x.raw()) / Fixed::scale;
        const double sy = static_cast<double>(sample.value().position.y.raw()) / Fixed::scale;
        all = all && std::abs(vertex.x - sx) < 0.05 && std::abs(vertex.y - sy) < 0.05;
    }
    expect(all, "PL-03: line points equal the movement's own samples of the plan");
}

void thinning_keeps_the_tail() {
    ui::PathLineBuilder builder;
    const std::vector<ui::PathLineShip> ships{{1, straight(5000)}};
    builder.update(true, ships, 0.0);
    expect(builder.stats().segments == 64 && builder.vertices().size() == 128 && builder.stats().ships_thinned == 1,
        "PL-08: a long plan is thinned to 64 segments");
    expect(close(builder.vertices().back().x, 5000.0) && close(builder.vertices().front().x, 0.0),
        "PL-08: thinning keeps both ends of the track");
    bool even = true;
    for (std::size_t segment = 0; segment < 64; ++segment) {
        const double length = builder.vertices()[segment * 2 + 1].x - builder.vertices()[segment * 2].x;
        even = even && length > 55.0 && length < 85.0;
    }
    expect(even, "PL-08: thinning raises the step evenly (about 78 units), it does not cut the tail");
}

void caps() {
    ui::PathLineBuilder builder;
    std::vector<ui::PathLineShip> ships;
    for (sim::EntityId id = 1; id <= 300; ++id) ships.push_back({id, straight(5000, 0, static_cast<std::int64_t>(id))});
    builder.update(true, ships, 0.0);
    const auto& stats = builder.stats();
    expect(stats.ships == 256 && stats.ships_over_cap == 44, "PL-08: at most 256 ships; the earliest selected win");
    expect(stats.segments == 256U * 64U && stats.segments <= 16384 && builder.vertices().size() == 2U * stats.segments,
        "PL-08: at most 16,384 segments for a full select-all");
    expect(stats.rebuilds == 1, "PL-08: one rebuild for the whole fleet");
    const auto work = stats.points_sampled;
    for (int frame = 1; frame <= 120; ++frame) expect(!builder.update(true, ships, 0.0 + frame / 60.0), "PL-07: an idle frame changes nothing");
    expect(builder.stats().points_sampled == work && builder.stats().rebuilds == 1 && builder.stats().segments_written == stats.segments,
        "PL-07: idle frames do zero builder work");
    // Only the first 256 ships, in selection order, are drawn.
    bool first_ships = true;
    for (std::size_t ship = 0; ship < 256; ++ship)
        first_ships = first_ships && close(builder.vertices()[ship * 128].y, static_cast<double>(ship + 1));
    expect(first_ships, "PL-08: the ship cap keeps the earliest-selected ships");
}

void rebuilds_only_on_events() {
    ui::PathLineBuilder builder;
    std::vector<ui::PathLineShip> ships{{1, straight(200)}, {2, straight(300, 0, 50)}};
    expect(builder.update(true, ships, 10.0) && builder.stats().rebuilds == 1, "PL-07: the first frame builds");
    const auto generation = builder.generation();
    expect(!builder.update(true, ships, 10.01) && builder.generation() == generation, "PL-07: an unchanged selection and plans do not rebuild");
    // A replan is a new shared plan: the rule rebuilds, but not within the rate limit.
    ships[0].path = straight(400, 30);
    expect(!builder.update(true, ships, 10.1) && builder.stats().rebuilds == 1 && builder.stats().deferred == 1,
        "PL-08: a replan inside the limit waits");
    expect(builder.update(true, ships, 10.3) && builder.stats().rebuilds == 2, "PL-08: the held change builds once the interval has passed");
    // A replan storm: one new plan every frame for a second is at most four rebuilds.
    ui::PathLineBuilder storm;
    std::vector<ui::PathLineShip> fleet;
    for (sim::EntityId id = 1; id <= 100; ++id) fleet.push_back({id, straight(1000)});
    for (int frame = 0; frame < 60; ++frame) {
        fleet[static_cast<std::size_t>(frame) % fleet.size()].path = straight(1000, frame);
        storm.update(true, fleet, 100.0 + frame / 60.0);
    }
    expect(storm.stats().rebuilds <= 4 && storm.stats().rebuilds >= 3, "PL-08: a replan storm costs at most four rebuilds a second");
    // Plans are sampled once each, not once per rebuild.
    expect(storm.stats().plans_sampled <= 100 + 60, "PL-07: a plan is sampled once, however often the list is rebuilt");
}

void clears() {
    ui::PathLineBuilder builder;
    std::vector<ui::PathLineShip> ships{{1, straight(200)}};
    builder.update(true, ships, 0.0);
    expect(builder.update(false, ships, 0.01) && builder.vertices().empty(), "PL-04: the flag off draws nothing");
    expect(!builder.update(false, ships, 0.02) && builder.stats().clears == 1, "PL-04: flag off is free once cleared");
    expect(builder.update(true, ships, 0.03) && !builder.vertices().empty(), "PL-04: the flag on builds at once after a clear");
    expect(builder.update(true, {}, 0.04) && builder.vertices().empty(), "PL-04: deselecting clears at once, inside the rate limit");
    std::vector<ui::PathLineShip> none{{1, nullptr}};
    expect(!builder.update(true, none, 0.5) || builder.vertices().empty(), "PL-04: a ship without a plan draws nothing");
}

} // namespace

int main() {
    known_plan();
    agrees_with_the_movement();
    thinning_keeps_the_tail();
    caps();
    rebuilds_only_on_events();
    clears();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " path line contract(s) failed\n";
        return 1;
    }
    std::cout << "path line contracts passed\n";
    return 0;
}
