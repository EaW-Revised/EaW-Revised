#pragma once

#include "eawr/sim/tactical/land_path.hpp"

#include <vector>

namespace eawr::sim::tactical {

// Step costs of the cell search in thousandths of a pitch: 1000 for an orthogonal step and 1414 for a
// diagonal one (LP-02). WLPA-17 gives the base cost of one per cell entered; the original's connectivity is
// not established, so this is the remake's choice and it only shapes the route before smoothing.
inline constexpr std::uint32_t cost_orthogonal = 1000;
inline constexpr std::uint32_t cost_diagonal = 1414;
// A search restarts when the grid changed under its finished route; a fourth restart ends it unstable.
inline constexpr std::uint32_t max_restarts = 3;

struct LandPathArena::Scratch {
    struct Slot {
        std::uint32_t stamp{}, g{};
        std::uint8_t direction{}, closed{};
    };
    std::vector<Slot> slots;
    std::vector<LandSearch::Entry> heap;
    std::vector<std::uint32_t> cells, kept;
    std::uint32_t epoch{};
    [[nodiscard]] std::size_t bytes() const noexcept {
        return slots.capacity() * sizeof(Slot) + heap.capacity() * sizeof(LandSearch::Entry)
            + (cells.capacity() + kept.capacity()) * sizeof(std::uint32_t);
    }
};

struct LandSearch::State {
    enum Phase : std::uint8_t { direct, search, check_width, smooth, finish, done };

    const LandOccupancy* occupancy{};
    LandPathArena* arena{};
    LandPathRequest request;
    const std::uint32_t* grid{};
    std::uint32_t columns{}, rows{};
    math::Fixed spacing;
    LandFootprint footprint;
    std::uint32_t need{1};  // WLPA-16: cells a corridor line needs to count as wide enough
    std::uint32_t start_cell{}, goal_cell{};
    Phase phase{search};
    std::uint8_t pass{};
    bool needs_validation{};
    bool begun{};  // the first pass has put its start on the open list (the scratch is taken on the first slice)
    std::uint64_t seen_revision{};
    std::uint32_t waited{};
    std::uint32_t check_index{};
    std::uint32_t smooth_anchor{}, smooth_low{}, smooth_high{}, smooth_step{};
    std::uint8_t smooth_mode{};
    LandPathStats totals;
    LandPath result;
    std::unique_ptr<LandPathArena::Scratch> scratch;

    void prepare();
    [[nodiscard]] bool point_open(std::int64_t x, std::int64_t y) const noexcept;
    [[nodiscard]] bool footprint_open(std::int64_t x, std::int64_t y);
    [[nodiscard]] bool wide_enough(std::int64_t x, std::int64_t y);
    [[nodiscard]] bool admissible(std::int64_t x, std::int64_t y, bool with_width);
    [[nodiscard]] bool sight(std::uint32_t from, std::uint32_t to);
    [[nodiscard]] std::uint32_t heuristic(std::uint32_t cell) const noexcept;
    void ensure_scratch();
    void release_scratch();
    void begin_pass();
    void end(LandPathStatus outcome);
    void reconstruct(std::uint32_t goal);
    void start_smoothing();
    void step();
    void step_direct();
    void step_search();
    void step_check_width();
    void step_smooth();
    void step_finish();
};

} // namespace eawr::sim::tactical
