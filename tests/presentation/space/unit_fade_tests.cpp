// #535: the per-unit fade as the local player's space fog reveals or hides a unit
// (presentation::space::UnitFade, docs/behaviour/space-fog-presentation.md FW-16 to FW-18).
#include "eawr/presentation/space/unit_fade.hpp"

#include <iostream>
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

} // namespace

int main() {
    test_spawn_is_instant();
    test_reveal_and_hide_ease();
    test_flip_before_settling_does_not_restart();
    test_destroyed_unit_is_forgotten_at_once();
    test_paused_battle_holds();
    if (failures != 0) {
        std::cerr << failures << " unit fade check(s) failed\n";
        return 1;
    }
    std::cout << "unit fade contracts passed\n";
    return 0;
}
