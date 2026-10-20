#include "eawr/core/json_text.hpp"
#include "eawr/core/text.hpp"
#include "loading_screen.hpp"
#include "loading_planet.hpp"

#include "viewer_path.hpp"
#include "ui/dialog_builder.hpp"
#include "ui/font_provider.hpp"
#include "ui/kit.hpp"
#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/theme.hpp"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace model = presentation::ui;

namespace {

// The retail loop redraws about twenty times a second (BLD-10); a draw costs one frame of the 2D layer.
constexpr std::chrono::milliseconds present_interval{50};
// Above the HUD and its battle overlay (layer 1); under the performance overlay and the capture
// pointer (100), which stay readable over the screen.
constexpr int screen_layer = 98;
// A draw on the spot needs the screen's Controls drawn once by a normal frame (see the header).
constexpr std::uint64_t present_after_frames = 2;
// The bar's own pieces draw the empty bar and its caps; the fill is painted over them (BLD-10).
constexpr const char* bar_gadget = "IDC_PROGRESS_SOLO";
constexpr const char* begin_gadget = "IDC_BEGIN_BUTTON";

[[nodiscard]] String text(const std::string& value) {
    return String::utf8(value.c_str(), static_cast<int64_t>(value.size()));
}

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}

[[nodiscard]] std::string json(const std::string& value) {
    return eawr::core::json_quote_drop_controls(value);
}

[[nodiscard]] bool starts_with(const std::string& value, const std::string_view prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}

[[nodiscard]] std::string_view loading_owner_text_key(const std::string_view raw) {
    const auto trimmed = core::trim_view(raw, core::xml_whitespace);
    return trimmed.empty() ? std::string_view{} : trimmed;
}

[[nodiscard]] std::filesystem::path default_font_cache(const std::filesystem::path& given) {
    if (!given.empty()) return given;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == String("--eawr-font-cache")) return ViewerPath{utf8(arguments[index + 1])}.native();
    }
    if (const String environment = OS::get_singleton()->get_environment("EAWR_FONT_CACHE"); !environment.is_empty()) {
        return ViewerPath{utf8(environment)}.native();
    }
    const std::filesystem::path project =
        ViewerPath{utf8(ProjectSettings::get_singleton()->globalize_path("res://"))}.native();
    return (project / ".." / ".." / ".." / "out" / "fonts").lexically_normal();
}

// A texture file by the name the data write it: Data/Art/Textures/<stem>.tga, then .dds (the install
// ships its pictures as DDS under the .tga names the XML uses).
[[nodiscard]] Ref<ImageTexture> load_named(const vfs::Vfs& filesystem, const std::string& name, std::string& failure) {
    if (name.empty()) return {};
    const auto dot = name.find_last_of('.');
    const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    std::vector<std::string> candidates;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) candidates.push_back(name);
    for (const char* extension : {".tga", ".dds"}) candidates.push_back("data/art/textures/" + stem + extension);
    for (const std::string& path : candidates) {
        auto loaded = assets::load_texture(filesystem, path);
        if (!loaded) continue;
        const Ref<Image> image = texture_image(loaded.value(), failure);
        if (image.is_valid()) return ImageTexture::create_from_image(image);
    }
    failure += (failure.empty() ? "" : "; ") + std::string("texture ") + name + " is not in the data";
    return {};
}

} // namespace

struct EawrLoadingScreen::Resources final {
    Options options;
    std::shared_ptr<const vfs::Vfs> filesystem;
    std::optional<data::ui::DialogCatalog> dialogs;
    std::optional<assets::MegaTextureAtlas> atlas;
    std::unique_ptr<UiTextures> textures;
    std::unique_ptr<FontProvider> fonts;
    std::optional<data::ui::TextDatabase> text_database;
    std::optional<data::ui::TextLookup> text;
    std::optional<model::LoadingScreenContent> content;
    std::vector<std::string> history_lines;
    std::string title;
};

struct EawrLoadingScreen::Layout final {
    Control* root{};
    EawrUiFrame* frame{};
    EawrUiBar* bar{};
    EawrUiButton* begin{};
    EawrUiList* history{};
    EawrUiList* tactical{};
    EawrUiList* advantage{};
    EawrUiList* mission{};
    Vector2 size;
    std::vector<std::string> images; // the picture gadgets that showed a texture
};

EawrLoadingScreen::EawrLoadingScreen() {
    set_layer(screen_layer);
    set_process(true);
    set_process_input(true);
}

bool EawrLoadingScreen::topmost() const noexcept {
    return shown_ && is_visible() && (!resources_->options.topmost || resources_->options.topmost());
}

void EawrLoadingScreen::_input(const Ref<InputEvent>& event) {
    if (!resources_ || !shown_ || event.is_null()) return;
    const auto* key = Object::cast_to<InputEventKey>(event.ptr());
    const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (!key && !button) return; // WGP-113: mouse movement has a separate gate.
    if (draining_) {
        // BLD-15: a key or click that waited in the window queue through the load.
        ++drained_inputs_; get_viewport()->set_input_as_handled(); return;
    }
    if (!resources_->options.campaign_space) return;
    if (campaign_gate_.unsafe()) {
        ++unsafe_inputs_; get_viewport()->set_input_as_handled(); return;
    }
    if (key && topmost() && key->is_pressed() && !key->is_echo()
        && (key->get_keycode() == KEY_ENTER || key->get_keycode() == KEY_KP_ENTER
            || key->get_keycode() == KEY_SPACE || key->get_keycode() == KEY_ESCAPE)) {
        request_begin(); get_viewport()->set_input_as_handled();
    }
}

EawrLoadingScreen::~EawrLoadingScreen() {
    planet_.reset();
    if (auto* rendering = RenderingServer::get_singleton(); rendering != nullptr && fill_item_.is_valid()) {
        rendering->free_rid(fill_item_);
    }
}

void EawrLoadingScreen::_notification(const int what) {
    // The screen can leave the tree with the 3D view still off (a failed load): never leave it off.
    if (what == NOTIFICATION_EXIT_TREE) { if (planet_) planet_->release(); set_3d(true); }
    if (what != NOTIFICATION_PROCESS) return;
    ++frames_;
    if (planet_ && shown_) planet_->advance(std::chrono::duration<double>(std::chrono::steady_clock::now() - planet_started_).count());
    if (planet_) set_meta("loading_planet", text(planet_->report_json()));
    // A load that ended before the screen could draw on the spot takes its ready still once the
    // frames after it have drawn the finished screen.
    if (capture_ready_pending_ && shown_ && frames_ >= ready_frame_ + present_after_frames) {
        capture_ready_pending_ = false;
        capture("ready");
    }
}

void EawrLoadingScreen::set_3d(const bool enabled) {
    if (enabled == !disabled_3d_) return;
    if (Viewport* viewport = get_viewport()) viewport->set_disable_3d(!enabled);
    disabled_3d_ = !enabled;
}

bool EawrLoadingScreen::build(Node& host, Options options) {
    resources_ = std::make_unique<Resources>();
    Resources& resources = *resources_;
    resources.options = std::move(options);
    if (!resources.options.content || !resources.options.content->filesystem) {
        failure_ = "the loading screen needs the installed content";
        return false;
    }
    resources.filesystem = resources.options.content->filesystem;
    const vfs::Vfs& filesystem = *resources.filesystem;
    auto dialogs = data::ui::load_dialog_catalog(filesystem);
    if (!dialogs) { failure_ = core::format_diagnostic(dialogs.error()); return false; }
    resources.dialogs.emplace(std::move(dialogs).value());
    auto atlas = assets::load_mega_texture_atlas(filesystem,
        "data/art/textures/" + resources.dialogs->skin.texture_file + ".mtd");
    if (!atlas) { failure_ = core::format_diagnostic(atlas.error()); return false; }
    resources.atlas.emplace(std::move(atlas).value());
    resources.textures = std::make_unique<UiTextures>(*resources.atlas, filesystem);
    if (!resources.textures->page_failure().empty()) { failure_ = resources.textures->page_failure(); return false; }
    const std::filesystem::path cache = default_font_cache(resources.options.font_cache);
    const vfs::MountSpec font_spec{.layer_id = "font-cache", .data_root = cache,
        .loose_logical_prefix = std::string(model::font_cache_prefix), .active_archives = {}};
    auto font_mount = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&font_spec, 1));
    const vfs::Vfs unmounted;
    resources.fonts = std::make_unique<FontProvider>(
        model::load_font_cache(font_mount ? font_mount.value() : unmounted, ViewerPath::utf8(cache)));
    if (auto loaded = data::ui::load_language_text_database(filesystem, "ENGLISH")) {
        resources.text_database.emplace(std::move(loaded).value());
        resources.text.emplace(*resources.text_database);
    } else {
        notes_.push_back("text database: " + core::format_diagnostic(loaded.error()) + "; captions show their keys");
    }

    model::LoadingScreenInput input;
    input.map_path = resources.options.map_path;
    input.context_name = resources.options.context_name;
    input.map_name = resources.options.map_name;
    input.objects = resources.options.content->catalog.get();
    input.text = resources.text ? &*resources.text : nullptr;
    input.exists = [&filesystem](const std::string_view path) { return static_cast<bool>(filesystem.stat(path)); };
    input.campaign = resources.options.campaign_space;
    input.battle = resources.options.battle;
    input.mission = resources.options.mission;
    input.mission_map = resources.options.mission_map;
    resources.content.emplace(model::loading_screen_content(input));
    for (const core::Diagnostic& diagnostic : resources.content->diagnostics) notes_.push_back(core::format_diagnostic(diagnostic));

    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == String("--eawr-loading-capture")) capture_directory_ = ViewerPath{utf8(arguments[index + 1])}.native();
    }

    host.add_child(this);
    const Vector2 size = get_viewport()->get_visible_rect().size;
    if (!build_layout(size)) {
        host.remove_child(this);
        return false;
    }
    shown_ = true;
    observe_preparation(resources.options.campaign_preparation);
    set_meta("loading_ready", false);
    set_meta("loading_drained_inputs", static_cast<int64_t>(0));
    set_3d(false);
    return true;
}

bool EawrLoadingScreen::build_layout(const Vector2 size) {
    Resources& resources = *resources_;
    const auto* dialog = resources.dialogs->script.find(resources.content->dialog);
    if (dialog == nullptr) {
        failure_ = resources.content->dialog + " is not in the dialog resources";
        return false;
    }
    if (layout_ && layout_->root != nullptr) {
        planet_.reset();
        if (auto* rendering = RenderingServer::get_singleton(); rendering != nullptr && fill_item_.is_valid()) {
            rendering->free_rid(fill_item_);
        }
        fill_item_ = RID();
        layout_->root->queue_free();
    }
    layout_ = std::make_unique<Layout>();
    Layout& layout = *layout_;
    layout.size = size;

    model::ThemeSources sources;
    sources.catalog = &*resources.dialogs;
    sources.atlas = &resources.atlas->directory;
    sources.standalone = model::vfs_standalone_textures(*resources.filesystem);
    sources.fonts = &resources.fonts->cache();
    const auto& families = resources.fonts->system_families();
    sources.system = [&families](const std::string_view face) { return model::match_system_face(face, families).has_value(); };
    const auto space = model::reference_space({static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y)},
                                              model::LayoutRules::retail);
    const auto theme_model = model::build_theme_model(sources, space);

    layout.root = memnew(Control);
    layout.root->set_name("EawrLoadingScreen");
    layout.root->set_position(Vector2());
    layout.root->set_size(size);
    // The screen stops the pointer: nothing reaches the battle behind it (BLD-12).
    layout.root->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    add_child(layout.root);
    auto* backdrop = memnew(ColorRect);
    backdrop->set_color(Color(0, 0, 0, 1));
    backdrop->set_size(size);
    backdrop->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    layout.root->add_child(backdrop);

    BuiltDialog built = build_dialog(*dialog, theme_model, resources.text ? &*resources.text : nullptr,
                                     model::Placement::full_screen);
    built.frame->set_theme(build_theme(theme_model, *resources.textures, *resources.fonts));
    layout.frame = built.frame;
    layout.root->add_child(built.frame);
    const model::PixelRect& frame_rect = built.layout.frame;
    const Vector2 origin(static_cast<float>(frame_rect.x), static_cast<float>(frame_rect.y));

    const model::LoadingScreenContent& content = *resources.content;
    std::string owner_name = resources.options.owner_name;
    if (resources.options.content->catalog && resources.text && !owner_name.empty()) {
        auto owner = resources.options.content->catalog->resolve(owner_name, data::Category::faction);
        if (owner) if (const auto* key = owner.value().value("Text_ID")) {
            owner_name = data::ui::to_utf8(resources.text->text(loading_owner_text_key(key->value.raw_text)));
        }
    }
    bool owner_caption{}, income_caption{};
    Control* title_control{};
    std::size_t child = 0;
    const auto place_picture = [&](const BuiltGadget& gadget, const std::string& file, const std::string& direct_path,
                                   const bool framed) {
        std::string why;
        const Ref<ImageTexture> texture = direct_path.empty() ? load_named(*resources.filesystem, file, why)
                                                                : load_named(*resources.filesystem, direct_path, why);
        if (texture.is_null()) {
            if (!why.empty()) notes_.push_back(gadget.id + ": " + why);
            return;
        }
        const Vector2 position(static_cast<float>(gadget.rect.x) - origin.x, static_cast<float>(gadget.rect.y) - origin.y);
        const Vector2 extent(static_cast<float>(gadget.rect.width), static_cast<float>(gadget.rect.height));
        if (framed) {
            // The retail frames a map picture (Image_Enable_Frames): the small frame's rim round it.
            auto* rim = memnew(EawrUiFrame);
            rim->set_small(true);
            rim->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            rim->set_position(position - Vector2(2, 2));
            rim->set_size(extent + Vector2(4, 4));
            layout.frame->add_child(rim);
            layout.frame->move_child(rim, static_cast<int32_t>(child++));
        }
        auto* picture = memnew(TextureRect);
        picture->set_name(text(gadget.id));
        picture->set_texture(texture);
        picture->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        picture->set_stretch_mode(TextureRect::STRETCH_SCALE);
        picture->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        picture->set_position(position);
        picture->set_size(extent);
        layout.frame->add_child(picture);
        layout.frame->move_child(picture, static_cast<int32_t>(child++));
        layout.images.push_back(gadget.id);
    };

    for (BuiltGadget& gadget : built.gadgets) {
        // The gadgets are the frame's children in dialog order; a picture gadget has no Control yet.
        if (gadget.control != nullptr) {
            ++child;
            const bool hidden = (starts_with(gadget.id, "IDC_PROGRESS") && gadget.id != bar_gadget)
                || starts_with(gadget.id, "IDC_PLAYER_NAME_") || gadget.id == "IDC_DIALOG_CLOSE_BUTTON"
                || (!resources.options.campaign_space && (gadget.id == "IDC_STATIC_FACTION"
                || gadget.id == "IDC_STATIC_CREDIT" || gadget.id == "IDC_CONTROLLING_FACTION_TEXT"
                || gadget.id == "IDC_CREDIT_PRODUCTION_TEXT"));
            if (hidden) gadget.control->set_visible(false);
            if (resources.options.campaign_space && (gadget.id == "IDC_STATIC_PLANET_NAME" || gadget.id == "IDC_STATIC_FACTION")) {
                // WGP-31: actual owner's authored Display_Font_Color, white when absent.
                const auto& colour = resources.options.owner_colour;
                const Color native(colour[0], colour[1], colour[2]);
                const String role = gadget.statement == "CTEXT" ? "C_Text" : gadget.statement == "RTEXT" ? "R_Text" : "L_Text";
                gadget.control->add_theme_color_override(role + String("_top"), native);
                gadget.control->add_theme_color_override(role + String("_bottom"), native);
            }
            const auto fill = [&](const std::string& value) {
                if (auto* label = Object::cast_to<EawrUiLabel>(gadget.control)) label->set_text(text(value));
            };
            if (gadget.id == "IDC_STATIC_PLANET_NAME") { fill(content.title); title_control = gadget.control; }
            else if (gadget.id == "IDC_STATIC_FACTION") {
                fill(owner_name); owner_caption = true;
            }
            else if (gadget.id == "IDC_STATIC_CREDIT") {
                fill(resources.options.basic_income ? std::to_string(static_cast<std::int64_t>(std::llround(*resources.options.basic_income))) : std::string()); income_caption = true;
            }
            else if (gadget.id == "IDC_MISSION_STATIC") {
                fill(content.mission_heading);
                gadget.control->add_theme_color_override("L_Text_top", Color(1, 1, 1));
                gadget.control->add_theme_color_override("L_Text_bottom", Color(1, 1, 1));
            }
            else if (gadget.id == "IDC_MISSION_LIST") {
                layout.mission = Object::cast_to<EawrUiList>(gadget.control);
                if (layout.mission) layout.mission->add_theme_color_override("font_color", Color(1, 1, 1));
            }
            else if (gadget.id == "IDC_STATIC_WEATHER") fill(content.weather);
            else if (gadget.id == "IDC_STATIC_TERRAIN") fill(content.terrain);
            else if (gadget.id == "IDC_STATIC_NATIVE") fill(content.population);
            else if (gadget.id == bar_gadget) layout.bar = Object::cast_to<EawrUiBar>(gadget.control);
            else if (gadget.id == begin_gadget) layout.begin = Object::cast_to<EawrUiButton>(gadget.control);
            else if (gadget.id == "IDC_LIST_REFERENCE") layout.history = Object::cast_to<EawrUiList>(gadget.control);
            else if (gadget.id == "IDC_LIST_TACTICAL") layout.tactical = Object::cast_to<EawrUiList>(gadget.control);
            else if (gadget.id == "IDC_LIST_ADVANTAGE") layout.advantage = Object::cast_to<EawrUiList>(gadget.control);
            continue;
        }
        if (gadget.id == "IDC_PLANET_ICON" && !content.planet.empty()) {
            planet_ = std::make_unique<LoadingPlanet>();
            planet_started_ = std::chrono::steady_clock::now();
            const Rect2 rect(static_cast<float>(gadget.rect.x) - origin.x, static_cast<float>(gadget.rect.y) - origin.y,
                static_cast<float>(gadget.rect.width), static_cast<float>(gadget.rect.height));
            const auto view = resources.options.campaign_preparation;
            if (planet_->build(*layout.frame, rect, *resources.options.content, content.planet, view ? &view->descriptor : nullptr,
                    resources.options.planet_model, resources.options.planet_transform)) {
                auto* picture = layout.frame->find_child("LoadingPlanetImage", false, false);
                layout.frame->move_child(picture, static_cast<int32_t>(child++));
            } else notes_.push_back("loading planet: " + planet_->report_json());
        }
        else if (gadget.id == "IDC_PLANET_OVERLAY") place_picture(gadget, content.overlay_icon, {}, false);
        else if (gadget.id == "IDC_WEATHER_ICON") place_picture(gadget, content.weather_icon, {}, false);
        else if (gadget.id == "IDC_NATIVE_ICON") place_picture(gadget, content.native_icon, {}, false);
        else if (gadget.id == "IDC_MAP_ICON" && !content.map_icon.empty()) place_picture(gadget, {}, content.map_icon, true);
    }

    if (resources.options.campaign_space && title_control) {
        // Stock space RC lacks these named captions. Keep the sourced values
        // visible in the free area below its title using explicit project placement.
        const auto caption = [&](const char* name, const std::string& value, const float row, const bool owner) {
            auto* label = memnew(EawrUiLabel); layout.frame->add_child(label);
            label->set_name(name); label->set_role("L_Text"); label->set_theme(layout.frame->get_theme());
            label->set_text(text(value)); label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            label->set_position(title_control->get_position() + Vector2(0, title_control->get_size().y * row));
            label->set_size(title_control->get_size());
            if (owner) {
                const auto& colour = resources.options.owner_colour;
                label->add_theme_color_override("L_Text_top", Color(colour[0], colour[1], colour[2]));
                label->add_theme_color_override("L_Text_bottom", Color(colour[0], colour[1], colour[2]));
            }
        };
        if (!owner_caption && !owner_name.empty()) caption("CampaignLoadingOwner", owner_name, 1.5F, true);
        if (!income_caption && resources.options.basic_income) caption("CampaignLoadingIncome",
            "Income: " + std::to_string(static_cast<std::int64_t>(std::llround(*resources.options.basic_income))), 3.0F, false);
    }
    if (layout.begin != nullptr) {
        layout.begin->set_visible(ready_ && !begin_pressed_);
        layout.begin->set_focus_mode(Control::FOCUS_NONE);
        layout.begin->connect("pressed", callable_mp(this, &EawrLoadingScreen::on_begin_pressed));
    }
    const auto fill_list = [&](EawrUiList* list, const std::string& body, const bool tactical) {
        if (!list) return;
        if (tactical) list->add_theme_color_override("font_color", Color(50.0F / 255, 200.0F / 255, 1));
        // BLD-05: the history as the lines of the list box, none selectable.
        const Ref<Font> font = list->get_theme_font(StringName("font"));
        const int font_size = list->get_theme_font_size(StringName("font_size"));
        float margin = 8.0F;
        if (const Ref<StyleBox> panel = list->get_theme_stylebox(StringName("panel")); panel.is_valid()) {
            margin = panel->get_content_margin(SIDE_LEFT) + panel->get_content_margin(SIDE_RIGHT);
        }
        float bar_width = 18.0F;
        if (VScrollBar* scroll = list->get_v_scroll_bar()) bar_width = scroll->get_combined_minimum_size().x;
        const double available = std::max(32.0, static_cast<double>(list->get_size().x - margin - bar_width));
        const auto lines = model::loading_wrap_lines(body, available, [&](const std::string_view line) {
            if (font.is_null()) return static_cast<double>(line.size()) * 6.0;
            return static_cast<double>(font->get_string_size(String::utf8(line.data(), static_cast<int64_t>(line.size())),
                HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x);
        });
        for (const std::string& line : lines) list->add_item(text(line), Ref<Texture2D>(), false);
        if (list == layout.history) resources.history_lines = lines;
    };
    fill_list(layout.history, content.history, false);
    fill_list(layout.mission, content.mission_body, false);
    if (resources.options.campaign_space) {
        fill_list(layout.tactical, content.tactical, true);
        fill_list(layout.advantage, content.advantage, false);
    }
    if (layout.bar != nullptr) {
        // The retail bar keeps its caps and its empty strip from the kit; the fill is the rendering
        // server's, painted at once (see the header).
        layout.bar->set_value(0.0);
        auto* rendering = RenderingServer::get_singleton();
        fill_item_ = rendering->canvas_item_create();
        rendering->canvas_item_set_parent(fill_item_, layout.bar->get_canvas_item());
        rendering->canvas_item_set_default_texture_filter(fill_item_, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
        paint_fill(ratio_);
    }
    return true;
}

void EawrLoadingScreen::paint_fill(const double ratio) {
    if (!layout_ || layout_->bar == nullptr || !fill_item_.is_valid()) return;
    auto* rendering = RenderingServer::get_singleton();
    rendering->canvas_item_clear(fill_item_);
    const auto piece = [&](const char* name) -> Ref<Texture2D> {
        const StringName slot(name);
        if (!layout_->bar->has_theme_icon(slot)) return {};
        Ref<Texture2D> texture = layout_->bar->get_theme_icon(slot);
        return texture.is_valid() && texture->has_meta(empty_piece_meta) ? Ref<Texture2D>() : texture;
    };
    const Ref<Texture2D> left = piece("Progress_Bar_Left");
    const Ref<Texture2D> right = piece("Progress_Bar_Right");
    const Ref<Texture2D> on = piece("Progress_Bar_Middle_On");
    if (on.is_null()) return;
    const Vector2 size = layout_->bar->get_size();
    const auto cap = [&](const Ref<Texture2D>& texture) {
        if (texture.is_null()) return 0.0F;
        const Vector2 native = texture->get_size();
        return native.y > 0.0F ? native.x * size.y / native.y : 0.0F;
    };
    float left_width = cap(left);
    float right_width = cap(right);
    if (left_width + right_width > size.x && left_width + right_width > 0.0F) {
        const float shrink = size.x / (left_width + right_width);
        left_width *= shrink;
        right_width *= shrink;
    }
    const float middle = size.x - left_width - right_width;
    const float filled = std::round(middle * static_cast<float>(std::clamp(ratio, 0.0, 1.0)));
    const Vector2 native = on->get_size();
    if (native.x <= 0.0F || native.y <= 0.0F || filled <= 0.0F) return;
    const float tile = native.x * size.y / native.y;
    for (float x = left_width; x < left_width + filled - 0.01F; x += tile) {
        const float width = std::min(tile, left_width + filled - x);
        rendering->canvas_item_add_texture_rect_region(fill_item_, Rect2(x, 0.0F, width, size.y), on->get_rid(),
            Rect2(0.0F, 0.0F, native.x * width / tile, native.y), Color(1, 1, 1, 1));
    }
}

void EawrLoadingScreen::present(const bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (frames_ < present_after_frames) return;
    if (!force && now - last_present_ < present_interval) return;
    last_present_ = now;
    ++presents_;
    if (planet_) planet_->advance(std::chrono::duration<double>(now - planet_started_).count());
    RenderingServer::get_singleton()->force_draw(true, 0.0);
}

void EawrLoadingScreen::capture(const char* label) {
    if (capture_directory_.empty() || frames_ < present_after_frames) return;
    std::error_code ignored;
    std::filesystem::create_directories(capture_directory_, ignored);
    const Ref<Image> image = get_viewport()->get_texture()->get_image();
    if (image.is_null()) return;
    const std::filesystem::path path = capture_directory_ / (std::string("loading_") + label + ".png");
    image->save_png(String::utf8(path.generic_string().c_str()));
    captures_.push_back(path.generic_string());
}

void EawrLoadingScreen::report(const model::LoadProgress& progress) {
    if (!shown_ || !layout_ || failed_ || native_progress_) return;
    const bool first = reports_ == 0;
    ++reports_;
    const auto now = std::chrono::steady_clock::now();
    if (!first) stage_ms_[progress.stage()] += std::chrono::duration<double, std::milli>(now - last_report_).count();
    last_report_ = now;
    const bool boundary = progress.stage() != stage_;
    stage_ = progress.stage();
    ratio_ = progress.fraction();
    paint_fill(ratio_);
    present(first || boundary);
    if (first) capture("start");
    if (!captured_half_ && ratio_ >= 0.5 && frames_ >= present_after_frames) {
        captured_half_ = true;
        present(true);
        capture("half");
    }
}

void EawrLoadingScreen::observe_preparation(std::shared_ptr<const platform::CampaignLoadingView> view) {
    if (!view || preparation_ == view) return;
    preparation_ = std::move(view);
    if (preparation_->progress) {
        native_progress_ = true;
        // WGP-163..166: numeric reports may regress and never arm Begin.
        ratio_ = preparation_->progress->value.fraction;
        reports_ = static_cast<std::size_t>(preparation_->progress->sequence);
        paint_fill(ratio_);
        present(true);
    }
    if (preparation_->outcome == platform::CampaignLoadingOutcome::failed) {
        if (planet_) planet_->release();
        failed_ = true; ready_ = false; campaign_gate_.complete(false);
        set_meta("loading_ready", false);
        if (layout_ && layout_->begin) layout_->begin->hide();
    }
}

void EawrLoadingScreen::show_failure(const std::string& message) {
    if (planet_) planet_->release();
    failed_ = true; ready_ = false; failure_ = message;
    campaign_gate_.complete(false);
    set_meta("loading_ready", false);
    if (layout_ && layout_->begin) layout_->begin->hide();
    if (failure_acknowledged_) return;
    present(true);
    // WGP-167: use OS acknowledgement, not a guessed retry/return dialog.
    OS::get_singleton()->alert(text(message), "Error");
    failure_acknowledged_ = true;
}

void EawrLoadingScreen::discard_queued_input() {
    // BLD-15, WGP-113: retail's load pumps its window messages with the input-safe flag clear, so a
    // key or click made during the load is dropped. Our load holds the main thread, so those events
    // wait in the window's queue and would reach Begin the frame after it arms. Dispatch them now,
    // while Begin is not armed, and have the screen swallow them.
    auto* display = DisplayServer::get_singleton();
    auto* input = Input::get_singleton();
    if (display == nullptr || input == nullptr) return;
    draining_ = true;
    display->process_events();
    input->flush_buffered_events();
    draining_ = false;
    set_meta("loading_drained_inputs", static_cast<int64_t>(drained_inputs_));
}

void EawrLoadingScreen::show_ready() {
    if (!shown_ || !layout_ || failed_
        || (preparation_ && preparation_->outcome != platform::CampaignLoadingOutcome::succeeded)) return;
    discard_queued_input();
    // The view's own preparation after the last placement belongs to the scene stage.
    if (reports_ != 0 && !native_progress_) {
        stage_ms_.back() += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - last_report_).count();
    }
    ratio_ = 1.0;
    ready_ = true;
    campaign_gate_.complete(true);
    set_meta("loading_ready", true);
    paint_fill(ratio_);
    if (layout_->begin != nullptr) layout_->begin->set_visible(true);
    else if (resources_->options.campaign_space) campaign_gate_.request(topmost()); // WGP-114 no-Begin fallback.
    // The 3D view draws again behind the screen so the battle's pipelines are built before Begin.
    set_3d(true);
    present(true);
    // Visibility changes queue a normal Control redraw. Capture after that redraw so Begin
    // is present in the ready still, including a screen that already drew during preparation.
    ready_frame_ = frames_;
    capture_ready_pending_ = true;
}

bool EawrLoadingScreen::take_begin() noexcept {
    if (resources_ && resources_->options.campaign_space) {
        if (!campaign_gate_.take(topmost())) return false;
        ++accepted_begins_; return true;
    }
    if (!ready_ || !begin_pressed_) return false;
    begin_pressed_ = false;
    return true;
}

void EawrLoadingScreen::request_begin() noexcept {
    if (resources_ && resources_->options.campaign_space) {
        campaign_gate_.request(topmost()); return;
    }
    if (ready_ && shown_) begin_pressed_ = true;
}

void EawrLoadingScreen::dismiss() {
    if (!shown_) return;
    shown_ = false;
    if (planet_) planet_->release();
    hide();
    set_3d(true);
}

void EawrLoadingScreen::viewport_changed(const float width, const float height) {
    if (!shown_ || !resources_ || !layout_) return;
    if (std::abs(layout_->size.x - width) < 0.5F && std::abs(layout_->size.y - height) < 0.5F) return;
    resources_->history_lines.clear();
    layout_->images.clear();
    static_cast<void>(build_layout(Vector2(width, height)));
}

std::optional<Rect2> EawrLoadingScreen::begin_rect() const {
    if (!layout_ || layout_->begin == nullptr || !layout_->begin->is_visible_in_tree()) return std::nullopt;
    return layout_->begin->get_global_rect();
}

std::string EawrLoadingScreen::report_json() const {
    std::ostringstream output;
    const model::LoadingScreenContent* content = resources_ && resources_->content ? &*resources_->content : nullptr;
    output << "{\"shown\": " << (shown_ ? "true" : "false") << ", \"ready\": " << (ready_ ? "true" : "false")
           << ", \"unsafe_inputs\": " << unsafe_inputs_ << ", \"drained_inputs\": " << drained_inputs_
           << ", \"accepted_begins\": " << accepted_begins_
           << ", \"begin_visible\": " << (layout_ && layout_->begin != nullptr && layout_->begin->is_visible_in_tree() ? "true" : "false")
           << ", \"progress\": " << ratio_ << ", \"reports\": " << reports_ << ", \"presents\": " << presents_;
    if (layout_) output << ", \"layout_size\": [" << layout_->size.x << ", " << layout_->size.y << "]";
    if (const auto rect = begin_rect()) {
        output << ", \"begin_rect\": [" << rect->position.x << ", " << rect->position.y << ", " << rect->size.x << ", "
               << rect->size.y << "]";
    }
    if (content != nullptr) {
        output << ", \"dialog\": " << json(content->dialog) << ", \"title\": " << json(content->title)
               << ", \"planet\": " << json(content->planet) << ", \"weather\": " << json(content->weather)
               << ", \"terrain\": " << json(content->terrain) << ", \"population\": " << json(content->population)
               << ", \"history_chars\": " << content->history.size()
               << ", \"tactical_chars\": " << content->tactical.size()
               << ", \"advantage_chars\": " << content->advantage.size()
               << ", \"mission_kind\": " << static_cast<int>(resources_->options.mission)
               << ", \"mission_heading\": " << json(content->mission_heading)
               << ", \"mission_body_chars\": " << content->mission_body.size()
               << ", \"campaign\": " << (resources_->options.campaign_space ? "true" : "false")
               << ", \"owner\": " << json(resources_->options.owner_name)
               << ", \"basic_income\": " << (resources_->options.basic_income ? std::to_string(*resources_->options.basic_income) : "null")
               << ", \"history_lines\": " << resources_->history_lines.size()
               << ", \"native_icon\": " << json(content->native_icon) << ", \"weather_icon\": " << json(content->weather_icon)
               << ", \"map_icon\": " << json(content->map_icon);
    }
    output << ", \"failed\": " << (failed_ ? "true" : "false")
           << ", \"failure_acknowledged\": " << (failure_acknowledged_ ? "true" : "false")
           << ", \"failure\": " << json(failure_)
           << ", \"progress_source\": " << json(native_progress_ ? "native" : "viewer-work-policy");
    output << ", \"planet_clone\": " << (planet_ ? planet_->report_json() : "null");
    if (preparation_) {
        output << ", \"preparation_revision\": " << preparation_->revision
               << ", \"selected_map\": " << json(preparation_->descriptor.selected_map)
               << ", \"current_model_available\": " << (preparation_->descriptor.current_model ? "true" : "false")
               << ", \"native_progress_newest_first\": [";
        bool first = true;
        for (auto node = preparation_->progress; node; node = node->previous) {
            output << (first ? "" : ", ") << "{\"sequence\": " << node->sequence
                   << ", \"producer\": " << static_cast<int>(node->value.producer)
                   << ", \"list\": " << node->value.list << ", \"index\": " << node->value.index
                   << ", \"count\": " << node->value.count << ", \"fraction\": " << node->value.fraction << "}";
            first = false;
        }
        output << "]";
    }
    output << ", \"stage_ms\": [";
    for (std::size_t index = 0; index < stage_ms_.size(); ++index) output << (index != 0 ? ", " : "") << stage_ms_[index];
    output << "]";
    output << ", \"pictures\": [";
    if (layout_) {
        for (std::size_t index = 0; index < layout_->images.size(); ++index) {
            output << (index != 0 ? ", " : "") << json(layout_->images[index]);
        }
    }
    output << "], \"captures\": [";
    for (std::size_t index = 0; index < captures_.size(); ++index) output << (index != 0 ? ", " : "") << json(captures_[index]);
    output << "], \"notes\": [";
    for (std::size_t index = 0; index < notes_.size(); ++index) output << (index != 0 ? ", " : "") << json(notes_[index]);
    output << "]}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
