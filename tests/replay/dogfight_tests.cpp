#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// #457 squadron dogfights (docs/behaviour/space-fighters.md FD-01 to FD-12, cases C-22 to C-28):
// the combat cell and retaliation, chase pairing and its ten-second timers, following, the end of
// a fight, and ship avoidance, each hashing alike with 1, 2, 4 and 8 workers.
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using eawr::sim::EntityId;
using math::Fixed;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr std::uint64_t seed = 4570;
constexpr tactical::PlayerId empire = 1;
constexpr tactical::PlayerId rebel = 2;
constexpr tactical::TypeId craft_type = 200;
constexpr tactical::TypeId trio_type = 320;   // three craft
constexpr tactical::TypeId single_type = 330; // one craft
constexpr tactical::TypeId frigate_type = 400;

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}
[[nodiscard]] double as_double(const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(one); }
[[nodiscard]] double distance(const math::Vec3& a, const math::Vec3& b) {
    const double x = as_double(a.x) - as_double(b.x);
    const double y = as_double(a.y) - as_double(b.y);
    const double z = as_double(a.z) - as_double(b.z);
    return std::sqrt(x * x + y * y + z * z);
}

// A TIE-fighter-like craft: the #75 fixture's flight plus the #457 follow and attack distances.
[[nodiscard]] tactical::MotionTable motion(const bool frigate_layer = true) {
    tactical::MotionTable table;
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
    craft.follow_distance = units(50);
    craft.attack_distance = units(500);
    table.squadrons.craft = {craft};
    // FO-11 (#599): gameconstants.xml's side errors.
    table.squadrons.side_error_min = Fixed::from_raw(one / 10);
    table.squadrons.side_error_max = units(30);
    table.squadrons.squadrons = {
        {trio_type, {craft_type, craft_type, craft_type}, {at(0, 0), at(-20, 20), at(-20, -20)}, units(1000), units(200),
            units(300), units(20)},
        {single_type, {craft_type}, {at(0, 0)}, units(1000), units(200), units(300), units(20)}};
    // FD-10: the frigate is a ship with a space layer (a sphere of 150 units); without the layer
    // craft do not see it.
    table.footprints = {tactical::Footprint{frigate_type,
        frigate_layer ? tactical::SpaceLayer::frigate : tactical::SpaceLayer::none, units(150), units(60), units(150), false}};
    return table;
}

[[nodiscard]] tactical::DurabilityTable durability(const std::int64_t craft_hull) {
    tactical::DurabilityTable table;
    table.rules = {Fixed::from_raw(one / 5), Fixed::from_raw(one * 2 / 5), Fixed::from_raw(one / 3)};
    table.profiles = {tactical::DurabilityProfile{craft_type, units(craft_hull), std::nullopt, false, {}},
        tactical::DurabilityProfile{frigate_type, units(1000000), std::nullopt, false, {}}};
    return table;
}

[[nodiscard]] tactical::CombatTable combat() {
    tactical::CombatTable table;
    tactical::PrioritySet set;
    set.unlisted = units(1000);
    set.rows = {{craft_type, units(2)}, {frigate_type, units(1)}};
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
    gun.shot = tactical::ShotProfile{units(10), tactical::no_type_index, units(20), units(700), true, true, {}};
    craft.weapons = {gun};
    table.profiles = {craft, tactical::CombatProfile{frigate_type, 4, std::nullopt, std::nullopt, {}, {}, {}}};
    return table;
}

[[nodiscard]] tactical::UnitState unit(const EntityId id, const tactical::TypeId type, const tactical::PlayerId owner,
    const math::Vec3& position, const std::int64_t yaw_degrees = 0) {
    tactical::UnitState state{id, type, owner, position, math::identity_quat(), {}};
    if (yaw_degrees == 180) state.rotation = {Fixed{}, Fixed{}, units(1), Fixed{}};
    return state;
}

[[nodiscard]] std::vector<tactical::Player> players() {
    return {{empire, 1, 1, tactical::player_flag_commandable}, {rebel, 2, 2, tactical::player_flag_commandable}};
}

[[nodiscard]] std::optional<math::Vec3> position_of(const tactical::TacticalSnapshot& snapshot, const EntityId id) {
    for (const auto& entry : snapshot.instances()) {
        if (entry.entity_id == id) {
            return math::Vec3{entry.fixed_transform.rows[0][3], entry.fixed_transform.rows[1][3], entry.fixed_transform.rows[2][3]};
        }
    }
    return std::nullopt;
}

struct Order {
    tactical::PlayerId player{};
    EntityId unit{};
    tactical::CommandPayload payload;
    std::vector<EntityId> more{}; // further units of the same command
    std::uint64_t tick{1};
};

struct Session {
    tactical::TacticalSetup setup;
    tactical::MotionTable motion;
    tactical::DurabilityTable durability;
    std::vector<Order> orders;
};

// Runs `ticks` ticks with `workers`, calling `observe` after every step; returns the hashes.
[[nodiscard]] std::vector<std::string> run(const Session& fixture, const std::size_t workers, const int ticks,
    const std::function<void(const tactical::TacticalSession&, const tactical::TacticalTick&)>& observe = {}) {
    std::vector<std::string> hashes;
    std::vector<tactical::SensorProfile> sensors{{craft_type, units(8000)}, {frigate_type, units(8000)}};
    for (const auto& squadron : fixture.setup.squadrons) {
        for (const auto& unit : fixture.setup.units) {
            if (unit.entity_id == squadron.container
                && std::none_of(sensors.begin(), sensors.end(), [&](const auto& s) { return s.type_id == unit.type_id; })) {
                sensors.push_back({unit.type_id, units(8000)});
            }
        }
    }
    std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    auto created = tactical::TacticalSession::create(fixture.setup, sensors, fixture.durability, fixture.motion, std::nullopt, combat());
    expect(static_cast<bool>(created), "the session builds");
    if (!created) {
        std::cerr << created.error().message << '\n';
        return hashes;
    }
    auto session = std::move(created).value();
    std::map<tactical::PlayerId, std::uint64_t> sequence;
    for (const auto& order : fixture.orders) {
        std::vector<EntityId> command_units{order.unit};
        command_units.insert(command_units.end(), order.more.begin(), order.more.end());
        const auto submitted = session.submit({{order.tick, order.player, ++sequence[order.player]}, command_units, order.payload});
        expect(static_cast<bool>(submitted), "the order submits");
    }
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < ticks; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "step failed: " + stepped.error().message);
            break;
        }
        hashes.push_back(stepped.value().state_sha256);
        if (observe) observe(session, stepped.value());
    }
    return hashes;
}

void expect_workers(const Session& fixture, const int ticks, const std::vector<std::string>& hashes, const std::string& label) {
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run(fixture, workers, ticks) == hashes, label + ": " + std::to_string(workers) + " workers hash like one");
    }
}

// Squadron 10 (empire, craft 11-13) attacks squadron 20 (rebel, craft 21-23) 700 units away;
// 20 has no order. Craft are too tough to die in the run.
[[nodiscard]] Session trio_fight() {
    Session fixture;
    fixture.setup.seed = seed;
    fixture.setup.players = players();
    fixture.setup.units = {unit(10, trio_type, empire, at(-350, 0)), unit(11, craft_type, empire, at(-350, 0)),
        unit(12, craft_type, empire, at(-370, 20)), unit(13, craft_type, empire, at(-370, -20)),
        unit(20, trio_type, rebel, at(350, 0), 180), unit(21, craft_type, rebel, at(350, 0), 180),
        unit(22, craft_type, rebel, at(370, 20), 180), unit(23, craft_type, rebel, at(370, -20), 180)};
    fixture.setup.squadrons = {{10, {11, 12, 13}}, {20, {21, 22, 23}}};
    fixture.motion = motion();
    fixture.durability = durability(1000000);
    fixture.orders = {{empire, 10, tactical::AttackPayload{20}}};
    return fixture;
}

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
// snapshots list it as spinning, and spin_away_ended follows 60 frames later (2 s).
void test_spin_in_dogfight() {
    auto fixture = fight_to_the_end(false);
    fixture.motion.squadrons.craft[0].spin_away = tactical::SpinAwayProfile{units(1), units(2)};
    std::optional<std::uint64_t> destroyed;
    std::optional<std::uint64_t> started;
    std::optional<std::uint64_t> ended;
    std::size_t spinning_ticks = 0;
    bool chased = false;
    const auto hashes = run(fixture, 1, 1200, [&](const tactical::TacticalSession& session, const tactical::TacticalTick& tick) {
        for (const auto& event : tick.snapshot->events()) {
            if (event.unit != 21) continue;
            if (event.kind == tactical::EventKind::unit_destroyed) destroyed = event.tick;
            if (event.kind == tactical::EventKind::spin_away_started) started = event.tick;
            if (event.kind == tactical::EventKind::spin_away_ended) ended = event.tick;
        }
        for (const auto& craft : tick.snapshot->spinning()) {
            if (craft.entity_id == 21) ++spinning_ticks;
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
    expect(started && ended && *ended == *started + 60, "C-28: spin_away_ended follows 60 frames later");
    expect(spinning_ticks == 60, "C-28: the snapshots show it spinning for 60 ticks, got " + std::to_string(spinning_ticks));
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

// C-27 (#552, #599, FO-07 to FO-11): four squadrons moved together by one command take four
// slots around the destination and fly their formation's lanes there; moved one by one they
// converge on the one point.
void test_group_move() {
    tactical::GroupSquadron member;
    member.radius = units(30);
    member.max_speed = units(5);
    member.min_speed = units(2);
    member.type = trio_type;
    member.attack_distance = units(500);
    std::vector<tactical::GroupSquadron> four(4, member);
    for (std::size_t index = 0; index < four.size(); ++index) {
        four[index].position = at(-600, -150 + 100 * static_cast<std::int64_t>(index));
    }
    const auto slots = tactical::squadron_group_slots(four, at(600, 0, 7));
    expect(static_cast<bool>(slots), "FO-08: the slots map");
    if (slots) {
        double closest = 1e9;
        for (std::size_t a = 0; a < slots.value().size(); ++a) {
            const auto& slot = slots.value()[a];
            expect(slot.point.z == units(7), "FO-08: a slot keeps the destination's height");
            expect(slot.lane && slot.lane->formation == 0 && slot.lane->direction == at(1, 0),
                "FO-10: every squadron flies the formation's path, started by the first");
            for (std::size_t b = a + 1; b < slots.value().size(); ++b) {
                closest = std::min(closest, distance(slot.point, slots.value()[b].point));
            }
        }
        // Two 60-unit-wide squadrons a row, rows one squadron deep: centres a width apart.
        expect(closest >= 59.0, "FO-09: slots lie a squadron's width apart, closest " + std::to_string(closest));
        // The squadron furthest left of the move (largest y) keeps the left.
        expect(slots.value()[3].point.y > slots.value()[0].point.y, "FO-09: squadrons keep their side of the move");
    }
    // FO-09: a row runs from the right of the move to its left in the order its squadrons stand,
    // and the frontmost fill the first row: moved east from a two-by-two block, each squadron
    // keeps its corner.
    auto block = four;
    block[0].position = at(-600, -50);
    block[1].position = at(-600, 50);
    block[2].position = at(-500, 50);
    block[3].position = at(-500, -50);
    if (const auto corners = tactical::squadron_group_slots(block, at(600, 0)); corners) {
        const auto& c = corners.value();
        expect(c[2].point.x > c[1].point.x && c[3].point.x > c[0].point.x && c[2].point.y > c[3].point.y
                && c[1].point.y > c[0].point.y,
            "FO-09: a two-by-two block keeps its corners");
    } else {
        expect(false, "FO-09: the block maps");
    }
    // FO-09: squadrons of another type fill their own rows (the shorter attack distance first).
    auto mixed = four;
    mixed[1].type = single_type;
    mixed[1].attack_distance = units(300);
    if (const auto rows = tactical::squadron_group_slots(mixed, at(600, 0)); rows) {
        const auto& r = rows.value();
        expect(r[1].lane && r[0].lane && r[1].lane->ahead > r[0].lane->ahead && std::abs(as_double(r[1].lane->aside)) < 0.01,
            "FO-09: a squadron of a shorter-ranged type heads its own row");
    } else {
        expect(false, "FO-09: the mixed formation maps");
    }
    auto far_apart = four;
    far_apart[3].position = at(-600, 3000);
    const auto split = tactical::squadron_group_slots(far_apart, at(600, 0));
    expect(split && split.value()[3].point == at(600, 0) && !split.value()[3].lane,
        "FO-07: a squadron far from the others flies alone to the point");

    // FO-10, FO-11: the lane steer and the row keeping.
    {
        std::vector<tactical::LaneMember> lanes(2);
        for (auto& lane : lanes) {
            lane.lane.direction = at(1, 0);
            lane.max_speed = units(5);
            lane.min_speed = units(2);
        }
        lanes[0].position = at(100, 10);   // 10 left of the path, its lane 40 left: 30 to go
        lanes[0].lane.aside = units(40);
        lanes[1].position = at(100, -60);  // level with the first where its slot lies 60 behind
        lanes[1].lane.aside = units(-60);
        lanes[1].lane.ahead = units(-60);
        const auto flights = tactical::formation_lane_flight(lanes, Fixed::from_raw(one / 10), units(30));
        expect(static_cast<bool>(flights), "FO-11: the lane flight computes");
        if (flights) {
            const auto& f = flights.value();
            // (30 - 0.1) / 30 to the left; the second is on its lane.
            expect(std::abs(as_double(f[0].shift) - 29.9 / 30.0) < 1e-3 && f[1].shift.raw() == 0,
                "FO-10: the side error beyond the minimum over the maximum");
            // Ahead of its place by 60 (at least 12, the largest), it slows to the minimum; the
            // first, behind by as much, may speed up only to its own maximum.
            expect(f[1].speed == units(2), "FO-11: a squadron ahead of its row slows to the minimum speed");
            expect(f[0].speed == units(5), "FO-11: no squadron flies faster than its maximum");
        }
    }

    const auto moved = [](const bool together, std::vector<std::vector<math::Vec3>>& centres) {
        Session fixture;
        fixture.setup.seed = seed;
        fixture.setup.players = players();
        std::vector<EntityId> containers;
        for (std::int64_t index = 0; index < 4; ++index) {
            const auto container = static_cast<EntityId>(10 + 10 * index);
            const std::int64_t y = -150 + 100 * index;
            fixture.setup.units.push_back(unit(container, trio_type, empire, at(-600, y)));
            fixture.setup.units.push_back(unit(container + 1, craft_type, empire, at(-600, y)));
            fixture.setup.units.push_back(unit(container + 2, craft_type, empire, at(-620, y + 20)));
            fixture.setup.units.push_back(unit(container + 3, craft_type, empire, at(-620, y - 20)));
            fixture.setup.squadrons.push_back({container, {container + 1, container + 2, container + 3}});
            containers.push_back(container);
        }
        fixture.motion = motion();
        fixture.durability = durability(1000000);
        if (together) {
            fixture.orders = {{empire, containers[0], tactical::MovePayload{at(600, 0)},
                {containers.begin() + 1, containers.end()}}};
        } else {
            for (const auto container : containers) {
                fixture.orders.push_back({empire, container, tactical::MovePayload{at(600, 0)}});
            }
        }
        double closest = 1e9;
        double apart = 0;
        int samples = 0;
        std::uint64_t landed = 0; // the first tick a squadron is no longer on its move
        bool took_off = false;
        std::set<std::pair<std::int64_t, std::int64_t>> anchors;
        centres.assign(containers.size(), {});
        const auto hashes = run(fixture, 1, 400, [&](const tactical::TacticalSession& session, const tactical::TacticalTick& tick) {
            if (tick.completed_tick == 5) {
                for (const auto container : containers) {
                    if (const auto state = session.squadron_state(container)) {
                        anchors.insert({state->anchor.x.raw(), state->anchor.y.raw()});
                    }
                }
            }
            // In flight: every squadron still on its move. At arrival (FO-02) each claims an idle cell
            // (FM-24, #687) and its container moves to the cell's point, 120 units apart where the
            // slots were 60; squadrons whose slots share a cell may pass each other on the way to
            // their cells, which is not the lane flight FO-10 keeps apart.
            bool flying = true;
            for (const auto container : containers) {
                const auto state = session.squadron_state(container);
                flying = flying && state && state->mode == tactical::SquadronMode::move;
            }
            if (flying) {
                took_off = true;
                for (std::size_t index = 0; index < containers.size(); ++index) {
                    if (const auto centre = position_of(*tick.snapshot, containers[index])) centres[index].push_back(*centre);
                }
            } else if (took_off && landed == 0) {
                landed = tick.completed_tick;
            }
            if (tick.completed_tick >= 300) {
                // Arrival: the nearest other squadron centre of each squadron, averaged.
                for (const auto a : containers) {
                    double nearest = 1e9;
                    for (const auto b : containers) {
                        const auto pa = position_of(*tick.snapshot, a);
                        const auto pb = position_of(*tick.snapshot, b);
                        if (b != a && pa && pb) nearest = std::min(nearest, distance(*pa, *pb));
                    }
                    apart += nearest;
                    ++samples;
                }
            }
            if (tick.completed_tick < 100 || tick.completed_tick > 260 || !flying) return;
            for (const auto a : containers) {
                for (const auto b : containers) {
                    if (b <= a) continue;
                    for (EntityId i = 1; i <= 3; ++i) {
                        for (EntityId j = 1; j <= 3; ++j) {
                            const auto pa = position_of(*tick.snapshot, a + i);
                            const auto pb = position_of(*tick.snapshot, b + j);
                            if (pa && pb) closest = std::min(closest, distance(*pa, *pb));
                        }
                    }
                }
            }
        });
        expect_workers(fixture, 400, hashes, together ? "C-27 together" : "C-27 one by one");
        std::cout << "C-27 " << (together ? "together" : "one by one") << ": every squadron on its move until tick " << landed << '\n';
        return std::tuple{anchors.size(), closest, samples > 0 ? apart / samples : 0.0};
    };
    // A lane crossing: two squadrons swap sides of the move (y, the move runs along x) while
    // they are less than a squadron's width (60) apart along it, so one passes through the other.
    const auto crossings = [](const std::vector<std::vector<math::Vec3>>& centres) {
        int count = 0;
        for (std::size_t a = 0; a < centres.size(); ++a) {
            for (std::size_t b = a + 1; b < centres.size(); ++b) {
                const auto ticks = std::min(centres[a].size(), centres[b].size());
                for (std::size_t t = 1; t < ticks; ++t) {
                    const double before = as_double(centres[a][t - 1].y) - as_double(centres[b][t - 1].y);
                    const double now = as_double(centres[a][t].y) - as_double(centres[b][t].y);
                    const double along = std::abs(as_double(centres[a][t].x) - as_double(centres[b][t].x));
                    if ((before < 0) != (now < 0) && along < 60.0) ++count;
                }
            }
        }
        return count;
    };
    std::vector<std::vector<math::Vec3>> together_centres;
    std::vector<std::vector<math::Vec3>> alone_centres;
    const auto [together_slots, together_closest, together_apart] = moved(true, together_centres);
    const auto [alone_slots, alone_closest, alone_apart] = moved(false, alone_centres);
    const int together_crossings = crossings(together_centres);
    std::cout << "C-27: closest craft of different squadrons, ticks 100-260: together " << together_closest
              << ", one by one " << alone_closest << "; lane crossings together " << together_crossings
              << ", one by one " << crossings(alone_centres) << "; mean nearest squadron centre from tick 300: together "
              << together_apart << ", one by one " << alone_apart << '\n';
    expect(together_slots == 4, "C-27 (FO-08): four squadrons moved together hold four points");
    expect(alone_slots == 1, "C-27: four squadrons moved one by one hold one point");
    // FM-24, FM-26 (#687): arrived, each squadron holds its own idle cell, whose point its container
    // stands on, so those moved one by one hold apart too.
    expect(together_apart >= 119.0, "C-27 (FO-09): squadrons moved together hold apart");
    expect(alone_apart >= 119.0, "C-27 (FM-24): squadrons moved one by one hold their own idle cells");
    // FO-10, FO-11 (#599): in flight too. A craft's soft radius here is under 10 units.
    expect(together_closest > 10.0, "C-27 (FO-10): squadrons moved together keep apart in flight, closest "
        + std::to_string(together_closest));
    expect(together_crossings == 0, "C-27 (FO-10): no squadron flies through another's lane");
}

} // namespace

int main() {
    test_can_follow();
    test_pairing();
    test_chase_timer();
    test_fight_end();
    test_spin_in_dogfight();
    test_avoidance();
    test_intercepted();
    test_group_move();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "dogfight tests passed\n";
    return 0;
}
