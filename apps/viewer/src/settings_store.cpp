#include "settings_store.hpp"

#include "audio_buses.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "viewer_path.hpp"

#include "eawr/presentation/ui/text_file.hpp"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <algorithm>
#include <cmath>
#include <system_error>

namespace eawr::presentation::godot_backend {
namespace ui = presentation::ui;

std::string keyboard_key_label(const std::string_view physical_chord) {
    std::string label(physical_chord);
    if (label.empty()) return label;
    const char letter = label.back();
    if (letter < 'A' || letter > 'Z' || (label.size() != 1 && label[label.size() - 2] != '+')) return label;
    auto* display = godot::DisplayServer::get_singleton();
    auto* os = godot::OS::get_singleton();
    if (!display || !os) return label;
    const auto physical = static_cast<godot::Key>(static_cast<int64_t>(godot::KEY_A) + letter - 'A');
    const auto layout = display->keyboard_get_keycode_from_physical(physical);
    if (layout == godot::KEY_NONE) return label;
    const godot::CharString converted = os->get_keycode_string(layout).utf8();
    label.replace(label.size() - 1, 1, converted.get_data());
    return label;
}

std::string camera_keys_help(const bool wasd) {
    std::string result = "Pan: Arrows";
    if (wasd) result += " / " + keyboard_key_label("W") + keyboard_key_label("A")
        + keyboard_key_label("S") + keyboard_key_label("D");
    for (const auto& [control, caption] : std::array<std::pair<const char*, const char*>, 3>{{
             {"IDC_EDIT_ATTACK", "Attack"}, {"IDC_EDIT_STOP", "Stop"}, {"IDC_EDIT_WAYPOINT", "Waypoint"}}}) {
        result += "   " + std::string(caption) + ": " + keyboard_key_label(ui::project_key_for(control, wasd));
    }
    return result;
}

namespace {

// The settings the player's view applies (ActiveSettings). Set on the main thread at start-up
// and read where the engine objects are made, also on the main thread.
std::optional<ui::PlayerSettings>& active_storage() {
    static std::optional<ui::PlayerSettings> storage;
    return storage;
}

VideoApplyCounts display_applies;

} // namespace

std::filesystem::path SettingsStore::file() const {
    if (!file_.empty()) return file_;
    const godot::String path = godot::ProjectSettings::get_singleton()->globalize_path("user://eawr-settings.cfg");
    const godot::CharString utf8 = path.utf8();
    return ViewerPath{std::string(utf8.get_data(), static_cast<std::size_t>(utf8.length()))}.native();
}

bool SettingsStore::exists() const {
    std::error_code error;
    return std::filesystem::is_regular_file(file(), error);
}

ui::SettingsLoad SettingsStore::load() const {
    const std::optional<std::string> text = ui::read_text_file(file());
    if (!text) {
        ui::SettingsLoad missing;
        missing.defaulted = true;
        return missing;
    }
    return ui::parse_settings(*text);
}

bool SettingsStore::save(const ui::PlayerSettings& settings, std::string& error) const {
    return ui::write_text_file_atomic(file(), ui::serialize_settings(settings), error);
}

const ui::PlayerSettings* ActiveSettings::get() noexcept {
    return active_storage() ? &*active_storage() : nullptr;
}

void ActiveSettings::set(ui::PlayerSettings settings) { active_storage() = std::move(settings); }

void ActiveSettings::clear() noexcept { active_storage().reset(); }

double audio_gain(const int percent) noexcept { return ui::volume_gain(percent); }

audio::MixLevels mix_levels(const ui::AudioSettings& audio) noexcept {
    audio::MixLevels levels;
    levels.master = audio_gain(audio.master_percent);
    levels.sfx = audio_gain(audio.sfx_percent);
    levels.speech = audio_gain(audio.speech_percent);
    levels.music = audio_gain(audio.music_percent);
    return levels;
}

void apply_audio(const ui::AudioSettings& audio, ApplyReport& report) {
    godot::AudioServer* server = godot::AudioServer::get_singleton();
    if (server == nullptr) {
        report.skipped.emplace_back("audio: no audio server");
        return;
    }
    // The buses exist from the viewer's start (audio_buses.hpp); a run that skipped that opens
    // them here, so the front end's music and a later battle hear the same levels.
    audio_buses::ensure(mix_levels(audio));
    report.applied.emplace_back("audio: " + std::to_string(audio_buses::count) + " bus level(s)");
}

VideoApplyCounts video_apply_counts() noexcept { return display_applies; }

void apply_startup_video(const ui::SettingsLoad& loaded, ApplyTargets targets, ApplyReport& report) {
    if (loaded.defaulted) {
        targets.window = false;
        targets.texture_detail = false;
    }
    apply_video(loaded.settings.video, targets, report);
}

void apply_video(const ui::VideoSettings& video, const ApplyTargets& targets, ApplyReport& report) {
    if (targets.texture_detail) {
        const auto drop = static_cast<std::uint32_t>(std::clamp(video.texture_mip_drop, 0, 4));
        if (GodotRenderer::gpu_memory().budget.minimum_drop != drop) {
            GodotRenderer::set_texture_mip_drop(drop);
            report.applied.emplace_back("texture mip drop: " + std::to_string(video.texture_mip_drop));
        }
    }
    godot::DisplayServer* display = godot::DisplayServer::get_singleton();
    if (display == nullptr) {
        report.skipped.emplace_back("video: no display server");
        return;
    }
    if (targets.vsync) {
        // ST-V5: request FIFO directly; repeating an unsupported mode can recreate the swapchain.
        const auto desired = video.vsync ? godot::DisplayServer::VSYNC_ENABLED : godot::DisplayServer::VSYNC_DISABLED;
        if (display->window_get_vsync_mode() != desired) {
            display->window_set_vsync_mode(desired);
            ++display_applies.vsync;
            report.applied.emplace_back(std::string("vsync: ") + (video.vsync ? "on" : "off"));
        }
    } else {
        report.skipped.emplace_back("vsync: not applied in this run");
    }
    if (!targets.window) {
        report.skipped.emplace_back("window: not applied in this run");
        return;
    }
    const auto mode = display->window_get_mode();
    const bool windowed = mode != godot::DisplayServer::WINDOW_MODE_FULLSCREEN
        && mode != godot::DisplayServer::WINDOW_MODE_EXCLUSIVE_FULLSCREEN;
    bool window_changed = false;
    if (video.windowed) {
        if (!windowed) {
            display->window_set_mode(godot::DisplayServer::WINDOW_MODE_WINDOWED);
            ++display_applies.window_mode;
            window_changed = true;
        }
        if (display->window_get_size() != godot::Vector2i(video.window_width, video.window_height)) {
            display->window_set_size(godot::Vector2i(video.window_width, video.window_height));
            ++display_applies.window_size;
            window_changed = true;
        }
        if (window_changed) report.applied.emplace_back("window: " + std::to_string(video.window_width) + "x"
                                                       + std::to_string(video.window_height));
    } else {
        // The window system keeps the display's own resolution in full screen; the chosen
        // full-screen size is stored for the player but not applied (ST-V1).
        if (windowed) {
            display->window_set_mode(godot::DisplayServer::WINDOW_MODE_FULLSCREEN);
            ++display_applies.window_mode;
            report.applied.emplace_back("window: full screen");
        }
    }
}

void update_frame_pacing(const ui::VideoSettings& video, const bool menu) {
    const godot::DisplayServer* display = godot::DisplayServer::get_singleton();
    godot::Engine* engine = godot::Engine::get_singleton();
    if (display == nullptr || engine == nullptr) return;
    const double hz = display->screen_get_refresh_rate(display->window_get_current_screen());
    const int refresh = std::isfinite(hz) && hz >= 30.0 && hz <= 1000.0 ? static_cast<int>(hz) : 60;
    const bool focused = display->window_is_focused()
        && display->window_get_mode() != godot::DisplayServer::WINDOW_MODE_MINIMIZED;
    const int cap = ui::frame_rate_cap(video, refresh, menu, focused);
    if (engine->get_max_fps() != cap) engine->set_max_fps(cap);
}

RenderProfile resolve_render_profile(const ui::RenderProfileSetting setting, const RenderProfile fallback) noexcept {
    switch (setting) {
    case ui::RenderProfileSetting::retail: return RenderProfile::retail;
    case ui::RenderProfileSetting::enhanced: return RenderProfile::enhanced;
    case ui::RenderProfileSetting::automatic: break;
    }
    return fallback;
}

ui::RenderProfileSetting setting_of(const RenderProfile profile) noexcept {
    return profile == RenderProfile::enhanced ? ui::RenderProfileSetting::enhanced : ui::RenderProfileSetting::retail;
}

DisplayFacts display_facts() {
    DisplayFacts facts;
    const godot::DisplayServer* display = godot::DisplayServer::get_singleton();
    if (display == nullptr) return facts;
    const godot::Vector2i screen = display->screen_get_size();
    const godot::Vector2i window = display->window_get_size();
    facts.screen = {screen.x, screen.y};
    facts.window = {window.x, window.y};
    const auto mode = display->window_get_mode();
    facts.windowed = mode != godot::DisplayServer::WINDOW_MODE_FULLSCREEN
        && mode != godot::DisplayServer::WINDOW_MODE_EXCLUSIVE_FULLSCREEN;
    facts.vsync = display->window_get_vsync_mode() != godot::DisplayServer::VSYNC_DISABLED;
    return facts;
}

} // namespace eawr::presentation::godot_backend
