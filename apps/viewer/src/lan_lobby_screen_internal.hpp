#pragma once
#include "eawr/battle/start.hpp"
#include "lan_lobby_screen.hpp"
#include "online_service_settings.hpp"
#include "dialog_rectangles.hpp"
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
#include "eawr/presentation/ui/skirmish_setup.hpp"
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
#include <cmath>
#include <map>
#include <functional>
#include <optional>
#include <numeric>
#include <set>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace model = presentation::ui;
namespace data = eawr::data::ui;
static_assert(model::lan_command_seat_limit == sim::max_match_seats);

namespace lan_lobby_detail {

// Above the loading screen (98) and under the message box (100): modal, so nothing under it takes
// input (UI-I1).
inline constexpr int screen_layer = 99;
// Keep the established 1080p geometry while following the front end's letterbox rule.
inline constexpr float lobby_width = 1920.0F;
inline constexpr float lobby_height = 1080.0F;

// The retail game types the host options list shows, with the text keys of the name and of the
// description (debug build, MNU-27). The space game and the campaign (Galactic Conquest, GCMP-07) are playable in this build.
struct GameTypeEntry {
    const char* name;
    const char* description;
    bool available;
};
inline constexpr GameTypeEntry game_types[] = {
    {"TEXT_GAME_TYPE_CAMPAIGN", "TEXT_GAME_DESCRIPTION_CAMPAIGN", true},
    {"TEXT_GAME_TYPE_TEAM_LAND", "TEXT_GAME_DESCRIPTION_TEAM_LAND", false},
    {"TEXT_GAME_TYPE_TEAM_SPACE", "TEXT_GAME_DESCRIPTION_TEAM_SPACE01", true},
    {"TEXT_GAME_TYPE_CAPTURE_CONTROL_POINTS", "TEXT_GAME_DESCRIPTION_CAPTURE_CONTROL_POINTS", false},
};

// The game type that opens a Galactic Conquest room (GCMP-07); every other type is a skirmish room.
inline constexpr const char* galactic_game_type = "TEXT_GAME_TYPE_CAMPAIGN";

inline constexpr const char* lobby_dialog = "IDD_LAN_LOBBY_DIALOG";
inline constexpr const char* host_dialog = "IDD_INTERNET_HOST_OPTIONS_DIALOG";
inline constexpr const char* staging_dialog = "IDD_LAN_STAGING_DIALOG";
inline constexpr const char* filter_dialog = "IDD_LOBBY_GAME_LIST_FILTER_DIALOG";
inline constexpr const char* direct_dialog = "IDD_IP_DIRECT_CONNECT_DIALOG";

// Our own extra control on the lobby: retail has no way in for a typed address.
inline constexpr const char* direct_button = "EAWR_LAN_DIRECT_CONNECT_BUTTON";
inline constexpr const char* lan_source_button = "EAWR_LOBBY_LAN_SOURCE";
inline constexpr const char* online_source_button = "EAWR_LOBBY_ONLINE_SOURCE";
inline constexpr const char* invitation_edit = "EAWR_LOBBY_INVITATION_CODE";
inline constexpr const char* cancel_join_button = "EAWR_LOBBY_CANCEL_JOIN";
inline constexpr const char* refresh_button = "EAWR_LOBBY_REFRESH";
inline constexpr const char* prev_button = "EAWR_LOBBY_PREV_PAGE";
inline constexpr const char* next_button = "EAWR_LOBBY_NEXT_PAGE";
inline constexpr const char* page_label = "EAWR_LOBBY_PAGE_LABEL";
inline constexpr const char* code_button = "EAWR_LOBBY_JOIN_CODE";
inline constexpr const char* online_notice_label = "EAWR_LOBBY_ONLINE_NOTICE";
inline constexpr const char* prompt_panel = "EAWR_LOBBY_PROMPT";
inline constexpr const char* prompt_edit = "EAWR_LOBBY_PROMPT_PASSWORD";
inline constexpr const char* prompt_title_label = "EAWR_LOBBY_PROMPT_TITLE";
inline constexpr const char* prompt_error_label = "EAWR_LOBBY_PROMPT_ERROR";
inline constexpr const char* prompt_ok_button = "EAWR_LOBBY_PROMPT_OK";
inline constexpr const char* prompt_cancel_button = "EAWR_LOBBY_PROMPT_CANCEL";

[[nodiscard]] inline String text(const std::string& value) {
    return String::utf8(value.c_str(), static_cast<int64_t>(value.size()));
}

[[nodiscard]] inline std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}

[[nodiscard]] inline bool same_ci(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// True when the two names differ by at most one insertion, deletion, substitution or adjacent swap.
[[nodiscard]] inline bool within_one_edit(const std::string& a, const std::string& b) {
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

[[nodiscard]] inline std::filesystem::path default_font_cache(const std::filesystem::path& given) {
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
[[nodiscard]] inline std::string colour_name(const std::string& constant) {
    constexpr std::string_view prefix = "MP_Color_";
    return constant.compare(0, prefix.size(), prefix) == 0 ? constant.substr(prefix.size()) : constant;
}

[[nodiscard]] inline bool same_slots(const std::optional<std::vector<skirmish::LobbySlot>>& a,
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

// LLB-08: network staging has nine authored row gadgets; local setup keeps WSS-08's eight.
[[nodiscard]] inline std::uint32_t network_row_count(const skirmish::SetupMap& map) {
    return skirmish::setup_row_count(map, false);
}

// Staging is editable before admission too; WSS-17/18 gate Ready and Start.
[[nodiscard]] inline skirmish::FixtureOptions staging_options(const skirmish::SetupSelection& selection) {
    skirmish::FixtureOptions options;
    options.map = selection.map;
    options.slots = selection.slots;
    options.open_slots = selection.open_slots;
    options.match = selection.match;
    if (options.match) {
        if (auto condition = sim::tactical::parse_victory_condition(options.match->space_win_condition))
            options.victory_condition = condition.value();
    }
    return options;
}

[[nodiscard]] inline std::uint32_t now_ms() { return static_cast<std::uint32_t>(Time::get_singleton()->get_ticks_msec()); }

} // namespace lan_lobby_detail
using namespace lan_lobby_detail;

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
    std::vector<battle::LobbyColour> palette;
    battle::MatchOptions defaults;
    std::optional<skirmish::CreditsBounds> credits_bounds; // MNU-22 / WSS-25: the options dialog's credits display
    std::vector<std::string> sfx_ids; // the effective SFX events, for the chat sounds
};

struct EawrLanLobby::PageState final {
    Control* root{};
    Control* canvas{};
    ColorRect* backdrop{};
    Vector2 connections_size;
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
    // The host's advanced options dialog: a copy being edited until Accept (R-SETUP-05).
    std::optional<BuiltDialog> options_dialog;
    Control* options_panel{};
    std::optional<skirmish::SetupOptionsEdit> editing;
    EawrUiCheck *opt_heroes{}, *opt_units{}, *opt_super{};
    EawrUiCombo* opt_win{};
    EawrUiButton *opt_accept{}, *opt_cancel{}, *opt_defaults{};
    bool opt_win_list_was_open{};
    // Set while a control is being filled: its signals must not edit the model.
    bool loading{};
};

struct EawrLanLobby::Impl final {
    std::shared_ptr<model::LobbyGameSource> source;
    std::shared_ptr<net::OnlineRoomSource> online_rooms;
    std::size_t online_page{};
    std::size_t online_rooms_total{};
    model::OnlineMenuChat menu_chat{model::OnlineMenuChat::idle};
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
    std::string start_reason;
    bool start_enabled{};
    std::size_t visible_rows{};
    bool remote_colour_applied{};
    std::uint64_t room_chat_sequence{};
    std::uint64_t room_revision{};
    std::string room_notice;
    std::vector<std::string> online_connections;
    bool linked{}; // the transport's link to the other player is open
    // GCMP-07 (#4363): the Galactic Conquest mode of the staging (lan_lobby_screen_galactic.cpp).
    std::shared_ptr<GalacticLobbyBackend> galactic_backend;
    std::optional<net::GalacticLobbyOptions> galactic_options; // the room's agreed options as last read
    std::vector<std::string> campaign_list;                    // the host's campaign list, in row order
    std::string galactic_campaign;
    bool galactic_seam_noted{};
};

namespace lan_lobby_detail {

template <class T>
[[nodiscard]] inline T* cast(Control* control) {
    return Object::cast_to<T>(control);
}

[[nodiscard]] inline Rect2 local_rect(const BuiltDialog& dialog, const BuiltGadget& gadget) {
    return Rect2(static_cast<float>(gadget.rect.x - dialog.layout.frame.x),
                 static_cast<float>(gadget.rect.y - dialog.layout.frame.y), static_cast<float>(gadget.rect.width),
                 static_cast<float>(gadget.rect.height));
}

[[nodiscard]] inline const BuiltGadget* find_gadget(const BuiltDialog& dialog, const std::string& id) {
    for (const BuiltGadget& gadget : dialog.gadgets) {
        if (gadget.id == id) return &gadget;
    }
    return nullptr;
}

} // namespace lan_lobby_detail
using namespace lan_lobby_detail;

#define EAWR_CONTROL(Type, id) cast<Type>(state_->controls.count(id) != 0 ? state_->controls.at(id) : nullptr)

// The text a key or literal shows, through the text database when there is one.
inline String resolved(std::optional<data::TextLookup>& lookup, const std::string& value) {
    if (!lookup || value.empty()) return text(value);
    return text(data::to_utf8(lookup->text(value)));
}

namespace lan_lobby_detail {
inline void fit_control_caption(Control& control, const char* role, const Vector2 bounds) {
    const KitTextStyle style = kit_text_style(control, role);
    if (style.font.is_null() || style.size <= 0) return;
    for (int caption_size = style.size - 1; caption_size > 0; --caption_size) {
        const Vector2 minimum = control.get_combined_minimum_size();
        if (minimum.x <= bounds.x && minimum.y <= bounds.y) break;
        control.add_theme_font_size_override(role, caption_size);
        control.add_theme_constant_override(String(role) + "_cell_ascent", style.cell_ascent * caption_size / style.size);
        control.add_theme_constant_override(String(role) + "_cell_descent", style.cell_descent * caption_size / style.size);
        control.update_minimum_size();
    }
}
} // namespace lan_lobby_detail
} // namespace eawr::presentation::godot_backend
