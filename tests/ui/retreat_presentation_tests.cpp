// The space retreat cues, countdown line and colour (docs/behaviour/gc-retreat-presentation.md, RP-01..RP-08).

#include "eawr/presentation/ui/retreat_presentation.hpp"
#include "ui_test_support.hpp"

#include <iostream>
#include <string>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
namespace tactical = sim::tactical;

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

ui::RetreatFactionPresentation rebel() {
    ui::RetreatFactionPresentation faction;
    faction.begin = "RHD_Retreat_Started_Space"; faction.enemy_begin = "Unit_Enemy_Retreat_Rebel_Soldier";
    faction.cancel = "RHD_Retreat_Cancelled"; faction.not_allowed = "RHD_Retreat_Not_Allowed";
    faction.reason = {"RHD_Retreat_Interdictor", "", ""};
    faction.countdown_text_id = "TEXT_RETREAT_SPACE_REBEL_COUNTDOWN";
    return faction;
}
ui::RetreatFactionPresentation empire() {
    ui::RetreatFactionPresentation faction = rebel();
    faction.begin = "EHD_Retreat_Started_Space"; faction.enemy_begin.clear(); faction.cancel = "EHD_Retreat_Cancelled";
    faction.not_allowed = "EHD_Retreat_Not_Allowed";
    faction.reason = {"EHD_Retreat_Interdictor", "Unit_Retreat_Tarkin", ""};
    return faction;
}

void test_start_cues() {
    expect(ui::retreat_start_cue(true, false, rebel(), "") == "RHD_Retreat_Started_Space", "RP-02: the retreating player hears the faction begin cue");
    expect(ui::retreat_start_cue(true, false, rebel(), "Type_Retreat_Start") == "Type_Retreat_Start",
        "WR-44: the local type cue takes precedence over the faction cue");
    expect(ui::retreat_start_cue(false, true, rebel(), "Type_Retreat_Start") == "Unit_Enemy_Retreat_Rebel_Soldier",
        "RP-02: an enemy hears the enemy cue of its OWN faction, never the type cue");
    expect(ui::retreat_start_cue(false, true, empire(), "").empty(), "RP-02: an authored absent enemy cue plays nothing");
    expect(ui::retreat_start_cue(false, false, rebel(), "").empty(), "RP-02: an ally hears nothing");
}

void test_cancel_cues() {
    expect(ui::retreat_cancel_cue(true, rebel(), "Type_Retreat_Cancel") == "Type_Retreat_Cancel", "RP-03: own cancel prefers the type cue");
    expect(ui::retreat_cancel_cue(true, rebel(), "") == "RHD_Retreat_Cancelled", "RP-03: own cancel falls back to the faction cue");
    expect(ui::retreat_cancel_cue(false, empire(), "Type_Retreat_Cancel") == "EHD_Retreat_Cancelled",
        "RP-03: any other cancel plays the retreating faction's cue");
}

void test_refusal_cues() {
    using tactical::RetreatRefusal;
    expect(ui::retreat_refusal_cue(RetreatRefusal::lockout, empire()) == "EHD_Retreat_Not_Allowed", "RP-05: lockout plays the generic cue");
    expect(ui::retreat_refusal_cue(RetreatRefusal::prevention, empire()) == "Unit_Retreat_Tarkin", "RP-05: hero prevention plays reason 2");
    expect(ui::retreat_refusal_cue(RetreatRefusal::prevention, rebel()).empty(),
        "RP-05: a blank reason cue replaces the generic cue and plays nothing");
    expect(ui::retreat_refusal_cue(RetreatRefusal::superweapon, empire()).empty(), "RP-05: superweapon reason 3 is blank in stock data");
    expect(ui::retreat_refusal_cue(RetreatRefusal::wrong_mode, empire()).empty(), "RP-05: gates that never reach the command bar are silent");
}

void test_countdown() {
    tactical::RetreatState state; state.phase = tactical::RetreatPhase::countdown; state.deadline = 400;
    expect(ui::retreat_countdown_visible(state, false) && !ui::retreat_countdown_visible(state, true),
        "RP-06: hidden while a cinematic plays");
    expect(!ui::retreat_countdown_visible(tactical::RetreatState{}, false), "RP-06: hidden when idle");
    expect(ui::retreat_countdown_line("RETREATING", state, 100, 30) == "RETREATING 010.000", "RP-07: 300 frames at 30 FPS");
    expect(ui::retreat_countdown_line("RETREATING", state, 399, 30) == "RETREATING 000.033", "RP-07: one frame is 33 thousandths");
    expect(ui::retreat_countdown_line("T", state, 400, 30) == "T 000.000", "RP-07: expiry reads zero");
    state.phase = tactical::RetreatPhase::escaping;
    expect(ui::retreat_countdown_line("T", state, 100, 30) == "T 000.000", "RP-07: the escape phase reads zero");
    tactical::RetreatState long_countdown;
    long_countdown.phase = tactical::RetreatPhase::countdown; long_countdown.deadline = 30 * 125 + 10;
    expect(ui::retreat_countdown_line("T", long_countdown, 10, 30) == "T 125.000", "RP-07: seconds past 99 keep three digits");
}

void test_colour() {
    expect(rebel().colour == (std::array<std::uint8_t, 4>{255, 255, 255, 255}), "RP-08: the authored colour is carried per faction");
}
} // namespace

int main() {
    test_start_cues();
    test_cancel_cues();
    test_refusal_cues();
    test_countdown();
    test_colour();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " retreat presentation check(s) failed\n";
        return 1;
    }
    std::cout << "ui retreat presentation passed\n";
    return 0;
}
