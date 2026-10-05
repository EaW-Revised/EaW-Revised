#include "viewer_host_internal.hpp"

#include "ui/input_routing.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>

namespace eawr::presentation::godot_backend {

namespace {

// Synthetic events enter through Input::parse_input_event, i.e. the same
// engine dispatch path as OS input, and reach ViewerHost::_unhandled_input
// next frame.
void inject_key(const std::string_view name, const bool pressed, const bool echo = false) {
    Ref<InputEventKey> event;
    event.instantiate();
    const Key code = OS::get_singleton()->find_keycode_from_string(
        String::utf8(name.data(), static_cast<int64_t>(name.size())));
    event->set_keycode(code);
    event->set_physical_keycode(code);
    event->set_pressed(pressed);
    event->set_echo(echo);
    Input::get_singleton()->parse_input_event(event);
}

void inject_mouse_button(const MouseButton index, const bool pressed, const Vector2 position) {
    Ref<InputEventMouseButton> event;
    event.instantiate();
    event->set_button_index(index);
    event->set_pressed(pressed);
    event->set_factor(1.0F);
    event->set_position(position);
    event->set_global_position(position);
    Input::get_singleton()->parse_input_event(event);
}

void inject_mouse_motion(const Vector2 position, const Vector2 relative) {
    Ref<InputEventMouseMotion> event;
    event.instantiate();
    event->set_position(position);
    event->set_global_position(position);
    event->set_relative(relative);
    Input::get_singleton()->parse_input_event(event);
}

[[nodiscard]] MouseButton godot_button(const std::uint32_t code) {
    return code == camera_input::mouse_code::left
        ? MOUSE_BUTTON_LEFT
        : code == camera_input::mouse_code::right ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_MIDDLE;
}

[[nodiscard]] float dot(const std::array<float, 3>& left, const std::array<float, 3>& right) {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] bool same_camera(const FixedCamera& left, const FixedCamera& right) {
    return left.width == right.width && left.height == right.height
        && left.vertical_fov_degrees == right.vertical_fov_degrees
        && left.near_plane == right.near_plane && left.far_plane == right.far_plane
        && left.eye == right.eye && left.target == right.target && left.up == right.up;
}

// First unmodified binding in `context` for an action and device, used by
// the self-test to drive whatever the project table binds. `sign` optionally
// selects a positive or negative scale.
[[nodiscard]] const camera_input::Binding* find_project_binding(
    const camera_input::Adapter& adapter, const camera_input::Context context,
    const camera_input::Action action, const camera_input::Device device, const int sign = 0) {
    const camera_input::BindingTable* table = adapter.table();
    if (!table) return nullptr;
    for (const camera_input::Binding& binding : table->bindings) {
        if (binding.context != context || binding.action != action
            || binding.device != device || binding.modifiers != 0U) {
            continue;
        }
        if (sign > 0 && !(binding.scale > 0.0F)) continue;
        if (sign < 0 && !(binding.scale < 0.0F)) continue;
        return &binding;
    }
    return nullptr;
}


} // namespace

bool ViewerHost::advance_camera_selftest() {
    CameraInteraction& run = *camera_interaction_;
    camera_input::Adapter& adapter = run.adapter;
    const std::uint64_t step = ++run.selftest_frame;
    const Vector2 extent = get_viewport()->get_visible_rect().size;
    const Vector2 centre = extent * 0.5F;
    const auto check = [&run](const std::string_view name, const bool passed) {
        run.checks.emplace_back(std::string(name), passed);
    };
    const auto propagate = [this](const int what) {
        get_tree()->get_root()->propagate_notification(what);
    };
    // Tactical bindings are looked up in the tactical context even while free
    // flight has switched the adapter to `free`.
    const camera_input::Context tactical =
        run.free_controller ? run.saved_context : adapter.context();
    constexpr camera_input::Context free_context = camera_input::Context::free;
    const camera_input::Binding* pan = find_project_binding(
        adapter, tactical, camera_input::Action::pan_right, camera_input::Device::keyboard);
    const camera_input::Binding* reset = find_project_binding(
        adapter, tactical, camera_input::Action::reset_view, camera_input::Device::keyboard);
    const camera_input::Binding* wheel = find_project_binding(adapter, tactical,
        camera_input::Action::zoom, camera_input::Device::mouse_wheel,
        run.pose.state.zoom < 1.0F ? 1 : -1);
    const camera_input::Binding* grab = find_project_binding(
        adapter, tactical, camera_input::Action::rotate_grab, camera_input::Device::mouse_button);
    const camera_input::Binding* rotate = find_project_binding(
        adapter, tactical, camera_input::Action::rotate, camera_input::Device::mouse_motion);
    // Free-flight bindings. A table without an entry toggle (every v1 table)
    // skips the free phase; one with a toggle must bind the whole phase.
    const camera_input::Binding* enter_toggle = find_project_binding(
        adapter, tactical, camera_input::Action::free_toggle, camera_input::Device::keyboard);
    const camera_input::Binding* exit_toggle = find_project_binding(
        adapter, free_context, camera_input::Action::free_toggle, camera_input::Device::keyboard);
    const camera_input::Binding* fly = find_project_binding(adapter, free_context,
        camera_input::Action::free_move_forward, camera_input::Device::keyboard);
    const camera_input::Binding* rise = find_project_binding(
        adapter, free_context, camera_input::Action::free_rise, camera_input::Device::keyboard);
    const camera_input::Binding* look_grab = find_project_binding(adapter, free_context,
        camera_input::Action::free_look_grab, camera_input::Device::mouse_button);
    const camera_input::Binding* look_yaw = find_project_binding(adapter, free_context,
        camera_input::Action::free_look_yaw, camera_input::Device::mouse_motion);
    if (!pan || !reset || !wheel || !grab || !rotate) {
        status_message_ = "project bindings lack a plain keyboard pan_right, keyboard "
                          "reset_view, mouse-wheel zoom, mouse-button rotate_grab or "
                          "mouse-motion rotate for the self-test";
        return false;
    }
    const std::string pan_key(camera_input::key_name(pan->code));
    const std::string reset_key(camera_input::key_name(reset->code));
    const MouseButton wheel_button = wheel->code == camera_input::mouse_code::wheel_up
        ? MOUSE_BUTTON_WHEEL_UP : MOUSE_BUTTON_WHEEL_DOWN;
    const MouseButton grab_button = godot_button(grab->code);
    const bool free_phase = enter_toggle != nullptr;
    if (free_phase && (!exit_toggle || !fly || !rise || !look_grab || !look_yaw)) {
        status_message_ = "project bindings enter free flight but lack a plain free-context "
                          "keyboard free_toggle, free_move_forward or free_rise, mouse-button "
                          "free_look_grab or mouse-motion free_look_yaw for the self-test";
        return false;
    }
    const std::string enter_key(free_phase ? camera_input::key_name(enter_toggle->code) : "");
    // Drag away from every edge so edge scrolling cannot contribute.
    const Vector2 drag = rotate->code == camera_input::mouse_code::motion_x
        ? Vector2(40.0F, 0.0F) : Vector2(0.0F, 40.0F);

    if (run.capture_locked_for_run) {
        // Hostile batch before and during the fixed capture at frame 8.
        switch (step) {
        case 1:
            run.capture_mark = capture_camera_;
            run.interactive_mark = run.camera;
            inject_key(pan_key, true);
            inject_key(reset_key, true);
            if (free_phase) inject_key(enter_key, true);
            inject_mouse_button(wheel_button, true, centre);
            inject_mouse_button(MOUSE_BUTTON_MIDDLE, true, centre);
            inject_mouse_motion(Vector2(0.0F, 0.0F), Vector2(40.0F, 0.0F));
            break;
        case 3:
            propagate(NOTIFICATION_APPLICATION_FOCUS_OUT);
            propagate(NOTIFICATION_APPLICATION_FOCUS_IN);
            publish_camera_viewport(0.0F, 0.0F);
            publish_camera_viewport(extent.x, extent.y);
            inject_key(pan_key, true, true);
            break;
        case 5:
            inject_key(pan_key, false);
            inject_key(reset_key, false);
            if (free_phase) {
                inject_key(enter_key, false);
                inject_key(enter_key, true);
            }
            inject_mouse_button(MOUSE_BUTTON_MIDDLE, false, centre);
            break;
        case 6:
            if (free_phase) inject_key(enter_key, false);
            break;
        case 7:
            check("synthetic events reached _input", run.input_callbacks > 0U);
            check("capture lock routed no event", adapter.counters().routed == 0U);
            check("capture camera unchanged", same_camera(capture_camera_, run.capture_mark));
            check("interactive camera unchanged", same_camera(run.camera, run.interactive_mark));
            check("no camera step moved", run.moving_steps == 0U);
            if (free_phase) {
                check("capture lock ignored the free-flight toggle",
                      !run.free_controller && run.free_entries == 0U
                          && run.free_transitions.empty() && run.free_steps == 0U
                          && adapter.context() == tactical);
            }
            break;
        default:
            break;
        }
        return true;
    }

    const auto held_right = [&run]() { return run.pose.target[0] > run.mark.target[0]; };
    switch (step) {
    case 1:
        propagate(NOTIFICATION_APPLICATION_FOCUS_IN);
        run.capture_mark = capture_camera_;
        run.mark = run.pose;
        break;
    case 2:
        inject_key(pan_key, true);
        break;
    case 8:
        check("synthetic events reached _input", run.input_callbacks > 0U);
        check("held pan moves +X at yaw 0",
              held_right() && run.pose.target[2] == run.mark.target[2]);
        propagate(NOTIFICATION_APPLICATION_FOCUS_OUT);
        run.mark = run.pose;
        break;
    case 10:
        inject_key(pan_key, false);  // late key-up while unfocused
        break;
    case 14:
        check("focus loss froze the camera", run.pose == run.mark);
        propagate(NOTIFICATION_APPLICATION_FOCUS_IN);
        break;
    case 20:
        check("focus regain resurrected nothing", run.pose == run.mark);
        inject_key(pan_key, true);
        break;
    case 26:
        check("fresh press after regain moves", held_right());
        inject_key(pan_key, false);
        break;
    case 29:
        run.mark = run.pose;
        inject_mouse_button(wheel_button, true, centre);
        inject_mouse_button(wheel_button, false, centre);
        break;
    case 32:
        check("wheel zooms without panning", run.pose.state.zoom != run.mark.state.zoom
                  && run.pose.target == run.mark.target);
        run.mark = run.pose;
        publish_camera_viewport(0.0F, 0.0F);
        inject_key(pan_key, true);
        break;
    case 38:
        check("zero viewport suspends movement", run.pose == run.mark);
        publish_camera_viewport(extent.x, extent.y);
        break;
    case 42:
        check("viewport restore does not restore held movement", run.pose == run.mark);
        inject_key(pan_key, false);
        break;
    case 44:
        // Eligible drag: a Godot mouse-button press and motion event go through
        // to_raw_camera_event and must rotate about the fixed target.
        run.mark = run.pose;
        inject_mouse_button(grab_button, true, centre);
        inject_mouse_motion(centre, drag);
        break;
    case 47:
        check("eligible mouse drag rotates yaw about a fixed target",
              run.pose.state.yaw_degrees != run.mark.state.yaw_degrees
                  && run.pose.target == run.mark.target
                  && run.pose.state.zoom == run.mark.state.zoom);
        inject_mouse_button(grab_button, false, centre);
        break;
    case 49:
        inject_key(pan_key, true);
        break;
    case 52:
        // A real window resize, delivered through the connected size_changed
        // signal rather than a direct publish, must cancel the held pan.
        run.window_size_mark = get_window()->get_size();
        run.resize_mark = run.resize_notifications;
        run.generation_mark = adapter.counters().viewport_generation;
        get_window()->set_size(run.window_size_mark - Vector2i(16, 16));
        break;
    case 56:
        run.mark = run.pose;
        break;
    case 60:
        check("real size_changed signal cancels held movement",
              run.resize_notifications > run.resize_mark
                  && adapter.counters().viewport_generation > run.generation_mark
                  && run.pose == run.mark);
        get_window()->set_size(run.window_size_mark);
        inject_key(pan_key, false);
        break;
    case 64:
        run.mark = run.pose;
        run.interactive_mark = run.camera;
        adapter.set_capture_locked(true);
        inject_key(pan_key, true);
        inject_key(reset_key, true);
        inject_mouse_button(wheel_button, true, centre);
        inject_mouse_button(MOUSE_BUTTON_MIDDLE, true, centre);
        inject_mouse_motion(Vector2(0.0F, 0.0F), Vector2(40.0F, 0.0F));
        break;
    case 67:
        propagate(NOTIFICATION_APPLICATION_FOCUS_OUT);
        propagate(NOTIFICATION_APPLICATION_FOCUS_IN);
        inject_key(pan_key, true, true);
        break;
    case 70:
        check("capture lock ignores pan, zoom, reset, rotate and lifecycle",
              run.pose == run.mark && same_camera(run.camera, run.interactive_mark));
        adapter.set_capture_locked(false);
        break;
    case 76:
        check("unlock requires a fresh press", run.pose == run.mark);
        inject_key(pan_key, false);
        inject_key(reset_key, false);
        inject_mouse_button(MOUSE_BUTTON_MIDDLE, false, centre);
        break;
    // UI-07 (#314 review P1): a modal dialog opening while a pan key and a rotate grab are held
    // stops both, although their key-up and button-up never reach the camera, and closing it
    // resurrects neither.
    case 77: {
        auto* layer = memnew(CanvasLayer);
        add_child(layer);
        auto* modal = memnew(presentation::godot_backend::EawrUiModalLayer);
        layer->add_child(modal);
        modal->set_position(Vector2());
        modal->set_size(extent);
        modal->hide();
        run.selftest_modal = modal;
        run.mark = run.pose;
        inject_key(pan_key, true);
        inject_mouse_button(grab_button, true, centre);
        break;
    }
    case 81:
        check("held pan moves before the modal opens", !(run.pose == run.mark));
        run.selftest_modal->show();
        break;
    case 83:
        run.mark = run.pose;
        inject_mouse_motion(centre, drag);
        break;
    case 87:
        check("a modal opening stops a held pan and a held rotate grab", run.pose == run.mark);
        inject_key(pan_key, false);
        break;
    case 88:
        run.selftest_modal->hide();
        break;
    case 90:
        inject_mouse_motion(centre, drag);
        break;
    case 93:
        check("closing the modal resurrects no camera hold", run.pose == run.mark);
        inject_mouse_button(grab_button, false, centre);
        run.selftest_modal->get_parent()->queue_free();
        run.selftest_modal = nullptr;
        break;
    default:
        break;
    }
    constexpr std::uint64_t modal_frames = 16;

    const auto finish = [&]() {
        check("capture camera never written by interaction",
              same_camera(capture_camera_, run.capture_mark));
        check("no adapter event was rejected", run.rejected_event.empty());
        bool passed = true;
        for (const auto& [name, ok] : run.checks) passed = passed && ok;
        if (!passed) status_message_ = "camera input self-test failed; see camera_input.selftest";
        const bool persisted = write_report(passed ? "camera_input_selftest_passed" : "failed");
        stop(passed && persisted ? 0 : 2);
    };
    if (step == 78 + modal_frames && !free_phase) {
        finish();
        return true;
    }

    // Free-flight phase: enter from the current tactical camera, fly, rise,
    // look, survive focus loss and a real resize, then leave and prove the
    // tactical state came back exactly.
    const std::string fly_key(free_phase ? camera_input::key_name(fly->code) : "");
    const std::string rise_key(free_phase ? camera_input::key_name(rise->code) : "");
    const std::string exit_key(free_phase ? camera_input::key_name(exit_toggle->code) : "");
    const MouseButton look_button = free_phase ? godot_button(look_grab->code) : MOUSE_BUTTON_RIGHT;
    const auto flying_pose = [&run]() {
        return run.free_controller ? run.free_controller->pose() : tactical_camera::FreeCameraPose{};
    };
    // Numbered as before the modal phase, which runs first.
    switch (step >= modal_frames ? step - modal_frames : 0U) {
    case 78:
        run.mark = run.pose;
        run.interactive_mark = run.camera;
        inject_key(enter_key, true);
        break;
    case 80: {
        const auto forward = tactical_camera::free_camera_forward(flying_pose());
        check("toggle enters free flight from the current camera without a jump",
              run.free_controller && run.free_entries == 1U
                  && adapter.context() == free_context && adapter.held_actions().empty()
                  && same_camera(run.camera, run.interactive_mark)
                  && flying_pose().eye == run.camera.eye && forward.has_value()
                  && dot(forward.value(), view_direction(run.camera)) > 0.9999F
                  && run.pose == run.mark);
        inject_key(enter_key, false);  // release after the context switch is inert
        run.free_mark = flying_pose();
        inject_key(fly_key, true);
        break;
    }
    case 84: {
        const tactical_camera::FreeCameraPose now = flying_pose();
        const std::array<float, 3> moved{now.eye[0] - run.free_mark.eye[0],
            now.eye[1] - run.free_mark.eye[1], now.eye[2] - run.free_mark.eye[2]};
        const float distance = std::sqrt(dot(moved, moved));
        const auto forward = tactical_camera::free_camera_forward(now);
        check("held free forward flies along the view direction",
              run.free_controller && distance > 0.0F && forward.has_value()
                  && dot(moved, forward.value()) > 0.9999F * distance
                  && now.yaw_degrees == run.free_mark.yaw_degrees
                  && now.pitch_degrees == run.free_mark.pitch_degrees
                  && run.pose == run.mark);
        inject_key(fly_key, false);
        break;
    }
    case 86:
        run.free_mark = flying_pose();
        inject_key(rise_key, true);
        break;
    case 90: {
        const tactical_camera::FreeCameraPose now = flying_pose();
        check("held rise moves only along world up",
              now.eye[1] > run.free_mark.eye[1] && now.eye[0] == run.free_mark.eye[0]
                  && now.eye[2] == run.free_mark.eye[2]
                  && now.yaw_degrees == run.free_mark.yaw_degrees
                  && now.pitch_degrees == run.free_mark.pitch_degrees);
        inject_key(rise_key, false);
        break;
    }
    case 92:
        run.free_mark = flying_pose();
        inject_mouse_button(look_button, true, centre);
        inject_mouse_motion(centre + Vector2(40.0F, 20.0F), Vector2(40.0F, 20.0F));
        break;
    case 95: {
        const tactical_camera::FreeCameraPose now = flying_pose();
        check("grabbed look turns the view without translating",
              now.yaw_degrees != run.free_mark.yaw_degrees && now.eye == run.free_mark.eye
                  && now.yaw_degrees >= -180.0F && now.yaw_degrees < 180.0F);
        inject_mouse_button(look_button, false, centre);
        break;
    }
    case 97:
        inject_key(fly_key, true);
        break;
    case 101:
        propagate(NOTIFICATION_APPLICATION_FOCUS_OUT);
        run.free_mark = flying_pose();
        break;
    case 105:
        check("focus loss froze free flight", flying_pose() == run.free_mark);
        propagate(NOTIFICATION_APPLICATION_FOCUS_IN);
        inject_key(fly_key, false);  // late key-up after regain
        break;
    case 109:
        check("focus regain resurrected no free flight", flying_pose() == run.free_mark);
        inject_key(fly_key, true);
        break;
    case 112:
        check("fresh press after regain flies again", flying_pose().eye != run.free_mark.eye);
        run.window_size_mark = get_window()->get_size();
        run.resize_mark = run.resize_notifications;
        run.generation_mark = adapter.counters().viewport_generation;
        get_window()->set_size(run.window_size_mark - Vector2i(16, 16));
        break;
    case 116:
        run.free_mark = flying_pose();
        break;
    case 120:
        check("real size_changed signal cancels free flight",
              run.resize_notifications > run.resize_mark
                  && adapter.counters().viewport_generation > run.generation_mark
                  && flying_pose() == run.free_mark);
        get_window()->set_size(run.window_size_mark);
        inject_key(fly_key, false);
        break;
    case 124:
        inject_key(exit_key, true);
        break;
    case 126:
        check("toggle exit restores the exact tactical state",
              !run.free_controller && run.free_exits == 1U && adapter.context() == tactical
                  && adapter.held_actions().empty() && run.pose == run.mark
                  && same_camera(run.camera, run.interactive_mark)
                  && run.free_moving_steps > 0U);
        inject_key(exit_key, false);
        run.mark = run.pose;
        inject_key(pan_key, true);
        break;
    case 130:
        check("tactical pan resumes after free flight", held_right());
        inject_key(pan_key, false);
        break;
    case 132:
        finish();
        break;
    default:
        break;
    }
    return true;
}
} // namespace eawr::presentation::godot_backend
