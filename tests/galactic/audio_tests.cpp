#include "eawr/presentation/audio/galactic.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

using namespace eawr::presentation::audio;

int main() {
    const auto f = [](std::int64_t n) { return eawr::sim::math::Fixed::from_raw(n * eawr::sim::math::Fixed::scale); };
    // WGP-55/59: supplied admitted observations, including queued usage. No HUD
    // polling, baseline lifecycle, cue existence or voice admission is inferred.
    for (const auto previous : {0, 1, 5, 6, 8}) for (const auto current : {0, 1, 5, 6, 8})
        for (const auto capacity : {0, 5, 6, 10}) {
            const bool expected = previous > 0 && current > previous && current >= capacity;
            assert(galactic_population_warning(3, 3, true, f(previous), f(current), f(capacity)) == expected);
            assert(!galactic_population_warning(2, 3, true, f(previous), f(current), f(capacity)));
            assert(!galactic_population_warning(3, 3, false, f(previous), f(current), f(capacity)));
            assert(!galactic_population_warning(0, 0, true, f(previous), f(current), f(capacity)));
        }
    auto previous = f(4);
    for (const auto current : {5, 6, 7}) {
        assert(galactic_population_warning(3, 3, true, previous, f(current), f(5)));
        // Caller stores even if a missing/refused cue makes the request silent.
        previous = f(current);
        assert(!galactic_population_warning(3, 3, true, previous, f(current), f(5)));
    }
    using Kind = eawr::sim::galactic::GalacticEventKind;
    eawr::sim::galactic::GalacticEvent event;
    assert(galactic_event_sound(event, 3, {}, {}, false, 3) == GalacticSound::construction_options);
    assert(!galactic_event_sound(event, 2, {}, {}, false, 3));
    assert(!galactic_event_sound(event, 0, {}, {}, false, 3));
    const std::vector<Field> unlock_faction{{"SFXEvent_New_Construction_Options_Available", "custom_unlock"}};
    assert(galactic_sound_event(GalacticSound::construction_options, unlock_faction, {}) == "custom_unlock");
    assert(galactic_sound_event(GalacticSound::construction_options, {}, unlock_faction).empty());
    const std::vector<Field> approach_faction{{"SFXEvent_Enemy_Fleet_Approaching_Planet", "renamed_approach"}};
    assert(galactic_sound_event(GalacticSound::enemy_approaching, approach_faction, {}) == "renamed_approach");
    assert(galactic_sound_event(GalacticSound::enemy_approaching, {}, approach_faction).empty());
    event.kind = Kind::discovery_feedback; event.value = 3;
    assert(galactic_event_sound(event, 3, {}, {}, false) == GalacticSound::planet_revealed);
    assert(!galactic_event_sound(event, 2, {}, {}, false) && !galactic_event_sound(event, 0, {}, {}, false));
    event.kind = Kind::planet_discovered;
    assert(!galactic_event_sound(event, 3, {}, {}, false)); // WGUI-05 coalesced feedback owns sound.
    event.kind = Kind::conflict_pending;
    for (const bool land : {false, true}) {
        const GalacticPendingAlert pending{2, 3, land};
        for (const auto local : {2U, 3U}) {
            const auto cue = galactic_event_sound(event, local, {}, pending, false);
            assert(cue == (land ? GalacticSound::land_battle_pending : GalacticSound::space_battle_pending));
            const std::vector<Field> faction{{std::string(galactic_sound_tag(*cue)), "local_pending"}};
            assert(galactic_sound_event(*cue, faction, {}) == "local_pending");
        }
        assert(!galactic_event_sound(event, 1, {}, pending, false));
    }
    assert(!galactic_event_sound(event, 3, {}, {}, false));
    event.kind = Kind::planet_owner_changed; event.value = 3;
    assert(galactic_event_sound(event, 3, 2, {}, false) == GalacticSound::planet_gained);
    assert(!galactic_event_sound(event, 3, {}, {}, false));
    event.kind = Kind::victory_matured;
    assert(!galactic_event_sound(event, 3, {}, {}, false)); // No sourced campaign loser yet.
    using Notice = eawr::sim::galactic::GalacticFeedbackNotice;
    for (const auto& [kind, expected] : {
        std::pair{Notice::Kind::capture_reward, GalacticSound::capture_bonus},
        std::pair{Notice::Kind::enemy_approaching, GalacticSound::enemy_approaching},
        std::pair{Notice::Kind::planet_corrupted, GalacticSound::planet_corrupted},
        std::pair{Notice::Kind::corruption_removed, GalacticSound::corruption_removed}}) {
        const Notice notice{kind, 3, 10};
        assert(galactic_event_sound(event, 3, {}, {}, false, {}, notice) == expected);
        assert(!galactic_event_sound(event, 2, {}, {}, false, {}, notice));
        assert(!galactic_event_sound(event, 0, {}, {}, false, {}, notice));
    }
    assert(galactic_owner_sound(3, 2, 3, true) == GalacticSound::planet_gained);
    assert(galactic_owner_sound(3, 3, 2, true) == GalacticSound::planet_lost);
    assert(!galactic_owner_sound(3, 1, 2, true));
    assert(!galactic_owner_sound(3, 3, 3, true));
    assert(!galactic_owner_sound(0, 0, 3, true));
    assert(!galactic_owner_sound(3, 3, 2, false)); // nonplayable or unknown new owner
    assert(!galactic_owner_sound(3, 3, 0, true)); // an absent owner cannot pass
    assert(galactic_owner_sound(3, 2, 3, false) == GalacticSound::planet_gained); // exact gain has its separate gate
    event.kind = Kind::planet_owner_changed; event.value = 2;
    assert(galactic_event_sound(event, 3, 3, {}, true) == GalacticSound::planet_lost);
    assert(!galactic_event_sound(event, 3, 3, {}, false));
    assert(!galactic_event_sound(event, 1, 3, {}, true)); // an ally/uninvolved player is not local
    event.value = 0; assert(!galactic_event_sound(event, 3, 3, {}, true));
    const auto loading = parse_music_event("load", std::vector<Field>{{"Files", "load.mp3"},
        {"MSS_Internal_Loop", "yes"}, {"Loop", "yes"}, {"Volume_Percent", "55"}});
    assert(loading.internal_loop && loading.loop && loading.volume == 0.55);
    const MusicEvent ambient{"ambient", {"a.mp3", "b.mp3"}, 0.6, 2, 2, true};
    const MusicEvent win{"win", {"win.mp3"}, 1, 0, 0, false};
    const MusicEvent lose{"lose", {"lose.mp3"}, 1, 0, 0, false};
    GalacticMusicDirector music;
    using Mode = GalacticMusicDirector::Mode;
    assert(!music.ambient(&ambient, false, false, true));
    assert(!music.ambient(nullptr, true));
    assert(music.mode() == Mode::none);
    assert(music.ambient(&ambient, true)->file == "a.mp3");
    assert(!music.ambient(&ambient, true));
    assert(music.track_ended()->file == "b.mp3");
    assert(music.track_ended()->file == "a.mp3");
    assert(!music.start(Mode::battle_pending, &ambient));
    assert(music.mode() == Mode::strategic_ambient);
    for (const auto protected_mode : {Mode::superweapon_attack, Mode::demo_finale, Mode::scripted}) {
        music.external_mode(protected_mode);
        assert(!music.ambient(&ambient, true));
        assert(music.ambient(&ambient, true, false, true));
    }
    music.external_mode(Mode::tactical);
    assert(!music.ambient(&ambient, true, true));
    assert(music.ambient(&ambient, true));
    assert(!music.outcome(true, false, &win, &lose));
    assert(!music.outcome(true, true, nullptr, &lose));
    assert(music.mode() == Mode::strategic_ambient);
    assert(music.outcome(true, true, &win, &lose)->mode == Mode::strategic_win);
    assert(!music.track_ended());
    assert(!music.outcome(true, true, &win, &lose));
    assert(music.outcome(false, true, &win, &lose)->mode == Mode::strategic_lose);
    assert(!music.outcome(false, true, &win, &lose));

    // WGP-46: missing load music must preserve both the mode and playlist
    // cursor. Pending/load share the same stream without changing its mode.
    GalacticMusicDirector loading_music;
    assert(loading_music.ambient(&ambient, true)->file == "a.mp3");
    auto load = loading_music.battle_load(nullptr);
    assert(!load.stop_music && !load.cue);
    assert(loading_music.mode() == Mode::strategic_ambient);
    assert(loading_music.track_ended()->file == "b.mp3");
    load = loading_music.battle_load(&loading);
    assert(load.stop_music && load.cue && load.cue->mode == Mode::battle_load);
    load = loading_music.battle_load(&loading);
    assert(!load.stop_music && !load.cue);
    loading_music.external_mode(Mode::none);
    assert(loading_music.start(Mode::battle_pending, &loading));
    load = loading_music.battle_load(&loading);
    assert(!load.stop_music && !load.cue && loading_music.mode() == Mode::battle_pending);
    for (const auto mode : {Mode::battle_pending, Mode::battle_load, Mode::scripted}) {
        loading_music.external_mode(mode);
        assert(loading_music.start(mode, &loading));
        load = loading_music.battle_load(nullptr, true, true);
        assert(!load.stop_music && !load.cue && loading_music.mode() == mode);
        load = loading_music.battle_load(&loading, false, true);
        assert(load.stop_music && load.cue && loading_music.mode() == Mode::battle_load);
        load = loading_music.battle_load(&loading, true);
        assert(load.stop_music && !load.cue && loading_music.mode() == Mode::none);
        assert(!loading_music.track_ended());
    }
    loading_music.external_mode(Mode::battle_pending);
    assert(loading_music.start(Mode::battle_pending, &ambient));
    load = loading_music.battle_load(&loading);
    assert(!load.stop_music && load.cue && loading_music.mode() == Mode::battle_load);

    const std::vector<Field> outcomes{
        {"Music_Event_Strategic_Win", "default_win"},
        {"Music_Event_Strategic_Lose", "default_lose"},
        {"Music_Event_Strategic_Win_Vs_Faction", "Renamed_Opponent, first"},
        {"Music_Event_Strategic_Win_Vs_Faction", "Renamed_Opponent, second"},
        {"Music_Event_Strategic_Lose_Vs_Faction", "Synthetic_Rival,"},
    };
    assert(strategic_result_event(outcomes, true, "renamed_opponent") == "first");
    assert(strategic_result_event(outcomes, true, "unknown") == "default_win");
    assert(strategic_result_event(outcomes, true, "") == "default_win");
    assert(strategic_result_event(outcomes, false, "synthetic_rival").empty());
    assert(strategic_result_event(outcomes, false, "") == "default_lose");
    const std::vector<Field> faction{
        {"SFXEvent_Planet_Gained_Control", "mod_gained"},
        {"SFXEvent_Planet_Lost_Control", "mod_lost"},
        {"SFXEvent_Planet_Revealed", "wrong_table"},
    };
    const std::vector<Field> audio{{"SFXEvent_Planet_Revealed", "mod_revealed"},
        {"SFXEvent_Planet_Capture_Bonus_Reward", "mod_bonus"}};
    assert(galactic_sound_event(GalacticSound::planet_gained, faction, audio) == "mod_gained");
    assert(galactic_sound_event(GalacticSound::planet_lost, faction, audio) == "mod_lost");
    assert(galactic_sound_event(GalacticSound::planet_revealed, faction, audio) == "mod_revealed");
    assert(galactic_sound_event(GalacticSound::capture_bonus, faction, audio) == "mod_bonus");
    assert(galactic_sound_event(GalacticSound::planet_corrupted, faction, audio).empty());
    for (const auto cue : {GalacticSound::planet_gained, GalacticSound::planet_lost,
        GalacticSound::planet_corrupted, GalacticSound::corruption_removed, GalacticSound::enemy_approaching, GalacticSound::planet_revealed,
        GalacticSound::capture_bonus, GalacticSound::population_cap, GalacticSound::credit_cap,
        GalacticSound::mission_added, GalacticSound::construction_options, GalacticSound::starbase_upgraded,
        GalacticSound::enemy_starbase_upgraded, GalacticSound::ally_starbase_upgraded, GalacticSound::blockade,
        GalacticSound::space_battle_pending, GalacticSound::land_battle_pending}) {
        const std::vector<Field> table{{std::string(galactic_sound_tag(cue)), "authored"}};
        assert(galactic_sound_event(cue, table, table) == "authored");
    }
    // WGEP-96/97, WGP-63: authored speech suppresses the type SFX; nonlocal builders stay silent.
    using Build = GalacticBuildCue;
    const std::vector<Field> spoken{{"Build_Speech_Underway", "speech_go"}, {"Build_Speech_Completed", "speech_done"},
        {"SFXEvent_Build_Started", "sfx_go"}, {"SFXEvent_Build_Complete", "sfx_done"}, {"SFXEvent_Build_Cancelled", "sfx_stop"}};
    auto feedback = galactic_build_feedback(Build::underway, spoken, true);
    assert(feedback.speech == "speech_go" && feedback.sfx.empty());
    feedback = galactic_build_feedback(Build::completed, spoken, true);
    assert(feedback.speech == "speech_done" && feedback.sfx.empty());
    feedback = galactic_build_feedback(Build::stopped, spoken, true);
    assert(feedback.speech.empty() && feedback.sfx == "sfx_stop");
    for (const auto cue : {Build::underway, Build::completed, Build::stopped}) {
        feedback = galactic_build_feedback(cue, spoken, false);
        assert(feedback.speech.empty() && feedback.sfx.empty());
    }
    const std::vector<Field> plain{{"SFXEvent_Build_Complete", "rhd_complete"}};
    feedback = galactic_build_feedback(Build::completed, plain, true);
    assert(feedback.speech.empty() && feedback.sfx == "rhd_complete");
    feedback = galactic_build_feedback(Build::underway, plain, true);
    assert(feedback.speech.empty() && feedback.sfx.empty());
    assert(galactic_build_speech_tag(Build::stopped) == "Build_Speech_Stopped");
    assert(galactic_build_sfx_tag(Build::underway) == "SFXEvent_Build_Started");
    std::cout << "galactic audio contracts passed\n";
}
