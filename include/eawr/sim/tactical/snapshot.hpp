#pragma once

#include "eawr/sim/tactical/instance.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/retreat.hpp"
#include "eawr/sim/tactical/victory.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <utility>

// The published, read-only view of one completed tactical tick. Presentation reads only this
// header's types; it never sees the session that produced them (UI-07 boundary,
// tools/check_presentation_boundary.py).
namespace eawr::sim::tactical {
namespace checkpoint_detail { struct Access; }

// A killed craft spinning away (#447, docs/behaviour/space-fighter-deaths.md SP rules). It left
// the session with its unit_destroyed event, so it is no instance: nothing targets, selects or
// counts it. Presentation draws the craft here until its spin_away_ended event. The facing is
// FoC's roll, pitch (positive lowers the nose) and yaw in degrees (FM-02); transform is the same
// pose as an instance transform. visible_to as for an instance.
struct SpinningCraft {
    EntityId entity_id{};
    TypeId type_id{};
    PlayerId owner{};
    math::Mat3x4 fixed_transform{};
    math::Fixed roll{};
    math::Fixed pitch{};
    math::Fixed yaw{};
    std::uint64_t visible_to{};
    friend bool operator==(const SpinningCraft&, const SpinningCraft&) = default;
};

// #424: a squadron's attack target (its team container's, FT-01), for presentation: the world UI
// lays out the icons of dogfighting squadrons (docs/behaviour/foc-battle-world-ui.md WU-25).
struct SquadronTarget {
    EntityId squadron{};
    EntityId target{};
    bool closing{};  // FA-01: its leader closes on the target beyond the strafe reach
    // #457: the sim's combat cell the squadron records (space-fighters FD-02), and whether it
    // joined it (FD-03).
    bool recorded{};
    bool joined{};
    std::int32_t cell_x{};
    std::int32_t cell_y{};
    friend constexpr bool operator==(const SquadronTarget&, const SquadronTarget&) noexcept = default;
};

struct SnapshotPlayer {
    PlayerId player_id{};
    TeamId team_id{};
    bool neutral{}; // WHZ-51: relationship metadata, derived from the bound faction data
    bool radar_enabled{}; // WRAD-02: enemy minimap permission, independent of world fog.
    friend constexpr bool operator==(const SnapshotPlayer&, const SnapshotPlayer&) noexcept = default;
};

// WHZ-51: ordinary combat excludes either neutral player and players on the same team.
// Both simulation and presentation use the same immutable player relationship table.
[[nodiscard]] bool players_hostile(std::span<const SnapshotPlayer> players, PlayerId owner, PlayerId target) noexcept;
[[nodiscard]] bool players_allied(std::span<const SnapshotPlayer> players, PlayerId owner, PlayerId target) noexcept;

// Immutable published copy of one completed tick: the players in ascending ID order,
// ascending-ID instances with their per-player visibility, and the events emitted while
// executing the frame that completed it (none for the tick-zero snapshot). Combat events (#73)
// are the targeting phase's acquisitions and shots, in ascending shooter ID, then weapon order.
// Projectiles (#74) are those in flight after the tick, ascending ID, for presentation (#80).
// Once a battle is decided (#77) every later snapshot carries its outcome.
// WBF-45: lifetime deaths survive the bounded live event history. These derived
// presentation rows never influence simulation decisions or canonical bytes.
struct BattleLoss {
    PlayerId owner{};
    TypeId type{};
    PlayerId killer{};
    std::uint64_t count{};
    std::uint64_t tick{};
    friend bool operator==(const BattleLoss&, const BattleLoss&) = default;
};

struct BattleProduction {
    PlayerId owner{};
    TypeId type{};
    std::uint64_t tick{};
    friend bool operator==(const BattleProduction&, const BattleProduction&) = default;
};

// WPR-30/31: accepted economy actions, retained for presentation across render stalls.
// These notifications do not participate in canonical bytes or replay hashes.
struct BattleEconomyCue {
    enum class Kind : std::uint8_t { started, cancelled, unit_cap, arrival };
    PlayerId owner{};
    TypeId type{};
    std::uint64_t tick{};
    Kind kind{};
    math::Vec3 position{}; // frame-35 position/visibility for an arrival, even after a render stall
    std::uint64_t visible_to{};
    friend bool operator==(const BattleEconomyCue&, const BattleEconomyCue&) = default;
};

// WSL-29: copied presentation timer; excluded from canonical bytes/hashes.
struct GarrisonTimerView {
    PlayerId player{};
    std::uint32_t duration_frames{};
    std::optional<std::uint64_t> due_frame;
    bool operator==(const GarrisonTimerView&) const = default;
};

class TacticalSnapshot final {
    friend struct checkpoint_detail::Access;
public:
    TacticalSnapshot(
        std::uint64_t completed_tick,
        std::vector<SnapshotPlayer> players,
        std::vector<TacticalInstance> instances,
        std::vector<Event> events,
        std::vector<CombatEvent> combat_events = {},
        std::vector<Projectile> projectiles = {},
        std::optional<BattleOutcome> outcome = std::nullopt,
        std::vector<SpinningCraft> spinning = {},
        std::vector<SquadronTarget> squadron_targets = {},
        std::vector<Squadron> squadrons = {},
        std::vector<EconomyView> economy = {},
        std::vector<PadView> pads = {},
        std::vector<PlayerQuit> quits = {},
        std::shared_ptr<const std::vector<BattleLoss>> losses = {},
        std::shared_ptr<const std::vector<BattleProduction>> productions = {},
        std::vector<std::pair<PlayerId, ManualPlayerClock>> manual_clocks = {},
        std::shared_ptr<const std::vector<BattleEconomyCue>> economy_cues = {},
        std::vector<AbilitySpawnState> ability_spawns = {},
        std::vector<RetreatView> retreat_views = {},
        std::vector<GarrisonTimerView> garrison_timers = {},
        std::vector<BeaconState> beacons = {},
        std::optional<BattleControllerPolicy> controllers = std::nullopt);

    [[nodiscard]] const std::optional<BattleControllerPolicy>& controller_policy() const noexcept { return controllers_; }
    [[nodiscard]] std::span<const RetreatView> retreat_views() const noexcept { return retreat_views_; }
    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    [[nodiscard]] std::span<const SnapshotPlayer> players() const noexcept;
    [[nodiscard]] std::span<const TacticalInstance> instances() const noexcept;
    [[nodiscard]] std::span<const Event> events() const noexcept;
    [[nodiscard]] std::span<const CombatEvent> combat_events() const noexcept;
    [[nodiscard]] std::span<const Projectile> projectiles() const noexcept;
    [[nodiscard]] std::span<const AbilitySpawnState> ability_spawns() const noexcept;
    // HCB-12c: only the observer's allies, including in unrevealed map regions.
    [[nodiscard]] std::vector<BeaconState> visible_beacons(PlayerId observer) const;
    [[nodiscard]] std::span<const GarrisonTimerView> garrison_timers() const noexcept;
    [[nodiscard]] const std::optional<BattleOutcome>& outcome() const noexcept;
    // Killed craft spinning away after the tick (#447), ascending ID.
    [[nodiscard]] std::span<const SpinningCraft> spinning() const noexcept;
    // #424: the squadrons with a target, ascending squadron ID. Presentation only: not part of
    // canonical_bytes(), so it moves no snapshot hash.
    [[nodiscard]] std::span<const SquadronTarget> squadron_targets() const noexcept;
    // #518: the live squadrons, ascending container ID: the setup's and those a spawner launched
    // since (FL-07). Presentation only, like squadron_targets(): not part of canonical_bytes().
    [[nodiscard]] std::span<const Squadron> squadrons() const noexcept;
    // #530: each economy player's credits, population, queues and pool, ascending player ID; empty
    // without economy rules. Presentation only: the state hash carries the economy.
    [[nodiscard]] std::span<const EconomyView> economy() const noexcept;
    [[nodiscard]] std::span<const PadView> pads() const noexcept;
    // WBF-48: durable intentional-quit status, ascending player ID, for results/scoring.
    [[nodiscard]] std::span<const PlayerQuit> quits() const noexcept;
    [[nodiscard]] std::span<const BattleLoss> losses() const noexcept;
    [[nodiscard]] std::span<const BattleProduction> productions() const noexcept;
    [[nodiscard]] std::span<const std::pair<PlayerId, ManualPlayerClock>> manual_clocks() const noexcept;
    [[nodiscard]] std::span<const BattleEconomyCue> economy_cues() const noexcept;
    // Ascending IDs of the instances `player` sees; empty for an unknown player.
    [[nodiscard]] std::vector<EntityId> visible_entities(PlayerId player) const;
    [[nodiscard]] std::vector<std::uint8_t> canonical_bytes() const;
    [[nodiscard]] std::string sha256() const;

private:
    std::vector<RetreatView> retreat_views_;
    std::uint64_t completed_tick_{};
    std::vector<SnapshotPlayer> players_;
    std::vector<TacticalInstance> instances_;
    std::vector<Event> events_;
    std::vector<CombatEvent> combat_events_;
    std::vector<Projectile> projectiles_;
    std::vector<AbilitySpawnState> ability_spawns_;
    std::vector<BeaconState> beacons_;
    std::optional<BattleControllerPolicy> controllers_;
    std::vector<GarrisonTimerView> garrison_timers_;
    std::optional<BattleOutcome> outcome_;
    std::vector<SpinningCraft> spinning_;
    std::vector<SquadronTarget> squadron_targets_;
    std::vector<Squadron> squadrons_;
    std::vector<EconomyView> economy_;
    std::vector<PadView> pads_;
    std::vector<PlayerQuit> quits_;
    std::shared_ptr<const std::vector<BattleLoss>> losses_;
    std::shared_ptr<const std::vector<BattleProduction>> productions_;
    std::vector<std::pair<PlayerId, ManualPlayerClock>> manual_clocks_;
    std::shared_ptr<const std::vector<BattleEconomyCue>> economy_cues_;
};

} // namespace eawr::sim::tactical

namespace eawr::sim::tactical {
// WSW-13/16: planetary target admission is independent of permission, recharge,
// range and visibility. A non-allied neutral target is admitted by this route.
[[nodiscard]] RejectReason planetary_special_fire_target_admission(
    const TacticalSnapshot& snapshot, EntityId source, EntityId target) noexcept;
[[nodiscard]] RejectReason planetary_special_fire_target_admission(
    std::span<const SnapshotPlayer> players, const TacticalInstance& source, const TacticalInstance& target) noexcept;
// WSW-13/40: returns attack_hull for an admitted planetary target, or the first
// authored manual gun's hardpoint. Independent of player permission/HUD cooldown;
// require_gun_ready applies to the ordinary manual gun route only.
[[nodiscard]] core::Result<std::optional<std::uint32_t>> manual_special_fire_admission(
    const TacticalSnapshot& snapshot, EntityId source, EntityId target, bool require_gun_ready = true);
[[nodiscard]] core::Result<std::optional<std::uint32_t>> manual_special_fire_admission(
    std::span<const SnapshotPlayer> players, const TacticalInstance& source, const TacticalInstance& target,
    bool require_gun_ready = true, std::optional<std::uint32_t> hardpoint = std::nullopt);
}
