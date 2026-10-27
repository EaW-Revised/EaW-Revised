#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/land_terrain.hpp"
#include "eawr/sim/tactical/path_service.hpp"
#include "eawr/sim/world.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

// The land path search (docs/behaviour/land-pathfinding.md, LP-01..LP-23): a deterministic cell search
// over the immutable LandTerrain and the committed LandOccupancy, run in budgeted slices behind the
// shared PathService contract. The walk (WLPA-15..18) lets an alternative planner replace the original
// rectangle graph as long as admission, width and destination semantics stay.
namespace eawr::sim::tactical {

// WLPA-16: a positive requested width repairs segments narrower than the request minus this tolerance
// (a code constant in the original).
inline constexpr std::int64_t land_width_tolerance_raw = math::Fixed::scale / 10;
// LC-10, WLPA-55: the giant-vehicle destination branch rejects a detour longer than 1.8 times the direct
// distance (a code constant). A caller selects the branch with its collision class and passes it as
// LandPathRequest::detour_ratio; it is no universal path length limit.
inline constexpr std::int64_t land_giant_detour_ratio_raw = math::Fixed::scale * 9 / 5;

// Cell coordinates (LR3-16): a cell's centre is (i * spacing, j * spacing) with no half-cell shift; the
// inverse adds half a pitch before the integer division. nullopt outside the grid.
[[nodiscard]] std::optional<std::uint32_t> land_cell_of(math::Fixed coordinate, math::Fixed spacing,
    std::uint32_t cell_count) noexcept;
[[nodiscard]] math::Fixed land_cell_centre(std::uint32_t cell, math::Fixed spacing) noexcept;

// The footprint rectangle of a soft radius around a cell centre, in cells relative to that cell (LC-07,
// WLPA-06, LP-03): both corners go through the same world-to-cell conversion as a point.
struct LandFootprint {
    std::int32_t low{}, high{};
    friend constexpr bool operator==(const LandFootprint&, const LandFootprint&) noexcept = default;
};
[[nodiscard]] LandFootprint land_footprint(math::Fixed radius, math::Fixed spacing) noexcept;

struct LandPathRequest {
    EntityId entity{};
    // The authored bits of the mover's movement class (LandTerrain::movement_mask); a cell is entered when
    // every bit of its derived passability is in the mask (WLPA-01).
    std::uint32_t mask{};
    math::Vec2 start{}, end{};
    // The soft footprint radius (WLPA-14): authored per object type, never a fixed infantry radius.
    math::Fixed radius{};
    // A positive width repairs narrow segments (WLPA-16); zero asks for the unconstrained route.
    math::Fixed width{};
    // LC-10: set for the giant-vehicle branch; the accepted detour is at most this multiple of the direct
    // distance, and a clear direct route is taken without searching.
    std::optional<math::Fixed> detour_ratio;
    // WLPA-15: an explicitly forced trivial path appends start and end directly.
    bool trivial{};
    // WLPA-18: smoothing is a postprocess; a positive width keeps it off for the repair and runs it after.
    bool smooth{true};
    // L5 copied dynamic geometry. Hard cells remain excluded on fallback; collision-marked
    // cells are excluded only in the first pass. The backend owns/saves the request snapshot.
    std::vector<std::uint32_t> blocked_cells, collision_cells;
    bool allow_collision_edges{};
    friend bool operator==(const LandPathRequest&, const LandPathRequest&) noexcept = default;
};

// LR3-01 asks for admission denied, in progress, failed and found to stay distinct outcomes; a denial is
// the admission ledger's (LandPathAdmission), never a completed no-route result.
enum class LandPathStatus : std::uint8_t {
    searching,
    found,
    no_route,             // the unconstrained search exhausted its reachable cells
    width_rejected,       // WLPA-16: the width-constrained repair failed
    detour_rejected,      // LC-10: longer than ratio * direct distance
    destination_blocked,  // the end cell is outside the grid or does not admit the footprint
    start_outside,        // the start is outside the grid
    unstable,             // the grid changed under the route more often than the restart limit
};

struct LandPath {
    LandPathStatus status{LandPathStatus::searching};
    std::vector<math::Vec2> nodes;  // start, interior cell centres (or smoothed corners), end
    math::Fixed length;
    friend bool operator==(const LandPath&, const LandPath&) = default;
};

// What a search counts (PC-08 analogue): deterministic work, never a clock. `work` is what the slice
// budget compares: one per cell expansion, plus the cells read by width scans and smoothing sight lines.
struct LandPathStats {
    EntityId entity{};
    std::uint64_t work{};
    std::uint64_t pops{};         // open-list entries taken, stale ones included
    std::uint64_t expansions{};   // cells expanded
    std::uint64_t pushes{};
    std::uint64_t evaluations{};  // footprint admission tests
    std::uint64_t width_cells{};
    std::uint64_t sight_cells{};  // cells read by smoothing and direct sight lines
    std::uint32_t restarts{};
    LandPathStats& operator+=(const LandPathStats& other) noexcept;
    friend bool operator==(const LandPathStats&, const LandPathStats&) = default;
};

class LandPathArena;

class LandSearch final {
public:
    struct Node {
        std::uint32_t cell{}, g{};
        std::uint8_t direction{}, closed{};
        friend constexpr bool operator==(const Node&, const Node&) noexcept = default;
    };
    struct Entry {
        std::uint32_t f{}, g{}, cell{};
        friend constexpr bool operator==(const Entry&, const Entry&) noexcept = default;
    };
    // A complete copy of the progress: restoring continues exactly where the search stood, against whatever
    // grid the restoring session has (LP-19).
    struct Checkpoint {
        LandPathRequest request;
        std::uint8_t phase{}, pass{};
        bool needs_validation{};
        std::uint32_t restarts{};
        std::uint32_t waited{};
        LandPathStats stats;
        std::vector<Node> nodes;
        std::vector<Entry> open;
        std::vector<std::uint32_t> cells, kept;
        std::uint32_t smooth_anchor{}, smooth_low{}, smooth_high{}, smooth_step{}, check_index{};
        std::uint8_t smooth_mode{};
        LandPath result;
        friend bool operator==(const Checkpoint&, const Checkpoint&) = default;
    };

    // The occupancy and arena must outlive the search. Invalid input ends the search at once with a status.
    LandSearch(const LandOccupancy& occupancy, LandPathArena& arena, const LandPathRequest& request);
    LandSearch(LandSearch&&) noexcept;
    LandSearch& operator=(LandSearch&&) noexcept;
    LandSearch(const LandSearch&) = delete;
    LandSearch& operator=(const LandSearch&) = delete;
    ~LandSearch();

    [[nodiscard]] static core::Result<LandSearch> from_checkpoint(const LandOccupancy& occupancy,
        LandPathArena& arena, const Checkpoint& saved);
    [[nodiscard]] Checkpoint checkpoint() const;

    // Runs until the search ends (true) or the slice has counted `budget` more work; the check sits between
    // two steps, so a slice can overshoot by one step (LR3-04). `stats`, when given, receives this slice.
    bool run(std::uint64_t budget, LandPathStats* stats = nullptr);
    [[nodiscard]] bool ended() const noexcept;
    // FC-68: true after the search ran out of routes, not after a rejected or unstable one.
    [[nodiscard]] bool exhausted() const noexcept;
    [[nodiscard]] LandPath result() const;
    [[nodiscard]] LandPathStatus status() const noexcept;
    [[nodiscard]] std::uint64_t expansions() const noexcept;  // the work of every slice so far
    [[nodiscard]] const LandPathStats& totals() const noexcept;
    [[nodiscard]] std::size_t held_bytes() const noexcept;
    [[nodiscard]] EntityId entity() const noexcept;
    // Slices the admission throttle has refused since the last one that ran; the scheduler serves the
    // longest-waiting search first, so a throttled search cannot starve.
    [[nodiscard]] std::uint32_t waited() const noexcept;
    void defer() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Reusable search scratch (LP-17): a search takes a scratch on its first slice and gives it back when it
// ends, so a warmed arena makes no heap allocation per slice. It is thread safe (searches run on workers).
class LandPathArena final {
public:
    struct Scratch;
    LandPathArena();
    ~LandPathArena();
    LandPathArena(const LandPathArena&) = delete;
    LandPathArena& operator=(const LandPathArena&) = delete;
    [[nodiscard]] std::unique_ptr<Scratch> acquire(std::size_t cells);
    void release(std::unique_ptr<Scratch> scratch);
    [[nodiscard]] std::size_t pooled() const;
    [[nodiscard]] std::size_t pooled_bytes() const;
private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Scratch>> pool_;
};

struct LandPathBackend final {
    using Search = LandSearch;
    using Checkpoint = LandSearch::Checkpoint;
    using Stats = LandPathStats;

    [[nodiscard]] static Search begin(const LandOccupancy& occupancy, LandPathArena& arena,
        const LandPathRequest& request) {
        return Search(occupancy, arena, request);
    }
    [[nodiscard]] static core::Result<Search> restore(const LandOccupancy& occupancy, LandPathArena& arena,
        const Checkpoint& checkpoint) {
        return Search::from_checkpoint(occupancy, arena, checkpoint);
    }
    // The whole search in one call: what a synchronous caller (the original's static search) gets.
    [[nodiscard]] static LandPath plan(const LandOccupancy& occupancy, LandPathArena& arena,
        const LandPathRequest& request, LandPathStats* stats = nullptr);
    // The search given up once its work reaches `budget` (nullopt then).
    [[nodiscard]] static std::optional<LandPath> plan_within(const LandOccupancy& occupancy, LandPathArena& arena,
        const LandPathRequest& request, std::uint64_t budget, LandPathStats* stats = nullptr);
};
using LandPathService = PathService<LandPathBackend>;

// LR3-01, LR3-06: the admission throttle. A slice is admitted only while the accumulated charge is
// strictly below the quota (equality refuses; a zero quota admits nothing) and may overshoot it. The reset
// follows the interval input: below 2 every frame, otherwise on frames divisible by it. Quota and interval
// are separate profile inputs in remake work units, not the original's graph-node units (LR3-02).
class LandPathAdmission final {
public:
    LandPathAdmission(std::uint64_t quota, std::uint32_t reset_interval) noexcept
        : quota_(quota), reset_interval_(reset_interval) {}
    void begin_frame(std::uint64_t frame) noexcept {
        if (reset_interval_ < 2 || frame % reset_interval_ == 0) spent_ = 0;
    }
    [[nodiscard]] bool admits() const noexcept { return spent_ < quota_; }
    void charge(std::uint64_t work) noexcept { spent_ += work; }
    [[nodiscard]] std::uint64_t spent() const noexcept { return spent_; }
private:
    std::uint64_t quota_{}, spent_{};
    std::uint32_t reset_interval_{};
};

struct LandSliceReport {
    std::size_t admitted{}, denied{}, ended{};
    std::uint64_t work{};
};

// One tick's slices (LP-18). The admission decision is serial and made in the given order, so it never
// depends on the worker count; the admitted searches then run their slices side by side in the
// `land-path-slices` phase, each reading only the committed grid and writing only its own state. The
// caller publishes ended results in ascending entity ID (the ordered commit).
class LandSliceScheduler final {
public:
    [[nodiscard]] core::Result<LandSliceReport> run(const PartitionExecutor& executor,
        std::span<LandSearch* const> searches, LandPathAdmission& admission, std::uint64_t slice);
private:
    std::vector<LandSearch*> order_, admitted_;
    std::vector<std::uint64_t> before_;
};

} // namespace eawr::sim::tactical
