#include "viewer_profile_arguments.hpp"
#include "eawr/core/json_text.hpp"
#include "attract_runner.hpp"

#include "viewer_path.hpp"
#include "viewer_load_notes.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>

namespace eawr::presentation::godot_backend {
using namespace godot;

namespace {

// Frames the standalone run lets pass before it samples frame times and the battle's first
// positions: the battle's own warm-up (shader and asset upload) is not the menu's steady state.
constexpr std::uint32_t warmup_frames = 30;
constexpr std::uint32_t baseline_frame = 31;

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}

[[nodiscard]] std::string vector3(const std::array<float, 3>& value) {
    std::ostringstream output;
    output.precision(9);
    output << '[' << value[0] << ", " << value[1] << ", " << value[2] << ']';
    return output.str();
}

[[nodiscard]] std::string json(const std::string_view value) {
    return eawr::core::json_quote(value);
}

[[nodiscard]] std::uint64_t now_ms() {
    return Time::get_singleton()->get_ticks_msec();
}

// The installation's mounted layers and effective catalog, as MapMode mounts them for a map run.
[[nodiscard]] std::shared_ptr<const BattleContent> mount_content(const AttractRunner::Options& options,
                                                                  std::string& failure) {
    const std::filesystem::path expansion = options.game_root / "corruption" / "Data";
    const std::filesystem::path base = options.game_root / "GameData" / "Data";
    if (!std::filesystem::is_directory(base)) {
        failure = "the attract requires an installation with GameData/Data";
        return nullptr;
    }
    const std::string profile = options.profile.empty() ? (!options.mod_root.empty()
        ? "remake" : std::filesystem::is_directory(expansion) ? "foc" : "eaw")
        : options.profile;
    if (profile != "eaw" && profile != "foc" && profile != "remake") {
        failure = "--eawr-profile must be eaw, foc or remake";
        return nullptr;
    }
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    if (profile == "remake") {
        if (options.mod_root.empty()) { failure = "the Remake profile requires --eawr-mod-root"; return nullptr; }
        if (!std::filesystem::is_directory(expansion)) {
            failure = "the Remake profile requires corruption/Data";
            return nullptr;
        }
        for (const auto& layer : vfs::mod_chain_roots(options.mod_root)) roots.push_back(layer);
        roots.emplace_back("expansion", expansion);
        roots.emplace_back("base", base);
    } else {
        if (profile == "foc") {
            if (!std::filesystem::is_directory(expansion)) {
                failure = "the FoC profile requires corruption/Data";
                return nullptr;
            }
            roots.emplace_back("expansion", expansion);
        }
        roots.emplace_back("base", base);
    }
    auto chain = vfs::resolve_manifest_chain(roots);
    if (!chain) { failure = core::format_diagnostic(chain.error()); return nullptr; }
    auto content = std::make_shared<BattleContent>();
    std::vector<vfs::MountSpec> specs;
    for (auto& manifest : chain.value()) {
        content->layers.push_back(manifest.mount.layer_id);
        specs.push_back(std::move(manifest.mount));
    }
    auto filesystem = vfs::Vfs::mount(specs);
    if (!filesystem) { failure = core::format_diagnostic(filesystem.error()); return nullptr; }
    content->filesystem = std::make_shared<const vfs::Vfs>(std::move(filesystem.value()));
    content->profile = profile;
    const data::Profile data_profile = profile == "remake" ? data::Profile::remake
        : profile == "foc" ? data::Profile::foc : data::Profile::eaw;
    data::LoadOptions load_options;
    load_options.parse_unregistered_xml = false;
    auto loaded = data::load_catalog(*content->filesystem, data_profile, load_options);
    if (loaded) publish_viewer_load_notes(std::move(loaded.value().notes), content->layers);
    if (!loaded) { failure = core::format_diagnostic(loaded.error()); return nullptr; }
    content->catalog = std::make_shared<const data::Catalog>(std::move(loaded.value().catalog));
    return content;
}

struct Step final {
    std::uint32_t frame{};
    std::string verb;
    std::string argument;
};

[[nodiscard]] std::optional<Step> parse_step(const std::string& text) {
    const auto first = text.find(':');
    if (first == std::string::npos) return std::nullopt;
    Step step;
    const auto parsed = std::from_chars(text.data(), text.data() + first, step.frame);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + first) return std::nullopt;
    const std::string rest = text.substr(first + 1);
    const auto second = rest.find(':');
    step.verb = rest.substr(0, second);
    if (second != std::string::npos) step.argument = rest.substr(second + 1);
    return step;
}

[[nodiscard]] std::optional<skirmish::AttractKey> key_named(const std::string& name) {
    using skirmish::AttractKey;
    if (name == "backspace") return AttractKey::backspace;
    if (name == "enter") return AttractKey::enter;
    if (name == "escape") return AttractKey::escape;
    if (name == "minus") return AttractKey::minus;
    if (name == "other") return AttractKey::other;
    return std::nullopt;
}

[[nodiscard]] skirmish::AttractKey key_of(const Key code) {
    switch (code) {
    case KEY_BACKSPACE: return skirmish::AttractKey::backspace;
    case KEY_ENTER:
    case KEY_KP_ENTER: return skirmish::AttractKey::enter;
    case KEY_ESCAPE: return skirmish::AttractKey::escape;
    case KEY_MINUS:
    case KEY_KP_SUBTRACT: return skirmish::AttractKey::minus;
    default: return skirmish::AttractKey::other;
    }
}

} // namespace

struct AttractRunner::State final {
    explicit State(Options value) : options(std::move(value)) {}

    Options options;
    std::shared_ptr<const BattleContent> content;
    std::optional<skirmish::AttractManager> manager;
    Node* parent{};
    Node3D* host{};
    std::uint64_t host_id{};
    std::unique_ptr<MapMode> map;
    std::string failure;
    // The map the battle shows: the settings' entry and its logical path.
    std::optional<std::size_t> map_index;
    std::string map_path;
    // Standalone run.
    std::uint32_t frame{};
    std::vector<Step> steps;
    bool steps_parsed{};
    bool dialog{};
    bool main_menu{};
    bool started_once{};
    bool completed{};
    double frame_ms_sum{};
    double frame_ms_max{};
    std::uint32_t frame_samples{};
    std::uint64_t starts{};
    std::uint64_t stops{};
    std::uint64_t advances{};
    std::uint64_t keys_consumed{};
    std::uint64_t keys_passed{};
    std::uint64_t world_events{};
    std::uint64_t failed_starts{};
    std::string stop_reason;
    MapMode::AttractStatus last_status;
    std::optional<MapMode::AttractStatus> final_status;
    bool session_freed_at_end{};
    // MAT-18: each audio bus's mute state while the battle runs and after stop(); none is muted.
    std::string buses_during = "{}";
    std::string buses_after = "{}";

    [[nodiscard]] static std::string bus_mutes() {
        AudioServer* server = AudioServer::get_singleton();
        std::string text = "{";
        bool first = true;
        for (const char* name : {"Master", "EAWR_SFX", "EAWR_Voice", "EAWR_Music", "EAWR_Mix"}) {
            const int32_t index = server->get_bus_index(String(name));
            if (index < 0) continue;
            text += std::string(first ? "" : ", ") + "\"" + name + "\": " + (server->is_bus_mute(index) ? "true" : "false");
            first = false;
        }
        return text + "}";
    }

    // MAT-08: frees the battle and its session (the host node last, after the mode's own nodes).
    void free_battle() {
        map.reset();
        if (host != nullptr) {
            if (UtilityFunctions::is_instance_id_valid(static_cast<int64_t>(host_id))) {
                if (Node* owner = host->get_parent()) owner->remove_child(host);
                memdelete(host);
            }
            host = nullptr;
            host_id = 0;
        }
        map_index.reset();
        map_path.clear();
    }

    [[nodiscard]] bool launch(const std::uint64_t now) {
        free_battle();
        if (!manager || parent == nullptr) return false;
        const auto index = manager->advance(now);
        if (!index) return false;
        ++advances;
        const auto& settings = manager->settings();
        const std::string path = skirmish::attract_map_path(settings.maps[*index]);
        if (path.empty()) {
            failure = "Demo_Attract_Maps entry " + std::to_string(*index) + " names no map under data/art/maps";
            ++failed_starts;
            return false;
        }
        host = memnew(Node3D);
        host->set_name("EawrAttractBattle");
        parent->add_child(host);
        host_id = static_cast<std::uint64_t>(host->get_instance_id());
        MapMode::Options map_options;
        map_options.game_root = options.game_root;
        map_options.mod_root = options.mod_root;
        map_options.profile = options.profile;
        map_options.map_path = path;
        map_options.populate = true;
        map_options.lighting = "sh";
        map_options.shadows = "on";
        map_options.environment = "map";
        map_options.interactive = true;
        map_options.content = content;
        map_options.shaders = options.shaders;
        map_options.attract = MapMode::Options::Attract{skirmish::attract_ai_players()};
        map = std::make_unique<MapMode>(std::move(map_options));
        if (!map->ready(*host)) {
            failure = map->failure().empty() ? "the attract battle could not start" : map->failure();
            UtilityFunctions::printerr(String::utf8(("EAWR attract: " + failure).c_str()));
            ++failed_starts;
            free_battle();
            return false;
        }
        failure.clear();
        map_index = *index;
        map_path = path;
        return true;
    }

    [[nodiscard]] skirmish::AttractFrame frame_at(const std::uint64_t now, const bool dialog_active,
                                                   const bool main_menu_active, const bool foreground) const {
        skirmish::AttractFrame result;
        result.now_ms = now;
        result.dialog_active = dialog_active;
        result.main_menu_active = main_menu_active;
        result.foreground = foreground;
        return result;
    }

    void write_report(const bool freed) {
        const std::string text = report_json(freed);
        if (options.report_path.empty()) return;
        std::error_code error;
        if (const auto parent_directory = options.report_path.parent_path(); !parent_directory.empty()) {
            std::filesystem::create_directories(parent_directory, error);
        }
        std::ofstream output(options.report_path, std::ios::binary | std::ios::trunc);
        output << text;
    }

    [[nodiscard]] std::string report_json(const bool freed) const {
        const auto& status = final_status ? *final_status : last_status;
        std::ostringstream out;
        out << "{\n  \"schema\": \"eawr-attract-v1\",\n"
            << "  \"enabled\": " << (manager ? "true" : "false") << ",\n"
            << "  \"forced_off\": " << (options.forced_off ? "true" : "false") << ",\n"
            << "  \"started\": " << (started_once ? "true" : "false") << ",\n"
            << "  \"map_index\": " << (map_index ? std::to_string(*map_index) : std::string("null")) << ",\n"
            << "  \"map\": " << json(map_path) << ",\n"
            << "  \"maps\": " << (manager ? manager->settings().maps.size() : 0U) << ",\n"
            << "  \"frames\": " << frame << ",\n"
            << "  \"frame_samples\": " << frame_samples << ",\n"
            << "  \"frame_ms_mean\": " << (frame_samples ? frame_ms_sum / frame_samples : 0.0) << ",\n"
            << "  \"frame_ms_max\": " << frame_ms_max << ",\n"
            << "  \"starts\": " << starts << ",\n"
            << "  \"stops\": " << stops << ",\n"
            << "  \"advances\": " << advances << ",\n"
            << "  \"failed_starts\": " << failed_starts << ",\n"
            << "  \"keys_consumed\": " << keys_consumed << ",\n"
            << "  \"keys_passed\": " << keys_passed << ",\n"
            << "  \"world_events\": " << world_events << ",\n"
            << "  \"stop_reason\": " << json(stop_reason) << ",\n"
            << "  \"battle\": {\"live\": " << (status.live ? "true" : "false")
            << ", \"paused\": " << (status.paused ? "true" : "false")
            << ", \"hud\": " << (status.hud ? "true" : "false")
            << ", \"battle_input\": " << (status.battle_input ? "true" : "false")
            << ", \"latest_tick\": " << status.latest_tick
            << ", \"units\": " << status.units
            << ", \"moved\": " << status.moved
            << ", \"damaged\": " << status.damaged
            << ", \"lost\": " << status.lost
            << ", \"alive\": " << status.alive
            << ", \"joined\": " << status.joined << "},\n"
            << "  \"camera\": {\"updates\": " << status.camera_updates
            << ", \"first_eye\": " << vector3(status.camera_first_eye)
            << ", \"eye\": " << vector3(status.camera_eye) << "},\n"
            << "  \"speech\": {\"refused\": " << status.speech_refused
            << ", \"queued\": " << status.speech_queued
            << ", \"started\": " << status.speech_started << "},\n"
            << "  \"sfx_3d\": {\"refused\": " << status.sfx_3d_refused
            << ", \"started\": " << status.sfx_3d_started << "},\n"
            << "  \"audio_events\": {\"refused\": " << status.audio_events_refused
            << ", \"admitted\": " << status.audio_events_admitted
            << ", \"started\": " << status.audio_events_started
            << ", \"deaths_2d_refused\": " << status.audio_deaths_2d_refused << "},\n"
            << "  \"buses_muted\": {\"during\": " << buses_during << ", \"after\": " << buses_after << "},\n"
            << "  \"session_freed\": " << (freed ? "true" : "false") << ",\n"
            << "  \"failure\": " << json(failure) << "\n}\n";
        return out.str();
    }
};

bool AttractRunner::requested() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (utf8(arguments[index]) == "--eawr-attract") return true;
    }
    return false;
}

AttractRunner::Options AttractRunner::from_command_line() {
    Options options;
    const PackedStringArray arguments = viewer_profile_arguments();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        const std::string argument = utf8(arguments[index]);
        const bool has_value = index + 1 < arguments.size();
        const std::string value = has_value ? utf8(arguments[index + 1]) : std::string{};
        bool consumed = has_value;
        if (argument == "--eawr-attract") {
            // An optional on|off follows.
            if (has_value && (value == "on" || value == "off")) options.forced_off = value == "off";
            else consumed = false;
        } else if (!has_value) {
            consumed = false;
        } else if (argument == "--eawr-game-root") options.game_root = ViewerPath{value}.native();
        else if (argument == "--eawr-mod-root") options.mod_root = ViewerPath{value}.native();
        else if (argument == "--eawr-profile") options.profile = value;
        else if (argument == "--eawr-report") options.report_path = ViewerPath{value}.native();
        else if (argument == "--eawr-capture") options.capture_path = ViewerPath{value}.native();
        else if (argument == "--eawr-attract-frames") {
            std::uint32_t frames{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), frames);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) options.frames = frames;
        } else if (argument == "--eawr-attract-do") options.steps.push_back(value);
        else consumed = false;
        if (consumed) ++index;
    }
    return options;
}

AttractRunner::AttractRunner(Options options) : state_(std::make_unique<State>(std::move(options))) {}

AttractRunner::~AttractRunner() {
    stop();
}

bool AttractRunner::start(Node& parent) {
    State& state = *state_;
    // MAT-01: an attract that already runs is not restarted.
    if (state.manager && state.manager->active() && state.map) return true;
    state.failure.clear();
    if (!state.content) {
        state.content = state.options.content;
        if (!state.content) state.content = mount_content(state.options, state.failure);
        if (!state.content) return false;
    }
    if (!state.options.shaders) state.options.shaders = std::make_shared<GodotShaderCache>();
    auto settings = skirmish::read_attract_settings(*state.content->filesystem);
    if (!settings) {
        state.failure = core::format_diagnostic(settings.error());
        return false;
    }
    if (state.options.forced_off) settings.value().menu_attract = false;
    // Attract off, or no map to show: the plain backdrop (no manager, nothing running).
    if (!settings.value().menu_attract || settings.value().maps.empty()) {
        state.manager.reset();
        return false;
    }
    state.manager.emplace(std::move(settings).value());
    state.parent = &parent;
    const std::uint64_t now = now_ms();
    state.manager->init(now); // MAT-02
    ++state.starts;
    state.started_once = true;
    return state.launch(now);
}

void AttractRunner::stop() {
    State& state = *state_;
    if (state.manager) {
        state.manager->shutdown();
        state.manager.reset();
        ++state.stops;
    }
    state.free_battle();
    state.parent = nullptr;
}

bool AttractRunner::running() const noexcept {
    return state_->map != nullptr;
}

const std::string& AttractRunner::failure() const noexcept {
    return state_->failure;
}

void AttractRunner::process(const double delta, const bool dialog_active, const bool main_menu_active,
                            const bool foreground) {
    State& state = *state_;
    if (!state.manager || !state.manager->initialized()) return;
    const std::uint64_t now = now_ms();
    const auto service = state.manager->service(state.frame_at(now, dialog_active, main_menu_active, foreground));
    if (state.map) {
        // MAT-05: after the start timeout without input the battle holds still.
        state.map->set_attract_paused(service.paused);
        if (const std::optional<int> code = state.map->process(delta)) {
            // The battle ended on its own (a failure of the session); the next advance restarts it.
            if (*code != 0) {
                state.failure = state.map->failure().empty()
                    ? "the attract battle ended with exit code " + std::to_string(*code) : state.map->failure();
            }
            state.free_battle();
        }
    }
    if (service.action == skirmish::AttractAction::advance) static_cast<void>(state.launch(now));
}

bool AttractRunner::key(const skirmish::AttractKey pressed, const bool dialog_active) {
    State& state = *state_;
    if (!state.manager) return false;
    const auto result = state.manager->key(pressed, state.frame_at(now_ms(), dialog_active, false, true));
    if (result.action == skirmish::AttractAction::advance) static_cast<void>(state.launch(now_ms()));
    if (result.action == skirmish::AttractAction::stop) stop(); // MAT-07: Escape ends the attract
    if (result.consumed) ++state.keys_consumed;
    else ++state.keys_passed;
    return result.consumed;
}

void AttractRunner::pointer_moved(const bool main_menu_active) {
    State& state = *state_;
    if (state.manager) state.manager->pointer_moved(now_ms(), main_menu_active);
}

void AttractRunner::set_front_end(const bool dialog_active, const bool main_menu_active) noexcept {
    state_->dialog = dialog_active;
    state_->main_menu = main_menu_active;
}

void AttractRunner::window_activated() {
    State& state = *state_;
    if (state.manager) state.manager->window_activated(now_ms());
}

void AttractRunner::input(const Ref<InputEvent>& event) {
    State& state = *state_;
    if (event.is_null()) return;
    if (const auto* pressed = Object::cast_to<InputEventKey>(event.ptr());
        pressed != nullptr && pressed->is_pressed() && !pressed->is_echo()) {
        const Key code = pressed->get_physical_keycode() != KEY_NONE ? pressed->get_physical_keycode()
                                                                     : pressed->get_keycode();
        if (key(key_of(code), state.dialog)) return;
    } else if (Object::cast_to<InputEventMouseMotion>(event.ptr()) != nullptr) {
        pointer_moved(state.main_menu);
    }
    // MAT-10, WGUI-76: the world rejects what reaches it.
    ++state.world_events;
    if (state.map) state.map->input(event);
}

MapMode::AttractStatus AttractRunner::status() {
    State& state = *state_;
    if (!state.map) return {};
    state.last_status = state.map->attract_status();
    return state.last_status;
}

std::optional<int> AttractRunner::standalone_frame(const double delta, Node& parent) {
    State& state = *state_;
    if (state.completed) return 0;
    if (!state.steps_parsed) {
        state.steps_parsed = true;
        for (const std::string& text : state.options.steps) {
            if (const auto step = parse_step(text)) state.steps.push_back(*step);
            else {
                state.failure = "--eawr-attract-do expects <frame>:<step>, not " + text;
                state.write_report(true);
                return 2;
            }
        }
    }
    if (state.frame == 0) {
        if (!start(parent) && !state.failure.empty()) {
            state.write_report(true);
            return 2;
        }
    }
    for (const Step& step : state.steps) {
        if (step.frame != state.frame) continue;
        if (step.verb == "key") {
            const auto named = key_named(step.argument);
            if (!named) {
                state.failure = "--eawr-attract-do key: expects backspace, enter, escape, minus or other";
                state.write_report(true);
                return 2;
            }
            static_cast<void>(key(*named, state.dialog));
        } else if (step.verb == "click") {
            Ref<InputEventMouseButton> click;
            click.instantiate();
            click->set_button_index(MOUSE_BUTTON_LEFT);
            click->set_pressed(true);
            click->set_position(Vector2(640.0F, 360.0F));
            input(click);
        } else if (step.verb == "stop") {
            stop();
            state.stop_reason = "scripted stop at frame " + std::to_string(state.frame);
        } else if (step.verb == "start") {
            static_cast<void>(start(parent));
        } else if (step.verb == "dialog") {
            state.dialog = step.argument == "on";
            state.main_menu = state.dialog;
        } else {
            state.failure = "--eawr-attract-do: unknown step " + step.verb;
            state.write_report(true);
            return 2;
        }
    }
    process(delta, state.dialog, state.main_menu, true);
    if (state.frame >= warmup_frames) {
        const double milliseconds = delta * 1000.0;
        state.frame_ms_sum += milliseconds;
        state.frame_ms_max = std::max(state.frame_ms_max, milliseconds);
        ++state.frame_samples;
    }
    if (state.frame == baseline_frame || (state.frame > baseline_frame && state.frame % 30U == 0U)) {
        static_cast<void>(status());
    }
    ++state.frame;
    if (state.options.frames != 0 && state.frame >= state.options.frames) {
        // The last frame: the battle's state and a picture, then the teardown MAT-13 asks for.
        state.final_status = status();
        state.buses_during = State::bus_mutes();
        if (!state.options.capture_path.empty() && parent.get_viewport() != nullptr) {
            const Ref<Image> image = parent.get_viewport()->get_texture()->get_image();
            if (image.is_valid()) {
                std::error_code error;
                if (const auto directory = state.options.capture_path.parent_path(); !directory.empty()) {
                    std::filesystem::create_directories(directory, error);
                }
                image->save_png(String::utf8(ViewerPath::utf8(state.options.capture_path).c_str()));
            }
        }
        stop();
        state.buses_after = State::bus_mutes();
        if (state.stop_reason.empty()) state.stop_reason = "frame budget reached";
        // Frames after the teardown must be harmless.
        process(delta, false, false, true);
        const bool freed = !running() && state.host == nullptr;
        state.session_freed_at_end = freed;
        state.completed = true;
        state.write_report(freed);
        return freed && state.failure.empty() ? 0 : 2;
    }
    return std::nullopt;
}

} // namespace eawr::presentation::godot_backend
