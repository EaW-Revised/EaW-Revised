#include "eawr/sim/tactical/land_path.hpp"
#include "eawr/sim/tactical/land_placement.hpp"
#include "land_test_support.hpp"
#include "eawr/skirmish/land_terrain.hpp"
#include "../support/check.hpp"
#include "../support/env.hpp"
#include "../support/reverse_partition_executor.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <new>
#include <string>

// Allocation guard: a warmed search makes no heap allocation per slice (LP-17).
static std::atomic<bool> measure_allocations{};
static std::atomic<std::size_t> measured_allocations{};
void* operator new(const std::size_t size) {
    if (measure_allocations.load(std::memory_order_relaxed)) measured_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](const std::size_t size) { return ::operator new(size); }
// Keep the allocation guard's malloc/free pairing behind the matching delete operators.
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void operator delete(void* pointer) noexcept { std::free(pointer); }
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void operator delete[](void* pointer) noexcept { std::free(pointer); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete[](pointer); }
#endif

namespace {
using namespace eawr;
using sim::math::Fixed;
using namespace sim::tactical;
testing::Checker checks;
void expect(const bool condition, const char* message) { checks.check(condition, message, std::cerr); }
using namespace land_test;
void coordinates() {
    expect(land_cell_of(whole(0), whole(20), 10) == 0u && land_cell_of(whole(9), whole(20), 10) == 0u, "centre convention: cell 0 holds 0..9");
    expect(land_cell_of(whole(10), whole(20), 10) == 1u, "half a pitch rounds up (LR3-16)");
    expect(land_cell_of(whole(-10), whole(20), 10) == 0u && !land_cell_of(whole(-11), whole(20), 10), "below half a pitch is outside, not cell 0");
    expect(land_cell_of(whole(189), whole(20), 10) == 9u && !land_cell_of(whole(190), whole(20), 10), "last cell and past the grid");
    expect(land_cell_centre(3, whole(20)) == whole(60), "no half-cell shift in a centre");
    expect(land_footprint(whole(0), whole(20)) == LandFootprint{0, 0} && land_footprint(whole(9), whole(20)) == LandFootprint{0, 0},
        "a small radius is one cell");
    expect(land_footprint(whole(11), whole(20)) == LandFootprint{-1, 1} && land_footprint(whole(20), whole(20)) == LandFootprint{-1, 1},
        "a radius past half a pitch reaches the neighbours");
    expect(land_footprint(whole(10), whole(20)) == LandFootprint{0, 1}, "WLPA-06 equality: both corners use the point conversion");
    expect(land_footprint(whole(40), whole(20)) == LandFootprint{-2, 2}, "two cells each side");
}

void open_ground() {
    auto field = make_field({"..........", "..........", "..........", "..........", ".........."});
    const auto req = request(*field.terrain, 1, 2, 8, 2);
    LandPathStats stats;
    const auto path = plan(field, req, &stats);
    expect(path.status == LandPathStatus::found, "open ground routes");
    expect(path.nodes.size() == 2 && path.nodes.front() == req.start && path.nodes.back() == req.end, "smoothing leaves start and end");
    expect(path.length == whole(140), "straight length");
    expect(stats.expansions > 0 && stats.work >= stats.expansions, "work is counted");
    auto unsmoothed = req;
    unsmoothed.smooth = false;
    const auto cells = plan(field, unsmoothed);
    expect(cells.status == LandPathStatus::found && cells.nodes.size() == 8, "unsmoothed route keeps every cell");
    auto trivial = req;
    trivial.trivial = true;
    LandPathStats trivial_stats;
    expect(plan(field, trivial, &trivial_stats).nodes.size() == 2 && trivial_stats.work == 0, "WLPA-15: forced trivial path costs nothing");
    auto same = req;
    same.end = at(req.start.x.raw() / Fixed::scale + 5, req.start.y.raw() / Fixed::scale);
    expect(plan(field, same).nodes.size() == 2, "same cell appends start and end");
}

void obstacles_and_classes() {
    auto field = make_field({
        ".....#....",
        ".....#....",
        ".....#....",
        "..........",
        ".....#....",
        ".....#....",
    });
    const auto req = request(*field.terrain, 1, 1, 8, 1);
    const auto path = plan(field, req);
    expect(path.status == LandPathStatus::found && path.nodes.size() > 2, "a wall with one gap forces a corner");
    expect(walkable(field, path, req.mask), "no segment crosses a closed cell");
    const bool through_gap = std::any_of(path.nodes.begin(), path.nodes.end(), [](const auto& node) {
        return node.x == whole(100) || node.y == whole(60);
    });
    expect(through_gap, "the route passes the gap row");
    auto sealed = make_field({".....#....", ".....#....", ".....#....", ".....#....", ".....#...."});
    const auto blocked = request(*sealed.terrain, 1, 1, 8, 1);
    LandPathArena arena;
    LandSearch search(sealed.occupancy, arena, blocked);
    expect(search.run(1'000'000) && search.status() == LandPathStatus::no_route && search.exhausted(), "a sealed wall is no route and exhausted");
    expect(arena.pooled() == 1, "an ended search returns its scratch");
    auto closed_end = request(*sealed.terrain, 1, 1, 5, 1);
    expect(plan(sealed, closed_end).status == LandPathStatus::destination_blocked, "a closed end cell is a distinct failure");
    auto outside = request(*sealed.terrain, 1, 1, 5, 1);
    outside.start = at(-200, 0);
    expect(plan(sealed, outside).status == LandPathStatus::start_outside, "a start outside the grid");
    outside = blocked;
    outside.end = at(5000, 0);
    expect(plan(sealed, outside).status == LandPathStatus::destination_blocked, "an end outside the grid");

    auto lake = make_field({"..~~~~..", "..~~~~..", "..~~~~..", "..~~~~.."});
    expect(plan(lake, request(*lake.terrain, 0, 1, 7, 1, "walker")).status == LandPathStatus::no_route, "WLPA-01: walkers cannot cross water");
    const auto swim = request(*lake.terrain, 0, 1, 7, 1, "hover");
    const auto crossing = plan(lake, swim);
    expect(crossing.status == LandPathStatus::found && crossing.length == whole(140), "hover crosses water straight");
    auto mixed = make_field({"..i..", "..i.."});
    expect(plan(mixed, request(*mixed.terrain, 0, 0, 4, 0, "walker")).status == LandPathStatus::no_route
        && plan(mixed, request(*mixed.terrain, 0, 0, 4, 0, "infantry")).status == LandPathStatus::found,
        "infantry-only ground admits infantry alone");
    auto occupied = make_field({"...", "...", "..."});
    const std::vector<LandOccupation> occupations{{1, wall, 0, {1, 4}, {}}};
    sim::InlineExecutor inline_executor;
    expect(bool(occupied.occupancy.commit(occupations, inline_executor)), "occupation committed");
    const auto around = plan(occupied, request(*occupied.terrain, 0, 0, 2, 0));
    expect(around.status == LandPathStatus::found && around.length > whole(40), "route around an occupied column");
    expect(plan(occupied, request(*occupied.terrain, 1, 0, 2, 0)).status == LandPathStatus::found, "a start on a closed cell is exempt");
}

Field gap_field(const std::string& door) {
    std::vector<std::string> rows;
    for (int row = 0; row < 3; ++row) rows.emplace_back(9, '.');
    rows.push_back(door);
    for (int row = 0; row < 5; ++row) rows.emplace_back(9, '.');
    return make_field(rows);
}
void footprints_and_width() {
    auto field = make_field({
        ".........",
        ".#######.",
        ".........",
        ".#######.",
        ".........",
    });
    auto thin = request(*field.terrain, 0, 2, 8, 2);
    expect(plan(field, thin).status == LandPathStatus::found, "radius 0 uses the one-cell corridor");
    thin.radius = whole(20);
    expect(plan(field, thin).status != LandPathStatus::found, "WLPA-06: a 3x3 footprint does not fit one cell");

    auto wide_door = gap_field("###...###");
    auto large = request(*wide_door.terrain, 4, 1, 4, 7);
    large.radius = whole(20);
    expect(plan(wide_door, large).status == LandPathStatus::found, "a 3x3 footprint passes a gap it fits");
    auto narrow_door = gap_field("####.####");
    large = request(*narrow_door.terrain, 4, 1, 4, 7);
    expect(plan(narrow_door, large).status == LandPathStatus::found, "radius 0 passes the one-cell door");
    large.radius = whole(20);
    expect(plan(narrow_door, large).status == LandPathStatus::no_route, "a footprint wider than the door is refused");

    // Width repair (WLPA-16): the short way is a one-cell door, so a wide request is refused.
    auto crossing = request(*narrow_door.terrain, 4, 0, 4, 8);
    const auto direct = plan(narrow_door, crossing);
    expect(direct.status == LandPathStatus::found && direct.length == whole(160), "no width: straight through the door");
    crossing.width = whole(20);
    expect(plan(narrow_door, crossing).status == LandPathStatus::found, "a width of one cell fits the door");
    crossing.width = whole(60);
    LandPathStats stats;
    expect(plan(narrow_door, crossing, &stats).status == LandPathStatus::width_rejected, "WLPA-16: a failed repair rejects the route");
    expect(stats.width_cells > 0 && stats.restarts == 0, "the repair ran its width scans");
    crossing.width = whole(60);
    expect(plan(wide_door, request(*wide_door.terrain, 4, 0, 4, 8)).status == LandPathStatus::found, "the three-cell door routes");
    auto three = request(*wide_door.terrain, 4, 0, 4, 8);
    three.width = whole(60);
    expect(plan(wide_door, three).status == LandPathStatus::found, "a width of three cells fits a three-cell door");
    three.width = whole(100);
    expect(plan(wide_door, three).status == LandPathStatus::width_rejected, "five cells do not fit a three-cell door");

    // The repair takes the wider, longer door when there is one.
    auto two_doors = make_field({
        ".............",
        ".............",
        ".............",
        "######.#...##",
        ".............",
        ".............",
        ".............",
    });
    auto choose = request(*two_doors.terrain, 6, 0, 6, 6);
    const auto short_way = plan(two_doors, choose);
    expect(short_way.status == LandPathStatus::found && short_way.length == whole(120), "no width: through the one-cell door");
    choose.width = whole(60);
    const auto wide_way = plan(two_doors, choose);
    expect(wide_way.status == LandPathStatus::found && wide_way.length > short_way.length, "the repair takes the three-cell door");
    expect(walkable(two_doors, wide_way, choose.mask), "the repaired route is walkable");
    const bool through_wide = std::any_of(wide_way.nodes.begin(), wide_way.nodes.end(), [](const auto& node) {
        return node.x >= whole(160) && node.x <= whole(200) && node.y >= whole(40) && node.y <= whole(80);
    });
    expect(through_wide, "the repaired route passes the wide door");
}

void detour_limit() {
    auto field = make_field({
        "...........",
        "...........",
        "..#########",
        "...........",
        "...........",
    });
    // The wall's open end is at the left: a 140-unit direct distance, a long way round.
    auto req = request(*field.terrain, 9, 1, 9, 3);
    const auto open_route = plan(field, req);
    expect(open_route.status == LandPathStatus::found, "the plain route exists");
    req.detour_ratio = Fixed::from_raw(land_giant_detour_ratio_raw);
    const auto direct_distance = distance(req.start, req.end);
    expect(open_route.length > Fixed::from_raw(direct_distance.raw() * 18 / 10), "the fixture detours more than 1.8 times");
    expect(plan(field, req).status == LandPathStatus::detour_rejected, "LC-10: a detour past 1.8x the direct distance is rejected");
    req.detour_ratio = whole(10);
    expect(plan(field, req).status == LandPathStatus::found, "a larger ratio accepts it");
    auto clear_field = make_field({".........", ".........", "........."});
    auto direct_req = request(*clear_field.terrain, 0, 1, 8, 1);
    direct_req.detour_ratio = Fixed::from_raw(land_giant_detour_ratio_raw);
    LandPathStats stats;
    const auto direct_path = plan(clear_field, direct_req, &stats);
    expect(direct_path.status == LandPathStatus::found && stats.expansions == 0 && direct_path.nodes.size() == 2,
        "LC-10: a clear direct route needs no search");
    static_cast<void>(direct_distance);
}

LandPath run_in_slices(const Field& field, const LandPathRequest& req, const std::uint64_t slice, LandPathStats* totals = nullptr,
    bool roundtrip = false) {
    LandPathArena arena;
    LandSearch search(field.occupancy, arena, req);
    std::size_t guard = 0;
    while (!search.ended() && guard++ < 1'000'000) {
        search.run(slice);
        if (roundtrip && !search.ended()) {
            const auto saved = search.checkpoint();
            auto restored = LandSearch::from_checkpoint(field.occupancy, arena, saved);
            expect(bool(restored), "checkpoint restores");
            if (!restored) return {};
            expect(restored.value().checkpoint() == saved, "restore then checkpoint is the identity");
            search = std::move(restored).value();
        }
    }
    if (totals) *totals = search.totals();
    return search.result();
}
void slicing_and_checkpoints() {
    // A serpentine of two-row bands with two-cell doors alternating right and left.
    std::vector<std::string> rows;
    for (int band = 0; band < 4; ++band) {
        rows.emplace_back(22, '.');
        rows.emplace_back(22, '.');
        if (band == 3) break;
        auto door = std::string(22, '#');
        if (band % 2 == 0) door.replace(20, 2, "..");
        else door.replace(0, 2, "..");
        rows.push_back(door);
    }
    auto field = make_field(rows);
    auto req = request(*field.terrain, 0, 0, 0, 10);
    req.radius = whole(0);
    LandPathStats whole_stats;
    const auto reference = run_in_slices(field, req, std::numeric_limits<std::uint64_t>::max(), &whole_stats);
    expect(reference.status == LandPathStatus::found, "the maze fixture routes");
    expect(walkable(field, reference, req.mask), "the reference route is walkable");
    for (const std::uint64_t slice : {1ULL, 2ULL, 3ULL, 7ULL, 50ULL}) {
        LandPathStats stats;
        const auto sliced = run_in_slices(field, req, slice, &stats);
        expect(sliced == reference, "a sliced search equals the whole one");
        expect(stats.expansions == whole_stats.expansions && stats.evaluations == whole_stats.evaluations
            && stats.pushes == whole_stats.pushes, "slicing never changes the counted work");
        const auto restored = run_in_slices(field, req, slice, nullptr, true);
        expect(restored == reference, "a checkpoint round trip at every slice boundary equals the whole one");
    }
    auto wide = req;
    wide.width = whole(40);
    wide.smooth = true;
    const auto wide_reference = run_in_slices(field, wide, std::numeric_limits<std::uint64_t>::max());
    expect(run_in_slices(field, wide, 5, nullptr, true) == wide_reference, "the width repair survives checkpoints");
    auto giant = req;
    giant.detour_ratio = whole(100);
    expect(run_in_slices(field, giant, 4, nullptr, true) == run_in_slices(field, giant, std::numeric_limits<std::uint64_t>::max()),
        "the direct and detour phases survive checkpoints");
    // A budget of zero makes no progress; a search that has not ended reports it.
    LandPathArena arena;
    LandSearch idle(field.occupancy, arena, req);
    expect(!idle.run(0) && idle.expansions() == 0, "a zero budget does no work");
    // A damaged checkpoint is refused.
    auto saved = idle.checkpoint();
    saved.open.push_back({0, 0, 1u << 30});
    expect(!LandSearch::from_checkpoint(field.occupancy, arena, saved), "an entry outside the grid is refused");
    saved = idle.checkpoint();
    saved.phase = 99;
    expect(!LandSearch::from_checkpoint(field.occupancy, arena, saved), "an unknown phase is refused");
}

void grid_changes_during_a_search() {
    auto field = make_field({
        "...............",
        "...............",
        "...............",
        "...............",
        "...............",
        "...............",
    });
    auto req = request(*field.terrain, 0, 2, 14, 2);
    req.smooth = true;
    LandPathArena arena;
    LandSearch search(field.occupancy, arena, req);
    sim::InlineExecutor executor;
    bool blocked = false;
    for (int guard = 0; !search.ended() && guard < 100000; ++guard) {
        search.run(1);
        const auto saved = search.checkpoint();
        if (!blocked && saved.phase >= 3 && saved.cells.size() > 4) {
            // Smoothing has begun on a finished route: close a cell in the middle of it.
            const auto cell = saved.cells[saved.cells.size() / 2];
            const std::vector<LandOccupation> wall_cells{{9, wall, 0, {cell}, {}}};
            expect(bool(field.occupancy.commit(wall_cells, executor)), "obstacle committed mid search");
            blocked = true;
        }
    }
    expect(blocked, "the route finished before smoothing began");
    const auto path = search.result();
    expect(path.status == LandPathStatus::found && walkable(field, path, req.mask), "the finished route is valid on the changed grid");
    expect(search.totals().restarts >= 1, "a route the grid invalidated is searched again");
    expect(field.occupancy.revision() >= 1, "the occupancy counts changing commits");
}

// A grid change committed after the last slice has not been seen by run() yet; a checkpoint taken then must still
// carry it, because the revision counter does not survive a save (LP-14).
void grid_change_before_a_checkpoint() {
    const auto finish = [](const bool restore, LandPathStats* totals) {
        auto field = make_field({
            "...............",
            "...............",
            "...............",
            "...............",
            "...............",
            "...............",
        });
        auto req = request(*field.terrain, 0, 2, 14, 2);
        req.smooth = true;
        LandPathArena arena;
        LandSearch search(field.occupancy, arena, req);
        sim::InlineExecutor executor;
        bool blocked = false;
        for (int guard = 0; !search.ended() && guard < 100000; ++guard) {
            if (!blocked) {
                search.run(1);
                const auto saved = search.checkpoint();
                if (saved.phase >= 3 && saved.cells.size() > 4) {
                    const auto cell = saved.cells[saved.cells.size() / 2];
                    const std::vector<LandOccupation> wall_cells{{9, wall, 0, {cell}, {}}};
                    expect(bool(field.occupancy.commit(wall_cells, executor)), "obstacle committed after the last slice");
                    blocked = true;
                    if (restore) {
                        auto restored = LandSearch::from_checkpoint(field.occupancy, arena, search.checkpoint());
                        expect(bool(restored), "a checkpoint taken after a grid change restores");
                        if (!restored) return LandPath{};
                        search = std::move(restored).value();
                    }
                }
            } else {
                search.run(1);
            }
        }
        expect(blocked, "the route finished before smoothing began (checkpoint fixture)");
        if (totals) *totals = search.totals();
        const auto path = search.result();
        expect(path.status == LandPathStatus::found && walkable(field, path, req.mask), "the route is valid on the changed grid");
        return path;
    };
    LandPathStats straight, restored;
    const auto reference = finish(false, &straight);
    const auto path = finish(true, &restored);
    expect(straight.restarts >= 1 && restored.restarts >= 1, "a route the grid invalidated is searched again after a restore");
    expect(path == reference && restored.restarts == straight.restarts, "a restore across a grid change equals the straight run");
}

// The scheduler admits the longest-waiting search first, so the wait counter must survive a checkpoint (LP-17).
void waiting_survives_a_checkpoint() {
    auto field = make_field({
        "..........................",
        ".########################.",
        "..........................",
        ".########################.",
        "..........................",
    });
    LandPathArena arena;
    auto first = request(*field.terrain, 0, 0, 25, 4);
    first.entity = 1;
    auto second = first;
    second.entity = 2;
    LandSearch early(field.occupancy, arena, first);
    LandSearch late(field.occupancy, arena, second);
    for (int i = 0; i < 3; ++i) late.defer();
    const auto saved = late.checkpoint();
    expect(saved.waited == 3, "a checkpoint carries the wait counter");
    auto restored = LandSearch::from_checkpoint(field.occupancy, arena, saved);
    expect(bool(restored), "a waiting search restores");
    if (!restored) return;
    expect(restored.value().waited() == 3 && restored.value().checkpoint() == saved, "the restore keeps the wait counter");
    sim::InlineExecutor executor;
    LandSliceScheduler scheduler;
    LandPathAdmission admission(1, 1);  // one admitted slice per frame
    std::vector<LandSearch*> searches{&early, &restored.value()};
    const auto report = scheduler.run(executor, searches, admission, 3);
    expect(bool(report) && report.value().admitted == 1 && report.value().denied == 1, "one search is admitted");
    expect(restored.value().expansions() > 0 && early.expansions() == 0 && early.waited() == 1,
        "the restored search that waited longest is admitted first, as the uninterrupted one would be");
}

struct Job {
    LandPathRequest request;
    std::unique_ptr<LandSearch> search;
};
std::vector<std::string> scheduled(const Field& field, const sim::PartitionExecutor& executor, std::uint64_t quota, std::uint64_t slice,
    std::vector<LandSliceReport>* reports = nullptr) {
    LandPathArena arena;
    std::vector<Job> jobs;
    for (sim::EntityId id = 1; id <= 12; ++id) {
        Job job;
        job.request = request(*field.terrain, 0, static_cast<std::int64_t>(id % 5), 15, static_cast<std::int64_t>((id * 3) % 7));
        job.request.entity = id;
        job.search = std::make_unique<LandSearch>(field.occupancy, arena, job.request);
        jobs.push_back(std::move(job));
    }
    LandPathAdmission admission(quota, 1);
    LandSliceScheduler scheduler;
    std::vector<LandSearch*> pointers;
    for (auto& job : jobs) pointers.push_back(job.search.get());
    // Presented in reverse: the scheduler's order must not depend on the caller's.
    std::reverse(pointers.begin(), pointers.end());
    for (std::uint64_t frame = 0; frame < 4000; ++frame) {
        admission.begin_frame(frame);
        auto report = scheduler.run(executor, pointers, admission, slice);
        expect(bool(report), "slices ran");
        if (!report) return {};
        if (reports) reports->push_back(report.value());
        if (std::all_of(jobs.begin(), jobs.end(), [](const Job& job) { return job.search->ended(); })) break;
    }
    std::vector<std::string> digest;
    for (auto& job : jobs) {
        const auto path = job.search->result();
        std::string row = std::to_string(job.request.entity) + ":" + std::to_string(static_cast<int>(path.status)) + ":"
            + std::to_string(path.length.raw()) + ":" + std::to_string(job.search->expansions());
        for (const auto& node : path.nodes) row += "," + std::to_string(node.x.raw()) + "/" + std::to_string(node.y.raw());
        digest.push_back(std::move(row));
    }
    return digest;
}
void admission_and_scheduling() {
    LandPathAdmission zero(0, 1);
    expect(!zero.admits(), "a zero quota admits nothing (LR3-01)");
    LandPathAdmission quota(100, 5);
    expect(quota.admits(), "below the quota admits");
    quota.charge(99);
    expect(quota.admits(), "one below the quota admits");
    quota.charge(1);
    expect(!quota.admits(), "equality refuses admission");
    quota.charge(500);
    expect(!quota.admits() && quota.spent() == 600, "a charge can overshoot");
    quota.begin_frame(7);
    expect(!quota.admits(), "LR3-06: not a multiple of the interval keeps the charge");
    quota.begin_frame(10);
    expect(quota.admits() && quota.spent() == 0, "a multiple of the interval clears it");
    LandPathAdmission every(10, 1);
    every.charge(10);
    every.begin_frame(3);
    expect(every.admits(), "an interval below 2 clears every frame");

    auto field = make_field({
        "......................",
        "..######..####..######",
        "......................",
        "..####..######..######",
        "......................",
        "..######..####..######",
        "......................",
        "..####..######..######",
    });
    sim::InlineExecutor inline_executor;
    std::vector<LandSliceReport> reports;
    const auto reference = scheduled(field, inline_executor, 400, 40, &reports);
    expect(reference.size() == 12, "every search finished under the throttle");
    expect(std::any_of(reports.begin(), reports.end(), [](const auto& r) { return r.denied > 0; }), "the quota denied some slices");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        expect(scheduled(field, executor, 400, 40) == reference, "throttled slices equal on 1/2/4/8 workers");
    }
    test_support::ReverseOrderExecutor reverse(8);
    expect(scheduled(field, reverse, 400, 40) == reference, "throttled slices equal in reverse partition order");
    expect(!reverse.phases.empty() && reverse.phases.front().name == "land-path-slices" && reverse.phases.front().descending(),
        "slices run in the partitioned land-path-slices phase");
    // The throttle changes when searches finish, never what they find or how much work they count.
    expect(scheduled(field, inline_executor, 1'000'000'000, 1'000'000'000) == reference, "the throttle never changes a result");
}

void allocations() {
    auto field = make_field({
        "..........................",
        ".########################.",
        "..........................",
        ".########################.",
        "..........................",
    });
    LandPathArena arena;
    auto req = request(*field.terrain, 0, 0, 25, 4);
    for (int i = 0; i < 3; ++i) {  // warm the pool, the heap and the path vectors
        LandSearch warm(field.occupancy, arena, req);
        warm.run(~0ULL);
    }
    LandSearch search(field.occupancy, arena, req);
    measured_allocations = 0;
    measure_allocations = true;
    std::size_t slices = 0;
    while (!search.run(8)) ++slices;
    measure_allocations = false;
    expect(slices > 5, "the allocation fixture is sliced");
    expect(measured_allocations <= 1, "a warmed search allocates only its result nodes, not per slice");
    expect(arena.pooled() >= 1 && arena.pooled_bytes() > 0, "scratch is pooled");
}

void service_contract() {
    // The shared path service drives the land backend through every entry point the space lattice has.
    auto field = make_field({"........", "..####..", "........", "........"});
    LandPathArena arena;
    const auto req = request(*field.terrain, 0, 3, 7, 0);
    auto search = LandPathService::begin(field.occupancy, arena, req);
    LandPathStats stats;
    std::size_t guard = 0;
    while (!LandPathService::ended(search) && guard++ < 10000) LandPathService::step(search, 3, &stats);
    const auto path = LandPathService::result(search);
    expect(path.status == LandPathStatus::found && !LandPathService::exhausted(search), "the service runs a search to its end");
    expect(LandPathService::expansions(search) == stats.work && stats.work > 0, "the service reports the work of every slice");
    expect(LandPathService::held_bytes(search) > 0, "a search reports the memory it holds");
    auto other = LandPathService::begin(field.occupancy, arena, req);
    LandPathService::step(other, 2);
    auto restored = LandPathService::restore(field.occupancy, arena, LandPathService::checkpoint(other));
    expect(bool(restored), "the service restores a checkpoint");
    if (restored) {
        LandPathService::step(restored.value(), ~0ULL);
        expect(LandPathService::result(restored.value()) == path, "a restored search finishes with the same route");
    }
    expect(LandPathService::plan(field.occupancy, arena, req) == path, "plan is the whole search");
    expect(!LandPathService::plan_within(field.occupancy, arena, req, 1), "plan_within gives up at its budget");
    expect(LandPathService::plan_within(field.occupancy, arena, req, ~0ULL) == path, "plan_within with room is the whole search");
    expect(LandPathService::slice_limit(5, 9, 40) == 40 && LandPathService::slice_limit(9, 9, 40) == ~0ULL, "the landing frame runs a search to its end");
}

void terrain_masks() {
    auto field = make_field({"...", "..."});
    expect(field.terrain->movement_mask("hover") == hover && field.terrain->movement_mask("walker") == walker
        && !field.terrain->movement_mask("tracked"), "movement masks resolve by their data names");
}

// --- stock land maps (installed) --------------------------------------------------------------------

// The oracle: breadth-first reachability under the same cell rules, independent of the search.
bool reachable(const LandOccupancy& occupancy, const std::uint32_t mask, const std::uint32_t columns, const std::uint32_t rows,
    const std::uint32_t from, const std::uint32_t to, std::vector<std::uint32_t>& queue, std::vector<std::uint8_t>& seen) {
    const auto open = [&](const std::int64_t x, const std::int64_t y) {
        return x >= 0 && y >= 0 && x < columns && y < rows && occupancy.admits(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), mask);
    };
    std::fill(seen.begin(), seen.end(), std::uint8_t{0});
    queue.assign(1, from);
    seen[from] = 1;
    constexpr int dx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::int64_t x = queue[head] % columns, y = queue[head] / columns;
        if (queue[head] == to) return true;
        for (int d = 0; d < 8; ++d) {
            const auto nx = x + dx[d], ny = y + dy[d];
            if (!open(nx, ny)) continue;
            if ((d & 1) && !(open(x + dx[d], y) && open(x, y + dy[d]))) continue;
            const auto cell = static_cast<std::uint32_t>(ny * columns + nx);
            if (seen[cell]) continue;
            seen[cell] = 1;
            queue.push_back(cell);
        }
    }
    return false;
}

int installed() {
    const auto root = testing::environment("EAWR_EAW_GAME_ROOT");
    if (!root || root->empty()) { std::cout << "SKIPPED: no game data\n"; return 77; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto& folder : {"corruption", "GameData"}) {
        auto manifest = vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data");
        if (!manifest) { std::cerr << manifest.error().message << '\n'; return 1; }
        mounts.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = vfs::Vfs::mount(mounts);
    if (!filesystem) return 1;
    auto records = filesystem.value().enumerate("data/art/maps/", ".ted");
    if (!records) return 1;
    std::size_t maps = 0, searches = 0, routed = 0;
    for (const auto& record : records.value()) {
        auto map = assets::load_map(filesystem.value(), record.canonical_path);
        if (!map) { std::cerr << record.canonical_path << ": map load failed\n"; return 1; }
        if (map.value().kind != assets::MapKind::land) continue;
        // The movement classes are content: the bit assignments are not pinned (R2/R3), so the stock-map
        // cases derive "open ground" from the map itself: the cell values that cover most of the grid.
        std::map<std::uint32_t, std::size_t> histogram;
        const auto& raw = map.value().raw_passability;
        for (std::size_t i = 0; i + 3 < raw.size(); i += 4) {
            std::uint32_t bits = 0;
            for (unsigned b = 0; b < 4; ++b) bits |= std::to_integer<std::uint32_t>(raw[i + b]) << (8U * b);
            ++histogram[bits];
        }
        const auto cells = raw.size() / 4;
        std::uint32_t open_bits = 0;
        for (const auto& [bits, count] : histogram) if (count * 50 >= cells) open_bits |= bits;
        auto terrain_result = skirmish::load_land_terrain(map.value(), {{"open_ground", open_bits}});
        if (!terrain_result) { std::cerr << record.canonical_path << ": " << terrain_result.error().message << '\n'; return 1; }
        auto terrain = std::make_shared<const LandTerrain>(std::move(terrain_result).value());
        // The occupancy needs one Clear bit; any single bit of the open ground works for these maps.
        const std::uint32_t clear_bit = open_bits & (~open_bits + 1);
        if (clear_bit == 0) continue;
        auto occupancy_result = LandOccupancy::create(terrain, clear_bit);
        if (!occupancy_result) continue;
        auto field = Field{terrain, std::move(occupancy_result).value(), terrain->width(), terrain->height()};
        const auto mask = *terrain->movement_mask("open_ground");
        std::vector<std::uint32_t> open_cells;
        for (std::uint32_t cell = 0; cell < field.occupancy.cells().size(); ++cell) {
            if ((field.occupancy.cells()[cell] & ~mask) == 0) open_cells.push_back(cell);
        }
        if (open_cells.size() < 100) continue;
        ++maps;
        std::uint64_t state = 0x9E3779B97F4A7C15ULL ^ open_cells.size();
        const auto next = [&] { state = state * 6364136223846793005ULL + 1442695040888963407ULL; return state >> 33; };
        std::vector<std::uint32_t> queue;
        std::vector<std::uint8_t> seen(terrain->passability().size());
        LandPathArena arena;
        const auto zones = LandZones::build(field.occupancy, mask);
        for (int pair = 0; pair < 6; ++pair) {
            const auto from = open_cells[next() % open_cells.size()], to = open_cells[next() % open_cells.size()];
            LandPathRequest req;
            req.entity = static_cast<sim::EntityId>(pair + 1);
            req.mask = mask;
            req.start = {land_cell_centre(from % terrain->width(), terrain->spacing()), land_cell_centre(from / terrain->width(), terrain->spacing())};
            req.end = {land_cell_centre(to % terrain->width(), terrain->spacing()), land_cell_centre(to / terrain->width(), terrain->spacing())};
            const auto expected = reachable(field.occupancy, mask, terrain->width(), terrain->height(), from, to, queue, seen);
            const auto whole_path = LandPathBackend::plan(field.occupancy, arena, req);
            ++searches;
            expect(expected == (whole_path.status == LandPathStatus::found), "the search finds a route exactly when one exists");
            expect(zones.connected(from, to) == expected, "the connected zones agree with the oracle (WLPA-13)");
            if (whole_path.status == LandPathStatus::found) {
                ++routed;
                expect(walkable(field, whole_path, mask), "a stock-map route stays on open ground");
            }
            expect(run_in_slices(field, req, 400) == whole_path, "stock-map slicing equals the whole search");
        }
    }
    expect(maps != 0 && routed != 0, "stock land maps were searched");
    std::cout << "land maps searched: " << maps << ", searches: " << searches << ", routed: " << routed << '\n';
    return checks.finish();
}
} // namespace

int main(const int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
    coordinates(); terrain_masks(); service_contract(); open_ground(); obstacles_and_classes(); footprints_and_width(); detour_limit();
    slicing_and_checkpoints(); grid_changes_during_a_search(); grid_change_before_a_checkpoint(); waiting_survives_a_checkpoint(); admission_and_scheduling(); allocations();
    return checks.finish();
}
