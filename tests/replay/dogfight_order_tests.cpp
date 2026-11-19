#include "dogfight_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace dogfight_test_support {

// OR-22/23/25: stock TIE-bomber flight and torpedo timing, with synthetic frigate
// geometry to distinguish an explicit far attachment from both autonomous choices.
void test_bomber_hardpoint_approach() {
    for (const std::string variant : {"far", "fogged", "other-destroyed", "moving", "in-range", "ordered-destroyed",
             "fog-lost", "autonomous"}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            const auto label = "bomber hardpoint " + variant + "/" + std::to_string(workers);
            const bool fogged = variant == "fogged";
            const bool moving = variant == "moving";
            const bool lost = variant == "ordered-destroyed";
            const bool fog_lost = variant == "fog-lost";
            const bool autonomous = variant == "autonomous";
            tactical::TacticalSetup setup;
            setup.seed = seed;
            setup.players = players();
            const auto start = variant == "in-range" || autonomous ? -300 : -1500;
            setup.units = {unit(10, trio_type, empire, at(start, 0)),
                unit(11, craft_type, empire, at(start, 0)), unit(12, craft_type, empire, at(start - 20, 20)),
                unit(13, craft_type, empire, at(start - 20, -20)), unit(14, craft_type, empire, at(start - 40, 0)),
                unit(20, frigate_type, rebel, at(0, 0))};
            setup.squadrons = {{10, {11, 12, 13, 14}}};
            auto flight = motion();
            auto& bomber = flight.squadrons.craft.front();
            bomber.max_speed = units(3);
            bomber.min_speed = units(2);
            bomber.rate_of_turn = Fixed::from_raw(one * 13 / 10);
            bomber.lift = units(3);
            bomber.thrust = units(1);
            bomber.roll_rate = units(3);
            bomber.bank_angle = units(30);
            bomber.strafe_distance = units(500);
            auto& team = flight.squadrons.squadrons.front();
            team.members.push_back(craft_type);
            team.offsets.push_back(at(-40, 0));
            if (moving) {
                tactical::MotionProfile ship;
                ship.type_id = frigate_type;
                ship.max_speed = ship.acceleration = ship.deceleration = units(1);
                ship.rate_of_turn = units(90);
                ship.turn_in_place_slowdown = units(1);
                flight.profiles = {ship};
            }
            auto health = durability(1000000);
            health.damage.emplace();
            health.damage->shield_recharge_frames = 30;
            tactical::HardpointProfile point;
            point.role = tactical::HardpointRole::weapon;
            point.destroyable = true;
            point.max_health = units(100000);
            health.profiles.back().hardpoints = {point, point, point};
            auto guns = combat();
            // 0 is nearest, 1 is highest ranked, and the ordered 2 is neither.
            guns.priority_sets.front().hardpoint_ranks = {units(1000), units(2), units(1), units(3)};
            auto& weapon = guns.profiles.front().weapons.front();
            weapon.range = units(700);
            weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 800;
            weapon.pulse_count = 2;
            weapon.pulse_delay_frames = 4;
            weapon.cone_width = weapon.cone_height = units(30);
            auto& frigate = guns.profiles.back();
            frigate.hardpoints = {{0, at(-80, 0), true, 1}, {1, at(0, -60), true, 2}, {2, at(80, 40), true, 3}};
            frigate.collision = tactical::CollisionBox{at(-100, -100, -100), at(100, 100, 100)};
            frigate.aimed_routes = {0, 1, 2};
            const auto reveal = fog_lost ? 100 : (fogged ? 900 : 8000);
            std::vector<tactical::SensorProfile> sight{{craft_type, units(reveal)},
                {trio_type, units(reveal)}, {frigate_type, units(8000)}};
            if (fog_lost) {
                // A short reveal radius can still contact the ship as the craft fly past.
                // Disable both friendly reveal sources to keep this loss control fogged.
                sight[0].reveals = sight[1].reveals = false;
            }
            auto made = tactical::TacticalSession::create(setup, sight, health, flight, std::nullopt, guns);
            expect(static_cast<bool>(made), label + ": session builds");
            if (!made) continue;
            auto session = std::move(made).value();
            if (!autonomous) expect(static_cast<bool>(session.submit({{0, empire, 1}, {10}, tactical::AttackPayload{20, 2}})),
                label + ": order submits");
            if (moving) expect(static_cast<bool>(session.submit({{0, rebel, 1}, {20}, tactical::MovePayload{at(1200, 0)}})),
                label + ": target move submits");
            if (variant == "other-destroyed" || lost) expect(static_cast<bool>(session.submit({{50, empire, 2}, {20},
                tactical::DamagePayload{units(100000), lost ? 2U : 0U}})), label + ": destruction submits");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            bool kept = true, aimed = true, hit = true, fell_back = false, released = false;
            std::set<EntityId> shooters, hitters;
            for (int tick = 0; tick < 1500; ++tick) {
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), label + ": step succeeds");
                if (!step) break;
                if (workers == 1) reference.push_back(step.value().state_sha256);
                else expect(static_cast<std::size_t>(tick) < reference.size() && step.value().state_sha256 == reference[tick],
                    label + ": hashes match one worker");
                if (fog_lost) {
                    for (const auto& instance : step.value().snapshot->instances()) {
                        if (instance.entity_id == 20) expect((instance.visible_to & 1U) == 0,
                            label + ": the loss control remains fogged to the attacking player");
                    }
                }
                // FO-03: the command reaches the craft in their next service.
                if (step.value().completed_tick == 1) continue;
                const auto state = session.squadron_state(10);
                const bool standing = !lost || step.value().completed_tick <= 50;
                for (const EntityId id : {EntityId{11}, EntityId{12}, EntityId{13}, EntityId{14}}) {
                    const auto craft = session.combat_state(id);
                    const bool retains = craft && craft->attack_target == 20 && craft->direct && craft->attack_hardpoint == 2;
                    if (autonomous) {
                        kept = kept && craft && !craft->direct && craft->attack_hardpoint != 2;
                    } else if (fog_lost && state && !state->approach && state->target == 0) {
                        released = true;
                        kept = kept && craft && !craft->direct && craft->attack_hardpoint == tactical::no_hardpoint;
                    } else if (standing && !retains) {
                        if (kept) std::cout << label << " first loss tick=" << step.value().completed_tick << " craft=" << id
                            << " target=" << (craft ? craft->attack_target : 0) << " direct=" << (craft && craft->direct)
                            << " hardpoint=" << (craft ? craft->attack_hardpoint : tactical::no_hardpoint)
                            << " team-hardpoint=" << (state ? state->target_hardpoint : tactical::attack_hull) << '\n';
                        kept = false;
                    }
                }
                for (const auto& event : step.value().snapshot->combat_events()) {
                    if (event.target != 20 || event.shooter < 11 || event.shooter > 14) continue;
                    if (event.kind == tactical::CombatEventKind::weapon_fired) {
                        shooters.insert(event.shooter);
                        if (standing) aimed = aimed && event.target_hardpoint == (autonomous ? 1U : 2U);
                        else fell_back = fell_back || event.target_hardpoint != 2;
                    }
                    if (event.kind == tactical::CombatEventKind::projectile_hit && standing) {
                        hitters.insert(event.shooter);
                        hit = hit && event.target_hardpoint == (autonomous ? 1U : 2U);
                    }
                }
            }
            expect(kept, label + ": every craft preserves the active order and clears it when lost");
            expect(aimed && hit, label + ": every shot and hit names the standing ordered hardpoint");
            if (fog_lost) expect(released && shooters.empty() && hitters.empty(),
                label + ": fog still blocks fire and ends an approach that never reveals its target");
            else expect(shooters.size() == 4 && (lost || hitters.size() == 4), label + ": all four bombers fire and hit");
            expect(!lost || fell_back, label + ": destroying the ordered hardpoint permits fallback");
            std::cout << label << " kept=" << kept << " aimed=" << aimed << " hit=" << hit
                << " shooters=" << shooters.size() << " hitters=" << hitters.size() << '\n';
        }
    }
}

// the rebels then order 20 onto hardpoint 1 of the empire frigate 40. The service turns 20 back
// on squadron 10 (FD-04, its target no longer a craft of a squadron), and the ordered hardpoint
// would name a hardpoint of another target: neither the squadron's state nor its craft's weapons
// may keep it.
void test_retarget_clears_the_hardpoint() {
    auto fixture = trio_fight();
    fixture.setup.units.push_back(unit(40, frigate_type, empire, at(0, 2500)));
    tactical::HardpointProfile hardpoint;
    hardpoint.role = tactical::HardpointRole::weapon;
    hardpoint.destroyable = true;
    hardpoint.max_health = units(100);
    for (auto& profile : fixture.durability.profiles) {
        if (profile.type_id == frigate_type) profile.hardpoints = {hardpoint, hardpoint};
    }
    for (auto& profile : fixture.weapons.profiles) {
        if (profile.type_id == frigate_type) profile.hardpoints = {{0, at(0, -30), true}, {1, at(0, 30), true}};
    }
    constexpr std::uint64_t order_tick = 500;
    fixture.orders.push_back({rebel, 20, tactical::AttackPayload{40, 1}, {}, order_tick});
    bool fighting = false;
    bool retargeted = false;
    bool stale = false;
    bool craft_stale = false;
    const auto hashes = run(fixture, 1, 700, [&](const tactical::TacticalSession& session, const tactical::TacticalTick& tick) {
        const auto rebels = session.squadron_state(20);
        const auto empires = session.squadron_state(10);
        if (!rebels || !empires) return;
        if (tick.completed_tick + 1 == order_tick) {
            fighting = rebels->joined && empires->joined && rebels->cell == empires->cell;
        }
        if (tick.completed_tick < order_tick) return;
        retargeted = retargeted || rebels->target == 10;
        stale = stale || (rebels->target_hardpoint != tactical::attack_hull && rebels->target != 40);
        for (const EntityId craft : {EntityId{21}, EntityId{22}, EntityId{23}}) {
            const auto combat_state = session.combat_state(craft);
            craft_stale = craft_stale
                || (combat_state && combat_state->direct && combat_state->attack_hardpoint != tactical::no_hardpoint
                    && combat_state->attack_target != 40);
        }
    });
    expect(hashes.size() == 700, "C-29: 700 ticks");
    expect(fighting, "C-29: the squadrons fight in one cell when the order comes");
    expect(retargeted, "C-29 (FD-04): the dogfight turns the ordered squadron back on its attacker");
    expect(!stale, "C-29 (OR-23): a retarget clears the squadron's ordered hardpoint");
    expect(!craft_stale, "C-29 (OR-23): no craft keeps a hardpoint of a target it was retargeted from");
    expect_workers(fixture, 700, hashes, "C-29");
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
    expect(split && split.value()[3].point == at(600, 0) && split.value()[3].lane
            && split.value()[3].lane->origin == far_apart[3].position
            && split.value()[3].lane->ahead == Fixed{} && split.value()[3].lane->aside == Fixed{},
        "FO-07: a squadron far from the others flies alone to the point");

    {
        tactical::SquadronProfile parent;
        parent.formation_type_max_speed = units(8);
        parent.formation_deceleration = units(2);
        const auto roster = tactical::formation_member_deceleration(parent, units(4), units(1));
        const auto modified = tactical::formation_member_deceleration(parent, units(4), units(2));
        const auto surviving = tactical::formation_member_deceleration(parent, units(8), units(1));
        expect(roster && modified && surviving && roster.value() == units(1)
            && modified.value() == units(2) && surviving.value() == units(2),
            "WMV-44: live team base speed and the member's own speed modifier scale its override");
        parent.formation_type_max_speed = Fixed{};
        const auto raw_override = tactical::formation_member_deceleration(parent, units(4), units(2));
        expect(raw_override && raw_override.value() == units(2),
            "WMV-44: explicitly zero type maximum bypasses both roster and modifier scaling");
    }
    // FO-10, FO-11: the lane steer and the row keeping.
    // WMV-42/44: independently exercise the forward and sideways correction ceilings.
    {
        std::vector<tactical::LaneMember> lanes(2);
        for (auto& lane : lanes) {
            lane.lane.direction = at(1, 0);
            lane.max_speed = units(30);
        }
        lanes[1].max_speed = units(2);
        lanes[1].position = at(24, 0);
        const auto absent = tactical::formation_lane_flight(lanes, Fixed{}, units(1));
        expect(absent && absent.value()[0].speed == units(2),
            "WMV-42: absent deceleration caps forward correction at formation speed");
        lanes[0].deceleration = Fixed::from_raw(one / 12);
        const auto authored = tactical::formation_lane_flight(lanes, Fixed{}, units(1));
        expect(authored && std::abs(as_double(authored.value()[0].speed) - 4.0) < 1e-5,
            "WMV-42: forward limit is formation speed plus sqrt(twice deceleration times error)");
        lanes[1].position = at(0, 0);
        lanes[0].lane.aside = units(4);
        lanes[0].deceleration = Fixed::from_raw(one / 2);
        const auto side = tactical::formation_lane_flight(lanes, Fixed{}, units(1));
        // Base forward speed 2; shift 4. Length sqrt(17); side ceiling 2+sqrt(4)=4.
        expect(side && std::abs(as_double(side.value()[0].speed) - std::sqrt(17.0)) < 1e-5,
            "WMV-42: positive sideways component respects its own error-based ceiling");
        lanes[0].deceleration = Fixed{};
        const auto zero = tactical::formation_lane_flight(lanes, Fixed{}, units(1));
        expect(zero && std::abs(as_double(zero.value()[0].speed) - std::sqrt(17.0) / 2) < 1e-5,
            "WMV-42: zero deceleration also caps positive sideways correction");
        lanes[0].lane.aside = units(-4);
        const auto negative = tactical::formation_lane_flight(lanes, Fixed{}, units(1));
        expect(negative && std::abs(as_double(negative.value()[0].speed) - 2 * std::sqrt(17.0)) < 1e-5,
            "WMV-42: debug-build sideways ceiling tests the signed positive component");
        lanes[0].lane.aside = Fixed::from_raw(1);
        const auto tiny = tactical::formation_lane_flight(lanes, Fixed{}, units(1));
        expect(tiny && tiny.value()[0].speed == units(2),
            "WMV-42: a tiny nonbinding side correction does not overflow the ceiling quotient");
    }
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

// Optional cross-build evidence: the same seeded sessions on the original scan

} // namespace dogfight_test_support
