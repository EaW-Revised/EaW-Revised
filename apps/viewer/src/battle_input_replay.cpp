#include "battle_input.hpp"

#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world2d.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;

namespace {
// #665: the centre of the unit's collision-mesh triangle nearest its box centre, in world space.
[[nodiscard]] ui::Vec3f mesh_aim_point(const ui::BattleUnit& unit) {
    const auto& box = unit.box;
    const ui::Vec3f middle{0.5F * (box.low[0] + box.high[0]), 0.5F * (box.low[1] + box.high[1]),
                           0.5F * (box.low[2] + box.high[2])};
    ui::Vec3f best{};
    float best_distance = std::numeric_limits<float>::infinity();
    for (const auto& triangle : unit.mesh->triangles) {
        ui::Vec3f centre{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            centre[axis] = (triangle[0][axis] + triangle[1][axis] + triangle[2][axis]) / 3.0F;
        }
        const float dx = centre[0] - middle[0];
        const float dy = centre[1] - middle[1];
        const float dz = centre[2] - middle[2];
        const float distance = dx * dx + dy * dy + dz * dz;
        if (distance < best_distance) {
            best_distance = distance;
            best = centre;
        }
    }
    const auto& m = box.model_to_world;
    return {m[0] * best[0] + m[1] * best[1] + m[2] * best[2] + m[3], m[4] * best[0] + m[5] * best[1] + m[6] * best[2] + m[7],
            m[8] * best[0] + m[9] * best[1] + m[10] * best[2] + m[11]};
}

} // namespace

void BattleInput::replay(const LiveSessionView::ScriptedInput& scripted) {
    Input* engine = Input::get_singleton();
    if (engine == nullptr) return;
    const auto modifiers = [&](InputEventWithModifiers& event) {
        event.set_shift_pressed(scripted.shift);
        event.set_ctrl_pressed(scripted.ctrl);
        event.set_alt_pressed(scripted.alt);
    };
    const std::string name = "scripted " + scripted.kind
        + (scripted.frame ? " at frame " + std::to_string(*scripted.frame) : " at tick " + std::to_string(scripted.tick));
    if (scripted.kind == "wheel") {
        for (const bool pressed : {true, false}) {
            Ref<InputEventMouseButton> event;
            event.instantiate();
            event->set_button_index(MOUSE_BUTTON_WHEEL_DOWN);
            event->set_pressed(pressed);
            event->set_factor(1.0F);
            event->set_position(Vector2(viewport_[0] * 0.5F, viewport_[1] * 0.5F));
            event->set_global_position(event->get_position());
            engine->parse_input_event(event);
        }
        overview_sample_pending_ = scripted.tick;
        ++scripted_fired_;
        return;
    }
    if (scripted.kind == "mdrag" || scripted.kind == "mclick") {
        replay_middle(scripted, name);
        return;
    }
    if (scripted.kind == "key") {
        const Key code = OS::get_singleton()->find_keycode_from_string(String(scripted.key.c_str()));
        if (code == KEY_NONE) {
            note(name + ": unknown key " + scripted.key);
            return;
        }
        for (const bool pressed : {true, false}) {
            Ref<InputEventKey> event;
            event.instantiate();
            event->set_keycode(code);
            event->set_physical_keycode(code);
            event->set_pressed(pressed);
            modifiers(**event);
            engine->parse_input_event(event);
        }
        if (code == KEY_INSERT) overview_sample_pending_ = scripted.tick;
        ++scripted_fired_;
        return;
    }
    const auto screen_of = [&](const std::size_t index) -> std::optional<std::array<float, 2>> {
        if (scripted.unit && scripted.icon) return world_ui_->icon_centre(*scripted.unit);
        if (scripted.unit && scripted.reticle) return world_ui_->reticle_centre(*scripted.unit, *scripted.reticle);
        if (scripted.card) return card_point_ ? card_point_(*scripted.card) : std::nullopt;
        if (scripted.ability) return ability_point_ ? ability_point_(*scripted.ability) : std::nullopt;
        if (!scripted.hud.empty()) return hud_point_ ? hud_point_(scripted.hud) : std::nullopt;
        if (scripted.unit) {
            // A craft's own pick volume, or the unit's (a squadron's first craft).
            const auto unit = std::find_if(units_.begin(), units_.end(), [&](const ui::BattleUnit& candidate) {
                return candidate.part == *scripted.unit || candidate.entity == *scripted.unit;
            });
            if (unit == units_.end()) return std::nullopt;
            if (scripted.offset && !scripted.points.empty()) {
                const auto& offset = scripted.points.front();
                return project({unit->position[0] + static_cast<float>(offset[0]),
                                unit->position[1] + static_cast<float>(offset[1]),
                                unit->position[2] + static_cast<float>(offset[2])});
            }
            // #665: a hand clicks on the model, and a box centre can fall in the open space of a
            // hull like the Nebulon-B's, so a unit with a collision mesh is aimed at on it.
            if (unit->mesh != nullptr && !unit->mesh->triangles.empty()) return project(mesh_aim_point(*unit));
            return unit->screen;
        }
        if (index >= scripted.points.size()) return std::nullopt;
        const auto& point = scripted.points[index];
        if (scripted.minimap) return minimap_point_ ? minimap_point_(point[0], point[1]) : std::nullopt;
        if (scripted.screen) return std::array<float, 2>{static_cast<float>(point[0]), static_cast<float>(point[1])};
        return project({static_cast<float>(point[0]), static_cast<float>(point[1]), static_cast<float>(point[2])});
    };
    const auto at = screen_of(0);
    if (!at) {
        note(name + ": its target is not on screen");
        return;
    }
    scripted_points_.push_back({scripted.kind, scripted.tick, *at});
    const auto button = [&](const MouseButton index, const bool pressed, const std::array<float, 2>& where,
                            const bool double_click) {
        Ref<InputEventMouseButton> event;
        event.instantiate();
        event->set_button_index(index);
        event->set_pressed(pressed);
        event->set_double_click(double_click);
        event->set_position(Vector2(where[0], where[1]));
        event->set_global_position(Vector2(where[0], where[1]));
        modifiers(**event);
        engine->parse_input_event(event);
    };
    // #459, #453: a hand reaches a HUD button before it clicks; Godot's GUI only takes the click
    // once the pointer has moved over the window.
    if (!scripted.hud.empty() && scripted.kind != "hover") {
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(Vector2((*at)[0], (*at)[1]));
        motion->set_global_position(Vector2((*at)[0], (*at)[1]));
        engine->parse_input_event(motion);
    }
    if (scripted.kind == "hover") {
        // The pointer moves there and stays: a motion event, no button.
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(Vector2((*at)[0], (*at)[1]));
        motion->set_global_position(Vector2((*at)[0], (*at)[1]));
        modifiers(**motion);
        engine->parse_input_event(motion);
    } else if (scripted.kind == "press" || scripted.kind == "release") {
        button(MOUSE_BUTTON_LEFT, scripted.kind == "press", *at, false);
    } else if (scripted.kind == "click" || scripted.kind == "rclick") {
        const MouseButton index = scripted.kind == "click" ? MOUSE_BUTTON_LEFT : MOUSE_BUTTON_RIGHT;
        button(index, true, *at, false);
        button(index, false, *at, false);
    } else if (scripted.kind == "dclick" || scripted.kind == "rdclick") {
        const auto index = scripted.kind == "dclick" ? MOUSE_BUTTON_LEFT : MOUSE_BUTTON_RIGHT;
        button(index, true, *at, false);
        button(index, false, *at, false);
        button(index, true, *at, true);
        button(index, false, *at, false);
    } else if (scripted.kind == "box") {
        const auto to = screen_of(1);
        if (!to) {
            note(name + ": its corner is not on screen");
            return;
        }
        scripted_points_.back().to = *to;
        button(MOUSE_BUTTON_LEFT, true, *at, false);
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(Vector2((*to)[0], (*to)[1]));
        motion->set_global_position(Vector2((*to)[0], (*to)[1]));
        motion->set_relative(Vector2((*to)[0] - (*at)[0], (*to)[1] - (*at)[1]));
        motion->set_button_mask(MOUSE_BUTTON_MASK_LEFT);
        modifiers(**motion);
        engine->parse_input_event(motion);
        button(MOUSE_BUTTON_LEFT, false, *to, false);
    }
    ++scripted_fired_;
}

void BattleInput::replay_middle(const LiveSessionView::ScriptedInput& scripted, const std::string& name) {
    Input* engine = Input::get_singleton();
    if (engine == nullptr) return;
    if (!frame_ || viewport_[0] <= 0.0F || viewport_[1] <= 0.0F) {
        note(name + ": no drawn camera yet");
        return;
    }
    // The events a hand makes, in the order the platform sends them: Ctrl's own key press and an
    // auto-repeat before the button, the button, the motion in steps (button held, Ctrl held),
    // the button's release and then Ctrl's release, which no longer carries the Ctrl bit.
    const auto ctrl_key = [&](const bool pressed, const bool echo) {
        Ref<InputEventKey> event;
        event.instantiate();
        event->set_keycode(KEY_CTRL);
        event->set_physical_keycode(KEY_CTRL);
        event->set_pressed(pressed);
        event->set_echo(echo);
        event->set_ctrl_pressed(pressed);
        engine->parse_input_event(event);
    };
    Vector2 at(viewport_[0] * 0.5F, viewport_[1] * 0.5F);
    const auto button = [&](const bool pressed) {
        Ref<InputEventMouseButton> event;
        event.instantiate();
        event->set_button_index(MOUSE_BUTTON_MIDDLE);
        event->set_pressed(pressed);
        event->set_button_mask(BitField<MouseButtonMask>(pressed ? MOUSE_BUTTON_MASK_MIDDLE : 0));
        event->set_position(at);
        event->set_global_position(at);
        event->set_ctrl_pressed(scripted.ctrl);
        engine->parse_input_event(event);
    };
    camera_samples_.push_back({name + " before", *frame_});
    if (scripted.ctrl) {
        ctrl_key(true, false);
        ctrl_key(true, true);
    }
    button(true);
    constexpr int steps = 8;
    const Vector2 step(static_cast<float>(scripted.drag[0]) / steps, static_cast<float>(scripted.drag[1]) / steps);
    for (int index = 0; scripted.kind == "mdrag" && index < steps; ++index) {
        at += step;
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(at);
        motion->set_global_position(at);
        motion->set_relative(step);
        motion->set_screen_relative(step);
        motion->set_button_mask(MOUSE_BUTTON_MASK_MIDDLE);
        motion->set_ctrl_pressed(scripted.ctrl);
        engine->parse_input_event(motion);
        if (scripted.ctrl && index == steps / 2) ctrl_key(true, true);
    }
    button(false);
    if (scripted.ctrl) ctrl_key(false, false);
    camera_sample_pending_ = name;
    ++scripted_fired_;
}

} // namespace eawr::presentation::godot_backend
