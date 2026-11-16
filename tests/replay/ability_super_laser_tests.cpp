#include "ability_fixtures.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

// #6248, WSPL-03/04/08: the authored SUPER_LASER aiming and firing controls. Each control is perturbed on its own and the
// first-damage tick, the facing or the release tick is compared with the rule. The lock is on tick 0; the fire stage is
// first tested on tick 1 (FS-54/FD-84), fires on the first tick k after the lock with k x clip rate / 30 >= Fire_Frame, and
// the damage service follows on the next tick.
namespace ability_tests {
namespace {

struct LaserCase {
    std::uint32_t delay{};
    bool face{}, can_turn{};
    Fixed turn_rate{units(10)}, cone{units(45)}, fire_frame{};
    std::uint32_t clip_frames{}, continuation{}, sub_index{};
    Fixed clip_fps{units(30)};
    std::int64_t target_x{100}, target_y{}; // the target stands at (target_x, target_y)
    std::int64_t target_hull{3600};
    std::uint32_t expiration{4};
    unsigned ticks{40};
};

struct LaserTrace {
    std::vector<Fixed> hull, yaw;
    std::vector<bool> active;
    std::vector<tactical::AbilityStatus> status; // the snapshot's published status of the super laser, per tick
    std::vector<std::string> hashes;
    [[nodiscard]] std::optional<std::size_t> first_damage(const Fixed start) const {
        for (std::size_t tick = 0; tick < hull.size(); ++tick) if (hull[tick] < start) return tick;
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::size_t> released() const { // the first tick the use is off again
        bool seen{};
        for (std::size_t tick = 0; tick < active.size(); ++tick) {
            seen = seen || active[tick];
            if (seen && !active[tick]) return tick;
        }
        return std::nullopt;
    }
};

LaserTrace run_laser(const LaserCase& scenario, const unsigned workers) {
    LaserTrace trace;
    auto initial = setup();
    initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
        {2, frigate_type, 2, at(scenario.target_x, scenario.target_y, 100), math::identity_quat(), {}}};
    auto health = durability();
    for (auto& row : health.profiles) {
        row.max_shields = {}; row.shield_refresh = {};
        if (row.type_id == frigate_type) row.max_hull = units(scenario.target_hull);
    }
    tactical::AbilityProfile ordinary;
    ordinary.kind = tactical::AbilityKind::super_laser; ordinary.recharge_frames = 20; ordinary.expiration_frames = scenario.expiration;
    ordinary.gui_activated_ability_name = "spha";
    auto holder = unit_of(corvette_type, ordinary);
    tactical::SpecialAbilityProfile nested;
    nested.name = "spha"; nested.style = tactical::SpecialActivationStyle::user_input;
    nested.kind = tactical::SpecialAbilityKind::super_laser;
    nested.service_interval = 1; nested.beam_max_range = units(300); nested.damage_per_frame = units(50);
    nested.fire_delay_frames = scenario.delay; nested.filter.applicable_categories = 2;
    nested.laser_face_target = scenario.face; nested.laser_can_turn = scenario.can_turn;
    nested.laser_turn_rate = scenario.turn_rate; nested.laser_firing_cone = scenario.cone;
    nested.laser_fire_frame = scenario.fire_frame; nested.laser_continuation_frames = scenario.continuation;
    nested.attack_clip_frames = scenario.clip_frames; nested.attack_clip_fps = scenario.clip_fps;
    nested.laser_anim_sub_index = scenario.sub_index;
    holder.special = {nested};
    tactical::AbilityTable table; table.profiles = {holder}; table.beam_damage_type = 0;
    tactical::CombatProfile source; source.type_id = corvette_type; source.category_bits = 1;
    tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
    tactical::CombatTable combat; combat.profiles = {source, target};
    auto created = tactical::TacticalSession::create(initial, {}, health, motion(), std::nullopt, combat, {}, table);
    expect(static_cast<bool>(created), "WSPL-08: the super laser fixture binds");
    if (!created) return trace;
    auto world = std::move(created).value();
    expect(static_cast<bool>(world.submit(command(0, 1, 0, 1,
        tactical::AbilityPayload{tactical::AbilityKind::super_laser, tactical::AbilityAction::activate, 2}))),
        "WSPL-08: the targeted super laser command submits");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (unsigned tick = 0; tick < scenario.ticks; ++tick) {
        const auto stepped = world.step(executor);
        expect(static_cast<bool>(stepped), "WSPL-08: the super laser tick succeeds");
        if (!stepped) break;
        trace.hashes.push_back(stepped.value().state_sha256);
        const auto hurt = world.durability_state(2);
        trace.hull.push_back(hurt ? hurt->hull : Fixed{});
        trace.active.push_back(world.ability_state(1)->slots.front().active);
        tactical::AbilityStatus published;
        for (const auto& instance : stepped.value().snapshot->instances())
            if (instance.entity_id == 1 && !instance.abilities.empty()) published = instance.abilities.front();
        trace.status.push_back(published);
        const auto yaw = tactical::yaw_degrees(world.unit_state(1)->rotation);
        trace.yaw.push_back(yaw ? yaw.value() : Fixed{});
    }
    return trace;
}

[[nodiscard]] bool near(const Fixed value, const std::int64_t degrees, const double tolerance = 0.02) {
    return std::abs(static_cast<double>(value.raw()) / static_cast<double>(one) - static_cast<double>(degrees)) < tolerance;
}

std::optional<std::size_t> damage_tick(const LaserCase& scenario) { return run_laser(scenario, 1).first_damage(units(3600)); }

void clip_gate() {
    // Fire_Frame on a 30 fps clip: fire at k = Fire_Frame (first test at tick 1), damage on the next tick.
    for (const std::int64_t frame : {0, 1, 10, 30}) {
        LaserCase scenario; scenario.clip_frames = 100; scenario.fire_frame = units(frame);
        const auto first = damage_tick(scenario);
        expect(first && *first == static_cast<std::size_t>(std::max<std::int64_t>(frame, 1) + 1),
            "WSPL-08: the first damage lands the tick after the clip reaches Fire_Frame (k = Fire_Frame fires, k - 1 does not)");
    }
    // A half-rate clip needs twice the ticks for the same frame: smallest k with k x 15 / 30 >= 5 is 10.
    LaserCase slow; slow.clip_frames = 100; slow.clip_fps = units(15); slow.fire_frame = units(5);
    expect(damage_tick(slow) == std::optional<std::size_t>{11}, "WSPL-08: the clip rate scales the Fire_Frame wait");
    // A model without the attack clip fires as soon as the aiming gate passes, whatever Fire_Frame says.
    LaserCase none; none.clip_frames = 0; none.fire_frame = units(10);
    expect(damage_tick(none) == std::optional<std::size_t>{2}, "WSPL-08: no attack clip means no Fire_Frame wait");
    // The clip is held two frames short of its end, so the last two frames are never reached.
    LaserCase last; last.clip_frames = 20; last.fire_frame = units(18);
    expect(damage_tick(last) == std::optional<std::size_t>{19}, "WSPL-08: the last playable frame fires");
    LaserCase beyond; beyond.clip_frames = 20; beyond.fire_frame = units(19); beyond.ticks = 80;
    const auto never = run_laser(beyond, 1);
    expect(!never.first_damage(units(3600)) && never.active.back(),
        "WSPL-08: a Fire_Frame beyond the last playable frame never fires and the use stays on");
    // The expiry timer starts at the fire tick: Fire_Frame 10 fires on tick 10 and three hits follow.
    LaserCase timed; timed.clip_frames = 100; timed.fire_frame = units(10); timed.ticks = 60;
    const auto served = run_laser(timed, 1);
    expect(served.hull.back() == units(3600 - 3 * 50), "WSPL-08: the expiry timer starts at the fire tick, not at the lock");
    // Fire_Delay keeps the same rule: the fire test waits for it.
    LaserCase delayed; delayed.delay = 6; delayed.clip_frames = 100; delayed.fire_frame = units(2);
    expect(damage_tick(delayed) == std::optional<std::size_t>{7}, "WSPL-03: Fire_Delay holds the fire test");
}

void aiming() {
    // The target is 90 degrees to the left of the unit's facing; the unit turns Turn_Rate degrees a frame (first turn on
    // tick 1) until it is inside the firing cone, starts the clip, stops turning, and fires the tick after.
    LaserCase scenario; scenario.face = true; scenario.can_turn = true; scenario.target_x = 0; scenario.target_y = 100; scenario.cone = units(15);
    scenario.ticks = 30;
    const auto fast = run_laser(scenario, 1);
    expect(fast.first_damage(units(3600)) == std::optional<std::size_t>{10}, "WSPL-04: the turn then the cone gate delay the fire stage");
    expect(near(fast.yaw[20], 80) && near(fast.yaw[29], 80), "WSPL-04: the unit stops turning once the cone is reached");
    expect(near(fast.yaw[3], 30), "WSPL-03: the unit turns Turn_Rate degrees a frame from the tick after the lock");
    auto slow = scenario; slow.turn_rate = units(5); slow.cone = units(17); slow.ticks = 40;
    const auto slower = run_laser(slow, 1);
    expect(slower.first_damage(units(3600)) == std::optional<std::size_t>{17} && near(slower.yaw[30], 75),
        "WSPL-04: Turn_Rate sets the turn time");
    auto wide = scenario; wide.cone = units(45);
    expect(run_laser(wide, 1).first_damage(units(3600)) == std::optional<std::size_t>{7},
        "WSPL-04: a wider Firing_Cone fires earlier");
    // A cone of 90 degrees or more is replaced by 0.01: the unit turns all the way round to the target.
    auto quirk = scenario; quirk.cone = units(95);
    const auto exact = run_laser(quirk, 1);
    expect(exact.first_damage(units(3600)) == std::optional<std::size_t>{11} && near(exact.yaw[29], 90),
        "WSPL-04: a Firing_Cone of 90 degrees or more acts as 0.01");
    // Turn_Rate zero or less turns at the unit's own rate of turn (1.5 degrees a frame in the fixture).
    auto own = scenario; own.turn_rate = units(-1); own.ticks = 70;
    const auto own_rate = run_laser(own, 1);
    expect(near(own_rate.yaw[10], 15) && own_rate.first_damage(units(3600)).has_value(), "WSPL-03: a non-positive Turn_Rate uses the unit's own rate");
    // Without Face_Target, or on a unit without the tactical-super-weapon behaviour, nothing turns.
    auto off = scenario; off.face = false;
    const auto still = run_laser(off, 1);
    expect(still.first_damage(units(3600)) == std::optional<std::size_t>{2} && near(still.yaw[29], 0),
        "WSPL-03: Face_Target off fires without a turn");
    auto inert = scenario; inert.can_turn = false;
    expect(run_laser(inert, 1).first_damage(units(3600)) == std::optional<std::size_t>{2},
        "WSPL-03: a unit without the behaviour never turns");
    // The clip starts when the cone is reached, and Fire_Frame counts from there.
    auto both = scenario; both.clip_frames = 100; both.fire_frame = units(5);
    expect(run_laser(both, 1).first_damage(units(3600)) == std::optional<std::size_t>{14},
        "WSPL-04: Fire_Frame counts from the clip start after the turn");
}

void continuation() {
    // The target dies to the second hit. Without Fire_Continuation the use ends when the death is noticed; with it the
    // use stays on, without damage, for trunc(seconds x 30) more frames.
    LaserCase scenario; scenario.target_hull = 100; scenario.expiration = 60; scenario.ticks = 40;
    const auto ended = run_laser(scenario, 1);
    auto kept = scenario; kept.continuation = 6;
    const auto continued = run_laser(kept, 1);
    expect(ended.released() && continued.released() && *continued.released() == *ended.released() + 6,
        "WSPL-08: Fire_Continuation keeps a fired use on for its frames after the target left");
    if (!(ended.released() && continued.released() && *continued.released() == *ended.released() + 6)) {
        std::cerr << "continuation trace (tick: hull active), no continuation then 6 frames:\n";
        for (const auto* trace : {&ended, &continued}) {
            for (std::size_t tick = 0; tick < 14 && tick < trace->hull.size(); ++tick)
                std::cerr << ' ' << tick << ':' << trace->hull[tick].raw() / one << (trace->active[tick] ? 'A' : '-');
            std::cerr << '\n';
        }
    }
}

void continuation_outlives_expiry() {
    // WSPL-09: Fire_Continuation counts from the moment the target left, whatever Expiration_Seconds says. Fire lands on
    // tick 1, the target dies on tick 3, the expiry timer (10 frames) would end the use on tick 11.
    LaserCase base; base.target_hull = 100; base.expiration = 10; base.ticks = 60;
    const auto ended = run_laser(base, 1);
    auto kept = base; kept.continuation = 30;
    const auto continued = run_laser(kept, 1);
    expect(ended.released() && continued.released() && *continued.released() == *ended.released() + 30,
        "WSPL-09: a Fire_Continuation longer than the remaining expiry runs its full frames");
    // A continuation shorter than the remaining expiry is unchanged by the timer rule.
    auto short_run = base; short_run.expiration = 60; short_run.continuation = 6;
    const auto short_ended = run_laser(short_run, 1);
    auto short_base = short_run; short_base.continuation = 0;
    expect(short_ended.released() && run_laser(short_base, 1).released()
        && *short_ended.released() == *run_laser(short_base, 1).released() + 6,
        "WSPL-09: a continuation inside the expiry window still ends on its own frame");
    // The expiry timer still ends a use whose target lives: no target left, so there is nothing to continue.
    LaserCase alive; alive.expiration = 4; alive.continuation = 30; alive.ticks = 60;
    auto alive_plain = alive; alive_plain.continuation = 0;
    expect(run_laser(alive, 1).released() && run_laser(alive, 1).released() == run_laser(alive_plain, 1).released(),
        "WSPL-09: the expiry timer ends a use whose target never left, whatever the continuation");
    // The class: the recharge starts when the continuation ends, not when the timer would have run out.
    for (const auto count : {2U, 4U, 8U})
        expect(run_laser(kept, count).hashes == continued.hashes,
            "WSPL-09: the continuation past the expiry hashes equal on 1/2/4/8 workers");
}

void published_phases() {
    using tactical::LaserPhase;
    // WSPL-08/09, #6554: the snapshot publishes where the use stands, for the viewer's clip and beam. No clip tick
    // or beam input is new sim state: they are read from the use state the fire stage keeps.
    LaserCase plain; plain.clip_frames = 100; plain.fire_frame = units(10); plain.sub_index = 1; plain.ticks = 30;
    const auto run = run_laser(plain, 1);
    expect(run.status[0].laser_phase == LaserPhase::clip && run.status[0].laser_clip_tick == 0,
        "#6554: without Face_Target the clip starts at the lock and the snapshot says so");
    const auto fired = std::find_if(run.status.begin(), run.status.end(), [](const auto& s) { return s.laser_phase == LaserPhase::fired; });
    const auto damage = run.first_damage(units(3600));
    expect(fired != run.status.end() && damage && fired > run.status.begin() && (fired - 1)->laser_phase == LaserPhase::clip
        && static_cast<std::size_t>(fired - run.status.begin()) + 1 == *damage,
        "#6554: the snapshot's phase changes from clip to fired on the fire tick, one tick before the first damage");
    const auto off = std::find_if(run.status.begin(), run.status.end(), [](const auto& s) { return s.active && s.laser_phase == LaserPhase::none; });
    expect(off == run.status.end(), "#6554: an active super laser always has a phase");
    expect(run.status.back().laser_phase == LaserPhase::none, "#6554: an ended use has no phase");
    // Aiming: the unit turns first, the clip starts at the tick the cone is reached.
    LaserCase turn; turn.face = true; turn.can_turn = true; turn.target_x = 0; turn.target_y = 100; turn.cone = units(15);
    turn.clip_frames = 100; turn.fire_frame = units(5); turn.ticks = 30;
    const auto aimed = run_laser(turn, 1);
    expect(aimed.status[1].laser_phase == LaserPhase::aiming, "#6554: a unit still turning is aiming");
    const auto began = std::find_if(aimed.status.begin(), aimed.status.end(), [](const auto& s) { return s.laser_phase == LaserPhase::clip; });
    expect(began != aimed.status.end() && began->laser_clip_tick > 0, "#6554: the clip starts after the turn");
    // Continuing: the target is gone, the snapshot says when the beam goes off.
    LaserCase death; death.target_hull = 100; death.expiration = 60; death.continuation = 12; death.ticks = 40;
    const auto ended = run_laser(death, 1);
    const auto continuing = std::find_if(ended.status.begin(), ended.status.end(), [](const auto& s) { return s.laser_phase == LaserPhase::continuing; });
    expect(continuing != ended.status.end() && continuing->target == eawr::sim::invalid_entity_id
        && continuing->laser_end_tick > 0, "#6554: a continuing beam publishes no target and its end tick");
    const auto first = static_cast<std::size_t>(continuing - ended.status.begin());
    expect(ended.status[first + 1].laser_phase == LaserPhase::continuing && ended.released().has_value()
        && continuing->laser_end_tick == *ended.released(),
        "#6554: the published continuation end is the tick the use releases");
}

void workers() {
    LaserCase turn; turn.face = true; turn.can_turn = true; turn.target_x = 0; turn.target_y = 100; turn.cone = units(15);
    turn.clip_frames = 100; turn.fire_frame = units(5); turn.ticks = 40;
    LaserCase death; death.target_hull = 100; death.expiration = 60; death.continuation = 6; death.ticks = 40;
    for (const auto& scenario : {turn, death}) {
        const auto reference = run_laser(scenario, 1);
        for (const auto count : {2U, 4U, 8U})
            expect(run_laser(scenario, count).hashes == reference.hashes, "WSPL-08: super laser state hashes equal on 1/2/4/8 workers");
    }
}

} // namespace

void test_super_laser_controls() {
    clip_gate();
    aiming();
    continuation();
    continuation_outlives_expiry();
    published_phases();
    workers();
}

} // namespace ability_tests
