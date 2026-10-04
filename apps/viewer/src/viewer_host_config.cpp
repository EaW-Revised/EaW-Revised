#include "viewer_host_internal.hpp"
#include "audio_output.hpp"
#include "startup_trace.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include <godot_cpp/classes/rendering_server.hpp>

namespace eawr::presentation::godot_backend {
namespace {

[[nodiscard]] std::optional<Vector2i> parse_window_size(const std::string_view text) {
    const std::size_t separator = text.find('x');
    if (separator == std::string_view::npos) return std::nullopt;
    const auto dimension = [](const std::string_view part) -> std::optional<int32_t> {
        int32_t value{};
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (error != std::errc{} || end != part.data() + part.size() || value < 1 || value > 16384) {
            return std::nullopt;
        }
        return value;
    };
    const auto width = dimension(text.substr(0, separator));
    const auto height = dimension(text.substr(separator + 1));
    if (!width || !height) return std::nullopt;
    return Vector2i(*width, *height);
}
} // namespace

void ViewerHost::read_host_options(const PackedStringArray& arguments) {
    ViewerPath live_replay;
    std::string live_mode;
    for (int64_t index = 0; index < arguments.size(); ++index) {
        const std::string argument = utf8(arguments[index]);
        const auto take_path = [&](std::filesystem::path& target) {
            if (index + 1 >= arguments.size()) return false;
            target = ViewerPath{utf8(arguments[++index])}.native();
            return true;
        };
        const auto take_input = [&](ViewerPath& target) {
            if (index + 1 >= arguments.size()) return false;
            target = ViewerPath{utf8(arguments[++index])};
            return true;
        };
        if (argument == "--eawr-game-root") {
            if (!take_path(options_->game_root)) break;
        } else if (argument == "--eawr-mod-root") {
            if (!take_path(options_->mod_root)) break;
        } else if (argument == "--eawr-profile") {
            if (index + 1 >= arguments.size()) {
                options_->profile_error = "missing value for --eawr-profile (expected eaw, foc or remake)";
                break;
            }
            options_->profile = utf8(arguments[++index]);
        } else if (argument == "--eawr-render-profile") {
            if (index + 1 >= arguments.size()) {
                options_->render_profile_missing = true;
                break;
            }
            options_->render_profile = utf8(arguments[++index]);
        } else if (argument == "--eawr-scene") {
            if (!take_input(options_->scene_path)) break;
        } else if (argument == "--eawr-replay") {
            if (!take_input(options_->replay_path)) break;
        } else if (argument == "--eawr-model") {
            if (index + 1 >= arguments.size()) break;
            options_->model_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-mesh") {
            if (index + 1 >= arguments.size()) break;
            options_->mesh_name = utf8(arguments[++index]);
        } else if (argument == "--eawr-texture") {
            if (index + 1 >= arguments.size()) break;
            options_->texture_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-animation") {
            if (index + 1 >= arguments.size()) break;
            options_->animation_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-animation-time") {
            if (index + 1 >= arguments.size()) break;
            try {
                options_->animation_time_seconds = std::stof(utf8(arguments[++index]));
            } catch (const std::exception&) {
                options_->animation_time_seconds = std::numeric_limits<float>::quiet_NaN();
            }
        } else if (argument == "--eawr-skirmish-setup") {
            options_->skirmish_setup = true;
        } else if (argument == "--eawr-setup-test") {
            options_->skirmish_setup_test = true;
        } else if (argument == "--eawr-content-cache") {
            const auto value = index + 1 < arguments.size() ? utf8(arguments[++index]) : std::string{};
            if (value != "on" && value != "off") options_->profile_error = "--eawr-content-cache expects on or off";
            else options_->cache_content = value == "on";
        } else if (argument == "--eawr-shader-cache") {
            const auto value = index + 1 < arguments.size() ? utf8(arguments[++index]) : std::string{};
            if (value != "on" && value != "off") options_->profile_error = "--eawr-shader-cache expects on or off";
            else options_->cache_shaders = value == "on";
        } else if (argument == "--eawr-load-bench-starts") {
            if (index + 1 < arguments.size()) {
                const auto count = utf8(arguments[++index]);
                std::uint32_t value{};
                const auto parsed = std::from_chars(count.data(), count.data() + count.size(), value);
                if (parsed.ec == std::errc{} && parsed.ptr == count.data() + count.size() && value >= 1 && value <= 8)
                    options_->load_bench_starts = value;
            }
        } else if (argument == "--eawr-map" || argument == "--eawr-skirmish-map") {
            if (index + 1 >= arguments.size()) break;
            options_->map_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-live-session") {
            if (index + 1 >= arguments.size()) break;
            live_mode = utf8(arguments[++index]);
            if (options_->map_path.empty() && (live_mode == "skirmish" || live_mode == "m2")) {
                options_->map_path = "data/art/maps/_mp_space_coruscant.ted";
            }
        } else if (argument == "--eawr-live-replay") {
            if (!take_input(live_replay)) break;
        } else if (argument == "--eawr-atlas") {
            if (index + 1 >= arguments.size()) break;
            options_->atlas_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-icon") {
            if (index + 1 >= arguments.size()) break;
            options_->icon_name = utf8(arguments[++index]);
        } else if (argument == "--eawr-camera-mode") {
            if (index + 1 >= arguments.size()) break;
            options_->camera_mode = utf8(arguments[++index]);
        } else if (argument == "--eawr-camera-zoom") {
            if (index + 1 >= arguments.size()) break;
            options_->camera_zoom_requested = true;
            options_->camera_zoom = std::numeric_limits<float>::quiet_NaN();
            if (auto zoom = tactical_camera::parse_scalar(utf8(arguments[++index]))) {
                options_->camera_zoom = zoom.value();
            }
        } else if (argument == "--eawr-camera-interactive") {
            options_->camera_interactive = true;
        } else if (argument == "--eawr-camera-input-selftest") {
            options_->camera_interactive = true;
            options_->camera_input_selftest = true;
        } else if (argument == "--eawr-camera-bindings") {
            if (index + 1 >= arguments.size()) break;
            options_->camera_bindings_path = utf8(arguments[++index]);
        } else if (argument == "--eawr-capture") {
            if (!take_path(options_->capture_path)) break;
        } else if (argument == "--eawr-report") {
            if (!take_path(options_->report_path)) break;
        } else if (argument == "--eawr-perf-trace") {
            options_->startup_metrics = true;
        } else if (argument == "--eawr-window-resize-test") {
            const std::string value = index + 1 < arguments.size() ? utf8(arguments[++index]) : std::string{};
            options_->window_resize_test = parse_window_size(value);
            options_->window_resize_invalid = !options_->window_resize_test;
            options_->window_resize_pending = options_->window_resize_test.has_value();
        } else if (argument == "--eawr-headless-probe") {
            options_->headless_probe = true;
        } else if (argument == "--eawr-benchmark") {
            options_->benchmark = true;
        } else if (argument == "--eawr-renderer-runtime-test") {
            options_->renderer_runtime_test = true;
        }
    }
    if (live_mode == "replay" && !live_replay.value().empty()) {
        if (!live_replay.is_godot_resource()) {
            std::error_code error;
            const auto size = std::filesystem::file_size(live_replay.native(), error);
            if (!error && size > sim::tactical::replay_max_bytes) {
                options_->profile_error = "--eawr-live-replay exceeds the 256 MiB input limit";
                return;
            }
        }
        const auto bytes = read_bytes(live_replay);
        if (bytes) {
            const auto replay = sim::tactical::parse_replay(std::span(
                reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size()));
            if (!replay) {
                options_->profile_error = "--eawr-live-replay: " + core::format_diagnostic(replay.error());
                return;
            }
            if (replay.value().setup.skirmish) {
                // Select the recording's map before the viewer prepares its scene or fog plane.
                options_->map_path = replay.value().setup.skirmish->map;
            }
        } else {
            options_->profile_error = "--eawr-live-replay: cannot read " + live_replay.value();
        }
    }
}

} // namespace eawr::presentation::godot_backend
