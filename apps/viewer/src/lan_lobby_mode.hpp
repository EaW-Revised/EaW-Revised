#pragma once

#include <godot_cpp/classes/node3d.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// A self-contained viewer mode (`--eawr-lan-lobby`, #2940) that shows the LAN screens
// (lan_lobby_screen.hpp) over a black backdrop with the FoC data's skin.
//
//   --eawr-lan-lobby                       open the lobby; the window stays until Cancel
//   --eawr-lan-name <text>                 the player's name (default Player)
//   --eawr-lan-colour <n>                  the player's colour index
//   --eawr-lan-broadcast-port <port>       the UDP port this lobby listens on (default 47625)
//   --eawr-lan-broadcast-extra <port>      one more UDP port to send to, repeatable
//   --eawr-lan-game <spec>                 a game the lobby lists (a fixed source instead of the UDP
//                                          discovery),
//                                          repeatable: name|map|type|players/max|address[|names[|flags]],
//                                          names comma separated (the host first), flags password and
//                                          other-version comma separated
//   --eawr-lan-player <spec>               a player the lobby lists: name|colour[|place[|game]], place
//                                          lobby (default), hosting or joining, repeatable
//   --eawr-lan-game-port <port>            the TCP port a hosted game listens on (0: any free one)
//   --eawr-online-room <spec>              a room the Online half lists (a fixed online source, no
//                                          network): name|host|map|type|players/max[|password];
//                                          repeatable
//   --eawr-online-password <room>=<pw>     the password a fixture room asks for, repeatable
//   --eawr-online-list-failed <bool>       the fixed source reports its room list as unavailable
//   --eawr-lan-fake <state>                a presentation fixture transport (lan_match_fake) instead
//                                          of the TCP one: host_waiting, joined, review, refused ...
//   --eawr-lan-do <action>                 a scripted step, repeatable, one per frame, in order:
//                                            name:<text>  colour:<n>  chat:<text>  sort:<column>
//                                            filter:<field>=<value> | filter:clear
//                                            host:<game name>  join:<game name>  direct:<ip[:port]>
//                                            map:<name>  ai:<row>  team:<row>=<n>
//                                            start  accept  cancel  leave
//                                            wait:<condition>[@<seconds>]  (game:<name> players:<n>
//                                              staging joined:<n> chat:<text> page:<name>)
//                                            online_chat:<name>|<text>  (a line the online window receives)
//                                            resize:<width>x<height> (real window resize)
//                                            sleep:<frames>  snapshot  shot:<file stem>
//   --eawr-ui-rules aspect|retail          the layout rules, as the UI gallery's
//   --eawr-capture <png>, --eawr-report <json>
//
// A scripted run ends by itself after its last step: it writes the PNG and the report (the
// snapshots, what each step did, the sounds the screen asked for) and exits.
class LanLobbyMode final {
public:
    struct Options final {
        std::filesystem::path game_root;
        std::filesystem::path mod_root;
        std::filesystem::path font_cache;
        std::filesystem::path report_path;
        std::filesystem::path capture_path;
        std::string profile;
        // --eawr-lan-name / --eawr-lan-colour win over the profile; without them the lobby starts
        // from the profile's network name and colour.
        std::string name{"Player"};
        std::uint32_t colour{};
        bool name_given{};
        bool colour_given{};
        // --eawr-profile-file: the player profile to use (a test names its own); default user://.
        std::filesystem::path profile_file;
        std::filesystem::path settings_file;
        std::uint16_t broadcast_port{};
        std::vector<std::uint16_t> extra_ports;
        std::vector<std::string> games;
        std::vector<std::string> players;
        std::vector<std::string> online_rooms, online_passwords;
        bool online_list_failed{};
        std::optional<std::uint16_t> game_port;
        std::string fake;
        std::string rules{"aspect"};
        std::vector<std::string> actions;
    };

    [[nodiscard]] static bool requested();
    [[nodiscard]] static Options from_command_line();

    explicit LanLobbyMode(Options options);
    ~LanLobbyMode();
    LanLobbyMode(LanLobbyMode&&) noexcept;
    LanLobbyMode& operator=(LanLobbyMode&&) noexcept;
    LanLobbyMode(const LanLobbyMode&) = delete;
    LanLobbyMode& operator=(const LanLobbyMode&) = delete;

    // Loads the data and opens the lobby. A false return has written the report.
    [[nodiscard]] bool ready(godot::Node3D& host);
    // One frame; returns the exit code once the report has been written.
    [[nodiscard]] std::optional<int> process();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
