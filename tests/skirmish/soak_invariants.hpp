#pragma once

// The soak's invariants (#627): checks over the simulation state of a long AI-vs-AI battle, made
// after every tick (or every few). They read sim state only, never a GPU or a clock. Each names
// the behaviour rule it guards and, when it fails, the tick and the units, so a nightly failure
// can be turned into a reproduction without reading the run. The driver adds the seed.
//
// The bounds are project choices, not original-game rules: where the original game's tolerance is
// not known they are as loose as the current sim needs to stay quiet, the worst value seen is
// reported in the seed's summary (Metrics), and a bound that later tightens is a change to Limits.

#include "eawr/sim/tactical/fog_cells.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::soak {

struct Limits {
    // Ticks after the start in which hulls may still overlap (the spawn unclip window: ships
    // placed by the map or launched overlapping fly clear of each other).
    std::uint64_t spawn_window{600};
    // hull-overlap: two tracked hulls of one tracking layer (or a hull and a static object) whose
    // rectangles of hard half extents overlap by more than this share of the smaller half extent
    // for `hull_ticks` ticks in a row. Looser than the placement rules of #597 (#605), which
    // will replace it.
    double hull_penetration{0.5};
    std::uint64_t hull_ticks{90};
    // slot-crowding: a craft within `slot_radius` units of a craft of another squadron of its own
    // side for `slot_ticks` ticks in a row.
    double slot_radius{4.0};
    std::uint64_t slot_ticks{900};
    // out-of-bounds: how far past the map's grid a position may lie (a clipped destination near
    // the edge can, space-movement AV-U3).
    double map_margin{1000.0};
    // speed: the most a unit may move in one tick, as a multiple of its type's maximum speed.
    // A ship has a hard bound; a squadron craft a far looser one, because FM-12's catch-up has
    // no cap in the original game (a follower far behind flies well past its maximum speed), so
    // only a jump like a teleport or a wrapped coordinate can exceed it.
    double speed_factor{4.0};
    double craft_speed_factor{40.0};
    friend bool operator==(const Limits&, const Limits&) = default;
};

struct Violation {
    std::string rule;    // the invariant: hull-overlap, slot-crowding, out-of-bounds, speed
    std::string guards;  // the behaviour rule it guards
    std::uint64_t tick{};
    std::string units;   // the units involved, "entity 12 (type 3735928559)"
    std::string message;
};

// The worst values seen, for tuning the limits; reported with every seed.
struct Metrics {
    double max_hull_penetration{};       // share of the smaller half extent, after the spawn window
    std::uint64_t max_hull_run{};        // longest run of ticks over the penetration limit
    std::uint64_t max_slot_run{};        // longest run of ticks within the slot radius
    double max_map_excess{};             // units past the map's grid, 0 inside it
    double max_speed_ratio{};            // a ship's move in one tick over its type's maximum speed
    double max_craft_speed_ratio{};      // the same for a squadron craft
    std::uint64_t ticks_observed{};
};

class Invariants final {
public:
    // `map` is the fog grid of the map (its bounds); without one only the sim's coordinate
    // range is checked.
    Invariants(const sim::tactical::MotionTable& motion, std::optional<sim::tactical::FogRules> map, Limits limits);

    // The state after completed tick `tick`: the live units and squadrons. Ticks must increase.
    void observe(std::uint64_t tick, std::span<const sim::tactical::UnitState> units,
        std::span<const sim::tactical::Squadron> squadrons);

    [[nodiscard]] const std::vector<Violation>& violations() const noexcept { return violations_; }
    [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }

    // The most violations kept; further ones are counted only.
    static constexpr std::size_t max_kept = 50;
    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }

private:
    struct Body {
        eawr::sim::EntityId id{};
        sim::tactical::TypeId type{};
        double x{};
        double y{};
        double z{};
    };
    struct Hull {
        eawr::sim::EntityId id{};
        sim::tactical::TypeId type{};
        sim::tactical::SpaceLayer layer{};
        bool immobile{};
        bool obstacle{};
        double x{};
        double y{};
        double cosine{1.0};
        double sine{};
        double half_x{};
        double half_y{};
    };

    void add(Violation violation);
    void check_bounds(std::uint64_t tick, std::span<const sim::tactical::UnitState> units);
    void check_speed(std::uint64_t tick, std::span<const sim::tactical::UnitState> units);
    void check_hulls(std::uint64_t tick, std::span<const sim::tactical::UnitState> units);
    void check_slots(std::uint64_t tick, std::span<const sim::tactical::UnitState> units,
        std::span<const sim::tactical::Squadron> squadrons);

    sim::tactical::MotionTable motion_;
    std::optional<sim::tactical::FogRules> map_;
    Limits limits_;
    std::vector<Violation> violations_;
    std::size_t dropped_{};
    Metrics metrics_;
    std::optional<std::uint64_t> last_tick_;
    std::map<eawr::sim::EntityId, Body> previous_;
    // Runs of consecutive observed ticks, keyed by the pair, with the tick each began on.
    std::map<std::pair<eawr::sim::EntityId, eawr::sim::EntityId>, std::uint64_t> hull_since_;
    std::map<std::pair<eawr::sim::EntityId, eawr::sim::EntityId>, std::uint64_t> slot_since_;
    std::set<std::pair<eawr::sim::EntityId, eawr::sim::EntityId>> hull_reported_;
    std::set<std::pair<eawr::sim::EntityId, eawr::sim::EntityId>> slot_reported_;
    std::set<eawr::sim::EntityId> bounds_reported_;
    std::set<eawr::sim::EntityId> speed_reported_;
};

// How far two rectangles of half extents (hx_a, hy_a) and (hx_b, hy_b), centred on (ax, ay) and
// (bx, by) and turned by the given unit facings, overlap: the smallest of the four separating
// axes' overlaps, 0 when they are apart. Exposed for the tests.
[[nodiscard]] double rectangle_penetration(double ax, double ay, double a_cos, double a_sin, double a_hx, double a_hy,
    double bx, double by, double b_cos, double b_sin, double b_hx, double b_hy) noexcept;

} // namespace eawr::soak
