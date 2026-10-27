#include "eawr/presentation/ui/settings.hpp"
#include "eawr/core/text.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string>
#include <system_error>

namespace eawr::presentation::ui {
namespace {

[[nodiscard]] std::optional<int> parse_int(const std::string_view text) {
    int value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
    return value;
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    // A carriage return is a separator only at the end of these line values.
    const auto last = text.find_last_not_of(core::line_whitespace);
    return core::trim_view(text.substr(0, last == std::string_view::npos ? 0 : last + 1),
        core::horizontal_whitespace);
}

[[nodiscard]] std::string_view profile_name(const RenderProfileSetting setting) noexcept {
    switch (setting) {
    case RenderProfileSetting::retail: return "retail";
    case RenderProfileSetting::enhanced: return "enhanced";
    case RenderProfileSetting::automatic: break;
    }
    return "automatic";
}

[[nodiscard]] std::optional<RenderProfileSetting> parse_profile(const std::string_view text) noexcept {
    if (text == "automatic") return RenderProfileSetting::automatic;
    if (text == "retail") return RenderProfileSetting::retail;
    if (text == "enhanced") return RenderProfileSetting::enhanced;
    return std::nullopt;
}

// One integer field of the record: its text key, where it lives and the bounds that make it
// valid. Booleans are 0 and 1.
struct IntField final {
    std::string_view key;
    int (*get)(const PlayerSettings&);
    void (*set)(PlayerSettings&, int);
    int minimum;
    int maximum;
};

#define EAWR_SETTINGS_FIELD(KEY, PATH, MIN, MAX)                                                    \
    IntField {                                                                                      \
        KEY, [](const PlayerSettings& s) { return static_cast<int>(s.PATH); },                      \
            [](PlayerSettings& s, const int v) { s.PATH = static_cast<decltype(s.PATH)>(v); }, MIN, \
            MAX                                                                                     \
    }

constexpr int speed_last = speed_step_count - 1;
constexpr int scroll_last = scroll_stop_count - 1;

const IntField fields[] = {
    EAWR_SETTINGS_FIELD("keyboard.camera_wasd", camera_wasd, 0, 1),
    EAWR_SETTINGS_FIELD("audio.master", audio.master_percent, 0, 100),
    EAWR_SETTINGS_FIELD("audio.sfx", audio.sfx_percent, 0, 100),
    EAWR_SETTINGS_FIELD("audio.speech", audio.speech_percent, 0, 100),
    EAWR_SETTINGS_FIELD("audio.music", audio.music_percent, 0, 100),
    EAWR_SETTINGS_FIELD("audio.subtitles", audio.subtitles, 0, 1),
    EAWR_SETTINGS_FIELD("video.fullscreen_width", video.fullscreen_width, display_size_minimum, display_size_maximum),
    EAWR_SETTINGS_FIELD("video.fullscreen_height", video.fullscreen_height, display_size_minimum, display_size_maximum),
    EAWR_SETTINGS_FIELD("video.window_width", video.window_width, display_size_minimum, display_size_maximum),
    EAWR_SETTINGS_FIELD("video.window_height", video.window_height, display_size_minimum, display_size_maximum),
    EAWR_SETTINGS_FIELD("video.windowed", video.windowed, 0, 1),
    EAWR_SETTINGS_FIELD("video.vsync", video.vsync, 0, 1),
    EAWR_SETTINGS_FIELD("video.render_thread", video.render_thread, 0, 1),
    EAWR_SETTINGS_FIELD("video.frame_rate_limit", video.frame_rate_limit, -1, std::numeric_limits<int>::max()),
    EAWR_SETTINGS_FIELD("video.detail_level", video.detail_level, 0, detail_level_maximum),
    EAWR_SETTINGS_FIELD("video.texture_mip_drop", video.texture_mip_drop, 0, 4),
    EAWR_SETTINGS_FIELD("video.gamma_permille", video.gamma_permille, 0, 4000),
    EAWR_SETTINGS_FIELD("video.widescreen", video.widescreen, 0, 1),
    EAWR_SETTINGS_FIELD("video.hardware_cursor", video.hardware_cursor, 0, 1),
    EAWR_SETTINGS_FIELD("gameplay.tactical_speed", gameplay.tactical_speed, 0, speed_last),
    EAWR_SETTINGS_FIELD("gameplay.strategic_speed", gameplay.strategic_speed, 0, speed_last),
    EAWR_SETTINGS_FIELD("gameplay.scroll_stop", gameplay.scroll_stop, 0, scroll_last),
    EAWR_SETTINGS_FIELD("gameplay.absolute_scrolling", gameplay.absolute_scrolling, 0, 1),
    EAWR_SETTINGS_FIELD("gameplay.delay_tooltips", gameplay.delay_tooltips, 0, 1),
    EAWR_SETTINGS_FIELD("gameplay.right_button_scroll", gameplay.right_button_scroll, 0, 1),
    EAWR_SETTINGS_FIELD("gameplay.both_buttons_rotate", gameplay.both_buttons_rotate, 0, 1),
    EAWR_SETTINGS_FIELD("gameplay.alternate_mouse", gameplay.alternate_mouse, 0, 1),
    EAWR_SETTINGS_FIELD("gameplay.simplified_faction_colours", gameplay.simplified_faction_colours, 0, 1),
    EAWR_SETTINGS_FIELD("gameplay.abilities_autofire", gameplay.abilities_autofire, 0, 1),
    EAWR_SETTINGS_FIELD("gameplay.ask_auto_resolve", gameplay.ask_auto_resolve, 0, 1),
    EAWR_SETTINGS_FIELD("network.port", network.port, 0, 65535),
    EAWR_SETTINGS_FIELD("network.allow_nat", network.allow_nat, 0, 1),
    EAWR_SETTINGS_FIELD("network.allow_firewall", network.allow_firewall, 0, 1),
    EAWR_SETTINGS_FIELD("network.allow_upnp", network.allow_upnp, 0, 1),
    EAWR_SETTINGS_FIELD("network.quickmatch_default", network.quickmatch_default, 0, 2),
    EAWR_SETTINGS_FIELD("network.preferred_faction", network.preferred_faction, 0, 64),
    EAWR_SETTINGS_FIELD("network.connection_speed", network.connection_speed, 0, 1),
    EAWR_SETTINGS_FIELD("network.lobby_locale", network.lobby_locale, -1, 8),
};

#undef EAWR_SETTINGS_FIELD

// A detail slot is -1 (follow the basic level) or a slider value; the bound is a sanity bound,
// not a FoC limit (ST-V4).
constexpr int detail_slot_maximum = 16;

[[nodiscard]] std::string detail_key(const std::size_t slot) { return "video.detail." + std::to_string(slot); }

[[nodiscard]] bool clamp_field(int& value, const int minimum, const int maximum) {
    const int clamped = std::clamp(value, minimum, maximum);
    const bool changed = clamped != value;
    value = clamped;
    return changed;
}

// Command names are identifiers: letters, digits and underscores, so a key line stays one line.
[[nodiscard]] bool valid_service_host(const std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    if (host.find(':') != std::string_view::npos) {
        const auto compressed = host.find("::");
        if (host.find(":::") != std::string_view::npos || (compressed != std::string_view::npos && host.find("::", compressed + 2) != std::string_view::npos)) return false;
        if (compressed == std::string_view::npos && (host.front() == ':' || host.back() == ':')) return false;
        std::size_t groups = 0, from = 0;
        while (from <= host.size()) {
            const auto end = std::min(host.find(':', from), host.size()); const auto group = host.substr(from, end - from);
            if (!group.empty()) {
                if (group.size() > 4 || !std::all_of(group.begin(), group.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); })) return false;
                ++groups;
            }
            from = end + 1;
        }
        return compressed == std::string_view::npos ? groups == 8 : groups < 8;
    }
    std::size_t from = 0;
    while (from <= host.size()) {
        const auto end = std::min(host.find('.', from), host.size()); const auto label = host.substr(from, end - from);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-'
            || !std::all_of(label.begin(), label.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'; })) return false;
        from = end + 1;
    }
    return true;
}

[[nodiscard]] bool valid_command_name(const std::string_view name) {
    return !name.empty() && name.size() <= 64
        && std::all_of(name.begin(), name.end(), [](const char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
           });
}

[[nodiscard]] bool valid_key_text(const std::string_view text) {
    return !text.empty() && text.size() <= 32
        && std::all_of(text.begin(), text.end(), [](const char c) {
               return static_cast<unsigned char>(c) >= 0x20U && static_cast<unsigned char>(c) < 0x7FU;
           });
}

constexpr OptionStatus honoured_option(const std::string_view control) { return {control, true, {}}; }
constexpr OptionStatus refused_option(const std::string_view control, const std::string_view reason) {
    return {control, false, reason};
}

constexpr std::string_view reason_detail = "Detail levels are not built yet.";
constexpr std::string_view reason_online = "Online play is not built yet.";
constexpr std::string_view reason_mouse = "The camera takes its mouse controls from the project's bindings.";
constexpr std::string_view reason_keys = "Keys cannot be rebound yet.";

constexpr OptionStatus statuses[] = {
    // Audio (ST-A1, ST-A2).
    honoured_option("IDC_SLIDER_MASTER_VOLUME"),
    honoured_option("IDC_SLIDER_SOUND_EFFECTS_VOLUME"),
    honoured_option("IDC_SLIDER_DIALOG_VOLUME"),
    honoured_option("IDC_SLIDER_MUSIC_VOLUME"),
    refused_option("IDC_USE_MOVIE_SUBTITLES", "No movie with subtitles plays yet."),
    refused_option("IDC_AUDIO_3D_TECHNOLOGY_COMBOBOX", "The sound output has no provider choice."),
    // Video (ST-V1 to ST-V3).
    honoured_option("IDC_RESOLUTION_LIST"),
    honoured_option("IDC_WINDOWED_MODE"),
    honoured_option("IDC_VSYNC_ENABLED"),
    honoured_option("IDC_ANTI_ALIAS_SLIDER"),
    refused_option("IDC_DETAIL_LEVEL_SLIDER", reason_detail),
    refused_option("IDC_GAMMA_SLIDER", "Gamma is not built yet."),
    refused_option("IDC_WIDESCREEN", "The layout already follows the window's shape."),
    refused_option("IDC_HARDWARE_MOUSE", "The cursor kind is not selectable yet."),
    refused_option("IDC_AUTO_DETECT", reason_detail),
    // Advanced video.
    refused_option("IDC_PARTICLE_DETAIL_SLIDER", reason_detail),
    refused_option("IDC_MESH_DETAIL_SLIDER", reason_detail),
    refused_option("IDC_TEXTURE_MIP_DETAIL_SLIDER", reason_detail),
    refused_option("IDC_WATER_DETAIL_SLIDER", reason_detail),
    refused_option("IDC_SHADER_DETAIL_SLIDER", reason_detail),
    refused_option("IDC_SHADOW_DETAIL_SLIDER", reason_detail),
    refused_option("IDC_ENVIRONMENT_DETAIL_SLIDER", reason_detail),
    refused_option("IDC_SOFT_SHADOWS", reason_detail),
    refused_option("IDC_HEAT_DISTORTIONS", reason_detail),
    refused_option("IDC_BLOOM", reason_detail),
    // Gameplay (ST-G1 to ST-G3).
    honoured_option("IDC_GAME_SPEED_SLIDER"),
    refused_option("IDC_STRATEGIC_GAME_SPEED_SLIDER", "The galactic game is not part of this build yet."),
    refused_option("IDC_SCROLL_SPEED_SLIDER", "Camera scroll speed is not applied yet."),
    refused_option("IDC_ABSOLUTE_SCROLLING", reason_mouse),
    refused_option("IDC_RIGHT_BUTTON_SCROLL", reason_mouse),
    refused_option("IDC_BOTH_MOUSE_BUTTONS_ROTATE", reason_mouse),
    refused_option("IDC_ALTERNATE_MOUSE_CONTROLS", reason_mouse),
    refused_option("IDC_DELAY_TOOLTIPS", "Tooltip timing is not selectable yet."),
    refused_option("IDC_AUTORESOLVE", "Auto-resolve belongs to the galactic game, which is not built yet."),
    refused_option("IDC_SIMPLIFIED_FACTION_COLORS", "Faction colours are not selectable yet."),
    refused_option("IDC_ABILITIES_DEFAULT_TO_AUTOFIRE", "Ability autofire is not selectable yet."),
    refused_option("IDC_RADIO_EASY", "Difficulty is chosen per game."),
    refused_option("IDC_RADIO_MEDIUM", "Difficulty is chosen per game."),
    refused_option("IDC_RADIO_HARD", "Difficulty is chosen per game."),
    // Network (ST-N1).
    refused_option("IDC_ALLOW_NAT_NEGOTIATION", reason_online),
    refused_option("IDC_NETWORK_PORT_NUMBER", reason_online),
    refused_option("IDC_CHECK_USE_DEFAULT_PORT", reason_online),
    refused_option("IDC_ALLOW_WINDOWS_FIREWALL_BYPASS", reason_online),
    refused_option("IDC_ALLOW_UPNP", reason_online),
    refused_option("IDC_NETWORK_QUICKMATCH_DEFAULT", reason_online),
    refused_option("IDC_INTERNET_CONNECTION_SPEED_COMBO", reason_online),
    refused_option("IDC_INTERNET_PREFERRED_LOCALE", reason_online),
    refused_option("IDC_INTERNET_PREFERRED_QUICKMATCH_FACTION", reason_online),
    refused_option("IDC_REFRESH_INFO_BUTTON", reason_online),
    refused_option("IDC_UPNP_DETAILS_BUTTON", reason_online),
    // Keyboard (ST-K1): the restore button; the edit boxes share one status below.
    refused_option("IDC_RESTORE_DEFAULTS", reason_keys),
};

constexpr OptionStatus key_edit_status = refused_option("IDC_EDIT_", reason_keys);

struct KeyFact final {
    std::string_view control;
    std::string_view key;
};

// The keys the project's battle input binds to a retail command (OR-01, G-1, overview).
constexpr KeyFact key_facts[] = {
    {"IDC_EDIT_WAYPOINT", "W"},
    {"IDC_EDIT_GUARD", "G"},
    {"IDC_EDIT_ATTACK", "A"},
    {"IDC_EDIT_MOVE", "M"},
    {"IDC_EDIT_STOP", "S"},
    {"IDC_EDIT_ATTACK_MOVE", "T"},
    {"IDC_EDIT_SELECT_ALL", "Ctrl+A"},
    {"IDC_EDIT_TACTICAL_OVERVIEW", "Insert"},
    {"IDC_EDIT_GROUP_0_SELECT", "0"},
    {"IDC_EDIT_GROUP_1_SELECT", "1"},
    {"IDC_EDIT_GROUP_2_SELECT", "2"},
    {"IDC_EDIT_GROUP_3_SELECT", "3"},
    {"IDC_EDIT_GROUP_4_SELECT", "4"},
    {"IDC_EDIT_GROUP_5_SELECT", "5"},
    {"IDC_EDIT_GROUP_6_SELECT", "6"},
    {"IDC_EDIT_GROUP_7_SELECT", "7"},
    {"IDC_EDIT_GROUP_8_SELECT", "8"},
    {"IDC_EDIT_GROUP_9_SELECT", "9"},
};

} // namespace

// All other bare defaults in the sourced map (ST-K3), including interface and
// ability keys. Validate the preset before any event can be dispatched.
static_assert([] {
    constexpr std::string_view occupied = "FGMERTBKCIP";
    for (std::size_t i = 0; i < camera_key_moves.size(); ++i) {
        const char key = camera_key_moves[i].wasd;
        if (occupied.find(key) != std::string_view::npos
            || std::string_view("WASD").find(key) != std::string_view::npos) return false;
        for (std::size_t j = 0; j < i; ++j) if (key == camera_key_moves[j].wasd) return false;
    }
    return true;
}());

char camera_command_key(const char physical, const bool camera_wasd) noexcept {
    if (!camera_wasd) return physical;
    for (const auto& move : camera_key_moves) if (physical == move.wasd) return move.retail;
    for (const auto& move : camera_key_moves) if (physical == move.retail) return '\0';
    return physical;
}

std::string_view project_key_for(const std::string_view control, const bool camera_wasd) noexcept {
    if (camera_wasd) {
        if (control == "IDC_EDIT_WAYPOINT") return "U";
        if (control == "IDC_EDIT_ATTACK") return "X";
        if (control == "IDC_EDIT_STOP") return "V";
    }
    return project_key_for(control);
}

std::optional<core::Diagnostic> validate_online_settings(const NetworkSettings& settings, const bool require_service) {
    const auto failure = [](const char* code, const char* message) {
        core::Diagnostic diagnostic;
        diagnostic.code = code; diagnostic.message = message;
        return std::optional<core::Diagnostic>{std::move(diagnostic)};
    };
    if (settings.online_host.empty()) {
        if (require_service) return failure("EAWR-NET-1745", "Set the online service in Network Options first.");
        // The project default is deliberately unconfigured.
    } else if (!valid_service_host(settings.online_host)) {
        return failure("EAWR-NET-1746", "Enter a service host name or address without a URL or port.");
    }
    if (settings.online_port < 1 || settings.online_port > 65535)
        return failure("EAWR-NET-1747", "Enter an online service port from 1 to 65535.");
    if (settings.online_trust != "pin" && settings.online_trust != "ca-file" && settings.online_trust != "system")
        return failure("EAWR-NET-1748", "Choose system, certificate pin or CA file trust.");
    if (settings.online_trust == "system" && !settings.online_host.empty()
        && (settings.online_host.find(':') != std::string::npos || std::all_of(settings.online_host.begin(), settings.online_host.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || c == '.'; })))
        return failure("EAWR-NET-1754", "System certificate trust needs the service's DNS host name. Use a pin or CA file for an address.");
    if (settings.online_trust == "pin" && (!settings.online_host.empty() || !settings.online_pin.empty())
        && (settings.online_pin.size() != 64 || !std::all_of(settings.online_pin.begin(), settings.online_pin.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }))) return failure("EAWR-NET-1749", "Enter the server certificate's 64-digit SHA-256 pin.");
    if (settings.online_ca_file.size() > 4096 || std::any_of(settings.online_ca_file.begin(), settings.online_ca_file.end(), [](unsigned char c) { return c < 32 || c == 127; })
        || (settings.online_trust == "ca-file" && !settings.online_host.empty() && settings.online_ca_file.empty()))
        return failure("EAWR-NET-1750", "Choose a CA certificate file for this service.");
    return std::nullopt;
}

double GameplaySettings::scroll_speed() const noexcept {
    const int stop = std::clamp(scroll_stop, 0, scroll_stop_count - 1);
    return static_cast<double>(stop) * scroll_speed_maximum / static_cast<double>(scroll_stop_count - 1);
}

std::vector<std::string> clamp_settings(PlayerSettings& settings) {
    std::vector<std::string> repaired;
    for (const IntField& field : fields) {
        int value = field.get(settings);
        if (clamp_field(value, field.minimum, field.maximum)) {
            field.set(settings, value);
            repaired.emplace_back(field.key);
        }
    }
    for (std::size_t slot = 0; slot < detail_slot_count; ++slot) {
        if (clamp_field(settings.video.detail[slot], detail_automatic, detail_slot_maximum)) {
            repaired.push_back(detail_key(slot));
        }
    }
    if (static_cast<int>(settings.video.render_profile) > static_cast<int>(RenderProfileSetting::enhanced)) {
        settings.video.render_profile = RenderProfileSetting::automatic;
        repaired.emplace_back("video.render_profile");
    }
    for (auto it = settings.keys.begin(); it != settings.keys.end();) {
        if (!valid_command_name(it->first) || !valid_key_text(it->second)) {
            repaired.push_back("key." + it->first);
            it = settings.keys.erase(it);
        } else {
            ++it;
        }
    }
    return repaired;
}

std::string serialize_settings(const PlayerSettings& settings) {
    std::string text = "eawr-settings " + std::to_string(settings_version) + "\n";
    for (const IntField& field : fields) {
        text += std::string(field.key) + "=" + std::to_string(field.get(settings)) + "\n";
    }
    for (std::size_t slot = 0; slot < detail_slot_count; ++slot) {
        text += detail_key(slot) + "=" + std::to_string(settings.video.detail[slot]) + "\n";
    }
    text += "video.render_profile=" + std::string(profile_name(settings.video.render_profile)) + "\n";
    // Refuse line injection even when the caller has not validated a draft yet.
    const auto line_value = [](const std::string& value) {
        return std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; }) ? value : std::string{};
    };
    text += "network.online_host=" + line_value(settings.network.online_host) + "\n";
    text += "network.online_port=" + std::to_string(settings.network.online_port) + "\n";
    text += "network.online_trust=" + line_value(settings.network.online_trust) + "\n";
    text += "network.online_pin=" + line_value(settings.network.online_pin) + "\n";
    text += "network.online_ca_file=" + line_value(settings.network.online_ca_file) + "\n";
    text += "network.online_relay_only=" + std::to_string(settings.network.online_relay_only) + "\n";
    for (const auto& [command, key] : settings.keys) {
        if (valid_command_name(command) && valid_key_text(key)) text += "key." + command + "=" + key + "\n";
    }
    return text;
}

SettingsLoad parse_settings(const std::string_view text) {
    SettingsLoad load;
    std::size_t position = 0;
    bool header = false;
    while (position <= text.size()) {
        const std::size_t end = std::min(text.find('\n', position), text.size());
        const std::string_view line = trim(text.substr(position, end - position));
        position = end + 1;
        if (line.empty() || line.front() == '#') continue;
        if (!header) {
            constexpr std::string_view prefix = "eawr-settings ";
            const auto version = line.substr(0, prefix.size()) == prefix
                ? parse_int(trim(line.substr(prefix.size()))) : std::nullopt;
            if (!version) {
                load.defaulted = true;
                load.notes.emplace_back("not a settings file; defaults used");
                return load;
            }
            if (static_cast<std::uint32_t>(*version) != settings_version) {
                load.defaulted = true;
                load.notes.push_back("settings version " + std::to_string(*version) + " is not "
                                     + std::to_string(settings_version) + "; defaults used");
                return load;
            }
            header = true;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            load.notes.push_back("line without a value ignored: " + std::string(line.substr(0, 40)));
            continue;
        }
        const std::string key(trim(line.substr(0, equals)));
        const std::string_view value = trim(line.substr(equals + 1));
        if (key.rfind("network.online_", 0) == 0) {
            auto& network = load.settings.network;
            if (key == "network.online_host") network.online_host = value;
            else if (key == "network.online_trust") network.online_trust = value;
            else if (key == "network.online_pin") network.online_pin = value;
            else if (key == "network.online_ca_file") network.online_ca_file = value;
            else if (key == "network.online_port") network.online_port = parse_int(value).value_or(0);
            else if (key == "network.online_relay_only") {
                if (value == "0" || value == "1") network.online_relay_only = value == "1";
                else load.notes.emplace_back("EAWR-NET-1751: Choose whether online games require the relay.");
            } else load.notes.emplace_back("unknown online setting ignored");
            continue;
        }
        if (key.rfind("key.", 0) == 0) {
            const std::string command = key.substr(4);
            if (valid_command_name(command) && valid_key_text(value)) load.settings.keys[command] = std::string(value);
            else load.notes.push_back("bad key binding ignored: " + key);
            continue;
        }
        if (key == "video.render_profile") {
            if (const auto profile = parse_profile(value)) load.settings.video.render_profile = *profile;
            else load.notes.push_back("bad value for " + key + " ignored");
            continue;
        }
        const auto number = parse_int(value);
        if (key.rfind("video.detail.", 0) == 0) {
            const auto slot = parse_int(std::string_view(key).substr(13));
            if (slot && *slot >= 0 && *slot < static_cast<int>(detail_slot_count) && number) {
                load.settings.video.detail[static_cast<std::size_t>(*slot)] = *number;
            } else {
                load.notes.push_back("bad detail value ignored: " + key);
            }
            continue;
        }
        const IntField* field = nullptr;
        for (const IntField& candidate : fields) {
            if (candidate.key == key) field = &candidate;
        }
        if (field == nullptr) {
            load.notes.push_back("unknown setting ignored: " + key);
        } else if (!number) {
            load.notes.push_back("bad value for " + key + " ignored");
        } else {
            field->set(load.settings, *number);
        }
    }
    if (!header) {
        load.defaulted = true;
        load.notes.emplace_back("empty settings file; defaults used");
        return load;
    }
    for (const std::string& repaired : clamp_settings(load.settings)) {
        load.notes.push_back("value out of range clamped: " + repaired);
    }
    if (const auto error = validate_online_settings(load.settings.network)) load.notes.push_back(core::format_diagnostic(*error));
    return load;
}

std::vector<DisplaySize> resolution_choices(const DisplaySize screen, const std::span<const DisplaySize> extras) {
    constexpr DisplaySize common[] = {
        {640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 720},  {1280, 800},  {1280, 960},
        {1280, 1024}, {1366, 768},  {1440, 900},  {1600, 900},  {1600, 1200}, {1680, 1050}, {1920, 1080},
        {1920, 1200}, {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160},
    };
    std::vector<DisplaySize> sizes;
    const auto usable = [](const DisplaySize& size) {
        return size.width >= display_size_minimum && size.height >= display_size_minimum
            && size.width <= display_size_maximum && size.height <= display_size_maximum;
    };
    for (const DisplaySize& size : common) {
        if (size.width <= screen.width && size.height <= screen.height) sizes.push_back(size);
    }
    if (usable(screen)) sizes.push_back(screen);
    for (const DisplaySize& size : extras) {
        if (usable(size)) sizes.push_back(size);
    }
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    return sizes;
}

std::span<const OptionStatus> option_statuses() noexcept { return statuses; }

const OptionStatus* find_option_status(const std::string_view control) noexcept {
    for (const OptionStatus& status : statuses) {
        if (status.control == control) return &status;
    }
    if (control.rfind("IDC_EDIT_", 0) == 0) return &key_edit_status;
    return nullptr;
}

std::string_view project_key_for(const std::string_view edit_control) noexcept {
    for (const KeyFact& fact : key_facts) {
        if (fact.control == edit_control) return fact.key;
    }
    return {};
}

} // namespace eawr::presentation::ui
