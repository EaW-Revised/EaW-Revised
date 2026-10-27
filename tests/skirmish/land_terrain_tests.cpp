#include "eawr/skirmish/land_terrain.hpp"
#include "../support/check.hpp"
#include "../support/env.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>
#include <filesystem>
#include <iostream>
#include <atomic>
#include <cstdlib>
#include <new>

// Regression guard for warmed occupancy commits, including executor callbacks.
static std::atomic<bool> measure_allocations{};
static std::atomic<std::size_t> measured_allocations{};
void* operator new(const std::size_t size) {
    if (measure_allocations.load(std::memory_order_relaxed)) measured_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](const std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
#endif

namespace {
using namespace eawr;
using sim::math::Fixed;
using namespace sim::tactical;
testing::Checker checks;
void expect(const bool condition, const char* message) { checks.check(condition, message, std::cerr); }
Fixed whole(const std::int64_t value) { return Fixed::from_integer(value).value(); }
assets::RawField field(const std::uint32_t id, const std::uint32_t value) {
    assets::RawField result; result.id = id;
    for (unsigned i = 0; i < 4; ++i) result.bytes.push_back(static_cast<std::byte>(value >> (8U * i)));
    return result;
}
assets::Map fixture() {
    assets::Map map; map.kind = assets::MapKind::land;
    map.terrain.emplace(); auto& terrain = *map.terrain;
    terrain.width = terrain.height = 3;
    assets::TerrainMaterial material; material.fields.push_back(field(0x14, 12));
    terrain.materials.push_back(material);
    for (std::int16_t i = 0; i < 9; ++i) {
        terrain.samples.push_back({static_cast<std::int16_t>(i * 512), 0, 255});
        for (const auto byte : field(0, i == 4 ? 0x80000004U : 1U).bytes) map.raw_passability.push_back(byte);
    }
    return map;
}
void contracts() {
    auto map = fixture();
    auto loaded = skirmish::load_land_terrain(map, {{"custom", 0x80000004U}});
    expect(bool(loaded), "synthetic terrain loads"); if (!loaded) return;
    const auto& terrain = loaded.value();
    expect(terrain.height_at(whole(10), whole(10)) == whole(50), "integer bilinear sample");
    expect(terrain.height_at(whole(40), whole(40)) == whole(200), "last vertex is sampleable");
    expect(!terrain.height_at(whole(-1), whole(0)) && !terrain.height_at(whole(41), whole(0)), "height outside fails");
    expect(terrain.surface_at(whole(0), whole(0)) == 12, "material surface decoded");
    expect(terrain.passability()[4] == 0x80000004U, "high/custom passability bits survive little-endian decode");
    expect(!terrain.admits(1, 1, 4) && terrain.admits(1, 1, 0x80000004U), "all required bits must be admitted");
    expect(terrain.admits(0, 0, 1) && !terrain.admits_footprint(0, 0, 1, 1, 1), "footprint tests all cells");
    expect(!terrain.admits_footprint(1, 0, 0, 1, ~0U) && !terrain.admits(3, 0, ~0U), "bad rectangle and cell fail");
    expect(!terrain.playable_centre(whole(1), whole(1)).has_value(), "unknown map alignment remains unsupported");
    expect(terrain.segment_intersection({}, {}).status == LandSegmentStatus::clear,
        "a zero-length segment on flat ground at the origin is clear");
    auto alternate = map;
    alternate.terrain->materials[0].fields[0] = field(0x14, 99);
    auto changed = skirmish::load_land_terrain(alternate);
    expect(changed && changed.value().identity() != terrain.identity(), "surface changes identity");
    alternate = map; alternate.raw_passability[0] = std::byte{2};
    changed = skirmish::load_land_terrain(alternate);
    auto plain = skirmish::load_land_terrain(map);
    expect(changed && plain && changed.value().identity() != plain.value().identity(), "passability changes identity");
    auto masks = skirmish::load_land_terrain(map, {{"z", 8}, {"a", 3}});
    auto reverse_masks = skirmish::load_land_terrain(map, {{"a", 3}, {"z", 8}});
    expect(masks && reverse_masks && masks.value().identity() == reverse_masks.value().identity(), "mask order canonicalized");
    expect(!skirmish::load_land_terrain(map, {{"a", 1}, {"a", 2}}), "duplicate movement class rejected");
    const LandBounds large{whole(-1000), whole(-1000), whole(1000), whole(1000)};
    auto bounded = skirmish::load_land_terrain(map, {}, large);
    expect(bounded && bounded.value().playable_bounds() == LandBounds{whole(-600), whole(-600), whole(600), whole(600)}, "WLEN-39 inset");
    expect(bounded && bounded.value().playable_centre(whole(-600), whole(0)) == false
        && bounded.value().playable_centre(whole(-599), whole(0)) == true, "WLPA-05 boundary equality fails");
    const LandBounds small{whole(0), whole(0), whole(800), whole(900)};
    bounded = skirmish::load_land_terrain(map, {}, small);
    expect(bounded && bounded.value().playable_bounds() == small, "empty inset uses whole bounds");
    map.water_records.push_back({"1/257/0", 0, {field(0x16, 1), field(0x15, std::bit_cast<std::uint32_t>(0.5F))}});
    auto wet = skirmish::load_land_terrain(map);
    expect(wet && wet.value().surface_at({}, {}) == 12, "water threshold strict at equality");
    map.water_records[0].fields[1] = field(0x15, std::bit_cast<std::uint32_t>(1.0F));
    wet = skirmish::load_land_terrain(map);
    expect(wet && wet.value().surface_at({}, {}) == 4, "submerged terrain uses water surface");
    for (const auto mode : {2U, 3U}) {
        map.water_records[0].fields[0] = field(0x16, mode);
        wet = skirmish::load_land_terrain(map);
        expect(wet && wet.value().surface_at({}, {}) == (mode == 2 ? 8 : 7), "lava and ice plane surfaces");
    }
    TacticalSetup setup; TacticalContent content;
    const auto original = setup.content_identity;
    expect(bool(skirmish::bind_land_terrain(content, setup, map)) && setup.content_identity == original
        && !content.land_terrain, "space content identity untouched");
    setup.domain = content.domain = BattleDomain::land;
    expect(bool(skirmish::bind_land_terrain(content, setup, map)) && setup.content_identity != original
        && content.land_terrain, "land content binds immutable field and identity");
    expect(!skirmish::bind_land_terrain(content, setup, map), "double binding rejected");
    map.raw_passability.pop_back();
    expect(!skirmish::load_land_terrain(map), "short passability rejected");
    map = fixture(); map.terrain->samples[0].material_slot = 99;
    expect(!skirmish::load_land_terrain(map), "invalid material slot rejected");
    map = fixture();
    assets::TerrainMaterial other; other.fields.push_back(field(0x14, 99));
    map.terrain->materials.push_back(other); map.terrain->samples[1].material_slot = 1;
    const auto nearest = skirmish::load_land_terrain(map);
    expect(nearest && nearest.value().surface_at(whole(9), {}) == 12
        && nearest.value().surface_at(whole(11), {}) == 99, "surface selects nearest vertex instead of bilinear material");
}
// ---- LandTerrain::segment_intersection (LR2-15/16, LR2-C8) ----
double real(const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(Fixed::scale); }
sim::math::Vec3 vec(const std::int64_t x, const std::int64_t y, const std::int64_t z) { return {whole(x), whole(y), whole(z)}; }
// Spacing 10, surface id of vertex i is 100 + i.
LandTerrain make_terrain(const std::uint32_t width, const std::uint32_t height,
    const std::vector<std::int64_t>& heights, const std::optional<LandWaterPlane> water = std::nullopt) {
    LandTerrainInput input;
    input.width = width; input.height = height; input.spacing = whole(10); input.water = water;
    for (std::size_t i = 0; i < heights.size(); ++i) {
        input.heights.push_back(whole(heights[i]));
        input.surfaces.push_back(static_cast<std::int32_t>(100 + i));
    }
    input.passability.assign(heights.size(), 1);
    auto built = LandTerrain::build(std::move(input));
    expect(bool(built), "segment test terrain builds");
    return std::move(built).value();
}
// Row-major, row 0 first. Cell (0,0) is noncoplanar: h00=0 h10=10 h01=0 h11=20, so with s=10
//   triangle (00,10,11), u>=v: h = u + v        triangle (00,11,01), u<v: h = 2u
// and the bilinear sampler gives 7.5 at the centre where both triangles give 10.
// Cell (1,0): h00=10 h10=10 h01=20 h11=10, so u>=v: h = 10 and u<v: h = 10 - u + v.
const std::vector<std::int64_t> cell_heights{0, 10, 10,  0, 20, 10,  0, 0, 0};
bool close_to(const double a, const double b, const double tolerance = 2e-5) { return std::abs(a - b) <= tolerance; }
void expect_hit(const LandSegmentResult& result, const LandContactSource source, const double fraction,
    const double x, const double y, const double z, const std::int32_t surface, const char* what) {
    const bool ok = result.status == LandSegmentStatus::hit && result.source == source && result.point
        && close_to(real(result.fraction), fraction) && close_to(real(result.point->x), x) && close_to(real(result.point->y), y)
        && close_to(real(result.point->z), z) && result.surface == surface;
    expect(ok, what);
}
void expect_normal(const LandSegmentResult& result, const double x, const double y, const double z, const char* what) {
    const double length = std::sqrt(x * x + y * y + z * z);
    expect(close_to(real(result.normal.x), x / length) && close_to(real(result.normal.y), y / length)
        && close_to(real(result.normal.z), z / length), what);
}
void segment_contracts() {
    const auto terrain = make_terrain(3, 3, cell_heights);
    const auto T = LandContactSource::terrain;
    expect(terrain.height_at(whole(5), whole(5)) == Fixed::from_ratio(15, 2).value(),
        "bilinear sampler stays separate: 7.5 at the cell centre");
    // Both triangles, worked out by hand (see cell_heights).
    auto hit = terrain.segment_intersection(vec(7, 2, 17), vec(7, 2, 1));
    expect_hit(hit, T, 0.5, 7, 2, 9, 101, "triangle (00,10,11): u+v = 9 at (7,2)");
    expect_normal(hit, -1, -1, 1, "triangle (00,10,11) normal");
    hit = terrain.segment_intersection(vec(2, 7, 12), vec(2, 7, -4));
    expect_hit(hit, T, 0.5, 2, 7, 4, 103, "triangle (00,11,01): 2u = 4 at (2,7)");
    expect_normal(hit, -2, 0, 1, "triangle (00,11,01) normal");
    hit = terrain.segment_intersection(vec(5, 5, 14), vec(5, 5, 6));
    expect_hit(hit, T, 0.5, 5, 5, 10, 104, "triangle contact (10) differs from the bilinear sample (7.5)");
    hit = terrain.segment_intersection(vec(12, 7, 31), vec(12, 7, -1));
    expect_hit(hit, T, 0.5, 12, 7, 15, 104, "cell (1,0) triangle (00,11,01): 10 - u + v = 15");
    expect_normal(hit, 1, -1, 1, "cell (1,0) triangle (00,11,01) normal");
    // Across two cells: touches the surface at (10,2,12) without crossing (clear there), the diagonal
    // of cell (0,0) is crossed at x = 2, and the shot finally meets the flat part at z = 10, x = 12.5.
    hit = terrain.segment_intersection(vec(0, 2, 20), vec(20, 2, 4));
    expect_hit(hit, T, 0.625, 12.5, 2, 10, 101, "multi-cell segment: touch is no contact, flat cell (1,0) is hit");
    expect_normal(hit, 0, 0, 1, "flat triangle normal");
    // Start below the surface: contact at fraction 0 (LR2-U1: documented, not measured).
    hit = terrain.segment_intersection(vec(7, 2, 3), vec(7, 2, 20));
    expect_hit(hit, T, 0, 7, 2, 3, 101, "start below the surface is a contact at the start");
    hit = terrain.segment_intersection(vec(7, 2, 3), vec(7, 2, 3));
    expect_hit(hit, T, 0, 7, 2, 3, 101, "zero-length below the surface hits at fraction 0");
    expect(terrain.segment_intersection(vec(7, 2, 12), vec(7, 2, 12)).status == LandSegmentStatus::clear,
        "zero-length above the surface is clear");
    // LR2-U1 constants: touching is not contact, along a flat run or at the end point.
    expect(terrain.segment_intersection(vec(0, 0, 10), vec(20, 0, 10)).status == LandSegmentStatus::clear,
        "tangent: a segment grazing the surface along y=0 is clear");
    expect(terrain.segment_intersection(vec(7, 2, 17), vec(7, 2, 9)).status == LandSegmentStatus::clear,
        "ending exactly on the surface is clear");
    expect(terrain.segment_intersection(vec(7, 2, 9), vec(7, 2, 17)).status == LandSegmentStatus::clear,
        "starting exactly on the surface and leaving it is clear");
    // The grid ends at 20: nothing outside collides.
    expect(terrain.segment_intersection(vec(50, 50, -5), vec(60, 60, -5)).status == LandSegmentStatus::clear,
        "a segment outside the grid is clear");
    expect(terrain.segment_intersection(vec(-100, 2, 50), vec(100, 2, 50)).status == LandSegmentStatus::clear,
        "a segment crossing the grid high above is clear");
    // Overflow is reported, never read as clear.
    const auto huge = sim::math::Vec3{Fixed::from_raw(std::numeric_limits<std::int64_t>::max()), {}, {}};
    const auto tiny = sim::math::Vec3{Fixed::from_raw(std::numeric_limits<std::int64_t>::min()), {}, {}};
    expect(terrain.segment_intersection(tiny, huge).status == LandSegmentStatus::overflow, "overflow is explicit");

    // Water plane above the cell (LR2-16): the segment meets it before the terrain.
    const auto wet = make_terrain(3, 3, cell_heights, LandWaterPlane{whole(24), 1});
    const auto W = LandContactSource::water_plane;
    hit = wet.segment_intersection(vec(7, 2, 40), vec(7, 2, -24));
    expect_hit(hit, W, 0.25, 7, 2, 24, 4, "water plane above the cell is hit first, plane surface type");
    expect_normal(hit, 0, 0, 1, "water plane normal is up");
    hit = wet.segment_intersection(vec(7, 2, 40), vec(7, 2, -24), LandSegmentOptions{false});
    expect_hit(hit, T, 31.0 / 64.0, 7, 2, 9, 101, "water not admitted: the terrain is hit");
    hit = wet.segment_intersection(vec(7, 2, 20), vec(7, 2, -4));
    expect_hit(hit, T, 11.0 / 24.0, 7, 2, 9, 101, "a segment starting under the plane meets the terrain only");
    const auto shallow = make_terrain(3, 3, cell_heights, LandWaterPlane{whole(5), 2});
    hit = shallow.segment_intersection(vec(7, 2, 40), vec(7, 2, -24));
    expect_hit(hit, T, 31.0 / 64.0, 7, 2, 9, 101, "plane below the terrain contact: the terrain comes first");
    const auto level = make_terrain(3, 3, cell_heights, LandWaterPlane{whole(9), 3});
    hit = level.segment_intersection(vec(7, 2, 17), vec(7, 2, 1));
    expect_hit(hit, T, 0.5, 7, 2, 9, 101, "terrain wins an exact tie with the plane (LR2-U2 placeholder)");
}
// An independent height oracle for the property check; heights are small integers, spacing 10.
double mesh_height(const std::vector<std::int64_t>& heights, const std::uint32_t width, const std::uint32_t rows,
    const double x, const double y) {
    const auto column = std::min(static_cast<std::uint32_t>(x / 10.0), width - 2);
    const auto row = std::min(static_cast<std::uint32_t>(y / 10.0), rows - 2);
    const auto h = [&](const std::uint32_t c, const std::uint32_t r) { return static_cast<double>(heights[r * width + c]); };
    const double u = x - column * 10.0, v = y - row * 10.0;
    const double h00 = h(column, row), h10 = h(column + 1, row), h01 = h(column, row + 1), h11 = h(column + 1, row + 1);
    return u >= v ? h00 + (u * (h10 - h00) + v * (h11 - h10)) / 10.0 : h00 + (u * (h11 - h01) + v * (h01 - h00)) / 10.0;
}
std::vector<std::pair<sim::math::Vec3, sim::math::Vec3>> random_segments(const std::size_t count) {
    std::uint64_t state = 0x2545F4914F6CDD1DULL;
    const auto next = [&](const std::int64_t span) { // eighths in [-span, span)
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::int64_t>((state >> 33) % static_cast<std::uint64_t>(2 * span)) - span;
    };
    const auto point = [&] {
        return sim::math::Vec3{Fixed::from_ratio(next(40 * 8) + 25 * 8, 8).value(),
            Fixed::from_ratio(next(40 * 8) + 25 * 8, 8).value(), Fixed::from_ratio(next(30 * 8) + 20 * 8, 8).value()};
    };
    std::vector<std::pair<sim::math::Vec3, sim::math::Vec3>> segments;
    for (std::size_t i = 0; i < count; ++i) { auto a = point(); auto b = point(); segments.emplace_back(a, b); }
    return segments;
}
const std::vector<std::int64_t> hill_heights{
    0, 12, 25, 31, 18, 5,   9, 30, 14, 3, 22, 16,   27, 2, 33, 20, 8, 11,
    15, 24, 6, 29, 13, 35,  4, 19, 28, 10, 32, 7,   23, 1, 17, 26, 21, 34};
void segment_property_check() {
    const auto terrain = make_terrain(6, 6, hill_heights);
    const double extent = 50.0;
    std::size_t hits = 0, clears = 0;
    const auto segments = random_segments(400);
    for (const auto& segment : segments) {
        const auto a = segment.first, b = segment.second;
        const auto result = terrain.segment_intersection(a, b);
        if (result.status == LandSegmentStatus::overflow) { expect(false, "random segment overflowed"); continue; }
        const auto at = [&](const double t) {
            return std::array<double, 3>{real(a.x) + t * (real(b.x) - real(a.x)), real(a.y) + t * (real(b.y) - real(a.y)),
                real(a.z) + t * (real(b.z) - real(a.z))};
        };
        const auto inside = [&](const std::array<double, 3>& p) { return p[0] >= 0 && p[0] <= extent && p[1] >= 0 && p[1] <= extent; };
        const bool is_hit = result.status == LandSegmentStatus::hit;
        const double stop = is_hit ? real(result.fraction) : 1.0;
        // Nothing before the reported contact (or anywhere, when clear) may lie under the mesh.
        for (int i = 0; i <= 64; ++i) {
            if (is_hit && (i == 64 || stop == 0.0)) break; // a start-below hit has nothing before it
            const auto p = at(stop * i / 64.0);
            if (inside(p)) expect(p[2] >= mesh_height(hill_heights, 6, 6, p[0], p[1]) - 0.02, "no sampled point before the contact is under the mesh");
        }
        if (is_hit) {
            ++hits;
            const auto p = at(stop);
            const double h = mesh_height(hill_heights, 6, 6, p[0], p[1]);
            // Otherwise the contact is the start of the segment or of the grid, already below the surface.
            const bool at_start = p[2] < h && (stop == 0.0 || !inside(at(std::max(0.0, stop - 1e-3))));
            expect(close_to(p[2], h, 0.02) || at_start, "the reported contact lies on the two-triangle mesh");
            expect(result.point && close_to(real(result.point->z), p[2], 1e-4), "contact point is on the segment");
        } else ++clears;
    }
    expect(hits > 40 && clears > 40, "the random segments exercise both outcomes");
}
std::vector<LandSegmentResult> batched_query(const LandTerrain& terrain,
    const std::vector<std::pair<sim::math::Vec3, sim::math::Vec3>>& segments, const sim::PartitionExecutor& executor) {
    std::vector<LandSegmentResult> results(segments.size());
    const auto done = executor.execute(sim::tick_partition_count, [&](const std::size_t partition) {
        for (std::size_t i = partition; i < segments.size(); i += sim::tick_partition_count)
            results[i] = terrain.segment_intersection(segments[i].first, segments[i].second);
    });
    expect(bool(done), "batched segment query ran");
    return results;
}
void segment_batch_equality() {
    const auto terrain = make_terrain(6, 6, hill_heights, LandWaterPlane{whole(16), 1});
    const auto segments = random_segments(1000);
    sim::InlineExecutor inline_executor;
    const auto reference = batched_query(terrain, segments, inline_executor);
    std::size_t planes = 0;
    for (const auto& result : reference) planes += result.source == LandContactSource::water_plane;
    expect(planes > 10, "the batch includes water plane contacts");
    for (std::size_t i = 0; i < segments.size(); ++i)
        expect(reference[i] == terrain.segment_intersection(segments[i].first, segments[i].second), "batch equals a direct query");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        expect(batched_query(terrain, segments, executor) == reference, "segment batch equal on 1/2/4/8 workers");
    }
    test_support::ReverseOrderExecutor reverse(8);
    expect(batched_query(terrain, segments, reverse) == reference, "segment batch equal in reverse partition order");
}
class FailReduction final : public sim::PartitionExecutor {
public:
    std::size_t worker_count() const noexcept override { return 1; }
    core::Result<void> execute(const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        for (std::size_t p = 0; p < count; ++p) partition(p);
        if (++calls == 1) return core::Result<void>::success();
        core::Diagnostic diagnostic; diagnostic.code = "synthetic-worker-failure";
        return core::Result<void>::failure(std::move(diagnostic));
    }
    mutable int calls{};
};
std::vector<std::uint32_t> occupancy(const sim::PartitionExecutor& executor) {
    auto field_result = skirmish::load_land_terrain(fixture());
    auto terrain = std::make_shared<const LandTerrain>(std::move(field_result).value());
    auto result = LandOccupancy::create(terrain, 1);
    expect(bool(result), "occupancy created");
    if (!result) return {};
    auto& grid = result.value();
    const LandOccupation first{10, 2, 8, {0, 1, 1, 4}, {2}};
    const LandOccupation second{20, 16, 32, {0, 4}, {2}};
    expect(bool(grid.commit({second, first}, executor)), "occupation snapshot committed");
    expect(grid.cells()[0] == 18 && grid.cells()[2] == 40 && grid.cells()[4] == 0x80000016U,
        "overlapping primary/secondary sources and base restrictions preserved");
    expect(grid.occupations()[0].object == 10 && grid.occupations()[0].primary_cells.size() == 3,
        "commit canonicalizes object order and duplicate raster cells");
    expect(!grid.admits(0, 0, 2) && grid.admits(0, 0, 18), "occupancy uses full subset predicate");
    expect(bool(grid.commit({first, second}, executor)) && grid.changed_cells().empty(), "identical derived values stay clean");
    expect(bool(grid.commit({second}, executor)) && grid.cells()[0] == 16 && grid.cells()[2] == 32
        && grid.cells()[4] == 0x80000014U, "removal preserves other occupant and base");
    const std::vector<std::uint32_t> before(grid.cells().begin(), grid.cells().end());
    auto bad = second; bad.primary_cells.push_back(99);
    expect(!grid.commit({bad}, executor) && std::vector<std::uint32_t>(grid.cells().begin(), grid.cells().end()) == before,
        "invalid input leaves committed cells unchanged");
    expect(!grid.commit({second, second}, executor), "duplicate occupant rejected");
    FailReduction failed;
    expect(!grid.commit({first}, failed) && std::vector<std::uint32_t>(grid.cells().begin(), grid.cells().end()) == before
        && grid.occupations().size() == 1 && grid.occupations()[0].object == second.object,
        "failed partition reduction leaves cells and ownership unchanged");
    expect(bool(grid.commit({}, executor)) && grid.cells()[0] == 1 && grid.cells()[4] == 0x80000004U,
        "empty snapshot restores original base");
    return before;
}
void occupancy_contracts() {
    sim::InlineExecutor inline_executor;
    const auto reference = occupancy(inline_executor);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        expect(occupancy(executor) == reference, "occupancy equal on 1/2/4/8 workers");
    }
    test_support::ReverseOrderExecutor reverse(8);
    expect(occupancy(reverse) == reference, "occupancy equal in reverse partition order");
    expect(!reverse.phases.empty() && reverse.phases.front().partitions == sim::tick_partition_count
        && reverse.phases.front().descending(), "occupancy actually uses partitioned execution");
}
void occupancy_work_budget() {
    LandTerrainInput input;
    input.width = input.height = 257; input.spacing = whole(20);
    input.heights.resize(257 * 257); input.surfaces.resize(257 * 257);
    input.passability.assign(257 * 257, 1);
    auto field_result = LandTerrain::build(std::move(input));
    auto terrain = std::make_shared<const LandTerrain>(std::move(field_result).value());
    auto result = LandOccupancy::create(terrain, 1); auto& grid = result.value();
    const std::vector<LandOccupation> states[]{ {{10, 2, 0, {0, 1}, {}}}, {{10, 2, 0, {1, 2}, {}}} };
    sim::InlineExecutor executor;
    for (int i = 0; i < 8; ++i) expect(bool(grid.commit(states[i % 2], executor)), "warm sparse occupancy buffers");
    measured_allocations = 0;
    measure_allocations = true;
    bool passed = true;
    for (int i = 0; i < 16; ++i) passed = bool(grid.commit(states[i % 2], executor)) && passed;
    measure_allocations = false;
    expect(passed && measured_allocations == 0, "warmed occupancy commits perform no heap allocations");
    expect(grid.last_work().cells_reduced == 3 && grid.last_work().references_bucketed == 2,
        "sparse move reduces only old/new touched cells, independent of map area");
    expect(bool(grid.commit(states[1], executor)) && grid.last_work().cells_reduced == 0
        && grid.last_work().objects_prepared == 0 && grid.changed_cells().empty(), "unchanged snapshot skips commit work");
    expect(grid.cells()[0] == 1 && grid.cells()[1] == 2 && grid.cells()[2] == 2 && grid.cells()[257] == 1,
        "sparse buffer swaps retain untouched base cells");
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
    std::size_t count = 0;
    for (const auto& record : records.value()) {
        auto map = assets::load_map(filesystem.value(), record.canonical_path);
        if (!map) { std::cerr << record.canonical_path << ": map load failed\n"; return 1; }
        if (map.value().kind != assets::MapKind::land) continue;
        auto terrain = skirmish::load_land_terrain(map.value());
        if (!terrain) { std::cerr << record.canonical_path << ": " << terrain.error().message << '\n'; return 1; }
        ++count;
    }
    expect(count != 0, "stock land maps were exercised");
    std::cout << "land maps loaded: " << count << '\n';
    return checks.finish();
}
}
int main(const int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
    contracts(); segment_contracts(); segment_property_check(); segment_batch_equality();
    occupancy_contracts(); occupancy_work_budget(); return checks.finish();
}
