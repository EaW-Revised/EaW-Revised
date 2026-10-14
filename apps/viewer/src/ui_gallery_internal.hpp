#pragma once

#include "ui_gallery_mode.hpp"

#include "capture_viewport.hpp"
#include "viewer_path.hpp"

#include "ui/credits_view.hpp"
#include "ui/dialog_builder.hpp"
#include "ui/font_provider.hpp"
#include "ui/hud_flytext.hpp"
#include "ui/kit.hpp"
#include "ui/load_game_view.hpp"
#include "ui/message_box_view.hpp"
#include "ui/movie_player.hpp"
#include "ui/tactical_hud.hpp"
#include "ui/theme_builder.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/theme.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace ui_gallery_detail {

namespace model = presentation::ui;
namespace data = eawr::data::ui;

constexpr int warmup_frames = 8;
// The movie page runs at least this many frames and this long, so the report
// sees the stream advance, and a short stream loop, at any frame rate.
constexpr int movie_frames = 120;
constexpr std::chrono::milliseconds movie_minimum_time{1500};
// Movie_tactical and Movie_galactic are 200-unit Icon slots on the
// 768-unit-high reference screen (CommandBarComponents.xml, UI-L1).
constexpr float movie_slot_units = 200.0F;
constexpr float reference_height = 768.0F;
// One drawn control: what it shows and, after the capture, how many distinct
// colours its rect holds (a skinned control is never flat).
struct Sample final {
    std::string kind;
    std::string state;
    std::string variation;
    Control* control{};
    Rect2 rect;
    std::size_t colours{};
};

} // namespace ui_gallery_detail

using namespace ui_gallery_detail;

struct UiGalleryMode::State final {
    explicit State(Options value) : options(std::move(value)) {}
    ~State() { if (load_active == this) load_active = nullptr; }

    Options options;
    std::string font_cache_source;
    std::optional<vfs::Vfs> filesystem;
    std::optional<vfs::Vfs> font_filesystem;
    std::optional<data::DialogCatalog> catalog;
    std::optional<data::TextDatabase> text_database;
    std::optional<data::TextLookup> text_lookup;
    std::optional<assets::MegaTextureAtlas> atlas;
    std::unique_ptr<FontProvider> fonts;
    std::optional<model::ThemeModel> theme_model;
    std::unique_ptr<UiTextures> textures;
    Ref<Theme> theme;
    ThemeBuildSummary summary;
    std::vector<std::string> notes;
    std::vector<Sample> samples;
    std::optional<BuiltDialog> dialog;
    std::optional<data::HudMovie> movie;
    std::optional<eawr::data::Catalog> hud_objects;
    std::unique_ptr<TacticalHud> hud;
    std::unique_ptr<HudFlyTextRenderer> flytext;
    std::optional<model::HudIconText> flytext_style;
    model::HudFlyTextView flytext_view;
    std::vector<model::CardUnit> demo_cards;
    // --eawr-message-box (MNU-42): the box under test, the probes "world" buttons under it and the
    // scripted clicks (ui_gallery_box.cpp).
    struct BoxStep {
        std::string name;
        std::function<void()> act;
        std::function<std::string()> check; // empty when the step held
        std::chrono::milliseconds wait_time{}; // real time to wait after the act; zero waits a few frames
    };
    struct BoxStepResult {
        std::string name;
        std::string problem;
    };
    model::MessageBoxRequest box_request;
    MessageBoxContext box_context;
    EawrMessageBox* box{};
    std::int64_t box_id{};
    Node* box_parent{};
    std::uint64_t box_probe_presses{};
    std::vector<std::int64_t> box_results;
    std::vector<BoxStep> box_steps;
    std::vector<BoxStepResult> box_step_results;
    std::size_t box_step{};
    std::chrono::steady_clock::time_point box_wait_until;
    int box_wait{};
    bool box_acting{true};
    bool box_captured{};
    static State* box_active;
    // --eawr-load-game (MNU-39, MNU-41): the screen under test; its scripted clicks reuse the box
    // steps and their runner (ui_gallery_load_game.cpp).
    EawrLoadGameScreen* load_screen{};
    EawrSaveReplayScreen* save_screen{};
    int64_t save_id{};
    int replay_serializations{};
    EawrBattleOverlay* replay_end{};
    std::int64_t load_id{};
    CanvasLayer* layer{};
    std::optional<std::pair<std::string, std::string>> load_play; // path, map of the chosen recording
    // The saved game Load handed to the launcher (directory, name), and how often the launcher ran.
    std::optional<std::pair<std::string, std::string>> load_save;
    int load_launches{};
    // The saved-game rows as the screen listed them first, for the report.
    struct LoadRow final {
        std::string name, text, date, problem;
        bool playable{};
    };
    std::vector<LoadRow> load_rows;
    int load_loading_boxes{}; // Loading boxes the screen had opened when the save was chosen
    bool load_closed{};
    std::string load_store;
    std::string load_saves;
    std::string load_refusal; // what the refusing launcher answers
    static State* load_active;
    // --eawr-credits (MNU-07): the credits screen under test, what it showed (kept after the screen
    // frees itself) and its scripted skip (ui_gallery_credits.cpp).
    struct CreditsSnapshot {
        bool opened{};
        bool movie{};
        std::string movie_name;
        std::string movie_source;
        std::array<std::int32_t, 2> texture{};
        std::size_t lines{};
        std::size_t logos{};
        float moved{};
        double elapsed{};
        std::vector<std::string> notes;
    };
    EawrCreditsScreen* credits_screen{};
    std::int64_t credits_id{};
    bool credits_closed{};
    CreditsSnapshot credits;
    std::set<std::string> credits_frame_hashes;
    unsigned credits_ticks{};
    static State* credits_active;
    VideoStreamPlayer* movie_player{};
    std::set<std::string> movie_frame_hashes;
    double movie_last_position{};
    unsigned movie_loops{};
    unsigned movie_ticks{};
    std::chrono::steady_clock::time_point movie_deadline;
    Node3D* host{};
    std::uint32_t width{};
    std::uint32_t height{};
    int frames{warmup_frames};
    bool completed{};
    std::string status{"failed"};
    std::string failure;
    std::optional<std::array<std::int32_t, 2>> png;
    std::string capture_sha256;
    BackendInfo backend;

    [[nodiscard]] std::filesystem::path cache_directory();
    [[nodiscard]] bool load();
    [[nodiscard]] String caption(std::string_view key);
    void build_controls(Control& root);
    void build_dialog_page(Control& root);
    void build_movie_page(Control& root, Vector2 size);
    void build_hud_page(Node3D& parent);
    void build_box_page(Node& parent, Control& root);
    void build_load_game_page(Control& root);
    [[nodiscard]] bool load_alive();
    void build_credits_page(Control& root);
    [[nodiscard]] bool credits_alive();
    void sample_credits();
    static void on_credits_closed();
    static void on_load_replay(const String& path, const String& map);
    static void on_load_save(const String& directory, const String& name);
    static void on_load_closed();
    // One frame of the box script; true once every step has run.
    [[nodiscard]] bool run_box_script();
    void open_box();
    [[nodiscard]] bool box_alive();
    static void on_box_probe();
    static void on_box_result(int64_t result);
    void sample_movie();
    [[nodiscard]] bool capture();
    [[nodiscard]] bool write_report() const;
};

} // namespace eawr::presentation::godot_backend
