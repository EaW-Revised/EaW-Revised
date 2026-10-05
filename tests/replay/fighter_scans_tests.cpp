#include "fighter_support.hpp"

namespace fighter_test_support {

void test_priority_scan() {
    const auto run_case = [](std::vector<tactical::UnitState> enemies, const bool shaped = false, const bool custom = false) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(1, carrier_type, empire, at(-3000, 0)), unit(10, squadron_a, empire, at(0, 0)),
            unit(11, craft_type, empire, at(0, 0)), unit(12, craft_type, empire, at(-10, 10))};
        for (auto& enemy : enemies) setup.units.push_back(enemy);
        setup.squadrons = {{10, {11, 12}}};
        auto table = motion();
        table.squadrons.spawners.clear();
        if (custom) table.footprints = {{frigate_type, tactical::SpaceLayer::frigate, units(5), units(5), units(5)}};
        auto guns = combat();
        if (shaped) for (auto& profile : guns.profiles) {
            if (profile.type_id == frigate_type) profile.collision = tactical::CollisionBox{at(-80, -5, -5), at(80, 5, 5)};
        }
        auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, guns);
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
    expect(chosen && chosen->attack_target == 20 && !chosen->direct, "C-05: the priority-1 frigate is an autonomous target");
    expect(chosen && chosen->next_scan_frame == 0,
        "WSQ-43: the leader's first share leaves the follower's unused scan clock untouched");
    const auto only_near = run_case({unit(21, corvette_type, rebel, at(300, 0))});
    expect(only_near && only_near->attack_target == 21 && !only_near->direct,
        "C-05 (WSQ-41): an in-range autonomous override over an idle base remains autonomous");
    // Idle chase 200 + attack distance 500 = 700 from the held point.
    const auto beyond = run_case({unit(20, frigate_type, rebel, at(900, 0))});
    expect(beyond && !beyond->direct, "C-05: a unit beyond the chase range plus attack distance is not the squadron's");

    auto long_target = unit(20, frigate_type, rebel, at(650, 300));
    const auto end_on = run_case({long_target}, true);
    expect(end_on && end_on->attack_target == 20 && !end_on->direct,
        "WSQ-45/AT-10: the target's long facing extent admits it beyond the centre-only diversion reach");
    const auto narrowed = run_case({long_target}, true, true);
    expect(narrowed && narrowed->attack_target == eawr::sim::invalid_entity_id,
        "WSQ-45/AV-05: authored hard extents override the target's longer collision bounds");
    const auto quarter_turn = math::normalize(math::Quat{Fixed{}, Fixed{}, units(1), units(1)});
    expect(static_cast<bool>(quarter_turn), "WSQ-45/AT-10: the target's quarter turn builds");
    if (quarter_turn) long_target.rotation = quarter_turn.value();
    const auto broadside = run_case({long_target}, true);
    expect(broadside && broadside->attack_target == eawr::sim::invalid_entity_id,
        "WSQ-45/AT-10: a target seen across its short extent remains beyond diversion reach");

    // WSQ-42/43/47/20: only the follower can find the enemy. Its scan is shared as the
    // enemy team and immediately notifies the whole roster, even before reaching the cell.
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
            unit(12, craft_type, empire, at(-650, 0)), unit(20, squadron_b, rebel, at(-1100, 0)),
            unit(21, craft_type, rebel, at(-1100, 0)), unit(22, craft_type, rebel, at(-1100, 10))};
        setup.squadrons = {{10, {11, 12}}, {20, {21, 22}}};
        auto table = motion();
        table.squadrons.spawners.clear();
        auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
        expect(static_cast<bool>(created), "WSQ-42: follower acquisition fixture builds");
        if (!created) continue;
        auto session = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 5; ++tick) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "WSQ-42: follower acquisition steps");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto mind = session.squadron_state(10);
            expect(mind && mind->formation && mind->formation->has_reached_done && mind->formation->attack_override,
                "WMV-18: an autonomous split retains completed-base history and records its attack override");
            if (tick == 0) {
                expect(mind && mind->formation && !mind->formation->complete
                    && mind->formation->base_position == at(0, 0),
                    "WMV-18: a completed position base is reset to the split team's current position");
            } else {
                expect(mind && mind->formation && mind->formation->complete,
                    "WMV-18: a space-team attack override with completed-base history initializes as done");
            }
            for (const eawr::sim::EntityId id : {11U, 12U}) {
                const auto target = session.combat_state(id);
                expect(target && target->attack_target == 20 && (tick != 0 || !target->direct),
                    "WSQ-43/47: initial sharing uses the autonomous flag and keeps the normalized squadron");
            }
            if (tick == 0) {
                const auto leader = session.craft_state(11), follower = session.craft_state(12);
                const auto chased1 = session.craft_state(21), chased2 = session.craft_state(22);
                expect(leader && follower && chased1 && chased2 && leader->chase == 21 && follower->chase == 22
                        && leader->chase_until == 301 && follower->chase_until == 301
                        && chased1->chase == eawr::sim::invalid_entity_id && chased2->chase == eawr::sim::invalid_entity_id
                        && chased1->chase_until == 301 && chased2->chase_until == 301,
                    "WSQ-20: acquisition pairs in roster order and restarts both timers without a cell/cone test");
                const auto scanner = session.combat_state(12);
                expect(scanner && scanner->next_scan_frame >= 30 && scanner->next_scan_frame <= 45,
                    "WSQ-42: the member owns its 30+0..15-frame scan clock");
            }
        }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WSQ-42/43/20: acquisition hashes match on 1/2/4/8 workers");
        expect(static_cast<bool>(session.submit({{6, empire, 1}, {10}, tactical::MovePayload{at(2000, 0)}})),
            "FT-07: move after autonomous acquisition submits");
        for (int tick = 0; tick < 3; ++tick) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "FT-07: move after autonomous acquisition steps");
        }
        for (const eawr::sim::EntityId id : {11U, 12U}) {
            const auto state = session.combat_state(id);
            expect(state && state->attack_target == eawr::sim::invalid_entity_id && !state->direct,
                "FT-07: removing the formation target clears every member's held target");
        }
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


} // namespace fighter_test_support
