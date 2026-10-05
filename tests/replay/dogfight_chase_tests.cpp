#include "dogfight_support.hpp"

namespace dogfight_test_support {

// C-22 (FD-01 to FD-04, FD-06): the attacker records a cell, the defender turns on it once its
// craft reach the cell point, both join one cell, and craft pair off one chaser to one chased.
void test_pairing() {
    const auto fixture = trio_fight();
    std::optional<tactical::CombatCell> first_record;
    int retaliated = -1;
    int shared = -1;
    int paired = -1;
    bool one_chaser_each = true;
    bool chased_do_not_chase = true;
    bool own_side_chased = false;
    const std::vector<EntityId> crafts{11, 12, 13, 21, 22, 23};
    double lowest = 0;
    double highest = 0;
    const auto hashes = run(fixture, 1, 900, [&](const tactical::TacticalSession& session, const tactical::TacticalTick& tick) {
        const auto tick_index = static_cast<int>(tick.completed_tick);
        for (const auto craft : crafts) {
            if (const auto position = position_of(*tick.snapshot, craft)) {
                lowest = std::min(lowest, as_double(position->z));
                highest = std::max(highest, as_double(position->z));
            }
        }
        const auto attacker = session.squadron_state(10);
        const auto defender = session.squadron_state(20);
        if (!attacker || !defender) return;
        if (!first_record && attacker->cell) first_record = attacker->cell;
        // The defender's own scan (FT-02) may take a craft of squadron 10, which stands for it.
        if (retaliated < 0 && defender->target >= 10 && defender->target <= 13) retaliated = tick_index;
        if (shared < 0 && attacker->joined && defender->joined && attacker->cell == defender->cell) shared = tick_index;
        std::map<EntityId, int> chasers;
        for (const auto craft : crafts) {
            const auto flight = session.craft_state(craft);
            if (!flight || flight->chase == eawr::sim::invalid_entity_id) continue;
            ++chasers[flight->chase];
            if (paired < 0) paired = tick_index;
            own_side_chased = own_side_chased || ((craft < 20) == (flight->chase < 20));
            const auto chased = session.craft_state(flight->chase);
            chased_do_not_chase = chased_do_not_chase && chased && chased->chase == eawr::sim::invalid_entity_id
                && chased->chase_until == flight->chase_until;
        }
        for (const auto& [chased, count] : chasers) one_chaser_each = one_chaser_each && count == 1;
    });
    expect(hashes.size() == 900, "C-22: 900 ticks");
    expect(first_record.has_value(), "C-22 (FD-02): the attacker records a combat cell");
    expect(retaliated > 0, "C-22 (FD-04): the defender turns on its attacker");
    expect(shared > 0, "C-22 (FD-03): both squadrons join one cell");
    expect(paired > 0, "C-22 (FD-06): craft pair off");
    expect(one_chaser_each, "C-22 (FD-06): a craft is chased by one craft at a time");
    expect(chased_do_not_chase, "C-22 (FD-06): a chased craft does not chase, and both timers run together");
    expect(!own_side_chased, "C-22 (FD-06): craft chase only enemies");
    std::cout << "C-22: cell recorded, retaliation at " << retaliated << ", shared cell at " << shared
              << ", first pair at " << paired << ", craft height " << lowest << " to " << highest << '\n';
    // FD-12 (owner, 2026-09-28; recording S-97: 90 % of retail's dogfight samples lie within
    // 100 units of the plane, the extremes 173): a dogfight stays near its layer height.
    expect(lowest > -200.0 && highest < 200.0, "C-22 (FD-12): the dogfight keeps within 200 units of the layer height");
    expect_workers(fixture, 900, hashes, "C-22");
}

// C-23 (FD-05, FD-06, FD-08): a chase runs ten seconds; when its timer runs out the chaser
// looks again; while it follows within reach the chaser keeps within its attack distance.
void test_chase_timer() {
    const auto fixture = trio_fight();
    struct Chase {
        EntityId chased{};
        std::uint64_t started{};
        std::uint64_t until{};
        std::optional<std::uint64_t> ended;
    };
    std::map<EntityId, std::vector<Chase>> chases;
    const std::vector<EntityId> crafts{11, 12, 13, 21, 22, 23};
    static_cast<void>(run(fixture, 1, 1500, [&](const tactical::TacticalSession& session, const tactical::TacticalTick& tick) {
        for (const auto craft : crafts) {
            const auto flight = session.craft_state(craft);
            if (!flight) continue;
            auto& list = chases[craft];
            const bool open = !list.empty() && !list.back().ended;
            if (open && (flight->chase != list.back().chased || flight->chase_until != list.back().until)) {
                list.back().ended = tick.completed_tick;
            }
            if (flight->chase != eawr::sim::invalid_entity_id
                && (list.empty() || list.back().ended || list.back().until != flight->chase_until)) {
                list.push_back({flight->chase, tick.completed_tick, flight->chase_until, std::nullopt});
            }
        }
    }));
    std::size_t total = 0;
    bool ten_seconds = true;
    bool ends_on_time = true;
    bool renewed = false;
    for (const auto& [craft, list] : chases) {
        for (std::size_t index = 0; index < list.size(); ++index) {
            const auto& chase = list[index];
            ++total;
            // The chase starts in the step that completes tick `started`, frame `started`.
            ten_seconds = ten_seconds && chase.until == chase.started + tactical::chase_frames;
            if (chase.ended) ends_on_time = ends_on_time && *chase.ended == chase.until;
            renewed = renewed || index > 0;
        }
    }
    expect(total > 2, "C-23: craft chase, got " + std::to_string(total));
    expect(ten_seconds, "C-23 (FD-06): every chase timer runs 300 frames");
    expect(ends_on_time, "C-23 (FD-08): a chase ends when its timer runs out, not before (no craft dies here)");
    expect(renewed, "C-23 (FD-08): a craft whose chase ran out chases again");
}

// #893: the real session reports its cone work per tick independently of worker
// scheduling. Timer expiry produces another pairing window in this run.
void test_chase_work_counts() {
    const auto fixture = trio_fight();
    std::vector<std::uint64_t> reference;
    const auto hashes = run(fixture, 1, 640, [&](const auto&, const tactical::TacticalTick& tick) {
        reference.push_back(tick.dogfight_cone_tests);
    });
    expect(std::any_of(reference.begin(), reference.end(), [](const auto count) { return count != 0; }),
        "chase work: the fixture exercises the cone tests");
    expect(std::all_of(reference.begin(), reference.end(), [](const auto count) { return count <= 18; }),
        "chase work: six scanning craft test at most three enemies apiece");
    for (const std::size_t workers : {2U, 4U, 8U}) {
        std::vector<std::uint64_t> counts;
        const auto parallel = run(fixture, workers, 640, [&](const auto&, const tactical::TacticalTick& tick) {
            counts.push_back(tick.dogfight_cone_tests);
        });
        expect(counts == reference, "chase work: every tick counts identically across workers");
        expect(parallel == hashes, "chase work counters do not change the state hash");
    }
}

// FD-05: the follow test: within the attack distance and 90 degrees of yaw and pitch.
void test_can_follow() {
    tactical::CraftProfile profile;
    profile.attack_distance = units(500);
    tactical::CraftView self{1, at(0, 0), {}, &profile};
    Fixed yaw;
    Fixed pitch;
    const auto ahead = tactical::in_follow_cone(self, at(400, 100), yaw, pitch);
    expect(ahead && ahead.value(), "FD-05: a craft ahead within reach can be followed");
    const auto side = tactical::in_follow_cone(self, at(10, 400), yaw, pitch);
    expect(side && side.value(), "FD-05: 88 degrees to the side is within the cone");
    const auto behind = tactical::in_follow_cone(self, at(-100, 50), yaw, pitch);
    expect(behind && !behind.value(), "FD-05: a craft behind cannot be followed");
    const auto far = tactical::in_follow_cone(self, at(501, 0), yaw, pitch);
    expect(far && !far.value(), "FD-05: a craft beyond the attack distance cannot be followed");
    expect(tactical::within_combat_cell_reach(at(200, 200), tactical::CombatCell{0, 0}),
        "FD-03: the cell point itself is within reach");
    expect(tactical::within_combat_cell_reach(at(400, 200), tactical::CombatCell{0, 0}),
        "FD-03: 200 units off the point is within 283");
    expect(!tactical::within_combat_cell_reach(at(484, 200), tactical::CombatCell{0, 0}),
        "FD-03: 284 units off the point is beyond 400 / sqrt(2)");
    const auto odd = tactical::combat_cell_of(at(250, 450));
    expect(odd.y == 1 && odd.x == 0, "WU-25: odd rows are shifted by half a cell");
    const auto odd_point = tactical::combat_cell_point(odd, units(7));
    expect(odd_point.x == units(400) && odd_point.y == units(600) && odd_point.z == units(7), "WU-25: an odd row's cell point");
}

// C-24 (FD-09): when the target squadron is gone and no enemy squadron is joined in the cell,
// combat ends: the squadron leaves its cell and holds where its leader was. Squadron 20's single
// craft is fragile; a second rebel squadron (30) is joined in the cell too in the variant, and
// the attacker then takes it at once.
[[nodiscard]] Session fight_to_the_end(const bool third) {
    Session fixture;
    fixture.setup.seed = seed;
    fixture.setup.players = players();
    fixture.setup.units = {unit(10, trio_type, empire, at(-350, 0)), unit(11, craft_type, empire, at(-350, 0)),
        unit(12, craft_type, empire, at(-370, 20)), unit(13, craft_type, empire, at(-370, -20)),
        unit(20, single_type, rebel, at(350, 0), 180), unit(21, craft_type, rebel, at(350, 0), 180)};
    fixture.setup.squadrons = {{10, {11, 12, 13}}, {20, {21}}};
    if (third) {
        fixture.setup.units.push_back(unit(30, single_type, rebel, at(350, 300), 180));
        fixture.setup.units.push_back(unit(31, craft_type, rebel, at(350, 300), 180));
        fixture.setup.squadrons.push_back({30, {31}});
        fixture.orders.push_back({rebel, 30, tactical::AttackPayload{10}});
    }
    fixture.motion = motion();
    fixture.durability = durability(40);
    fixture.orders.push_back({empire, 10, tactical::AttackPayload{20}});
    // The synthetic guns seldom hit a dogfighting craft: scripted damage (HD-30) kills the lone
    // craft once the squadrons have paired off.
    fixture.orders.push_back({rebel, 21, tactical::DamagePayload{units(1000)}, {}, 400});
    return fixture;
}

void test_fight_end() {
    const auto fixture = fight_to_the_end(false);
    std::optional<std::uint64_t> died;
    std::optional<tactical::SquadronState> after;
    std::optional<math::Vec3> leader_before;
    const auto hashes = run(fixture, 1, 1200, [&](const tactical::TacticalSession& session, const tactical::TacticalTick& tick) {
        const auto defender = session.squadron_state(20);
        const auto attacker = session.squadron_state(10);
        if (!died && !defender) {
            died = tick.completed_tick;
            after = attacker;
        } else if (!died) {
            leader_before = position_of(*tick.snapshot, 11);
        }
    });
    expect(died.has_value(), "C-24: the lone craft dies");
    if (died && after) {
        // The attacker ends combat in the frame after the loss.
        expect(after->target == 20 || after->target == eawr::sim::invalid_entity_id, "C-24: the target is dropped");
    }
    std::optional<tactical::SquadronState> settled;
    static_cast<void>(run(fixture, 1, died ? static_cast<int>(*died) + 2 : 0,
        [&](const tactical::TacticalSession& session, const tactical::TacticalTick&) { settled = session.squadron_state(10); }));
    if (died && settled && leader_before) {
        expect(settled->target == eawr::sim::invalid_entity_id, "C-24 (FD-09): no enemy squadron in the cell: combat ends");
        expect(!settled->cell && !settled->joined, "C-24 (FD-09): the squadron leaves its cell");
        expect(distance(settled->anchor, at(-350, 0)) > 100.0, "C-24 (FD-09): it no longer holds its start point");
        // FM-24 (#687): it holds the point of the idle cell it claims there, at most a cell's
        // half-diagonal (85 units) plus a frame's flight off.
        expect(settled->idle_cell.has_value()
                && settled->anchor == tactical::idle_cell_point(*settled->idle_cell, settled->anchor.z),
            "C-24 (FM-24): it holds an idle cell's point");
        expect(distance({settled->anchor.x, settled->anchor.y, Fixed{}}, {leader_before->x, leader_before->y, Fixed{}}) < 100.0,
            "C-24 (FD-09): it holds the idle cell where its leader was when the fight ended");
    }
    expect_workers(fixture, 1200, hashes, "C-24");

    const auto crowded = fight_to_the_end(true);
    std::optional<std::uint64_t> gone;
    std::optional<EntityId> next;
    const auto crowded_hashes = run(crowded, 1, 1200, [&](const tactical::TacticalSession& session, const tactical::TacticalTick& tick) {
        const auto attacker = session.squadron_state(10);
        if (!gone && !session.squadron_state(20)) gone = tick.completed_tick;
        if (gone && !next && attacker && tick.completed_tick == *gone + 1) next = attacker->target;
    });
    const auto third = run(crowded, 1, 1);
    expect(gone.has_value(), "C-24: squadron 20 dies with 30 in the fight");
    if (gone && next) {
        expect(*next == 30, "C-24 (FD-09): the attacker takes the enemy squadron joined in its cell, got "
            + std::to_string(*next));
    }
    expect_workers(crowded, 1200, crowded_hashes, "C-24 crowded");
}

// C-28 (#467 SP-02, SP-03 with FD-06): a craft killed in a dogfight, with a chase running, still
// spins away: its unit_destroyed and spin_away_started events come in the tick of the kill, the
// snapshots list it as spinning, and spin_away_ended follows the SP-04/SP-06 service interval.
void test_spin_in_dogfight() {
    auto fixture = fight_to_the_end(false);
    fixture.motion.squadrons.craft[0].spin_away = tactical::SpinAwayProfile{units(1), units(2)};
    std::optional<std::uint64_t> destroyed;
    std::optional<std::uint64_t> started;
    std::optional<std::uint64_t> ended;
    std::size_t spinning_ticks = 0;
    std::optional<Fixed> death_roll;
    bool chased = false;
    const auto hashes = run(fixture, 1, 1200, [&](const tactical::TacticalSession& session, const tactical::TacticalTick& tick) {
        for (const auto& event : tick.snapshot->events()) {
            if (event.unit != 21) continue;
            if (event.kind == tactical::EventKind::unit_destroyed) destroyed = event.tick;
            if (event.kind == tactical::EventKind::spin_away_started) started = event.tick;
            if (event.kind == tactical::EventKind::spin_away_ended) ended = event.tick;
        }
        for (const auto& craft : tick.snapshot->spinning()) {
            if (craft.entity_id != 21) continue;
            ++spinning_ticks;
            if (!death_roll) death_roll = craft.roll;
        }
        if (!destroyed) {
            for (const auto id : {11U, 12U, 13U}) {
                const auto craft = session.craft_state(id);
                chased = chased || (craft && craft->chase == 21);
            }
        }
    });
    expect(destroyed.has_value(), "C-28: the craft is killed");
    expect(chased, "C-28: it was chased when it died");
    expect(destroyed && started && *started == *destroyed, "C-28: spin_away_started comes with its unit_destroyed");
    // SP-04 (also SC-04): inherited -20*k-degree roll reaches the zero sentinel after k
    // services and rebuilds the path, adding k to SP-06's ordinary 60 services.
    const auto roll_step = units(20).raw();
    const auto rebuild = death_roll && death_roll->raw() < 0 && death_roll->raw() % roll_step == 0
        ? static_cast<std::uint64_t>(-death_roll->raw() / roll_step) : 0U;
    const auto services = 60U + rebuild;
    expect(death_roll.has_value(), "C-28: the dead copy retains its kill-frame roll");
    expect(started && ended && *ended == *started + services,
        "C-28 (SP-04/SP-06): spin_away_ended follows the roll-conditioned service interval");
    expect(spinning_ticks == services, "C-28: snapshots show the roll-conditioned spin for "
        + std::to_string(services) + " ticks, got " + std::to_string(spinning_ticks));
    expect_workers(fixture, 1200, hashes, "C-28");
}

// C-25 (FD-10, FD-11): a lone craft ordered through a frigate's centre steers away from it when
// the frigate has a space layer and flies through it when it has none; two craft ordered through
// each other do not avoid each other.
void test_avoidance() {
    const auto through = [](const bool layered, double& closest, bool& relaxed) {
        Session fixture;
        fixture.setup.seed = seed;
        fixture.setup.players = players();
        fixture.setup.units = {unit(10, single_type, empire, at(-800, 0)), unit(11, craft_type, empire, at(-800, 0)),
            unit(40, frigate_type, rebel, at(0, 0))};
        fixture.setup.squadrons = {{10, {11}}};
        fixture.motion = motion(layered);
        fixture.durability = durability(1000000);
        fixture.orders = {{empire, 10, tactical::MovePayload{at(1500, 0)}}};
        closest = 1e9;
        relaxed = false;
        const auto hashes = run(fixture, 1, 400, [&](const tactical::TacticalSession& session, const tactical::TacticalTick& tick) {
            if (const auto craft = position_of(*tick.snapshot, 11)) closest = std::min(closest, distance(*craft, at(0, 0)));
            if (const auto flight = session.craft_state(11)) relaxed = relaxed || flight->relaxation > 0;
        });
        expect_workers(fixture, 400, hashes, layered ? "C-25 layered" : "C-25 unlayered");
    };
    double open = 0;
    double avoided = 0;
    bool open_relaxed = true;
    bool avoided_relaxed = false;
    through(false, open, open_relaxed);
    through(true, avoided, avoided_relaxed);
    std::cout << "C-25: closest approach without a layer " << open << ", with one " << avoided << '\n';
    expect(open < 10.0, "C-25: a frigate without a space layer is flown through");
    expect(!open_relaxed, "C-25: no avoidance without a layered ship");
    expect(avoided_relaxed, "C-25 (FD-10): the craft steers away from the layered frigate");
    // FoC only reacts inside the ship's bounds plus its speed, so a craft heading straight at a
    // ship's centre still passes close (S-28: 16 to 51 units off the corvette's centre).
    expect(avoided > open, "C-25 (FD-10): the craft passes further off the layered frigate's centre");

    // FD-11: two craft on crossing moves pass through each other.
    Session crossing;
    crossing.setup.seed = seed;
    crossing.setup.players = players();
    crossing.setup.units = {unit(10, single_type, empire, at(-600, 0)), unit(11, craft_type, empire, at(-600, 0)),
        unit(20, single_type, empire, at(600, 0), 180), unit(21, craft_type, empire, at(600, 0), 180)};
    crossing.setup.squadrons = {{10, {11}}, {20, {21}}};
    crossing.motion = motion();
    crossing.durability = durability(1000000);
    crossing.orders = {{empire, 10, tactical::MovePayload{at(1500, 0)}}, {empire, 20, tactical::MovePayload{at(-1500, 0)}}};
    double closest = 1e9;
    static_cast<void>(run(crossing, 1, 300, [&](const tactical::TacticalSession&, const tactical::TacticalTick& tick) {
        const auto a = position_of(*tick.snapshot, 11);
        const auto b = position_of(*tick.snapshot, 21);
        if (a && b) closest = std::min(closest, distance(*a, *b));
    }));
    expect(closest < 10.0, "C-25 (FD-11): craft do not avoid each other, closest " + std::to_string(closest));
}

// C-26 (FD-04; recording S-98): a squadron ordered to attack a frigate keeps attacking it when a
// squadron engages it; FoC's retaliation needs the target squadron in a combat cell, which a
// squadron attacking a ship never records (S-98: the TIE bombers keep the corvette).
void test_intercepted() {
    Session fixture;
    fixture.setup.seed = seed;
    fixture.setup.players = players();
    fixture.setup.units = {unit(10, trio_type, empire, at(-300, -700)), unit(11, craft_type, empire, at(-300, -700)),
        unit(12, craft_type, empire, at(-320, -680)), unit(13, craft_type, empire, at(-320, -720)),
        unit(20, trio_type, rebel, at(-400, 0)), unit(21, craft_type, rebel, at(-400, 0)),
        unit(22, craft_type, rebel, at(-420, 20)), unit(23, craft_type, rebel, at(-420, -20)),
        unit(40, frigate_type, empire, at(1200, 0))};
    fixture.setup.squadrons = {{10, {11, 12, 13}}, {20, {21, 22, 23}}};
    fixture.motion = motion();
    fixture.durability = durability(1000000);
    fixture.orders = {{rebel, 20, tactical::AttackPayload{40}}, {empire, 10, tactical::AttackPayload{20}}};
    bool kept = true;
    bool engaged = false;
    const auto hashes = run(fixture, 1, 600, [&](const tactical::TacticalSession& session, const tactical::TacticalTick& tick) {
        if (tick.completed_tick < 3) return;
        if (const auto state = session.squadron_state(20)) kept = kept && state->target == 40;
        if (const auto state = session.squadron_state(10)) engaged = engaged || state->cell.has_value();
    });
    expect_workers(fixture, 600, hashes, "C-26");
    expect(engaged, "C-26 (FD-02): the interceptors record a cell around the squadron");
    expect(kept, "C-26 (FD-04, S-98): the squadron attacking the frigate keeps it as its target");
}

// C-29 (#531, OR-23; #585): a squadron ordered onto a hardpoint of a ship that a dogfight retargets
// does not keep the hardpoint. Squadrons 10 and 20 fight in one cell, 20 on one of 10's craft;

} // namespace dogfight_test_support
