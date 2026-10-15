#include "options_mode.hpp"

#include "audio_buses.hpp"
#include "capture_viewport.hpp"
#include "options_screen.hpp"
#include "settings_store.hpp"
#include "viewer_path.hpp"

#include "eawr/core/sha256.hpp"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <array>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace model = presentation::ui;

namespace {

constexpr int warmup_frames = 8;
constexpr int settle_frames = 6;

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}

[[nodiscard]] std::string json(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) output << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
            else output << static_cast<char>(character);
        }
    }
    output << '"';
    return output.str();
}

[[nodiscard]] bool write_bytes(const std::filesystem::path& path, const PackedByteArray& bytes) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.ptr()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] std::string controls_json(const std::vector<EawrOptionsScreen::ControlState>& controls) {
    std::ostringstream output;
    output << "[";
    for (std::size_t index = 0; index < controls.size(); ++index) {
        const EawrOptionsScreen::ControlState& control = controls[index];
        output << (index ? ",\n      " : "\n      ") << "{\"id\": " << json(control.id) << ", \"kind\": " << json(control.kind)
               << ", \"enabled\": " << (control.enabled ? "true" : "false")
               << ", \"visible\": " << (control.visible ? "true" : "false") << ", \"value\": " << control.value
               << ", \"text\": " << json(control.text) << ", \"reason\": " << json(control.reason) << "}";
    }
    output << (controls.empty() ? "]" : "\n    ]");
    return output.str();
}

[[nodiscard]] std::string lines_json(const std::string& text) {
    std::ostringstream output;
    output << "[";
    std::istringstream input(text);
    std::string line;
    bool first = true;
    while (std::getline(input, line)) {
        output << (first ? "" : ", ") << json(line);
        first = false;
    }
    output << "]";
    return output.str();
}

// What a battle started now would take from the settings (#2927): the bus gains and speed step
// of the active record, as the audio buses and the live session read them. Null with none.
[[nodiscard]] std::string session_json() {
    const model::PlayerSettings* active = ActiveSettings::get();
    if (active == nullptr) return "null";
    const model::SessionValues values = model::session_values(*active);
    std::ostringstream output;
    output << "{\"master\": " << values.master_gain << ", \"sfx\": " << values.sfx_gain
           << ", \"speech\": " << values.speech_gain << ", \"music\": " << values.music_gain
           << ", \"speed_step\": " << values.speed_step << "}";
    return output.str();
}

[[nodiscard]] std::string strings_json(const std::vector<std::string>& values) {
    std::ostringstream output;
    output << "[";
    for (std::size_t index = 0; index < values.size(); ++index) output << (index ? ", " : "") << json(values[index]);
    output << "]";
    return output.str();
}

} // namespace

struct OptionsMode::State final {
    explicit State(Options value) : options(std::move(value)) {}

    Options options;
    Node3D* host{};
    EawrOptionsScreen* screen{};
    std::uint32_t width{};
    std::uint32_t height{};
    int frames{};
    std::size_t next_action{};
    int settle{-1};
    bool scripted{};
    bool was_shown{};
    bool completed{};
    std::string status{"failed"};
    std::string failure;
    std::vector<std::string> performed;
    std::vector<std::string> snapshots;
    std::optional<std::array<std::int32_t, 2>> png;
    std::string capture_sha256;

    [[nodiscard]] bool run_action(const std::string& action);
    [[nodiscard]] bool capture();
    [[nodiscard]] bool write_report() const;
};

bool OptionsMode::requested() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (utf8(arguments[index]) == "--eawr-options") return true;
    }
    return false;
}

OptionsMode::Options OptionsMode::from_command_line() {
    Options options;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
        const std::string argument = utf8(arguments[index]);
        const std::string value = utf8(arguments[index + 1]);
        bool consumed = true;
        if (argument == "--eawr-game-root") options.game_root = ViewerPath{value}.native();
        else if (argument == "--eawr-mod-root") options.mod_root = ViewerPath{value}.native();
        else if (argument == "--eawr-font-cache") options.font_cache = ViewerPath{value}.native();
        else if (argument == "--eawr-report") options.report_path = ViewerPath{value}.native();
        else if (argument == "--eawr-capture") options.capture_path = ViewerPath{value}.native();
        else if (argument == "--eawr-settings-file") options.settings_file = ViewerPath{value}.native();
        else if (argument == "--eawr-options-page") options.page = value;
        else if (argument == "--eawr-options-context") options.context = value;
        else if (argument == "--eawr-options-do") options.actions.push_back(value);
        else if (argument == "--eawr-ui-rules") options.rules = value;
        else consumed = false;
        if (consumed) ++index;
    }
    return options;
}

OptionsMode::OptionsMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
OptionsMode::~OptionsMode() = default;
OptionsMode::OptionsMode(OptionsMode&&) noexcept = default;
OptionsMode& OptionsMode::operator=(OptionsMode&&) noexcept = default;

bool OptionsMode::State::run_action(const std::string& action) {
    const auto colon = action.find(':');
    const std::string verb = action.substr(0, colon);
    const std::string argument = colon == std::string::npos ? std::string{} : action.substr(colon + 1);
    if (verb == "snapshot") {
        std::ostringstream output;
        output << "{\"page\": " << json(EawrOptionsScreen::page_name(screen->page())) << ", \"controls\": "
               << controls_json(screen->control_states()) << "}";
        snapshots.push_back(output.str());
        return true;
    }
    if (verb == "page") {
        EawrOptionsScreen::Page target{};
        if (!EawrOptionsScreen::parse_page(argument, target)) return false;
        screen->open_page(target);
        return true;
    }
    if (verb == "press") return screen->press(argument);
    if (verb == "text") {
        const auto equals = argument.find('=');
        return equals != std::string::npos && screen->set_text_control(argument.substr(0, equals), argument.substr(equals + 1));
    }
    if (verb == "set") {
        const auto equals = argument.find('=');
        if (equals == std::string::npos) return false;
        double value{};
        const std::string number = argument.substr(equals + 1);
        const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (error != std::errc{} || end != number.data() + number.size()) return false;
        return screen->set_control(argument.substr(0, equals), value);
    }
    return false;
}

bool OptionsMode::State::capture() {
    Ref<Image> image = host->get_viewport()->get_texture()->get_image();
    if (image.is_null() || image->is_empty()) {
        failure = "the viewport could not be read back";
        return false;
    }
    image->convert(Image::FORMAT_RGBA8);
    const PackedByteArray png_bytes = image->save_png_to_buffer();
    capture_sha256 = core::sha256_hex(
        std::span<const std::uint8_t>(png_bytes.ptr(), static_cast<std::size_t>(png_bytes.size())));
    if (!options.capture_path.empty()) {
        if (!write_bytes(options.capture_path, png_bytes)) {
            failure = "the capture PNG could not be written";
            return false;
        }
        png = std::array<std::int32_t, 2>{image->get_width(), image->get_height()};
    }
    return true;
}

namespace {

// #3165: each shared audio bus's volume in dB (null when the bus does not exist), the receipt that
// a slider reached the engine.
std::string buses_json() {
    godot::AudioServer* server = godot::AudioServer::get_singleton();
    std::ostringstream output;
    output << "{";
    for (std::size_t which = 0; which < audio_buses::count; ++which) {
        const std::int32_t index = server != nullptr ? server->get_bus_index(audio_buses::names[which]) : -1;
        output << (which != 0 ? ", " : "") << "\"" << audio_buses::names[which] << "\": ";
        if (index < 0) output << "null";
        else output << server->get_bus_volume_db(index);
    }
    output << "}";
    return output.str();
}

} // namespace

bool OptionsMode::State::write_report() const {
    if (options.report_path.empty()) return false;
    RenderingServer* rendering = RenderingServer::get_singleton();
    const DisplayFacts display = display_facts();
    const DisplayServer* display_server = DisplayServer::get_singleton();
    const int vsync_mode = display_server != nullptr ? display_server->window_get_vsync_mode() : -1;
    std::ostringstream output;
    output << "{\n  \"mode\": \"options\",\n  \"status\": " << json(status) << ",\n  \"failure\": " << json(failure)
           << ",\n  \"context\": " << json(options.context) << ",\n  \"rules\": " << json(options.rules)
           << ",\n  \"backend\": {\"rendering_method\": " << json(utf8(rendering->get_current_rendering_method()))
           << ", \"adapter_name\": " << json(utf8(rendering->get_video_adapter_name()))
           << "},\n  \"viewport\": {\"width\": " << width << ", \"height\": " << height << "}"
           << ",\n  \"display\": {\"window\": [" << display.window.width << ", " << display.window.height
           << "], \"screen\": [" << display.screen.width << ", " << display.screen.height
           << "], \"windowed\": " << (display.windowed ? "true" : "false")
           << ", \"vsync\": " << (display.vsync ? "true" : "false")
           << ", \"vsync_mode\": " << vsync_mode << "}"
           << ",\n  \"actions\": " << strings_json(performed);
    if (screen != nullptr) {
        output << ",\n  \"settings_file\": {\"path\": " << json(screen->settings_file())
               << ", \"existed_at_start\": " << (screen->settings_file_existed() ? "true" : "false")
               << ", \"notes\": " << strings_json(screen->settings_notes()) << "}"
               << ",\n  \"page\": " << json(EawrOptionsScreen::page_name(screen->page()))
               << ",\n  \"committed\": " << lines_json([&] { auto safe = screen->committed(); safe.network.online_host.clear();
                    safe.network.online_pin.clear(); safe.network.online_ca_file.clear(); return model::serialize_settings(safe); }())
               << ",\n  \"applied\": " << strings_json(screen->last_apply().applied)
               << ",\n  \"skipped\": " << strings_json(screen->last_apply().skipped)
               << ",\n  \"notes\": " << strings_json(screen->notes())
               << ",\n  \"session\": " << session_json()
               << ",\n  \"buses\": " << buses_json()
               << ",\n  \"shown\": " << (screen->shown() ? "true" : "false")
               << ",\n  \"controls\": " << controls_json(screen->control_states());
    }
    output << ",\n  \"snapshots\": [";
    for (std::size_t index = 0; index < snapshots.size(); ++index) output << (index ? ",\n    " : "\n    ") << snapshots[index];
    output << (snapshots.empty() ? "]" : "\n  ]");
    if (png) {
        output << ",\n  \"capture\": {\"width\": " << (*png)[0] << ", \"height\": " << (*png)[1]
               << ", \"sha256\": " << json(capture_sha256) << "}";
    }
    output << "\n}\n";
    const std::string text = output.str();
    std::error_code error;
    if (!options.report_path.parent_path().empty()) std::filesystem::create_directories(options.report_path.parent_path(), error);
    std::ofstream file(options.report_path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << text;
    return static_cast<bool>(file);
}

bool OptionsMode::ready(Node3D& host) {
    State& state = *state_;
    state.host = &host;
    const auto fail = [&]() {
        state.completed = true;
        static_cast<void>(state.write_report());
        return false;
    };
    if (state.options.rules != "aspect" && state.options.rules != "retail") {
        state.failure = "--eawr-ui-rules must be aspect or retail";
        return fail();
    }
    if (state.options.context != "main" && state.options.context != "game") {
        state.failure = "--eawr-options-context must be main or game";
        return fail();
    }
    EawrOptionsScreen::Page start{};
    if (!EawrOptionsScreen::parse_page(state.options.page, start)) {
        state.failure = "--eawr-options-page must be hub, audio, video, advanced, gameplay, keyboard or network";
        return fail();
    }
    const Vector2 size = host.get_viewport()->get_visible_rect().size;
    if (size.x < 1.0F || size.y < 1.0F) {
        state.failure = "the viewport has no size";
        return fail();
    }
    state.width = static_cast<std::uint32_t>(size.x);
    state.height = static_cast<std::uint32_t>(size.y);
    const bool capturing = !state.options.capture_path.empty();
    if (capturing) pin_capture_viewport(*host.get_window(), state.width, state.height);
    state.scripted = capturing || !state.options.report_path.empty() || !state.options.actions.empty();

    state.screen = memnew(EawrOptionsScreen);
    EawrOptionsScreen::Options screen_options;
    screen_options.game_root = state.options.game_root;
    screen_options.mod_root = state.options.mod_root;
    screen_options.font_cache = state.options.font_cache;
    screen_options.settings_file = state.options.settings_file;
    screen_options.rules = state.options.rules == "retail" ? model::LayoutRules::retail : model::LayoutRules::aspect_correct;
    screen_options.context = state.options.context == "game" ? EawrOptionsScreen::Context::in_game
                                                            : EawrOptionsScreen::Context::main_menu;
    // A pinned capture window is not resized by a saved video setting.
    screen_options.apply_window = !capturing;
    if (!state.screen->build(std::move(screen_options))) {
        state.failure = state.screen->failure();
        memdelete(state.screen);
        state.screen = nullptr;
        return fail();
    }
    state.screen->show(host, start);
    state.was_shown = state.screen->shown();
    return true;
}

std::optional<int> OptionsMode::process() {
    State& state = *state_;
    if (state.completed) return std::nullopt;
    ++state.frames;
    if (state.screen != nullptr && state.was_shown && !state.screen->shown() && !state.scripted) {
        // An interactive run ends when the player leaves the hub.
        state.completed = true;
        state.status = "closed";
        static_cast<void>(state.write_report());
        return 0;
    }
    if (!state.scripted) return std::nullopt;
    if (state.frames <= warmup_frames) return std::nullopt;
    if (state.next_action < state.options.actions.size()) {
        const std::string& action = state.options.actions[state.next_action++];
        const bool ok = state.run_action(action);
        const std::string recorded = action.rfind("text:", 0) == 0 ? action.substr(0, action.find('=')) + "=***" : action;
        state.performed.push_back(recorded + (ok ? " ok" : " FAILED"));
        if (!ok && state.failure.empty()) state.failure = "action failed: " + recorded;
        return std::nullopt;
    }
    if (state.settle < 0) state.settle = settle_frames;
    if (state.settle > 0) {
        --state.settle;
        return std::nullopt;
    }
    state.completed = true;
    const bool captured = state.capture();
    state.status = captured && state.failure.empty() ? "ok" : "failed";
    const bool written = state.write_report();
    return state.failure.empty() && written == !state.options.report_path.empty() ? 0 : 2;
}

} // namespace eawr::presentation::godot_backend
