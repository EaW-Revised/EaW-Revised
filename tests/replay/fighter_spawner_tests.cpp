#include "fighter_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    for (const std::size_t count : {65U, 186U, 257U}) {
        auto large = motion().squadrons;
        large.squadrons[0].members.assign(count, craft_type);
        large.squadrons[0].offsets.assign(count, at(0, 0));
        expect(static_cast<bool>(tactical::validate_squadron_table(large)),
            "FL-02: authored rosters above 64 are admitted");
    }
    {
        auto large = motion().squadrons;
        auto& profile = large.spawners.front();
        profile.entries.assign(65, {squadron_a, 0, 0});
        profile.entries.back().starting = 1;
        profile.bays.assign(256, profile.bays.front());
        expect(static_cast<bool>(tactical::validate_squadron_table(large)),
            "FL-02: more than 64 spawn entries and 255 bays are admitted");
        std::vector<bool> intact(256, false);
        intact.back() = true;
        auto state = tactical::initial_spawner(seed, 1, 1);
        const auto decision = tactical::service_spawner(profile, state, seed, state.next_service_frame, 1, intact);
        expect(decision && decision->entry == 64 && decision->bay == 255,
            "FL-02: the last large-list entry and bay actually launch");
        expect(tactical::free_garrison_bay(profile, seed, 1, 1, intact) == 255U,
            "FL-02: free garrisons use bays beyond the old bound too");
    }
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
    auto banked = motion().squadrons;
    for (const auto angle : {360, -40}) {
        banked.craft[0].bank_angle = units(angle);
        expect(static_cast<bool>(tactical::validate_squadron_table(banked)), "FM-03: modded bank angles load");
        expect(banked.craft[0].bank_angle == units(angle), "FM-03: the authored bank angle is preserved");
        auto setup = tactical::TacticalSetup{};
        setup.players = players();
        auto table = motion();
        table.squadrons = banked;
        expect(static_cast<bool>(tactical::TacticalSession::create(setup, {}, {}, table)),
            "FM-03: a session accepts a modded bank angle");
    }
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
    frame.attack_lane = tactical::LaneFlight{at(1, 0), {}, profile.max_speed, true};
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
// the leader climbs to its layer height along the installed segment; after it both dive
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
    frame.attack_lane = tactical::LaneFlight{at(1, 0), {}, profile.max_speed, true};
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

void test_attack_path_boundary() {
    auto profile = motion().squadrons.craft.front();
    profile.out_of_combat_defense = units(-1);
    tactical::SquadronState state;
    state.container = 10;
    state.approach = true;
    state.attack_path.emplace();
    tactical::SquadronAttackRoute route;
    route.identity = 10;
    route.nodes = {at(0, 0), at(100, 0), at(100, 100)};
    route.trackers = {{10, {}}, {11, {}}, {12, {}}};
    state.attack_path->stack.push_back(route);
    const std::array<eawr::sim::EntityId, 1> admitted{11};
    tactical::advance_squadron_attack_path(*state.attack_path, 11, at(101, 0), admitted);
    for (const auto& [id, tracker] : state.attack_path->stack.back().trackers) {
        expect(tracker.segment == 1, "FSC-74: leader half-plane crossing advances every roster tracker");
        expect(tracker.started == (id == 11), "FSC-73: only admitted movement marks its member started");
    }
    tactical::CraftView leader{11, at(100, 99), {}, &profile};
    leader.state.velocity = {profile.max_speed, {}, {}};
    tactical::CraftFrame frame;
    frame.self = frame.leader = &leader;
    frame.attacking = frame.approach = true;
    frame.target_position = at(2000, 2000);
    const auto read = [&] {
        const auto flight = tactical::squadron_attack_flight(*state.attack_path, 11, leader.position,
            profile.max_speed, {}, {});
        frame.attack_lane = flight ? flight.value() : std::nullopt;
        return tactical::step_craft(frame);
    };
    const auto before = read();
    expect(before && before.value().defense == units(-1), "FSC-74: unfinished path outside strafe reach is out of combat");
    tactical::advance_squadron_attack_path(*state.attack_path, 11, at(100, 100), admitted);
    const auto ended = read();
    expect(ended && !frame.attack_lane && ended.value().defense == Fixed{},
        "FSC-74: final half-plane equality selects direct-target flight outside strafe reach");
    for (const bool shared : {false, true}) {
        state.attack_path->stack = {route};
        state.approach = true;
        tactical::squadron_attack_combat_boundary(state, !shared, shared);
        expect(!state.approach && state.attack_path->stack.empty(),
            "FSC-74: strafe reach and a shared recorded cell independently clear unfinished approach stacks");
    }
}

void test_attack_path_refresh() {
    tactical::SquadronAttackPath original;
    original.target = 20;
    original.next_refresh_frame = 101;
    tactical::SquadronAttackRoute route;
    route.identity = 10;
    route.nodes = {at(0, 0), at(1000, 0)};
    route.trackers = {{10, {}}, {11, {}}, {12, {}}};
    original.stack = {route};
    tactical::SquadronAttackRefresh observation;
    observation.target_position = at(101, 0);
    observation.effective_reach = units(200);
    auto path = original;
    expect(!tactical::refresh_squadron_attack_path(path, 100, observation) && path == original,
        "FSC-72: displacement does not bypass the independent formation deadline");
    expect(tactical::refresh_squadron_attack_path(path, 101, observation) && path.next_refresh_frame == 131
        && path.stack == original.stack, "FSC-72: equality consumes the clock before requesting later installation");
    for (const auto displacement : {99, 100}) {
        path = original;
        observation.target_position = at(displacement, 0);
        for (const std::uint64_t frame : {101U, 131U, 161U}) {
            expect(!tactical::refresh_squadron_attack_path(path, frame, observation)
                && path.next_refresh_frame == frame + 30 && path.stack == original.stack,
                "FSC-72: subthreshold/equal displacement consumes visits without expiring the old path");
        }
    }
    observation.target_position = at(101, 0);
    for (int gate = 0; gate != 6; ++gate) {
        auto gated = observation;
        if (gate == 0) gated.shared_cell = true;
        if (gate == 1) gated.visible = false;
        if (gate == 2) gated.repath_permitted = false;
        if (gate == 3) gated.protected_destination = true;
        if (gate == 4) gated.attack_positioning = false;
        if (gate == 5) gated.in_attack_reach = true;
        path = original;
        expect(!tactical::refresh_squadron_attack_path(path, 101, gated) && path.next_refresh_frame == 131
            && path.stack == original.stack, "FSC-72: refused/shared/fog/range visits consume the clock and retain geometry");
        if (gate == 5) {
            path = original;
            gated.pursuing = true;
            expect(tactical::refresh_squadron_attack_path(path, 101, gated),
                "FSC-72: pursuit is a separate in-range replacement gate");
        }
    }
    observation.target_position = original.cached_position;
    observation.completed_or_stopping = true;
    path = original;
    expect(tactical::refresh_squadron_attack_path(path, 101, observation),
        "FSC-72: completed/stopping out-of-range attack requests replacement independently of displacement");
    observation.completed_or_stopping = false;
    observation.escorted_destination = 40;
    path = original;
    expect(tactical::refresh_squadron_attack_path(path, 101, observation),
        "FSC-72: escort identity change is distinct from ordinary target displacement (UFS-36 copying remains open)");

    path = original;
    expect(tactical::install_squadron_attack_path(path, route, 200) == tactical::SquadronPathAdmission::active
        && path.stack_changed_frame == 200 && !path.stack.back().trackers.at(11).started,
        "FSC-73/FC-72: old ordinary path can be replaced by identity, activation does not start flight");
    path = original;
    path.collision_identity = 10;
    expect(tactical::install_squadron_attack_path(path, route, 200) == tactical::SquadronPathAdmission::active,
        "FSC-73/FC-72: a matching collision identity can become active");
    path = original;
    path.collision_identity = 10;
    auto free_route = route;
    free_route.identity = 0;
    expect(tactical::install_squadron_attack_path(path, free_route, 200) == tactical::SquadronPathAdmission::inserted
        && path.stack.size() == 2 && path.stack.back() == route && path.stack_changed_frame == original.stack_changed_frame,
        "FSC-73/FC-72: collision-free insertion below a protected tracker is not activation or a stack-change stamp");
    expect(tactical::install_squadron_attack_path(path, route, 201) == tactical::SquadronPathAdmission::active
        && path.stack.size() == 3 && path.stack.front() == free_route
        && path.stack_changed_frame == 201 && !path.stack.back().trackers.at(11).started,
        "FSC-73/FC-72: matching identity becomes top without discarding protected/lower entries or starting flight");
    auto incompatible = route;
    incompatible.identity = 99;
    const auto before = path;
    expect(tactical::install_squadron_attack_path(path, incompatible, 201) == tactical::SquadronPathAdmission::refused
        && path == before, "FSC-73/FC-72: incompatible nonzero identity refuses without disturbing the protected path");
    tactical::SquadronState retained;
    retained.formation.emplace();
    retained.formation->attack_target = 20;
    retained.attack_path = original;
    retained.attack_path->initialization_pending = true;
    retained.attack_path->failure_frame = 100;
    tactical::SquadronAttackRoute failed;
    expect(tactical::install_squadron_attack_path(*retained.attack_path, failed, 100)
        == tactical::SquadronPathAdmission::refused && retained.formation->attack_target == 20,
        "FSC-73: failed installation does not revoke an accepted target destination");
    expect(!tactical::squadron_attack_initialization_due(*retained.attack_path, 160)
        && tactical::squadron_attack_initialization_due(*retained.attack_path, 161),
        "FSC-73/FC-68: failure history has strict two-second expiry, independent of the refresh deadline");
    retained.attack_path->failure_frame.reset();
    expect(tactical::squadron_attack_initialization_due(*retained.attack_path, 101),
        "FSC-73: first initialization has no fabricated failure history");
    // This squadron-only interface does not create queued non-team zero-speed
    // assignments; that distinct FC-69 producer stays outside its closure.
}

void test_installed_attack_segment() {
    auto profile = motion().squadrons.craft.front();
    tactical::CraftView leader{11, at(0, 0), {}, &profile};
    leader.state.velocity = {profile.max_speed, {}, {}};
    tactical::CraftView follower{12, at(-10, 10), leader.state, &profile};
    tactical::SquadronAttackPath path;
    path.target = 20;
    path.cached_position = at(2000, 1000);
    path.next_refresh_frame = 20;
    tactical::SquadronAttackRoute route;
    route.nodes = {at(0, 0), at(1000, 0), at(2000, 1000)};
    route.trackers = {{10, {}}, {11, {}}, {12, {}}};
    path.stack.push_back(route);
    tactical::CraftFrame frame;
    frame.self = &leader;
    frame.leader = &leader;
    frame.attacking = true;
    frame.approach = true;
    frame.target_position = path.cached_position;
    auto flight = tactical::squadron_attack_flight(path, leader.id, at(0, 0), profile.max_speed, {}, units(1));
    expect(flight && flight.value() && flight.value()->direction == at(1, 0),
        "FSC-71: a northeast target retains the installed east segment");
    if (!flight || !flight.value()) return;
    frame.attack_lane = flight.value();
    for (int tick = 0; tick != 3; ++tick) {
        frame.target_position.y = units(1000 + tick * 10);
        const auto flown = tactical::step_craft(frame);
        expect(flown && flown.value().state.yaw == Fixed{},
            "FSC-71: slight target displacement does not replace installed leader heading");
        if (flown) { leader.position = flown.value().position; leader.state = flown.value().state; }
    }
    frame.self = &follower;
    frame.own_offset = at(-10, 10);
    frame.formation_tolerance = units(20);
    const auto first = tactical::step_craft(frame);
    frame.attack_lane->direction = at(0, 1);
    const auto second = tactical::step_craft(frame);
    expect(first && second && first.value().position == second.value().position
        && first.value().state == second.value().state,
        "FSC-71: follower slot flight reads leader velocity rather than its own path heading");
    path.stack.back().aside = units(20);
    const auto mapped = tactical::squadron_attack_flight(path, leader.id, at(0, 0), profile.max_speed, {}, units(1));
    frame.self = &leader;
    frame.attack_lane = mapped ? mapped.value() : std::nullopt;
    const auto shifted = tactical::step_craft(frame);
    expect(shifted && shifted.value().state.yaw > Fixed{}, "FSC-71: a nonzero mapped side offset steers the leader");
    tactical::MotionRules rules;
    rules.arc_degrees = units(10);
    rules.expansion_distance = units(20);
    const std::array<eawr::sim::EntityId, 2> roster{11, 12};
    const auto north = tactical::plan_squadron_attack_route(profile, rules, 10, roster, 0,
        at(0, 0), units(90), profile.max_speed, at(2000, 0));
    const auto east = tactical::plan_squadron_attack_route(profile, rules, 10, roster, 0,
        at(0, 0), Fixed{}, profile.max_speed, at(2000, 0));
    expect(north && east && north.value().nodes == east.value().nodes
        && north.value().start_facing != east.value().start_facing,
        "FSC-71: current start facing is independent of the unobstructed segment bearing");
    const auto decimal = [](const char* value) { return Fixed::from_decimal(value).value(); };
    const math::Vec3 captured_start{decimal("-369.418579"), decimal("-1459.639404"), decimal("9.208252")};
    const math::Vec3 captured_end{decimal("407.160217"), decimal("-1457.412354"), decimal("1.970062")};
    auto captured = tactical::plan_squadron_attack_route(profile, rules, 10, roster, 376,
        captured_start, decimal("165.068619"), profile.max_speed, captured_end);
    expect(captured && captured.value().nodes == std::vector<math::Vec3>{captured_start, captured_end}
        && captured.value().ahead == Fixed{} && captured.value().aside == Fixed{}
        && captured.value().node_speeds == std::vector<Fixed>{Fixed{}, Fixed{}},
        "FSC-71/FD-65: captured unobstructed route retains two nodes and zero mapping");
    if (!captured) return;
    const auto& nodes = captured.value().nodes;
    const auto bearing = std::atan2(static_cast<double>(nodes[1].y.raw() - nodes[0].y.raw()),
        static_cast<double>(nodes[1].x.raw() - nodes[0].x.raw())) * 180.0 / std::acos(-1.0);
    expect(std::abs(bearing - 0.164310771) < 0.000001,
        "FSC-71: captured bearing survives squadron route production");
    for (const auto& [id, tracker] : captured.value().trackers) {
        static_cast<void>(id);
        expect(tracker.turn_scale == units(1) && tracker.speed_scale == units(1) && !tracker.started,
            "FSC-71/73: unit annotations are retained without starting a member");
    }
    path.stack = {captured.value()};
    path.cached_position = captured_end;
    path.next_refresh_frame = 416;
    tactical::SquadronAttackRefresh observation;
    observation.target_position = captured_end;
    observation.effective_reach = units(500);
    for (const auto due : {416U, 446U, 476U}) {
        expect(!tactical::refresh_squadron_attack_path(path, due, observation)
            && path.next_refresh_frame == due + 30 && path.stack.front() == captured.value(),
            "FSC-72: captured due boundaries consume thirty frames and retain the same route");
    }
    for (const auto& point : {std::pair{math::Vec3{decimal("-390.766632"), decimal("-1410.495239"), {}}, -1.6368354559},
             std::pair{math::Vec3{decimal("-307.285919"), decimal("-1359.540161"), {}}, -3.3273465633}}) {
        auto lane = tactical::squadron_attack_flight(path, 11, point.first, profile.max_speed, decimal("0.1"), units(30));
        expect(lane && lane.value() && std::abs(static_cast<double>(lane.value()->shift.raw()) / one - point.second) < 0.00001,
            "FSC-71: container side correction is distinct from zero path mapping and live range observation");
    }
}

// FL-06 facing: a launch along (1, 0, -1) yaws 0, pitches 45 (nose down) at full speed.
void test_launch_state() {
    // FM-27: direct ships use the bay's planar heading, including authored vertical vectors.
    // The full-zero bay vector still takes the existing forward-vector fallback.
    for (const std::int64_t vertical : {-1, 0, 1}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = seed;
            setup.players = players();
            setup.units = {unit(1, carrier_type, empire, at(0, 0))};
            auto table = motion();
            auto& hangar = table.squadrons.spawners.front();
            hangar.entries = {{frigate_type, 1, 0, true}};
            hangar.bays.front().spawn_vector = at(0, 0, vertical);
            auto made = tactical::TacticalSession::create(setup, sensors(), durability(), table);
            expect(static_cast<bool>(made), "FM-27: vertical/zero direct-ship bay fixture creates");
            if (!made) return;
            auto session = std::move(made).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            bool launched = false;
            for (int tick = 0; tick < 40 && !launched; ++tick) {
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "FM-27: vertical/zero bay launches without a heading failure");
                if (!step) return;
                hashes.push_back(step.value().state_sha256);
                for (const auto& ship : session.units()) {
                    if (ship.type_id != frigate_type) continue;
                    launched = true;
                    const auto yaw = tactical::yaw_degrees(ship.rotation);
                    expect(yaw && yaw.value() == Fixed{}, "FM-27: vertical/zero direct-ship bay uses zero yaw");
                }
            }
            expect(launched, "FM-27: direct ship actually launches within its first service");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "FM-27: vertical/zero bay launch agrees on 1/2/4/8 workers");
        }
    }
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

static void controller_launch_attribution() {
    for (const auto humans : {2U, 3U, 4U}) for (const bool shared : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup start;
            start.seed = seed; start.players = players();
            start.players.push_back({3, 1, 1, tactical::player_flag_commandable});
            start.departure_policy = tactical::MultiplayerDeparturePolicy{{empire, rebel, 3}};
            start.units = {unit(1, carrier_type, empire, at(0, 0))};
            start.first_free_entity_id = 2;
            tactical::EconomyRules money; money.campaign = true;
            for (const auto owner : {empire, rebel, 3U}) {
                tactical::EconomyPlayer account;
                account.player = owner; account.credit_cap = Fixed{}; account.population_cap = 20;
                account.ai = owner == rebel; money.players.push_back(account);
            }
            start.campaign.emplace(); start.campaign->players = money.players;
            start.controllers.emplace();
            start.controllers->factions = {{empire, tactical::FactionController::humans},
                {rebel, tactical::FactionController::ai}, {3, tactical::FactionController::humans}};
            for (std::uint32_t seat = 1; seat <= humans; ++seat)
                start.controllers->seats.push_back({{seat}, empire, eawr::sim::SeatKind::human, true});
            start.controllers->seats.push_back({{humans + 1}, rebel, eawr::sim::SeatKind::ai, true});
            for (std::uint32_t seat = humans + 2; seat <= humans * 2 + 1; ++seat)
                start.controllers->seats.push_back({{seat}, 3, eawr::sim::SeatKind::human, true});
            start.units.front().control = shared ? tactical::UnitControl::shared_structure : tactical::UnitControl::mobile;
            start.units.front().controller_seat = {shared ? 0U : humans};
            auto table = motion();
            table.squadrons.spawners.front().entries = {{squadron_a, static_cast<std::int32_t>(humans + 1), 1}};
            table.squadrons.spawners.front().delay_frames = 1;
            tactical::CombatTable weapons;
            if (shared) {
                tactical::CombatProfile station;
                station.type_id = carrier_type; station.star_base = true;
                weapons.profiles.push_back(station);
            }
            auto made = tactical::TacticalSession::create(start, sensors(), durability(), table, std::nullopt, weapons, {}, {}, money);
            expect(static_cast<bool>(made), "P3: shared/mobile launch fixture creates for 2/3/4 humans");
            if (!made) continue;
            auto world = std::move(made).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::optional<tactical::TacticalSession> restored;
            std::vector<eawr::sim::EntityId> births;
            std::vector<std::string> hashes;
            std::vector<eawr::sim::EntityId> first_members;
            bool saved_first_birth{};
            bool gifted_carrier{};
            // FC-28 services every 30 frames, independently of the one-frame spawn delay.
            for (unsigned frame = 0; frame < 280; ++frame) {
                if (!shared && !gifted_carrier && !births.empty()) {
                    tactical::PlayerCommand gift{{frame, empire, frame}, {1}, tactical::GiftPayload{{1}},
                        {humans}, tactical::CommandOrigin::seat};
                    expect(static_cast<bool>(world.submit(gift)), "P4: carrier gift submits after its first independent launch");
                    if (restored) expect(static_cast<bool>(restored->submit(gift)), "P4: restored carrier receives the gift");
                    gifted_carrier = true;
                }
                if (frame == 200 || frame == 250) {
                    tactical::PlayerCommand command{{frame, empire, frame}, first_members, tactical::DamagePayload{units(1000)}};
                    if (frame == 250) { command.units.clear(); command.payload = tactical::QuitPayload{}; }
                    command.origin = tactical::CommandOrigin::engine;
                    expect(static_cast<bool>(world.submit(command)), "P3: replacement or conversion event submits");
                    if (restored) expect(static_cast<bool>(restored->submit(command)), "P3: restored producer receives the same event");
                }
                if (workers != 1) world.scramble_storage_for_testing();
                const auto advanced = world.step(executor);
                expect(static_cast<bool>(advanced), "P3: carrier attribution advances");
                if (!advanced) { std::cerr << advanced.error().message << '\n'; break; }
                hashes.push_back(advanced.value().state_sha256 + advanced.value().snapshot->sha256());
                if (restored) {
                    const auto resumed = restored->step(executor);
                    expect(resumed && resumed.value().state_sha256 == advanced.value().state_sha256
                        && resumed.value().snapshot->canonical_bytes() == advanced.value().snapshot->canonical_bytes(),
                        "P3: restored output cursor and replacement/conversion equal live");
                }
                for (const auto& group : world.squadrons()) {
                    const auto* container = instance(*world.snapshot(), group.container);
                    if (!container) { expect(false, "P3: launched group publishes its root"); continue; }
                    const auto known = std::find(births.begin(), births.end(), group.container);
                    if (known == births.end()) {
                        const auto seat = shared ? 1U + static_cast<std::uint32_t>(births.size() % humans)
                            : gifted_carrier ? 1U : humans;
                        expect(container->controller_seat.value == seat && container->control == tactical::UnitControl::mobile,
                            "P3: mobile output inherits producer; shared output rotates eligible humans per successful birth");
                        births.push_back(group.container);
                        if (first_members.empty()) first_members = group.members;
                    }
                    if (!shared && gifted_carrier && frame < 200 && !births.empty() && group.container == births.front())
                        expect(container->controller_seat.value == humans,
                            "P4: launched squadron retains its previous controller after gifting its carrier");
                    for (const auto id : group.members) {
                        const auto* craft = instance(*world.snapshot(), id);
                        expect(craft && craft->owner == container->owner && craft->controller_seat == container->controller_seat,
                            "P3: launch, replacement and conversion retain one controller across root and members");
                    }
                    if (frame >= 251) expect(container->owner == 3
                        && container->controller_seat.value >= humans + 2 && container->controller_seat.value <= humans * 2 + 1,
                        "P3: ownership conversion remaps every independent squadron into the new faction's controller domain");
                }
                if (frame == 0 || (births.size() == 1 && !saved_first_birth)) {
                    if (births.size() == 1) saved_first_birth = true;
                    const auto saved = world.save_checkpoint_state();
                    auto fresh = tactical::TacticalSession::from_replay(world.record(), sensors(), durability(), table, std::nullopt, weapons, {}, {}, money);
                    expect(saved && fresh, "P3: carrier checkpoint factory creates");
                    if (saved && fresh) {
                        const auto loaded = fresh.value().load_checkpoint_state(saved.value());
                        expect(static_cast<bool>(loaded), "P3: checkpoint restores shared producer output ordinal");
                        if (!loaded) { std::cerr << loaded.error().message << '\n'; break; }
                        const auto recaptured = fresh.value().save_checkpoint_state();
                        expect(recaptured && recaptured.value() == saved.value(), "P3: producer continuation round-trips exactly");
                        restored.emplace(std::move(fresh).value());
                    }
                }
            }
            expect(births.size() == humans + 2, "P3: initial output and a depleted squadron's replacement both appear");
            if (reference.empty()) reference = hashes;
            expect(hashes == reference, "P3: producer attribution and conversion agree on 1/2/4/8 workers");
        }
    }
}

// C-04 (FL-06, FL-07): the station's squadron leaves its bay; craft first, then the container.
void test_launch() {
    controller_launch_attribution();
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
    // WDF-05: the squadron escorts a carrier near the origin, and the fixture lies below it.
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, tactical::CombatTable{},
        tactical::VictoryRules{}, tactical::AbilityTable{}, tactical::EconomyRules{}, tactical::MapBox{units(-2400), units(-2400)});
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
    expect(instance(*session.snapshot(), 14)->garrison_unit,
        "WU-37: a hangar-launched team's birth snapshot is marked");
    expect(instance(*session.snapshot(), 12)->garrison_unit && instance(*session.snapshot(), 13)->garrison_unit,
        "WU-37: every hangar craft is marked at birth");
    expect(!instance(*session.snapshot(), 10)->garrison_unit,
        "WU-37: the starting team stays unmarked");
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
    expect(instance(*snapshot, 14) && instance(*snapshot, 14)->garrison_unit,
        "WU-37: hangar provenance survives later snapshots and movement");
    expect(instance(*snapshot, 12)->garrison_unit && instance(*snapshot, 13)->garrison_unit,
        "WU-37: craft marks survive subsequent gather, movement and snapshot rebuilds");
    std::vector<tactical::TacticalInstance> unmarked(snapshot->instances().begin(), snapshot->instances().end());
    auto marked = unmarked;
    for (auto& value : unmarked) value.garrison_unit = false;
    for (auto& value : marked) value.garrison_unit = true;
    expect(tactical::TacticalSnapshot(0, {}, unmarked, {}).canonical_bytes()
        == tactical::TacticalSnapshot(0, {}, marked, {}).canonical_bytes(),
        "WU-37: garrison presentation does not affect canonical snapshot bytes");
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

// WU-37: retain provenance independently of the registry and the current team roster.
void test_garrison_provenance() {
    constexpr tactical::TypeId wing_type = craft_type + 1;
    for (const bool parent : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = seed; setup.players = players();
            setup.players.push_back({3, 1, 1, tactical::player_flag_commandable});
            setup.departure_policy = tactical::MultiplayerDeparturePolicy{{empire, rebel, 3}};
            setup.units = {unit(1, carrier_type, empire, at(0, 0)),
                unit(10, craft_type, empire, at(100, 0))};
            setup.units.front().garrison_enabled = false;
            setup.free_garrisons = {{empire, 1, {parent ? squadron_a : craft_type}, {10}}};
            auto table = motion();
            auto wing = table.squadrons.craft.front(); wing.type_id = wing_type;
            table.squadrons.craft.push_back(wing);
            table.squadrons.squadrons.front().members = {craft_type, wing_type, wing_type};
            table.squadrons.squadrons.front().offsets = {at(0, 0), at(-10, 10), at(10, 10)};
            table.squadrons.spawners.front().entries.clear();
            table.squadrons.spawners.front().starbase = true;
            table.squadrons.spawners.front().delay_frames = 1;
            auto health = durability();
            auto wing_health = health.profiles[1]; wing_health.type_id = wing_type;
            health.profiles.insert(health.profiles.begin() + 2, wing_health);
            tactical::AbilityProfile replenish;
            replenish.kind = tactical::AbilityKind::replenish_wingmen;
            replenish.recharge_frames = 1; replenish.replenish_team = squadron_a;
            tactical::UnitAbilityProfile leader;
            leader.type_id = craft_type; leader.abilities = {replenish};
            tactical::AbilityTable abilities; abilities.profiles = {leader};
            auto made = tactical::TacticalSession::create(setup, {}, health, table, std::nullopt, {}, {}, abilities);
            expect(static_cast<bool>(made), "WU-37: free-garrison wingman fixture creates");
            if (!made) return;
            auto world = std::move(made).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            const auto advance = [&]() {
                if (workers != 1) world.scramble_storage_for_testing();
                auto step = world.step(executor);
                expect(static_cast<bool>(step), "WU-37: provenance fixture steps");
                if (step) hashes.push_back(step.value().state_sha256);
                return static_cast<bool>(step);
            };
            expect(static_cast<bool>(world.submit({{0, empire, 0}, {10}, tactical::DamagePayload{units(1000)}})),
                "WU-37: deplete original free-garrison registry");
            eawr::sim::EntityId container{}, source{};
            for (unsigned frame = 0; frame < 100 && !source; ++frame) {
                if (!advance()) return;
                for (const auto& group : world.squadrons()) if (group.container > 10) {
                    container = group.container; source = group.members.front();
                }
            }
            expect(source != 0, "WU-37: free-garrison replacement launches");
            if (!source) return;
            const auto originals = world.squadrons().front().members;
            expect(instance(*world.snapshot(), source)->garrison_unit
                && instance(*world.snapshot(), container)->garrison_unit,
                "WU-37: solo or parent and original craft are marked at birth");
            if (!advance()) return;
            expect(instance(*world.snapshot(), source)->garrison_unit,
                "WU-37: the independent craft mark survives ECS gather and shuffled rebuild");
            if (parent) {
                std::vector<eawr::sim::EntityId> lost(originals.begin() + 1, originals.end());
                expect(static_cast<bool>(world.submit({{world.completed_tick(), empire, 1}, lost,
                    tactical::DamagePayload{units(1000)}})), "WU-37: remove original wingmen");
                if (!advance()) return;
            }
            expect(static_cast<bool>(world.submit({{world.completed_tick(), empire, 2}, {source},
                tactical::AbilityPayload{tactical::AbilityKind::replenish_wingmen, tactical::AbilityAction::activate}})),
                "WU-37: replenish the marked leader");
            if (!advance()) return;
            const auto groups = world.squadrons();
            const auto group = std::find_if(groups.begin(), groups.end(), [source](const auto& value) {
                return std::find(value.members.begin(), value.members.end(), source) != value.members.end();
            });
            expect(group != groups.end() && group->members.size() == 3,
                "WU-37: replenishment retains the leader and creates two wingmen");
            if (group == groups.end() || group->members.size() != 3) return;
            const auto retained_container = group->container;
            expect((retained_container == container) == parent
                && instance(*world.snapshot(), retained_container)->garrison_unit == parent
                && world.squadron_state(retained_container)->garrison_unit == parent,
                "WU-37: retain an existing marked parent; a new parent is unmarked despite its marked leader");
            for (const auto id : group->members) expect(instance(*world.snapshot(), id)->garrison_unit == (id == source),
                "WU-37: new wingmen are unmarked and the retained leader stays marked");
            expect(static_cast<bool>(world.submit({{world.completed_tick(), empire, 3}, {source},
                tactical::DamagePayload{units(1000)}})), "WU-37: remove the last registered original craft");
            if (!advance() || !advance()) return;
            expect(instance(*world.snapshot(), retained_container)
                && instance(*world.snapshot(), retained_container)->garrison_unit == parent
                && world.squadron_state(retained_container)->garrison_unit == parent,
                "WU-37: original-craft depletion never changes the surviving parent's birth mark");
            // Capture and departure use the same ownership boundary. The remaining craft
            // are all unmarked, so the team's mark cannot be inferred from its members.
            expect(static_cast<bool>(world.submit({{world.completed_tick(), empire, 4}, {}, tactical::QuitPayload{}})),
                "WU-37: transfer the surviving team to the remaining ally");
            if (!advance() || !advance()) return;
            const auto* transferred = instance(*world.snapshot(), retained_container);
            const auto transferred_mind = world.squadron_state(retained_container);
            expect(transferred && transferred->owner == 3 && transferred->garrison_unit == parent
                && transferred_mind && transferred_mind->garrison_unit == parent,
                "WU-37: team ownership transfer preserves the independent container mark after commit");
            for (const auto member : world.squadrons().front().members) {
                const auto* craft = instance(*world.snapshot(), member);
                expect(craft && craft->owner == 3 && !craft->garrison_unit,
                    "WU-37: transferred replenished members stay unmarked beneath a marked team");
            }
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WU-37: provenance and wingman loss agree on 1/2/4/8 workers");
        }
    }
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
    // Large garrisons still use the ordinary partitioned flight phases and ordered births.
    for (const std::size_t count : {65U, 186U}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup large_setup;
            large_setup.seed = seed;
            large_setup.players = players();
            large_setup.units = {unit(1, carrier_type, empire, at(0, 0))};
            auto table = motion();
            auto alternate = table.squadrons.craft.front();
            alternate.type_id = craft_type + 1;
            alternate.max_speed = units(4);
            table.squadrons.craft.push_back(alternate);
            auto& profile = table.squadrons.squadrons.front();
            profile.max_squad_size = 200;
            profile.members.assign(count, craft_type);
            for (std::size_t index = 1; index < count; index += 2) profile.members[index] = alternate.type_id;
            profile.offsets.assign(count, at(0, 0));
            table.squadrons.spawners.front().entries = {{squadron_a, 1, 0}};
            auto health = durability();
            auto alternate_health = health.profiles[1];
            alternate_health.type_id = alternate.type_id;
            health.profiles.insert(health.profiles.begin() + 2, alternate_health);
            auto created = tactical::TacticalSession::create(large_setup, {}, health, table);
            expect(static_cast<bool>(created), "FL-02: a large garrison session creates");
            if (!created) continue;
            auto world = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (int tick = 0; tick < 40; ++tick) {
                if (workers != 1) world.scramble_storage_for_testing();
                const auto stepped = world.step(executor);
                expect(static_cast<bool>(stepped), "FL-02: large-garrison flight advances");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
            }
            const auto groups = world.squadrons();
            expect(groups.size() == 1 && groups.front().members.size() == count,
                "FL-02: every authored member launches in one garrison");
            if (groups.size() == 1) {
                for (std::size_t index = 0; index < groups.front().members.size(); ++index)
                    expect(groups.front().members[index] == index + 2,
                        "FL-02: launch preserves stable member order above 64");
            }
            expect(hashes.size() == 40, "FL-02: large garrison completes all flight ticks");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "FL-02: large garrisons agree on 1/2/4/8 workers");
        }
    }
    // FL-06/PG-08: N-1, N, N+1 and an uneven tail; mixed craft keep input order.
    for (const std::size_t count : {2U, 3U, 4U, 8U}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto table = motion();
            auto alternate = table.squadrons.craft.front();
            alternate.type_id = craft_type + 1;
            table.squadrons.craft.push_back(alternate);
            auto& profile = table.squadrons.squadrons.front();
            profile.max_squad_size = 3;
            profile.members.assign(count, craft_type);
            for (std::size_t i = 1; i < count; i += 2) profile.members[i] = alternate.type_id;
            profile.offsets.assign(count, {});
            table.squadrons.spawners.front().entries = {{squadron_a, 1, 1}};
            table.squadrons.spawners.front().mobile = true;
            auto health = durability();
            auto alternate_health = health.profiles[1];
            alternate_health.type_id = alternate.type_id;
            health.profiles.insert(health.profiles.begin() + 2, alternate_health);
            tactical::TacticalSetup setup;
            setup.seed = seed; setup.players = players();
            setup.units = {unit(1, carrier_type, empire, at(0, 0))};
            auto created = tactical::TacticalSession::create(setup, {}, health, table);
            expect(static_cast<bool>(created), "FL-06: split-team session builds");
            if (!created) continue;
            auto world = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (int tick = 0; tick < 40; ++tick) {
                if (workers != 1) world.scramble_storage_for_testing();
                const auto stepped = world.step(executor);
                expect(static_cast<bool>(stepped), "FL-06: split teams advance");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
            }
            const std::vector<tactical::Squadron> groups(world.squadrons().begin(), world.squadrons().end());
            expect(groups.size() == (count + 2) / 3, "FL-06: chunk count includes uneven tail");
            for (std::size_t i = 0; i < groups.size(); ++i) {
                expect(groups[i].container == count + 2 + i, "FL-06: all craft IDs precede ordered team IDs");
                expect(groups[i].members.size() == std::min<std::size_t>(3, count - i * 3), "FL-06: chunk has bounded size");
                for (std::size_t j = 0; j < groups[i].members.size(); ++j)
                    expect(groups[i].members[j] == 2 + i * 3 + j, "FL-06: mixed authored member order survives grouping");
                const auto mind = world.squadron_state(groups[i].container);
                expect(mind && mind->spawner == (i == 0 ? 1U : 0U), "FL-06: only first team notifies its bay");
                expect(mind && mind->garrison_unit == (i == 0), "FL-06: only returned first team is marked as a garrison");
                expect(mind && (i == 0 ? mind->mode == tactical::SquadronMode::escort
                    : mind->mode == tactical::SquadronMode::idle), "FL-07: only first team receives the bay escort");
            }
            // Depleting a tail does not release the first team's launch slot.
            if (groups.size() > 1) {
                expect(static_cast<bool>(world.submit({{world.completed_tick(), empire, 1}, groups.back().members,
                    tactical::DamagePayload{units(1000)}})), "FL-06: tail depletion submits");
                for (int tick = 0; tick < 200; ++tick) {
                    const auto stepped = world.step(executor);
                    expect(static_cast<bool>(stepped), "FL-06: depleted teams advance");
                    if (!stepped) break;
                    hashes.push_back(stepped.value().state_sha256);
                }
                expect(world.squadrons().size() == groups.size() - 1, "FL-06: a lost tail is not replenished");
                expect(static_cast<bool>(world.submit({{world.completed_tick(), empire, 2}, groups.front().members,
                    tactical::DamagePayload{units(1000)}})), "FL-06: first-team depletion submits");
                for (int tick = 0; tick < 200; ++tick) {
                    const auto stepped = world.step(executor);
                    expect(static_cast<bool>(stepped), "FL-06: replacement teams advance");
                    if (!stepped) break;
                    hashes.push_back(stepped.value().state_sha256);
                }
                expect(world.squadrons().size() == 2 * groups.size() - 2, "FL-06: losing first team releases exactly one whole roster");
            }
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "FL-06: split births, depletion and replacement agree on 1/2/4/8 workers");
        }
    }
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
