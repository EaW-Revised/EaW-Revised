#pragma once

#include "eawr/sim/tactical/content.hpp"
#include "session_types.hpp"

#include "eawr/sim/tactical/land_placement.hpp"
#include "eawr/sim/world.hpp"

#include <array>
#include <map>
#include <variant>
#include <vector>

namespace eawr::sim::tactical {

// A company's members placed so far; a candidate position inside any of them is refused (creation and landing admission).
class LandSpawnFilter final : public LandPlacementFilter {
public:
    struct Body { math::Vec2 position; math::Fixed radius; };
    std::vector<Body> bodies;
    math::Fixed radius;
    bool rejects(const math::Vec2 position) const override {
        for (const auto& body : bodies) {
            math::Vec2 delta;
            math::Fixed distance, sum;
            if (!math::try_subtract(position.x, body.position.x, delta.x)
                || !math::try_subtract(position.y, body.position.y, delta.y)
                || !math::try_length(delta, distance) || !math::try_add(radius, body.radius, sum)
                || distance < sum) return true;
        }
        return false;
    }
};

} // namespace eawr::sim::tactical

namespace eawr::sim::tactical::session_detail {

// Remake work policy, independent of machine speed and the retail graph's units.
inline constexpr std::uint64_t land_session_path_budget = 4096;
inline constexpr std::uint64_t land_session_path_slice = 256;

struct LandCombatState {
    std::map<EntityId, std::pair<EntityId, std::uint64_t>> targets;
};

// G-04..07: the command and physical roster of one entrant (a squad container or a lone object). The garrison service
// (session_land_garrison.cpp) owns these records. While the entrant walks in (moving, fading) it is an ordinary live squad;
// once inside it is neither live nor in movement: its squad record and live rows are kept here (`detached`, `hidden`) and its
// members are nested on the host as carried objects. Section 16 of the land state archives the records.
enum class LandGarrisonPhase : std::uint8_t { moving, fading, inside, waiting_exit };
inline constexpr std::uint32_t land_garrison_fade_frames = 2; // LG-FADE: the fixed frames the 0.03 s smoothing implies
struct LandGarrisonEntry {
    EntityId entrant{}, host{};
    TypeId host_type{};
    std::uint32_t bone{};
    std::uint32_t value{1};       // the entrant's slot cost (Garrison_Value), fixed at admission
    LandGarrisonPhase phase{LandGarrisonPhase::moving};
    bool destroy_with_host{};     // a hovering host takes its occupants down with it (G-09)
    std::uint32_t fade_frames{};  // fading: frames left
    math::Vec2 host_position{};   // last known host pose while inside: a dead host leaves no live row (G-09)
    math::Fixed host_facing{};    // turns
    LandPathRequest request;      // the entrant squad's path request, restored on exit
    std::pair<EntityId, std::uint64_t> target{}; // its held combat target and next scan frame
    std::optional<LandMovement::DetachedSquad> detached;
    std::vector<LiveUnit> hidden; // the squad wrapper and its members, ascending id
};

// LMF-09: the centre route and surviving slots remain until their members finish or receive other orders.
struct LandWalkingMember {
    LandFormationSlot slot;
    std::vector<std::uint32_t> centre_segments;
    std::uint32_t current_segment{};
    friend bool operator==(const LandWalkingMember&, const LandWalkingMember&) = default;
};
struct LandWalkingFormation {
    LandPath centre;
    std::vector<LandWalkingMember> slots;
    friend bool operator==(const LandWalkingFormation&, const LandWalkingFormation&) = default;
};
struct LandFormationScratch {
    std::vector<std::size_t> indices;
    std::vector<math::Fixed> deviations;
    std::vector<std::uint32_t> segments;
    std::vector<LandFormationObservation> observations;
    std::vector<std::pair<std::size_t, math::Fixed>> speeds;
    std::vector<EntityId> finished;
    bool valid{true};
};

// LC-51 (#5894): a bought company on its way down. Its whole timeline derives from these two frames and the content
// rules (waiting, landing clip, unload steps); the company's objects exist from `finalize_frame` on.
struct LandLanding {
    std::uint64_t sequence{};      // per-session landing number, strictly increasing
    PlayerId owner{};
    TypeId company{};
    math::Vec3 position{};         // the landing point, on the plane
    math::Fixed yaw{};             // degrees: the owner's reinforcement facing
    std::uint64_t created_frame{};
    std::uint64_t finalize_frame{};
    std::uint64_t pool_token{};    // the pool entry the company was bought as
    SeatId seat{};                 // the issuing seat, preferred controller of the landed objects
    friend bool operator==(const LandLanding&, const LandLanding&) = default;
};

// Stable allocation: searches retain references to this occupancy and arena.
struct LandSession final {
    LandSessionContent content;
    LandOccupancy occupancy;
    LandPathArena arena;
    LandSliceScheduler scheduler;
    LandMovement movement;
    std::map<EntityId, LandSearch> searches;
    std::vector<LandSearch*> search_order;
    std::map<EntityId, LandPathRequest> requests;
    // LMF-01..LMF-08: formations whose centre path search has not ended, by lead squad. Retained and hashed.
    std::map<EntityId, LandFormationOrder> formations;
    std::map<EntityId, LandWalkingFormation> walking_formations;
    // Reused proposal buffers, derived each tick and never archived.
    std::vector<LandWalkingFormation*> formation_work;
    std::vector<LandFormationScratch> formation_scratch;
    std::vector<std::optional<math::Fixed>> formation_speeds;
    std::vector<EntityId> formation_finished;
    // Q1/Q2: retained shared target and next opportunity scan frame for each living squad.
    LandCombatState combat_state;
    std::map<EntityId, LandGarrisonEntry> garrison_entries; // ascending entrant id
    // Whether `object` is a squad or member kept off the live roster inside a host.
    [[nodiscard]] const LandGarrisonEntry* garrison_hiding(const EntityId object) const {
        for (const auto& [id, entry] : garrison_entries) {
            static_cast<void>(id);
            if (entry.phase != LandGarrisonPhase::inside && entry.phase != LandGarrisonPhase::waiting_exit) continue;
            for (const auto& unit : entry.hidden) if (unit.state.entity_id == object) return &entry;
        }
        return nullptr;
    }
    std::map<EntityId, std::pair<TypeId, PlayerId>> bindings; // derived from setup expansion and dynamic_children, never archived
    std::map<EntityId, UnitState> static_roots; // setup poses, rederived on restore, plus the dynamic children
    // LC-55: objects the economy created after setup (a pad's construction and its completed child). They are static
    // roots with a binding; the sorted ids are archived so a restore can tell them from a setup identity.
    std::vector<EntityId> dynamic_children;
    // LC-51: companies in flight, ascending sequence; and the ids of every squad and member the landings created, strictly
    // ascending (archived, so a restore can tell them from the setup roster).
    std::vector<LandLanding> landings;
    std::vector<EntityId> landed;
    std::uint64_t next_landing{1};
    // Retained canonical-encoding scratch, never state: per-partition record bytes (worker phase) and the
    // previous tail size. Cleared and refilled every tick; capacity persists so the tick does not allocate.
    using EncodedParts = std::array<std::vector<std::uint8_t>, tick_partition_count>;
    EncodedParts occupation_parts, squad_parts, member_parts, contact_parts, unit_parts;
    // Pure-query scratch, one retained crossing buffer per projectile partition.
    std::array<std::vector<math::Fixed>, tick_partition_count> projectile_crossings;
    math::Fixed target_query_margin{}; // derived once from immutable combat geometry
    std::size_t canonical_size_hint{};
    explicit LandSession(LandSessionContent source, LandOccupancy field)
        : content(std::move(source)), occupancy(std::move(field)) {}
};

} // namespace eawr::sim::tactical::session_detail

namespace eawr::sim::tactical::session_detail {
// The routes the land step hands to the shared economy (same command path as space).
[[nodiscard]] inline bool land_economy_route(const CommandPayload& payload) {
    return std::holds_alternative<BuyPayload>(payload) || std::holds_alternative<CancelPayload>(payload)
        || std::holds_alternative<PadBuildPayload>(payload) || std::holds_alternative<PadSellPayload>(payload)
        || std::holds_alternative<ReinforcePayload>(payload);
}
} // namespace eawr::sim::tactical::session_detail
