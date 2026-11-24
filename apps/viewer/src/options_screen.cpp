#include "revised_layer.hpp"
#include "eawr/vfs/vfs.hpp"
#include "options_screen.hpp"
#include "eawr/presentation/ui/build_tray_files.hpp"
#include "eawr/presentation/ui/selection_config.hpp"
#include "dialog_rectangles.hpp"

#include "render_profile_viewport.hpp"
#include "viewer_path.hpp"
#include "saved_games.hpp"

#include "ui/dialog_builder.hpp"
#include "ui/font_provider.hpp"
#include "ui/input_routing.hpp"
#include "ui/kit.hpp"
#include "ui/message_box_view.hpp"
#include "ui/load_game_view.hpp"
#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/theme.hpp"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <map>
#include <optional>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace model = presentation::ui;
namespace data = eawr::data::ui;

namespace {

class OptionsTiming final {
public:
    explicit OptionsTiming(const char* phase) : phase_(phase), enabled_(enabled()) {
        if (enabled_) start_ = std::chrono::steady_clock::now();
    }
    ~OptionsTiming() {
        if (!enabled_) return;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count();
        UtilityFunctions::print("EAWR options timing ", phase_, ": ", us, " us");
    }
private:
    static bool enabled() {
        static const bool value = OS::get_singleton()->has_environment("EAWR_OPTIONS_TIMING");
        return value;
    }
    const char* phase_;
    bool enabled_;
    std::chrono::steady_clock::time_point start_{};
};

[[nodiscard]] const char* supersampling_name(const int level) noexcept {
    return level == 2 ? "2x" : level == 1 ? "1.5x" : "off";
}

// Above the loading screen (98) and under the performance overlay and capture pointer (100): the
// screen is modal, so nothing under it takes input (UI-I1).
constexpr int screen_layer = 99;

[[nodiscard]] String text(const std::string& value) {
    return String::utf8(value.c_str(), static_cast<int64_t>(value.size()));
}

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
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

struct PageInfo final {
    EawrOptionsScreen::Page page;
    const char* name;
    const char* dialog;
};
constexpr PageInfo pages[] = {
    {EawrOptionsScreen::Page::hub, "hub", ""},
    {EawrOptionsScreen::Page::audio, "audio", "IDD_AUDIO_OPTIONS_DIALOG"},
    {EawrOptionsScreen::Page::video, "video", "IDD_VIDEO_OPTIONS_DIALOG"},
    {EawrOptionsScreen::Page::performance, "performance", "IDD_VIDEO_OPTIONS_DIALOG"},
    {EawrOptionsScreen::Page::tweaks, "tweaks", "IDD_VIDEO_OPTIONS_DIALOG"},
    {EawrOptionsScreen::Page::advanced_video, "advanced", "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG"},
    {EawrOptionsScreen::Page::gameplay, "gameplay", "IDD_GAMEPLAY_OPTIONS_DIALOG"},
    {EawrOptionsScreen::Page::keyboard, "keyboard", "IDD_KEYBOARD_OPTIONS_DIALOG"},
    {EawrOptionsScreen::Page::network, "network", "IDD_NETWORK_OPTIONS_DIALOG"},
    {EawrOptionsScreen::Page::game_menu, "game", "IDD_GAME_OPTIONS_DIALOG"},
    {EawrOptionsScreen::Page::save_game, "save", "IDD_SAVE_GAME_DIALOG"},
};

[[nodiscard]] std::string keyboard_dialog(const int number) {
    return number <= 1 ? "IDD_KEYBOARD_OPTIONS_DIALOG" : "IDD_KEYBOARD_OPTIONS_DIALOG_" + std::to_string(number);
}

constexpr int keyboard_page_count = 5;

// A page's gadgets that retail hides or this screen leaves to the menu around it.
[[nodiscard]] bool hidden_gadget(const std::string& id) {
    return id == "IDC_DIALOG_CLOSE_BUTTON" || id == "IDC_MENU_CREDITS" || id == "IDC_MENU_PETRO_LOGO"
        || id == "IDC_RADIO_EASY" || id == "IDC_RADIO_MEDIUM" || id == "IDC_RADIO_HARD" || id == "IDC_RADIO_BLANK"
        || id == "IDC_RADIO_BLANK1" || id == "IDC_RADIO_BLANK2" || id == "IDC_RADIO_BLANK3";
}

[[nodiscard]] bool is_audio_slider(const std::string& id) {
    return id == "IDC_SLIDER_MASTER_VOLUME" || id == "IDC_SLIDER_SOUND_EFFECTS_VOLUME"
        || id == "IDC_SLIDER_DIALOG_VOLUME" || id == "IDC_SLIDER_MUSIC_VOLUME";
}

// The video page's controls whose change enables Apply (retail's Set_Settings_Dirty).
[[nodiscard]] bool is_video_edit(const std::string& id) {
    return id == "IDC_RESOLUTION_LIST" || id == "IDC_WINDOWED_MODE" || id == "IDC_VSYNC_ENABLED"
        || id == "IDC_ANTI_ALIAS_SLIDER";
}

} // namespace

struct EawrOptionsScreen::Resources final {
    std::shared_ptr<const vfs::Vfs> filesystem;
    std::optional<data::DialogCatalog> catalog;
    std::optional<assets::MegaTextureAtlas> atlas;
    std::unique_ptr<UiTextures> textures;
    std::unique_ptr<FontProvider> fonts;
    std::optional<vfs::Vfs> font_filesystem;
    std::optional<data::TextDatabase> text_database;
    std::optional<data::TextLookup> text;
    std::optional<model::ThemeModel> theme_model;
    Ref<Theme> theme;
    std::unique_ptr<ThemeBuildTask> theme_task;
    Vector2 theme_size; // the viewport the theme was made for
};

struct EawrOptionsScreen::PageState final {
    Control* root{};       // the modal layer
    Control* backdrop{};
    std::optional<BuiltDialog> dialog;
    std::map<std::string, Control*> controls;
    std::map<std::string, Control*> focus_outlines;
    std::map<std::string, std::string> reasons; // disabled controls and why
    std::vector<model::DisplaySize> sizes;      // the video page's list
    model::DisplaySize selected_size;
    bool windowed_draft{};
    bool profile_touched{};
    // The Advanced page's two honoured controls (#6560): untouched, they leave the stored value
    // (possibly automatic) as it is.
    bool particle_touched{};
    bool heat_touched{};
    ScrollContainer* performance_scroll{};
    std::vector<std::string> saves; // the Save page's list, in list order
};

EawrOptionsScreen::EawrOptionsScreen() {
    set_layer(screen_layer);
    set_visible(false);
}

EawrOptionsScreen::~EawrOptionsScreen() = default;

void EawrOptionsScreen::_bind_methods() { ADD_SIGNAL(MethodInfo("closed")); }

const char* EawrOptionsScreen::page_name(const Page which) noexcept {
    for (const PageInfo& info : pages) {
        if (info.page == which) return info.name;
    }
    return "hub";
}

bool EawrOptionsScreen::parse_page(const std::string& name, Page& result) noexcept {
    for (const PageInfo& info : pages) {
        if (name == info.name) {
            result = info.page;
            return true;
        }
    }
    return false;
}

std::string EawrOptionsScreen::settings_file() const { return ViewerPath::utf8(store_.file()); }

void EawrOptionsScreen::begin_build(Options options) {
    options_ = std::move(options);
    resources_ = std::make_unique<Resources>();
    build_stage_ = 0;
    built_ = false;
    failure_.clear();
}

EawrOptionsScreen::BuildProgress EawrOptionsScreen::advance_build() {
    if (!resources_ || !failure_.empty()) return BuildProgress::failed;
    if (built_) return BuildProgress::ready;
    OptionsTiming timing("build-step");
    Resources& resources = *resources_;
    switch (build_stage_) {
    case 0: {
        store_ = SettingsStore(options_.settings_file);
        break;
    }
    case 1: {
        resources.filesystem = options_.filesystem;
        if (!resources.filesystem) {
            if (options_.game_root.empty()) {
                failure_ = "the options screen needs the game root or the content a menu mounted";
                return BuildProgress::failed;
            }
            const std::filesystem::path expansion = options_.game_root / "corruption" / "Data";
            const std::filesystem::path base = options_.game_root / "GameData" / "Data";
            if (!std::filesystem::is_directory(expansion) || !std::filesystem::is_directory(base)) {
                failure_ = "the game root needs corruption/Data and GameData/Data (FoC)";
                return BuildProgress::failed;
            }
            std::vector<std::pair<std::string, std::filesystem::path>> roots;
            if (!options_.mod_root.empty()) {
                const std::filesystem::path data_folder = options_.mod_root / "Data";
                roots.emplace_back("mod", std::filesystem::is_directory(data_folder) ? data_folder : options_.mod_root);
            }
            roots.emplace_back("expansion", expansion);
            roots.emplace_back("base", base);
            mount_revised_layer(roots);
            std::vector<vfs::MountSpec> specs;
            for (const auto& [id, root] : roots) {
                auto manifest = vfs::resolve_manifest_mount(id, root);
                if (!manifest) {
                    failure_ = core::format_diagnostic(manifest.error());
                    return BuildProgress::failed;
                }
                specs.push_back(std::move(manifest.value().mount));
            }
            auto mounted = vfs::Vfs::mount(specs);
            if (!mounted) {
                failure_ = core::format_diagnostic(mounted.error());
                return BuildProgress::failed;
            }
            resources.filesystem = std::make_shared<const vfs::Vfs>(std::move(mounted.value()));
        }
        break;
    }
    case 2: {
        auto catalog = data::load_dialog_catalog(*resources.filesystem);
        if (!catalog) {
            failure_ = core::format_diagnostic(catalog.error());
            return BuildProgress::failed;
        }
        resources.catalog.emplace(std::move(catalog.value()));
        break;
    }
    case 3: {
        auto atlas = assets::load_mega_texture_atlas(*resources.filesystem,
            "data/art/textures/" + resources.catalog->skin.texture_file + ".mtd");
        if (!atlas) {
            failure_ = core::format_diagnostic(atlas.error());
            return BuildProgress::failed;
        }
        resources.atlas.emplace(std::move(atlas.value()));
        resources.textures = std::make_unique<UiTextures>(*resources.atlas, *resources.filesystem);
        if (!resources.textures->page_failure().empty()) {
            failure_ = "atlas page: " + resources.textures->page_failure();
            return BuildProgress::failed;
        }
        break;
    }
    case 4: {
        if (auto text_database = data::load_language_text_database(*resources.filesystem, "ENGLISH")) {
            resources.text_database.emplace(std::move(text_database.value()));
            resources.text.emplace(*resources.text_database);
        } else {
            notes_.push_back("text database: " + core::format_diagnostic(text_database.error()) + "; captions show their keys");
        }
        break;
    }
    case 5: {
        const std::filesystem::path cache = default_font_cache(options_.font_cache);
        const vfs::MountSpec mount{.layer_id = "font-cache", .data_root = cache,
                                   .loose_logical_prefix = std::string(model::font_cache_prefix), .active_archives = {}};
        auto font_mount = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&mount, 1));
        if (font_mount) resources.font_filesystem.emplace(std::move(font_mount.value()));
        const vfs::Vfs unmounted;
        resources.fonts = std::make_unique<FontProvider>(model::load_font_cache(
            resources.font_filesystem ? *resources.font_filesystem : unmounted, ViewerPath::utf8(cache)));
        built_ = true;
        return BuildProgress::ready;
    }
    default: return BuildProgress::failed;
    }
    ++build_stage_;
    return BuildProgress::pending;
}

bool EawrOptionsScreen::build(Options options) {
    OptionsTiming timing("build");
    begin_build(std::move(options));
    BuildProgress result;
    do { result = advance_build(); } while (result == BuildProgress::pending);
    return result == BuildProgress::ready;
}

void EawrOptionsScreen::set_game_actions(Options options) {
    options_.save_names = std::move(options.save_names);
    options_.save_game = std::move(options.save_game);
    options_.load_directory = std::move(options.load_directory);
    options_.load_game = std::move(options.load_game);
}

bool EawrOptionsScreen::prewarm_theme(Node& parent, const bool finish) {
    OptionsTiming timing("theme");
    if (!built_ || parent.get_viewport() == nullptr) return false;
    Resources& resources = *resources_;
    const Vector2 size = parent.get_viewport()->get_visible_rect().size;
    // The theme follows the viewport's size (UI-L1): made once per size, not per page.
    if (resources.theme_size != size) {
        resources.theme_task.reset();
        resources.theme.unref();
    }
    if (resources.theme.is_null() && !resources.theme_task) {
        model::ThemeSources sources;
        sources.catalog = &*resources.catalog;
        sources.atlas = &resources.atlas->directory;
        sources.standalone = model::vfs_standalone_textures(*resources.filesystem);
        sources.fonts = &resources.fonts->cache();
        const std::vector<std::string>& families = resources.fonts->system_families();
        sources.system = [&families](const std::string_view face) {
            return model::match_system_face(face, families).has_value();
        };
        resources.theme_model.emplace(model::build_theme_model(
            sources, model::reference_space({static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y)},
                                            options_.rules)));
        resources.theme_size = size;
        resources.theme_task = std::make_unique<ThemeBuildTask>(*resources.theme_model, *resources.textures, *resources.fonts);
    }

    if (resources.theme_task) {
        bool done = false;
        for (int step = 0; step < 8 && !done; ++step) done = resources.theme_task->step();
        while (finish && !done) done = resources.theme_task->step();
        if (!done) return false;
        resources.theme = resources.theme_task->theme();
        resources.theme_task.reset();
    }
    return resources.theme.is_valid();
}

void EawrOptionsScreen::reload_settings() {
    store_ = SettingsStore(options_.settings_file);
    // The settings: a missing file is the defaults, an unreadable one the defaults with notes.
    file_existed_ = store_.exists();
    model::SettingsLoad loaded = store_.load();
    settings_notes_ = std::move(loaded.notes);
    committed_ = std::move(loaded.settings);
    if (!file_existed_) {
        // No file yet: the video page starts from the window the player has, not from FoC's 800x600.
        const DisplayFacts facts = display_facts();
        if (facts.window.width > 0 && facts.window.height > 0) {
            committed_.video.window_width = facts.window.width;
            committed_.video.window_height = facts.window.height;
            committed_.video.fullscreen_width = facts.screen.width > 0 ? facts.screen.width : facts.window.width;
            committed_.video.fullscreen_height = facts.screen.height > 0 ? facts.screen.height : facts.window.height;
            committed_.video.windowed = facts.windowed;
            committed_.video.vsync = facts.vsync;
            static_cast<void>(model::clamp_settings(committed_));
        }
    }
    draft_ = committed_;

    const vfs::Vfs& filesystem = *resources_->filesystem;
    model::apply_selection_config(committed_, filesystem);
    {
        const auto layout = model::build_tray::load_tray_layout(filesystem);
        committed_.galactic_build_tray_layer_default = model::build_tray::layer_enabled_by_default(layout);
        // GBT-11: a rejected layout file gets one EAWR-UI diagnostic. The screen is built often, so a reason already
        // reported is not repeated; a different broken file reports again.
        static std::string reported;
        if (layout.invalid && layout.first_error != reported) {
            reported = layout.first_error;
            UtilityFunctions::push_warning(text("EAWR-UI-BUILD-TRAY: invalid layout file; retaining the last valid one: "
                + layout.first_error));
        }
    }
    draft_ = committed_;

}

void EawrOptionsScreen::show(Node& parent, const Page start) {
    OptionsTiming timing("show");
    if (!built_ || shown_) return;
    reload_settings();
    parent.add_child(this);
    set_visible(true);
    shown_ = true;
    set_process_input(true);
    draft_ = committed_;
    page_ = start;
    menu_root_ = start == Page::game_menu;
    quit_requested_ = false;
    if (Viewport* viewport = get_viewport(); viewport != nullptr && !resize_connected_) {
        viewport->connect("size_changed", callable_mp(this, &EawrOptionsScreen::on_viewport_resized));
        resize_connected_ = true;
    }
    rebuild_page();
}

void EawrOptionsScreen::on_viewport_resized() {
    if (!shown_ || !resources_) return;
    Viewport* viewport = get_viewport();
    if (!viewport) return;
    const Vector2 size = viewport->get_visible_rect().size;
    if (size.x == built_width_ && size.y == built_height_) return;
    const bool was_dirty = dirty_;
    std::vector<std::pair<std::string, String>> edits;
    if (state_) {
        for (const auto& [id, control] : state_->controls) {
            if (auto* edit = Object::cast_to<LineEdit>(control)) edits.emplace_back(id, edit->get_text());
        }
        sync_draft_from_controls();
    }
    const bool had_confirmation = confirm_open();
    if (had_confirmation) {
        if (Node* box = Object::cast_to<Node>(UtilityFunctions::instance_from_id(confirm_id_))) {
            remove_child(box);
            box->queue_free();
        }
        confirm_id_ = 0;
    }
    rebuild_page();
    for (const auto& [id, value] : edits) {
        const auto found = state_->controls.find(id);
        if (found != state_->controls.end()) {
            if (auto* edit = Object::cast_to<LineEdit>(found->second)) edit->set_text(value);
        }
    }
    dirty_ = was_dirty;
    if (dirty_) {
        const auto found = state_->controls.find("IDC_APPLY");
        if (found != state_->controls.end()) {
            if (auto* apply = Object::cast_to<BaseButton>(found->second)) apply->set_disabled(false);
        }
    }
    if (had_confirmation) {
        if (confirm_ == Confirm::overwrite) ask_overwrite(overwrite_name_);
        else ask_quit();
    }
}

void EawrOptionsScreen::clear_page() {
    if (!state_) return;
    if (state_->performance_scroll) performance_scroll_ = state_->performance_scroll->get_v_scroll();
    if (state_->root != nullptr) {
        remove_child(state_->root);
        state_->root->queue_free();
    }
    state_.reset();
}

void EawrOptionsScreen::close_screen() {
    if (!shown_) return;
    if (confirm_open()) {
        if (Node* box = Object::cast_to<Node>(UtilityFunctions::instance_from_id(confirm_id_))) {
            remove_child(box);
            box->queue_free();
        }
    }
    confirm_id_ = 0;
    shown_ = false;
    if (resize_connected_) {
        if (Viewport* viewport = get_viewport()) viewport->disconnect("size_changed", callable_mp(this, &EawrOptionsScreen::on_viewport_resized));
        resize_connected_ = false;
    }
    set_process_input(false);
    clear_page();
    set_visible(false);
    if (Node* parent = get_parent()) parent->remove_child(this);
    emit_signal("closed");
}

void EawrOptionsScreen::rebuild_page() {
    if (page_ != Page::keyboard) capturing_.reset(); // a waiting binding capture never outlives its page
    clear_page();
    Resources& resources = *resources_;
    state_ = std::make_unique<PageState>();
    PageState& state = *state_;
    const Vector2 size = get_viewport()->get_visible_rect().size;
    built_width_ = size.x;
    built_height_ = size.y;

    static_cast<void>(prewarm_theme(*this));

    auto* root = memnew(EawrUiModalLayer);
    root->set_name("EawrOptionsScreen");
    root->set_position(Vector2());
    root->set_size(size);
    root->set_theme(resources.theme);
    add_child(root);
    state.root = root;
    auto* backdrop = memnew(ColorRect);
    // The main menu's own backdrop shows behind the dialog; the game under the in-game menu is dimmed.
    backdrop->set_color(Color(0.0F, 0.0F, 0.0F, options_.context == Context::main_menu ? 0.0F : 0.55F));
    backdrop->set_size(size);
    backdrop->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    root->add_child(backdrop);
    state.backdrop = backdrop;

    std::string dialog_name;
    model::Placement placement = model::Placement::centre;
    switch (page_) {
    case Page::hub:
        dialog_name = options_.context == Context::main_menu ? "IDD_MAIN_OPTIONS_DIALOG" : "IDD_IN_GAME_OPTIONS_DIALOG";
        if (options_.context == Context::main_menu) placement = model::Placement::full_screen;
        break;
    case Page::keyboard: dialog_name = keyboard_dialog(keyboard_page_); break;
    default:
        for (const PageInfo& info : pages) {
            if (info.page == page_) dialog_name = info.dialog;
        }
    }
    const data::Dialog* found = resources.catalog->script.find(dialog_name);
    if (found == nullptr) {
        failure_ = "dialog " + dialog_name + " is not in the dialog resources";
        notes_.push_back(failure_);
        return;
    }
    data::Dialog service = *found;
    if (page_ == Page::hub && options_.context == Context::main_menu) {
        const auto back = std::find_if(service.controls.begin(), service.controls.end(),
                                      [](const auto& c) { return c.id_name == "IDC_OK_MAIN"; });
        if (back != service.controls.end()) {
            // ST-N2: the owner-requested entry belongs immediately before Back.
            // Clone the loaded button so mod skins and resource styles still apply.
            data::DialogControl network = *back;
            network.id_name = "IDC_MAIN_OPTIONS_NETWORK";
            network.id.reset();
            network.text = "Network";
            service.controls.insert(back, std::move(network));
        }
    }
    if (page_ == Page::hub) {
        const bool main = options_.context == Context::main_menu;
        const auto back = std::find_if(service.controls.begin(), service.controls.end(), [main](const auto& control) {
            return control.id_name == (main ? "IDC_OK_MAIN" : "IDC_OK_IN_GAME");
        });
        if (back != service.controls.end()) {
            data::DialogControl performance = *back;
            performance.id_name = main ? "IDC_MAIN_OPTIONS_PERFORMANCE" : "IDC_IN_GAME_OPTIONS_PERFORMANCE";
            performance.id.reset();
            performance.text = "Performance";
            if (!main) {
                const int step = std::max(24, back->rect.height + 8);
                back->rect.y += step;
                service.rect.height += step;
            }
            data::DialogControl tweaks = performance;
            tweaks.id_name = main ? "IDC_MAIN_OPTIONS_TWEAKS" : "IDC_IN_GAME_OPTIONS_TWEAKS";
            tweaks.text = "TEXT_EAWR_TWEAKS";
            if (!main) { back->rect.y += std::max(24, back->rect.height + 8); service.rect.height += std::max(24, back->rect.height + 8); }
            const auto position = service.controls.insert(back, std::move(performance));
            service.controls.insert(position + 1, std::move(tweaks));
        }
    }
    if (page_ == Page::keyboard) {
        // ST-K3: append the owner-requested preset to the loaded keyboard page.
        const int top = service.rect.height;
        service.rect.height += 204;
        data::DialogControl label;
        label.statement = "LTEXT"; label.id_name = "IDC_CAMERA_KEYS_LABEL";
        label.text = "Camera keys"; label.rect = {12, top + 8, service.rect.width / 3 - 18, 20};
        label.style = {"WS_VISIBLE"}; service.controls.push_back(label);
        data::DialogControl combo;
        combo.statement = "COMBOBOX"; combo.id_name = "IDC_CAMERA_KEYS";
        combo.rect = {service.rect.width / 3, top + 4, service.rect.width * 2 / 3 - 12, 26};
        combo.style = {"WS_VISIBLE", "WS_TABSTOP", "CBS_DROPDOWNLIST"}; service.controls.push_back(combo);
        // #5131: a press-to-bind button per action; the next key press is the binding.
        data::DialogControl bind;
        bind.statement = "PUSHBUTTON"; bind.style = {"WS_VISIBLE", "WS_TABSTOP"};
        for (std::size_t action = 0; action < model::selection_commands.size(); ++action) {
            label.id_name = action == 0 ? "IDC_SELECTION_ALL_LABEL" : "IDC_SELECTION_SCREEN_LABEL";
            label.text = action == 0 ? "All own units" : "Own units on screen";
            label.rect = {12, top + 40 + static_cast<int>(action) * 34, service.rect.width / 2 - 18, 20};
            service.controls.push_back(label);
            bind.id_name = action == 0 ? "IDC_SELECTION_ALL_KEY" : "IDC_SELECTION_SCREEN_KEY";
            bind.text = capturing_ == action ? "Press key / mouse..." : keyboard_key_label(model::selection_key(draft_, action));
            bind.rect = {service.rect.width / 2, top + 36 + static_cast<int>(action) * 34, service.rect.width / 2 - 12, 26};
            service.controls.push_back(bind);
        }
        // GBT-07: the galactic build tray's key, a third press-to-bind row (any one key without modifiers).
        label.id_name = "IDC_BUILD_TRAY_KEY_LABEL";
        label.text = "Galactic build tray";
        label.rect = {12, top + 108, service.rect.width / 2 - 18, 20};
        service.controls.push_back(label);
        bind.id_name = "IDC_BUILD_TRAY_KEY";
        bind.text = capturing_ == build_tray_capture ? "Press key..." : std::string(model::build_tray_key(draft_));
        bind.rect = {service.rect.width / 2, top + 104, service.rect.width / 2 - 12, 26};
        service.controls.push_back(bind);
        data::DialogControl defaults;
        defaults.statement = "PUSHBUTTON"; defaults.id_name = "IDC_SELECTION_KEYS_DEFAULTS";
        defaults.text = "Reset selection keys";
        defaults.rect = {12, top + 142, service.rect.width / 2 - 18, 24};
        defaults.style = {"WS_VISIBLE", "WS_TABSTOP"}; service.controls.push_back(defaults);
        defaults.id_name = "IDC_SELECTION_KEYS_CANCEL"; defaults.text = "Cancel";
        defaults.rect.x = service.rect.width / 2;
        service.controls.push_back(defaults);
        data::DialogControl status;
        status.statement = "LTEXT"; status.id_name = "IDC_SELECTION_KEYS_STATUS";
        status.text = selection_message_.empty() ? "Click a binding, then press a key or mouse button." : selection_message_;
        status.rect = {12, top + 180, service.rect.width - 24, 20};
        status.style = {"WS_VISIBLE"}; service.controls.push_back(status);
    }
    if (page_ == Page::video) {
        // The AA slider's top end is High (the removed Max level folded into High plus supersampling).
        for (auto& control : service.controls) {
            if (control.id_name == "IDC_ANTI_ALIAS_MAX_STATIC") control.text = "High";
        }
        // ST-P2 is an owner-requested addition to the loaded Graphics dialog. The row sits in the free band
        // between the check boxes and the Cancel / Apply / Accept buttons, so the retail layout keeps its size.
        int rows_bottom = 0;
        int buttons_top = service.rect.height;
        for (const auto& control : service.controls) {
            if (control.id_name == "IDC_CANCEL" || control.id_name == "IDC_APPLY" || control.id_name == "IDC_OK") {
                buttons_top = std::min(buttons_top, control.rect.y);
            } else if (control.id_name != "IDC_STATIC_MEDIUM" && control.id_name != "IDC_DEVICE_DESC_STATIC") {
                rows_bottom = std::max(rows_bottom, control.rect.y + control.rect.height);
            }
        }
        const int row_height = 22;
        int row_top = rows_bottom + (buttons_top - rows_bottom - row_height) / 2;
        if (buttons_top - rows_bottom < row_height + 4) {
            // No free band (a mod's dialog is tighter): grow the dialog and move the buttons down.
            const int grow = row_height + 8 - (buttons_top - rows_bottom);
            for (auto& control : service.controls) {
                if (control.rect.y >= buttons_top) control.rect.y += grow;
            }
            service.rect.height += grow;
            row_top = rows_bottom + 4;
        }
        data::DialogControl label;
        label.statement = "LTEXT"; label.id_name = "IDC_FRAME_RATE_LIMIT_LABEL";
        label.text = "Frame-rate limit";
        label.rect = {22, row_top + 2, service.rect.width / 2 - 28, 18};
        label.style = {"WS_VISIBLE"};
        service.controls.push_back(label);
        data::DialogControl combo;
        combo.statement = "COMBOBOX"; combo.id_name = "IDC_FRAME_RATE_LIMIT";
        combo.rect = {264, row_top, service.rect.width - 264 - 14, row_height};
        combo.style = {"WS_VISIBLE", "WS_TABSTOP", "CBS_DROPDOWNLIST"};
        service.controls.push_back(combo);
    }
    if (page_ == Page::advanced_video) {
        // Supersampling is a slider here, one row below the authored detail sliders (the checkboxes and
        // buttons below move down one row). End labels are the first and last step.
        constexpr int row_step = 30;
        int last_slider_top = 0;
        for (const auto& control : service.controls) {
            if (control.id_name == "IDC_ENVIRONMENT_DETAIL_SLIDER") last_slider_top = control.rect.y;
        }
        if (last_slider_top > 0) {
            const int slider_top = last_slider_top + row_step;
            for (auto& control : service.controls) {
                if (control.rect.y >= slider_top - 5) control.rect.y += row_step;
            }
            service.rect.height += row_step;
            data::DialogControl label;
            label.statement = "LTEXT"; label.id_name = "IDC_SUPERSAMPLING_LABEL";
            label.text = "Supersampling"; label.rect = {9, slider_top + 3, 173, 8};
            label.style = {"WS_VISIBLE"}; service.controls.push_back(label);
            label.id_name = "IDC_SUPERSAMPLING_OFF_STATIC"; label.text = "Off"; label.rect = {181, slider_top + 3, 48, 8};
            service.controls.push_back(label);
            label.id_name = "IDC_SUPERSAMPLING_MAX_STATIC"; label.text = "2x"; label.rect = {489, slider_top + 3, 75, 8};
            service.controls.push_back(label);
            data::DialogControl slider;
            slider.statement = "CONTROL"; slider.class_name = "msctls_trackbar32";
            slider.id_name = "IDC_SUPERSAMPLING_SLIDER";
            slider.rect = {230, slider_top, 251, 14};
            slider.style = {"TBS_BOTH", "TBS_NOTICKS", "WS_TABSTOP"};
            service.controls.push_back(slider);
        }
    }
    if (page_ == Page::performance || page_ == Page::tweaks) {
        // Project-owned page; reuse the loaded dialog's skin and button styles.
        data::Dialog performance = service;
        // Performance: four rows end at 178, the footer follows at 192 (Tweaks: rows end at 238).
        performance.rect.height = page_ == Page::tweaks ? 380 : 262;
        performance.controls.clear();
        const int width = performance.rect.width;
        const auto label = [&](const char* id, const char* caption, const data::RcRect rect) {
            data::DialogControl control;
            control.statement = "LTEXT"; control.id_name = id; control.text = caption;
            control.rect = rect; control.style = {"WS_VISIBLE"};
            performance.controls.push_back(std::move(control));
        };
        data::DialogControl combo;
        combo.statement = "COMBOBOX";
        combo.style = {"WS_VISIBLE", "WS_TABSTOP", "CBS_DROPDOWNLIST"};
        if (page_ == Page::performance) {
            label("IDC_PERFORMANCE_TITLE", "TEXT_EAWR_PERFORMANCE", {12, 12, width - 24, 24});
            label("IDC_PARTICLE_THREADS_LABEL", "TEXT_EAWR_PARTICLE_THREADS", {12, 52, width / 2 - 18, 24});
            combo.id_name = "IDC_PARTICLE_THREADS";
            combo.rect = {width / 2, 50, width / 2 - 12, 26};
            performance.controls.push_back(combo);
            label("IDC_SIMULATION_THREADS_LABEL", "TEXT_EAWR_SIMULATION_THREADS", {12, 86, width / 2 - 18, 24});
            combo.id_name = "IDC_SIMULATION_THREADS";
            combo.rect.y = 84;
            performance.controls.push_back(combo);
            label("IDC_DEPTH_PREPASS_LABEL", "TEXT_EAWR_DEPTH_PREPASS", {12, 120, width / 2 - 18, 24});
            combo.id_name = "IDC_DEPTH_PREPASS";
            combo.rect.y = 118;
            performance.controls.push_back(combo);
            label("IDC_RENDER_THREAD_LABEL", "TEXT_EAWR_RENDER_THREAD", {12, 154, width / 2 - 18, 24});
            combo.id_name = "IDC_RENDER_THREAD";
            combo.rect = {width / 2, 152, width / 2 - 12, 26};
            performance.controls.push_back(combo);
        } else {
            label("IDC_TWEAKS_TITLE", "TEXT_EAWR_TWEAKS", {12, 12, width - 24, 24});
            label("IDC_SELECTION_HOTKEYS_LABEL", "TEXT_EAWR_SELECTION_HOTKEYS", {12, 52, width / 2 - 18, 24});
            combo.id_name = "IDC_SELECTION_HOTKEYS";
            combo.rect = {width / 2, 50, width / 2 - 12, 26};
            performance.controls.push_back(combo);
            data::DialogControl check;
            check.statement = "CONTROL"; check.class_name = "Button";
            check.style = {"WS_VISIBLE", "WS_TABSTOP", "BS_AUTOCHECKBOX"};
            check.id_name = "IDC_HOVER_OWNER_NAMES"; check.text = "TEXT_EAWR_HOVER_OWNER_NAMES";
            check.rect = {12, 88, width - 24, 24}; performance.controls.push_back(check);
            check.id_name = "IDC_REINFORCEMENT_AREAS_ONLY_WHILE_PLACING";
            check.text = "TEXT_EAWR_REINFORCEMENT_AREAS"; check.rect.y = 120;
            performance.controls.push_back(check);
            check.id_name = "IDC_ANIMATED_LOADING_BACKGROUND";
            check.text = "TEXT_EAWR_ANIMATED_LOADING_BACKGROUND"; check.rect.y = 150;
            performance.controls.push_back(check);
            check.id_name = "IDC_FIT_RADAR_TO_GALAXY";
            check.text = "TEXT_EAWR_FIT_RADAR_TO_GALAXY"; check.rect.y = 182;
            performance.controls.push_back(check);
            check.id_name = "IDC_SPACE_PAUSES_GALACTIC";
            check.text = "TEXT_EAWR_SPACE_PAUSES_GALACTIC"; check.rect.y = 214;
            performance.controls.push_back(check);
            check.id_name = "IDC_GALACTIC_BUILD_TRAY";
            check.text = "TEXT_EAWR_GALACTIC_BUILD_TRAY"; check.rect.y = 246;
            performance.controls.push_back(check);
            int colours_y = 278;
            int colours_label_y = 280;
            if (auto layout = eawr::data::load_document(*resources.filesystem, "data/xml/RevisedTweaks.xml")) {
                for (const auto& node : layout.value().root.children) {
                    int y{};
                    const auto result = std::from_chars(node.raw_text.data(), node.raw_text.data() + node.raw_text.size(), y);
                    if (result.ec != std::errc{} || result.ptr != node.raw_text.data() + node.raw_text.size() || y < 0 || y > 300) continue;
                    if (node.name == "IDC_GC_BATTLE_COLOURS") colours_y = y;
                    if (node.name == "IDC_GC_BATTLE_COLOURS_LABEL") colours_label_y = y;
                }
            }
            label("IDC_GC_BATTLE_COLOURS_LABEL", "TEXT_EAWR_GC_BATTLE_COLOURS", {12, colours_label_y, width / 2 - 18, 24});
            combo.id_name = "IDC_GC_BATTLE_COLOURS";
            combo.rect = {width / 2, colours_y, width / 2 - 12, 26};
            performance.controls.push_back(combo);
        }

        const auto button = [&](const char* id, const char* caption, const int column, const int row) {
            const auto authored = std::find_if(service.controls.begin(), service.controls.end(),
                                               [id](const auto& control) { return control.id_name == id; });
            data::DialogControl control;
            if (authored != service.controls.end()) control = *authored;
            else {
                const auto prototype = std::find_if(service.controls.begin(), service.controls.end(), [](const auto& c) {
                    return c.statement == "PUSHBUTTON" || c.statement == "DEFPUSHBUTTON";
                });
                if (prototype != service.controls.end()) control = *prototype;
                else { control.statement = "PUSHBUTTON"; control.style = {"WS_VISIBLE", "WS_TABSTOP"}; }
            }
            control.id_name = id; control.id.reset();
            if (caption != nullptr) control.text = caption;
            const int button_width = (width - 36) / 2;
            control.rect = {12 + column * (button_width + 12), (page_ == Page::tweaks ? 312 : 192) + row * 30, button_width, 24};
            performance.controls.push_back(std::move(control));
        };
        button("IDC_RESTORE_DEFAULT", "TEXT_EAWR_DEFAULTS", 0, 0);
        button("IDC_APPLY", "TEXT_EAWR_APPLY", 1, 0);
        button("IDC_CANCEL", "TEXT_BUTTON_CANCEL", 0, 1);
        button("IDC_OK", "TEXT_EAWR_ACCEPT", 1, 1);
        service = std::move(performance);
    }
    if (page_ == Page::network) {
        // Keep every authored control in place. Extend the loaded dialog with a
        // service column beside the retail network controls.
        const int width = service.rect.width, height = service.rect.height;
        service.rect.width += width * 2 / 3;
        const auto add = [&](const char* kind, const char* id, const char* caption, int row, bool label) {
            data::DialogControl control;
            control.statement = kind; control.id_name = id;
            if (caption) control.text = caption;
            control.rect = {width + width / 24, height * (14 + row * 10) / 100 + (label ? 0 : height * 3 / 100),
                            width * 7 / 12, height * (label ? 3 : 5) / 100};
            control.style = {"WS_VISIBLE", "WS_TABSTOP"};
            service.controls.push_back(std::move(control));
        };
        const std::array<std::pair<const char*, const char*>, 5> edits{{
            {"IDC_ONLINE_SERVICE_HOST", "Online service host"}, {"IDC_ONLINE_SERVICE_PORT", "Service port"},
            {"IDC_ONLINE_SERVICE_PIN", "Certificate SHA-256 pin"}, {"IDC_ONLINE_SERVICE_CA", "CA certificate file"},
            {"IDC_ONLINE_SERVICE_TRUST", "Trust: system, pin or ca-file"}}};
        int row = 0;
        for (const auto& [id, caption] : edits) {
            const std::string label = std::string(id) + "_LABEL";
            add("LTEXT", label.c_str(), caption, row, true); add("EDITTEXT", id, nullptr, row++, false);
        }
        add("CONTROL", "IDC_ONLINE_RELAY_ONLY", "Hide my IP: always use a relay", row++, true);
        service.controls.back().class_name = "Button";
        service.controls.back().style.push_back("BS_AUTOCHECKBOX");
        add("LTEXT", "IDC_ONLINE_SERVICE_ERROR", "", row, true);
        service.controls.back().rect.height = height / 6;
    }
    state.dialog.emplace(build_dialog(service, *resources.theme_model, resources.text ? &*resources.text : nullptr, placement, page_ == Page::network || page_ == Page::video || page_ == Page::performance || page_ == Page::tweaks || page_ == Page::keyboard));
    root->add_child(state.dialog->frame);
    for (BuiltGadget& gadget : state.dialog->gadgets) {
        if (gadget.control == nullptr) continue;
        state.controls[gadget.id] = gadget.control;
        if (page_ == Page::network && gadget.kind == "label") {
            // Preserve authored label rectangles. Fit each caption to its row so wrapping cannot
            // paint the next row; service validation alone has an explicitly multiline area.
            if (auto* label = Object::cast_to<EawrUiLabel>(gadget.control)) {
                const bool multiline = gadget.id == "IDC_ONLINE_SERVICE_ERROR";
                label->set_wrap(multiline);
                if (!multiline) {
                    const auto index = static_cast<std::size_t>(&gadget - state.dialog->gadgets.data());
                    const std::string role(data::to_string(data::font_role(service.controls[index])));
                    const KitTextStyle style = kit_text_style(*label, role);
                    if (style.font.is_valid() && style.size > 0) {
                        const Vector2 bounds = label->get_size();
                        int caption_size = style.size;
                        while (caption_size > 1) {
                            const Vector2 measured = style.font->get_string_size(label->get_text(), HORIZONTAL_ALIGNMENT_LEFT, -1, caption_size);
                            if (measured.x <= bounds.x && measured.y <= bounds.y) break;
                            --caption_size;
                        }
                        label->add_theme_font_size_override(text(role), caption_size);
                        label->add_theme_constant_override(text(role + "_cell_ascent"), style.cell_ascent * caption_size / style.size);
                        label->add_theme_constant_override(text(role + "_cell_descent"), style.cell_descent * caption_size / style.size);
                    }
                }
                label->set_clip_contents(true);
            }
        }
        if (page_ == Page::network && gadget.kind == "edit") {
            // Native edits otherwise retain a font-driven minimum taller than a compact
            // authored row. Fit the edit to that row; neighbouring labels stay untouched.
            const KitTextStyle style = kit_text_style(*gadget.control, "Edit_Box");
            for (int caption_size = style.size - 1; caption_size > 0; --caption_size) {
                if (gadget.control->get_combined_minimum_size().y <= gadget.rect.height) break;
                gadget.control->add_theme_font_size_override("Edit_Box", caption_size);
                gadget.control->update_minimum_size();
            }
            gadget.control->set_size(Vector2(static_cast<float>(gadget.rect.width), static_cast<float>(gadget.rect.height)));
        }
        // Options Tweaks supports ordinary keyboard focus as well as pointer input.
        if ((page_ == Page::tweaks && (gadget.kind == "button" || gadget.kind == "check" || gadget.kind == "combo"))
            || (page_ == Page::hub && (gadget.id == "IDC_MAIN_OPTIONS_TWEAKS" || gadget.id == "IDC_IN_GAME_OPTIONS_TWEAKS"))) {
            gadget.control->set_focus_mode(Control::FOCUS_ALL);
            // A thin outline makes keyboard focus visible without changing the shared kit.
            auto* outline = memnew(Control);
            outline->set_name("TweaksFocusOutline");
            outline->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            gadget.control->add_child(outline);
            outline->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
            outline->set_visible(false);
            state.focus_outlines[gadget.id] = outline;
            for (int edge = 0; edge < 4; ++edge) {
                auto* line = memnew(ColorRect);
                line->set_color(Color(1.0F, 0.85F, 0.3F, 1.0F));
                line->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                outline->add_child(line);
                const bool horizontal = edge < 2;
                line->set_anchor(SIDE_LEFT, edge == 3 ? 1.0F : 0.0F);
                line->set_anchor(SIDE_TOP, edge == 1 ? 1.0F : 0.0F);
                line->set_anchor(SIDE_RIGHT, horizontal || edge == 3 ? 1.0F : 0.0F);
                line->set_anchor(SIDE_BOTTOM, !horizontal || edge == 1 ? 1.0F : 0.0F);
                line->set_offset(SIDE_LEFT, edge == 3 ? -2.0F : 0.0F);
                line->set_offset(SIDE_TOP, edge == 1 ? -2.0F : 0.0F);
                line->set_offset(SIDE_RIGHT, edge == 2 ? 2.0F : 0.0F);
                line->set_offset(SIDE_BOTTOM, edge == 0 ? 2.0F : 0.0F);
            }
            gadget.control->connect("focus_entered", callable_mp(this, &EawrOptionsScreen::on_focus_changed).bind(text(gadget.id), true));
            gadget.control->connect("focus_exited", callable_mp(this, &EawrOptionsScreen::on_focus_changed).bind(text(gadget.id), false));
        }
        if (hidden_gadget(gadget.id)) gadget.control->set_visible(false);
        // A group box is a backdrop: the kit paints its background, so one declared after the
        // controls it holds would cover them. Drawn first, it sits behind them as retail's does.
        if (gadget.kind == "group") state.dialog->frame->move_child(gadget.control, 0);
    }
    if (page_ == Page::performance) {
        // The panel is authored tall enough for every row, so the rows show without a scroll bar. The
        // scroll container stays as the fallback: when the live controls are taller than the authored
        // area (a thick skin) the panel grows first, and only a screen too short to hold it scrolls.
        Control* frame = state.dialog->frame;
        const float scale = frame->get_size().y / static_cast<float>(service.rect.height);
        auto* scroll = memnew(ScrollContainer);
        state.performance_scroll = scroll;
        scroll->set_name("PerformanceRows");
        scroll->set_position(Vector2(0.0F, 42.0F * scale));
        const Control* footer = state.controls["IDC_RESTORE_DEFAULT"];
        const float footer_top = footer != nullptr ? footer->get_position().y : 132.0F * scale;
        const float rows_height = std::max(0.0F, footer_top - 8.0F * scale - scroll->get_position().y);
        scroll->set_size(Vector2(frame->get_size().x, rows_height));
        scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
        frame->add_child(scroll);
        auto* rows = memnew(Control);
        scroll->add_child(rows);
        float content_bottom = 0.0F;
        for (const char* id : {"IDC_PARTICLE_THREADS_LABEL", "IDC_PARTICLE_THREADS", "IDC_SIMULATION_THREADS_LABEL",
                               "IDC_SIMULATION_THREADS", "IDC_DEPTH_PREPASS_LABEL", "IDC_DEPTH_PREPASS",
                               "IDC_RENDER_THREAD_LABEL", "IDC_RENDER_THREAD"}) {
            if (Control* control = state.controls[id]) {
                const Vector2 pos = control->get_position() - scroll->get_position();
                control->reparent(rows);
                control->set_position(pos);
                content_bottom = std::max(content_bottom, pos.y + control->get_size().y);
            }
        }
        rows->set_custom_minimum_size(Vector2(frame->get_size().x - 20.0F * scale, content_bottom));
        const float shortfall = content_bottom - rows_height;
        if (shortfall > 0.0F) {
            const float room = std::max(0.0F, size.y * 0.96F - frame->get_size().y);
            const float grow = std::min(shortfall, room);
            if (grow > 0.0F) {
                for (const char* id : {"IDC_RESTORE_DEFAULT", "IDC_APPLY", "IDC_CANCEL", "IDC_OK"}) {
                    if (Control* button = state.controls[id]) button->set_position(button->get_position() + Vector2(0.0F, grow));
                }
                frame->set_size(Vector2(frame->get_size().x, frame->get_size().y + grow));
                frame->set_position(Vector2(frame->get_position().x, (size.y - frame->get_size().y) / 2.0F));
                scroll->set_size(Vector2(frame->get_size().x, rows_height + grow));
            }
        }
        scroll->call_deferred("set_v_scroll", performance_scroll_);
    }
    wire_page();
    if (page_ == Page::keyboard) {
        // Fit the footer's live skin height, which can exceed its authored rectangle.
        Control* frame = state.dialog->frame;
        float required = frame->get_size().y;
        for (const char* id : {"IDC_SELECTION_KEYS_DEFAULTS", "IDC_SELECTION_KEYS_CANCEL"}) {
            if (Control* control = state.controls[id]) {
                required = std::max(required, control->get_position().y + control->get_size().y + 8.0F);
            }
        }
        if (required > frame->get_size().y) {
            frame->set_size(Vector2(frame->get_size().x, required));
            frame->set_position(Vector2(frame->get_position().x, (size.y - frame->get_size().y) / 2.0F));
        }
    }
    if (page_ == Page::performance || page_ == Page::tweaks) {
        // A skin can make live buttons taller than their authored resource rectangles.
        Control* buttons[] = {state.controls["IDC_RESTORE_DEFAULT"], state.controls["IDC_APPLY"],
                              state.controls["IDC_CANCEL"], state.controls["IDC_OK"]};
        if (buttons[0] && buttons[1] && buttons[2] && buttons[3]) {
            float row_height = 0.0F;
            for (Control* control : buttons) row_height = std::max(row_height, control->get_size().y);
            const float top = buttons[0]->get_position().y;
            const float gap = std::max(6.0F, row_height / 5.0F);
            for (int index = 0; index < 4; ++index) {
                Control* control = buttons[index];
                Vector2 button_size = control->get_size();
                button_size.y = row_height;
                control->set_size(button_size);
                control->set_position(Vector2(control->get_position().x,
                    top + static_cast<float>(index / 2) * (row_height + gap)));
            }
            Control* frame = state.dialog->frame;
            const float required = top + 2.0F * (row_height + gap);
            if (required > frame->get_size().y) {
                frame->set_size(Vector2(frame->get_size().x, required));
                frame->set_position(Vector2(frame->get_position().x, (size.y - frame->get_size().y) / 2.0F));
            }
        }
    }
    if (page_ == Page::hub) {
        // Authored rectangles can be shorter than the skin's minimum button height.
        // Lay out the live controls, including additions, rather than scanning for
        // empty authored rectangles (ST-N2). Keep mod entries in their visual order.
        std::vector<Control*> column;
        Control* back = nullptr;
        const char* back_id = options_.context == Context::main_menu ? "IDC_OK_MAIN" : "IDC_OK_IN_GAME";
        for (const BuiltGadget& gadget : state.dialog->gadgets) {
            if (gadget.kind != "button" || !gadget.control || !gadget.control->is_visible()) continue;
            if (gadget.id == back_id) back = gadget.control;
            else column.push_back(gadget.control);
        }
        if (back && !column.empty()) {
            std::stable_sort(column.begin(), column.end(), [](const Control* a, const Control* b) {
                return a->get_position().y < b->get_position().y;
            });
            column.push_back(back);
            const float top = column.front()->get_position().y;
            float heights = 0.0F;
            float gap = std::max(0.0F, column[1]->get_position().y - top - column.front()->get_size().y);
            for (Control* control : column) heights += control->get_size().y;
            const float available = state.dialog->frame->get_size().y - top;
            gap = std::min(gap, std::max(0.0F, (available - heights) / static_cast<float>(column.size() - 1)));
            float y = std::min(top, std::max(0.0F, state.dialog->frame->get_size().y - heights
                                                - gap * static_cast<float>(column.size() - 1)));
            for (Control* control : column) {
                control->set_position(Vector2(control->get_position().x, y));
                y += control->get_size().y + gap;
            }
        }
    }
}

void EawrOptionsScreen::disable(Control& control, const std::string& id, const std::string& reason) {
    state_->reasons[id] = reason;
    const String tip = text(reason);
    control.set_tooltip_text(tip);
    if (auto* button = Object::cast_to<BaseButton>(&control)) {
        button->set_disabled(true);
        if (auto* combo = Object::cast_to<EawrUiCombo>(&control)) combo->set_forced_state(KitState::disabled);
        return;
    }
    if (auto* edit = Object::cast_to<LineEdit>(&control)) {
        edit->set_editable(false);
        return;
    }
    if (auto* slider = Object::cast_to<EawrUiSlider>(&control)) {
        slider->set_forced_states(KitState::disabled, KitState::disabled);
    }
    // A slider or a list has no disabled state of its own: a transparent control over it takes the
    // pointer and carries the tooltip.
    control.set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    auto* overlay = memnew(Control);
    overlay->set_name(text(id + "_Disabled"));
    overlay->set_position(control.get_position());
    overlay->set_size(control.get_size());
    overlay->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    overlay->set_tooltip_text(tip);
    if (Node* parent = control.get_parent()) parent->add_child(overlay);
}

void EawrOptionsScreen::update_performance_dirty() {
    dirty_ = draft_.video.render_thread != committed_.video.render_thread
        || draft_.video.particle_threads != committed_.video.particle_threads
        || draft_.video.simulation_threads != committed_.video.simulation_threads
        || draft_.video.depth_prepass != committed_.video.depth_prepass;
    if (auto* apply = Object::cast_to<BaseButton>(state_->controls["IDC_APPLY"])) {
        apply->set_disabled(!dirty_);
        const bool depth_only = draft_.video.depth_prepass != committed_.video.depth_prepass
            && draft_.video.render_thread == committed_.video.render_thread
            && draft_.video.particle_threads == committed_.video.particle_threads
            && draft_.video.simulation_threads == committed_.video.simulation_threads;
        apply->set_tooltip_text(text(!dirty_ ? "Nothing to apply yet."
            : depth_only ? "Depth pre-pass takes effect after restarting the game."
                         : "Changes take effect in the next battle or after restarting the game."));
    }
}

void EawrOptionsScreen::wire_page() {
    PageState& state = *state_;
    loading_ = true;
    for (BuiltGadget& gadget : state.dialog->gadgets) {
        Control* control = gadget.control;
        if (control == nullptr) continue;
        const String id = text(gadget.id);
        if (gadget.kind == "button") {
            control->connect("pressed", callable_mp(this, &EawrOptionsScreen::on_pressed).bind(id));
        } else if (gadget.kind == "check" || gadget.kind == "radio") {
            control->connect("toggled", callable_mp(this, &EawrOptionsScreen::on_toggled).bind(id));
        } else if (gadget.kind == "slider") {
            control->connect("value_changed", callable_mp(this, &EawrOptionsScreen::on_value_changed).bind(id));
        } else if (gadget.kind == "list" || gadget.kind == "combo") {
            control->connect("item_selected", callable_mp(this, &EawrOptionsScreen::on_item_selected).bind(id));
        }
        // The retail controls this build cannot honour show disabled, with their reason.
        if (const model::OptionStatus* status = model::find_option_status(gadget.id);
            status != nullptr && !status->honoured) {
            disable(*control, gadget.id, std::string(status->reason));
        }
    }
    const auto slider = [&](const char* id) { return Object::cast_to<EawrUiSlider>(state.controls[id]); };
    const auto check = [&](const char* id) { return Object::cast_to<EawrUiCheck>(state.controls[id]); };
    const auto has = [&](const char* id) { return state.controls.count(id) != 0 && state.controls[id] != nullptr; };

    switch (page_) {
    case Page::hub:
        if (has("IDC_MAIN_OPTIONS_MODS")) {
            Control* mods = state.controls["IDC_MAIN_OPTIONS_MODS"];
            // Retail shows the entry unless a mod removes the mods dialog (MNU-10). Its caption is the
            // executable's built-in "Mods"; the page itself is a separate feature, so it is disabled.
            if (resources_->catalog->script.find("IDD_SELECT_MOD_DIALOG") == nullptr) mods->set_visible(false);
            else disable(*mods, "IDC_MAIN_OPTIONS_MODS", "The mods page is not built yet.");
        }
        break;
    case Page::game_menu:
        // MNU-19: Save and Load are available when the battle supplies their callbacks.
        for (const char* id : {"IDC_SAVE_GAME", "IDC_LOAD_GAME"}) {
            const bool works = std::string_view(id) == "IDC_SAVE_GAME"
                ? bool(options_.save_game && options_.save_names) : bool(options_.load_game);
            if (has(id) && !works) disable(*state.controls[id], id, "");
        }
        break;
    case Page::save_game:
        // #3989: the retail Save dialog. Its list holds the saves of the folder; a row's name fills the name field.
        if (has("IDC_CLOSE_BUTTON")) state.controls["IDC_CLOSE_BUTTON"]->set_visible(false);
        if (has("IDC_DELETE_GAME_BUTTON")) disable(*state.controls["IDC_DELETE_GAME_BUTTON"], "IDC_DELETE_GAME_BUTTON", "");
        if (options_.save_names) state.saves = options_.save_names();
        if (has("IDC_SAVE_GAME_LIST")) {
            if (auto* list = Object::cast_to<EawrUiList>(state.controls["IDC_SAVE_GAME_LIST"])) {
                for (const std::string& name : state.saves) list->add_item(text(name));
            }
        }
        if (has("IDC_SAVE_GAME_NAME_EDIT")) {
            if (auto* edit = Object::cast_to<LineEdit>(state.controls["IDC_SAVE_GAME_NAME_EDIT"])) {
                edit->set_max_length(64); // the save name rule (1 to 64 characters)
                edit->set_editable(true);
            }
        }
        break;
    case Page::audio: {
        const model::AudioSettings& audio = draft_.audio;
        const std::array<std::pair<const char*, int>, 4> sliders{{
            {"IDC_SLIDER_MASTER_VOLUME", audio.master_percent},
            {"IDC_SLIDER_SOUND_EFFECTS_VOLUME", audio.sfx_percent},
            {"IDC_SLIDER_DIALOG_VOLUME", audio.speech_percent},
            {"IDC_SLIDER_MUSIC_VOLUME", audio.music_percent},
        }};
        for (const auto& [id, percent] : sliders) {
            if (auto* control = has(id) ? slider(id) : nullptr) {
                control->set_min(0.0);
                control->set_max(100.0);
                control->set_step(1.0);
                control->set_value_no_signal(percent);
            }
        }
        if (has("IDC_USE_MOVIE_SUBTITLES")) check("IDC_USE_MOVIE_SUBTITLES")->set_pressed_no_signal(audio.subtitles);
        break;
    }
    case Page::video: {
        const model::VideoSettings& video = draft_.video;
        if (has("IDC_FRAME_RATE_LIMIT")) {
            auto* combo = Object::cast_to<EawrUiCombo>(state.controls["IDC_FRAME_RATE_LIMIT"]);
            const std::array<int, 9> choices{0, -1, 30, 60, 90, 120, 144, 165, 240};
            int selected = -1;
            for (std::size_t i = 0; i < choices.size(); ++i) {
                const int fps = choices[i];
                combo->add_item(text(fps == 0 ? "Off" : fps < 0 ? "Display refresh rate" : std::to_string(fps)));
                if (fps == video.frame_rate_limit) selected = static_cast<int>(i);
            }
            if (selected < 0) {
                selected = static_cast<int>(choices.size());
                combo->add_item(text(std::to_string(video.frame_rate_limit)));
            }
            combo->select(selected);
        }
        state.windowed_draft = video.windowed;
        state.selected_size = video.windowed ? model::DisplaySize{video.window_width, video.window_height}
                                             : model::DisplaySize{video.fullscreen_width, video.fullscreen_height};
        const DisplayFacts facts = display_facts();
        const std::array<model::DisplaySize, 3> extras{
            model::DisplaySize{video.window_width, video.window_height},
            model::DisplaySize{video.fullscreen_width, video.fullscreen_height}, facts.window};
        state.sizes = model::resolution_choices(facts.screen, extras);
        if (has("IDC_RESOLUTION_LIST")) {
            auto* list = Object::cast_to<EawrUiList>(state.controls["IDC_RESOLUTION_LIST"]);
            for (const model::DisplaySize& size : state.sizes) {
                list->add_item(text(std::to_string(size.width) + " x " + std::to_string(size.height)));
            }
            for (std::size_t index = 0; index < state.sizes.size(); ++index) {
                if (state.sizes[index] == state.selected_size) list->select(static_cast<int32_t>(index));
            }
        }
        if (has("IDC_WINDOWED_MODE")) check("IDC_WINDOWED_MODE")->set_pressed_no_signal(video.windowed);
        if (has("IDC_VSYNC_ENABLED")) check("IDC_VSYNC_ENABLED")->set_pressed_no_signal(video.vsync);
        if (has("IDC_HARDWARE_MOUSE")) check("IDC_HARDWARE_MOUSE")->set_pressed_no_signal(video.hardware_cursor);
        if (has("IDC_WIDESCREEN")) check("IDC_WIDESCREEN")->set_pressed_no_signal(video.widescreen);
        if (auto* aa = has("IDC_ANTI_ALIAS_SLIDER") ? slider("IDC_ANTI_ALIAS_SLIDER") : nullptr) {
            // ST-P1: four AA levels, Off / Low / Medium / High.
            const RenderProfile shown = resolve_render_profile(video.render_profile, active_render_profile());
            aa->set_tooltip_text("Off / Low / Medium / High");
            aa->set_min(0.0);
            aa->set_max(static_cast<double>(aa_slider_profiles.size() - 1));
            aa->set_step(1.0);
            aa->set_value_no_signal(aa_slider_index(shown));
        }
        if (auto* detail = has("IDC_DETAIL_LEVEL_SLIDER") ? slider("IDC_DETAIL_LEVEL_SLIDER") : nullptr) {
            detail->set_min(0.0);
            detail->set_max(model::detail_level_maximum);
            detail->set_step(1.0);
            detail->set_value_no_signal(video.detail_level);
        }
        if (auto* gamma = has("IDC_GAMMA_SLIDER") ? slider("IDC_GAMMA_SLIDER") : nullptr) {
            gamma->set_min(0.0);
            gamma->set_max(4000.0);
            gamma->set_value_no_signal(video.gamma_permille);
        }
        if (has("IDC_APPLY")) {
            auto* apply = Object::cast_to<BaseButton>(state.controls["IDC_APPLY"]);
            apply->set_disabled(true); // enabled by the first change, as retail's dirty flag does
            apply->set_tooltip_text(text("Nothing to apply yet."));
        }
        break;
    }
    case Page::performance: {
        const auto add_threads = [&](const char* id, const auto& choices, const int saved) {
            auto* combo = Object::cast_to<EawrUiCombo>(state.controls[id]);
            int selected = -1;
            for (std::size_t i = 0; i < choices.size(); ++i) {
                const int count = choices[i];
                combo->add_item(count == 0 ? text(resources_->text ? data::to_utf8(resources_->text->text("TEXT_EAWR_AUTOMATIC")) : "Automatic") : text(std::to_string(count)));
                if (count == saved) selected = static_cast<int>(i);
            }
            if (selected < 0) { selected = static_cast<int>(choices.size()); combo->add_item(text(std::to_string(saved))); }
            combo->select(selected);
            combo->set_tooltip_text(text(std::string(id) == "IDC_SIMULATION_THREADS"
                ? "For skirmish and multiplayer battles, not galactic conquest. Takes effect in the next battle. Launch flags override this setting."
                : "Takes effect in the next battle. Launch flags override this setting."));
        };
        add_threads("IDC_PARTICLE_THREADS", std::array{0, 1, 2, 4, 6, 8}, draft_.video.particle_threads);
        add_threads("IDC_SIMULATION_THREADS", std::array{0, 2, 4, 6, 8, 12, 16}, draft_.video.simulation_threads);
        if (has("IDC_DEPTH_PREPASS")) {
            auto* depth = Object::cast_to<EawrUiCombo>(state.controls["IDC_DEPTH_PREPASS"]);
            depth->add_item(text("On"));
            depth->add_item(text("Off"));
            depth->select(draft_.video.depth_prepass ? 0 : 1);
            depth->set_tooltip_text(text("Takes effect after restarting the game. Launch flags override this setting."));
        }
        if (has("IDC_RENDER_THREAD")) {
            auto* combo = Object::cast_to<EawrUiCombo>(state.controls["IDC_RENDER_THREAD"]);
            combo->add_item(text(resources_->text ? data::to_utf8(resources_->text->text("TEXT_EAWR_SEPARATE")) : "Separate (default)"));
            if (draft_.video.render_thread == model::RenderThreadSetting::safe) combo->add_item(text("Safe (previous setting)"));
            combo->select(draft_.video.render_thread == model::RenderThreadSetting::safe ? 1 : 0);
            combo->set_tooltip_text(text("Takes effect after restart. Launch flags and mod profiles override this setting."));
        }
        if (has("IDC_APPLY")) {
            auto* apply = Object::cast_to<BaseButton>(state.controls["IDC_APPLY"]);
            apply->set_disabled(true);
            apply->set_tooltip_text(text("Nothing to apply yet."));
        }
        break;
    }
    case Page::tweaks: {
        if (auto* combo = Object::cast_to<EawrUiCombo>(state.controls["IDC_GC_BATTLE_COLOURS"])) {
            combo->add_item(text(resources_->text ? data::to_utf8(resources_->text->text("TEXT_EAWR_GC_FACTION_UNITS")) : "Faction units, player icons"));
            combo->add_item(text(resources_->text ? data::to_utf8(resources_->text->text("TEXT_EAWR_GC_PLAYER_UNITS")) : "Player-coloured units"));
            combo->select(draft_.gameplay.gc_battle_colours);
            combo->set_tooltip_text(text(resources_->text ? data::to_utf8(resources_->text->text("TEXT_EAWR_GC_COLOURS_TOOLTIP")) : "This choice applies only to your view."));
        }
        if (auto* combo = Object::cast_to<EawrUiCombo>(state.controls["IDC_SELECTION_HOTKEYS"])) {
            combo->add_item(text(resources_->text ? data::to_utf8(resources_->text->text("TEXT_EAWR_OFF")) : "TEXT_EAWR_OFF"));
            combo->add_item(text(resources_->text ? data::to_utf8(resources_->text->text("TEXT_EAWR_ON")) : "TEXT_EAWR_ON"));
            combo->select(draft_.selection_hotkeys ? 1 : 0);
            combo->set_tooltip_text(text("Select own mobile units without heroes. Bind keys under Controls."));
        }
        if (has("IDC_FIT_RADAR_TO_GALAXY")) check("IDC_FIT_RADAR_TO_GALAXY")->set_pressed_no_signal(draft_.gameplay.fit_radar_to_galaxy);
        if (has("IDC_SPACE_PAUSES_GALACTIC")) check("IDC_SPACE_PAUSES_GALACTIC")->set_pressed_no_signal(draft_.gameplay.space_pauses_galactic);
        if (has("IDC_GALACTIC_BUILD_TRAY")) check("IDC_GALACTIC_BUILD_TRAY")->set_pressed_no_signal(model::galactic_build_tray_enabled(draft_.gameplay, draft_.galactic_build_tray_layer_default));
        if (has("IDC_ANIMATED_LOADING_BACKGROUND")) check("IDC_ANIMATED_LOADING_BACKGROUND")->set_pressed_no_signal(draft_.gameplay.animated_loading_background);
        if (has("IDC_HOVER_OWNER_NAMES")) check("IDC_HOVER_OWNER_NAMES")->set_pressed_no_signal(model::hover_owner_names_enabled(draft_.gameplay, options_.multiplayer));
        if (has("IDC_REINFORCEMENT_AREAS_ONLY_WHILE_PLACING")) check("IDC_REINFORCEMENT_AREAS_ONLY_WHILE_PLACING")->set_pressed_no_signal(draft_.gameplay.reinforcement_areas_only_while_placing);
        if (auto* apply = Object::cast_to<BaseButton>(state.controls["IDC_APPLY"])) apply->set_disabled(true);
        break;
    }
    case Page::advanced_video: {
        const model::VideoSettings& video = draft_.video;
        const std::array<std::pair<const char*, std::size_t>, 7> sliders{{
            {"IDC_TEXTURE_MIP_DETAIL_SLIDER", 0},
            {"IDC_SHADER_DETAIL_SLIDER", 1},
            {"IDC_WATER_DETAIL_SLIDER", 2},
            {"IDC_SHADOW_DETAIL_SLIDER", 9},
            {"IDC_PARTICLE_DETAIL_SLIDER", 7},
            {"IDC_MESH_DETAIL_SLIDER", 8},
            {"IDC_ENVIRONMENT_DETAIL_SLIDER", 10},
        }};
        for (const auto& [id, slot] : sliders) {
            if (auto* control = has(id) ? slider(id) : nullptr) {
                control->set_min(0.0);
                control->set_max(model::detail_level_maximum);
                control->set_step(1.0);
                // The particle slider shows what a battle will draw: automatic is the top level (1.0).
                const int automatic = slot == model::detail_slot_particle ? model::detail_level_maximum : video.detail_level;
                const int value = video.detail[slot] == model::detail_automatic ? automatic : video.detail[slot];
                control->set_value_no_signal(value);
                if (slot == model::detail_slot_particle) {
                    control->set_tooltip_text(text("Our scale, not retail's: 0 = 40%, 1 = 60%, 2 = 80%, 3 = 100% of the particles "
                                                   "drawn. Takes effect when the next battle starts. A launch flag overrides it."));
                }
            }
        }
        const std::array<std::pair<const char*, std::size_t>, 3> checks{{
            {"IDC_SOFT_SHADOWS", 4}, {"IDC_HEAT_DISTORTIONS", 5}, {"IDC_BLOOM", 6}}};
        for (const auto& [id, slot] : checks) {
            if (has(id)) {
                // Heat distortions are on unless switched off: automatic keeps today's look.
                check(id)->set_pressed_no_signal(slot == model::detail_slot_heat ? model::particle_heat_enabled(video)
                    : video.detail[slot] == model::detail_automatic ? video.detail_level > 0 : video.detail[slot] > 0);
                if (slot == model::detail_slot_heat) {
                    check(id)->set_tooltip_text(text("Takes effect when the next battle starts. A launch flag overrides it."));
                }
            }
        }
        if (auto* ss = has("IDC_SUPERSAMPLING_SLIDER") ? slider("IDC_SUPERSAMPLING_SLIDER") : nullptr) {
            ss->set_min(0.0);
            ss->set_max(static_cast<double>(model::supersampling_scales.size() - 1));
            ss->set_step(1.0);
            ss->set_value_no_signal(video.supersampling);
            ss->set_tooltip_text(text("Off / 1.5x / 2x. Applies when you press Apply or Accept."));
        }
        // Apply works for the particle slider, heat distortions and supersampling; the other controls stay inert.
        if (has("IDC_APPLY")) {
            auto* apply = Object::cast_to<BaseButton>(state.controls["IDC_APPLY"]);
            apply->set_disabled(true);
            apply->set_tooltip_text(text("Nothing to apply yet."));
        }
        break;
    }
    case Page::gameplay: {
        const model::GameplaySettings& play = draft_.gameplay;
        const std::array<std::pair<const char*, int>, 3> sliders{{
            {"IDC_GAME_SPEED_SLIDER", play.tactical_speed},
            {"IDC_STRATEGIC_GAME_SPEED_SLIDER", play.strategic_speed},
            {"IDC_SCROLL_SPEED_SLIDER", play.scroll_stop},
        }};
        for (const auto& [id, value] : sliders) {
            if (auto* control = has(id) ? slider(id) : nullptr) {
                control->set_min(0.0);
                control->set_max(std::string(id) == "IDC_SCROLL_SPEED_SLIDER" ? model::scroll_stop_count - 1
                                                                              : model::speed_step_count - 1);
                control->set_step(1.0);
                control->set_value_no_signal(value);
            }
        }
        const std::array<std::pair<const char*, bool>, 8> checks{{
            {"IDC_ABSOLUTE_SCROLLING", play.absolute_scrolling},
            {"IDC_DELAY_TOOLTIPS", play.delay_tooltips},
            {"IDC_RIGHT_BUTTON_SCROLL", play.right_button_scroll},
            {"IDC_BOTH_MOUSE_BUTTONS_ROTATE", play.both_buttons_rotate},
            {"IDC_ALTERNATE_MOUSE_CONTROLS", play.alternate_mouse},
            {"IDC_SIMPLIFIED_FACTION_COLORS", play.simplified_faction_colours},
            {"IDC_ABILITIES_DEFAULT_TO_AUTOFIRE", play.abilities_autofire},
            {"IDC_AUTORESOLVE", play.ask_auto_resolve},
        }};
        for (const auto& [id, value] : checks) {
            if (has(id)) check(id)->set_pressed_no_signal(value);
        }
        break;
    }
    case Page::keyboard: {
        for (const char* id : {"IDC_SELECTION_ALL_KEY", "IDC_SELECTION_SCREEN_KEY"}) {
            if (has(id)) state.controls[id]->set_tooltip_text(text(
                "Click, then press a key or mouse button. Mouse buttons support Ctrl/Shift/Alt. Esc cancels. Reserved inputs are refused."));
        }
        if (auto* combo = Object::cast_to<EawrUiCombo>(state.controls["IDC_CAMERA_KEYS"])) {
            combo->clear();
            combo->add_item("Arrows (default, retail)");
            combo->add_item("WASD (ZQSD on AZERTY)");
            combo->select(draft_.camera_wasd ? 1 : 0);
            combo->set_tooltip_text(text(camera_keys_help(draft_.camera_wasd)));
        }
        if (auto* apply = Object::cast_to<BaseButton>(state.controls["IDC_APPLY"])) {
            apply->set_disabled(draft_.camera_wasd == committed_.camera_wasd && draft_.keys == committed_.keys);
        }
        for (BuiltGadget& gadget : state.dialog->gadgets) {
            if (gadget.control == nullptr) continue;
            if (auto* edit = Object::cast_to<LineEdit>(gadget.control)) {
                const auto override_it = draft_.keys.find(gadget.id.substr(9)); // after IDC_EDIT_
                const std::string_view key = override_it != draft_.keys.end()
                    ? std::string_view(override_it->second) : model::project_key_for(gadget.id, draft_.camera_wasd);
                edit->set_text(text(keyboard_key_label(key)));
            }
        }
        for (int number = 1; number <= keyboard_page_count; ++number) {
            const std::string id = "IDC_KEYBOARD_OPTIONS_" + std::to_string(number);
            if (has(id.c_str())) {
                Object::cast_to<EawrUiCheck>(state.controls[id])->set_pressed_no_signal(number == keyboard_page_);
            }
        }
        break;
    }
    case Page::network: {
        const model::NetworkSettings& net = draft_.network;
        const std::array<std::pair<const char*, std::string>, 5> values{{
            {"IDC_ONLINE_SERVICE_HOST", net.online_host}, {"IDC_ONLINE_SERVICE_PORT", std::to_string(net.online_port)},
            {"IDC_ONLINE_SERVICE_PIN", net.online_pin}, {"IDC_ONLINE_SERVICE_CA", net.online_ca_file},
            {"IDC_ONLINE_SERVICE_TRUST", net.online_trust}}};
        for (const auto& [id, value] : values) if (has(id)) Object::cast_to<LineEdit>(state.controls[id])->set_text(text(value));
        if (has("IDC_ONLINE_RELAY_ONLY")) check("IDC_ONLINE_RELAY_ONLY")->set_pressed_no_signal(net.online_relay_only);
        if (has("IDC_NETWORK_PORT_NUMBER")) {
            Object::cast_to<LineEdit>(state.controls["IDC_NETWORK_PORT_NUMBER"])
                ->set_text(text(std::to_string(net.port)));
        }
        const std::array<std::pair<const char*, bool>, 4> checks{{
            {"IDC_CHECK_USE_DEFAULT_PORT", net.uses_default_port()},
            {"IDC_ALLOW_NAT_NEGOTIATION", net.allow_nat},
            {"IDC_ALLOW_WINDOWS_FIREWALL_BYPASS", net.allow_firewall},
            {"IDC_ALLOW_UPNP", net.allow_upnp},
        }};
        for (const auto& [id, value] : checks) {
            if (has(id)) check(id)->set_pressed_no_signal(value);
        }
        const auto key_text = [this](const char* key) {
            return resources_->text ? data::to_utf8(resources_->text->text(key)) : std::string(key);
        };
        const auto fill = [&](const char* id, const std::vector<std::string>& items, const int selected) {
            if (!has(id)) return;
            auto* combo = Object::cast_to<EawrUiCombo>(state.controls[id]);
            for (const std::string& item : items) combo->add_item(text(item));
            if (selected >= 0 && selected < static_cast<int>(items.size())) combo->select(selected);
        };
        fill("IDC_NETWORK_QUICKMATCH_DEFAULT",
             {key_text("TEXT_QUICKMATCH_1V1_CAMPAIGN"), key_text("TEXT_QUICKMATCH_1V1_LAND"),
              key_text("TEXT_QUICKMATCH_1V1_SPACE")},
             net.quickmatch_default);
        fill("IDC_INTERNET_CONNECTION_SPEED_COMBO",
             {key_text("TEXT_INTERNET_CONNECTION_SPEED_MODEM"), key_text("TEXT_INTERNET_CONNECTION_SPEED_BROADBAND")},
             net.connection_speed);
        fill("IDC_INTERNET_PREFERRED_QUICKMATCH_FACTION", {key_text("TEXT_QUICKMATCH_RANDOM_FACTION")}, 0);
        // The network page's own Defaults button restores values nothing here can change.
        break;
    }
    }
    loading_ = false;
    dirty_ = false;
    if (page_ == Page::performance) update_performance_dirty();
}

// --- Signals -------------------------------------------------------------------------------

void EawrOptionsScreen::update_advanced_dirty() {
    dirty_ = draft_.video.detail[model::detail_slot_particle] != committed_.video.detail[model::detail_slot_particle]
        || draft_.video.detail[model::detail_slot_heat] != committed_.video.detail[model::detail_slot_heat];
    const bool battle_change = dirty_;
    dirty_ = dirty_ || draft_.video.supersampling != committed_.video.supersampling;
    if (auto* apply = Object::cast_to<BaseButton>(state_->controls["IDC_APPLY"])) {
        apply->set_disabled(!dirty_);
        apply->set_tooltip_text(text(!dirty_ ? "Nothing to apply yet."
            : battle_change ? "Particle detail and heat distortions take effect when the next battle starts."
                            : ""));
    }
}

void EawrOptionsScreen::update_tweaks_dirty() {
    dirty_ = draft_.selection_hotkeys != committed_.selection_hotkeys
        || draft_.selection_hotkeys_authored != committed_.selection_hotkeys_authored
        || draft_.gameplay.fit_radar_to_galaxy != committed_.gameplay.fit_radar_to_galaxy
        || draft_.gameplay.space_pauses_galactic != committed_.gameplay.space_pauses_galactic
        || draft_.gameplay.gc_battle_colours != committed_.gameplay.gc_battle_colours
        || draft_.gameplay.galactic_build_tray != committed_.gameplay.galactic_build_tray
        || draft_.gameplay.animated_loading_background != committed_.gameplay.animated_loading_background
        || draft_.gameplay.hover_owner_names != committed_.gameplay.hover_owner_names
        || draft_.gameplay.reinforcement_areas_only_while_placing != committed_.gameplay.reinforcement_areas_only_while_placing;
    if (auto* apply = Object::cast_to<BaseButton>(state_->controls["IDC_APPLY"])) apply->set_disabled(!dirty_);
}

void EawrOptionsScreen::on_focus_changed(String id, const bool focused) {
    if (!state_) return;
    const auto found = state_->focus_outlines.find(utf8(id));
    if (found != state_->focus_outlines.end() && found->second != nullptr) found->second->set_visible(focused);
}

void EawrOptionsScreen::on_pressed(String id) { handle_button(utf8(id)); }

void EawrOptionsScreen::on_toggled(const bool pressed, String id) {
    if (loading_ || !state_) return;
    const std::string name = utf8(id);
    if (page_ == Page::keyboard && name.rfind("IDC_KEYBOARD_OPTIONS_", 0) == 0) {
        const int number = name.back() - '0';
        if (number >= 1 && number <= keyboard_page_count && number != keyboard_page_) {
            keyboard_page_ = number;
            rebuild_page();
        } else if (auto* radio = Object::cast_to<EawrUiCheck>(state_->controls[name])) {
            radio->set_pressed_no_signal(true); // the shown page's own button stays on
        }
        return;
    }
    if (page_ == Page::advanced_video && name == "IDC_HEAT_DISTORTIONS") {
        state_->heat_touched = true;
        draft_.video.detail[model::detail_slot_heat] = pressed ? 1 : 0;
        update_advanced_dirty();
        return;
    }
    if (page_ == Page::tweaks && (name == "IDC_HOVER_OWNER_NAMES" || name == "IDC_REINFORCEMENT_AREAS_ONLY_WHILE_PLACING" || name == "IDC_ANIMATED_LOADING_BACKGROUND" || name == "IDC_FIT_RADAR_TO_GALAXY" || name == "IDC_SPACE_PAUSES_GALACTIC" || name == "IDC_GALACTIC_BUILD_TRAY")) {
        if (name == "IDC_HOVER_OWNER_NAMES") draft_.gameplay.hover_owner_names = pressed ? 1 : 0;
        else if (name == "IDC_FIT_RADAR_TO_GALAXY") draft_.gameplay.fit_radar_to_galaxy = pressed;
        else if (name == "IDC_SPACE_PAUSES_GALACTIC") draft_.gameplay.space_pauses_galactic = pressed;
        else if (name == "IDC_GALACTIC_BUILD_TRAY") draft_.gameplay.galactic_build_tray = pressed ? 1 : 0; // GBT-13: a click is an explicit choice
        else if (name == "IDC_ANIMATED_LOADING_BACKGROUND") draft_.gameplay.animated_loading_background = pressed;
        else draft_.gameplay.reinforcement_areas_only_while_placing = pressed;
        update_tweaks_dirty();
        return;
    }
    if (page_ == Page::video && name == "IDC_WINDOWED_MODE") {
        // The list holds one size at a time: the window's in windowed mode, the screen mode's otherwise.
        PageState& state = *state_;
        const model::VideoSettings& video = draft_.video;
        state.windowed_draft = pressed;
        state.selected_size = pressed ? model::DisplaySize{video.window_width, video.window_height}
                                      : model::DisplaySize{video.fullscreen_width, video.fullscreen_height};
        if (auto* list = Object::cast_to<EawrUiList>(state.controls["IDC_RESOLUTION_LIST"])) {
            for (std::size_t index = 0; index < state.sizes.size(); ++index) {
                if (state.sizes[index] == state.selected_size) list->select(static_cast<int32_t>(index));
            }
        }
    }
    if (page_ == Page::video && is_video_edit(name)) {
        dirty_ = true;
        if (auto* apply = Object::cast_to<BaseButton>(state_->controls["IDC_APPLY"])) {
            apply->set_disabled(false);
            apply->set_tooltip_text(String());
        }
    }
}

void EawrOptionsScreen::on_value_changed(const double value, String id) {
    if (loading_ || !state_) return;
    const std::string name = utf8(id);
    if (page_ == Page::audio && is_audio_slider(name)) {
        // ST-A3: a volume slider is heard at once; Cancel and Esc put the saved levels back.
        const int percent = static_cast<int>(value + 0.5);
        if (name == "IDC_SLIDER_MASTER_VOLUME") draft_.audio.master_percent = percent;
        else if (name == "IDC_SLIDER_SOUND_EFFECTS_VOLUME") draft_.audio.sfx_percent = percent;
        else if (name == "IDC_SLIDER_DIALOG_VOLUME") draft_.audio.speech_percent = percent;
        else draft_.audio.music_percent = percent;
        apply_preview();
        return;
    }
    if (page_ == Page::advanced_video && name == "IDC_PARTICLE_DETAIL_SLIDER") {
        state_->particle_touched = true;
        draft_.video.detail[model::detail_slot_particle] = static_cast<int>(value + 0.5);
        update_advanced_dirty();
        return;
    }
    if (page_ == Page::advanced_video && name == "IDC_SUPERSAMPLING_SLIDER") {
        draft_.video.supersampling = std::clamp(static_cast<int>(value + 0.5), 0,
                                                static_cast<int>(model::supersampling_scales.size()) - 1);
        update_advanced_dirty();
        return;
    }
    if (page_ == Page::video && is_video_edit(name)) {
        if (name == "IDC_ANTI_ALIAS_SLIDER") state_->profile_touched = true;
        dirty_ = true;
        if (auto* apply = Object::cast_to<BaseButton>(state_->controls["IDC_APPLY"])) {
            apply->set_disabled(false);
            apply->set_tooltip_text(String());
        }
    }
}

void EawrOptionsScreen::on_item_selected(const int64_t index, String id) {
    if (loading_ || !state_) return;
    const std::string name = utf8(id);
    if (page_ == Page::tweaks && name == "IDC_GC_BATTLE_COLOURS" && (index == 0 || index == 1)) {
        draft_.gameplay.gc_battle_colours = static_cast<int>(index);
        update_tweaks_dirty();
        return;
    }
    if (page_ == Page::save_game && name == "IDC_SAVE_GAME_LIST") {
        const auto found = state_->controls.find("IDC_SAVE_GAME_NAME_EDIT");
        auto* edit = found != state_->controls.end() ? Object::cast_to<LineEdit>(found->second) : nullptr;
        if (edit != nullptr && index >= 0 && index < static_cast<int64_t>(state_->saves.size()))
            edit->set_text(text(state_->saves[static_cast<std::size_t>(index)]));
        return;
    }
    if (page_ == Page::tweaks && name == "IDC_SELECTION_HOTKEYS" && (index == 0 || index == 1)) {
        if (draft_.selection_hotkeys != (index == 1)) {
            draft_.selection_hotkeys = index == 1;
            draft_.selection_hotkeys_authored = true;
        }
        update_tweaks_dirty();
        return;
    }
    if (page_ == Page::keyboard && name == "IDC_CAMERA_KEYS" && (index == 0 || index == 1)) {
        draft_.camera_wasd = index == 1;
        dirty_ = draft_.camera_wasd != committed_.camera_wasd || draft_.keys != committed_.keys;
        rebuild_page(); // refresh every displayed shortcut from the same resolved preset
        return;
    }
    if (page_ == Page::performance && (name == "IDC_PARTICLE_THREADS" || name == "IDC_SIMULATION_THREADS")) {
        auto* combo = Object::cast_to<EawrUiCombo>(state_->controls[name]);
        if (index < 0 || index >= combo->get_item_count()) return;
        int count = 0;
        if (index != 0) {
            const std::string value = utf8(combo->get_item_text(static_cast<int32_t>(index)));
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
            if (error != std::errc{} || end != value.data() + value.size()) return;
        }
        if (name == "IDC_PARTICLE_THREADS") draft_.video.particle_threads = count;
        else draft_.video.simulation_threads = count;
        update_performance_dirty();
        return;
    }
    if (page_ == Page::performance && name == "IDC_DEPTH_PREPASS" && (index == 0 || index == 1)) {
        draft_.video.depth_prepass = index == 0;
        update_performance_dirty();
        return;
    }
    if (page_ == Page::performance && name == "IDC_RENDER_THREAD" && (index == 0 || index == 1)) {
        draft_.video.render_thread = index == 1 ? model::RenderThreadSetting::safe : model::RenderThreadSetting::separate;
        update_performance_dirty();
        return;
    }
    if (page_ == Page::video && name == "IDC_FRAME_RATE_LIMIT") {
        const std::array<int, 9> choices{0, -1, 30, 60, 90, 120, 144, 165, 240};
        if (index >= 0 && index <= static_cast<int64_t>(choices.size())) {
            int fps = draft_.video.frame_rate_limit;
            if (index < static_cast<int64_t>(choices.size())) fps = choices[static_cast<std::size_t>(index)];
            else {
                const auto* combo = Object::cast_to<EawrUiCombo>(state_->controls[name]);
                if (combo == nullptr || index >= combo->get_item_count()) return;
                const std::string caption = utf8(combo->get_item_text(static_cast<int32_t>(index)));
                const auto [end, error] = std::from_chars(caption.data(), caption.data() + caption.size(), fps);
                if (error != std::errc{} || end != caption.data() + caption.size()) return;
            }
            draft_.video.frame_rate_limit = fps;
            dirty_ = true;
            if (auto* apply = Object::cast_to<BaseButton>(state_->controls["IDC_APPLY"])) {
                apply->set_disabled(false);
                apply->set_tooltip_text(String());
            }
        }
    }
    if (page_ == Page::video && name == "IDC_RESOLUTION_LIST" && index >= 0
        && index < static_cast<int64_t>(state_->sizes.size())) {
        state_->selected_size = state_->sizes[static_cast<std::size_t>(index)];
        dirty_ = true;
        if (auto* apply = Object::cast_to<BaseButton>(state_->controls["IDC_APPLY"])) {
            apply->set_disabled(false);
            apply->set_tooltip_text(String());
        }
    }
}

// --- Buttons -------------------------------------------------------------------------------

void EawrOptionsScreen::apply_preview() {
    ApplyReport report;
    apply_audio(draft_.audio, report);
    last_apply_ = std::move(report);
}

void EawrOptionsScreen::apply_committed(const model::VideoSettings& previous_video) {
    ApplyReport report;
    apply_audio(committed_.audio, report);
    ApplyTargets targets;
    const model::VideoSettings& video = committed_.video;
    const bool window_changed = video.windowed != previous_video.windowed
        || (video.windowed && (video.window_width != previous_video.window_width
                              || video.window_height != previous_video.window_height));
    if (video.frame_rate_limit != previous_video.frame_rate_limit) {
        update_frame_pacing(video, options_.context == Context::main_menu);
        report.applied.emplace_back("frame-rate limit: " + std::to_string(video.frame_rate_limit));
    }
    targets.window = options_.apply_window && window_changed;
    targets.vsync = video.vsync != previous_video.vsync;
    targets.texture_detail = video.texture_mip_drop != previous_video.texture_mip_drop;
    // ST-V5: an unchanged Accept must not reset the window or renderer. Compare with the
    // last committed values, advancing the baseline after Apply as well as Accept.
    if (window_changed || targets.vsync || targets.texture_detail) apply_video(video, targets, report);
    // The render profile is the player's explicit choice only; "automatic" leaves the run's own.
    if ((video.render_profile != previous_video.render_profile
         && video.render_profile != model::RenderProfileSetting::automatic)
        || video.supersampling != previous_video.supersampling) {
        if (auto* tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop())) {
            const RenderProfile profile = resolve_render_profile(video.render_profile, active_render_profile());
            apply_render_profile(*tree->get_root(), profile,
                model::supersampling_scales[static_cast<std::size_t>(video.supersampling)]);
            report.applied.emplace_back(std::string("render profile: ") + std::string(render_profile_name(profile))
                                        + ", supersampling " + supersampling_name(video.supersampling));
        }
    }
    last_apply_ = std::move(report);
}

void EawrOptionsScreen::sync_draft_from_controls() {
    if (!state_) return;
    PageState& state = *state_;
    const auto value = [&](const char* id) -> std::optional<double> {
        const auto found = state.controls.find(id);
        if (found == state.controls.end() || found->second == nullptr) return std::nullopt;
        if (state.reasons.count(id) != 0) return std::nullopt; // not honoured: the saved value stays
        if (const auto* slider = Object::cast_to<EawrUiSlider>(found->second)) return slider->get_value();
        if (const auto* button = Object::cast_to<BaseButton>(found->second)) return button->is_pressed() ? 1.0 : 0.0;
        return std::nullopt;
    };
    switch (page_) {
    case Page::audio:
        if (const auto v = value("IDC_SLIDER_MASTER_VOLUME")) draft_.audio.master_percent = static_cast<int>(*v + 0.5);
        if (const auto v = value("IDC_SLIDER_SOUND_EFFECTS_VOLUME")) draft_.audio.sfx_percent = static_cast<int>(*v + 0.5);
        if (const auto v = value("IDC_SLIDER_DIALOG_VOLUME")) draft_.audio.speech_percent = static_cast<int>(*v + 0.5);
        if (const auto v = value("IDC_SLIDER_MUSIC_VOLUME")) draft_.audio.music_percent = static_cast<int>(*v + 0.5);
        if (const auto v = value("IDC_USE_MOVIE_SUBTITLES")) draft_.audio.subtitles = *v > 0.5;
        break;
    case Page::video: {
        if (const auto v = value("IDC_WINDOWED_MODE")) draft_.video.windowed = *v > 0.5;
        if (const auto v = value("IDC_VSYNC_ENABLED")) draft_.video.vsync = *v > 0.5;
        if (state.selected_size.width > 0) {
            if (draft_.video.windowed) {
                draft_.video.window_width = state.selected_size.width;
                draft_.video.window_height = state.selected_size.height;
            } else {
                draft_.video.fullscreen_width = state.selected_size.width;
                draft_.video.fullscreen_height = state.selected_size.height;
            }
        }
        if (state.profile_touched) {
            if (const auto v = value("IDC_ANTI_ALIAS_SLIDER")) {
                draft_.video.render_profile = setting_of(aa_slider_profiles[
                    static_cast<std::size_t>(std::clamp(*v, 0.0, static_cast<double>(aa_slider_profiles.size() - 1)))]);
            }
        }
        break;
    }
    case Page::tweaks:
        if (const auto v = value("IDC_FIT_RADAR_TO_GALAXY")) draft_.gameplay.fit_radar_to_galaxy = *v > 0.5;
        if (const auto v = value("IDC_SPACE_PAUSES_GALACTIC")) draft_.gameplay.space_pauses_galactic = *v > 0.5;
        if (const auto v = value("IDC_GALACTIC_BUILD_TRAY")) draft_.gameplay.galactic_build_tray = *v > 0.5 ? 1 : 0;
        if (const auto v = value("IDC_ANIMATED_LOADING_BACKGROUND")) draft_.gameplay.animated_loading_background = *v > 0.5;
        if (const auto v = value("IDC_REINFORCEMENT_AREAS_ONLY_WHILE_PLACING"))
            draft_.gameplay.reinforcement_areas_only_while_placing = *v > 0.5;
        if (const auto v = value("IDC_HOVER_OWNER_NAMES"); v && (*v > 0.5) != model::hover_owner_names_enabled(draft_.gameplay, options_.multiplayer))
            draft_.gameplay.hover_owner_names = *v > 0.5;
        break;
    case Page::advanced_video:
        // Only the two honoured controls, and only once touched: an automatic value stays automatic.
        if (state.particle_touched) {
            if (const auto v = value("IDC_PARTICLE_DETAIL_SLIDER")) {
                draft_.video.detail[model::detail_slot_particle] = static_cast<int>(*v + 0.5);
            }
        }
        if (state.heat_touched) {
            if (const auto v = value("IDC_HEAT_DISTORTIONS")) draft_.video.detail[model::detail_slot_heat] = *v > 0.5 ? 1 : 0;
        }
        if (const auto v = value("IDC_SUPERSAMPLING_SLIDER")) {
            draft_.video.supersampling = std::clamp(static_cast<int>(*v + 0.5), 0,
                                                    static_cast<int>(model::supersampling_scales.size()) - 1);
        }
        break;
    case Page::gameplay:
        if (const auto v = value("IDC_GAME_SPEED_SLIDER")) draft_.gameplay.tactical_speed = static_cast<int>(*v + 0.5);
        break;
    case Page::network: {
        const auto edit = [&](const char* id) { const auto found = state.controls.find(id); return found != state.controls.end()
            ? utf8(Object::cast_to<LineEdit>(found->second)->get_text()) : std::string{}; };
        draft_.network.online_host = edit("IDC_ONLINE_SERVICE_HOST");
        draft_.network.online_pin = edit("IDC_ONLINE_SERVICE_PIN");
        draft_.network.online_ca_file = edit("IDC_ONLINE_SERVICE_CA");
        draft_.network.online_trust = edit("IDC_ONLINE_SERVICE_TRUST");
        const std::string port = edit("IDC_ONLINE_SERVICE_PORT");
        int parsed{}; const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), parsed);
        draft_.network.online_port = error == std::errc{} && end == port.data() + port.size() ? parsed : 0;
        if (const auto v = value("IDC_ONLINE_RELAY_ONLY")) draft_.network.online_relay_only = *v > 0.5;
        break;
    }
    default: break;
    }
}

void EawrOptionsScreen::commit_page(const bool leave) {
    const model::VideoSettings previous_video = committed_.video;
    sync_draft_from_controls();
    if (page_ == Page::network) {
        if (const auto error = model::validate_online_settings(draft_.network)) {
            if (auto* label = Object::cast_to<EawrUiLabel>(state_->controls["IDC_ONLINE_SERVICE_ERROR"])) label->set_text(text(error->message));
            return;
        }
    }
    static_cast<void>(model::clamp_settings(draft_));
    // Only this page's section is committed: the others are the saved ones still.
    switch (page_) {
    case Page::audio: committed_.audio = draft_.audio; break;
    case Page::video: committed_.video = draft_.video; break;
    case Page::advanced_video:
        // The other detail controls are refused, so only these two slots are committed (#6560).
        committed_.video.detail[model::detail_slot_particle] = draft_.video.detail[model::detail_slot_particle];
        committed_.video.detail[model::detail_slot_heat] = draft_.video.detail[model::detail_slot_heat];
        committed_.video.supersampling = draft_.video.supersampling;
        break;
    case Page::performance:
        committed_.video.render_thread = draft_.video.render_thread;
        committed_.video.particle_threads = draft_.video.particle_threads;
        committed_.video.simulation_threads = draft_.video.simulation_threads;
        committed_.video.depth_prepass = draft_.video.depth_prepass;
        break;
    case Page::tweaks:
        committed_.selection_hotkeys = draft_.selection_hotkeys; committed_.selection_hotkeys_authored = draft_.selection_hotkeys_authored;
        committed_.gameplay.fit_radar_to_galaxy = draft_.gameplay.fit_radar_to_galaxy;
        committed_.gameplay.space_pauses_galactic = draft_.gameplay.space_pauses_galactic;
        committed_.gameplay.gc_battle_colours = draft_.gameplay.gc_battle_colours;
        committed_.gameplay.galactic_build_tray = draft_.gameplay.galactic_build_tray;
        committed_.gameplay.animated_loading_background = draft_.gameplay.animated_loading_background;
        committed_.gameplay.hover_owner_names = draft_.gameplay.hover_owner_names;
        committed_.gameplay.reinforcement_areas_only_while_placing = draft_.gameplay.reinforcement_areas_only_while_placing; break;
    case Page::gameplay: committed_.gameplay = draft_.gameplay; break;
    case Page::network: committed_.network = draft_.network; break;
    case Page::keyboard: committed_.camera_wasd = draft_.camera_wasd; committed_.keys = draft_.keys; break;
    default: break;
    }
    draft_ = committed_;
    std::string error;
    if (!store_.save(committed_, error)) notes_.push_back("settings not saved: " + error);
    apply_committed(previous_video);
    // The viewer's other readers (the next battle's buses, the live speed step) see the same record.
    ActiveSettings::set(committed_);
    dirty_ = false;
    if (leave) leave_to_hub();
    else if (state_) {
        if (auto* apply = Object::cast_to<BaseButton>(state_->controls["IDC_APPLY"])) {
            apply->set_disabled(true);
            apply->set_tooltip_text(text("Nothing to apply yet."));
        }
    }
}

void EawrOptionsScreen::cancel_page() {
    // ST-A3: the levels heard while a slider moved go back to the saved ones.
    if (page_ == Page::audio) {
        ApplyReport report;
        apply_audio(committed_.audio, report);
        last_apply_ = std::move(report);
    }
    draft_ = committed_;
    if (page_ == Page::advanced_video) {
        page_ = Page::video;
        rebuild_page();
        return;
    }
    leave_to_hub();
}

void EawrOptionsScreen::defaults_page() {
    const model::PlayerSettings defaults;
    switch (page_) {
    case Page::audio: draft_.audio = defaults.audio; break;
    case Page::performance:
        draft_.video.render_thread = defaults.video.render_thread;
        draft_.video.particle_threads = defaults.video.particle_threads;
        draft_.video.simulation_threads = defaults.video.simulation_threads;
        draft_.video.depth_prepass = defaults.video.depth_prepass;
        rebuild_page();
        update_performance_dirty();
        return;
    case Page::tweaks:
        draft_.selection_hotkeys = draft_.selection_config.enabled;
        draft_.selection_hotkeys_authored = false;
        draft_.gameplay.fit_radar_to_galaxy = defaults.gameplay.fit_radar_to_galaxy;
        draft_.gameplay.space_pauses_galactic = defaults.gameplay.space_pauses_galactic;
        draft_.gameplay.gc_battle_colours = defaults.gameplay.gc_battle_colours;
        draft_.gameplay.galactic_build_tray = defaults.gameplay.galactic_build_tray;
        draft_.gameplay.animated_loading_background = defaults.gameplay.animated_loading_background;
        draft_.gameplay.hover_owner_names = defaults.gameplay.hover_owner_names;
        draft_.gameplay.reinforcement_areas_only_while_placing = defaults.gameplay.reinforcement_areas_only_while_placing;
        rebuild_page();
        update_tweaks_dirty();
        return;
    case Page::gameplay:
        // The honoured option only: the others are not editable here, so Defaults leaves them.
        draft_.gameplay.tactical_speed = defaults.gameplay.tactical_speed;
        break;
    case Page::keyboard:
        draft_.camera_wasd = defaults.camera_wasd;
        for (const auto command : model::selection_commands) draft_.keys.erase(std::string(command));
        break;
    case Page::network:
        draft_.network.online_host = defaults.network.online_host;
        draft_.network.online_port = defaults.network.online_port;
        draft_.network.online_trust = defaults.network.online_trust;
        draft_.network.online_pin = defaults.network.online_pin;
        draft_.network.online_ca_file = defaults.network.online_ca_file;
        draft_.network.online_relay_only = defaults.network.online_relay_only;
        draft_.network.online_turn_provider = defaults.network.online_turn_provider;
        break;
    default: return;
    }
    rebuild_page();
    if (page_ == Page::audio) apply_preview();
}

void EawrOptionsScreen::leave_to_hub() {
    page_ = Page::hub;
    rebuild_page();
}

void EawrOptionsScreen::leave_hub() {
    if (!menu_root_) {
        close_screen();
        return;
    }
    page_ = Page::game_menu;
    rebuild_page();
}

bool EawrOptionsScreen::confirm_open() const {
    return confirm_id_ != 0 && UtilityFunctions::is_instance_id_valid(confirm_id_);
}

// #3989: the Save page's Save. A name that is no valid save name does nothing (the field keeps it).
void EawrOptionsScreen::save_named(const std::string& name, const bool overwrite) {
    if (!options_.save_game) return;
    if (!overwrite) {
        const auto found = std::find_if(state_->saves.begin(), state_->saves.end(), [&](const std::string& existing) {
            return text(existing).nocasecmp_to(text(name)) == 0;
        });
        if (found != state_->saves.end()) {
            ask_overwrite(*found);
            return;
        }
    }
    if (options_.save_game(name, overwrite)) {
        close_screen();
        return;
    }
    notes_.push_back("save refused: " + name);
}

// WGS-153: replacing a save asks TEXT_SAVE_CONFIRM with Yes / No; No keeps the Save page.
void EawrOptionsScreen::ask_overwrite(std::string name) {
    if (confirm_open() || !resources_ || !resources_->theme_model) return;
    Resources& resources = *resources_;
    model::MessageBoxRequest request;
    request.kind = model::BoxKind::standard;
    request.caption = "TEXT_SAVE_CONFIRM";
    request.buttons = {model::BoxButton{"TEXT_BUTTON_YES", 1, false}, model::BoxButton{"TEXT_BUTTON_NO", 0, true}};
    const MessageBoxContext context{&*resources.catalog, &*resources.theme_model,
                                    resources.text ? &*resources.text : nullptr, resources.theme,
                                    get_viewport()->get_visible_rect().size};
    auto* box = memnew(EawrMessageBox);
    if (!box->open(*this, context, request)) {
        notes_.push_back("overwrite confirmation: " + box->failure());
        memdelete(box);
        return;
    }
    confirm_ = Confirm::overwrite;
    overwrite_name_ = std::move(name);
    confirm_id_ = static_cast<int64_t>(box->get_instance_id());
    box->connect("result_chosen", callable_mp(this, &EawrOptionsScreen::on_confirm_result));
}

// MNU-42, WGS-165: Quit Game asks TEXT_QUIT_SURE with Yes / No; Esc or No keeps the menu.
void EawrOptionsScreen::ask_quit() {
    if (confirm_open() || !resources_ || !resources_->theme_model) return;
    Resources& resources = *resources_;
    model::MessageBoxRequest request;
    request.kind = model::BoxKind::quit_confirm;
    request.caption = "TEXT_QUIT_SURE";
    request.buttons = {model::BoxButton{"TEXT_BUTTON_YES", 1, false}, model::BoxButton{"TEXT_BUTTON_NO", 0, true}};
    const MessageBoxContext context{&*resources.catalog, &*resources.theme_model,
                                    resources.text ? &*resources.text : nullptr, resources.theme,
                                    get_viewport()->get_visible_rect().size};
    auto* box = memnew(EawrMessageBox);
    if (!box->open(*this, context, request)) {
        notes_.push_back("quit confirmation: " + box->failure());
        memdelete(box);
        return;
    }
    confirm_ = Confirm::quit;
    confirm_id_ = static_cast<int64_t>(box->get_instance_id());
    box->connect("result_chosen", callable_mp(this, &EawrOptionsScreen::on_confirm_result));
}

void EawrOptionsScreen::open_load_game() {
    if (!options_.load_game || !resources_ || !resources_->theme_model) return;
    LoadGameContext context;
    context.boxes = MessageBoxContext{&*resources_->catalog, &*resources_->theme_model,
        resources_->text ? &*resources_->text : nullptr, resources_->theme, get_viewport()->get_visible_rect().size};
    context.saves = std::make_shared<viewer::DirectorySavedGameStore>(options_.load_directory);
    context.launcher = options_.load_game;
    auto* screen = memnew(EawrLoadGameScreen);
    if (!screen->open(*this, context)) {
        notes_.push_back("load screen: " + screen->failure());
        memdelete(screen);
        return;
    }
    set_process_input(false);
    // The options menu normally sits above the load screen's layer; put it underneath
    // while the child is open so the load controls receive pointer input.
    set_layer(EawrLoadGameScreen::layer_index - 1);
    load_screen_id_ = static_cast<int64_t>(screen->get_instance_id());
    screen->connect("closed", callable_mp(this, &EawrOptionsScreen::on_load_closed));
}

void EawrOptionsScreen::on_load_closed() { load_screen_id_ = 0; set_layer(screen_layer); set_process_input(true); }

void EawrOptionsScreen::on_confirm_result(const int64_t result) {
    // The box frees itself when a button is chosen (EawrMessageBox::choose), so the id is just forgotten here.
    confirm_id_ = 0;
    if (result != 1) return;
    if (confirm_ == Confirm::overwrite) {
        save_named(std::exchange(overwrite_name_, std::string{}), true);
        return;
    }
    quit_requested_ = true;
    close_screen();
}

std::optional<Vector2> EawrOptionsScreen::control_point(const std::string& name) const {
    if (!shown_) return std::nullopt;
    if (load_screen_id_ && UtilityFunctions::is_instance_id_valid(load_screen_id_)) {
        const auto* screen = Object::cast_to<EawrLoadGameScreen>(UtilityFunctions::instance_from_id(load_screen_id_));
        if (!screen) return std::nullopt;
        if (name == "load_first") {
            const auto* list = Object::cast_to<ItemList>(screen->gadget("IDC_SAVE_GAME_LIST"));
            if (!list || list->get_item_count() == 0) return std::nullopt;
            return list->get_global_position() + list->get_item_rect(0).get_center();
        }
        if (name == "load_button") {
            const auto* button = screen->gadget("IDC_LOAD_GAME_BUTTON");
            if (button) return button->get_global_rect().get_center();
        }
        return std::nullopt;
    }
    if (confirm_open()) {
        const auto* box = Object::cast_to<EawrMessageBox>(UtilityFunctions::instance_from_id(confirm_id_));
        if (box == nullptr || (name != "yes" && name != "no")) return std::nullopt;
        for (const auto& button : box->plan().buttons) {
            if ((button.result == 1) != (name == "yes")) continue;
            if (const Control* control = box->button(button.control)) return control->get_global_rect().get_center();
        }
        return std::nullopt;
    }
    if (!state_) return std::nullopt;
    // "<list id>#<row>": a row of a list (#3989: the Save page's listed saves).
    const auto hash = name.find('#');
    const auto found = state_->controls.find(name.substr(0, hash));
    if (found == state_->controls.end() || found->second == nullptr || !found->second->is_visible_in_tree()) return std::nullopt;
    if (hash != std::string::npos) {
        const auto* list = Object::cast_to<ItemList>(found->second);
        int row = -1;
        const auto* begin = name.data() + hash + 1;
        const auto* end = name.data() + name.size();
        if (list == nullptr || std::from_chars(begin, end, row).ptr != end || row < 0 || row >= list->get_item_count())
            return std::nullopt;
        const Rect2 rect = list->get_item_rect(row);
        return list->get_global_transform().xform(rect.get_center());
    }
    return found->second->get_global_rect().get_center();
}

void EawrOptionsScreen::handle_button(const std::string& id) {
    if (!shown_ || !state_) return;
    const auto reason = state_->reasons.find(id);
    if (reason != state_->reasons.end()) return;
    switch (page_) {
    case Page::game_menu:
        if (id == "IDC_GAME_OPTIONS_RESUME") close_screen();
        else if (id == "IDC_INGAME_OPTIONS") leave_to_hub();
        else if (id == "IDC_GAME_OPTIONS_QUIT") ask_quit();
        else if (id == "IDC_SAVE_GAME") open_page(Page::save_game);
        else if (id == "IDC_LOAD_GAME") open_load_game();
        break;
    case Page::save_game:
        if (id == "IDC_SAVE_GAME_BUTTON") {
            const auto found = state_->controls.find("IDC_SAVE_GAME_NAME_EDIT");
            const auto* edit = found != state_->controls.end() ? Object::cast_to<LineEdit>(found->second) : nullptr;
            const std::string name = edit != nullptr ? utf8(edit->get_text().strip_edges()) : std::string{};
            if (!name.empty()) save_named(name, false);
        } else if (id == "IDC_SAVE_CANCEL_BUTTON") {
            page_ = Page::game_menu;
            rebuild_page();
        }
        break;
    case Page::hub:
        if (id == "IDC_OK_MAIN" || id == "IDC_OK_IN_GAME") leave_hub();
        else if (id == "IDC_MAIN_OPTIONS_AUDIO" || id == "IDC_IN_GAME_OPTIONS_AUDIO") open_page(Page::audio);
        else if (id == "IDC_MAIN_OPTIONS_VIDEO" || id == "IDC_IN_GAME_OPTIONS_VIDEO") open_page(Page::video);
        else if (id == "IDC_MAIN_OPTIONS_PERFORMANCE" || id == "IDC_IN_GAME_OPTIONS_PERFORMANCE") open_page(Page::performance);
        else if (id == "IDC_MAIN_OPTIONS_TWEAKS" || id == "IDC_IN_GAME_OPTIONS_TWEAKS") open_page(Page::tweaks);
        else if (id == "IDC_MAIN_OPTIONS_KEYBOARD" || id == "IDC_IN_GAME_OPTIONS_KEYBOARD") open_page(Page::keyboard);
        else if (id == "IDC_MAIN_OPTIONS_GAMEPLAY" || id == "IDC_IN_GAME_OPTIONS_GAMEPLAY") open_page(Page::gameplay);
        else if (id == "IDC_IN_GAME_OPTIONS_NETWORK" || id == "IDC_MAIN_OPTIONS_NETWORK") open_page(Page::network);
        break;
    case Page::audio:
        if (id == "IDC_OK") commit_page(true);
        else if (id == "IDC_CANCEL") cancel_page();
        else if (id == "IDC_RESTORE_DEFAULT") defaults_page();
        break;
    case Page::video:
        if (id == "IDC_OK") commit_page(true);
        else if (id == "IDC_APPLY") commit_page(false);
        else if (id == "IDC_CANCEL") cancel_page();
        else if (id == "IDC_ADVANCED") open_page(Page::advanced_video);
        break;
    case Page::performance:
    case Page::tweaks:
        if (id == "IDC_OK") commit_page(true);
        else if (id == "IDC_APPLY") commit_page(false);
        else if (id == "IDC_CANCEL") cancel_page();
        else if (id == "IDC_RESTORE_DEFAULT") defaults_page();
        break;
    case Page::advanced_video:
        if (id == "IDC_OK") {
            commit_page(false);
            page_ = Page::video; // back to the Video page, as Cancel does
            rebuild_page();
        } else if (id == "IDC_APPLY") commit_page(false);
        else if (id == "IDC_CANCEL") cancel_page();
        break;
    case Page::gameplay:
        if (id == "IDC_BUTTON_ACCEPT") commit_page(true);
        else if (id == "IDC_CANCEL_BUTTON") cancel_page();
        else if (id == "IDC_DEFAULTS_BUTTON") defaults_page();
        break;
    case Page::keyboard:
        if (id == "IDC_SELECTION_ALL_KEY" || id == "IDC_SELECTION_SCREEN_KEY") {
            begin_capture(id == "IDC_SELECTION_ALL_KEY" ? 0 : 1);
            break;
        }
        if (id == "IDC_BUILD_TRAY_KEY") {
            begin_capture(build_tray_capture);
            break;
        }
        if (capturing_) end_capture(); // any other button ends a waiting capture
        if (id == "IDC_SELECTION_KEYS_CANCEL") cancel_page();
        else if (id == "IDC_SELECTION_KEYS_DEFAULTS") {
            for (const auto command : model::selection_commands) draft_.keys.erase(std::string(command));
            draft_.keys.erase(std::string(model::build_tray_command));
            dirty_ = draft_.keys != committed_.keys || draft_.camera_wasd != committed_.camera_wasd;
            selection_message_.clear();
            rebuild_page();
        }
        else if (id == "IDC_OK") commit_page(true);
        else if (id == "IDC_APPLY") commit_page(false);
        else if (id == "IDC_CANCEL" || id == "IDC_BACK") cancel_page();
        else if (id == "IDC_DEFAULTS" || id == "IDC_RESTORE_DEFAULT") defaults_page();
        break;
    case Page::network:
        if (id == "IDC_OK") commit_page(true);
        else if (id == "IDC_BACK") cancel_page();
        else if (id == "IDC_SET_DEFAULT") defaults_page();
        break;
    }
}

void EawrOptionsScreen::open_page(const Page target) {
    if (!shown_) return;
    // Entering a page starts from the saved settings; an advanced page pending from the video
    // page is dropped as retail does.
    draft_ = committed_;
    page_ = target;
    capturing_.reset();
    selection_message_.clear();
    rebuild_page();
}

void EawrOptionsScreen::begin_capture(const std::size_t action) {
    capturing_ = action;
    selection_message_ = action == build_tray_capture ? "Press a key (Esc cancels)." : "Press a key or mouse button (Esc cancels).";
    rebuild_page();
}

void EawrOptionsScreen::end_capture() { capturing_.reset(); }

// The one place a pressed key becomes a binding: the real key path and the scripted `key:` step both land here.
void EawrOptionsScreen::capture_chord(const std::string& chord) {
    if (!capturing_) return;
    const std::size_t action = *capturing_;
    if (action == build_tray_capture) {
        // GBT-07: one key without modifiers; the galaxy view's own keys and the selection keys are refused.
        if (model::set_build_tray_key(draft_, chord)) selection_message_ = chord + " bound to the galactic build tray.";
        else {
            selection_message_ = chord + " is used by another command; press a different key.";
            notes_.push_back("Build tray binding refused: " + chord + " is reserved.");
        }
        end_capture();
        dirty_ = draft_.keys != committed_.keys || draft_.camera_wasd != committed_.camera_wasd;
        rebuild_page();
        return;
    }
    switch (model::selection_chord_status(draft_, action, chord)) {
    case model::SelectionChordStatus::ok:
        model::set_selection_key(draft_, action, chord);
        selection_message_ = keyboard_key_label(chord) + " bound.";
        end_capture();
        break;
    case model::SelectionChordStatus::duplicate:
        selection_message_ = keyboard_key_label(chord) + " is already used by the other selection key.";
        notes_.push_back("Selection binding conflict: choose a different key for each action.");
        end_capture();
        break;
    case model::SelectionChordStatus::mouse_reserved:
        selection_message_ = "Left and right mouse buttons are used for selecting and orders";
        end_capture();
        break;
    case model::SelectionChordStatus::camera:
        selection_message_ = keyboard_key_label(chord) + " is used by the camera; press a different button.";
        end_capture();
        break;
    default:
        selection_message_ = keyboard_key_label(chord) + " is used by another command; press a different key.";
        notes_.push_back("Selection binding refused: " + chord + " is reserved.");
        end_capture();
        break;
    }
    dirty_ = draft_.keys != committed_.keys || draft_.camera_wasd != committed_.camera_wasd;
    rebuild_page();
}

bool EawrOptionsScreen::press_key(const std::string& chord) {
    if (!shown_ || !capturing_) return false;
    if (*capturing_ == build_tray_capture) { // scripted `key:` step: the key's name, as the real key path sends it
        if (chord.find("Mouse ") != std::string::npos) return false;
        capture_chord(chord);
        return true;
    }
    if (chord.find("Mouse ") != std::string::npos) {
        auto suffix = std::string_view(chord);
        const bool ctrl = suffix.starts_with("Ctrl+");
        if (ctrl) suffix.remove_prefix(5);
        const bool shift = suffix.starts_with("Shift+");
        if (shift) suffix.remove_prefix(6);
        const bool alt = suffix.starts_with("Alt+");
        if (alt) suffix.remove_prefix(4);
        if (!suffix.starts_with("Mouse ")) return false;
        suffix.remove_prefix(6);
        int number{};
        const auto [end, error] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), number);
        if (error != std::errc{} || end != suffix.data() + suffix.size() || number < 1 || number > 28) return false;
        Ref<InputEventMouseButton> mouse;
        mouse.instantiate();
        mouse->set_button_index(static_cast<MouseButton>(number <= 3 ? number : number + 4));
        mouse->set_pressed(true);
        mouse->set_ctrl_pressed(ctrl);
        mouse->set_shift_pressed(shift);
        mouse->set_alt_pressed(alt);
        _input(mouse);
        return true;
    }
    const bool ctrl = chord.starts_with("Ctrl+");
    const std::string letter = ctrl ? chord.substr(5) : chord;
    if (letter.size() != 1 || letter[0] < 'A' || letter[0] > 'Z') return false;
    capture_chord(model::selection_chord_for(letter[0], ctrl));
    return true;
}

void EawrOptionsScreen::_input(const Ref<InputEvent>& event) {
    if (!shown_) return;
    const Ref<InputEventMouseButton> mouse = event;
    if (mouse.is_valid() && capturing_ && !confirm_open()) {
        get_viewport()->set_input_as_handled();
        if (!mouse->is_pressed() || *capturing_ == build_tray_capture) return; // the tray key takes keys only
        const int number = selection_mouse_number(static_cast<int>(mouse->get_button_index()));
        if (number == 0 || mouse->is_meta_pressed()) return;
        capture_chord(model::selection_mouse_chord_for(number, mouse->is_shift_pressed(),
            mouse->is_ctrl_pressed(), mouse->is_alt_pressed()));
        return;
    }
    const Ref<InputEventKey> key = event;
    if (key.is_null() || !key->is_pressed() || key->is_echo()) return;
    if (capturing_ && !confirm_open()) {
        get_viewport()->set_input_as_handled();
        const Key code = key->get_keycode();
        if (code == KEY_SHIFT || code == KEY_CTRL || code == KEY_ALT || code == KEY_META) return; // a modifier waits for its key
        if (code == KEY_ESCAPE) {
            end_capture();
            selection_message_.clear();
            rebuild_page();
            return;
        }
        const Key physical = key->get_physical_keycode() != KEY_NONE ? key->get_physical_keycode() : code;
        if (*capturing_ == build_tray_capture) {
            if (key->is_ctrl_pressed() || key->is_shift_pressed() || key->is_alt_pressed() || key->is_meta_pressed()) {
                selection_message_ = "The build tray key is one key without Ctrl, Shift or Alt.";
                rebuild_page();
                return;
            }
            capture_chord(utf8(OS::get_singleton()->get_keycode_string(physical)));
            return;
        }
        const bool letter = physical >= KEY_A && physical <= KEY_Z;
        if (!letter || key->is_shift_pressed() || key->is_alt_pressed() || key->is_meta_pressed()) {
            // The dispatcher matches a physical letter, bare or with Ctrl; stay armed for another key.
            selection_message_ = "Only a letter key, alone or with Ctrl, can be bound.";
            notes_.push_back(selection_message_);
            rebuild_page();
            return;
        }
        capture_chord(model::selection_chord_for(static_cast<char>('A' + (physical - KEY_A)), key->is_ctrl_pressed()));
        return;
    }
    if (key->get_keycode() != KEY_ESCAPE) return;
    if (confirm_open()) return; // the confirmation box answers Esc itself
    get_viewport()->set_input_as_handled();
    if (page_ == Page::game_menu) close_screen();
    else if (page_ == Page::hub) leave_hub();
    else if (page_ == Page::save_game) {
        page_ = Page::game_menu;
        rebuild_page();
    } else cancel_page();
}

// --- Scripted use ----------------------------------------------------------------------------

bool EawrOptionsScreen::set_control(const std::string& id, const double value) {
    if (!state_) return false;
    const auto found = state_->controls.find(id);
    if (found == state_->controls.end() || found->second == nullptr) return false;
    if (state_->reasons.count(id) != 0) return false;
    Control* control = found->second;
    if (auto* slider = Object::cast_to<EawrUiSlider>(control)) {
        slider->set_value(value);
        return true;
    }
    if (auto* list = Object::cast_to<EawrUiList>(control)) {
        const auto index = static_cast<int32_t>(value);
        if (index < 0 || index >= list->get_item_count()) return false;
        list->select(index);
        list->emit_signal("item_selected", static_cast<int64_t>(index));
        return true;
    }
    if (auto* combo = Object::cast_to<EawrUiCombo>(control)) {
        const auto index = static_cast<int32_t>(value);
        if (index < 0 || index >= combo->get_item_count()) return false;
        combo->select(index);
        combo->emit_signal("item_selected", static_cast<int64_t>(index));
        return true;
    }
    if (auto* button = Object::cast_to<BaseButton>(control)) {
        if (button->is_disabled()) return false;
        button->set_pressed(value > 0.5);
        return true;
    }
    return false;
}

bool EawrOptionsScreen::set_text_control(const std::string& id, const std::string& value) {
    if (!state_) return false;
    const auto found = state_->controls.find(id);
    if (found == state_->controls.end() || state_->reasons.count(id)) return false;
    auto* edit = Object::cast_to<LineEdit>(found->second);
    if (!edit || !edit->is_editable()) return false;
    edit->set_text(text(value)); return true;
}

bool EawrOptionsScreen::press(const std::string& id) {
    if (!state_) return false;
    const auto found = state_->controls.find(id);
    if (found == state_->controls.end()) return false;
    auto* button = Object::cast_to<BaseButton>(found->second);
    if (button == nullptr || button->is_disabled() || !button->is_visible()) return false;
    handle_button(id);
    return true;
}

std::string EawrOptionsScreen::rectangles() const {
    if (!state_ || !state_->dialog) return "{}";
    std::string json = dialog_rectangles(*state_->dialog);
    if (state_->performance_scroll != nullptr && state_->performance_scroll->get_child_count() > 0 && !json.empty()
        && json.back() == '}') {
        // The Performance rows scroll only when this content is taller than the viewport (the fallback).
        const auto* rows = Object::cast_to<Control>(state_->performance_scroll->get_child(0));
        json.pop_back();
        json += ",\"performance_rows\":{\"viewport\":" + std::to_string(state_->performance_scroll->get_size().y)
            + ",\"content\":" + std::to_string(rows != nullptr ? rows->get_combined_minimum_size().y : 0.0F) + "}}";
    }
    return json;
}

std::vector<EawrOptionsScreen::ControlState> EawrOptionsScreen::control_states() const {
    std::vector<ControlState> states;
    if (!state_ || !state_->dialog) return states;
    for (const BuiltGadget& gadget : state_->dialog->gadgets) {
        if (gadget.control == nullptr) continue;
        ControlState entry;
        entry.id = gadget.id;
        entry.kind = gadget.kind;
        entry.visible = gadget.control->is_visible();
        const auto reason = state_->reasons.find(gadget.id);
        entry.enabled = reason == state_->reasons.end();
        if (!entry.enabled) entry.reason = reason->second;
        entry.text = gadget.caption;
        if (const auto* slider = Object::cast_to<EawrUiSlider>(gadget.control)) {
            entry.value = slider->get_value();
        } else if (const auto* list = Object::cast_to<EawrUiList>(gadget.control)) {
            const PackedInt32Array selected = const_cast<EawrUiList*>(list)->get_selected_items();
            entry.value = selected.size() > 0 ? selected[0] : -1;
            if (selected.size() > 0) entry.text = utf8(list->get_item_text(selected[0]));
        } else if (const auto* combo = Object::cast_to<EawrUiCombo>(gadget.control)) {
            entry.value = combo->get_selected();
            if (combo->get_selected() >= 0) entry.text = utf8(combo->get_item_text(combo->get_selected()));
        } else if (const auto* edit = Object::cast_to<LineEdit>(gadget.control)) {
            // Reports describe the control state, never a service address, pin or local path.
            entry.text = gadget.id == "IDC_ONLINE_SERVICE_HOST" || gadget.id == "IDC_ONLINE_SERVICE_PIN" || gadget.id == "IDC_ONLINE_SERVICE_CA"
                ? (edit->get_text().is_empty() ? "" : "configured") : utf8(edit->get_text());
        } else if (const auto* button = Object::cast_to<BaseButton>(gadget.control)) {
            entry.value = button->is_pressed() ? 1.0 : 0.0;
            if (button->is_disabled() && entry.enabled) entry.enabled = false;
        }
        states.push_back(std::move(entry));
    }
    return states;
}

} // namespace eawr::presentation::godot_backend
