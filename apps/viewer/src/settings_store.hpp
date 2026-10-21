#pragma once

// #2927: the viewer's side of the player settings (docs/ui/ui-settings.md). The record, its
// text form and the defaults are engine-free (eawr/presentation/ui/settings.hpp); this file
// stores the record in the user's data folder and applies it to the engine: the audio buses,
// the window and vsync, and the render profile. Settings are UI state (UI-C2): nothing here
// reaches the simulation, a replay or a hash.

#include "render_profile.hpp"

#include "eawr/presentation/audio/sfx.hpp"
#include "eawr/presentation/ui/settings.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// The settings file: `user://eawr-settings.cfg` unless a path is given (a test names its own).
class SettingsStore final {
public:
    SettingsStore() = default;
    explicit SettingsStore(std::filesystem::path file) : file_(std::move(file)) {}

    // The file in use: the given path, else the user data folder's.
    [[nodiscard]] std::filesystem::path file() const;
    [[nodiscard]] bool exists() const;
    // A missing or unreadable file is the defaults (`defaulted`, no note for a missing one).
    [[nodiscard]] presentation::ui::SettingsLoad load() const;
    // Written to a sibling file and renamed over the target, so a crash keeps the old file.
    [[nodiscard]] bool save(const presentation::ui::PlayerSettings& settings, std::string& error) const;

private:
    std::filesystem::path file_;
};

// The settings a player's view applies, set once at start-up and read where the engine objects
// are made (the battle's audio buses, the live session's speed step). Null when none apply.
class ActiveSettings final {
public:
    [[nodiscard]] static const presentation::ui::PlayerSettings* get() noexcept;
    static void set(presentation::ui::PlayerSettings settings);
    static void clear() noexcept;
};

// What one apply did, for the report: each entry names an option and what happened to it.
struct ApplyReport final {
    std::vector<std::string> applied;
    std::vector<std::string> skipped;
};

struct ApplyTargets final {
    bool audio{true};
    // The window's mode and size; off in a capture run, whose viewport is pinned.
    bool window{true};
    bool vsync{true};
    // The render profile setting; off when --eawr-render-profile already chose one.
    bool render_profile{true};
    bool texture_detail{true};
};

// Volumes on the shared audio buses (audio_buses.hpp): the front end's music and sounds and a
// battle's. The buses are opened if the run has none yet; a battle made later takes the same
// values from ActiveSettings.
void apply_audio(const presentation::ui::AudioSettings& audio, ApplyReport& report);
// Also checks the current engine state: startup and menu commits never repeat a display setter.
void apply_video(const presentation::ui::VideoSettings& video, const ApplyTargets& targets, ApplyReport& report);
// A missing, damaged or foreign-version record uses fresh defaults without replacing launch geometry.
void apply_startup_video(const presentation::ui::SettingsLoad& loaded, ApplyTargets targets, ApplyReport& report);

// Main-thread counters of our display calls that may request swapchain recreation.
// Compare snapshots around menu gestures; the engine's internal recreations are not counted.
struct VideoApplyCounts final {
    std::uint64_t vsync{};
    std::uint64_t window_mode{};
    std::uint64_t window_size{};
};
[[nodiscard]] VideoApplyCounts video_apply_counts() noexcept;

// ST-P2: update only when the effective cap changes; menu includes the demo battle.
void update_frame_pacing(const presentation::ui::VideoSettings& video, bool menu);

// The profile a render-profile setting stands for in this run: the setting when explicit, else
// `fallback` (the run's own default).
[[nodiscard]] RenderProfile resolve_render_profile(presentation::ui::RenderProfileSetting setting,
                                                   RenderProfile fallback) noexcept;
[[nodiscard]] presentation::ui::RenderProfileSetting setting_of(RenderProfile profile) noexcept;

// The mix levels a battle's buses take from the settings.
[[nodiscard]] double audio_gain(int percent) noexcept;
[[nodiscard]] audio::MixLevels mix_levels(const presentation::ui::AudioSettings& audio) noexcept;

// The window's size, the screen's size and the vsync flag, for the options page and its report.
struct DisplayFacts final {
    presentation::ui::DisplaySize window;
    presentation::ui::DisplaySize screen;
    bool windowed{true};
    bool vsync{};
};
[[nodiscard]] DisplayFacts display_facts();

// ST-K3: match physical positions but show the active layout's letter.
[[nodiscard]] std::string keyboard_key_label(std::string_view physical_chord);
[[nodiscard]] std::string camera_keys_help(bool wasd);

} // namespace eawr::presentation::godot_backend
