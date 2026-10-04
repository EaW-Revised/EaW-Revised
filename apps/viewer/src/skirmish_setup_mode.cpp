#include "skirmish_setup_mode.hpp"
#include "battle_content.hpp"
#include "startup_trace.hpp"
#include "viewer_path.hpp"
#include "ui/font_provider.hpp"
#include "ui/kit.hpp"
#include "ui/theme_builder.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/godot/renderer.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <fstream>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace model = presentation::ui;
namespace {
String text(std::string_view value) { return String::utf8(value.data(), static_cast<int64_t>(value.size())); }
std::string utf8(const String& value) { return value.utf8().get_data(); }

void click(Vector2 position) {
    for (bool pressed : {true, false}) {
        Ref<InputEventMouseButton> event;
        event.instantiate();
        event->set_position(position);
        event->set_global_position(position);
        event->set_button_index(MOUSE_BUTTON_LEFT);
        event->set_pressed(pressed);
        Input::get_singleton()->parse_input_event(event);
    }
}

void key(Key code) {
    for (bool pressed : {true, false}) {
        Ref<InputEventKey> event;
        event.instantiate();
        event->set_keycode(code);
        event->set_physical_keycode(code);
        event->set_pressed(pressed);
        Input::get_singleton()->parse_input_event(event);
    }
}

void navigate_popup(EawrUiCombo* combo, int target) {
    const auto focused = combo->get_popup()->get_focused_item();
    if (focused < target) key(KEY_DOWN);
    else if (focused > target) key(KEY_UP);
}
} // namespace

struct SkirmishSetupMode::State final {
    explicit State(Options value) : options(std::move(value)),
        shaders(options.cache_shaders ? std::make_shared<GodotShaderCache>() : nullptr) {}
    Options options;
    Node3D* host{};
    CanvasLayer* layer{};
    Control* root{};
    EawrUiList* list{};
    EawrUiCombo* map_kind{};
    std::vector<Control*> start_icons;
    TextureRect* preview{};
    EawrUiLabel* status{};
    EawrUiButton* start{};
    EawrUiButton *advanced{}, *accept{}, *cancel{}, *defaults_button{};
    ColorRect* options_panel{};
    EawrUiCheck *heroes{}, *starting_units{}, *superweapons{};
    skirmish::MatchOptions defaults;
    std::optional<skirmish::SetupOptionsEdit> editing;
    std::array<EawrUiCombo*, skirmish::local_setup_rows> faction{}, team{}, controller{}, colour{};
    std::array<EawrUiLabel*, skirmish::local_setup_rows> player_name{};
    std::shared_ptr<const vfs::Vfs> filesystem;
    std::shared_ptr<const data::Catalog> catalog;
    std::shared_ptr<const BattleContent> content;
    std::shared_ptr<GodotShaderCache> shaders;
    std::optional<data::ui::DialogCatalog> dialogs;
    std::optional<assets::MegaTextureAtlas> atlas;
    std::unique_ptr<FontProvider> fonts;
    std::unique_ptr<UiTextures> textures;
    std::vector<skirmish::SetupMap> maps;
    std::vector<skirmish::SetupMap> all_maps;
    std::vector<skirmish::LobbyColour> palette;
    bool custom_maps{};
    skirmish::SetupSelection selection;
    std::optional<skirmish::FixtureOptions> pending;
    int selected{-1};
    std::uint32_t frames{};
    std::string benchmark_map;
    bool benchmark{};
    bool three_players{};
    std::string three_player_test_map{"data/art/maps/_mp_space_ryloth.ted"};
    bool two_vs_two{};
    bool policy_test{};
    bool startup_metrics{};
    bool visible{true}, test{}, captured{};
    std::string failure;
    std::string notice;

    void refresh_map(int index);
    void filter_maps();
    void validate();
    void refresh_options();
    void process_options();
    bool load();
    void build();
};

bool SkirmishSetupMode::State::load() {
    const auto base = options.game_root / "GameData" / "Data";
    const auto expansion = options.game_root / "corruption" / "Data";
    std::vector<vfs::LayerRoot> roots;
    if (!std::filesystem::is_directory(base) || !std::filesystem::is_directory(expansion)) {
        failure = "Skirmish setup requires an installation containing GameData/Data and corruption/Data.";
        return false;
    }
    const std::string profile = options.profile.empty() ? (options.mod_root.empty() ? "foc" : "remake") : options.profile;
    if (profile != "foc" && profile != "remake") { failure = "Skirmish setup requires the FoC or mod profile."; return false; }
    if (profile == "remake") {
        if (options.mod_root.empty()) { failure = "The mod profile requires a mod root."; return false; }
        roots = vfs::mod_chain_roots(options.mod_root);
    }
    roots.emplace_back("expansion", expansion);
    roots.emplace_back("base", base);
    auto chain = vfs::resolve_manifest_chain(roots);
    if (!chain) { failure = chain.error().message; return false; }
    std::vector<vfs::MountSpec> specs;
    std::vector<std::string> layers;
    for (auto& manifest : chain.value()) {
        layers.push_back(manifest.mount.layer_id);
        specs.push_back(std::move(manifest.mount));
    }
    auto mounted = vfs::Vfs::mount(specs);
    if (!mounted) { failure = mounted.error().message; return false; }
    filesystem = std::make_shared<const vfs::Vfs>(std::move(mounted).value());
    auto objects = data::load_catalog(*filesystem, profile == "foc" ? data::Profile::foc : data::Profile::remake);
    if (!objects) { failure = objects.error().message; return false; }
    catalog = std::make_shared<const data::Catalog>(std::move(objects.value().catalog));
    content = std::make_shared<const BattleContent>(BattleContent{filesystem, catalog, profile, std::move(layers)});
    auto colours = skirmish::read_lobby_colours(*filesystem);
    if (!colours) { failure = colours.error().message; return false; }
    palette = std::move(colours).value();
    auto match_defaults = skirmish::read_match_defaults(*filesystem);
    if (!match_defaults) { failure = match_defaults.error().message; return false; }
    defaults = std::move(match_defaults).value();
    selection.match = defaults;
    skirmish::SetupMapQuery query;
    query.custom.reset();
    auto found = skirmish::setup_maps(*filesystem, *catalog, query);
    if (!found) { failure = found.error().message; return false; }
    all_maps = std::move(found).value();
    for (const auto& map : all_maps) if (map.metadata.custom == false) maps.push_back(map);
    auto skin = data::ui::load_dialog_catalog(*filesystem);
    if (!skin) { failure = skin.error().message; return false; }
    dialogs.emplace(std::move(skin).value());
    auto page = assets::load_mega_texture_atlas(*filesystem, "data/art/textures/" + dialogs->skin.texture_file + ".mtd");
    if (!page) { failure = page.error().message; return false; }
    atlas.emplace(std::move(page).value());
    textures = std::make_unique<UiTextures>(*atlas, *filesystem);
    if (!textures->page_failure().empty()) { failure = textures->page_failure(); return false; }
    std::filesystem::path cache;
    const auto args = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--eawr-perf-trace") startup_metrics = true;
        if (args[i] == "--eawr-font-cache" && i + 1 < args.size()) cache = ViewerPath{utf8(args[++i])}.native();
        if (args[i] == "--eawr-setup-test") test = true;
        if (args[i] == "--eawr-setup-three-player-test") three_players = true;
        if (args[i] == "--eawr-setup-ffa-map-test" && i + 1 < args.size())
            three_player_test_map = utf8(args[++i]);
        if (args[i] == "--eawr-setup-2v2-test") two_vs_two = true;
        if (args[i] == "--eawr-setup-policy-test") policy_test = true;
        if (args[i] == "--eawr-load-bench-map" && i + 1 < args.size()) {
            benchmark_map = utf8(args[++i]);
            benchmark = true;
        }
    }
    if (cache.empty()) {
        const auto environment = OS::get_singleton()->get_environment("EAWR_FONT_CACHE");
        cache = environment.is_empty()
            ? (ViewerPath{utf8(ProjectSettings::get_singleton()->globalize_path("res://"))}.native()
               / ".." / ".." / ".." / "out" / "fonts").lexically_normal()
            : ViewerPath{utf8(environment)}.native();
    }
    const vfs::MountSpec font_spec{.layer_id = "font-cache", .data_root = cache,
        .loose_logical_prefix = std::string(model::font_cache_prefix), .active_archives = {}};
    auto mounted_fonts = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&font_spec, 1));
    const vfs::Vfs empty;
    fonts = std::make_unique<FontProvider>(model::load_font_cache(mounted_fonts ? mounted_fonts.value() : empty, ViewerPath::utf8(cache)));
    return true;
}

void SkirmishSetupMode::State::build() {
    layer = memnew(CanvasLayer);
    layer->set_layer(10);
    host->add_child(layer);
    auto* background = memnew(ColorRect);
    background->set_color(Color(0.018F, 0.027F, 0.055F));
    background->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    layer->add_child(background);
    root = memnew(Control);
    root->set_size(Vector2(1280, 720));
    background->add_child(root);
    model::ThemeSources sources;
    sources.catalog = &*dialogs;
    sources.atlas = &atlas->directory;
    sources.standalone = model::vfs_standalone_textures(*filesystem);
    sources.fonts = &fonts->cache();
    const auto& families = fonts->system_families();
    sources.system = [&families](std::string_view face) { return model::match_system_face(face, families).has_value(); };
    auto theme_model = model::build_theme_model(sources, model::reference_space({1280, 720}));
    root->set_theme(build_theme(theme_model, *textures, *fonts));
    const auto place = [&](Control* control, float x, float y, float w, float h) {
        root->add_child(control);
        control->set_position(Vector2(x, y));
        control->set_size(Vector2(w, h));
        return control;
    };
    const auto label = [&](std::string_view caption, float x, float y, float w, float h = 28.0F) {
        auto* value = memnew(EawrUiLabel);
        value->set_text(text(caption));
        value->set_role("L_Text");
        value->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        place(value, x, y, w, h);
        return value;
    };
    auto* frame = memnew(EawrUiFrame);
    // R-SETUP-01: primary retail capture geometry at 1280 by 720.
    place(frame, 38, 164, 1204, 392);
    frame->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    auto* title = label("Skirmish Battle Setup", 58, 174, 760, 28);
    title->set_theme_type_variation("EawrUi__IDC_STATIC_MEDIUM");
    title->add_theme_font_size_override("L_Text", 14);
    label("Space", 58, 204, 238, 20);
    label("Select a map", 307, 271, 287, 24);
    map_kind = memnew(EawrUiCombo);
    map_kind->add_item("Official maps");
    map_kind->add_item("Custom maps");
    place(map_kind, 307, 235, 287, 23);
    list = memnew(EawrUiList);
    place(list, 307, 304, 287, 185);
    for (const auto& map : maps) {
        list->add_item(text(map.name + (map.unavailable.empty() ? "" : " (unavailable)")));
        list->set_item_tooltip(list->get_item_count() - 1, text(map.unavailable.empty() ? map.path : map.unavailable));
    }
    preview = memnew(TextureRect);
    preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    preview->set_stretch_mode(TextureRect::STRETCH_SCALE);
    preview->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    place(preview, 60, 233, 234, 176);
    auto* preview_frame = memnew(EawrUiFrame);
    preview_frame->set_small(true);
    preview_frame->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    place(preview_frame, 58, 231, 238, 180);
    // Keep the image above the frame's interior fill while retaining its border.
    root->move_child(preview, -1);
    label("Player", 633, 204, 130, 20);
    label("Faction", 768, 204, 150, 20);
    label("Colour", 926, 204, 95, 20);
    label("Control", 1029, 204, 90, 20);
    label("Team", 1127, 204, 102, 20);
    for (std::size_t slot = 0; slot < skirmish::local_setup_rows; ++slot) {
        const float y = 231.0F + 31.0F * static_cast<float>(slot);
        player_name[slot] = label(slot == 0 ? "You" : "Open", 633, y + 4, 130, 23);
        faction[slot] = memnew(EawrUiCombo);
        faction[slot]->set_focus_mode(Control::FOCUS_ALL);
        for (const char* name : {"Rebel", "Empire", "Underworld"}) faction[slot]->add_item(name);
        faction[slot]->select(slot == 0 ? 0 : 1);
        place(faction[slot], 768, y, 150, 23);
        colour[slot] = memnew(EawrUiCombo);
        colour[slot]->set_focus_mode(Control::FOCUS_ALL);
        for (std::size_t i = 0; i < palette.size(); ++i) {
            const auto& rgb = palette[i].rgb;
            auto swatch = Image::create_empty(12, 12, false, Image::FORMAT_RGBA8);
            swatch->fill(Color(rgb[0] / 255.0F, rgb[1] / 255.0F, rgb[2] / 255.0F));
            colour[slot]->add_icon_item(ImageTexture::create_from_image(swatch), text(palette[i].constant.substr(9)), static_cast<int>(i));
        }
        colour[slot]->select(static_cast<int>(slot));
        place(colour[slot], 926, y, 95, 23);
        team[slot] = memnew(EawrUiCombo);
        team[slot]->set_focus_mode(Control::FOCUS_ALL);
        place(team[slot], 1127, y, 102, 23);
        controller[slot] = memnew(EawrUiCombo);
        if (slot == 0) {
            controller[slot]->add_item("Human");
            controller[slot]->set_disabled(true);
        } else {
            controller[slot]->add_item("Open");
            controller[slot]->add_item("AI");
            controller[slot]->select(slot == 1 ? 1 : 0);
        }
        place(controller[slot], 1029, y, 90, 23);
    }
    label("Choose at least two teams; teammates use one faction.", 633, 490, 596, 23);
    status = label("", 58, 426, 238, 70);
    status->set_wrap(true);
    start = memnew(EawrUiButton);
    start->set_focus_mode(Control::FOCUS_ALL);
    start->set_text("Start Battle");
    place(start, 929, 518, 240, 28);
    advanced = memnew(EawrUiButton);
    advanced->set_text("Advanced options");
    advanced->set_toggle_mode(true);
    place(advanced, 307, 518, 220, 28);
    options_panel = memnew(ColorRect);
    options_panel->set_color(Color(0.01F, 0.015F, 0.025F, 0.9F));
    place(options_panel, 0, 0, 1280, 720);
    const auto option_place = [&](Control* control, float x, float y, float w, float h) {
        options_panel->add_child(control);
        control->set_position(Vector2(x, y));
        control->set_size(Vector2(w, h));
    };
    auto* option_frame = memnew(EawrUiFrame);
    option_frame->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    option_place(option_frame, 380, 210, 520, 300);
    auto* option_title = memnew(EawrUiLabel);
    option_title->set_role("L_Text");
    option_title->set_text("Advanced options");
    option_title->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    option_place(option_title, 410, 229, 460, 28);
    heroes = memnew(EawrUiCheck);
    heroes->set_text("Allow heroes");
    starting_units = memnew(EawrUiCheck);
    starting_units->set_text("Free starting units");
    superweapons = memnew(EawrUiCheck);
    superweapons->set_text("Allow superweapons");
    std::array<EawrUiCheck*, 3> checks{heroes, starting_units, superweapons};
    for (std::size_t i = 0; i < checks.size(); ++i) {
        checks[i]->set_toggle_mode(true);
        option_place(checks[i], 410, 271.0F + 42.0F * static_cast<float>(i), 460, 28);
    }
    accept = memnew(EawrUiButton); accept->set_text("Accept");
    cancel = memnew(EawrUiButton); cancel->set_text("Cancel");
    defaults_button = memnew(EawrUiButton); defaults_button->set_text("Defaults");
    const std::array<EawrUiButton*, 3> buttons{defaults_button, accept, cancel};
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        buttons[i]->set_toggle_mode(true);
        option_place(buttons[i], 410.0F + 156.0F * static_cast<float>(i), 451, 145, 28);
    }
    options_panel->hide();
    int initial = 0;
    for (std::size_t i = 0; i < maps.size(); ++i) if (maps[i].path == skirmish::m2_fixture().map) initial = static_cast<int>(i);
    if (benchmark) {
        for (std::size_t i = 0; i < maps.size(); ++i)
            if (maps[i].path == benchmark_map) initial = static_cast<int>(i);
    }
    list->select(initial);
    list->ensure_current_is_visible();
    refresh_map(initial);
}

void SkirmishSetupMode::State::filter_maps() {
    const auto previous = selection.map;
    maps.clear();
    list->clear();
    for (const auto& map : all_maps) {
        if (map.metadata.custom != custom_maps) continue;
        maps.push_back(map);
        list->add_item(text(map.name + (map.unavailable.empty() ? "" : " (unavailable)")));
        list->set_item_tooltip(list->get_item_count() - 1, text(map.unavailable.empty() ? map.path : map.unavailable));
    }
    selected = -1;
    selection.map.clear();
    preview->set_texture(Ref<ImageTexture>());
    for (auto* icon : start_icons) { icon->hide(); icon->queue_free(); }
    start_icons.clear();
    if (maps.empty()) {
        for (auto* value : team) value->clear();
        for (std::size_t row = 0; row < skirmish::local_setup_rows; ++row) {
            faction[row]->hide(); team[row]->hide(); controller[row]->hide(); colour[row]->hide(); player_name[row]->hide();
        }
        start->set_disabled(true);
        status->set_text("No eligible maps in this list.");
        return;
    }
    int initial = 0;
    for (std::size_t i = 0; i < maps.size(); ++i) if (maps[i].path == previous) initial = static_cast<int>(i);
    list->select(initial);
    list->ensure_current_is_visible();
    refresh_map(initial);
}

void SkirmishSetupMode::State::refresh_map(const int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= maps.size()) return;
    selected = index;
    const auto& map = maps[static_cast<std::size_t>(index)];
    skirmish::repair_setup_selection(selection, map);
    notice.clear();
    const auto rows = skirmish::setup_row_count(map);
    for (std::size_t slot = 0; slot < skirmish::local_setup_rows; ++slot) {
        const bool visible_row = slot < rows;
        for (auto* widget : {static_cast<Control*>(faction[slot]), static_cast<Control*>(team[slot]),
            static_cast<Control*>(controller[slot]), static_cast<Control*>(colour[slot]), static_cast<Control*>(player_name[slot])}) widget->set_visible(visible_row);
        team[slot]->clear();
        for (const auto& side : map.teams) team[slot]->add_item(text("Team " + std::to_string(side.team + 1)), static_cast<int>(side.team));
        const auto record = std::find_if(selection.slots.begin(), selection.slots.end(),
            [&](const auto& entry) { return entry.slot == slot + 1; });
        if (slot != 0) controller[slot]->select(record == selection.slots.end() ? 0 : 1);
        const auto chosen = record == selection.slots.end() ? (slot == 0 ? 0U : 1U) : record->team;
        for (int i = 0; i < team[slot]->get_item_count(); ++i)
            if (team[slot]->get_item_id(i) == static_cast<int>(chosen)) team[slot]->select(i);
        if (record != selection.slots.end()) {
            colour[slot]->select(static_cast<int>(record->colour_index.value_or(record->slot - 1)));
            for (int i = 0; i < faction[slot]->get_item_count(); ++i)
                if (utf8(faction[slot]->get_item_text(i)) == record->faction) faction[slot]->select(i);
        }
    }
    Ref<Image> image;
    if (!map.preview_path.empty()) {
        if (auto texture = assets::load_texture(*filesystem, map.preview_path)) image = texture_image(texture.value(), failure);
    } else if (!map.embedded_preview.empty()) {
        if (auto texture = assets::load_texture(map.embedded_preview, assets::Source{})) image = texture_image(texture.value(), failure);
    }
    if (image.is_null()) {
        if (auto texture = assets::load_texture(*filesystem, "data/art/textures/generic_space.dds")) image = texture_image(texture.value(), failure);
    }
    preview->set_texture(image.is_valid() ? ImageTexture::create_from_image(image) : Ref<ImageTexture>());
    for (auto* icon : start_icons) { icon->hide(); icon->queue_free(); }
    start_icons.clear();
    if (map.metadata.new_markers == true && map.metadata.start_positions) {
        auto decoded = assets::load_map(*filesystem, map.path);
        if (decoded && decoded.value().declared_extents) {
            const auto& extents = *decoded.value().declared_extents;
            if (extents.first > 0 && extents.second > 0) {
                // WSS-10: centred space coordinates; preview Y points down.
                const auto origin = preview->get_position();
                const auto size = preview->get_size();
                for (std::size_t i = 0; i < map.metadata.start_positions->size(); ++i) {
                    const auto& point = map.metadata.start_positions->at(i);
                    const float x = std::clamp(0.5F + point.x / extents.first, 0.0F, 1.0F);
                    const float y = std::clamp(0.5F - point.y / extents.second, 0.0F, 1.0F);
                    auto* backing = memnew(ColorRect);
                    backing->set_color(Color(0.01F, 0.01F, 0.02F, 0.85F));
                    backing->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                    // Keep numbers readable at the preview edges (project inset).
                    backing->set_position(origin + Vector2(size.x * (0.1F + x * 0.8F) - 10,
                        size.y * (0.1F + y * 0.8F) - 10));
                    backing->set_size(Vector2(20, 20));
                    root->add_child(backing);
                    auto* number = memnew(EawrUiLabel);
                    number->set_role("L_Text");
                    number->set_text(text(std::to_string(i + 1)));
                    number->set_alignment(HORIZONTAL_ALIGNMENT_CENTER);
                    number->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                    number->set_size(Vector2(20, 20));
                    backing->add_child(number);
                    start_icons.push_back(backing);
                }
            }
        }
    }
    validate();
}

void SkirmishSetupMode::State::validate() {
    if (selected < 0) { start->set_disabled(true); return; }
    const auto& map = maps[static_cast<std::size_t>(selected)];
    std::vector<skirmish::LobbySlot> occupied;
    std::vector<std::uint32_t> edited_colours;
    for (std::size_t slot = 0; slot < skirmish::setup_row_count(map); ++slot) {
        const bool active = slot == 0 || controller[slot]->get_selected() == 1;
        faction[slot]->set_disabled(!active);
        team[slot]->set_disabled(!active);
        colour[slot]->set_disabled(!active);
        player_name[slot]->set_text(slot == 0 ? "You" : active ? "AI" : "Open");
        if (!active) continue;
        const auto previous = std::find_if(selection.slots.begin(), selection.slots.end(),
            [&](const auto& entry) { return entry.slot == slot + 1; });
        if (previous == selection.slots.end() && slot != 0 && !occupied.empty()) {
            // WSS-15: a newly occupied row starts opposite the host when valid.
            const int opponent = occupied.front().faction == "Rebel" ? 1 : 0;
            faction[slot]->select(opponent);
            for (int i = 0; i < team[slot]->get_item_count(); ++i) {
                if (team[slot]->get_item_id(i) != static_cast<int>(occupied.front().team)) { team[slot]->select(i); break; }
            }
        }
        skirmish::LobbySlot record;
        record.slot = static_cast<std::uint32_t>(slot + 1);
        record.human = slot == 0;
        record.team = team[slot]->get_selected() >= 0 ? static_cast<std::uint32_t>(team[slot]->get_selected_id()) : sim::tactical::max_players;
        record.faction = utf8(faction[slot]->get_item_text(faction[slot]->get_selected()));
        record.colour_index = static_cast<std::uint32_t>(colour[slot]->get_selected_id());
        if (previous == selection.slots.end() || previous->colour_index.value_or(previous->slot - 1) != record.colour_index)
            edited_colours.push_back(record.slot);
        if (previous == selection.slots.end() || previous->team != record.team || previous->faction != record.faction) notice.clear();
        const auto side = std::find_if(map.teams.begin(), map.teams.end(), [&](const auto& t) { return t.team == record.team; });
        for (int i = 0; i < faction[slot]->get_item_count(); ++i) {
            const auto name = utf8(faction[slot]->get_item_text(i));
            const bool allowed = side != map.teams.end() && std::find(side->factions.begin(), side->factions.end(), name) != side->factions.end();
            faction[slot]->set_item_disabled(i, !allowed);
        }
        faction[slot]->set_tooltip_text("Disabled factions have no supported starting station on this map and team.");
        occupied.push_back(std::move(record));
    }
    if (occupied.size() != selection.slots.size()) notice.clear();
    selection.slots = std::move(occupied);
    for (const auto row : edited_colours) {
        const auto choice = static_cast<std::uint32_t>(colour[row - 1]->get_selected_id());
        const auto repaired = skirmish::select_setup_colour(selection, row, choice, palette.size());
        if (!repaired) { start->set_disabled(true); status->set_text(text(repaired.error().message)); return; }
    }
    for (const auto& record : selection.slots) colour[record.slot - 1]->select(static_cast<int>(*record.colour_index));
    for (std::size_t i = 0; i < start_icons.size(); ++i) {
        auto* icon = static_cast<ColorRect*>(start_icons[i]);
        icon->set_color(Color(0.01F, 0.01F, 0.02F, 0.85F));
        for (const auto& record : selection.slots) {
            if (record.team != i || *record.colour_index >= palette.size()) continue;
            const auto& rgb = palette[*record.colour_index].rgb;
            icon->set_color(Color(rgb[0] / 510.0F, rgb[1] / 510.0F, rgb[2] / 510.0F, 0.95F));
            break;
        }
    }
    const auto options_result = skirmish::setup_options(selection, maps);
    start->set_disabled(!options_result || editing.has_value());
    status->set_text(options_result ? text(notice.empty() ? map.name : notice) : text(options_result.error().message));
}

void SkirmishSetupMode::State::refresh_options() {
    if (!editing) return;
    heroes->set_pressed_no_signal(editing->value.allow_heroes);
    starting_units->set_pressed_no_signal(editing->value.free_starting_units);
    superweapons->set_pressed_no_signal(editing->value.allow_superweapons);
}

void SkirmishSetupMode::State::process_options() {
    if (advanced->is_pressed()) {
        advanced->set_pressed_no_signal(false);
        editing = skirmish::begin_setup_options(selection, defaults);
        refresh_options();
        root->move_child(options_panel, -1);
        options_panel->show();
    }
    if (!editing) return;
    if (heroes->is_pressed() != editing->value.allow_heroes
        || starting_units->is_pressed() != editing->value.free_starting_units
        || superweapons->is_pressed() != editing->value.allow_superweapons) editing->custom = true;
    editing->value.allow_heroes = heroes->is_pressed();
    editing->value.free_starting_units = starting_units->is_pressed();
    editing->value.allow_superweapons = superweapons->is_pressed();
    if (defaults_button->is_pressed()) {
        defaults_button->set_pressed_no_signal(false);
        skirmish::reset_setup_options(*editing, defaults);
        refresh_options();
    }
    if (accept->is_pressed()) {
        accept->set_pressed_no_signal(false);
        skirmish::accept_setup_options(selection, *editing);
        editing.reset();
        options_panel->hide();
    } else if (cancel->is_pressed() || Input::get_singleton()->is_physical_key_pressed(KEY_ESCAPE)) {
        cancel->set_pressed_no_signal(false);
        editing.reset();
        options_panel->hide();
    }
}

SkirmishSetupMode::SkirmishSetupMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
SkirmishSetupMode::~SkirmishSetupMode() = default;
bool SkirmishSetupMode::ready(Node3D& host) {
    state_->host = &host;
    if (!state_->load()) { UtilityFunctions::printerr(text(state_->failure)); return false; }
    if (state_->test) host.get_viewport()->set_embedding_subwindows(true);
    state_->build();
    UtilityFunctions::print("EAWR skirmish setup ready");
    return true;
}

void SkirmishSetupMode::process() {
    auto& state = *state_;
    if (!state.visible) return;
    const auto size = state.host->get_viewport()->get_visible_rect().size;
    const float scale = std::min(size.x / 1280.0F, size.y / 720.0F);
    state.root->set_scale(Vector2(scale, scale));
    state.root->set_position((size - Vector2(1280, 720) * scale) * 0.5F);
    if ((state.map_kind->get_selected() == 1) != state.custom_maps) {
        state.custom_maps = state.map_kind->get_selected() == 1;
        state.filter_maps();
    }
    const auto selected = state.list->get_selected_items();
    if (!selected.is_empty() && selected[0] != state.selected) state.refresh_map(selected[0]);
    state.process_options();
    state.validate();
    ++state.frames;
    if (!state.captured && state.frames >= (state.two_vs_two && state.test ? 94U : state.policy_test ? 114U : state.three_players ? 98U : state.test ? 78U : 30U) && !state.options.capture_path.empty()) {
        std::filesystem::create_directories(state.options.capture_path.parent_path());
        auto image = state.host->get_viewport()->get_texture()->get_image();
        state.captured = image.is_valid() && image->save_png(text(ViewerPath::utf8(state.options.capture_path))) == OK;
    }
    // GPU contract hook: real GUI pointer/key events, then the ordinary Start handler.
    if (state.test) {
        if (state.two_vs_two) {
            // Exercise the same controls as a local player; no fixture injection.
            if (state.frames >= 32 && state.frames <= 48) {
                state.list->grab_focus();
                for (std::size_t i = 0; i < state.maps.size(); ++i) {
                    if (state.maps[i].path.ends_with("_mp_space_coruscant.ted") && state.selected != static_cast<int>(i))
                        key(state.selected < static_cast<int>(i) ? KEY_DOWN : KEY_UP);
                }
            } else if (state.frames == 50) click(state.controller[2]->get_global_rect().get_center());
            else if (state.frames >= 52 && state.frames <= 54) navigate_popup(state.controller[2], 1);
            else if (state.frames == 56 || state.frames == 64 || state.frames == 72 || state.frames == 80) key(KEY_ENTER);
            else if (state.frames == 58) click(state.team[2]->get_global_rect().get_center());
            else if (state.frames >= 60 && state.frames <= 62) navigate_popup(state.team[2], 0);
            else if (state.frames == 66) click(state.faction[2]->get_global_rect().get_center());
            else if (state.frames >= 68 && state.frames <= 70) navigate_popup(state.faction[2], 0);
            else if (state.frames == 74) click(state.controller[3]->get_global_rect().get_center());
            else if (state.frames >= 76 && state.frames <= 78) navigate_popup(state.controller[3], 1);
            else if (state.frames == 96) click(state.start->get_global_rect().get_center());
            return;
        }
        if (state.policy_test && state.frames >= 80) {
            if (state.frames == 99 && !state.options.capture_path.empty()) {
                auto image = state.host->get_viewport()->get_texture()->get_image();
                const auto destination = state.options.capture_path.parent_path()
                    / (state.options.capture_path.stem().string() + ".options.png");
                if (image.is_valid()) image->save_png(text(ViewerPath::utf8(destination)));
            }
            if (state.frames == 80 || state.frames == 92) click(state.advanced->get_global_rect().get_center());
            else if (state.frames == 82 || state.frames == 94 || state.frames == 104) click(state.heroes->get_global_rect().get_center());
            else if (state.frames == 84 || state.frames == 96 || state.frames == 106) click(state.starting_units->get_global_rect().get_center());
            else if (state.frames == 86 || state.frames == 98 || state.frames == 108) click(state.superweapons->get_global_rect().get_center());
            else if (state.frames == 88) click(state.cancel->get_global_rect().get_center());
            else if (state.frames == 100) click(state.defaults_button->get_global_rect().get_center());
            else if (state.frames == 110) click(state.accept->get_global_rect().get_center());
            else if (state.frames == 90 || state.frames == 102 || state.frames == 112) {
                const auto current = state.editing ? state.editing->value : *state.selection.match;
                UtilityFunctions::print("EAWR setup policy frame ", state.frames, ": ", current.allow_heroes,
                    ",", current.free_starting_units, ",", current.allow_superweapons, "; custom: ",
                    state.editing ? state.editing->custom : state.selection.custom_options);
            } else if (state.frames == 116) click(state.start->get_global_rect().get_center());
            return;
        }
        if (state.benchmark) {
            if (state.frames == 60) click(state.start->get_global_rect().get_center());
        } else if (state.frames == (state.three_players ? 82U : 62U)) {
            click(state.colour[0]->get_global_rect().get_center());
        } else if (state.frames >= (state.three_players ? 84U : 64U)
            && state.frames <= (state.three_players ? 88U : 68U)) navigate_popup(state.colour[0], 4);
        else if (state.frames >= (state.three_players ? 92U : 72U)
            && state.frames <= (state.three_players ? 95U : 75U)) navigate_popup(state.colour[1], 3);
        else if (state.frames == (state.three_players ? 89U : 69U)
            || state.frames == (state.three_players ? 96U : 76U)) key(KEY_ENTER);
        else if (state.frames == (state.three_players ? 90U : 70U)) {
            click(state.colour[1]->get_global_rect().get_center());
        } else if (state.frames == 8 || state.frames == 20) {
            click(state.map_kind->get_global_rect().get_center());
        } else if (state.frames == 10 || state.frames == 11) key(KEY_DOWN);
        else if (state.frames == 22 || state.frames == 23) {
            const auto focused = state.map_kind->get_popup()->get_focused_item();
            UtilityFunctions::print("EAWR setup map-kind popup focus: ", focused);
            if (focused < 0) key(KEY_DOWN);
            else if (focused != 0) key(KEY_UP);
        }
        else if (state.frames == 12 || state.frames == 24) key(KEY_ENTER);
        else if (state.frames == 16) {
            UtilityFunctions::print("EAWR setup custom map list: ", state.custom_maps,
                "; empty: ", state.maps.empty(), "; start disabled: ", state.start->is_disabled());
        } else if (state.frames == 28) {
            UtilityFunctions::print("EAWR setup official map list: ", !state.custom_maps,
                "; maps: ", static_cast<int64_t>(state.maps.size()),
                "; preview starts: ", static_cast<int64_t>(state.start_icons.size()));
        } else if (state.frames >= 32 && state.frames <= 48) {
            state.list->grab_focus();
            for (std::size_t i = 0; i < state.maps.size(); ++i) if (state.three_players
                ? state.maps[i].path == state.three_player_test_map : state.maps[i].path.ends_with("_mp_space_kessel.ted")) {
                const auto target = static_cast<int>(i);
                const auto distance = state.selected < target ? target - state.selected : state.selected - target;
                for (int step = 0; step < std::min(distance, 3); ++step)
                    key(state.selected < target ? KEY_DOWN : KEY_UP);
            }
        } else if (state.frames == 50) {
            click(state.faction[0]->get_global_rect().get_center());
        } else if (state.frames == 52 || state.frames == 53) key(KEY_DOWN);
        else if (state.frames == 54) key(KEY_ENTER);
        else if (!state.three_players && state.frames == 80) click(state.start->get_global_rect().get_center());
        else if (state.three_players) {
            // Two-player maps hide the third row; its absent popup must not
            // send navigation keys into the still-focused map list.
            if (!state.controller[2]->is_visible_in_tree() && state.frames != 98) return;
            if (state.frames == 62) click(state.controller[2]->get_global_rect().get_center());
            else if (state.frames == 64 || state.frames == 65) navigate_popup(state.controller[2], 1);
            else if (state.frames == 66 || state.frames == 74 || state.frames == 80) key(KEY_ENTER);
            else if (state.frames == 68) click(state.team[2]->get_global_rect().get_center());
            else if (state.frames >= 70 && state.frames <= 72) navigate_popup(state.team[2], 2);
            else if (state.frames == 78 || state.frames == 79) navigate_popup(state.faction[2], 1);
            else if (state.frames == 76) click(state.faction[2]->get_global_rect().get_center());
            else if (state.frames == 98) {
                if (state.selected >= 0 && state.selected < static_cast<int>(state.maps.size()))
                    UtilityFunctions::print("EAWR FFA setup map: ", text(state.maps[static_cast<std::size_t>(state.selected)].path));
                UtilityFunctions::print("EAWR FFA setup third row: ", state.controller[2]->is_visible_in_tree());
                UtilityFunctions::print("EAWR FFA setup teams: ", state.team[2]->get_item_count());
                // Stock capacity does not imply another team start. Exercise the real
                // selectors, save their roster, and finish this test without a battle.
                if (state.team[2]->get_item_count() < 3) state.host->get_tree()->quit();
            } else if (state.frames == 100) click(state.start->get_global_rect().get_center());
        }
    }
}

void SkirmishSetupMode::request_start() {
    if (state_->editing) return;
    const auto pressed = StartupTrace::Clock::now();
    state_->validate();
    auto options = skirmish::setup_options(state_->selection, state_->maps);
    if (options) {
        if (!state_->pending && (!state_->options.report_path.empty() || state_->startup_metrics))
            startup_trace.begin(true, pressed);
        state_->pending = std::move(options).value();
    }
}
std::optional<skirmish::FixtureOptions> SkirmishSetupMode::take_start() { return std::exchange(state_->pending, std::nullopt); }
std::shared_ptr<const BattleContent> SkirmishSetupMode::content() const { return state_->content; }
std::shared_ptr<GodotShaderCache> SkirmishSetupMode::shaders() const { return state_->shaders; }
BaseButton* SkirmishSetupMode::start_button() const { return state_->start; }
void SkirmishSetupMode::hide() { state_->visible = false; state_->layer->hide(); }
void SkirmishSetupMode::show(std::string message) {
    state_->visible = true;
    state_->layer->show();
    state_->notice = std::move(message);
    state_->validate();
    state_->test = state_->benchmark;
    state_->frames = 0;
    if (!state_->options.capture_path.empty()) {
        state_->options.capture_path = state_->options.capture_path.parent_path()
            / (state_->options.capture_path.stem().string() + ".returned.png");
        state_->captured = false;
    }
    UtilityFunctions::print("EAWR skirmish setup returned");
}

} // namespace eawr::presentation::godot_backend
