// #2927: the player settings record (docs/ui/ui-settings.md): FoC's defaults, the text form's
// round trip, tolerance of damaged files, clamping, and the table of what each retail
// control does today. The screen and the GPU case are the viewer's (test_options_screen.py).

#include "eawr/presentation/ui/settings.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>

namespace {
using namespace eawr;
namespace ui = presentation::ui;

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

bool has_note(const ui::SettingsLoad& load, const std::string& part) {
    return std::any_of(load.notes.begin(), load.notes.end(),
                       [&](const std::string& note) { return note.find(part) != std::string::npos; });
}


void test_online_settings() {
    ui::NetworkSettings network;
    expect(!ui::validate_online_settings(network), "empty project service can be saved");
    expect(ui::validate_online_settings(network, true)->code == "EAWR-NET-1745", "Online requires a configured service");
    network.online_host = "service.example"; network.online_pin = std::string(64, 'a');
    expect(!ui::validate_online_settings(network, true), "explicit service pin validates");
    ui::PlayerSettings stored; stored.network = network; stored.network.online_relay_only = true;
    expect(ui::parse_settings(ui::serialize_settings(stored)).settings == stored, "service and relay-only choice round trip");
    network.online_host = "https://secret.example:123";
    auto error = ui::validate_online_settings(network, true);
    expect(error && error->code == "EAWR-NET-1746" && error->message.find("secret") == std::string::npos, "host validation is curated and private");
    for (const char* invalid : {"service:27020", ":::1", "-bad.example", "bad..example"}) {
        network = stored.network; network.online_host = invalid;
        expect(ui::validate_online_settings(network)->code == "EAWR-NET-1746", "malformed host syntax is refused before network I/O");
    }
    network = stored.network; network.online_host = "::1";
    expect(!ui::validate_online_settings(network, true), "raw IPv6 service address validates");
    network = stored.network; network.online_port = 0;
    expect(ui::validate_online_settings(network)->code == "EAWR-NET-1747", "zero service port is refused, not clamped");
    network = stored.network; network.online_pin = "secret-pin";
    expect(ui::validate_online_settings(network)->code == "EAWR-NET-1749", "invalid pin cannot silently select another trust mode");
    network = stored.network; network.online_trust = "ca-file"; network.online_ca_file = "certificates/community.pem";
    expect(!ui::validate_online_settings(network, true), "explicit CA file is accepted");
    network.online_ca_file.clear();
    expect(ui::validate_online_settings(network)->code == "EAWR-NET-1750", "CA mode needs its trust file");
    network = stored.network; network.online_trust = "system";
    expect(!ui::validate_online_settings(network, true), "system trust accepts a DNS host");
    for (const char* literal : {"127.0.0.1", "::1"}) {
        network.online_host = literal;
        expect(ui::validate_online_settings(network, true)->code == "EAWR-NET-1754", "system trust refuses IP literals with a curated setting message");
    }
    network.online_host = "service.example"; stored.network = network;
    expect(ui::parse_settings(ui::serialize_settings(stored)).settings == stored, "system trust choice round trips");
    const auto malformed = ui::parse_settings("eawr-settings 1\nnetwork.online_host=secret.example\nnetwork.online_port=bad\nnetwork.online_pin=private\n");
    expect(has_note(malformed, "EAWR-NET-1747") && !has_note(malformed, "secret.example"), "damaged service remains invalid with a private typed note");
    stored.network.online_host = "service.example\nnetwork.allow_upnp=0";
    expect(ui::parse_settings(ui::serialize_settings(stored)).settings.network.allow_upnp, "service text cannot inject settings lines");
}

void test_defaults() {
    const ui::PlayerSettings defaults;
    // ST-A1: the owner-requested quieter master keeps the retail channel balance.
    expect(defaults.audio.master_percent == 37 && defaults.audio.sfx_percent == 75
               && defaults.audio.speech_percent == 75 && defaults.audio.music_percent == 75
               && defaults.audio.subtitles,
           "ST-A1: master defaults to 37 percent, channels 75 percent, subtitles on");
    // ST-V1: retail video defaults, with the owner-requested ST-V5 vsync-on default.
    expect(defaults.video.fullscreen_width == 800 && defaults.video.fullscreen_height == 600
               && defaults.video.window_width == 800 && defaults.video.window_height == 600
               && !defaults.video.windowed && defaults.video.vsync && defaults.video.hardware_cursor
               && defaults.video.gamma_permille == 1000 && defaults.video.detail_level == 2,
           "ST-V1: the video defaults");
    expect(std::all_of(defaults.video.detail.begin(), defaults.video.detail.end(),
                       [](const int value) { return value == ui::detail_automatic; }),
           "ST-V4: every advanced detail value follows the basic level");
    expect(defaults.video.render_profile == ui::RenderProfileSetting::automatic,
           "ST-P1: the render profile is the run's own choice");
    // ST-G1: both game speeds at the middle step, scroll speed 1.0 at stop 3, absolute scrolling on,
    // ability autofire on, everything else off.
    const ui::GameplaySettings& play = defaults.gameplay;
    expect(play.tactical_speed == 2 && play.strategic_speed == 2 && play.scroll_stop == 3
               && play.scroll_speed() == 1.0 && play.absolute_scrolling && play.abilities_autofire
               && !play.delay_tooltips && !play.right_button_scroll && !play.both_buttons_rotate
               && !play.alternate_mouse && !play.simplified_faction_colours && !play.ask_auto_resolve,
           "ST-G1: the gameplay defaults");
    // ST-N1: the Defaults button's values.
    const ui::NetworkSettings& net = defaults.network;
    expect(net.port == 1234 && net.uses_default_port() && net.allow_nat && net.allow_firewall && net.allow_upnp
               && net.quickmatch_default == 1 && net.connection_speed == 0,
           "ST-N1: the network defaults");
    expect(defaults.keys.empty(), "ST-K1: no key overrides");
    expect(ui::volume_gain(75) == 0.75 && ui::volume_gain(0) == 0.0 && ui::volume_gain(100) == 1.0
               && ui::volume_gain(-5) == 0.0 && ui::volume_gain(500) == 1.0,
           "a percent slider is a 0..1 gain, bounded");
    expect(ui::GameplaySettings{.scroll_stop = 0}.scroll_speed() == 0.0
               && ui::GameplaySettings{.scroll_stop = 9}.scroll_speed() == 3.0,
           "ST-G2: scroll speed spans 0..3 over ten stops");
}

void test_camera_keys() {
    ui::PlayerSettings settings;
    expect(!settings.camera_wasd, "ST-K3: retail arrows are the fresh default");
    for (const bool wasd : {false, true}) {
        settings.camera_wasd = wasd;
        settings.video.vsync = false;
        settings.audio.master_percent = 81;
        settings.keys["GUARD"] = "Ctrl+G";
        const auto loaded = ui::parse_settings(ui::serialize_settings(settings));
        expect(loaded.notes.empty() && loaded.settings == settings,
               "ST-K3: camera preset round trips with all stored profile choices");
    }
    const auto old = ui::parse_settings("eawr-settings 1\nvideo.vsync=0\naudio.master=81\n");
    expect(!old.settings.camera_wasd && !old.settings.video.vsync && old.settings.audio.master_percent == 81,
           "ST-K3: missing preset adds only its default");
    expect(ui::project_key_for("IDC_EDIT_ATTACK", true) == "X"
        && ui::project_key_for("IDC_EDIT_STOP", true) == "V"
        && ui::project_key_for("IDC_EDIT_WAYPOINT", true) == "U", "ST-K3: keyboard help uses moved keys");
    for (const auto& move : ui::camera_key_moves) {
        expect(ui::camera_command_key(move.retail, false) == move.retail
            && ui::camera_command_key(move.retail, true) == '\0'
            && ui::camera_command_key(move.wasd, true) == move.retail,
            "ST-K3: a bare physical chord belongs to exactly one camera or command action");
    }
    expect(ui::camera_command_key('Q', true) == 'Q' && ui::camera_command_key('Z', true) == 'Z',
           "ST-K3: layout letters are not additional physical pan positions");
}

void test_frame_pacing() {
    ui::VideoSettings video;
    video.vsync = false;
    expect(video.frame_rate_limit == 120, "ST-P2: fresh render cap is 120 FPS");
    expect(ui::frame_rate_cap(video, 144, false, true) == 120, "battle uses selected cap");
    expect(ui::frame_rate_cap(video, 144, true, true) == 60, "demo and menu use 60 FPS");
    expect(ui::frame_rate_cap(video, 144, false, false) == 30, "unfocused battle uses 30 FPS");
    video.frame_rate_limit = 30;
    expect(ui::frame_rate_cap(video, 144, true, true) == 30, "lower selected cap wins in menus");
    video.frame_rate_limit = 0;
    expect(ui::frame_rate_cap(video, 144, false, true) == 0, "Off allows uncapped focused battle");
    expect(ui::frame_rate_cap(video, 144, true, false) == 30, "Off still limits background menus");
    video.vsync = true;
    expect(ui::frame_rate_cap(video, 144, false, true) == 144, "vsync limits an otherwise uncapped battle");
    video.frame_rate_limit = 120;
    expect(ui::frame_rate_cap(video, 60, false, true) == 60, "vsync and explicit limit combine lower wins");
    video.vsync = false;
    video.frame_rate_limit = -1;
    expect(ui::frame_rate_cap(video, 165, false, true) == 165, "refresh selection follows the current display");
    expect(ui::frame_rate_cap(video, -1, false, true) == 60, "unknown refresh safely uses 60 FPS");
    video.frame_rate_limit = 1;
    expect(ui::frame_rate_cap(video, 144, false, true) == 30, "render floor allows the 30 Hz tick clock");
    for (int fps : {0, -1, 30, 60, 90, 120, 144, 165, 240, 360}) {
        ui::PlayerSettings stored;
        stored.video.frame_rate_limit = fps;
        const auto loaded = ui::parse_settings(ui::serialize_settings(stored));
        expect(loaded.notes.empty() && loaded.settings.video.frame_rate_limit == fps,
               "stored FPS choices survive loading without default replacement");
    }
    const auto old = ui::parse_settings("eawr-settings 1\nvideo.vsync=0\naudio.master=81\n");
    expect(!old.settings.video.vsync && old.settings.audio.master_percent == 81
           && old.settings.video.frame_rate_limit == 120, "missing FPS field adds the default and keeps stored settings");
}

void test_round_trip() {
    ui::PlayerSettings changed;
    changed.audio = {10, 20, 30, 40, false};
    changed.video.fullscreen_width = 1920;
    changed.video.fullscreen_height = 1080;
    changed.video.window_width = 1280;
    changed.video.window_height = 720;
    changed.video.windowed = true;
    changed.video.vsync = true;
    changed.video.detail_level = 3;
    changed.video.texture_mip_drop = 2;
    changed.video.gamma_permille = 1250;
    changed.video.widescreen = true;
    changed.video.hardware_cursor = false;
    changed.video.detail[4] = 2;
    changed.video.render_profile = ui::RenderProfileSetting::retail;
    changed.gameplay.tactical_speed = 4;
    changed.gameplay.strategic_speed = 0;
    changed.gameplay.scroll_stop = 9;
    changed.gameplay.absolute_scrolling = false;
    changed.gameplay.abilities_autofire = false;
    changed.gameplay.delay_tooltips = true;
    changed.network.port = 4000;
    changed.network.allow_upnp = false;
    changed.network.quickmatch_default = 2;
    changed.network.lobby_locale = 3;
    changed.keys["STOP"] = "X";

    const ui::SettingsLoad loaded = ui::parse_settings(ui::serialize_settings(changed));
    expect(!loaded.defaulted && loaded.notes.empty(), "a written file loads clean");
    expect(loaded.settings == changed, "the record round-trips exactly");
    expect(ui::serialize_settings(loaded.settings) == ui::serialize_settings(changed),
           "the text form is stable across a load");
    expect(ui::parse_settings(ui::serialize_settings(ui::PlayerSettings{})).settings == ui::PlayerSettings{},
           "the defaults round-trip");
}

void test_garbage() {
    // ST-S1: nothing a file holds can fail the load; a bad file is the defaults.
    for (const std::string_view text : {std::string_view{}, std::string_view{"\n\n"}, std::string_view{"hello"},
                                        std::string_view{"eawr-settings"}, std::string_view{"eawr-settings x"},
                                        std::string_view{"\xFF\xFE\x01 junk"}}) {
        const ui::SettingsLoad load = ui::parse_settings(text);
        expect(load.defaulted && load.settings == ui::PlayerSettings{} && !load.notes.empty(),
               "ST-S1: a file that is not a settings file loads as the defaults");
    }
    const ui::SettingsLoad other = ui::parse_settings("eawr-settings 99\naudio.master=5\n");
    expect(other.defaulted && other.settings.audio.master_percent == 37 && has_note(other, "version 99"),
           "ST-S2: another version loads as the defaults, with a note");

    const ui::SettingsLoad mixed = ui::parse_settings(
        "eawr-settings 1\r\n"
        "# a comment\r\n"
        "audio.master=abc\r\n"          // not a number: ignored, default kept
        "audio.sfx=12x\r\n"             // trailing text: ignored
        "audio.music=250\r\n"           // out of range: clamped
        "video.windowed=1\r\n"
        "video.fullscreen_width=-4\r\n" // below the minimum: clamped
        "gameplay.tactical_speed=\r\n"  // empty value
        "no equals sign\r\n"
        "nonsense.key=1\r\n"
        "video.detail.99=1\r\n"
        "video.detail.2=77\r\n"         // clamped to the sanity bound
        "video.render_profile=fancy\r\n"
        "key.STOP=X\r\n"
        "key.bad name=Y\r\n"
        "audio.speech=60\r\n");
    expect(!mixed.defaulted, "a damaged body is not a default file");
    expect(mixed.settings.audio.master_percent == 37, "a malformed number keeps the default");
    expect(mixed.settings.audio.sfx_percent == 75, "trailing text is a malformed number");
    expect(mixed.settings.audio.music_percent == 100, "an out-of-range volume is clamped");
    expect(mixed.settings.audio.speech_percent == 60, "valid lines after damage still load");
    expect(mixed.settings.video.windowed, "CRLF line ends load");
    expect(mixed.settings.video.fullscreen_width == ui::display_size_minimum, "a size below the minimum is clamped");
    expect(mixed.settings.gameplay.tactical_speed == 2, "an empty value keeps the default");
    expect(mixed.settings.video.detail[2] == 16, "a detail value is held to the sanity bound");
    expect(mixed.settings.video.render_profile == ui::RenderProfileSetting::automatic,
           "an unknown profile name keeps the default");
    expect(mixed.settings.keys.size() == 1 && mixed.settings.keys.at("STOP") == "X", "only a valid key line loads");
    expect(has_note(mixed, "unknown setting") && has_note(mixed, "bad value") && has_note(mixed, "clamped"),
           "every repair is noted");
}

void test_clamping() {
    ui::PlayerSettings wild;
    wild.audio = {-10, 1000, 101, -1, true};
    wild.video.window_width = 9'999'999;
    wild.video.window_height = 0;
    wild.video.detail_level = 9;
    wild.video.texture_mip_drop = 99;
    wild.video.detail[0] = -50;
    wild.gameplay.tactical_speed = 7;
    wild.gameplay.strategic_speed = -2;
    wild.gameplay.scroll_stop = 99;
    wild.network.port = 70000;
    wild.network.quickmatch_default = 5;
    wild.network.lobby_locale = -9;
    wild.keys["ok"] = "A";
    wild.keys["bad name"] = "A";
    wild.keys["EMPTY"] = "";
    const std::vector<std::string> repaired = ui::clamp_settings(wild);
    expect(repaired.size() >= 10, "every out-of-range field is reported");
    expect(wild.audio.master_percent == 0 && wild.audio.sfx_percent == 100 && wild.audio.speech_percent == 100
               && wild.audio.music_percent == 0,
           "volumes are held to 0..100");
    expect(wild.video.window_width == ui::display_size_maximum && wild.video.window_height == ui::display_size_minimum,
           "sizes are held to the display bounds");
    expect(wild.video.detail_level == 3 && wild.video.detail[0] == -1, "detail values are held");
    expect(wild.video.texture_mip_drop == 4, "GM-04: mip detail is held to the supported drop range");
    expect(wild.gameplay.tactical_speed == 4 && wild.gameplay.strategic_speed == 0 && wild.gameplay.scroll_stop == 9,
           "ST-G1: speeds and scroll stops are held to their steps");
    expect(wild.network.port == 65535 && wild.network.quickmatch_default == 2 && wild.network.lobby_locale == -1,
           "network values are held");
    expect(wild.keys.size() == 1 && wild.keys.count("ok") == 1, "invalid key bindings are dropped");
    expect(ui::clamp_settings(wild).empty(), "a clamped record is stable");
}

void test_option_status() {
    // ST-O1: every audio slider, the resolution list, windowed, vsync, the anti-aliasing slider
    // and the tactical speed are honoured; the rest says why not.
    for (const char* control : {"IDC_SLIDER_MASTER_VOLUME", "IDC_SLIDER_SOUND_EFFECTS_VOLUME", "IDC_SLIDER_DIALOG_VOLUME",
                                "IDC_SLIDER_MUSIC_VOLUME", "IDC_RESOLUTION_LIST", "IDC_WINDOWED_MODE",
                                "IDC_VSYNC_ENABLED", "IDC_ANTI_ALIAS_SLIDER", "IDC_GAME_SPEED_SLIDER"}) {
        const ui::OptionStatus* status = ui::find_option_status(control);
        expect(status != nullptr && status->honoured && status->reason.empty(),
               std::string("ST-O1: ") + control + " is honoured");
    }
    std::set<std::string_view> seen;
    for (const ui::OptionStatus& status : ui::option_statuses()) {
        expect(seen.insert(status.control).second, "one status per control: " + std::string(status.control));
        expect(status.honoured == status.reason.empty(),
               "a refused control says why and an honoured one does not: " + std::string(status.control));
    }
    const ui::OptionStatus* gamma = ui::find_option_status("IDC_GAMMA_SLIDER");
    expect(gamma != nullptr && !gamma->honoured, "an unbuilt option is refused");
    const ui::OptionStatus* key = ui::find_option_status("IDC_EDIT_SELECT_ALL");
    expect(key != nullptr && !key->honoured, "ST-K1: key edit boxes are read-only");
    expect(ui::find_option_status("IDC_OK") == nullptr && ui::find_option_status("IDC_STATIC") == nullptr,
           "buttons and labels have no status");
    expect(ui::project_key_for("IDC_EDIT_SELECT_ALL") == "Ctrl+A" && ui::project_key_for("IDC_EDIT_GROUP_7_SELECT") == "7"
               && ui::project_key_for("IDC_EDIT_STOP") == "S" && ui::project_key_for("IDC_EDIT_SPRINT").empty(),
           "ST-K2: the keyboard page shows the project's keys, blank where there is none");
}

void test_resolution_choices() {
    // ST-V2: the common sizes that fit, the screen's own, and the extras, each once, ascending.
    const ui::DisplaySize extras[] = {{1280, 720}, {1366, 700}, {100, 100}, {99999, 99999}};
    const std::vector<ui::DisplaySize> sizes = ui::resolution_choices({1920, 1080}, extras);
    expect(!sizes.empty() && std::is_sorted(sizes.begin(), sizes.end()), "ST-V2: the list is ascending");
    expect(std::adjacent_find(sizes.begin(), sizes.end()) == sizes.end(), "ST-V2: no size twice");
    expect(std::find(sizes.begin(), sizes.end(), ui::DisplaySize{1920, 1080}) != sizes.end()
               && std::find(sizes.begin(), sizes.end(), ui::DisplaySize{1280, 720}) != sizes.end()
               && std::find(sizes.begin(), sizes.end(), ui::DisplaySize{1366, 700}) != sizes.end(),
           "ST-V2: the screen's size and a usable extra are listed");
    expect(std::none_of(sizes.begin(), sizes.end(),
                        [](const ui::DisplaySize& size) { return size.width > 1920 || size.height > 1080
                                                              || size.width < ui::display_size_minimum; }),
           "ST-V2: nothing larger than the screen, nothing below the minimum, unusable extras dropped");
    expect(ui::resolution_choices({0, 0}, {}).empty(), "no screen and no extras is an empty list");
}

void test_session_values() {
    // The start-up apply and a battle read the saved record through one mapping: each volume
    // slider reaches its own bus and the tactical speed reaches the speed step.
    const ui::SessionValues defaults = ui::session_values(ui::PlayerSettings{});
    expect(defaults.master_gain == 0.37 && defaults.sfx_gain == 0.75 && defaults.speech_gain == 0.75
               && defaults.music_gain == 0.75 && defaults.speed_step == 2,
           "the defaults reach a battle as master 0.37, channels 0.75 and speed step 2");
    const ui::SettingsLoad existing = ui::parse_settings(
        "eawr-settings 1\naudio.master=75\naudio.sfx=80\naudio.speech=65\naudio.music=90\nvideo.vsync=0\n");
    expect(!existing.defaulted && existing.settings.audio.master_percent == 75
               && existing.settings.audio.sfx_percent == 80 && existing.settings.audio.speech_percent == 65
               && existing.settings.audio.music_percent == 90 && !existing.settings.video.vsync,
           "stored sliders and disabled vsync keep their values, including former defaults");
    const ui::SettingsLoad saved = ui::parse_settings(
        "eawr-settings 1\naudio.master=30\naudio.sfx=40\naudio.speech=55\naudio.music=100\n"
        "gameplay.tactical_speed=4\n");
    expect(!saved.defaulted, "the saved fixture is a settings file");
    const ui::SessionValues values = ui::session_values(saved.settings);
    expect(values.master_gain == 0.30 && values.sfx_gain == 0.40 && values.speech_gain == 0.55
               && values.music_gain == 1.0 && values.speed_step == 4,
           "saved volumes and speed reach their own bus and the speed step");
    const ui::SessionValues round_trip = ui::session_values(ui::parse_settings(ui::serialize_settings(saved.settings)).settings);
    expect(round_trip.master_gain == values.master_gain && round_trip.speed_step == values.speed_step,
           "the mapping survives a save and a load");
}

} // namespace

int main() {
    test_online_settings();
    test_defaults();
    test_camera_keys();
    test_frame_pacing();
    test_round_trip();
    test_garbage();
    test_clamping();
    test_option_status();
    test_resolution_choices();
    test_session_values();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " settings check(s) failed\n";
        return 1;
    }
    std::cout << "ui settings passed\n";
    return 0;
}
