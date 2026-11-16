// TSW-15: the tactical super weapon's presentation helpers (speech tags, name lists, the explosion pick, the clip window).

#include "../../support/check.hpp"
#include "eawr/presentation/space/tsw.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

namespace space = eawr::presentation::space;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string& message) {
    if (!condition) local_checker.check_before_print(condition, message, std::cerr, "FAIL: ");
}

void speech() {
    expect(space::tsw_speech_for_threshold(60) == space::TswSpeech::range_60, "60 s");
    expect(space::tsw_speech_for_threshold(30) == space::TswSpeech::range_30, "30 s");
    expect(space::tsw_speech_for_threshold(15) == space::TswSpeech::range_15, "15 s");
    expect(space::tsw_speech_for_threshold(5) == space::TswSpeech::range_05, "5 s");
    expect(!space::tsw_speech_for_threshold(45) && !space::tsw_speech_for_threshold(0), "other seconds name no line");
    expect(space::tsw_speech_tag(space::TswSpeech::moving_into_range, true) == "SpeechEvent_Super_Weapon_Moving_Into_Range",
           "own approach line");
    expect(space::tsw_speech_tag(space::TswSpeech::range_60, true) == "SpeechEvent_Super_Weapon_Moving_Range_60_Seconds",
           "own 60 s line");
    expect(space::tsw_speech_tag(space::TswSpeech::range_05, false) == "SpeechEvent_Super_Weapon_Enemy_Moving_Range_05_Seconds",
           "enemy 5 s line keeps the zero pad");
    expect(space::tsw_speech_tag(space::TswSpeech::moved_into_range, false) == "SpeechEvent_Super_Weapon_Enemy_Moved_Into_Range",
           "enemy arrival line");
}

void names() {
    const auto split = space::tsw_split_names(" Large_Explosion_Space , Planet_Chunks\n; Small ,, ");
    expect(split.size() == 3 && split[0] == "Large_Explosion_Space" && split[1] == "Planet_Chunks" && split[2] == "Small",
           "names split at commas, semicolons and whitespace, in authored order");
    expect(space::tsw_split_names("   ").empty() && space::tsw_split_names("").empty(), "no names");
}

void pick() {
    expect(!space::tsw_pick_explosion(0, 123), "an empty list picks nothing");
    expect(space::tsw_pick_explosion(1, 0xFFFFFFFFU) == 0U, "one entry is always the pick");
    expect(space::tsw_pick_explosion(3, 0) == 0U, "the lowest draw picks the first");
    expect(space::tsw_pick_explosion(3, 0xFFFFFFFFU) == 2U, "the highest draw picks the last");
    expect(space::tsw_pick_explosion(4, 0x40000000U) == 1U, "a quarter draw picks the second of four");
}

void clip() {
    space::TswClipWindow window{100, std::nullopt};
    expect(!space::tsw_clip_elapsed(window, 99.0), "no clip before the attack start");
    expect(space::tsw_clip_elapsed(window, 100) == 0.0, "frame 0 at the start tick");
    expect(space::tsw_clip_elapsed(window, 100.5) == 0.5, "it advances between ticks");
    expect(space::tsw_clip_elapsed(window, 5000) == 4900.0, "it plays on until the destruction is seen");
    window.end_tick = 175;
    expect(space::tsw_clip_elapsed(window, 174) == 74.0, "the last shown tick");
    expect(!space::tsw_clip_elapsed(window, 175.0), "the destruction tick ends the clip");
}

} // namespace

int main() {
    speech();
    names();
    pick();
    clip();
    return failures == 0 ? 0 : 1;
}
