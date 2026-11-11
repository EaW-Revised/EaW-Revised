// LC-85..87: the land fog grid (size and cell shrink from the authored terrain extent), the reveal circle with its floor,
// the regrow timer from the land constant, the work counter of the largest grid, and the grid of every stock land map.
#include "eawr/assets/map.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/skirmish/land_fog.hpp"
#include "eawr/skirmish/land_start.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../support/check.hpp"
#include "../support/env.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace eawr;
using sim::math::Fixed;
using sim::tactical::FogCells;
using sim::tactical::FogRevealer;
using sim::tactical::FogRules;
testing::Checker checks;
void expect(const bool ok, const std::string& message) { checks.check(ok, message, std::cerr); }
template <class T> T take(core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(value).value();
}
void must(core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
Fixed whole(const std::int64_t n) { return Fixed::from_raw(n * Fixed::scale); }

// The oracle: exact integer arithmetic written out per case, not the production formula.
struct Extent { std::uint32_t samples; std::uint32_t cells; std::int64_t cell_raw; };

void grid_from_extent() {
    // Terrain samples are 20 apart. 15 divides 3120 and 4980 exactly; the other sizes shrink their cells below 15.
    const Extent cases[] = {
        {156, 208, 251658240},   // 3120 / 15 = 208 cells of exactly 15
        {249, 332, 251658240},   // 4980 / 15 = 332 cells of exactly 15
        {187, 250, 250987151},   // 3740 / 15 = 249.3 -> 250 cells of 14.96
        {311, 415, 251456105},   // 6220 / 15 = 414.7 -> 415 cells
        {373, 498, 251321348},   // 7460 / 15 = 497.3 -> 498 cells
        {94, 126, 250326715},    // 1880 / 15 = 125.3 -> 126 cells
        {1, 2, 167772160},       // 20 / 15 -> 2 cells of exactly 10
        {600, 512, 393216000},   // 12000 / 15 = 800 -> capped at 512, cells grow to exactly 23.4375
    };
    for (const auto& row : cases) {
        const auto rules = take(skirmish::land_fog_rules(row.samples, row.samples, whole(20), whole(15), whole(6)));
        const auto name = std::to_string(row.samples) + " samples";
        expect(rules.cells_wide == row.cells && rules.cells_tall == row.cells, "LC-85: cells per axis for " + name);
        expect(rules.cell_size.raw() == row.cell_raw, "LC-85: the cell size is the extent over the cell count for " + name);
        expect(rules.map_left == Fixed{} && rules.map_top.raw() == std::int64_t{row.samples} * 20 * Fixed::scale,
            "LC-85: the grid starts at the low corner and row 0 is the top of the terrain for " + name);
        expect(rules.cell_size.raw() * row.cells >= std::int64_t{row.samples} * 20 * Fixed::scale - row.cells
                && rules.cell_size.raw() * row.cells <= std::int64_t{row.samples} * 20 * Fixed::scale + row.cells,
            "LC-85: the cells fill the terrain to within rounding for " + name);
    }
    // A rectangle whose two axes still land on one cell size is accepted: 100 x 50 samples gives 134 x 67 cells.
    const auto wide = take(skirmish::land_fog_rules(100, 50, whole(20), whole(15), whole(6)));
    expect(wide.cells_wide == 134 && wide.cells_tall == 67 && wide.cell_size.raw() == 250406209
            && wide.map_top.raw() == 1000 * Fixed::scale,
        "LC-85: a rectangular terrain with equal cell sizes keeps its own width and height");
    // The authored constants decide the grid: a 30 request halves the counts, a 12 s regrow halves the ramp step.
    const auto coarse = take(skirmish::land_fog_rules(156, 156, whole(20), whole(30), whole(12)));
    expect(coarse.cells_wide == 104 && coarse.cell_size == whole(30) && coarse.ramp_down_step == 10,
        "LC-85: DesiredLandFOWCellSize and LandFOWRegrowTime reach the grid");
    expect(take(skirmish::land_fog_rules(156, 156, whole(20), whole(15), whole(6))).ramp_down_step == 21
            && take(skirmish::land_fog_rules(156, 156, whole(20), whole(15), whole(3))).ramp_down_step == 42,
        "LC-87: the ramp step follows the regrow time (6 s gives 21)");
    // Refusals: no extent, a constant that is not positive, and two axes with different cell sizes.
    expect(!skirmish::land_fog_rules(0, 156, whole(20), whole(15), whole(6)), "a terrain without width has no grid");
    expect(!skirmish::land_fog_rules(156, 156, whole(20), Fixed{}, whole(6)), "a zero cell size is refused");
    expect(!skirmish::land_fog_rules(156, 156, whole(20), whole(15), Fixed{}), "a zero regrow time is refused");
    const auto lopsided = skirmish::land_fog_rules(100, 150, whole(20), whole(15), whole(6));
    expect(!lopsided && lopsided.error().message.find("square") != std::string::npos,
        "a terrain whose axes get different cell sizes is refused with a named reason");
}

FogRules rules_156() { return take(skirmish::land_fog_rules(156, 156, whole(20), whole(15), whole(6))); }

sim::math::Vec3 centre_of(const FogRules& rules, const std::int64_t column, const std::int64_t row) {
    return {Fixed::from_raw(rules.map_left.raw() + column * rules.cell_size.raw() + rules.cell_size.raw() / 2),
        Fixed::from_raw(rules.map_top.raw() - row * rules.cell_size.raw() - rules.cell_size.raw() / 2), Fixed{}};
}

std::vector<sim::tactical::Player> two_players() {
    return {{1, 0, 1, sim::tactical::player_flag_commandable}, {2, 1, 2, sim::tactical::player_flag_commandable}};
}

// The circle one revealer marks: the largest offset along a row and along a column that is still revealed.
struct Reach { std::int64_t along_row{}; std::int64_t along_column{}; };
Reach reach(const FogCells& fog, const FogRules& rules, const std::int64_t column, const std::int64_t row) {
    Reach result;
    while (fog.revealed(0, centre_of(rules, column + result.along_row + 1, row))) ++result.along_row;
    while (fog.revealed(0, centre_of(rules, column, row + result.along_column + 1))) ++result.along_column;
    return result;
}

FogCells marked(const FogRules& rules, const std::int64_t column, const std::int64_t row, const Fixed range,
    const Fixed dense = Fixed::from_raw(Fixed::scale / 2)) {
    const auto players = two_players();
    FogCells fog(rules, players);
    const std::vector<FogRevealer> revealers{{1, 1, centre_of(rules, column, row), range, dense}};
    const sim::InlineExecutor executor;
    must(fog.advance(0, false, revealers, executor));
    return fog;
}

void reveal_circle() {
    const auto rules = rules_156();
    // Range 130 over cells of 15: int(130 / 15 + 0.5) = 9 cells.
    const auto fog = marked(rules, 100, 100, whole(130));
    const auto circle = reach(fog, rules, 100, 100);
    expect(circle.along_row == 9 && circle.along_column == 9, "LC-86: a range of 130 reveals 9 cells of 15 each way");
    expect(fog.revealed(0, centre_of(rules, 100, 100)) && !fog.revealed(1, centre_of(rules, 100, 100)),
        "LC-86: the circle is in the owner's team grid only");
    // The floor: any range below 10 reveals as 10, which is int(10 / 15 + 0.5) = 1 cell.
    for (const std::int64_t tiny : {0, 3, 9}) {
        const auto small = reach(marked(rules, 100, 100, whole(tiny)), rules, 100, 100);
        expect(small.along_row == 1 && small.along_column == 1, "LC-86: a range of " + std::to_string(tiny) + " is floored to 10 (one cell)");
    }
    // 22 / 15 + 0.5 = 1.97 -> 1 cell; 23 / 15 + 0.5 = 2.03 -> 2 cells: the radius truncates after the half cell.
    expect(reach(marked(rules, 100, 100, whole(22)), rules, 100, 100).along_row == 1
            && reach(marked(rules, 100, 100, whole(23)), rules, 100, 100).along_row == 2,
        "LC-86: the radius in cells is the range over the cell size plus a half, truncated");
    // The edge of the terrain clips the circle instead of wrapping or failing.
    const auto corner = marked(rules, 0, 0, whole(130));
    expect(corner.revealed(0, centre_of(rules, 9, 0)) && !corner.revealed(0, centre_of(rules, 10, 0))
            && corner.revealed(0, centre_of(rules, 0, 9)) && !corner.revealed(0, centre_of(rules, 0, 10)),
        "LC-85: a circle at the terrain corner is clipped to the grid");
    // A land map has no dense areas (named deviation): the dense multiplier changes nothing.
    const auto dense_zero = marked(rules, 100, 100, whole(130), Fixed{});
    const auto dense_one = marked(rules, 100, 100, whole(130), whole(1));
    std::vector<std::uint8_t> a, b;
    dense_zero.append_state(a);
    dense_one.append_state(b);
    expect(a == b, "a land grid without dense circles ignores the dense multiplier");
}

// First tick at which the released circle's centre cell fogs out, ticking with no revealers from tick 1.
std::uint64_t fog_out_tick(const FogRules& rules) {
    const auto players = two_players();
    FogCells fog(rules, players);
    const sim::InlineExecutor executor;
    const std::vector<FogRevealer> revealer{{1, 1, centre_of(rules, 50, 50), whole(130), Fixed::from_raw(Fixed::scale / 2)}};
    must(fog.advance(0, false, revealer, executor));
    for (std::uint64_t tick = 1; tick < 2000; ++tick) {
        must(fog.advance(tick, true, {}, executor));
        if (!fog.revealed(0, centre_of(rules, 50, 50))) return tick;
    }
    return 0;
}

void regrow_timer() {
    // Player 1's grid is serviced on ticks 1, 17, 33, ... (id % 16). The first service keeps the held value, the
    // second ramps 255 to 238, then each service lowers it by the step: 238 - 21 x 11 = 7, and 0 at the 13th ramp.
    expect(fog_out_tick(rules_156()) == 209, "LC-87: a released land cell fogs out on the 209th tick with the 6 s regrow (step 21)");
    const auto slow = take(skirmish::land_fog_rules(156, 156, whole(20), whole(15), whole(12)));
    expect(fog_out_tick(slow) == 401, "LC-87: the 12 s land regrow (step 10) fogs a released cell out on tick 401");
    const auto fast = take(skirmish::land_fog_rules(156, 156, whole(20), whole(15), whole(3)));
    expect(fog_out_tick(fast) < 209 && fog_out_tick(fast) > 17, "LC-87: a shorter regrow fogs out earlier");
}

void largest_grid_work() {
    // 600 x 600 samples hit the 512 cap: 512 x 512 cells of 23.4375, four times the cells of a space grid.
    const auto rules = take(skirmish::land_fog_rules(600, 600, whole(20), whole(15), whole(6)));
    expect(rules.cells_wide == 512 && rules.cells_tall == 512, "the largest land grid is 512 x 512");
    const auto players = two_players();
    std::vector<FogRevealer> revealers;
    for (sim::EntityId id = 1; id <= 40; ++id)
        revealers.push_back({id, 1, centre_of(rules, 20 + static_cast<std::int64_t>(id % 8) * 60, 20 + static_cast<std::int64_t>(id / 8) * 60),
            whole(130), Fixed::from_raw(Fixed::scale / 2)});
    std::vector<std::uint8_t> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        FogCells fog(rules, players);
        must(fog.advance(0, false, revealers, executor));
        // int(130 / 23.4375 + 0.5) = 6: each circle touches at most 13 rows of the one allied grid; a touched row copies
        // its 512 value bytes and 512 hold words once. An untouched row is shared, never copied.
        const auto bound = std::size_t{40} * 13 * (512 + 512 * 4);
        expect(fog.copied_grid_bytes() > 0 && fog.copied_grid_bytes() <= bound,
            "the first mark of 40 revealers copies only the rows its circles touch (" + std::to_string(workers) + " workers)");
        must(fog.advance(1, true, revealers, executor));
        expect(fog.copied_grid_bytes() == 0, "stationary revealers and a serviced grid copy nothing (" + std::to_string(workers) + " workers)");
        std::vector<std::uint8_t> state;
        fog.append_state(state);
        if (reference.empty()) reference = std::move(state);
        else expect(state == reference, "the 512 x 512 land fog digest is equal on 1/2/4/8 workers");
    }
}

int installed() {
    const auto root = testing::environment("EAWR_EAW_GAME_ROOT");
    if (!root || root->empty()) { std::cout << "SKIPPED: no game data\n"; return 77; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"})
        mounts.push_back(take(vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data")).mount);
    const auto filesystem = take(vfs::Vfs::mount(mounts));
    const auto loaded = take(data::load_catalog(filesystem, data::Profile::foc));
    // The stock constants, read through the start inputs of a stock land map.
    skirmish::Fixture fixture;
    fixture.map = "data/art/maps/_mp_land_naboo.ted";
    fixture.slots = {{1, "Rebel", 0, true, {}}, {2, "Empire", 1, true, {}}};
    units::UnitTables empty;
    const auto discovered = take(skirmish::read_land_start_inputs(fixture, filesystem, loaded.catalog, empty));
    expect(discovered.common.land_fog_cell_size == whole(15) && discovered.common.land_fog_regrow_seconds == whole(6),
        "the stock constants read DesiredLandFOWCellSize 15 and LandFOWRegrowTime 6");
    // Every stock land map gets a square grid that fills its terrain; none reaches the 512 cap (stock data fact).
    const auto maps = take(skirmish::land_setup_maps(filesystem, loaded.catalog));
    std::size_t checked = 0;
    for (const auto& entry : maps) {
        auto map = assets::load_map(filesystem, entry.path, assets::object_type_catalog(loaded.catalog));
        if (!map || !map.value().terrain) continue;
        const auto& terrain = *map.value().terrain;
        const auto rules = skirmish::land_fog_rules(terrain.width, terrain.height, whole(20),
            discovered.common.land_fog_cell_size, discovered.common.land_fog_regrow_seconds);
        expect(static_cast<bool>(rules), "the stock land map " + entry.path + " gets a fog grid");
        if (!rules) continue;
        const auto expected = [](const std::uint32_t samples) { return (std::uint64_t{samples} * 20 + 14) / 15; };
        expect(rules.value().cells_wide == expected(terrain.width) && rules.value().cells_tall == expected(terrain.height)
                && rules.value().cells_wide <= 512 && rules.value().cell_size.raw() <= whole(15).raw()
                && rules.value().cell_size.raw() > whole(14).raw(),
            "the stock land map " + entry.path + " has ceil(extent / 15) square cells just under 15 wide");
        ++checked;
        std::cout << entry.path << " samples=" << terrain.width << 'x' << terrain.height << " cells=" << rules.value().cells_wide
            << 'x' << rules.value().cells_tall << " cell_raw=" << rules.value().cell_size.raw() << '\n';
    }
    expect(checked >= 10, "the stock land maps were checked");
    return checks.finish();
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
        grid_from_extent(); reveal_circle(); regrow_timer(); largest_grid_work();
        return checks.finish();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
