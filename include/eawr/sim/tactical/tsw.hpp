#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The tactical super weapon (the Death Star's approach, ready and fire sequence), docs/behaviour/space-tactical-superweapon.md.
// This header holds the engine-free data and the pure per-frame math; the session service (src/sim/tactical/session_step_tsw.cpp)
// only wires them to the tick.
namespace eawr::sim::tactical {

// The object's state machine, in the behaviour doc's order (TSW-02). idle .. ready are the approach (TSW-05);
// powering_up .. complete are the fire sequence (TSW-08..TSW-11).
enum class TswPhase : std::uint8_t {
    idle = 0,
    facing = 1,
    approaching = 2,
    ready = 3,
    powering_up = 4,
    charged = 5,
    attack_start = 6,
    attack_anim = 7,
    target_destruction = 8,
    debris_wait = 9,
    post_wait = 10,
    complete = 11,
};
inline constexpr TswPhase tsw_last_serviced_phase = TswPhase::complete;

// The phases in which the battle's victory tests are deferred (TSW-09): from the power-up entry until the frame that
// takes the complete phase back to idle, which releases the hold.
[[nodiscard]] constexpr bool tsw_holds_victory(const TswPhase phase) noexcept {
    return phase >= TswPhase::powering_up && phase <= TswPhase::complete;
}

// The ETA warnings, seconds before arrival (TSW-06): the speech thresholds, descending. Bit i of TswState::speech_mask
// is set once threshold i has been announced.
inline constexpr std::uint32_t tsw_speech_thresholds[] = {60, 30, 15, 5};
inline constexpr std::uint32_t tsw_speech_threshold_count = 4;

inline constexpr std::uint32_t tsw_max_targets = 256;

// One DUMMY_TACTICAL_SUPER_WEAPON_TARGET placement, from the map (TSW-03). `faction` is the faction its editor owner
// names (zero when the editor owner maps to no faction: such a target is nobody's own and counts as an enemy).
struct TswTargetSetup {
    std::uint32_t record{};
    FactionId faction{};
    TypeId type{};
    math::Vec3 position{};
    math::Fixed yaw_degrees{};
    friend constexpr bool operator==(const TswTargetSetup&, const TswTargetSetup&) noexcept = default;
};

// The conflict facts the gate needs (TSW-04). Absent = no parent conflict (a skirmish): the gate is shut.
// TODO(#6952): the campaign builder fills these from the conflict record once the galactic side supplies them.
struct TswConflictSetup {
    PlayerId defender{}; // the player that holds the planet; its object starts in range (TSW-03), zero: none
    bool planet_destroyable{};
    bool planet_destroyed{};
    friend constexpr bool operator==(const TswConflictSetup&, const TswConflictSetup&) noexcept = default;
};

// Present only in a battle that has a tactical super weapon object; the replay and checkpoint bytes of every other
// battle are unchanged.
struct TswSetup {
    std::vector<TswTargetSetup> targets; // record order; the state names one by its 1-based position here
    std::optional<TswConflictSetup> conflict;
    friend bool operator==(const TswSetup&, const TswSetup&) = default;
};

// A type's tactical super weapon numbers (TSW-01..TSW-05, all from the object's XML; absent tags are zero, TSW-12).
// The four frame counts belong to the sequence slice and are carried, validated and hashed here.
struct TswProfile {
    math::Fixed attack_distance{}; // TSW_Attack_Distance_From_Target: the approach stops, the ETA ends, at this distance
    math::Fixed max_speed{};       // Max_Speed, units per frame
    math::Fixed max_thrust{};      // Max_Thrust, units per frame per frame
    std::uint32_t power_up_frames{};
    std::uint32_t attack_anim_frames{};
    std::uint32_t debris_delay_frames{};
    std::uint32_t post_destruction_wait_frames{};
    friend constexpr bool operator==(const TswProfile&, const TswProfile&) noexcept = default;
};

// One object's record (TSW-02). `target` is the 1-based index into TswSetup::targets, zero for none.
struct TswState {
    TswPhase phase{TswPhase::idle};
    bool serviced{}; // the first service (TSW-03) ran
    std::uint32_t target{};
    math::Fixed speed{};
    std::uint32_t countdown{}; // frames left in a timed phase (the sequence slice)
    std::uint32_t speech_mask{};
    friend constexpr bool operator==(const TswState&, const TswState&) noexcept = default;
};

// The battle-wide facts of the fire sequence (TSW-09, TSW-10), saved and hashed with the records.
// `victory_hold` counts the objects that are between the power-up entry and the end of the sequence; while it is above
// zero the battle's victory tests are recorded in `deferred` instead of being applied. `destroyed_targets` lists the
// 1-based target indices already destroyed, ascending; `planet_destroyed` is the fact the galactic side commits.
enum class TswDeferredKind : std::uint8_t { unit = 1, leader = 2, station = 3 };
struct TswDeferredVictory {
    TswDeferredKind kind{TswDeferredKind::unit};
    PlayerId owner{};
    EntityId unit{};
    friend constexpr bool operator==(const TswDeferredVictory&, const TswDeferredVictory&) noexcept = default;
};
struct TswWorld {
    std::uint32_t victory_hold{};
    bool planet_destroyed{};
    std::vector<std::uint32_t> destroyed_targets;
    std::vector<TswDeferredVictory> deferred;
    friend bool operator==(const TswWorld&, const TswWorld&) = default;
};

// TSW-13: what the switch presenter reads from the published snapshot, one row per object with the behaviour, ascending
// entity id. A copy of committed state, not state: it is not part of the snapshot's canonical bytes and moves no hash.
// `eta_frames` is the frames to arrival (TSW-06): zero when ready or in range, `no_eta` for an object that cannot move
// or has no target. `has_target` is false while idle with nothing to approach and after the sequence. `gate_open` is
// the TSW-04 gate including the battle's own planet fact (TSW-10); it is what the request admission would use.
struct TswView {
    static constexpr std::uint64_t no_eta = ~std::uint64_t{0};
    EntityId unit{};
    PlayerId owner{};
    TswPhase phase{TswPhase::idle};
    bool serviced{};
    bool has_target{};
    bool gate_open{};
    std::uint64_t eta_frames{no_eta};
    friend constexpr bool operator==(const TswView&, const TswView&) noexcept = default;
};

// TSW-04: both requests are refused unless the conflict exists, its planet stands and may be destroyed.
[[nodiscard]] constexpr bool tsw_gate_open(const std::optional<TswConflictSetup>& conflict) noexcept {
    return conflict && conflict->planet_destroyable && !conflict->planet_destroyed;
}

// TSW-03: the nearest target (squared distance, the lowest index on a tie) whose faction is not `own_faction`.
// Returns its 1-based index, zero when none qualifies. A zero own_faction skips nothing. `destroyed` lists the 1-based
// indices already destroyed (TSW-10); they never qualify.
[[nodiscard]] core::Result<std::uint32_t> tsw_select_target(const TswSetup& setup, FactionId own_faction,
    const math::Vec3& from, std::span<const std::uint32_t> destroyed = {});

// TSW-05: one approach frame. The speed rises by max_thrust, capped at max_speed, then the object moves that far
// along the line to the target, set directly (no collision). `in_range` is the stop test, made before moving: the
// object is at or inside the attack distance, so it does not move that frame.
struct TswApproachStep {
    math::Vec3 position{};
    math::Fixed speed{};
    bool in_range{};
};
[[nodiscard]] core::Result<TswApproachStep> tsw_approach_step(
    const TswProfile& profile, const math::Vec3& position, math::Fixed speed, const math::Vec3& target);

// TSW-05: the ready phase's deceleration, one frame: the speed falls by max_thrust, to zero.
[[nodiscard]] math::Fixed tsw_decelerate(const TswProfile& profile, math::Fixed speed) noexcept;

// TSW-05: one ready frame. The speed falls by max_thrust (to zero) and the object still moves that far along the line
// to its target while the speed is above zero; it does not turn.
[[nodiscard]] core::Result<TswApproachStep> tsw_ready_step(
    const TswProfile& profile, const math::Vec3& position, math::Fixed speed, const math::Vec3& target);

// TSW-05: the facing state's one-frame turn: the rotation whose forward axis (+X) points from `from` at `to`, full 3D
// (heading and elevation, no roll). None when the two points coincide.
[[nodiscard]] core::Result<std::optional<math::Quat>> tsw_facing(const math::Vec3& from, const math::Vec3& to);

// TSW-06: frames to arrival, the distance still to fly to the attack distance over max_speed (truncated), zero when
// in range or when max_speed is zero.
[[nodiscard]] core::Result<std::uint64_t> tsw_eta_frames(
    const TswProfile& profile, const math::Vec3& position, const math::Vec3& target);

// The ETA warnings to announce this frame: thresholds (descending) not yet in `speech_mask` whose time is reached.
// Returns the updated mask; `announce` receives the crossed thresholds in seconds.
[[nodiscard]] std::uint32_t tsw_speech_crossings(
    std::uint64_t eta_frames, std::uint32_t speech_mask, std::vector<std::uint32_t>& announce);

// TSW-07: the admission of one request against the object's current phase: start_approach needs the idle phase,
// start_power_up the ready phase; both need the gate. Returns true when the request is accepted.
[[nodiscard]] constexpr bool tsw_request_accepted(
    const TswStage stage, const TswPhase phase, const bool gate_open) noexcept {
    if (!gate_open) return false;
    return stage == TswStage::start_approach ? phase == TswPhase::idle : phase == TswPhase::ready;
}

// TSW-08..TSW-11: one service frame of the fire sequence, a pure function of the phase and its countdown. A timed phase
// lowers its countdown by one each frame and moves on in the frame that reaches zero (a zero countdown moves on in its
// first frame); the other phases last one frame. `action` is what that frame does besides changing phase.
enum class TswAction : std::uint8_t { none, attack_started, destroy_target, create_debris, finish };
struct TswSequenceStep {
    TswPhase phase{};
    std::uint32_t countdown{};
    TswAction action{TswAction::none};
};
[[nodiscard]] constexpr TswSequenceStep tsw_sequence_step(
    const TswProfile& profile, const TswPhase phase, std::uint32_t countdown) noexcept {
    const auto timed = [&](const TswPhase next) {
        if (countdown > 0) --countdown;
        return countdown == 0 ? next : phase;
    };
    switch (phase) {
    case TswPhase::powering_up: { const auto next = timed(TswPhase::charged); return {next, countdown, TswAction::none}; }
    case TswPhase::charged: return {TswPhase::attack_start, 0, TswAction::none};
    case TswPhase::attack_start: return {TswPhase::attack_anim, profile.attack_anim_frames, TswAction::attack_started};
    case TswPhase::attack_anim: { const auto next = timed(TswPhase::target_destruction); return {next, countdown, TswAction::none}; }
    case TswPhase::target_destruction: return {TswPhase::debris_wait, profile.debris_delay_frames, TswAction::destroy_target};
    case TswPhase::debris_wait: {
        const auto next = timed(TswPhase::post_wait);
        if (next == TswPhase::post_wait) return {next, profile.post_destruction_wait_frames, TswAction::create_debris};
        return {next, countdown, TswAction::none};
    }
    case TswPhase::post_wait: { const auto next = timed(TswPhase::complete); return {next, countdown, TswAction::none}; }
    case TswPhase::complete: return {TswPhase::idle, 0, TswAction::finish};
    default: return {phase, countdown, TswAction::none};
    }
}

} // namespace eawr::sim::tactical
