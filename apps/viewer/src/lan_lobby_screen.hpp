#pragma once

// The LAN multiplayer screens (docs/ui/menu-screens.md MNU-26 to MNU-29): the LAN lobby with its
// game list, filter, chat and name and colour; the host options page; the LAN staging with the
// dialog's player rows; and the direct-connect page. All four are the retail dialogs built from the
// loaded dialog resources through the kit's dialog builder, over the existing lan_match transport
// (host by port, join by address, setup offer, accept, start) and a LobbyGameSource that feeds the game
// list, the players and the chat. The model (presentation/ui/lan_lobby.hpp) decides lists, checks and texts.
//
// Reuse: a menu creates one screen, calls build() once, then show(parent). The screen emits
// `closed` when the lobby's Cancel (or Esc on the lobby) is pressed: retail returns to the main
// menu from there. It emits `started` when a match is ready to load: take_start() then gives the
// FixtureOptions and lan_match() the transport, exactly what the skirmish setup screen hands the
// viewer for a LAN battle. It is a modal layer (UI-I1).
//
// Ownership: `closed` (and dismiss()) only remove the screen from the tree; they do not free it. Whoever
// owns the screen (the standalone mode's state, the front-end shell) must free it after it closes, or
// it leaks.

#include "battle_content.hpp"
#include "lan_match.hpp"

#include "eawr/net/online_lobby_source.hpp"
#include "eawr/presentation/ui/settings.hpp"
#include "eawr/net/tcp_transport.hpp"
#include "eawr/presentation/ui/lan_lobby.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/skirmish/start.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

class EawrLanLobby final : public godot::CanvasLayer {
    GDCLASS(EawrLanLobby, godot::CanvasLayer)

public:
    enum class Page : std::uint8_t { lobby, host_options, staging, direct_connect, filter };

    struct Options final {
        // The content the caller already loaded (the setup screen's); otherwise the roots below
        // are mounted and the object catalogue is read, as the setup screen does.
        std::shared_ptr<const BattleContent> content;
        std::filesystem::path game_root;
        std::filesystem::path mod_root;
        std::string profile;
        // Empty: --eawr-font-cache, else EAWR_FONT_CACHE, else the checkout's out/fonts.
        std::filesystem::path font_cache;
        presentation::ui::LayoutRules rules{presentation::ui::LayoutRules::aspect_correct};
        std::string language{"ENGLISH"};
        // The transport; null makes the real TCP one. A presentation fixture passes a fake.
        std::shared_ptr<LanMatch> match;
        // Where the match's desync diagnostics go; empty uses the transport's default.
        std::filesystem::path diagnostic_root;
        std::uint64_t input_delay{3};
        // Where the game list, the players and the chat come from, and where the lobby's own presence
        // and chat go. Null: the list stays empty and a typed address (direct connect) still joins.
        std::shared_ptr<presentation::ui::LobbyGameSource> source;
        // The online room source behind the Online half of the same screen (public and password
        // rooms, refresh, the password join and the hosting state). Null: Online stays unavailable.
        std::shared_ptr<net::OnlineRoomSource> online_rooms;
        // Null loads the player's saved settings. Fixtures keep their supplied sources.
        std::optional<presentation::ui::NetworkSettings> online_settings;
        // The TCP port a hosted game listens on; 0 lets the system pick one (the lobby announces it).
        std::uint16_t game_port{net::TcpOptions::default_port};
        // The profile's name and colour until the player changes them.
        std::string player_name{"Player"};
        std::uint32_t player_colour{};
        // Quiet runs (captures and tests) do not ask for sounds.
        bool silent{};
    };

    // What the screen shows, for the report and the GPU case.
    struct RowState final {
        std::string kind; // none, open, human, ai
        std::string name;
        std::string faction;
        std::string team;
        std::string colour;
        bool accepted{};
        bool enabled{};
        bool visible{};
    };
    struct Snapshot final {
        std::string page;
        std::string name;
        std::uint32_t colour{};
        std::vector<std::string> games;      // lobby rows as shown
        std::vector<std::string> players;    // lobby player list
        std::vector<std::string> chat;       // the chat log (the model's lines)
        std::size_t chat_shown{};            // items in the drawn chat list
        std::string chat_last_shown;         // the drawn list's last item
        std::vector<RowState> rows;          // staging rows
        std::string status;                  // the staging's or lobby's last message
        std::string map;
        std::string start_caption;
        bool start_enabled{};
        bool host{};
        std::uint64_t room_revision{};
        bool local_ready{}, preparing{};
        std::size_t ready_seats{};
        std::size_t dialog_rows{};
        bool box_open{};                     // a message box (refused connection) is up
        std::vector<std::string> hidden;     // staging controls the page keeps hidden (preview, options, map list)
        std::string source;                  // lan or online
        std::size_t room_page{}, room_pages{1};
        std::size_t rooms_total{};           // online rooms after the filter, over all pages
        bool prompt_open{};                  // the online password prompt is up
        std::string prompt_error;            // its last refusal, as shown
        std::string hosting;                 // idle, publishing, hosted or retrying
        std::string room_notice;             // the empty/unavailable line the online list shows
        bool chat_open{};                    // the online chat window can send
        bool room_private{};                 // the room being hosted asks for a password
        std::vector<std::string> locked;     // names of the rooms drawn with the lock
    };

    EawrLanLobby();
    ~EawrLanLobby() override;

    // Loads the dialog catalogue, skin, fonts, text, maps and colours. False with failure() set
    // when the data could not be read; the screen then cannot be shown.
    [[nodiscard]] bool build(Options options);
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }

    // Adds the screen to `parent`, opens the lobby and starts announcing. Emits `closed` once the
    // player leaves it; the screen is then out of the tree.
    void show(godot::Node& parent, std::optional<presentation::ui::LobbySourceKind> initial_source = std::nullopt);
    [[nodiscard]] bool shown() const noexcept { return shown_; }
    // Removes the screen without the `closed` signal: the menu that took a started match calls it.
    void dismiss();
    [[nodiscard]] Page page() const noexcept { return page_; }

    // The match that is ready to load, once, after `started`.
    [[nodiscard]] std::optional<skirmish::FixtureOptions> take_start();
    [[nodiscard]] std::shared_ptr<LanMatch> lan_match() const { return match_; }

    // Scripted use (the standalone mode and the GPU case): a player's click or edit by name.
    //   name:<text>  colour:<index>  chat:<text>  filter:<field>=<value>  sort:<column>
    //   host:<game name>  join:<game name>  direct:<ip[:port]>  map:<name>  ai:<row>  team:<row>=<n>
    //   accept  start  cancel  leave
    // Online half: source:online|lan  refresh  rooms:next|prev  code:<text>  password:<text>
    //   prompt_cancel  (host:<name>|<password> makes a private online room)
    [[nodiscard]] bool run_action(const std::string& action);
    // True when the condition holds; the conditions the standalone mode's waits use:
    //   game:<name>  players:<n>  staging  joined:<n>  chat:<text>  page:<name>
    //   prompt  prompt_error:<text>  hosting:<state>  rooms:<n>
    [[nodiscard]] bool condition(const std::string& wanted) const;
    [[nodiscard]] Snapshot snapshot() const;
    // The lobby name and colour as they stand (the profile writes them back when they change).
    [[nodiscard]] std::string player_name() const;
    [[nodiscard]] std::uint32_t player_colour() const;
    [[nodiscard]] const std::vector<std::string>& sounds() const noexcept { return sounds_; }

    void _input(const godot::Ref<godot::InputEvent>& event) override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    struct Resources;
    struct PageState;
    struct Impl;

    // Pages
    void open_lobby();
    void open_host_options();
    void fill_host_seats(std::uint32_t requested_count = 0);
    void prepare_host_rows(const skirmish::SetupMap&);
    void configure_staging();
    void publish_staging_selection();
    void open_staging(bool host, const std::string& game, const std::string& address);
    void open_direct_connect();
    void open_filter();
    void fill_map_list();
    void clear_page();
    void close_screen(bool emit);
    void build_page(const char* dialog_name);

    // Signal sinks, bound with the gadget id.
    void on_pressed(godot::String id);
    void on_text_submitted(godot::String text, godot::String id);
    void on_item_selected(int64_t index, godot::String id);
    void on_rows_chosen(int64_t index, bool activated);
    void on_box_result(int64_t result);

    void handle_button(const std::string& id);
    void refresh_lobby();
    void draw_chat(class EawrUiList& list, const presentation::ui::ChatLog& log);
    void commit_name_from_edit();
    void refresh_staging();
    void commit_name(const std::string& typed);
    void commit_colour(std::uint32_t colour);
    void send_chat(const std::string& typed);
    void join_selected();
    void join_address(const std::string& address, const std::string& game);
    void select_source(presentation::ui::LobbySourceKind source);
    bool configure_online_match();
    void cancel_join();
    void join_code(const std::string& code);
    // Online rooms: the list, the password prompt and the hosting state.
    void refresh_online_rooms();
    void join_online_selected();
    void begin_online_join(const std::string& game, const std::string& room, const std::string& password);
    void open_password_prompt(const presentation::ui::LobbyGame& game);
    void close_password_prompt();
    void submit_password(const std::string& typed);
    void pump_online();
    void online_failure(const core::Diagnostic& diagnostic);
    void begin_hosting();
    void leave_staging();
    void announce();
    void show_failure(const std::string& message);
    void note_chat(presentation::ui::ChatKind kind, const std::string& line, const std::string& sender = {});
    void play_sound(const std::string& wanted);

    std::unique_ptr<Resources> resources_;
    std::unique_ptr<PageState> state_;
    std::unique_ptr<Impl> impl_;
    Options options_;
    std::shared_ptr<LanMatch> match_;
    std::shared_ptr<LanMatch> lan_match_;
    std::shared_ptr<LanMatch> online_match_;
    std::vector<std::string> notes_;
    std::vector<std::string> sounds_;
    std::string failure_;
    Page page_{Page::lobby};
    bool shown_{};
    bool built_{};
    bool loading_{};
};

void register_lan_lobby_classes();

} // namespace eawr::presentation::godot_backend
