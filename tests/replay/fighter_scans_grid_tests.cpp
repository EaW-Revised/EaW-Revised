#include "fighter_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fighter_test_support {

// WDF-05: both grids laid over the map box (Coruscant's low corner, -6500 on both axes).
void test_map_box_grids() {
    const tactical::MapBox box{units(-6500), units(-6500)};
    // The S-97 recording: the X-wing squadron flew to the cell point (500, -1500), which only the
    // box-anchored grid has.
    const auto recorded = tactical::combat_cell_of(at(500, -1500), box);
    expect(recorded == tactical::CombatCell{17, 12}, "WDF-05: (500, -1500) lies in cell (17, 12) of the box grid");
    const auto recorded_point = tactical::combat_cell_point(recorded, units(9), box);
    expect(recorded_point.x == units(500) && recorded_point.y == units(-1500) && recorded_point.z == units(9),
        "WDF-05: cell (17, 12)'s point is the recorded (500, -1500) at the craft's height");
    expect(tactical::combat_cell_point(tactical::combat_cell_of(at(500, -1500), tactical::MapBox{}), Fixed{}, tactical::MapBox{}).x
            != units(500),
        "WDF-05: the world-origin grid has no point at (500, -1500)");
    // An odd row moves x back half a cell and its point half a cell forward.
    const auto odd = tactical::combat_cell_of(at(700, -1100), box);
    expect(odd == tactical::CombatCell{17, 13}, "WDF-05: an odd row shifts x half a cell");
    const auto odd_point = tactical::combat_cell_point(odd, Fixed{}, box);
    expect(odd_point.x == units(700) && odd_point.y == units(-1100), "WDF-05: an odd row's cell point");
    // x is clamped to the box's low x after the odd-row shift.
    expect(tactical::combat_cell_of(at(-6450, -6000), box) == tactical::CombatCell{0, 1},
        "WDF-05: an odd row's x clamps to the box's low x");
    const auto corner = tactical::combat_cell_point({0, 1}, Fixed{}, box);
    expect(corner.x == units(-6100) && corner.y == units(-5900), "WDF-05: cell (0, 1)'s point");
    expect(tactical::combat_cell_of(at(-6500, -6500), box) == tactical::CombatCell{0, 0}, "WDF-05: the low corner is cell (0, 0)");
    // Negative offsets truncate toward zero: a point under the box by less than a cell is row 0.
    expect(tactical::combat_cell_of(at(-6500, -6700), box) == tactical::CombatCell{0, 0},
        "WDF-05: the row truncates, so 200 units under the box is row 0");
    expect(tactical::combat_cell_of(at(-6500, -7000), box) == tactical::CombatCell{0, -1},
        "WDF-05: 500 units under the box is row -1, and its odd row clamps x");
    expect(tactical::combat_cell_of(at(-7000, -6400), box) == tactical::CombatCell{0, 0}, "WDF-05: x left of the box clamps to column 0");
    // The idle grid is the same rule at 120 units.
    expect(tactical::idle_cell_of(at(-6400, -6400), box) == tactical::CombatCell{0, 0}, "WDF-05: idle cell (0, 0)");
    const auto idle_point = tactical::idle_cell_point({0, 0}, units(4), box);
    expect(idle_point.x == units(-6440) && idle_point.y == units(-6440) && idle_point.z == units(4),
        "WDF-05: idle cell (0, 0)'s point is min + 60");
    expect(tactical::idle_cell_of(at(-6300, -6300), box) == tactical::CombatCell{1, 1}, "WDF-05: an odd idle row shifts x half a cell");
    const auto idle_odd = tactical::idle_cell_point({1, 1}, Fixed{}, box);
    expect(idle_odd.x == units(-6260) && idle_odd.y == units(-6320), "WDF-05: an odd idle row's point");
    // The cell point round-trips inside the box, on both grids.
    for (std::int32_t y = 0; y < 8; ++y) {
        for (std::int32_t x = 0; x < 8; ++x) {
            const tactical::CombatCell cell{x, y};
            expect(tactical::combat_cell_of(tactical::combat_cell_point(cell, Fixed{}, box), box) == cell,
                "WDF-05: a combat cell's point lies in that cell");
            expect(tactical::idle_cell_of(tactical::idle_cell_point(cell, Fixed{}, box), box) == cell,
                "WDF-05: an idle cell's point lies in that cell");
        }
    }
    // The claim searches around the cell under the point, in the box's grid.
    const auto claimed = tactical::idle_cell_claim(at(-6300, -6300), {}, box);
    expect(claimed && *claimed == tactical::CombatCell{1, 1}, "WDF-05: the idle claim starts at the box grid's cell");
}

// C-30 (#687, FM-23, FM-24): the idle cell a squadron claims.
void test_idle_cell_claim() {
    const auto desired = at(70, 250);
    const auto start = tactical::idle_cell_of(desired, tactical::MapBox{});
    expect(start.x == 0 && start.y == 2, "FM-23: (70, 250) lies in cell (0, 2)");
    const auto point = tactical::idle_cell_point(start, units(20), tactical::MapBox{});
    expect(point.x == units(60) && point.y == units(300) && point.z == units(20), "FM-23: cell (0, 2)'s point is (60, 300)");
    const auto odd = tactical::idle_cell_point({0, 1}, Fixed{}, tactical::MapBox{});
    expect(odd.x == units(120) && odd.y == units(180), "FM-23: an odd row is shifted half a cell");
    const auto free = tactical::idle_cell_claim(desired, {}, tactical::MapBox{});
    expect(free && *free == start, "FM-24: the free cell under the desired point");
    const std::vector<tactical::CombatCell> taken{start};
    const auto next = tactical::idle_cell_claim(desired, taken, tactical::MapBox{});
    expect(next && *next == tactical::CombatCell{0, 1}, "FM-24: the nearest free cell of the first ring, (120, 180)");
    std::vector<tactical::CombatCell> full;
    for (std::int32_t y = start.y - 4; y <= start.y + 4; ++y) {
        for (std::int32_t x = start.x - 4; x <= start.x + 4; ++x) full.push_back({x, y});
    }
    expect(!tactical::idle_cell_claim(desired, full, tactical::MapBox{}), "FM-24: no claim once 64 cells show no free one");
    full.erase(std::find(full.begin(), full.end(), tactical::CombatCell{start.x + 3, start.y + 3}));
    expect(tactical::idle_cell_claim(desired, full, tactical::MapBox{}).has_value(),
        "FM-24: a free cell of the third ring is found within the 64 examined");
}

// C-31 (#687, FM-24 to FM-26): squadrons 10 (craft 11, 12) and 20 (craft 21) each moved to (1500,
// 0) by their own order hold two idle cells; the mobile carrier 1's two launched squadrons escort it and
// hold two more. The run records the hashes and the cells at the end.
struct IdleRun {
    std::vector<std::string> hashes;
    std::vector<std::pair<eawr::sim::EntityId, tactical::CombatCell>> cells; // ascending container
};

// WDF-05: the idle fixture lies below the world origin; its box starts below the carrier.
const tactical::MapBox idle_box{units(-4800), units(-4800)};

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
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat(),
        tactical::VictoryRules{}, tactical::AbilityTable{}, tactical::EconomyRules{}, idle_box);
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
            const auto p = tactical::idle_cell_point(run.cells[a].second, Fixed{}, idle_box);
            const auto q = tactical::idle_cell_point(run.cells[b].second, Fixed{}, idle_box);
            const double dx = static_cast<double>(p.x.raw() - q.x.raw()) / static_cast<double>(one);
            const double dy = static_cast<double>(p.y.raw() - q.y.raw()) / static_cast<double>(one);
            expect(std::sqrt(dx * dx + dy * dy) >= 119.0, "FM-23: held cells are at least a cell apart");
        }
    }
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_idle(workers).hashes == run.hashes, "C-31: " + std::to_string(workers) + " workers hash like one");
    }
}
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

// FM-20 (#1192): a placed squadron's idle hold is its leader's own point, not the container's.
// The leader starts on it, so it no longer aims back past the container and the wingman's
// half-distance speed no longer runs away; 1, 2, 4 and 8 workers alike. `tracks` takes crafts 11
// and 12 at every frame, the start included.
[[nodiscard]] std::vector<std::string> run_placed_idle(const std::size_t workers, std::optional<math::Vec3>* anchor,
    std::array<std::vector<math::Vec3>, 2>* tracks) {
    std::vector<std::string> hashes;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(20, -30)),
        unit(12, craft_type, empire, at(-10, 30))};
    setup.squadrons = {{10, {11, 12}}};
    auto table = motion();
    table.squadrons.spawners.clear();
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
    expect(static_cast<bool>(created), "FM-20: the session builds");
    if (!created) return hashes;
    auto session = std::move(created).value();
    if (anchor != nullptr) {
        const auto state = session.squadron_state(10);
        if (state) *anchor = state->anchor;
    }
    if (tracks != nullptr) {
        (*tracks)[0].push_back(at(20, -30));
        (*tracks)[1].push_back(at(-10, 30));
    }
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 60; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "FM-20: step failed: " + stepped.error().message);
            break;
        }
        hashes.push_back(stepped.value().state_sha256);
        if (tracks != nullptr) {
            for (const auto& unit : session.units()) {
                if (unit.entity_id == 11) (*tracks)[0].push_back(unit.position);
                if (unit.entity_id == 12) (*tracks)[1].push_back(unit.position);
            }
        }
    }
    return hashes;
}

void test_placed_idle_hold() {
    // A valid rotation can have no planar heading. Initialization must not
    // dereference the failed yaw result; zero is the deterministic project fallback.
    {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(10, squadron_b, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0))};
        const auto half = Fixed::from_raw(one / 2);
        setup.units[1].rotation = {half, half, Fixed::from_raw(-one / 2), half};
        expect(!tactical::yaw_degrees(setup.units[1].rotation), "vertical craft rotation has no planar yaw");
        setup.squadrons = {{10, {11}}};
        auto table = motion();
        table.squadrons.spawners.clear();
        auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table);
        expect(static_cast<bool>(created), "placed vertical craft initializes without an unchecked yaw read");
        if (created) {
            const auto flight = created.value().craft_state(11);
            expect(flight && flight->yaw == Fixed{}, "placed vertical craft uses the deterministic zero heading");
        }
    }
    std::optional<math::Vec3> anchor;
    std::array<std::vector<math::Vec3>, 2> tracks;
    const auto hashes = run_placed_idle(1, &anchor, &tracks);
    expect(anchor && anchor->x == units(20) && anchor->y == units(-30),
        "FM-20: a placed squadron holds its leader's point, not the container's");
    expect(tracks[0].size() == 61 && tracks[1].size() == 61, "FM-20: both craft are tracked over 60 frames");
    // Test-only bound, no runtime clamp (G-F10): retail's idle craft peak at about 2.6 times Max_Speed
    // in the S-97 capture (377 and 372 units/s against the craft's own maximum), so no idle craft of
    // a placed squadron may exceed three times its type's Max_Speed in any frame. Before the leader
    // point hold the wingmen climbed to four to six times in half a second.
    const auto profiles = motion(); // the table outlives the pointer into it
    const auto* craft = profiles.squadrons.find_craft(craft_type);
    expect(craft != nullptr, "FM-20: the fixture craft profile exists");
    if (craft != nullptr) {
        const double bound = 3.0 * static_cast<double>(craft->max_speed.raw()) / static_cast<double>(one);
        for (const auto& track : tracks) {
            for (std::size_t frame = 1; frame < track.size(); ++frame) {
                const double dx = static_cast<double>(track[frame].x.raw() - track[frame - 1].x.raw()) / static_cast<double>(one);
                const double dy = static_cast<double>(track[frame].y.raw() - track[frame - 1].y.raw()) / static_cast<double>(one);
                const double dz = static_cast<double>(track[frame].z.raw() - track[frame - 1].z.raw()) / static_cast<double>(one);
                expect(std::sqrt(dx * dx + dy * dy + dz * dz) <= bound,
                    "FM-20: an idle craft of a placed squadron stays within 3x Max_Speed, frame "
                    + std::to_string(frame));
            }
        }
    }
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_placed_idle(workers, nullptr, nullptr) == hashes,
            "FM-20: " + std::to_string(workers) + " workers hash like one");
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

    // WSQ-64: the original attack destination dies, but other live targets remain.
    // WMV-18 preserves its converted position formation; a later autonomous
    // replacement must not rebase the already-idle hold point.
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(10, squadron_a, empire, at(1000, 500, -200)),
            unit(11, craft_type, empire, at(1000, 500, -200)),
            unit(12, craft_type, empire, at(990, 510, -200)),
            unit(20, corvette_type, rebel, at(1300, 500, -200)),
            unit(30, corvette_type, rebel, at(1100, 700))};
        setup.squadrons = {{10, {11, 12}}};
        auto table = motion();
        table.squadrons.spawners.clear();
        auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
        expect(static_cast<bool>(created), "WSQ-64: target-loss fixture builds");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(tactical::PlayerCommand{{0, empire, 0}, {10}, tactical::AttackPayload{20}})),
            "WSQ-64: original attack submits");
        expect(static_cast<bool>(session.submit(tactical::PlayerCommand{{8, empire, 1}, {20}, tactical::DamagePayload{units(4000)}})),
            "WSQ-64: original target destruction submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        bool replaced = false;
        for (std::uint64_t tick = 0; tick < 160; ++tick) {
            if (tick == 70) {
                const auto held = session.squadron_state(10);
                expect(held && held->formation && held->formation->base_target == 0 && held->target == 30,
                    "WSQ-64: converted formation retains the other live target before replacement");
                const auto spawned = session.stage_spawn(unit(0, frigate_type, rebel, at(1000, 600)));
                expect(spawned && spawned.value() == 31, "WSQ-64: better live target stages at the next stable ID");
            }
            const auto before = session.squadron_state(10);
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "WSQ-64: target-loss and retarget step completes");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            const auto after = session.squadron_state(10);
            if (before && after && before->target == 30 && after->target == 31) {
                replaced = true;
                expect(before->formation && after->formation && after->formation->attack_target == 31,
                    "WSQ-64: autonomous replacement uses the preserved formation");
                expect(after->anchor == before->anchor,
                    "WSQ-64: replacing a live target preserves the idle hold point");
                const auto leader = session.combat_state(11), follower = session.combat_state(12);
                expect(leader && follower && (leader->attack_target == 31 || follower->attack_target == 31),
                    "WSQ-64: replacement still reaches member targeting");
            }
        }
        expect(replaced, "WSQ-64: a live autonomous target replacement was observed");
        expect(hashes.size() == 160, "WSQ-64: the complete target-loss control ran");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WSQ-64: target-loss retarget hashes agree on 1/2/4/8 workers");
    }
}

} // namespace fighter_test_support
