// #459: the tactical time panel's model (docs/behaviour/tactical-time-controls.md TM-01 to TM-08,
// TP-04, cases TMC-01 to TMC-04). The pacing itself (no tick while paused, hashes unchanged) is
// tactical_live_session_contracts; the buttons in the viewer are test_tactical_hud.py and
// test_live_session.py.

#include "eawr/presentation/ui/battle_messages.hpp"
#include "eawr/presentation/ui/time_controls.hpp"
#include "ui_test_support.hpp"

#include <iostream>
#include <string>

namespace {
using namespace eawr;
namespace ui = presentation::ui;

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

void test_speed_steps() {
    const std::uint32_t rates[] = {10, 20, 30, 45, 60};
    for (std::uint32_t step = 0; step < 5; ++step) {
        const ui::TimeControls controls(step);
        expect(controls.target_rate() == rates[step], "TM-01: speed step " + std::to_string(step) + " targets its rate");
    }
    const ui::TimeControls defaults;
    expect(defaults.speed_step() == 2 && defaults.target_rate() == 30 && defaults.tick_factor() == 1.0,
           "TMC-01: the default step runs at the nominal 30");
    expect(ui::TimeControls(9).speed_step() == 4, "a step past 4 is clamped");
    expect(defaults.state() == ui::TimeState::play && defaults.track().empty(), "the panel starts at play");
}

void test_fast_forward() {
    ui::TimeControls controls(1);
    expect(controls.press_fast_forward(10) && controls.state() == ui::TimeState::fast_forward
               && controls.target_rate() == 120 && controls.tick_factor() == 4.0,
           "TMC-02: fast forward targets 120");
    expect(controls.press_fast_forward(20) && controls.state() == ui::TimeState::play && controls.target_rate() == 20,
           "TMC-02: a second press returns to the speed setting");
}

void test_pause() {
    ui::TimeControls controls;
    expect(controls.press_pause(5) && controls.paused() && controls.tick_factor() == 0.0, "TM-05: pause");
    expect(!controls.fast_forward_enabled() && controls.pause_enabled(), "TM-08: fast forward is disabled while paused");
    expect(!controls.press_fast_forward(6) && controls.paused(), "TMC-03: fast forward does nothing while paused");
    expect(controls.press_pause(7) && controls.state() == ui::TimeState::play, "TM-05: the second press plays");
    // TMC-04: pausing from fast forward ends it; playing returns to the setting, not to fast forward.
    controls.press_fast_forward(8);
    controls.press_pause(9);
    expect(controls.paused(), "TMC-04: pause from fast forward");
    expect(controls.resume(9) && controls.state() == ui::TimeState::play, "TM-09: resume plays");
    expect(controls.target_rate() == 30, "TMC-04: play returns to the speed setting");
    expect(!controls.resume(10), "resume does nothing while playing");
}

void test_end_and_track() {
    ui::TimeControls controls;
    controls.press_fast_forward(3);
    controls.press_pause(40);
    controls.press_pause(40);
    controls.end(250);
    expect(controls.ended() && !controls.running() && controls.tick_factor() == 0.0, "BEP-02: the end stops the ticks");
    expect(!controls.press_pause(251) && !controls.press_fast_forward(251) && !controls.resume(251),
           "the panel does nothing after the end");
    expect(!controls.pause_enabled() && !controls.fast_forward_enabled(), "both buttons are disabled after the end");
    const std::string csv = controls.track_csv();
    expect(csv == "tick,state,ticks_per_second\n3,fast_forward,120\n40,paused,0\n40,play,30\n250,ended,0\n",
           "TP-04: the time track: " + csv);
}

data::XmlNode tag(std::string name, std::string text) {
    data::XmlNode node;
    node.name = std::move(name);
    node.raw_text = std::move(text);
    return node;
}

// #453 BE-02, BE-03 and TM-09: the message keys, the looks from GameConstants and the placement.
void test_battle_messages() {
    data::XmlNode root;
    root.name = "GameConstants";
    root.children = {tag("Win_Message_Color", "223, 243, 255, 255"), tag("Lose_Message_Color", " 255, 244, 223, 255 "),
                     tag("Win_Lose_Message_Font", "EmpireAtWar-Bold"), tag("Win_Lose_Message_Font_Size", " 36 "),
                     tag("Win_Lose_Message_Font_Size", " 24 "), tag("Battle_Pending_Message_Color", "255, 32, 32, 240")};
    const ui::BattleMessageLooks looks = ui::battle_message_looks(root);
    expect(looks.diagnostics.empty(), "the FoC constants parse without warnings");
    expect(looks.point_size == 24, "the last Win_Lose_Message_Font_Size wins");
    expect(looks.win == data::ui::Rgba8{223, 243, 255, 255} && looks.lose == data::ui::Rgba8{255, 244, 223, 255}
               && looks.pending == data::ui::Rgba8{255, 32, 32, 240} && looks.font == "EmpireAtWar-Bold",
           "BE-03 and TM-09 looks");
    data::XmlNode broken;
    broken.children = {tag("Win_Message_Color", "223, 243, 255"), tag("Win_Lose_Message_Font_Size", "big")};
    const ui::BattleMessageLooks fallback = ui::battle_message_looks(broken);
    expect(fallback.win == data::ui::Rgba8{223, 243, 255, 255} && fallback.point_size == 24 && fallback.diagnostics.size() == 5,
           "missing or malformed constants keep the FoC values, one warning each");
    expect(ui::battle_message_key(ui::BattleResult::victory) == "TEXT_WIN_TACTICAL"
               && ui::battle_message_key(ui::BattleResult::defeat) == "TEXT_LOSE_TACTICAL",
           "BE-02 message keys");
    expect(ui::battle_end_title_key(ui::BattleResult::victory) == "TEXT_VICTORY"
               && ui::battle_end_title_key(ui::BattleResult::defeat) == "TEXT_DEFEAT",
           "BEP-03 panel titles");
    const ui::MessagePlacement at = ui::battle_message_placement(1280.0, 720.0, 400.0);
    expect(at.x == 440.0 && at.y == 288.0, "BE-03: centred, top at 0.4 of the height");
}

} // namespace

int main() {
    test_battle_messages();
    test_speed_steps();
    test_fast_forward();
    test_pause();
    test_end_and_track();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " time control check(s) failed\n";
        return 1;
    }
    std::cout << "ui time controls passed\n";
    return 0;
}
