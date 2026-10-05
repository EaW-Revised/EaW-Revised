#include "fighter_support.hpp"

namespace fighter_test_support {

struct Launch {
    std::uint64_t frame{};
    std::uint32_t entry{};
};

// Services the hangar every frame in [from, to), recording the launches.
[[nodiscard]] std::vector<Launch> service(const tactical::SpawnerProfile& profile, tactical::SpawnerState& state,
    const std::uint64_t from, const std::uint64_t to, const std::vector<bool>& intact = {true}) {
    std::vector<Launch> launches;
    for (auto frame = from; frame < to; ++frame) {
        if (const auto decision = tactical::service_spawner(profile, state, seed, frame, 1, intact)) {
            launches.push_back({frame, decision->entry});
        }
    }
    return launches;
}

// C-01, C-02 (FL-01 to FL-05, FL-08).
void test_hangar() {
    const auto profile = spawner();
    auto state = tactical::initial_spawner(seed, 1, 1);
    const auto first = state.next_service_frame;
    expect(first >= 1 && first <= 30, "FL-01: the first service is within 30 frames of creation");
    expect(tactical::initial_spawner(seed, 1, 1) == state, "FL-01: the service draw is keyed");

    auto launches = service(profile, state, 1, first + 400);
    expect(launches.size() == 2, "C-01: two launches while both squadrons live");
    if (launches.size() == 2) {
        expect(launches[0].frame == first, "C-01: the first service launches at once");
        expect(launches[1].frame == first + 150, "C-01: the second launch waits the 150-frame delay");
        expect(launches[0].entry != launches[1].entry, "C-01: both entries launch once");
    }
    expect(state.entries.size() == 2 && state.entries[0].alive == 1 && state.entries[1].alive == 1, "C-01: both alive");
    expect(state.entries[0].remaining == 2 && state.entries[1].remaining == 0, "FL-02: starting plus reserve, one used");

    // C-02: losing A (entry 0) while every entry is full delays the next launch a full delay.
    const auto lost = first + 400;
    tactical::squadron_lost(profile, state, 0, lost);
    expect(state.entries[0].alive == 0, "FL-08: the entry's alive count drops");
    launches = service(profile, state, lost, lost + 400);
    expect(launches.size() == 1, "C-02: A's reserve replaces it once");
    if (!launches.empty()) {
        expect(launches[0].entry == 0, "C-02: the replacement is A's");
        expect(launches[0].frame >= lost + 150 && launches[0].frame < lost + 180, "C-02: at the first service after the delay");
    }
    // B has nothing left: losing it launches nothing.
    tactical::squadron_lost(profile, state, 1, lost + 400);
    launches = service(profile, state, lost + 400, lost + 1000);
    expect(launches.empty(), "C-02: an entry with no reserve left is not replaced");
    // A's last reserve squadron, then nothing more.
    tactical::squadron_lost(profile, state, 0, lost + 1000);
    launches = service(profile, state, lost + 1000, lost + 1400);
    expect(launches.size() == 1 && state.entries[0].remaining == 0, "FL-04: the last reserve launches");
    tactical::squadron_lost(profile, state, 0, lost + 1400);
    expect(service(profile, state, lost + 1400, lost + 1800).empty(), "FL-04: an exhausted entry launches nothing");

    // FL-08: a loss while an earlier entry is not full keeps the running delay.
    auto partial = tactical::initial_spawner(seed, 1, 1);
    static_cast<void>(service(profile, partial, 1, first + 1));
    const auto running = partial.next_spawn_frame;
    tactical::squadron_lost(profile, partial, partial.entries[0].alive == 1 ? 0U : 1U, first + 10);
    expect(partial.next_spawn_frame == running || partial.next_spawn_frame == first + 10 + 150,
        "FL-08: the delay restarts only when every entry before was full");

    // Unlimited reserve never runs out.
    auto unlimited_profile = profile;
    unlimited_profile.entries = {{squadron_a, 1, tactical::unlimited_reserve}};
    auto unlimited = tactical::initial_spawner(seed, 1, 1);
    std::size_t total = 0;
    for (std::uint64_t round = 0; round < 5; ++round) {
        const auto from = 1 + round * 300;
        total += service(unlimited_profile, unlimited, from, from + 300).size();
        tactical::squadron_lost(unlimited_profile, unlimited, 0, from + 299);
    }
    expect(total == 5 && unlimited.entries[0].remaining == tactical::unlimited_reserve, "FL-02: a negative reserve never runs out");
}

// C-03 (FL-03).
void test_destroyed_bay() {
    auto state = tactical::initial_spawner(seed, 1, 1);
    expect(service(spawner(), state, 1, 600, {false}).empty(), "C-03: a spawner without a standing bay launches nothing");
    auto bayless = spawner();
    bayless.bays.clear();
    auto none = tactical::initial_spawner(seed, 1, 1);
    expect(service(bayless, none, 1, 600, {}).empty(), "C-03: a spawner without bays launches nothing");
}

void test_validation() {
    expect(static_cast<bool>(tactical::validate_squadron_table(motion().squadrons)), "the synthetic table is valid");
    auto unordered = motion().squadrons;
    std::swap(unordered.squadrons[0], unordered.squadrons[1]);
    expect(!tactical::validate_squadron_table(unordered), "squadron types must strictly increase");
    auto stranger = motion().squadrons;
    stranger.squadrons[0].members[0] = 999;
    expect(!tactical::validate_squadron_table(stranger), "members must be craft of the table");
    auto offsets = motion().squadrons;
    offsets.squadrons[0].offsets.pop_back();
    expect(!tactical::validate_squadron_table(offsets), "one offset per member");
    auto negative = motion().squadrons;
    negative.spawners[0].entries[0].starting = -1;
    expect(!tactical::validate_squadron_table(negative), "spawn counts are nonnegative");
    auto overflow = motion().squadrons;
    overflow.spawners[0].entries[0].starting = 2;
    overflow.spawners[0].entries[0].reserve = std::numeric_limits<std::int32_t>::max();
    expect(!tactical::validate_squadron_table(overflow), "a finite starting plus reserve count must fit in int32");
    auto still = motion().squadrons;
    still.craft[0].max_speed = Fixed{};
    expect(!tactical::validate_squadron_table(still), "a craft needs a maximum speed");
    auto defenseless = motion().squadrons;
    defenseless.craft[0].out_of_combat_defense = units(-1);
    expect(static_cast<bool>(tactical::validate_squadron_table(defenseless)), "FoC's -1 out-of-combat adjustment is valid");
    defenseless.craft[0].out_of_combat_defense = units(-2);
    expect(!tactical::validate_squadron_table(defenseless), "an adjustment below -1 is rejected (DG-26's Q24 bound)");
    defenseless.craft[0].out_of_combat_defense = units(2);
    expect(!tactical::validate_squadron_table(defenseless), "an adjustment above 1 is rejected");
    auto setup = tactical::TacticalSetup{};
    setup.players = players();
    auto bad = motion();
    bad.squadrons = stranger;
    expect(!tactical::TacticalSession::create(setup, {}, {}, bad), "a session rejects an invalid squadron table");
}

void test_finite_reserve_boundary() {
    auto profile = spawner();
    profile.entries = {{squadron_a, 1, 0}};
    auto state = tactical::initial_spawner(seed, 1, 1);
    state.ready = true;
    state.entries = {{0, -2}}; // Only -1 means unlimited, even for a malformed restored state.
    expect(service(profile, state, 1, state.next_service_frame + 1).empty(),
        "a negative remaining count other than -1 cannot launch");
}

// FM-12 (E75-18, recording S-28): FoC clamps the catch-up share only for a follower ahead of its
// slot; a follower far behind extrapolates past the 1.5x bound (the recorded followers reach
// about 12 units per frame behind a leader at 5.4).
void test_formation_catch_up() {
    auto profile = motion().squadrons.craft.front();
    tactical::CraftView leader{1, at(0, 0), {}, &profile};
    leader.state.velocity = {profile.max_speed, Fixed{}, Fixed{}};
    tactical::CraftView follower{2, at(-200, 0), leader.state, &profile};
    tactical::CraftFrame frame;
    frame.self = &follower;
    frame.leader = &leader;
    frame.attacking = true;
    frame.target_craft = true;
    frame.target_position = leader.position;
    frame.formation_tolerance = units(20);
    for (int tick = 0; tick < 10; ++tick) {
        const auto stepped = tactical::step_craft(frame);
        expect(static_cast<bool>(stepped), "the lagging follower advances");
        if (!stepped) return;
        follower.position = stepped.value().position;
        follower.state = stepped.value().state;
    }
    const Fixed fast = Fixed::from_raw(one * 81 / 10);
    expect(follower.state.velocity.x > fast,
        "FM-12: a follower 200 behind a leader at 5.4 keeps accelerating past 1.5 times its speed");
}

void test_coordinate_limit() {
    auto profile = motion().squadrons.craft.front();
    tactical::CraftView craft{1, at(-262144, -262144), {}, &profile};
    tactical::CraftFrame frame;
    frame.self = &craft;
    frame.leader = &craft;
    frame.hold = at(262144, 262144);
    expect(static_cast<bool>(tactical::step_craft(frame)), "accepted opposite map corners advance while idle");
    frame.attacking = true;
    frame.target_position = frame.hold;
    expect(static_cast<bool>(tactical::step_craft(frame)), "accepted opposite map corners advance on an attack run");

    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(10, squadron_a, empire, at(262144, 262144)),
        unit(11, craft_type, empire, at(-262144, -262144)),
        unit(12, craft_type, empire, at(-262144, -262144))};
    setup.squadrons = {{10, {11, 12}}};
    auto table = motion();
    table.squadrons.spawners.clear();
    auto created = tactical::TacticalSession::create(setup, {}, {}, table);
    expect(static_cast<bool>(created), "opposite map corners pass setup validation");
    if (!created) return;
    auto session = std::move(created).value();
    const eawr::platform::ThreadWorkerAdapter executor(1);
    expect(static_cast<bool>(session.step(executor)), "a validated opposite-corner squadron advances one tick");
}

// C-11, DG-26 (#409, #469): a craft is out of combat while idle and while its squadron flies its
// approach beyond the strafe reach (FA-07); a straight dive beyond the reach after it (FA-01) and
// every leg inside the reach are in combat, defense 0.
void test_out_of_combat() {
    auto profile = motion().squadrons.craft.front();
    profile.out_of_combat_defense = units(-1);
    tactical::CraftView craft{1, at(0, 0), {}, &profile};
    craft.state.velocity = {profile.max_speed, Fixed{}, Fixed{}};
    tactical::CraftFrame frame;
    frame.self = &craft;
    frame.leader = &craft;
    frame.hold = at(0, 0);
    const auto defense = [&] {
        const auto stepped = tactical::step_craft(frame);
        expect(static_cast<bool>(stepped), "the craft steps");
        return stepped ? stepped.value().defense : Fixed::from_raw(one);
    };
    expect(defense() == units(-1), "DG-26: an idle craft is out of combat");
    frame.attacking = true;
    frame.approach = true;
    frame.target_position = at(600, 0);
    frame.target_radius = units(50);
    expect(defense() == units(-1), "DG-26: the approach beyond the strafe reach (200 + 50) is out of combat");
    frame.approach = false;
    expect(defense() == Fixed{}, "DG-26: a straight dive beyond the strafe reach after the approach is in combat");
    frame.approach = true;
    frame.target_position = at(240, 0);
    expect(defense() == Fixed{}, "DG-26: inside the strafe reach the craft is in combat");
    profile.out_of_combat_defense = Fixed{};
    frame.target_position = at(600, 0);
    frame.approach = true;
    expect(defense() == Fixed{}, "DG-26: a craft type without the adjustment keeps defense 0");
}

// C-13, FA-07 (#469): on the approach beyond the strafe reach a follower forms up on its slot and
// the leader climbs to its layer height along the line to the target; after it both dive
// straight at the target (FA-01).
void test_approach() {
    auto profile = motion().squadrons.craft.front();
    profile.layer_z = Fixed{};
    tactical::CraftView leader{1, at(0, 0, -100), {}, &profile};
    leader.state.velocity = {profile.max_speed, Fixed{}, Fixed{}};
    tactical::CraftView follower{2, at(0, 0, -100), leader.state, &profile};
    tactical::CraftFrame frame;
    frame.leader = &leader;
    frame.leader_offset = at(0, 0);
    frame.own_offset = at(-10, 10);
    frame.formation_tolerance = units(20);
    frame.attacking = true;
    frame.target_position = at(2000, 0, -100);
    const auto step = [&](const tactical::CraftView& self, const bool approach) {
        frame.self = &self;
        frame.approach = approach;
        const auto stepped = tactical::step_craft(frame);
        expect(static_cast<bool>(stepped), "the craft steps");
        return stepped ? stepped.value() : tactical::CraftStep{};
    };
    expect(step(follower, false).state.yaw == Fixed{}, "FA-01: a follower dives straight at a target dead ahead");
    expect(step(follower, true).state.yaw > Fixed{}, "FA-07: a follower turns toward its slot on the left");
    expect(step(leader, false).state.pitch == Fixed{}, "FA-01: the leader keeps level toward a level target");
    expect(step(leader, true).state.pitch < Fixed{}, "FA-07: the leader climbs toward its layer height");
    profile.out_of_combat_defense = units(-1);
    expect(step(leader, true).defense == units(-1) && step(leader, false).defense == Fixed{},
        "DG-26: the approach is out of combat, the straight dive in combat");
}

// FL-06 facing: a launch along (1, 0, -1) yaws 0, pitches 45 (nose down) at full speed.
void test_launch_state() {
    const auto state = tactical::launch_state(at(1, 0, -1), units(5));
    expect(static_cast<bool>(state), "launch facing builds");
    if (!state) return;
    const auto near = [](const Fixed value, const std::int64_t expected_thousandths) {
        const auto difference = value.raw() * 1000 / one - expected_thousandths;
        return difference >= -2 && difference <= 2;
    };
    expect(near(state.value().yaw, 0) && near(state.value().pitch, 45000), "FL-06: yaw 0, pitch 45");
    expect(near(state.value().velocity.x, 3536) && near(state.value().velocity.z, -3536), "FL-06: full speed along the vector");
}

// C-04 (FL-06, FL-07): the station's squadron leaves its bay; craft first, then the container.
void test_launch() {
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(1, carrier_type, empire, at(0, 0))};
    auto table = motion();
    table.squadrons.spawners[0].entries = {{squadron_a, 1, 0}};
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table);
    expect(static_cast<bool>(created), "C-04: the session builds");
    if (!created) return;
    auto session = std::move(created).value();
    const eawr::platform::ThreadWorkerAdapter executor(2);
    std::optional<std::uint64_t> launched;
    for (std::uint64_t tick = 0; tick < 40 && !launched; ++tick) {
        const auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "C-04: steps");
        if (!stepped) return;
        if (instance(*stepped.value().snapshot, 4) != nullptr) launched = stepped.value().completed_tick;
    }
    expect(launched.has_value() && *launched <= 30, "C-04: the first service launches within 30 frames");
    const auto snapshot = session.snapshot();
    const auto* first = instance(*snapshot, 2);
    const auto* second = instance(*snapshot, 3);
    const auto* container = instance(*snapshot, 4);
    expect(first != nullptr && second != nullptr && container != nullptr, "C-04: two craft (2, 3) then their container (4)");
    if (first == nullptr || second == nullptr || container == nullptr) return;
    expect(position(*first) == at(20, 0, -10) && position(*second) == at(20, 0, -10), "C-04: every craft starts at the bay");
    expect(position(*container) == at(20, 0, -10), "C-04: the container starts at the bay");
    const auto& rows = first->fixed_transform.rows;
    expect(rows[0][0].raw() > one / 2 && rows[2][0].raw() < -one / 2 && rows[1][0].raw() == 0,
        "C-04: craft face along the spawn vector (forward, nose down)");
    // The next frame they fly on at full speed along the vector.
    const auto stepped = session.step(executor);
    expect(static_cast<bool>(stepped), "C-04: steps after the launch");
    if (!stepped) return;
    const auto* moved = instance(*stepped.value().snapshot, 2);
    expect(moved != nullptr && position(*moved).x > at(23, 0).x && position(*moved).z < at(0, 0, -12).z,
        "C-04: craft leave along the vector");
    expect(!session.combat_state(4).has_value() || session.combat_state(4)->attack_target == 0, "the container never attacks");
}

// #518: a launched squadron is a squadron like a tick-zero one. The snapshot lists it (its
// container and craft) from the tick it launches, next to the setup's; a player's move given to
// its container moves every craft, as one unit.
void test_launched_squadron() {
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(1, carrier_type, empire, at(0, 0)), unit(10, squadron_b, empire, at(-500, 0)),
        unit(11, craft_type, empire, at(-500, 0))};
    setup.squadrons = {{10, {11}}};
    auto table = motion();
    table.squadrons.spawners[0].entries = {{squadron_a, 1, 0}};
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table);
    expect(static_cast<bool>(created), "#518: the session builds");
    if (!created) return;
    auto session = std::move(created).value();
    const std::vector<tactical::Squadron> start{{10, {11}}};
    expect(std::vector<tactical::Squadron>(session.snapshot()->squadrons().begin(), session.snapshot()->squadrons().end())
               == start,
        "#518: the tick-zero snapshot lists the setup's squadron");
    const eawr::platform::ThreadWorkerAdapter executor(2);
    std::optional<std::uint64_t> launched;
    for (std::uint64_t tick = 0; tick < 40 && !launched; ++tick) {
        const auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "#518: steps");
        if (!stepped) return;
        if (instance(*stepped.value().snapshot, 14) != nullptr) launched = stepped.value().completed_tick;
    }
    expect(launched.has_value(), "#518: the squadron (craft 12, 13; container 14) launches");
    if (!launched) return;
    const std::vector<tactical::Squadron> both{{10, {11}}, {14, {12, 13}}};
    const auto listed = session.snapshot()->squadrons();
    expect(std::vector<tactical::Squadron>(listed.begin(), listed.end()) == both,
        "#518: the snapshot lists the launched squadron after the setup's");
    const auto destination = at(-1500, 1500);
    expect(static_cast<bool>(session.submit({{*launched + 1, empire, 1}, {14}, tactical::MovePayload{destination}})),
        "#518: the player's move to the launched squadron's container submits");
    for (int tick = 0; tick < 600; ++tick) {
        const auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "#518: steps after the order");
        if (!stepped) return;
    }
    const auto snapshot = session.snapshot();
    const auto near = [&](const eawr::sim::EntityId id) {
        const auto* found = instance(*snapshot, id);
        if (found == nullptr) return false;
        const auto where = position(*found);
        const double dx = static_cast<double>(where.x.raw() - destination.x.raw()) / static_cast<double>(one);
        const double dy = static_cast<double>(where.y.raw() - destination.y.raw()) / static_cast<double>(one);
        return std::sqrt(dx * dx + dy * dy) < 250.0;
    };
    expect(near(14) && near(12) && near(13), "#518: the launched squadron and both its craft reach the move's destination");
    const auto* idle = instance(*snapshot, 11);
    expect(idle != nullptr && !near(11), "#518: the order moves only the launched squadron");
}

// C-05 (FT-02, FT-03): priority 1 beats a nearer priority 3; nothing beyond chase + attack reach.

// C-08 (FT-06, FT-07): a launched squadron takes no target for its first second, then attacks.
// The frigate is within the craft's attack distance, yet until then no craft (11, 12) holds a
// target of its own or fires; once engaged they do.
void test_launch_idle() {
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(1, carrier_type, empire, at(0, 0)), unit(10, frigate_type, rebel, at(300, 0))};
    auto table = motion();
    table.squadrons.spawners[0].entries = {{squadron_a, 1, 0}};
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
    expect(static_cast<bool>(created), "C-08: the session builds");
    if (!created) return;
    auto session = std::move(created).value();
    const eawr::platform::ThreadWorkerAdapter executor(2);
    std::optional<std::uint64_t> launched;
    std::optional<std::uint64_t> engaged;
    std::optional<std::uint64_t> fired;
    std::vector<std::string> idle_breaks;
    for (std::uint64_t tick = 0; tick < 240 && !fired; ++tick) {
        const auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "C-08: steps");
        if (!stepped) return;
        const auto completed = stepped.value().completed_tick;
        if (!launched && instance(*stepped.value().snapshot, 11) != nullptr) launched = completed;
        const auto state = session.combat_state(11);
        if (launched && !engaged && state && state->attack_target == 10) engaged = completed;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.shooter != 11 && event.shooter != 12) continue;
            if (!engaged) {
                idle_breaks.push_back("tick " + std::to_string(completed) + " craft " + std::to_string(event.shooter)
                    + " " + std::string(tactical::to_string(event.kind)));
            } else if (event.kind == tactical::CombatEventKind::weapon_fired && !fired) {
                fired = completed;
            }
        }
        if (!launched || engaged) continue;
        for (const auto craft : {eawr::sim::EntityId{11}, eawr::sim::EntityId{12}}) {
            const auto held = session.combat_state(craft);
            if (held && held->attack_target != eawr::sim::invalid_entity_id) {
                idle_breaks.push_back("tick " + std::to_string(completed) + " craft " + std::to_string(craft)
                    + " holds target " + std::to_string(held->attack_target));
            }
        }
    }
    expect(launched.has_value(), "C-08: the squadron (11, 12; container 13) launches");
    expect(engaged.has_value(), "C-08: the squadron engages the frigate");
    expect(idle_breaks.empty(),
        "C-08: no craft targets or fires before its squadron's first target, got "
            + (idle_breaks.empty() ? std::string{} : idle_breaks.front()));
    expect(fired.has_value(), "C-08: an engaged craft fires at the frigate");
    if (!launched || !engaged) return;
    expect(*engaged >= *launched + 29 && *engaged <= *launched + 31,
        "C-08: the squadron's first target comes one second after the launch, got " + std::to_string(*engaged - *launched));
}

// C-06: launch, attack, losses and a replacement hash the same with 1, 2, 4 and 8 workers.
void test_worker_equality() {
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(1, carrier_type, empire, at(0, 0)), unit(2, frigate_type, rebel, at(900, 0)),
        unit(3, corvette_type, rebel, at(700, 300))};
    const auto hashes = [&setup](const std::size_t workers) {
        std::vector<std::string> rows;
        auto table = motion();
        table.squadrons.spawners[0].entries = {{squadron_a, 1, 1}};
        auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
        expect(static_cast<bool>(created), "C-06: the session builds");
        if (!created) return rows;
        auto session = std::move(created).value();
        // Kill the first squadron's craft (4, 5; container 6) at tick 120 to force its replacement.
        const auto kill = tactical::PlayerCommand{{120, empire, 1}, {4, 5}, tactical::DamagePayload{units(1000)}};
        expect(static_cast<bool>(session.submit(kill)), "C-06: the kill submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        bool replaced = false;
        for (int tick = 0; tick < 450; ++tick) {
            const auto stepped = session.step(executor);
            if (!stepped) {
                expect(false, "C-06: step failed: " + stepped.error().message);
                break;
            }
            rows.push_back(stepped.value().state_sha256);
            replaced = replaced || instance(*stepped.value().snapshot, 9) != nullptr;
        }
        expect(replaced, "C-06: the reserve squadron (7, 8; container 9) replaced the lost one");
        return rows;
    };
    const auto reference = hashes(1);
    expect(reference.size() == 450, "C-06: 450 ticks");
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(hashes(workers) == reference, "C-06: " + std::to_string(workers) + " workers hash like one");
    }
}

// FO-01 to FO-03 (#424): player orders to a squadron's team container. A tick-zero squadron (10;
// craft 11, 12) of craft that cruise at 5.4 per frame moves 1700 units, stops, and attacks a

// An empty squadron table changes nothing: craft do not move and nothing launches.
void test_empty_table() {
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(1, carrier_type, empire, at(0, 0)), unit(10, squadron_a, empire, at(100, 0)),
        unit(11, craft_type, empire, at(100, 0))};
    setup.squadrons = {{10, {11}}};
    auto session = tactical::TacticalSession::create(setup, sensors(), durability()).value();
    const eawr::platform::ThreadWorkerAdapter executor(1);
    for (int tick = 0; tick < 60; ++tick) static_cast<void>(session.step(executor));
    const auto snapshot = session.snapshot();
    expect(snapshot->instances().size() == 3, "an empty table launches nothing");
    const auto* craft = instance(*snapshot, 11);
    expect(craft != nullptr && position(*craft) == at(100, 0), "an empty table leaves craft in place");
}

void test_staged_flight_rollback() {
    class LateFailure final : public eawr::sim::PartitionExecutor {
    public:
        explicit LateFailure(const std::string_view phase) : phase_(phase) {}
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            return inline_.execute(count, partition);
        }
        eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            if (phase != phase_) return execute(count, partition);
            return eawr::core::Result<void>::failure(eawr::core::Diagnostic{
                .code = std::string(tactical::diagnostic_codes::worker_failure),
                .severity = eawr::core::Severity::error,
                .message = "synthetic late phase failure",
                .logical_path = std::nullopt, .line = std::nullopt, .column = std::nullopt,
                .source_id = std::string("fighter-test")});
        }
    private:
        std::string_view phase_;
        eawr::sim::InlineExecutor inline_;
    };
    const eawr::platform::ThreadWorkerAdapter executor(4);
    for (const auto phase : {"unit-systems", "hangars", "visibility"}) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(1, carrier_type, empire, at(0, 0)), unit(10, frigate_type, rebel, at(300, 0))};
        auto created = tactical::TacticalSession::create(setup, sensors(), durability(), motion(), std::nullopt, combat());
        auto control_created = tactical::TacticalSession::create(setup, sensors(), durability(), motion(), std::nullopt, combat());
        expect(created && control_created, "flight rollback setups create");
        if (!created || !control_created) return;
        auto session = std::move(created).value();
        auto control = std::move(control_created).value();
        // The first successful tick launches craft and creates their mind and tree state.
        expect(session.step(executor) && control.step(executor), "flight rollback launch ticks execute");
        const auto hash = session.state_sha256();
        const auto snapshot = session.snapshot();
        const auto record = session.record();
        const LateFailure failure(phase);
        expect(!session.step(failure), "late phase failure is returned");
        expect(session.state_sha256() == hash && session.snapshot() == snapshot && session.record() == record,
            "late phase failure restores flight, mind, hangar, registry and collection state");
        const auto retry = session.step(executor);
        const auto expected = control.step(executor);
        expect(retry && expected && retry.value().state_sha256 == expected.value().state_sha256,
            "retry after a journal rollback hashes like an uninterrupted tick");
    }
}


} // namespace fighter_test_support
