#include "lan_lobby_mode.hpp"
#include "settings_store.hpp"

#include "capture_viewport.hpp"
#include "lan_broadcast_source.hpp"
#include "lan_lobby_screen.hpp"
#include "lan_match.hpp"
#include "profile_store.hpp"
#include "viewer_path.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/net/online_lobby_source.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <sstream>
#include <system_error>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace model = presentation::ui;

namespace {

constexpr int warmup_frames = 8;
constexpr int settle_frames = 10;
constexpr double default_wait_seconds = 60.0;

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
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

[[nodiscard]] std::string strings_json(const std::vector<std::string>& values) {
    std::ostringstream output;
    output << "[";
    for (std::size_t index = 0; index < values.size(); ++index) output << (index ? ", " : "") << json(values[index]);
    output << "]";
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

[[nodiscard]] std::string snapshot_json(const EawrLanLobby::Snapshot& shot) {
    std::ostringstream output;
    output << "{\"page\": " << json(shot.page) << ", \"name\": " << json(shot.name) << ", \"colour\": " << shot.colour
           << ", \"host\": " << (shot.host ? "true" : "false") << ", \"map\": " << json(shot.map)
           << ", \"status\": " << json(shot.status) << ", \"start\": {\"caption\": " << json(shot.start_caption)
           << ", \"enabled\": " << (shot.start_enabled ? "true" : "false") << "}"
           << ", \"room_revision\": " << shot.room_revision
           << ", \"local_ready\": " << (shot.local_ready ? "true" : "false")
           << ", \"preparing\": " << (shot.preparing ? "true" : "false")
           << ", \"ready_seats\": " << shot.ready_seats
           << ", \"dialog_rows\": " << shot.dialog_rows << ", \"games\": " << strings_json(shot.games)
           << ", \"players\": " << strings_json(shot.players) << ", \"chat\": " << strings_json(shot.chat)
           << ", \"chat_shown\": " << shot.chat_shown << ", \"chat_last_shown\": " << json(shot.chat_last_shown)
           << ", \"box_open\": " << (shot.box_open ? "true" : "false") << ", \"hidden\": " << strings_json(shot.hidden)
           << ", \"source\": " << json(shot.source) << ", \"room_page\": " << shot.room_page
           << ", \"room_pages\": " << shot.room_pages << ", \"rooms_total\": " << shot.rooms_total
           << ", \"prompt_open\": " << (shot.prompt_open ? "true" : "false") << ", \"prompt_error\": " << json(shot.prompt_error)
           << ", \"hosting\": " << json(shot.hosting) << ", \"room_notice\": " << json(shot.room_notice)
           << ", \"room_private\": " << (shot.room_private ? "true" : "false")
           << ", \"chat_open\": " << (shot.chat_open ? "true" : "false") << ", \"locked\": " << strings_json(shot.locked)
           << ", \"rows\": [";
    for (std::size_t index = 0; index < shot.rows.size(); ++index) {
        const EawrLanLobby::RowState& row = shot.rows[index];
        output << (index ? ", " : "") << "{\"kind\": " << json(row.kind) << ", \"name\": " << json(row.name)
               << ", \"faction\": " << json(row.faction) << ", \"team\": " << json(row.team)
               << ", \"colour\": " << json(row.colour) << ", \"accepted\": " << (row.accepted ? "true" : "false")
               << ", \"enabled\": " << (row.enabled ? "true" : "false") << ", \"visible\": "
               << (row.visible ? "true" : "false") << "}";
    }
    output << "]}";
    return output.str();
}

// The report lists every step. A room password typed by a step stays out of it.
[[nodiscard]] std::string redacted(const std::string& action) {
    if (action.rfind("password:", 0) == 0) return "password:***";
    if (action.rfind("code:", 0) == 0) return "code:***";
    if (action.rfind("host:", 0) == 0) {
        if (const auto bar = action.find('|'); bar != std::string::npos) return action.substr(0, bar) + "|***";
    }
    return action;
}

[[nodiscard]] std::vector<std::string> split(const std::string& text, const char separator) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = text.find(separator, begin);
        parts.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) return parts;
        begin = end + 1;
    }
}

[[nodiscard]] std::optional<std::uint32_t> number(const std::string& text) {
    unsigned value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

// name|map|type|players/max|address[|names[|flags]]
[[nodiscard]] std::optional<model::LobbyGame> parse_game(const std::string& spec) {
    const std::vector<std::string> fields = split(spec, '|');
    if (fields.size() < 5 || fields.size() > 7 || fields[0].empty()) return std::nullopt;
    const std::vector<std::string> counts = split(fields[3], '/');
    if (counts.size() != 2) return std::nullopt;
    const auto players = number(counts[0]);
    const auto maximum = number(counts[1]);
    if (!players || !maximum) return std::nullopt;
    model::LobbyGame game;
    game.name = fields[0];
    game.map = fields[1];
    game.game_type = fields[2];
    game.players = *players;
    game.max_players = *maximum;
    game.address = fields[4];
    if (fields.size() > 5 && !fields[5].empty()) game.player_names = split(fields[5], ',');
    if (fields.size() > 6) {
        for (const std::string& flag : split(fields[6], ',')) {
            if (flag == "password") game.passworded = true;
            else if (flag == "other-version") game.version_matches = false;
            else if (!flag.empty()) return std::nullopt;
        }
    }
    return game;
}

// name|host|map|type|players/max[|flags]; the room id is a stable 128-bit hex string from the position.
[[nodiscard]] std::optional<net::OnlineRoomRow> parse_online_room(const std::string& spec, const std::size_t position) {
    const std::vector<std::string> fields = split(spec, '|');
    if (fields.size() < 5 || fields.size() > 6 || fields[0].empty() || fields[1].empty()) return std::nullopt;
    const std::vector<std::string> counts = split(fields[4], '/');
    if (counts.size() != 2) return std::nullopt;
    const auto players = number(counts[0]);
    const auto maximum = number(counts[1]);
    if (!players || !maximum || *players == 0 || *maximum == 0 || *players > *maximum || *maximum > 9) return std::nullopt;
    net::OnlineRoomRow row;
    char id[40];
    std::snprintf(id, sizeof id, "%032x", static_cast<unsigned>(position + 1));
    row.room = id;
    row.metadata.name = fields[0];
    row.metadata.host_name = fields[1];
    row.metadata.map = fields[2];
    row.metadata.mode = fields[3];
    row.metadata.occupancy = *players;
    row.metadata.capacity = *maximum;
    row.metadata.build = row.metadata.protocol = row.metadata.content = "fixture";
    if (fields.size() > 5) {
        if (fields[5] != "password") return std::nullopt;
        row.passworded = true;
    }
    return row;
}

// name|colour[|place[|game]]
[[nodiscard]] std::optional<model::LobbyPlayer> parse_player(const std::string& spec) {
    const std::vector<std::string> fields = split(spec, '|');
    if (fields.size() < 2 || fields.size() > 4 || fields[0].empty()) return std::nullopt;
    const auto colour = number(fields[1]);
    if (!colour) return std::nullopt;
    model::LobbyPlayer player;
    player.name = fields[0];
    player.colour = *colour;
    if (fields.size() > 2 && !fields[2].empty()) {
        if (fields[2] == "hosting") player.place = model::LobbyPlace::hosting;
        else if (fields[2] == "joining") player.place = model::LobbyPlace::joining;
        else if (fields[2] != "lobby") return std::nullopt;
    }
    if (fields.size() > 3) player.game = fields[3];
    return player;
}

} // namespace

struct LanLobbyMode::State final {
    explicit State(Options value) : options(std::move(value)) {}
    ~State() {
        // A screen that closed itself is out of the tree and ours to free; one still shown was freed
        // with the host before this runs, so the pointer is looked up by id, never used as it is.
        if (screen_id == 0) return;
        if (Node* live = Object::cast_to<Node>(ObjectDB::get_instance(screen_id));
            live != nullptr && live->get_parent() == nullptr) {
            memdelete(live);
        }
    }

    Options options;
    Node3D* host{};
    EawrLanLobby* screen{};
    std::shared_ptr<model::FixedLobbySource> fixed;
    std::shared_ptr<net::FixedOnlineLobbySource> fixed_online;
    // Why the UDP discovery is off, when it could not be opened (the lobby then lists nothing).
    std::string discovery_failure;
    std::uint64_t screen_id{};
    std::uint32_t width{};
    std::uint32_t height{};
    int frames{};
    std::size_t next_action{};
    int sleep_left{};
    std::optional<std::chrono::steady_clock::time_point> wait_deadline;
    int settle{-1};
    bool scripted{};
    bool completed{};
    bool closed{};
    bool started{};
    std::string status{"failed"};
    std::string failure;
    std::vector<std::string> performed;
    std::vector<std::string> snapshots;
    std::vector<std::string> shots;
    std::optional<std::array<std::int32_t, 2>> png;
    std::string capture_sha256;
    // The player profile (UI-only, UI-C2): read when the lobby opens, written back when the name
    // or colour changes. The whole loaded record is kept and saved, never rebuilt from the screen.
    std::optional<ProfileStore> profile_store;
    presentation::ui::PlayerProfile profile;
    std::string saved_name;
    std::uint32_t saved_colour{};
    std::string profile_note;

    void write_back_profile();
    [[nodiscard]] bool run_action(const std::string& action);
    [[nodiscard]] bool capture(const std::filesystem::path& path, bool final);
    [[nodiscard]] bool write_report() const;
};

void LanLobbyMode::State::write_back_profile() {
    if (!profile_store || screen == nullptr) return;
    const std::string name = screen->player_name();
    const std::uint32_t colour = screen->player_colour();
    const bool name_changed = name != saved_name;
    const bool colour_changed = colour != saved_colour;
    if (!name_changed && !colour_changed) return;
    // Only what the player changed is written: a colour-only edit must not pin the platform name
    // into the profile (PRF-06: an empty network name follows the player name).
    if (name_changed) {
        saved_name = name;
        // The lobby name is the network name (MNU-26).
        model::set_network_name(profile, name);
    }
    if (colour_changed) {
        saved_colour = colour;
        // A colour is a row of the colour list.
        model::set_lobby_colour(profile, static_cast<int>(std::min<std::uint32_t>(colour, 1000)));
    }
    std::string error;
    if (!profile_store->save(profile, error)) profile_note = "the profile could not be saved: " + error;
}

bool LanLobbyMode::requested() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (utf8(arguments[index]) == "--eawr-lan-lobby") return true;
    }
    return false;
}

LanLobbyMode::Options LanLobbyMode::from_command_line() {
    Options options;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    const auto port = [](const std::string& value) -> std::optional<std::uint16_t> {
        unsigned parsed{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed > 65535) return std::nullopt;
        return static_cast<std::uint16_t>(parsed);
    };
    for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
        const std::string argument = utf8(arguments[index]);
        const std::string value = utf8(arguments[index + 1]);
        bool consumed = true;
        if (argument == "--eawr-game-root") options.game_root = ViewerPath{value}.native();
        else if (argument == "--eawr-mod-root") options.mod_root = ViewerPath{value}.native();
        else if (argument == "--eawr-font-cache") options.font_cache = ViewerPath{value}.native();
        else if (argument == "--eawr-report") options.report_path = ViewerPath{value}.native();
        else if (argument == "--eawr-capture") options.capture_path = ViewerPath{value}.native();
        else if (argument == "--eawr-lan-name") {
            options.name = value;
            options.name_given = true;
        } else if (argument == "--eawr-profile-file") options.profile_file = ViewerPath{value}.native();
        else if (argument == "--eawr-settings-file") options.settings_file = ViewerPath{value}.native();
        else if (argument == "--eawr-lan-colour") {
            unsigned parsed{};
            if (std::from_chars(value.data(), value.data() + value.size(), parsed).ec == std::errc{}) {
                options.colour = parsed;
                options.colour_given = true;
            }
        } else if (argument == "--eawr-lan-broadcast-port") {
            if (const auto parsed = port(value)) options.broadcast_port = *parsed;
        } else if (argument == "--eawr-lan-broadcast-extra") {
            if (const auto parsed = port(value)) options.extra_ports.push_back(*parsed);
        } else if (argument == "--eawr-lan-game") {
            options.games.push_back(value);
        } else if (argument == "--eawr-lan-player") {
            options.players.push_back(value);
        } else if (argument == "--eawr-lan-game-port") {
            if (const auto parsed = port(value)) options.game_port = *parsed;
        } else if (argument == "--eawr-online-room") options.online_rooms.push_back(value);
        else if (argument == "--eawr-online-password") options.online_passwords.push_back(value);
        else if (argument == "--eawr-online-list-failed") options.online_list_failed = value == "true" || value == "1";
        else if (argument == "--eawr-lan-fake") options.fake = value;
        else if (argument == "--eawr-lan-do") options.actions.push_back(value);
        else if (argument == "--eawr-ui-rules") options.rules = value;
        else if (argument == "--eawr-profile") options.profile = value;
        else consumed = false;
        if (consumed) ++index;
    }
    return options;
}

LanLobbyMode::LanLobbyMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
LanLobbyMode::~LanLobbyMode() = default;
LanLobbyMode::LanLobbyMode(LanLobbyMode&&) noexcept = default;
LanLobbyMode& LanLobbyMode::operator=(LanLobbyMode&&) noexcept = default;

bool LanLobbyMode::State::run_action(const std::string& action) {
    const auto colon = action.find(':');
    const std::string verb = action.substr(0, colon);
    const std::string argument = colon == std::string::npos ? std::string{} : action.substr(colon + 1);
    if (verb == "snapshot") {
        snapshots.push_back(snapshot_json(screen->snapshot()));
        return true;
    }
    if (verb == "cooldown") {
        // cooldown:clear: the fixed service forgets the wrong-password count (a minute has passed).
        if (!fixed_online || argument != "clear") return false;
        fixed_online->clear_cooldown();
        return true;
    }
    if (verb == "online_chat") {
        // online_chat:<name>|<text>: a line the online window receives from the service.
        const auto bar = argument.find('|');
        if (!fixed_online || bar == std::string::npos) return false;
        fixed_online->queue_chat({model::LobbyChatScope::game, {}, argument.substr(0, bar), argument.substr(bar + 1)});
        return true;
    }
    if (verb == "shot") {
        const std::filesystem::path base = options.capture_path.empty() ? options.report_path : options.capture_path;
        if (base.empty()) return false;
        const std::filesystem::path path = base.parent_path() / (argument + ".png");
        if (!capture(path, false)) return false;
        shots.push_back(path.filename().string());
        return true;
    }
    return screen->run_action(action);
}

bool LanLobbyMode::State::capture(const std::filesystem::path& path, const bool final) {
    Ref<Image> image = host->get_viewport()->get_texture()->get_image();
    if (image.is_null() || image->is_empty()) {
        failure = "the viewport could not be read back";
        return false;
    }
    image->convert(Image::FORMAT_RGBA8);
    const PackedByteArray png_bytes = image->save_png_to_buffer();
    if (final) {
        capture_sha256 = core::sha256_hex(
            std::span<const std::uint8_t>(png_bytes.ptr(), static_cast<std::size_t>(png_bytes.size())));
    }
    if (!path.empty()) {
        if (!write_bytes(path, png_bytes)) {
            failure = "the capture PNG could not be written";
            return false;
        }
        if (final) png = std::array<std::int32_t, 2>{image->get_width(), image->get_height()};
    }
    return true;
}

bool LanLobbyMode::State::write_report() const {
    if (options.report_path.empty()) return false;
    RenderingServer* rendering = RenderingServer::get_singleton();
    std::ostringstream output;
    output << "{\n  \"mode\": \"lan_lobby\",\n  \"status\": " << json(status) << ",\n  \"failure\": " << json(failure)
           << ",\n  \"rules\": " << json(options.rules) << ",\n  \"fake\": " << json(options.fake)
           << ",\n  \"backend\": {\"rendering_method\": " << json(utf8(rendering->get_current_rendering_method()))
           << ", \"adapter_name\": " << json(utf8(rendering->get_video_adapter_name()))
           << "},\n  \"viewport\": {\"width\": " << width << ", \"height\": " << height << "}"
           << ",\n  \"closed\": " << (closed ? "true" : "false") << ",\n  \"started\": " << (started ? "true" : "false")
           << ",\n  \"actions\": " << strings_json(performed);
    if (screen != nullptr) {
        output << ",\n  \"notes\": " << strings_json(screen->notes())
               << ",\n  \"sounds\": " << strings_json(screen->sounds())
               << ",\n  \"shown\": " << (screen->shown() ? "true" : "false")
               << ",\n  \"final\": " << snapshot_json(screen->snapshot());
    }
    output << ",\n  \"discovery_failure\": " << json(discovery_failure);
    output << ",\n  \"profile_note\": " << json(profile_note);
    if (fixed) {
        std::vector<std::string> sent;
        for (const model::LobbyChatMessage& message : fixed->sent()) {
            sent.push_back((message.scope == model::LobbyChatScope::game ? "game:" : "lobby:") + message.name + ": "
                           + message.text);
        }
        const model::LobbyPresence& announced = fixed->announced();
        output << ",\n  \"sent_chat\": " << strings_json(sent) << ",\n  \"announced\": {\"place\": "
               << json(announced.place == model::LobbyPlace::hosting ? "hosting"
                       : announced.place == model::LobbyPlace::joining ? "joining" : "lobby")
               << ", \"name\": " << json(announced.name) << ", \"game\": " << json(announced.game) << "}";
    }
    if (fixed_online) {
        std::vector<std::string> sent;
        for (const model::LobbyChatMessage& message : fixed_online->sent()) sent.push_back(message.name + ": " + message.text);
        const model::LobbyPresence& announced = fixed_online->announced();
        // Whether a room password was handed over, never the password itself.
        output << ",\n  \"online\": {\"sent_chat\": " << strings_json(sent) << ", \"announced_place\": "
               << json(announced.place == model::LobbyPlace::hosting ? "hosting" : "lobby")
               << ", \"announced_game\": " << json(announced.game) << ", \"announced_private\": "
               << (announced.passworded ? "true" : "false") << ", \"host_password_set\": "
               << (fixed_online->host_password().empty() ? "false" : "true") << ", \"refreshes\": " << fixed_online->refreshes() << "}";
    }
    output << ",\n  \"snapshots\": [";
    for (std::size_t index = 0; index < snapshots.size(); ++index) output << (index ? ",\n    " : "\n    ") << snapshots[index];
    output << (snapshots.empty() ? "]" : "\n  ]") << ",\n  \"shots\": " << strings_json(shots);
    if (png) {
        output << ",\n  \"capture\": {\"width\": " << (*png)[0] << ", \"height\": " << (*png)[1]
               << ", \"sha256\": " << json(capture_sha256) << "}";
    }
    output << "\n}\n";
    std::error_code error;
    if (!options.report_path.parent_path().empty()) std::filesystem::create_directories(options.report_path.parent_path(), error);
    std::ofstream file(options.report_path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << output.str();
    return static_cast<bool>(file);
}

bool LanLobbyMode::ready(Node3D& host) {
    State& state = *state_;
    state.host = &host;
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
    const bool capturing = !state.options.capture_path.empty();
    if (capturing) pin_capture_viewport(*host.get_window(), state.width, state.height);
    state.scripted = capturing || !state.options.report_path.empty() || !state.options.actions.empty();

    state.screen = memnew(EawrLanLobby);
    state.screen_id = state.screen->get_instance_id();
    EawrLanLobby::Options screen_options;
    screen_options.game_root = state.options.game_root;
    screen_options.mod_root = state.options.mod_root;
    screen_options.font_cache = state.options.font_cache;
    screen_options.profile = state.options.profile;
    screen_options.rules = state.options.rules == "retail" ? model::LayoutRules::retail : model::LayoutRules::aspect_correct;
    // The lobby starts from the profile: its network name (else the player name, else the
    // platform's user name) and its lobby colour. A name or colour on the command line wins.
    state.profile_store = state.options.profile_file.empty() ? ProfileStore{} : ProfileStore{state.options.profile_file};
    state.profile = state.profile_store->load().profile;
    if (!state.options.name_given) {
        std::string platform = utf8(OS::get_singleton()->get_environment("USERNAME"));
        if (platform.empty()) platform = utf8(OS::get_singleton()->get_environment("USER"));
        state.options.name = model::effective_network_name(state.profile, platform);
    }
    if (!state.options.colour_given) state.options.colour = static_cast<std::uint32_t>(std::max(0, state.profile.lobby_colour));
    screen_options.online_settings = SettingsStore{state.options.settings_file}.load().settings.network;
    screen_options.player_name = state.options.name;
    screen_options.player_colour = state.options.colour;
    if (!state.options.games.empty() || !state.options.players.empty()) {
        std::vector<model::LobbyGame> games;
        std::vector<model::LobbyPlayer> players;
        for (const std::string& spec : state.options.games) {
            if (auto game = parse_game(spec)) {
                game->host = static_cast<std::uint64_t>(games.size()) + 1;
                games.push_back(std::move(*game));
            } else {
                state.failure = "bad --eawr-lan-game: " + spec;
            }
        }
        for (const std::string& spec : state.options.players) {
            if (auto player = parse_player(spec)) {
                player->id = static_cast<std::uint64_t>(players.size()) + 100;
                players.push_back(std::move(*player));
            } else {
                state.failure = "bad --eawr-lan-player: " + spec;
            }
        }
        if (!state.failure.empty()) {
            memdelete(state.screen);
            state.screen = nullptr;
            state.screen_id = 0;
            return fail();
        }
        state.fixed = std::make_shared<model::FixedLobbySource>(std::move(games), std::move(players));
        screen_options.source = state.fixed;
    } else {
        BroadcastLobbySource::Options discovery;
        if (state.options.broadcast_port != 0) discovery.port = state.options.broadcast_port;
        discovery.extra_ports = state.options.extra_ports;
        screen_options.source = BroadcastLobbySource::open(std::move(discovery), state.discovery_failure);
    }
    if (!state.options.online_rooms.empty() || state.options.online_list_failed) {
        state.fixed_online = std::make_shared<net::FixedOnlineLobbySource>();
        std::vector<net::OnlineRoomRow> rooms;
        for (const std::string& spec : state.options.online_rooms) {
            if (auto row = parse_online_room(spec, rooms.size())) rooms.push_back(std::move(*row));
            else state.failure = "bad --eawr-online-room: " + spec;
        }
        for (const std::string& spec : state.options.online_passwords) {
            // <room name>=<password>; the password stays in the fixture.
            const auto equals = spec.find('=');
            const auto room = equals == std::string::npos ? rooms.end()
                : std::find_if(rooms.begin(), rooms.end(), [&](const auto& r) { return r.metadata.name == spec.substr(0, equals); });
            if (room == rooms.end()) state.failure = "bad --eawr-online-password";
            else state.fixed_online->set_room_password(room->room, spec.substr(equals + 1));
        }
        if (!state.failure.empty()) {
            memdelete(state.screen);
            state.screen = nullptr;
            state.screen_id = 0;
            return fail();
        }
        state.fixed_online->set_rooms(std::move(rooms));
        state.fixed_online->fail_listing(state.options.online_list_failed);
        screen_options.online_rooms = state.fixed_online;
    }
    if (state.options.game_port) screen_options.game_port = *state.options.game_port;
    screen_options.silent = true;
    if (!state.options.fake.empty()) screen_options.match = make_fake_lan_match(state.options.fake);
    if (!state.screen->build(std::move(screen_options))) {
        state.failure = state.screen->failure();
        memdelete(state.screen);
        state.screen = nullptr;
        state.screen_id = 0;
        return fail();
    }
    state.screen->show(host);
    // What the lobby opened with is what the profile already holds: a change is what gets written.
    state.saved_name = state.screen->player_name();
    state.saved_colour = state.screen->player_colour();
    return true;
}

std::optional<int> LanLobbyMode::process() {
    State& state = *state_;
    if (state.completed) return std::nullopt;
    ++state.frames;
    state.write_back_profile();
    if (state.screen != nullptr && !state.screen->shown() && state.frames > 1) state.closed = true;
    if (state.closed && !state.scripted) {
        // An interactive run ends when the player leaves the lobby.
        state.completed = true;
        state.status = "closed";
        static_cast<void>(state.write_report());
        return 0;
    }
    if (!state.scripted) return std::nullopt;
    if (state.frames <= warmup_frames) return std::nullopt;
    if (state.sleep_left > 0) {
        --state.sleep_left;
        return std::nullopt;
    }
    if (state.next_action < state.options.actions.size() && !state.closed) {
        const std::string& action = state.options.actions[state.next_action];
        if (action.rfind("wait:", 0) == 0) {
            // wait:<condition>[@<seconds>]
            std::string condition = action.substr(5);
            double seconds = default_wait_seconds;
            if (const auto at = condition.rfind('@'); at != std::string::npos) {
                seconds = std::strtod(condition.c_str() + at + 1, nullptr);
                condition.resize(at);
            }
            if (!state.wait_deadline) {
                state.wait_deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
            }
            if (state.screen->condition(condition)) {
                state.performed.push_back(action + " ok");
                state.wait_deadline.reset();
                ++state.next_action;
            } else if (std::chrono::steady_clock::now() > *state.wait_deadline) {
                state.performed.push_back(action + " TIMEOUT");
                if (state.failure.empty()) state.failure = "wait timed out: " + action;
                state.wait_deadline.reset();
                // A run that cannot reach its state ends instead of running the rest blind.
                state.next_action = state.options.actions.size();
            }
            return std::nullopt;
        }
        if (action.rfind("sleep:", 0) == 0) {
            state.sleep_left = std::atoi(action.c_str() + 6);
            state.performed.push_back(action + " ok");
            ++state.next_action;
            return std::nullopt;
        }
        ++state.next_action;
        const bool ok = state.run_action(action);
        state.performed.push_back(redacted(action) + (ok ? " ok" : " FAILED"));
        if (!ok && state.failure.empty()) state.failure = "action failed: " + redacted(action);
        return std::nullopt;
    }
    if (state.settle < 0) state.settle = settle_frames;
    if (state.settle > 0) {
        --state.settle;
        return std::nullopt;
    }
    state.completed = true;
    const bool captured = state.capture(state.options.capture_path, true);
    state.status = captured && state.failure.empty() ? "ok" : "failed";
    const bool written = state.write_report();
    return state.failure.empty() && written == !state.options.report_path.empty() ? 0 : 2;
}

} // namespace eawr::presentation::godot_backend
