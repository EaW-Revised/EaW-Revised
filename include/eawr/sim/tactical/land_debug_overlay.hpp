#pragma once

#include "eawr/sim/tactical/land_movement.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace eawr::sim::tactical {

// Read-only debugging view of the land movement state (owner pathing reports, 11-04). Display values only: plain doubles in
// world units, never fed back into the simulation, never serialized or hashed.
enum class LandOverlaySquadState : std::uint8_t { idle, moving, turning, yielded, lagging };

struct LandOverlayPoint {
    double x{}, y{};
    friend bool operator==(const LandOverlayPoint&, const LandOverlayPoint&) = default;
};

struct LandOverlayMember {
    EntityId object{};
    LandOverlayPoint position;
    LandOverlayPoint slot_target; // parent.position + forward(parent.facing) * slot.x + side * slot.y
    friend bool operator==(const LandOverlayMember&, const LandOverlayMember&) = default;
};

struct LandOverlaySquad {
    EntityId object{};
    LandOverlayPoint position;
    double facing_turns{}, mapped_axis_turns{};
    bool has_mapped{};
    LandOverlaySquadState state{LandOverlaySquadState::idle};
    LandMoveOrder order{LandMoveOrder::idle};
    bool has_order{};
    LandOverlayPoint order_target;      // last path node, the destination cross
    std::vector<LandOverlayPoint> path; // all path nodes
    std::size_t next_node{};            // index into `path` of the node being walked to
    std::vector<LandOverlayMember> members;
    friend bool operator==(const LandOverlaySquad&, const LandOverlaySquad&) = default;
};

struct LandOverlayData {
    std::uint64_t tick{};
    std::vector<LandOverlaySquad> squads;
    friend bool operator==(const LandOverlayData&, const LandOverlayData&) = default;
};

// Builds the overlay rows for `only` (squad object ids), or for every squad when `only` is empty. Squads are in storage order,
// which is ascending object order. Pure and deterministic; reads `movement` only.
[[nodiscard]] LandOverlayData build_land_overlay(const LandMovement& movement, std::span<const EntityId> only = {});

// One line per movement trace event, e.g. "EAWR land-move tick=120 squad=7 remap heading 0.250 -> 0.310". Empty for none.
[[nodiscard]] std::string format_land_move_trace(const LandMoveTrace& event);

// Rate limit for the trace log: re-maps of one squad are written at most once per `min_gap` ticks (the others are counted and
// reported on the next written line); turn state changes are rare and always pass. Thread-safe; logging only.
class LandMoveTraceLimiter final {
public:
    explicit LandMoveTraceLimiter(std::uint64_t min_gap = 5) : min_gap_(min_gap) {}
    // Returns the formatted line, or an empty string when the event is suppressed.
    [[nodiscard]] std::string line(const LandMoveTrace& event);
private:
    struct Entry { std::uint64_t last_tick{}; std::uint64_t suppressed{}; bool seen{}; };
    std::uint64_t min_gap_;
    std::mutex mutex_;
    std::unordered_map<EntityId, Entry> remaps_;
};

} // namespace eawr::sim::tactical
