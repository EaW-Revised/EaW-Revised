#include "eawr/presentation/animation/land_locomotion_clips.hpp"

#include <algorithm>
#include <cmath>

namespace eawr::presentation::animation {
namespace {
float movement_rate(const LandClipInputs& i, float authored) noexcept {
    const float fraction = i.maximum_speed > 0 ? i.speed / i.maximum_speed : 0;
    const float multiplier = authored != 0 ? i.maximum_speed / authored : 1;
    return std::max(fraction, i.turn_fraction) * multiplier;
}
}
LandClipChoice select_land_clip(const LandClipInputs& i) noexcept {
    LandClipChoice result;
    if (i.bike) {
        result.blend_seconds = 0.5F;
        result.start_frame = LandStartFrame::carried;
        if (i.motion == LandMotion::starting) return {LandClip::start, 1, 0.5F, LandStartFrame::zero, false};
        if (i.motion == LandMotion::stopping && i.speed < 0.55F * i.maximum_speed)
            return {LandClip::stop, 1, 0.5F, LandStartFrame::zero, false};
        if (i.motion != LandMotion::moving && i.motion != LandMotion::stopping && i.motion != LandMotion::turning) return {};
        const float threshold = i.motion == LandMotion::turning ? 0.0F : 10.0F;
        const auto desired = i.heading_degrees < -threshold ? LandClip::turn_left :
            i.heading_degrees > threshold ? LandClip::turn_right :
            i.motion == LandMotion::turning ? i.active : LandClip::move;
        result.kind = (desired == LandClip::turn_left && i.active == LandClip::turn_right) ||
            (desired == LandClip::turn_right && i.active == LandClip::turn_left) ? LandClip::move : desired;
        return result;
    }
    if (i.walk_transition && i.motion == LandMotion::starting)
        return {LandClip::start, i.move_speed != 0 ? i.maximum_speed / i.move_speed : 1, 0.2F, LandStartFrame::zero, false};
    if (i.walk_transition && i.motion == LandMotion::stopping && i.stop_point && i.stop_clip_count != 0) {
        const float phase = std::clamp(i.cycle_phase, 0.0F, 0.999999F);
        const auto index = static_cast<std::uint32_t>(phase * static_cast<float>(i.stop_clip_count));
        return {LandClip::stop, movement_rate(i, i.move_speed), 0.2F, LandStartFrame::carried, false, index};
    }
    if (i.motion == LandMotion::turning && i.has_turn) {
        const bool left = i.turn_phase == LandTurnPhase::end ?
            (i.active == LandClip::turn_left || i.active == LandClip::turn_left_quarter ||
             i.active == LandClip::turn_left_half || i.active == LandClip::turn_left_begin) : i.heading_degrees < 0;
        LandClip clip = left ? LandClip::turn_left : LandClip::turn_right;
        if (i.pre_turn) {
            if (i.turn_phase != LandTurnPhase::main) {
                clip = i.turn_phase == LandTurnPhase::begin ?
                    (left ? LandClip::turn_left_begin : LandClip::turn_right_begin) :
                    (left ? LandClip::turn_left_end : LandClip::turn_right_end);
                return {clip, 1, 0.2F, LandStartFrame::zero, false};
            }
            const float angle = std::abs(i.heading_degrees);
            if (angle < 22.5F) clip = left ? LandClip::turn_left_quarter : LandClip::turn_right_quarter;
            else if (angle < 45) clip = left ? LandClip::turn_left_half : LandClip::turn_right_half;
            const float rate = i.stopped_turn_rate > 0 && i.rotation_speed > 0 ? i.stopped_turn_rate / i.rotation_speed : 1;
            return {clip, rate, 4.0F / 30.0F, LandStartFrame::zero, false};
        }
        return {clip, 1, std::min(i.infantry_turn_blend, i.turn_duration), LandStartFrame::zero, false};
    }
    // UA-04: a member standing still that fired lately plays ATTACK once per shot (0.1 s blend from an attack clip) and
    // otherwise holds ATTACKIDLE (looped); a model without ATTACKIDLE returns to idle when ATTACK has ended.
    if (i.motion == LandMotion::stopped && i.ticks_since_shot <= land_attack_hold_ticks) {
        const bool in_attack = i.active == LandClip::attack, in_stance = i.active == LandClip::attack_idle;
        if (i.has_attack && (i.ticks_since_shot == 0 || (in_attack && !i.attack_finished)))
            return {LandClip::attack, 1, in_attack || in_stance ? 0.1F : i.attack_blend, LandStartFrame::zero, false};
        if (i.has_attack_idle)
            return {LandClip::attack_idle, 1, in_attack ? 0.1F : i.attack_idle_blend, LandStartFrame::random, true};
    }
    if (i.motion == LandMotion::stopped && (i.active == LandClip::attack || i.active == LandClip::attack_idle))
        result.blend_seconds = i.attack_idle_blend; // the stance ends: back to idle over the stance blend
    if (i.motion != LandMotion::moving && i.motion != LandMotion::stopping) return result;
    if (!i.walk_transition && i.speed <= 0 && i.turn_fraction <= 0) return result;
    result.kind = !i.walk_transition && i.spread_out && i.has_crouch ? LandClip::crouch :
        (!i.walk_transition && i.has_walk && i.speed < i.maximum_speed * i.cutoff ? LandClip::walk : LandClip::move);
    if (!i.walk_transition && i.blending &&
        (i.active == LandClip::move || i.active == LandClip::walk || i.active == LandClip::crouch)) result.kind = i.active;
    const float authored = result.kind == LandClip::walk ? i.walk_speed : result.kind == LandClip::crouch ? i.crouch_speed : i.move_speed;
    result.rate = movement_rate(i, authored);
    result.blend_seconds = i.walk_transition ? 0 : result.kind == LandClip::crouch ?
        (i.team ? i.squad_crouch_blend : i.crouch_blend) : (i.team ? i.squad_move_blend : i.move_blend);
    result.start_frame = i.walk_transition ? LandStartFrame::carried :
        (i.active == LandClip::move || i.active == LandClip::walk || i.active == LandClip::crouch ?
            LandStartFrame::normalized : LandStartFrame::random);
    if (!i.walk_transition && i.active == LandClip::idle)
        result.blend_seconds = result.kind == LandClip::crouch ? i.crouch_idle_walk_blend : i.idle_walk_blend;
    return result;
}
float land_clip_speed(const float member_speed, const float team_speed, const bool has_team_speed) noexcept {
    // Only while the squad moves: a halted squad has speed 0 while its members still walk to their slots.
    return has_team_speed && std::isfinite(team_speed) && team_speed > 0.0F ? team_speed : member_speed;
}
float land_bike_roll(float current, float rate, float limit, LandClip active) noexcept {
    rate = std::max(rate, 0.0F);
    limit = std::max(limit, 0.0F);
    const float target = active == LandClip::turn_left ? -limit : active == LandClip::turn_right ? limit : 0;
    return std::clamp(current + std::clamp(target - current, -rate, rate), -limit, limit);
}
float land_turn_fraction(const float facing_delta, const float turn_rate_limit, const float ticks_per_second) noexcept {
    if (!std::isfinite(facing_delta) || !std::isfinite(turn_rate_limit) || !std::isfinite(ticks_per_second)
        || turn_rate_limit <= 0.0F || ticks_per_second <= 0.0F) return 0.0F;
    return std::clamp(std::abs(facing_delta) * ticks_per_second / turn_rate_limit, 0.0F, 1.0F);
}
} // namespace eawr::presentation::animation
