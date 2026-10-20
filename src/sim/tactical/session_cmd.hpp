#pragma once

#include "session_tick.hpp"

#include <array>
#include <set>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace eawr::sim::tactical::session_detail {

// Private handler for admitted orders, called only by commands() in its existing
// serial unit loop. Reference wrappers borrow callbacks without allocating or
// extending their lifetimes; lane planning remains owned by commands().
struct Tick::CommandOrders final {
    Tick& owner;
    std::function<core::Result<RejectReason>(EntityId, const AbilityPayload&)> apply_ability;
    std::function<core::Result<void>(EntityId, const LiveUnit&, std::size_t, std::optional<std::uint64_t>)> submit;
    std::function<core::Result<void>(std::size_t, CollisionWorld&)> world_for;
    std::function<core::Result<void>(EntityId, LiveUnit&, const Impl::ApproachPlan&)> plan_approach;
    std::function<std::optional<std::pair<EntityId, core::Diagnostic>>()> plan_pending;
    std::function<bool()> pending_empty;
    std::function<core::Result<void>(EntityId, const core::Diagnostic&)> unit_failure;

    core::Result<void> apply(const PlayerCommand& command, const std::vector<EntityId>& selected_units,
        EntityId unit_id, LiveUnit& live, EntityId approach, bool waypoint_resume,
        bool attack_assignment_allowed, std::vector<EntityId>& group,
        std::vector<EntityId>& squadron_group) const;
};

// Economy and ability transactions run only at their original ordered command
// sites. Callback wrappers borrow phase-local storage, as CommandOrders does.
struct Tick::CommandHandlers final {
    Tick& phase;
    std::function<core::Result<void>(EntityId)> replan;
    std::function<core::Result<void>(EntityId, const LiveUnit&, std::size_t, std::optional<std::uint64_t>)> submit;
    std::function<core::Result<void>(EntityId, LiveUnit&, const Impl::ApproachPlan&)> plan_approach;
    std::function<core::Result<void>(std::size_t, CollisionWorld&)> world_for;
    std::deque<PlayerCommand>& concentrate_batches;
    std::vector<SpaceBody>& concentrate_new_bodies;
    CommandKey& concentrate_command_key;

    std::vector<LiveUnit*> ordinary_ability_holders(EntityId unit_id, AbilityKind kind) const;
    core::Result<RejectReason> apply_ability(EntityId unit_id, const AbilityPayload& payload) const;
private:
    core::Result<RejectReason> switch_ordinary_ability(const AbilityPayload& payload,
        const std::vector<LiveUnit*>& holders, auto& slot_of, auto& profile_of, const std::uint64_t& command_tick,
        const std::uint64_t& frame, std::vector<PlayerEconomy>& ledgers, detail::MapStage<CraftState>& crafts,
        UnitStage& staged, std::vector<EntityId>& changed_speed, bool& hunt_stopped) const;
    core::Result<RejectReason> apply_blast(EntityId unit_id, const AbilityPayload& payload,
        const std::uint64_t& command_tick, UnitStage& staged) const;
    core::Result<RejectReason> apply_corrupt_systems(EntityId unit_id, const AbilityPayload& payload,
        const std::uint64_t& command_tick, std::map<EntityId, ArrivalState>& arrivals, const std::uint64_t& frame,
        UnitStage& staged, const MotionTable& table) const;
    core::Result<RejectReason> apply_replenish_wingmen(EntityId unit_id, const AbilityPayload& payload,
        const std::uint64_t& command_tick, TacticalSession::PlacementWork& reinforcement_work,
        detail::MapStage<SquadronState>& minds, detail::MapStage<CraftState>& crafts,
        std::map<EntityId, ArrivalState>& arrivals, std::vector<PlayerEconomy>& ledgers,
        std::vector<PlayerCommand>& pad_requests, std::map<EntityId, PopulationShare>& shares,
        std::vector<Squadron>& arrived_squadrons, std::vector<std::pair<EntityId, PlayerId>>& earners,
        EntityId& next_id, UnitStage& staged, const SquadronTable& squadron_table,
        std::map<EntityId, PadState>& pads, std::map<EntityId, ConstructionState>& construction) const;
    core::Result<RejectReason> apply_spawned_ability(EntityId unit_id, const AbilityPayload& payload,
        const std::uint64_t& command_tick) const;
    core::Result<RejectReason> apply_beam_ability(EntityId unit_id, const AbilityPayload& payload,
        const std::uint64_t& command_tick, std::vector<PlayerEconomy>& ledgers, UnitStage& staged) const;
    core::Result<RejectReason> apply_concentrate_fire(EntityId unit_id, const AbilityPayload& payload,
        const std::uint64_t& command_tick, std::map<EntityId, ArrivalState>& arrivals,
        std::vector<PlayerEconomy>& ledgers, UnitStage& staged, const detail::CombatWorld& view) const;
    core::Result<RejectReason> apply_buzz_droids(EntityId unit_id, const AbilityPayload& payload,
        const std::uint64_t& command_tick, detail::MapStage<SquadronState>& minds, EntityId& next_id,
        const std::uint64_t& frame, UnitStage& staged, const SquadronTable& squadron_table,
        const MotionTable& table) const;
    core::Result<RejectReason> apply_ion_cannon_shot(EntityId unit_id, const AbilityPayload& payload,
        const std::uint64_t& command_tick, detail::MapStage<SquadronState>& minds, UnitStage& staged,
        const SquadronTable& squadron_table) const;
    core::Result<RejectReason> apply_ordinary_ability(EntityId unit_id, const AbilityPayload& payload,
        const std::uint64_t& command_tick, detail::MapStage<SquadronState>& minds,
        detail::MapStage<CraftState>& crafts, std::vector<PlayerEconomy>& ledgers, const std::uint64_t& frame,
        UnitStage& staged, const SquadronTable& squadron_table, const MotionTable& table) const;
public:
    core::Result<RejectReason> apply_area_ability(EntityId unit_id, const AreaAbilityPayload& payload) const;
    core::Result<RejectReason> apply_manual_target(EntityId unit_id, const ManualTargetPayload& payload,
        PlayerId issuer, bool player_clock = true) const;
    core::Result<void> special_fire(const PlayerCommand& command, const SpecialFirePayload& payload, PlayerId issuer) const;
    core::Result<void> repair_hardpoint(const PlayerCommand& command, const RepairHardpointPayload& payload, PlayerId issuer) const;
    core::Result<void> economy(const PlayerCommand& command) const;
};

// Phase-local path lanes and command preparation; ordered callers retain all barriers.
struct Tick::PlanLanes final {
    explicit PlanLanes(Tick& owner) : phase(owner) {}

    Tick& phase;
    struct PlanJob {
        EntityId unit{};
        math::Vec3 destination{};
        std::optional<math::Fixed> max_speed;
        std::size_t layer{};
        CommandKey order{};
        std::uint32_t rank{};
        bool landing{};
    };
    std::vector<PlanJob> pending;
    std::array<std::uint64_t, 4> spent{};
    std::deque<PlayerCommand> concentrate_batches;
    std::vector<SpaceBody> concentrate_new_bodies;
    CommandKey concentrate_command_key{};
    std::optional<std::deque<PlayerCommand>> waypoint_legs;
    std::deque<PlayerCommand> autofire_commands;
    std::set<EntityId> autofire_switched;

    core::Result<void> submit(const EntityId unit_id, const LiveUnit& live, const std::size_t layer, const std::optional<std::uint64_t> clip);
    core::Result<void> publish(const EntityId unit_id, const LiveUnit& live, const Impl::SlicedSearch& search);
    core::Result<void> world_for(const std::size_t layer, CollisionWorld& collisions);
    core::Result<void> unit_failure(const EntityId unit_id, const core::Diagnostic& error);
    core::Result<void> start_sliced(const PlanJob& job);
    core::Result<void> land(const PlanJob& job);
    std::optional<std::pair<EntityId, core::Diagnostic>> plan_pending();
    core::Result<void> spread_squadrons(const std::vector<EntityId>& squadrons);
    core::Result<void> plan_group(const PlayerCommand& command, const std::vector<EntityId>& group);
    core::Result<void> plan_approach(const EntityId unit_id, LiveUnit& live, const Impl::ApproachPlan& plan);
    core::Result<void> replan(const EntityId unit_id);
    core::Result<void> queue_targeted_autofire();
    core::Result<void> prepare_paths();
    core::Result<void> prepare_commands();
    core::Result<void> finish();
    core::Result<void> scripted_damage(const PlayerCommand& command, const DamagePayload* damage, const EntityId unit_id, const DurabilityProfile* profile);
    core::Result<void> apply_cinematic(const PlayerCommand& command, const RetreatCinematicPayload* cinematic, const PlayerId issuer);
    core::Result<void> apply_retreat(const PlayerCommand& command, const RetreatPayload* retreat, const PlayerId issuer);
    core::Result<void> apply_beacon(const PlayerCommand& command, const BeaconPayload* beacon, const PlayerId issuer);
    core::Result<void> apply_reveal(const PlayerCommand& command, const RevealAllPayload* reveal, const PlayerId issuer);
};

} // namespace eawr::sim::tactical::session_detail
