// #535: the per-unit fade as the local player's space fog reveals or hides a unit
// (presentation::space::UnitFade, docs/behaviour/space-fog-presentation.md FW-16 to FW-18).
#include "eawr/presentation/space/unit_fade.hpp"

#include <iostream>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace space = eawr::presentation::space;
using eawr::sim::EntityId;

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// FW-17: an entity `alive` has not held before starts at full opacity at once when it is shown.
void test_spawn_is_instant() {
    space::UnitFade fade;
    fade.advance({{1, 2}}, {{1, 2, 3}}, 0.0);
    expect(fade.opacity(1) == std::optional<float>(1.0F), "a unit visible at its first sight starts at full opacity");
    expect(fade.opacity(2) == std::optional<float>(1.0F), "a second unit visible at its first sight also starts full");
    expect(!fade.opacity(3).has_value(), "a unit alive but fogged at its first sight is not drawn");
    const auto drawn = fade.drawn();
    expect(drawn.size() == 2 && drawn[0] == 1 && drawn[1] == 2, "only the shown units are drawn");
}

void test_projectile_observer_hide() {
    space::ProjectileHide shot;
    bool fog_clear = false;
    int queries = 0;
    const auto query = [&] { ++queries; return fog_clear; };
    shot.advance(0, false, false, query);
    expect(!shot.drawn(), "WPJ-38: visible endpoints do not admit a fogged projectile");
    for (int tick = 1; tick < 30; ++tick) shot.advance(tick, false, false, query);
    expect(queries == 1, "WPJ-38: settled projectiles query once per 30 logical frames");
    fog_clear = true;
    shot.advance(30, false, false, query);
    expect(queries == 2 && shot.opacity() > 0 && shot.opacity() < 0.1F,
           "a revealed projectile starts easing at its recheck, independently of the source");
    shot.advance(31, false, false, query);
    expect(queries == 3 && shot.drawn(), "active transitions keep servicing before 30 frames");
    fog_clear = false;
    shot.advance(32, false, false, query);
    expect(queries == 4 && !shot.revealed(), "a transition rechecks a fog boundary immediately");
    for (int tick = 33; tick < 100; ++tick) shot.advance(tick, false, false, query);
    expect(!shot.drawn(), "hidden flight stops drawing after its transition");
    fog_clear = true;
    space::ProjectileHide immediate;
    immediate.advance(0, false, true, query);
    expect(immediate.opacity() == 1.0F && immediate.drawn(), "visible initialization is immediate");
    fog_clear = false;
    immediate.advance(1, false, true, query);
    expect(immediate.drawn(), "WPJ-39: immediate mode preserves the settled service timer");
    immediate.advance(30, false, true, query);
    expect(!immediate.drawn(), "WPJ-39: Last_State_Visible_Under_FOW hides immediately when serviced");
    fog_clear = true;
    immediate.advance(60, false, true, query);
    expect(immediate.opacity() == 1.0F && immediate.drawn(), "immediate mode reveals immediately when serviced");
    expect(space::ProjectileHide::model_visible(0.025), "WPJ-39: equality at 0.025 remains drawn");
    expect(!space::ProjectileHide::model_visible(std::nextafter(0.025, 0.0)),
           "WPJ-39: values strictly below 0.025 hide");
    space::ProjectileHide delayed;
    delayed.advance(0, true, false, query);
    fog_clear = true;
    delayed.advance(30, true, false, query);
    expect(!delayed.drawn(), "WPJ-38: delayed appearance stays in limbo");
    delayed.advance(31, false, false, query);
    expect(delayed.drawn() && delayed.opacity() == 1.0F,
           "appearance queries current fog without a stale pre-delay transition");
    delayed.advance(31, false, false, [] { return false; });
    expect(delayed.drawn(), "a paused presentation does not change projectile admission");
}

// FW-16, FW-17: a unit already held before that is revealed later eases up from zero instead of
// popping, and a unit that loses visibility eases down instead of vanishing.
void test_reveal_and_hide_ease() {
    space::UnitFade fade;
    fade.advance({}, {{1}}, 0.0); // tick 0: entity 1 exists but is fogged
    expect(!fade.opacity(1).has_value(), "fogged from the start is not drawn");
    fade.advance({{1}}, {{1}}, 1.0); // revealed one logical frame later
    const auto after_one_frame = fade.opacity(1);
    expect(after_one_frame.has_value() && *after_one_frame > 0.0F && *after_one_frame < 0.1F,
           "a reveal ramps up over the first frame instead of stepping to full");
    fade.advance({{1}}, {{1}}, 60.0); // two seconds later, well past the 0.25 s ease
    const auto settled = fade.opacity(1);
    expect(settled.has_value() && *settled > 0.99F, "the ease converges to full opacity once it has run long enough");

    fade.advance({}, {{1}}, 1.0); // hidden again, one frame later
    const auto after_hide_frame = fade.opacity(1);
    expect(after_hide_frame.has_value() && *after_hide_frame > 0.9F && *after_hide_frame < 1.0F,
           "hiding ramps down over the first frame instead of vanishing at once");
    fade.advance({}, {{1}}, 60.0); // two seconds later, well past the ease and the hide threshold
    expect(!fade.opacity(1).has_value(), "a unit that has fully eased out while still hidden is forgotten");
}

// FW-17: a target flip before the ease settles keeps easing from where it stood (carrying its
// rate of change too, so it can still dip a little further before turning around), never from a
// reset to zero.
void test_flip_before_settling_does_not_restart() {
    space::UnitFade fade;
    fade.advance({{1}}, {{1}}, 0.0);
    fade.advance({}, {{1}}, 2.0);   // starts fading out, nowhere near the hide threshold yet
    const auto partway = fade.opacity(1);
    expect(partway.has_value() && *partway < 1.0F && *partway > 0.5F, "hiding has started easing down, not yet near hidden");
    fade.advance({{1}}, {{1}}, 1.0); // shown again before it settled
    expect(fade.opacity(1).has_value(), "a unit shown again before fading out is never dropped mid-flip");
    fade.advance({{1}}, {{1}}, 10.0); // several more frames for the ease to resolve
    const auto resumed = fade.opacity(1);
    expect(resumed.has_value() && *resumed > 0.9F,
           "a unit shown again before fading out converges back to full opacity, not a fresh fade-in from zero");
}

// FW-21 (this fidelity's fade never sees a death event itself; the tracker only reads `alive`):
// a unit the session no longer holds is forgotten at once, whatever its opacity.
void test_destroyed_unit_is_forgotten_at_once() {
    space::UnitFade fade;
    fade.advance({{1}}, {{1}}, 0.0);
    fade.advance({}, {{1}}, 1.0); // mid fade-out, well above the hide threshold
    expect(fade.opacity(1).has_value(), "still mid fade-out");
    fade.advance({}, {}, 1.0); // the entity leaves the session entirely (destroyed)
    expect(!fade.opacity(1).has_value(), "a destroyed unit is dropped at once, not left to finish fading");
}

// FW-16: fading is counted in logical frames, so a paused battle (0 frames) holds it exactly.
void test_paused_battle_holds() {
    space::UnitFade fade;
    fade.advance({}, {{1}}, 0.0);
    fade.advance({{1}}, {{1}}, 3.0);
    const auto before_pause = fade.opacity(1);
    fade.advance({{1}}, {{1}}, 0.0);
    expect(fade.opacity(1) == before_pause, "no logical frames elapsed leaves the ease exactly where it was");
}

void test_catch_up_matches_small_steps() {
    for (const double smooth : {0.25, 10.0}) {
        for (const double frames : {2.0, 20.25, 90.5, 300.75}) {
            // Isolate easing from retirement: with a slow curve the first reveal is below
            // the normal hide cutoff, so a reversal would retire both before their rates compare.
            space::UnitFade batched({smooth, 0.0}), stepped({smooth, 0.0});
            batched.advance({}, {{1}}, 0.0);
            stepped.advance({}, {{1}}, 0.0);
            batched.advance({{1}}, {{1}}, frames);
            for (double frame = 0; frame < std::floor(frames); ++frame)
                stepped.advance({{1}}, {{1}}, 1.0);
            stepped.advance({{1}}, {{1}}, frames - std::floor(frames));
            expect(batched.opacity(1) && stepped.opacity(1)
                && std::abs(*batched.opacity(1) - *stepped.opacity(1)) < 0.000001F,
                "bounded catch-up preserves whole and fractional native steps even with a slow ease");
            // Compare after a reversal too: matching opacity alone would not pin the carried velocity.
            batched.advance({}, {{1}}, 2.5);
            stepped.advance({}, {{1}}, 2.5);
            expect(batched.opacity(1) && stepped.opacity(1)
                && std::abs(*batched.opacity(1) - *stepped.opacity(1)) < 0.000001F,
                "catch-up preserves the velocity across a target reversal");
        }
    }
}

void test_extreme_elapsed_frames() {
    space::UnitFade fade;
    fade.advance({}, {{1}}, 0.0);
    fade.advance({{1}}, {{1}}, 1.0e12);
    expect(fade.opacity(1) == std::optional<float>(1.0F), "a huge elapsed interval settles in bounded work");
    fade.advance({}, {{1}}, 1.0e12);
    expect(!fade.opacity(1), "a huge elapsed hide interval retires the ghost");
    fade.advance({{1}}, {{1}}, 3.0);
    const auto before = fade.opacity(1);
    for (const double frames : {-1.0, std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN()}) {
        fade.advance({{1}}, {{1}}, frames);
        expect(fade.opacity(1) == before, "invalid elapsed frames hold the current ease");
    }
}

} // namespace

int main() {
    {
        space::FogGhosts memory;
        const std::array<double, 3> first{100, 200, 0}, changed{900, 800, 0};
        memory.advance({{{1, first, false, false}}}, {});
        expect(!memory.states().at(1).ghost, "FW-26: never seen and initially false leaves no ghost");
        memory.advance({{{1, first, true, false}}}, {});
        expect(memory.states().at(1).known && !memory.states().at(1).ghost, "first sight records knowledge");
        memory.advance({{{1, changed, false, false}}}, {});
        expect(memory.states().at(1).ghost && memory.states().at(1).position == first,
               "FW-26: hidden movement does not refresh the last seen transform");
        memory.advance({{{1, changed, true, false}}}, {});
        expect(!memory.states().at(1).ghost && memory.states().at(1).position == changed,
               "FW-28: re-seeing replaces the ghost and refreshes the remembered pose");
        memory.advance({{{1, first, false, false}}}, {});
        memory.advance({}, [](const auto&) { return false; });
        expect(memory.states().at(1).ghost && memory.states().at(1).position == changed,
               "FW-27: destruction while fogged retains the unchanged ghost");
        memory.advance({}, [&](const auto& point) { return point == first; });
        expect(memory.states().contains(1), "seeing another cell does not reveal a hidden destruction");
        memory.advance({}, [&](const auto& point) { return point == changed; });
        expect(!memory.states().contains(1), "seeing the saved cell removes the destroyed ghost");
        space::FogGhosts other_player;
        other_player.advance({{{1, first, false, false}}}, {});
        expect(!other_player.states().at(1).known, "FW-29: one observer's memory does not reveal to another");
        memory.advance({{{2, first, false, true}}}, {});
        expect(memory.states().at(2).known, "FW-25: initial tag seeds knowledge");
        memory.advance({{{3, first, true, false}}}, {});
        memory.advance({}, [](const auto&) { return true; });
        expect(!memory.states().contains(3), "visible destruction leaves no ghost");
        space::UnitFade instant;
        instant.advance({}, {{1, 2}}, 0, {{1}});
        instant.advance({{1, 2}}, {{1, 2}}, 1, {{1}});
        expect(instant.opacity(1) == std::optional<float>(1.0F), "WSU-05: a tagged reveal jumps to full opacity");
        expect(instant.opacity(2).value_or(1) < 0.1F, "ordinary units still ease");
        instant.advance({}, {{1, 2}}, 1, {{1}});
        expect(!instant.opacity(1), "WSU-05: a tagged hide jumps to hidden");
        expect(instant.opacity(2).has_value(), "ordinary units retain their moving fade");
    }
    space::NebulaBlend nebula;
    nebula.service(true);
    expect(nebula.value() == 0.0F, "WHZ-71: first contact changes the target after the visual service");
    nebula.service(true);
    expect(nebula.value() == 1.0F, "WHZ-71: cached contact settles the next visual service immediately");
    nebula.service(false);
    expect(nebula.value() == 1.0F, "WHZ-71: exit changes the target after the visual service");
    nebula.service(false);
    expect(nebula.value() > 0.9F && nebula.value() < 1.0F, "WHZ-71: exit uses the independent 0.15-second ease");
    for (int frame = 0; frame < 30; ++frame) nebula.service(false);
    expect(nebula.value() < 0.01F, "WHZ-71: exit converges below the material threshold");
    test_spawn_is_instant();
    test_projectile_observer_hide();
    test_reveal_and_hide_ease();
    test_flip_before_settling_does_not_restart();
    test_destroyed_unit_is_forgotten_at_once();
    test_paused_battle_holds();
    test_catch_up_matches_small_steps();
    test_extreme_elapsed_frames();
    if (failures != 0) {
        std::cerr << failures << " unit fade check(s) failed\n";
        return 1;
    }
    std::cout << "unit fade contracts passed\n";
    return 0;
}
