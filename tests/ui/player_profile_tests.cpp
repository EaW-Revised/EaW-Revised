// #2935: the player profile record (docs/ui/player-profile.md): defaults, the text form's round
// trip, tolerance of damaged files, non-ASCII names and paths, and the write-back calls the
// skirmish setup and the LAN lobby make. Engine-free; the viewer's ProfileStore only picks the path.

#include "eawr/presentation/ui/player_profile.hpp"
#include "eawr/presentation/ui/text_file.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using namespace eawr;
namespace ui = presentation::ui;

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

bool has_note(const ui::ProfileLoad& load, const std::string& part) {
    return std::any_of(load.notes.begin(), load.notes.end(),
                       [&](const std::string& note) { return note.find(part) != std::string::npos; });
}

// "Jürgen Ωmega 提督" as UTF-8 bytes, so the test does not depend on the source encoding.
const std::string non_ascii_name = "J\xC3\xBCrgen \xCE\xA9mega \xE6\x8F\x90\xE7\x9D\xA3";

void test_defaults() {
    const ui::PlayerProfile profile = ui::default_profile();
    expect(profile.player_name.empty() && profile.network_name.empty() && profile.lobby_colour == 0,
           "PRF-01: no name stored, the first colour");
    expect(profile.skirmish.space.map.empty() && profile.skirmish.land.map.empty()
               && profile.skirmish.space.faction.empty() && profile.skirmish.space.colour == 0
               && profile.skirmish.space.team == 0,
           "PRF-03: no last map, faction 0, colour 0, team 0");
    expect(profile.skirmish.game_type == 1 && profile.skirmish.credits == -1 && profile.skirmish.timer_minutes == 0
               && profile.skirmish.game_name.empty(),
           "PRF-04: game type 1, authored credits, no timer");
    expect(!profile.tutorial_seen() && profile.tutorials_played.empty(), "PRF-05: no tutorial played");
    expect(ui::effective_player_name(profile, "WinUser") == "WinUser", "PRF-01: the platform user name is the default name");
    expect(ui::effective_player_name(profile, "") == "Player" && ui::effective_player_name(profile, " \t") == "Player",
           "PRF-01: a fixed name when the platform gives none");
    expect(ui::effective_network_name(profile, "WinUser") == "WinUser", "PRF-06: the lobby name follows the player name");
    // A fresh profile loads the same from a missing file.
    const ui::ProfileLoad missing = ui::load_profile_file(std::filesystem::temp_directory_path() / "eawr-no-such-profile.cfg");
    expect(missing.defaulted && missing.notes.empty() && missing.profile == profile,
           "ST-S1: a missing file is the defaults with no note");
}

ui::PlayerProfile changed_profile() {
    ui::PlayerProfile profile;
    expect(ui::set_player_name(profile, non_ascii_name), "a non-ASCII name is accepted");
    ui::set_network_name(profile, "LAN Commander");
    ui::set_lobby_colour(profile, 5);
    ui::SkirmishChoices choices;
    choices.space = {"Space Map One", "REBEL", 3, 1};
    choices.land = {"Land Map \xC3\x84", "UNDERWORLD", 7, 2};
    choices.scenario_map = "Tutorial_Space";
    choices.game_type = 2;
    choices.credits = 7500;
    choices.timer_minutes = 20;
    choices.game_name = "Friday game";
    ui::record_skirmish_choices(profile, choices);
    ui::mark_tutorial_played(profile, "Tutorial_One");
    ui::mark_tutorial_played(profile, "Tutorial_Two");
    return profile;
}

void test_round_trip() {
    const ui::PlayerProfile profile = changed_profile();
    const std::string text = ui::serialize_profile(profile);
    expect(text.rfind("eawr-profile 1\n", 0) == 0, "ST-S2: the first line carries the version");
    const ui::ProfileLoad load = ui::parse_profile(text);
    expect(!load.defaulted && load.notes.empty(), "a written profile loads clean");
    expect(load.profile == profile, "the profile round-trips exactly");
    expect(load.profile.player_name == non_ascii_name, "the non-ASCII name is kept byte for byte");
    expect(load.profile.tutorial_seen() && load.profile.tutorial_played("Tutorial_Two"), "the tutorials are kept");
    expect(ui::serialize_profile(load.profile) == text, "a second write is identical");
}

void test_file_with_non_ascii_path() {
    // A folder and file whose names are not ASCII: the store must not go through an ANSI step.
    const std::filesystem::path folder = std::filesystem::temp_directory_path()
        / std::filesystem::path(u8"eawr-profile-test-Jürgen-提督");
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    const std::filesystem::path file = folder / std::filesystem::path(u8"éawr-profile.cfg");
    const ui::PlayerProfile profile = changed_profile();
    std::string error;
    expect(ui::save_profile_file(file, profile, error), "the profile saves into a non-ASCII folder: " + error);
    expect(std::filesystem::exists(file) && !std::filesystem::exists(std::filesystem::path(file) += ".tmp"),
           "the file exists and no temporary is left");
    const ui::ProfileLoad load = ui::load_profile_file(file);
    expect(!load.defaulted && load.profile == profile, "the profile loads back from the non-ASCII path");
    // Saving again replaces the file.
    ui::PlayerProfile second = profile;
    ui::set_lobby_colour(second, 9);
    expect(ui::save_profile_file(file, second, error), "the profile saves over an existing file: " + error);
    expect(ui::load_profile_file(file).profile.lobby_colour == 9, "the second save replaced the first");
    std::filesystem::remove_all(folder, ignored);
}

void test_garbage() {
    expect(ui::parse_profile("").defaulted, "an empty file is the defaults");
    expect(ui::parse_profile("not a profile\nplayer.name=x\n").defaulted, "a foreign file is the defaults");
    expect(ui::parse_profile(std::string("\x00\x01\x02\xFF\xFE", 5)).defaulted, "binary junk is the defaults");
    const ui::ProfileLoad other = ui::parse_profile("eawr-profile 99\nplayer.name=x\n");
    expect(other.defaulted && has_note(other, "version"), "another version is the defaults, with a note");
    expect(other.profile == ui::default_profile(), "nothing of another version leaks in");

    const ui::ProfileLoad mixed = ui::parse_profile(
        "eawr-profile 1\r\n# comment\r\nplayer.name = Ann \r\nplayer.lobby_colour=abc\r\nskirmish.credits=\r\n"
        "no equals sign here\r\nunknown.key=1\r\nskirmish.space.team=zz\r\nskirmish.land.map=Dune\r\n");
    expect(!mixed.defaulted, "damaged lines do not default the file");
    expect(mixed.profile.player_name == "Ann" && mixed.profile.skirmish.land.map == "Dune",
           "good lines survive (CRLF, spaces around the value)");
    expect(mixed.profile.lobby_colour == 0 && mixed.profile.skirmish.credits == -1
               && mixed.profile.skirmish.space.team == 0,
           "bad numbers keep their defaults");
    expect(has_note(mixed, "player.lobby_colour") && has_note(mixed, "unknown profile key")
               && has_note(mixed, "without a value") && has_note(mixed, "skirmish.space.team"),
           "each dropped line has a note");

    const ui::ProfileLoad bom = ui::parse_profile("\xEF\xBB\xBF" "eawr-profile 1\nplayer.name=Bom\n");
    expect(!bom.defaulted && bom.profile.player_name == "Bom", "a UTF-8 byte order mark is skipped");
}

void test_clamping_and_names() {
    const ui::ProfileLoad load = ui::parse_profile(
        "eawr-profile 1\nplayer.lobby_colour=500\nskirmish.space.colour=-4\nskirmish.land.team=99\n"
        "skirmish.credits=99999999\nskirmish.timer_minutes=-5\nskirmish.game_type=77\n");
    expect(load.profile.lobby_colour == ui::colour_index_maximum && load.profile.skirmish.space.colour == 0
               && load.profile.skirmish.land.team == ui::team_index_maximum
               && load.profile.skirmish.credits == ui::credits_maximum && load.profile.skirmish.timer_minutes == 0
               && load.profile.skirmish.game_type == ui::game_type_maximum,
           "out-of-range numbers are clamped");
    expect(has_note(load, "skirmish.credits"), "a clamped value has a note");

    // Invalid UTF-8, control characters and over-long names.
    expect(ui::sanitize_name("  Bob\x01\x7F  ") == "Bob", "control characters and outer spaces are dropped");
    expect(ui::sanitize_name("A\xFF" "B\xC3") == "AB", "invalid UTF-8 bytes are dropped");
    expect(ui::sanitize_name("\xED\xA0\x80x") == "x", "a surrogate encoding is dropped");
    expect(ui::sanitize_name("\xC0\xAF" "x") == "x", "an overlong encoding is dropped");
    const std::string long_name(100, 'a');
    expect(ui::sanitize_name(long_name).size() == ui::player_name_max_chars, "a long name is cut");
    std::string wide;
    for (int i = 0; i < 40; ++i) wide += "\xE6\x8F\x90"; // 40 characters of three bytes each
    const std::string cut = ui::sanitize_name(wide);
    expect(cut.size() == ui::player_name_max_chars * 3, "a long non-ASCII name is cut on a character, not a byte");

    ui::PlayerProfile profile;
    expect(ui::set_player_name(profile, "Ann"), "a name is set");
    expect(!ui::set_player_name(profile, " \x01 ") && profile.player_name == "Ann",
           "a name with nothing left keeps the old one");
    ui::set_network_name(profile, "Net");
    expect(ui::effective_network_name(profile, "WinUser") == "Net", "the network name wins in the lobby");
    ui::set_network_name(profile, "");
    expect(ui::effective_network_name(profile, "WinUser") == "Ann", "an empty network name follows the player");
    ui::set_lobby_colour(profile, -3);
    expect(profile.lobby_colour == 0, "a colour below the list is the first colour");
}

void test_write_back() {
    ui::PlayerProfile profile;
    ui::SkirmishChoices first;
    first.space = {"Alpha", "EMPIRE", 2, 1};
    first.credits = 5000;
    ui::record_skirmish_choices(profile, first);
    expect(profile.skirmish.space.map == "Alpha" && profile.skirmish.space.faction == "EMPIRE"
               && profile.skirmish.credits == 5000,
           "Start in the skirmish setup records the choices");
    ui::SkirmishChoices bad;
    bad.land = {"  Beta \x02", "REBEL", 400, -1};
    bad.credits = -50;
    ui::record_skirmish_choices(profile, bad);
    expect(profile.skirmish.land.map == "Beta" && profile.skirmish.land.colour == ui::colour_index_maximum
               && profile.skirmish.land.team == 0 && profile.skirmish.credits == ui::credits_authored,
           "recorded choices are cleaned like a loaded file");
    expect(profile.skirmish.space.map.empty(), "a record replaces the earlier choices as a whole");

    ui::mark_tutorial_played(profile, "T1");
    ui::mark_tutorial_played(profile, "T1");
    ui::mark_tutorial_played(profile, "");
    expect(profile.tutorials_played.size() == 1 && profile.tutorial_seen(), "a tutorial is kept once");
    for (int i = 0; i < 200; ++i) ui::mark_tutorial_played(profile, "T" + std::to_string(i));
    expect(profile.tutorials_played.size() == ui::tutorials_played_max, "the tutorial list is bounded");
}

void test_failed_replace_keeps_old_file() {
    // The replace cannot succeed when the target is a non-empty folder: the old content must survive
    // and no temporary may stay behind (the writer never deletes the target first).
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "eawr-atomic-write-test";
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    const std::filesystem::path target = folder / "settings.cfg";
    std::filesystem::create_directories(target, ignored);
    { std::ofstream(target / "old.txt") << "old"; }
    std::string error;
    expect(!ui::write_text_file_atomic(target, "new", error) && !error.empty(), "a failed replace reports an error");
    expect(std::filesystem::exists(target / "old.txt"), "the old target survives a failed replace");
    expect(!std::filesystem::exists(std::filesystem::path(target) += ".tmp"), "the temporary is removed after a failed replace");
    std::filesystem::remove_all(folder, ignored);
}

} // namespace

int main() {
    test_failed_replace_keeps_old_file();
    test_defaults();
    test_round_trip();
    test_file_with_non_ascii_path();
    test_garbage();
    test_clamping_and_names();
    test_write_back();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " player profile check(s) failed\n";
        return 1;
    }
    std::cout << "ui player profile passed\n";
    return 0;
}
