// #459: the tactical time panel's model (docs/behaviour/tactical-time-controls.md TM-01 to TM-08,
// TP-04, cases TMC-01 to TMC-04). The pacing itself (no tick while paused, hashes unchanged) is
// tactical_live_session_contracts; the buttons in the viewer are test_tactical_hud.py and
// test_live_session.py.

#include "eawr/presentation/ui/battle_messages.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/ui/time_controls.hpp"
#include "ui_test_support.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>
#include <string>

// #5355 (review of #6272): count every allocation of this binary, so the shared speed notice can prove that an
// unchanged refresh allocates nothing. The array forms default to these.
namespace {
std::atomic<std::size_t> allocations{0};
} // namespace
void* operator new(const std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

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
    ui::TimeControls agreed;
    expect(agreed.set_speed_step(4, 23) && agreed.target_rate() == 60, "MP-06: observe agreed speed");
    expect(!agreed.set_speed_step(5, 24) && !agreed.set_speed_step(4, 24), "invalid and unchanged choices do nothing");
    expect(agreed.track().size() == 1 && agreed.track().front().tick == 23, "speed track uses its agreed tick");
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
                     tag("Win_Lose_Message_Font_Size", " 24 "), tag("Battle_Pending_Message_Color", "255, 32, 32, 240"),
                     tag("GUI_Tactical_Countdown_Timers_Screen_X", " 0.005 "), tag("GUI_Tactical_Countdown_Timers_Screen_Y", "0.25")};
    const ui::BattleMessageLooks looks = ui::battle_message_looks(root);
    expect(looks.diagnostics.empty(), "the FoC constants parse without warnings");
    expect(looks.point_size == 24, "the last Win_Lose_Message_Font_Size wins");
    expect(looks.win == data::ui::Rgba8{223, 243, 255, 255} && looks.lose == data::ui::Rgba8{255, 244, 223, 255}
               && looks.pending == data::ui::Rgba8{255, 32, 32, 240} && looks.font == "EmpireAtWar-Bold",
           "BE-03 and TM-09 looks");
    expect(looks.countdown_x == 0.005 && looks.countdown_y == 0.25, "RP-06: the tactical countdown line position comes from GameConstants");
    data::XmlNode broken;
    broken.children = {tag("Win_Message_Color", "223, 243, 255"), tag("Win_Lose_Message_Font_Size", "big")};
    const ui::BattleMessageLooks fallback = ui::battle_message_looks(broken);
    expect(fallback.win == data::ui::Rgba8{223, 243, 255, 255} && fallback.point_size == 24 && fallback.diagnostics.size() == 7,
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

// #5355 (TP-MP06): the LAN GC battle child's shared 2x toggle and its one-request-in-flight guard.
void test_shared_fast_forward() {
    static_assert(ui::shared_fast_forward_rate == 60 && ui::shared_fast_forward_target(30) == 60
        && ui::shared_fast_forward_target(60) == 30 && ui::shared_fast_forward_target(20) == 60);
    ui::SharedSpeedRequest request;
    expect(!request.begin(60, 60, 5), "the agreed rate is not a request");
    expect(request.begin(30, 60, 10) && request.target() == 60u, "a press requests 2x");
    expect(!request.begin(30, 60, 11) && !request.begin(30, 30, 11), "a second press waits for the agreed answer");
    request.observe(30, 12);
    expect(request.target() == 60u, "no agreed answer yet");
    request.observe(60, 14);
    expect(!request.target(), "the agreed 2x answers the request");
    expect(request.begin(60, 30, 20) && !request.begin(60, 30, 21), "back to 1x: one request again");
    request.observe(60, 20 + ui::SharedSpeedRequest::release_ticks - 1);
    expect(request.target() == 30u, "a refused request waits under one window");
    request.observe(60, 20 + ui::SharedSpeedRequest::release_ticks);
    expect(!request.target(), "one shared speed window releases it: never stuck");
    expect(request.begin(60, 30, 400), "pressable again");
    request.observe(60, 3);
    expect(!request.target(), "a new battle (tick went back) releases it");
    expect(request.begin(30, 60, 4), "pressable in the new battle");
    request.reset();
    expect(!request.target(), "reset clears it");
}

// #5355 (TP-MP06): the shared battle speed line says what really runs.
void test_shared_speed_line() {
    expect(ui::speed_factor_label(60) == "2x" && ui::speed_factor_label(30) == "1x" && ui::speed_factor_label(45) == "1.5x"
        && ui::speed_factor_label(20) == "0.67x" && ui::speed_factor_label(10) == "0.33x", "factor labels");
    expect(ui::shared_speed_line("Battle", 30, 30.0).empty() && ui::shared_speed_line("Battle", 30, std::nullopt).empty(), "no line at 1x");
    expect(ui::shared_speed_line("Battle", 60, std::nullopt) == "Battle speed 2x requested; measuring the real pace", "before a measurement");
    expect(ui::shared_speed_line("Battle", 60, 58.0) == "Battle speed 2x", "a held 2x reads plainly");
    expect(ui::shared_speed_line("Battle", 60, 42.0)
        == "Battle speed 2x requested, running at 1.4x: the slowest player's computer sets the pace", "a slower real pace is named");
    expect(ui::shared_speed_line("Battle", 60, 1.0).find("running at under 0.1x") != std::string::npos, "a near stop is not 0x");
    expect(ui::shared_speed_line("Battle", 20, 20.0) == "Battle speed 0.67x", "slow speeds read plainly");
    ui::SharedPaceMeter meter;
    meter.sample(100, 1000, 60, true);
    expect(!meter.ticks_per_second(), "one sample measures nothing");
    for (std::uint64_t ms = 1100, tick = 103; ms <= 4000; ms += 100, tick += 3) meter.sample(tick, ms, 60, true);
    const auto rate = meter.ticks_per_second();
    expect(rate && *rate > 29.0 && *rate < 31.0, "30 ticks a second over the window");
    meter.sample(190, 4100, 60, false);
    expect(!meter.ticks_per_second(), "a pause drops the samples");
    meter.sample(190, 4200, 60, true); meter.sample(250, 5300, 30, true);
    expect(!meter.ticks_per_second(), "a new requested rate starts a new measurement");
    meter.sample(260, 5400, 30, true); meter.sample(400, 7000, 30, true);
    expect(!meter.ticks_per_second(), "a gap in the samples starts a new measurement");
    ui::SharedPaceMeter busy;
    for (std::uint64_t ms = 10000, tick = 0; ms < 70000; ++ms, tick += (ms % 33 == 0)) busy.sample(tick, ms, 60, true);
    expect(busy.samples() <= ui::SharedPaceMeter::capacity && busy.samples() >= ui::SharedPaceMeter::window_ms / ui::SharedPaceMeter::sample_ms,
        "a refresh every millisecond keeps a bounded sample set");
    const auto busy_rate = busy.ticks_per_second();
    expect(busy_rate && *busy_rate > 29.0 && *busy_rate < 31.5, "and still measures the real pace");
}

// #5355 (review of #6272): the HUD notice formats only when what it shows changes; an unchanged speed, pace and
// (expired or showing) actor line cost no allocation, also with a long name.
void test_shared_speed_notice() {
    ui::SharedSpeedNotice notice;
    expect(notice.refresh(100, 1000, 30, true) && notice.text().empty(), "1x shows nothing");
    expect(!notice.refresh(101, 1016, 30, true), "an unchanged 1x does not refresh the text");
    const std::string name(512, 'N');
    notice.change(7, name + " set the battle speed to 2x", 1032);
    expect(notice.revision() == 7, "the event identity is kept for the caller's check");
    expect(notice.refresh(102, 1032, 60, true) && notice.text() == name + " set the battle speed to 2x\nBattle speed 2x requested; measuring the real pace",
        "a new speed event names its actor and the request");
    // A stable 1.4x against the requested 2x: 42 ticks a second.
    const auto tick_at = [](std::uint64_t ms) { return 102 + (ms - 1032) * 42 / 1000; };
    std::uint64_t ms = 1032;
    for (; ms < 4000; ms += 16) static_cast<void>(notice.refresh(tick_at(ms), ms, 60, true));
    expect(notice.text().ends_with("Battle speed 2x requested, running at 1.4x: the slowest player's computer sets the pace")
        && notice.text().starts_with(name), "the measured pace shows beside the actor line");
    const auto before = allocations.load();
    unsigned changes = 0;
    for (; ms < 5000; ms += 16) changes += notice.refresh(tick_at(ms), ms, 60, true) ? 1U : 0U;
    const bool quiet = changes == 0 && allocations.load() == before; // read before expect() builds its message
    expect(quiet, "an unchanged actor, speed and pace allocate nothing");
    for (; ms < 6100; ms += 16) changes += notice.refresh(tick_at(ms), ms, 60, true) ? 1U : 0U;
    expect(changes == 1 && notice.text() == "Battle speed 2x requested, running at 1.4x: the slowest player's computer sets the pace",
        "the actor line expires after five seconds, once");
    const auto expired = allocations.load();
    for (; ms < 12000; ms += 16) changes += notice.refresh(tick_at(ms), ms, 60, true) ? 1U : 0U;
    const bool still_quiet = changes == 1 && allocations.load() == expired;
    expect(still_quiet, "an expired actor line is never rebuilt");
    expect(notice.refresh(tick_at(ms), ms, 60, false) && notice.text().empty(), "a pause hides the line");
    expect(!notice.refresh(tick_at(ms), ms + 16, 60, false), "and stays hidden without work");
    notice.change(8, name + " set the battle speed to 1x", ms + 32);
    expect(notice.refresh(tick_at(ms), ms + 32, 30, true) && notice.text() == name + " set the battle speed to 1x",
        "the next event shows its actor again");
}

} // namespace

int main() {
    test_battle_messages();
    test_speed_steps();
    test_fast_forward();
    test_pause();
    test_end_and_track();
    test_shared_fast_forward();
    test_shared_speed_line();
    test_shared_speed_notice();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " time control check(s) failed\n";
        return 1;
    }
    std::cout << "ui time controls passed\n";
    return 0;
}
