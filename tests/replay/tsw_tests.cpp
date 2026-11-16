// TSW-02..TSW-11 (docs/behaviour/space-tactical-superweapon.md): the tactical super weapon's data, targets, first
// service, approach, ready state, ETA warnings, the switch command, the fire sequence (power-up, attack, destruction,
// debris, post wait, complete), the victory hold, the planet fact, its replay bytes, its checkpoint and its
// worker-count independence. Frame-exact against a plain integer oracle on an axis-aligned line, so the expectation
// shares no code with the service.
#include "../support/check.hpp"
#include "../support/self_test.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/tsw.hpp"
#include "eawr/sim/tactical/victory.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;

eawr::testing::Checker checker;

void expect(const bool condition, const std::string_view message) { checker.check(condition, message, std::cerr); }

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr tactical::TypeId tsw_type = 10;
constexpr tactical::TypeId enemy_type = 11;
constexpr eawr::sim::EntityId tsw_id = 1;
constexpr eawr::sim::EntityId enemy_id = 2;
constexpr tactical::TypeId station_type = 40;
constexpr eawr::sim::EntityId station_owner_one = 3;
constexpr eawr::sim::EntityId station_owner_two = 4;

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y = 0, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}

// The stock numbers: attack distance 5800, Max_Speed 0.75, Max_Thrust 0.02 (both per frame).
constexpr std::int64_t stock_attack = 5800;
constexpr std::int64_t stock_start = 8000;
constexpr std::int64_t speed_raw = one * 3 / 4;
constexpr std::int64_t thrust_raw = one / 50;

[[nodiscard]] tactical::TswProfile stock_profile(const std::int64_t attack = stock_attack) {
    tactical::TswProfile profile;
    profile.attack_distance = units(attack);
    profile.max_speed = Fixed::from_raw(speed_raw);
    profile.max_thrust = Fixed::from_raw(thrust_raw);
    profile.power_up_frames = 270;
    profile.attack_anim_frames = 75;
    profile.debris_delay_frames = 33;
    profile.post_destruction_wait_frames = 350;
    return profile;
}

[[nodiscard]] tactical::CombatTable combat_table(const tactical::TswProfile& profile) {
    tactical::CombatTable table;
    tactical::CombatProfile weapon;
    weapon.type_id = tsw_type;
    weapon.category_bits = 1U;
    weapon.tactical_super_weapon = profile;
    table.profiles = {weapon, tactical::CombatProfile{enemy_type, 2U, std::nullopt, std::nullopt, {}, {}, {}}};
    return table;
}

[[nodiscard]] std::vector<tactical::SensorProfile> sensors() {
    return {{tsw_type, units(2000)}, {enemy_type, units(2000)}};
}

[[nodiscard]] tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type,
    const tactical::PlayerId owner, const math::Vec3 position) {
    tactical::UnitState state;
    state.entity_id = id;
    state.type_id = type;
    state.owner = owner;
    state.position = position;
    return state;
}

struct Scenario {
    tactical::TacticalSetup setup;
    tactical::TswProfile profile{stock_profile()};
};

// Player 1 (faction 1) owns the Death Star at (start, 0, 0); player 2 (faction 2) owns a unit; the target props stand at
// the origin (faction 2) and, nearer to the Death Star but its own faction's, at (start - 100, 0, 0).
[[nodiscard]] Scenario scenario(const std::int64_t start = stock_start, const bool with_conflict = true,
    const bool destroyable = true, const tactical::PlayerId defender = 0, const tactical::PlayerId tsw_owner = 1) {
    Scenario result;
    auto& setup = result.setup;
    setup.seed = 7;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {unit(tsw_id, tsw_type, tsw_owner, at(tsw_owner == 1 ? start : -start)),
        unit(enemy_id, enemy_type, tsw_owner == 1 ? 2U : 1U, at(0, 4000, 0))};
    tactical::TswSetup tsw;
    const tactical::FactionId enemy_faction = tsw_owner == 1 ? 2 : 1;
    tsw.targets = {{1, enemy_faction, 0, at(0), {}}, {2, tsw_owner == 1 ? 1U : 2U, 0, at(tsw_owner == 1 ? start - 100 : -start + 100), {}}};
    if (with_conflict) tsw.conflict = tactical::TswConflictSetup{defender, destroyable, false};
    setup.tsw = tsw;
    return result;
}

[[nodiscard]] tactical::TacticalSession make(const Scenario& value) {
    auto created = tactical::TacticalSession::create(value.setup, sensors(), {}, {}, std::nullopt, combat_table(value.profile));
    expect(static_cast<bool>(created), "super weapon session is created");
    return std::move(created).value();
}

struct Tick {
    std::uint64_t tick{};
    std::vector<tactical::Event> events;
    std::string hash;
};

std::vector<Tick> run(tactical::TacticalSession& session, const std::uint64_t ticks, const std::size_t workers = 1) {
    std::vector<Tick> result;
    eawr::platform::ThreadWorkerAdapter executor(workers);
    for (std::uint64_t index = 0; index < ticks; ++index) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "super weapon step succeeds");
        if (!stepped) break;
        Tick row;
        row.tick = index;
        row.hash = stepped.value().state_sha256;
        for (const auto& event : stepped.value().snapshot->events()) row.events.push_back(event);
        result.push_back(std::move(row));
    }
    return result;
}

[[nodiscard]] math::Vec3 position_of(const tactical::TacticalSession& session, const eawr::sim::EntityId id) {
    for (const auto& instance : session.snapshot()->instances())
        if (instance.entity_id == id)
            return {instance.fixed_transform.rows[0][3], instance.fixed_transform.rows[1][3], instance.fixed_transform.rows[2][3]};
    expect(false, "unit has an instance");
    return {};
}

[[nodiscard]] std::optional<std::uint64_t> first_event(const std::vector<Tick>& ticks, const tactical::EventKind kind) {
    for (const auto& row : ticks)
        for (const auto& event : row.events) if (event.kind == kind) return row.tick;
    return std::nullopt;
}

void test_pure_rules() {
    using tactical::TswPhase;
    using tactical::TswStage;
    expect(!tactical::tsw_gate_open(std::nullopt), "TSW-04: no conflict, no gate");
    expect(!tactical::tsw_gate_open(tactical::TswConflictSetup{0, false, false}), "TSW-04: an undestroyable planet shuts the gate");
    expect(!tactical::tsw_gate_open(tactical::TswConflictSetup{0, true, true}), "TSW-04: a destroyed planet shuts the gate");
    expect(tactical::tsw_gate_open(tactical::TswConflictSetup{0, true, false}), "TSW-04: a standing destroyable planet opens it");
    expect(tactical::tsw_request_accepted(TswStage::start_approach, TswPhase::idle, true), "TSW-07: approach from idle");
    expect(!tactical::tsw_request_accepted(TswStage::start_approach, TswPhase::idle, false), "TSW-07: approach refused by the gate");
    for (const auto phase : {TswPhase::facing, TswPhase::approaching, TswPhase::ready})
        expect(!tactical::tsw_request_accepted(TswStage::start_approach, phase, true), "TSW-07: approach only from idle");
    expect(tactical::tsw_request_accepted(TswStage::start_power_up, TswPhase::ready, true), "TSW-07: power-up from ready");
    for (const auto phase : {TswPhase::idle, TswPhase::facing, TswPhase::approaching, TswPhase::powering_up})
        expect(!tactical::tsw_request_accepted(TswStage::start_power_up, phase, true), "TSW-07: power-up only from ready");

    tactical::TswSetup setup;
    setup.targets = {{1, 5, 0, at(100), {}}, {2, 7, 0, at(-100), {}}, {3, 7, 0, at(0, 90), {}}};
    const auto from = at(0);
    // The equal-distance targets 1 and 2 tie below target 3's nearer point: the lower index wins.
    auto picked = tactical::tsw_select_target(setup, 0, at(0, 200));
    expect(picked && picked.value() == 3, "TSW-03: nearest target wins");
    picked = tactical::tsw_select_target(setup, 0, from);
    expect(picked && picked.value() == 3, "TSW-03: nearest of three");
    picked = tactical::tsw_select_target(setup, 7, from);
    expect(picked && picked.value() == 1, "TSW-03: the object's own faction is skipped");
    picked = tactical::tsw_select_target(setup, 5, at(0, 1000));
    expect(picked && picked.value() == 3, "TSW-03: a different faction qualifies");
    tactical::TswSetup tie;
    tie.targets = {{1, 5, 0, at(100), {}}, {2, 5, 0, at(-100), {}}};
    picked = tactical::tsw_select_target(tie, 0, from);
    expect(picked && picked.value() == 1, "TSW-03: the lower record wins an equal distance");
    picked = tactical::tsw_select_target(tie, 5, from);
    expect(picked && picked.value() == 0, "TSW-03: no target outside the own faction");

    const auto profile = stock_profile();
    auto step = tactical::tsw_approach_step(profile, at(8000), Fixed{}, at(0));
    expect(step && !step.value().in_range && step.value().speed.raw() == thrust_raw
            && step.value().position.x.raw() == units(8000).raw() - thrust_raw && step.value().position.y.raw() == 0,
        "TSW-05: the first approach frame moves by Max_Thrust");
    step = tactical::tsw_approach_step(profile, at(8000), Fixed::from_raw(speed_raw - 1), at(0));
    expect(step && step.value().speed.raw() == speed_raw, "TSW-05: the speed is capped at Max_Speed");
    step = tactical::tsw_approach_step(profile, at(5800), Fixed::from_raw(speed_raw), at(0));
    expect(step && step.value().in_range && step.value().position == at(5800), "TSW-05: at the attack distance it is in range and does not move");
    step = tactical::tsw_approach_step(profile, at(5800, 0, 0), Fixed::from_raw(speed_raw), at(0, 0, 0));
    expect(step && step.value().in_range, "TSW-05: the stop test is distance <= attack distance");
    step = tactical::tsw_approach_step(profile, math::Vec3{Fixed::from_raw(units(5800).raw() + 1), Fixed{}, Fixed{}}, Fixed{}, at(0));
    expect(step && !step.value().in_range, "TSW-05: just outside the attack distance it still flies");
    expect(tactical::tsw_decelerate(profile, Fixed::from_raw(speed_raw)).raw() == speed_raw - thrust_raw, "TSW-05: ready slows by Max_Thrust");
    expect(tactical::tsw_decelerate(profile, Fixed::from_raw(5)).raw() == 0, "TSW-05: ready stops at zero");

    auto eta = tactical::tsw_eta_frames(profile, at(8000), at(0));
    // 2200 units at 0.75 per frame: 2933.33 frames, truncated.
    expect(eta && eta.value() == 2933, "TSW-06: the ETA is the distance to range over Max_Speed in whole frames");
    eta = tactical::tsw_eta_frames(profile, at(5800), at(0));
    expect(eta && eta.value() == 0, "TSW-06: in range the ETA is zero");
    auto frozen = profile;
    frozen.max_speed = Fixed{};
    eta = tactical::tsw_eta_frames(frozen, at(8000), at(0));
    expect(eta && eta.value() == std::numeric_limits<std::uint64_t>::max(), "TSW-06: a type that cannot move never arrives");

    std::vector<std::uint32_t> announced;
    auto mask = tactical::tsw_speech_crossings(1801, 0, announced);
    expect(mask == 0 && announced.empty(), "TSW-06: above 60 s nothing is announced");
    mask = tactical::tsw_speech_crossings(1800, mask, announced);
    expect(mask == 1U && announced == std::vector<std::uint32_t>({60}), "TSW-06: 60 s is announced at 1800 frames");
    announced.clear();
    mask = tactical::tsw_speech_crossings(1800, mask, announced);
    expect(announced.empty(), "TSW-06: a threshold is announced once");
    mask = tactical::tsw_speech_crossings(100, mask, announced);
    expect(mask == 0xFU && announced == std::vector<std::uint32_t>({30, 15, 5}), "TSW-06: later thresholds, descending");
    announced.clear();
    const auto all = tactical::tsw_speech_crossings(0, 0, announced);
    expect(all == 0xFU && announced == std::vector<std::uint32_t>({60, 30, 15, 5}), "TSW-06: all four descending when already in range");
}

// The oracle: axis-aligned line, integer arithmetic, no shared code.
struct Flight {
    std::uint64_t arrival_tick{}; // the tick whose service finds the object in range
    std::vector<std::int64_t> x; // raw x after each tick's service, index = tick
};
[[nodiscard]] Flight oracle(const std::int64_t start, const std::int64_t attack, const std::uint64_t ticks) {
    Flight flight;
    std::int64_t x = start * one, speed = 0;
    const std::int64_t range = attack * one;
    bool arrived = false;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        if (tick >= 1 && !arrived) {
            if (x <= range) { arrived = true; flight.arrival_tick = tick; }
            else {
                speed = std::min(speed + thrust_raw, speed_raw);
                x -= speed;
            }
        } else if (arrived) {
            // Ready: the speed falls by Max_Thrust and the object still moves while it is above zero.
            speed = std::max<std::int64_t>(speed - thrust_raw, 0);
            x -= speed;
        }
        flight.x.push_back(x);
    }
    return flight;
}

void test_attacker_flight() {
    auto value = scenario(stock_start);
    auto session = make(value);
    expect(session.tsw_state(tsw_id).has_value() && !session.tsw_state(enemy_id).has_value(),
        "TSW-02: the super weapon object has a record, another unit has none");
    expect(session.tsw_state(tsw_id)->phase == tactical::TswPhase::idle && !session.tsw_state(tsw_id)->serviced,
        "TSW-02: a new record is idle and unserviced");
    constexpr std::uint64_t horizon = 3200;
    const auto expected = oracle(stock_start, stock_attack, horizon);
    expect(expected.arrival_tick > 2900 && expected.arrival_tick < 3000, "oracle: about 98 seconds of flight");
    eawr::platform::ThreadWorkerAdapter executor(1);
    std::vector<Tick> ticks;
    std::uint64_t checked = 0;
    for (std::uint64_t index = 0; index < horizon; ++index) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "attacker step succeeds");
        if (!stepped) return;
        Tick row;
        row.tick = index;
        for (const auto& event : stepped.value().snapshot->events()) row.events.push_back(event);
        ticks.push_back(std::move(row));
        const auto record = session.tsw_state(tsw_id);
        expect(record.has_value(), "TSW-02: the record persists");
        if (index == 0) {
            expect(record->serviced && record->target == 1,
                "TSW-03: the first service picks the nearest enemy-faction target, not the nearer one of its own faction");
            expect(record->phase == tactical::TswPhase::approaching,
                "TSW-03/05: the attacker starts the approach at once, turns, and flies from the next frame");
        }
        if (index < 400 || index + 60 > expected.arrival_tick) {
            const auto position = position_of(session, tsw_id);
            if (position.x.raw() != expected.x[index] || position.y.raw() != 0 || position.z.raw() != 0) ++checked;
        }
    }
    expect(checked == 0, "TSW-05: the position matches the integer oracle every frame (ramp and arrival)");
    const auto started = first_event(ticks, tactical::EventKind::tsw_approach_started);
    expect(started && *started == 0, "TSW-03: the approach-started event is published on the first tick");
    std::uint32_t starts = 0;
    for (const auto& row : ticks) for (const auto& event : row.events) starts += event.kind == tactical::EventKind::tsw_approach_started ? 1U : 0U;
    expect(starts == 1, "TSW-03: it is published once");
    const auto in_range = first_event(ticks, tactical::EventKind::tsw_in_range);
    expect(in_range && *in_range == expected.arrival_tick, "TSW-05: the in-range event is on the oracle's arrival tick");
    expect(session.tsw_state(tsw_id)->phase == tactical::TswPhase::ready, "TSW-05: the object is ready in range");
    expect(position_of(session, tsw_id).x.raw() <= units(stock_attack).raw()
            && position_of(session, tsw_id).x.raw() > units(stock_attack).raw() - units(40).raw(),
        "TSW-05: it stops within the 38-frame slowing run of the attack distance");
    expect(position_of(session, tsw_id).x.raw() == expected.x[horizon - 1], "TSW-05: the final position matches the oracle");
    // TSW-06: each warning lands when the remaining flight first fits the threshold (within one frame of the oracle).
    std::vector<std::pair<std::uint32_t, std::uint64_t>> speech;
    for (const auto& row : ticks)
        for (const auto& event : row.events)
            if (event.kind == tactical::EventKind::tsw_speech_threshold) speech.emplace_back(static_cast<std::uint32_t>(event.sequence), row.tick);
    expect(speech.size() == 4 && speech[0].first == 60 && speech[1].first == 30 && speech[2].first == 15 && speech[3].first == 5,
        "TSW-06: four warnings, 60, 30, 15, 5, in order, once each");
    if (speech.size() == 4) {
        for (const auto& [seconds, tick] : speech) {
            const auto before = expected.arrival_tick - tick;
            const auto want = static_cast<std::uint64_t>(seconds) * 30U;
            expect(before + 2 >= want && before <= want + 2, "TSW-06: a warning is " + std::to_string(seconds) + " s before arrival");
        }
    }
}

void test_ready_deceleration_and_hold() {
    auto value = scenario(700, true, true, 0);
    value.profile = stock_profile(500);
    auto session = make(value);
    const auto expected = oracle(700, 500, 1200);
    const auto ticks = run(session, expected.arrival_tick + 60);
    expect(position_of(session, tsw_id).x.raw() == expected.x[expected.arrival_tick + 59],
        "TSW-05: ready still moves while it slows, to the oracle's rest point");
    expect(expected.x[expected.arrival_tick + 59] < expected.x[expected.arrival_tick], "oracle: ready moves");
    expect(session.tsw_state(tsw_id)->speed.raw() == 0, "TSW-05: the speed reaches zero within 38 ready frames");
    auto again = make(value);
    run(again, expected.arrival_tick + 10);
    // The object is ready from the arrival tick; the nine ticks after it are nine ready frames.
    expect(again.tsw_state(tsw_id)->speed.raw() == speed_raw - thrust_raw * 9, "TSW-05: the speed falls by Max_Thrust per ready frame");
    static_cast<void>(ticks);
}

void test_facing() {
    // TSW-05: the facing state points +X at the target in full 3D (heading and elevation, no roll).
    const std::vector<std::pair<math::Vec3, math::Vec3>> cases = {
        {at(8000), at(0)}, {at(0, 5000), at(0)}, {at(-3000, 4000, 0), at(0, 0, 1200)}, {at(8000, 0, 2000), at(0, 0, -300)},
        {at(0, 0, 0), at(0, 0, 900)}, {at(100, 100, 100), at(-4000, 2500, 1700)}};
    for (const auto& [from, to] : cases) {
        const auto facing = tactical::tsw_facing(from, to);
        expect(facing && facing.value().has_value(), "TSW-05: a facing exists between distinct points");
        if (!facing || !facing.value()) continue;
        const auto matrix = math::to_matrix(*facing.value(), {});
        expect(static_cast<bool>(matrix), "TSW-05: the facing is a rotation");
        if (!matrix) continue;
        const auto forward = math::transform_vector(matrix.value(), at(1));
        const auto delta = math::Vec3{Fixed::from_raw(to.x.raw() - from.x.raw()), Fixed::from_raw(to.y.raw() - from.y.raw()),
            Fixed::from_raw(to.z.raw() - from.z.raw())};
        const auto wanted = math::normalize(delta);
        expect(forward && wanted, "TSW-05: forward and wanted directions exist");
        if (!forward || !wanted) continue;
        const auto close = [](const Fixed a, const Fixed b) { return std::abs(a.raw() - b.raw()) < one / 500; };
        expect(close(forward.value().x, wanted.value().x) && close(forward.value().y, wanted.value().y)
                && close(forward.value().z, wanted.value().z),
            "TSW-05: the facing's forward axis points at the target");
        // No roll: the facing's lateral axis stays level.
        const auto side = math::transform_vector(matrix.value(), at(0, 1));
        expect(side && std::abs(side.value().z.raw()) < one / 500, "TSW-05: the facing keeps no roll");
    }
    const auto none = tactical::tsw_facing(at(1, 2, 3), at(1, 2, 3));
    expect(none && !none.value().has_value(), "TSW-05: coinciding points give no facing");
    // In the session the object faces its target at once, then flies.
    auto value = scenario(stock_start);
    value.setup.tsw->targets[0].position = at(0, 0, 1200);
    auto session = make(value);
    run(session, 1);
    for (const auto& instance : session.snapshot()->instances()) {
        if (instance.entity_id != tsw_id) continue;
        const auto forward = math::transform_vector(instance.fixed_transform, at(1));
        expect(forward && forward.value().x.raw() < 0 && forward.value().z.raw() > 0,
            "TSW-05: after the facing frame the object points toward and up at a raised target");
    }
}

void test_defender_and_no_target() {
    // A defending owner's object starts ready at its attack distance and never approaches (TSW-03).
    auto value = scenario(stock_attack, true, true, 1);
    auto session = make(value);
    const auto ticks = run(session, 40);
    expect(session.tsw_state(tsw_id)->phase == tactical::TswPhase::ready && session.tsw_state(tsw_id)->target == 1,
        "TSW-03: a defender's object is ready with its target");
    expect(!first_event(ticks, tactical::EventKind::tsw_approach_started) && !first_event(ticks, tactical::EventKind::tsw_in_range),
        "TSW-03: a defender publishes no approach");
    expect(position_of(session, tsw_id).x.raw() == units(stock_attack).raw(), "TSW-03: a defender does not move");

    // No qualifying target: serviced, idle, motionless.
    auto empty = scenario(stock_start);
    empty.setup.tsw->targets.clear();
    auto idle = make(empty);
    run(idle, 30);
    expect(idle.tsw_state(tsw_id)->serviced && idle.tsw_state(tsw_id)->phase == tactical::TswPhase::idle
            && idle.tsw_state(tsw_id)->target == 0 && position_of(idle, tsw_id).x.raw() == units(stock_start).raw(),
        "TSW-03: with no target the object stays idle where it is");

    // The object's own faction's targets never qualify.
    auto own = scenario(stock_start);
    own.setup.tsw->targets = {{1, 1, 0, at(0), {}}};
    auto none = make(own);
    run(none, 5);
    expect(none.tsw_state(tsw_id)->target == 0 && none.tsw_state(tsw_id)->phase == tactical::TswPhase::idle,
        "TSW-03: a target of the object's own faction is skipped");

    // A skirmish has no defender: owner 1 with no conflict approaches like an attacker, and the gate is shut.
    auto skirmish = scenario(stock_start, false);
    auto flying = make(skirmish);
    run(flying, 5);
    expect(flying.tsw_state(tsw_id)->phase == tactical::TswPhase::approaching, "TSW-03: a skirmish object approaches (no defender)");
}

[[nodiscard]] tactical::PlayerCommand request(const std::uint64_t tick, const std::uint64_t sequence,
    const tactical::PlayerId player, const tactical::TswStage stage,
    const tactical::TswAdmission admission = tactical::TswAdmission::player) {
    return {{tick, player, sequence}, {tsw_id}, tactical::TacticalSuperweaponPayload{stage, admission}};
}

[[nodiscard]] std::vector<tactical::Event> command_events(const std::vector<Tick>& ticks, const tactical::OrderKind kind) {
    std::vector<tactical::Event> result;
    for (const auto& row : ticks)
        for (const auto& event : row.events) if (event.order == kind) result.push_back(event);
    return result;
}

void test_commands() {
    // An accepted request at tick 0 is the first service: same flight, same state as no request.
    auto value = scenario(700);
    value.profile = stock_profile(500);
    auto plain = make(value);
    const auto plain_ticks = run(plain, 120);
    auto commanded = make(value);
    expect(static_cast<bool>(commanded.submit(request(0, 1, 1, tactical::TswStage::start_approach))), "TSW-07: the request is submitted");
    const auto commanded_ticks = run(commanded, 120);
    const auto events = command_events(commanded_ticks, tactical::OrderKind::tactical_superweapon);
    expect(events.size() == 1 && events.front().kind == tactical::EventKind::order_accepted && events.front().unit == tsw_id,
        "TSW-07: an owner's approach request on an idle object with the gate open is accepted");
    std::uint32_t starts = 0;
    for (const auto& row : commanded_ticks) for (const auto& event : row.events) starts += event.kind == tactical::EventKind::tsw_approach_started ? 1U : 0U;
    expect(starts == 1, "TSW-07: the approach starts once when a request precedes the first service");
    expect(plain_ticks.back().hash == commanded_ticks.back().hash, "TSW-07: a request at tick 0 gives the state of the first service");

    // Refusals.
    struct Case { const char* name; Scenario scenario; tactical::PlayerCommand command; tactical::RejectReason reason; };
    auto shut = scenario(700, true, false);
    shut.profile = stock_profile(500);
    auto none = scenario(700, false);
    none.profile = stock_profile(500);
    std::vector<Case> cases;
    cases.push_back({"the gate shut by an undestroyable planet", shut, request(0, 1, 1, tactical::TswStage::start_approach), tactical::RejectReason::ability_unavailable});
    cases.push_back({"no conflict (a skirmish)", none, request(0, 1, 1, tactical::TswStage::start_approach), tactical::RejectReason::ability_unavailable});
    cases.push_back({"a non-owner", value, request(0, 1, 2, tactical::TswStage::start_approach), tactical::RejectReason::unit_not_owned});
    cases.push_back({"power-up from idle", value, request(0, 1, 1, tactical::TswStage::start_power_up), tactical::RejectReason::ability_unavailable});
    for (auto& row : cases) {
        auto session = make(row.scenario);
        expect(static_cast<bool>(session.submit(row.command)), std::string("TSW-07: ") + row.name + ": submitted");
        const auto ticks = run(session, 3);
        const auto refused = command_events(ticks, tactical::OrderKind::tactical_superweapon);
        expect(refused.size() == 1 && refused.front().kind == tactical::EventKind::order_rejected && refused.front().reason == row.reason,
            std::string("TSW-07: refused: ") + row.name);
        // The first service is not a request: the gate does not stop the flight.
        expect(session.tsw_state(tsw_id)->phase == tactical::TswPhase::approaching,
            std::string("TSW-04: the first-service approach is not gated: ") + row.name);
    }

    // A repeated or out-of-phase request is a no-op with a reason; a stage-2 request is refused until the sequence is serviced.
    auto flying = make(value);
    expect(static_cast<bool>(flying.submit(request(5, 1, 1, tactical::TswStage::start_approach))), "TSW-07: a mid-approach request is submitted");
    expect(static_cast<bool>(flying.submit(request(6, 2, 1, tactical::TswStage::start_power_up))), "TSW-07: a mid-approach power-up request is submitted");
    const auto mid = run(flying, 10);
    const auto refused = command_events(mid, tactical::OrderKind::tactical_superweapon);
    expect(refused.size() == 2 && refused[0].kind == tactical::EventKind::order_rejected && refused[1].kind == tactical::EventKind::order_rejected,
        "TSW-07: approach and power-up requests while approaching are refused");

    const auto arrival = oracle(700, 500, 400).arrival_tick;
    auto ready = make(value);
    expect(static_cast<bool>(ready.submit(request(arrival + 5, 1, 1, tactical::TswStage::start_power_up))), "TSW-07: a ready power-up request is submitted");
    const auto at_ready = run(ready, arrival + 8);
    const auto ready_events = command_events(at_ready, tactical::OrderKind::tactical_superweapon);
    expect(ready_events.size() == 1 && ready_events.front().kind == tactical::EventKind::order_accepted
            && ready.tsw_state(tsw_id)->phase == tactical::TswPhase::powering_up && ready.tsw_world().victory_hold == 1,
        "TSW-08: a ready object accepts the power-up request: it powers up and the victory hold is one");
    expect(first_event(at_ready, tactical::EventKind::tsw_power_up_started) == arrival + 5,
        "TSW-08: the power-up-started event is published on the request's tick");

    // A human wire packet cannot carry the AI admission (TSW-07).
    expect(!tactical::human_wire_payload(tactical::TacticalSuperweaponPayload{tactical::TswStage::start_approach, tactical::TswAdmission::ai_object}),
        "TSW-07: the AI admission is not a human wire command");
    expect(tactical::human_wire_payload(tactical::TacticalSuperweaponPayload{tactical::TswStage::start_approach, tactical::TswAdmission::player}),
        "TSW-07: the player admission is");
}

void test_defender_request_before_first_service() {
    // TSW-03/TSW-07: a defender's object is ready from its first service, so an approach request that reaches it before
    // that service (a battle's first tick runs commands first) is refused: nothing turns, nothing is announced.
    auto value = scenario(stock_attack, true, true, 1);
    auto session = make(value);
    expect(static_cast<bool>(session.submit(request(0, 1, 1, tactical::TswStage::start_approach))), "TSW-07: the defender's request is submitted");
    const auto ticks = run(session, 5);
    const auto events = command_events(ticks, tactical::OrderKind::tactical_superweapon);
    expect(events.size() == 1 && events.front().kind == tactical::EventKind::order_rejected
            && events.front().reason == tactical::RejectReason::ability_unavailable,
        "TSW-07: a defender's approach request before the first service is refused");
    expect(!first_event(ticks, tactical::EventKind::tsw_approach_started), "TSW-07: no approach event for a defender's refused request");
    expect(session.tsw_state(tsw_id)->phase == tactical::TswPhase::ready && session.tsw_state(tsw_id)->target == 1,
        "TSW-03: the defender is ready after the first service, as without a request");
    auto plain = make(value);
    const auto plain_ticks = run(plain, 5);
    expect(plain.tsw_state(tsw_id) == session.tsw_state(tsw_id), "TSW-07: the refused request leaves the object's state alone");
    static_cast<void>(plain_ticks);
}

void test_speech_gate() {
    // TSW-06 (debug build walk): the threshold check runs only while the speed equals Max_Speed. A short approach passes
    // the 60, 30 and 15 second marks during the thrust ramp: nothing is announced until the first cruise frame.
    auto value = scenario(700);
    value.profile = stock_profile(500);
    auto session = make(value);
    eawr::platform::ThreadWorkerAdapter executor(1);
    std::optional<std::uint64_t> cruise;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> speech;
    for (std::uint64_t index = 0; index < 600; ++index) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "speech gate step succeeds");
        if (!stepped) return;
        if (!cruise && session.tsw_state(tsw_id)->speed.raw() == speed_raw) cruise = index;
        for (const auto& event : stepped.value().snapshot->events())
            if (event.kind == tactical::EventKind::tsw_speech_threshold) speech.emplace_back(index, static_cast<std::uint32_t>(event.sequence));
    }
    expect(cruise.has_value() && *cruise > 30, "the object reaches cruise speed after the ramp");
    expect(speech.size() == 4 && cruise && speech.front().first == *cruise,
        "TSW-06: the passed thresholds are announced on the first cruise frame, not during the ramp");
    if (speech.size() == 4)
        expect(speech[0].second == 60 && speech[1].second == 30 && speech[2].second == 15 && speech[3].second == 5 && speech[3].first > speech[2].first,
            "TSW-06: in order, the 5 second mark later");
}

void test_replay_and_checkpoint() {
    auto value = scenario(700);
    value.profile = stock_profile(500);
    tactical::TacticalReplay replay;
    replay.setup = value.setup;
    replay.final_tick_count = 100;
    replay.commands = {request(2, 1, 1, tactical::TswStage::start_approach),
        request(3, 2, 1, tactical::TswStage::start_power_up, tactical::TswAdmission::ai_object)};
    const auto encoded = tactical::write_replay(replay);
    expect(static_cast<bool>(encoded), "TSW: the replay with a super weapon setup writes");
    if (!encoded) return;
    const auto parsed = tactical::parse_replay(encoded.value());
    expect(parsed && parsed.value() == replay, "TSW: the replay round-trips (targets, conflict gate, defender, both stages, both admissions)");
    expect(parsed && parsed.value().setup.tsw && parsed.value().setup.tsw->targets.size() == 2
            && parsed.value().setup.tsw->conflict && parsed.value().setup.tsw->conflict->planet_destroyable,
        "TSW: the parsed setup carries the target list and the gate");
    for (const bool conflict : {false, true}) for (const tactical::PlayerId defender : {0U, 2U}) {
        auto variant = replay;
        variant.setup.tsw->conflict = conflict ? std::optional(tactical::TswConflictSetup{defender, false, true}) : std::nullopt;
        if (!conflict && defender != 0) continue;
        const auto bytes = tactical::write_replay(variant);
        const auto back = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(back && back.value() == variant, "TSW: every conflict form round-trips");
    }
    auto plain = replay;
    plain.setup.tsw.reset();
    plain.commands.clear();
    const auto plain_bytes = tactical::write_replay(plain);
    expect(plain_bytes && encoded && plain_bytes.value().size() < encoded.value().size(),
        "TSW: a battle without the setup writes no extension");

    // Checkpoint: a save mid-approach resumes equal to the uninterrupted run, at every worker count.
    auto live = make(value);
    run(live, 60);
    const auto saved = live.save_checkpoint_state();
    expect(static_cast<bool>(saved), "TSW: the checkpoint saves");
    if (!saved) return;
    auto resumed = make(value);
    const auto loaded = resumed.load_checkpoint_state(saved.value());
    expect(static_cast<bool>(loaded), "TSW: the checkpoint loads");
    if (!loaded) return;
    expect(resumed.tsw_state(tsw_id) == live.tsw_state(tsw_id) && resumed.state_sha256() == live.state_sha256(),
        "TSW: the loaded state equals the saved state");
    const auto straight = run(live, 320);
    const auto after = run(resumed, 320);
    bool equal = straight.size() == after.size();
    for (std::size_t index = 0; equal && index < straight.size(); ++index) equal = straight[index].hash == after[index].hash;
    expect(equal, "TSW: resume equals the uninterrupted run");
    expect(live.tsw_state(tsw_id)->phase == tactical::TswPhase::ready, "TSW: the saved run reaches the ready phase");

    // A record naming a phase beyond the state machine, a sequence phase without the matching hold, or a target outside
    // the list is refused: the record is found by its bytes (map size 1, object 1, approaching, serviced).
    const std::vector<std::uint8_t> pattern{1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 1};
    const auto& state_bytes = saved.value();
    std::size_t matches = 0;
    bool phase_refused = false, target_refused = false, hold_refused = false;
    for (std::size_t offset = 0; offset + pattern.size() + 4 < state_bytes.size(); ++offset) {
        if (!std::equal(pattern.begin(), pattern.end(), state_bytes.begin() + static_cast<std::ptrdiff_t>(offset))) continue;
        ++matches;
        auto phase_copy = state_bytes;
        phase_copy[offset + 16] = static_cast<std::uint8_t>(tactical::TswPhase::complete) + 1;
        auto probe = make(value);
        if (!probe.load_checkpoint_state(phase_copy)) phase_refused = true;
        auto hold_copy = state_bytes;
        hold_copy[offset + 16] = static_cast<std::uint8_t>(tactical::TswPhase::powering_up);
        auto held = make(value);
        if (!held.load_checkpoint_state(hold_copy)) hold_refused = true;
        auto target_copy = state_bytes;
        target_copy[offset + 18] = 9;
        auto other = make(value);
        if (!other.load_checkpoint_state(target_copy)) target_refused = true;
    }
    expect(matches >= 1, "TSW: the checkpoint carries the record");
    expect(phase_refused, "TSW: a record in a phase beyond the state machine is refused");
    expect(hold_refused, "TSW: a sequence record without the victory hold that goes with it is refused");
    expect(target_refused, "TSW: a record naming a target outside the list is refused");
}

void test_workers_and_battles_without_the_setup() {
    auto value = scenario(700);
    value.profile = stock_profile(500);
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto session = make(value);
        expect(static_cast<bool>(session.submit(request(3, 1, 1, tactical::TswStage::start_approach))), "TSW: submitted");
        const auto ticks = run(session, 400, workers);
        std::vector<std::string> hashes;
        for (const auto& row : ticks) hashes.push_back(row.hash);
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "TSW: the state hashes match at workers 1/2/4/8");
    }
    // Without the setup the behaviour is inert and the state carries nothing (the other battles keep their bytes).
    auto inert = value;
    inert.setup.tsw.reset();
    auto session = make(inert);
    const auto before = position_of(session, tsw_id);
    run(session, 50);
    expect(!session.tsw_state(tsw_id).has_value() && position_of(session, tsw_id) == before,
        "TSW-02: a battle without a super weapon setup keeps the object inert");
    // A type without the behaviour never gets a record.
    auto other = value;
    other.setup.units = {unit(enemy_id, enemy_type, 2, at(0, 4000, 0))};
    auto lone = make(other);
    run(lone, 5);
    expect(!lone.tsw_state(enemy_id).has_value(), "TSW-02: only the behaviour's object has a record");
}


// ---- The fire sequence (TSW-08..TSW-11) -----------------------------------------------------------------------------

// The oracle: the tick of every step of the sequence from the request tick `e` and the four numbers, by plain integer
// arithmetic (a timed phase moves on in the frame its countdown reaches zero, the other phases last one frame; the
// request's own tick counts the first power-up frame).
struct Timeline {
    std::uint64_t charged{}, attack_start{}, attack_started{}, destroyed{}, debris{}, complete{}, finish{};
};
[[nodiscard]] Timeline timeline(const std::uint64_t e, const tactical::TswProfile& profile) {
    const std::uint64_t power = profile.power_up_frames, attack = profile.attack_anim_frames, delay = profile.debris_delay_frames,
        wait = profile.post_destruction_wait_frames;
    Timeline line;
    line.charged = e + (power > 0 ? power - 1 : 0);
    line.attack_start = line.charged + 1;
    line.attack_started = line.attack_start + 1;
    line.destroyed = line.attack_started + attack + 1;
    line.debris = line.destroyed + delay;
    line.complete = line.debris + wait;
    line.finish = line.complete + 1;
    return line;
}

[[nodiscard]] std::vector<std::uint64_t> event_ticks(const std::vector<Tick>& ticks, const tactical::EventKind kind) {
    std::vector<std::uint64_t> result;
    for (const auto& row : ticks)
        for (const auto& event : row.events) if (event.kind == kind) result.push_back(row.tick);
    return result;
}

[[nodiscard]] tactical::TswProfile short_profile() {
    auto profile = stock_profile();
    profile.power_up_frames = 20;
    profile.attack_anim_frames = 10;
    profile.debris_delay_frames = 6;
    profile.post_destruction_wait_frames = 12;
    return profile;
}

void test_sequence_rules() {
    using tactical::TswAction;
    using tactical::TswPhase;
    const auto profile = stock_profile();
    // The pure step: every timed phase lowers its countdown by one and moves on in the frame it reaches zero.
    auto step = tactical::tsw_sequence_step(profile, TswPhase::powering_up, 2);
    expect(step.phase == TswPhase::powering_up && step.countdown == 1 && step.action == TswAction::none, "TSW-08: S4 counts down");
    step = tactical::tsw_sequence_step(profile, TswPhase::powering_up, 1);
    expect(step.phase == TswPhase::charged && step.countdown == 0, "TSW-08: S4 ends in the frame its countdown reaches zero");
    step = tactical::tsw_sequence_step(profile, TswPhase::powering_up, 0);
    expect(step.phase == TswPhase::charged, "TSW-08: a zero countdown moves on in its first frame");
    step = tactical::tsw_sequence_step(profile, TswPhase::charged, 0);
    expect(step.phase == TswPhase::attack_start && step.action == TswAction::none, "TSW-08: S5 lasts one frame");
    step = tactical::tsw_sequence_step(profile, TswPhase::attack_start, 0);
    expect(step.phase == TswPhase::attack_anim && step.countdown == 75 && step.action == TswAction::attack_started,
        "TSW-08: S6 starts the attack and loads the animation countdown");
    step = tactical::tsw_sequence_step(profile, TswPhase::attack_anim, 1);
    expect(step.phase == TswPhase::target_destruction && step.action == TswAction::none, "TSW-08: S7 ends at zero");
    step = tactical::tsw_sequence_step(profile, TswPhase::target_destruction, 0);
    expect(step.phase == TswPhase::debris_wait && step.countdown == 33 && step.action == TswAction::destroy_target,
        "TSW-10: S8 destroys the target and loads the debris delay");
    step = tactical::tsw_sequence_step(profile, TswPhase::debris_wait, 1);
    expect(step.phase == TswPhase::post_wait && step.countdown == 350 && step.action == TswAction::create_debris,
        "TSW-10: S9 creates the debris at zero and loads the post wait");
    step = tactical::tsw_sequence_step(profile, TswPhase::debris_wait, 5);
    expect(step.phase == TswPhase::debris_wait && step.countdown == 4 && step.action == TswAction::none, "TSW-10: S9 counts down");
    step = tactical::tsw_sequence_step(profile, TswPhase::post_wait, 1);
    expect(step.phase == TswPhase::complete, "TSW-11: S10 ends at zero");
    step = tactical::tsw_sequence_step(profile, TswPhase::complete, 0);
    expect(step.phase == TswPhase::idle && step.action == TswAction::finish, "TSW-11: S11 returns to idle");
    step = tactical::tsw_sequence_step(profile, TswPhase::ready, 0);
    expect(step.phase == TswPhase::ready && step.action == TswAction::none, "the approach phases are not sequence phases");
    for (const auto phase : {TswPhase::idle, TswPhase::facing, TswPhase::approaching, TswPhase::ready})
        expect(!tactical::tsw_holds_victory(phase), "TSW-09: the approach phases hold no victory test");
    for (const auto phase : {TswPhase::powering_up, TswPhase::charged, TswPhase::attack_start, TswPhase::attack_anim,
             TswPhase::target_destruction, TswPhase::debris_wait, TswPhase::post_wait, TswPhase::complete})
        expect(tactical::tsw_holds_victory(phase), "TSW-09: S4..S11 hold the victory tests");

    // TSW-10: a destroyed target never qualifies again.
    tactical::TswSetup setup;
    setup.targets = {{1, 5, 0, at(100), {}}, {2, 5, 0, at(300), {}}};
    const std::vector<std::uint32_t> gone{1};
    auto picked = tactical::tsw_select_target(setup, 7, at(0), gone);
    expect(picked && picked.value() == 2, "TSW-10: the nearest standing target wins when the nearer one is destroyed");
    const std::vector<std::uint32_t> all{1, 2};
    picked = tactical::tsw_select_target(setup, 7, at(0), all);
    expect(picked && picked.value() == 0, "TSW-10: with every target destroyed there is none");
}

// A defender's object (ready at its first service) so the tests control the request tick; player 1 owns it.
[[nodiscard]] Scenario defender_scenario(const tactical::TswProfile& profile) {
    auto value = scenario(stock_attack, true, true, 1);
    value.profile = profile;
    return value;
}

void test_sequence_timeline() {
    const auto profile = stock_profile();
    constexpr std::uint64_t request_tick = 3;
    const auto line = timeline(request_tick, profile);
    auto value = defender_scenario(profile);
    auto session = make(value);
    expect(static_cast<bool>(session.submit(request(request_tick, 1, 1, tactical::TswStage::start_power_up))), "TSW-08: the fire click is submitted");
    const auto ticks = run(session, line.finish + 40);
    const auto start = position_of(session, tsw_id);
    expect(start.x.raw() == units(stock_attack).raw(), "TSW-08: the object does not move through the sequence");

    expect(event_ticks(ticks, tactical::EventKind::tsw_power_up_started) == std::vector<std::uint64_t>({request_tick}), "TSW-08: S4 opens on the request tick");
    expect(event_ticks(ticks, tactical::EventKind::tsw_attack_started) == std::vector<std::uint64_t>({line.attack_started}),
        "TSW-08: the attack starts at the oracle's tick (270 + 1 frames after the request)");
    expect(event_ticks(ticks, tactical::EventKind::tsw_target_destroyed) == std::vector<std::uint64_t>({line.destroyed}),
        "TSW-10: the target is destroyed after the 75-frame attack animation");
    expect(event_ticks(ticks, tactical::EventKind::planet_obliterated) == std::vector<std::uint64_t>({line.destroyed}),
        "TSW-10: planet_obliterated is published once, with the target's destruction");
    expect(event_ticks(ticks, tactical::EventKind::tsw_debris_created) == std::vector<std::uint64_t>({line.debris}),
        "TSW-10: the debris is created 33 frames after the destruction");
    expect(event_ticks(ticks, tactical::EventKind::tsw_sequence_complete) == std::vector<std::uint64_t>({line.finish}),
        "TSW-11: the sequence completes after the 350-frame post wait");
    expect(line.attack_started == request_tick + 270 + 1 && line.destroyed == request_tick + 270 + 2 + 75
            && line.debris == line.destroyed + 33 && line.complete == line.debris + 350,
        "the oracle itself matches the stock numbers: 270 / 75 / 33 / 350");
    // The planet event carries what the galactic side needs: the object, its owner and the target index.
    for (const auto& row : ticks)
        for (const auto& event : row.events)
            if (event.kind == tactical::EventKind::planet_obliterated)
                expect(event.unit == tsw_id && event.player == 1 && event.sequence == 1, "TSW-10: planet_obliterated names the object, its owner and the target");
    // Phase by tick, hold by tick (the hash of each tick is checked against the phases in the save test).
    auto replayed = make(value);
    expect(static_cast<bool>(replayed.submit(request(request_tick, 1, 1, tactical::TswStage::start_power_up))), "submitted again");
    eawr::platform::ThreadWorkerAdapter executor(1);
    for (std::uint64_t index = 0; index <= line.finish + 1; ++index) {
        auto stepped = replayed.step(executor);
        expect(static_cast<bool>(stepped), "sequence step succeeds");
        if (!stepped) return;
        const auto record = replayed.tsw_state(tsw_id);
        const auto world = replayed.tsw_world();
        const bool holding = index >= request_tick && index < line.finish;
        expect(world.victory_hold == (holding ? 1U : 0U), "TSW-09: the hold is one from the request until the complete frame, then zero");
        expect(world.planet_destroyed == (index >= line.destroyed), "TSW-10: the planet fact is set from the destruction tick");
        expect(world.destroyed_targets.empty() == (index < line.destroyed), "TSW-10: the target is listed destroyed from the destruction tick");
        if (index == line.charged) expect(record->phase == tactical::TswPhase::charged, "TSW-08: S5 on the oracle's tick");
        if (index == line.attack_started) expect(record->phase == tactical::TswPhase::attack_anim && record->countdown == 75, "TSW-08: S7 with 75 frames");
        if (index == line.destroyed) expect(record->phase == tactical::TswPhase::debris_wait && record->countdown == 33, "TSW-10: S9 with 33 frames");
        if (index == line.debris) expect(record->phase == tactical::TswPhase::post_wait && record->countdown == 350, "TSW-10: S10 with 350 frames");
        if (index == line.complete) expect(record->phase == tactical::TswPhase::complete, "TSW-11: S11");
        if (index == line.finish) expect(record->phase == tactical::TswPhase::idle && record->target == 0, "TSW-11: back to idle with no target");
    }

    // After the sequence the gate is shut: no second approach or power-up (the destroyed-planet gate).
    auto again = make(value);
    expect(static_cast<bool>(again.submit(request(request_tick, 1, 1, tactical::TswStage::start_power_up))), "submitted");
    expect(static_cast<bool>(again.submit(request(line.finish + 5, 2, 1, tactical::TswStage::start_approach))), "a later approach request is submitted");
    expect(static_cast<bool>(again.submit(request(line.finish + 6, 3, 1, tactical::TswStage::start_power_up))), "a later power-up request is submitted");
    const auto later = run(again, line.finish + 10);
    const auto refused = command_events(later, tactical::OrderKind::tactical_superweapon);
    expect(refused.size() == 3 && refused[0].kind == tactical::EventKind::order_accepted && refused[1].kind == tactical::EventKind::order_rejected
            && refused[2].kind == tactical::EventKind::order_rejected,
        "TSW-04: once the planet is destroyed both requests are refused");
    expect(again.tsw_planet_destroyed(), "TSW-10: the session reports the planet fact");

    // A second power-up request while the first is running is refused and changes nothing.
    auto twice = make(value);
    expect(static_cast<bool>(twice.submit(request(request_tick, 1, 1, tactical::TswStage::start_power_up))), "submitted");
    expect(static_cast<bool>(twice.submit(request(request_tick + 4, 2, 1, tactical::TswStage::start_power_up))), "submitted");
    run(twice, request_tick + 8);
    expect(twice.tsw_world().victory_hold == 1 && twice.tsw_state(tsw_id)->phase == tactical::TswPhase::powering_up,
        "TSW-07: a repeated power-up request is a no-op (the hold stays one)");

    // A non-owner never starts it; the gate-shut planet refuses it.
    auto gated = defender_scenario(profile);
    gated.setup.tsw->conflict->planet_destroyable = false;
    auto shut = make(gated);
    expect(static_cast<bool>(shut.submit(request(request_tick, 1, 1, tactical::TswStage::start_power_up))), "submitted");
    run(shut, 20);
    expect(shut.tsw_state(tsw_id)->phase == tactical::TswPhase::ready && shut.tsw_world().victory_hold == 0,
        "TSW-04: a planet that may not be destroyed refuses the power-up");
}

[[nodiscard]] tactical::DurabilityTable durability_table() {
    tactical::DurabilityTable table;
    table.rules = {Fixed::from_decimal("0.2").value(), Fixed::from_decimal("0.4").value(), Fixed::from_decimal("0.33").value()};
    table.profiles = {{tsw_type, units(600), std::nullopt, false, {}}, {enemy_type, units(600), std::nullopt, false, {}},
        {station_type, units(2400), std::nullopt, false, {}}};
    return table;
}

[[nodiscard]] tactical::VictoryRules victory_rules() {
    tactical::VictoryRules rules;
    rules.condition = tactical::VictoryCondition::enemy_starbase_destroyed;
    rules.starbase_types = {station_type};
    rules.contenders = {1, 2};
    rules.humans = {1};
    return rules;
}

// The defender scenario plus a star base each: player 1 (the Death Star's owner) wins when player 2's falls.
[[nodiscard]] Scenario victory_scenario(const tactical::TswProfile& profile) {
    auto value = defender_scenario(profile);
    value.setup.units.push_back(unit(station_owner_one, station_type, 1, at(0, -3000, 0)));
    value.setup.units.push_back(unit(station_owner_two, station_type, 2, at(0, 3000, 0)));
    return value;
}

[[nodiscard]] tactical::TacticalSession make_with_victory(const Scenario& value) {
    std::vector<tactical::SensorProfile> profiles = sensors();
    profiles.push_back({station_type, units(2000)});
    auto created = tactical::TacticalSession::create(value.setup, profiles, durability_table(), {}, std::nullopt,
        combat_table(value.profile), victory_rules());
    expect(static_cast<bool>(created), "victory session is created");
    return std::move(created).value();
}

[[nodiscard]] tactical::PlayerCommand destroy_command(const std::uint64_t tick, const std::uint64_t sequence,
    const tactical::PlayerId issuer, const eawr::sim::EntityId target) {
    return {{tick, issuer, sequence}, {target}, tactical::DamagePayload{units(3000)}};
}

void test_victory_hold() {
    const auto profile = stock_profile();
    constexpr std::uint64_t request_tick = 3;
    const auto line = timeline(request_tick, profile);
    const auto value = victory_scenario(profile);

    // Control: without a sequence the destruction decides at once.
    auto plain = make_with_victory(value);
    expect(static_cast<bool>(plain.submit(destroy_command(100, 1, 1, station_owner_two))), "TSW-09: the base kill is submitted");
    run(plain, 120);
    expect(plain.outcome() && plain.outcome()->winner == 1 && plain.outcome()->decided_tick == 100
            && plain.outcome()->condition == tactical::VictoryCondition::enemy_starbase_destroyed,
        "TSW-09: control: the enemy base falling decides the battle on its own tick");

    // During the sequence the same kill is deferred; the retest at the end decides it.
    auto held = make_with_victory(value);
    expect(static_cast<bool>(held.submit(request(request_tick, 1, 1, tactical::TswStage::start_power_up))), "submitted");
    expect(static_cast<bool>(held.submit(destroy_command(request_tick + 100, 2, 1, station_owner_two))), "submitted");
    const auto ticks = run(held, line.finish + 5);
    expect(event_ticks(ticks, tactical::EventKind::victory) == std::vector<std::uint64_t>({line.finish}),
        "TSW-09: the victory is published on the tick the hold is released, not on the kill's tick");
    expect(held.outcome() && held.outcome()->winner == 1 && held.outcome()->decided_tick == line.finish
            && held.outcome()->end_tick == line.finish + tactical::victory_countdown_frames
            && held.outcome()->deciding_unit == station_owner_two,
        "TSW-09: the retest decides with the release tick and the deferred base");
    expect(held.tsw_world().victory_hold == 0 && held.tsw_world().deferred.empty(), "TSW-09: the hold and the pending list are empty after the release");

    // A kill before the power-up is not deferred; a kill after the release is not either.
    auto before = make_with_victory(value);
    expect(static_cast<bool>(before.submit(destroy_command(2, 1, 1, station_owner_two))), "submitted");
    expect(static_cast<bool>(before.submit(request(request_tick, 2, 1, tactical::TswStage::start_power_up))), "submitted");
    run(before, 10);
    expect(before.outcome() && before.outcome()->decided_tick == 2, "TSW-09: a victory decided before the power-up stands");
    auto after = make_with_victory(value);
    expect(static_cast<bool>(after.submit(request(request_tick, 1, 1, tactical::TswStage::start_power_up))), "submitted");
    expect(static_cast<bool>(after.submit(destroy_command(line.finish + 3, 2, 1, station_owner_two))), "submitted");
    run(after, line.finish + 8);
    expect(after.outcome() && after.outcome()->decided_tick == line.finish + 3, "TSW-09: after the sequence the tests run at once again");

    // The battle goes on during the sequence: a loss during the hold is only recorded; the retest decides on the state
    // at the release (here the second player's base fell, so the recorded test finds the first player the winner).
    auto both = make_with_victory(value);
    expect(static_cast<bool>(both.submit(request(request_tick, 1, 1, tactical::TswStage::start_power_up))), "submitted");
    expect(static_cast<bool>(both.submit(destroy_command(request_tick + 50, 2, 1, station_owner_two))), "submitted");
    run(both, request_tick + 60);
    expect(!both.outcome() && both.tsw_world().deferred.size() == 1, "TSW-09: a kill during the sequence is recorded, no outcome yet");
    // Retail drops the list on release even when no test decides (checked through the world: it empties).

    // The object lost mid-sequence releases its hold (unverified retail behaviour, documented).
    auto lost = make_with_victory(value);
    expect(static_cast<bool>(lost.submit(request(request_tick, 1, 1, tactical::TswStage::start_power_up))), "submitted");
    expect(static_cast<bool>(lost.submit(destroy_command(request_tick + 20, 2, 1, station_owner_two))), "submitted");
    expect(static_cast<bool>(lost.submit(destroy_command(request_tick + 50, 3, 2, tsw_id))), "submitted");
    run(lost, request_tick + 70);
    expect(!lost.tsw_state(tsw_id).has_value() && lost.tsw_world().victory_hold == 0 && lost.outcome().has_value(),
        "TSW-09: the object destroyed mid-sequence releases the hold and the deferred test decides");
}

// Two super weapon objects whose sequences overlap. The second one's request is accepted (the planet still stands),
// both sequences run to their own end, and the hand-off to the galactic side is published once.
void test_two_objects_one_planet_event() {
    constexpr eawr::sim::EntityId second_id = 5;
    const auto profile = short_profile();
    auto value = defender_scenario(profile);
    value.setup.units.push_back(unit(second_id, tsw_type, 1, at(stock_attack, 50, 0)));
    constexpr std::uint64_t first_request = 3;
    constexpr std::uint64_t second_request = 5;
    const auto first = timeline(first_request, profile);
    const auto second = timeline(second_request, profile);
    expect(second.attack_started < first.destroyed && first.destroyed < second.destroyed,
        "the second sequence is running when the first destroys the target");
    auto session = make(value);
    expect(static_cast<bool>(session.submit(request(first_request, 1, 1, tactical::TswStage::start_power_up))), "submitted");
    expect(static_cast<bool>(session.submit({{second_request, 1, 2}, {second_id},
        tactical::TacticalSuperweaponPayload{tactical::TswStage::start_power_up, tactical::TswAdmission::player}})), "submitted");
    eawr::platform::ThreadWorkerAdapter executor(1);
    std::vector<Tick> ticks;
    std::uint32_t peak_hold = 0;
    for (std::uint64_t index = 0; index <= second.finish + 2; ++index) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "two-object step succeeds");
        if (!stepped) return;
        Tick row;
        row.tick = index;
        for (const auto& event : stepped.value().snapshot->events()) row.events.push_back(event);
        ticks.push_back(std::move(row));
        peak_hold = std::max(peak_hold, session.tsw_world().victory_hold);
        if (index == first.finish) expect(session.tsw_world().victory_hold == 1, "TSW-09: the hold stays up while the second sequence runs");
    }
    expect(peak_hold == 2 && session.tsw_world().victory_hold == 0, "TSW-09: two sequences raise the hold twice and release it twice");
    expect(command_events(ticks, tactical::OrderKind::tactical_superweapon).size() == 2, "TSW-07: both power-up requests are accepted");
    expect(event_ticks(ticks, tactical::EventKind::planet_obliterated) == std::vector<std::uint64_t>({first.destroyed}),
        "TSW-10: planet_obliterated is published once per battle, by the first destruction");
    expect(event_ticks(ticks, tactical::EventKind::tsw_target_destroyed) == std::vector<std::uint64_t>({first.destroyed, second.destroyed}),
        "TSW-10: each object still ends its own sequence at its own tick");
    expect(event_ticks(ticks, tactical::EventKind::tsw_sequence_complete) == std::vector<std::uint64_t>({first.finish, second.finish}),
        "TSW-11: both sequences complete");
    expect(session.tsw_world().destroyed_targets == std::vector<std::uint32_t>({1}) && session.tsw_planet_destroyed(),
        "TSW-10: the one target is listed once and the planet fact stays set");
    for (const auto& row : ticks)
        for (const auto& event : row.events)
            if (event.kind == tactical::EventKind::planet_obliterated)
                expect(event.unit == tsw_id && event.player == 1, "TSW-10: the first object publishes the hand-off");
}

// More than 64 relevant kills during one hold. Nothing is dropped: the test for the loser's owner is recorded once
// (a unit test depends only on the owner and the state at the release) and decides at the release.
void test_many_kills_during_the_hold() {
    constexpr eawr::sim::EntityId first_extra = 10;
    constexpr std::size_t extra = 70;
    const auto profile = stock_profile();
    constexpr std::uint64_t request_tick = 3;
    const auto line = timeline(request_tick, profile);
    auto value = defender_scenario(profile);
    for (std::size_t index = 0; index < extra; ++index)
        value.setup.units.push_back(unit(first_extra + index, enemy_type, 2, at(0, 4000 + 10 * static_cast<std::int64_t>(index), 0)));
    auto rules = victory_rules();
    rules.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    rules.starbase_types.clear();
    rules.relevant_types = {tsw_type, enemy_type};
    rules.controlled_players = {1, 2};
    auto created = tactical::TacticalSession::create(value.setup, sensors(), durability_table(), {}, std::nullopt,
        combat_table(value.profile), rules);
    expect(static_cast<bool>(created), "all-units victory session is created");
    if (!created) return;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit(request(request_tick, 1, 1, tactical::TswStage::start_power_up))), "submitted");
    std::uint64_t sequence = 2;
    expect(static_cast<bool>(session.submit(destroy_command(request_tick + 10, sequence++, 1, enemy_id))), "submitted");
    for (std::size_t index = 0; index < extra; ++index)
        expect(static_cast<bool>(session.submit(destroy_command(request_tick + 11 + index, sequence++, 1, first_extra + index))), "submitted");
    const std::uint64_t killed_by = request_tick + 11 + extra + 5;
    run(session, killed_by);
    expect(!session.outcome() && session.tsw_world().victory_hold == 1, "TSW-09: 71 kills during the hold decide nothing yet");
    expect(session.tsw_world().deferred.size() == 1 && session.tsw_world().deferred.front().owner == 2
            && session.tsw_world().deferred.front().unit == enemy_id,
        "TSW-09: the loser's test is recorded once, with the first kill (no cap, nothing dropped)");
    const auto rest = run(session, line.finish + 5 - killed_by);
    expect(event_ticks(rest, tactical::EventKind::victory).size() == 1, "TSW-09: the held kills decide the battle once at the release");
    expect(session.outcome() && session.outcome()->winner == 1 && session.outcome()->decided_tick == line.finish
            && session.outcome()->deciding_unit == enemy_id,
        "TSW-09: the retest at the release names the first player the winner on the release tick");
    expect(session.tsw_world().deferred.empty() && session.tsw_world().victory_hold == 0, "TSW-09: the list is empty after the release");
}

// Steps until `completed_tick` reaches `until`, submitting each command just before its tick executes: a checkpoint
// refuses a command cursor that already points at a future tick, so a resumed session must be fed the same way.
std::vector<Tick> run_fed(tactical::TacticalSession& session, const std::uint64_t until,
    const std::vector<tactical::PlayerCommand>& commands, const std::size_t workers = 1) {
    std::vector<Tick> result;
    eawr::platform::ThreadWorkerAdapter executor(workers);
    while (session.completed_tick() < until) {
        const auto tick = session.completed_tick();
        for (const auto& command : commands)
            if (command.key.tick == tick) expect(static_cast<bool>(session.submit(command)), "the command is submitted on its tick");
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "fed step succeeds");
        if (!stepped) break;
        Tick row;
        row.tick = tick;
        row.hash = stepped.value().state_sha256;
        for (const auto& event : stepped.value().snapshot->events()) row.events.push_back(event);
        result.push_back(std::move(row));
    }
    return result;
}

void test_sequence_save_and_workers() {
    const auto profile = short_profile();
    constexpr std::uint64_t request_tick = 3;
    const auto line = timeline(request_tick, profile);
    const auto value = victory_scenario(profile);
    const std::vector<tactical::PlayerCommand> commands{request(request_tick, 1, 1, tactical::TswStage::start_power_up),
        destroy_command(request_tick + 8, 2, 1, station_owner_two)};
    std::vector<tactical::SensorProfile> profiles = sensors();
    profiles.push_back({station_type, units(2000)});
    const std::uint64_t horizon = line.finish + 20;
    // Save after the tick that leaves the object in each phase, resume, and compare with the uninterrupted run.
    struct Probe { std::uint64_t tick; tactical::TswPhase phase; };
    const std::vector<Probe> probes = {{request_tick - 1, tactical::TswPhase::ready}, {request_tick + 5, tactical::TswPhase::powering_up},
        {line.charged, tactical::TswPhase::charged}, {line.attack_start, tactical::TswPhase::attack_start},
        {line.attack_started, tactical::TswPhase::attack_anim}, {line.destroyed - 1, tactical::TswPhase::target_destruction},
        {line.destroyed, tactical::TswPhase::debris_wait}, {line.debris, tactical::TswPhase::post_wait},
        {line.complete, tactical::TswPhase::complete}, {line.finish, tactical::TswPhase::idle}};
    auto reference = make_with_victory(value);
    const auto straight = run_fed(reference, horizon, commands);
    expect(straight.size() == horizon, "TSW: the reference run has every tick");
    for (const auto& probe : probes) {
        auto live = make_with_victory(value);
        run_fed(live, probe.tick + 1, commands);
        expect(live.tsw_state(tsw_id)->phase == probe.phase, "TSW: the saved phase is the oracle's phase at tick " + std::to_string(probe.tick));
        const auto saved = live.save_checkpoint_state();
        expect(static_cast<bool>(saved), "TSW: the sequence checkpoint saves");
        if (!saved) continue;
        auto created = tactical::TacticalSession::from_replay(live.record(), profiles, durability_table(), {}, std::nullopt,
            combat_table(value.profile), victory_rules());
        expect(static_cast<bool>(created), "TSW: the resume session is created");
        if (!created) continue;
        auto resumed = std::move(created).value();
        const auto loaded = resumed.load_checkpoint_state(saved.value());
        expect(static_cast<bool>(loaded), "TSW: the sequence checkpoint loads at tick " + std::to_string(probe.tick));
        if (!loaded) continue;
        expect(resumed.tsw_state(tsw_id) == live.tsw_state(tsw_id) && resumed.tsw_world() == live.tsw_world()
                && resumed.state_sha256() == live.state_sha256(),
            "TSW: the loaded sequence state equals the saved state at tick " + std::to_string(probe.tick));
        const auto rest = run_fed(resumed, horizon, commands);
        bool equal = rest.size() + probe.tick + 1 == horizon;
        for (std::size_t index = 0; equal && index < rest.size(); ++index) equal = rest[index].hash == straight[probe.tick + 1 + index].hash;
        expect(equal, "TSW: resume equals the uninterrupted run from tick " + std::to_string(probe.tick));
        expect(resumed.outcome() == reference.outcome() && resumed.tsw_world() == reference.tsw_world(),
            "TSW: the resumed run ends with the same outcome and planet fact");
    }
    expect(reference.outcome() && reference.outcome()->decided_tick == line.finish,
        "TSW-09: the short sequence decides the deferred kill at its release");

    // 1/2/4/8 workers give the same hashes.
    std::vector<std::string> hashes;
    for (const auto& row : straight) hashes.push_back(row.hash);
    for (const std::size_t workers : {2U, 4U, 8U}) {
        auto session = make_with_victory(value);
        const auto ticks = run_fed(session, horizon, commands, workers);
        std::vector<std::string> other;
        for (const auto& row : ticks) other.push_back(row.hash);
        expect(other == hashes, "TSW: the sequence state hashes match at workers " + std::to_string(workers));
    }

    // The recorded replay round-trips and replays to the live run's final state.
    tactical::TacticalReplay replay;
    replay.setup = value.setup;
    replay.final_tick_count = horizon;
    replay.commands = commands;
    const auto bytes = tactical::write_replay(replay);
    expect(static_cast<bool>(bytes), "TSW: the sequence replay writes");
    if (!bytes) return;
    const auto parsed = tactical::parse_replay(bytes.value());
    expect(parsed && parsed.value() == replay, "TSW: the sequence replay round-trips");
    if (!parsed) return;
    auto created = tactical::TacticalSession::from_replay(parsed.value(), profiles, durability_table(), {}, std::nullopt,
        combat_table(value.profile), victory_rules());
    expect(static_cast<bool>(created), "TSW: the replay session is created");
    if (!created) return;
    auto from_replay = std::move(created).value();
    const auto replayed = run(from_replay, horizon);
    auto direct = make_with_victory(value);
    for (const auto& command : commands) expect(static_cast<bool>(direct.submit(command)), "the command is submitted up front");
    const auto upfront = run(direct, horizon);
    bool same = replayed.size() == upfront.size();
    for (std::size_t index = 0; same && index < replayed.size(); ++index) same = replayed[index].hash == upfront[index].hash;
    expect(same && !replayed.empty(), "TSW: replaying the recorded commands gives the live run's hash at every tick");
    expect(from_replay.outcome() == reference.outcome() && direct.outcome() == reference.outcome(),
        "TSW: the replay decides the same outcome");
}

// TSW-13: the snapshot's switch rows (presentation only): one per object with the behaviour, from the committed record.
void test_snapshot_view() {
    using tactical::TswPhase;
    // A defender is ready at once: the row says ready, a target, the gate open, and a zero ETA.
    auto defender = make(scenario(stock_attack, true, true, 1));
    run(defender, 5);
    auto rows = defender.snapshot()->tsw_views();
    expect(rows.size() == 1 && rows[0].unit == tsw_id && rows[0].owner == 1 && rows[0].phase == TswPhase::ready
            && rows[0].has_target && rows[0].serviced && rows[0].gate_open && rows[0].eta_frames == 0,
        "TSW-13: a defender's row is ready, with a target, an open gate and a zero ETA");

    // An attacker mid-flight: approaching, and the ETA is the one the service would announce from.
    auto value = scenario(stock_start);
    auto attacker = make(value);
    run(attacker, 60);
    rows = attacker.snapshot()->tsw_views();
    const auto expected = tactical::tsw_eta_frames(value.profile, position_of(attacker, tsw_id), at(0));
    expect(rows.size() == 1 && rows[0].phase == TswPhase::approaching && rows[0].has_target && expected
            && rows[0].eta_frames == expected.value() && rows[0].eta_frames > 0,
        "TSW-13: an approaching row carries the ETA to arrival");

    // The gate: a skirmish, an undestroyable planet and an already destroyed planet all shut it.
    auto skirmish = make(scenario(stock_attack, false));
    run(skirmish, 3);
    expect(skirmish.snapshot()->tsw_views().size() == 1 && !skirmish.snapshot()->tsw_views()[0].gate_open,
        "TSW-13: no conflict, no gate");
    auto undestroyable = make(scenario(stock_attack, true, false, 1));
    run(undestroyable, 3);
    expect(!undestroyable.snapshot()->tsw_views()[0].gate_open, "TSW-13: an undestroyable planet shuts the gate");
    auto destroyed_setup = scenario(stock_attack, true, true, 1);
    destroyed_setup.setup.tsw->conflict->planet_destroyed = true;
    auto destroyed = make(destroyed_setup);
    run(destroyed, 3);
    expect(!destroyed.snapshot()->tsw_views()[0].gate_open, "TSW-13: a destroyed planet shuts the gate");

    // The fire sequence: after an accepted power-up the row is in the sequence with its target kept and no ETA.
    auto firing = make(scenario(stock_attack, true, true, 1));
    expect(static_cast<bool>(firing.submit(request(2, 1, 1, tactical::TswStage::start_power_up))), "TSW-13: the power-up request is submitted");
    run(firing, 6);
    rows = firing.snapshot()->tsw_views();
    expect(rows.size() == 1 && rows[0].phase == TswPhase::powering_up && rows[0].has_target && rows[0].eta_frames == tactical::TswView::no_eta,
        "TSW-13: a powering-up row keeps its target and shows no ETA");

    // A battle without the behaviour publishes no rows.
    tactical::TacticalSetup plain;
    plain.seed = 7;
    plain.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    plain.units = {unit(enemy_id, enemy_type, 2, at(0, 4000, 0))};
    auto created = tactical::TacticalSession::create(plain, sensors(), {}, {}, std::nullopt, combat_table(stock_profile()));
    expect(static_cast<bool>(created), "TSW-13: a battle without the behaviour is created");
    if (created) {
        auto session = std::move(created).value();
        run(session, 3);
        expect(session.snapshot()->tsw_views().empty(), "TSW-13: a battle without the behaviour publishes no rows");
    }
}

} // namespace

void test_presented_events() {
    // TSW-15: the viewer reads the sequence's events from the live event log, which keeps only presented kinds.
    using tactical::EventKind;
    for (const auto kind : {EventKind::tsw_approach_started, EventKind::tsw_speech_threshold, EventKind::tsw_in_range,
             EventKind::tsw_power_up_started, EventKind::tsw_attack_started, EventKind::tsw_target_destroyed,
             EventKind::tsw_debris_created, EventKind::tsw_sequence_complete}) {
        tactical::Event event;
        event.kind = kind;
        expect(eawr::platform::LiveEventLog::presented(event), "TSW-15: the live event log keeps every presented TSW event");
    }
    tactical::Event hand_off;
    hand_off.kind = EventKind::planet_obliterated;
    expect(!eawr::platform::LiveEventLog::presented(hand_off), "TSW-15: the galactic hand-off is not a presentation event");
}

int main() {
    if (!eawr::testing::support_contract_self_test()) {
        std::cerr << "FAIL: shared check/require self-test\n";
        return 1;
    }
    test_pure_rules();
    test_attacker_flight();
    test_ready_deceleration_and_hold();
    test_facing();
    test_defender_and_no_target();
    test_commands();
    test_defender_request_before_first_service();
    test_speech_gate();
    test_replay_and_checkpoint();
    test_workers_and_battles_without_the_setup();
    test_sequence_rules();
    test_sequence_timeline();
    test_victory_hold();
    test_two_objects_one_planet_event();
    test_many_kills_during_the_hold();
    test_sequence_save_and_workers();
    test_snapshot_view();
    test_presented_events();
    if (checker.finish() == 0) std::cout << "tsw tests passed\n";
    return checker.finish();
}
