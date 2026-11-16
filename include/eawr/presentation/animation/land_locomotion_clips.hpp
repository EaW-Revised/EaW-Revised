#pragma once

#include <cstdint>

namespace eawr::presentation::animation {

// WLAN-01..06, 10..12. The caller supplies published motion and authored values.
enum class LandClip { idle, move, walk, crouch, start, stop, turn_left, turn_right,
    turn_left_quarter, turn_right_quarter, turn_left_half, turn_right_half,
    turn_left_begin, turn_right_begin, turn_left_end, turn_right_end, attack, attack_idle };
enum class LandMotion { stopped, starting, moving, turning, stopping };
enum class LandStartFrame { random, zero, carried, normalized };
enum class LandTurnPhase { main, begin, end };
inline constexpr std::uint32_t land_no_shot = 0xFFFFFFFFu;
// How long after its last shot a stationary member keeps its attack stance (ATTACKIDLE), in 30 Hz ticks. A seam: the
// retail stance ends with the target, which the land snapshot does not publish.
inline constexpr std::uint32_t land_attack_hold_ticks = 90;
struct LandClipInputs {
    LandMotion motion{LandMotion::stopped};
    LandClip active{LandClip::idle};
    LandTurnPhase turn_phase{LandTurnPhase::main};
    float speed{}, maximum_speed{}, turn_fraction{}, heading_degrees{}, turn_duration{};
    float move_speed{}, walk_speed{}, crouch_speed{}, rotation_speed{}, stopped_turn_rate{};
    float cutoff{}, move_blend{}, crouch_blend{}, squad_move_blend{}, squad_crouch_blend{};
    float infantry_turn_blend{}, idle_walk_blend{}, crouch_idle_walk_blend{};
    float cycle_phase{}; // normalized movement position for the stop-region choice
    std::uint32_t stop_clip_count{};
    bool bike{}, walk_transition{}, pre_turn{}, spread_out{}, team{}, has_walk{}, has_crouch{}, has_turn{}, blending{}, stop_point{};
    // UA-04 on land (presentation only): ticks since this member's weapon last fired (land_no_shot: never), whether its
    // ATTACK clip has played to its end, which attack clips its model has, and the blend into ATTACK and ATTACKIDLE.
    std::uint32_t ticks_since_shot{land_no_shot};
    float attack_blend{}, attack_idle_blend{};
    bool attack_finished{}, has_attack{}, has_attack_idle{};
};
struct LandClipChoice {
    LandClip kind{LandClip::idle};
    float rate{1}, blend_seconds{};
    LandStartFrame start_frame{LandStartFrame::random};
    bool loop{true};
    std::uint32_t stop_index{}; // zero-based MOVE_END variant
};
[[nodiscard]] LandClipChoice select_land_clip(const LandClipInputs& input) noexcept;
// #6976 B: the speed the clip choice and its rate read: the squad's (steady) speed while the squad moves, else the member's own
// (no squad, or a halted squad whose members still walk to their slots).
[[nodiscard]] float land_clip_speed(float member_speed, float team_speed, bool has_team_speed) noexcept;
[[nodiscard]] float land_bike_roll(float current, float rate, float limit, LandClip active) noexcept;
// D-9: a presentation estimate from committed turns per tick and maximum turns per second.
[[nodiscard]] float land_turn_fraction(float facing_delta, float turn_rate_limit, float ticks_per_second) noexcept;
} // namespace eawr::presentation::animation
