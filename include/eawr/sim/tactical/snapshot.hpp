#pragma once

#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/types.hpp"
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

// One hardpoint of a durable instance (#72): `enabled` is false once it is destroyed, so a
// destroyed weapon hardpoint may not fire (HD-10).
struct HardpointStatus {
    HardpointRole role{HardpointRole::other};
    HardpointState state{HardpointState::intact};
    bool enabled{true};
    math::Fixed health{};
    friend constexpr bool operator==(const HardpointStatus&, const HardpointStatus&) noexcept = default;
};

// Health and its consequences for an instance whose type has a durability profile (#72;
// docs/behaviour/space-hardpoints.md). max_speed is the type's Max_Speed times max_speed_factor.
struct InstanceDurability {
    math::Fixed hull{};
    math::Fixed max_hull{};
    math::Fixed max_speed_factor{};
    std::optional<math::Fixed> max_speed{};
    bool engines_online{};
    bool shields_online{};
    bool launch_ready{};
    std::vector<HardpointStatus> hardpoints; // HardPoints order
    // A shielded unit of a session with damage rules (#74): its shield and maximum.
    std::optional<math::Fixed> shields{};
    std::optional<math::Fixed> max_shields{};
    friend bool operator==(const InstanceDurability&, const InstanceDurability&) = default;
};

// One ability of an instance (#76, docs/behaviour/space-abilities.md AB-50): whether it is on,
// may be switched on (AB-13), is on autofire, and its timer. While a time-limited ability is on,
// `remaining_frames` counts down its duration (`total_frames`, the Expiration_Seconds frames);
// while it recharges, its recharge (`total_frames`, the Recharge_Seconds frames); otherwise both
// are zero. A squadron container has none; its craft carry theirs (AB-15).
struct AbilityStatus {
    AbilityKind kind{AbilityKind::none};
    bool active{};
    bool ready{};
    bool autofire{};
    bool supports_autofire{};
    std::uint32_t remaining_frames{};
    std::uint32_t total_frames{};
    EntityId target{}; // WHE-26/27: presentation only, published from the ordinary tracked slot
    std::uint32_t target_hardpoint{0xffffffffU};
    std::uint64_t started_tick{}; // presentation-only birth of instant wingman effects
    friend constexpr bool operator==(const AbilityStatus&, const AbilityStatus&) noexcept = default;
};

struct ManualWeaponView {
    std::uint32_t hardpoint{};
    EntityId target{};
    PlayerId requesting_player{};
    std::uint64_t assigned_frame{};
    math::Fixed yaw{};
    math::Fixed pitch{};
    friend constexpr bool operator==(const ManualWeaponView&, const ManualWeaponView&) noexcept = default;
};

// visible_to bit k is the k-th snapshot player; reveal_range is the type's sensor profile.
struct TacticalInstance {
    EntityId entity_id{};
    TypeId type_id{};
    PlayerId owner{};
    TeamId team{};
    math::Mat3x4 fixed_transform{};
    std::uint64_t visible_to{};
    std::optional<math::Fixed> reveal_range{};
    std::optional<InstanceDurability> durability{};
    std::vector<AbilityStatus> abilities{}; // #76: the type's abilities, Unit_Abilities_Data order
    // #530 (PU-35 to PU-39): the arrival frames an arriving unit has flown (0 in the frame it was
    // brought in, up to 149); nothing for a unit that is not arriving. Presentation only: not part
    // of canonical_bytes().
    std::optional<std::uint32_t> arrival{};
    // #561: the frames left of its ion stun (IS-03, IS-09 shows it); zero when not stunned.
    std::uint32_t ion_stun_frames{};
    // WSU-34: renderer inputs for the squadron icon's slide and idle-grid braking.
    // Presentation only; neither field participates in canonical_bytes() or hashes.
    std::optional<math::Vec3> craft_velocity_per_frame{};
    std::optional<bool> squadron_in_idle_grid{};
    bool in_asteroid_field{}; // WHZ-13: presence of the recorded contact, without age grace
    bool in_nebula{}; // WHZ-25: cached service, member union, or hard-box fallback
    bool in_ion_storm{}; // WHZ-31: shield service's cached contact, never a zeroed pool
    std::vector<ManualWeaponView> manual_weapons{}; // WAD-39: existing snapshot/command consumer hook
    bool in_tractor_beam{}; // WHE-27: derived from live source-owned tractor entries
    // WU-49: the stationary destination for an arriving unit's world identity.
    // Presentation only, alongside arrival; excluded from canonical_bytes().
    std::optional<math::Vec3> arrival_exit{};
    // SND-46: locomotor path presence for presentation audio, not velocity or idle orbiting.
    // A squadron publishes its members' shared move/approach here. Excluded from canonical bytes.
    bool has_movement_path{};
    friend bool operator==(const TacticalInstance&, const TacticalInstance&) = default;
};

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
    friend constexpr bool operator==(const SnapshotPlayer&, const SnapshotPlayer&) noexcept = default;
};

// WHZ-51: ordinary combat excludes either neutral player and players on the same team.
// Both simulation and presentation use the same immutable player relationship table.
[[nodiscard]] bool players_hostile(std::span<const SnapshotPlayer> players, PlayerId owner, PlayerId target) noexcept;

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

class TacticalSnapshot final {
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
        std::vector<AbilitySpawnState> ability_spawns = {});

    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    [[nodiscard]] std::span<const SnapshotPlayer> players() const noexcept;
    [[nodiscard]] std::span<const TacticalInstance> instances() const noexcept;
    [[nodiscard]] std::span<const Event> events() const noexcept;
    [[nodiscard]] std::span<const CombatEvent> combat_events() const noexcept;
    [[nodiscard]] std::span<const Projectile> projectiles() const noexcept;
    [[nodiscard]] std::span<const AbilitySpawnState> ability_spawns() const noexcept;
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
    std::uint64_t completed_tick_{};
    std::vector<SnapshotPlayer> players_;
    std::vector<TacticalInstance> instances_;
    std::vector<Event> events_;
    std::vector<CombatEvent> combat_events_;
    std::vector<Projectile> projectiles_;
    std::vector<AbilitySpawnState> ability_spawns_;
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
