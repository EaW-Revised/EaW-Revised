#include "frame_timer.hpp"
#include "startup_trace.hpp"
#include "battle_content.hpp"
#include "map_mode.hpp"
#include "map_mode_internal.hpp"
#include "eawr/presentation/camera/overview.hpp"
#include "render_profile_viewport.hpp"
#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace eawr::presentation::godot_backend {
namespace {

[[nodiscard]] std::optional<std::string> read_camera_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > (1U << 20U)) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
    return bytes;
}

[[nodiscard]] bool orbit_focus_at_centre(const camera::TacticalFrame& frame,
    const std::array<float, 3>& focus, const float radius) {
    const Vector3 eye(frame.eye[0], frame.eye[1], frame.eye[2]);
    const Vector3 target(frame.target[0], frame.target[1], frame.target[2]);
    const Vector3 point(focus[0], focus[1], focus[2]);
    const Vector3 up(frame.up[0], frame.up[1], frame.up[2]);
    Transform3D view;
    view.origin = eye;
    view = view.looking_at(target, up);
    const Vector3 local = view.affine_inverse().xform(point);
    return point.distance_to(target) < 0.01F && std::abs(local.x) < 0.01F
        && std::abs(local.y) < 0.01F && local.z < 0.0F
        && std::abs(eye.distance_to(point) - radius) < 0.01F;
}

void inject_map_ctrl(const bool pressed, const bool echo = false) {
    Ref<InputEventKey> event;
    event.instantiate();
    event->set_keycode(KEY_CTRL);
    event->set_physical_keycode(KEY_CTRL);
    event->set_pressed(pressed);
    event->set_echo(echo);
    event->set_ctrl_pressed(pressed);
    Input::get_singleton()->parse_input_event(event);
}

void inject_map_button(const MouseButton button, const Vector2 position,
    const bool pressed = true, const bool ctrl = false) {
    Ref<InputEventMouseButton> event;
    event.instantiate();
    event->set_button_index(button);
    event->set_pressed(pressed);
    event->set_factor(1.0F);
    event->set_position(position);
    event->set_ctrl_pressed(ctrl);
    Input::get_singleton()->parse_input_event(event);
}

void inject_map_motion(const Vector2 position, const Vector2 relative, const bool ctrl = false) {
    Ref<InputEventMouseMotion> event;
    event.instantiate();
    event->set_position(position);
    event->set_relative(relative);
    event->set_ctrl_pressed(ctrl);
    Input::get_singleton()->parse_input_event(event);
}


} // namespace

[[nodiscard]] camera::TacticalFrame map_mode_detail::camera_frame(const FixedCamera& source) {
    return {source.width, source.height, source.vertical_fov_degrees,
        source.near_plane, source.far_plane, source.eye, source.target, source.up};
}

[[nodiscard]] FixedCamera map_mode_detail::render_camera(const camera::TacticalFrame& source) {
    FixedCamera result;
    result.width = source.width;
    result.height = source.height;
    result.vertical_fov_degrees = source.vertical_fov_degrees;
    result.near_plane = source.near_plane;
    result.far_plane = source.far_plane;
    result.eye = source.eye;
    result.target = source.target;
    result.up = source.up;
    return result;
}

void map_mode_detail::inject_map_key(const Key code, const bool pressed) {
    Ref<InputEventKey> event;
    event.instantiate();
    event->set_keycode(code);
    event->set_physical_keycode(code);
    event->set_pressed(pressed);
    Input::get_singleton()->parse_input_event(event);
}

// The Ctrl key's own events, as the platform sends them around a Ctrl gesture: the press and
// its auto-repeat carry the Ctrl bit, the release does not.

std::optional<eawr::viewer::MapCameraSource> MapMode::State::load_map_camera(
    const camera::Mode mode, std::string& failure_text, const eawr::viewer::MapCameraConfig* generated) const {
    eawr::viewer::MapCameraSource source;
    std::optional<std::string> config_text;
    if (generated) {
        std::ostringstream identity;
        identity << std::setprecision(std::numeric_limits<float>::max_digits10)
                 << "SC-02 " << generated->map_path << ' ' << generated->map_sha256 << ' '
                 << generated->bounds.min_x << ' ' << generated->bounds.max_x << ' '
                 << generated->bounds.min_y << ' ' << generated->bounds.max_y << ' '
                 << generated->target_x << ' ' << generated->target_y << " distance=1200 yaw=XML"
                 << " Distance_Min=100 Tactical_Min_Scroll_Speed=823.529412 Pitch_Min=-60 overview=5";
        config_text = identity.str();
    } else config_text = read_camera_file(map_camera_config_path);
    if (!config_text) {
        failure_text = "map camera config could not be read or exceeds 1 MiB";
        return std::nullopt;
    }
    source.config_sha256 = sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(config_text->data()), config_text->size()));
    source.config_file = generated ? "skirmish-auto-camera" : ViewerPath::utf8(map_camera_config_path.filename());
    // Map identity and bounds are validated here, before any activation.
    auto config = generated ? core::Result<eawr::viewer::MapCameraConfig>::success(*generated)
                            : eawr::viewer::parse_map_camera_config(*config_text, options.map_path, map_hash, mode);
    if (!config) {
        failure_text = core::format_diagnostic(config.error());
        return std::nullopt;
    }
    // --eawr-camera-zoom (validated in ready) replaces the initial and reset zoom.
    if (options.camera_zoom) config.value().zoom = *options.camera_zoom;
    const std::filesystem::path bindings_path = (generated ? ViewerPath{std::string(
        ProjectSettings::get_singleton()->globalize_path("res://config").utf8().get_data())}.native()
        : map_camera_config_path.parent_path()) / ViewerPath{config.value().bindings_path}.native();
    auto bindings_text = read_camera_file(bindings_path);
    if (!bindings_text) {
        failure_text = "map camera bindings could not be read or exceeds 1 MiB";
        return std::nullopt;
    }
    source.bindings_sha256 = sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bindings_text->data()), bindings_text->size()));
    constexpr std::string_view tactical_path = "data/xml/tacticalcameras.xml";
    constexpr std::string_view constants_path = "data/xml/gameconstants.xml";
    auto tactical_bytes = filesystem->open(tactical_path);
    auto constants_bytes = filesystem->open(constants_path);
    if (!tactical_bytes || !constants_bytes) {
        failure_text = core::format_diagnostic(!tactical_bytes ? tactical_bytes.error() : constants_bytes.error());
        return std::nullopt;
    }
    source.tactical_xml_sha256 = hash_bytes(tactical_bytes.value());
    source.gameconstants_xml_sha256 = hash_bytes(constants_bytes.value());
    if (mode == camera::Mode::space) {
        auto overview = camera::load_overview_constants(
            {tactical_bytes.value(), tactical_path, source.tactical_xml_sha256}, camera::Mode::space);
        if (overview) source.overview_base_clicks = overview.value().clicks;
    }
    auto loaded = camera::load_constants(
        {tactical_bytes.value(), tactical_path, source.tactical_xml_sha256},
        {constants_bytes.value(), constants_path, source.gameconstants_xml_sha256}, mode);
    if (!loaded) {
        failure_text = core::format_diagnostic(loaded.error());
        return std::nullopt;
    }
    if (generated) {
        // Same project distance and owner overrides as the Coruscant live XML.
        const auto& constants = loaded.value().constants;
        config.value().zoom = options.camera_zoom.value_or(std::clamp(
            (1200.0F - 100.0F) / (constants.distance_max - 100.0F), 0.0F, 1.0F));
        config.value().yaw_degrees = constants.yaw_default;
    }
    // Precedence: effective-VFS XML, then the config's project-authored map
    // overrides; a fixed capture later pins the whole frame over both.
    auto resolved = eawr::viewer::resolve_map_constants(config.value(), std::move(loaded.value()),
        source.config_file, source.config_sha256);
    if (!resolved) {
        failure_text = core::format_diagnostic(resolved.error());
        return std::nullopt;
    }
    source.config = std::move(config.value());
    source.constants = std::move(resolved.value().constants);
    source.provenance = std::move(resolved.value().provenance);
    source.overrides = std::move(resolved.value().overrides);
    source.bindings_json = std::move(*bindings_text);
    return source;
}

void MapMode::State::free_selftest_tick() {
    if (!map_free_selftest || !map_camera) return;
    const std::uint32_t tick = frame + 1U;
    const bool locked = map_camera->controller().capture_locked();
    const Vector2 centre = map_camera_host->get_viewport()->get_visible_rect().size * 0.5F;
    const auto check = [this](std::string name, const bool passed) {
        map_free_checks.emplace_back(std::move(name), passed);
    };
    // A real window may publish a new viewport before the entry callback.
    // Entry must preserve pose and projection; width and height may change.
    const auto same_initial_pose = [this] {
        const auto& current = map_camera->frame();
        const auto initial = camera_frame(map_camera_initial_frame);
        return current.eye == initial.eye && current.target == initial.target
            && current.up == initial.up && current.vertical_fov_degrees == initial.vertical_fov_degrees
            && current.near_plane == initial.near_plane && current.far_plane == initial.far_plane;
    };
    if (map_camera->free_rejections() > 0 && tick > 2U) return;
    switch (tick) {
    case 1:
        map_camera_host->get_tree()->get_root()->propagate_notification(
            Node::NOTIFICATION_APPLICATION_FOCUS_IN);
        inject_map_key(KEY_F, true);
        break;
    case 2:
        if (locked) {
            check("fixed capture suppresses free entry", !map_camera->free_active()
                && camera_frame(render_camera(map_camera->frame()))
                    == camera_frame(map_camera_initial_frame));
        } else if (map_camera->free_rejections() > 0) {
            check("incompatible entry leaves tactical pose unchanged", !map_camera->free_active()
                && map_camera->free_entries() == 0
                && map_camera->adapter().context() == viewer::camera_input::Context::land
                && same_initial_pose());
        } else {
            check("free entry keeps tactical camera identity", map_camera->free_active()
                && map_camera->free_entries() == 1
                && same_initial_pose());
        }
        inject_map_key(KEY_F, false);
        if (map_camera->free_rejections() > 0) break;
        inject_map_key(KEY_W, true);
        inject_map_button(MOUSE_BUTTON_RIGHT, centre);
        break;
    case 3:
        if (!locked) {
            if (auto moved = map_camera->step(4.0F); !moved) {
                failure = core::format_diagnostic(moved.error());
            }
            check("free movement uses held keyboard callback", map_camera->free_active()
                && map_camera->frame().eye != map_camera_initial_frame.eye);
            const auto& eye = map_camera->frame().eye;
            const auto& bounds = map_camera->controller().render_bounds();
            check("free eye is not clamped to tactical target bounds",
                eye[0] < bounds.min_x || eye[0] > bounds.max_x
                    || eye[2] < bounds.min_z || eye[2] > bounds.max_z);
        }
        inject_map_motion(centre, Vector2(24.0F, 12.0F));
        break;
    case 4:
        if (!locked) {
            check("free look changes yaw and pitch", map_camera->free_pose()
                && map_camera->free_pose()->yaw_degrees != map_camera->controller().yaw_degrees()
                && map_camera->free_pose()->pitch_degrees
                    != map_camera->controller().state().pitch_degrees);
        }
        map_camera_host->get_tree()->get_root()->propagate_notification(
            Node::NOTIFICATION_APPLICATION_FOCUS_OUT);
        map_free_mark = map_camera->frame().eye;
        break;
    case 5:
        check("focus loss clears held free movement", map_camera->frame().eye == map_free_mark);
        map_camera_host->get_tree()->get_root()->propagate_notification(
            Node::NOTIFICATION_APPLICATION_FOCUS_IN);
        inject_map_key(KEY_W, false);
        inject_map_button(MOUSE_BUTTON_RIGHT, centre, false);
        break;
    case 6: inject_map_key(KEY_D, true); break;
    case 7:
        if (!locked) {
            if (auto moved = map_camera->step(0.1F); !moved) {
                failure = core::format_diagnostic(moved.error());
            }
        }
        map_free_mark = map_camera->frame().eye;
        map_camera_window_size = map_camera_host->get_window()->get_size();
        map_free_generation_mark = map_camera->adapter().counters().viewport_generation;
        map_camera_host->get_window()->set_size(map_camera_window_size - Vector2i(16, 16));
        break;
    case 10:
        check("resize clears held free movement and preserves pose",
            map_camera->frame().eye == map_free_mark
                && (locked ? map_camera->adapter().counters().viewport_generation
                        == map_free_generation_mark
                    : map_camera->adapter().counters().viewport_generation
                        > map_free_generation_mark));
        inject_map_key(KEY_F, true);
        break;
    case 11:
        if (!locked) {
            const auto& restored = map_camera->frame();
            check("exit restores tactical pose in current viewport",
                !map_camera->free_active() && map_camera->free_exits() == 1
                    && map_camera->adapter().context() == viewer::camera_input::Context::land
                    && restored.eye == map_camera_initial_frame.eye
                    && restored.target == map_camera_initial_frame.target
                    && restored.width == map_camera->controller().frame().width
                    && restored.height == map_camera->controller().frame().height);
        } else {
            check("fixed capture survives resize", !map_camera->free_active()
                && camera_frame(render_camera(map_camera->frame()))
                    == camera_frame(map_camera_initial_frame));
        }
        map_free_mark = map_camera->frame().target;
        inject_map_key(KEY_F, false);
        inject_map_key(KEY_D, false);
        break;
    case 14:
        check("context switch clears held free control", map_camera->frame().target == map_free_mark);
        break;
    default: break;
    }
}

void MapMode::State::camera_selftest_tick() {
    if (!map_camera_selftest || !map_camera) return;
    const std::uint32_t tick = frame + 1U;
    const bool locked = map_camera->controller().capture_locked();
    const Vector2 centre = map_camera_host->get_viewport()->get_visible_rect().size * 0.5F;
    const auto unchanged = [&] { return camera_frame(render_camera(map_camera->frame()))
        == camera_frame(map_camera_initial_frame); };
    const auto check = [this](std::string name, const bool passed) {
        map_camera_checks.emplace_back(std::move(name), passed);
    };
    switch (tick) {
    case 1:
        map_camera_host->get_tree()->get_root()->propagate_notification(
            Node::NOTIFICATION_APPLICATION_FOCUS_IN);
        check("focus established before input", map_camera->adapter().focused()
            && map_camera_focus_notifications > 0);
        inject_map_key(KEY_D, true);
        break;
    case 2:
        // A declared one-second test step makes the clamp independent of how
        // quickly the graphical runner schedules its warmup frames.
        static_cast<void>(map_camera->step(1.0F));
        break;
    case 3: inject_map_key(KEY_D, false); break;
    case 4:
        check("pan reaches authored X clamp", locked ? unchanged()
            : map_camera->frame().target[0] == map_camera->controller().render_bounds().max_x);
        break;
    case 5:
        map_camera_mark = map_camera->frame().target;
        inject_map_button(MOUSE_BUTTON_MIDDLE, centre);
        break;
    // A middle drag without Ctrl translates (FoC); left and down, away from the X clamp.
    case 6: inject_map_motion(centre, Vector2(-20.0F, 20.0F)); break;
    case 7:
        map_camera_zoom_mark = map_camera->controller().state().zoom;
        inject_map_button(MOUSE_BUTTON_WHEEL_DOWN, centre);
        break;
    case 8:
        check("wheel ignored during middle translation",
            map_camera->controller().state().zoom == map_camera_zoom_mark);
        check("grabbed motion translates without turning", locked ? unchanged()
            : map_camera->controller().yaw_degrees() == map_camera->config().yaw_degrees
                && map_camera->frame().target[0] < map_camera_mark[0]
                && map_camera->frame().target[2] > map_camera_mark[2]);
        inject_map_button(MOUSE_BUTTON_MIDDLE, centre, false);
        break;
    case 10:
        map_camera_zoom_mark = map_camera->controller().state().zoom;
        inject_map_button(MOUSE_BUTTON_WHEEL_DOWN, centre);
        break;
    case 12:
        check("wheel changes zoom", locked ? unchanged()
            : map_camera->controller().state().zoom > map_camera_zoom_mark);
        break;
    case 14:
        map_camera_mark = map_camera->frame().target;
        inject_map_key(KEY_A, true);
        break;
    case 15:
        map_camera_focus_pan_moved = map_camera->frame().target != map_camera_mark;
        map_camera_host->get_tree()->get_root()->propagate_notification(
            Node::NOTIFICATION_APPLICATION_FOCUS_OUT);
        break;
    case 16: map_camera_mark = map_camera->frame().target; break;
    case 18:
        map_camera_host->get_tree()->get_root()->propagate_notification(
            Node::NOTIFICATION_APPLICATION_FOCUS_IN);
        break;
    case 20:
        check("focus loss cancels held pan", locked ? (unchanged() && map_camera_focus_notifications >= 3)
            : map_camera_focus_pan_moved && map_camera->frame().target == map_camera_mark);
        inject_map_key(KEY_A, false);
        break;
    case 22:
        map_camera_mark = map_camera->frame().target;
        inject_map_key(KEY_A, true);
        break;
    case 24:
        map_camera_resize_pan_moved = map_camera->frame().target != map_camera_mark;
        map_camera_window_size = map_camera_host->get_window()->get_size();
        map_camera_generation_mark = map_camera->adapter().counters().viewport_generation;
        map_camera_host->get_window()->set_size(map_camera_window_size - Vector2i(16, 16));
        break;
    case 26: map_camera_mark = map_camera->frame().target; break;
    case 29:
        check("real resize cancels held pan", locked ? (unchanged()
            && map_camera_host->get_window()->get_size() != map_camera_window_size
            && map_camera->adapter().counters().viewport_generation == map_camera_generation_mark)
            : map_camera_resize_pan_moved && map_camera_resize_notifications > 0
                && map_camera->adapter().counters().viewport_generation > map_camera_generation_mark
                && map_camera->frame().target == map_camera_mark);
        if (!locked) map_camera_host->get_window()->set_size(map_camera_window_size);
        inject_map_key(KEY_A, false);
        break;
    case 30:
        map_camera_mark = map_camera->frame().target;
        break;
    case 31:
        // Use the bridge's raw pointer path and a declared duration so the
        // edge check is independent of OS window size and graphical frame time.
        if (auto sampled = map_camera->handle(viewer::camera_input::RawEvent{
                .kind = viewer::camera_input::RawKind::mouse_motion,
                .position_x = map_camera_mark[0]
                    <= map_camera->controller().render_bounds().min_x
                    ? static_cast<float>(map_camera->frame().width - 1U) : 0.0F,
                .position_y = static_cast<float>(map_camera->frame().height) * 0.5F,
                .has_position = true}); !sampled) {
            failure = core::format_diagnostic(sampled.error());
        }
        if (auto moved = map_camera->step(0.1F); !moved) {
            failure = core::format_diagnostic(moved.error());
        }
        map_camera_edge_pan_moved = map_camera->frame().target != map_camera_mark;
        map_camera_host->get_tree()->get_root()->propagate_notification(
            Node::NOTIFICATION_WM_MOUSE_EXIT);
        map_camera_mark = map_camera->frame().target;
        break;
    case 32:
        check("pointer exit cancels edge pan", locked ? (unchanged()
            && !map_camera->adapter().pointer_valid())
            : map_camera_edge_pan_moved && !map_camera->adapter().pointer_valid()
                && map_camera->frame().target == map_camera_mark);
        break;
    case 34: inject_map_key(KEY_HOME, true); break;
    case 37:
        check("reset restores authored pose", locked ? unchanged()
            : map_camera->resets() == 1U
                && map_camera->frame().target[0] == map_camera->config().target_x
                && map_camera->frame().target[2] == -map_camera->config().target_y
                && map_camera->controller().yaw_degrees() == map_camera->config().yaw_degrees
                && map_camera->controller().state().zoom == map_camera->config().zoom);
        inject_map_key(KEY_HOME, false);
        break;
    case 39:
        check("input callbacks reached map adapter", map_camera->input_callbacks() >= 8U);
        check("capture lock isolates input", !locked ||
            (unchanged() && map_camera->adapter().counters().ignored_ineligible > 0));
        break;
    case 41: {
        map_camera_mark = map_camera->frame().target;
        map_camera_zoom_mark = map_camera->controller().state().zoom;
        map_camera_yaw_mark = map_camera->controller().yaw_degrees();
        map_camera_pitch_mark = map_camera->controller().state().pitch_degrees;
        const auto& eye = map_camera->frame().eye;
        map_camera_orbit_radius_mark = std::hypot(eye[0] - map_camera_mark[0],
            eye[1] - map_camera_mark[1], eye[2] - map_camera_mark[2]);
        inject_map_ctrl(true);
        inject_map_ctrl(true, true);
        inject_map_button(MOUSE_BUTTON_MIDDLE, centre, true, true);
        break;
    }
    // Ctrl + middle-drag right and up: yaw falls and the pitch tilts toward the
    // horizon (#348 owner deviation: FoC land tilts 0 per mouse unit).
    case 42:
        inject_map_motion(centre, Vector2(8.0F, -40.0F), true);
        inject_map_button(MOUSE_BUTTON_WHEEL_DOWN, centre, true, true);
        inject_map_ctrl(true, true);
        break;
    case 44: {
        check("Ctrl grabbed motion rotates yaw and tilts", locked ? unchanged()
            : map_camera->controller().yaw_degrees() < map_camera_yaw_mark
                && map_camera->controller().state().pitch_degrees < map_camera_pitch_mark
                && map_camera->controller().orbit_pitch_offset() < 0.0F
                && map_camera->controller().state().zoom == map_camera_zoom_mark
                && orbit_focus_at_centre(map_camera->frame(), map_camera_mark,
                    map_camera_orbit_radius_mark));
        check("wheel ignored during middle rotation",
            map_camera->controller().state().zoom == map_camera_zoom_mark);
        bool continuous = true;
        for (const float direction : {-1.0F, 1.0F}) {
            for (int drag = 0; drag < 12; ++drag) {
                const float before = map_camera->controller().yaw_degrees();
                const float width = static_cast<float>(map_camera->frame().width);
                inject_map_motion(centre, Vector2(direction * width, 0.0F), true);
                Input::get_singleton()->flush_buffered_events();
                static_cast<void>(map_camera->step(0.0F));
                const float after = map_camera->controller().yaw_degrees();
                const float expected = -direction * 100.0F
                    * map_camera->constants().yaw_per_mouse_unit;
                continuous = continuous && (locked ? unchanged()
                    : after >= -180.0F && after < 180.0F
                        && std::abs(std::remainder(after - before - expected, 360.0F)) < 0.01F);
            }
        }
        check("full width drags preserve yaw continuity", continuous);
        inject_map_button(MOUSE_BUTTON_MIDDLE, centre, false, true);
        inject_map_ctrl(false);
        break;
    }
    // A Ctrl click does not reset; a plain middle click resets the view in place.
    case 46:
        inject_map_ctrl(true);
        inject_map_button(MOUSE_BUTTON_MIDDLE, centre, true, true);
        break;
    case 47:
        inject_map_button(MOUSE_BUTTON_MIDDLE, centre, false, true);
        inject_map_ctrl(false);
        break;
    case 49:
        check("Ctrl click keeps the view", locked ? unchanged()
            : map_camera->view_resets() == 0U
                && map_camera->controller().yaw_degrees() != map_camera->config().yaw_degrees);
        map_camera_mark = map_camera->frame().target;
        inject_map_button(MOUSE_BUTTON_MIDDLE, centre);
        break;
    case 50: inject_map_button(MOUSE_BUTTON_MIDDLE, centre, false); break;
    case 52: {
        // The reset step's own trace entry: terrain following may ease the
        // height on later frames, but the reset itself keeps all three axes.
        const auto& trace = map_camera->trace();
        const bool kept = !trace.empty() && map_camera->trace_dropped() == 0U
            && trace.back().view_resets == 1U
            && trace.back().target_after == trace.back().target_before;
        check("middle click resets the view around the target", locked ? unchanged()
            : map_camera->view_resets() == 1U
                && map_camera->controller().yaw_degrees() == map_camera->config().yaw_degrees
                && map_camera->controller().state().zoom == map_camera->config().zoom
                && map_camera->controller().orbit_pitch_offset() == 0.0F
                && map_camera->frame().target[0] == map_camera_mark[0]
                && map_camera->frame().target[2] == map_camera_mark[2] && kept);
        break;
    }
    default: break;
    }
    // The final interactive camera change is deliberately later than the
    // normal timed window. Godot reads the preceding rendered frame, so the
    // terminal capture must follow a complete settled draw at this pose.
    const std::uint32_t terminal = options.warmup_frames + options.timed_frames;
    if (!locked && options.capture_path.empty()) {
        if (tick == terminal + 1U) inject_map_key(KEY_D, true);
        if (tick == terminal + 2U) {
            if (auto moved = map_camera->step(1.0F); !moved) {
                failure = core::format_diagnostic(moved.error());
            }
        }
        if (tick == terminal + 3U) inject_map_key(KEY_D, false);
    }
}

bool MapMode::State::verify_unlocked_capture(const CaptureResult& capture) {
    PackedByteArray encoded;
    encoded.resize(static_cast<int64_t>(capture.png_bytes.size()));
    if (!capture.png_bytes.empty()) {
        std::memcpy(encoded.ptrw(), capture.png_bytes.data(), capture.png_bytes.size());
    }
    Ref<Image> image;
    image.instantiate();
    if (image->load_png_from_buffer(encoded) != OK || image->is_empty()) {
        failure = "unlocked camera capture PNG could not be decoded";
        return false;
    }
    const Color background = image->get_pixel(0, 0);
    std::size_t sampled{};
    std::size_t changed{};
    for (int32_t y = 0; y < image->get_height(); y += 2) {
        for (int32_t x = 0; x < image->get_width(); x += 2) {
            const Color pixel = image->get_pixel(x, y);
            const float difference = std::abs(pixel.r - background.r)
                + std::abs(pixel.g - background.g) + std::abs(pixel.b - background.b);
            ++sampled;
            if (difference > 0.04F) ++changed;
        }
    }
    unlocked_changed_pixel_coverage = static_cast<float>(changed) / static_cast<float>(sampled);
    if (changed < 64 || unlocked_changed_pixel_coverage < 0.01F) {
        failure = "unlocked camera capture has insufficient drawn pixel coverage";
        return false;
    }
    if (map_camera_selftest) {
        const Ref<Image> before = decode_png(map_camera_terminal_before);
        if (before.is_null() || before->get_size() != image->get_size()) {
            failure = "terminal camera control image is missing or has different dimensions";
            return false;
        }
        for (int32_t y = 0; y < image->get_height(); y += 2) {
            for (int32_t x = 0; x < image->get_width(); x += 2) {
                if (before->get_pixel(x, y) != image->get_pixel(x, y)) {
                    ++map_camera_terminal_changed_pixels;
                }
            }
        }
        if (map_camera_terminal_changed_pixels < 64) {
            failure = "terminal camera movement did not change rendered pixels";
            return false;
        }
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
