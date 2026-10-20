#include "ability_fixtures.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"
#include "../../src/sim/tactical/blast_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ability_tests {



void test_corrupt_scatter() {
    namespace detail = tactical::detail;
    const std::vector<tactical::Player> owners{{1, 1, 1, tactical::player_flag_commandable},
        {2, 2, 2, tactical::player_flag_commandable}};
    const std::vector<tactical::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}};
    for (const auto weapon_kind : {tactical::object_weapon, 0U}) {
        tactical::CombatProfile shooter, target;
        shooter.category_bits = target.category_bits = 1;
        tactical::WeaponProfile weapon;
        weapon.hardpoint = weapon_kind; weapon.range = units(1000); weapon.pulse_count = 1;
        weapon.cone_width = weapon.cone_height = units(360);
        tactical::ShotProfile shot;
        shot.speed = units(10); shot.max_travel = units(1000); shot.inaccuracy = {{1, units(10)}};
        weapon.shot = shot; shooter.weapons = {weapon};
        tactical::CombatState state;
        state.attack_target = 2; state.direct = true; state.weapons.resize(1); state.weapons[0].pulses_left = 1;
        detail::CombatWorld world;
        world.players = owners; world.relationships = relationships; world.seed = 777; world.frame = 100;
        detail::CombatUnit self, enemy;
        self.id = 1; self.owner = self.team = 1; self.profile = &shooter; self.combat = &state;
        self.transform = math::identity_matrix();
        enemy.id = 2; enemy.owner = enemy.team = 2; enemy.visible_to = 1; enemy.profile = &target;
        const auto fire = [&](std::int64_t distance, std::optional<Fixed> radius) {
            enemy.position = enemy.previous_position = at(distance, 0);
            enemy.transform = math::to_matrix(math::identity_quat(), enemy.position).value();
            self.corrupt_inaccuracy = radius;
            world.units = {self, enemy};
            const auto step = detail::step_combat(world, world.units.front());
            expect(step && step.value().events.size() == 1, "WHE-66: both weapon routes fire a corruption test shot");
            if (!step || step.value().events.empty()) return math::Vec3{};
            const auto aim = step.value().events.front().aim;
            return math::Vec3{Fixed::from_raw(aim.x.raw() - enemy.position.x.raw()), aim.y, aim.z};
        };
        const auto near = fire(200, units(500)), far = fire(400, units(500));
        expect(near == far && near != math::Vec3{}, "WHE-66: corrupt scatter is nonzero and independent of target range");
        expect(std::abs(near.x.raw()) <= units(500).raw() && std::abs(near.y.raw()) <= units(500).raw()
            && std::abs(near.z.raw()) <= units(500).raw(), "WHE-66: all three offsets obey the authored radius");
        expect(fire(200, Fixed{}) == fire(200, std::nullopt), "WHE-66: zero corruption radius retains ordinary scatter");
    }
}


void test_corrupt_systems() {
    using Kind = tactical::AbilityKind;
    expect(tactical::ability_kind("corrupt_systems") == Kind::corrupt_systems
        && static_cast<unsigned>(Kind::corrupt_systems) == 22U, "WHE-33: corrupt systems binds its reserved kind");
    auto health = durability();
    for (auto& profile : health.profiles) profile.shield_refresh = {};
    auto movement = motion();
    auto edge_motion = *movement.find(frigate_type);
    edge_motion.type_id = 14;
    movement.profiles.push_back(edge_motion);
    std::sort(movement.profiles.begin(), movement.profiles.end(), [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    tactical::CombatTable combat;
    for (const auto type : {corvette_type, frigate_type, plain_type}) {
        tactical::CombatProfile profile;
        profile.type_id = type;
        profile.category_bits = 1;
        combat.profiles.push_back(profile);
    }
    tactical::CombatProfile edge;
    edge.type_id = 14; edge.category_bits = 1;
    edge.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
    combat.profiles.push_back(edge);
    tactical::AbilityProfile cast;
    cast.kind = Kind::corrupt_systems;
    cast.recharge_frames = 1800;
    cast.recharge_at_activation = true;
    cast.effective_radius = units(400);
    cast.gui_activated_ability_name = "area";
    tactical::SpecialAbilityProfile handler;
    handler.name = "area";
    handler.kind = tactical::SpecialAbilityKind::corrupt_systems;
    handler.style = tactical::SpecialActivationStyle::user_input;
    handler.corrupt_min_range = units(20);
    handler.corrupt_max_range = units(350);
    handler.corrupt_drain_per_frame = units(2);
    handler.corrupt_inaccuracy = units(500);
    handler.corrupt_duration_frames = 360;
    tactical::AbilityTable table;
    table.profiles = {{corvette_type, {cast}, false, {handler}}, {frigate_type, {turbo(), defend()}, false}};
    table.humans = {1, 2};
    auto invalid = table;
    invalid.profiles[0].special[0].corrupt_duration_frames = 0;
    expect(!tactical::validate_abilities(invalid), "WHE-34: zero status duration is refused");
    auto scene = setup();
    scene.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
        {2, frigate_type, 1, at(100, 0), math::identity_quat(), {}},
        {3, frigate_type, 2, at(100, 0), math::identity_quat(), {}},
        {4, plain_type, 2, at(100, 0), math::identity_quat(), {}},
        {5, 14, 2, at(501, 0), math::identity_quat(), {}},
        {6, frigate_type, 2, at(500, 0), math::identity_quat(), {}},
        {7, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
        {8, frigate_type, 2, at(100, 0, 1000), math::identity_quat(), {}}};
    for (std::uint64_t id = 9; id < 209; ++id)
        scene.units.push_back({id, frigate_type, 2, at(10000, static_cast<std::int64_t>(id)), math::identity_quat(), {}});
    tactical::AbilityPayload payload{Kind::corrupt_systems, tactical::AbilityAction::activate};
    payload.position = at(100, 0);
    auto minimum = payload; minimum.position = at(20, 0);
    auto beyond = payload; beyond.position = at(351, 0);
    std::vector<tactical::PlayerCommand> commands{
        command(0, 1, 0, 7, minimum), command(0, 1, 1, 7, beyond),
        command(0, 2, 0, 3, tactical::AbilityPayload{Kind::turbo, tactical::AbilityAction::activate}),
        command(1, 1, 2, 1, payload),
        command(2, 1, 3, 1, payload),
        command(2, 2, 1, 3, tactical::AbilityPayload{Kind::defend, tactical::AbilityAction::activate}),
        command(10, 1, 4, 7, payload)};
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(scene, {}, health, movement, std::nullopt, combat, {}, table);
        expect(static_cast<bool>(created), "WHE-33: area-status session creates");
        if (!created) return;
        auto session = std::move(created).value();
        for (const auto& entry : commands) expect(static_cast<bool>(session.submit(entry)), "WHE-33: valid wire payload is admitted");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick <= 371; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "WHE-33: area-status tick succeeds");
            if (!stepped) return;
            hashes.push_back(stepped.value().state_sha256);
            const auto instances = session.snapshot()->instances();
            const auto recipient = std::find_if(instances.begin(), instances.end(), [](const auto& instance) { return instance.entity_id == 3; });
            expect(recipient != instances.end(), "WHE-34: drain does not destroy its recipient");
            if (tick == 1) {
                expect(recipient->corrupt && recipient->corrupt->expires_tick == 361 && recipient->corrupt->inaccuracy == units(500),
                    "WHE-33: authored duration and accuracy are copied once");
                for (const auto& instance : instances) {
                    const bool selected = instance.entity_id == 3 || instance.entity_id == 5 || instance.entity_id == 6 || instance.entity_id == 8;
                    expect(instance.corrupt.has_value() == selected, "WHE-33: enemy locomotor and closed radius gates exclude other units");
                }
                const auto work = session.tick_work();
                expect(work.corrupt_queries == 1 && work.corrupt_recipients == 4 && work.corrupt_candidates < 10,
                    "WHE-33: remote fleet size does not expand the local recipient scan");
                expect(session.ability_state(1)->slots[0].ready_tick == 1801 && !session.ability_state(1)->slots[0].active,
                    "WHE-33: instant cast starts recharge without an active toggle");
            }
            if (tick == 2) {
                expect(session.durability_state(3)->shields == units(698), "WHE-34: 60 per second drains two shields each frame");
                const auto state = session.ability_state(3);
                expect(state->slots[0].active && !state->slots[1].active && !recipient->abilities[1].ready,
                    "WHE-65: existing mode remains active but a new mode cannot activate");
            }
            if (tick == 10) expect(recipient->corrupt && recipient->corrupt->started_tick == 10 && recipient->corrupt->expires_tick == 370,
                "WHE-33: second source overwrites one recipient effect and refreshes its lifetime");
            if (tick == 11) expect(session.durability_state(3)->shields == units(680), "WHE-34: refresh never doubles the drain");
            if (tick == 369) expect(recipient->corrupt.has_value(), "WHE-34: effect survives until its final service");
            if (tick == 370) {
                expect(!recipient->corrupt && session.durability_state(3)->shields == Fixed{}, "WHE-34: drain clamps at zero and status expires");
                const auto state = session.ability_state(3);
                expect(state->slots[0].active && state->slots[0].ready_tick == 0 && state->slots[1].ready_tick == 2170,
                    "WHE-34: only a recharged slot starts full recharge; an expiration countdown is preserved");
            }
        }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WHE-33/34: state hashes match at every worker count");
        const auto recorded = session.record();
        const auto bytes = tactical::write_replay(recorded);
        const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == recorded, "WHE-33: point and kind round-trip through replay");
        if (parsed && workers == 1) {
            auto playback = tactical::TacticalSession::from_replay(parsed.value(), {}, health, movement,
                std::nullopt, combat, {}, table);
            expect(playback.has_value(), "WHE-33: corruption replay creates");
            if (playback) {
                std::vector<std::string> replay_hashes;
                while (playback.value().completed_tick() < recorded.final_tick_count) {
                    const auto replay_tick = playback.value().step(executor);
                    if (!replay_tick) { expect(false, "WHE-33: corruption replay steps"); break; }
                    replay_hashes.push_back(replay_tick.value().state_sha256);
                }
                expect(replay_hashes == hashes, "WHE-33/34: replay reproduces activation, refresh and expiry hashes");
            }
        }
    }
}


// WLU-01..WLU-07: LURE. A pulse orders each hostile team with a listed craft type in reach of
// a luring craft to attack that craft; unlisted, distant and friendly teams are left alone; the
// last craft to pulse wins; pulses repeat every pulse period while on; expiry stops the lurers.
void test_lure() {
    constexpr tactical::TypeId awing = 40, tie = 41, ywing = 42, awing_team = 43, tie_team = 44, ywing_team = 45;
    tactical::LureProfile cadence;
    cadence.pulse_frames = 15;
    cadence.max_pulses = 40;
    expect(tactical::lure_pulses(cadence, 2, 2) && tactical::lure_pulses(cadence, 2, 17) && !tactical::lure_pulses(cadence, 2, 3)
            && tactical::lure_pulses(cadence, 2, 2 + 15 * 39) && !tactical::lure_pulses(cadence, 2, 2 + 15 * 40)
            && !tactical::lure_pulses(cadence, 2, 1),
        "WLU-03: a pulse at the switch-on, then every period, at most the authored count");
    cadence.pulse_frames = 0;
    cadence.max_pulses = 3;
    expect(tactical::lure_pulses(cadence, 5, 7) && !tactical::lure_pulses(cadence, 5, 8), "WLU-03: a zero period pulses every frame");

    tactical::AbilityProfile lure;
    lure.kind = tactical::AbilityKind::lure;
    lure.expiration_frames = 480;
    lure.recharge_frames = 1350;
    lure.modifiers.take_damage = decimal("0.7");
    lure.lure = tactical::LureProfile{units(350), 15, 40, {tie}, "Lure_Ability_Particle"};
    tactical::AbilityTable table;
    table.profiles = {unit_of(awing, lure)};
    expect(static_cast<bool>(tactical::validate_abilities(table)), "WLU-01: a craft's lure profile validates");
    auto broken = table;
    broken.profiles[0].abilities[0].lure->target_types = {tie, tie};
    expect(!tactical::validate_abilities(broken), "WLU-01: target types are sorted and unique");
    broken = table;
    broken.profiles[0].abilities[0].lure.reset();
    expect(!tactical::validate_abilities(broken), "WLU-01: LURE needs its pulse data");
    {
        tactical::AbilityState state = tactical::initial_abilities(table.profiles[0]);
        static_cast<void>(tactical::activate_ability(lure, state.slots[0], open_gate, 0));
        expect(tactical::ability_multiplier(table.profiles[0], state, tactical::AbilityModifier::take_damage) == decimal("0.7"),
            "WLU-08: the lurer takes 0.7 of the damage while on");
    }

    tactical::MotionTable motion;
    for (const auto type : {awing, tie, ywing}) {
        tactical::CraftProfile craft;
        craft.type_id = type;
        craft.max_speed = decimal("5.2");
        craft.min_speed = decimal("3.5");
        craft.rate_of_turn = units(4);
        craft.lift = units(4);
        craft.thrust = units(1);
        craft.roll_rate = units(5);
        craft.bank_angle = units(50);
        craft.strafe_distance = units(200);
        motion.squadrons.craft.push_back(craft);
    }
    motion.squadrons.squadrons = {
        {awing_team, {awing, awing}, {at(0, 0), at(-10, 10)}, units(1000), units(200), units(300), units(25)},
        {tie_team, {tie, tie}, {at(0, 0), at(-10, 10)}, units(1000), units(200), units(300), units(25)},
        {ywing_team, {ywing}, {at(0, 0)}, units(1000), units(200), units(300), units(25)}};
    tactical::TacticalSetup initial;
    initial.seed = 1876;
    initial.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    const auto unit = [](const eawr::sim::EntityId id, const tactical::TypeId type, const tactical::PlayerId owner,
                          const math::Vec3 position) {
        return tactical::UnitState{id, type, owner, position, math::identity_quat(), {}};
    };
    initial.units = {unit(10, awing_team, 1, at(0, 0)), unit(11, awing, 1, at(0, 0)), unit(12, awing, 1, at(-10, 10)),
        unit(20, tie_team, 2, at(200, 0)), unit(21, tie, 2, at(200, 0)), unit(22, tie, 2, at(190, 10)),
        unit(30, ywing_team, 2, at(200, 60)), unit(31, ywing, 2, at(200, 60)),
        unit(40, tie_team, 2, at(2000, 0)), unit(41, tie, 2, at(2000, 0)), unit(42, tie, 2, at(1990, 10)),
        unit(50, tie_team, 1, at(100, 100)), unit(51, tie, 1, at(100, 100)), unit(52, tie, 1, at(90, 110))};
    initial.squadrons = {{10, {11, 12}}, {20, {21, 22}}, {30, {31}}, {40, {41, 42}}, {50, {51, 52}}};
    const std::vector<tactical::PlayerCommand> orders = {
        command(0, 1, 0, 10, tactical::MovePayload{at(-400, 0)}),
        command(2, 1, 1, 10, tactical::AbilityPayload{tactical::AbilityKind::lure, tactical::AbilityAction::activate}),
        command(5, 2, 0, 20, tactical::MovePayload{at(3000, 0)}),
        command(490, 2, 1, 20, tactical::MovePayload{at(3000, 0)}),
    };
    const auto order_of = [](const tactical::TacticalSession& session, const eawr::sim::EntityId id) {
        for (const auto& state : session.units()) if (state.entity_id == id) return state.order;
        return tactical::Order{};
    };
    const auto lured_by = [&](const tactical::TacticalSession& session, const eawr::sim::EntityId id, const eawr::sim::EntityId craft) {
        const auto order = order_of(session, id);
        return order.kind == tactical::OrderKind::attack && order.target == craft;
    };
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(initial, {}, {}, motion, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "WLU-01: lure session is valid");
        if (!created) continue;
        auto session = std::move(created).value();
        for (const auto& order : orders) expect(static_cast<bool>(session.submit(order)), "lure command is recordable");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < 520; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "lure session step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto a = session.ability_state(11), b = session.ability_state(12);
            // WAB-12: the countdown ends the slot 479 or 480 frames after the switch.
            if (tick < 2 + 479 || tick >= 2 + 480) {
                const bool active = tick >= 2 && tick < 2 + 479;
                expect(a && b && a->slots[0].active == active && b->slots[0].active == active,
                    "WLU-02: the squadron's command switches both craft; the 16 s limit ends it");
            }
            if (tick == 2) {
                expect(lured_by(session, 20, 12), "WLU-05/06: the in-reach enemy team attacks the last craft to pulse");
                const auto team = session.squadron_state(20);
                expect(team && team->target == 10, "WLU-05: the lured team's destination is the luring team");
                expect(order_of(session, 30).kind != tactical::OrderKind::attack, "WLU-04: an unlisted type is not lured");
                expect(order_of(session, 40).kind != tactical::OrderKind::attack, "WLU-04: a team out of reach is not lured");
                expect(order_of(session, 50).kind != tactical::OrderKind::attack, "WLU-05: a friendly team is not lured");
                const auto own = session.squadron_state(10);
                expect(own && own->mode == tactical::SquadronMode::idle && order_of(session, 10).kind == tactical::OrderKind::stop,
                    "WLU-02: switching LURE on stops the luring squadron's move");
                expect(a && a->slots[0].expires_tick != 0, "WLU-02: the switch starts the expiration countdown");
            }
            if (tick == 10) {
                expect(order_of(session, 20).kind == tactical::OrderKind::move, "WLU-06: between pulses the enemy's own order stands");
            }
            if (tick == 17) expect(lured_by(session, 20, 12), "WLU-03: the next pulse lures it again");
            if (tick == 2 + 480) {
                const auto own = session.squadron_state(10);
                expect(own && own->mode == tactical::SquadronMode::idle, "WLU-07: expiry stops the luring squadron");
                expect(a && !a->slots[0].active && a->slots[0].ready_tick > tick, "WLU-07: expiry starts the recharge");
            }
            if (tick == 515) {
                expect(order_of(session, 20).kind == tactical::OrderKind::move, "WLU-07: no pulse after the ability ends");
            }
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "lure hashes are equal for 1/2/4/8 workers");
        const auto encoded = tactical::write_replay(session.record());
        expect(static_cast<bool>(encoded), "LURE uses the existing ability command encoding");
        if (!encoded) continue;
        const auto parsed = tactical::parse_replay(encoded.value());
        expect(parsed && parsed.value() == session.record(), "LURE command bytes round trip");
        if (!parsed) continue;
        auto playback = tactical::TacticalSession::from_replay(parsed.value(), {}, {}, motion, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(playback), "LURE replay creates a session");
        if (playback) {
            for (std::size_t tick = 0; tick < hashes.size(); ++tick) {
                auto step = playback.value().step(executor);
                expect(step && step.value().state_sha256 == hashes[tick], "LURE replay matches every live frame");
            }
        }
    }
}


void test_space_healing() {
    tactical::DurabilityProfile health;
    health.type_id = 100; health.max_hull = units(1000); health.max_shields = units(500);
    health.destroyed_with_hardpoints = true;
    health.hardpoints = {{tactical::HardpointRole::other, true, units(100)},
        {tactical::HardpointRole::other, true, units(200)}, {tactical::HardpointRole::other, true, units(100)}};
    auto state = tactical::full_durability(health);
    state.hull = units(500); state.shields = units(200);
    state.hardpoints = {units(20), units(100), {}};
    const auto fraction = tactical::healing_fraction(health, state);
    expect(fraction == decimal("0.4"), "WSH-03: dead hardpoints do not dilute the healing-health fraction");
    const auto healed = tactical::heal_health(health, state, decimal("0.1"), units(2));
    expect(healed && healed.value() == 0 && std::abs(state.hardpoints[0].raw() - units(32).raw()) <= 64,
        "WSH-04: lowest surviving point gains percent of maximum plus amount");
    expect(state.hull == units(500) && state.shields == units(200) && state.hardpoints[2].raw() == 0,
        "WSH-04: healing a point leaves hull, shields and destroyed points alone");
    state.hardpoints = {units(100), units(200), {}};
    expect(tactical::heal_health(health, state, decimal("0.1"), {}).value() == tactical::hull_target
        && std::abs(state.hull.raw() - units(600).raw()) <= 1000, "WSH-04: no damaged surviving point falls back to hull");
    state.hull = health.max_hull;
    expect(tactical::healing_fraction(health, state) == units(1),
        "FSC-49/50: missing shields and destroyed points do not admit full surviving health");

    tactical::TacticalSetup setup;
    setup.seed = 2275;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 1, 1, tactical::player_flag_commandable},
        {3, 3, 3, tactical::player_flag_commandable}};
    setup.units = {{1, 99, 1, at(0, 0), math::identity_quat(), {}},
        {2, 100, 1, at(500, 0), math::identity_quat(), {}},
        {3, 100, 2, at(200, 0), math::identity_quat(), {}},
        {4, 100, 3, at(100, 0), math::identity_quat(), {}},
        {5, 101, 1, at(100, 0), math::identity_quat(), {}},
        {6, 100, 1, at(700, 0), math::identity_quat(), {}}};
    tactical::DurabilityTable durability;
    for (const tactical::TypeId type : {99U, 100U, 101U}) {
        tactical::DurabilityProfile profile; profile.type_id = type; profile.max_hull = units(1000);
        durability.profiles.push_back(profile);
    }
    tactical::CombatTable combat;
    for (const tactical::TypeId type : {99U, 100U, 101U}) {
        tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = type == 101 ? 2 : 1;
        combat.profiles.push_back(profile);
    }
    tactical::AbilityTable abilities;
    tactical::UnitAbilityProfile owner; owner.type_id = 99;
    tactical::SpecialAbilityProfile heal; heal.name = "Repair"; heal.kind = tactical::SpecialAbilityKind::force_healing;
    heal.style = tactical::SpecialActivationStyle::space_automatic; heal.initially_enabled = true;
    heal.heal_range = units(550); heal.heal_percent = decimal("0.1"); heal.heal_interval_frames = 60;
    heal.single_target_heal = true; heal.filter.applicable_categories = 1;
    owner.special.push_back(heal); abilities.profiles.push_back(owner);
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto made = tactical::TacticalSession::create(setup, {}, durability, {}, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(made), "automatic healer session creates"); if (!made) continue;
        auto session = std::move(made.value());
        const auto first_poll = session.ability_state(1)->special.slots[0].next_service_frame;
        for (const eawr::sim::EntityId id : {2U, 3U, 4U, 5U, 6U}) {
            expect(static_cast<bool>(session.submit(command(1, 1, id, id,
                tactical::DamagePayload{units(id == 2 ? 300 : 800), tactical::hull_target}))), "healing fixture damage submits");
        }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick <= 120; ++tick) {
            const auto stepped = session.step(executor); expect(static_cast<bool>(stepped), "healing tick advances");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto events = stepped.value().snapshot->combat_events();
            const auto pulse = std::find_if(events.begin(), events.end(), [](const auto& event) {
                return event.kind == tactical::CombatEventKind::heal_pulse;
            });
            expect((pulse != events.end()) == (tick >= 1 && tick >= first_poll && (tick - first_poll) % 60 == 0),
                "WSH-06: immediate first poll, then retained full-interval pulses");
            if (pulse != events.end()) expect(pulse->target == 3, "WSH-02: most damaged ally wins over closer hostile/excluded/outside targets");
        }
        if (baseline.empty()) baseline = hashes; else expect(hashes == baseline, "healing 1/2/4/8 hashes match");
        const auto encoded = tactical::write_replay(session.record());
        expect(static_cast<bool>(encoded), "healer replay writes"); if (!encoded) continue;
        const auto parsed = tactical::parse_replay(encoded.value());
        expect(static_cast<bool>(parsed), "healer replay encodes");
        if (parsed) {
            auto replay = tactical::TacticalSession::from_replay(parsed.value(), {}, durability, {}, std::nullopt, combat, {}, abilities);
            expect(static_cast<bool>(replay), "healer replay creates");
            if (replay) for (const auto& hash : hashes) {
                const auto step = replay.value().step(executor);
                expect(step && step.value().state_sha256 == hash, "healer deadline and health replay identically");
            }
        }
        // FSC-48/51: the adapter consumes an empty due scan, and deliberately uses
        // damage-before-healing inputs at the next due poll (WSH-07's named seam).
        auto empty_setup = setup;
        empty_setup.units.resize(2);
        auto empty = tactical::TacticalSession::create(empty_setup, {}, durability, {}, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(empty), "empty-scan control creates");
        if (empty) {
            const auto first = empty.value().ability_state(1)->special.slots[0].next_service_frame;
            expect(static_cast<bool>(empty.value().submit(command(first + 1, 1, 1, 2,
                tactical::DamagePayload{units(200), tactical::hull_target}))), "post-empty damage submits");
            expect(static_cast<bool>(empty.value().submit(command(first + 60, 1, 2, 2,
                tactical::DamagePayload{units(200), tactical::hull_target}))), "due-frame damage submits");
            for (std::uint64_t tick = 0; tick <= first + 60; ++tick) {
                const auto step = empty.value().step(executor);
                expect(static_cast<bool>(step), "empty-scan control advances");
                if (!step) break;
                const auto events = step.value().snapshot->combat_events();
                const auto pulses = std::count_if(events.begin(), events.end(), [](const auto& event) {
                    return event.kind == tactical::CombatEventKind::heal_pulse;
                });
                expect(pulses == (tick == first + 60 ? 1 : 0),
                    "FSC-51: damage after an empty scan waits for deadline equality");
            }
            expect(std::abs(empty.value().durability_state(2)->hull.raw() - units(700).raw()) <= 1000,
                "FSC-48/WSH-07: same-frame command damage precedes the maximum-health repair pulse");
        }
    }
}


void test_peacebringer_blast_session() {
    class PhaseOrder final : public eawr::sim::PartitionExecutor {
    public:
        mutable std::vector<std::string> names;
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            return eawr::sim::InlineExecutor{}.execute(count, partition);
        }
        eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            names.emplace_back(phase);
            return execute(count, partition);
        }
    };
    tactical::AbilityProfile ability;
    ability.kind = tactical::AbilityKind::blast;
    ability.gui_activated_ability_name = "charged_guns";
    ability.recharge_frames = 2700;
    ability.recharge_at_activation = true;
    ability.charged_blast = tactical::ChargedBlastProfile{150, units(5), {}, {}, "charge_sound"};
    auto source = unit_of(1, ability);
    tactical::SpecialAbilityProfile nested;
    nested.name = "charged_guns"; nested.kind = tactical::SpecialAbilityKind::blast;
    nested.style = tactical::SpecialActivationStyle::user_input; nested.service_interval = 1;
    nested.charged_blast = ability.charged_blast;
    source.special = {nested};
    tactical::AbilityTable abilities; abilities.profiles = {source}; abilities.humans = {1};
    expect(static_cast<bool>(tactical::validate_abilities(abilities)), "BLAST matching nested profile validates");
    auto invalid = abilities; invalid.profiles.front().special.front().name = "another_handler";
    expect(!tactical::validate_abilities(invalid), "BLAST rejects a mismatched named handler");
    tactical::CombatProfile shooter; shooter.type_id = 1; shooter.max_attack_distance = units(1000);
    tactical::WeaponProfile gun; gun.special = true; gun.hardpoint = 0; gun.range = units(1000);
    gun.min_recharge_hundredths = gun.max_recharge_hundredths = 100;
    gun.shot.emplace(); gun.shot->speed = units(1); gun.shot->max_travel = units(1000); gun.shot->damage = units(600);
    gun.charged_shot = gun.shot; gun.charged_shot->blast = {units(400), units(250), 0, {}};
    shooter.weapons = {gun};
    gun.hardpoint = 1; gun.charged_shot.reset(); shooter.weapons.push_back(gun);
    {
        tactical::DurabilityProfile profile; profile.type_id = 1; profile.max_hull = units(100);
        profile.hardpoints = {{tactical::HardpointRole::other, true, units(100)},
            {tactical::HardpointRole::other, true, units(100)}};
        auto state = tactical::full_durability(profile);
        state.hardpoints[0] = {}; state.disabled = {false, true};
        for (const bool healthy : {false, true}) {
            auto guns = shooter;
            if (healthy) {
                profile.hardpoints.push_back({tactical::HardpointRole::other, true, units(100)});
                state.hardpoints.push_back(units(100)); state.disabled.push_back(false);
                auto third = gun; third.hardpoint = 2; guns.weapons.push_back(third);
            }
            std::optional<tactical::ChargedBlastState> charge{tactical::ChargedBlastState{1, 0, false}};
            tactical::service_charged_blast(charge, tactical::detail::eligible_blast_specials(guns, &profile, &state));
            expect(charge && charge->shots_remaining == (healthy ? 1U : 0U) && charge->charged_presentation,
                "FC-63/FSC-60: destroyed and disabled SPECIALs are excluded; only healthy guns count");
            if (!healthy) {
                tactical::service_charged_blast(charge, 0);
                expect(!charge, "FD-55: zero eligible guns publish once and clean on the next poll");
            }
        }
        // FSC-60 local constraint seam: snapshot precedes same-tick hull drag.
        profile.hardpoints.resize(2);
        state = tactical::full_durability(profile); state.hull = units(10);
        std::optional<tactical::ChargedBlastState> charge{tactical::ChargedBlastState{1, 0, false}};
        tactical::service_charged_blast(charge, tactical::detail::eligible_blast_specials(shooter, &profile, &state));
        const auto dragged = tactical::service_durability(profile, {decimal("0.2"), {}, {}}, state);
        expect(dragged && state.hardpoints[0] > units(29) && state.hardpoints[0] < units(31)
            && state.hardpoints[0] == state.hardpoints[1]
            && charge && charge->shots_remaining == 2 && charge->charged_presentation,
            "FS-34/FSC-60: hull drag follows the published two-gun snapshot and retains its budget");
    }
    tactical::CombatProfile target; target.type_id = 2;
    tactical::CombatTable combat; combat.profiles = {shooter, target};
    tactical::TacticalSetup start; start.seed = 946;
    start.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    start.units = {{1, 1, 1, at(0, 0), math::identity_quat(), {}}, {2, 2, 2, at(500, 0), math::identity_quat(), {}}};
    const std::vector<tactical::SensorProfile> sensors{{1, units(2000)}, {2, units(2000)}};
    // Projectile creation is enabled by damage rules; geometry-only recipients keep
    // both slow shots alive so their selected profiles can be inspected at completion.
    auto health = durability();
    health.profiles.clear();
    for (const bool enabled : {false, true}) {
        auto zero_guns = abilities;
        auto& profile = zero_guns.profiles.front();
        profile.abilities.front().charged_blast->charge_frames = 2;
        profile.special.front().charged_blast->charge_frames = 2;
        profile.special.front().initially_enabled = enabled;
        auto created = tactical::TacticalSession::create(start, sensors, {}, {}, std::nullopt, {}, {}, zero_guns);
        expect(static_cast<bool>(created), "FSC-58: zero-gun BLAST fixture creates");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(session.snapshot()->instances().front().abilities.front().ready == enabled,
            "WHE-35: HUD and existing AI readiness honor named handler admission");
        expect(static_cast<bool>(session.submit({{1, 1, 0}, {1}, tactical::AbilityPayload{
            tactical::AbilityKind::blast, tactical::AbilityAction::activate}})), "zero-gun BLAST queues");
        for (std::uint64_t frame = 0; frame < 5; ++frame) {
            PhaseOrder executor;
            auto step = session.step(executor);
            expect(static_cast<bool>(step), "zero-gun BLAST frame succeeds");
            if (!step) break;
            if (enabled && frame == 3) {
                const auto poll = std::find(executor.names.begin(), executor.names.end(), "blast-charge");
                const auto hull = std::find(executor.names.begin(), executor.names.end(), "hull-service");
                expect(poll != executor.names.end() && hull != executor.names.end() && poll < hull,
                    "FS-34/FSC-60: the completion phase runs before hull constraints in the live tick");
            }
            const auto& status = step.value().snapshot->instances().front().abilities.front();
            if (frame == 3) expect(status.blast_charged_presentation == enabled,
                "FSC-58: admitted zero-gun completion publishes exactly once; disabled initiation stays absent");
            if (frame == 4) expect(!status.blast_charged_presentation,
                "FD-55: a zero-gun budget cleans on the following admitted poll");
        }
    }
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(start, sensors, health, {}, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(created), "BLAST session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit({{0, 1, 0}, {1}, tactical::AbilityPayload{tactical::AbilityKind::blast,
            tactical::AbilityAction::activate}})), "BLAST activation queues through the existing opcode");
        expect(static_cast<bool>(session.submit({{1, 1, 1}, {1}, tactical::AttackPayload{2}})), "BLAST attack queues after charge initiation");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t frame = 0; frame < 154; ++frame) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "BLAST frame succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto& snapshot = *stepped.value().snapshot;
            const auto& status = snapshot.instances().front().abilities.front();
            if (frame < 150) expect(status.blast_charge_remaining == 150 - frame && !status.blast_charged_presentation,
                "FC-62: late-command activation completes after exactly 150 subsequent admitted polls");
            if (frame == 150) {
                expect(status.blast_charged_presentation && status.blast_charge_remaining == 0,
                    "FD-55: completion and shot consumption retain charged presentation until the next poll");
                expect(snapshot.projectiles().size() == 2, "FSC-56: both eligible SPECIAL guns fire on the completion frame");
                expect(std::count_if(snapshot.combat_events().begin(), snapshot.combat_events().end(), [](const auto& event) {
                    return event.kind == tactical::CombatEventKind::weapon_fired && (event.outcome & tactical::fired_charged_shot) != 0;
                }) == 2, "FD-54: completion consumes both successful charged fire attempts");
                for (const auto& projectile : snapshot.projectiles()) {
                    expect(projectile.damage == units(3000) && projectile.profile_key
                        && projectile.profile_key->kind == tactical::ProjectileProfileKind::charged_blast,
                        "WAD-35/36: ordinary and override SPECIAL shots retain kind 4 and direct multiplier 5");
                    expect(projectile.blast.damage == (projectile.weapon == 0 ? units(400) : Fixed{}),
                        "WAD-36: charged instance scaling leaves the selected type's area budget unchanged");
                }
            }
            if (frame == 151) expect(!status.blast_charged_presentation, "FD-55: next admitted poll clears exhausted presentation");
            expect(!status.active && !status.ready, "BLAST initiation is instantaneous and its 90-second recharge remains running");
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "BLAST live hashes match for 1/2/4/8 workers");
        auto encoded = tactical::write_replay(session.record());
        expect(static_cast<bool>(encoded), "BLAST replay encodes");
        if (!encoded) continue;
        auto parsed = tactical::parse_replay(encoded.value());
        expect(parsed && parsed.value() == session.record(), "BLAST replay round trips without a new command opcode");
        if (!parsed) continue;
        auto replay = tactical::TacticalSession::from_replay(parsed.value(), sensors, health, {}, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(replay), "BLAST replay creates");
        if (replay) for (const auto& hash : hashes) {
            auto step = replay.value().step(executor);
            expect(step && step.value().state_sha256 == hash, "BLAST replay matches every live frame");
        }
    }
}


void test_replenished_radar_creation() {
    constexpr tactical::TypeId escort = 23;
    for (const auto source_type : {escort, xwing_squadron}) for (const bool enabled : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{11, xwing_type, 1, at(0, 0), math::identity_quat(), {}}};
            auto movement = hangar_motion();
            movement.squadrons.spawners.clear();
            auto wing = movement.squadrons.craft.front(); wing.type_id = escort;
            movement.squadrons.craft.push_back(wing);
            movement.squadrons.squadrons.front().members = {xwing_type, escort};
            auto health = hangar_durability();
            auto escort_health = health.profiles.back(); escort_health.type_id = escort;
            health.profiles.push_back(escort_health);
            tactical::SpecialAbilityProfile radar;
            radar.name = "Replenished_Radar";
            radar.kind = tactical::SpecialAbilityKind::enable_radar;
            radar.style = tactical::SpecialActivationStyle::skirmish_automatic;
            radar.initially_enabled = enabled;
            // A full creation notification would unlock this even in the disabled case.
            for (auto& row : health.profiles) if (row.type_id == source_type) {
                tactical::HardpointProfile point;
                point.destroyable = true; point.max_health = units(100); point.special_ability_name = radar.name;
                row.hardpoints.push_back(point);
            }
            tactical::AbilityProfile fill;
            fill.kind = tactical::AbilityKind::replenish_wingmen;
            fill.replenish_team = xwing_squadron; fill.recharge_frames = 6;
            tactical::UnitAbilityProfile leader; leader.type_id = xwing_type; leader.abilities = {fill};
            tactical::UnitAbilityProfile source; source.type_id = source_type; source.special = {radar};
            tactical::AbilityTable abilities; abilities.profiles = {leader, source};
            std::sort(abilities.profiles.begin(), abilities.profiles.end(),
                [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
            auto made = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, abilities);
            expect(static_cast<bool>(made), "FC-75: replenish radar fixture creates");
            if (!made) { std::cerr << made.error().message << '\n'; continue; }
            auto world = std::move(made).value();
            expect(!world.snapshot()->players().front().radar_enabled, "FC-75: no replenish radar source before birth");
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 11,
                tactical::AbilityPayload{tactical::AbilityKind::replenish_wingmen, tactical::AbilityAction::activate}))),
                "FC-75: replenish radar submits");
            eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned tick = 0; tick < 3; ++tick) {
                auto result = world.step(executor);
                expect(static_cast<bool>(result), "FC-75: replenish radar steps");
                if (!result) { std::cerr << result.error().message << '\n'; break; }
                expect(world.squadrons().size() == 1 && world.squadrons().front().members.size() == 2,
                    "FC-75: replenish creates escort and container");
                expect(world.snapshot()->players().front().radar_enabled == enabled,
                    "FC-75: newly replenished source publishes without changing its named gate");
                hashes.push_back(result.value().state_sha256 + result.value().snapshot->sha256());
            }
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "FC-75: replenish radar hashes and snapshots equal on 1/2/4/8 workers");
        }
    }
}


void test_radar_lifecycle_work_budget() {
    class RadarWork final : public eawr::sim::PartitionExecutor {
    public:
        mutable std::size_t phases{};
        bool fail{};
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            return eawr::sim::InlineExecutor{}.execute(count, partition);
        }
        eawr::core::Result<void> execute_phase(const std::string_view name, const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            if (name == "named-ability-radar") {
                ++phases;
                if (fail) return eawr::core::Result<void>::failure({});
            }
            return execute(count, partition);
        }
    } executor;
    tactical::SpecialAbilityProfile radar;
    radar.name = "Comm_Radar"; radar.kind = tactical::SpecialAbilityKind::enable_radar;
    radar.style = tactical::SpecialActivationStyle::skirmish_automatic;
    radar.initially_enabled = false;
    tactical::UnitAbilityProfile profile; profile.type_id = corvette_type; profile.special = {radar};
    tactical::AbilityTable table; table.profiles = {profile};
    auto start = setup();
    start.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}}};
    auto health = durability();
    health.profiles[0].max_shields = {}; health.profiles[0].shield_refresh = {};
    health.profiles[0].hardpoints = {{tactical::HardpointRole::other, true, units(50)},
        {tactical::HardpointRole::other, true, units(500)}};
    health.profiles[0].hardpoints[0].special_ability_name = radar.name;
    auto made = tactical::TacticalSession::create(start, {}, health, {}, {}, {}, {}, table);
    expect(static_cast<bool>(made), "FC-75: radar-free battle with a radar table creates");
    if (!made) return;
    auto world = std::move(made).value();
    for (unsigned tick = 0; tick < 4; ++tick)
        expect(static_cast<bool>(world.step(executor)), "FC-75: radar-free tick succeeds");
    expect(executor.phases == 0, "FC-75: no radar station means zero radar work despite table handlers");
    const auto born = world.stage_spawn({0, corvette_type, 1, at(50, 0), math::identity_quat(), {}});
    expect(static_cast<bool>(born), "FC-75: radar station stages");
    if (!born) return;
    world.scramble_storage_for_testing();
    for (unsigned tick = 0; tick < 4; ++tick)
        expect(static_cast<bool>(world.step(executor)), "FC-75: steady radar tick succeeds");
    expect(executor.phases == 0 && world.snapshot()->players().front().radar_enabled,
        "FC-75: enabled radar has no recurring handler poll after storage rebuild");
    expect(static_cast<bool>(world.submit(command(world.completed_tick(), 1, 0, born.value(),
        tactical::DamagePayload{units(50), 0}))), "FC-75: radar lock change submits");
    const auto before = world.canonical_state_bytes();
    executor.fail = true;
    expect(!world.step(executor) && world.canonical_state_bytes() == before
        && world.snapshot()->players().front().radar_enabled,
        "FC-75: failed radar publication preserves canonical state and cached permission");
    executor.fail = false;
    expect(static_cast<bool>(world.step(executor)) && executor.phases == 2
        && !world.snapshot()->players().front().radar_enabled,
        "FC-75: retry publishes the lock change once in the admitted tick");
    expect(static_cast<bool>(world.step(executor)) && executor.phases == 2,
        "FC-75: unchanged disabled radar performs no further work");
    expect(static_cast<bool>(world.stage_remove(born.value())), "FC-75: radar station removes");
    expect(static_cast<bool>(world.step(executor)) && executor.phases == 2,
        "FC-75: removing the last radar source restores zero radar work");
}


void test_station_radar_and_named_locks() {
    tactical::SpecialAbilityProfile radar;
    radar.name = "Comm_Radar"; radar.kind = tactical::SpecialAbilityKind::enable_radar;
    radar.style = tactical::SpecialActivationStyle::skirmish_automatic;
    radar.initially_enabled = false; radar.affects_all_allies = true;
    tactical::UnitAbilityProfile profile; profile.type_id = corvette_type; profile.special = {radar};
    auto other = radar; other.name = "other"; other.initially_enabled = true;
    other.style = tactical::SpecialActivationStyle::user_input;
    profile.special.push_back(other);
    tactical::NamedAbilityLocks locks;
    expect(!tactical::set_named_ability_lock(locks, 1, profile, "absent", true) && locks.empty(),
        "WHD-05: absent names create no retained lock");
    expect(!tactical::named_ability_enabled(locks, 1, corvette_type, radar), "WRAD-04: authored radar starts disabled");
    expect(tactical::set_named_ability_lock(locks, 1, profile, "COMM_RADAR", false), "WHD-05: first explicit unlock writes");
    expect(!tactical::set_named_ability_lock(locks, 1, profile, "comm_radar", false) && locks.size() == 1,
        "WHD-05: case-insensitive repeated writes are idempotent");
    expect(tactical::set_named_ability_lock(locks, 1, profile, "Comm_Radar", true)
        && !tactical::named_ability_enabled(locks, 1, corvette_type, radar)
        && tactical::named_ability_enabled(locks, 1, corvette_type, other),
        "WHD-05: opposite write replaces the boolean without locking an unrelated name");

    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto health = durability();
        for (auto& row : health.profiles) {
            row.max_shields = {}; row.shield_refresh = {};
            row.hardpoints = {{tactical::HardpointRole::other, true, units(50)},
                {tactical::HardpointRole::other, true, units(500)}};
            row.hardpoints[0].special_ability_name = radar.name;
        }
        tactical::AbilityTable table; table.profiles = {profile}; table.humans = {1, 2, 3};
        auto different = profile; different.type_id = frigate_type; table.profiles.push_back(different);
        auto start = setup();
        start.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 1, 2, tactical::player_flag_commandable},
            {3, 3, 3, tactical::player_flag_commandable}};
        start.departure_policy = tactical::MultiplayerDeparturePolicy{{1, 2, 3}};
        start.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
            {2, corvette_type, 1, at(50, 0), math::identity_quat(), {}},
            {3, corvette_type, 2, at(100, 0), math::identity_quat(), {}},
            {4, frigate_type, 1, at(150, 0), math::identity_quat(), {}},
            {5, plain_type, 3, at(5000, 0), math::identity_quat(), {}}};
        const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(500)}, {frigate_type, units(500)}};
        tactical::FogRules fog; fog.map_left = units(-2000); fog.map_top = units(2000);
        fog.cell_size = units(100); fog.cells_wide = 80; fog.cells_tall = 40;
        fog.service_period = 1; fog.ramp_down_step = 238;
        auto created = tactical::TacticalSession::create(start, sensors, health, {}, fog, {}, {}, table);
        expect(static_cast<bool>(created), "WRAD: station radar fixture creates");
        if (!created) { std::cerr << created.error().message << '\n'; continue; }
        auto world = std::move(created).value();
        const auto enabled = [&](const eawr::sim::EntityId id) {
            const auto state = world.ability_state(id);
            return state && state->special.slots.front().enabled;
        };
        const auto permission = [&](const tactical::PlayerId owner) {
            for (const auto& player : world.snapshot()->players()) if (player.player_id == owner) return player.radar_enabled;
            return false;
        };
        const auto bytes_have_lock = [&] {
            const auto bytes = world.canonical_state_bytes();
            constexpr std::array<std::uint8_t, 4> tag{'N','L','C','K'};
            return std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end()) != bytes.end();
        };
        expect(enabled(1) && enabled(2) && enabled(3) && enabled(4) && permission(1) && permission(2) && !permission(3),
            "WRAD-01/04: creation unlocks named radar for owner and allies only");
        expect(world.snapshot()->instances().back().visible_to == 4, "WRAD-01: radar does not reveal the distant enemy");
        expect(world.fog_cells() && !world.fog_cells()->revealed(0, start.units.back().position),
            "WRAD-01: world fog cells stay hidden with allied radar enabled");
        expect(bytes_have_lock(), "WHD-05: explicit creation unlock is canonical NLCK state");
        expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, tactical::DamagePayload{units(50), 0}))), "radar loss submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const auto step = [&] {
            const auto result = world.step(executor);
            expect(static_cast<bool>(result), "WRAD: radar lifecycle tick succeeds");
            if (result) hashes.push_back(result.value().state_sha256);
        };
        step();
        expect(!enabled(1) && !enabled(2) && enabled(3) && enabled(4),
            "WHD-05: one comm-array loss locks intact same-owner/type instances and isolates owner/type");
        expect(world.ability_state(2)->special.slots[1].enabled && permission(1),
            "WRAD-03: unrelated named handler and another allied radar source survive");
        expect(static_cast<bool>(world.stage_remove(3)) && static_cast<bool>(world.stage_remove(4)), "other radar sources remove");
        step();
        expect(!permission(1) && !permission(2) && !enabled(2) && bytes_have_lock(),
            "WRAD-03/WHD-05: removal clears source contributions without forgetting retained locks");
        auto spawn = start.units.front(); spawn.entity_id = 0;
        const auto born = world.stage_spawn(spawn);
        expect(static_cast<bool>(born), "new station stages");
        step();
        expect(enabled(2) && permission(1) && permission(2), "WHD-05: fresh intact comm array unlocks existing same-type holders");
        if (born) expect(static_cast<bool>(world.stage_remove(born.value())), "fresh station removes");
        step();
        expect(enabled(2) && permission(1), "WHD-05: explicit unlock persists beyond its originating object");
        expect(static_cast<bool>(world.submit(command(world.completed_tick(), 1, 1, 2,
            tactical::DamagePayload{units(50), 0}))), "second loss submits");
        step();
        expect(!enabled(2) && !permission(1), "WHD-05: next opposite notification replaces the unlock");
        expect(static_cast<bool>(world.submit({{world.completed_tick(), 1, 2}, {}, tactical::QuitPayload{}})), "ownership transfer submits");
        step();
        expect(world.units().front().owner == 2 && enabled(2) && permission(2),
            "WHD-05: transferred holders read the new owner's retained lock, not the old owner's lock");
        expect(world.snapshot()->instances().back().visible_to == 4, "WRAD-01: lifecycle changes still do not reveal world fog");
        expect(world.fog_cells() && !world.fog_cells()->revealed(0, start.units.back().position),
            "WRAD-01: loss, restoration, removal and transfer never reveal the distant fog cells");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "WRAD/WHD-05: lifecycle and retained state equal on 1/2/4/8 workers");
    }
}


void test_hardpoint_dependencies() {
    using K = tactical::AbilityKind;
    for (const auto kind : {K::blast, K::leech_shields, K::tractor_beam}) {
      for (const bool activation_tick_loss : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            health.profiles[0].hardpoints = {{tactical::HardpointRole::other, true, units(50)},
                {tactical::HardpointRole::other, true, units(50)}, {tactical::HardpointRole::other, true, units(500)}};
            health.profiles[1].max_shields = units(1000);
            tactical::AbilityProfile ability;
            ability.kind = kind; ability.requires_hardpoint = true; ability.required_hardpoints = {0, 1};
            ability.gui_activated_ability_name = "dependency"; ability.recharge_frames = 10;
            ability.expiration_frames = 100; ability.recharge_at_activation = kind != K::tractor_beam;
            tactical::SpecialAbilityProfile nested;
            nested.name = "dependency"; nested.style = tactical::SpecialActivationStyle::user_input;
            nested.kind = kind == K::blast ? tactical::SpecialAbilityKind::blast
                : kind == K::leech_shields ? tactical::SpecialAbilityKind::leech_shields : tactical::SpecialAbilityKind::tractor_beam;
            nested.service_interval = 1; nested.beam_max_range = units(1000); nested.filter.applicable_categories = 2;
            nested.leech_duration_frames = 100; nested.damage_per_frame = units(1);
            nested.leech_damage_bonus = units(3);
            if (kind == K::blast) {
                ability.expiration_frames = 0;
                ability.charged_blast = tactical::ChargedBlastProfile{8, units(5), {}, {}, {}};
                nested.charged_blast = ability.charged_blast;
            }
            auto holder = unit_of(corvette_type, ability); holder.special = {nested};
            tactical::AbilityTable table; table.profiles = {holder}; table.humans = {1, 2};
            tactical::CombatProfile shooter; shooter.type_id = corvette_type; shooter.category_bits = 1;
            tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
            if (kind == K::blast) {
                tactical::WeaponProfile gun; gun.special = true; gun.hardpoint = 0;
                shooter.weapons = {gun}; gun.hardpoint = 1; shooter.weapons.push_back(gun);
            }
            tactical::CombatTable combat; combat.profiles = {shooter, target};
            auto start = setup();
            start.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(100, 0), math::identity_quat(), {}}};
            auto created = tactical::TacticalSession::create(start, {}, health, motion(), std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WHD: dependency session creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            std::uint64_t sequence{};
            const auto submit = [&](const std::uint64_t tick, tactical::CommandPayload payload) {
                expect(static_cast<bool>(world.submit(command(tick, 1, sequence++, 1, std::move(payload)))), "WHD: command submits");
            };
            submit(0, tactical::AbilityPayload{kind, tactical::AbilityAction::activate, kind == K::blast ? 0U : 2U});
            submit(activation_tick_loss ? 0 : 2, tactical::DamagePayload{units(50), 0});
            submit(activation_tick_loss ? 0 : 4, tactical::DamagePayload{units(50), 1});
            submit(20, tactical::AbilityPayload{kind, tactical::AbilityAction::activate, kind == K::blast ? 0U : 2U});
            eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 22; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "WHD: dependency tick succeeds"); if (!step) break;
                hashes.push_back(step.value().state_sha256);
                const auto& status = step.value().snapshot->instances().front().abilities.front();
                if (frame == 0) {
                    expect(kind == K::blast ? world.ability_state(1)->charged_blast.has_value()
                        : status.active == !activation_tick_loss,
                        "WHD-02/03: activation-tick loss releases beams but preserves an admitted BLAST charge");
                    expect(std::none_of(step.value().snapshot->events().begin(), step.value().snapshot->events().end(), [](const auto& event) {
                        return event.kind == tactical::EventKind::order_rejected;
                    }), "WHD-01: activation is admitted before later damage commands in the same tick");
                }
                if (!activation_tick_loss && frame == 3) {
                    const auto state = world.durability_state(1);
                    tactical::AbilityGate gate; gate.hardpoint_profile = &health.profiles[0];
                    gate.hardpoint_state = state ? &*state : nullptr;
                    expect(tactical::ability_hardpoints_online(ability, gate),
                        "WHD-01: first loss leaves the second dependency available");
                }
                if (frame >= (activation_tick_loss ? 0U : 5U))
                    expect(!status.ready, "WHD-01: last loss greys HUD even after recharge");
                if (kind != K::blast && frame == 5) expect(!status.active && world.ability_state(1)->slots.front().target == 0,
                    "WHD-02: lost beam dependency releases active target");
                if (kind == K::blast && frame == 5) expect(status.blast_charge_remaining == 3,
                    "WHD-03: destruction does not cancel an in-progress charge");
                if (kind == K::blast && frame == 8) expect(status.blast_charged_presentation
                    && world.ability_state(1)->charged_blast->shots_remaining == 0,
                    "WHD-03: completion counts zero surviving SPECIALs");
                if (kind == K::blast && frame == 9) expect(!status.blast_charged_presentation,
                    "WHD-03: zero budget cleans on the next poll");
                if (frame == 20) expect(std::any_of(step.value().snapshot->events().begin(), step.value().snapshot->events().end(), [](const auto& event) {
                    return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::ability_unavailable;
                }), "WHD-01: lost dependency refuses with ability_unavailable");
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "WHD: dependency hashes equal at 1/2/4/8 workers");
        }
      }
    }
    tactical::DurabilityProfile health; health.hardpoints = {{tactical::HardpointRole::other, false, {}}};
    auto state = tactical::full_durability(health);
    tactical::AbilityProfile ability; ability.kind = K::blast; ability.requires_hardpoint = true; ability.required_hardpoints = {0};
    tactical::AbilityGate gate; gate.hardpoint_profile = &health; gate.hardpoint_state = &state;
    expect(tactical::ability_ready(ability, {}, gate, 0), "WHD-01: indestructible dependency remains available at zero health");
    state.disabled = {true};
    expect(!tactical::ability_ready(ability, {}, gate, 0), "WHD-01: disabled dependency is excluded");
    ability.required_hardpoints.clear();
    expect(!tactical::ability_ready(ability, {}, gate, 0), "WHD-01: absent required type is unavailable");
    ability.requires_hardpoint = false; ability.kind = K::energy_weapon;
    expect(tactical::ability_ready(ability, {}, gate, 0), "WHD-01: visual energy-weapon bone creates no dependency");
    gate.ion_stunned = true;
    for (const auto kind : {K::tractor_beam, K::energy_weapon}) {
        ability.kind = kind;
        ability.requires_hardpoint = kind == K::tractor_beam;
        ability.required_hardpoints = {0};
        state.disabled = {false};
        expect(tactical::ability_ready(ability, {}, gate, 0),
            "WHD-07: ion stun does not inherit ordinary-stun tractor/energy refusal");
    }
}

} // namespace ability_tests
