#include "lan_lobby_screen.hpp"
#include "online_service_settings.hpp"
#include "settings_store.hpp"
#include "skirmish_prepare.hpp"

#include "viewer_path.hpp"

#include "ui/dialog_builder.hpp"
#include "ui/font_provider.hpp"
#include "ui/input_routing.hpp"
#include "ui/kit.hpp"
#include "ui/message_box_view.hpp"
#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/net/tcp_transport.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/lan_lobby.hpp"
#include "eawr/presentation/ui/theme.hpp"
#include "eawr/skirmish/setup.hpp"
#include "eawr/scene/scene.hpp"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <map>
#include <functional>
#include <optional>
#include <set>
#include <system_error>
#include <tuple>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace model = presentation::ui;
namespace data = eawr::data::ui;
static_assert(model::lan_command_seat_limit == sim::max_match_seats);

namespace {

// Above the loading screen (98) and under the message box (100): modal, so nothing under it takes
// input (UI-I1).
constexpr int screen_layer = 99;

// The retail game types the host options list shows, with the text keys of the name and of the
// description (debug build, MNU-27). Only the space game is playable in this build.
struct GameTypeEntry {
    const char* name;
    const char* description;
    bool available;
};
constexpr GameTypeEntry game_types[] = {
    {"TEXT_GAME_TYPE_CAMPAIGN", "TEXT_GAME_DESCRIPTION_CAMPAIGN", false},
    {"TEXT_GAME_TYPE_TEAM_LAND", "TEXT_GAME_DESCRIPTION_TEAM_LAND", false},
    {"TEXT_GAME_TYPE_TEAM_SPACE", "TEXT_GAME_DESCRIPTION_TEAM_SPACE01", true},
    {"TEXT_GAME_TYPE_CAPTURE_CONTROL_POINTS", "TEXT_GAME_DESCRIPTION_CAPTURE_CONTROL_POINTS", false},
};

constexpr const char* lobby_dialog = "IDD_LAN_LOBBY_DIALOG";
constexpr const char* host_dialog = "IDD_INTERNET_HOST_OPTIONS_DIALOG";
constexpr const char* staging_dialog = "IDD_LAN_STAGING_DIALOG";
constexpr const char* filter_dialog = "IDD_LOBBY_GAME_LIST_FILTER_DIALOG";
constexpr const char* direct_dialog = "IDD_IP_DIRECT_CONNECT_DIALOG";

// Our own extra control on the lobby: retail has no way in for a typed address.
constexpr const char* direct_button = "EAWR_LAN_DIRECT_CONNECT_BUTTON";
constexpr const char* lan_source_button = "EAWR_LOBBY_LAN_SOURCE";
constexpr const char* online_source_button = "EAWR_LOBBY_ONLINE_SOURCE";
constexpr const char* invitation_edit = "EAWR_LOBBY_INVITATION_CODE";
constexpr const char* cancel_join_button = "EAWR_LOBBY_CANCEL_JOIN";
constexpr const char* refresh_button = "EAWR_LOBBY_REFRESH";
constexpr const char* prev_button = "EAWR_LOBBY_PREV_PAGE";
constexpr const char* next_button = "EAWR_LOBBY_NEXT_PAGE";
constexpr const char* page_label = "EAWR_LOBBY_PAGE_LABEL";
constexpr const char* code_button = "EAWR_LOBBY_JOIN_CODE";
constexpr const char* online_notice_label = "EAWR_LOBBY_ONLINE_NOTICE";
constexpr const char* prompt_panel = "EAWR_LOBBY_PROMPT";
constexpr const char* prompt_edit = "EAWR_LOBBY_PROMPT_PASSWORD";
constexpr const char* prompt_title_label = "EAWR_LOBBY_PROMPT_TITLE";
constexpr const char* prompt_error_label = "EAWR_LOBBY_PROMPT_ERROR";
constexpr const char* prompt_ok_button = "EAWR_LOBBY_PROMPT_OK";
constexpr const char* prompt_cancel_button = "EAWR_LOBBY_PROMPT_CANCEL";

[[nodiscard]] String text(const std::string& value) {
    return String::utf8(value.c_str(), static_cast<int64_t>(value.size()));
}

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}

[[nodiscard]] bool same_ci(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// True when the two names differ by at most one insertion, deletion, substitution or adjacent swap.
[[nodiscard]] bool within_one_edit(const std::string& a, const std::string& b) {
    if (a.size() > b.size() + 1 || b.size() > a.size() + 1) return false;
    if (a.size() == b.size()) {
        std::vector<std::size_t> differ;
        for (std::size_t index = 0; index < a.size() && differ.size() <= 2; ++index) {
            if (std::tolower(static_cast<unsigned char>(a[index])) != std::tolower(static_cast<unsigned char>(b[index]))) {
                differ.push_back(index);
            }
        }
        if (differ.size() <= 1) return true;
        return differ.size() == 2 && differ[1] == differ[0] + 1
            && std::tolower(static_cast<unsigned char>(a[differ[0]])) == std::tolower(static_cast<unsigned char>(b[differ[1]]))
            && std::tolower(static_cast<unsigned char>(a[differ[1]])) == std::tolower(static_cast<unsigned char>(b[differ[0]]));
    }
    const std::string& shorter = a.size() < b.size() ? a : b;
    const std::string& longer = a.size() < b.size() ? b : a;
    std::size_t skipped = 0;
    for (std::size_t index = 0, other = 0; index < shorter.size(); ++other) {
        if (other >= longer.size()) return false;
        if (std::tolower(static_cast<unsigned char>(shorter[index])) == std::tolower(static_cast<unsigned char>(longer[other]))) {
            ++index;
        } else if (++skipped > 1) {
            return false;
        }
    }
    return true;
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

// "MP_Color_Red" shows as "Red".
[[nodiscard]] std::string colour_name(const std::string& constant) {
    constexpr std::string_view prefix = "MP_Color_";
    return constant.compare(0, prefix.size(), prefix) == 0 ? constant.substr(prefix.size()) : constant;
}

[[nodiscard]] bool same_slots(const std::optional<std::vector<skirmish::LobbySlot>>& a,
                              const std::optional<std::vector<skirmish::LobbySlot>>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    if (a->size() != b->size()) return false;
    for (std::size_t index = 0; index < a->size(); ++index) {
        const skirmish::LobbySlot& x = (*a)[index];
        const skirmish::LobbySlot& y = (*b)[index];
        if (x.slot != y.slot || x.faction != y.faction || x.team != y.team || x.human != y.human
            || x.colour_index != y.colour_index) return false;
    }
    return true;
}

[[nodiscard]] std::uint32_t now_ms() { return static_cast<std::uint32_t>(Time::get_singleton()->get_ticks_msec()); }

} // namespace

// The lobby's game list: the retail columns at the retail positions of the row (name, type,
// players, map, ping), a selection bar, the wheel and a double click to join. An ItemList draws one
// text per row, so the five columns need their own control.
class EawrLobbyGameList final : public Control {
    GDCLASS(EawrLobbyGameList, Control)

public:
    struct Row {
        std::array<String, 5> cells;
        Color colour;
        bool locked{}; // a password room: a padlock before the name
    };

    EawrLobbyGameList() {
        set_theme_type_variation("EawrUi");
        set_focus_mode(FOCUS_ALL);
        set_clip_contents(true);
    }
    void set_rows(std::vector<Row> rows, const int selected) {
        rows_ = std::move(rows);
        selected_ = selected;
        if (first_ >= rows_.size()) first_ = rows_.empty() ? 0 : rows_.size() - 1;
        queue_redraw();
    }
    [[nodiscard]] int selected() const noexcept { return selected_; }
    void select(const int index) {
        selected_ = index;
        queue_redraw();
    }
    [[nodiscard]] std::size_t row_count() const noexcept { return rows_.size(); }

    void _draw() override {
        const Vector2 size = get_size();
        draw_small_frame(*this, Rect2(Vector2(), size), true);
        const KitTextStyle style = kit_text_style(*this, "List_Box");
        const float height = row_height(style);
        const float pad = 4.0F;
        constexpr float columns[5] = {0.0F, 0.25F, 0.45F, 0.56F, 0.91F};
        const std::size_t visible = static_cast<std::size_t>(std::max(1.0F, (size.y - 2.0F * pad) / height));
        for (std::size_t line = 0; line < visible && line + first_ < rows_.size(); ++line) {
            const std::size_t index = line + first_;
            const float y = pad + static_cast<float>(line) * height;
            if (static_cast<int>(index) == selected_) {
                draw_rect(Rect2(pad, y, size.x - 2.0F * pad, height), Color(0.12F, 0.32F, 0.52F, 0.65F));
            }
            const float lock_width = rows_[index].locked ? height * 0.8F : 0.0F;
            if (rows_[index].locked) {
                // The retail look has no lock art: a small body and shackle in the row's colour.
                const float side = height * 0.5F;
                const float left = pad + 3.0F;
                const float top = y + height * 0.3F;
                draw_rect(Rect2(left, top + side * 0.45F, side, side * 0.6F), rows_[index].colour);
                draw_arc(Vector2(left + side * 0.5F, top + side * 0.45F), side * 0.28F, 3.14159265F, 6.2831853F, 12,
                         rows_[index].colour, 1.5F);
            }
            for (std::size_t column = 0; column < 5; ++column) {
                if (style.font.is_null()) continue;
                const float shift = column == 0 ? lock_width : 0.0F;
                const float x = pad + 2.0F + columns[column] * (size.x - 2.0F * pad) + shift;
                const float width = (column == 4 ? 1.0F : columns[column + 1]) * (size.x - 2.0F * pad)
                    - columns[column] * (size.x - 2.0F * pad) - 4.0F - shift;
                draw_string(style.font, Vector2(x, y + height * 0.75F), rows_[index].cells[column],
                            HORIZONTAL_ALIGNMENT_LEFT, width, style.size, rows_[index].colour);
            }
        }
    }

    void _gui_input(const Ref<InputEvent>& event) override {
        const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr());
        if (button == nullptr || !button->is_pressed()) return;
        const KitTextStyle style = kit_text_style(*this, "List_Box");
        const float height = row_height(style);
        if (button->get_button_index() == MOUSE_BUTTON_WHEEL_UP) {
            if (first_ > 0) --first_;
            queue_redraw();
        } else if (button->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN) {
            if (first_ + 1 < rows_.size()) ++first_;
            queue_redraw();
        } else if (button->get_button_index() == MOUSE_BUTTON_LEFT) {
            const float y = button->get_position().y - 4.0F;
            if (y < 0.0F) return;
            const std::size_t index = first_ + static_cast<std::size_t>(y / height);
            if (index >= rows_.size()) return;
            selected_ = static_cast<int>(index);
            queue_redraw();
            emit_signal("row_chosen", static_cast<int64_t>(index), button->is_double_click());
            accept_event();
        }
    }

protected:
    static void _bind_methods() {
        ADD_SIGNAL(MethodInfo("row_chosen", PropertyInfo(Variant::INT, "index"), PropertyInfo(Variant::BOOL, "activated")));
    }

private:
    [[nodiscard]] static float row_height(const KitTextStyle& style) {
        return std::max(12.0F, static_cast<float>(style.size) * 1.45F);
    }

    std::vector<Row> rows_;
    std::size_t first_{};
    int selected_{-1};
};

struct EawrLanLobby::Resources final {
    std::shared_ptr<const BattleContent> content;
    std::optional<data::DialogCatalog> dialogs;
    std::optional<assets::MegaTextureAtlas> atlas;
    std::unique_ptr<UiTextures> textures;
    std::unique_ptr<FontProvider> fonts;
    std::optional<vfs::Vfs> font_filesystem;
    std::optional<data::TextDatabase> text_database;
    std::optional<data::TextLookup> text;
    std::optional<model::ThemeModel> theme_model;
    Ref<Theme> theme;
    Vector2 theme_size;
    std::vector<skirmish::SetupMap> maps;
    std::vector<skirmish::LobbyColour> palette;
    skirmish::MatchOptions defaults;
    std::vector<std::string> sfx_ids; // the effective SFX events, for the chat sounds
};

struct EawrLanLobby::PageState final {
    Control* root{};
    EawrUiCheck* port_mapping{};
    std::optional<BuiltDialog> dialog;
    std::map<std::string, Control*> controls;
    EawrLobbyGameList* game_list{};
    TextureRect* preview{};
    std::map<std::string, ColorRect*> accept_icons;
    std::size_t chat_drawn{};
    // New chat lines were added: the list scrolls to the end once its layout has caught up.
    bool chat_scroll{};
    std::size_t dialog_rows{};
    // Set while a control is being filled: its signals must not edit the model.
    bool loading{};
};

struct EawrLanLobby::Impl final {
    std::shared_ptr<model::LobbyGameSource> source;
    std::shared_ptr<net::OnlineRoomSource> online_rooms;
    std::size_t online_page{};
    std::size_t online_rooms_total{};
    std::optional<model::OnlinePasswordPrompt> prompt;
    model::OnlineHostingPhase hosting{model::OnlineHostingPhase::idle};
    std::string join_game, join_room; // the online room being joined: its name and id
    std::string host_password;        // the password of the room being hosted; never logged or shown
    std::string list_notice;
    std::string last_online_notice; // the last service notice shown, so a retrying call does not repeat it
    bool chat_open{};
    model::LobbyJoinSelection join_selection;
    std::string entered_code;
    model::LobbyFilter filter;
    model::LobbySort sort{model::LobbySort::game_name};
    std::vector<model::LobbyGame> games; // as announced
    std::vector<model::LobbyGame> shown; // filtered and sorted
    std::string selected_key;            // name|address of the selected game
    std::vector<model::LobbyPlayer> players;
    model::ChatLog lobby_chat;
    model::ChatLog staging_chat;
    std::string name;
    std::uint32_t colour{};
    std::vector<std::string> direct_addresses;
    std::uint32_t last_list_ms{};
    bool dirty{true};

    // Staging
    bool host{};
    std::string game;
    std::string address;
    std::string host_name;
    std::string last_message;
    model::HostOptions host_options;
    std::vector<sim::tactical::PlayerId> human_owners{1, 2};
    std::uint32_t direct_seats{2};
    skirmish::SetupSelection selection;
    bool custom_maps{};
    int map_index{-1};                  // into resources.maps
    std::vector<std::size_t> listed;    // map indexes in the list
    std::optional<skirmish::FixtureOptions> launch;
    std::optional<skirmish::FixtureOptions> offered;
    bool locked{};
    bool refused{};
    EawrMessageBox* box{};
    // The rows as last shown, for the report and the start checks.
    std::vector<model::StagingRow> rows;
    std::vector<EawrLanLobby::RowState> row_states;
    std::string rows_signature;
    std::string preview_path;
    std::string status;
    std::string start_caption;
    bool start_enabled{};
    std::size_t visible_rows{};
    bool remote_colour_applied{};
    std::uint64_t room_chat_sequence{};
    std::uint64_t room_revision{};
    std::string room_notice;
    bool linked{}; // the transport's link to the other player is open
};

EawrLanLobby::EawrLanLobby() {
    set_layer(screen_layer);
    set_visible(false);
}

EawrLanLobby::~EawrLanLobby() = default;

void EawrLanLobby::_bind_methods() {
    ADD_SIGNAL(MethodInfo("closed"));
    ADD_SIGNAL(MethodInfo("started"));
    ADD_SIGNAL(MethodInfo("profile_changed", PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::INT, "colour")));
    ADD_SIGNAL(MethodInfo("sound_requested", PropertyInfo(Variant::STRING, "event")));
}

void register_lan_lobby_classes() {
    GDREGISTER_CLASS(EawrLobbyGameList);
    GDREGISTER_CLASS(EawrLanLobby);
}

// ---- Loading ----------------------------------------------------------------------------------

bool EawrLanLobby::build(Options options) {
    options_ = std::move(options);
    resources_ = std::make_unique<Resources>();
    impl_ = std::make_unique<Impl>();
    Resources& resources = *resources_;
    impl_->name = model::clamp_player_name(options_.player_name);
    if (impl_->name.empty()) impl_->name = "Player";
    impl_->colour = options_.player_colour;
    impl_->lobby_chat = model::ChatLog();
    impl_->online_rooms = options_.online_rooms;
    if (!options_.online_settings) {
        if (const auto* saved = ActiveSettings::get()) options_.online_settings = saved->network;
        else options_.online_settings = SettingsStore{}.load().settings.network;
    }

    resources.content = options_.content;
    if (!resources.content) {
        if (options_.game_root.empty()) {
            failure_ = "the LAN screens need the game root or the content a menu loaded";
            return false;
        }
        const std::filesystem::path base = options_.game_root / "GameData" / "Data";
        const std::filesystem::path expansion = options_.game_root / "corruption" / "Data";
        if (!std::filesystem::is_directory(base) || !std::filesystem::is_directory(expansion)) {
            failure_ = "the LAN screens need an installation with GameData/Data and corruption/Data";
            return false;
        }
        const std::string profile = options_.profile.empty() ? (options_.mod_root.empty() ? "foc" : "remake")
                                                             : options_.profile;
        if (profile != "foc" && profile != "remake") {
            failure_ = "the LAN screens need the FoC or mod profile";
            return false;
        }
        std::vector<vfs::LayerRoot> roots;
        if (profile == "remake") {
            if (options_.mod_root.empty()) {
                failure_ = "the mod profile needs a mod root";
                return false;
            }
            roots = vfs::mod_chain_roots(options_.mod_root);
        }
        roots.emplace_back("expansion", expansion);
        roots.emplace_back("base", base);
        auto chain = vfs::resolve_manifest_chain(roots);
        if (!chain) {
            failure_ = core::format_diagnostic(chain.error());
            return false;
        }
        std::vector<vfs::MountSpec> specs;
        std::vector<std::string> layers;
        for (auto& manifest : chain.value()) {
            layers.push_back(manifest.mount.layer_id);
            specs.push_back(std::move(manifest.mount));
        }
        auto mounted = vfs::Vfs::mount(specs);
        if (!mounted) {
            failure_ = core::format_diagnostic(mounted.error());
            return false;
        }
        auto filesystem = std::make_shared<const vfs::Vfs>(std::move(mounted).value());
        auto objects = eawr::data::load_catalog_strict(*filesystem, profile == "foc" ? eawr::data::Profile::foc
                                                                               : eawr::data::Profile::remake);
        if (!objects) {
            failure_ = core::format_diagnostic(objects.error());
            return false;
        }
        auto catalog = std::make_shared<const eawr::data::Catalog>(std::move(objects.value().catalog));
        resources.content = std::make_shared<const BattleContent>(
            BattleContent{std::move(filesystem), std::move(catalog), profile, std::move(layers)});
    }
    const vfs::Vfs& filesystem = *resources.content->filesystem;

    auto colours = skirmish::read_lobby_colours(filesystem);
    if (!colours) {
        failure_ = colours.error().message;
        return false;
    }
    resources.palette = std::move(colours).value();
    auto defaults = skirmish::read_match_defaults(filesystem);
    if (!defaults) {
        failure_ = defaults.error().message;
        return false;
    }
    resources.defaults = std::move(defaults).value();
    skirmish::SetupMapQuery query;
    query.custom.reset();
    auto maps = skirmish::setup_maps(filesystem, *resources.content->catalog, query);
    if (!maps) {
        failure_ = maps.error().message;
        return false;
    }
    resources.maps = std::move(maps).value();

    auto dialogs = data::load_dialog_catalog(filesystem);
    if (!dialogs) {
        failure_ = core::format_diagnostic(dialogs.error());
        return false;
    }
    resources.dialogs.emplace(std::move(dialogs.value()));
    // A mod that removes a dialog removes its entry, with one note, never a crash.
    for (const char* required : {lobby_dialog, staging_dialog}) {
        if (resources.dialogs->script.find(required) == nullptr) {
            failure_ = std::string("the data has no dialog ") + required;
            return false;
        }
    }
    for (const char* optional : {host_dialog, filter_dialog, direct_dialog}) {
        if (resources.dialogs->script.find(optional) == nullptr) {
            notes_.push_back(std::string("dialog ") + optional + " is not in the data; its entry is disabled");
        }
    }
    auto atlas = assets::load_mega_texture_atlas(filesystem,
                                                 "data/art/textures/" + resources.dialogs->skin.texture_file + ".mtd");
    if (!atlas) {
        failure_ = core::format_diagnostic(atlas.error());
        return false;
    }
    resources.atlas.emplace(std::move(atlas.value()));
    resources.textures = std::make_unique<UiTextures>(*resources.atlas, filesystem);
    if (!resources.textures->page_failure().empty()) {
        failure_ = "atlas page: " + resources.textures->page_failure();
        return false;
    }
    if (auto database = data::load_language_text_database(filesystem, options_.language)) {
        resources.text_database.emplace(std::move(database.value()));
        resources.text.emplace(*resources.text_database);
    } else {
        notes_.push_back("text database: " + core::format_diagnostic(database.error()) + "; captions show their keys");
    }
    const std::filesystem::path cache = default_font_cache(options_.font_cache);
    const vfs::MountSpec mount{.layer_id = "font-cache", .data_root = cache,
                               .loose_logical_prefix = std::string(model::font_cache_prefix), .active_archives = {}};
    auto font_mount = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&mount, 1));
    if (font_mount) resources.font_filesystem.emplace(std::move(font_mount.value()));
    const vfs::Vfs unmounted;
    resources.fonts = std::make_unique<FontProvider>(model::load_font_cache(
        resources.font_filesystem ? *resources.font_filesystem : unmounted, ViewerPath::utf8(cache)));

    for (const eawr::data::Definition& definition : resources.content->catalog->definitions()) {
        if (definition.category == eawr::data::Category::sfx && definition.active) {
            resources.sfx_ids.push_back(definition.id);
        }
    }

    match_ = options_.match;
    if (match_) match_->configure_factions(scene::faction_order(*resources_->content->catalog));
    if (!match_) {
        const std::filesystem::path diagnostics = options_.diagnostic_root.empty()
            ? ViewerPath{utf8(OS::get_singleton()->get_user_data_dir())}.native() / "lan-desync"
            : options_.diagnostic_root;
        match_ = make_lan_match(diagnostics, options_.input_delay);
        match_->configure_factions(scene::faction_order(*resources_->content->catalog));
    }
    built_ = true;
    lan_match_ = match_;
    return true;
}

// ---- Showing and closing ----------------------------------------------------------------------

void EawrLanLobby::show(Node& parent, std::optional<model::LobbySourceKind> initial_source) {
    if (!built_ || shown_) return;
    parent.add_child(this);
    set_visible(true);
    shown_ = true;
    set_process(true);
    set_process_input(true);

    impl_->source = impl_->join_selection.source() == model::LobbySourceKind::lan ? options_.source
                  : std::shared_ptr<model::LobbyGameSource>(impl_->online_rooms);
    open_lobby();
    if (initial_source) select_source(*initial_source);
}

void EawrLanLobby::clear_page() {
    if (!state_) return;
    if (state_->root != nullptr) {
        remove_child(state_->root);
        state_->root->queue_free();
    }
    state_.reset();
}

void EawrLanLobby::close_screen(const bool emit) {
    if (!shown_) return;
    cancel_join();
    shown_ = false;
    set_process(false);
    set_process_input(false);
    clear_page();
    impl_->source.reset();
    set_visible(false);
    if (Node* parent = get_parent()) parent->remove_child(this);
    if (emit) emit_signal("closed");
}

void EawrLanLobby::dismiss() { close_screen(false); }

std::optional<skirmish::FixtureOptions> EawrLanLobby::take_start() { return std::exchange(impl_->launch, std::nullopt); }

// ---- Page building ----------------------------------------------------------------------------

void EawrLanLobby::build_page(const char* dialog_name) {
    clear_page();
    Resources& resources = *resources_;
    state_ = std::make_unique<PageState>();
    PageState& state = *state_;
    const Vector2 size = get_viewport()->get_visible_rect().size;
    if (resources.theme.is_null() || resources.theme_size != size) {
        model::ThemeSources sources;
        sources.catalog = &*resources.dialogs;
        sources.atlas = &resources.atlas->directory;
        sources.standalone = model::vfs_standalone_textures(*resources.content->filesystem);
        sources.fonts = &resources.fonts->cache();
        const std::vector<std::string>& families = resources.fonts->system_families();
        sources.system = [&families](const std::string_view face) {
            return model::match_system_face(face, families).has_value();
        };
        resources.theme_model.emplace(model::build_theme_model(
            sources, model::reference_space({static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y)},
                                            options_.rules)));
        resources.theme = build_theme(*resources.theme_model, *resources.textures, *resources.fonts);
        resources.theme_size = size;
    }
    auto* root = memnew(EawrUiModalLayer);
    root->set_name("EawrLanLobby");
    root->set_position(Vector2());
    root->set_size(size);
    root->set_theme(resources.theme);
    add_child(root);
    state.root = root;
    // The lobby and the staging are full-screen opaque dialogs: nothing of the world shows behind.
    auto* backdrop = memnew(ColorRect);
    backdrop->set_color(Color(0.0F, 0.0F, 0.0F, 1.0F));
    backdrop->set_size(size);
    backdrop->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    root->add_child(backdrop);

    const data::Dialog* dialog = resources.dialogs->script.find(dialog_name);
    state.dialog = build_dialog(*dialog, *resources.theme_model, resources.text ? &*resources.text : nullptr,
                                model::Placement::centre);
    root->add_child(state.dialog->frame);
    for (const BuiltGadget& gadget : state.dialog->gadgets) {
        if (gadget.control != nullptr) state.controls[gadget.id] = gadget.control;
    }
}

namespace {

template <class T>
[[nodiscard]] T* cast(Control* control) {
    return Object::cast_to<T>(control);
}

[[nodiscard]] Rect2 local_rect(const BuiltDialog& dialog, const BuiltGadget& gadget) {
    return Rect2(static_cast<float>(gadget.rect.x - dialog.layout.frame.x),
                 static_cast<float>(gadget.rect.y - dialog.layout.frame.y), static_cast<float>(gadget.rect.width),
                 static_cast<float>(gadget.rect.height));
}

[[nodiscard]] const BuiltGadget* find_gadget(const BuiltDialog& dialog, const std::string& id) {
    for (const BuiltGadget& gadget : dialog.gadgets) {
        if (gadget.id == id) return &gadget;
    }
    return nullptr;
}

} // namespace

#define EAWR_CONTROL(Type, id) cast<Type>(state_->controls.count(id) != 0 ? state_->controls.at(id) : nullptr)

// The text a key or literal shows, through the text database when there is one.
static String resolved(std::optional<data::TextLookup>& lookup, const std::string& value) {
    if (!lookup || value.empty()) return text(value);
    return text(data::to_utf8(lookup->text(value)));
}

void EawrLanLobby::on_pressed(String id) { handle_button(utf8(id)); }

void EawrLanLobby::on_text_submitted(String typed, String id) {
    const std::string gadget = utf8(id);
    const std::string value = utf8(typed);
    if (gadget == "IDC_LAN_NAME_EDIT") commit_name(value);
    else if (gadget.starts_with("EAWR_STAGING_NAME_")) static_cast<void>(run_action("name:" + value));
    else if (gadget == "IDC_LAN_CHAT_EDIT") send_chat(value);
    else if (gadget == "IDC_INTERNET_GAME_NAME_EDIT" || gadget == "IDC_INTERNET_GAME_PASSWORD_EDIT") handle_button("IDC_INTERNET_CREATE_GAME_BUTTON");
    else if (gadget == "IDC_EDIT_IP") handle_button("IDC_ADD_IP_BUTTON");
    else if (gadget == invitation_edit) join_code(value);
    else if (gadget == prompt_edit) submit_password(value);
}

void EawrLanLobby::on_box_result(const int64_t) {
    impl_->box = nullptr;
    // A refused or dropped connection leaves the staging for the lobby once the player read it.
    if (impl_->refused) {
        impl_->refused = false;
        leave_staging();
    }
}

// ---- Lobby ------------------------------------------------------------------------------------

void EawrLanLobby::open_lobby() {
    page_ = Page::lobby;
    impl_->prompt.reset();
    build_page(lobby_dialog);
    PageState& state = *state_;
    const BuiltDialog& dialog = *state.dialog;
    Resources& resources = *resources_;
    state.loading = true;

    // The retail dialog's second short list has no traced use: it stays hidden.
    if (auto* extra = EAWR_CONTROL(Control, "IDC_GAME_LIST2")) extra->set_visible(false);
    if (const BuiltGadget* gadget = find_gadget(dialog, "IDC_GAME_LIST")) {
        state.controls["IDC_GAME_LIST"]->set_visible(false);
        state.game_list = memnew(EawrLobbyGameList);
        state.game_list->set_name("IDC_GAME_LIST");
        Rect2 rect = local_rect(dialog, *gadget);
        // Prefer the unused second list's space, keeping the loaded LAN list geometry intact.
        const BuiltGadget* spare = find_gadget(dialog, "IDC_GAME_LIST2");
        const Rect2 selector = spare != nullptr ? local_rect(dialog, *spare) : rect;
        const float selector_height = std::min(28.0F, selector.size.y);
        for (const auto& [id, caption] : std::array<std::pair<const char*, const char*>, 2>{{
                 {lan_source_button, "LAN"}, {online_source_button, "Online"}}}) {
            auto* button = memnew(EawrUiButton);
            button->set_name(id);
            button->set_text(caption);
            const bool lan = std::string_view(id) == lan_source_button;
            const float x = lan ? 0.0F : selector.size.x / 2.0F;
            button->set_position(selector.position + Vector2(x, 0.0F));
            button->set_size(Vector2(selector.size.x / 2.0F, selector_height));
            button->set_disabled(lan ==
                (impl_->join_selection.source() == model::LobbySourceKind::lan));
            dialog.frame->add_child(button);
            button->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(id)));
            state.controls[id] = button;
        }
        if (spare == nullptr) {
            rect.position.y += selector_height + 8.0F;
            rect.size.y = std::max(0.0F, rect.size.y - selector_height - 8.0F);
        }
        const bool online = impl_->join_selection.source() == model::LobbySourceKind::online;
        // Online adds two small rows under the list: the room code and the join/cancel buttons, then
        // refresh, paging and the page count. They reuse the dialog's own controls; there is no new art.
        constexpr float row_height = 28.0F, row_gap = 4.0F;
        const float footer = 2.0F * row_height + row_gap + 6.0F;
        const Rect2 list_rect(rect.position, Vector2(rect.size.x, online ? std::max(0.0F, rect.size.y - footer) : rect.size.y));
        state.game_list->set_position(list_rect.position);
        state.game_list->set_size(list_rect.size);
        dialog.frame->add_child(state.game_list);
        state.game_list->connect("row_chosen", callable_mp(this, &EawrLanLobby::on_rows_chosen));
        if (online) {
            const float width = rect.size.x;
            const float top = rect.position.y + rect.size.y - footer + 3.0F;
            const float second = top + row_height + row_gap;
            const auto add_button = [&](const char* id, const char* caption, const float x, const float y, const float w) {
                auto* button = memnew(EawrUiButton);
                button->set_name(id);
                button->set_text(caption);
                button->set_position(Vector2(rect.position.x + x * width, y));
                button->set_size(Vector2(w * width, row_height));
                dialog.frame->add_child(button);
                button->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(id)));
                state.controls[id] = button;
            };
            auto* notice = memnew(EawrUiLabel);
            notice->set_name(online_notice_label);
            notice->set_position(list_rect.position + Vector2(8.0F, 40.0F));
            notice->set_size(Vector2(std::max(0.0F, list_rect.size.x - 16.0F), 48.0F));
            notice->set_wrap(true);
            notice->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            dialog.frame->add_child(notice);
            state.controls[online_notice_label] = notice;
            auto* code = memnew(EawrUiEdit);
            code->set_name(invitation_edit);
            code->set_placeholder("Room code");
            code->set_text(text(impl_->entered_code));
            code->set_position(Vector2(rect.position.x, top));
            code->set_size(Vector2(0.44F * width, row_height));
            code->set_max_length(128);
            dialog.frame->add_child(code);
            code->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String(invitation_edit)));
            state.controls[invitation_edit] = code;
            add_button(code_button, "Join code", 0.46F, top, 0.24F);
            add_button(cancel_join_button, "Cancel join", 0.72F, top, 0.28F);
            add_button(refresh_button, "Refresh", 0.0F, second, 0.2F);
            add_button(prev_button, "<", 0.22F, second, 0.1F);
            add_button(next_button, ">", 0.34F, second, 0.1F);
            auto* pages = memnew(EawrUiLabel);
            pages->set_name(page_label);
            pages->set_position(Vector2(rect.position.x + 0.46F * width, second + 4.0F));
            pages->set_size(Vector2(0.54F * width, row_height));
            pages->set_wrap(false);
            pages->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            dialog.frame->add_child(pages);
            state.controls[page_label] = pages;
        }
    }
    if (auto* name = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) {
        name->set_text(text(impl_->name));
        name->set_max_length(static_cast<int>(model::lan_name_max));
        name->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String("IDC_LAN_NAME_EDIT")));
        name->connect("focus_exited", callable_mp(this, &EawrLanLobby::commit_name_from_edit));
    }
    if (auto* chat = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) {
        chat->set_max_length(static_cast<int>(model::lan_chat_max));
        chat->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String("IDC_LAN_CHAT_EDIT")));
    }
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_LAN_CHAT_LIST")) list->set_select_mode(ItemList::SELECT_SINGLE);
    if (auto* colour = EAWR_CONTROL(EawrUiCombo, "IDC_LAN_COLOR_SELECT")) {
        for (std::size_t index = 0; index < resources.palette.size(); ++index) {
            const auto& rgb = resources.palette[index].rgb;
            Ref<Image> swatch = Image::create_empty(12, 12, false, Image::FORMAT_RGBA8);
            swatch->fill(Color(rgb[0] / 255.0F, rgb[1] / 255.0F, rgb[2] / 255.0F));
            colour->add_icon_item(ImageTexture::create_from_image(swatch), text(colour_name(resources.palette[index].constant)),
                                  static_cast<int>(index));
        }
        const int count = colour->get_item_count();
        if (count > 0) colour->select(std::min<int>(static_cast<int>(impl_->colour), count - 1));
        colour->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(String("IDC_LAN_COLOR_SELECT")));
    }
    for (const BuiltGadget& gadget : dialog.gadgets) {
        if (gadget.kind != "button" || gadget.control == nullptr) continue;
        gadget.control->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(text(gadget.id)));
    }
    if (resources.dialogs->script.find(host_dialog) == nullptr) {
        if (auto* host = EAWR_CONTROL(EawrUiButton, "IDC_LAN_HOST_GAME_BUTTON")) {
            host->set_disabled(true);
            host->set_tooltip_text("The host options dialog is not in the loaded data.");
        }
    }
    if (resources.dialogs->script.find(filter_dialog) == nullptr) {
        if (auto* filter = EAWR_CONTROL(EawrUiButton, "IDC_SET_FILTER_BUTTON")) filter->set_disabled(true);
    }
    // Our extra: a typed address, for networks the broadcast does not cross.
    if (resources.dialogs->script.find(direct_dialog) != nullptr) {
        if (const BuiltGadget* filter = find_gadget(dialog, "IDC_SET_FILTER_BUTTON")) {
            const Rect2 set = local_rect(dialog, *filter);
            if (const BuiltGadget* host = find_gadget(dialog, "IDC_LAN_HOST_GAME_BUTTON")) {
                const Rect2 hosted = local_rect(dialog, *host);
                auto* direct = memnew(EawrUiButton);
                direct->set_name(direct_button);
                direct->set_text(resolved(resources.text, "TEXT_IP"));
                const float left = set.position.x + set.size.x + 8.0F;
                direct->set_position(Vector2(left, set.position.y));
                const float width = std::max(0.0F, std::min(hosted.position.x - 8.0F,
                    dialog.frame->get_size().x) - left);
                direct->set_size(Vector2(width, set.size.y));
                direct->set_visible(width > 0.0F);
                direct->set_tooltip_text(resolved(resources.text, "TEXT_IP_DIRECT_CONNECT"));
                dialog.frame->add_child(direct);
                direct->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(direct_button)));
                state.controls[direct_button] = direct;
            }
        }
    }
    if (impl_->join_selection.source() == model::LobbySourceKind::online) {
        // The online list keeps the sort headers; the type column shows the host. The filter and the
        // typed address are LAN features.
        for (const char* id : {"IDC_SET_FILTER_BUTTON", direct_button}) {
            if (auto* button = EAWR_CONTROL(EawrUiButton, id)) button->set_disabled(true);
        }
        if (auto* host_column = EAWR_CONTROL(EawrUiButton, "IDC_INTERNET_GAME_TYPE_BUTTON")) host_column->set_text("Host");
        if (auto* host = EAWR_CONTROL(EawrUiButton, "IDC_LAN_HOST_GAME_BUTTON")) {
            host->set_text("Host Online");
            host->set_disabled(!impl_->online_rooms || resources.dialogs->script.find(host_dialog) == nullptr);
            host->set_tooltip_text(impl_->online_rooms
                ? "Host a public room, or a private one with a password."
                : "Online play is not available in this build or service configuration.");
        }
        if (auto* join = EAWR_CONTROL(EawrUiButton, "IDC_LAN_JOIN_GAME_BUTTON")) join->set_disabled(!impl_->online_rooms);
    }
    state.loading = false;
    // The player list shows who is in the lobby; the first line is the player.
    impl_->dirty = true;
    announce();
    refresh_lobby();
    if (auto* chat = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) chat->grab_focus();
    if (impl_->name == "Player") {
        // Retail focuses the name edit while the profile still has the default name.
        if (auto* name = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) name->grab_focus();
    }
}

// Adds the lines the list has not shown yet. A key or a literal notice shows through the text
// database; a player's line shows as typed. Retail's chat list is not selectable.
void EawrLanLobby::draw_chat(EawrUiList& list, const model::ChatLog& log) {
    PageState& state = *state_;
    const auto& lines = log.lines();
    if (state.chat_scroll) {
        if (VScrollBar* bar = list.get_v_scroll_bar()) bar->set_value(bar->get_max());
        state.chat_scroll = false;
    }
    // chat_drawn counts lines ever drawn, so a full log (oldest line dropped per add) still shows new lines.
    const std::size_t first_kept = log.total() - lines.size();
    if (state.chat_drawn < first_kept) state.chat_drawn = first_kept;
    if (state.chat_drawn > log.total()) state.chat_drawn = log.total();
    for (; state.chat_drawn < log.total(); ++state.chat_drawn) {
        const model::ChatLine& line = lines[state.chat_drawn - first_kept];
        const bool is_key = line.sender.empty() && line.text.rfind("TEXT_", 0) == 0;
        const int index = list.add_item(is_key ? resolved(resources_->text, line.text) : text(line.text));
        list.set_item_selectable(index, false);
        if (line.kind == model::ChatKind::error) list.set_item_custom_fg_color(index, Color(1.0F, 0.2F, 0.2F));
        else if (line.kind == model::ChatKind::notice) list.set_item_custom_fg_color(index, Color(1.0F, 0.9F, 0.4F));
        state.chat_scroll = true;
        // The list keeps what the log keeps: the oldest line goes when it is full.
        while (static_cast<std::size_t>(list.get_item_count()) > log.capacity()) list.remove_item(0);
    }
}

void EawrLanLobby::commit_name_from_edit() {
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) commit_name(utf8(edit->get_text()));
}

void EawrLanLobby::note_chat(const model::ChatKind kind, const std::string& line, const std::string& sender) {
    (page_ == Page::staging ? impl_->staging_chat : impl_->lobby_chat).add(kind, line, sender);
    impl_->dirty = true; // the lobby redraws its lists on a timer otherwise
}

void EawrLanLobby::play_sound(const std::string& wanted) {
    // The sound the debug build names is looked up in the effective data; a name the data spells
    // differently resolves to the one event within one edit of it (docs/ui/menu-screens.md).
    std::string found;
    for (const std::string& id : resources_->sfx_ids) {
        if (same_ci(id, wanted)) {
            found = id;
            break;
        }
    }
    if (found.empty()) {
        std::size_t matches = 0;
        for (const std::string& id : resources_->sfx_ids) {
            if (within_one_edit(id, wanted)) {
                found = id;
                ++matches;
            }
        }
        if (matches != 1) found.clear();
    }
    if (found.empty()) return;
    if (sounds_.size() < 64) sounds_.push_back(found);
    if (!options_.silent) emit_signal("sound_requested", text(found));
}

void EawrLanLobby::commit_name(const std::string& typed) {
    const std::string name = model::clamp_player_name(typed);
    if (name.empty() || name == impl_->name) {
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) edit->set_text(text(impl_->name));
        return;
    }
    impl_->name = name;
    emit_signal("profile_changed", text(impl_->name), static_cast<int64_t>(impl_->colour));
    announce();
    impl_->dirty = true;
    refresh_lobby();
    if (auto* chat = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) chat->grab_focus();
}

void EawrLanLobby::commit_colour(const std::uint32_t colour) {
    if (colour == impl_->colour) return;
    impl_->colour = colour;
    emit_signal("profile_changed", text(impl_->name), static_cast<int64_t>(impl_->colour));
    announce();
    impl_->dirty = true;
}

void EawrLanLobby::send_chat(const std::string& typed) {
    const std::optional<std::string> line = model::chat_line_to_send(typed);
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) {
        edit->set_text("");
        edit->grab_focus();
    }
    if (!line) return;
    if (impl_->join_selection.source() == model::LobbySourceKind::online && page_ == Page::lobby) {
        // The service carries chat for the room the player is in; the service sends the line back, so
        // there is no local echo. Without a room the window says why nothing was sent.
        const auto members = impl_->online_rooms ? impl_->online_rooms->players() : std::vector<model::LobbyPlayer>{};
        if (members.empty()) {
            note_chat(model::ChatKind::notice, std::string(model::online_chat_closed_notice()));
            return;
        }
        impl_->online_rooms->send_chat(model::LobbyChatScope::game, members.front().game, impl_->name, *line);
        play_sound("GUI_Common_Chat_Send");
        return;
    }
    if (page_ == Page::staging && match_->lobby_enabled()) {
        if (match_->lobby_chat(*line)) play_sound("GUI_Common_Chat_Send");
        else if (!match_->lobby_state()) note_chat(model::ChatKind::notice, "TEXT_WAITING");
        return;
    }
    if (impl_->source) {
        impl_->source->send_chat(page_ == Page::staging ? model::LobbyChatScope::game : model::LobbyChatScope::lobby,
                                 page_ == Page::staging ? impl_->game : std::string{}, impl_->name, *line);
    }
    note_chat(model::ChatKind::normal, impl_->name + ": " + *line, impl_->name);
    play_sound("GUI_Common_Chat_Send");
}

void EawrLanLobby::announce() {
    if (!impl_->source) return;
    model::LobbyPresence presence;
    presence.name = impl_->name;
    presence.colour = impl_->colour;
    if (page_ == Page::staging && impl_->host && match_->view().phase != LanPhase::ended) {
        presence.place = model::LobbyPlace::hosting;
        presence.game = impl_->game;
        presence.map = impl_->map_index >= 0 ? resources_->maps[static_cast<std::size_t>(impl_->map_index)].name : std::string{};
        presence.game_type = impl_->host_options.game_type;
        presence.tcp_port = match_ ? match_->view().port : 0;
        presence.max_players = static_cast<std::uint32_t>(impl_->human_owners.size());
        presence.passworded = impl_->join_selection.source() == model::LobbySourceKind::online
            && !impl_->host_password.empty();
        presence.locked = impl_->locked;
    } else if (page_ == Page::staging && match_->view().phase != LanPhase::ended) {
        presence.place = model::LobbyPlace::joining;
        presence.game = impl_->game;
    }
    impl_->source->announce(presence);
}

void EawrLanLobby::on_rows_chosen(const int64_t index, const bool activated) {
    if (index < 0 || static_cast<std::size_t>(index) >= impl_->shown.size()) return;
    const model::LobbyGame& game = impl_->shown[static_cast<std::size_t>(index)];
    impl_->selected_key = game.name + "|" + game.address;
    if (activated) {
        if (impl_->join_selection.source() == model::LobbySourceKind::online) join_online_selected();
        else join_selected();
    }
}

void EawrLanLobby::on_item_selected(const int64_t index, String id) {
    if (state_ && state_->loading) return;
    const std::string gadget = utf8(id);
    if (gadget == "IDC_INTERNET_HOST_CLAN_COMBO") {
        if (auto* combo = EAWR_CONTROL(EawrUiCombo, gadget)) {
            const auto count = static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index)));
            fill_host_seats(count);
        }
        return;
    }
    if (gadget == "EAWR_LAN_HOST_TEAMS") {
        if (auto* combo = EAWR_CONTROL(EawrUiCombo, gadget)) {
            const int value = combo->get_item_id(static_cast<int>(index));
            const auto first = static_cast<std::uint32_t>(value / 10);
            const auto second = static_cast<std::uint32_t>(value % 10);
            const auto count = static_cast<std::uint32_t>(impl_->human_owners.size());
            impl_->human_owners.assign(count, 1);
            for (std::uint32_t seat = first; seat < count; ++seat)
                impl_->human_owners[seat] = seat < first + second ? 2 : 3;
        }
        return;
    }
    if (gadget == "EAWR_LAN_DIRECT_SEATS") {
        if (auto* combo = EAWR_CONTROL(EawrUiCombo, gadget))
            impl_->direct_seats = static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index)));
        return;
    }
    if (gadget == "IDC_LAN_COLOR_SELECT") {
        if (auto* combo = EAWR_CONTROL(EawrUiCombo, "IDC_LAN_COLOR_SELECT")) {
            commit_colour(static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index))));
        }
        return;
    }
    if (gadget == "IDC_GAME_TYPE_LIST") {
        if (index >= 0 && static_cast<std::size_t>(index) < std::size(game_types)) {
            impl_->host_options.game_type = game_types[index].name;
            if (auto* description = EAWR_CONTROL(EawrUiLabel, "IDC_GAME_TYPE_DESCRIPTION_TEXT")) {
                description->set_text(resolved(resources_->text, game_types[index].description));
            }
        }
        return;
    }
    if (page_ != Page::staging) return;
    if (!impl_->host && match_->lobby_state()) {
        const auto room = *match_->lobby_state();
        auto local = std::find_if(room.snapshot.seats.begin(), room.snapshot.seats.end(), [&](const auto& seat) { return seat.peer == match_->view().seat; });
        if (local == room.snapshot.seats.end()) return;
        const auto suffix = std::to_string(local->selection.slot - 1);
        auto choice = local->selection;
        auto* combo = EAWR_CONTROL(EawrUiCombo, gadget); if (!combo) return;
        if (gadget == "IDC_PLAYER_FACTION_" + suffix) choice.faction = utf8(combo->get_item_text(static_cast<int>(index)));
        else if (gadget == "IDC_PLAYER_COLOR_" + suffix) choice.colour = static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index)));
        else if (gadget == "IDC_PLAYER_TEAM_" + suffix) choice.team = static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index)));
        else if (gadget == "IDC_PLAYER_NAME_" + suffix) choice.slot = static_cast<std::uint32_t>(combo->get_item_id(static_cast<int>(index)));
        else return;
        (void)match_->lobby_select(std::move(choice)); return;
    }
    if (!impl_->host) return;
    Impl& impl = *impl_;
    Resources& resources = *resources_;
    if (gadget == "IDC_STAGING_MAP_LIST") {
        if (index >= 0 && static_cast<std::size_t>(index) < impl.listed.size()) {
            impl.map_index = static_cast<int>(impl.listed[static_cast<std::size_t>(index)]);
            skirmish::repair_setup_selection(impl.selection, resources.maps[static_cast<std::size_t>(impl.map_index)]);
            prepare_host_rows(resources.maps[static_cast<std::size_t>(impl.map_index)]);
            impl.rows_signature.clear();
            fill_host_seats();
            publish_staging_selection();
            announce();
        }
        return;
    }
    if (impl.map_index < 0) return;
    const skirmish::SetupMap& map = resources.maps[static_cast<std::size_t>(impl.map_index)];
    // The row gadgets are IDC_PLAYER_<FIELD>_<row>.
    const auto split = gadget.rfind('_');
    if (split == std::string::npos) return;
    const std::string field = gadget.substr(0, split);
    std::uint32_t row{};
    const auto parsed = std::from_chars(gadget.data() + split + 1, gadget.data() + gadget.size(), row);
    if (parsed.ec != std::errc{}) return;
    const std::uint32_t slot = row + 1;
    auto* combo = EAWR_CONTROL(EawrUiCombo, gadget);
    if (combo == nullptr) return;
    const int id_value = combo->get_item_id(static_cast<int>(index));
    auto record = std::find_if(impl.selection.slots.begin(), impl.selection.slots.end(),
                               [slot](const skirmish::LobbySlot& value) { return value.slot == slot; });
    if (field == "IDC_PLAYER_NAME") {
        if (id_value >= 10000 && match_->lobby_state()) { (void)match_->lobby_kick(static_cast<net::PeerIndex>(id_value - 10000)); return; }
        if (slot < 3) return;
        if (id_value == 0) {
            if (record != impl.selection.slots.end()) impl.selection.slots.erase(record);
        } else if (id_value == 3 && record == impl.selection.slots.end()) {
            if (auto added = append_lan_ai_row(impl.selection, map, slot, resources.palette.size()); !added) {
                note_chat(model::ChatKind::error, core::format_diagnostic(net::lobby_diagnostic(net::LobbyReason::invalid_choice, added.error().message)));
                impl.rows_signature.clear();
                return;
            }
        }
    } else if (record != impl.selection.slots.end()) {
        if (field == "IDC_PLAYER_FACTION") {
            record->faction = utf8(combo->get_item_text(static_cast<int>(index)));
        } else if (field == "IDC_PLAYER_TEAM") {
            record->team = static_cast<std::uint32_t>(id_value);
            // Keep a faction the new team can start with.
            const auto side = std::find_if(map.teams.begin(), map.teams.end(),
                                           [&](const skirmish::SetupTeam& t) { return t.team == record->team; });
            if (side != map.teams.end() && !side->factions.empty()
                && std::find(side->factions.begin(), side->factions.end(), record->faction) == side->factions.end()) {
                record->faction = side->factions.front();
            }
        } else if (field == "IDC_PLAYER_COLOR") {
            static_cast<void>(skirmish::select_setup_colour(impl.selection, slot, static_cast<std::uint32_t>(id_value),
                                                            resources.palette.size()));
        }
    }
    publish_staging_selection();
    impl.rows_signature.clear();
}

void EawrLanLobby::refresh_lobby() {
    if (page_ != Page::lobby || !state_) return;
    Impl& impl = *impl_;
    Resources& resources = *resources_;
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    if (online) {
        const bool busy = impl.join_selection.attempt().has_value() || impl.prompt.has_value();
        if (auto* join = EAWR_CONTROL(EawrUiButton, "IDC_LAN_JOIN_GAME_BUTTON")) join->set_disabled(busy || !impl.online_rooms);
        if (auto* code = EAWR_CONTROL(EawrUiButton, code_button)) code->set_disabled(busy);
        if (auto* cancel = EAWR_CONTROL(EawrUiButton, cancel_join_button)) cancel->set_disabled(!impl.join_selection.attempt());
    }
    // The name edit is read as retail reads it: a changed text is the player's name.
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) {
        if (!edit->has_focus()) {
            const std::string typed = utf8(edit->get_text());
            if (typed != impl.name) edit->set_text(text(impl.name));
        }
    }
    impl.games.clear();
    impl.players.clear();
    if (impl.source) {
        impl.games = impl.source->games();
        impl.players = impl.source->players();
    }
    // Online ping stays unknown ("?"): the service round trip is not the game's.
    if (match_ && !online) apply_lan_lobby_ping(impl.games, match_->view());
    impl.shown = model::arrange_games(impl.games, impl.filter, impl.sort);
    if (online) {
        // One page of the filtered rooms is drawn; `shown` is that page, so a row index maps straight to it.
        impl.online_rooms_total = impl.shown.size();
        const model::OnlinePageView view = model::online_page(impl.shown.size(), impl.online_page);
        impl.online_page = view.page;
        impl.shown = std::vector<model::LobbyGame>(impl.shown.begin() + static_cast<std::ptrdiff_t>(view.first),
                                                   impl.shown.begin() + static_cast<std::ptrdiff_t>(view.first + view.count));
        impl.list_notice = impl.online_rooms
            ? std::string(model::online_list_notice(impl.online_rooms->last_error().has_value(), impl.online_rooms_total))
            : std::string(model::online_join_notice(model::OnlineJoinStatus::not_configured));
        if (!impl.online_rooms && options_.online_settings) {
            if (const auto error = model::validate_online_settings(*options_.online_settings, true)) impl.list_notice = error->message;
        } else if (impl.online_rooms) {
            if (const auto error = impl.online_rooms->last_error(); error && (error->code == "EAWR-NET-1519" || error->code == "EAWR-NET-1528"))
                impl.list_notice = "Could not reach or trust the online service. Check Network Options.";
        }
        impl.chat_open = impl.online_rooms && !impl.players.empty();
        impl.status = impl.list_notice;
        if (auto* notice = EAWR_CONTROL(EawrUiLabel, online_notice_label)) {
            notice->set_text(text(impl.list_notice));
            notice->set_visible(!impl.list_notice.empty());
        }
        if (auto* pages = EAWR_CONTROL(EawrUiLabel, page_label)) {
            pages->set_text(text("Page " + std::to_string(view.page + 1) + "/" + std::to_string(view.pages) + " - "
                                 + std::to_string(impl.online_rooms_total) + (impl.online_rooms_total == 1 ? " room" : " rooms")));
        }
        if (auto* prev = EAWR_CONTROL(EawrUiButton, prev_button)) prev->set_disabled(view.page == 0);
        if (auto* next = EAWR_CONTROL(EawrUiButton, next_button)) next->set_disabled(view.page + 1 >= view.pages);
        if (auto* refresh = EAWR_CONTROL(EawrUiButton, refresh_button)) refresh->set_disabled(!impl.online_rooms);
    }
    int selected = -1;
    std::vector<EawrLobbyGameList::Row> rows;
    for (std::size_t index = 0; index < impl.shown.size(); ++index) {
        const model::LobbyGame& game = impl.shown[index];
        if (game.name + "|" + game.address == impl.selected_key) selected = static_cast<int>(index);
        const model::LobbyRowText row = online ? model::online_row_text(game) : model::lobby_row_text(game);
        EawrLobbyGameList::Row drawn;
        drawn.locked = online && game.passworded;
        drawn.cells = {text(row.name), online ? text(row.type) : resolved(resources.text, row.type), text(row.players),
                       text(row.map), text(row.ping)};
        // Retail colours: password games yellow, another version dark orange, the rest cyan.
        drawn.colour = game.passworded ? Color(1.0F, 0.9F, 0.2F) : !game.version_matches ? Color(0.75F, 0.4F, 0.05F)
                                                                                         : Color(0.35F, 0.95F, 1.0F);
        rows.push_back(std::move(drawn));
    }
    if (state_->game_list != nullptr) state_->game_list->set_rows(std::move(rows), selected);

    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_PLAYER_LIST")) {
        list->clear();
        const auto add_player = [&](const std::string& name, const std::uint32_t colour) {
            const int index = list->add_item(text(name));
            if (colour < resources.palette.size()) {
                const auto& rgb = resources.palette[colour].rgb;
                list->set_item_custom_fg_color(index, Color(rgb[0] / 255.0F, rgb[1] / 255.0F, rgb[2] / 255.0F));
            }
        };
        add_player(impl.name, impl.colour);
        for (const model::LobbyPlayer& player : impl.players) {
            // Online lists the admitted members of the room the player is in, minus the player.
            if (online ? !same_ci(player.name, impl.name) : player.place == model::LobbyPlace::lobby)
                add_player(player.name, player.colour);
        }
    }
    // The chat list: only appended lines are added, so a read position is kept.
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_LAN_CHAT_LIST")) draw_chat(*list, impl.lobby_chat);
    impl.last_list_ms = now_ms();
    impl.dirty = false;
}

void EawrLanLobby::join_selected() {
    if (impl_->join_selection.source() != model::LobbySourceKind::lan) return;
    Impl& impl = *impl_;
    const model::LobbyGame* selected = nullptr;
    for (const model::LobbyGame& game : impl.shown) {
        if (game.name + "|" + game.address == impl.selected_key) selected = &game;
    }
    const model::JoinVerdict verdict = model::check_join(selected, impl.name);
    if (!verdict.ok()) {
        note_chat(model::ChatKind::error, verdict.key.empty() ? verdict.literal : verdict.key);
        refresh_lobby();
        return;
    }
    impl.direct_seats = selected->max_players;
    if (impl.direct_seats < 2 || impl.direct_seats > net::max_match_seats) {
        show_failure("Choose a hosted game with two to nine human seats."); return;
    }
    const auto attempt = impl.join_selection.begin(model::LanDiscoveryTarget{*selected});
    if (!attempt) return;
    match_->configure_seats(std::vector<sim::tactical::PlayerId>(impl.direct_seats, 1));
    configure_staging();
    match_->join(selected->address);
    open_staging(false, selected->name, selected->address);
}

void EawrLanLobby::join_address(const std::string& address, const std::string& game) {
    if (impl_->join_selection.source() != model::LobbySourceKind::lan) return;
    if (impl_->direct_seats < 2 || impl_->direct_seats > net::max_match_seats) {
        show_failure("Choose a hosted game with two to nine human seats."); return;
    }
    if (!impl_->join_selection.begin(model::LanAddressTarget{address})) return;
    match_->configure_seats(std::vector<sim::tactical::PlayerId>(impl_->direct_seats, 1));
    configure_staging();
    match_->join(address);
    open_staging(false, game, address);
}

void EawrLanLobby::cancel_join() {
    if (impl_->online_rooms && impl_->join_selection.attempt()
        && impl_->join_selection.attempt()->source == model::LobbySourceKind::online) {
        impl_->online_rooms->cancel(*impl_->join_selection.attempt());
    }
    impl_->join_selection.cancel();
}

void EawrLanLobby::refresh_online_rooms() {
    if (impl_->online_rooms) impl_->online_rooms->refresh();
    impl_->dirty = true;
}

void EawrLanLobby::select_source(const model::LobbySourceKind source) {
    if (page_ != Page::lobby || source == impl_->join_selection.source()) return;
    cancel_join();
    impl_->join_selection.select_source(source);
    if (source == model::LobbySourceKind::online) {
        if (!options_.online_rooms && !impl_->online_rooms && options_.online_settings) {
            auto configured = online_match_options(*options_.online_settings);
            if (configured) impl_->online_rooms = make_live_online_source(configured.value());
        }
        static_cast<void>(configure_online_match());
    } else {
        if (match_ && match_ != lan_match_) match_->cancel();
        match_ = lan_match_;
    }
    // No old source rows, queued chat, selection or invitation survives a switch.
    if (impl_->source) static_cast<void>(impl_->source->take_chat());
    impl_->source = source == model::LobbySourceKind::lan ? options_.source
                                                          : std::shared_ptr<model::LobbyGameSource>(impl_->online_rooms);
    impl_->online_page = 0;
    impl_->prompt.reset();
    impl_->join_game.clear();
    impl_->join_room.clear();
    if (impl_->source) {
        // Drain arrivals accumulated while this source was inactive before drawing its new page.
        static_cast<void>(impl_->source->pump());
        static_cast<void>(impl_->source->take_chat());
    }
    impl_->games.clear();
    impl_->shown.clear();
    impl_->players.clear();
    impl_->selected_key.clear();
    impl_->entered_code.clear();
    impl_->lobby_chat.clear();
    impl_->last_message.clear();
    impl_->status.clear();
    open_lobby();
    if (source == model::LobbySourceKind::online && !impl_->online_rooms) {
        if (const auto error = model::validate_online_settings(*options_.online_settings, true)) {
            note_chat(model::ChatKind::error, error->message); impl_->list_notice = error->message; refresh_lobby();
        }
    }
}

bool EawrLanLobby::configure_online_match() {
    if (options_.match) { match_ = options_.match; return true; }
    if (online_match_) { match_ = online_match_; return true; }
    auto configured = online_match_options(*options_.online_settings);
    if (!configured) return false;
    if (match_ && match_ != lan_match_) match_->cancel();
    match_ = make_online_match(std::move(configured).value(), options_.diagnostic_root, options_.input_delay);
    match_->configure_factions(scene::faction_order(*resources_->content->catalog));
    online_match_ = match_;
    return true;
}

// The room code field: a four-character code or a full invitation goes straight to the two-player
// online bridge, the same way a picked room does.
void EawrLanLobby::join_code(const std::string& code) {
    if (page_ != Page::lobby || impl_->join_selection.source() != model::LobbySourceKind::online
        || impl_->join_selection.attempt() || impl_->prompt) return;
    impl_->entered_code = code;
    std::optional<std::string> canonical = model::normalize_short_code(code);
    if (!canonical) canonical = model::normalize_invitation_code(code);
    if (!canonical) {
        note_chat(model::ChatKind::error, "Enter four letters or digits.");
    } else if (!impl_->online_rooms || !configure_online_match()) {
        note_chat(model::ChatKind::error, std::string(model::online_join_notice(model::OnlineJoinStatus::not_configured)));
    } else if (impl_->join_selection.begin(model::OnlineInvitationTarget{"online", *canonical})) {
        note_chat(model::ChatKind::notice, "Finding game…");
        impl_->direct_seats = 2;
        match_->configure_seats(std::vector<sim::tactical::PlayerId>(impl_->direct_seats, 1));
        configure_staging();
        match_->join(*canonical);
        open_staging(false, {}, {});
        return;
    } else {
        note_chat(model::ChatKind::error, std::string(model::online_join_notice(model::OnlineJoinStatus::not_configured)));
    }
    refresh_lobby();
}

// A picked online room: a passworded one asks for its password first.
void EawrLanLobby::join_online_selected() {
    Impl& impl = *impl_;
    if (impl.join_selection.source() != model::LobbySourceKind::online || page_ != Page::lobby || impl.prompt
        || impl.join_selection.attempt()) return;
    if (!impl.online_rooms) {
        note_chat(model::ChatKind::error, std::string(model::online_join_notice(model::OnlineJoinStatus::not_configured)));
        refresh_lobby();
        return;
    }
    const model::LobbyGame* selected = nullptr;
    for (const model::LobbyGame& game : impl.shown) {
        if (game.name + "|" + game.address == impl.selected_key) selected = &game;
    }
    const model::JoinVerdict verdict = model::check_join(selected, impl.name, true);
    if (!verdict.ok()) {
        note_chat(model::ChatKind::error, verdict.key.empty() ? verdict.literal : verdict.key);
        refresh_lobby();
        return;
    }
    if (selected->passworded) open_password_prompt(*selected);
    else begin_online_join(selected->name, selected->address, {});
}

void EawrLanLobby::begin_online_join(const std::string& game, const std::string& room, const std::string& password) {
    Impl& impl = *impl_;
    const auto attempt = impl.join_selection.begin(model::OnlineInvitationTarget{"online", room});
    if (!attempt) {
        note_chat(model::ChatKind::error, std::string(model::online_failure_text(model::OnlineFailure::unavailable)));
        refresh_lobby();
        return;
    }
    impl.join_game = game;
    impl.join_room = room;
    impl.last_online_notice.clear();
    impl.online_rooms->begin_join(*attempt, room, password, impl.name);
    note_chat(model::ChatKind::notice, "Finding game…");
    refresh_lobby();
}

void EawrLanLobby::open_password_prompt(const model::LobbyGame& game) {
    Impl& impl = *impl_;
    if (!state_ || page_ != Page::lobby || state_->dialog->frame == nullptr) return;
    model::OnlinePasswordPrompt prompt;
    prompt.game = game.name;
    prompt.room = game.address;
    impl.prompt = prompt;
    PageState& state = *state_;
    Control* frame = state.dialog->frame;
    const Vector2 area = frame->get_size();
    const Vector2 size(std::min(380.0F, area.x - 40.0F), 150.0F);
    auto* panel = memnew(ColorRect);
    panel->set_name(prompt_panel);
    panel->set_color(Color(0.02F, 0.05F, 0.1F, 0.96F));
    panel->set_position(Vector2((area.x - size.x) / 2.0F, (area.y - size.y) / 2.0F));
    panel->set_size(size);
    frame->add_child(panel);
    state.controls[prompt_panel] = panel;
    const float inner = size.x - 24.0F;
    auto* title = memnew(EawrUiLabel);
    title->set_name(prompt_title_label);
    title->set_text(text("Password for " + game.name));
    title->set_position(Vector2(12.0F, 10.0F));
    title->set_size(Vector2(inner, 24.0F));
    title->set_wrap(false);
    title->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    panel->add_child(title);
    auto* edit = memnew(EawrUiEdit);
    edit->set_name(prompt_edit);
    edit->set_secret(true);
    edit->set_max_length(static_cast<int>(model::lan_password_max));
    edit->set_position(Vector2(12.0F, 40.0F));
    edit->set_size(Vector2(inner, 30.0F));
    panel->add_child(edit);
    edit->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String(prompt_edit)));
    state.controls[prompt_edit] = edit;
    auto* error = memnew(EawrUiLabel);
    error->set_name(prompt_error_label);
    error->set_position(Vector2(12.0F, 74.0F));
    error->set_size(Vector2(inner, 28.0F));
    error->set_wrap(true);
    error->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    panel->add_child(error);
    state.controls[prompt_error_label] = error;
    for (const auto& [id, caption, x] : std::array<std::tuple<const char*, const char*, float>, 2>{{
             {prompt_ok_button, "OK", 12.0F}, {prompt_cancel_button, "Cancel", 12.0F + inner / 2.0F + 4.0F}}}) {
        auto* button = memnew(EawrUiButton);
        button->set_name(id);
        button->set_text(caption);
        button->set_position(Vector2(x, size.y - 40.0F));
        button->set_size(Vector2(inner / 2.0F - 4.0F, 30.0F));
        panel->add_child(button);
        button->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(String(id)));
        state.controls[id] = button;
    }
    edit->grab_focus();
    refresh_lobby();
}

void EawrLanLobby::close_password_prompt() {
    Impl& impl = *impl_;
    impl.prompt.reset();
    if (!state_) return;
    if (auto* panel = EAWR_CONTROL(Control, prompt_panel)) {
        panel->get_parent()->remove_child(panel);
        panel->queue_free();
    }
    for (const char* id : {prompt_panel, prompt_edit, prompt_title_label, prompt_error_label, prompt_ok_button,
                           prompt_cancel_button}) {
        state_->controls.erase(id);
    }
    if (auto* chat = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) chat->grab_focus();
}

void EawrLanLobby::submit_password(const std::string& typed) {
    Impl& impl = *impl_;
    if (!impl.prompt || !impl.online_rooms) return;
    const std::string password = model::clamp_room_password(typed);
    if (!model::can_submit_password(*impl.prompt, password)) return;
    const std::string game = impl.prompt->game, room = impl.prompt->room;
    const auto attempt = impl.join_selection.begin(model::OnlineInvitationTarget{"online", room});
    if (!attempt) return;
    impl.prompt->waiting = true;
    impl.prompt->error.clear();
    if (auto* error = EAWR_CONTROL(EawrUiLabel, prompt_error_label)) error->set_text("");
    impl.join_game = game;
    impl.join_room = room;
    impl.last_online_notice.clear();
    // The password goes to the source and nowhere else: not to the chat, the report or a log.
    impl.online_rooms->begin_join(*attempt, room, password, impl.name);
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, prompt_edit)) edit->set_text("");
    note_chat(model::ChatKind::notice, "Finding game…");
}

// A refused service call: the curated text, never the diagnostic's own words.
void EawrLanLobby::online_failure(const core::Diagnostic& diagnostic) {
    const std::string message(model::online_failure_text(model::classify_online_failure(diagnostic.code)));
    if (message == impl_->last_online_notice) return;
    impl_->last_online_notice = message;
    note_chat(model::ChatKind::error, message);
}

// One frame of the online half: the join answers, the service notices and the hosting state.
void EawrLanLobby::pump_online() {
    Impl& impl = *impl_;
    if (!impl.online_rooms) return;
    for (const net::OnlineLobbyJoinCompletion& result : impl.online_rooms->take_join_results()) {
        if (!impl.join_selection.complete(result.attempt)) continue;
        if (result.admission) {
            // Admitted: the room's code goes into the two-player online bridge, then the shared staging.
            const std::string game = impl.join_game, room = impl.join_room;
            close_password_prompt();
            if (!configure_online_match() || (!options_.match && result.admission->bridge_invitation.empty())) {
                impl.online_rooms->cancel(result.attempt);
                core::Diagnostic error; error.code = "EAWR-NET-1752"; error.message = "The game is no longer available.";
                online_failure(error);
                continue;
            }
            impl.direct_seats = 2;
            match_->configure_seats(std::vector<sim::tactical::PlayerId>(impl.direct_seats, 1));
            configure_staging();
            match_->join(options_.match ? room : result.admission->bridge_invitation);
            open_staging(false, game, {});
            return;
        }
        const model::OnlineFailure failure = result.refusal ? model::classify_online_failure(result.refusal->code)
                                                            : model::OnlineFailure::other;
        const std::string message(model::online_failure_text(failure));
        if (impl.prompt && model::online_failure_keeps_prompt(failure)) {
            impl.prompt->waiting = false;
            impl.prompt->error = message;
            if (auto* error = EAWR_CONTROL(EawrUiLabel, prompt_error_label)) error->set_text(text(message));
            if (auto* edit = EAWR_CONTROL(EawrUiEdit, prompt_edit)) edit->grab_focus();
        } else if (impl.prompt) {
            close_password_prompt();
        }
        // The room and the name stay as they were; only the attempt is over.
        note_chat(model::ChatKind::error, message);
    }
    for (const core::Diagnostic& notice : impl.online_rooms->take_notices()) {
        // A join refusal was shown above, the room list failure shows in the list's own line; the
        // rest (publishing, room membership) shows here.
        const model::OnlineFailure failure = model::classify_online_failure(notice.code);
        if (failure == model::OnlineFailure::wrong_password || failure == model::OnlineFailure::cooldown
            || failure == model::OnlineFailure::full || failure == model::OnlineFailure::unavailable
            || failure == model::OnlineFailure::duplicate_name) continue;
        // On the lobby page the room list has its own line; chat refusals and removal still show.
        if (page_ == Page::lobby && failure != model::OnlineFailure::busy && failure != model::OnlineFailure::removed) continue;
        online_failure(notice);
    }
    const net::OnlineHostingState state = impl.online_rooms->hosting_state();
    const model::OnlineHostingPhase phase = state == net::OnlineHostingState::publishing ? model::OnlineHostingPhase::publishing
        : state == net::OnlineHostingState::hosted ? model::OnlineHostingPhase::hosted
        : state == net::OnlineHostingState::retrying ? model::OnlineHostingPhase::retrying : model::OnlineHostingPhase::idle;
    if (phase != impl.hosting) {
        impl.hosting = phase;
        if (page_ == Page::staging && impl.host && impl.join_selection.source() == model::LobbySourceKind::online) {
            const std::string_view line = model::online_hosting_notice(phase);
            if (!line.empty()) {
                note_chat(phase == model::OnlineHostingPhase::retrying ? model::ChatKind::error : model::ChatKind::notice,
                          std::string(line));
            }
        }
    }
}

// ---- Host options -----------------------------------------------------------------------------

void EawrLanLobby::open_host_options() {
    if (resources_->dialogs->script.find(host_dialog) == nullptr) return;
    page_ = Page::host_options;
    build_page(host_dialog);
    PageState& state = *state_;
    Resources& resources = *resources_;
    state.loading = true;
    if (auto* name = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_NAME_EDIT")) {
        name->set_text(text(impl_->host_options.game_name));
        name->set_max_length(static_cast<int>(model::lan_game_name_max));
        name->connect("text_submitted",
                      callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String("IDC_INTERNET_GAME_NAME_EDIT")));
        name->grab_focus();
    }
    if (auto* password = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_PASSWORD_EDIT")) {
        // Decision D-b: password games are internet-lobby features, disabled with a reason.
        password->set_secret(true);
        password->set_max_length(static_cast<int>(model::lan_password_max));
        // Passwords belong to online rooms: empty makes a public room, a password a private one.
        password->set_editable(impl_->join_selection.source() == model::LobbySourceKind::online && impl_->online_rooms != nullptr);
        password->set_tooltip_text(password->is_editable()
            ? "Leave empty for a public room. A password makes it a private room."
            : "Passwords are not supported in this build yet.");
    }
    // Match the clan control in the free row between it and the game-type list.
    // Derive the row spacing from the retail name/password edits so it scales with the dialog.
    if (auto* seats = EAWR_CONTROL(EawrUiCombo, "IDC_INTERNET_HOST_CLAN_COMBO")) {
        const auto* name = EAWR_CONTROL(Control, "IDC_INTERNET_GAME_NAME_EDIT");
        const auto* password = EAWR_CONTROL(Control, "IDC_INTERNET_GAME_PASSWORD_EDIT");
        const auto* clan_label = EAWR_CONTROL(EawrUiLabel, "IDC_TEXT_HOST_CLAN");
        if (name && password && clan_label) {
            const Vector2 offset(0, password->get_position().y - name->get_position().y);
            seats->set_fit_to_longest_item(false);
            seats->set_clip_text(true);
            seats->set_retail_list(true);
            auto* teams = memnew(EawrUiCombo);
            teams->set_name("EAWR_LAN_HOST_TEAMS");
            teams->set_theme_type_variation(seats->get_theme_type_variation());
            teams->set_fit_to_longest_item(false);
            teams->set_clip_text(true);
            teams->set_retail_list(true);
            state.dialog->frame->add_child(teams);
            teams->set_position(seats->get_position() + offset);
            teams->set_size(seats->get_size());
            state.controls["EAWR_LAN_HOST_TEAMS"] = teams;
            teams->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(String("EAWR_LAN_HOST_TEAMS")));
            auto* label = memnew(EawrUiLabel);
            label->set_text("Teams");
            label->set_role("R_Text");
            label->set_theme_type_variation(clan_label->get_theme_type_variation());
            label->set_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
            state.dialog->frame->add_child(label);
            label->set_position(clan_label->get_position() + offset);
            label->set_size(clan_label->get_size());
        }
    }
    fill_host_seats();
    if (auto* seats = EAWR_CONTROL(EawrUiCombo, "IDC_INTERNET_HOST_CLAN_COMBO"))
        seats->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(String("IDC_INTERNET_HOST_CLAN_COMBO")));
    if (auto* label = EAWR_CONTROL(EawrUiLabel, "IDC_TEXT_HOST_CLAN")) label->set_text("Human players");
    // Human players use the clan row; the LAN password row remains available for mapping.
    if (auto* password = EAWR_CONTROL(Control, "IDC_INTERNET_GAME_PASSWORD_EDIT");
        password != nullptr && impl_->join_selection.source() == model::LobbySourceKind::lan) {
        password->set_visible(false);
        auto position = password->get_position(); auto size = password->get_size();
        const auto right = position.x + size.x;
        for (const BuiltGadget& gadget : state.dialog->gadgets) {
            if (gadget.kind != "label" || gadget.control == nullptr) continue;
            const auto label_position = gadget.control->get_position();
            const auto label_size = gadget.control->get_size();
            if (label_position.x < position.x && label_position.y < position.y + size.y
                && label_position.y + label_size.y > position.y) {
                position.x = std::min(position.x, label_position.x);
                gadget.control->set_visible(false);
            }
        }
        size.x = right - position.x;
        state.port_mapping = memnew(EawrUiCheck);
        state.port_mapping->set_text("Map router TCP port for Internet play");
        state.port_mapping->set_tooltip_text("Ask your router for a temporary port mapping. This cannot bypass upstream NAT or CGNAT.");
        state.dialog->frame->add_child(state.port_mapping);
        state.port_mapping->set_position(position); state.port_mapping->set_size(size);
    }
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_GAME_TYPE_LIST")) {
        int chosen = 0;
        for (std::size_t index = 0; index < std::size(game_types); ++index) {
            const int item = list->add_item(resolved(resources.text, game_types[index].name));
            if (!game_types[index].available) {
                list->set_item_disabled(item, true);
                list->set_item_tooltip(item, "This game type needs the land game or the campaign, which are not built yet.");
            } else if (impl_->host_options.game_type.empty() || impl_->host_options.game_type == game_types[index].name) {
                chosen = item;
            }
        }
        list->select(chosen);
        impl_->host_options.game_type = game_types[chosen].name;
        list->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(String("IDC_GAME_TYPE_LIST")));
        if (auto* description = EAWR_CONTROL(EawrUiLabel, "IDC_GAME_TYPE_DESCRIPTION_TEXT")) {
            description->set_text(resolved(resources.text, game_types[chosen].description));
        }
    }
    for (const BuiltGadget& gadget : state.dialog->gadgets) {
        if (gadget.kind != "button" || gadget.control == nullptr) continue;
        gadget.control->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(text(gadget.id)));
    }
    state.loading = false;
}

void EawrLanLobby::fill_host_seats(const std::uint32_t requested_count) {
    auto* seats = EAWR_CONTROL(EawrUiCombo, "IDC_INTERNET_HOST_CLAN_COMBO");
    auto* teams = EAWR_CONTROL(EawrUiCombo, "EAWR_LAN_HOST_TEAMS");
    if (!seats || !teams) return;
    const bool was_loading = state_->loading;
    state_->loading = true;
    seats->clear();
    teams->clear();
    const skirmish::SetupMap* map = nullptr;
    if (impl_->map_index >= 0 && static_cast<std::size_t>(impl_->map_index) < resources_->maps.size())
        map = &resources_->maps[static_cast<std::size_t>(impl_->map_index)];
    if (!map) {
        const auto preferred = std::find_if(resources_->maps.begin(), resources_->maps.end(), [&](const auto& candidate) {
            return candidate.path == skirmish::m2_fixture().map && candidate.metadata.custom == impl_->custom_maps;
        });
        if (preferred != resources_->maps.end()) map = &*preferred;
        else for (const auto& candidate : resources_->maps) if (candidate.metadata.custom == impl_->custom_maps) { map = &candidate; break; }
    }
    const auto owner_rows = map ? std::min(skirmish::setup_row_count(*map), static_cast<std::uint32_t>(sim::max_match_human_sides)) : 0U;
    // Map capacity bounds owner rows, not the humans sharing each row.
    const int seat_limit = impl_->join_selection.source() == model::LobbySourceKind::online
        ? 2 : static_cast<int>(sim::max_match_seats);
    const auto count = std::clamp(requested_count == 0 ? static_cast<std::uint32_t>(impl_->human_owners.size())
        : requested_count, 2U, static_cast<std::uint32_t>(seat_limit));
    if (owner_rows > 0) for (int humans = 2; humans <= seat_limit; ++humans)
        seats->add_item(text(std::to_string(humans)), humans);
    model::HumanTeamSplit previous{};
    for (const auto owner : impl_->human_owners) if (owner >= 1 && owner <= previous.size()) ++previous[owner - 1];
    const auto choices = model::human_team_splits(count, owner_rows);
    const auto selected = model::closest_human_team_split(choices, previous);
    for (const auto& split : choices) {
        teams->add_item(text(model::human_team_split_text(split)), static_cast<int>(split[0] * 10 + split[1]));
        if (split == selected) teams->select(teams->get_item_count() - 1);
    }
    if (!choices.empty()) {
        seats->select(static_cast<int>(count) - 2);
        auto canonical_previous = previous;
        std::sort(canonical_previous.begin(), canonical_previous.end(), std::greater<>());
        // Opening the options or refreshing the map keeps existing owner assignments intact.
        if (count != impl_->human_owners.size() || selected != canonical_previous) {
            impl_->human_owners.assign(count, 1);
            for (std::uint32_t seat = selected[0]; seat < count; ++seat)
                impl_->human_owners[seat] = seat < selected[0] + selected[1] ? 2 : 3;
        }
    }
    seats->set_disabled(choices.empty());
    teams->set_disabled(choices.empty());
    teams->set_tooltip_text("Humans on one side share its units, credits, reinforcement pool and fog.");
    state_->loading = was_loading;
}

void EawrLanLobby::prepare_host_rows(const skirmish::SetupMap& map) {
    const std::set<sim::tactical::PlayerId> owners(impl_->human_owners.begin(), impl_->human_owners.end());
    for (const auto owner : owners) {
        if (owner > skirmish::setup_row_count(map)) {
            note_chat(model::ChatKind::error, core::format_diagnostic(net::lobby_diagnostic(net::LobbyReason::invalid_choice,
                "A planned human owner is outside this map's loaded rows.")));
            return;
        }
        if (std::any_of(impl_->selection.slots.begin(), impl_->selection.slots.end(), [&](const auto& row) { return row.slot == owner; })) continue;
        // Reuse validated data-driven row admission, preferring a new authored team for a new human owner.
        if (auto added = append_lan_ai_row(impl_->selection, map, owner, resources_->palette.size(), true); !added) {
            note_chat(model::ChatKind::error, core::format_diagnostic(net::lobby_diagnostic(net::LobbyReason::invalid_choice, added.error().message)));
            return;
        }
    }
}

void EawrLanLobby::begin_hosting() {
    Impl& impl = *impl_;
    const auto* name = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_NAME_EDIT");
    const std::optional<std::string> cleaned = model::clean_game_name(name != nullptr ? utf8(name->get_text()) : "");
    if (!cleaned) {
        impl.host_password.clear(); impl.host_options.password.clear();
        // Retail puts the focus back on the name edit when it is empty.
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_NAME_EDIT")) edit->grab_focus();
        return;
    }
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    if (online && !impl.online_rooms) {
        impl.host_password.clear(); impl.host_options.password.clear();
        show_failure(std::string(model::online_join_notice(model::OnlineJoinStatus::not_configured)));
        return;
    }
    impl.host_options.game_name = *cleaned;
    // The room password is read here, handed to the source and kept only as long as the room is listed.
    const auto* password = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_PASSWORD_EDIT");
    impl.host_options.password = online && password != nullptr ? model::clamp_room_password(utf8(password->get_text())) : std::string{};
    impl.host_password = impl.host_options.password;
    impl.last_online_notice.clear();
    if (online && !configure_online_match()) {
        impl.host_password.clear(); impl.host_options.password.clear();
        show_failure("Set the online service in Network Options first."); return;
    }
    if (online && !impl.online_rooms->set_host_password(impl.host_password)) {
        impl.host_password.clear(); impl.host_options.password.clear();
        show_failure(std::string(model::online_failure_text(model::OnlineFailure::password_locked)));
        return;
    }
    match_->configure_seats(impl.human_owners);
    match_->set_port_mapping(state_->port_mapping && state_->port_mapping->is_pressed());
    configure_staging();
    if (online) { impl.online_rooms->set_bridge_invitation({}); match_->set_public_invitation(impl.host_password.empty()); }
    match_->host(options_.game_port);
    open_staging(true, *cleaned, {});
}

// ---- Staging ----------------------------------------------------------------------------------

void EawrLanLobby::publish_staging_selection() {
    if (!impl_->host || !match_->lobby_state()) return;
    auto selected = validated_lan_options(impl_->selection, resources_->maps);
    if (!selected) {
        note_chat(model::ChatKind::error, core::format_diagnostic(net::lobby_diagnostic(net::LobbyReason::invalid_choice, selected.error().message)));
        return;
    }
    (void)match_->lobby_game(selected.value());
}

void EawrLanLobby::configure_staging() {
    const auto content = resources_->content;
    match_->configure_factions(scene::faction_order(*content->catalog));
    match_->configure_lobby(impl_->name, impl_->colour, resources_->maps,
        [content](const skirmish::FixtureOptions& options) -> core::Result<sim::tactical::TacticalSetup> {
            auto fixture = skirmish::fixture_from_options(options, *content->filesystem, *content->catalog);
            if (!fixture) return core::Result<sim::tactical::TacticalSetup>::failure(fixture.error());
            auto prepared = prepare_skirmish(fixture.value(), *content->filesystem, *content->catalog);
            if (!prepared) return core::Result<sim::tactical::TacticalSetup>::failure(prepared.error());
            return core::Result<sim::tactical::TacticalSetup>::success(lan_recording_setup(fixture.value(), prepared.value().start));
        });
}

void EawrLanLobby::open_staging(const bool host, const std::string& game, const std::string& address) {
    Impl& impl = *impl_;
    impl.host = host;
    impl.game = game;
    impl.address = address;
    impl.locked = false;
    impl.launch.reset();
    impl.offered.reset();
    impl.last_message.clear();
    impl.staging_chat = model::ChatLog();
    impl.room_chat_sequence = impl.room_revision = 0; impl.room_notice.clear();
    impl.refused = false;
    page_ = Page::staging;
    build_page(staging_dialog);
    PageState& state = *state_;
    Resources& resources = *resources_;
    state.loading = true;
    const BuiltDialog& dialog = *state.dialog;

    std::vector<std::string> ids;
    for (const BuiltGadget& gadget : dialog.gadgets) ids.push_back(gadget.id);
    state.dialog_rows = model::staging_row_count(ids);

    if (auto* chat = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_CHAT_EDIT")) {
        chat->set_max_length(static_cast<int>(model::lan_chat_max));
        chat->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String("IDC_LAN_CHAT_EDIT")));
        chat->grab_focus();
    }
    // The preview image the dialog builder leaves to the screen. The joiner sees none (LLB-08, MNU-28).
    if (const BuiltGadget* image = find_gadget(dialog, "IDC_IMAGE"); image != nullptr && host) {
        state.preview = memnew(TextureRect);
        state.preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        state.preview->set_stretch_mode(TextureRect::STRETCH_SCALE);
        state.preview->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        const Rect2 rect = local_rect(dialog, *image);
        state.preview->set_position(rect.position);
        state.preview->set_size(rect.size);
        dialog.frame->add_child(state.preview);
    }
    // The accepted and status icons are image gadgets too: a small square per row.
    for (std::size_t row = 0; row < state.dialog_rows; ++row) {
        const std::string id = "IDC_ACCEPT_PLAYER_" + std::to_string(row);
        if (const BuiltGadget* gadget = find_gadget(dialog, id)) {
            auto* icon = memnew(ColorRect);
            const Rect2 rect = local_rect(dialog, *gadget);
            icon->set_position(rect.position + Vector2(2, 2));
            icon->set_size(rect.size - Vector2(4, 4));
            icon->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            icon->set_color(Color(0.1F, 0.1F, 0.1F, 0.9F));
            dialog.frame->add_child(icon);
            state.accept_icons[id] = icon;
        }
    }
    // Rows: a combo each for the name, the faction, the colour and the team.
    for (std::size_t row = 0; row < state.dialog_rows; ++row) {
        const std::string suffix = std::to_string(row);
        for (const char* field : {"IDC_PLAYER_NAME_", "IDC_PLAYER_FACTION_", "IDC_PLAYER_COLOR_", "IDC_PLAYER_TEAM_"}) {
            const std::string id = field + suffix;
            if (auto* combo = EAWR_CONTROL(EawrUiCombo, id)) {
                combo->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(text(id)));
            }
        }
    }
    for (const BuiltGadget& gadget : dialog.gadgets) {
        if (gadget.kind != "button" || gadget.control == nullptr) continue;
        gadget.control->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(text(gadget.id)));
    }
    if (auto* load = EAWR_CONTROL(EawrUiButton, "IDC_LOAD_GAME")) {
        load->set_visible(host);
        load->set_text(resolved(resources.text, "TEXT_BUTTON_ACCEPT"));
        load->set_disabled(true);
    }
    if (auto* options = EAWR_CONTROL(EawrUiButton, "IDC_SELECT_OPTIONS")) {
        options->set_disabled(true);
        options->set_tooltip_text("The match options page is opened from the skirmish setup screen for now.");
    }
    if (auto* start = EAWR_CONTROL(EawrUiButton, "IDC_LAN_START_GAME_BUTTON")) {
        start->set_text(resolved(resources.text, host ? "TEXT_BUTTON_START_GAME" : "TEXT_BUTTON_ACCEPT"));
        start->set_disabled(true);
    }
    if (auto* title = EAWR_CONTROL(EawrUiLabel, "IDC_INTERNET_DIALOG_HEADER")) {
        title->set_text(resolved(resources.text, host ? "TEXT_INTERNET_HOST_GAME_TITLE" : "TEXT_INTERNET_JOIN_GAME_TITLE"));
    }
    if (host && impl.join_selection.source() == model::LobbySourceKind::online && impl.host_password.empty()) {
        auto* invitation = memnew(EawrUiLabel); invitation->set_name("EAWR_ONLINE_PUBLIC_INVITATION");
        invitation->set_position(Vector2(dialog.frame->get_size().x * 0.08F, dialog.frame->get_size().y * 0.10F));
        invitation->set_size(Vector2(dialog.frame->get_size().x * 0.84F, 28.0F));
        invitation->set_mouse_filter(Control::MOUSE_FILTER_IGNORE); dialog.frame->add_child(invitation);
        state.controls["EAWR_ONLINE_PUBLIC_INVITATION"] = invitation;
    }
    // The joiner never edits the host's map; the host's list is filled below.
    if (!host) {
        for (const char* hidden : {"IDC_SELECT_OFFICIAL_MAPS", "IDC_SELECT_CUSTOM_MAPS", "IDC_STAGING_MAP_LIST", "IDC_IMAGE", "IDC_SELECT_OPTIONS"}) {
            if (auto* control = EAWR_CONTROL(Control, hidden)) control->set_visible(false);
        }
    } else {
        impl.custom_maps = false;
        impl.selection = skirmish::SetupSelection{};
        impl.selection.match = resources.defaults;
        impl.map_index = -1;
        impl.remote_colour_applied = false;
        // The host's row takes the player's profile colour.
        if (!impl.selection.slots.empty() && impl.colour < resources.palette.size()) {
            static_cast<void>(skirmish::select_setup_colour(impl.selection, 1, impl.colour, resources.palette.size()));
        }
    }
    state.loading = false;
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_STAGING_MAP_LIST"); list != nullptr && host) {
        list->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(String("IDC_STAGING_MAP_LIST")));
    }
    if (host) fill_map_list();
    announce();
    refresh_staging();
}

// The host's map list for the official or the custom maps; keeps the current map when it is listed.
void EawrLanLobby::fill_map_list() {
    Impl& impl = *impl_;
    Resources& resources = *resources_;
    auto* list = EAWR_CONTROL(EawrUiList, "IDC_STAGING_MAP_LIST");
    if (list == nullptr) return;
    const std::string previous = impl.map_index >= 0 ? resources.maps[static_cast<std::size_t>(impl.map_index)].path
                                                     : skirmish::m2_fixture().map;
    state_->loading = true;
    list->clear();
    impl.listed.clear();
    int initial = 0;
    for (std::size_t index = 0; index < resources.maps.size(); ++index) {
        const skirmish::SetupMap& map = resources.maps[index];
        if (map.metadata.custom != impl.custom_maps) continue;
        impl.listed.push_back(index);
        list->add_item(text(map.name + (map.unavailable.empty() ? "" : " (unavailable)")));
        list->set_item_tooltip(list->get_item_count() - 1, text(map.unavailable.empty() ? map.path : map.unavailable));
        if (map.path == previous) initial = list->get_item_count() - 1;
    }
    impl.map_index = -1;
    if (!impl.listed.empty()) {
        list->select(initial);
        impl.map_index = static_cast<int>(impl.listed[static_cast<std::size_t>(initial)]);
        skirmish::repair_setup_selection(impl.selection, resources.maps[static_cast<std::size_t>(impl.map_index)]);
        prepare_host_rows(resources.maps[static_cast<std::size_t>(impl.map_index)]);
    }
    fill_host_seats();
    state_->loading = false;
    impl.rows_signature.clear();
}

void EawrLanLobby::leave_staging() {
    cancel_join();
    impl_->host_password.clear();
    if (match_) { match_->lobby_leave(); match_->cancel(); }
    impl_->launch.reset();
    impl_->offered.reset();
    impl_->locked = false;
    open_lobby();
}

// ---- Frame ------------------------------------------------------------------------------------

void EawrLanLobby::_process(const double) {
    if (!shown_ || !impl_) return;
    Impl& impl = *impl_;
    bool changed = false;
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    if (online) {
        pump_online();
    }
    if (impl.source) {
        changed = impl.source->pump() || changed;
        for (const model::LobbyChatMessage& chat : impl.source->take_chat()) {
            // The online window shows the service's room chat; the LAN one shows lobby chat.
            const bool for_lobby = page_ != Page::staging
                && (chat.scope == model::LobbyChatScope::lobby || (online && chat.scope == model::LobbyChatScope::game));
            const bool for_game =
                chat.scope == model::LobbyChatScope::game && page_ == Page::staging && chat.game == impl.game
                && (online || !match_->lobby_enabled());
            if (!for_lobby && !for_game) continue;
            note_chat(model::ChatKind::normal, chat.name + ": " + chat.text, chat.name);
            play_sound("GUI_Common_Chat_Receive");
            changed = true;
        }
    }
    if (page_ == Page::lobby) {
        // Retail rebuilds the list once a second.
        if (changed || impl.dirty || now_ms() - impl.last_list_ms >= model::lan_game_list_refresh_ms) refresh_lobby();
    } else if (page_ == Page::staging) {
        refresh_staging();
    }
}

void EawrLanLobby::_input(const Ref<InputEvent>& event) {
    if (!shown_ || impl_->box != nullptr) return;
    const auto* key = Object::cast_to<InputEventKey>(event.ptr());
    if (key == nullptr || !key->is_pressed() || key->is_echo() || key->get_keycode() != KEY_ESCAPE) return;
    if (Viewport* viewport = get_viewport()) viewport->set_input_as_handled();
    switch (page_) {
    case Page::lobby: handle_button(impl_->prompt ? prompt_cancel_button : "IDC_LAN_CANCEL_BUTTON"); break;
    case Page::staging: leave_staging(); break;
    case Page::host_options:
    case Page::direct_connect:
    case Page::filter: open_lobby(); break;
    }
}

void EawrLanLobby::show_failure(const std::string& message) {
    Impl& impl = *impl_;
    if (impl.box != nullptr) return;
    model::MessageBoxRequest request;
    request.caption = "TEXT_LAN_LOBBY_TITLE";
    request.lines[0] = message;
    request.buttons.push_back({"TEXT_BUTTON_OK", 1, true});
    Resources& resources = *resources_;
    const MessageBoxContext context{&*resources.dialogs, &*resources.theme_model,
                                    resources.text ? &*resources.text : nullptr, resources.theme, resources.theme_size};
    auto* box = memnew(EawrMessageBox);
    if (!box->open(*this, context, request)) {
        notes_.push_back("message box: " + box->failure());
        memdelete(box);
        return;
    }
    impl.box = box;
    box->connect("result_chosen", callable_mp(this, &EawrLanLobby::on_box_result));
}

void EawrLanLobby::handle_button(const std::string& id) {
    Impl& impl = *impl_;
    if (impl.box != nullptr) return;
    // The password prompt is modal: only its own buttons act while it is up.
    if (impl.prompt && id != prompt_ok_button && id != prompt_cancel_button) return;
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    if (id == prompt_ok_button) {
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, prompt_edit)) submit_password(utf8(edit->get_text()));
    } else if (id == prompt_cancel_button) {
        cancel_join();
        close_password_prompt();
        note_chat(model::ChatKind::notice, "Join cancelled.");
        refresh_lobby();
    } else if (id == code_button) {
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, invitation_edit)) join_code(utf8(edit->get_text()));
    } else if (id == refresh_button) {
        refresh_online_rooms();
    } else if (id == prev_button || id == next_button) {
        if (id == prev_button && impl.online_page > 0) --impl.online_page;
        if (id == next_button) ++impl.online_page; // refresh_lobby clamps it to the last page
        refresh_lobby();
    } else if (id == lan_source_button || id == online_source_button) {
        select_source(id == lan_source_button ? model::LobbySourceKind::lan : model::LobbySourceKind::online);
    } else if (id == cancel_join_button) {
        cancel_join();
        note_chat(model::ChatKind::notice, "Join cancelled.");
        refresh_lobby();
    } else if (id == "IDC_LAN_CANCEL_BUTTON") {
        if (page_ == Page::staging) {
            leave_staging();
            return;
        }
        // Retail shuts the LAN session down and returns to the main menu.
        if (match_) match_->cancel();
        close_screen(true);
    } else if (id == "IDC_LAN_HOST_GAME_BUTTON") {
        if (!online || impl.online_rooms) open_host_options();
    } else if (id == "IDC_LAN_JOIN_GAME_BUTTON") {
        if (online) join_online_selected();
        else join_selected();
    } else if (id == "IDC_SET_FILTER_BUTTON") {
        if (impl.join_selection.source() == model::LobbySourceKind::lan) open_filter();
    } else if (id == direct_button) {
        if (impl.join_selection.source() == model::LobbySourceKind::lan) open_direct_connect();
    } else if (id == "IDC_INTERNET_SERVER_NAME_BUTTON") {
        impl.sort = model::LobbySort::game_name;
        impl.dirty = true;
    } else if (id == "IDC_INTERNET_GAME_TYPE_BUTTON") {
        impl.sort = online ? model::LobbySort::host : model::LobbySort::game_type;
        impl.dirty = true;
    } else if (id == "IDC_INTERNET_PLAYERS_BUTTON") {
        impl.sort = model::LobbySort::players;
        impl.dirty = true;
    } else if (id == "IDC_INTERNET_MAP_NAME_BUTTON") {
        impl.sort = model::LobbySort::map_name;
        impl.dirty = true;
    } else if (id == "IDC_INTERNET_PING_BUTTON") {
        impl.sort = model::LobbySort::ping;
        impl.dirty = true;
    } else if (id == "IDC_CANCEL_BUTTON") {
        // The host options, the filter and the direct-connect pages all return to the lobby.
        open_lobby();
    } else if (id == "IDC_INTERNET_CREATE_GAME_BUTTON") {
        begin_hosting();
    } else if (id == "IDC_ADD_IP_BUTTON") {
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_EDIT_IP")) {
            const std::string typed = utf8(edit->get_text());
            if (!typed.empty() && std::find(impl.direct_addresses.begin(), impl.direct_addresses.end(), typed) == impl.direct_addresses.end()) {
                impl.direct_addresses.push_back(typed);
                if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_IP_LIST")) list->add_item(text(typed));
            }
        }
    } else if (id == "IDC_IP_CONNECT_BUTTON") {
        std::string address;
        if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_IP_LIST")) {
            const PackedInt32Array chosen = list->get_selected_items();
            if (chosen.size() > 0) address = utf8(list->get_item_text(chosen[0]));
        }
        if (address.empty()) {
            if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_EDIT_IP")) address = utf8(edit->get_text());
        }
        if (!address.empty()) join_address(address, "");
    } else if (id == "IDC_LOAD_GAME" && page_ == Page::staging && impl.host && match_->lobby_state()) {
        const auto state = *match_->lobby_state(); (void)match_->lobby_ready(!state.local_ready);
    } else if (id.starts_with("IDC_BUTTON_KICK_PLAYER") && page_ == Page::staging && impl.host) {
        std::uint32_t row{};
        const auto suffix = id.substr(std::string("IDC_BUTTON_KICK_PLAYER").size());
        if (std::from_chars(suffix.data(), suffix.data() + suffix.size(), row).ec == std::errc{}) {
            if (auto room = match_->lobby_state()) for (const auto& seat : room->snapshot.seats)
                if (seat.selection.slot == row + 1 && seat.peer != room->snapshot.host) { (void)match_->lobby_kick(seat.peer); break; }
        }
    } else if (id == "IDC_LAN_START_GAME_BUTTON") {
        // Start (host) or Accept (joiner); the checks run in refresh_staging's enabled state and here.
        if (page_ != Page::staging) return;
        static_cast<void>(run_action(impl.host ? "start" : "accept"));
    } else if (id == "IDC_BUTTON_ACCEPT") {
        // The filter page's Accept: the combos are the player's filter.
        auto value = [&](const char* gadget) -> std::optional<std::int64_t> {
            if (auto* combo = EAWR_CONTROL(EawrUiCombo, gadget)) {
                const int selected = combo->get_selected();
                if (selected >= 0 && combo->get_item_id(selected) != 0) return combo->get_item_id(selected);
            }
            return std::nullopt;
        };
        model::LobbyFilter filter;
        if (const auto type = value("IDC_GAME_TYPE_FILTER_COMBO"); type && *type >= 1 && *type <= 4) {
            filter.game_type = game_types[*type - 1].name;
        }
        if (const auto min = value("IDC_MIN_PLAYERS_FILTER_COMBO")) filter.min_players = static_cast<std::uint32_t>(*min);
        if (const auto max = value("IDC_MAX_PLAYERS_FILTER_COMBO")) filter.max_players = static_cast<std::uint32_t>(*max);
        if (const auto ping = value("IDC_MAX_PING_FILTER_COMBO")) filter.max_ping_ms = static_cast<std::uint32_t>(*ping);
        impl.filter = filter;
        impl.dirty = true;
        open_lobby();
    } else if (id == "IDC_DEFAULTS_BUTTON") {
        // Clear filters: the combos go back to their first entry.
        for (const char* gadget : {"IDC_GAME_TYPE_FILTER_COMBO", "IDC_MIN_PLAYERS_FILTER_COMBO", "IDC_MAX_PLAYERS_FILTER_COMBO",
                                   "IDC_MAX_PING_FILTER_COMBO"}) {
            if (auto* combo = EAWR_CONTROL(EawrUiCombo, gadget)) combo->select(0);
        }
        impl.filter.clear();
    } else if (id == "IDC_SELECT_OFFICIAL_MAPS" || id == "IDC_SELECT_CUSTOM_MAPS") {
        impl.custom_maps = id == "IDC_SELECT_CUSTOM_MAPS";
        fill_map_list();
        publish_staging_selection();
        announce();
    }
}

// ---- The filter and direct connect pages ------------------------------------------------------

void EawrLanLobby::open_filter() {
    if (resources_->dialogs->script.find(filter_dialog) == nullptr) return;
    page_ = Page::filter;
    build_page(filter_dialog);
    PageState& state = *state_;
    Resources& resources = *resources_;
    state.loading = true;
    const model::LobbyFilter& filter = impl_->filter;
    const auto fill = [&](const char* gadget, const std::vector<std::pair<std::string, int>>& items, int selected_id) {
        if (auto* combo = EAWR_CONTROL(EawrUiCombo, gadget)) {
            int selected = 0;
            for (const auto& [label, id] : items) {
                combo->add_item(resolved(resources.text, label), id);
                if (id == selected_id) selected = combo->get_item_count() - 1;
            }
            combo->select(selected);
        }
    };
    std::vector<std::pair<std::string, int>> types{{"TEXT_GAME_TYPE_FILTER_ALL", 0}};
    int type_id = 0;
    for (std::size_t index = 0; index < std::size(game_types); ++index) {
        types.emplace_back(game_types[index].name, static_cast<int>(index) + 1);
        if (filter.game_type && *filter.game_type == game_types[index].name) type_id = static_cast<int>(index) + 1;
    }
    fill("IDC_GAME_TYPE_FILTER_COMBO", types, type_id);
    // 2 to 9 players, as retail lists; the first entry shows no limit.
    std::vector<std::pair<std::string, int>> players{{"TEXT_GAME_TYPE_FILTER_ALL", 0}};
    for (int count = 2; count < 10; ++count) players.emplace_back(std::to_string(count), count);
    fill("IDC_MIN_PLAYERS_FILTER_COMBO", players, filter.min_players ? static_cast<int>(*filter.min_players) : 0);
    fill("IDC_MAX_PLAYERS_FILTER_COMBO", players, filter.max_players ? static_cast<int>(*filter.max_players) : 0);
    std::vector<std::pair<std::string, int>> pings{{"TEXT_ANY_PING_FILTER_TIME", 0}};
    for (int ping = 100; ping <= 1000; ping += 100) pings.emplace_back(std::to_string(ping), ping);
    fill("IDC_MAX_PING_FILTER_COMBO", pings, filter.max_ping_ms ? static_cast<int>(*filter.max_ping_ms) : 0);
    for (const BuiltGadget& gadget : state.dialog->gadgets) {
        if (gadget.kind != "button" || gadget.control == nullptr) continue;
        gadget.control->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(text(gadget.id)));
    }
    state.loading = false;
}

void EawrLanLobby::open_direct_connect() {
    if (resources_->dialogs->script.find(direct_dialog) == nullptr) return;
    page_ = Page::direct_connect;
    build_page(direct_dialog);
    PageState& state = *state_;
    state.loading = true;
    if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_EDIT_IP")) {
        edit->set_placeholder("127.0.0.1:47624");
        edit->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(String("IDC_EDIT_IP")));
        edit->grab_focus();
    }
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_IP_LIST")) {
        for (const std::string& address : impl_->direct_addresses) list->add_item(text(address));
        auto* seats = memnew(EawrUiCombo);
        seats->set_name("EAWR_LAN_DIRECT_SEATS");
        seats->set_position(list->get_position()); seats->set_size(Vector2(list->get_size().x, 26));
        for (int count = 2; count <= static_cast<int>(net::max_match_seats); ++count) seats->add_item(text(std::to_string(count) + " human seats"), count);
        seats->select(static_cast<int>(impl_->direct_seats) - 2);
        seats->set_tooltip_text("Choose the same human seat count as the hosted game.");
        list->set_position(list->get_position() + Vector2(0, 31));
        list->set_size(Vector2(list->get_size().x, std::max(0.0F, list->get_size().y - 31)));
        state.dialog->frame->add_child(seats); state.controls["EAWR_LAN_DIRECT_SEATS"] = seats;
        seats->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(String("EAWR_LAN_DIRECT_SEATS")));
    }
    for (const BuiltGadget& gadget : state.dialog->gadgets) {
        if (gadget.kind != "button" || gadget.control == nullptr) continue;
        gadget.control->connect("pressed", callable_mp(this, &EawrLanLobby::on_pressed).bind(text(gadget.id)));
    }
    state.loading = false;
}

// ---- The staging's frame ----------------------------------------------------------------------

void EawrLanLobby::refresh_staging() {
    if (page_ != Page::staging || !state_) return;
    Impl& impl = *impl_;
    PageState& state = *state_;
    Resources& resources = *resources_;
    const std::size_t dialog_rows = state.dialog_rows;

    // LobbySession owns editable staging; SetupSession owns the frozen final barrier.
    std::optional<skirmish::FixtureOptions> built;
    if (impl.host && impl.map_index >= 0) {
        if (auto options = validated_lan_options(impl.selection, resources.maps)) built = std::move(options).value();
    }
    match_->pump_setup(built ? *built : skirmish::FixtureOptions{});
    const LanView view = match_->view();
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    if (online && impl.host && impl.online_rooms) {
        impl.online_rooms->set_bridge_invitation(match_->bridge_invitation(), match_->bridge_ownership());
        if (auto* invitation = EAWR_CONTROL(EawrUiLabel, "EAWR_ONLINE_PUBLIC_INVITATION")) {
            // Display public invites in their own control, never in chat/status reports.
            invitation->set_text(text(impl.host_password.empty() && view.invitation_code.size() == 4 ? "Room code: " + view.invitation_code : std::string{}));
        }
    }
    if (!impl.host) {
        if (auto offered = match_->offered_setup()) {
            if (!impl.offered || offered->map != impl.offered->map || !same_slots(offered->slots, impl.offered->slots)) {
                impl.offered = offered;
                impl.rows_signature.clear();
            }
        }
    }

    // The status line is one notice per change, like a chat line.
    const auto room = match_->lobby_state();
    std::string status;
    if (view.phase != LanPhase::idle && view.phase != LanPhase::running) status = online ? view.message : lan_message(view);
    if (view.phase == LanPhase::setup && view.read_only && !view.accept_enabled) status = utf8(resolved(resources.text, "TEXT_WAITING"));
    if (lan_reconnect_disabled_notice(view)) {
        if (!status.empty()) status += "\n";
        status += "Reconnect: disabled. " + view.reconnect_unavailable;
    }
    if (room && (room->preparing || room->snapshot.phase == net::LobbyPhase::starting)) status = utf8(resolved(resources.text, "TEXT_WAITING"));
    if (!status.empty() && status != impl.status) {
        impl.status = status;
        note_chat(model::ChatKind::notice, status);
    }

    std::vector<model::StagingRow> rows(dialog_rows);
    const skirmish::SetupMap* map = nullptr;
    std::size_t visible = 0;
    const bool connected = view.connected && view.phase != LanPhase::ended;
    if (room) {
        impl.linked = room->snapshot.seats.size() > 1;
        auto authoritative = decode_lan_lobby_game(room->snapshot);
        if (authoritative) {
            impl.offered = authoritative.value();
            if (impl.host) { impl.selection.map = *authoritative.value().map; impl.selection.slots = *authoritative.value().slots; impl.selection.match = authoritative.value().match; }
            for (std::size_t i = 0; i < resources.maps.size(); ++i) if (resources.maps[i].path == room->snapshot.game.map) {
                map = &resources.maps[i]; impl.map_index = static_cast<int>(i);
            }
            if (map) visible = model::visible_staging_rows(skirmish::setup_row_count(*map), dialog_rows);
            auto planned_snapshot = room->snapshot; planned_snapshot.seats.clear();
            const auto planned = decode_lan_lobby_game(planned_snapshot);
            for (const auto& slot : *authoritative.value().slots) {
                if (slot.slot == 0 || slot.slot > dialog_rows) continue;
                auto& row = rows[slot.slot - 1]; row.kind = model::RowKind::ai;
                if (planned && room->snapshot.seats.size() < room->snapshot.capacity && std::any_of(planned.value().slots->begin(), planned.value().slots->end(), [&](const auto& desired) { return desired.slot == slot.slot && desired.human; })) row.kind = model::RowKind::open;
                row.faction = slot.faction; row.team = slot.team; row.colour = slot.colour_index.value_or(slot.slot - 1); row.accepted = row.kind == model::RowKind::ai;
                bool first = true;
                for (const auto& seat : room->snapshot.seats) if (seat.selection.slot == slot.slot) {
                    row.kind = model::RowKind::human; row.name += (first ? "" : ", ") + seat.selection.name;
                    row.local = row.local || seat.peer == view.seat;
                    row.accepted = first ? seat.ready : row.accepted && seat.ready; first = false;
                }
            }
        }
        const auto local = std::find_if(room->snapshot.seats.begin(), room->snapshot.seats.end(), [&](const auto& seat) { return seat.peer == view.seat; });
        if (!room->identity_pending && local != room->snapshot.seats.end() && (impl.name != local->selection.name || impl.colour != local->selection.colour)) {
            impl.name = local->selection.name; impl.colour = local->selection.colour;
            emit_signal("profile_changed", text(impl.name), static_cast<int64_t>(impl.colour));
        }
        impl.room_revision = room->snapshot.revision;
        for (const auto& chat : room->chat) if (chat.sequence > impl.room_chat_sequence) {
            note_chat(model::ChatKind::normal, chat.name + ": " + chat.text, chat.name);
            impl.room_chat_sequence = chat.sequence;
            if (chat.name != impl.name) play_sound("GUI_Common_Chat_Receive");
        }
        if (room->notice && room->notice->message != impl.room_notice) {
            impl.room_notice = room->notice->message;
            const auto key = room->notice->reason == net::LobbyReason::kicked ? "TEXT_INTERNET_PLAYER_KICKED"
                : room->notice->reason == net::LobbyReason::host_closed ? "TEXT_HOST_LEFT_GAME" : "";
            note_chat(model::ChatKind::error, *key ? key : impl.room_notice);
        }
        if (auto* ready = EAWR_CONTROL(EawrUiButton, "IDC_LOAD_GAME")) {
            ready->set_text(resolved(resources.text, room->local_ready ? "TEXT_BUTTON_CANCEL" : "TEXT_BUTTON_ACCEPT"));
            ready->set_disabled(!room->ready_enabled);
        }
    } else {
        // Who is in the game, from the presence: the host's name for the joiner, the joiner's for the host.
        std::string remote_name;
        std::uint32_t remote_colour = 1;
        if (impl.source) {
            if (impl.host) {
                const auto joining = impl.source->joining(impl.game);
                if (!joining.empty()) {
                    remote_name = joining.front().name;
                    remote_colour = joining.front().colour;
                }
            } else {
                for (const model::LobbyGame& game : impl.source->games()) {
                    if (game.name != impl.game && game.address != impl.address) continue;
                    // A game joined by a typed address learns its name from the host's presence.
                    if (impl.game.empty()) impl.game = game.name;
                    impl.host_name = game.player_names.empty() ? std::string{} : game.player_names.front();
                    for (const model::LobbyPlayer& player : impl.source->players()) {
                        if (player.id == game.host) remote_colour = player.colour;
                    }
                    break;
                }
            }
        }
        if (remote_name.empty() && impl.host && view.connected) remote_name = view.peer_name;

        impl.linked = connected;
        if (impl.host && !connected) impl.remote_colour_applied = false;
        if (impl.host && connected && !impl.remote_colour_applied && !remote_name.empty()
            && remote_colour < resources.palette.size()) {
            // The joiner's row takes its profile colour once; the host may change it afterwards.
            impl.remote_colour_applied = true;
            static_cast<void>(skirmish::select_setup_colour(impl.selection, 2, remote_colour, resources.palette.size()));
            impl.rows_signature.clear();
        }

        // The rows as the player sees them.
        if (impl.host && impl.map_index >= 0) map = &resources.maps[static_cast<std::size_t>(impl.map_index)];
        if (!impl.host && impl.offered && impl.offered->map) {
            for (const skirmish::SetupMap& candidate : resources.maps) {
                if (candidate.path == *impl.offered->map) map = &candidate;
            }
        }
        if (map != nullptr) visible = model::visible_staging_rows(skirmish::setup_row_count(*map), dialog_rows);
        if (impl.host) {
            // Rows describe faction owners; the independent human seats can share a row.
            for (const skirmish::LobbySlot& slot : impl.selection.slots) {
                if (slot.slot == 0 || slot.slot > dialog_rows) continue;
                model::StagingRow& row = rows[slot.slot - 1];
                const auto seats = std::count(impl.human_owners.begin(), impl.human_owners.end(), slot.slot);
                row.kind = seats > 0 ? (slot.slot == 1 || connected ? model::RowKind::human : model::RowKind::open) : model::RowKind::ai;
                row.name = seats == 0 ? std::string{} : slot.slot == 1 ? impl.name : remote_name;
                if (slot.slot != 1 && seats > 0 && impl.human_owners.size() > 2) row.name = "Human side";
                if (seats > 1) row.name += " (" + std::to_string(seats) + " human seats)";
                row.faction = slot.faction;
                row.team = slot.team;
                row.colour = slot.colour_index.value_or(slot.slot - 1);
                row.local = slot.slot == 1;
                row.accepted = slot.slot == 1 || row.kind == model::RowKind::ai || (seats > 0 && connected);
            }
            if (dialog_rows > 1 && rows[1].kind == model::RowKind::none) rows[1].kind = model::RowKind::open;
        } else {
            if (dialog_rows > 0) {
                rows[0].kind = model::RowKind::human;
                rows[0].name = impl.host_name.empty() ? "Host" : impl.host_name;
                rows[0].colour = remote_colour;
                rows[0].accepted = true;
            }
            if (dialog_rows > 1) {
                rows[1].kind = model::RowKind::human;
                rows[1].name = impl.name;
                rows[1].colour = impl.colour;
                rows[1].local = true;
                rows[1].accepted = view.phase == LanPhase::barrier || view.phase == LanPhase::running;
            }
            if (impl.offered && impl.offered->slots) {
                for (const skirmish::LobbySlot& slot : *impl.offered->slots) {
                    if (slot.slot == 0 || slot.slot > dialog_rows) continue;
                    model::StagingRow& row = rows[slot.slot - 1];
                    row.kind = slot.human ? model::RowKind::human : model::RowKind::ai;
                    row.local = slot.human && slot.slot == view.player;
                    row.accepted = !slot.human || view.phase == LanPhase::barrier || view.phase == LanPhase::running;
                    if (slot.slot == 1) row.accepted = true;
                    const auto seats = std::count(view.human_owners.begin(), view.human_owners.end(), slot.slot);
                    if (!slot.human) row.name.clear();
                    else if (view.human_owners.size() > 2 || seats > 1)
                        row.name = (row.local ? impl.name : std::string("Human side")) + " (" + std::to_string(seats) + " human seats)";
                    row.faction = slot.faction;
                    row.team = slot.team;
                    row.colour = slot.colour_index.value_or(slot.slot - 1);
                }
            }
        }
    }
    impl.rows = rows;
    impl.visible_rows = visible;

    // Refill the combos only when what they show changed: a refill closes an open drop-down.
    std::string signature = std::string(impl.host ? "H" : "J") + std::to_string(impl.map_index) + "|"
        + std::to_string(visible) + "|" + (connected ? "c" : "-");
    for (const model::StagingRow& row : rows) {
        signature += "|" + std::to_string(static_cast<int>(row.kind)) + "," + row.name + "," + row.faction + ","
            + (row.team ? std::to_string(*row.team) : std::string("-")) + "," + std::to_string(row.colour) + ","
            + (row.accepted ? "a" : "p");
    }
    if (signature != impl.rows_signature) {
        impl.rows_signature = signature;
        state.loading = true;
        std::vector<std::string> factions;
        if (map != nullptr) {
            for (const skirmish::SetupTeam& team : map->teams) {
                for (const std::string& faction : team.factions) {
                    if (std::find(factions.begin(), factions.end(), faction) == factions.end()) factions.push_back(faction);
                }
            }
        }
        impl.row_states.clear();
        for (std::size_t index = 0; index < dialog_rows; ++index) {
            const model::StagingRow& row = rows[index];
            const std::string suffix = std::to_string(index);
            // The primary owner rows stay visible; the host sees as many as the map holds.
            const bool shown = index < visible || (!impl.host && index < 2);
            auto* name = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_NAME_" + suffix);
            auto* faction = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_FACTION_" + suffix);
            auto* colour = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_COLOR_" + suffix);
            auto* team = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_TEAM_" + suffix);
            const bool editable = impl.host && row.kind == model::RowKind::ai;
            const bool choices_editable = row.occupied() && (impl.host || (room && row.local))
                && (!room || room->snapshot.phase == net::LobbyPhase::staging);
            if (name != nullptr) {
                name->clear();
                name->set_visible(shown);
                if (impl.host && index >= 2 && row.kind != model::RowKind::human && row.kind != model::RowKind::open) {
                    // Unplanned rows offer none or AI; unavailable AI levels stay disabled.
                    // Planned human/open rows use the shared occupancy controls instead.
                    name->add_item(resolved(resources.text, "TEXT_NO_PLAYER"), 0);
                    name->add_item(resolved(resources.text, "TEXT_OPEN_PLAYER"), 1);
                    name->add_item(resolved(resources.text, "TEXT_EASY_AI_PLAYER"), 2);
                    name->add_item(resolved(resources.text, "TEXT_MEDIUM_AI_PLAYER"), 3);
                    name->add_item(resolved(resources.text, "TEXT_HARD_AI_PLAYER"), 4);
                    for (const int item : {1, 2, 4}) name->set_item_disabled(item, true);
                    name->set_tooltip_text("Humans on one side share its units, credits and fog; AI rows use the selected AI level.");
                    name->select(row.kind == model::RowKind::ai ? 3 : 0);
                    name->set_disabled(false);
                } else {
                    std::string label = row.name.empty()
                        ? (row.kind == model::RowKind::open ? std::string("TEXT_OPEN_PLAYER")
                            : row.kind == model::RowKind::ai ? std::string("TEXT_MEDIUM_AI_PLAYER") : std::string{}) : row.name;
                    const auto& owners = impl.host ? impl.human_owners : view.human_owners;
                    const auto seats = room
                        ? std::count_if(room->snapshot.seats.begin(), room->snapshot.seats.end(), [&](const auto& seat) { return seat.selection.slot == index + 1; })
                        : std::count(owners.begin(), owners.end(), static_cast<sim::tactical::PlayerId>(index + 1));
                    if ((impl.host || impl.offered) && seats > 0 && (owners.size() > 2 || seats > 1)) {
                        const auto detail = " (" + std::to_string(seats) + " human seats)";
                        if (label.ends_with(detail)) label.resize(label.size() - detail.size());
                        label = "[" + std::to_string(seats) + "] " + label;
                    }
                    name->add_item(resolved(resources.text, label), 0);
                    name->set_tooltip_text(text(row.name));
                    if (room && row.local && !impl.host) {
                        name->clear();
                        for (const auto& target : rows) if (target.kind == model::RowKind::human || target.kind == model::RowKind::open) {
                            const auto slot = static_cast<int>(&target - rows.data() + 1);
                            name->add_item(text(std::to_string(slot)), slot);
                            if (slot == static_cast<int>(index + 1)) name->select(name->get_item_count() - 1);
                        }
                        name->set_disabled(!choices_editable);
                    } else {
                        name->set_disabled(true);
                        if (room && impl.host && row.kind == model::RowKind::human) {
                            for (const auto& seat : room->snapshot.seats) if (seat.selection.slot == index + 1 && seat.peer != room->snapshot.host) {
                                name->add_item(resolved(resources.text, "TEXT_KICK") + " " + text(seat.selection.name), static_cast<int>(10000 + seat.peer));
                                name->set_disabled(!choices_editable);
                            }
                        }
                    }
                }
            }
            const std::string rename_id = "EAWR_STAGING_NAME_" + suffix;
            auto* rename = EAWR_CONTROL(EawrUiEdit, rename_id);
            if (room && row.local && shown) {
                const auto* authored = find_gadget(*state.dialog, "IDC_PLAYER_NAME_" + suffix);
                if (authored && name) {
                    const Rect2 rect = local_rect(*state.dialog, *authored);
                    if (!rename) {
                        rename = memnew(EawrUiEdit); rename->set_theme(resources.theme);
                        rename->set_max_length(static_cast<int>(model::lan_name_max));
                        rename->connect("text_submitted", callable_mp(this, &EawrLanLobby::on_text_submitted).bind(text(rename_id)));
                        state.dialog->frame->add_child(rename); state.controls[rename_id] = rename;
                    }
                    rename->set_position(rect.position); const bool shared_remote = room && std::any_of(room->snapshot.seats.begin(), room->snapshot.seats.end(), [&](const auto& seat) { return seat.selection.slot == index + 1 && seat.peer != room->snapshot.host; });
                    const bool combo_shown = !impl.host || shared_remote;
                    rename->set_size(Vector2(rect.size.x * (combo_shown ? 0.7F : 1.0F), rect.size.y));
                    rename->set_text(text(impl.name)); rename->set_editable(choices_editable); rename->set_visible(true);
                    name->set_visible(combo_shown);
                    if (combo_shown) { name->set_position(rect.position + Vector2(rect.size.x * 0.7F, 0)); name->set_size(Vector2(rect.size.x * 0.3F, rect.size.y)); }
                }
            } else if (rename) rename->set_visible(false);
            const bool filled = shown && (row.occupied() || !impl.host);
            if (faction != nullptr) {
                faction->clear();
                faction->set_visible(filled);
                for (const std::string& value : factions) faction->add_item(text(value));
                if (!row.faction.empty() && factions.empty()) faction->add_item(text(row.faction));
                for (int item = 0; item < faction->get_item_count(); ++item) {
                    if (utf8(faction->get_item_text(item)) == row.faction) faction->select(item);
                }
                faction->set_disabled(!choices_editable);
            }
            if (colour != nullptr) {
                colour->clear();
                colour->set_visible(filled);
                for (std::size_t item = 0; item < resources.palette.size(); ++item) {
                    const auto& rgb = resources.palette[item].rgb;
                    Ref<Image> swatch = Image::create_empty(12, 12, false, Image::FORMAT_RGBA8);
                    swatch->fill(Color(rgb[0] / 255.0F, rgb[1] / 255.0F, rgb[2] / 255.0F));
                    colour->add_icon_item(ImageTexture::create_from_image(swatch),
                                          text(colour_name(resources.palette[item].constant)), static_cast<int>(item));
                }
                if (row.colour < resources.palette.size()) colour->select(static_cast<int>(row.colour));
                colour->set_disabled(!choices_editable);
            }
            if (team != nullptr) {
                team->clear();
                team->set_visible(filled);
                if (map != nullptr) {
                    for (const skirmish::SetupTeam& side : map->teams) {
                        team->add_item(text("Team " + std::to_string(side.team + 1)), static_cast<int>(side.team));
                    }
                }
                if (row.team) {
                    for (int item = 0; item < team->get_item_count(); ++item) {
                        if (team->get_item_id(item) == static_cast<int>(*row.team)) team->select(item);
                    }
                }
                team->set_disabled(!choices_editable);
            }
            if (auto* kick = EAWR_CONTROL(EawrUiButton, "IDC_BUTTON_KICK_PLAYER" + suffix)) {
                const bool remote = room && std::any_of(room->snapshot.seats.begin(), room->snapshot.seats.end(), [&](const auto& seat) { return seat.selection.slot == index + 1 && seat.peer != room->snapshot.host; });
                kick->set_visible(shown && impl.host && remote);
                kick->set_disabled(!room || room->snapshot.phase != net::LobbyPhase::staging);
            }
            // Faction choices a team has no station for stay listed and disabled, as the setup screen does.
            if (faction != nullptr && choices_editable && map != nullptr && row.team) {
                const auto side = std::find_if(map->teams.begin(), map->teams.end(),
                                               [&](const skirmish::SetupTeam& t) { return t.team == *row.team; });
                for (int item = 0; item < faction->get_item_count(); ++item) {
                    const std::string value = utf8(faction->get_item_text(item));
                    const bool allowed = side != map->teams.end()
                        && std::find(side->factions.begin(), side->factions.end(), value) != side->factions.end();
                    faction->set_item_disabled(item, !allowed);
                }
            }
            const auto icon = state.accept_icons.find("IDC_ACCEPT_PLAYER_" + suffix);
            if (icon != state.accept_icons.end()) {
                icon->second->set_visible(shown && row.occupied());
                icon->second->set_color(row.accepted ? Color(0.2F, 0.85F, 0.3F, 0.95F) : Color(0.15F, 0.15F, 0.15F, 0.9F));
            }
            RowState row_state;
            row_state.kind = row.kind == model::RowKind::human ? "human" : row.kind == model::RowKind::ai ? "ai"
                           : row.kind == model::RowKind::open ? "open" : "none";
            row_state.name = row.name;
            row_state.faction = row.faction;
            row_state.team = row.team ? std::to_string(*row.team + 1) : std::string{};
            row_state.colour = row.colour < resources.palette.size() ? colour_name(resources.palette[row.colour].constant)
                                                                     : std::string{};
            row_state.accepted = row.accepted;
            row_state.enabled = editable || choices_editable;
            row_state.visible = shown;
            impl.row_states.push_back(std::move(row_state));
        }
        // The preview follows the map.
        if (state.preview != nullptr && map != nullptr && impl.preview_path != map->path) {
            impl.preview_path = map->path;
            Ref<Image> image;
            std::string failure;
            if (!map->preview_path.empty()) {
                if (auto texture = assets::load_texture(*resources.content->filesystem, map->preview_path)) {
                    image = texture_image(texture.value(), failure);
                }
            } else if (!map->embedded_preview.empty()) {
                if (auto texture = assets::load_texture(map->embedded_preview, assets::Source{})) {
                    image = texture_image(texture.value(), failure);
                }
            }
            if (image.is_null()) {
                if (auto texture = assets::load_texture(*resources.content->filesystem, "data/art/textures/generic_space.dds")) {
                    image = texture_image(texture.value(), failure);
                }
            }
            state.preview->set_texture(image.is_valid() ? ImageTexture::create_from_image(image) : Ref<ImageTexture>());
        }
        state.loading = false;
    }

    // The start (host) or accept (joiner) button.
    if (auto* start = EAWR_CONTROL(EawrUiButton, "IDC_LAN_START_GAME_BUTTON")) {
        const bool enabled = room ? (impl.host ? room->start_enabled : room->ready_enabled)
            : impl.host ? (view.start_enabled && model::host_start_enabled(rows, impl.human_owners.size())) : view.accept_enabled;
        if (room && !impl.host) start->set_text(resolved(resources.text, room->local_ready ? "TEXT_BUTTON_CANCEL" : "TEXT_BUTTON_ACCEPT"));
        start->set_disabled(!enabled || impl.launch.has_value());
        impl.start_enabled = enabled;
        impl.start_caption = utf8(start->get_text());
    }
    // The chat list.
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_LAN_CHAT_LIST")) draw_chat(*list, impl.staging_chat);
    announce();

    // A launch the transport handed over (the host's Start; the joiner's Accept).
    if (!impl.launch) {
        if (auto launch = match_->take_start()) {
            impl.launch = std::move(launch);
            impl.locked = true;
            announce();
            emit_signal("started");
        }
    }
    // A refused or dropped connection ends the staging once the player has read why.
    if (view.phase == LanPhase::ended && !impl.refused && !impl.launch) {
        impl.refused = true;
        if (room && room->notice && room->notice->reason == net::LobbyReason::kicked) show_failure("TEXT_INTERNET_PLAYER_KICKED");
        else if (room && room->notice && room->notice->reason == net::LobbyReason::host_closed) show_failure("TEXT_HOST_LEFT_GAME");
        else show_failure(lan_message(view));
    }
}

// ---- Scripted use -----------------------------------------------------------------------------

bool EawrLanLobby::run_action(const std::string& action) {
    Impl& impl = *impl_;
    if (!shown_) return false;
    const auto colon = action.find(':');
    const std::string verb = action.substr(0, colon);
    const std::string argument = colon == std::string::npos ? std::string{} : action.substr(colon + 1);
    const auto number = [](const std::string& value, std::uint32_t& out) {
        return !value.empty() && std::from_chars(value.data(), value.data() + value.size(), out).ec == std::errc{};
    };
    if (verb == "source") {
        if (page_ != Page::lobby || (argument != "lan" && argument != "online")) return false;
        select_source(argument == "lan" ? model::LobbySourceKind::lan : model::LobbySourceKind::online);
        return true;
    }
    if (verb == "cancel_join") {
        if (page_ != Page::lobby) return false;
        handle_button(cancel_join_button);
        return true;
    }
    if (verb == "name" && page_ != Page::staging) {
        if (page_ != Page::lobby) return false;
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_LAN_NAME_EDIT")) edit->set_text(text(argument));
        commit_name(argument);
        return true;
    }
    if (verb == "colour" && page_ != Page::staging) {
        std::uint32_t value{};
        if (page_ != Page::lobby || !number(argument, value) || value >= resources_->palette.size()) return false;
        if (auto* combo = EAWR_CONTROL(EawrUiCombo, "IDC_LAN_COLOR_SELECT")) combo->select(static_cast<int>(value));
        commit_colour(value);
        return true;
    }
    if (verb == "chat") {
        if (page_ != Page::lobby && page_ != Page::staging) return false;
        send_chat(argument);
        return true;
    }
    if (verb == "sort") {
        static const std::map<std::string, model::LobbySort> columns{{"name", model::LobbySort::game_name},
            {"type", model::LobbySort::game_type}, {"players", model::LobbySort::players},
            {"map", model::LobbySort::map_name}, {"ping", model::LobbySort::ping}, {"host", model::LobbySort::host}};
        const auto found = columns.find(argument);
        if (found == columns.end()) return false;
        impl.sort = found->second;
        impl.dirty = true;
        return true;
    }
    if (verb == "filter") {
        // filter:clear, or filter:type=<key>, min=<n>, max=<n>, ping=<ms>
        if (argument == "clear") {
            impl.filter.clear();
        } else {
            const auto equals = argument.find('=');
            if (equals == std::string::npos) return false;
            const std::string field = argument.substr(0, equals);
            const std::string value = argument.substr(equals + 1);
            std::uint32_t parsed{};
            const bool numeric = number(value, parsed);
            if (field == "type") impl.filter.game_type = value;
            else if (field == "min" && numeric) impl.filter.min_players = parsed;
            else if (field == "max" && numeric) impl.filter.max_players = parsed;
            else if (field == "ping" && numeric) impl.filter.max_ping_ms = parsed;
            else return false;
        }
        impl.dirty = true;
        return true;
    }
    if (verb == "roster") {
        if (impl.join_selection.source() != model::LobbySourceKind::lan) return false;
        if (page_ != Page::lobby && page_ != Page::host_options) return false;
        const auto slash = argument.find('/');
        const auto next = slash == std::string::npos ? slash : argument.find('/', slash + 1);
        std::uint32_t count{}, first{}, second{};
        if (slash == std::string::npos || !number(argument.substr(0, slash), count)
            || !number(argument.substr(slash + 1, next == std::string::npos ? next : next - slash - 1), first)
            || count < 2 || count > sim::max_match_seats || first >= count) return false;
        if (next == std::string::npos) {
            second = first == 0 ? 0 : count - first;
            if (first == 0) first = count;
        } else if (!number(argument.substr(next + 1), second) || first == 0 || second == 0 || first + second > count) return false;
        if (page_ == Page::lobby) open_host_options();
        fill_host_seats(count);
        model::HumanTeamSplit split{first, second, count - first - second};
        auto canonical = split;
        std::sort(canonical.begin(), canonical.end(), std::greater<>());
        auto* combo = EAWR_CONTROL(EawrUiCombo, "EAWR_LAN_HOST_TEAMS");
        if (!combo || impl.human_owners.size() != count) return false;
        for (int item = 0; item < combo->get_item_count(); ++item)
            if (combo->get_item_id(item) == static_cast<int>(canonical[0] * 10 + canonical[1])) {
                combo->select(item);
                impl.human_owners.assign(count, 1);
                for (std::uint32_t seat = first; seat < count; ++seat)
                    impl.human_owners[seat] = seat < first + second ? 2 : 3;
                return true;
            }
        return false;
    }
    if (verb == "humans" && page_ == Page::host_options) {
        std::uint32_t count{};
        if (!number(argument, count)) return false;
        auto* combo = EAWR_CONTROL(EawrUiCombo, "IDC_INTERNET_HOST_CLAN_COMBO");
        if (!combo) return false;
        for (int item = 0; item < combo->get_item_count(); ++item) if (combo->get_item_id(item) == static_cast<int>(count)) {
            combo->select(item); on_item_selected(item, "IDC_INTERNET_HOST_CLAN_COMBO"); return true;
        }
        return false;
    }
    if (verb == "host_dropdown" && page_ == Page::host_options) {
        const char* id = argument == "humans" ? "IDC_INTERNET_HOST_CLAN_COMBO" : "EAWR_LAN_HOST_TEAMS";
        if (argument != "humans" && argument != "teams") return false;
        if (auto* combo = EAWR_CONTROL(EawrUiCombo, id)) {
            const char* other = argument == "humans" ? "EAWR_LAN_HOST_TEAMS" : "IDC_INTERNET_HOST_CLAN_COMBO";
            if (auto* previous = EAWR_CONTROL(EawrUiCombo, other)) previous->get_popup()->hide();
            combo->show_popup(); return true;
        }
        return false;
    }

    if (verb == "host") {
        const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
        if (online && !impl.online_rooms) return false;
        if (page_ != Page::lobby && page_ != Page::host_options) return false;
        if (page_ == Page::lobby) open_host_options();
        if (page_ != Page::host_options) return false;
        // Online: host:<name>|<password> makes a private room, host:<name> a public one.
        std::string name = argument, room_password;
        if (online) {
            if (const auto bar = argument.find('|'); bar != std::string::npos) {
                name = argument.substr(0, bar);
                room_password = argument.substr(bar + 1);
            }
        }
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_NAME_EDIT")) edit->set_text(text(name));
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_INTERNET_GAME_PASSWORD_EDIT"); edit != nullptr && online) edit->set_text(text(room_password));
        begin_hosting();
        return page_ == Page::staging;
    }
    if (verb == "join") {
        if (page_ != Page::lobby) return false;
        refresh_lobby();
        for (const model::LobbyGame& game : impl.shown) {
            if (game.name == argument) {
                impl.selected_key = game.name + "|" + game.address;
                if (impl.join_selection.source() == model::LobbySourceKind::online) {
                    join_online_selected();
                    return impl.prompt.has_value() || impl.join_selection.attempt().has_value() || page_ == Page::staging;
                }
                join_selected();
                return page_ == Page::staging;
            }
        }
        return false;
    }
    if (verb == "password") {
        if (page_ != Page::lobby || !impl.prompt) return false;
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, prompt_edit)) edit->set_text(text(argument));
        submit_password(argument);
        return impl.prompt.has_value() ? impl.prompt->waiting : page_ == Page::staging;
    }
    if (verb == "prompt_cancel") {
        if (page_ != Page::lobby || !impl.prompt) return false;
        handle_button(prompt_cancel_button);
        return true;
    }
    if (verb == "refresh") {
        if (page_ != Page::lobby || impl.join_selection.source() != model::LobbySourceKind::online) return false;
        refresh_online_rooms();
        return true;
    }
    if (verb == "rooms") {
        if (page_ != Page::lobby || impl.join_selection.source() != model::LobbySourceKind::online
            || (argument != "next" && argument != "prev")) return false;
        handle_button(argument == "next" ? next_button : prev_button);
        return true;
    }
    if (verb == "code") {
        if (page_ != Page::lobby || impl.join_selection.source() != model::LobbySourceKind::online) return false;
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, invitation_edit)) edit->set_text(text(argument));
        join_code(argument);
        return true;
    }
    if (verb == "direct") {
        if (impl.join_selection.source() != model::LobbySourceKind::lan) return false;
        if (page_ != Page::lobby) return false;
        open_direct_connect();
        if (page_ != Page::direct_connect) return false;
        if (auto* edit = EAWR_CONTROL(EawrUiEdit, "IDC_EDIT_IP")) edit->set_text(text(argument));
        handle_button("IDC_IP_CONNECT_BUTTON");
        return page_ == Page::staging;
    }
    if (verb == "cancel") {
        handle_button("IDC_LAN_CANCEL_BUTTON");
        return true;
    }
    if (verb == "leave") {
        if (page_ != Page::staging) return false;
        leave_staging();
        return true;
    }
    if (page_ != Page::staging) return false;
    if (verb == "map" && impl.host) {
        for (std::size_t item = 0; item < impl.listed.size(); ++item) {
            if (resources_->maps[impl.listed[item]].name == argument) {
                if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_STAGING_MAP_LIST")) list->select(static_cast<int>(item));
                on_item_selected(static_cast<int64_t>(item), "IDC_STAGING_MAP_LIST");
                return true;
            }
        }
        return false;
    }
    if (verb == "ai" && impl.host) {
        std::uint32_t row{};
        if (!number(argument, row) || row < 2) return false;
        const std::string id = "IDC_PLAYER_NAME_" + std::to_string(row);
        auto* combo = EAWR_CONTROL(EawrUiCombo, id);
        if (combo == nullptr || combo->get_item_count() < 4) return false;
        combo->select(3);
        on_item_selected(3, text(id));
        return true;
    }
    if (verb == "team" && page_ == Page::staging) {
        const auto equals = argument.find('=');
        if (equals == std::string::npos) return false;
        std::uint32_t row{}, team{};
        if (!number(argument.substr(0, equals), row) || !number(argument.substr(equals + 1), team) || team == 0) return false;
        const std::string id = "IDC_PLAYER_TEAM_" + std::to_string(row);
        auto* combo = EAWR_CONTROL(EawrUiCombo, id);
        if (combo == nullptr) return false;
        for (int item = 0; item < combo->get_item_count(); ++item) {
            if (combo->get_item_id(item) == static_cast<int>(team) - 1) {
                combo->select(item);
                on_item_selected(item, text(id));
                return true;
            }
        }
        return false;
    }
    if ((verb == "ready" || verb == "unready") && page_ == Page::staging && match_->lobby_state()) {
        const bool result = match_->lobby_ready(verb == "ready"); refresh_staging(); return result;
    }
    if (verb == "kick" && impl.host && page_ == Page::staging) { std::uint32_t peer{}; return number(argument, peer) && match_->lobby_kick(peer); }
    if (page_ == Page::staging && match_->lobby_state() && (verb == "slot" || verb == "faction" || verb == "name" || verb == "colour")) {
        const auto room = *match_->lobby_state();
        const auto local = std::find_if(room.snapshot.seats.begin(), room.snapshot.seats.end(), [&](const auto& seat) { return seat.peer == match_->view().seat; });
        if (local == room.snapshot.seats.end()) return false;
        auto choice = local->selection;
        if (verb == "name") choice.name = model::clamp_player_name(argument);
        else if (verb == "faction") choice.faction = argument;
        else if (verb == "slot") { if (!number(argument, choice.slot)) return false; }
        else if (!number(argument, choice.colour)) return false;
        return match_->lobby_select(std::move(choice));
    }
    if (verb == "start" && impl.host) {
        refresh_staging();
        const auto room = match_->lobby_state();
        const auto humans = room ? room->snapshot.seats.size() : impl.human_owners.size();
        const model::StartVerdict verdict = model::check_host_start(impl.rows, impl.map_index >= 0, humans);
        if (!verdict.ok()) {
            if (!verdict.key.empty()) note_chat(model::ChatKind::error, verdict.key);
            return false;
        }
        auto options = validated_lan_options(impl.selection, resources_->maps);
        if (!options) {
            note_chat(model::ChatKind::error, options.error().message);
            return false;
        }
        match_->start(options.value());
        refresh_staging();
        return impl.launch.has_value();
    }
    if (verb == "accept" && !impl.host) {
        if (const auto room = match_->lobby_state()) {
            const bool result = match_->lobby_ready(!room->local_ready); refresh_staging(); return result;
        }
        bool available = false;
        if (impl.offered && impl.offered->map) {
            for (const skirmish::SetupMap& candidate : resources_->maps) {
                if (candidate.path == *impl.offered->map && candidate.unavailable.empty()) available = true;
            }
        }
        const model::StartVerdict verdict = model::check_client_accept(impl.rows, available);
        if (!verdict.ok()) {
            note_chat(model::ChatKind::error, verdict.key);
            return false;
        }
        match_->accept();
        refresh_staging();
        return true;
    }
    return false;
}

bool EawrLanLobby::condition(const std::string& wanted) const {
    if (!impl_ || !shown_) return false;
    const Impl& impl = *impl_;
    const auto colon = wanted.find(':');
    const std::string key = wanted.substr(0, colon);
    const std::string value = colon == std::string::npos ? std::string{} : wanted.substr(colon + 1);
    const auto count_of = [&value](std::uint32_t& out) {
        return !value.empty() && std::from_chars(value.data(), value.data() + value.size(), out).ec == std::errc{};
    };
    if (key == "page") {
        static constexpr const char* names[] = {"lobby", "host", "staging", "direct", "filter"};
        return value == names[static_cast<std::size_t>(page_)];
    }
    if (key == "game") {
        return std::any_of(impl.shown.begin(), impl.shown.end(), [&](const model::LobbyGame& game) { return game.name == value; });
    }
    if (key == "players") {
        std::uint32_t count{};
        return count_of(count) && impl.players.size() >= count;
    }
    if (key == "staging") return page_ == Page::staging;
    if (key == "box") return impl.box != nullptr;
    if (key == "prompt") return impl.prompt.has_value();
    if (key == "prompt_ready") return impl.prompt && !impl.prompt->waiting;
    if (key == "prompt_error") return impl.prompt && impl.prompt->error.find(value) != std::string::npos;
    if (key == "hosting") return model::online_hosting_name(impl.hosting) == value;
    if (key == "rooms") {
        std::uint32_t count{};
        return count_of(count) && impl.online_rooms_total == count;
    }
    if (key == "source") {
        return value == (impl.join_selection.source() == model::LobbySourceKind::online ? "online" : "lan");
    }
    if (key == "room_player") {
        const auto room = match_->lobby_state(); return room && std::any_of(room->snapshot.seats.begin(), room->snapshot.seats.end(), [&](const auto& seat) { return seat.selection.name == value; });
    }
    if (key == "unready") {
        const auto room = match_->lobby_state(); return room && std::none_of(room->snapshot.seats.begin(), room->snapshot.seats.end(), [](const auto& seat) { return seat.ready; });
    }
    if (key == "ready") {
        const auto room = match_->lobby_state(); if (!room) return false;
        if (value == "all") return !room->snapshot.seats.empty() && std::all_of(room->snapshot.seats.begin(), room->snapshot.seats.end(), [](const auto& seat) { return seat.ready; });
        std::uint32_t count{}; return count_of(count) && std::count_if(room->snapshot.seats.begin(), room->snapshot.seats.end(), [](const auto& seat) { return seat.ready; }) >= count;
    }
    if (key == "joined") {
        std::uint32_t count{};
        if (!count_of(count) || page_ != Page::staging || !impl.linked) return false;
        return match_->view().connected_seats >= count;
    }
    if (key == "chat") {
        const auto& lines = (page_ == Page::staging ? impl.staging_chat : impl.lobby_chat).lines();
        return std::any_of(lines.begin(), lines.end(), [&](const model::ChatLine& line) {
            return line.text.find(value) != std::string::npos;
        });
    }
    return false;
}

std::string EawrLanLobby::player_name() const { return impl_ ? impl_->name : std::string{}; }

std::uint32_t EawrLanLobby::player_colour() const { return impl_ ? impl_->colour : 0; }

EawrLanLobby::Snapshot EawrLanLobby::snapshot() const {
    Snapshot shot;
    if (!impl_) return shot;
    const Impl& impl = *impl_;
    static constexpr const char* pages[] = {"lobby", "host_options", "staging", "direct_connect", "filter"};
    shot.page = pages[static_cast<std::size_t>(page_)];
    shot.name = impl.name;
    shot.colour = impl.colour;
    for (const model::LobbyGame& game : impl.shown) {
        const model::LobbyRowText row = impl.join_selection.source() == model::LobbySourceKind::online
            ? model::online_row_text(game) : model::lobby_row_text(game);
        shot.games.push_back(row.name + " | " + row.type + " | " + row.players + " | " + row.map + " | " + row.ping
                             + (game.version_matches ? "" : " | other version") + (game.passworded ? " | password" : ""));
    }
    shot.players.push_back(impl.name);
    for (const model::LobbyPlayer& player : impl.players) {
        if (player.place == model::LobbyPlace::lobby) shot.players.push_back(player.name);
    }
    const auto& lines = (page_ == Page::staging ? impl.staging_chat : impl.lobby_chat).lines();
    for (const model::ChatLine& line : lines) shot.chat.push_back(line.text);
    // The page is gone once the screen closed: the report then has the model's lines only.
    if (const auto* list = state_ ? EAWR_CONTROL(EawrUiList, "IDC_LAN_CHAT_LIST") : nullptr) {
        shot.chat_shown = static_cast<std::size_t>(list->get_item_count());
        if (shot.chat_shown > 0) shot.chat_last_shown = utf8(list->get_item_text(list->get_item_count() - 1));
    }
    shot.rows = impl.row_states;
    shot.status = impl.status;
    if (impl.host && impl.map_index >= 0) shot.map = resources_->maps[static_cast<std::size_t>(impl.map_index)].name;
    else if (impl.offered && impl.offered->map) shot.map = *impl.offered->map;
    shot.start_caption = impl.start_caption;
    shot.start_enabled = impl.start_enabled;
    shot.host = impl.host;
    if (const auto room = match_->lobby_state()) {
        shot.room_revision = room->snapshot.revision; shot.local_ready = room->local_ready; shot.preparing = room->preparing;
        shot.ready_seats = static_cast<std::size_t>(std::count_if(room->snapshot.seats.begin(), room->snapshot.seats.end(), [](const auto& seat) { return seat.ready; }));
    }
    shot.dialog_rows = state_ ? state_->dialog_rows : 0;
    shot.box_open = impl.box != nullptr;
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    shot.source = online ? "online" : "lan";
    if (online) {
        const model::OnlinePageView view = model::online_page(impl.online_rooms_total, impl.online_page);
        shot.room_page = view.page;
        shot.room_pages = view.pages;
        shot.rooms_total = impl.online_rooms_total;
        shot.prompt_open = impl.prompt.has_value();
        shot.prompt_error = impl.prompt ? impl.prompt->error : std::string{};
        shot.hosting = std::string(model::online_hosting_name(impl.hosting));
        shot.room_notice = impl.list_notice;
        shot.chat_open = impl.chat_open;
        shot.room_private = !impl.host_password.empty();
        for (const model::LobbyGame& game : impl.shown) if (game.passworded) shot.locked.push_back(game.name);
    }
    if (state_ && page_ == Page::staging) {
        for (const char* id : {"IDC_SELECT_OFFICIAL_MAPS", "IDC_SELECT_CUSTOM_MAPS", "IDC_STAGING_MAP_LIST", "IDC_SELECT_OPTIONS"}) {
            if (const auto* control = EAWR_CONTROL(Control, id); control != nullptr && !control->is_visible()) shot.hidden.emplace_back(id);
        }
        // The preview is a texture rect the screen makes for the host only.
        if (state_->preview == nullptr) shot.hidden.emplace_back("IDC_IMAGE");
    }
    return shot;
}

} // namespace eawr::presentation::godot_backend
