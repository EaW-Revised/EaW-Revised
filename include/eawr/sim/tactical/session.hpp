#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/fog_cells.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/victory.hpp"
#include "eawr/sim/tactical/visibility.hpp"
#include "eawr/sim/world.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::sim::tactical {

struct TacticalTick {
    std::uint64_t completed_tick{};
    std::string state_sha256;
    std::shared_ptr<const TacticalSnapshot> snapshot;
    // Warnings for commands whose unit orders were rejected at execution.
    std::vector<core::Diagnostic> diagnostics;
    // #636: the projectile phase's work this tick, a performance budget's deterministic measure
    // (not state, never hashed): the units its broad phase took from the space index, and those
    // that reached the exact collision tests.
    std::uint64_t projectile_candidates{};
    std::uint64_t projectile_exact_tests{};
};

// Deterministic home of one tactical session. Commands are queued by submit() in
// canonical (tick, player, sequence) order whatever order players submit them in, and
// execute atomically in step(). Rules v1 moves and turns units that have a motion profile
// (#70), chooses targets and fires the weapons of units that have a combat profile (#73; shots
// are events, projectiles are #74), records orders, applies scripted damage to durable units
// (#72), services their hull and hardpoints, removes dead units, keeps each squadron's container
// on its craft (#271), decides the battle's outcome (#77), and publishes per-player sensor
// visibility.
class TacticalSession final {
public:
    TacticalSession(TacticalSession&&) noexcept;
    TacticalSession& operator=(TacticalSession&&) noexcept;
    ~TacticalSession();

    TacticalSession(const TacticalSession&) = delete;
    TacticalSession& operator=(const TacticalSession&) = delete;

    // `sensors` is the sensor table of the content the setup's content identity names
    // (validate_sensors); without one no unit reveals anything beyond its own team.
    // `durability` is that content's durability table (validate_durability); a unit whose type
    // has no profile has no health, takes no damage and never dies. `motion` is its motion table
    // (validate_motion); a unit whose type has no profile never moves or turns. `fog` is the
    // map's fog grid (validate_fog_rules, #274); without it, visibility uses exact range tests.
    // `combat` is its combat table (validate_combat); a unit whose type has no profile never
    // targets or fires (#73). `victory` is its skirmish's victory rules (validate_victory); without
    // them the session never decides an outcome (#77). `abilities` is its ability table
    // (validate_abilities, #76); a unit whose type has no ability profile has no abilities.
    [[nodiscard]] static core::Result<TacticalSession> create(const TacticalSetup& setup,
        std::span<const SensorProfile> sensors = {}, const DurabilityTable& durability = DurabilityTable{},
        const MotionTable& motion = MotionTable{}, const std::optional<FogRules>& fog = std::nullopt,
        const CombatTable& combat = CombatTable{}, const VictoryRules& victory = VictoryRules{},
        const AbilityTable& abilities = AbilityTable{});
    // Creates the setup session and submits every recorded command.
    [[nodiscard]] static core::Result<TacticalSession> from_replay(const TacticalReplay& replay,
        std::span<const SensorProfile> sensors = {}, const DurabilityTable& durability = DurabilityTable{},
        const MotionTable& motion = MotionTable{}, const std::optional<FogRules>& fog = std::nullopt,
        const CombatTable& combat = CombatTable{}, const VictoryRules& victory = VictoryRules{},
        const AbilityTable& abilities = AbilityTable{});

    // Rejects, without changing the session, an unknown or non-commandable issuer, a
    // malformed payload, a command for a tick that has already executed, and a command
    // whose (tick, sequence) does not follow the issuer's previous submission.
    [[nodiscard]] core::Result<void> submit(const PlayerCommand& command);
    // Fails without changing the session at completed tick max_ticks, so record() always
    // stays within the replay tick limit.
    [[nodiscard]] core::Result<TacticalTick> step(const PartitionExecutor& executor);

    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    [[nodiscard]] EntityId next_entity_id() const noexcept;
    [[nodiscard]] std::uint64_t rng_state() const noexcept;
    [[nodiscard]] std::span<const Player> players() const noexcept;
    [[nodiscard]] std::span<const SensorProfile> sensors() const noexcept;
    [[nodiscard]] const DurabilityTable& durability() const noexcept;
    [[nodiscard]] const AbilityTable& abilities() const noexcept;
    [[nodiscard]] const MotionTable& motion() const noexcept;
    [[nodiscard]] const CombatTable& combat() const noexcept;
    [[nodiscard]] const VictoryRules& victory() const noexcept;
    // The decided battle (#77, docs/behaviour/space-victory.md), or nothing while undecided.
    [[nodiscard]] const std::optional<BattleOutcome>& outcome() const noexcept;
    // Live units in ascending ID; a unit leaves the list when its hull reaches zero.
    [[nodiscard]] std::vector<UnitState> units() const;
    // Live squadrons in ascending container ID, each with its live craft (#271).
    [[nodiscard]] std::span<const Squadron> squadrons() const noexcept;
    // The retail fog cells of a session bound to fog rules (#274); null otherwise.
    [[nodiscard]] const FogCells* fog_cells() const noexcept;
    // Projectiles in flight, ascending ID; always empty without damage rules (#74).
    [[nodiscard]] std::span<const Projectile> projectiles() const noexcept;
    // The unit's health, or nothing when it is not live or its type has no durability profile.
    [[nodiscard]] std::optional<DurabilityState> durability_state(EntityId unit) const;
    // The unit's abilities (#76), or nothing when it is not live or its type has none.
    [[nodiscard]] std::optional<AbilityState> ability_state(EntityId unit) const;
    // The unit's movement, or nothing when it is not live or its type has no motion profile.
    [[nodiscard]] std::optional<MotionState> motion_state(EntityId unit) const;
    // PC-08 (#520): the path searches running in slices now, and the memory they hold (their
    // copied layer views and search scratch). Diagnostics only: not state.
    struct SlicedSearchUse {
        std::size_t searches{};
        std::size_t bytes{};
    };
    [[nodiscard]] SlicedSearchUse sliced_searches() const noexcept;
    // The unit's bank roll in degrees (#351), or nothing when it is not live or its type has no
    // motion profile.
    [[nodiscard]] std::optional<math::Fixed> roll_degrees(EntityId unit) const;
    // The unit's targets and fire cycle, or nothing when it is not live or its type has no combat profile.
    [[nodiscard]] std::optional<CombatState> combat_state(EntityId unit) const;
    // A squadron craft's flight (#457: its chase), or nothing when it is not a live craft.
    [[nodiscard]] std::optional<CraftState> craft_state(EntityId craft) const;
    // A squadron's orders (#457: its combat cell), by team container, or nothing.
    [[nodiscard]] std::optional<SquadronState> squadron_state(EntityId container) const;
    [[nodiscard]] std::size_t pending_command_count() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> canonical_state_bytes() const;
    [[nodiscard]] std::string state_sha256() const;
    [[nodiscard]] std::shared_ptr<const TacticalSnapshot> snapshot() const noexcept;

    // The replay of this session so far: its setup, final tick = completed tick, and
    // every executed command (accepted or rejected at execution) in canonical order.
    [[nodiscard]] TacticalReplay record() const;
    // The replay through the next tick (#615): record() with the final tick one past the
    // completed tick and that tick's queued commands. A failed step changes nothing, so after a
    // failure this replays up to and including the failing tick.
    [[nodiscard]] TacticalReplay record_through_next_tick() const;

    // Test seam: changes only private ECS storage order. Authoritative ordering is unchanged.
    void scramble_storage_for_testing();

    // Scenario staging seam (sim_headless --scenario, docs/traces.md): adds a unit with the next
    // stable ID, or removes a live unit, between two ticks, as the original-game recorder does.
    // Neither is a replay command, so record() does not reproduce a session staged this way.
    // `unit.entity_id` is ignored; the unit starts with no order at full health.
    [[nodiscard]] core::Result<EntityId> stage_spawn(const UnitState& unit);
    [[nodiscard]] core::Result<void> stage_remove(EntityId unit);

private:
    class Impl;
    explicit TacticalSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::sim::tactical
