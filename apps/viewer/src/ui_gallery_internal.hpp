#pragma once

#include "ui_gallery_mode.hpp"

#include "capture_viewport.hpp"
#include "viewer_path.hpp"

#include "ui/dialog_builder.hpp"
#include "ui/font_provider.hpp"
#include "ui/kit.hpp"
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
    std::vector<model::CardUnit> demo_cards;
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
    void sample_movie();
    [[nodiscard]] bool capture();
    [[nodiscard]] bool write_report() const;
};

} // namespace eawr::presentation::godot_backend
