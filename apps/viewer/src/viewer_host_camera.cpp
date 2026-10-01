#include "viewer_host_internal.hpp"

#include "ui/input_routing.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>

namespace eawr::presentation::godot_backend {
namespace {

enum class BoundedRead : std::uint8_t { read, unreadable, too_large };

// Like read_bytes, but checks the file length against `max_bytes` before any
// buffer is allocated or filled, so an oversized file cannot exhaust memory
// before its contents are validated.
[[nodiscard]] BoundedRead read_bytes_bounded(
    const ViewerPath& path, const std::size_t max_bytes, std::vector<std::byte>& out) {
    out.clear();
    if (path.is_godot_resource()) {
        const std::string& identifier = path.value();
        const Ref<FileAccess> input = FileAccess::open(
            String::utf8(identifier.data(), static_cast<int64_t>(identifier.size())),
            FileAccess::READ);
        if (input.is_null()) return BoundedRead::unreadable;
        const std::uint64_t length = input->get_length();
        if (length > max_bytes) return BoundedRead::too_large;
        const PackedByteArray packed = input->get_buffer(static_cast<int64_t>(length));
        if (static_cast<std::uint64_t>(packed.size()) != length) return BoundedRead::unreadable;
        out.resize(static_cast<std::size_t>(length));
        if (!out.empty()) std::memcpy(out.data(), packed.ptr(), out.size());
        return BoundedRead::read;
    }
    std::ifstream input(path.native(), std::ios::binary | std::ios::ate);
    if (!input) return BoundedRead::unreadable;
    const std::streamoff end = input.tellg();
    if (end < 0) return BoundedRead::unreadable;
    if (static_cast<std::uint64_t>(end) > max_bytes) return BoundedRead::too_large;
    out.resize(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!out.empty()) {
        input.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }
    return input || out.empty() ? BoundedRead::read : BoundedRead::unreadable;
}

} // namespace

// P1-09 camera input: Godot event translation and synthetic injection. The
// binding loader and lifecycle adapter themselves are engine independent
// (camera_input.hpp); only this translation layer knows Godot types.
namespace {

[[nodiscard]] std::uint8_t camera_modifiers(const InputEventWithModifiers& event) {
    std::uint8_t modifiers{};
    if (event.is_shift_pressed()) modifiers |= camera_input::modifier::shift;
    if (event.is_ctrl_pressed()) modifiers |= camera_input::modifier::ctrl;
    if (event.is_alt_pressed()) modifiers |= camera_input::modifier::alt;
    if (event.is_meta_pressed()) modifiers |= camera_input::modifier::meta;
    return modifiers;
}

// Keys are matched by physical position (US-layout name) when the platform
// reports one, so project WASD bindings survive non-QWERTY layouts. A key
// outside the v1 vocabulary becomes code 0, which no binding can match.
[[nodiscard]] std::optional<camera_input::RawEvent> to_raw_camera_event(
    const Ref<InputEvent>& event) {
    camera_input::RawEvent raw;
    if (const auto* key = Object::cast_to<InputEventKey>(event.ptr())) {
        const Key code = key->get_physical_keycode() != KEY_NONE
            ? key->get_physical_keycode() : key->get_keycode();
        raw.kind = camera_input::RawKind::key;
        raw.code = camera_input::key_code(utf8(OS::get_singleton()->get_keycode_string(code)))
                       .value_or(0U);
        raw.pressed = key->is_pressed();
        raw.echo = key->is_echo();
        raw.modifiers = camera_modifiers(*key);
        return raw;
    }
    if (const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr())) {
        raw.modifiers = camera_modifiers(*button);
        raw.pressed = button->is_pressed();
        raw.position_x = button->get_position().x;
        raw.position_y = button->get_position().y;
        raw.has_position = true;
        switch (button->get_button_index()) {
        case MOUSE_BUTTON_LEFT: raw.code = camera_input::mouse_code::left; break;
        case MOUSE_BUTTON_RIGHT: raw.code = camera_input::mouse_code::right; break;
        case MOUSE_BUTTON_MIDDLE: raw.code = camera_input::mouse_code::middle; break;
        case MOUSE_BUTTON_WHEEL_UP:
            raw.kind = camera_input::RawKind::mouse_wheel;
            raw.code = camera_input::mouse_code::wheel_up;
            raw.factor = button->get_factor();
            return raw;
        case MOUSE_BUTTON_WHEEL_DOWN:
            raw.kind = camera_input::RawKind::mouse_wheel;
            raw.code = camera_input::mouse_code::wheel_down;
            raw.factor = button->get_factor();
            return raw;
        default: return std::nullopt;
        }
        raw.kind = camera_input::RawKind::mouse_button;
        return raw;
    }
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        raw.kind = camera_input::RawKind::mouse_motion;
        raw.modifiers = camera_modifiers(*motion);
        raw.relative_x = motion->get_relative().x;
        raw.relative_y = motion->get_relative().y;
        raw.position_x = motion->get_position().x;
        raw.position_y = motion->get_position().y;
        raw.has_position = true;
        return raw;
    }
    return std::nullopt;
}

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

} // namespace

namespace viewer_host_detail {

// Unit vector from a camera's eye toward its target; zero for a degenerate
// camera, which no self-test comparison accepts.
[[nodiscard]] std::array<float, 3> view_direction(const FixedCamera& camera) {
    const std::array<float, 3> delta{camera.target[0] - camera.eye[0],
        camera.target[1] - camera.eye[1], camera.target[2] - camera.eye[2]};
    const float length = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    if (!(length > 0.0F)) return {};
    return {delta[0] / length, delta[1] / length, delta[2] / length};
}

} // namespace viewer_host_detail

namespace {

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

bool ViewerHost::start_camera_interaction() {
    const auto context = camera_input::context_for(tactical_camera_->mode);
    if (!context) {
        status_message_ = "the unlocked camera mode is not interactive; free flight is "
                          "toggled from land or space";
        return false;
    }
    // The 1 MiB cap is enforced on the file length before reading; the parser
    // repeats it as defence in depth.
    std::vector<std::byte> bytes;
    switch (read_bytes_bounded(ViewerPath{options_->camera_bindings_path},
                               camera_input::max_binding_document_bytes, bytes)) {
    case BoundedRead::read: break;
    case BoundedRead::too_large:
        status_message_ = std::string(camera_input::diagnostic_codes::invalid_bindings)
            + ": camera bindings exceed 1 MiB and were not read: "
            + options_->camera_bindings_path;
        return false;
    case BoundedRead::unreadable:
        status_message_ = "camera bindings could not be read: " + options_->camera_bindings_path;
        return false;
    }
    DisplayServer* display = DisplayServer::get_singleton();
    auto run = std::make_unique<CameraInteraction>(
        *context, display != nullptr && display->window_is_focused());
    run->bindings_path = options_->camera_bindings_path;
    run->bindings_sha256 = hash_bytes(bytes);
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (auto activated = run->adapter.activate(text); !activated) {
        status_message_ = core::format_diagnostic(activated.error());
        return false;
    }
    run->pose = camera_input::TacticalPose{tactical_camera_->state, capture_camera_.target};
    run->default_zoom = tactical_camera_->state.zoom;
    run->camera = capture_camera_;
    run->selftest = options_->camera_input_selftest;
    if (!options_->capture_path.empty()) {
        // A requested capture is a fixed capture: input is locked before the
        // first frame and stays locked for the entire run.
        run->capture_locked_for_run = true;
        run->adapter.set_capture_locked(true);
    }
    camera_interaction_ = std::move(run);
    const Vector2 size = get_viewport()->get_visible_rect().size;
    publish_camera_viewport(size.x, size.y);
    get_viewport()->connect("size_changed", callable_mp(this, &ViewerHost::on_viewport_size_changed));
    set_process_input(true);
    set_process_unhandled_input(true);
    return true;
}

void ViewerHost::publish_camera_viewport(const float width, const float height) {
    if (!camera_interaction_) return;
    if (auto published = camera_interaction_->adapter.set_viewport(width, height); !published) {
        // An unusable extent suspends input rather than keeping stale state.
        static_cast<void>(camera_interaction_->adapter.set_viewport(0.0F, 0.0F));
    }
}

void ViewerHost::on_viewport_size_changed() {
    if (!camera_interaction_) return;
    ++camera_interaction_->resize_notifications;
    const Vector2 size = get_viewport()->get_visible_rect().size;
    publish_camera_viewport(size.x, size.y);
}

void ViewerHost::on_map_viewport_size_changed() {
    if (!map_mode_) return;
    const Vector2 size = get_viewport()->get_visible_rect().size;
    map_mode_->viewport_changed(size.x, size.y);
}

void ViewerHost::_input(const Ref<InputEvent>& event) {
    sync_modal_holds();
    std::uint32_t button{};
    const auto input = input_class(event, button);
    if (!input || !presentation::ui::world_takes_before_gui(*input, button, world_capture_)) return;
    if (*input == presentation::ui::InputClass::button_release) world_capture_.released(button);
    get_viewport()->set_input_as_handled();
    route_world_input(event);
}

void ViewerHost::_unhandled_input(const Ref<InputEvent>& event) {
    std::uint32_t button{};
    const auto input = input_class(event, button);
    // Events the world never reads (joypad, gestures) went nowhere before either.
    if (!input) return;
    if (!presentation::ui::world_accepts(*input, input_focus(*get_viewport()))) {
        if (input_routing_mode_) input_routing_mode_->world_dropped(*input);
        return;
    }
    if (*input == presentation::ui::InputClass::button_press) world_capture_.pressed(button);
    if (*input == presentation::ui::InputClass::button_release) world_capture_.released(button);
    route_world_input(event);
}

void ViewerHost::on_gui_focus_changed(Control* control) {
    if (is_text_control(control)) cancel_held_world_input();
}

void ViewerHost::sync_modal_holds() {
    if (!input_routing_mode_ && !map_mode_ && !camera_interaction_) return;
    if (!modal_holds_.changed(input_focus(*get_viewport()).modal_open)) return;
    world_capture_.clear();
    cancel_held_world_input();
}

void ViewerHost::cancel_held_world_input() {
    DisplayServer* display = DisplayServer::get_singleton();
    const bool window_focused = display != nullptr && display->window_is_focused();
    if (input_routing_mode_) input_routing_mode_->world_cancelled();
    if (map_mode_) {
        map_mode_->focus(false);
        map_mode_->focus(window_focused);
    }
    if (camera_interaction_) {
        // Cancel, then keep the focus the adapter had: the window's focus reaches it through
        // the focus notifications, and a cancel must not change it.
        camera_input::Adapter& adapter = camera_interaction_->adapter;
        const bool focused = adapter.focused();
        adapter.set_focus(false);
        adapter.set_focus(focused);
    }
}

void ViewerHost::route_world_input(const Ref<InputEvent>& event) {
    if (input_routing_mode_) {
        static_cast<void>(input_routing_mode_->world_input(event));
        return;
    }
    if (map_mode_) {
        map_mode_->input(event);
        return;
    }
    if (!camera_interaction_ || event.is_null()) return;
    ++camera_interaction_->input_callbacks;
    const auto raw = to_raw_camera_event(event);
    if (!raw) return;
    if (auto handled = camera_interaction_->adapter.handle(*raw); !handled) {
        camera_interaction_->rejected_event = core::format_diagnostic(handled.error());
    }
}

void ViewerHost::_notification(const int what) {
    switch (what) {
    case NOTIFICATION_PREDELETE:
        // #961: Godot sends this to the extension before deleting the host's
        // children. Map teardown stops and releases its audio players, which
        // are those children; the extension destructor runs after their deletion.
        // This also covers engine-driven quits such as --quit-after, which do
        // not send a window close request.
        map_mode_.reset();
        return;
    case NOTIFICATION_WM_CLOSE_REQUEST:
        shutdown_trace::mark("window close requested");
        if (map_mode_) map_mode_->close_requested();
        break;
    case NOTIFICATION_APPLICATION_FOCUS_OUT:
    case NOTIFICATION_WM_WINDOW_FOCUS_OUT:
    case NOTIFICATION_WM_MOUSE_EXIT: world_capture_.clear(); break;
    default: break;
    }
    if (map_mode_) {
        switch (what) {
        case NOTIFICATION_APPLICATION_FOCUS_OUT:
        case NOTIFICATION_WM_WINDOW_FOCUS_OUT: map_mode_->focus(false); break;
        case NOTIFICATION_APPLICATION_FOCUS_IN:
        case NOTIFICATION_WM_WINDOW_FOCUS_IN: map_mode_->focus(true); break;
        case NOTIFICATION_WM_MOUSE_EXIT: map_mode_->pointer_left(); break;
        default: break;
        }
        return;
    }
    if (!camera_interaction_) return;
    camera_input::Adapter& adapter = camera_interaction_->adapter;
    switch (what) {
    case NOTIFICATION_APPLICATION_FOCUS_OUT:
    case NOTIFICATION_WM_WINDOW_FOCUS_OUT:
        ++camera_interaction_->focus_notifications;
        adapter.set_focus(false);
        break;
    case NOTIFICATION_APPLICATION_FOCUS_IN:
    case NOTIFICATION_WM_WINDOW_FOCUS_IN:
        ++camera_interaction_->focus_notifications;
        adapter.set_focus(true);
        break;
    case NOTIFICATION_WM_MOUSE_EXIT:
        adapter.pointer_left();
        break;
    default:
        break;
    }
}

bool ViewerHost::step_camera_interaction(const double delta) {
    CameraInteraction& run = *camera_interaction_;
    if (run.selftest && !advance_camera_selftest()) return false;
    auto intent = run.adapter.take_step(tactical_camera_->constants);
    if (!intent) {
        status_message_ = core::format_diagnostic(intent.error());
        return false;
    }
    // #22 precedence: nothing about the camera changes while locked, and no
    // free-flight toggle is honoured.
    if (run.adapter.capture_locked()) return true;
    // A step that carries a toggle performs only the transition; the rest of
    // its intent belonged to the context being left.
    if (intent.value().free_toggle_requests > 0U) return toggle_free_camera();
    // The self-test uses a fixed step so its assertions do not depend on the
    // render cadence; interactive use takes the frame delta.
    const float seconds = run.selftest ? 1.0F / 60.0F : static_cast<float>(delta);
    if (run.free_controller) return step_free_camera(intent.value(), seconds);
    auto next = camera_input::advance_pose(
        tactical_camera_->constants, run.pose, intent.value(), run.default_zoom, seconds);
    if (!next) {
        status_message_ = core::format_diagnostic(next.error());
        return false;
    }
    ++run.steps;
    if (next.value() == run.pose) return true;
    ++run.moving_steps;
    run.pose = next.value();
    auto eye = tactical_camera::eye_position(std::span<const float, 3>{run.pose.target},
        run.pose.state.distance, run.pose.state.pitch_degrees, run.pose.state.yaw_degrees);
    if (!eye) {
        status_message_ = core::format_diagnostic(eye.error());
        return false;
    }
    run.camera.eye = eye.value();
    run.camera.target = run.pose.target;
    // #515: the XML angle is FoC's (horizontal on 4:3); the renderer takes the vertical one.
    auto vertical = tactical_camera::vertical_fov_degrees(run.pose.state.fov_degrees);
    if (!vertical) {
        status_message_ = core::format_diagnostic(vertical.error());
        return false;
    }
    run.camera.vertical_fov_degrees = vertical.value();
    renderer_->set_camera(run.camera);
    return true;
}

// Enters free flight from the current tactical camera, or leaves it.
//
// Entry saves the whole tactical state (pose, render camera, context), builds
// the controller from the current eye/yaw/pitch and switches the adapter to
// the `free` context, which cancels every held control, pending delta and
// pointer sample. The render camera is not rewritten on entry, so the first
// free frame is pixel-identical to the last tactical frame. A pose the
// controller refuses (for example a pitch outside the project bounds) leaves
// the tactical camera active and is reported, never clamped.
//
// Exit restores the saved state exactly and switches back to the saved
// context, which cancels again. No map bound is applied: typed #26 bounds are
// not available to this host.
bool ViewerHost::toggle_free_camera() {
    CameraInteraction& run = *camera_interaction_;
    if (run.free_controller) {
        run.free_controller.reset();
        run.pose = run.saved_pose;
        run.camera = run.saved_camera;
        run.adapter.set_context(run.saved_context);
        renderer_->set_camera(run.camera);
        ++run.free_exits;
        run.free_transitions.emplace_back("exit", frame_);
        return true;
    }
    const camera_input::BindingTable* table = run.adapter.table();
    if (!table || !table->free_camera) {
        // Unreachable with a validated table: only v2 binds free_toggle, and
        // v2 requires settings. Kept so a toggle can never enter unconfigured.
        ++run.free_rejections;
        run.free_rejection = "the active binding table has no free_camera settings";
        run.free_transitions.emplace_back("rejected", frame_);
        return true;
    }
    auto created = tactical_camera::FreeCameraController::create(*table->free_camera,
        std::span<const float, 3>{run.camera.eye}, run.pose.state.yaw_degrees,
        run.pose.state.pitch_degrees);
    if (!created) {
        ++run.free_rejections;
        run.free_rejection = core::format_diagnostic(created.error());
        run.free_transitions.emplace_back("rejected", frame_);
        return true;
    }
    run.saved_pose = run.pose;
    run.saved_camera = run.camera;
    run.saved_context = run.adapter.context();
    run.free_controller.emplace(std::move(created).value());
    run.free_pose = run.free_controller->pose();
    run.adapter.set_context(camera_input::Context::free);
    ++run.free_entries;
    run.free_transitions.emplace_back("enter", frame_);
    return true;
}

// One free-flight step. The render camera keeps the entry FOV, clip planes
// and world up; its look-at point sits along the view direction at the saved
// tactical distance, which only orients the camera.
bool ViewerHost::step_free_camera(const camera_input::StepIntent& intent, const float seconds) {
    CameraInteraction& run = *camera_interaction_;
    const tactical_camera::FreeCameraPose before = run.free_controller->pose();
    const tactical_camera::FreeCameraIntent free_intent{intent.free_move_x, intent.free_move_z,
        intent.free_move_y, intent.free_look_yaw_units, intent.free_look_pitch_units};
    if (auto advanced = run.free_controller->advance(free_intent, seconds); !advanced) {
        status_message_ = core::format_diagnostic(advanced.error());
        return false;
    }
    ++run.free_steps;
    const tactical_camera::FreeCameraPose& pose = run.free_controller->pose();
    run.free_pose = pose;
    if (pose == before) return true;
    ++run.free_moving_steps;
    auto forward = tactical_camera::free_camera_forward(pose);
    if (!forward) {
        status_message_ = core::format_diagnostic(forward.error());
        return false;
    }
    const float reach = std::max(run.saved_pose.state.distance, 1.0F);
    run.camera.eye = pose.eye;
    run.camera.target = {pose.eye[0] + forward.value()[0] * reach,
        pose.eye[1] + forward.value()[1] * reach, pose.eye[2] + forward.value()[2] * reach};
    renderer_->set_camera(run.camera);
    return true;
}

// Scripted synthetic-input run. Key/mouse events go through the engine's
// Input::parse_input_event dispatch; focus changes are propagated from the
// scene root as the engine does; zero and restored extents go through the same
// publish function as the size_changed signal, and one real window resize goes
// through the signal itself. Each check compares exact state.
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
