#include "station_upgrade_fixtures.hpp"
#include "eawr/sim/tactical/session.hpp"
#include <optional>
#include <vector>
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/platform/sim_workers.hpp"
#include <algorithm>
#include <array>
#include <string>

namespace station_upgrade_test_support {
bool has_upgrade_tag(const t::TacticalSession& world) {
    const auto bytes = world.canonical_state_bytes();
    const std::array<std::uint8_t, 4> tag{'U', 'P', 'G', 'D'};
    return std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end()) != bytes.end();
}
void test_no_upgrade_layout() {
    auto initial = create(); if (!initial) return;
    expect(!has_upgrade_tag(*initial), "UPGD omitted for lobby tech one before research");
    for (const auto tech : {0U, 1U}) {
        auto rules = economy(); rules.upgrades.clear();
        for (auto& player : rules.players) player.start_tech = tech;
        for (auto& menu : rules.menus) menu.options.resize(1);
        auto ordinary = create(rules); if (!ordinary) return;
        expect(!has_upgrade_tag(*ordinary), "UPGD omitted in ordinary initial state");
        expect(static_cast<bool>(ordinary->submit(buy(0, 1, 0, 1, 10))), "ordinary buy submits");
        through(*ordinary, 21);
        expect(account(*ordinary, 1).lifetime.at(10) == 1 && !has_upgrade_tag(*ordinary),
            "UPGD omitted after ordinary ship completion and lifetime increment");
    }
}
void test_replacement_attack() {
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto slot : {t::attack_hull, 0U, 1U}) {
        auto health = durability();
        health.profiles.back().hardpoints.resize(1); // slot 1 exists only on the old station
        t::CombatTable combat;
        for (const auto type : {10ULL, 40ULL, 41ULL}) {
            t::CombatProfile profile; profile.type_id = type;
            if (type == 40) profile.hardpoints = {{0, {}, true}, {1, {}, true}};
            if (type == 41) profile.hardpoints = {{0, {}, true}};
            combat.profiles.push_back(profile);
        }
        const std::vector<t::SensorProfile> sensors{{10, whole(1000)}, {40, whole(1000)}, {41, whole(1000)}};
        auto made = t::TacticalSession::create(setup(), sensors, health, {}, std::nullopt, combat, {}, {}, economy());
        expect(static_cast<bool>(made), "replacement target fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        world.scramble_storage_for_testing();
        expect(static_cast<bool>(world.submit(buy(0, 1, 0, 1, 91))), "target fixture levelup submits");
        expect(static_cast<bool>(world.submit({{0, 4, 0}, {5}, t::AttackPayload{1, slot}})), "direct station attack submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        eawr::sim::EntityId replacement{};
        for (unsigned tick = 0; tick < 6; ++tick) {
            const auto stepped = world.step(executor); expect(static_cast<bool>(stepped), "target fixture steps"); if (!stepped) return;
            for (const auto& event : stepped.value().snapshot->events()) if (event.kind == t::EventKind::station_replaced) replacement = event.sequence;
            const auto target = replacement == 0 ? 1 : replacement;
            const auto state = world.combat_state(5);
            expect(state && state->direct && state->attack_target == target, "WPR-53: direct target survives replacement and next tick");
            const auto units = world.units();
            const auto attacker = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.entity_id == 5; });
            expect(attacker != units.end() && attacker->order.target == target, "WPR-53: order follows replacement");
            if (replacement != 0) expect(state->attack_hardpoint == (slot == 1 ? t::no_hardpoint : slot),
                "WPR-53: valid hardpoint retained, missing slot falls back to hull");
        }
        expect(replacement != 0, "target fixture actually replaced station");
    }
}
// The commands phase inserts a reinforcement into the staged units, which moves them. An attack
// order later in the same tick reads its target's health and health profile from the staged
// unit: an upgraded unit keeps its profile in the unit itself, so the targeting view's copy of
// that pointer no longer names it (a use after free in has_aim_hardpoint before the fix).
void test_order_after_reinforcement() {
    t::CombatTable combat;
    for (const auto type : {10ULL, 40ULL, 41ULL}) {
        t::CombatProfile profile; profile.type_id = type;
        profile.max_attack_distance = whole(500);
        profile.hardpoints = {{0, {}, true}};
        combat.profiles.push_back(profile);
    }
    t::MotionTable motion;
    motion.rules.arc_degrees = whole(15);
    motion.rules.expansion_distance = whole(300);
    t::MotionProfile ship;
    ship.type_id = 10; ship.max_speed = whole(3); ship.rate_of_turn = whole(2); ship.turn_in_place_slowdown = whole(1);
    ship.acceleration = decimal("0.05"); ship.deceleration = decimal("0.05");
    motion.profiles = {ship};
    auto initial = setup();
    initial.units[4].position = {whole(3000), {}, {}}; // player 4's ship, out of reach
    const std::vector<t::SensorProfile> sensors{{10, whole(20000)}, {40, whole(20000)}, {41, whole(20000)}};
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto made = t::TacticalSession::create(initial, sensors, durability(), motion, std::nullopt, combat, {}, {}, economy());
        expect(static_cast<bool>(made), "reinforcement order fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit(buy(0, 1, 0, 1, 90))), "buy the hull upgrade");
        expect(static_cast<bool>(world.submit(buy(0, 1, 1, 1, 10))), "buy a ship to reinforce");
        expect(static_cast<bool>(world.submit({{22, 1, 2}, {}, t::ReinforcePayload{10, {whole(-2000), {}, {}}}})),
            "reinforce in the attack's tick");
        expect(static_cast<bool>(world.submit({{22, 4, 0}, {5}, t::AttackPayload{3}})), "attack the upgraded ship");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        bool upgraded = false;
        for (unsigned tick = 0; tick < 25; ++tick) {
            const auto stepped = world.step(executor);
            expect(static_cast<bool>(stepped), "reinforcement order fixture steps"); if (!stepped) return;
            if (tick == 21) upgraded = world.durability_state(3) && world.durability_state(3)->hull == decimal("125");
        }
        expect(upgraded, "the target carries its upgrade bonus before the order");
        const auto state = world.combat_state(5);
        expect(state && state->direct && state->attack_target == 3, "the order after a reinforcement takes its target");
        expect(account(world, 1).pool.empty(), "the reinforcement left the pool in the order's tick");
    }
}
void test_destroyed_station() {
    for (const bool ai : {false, true}) {
        auto rules = economy(); rules.players[0].ai = ai;
        auto doomed = create(rules); if (!doomed) return;
        expect(static_cast<bool>(doomed->submit(buy(0, 1, 0, 1, 91))), "doomed levelup submits");
        expect(static_cast<bool>(doomed->submit({{3, 1, 1}, {1}, t::DamagePayload{whole(100000)}})), "station destruction submits");
        const auto events = through(*doomed, 6);
        expect(std::none_of(events.begin(), events.end(), [](const auto& event) { return event.kind == t::EventKind::station_replaced; }),
            "dead station never replaced");
        expect(account(*doomed, 1).queues[0].empty() && account(*doomed, 1).completed.empty(), "dead station drops levelup queue");
        expect(account(*doomed, 1).credits == whole(ai ? 6000 : 5990), "destruction refunds AI only");
    }
}
void test_replacement_projectiles() {
    auto initial = setup();
    for (auto& unit : initial.units) unit.position.x = whole(unit.entity_id == 5 ? -100 : 0);
    t::CombatTable combat;
    t::CombatProfile attacker; attacker.type_id = 10;
    t::WeaponProfile weapon; weapon.range = whole(1000);
    weapon.shot = t::ShotProfile{whole(1), t::no_type_index, whole(1), whole(1000), true, true, {}};
    weapon.shot->homing = true; weapon.shot->turn_rate = whole(10);
    attacker.weapons = {weapon}; combat.profiles.push_back(attacker);
    for (const auto type : {40ULL, 41ULL}) {
        t::CombatProfile target; target.type_id = type; combat.profiles.push_back(target);
    }
    const std::vector<t::SensorProfile> sensors{{10, whole(1000)}, {40, whole(1000)}, {41, whole(1000)}};
    auto made = t::TacticalSession::create(initial, sensors, durability(), {}, std::nullopt, combat, {}, {}, economy());
    expect(static_cast<bool>(made), "projectile replacement fixture creates"); if (!made) return;
    auto world = std::move(made).value();
    expect(static_cast<bool>(world.submit(buy(0, 1, 0, 1, 91))), "projectile fixture levelup submits");
    expect(static_cast<bool>(world.submit({{0, 4, 0}, {5}, t::AttackPayload{1}})), "projectile fixture attack submits");
    through(world, 4);
    const auto before = world.snapshot()->projectiles();
    expect(!before.empty() && before.front().target == 1, "homing shot is in flight before replacement");
    if (before.empty()) return;
    const auto shot_id = before.front().id;
    const auto events = through(world, 7);
    eawr::sim::EntityId replacement{};
    for (const auto& event : events) if (event.kind == t::EventKind::station_replaced) replacement = event.sequence;
    through(world, 6);
    const auto after = world.snapshot()->projectiles();
    const auto retained = std::find_if(after.begin(), after.end(), [=](const auto& shot) { return shot.id == shot_id; });
    expect(replacement != 0 && retained != after.end() && retained->target == replacement && retained->locked,
        "WPR-53: the same homing projectile remains locked on the replacement");
}
void test_hangar_replacement() {
    for (const bool disabled : {false, true}) for (const bool reordered : {false, true}) for (const auto reserve : {-1, 0, 2}) {
      std::vector<std::string> hashes;
      for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto launch_setup = setup();
        std::erase_if(launch_setup.units, [](const auto& unit) { return unit.entity_id == 2; });
        launch_setup.units.front().garrison_enabled = !disabled;
        if (disabled) {
            launch_setup.units.push_back({6, 30, 1, {}, m::identity_quat(), {}});
            launch_setup.units.push_back({7, 20, 1, {}, m::identity_quat(), {}});
            launch_setup.units.push_back({8, 30, 1, {}, m::identity_quat(), {}});
            launch_setup.units.push_back({9, 20, 1, {}, m::identity_quat(), {}});
            launch_setup.squadrons = {{6, {7}}, {8, {9}}};
        }
        auto health = durability(); auto craft_health = health.profiles.front(); craft_health.type_id = 20;
        health.profiles.insert(health.profiles.begin() + 1, craft_health);
        t::MotionTable motion;
        t::CraftProfile craft;
        craft.type_id = 20; craft.max_speed = whole(5); craft.min_speed = whole(1);
        craft.rate_of_turn = whole(6); craft.lift = whole(6); craft.thrust = decimal("0.2");
        craft.roll_rate = whole(6); craft.bank_angle = whole(70); craft.strafe_distance = whole(200);
        motion.squadrons.craft = {craft};
        motion.squadrons.squadrons = {{30, {20}, {m::Vec3{}}, whole(1000), whole(200), whole(300), whole(20)},
            {31, {20}, {m::Vec3{}}, whole(1000), whole(200), whole(300), whole(20)}};
        for (const auto type : {40ULL, 41ULL}) {
            t::SpawnerProfile hangar; hangar.type_id = type; hangar.entries = {{30, 1, reserve}}; hangar.delay_frames = 1;
            if (reordered && type == 41) hangar.entries.insert(hangar.entries.begin(), {31, 0, 0});
            hangar.bays = {{0, {}, {whole(1), {}, {}}}};
            motion.squadrons.spawners.push_back(hangar);
        }
        auto made = t::TacticalSession::create(launch_setup, {}, health, motion, std::nullopt, {}, {}, {}, economy());
        expect(static_cast<bool>(made), "hangar replacement fixture creates"); if (!made) return;
        auto carrier = std::move(made).value();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto advance = [&](std::uint64_t until) {
            std::vector<t::Event> events;
            while (carrier.completed_tick() < until) {
                const auto tick = carrier.completed_tick();
                if (workers != 1) carrier.scramble_storage_for_testing();
                const auto step = carrier.step(executor);
                expect(static_cast<bool>(step), "hangar upgrade steps at every worker count");
                if (!step) break;
                const auto emitted = step.value().snapshot->events();
                events.insert(events.end(), emitted.begin(), emitted.end());
                const auto hash = carrier.state_sha256();
                if (workers == 1) hashes.push_back(hash);
                else expect(hash == hashes[tick], "FL-13: hangar upgrade hashes agree at 1/2/4/8 workers and shuffled storage");
            }
            return events;
        };
        advance(100);
        expect(carrier.squadrons().size() == (disabled ? 2U : 1U), "FL-13: disabled station retains only its free starting set");
        expect(static_cast<bool>(carrier.submit(buy(100, 1, 0, 1, 91))), "hangar levelup submits");
        const auto events = advance(200);
        eawr::sim::EntityId replacement{};
        for (const auto& event : events) if (event.kind == t::EventKind::station_replaced) replacement = event.sequence;
        expect(replacement != 0 && carrier.squadrons().size() == (disabled ? 2U : 1U),
            "FL-13/WPR-56: station replacement adds no fresh squadrons");
        if (disabled) {
            const auto units = carrier.units();
            const auto station = std::find_if(units.begin(), units.end(), [=](const auto& unit) { return unit.entity_id == replacement; });
            expect(station != units.end() && !station->garrison_enabled, "FL-13: replacement preserves the object garrison flag");
            expect(std::ranges::equal(carrier.squadrons(), launch_setup.squadrons),
                "FL-13: the two free starting squadrons retain their IDs and craft");
            expect(static_cast<bool>(carrier.submit({{200, 1, 1}, {7, 9}, t::DamagePayload{whole(10000)}})), "free starting craft destruction submits");
            advance(300);
            expect(carrier.squadrons().empty(), "FL-13: authored station reserves cannot replace free starting squads");
            continue;
        }
        if (carrier.squadrons().empty()) continue;
        const auto squadron = carrier.squadrons().front(); const auto mind = carrier.squadron_state(squadron.container);
        expect(mind && mind->spawner == replacement && mind->entry == (reordered ? 1U : 0U), "WPR-56: entry matched by type");
        expect(static_cast<bool>(carrier.submit({{200, 1, 1}, squadron.members, t::DamagePayload{whole(10000)}})), "retained craft destruction submits");
        advance(300);
        expect(carrier.squadrons().size() == (reserve == 0 ? 0U : 1U), "WPR-56: retained death releases correct entry without replenishing spent reserve");
      }
    }
}
t::MotionTable free_garrison_motion() {
    t::MotionTable motion;
    t::CraftProfile craft;
    craft.type_id = 20; craft.max_speed = whole(5); craft.min_speed = whole(1);
    craft.rate_of_turn = whole(6); craft.lift = whole(6); craft.thrust = decimal("0.2");
    craft.roll_rate = whole(6); craft.bank_angle = whole(70); craft.strafe_distance = whole(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{30, {20}, {m::Vec3{}}, whole(1000), whole(200), whole(300), whole(20)},
        {31, {20}, {m::Vec3{}}, whole(1000), whole(200), whole(300), whole(20)}};
    for (const auto type : {40ULL, 41ULL}) {
        t::SpawnerProfile hangar; hangar.type_id = type; hangar.entries = {{31, 1, -1}};
        hangar.delay_frames = 30; hangar.bays = {{0, {whole(100), {}, {}}, {whole(1), {}, {}}}};
        hangar.starbase = true;
        motion.squadrons.spawners.push_back(hangar);
    }
    return motion;
}
t::DurabilityTable free_garrison_health() {
    auto health = durability();
    auto craft = health.profiles.front(); craft.type_id = 20;
    health.profiles.insert(health.profiles.begin() + 1, craft);
    return health;
}
t::TacticalSetup free_garrison_setup() {
    auto value = setup();
    value.units = {{1, 40, 2, {}, m::identity_quat(), {}},
        {6, 30, 1, {}, m::identity_quat(), {}}, {7, 20, 1, {}, m::identity_quat(), {}},
        {8, 30, 1, {}, m::identity_quat(), {}}, {9, 20, 1, {}, m::identity_quat(), {}}};
    value.units.front().garrison_enabled = false;
    value.squadrons = {{6, {7}}, {8, {9}}};
    value.free_garrisons = {{1, 600, {30, 30}, {7, 9}}};
    return value;
}
void test_garrison_marked_station_upgrade() {
    const auto find_instance = [](const t::TacticalSession& world, eawr::sim::EntityId id) -> const t::TacticalInstance* {
        const auto instances = world.snapshot()->instances();
        const auto found = std::find_if(instances.begin(), instances.end(),
            [id](const auto& instance) { return instance.entity_id == id; });
        return found != instances.end() ? &*found : nullptr;
    };
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup();
        initial.units = {{1, 40, 1, {}, m::identity_quat(), {}},
            {3, 10, 1, {}, m::identity_quat(), {}}};
        initial.units.front().garrison_enabled = false;
        // A synthetic generic free-garrison birth exercises an otherwise stock-unreachable
        // marked station. No test hook writes provenance into the simulation.
        initial.free_garrisons = {{1, 1, {40}, {3}}};
        auto motion = free_garrison_motion();
        for (auto& hangar : motion.squadrons.spawners) {
            hangar.entries.clear(); hangar.delay_frames = 1;
        }
        auto made = t::TacticalSession::create(initial, {}, durability(), motion, std::nullopt, {}, {}, {}, economy());
        expect(static_cast<bool>(made), "WU-37: marked station upgrade fixture creates");
        if (!made) return;
        auto world = std::move(made).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const auto advance = [&]() {
            if (workers != 1) world.scramble_storage_for_testing();
            auto stepped = world.step(executor);
            expect(static_cast<bool>(stepped), "WU-37: marked station upgrade steps");
            if (stepped) hashes.push_back(stepped.value().state_sha256);
            return stepped;
        };
        expect(static_cast<bool>(world.submit({{0, 1, 0}, {3}, t::DamagePayload{whole(10000)}})),
            "WU-37: deplete generic free-garrison original");
        eawr::sim::EntityId marked{};
        for (unsigned frame = 0; frame < 100 && marked == 0; ++frame) {
            if (!advance()) return;
            for (const auto& instance : world.snapshot()->instances())
                if (instance.type_id == 40 && instance.garrison_unit) marked = instance.entity_id;
        }
        const auto* starting = find_instance(world, 1);
        expect(marked != 0 && starting && !starting->garrison_unit,
            "WU-37: generic birth marks the new station while the starting station stays unmarked");
        if (marked == 0) return;
        expect(static_cast<bool>(world.submit(buy(world.completed_tick(), 1, 1, marked, 91))),
            "WU-37: marked station level-up submits");
        eawr::sim::EntityId replacement{};
        for (unsigned frame = 0; frame < 12; ++frame) {
            auto stepped = advance();
            if (!stepped) return;
            for (const auto& event : stepped.value().snapshot->events())
                if (event.kind == t::EventKind::station_replaced && event.unit == marked) replacement = event.sequence;
            if (replacement != 0) {
                const auto* upgraded = find_instance(world, replacement);
                expect(upgraded && upgraded->garrison_unit,
                    "WU-37: replacement retains the station birth mark through subsequent ECS commits");
            }
        }
        const auto* upgraded = find_instance(world, replacement);
        expect(replacement != 0 && upgraded && upgraded->type_id == 41,
            "WU-37: marked station actually upgrades to its replacement type");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WU-37: marked station replacement agrees on 1/2/4/8 workers");
    }
}

void test_free_garrison_replenishment() {
    std::vector<std::string> hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = free_garrison_setup();
        const auto motion = free_garrison_motion();
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), motion,
            std::nullopt, {}, {}, {}, economy());
        expect(static_cast<bool>(made), "FL-14: free garrison fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        bool checked_bay = false;
        const auto advance = [&](const std::uint64_t until) {
            while (world.completed_tick() < until) {
                const auto tick = world.completed_tick();
                if (workers != 1) world.scramble_storage_for_testing();
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "FL-14: depletion/replenishment steps"); if (!step) return;
                const auto hash = world.state_sha256();
                if (workers == 1) hashes.push_back(hash);
                else expect(hash == hashes[tick], "FL-14: every tick agrees on 1/2/4/8 and shuffled storage");
                if (!checked_bay && tick >= 1350 && !world.squadrons().empty()) {
                    const auto squadron = world.squadrons().front();
                    const auto views = world.snapshot()->instances();
                    const auto view = std::find_if(views.begin(), views.end(), [&](const auto& value) {
                        return value.entity_id == squadron.container;
                    });
                    expect(view != views.end() && view->garrison_unit,
                        "WU-37: a free-garrison respawn from a hangar keeps the flag");
                    const auto units = world.units();
                    const auto craft = std::find_if(units.begin(), units.end(), [&](const auto& unit) {
                        return unit.entity_id == squadron.members.front();
                    });
                    expect(craft != units.end() && craft->owner == 1 && craft->position.x == whole(100),
                        "FL-14: allied station launches player-owned craft from its bay");
                    const auto mind = world.squadron_state(squadron.container);
                    expect(mind && mind->spawner == 0 && squadron.container > 9,
                        "FL-14: pending births have new identities and no authored garrison counter");
                    checked_bay = true;
                }
            }
        };
        advance(100);
        expect(static_cast<bool>(world.submit({{100, 1, 0}, {7}, t::DamagePayload{whole(10000)}})), "partial loss submits");
        advance(750);
        expect(world.squadrons().size() == 1 && world.squadrons().front().container == 8,
            "FL-14: partial depletion does not replenish after the faction delay");
        expect(static_cast<bool>(world.submit({{750, 1, 1}, {9}, t::DamagePayload{whole(10000)}})), "full loss submits");
        expect(static_cast<bool>(world.submit(buy(800, 2, 0, 1, 91))), "station upgrades while player waits");
        advance(1350);
        expect(world.squadrons().empty(), "FL-14: full depletion waits all 600 faction-delay frames");
        advance(1420);
        expect(checked_bay && world.squadrons().size() == 2, "FL-14: ordered duplicate templates both launch after the delay");
        expect(world.ledgers()[0].credits == whole(6000) && world.ledgers()[0].pool.empty(),
            "FL-14: free launches spend no credits and use no purchase pool");
        std::vector<eawr::sim::EntityId> members;
        for (const auto& squadron : world.squadrons()) members.insert(members.end(), squadron.members.begin(), squadron.members.end());
        std::sort(members.begin(), members.end());
        expect(static_cast<bool>(world.submit({{1420, 1, 2}, members, t::DamagePayload{whole(10000)}})), "replacement loss submits");
        advance(2020);
        expect(world.squadrons().empty(), "FL-14: replacement craft register for the next full depletion delay");
        advance(2090);
        expect(world.squadrons().size() == 2, "FL-14: another full set replenishes after repeated depletion");
    }
}
void test_free_garrison_timer_order() {
    const t::TacticalSnapshot plain(0, {}, {}, {});
    const t::TacticalSnapshot decorated(0, {}, {}, {}, {}, {}, std::nullopt,
        {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {{1, 600, 615}});
    expect(plain.canonical_bytes() == decorated.canonical_bytes()
        && plain.sha256() == decorated.sha256(),
        "HCB-62: published timer metadata changes no canonical snapshot bytes or hash");
    std::vector<std::string> hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = free_garrison_setup();
        expect(t::initial_spawner(initial.seed, 1, 1).next_service_frame == 15,
            "FL-14: seed 540 pins hangar service at frame 15 plus 30n");
        constexpr std::uint64_t due = 615;
        initial.free_garrisons.front().delay_frames = 600;
        initial.free_garrisons.front().templates = {30};
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion());
        expect(static_cast<bool>(made), "FL-14: coincident timer/hangar fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        const auto initial_snapshot = world.snapshot();
        expect(initial_snapshot->garrison_timers().size() == 1
            && initial_snapshot->garrison_timers().front().player == 1
            && initial_snapshot->garrison_timers().front().duration_frames == 600
            && !initial_snapshot->garrison_timers().front().due_frame,
            "HCB-62: initial publication includes the player's authored delay without a running wait");
        expect(static_cast<bool>(world.submit({{14, 1, 0}, {7, 9}, t::DamagePayload{whole(10000)}})),
            "FL-14: full depletion at frame 15 starts the 600-frame timer");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto advance = [&](const std::uint64_t until) {
            while (world.completed_tick() < until) {
                const auto tick = world.completed_tick();
                if (workers != 1) world.scramble_storage_for_testing();
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "FL-14: timer boundary steps"); if (!step) return false;
                const auto hash = world.state_sha256();
                if (workers == 1) hashes.push_back(hash);
                else expect(hash == hashes[tick], "FL-14: timer boundary every tick agrees on 1/2/4/8");
            }
            return true;
        };
        if (!advance(15)) return;
        const auto waiting_snapshot = world.snapshot();
        expect(waiting_snapshot->garrison_timers().size() == 1
            && waiting_snapshot->garrison_timers().front().due_frame == due
            && !initial_snapshot->garrison_timers().front().due_frame,
            "HCB-62: completed-tick publication copies the committed deadline and preserves older snapshots");
        if (!advance(due)) return;
        expect(!world.snapshot()->garrison_timers().front().due_frame
            && waiting_snapshot->garrison_timers().front().due_frame == due,
            "HCB-62: expiry removes the published wait without mutating a retained snapshot");
        expect(world.squadrons().empty(), "FL-14: frame 615 timer maturation cannot feed the same-frame hangar pass");
        if (!advance(due + 29)) return;
        expect(world.squadrons().empty(), "FL-14: matured queue remains unlaunched through frame 644");
        if (!advance(due + 30)) return;
        expect(world.squadrons().size() == 1, "FL-14: first replacement launches exactly at frame 645");
    }
}
void test_free_garrison_pending_and_generic() {
    {
        auto initial = free_garrison_setup();
        initial.units.insert(initial.units.begin() + 1, {2, 41, 1, {}, m::identity_quat(), {}});
        auto motion = free_garrison_motion();
        motion.squadrons.spawners.back().starbase = false;
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), motion);
        expect(static_cast<bool>(made), "FL-14: carrier-loss fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        through(world, 100);
        std::vector<eawr::sim::EntityId> carrier_craft;
        for (const auto& squadron : world.squadrons()) {
            const auto mind = world.squadron_state(squadron.container);
            if (mind && mind->spawner == 2)
                carrier_craft.insert(carrier_craft.end(), squadron.members.begin(), squadron.members.end());
        }
        expect(!carrier_craft.empty(), "FL-14: an authored carrier squadron launches normally");
        if (!carrier_craft.empty()) expect(static_cast<bool>(world.submit(
            {{100, 1, 0}, carrier_craft, t::DamagePayload{whole(10000)}})), "authored carrier loss submits");
        through(world, 800);
        const auto units = world.units();
        expect(std::count_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 30; }) == 2,
            "FL-14: authored carrier losses do not replenish the player's free starting force");
    }
    {
        auto initial = free_garrison_setup(); initial.free_garrisons.front().delay_frames = 60;
        initial.units.front().owner = 4;
        initial.players.back().team_id = 0;
        initial.players.back().faction_id = 999;
        t::CombatTable relationships; relationships.pad_neutral_factions = {999};
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion(),
            std::nullopt, relationships);
        expect(static_cast<bool>(made), "FL-14: neutral same-team station fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit({{0, 1, 0}, {7, 9}, t::DamagePayload{whole(10000)}})),
            "neutral-station depletion submits");
        through(world, 100);
        expect(world.squadrons().empty(), "FL-14: neutral ownership cannot claim an allied free-force queue");
        t::UnitState station{0, 41, 3, {}, m::identity_quat(), {}}; station.garrison_enabled = false;
        expect(static_cast<bool>(world.stage_spawn(station)), "a real allied station enters beside the neutral station");
        through(world, 200);
        expect(world.squadrons().size() == 2, "FL-14: the real allied station consumes the retained pending queue");
    }
    for (const bool generic : {false, true}) for (const bool quit : {false, true}) {
        auto initial = free_garrison_setup(); initial.free_garrisons.front().delay_frames = 60;
        if (generic) initial.free_garrisons.front().templates = {10};
        auto motion = free_garrison_motion();
        motion.squadrons.spawners.front().starbase = false; // a carrier cannot claim a player pending queue
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), motion);
        expect(static_cast<bool>(made), "FL-14: pending queue fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit({{0, 1, 0}, {7, 9}, t::DamagePayload{whole(10000)}})), "pending fixture depletion submits");
        if (quit) expect(static_cast<bool>(world.submit({{1, 1, 1}, {}, t::QuitPayload{}})), "pending player quits");
        through(world, 100);
        expect(world.squadrons().empty(), "FL-14: non-starbase cannot launch pending free forces");
        t::UnitState station{0, 41, 3, {}, m::identity_quat(), {}}; station.garrison_enabled = false;
        const auto born = world.stage_spawn(station);
        expect(static_cast<bool>(born), "an allied eligible station arrives after the timer");
        through(world, 200);
        const auto units = world.units();
        const auto created = std::count_if(units.begin(), units.end(), [](const auto& unit) {
            return unit.owner == 1 && (unit.type_id == 10 || unit.type_id == 20);
        });
        expect(created == (quit ? 0 : generic ? 1 : 2), "FL-14: pending queue survives missing eligibility; quit players cannot replenish");
        if (generic && !quit) {
            expect(world.squadrons().empty(), "FL-14: a generic free company creates one actual object without a team container");
            const auto ship = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 10; });
            if (ship != units.end()) expect(static_cast<bool>(world.submit({{200, 1, 1}, {ship->entity_id}, t::DamagePayload{whole(10000)}})),
                "generic free object loss submits");
            through(world, 300);
            const auto after = world.units();
            expect(std::count_if(after.begin(), after.end(), [](const auto& unit) { return unit.type_id == 10; }) == 1,
                "FL-14: a generic replacement is registered and replenishes again");
        }
    }
}
void test_free_garrison_bays_and_claims() {
    std::vector<std::string> hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = free_garrison_setup();
        auto second = initial.units.front(); second.entity_id = 2; second.owner = 3;
        initial.units.insert(initial.units.begin() + 1, second);
        initial.units[4].owner = initial.units[5].owner = 2;
        initial.free_garrisons = {{1, 0, {30}, {7}}, {2, 0, {31}, {9}}};
        for (initial.seed = 0; initial.seed < 4096; ++initial.seed) {
            if (t::initial_spawner(initial.seed, 1, 1).next_service_frame
                == t::initial_spawner(initial.seed, 1, 2).next_service_frame) break;
        }
        expect(initial.seed < 4096, "FL-14: competing stations share a service frame");
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion());
        expect(static_cast<bool>(made), "FL-14: competing stations fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit({{0, 1, 0}, {7}, t::DamagePayload{whole(10000)}})), "first allied player depleted");
        expect(static_cast<bool>(world.submit({{0, 2, 0}, {9}, t::DamagePayload{whole(10000)}})), "second allied player depleted");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        while (world.completed_tick() < 100) {
            const auto tick = world.completed_tick();
            if (workers != 1) world.scramble_storage_for_testing();
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "FL-14: concurrent allied queue claims step"); if (!step) return;
            const auto hash = world.state_sha256();
            if (workers == 1) hashes.push_back(hash);
            else expect(hash == hashes[tick], "FL-14: overlapping claims agree across all worker counts");
        }
        expect(world.squadrons().size() == 2, "FL-14: two stations claim each pending company exactly once");
        const auto units = world.units();
        expect(std::count_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 30 && unit.owner == 1; }) == 1
            && std::count_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 31 && unit.owner == 2; }) == 1,
            "FL-14: shared station service preserves each pending player's template and ownership");
    }
    auto initial = free_garrison_setup(); initial.free_garrisons.front().delay_frames = 60;
    auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion());
    expect(static_cast<bool>(made), "FL-14: destroyed bay fixture creates"); if (!made) return;
    auto world = std::move(made).value();
    expect(static_cast<bool>(world.submit({{0, 1, 0}, {7, 9}, t::DamagePayload{whole(10000)}})), "bay fixture depletes free craft");
    expect(static_cast<bool>(world.submit({{0, 2, 0}, {1}, t::DamagePayload{whole(200), 0}})), "bay fixture destroys the launch hardpoint");
    through(world, 100);
    const auto health = world.durability_state(1);
    expect(health && health->hardpoints[0] == Fixed{} && world.squadrons().empty(),
        "FL-14: a destroyed fighter bay holds matured pending forces");
    t::UnitState station{0, 41, 3, {}, m::identity_quat(), {}}; station.garrison_enabled = false;
    expect(static_cast<bool>(world.stage_spawn(station)), "another allied intact bay enters");
    through(world, 200);
    expect(world.squadrons().size() == 2, "FL-14: an intact allied bay consumes the held pending queue");

    std::vector<std::string> restored_hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto restored_setup = free_garrison_setup();
        restored_setup.free_garrisons.front().delay_frames = 60;
        auto restored = t::TacticalSession::create(restored_setup, {}, free_garrison_health(), free_garrison_motion(),
            std::nullopt, {}, {}, {}, economy());
        expect(static_cast<bool>(restored), "FL-03: disabled upgraded bay fixture creates");
        if (!restored) return;
        auto restored_world = std::move(restored).value();
        expect(static_cast<bool>(restored_world.submit({{0, 1, 0}, {7, 9}, t::DamagePayload{whole(10000)}})),
            "FL-14: upgraded bay fixture depletes free craft");
        expect(static_cast<bool>(restored_world.submit({{0, 2, 0}, {1}, t::DamagePayload{whole(200), 0}})),
            "FL-03: upgraded bay fixture destroys its only bay");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto advance = [&](const auto until) {
            while (restored_world.completed_tick() < until) {
                const auto tick = restored_world.completed_tick();
                if (workers != 1) restored_world.scramble_storage_for_testing();
                const auto step = restored_world.step(executor);
                expect(static_cast<bool>(step), "FL-03: disabled upgraded bay steps");
                if (!step) return false;
                const auto hash = restored_world.state_sha256();
                if (workers == 1) restored_hashes.push_back(hash);
                else expect(hash == restored_hashes[tick], "FL-03: disabled bay launches agree across workers");
            }
            return true;
        };
        if (!advance(100U)) return;
        const auto destroyed = restored_world.durability_state(1);
        expect(destroyed && destroyed->hardpoints[0] == Fixed{} && restored_world.squadrons().empty(),
            "FL-03: destroyed bay cannot consume the matured pending queue");
        expect(static_cast<bool>(restored_world.submit(buy(100, 2, 1, 1, 91))),
            "FL-03: upgrade restores the destroyed bay");
        if (!advance(107U)) return; // WSL-31: include the latest first eligible ability service.
        const auto units = restored_world.units();
        const auto replacement = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 41; });
        expect(replacement != units.end(), "FL-03: upgraded station exists");
        if (replacement == units.end()) return;
        const auto disabled_bay = restored_world.durability_state(replacement->entity_id);
        expect(disabled_bay && disabled_bay->hardpoints[0] == decimal("0.1") && t::hardpoint_disabled(*disabled_bay, 0),
            "FL-03/WPR-52: replacement bay is disabled at positive 0.1 health");
        if (!advance(200U)) return;
        expect(restored_world.squadrons().size() == 2,
            "FL-03/14: disabled restored bay launches both pending free companies");
    }
}
void test_disabled_authored_bay() {
    std::vector<std::string> hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = free_garrison_setup();
        initial.units = {{1, 40, 2, {}, m::identity_quat(), {}}};
        initial.squadrons.clear();
        initial.free_garrisons.clear();
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion(),
            {}, {}, {}, {}, economy());
        expect(static_cast<bool>(made), "FL-03: disabled authored bay fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit({{0, 2, 0}, {1}, t::DamagePayload{whole(200), 0}})),
            "FL-03: authored bay is destroyed before service");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto advance = [&](const std::uint64_t until) {
            while (world.completed_tick() < until) {
                const auto tick = world.completed_tick();
                if (workers != 1) world.scramble_storage_for_testing();
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "FL-03: disabled authored bay steps"); if (!step) return false;
                const auto hash = world.state_sha256();
                if (workers == 1) hashes.push_back(hash);
                else expect(hash == hashes[tick], "FL-03: disabled authored bay every tick agrees on 1/2/4/8");
            }
            return true;
        };
        if (!advance(100)) return;
        expect(world.squadrons().empty(), "FL-03: destroyed authored bay cannot launch");
        expect(static_cast<bool>(world.submit(buy(100, 2, 1, 1, 91))), "FL-03: authored bay upgrade submits");
        if (!advance(107)) return; // WSL-31: include the latest first eligible ability service.
        const auto units = world.units();
        const auto station = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 41; });
        expect(station != units.end(), "FL-03: authored bay upgrade replaces station");
        if (station == units.end()) return;
        const auto health = world.durability_state(station->entity_id);
        expect(health && health->hardpoints[0] == decimal("0.1") && t::hardpoint_disabled(*health, 0),
            "FL-03: upgraded authored bay survives disabled at 0.1 health");
        if (!advance(200)) return;
        expect(!world.squadrons().empty(), "FL-03: disabled surviving authored bay launches a company");
    }
}
void test_free_garrison_replay() {
    auto initial = free_garrison_setup(); initial.queue_identities = true;
    const auto snapshot = t::TacticalSession::create(initial);
    expect(snapshot && snapshot.value().units().size() == initial.units.size(),
        "FL-14: tableless tick-zero snapshots retain free-garrison bindings without requiring creation tables");
    const auto health_only = t::TacticalSession::create(initial, {}, free_garrison_health());
    expect(health_only && health_only.value().durability_state(7).has_value(),
        "FL-14: health-only worlds retain the free roster without requiring an unbound hangar creation table");
    const auto bytes = t::write_replay({initial, 0, {}});
    const auto parsed = bytes ? t::parse_replay(bytes.value()) : eawr::core::Result<t::TacticalReplay>::failure(bytes.error());
    expect(parsed && parsed.value().setup == initial, "FL-14: mixed GSPN/QIDS/GARR replay roundtrips");
    if (parsed) expect(t::write_replay(parsed.value()).value() == bytes.value(), "FL-14: pending setup has one wire encoding");
    if (bytes) {
        std::size_t tag = t::replay_header_size + 4;
        const auto u16 = [&](std::size_t offset) {
            return static_cast<unsigned>(bytes.value()[offset])
                | (static_cast<unsigned>(bytes.value()[offset + 1]) << 8);
        };
        while (u16(tag) != t::replay_extension_free_garrison) tag += 4 + u16(tag + 2);
        const auto body = tag + 4;
        const auto corrupt = [&](std::size_t offset, std::uint32_t value) {
            auto malformed = bytes.value();
            for (unsigned i = 0; i < 4; ++i)
                malformed[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
            expect(!t::parse_replay(malformed), "FL-14: malformed GARR version/count/owner/object is rejected");
        };
        corrupt(body, 2); // unsupported version
        corrupt(body + 4, 0); corrupt(body + 4, 65); // empty/oversized player table
        corrupt(body + 8, 9); // undeclared owner
        corrupt(body + 16, 0); corrupt(body + 16, 1025); // invalid template count
        corrupt(body + 20, 0xffffffffU); // object count exceeds bounded record
        corrupt(body + 40, 6); // team container is not an actual registered object
        for (const auto length : {0U, 7U, 23U, 57U}) {
            auto malformed = bytes.value();
            malformed[tag + 2] = static_cast<std::uint8_t>(length);
            malformed[tag + 3] = 0;
            expect(!t::parse_replay(malformed), "FL-14: partial or trailing GARR record is rejected");
        }
        auto truncated = bytes.value(); truncated.resize(body + 55);
        expect(!t::parse_replay(truncated), "FL-14: truncated GARR body is rejected");
    }
    for (const auto kind : {0, 1, 2, 3, 4, 5}) {
        auto malformed = initial;
        if (kind == 0) malformed.free_garrisons.front().player = 9;
        if (kind == 1) malformed.free_garrisons.front().registered = {9, 7};
        if (kind == 2) malformed.free_garrisons.front().registered = {7, 7};
        if (kind == 3) malformed.free_garrisons.front().registered = {6};
        if (kind == 4) malformed.free_garrisons.front().registered = {1};
        if (kind == 5) malformed.free_garrisons.front().templates = {};
        expect(!t::write_replay({malformed, 0, {}}), "FL-14: invalid free player/object/template binding is rejected");
    }
    auto unknown = initial; unknown.free_garrisons.front().templates = {999};
    expect(!t::TacticalSession::create(unknown, {}, free_garrison_health(), free_garrison_motion()),
        "FL-14: a replay cannot bind an unknown creation template");
    auto different = initial; ++different.free_garrisons.front().delay_frames;
    const auto first = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion());
    const auto second = t::TacticalSession::create(different, {}, free_garrison_health(), free_garrison_motion());
    expect(first && second && first.value().state_sha256() != second.value().state_sha256(),
        "FL-14: waiting/configuration state affects canonical hashes");
    auto oversized = initial;
    for (eawr::sim::EntityId id = 10; id < 8200; ++id) {
        oversized.units.push_back({id, 20, 1, {}, m::identity_quat(), {}});
        oversized.free_garrisons.front().registered.push_back(id);
    }
    expect(!t::write_replay({oversized, 0, {}}), "FL-14: combined GARR header cannot wrap uint16");
}
void test_garrison_replay_extension() {
    auto initial = setup();
    const auto legacy = t::write_replay({initial, 0, {}});
    expect(legacy && legacy.value().size() == t::replay_header_size + 24U * initial.players.size()
        + 80U * initial.units.size(), "FL-13: all-enabled setup retains the legacy wire layout");
    const auto enabled = t::TacticalSession::create(initial);
    initial.units[0].garrison_enabled = initial.units[1].garrison_enabled = false;
    const auto disabled = t::TacticalSession::create(initial);
    expect(enabled && disabled && enabled.value().state_sha256() != disabled.value().state_sha256(),
        "FL-13: the object flag binds canonical state");
    for (const bool squadrons : {false, true}) {
        auto recorded = initial;
        if (squadrons) {
            recorded.units[3].owner = 1;
            recorded.squadrons = {{3, {4}}};
        }
        const auto bytes = t::write_replay({recorded, 0, {}});
        expect(static_cast<bool>(bytes), "GSPN replay writes");
        if (!bytes) return;
        expect(t::peek_replay_format_version(bytes.value()) == (squadrons ? 5U : 4U),
            "GSPN uses tagged format with or without squadrons");
        const auto parsed = t::parse_replay(bytes.value());
        expect(parsed && parsed.value().setup == recorded, "GSPN round-trips disabled entity IDs");
        if (parsed) {
            const auto rewritten = t::write_replay(parsed.value());
            expect(rewritten && rewritten.value() == bytes.value(), "GSPN has one canonical wire encoding");
        }
        for (const auto id : {0U, 2U, 9U}) {
            auto malformed = bytes.value();
            for (std::size_t offset = 0; offset < 8; ++offset) malformed[112 + offset] = 0;
            malformed[112] = static_cast<std::uint8_t>(id);
            expect(!t::parse_replay(malformed), "GSPN rejects zero, duplicate and absent IDs");
        }
        auto unordered = bytes.value();
        unordered[112] = 2; unordered[120] = 1;
        expect(!t::parse_replay(unordered), "GSPN rejects descending IDs");
        for (const auto length : {0U, 7U, 24U}) {
            auto malformed = bytes.value();
            malformed[110] = static_cast<std::uint8_t>(length);
            expect(!t::parse_replay(malformed), "GSPN rejects empty, partial and oversized bodies");
        }
        auto truncated = bytes.value(); truncated.resize(119);
        expect(!t::parse_replay(truncated), "GSPN rejects truncated header data");
    }
    auto combined = initial;
    combined.queue_identities = true;
    const auto combined_bytes = t::write_replay({combined, 0, {}});
    expect(static_cast<bool>(combined_bytes), "GSPN and QIDS write together");
    if (combined_bytes) {
        const auto parsed = t::parse_replay(combined_bytes.value());
        expect(parsed && parsed.value().setup == combined, "GSPN and QIDS round-trip together");
        if (parsed) {
            const auto rewritten = t::write_replay(parsed.value());
            expect(rewritten && rewritten.value() == combined_bytes.value(),
                "GSPN precedes QIDS in one canonical encoding");
        }
    }
    initial.units.clear();
    for (eawr::sim::EntityId id = 1; id <= 8200; ++id) {
        initial.units.push_back({id, 40, 1, {}, m::identity_quat(), {}});
        initial.units.back().garrison_enabled = false;
    }
    expect(!t::write_replay({initial, 0, {}}), "GSPN header size cannot wrap uint16");
}

} // namespace station_upgrade_test_support
