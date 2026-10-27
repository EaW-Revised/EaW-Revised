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

enum class LandMoveOrder : std::uint8_t { idle, following, yielded, recovering, dropped, reached };
enum class LandWalkState : std::uint8_t { stopped, starting, moving, turning, stopping, deploying, deployed, undeploying };

// Physical values are instance inputs, in world units/second (turns/second for turn_rate).
// The adapter supplies effective data/modifier values; no stock-unit defaults live here.
struct LandLocomotionProfile {
    math::Fixed maximum_speed{}, acceleration{}, deceleration{}, turn_rate{}, radius{};
    bool walk_transition{};
    // WLPA-73: authored collision class; data only, the contact selector does not read it yet.
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
    bool pass_admitted{}, path_around_admitted{true}, offset_compatible{true}, terrain_blocked{};
    math::Fixed assigned_speed{}, collision_speed{};
    EntityId collision_lock{};
    math::Vec2 terrain_target{};
    bool escape_aligned_admitted{true}, escape_opposed_admitted{true};
    std::uint64_t progress{};
    // LC-83: generic teams share path nodes, but each locomotor owns its trackers.
    std::size_t next_node{}, temporary_next{};
    // WLPA-69: a terrain-blocked member owns a short temporary path on its own locomotor (the squad
    // route and the other members stay untouched). Empty or fully walked means no detour.
    LandPath detour_path;
    std::size_t detour_next{};
    // True only when this member's own step was refused; terrain_blocked also mirrors a refused parent step.
    bool own_terrain_block{};
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
    bool turning{}; // WLPA-68: squad walk-turning state after a path starts; members mirror it (WLPA-79)
    friend bool operator==(const LandMovingSquad&, const LandMovingSquad&) = default;
};

// WLPA-83: two moving Infantry/Bike members of different squads whose headings are opposed or crossing
// (dot <= 0) are a stateless soft pair. One predicate for the contact layer and the movement pair guard.
[[nodiscard]] bool land_soft_opposed(const LandMovingMember& x, const LandMovingSquad& xs,
    const LandMovingMember& y, const LandMovingSquad& ys, bool enabled) noexcept;

// LR3-14/15: exposed for exact boundary tests; turning requests zero before acceleration.
[[nodiscard]] math::Fixed land_member_desired_speed(math::Fixed parent_speed, math::Fixed maximum,
    math::Fixed longitudinal_error, bool turning) noexcept;

// Land-only service seam beside L4, deliberately free of space containers and session bytes.
// LR1-03/D5: caller runs ordinary motion once, before late contact/coordinator publication.
class LandMovement final {
public:
    // Deterministic work counts of the last ordinary step (scratch, never serialized or hashed): retained
    // contact rows, contact-row visits made by the squad and member proposal phases, and pair-sweep candidates.
    struct StepWork {
        std::uint64_t contact_rows{}, contact_visits{}, pair_candidates{};
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
    [[nodiscard]] std::span<const LandMovingSquad> squads() const noexcept { return state_.squads; }
    [[nodiscard]] std::span<const LandMovingMember> members() const noexcept { return state_.members; }
    [[nodiscard]] bool install_path(std::size_t squad, LandPath path, std::uint64_t following_start);
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
    LandNeighbours neighbours_;
    std::vector<std::vector<std::size_t>> neighbour_candidates_;
    // Scratch rebuilt every step. Contact adjacency is a CSR over member indices (rows touching a member);
    // avoid_peers_ holds the member-steering peers (WLPA-59: retained idle peers plus WLPA-83 moving soft peers).
    std::vector<std::uint32_t> adjacency_offset_, adjacency_cursor_, adjacency_row_;
    std::vector<std::vector<std::size_t>> avoid_peers_, soft_candidates_;
    std::vector<std::size_t> member_rank_;
    std::vector<std::uint32_t> member_visits_, squad_visits_, pair_visits_;
    LandNeighbours soft_index_;
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
        bool terrain_blocked{}, own_terrain_block{};
        math::Vec2 terrain_target{};
        std::size_t next_node{}, temporary_next{}, detour_next{};
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
    };
    std::vector<Proposal> proposals_;
    std::vector<SquadProposal> squad_proposals_;
    std::vector<std::size_t> member_order_;
    std::vector<std::vector<std::size_t>> squad_members_;
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
