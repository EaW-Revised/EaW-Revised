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

#include "map_mode_ready_internal.hpp"

namespace eawr::presentation::godot_backend {


bool MapMode::State::fail_ready(std::string message) {
    State& state = *this;

        state.failure = std::move(message);
        state.status = "failed";
        state.release_particles();
        static_cast<void>(state.write_report());
        return false;
    }

bool MapMode::ready(Node3D& host) {
    core::load_profile::Scope ready_scope(core::load_profile::Phase::scene);
    State& state = *state_;
    startup_trace.report_path(state.options.report_path);
    std::string lighting_argument = state.options.lighting;
    std::string shadows_argument = state.options.shadows;
    std::string environment_argument = state.options.environment;
    std::string bloom_argument;
    std::string fog_argument_error;
    {
        const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
        for (int64_t index = 0; index < arguments.size(); ++index) {
            const String argument = arguments[index];
            if (argument == "--eawr-skirmish-setup" || argument == "--eawr-setup-test") continue;
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
                else state.map_camera_config_path =
                    ViewerPath{std::string(String(arguments[++index]).utf8().get_data())}.native();
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
                    ViewerPath{std::string(String(arguments[++index]).utf8().get_data())}.native();
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
                state.fog_paths.push_back(ViewerPath{std::string(value.get_data())}.native());
                ++index;
                continue;
            }
            if (argument == String("--eawr-fog-sha256")) {
                state.fog_expected_hashes.emplace_back(value.get_data());
                ++index;
                continue;
            }
            if (argument == String("--eawr-fog-paint-evidence")) {
                state.fog_paint_evidence = ViewerPath{std::string(value.get_data())}.native();
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
    if (state.options.setup) {
        state.live_options.fixture = "skirmish";
        state.live_options.skirmish = *state.options.setup;
        state.live_options.player = 1;
    }

    if (!fog_argument_error.empty()) return state.fail_ready(fog_argument_error);
    if (state.live_options.fixture != "skirmish" && (state.live_options.skirmish.map
        || state.live_options.skirmish.slots || state.live_options.skirmish.seed)) {
        return state.fail_ready("--eawr-skirmish-* options require --eawr-live-session skirmish");
    }
    if (!state.audio_argument.empty() && state.audio_argument != "on" && state.audio_argument != "off") {
        return state.fail_ready("--eawr-audio expects on or off");
    }
    if (!state.audio_argument.empty() && state.live_options.fixture.empty()) {
        return state.fail_ready("--eawr-audio requires --eawr-live-session");
    }
    if (const std::string perf_error =
            parse_perf_overlay_argument(OS::get_singleton()->get_cmdline_user_args(), state.perf_requested);
        !perf_error.empty()) {
        return state.fail_ready(perf_error);
    }
    {
        std::string trace_error;
        const auto trace = PerfTrace::parse(OS::get_singleton()->get_cmdline_user_args(), trace_error);
        if (!trace_error.empty()) return state.fail_ready(trace_error);
        if (trace && state.live_options.fixture.empty()) return state.fail_ready("--eawr-perf-trace requires --eawr-live-session");
        if (trace && !state.perf_trace.emplace().open(*trace, trace_error)) return state.fail_ready(trace_error);
    }
    if (const std::string hud_error = parse_hud_arguments(OS::get_singleton()->get_cmdline_user_args(),
                                                          !state.live_options.fixture.empty(), state.hud_options,
                                                          state.hud_faction_given);
        !hud_error.empty()) {
        return state.fail_ready(hud_error);
    }
    if (state.live_options.fixture.empty()
        && (state.live_options.player || !state.live_options.orders.empty() || !state.live_options.capture_ticks.empty()
            || !state.live_options.follow_groups.empty()
            || state.live_options.end_tick || state.live_options.workers || !state.live_options.hashes_path.empty()
            || !state.live_options.replay_path.empty())) {
        return state.fail_ready("--eawr-live-* options require --eawr-live-session");
    }
    if (state.benchmark) {
        // The recorded frame time is not capped by the display refresh.
        if (auto* display = DisplayServer::get_singleton()) {
            display->window_set_vsync_mode(DisplayServer::VSYNC_DISABLED);
        }
    }
    if ((state.map_camera_selftest || state.map_free_selftest) && state.map_camera_config_path.empty()) {
        return state.fail_ready("map camera selftest requires --eawr-map-camera-config");
    }
    if (state.map_camera_terminal_baseline_test && (state.map_camera_config_path.empty()
        || state.map_camera_selftest || !state.options.capture_path.empty()
        || state.map_camera_terminal_hold_test || state.map_camera_terminal_release_test)) {
        return state.fail_ready("map camera terminal baseline test requires one unlocked camera config and no selftest");
    }
    if (state.map_camera_terminal_hold_test && (state.map_camera_config_path.empty()
        || state.map_camera_selftest || !state.options.capture_path.empty())) {
        return state.fail_ready("map camera terminal hold test requires an unlocked camera without selftest");
    }
    if (state.map_camera_terminal_release_test && (state.map_camera_config_path.empty()
        || state.map_camera_selftest || !state.options.capture_path.empty()
        || state.map_camera_terminal_hold_test || state.map_camera_terminal_baseline_test)) {
        return state.fail_ready("map camera terminal release test requires one unlocked camera config and no selftest");
    }
    if ((state.map_free_terminal_hold_test || state.map_free_terminal_release_test)
        && (state.map_camera_config_path.empty() || !state.options.capture_path.empty()
            || state.map_free_selftest || state.map_camera_selftest
            || (state.map_free_terminal_hold_test && state.map_free_terminal_release_test))) {
        return state.fail_ready("map free terminal test requires one unlocked camera config and no selftest");
    }
    if (!state.map_camera_unlocked_capture_path.empty()
        && (state.map_camera_config_path.empty() || !state.options.capture_path.empty())) {
        return state.fail_ready("unlocked camera capture requires an unlocked map camera");
    }
    if (state.options.camera_zoom && ((state.map_camera_config_path.empty() && state.live_options.fixture.empty())
            || !std::isfinite(*state.options.camera_zoom)
            || *state.options.camera_zoom < 0.0F || *state.options.camera_zoom > 1.0F)) {
        return state.fail_ready("--eawr-camera-zoom with a map needs --eawr-map-camera-config and a zoom in [0, 1]");
    }
    if (state.options.interactive) {
        if ((state.map_camera_config_path.empty() && state.live_options.fixture.empty()) || !state.options.capture_path.empty()
            || !state.map_camera_unlocked_capture_path.empty() || state.map_camera_selftest
            || state.map_free_selftest || state.map_camera_terminal_baseline_test
            || state.map_camera_terminal_hold_test
            || state.map_camera_terminal_release_test || state.map_free_terminal_hold_test
            || state.map_free_terminal_release_test) {
            return state.fail_ready("--eawr-camera-interactive with a map needs --eawr-map-camera-config "
                           "and no capture or camera test");
        }
        // Never reached in practice (about 19 days at 60 frames per second):
        // the owner closes the window. Land and space loops both read it.
        state.options.timed_frames = 100'000'000U;
    }
    // --eawr-map-timed-frames (CI uses fewer on software Vulkan). The bound
    // keeps warmup plus timed frames inside uint32 frame counters.
    if (state.options.timed_frames == 0 || state.options.timed_frames > 100'000'000U) {
        return state.fail_ready("map timed frames must be in [1, 100000000]");
    }
    if (state.options.particle_frames == 0 || state.options.particle_capacity == 0) {
        return state.fail_ready("particle frames and capacity must be positive");
    }
    const bool fog_requested = !state.fog_paths.empty() || !state.fog_expected_hashes.empty()
        || state.fog_team || state.fog_revision || state.fog_tick
        || state.fog_paint_requested || state.fog_inject_input || !state.fog_paint_evidence.empty();
    if (!state.space_fog_admit.empty() && !fog_requested) {
        return state.fail_ready("--eawr-space-fog-admit requires the fog grid arguments");
    }
    if (state.effect_animation_idle() && fog_requested) {
        // Fog evidence binds one resource per emitter; generations and drains
        // of an owner would need per-generation evidence, which is not built.
        return state.fail_ready("--eawr-map-effect-animation idle is not supported with fog");
    }
    if (state.effect_animation_idle() && !state.populate) {
        return state.fail_ready("--eawr-map-effect-animation idle requires --eawr-populate");
    }
    if (state.effect_animation_idle() && !state.options.map_effects) {
        // Nothing would be spawned or verified: refuse instead of reporting
        // owners that never ran.
        return state.fail_ready("--eawr-map-effect-animation idle requires --eawr-map-effects on");
    }
    if (fog_requested) {
        if (state.fog_paths.empty() || !state.fog_team || !state.fog_revision || !state.fog_tick
            || state.fog_expected_hashes.size() != state.fog_paths.size()) {
            return state.fail_ready("fog requires each --eawr-fog-grid with --eawr-fog-sha256, plus --eawr-fog-team, --eawr-fog-revision and --eawr-fog-tick");
        }
        if (state.fog_paint_requested && !state.options.capture_path.empty()) {
            return state.fail_ready("--eawr-fog-paint contradicts fixed --eawr-capture");
        }
        if (!state.fog_paint_evidence.empty()
            && (!state.options.capture_path.empty() || state.fog_paint_requested)) {
            return state.fail_ready("--eawr-fog-paint-evidence manages painting and requires no fixed capture");
        }
        std::string fog_error;
        state.fog = FogMode::load(state.fog_paths, *state.fog_team, *state.fog_tick, fog_error);
        if (!state.fog) return state.fail_ready(fog_error);
        for (std::size_t i = 0; i < state.fog_expected_hashes.size(); ++i) {
            if (state.fog_expected_hashes[i] != state.fog->sources()[i].sha256) {
                return state.fail_ready("fog source SHA-256 mismatch: " + ViewerPath::utf8(state.fog_paths[i]));
            }
        }
        if (state.fog->source().find(*state.fog_team)->revision() != *state.fog_revision) {
            return state.fail_ready("selected fog grid revision does not match --eawr-fog-revision");
        }
        if (!state.options.capture_path.empty()) state.fog->lock_capture();
        else if (state.fog_paint_requested) static_cast<void>(state.fog->set_painting(true));
    }
    if (!lighting_argument.empty()) {
        const auto parsed = lighting::parse_policy(lighting_argument);
        if (!parsed) return state.fail_ready("--eawr-lighting must be sh, hemisphere or off");
        state.policy = *parsed;
    }
    if (!shadows_argument.empty()) {
        if (shadows_argument != "on" && shadows_argument != "off") return state.fail_ready("--eawr-shadows must be on or off");
        state.shadows = shadows_argument == "on";
    }
    if (!bloom_argument.empty()) {
        if (bloom_argument != "on" && bloom_argument != "off") return state.fail_ready("--eawr-bloom must be on or off");
        state.bloom = bloom_argument == "on";
    }
    if (!environment_argument.empty()) {
        if (environment_argument != "default" && environment_argument != "map") {
            return state.fail_ready("--eawr-environment must be default or map");
        }
        state.environment_choice = environment_argument;
    }
    if (state.shadows && state.policy == lighting::Policy::off) {
        return state.fail_ready("--eawr-shadows on needs a lighting policy: shadows are received by the lit adapters");
    }
    // #307: bloom on an unlit debug image (full-bright textures) means nothing,
    // so without a lighting policy the default is no bloom.
    if (state.policy == lighting::Policy::off) {
        if (bloom_argument == "on") return state.fail_ready("--eawr-bloom on needs a lighting policy: an unlit image does not bloom");
        state.bloom_skipped_unlit = state.bloom;
        state.bloom = false;
    }

    assets::ObjectTypeCatalog catalog;
    std::string catalog_failure;
    if (state.options.content) {
        const auto& content = *state.options.content;
        state.filesystem = content.filesystem;
        state.catalog = content.catalog;
        state.profile = content.profile;
        state.layers = content.layers;
        catalog = assets::object_type_catalog(*state.catalog);
    } else {
        // The installed layers select the default; an explicit profile keeps the
        // base-game corpus available for regression fixtures.
        const std::filesystem::path expansion = state.options.game_root / "corruption" / "Data";
        const std::filesystem::path base = state.options.game_root / "GameData" / "Data";
        std::vector<std::pair<std::string, std::filesystem::path>> roots;
        if (!std::filesystem::is_directory(base)) {
            return state.fail_ready("map mode requires an installation with GameData/Data");
        }
        state.profile = state.options.profile.empty() ? (!state.options.mod_root.empty()
            ? "remake" : std::filesystem::is_directory(expansion) ? "foc" : "eaw")
            : state.options.profile;
        if (state.profile != "eaw" && state.profile != "foc" && state.profile != "remake") {
            return state.fail_ready("--eawr-profile must be eaw, foc or remake");
        }
        if (state.profile == "remake") {
            if (state.options.mod_root.empty()) return state.fail_ready("the Remake profile requires --eawr-mod-root");
            if (!std::filesystem::is_directory(expansion)) {
                return state.fail_ready("the Remake profile requires corruption/Data");
            }
            for (const auto& layer : eawr::vfs::mod_chain_roots(state.options.mod_root)) roots.push_back(layer);
            roots.emplace_back("expansion", expansion);
            roots.emplace_back("base", base);
        } else {
            if (state.profile == "foc") {
                if (!std::filesystem::is_directory(expansion)) return state.fail_ready("the FoC profile requires corruption/Data");
                roots.emplace_back("expansion", expansion);
            }
            roots.emplace_back("base", base);
        }
        std::vector<vfs::MountSpec> specs;
        auto chain = vfs::resolve_manifest_chain(roots);
        if (!chain) return state.fail_ready(core::format_diagnostic(chain.error()));
        for (auto& manifest : chain.value()) {
            state.layers.push_back(manifest.mount.layer_id);
            specs.push_back(std::move(manifest.mount));
        }
        auto filesystem = vfs::Vfs::mount(specs);
        if (!filesystem) return state.fail_ready(core::format_diagnostic(filesystem.error()));
        state.filesystem = std::make_shared<const vfs::Vfs>(std::move(filesystem.value()));

        // The catalog is needed because a TED skydome field names an XML object,
        // not an art file. A catalog that cannot load is a recorded diagnostic, not
        // a silently skipped skydome.
        const data::Profile profile = state.profile == "remake" ? data::Profile::remake
            : state.profile == "foc" ? data::Profile::foc : data::Profile::eaw;
        if (auto loaded = data::load_catalog(*state.filesystem, profile)) {
            catalog = assets::object_type_catalog(loaded.value().catalog);
            state.catalog = std::make_shared<const data::Catalog>(std::move(loaded.value().catalog));
        } else {
            catalog_failure = core::format_diagnostic(loaded.error());
        }
    }

    auto map_bytes = state.filesystem->open(state.options.map_path);
    if (!map_bytes) return state.fail_ready(core::format_diagnostic(map_bytes.error()));
    state.map_hash = hash_bytes(map_bytes.value());
    auto loaded_map = assets::load_map(*state.filesystem, state.options.map_path, catalog);
    if (!loaded_map) return state.fail_ready(core::format_diagnostic(loaded_map.error()));
    const assets::Map& map = loaded_map.value();
    state.semantic_complete = map.semantic_complete;
    state.map_kind = !map.kind ? "unknown"
        : (*map.kind == assets::MapKind::land ? "land" : "space");
    state.environment_records = map.environments.size();
    if (state.environment_record_argument) {
        if (*state.environment_record_argument >= map.environments.size()) {
            return state.fail_ready("--eawr-environment-record " + std::to_string(*state.environment_record_argument)
                + ": the map declares " + std::to_string(map.environments.size()) + " environment records");
        }
        // The space view composes its sky, light and backdrop from record 0.
        if (map.kind == assets::MapKind::space && *state.environment_record_argument != 0) {
            return state.fail_ready("--eawr-environment-record applies to land maps; the space view shows environment 0");
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
        return state.ready_space(host, map, catalog, catalog_failure);
    }
    if (!state.space_camera.empty() || !state.space_control.empty()) {
        return state.fail_ready("--eawr-space-camera and --eawr-space-control apply only to a kind-2 (space) map");
    }
    if (state.space_place_object || !state.space_hardpoint_states.empty())
        return state.fail_ready("--eawr-space-place-object applies only to a kind-2 (space) map");
    if (!state.space_place_at.empty()) return state.fail_ready("--eawr-space-place-at applies only to a kind-2 (space) map");
    if (!state.live_options.fixture.empty()) return state.fail_ready("--eawr-live-session applies only to a kind-2 (space) map");
    if (state.idle_offset && !state.populate) return state.fail_ready("--eawr-map-idle-offset requires --eawr-populate");
    if (!state.space_fog_admit.empty()) {
        return state.fail_ready("--eawr-space-fog-admit applies only to a kind-2 (space) map");
    }
    if (state.map_camera_terminal_release_test) {
        return state.fail_ready("--eawr-map-camera-terminal-release-test applies only to a space map");
    }
    if (state.map_camera_terminal_baseline_test) {
        return state.fail_ready("--eawr-map-camera-terminal-baseline-test applies only to a space map");
    }

    auto built = terrain::build(map);
    if (!built) return state.fail_ready(core::format_diagnostic(built.error()));
    const terrain::Mesh& mesh = built.value();
    state.chunk_count = static_cast<std::uint32_t>(mesh.chunks.size());
    state.vertex_count = mesh.vertex_count;
    state.triangle_count = mesh.triangle_count;

    state.renderer = std::make_unique<GodotRenderer>(host, state.options.shaders);
    state.renderer->set_scene_bloom(state.scene_bloom);
    state.scene_bloom_applied = state.renderer->scene_bloom_active();
    if (state.fog) {
        // Land units use every accepted legacy family, so each gets a derived
        // fog stage (#28); the BatchMesh passes keep their fixed variants.
        const auto enabled = state.renderer->enable_fog({
            .selection = {state.fog->stream(), state.fog->team()}, .derive_legacy_stages = true});
        if (!enabled) return state.fail_ready(core::format_diagnostic(enabled.error()));
        state.fog_renderer_stream = state.fog->stream();
        host.set_process_input(true);
        host.set_process_unhandled_input(true);
    }

    if (state.environment_choice == "map") {
        if (map.environments.empty()) return state.fail_ready("--eawr-environment map: the map declares no environment record");
        const assets::EnvironmentDescriptor& record = map.environments[state.environment_record];
        std::vector<lighting::RawField> fields;
        for (const auto& field : record.fields) fields.push_back({field.id, field.bytes});
        auto candidate = lighting::candidate_environment(fields);
        if (!candidate) {
            return state.fail_ready("--eawr-environment map: environment " + std::to_string(state.environment_record)
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

    state.record_terrain_slots(mesh);
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
    if (!blend) return state.fail_ready(core::format_diagnostic(blend.error()));
    auto blend_bindings = land_look::register_terrain_blend(*state.renderer, blend.value(),
        map.terrain->materials, lookup_texture, state.terrain_blend);
    if (!blend_bindings) return state.fail_ready(core::format_diagnostic(blend_bindings.error()));

    std::vector<sim::RenderInstance> instances;
    sim::AssetId next_asset = 1;
    sim::EntityId next_entity = 1;
    const MaterialDescription terrain_material{
        .schema_version = MaterialDescription::current_schema_version,
        .route = MaterialRoute::modern_spatial,
        .pass = RenderPass::opaque,
        .program = state.terrain_material_program(),
        .technique = {},
        .pass_name = {},
        .bindings = std::move(blend_bindings.value()),
    };
    // Every surface draws the same blended material; its own texture is unused.
    const assets::Texture unused_texture = placeholder_texture();
    if (!state.upload_terrain_surfaces(mesh, terrain_material, unused_texture, instances, next_asset, next_entity)) return false;

    // Approximate water: the header's plane and every authored river.
    if (auto water = land_look::compose_water(*state.renderer, map, mesh, lookup_texture,
            next_asset, next_entity, instances, state.water_capture_time, state.options.interactive,
            state.fog.has_value())) {
        state.water = std::move(water.value());
        state.water_status = state.water.status;
        state.water_cause = state.water.cause;
    } else {
        return state.fail_ready("water: " + core::format_diagnostic(water.error()));
    }

    state.compose_skydome(map, catalog, catalog_failure, lookup_texture, next_asset, next_entity, instances);

    if (!state.ready_land_population(map, instances, next_asset, next_entity)) return false;

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
            return state.fail_ready("--eawr-map-view tactical: " + core::format_diagnostic(tactical.error()));
        } else {
            state.camera_view_cause = "tactical constants unavailable: "
                + core::format_diagnostic(tactical.error());
        }
    }

    if (!state.map_camera_config_path.empty()) {
        std::string camera_failure;
        auto source = state.load_map_camera(camera::Mode::land, camera_failure);
        if (!source) return state.fail_ready(camera_failure);
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
            return state.fail_ready(core::format_diagnostic(active.error()));
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
            return state.fail_ready("map particle frames must be less than warmup plus timed frames "
                "to leave a rendered frame before capture");
        }
        if (!state.attached_animation_failure.empty()) return state.fail_ready(state.attached_animation_failure);
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
            return state.fail_ready("one or more map particle placements failed; identities and causes are in map_particles");
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
        return state.fail_ready("fog-enabled populated capture has unsupported required consumer: "
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
    if (!state.build_hud(host, map.context_name)) return state.fail_ready(state.failure);
    return true;
}

} // namespace eawr::presentation::godot_backend
