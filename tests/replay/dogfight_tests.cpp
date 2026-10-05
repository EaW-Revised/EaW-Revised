#include "dogfight_support.hpp"
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
#include <fstream>
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
namespace dogfight_test_support {


int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}


[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z) {
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
[[nodiscard]] tactical::MotionTable motion(const bool frigate_layer) {
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
    const math::Vec3& position, const std::int64_t yaw_degrees) {
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


// Runs `ticks` ticks with `workers`, calling `observe` after every step; returns the hashes.
[[nodiscard]] std::vector<std::string> run(const Session& fixture, const std::size_t workers, const int ticks,
    const std::function<void(const tactical::TacticalSession&, const tactical::TacticalTick&)>& observe) {
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
    auto created = tactical::TacticalSession::create(fixture.setup, sensors, fixture.durability, fixture.motion, std::nullopt, fixture.weapons);
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

// and the index emit every completed tick's hash and live craft's chosen chase.
int trace_seeded_dogfights(const char* path) {
    std::ofstream trace(path);
    if (!trace) return 1;
    for (const std::uint64_t fight_seed : {4570U, 8930U, 9740U}) {
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            for (const bool deaths : {false, true}) {
                auto fixture = deaths ? fight_to_the_end(true) : trio_fight();
                fixture.setup.seed = fight_seed;
                const auto hashes = run(fixture, workers, 640, [&](const auto& session, const tactical::TacticalTick& tick) {
                    trace << fight_seed << ',' << workers << ',' << deaths << ',' << tick.completed_tick
                          << ',' << tick.state_sha256;
                    for (const auto& squadron : session.squadrons()) {
                        for (const auto member : squadron.members) {
                            if (const auto flight = session.craft_state(member)) {
                                trace << ',' << member << ':' << flight->chase << ':' << flight->chase_until;
                            }
                        }
                    }
                    trace << '\n';
                });
                expect(hashes.size() == 640, "seeded chase trace completes every tick");
            }
        }
    }
    return failures == 0 && trace.good() ? 0 : 1;
}

// WSQ-51/57: remove either the leader or a middle craft, then check the next
// flight step against the surviving roster's slots, including banked height.
void test_survivor_formation_slots() {
    for (const EntityId removed : {EntityId{11}, EntityId{12}}) {
        auto fixture = trio_fight();
        fixture.setup.units.resize(4);
        fixture.setup.squadrons.resize(1);
        fixture.motion.squadrons.squadrons[0].formation_tolerance = Fixed{};
        fixture.orders = {{empire, 10, tactical::MovePayload{at(2000, 0)}},
            {empire, removed, tactical::DamagePayload{units(2000000)}}};
        const auto& profile = fixture.motion.squadrons.craft.front();
        std::optional<tactical::CraftStep> expected;
        std::optional<tactical::CraftStep> old_slot;
        bool checked = false;
        const auto hashes = run(fixture, 1, 4, [&](const auto& session, const auto& tick) {
            if (tick.completed_tick == 2) {
                const EntityId leader_id = removed == 11 ? 12 : 11;
                const auto leader_position = position_of(*tick.snapshot, leader_id);
                const auto follower_position = position_of(*tick.snapshot, 13);
                const auto leader_state = session.craft_state(leader_id);
                const auto follower_state = session.craft_state(13);
                expect(!session.craft_state(removed), "WSQ-57: the removed craft leaves flight state");
                if (!leader_position || !follower_position || !leader_state || !follower_state) return;
                const tactical::CraftView leader{leader_id, *leader_position, *leader_state, &profile};
                const tactical::CraftView follower{13, *follower_position, *follower_state, &profile};
                tactical::CraftFrame frame;
                frame.self = &follower;
                frame.leader = &leader;
                frame.own_offset = fixture.motion.squadrons.squadrons[0].offsets[1];
                frame.leader_offset = fixture.motion.squadrons.squadrons[0].offsets[0];
                frame.moving = true;
                frame.hold = at(2000, 0);
                frame.lane = tactical::LaneFlight{at(1, 0), Fixed{}, profile.max_speed, true};
                auto compact = tactical::step_craft(frame);
                if (compact) expected = compact.value();
                frame.own_offset = fixture.motion.squadrons.squadrons[0].offsets[2];
                if (removed == 11) frame.leader_offset = fixture.motion.squadrons.squadrons[0].offsets[1];
                auto lifetime = tactical::step_craft(frame);
                if (lifetime) old_slot = lifetime.value();
            } else if (tick.completed_tick == 3 && expected && old_slot) {
                checked = true;
                const auto flight = session.craft_state(13);
                const auto position = position_of(*tick.snapshot, 13);
                expect(flight && position && *flight == expected->state && *position == expected->position,
                    "WSQ-51: the survivor flies the compacted roster's slot on the next frame");
                expect(expected->state != old_slot->state,
                    "WSQ-51: the contract distinguishes live slots from launch slots");
            }
        });
        expect(checked, "WSQ-51: leader and middle-member removal exercise the next flight step");
        expect_workers(fixture, 4, hashes, "WSQ-51 survivor slots");
    }
}

void test_solo_fighter() {
    // WHE-SQ-02: the hero is one real craft, not a ship or an extra invisible container.
    for (const bool enemy_solo : {false, true}) {
        auto fixture = trio_fight();
        fixture.setup.units.erase(fixture.setup.units.begin(), fixture.setup.units.begin() + 4);
        fixture.setup.units.insert(fixture.setup.units.begin(), unit(10, craft_type, empire, at(-350, 0)));
        fixture.setup.squadrons.erase(fixture.setup.squadrons.begin());
        fixture.motion.squadrons.squadrons.insert(fixture.motion.squadrons.squadrons.begin(),
            {craft_type, {craft_type}, {at(0, 0)}, units(1000), units(200), units(300), units(20)});
        if (enemy_solo) {
            fixture.setup.units.resize(2);
            fixture.setup.units[1] = unit(20, craft_type, rebel, at(350, 0), 180);
            fixture.setup.squadrons.clear();
        }
        bool moved = false;
        bool cell = false;
        bool chase = false;
        const auto hashes = run(fixture, 1, 360, [&](const auto& session, const auto& tick) {
            const auto position = position_of(*tick.snapshot, 10);
            moved = moved || (position && *position != at(-350, 0));
            expect(session.units().size() == fixture.setup.units.size(),
                "WHE-SQ-02: solo flight creates no extra entity");
            const auto flight = session.craft_state(10);
            chase = chase || (flight && flight->chase != eawr::sim::invalid_entity_id);
            for (const auto& target : tick.snapshot->squadron_targets())
                if (target.squadron == 10) cell = cell || target.recorded;
        });
        expect(moved && cell && chase, "WHE-SQ-02: solo fighter moves, joins fighter combat and chases");
        expect_workers(fixture, 360, hashes, "WHE-SQ-02 solo fighter");
    }
}

} // namespace

using namespace dogfight_test_support;

int main(const int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--trace") return trace_seeded_dogfights(argv[2]);
    if (argc != 1) return 1;
    test_can_follow();
    test_pairing();
    test_chase_timer();
    test_chase_work_counts();
    test_fight_end();
    test_spin_in_dogfight();
    test_avoidance();
    test_intercepted();
    test_retarget_clears_the_hardpoint();
    test_group_move();
    test_solo_fighter();
    test_survivor_formation_slots();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "dogfight tests passed\n";
    return 0;
}
