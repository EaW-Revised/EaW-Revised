#pragma once

#include "eawr/sim/tactical/instance.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace eawr::presentation::ui {

// PL-05..PL-08: the selected-ship path line (docs/behaviour/space-path-lines.md). Engine-free: the
// Godot half only uploads `vertices()`. Nothing here reaches the session.

// Our caps are a deviation from the original game, which draws every segment of every selected ship
// each frame (PL-08).
struct PathLineCaps final {
    std::uint32_t max_ships{256};
    std::uint32_t max_segments_per_ship{64};
    std::uint32_t max_segments{16384};
    float step{20.0F};                    // PL-05: a segment ends once the track has moved this far
    float max_rebuilds_per_second{4.0F};  // PL-08: the hard limit for any real rebuild
};

// A line end in the sim's world axes. `frame` is the plan frame at which the ship is there, relative
// to `PathLineBuilder::base_frame()` (exact in a float; the shader trims by it).
struct PathLineVertex final {
    float x{}, y{}, z{};
    float frame{};
};

struct PathLineShip final {
    sim::EntityId entity{};
    std::shared_ptr<const sim::tactical::PredictedPath> path;
};

// Deterministic work counters: what a test asserts instead of a clock.
struct PathLineStats final {
    std::uint64_t rebuilds{};        // vertex lists built
    std::uint64_t clears{};          // lists emptied (flag off, nothing selected)
    std::uint64_t deferred{};        // changes held back by the rebuild rate limit
    std::uint64_t plans_sampled{};   // plans turned into track points (once per plan)
    std::uint64_t points_sampled{};  // track points evaluated
    std::uint64_t segments_written{};// segments written over all rebuilds
    std::uint32_t ships{};           // ships in the current list
    std::uint32_t segments{};        // segments in the current list
    std::uint32_t ships_over_cap{};  // selected ships left out by the ship cap
    std::uint32_t ships_thinned{};   // ships whose track was thinned to the per-ship cap
};

class PathLineBuilder final {
public:
    explicit PathLineBuilder(PathLineCaps caps = {}) : caps_(caps) {}

    // One presented frame. `ships` are the selected own ships that have a plan, earliest selected
    // first; `enabled` is the data flag. Returns true when the vertex list changed. A change of the
    // selection or of a plan (a new shared plan) rebuilds, at most `max_rebuilds_per_second` times a
    // second (`clock_seconds` is any monotonic clock); an unchanged frame does no work. Turning the
    // flag off or deselecting clears at once.
    bool update(bool enabled, std::span<const PathLineShip> ships, double clock_seconds);

    [[nodiscard]] std::span<const PathLineVertex> vertices() const noexcept { return vertices_; }
    [[nodiscard]] double base_frame() const noexcept { return base_frame_; }
    // Changes whenever `vertices()` changes.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const PathLineStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const PathLineCaps& caps() const noexcept { return caps_; }

private:
    struct TrackPoint final { float x, y, z; double frame; };
    struct Track final {
        std::shared_ptr<const sim::tactical::PredictedPath> path;
        std::vector<TrackPoint> points; // at least `step` apart, the plan's last point included
        double length{};
    };
    struct Shown final {
        sim::EntityId entity{};
        std::shared_ptr<const sim::tactical::PredictedPath> path; // held, so a freed plan's address is never reused
    };
    [[nodiscard]] bool differs(std::span<const PathLineShip> ships) const;
    [[nodiscard]] const Track& track_for(const PathLineShip& ship);
    void rebuild(std::span<const PathLineShip> ships);
    void clear();

    PathLineCaps caps_;
    std::vector<PathLineVertex> vertices_;
    std::vector<Shown> shown_;
    std::unordered_map<sim::EntityId, Track> tracks_;
    double base_frame_{};
    double last_rebuild_{};
    bool built_{};
    std::uint64_t generation_{};
    PathLineStats stats_;
};

} // namespace eawr::presentation::ui
