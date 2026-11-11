#include "../../../src/presentation/animation/land_locomotion_clips.hpp"

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
    return 0;
}
