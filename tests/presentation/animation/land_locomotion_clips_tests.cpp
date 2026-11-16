#include "eawr/presentation/animation/land_locomotion_clips.hpp"

#include <array>
#include <cmath>
#include <iostream>

using namespace eawr::presentation::animation;
int main() {
    struct Example { const char* name; float maximum, authored, rate; bool bike; };
    constexpr std::array examples{
        Example{"Stormtrooper", 0.8F, 0.8F, 1.0F, false},
        Example{"Rebel_Trooper", 1.0F, 0.65F, 1.0F / 0.65F, false},
        Example{"AT-ST", 1.2F, 0.8117F, 1.2F / 0.8117F, false},
        Example{"AT-AT", 0.5F, 0.1366F, 0.5F / 0.1366F, false},
        Example{"bike", 1.0F, 3.0F, 1.0F, true}};
    for (const auto& example : examples) {
        LandClipInputs input;
        input.motion = LandMotion::moving;
        input.maximum_speed = input.speed = example.maximum;
        input.move_speed = example.authored;
        input.bike = example.bike;
        const auto choice = select_land_clip(input);
        if (choice.kind != LandClip::move || std::abs(choice.rate - example.rate) > 0.0001F) {
            std::cerr << example.name << " rate or clip mismatch\n";
            return 1;
        }
    }
    LandClipInputs input;
    input.motion = LandMotion::moving; input.maximum_speed = 1; input.speed = 0.3F;
    input.cutoff = 0.6F; input.has_walk = true; input.walk_speed = 0.366F;
    if (select_land_clip(input).kind != LandClip::walk) return 2;
    input.spread_out = true; input.has_crouch = true;
    if (select_land_clip(input).kind != LandClip::crouch) return 3;
    input.blending = true; input.active = LandClip::walk;
    if (select_land_clip(input).kind != LandClip::walk) return 7;
    input.blending = false;
    input.walk_transition = true;
    if (select_land_clip(input).kind != LandClip::move) return 8;
    input.walk_transition = false;
    input.bike = true; input.spread_out = false; input.heading_degrees = -15;
    input.active = LandClip::turn_right;
    if (select_land_clip(input).kind != LandClip::move) return 4;
    if (land_bike_roll(34, 2, 35, LandClip::turn_right) != 35) return 5;
    if (land_bike_roll(1, 2, 35, LandClip::move) != 0) return 6;
    input = {}; input.motion = LandMotion::turning; input.has_turn = true; input.pre_turn = true;
    input.heading_degrees = -30; input.stopped_turn_rate = 7.5F; input.rotation_speed = 2.5F;
    const auto turn = select_land_clip(input);
    if (turn.kind != LandClip::turn_left_half || turn.rate != 3 || turn.loop) return 9;
    input.turn_phase = LandTurnPhase::end; input.heading_degrees = 10; input.active = LandClip::turn_left_half;
    if (select_land_clip(input).kind != LandClip::turn_left_end) return 10;
    input = {}; input.motion = LandMotion::stopping; input.walk_transition = true;
    input.stop_clip_count = 4; input.cycle_phase = 0.6F;
    if (select_land_clip(input).kind != LandClip::move) return 12;
    input.stop_point = true;
    if (select_land_clip(input).stop_index != 2) return 11;
    input = {}; input.motion = LandMotion::starting; input.walk_transition = true;
    input.maximum_speed = 1.2F; input.move_speed = 0.8F;
    const auto start = select_land_clip(input);
    if (start.kind != LandClip::start || std::abs(start.rate - 1.5F) > 0.0001F || start.loop || start.blend_seconds != 0.2F
        || start.start_frame != LandStartFrame::zero) return 13;
    input.motion = LandMotion::moving; input.speed = input.maximum_speed;
    const auto walk_transition = select_land_clip(input);
    if (walk_transition.kind != LandClip::move || walk_transition.blend_seconds != 0
        || walk_transition.start_frame != LandStartFrame::carried) return 14;
    input = {}; input.bike = true; input.motion = LandMotion::starting;
    const auto bike_start = select_land_clip(input);
    if (bike_start.kind != LandClip::start || bike_start.blend_seconds != 0.5F || bike_start.loop) return 15;
    input.motion = LandMotion::stopping; input.maximum_speed = 1; input.speed = 0.5F;
    const auto bike_stop = select_land_clip(input);
    if (bike_stop.kind != LandClip::stop || bike_stop.blend_seconds != 0.5F || bike_stop.loop
        || bike_stop.start_frame != LandStartFrame::zero) return 16;
    input.motion = LandMotion::moving; input.speed = 1; input.heading_degrees = 15;
    const auto bike_lean = select_land_clip(input);
    if (bike_lean.kind != LandClip::turn_right || bike_lean.rate != 1 || bike_lean.blend_seconds != 0.5F
        || bike_lean.start_frame != LandStartFrame::carried) return 17;
    if (land_turn_fraction(-0.01F, 0.6F, 30.0F) < 0.499F
        || land_turn_fraction(-0.01F, 0.6F, 30.0F) > 0.501F
        || land_turn_fraction(1.0F, 0.6F, 30.0F) != 1.0F
        || land_turn_fraction(1.0F, 0.0F, 30.0F) != 0.0F) return 18;
    // N7: a negative authored rate or limit leaves the roll at zero instead of reaching a clamp with its bounds reversed.
    if (land_bike_roll(0, -2, 35, LandClip::turn_right) != 0 || land_bike_roll(0, 2, -35, LandClip::turn_left) != 0
        || land_bike_roll(5, -2, -35, LandClip::move) != 0) return 19;
    // #6976 B: a member's own speed wobbles around the walk cutoff while its squad moves at a steady speed. Fed the member
    // speed, the choice flips between MOVE and WALK on nearly every step (no blend hold is set here); fed the squad speed,
    // the member speed varying under the steady squad speed, the choice and the clip rate stay the same on every step.
    {
        const float wobble[] = {0.62F, 0.58F, 0.61F, 0.57F, 0.63F, 0.59F, 0.6F, 0.56F, 0.64F, 0.58F};
        const auto flips = [&](const bool use_team) {
            LandClip active = LandClip::move; int count = 0; float first_rate = -1.0F; bool rate_changed = false;
            for (const float own : wobble) {
                LandClipInputs step;
                step.motion = LandMotion::moving; step.maximum_speed = 1.0F; step.cutoff = 0.6F; step.move_speed = 1.0F;
                step.walk_speed = 0.6F; step.has_walk = true; step.team = true; step.squad_move_blend = 0.2F;
                step.speed = land_clip_speed(own, 0.75F, use_team); step.active = active;
                const auto choice = select_land_clip(step);
                if (first_rate < 0.0F) first_rate = choice.rate;
                if (use_team && choice.rate != first_rate) rate_changed = true;
                if (choice.kind != active) ++count;
                active = choice.kind;
            }
            return rate_changed ? -1 : count;
        };
        if (flips(false) < 5) return 31;                           // the wobbling member speed does flip (the bug)
        if (flips(true) != 0) return 32;                           // the steady squad speed does not
        // A halted squad (speed 0) whose member is still walking to its slot: the member's own speed and rate stand, no idle.
        if (land_clip_speed(0.4F, 0.0F, true) != 0.4F) return 34;
        {
            LandClipInputs catching;
            catching.motion = LandMotion::moving; catching.maximum_speed = 1.0F; catching.cutoff = 0.6F; catching.move_speed = 1.0F;
            catching.walk_speed = 0.6F; catching.has_walk = true; catching.team = true; catching.squad_move_blend = 0.2F;
            catching.speed = land_clip_speed(0.4F, 0.0F, true);
            const auto choice = select_land_clip(catching);
            if (choice.kind != LandClip::walk || choice.rate <= 0.0F) return 35;
        }
        if (land_clip_speed(0.3F, 0.7F, false) != 0.3F || land_clip_speed(0.3F, 0.7F, true) != 0.7F) return 33;
    }
    // UA-04 on land: a stationary member that fires plays ATTACK once, then holds ATTACKIDLE, then returns to idle.
    input = {}; input.motion = LandMotion::stopped; input.has_attack = input.has_attack_idle = true;
    input.attack_blend = 0.2F; input.attack_idle_blend = 0.4F;
    if (select_land_clip(input).kind != LandClip::idle) return 20;                       // never fired
    input.ticks_since_shot = 0;
    const auto shot = select_land_clip(input);
    if (shot.kind != LandClip::attack || shot.loop || shot.blend_seconds != 0.2F || shot.start_frame != LandStartFrame::zero) return 21;
    input.active = LandClip::attack; input.ticks_since_shot = 3;
    if (select_land_clip(input).kind != LandClip::attack) return 22;                      // still playing
    input.ticks_since_shot = 0;
    if (select_land_clip(input).blend_seconds != 0.1F) return 23;                         // a new shot from an attack clip
    input.ticks_since_shot = 10; input.attack_finished = true;
    const auto stance = select_land_clip(input);
    if (stance.kind != LandClip::attack_idle || !stance.loop || stance.blend_seconds != 0.1F) return 24;
    input.active = LandClip::attack_idle; input.attack_finished = false; input.ticks_since_shot = land_attack_hold_ticks;
    const auto held = select_land_clip(input);
    if (held.kind != LandClip::attack_idle || held.blend_seconds != 0.4F) return 25;
    input.ticks_since_shot = land_attack_hold_ticks + 1;
    const auto released = select_land_clip(input);
    if (released.kind != LandClip::idle || released.blend_seconds != 0.4F) return 26;      // stance over
    // A model without ATTACKIDLE (a walker): ATTACK, then idle once it has ended.
    input = {}; input.motion = LandMotion::stopped; input.has_attack = true; input.attack_blend = 0.2F; input.ticks_since_shot = 0;
    if (select_land_clip(input).kind != LandClip::attack) return 27;
    input.active = LandClip::attack; input.ticks_since_shot = 20; input.attack_finished = true;
    if (select_land_clip(input).kind != LandClip::idle) return 28;
    // A model with no attack clip, or a member that is moving, keeps its locomotion choice.
    input = {}; input.motion = LandMotion::stopped; input.ticks_since_shot = 0;
    if (select_land_clip(input).kind != LandClip::idle) return 29;
    input = {}; input.motion = LandMotion::moving; input.maximum_speed = input.speed = 1; input.move_speed = 1;
    input.has_attack = input.has_attack_idle = true; input.ticks_since_shot = 0;
    if (select_land_clip(input).kind != LandClip::move) return 30;
    return 0;
}
