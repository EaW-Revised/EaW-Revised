// #535: the per-unit fade as the local player's space fog reveals or hides a unit
// (presentation::live::UnitFade, docs/behaviour/space-fog-presentation.md FW-16 to FW-18).

#include "../../support/check.hpp"
#include "eawr/presentation/live/unit_fade.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace {

namespace live = eawr::presentation::live;
using eawr::sim::EntityId;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string& message) {
    if (!condition) local_checker.check_before_print(condition, message, std::cerr, "FAIL: ");
}

// FW-17: an entity `alive` has not held before starts at full opacity at once when it is shown.
void test_spawn_is_instant() {
    live::UnitFade fade;
    fade.advance({{1, 2}}, {{1, 2, 3}}, 0.0);
    expect(fade.opacity(1) == std::optional<float>(1.0F), "a unit visible at its first sight starts at full opacity");
    expect(fade.opacity(2) == std::optional<float>(1.0F), "a second unit visible at its first sight also starts full");
    expect(!fade.opacity(3).has_value(), "a unit alive but fogged at its first sight is not drawn");
    const auto drawn = fade.drawn();
    expect(drawn.size() == 2 && drawn[0] == 1 && drawn[1] == 2, "only the shown units are drawn");
}

void test_projectile_observer_hide() {
    live::ProjectileHide shot;
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
    live::ProjectileHide immediate;
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
    expect(live::ProjectileHide::model_visible(0.025), "WPJ-39: equality at 0.025 remains drawn");
    expect(!live::ProjectileHide::model_visible(std::nextafter(0.025, 0.0)),
           "WPJ-39: values strictly below 0.025 hide");
    live::ProjectileHide delayed;
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
    live::UnitFade fade;
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
    live::UnitFade fade;
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
    live::UnitFade fade;
    fade.advance({{1}}, {{1}}, 0.0);
    fade.advance({}, {{1}}, 1.0); // mid fade-out, well above the hide threshold
    expect(fade.opacity(1).has_value(), "still mid fade-out");
    fade.advance({}, {}, 1.0); // the entity leaves the session entirely (destroyed)
    expect(!fade.opacity(1).has_value(), "a destroyed unit is dropped at once, not left to finish fading");
}

// FW-16: fading is counted in logical frames, so a paused battle (0 frames) holds it exactly.
void test_paused_battle_holds() {
    live::UnitFade fade;
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
            live::UnitFade batched({smooth, 0.0}), stepped({smooth, 0.0});
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
    live::UnitFade fade;
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

// The pre-flat implementation of UnitFade::advance (one std::map entry per tracked unit, binary searches of the
// ascending inputs), kept here as the reference the flat sorted-vector version must equal bit for bit.
class ReferenceFade final {
public:
    explicit ReferenceFade(const live::UnitFadeLooks looks) : looks_(looks) {}

    void advance(const std::span<const EntityId> visible, const std::span<const EntityId> alive, const double frames,
                 const std::span<const EntityId> immediate) {
        std::vector<EntityId> first_sight;
        for (const EntityId entity : alive) {
            if (!std::binary_search(ever_seen_.begin(), ever_seen_.end(), entity)) first_sight.push_back(entity);
        }
        if (!first_sight.empty()) {
            ever_seen_.insert(ever_seen_.end(), first_sight.begin(), first_sight.end());
            std::sort(ever_seen_.begin(), ever_seen_.end());
        }
        for (auto entry = fading_.begin(); entry != fading_.end();) {
            entry = std::binary_search(alive.begin(), alive.end(), entry->first) ? std::next(entry) : fading_.erase(entry);
        }
        for (const EntityId entity : visible) {
            auto entry = fading_.find(entity);
            if (entry != fading_.end()) {
                entry->second.target_visible = true;
                continue;
            }
            const bool spawned_now = std::binary_search(first_sight.begin(), first_sight.end(), entity)
                || std::binary_search(immediate.begin(), immediate.end(), entity);
            fading_.emplace(entity, State{spawned_now ? 1.0 : 0.0, 0.0, true});
        }
        for (auto& [entity, state] : fading_) {
            if (!std::binary_search(visible.begin(), visible.end(), entity)) state.target_visible = false;
        }
        const double elapsed = std::isfinite(frames) ? std::max(0.0, frames) : 0.0;
        const double whole = std::floor(elapsed);
        const double fraction = elapsed - whole;
        const double frame_seconds = 1.0 / static_cast<double>(eawr::sim::tactical::logical_frames_per_second);
        for (auto& [entity, state] : fading_) {
            const double target = state.target_visible ? 1.0 : 0.0;
            if (std::binary_search(immediate.begin(), immediate.end(), entity)) {
                state.value = target;
                state.velocity = 0.0;
                continue;
            }
            ease(state.value, state.velocity, target, looks_.smooth_seconds, frame_seconds, whole);
            ease(state.value, state.velocity, target, looks_.smooth_seconds, fraction * frame_seconds);
        }
        for (auto& [entity, state] : fading_) state.value = std::clamp(state.value, 0.0, 1.0);
        for (auto entry = fading_.begin(); entry != fading_.end();) {
            const bool done = !entry->second.target_visible && entry->second.value <= looks_.hide_threshold;
            entry = done ? fading_.erase(entry) : std::next(entry);
        }
    }

    [[nodiscard]] std::vector<live::UnitFade::Row> rows() const {
        std::vector<live::UnitFade::Row> result;
        for (const auto& [entity, state] : fading_) result.push_back({entity, state.value, state.velocity, state.target_visible});
        return result;
    }

private:
    struct State {
        double value{1.0};
        double velocity{0.0};
        bool target_visible{true};
    };
    static void ease(double& value, double& velocity, const double target, const double smooth_seconds,
                     const double dt, const double steps = 1.0) {
        if (dt <= 0.0 || smooth_seconds <= 0.0 || steps <= 0.0) return;
        const double omega = 2.0 / smooth_seconds;
        const double x = omega * dt;
        const double one_decay = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
        const double decay = steps == 1.0 ? one_decay : std::pow(one_decay, steps);
        if (decay == 0.0) {
            value = target;
            velocity = 0.0;
            return;
        }
        const double change = value - target;
        const double temp = (velocity + omega * change) * dt * steps;
        velocity = (velocity - omega * temp) * decay;
        value = target + (change + temp) * decay;
    }

    live::UnitFadeLooks looks_;
    std::map<EntityId, State> fading_;
    std::vector<EntityId> ever_seen_;
};

[[nodiscard]] std::uint64_t bits(const double value) { return std::bit_cast<std::uint64_t>(value); }

// The flat sorted-vector UnitFade equals the map reference on a long seeded sequence: after every call the
// tracked entities, every stored double (compared as bits), opacity() and drawn() are identical. The sequence
// covers births, deaths, visibility flips, tagged-instant sets (also for ids outside `alive`), a duplicated
// visible id, visible ids the session does not hold, fractional, zero, stalled, negative and non-finite frame
// counts, and three ease configurations including the no-retirement one.
void test_flat_matches_reference() {
    const live::UnitFadeLooks configurations[] = {{}, {10.0, 0.0}, {0.25, 0.4}};
    std::size_t compared_rows = 0;
    for (std::size_t configuration = 0; configuration < std::size(configurations); ++configuration) {
        std::mt19937_64 random(0x5eed0000ULL + configuration);
        live::UnitFade flat(configurations[configuration]);
        ReferenceFade reference(configurations[configuration]);
        const double frame_choices[] = {0.0, 1.0, 1.0, 1.0, 0.5, 2.25, 7.0, 40.75, 400.5, -3.0,
                                        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()};
        for (int step = 0; step < 600; ++step) {
            std::vector<EntityId> alive, visible, immediate;
            for (EntityId id = 1; id <= 160; ++id) {
                if (random() % 100 < 85U + static_cast<unsigned>((step / 40) % 10)) alive.push_back(id + (id > 80 ? 5000 : 0));
            }
            for (const EntityId id : alive) {
                if (random() % 100 < 45) visible.push_back(id);
                if (random() % 100 < 4) immediate.push_back(id);
            }
            if (step % 7 == 0) visible.push_back(9000 + step); // shown but not held by the session
            if (step % 11 == 0 && !visible.empty()) visible.insert(visible.begin() + 1, visible.front()); // duplicate id
            if (step % 13 == 0) immediate.push_back(9500 + step); // tag outside `alive`
            std::sort(visible.begin(), visible.end());
            std::sort(immediate.begin(), immediate.end());
            immediate.erase(std::unique(immediate.begin(), immediate.end()), immediate.end());
            const double frames = frame_choices[random() % std::size(frame_choices)];
            flat.advance(visible, alive, frames, immediate);
            reference.advance(visible, alive, frames, immediate);

            const auto got = flat.rows();
            const auto want = reference.rows();
            bool same = got.size() == want.size();
            for (std::size_t index = 0; same && index < got.size(); ++index) {
                same = got[index].entity == want[index].entity && bits(got[index].value) == bits(want[index].value)
                    && bits(got[index].velocity) == bits(want[index].velocity)
                    && got[index].target_visible == want[index].target_visible;
            }
            if (!same) {
                expect(false, "flat UnitFade differs from the reference at configuration " + std::to_string(configuration)
                                  + ", step " + std::to_string(step));
                return;
            }
            compared_rows += got.size();
            const auto drawn = flat.drawn();
            bool drawn_same = drawn.size() == want.size();
            for (std::size_t index = 0; drawn_same && index < drawn.size(); ++index) {
                drawn_same = drawn[index] == want[index].entity
                    && flat.opacity(drawn[index]) == std::optional<float>(static_cast<float>(want[index].value));
            }
            if (!drawn_same) {
                expect(false, "flat UnitFade drawn()/opacity() differ from the reference at step " + std::to_string(step));
                return;
            }
            if (step % 50 == 0 && !alive.empty()) {
                expect(!flat.opacity(1234567).has_value(), "an unknown entity has no opacity");
            }
        }
    }
    expect(compared_rows > 20000, "the sequence compared enough tracked rows to mean something");
}

// A caller that hands over unsorted or duplicated ids gets the decisions of the sorted, unique input.
void test_unsorted_inputs_are_normalised() {
    live::UnitFade sorted, shuffled;
    sorted.advance({{1, 2, 3}}, {{1, 2, 3, 4}}, 0.0);
    shuffled.advance({{3, 1, 2, 2}}, {{4, 2, 1, 3}}, 0.0);
    sorted.advance({{2}}, {{1, 2, 3, 4}}, 3.0);
    shuffled.advance({{2, 2}}, {{3, 4, 1, 2}}, 3.0);
    const auto a = sorted.rows();
    const auto b = shuffled.rows();
    bool same = a.size() == b.size();
    for (std::size_t index = 0; same && index < a.size(); ++index) {
        same = a[index].entity == b[index].entity && bits(a[index].value) == bits(b[index].value)
            && bits(a[index].velocity) == bits(b[index].velocity) && a[index].target_visible == b[index].target_visible;
    }
    expect(same, "unsorted inputs decide like their sorted unique equivalent");
}

} // namespace

// opacity_walk answers exactly like opacity(), for ascending, repeated and descending queries alike.
void test_opacity_walk_matches_opacity() {
    std::mt19937_64 random(0x0ac17eULL);
    live::UnitFade fade;
    std::vector<EntityId> visible, alive;
    for (EntityId id = 1; id <= 300; ++id) {
        alive.push_back(id * 2);
        if (random() % 3 != 0) visible.push_back(id * 2);
    }
    for (int step = 0; step < 40; ++step) {
        for (auto& id : visible) if (random() % 11 == 0) id = alive[static_cast<std::size_t>(random() % alive.size())];
        std::sort(visible.begin(), visible.end());
        visible.erase(std::unique(visible.begin(), visible.end()), visible.end());
        fade.advance(visible, alive, 1.0);
        std::size_t cursor = 0;
        for (EntityId id = 0; id <= 605; ++id) {
            expect(fade.opacity_walk(id, cursor) == fade.opacity(id), "ascending walk matches opacity()");
        }
        for (int probe = 0; probe < 200; ++probe) {
            const EntityId id = random() % 620;
            expect(fade.opacity_walk(id, cursor) == fade.opacity(id), "random-order walk matches opacity()");
        }
        std::size_t stale = 100000;
        expect(fade.opacity_walk(40, stale) == fade.opacity(40), "an out-of-range cursor is repositioned");
    }
}

int main() {
    {
        live::FogGhosts memory;
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
        live::FogGhosts other_player;
        other_player.advance({{{1, first, false, false}}}, {});
        expect(!other_player.states().at(1).known, "FW-29: one observer's memory does not reveal to another");
        memory.advance({{{2, first, false, true}}}, {});
        expect(memory.states().at(2).known, "FW-25: initial tag seeds knowledge");
        memory.advance({{{3, first, true, false}}}, {});
        memory.advance({}, [](const auto&) { return true; });
        expect(!memory.states().contains(3), "visible destruction leaves no ghost");
        live::UnitFade instant;
        instant.advance({}, {{1, 2}}, 0, {{1}});
        instant.advance({{1, 2}}, {{1, 2}}, 1, {{1}});
        expect(instant.opacity(1) == std::optional<float>(1.0F), "WSU-05: a tagged reveal jumps to full opacity");
        expect(instant.opacity(2).value_or(1) < 0.1F, "ordinary units still ease");
        instant.advance({}, {{1, 2}}, 1, {{1}});
        expect(!instant.opacity(1), "WSU-05: a tagged hide jumps to hidden");
        expect(instant.opacity(2).has_value(), "ordinary units retain their moving fade");
    }
    live::NebulaBlend nebula;
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
    eawr::presentation::live::StealthBlend cloak;
    cloak.service(true, 0.5);
    expect(cloak.value() > 0 && cloak.value() < 0.02F, "WST-12: cloak starts a damped fade toward colour");
    for (int frame = 1; frame < 15; ++frame) cloak.service(true, 0.5);
    expect(cloak.value() > 0.58F && cloak.value() < 0.61F, "WST-12: characteristic time is not a linear completion time");
    for (int frame = 0; frame < 60; ++frame) cloak.service(true, 0.5);
    expect(cloak.value() > 0.99F, "WST-12: long cloak settles toward authored colour");
    cloak.service(false, 0.5);
    expect(cloak.value() > 0.98F, "WST-12: returning hull retains colour during the outward fade");
    for (int frame = 0; frame < 60; ++frame) cloak.service(false, 0.5);
    expect(cloak.value() < 0.01F, "WST-12: return settles below model tint threshold");
    cloak.service(true, 0);
    expect(cloak.value() == 1, "WST-12: zero transition is immediate");
    cloak.service(false, 0);
    expect(cloak.value() == 0, "WST-12: zero transition clears tint immediately");
    test_spawn_is_instant();
    test_projectile_observer_hide();
    test_reveal_and_hide_ease();
    test_flip_before_settling_does_not_restart();
    test_destroyed_unit_is_forgotten_at_once();
    test_paused_battle_holds();
    test_catch_up_matches_small_steps();
    test_extreme_elapsed_frames();
    test_flat_matches_reference();
    test_unsorted_inputs_are_normalised();
    test_opacity_walk_matches_opacity();
    if (failures != 0) {
        std::cerr << failures << " unit fade check(s) failed\n";
        return 1;
    }
    std::cout << "unit fade contracts passed\n";
    return 0;
}
