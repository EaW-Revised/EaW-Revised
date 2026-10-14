#include "eawr/presentation/ui/player_profile.hpp"

#include "eawr/presentation/ui/text_file.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <system_error>

namespace eawr::presentation::ui {
namespace {

constexpr std::size_t map_name_max_chars = 128;
constexpr std::size_t faction_name_max_chars = 64;
constexpr std::size_t tutorial_name_max_chars = 64;
constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";

[[nodiscard]] std::optional<int> parse_int(const std::string_view text) {
    int value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
    return value;
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

// The length of the well-formed UTF-8 sequence at the start of `text` and its code point;
// length 0 for a stray, truncated, overlong or out-of-range sequence.
struct Decoded final {
    std::size_t length{};
    char32_t code_point{};
};

[[nodiscard]] Decoded decode(const std::string_view text) noexcept {
    const auto byte = [&](const std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const unsigned char lead = byte(0);
    if (lead < 0x80) return {1, lead};
    std::size_t length = 0;
    char32_t code = 0;
    char32_t minimum = 0;
    if ((lead & 0xE0) == 0xC0) { length = 2; code = lead & 0x1F; minimum = 0x80; }
    else if ((lead & 0xF0) == 0xE0) { length = 3; code = lead & 0x0F; minimum = 0x800; }
    else if ((lead & 0xF8) == 0xF0) { length = 4; code = lead & 0x07; minimum = 0x10000; }
    else return {};
    if (text.size() < length) return {};
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(i) & 0xC0) != 0x80) return {};
        code = (code << 6) | (byte(i) & 0x3F);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return {};
    return {length, code};
}

// Valid UTF-8 only, no control characters (C0, DEL, C1), outer spaces trimmed, at most
// `max_chars` characters. Interior text is kept as is.
[[nodiscard]] std::string clean_text(const std::string_view text, const std::size_t max_chars) {
    std::string out;
    std::size_t chars = 0;
    std::size_t position = 0;
    while (position < text.size() && chars < max_chars) {
        const Decoded decoded = decode(text.substr(position));
        if (decoded.length == 0) {
            ++position;
            continue;
        }
        const std::string_view piece = text.substr(position, decoded.length);
        position += decoded.length;
        const char32_t code = decoded.code_point;
        if (code < 0x20 || (code >= 0x7F && code <= 0x9F)) continue;
        out.append(piece);
        ++chars;
    }
    const std::size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    out.erase(0, first);
    out.erase(out.find_last_not_of(' ') + 1);
    return out;
}

bool clamp_int(int& value, const int minimum, const int maximum) noexcept {
    const int clamped = std::clamp(value, minimum, maximum);
    const bool changed = clamped != value;
    value = clamped;
    return changed;
}

bool clean_field(std::string& value, const std::size_t max_chars) {
    std::string cleaned = clean_text(value, max_chars);
    const bool changed = cleaned != value;
    value = std::move(cleaned);
    return changed;
}

void append(std::string& text, const std::string_view key, const std::string_view value) {
    text.append(key).append("=").append(value).append("\n");
}

void append(std::string& text, const std::string_view key, const int value) {
    append(text, key, std::to_string(value));
}

void write_side(std::string& text, const std::string_view prefix, const SkirmishSideChoice& side) {
    const std::string base(prefix);
    append(text, base + ".map", clean_text(side.map, map_name_max_chars));
    append(text, base + ".faction", clean_text(side.faction, faction_name_max_chars));
    append(text, base + ".colour", side.colour);
    append(text, base + ".team", side.team);
}

void clamp_side(SkirmishSideChoice& side, const std::string_view prefix, std::vector<std::string>& repaired) {
    const std::string base(prefix);
    const auto note = [&](const char* field) { repaired.push_back(base + "." + field); };
    if (clean_field(side.map, map_name_max_chars)) note("map");
    if (clean_field(side.faction, faction_name_max_chars)) note("faction");
    if (clamp_int(side.colour, 0, colour_index_maximum)) note("colour");
    if (clamp_int(side.team, 0, team_index_maximum)) note("team");
}

// A side field's text key, within "skirmish.space." or "skirmish.land.".
bool set_side_field(SkirmishSideChoice& side, const std::string_view field, const std::string_view value,
                    std::vector<std::string>& notes, const std::string_view key) {
    if (field == "map") side.map = std::string(value);
    else if (field == "faction") side.faction = std::string(value);
    else if (field == "colour" || field == "team") {
        const auto number = parse_int(value);
        if (!number) {
            notes.push_back("bad value for " + std::string(key) + " ignored");
            return true;
        }
        (field == "colour" ? side.colour : side.team) = *number;
    } else {
        return false;
    }
    return true;
}

} // namespace

bool PlayerProfile::tutorial_played(const std::string_view name) const {
    return std::find(tutorials_played.begin(), tutorials_played.end(), name) != tutorials_played.end();
}

PlayerProfile default_profile() { return {}; }

std::string sanitize_name(const std::string_view text) { return clean_text(text, player_name_max_chars); }

bool set_player_name(PlayerProfile& profile, const std::string_view name) {
    std::string cleaned = sanitize_name(name);
    if (cleaned.empty()) return false;
    profile.player_name = std::move(cleaned);
    return true;
}

void set_network_name(PlayerProfile& profile, const std::string_view name) {
    profile.network_name = sanitize_name(name);
}

void set_lobby_colour(PlayerProfile& profile, const int colour) {
    profile.lobby_colour = std::clamp(colour, 0, colour_index_maximum);
}

std::string effective_player_name(const PlayerProfile& profile, const std::string_view platform_name) {
    if (!profile.player_name.empty()) return profile.player_name;
    std::string platform = sanitize_name(platform_name);
    return platform.empty() ? std::string(fallback_player_name) : platform;
}

std::string effective_network_name(const PlayerProfile& profile, const std::string_view platform_name) {
    if (!profile.network_name.empty()) return profile.network_name;
    return effective_player_name(profile, platform_name);
}

void record_skirmish_choices(PlayerProfile& profile, const SkirmishChoices& choices) {
    profile.skirmish = choices;
    PlayerProfile probe;
    probe.skirmish = profile.skirmish;
    clamp_profile(probe);
    profile.skirmish = std::move(probe.skirmish);
}

void mark_tutorial_played(PlayerProfile& profile, const std::string_view name) {
    const std::string cleaned = clean_text(name, tutorial_name_max_chars);
    if (cleaned.empty() || profile.tutorial_played(cleaned)) return;
    if (profile.tutorials_played.size() >= tutorials_played_max) return;
    profile.tutorials_played.push_back(cleaned);
}

std::vector<std::string> clamp_profile(PlayerProfile& profile) {
    std::vector<std::string> repaired;
    if (clean_field(profile.player_name, player_name_max_chars)) repaired.emplace_back("player.name");
    if (clean_field(profile.network_name, player_name_max_chars)) repaired.emplace_back("player.network_name");
    if (clamp_int(profile.lobby_colour, 0, colour_index_maximum)) repaired.emplace_back("player.lobby_colour");
    SkirmishChoices& skirmish = profile.skirmish;
    clamp_side(skirmish.space, "skirmish.space", repaired);
    clamp_side(skirmish.land, "skirmish.land", repaired);
    if (clean_field(skirmish.scenario_map, map_name_max_chars)) repaired.emplace_back("skirmish.scenario_map");
    if (clean_field(skirmish.game_name, player_name_max_chars)) repaired.emplace_back("skirmish.game_name");
    if (clamp_int(skirmish.game_type, 0, game_type_maximum)) repaired.emplace_back("skirmish.game_type");
    if (skirmish.credits < credits_authored || skirmish.credits > credits_maximum) {
        skirmish.credits = std::clamp(skirmish.credits, credits_authored, credits_maximum);
        repaired.emplace_back("skirmish.credits");
    }
    if (clamp_int(skirmish.timer_minutes, 0, timer_minutes_maximum)) repaired.emplace_back("skirmish.timer_minutes");

    std::vector<std::string> tutorials;
    for (const std::string& name : profile.tutorials_played) {
        std::string cleaned = clean_text(name, tutorial_name_max_chars);
        if (cleaned.empty() || std::find(tutorials.begin(), tutorials.end(), cleaned) != tutorials.end()) continue;
        if (tutorials.size() >= tutorials_played_max) break;
        tutorials.push_back(std::move(cleaned));
    }
    if (tutorials != profile.tutorials_played) {
        profile.tutorials_played = std::move(tutorials);
        repaired.emplace_back("tutorial.played");
    }
    return repaired;
}

std::string serialize_profile(const PlayerProfile& profile) {
    std::string text = "eawr-profile " + std::to_string(profile_version) + "\n";
    append(text, "player.name", clean_text(profile.player_name, player_name_max_chars));
    append(text, "player.lobby_colour", std::clamp(profile.lobby_colour, 0, colour_index_maximum));
    append(text, "player.network_name", clean_text(profile.network_name, player_name_max_chars));
    write_side(text, "skirmish.space", profile.skirmish.space);
    write_side(text, "skirmish.land", profile.skirmish.land);
    append(text, "skirmish.scenario_map", clean_text(profile.skirmish.scenario_map, map_name_max_chars));
    append(text, "skirmish.game_type", profile.skirmish.game_type);
    append(text, "skirmish.credits", profile.skirmish.credits);
    append(text, "skirmish.timer_minutes", profile.skirmish.timer_minutes);
    append(text, "skirmish.game_name", clean_text(profile.skirmish.game_name, player_name_max_chars));
    for (const std::string& name : profile.tutorials_played) {
        const std::string cleaned = clean_text(name, tutorial_name_max_chars);
        if (!cleaned.empty()) append(text, "tutorial.played", cleaned);
    }
    return text;
}

ProfileLoad parse_profile(std::string_view text) {
    ProfileLoad load;
    PlayerProfile& profile = load.profile;
    if (text.substr(0, utf8_bom.size()) == utf8_bom) text.remove_prefix(utf8_bom.size());
    std::size_t position = 0;
    bool header = false;
    while (position <= text.size()) {
        const std::size_t end = std::min(text.find('\n', position), text.size());
        const std::string_view line = trim(text.substr(position, end - position));
        position = end + 1;
        if (line.empty() || line.front() == '#') continue;
        if (!header) {
            constexpr std::string_view prefix = "eawr-profile ";
            const auto version = line.substr(0, prefix.size()) == prefix
                ? parse_int(trim(line.substr(prefix.size()))) : std::nullopt;
            if (!version) {
                load.defaulted = true;
                load.notes.emplace_back("not a profile file; defaults used");
                return load;
            }
            if (static_cast<std::uint32_t>(*version) != profile_version) {
                load.defaulted = true;
                load.notes.push_back("profile version " + std::to_string(*version) + " is not "
                                     + std::to_string(profile_version) + "; defaults used");
                return load;
            }
            header = true;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            load.notes.push_back("line without a value ignored: " + clean_text(line.substr(0, 40), 40));
            continue;
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        bool known = true;
        if (key == "player.name") profile.player_name = std::string(value);
        else if (key == "player.network_name") profile.network_name = std::string(value);
        else if (key == "player.lobby_colour") {
            if (const auto number = parse_int(value)) profile.lobby_colour = *number;
            else load.notes.push_back("bad value for " + std::string(key) + " ignored");
        } else if (key.rfind("skirmish.space.", 0) == 0) {
            known = set_side_field(profile.skirmish.space, key.substr(15), value, load.notes, key);
        } else if (key.rfind("skirmish.land.", 0) == 0) {
            known = set_side_field(profile.skirmish.land, key.substr(14), value, load.notes, key);
        } else if (key == "skirmish.scenario_map") profile.skirmish.scenario_map = std::string(value);
        else if (key == "skirmish.game_name") profile.skirmish.game_name = std::string(value);
        else if (key == "skirmish.game_type" || key == "skirmish.credits" || key == "skirmish.timer_minutes") {
            const auto number = parse_int(value);
            if (!number) load.notes.push_back("bad value for " + std::string(key) + " ignored");
            else if (key == "skirmish.game_type") profile.skirmish.game_type = *number;
            else if (key == "skirmish.credits") profile.skirmish.credits = *number;
            else profile.skirmish.timer_minutes = *number;
        } else if (key == "tutorial.played") profile.tutorials_played.emplace_back(value);
        else known = false;
        if (!known) load.notes.push_back("unknown profile key ignored: " + clean_text(key.substr(0, 40), 40));
    }
    if (!header) {
        load.defaulted = true;
        load.notes.emplace_back("empty profile file; defaults used");
        return load;
    }
    for (const std::string& repaired : clamp_profile(profile)) {
        load.notes.push_back("value repaired: " + repaired);
    }
    return load;
}

ProfileLoad load_profile_file(const platform::Path& file) {
    const std::optional<std::string> text = read_text_file(file);
    if (!text) {
        ProfileLoad missing;
        missing.defaulted = true;
        return missing;
    }
    return parse_profile(*text);
}

bool save_profile_file(const platform::Path& file, const PlayerProfile& profile, std::string& error) {
    return write_text_file_atomic(file, serialize_profile(profile), error);
}

} // namespace eawr::presentation::ui
