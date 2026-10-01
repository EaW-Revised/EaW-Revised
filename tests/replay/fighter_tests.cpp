#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #75 fighter squadrons (docs/behaviour/space-fighters.md, cases C-01 to C-06): the hangar
// (FL-01 to FL-08) on a synthetic spawner, a launch from a bay, the squadron's priority-set
// scan (FT-02, FT-03) and worker-count hash equality of a launch-and-attack session.
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr std::uint64_t seed = 12345;
constexpr tactical::PlayerId empire = 1;
constexpr tactical::PlayerId rebel = 2;
constexpr tactical::TypeId carrier_type = 100; // SPAWN_SQUADRON station
constexpr tactical::TypeId craft_type = 200;   // TIE-fighter-like craft
constexpr tactical::TypeId squadron_a = 300;   // two craft
constexpr tactical::TypeId squadron_b = 310;   // one craft
constexpr tactical::TypeId frigate_type = 400; // priority 1
constexpr tactical::TypeId corvette_type = 410; // priority 3

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}

// Entries A (1 alive, reserve 2) and B (1 alive, no reserve), a 150-frame delay, one bay.
[[nodiscard]] tactical::SpawnerProfile spawner() {
    tactical::SpawnerProfile profile;
    profile.type_id = carrier_type;
    profile.entries = {{squadron_a, 1, 2}, {squadron_b, 1, 0}};
    profile.delay_frames = 150;
    profile.bays = {{0, at(20, 0, -10), at(1, 0, -1)}};
    return profile;
}

[[nodiscard]] tactical::MotionTable motion() {
    tactical::MotionTable table;
    auto& squadrons = table.squadrons;
    tactical::CraftProfile craft;
    craft.type_id = craft_type;
    craft.max_speed = Fixed::from_raw(one * 54 / 10);
    craft.min_speed = Fixed::from_raw(one * 18 / 10);
    craft.rate_of_turn = units(6);
    craft.lift = units(6);
    craft.thrust = Fixed::from_raw(one / 5);
    craft.roll_rate = units(6);
    craft.bank_angle = units(70);
    craft.strafe_distance = units(200);
    squadrons.craft = {craft};
    squadrons.squadrons = {
        {squadron_a, {craft_type, craft_type}, {at(0, 0), at(-10, 10)}, units(1000), units(200), units(300), units(20)},
        {squadron_b, {craft_type}, {at(0, 0)}, units(1000), units(200), units(300), units(20)}};
    squadrons.spawners = {spawner()};
    return table;
}

[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {Fixed::from_raw(one / 5), Fixed::from_raw(one * 2 / 5), Fixed::from_raw(one / 3)};
    table.profiles = {tactical::DurabilityProfile{carrier_type, units(5000), std::nullopt, false, {}},
        tactical::DurabilityProfile{craft_type, units(90), units(5), false, {}},
        tactical::DurabilityProfile{frigate_type, units(3000), units(3), false, {}},
        tactical::DurabilityProfile{corvette_type, units(1000), units(3), false, {}}};
    return table;
}

// Craft attack by the set (frigate 1, corvette 3); a ship-level object weapon so they fire.
[[nodiscard]] tactical::CombatTable combat() {
    tactical::CombatTable table;
    tactical::PrioritySet set;
    set.unlisted = units(1000);
    set.rows = {{frigate_type, units(1)}, {corvette_type, units(3)}};
    table.priority_sets = {set};
    tactical::CombatProfile craft;
    craft.type_id = craft_type;
    craft.category_bits = 1;
    craft.priority_set = 0;
    craft.max_attack_distance = units(500);
    tactical::WeaponProfile gun;
    gun.range = units(500);
    gun.min_recharge_hundredths = 100;
    gun.max_recharge_hundredths = 100;
    craft.weapons = {gun};
    table.profiles = {tactical::CombatProfile{carrier_type, 2, std::nullopt, std::nullopt, {}, {}, {}}, craft,
        tactical::CombatProfile{frigate_type, 4, std::nullopt, std::nullopt, {}, {}, {}},
        tactical::CombatProfile{corvette_type, 4, std::nullopt, std::nullopt, {}, {}, {}}};
    return table;
}

[[nodiscard]] std::vector<tactical::SensorProfile> sensors() {
    return {{carrier_type, units(8000)}, {craft_type, units(8000)}, {frigate_type, units(8000)}, {corvette_type, units(8000)}};
}

[[nodiscard]] tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type,
    const tactical::PlayerId owner, const math::Vec3& position) {
    return tactical::UnitState{id, type, owner, position, math::identity_quat(), {}};
}

[[nodiscard]] std::vector<tactical::Player> players() {
    return {{empire, 1, 1, tactical::player_flag_commandable}, {rebel, 2, 2, tactical::player_flag_commandable}};
}

[[nodiscard]] const tactical::TacticalInstance* instance(const tactical::TacticalSnapshot& snapshot,
    const eawr::sim::EntityId id) {
    for (const auto& entry : snapshot.instances()) {
        if (entry.entity_id == id) return &entry;
    }
    return nullptr;
}

[[nodiscard]] math::Vec3 position(const tactical::TacticalInstance& found) {
    return {found.fixed_transform.rows[0][3], found.fixed_transform.rows[1][3], found.fixed_transform.rows[2][3]};
}

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
void test_priority_scan() {
    const auto run_case = [](std::vector<tactical::UnitState> enemies) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(1, carrier_type, empire, at(-3000, 0)), unit(10, squadron_a, empire, at(0, 0)),
            unit(11, craft_type, empire, at(0, 0)), unit(12, craft_type, empire, at(-10, 10))};
        for (auto& enemy : enemies) setup.units.push_back(enemy);
        setup.squadrons = {{10, {11, 12}}};
        auto table = motion();
        table.squadrons.spawners.clear();
        auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
        expect(static_cast<bool>(created), "C-05: the session builds");
        std::optional<tactical::CombatState> state;
        if (!created) return state;
        auto session = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(1);
        for (int tick = 0; tick < 5; ++tick) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "C-05: steps");
            if (!stepped) return state;
        }
        state = session.combat_state(12);
        return state;
    };
    const auto chosen = run_case({unit(20, frigate_type, rebel, at(650, 0)), unit(21, corvette_type, rebel, at(300, 0))});
    expect(chosen && chosen->attack_target == 20 && chosen->direct, "C-05: the priority-1 frigate over the nearer corvette");
    const auto only_near = run_case({unit(21, corvette_type, rebel, at(300, 0))});
    expect(only_near && only_near->attack_target == 21 && only_near->direct, "C-05: a priority-3 unit when it is alone");
    // Idle chase 200 + attack distance 500 = 700 from the held point.
    const auto beyond = run_case({unit(20, frigate_type, rebel, at(900, 0))});
    expect(beyond && !beyond->direct, "C-05: a unit beyond the chase range plus attack distance is not the squadron's");
}

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
        if (launched && !engaged && state && state->direct && state->attack_target == 10) engaged = completed;
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
// frigate far beyond its chase reach; the hashes are the same with 1, 2 and 4 workers.
struct OrderRun {
    std::vector<std::string> hashes;
    std::map<int, math::Vec3> centre;          // the container (craft centre) by tick
    std::map<int, std::optional<tactical::CombatState>> craft; // craft 12's combat state by tick
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
    }
    return run;
}

[[nodiscard]] double distance_to(const math::Vec3& from, const std::int64_t x, const std::int64_t y) {
    const double dx = static_cast<double>(from.x.raw()) / static_cast<double>(one) - static_cast<double>(x);
    const double dy = static_cast<double>(from.y.raw()) / static_cast<double>(one) - static_cast<double>(y);
    return std::sqrt(dx * dx + dy * dy);
}

void test_squadron_orders() {
    expect(tactical::squadron_move_arrived(at(0, 0), at(100, 0), at(100, 50)), "FO-02: on the destination line");
    expect(tactical::squadron_move_arrived(at(0, 0), at(100, 0), at(140, -900)), "FO-02: past the destination line");
    expect(!tactical::squadron_move_arrived(at(0, 0), at(100, 0), at(99, 0)), "FO-02: short of the destination");
    expect(tactical::squadron_move_arrived(at(5, 5), at(5, 5), at(-300, 7)), "FO-02: a zero-length move has arrived");
    expect(!tactical::squadron_move_arrived(at(-60000, -60000), at(60000, 60000), at(59999, 59999)),
        "FO-02: exact far from the origin");

    const auto run = run_orders(1, true);
    expect(run.hashes.size() == 1060, "FO: 1060 ticks");
    if (run.hashes.size() != 1060) return;
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
        if (!run.engaged && craft && craft->attack_target == 20 && craft->direct) run.engaged = tick;
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

// C-30 (#687, FM-23, FM-24): the idle cell a squadron claims.
void test_idle_cell_claim() {
    const auto desired = at(70, 250);
    const auto start = tactical::idle_cell_of(desired);
    expect(start.x == 0 && start.y == 2, "FM-23: (70, 250) lies in cell (0, 2)");
    const auto point = tactical::idle_cell_point(start, units(20));
    expect(point.x == units(60) && point.y == units(300) && point.z == units(20), "FM-23: cell (0, 2)'s point is (60, 300)");
    const auto odd = tactical::idle_cell_point({0, 1}, Fixed{});
    expect(odd.x == units(120) && odd.y == units(180), "FM-23: an odd row is shifted half a cell");
    const auto free = tactical::idle_cell_claim(desired, {});
    expect(free && *free == start, "FM-24: the free cell under the desired point");
    const std::vector<tactical::CombatCell> taken{start};
    const auto next = tactical::idle_cell_claim(desired, taken);
    expect(next && *next == tactical::CombatCell{0, 1}, "FM-24: the nearest free cell of the first ring, (120, 180)");
    std::vector<tactical::CombatCell> full;
    for (std::int32_t y = start.y - 4; y <= start.y + 4; ++y) {
        for (std::int32_t x = start.x - 4; x <= start.x + 4; ++x) full.push_back({x, y});
    }
    expect(!tactical::idle_cell_claim(desired, full), "FM-24: no claim once 64 cells show no free one");
    full.erase(std::find(full.begin(), full.end(), tactical::CombatCell{start.x + 3, start.y + 3}));
    expect(tactical::idle_cell_claim(desired, full).has_value(),
        "FM-24: a free cell of the third ring is found within the 64 examined");
}

// C-31 (#687, FM-24 to FM-26): squadrons 10 (craft 11, 12) and 20 (craft 21) each moved to (1500,
// 0) by their own order hold two idle cells; the mobile carrier 1's two launched squadrons escort it and
// hold two more. The run records the hashes and the cells at the end.
struct IdleRun {
    std::vector<std::string> hashes;
    std::vector<std::pair<eawr::sim::EntityId, tactical::CombatCell>> cells; // ascending container
};

[[nodiscard]] IdleRun run_idle(const std::size_t workers) {
    IdleRun run;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(1, carrier_type, empire, at(-2000, -2000)), unit(10, squadron_a, empire, at(0, 0)),
        unit(11, craft_type, empire, at(0, 0)), unit(12, craft_type, empire, at(-10, 10)),
        unit(20, squadron_b, empire, at(0, 300)), unit(21, craft_type, empire, at(0, 300))};
    setup.squadrons = {{10, {11, 12}}, {20, {21}}};
    auto table = motion();
    table.squadrons.spawners[0].mobile = true; // a carrier: its launches escort it (FL-07)
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
    expect(static_cast<bool>(created), "C-31: the session builds");
    if (!created) return run;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit({{5, empire, 1}, {10}, tactical::MovePayload{at(1500, 0)}})), "C-31: move 10");
    expect(static_cast<bool>(session.submit({{5, empire, 2}, {20}, tactical::MovePayload{at(1500, 0)}})), "C-31: move 20");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 900; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "C-31: step failed: " + stepped.error().message);
            return run;
        }
        run.hashes.push_back(stepped.value().state_sha256);
    }
    for (eawr::sim::EntityId id = 1; id < 200; ++id) {
        if (const auto state = session.squadron_state(id); state && state->idle_cell) run.cells.emplace_back(id, *state->idle_cell);
    }
    return run;
}

void test_idle_grid() {
    const auto run = run_idle(1);
    expect(run.hashes.size() == 900, "C-31: 900 ticks");
    const auto held = [&run](const eawr::sim::EntityId id) -> std::optional<tactical::CombatCell> {
        for (const auto& [container, cell] : run.cells) {
            if (container == id) return cell;
        }
        return std::nullopt;
    };
    expect(held(10) && held(20) && !(*held(10) == *held(20)), "FM-24: two squadrons moved to one point hold two cells");
    std::size_t escorts = 0;
    for (const auto& [container, cell] : run.cells) {
        if (container != 10 && container != 20) ++escorts;
    }
    expect(escorts == 2, "FM-24: the carrier's two escorts each hold a cell, got " + std::to_string(escorts));
    for (std::size_t a = 0; a < run.cells.size(); ++a) {
        for (std::size_t b = a + 1; b < run.cells.size(); ++b) {
            const auto p = tactical::idle_cell_point(run.cells[a].second, Fixed{});
            const auto q = tactical::idle_cell_point(run.cells[b].second, Fixed{});
            const double dx = static_cast<double>(p.x.raw() - q.x.raw()) / static_cast<double>(one);
            const double dy = static_cast<double>(p.y.raw() - q.y.raw()) / static_cast<double>(one);
            expect(std::sqrt(dx * dx + dy * dy) >= 119.0, "FM-23: held cells are at least a cell apart");
        }
    }
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_idle(workers).hashes == run.hashes, "C-31: " + std::to_string(workers) + " workers hash like one");
    }
}

void test_squadron_guard() {
    // FO-06: a guard of the Empire frigate 30 escorts it and engages the rebel frigate 1300 units
    // from it, within the 1000-unit Guard_Chase_Range plus the 500-unit attack distance.
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
// fires at the squadron's craft nearest itself. A gunship (30) attacks squadron 20 (craft 21, 22),
// and squadron 10 (craft 11, 12) attacks it from beyond its chase reach; 1, 2, 4 and 8 workers
// hash alike.
constexpr tactical::TypeId gunship_type = 420; // an object weapon, no motion

struct TeamRun {
    std::vector<std::string> hashes;
    std::vector<tactical::CombatEvent> fired; // weapon_fired events of 30, 11 and 12
    std::optional<tactical::CombatState> gunship;
};

[[nodiscard]] TeamRun run_team_attack(const std::size_t workers) {
    TeamRun run;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(10, squadron_a, empire, at(-1500, 0)), unit(11, craft_type, empire, at(-1500, 0)),
        unit(12, craft_type, empire, at(-1510, 10)), unit(20, squadron_a, rebel, at(300, 0)),
        unit(21, craft_type, rebel, at(300, 0)), unit(22, craft_type, rebel, at(290, 10)),
        unit(30, gunship_type, empire, at(0, 0))};
    setup.squadrons = {{10, {11, 12}}, {20, {21, 22}}};
    auto table = motion();
    table.squadrons.spawners.clear();
    auto health = durability();
    health.profiles.push_back(tactical::DurabilityProfile{gunship_type, units(5000), std::nullopt, false, {}});
    auto weapons = combat();
    auto gunship = weapons.profiles[1]; // the craft's gun and attack distance
    gunship.type_id = gunship_type;
    gunship.category_bits = 4;
    gunship.priority_set.reset();
    weapons.profiles.push_back(gunship);
    auto sight = sensors();
    sight.push_back({gunship_type, units(8000)});
    auto created = tactical::TacticalSession::create(setup, sight, health, table, std::nullopt, weapons);
    expect(static_cast<bool>(created), "C-10: the session builds");
    if (!created) return run;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit({{1, empire, 1}, {30}, tactical::AttackPayload{20}})), "C-10: the gunship's attack submits");
    expect(static_cast<bool>(session.submit({{1, empire, 2}, {10}, tactical::AttackPayload{20}})), "C-10: the squadron's attack submits");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 600; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "C-10: step failed: " + stepped.error().message);
            break;
        }
        run.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.kind == tactical::CombatEventKind::weapon_fired
                && (event.shooter == 30 || event.shooter == 11 || event.shooter == 12)) {
                run.fired.push_back(event);
            }
        }
        if (tick == 3) run.gunship = session.combat_state(30);
    }
    return run;
}

void test_team_attack() {
    const auto run = run_team_attack(1);
    expect(run.hashes.size() == 600, "C-10: 600 ticks");
    expect(run.gunship && run.gunship->attack_target == 20 && run.gunship->direct,
        "C-10: the gunship's target is the squadron's team container");
    const auto fired_by = [&run](const std::initializer_list<eawr::sim::EntityId> shooters) {
        std::vector<tactical::CombatEvent> events;
        for (const auto& event : run.fired) {
            if (std::find(shooters.begin(), shooters.end(), event.shooter) != shooters.end()) events.push_back(event);
        }
        return events;
    };
    const auto gunship = fired_by({30});
    expect(!gunship.empty(), "C-10: the gunship fires at the squadron it was ordered to attack");
    // Craft 22 (290, 10) is nearer the gunship at the origin than craft 21 (300, 0).
    expect(!gunship.empty() && gunship.front().target == 22, "C-10: the first shot goes to the nearest craft");
    const auto squadron = fired_by({11, 12});
    expect(!squadron.empty(), "C-10: the ordered squadron fires at the enemy squadron");
    for (const auto& event : run.fired) {
        expect(event.target == 21 || event.target == 22,
            "C-10: every shot of an attacker goes to a craft of the squadron, got " + std::to_string(event.target));
    }
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_team_attack(workers).hashes == run.hashes, "C-10: " + std::to_string(workers) + " workers hash like one");
    }
}

// C-12 (FO-04): the craft an attack on a squadron fires at is not kept. Each shot goes to the
// squadron's live craft nearest the shooter at that frame (only a special ability's own target is
// kept, and M2 has none). The gunship (30) attacks squadron 20 while 20 flies past it to the far
// side, so the nearest craft changes; 1, 2, 4 and 8 workers hash alike.
struct RetargetShot {
    tactical::CombatEvent event;
    math::Vec3 first{};   // craft 21 at the shot's frame
    math::Vec3 second{};  // craft 22 at the shot's frame
};

struct RetargetRun {
    std::vector<std::string> hashes;
    std::vector<RetargetShot> shots;
};

[[nodiscard]] RetargetRun run_team_retarget(const std::size_t workers) {
    RetargetRun run;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(20, squadron_a, rebel, at(900, 0)), unit(21, craft_type, rebel, at(900, 0)),
        unit(22, craft_type, rebel, at(880, 30)), unit(30, gunship_type, empire, at(0, 0))};
    setup.squadrons = {{20, {21, 22}}};
    auto table = motion();
    table.squadrons.spawners.clear();
    auto health = durability();
    health.profiles.push_back(tactical::DurabilityProfile{gunship_type, units(5000), std::nullopt, false, {}});
    for (auto& profile : health.profiles) {
        if (profile.type_id == craft_type) profile.max_hull = units(100000); // the pass outlives the fire
    }
    auto weapons = combat();
    auto gunship = weapons.profiles[1];
    gunship.type_id = gunship_type;
    gunship.category_bits = 4;
    gunship.priority_set.reset();
    weapons.profiles.push_back(gunship);
    auto sight = sensors();
    sight.push_back({gunship_type, units(8000)});
    auto created = tactical::TacticalSession::create(setup, sight, health, table, std::nullopt, weapons);
    expect(static_cast<bool>(created), "C-12: the session builds");
    if (!created) return run;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit({{1, empire, 1}, {30}, tactical::AttackPayload{20}})), "C-12: the attack submits");
    expect(static_cast<bool>(session.submit({{1, rebel, 1}, {20}, tactical::MovePayload{at(-2500, 0)}})), "C-12: the move submits");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 700; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "C-12: step failed: " + stepped.error().message);
            break;
        }
        run.hashes.push_back(stepped.value().state_sha256);
        const auto& snapshot = *stepped.value().snapshot;
        const auto* first = instance(snapshot, 21);
        const auto* second = instance(snapshot, 22);
        for (const auto& event : snapshot.combat_events()) {
            if (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 30 && first && second) {
                run.shots.push_back({event, position(*first), position(*second)});
            }
        }
    }
    return run;
}

void test_team_retarget() {
    const auto run = run_team_retarget(1);
    expect(run.hashes.size() == 700, "C-12: 700 ticks");
    expect(run.shots.size() > 4, "C-12: the gunship fires through the pass, got " + std::to_string(run.shots.size()));
    const auto squared = [](const math::Vec3& point) {
        const double x = static_cast<double>(point.x.raw()) / static_cast<double>(one);
        const double y = static_cast<double>(point.y.raw()) / static_cast<double>(one);
        const double z = static_cast<double>(point.z.raw()) / static_cast<double>(one);
        return x * x + y * y + z * z;
    };
    bool hit_first = false;
    bool hit_second = false;
    for (const RetargetShot& shot : run.shots) {
        const double first = squared(shot.first);
        const double second = squared(shot.second);
        hit_first = hit_first || shot.event.target == 21;
        hit_second = hit_second || shot.event.target == 22;
        if (std::abs(first - second) < 1.0) continue; // too close to call in doubles
        const eawr::sim::EntityId nearest = first < second ? 21 : 22;
        expect(shot.event.target == nearest, "C-12: the shot at frame " + std::to_string(shot.event.tick)
            + " goes to the nearest craft " + std::to_string(nearest) + ", got " + std::to_string(shot.event.target));
    }
    expect(hit_first && hit_second, "C-12: the gunship moves to the other craft when it becomes the nearer");
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_team_retarget(workers).hashes == run.hashes, "C-12: " + std::to_string(workers) + " workers hash like one");
    }
}

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

} // namespace

int main() {
    test_hangar();
    test_destroyed_bay();
    test_validation();
    test_finite_reserve_boundary();
    test_formation_catch_up();
    test_coordinate_limit();
    test_launch_state();
    test_out_of_combat();
    test_approach();
    test_launch();
    test_launched_squadron();
    test_priority_scan();
    test_launch_idle();
    test_worker_equality();
    test_squadron_orders();
    test_squadron_attack_move();
    test_squadron_guard();
    test_idle_cell_claim();
    test_idle_grid();
    test_attack_order_approach();
    test_team_attack();
    test_team_retarget();
    test_squadron_hardpoint_attack();
    test_empty_table();
    if (failures != 0) {
        std::cerr << failures << " fighter contract failure(s)\n";
        return 1;
    }
    std::cout << "fighter contracts passed\n";
    return 0;
}
