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

void ViewerHost::on_skirmish_start() {
    if (skirmish_setup_) skirmish_setup_->request_start();
}

void ViewerHost::_input(const Ref<InputEvent>& event) {
    sync_modal_holds();
    if (map_mode_) map_mode_->observe_pointer(event);
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


} // namespace eawr::presentation::godot_backend
