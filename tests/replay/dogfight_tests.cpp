// #457 squadron dogfights (docs/behaviour/space-fighters.md FD-01 to FD-12, cases C-22 to C-28):
// the combat cell and retaliation, chase pairing and its ten-second timers, following, the end of
// a fight, and ship avoidance, each hashing alike with 1, 2, 4 and 8 workers.

#include "../support/check.hpp"
#include "dogfight_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dogfight_test_support {


eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
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
    auto created = tactical::TacticalSession::create(fixture.setup, sensors, fixture.durability, fixture.motion, std::nullopt, fixture.weapons,
        tactical::VictoryRules{}, tactical::AbilityTable{}, tactical::EconomyRules{}, fixture.box);
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

void test_refused_member_find() {
    auto fixture = trio_fight();
    fixture.orders = {{rebel, 20, tactical::MovePayload{at(2000, 0)}}};
    // The detached follower can see and attack the enemy; the formation's leader cannot
    // attack it from the idle allowance circle. Sharing and pairing still run on refusal.
    fixture.setup.units[2].position = at(600, 20);
    fixture.setup.units[3].position = at(600, -20);
    for (std::size_t index = 4; index < fixture.setup.units.size(); ++index)
        fixture.setup.units[index].position.x = units(900);
    bool found = false;
    const auto hashes = run(fixture, 1, 2, [&](const auto& session, const auto&) {
        const auto formation = session.squadron_state(10);
        expect(formation && formation->target == 20 && formation->formation
            && formation->formation->attack_target == 0 && !formation->formation->attack_override,
            "WDF-01/03: the team shares the find while the idle formation refuses diversion");
        for (const EntityId member : {EntityId{12}, EntityId{13}}) {
            const auto combat_state = session.combat_state(member);
            const auto flight = session.craft_state(member);
            found = found || (combat_state && combat_state->attack_target == 20);
            const auto chased = session.craft_state(member + 10);
            expect(flight && chased && flight->chase == 0 && flight->chase_until == 301
                && chased->chase == member && chased->chase_until == 301,
                "FS-18/WDF-03/WSQ-20: earlier notification reserves the reciprocal roster clocks despite refusal");
        }
        const auto leader = session.combat_state(11);
        expect(leader && leader->attack_target == 0,
            "WDF-02: the refused shared target is cleared only for the out-of-reach member");
    });
    expect(found, "WDF-02: an in-reach refused find remains a member target");
    expect_workers(fixture, 2, hashes, "WDF-02/03 refused member find");
}

void test_solo_fighter() {
    // WHE-SQ-02: the hero is one real craft, not a ship or an extra invisible container.
    for (const bool enemy_solo : {false, true}) {
        auto fixture = trio_fight();
        fixture.setup.units.erase(fixture.setup.units.begin(), fixture.setup.units.begin() + 4);
        fixture.setup.units.insert(fixture.setup.units.begin(), unit(30, craft_type, empire, at(-350, 0)));
        fixture.setup.squadrons.erase(fixture.setup.squadrons.begin());
        fixture.motion.squadrons.squadrons.insert(fixture.motion.squadrons.squadrons.begin(),
            {craft_type, {craft_type}, {at(0, 0)}, units(1000), units(200), units(300), units(20)});
        if (enemy_solo) {
            fixture.setup.units.resize(2);
            fixture.setup.units[1] = unit(20, craft_type, rebel, at(350, 0), 180);
            fixture.setup.squadrons.clear();
        }
        // FS-18/20: create the tested solo craft after the opposing group so
        // this remains a chaser control, rather than only a chased reservation.
        std::sort(fixture.setup.units.begin(), fixture.setup.units.end(), [](const auto& left, const auto& right) {
            return left.entity_id < right.entity_id;
        });
        for (auto& order : fixture.orders) if (order.unit == 10) order.unit = 30;
        bool moved = false;
        bool cell = false;
        bool chase = false;
        const auto hashes = run(fixture, 1, 360, [&](const auto& session, const auto& tick) {
            const auto position = position_of(*tick.snapshot, 30);
            moved = moved || (position && *position != at(-350, 0));
            expect(session.units().size() == fixture.setup.units.size(),
                "WHE-SQ-02: solo flight creates no extra entity");
            const auto flight = session.craft_state(30);
            chase = chase || (flight && flight->chase != eawr::sim::invalid_entity_id);
            for (const auto& target : tick.snapshot->squadron_targets())
                if (target.squadron == 30) cell = cell || target.recorded;
        });
        expect(moved && cell && chase, "WHE-SQ-02: solo fighter moves, joins fighter combat and chases");
        expect_workers(fixture, 360, hashes, "WHE-SQ-02 solo fighter");
    }
}

// WSQ-15, WWP-21 (#739): the strafe reach adds the target's soft radius as the combat world gives
// it: a lone fighter its type's radius, a team container in a formation its ideal team box's.
// Squadron 10 records the WU-25a cell (FD-02) on the first frame its leader starts within
// Strafe_Distance (200) plus that radius of the target (unit 20), in the plane.
void test_reach_soft_radius() {
    for (const bool team : {false, true}) {
        auto fixture = trio_fight();
        double radius = 100.0;
        if (team) {
            // Craft hard extents of 10 about the slots (0, 0), (-20, 20), (-20, -20) and the origin:
            // half extents 20 and 30.
            fixture.weapons.footprints = {tactical::TargetFootprint{craft_type, units(10), units(10), units(10)}};
            radius = std::sqrt(20.0 * 20.0 + 30.0 * 30.0);
        } else {
            // WHE-SQ-02: unit 20 is one craft flying as its own squadron; its type radius is 100.
            fixture.setup.units.resize(5);
            fixture.setup.units[4] = unit(20, craft_type, rebel, at(350, 0), 180);
            fixture.setup.squadrons.resize(1);
            fixture.motion.squadrons.squadrons.insert(fixture.motion.squadrons.squadrons.begin(),
                {craft_type, {craft_type}, {at(0, 0)}, units(1000), units(200), units(300), units(20)});
            fixture.weapons.footprints = {tactical::TargetFootprint{craft_type, units(100), units(10), units(10)}};
        }
        std::optional<math::Vec3> leader;
        std::optional<math::Vec3> target;
        bool target_formed = false;
        std::optional<double> reached;
        bool formed_then = false;
        const auto hashes = run(fixture, 1, 200, [&](const auto& session, const auto& tick) {
            const auto state = session.squadron_state(10);
            if (!reached && state && state->cell && leader && target) {
                const double x = as_double(leader->x) - as_double(target->x);
                const double y = as_double(leader->y) - as_double(target->y);
                reached = std::sqrt(x * x + y * y);
                formed_then = target_formed;
            }
            leader = position_of(*tick.snapshot, 11);
            target = position_of(*tick.snapshot, 20);
            const auto theirs = session.squadron_state(20);
            target_formed = theirs && theirs->formation.has_value();
        });
        const std::string label = team ? "formed team" : "lone fighter";
        if (team) expect(formed_then, "WWP-21: squadron 20 is in a formation when squadron 10 reaches it");
        expect(reached && *reached <= 200.0 + radius + 0.001 && *reached > 200.0 + radius - 12.0,
            "WSQ-15: the " + label + " target's soft radius widens the strafe reach (cell recorded at "
                + (reached ? std::to_string(*reached) : std::string{"never"}) + ", reach "
                + std::to_string(200.0 + radius) + ")");
        expect_workers(fixture, 200, hashes, "WSQ-15 " + label + " target reach");
    }
}

// WSQ-53, FM-14 (#741): every frame the team container takes its leader's whole facing, and its
// snapshot placement turns with it about the craft's centre.
void test_container_facing() {
    auto fixture = trio_fight();
    fixture.orders = {{empire, 10, tactical::MovePayload{at(-350, 1500)}}};
    const auto rotation_rows = [](const tactical::TacticalSnapshot& snapshot, const EntityId id) {
        std::optional<std::array<Fixed, 9>> rows;
        for (const auto& entry : snapshot.instances()) {
            if (entry.entity_id != id) continue;
            rows.emplace();
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 3; ++column) (*rows)[row * 3 + column] = entry.fixed_transform.rows[row][column];
            }
        }
        return rows;
    };
    bool turned = false;
    const auto hashes = run(fixture, 1, 90, [&](const auto& session, const auto& tick) {
        std::optional<math::Quat> container;
        std::optional<math::Quat> leader;
        for (const auto& state : session.units()) {
            if (state.entity_id == 10) container = state.rotation;
            if (state.entity_id == 11) leader = state.rotation;
        }
        expect(container && leader && *container == *leader,
            "WSQ-53: the container's facing is its leader's at tick " + std::to_string(tick.completed_tick));
        const auto own = rotation_rows(*tick.snapshot, 10);
        expect(own && own == rotation_rows(*tick.snapshot, 11),
            "WSQ-53: the container's placement turns with its leader at tick " + std::to_string(tick.completed_tick));
        turned = turned || (leader && *leader != math::identity_quat());
    });
    expect(turned, "WSQ-53: the leader turns during the move");
    expect_workers(fixture, 90, hashes, "WSQ-53 container facing");
}

// WSQ-59 (#741): a move given to one craft of a squadron is accepted (it still ends the team's
// ion shot and Hunt, WAB-40, WAB-51) but changes nothing on the craft: the battle runs as if
// no order came.
void test_member_move() {
    const auto fixture = trio_fight();
    auto ordered = fixture;
    ordered.orders.push_back({empire, 12, tactical::MovePayload{at(-2000, 0)}, {}, 30});
    std::vector<std::vector<tactical::UnitState>> plain;
    std::vector<std::optional<tactical::CraftState>> plain_flight;
    static_cast<void>(run(fixture, 1, 120, [&](const auto& session, const auto&) {
        plain.push_back(session.units());
        plain_flight.push_back(session.craft_state(12));
    }));
    bool accepted = false;
    std::size_t index = 0;
    bool same = true;
    const auto hashes = run(ordered, 1, 120, [&](const auto& session, const auto& tick) {
        for (const auto& event : tick.snapshot->events()) {
            accepted = accepted || (event.unit == 12 && event.kind == tactical::EventKind::order_accepted);
        }
        same = same && index < plain.size() && session.units() == plain[index] && session.craft_state(12) == plain_flight[index];
        ++index;
    });
    expect(accepted, "WSQ-59: the move given to one craft is accepted");
    expect(same && index == plain.size(), "WSQ-59: the craft keeps its order and flight as if no move came");
    expect_workers(ordered, 120, hashes, "WSQ-59 member move");
}

// WHZ-20/23, WSQ-06/51: nebula contact disables Hunt, but a surviving
// squadron leader still has its ordinary idle flight and target service.
void test_nebula_survivor() {
    constexpr tactical::TypeId volume_type = 500;
    for (const bool human : {false, true}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = seed;
            setup.players = players();
            setup.units = {unit(10, trio_type, empire, at(0, 0)),
                unit(11, craft_type, empire, at(0, 0)),
                unit(12, craft_type, empire, at(-20, 20)),
                unit(13, craft_type, empire, at(-20, -20)),
                unit(20, frigate_type, rebel, at(300, 0)),
                unit(30, volume_type, empire, at(0, 0))};
            setup.squadrons = {{10, {11, 12, 13}}};
            auto movement = motion(false);
            movement.nebula_service_types = {craft_type};
            movement.nebula_disable_seconds = units(5);
            // The same validated avoidance interface as the hazard contracts.
            const auto decimal = [](const char* text) { return Fixed::from_decimal(text).value(); };
            movement.avoidance = tactical::AvoidanceRules{units(24), decimal("0.2"), units(100),
                decimal("0.8"), units(15), decimal("0.66"), decimal("1.2"), decimal("0.25"),
                decimal("1.7"), decimal("0.5"), decimal("0.5"), 3500, 6, 90, 45, units(50)};
            tactical::Footprint volume;
            volume.type_id = volume_type;
            volume.layer = tactical::SpaceLayer::static_object;
            volume.radius = units(2800);
            volume.obstacle = true;
            volume.nebula = true;
            movement.footprints.push_back(volume);
            tactical::AbilityProfile hunt;
            hunt.kind = tactical::AbilityKind::hunt;
            tactical::AbilityTable abilities;
            abilities.profiles = {{craft_type, {hunt}, false}};
            if (human) abilities.humans = {empire};
            const std::vector<tactical::SensorProfile> sensors{{craft_type, units(800)}, {trio_type, units(800)}};
            auto created = tactical::TacticalSession::create(setup, sensors, durability(1000000),
                movement, std::nullopt, combat(), {}, abilities);
            expect(static_cast<bool>(created), "nebula survivor fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto session = std::move(created).value();
            expect(static_cast<bool>(session.submit({{1, empire, 1}, {11, 12},
                tactical::DamagePayload{units(2000000)}})), "reduce squadron to its last craft");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            bool moved = false, targeted = false, contacted = false;
            std::optional<math::Vec3> previous;
            for (std::size_t tick = 0; tick < 240; ++tick) {
                const auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "nebula survivor advances");
                if (!stepped) { std::cerr << stepped.error().message << '\n'; break; }
                if (workers == 1) reference.push_back(stepped.value().state_sha256);
                else expect(tick < reference.size() && stepped.value().state_sha256 == reference[tick],
                    "nebula survivor remains equal on 1/2/4/8 workers");
                const auto point = position_of(*stepped.value().snapshot, 13);
                if (tick >= 3 && point && previous) moved = moved || *point != *previous;
                previous = point;
                const auto mind = session.squadron_state(10);
                targeted = targeted || (mind && mind->target != 0);
                const auto flight = session.craft_state(13);
                bool nebula = false;
                for (const auto& instance : stepped.value().snapshot->instances())
                    if (instance.entity_id == 13) nebula = instance.in_nebula;
                contacted = contacted || nebula;
                if (workers == 1 && tick % 30 == 0 && point && mind && flight) {
                    std::cout << "nebula-survivor human=" << human << " tick=" << tick
                        << " mode=" << static_cast<int>(mind->mode) << " target=" << mind->target
                        << " roster=";
                    for (const auto id : mind->roster) std::cout << id << ':';
                    std::cout << " x=" << point->x.raw() << " y=" << point->y.raw()
                        << " vx=" << flight->velocity.x.raw() << " vy=" << flight->velocity.y.raw()
                        << " nebula=" << nebula << '\n';
                }
                if (tick == 3) expect(!session.craft_state(11) && !session.craft_state(12) && flight,
                    "last non-original leader survives alone");
            }
            expect(contacted, "survivor really is inside a nebula");
            expect(moved, "nebula survivor retains ordinary fighter motion");
            expect(targeted, "nebula survivor acquires a revealed hostile in range");
        }
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
    test_refused_member_find();
    test_solo_fighter();
    test_survivor_formation_slots();
    test_reach_soft_radius();
    test_container_facing();
    test_member_move();
    test_nebula_survivor();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "dogfight tests passed\n";
    return 0;
}
