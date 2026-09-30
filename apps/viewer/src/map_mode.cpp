#include "map_mode.hpp"
#include "map_mode_internal.hpp"
#include "eawr/presentation/camera/overview.hpp"
#include "render_profile_viewport.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace eawr::presentation::godot_backend {
namespace {


// Clean-room Godot spatial adapters authored for this slice. They are the
// modern_spatial route, not a translation of the TERRAIN/SKYDOME effects: the
// legacy route's selector allowlist has no terrain or sky entry and fails
// closed, which is recorded per surface rather than worked around.
// The terrain surface blends every material layer per fragment
// (land_look::terrain_shader, P1-06 #27). Unshaded unless a lighting policy is
// requested; the lit variant is 2 * E(n) * blended diffuse, where E(n) = n4' M
// n4 is the scene irradiance bound as eawr_sph_r/g/b and the doubling is the
// MODULATE2X of the descriptor's live fixed TerrainRenderBump t3_p0 cascade.
// Receiving the directional shadow needs the lit pipeline, so ambient is
// disabled and light() contributes exactly mix(shadow_floor, 1, ATTENUATION)
// per channel.
const std::string terrain_surface_shader = land_look::terrain_shader(false);
const std::string terrain_lit_shader = land_look::terrain_shader(true);

// Fixed variants of the authored terrain shaders. Only the fog declarations,
// world-position varying and one RGB multiply are inserted; render modes,
// lighting, alpha and pass order remain those of the source adapters.
[[nodiscard]] std::string fog_terrain_shader(const bool lit) {
    return land_look::with_map_fog(lit ? terrain_lit_shader : terrain_surface_shader);
}

constexpr std::string_view skydome_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never;
uniform sampler2D eawr_diffuse : filter_linear_mipmap, repeat_enable;
void vertex() {
    vec4 clip = PROJECTION_MATRIX * MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
    POSITION = vec4(clip.xy, clip.w * CLIP_SPACE_FAR, clip.w);
}
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb;
}
)GODOT";

template <class T> [[nodiscard]] std::optional<T> parse_unsigned(const std::string_view text) {
    T result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return result;
}

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

[[nodiscard]] camera::TacticalFrame camera_frame(const FixedCamera& source) {
    return {source.width, source.height, source.vertical_fov_degrees,
        source.near_plane, source.far_plane, source.eye, source.target, source.up};
}

[[nodiscard]] FixedCamera render_camera(const camera::TacticalFrame& source) {
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

[[nodiscard]] std::uint8_t camera_modifiers(const InputEventWithModifiers& event) {
    std::uint8_t result{};
    if (event.is_shift_pressed()) result |= viewer::camera_input::modifier::shift;
    if (event.is_ctrl_pressed()) result |= viewer::camera_input::modifier::ctrl;
    if (event.is_alt_pressed()) result |= viewer::camera_input::modifier::alt;
    if (event.is_meta_pressed()) result |= viewer::camera_input::modifier::meta;
    return result;
}

[[nodiscard]] std::optional<viewer::camera_input::RawEvent> map_camera_event(
    const Ref<InputEvent>& event) {
    namespace input = viewer::camera_input;
    input::RawEvent raw;
    if (const auto* key = Object::cast_to<InputEventKey>(event.ptr())) {
        const Key code = key->get_physical_keycode() != KEY_NONE
            ? key->get_physical_keycode() : key->get_keycode();
        raw.kind = input::RawKind::key;
        raw.code = input::key_code(std::string(OS::get_singleton()->get_keycode_string(code).utf8().get_data()))
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
        case MOUSE_BUTTON_LEFT: raw.code = input::mouse_code::left; break;
        case MOUSE_BUTTON_RIGHT: raw.code = input::mouse_code::right; break;
        case MOUSE_BUTTON_MIDDLE: raw.code = input::mouse_code::middle; break;
        case MOUSE_BUTTON_WHEEL_UP:
            raw.kind = input::RawKind::mouse_wheel;
            raw.code = input::mouse_code::wheel_up;
            raw.factor = button->get_factor();
            return raw;
        case MOUSE_BUTTON_WHEEL_DOWN:
            raw.kind = input::RawKind::mouse_wheel;
            raw.code = input::mouse_code::wheel_down;
            raw.factor = button->get_factor();
            return raw;
        default: return std::nullopt;
        }
        raw.kind = input::RawKind::mouse_button;
        return raw;
    }
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        raw.kind = input::RawKind::mouse_motion;
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

void inject_map_key(const Key code, const bool pressed) {
    Ref<InputEventKey> event;
    event.instantiate();
    event->set_keycode(code);
    event->set_physical_keycode(code);
    event->set_pressed(pressed);
    Input::get_singleton()->parse_input_event(event);
}

// The Ctrl key's own events, as the platform sends them around a Ctrl gesture: the press and
// its auto-repeat carry the Ctrl bit, the release does not.
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

[[nodiscard]] sim::math::Mat3x4 identity_transform() {
    using Fixed = sim::math::Fixed;
    const Fixed zero = Fixed::from_raw(0);
    const Fixed one = Fixed::from_raw(Fixed::scale);
    sim::math::Mat3x4 result{};
    result.rows[0] = {one, zero, zero, zero};
    result.rows[1] = {zero, one, zero, zero};
    result.rows[2] = {zero, zero, one, zero};
    return result;
}
} // namespace

namespace map_mode_detail {

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](const char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
        };
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

[[nodiscard]] std::string hash_bytes(const std::span<const std::byte> bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}


// Authored names carry the source-art suffix where the shipped asset often
// carries another, so a reference is probed as written and then by stem. This
// mirrors the asset layer's own rule rather than inventing a second one.
[[nodiscard]] std::optional<std::string> probe_reference(
    const vfs::Vfs& filesystem, const std::string_view root,
    const std::string_view name, const std::span<const std::string_view> suffixes) {
    std::string canonical;
    canonical.reserve(name.size());
    for (const char character : name) {
        const char folded = character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A')) : character;
        canonical.push_back(folded == '\\' ? '/' : folded);
    }
    if (canonical.empty()) return std::nullopt;
    const std::string base = std::string(root) + canonical;
    if (filesystem.stat(base)) return base;
    std::string stem = canonical;
    for (const std::string_view suffix : suffixes) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    for (const std::string_view suffix : suffixes) {
        const std::string candidate = std::string(root) + stem + std::string(suffix);
        if (filesystem.stat(candidate)) return candidate;
    }
    return std::nullopt;
}

[[nodiscard]] assets::Texture placeholder_texture() {
    assets::MipLevel mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.bytes = {std::byte{190}, std::byte{190}, std::byte{190}, std::byte{255}};
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.format = assets::PixelFormat::rgba8;
    texture.mips.push_back(std::move(mip));
    return texture;
}

} // namespace map_mode_detail

MapMode::MapMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
MapMode::~MapMode() = default;
MapMode::MapMode(MapMode&&) noexcept = default;
MapMode& MapMode::operator=(MapMode&&) noexcept = default;

bool MapMode::ready(Node3D& host) {
    State& state = *state_;
    std::string lighting_argument = state.options.lighting;
    std::string shadows_argument = state.options.shadows;
    std::string environment_argument = state.options.environment;
    std::string bloom_argument;
    std::string fog_argument_error;
    {
        const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
        for (int64_t index = 0; index < arguments.size(); ++index) {
            const String argument = arguments[index];
            const bool has_value = index + 1 < arguments.size();
            {
                // --eawr-live-* (#80): the live tactical session.
                const std::optional<std::string> value = has_value
                    ? std::optional<std::string>(String(arguments[index + 1]).utf8().get_data()) : std::nullopt;
                bool value_used = false;
                std::string live_error;
                if (LiveSessionView::parse_argument(argument.utf8().get_data(), value, state.live_options, value_used,
                                                    live_error)) {
                    if (!live_error.empty()) fog_argument_error = live_error;
                    if (value_used) ++index;
                    continue;
                }
            }
            if (argument == String("--eawr-audio")) {
                if (!has_value) fog_argument_error = "missing value for --eawr-audio";
                else state.audio_argument = std::string(String(arguments[++index]).utf8().get_data());
                continue;
            }
            if (argument == String("--eawr-populate") && !state.options.populate) state.populate = true;
            if (argument == String("--eawr-map-camera-config")) {
                if (!has_value) fog_argument_error = "missing value for --eawr-map-camera-config";
                else state.map_camera_config_path = std::string(String(arguments[++index]).utf8().get_data());
                continue;
            }
            if (argument == String("--eawr-space-place-object")) {
                if (!has_value) {
                    fog_argument_error = "missing value for --eawr-space-place-object";
                } else {
                    const std::string specification = String(arguments[++index]).utf8().get_data();
                    const std::size_t separator = specification.find('@');
                    const std::string object_id = specification.substr(0, separator);
                    const auto valid_id = [](const std::string& id) {
                        return !id.empty() && std::all_of(id.begin(), id.end(), [](const unsigned char c) {
                            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                                || (c >= '0' && c <= '9') || c == '_';
                        });
                    };
                    auto ship = SpacePopulation::Options::DebugShip{.object_id = object_id};
                    bool valid = valid_id(object_id);
                    if (separator != std::string::npos) {
                        const auto record = parse_unsigned<std::uint32_t>(
                            std::string_view(specification).substr(separator + 1));
                        valid = valid && specification.find('@', separator + 1) == std::string::npos
                            && record.has_value();
                        if (record) ship.spawn_record = *record;
                    }
                    if (!valid) {
                        fog_argument_error = "--eawr-space-place-object expects <SpaceUnit or StarBase XML id>[@<marker record index>]";
                    } else state.space_place_object = std::move(ship);
                }
                continue;
            }
            if (argument == String("--eawr-space-place-at")) {
                // <FoC SpaceUnit XML id>@<x>,<y>,<z>,<yaw degrees> in TED source units.
                constexpr const char* usage = "--eawr-space-place-at expects <SpaceUnit XML id>@<x>,<y>,<z>,<yaw>";
                if (!has_value) {
                    fog_argument_error = "missing value for --eawr-space-place-at";
                    continue;
                }
                const std::string specification = String(arguments[++index]).utf8().get_data();
                const std::size_t separator = specification.find('@');
                SpacePopulation::Options::PlacedShip ship;
                ship.object_id = specification.substr(0, separator);
                std::vector<float> values;
                bool valid = separator != std::string::npos && !ship.object_id.empty()
                    && std::all_of(ship.object_id.begin(), ship.object_id.end(), [](const unsigned char c) {
                           return std::isalnum(c) != 0 || c == '_' || c == '-';
                       });
                std::size_t start = separator == std::string::npos ? specification.size() : separator + 1;
                while (valid && start <= specification.size()) {
                    const std::size_t comma = specification.find(',', start);
                    const std::string item = specification.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    char* end = nullptr;
                    const double value = std::strtod(item.c_str(), &end);
                    valid = !item.empty() && end == item.c_str() + item.size() && std::isfinite(value);
                    values.push_back(static_cast<float>(value));
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                if (!valid || values.size() != 4 || state.space_place_at.size() >= 256) {
                    fog_argument_error = usage;
                } else {
                    ship.position = {values[0], values[1], values[2]};
                    ship.yaw_degrees = values[3];
                    state.space_place_at.push_back(std::move(ship));
                }
                continue;
            }
            if (argument == String("--eawr-space-hardpoint-state")) {
                if (!has_value) {
                    fog_argument_error = "missing value for --eawr-space-hardpoint-state";
                } else {
                    const std::string specification = String(arguments[++index]).utf8().get_data();
                    const std::size_t separator = specification.find('=');
                    const auto parsed = separator == std::string::npos ? std::nullopt
                        : scene::parse_hardpoint_state(std::string_view(specification).substr(separator + 1));
                    if (separator == 0 || !parsed) {
                        fog_argument_error = "--eawr-space-hardpoint-state expects <HardPoint id>=<intact|damaged|destroyed> (damaged looks intact; destroyed removes the model and shows damage art)";
                    } else {
                        state.space_hardpoint_states.emplace_back(specification.substr(0, separator), *parsed);
                    }
                }
                continue;
            }
            if (argument == String("--eawr-map-water-time")) {
                if (!has_value) {
                    fog_argument_error = "missing value for --eawr-map-water-time";
                } else {
                    const std::string value(String(arguments[++index]).utf8().get_data());
                    char* end = nullptr;
                    const float parsed = std::strtof(value.c_str(), &end);
                    if (end != value.c_str() + value.size() || !std::isfinite(parsed)) {
                        fog_argument_error = "--eawr-map-water-time expects a finite number of seconds";
                    } else {
                        state.water_capture_time = parsed;
                    }
                }
                continue;
            }
            if (argument == String("--eawr-map-camera-selftest")) {
                state.map_camera_selftest = true;
                continue;
            }
            // P1-06 #27 framing and frame-time options (land_look.hpp).
            if (argument == String("--eawr-benchmark")) {
                state.benchmark = true;
                continue;
            }
            if (argument == String("--eawr-map-view") || argument == String("--eawr-map-zoom")
                || argument == String("--eawr-map-yaw") || argument == String("--eawr-map-target")) {
                const std::string name(argument.utf8().get_data());
                if (!has_value) {
                    fog_argument_error = "missing value for " + name;
                    continue;
                }
                const std::string value(String(arguments[++index]).utf8().get_data());
                const auto number = [](const std::string& text) -> std::optional<float> {
                    if (text.empty()) return std::nullopt;
                    char* end = nullptr;
                    const float parsed = std::strtof(text.c_str(), &end);
                    if (end != text.c_str() + text.size() || !std::isfinite(parsed)) return std::nullopt;
                    return parsed;
                };
                if (name == "--eawr-map-view") {
                    if (value != "tactical" && value != "overview") {
                        fog_argument_error = "--eawr-map-view expects tactical or overview";
                    } else {
                        state.requested_view = value;
                    }
                } else if (name == "--eawr-map-target") {
                    const std::size_t comma = value.find(',');
                    const auto x = comma == std::string::npos ? std::nullopt : number(value.substr(0, comma));
                    const auto y = comma == std::string::npos ? std::nullopt : number(value.substr(comma + 1));
                    if (!x || !y) fog_argument_error = "--eawr-map-target expects <x>,<y> in source units";
                    else state.requested_target = std::array<float, 2>{*x, *y};
                } else if (const auto parsed = number(value)) {
                    if (name == "--eawr-map-zoom") state.requested_zoom = *parsed;
                    else state.requested_yaw = *parsed;
                } else {
                    fog_argument_error = name + " expects a finite number";
                }
                continue;
            }
            if (argument == String("--eawr-map-free-selftest")) {
                state.map_free_selftest = true;
                continue;
            }
            if (argument == String("--eawr-map-camera-terminal-hold-test")) {
                state.map_camera_terminal_hold_test = true;
                continue;
            }
            if (argument == String("--eawr-map-camera-terminal-baseline-test")) {
                state.map_camera_terminal_baseline_test = true;
                continue;
            }
            if (argument == String("--eawr-map-camera-terminal-release-test")) {
                state.map_camera_terminal_release_test = true;
                continue;
            }
            if (argument == String("--eawr-map-free-terminal-hold-test")) {
                state.map_free_terminal_hold_test = true;
                continue;
            }
            if (argument == String("--eawr-map-free-terminal-release-test")) {
                state.map_free_terminal_release_test = true;
                continue;
            }
            if (argument == String("--eawr-map-camera-unlocked-capture")) {
                if (!has_value) fog_argument_error = "missing value for --eawr-map-camera-unlocked-capture";
                else state.map_camera_unlocked_capture_path =
                    std::string(String(arguments[++index]).utf8().get_data());
                continue;
            }
            if (argument == String("--eawr-map-effects") && has_value) {
                if (arguments[index + 1] == String("off")) state.options.map_effects = false;
                else if (arguments[index + 1] == String("on")) state.options.map_effects = true;
                else fog_argument_error = "--eawr-map-effects expects on or off";
                ++index;
                continue;
            }
            if (argument == String("--eawr-map-effects") && !has_value) {
                fog_argument_error = "missing value for --eawr-map-effects";
            }
            if (argument == String("--eawr-map-effect-animation")) {
                if (!has_value) {
                    fog_argument_error = "missing value for --eawr-map-effect-animation";
                } else {
                    const String value = arguments[++index];
                    if (value == String("none")) {
                        state.options.effect_animation = Options::EffectAnimation::none;
                    } else if (value == String("idle")) {
                        state.options.effect_animation = Options::EffectAnimation::idle;
                    } else {
                        fog_argument_error = "--eawr-map-effect-animation expects none or idle";
                    }
                }
                continue;
            }
            if (argument == String("--eawr-map-particle-seed")
                || argument == String("--eawr-map-particle-frames")
                || argument == String("--eawr-map-particle-capacity")
                || argument == String("--eawr-map-effect-alt")
                || argument == String("--eawr-map-effect-lod")
                || argument == String("--eawr-map-attached-capacity")
                || argument == String("--eawr-map-idle-offset")
                || argument == String("--eawr-map-timed-frames")) {
                if (!has_value) {
                    fog_argument_error = "missing value for " + std::string(argument.utf8().get_data());
                } else {
                    const auto parsed = parse_unsigned<std::uint32_t>(
                        std::string_view(String(arguments[index + 1]).utf8().get_data()));
                    if (!parsed) fog_argument_error = "map particle option requires a uint32";
                    else if (argument == String("--eawr-map-particle-seed")) state.options.particle_seed = *parsed;
                    else if (argument == String("--eawr-map-particle-frames")) state.options.particle_frames = *parsed;
                    else if (argument == String("--eawr-map-particle-capacity")) {
                        state.options.particle_capacity = *parsed;
                        state.options.particle_capacity_explicit = true;
                    }
                    else if (argument == String("--eawr-map-effect-alt")) state.options.effect_alt = *parsed;
                    else if (argument == String("--eawr-map-effect-lod")) state.options.effect_lod = *parsed;
                    else if (argument == String("--eawr-map-idle-offset")) state.idle_offset = *parsed;
                    else if (argument == String("--eawr-map-timed-frames")) state.options.timed_frames = *parsed;
                    else {
                        state.options.attached_capacity = *parsed;
                        state.options.attached_capacity_explicit = true;
                    }
                    ++index;
                }
                continue;
            }
            if (argument == String("--eawr-environment-record")) {
                if (!has_value) {
                    fog_argument_error = "missing value for --eawr-environment-record";
                } else {
                    state.environment_record_argument = parse_unsigned<std::uint32_t>(
                        std::string_view(String(arguments[++index]).utf8().get_data()));
                    if (!state.environment_record_argument) {
                        fog_argument_error = "--eawr-environment-record requires a uint32";
                    }
                }
                continue;
            }
            if (argument == String("--eawr-fog-paint")) state.fog_paint_requested = true;
            if (argument == String("--eawr-fog-inject-input")) state.fog_inject_input = true;
            if (argument == String("--eawr-space-populate-reject-upload-test"))
                state.space_populate_reject_upload_test = true;
            if (!has_value) {
                if (argument == String("--eawr-fog-grid") || argument == String("--eawr-fog-sha256")
                    || argument == String("--eawr-fog-team") || argument == String("--eawr-fog-revision")
                    || argument == String("--eawr-fog-tick")
                    || argument == String("--eawr-fog-paint-evidence")
                    || argument == String("--eawr-space-fog-admit")) {
                    fog_argument_error = "missing value for " + std::string(argument.utf8().get_data());
                }
                continue;
            }
            const CharString value = String(arguments[index + 1]).utf8();
            if (argument == String("--eawr-fog-grid")) {
                state.fog_paths.emplace_back(value.get_data());
                ++index;
                continue;
            }
            if (argument == String("--eawr-fog-sha256")) {
                state.fog_expected_hashes.emplace_back(value.get_data());
                ++index;
                continue;
            }
            if (argument == String("--eawr-fog-paint-evidence")) {
                state.fog_paint_evidence = value.get_data();
                ++index;
                continue;
            }
            if (argument == String("--eawr-space-fog-admit")) {
                state.space_fog_admit.emplace_back(value.get_data());
                ++index;
                continue;
            }
            if (argument == String("--eawr-fog-team")) {
                state.fog_team = parse_unsigned<std::uint32_t>(value.get_data());
                if (!state.fog_team) fog_argument_error = "--eawr-fog-team requires a uint32";
                ++index;
                continue;
            }
            if (argument == String("--eawr-fog-tick")) {
                state.fog_tick = parse_unsigned<std::uint64_t>(value.get_data());
                if (!state.fog_tick) fog_argument_error = "--eawr-fog-tick requires a uint64";
                ++index;
                continue;
            }
            if (argument == String("--eawr-fog-revision")) {
                state.fog_revision = parse_unsigned<std::uint64_t>(value.get_data());
                if (!state.fog_revision) fog_argument_error = "--eawr-fog-revision requires a uint64";
                ++index;
                continue;
            }
            if (argument == String("--eawr-lighting") && lighting_argument.empty()) lighting_argument = value.get_data();
            if (argument == String("--eawr-shadows") && shadows_argument.empty()) shadows_argument = value.get_data();
            if (argument == String("--eawr-environment") && environment_argument.empty()) {
                environment_argument = value.get_data();
            }
            if (argument == String("--eawr-bloom") && bloom_argument.empty()) bloom_argument = value.get_data();
            if (argument == String("--eawr-space-camera")) state.space_camera = value.get_data();
            if (argument == String("--eawr-space-control")) state.space_control = value.get_data();
        }
    }
    if (state.options.populate) state.populate = *state.options.populate;
    const auto give_up = [&](std::string message) {
        state.failure = std::move(message);
        state.status = "failed";
        state.release_particles();
        static_cast<void>(state.write_report());
        return false;
    };
    if (!fog_argument_error.empty()) return give_up(fog_argument_error);
    if (!state.audio_argument.empty() && state.audio_argument != "on" && state.audio_argument != "off") {
        return give_up("--eawr-audio expects on or off");
    }
    if (!state.audio_argument.empty() && state.live_options.fixture.empty()) {
        return give_up("--eawr-audio requires --eawr-live-session");
    }
    if (const std::string perf_error =
            parse_perf_overlay_argument(OS::get_singleton()->get_cmdline_user_args(), state.perf_requested);
        !perf_error.empty()) {
        return give_up(perf_error);
    }
    {
        std::string trace_error;
        const auto trace = PerfTrace::parse(OS::get_singleton()->get_cmdline_user_args(), trace_error);
        if (!trace_error.empty()) return give_up(trace_error);
        if (trace && state.live_options.fixture.empty()) return give_up("--eawr-perf-trace requires --eawr-live-session");
        if (trace && !state.perf_trace.emplace().open(*trace, trace_error)) return give_up(trace_error);
    }
    if (const std::string hud_error = parse_hud_arguments(OS::get_singleton()->get_cmdline_user_args(),
                                                          !state.live_options.fixture.empty(), state.hud_options,
                                                          state.hud_faction_given);
        !hud_error.empty()) {
        return give_up(hud_error);
    }
    if (state.live_options.fixture.empty()
        && (state.live_options.player || !state.live_options.orders.empty() || !state.live_options.capture_ticks.empty()
            || !state.live_options.follow_groups.empty()
            || state.live_options.end_tick || state.live_options.workers || !state.live_options.hashes_path.empty()
            || !state.live_options.replay_path.empty())) {
        return give_up("--eawr-live-* options require --eawr-live-session");
    }
    if (state.benchmark) {
        // The recorded frame time is not capped by the display refresh.
        if (auto* display = DisplayServer::get_singleton()) {
            display->window_set_vsync_mode(DisplayServer::VSYNC_DISABLED);
        }
    }
    if ((state.map_camera_selftest || state.map_free_selftest) && state.map_camera_config_path.empty()) {
        return give_up("map camera selftest requires --eawr-map-camera-config");
    }
    if (state.map_camera_terminal_baseline_test && (state.map_camera_config_path.empty()
        || state.map_camera_selftest || !state.options.capture_path.empty()
        || state.map_camera_terminal_hold_test || state.map_camera_terminal_release_test)) {
        return give_up("map camera terminal baseline test requires one unlocked camera config and no selftest");
    }
    if (state.map_camera_terminal_hold_test && (state.map_camera_config_path.empty()
        || state.map_camera_selftest || !state.options.capture_path.empty())) {
        return give_up("map camera terminal hold test requires an unlocked camera without selftest");
    }
    if (state.map_camera_terminal_release_test && (state.map_camera_config_path.empty()
        || state.map_camera_selftest || !state.options.capture_path.empty()
        || state.map_camera_terminal_hold_test || state.map_camera_terminal_baseline_test)) {
        return give_up("map camera terminal release test requires one unlocked camera config and no selftest");
    }
    if ((state.map_free_terminal_hold_test || state.map_free_terminal_release_test)
        && (state.map_camera_config_path.empty() || !state.options.capture_path.empty()
            || state.map_free_selftest || state.map_camera_selftest
            || (state.map_free_terminal_hold_test && state.map_free_terminal_release_test))) {
        return give_up("map free terminal test requires one unlocked camera config and no selftest");
    }
    if (!state.map_camera_unlocked_capture_path.empty()
        && (state.map_camera_config_path.empty() || !state.options.capture_path.empty())) {
        return give_up("unlocked camera capture requires an unlocked map camera");
    }
    if (state.options.camera_zoom && (state.map_camera_config_path.empty()
            || !std::isfinite(*state.options.camera_zoom)
            || *state.options.camera_zoom < 0.0F || *state.options.camera_zoom > 1.0F)) {
        return give_up("--eawr-camera-zoom with a map needs --eawr-map-camera-config and a zoom in [0, 1]");
    }
    if (state.options.interactive) {
        if (state.map_camera_config_path.empty() || !state.options.capture_path.empty()
            || !state.map_camera_unlocked_capture_path.empty() || state.map_camera_selftest
            || state.map_free_selftest || state.map_camera_terminal_baseline_test
            || state.map_camera_terminal_hold_test
            || state.map_camera_terminal_release_test || state.map_free_terminal_hold_test
            || state.map_free_terminal_release_test) {
            return give_up("--eawr-camera-interactive with a map needs --eawr-map-camera-config "
                           "and no capture or camera test");
        }
        // Never reached in practice (about 19 days at 60 frames per second):
        // the owner closes the window. Land and space loops both read it.
        state.options.timed_frames = 100'000'000U;
    }
    // --eawr-map-timed-frames (CI uses fewer on software Vulkan). The bound
    // keeps warmup plus timed frames inside uint32 frame counters.
    if (state.options.timed_frames == 0 || state.options.timed_frames > 100'000'000U) {
        return give_up("map timed frames must be in [1, 100000000]");
    }
    if (state.options.particle_frames == 0 || state.options.particle_capacity == 0) {
        return give_up("particle frames and capacity must be positive");
    }
    const bool fog_requested = !state.fog_paths.empty() || !state.fog_expected_hashes.empty()
        || state.fog_team || state.fog_revision || state.fog_tick
        || state.fog_paint_requested || state.fog_inject_input || !state.fog_paint_evidence.empty();
    if (!state.space_fog_admit.empty() && !fog_requested) {
        return give_up("--eawr-space-fog-admit requires the fog grid arguments");
    }
    if (state.effect_animation_idle() && fog_requested) {
        // Fog evidence binds one resource per emitter; generations and drains
        // of an owner would need per-generation evidence, which is not built.
        return give_up("--eawr-map-effect-animation idle is not supported with fog");
    }
    if (state.effect_animation_idle() && !state.populate) {
        return give_up("--eawr-map-effect-animation idle requires --eawr-populate");
    }
    if (state.effect_animation_idle() && !state.options.map_effects) {
        // Nothing would be spawned or verified: refuse instead of reporting
        // owners that never ran.
        return give_up("--eawr-map-effect-animation idle requires --eawr-map-effects on");
    }
    if (fog_requested) {
        if (state.fog_paths.empty() || !state.fog_team || !state.fog_revision || !state.fog_tick
            || state.fog_expected_hashes.size() != state.fog_paths.size()) {
            return give_up("fog requires each --eawr-fog-grid with --eawr-fog-sha256, plus --eawr-fog-team, --eawr-fog-revision and --eawr-fog-tick");
        }
        if (state.fog_paint_requested && !state.options.capture_path.empty()) {
            return give_up("--eawr-fog-paint contradicts fixed --eawr-capture");
        }
        if (!state.fog_paint_evidence.empty()
            && (!state.options.capture_path.empty() || state.fog_paint_requested)) {
            return give_up("--eawr-fog-paint-evidence manages painting and requires no fixed capture");
        }
        std::string fog_error;
        state.fog = FogMode::load(state.fog_paths, *state.fog_team, *state.fog_tick, fog_error);
        if (!state.fog) return give_up(fog_error);
        for (std::size_t i = 0; i < state.fog_expected_hashes.size(); ++i) {
            if (state.fog_expected_hashes[i] != state.fog->sources()[i].sha256) {
                return give_up("fog source SHA-256 mismatch: " + state.fog_paths[i].string());
            }
        }
        if (state.fog->source().find(*state.fog_team)->revision() != *state.fog_revision) {
            return give_up("selected fog grid revision does not match --eawr-fog-revision");
        }
        if (!state.options.capture_path.empty()) state.fog->lock_capture();
        else if (state.fog_paint_requested) static_cast<void>(state.fog->set_painting(true));
    }
    if (!lighting_argument.empty()) {
        const auto parsed = lighting::parse_policy(lighting_argument);
        if (!parsed) return give_up("--eawr-lighting must be sh, hemisphere or off");
        state.policy = *parsed;
    }
    if (!shadows_argument.empty()) {
        if (shadows_argument != "on" && shadows_argument != "off") return give_up("--eawr-shadows must be on or off");
        state.shadows = shadows_argument == "on";
    }
    if (!bloom_argument.empty()) {
        if (bloom_argument != "on" && bloom_argument != "off") return give_up("--eawr-bloom must be on or off");
        state.bloom = bloom_argument == "on";
    }
    if (!environment_argument.empty()) {
        if (environment_argument != "default" && environment_argument != "map") {
            return give_up("--eawr-environment must be default or map");
        }
        state.environment_choice = environment_argument;
    }
    if (state.shadows && state.policy == lighting::Policy::off) {
        return give_up("--eawr-shadows on needs a lighting policy: shadows are received by the lit adapters");
    }
    // #307: bloom on an unlit debug image (full-bright textures) means nothing,
    // so without a lighting policy the default is no bloom.
    if (state.policy == lighting::Policy::off) {
        if (bloom_argument == "on") return give_up("--eawr-bloom on needs a lighting policy: an unlit image does not bloom");
        state.bloom_skipped_unlit = state.bloom;
        state.bloom = false;
    }

    // The installed layers select the default; an explicit profile keeps the
    // base-game corpus available for regression fixtures.
    const std::filesystem::path expansion = state.options.game_root / "corruption" / "Data";
    const std::filesystem::path base = state.options.game_root / "GameData" / "Data";
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    if (!std::filesystem::is_directory(base)) {
        return give_up("map mode requires an installation with GameData/Data");
    }
    state.profile = state.options.profile.empty() ? (!state.options.mod_root.empty()
        ? "remake" : std::filesystem::is_directory(expansion) ? "foc" : "eaw")
        : state.options.profile;
    if (state.profile != "eaw" && state.profile != "foc" && state.profile != "remake") {
        return give_up("--eawr-profile must be eaw, foc or remake");
    }
    if (state.profile == "remake") {
        if (state.options.mod_root.empty()) return give_up("the Remake profile requires --eawr-mod-root");
        if (!std::filesystem::is_directory(expansion)) {
            return give_up("the Remake profile requires corruption/Data");
        }
        for (const auto& layer : eawr::vfs::mod_chain_roots(state.options.mod_root)) roots.push_back(layer);
        roots.emplace_back("expansion", expansion);
        roots.emplace_back("base", base);
    } else {
        if (state.profile == "foc") {
            if (!std::filesystem::is_directory(expansion)) return give_up("the FoC profile requires corruption/Data");
            roots.emplace_back("expansion", expansion);
        }
        roots.emplace_back("base", base);
    }
    std::vector<vfs::MountSpec> specs;
    auto chain = vfs::resolve_manifest_chain(roots);
    if (!chain) return give_up(core::format_diagnostic(chain.error()));
    for (auto& manifest : chain.value()) {
        state.layers.push_back(manifest.mount.layer_id);
        specs.push_back(std::move(manifest.mount));
    }
    auto filesystem = vfs::Vfs::mount(specs);
    if (!filesystem) return give_up(core::format_diagnostic(filesystem.error()));
    state.filesystem.emplace(std::move(filesystem.value()));

    // The catalog is needed because a TED skydome field names an XML object,
    // not an art file. A catalog that cannot load is a recorded diagnostic, not
    // a silently skipped skydome.
    assets::ObjectTypeCatalog catalog;
    std::string catalog_failure;
    const data::Profile profile = state.profile == "remake" ? data::Profile::remake
        : state.profile == "foc" ? data::Profile::foc : data::Profile::eaw;
    if (auto loaded = data::load_catalog(*state.filesystem, profile)) {
        catalog = assets::object_type_catalog(loaded.value().catalog);
        state.catalog = std::move(loaded.value().catalog);
    } else {
        catalog_failure = core::format_diagnostic(loaded.error());
    }

    auto map_bytes = state.filesystem->open(state.options.map_path);
    if (!map_bytes) return give_up(core::format_diagnostic(map_bytes.error()));
    state.map_hash = hash_bytes(map_bytes.value());
    auto loaded_map = assets::load_map(*state.filesystem, state.options.map_path, catalog);
    if (!loaded_map) return give_up(core::format_diagnostic(loaded_map.error()));
    const assets::Map& map = loaded_map.value();
    state.semantic_complete = map.semantic_complete;
    state.map_kind = !map.kind ? "unknown"
        : (*map.kind == assets::MapKind::land ? "land" : "space");
    state.environment_records = map.environments.size();
    if (state.environment_record_argument) {
        if (*state.environment_record_argument >= map.environments.size()) {
            return give_up("--eawr-environment-record " + std::to_string(*state.environment_record_argument)
                + ": the map declares " + std::to_string(map.environments.size()) + " environment records");
        }
        // The space view composes its sky, light and backdrop from record 0.
        if (map.kind == assets::MapKind::space && *state.environment_record_argument != 0) {
            return give_up("--eawr-environment-record applies to land maps; the space view shows environment 0");
        }
        state.environment_record = *state.environment_record_argument;
    }
    if (state.environment_record < map.environments.size()) {
        state.environment_record_name = map.environments[state.environment_record].name.value_or("");
    }
    state.water_records = map.water_records.size();
    // #201: retail land and space battles bloom with the current environment's
    // parameters; the viewer's current environment is the selected record
    // (--eawr-environment-record, default 0).
    if (state.bloom) {
        state.scene_bloom = map.environments.empty()
            ? lighting::bloom::SceneBloom{} : lighting::bloom::environment_bloom(map.environments[state.environment_record]);
    }

    // Water is composed with the terrain below (land_look::compose_water): an
    // approximate plane and river ribbons, never a TerrainWater technique.
    state.water_status = "absent";
    state.water_cause = "water is composed only for a land map";
    state.nebula_status = state.map_kind == "space"
        ? "classified_not_rendered" : "not_applicable";

    // A kind-2 map has no terrain, and terrain::build rightly rejects it, so
    // composition branches on the validated kind instead: only the land path
    // below builds terrain, and a land map without terrain still fails there.
    if (map.kind == assets::MapKind::space) {
        if (state.effect_animation_idle()) {
            return give_up("--eawr-map-effect-animation idle applies only to land maps");
        }
        std::optional<eawr::viewer::MapCameraSource> space_camera_source;
        if (!state.map_camera_config_path.empty()) {
            // P1 #30: the space tactical camera is tactical only. Free flight
            // has no space policy, so its opt-ins are refused explicitly.
            if (state.map_free_selftest || state.map_free_terminal_hold_test
                || state.map_free_terminal_release_test) {
                return give_up("the space map camera has no free flight: free-camera options are unsupported");
            }
            std::string camera_failure;
            space_camera_source = state.load_map_camera(camera::Mode::space, camera_failure);
            if (!space_camera_source) return give_up(camera_failure);
        }
        if (state.fog) {
            // Opt-in synthetic space fog: units only from caller-declared XML
            // element types; the sky is never a fog consumer.
            if (const std::string invalid = space_fog::validate_admission(state.space_fog_admit); !invalid.empty()) {
                return give_up(invalid + "; no space placement classification is inferred");
            }
            if (state.fog_paint_requested || state.fog_inject_input) {
                return give_up("interactive fog painting and injected fog input are not wired for space maps; "
                               "use --eawr-fog-paint-evidence without --eawr-capture");
            }
        }
        const bool live = !state.live_options.fixture.empty();
        if (state.populate) {
            // P1-11 #32: the default environment view calls the population
            // hook after composing its sky and background placements.
            if (state.fog) return give_up("--eawr-populate is not composed with space fog, which admits its own units");
            if (!state.space_control.empty()) {
                return give_up("--eawr-populate takes no --eawr-space-control");
            }
            std::string environment_failure;
            const auto environment = SpacePopulation::environment(map, state.environment_choice, environment_failure);
            if (!environment) return give_up(environment_failure);
            if (!state.space_hardpoint_states.empty() && !state.space_place_object) {
                return give_up("--eawr-space-hardpoint-state requires --eawr-space-place-object");
            }
            if (state.space_place_object) state.space_place_object->hardpoint_states = state.space_hardpoint_states;
            if (state.space_place_object && !state.space_place_at.empty()) {
                return give_up("--eawr-space-place-at is not composed with --eawr-space-place-object");
            }
            std::vector<std::uint32_t> session_records;
            if (live) {
                // #80: the session's units are drawn from its snapshots, through
                // the placed-ship upload path.
                if (state.space_place_object || !state.space_place_at.empty()) {
                    return give_up("--eawr-live-session is not composed with --eawr-space-place-object or -at");
                }
                if (!state.catalog) return give_up("--eawr-live-session requires the XML catalog, which did not load");
                state.live_options.real_time = state.options.interactive;
                state.live_options.warmup_frames = state.options.warmup_frames;
                if (state.live_options.audio_pace && !state.live_options.real_time) {
                    // #474: see the Options::audio_pace comment (live_session_view.hpp). A frame at
                    // this rate covers exactly one tick's worth of real time, so BattleAudio's
                    // requests reach Godot's real mixer on the same real schedule as the battle's
                    // own tick rate, on every host.
                    const double fps = static_cast<double>(sim::tactical::logical_frames_per_second)
                        / state.live_options.ticks_per_frame;
                    Engine::get_singleton()->set_max_fps(std::max(1, static_cast<int>(std::llround(fps))));
                }
                state.live_session = std::make_unique<LiveSessionView>(state.live_options);
                std::string live_failure;
                if (!state.live_session->prepare(*state.filesystem, *state.catalog, state.options.map_path, live_failure)) {
                    return give_up(live_failure);
                }
                // #391: the breakoff props join the placed ships before they are composed.
                state.debris_props = std::make_unique<DebrisProps>(host, *state.filesystem, *state.catalog);
                state.live_session->attach_debris(*state.debris_props);
                // #456: so do the model projectiles' slots.
                state.battle_effects = std::make_unique<BattleEffects>(host, *state.filesystem, *state.catalog);
                state.battle_effects->prepare(*state.live_session->tables(), *state.live_session->combat());
                state.live_session->attach_projectile_models(*state.battle_effects);
                state.space_place_at = state.live_session->placed_ships();
                session_records = state.live_session->session_records();
                state.battle = std::make_unique<BattleInput>(host);
                state.battle->prepare(*state.filesystem, *state.catalog, *state.live_session);
                // #494: the local player's fog of war in the world.
                state.live_fog = std::make_unique<LiveFogView>(host);
                state.live_fog->prepare(*state.filesystem, state.live_options.reveal, state.live_options.deploy_overlay);
                // #76: the command bar's ability buttons read and switch the simulated abilities;
                // --eawr-live-ability-demo keeps the stand-in's staged states to look at.
                if (!state.live_session->options().ability_demo) {
                    state.battle->set_abilities(&state.live_session->abilities(), &state.live_session->abilities());
                }
                // #84: sound by default in the interactive view, muted in a capture run.
                const bool audible = state.audio_argument.empty() ? state.options.interactive : state.audio_argument == "on";
                state.battle_audio = std::make_unique<BattleAudio>(host, *state.filesystem, *state.catalog,
                                                                   BattleAudio::Options{.muted = !audible});
                state.battle_audio->prepare(*state.live_session->tables(), *state.live_session->combat(),
                                            state.live_session->local_faction());
                // #394: the live units' own emitters (engines, destroyed hardpoints' damage), which
                // the static attached plan cannot run on moving units.
                if (state.options.map_effects) state.unit_emitters = std::make_unique<UnitEmitters>(host, *state.filesystem);
            }
            state.space_population = std::make_unique<SpacePopulation>(SpacePopulation::Options{
                .map_sha256 = state.map_hash,
                .catalog = state.catalog ? &*state.catalog : nullptr,
                .policy = state.policy,
                .shadows = state.shadows,
                .environment = *environment,
                .environment_source = state.environment_choice,
                .animation_frames = state.options.particle_frames,
                .live_clock = state.options.interactive,
                .idle_offset = state.idle_offset.value_or(0U),
                .team_colour = [](const scene::Placement& placement) { return placement.team_colour; },
                .reject_upload_for_test = state.space_populate_reject_upload_test,
                .debug_ship = state.space_place_object,
                .placed_ships = state.space_place_at,
                .session_records = std::move(session_records),
                // The composed scene (the debug ship included) and its
                // hardpoint states re-plan the attached effects (#136). The
                // live session's units move: their emitters are UnitEmitters' (#394),
                // so this static plan plans none.
                .attached_effects = state.options.map_effects && state.catalog && !live
                    ? std::function<bool(SpacePopulation&)>([&state, host = &host](SpacePopulation& population) {
                          return state.sync_space_attached(*host, population);
                      })
                    : std::function<bool(SpacePopulation&)>{},
            });
        } else if (state.space_place_object || !state.space_hardpoint_states.empty()) {
            return give_up("--eawr-space-place-object requires --eawr-populate");
        } else if (!state.space_place_at.empty()) {
            return give_up("--eawr-space-place-at requires --eawr-populate");
        } else if (live) {
            return give_up("--eawr-live-session requires --eawr-populate");
        } else if (state.idle_offset) {
            return give_up("--eawr-map-idle-offset requires --eawr-populate");
        } else if (state.policy != lighting::Policy::off || state.shadows || state.environment_choice != "default") {
            return give_up("space map mode composes no lighting without --eawr-populate; the sky adapter is unshaded");
        }
        if (state.populate && state.options.map_effects && state.catalog && !live) {
            scene::VfsAssetCache cache(*state.filesystem);
            scene::BuildInput input;
            input.map = &map;
            input.map_sha256 = state.map_hash;
            input.catalog = &*state.catalog;
            input.access = cache.access();
            state.scene = scene::build(input);
            state.build_attached_plan(cache, state.space_population.get());
            if (!state.attached_animation_failure.empty()) return give_up(state.attached_animation_failure);
            if (!state.attached_plan.records.empty()) {
                state.particles = std::make_unique<MapParticleProvider>(host, *state.filesystem);
                if (!state.particles->prepare_attached(state.attached_plan, *state.scene)) {
                    return give_up("space attached effect preparation failed");
                }
            }
        }
        if (state.space_population) {
            SpacePopulation::AttachedEffects attached = state.space_attached_effects();
            if (live) attached.cause = "--eawr-live-session: the live units' emitters follow them under unit_emitters (#394)";
            state.space_population->set_attached_effects(std::move(attached));
        }
        // Population compose re-plans over its own scene, which may admit
        // emitters (the debug ship's) this map-only plan has none of.
        const bool attached_effects = state.populate && state.options.map_effects && state.catalog && !live;
        state.space = std::make_unique<SpaceEnvironment>(SpaceEnvironment::Options{
            .map_path = state.options.map_path,
            .map_sha256 = state.map_hash,
            .semantic_complete = state.semantic_complete,
            .profile = state.profile,
            .layers = state.layers,
            .report_path = state.options.report_path,
            .capture_path = state.options.capture_path,
            .warmup_frames = state.options.warmup_frames,
            .timed_frames = state.options.timed_frames,
            .real_time_clock = state.options.interactive,
            .clock_hold_ticks = state.options.particle_frames,
            .clock_offset = state.idle_offset.value_or(0U),
            .camera = state.space_camera,
            .control = state.space_control,
            .map_camera = std::move(space_camera_source),
            .camera_selftest = state.map_camera_selftest,
            .camera_terminal_baseline_test = state.map_camera_terminal_baseline_test,
            .camera_terminal_hold_test = state.map_camera_terminal_hold_test,
            .camera_terminal_release_test = state.map_camera_terminal_release_test,
            .unlocked_capture_path = state.map_camera_unlocked_capture_path,
            .bloom = state.scene_bloom,
            .bloom_skipped_unlit = state.bloom_skipped_unlit,
            .fog = state.fog ? &*state.fog : nullptr,
            .catalog = state.catalog ? &*state.catalog : nullptr,
            .fog_admit = state.space_fog_admit,
            .fog_paint_evidence = state.fog ? state.fog_paint_evidence : std::filesystem::path{},
            .populate = state.space_population
                ? SpacePopulateHook{[population = state.space_population.get(), filesystem = &*state.filesystem,
                                     live_session = state.live_session.get(), map_state = &state,
                                     effects = state.battle_effects.get(), debris = state.debris_props.get(),
                                     emitters = state.unit_emitters.get(), sound = state.battle_audio.get(),
                                     fog = state.live_fog.get()]
                    (const SpacePopulateContext& context) -> core::Result<SpacePopulateResult> {
                    if (!population->compose(context.renderer, context.map, *filesystem, context.camera,
                                             context.environment_records, context.first_asset, context.first_entity)) {
                        const std::string failure = population->failure();
                        population->release(context.renderer);
                        return core::Result<SpacePopulateResult>::failure({.code = "EAWR-VIEWER-SPACE-POPULATE",
                                                                            .message = failure});
                    }
                    SpacePopulateResult result;
                    result.instances = population->instances();
                    result.tick = [population](GodotRenderer& renderer, const std::uint32_t sample) {
                        population->pose_units(renderer, sample);
                    };
                    result.release = [population, live_session, effects, debris, emitters, sound, fog](GodotRenderer& renderer) {
                        if (live_session) live_session->finish();
                        if (fog) fog->release();
                        if (effects) effects->release();
                        if (sound) sound->release();
                        if (debris) debris->release();
                        if (emitters) emitters->release();
                        population->release(renderer);
                    };
                    result.write_report = [population, live_session, map_state, effects, debris, emitters,
                                           sound, fog](std::ostream& output) {
                        population->write_report(output);
                        if (live_session) live_session->write_report(output);
                        if (fog) fog->write_report(output);
                        if (effects) effects->write_report(output);
                        if (sound) sound->write_report(output);
                        if (debris) debris->write_report(output);
                        if (emitters) emitters->write_report(output);
                        if (map_state->battle && map_state->space) map_state->battle->write_report(output, *map_state->space);
                    };
                    if (live_session) {
                        // The session starts once its units are composed (#80).
                        std::string live_failure;
                        if (!live_session->start(live_failure)) {
                            population->release(context.renderer);
                            return core::Result<SpacePopulateResult>::failure(
                                {.code = "EAWR-VIEWER-LIVE-SESSION", .message = live_failure});
                        }
                        // The rig's live-quit driver waits for this line before it lets the battle run.
                        godot::UtilityFunctions::print("EAWR live session started");
                        result.live = [population, live_session, effects, debris, emitters, sound, fog,
                                       map_state](GodotRenderer& renderer, const double delta,
                                                  const FixedCamera& camera) {
                            auto update = live_session->frame(*population, renderer, delta);
                            // #494: the fog plane follows this frame's snapshot before it is drawn.
                            if (fog && update && !update.value().error) {
                                fog->frame(*live_session, map_state->space ? map_state->space->live_camera_bounds()
                                                                           : std::nullopt);
                            }
                            // #453, #459: the HUD shows this frame's time panel and outcome.
                            map_state->sync_battle_hud();
                            const auto& battle = live_session->battle_frame();
                            if (!update || update.value().error || !battle.latest) return update;
                            // #394: the units' engine and damage emitters, at the poses just drawn.
                            if (emitters && !emitters->frame(*population, battle.reached,
                                    [live_session](const std::uint64_t tick) { return live_session->snapshot_at(tick); },
                                    live_session->local_player(), *battle.previous, *battle.latest,
                                    // #421: a death clone's pose and clip pose at a sample's tick.
                                    [live_session, population](const std::size_t ship, const double tick)
                                        -> std::optional<UnitEmitters::ClonePose> {
                                        const auto shown = live_session->clone_frame(*population, ship, tick);
                                        const animation::Player* player = population->live_clip(ship);
                                        if (!shown || !shown->death || player == nullptr) return std::nullopt;
                                        const auto transform = population->live_transform(shown->pose);
                                        auto pose = animation::sample_death_frame(*player, *shown->death);
                                        if (!transform || !pose) return std::nullopt;
                                        return UnitEmitters::ClonePose{*transform, std::move(pose.value().bones)};
                                    },
                                    // #456: a model projectile's pose at a sample's tick.
                                    [live_session, population, effects](const std::size_t ship, const double tick)
                                        -> std::optional<sim::math::Mat3x4> {
                                        if (effects == nullptr) return std::nullopt;
                                        const auto pose = effects->projectile_model_pose_at(ship,
                                            [live_session](const std::uint64_t at) { return live_session->snapshot_at(at); }, tick);
                                        return pose ? population->live_transform(*pose) : std::nullopt;
                                    },
                                    camera, battle.presented_tick, live_session->options().reveal,
                                    [live_session](const sim::EntityId entity) { return live_session->unit_opacity(entity); })) {
                                return core::Result<SpaceLiveUpdate>::failure(
                                    {.code = "EAWR-VIEWER-UNIT-EMITTERS", .message = emitters->failure()});
                            }
                            // #429: the clones that left this frame go once the emitters ran the
                            // samples they still stood in.
                            live_session->retire_clones(*population, renderer);
                            if (!effects) return update;
                            // #80: the frame's shots, hits and explosions, from the snapshots only.
                            const bool shown = effects->frame(battle.reached, *battle.previous, *battle.latest,
                                battle.alpha, [live_session](const sim::EntityId entity) {
                                    return live_session->unit_frame(entity);
                                }, camera, battle.presented_tick,
                                [live_session](const std::uint64_t tick) { return live_session->snapshot_at(tick); });
                            if (!shown) {
                                return core::Result<SpaceLiveUpdate>::failure(
                                    {.code = "EAWR-VIEWER-BATTLE-EFFECTS", .message = effects->failure()});
                            }
                            // #84: the frame's sounds and the unit responses to the player's gestures.
                            // The ability clicks are taken every frame so a run without battle audio keeps none.
                            auto ability_clicks = live_session->take_ability_clicks();
                            if (sound) {
                                sound->frame(*live_session, map_state->battle ? map_state->battle->take_acknowledgements()
                                                                              : std::vector<BattleInput::Acknowledgement>{},
                                             std::move(ability_clicks), camera, delta);
                            }
                            // #391: the breakoff props' fires and explosions.
                            if (debris && !debris->effects(camera, battle.presented_tick)) {
                                return core::Result<SpaceLiveUpdate>::failure(
                                    {.code = "EAWR-VIEWER-BREAKOFF-PROPS", .message = debris->failure()});
                            }
                            return update;
                        };
                    }
                    return core::Result<SpacePopulateResult>::success(std::move(result));
                }} : SpacePopulateHook{},
            .effects = attached_effects ? SpaceEffectsHook{
                .tick = [&state](const FixedCamera& camera, const std::uint32_t tick) {
                    if (!state.particles || !state.particles->has_work()) return true;
                    // Sample n is n/30 s. A capture takes one per frame and
                    // holds after its particle frames; the live view takes
                    // those its real-time tick is due, not one per rendered
                    // frame (#186).
                    const std::uint32_t last = state.options.interactive
                        ? tick : std::min(tick, state.options.particle_frames - 1U);
                    const auto view = particles::camera_frame_from_render(camera.eye, camera.target, camera.up);
                    for (std::uint32_t due = particles::map_owner_samples_due(state.particles->frames(), last);
                         due != 0; --due) {
                        if (!state.particles->advance(particles::map_owner_delta(state.particles->frames()), view)) {
                            return false;
                        }
                    }
                    return true;
                },
                .follow_lighting = [&state](const GodotRenderer& renderer) {
                    if (state.particles) state.particles->follow_lighting(renderer);
                },
                .release = [&state]() {
                    if (state.particles) state.particles->release();
                },
                .write_report = [&state](std::ostream& output) {
                    if (!state.particles) return;
                    output << "  \"map_particles\": {\"enabled\": true, \"advanced_frames\": "
                        << state.particles->frames() << ", \"clock\": "
                        << json(state.options.interactive ? "live" : "held") << ", \"attached_records\": "
                        << state.attached_plan.records.size()
                        << ", \"aggregate_capacity\": " << state.attached_plan.aggregate_capacity
                        << ", \"allocated_capacity\": " << state.attached_plan.allocated_capacity
                        << ", \"live_rids_after_release\": "
                        << state.particles->live_rids() << ", \"live_resources_after_release\": "
                        << state.particles->live_resources() << ", \"placements\": [";
                    const auto& placed = state.particles->placements();
                    for (std::size_t index = 0; index < placed.size(); ++index) {
                        output << (index ? ", " : "") << "{\"identity\": " << json(placed[index].identity)
                            << ", \"status\": " << json(placed[index].status)
                            << ", \"effect\": " << json(placed[index].logical_path)
                            << ", \"seed\": " << placed[index].seed
                            << ", \"capacity\": " << placed[index].capacity
                            << ", \"particles\": " << placed[index].stats.particles
                            << ", \"particle_hash\": " << placed[index].stats.hash << "}";
                    }
                    output << "]},\n";
                },
            } : SpaceEffectsHook{},
            .write_hud_report = [&state](std::ostream& output) {
                if (state.hud) output << "  \"hud\": " << state.hud->report_json() << ",\n";
                output << "  \"perf_overlay\": " << state.perf_report_json() << ",\n";
                if (state.live_session) output << "  \"overview_ui\": " << state.overview_report_json() << ",\n";
            },
        });
        if (!state.build_hud(host, map.context_name)) return give_up(state.failure);
        return state.space->ready(host, map, *state.filesystem, catalog, state.catalog.has_value(), catalog_failure);
    }
    if (!state.space_camera.empty() || !state.space_control.empty()) {
        return give_up("--eawr-space-camera and --eawr-space-control apply only to a kind-2 (space) map");
    }
    if (state.space_place_object || !state.space_hardpoint_states.empty())
        return give_up("--eawr-space-place-object applies only to a kind-2 (space) map");
    if (!state.space_place_at.empty()) return give_up("--eawr-space-place-at applies only to a kind-2 (space) map");
    if (!state.live_options.fixture.empty()) return give_up("--eawr-live-session applies only to a kind-2 (space) map");
    if (state.idle_offset && !state.populate) return give_up("--eawr-map-idle-offset requires --eawr-populate");
    if (!state.space_fog_admit.empty()) {
        return give_up("--eawr-space-fog-admit applies only to a kind-2 (space) map");
    }
    if (state.map_camera_terminal_release_test) {
        return give_up("--eawr-map-camera-terminal-release-test applies only to a space map");
    }
    if (state.map_camera_terminal_baseline_test) {
        return give_up("--eawr-map-camera-terminal-baseline-test applies only to a space map");
    }

    auto built = terrain::build(map);
    if (!built) return give_up(core::format_diagnostic(built.error()));
    const terrain::Mesh& mesh = built.value();
    state.chunk_count = static_cast<std::uint32_t>(mesh.chunks.size());
    state.vertex_count = mesh.vertex_count;
    state.triangle_count = mesh.triangle_count;

    state.renderer = std::make_unique<GodotRenderer>(host);
    state.renderer->set_scene_bloom(state.scene_bloom);
    state.scene_bloom_applied = state.renderer->scene_bloom_active();
    if (state.fog) {
        // Land units use every accepted legacy family, so each gets a derived
        // fog stage (#28); the BatchMesh passes keep their fixed variants.
        const auto enabled = state.renderer->enable_fog({
            .selection = {state.fog->stream(), state.fog->team()}, .derive_legacy_stages = true});
        if (!enabled) return give_up(core::format_diagnostic(enabled.error()));
        state.fog_renderer_stream = state.fog->stream();
        host.set_process_input(true);
        host.set_process_unhandled_input(true);
    }

    if (state.environment_choice == "map") {
        if (map.environments.empty()) return give_up("--eawr-environment map: the map declares no environment record");
        const assets::EnvironmentDescriptor& record = map.environments[state.environment_record];
        std::vector<lighting::RawField> fields;
        for (const auto& field : record.fields) fields.push_back({field.id, field.bytes});
        auto candidate = lighting::candidate_environment(fields);
        if (!candidate) {
            return give_up("--eawr-environment map: environment " + std::to_string(state.environment_record)
                + " does not decode under the candidate mapping");
        }
        state.environment = *candidate;
        state.wind = lighting::wind::environment_wind(record);
    }
    if (state.policy != lighting::Policy::off) {
        // Set before any upload so legacy materials compile in the
        // shadow-receiving variant; the shadow distance is refined once the
        // capture camera is known.
        const lighting::Policy other = state.policy == lighting::Policy::sh
            ? lighting::Policy::hemisphere : lighting::Policy::sh;
        state.configured_lighting = state.lighting_state(state.policy, 4096.0F);
        state.other_lighting = state.lighting_state(other, 4096.0F);
        state.renderer->set_lighting(state.configured_lighting);
    }

    // Every declared slot is resolved and decoded for the report; the slots the
    // samples use become the blend's layer array (land_look.hpp).
    for (const terrain::MaterialSlot& slot : mesh.slots) {
        SlotRecord record;
        record.slot = slot.slot;
        record.cells = slot.cells;
        record.effect_program = effect_name(slot.effect);
        record.effect_technique = std::string(terrain::effect_technique(slot.effect));
        record.effect_pass = std::string(terrain::effect_pass(slot.effect));
        if (terrain::declared(slot.primary_texture)) record.declared_primary = *slot.primary_texture;
        if (terrain::declared(slot.secondary_texture)) {
            record.declared_secondary = *slot.secondary_texture;
        }
        if (terrain::declared(slot.primary_texture)) {
            if (const auto resolved = probe_reference(*state.filesystem, "data/art/textures/",
                    *slot.primary_texture, texture_suffixes)) {
                record.resolved_primary = *resolved;
                record.texture_resolved = assets::load_texture(*state.filesystem, *resolved).has_value();
            }
        }
        if (terrain::declared(slot.secondary_texture)) {
            if (const auto resolved = probe_reference(*state.filesystem, "data/art/textures/",
                    *slot.secondary_texture, texture_suffixes)) {
                record.resolved_secondary = *resolved;
            }
        }
        state.slots.push_back(std::move(record));
    }
    const land_look::TextureLookup lookup_texture = [&state](const std::string_view declared)
        -> std::optional<assets::Texture> {
        const auto resolved = probe_reference(*state.filesystem, "data/art/textures/", declared, texture_suffixes);
        if (!resolved) return std::nullopt;
        auto decoded = assets::load_texture(*state.filesystem, *resolved);
        if (!decoded) return std::nullopt;
        // RGBA rows top-left where the decoder kept BGRA or bottom-left rows.
        if (auto normal = space::normalize_texture(decoded.value())) return std::move(normal.value());
        return std::move(decoded.value());
    };
    auto blend = terrain::blend_map(map);
    if (!blend) return give_up(core::format_diagnostic(blend.error()));
    auto blend_bindings = land_look::register_terrain_blend(*state.renderer, blend.value(),
        map.terrain->materials, lookup_texture, state.terrain_blend);
    if (!blend_bindings) return give_up(core::format_diagnostic(blend_bindings.error()));

    std::vector<sim::RenderInstance> instances;
    sim::AssetId next_asset = 1;
    sim::EntityId next_entity = 1;
    const MaterialDescription terrain_material{
        .schema_version = MaterialDescription::current_schema_version,
        .route = MaterialRoute::modern_spatial,
        .pass = RenderPass::opaque,
        .program = state.fog ? fog_terrain_shader(state.policy != lighting::Policy::off)
            : std::string(state.policy == lighting::Policy::off ? terrain_surface_shader : terrain_lit_shader),
        .technique = {},
        .pass_name = {},
        .bindings = std::move(blend_bindings.value()),
    };
    // Every surface draws the same blended material; its own texture is unused.
    const assets::Texture unused_texture = placeholder_texture();
    for (const terrain::Chunk& chunk : mesh.chunks) {
        for (const terrain::Surface& surface : chunk.surfaces) {
            const assets::Model model = terrain::surface_model(mesh, chunk, surface);
            const auto uploaded = state.renderer->upload(
                next_asset, model, unused_texture, terrain_material);
            if (!uploaded) return give_up(core::format_diagnostic(uploaded.error()));
            // Retail shadows are stencil volumes of object meshes; the terrain
            // has none, so it receives shadows but never casts them.
            state.renderer->set_casts_shadows(next_asset, false);
            if (state.fog) {
                const auto declared = state.renderer->declare_fog_consumer(next_asset);
                if (!declared) return give_up("terrain fog consumer " + std::to_string(next_asset)
                    + ": " + core::format_diagnostic(declared.error()));
            }
            instances.push_back({next_entity++, next_asset++, identity_transform()});
            ++state.uploaded_surfaces;
        }
    }
    if (instances.empty()) return give_up("terrain produced no uploaded surface");

    // Approximate water: the header's plane and every authored river.
    if (auto water = land_look::compose_water(*state.renderer, map, mesh, lookup_texture,
            next_asset, next_entity, instances, state.water_capture_time, state.options.interactive,
            state.fog.has_value())) {
        state.water = std::move(water.value());
        state.water_status = state.water.status;
        state.water_cause = state.water.cause;
    } else {
        return give_up("water: " + core::format_diagnostic(water.error()));
    }

    // Skydome. The TED environment field names an XML object; resolving it is
    // catalog work, and every failure along the way is an explicit status
    // rather than a silent skip.
    if (map.environments.empty() || !map.environments[state.environment_record].primary_sky) {
        state.skydome_status = "absent";
    } else {
        state.skydome_object = *map.environments[state.environment_record].primary_sky;
        const assets::ObjectTypeRef* type = assets::find_object_type(catalog, state.skydome_object);
        if (type == nullptr) {
            state.skydome_status = catalog_failure.empty()
                ? "object_not_in_catalog" : "catalog_unavailable";
            if (!catalog_failure.empty()) state.failure = catalog_failure;
        } else {
            const std::optional<std::string>& declared =
                map.kind == assets::MapKind::space && type->space_model_name
                    ? type->space_model_name
                    : (type->land_model_name ? type->land_model_name : type->model_name);
            if (!declared) {
                state.skydome_status = "object_declares_no_model";
            } else if (const auto resolved = probe_reference(*state.filesystem,
                           "data/art/models/", *declared, model_suffixes)) {
                state.skydome_model_path = *resolved;
                auto bytes = state.filesystem->open(*resolved);
                auto model = assets::load_model(*state.filesystem, *resolved);
                if (!bytes || !model) {
                    state.skydome_status = "model_failed_to_load";
                } else {
                    state.skydome_model_hash = hash_bytes(bytes.value());
                    const auto& light = state.environment.lights[0].direction;
                    land_look::SkyInputs sky_inputs;
                    sky_inputs.toward_sun = assets::Vec3f{-light.x, -light.y, -light.z};
                    const auto sky = land_look::compose_sky(*state.renderer, model.value(),
                        lookup_texture, sky_inputs, next_asset, next_entity, instances);
                    if (!sky) {
                        state.skydome_status = "upload_rejected";
                        state.failure = core::format_diagnostic(sky.error());
                    } else {
                        state.skydome_casts_shadows = false;
                        state.skydome_drawn = sky.value().dome_surfaces != 0;
                        state.skydome_status = state.skydome_drawn ? "drawn" : "no_dome_surface";
                    }
                }
            } else {
                state.skydome_status = "model_not_in_vfs";
                state.skydome_model_path = *declared;
            }
        }
    }

    if (state.populate) {
        if (!state.compose_placements(map, instances, next_asset, next_entity)) {
            return give_up(state.failure);
        }
    }

    // A top-down camera makes the terrain footprint a known central rectangle,
    // so the evidence check tests where terrain drew rather than only that
    // something drew.
    const assets::Vec3f minimum = terrain::asset_to_render(mesh.bounds_min);
    const assets::Vec3f maximum = terrain::asset_to_render(mesh.bounds_max);
    const float centre_x = (minimum.x + maximum.x) * 0.5F;
    const float centre_z = (minimum.z + maximum.z) * 0.5F;
    const float extent_x = std::abs(maximum.x - minimum.x);
    const float extent_z = std::abs(maximum.z - minimum.z);
    const float aspect = static_cast<float>(state.camera.width)
        / static_cast<float>(state.camera.height);
    // Fit the larger extent into 70% of the frame so the whole map, and a
    // margin of background, are both inside the capture.
    constexpr float fill = 0.70F;
    const float half_height_world = std::max(
        extent_z * 0.5F / fill, (extent_x * 0.5F / fill) / aspect);
    const float half_fov = state.camera.vertical_fov_degrees * 0.5F * 3.14159265F / 180.0F;
    const float distance = half_height_world / std::tan(half_fov);
    state.camera.target = {centre_x, 0.0F, centre_z};
    state.camera.eye = {centre_x, distance, centre_z};
    // Looking straight down needs an up vector that is not the view direction.
    state.camera.up = {0.0F, 0.0F, -1.0F};
    state.camera.near_plane = std::max(1.0F, distance * 0.01F);
    state.camera.far_plane = distance * 8.0F;
    state.footprint_half_height = (extent_z * 0.5F) / half_height_world;
    state.footprint_half_width = (extent_x * 0.5F) / (half_height_world * aspect);

    // P1-06 #27 default framing: the Land_Mode tactical view at the skirmish
    // start when the effective VFS carries the XML camera constants. The
    // top-down overview above stays for `--eawr-map-view overview`, for a map
    // camera config (which frames itself) and when the constants are absent.
    if (state.requested_view == "overview") {
        state.camera_view_cause = "requested";
    } else if (!state.map_camera_config_path.empty() && state.options.capture_path.empty()) {
        state.camera_view_cause = "a map camera config frames the view";
    } else {
        auto tactical = land_look::tactical_default(*state.filesystem, map, mesh,
            state.camera.width, state.camera.height,
            {state.requested_zoom, state.requested_yaw, state.requested_target});
        if (tactical) {
            state.camera = tactical.value().camera;
            state.tactical = std::move(tactical.value());
            state.camera_view = "tactical";
            state.camera_view_cause = "default";
            // No single terrain footprint: the whole frame is the view.
            state.footprint_half_width = 1.0F / 0.85F;
            state.footprint_half_height = 1.0F / 0.85F;
        } else if (state.requested_view == "tactical") {
            return give_up("--eawr-map-view tactical: " + core::format_diagnostic(tactical.error()));
        } else {
            state.camera_view_cause = "tactical constants unavailable: "
                + core::format_diagnostic(tactical.error());
        }
    }

    if (!state.map_camera_config_path.empty()) {
        std::string camera_failure;
        auto source = state.load_map_camera(camera::Mode::land, camera_failure);
        if (!source) return give_up(camera_failure);
        state.map_camera_config_sha256 = source->config_sha256;
        state.map_camera_bindings_sha256 = source->bindings_sha256;
        state.map_camera_tactical_xml_sha256 = source->tactical_xml_sha256;
        state.map_camera_gameconstants_xml_sha256 = source->gameconstants_xml_sha256;
        state.map_camera_constant_sources = std::move(source->provenance);
        state.map_camera_constant_overrides = std::move(source->overrides);
        const auto viewport = host.get_viewport()->get_visible_rect().size;
        const std::uint32_t width = state.options.capture_path.empty()
            ? static_cast<std::uint32_t>(viewport.x) : state.camera.width;
        const std::uint32_t height = state.options.capture_path.empty()
            ? static_cast<std::uint32_t>(viewport.y) : state.camera.height;
        auto* display = DisplayServer::get_singleton();
        auto bridge = std::make_unique<eawr::viewer::MapCameraBridge>(
            display && display->window_is_focused());
        if (map.terrain) {
            // Location_Follows_Terrain: the same samples terrain::build meshes.
            eawr::viewer::TerrainGround ground{map.terrain->width, map.terrain->height,
                map.terrain->cell_spacing, {}};
            ground.heights.reserve(map.terrain->samples.size());
            for (const auto& sample : map.terrain->samples) {
                ground.heights.push_back(static_cast<float>(
                    static_cast<double>(sample.height_sample) * map.terrain->height_scale));
            }
            bridge->set_ground(std::move(ground));
        }
        const std::optional<camera::TacticalFrame> fixed = state.options.capture_path.empty()
            ? std::nullopt : std::optional<camera::TacticalFrame>(camera_frame(state.camera));
        if (auto active = bridge->activate(std::move(source->config),
                std::move(source->constants), source->bindings_json, width, height, fixed);
            !active) {
            return give_up(core::format_diagnostic(active.error()));
        }
        state.camera = render_camera(bridge->frame());
        state.map_camera_initial_frame = state.camera;
        state.map_camera = std::move(bridge);
        state.map_camera_host = &host;
        if (fixed || !(state.options.interactive || state.map_camera_selftest || state.map_free_selftest)) {
            // Keep the render target at the capture identity while the real
            // host window is resized: by the locked graphical probe, or by a
            // window manager (FancyZones snaps a new window into its zone)
            // under an unlocked probe, which then never sees a resize. Only an
            // interactive run and an unlocked self-test, whose subject is
            // following real resizes, draw at the window size.
            pin_capture_viewport(*host.get_window(), width, height);
        }
        host.set_process_input(true);
        host.set_process_unhandled_input(true);
    } else {
        // Without a map camera bridge every map run reads its fixed camera
        // back, captured or not. Pin its render target before drawing begins.
        pin_capture_viewport(*host.get_window(), state.camera.width, state.camera.height);
    }

    // The renderer reads back the last drawn frame, so the capture camera has
    // to be live before the timed window rather than only at capture time;
    // otherwise the image returned was drawn with the previous camera.
    state.renderer->set_camera(state.camera);

    const bool has_admitted_attached = std::any_of(state.attached_plan.records.begin(),
        state.attached_plan.records.end(), [](const particles::MapEffectRecord& record) {
            return record.status == particles::MapEffectStatus::admitted;
        });
    if (state.populate && state.options.map_effects && state.scene
        && (state.scene->count(scene::Cause::model_particle_system) != 0
            || state.scene->count(scene::Cause::model_not_in_vfs) != 0 || has_admitted_attached)) {
        if ((state.scene->count(scene::Cause::model_particle_system) != 0 || has_admitted_attached)
            && static_cast<std::uint64_t>(state.options.particle_frames)
                >= static_cast<std::uint64_t>(state.options.warmup_frames)
                    + state.options.timed_frames) {
            return give_up("map particle frames must be less than warmup plus timed frames "
                "to leave a rendered frame before capture");
        }
        if (!state.attached_animation_failure.empty()) return give_up(state.attached_animation_failure);
        state.particles = std::make_unique<MapParticleProvider>(host, *state.filesystem,
            state.fog ? state.renderer.get() : nullptr, state.fog ? &*state.fog : nullptr);
        // Ordinary placements get what the attached allocation and its drain
        // headroom leave; the headroom is 0 unless idle owners exist.
        const bool prepared = state.particles->prepare(map, *state.scene,
            state.options.particle_seed,
            state.options.particle_capacity - static_cast<std::uint32_t>(state.attached_plan.allocated_capacity)
                - static_cast<std::uint32_t>(state.attached_drain_headroom));
        const bool attached_prepared = state.particles->prepare_attached(state.attached_plan, *state.scene,
            state.effect_animation_idle() ? &state.attached_owners : nullptr);
        if (!prepared || !attached_prepared) {
            return give_up("one or more map particle placements failed; identities and causes are in map_particles");
        }
    }

    if (state.policy != lighting::Policy::off) {
        // The shadow range covers the farthest terrain corner seen by the
        // capture camera. Cascades split by view depth: the tactical and live
        // cameras keep the land splits, while a top-down overview sees the
        // whole map at nearly one depth and gets one orthogonal map instead.
        const bool live_camera = !state.map_camera_config_path.empty() && state.options.capture_path.empty();
        if (state.camera_view != "tactical" && !live_camera) {
            state.configured_lighting.shadow_layout = GodotRenderer::ShadowLayout::orthogonal;
            state.other_lighting.shadow_layout = GodotRenderer::ShadowLayout::orthogonal;
        }
        float farthest = 0.0F;
        for (const float x : {minimum.x, maximum.x}) {
            for (const float y : {minimum.y, maximum.y}) {
                for (const float z : {minimum.z, maximum.z}) {
                    const float dx = x - state.camera.eye[0];
                    const float dy = y - state.camera.eye[1];
                    const float dz = z - state.camera.eye[2];
                    farthest = std::max(farthest, std::sqrt(dx * dx + dy * dy + dz * dz));
                }
            }
        }
        // Only the overview fits the scene. Tactical views keep a fixed reach
        // across pan/orbit, so Godot's snapped sphere has an invariant radius.
        const float shadow_reach = state.configured_lighting.shadow_layout == GodotRenderer::ShadowLayout::orthogonal
            ? farthest * 1.1F : shadow_settings(active_render_profile(), false).max_distance;
        state.configured_lighting.shadow_max_distance = shadow_reach;
        state.other_lighting.shadow_max_distance = shadow_reach;
        state.renderer->set_lighting(state.configured_lighting);
    }

    if (state.fog && !state.fog_unsupported.empty()) {
        return give_up("fog-enabled populated capture has unsupported required consumer: "
            + state.fog_unsupported.front());
    }
    state.snapshot = state.fog ? state.fog->snapshot(std::move(instances))
        : std::make_shared<const sim::RenderSnapshot>(0, std::move(instances));
    if (state.fog && state.terrain_snapshot) {
        std::vector<sim::RenderInstance> terrain(
            state.terrain_snapshot->instances().begin(), state.terrain_snapshot->instances().end());
        state.terrain_snapshot = state.fog->snapshot(std::move(terrain));
    }
    if (state.fog_inject_input) {
        // Deterministic synthetic input enters through the same map event
        // handler as keyboard events, before the first submitted frame.
        for (const Key code : {KEY_F, KEY_P, KEY_T}) {
            Ref<InputEventKey> event;
            event.instantiate();
            event->set_physical_keycode(code);
            event->set_pressed(true);
            input(event);
        }
    }
    if (!state.build_hud(host, map.context_name)) return give_up(state.failure);
    return true;
}

std::optional<eawr::viewer::MapCameraSource> MapMode::State::load_map_camera(
    const camera::Mode mode, std::string& failure_text) const {
    eawr::viewer::MapCameraSource source;
    const auto config_text = read_camera_file(map_camera_config_path);
    if (!config_text) {
        failure_text = "map camera config could not be read or exceeds 1 MiB";
        return std::nullopt;
    }
    source.config_sha256 = sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(config_text->data()), config_text->size()));
    source.config_file = map_camera_config_path.filename().generic_string();
    // Map identity and bounds are validated here, before any activation.
    auto config = eawr::viewer::parse_map_camera_config(*config_text, options.map_path, map_hash, mode);
    if (!config) {
        failure_text = core::format_diagnostic(config.error());
        return std::nullopt;
    }
    // --eawr-camera-zoom (validated in ready) replaces the initial and reset zoom.
    if (options.camera_zoom) config.value().zoom = *options.camera_zoom;
    const std::filesystem::path bindings_path = map_camera_config_path.parent_path() / config.value().bindings_path;
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
    // Precedence: effective-VFS XML, then the config's project-authored map
    // overrides; a fixed capture later pins the whole frame over both.
    auto resolved = eawr::viewer::resolve_map_constants(config.value(), std::move(loaded.value()),
        map_camera_config_path.filename().generic_string(), source.config_sha256);
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
    case 8:
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
        inject_map_ctrl(true, true);
        break;
    case 44:
        check("Ctrl grabbed motion rotates yaw and tilts", locked ? unchanged()
            : map_camera->controller().yaw_degrees() < map_camera_yaw_mark
                && map_camera->controller().state().pitch_degrees < map_camera_pitch_mark
                && map_camera->controller().orbit_pitch_offset() < 0.0F
                && map_camera->controller().state().zoom == map_camera_zoom_mark
                && orbit_focus_at_centre(map_camera->frame(), map_camera_mark,
                    map_camera_orbit_radius_mark));
        inject_map_button(MOUSE_BUTTON_MIDDLE, centre, false, true);
        inject_map_ctrl(false);
        break;
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

void MapMode::close_requested() {
    if (state_->space && state_->live_session) state_->space->close();
}

std::optional<int> MapMode::process(const double delta) {
    State& state = *state_;
    state.sync_perf_overlay();
    if (state.space) {
        const std::optional<int> finished = state.space->process(delta);
        if (state.perf_trace && state.live_session) {
            PerfTrace::Frame frame;
            frame.frame_ms = delta * 1000.0;
            frame.presented_tick = static_cast<std::uint64_t>(std::max(0.0, state.live_session->presented_tick()));
            frame.units = state.live_session->visible_units().size();
            if (const auto snapshot = state.live_session->snapshot_at(frame.presented_tick)) {
                frame.projectiles = snapshot->projectiles().size();
            }
            if (state.battle_effects) {
                frame.effects = state.battle_effects->live_effects();
                frame.effect_particles = state.battle_effects->particles();
            }
            if (state.unit_emitters) frame.emitter_particles = state.unit_emitters->particles();
            state.perf_trace->frame(frame);
        }
        // #82: selection and orders see the frame the view just drew.
        // #848: the overview level the camera just took decides what the battle UI draws this frame.
        if (!finished) state.sync_overview_ui();
        if (!finished && state.battle && state.live_session && state.space_population) {
            state.battle->frame(*state.live_session, *state.space_population, *state.space);
            if (state.hud) state.hud->set_unit_cards(state.battle->card_layout(), state.battle->card_units());
            if (state.hud) state.hud->set_ability_bar(state.battle->ability_bar());
            state.sync_minimap();
        }
        // #447 --eawr-live-follow: the next frame looks at the unit where this one drew it.
        if (!finished && state.live_session && state.live_session->options().follow) {
            if (const auto unit = state.live_session->unit_frame(*state.live_session->options().follow)) {
                state.space->live_camera_focus(static_cast<float>(unit->position[0]),
                                               static_cast<float>(unit->position[1]));
            }
        }
        return finished;
    }
    if (state.completed || !state.renderer) return std::nullopt;
    if (state.map_camera && !state.failure.empty()) {
        state.completed = true;
        static_cast<void>(state.write_report());
        return 2;
    }
    if (state.map_camera && state.phases.empty() && !state.map_camera_settle_started) {
        if (!std::isfinite(delta) || delta < 0.0
            || delta > static_cast<double>(std::numeric_limits<float>::max())) {
            state.failure = "map camera process delta is invalid";
            state.completed = true;
            static_cast<void>(state.write_report());
            return 2;
        }
        const std::uint32_t tick = state.frame + 1U;
        const std::uint32_t terminal = state.options.warmup_frames + state.options.timed_frames;
        const bool free_terminal_test = state.map_free_terminal_hold_test
            || state.map_free_terminal_release_test;
        if (free_terminal_test && tick == terminal) {
            if (!state.map_camera->free_active()) {
                state.failure = "free terminal test did not enter free flight";
            } else if (!state.map_camera->adapter().is_held(
                viewer::camera_input::Action::free_move_forward)) {
                state.failure = "free terminal forward input did not reach adapter before ordinary step";
            } else {
                state.map_free_terminal_forward_held_at_step = true;
            }
        }
        const float step_seconds = free_terminal_test && tick == terminal
            ? 0.05F : static_cast<float>(delta);
        if (auto stepped = state.map_camera->step(step_seconds); !stepped) {
            state.failure = core::format_diagnostic(stepped.error());
            state.completed = true;
            static_cast<void>(state.write_report());
            return 2;
        }
        state.camera_selftest_tick();
        state.free_selftest_tick();
        if (state.map_camera_terminal_hold_test) {
            if (tick == terminal - 1U) {
                // Drive the bridge directly for the declared final pan step;
                // OS input delivery can otherwise slip past the capture tick.
                if (auto pressed = state.map_camera->handle(viewer::camera_input::RawEvent{
                        .kind = viewer::camera_input::RawKind::key,
                        .code = *viewer::camera_input::key_code("D"),
                        .pressed = true}); !pressed) {
                    state.failure = core::format_diagnostic(pressed.error());
                }
            }
            if (tick == terminal) {
                // Leave the key pressed through the final ordinary interactive frame.
                if (auto moved = state.map_camera->step(0.05F); !moved) {
                    state.failure = core::format_diagnostic(moved.error());
                }
            }
        }
        if (state.map_free_terminal_hold_test || state.map_free_terminal_release_test) {
            if (tick == 1U) inject_map_key(KEY_F, true);
            if (tick == 2U) inject_map_key(KEY_F, false);
            // Godot delivers injected events after this process callback. Press
            // before terminal so its ordinary step consumes the held action.
            if (tick == terminal - 1U) inject_map_key(KEY_W, true);
            if (tick == terminal && state.map_free_terminal_release_test) {
                inject_map_key(KEY_W, false);
            }
        }
        state.camera = render_camera(state.map_camera->frame());
        state.renderer->set_camera(state.camera);
    }
    if (!state.phases.empty()) {
        // Comparison phases: the same camera with one setting changed. The
        // renderer reads back the last drawn frame, so a few frames are drawn
        // before each capture is taken.
        State::Phase& phase = state.phases.front();
        if (!phase.started) {
            phase.started = true;
            state.comparison_frames = 0;
            if (phase.apply) phase.apply(state);
            if (state.particles) state.particles->follow_lighting(*state.renderer);
        }
        state.renderer->submit(phase.terrain_only ? state.terrain_snapshot : state.snapshot);
        if (state.fog && !state.renderer->fog_status().ready()) {
            state.completed = true;
            state.failure = "fog renderer rejected selected source during comparison capture";
            state.release_particles();
            static_cast<void>(state.write_report());
            return 2;
        }
        if (++state.comparison_frames < 4) return std::nullopt;
        auto capture = state.renderer->capture(state.camera);
        if (!capture) {
            state.completed = true;
            state.failure = core::format_diagnostic(capture.error());
            state.release_particles();
            static_cast<void>(state.write_report());
            return 2;
        }
        state.capture_hashes[phase.name] = hash_bytes(capture.value().png_bytes);
        if (!state.options.capture_path.empty()) {
            // Comparison captures sit next to the main capture, named by phase.
            std::filesystem::path path = state.options.capture_path;
            path.replace_extension(std::filesystem::path("." + phase.name + ".png"));
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (output) {
                output.write(reinterpret_cast<const char*>(capture.value().png_bytes.data()),
                    static_cast<std::streamsize>(capture.value().png_bytes.size()));
            }
        }
        state.captures[phase.name] = std::move(capture.value().png_bytes);
        state.phases.pop_front();
        if (!state.phases.empty()) return std::nullopt;
        return state.finish();
    }
    ++state.frame;
    // The frame's presentation tick: its frame count in a capture, real time
    // at 30 Hz in the live view. A capture holds after its particle frames.
    // Map particles and unit idle clips both run on it (#157, #196).
    const std::uint32_t tick = state.options.interactive
        ? live_idle_tick(state.idle_clock_seconds, delta)
        : std::min(state.frame - 1U, state.options.particle_frames - 1U);
    if (state.particles) state.particles->follow_lighting(*state.renderer);
    if (state.particles && state.particles->has_work()) {
        // Sample n is n/30 s. A capture takes one per frame; the live view
        // takes those its real-time tick is due, as the space view does (#186).
        const auto camera_frame = particles::camera_frame_from_render(
            state.camera.eye, state.camera.target, state.camera.up);
        for (std::uint32_t due = particles::map_owner_samples_due(state.particles->frames(), tick);
             due != 0; --due) {
            if (!state.particles->advance(particles::map_owner_delta(state.particles->frames()), camera_frame)) {
                state.completed = true;
                state.release_particles();
                state.failure = "map particle advance failed; identities and causes are in map_particles";
                static_cast<void>(state.write_report());
                return 2;
            }
        }
    }
    state.pose_units(tick);
    // Foliage bends on the scene clock (#147), which follows the same clock.
    state.advance_wind(delta);
    state.renderer->submit(state.snapshot);
    if (state.fog && !state.renderer->fog_status().ready()) {
        state.completed = true;
        const auto fog_status = state.renderer->fog_status();
        state.failure = fog_status.last_rejection
            ? core::format_diagnostic(*fog_status.last_rejection)
            : "fog renderer did not bind the selected source";
        state.release_particles();
        static_cast<void>(state.write_report());
        return 2;
    }
    if (!state.fog_paint_evidence.empty() && (state.frame == 4 || state.frame == 8 || state.frame == 12)) {
        const std::string label = state.frame == 4 ? "source"
            : (state.frame == 8 ? "painted" : "restored");
        auto evidence = state.renderer->capture(state.camera);
        if (!evidence) {
            state.completed = true;
            state.failure = core::format_diagnostic(evidence.error());
            state.release_particles();
            static_cast<void>(state.write_report());
            return 2;
        }
        std::filesystem::path path = state.fog_paint_evidence;
        path.replace_extension(std::filesystem::path("." + label + ".png"));
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(evidence.value().png_bytes.data()),
            static_cast<std::streamsize>(evidence.value().png_bytes.size()));
        if (!output) {
            state.completed = true;
            state.failure = "cannot write fog paint evidence " + path.string();
            state.release_particles();
            static_cast<void>(state.write_report());
            return 2;
        }
        state.fog_paint_hashes[label] = hash_bytes(evidence.value().png_bytes);
        state.fog_paint_uploads[label] = state.renderer->fog_status().cache.uploads;
        state.fog_paint_updates[label] = state.renderer->fog_status().cache.updates;
        if (state.frame == 4) {
            const auto* source = state.fog->source().find(state.fog->team());
            const std::uint32_t x = source->desc().width / 2;
            const std::uint32_t y = source->desc().height / 2;
            const auto current = source->cell(x, y);
            static_cast<void>(state.fog->set_painting(true));
            static_cast<void>(state.fog->paint_cell(x, y, current && *current == 255 ? 0 : 255));
            state.refresh_fog_snapshots();
        } else if (state.frame == 8) {
            static_cast<void>(state.fog->set_painting(false));
            state.refresh_fog_snapshots();
        }
    }
    if (state.frame == state.options.warmup_frames) {
        state.timing_start = std::chrono::steady_clock::now();
        return std::nullopt;
    }
    if (state.frame < state.options.warmup_frames + state.options.timed_frames) {
        return std::nullopt;
    }
    state.timed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - state.timing_start).count();

    if (state.map_camera && !state.map_camera->controller().capture_locked()) {
        const std::uint32_t terminal = state.options.warmup_frames + state.options.timed_frames;
        if (state.map_camera_selftest && state.frame == terminal) {
            auto before = state.renderer->capture(state.camera);
            if (!before) {
                state.failure = core::format_diagnostic(before.error());
                state.completed = true;
                static_cast<void>(state.write_report());
                return 2;
            }
            state.map_camera_terminal_before = std::move(before.value().png_bytes);
        }
        const std::uint32_t last_interactive = terminal + (state.map_camera_selftest ? 3U : 0U);
        if (state.frame < last_interactive) return std::nullopt;
        if (!state.map_camera_settle_started) {
            state.map_camera_settle_started = true;
            state.map_camera_steps_at_freeze = state.map_camera->steps();
            return std::nullopt;
        }
        ++state.map_camera_settle_frames;
        if (state.map_camera_settle_frames < 3U) return std::nullopt;
        if (state.map_camera->steps() != state.map_camera_steps_at_freeze
            || camera_frame(render_camera(state.map_camera->frame())) != camera_frame(state.camera)) {
            state.failure = "map camera changed during terminal settle";
            state.completed = true;
            static_cast<void>(state.write_report());
            return 2;
        }
    }

    auto capture = state.renderer->capture(state.camera);
    if (!capture) {
        state.completed = true;
        state.failure = core::format_diagnostic(capture.error());
        state.release_particles();
        static_cast<void>(state.write_report());
        return 2;
    }
    if (state.map_camera) {
        const Ref<Image> image = decode_png(capture.value().png_bytes);
        if (image.is_null() || image->get_width() != static_cast<int32_t>(state.camera.width)
            || image->get_height() != static_cast<int32_t>(state.camera.height)
            || capture.value().width != state.camera.width
            || capture.value().height != state.camera.height) {
            state.completed = true;
            state.failure = "map camera capture dimensions differ from camera identity";
            static_cast<void>(state.write_report());
            return 2;
        }
        if ((state.map_camera_selftest || state.map_free_selftest)
            && !state.options.capture_path.empty()) {
            state.map_camera_resize_active_at_capture =
                state.map_camera_host->get_window()->get_size() != state.map_camera_window_size;
            if (!state.map_camera_resize_active_at_capture) {
                state.completed = true;
                state.failure = "locked capture restored the host window before readback";
                static_cast<void>(state.write_report());
                return 2;
            }
            state.map_camera_host->get_window()->set_size(state.map_camera_window_size);
        }
    } else if (capture.value().width != state.camera.width
        || capture.value().height != state.camera.height) {
        state.completed = true;
        state.failure = "the map capture viewport " + std::to_string(capture.value().width) + "x"
            + std::to_string(capture.value().height) + " disagrees with the fixed camera viewport "
            + std::to_string(state.camera.width) + "x" + std::to_string(state.camera.height);
        state.release_particles();
        static_cast<void>(state.write_report());
        return 2;
    }
    state.capture_hash = hash_bytes(capture.value().png_bytes);
    state.capture_size = {capture.value().width, capture.value().height};
    state.evidence_verified = state.map_camera && state.options.capture_path.empty()
        ? state.verify_unlocked_capture(capture.value()) : state.verify_capture(capture.value());
    if (!state.options.capture_path.empty()) {
        std::error_code error;
        const std::filesystem::path parent = state.options.capture_path.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, error);
        std::ofstream output(state.options.capture_path, std::ios::binary | std::ios::trunc);
        if (output) {
            output.write(reinterpret_cast<const char*>(capture.value().png_bytes.data()),
                static_cast<std::streamsize>(capture.value().png_bytes.size()));
        }
    }
    if (!state.map_camera_unlocked_capture_path.empty()) {
        std::error_code error;
        const auto parent = state.map_camera_unlocked_capture_path.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, error);
        std::ofstream output(state.map_camera_unlocked_capture_path, std::ios::binary | std::ios::trunc);
        if (output) {
            output.write(reinterpret_cast<const char*>(capture.value().png_bytes.data()),
                static_cast<std::streamsize>(capture.value().png_bytes.size()));
        }
        if (error || !output) {
            state.failure = "cannot write unlocked map camera capture";
            state.completed = true;
            static_cast<void>(state.write_report());
            return 2;
        }
    }
    if (capture.value().png_bytes.empty()) {
        state.completed = true;
        state.failure = "capture is empty";
        state.release_particles();
        static_cast<void>(state.write_report());
        return 2;
    }
    state.capture_hashes["configured"] = state.capture_hash;
    state.captures["configured"] = capture.value().png_bytes;
    if (state.map_camera && state.options.capture_path.empty()) {
        state.release_particles();
        const bool checks = !state.map_camera_selftest ||
            (state.map_camera_checks.size() == map_camera_selftest_check_count && std::all_of(
                state.map_camera_checks.begin(), state.map_camera_checks.end(),
                [](const auto& entry) { return entry.second; }));
        const bool free_checks = !state.map_free_selftest ||
            (!state.map_free_checks.empty() && std::all_of(
                state.map_free_checks.begin(), state.map_free_checks.end(),
                [](const auto& entry) { return entry.second; }));
        state.completed = true;
        const bool particles_clean = !state.particles || (!state.particles->failed()
            && state.particle_rids_after_release == 0
            && state.particle_resources_after_release == 0);
        state.status = state.evidence_verified && checks && free_checks && particles_clean && state.failure.empty()
            ? ((state.map_camera_selftest || state.map_free_selftest)
                ? "map_camera_selftest_passed" : "map_camera_render_passed")
            : "failed";
        if (!checks && state.failure.empty()) state.failure = "map camera graphical selftest failed";
        if (!free_checks && state.failure.empty()) state.failure = "map free camera graphical selftest failed";
        if (!state.write_report()) return 2;
        return state.status == "failed" ? 2 : 0;
    }
    if (state.fog && state.particles) {
        state.particles->capture_fog_evidence(*state.renderer, *state.fog);
        const auto fog_status = state.renderer->fog_status();
        state.particle_fog_consumers_at_capture = fog_status.external_consumers;
        state.particle_fog_bound_at_capture = fog_status.ready();
    }
    if (state.particles) {
        state.particle_rids_at_capture = state.particles->live_rids();
        state.particle_resources_at_capture = state.particles->live_resources();
    }
    if (state.fog && state.populate && state.terrain_snapshot) {
        // A hidden unit cannot be established from the PNG alone. Verify the
        // configured frame actually submitted every snapshot instance and
        // that each populated asset draws with its attached fog material.
        std::map<sim::EntityId, sim::AssetId> expected;
        std::set<sim::EntityId> terrain_entities;
        for (const auto& instance : state.snapshot->instances()) {
            expected.emplace(instance.entity_id, instance.asset_id);
        }
        for (const auto& instance : state.terrain_snapshot->instances()) {
            terrain_entities.insert(instance.entity_id);
        }
        std::map<sim::EntityId, sim::AssetId> observed;
        for (const auto& entry : state.renderer->submission_evidence()) {
            observed.emplace(entry.entity_id, entry.asset_id);
        }
        const auto consumers = state.renderer->fog_consumers();
        bool bound_units = false;
        for (const auto& [entity, asset] : expected) {
            if (terrain_entities.contains(entity)) continue;
            const auto consumer = std::find_if(consumers.begin(), consumers.end(),
                [asset](const auto& value) { return value.asset_id == asset; });
            if (consumer == consumers.end() || !consumer->attached || consumer->surfaces == 0
                || consumer->surfaces_with_fog_material != consumer->surfaces) {
                bound_units = false;
                break;
            }
            bound_units = true;
        }
        state.fog_unit_submissions_verified = bound_units && observed == expected
            && state.renderer->instance_count() == expected.size();
    }

    const bool lit = state.policy != lighting::Policy::off;
    if (state.shadows) {
        state.phases.push_back({"shadows_off", [](State& self) { self.renderer->set_shadows_enabled(false); }, false, false});
    }
    if (lit && !state.fog) {
        state.phases.push_back({"other_policy", [](State& self) {
            GodotRenderer::LightingState other = self.other_lighting;
            other.shadows = false;
            self.renderer->set_lighting(other);
        }, false, false});
    }
    // With idle-clip owners the comparison is always captured, so a final
    // phase in which every owner is empty is verified to change no pixel.
    if (state.particles && (state.particles->has_live() || state.particles->has_owners())) {
        state.phases.push_back({"effects_off", [](State& self) {
            if (self.policy != lighting::Policy::off) self.renderer->set_lighting(self.configured_lighting);
            self.release_particles();
        }, false, false});
    }
    if (state.populate) {
        state.phases.push_back({"terrain_only", [lit](State& self) {
            if (lit) {
                GodotRenderer::LightingState configured = self.configured_lighting;
                configured.shadows = false;
                self.renderer->set_lighting(configured);
            }
        }, true, false});
    }
    if (!state.phases.empty() && state.scene_bloom_applied) {
        state.phases.push_front({"bloom_off", [](State& self) { self.renderer->set_scene_bloom(std::nullopt); },
                                 false, false});
    }
    if (!state.phases.empty()) return std::nullopt;
    return state.finish();
}

void MapMode::input(const Ref<InputEvent>& event) {
    State& state = *state_;
    // #558: the performance overlay's key, in any run; nothing else sees it.
    if (const auto* pressed = Object::cast_to<InputEventKey>(event.ptr());
        pressed != nullptr && pressed->is_pressed() && !pressed->is_echo() && !pressed->is_shift_pressed()
        && !pressed->is_ctrl_pressed() && !pressed->is_alt_pressed() && !pressed->is_meta_pressed()
        && (pressed->get_physical_keycode() != KEY_NONE ? pressed->get_physical_keycode() : pressed->get_keycode())
            == KEY_F3) {
        state.set_perf_overlay(state.perf == nullptr || !state.perf->shown());
        return;
    }
    if (state.hud) state.hud->world_input(event);
    if (state.space) {
        // #82: the live battle's world layer takes selection and order input ahead of the camera.
        if (state.battle && state.live_session && state.space_population
            && state.battle->input(event, *state.live_session, *state.space_population, *state.space)) {
            if (state.hud) state.hud->set_unit_cards(state.battle->card_layout(), state.battle->card_units());
            if (state.hud) state.hud->set_ability_bar(state.battle->ability_bar());
            return;
        }
        // The same Godot event translation as the land camera; the space
        // environment ignores it unless its opt-in camera is active.
        if (event.is_valid()) {
            if (const auto raw = map_camera_event(event)) state.space->camera_event(*raw);
        }
        return;
    }
    if (state.map_camera && event.is_valid()) {
        if (const auto raw = map_camera_event(event)) {
            if (auto handled = state.map_camera->handle(*raw); !handled) {
                state.failure = core::format_diagnostic(handled.error());
            }
        }
    }
    if (!state.fog || event.is_null() || !state.snapshot) return;
    const auto* key = Object::cast_to<InputEventKey>(event.ptr());
    if (!key || !key->is_pressed() || key->is_echo()) return;
    const Key code = key->get_physical_keycode() != KEY_NONE
        ? key->get_physical_keycode() : key->get_keycode();
    if (state.fog->fixed_capture()) {
        if (code == KEY_F || code == KEY_T || code == KEY_P) ++state.fog_ignored_inputs;
        return;
    }
    if (code == KEY_F) {
        static_cast<void>(state.fog->set_painting(!state.fog->painting()));
        state.refresh_fog_snapshots();
    } else if (code == KEY_T) {
        const auto grids = state.fog->source().grids();
        const auto current = std::find_if(grids.begin(), grids.end(), [&](const auto& grid) {
            return grid.team_id() == state.fog->team();
        });
        if (current != grids.end() && !grids.empty()) {
            const auto next = std::next(current) == grids.end() ? grids.begin() : std::next(current);
            if (state.fog->set_team(next->team_id())) {
                state.renderer->set_fog_team(next->team_id());
                state.refresh_fog_snapshots();
            }
        }
    } else if (code == KEY_P && state.fog->painting()) {
        // A deterministic grid-cell brush avoids inventing terrain picking.
        // Repeated presses alternate one selected source cell between dark and
        // fully visible; the source grid remains immutable.
        const auto* source = state.fog->source().find(state.fog->team());
        if (!source) return;
        const std::uint32_t x = source->desc().width / 2;
        const std::uint32_t y = source->desc().height / 2;
        const auto current = state.fog->effective().find(state.fog->team())->cell(x, y);
        const std::uint8_t next = current && *current == 255 ? 0 : 255;
        if (state.fog->paint_cell(x, y, next)) state.refresh_fog_snapshots();
    }
}

void MapMode::focus(const bool focused) {
    if (state_->battle && !focused) state_->battle->cancel();
    if (state_->space) {
        state_->space->camera_focus(focused);
        return;
    }
    if (!state_->map_camera) return;
    ++state_->map_camera_focus_notifications;
    state_->map_camera->set_focus(focused);
}

void MapMode::pointer_left() {
    if (state_->battle) state_->battle->cancel();
    if (state_->space) state_->space->camera_pointer_left();
    if (state_->map_camera) state_->map_camera->pointer_left();
}

void MapMode::viewport_changed(const float width, const float height) {
    if (state_->space) {
        state_->space->camera_viewport(width, height);
        return;
    }
    if (!state_->map_camera) return;
    ++state_->map_camera_resize_notifications;
    if (state_->map_camera_settle_started) return;
    if (auto resized = state_->map_camera->set_viewport(width, height); !resized) {
        state_->failure = core::format_diagnostic(resized.error());
    }
}

} // namespace eawr::presentation::godot_backend
