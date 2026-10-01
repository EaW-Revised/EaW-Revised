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
namespace {

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
const Color backdrop(0.03F, 0.05F, 0.11F);

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}

[[nodiscard]] String text(const std::string_view value) {
    return String::utf8(value.data(), static_cast<int64_t>(value.size()));
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

[[nodiscard]] std::string_view state_name(const KitState state) {
    switch (state) {
    case KitState::normal: return "normal";
    case KitState::hover: return "hover";
    case KitState::pressed: return "pressed";
    case KitState::disabled: return "disabled";
    default: return "automatic";
    }
}

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

// --eawr-ui-hud-cards: <type>[*<count>][:<ABILITY>][@<health>][~<shield>], comma-separated; made-up
// entity IDs from 1 in order.
[[nodiscard]] std::optional<std::vector<model::CardUnit>> demo_card_units(const std::string& spec) {
    std::vector<model::CardUnit> units;
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t comma = spec.find(',', start);
        std::string entry = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? spec.size() + 1 : comma + 1;
        const auto take = [&entry](const char mark) -> std::optional<std::string> {
            const std::size_t at = entry.find(mark);
            if (at == std::string::npos) return std::nullopt;
            std::size_t end = entry.find_first_of("*:@~", at + 1);
            std::string value = entry.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
            entry.erase(at, end == std::string::npos ? std::string::npos : end - at);
            return value;
        };
        const auto shield = take('~');
        const auto health = take('@');
        const auto ability = take(':');
        const auto count = take('*');
        if (entry.empty()) return std::nullopt;
        std::size_t copies = 1;
        double health_value = 1.0;
        std::optional<double> shield_value;
        try {
            if (count) copies = static_cast<std::size_t>(std::stoul(*count));
            if (health) health_value = std::stod(*health);
            if (shield) shield_value = std::stod(*shield);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (copies == 0 || copies > 200 || units.size() + copies > 400) return std::nullopt;
        for (std::size_t copy = 0; copy < copies; ++copy) {
            model::CardUnit unit;
            unit.id = static_cast<sim::EntityId>(units.size() + 1);
            unit.members = {unit.id};
            unit.type = entry;
            unit.ability = ability ? model::ability_index(*ability) : model::ability_none;
            unit.health = health_value;
            unit.shield = shield_value;
            units.push_back(std::move(unit));
        }
    }
    return units;
}

} // namespace

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

std::filesystem::path UiGalleryMode::State::cache_directory() {
    if (!options.font_cache.empty()) {
        font_cache_source = "flag";
        return options.font_cache;
    }
    const String environment = OS::get_singleton()->get_environment("EAWR_FONT_CACHE");
    if (!environment.is_empty()) {
        font_cache_source = "environment";
        return ViewerPath{utf8(environment)}.native();
    }
    font_cache_source = "checkout";
    const std::filesystem::path project = ViewerPath{utf8(ProjectSettings::get_singleton()->globalize_path("res://"))}
                                              .native();
    return (project / ".." / ".." / ".." / "out" / "fonts").lexically_normal();
}

bool UiGalleryMode::State::load() {
    if (options.game_root.empty()) {
        failure = "--eawr-ui-gallery requires --eawr-game-root";
        return false;
    }
    // The project targets FoC: the expansion over the base game, and a mod
    // (its folder or its Data folder) over both when one is given.
    const std::filesystem::path expansion = options.game_root / "corruption" / "Data";
    const std::filesystem::path base = options.game_root / "GameData" / "Data";
    if (!std::filesystem::is_directory(expansion) || !std::filesystem::is_directory(base)) {
        failure = "the game root needs corruption/Data and GameData/Data (FoC)";
        return false;
    }
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    if (!options.mod_root.empty()) {
        const std::filesystem::path data = options.mod_root / "Data";
        roots.emplace_back("mod", std::filesystem::is_directory(data) ? data : options.mod_root);
    }
    roots.emplace_back("expansion", expansion);
    roots.emplace_back("base", base);
    std::vector<vfs::MountSpec> specs;
    for (const auto& [id, root] : roots) {
        auto manifest = vfs::resolve_manifest_mount(id, root);
        if (!manifest) {
            failure = core::format_diagnostic(manifest.error());
            return false;
        }
        specs.push_back(std::move(manifest.value().mount));
    }
    auto mounted = vfs::Vfs::mount(specs);
    if (!mounted) {
        failure = core::format_diagnostic(mounted.error());
        return false;
    }
    filesystem.emplace(std::move(mounted.value()));

    // The HUD page loads its own shell, textures and fonts (TacticalHud).
    if (!options.hud.empty()) return true;

    // The movie page needs neither the skin nor the fonts.
    if (!options.movie.empty()) {
        if (options.movie_cache.empty()) {
            failure = "--eawr-ui-movie requires --eawr-movie-cache";
            return false;
        }
        auto resolved = data::resolve_hud_movie(*filesystem, options.movie);
        if (!resolved) {
            failure = core::format_diagnostic(resolved.error());
            return false;
        }
        movie.emplace(std::move(resolved.value()));
        frames = movie_frames;
        return true;
    }

    auto loaded_catalog = data::load_dialog_catalog(*filesystem);
    if (!loaded_catalog) {
        failure = core::format_diagnostic(loaded_catalog.error());
        return false;
    }
    catalog.emplace(std::move(loaded_catalog.value()));
    const std::string mtd = "Data/Art/Textures/" + catalog->skin.texture_file + ".mtd";
    auto loaded_atlas = assets::load_mega_texture_atlas(*filesystem, mtd);
    if (!loaded_atlas) {
        failure = core::format_diagnostic(loaded_atlas.error());
        return false;
    }
    atlas.emplace(std::move(loaded_atlas.value()));
    if (auto loaded_text = data::load_language_text_database(*filesystem, "ENGLISH")) {
        text_database.emplace(std::move(loaded_text.value()));
        text_lookup.emplace(*text_database);
    } else {
        notes.push_back("text database: " + core::format_diagnostic(loaded_text.error()) + "; captions show their keys");
    }

    // The font cache is its own loose layer at fonts/; a missing directory is an empty cache.
    const std::filesystem::path directory = cache_directory();
    const vfs::MountSpec mount{.layer_id = "font-cache", .data_root = directory,
                               .loose_logical_prefix = std::string(model::font_cache_prefix), .active_archives = {}};
    auto font_mount = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&mount, 1));
    if (font_mount) font_filesystem.emplace(std::move(font_mount.value()));
    const vfs::Vfs unmounted;
    fonts = std::make_unique<FontProvider>(
        model::load_font_cache(font_filesystem ? *font_filesystem : unmounted, ViewerPath::utf8(directory)));

    const model::LayoutRules rules = options.rules == "retail" ? model::LayoutRules::retail
                                                                : model::LayoutRules::aspect_correct;
    model::ThemeSources sources;
    sources.catalog = &*catalog;
    sources.atlas = &atlas->directory;
    sources.standalone = model::vfs_standalone_textures(*filesystem);
    sources.fonts = &fonts->cache();
    const std::vector<std::string>& families = fonts->system_families();
    sources.system = [&families](const std::string_view face) {
        return model::match_system_face(face, families).has_value();
    };
    theme_model.emplace(model::build_theme_model(sources, model::reference_space({width, height}, rules)));
    textures = std::make_unique<UiTextures>(*atlas, *filesystem);
    if (!textures->page_failure().empty()) {
        failure = "atlas page: " + textures->page_failure();
        return false;
    }
    theme = build_theme(*theme_model, *textures, *fonts, &summary);
    return true;
}

String UiGalleryMode::State::caption(const std::string_view key) {
    if (!text_lookup) return text(key);
    return text(data::to_utf8(text_lookup->text(key)));
}

void UiGalleryMode::State::build_controls(Control& root) {
    const float k = static_cast<float>(height) / 720.0F;
    const auto at = [k](const float x, const float y, const float w, const float h) {
        return Rect2(std::round(x * k), std::round(y * k), std::round(w * k), std::round(h * k));
    };
    const auto place = [&](Control* control, const Rect2& rect, const std::string& kind, const KitState state,
                           const std::string& variation = std::string(model::theme_type)) {
        control->set_position(rect.position);
        control->set_size(rect.size);
        control->set_theme_type_variation(text(variation));
        root.add_child(control);
        samples.push_back({kind, std::string(state_name(state)), variation, control, rect, 0U});
        return control;
    };
    const auto label = [&](const String& value, const Rect2& rect, const std::string_view role,
                           const HorizontalAlignment alignment = HORIZONTAL_ALIGNMENT_LEFT,
                           const std::string& variation = std::string(model::theme_type)) {
        auto* item = memnew(EawrUiLabel);
        item->set_text(value);
        item->set_role(text(role));
        item->set_alignment(alignment);
        item->set_wrap(false);
        item->set_position(rect.position);
        item->set_size(rect.size);
        item->set_theme_type_variation(text(variation));
        root.add_child(item);
        return item;
    };

    auto* panel = memnew(EawrUiFrame);
    place(panel, at(28.0F, 28.0F, 1224.0F, 664.0F), "frame", KitState::normal);
    label(caption("TEXT_GAME_OPTIONS_TITLE") + " - UI kit (UI-06)", at(48.0F, 40.0F, 800.0F, 30.0F), "L_Text",
          HORIZONTAL_ALIGNMENT_LEFT, "EawrUi__IDC_STATIC_MEDIUM");

    const std::array<KitState, 4> states{KitState::normal, KitState::hover, KitState::pressed, KitState::disabled};
    const std::array<const char*, 4> headers{"Normal", "Mouse over", "Pressed / on", "Disabled"};
    const float column0 = 232.0F;
    const float column_step = 170.0F;
    const float cell = 156.0F;
    for (std::size_t index = 0; index < headers.size(); ++index) {
        label(headers[index], at(column0 + column_step * static_cast<float>(index), 78.0F, cell, 16.0F),
              "Global_Default", HORIZONTAL_ALIGNMENT_CENTER);
    }
    float row = 100.0F;
    const float row_step = 46.0F;
    const auto row_label = [&](const char* name) {
        label(name, at(48.0F, row + 3.0F, 176.0F, 18.0F), "L_Text");
    };
    const auto column = [&](const std::size_t index, const float height_px, const float width_px = 156.0F) {
        return at(column0 + column_step * static_cast<float>(index), row, width_px, height_px);
    };
    const float button_height = 24.0F * 0.9375F;

    row_label("Push button");
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* button = memnew(EawrUiButton);
        button->set_text(caption("TEXT_BUTTON_RESUME_GAME"));
        button->set_forced_state(states[index]);
        if (states[index] == KitState::disabled) button->set_disabled(true);
        place(button, column(index, button_height), "button", states[index]);
    }
    row += row_step;

    row_label("Button variation");
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* button = memnew(EawrUiButton);
        button->set_text(caption("TEXT_BUTTON_CANCEL"));
        button->set_forced_state(states[index]);
        place(button, column(index, button_height), "button", states[index], "EawrUi__IDC_BUTTON_KICK_PLAYER0");
    }
    row += row_step;

    row_label("Check box");
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* check = memnew(EawrUiCheck);
        check->set_text(caption("TEXT_CHECK_USE_MOVIE_SUBTITLES"));
        check->set_forced_state(states[index]);
        check->set_pressed(states[index] == KitState::pressed || states[index] == KitState::disabled);
        place(check, column(index, 22.0F), "check", states[index]);
    }
    row += row_step;

    row_label("Radio button");
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* radio = memnew(EawrUiCheck);
        radio->set_radio(true);
        radio->set_text("Easy");
        radio->set_forced_state(states[index]);
        radio->set_pressed(states[index] == KitState::pressed || states[index] == KitState::disabled);
        place(radio, column(index, 22.0F), "radio", states[index]);
    }
    row += row_step;

    row_label("Slider");
    const std::array<std::pair<KitState, KitState>, 4> slider_states{{
        {KitState::normal, KitState::normal},
        {KitState::hover, KitState::hover},
        {KitState::pressed, KitState::pressed},
        {KitState::disabled, KitState::disabled},
    }};
    for (std::size_t index = 0; index < slider_states.size(); ++index) {
        auto* slider = memnew(EawrUiSlider);
        slider->set_max(100.0);
        slider->set_value(20.0 + 20.0 * static_cast<double>(index));
        slider->set_forced_states(slider_states[index].first, slider_states[index].second);
        place(slider, column(index, 24.0F), "slider", states[index]);
    }
    row += row_step;

    row_label("Combo box");
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* combo = memnew(EawrUiCombo);
        combo->add_item("Easy AI");
        combo->add_item("Hard AI");
        combo->set_forced_state(states[index]);
        if (states[index] == KitState::disabled) combo->set_disabled(true);
        place(combo, column(index, 25.0F * 0.9375F), "combo", states[index]);
    }
    row += row_step;

    row_label("Edit box");
    const std::array<const char*, 4> edits{"Commander", "Commander", "", "Commander"};
    for (std::size_t index = 0; index < states.size(); ++index) {
        auto* edit = memnew(EawrUiEdit);
        edit->set_text(edits[index]);
        edit->set_placeholder("Player name");
        if (states[index] == KitState::disabled) edit->set_editable(false);
        place(edit, column(index, 20.0F), "edit", states[index]);
        if (states[index] == KitState::pressed) edit->grab_focus();
    }
    row += row_step;

    row_label("Progress bar");
    const std::array<double, 4> fills{0.0, 35.0, 70.0, 100.0};
    for (std::size_t index = 0; index < fills.size(); ++index) {
        auto* bar = memnew(EawrUiBar);
        bar->set_max(100.0);
        bar->set_value(fills[index]);
        place(bar, column(index, 14.0F), "bar", KitState::normal);
    }
    row += row_step;

    row_label("Text roles");
    struct Role final {
        const char* role;
        const char* sample;
        const char* variation;
        HorizontalAlignment alignment;
    };
    const std::array<Role, 4> roles{{
        {"Global_Default", "Global_Default", "EawrUi", HORIZONTAL_ALIGNMENT_LEFT},
        {"L_Text", "L_Text x1.3", "EawrUi", HORIZONTAL_ALIGNMENT_LEFT},
        {"L_Text", "Gradient", "EawrUi__IDD_PLANET_QUICK_REFERENCE", HORIZONTAL_ALIGNMENT_LEFT},
        {"Overlay_Caption_Text", "Outline", "EawrUi", HORIZONTAL_ALIGNMENT_LEFT},
    }};
    for (std::size_t index = 0; index < roles.size(); ++index) {
        auto* item = label(roles[index].sample, column(index, 22.0F), roles[index].role, roles[index].alignment,
                           roles[index].variation);
        samples.push_back({"label", roles[index].role, roles[index].variation, item, column(index, 22.0F), 0U});
    }
    row += row_step;

    // The list and a bare small frame sit in a column on the right.
    label("List box", at(930.0F, 78.0F, 290.0F, 16.0F), "Global_Default", HORIZONTAL_ALIGNMENT_CENTER);
    auto* list = memnew(EawrUiList);
    for (const char* name : {"(2) Alderaan", "(2) Bespin", "(4) Shipyards of Kuat", "(4) Tatooine Straits",
                             "(4) Utapau Insurgence", "(6) Alderaan Defense", "(6) Chaos Above Kashyyyk",
                             "(6) Coruscant Siege", "(6) High Point Felucia", "(6) Kamino Storm",
                             "(6) Resistance Over Shola", "(6) The Maw Installation", "(8) Hoth Conflict",
                             "(8) Yavin Lost", "(9) Saleucami Unseen"}) {
        list->add_item(name);
    }
    list->select(7);
    place(list, at(930.0F, 100.0F, 290.0F, 226.0F), "list", KitState::normal);
    label("Small frame", at(930.0F, 344.0F, 290.0F, 16.0F), "Global_Default", HORIZONTAL_ALIGNMENT_CENTER);
    auto* small = memnew(EawrUiFrame);
    small->set_small(true);
    place(small, at(930.0F, 366.0F, 290.0F, 60.0F), "small_frame", KitState::normal);
    label("Dialog frame: this panel", at(930.0F, 444.0F, 290.0F, 16.0F), "Global_Default",
          HORIZONTAL_ALIGNMENT_CENTER);
}

void UiGalleryMode::State::build_dialog_page(Control& root) {
    const data::Dialog* found = catalog->script.find(options.dialog);
    if (found == nullptr) {
        failure = "dialog " + options.dialog + " is not in the catalogue";
        return;
    }
    dialog.emplace(build_dialog(*found, *theme_model, text_lookup ? &*text_lookup : nullptr,
                                model::Placement::centre));
    root.add_child(dialog->frame);
    const model::PixelRect& frame = dialog->layout.frame;
    samples.push_back({"dialog", "normal", theme_model->variation(*found, nullptr), dialog->frame,
                       Rect2(static_cast<float>(frame.x), static_cast<float>(frame.y), static_cast<float>(frame.width),
                             static_cast<float>(frame.height)),
                       0U});
    for (const BuiltGadget& gadget : dialog->gadgets) {
        if (gadget.control == nullptr) continue;
        samples.push_back({gadget.kind, "normal", gadget.variation, gadget.control,
                           Rect2(static_cast<float>(gadget.rect.x), static_cast<float>(gadget.rect.y),
                                 static_cast<float>(gadget.rect.width), static_cast<float>(gadget.rect.height)),
                           0U});
    }
}

// The movie at twice its HUD slot size, centred, with a lighter band behind
// its right half so the capture shows what the alpha lets through.
void UiGalleryMode::State::build_hud_page(Node3D& parent) {
    TacticalHud::Options hud_options;
    const auto faction = model::hud_faction_from(options.hud);
    if (!faction) {
        failure = "--eawr-ui-hud expects empire, rebel or underworld";
        return;
    }
    hud_options.faction = *faction;
    hud_options.rules = options.rules == "retail" ? model::LayoutRules::retail : model::LayoutRules::aspect_correct;
    const std::filesystem::path directory = cache_directory();
    const vfs::MountSpec mount{.layer_id = "font-cache", .data_root = directory,
                               .loose_logical_prefix = std::string(model::font_cache_prefix), .active_archives = {}};
    auto font_mount = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&mount, 1));
    const vfs::Vfs unmounted;
    hud_options.font_cache = model::load_font_cache(font_mount ? font_mount.value() : unmounted, ViewerPath::utf8(directory));
    hud_options.font_cache_source = font_cache_source;
    // The planet: the map's root field 0x09 and its Planet object (hud_shell.hpp).
    // The HUD keeps reading the objects (the unit cards' Icon_Name and Text_ID), so they live in the state.
    std::optional<eawr::data::Catalog>& objects = hud_objects;
    if (auto loaded = eawr::data::load_catalog(*filesystem, eawr::data::Profile::foc)) {
        objects.emplace(std::move(loaded.value().catalog));
    } else {
        notes.push_back("objects: " + core::format_diagnostic(loaded.error()));
    }
    std::optional<std::string> context;
    const assets::ObjectTypeCatalog types = objects ? assets::object_type_catalog(*objects) : assets::ObjectTypeCatalog{};
    if (auto map = assets::load_map(*filesystem, options.hud_map, types)) {
        context = map.value().context_name;
    } else {
        notes.push_back("--eawr-ui-hud-map: " + core::format_diagnostic(map.error()));
    }
    hud = std::make_unique<TacticalHud>(std::move(hud_options));
    if (!hud->build(*filesystem, objects ? &*objects : nullptr, context, parent)) {
        failure = "--eawr-ui-hud: " + hud->failure();
        return;
    }
    if (!options.hud_cards.empty()) {
        auto units = demo_card_units(options.hud_cards);
        if (!units) {
            failure = "--eawr-ui-hud-cards expects <type>[*<count>][:<ABILITY>][@<health>][~<shield>], comma-separated";
            return;
        }
        if (EawrUnitCards* cards = hud->unit_cards()) {
            demo_cards = std::move(*units);
            hud->set_unit_cards(model::layout_unit_cards(demo_cards, cards->slot_count()), demo_cards);
        } else {
            notes.push_back("--eawr-ui-hud-cards: the shell has no unit card slots");
        }
    }
}

void UiGalleryMode::State::build_movie_page(Control& root, const Vector2 size) {
    const float side = 2.0F * movie_slot_units * size.y / reference_height;
    const Rect2 rect((size - Vector2(side, side)) * 0.5F, Vector2(side, side));
    ColorRect* band = memnew(ColorRect);
    band->set_color(Color(0.55F, 0.58F, 0.62F));
    band->set_position(Vector2(size.x * 0.5F, 0.0F));
    band->set_size(Vector2(size.x * 0.5F, size.y));
    band->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    root.add_child(band);
    const std::u8string cache = options.movie_cache.generic_u8string();
    auto player = attach_hud_movie(root, *movie,
        String::utf8(reinterpret_cast<const char*>(cache.data()), static_cast<int64_t>(cache.size())), rect, true);
    if (!player) {
        failure = core::format_diagnostic(player.error());
        return;
    }
    movie_player = player.value();
    movie_deadline = std::chrono::steady_clock::now() + movie_minimum_time;
}

// Counts loops (the position jumps back) and hashes every tenth frame, so the
// report shows that the stream advances.
void UiGalleryMode::State::sample_movie() {
    const double position = movie_player->get_stream_position();
    if (position < movie_last_position) ++movie_loops;
    movie_last_position = position;
    if (movie_ticks++ % 10 != 0) return;
    const Ref<Texture2D> texture = movie_player->get_video_texture();
    if (texture.is_null()) return;
    const Ref<Image> frame = texture->get_image();
    if (frame.is_null() || frame->is_empty()) return;
    const PackedByteArray bytes = frame->get_data();
    movie_frame_hashes.insert(core::sha256_hex({bytes.ptr(), static_cast<std::size_t>(bytes.size())}));
}

bool UiGalleryMode::State::capture() {
    Ref<Image> image = host->get_viewport()->get_texture()->get_image();
    if (image.is_null() || image->is_empty()) {
        failure = "the viewport could not be read back";
        return false;
    }
    FixedCamera requested;
    requested.width = width;
    requested.height = height;
    CaptureResult read_back;
    read_back.width = static_cast<std::uint32_t>(image->get_width());
    read_back.height = static_cast<std::uint32_t>(image->get_height());
    if (const std::string problem = capture_size_problem(read_back, requested); !problem.empty()) {
        failure = problem;
        return false;
    }
    image->convert(Image::FORMAT_RGBA8);
    const PackedByteArray pixels = image->get_data();
    for (Sample& sample : samples) {
        std::set<std::uint32_t> seen;
        const auto x0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(sample.rect.position.x));
        const auto y0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(sample.rect.position.y));
        const std::int64_t x1 = std::min<std::int64_t>(image->get_width(),
                                                       static_cast<std::int64_t>(sample.rect.get_end().x));
        const std::int64_t y1 = std::min<std::int64_t>(image->get_height(),
                                                       static_cast<std::int64_t>(sample.rect.get_end().y));
        for (std::int64_t y = y0; y < y1 && seen.size() < 4096U; ++y) {
            for (std::int64_t x = x0; x < x1; ++x) {
                const std::int64_t at = (y * image->get_width() + x) * 4;
                seen.insert((static_cast<std::uint32_t>(pixels[at]) << 16U)
                            | (static_cast<std::uint32_t>(pixels[at + 1]) << 8U) | pixels[at + 2]);
            }
        }
        sample.colours = seen.size();
    }
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

bool UiGalleryMode::State::write_report() const {
    if (options.report_path.empty()) return false;
    std::ostringstream output;
    output << "{\n  \"mode\": \"ui_gallery\",\n  \"status\": " << json(status) << ",\n  \"failure\": " << json(failure)
           << ",\n  \"page\": " << json(!options.hud.empty() ? "hud" : options.dialog.empty() ? "controls" : options.dialog)
           << ",\n  \"mod\": "
           << (options.mod_root.empty() ? "false" : "true") << ",\n  \"rules\": "
           << json(options.rules) << ",\n  \"backend\": {\"engine\": " << json(backend.engine)
           << ", \"rendering_method\": " << json(backend.rendering_method) << ", \"adapter_vendor\": "
           << json(backend.adapter_vendor) << ", \"adapter_name\": " << json(backend.adapter_name)
           << ", \"driver_api\": " << json(backend.driver_api) << "},\n  \"viewport\": {\"width\": " << width
           << ", \"height\": " << height << "}";
    if (movie) {
        const Ref<Texture2D> texture = movie_player ? movie_player->get_video_texture() : Ref<Texture2D>();
        output << ",\n  \"movie\": {\"name\": " << json(movie->name)
               << ", \"source\": " << json(movie->source.canonical_path)
               << ", \"source_id\": " << json(movie->source.source_id)
               << ", \"cache_key\": " << json(movie->cache_key)
               << ", \"alpha\": " << (movie->alpha ? "true" : "false")
               << ", \"texture\": [" << (texture.is_valid() ? texture->get_width() : 0) << ", "
               << (texture.is_valid() ? texture->get_height() : 0) << "]"
               << ", \"position\": " << (movie_player ? movie_player->get_stream_position() : 0.0)
               << ", \"distinct_frames\": " << movie_frame_hashes.size()
               << ", \"loops\": " << movie_loops
               << ", \"playing\": " << (movie_player && movie_player->is_playing() ? "true" : "false") << "}";
    }
    if (hud) output << ",\n  \"hud\": " << hud->report_json();
    if (atlas) {
        output << ",\n  \"atlas\": {\"mtd\": " << json(atlas->directory.source.logical_path) << ", \"entries\": "
               << atlas->directory.entries.size() << ", \"page\": " << json(atlas->page.source.logical_path)
               << ", \"width\": " << atlas->page.width << ", \"height\": " << atlas->page.height << ", \"format\": "
               << json(assets::to_string(atlas->page.format)) << "}";
    }
    if (fonts) {
        const model::FontCache& cache = fonts->cache();
        output << ",\n  \"font_cache\": {\"directory\": " << json(cache.directory) << ", \"source\": "
               << json(font_cache_source) << ", \"faces\": [";
        for (std::size_t index = 0; index < cache.faces.size(); ++index) {
            output << (index ? ", " : "") << json(cache.faces[index].face);
        }
        output << "], \"diagnostics\": " << cache.diagnostics.size() << "}";
    }
    if (theme_model) {
        const model::ThemeModel& model_value = *theme_model;
        std::map<std::string, std::size_t> origins;
        for (const model::ThemeTexture& slot : model_value.defaults.textures) ++origins[std::string(model::to_string(slot.origin))];
        std::size_t chained{};
        for (const model::ThemeStyle& style : model_value.variations) chained += style.base != model::theme_type ? 1U : 0U;
        output << ",\n  \"theme\": {\"scale_x\": " << model_value.scale_x << ", \"scale_y\": " << model_value.scale_y
               << ", \"font_screen_height\": " << model_value.font_screen_height << ", \"default_slots\": "
               << model_value.defaults.textures.size() << ", \"origins\": {";
        bool first = true;
        for (const auto& [origin, count] : origins) {
            output << (first ? "" : ", ") << json(origin) << ": " << count;
            first = false;
        }
        output << "}, \"variations\": " << model_value.variations.size() << ", \"chained\": " << chained
               << ", \"unmatched\": " << model_value.unmatched.size() << ",\n    \"built\": {\"styles\": "
               << summary.styles << ", \"icons\": " << summary.icons << ", \"empty_icons\": " << summary.empty_icons
               << ", \"fonts\": " << summary.fonts << ", \"font_variations\": " << summary.font_variations << "}";
        output << ",\n    \"fonts\": [";
        for (std::size_t index = 0; index < model_value.defaults.fonts.size(); ++index) {
            const model::ThemeFont& font = model_value.defaults.fonts[index];
            output << (index ? ",\n      " : "\n      ") << "{\"role\": " << json(data::to_string(font.role))
                   << ", \"face\": " << json(font.face) << ", \"point_size\": " << font.point_size
                   << ", \"resolved\": " << json(font.resolved.face) << ", \"source\": "
                   << json(model::to_string(font.resolved.source)) << ", \"substituted\": "
                   << (font.resolved.substituted ? "true" : "false") << ", \"em_height\": " << font.pixels.em_height
                   << ", \"glyph_height\": " << font.pixels.glyph_height << "}";
        }
        output << "],\n    \"diagnostics\": [";
        for (std::size_t index = 0; index < model_value.diagnostics.size(); ++index) {
            output << (index ? ",\n      " : "\n      ") << json(core::format_diagnostic(model_value.diagnostics[index]));
        }
        output << "]}";
    }
    output << ",\n  \"notes\": [";
    std::vector<std::string> all_notes = notes;
    if (textures) all_notes.insert(all_notes.end(), textures->problems().begin(), textures->problems().end());
    for (std::size_t index = 0; index < all_notes.size(); ++index) {
        output << (index ? ", " : "") << json(all_notes[index]);
    }
    output << "],\n  \"controls\": [";
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const Sample& sample = samples[index];
        output << (index ? ",\n    " : "\n    ") << "{\"kind\": " << json(sample.kind) << ", \"state\": "
               << json(sample.state) << ", \"variation\": " << json(sample.variation) << ", \"rect\": ["
               << sample.rect.position.x << ", " << sample.rect.position.y << ", " << sample.rect.size.x << ", "
               << sample.rect.size.y << "], \"colours\": " << sample.colours << "}";
    }
    output << "]";
    if (dialog) {
        output << ",\n  \"dialog\": {\"name\": " << json(options.dialog) << ", \"frame\": [" << dialog->layout.frame.x
               << ", " << dialog->layout.frame.y << ", " << dialog->layout.frame.width << ", "
               << dialog->layout.frame.height << "], \"gadgets\": [";
        for (std::size_t index = 0; index < dialog->gadgets.size(); ++index) {
            const BuiltGadget& gadget = dialog->gadgets[index];
            output << (index ? ",\n    " : "\n    ") << "{\"id\": " << json(gadget.id) << ", \"statement\": "
                   << json(gadget.statement) << ", \"kind\": " << json(gadget.kind) << ", \"variation\": "
                   << json(gadget.variation) << ", \"caption\": " << json(gadget.caption) << ", \"rect\": ["
                   << gadget.rect.x << ", " << gadget.rect.y << ", " << gadget.rect.width << ", "
                   << gadget.rect.height << "]}";
        }
        output << "]}";
    }
    output << ",\n  \"capture\": {\"png\": "
           << (png ? "{\"width\": " + std::to_string((*png)[0]) + ", \"height\": " + std::to_string((*png)[1]) + "}"
                   : std::string("null"))
           << ", \"sha256\": " << json(capture_sha256) << "}\n}\n";
    std::error_code error;
    if (!options.report_path.parent_path().empty()) {
        std::filesystem::create_directories(options.report_path.parent_path(), error);
    }
    std::ofstream file(options.report_path, std::ios::binary | std::ios::trunc);
    file << output.str();
    return static_cast<bool>(file);
}

bool UiGalleryMode::requested() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (utf8(arguments[index]) == "--eawr-ui-gallery") return true;
    }
    return false;
}

UiGalleryMode::Options UiGalleryMode::from_command_line() {
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
        else if (argument == "--eawr-ui-dialog") options.dialog = value;
        else if (argument == "--eawr-ui-movie") options.movie = value;
        else if (argument == "--eawr-movie-cache") options.movie_cache = ViewerPath{value}.native();
        else if (argument == "--eawr-ui-rules") options.rules = value;
        else if (argument == "--eawr-ui-hud") options.hud = value;
        else if (argument == "--eawr-ui-hud-map") options.hud_map = value;
        else if (argument == "--eawr-ui-hud-cards") options.hud_cards = value;
        else consumed = false;
        if (consumed) ++index;
    }
    return options;
}

UiGalleryMode::UiGalleryMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
UiGalleryMode::~UiGalleryMode() = default;
UiGalleryMode::UiGalleryMode(UiGalleryMode&&) noexcept = default;
UiGalleryMode& UiGalleryMode::operator=(UiGalleryMode&&) noexcept = default;

bool UiGalleryMode::ready(Node3D& host) {
    State& state = *state_;
    state.host = &host;
    RenderingServer* rendering = RenderingServer::get_singleton();
    state.backend = {"Godot 4.7.2-stable", utf8(rendering->get_current_rendering_method()),
                     utf8(rendering->get_video_adapter_vendor()), utf8(rendering->get_video_adapter_name()),
                     utf8(rendering->get_video_adapter_api_version())};
    const auto fail = [&]() {
        state.completed = true;
        static_cast<void>(state.write_report());
        return false;
    };
    if (state.options.rules != "aspect" && state.options.rules != "retail") {
        state.failure = "--eawr-ui-rules must be aspect or retail";
        return fail();
    }
    const Vector2 size = host.get_viewport()->get_visible_rect().size;
    if (size.x < 1.0F || size.y < 1.0F) {
        state.failure = "the viewport has no size";
        return fail();
    }
    state.width = static_cast<std::uint32_t>(size.x);
    state.height = static_cast<std::uint32_t>(size.y);
    pin_capture_viewport(*host.get_window(), state.width, state.height);
    if (!state.load()) return fail();

    CanvasLayer* layer = memnew(CanvasLayer);
    host.add_child(layer);
    auto* root = memnew(Control);
    root->set_position(Vector2());
    root->set_size(size);
    root->set_theme(state.theme);
    root->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    layer->add_child(root);
    ColorRect* fill = memnew(ColorRect);
    fill->set_color(backdrop);
    fill->set_size(size);
    fill->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    root->add_child(fill);
    if (!state.options.hud.empty()) state.build_hud_page(host);
    else if (state.movie) state.build_movie_page(*root, size);
    else if (state.options.dialog.empty()) state.build_controls(*root);
    else state.build_dialog_page(*root);
    if (!state.failure.empty()) return fail();
    return true;
}

std::optional<int> UiGalleryMode::process() {
    State& state = *state_;
    if (state.completed) return std::nullopt;
    if (state.movie_player) state.sample_movie();
    if (state.frames > 0) {
        --state.frames;
        return std::nullopt;
    }
    if (state.movie_player && std::chrono::steady_clock::now() < state.movie_deadline) return std::nullopt;
    state.completed = true;
    if (state.movie_player) {
        const auto decoded = validate_hud_movie(*state.movie_player, *state.movie);
        if (!decoded) state.failure = core::format_diagnostic(decoded.error());
    }
    state.status = state.capture() ? "captured" : "failed";
    const bool written = state.write_report();
    return state.failure.empty() && written ? 0 : 2;
}

} // namespace eawr::presentation::godot_backend
