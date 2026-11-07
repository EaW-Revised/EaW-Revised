#pragma once

#include "eawr/sim/tactical/land_placement.hpp"
#include "eawr/sim/tactical/land_contacts.hpp"
#include "eawr/sim/tactical/land_neighbours.hpp"
#include "eawr/units/land_companies.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace eawr::sim::tactical {

inline constexpr std::uint8_t land_straggler_wait_visits = 30; // WLPA-93

enum class LandMoveOrder : std::uint8_t { idle, following, yielded, recovering, dropped, reached };
enum class LandWalkState : std::uint8_t { stopped, starting, moving, turning, stopping, deploying, deployed, undeploying };

// Physical values are instance inputs, in world units/second (turns/second for turn_rate).
// The adapter supplies effective data/modifier values; no stock-unit defaults live here.
struct LandLocomotionProfile {
    math::Fixed maximum_speed{}, acceleration{}, deceleration{}, turn_rate{}, radius{};
    bool walk_transition{};
    // WLPA-73: authored collision class; the contact selector reads it for generic-team members and single objects (a formation
    // squad's container carries its own class, WLPA-82).
    units::LandCollisionGroup collision_class{units::LandCollisionGroup::none};
    friend bool operator==(const LandLocomotionProfile&, const LandLocomotionProfile&) = default;
};

// WLPA-82: the authored UnitCollisionClass (WLPA-73), never the object kind, makes a moving/stopped pair
// repulse: Infantry and Bike are soft. Every other class keeps the hard pair guard and the path-around.
[[nodiscard]] constexpr bool land_soft_collision_class(units::LandCollisionGroup group) noexcept {
    return group == units::LandCollisionGroup::infantry || group == units::LandCollisionGroup::bike;
}

// Authoritative animation admission, distinct from presentation playback. R13-16..21:
// callers publish clip completion/stop-point and attack/deploy gates, never a guessed tick delay.
struct LandMotionGates {
    bool start_almost_finished{}, stop_point{}, stop_almost_finished{}, turn_almost_finished{true};
    bool animation_prevents_movement{}, deployment_almost_finished{}, turret_aligned{true};
    friend bool operator==(const LandMotionGates&, const LandMotionGates&) = default;
};

struct LandMovingMember {
    EntityId object{};
    std::size_t squad{};
    units::LandCompanyMember source; // L2 kind, locomotor and movement-class identity, retained verbatim
    LandLocomotionProfile profile;
    std::uint32_t mask{};
    math::Vec2 position{}, slot{}; // slot is forward/side in parent-facing coordinates
    math::Fixed facing{}, speed{}, stop_velocity{}, transition_frame{}, deployment_frame{};
    LandWalkState state{LandWalkState::stopped};
    LandMotionGates gates;
    // Copied pair-policy/lock inputs; crush/flee and attack producers remain separate adapters.
    std::uint32_t collision_class{};
    // WLPA-69: only a generic-team member has a terrain record of its own; a formation soldier has none (the
    // container tests its centre point). For a formation squad the first member carries the squad's lock inputs.
    bool pass_admitted{}, path_around_admitted{true}, offset_compatible{true}, terrain_blocked{};
    math::Fixed assigned_speed{}, collision_speed{};
    EntityId collision_lock{};
    math::Vec2 terrain_target{};
    bool escape_aligned_admitted{true}, escape_opposed_admitted{true};
    std::uint64_t progress{};
    // LC-83: generic teams share path nodes, but each locomotor owns its trackers.
    std::size_t next_node{}, temporary_next{};
    // WLPA-84: authored hard extents and spacing gap (formation spacing x 20) of the member's cell in the rows layout.
    // A member with no hard_y keeps the slot it was placed with and never re-maps.
    math::Fixed hard_x{}, hard_y{}, gap{};
    math::Fixed straggler_best_lag{math::Fixed::from_raw(-1)};
    friend bool operator==(const LandMovingMember&, const LandMovingMember&) = default;
};

struct LandMovingSquad {
    EntityId object{};
    math::Vec2 position{};
    math::Fixed facing{}, maximum_speed{}, speed{}, rear_extent{};
    LandPath path; // exactly one L4 result shared by the whole squad
    std::size_t next_node{};
    LandMoveOrder order{LandMoveOrder::idle};
    std::uint64_t following_start{}, progress{};
    EntityId yielding_to{};
    std::uint64_t blocker_progress{};
    bool has_formation{true};
    math::Fixed formation_length{};
    LandPath temporary_path;
    std::size_t temporary_next{};
    EntityId temporary_first{}, temporary_second{};
    bool generic_team{};
    // WLPA-82/95: the container's own collision class and MaxJiggleDistance. A formation squad is one collision body
    // read from these; class None means the squad has no body. Generic teams are never bodies (their members are).
    units::LandCollisionGroup collision_class{units::LandCollisionGroup::none};
    math::Fixed jiggle{};
    // The container's own refused step (WLPA-36) and its contact speeds (deadlock qualification of its contacts).
    bool terrain_blocked{};
    math::Vec2 terrain_target{};
    math::Fixed assigned_speed{}, collision_speed{};
    bool turning{}; // WLPA-68: squad walk-turning state after a path starts; members mirror it (WLPA-79)
    // WLPA-84: the heading (turns) the slots were last mapped for; slots re-map when the path heading leaves it by more
    // than dot 0.99. has_mapped is false for slots that were only placed.
    math::Fixed mapped_axis{};
    bool has_mapped{};
    std::uint8_t straggler_wait_remaining{land_straggler_wait_visits};
    friend bool operator==(const LandMovingSquad&, const LandMovingSquad&) = default;
};

// Logging only (11-04 owner pathing diagnosis): one event per rows re-map and per walk-turning state change of a squad.
// Events go to an optional process-wide sink that nothing in the simulation reads; they never touch state, hash or replay.
// `reason` is meaningful for turn_enter (new_order_from_standing) and turn_retarget (re_order_while_turning).
struct LandMoveTrace {
    enum class Kind : std::uint8_t { remap, turn_enter, turn_retarget, turn_exit };
    Kind kind{Kind::remap};
    std::uint64_t tick{};
    EntityId squad{};
    bool has_old_heading{};
    math::Fixed old_heading{}, new_heading{}; // turns
};
using LandMoveTraceSink = void (*)(const LandMoveTrace&);
// Null removes the sink. The sink runs on the simulation thread inside the ordered commit; it must be thread-safe and quick.
void set_land_move_trace_sink(LandMoveTraceSink sink) noexcept;

// LR3-14/15: exposed for exact boundary tests; turning requests zero before acceleration.
[[nodiscard]] math::Fixed land_member_desired_speed(math::Fixed parent_speed, math::Fixed maximum,
    math::Fixed longitudinal_error, bool turning) noexcept;

// WLPA-84: one member of a rows layout. forward/side are its current offset from the container in the new heading frame
// (used for the order only); hard_x/hard_y/gap are the member's cell, id is the total-order tiebreak.
struct LandRowsInput {
    EntityId id{};
    math::Fixed forward{}, side{}, hard_x{}, hard_y{}, gap{};
};
// Rows layout: members sorted by forward offset (front first, id tiebreak) form consecutive rows of ceil(sqrt N), each row
// sorted by side offset (id tiebreak) and centred laterally on the container; row r stands behind row r-1 by the mean of
// their depths. Returns slots (forward, side) in input order, or nullopt on overflow. Pure and deterministic.
[[nodiscard]] std::optional<std::vector<math::Vec2>> land_rows_layout(std::span<const LandRowsInput> members);

// Land-only service seam beside L4, deliberately free of space containers and session bytes.
// LR1-03/D5: caller runs ordinary motion once, before late contact/coordinator publication.
class LandMovement final {
public:
    // Deterministic work counts of the last ordinary step (scratch, never serialized or hashed): retained
    // contact rows, contact-row visits made by the squad and member proposal phases, and pair-sweep candidates.
    struct StepWork {
        std::uint64_t contact_rows{}, contact_visits{}, pair_candidates{}, bodies{};
    };
    struct Checkpoint {
        std::uint64_t tick{};
        std::vector<LandMovingSquad> squads;
        std::vector<LandMovingMember> members;
        LandContactSettings contact_settings;
        std::vector<LandContact> contacts;
        std::uint64_t contact_complexity{}, last_contact_frame{};
        bool contacts_serviced{};
        friend bool operator==(const Checkpoint&, const Checkpoint&) = default;
    };
    // Invalid IDs, squad indices, geometry, profiles or trackers are refused. Scratch is rebuilt.
    [[nodiscard]] static std::optional<LandMovement> from_checkpoint(Checkpoint saved);
    [[nodiscard]] Checkpoint checkpoint() const { return state_; }
    // Borrowed view for encoders that must not deep-copy the retained state each tick.
    [[nodiscard]] const Checkpoint& state() const noexcept { return state_; }
    [[nodiscard]] const StepWork& last_step_work() const noexcept { return work_; }
    // WLPA-82/95: the number of collision bodies (formation containers with a class plus generic-team members).
    [[nodiscard]] std::size_t body_count() const noexcept { return bodies_.size(); }
    [[nodiscard]] std::span<const LandMovingSquad> squads() const noexcept { return state_.squads; }
    [[nodiscard]] std::span<const LandMovingMember> members() const noexcept { return state_.members; }
    [[nodiscard]] bool install_path(std::size_t squad, LandPath path, std::uint64_t following_start);
    // WLPA-88: the node a walking container finishes its current segment at. Present only for a squad that is
    // translating on its own path (non-generic, following, no contact route, not turning, a node left to walk).
    // A re-order searches from this node and splices the result in, so the squad never stops for the search.
    [[nodiscard]] std::optional<math::Vec2> splice_anchor(std::size_t squad) const;
    [[nodiscard]] std::optional<math::Vec2> bounded_splice_anchor(std::size_t squad) const;
    void cut_splice_anchor(std::size_t squad, math::Vec2 anchor);
    // WLPA-88/89: install a path that starts at splice_anchor() behind the squad's current position, keeping the walk.
    // `sharp` leaves everything untouched: the first corner at the anchor is beyond the smooth-start gate and the caller
    // falls back to the standing start. `rejected` is a stale anchor (the squad moved on, ended or yielded).
    enum class SpliceResult : std::uint8_t { installed, sharp, rejected };
    [[nodiscard]] SpliceResult splice_path(std::size_t squad, LandPath path);
    [[nodiscard]] bool yield(std::size_t squad, EntityId blocker, std::uint64_t blocker_progress);
    // Explicit coordinator recovery uses observed blocker progress; pair service qualifies deadlock separately.
    [[nodiscard]] bool observe_progress(std::size_t squad, EntityId blocker, std::uint64_t progress);
    [[nodiscard]] bool drop_order(std::size_t squad);
    // Sparse death commit: preserves surviving slots, removes contacts, and erases an empty squad.
    [[nodiscard]] bool remove_member(EntityId member);
    [[nodiscard]] bool set_gates(EntityId member, LandMotionGates gates);
    [[nodiscard]] bool set_walk_state(EntityId member, LandWalkState state);
    [[nodiscard]] bool set_animation_progress(EntityId member, math::Fixed transition_frame, math::Fixed deployment_frame);
    [[nodiscard]] bool configure_contacts(LandContactSettings settings);
    // LR1-09: invoke once in the late service, after ordinary motion. Does not move objects.
    [[nodiscard]] core::Result<void> service_contacts(const LandOccupancy& occupancy,
        const PartitionExecutor& executor, std::uint64_t synchronization_frame);
    [[nodiscard]] std::span<const LandContact> contacts() const noexcept { return state_.contacts; }
    // Preserves nonmember occupations. Dynamic members keep zero static restriction bits;
    // swept pair admission and the late retained-contact service own their local restrictions.
    [[nodiscard]] core::Result<void> step(LandOccupancy& occupancy, const PartitionExecutor& executor,
        math::Fixed seconds, bool defer_occupancy = false);
    // Session LR1-03/D5 barrier: motion once, late contacts, then occupancy publication.
    [[nodiscard]] core::Result<void> commit_occupancy(LandOccupancy& occupancy, const PartitionExecutor& executor);
    // Debug/reference oracle; scratch policy only, never serialized or hashed.
    void set_reference_pair_scans(bool enabled) noexcept { reference_pair_scans_ = enabled; }
    // Test-only A/B policy for #4840; not authoritative state or checkpoint identity.
    void set_straggler_repulsion(bool enabled) noexcept { straggler_repulsion_ = enabled; }
private:
    bool reference_pair_scans_{};
    bool straggler_repulsion_{true};
    // WLPA-82/95 collision bodies, scratch rebuilt from the checkpoint, ascending object id. A formation squad with a
    // class is one body (the container, `member == no_body`); a single object (a squad without a formation: a hero, a
    // bike, a lone vehicle) is one body by its own member's class; every generic-team member is a body; a formation
    // soldier is none. Contacts name bodies, so a container contact is keyed by the squad's object id.
    static constexpr std::size_t no_body = static_cast<std::size_t>(-1);
    struct Body {
        EntityId object{};
        std::size_t squad{}, member{no_body};
        std::size_t holder{}; // member index that carries the lock and admission inputs (first member for a container)
        math::Vec2 position{};
        math::Fixed facing{}, speed{}, radius{}, half_x{}, half_y{}, maximum_speed{}, acceleration{};
        math::Fixed assigned_speed{}, collision_speed{};
        units::LandCollisionGroup group{units::LandCollisionGroup::none};
        std::uint32_t rank{};
        std::uint64_t progress{};
        bool container{}, single{}, generic{}, active{}, moving{}, valid{true};
    };
    // Retail's categoriser calls two headings the same direction when their dot exceeds 0.5 (about 60 degrees); every other
    // pair is opposed or crossing. One threshold for soft_opposed, leads and the contact observation.
    [[nodiscard]] static bool same_heading(math::Fixed x_facing, math::Fixed y_facing) noexcept;
    // WLPA-83: two moving soft bodies of different squads that are opposed or crossing (not the same heading), or whose
    // pair is stalled (either assigned speed is zero). The contact layer and the movement pair guard share this predicate.
    [[nodiscard]] static bool soft_opposed(const Body& x, const Body& y, bool enabled) noexcept;
    // Which of two bodies is ahead along the pair's mean heading (a tie goes to the lower object id). Retail's slave-speed
    // branch compares the two signed plane distances; their difference is exactly this sign, so the choice of leader is
    // retail's own. Exactly one of the two is ahead.
    [[nodiscard]] static bool ahead_of(const Body& x, const Body& y) noexcept;
    // Two moving soft containers of different squads heading the same way: the one ahead keeps its move; the pair sweep
    // holds only the other one. (Retail never vetoes: it lowers the follower's speed from the closing rate and the box
    // distance; the hard hold at disc contact is the parallel sweep's stand-in for that.)
    [[nodiscard]] static bool leads(const Body& x, const Body& y, bool enabled) noexcept;
    // WLPA-95 / stopped-pair overlap: the oriented-box intersection coefficient of two bodies (zero when separated).
    [[nodiscard]] static math::Fixed box_overlap(const Body& x, const Body& y);
    std::vector<Body> bodies_;
    std::vector<std::size_t> body_of_member_, body_of_squad_;
    void build_bodies();
    void refresh_bodies();
    [[nodiscard]] std::size_t body_index(EntityId id) const;
    LandNeighbours neighbours_;
    std::vector<std::vector<std::size_t>> neighbour_candidates_;
    // Scratch rebuilt every step. Contact adjacency is a CSR over body indices (rows touching a body); body_peers_
    // holds the member indices a body's soldiers steer against (WLPA-59/83): only while a qualifying contact exists.
    std::vector<std::uint32_t> adjacency_offset_, adjacency_cursor_, adjacency_row_;
    std::vector<std::vector<std::size_t>> body_peers_;
    std::vector<std::uint32_t> member_visits_, squad_visits_, pair_visits_;
    StepWork work_;
    void invalidate_contacts(std::size_t squad);
    Checkpoint state_;
    // Only physical proposal fields are staged: no string/vector copies or allocations per member/tick.
    struct Proposal {
        bool valid{true};
        bool at_slot{};
        math::Vec2 position{};
        math::Fixed facing{}, speed{}, stop_velocity{};
        LandWalkState state{};
        math::Fixed assigned_speed{};
        bool terrain_blocked{};
        math::Vec2 terrain_target{};
        std::size_t next_node{}, temporary_next{};
    };
    struct SquadProposal {
        bool valid{true};
        math::Vec2 position{};
        math::Fixed facing{}, speed{};
        std::size_t next_node{};
        std::uint64_t progress{};
        std::size_t temporary_next{};
        bool terrain_blocked{};
        math::Vec2 terrain_target{};
        bool start_admitted{true};
        bool turning{};
        std::uint8_t straggler_wait_remaining{land_straggler_wait_visits};
        // WLPA-84: a re-map staged by this squad's partition; the member phase reads staged_slot_, the ordered commit
        // writes the slots, the axis and the new rear extent together (the gate reads them from the next step).
        bool remapped{}, has_mapped{};
        math::Fixed mapped_axis{}, rear_extent{}, assigned_speed{};
    };
    std::vector<Proposal> proposals_;
    std::vector<math::Vec2> staged_slot_;
    std::vector<math::Fixed> staged_straggler_lag_;
    std::vector<SquadProposal> squad_proposals_;
    std::vector<std::size_t> member_order_;
    std::vector<std::vector<std::size_t>> squad_members_;
    std::vector<math::Vec2> body_proposed_;
    std::vector<LandOccupation> occupations_;
    std::vector<std::vector<LandContact>> discovered_;
    struct ContactObservation {
        bool valid{true}, predicts{}, same_direction{}, first_behind{}, maneuver{true};
        EntityId primary{};
        math::Fixed score{};
        LandContactKind kind{LandContactKind::terrain};
    };
    std::vector<ContactObservation> contact_observations_;
    std::vector<std::size_t> contact_order_;
    std::vector<std::uint8_t> discovery_valid_;
    std::vector<std::uint8_t> pair_blocked_;
    std::unique_ptr<LandPathArena> contact_arena_;
};

} // namespace eawr::sim::tactical
