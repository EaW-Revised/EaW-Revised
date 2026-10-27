#include "land_path_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

namespace eawr::sim::tactical {
namespace {
using math::Fixed;

constexpr std::int32_t step_x[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr std::int32_t step_y[8] = {0, 1, 1, 1, 0, -1, -1, -1};

std::int64_t floor_div(const std::int64_t numerator, const std::int64_t denominator) noexcept {
    auto quotient = numerator / denominator;
    if ((numerator % denominator != 0) && (numerator < 0)) --quotient;
    return quotient;
}

core::Diagnostic checkpoint_error(const char* message) {
    core::Diagnostic diagnostic;
    diagnostic.code = diagnostic_codes::invalid_land_path;
    diagnostic.message = message;
    return diagnostic;
}

bool heap_before(const LandSearch::Entry& a, const LandSearch::Entry& b) noexcept {
    // Lowest f first; the deeper node (larger g) on equal f; the lower cell index last: a total order,
    // so the pop sequence does not depend on the heap layout or the standard library.
    if (a.f != b.f) return a.f < b.f;
    if (a.g != b.g) return a.g > b.g;
    return a.cell < b.cell;
}
bool heap_less(const LandSearch::Entry& a, const LandSearch::Entry& b) noexcept { return heap_before(b, a); }
} // namespace

std::optional<std::uint32_t> land_cell_of(const Fixed coordinate, const Fixed spacing, const std::uint32_t cell_count) noexcept {
    const auto pitch = spacing.raw();
    if (pitch <= 0) return std::nullopt;
    const auto value = coordinate.raw();
    // LR3-16: add half a pitch, then truncate. Below -half a pitch is cell -1: outside, never cell 0.
    const auto half = pitch / 2;
    if (value < -half || value / pitch > static_cast<std::int64_t>(cell_count)) return std::nullopt;
    const auto cell = (value + half) / pitch;
    if (cell >= static_cast<std::int64_t>(cell_count)) return std::nullopt;
    return static_cast<std::uint32_t>(cell);
}

Fixed land_cell_centre(const std::uint32_t cell, const Fixed spacing) noexcept {
    return Fixed::from_raw(static_cast<std::int64_t>(cell) * spacing.raw());
}

LandFootprint land_footprint(const Fixed radius, const Fixed spacing) noexcept {
    const auto pitch = spacing.raw();
    if (pitch <= 0) return {};
    const auto reach = std::max<std::int64_t>(radius.raw(), 0);
    const auto half = pitch / 2;
    return {static_cast<std::int32_t>(floor_div(half - reach, pitch)), static_cast<std::int32_t>(floor_div(half + reach, pitch))};
}

LandPathStats& LandPathStats::operator+=(const LandPathStats& other) noexcept {
    work += other.work; pops += other.pops; expansions += other.expansions; pushes += other.pushes;
    evaluations += other.evaluations; width_cells += other.width_cells; sight_cells += other.sight_cells;
    restarts += other.restarts;
    return *this;
}

// --- State -----------------------------------------------------------------------------------------

void LandSearch::State::prepare() {
    const auto& terrain = occupancy->terrain();
    columns = terrain.width();
    rows = terrain.height();
    spacing = terrain.spacing();
    footprint = land_footprint(request.radius, spacing);
    need = 1;
    if (request.width.raw() > 0) {
        // WLPA-16: a segment is narrow below the request minus the tolerance; a line of k cells is k pitches wide.
        const auto required = request.width.raw() - land_width_tolerance_raw;
        if (required > spacing.raw()) {
            const auto cells = (required + spacing.raw() - 1) / spacing.raw();
            need = static_cast<std::uint32_t>(std::min<std::int64_t>(cells, std::max(columns, rows)));
        }
    }
    seen_revision = occupancy->revision();
}

bool LandSearch::State::point_open(const std::int64_t x, const std::int64_t y) const noexcept {
    if (x < 0 || y < 0 || x >= columns || y >= rows) return false;
    const auto cell = static_cast<std::uint32_t>(y * columns + x);
    return (grid[cell] & ~request.mask) == 0
        && !std::binary_search(request.blocked_cells.begin(), request.blocked_cells.end(), cell)
        && (request.allow_collision_edges
            || !std::binary_search(request.collision_cells.begin(), request.collision_cells.end(), cell));
}

bool LandSearch::State::footprint_open(const std::int64_t x, const std::int64_t y) {
    ++totals.evaluations;
    const auto low_x = x + footprint.low, high_x = x + footprint.high;
    const auto low_y = y + footprint.low, high_y = y + footprint.high;
    if (low_x < 0 || low_y < 0 || high_x >= columns || high_y >= rows) return false;
    for (auto row = low_y; row <= high_y; ++row) {
        for (auto column = low_x; column <= high_x; ++column) {
            if (!point_open(column, row)) return false;
        }
    }
    return true;
}

bool LandSearch::State::wide_enough(const std::int64_t x, const std::int64_t y) {
    if (need <= 1) return true;
    // A line of admitted cells through the cell on each axis must reach the required count (LP-07).
    for (int axis = 0; axis < 2; ++axis) {
        const std::int64_t along_x = axis == 0, along_y = axis == 1;
        std::uint32_t count = 1;
        for (std::int64_t sign = -1; sign <= 1 && count < need; sign += 2) {
            for (std::int64_t offset = 1; count < need; ++offset) {
                ++totals.width_cells;
                if (!point_open(x + sign * offset * along_x, y + sign * offset * along_y)) break;
                ++count;
            }
        }
        if (count < need) return false;
    }
    return true;
}

bool LandSearch::State::admissible(const std::int64_t x, const std::int64_t y, const bool with_width) {
    if (!footprint_open(x, y)) return false;
    if (!with_width || need <= 1) return true;
    // The start (a unit may stand in a narrow place) and the end (the destination's own legality is the
    // placement query's) are exempt from the width test.
    const auto cell = static_cast<std::uint32_t>(y * columns + x);
    return cell == start_cell || cell == goal_cell || wide_enough(x, y);
}

bool LandSearch::State::sight(const std::uint32_t from, const std::uint32_t to) {
    // The cells a segment between two cell centres crosses, corner cells included (the corner rule of the
    // search); the endpoints are validated by whoever owns them.
    const std::int64_t x0 = from % columns, y0 = from / columns, x1 = to % columns, y1 = to / columns;
    const std::int64_t count_x = std::abs(x1 - x0), count_y = std::abs(y1 - y0);
    const std::int64_t sign_x = x1 > x0 ? 1 : -1, sign_y = y1 > y0 ? 1 : -1;
    std::int64_t x = x0, y = y0;
    const auto open = [&](const std::int64_t cx, const std::int64_t cy) {
        ++totals.sight_cells;
        return admissible(cx, cy, need > 1);
    };
    for (std::int64_t ix = 0, iy = 0; ix < count_x || iy < count_y;) {
        const auto decision = (1 + 2 * ix) * count_y - (1 + 2 * iy) * count_x;
        if (decision == 0) {
            if (!open(x + sign_x, y) || !open(x, y + sign_y)) return false;
            x += sign_x; y += sign_y; ++ix; ++iy;
        } else if (decision < 0) {
            x += sign_x; ++ix;
        } else {
            y += sign_y; ++iy;
        }
        if ((x != x1 || y != y1) && !open(x, y)) return false;
    }
    return true;
}

void LandSearch::State::ensure_scratch() {
    if (!scratch && phase != done) scratch = arena->acquire(static_cast<std::size_t>(columns) * rows);
}

void LandSearch::State::release_scratch() {
    if (scratch) arena->release(std::move(scratch));
}

void LandSearch::State::begin_pass() {
    auto& s = *scratch;
    begun = true;
    if (++s.epoch == 0) {
        for (auto& slot : s.slots) slot = {};
        s.epoch = 1;
    }
    s.heap.clear();
    s.cells.clear();
    s.kept.clear();
    auto& slot = s.slots[start_cell];
    slot = {s.epoch, 0, 255, 0};
    s.heap.push_back({heuristic(start_cell), 0, start_cell});
    ++totals.pushes;
}

std::uint32_t LandSearch::State::heuristic(const std::uint32_t cell) const noexcept {
    const auto dx = std::abs(static_cast<std::int64_t>(cell % columns) - static_cast<std::int64_t>(goal_cell % columns));
    const auto dy = std::abs(static_cast<std::int64_t>(cell / columns) - static_cast<std::int64_t>(goal_cell / columns));
    // Octile distance: consistent for the two step costs, so a closed cell is never reopened.
    return static_cast<std::uint32_t>(cost_orthogonal * std::max(dx, dy) + (cost_diagonal - cost_orthogonal) * std::min(dx, dy));
}

void LandSearch::State::end(const LandPathStatus outcome) {
    result.status = outcome;
    phase = done;
    release_scratch();
}

void LandSearch::State::step() {
    // One step is one open-list pop, one width scan of a route cell or one sight line; the work it counts is
    // that one plus every footprint test and width read it made, whatever the footprint's size.
    const auto before = totals.evaluations + totals.width_cells;
    switch (phase) {
        case direct: step_direct(); break;
        case search: step_search(); break;
        case check_width: step_check_width(); break;
        case smooth: step_smooth(); break;
        case finish: step_finish(); break;
        case done: break;
    }
    totals.work += 1 + (totals.evaluations + totals.width_cells - before);
}

void LandSearch::State::step_direct() {
    // LC-10: the clear direct cell route is taken without a search.
    auto& s = *scratch;
    s.cells.assign({start_cell, goal_cell});
    if (sight(start_cell, goal_cell)) {
        s.kept.assign({1});
        phase = finish;
        return;
    }
    s.cells.clear();
    begin_pass();
    phase = search;
}

void LandSearch::State::step_search() {
    auto& s = *scratch;
    if (s.heap.empty()) {
        end(pass == 0 ? LandPathStatus::no_route : LandPathStatus::width_rejected);
        return;
    }
    std::pop_heap(s.heap.begin(), s.heap.end(), heap_less);
    const auto top = s.heap.back();
    s.heap.pop_back();
    ++totals.pops;
    auto* slot = &s.slots[top.cell];
    if (slot->stamp != s.epoch || slot->closed != 0 || slot->g != top.g) return;  // a stale entry
    slot->closed = 1;
    ++totals.expansions;
    if (top.cell == goal_cell) {
        reconstruct(top.cell);
        return;
    }
    const std::int64_t x = top.cell % columns, y = top.cell / columns;
    std::int8_t open[8];
    std::fill(std::begin(open), std::end(open), std::int8_t{-1});
    const auto neighbour = [&](const int direction, std::uint32_t& cell) {
        const auto nx = x + step_x[direction], ny = y + step_y[direction];
        if (nx < 0 || ny < 0 || nx >= columns || ny >= rows) return false;
        cell = static_cast<std::uint32_t>(ny * columns + nx);
        return true;
    };
    const auto test = [&](const int direction) {
        if (open[direction] < 0) open[direction] = admissible(x + step_x[direction], y + step_y[direction], pass == 1) ? 1 : 0;
        return open[direction] == 1;
    };
    for (int direction = 0; direction < 8; ++direction) {
        std::uint32_t cell;
        if (!neighbour(direction, cell)) continue;
        const bool diagonal = (direction & 1) != 0;
        const auto g = top.g + (diagonal ? cost_diagonal : cost_orthogonal);
        const auto& known = s.slots[cell];
        if (known.stamp == s.epoch && (known.closed != 0 || known.g <= g)) continue;
        if (!test(direction)) continue;
        // The corner rule: a diagonal step needs both orthogonal neighbours admitted (WLPA-17 admission per cell).
        if (diagonal && !(test((direction + 7) & 7) && test((direction + 1) & 7))) continue;
        s.slots[cell] = {s.epoch, g, static_cast<std::uint8_t>(direction), 0};
        s.heap.push_back({g + heuristic(cell), g, cell});
        std::push_heap(s.heap.begin(), s.heap.end(), heap_less);
        ++totals.pushes;
    }
}

void LandSearch::State::reconstruct(const std::uint32_t goal) {
    auto& s = *scratch;
    s.cells.clear();
    auto cell = goal;
    for (;;) {
        s.cells.push_back(cell);
        const auto& slot = s.slots[cell];
        if (slot.direction == 255) break;
        const std::int64_t x = static_cast<std::int64_t>(cell % columns) - step_x[slot.direction];
        const std::int64_t y = static_cast<std::int64_t>(cell / columns) - step_y[slot.direction];
        cell = static_cast<std::uint32_t>(y * columns + x);
    }
    std::reverse(s.cells.begin(), s.cells.end());
    // WLPA-16: a positive width first takes the unconstrained route and repairs its narrow runs.
    if (pass == 0 && need > 1 && s.cells.size() > 2) {
        check_index = 1;
        phase = check_width;
        return;
    }
    start_smoothing();
}

void LandSearch::State::step_check_width() {
    auto& s = *scratch;
    const auto cell = s.cells[check_index];
    if (!wide_enough(cell % columns, cell / columns)) {
        pass = 1;
        begin_pass();
        phase = search;
        return;
    }
    if (++check_index + 1 >= s.cells.size()) start_smoothing();  // the end cell is exempt
}

void LandSearch::State::start_smoothing() {
    auto& s = *scratch;
    s.kept.clear();
    smooth_anchor = 0;
    smooth_low = 1;
    smooth_high = 0;
    smooth_step = 1;
    smooth_mode = 0;
    if (!request.smooth || s.cells.size() <= 2) {
        for (std::uint32_t i = 1; i < s.cells.size(); ++i) s.kept.push_back(i);
        phase = finish;
        return;
    }
    phase = smooth;
}

void LandSearch::State::step_smooth() {
    // WLPA-18: binary smoothing, resumable. From an anchor the sight line doubles outward until it fails,
    // then bisects; the farthest visible node becomes the next anchor. One sight line per step.
    auto& s = *scratch;
    const auto last = static_cast<std::uint32_t>(s.cells.size() - 1);
    const auto advance = [&](const std::uint32_t chosen) {
        s.kept.push_back(chosen);
        smooth_anchor = chosen;
        smooth_low = std::min(chosen + 1, last);
        smooth_mode = 0;
        smooth_step = 1;
        if (chosen == last) phase = finish;
    };
    const auto probe = [&](const std::uint32_t index) { return sight(s.cells[smooth_anchor], s.cells[index]); };
    if (smooth_mode == 0) {
        const auto reach = smooth_step < 31 ? (std::uint64_t{1} << smooth_step) : std::uint64_t{1} << 31;
        const auto candidate = static_cast<std::uint32_t>(std::min<std::uint64_t>(smooth_anchor + reach, last));
        if (candidate <= smooth_low) { advance(smooth_low); return; }
        if (probe(candidate)) {
            smooth_low = candidate;
            ++smooth_step;
        } else {
            smooth_high = candidate;
            smooth_mode = 1;
        }
        return;
    }
    if (smooth_high - smooth_low <= 1) { advance(smooth_low); return; }
    const auto middle = smooth_low + (smooth_high - smooth_low) / 2;
    if (probe(middle)) smooth_low = middle;
    else smooth_high = middle;
}

void LandSearch::State::step_finish() {
    auto& s = *scratch;
    if (needs_validation) {
        // The grid changed while this search ran (LP-19): the route must still be walkable now.
        bool walkable = true;
        std::uint32_t previous = s.cells.front();
        for (const auto index : s.kept) {
            const auto cell = s.cells[index];
            walkable = admissible(cell % columns, cell / columns, need > 1) && sight(previous, cell);
            if (!walkable) break;
            previous = cell;
        }
        needs_validation = false;
        if (!walkable) {
            ++totals.restarts;
            if (totals.restarts > max_restarts) { end(LandPathStatus::unstable); return; }
            pass = 0;
            begin_pass();
            phase = search;
            return;
        }
    }
    // Start and end keep their exact positions; interior nodes are cell centres (LR3-16).
    std::vector<math::Vec2> nodes;
    nodes.reserve(s.kept.size() + 1);
    nodes.push_back(request.start);
    for (std::size_t i = 0; i + 1 < s.kept.size(); ++i) {
        const auto cell = s.cells[s.kept[i]];
        nodes.push_back({land_cell_centre(cell % columns, spacing), land_cell_centre(cell / columns, spacing)});
    }
    nodes.push_back(request.end);
    Fixed length;
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        math::Vec2 delta;
        Fixed segment;
        if (math::try_subtract(nodes[i].x, nodes[i - 1].x, delta.x) && math::try_subtract(nodes[i].y, nodes[i - 1].y, delta.y)
            && math::try_length(delta, segment)) {
            static_cast<void>(math::try_add(length, segment, length));
        }
    }
    if (request.detour_ratio) {
        // LC-10: the detour limit is the ratio times the direct distance, equality accepted.
        math::Vec2 delta;
        Fixed direct_length, limit;
        if (math::try_subtract(request.end.x, request.start.x, delta.x) && math::try_subtract(request.end.y, request.start.y, delta.y)
            && math::try_length(delta, direct_length) && math::try_multiply(direct_length, *request.detour_ratio, limit)
            && length > limit) {
            end(LandPathStatus::detour_rejected);
            return;
        }
    }
    result.nodes = std::move(nodes);
    result.length = length;
    end(LandPathStatus::found);
}

// --- LandSearch --------------------------------------------------------------------------------------

LandSearch::LandSearch(const LandOccupancy& occupancy, LandPathArena& arena, const LandPathRequest& request)
    : state_(std::make_unique<State>()) {
    auto& s = *state_;
    s.occupancy = &occupancy;
    s.arena = &arena;
    s.request = request;
    std::sort(s.request.blocked_cells.begin(), s.request.blocked_cells.end());
    std::sort(s.request.collision_cells.begin(), s.request.collision_cells.end());
    s.totals.entity = request.entity;
    s.prepare();
    s.grid = occupancy.cells().data();
    const auto from_x = land_cell_of(request.start.x, s.spacing, s.columns);
    const auto from_y = land_cell_of(request.start.y, s.spacing, s.rows);
    if (!from_x || !from_y) { s.end(LandPathStatus::start_outside); return; }
    const auto to_x = land_cell_of(request.end.x, s.spacing, s.columns);
    const auto to_y = land_cell_of(request.end.y, s.spacing, s.rows);
    if (!to_x || !to_y) { s.end(LandPathStatus::destination_blocked); return; }
    s.start_cell = *from_y * s.columns + *from_x;
    s.goal_cell = *to_y * s.columns + *to_x;
    const auto finish_direct = [&] {
        s.result.nodes = {request.start, request.end};
        math::Vec2 delta;
        Fixed length;
        if (math::try_subtract(request.end.x, request.start.x, delta.x) && math::try_subtract(request.end.y, request.start.y, delta.y)
            && math::try_length(delta, length)) s.result.length = length;
        s.end(LandPathStatus::found);
    };
    // WLPA-15: a forced trivial path, or a destination in the start's own cell, appends start and end.
    if ((!s.request.blocked_cells.empty() || (!s.request.allow_collision_edges && !s.request.collision_cells.empty()))
        && !s.footprint_open(*to_x, *to_y)) { s.end(LandPathStatus::destination_blocked); return; }
    if (request.trivial || s.start_cell == s.goal_cell) { finish_direct(); return; }
    // The start cell is exempt (a unit may leave a cell the grid has since closed); the end must admit it.
    if (!s.footprint_open(*to_x, *to_y)) { s.end(LandPathStatus::destination_blocked); return; }
    s.phase = request.detour_ratio ? State::direct : State::search;
}

LandSearch::LandSearch(LandSearch&&) noexcept = default;
LandSearch& LandSearch::operator=(LandSearch&&) noexcept = default;
LandSearch::~LandSearch() {
    if (state_) state_->release_scratch();
}

bool LandSearch::run(const std::uint64_t budget, LandPathStats* stats) {
    auto& s = *state_;
    if (s.phase == State::done) return true;
    const auto before = s.totals;
    s.waited = 0;
    s.grid = s.occupancy->cells().data();
    if (s.occupancy->revision() != s.seen_revision) {
        s.needs_validation = true;
        s.seen_revision = s.occupancy->revision();
    }
    s.ensure_scratch();
    if (s.phase == State::search && !s.begun) s.begin_pass();
    const auto limit = budget > std::numeric_limits<std::uint64_t>::max() - s.totals.work
        ? std::numeric_limits<std::uint64_t>::max() : s.totals.work + budget;
    while (s.phase != State::done && s.totals.work < limit) s.step();
    if (stats != nullptr) {
        LandPathStats slice = s.totals;
        stats->entity = s.request.entity;
        slice.work -= before.work; slice.pops -= before.pops; slice.expansions -= before.expansions;
        slice.pushes -= before.pushes; slice.evaluations -= before.evaluations; slice.width_cells -= before.width_cells;
        slice.sight_cells -= before.sight_cells; slice.restarts -= before.restarts;
        *stats += slice;
    }
    return s.phase == State::done;
}

bool LandSearch::ended() const noexcept { return state_->phase == State::done; }
bool LandSearch::exhausted() const noexcept {
    return ended() && (state_->result.status == LandPathStatus::no_route || state_->result.status == LandPathStatus::width_rejected);
}
LandPath LandSearch::result() const { return state_->result; }
LandPathStatus LandSearch::status() const noexcept { return state_->result.status; }
std::uint64_t LandSearch::expansions() const noexcept { return state_->totals.work; }
const LandPathStats& LandSearch::totals() const noexcept { return state_->totals; }
EntityId LandSearch::entity() const noexcept { return state_->request.entity; }
std::uint32_t LandSearch::waited() const noexcept { return state_->waited; }
void LandSearch::defer() noexcept { ++state_->waited; }

std::size_t LandSearch::held_bytes() const noexcept {
    const auto& s = *state_;
    std::size_t bytes = sizeof(State) + s.result.nodes.capacity() * sizeof(math::Vec2);
    if (s.scratch) bytes += s.scratch->bytes();
    return bytes;
}

LandSearch::Checkpoint LandSearch::checkpoint() const {
    const auto& s = *state_;
    Checkpoint saved;
    saved.request = s.request;
    saved.phase = s.phase;
    saved.pass = s.pass;
    // A grid change after the last slice has not been seen by run() yet: fold it in, because the revision counter
    // does not survive a save and a restored search would otherwise miss it (LP-14).
    saved.needs_validation = s.needs_validation || s.occupancy->revision() != s.seen_revision;
    saved.waited = s.waited;
    saved.restarts = s.totals.restarts;
    saved.stats = s.totals;
    saved.check_index = s.check_index;
    saved.smooth_anchor = s.smooth_anchor; saved.smooth_low = s.smooth_low; saved.smooth_high = s.smooth_high;
    saved.smooth_step = s.smooth_step; saved.smooth_mode = s.smooth_mode;
    saved.result = s.result;
    if (s.scratch) {
        const auto& scratch = *s.scratch;
        for (std::uint32_t cell = 0; cell < scratch.slots.size(); ++cell) {
            const auto& slot = scratch.slots[cell];
            if (slot.stamp == scratch.epoch) saved.nodes.push_back({cell, slot.g, slot.direction, slot.closed});
        }
        saved.open = scratch.heap;
        saved.cells = scratch.cells;
        saved.kept = scratch.kept;
    }
    return saved;
}

core::Result<LandSearch> LandSearch::from_checkpoint(const LandOccupancy& occupancy, LandPathArena& arena,
    const Checkpoint& saved) {
    using Result = core::Result<LandSearch>;
    LandSearch search(occupancy, arena, saved.request);
    auto& s = *search.state_;
    // A rebuilt search starts from its request; the saved progress replaces what the constructor derived.
    s.release_scratch();
    if (saved.phase > State::done || saved.pass > 1) return Result::failure(checkpoint_error("land path checkpoint phase is invalid"));
    // The constructor ended the search itself (a start or end outside the grid): the saved progress cannot fit.
    if (search.state_->phase == State::done && saved.phase != State::done)
        return Result::failure(checkpoint_error("land path checkpoint disagrees with its request"));
    const auto cells = static_cast<std::size_t>(s.columns) * s.rows;
    for (const auto& node : saved.nodes) {
        if (node.cell >= cells || (node.direction > 7 && node.direction != 255))
            return Result::failure(checkpoint_error("land path node is outside the grid"));
    }
    for (const auto& entry : saved.open) if (entry.cell >= cells) return Result::failure(checkpoint_error("land path entry is outside the grid"));
    for (const auto cell : saved.cells) if (cell >= cells) return Result::failure(checkpoint_error("land path cell is outside the grid"));
    for (const auto index : saved.kept) if (index >= saved.cells.size()) return Result::failure(checkpoint_error("land path kept index is invalid"));
    s.phase = static_cast<State::Phase>(saved.phase);
    s.pass = saved.pass;
    s.needs_validation = saved.needs_validation;
    s.waited = saved.waited;
    s.totals = saved.stats;
    s.totals.entity = saved.request.entity;
    s.totals.restarts = saved.restarts;
    s.check_index = saved.check_index; s.smooth_anchor = saved.smooth_anchor; s.smooth_low = saved.smooth_low;
    s.smooth_high = saved.smooth_high; s.smooth_step = saved.smooth_step; s.smooth_mode = saved.smooth_mode;
    s.result = saved.result;
    if (s.phase == State::done) return Result::success(std::move(search));
    s.begun = !saved.nodes.empty();
    if (saved.nodes.empty() && saved.open.empty() && saved.cells.empty() && saved.kept.empty()) return Result::success(std::move(search));
    s.scratch = arena.acquire(cells);
    auto& scratch = *s.scratch;
    if (++scratch.epoch == 0) {
        for (auto& slot : scratch.slots) slot = {};
        scratch.epoch = 1;
    }
    for (const auto& node : saved.nodes) scratch.slots[node.cell] = {scratch.epoch, node.g, node.direction, node.closed};
    scratch.heap = saved.open;
    scratch.cells = saved.cells;
    scratch.kept = saved.kept;
    return Result::success(std::move(search));
}

} // namespace eawr::sim::tactical
