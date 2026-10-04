#include "fighter_support.hpp"

#include <array>

namespace fighter_test_support {

// frigate far beyond its chase reach; the hashes are the same with 1, 2 and 4 workers.
struct OrderRun {
    std::vector<std::string> hashes;
    std::map<int, math::Vec3> centre;          // the container (craft centre) by tick
    std::map<int, std::optional<tactical::CombatState>> craft; // craft 12's combat state by tick
    std::map<int, std::optional<tactical::SquadronFormationState>> formations;
};

[[nodiscard]] OrderRun run_orders(const std::size_t workers, const bool orders) {
    OrderRun run;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
        unit(12, craft_type, empire, at(-10, 10)), unit(20, frigate_type, rebel, at(4000, -3000))};
    setup.squadrons = {{10, {11, 12}}};
    auto table = motion();
    table.squadrons.spawners.clear();
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
    expect(static_cast<bool>(created), "FO: the session builds");
    if (!created) return run;
    auto session = std::move(created).value();
    if (orders) {
        const auto submit = [&session](const tactical::PlayerCommand& command, const std::string_view what) {
            expect(static_cast<bool>(session.submit(command)), std::string("FO: the ") + std::string(what) + " submits");
        };
        submit({{5, empire, 1}, {10}, tactical::MovePayload{at(1500, 800)}}, "move");
        submit({{700, empire, 2}, {10}, tactical::MovePayload{at(-1500, 0)}}, "second move");
        submit({{760, empire, 3}, {10}, tactical::StopPayload{}}, "stop");
        submit({{1000, empire, 4}, {10}, tactical::AttackPayload{20}}, "attack");
    }
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 1060; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "FO: step failed: " + stepped.error().message);
            break;
        }
        run.hashes.push_back(stepped.value().state_sha256);
        if (const auto* container = instance(*stepped.value().snapshot, 10)) run.centre[tick] = position(*container);
        run.craft[tick] = session.combat_state(12);
        if (const auto state = session.squadron_state(10)) run.formations[tick] = state->formation;
    }
    return run;
}

[[nodiscard]] double distance_to(const math::Vec3& from, const std::int64_t x, const std::int64_t y) {
    const double dx = static_cast<double>(from.x.raw()) / static_cast<double>(one) - static_cast<double>(x);
    const double dy = static_cast<double>(from.y.raw()) / static_cast<double>(one) - static_cast<double>(y);
    return std::sqrt(dx * dx + dy * dy);
}

struct RejoinRun {
    std::vector<std::string> hashes;
    std::vector<double> late_heights;
    int regroup{-1};
};

[[nodiscard]] RejoinRun run_late_reversal(const std::size_t workers, const bool face = false) {
    RejoinRun run;
    auto table = motion();
    table.squadrons.spawners.clear();
    auto& craft = table.squadrons.craft.front();
    craft.max_speed = Fixed::from_raw(one * 48 / 10);
    craft.min_speed = units(3);
    craft.rate_of_turn = Fixed::from_raw(one * 36 / 10);
    craft.lift = Fixed::from_raw(one * 48 / 10);
    craft.thrust = units(1);
    craft.roll_rate = units(6);
    craft.bank_angle = units(40);
    auto& squadron = table.squadrons.squadrons.front();
    squadron.members.assign(5, craft_type);
    squadron.offsets = {at(30, 0), at(0, 15), at(0, -15), at(-30, 30), at(-30, -30)};
    squadron.formation_tolerance = units(25);
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units.push_back(unit(10, squadron_a, empire, at(0, 0)));
    setup.squadrons.push_back({10, {11, 12, 13, 14, 15}});
    for (std::size_t index = 0; index < squadron.offsets.size(); ++index) {
        setup.units.push_back(unit(static_cast<eawr::sim::EntityId>(11 + index), craft_type, empire, squadron.offsets[index]));
    }
    auto made = tactical::TacticalSession::create(setup, sensors(), durability(), table);
    expect(static_cast<bool>(made), "WSQ-17: the late-reversal session builds");
    if (!made) return run;
    auto session = std::move(made).value();
    expect(static_cast<bool>(session.submit({{1, empire, 1}, {10}, tactical::MovePayload{at(8000, 0)}})),
        "WSQ-17: the first leg submits");
    expect(static_cast<bool>(session.submit({{750, empire, 2}, {10}, tactical::MovePayload{at(-4000, 0)}})),
        "WSQ-17: the late reversal submits");
    if (face) expect(static_cast<bool>(session.submit({{800, empire, 3}, {10}, tactical::FacePayload{at(4000, 0)}})),
        "FO-03: a face order during the reversal submits");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::optional<tactical::SquadronLane> before_face;
    for (int tick = 0; tick < 1050; ++tick) {
        const auto step = session.step(executor);
        expect(static_cast<bool>(step), "WSQ-17: the late-reversal session steps");
        if (!step) return run;
        run.hashes.push_back(step.value().state_sha256);
        if (tick == 799) before_face = session.squadron_state(10)->lane;
        if (face && tick == 800) expect(session.squadron_state(10)->lane == before_face,
            "FO-03: a face order leaves the retained path segment intact");
        double worst = 0.0;
        for (const auto id : {11U, 12U, 13U, 14U, 15U}) {
            const auto* view = instance(*step.value().snapshot, id);
            expect(view != nullptr, "WSQ-17: each craft survives the reversal");
            if (view == nullptr) continue;
            const double height = static_cast<double>(position(*view).z.raw()) / static_cast<double>(one);
            worst = std::max(worst, std::abs(height));
            if (tick == 900) run.late_heights.push_back(height);
        }
        if (tick > 800 && worst < 1.0 && run.regroup < 0) run.regroup = tick;
    }
    return run;
}

void test_squadron_orders() {
    const auto reversal = run_late_reversal(1);
    // WSQ-17: native frame 901, authored slot order (leader, +/-15, +/-30).
    // Native reversal 751 -> first all-below-one sample 906; formation slack
    // ceil(25 / 4.8) plus its two-frame sampling interval gives eight frames.
    constexpr std::array<double, 5> native_late{0.158724, -0.469523, -0.663341, -1.253026, -0.843593};
    expect(reversal.late_heights.size() == native_late.size(), "WSQ-17: the native comparison includes all five craft");
    expect(std::abs((reversal.regroup - 750) - (906 - 751)) <= 8,
        "WSQ-17: the session's regroup time agrees with the native trace within formation slack and sampling");
    for (std::size_t index = 0; index < reversal.late_heights.size() && index < native_late.size(); ++index) {
        expect(std::abs(reversal.late_heights[index] - native_late[index]) < 1.0,
            "WSQ-17: the session's leader and follower heights agree with the native late sample within one unit");
    }
    expect(reversal.hashes.size() == 1050 && reversal.regroup >= 801 && reversal.regroup <= 900,
        "WSQ-17: the session reforms after the late reversal by tick 900 (" + std::to_string(reversal.regroup) + ")");
    for (const double height : reversal.late_heights) {
        expect(std::abs(height) < 1.0, "WSQ-17: leader and followers have returned to their layer at tick 900");
    }
    for (const std::size_t workers : {2U, 4U, 8U}) {
        const auto parallel = run_late_reversal(workers);
        expect(parallel.hashes == reversal.hashes && parallel.regroup == reversal.regroup,
            "WSQ-17: the late reversal hashes and regroup tick match with " + std::to_string(workers) + " workers");
    }
    const auto faced = run_late_reversal(1, true);
    expect(faced.regroup == reversal.regroup && faced.late_heights == reversal.late_heights,
        "FO-03: an ignored face order does not change formation rejoin");
    std::cout << "session late reversal: regroup " << reversal.regroup << ", tick900 heights";
    for (const double height : reversal.late_heights) std::cout << ' ' << height;
    std::cout << '\n';
    expect(tactical::squadron_move_arrived(at(0, 0), at(100, 0), at(100, 50)), "FO-02: on the destination line");
    expect(tactical::squadron_move_arrived(at(0, 0), at(100, 0), at(140, -900)), "FO-02: past the destination line");
    expect(!tactical::squadron_move_arrived(at(0, 0), at(100, 0), at(99, 0)), "FO-02: short of the destination");
    expect(tactical::squadron_move_arrived(at(5, 5), at(5, 5), at(-300, 7)), "FO-02: a zero-length move has arrived");
    expect(!tactical::squadron_move_arrived(at(-60000, -60000), at(60000, 60000), at(59999, 59999)),
        "FO-02: exact far from the origin");

    const auto run = run_orders(1, true);
    expect(run.hashes.size() == 1060, "FO: 1060 ticks");
    if (run.hashes.size() != 1060) return;
    expect(run.formations.at(0) && run.formations.at(0)->complete && run.formations.at(0)->has_reached_done,
        "WMV-18: a starting team retains its completed position formation before a player order");
    expect(run.formations.at(6) && !run.formations.at(6)->complete
        && run.formations.at(6)->base_position == at(1500, 800, 0),
        "WMV-18: a move replaces the base destination and starts its own completion");
    expect(run.formations.at(600) && run.formations.at(600)->complete
        && run.formations.at(600)->base_position == at(1500, 800, 0),
        "WMV-18: arrival retains the completed formation and authored base destination");
    expect(run.formations.at(761) && run.formations.at(761)->complete && run.formations.at(761)->has_reached_done,
        "WMV-18: stopping registers a completed position destination");
    expect(run.formations.at(1002) && run.formations.at(1002)->base_target == 20
        && !run.formations.at(1002)->attack_override,
        "WMV-17: a player attack supplies a base object destination rather than an autonomous override");
    // FO-01: the move flies at full speed (5.4 per frame; the idle crawl is 1.8), then holds.
    const double early = distance_to(run.centre.at(6), 1500, 800);
    const double later = distance_to(run.centre.at(206), 1500, 800);
    expect(early - later > 200.0 * 4.0, "FO-01: the squadron closes at full speed, got "
        + std::to_string((early - later) / 200.0) + " per frame");
    for (const int tick : {500, 600, 699}) {
        expect(distance_to(run.centre.at(tick), 1500, 800) < 150.0,
            "FO-02: the squadron holds the destination at tick " + std::to_string(tick) + ", "
                + std::to_string(distance_to(run.centre.at(tick), 1500, 800)) + " away");
    }
    // FO-03: a stop 60 ticks into the second move holds where the squadron stood.
    const auto stopped = run.centre.at(761);
    const double stop_x = static_cast<double>(stopped.x.raw()) / static_cast<double>(one);
    const double stop_y = static_cast<double>(stopped.y.raw()) / static_cast<double>(one);
    for (const int tick : {900, 999}) {
        const double dx = static_cast<double>(run.centre.at(tick).x.raw()) / static_cast<double>(one) - stop_x;
        const double dy = static_cast<double>(run.centre.at(tick).y.raw()) / static_cast<double>(one) - stop_y;
        expect(std::sqrt(dx * dx + dy * dy) < 150.0, "FO-03: the stopped squadron holds at tick " + std::to_string(tick));
    }
    expect(distance_to(run.centre.at(999), -1500, 0) > 1000.0, "FO-03: the stop cut the second move short");
    // FO-03: nothing is in reach before the attack order; after it every craft targets the frigate.
    expect(run.craft.at(999) && !run.craft.at(999)->direct, "FO-03: no target before the attack order");
    expect(run.craft.at(1002) && run.craft.at(1002)->attack_target == 20 && run.craft.at(1002)->direct,
        "FO-03: the attack order is the squadron's target");
    expect(distance_to(run.centre.at(1059), 4000, -3000) < distance_to(run.centre.at(1001), 4000, -3000) - 200.0,
        "FO-03: the squadron closes on its target");

    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_orders(workers, true).hashes == run.hashes,
            "FO: " + std::to_string(workers) + " workers hash like one");
    }
    // A session without squadron orders is unchanged by them (move state is hashed only in a move).
    const auto quiet = run_orders(1, false);
    expect(quiet.hashes.size() == 1060 && quiet.hashes[4] == run.hashes[4], "FO: the ticks before the first order hash alike");

    // WSQ-47/WMV-18: ordering a team against a craft stores the craft's team as its
    // destination, while preserving the user's requested ID in the recorded command/order.
    std::vector<std::string> craft_reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
            unit(12, craft_type, empire, at(-10, 10)), unit(20, squadron_a, rebel, at(1000, 0)),
            unit(21, craft_type, rebel, at(1000, 0)), unit(22, craft_type, rebel, at(990, 10))};
        setup.squadrons = {{10, {11, 12}}, {20, {21, 22}}};
        auto table = motion();
        table.squadrons.spawners.clear();
        auto made = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
        expect(static_cast<bool>(made), "WMV-18: explicit craft-order session builds");
        if (!made) continue;
        auto session = std::move(made).value();
        expect(static_cast<bool>(session.submit({{1, empire, 1}, {10}, tactical::AttackPayload{21}})),
            "WMV-18: explicit craft attack submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 3; ++tick) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "WMV-18: craft attack steps");
            if (stepped) hashes.push_back(stepped.value().state_sha256);
        }
        const auto mind = session.squadron_state(10);
        expect(mind && mind->target == 20 && mind->formation && mind->formation->base_target == 20,
            "WMV-18: explicit craft attack uses its team for target and base destination");
        const auto live = session.units();
        const auto ordered = std::find_if(live.begin(), live.end(), [](const tactical::UnitState& unit) { return unit.entity_id == 10; });
        expect(ordered != live.end() && ordered->order.target == 21,
            "WSQ-47: published player order retains the requested craft ID");
        const auto recorded = session.record();
        expect(recorded.commands.size() == 1 && std::get<tactical::AttackPayload>(recorded.commands.front().payload).target == 21,
            "WSQ-47: recorded player command retains the requested craft ID");
        expect(hashes.size() == 3, "WMV-18: explicit craft-order run completes three ticks");
        if (workers == 1) craft_reference = hashes;
        else expect(hashes == craft_reference, "WMV-18: explicit craft-order hashes match on 1/2/4/8 workers");
    }
}

// C-14, C-15 (#452, FO-05, FO-06): a player attack-move or guard given to a squadron's team
// container. Squadron 10 (craft 11, 12) stands at `start`; the rebel frigate 20 stands at `enemy`
// and the Empire frigate 30 at (1000, 0). The order (to `point` where it names one) goes in at tick 5; the run records the tick
// craft 12 first targets the frigate 20 and the container's centre by tick.
enum class Divert { move, attack_move, attack_move_unit, guard_point, guard_unit };

struct DivertRun {
    std::vector<std::string> hashes;
    std::map<int, math::Vec3> centre;
    std::optional<int> engaged; // the first tick craft 12's target is the frigate 20
};

[[nodiscard]] DivertRun run_divert(const std::size_t workers, const Divert order, const math::Vec3& start,
    const math::Vec3& enemy, const math::Vec3& point) {
    DivertRun run;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    const math::Vec3 wing{Fixed::from_raw(start.x.raw() - 10 * one), Fixed::from_raw(start.y.raw() + 10 * one), start.z};
    setup.units = {unit(10, squadron_a, empire, start), unit(11, craft_type, empire, start),
        unit(12, craft_type, empire, wing), unit(20, frigate_type, rebel, enemy),
        unit(30, frigate_type, empire, at(1000, 0))};
    setup.squadrons = {{10, {11, 12}}};
    auto table = motion();
    table.squadrons.spawners.clear();
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
    expect(static_cast<bool>(created), "C-14: the session builds");
    if (!created) return run;
    auto session = std::move(created).value();
    tactical::CommandPayload payload = tactical::MovePayload{point};
    switch (order) {
    case Divert::move: break;
    case Divert::attack_move: payload = tactical::AttackMovePayload{point, 0}; break;
    case Divert::attack_move_unit: payload = tactical::AttackMovePayload{{}, 30}; break;
    case Divert::guard_point: payload = tactical::GuardPayload{point, 0}; break;
    case Divert::guard_unit: payload = tactical::GuardPayload{{}, 30}; break;
    }
    expect(static_cast<bool>(session.submit({{5, empire, 1}, {10}, payload})), "C-14: the order submits");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 600; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "C-14: step failed: " + stepped.error().message);
            break;
        }
        run.hashes.push_back(stepped.value().state_sha256);
        if (const auto* container = instance(*stepped.value().snapshot, 10)) run.centre[tick] = position(*container);
        const auto craft = session.combat_state(12);
        if (!run.engaged && craft && craft->attack_target == 20) run.engaged = tick;
    }
    return run;
}

void test_squadron_attack_move() {
    // FO-05: an attack-move to (4000, 0) diverts to a frigate 700 units off the way, within the
    // 300-unit Attack_Move_Response_Range plus the craft's 500-unit attack distance of the leader;
    // the same move without the attack flag flies past it (FO-01).
    const auto enemy = at(2000, 700);
    const auto moved = run_divert(1, Divert::move, at(0, 0), enemy, at(4000, 0));
    expect(moved.hashes.size() == 600 && !moved.engaged, "FO-01: a move does not divert");
    const auto run = run_divert(1, Divert::attack_move, at(0, 0), enemy, at(4000, 0));
    expect(run.hashes.size() == 600, "C-14: 600 ticks");
    expect(run.engaged.has_value(), "FO-05: the attack-move diverts to the frigate off its way");
    if (run.engaged && run.centre.count(*run.engaged) != 0U) {
        const double along = static_cast<double>(run.centre.at(*run.engaged).x.raw()) / static_cast<double>(one);
        expect(along > 1000.0, "FO-05: the squadron scans on the way, engaging at x = " + std::to_string(along));
    }
    // FO-05: an attack-move on the Empire frigate 30 follows it; the rebel frigate, 1300 units
    // from it, is beyond the 800-unit reach, so the squadron does not engage.
    const auto unit_run = run_divert(1, Divert::attack_move_unit, at(0, 0), at(1000, 1300), {});
    expect(unit_run.hashes.size() == 600 && !unit_run.engaged, "FO-05: the frigate is beyond the attack-move reach");
    expect(unit_run.centre.count(599) != 0U && distance_to(unit_run.centre.at(599), 1000, 0) < 150.0,
        "FO-05: the squadron follows the unit it attack-moves to");
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_divert(workers, Divert::attack_move, at(0, 0), enemy, at(4000, 0)).hashes == run.hashes,
            "C-14: " + std::to_string(workers) + " workers hash like one");
        expect(run_divert(workers, Divert::attack_move_unit, at(0, 0), at(1000, 1300), {}).hashes == unit_run.hashes,
            "C-14: " + std::to_string(workers) + " workers hash like one (unit)");
    }
}


void test_squadron_guard() {
    // FO-06: a guard of the Empire frigate 30 escorts it and engages the rebel frigate 1300 units
    // from it, within the 1000-unit Guard_Chase_Range plus the 500-unit attack distance.
    // WSQ-42/45: it lies in the member's collection box; formation diversion admits it
    // relative to the guarded unit even though it is outside the member's attack circle.
    const auto enemy = at(1000, 1300);
    const auto run = run_divert(1, Divert::guard_unit, at(0, 0), enemy, {});
    expect(run.hashes.size() == 600, "C-15: 600 ticks");
    expect(run.engaged.has_value() && *run.engaged < 60, "FO-06: the guard engages near the guarded unit");
    // FO-06: a guard of the point (1000, 0) from 2500 units away flies there and engages once the
    // leader is within Guard_Chase_Range of the point; a move there only holds the point, with the
    // 200-unit idle chase range (FT-02), and never reaches the frigate.
    const auto point = run_divert(1, Divert::guard_point, at(-1500, 0), enemy, at(1000, 0));
    expect(point.engaged.has_value(), "FO-06: the guard of a point engages near the point");
    if (point.engaged && point.centre.count(*point.engaged) != 0U) {
        const double away = distance_to(point.centre.at(*point.engaged), 1000, 0);
        expect(away < 1100.0 && away > 500.0,
            "FO-06: the squadron engages once near the point, " + std::to_string(away) + " away");
    }
    const auto moved = run_divert(1, Divert::move, at(-1500, 0), enemy, at(1000, 0));
    expect(!moved.engaged, "FT-02: a move to the point does not engage the frigate");
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_divert(workers, Divert::guard_unit, at(0, 0), enemy, {}).hashes == run.hashes,
            "C-15: " + std::to_string(workers) + " workers hash like one");
        expect(run_divert(workers, Divert::guard_point, at(-1500, 0), enemy, at(1000, 0)).hashes == point.hashes,
            "C-15: " + std::to_string(workers) + " workers hash like one (point)");
    }
}

// C-16 (#497, FA-07): a player attack order starts the squadron's approach. Squadron 10 (craft 11,
// 12) at the origin at its layer height 0 is ordered at tick 5 to attack the rebel frigate 20,
// 3000 units ahead and 400 below. On the approach the craft keep the layer height until the
// strafe reach; a straight dive (FA-01) would sink toward the frigate on the way. `again` gives
// the same order a second time once the squadron already has the frigate as its target.
struct ApproachRun {
    std::vector<std::string> hashes;
    std::map<int, math::Vec3> leader; // craft 11 by tick
};

[[nodiscard]] ApproachRun run_attack_approach(const std::size_t workers, const bool again) {
    ApproachRun run;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
        unit(12, craft_type, empire, at(-10, 10)), unit(20, frigate_type, rebel, at(3000, 0, -400))};
    setup.squadrons = {{10, {11, 12}}};
    auto table = motion();
    table.squadrons.spawners.clear();
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
    expect(static_cast<bool>(created), "C-16: the session builds");
    if (!created) return run;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit({{5, empire, 1}, {10}, tactical::AttackPayload{20}})), "C-16: the attack submits");
    if (again) {
        expect(static_cast<bool>(session.submit({{60, empire, 2}, {10}, tactical::AttackPayload{20}})),
            "C-16: the second attack submits");
    }
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 400; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "C-16: step failed: " + stepped.error().message);
            break;
        }
        run.hashes.push_back(stepped.value().state_sha256);
        if (const auto* craft = instance(*stepped.value().snapshot, 11)) run.leader[tick] = position(*craft);
    }
    return run;
}

void test_attack_order_approach() {
    for (const bool again : {false, true}) {
        const auto run = run_attack_approach(1, again);
        expect(run.hashes.size() == 400, "C-16: 400 ticks");
        // Halfway, 1500 units short of the frigate and far beyond the 200-unit strafe reach, the
        // leader still flies at its layer height; a dive would be about 200 units down by then.
        bool checked = false;
        for (const auto& [tick, where] : run.leader) {
            const double x = static_cast<double>(where.x.raw()) / static_cast<double>(one);
            if (x < 1500.0) continue;
            const double z = static_cast<double>(where.z.raw()) / static_cast<double>(one);
            expect(z > -40.0, std::string("FA-07: the ordered squadron keeps its layer height on the approach")
                + (again ? " after a repeated order" : "") + ", z = " + std::to_string(z) + " at tick "
                + std::to_string(tick));
            checked = true;
            break;
        }
        expect(checked, "C-16: the leader passes halfway to the frigate");
        for (const std::size_t workers : {2U, 4U, 8U}) {
            expect(run_attack_approach(workers, again).hashes == run.hashes,
                "C-16: " + std::to_string(workers) + " workers hash like one");
        }
    }
}

// C-10 (#424, FO-04): a player attack on a
// squadron targets its team container, which has no weapons target of its own; each attacker
// #531 (space-orders OR-26): a squadron ordered to attack one hardpoint of a ship has each craft's
// weapon aim at it. The frigate (20) has two targetable hardpoints, 0 abeam to one side and 1 to
// the other; squadron 10 (craft 11, 12) attacks hardpoint 1. When it is destroyed the craft go back
// to the nearest standing one; 1, 2, 4 and 8 workers hash alike.
struct HardpointRun {
    std::vector<std::string> hashes;
    std::vector<tactical::CombatEvent> shots; // the craft's shots at the frigate
};

[[nodiscard]] HardpointRun run_hardpoint_attack(const std::size_t workers) {
    HardpointRun run;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
        unit(12, craft_type, empire, at(-10, 10)), unit(20, frigate_type, rebel, at(700, 0))};
    setup.squadrons = {{10, {11, 12}}};
    auto table = motion();
    table.squadrons.spawners.clear();
    auto health = durability();
    tactical::HardpointProfile hardpoint;
    hardpoint.role = tactical::HardpointRole::weapon;
    hardpoint.destroyable = true;
    hardpoint.max_health = units(100);
    for (auto& profile : health.profiles) {
        if (profile.type_id == frigate_type) profile.hardpoints = {hardpoint, hardpoint};
    }
    auto weapons = combat();
    for (auto& profile : weapons.profiles) {
        if (profile.type_id == frigate_type) profile.hardpoints = {{0, at(0, -30), true}, {1, at(0, 30), true}};
    }
    auto created = tactical::TacticalSession::create(setup, sensors(), health, table, std::nullopt, weapons);
    expect(static_cast<bool>(created), "C-17: the session builds");
    if (!created) return run;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit({{5, empire, 1}, {10}, tactical::AttackPayload{20, 1}})),
        "C-17: the hardpoint attack submits");
    expect(static_cast<bool>(session.submit({{400, empire, 2}, {20}, tactical::DamagePayload{units(100), 1}})),
        "C-17: the ordered hardpoint's destruction submits");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 800; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "C-17: step failed: " + stepped.error().message);
            break;
        }
        run.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.kind == tactical::CombatEventKind::weapon_fired && event.target == 20) run.shots.push_back(event);
        }
    }
    return run;
}

void test_squadron_hardpoint_attack() {
    const auto run = run_hardpoint_attack(1);
    expect(run.hashes.size() == 800, "C-17: 800 ticks");
    std::size_t before = 0;
    std::size_t after = 0;
    for (const auto& shot : run.shots) {
        if (shot.tick <= 402) {
            expect(shot.target_hardpoint == 1, "C-17: before its destruction every shot goes to the ordered hardpoint, got "
                + std::to_string(shot.target_hardpoint));
            ++before;
        } else if (shot.tick > 405) {
            expect(shot.target_hardpoint == 0, "C-17: after its destruction the shots go to the one left, got "
                + std::to_string(shot.target_hardpoint));
            ++after;
        }
    }
    expect(before != 0 && after != 0, "C-17: the craft fire before and after the destruction");
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_hardpoint_attack(workers).hashes == run.hashes,
            "C-17: " + std::to_string(workers) + " workers hash like one");
    }
}

} // namespace fighter_test_support
