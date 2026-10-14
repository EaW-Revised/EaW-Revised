#include "settings_store.hpp"

#include "audio_buses.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "viewer_path.hpp"

#include "eawr/presentation/ui/text_file.hpp"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <algorithm>
#include <cmath>
#include <system_error>

namespace eawr::presentation::godot_backend {
namespace ui = presentation::ui;
namespace {

// The settings the player's view applies (ActiveSettings). Set on the main thread at start-up
// and read where the engine objects are made, also on the main thread.
std::optional<ui::PlayerSettings>& active_storage() {
    static std::optional<ui::PlayerSettings> storage;
    return storage;
}

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

void apply_video(const ui::VideoSettings& video, const ApplyTargets& targets, ApplyReport& report) {
    if (targets.texture_detail) {
        GodotRenderer::set_texture_mip_drop(static_cast<std::uint32_t>(std::clamp(video.texture_mip_drop, 0, 4)));
        report.applied.emplace_back("texture mip drop: " + std::to_string(video.texture_mip_drop));
    }
    godot::DisplayServer* display = godot::DisplayServer::get_singleton();
    if (display == nullptr) {
        report.skipped.emplace_back("video: no display server");
        return;
    }
    if (targets.vsync) {
        // Mailbox keeps synchronized presentation without blocking rendering on FIFO pacing.
        // Godot falls back to enabled (FIFO) when the driver cannot provide mailbox.
        display->window_set_vsync_mode(video.vsync ? godot::DisplayServer::VSYNC_MAILBOX
                                                    : godot::DisplayServer::VSYNC_DISABLED);
        report.applied.emplace_back(std::string("vsync: ") + (video.vsync ? "on" : "off"));
    } else {
        report.skipped.emplace_back("vsync: not applied in this run");
    }
    if (!targets.window) {
        report.skipped.emplace_back("window: not applied in this run");
        return;
    }
    if (video.windowed) {
        display->window_set_mode(godot::DisplayServer::WINDOW_MODE_WINDOWED);
        display->window_set_size(godot::Vector2i(video.window_width, video.window_height));
        report.applied.emplace_back("window: " + std::to_string(video.window_width) + "x"
                                    + std::to_string(video.window_height));
    } else {
        // The window system keeps the display's own resolution in full screen; the chosen
        // full-screen size is stored for the player but not applied (ST-V1).
        display->window_set_mode(godot::DisplayServer::WINDOW_MODE_FULLSCREEN);
        report.applied.emplace_back("window: full screen");
    }
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
