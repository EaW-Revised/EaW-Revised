#include "unit_tables_foc_fixtures.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/presentation/ui/ability_buttons.hpp"
#include "eawr/presentation/ui/command_sink.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace unit_tables_test_support {


// WWP-31/33: the stock bomber's two torpedoes are separated by four logical frames.
void foc_bomber_torpedo_volley(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"Y-Wing", "TIE_Bomber", "Nebulon_B_Frigate"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WWP-33: stock bomber volley data loads");
    if (!loaded) return;
    const auto combat = eawr::units::combat_table(loaded.value());
    const auto durability = eawr::units::durability_table(loaded.value());
    expect(combat && durability, "WWP-33: stock bomber profiles bind");
    if (!combat || !durability) return;
    const auto target = eawr::assets::object_type_crc("Nebulon_B_Frigate");
    for (const auto* name : {"Y-Wing", "TIE_Bomber"}) {
        const auto shooter = eawr::assets::object_type_crc(name);
        const auto* source = loaded.value().find(name);
        const auto* profile = combat.value().find(shooter);
        if (!source || !profile) { expect(false, "WWP-33: bomber exists"); continue; }
        const auto hardpoint_name = std::string_view{name} == "Y-Wing" ? "HP_BOMBER_03" : "HP_BOMBER_02";
        const auto hp = std::find_if(source->hardpoints.begin(), source->hardpoints.end(),
            [hardpoint_name](const auto& row) { return row.id == hardpoint_name; });
        expect(hp != source->hardpoints.end(), "WWP-33: bomber has its authored torpedo hardpoint");
        if (hp == source->hardpoints.end()) continue;
        const auto index = static_cast<std::uint32_t>(hp - source->hardpoints.begin());
        const auto weapon = std::find_if(profile->weapons.begin(), profile->weapons.end(),
            [index](const auto& row) { return row.hardpoint == index; });
        expect(weapon != profile->weapons.end(), "WWP-33: torpedo hardpoint binds");
        if (weapon == profile->weapons.end()) continue;
        expect(weapon->pulse_count == 2 && weapon->pulse_delay_frames == 4
            && weapon->min_recharge_hundredths == 800 && weapon->max_recharge_hundredths == 800
            && weapon->shot && weapon->shot->homing && weapon->fire_a == weapon->fire_b,
            "WWP-31/33: two same-muzzle torpedoes, four-frame gap, eight-second recharge");
        // R-01: the stock cutoff is 3.0 s; the eight-second torpedo is above it.
        expect(!weapon->ai_opportunity_forced, "R-01: a slow torpedo hardpoint is not forced for an AI owner");
        auto isolated = combat.value();
        for (auto& row : isolated.profiles) {
            if (row.type_id == shooter) row.weapons = {*weapon};
            else if (!row.planetary_weapon) row.weapons.clear();
        }
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = 1565;
            setup.players = {{1, 1, 1, tactical::player_flag_commandable},
                             {2, 2, 2, tactical::player_flag_commandable}};
            setup.units = {{1, shooter, 1, Vec3{}, eawr::sim::math::identity_quat(), {}},
                {2, target, 2, point(raw(400), 0, 0), eawr::sim::math::identity_quat(), {}}};
            std::vector<tactical::SensorProfile> sensors{{shooter, Fixed::from_raw(raw(2000))},
                                                       {target, Fixed::from_raw(raw(2000))}};
            std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
            // Stationary units isolate the authored weapon cadence from attack-pass cone gates.
            auto session = tactical::TacticalSession::create(setup, sensors, durability.value(), {}, std::nullopt, isolated);
            expect(static_cast<bool>(session), "WWP-33: stationary bomber trace starts");
            if (!session) continue;
            expect(static_cast<bool>(session.value().submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})),
                "WWP-33: bomber accepts attack order");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::uint64_t> fired;
            std::vector<std::string> hashes;
            // Spawn readiness is randomized across the initial eight-second recharge.
            for (unsigned tick = 0; tick < 600 && fired.size() < 4; ++tick) {
                const auto stepped = session.value().step(executor);
                expect(static_cast<bool>(stepped), "WWP-33: bomber trace steps");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                for (const auto& event : stepped.value().snapshot->combat_events()) {
                    if (event.kind != tactical::CombatEventKind::weapon_fired || event.shooter != 1
                        || event.weapon != index) continue;
                    fired.push_back(event.tick);
                    std::cout << "bomber-volley," << name << ',' << workers << ',' << event.tick
                        << ',' << event.origin.x.raw() << ',' << event.origin.y.raw() << ',' << event.origin.z.raw() << '\n';
                }
            }
            expect(fired.size() == 4, "WWP-31: exactly two torpedoes per volley");
            if (fired.size() == 4) expect(fired[1] - fired[0] == 4 && fired[2] - fired[1] == 240
                && fired[3] - fired[2] == 4, "WWP-33: four-frame pulse gap and 240-frame volley recharge");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WWP-33: bomber volley hashes equal on 1/2/4/8 workers");
        }
    }
}

// RG-02 / WAD-04/18: ordinary stock rockets damage neighbours independently of BARRAGE.
void foc_diamond_boron_cluster(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"Broadside_Class_Cruiser", "Marauder_Missile_Cruiser", "TIE_Fighter"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "RG-02: stock blast ships load");
    if (!loaded) return;
    const auto combat = eawr::units::combat_table(loaded.value());
    const auto durability = eawr::units::durability_table(loaded.value());
    expect(combat && durability, "WAD-01: stock blast combat and damage profiles bind");
    if (!combat || !durability) return;
    const auto target = eawr::assets::object_type_crc("TIE_Fighter");
    auto health = durability.value();
    for (auto& unit : health.profiles) if (unit.type_id == target) {
        // Keep the ordered target alive through the volley, preventing autonomous retargeting.
        unit.max_hull = Fixed::from_raw(raw(100000));
    }
    for (const auto* name : {"Broadside_Class_Cruiser", "Marauder_Missile_Cruiser"}) {
        const auto shooter = eawr::assets::object_type_crc(name);
        const auto* profile = combat.value().find(shooter);
        expect(profile && !profile->weapons.empty() && profile->weapons.front().shot
            && profile->weapons.front().shot->blast.damage == Fixed::from_raw(raw(150))
            && profile->weapons.front().shot->blast.radius == Fixed::from_raw(raw(200)),
            std::string("WAD-01: authored Diamond Boron blast binds: ") + name);
        if (!profile) continue;
        std::vector<std::string> reference;
        for (const bool blast : {false, true}) {
            auto isolated = combat.value();
            for (auto& unit : isolated.profiles) {
                if (unit.type_id != shooter && !unit.planetary_weapon) unit.weapons.clear();
                else if (!blast) for (auto& weapon : unit.weapons) {
                    if (weapon.shot) weapon.shot->blast.damage = Fixed{};
                }
            }
            for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
                tactical::TacticalSetup setup;
                setup.seed = 1604;
                setup.players = {{1, 1, 1, tactical::player_flag_commandable},
                                 {2, 2, 2, tactical::player_flag_commandable}};
                setup.units = {{1, shooter, 1, Vec3{}, eawr::sim::math::identity_quat(), {}},
                    {2, target, 2, point(raw(800), 0, 0), eawr::sim::math::identity_quat(), {}},
                    // WAD-12: the nearby craft receives area damage off the direct flight plane.
                    {3, target, 2, point(raw(800), raw(100), raw(100)), eawr::sim::math::identity_quat(), {}},
                    {4, target, 2, point(raw(800), raw(500), 0), eawr::sim::math::identity_quat(), {}}};
                std::vector<tactical::SensorProfile> sensors{{shooter, Fixed::from_raw(raw(2000))},
                                                             {target, Fixed::from_raw(raw(2000))}};
                std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
                auto session = tactical::TacticalSession::create(setup, sensors, health, {}, std::nullopt, isolated);
                expect(static_cast<bool>(session), "WAD-18: stationary stock missile cluster starts");
                if (!session) continue;
                expect(static_cast<bool>(session.value().submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})),
                    "RG-02: blast ship accepts ordinary attack");
                const eawr::platform::ThreadWorkerAdapter executor(workers);
                bool fired = false, detonated = false, direct_neighbour = false, damaged_neighbour = false;
                std::vector<std::string> hashes;
                for (unsigned tick = 0; tick < 600; ++tick) {
                    const auto stepped = session.value().step(executor);
                    expect(static_cast<bool>(stepped), "WAD-04: stock rocket cluster step succeeds");
                    if (!stepped) break;
                    hashes.push_back(stepped.value().state_sha256);
                    detonated = detonated || stepped.value().blast_detonations > 0;
                    for (const auto& event : stepped.value().snapshot->combat_events()) {
                        fired = fired || (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1);
                        direct_neighbour = direct_neighbour || (event.kind == tactical::CombatEventKind::projectile_hit && event.target == 3);
                    }
                    const auto near = session.value().durability_state(3);
                    const auto far = session.value().durability_state(4);
                    damaged_neighbour = damaged_neighbour || !near || near->hull < health.find(target)->max_hull;
                    expect(far && far->hull == health.find(target)->max_hull,
                        "WAD-10: isolated distant craft stays outside the blast");
                }
                expect(fired && !direct_neighbour, "WAD-18: missiles fire at the primary craft only");
                expect(damaged_neighbour == blast, "WAD-18: only blast damages the neighbouring craft");
                expect(detonated == blast, "WAD-01: zero area damage control disables detonation work");
                if (workers == 1) reference = hashes;
                else expect(hashes == reference, "WAD: stock cluster hashes match at 1/2/4/8 workers");
            }
        }
    }
}

// RG-03 / WAD-38: stock buttons pass through the production command sink to actual shots.
void foc_barrage_buttons(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    namespace ui = eawr::presentation::ui;
    input.types = {"Broadside_Class_Cruiser", "Marauder_Missile_Cruiser", "TIE_Fighter"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WAD-38: stock BARRAGE ships and proxy load");
    if (!loaded) return;
    const std::array<tactical::PlayerId, 1> humans{1};
    const auto content = eawr::skirmish::session_content(loaded.value(), humans);
    expect(static_cast<bool>(content), "RG-03: roster-filtered stock BARRAGE content binds");
    if (!content) return;
    const auto proxy_type = eawr::assets::object_type_crc("Dummy_Barrage_Target");
    for (const auto* name : {"Broadside_Class_Cruiser", "Marauder_Missile_Cruiser"}) {
        expect(eawr::skirmish::roster_ability_reason(name, "BARRAGE").empty(), "RG-03: stock BARRAGE gate is lifted");
        const auto shooter = eawr::assets::object_type_crc(name);
        const auto* profile = content.value().abilities.find(shooter);
        const auto slot = profile ? tactical::ability_slot(*profile, tactical::AbilityKind::barrage) : std::nullopt;
        expect(slot.has_value(), "RG-03: filtered ability profile retains stock BARRAGE");
        if (!slot) continue;
        const auto& ability = profile->abilities[*slot];
        expect(ability.expiration_frames == 300 && ability.recharge_frames == 1200
            && ability.modifiers.fire_rate == Fixed::from_raw(raw(3))
            && ability.fixed_inaccuracy == Fixed::from_raw(raw(320))
            && ability.target_z_offset == Fixed::from_raw(raw(-150)), "WAD-38: stock timers and modifiers are complete");
        auto combat = content.value().combat;
        for (auto& row : combat.profiles) if (row.type_id != shooter && !row.planetary_weapon) row.weapons.clear();
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = 1651;
            setup.players = {{1, 1, 1, tactical::player_flag_commandable},
                            {2, 2, 2, tactical::player_flag_commandable}};
            setup.units = {{1, shooter, 1, {}, eawr::sim::math::identity_quat(), {}},
                {2, eawr::assets::object_type_crc("TIE_Fighter"), 2, point(raw(800), raw(500), 0),
                    eawr::sim::math::identity_quat(), {}}};
            auto session = tactical::TacticalSession::create(setup, content.value().sensors, content.value().durability,
                content.value().motion, std::nullopt, combat, {}, content.value().abilities);
            expect(static_cast<bool>(session), "WAD-38: stock BARRAGE session starts");
            if (!session) continue;
            const auto state = session.value().ability_state(1);
            expect(state && tactical::ability_ready(ability, state->slots[*slot], {}, 0),
                "RG-03: stock BARRAGE is ready in the simulation");
            ui::CardUnit card;
            card.id = 1; card.members = {1}; card.type = name; card.ability = ui::ability_index("BARRAGE");
            const std::array<ui::CardUnit, 1> cards{card};
            ui::ReadyAbilities buttons;
            buttons.set_units(cards);
            if (!state || !tactical::ability_ready(ability, state->slots[*slot], {}, 0))
                buttons.stage({{1, card.ability, {ui::AbilityStatus::disabled, 1.0, false}}});
            const auto bar = ui::ability_bar(ui::layout_unit_cards(cards, 24), cards, buttons, nullptr);
            expect(bar.buttons.size() == 1 && !bar.buttons.front().disabled,
                "RG-03: both stock ships show an enabled BARRAGE button");
            if (bar.buttons.empty()) continue;
            const auto click = ui::ability_click(bar.buttons.front(), false, buttons);
            expect(click && click->targeted, "AB-09: stock BARRAGE click requests a world target");
            if (!click) continue;
            ui::TacticalIntent intent;
            intent.verb = ui::TacticalVerb::ability; intent.units = click->units;
            intent.unit_ability = tactical::AbilityKind::barrage;
            intent.ability_action = tactical::AbilityAction::activate;
            intent.destination = point(raw(800), 0, 0);
            const auto payload = ui::command_payload(intent);
            expect(payload && std::holds_alternative<tactical::AreaAbilityPayload>(payload.value()),
                "WAD-38: stock button request uses the production area command");
            if (!payload) continue;
            expect(static_cast<bool>(session.value().submit({{0, 1, 1}, intent.units, payload.value()})),
                "WAD-38: stock button activation submits");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            bool fired = false, proxy_seen = false;
            std::vector<std::string> hashes;
            for (unsigned tick = 0; tick < 310; ++tick) {
                const auto step = session.value().step(executor);
                expect(static_cast<bool>(step), "WAD-38: stock BARRAGE step succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
                const auto active = session.value().ability_state(1);
                if (active && active->slots[*slot].target != 0) {
                    const auto units = session.value().units();
                    const auto proxy = std::find_if(units.begin(), units.end(), [&](const auto& unit) {
                        return unit.entity_id == active->slots[*slot].target;
                    });
                    proxy_seen = proxy_seen || (proxy != units.end() && proxy->type_id == proxy_type
                        && proxy->owner == 2 && proxy->position.z == Fixed::from_raw(raw(-150)));
                }
                for (const auto& event : step.value().snapshot->combat_events())
                    fired = fired || (event.kind == tactical::CombatEventKind::weapon_fired
                        && event.shooter == 1 && (event.outcome & tactical::fired_barrage_shot) != 0U);
            }
            expect(proxy_seen && fired, std::string("WAD-38: stock BARRAGE proxy and override fire: ") + name);
            const auto ended = session.value().ability_state(1);
            expect(ended && !ended->slots[*slot].active && ended->slots[*slot].target == 0,
                "WAD-38: stock BARRAGE expires and clears its proxy target");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WAD-38: stock BARRAGE hashes agree on 1/2/4/8 workers");
        }
    }
}

void foc_super_capitals(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    input.types = {"Eclipse_Super_Star_Destroyer", "Executor_Super_Star_Destroyer", "UM12_SUPER_STAR_DESTROYER"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "MV-01: all three stock super capitals load");
    if (!loaded) return;
    const auto motion = eawr::units::motion_table(loaded.value());
    expect(static_cast<bool>(motion), "MV-01: stock super-capital motion binds");
    if (!motion) return;
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
    std::vector<tactical::PlayerCommand> commands;
    for (std::size_t i = 0; i < input.types.size(); ++i) {
        const auto* unit = loaded.value().find(input.types[i]);
        const auto id = eawr::assets::object_type_crc(input.types[i]);
        const auto* profile = motion.value().find(id);
        expect(unit && profile, "MV-01: stock super capital has a moving profile: " + input.types[i]);
        if (!unit || !profile) return;
        const auto expected = math::multiply(*unit->movement.max_rate_of_turn, Fixed::from_decimal("1.2").value()).value();
        expect(profile->rate_of_turn == expected && profile->turn_in_place_slowdown == Fixed::from_raw(raw(4)),
            "MV-01/20: stock super capital retains its authored rate and slowdown");
        const auto entity = static_cast<eawr::sim::EntityId>(i + 1);
        const Vec3 at{Fixed::from_raw(raw(static_cast<std::int64_t>(i) * 10000)), Fixed{}, Fixed{}};
        setup.units.push_back({entity, id, 1, at, tactical::yaw_rotation(Fixed{}).value(), {}});
        const Vec3 target{at.x, Fixed::from_raw(raw(10000)), at.z};
        commands.push_back({{0, 1, i}, {entity}, tactical::FacePayload{target}});
        commands.push_back({{30, 1, i + 3}, {entity}, tactical::MovePayload{target}});
    }
    std::optional<std::vector<std::string>> reference;
    tactical::TacticalReplay replay;
    replay.setup = setup;
    replay.commands = commands;
    std::sort(replay.commands.begin(), replay.commands.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
    replay.final_tick_count = 120;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto session = tactical::TacticalSession::from_replay(replay, {}, {}, motion.value());
        expect(static_cast<bool>(session), "MV-01: super capitals enter battle");
        if (!session) return;
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::size_t accepted = 0;
        for (unsigned tick = 0; tick < 120; ++tick) {
            const auto stepped = session.value().step(executor);
            expect(static_cast<bool>(stepped), "MV-01: super-capital battle steps");
            if (!stepped) return;
            hashes.push_back(stepped.value().state_sha256);
            for (const auto& event : stepped.value().snapshot->events()) accepted += event.kind == tactical::EventKind::order_accepted;
            if (tick == 20) for (const auto& unit : session.value().units()) {
                if (unit.entity_id > 3) continue;
                const auto* profile = motion.value().find(unit.type_id);
                const auto angle = tactical::yaw_degrees(unit.rotation);
                // MV-02: the order starts at frame 1; movement sampled completed_tick.
                const auto elapsed = static_cast<std::int64_t>(stepped.value().completed_tick - 1);
                const auto expected = math::divide(math::multiply(profile->rate_of_turn, Fixed::from_raw(raw(elapsed))).value(), profile->turn_in_place_slowdown).value();
                expect(angle && std::abs(angle.value().raw() - expected.raw()) < 512,
                    "MV-20: battle turns each super capital at its authored rate / slowdown: raw "
                        + std::to_string(angle ? angle.value().raw() : 0) + " expected " + std::to_string(expected.raw()));
            }
        }
        expect(accepted >= 6, "MV-01: battle accepts all super-capital face and move commands");
        for (std::size_t i = 0; i < setup.units.size(); ++i) {
            const auto& units = session.value().units();
            const auto found = std::find_if(units.begin(), units.end(), [&](const auto& unit) { return unit.entity_id == setup.units[i].entity_id; });
            expect(found != units.end() && found->position != setup.units[i].position, "MV-01: each super capital moves in battle");
        }
        if (!reference) reference = hashes;
        else expect(hashes == *reference, "super-capital battle hashes match with 1/2/4/8 workers");
    }
}

// W-09: load the stock types affected by an omitted Fires_Forward. Keep StarViper
// movement out of this contract so the test observes its cone before it turns.
void foc_object_weapon_defaults(eawr::units::LoadInput input) {
    {
        auto turret_input = input;
        turret_input.types = {"Empire_Defense_Satellite_Laser", "Rebel_Defense_Satellite_Laser", "Underworld_Defense_Satellite_Plasma"};
        turret_input.obstacles.clear();
        const auto loaded = eawr::units::load_unit_tables(turret_input);
        expect(static_cast<bool>(loaded), "UA-05: installed satellite turret profiles load");
        if (loaded) {
            const auto combat = eawr::units::combat_table(loaded.value());
            expect(static_cast<bool>(combat), "UA-05: installed satellite mechanics bind");
            for (const auto& name : turret_input.types) {
                const auto* unit = loaded.value().find(name);
                expect(unit && unit->object_turret && unit->object_turret->turret.bone == "B_Turret"
                    && unit->object_turret->turret.position && unit->object_turret->barrel.bone.empty(),
                    "UA-05: satellite binds its turret bone without a barrel");
                const auto* profile = combat ? combat.value().find(eawr::assets::object_type_crc(name)) : nullptr;
                expect(profile && profile->weapons.size() == 1 && profile->weapons.front().object_turret_required
                    && profile->weapons.front().object_turret
                    && profile->weapons.front().object_turret->speed == Fixed::from_raw(7 * Fixed::scale / 2)
                    && profile->weapons.front().object_turret->yaw_extent == Fixed::from_raw(360 * Fixed::scale)
                    && profile->weapons.front().object_turret->pitch_extent == Fixed::from_raw(90 * Fixed::scale)
                    && profile->weapons.front().object_turret_rest_delay == Fixed::from_raw(2 * Fixed::scale),
                    "UA-05: stock satellite uses authored rate/extents and the two-second idle-delay default");
            }
        }
    }
    const std::vector<std::tuple<std::string, int, int>> rows{
        {"Starviper_Fighter", 45, 45}, {"Broadside_Class_Cruiser", 30, 40},
        {"Marauder_Missile_Cruiser", 30, 40}, {"Broadside_Underworld", 30, 40},
        {"Marauder_Underworld", 30, 40}, {"Hutt_Marauder", 30, 40},
        {"Slave_I", 50, 50}, {"TIE_Prototype", 20, 40}, {"Moldy_Crow", 20, 40},
        {"Hutt_IPV1_Craft", 360, 180}, {"Tyber_Zann_Boarding_Shuttle_Prologue", 360, 180},
        {"Defense_Satellite", 360, 90}, {"Defense_Satellite_Missile", 360, 60},
        {"Defense_Satellite_Laser", 360, 40}, {"Defense_Satellite_Laser_Small", 360, 40},
        {"Pirate_Defense_Satellite_Laser", 360, 40}};
    input.types = {"TIE_Fighter"};
    for (const auto& [id, yaw, pitch] : rows) {
        static_cast<void>(yaw);
        static_cast<void>(pitch);
        input.types.push_back(id);
    }
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "W-09: affected stock object weapons load");
    if (!loaded) return;
    for (const auto& [id, yaw, pitch] : rows) {
        const auto* unit = loaded.value().find(id);
        expect(unit && unit->weapon, "W-09: stock object weapon exists: " + id);
        if (!unit || !unit->weapon) continue;
        expect(unit->weapon->cone_width_degrees == std::optional{Fixed::from_raw(raw(yaw))}
                   && unit->weapon->cone_height_degrees == std::optional{Fixed::from_raw(raw(pitch))},
               "W-09: stock object weapon extents: " + id);
    }
    auto combat = eawr::units::combat_table(loaded.value());
    expect(static_cast<bool>(combat), "W-09: affected stock weapons produce combat profiles");
    if (!combat) return;
    const auto durability = eawr::units::durability_table(loaded.value());
    expect(static_cast<bool>(durability), "W-09: stock weapons have their authored energy pools");
    if (!durability) return;
    namespace tactical = eawr::sim::tactical;
    const auto shooter = eawr::assets::object_type_crc("Starviper_Fighter");
    const auto target = eawr::assets::object_type_crc("TIE_Fighter");
    std::vector<tactical::SensorProfile> sensors{{shooter, Fixed::from_raw(raw(2000))},
                                               {target, Fixed::from_raw(raw(2000))}};
    std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    for (const auto& [position, should_fire] : {
             std::pair{point(raw(-300), 0, 0), false},
             std::pair{point(raw(300), raw(250), 0), true},
             // The cone opens at the muzzle bone (WWP-53), which sits off the unit origin: stay clear of the
             // exact 45-degree boundary the origin would give.
             std::pair{point(raw(300), raw(270), 0), true},
             std::pair{point(raw(250), raw(300), 0), false},
             std::pair{point(raw(300), 0, raw(250)), true},
             std::pair{point(raw(250), 0, raw(300)), false}}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            tactical::TacticalSetup setup;
            setup.seed = 384;
            setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
            setup.units = {{1, shooter, 1, Vec3{}, eawr::sim::math::identity_quat(), {}},
                           {2, target, 2, position, eawr::sim::math::identity_quat(), {}}};
            auto session = tactical::TacticalSession::create(setup, sensors, durability.value(), {}, std::nullopt, combat.value());
            expect(static_cast<bool>(session), "W-09: StarViper cone session is created");
            if (!session) continue;
            expect(static_cast<bool>(session.value().submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})),
                   "W-09: StarViper is ordered to fire at the fixed target");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            bool fired = false;
            std::vector<std::string> hashes;
            for (int tick = 0; tick < 150; ++tick) {
                auto stepped = session.value().step(executor);
                expect(static_cast<bool>(stepped), "W-09: StarViper cone step succeeds");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256 + stepped.value().snapshot->sha256());
                for (const auto& event : stepped.value().snapshot->combat_events()) {
                    fired = fired || (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1);
                }
            }
            expect(fired == should_fire, "W-09: StarViper cone at " + std::to_string(position.x.trunc_to_integer())
                   + "," + std::to_string(position.y.trunc_to_integer()) + ","
                   + std::to_string(position.z.trunc_to_integer()));
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "W-09: StarViper state and snapshots match at 1/2/4/8 workers");
        }
    }
}

// MD-01 to MD-05: all seven obtainable mass-driver carriers in the space census.
// Keep this separate from the M2 pins: it exercises the stock models and data.
void foc_mass_drivers(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    using eawr::units::HardpointType;
    const std::vector<std::string> carriers{"Kedalbe_Battleship", "Vengeance_Frigate",
        "Skirmish_Underworld_Star_Base_1", "Skirmish_Underworld_Star_Base_2",
        "Skirmish_Underworld_Star_Base_3", "Skirmish_Underworld_Star_Base_4",
        "Skirmish_Underworld_Star_Base_5"};
    input.types = carriers;
    // Stock targets spanning the space armor rows, including craft spawned by the stations.
    for (const auto* id : {"Corellian_Corvette", "Corellian_Gunboat", "Broadside_Class_Cruiser",
             "Tartan_Patrol_Cruiser", "Interdictor_Cruiser", "Nebulon_B_Frigate", "Acclamator_Assault_Ship",
             "Empire_Pirate_Frigate", "Victory_Destroyer", "Alliance_Assault_Frigate", "Calamari_Cruiser",
             "Star_Destroyer", "MC30_Frigate", "X-Wing", "Y-Wing", "Millennium_Falcon"}) {
        input.types.emplace_back(id);
    }
    auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "MD-01: census mass-driver carrier tables load");
    if (!loaded) return;
    const auto& tables = loaded.value();
    // Targets supply armor rows, but their unrelated weapons are outside this contract.
    // Keep the projectile and type indices intact while checking each complete carrier.
    auto carrier_tables = tables;
    std::erase_if(carrier_tables.units, [&](const auto& unit) {
        return std::find(carriers.begin(), carriers.end(), unit.id) == carriers.end();
    });
    auto combat = eawr::units::combat_table(carrier_tables);
    auto durability = eawr::units::durability_table(tables);
    expect(static_cast<bool>(combat), "MD-01: carrier combat table resolves");
    if (!combat) std::cerr << "MD-01: " << combat.error().message << '\n';
    expect(static_cast<bool>(durability), "MD-01: carrier durability table resolves");
    if (!combat || !durability) return;
    const auto types = eawr::units::damage_type_index(tables);
    const auto mass_damage = types.damage("Damage_Mass_Driver_Space");
    expect(mass_damage != tactical::no_type_index, "MD-04: mass-driver damage type resolves");
    for (const auto& id : carriers) {
        const auto* unit = tables.find(id);
        expect(unit != nullptr, "MD-01: census carrier loaded: " + id);
        if (!unit) continue;
        const auto type = eawr::assets::object_type_crc(id);
        const auto* guns = combat.value().find(type);
        const auto* health = durability.value().find(type);
        expect(guns && health, "MD-01: carrier binds both combat tables: " + id);
        if (!guns || !health) continue;
        std::size_t count{};
        for (std::size_t index = 0; index < unit->hardpoints.size(); ++index) {
            const auto& hp = unit->hardpoints[index];
            if (hp.type_name != "HARD_POINT_WEAPON_MASS_DRIVER") continue;
            ++count;
            expect(hp.type == HardpointType::weapon_mass_driver && hp.weapon.has_value(),
                "MD-01: mass-driver hardpoint is a loaded weapon: " + hp.id);
            expect(index < health->hardpoints.size()
                    && health->hardpoints[index].role == tactical::HardpointRole::weapon,
                "MD-01: destroying the battery disables its weapon: " + hp.id);
            const auto gun = std::find_if(guns->weapons.begin(), guns->weapons.end(),
                [index](const auto& entry) { return entry.hardpoint == index; });
            expect(gun != guns->weapons.end() && gun->shot.has_value(), "MD-01: battery fires: " + hp.id);
            if (gun == guns->weapons.end() || !gun->shot || !hp.weapon) continue;
            const auto& shot = *gun->shot;
            const auto range = id == "Vengeance_Frigate" ? 1700 : id == "Kedalbe_Battleship" ? 1800 : 2000;
            expect(shot.speed.raw() == raw(70) && shot.max_travel.raw() == raw(range) && !shot.homing,
                "MD-02: straight flight at authored speed with hardpoint travel: " + hp.id);
            expect(shot.damage.raw() == raw(10) && shot.damage_type == mass_damage && !shot.shield_damage
                    && shot.hitpoint_damage && !shot.energy_damage && !shot.ion_stun,
                "MD-03: bypass shields, damage hull, leave energy intact: " + hp.id);
            expect(gun->pulse_count == 8 && gun->pulse_delay_frames == 6
                    && gun->min_recharge_hundredths == 300 && gun->max_recharge_hundredths == 400,
                "MD-05: eight pulses and authored recharge: " + hp.id);
            expect(!hp.fire_a.bone.empty() && hp.fire_a.position && hp.fire_a.axes && hp.fire_b.position,
                "MD-05: stock fire bones resolve: " + hp.id);
        }
        const std::size_t expected = id == "Kedalbe_Battleship" || id == "Vengeance_Frigate" ? 2
            : id == "Skirmish_Underworld_Star_Base_5" ? 3
            : id == "Skirmish_Underworld_Star_Base_3" || id == "Skirmish_Underworld_Star_Base_4" ? 2 : 1;
        expect(count == expected, "MD-01: census carrier has every mass-driver battery: " + id);
    }
    // Check every stock row that enters the loaded armor vocabulary, through the real hit path.
    const auto& rules = *durability.value().damage;
    std::size_t armor_rows{};
    for (const auto& row : tables.constants.damage_to_armor) {
        if (row.damage_type != "Damage_Mass_Driver_Space") continue;
        ++armor_rows;
        const auto expected_factor = row.armor_type == "Armor_Bomber" || row.armor_type == "Armor_Transport"
                || row.armor_type == "Armor_Fighter" || row.armor_type == "Armor_Hero_Transport" ? "2"
            : row.armor_type == "Armor_Corellian_Corvette" || row.armor_type == "Armor_Missile_Cruiser"
                || row.armor_type == "Armor_Corellian_Gunboat" || row.armor_type == "Armor_Tartan"
                || row.armor_type == "Armor_Interdictor" || row.armor_type == "Armor_MC30_Frigate" ? "1.5"
            : row.armor_type == "Armor_Nebulon_B" || row.armor_type == "Armor_Acclamator"
                || row.armor_type == "Armor_Pirate_Frigate" || row.armor_type == "Armor_Frigate"
                || row.armor_type == "Armor_Vengeance" ? "1.25" : "1";
        expect(row.multiplier == Fixed::from_decimal(expected_factor).value(),
            "MD-04: stock mass-driver armor factor: " + row.armor_type);
        const auto armor = types.armor(row.armor_type);
        if (armor == tactical::no_type_index) continue;
        tactical::DurabilityProfile profile;
        profile.max_hull = Fixed::from_raw(raw(1000));
        profile.max_shields = Fixed::from_raw(raw(500));
        profile.armor_type = armor;
        profile.shield_armor_type = armor;
        profile.powered = true;
        profile.max_energy = Fixed::from_raw(raw(500));
        profile.hardpoints = {{tactical::HardpointRole::weapon, true, Fixed::from_raw(raw(100)), {}, {}}};
        for (const bool shielded : {false, true}) {
            for (const bool hardpoint : {false, true}) {
                auto state = tactical::full_durability(profile);
                if (!shielded) state.shields = {};
                const auto before = state;
                tactical::Hit hit{Fixed::from_raw(raw(10)), mass_damage, true, false, true,
                    hardpoint ? 0U : tactical::hull_target};
                auto result = tactical::apply_hit(profile, rules, state, hit, 1);
                expect(static_cast<bool>(result), "MD-04: stock armor hit resolves: " + row.armor_type);
                if (!result) continue;
                const auto expected = eawr::sim::math::multiply(hit.amount, row.multiplier).value();
                expect(state.shields == before.shields && state.energy == before.energy
                        && result.value().absorbed.raw() == 0 && !result.value().shield_absorbed,
                    "MD-03: stock armor hit bypasses shield and energy: " + row.armor_type);
                expect(hardpoint ? before.hardpoints[0].raw() - state.hardpoints[0].raw() == expected.raw()
                                    : before.hull.raw() - state.hull.raw() == expected.raw(),
                    "MD-04: stock armor scales hull and hardpoint damage: " + row.armor_type);
            }
        }
    }
    expect(armor_rows >= 19, "MD-04: stock targets cover the space armor families");
}

// WAB-80: only the installed Interceptor IV's missile hardpoint carries the salvo multiplier.
void foc_full_salvo(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"Interceptor4_Frigate"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WAB-80: the installed Interceptor IV loads");
    if (!loaded) return;
    const auto combat = eawr::units::combat_table(loaded.value());
    const auto abilities = eawr::units::ability_table(loaded.value(), std::vector<tactical::PlayerId>{1, 2});
    expect(combat && abilities, "WAB-80: the Interceptor IV profiles bind");
    if (!combat || !abilities) return;
    const auto type = eawr::assets::object_type_crc("Interceptor4_Frigate");
    const auto* ability = abilities.value().find(type);
    const auto slot = ability ? tactical::ability_slot(*ability, tactical::AbilityKind::full_salvo) : std::nullopt;
    expect(slot.has_value(), "WAB-80: the Interceptor IV's FULL_SALVO is modelled");
    if (!slot) return;
    const auto& salvo = ability->abilities[*slot];
    expect(salvo.expiration_frames == 210 && salvo.recharge_frames == 1350 && salvo.modifiers == tactical::AbilityModifiers{},
        "WAB-80: seven seconds on, 45 s recharge, no unit-level multiplier");
    const auto* profile = combat.value().find(type);
    expect(profile != nullptr, "WAB-80: the Interceptor IV has a combat profile");
    if (!profile) return;
    const auto* source = loaded.value().find("Interceptor4_Frigate");
    std::size_t boosted = 0;
    for (const auto& weapon : profile->weapons) {
        if (weapon.full_salvo_delay.raw() == Fixed::from_raw(raw(1)).raw()) continue;
        ++boosted;
        const bool is_missile = source && weapon.hardpoint < source->hardpoints.size()
            && source->hardpoints[weapon.hardpoint].id == "HP_Interceptor_Frigate_00";
        expect(is_missile && weapon.full_salvo_delay == eawr::sim::math::Fixed::from_decimal("0.18").value()
            && weapon.pulse_count == 6 && weapon.pulse_delay_frames == 15,
            "WAB-80: only the missile hardpoint scales, by 0.18");
    }
    expect(boosted == 1, "WAB-80: exactly one Interceptor IV hardpoint carries the multiplier");
}

// WSD-01, WSD-06, WSD-09: the installed Vengeance and Krayt bind SELF_DESTRUCT (3.5 s, 10 s) with their blast objects
// (900 / 500 and 1500 / 700, enemy-only area damage); only the Krayt runs the object script and supports autofire.
void foc_self_destruct(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"Vengeance_Frigate", "Krayt_Class_Destroyer"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WSD-01: the installed Vengeance and Krayt load");
    if (!loaded) return;
    const auto abilities = eawr::units::ability_table(loaded.value(), std::vector<tactical::PlayerId>{1, 2});
    expect(static_cast<bool>(abilities), "WSD-01: the SELF_DESTRUCT profiles bind");
    if (!abilities) return;
    struct Expected { const char* unit; const char* blast; std::int64_t damage, radius; bool krayt; };
    for (const auto& want : {Expected{"Vengeance_Frigate", "Vengeance_Self_Destruct_Blast", 900, 500, false},
                             Expected{"Krayt_Class_Destroyer", "Krayt_Self_Destruct_Blast", 1500, 700, true}}) {
        const auto* ability = abilities.value().find(eawr::assets::object_type_crc(want.unit));
        const auto slot = ability ? tactical::ability_slot(*ability, tactical::AbilityKind::self_destruct) : std::nullopt;
        expect(slot.has_value(), std::string("WSD-01: ") + want.unit + " models SELF_DESTRUCT");
        if (!slot) continue;
        const auto& self_destruct = ability->abilities[*slot];
        expect(self_destruct.expiration_frames == 105 && self_destruct.recharge_frames == 300
                && self_destruct.modifiers == tactical::AbilityModifiers{},
            std::string("WSD-03: ") + want.unit + " counts down 3.5 s and recharges 10 s after a cancel");
        expect(self_destruct.supports_autofire == want.krayt, std::string("WSD-09: ") + want.unit + " autofire follows its data");
        expect(ability->self_destruct_script == want.krayt, std::string("WSD-09: ") + want.unit + " runs the Krayt script only on the Krayt");
        const auto& spawned = self_destruct.spawned;
        expect(spawned.has_value() && spawned->type == eawr::assets::object_type_crc(want.blast)
                && spawned->countdown_frames == 0 && spawned->damage.raw() == 0
                && spawned->blast.damage == Fixed::from_raw(raw(want.damage)) && spawned->blast.radius == Fixed::from_raw(raw(want.radius))
                && !spawned->blast.dropoff && spawned->blast.max_delay.raw() == 0
                && spawned->shield_damage && spawned->hitpoint_damage,
            std::string("WSD-06: ") + want.unit + "'s blast object binds with its area damage and radius");
    }
}

void foc_peacebringer_blast(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"The_Peacebringer"}; input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WHE-35: installed Peacebringer loads");
    if (!loaded) return;
    const auto abilities = eawr::units::ability_table(loaded.value(), std::vector<tactical::PlayerId>{1});
    const auto combat = eawr::units::combat_table(loaded.value());
    expect(abilities && combat, "WHE-35: Peacebringer abilities and SPECIAL weapons bind");
    if (!abilities || !combat) return;
    const auto id = eawr::assets::object_type_crc("The_Peacebringer");
    const auto* profile = abilities.value().find(id);
    const auto slot = profile ? tactical::ability_slot(*profile, tactical::AbilityKind::blast) : std::nullopt;
    expect(slot.has_value(), "WHE-35: named BLAST is exposed");
    if (!slot) return;
    const auto& ability = profile->abilities[*slot];
    expect(ability.gui_activated_ability_name == "Peacebringer_Blast" && ability.recharge_frames == 2700
        && ability.recharge_at_activation && ability.charged_blast && ability.charged_blast->charge_frames == 150
        && ability.charged_blast->damage_multiplier == Fixed::from_raw(raw(5)),
        "WHE-35/36: installed BLAST binds five-second charge, multiplier five and ninety-second recharge");
    if (ability.charged_blast) expect(ability.charged_blast->charging_effect.empty()
        && ability.charged_blast->charged_effect.empty() && ability.charged_blast->activate_sfx == "Unit_Underworld_Plasma_Fire",
        "WHE-36: stock empty phase effects stay empty and activation audio retains its authored cue");
    const auto* guns = combat.value().find(id);
    std::size_t specials{}, overrides{};
    if (guns) for (const auto& gun : guns->weapons) if (gun.special) {
        ++specials;
        expect(gun.shot && gun.shot->damage == Fixed::from_raw(raw(gun.charged_shot ? 400 : 600)),
            "WAD-31: ordinary damage SPECIAL is 400 and ordinary ion SPECIAL is 600");
        if (gun.charged_shot) {
            ++overrides;
            expect(gun.charged_shot->damage == Fixed::from_raw(raw(600))
                && gun.charged_shot->blast.damage == Fixed::from_raw(raw(400))
                && gun.charged_shot->blast.radius == Fixed::from_raw(raw(250)),
                "WAD-35/36: charged damage profile retains direct 600 and area 400/radius 250 before instance scaling");
        }
    }
    expect(specials == 2 && overrides == 1, "WAD-35: ion SPECIAL retains its ordinary projectile while damage SPECIAL has an override");
    expect(ability.requires_hardpoint && ability.required_hardpoints.size() == 2,
        "WHD-01: stock Merciless BLAST depends on both SPECIAL points as alternatives");
    const auto health = eawr::units::durability_table(loaded.value());
    expect(static_cast<bool>(health), "WHD-01: installed Merciless durability binds");
    if (!health) return;
    const auto* hull = health.value().find(id);
    expect(hull != nullptr, "WHD-01: installed Merciless hull has a durability profile");
    if (!hull) return;
    for (const bool loss_first : {false, true}) {
        tactical::TacticalSetup setup;
        setup.seed = 2462;
        setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
        setup.units = {{1, id, 1, {}, eawr::sim::math::identity_quat(), {}}};
        auto created = tactical::TacticalSession::create(setup, {}, health.value(), {}, {}, combat.value(), {}, abilities.value());
        expect(static_cast<bool>(created), "WHD-01: installed Merciless loss session creates");
        if (!created) continue;
        auto& world = created.value();
        std::uint64_t sequence{};
        const auto activate = [&]() {
            expect(static_cast<bool>(world.submit({{0, 1, sequence++}, {1},
                tactical::AbilityPayload{tactical::AbilityKind::blast, tactical::AbilityAction::activate}})),
                "WHD-01: installed Merciless BLAST submits");
        };
        if (!loss_first) activate();
        for (const auto index : ability.required_hardpoints) {
            expect(index < hull->hardpoints.size(), "WHD-01: stock dependency index addresses durability");
            if (index >= hull->hardpoints.size()) continue;
            const auto damage = Fixed::from_raw(hull->max_shields.raw() + hull->hardpoints[index].max_health.raw());
            expect(static_cast<bool>(world.submit({{0, 1, sequence++}, {1}, tactical::DamagePayload{damage, index}})),
                "WHD-01: installed Merciless SPECIAL loss submits");
        }
        if (loss_first) activate();
        const auto step = world.step(eawr::sim::InlineExecutor{});
        expect(static_cast<bool>(step), "WHD-01: installed Merciless loss tick succeeds");
        if (!step) continue;
        const auto state = world.ability_state(1);
        expect(state && state->charged_blast.has_value() == !loss_first,
            "WHD-01/03: stock destruction before activation refuses; destruction after activation preserves charge");
        const auto instances = step.value().snapshot->instances();
        const auto ship = std::find_if(instances.begin(), instances.end(), [](const auto& instance) { return instance.entity_id == 1; });
        expect(ship != instances.end() && !ship->abilities[*slot].ready,
            "WHD-01: installed Merciless HUD greys after both SPECIAL points die");
        expect(std::any_of(step.value().snapshot->events().begin(), step.value().snapshot->events().end(), [](const auto& event) {
            return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::ability_unavailable;
        }) == loss_first, "WHD-01: installed Merciless command refusal follows same-tick command order");
    }
    // WHE-37, WST-01 (legacy #946): the Peacebringer's STEALTH binds through its own Stealth_Peacebringer
    // handler, with no purchase gate in a skirmish, and cloaks on command.
    const auto stealth = tactical::ability_slot(*profile, tactical::AbilityKind::stealth);
    expect(stealth.has_value(), "WHE-37: the installed Peacebringer has the STEALTH slot");
    if (!stealth) return;
    const auto& cloak = profile->abilities[*stealth];
    expect(cloak.gui_activated_ability_name == "Stealth_Peacebringer" && cloak.expiration_frames == 5400
        && cloak.recharge_frames == 300, "WHE-37/WST-02/03: installed 180 s cloak, 10 s recharge");
    expect(std::any_of(profile->special.begin(), profile->special.end(), [](const auto& entry) {
        return entry.kind == tactical::SpecialAbilityKind::stealth && entry.name == "Stealth_Peacebringer"
            && entry.stealth_colour == std::array<std::uint8_t, 4>{255, 255, 255, 255};
    }), "WST-12: the installed Stealth_Peacebringer handler and its white tint are read");
    tactical::TacticalSetup setup;
    setup.seed = 946;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
    setup.units = {{1, id, 1, {}, eawr::sim::math::identity_quat(), {}}};
    auto created = tactical::TacticalSession::create(setup, {}, health.value(), {}, {}, combat.value(), {}, abilities.value());
    expect(static_cast<bool>(created), "WHE-37: installed Peacebringer stealth session creates");
    if (!created) return;
    auto& world = created.value();
    expect(static_cast<bool>(world.submit({{0, 1, 0}, {1},
        tactical::AbilityPayload{tactical::AbilityKind::stealth, tactical::AbilityAction::activate}})),
        "WHE-37: installed Peacebringer STEALTH submits");
    const auto step = world.step(eawr::sim::InlineExecutor{});
    expect(static_cast<bool>(step), "WHE-37: installed Peacebringer stealth tick succeeds");
    if (!step) return;
    const auto instances = step.value().snapshot->instances();
    const auto ship = std::find_if(instances.begin(), instances.end(), [](const auto& instance) { return instance.entity_id == 1; });
    expect(ship != instances.end() && ship->stealth_on, "WHE-37/WST-02: the installed Peacebringer cloaks in a skirmish");
}

// W-07/WAD-37: preserve the authored cone through ordinary and charged SPECIAL attempts.
// With motion omitted the target stays off the bow; with motion it must turn into the cone.
void foc_asymmetric_release_geometry(eawr::units::LoadInput input) {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    input.types = {"Krayt_Class_Destroyer", "The_Peacebringer", "Acclamator_Assault_Ship", "Nebulon_B_Frigate"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "W-07: installed asymmetric carriers load");
    if (!loaded) return;
    auto combat = eawr::units::combat_table(loaded.value());
    auto health = eawr::units::durability_table(loaded.value());
    const auto motion = eawr::units::motion_table(loaded.value());
    const auto abilities = eawr::units::ability_table(loaded.value(), std::vector<t::PlayerId>{1});
    expect(combat && health && motion && abilities, "W-07: installed cone test tables bind");
    if (!combat || !health || !motion || !abilities) return;
    for (auto& profile : health.value().profiles) profile.scenario_invulnerable = true;
    const auto target = eawr::assets::object_type_crc("Nebulon_B_Frigate");
    const auto number = [](const Fixed value) { return static_cast<double>(value.raw()) / Fixed::scale; };
    const auto bearing = [&](const Vec3& delta) {
        return std::atan2(number(delta.y), number(delta.x)) * 180 / std::acos(-1.0);
    };
    for (const std::string name : {"Krayt_Class_Destroyer", "The_Peacebringer", "Acclamator_Assault_Ship"}) {
        const auto shooter = eawr::assets::object_type_crc(name);
        auto isolated = combat.value();
        const auto* unit = loaded.value().find(name);
        expect(unit != nullptr, "W-07: cone carrier exists");
        if (!unit) continue;
        for (auto& profile : isolated.profiles) {
            if (profile.type_id != shooter) {
                if (!profile.planetary_weapon) profile.weapons.clear();
            }
            else std::erase_if(profile.weapons, [&](const auto& gun) {
                return name == "Acclamator_Assault_Ship" ? gun.hardpoint >= unit->hardpoints.size()
                    || unit->hardpoints[gun.hardpoint].id != "HP_Acclamator_Weapon_FC" : !gun.special;
            });
        }
        const auto* source = isolated.find(shooter);
        expect(source && source->weapons.size() == (name == "Acclamator_Assault_Ship" ? 1U : 2U),
            "W-07: isolated authored asymmetric weapons");
        if (!source || source->weapons.empty()) continue;
        for (const auto& gun : source->weapons) {
            expect(gun.fire_axes.has_value(), "W-07: stock cone retains its bone frame");
            expect(gun.cone_width == Fixed::from_raw(raw(name == "Acclamator_Assault_Ship" ? 130 : 40))
                && gun.cone_height == Fixed::from_raw(raw(name == "Acclamator_Assault_Ship" ? 90 : 180)),
                "W-07: width and height retain their authored order");
            if (gun.fire_axes) expect((*gun.fire_axes)[0].x > Fixed::from_decimal("0.999").value(),
                "W-07: stock asymmetric fire bone points along the bow");
        }
        for (const auto& [angle, position] : {
            std::pair{0, point(raw(1000), 0, 0)}, std::pair{30, point(raw(866), raw(500), 0)},
            std::pair{60, point(raw(500), raw(866), 0)}, std::pair{90, point(0, raw(1000), 0)}}) {
            for (const bool turning : {false, true}) {
                if (turning && (angle != 90 || name == "Acclamator_Assault_Ship")) continue;
                // Idle opportunity acquisition, player attack/BLAST, and orders from the AI seat.
                for (const int mode : {0, 1, 2, 3}) {
                    if ((mode == 2 && name != "The_Peacebringer") || (turning && mode == 0)) continue;
                    t::TacticalSetup setup;
                    setup.seed = 2418;
                    setup.players = {{1, 1, 1, t::player_flag_commandable}, {2, 2, 2, t::player_flag_commandable}};
                    const auto& muzzle = source->weapons.front().fire_a;
                    const Vec3 target_position{Fixed::from_raw(position.x.raw() + muzzle.x.raw()),
                        Fixed::from_raw(position.y.raw() + muzzle.y.raw()), position.z};
                    const t::PlayerId owner = mode == 3 ? 2U : 1U;
                    setup.units = {{1, shooter, owner, Vec3{}, m::identity_quat(), {}},
                        {2, target, owner == 1 ? 2U : 1U, target_position, m::identity_quat(), {}}};
                    std::vector<t::SensorProfile> sensors{{shooter, Fixed::from_raw(raw(3000))},
                        {target, Fixed::from_raw(raw(3000))}};
                    std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
                    auto created = t::TacticalSession::create(setup, sensors, health.value(),
                        turning ? motion.value() : t::MotionTable{}, {}, isolated, {}, abilities.value());
                    expect(static_cast<bool>(created), "W-07: stock cone session creates");
                    if (!created) {
                        std::cerr << eawr::core::format_diagnostic(created.error()) << '\n';
                        continue;
                    }
                    auto& session = created.value();
                    if (mode != 0) expect(static_cast<bool>(session.submit({{0, owner, 0}, {1}, t::AttackPayload{2}})),
                        "W-07: stock cone receives player attack");
                    if (mode == 2 || (mode == 3 && name == "The_Peacebringer"))
                        expect(static_cast<bool>(session.submit({{0, owner, 1}, {1},
                        t::AbilityPayload{t::AbilityKind::blast, t::AbilityAction::activate}})),
                        "WAD-37: cone carrier receives BLAST");
                    const eawr::sim::InlineExecutor executor;
                    bool fired = false, charged = false, banked_release = false;
                    std::map<std::uint64_t, double> projectile_bearings;
                    std::size_t direction_changes{};
                    for (int tick = 0; tick < 600; ++tick) {
                        auto step = session.step(executor);
                        expect(static_cast<bool>(step), "W-07: stock cone step succeeds");
                        if (!step) break;
                        const auto& snapshot = *step.value().snapshot;
                        if (name != "Acclamator_Assault_Ship") for (const auto& projectile : snapshot.projectiles()) {
                            if (projectile.shooter != 1) continue;
                            expect(!projectile.homing, "WAD-37: Krayt SPECIAL is a direct projectile");
                            const auto direction = bearing(projectile.step);
                            const auto [first, inserted] = projectile_bearings.emplace(projectile.id, direction);
                            if (!inserted && std::abs(direction - first->second) > 0.01) ++direction_changes;
                        }
                        for (const auto& event : snapshot.combat_events()) {
                            if (event.kind != t::CombatEventKind::weapon_fired || event.shooter != 1) continue;
                            fired = true;
                            charged = charged || (event.outcome & t::fired_charged_shot) != 0;
                            const auto instance = std::find_if(snapshot.instances().begin(), snapshot.instances().end(),
                                [](const auto& value) { return value.entity_id == 1; });
                            const auto gun = std::find_if(source->weapons.begin(), source->weapons.end(),
                                [&](const auto& value) { return value.hardpoint == event.weapon; });
                            expect(instance != snapshot.instances().end() && gun != source->weapons.end(),
                                "W-07: release retains its source pose and weapon");
                            if (instance == snapshot.instances().end() || gun == source->weapons.end() || !gun->fire_axes) continue;
                            // WWP-73/74, BK-05: release and cone use the drawn, banked bone frame.
                            const auto& release_transform = instance->fixed_transform;
                            const auto& rows = release_transform.rows;
                            const Vec3 target_delta{Fixed::from_raw(target_position.x.raw() - rows[0][3].raw()),
                                Fixed::from_raw(target_position.y.raw() - rows[1][3].raw()),
                                Fixed::from_raw(target_position.z.raw() - rows[2][3].raw())};
                            const Vec3 hull_target{
                                m::dot(Vec3{rows[0][0], rows[1][0], rows[2][0]}, target_delta).value(),
                                m::dot(Vec3{rows[0][1], rows[1][1], rows[2][1]}, target_delta).value(), {}};
                            const auto display_muzzle = m::transform_point(instance->fixed_transform, gun->fire_a).value();
                            const Vec3 display_offset{Fixed::from_raw(display_muzzle.x.raw() - event.origin.x.raw()),
                                Fixed::from_raw(display_muzzle.y.raw() - event.origin.y.raw()),
                                Fixed::from_raw(display_muzzle.z.raw() - event.origin.z.raw())};
                            const auto offset_from = [&](const Vec3& local) {
                                const auto world = m::transform_point(release_transform, local).value();
                                return m::length(Vec3{Fixed::from_raw(world.x.raw() - event.origin.x.raw()),
                                    Fixed::from_raw(world.y.raw() - event.origin.y.raw()),
                                    Fixed::from_raw(world.z.raw() - event.origin.z.raw())}).value();
                            };
                            if (gun->randomize_between_fire_bones && gun->has_fire_b) {
                                // WWP-22: a banked pair bounds a world box, rather than two possible origins.
                                const auto a = m::transform_point(release_transform, gun->fire_a).value();
                                const auto b = m::transform_point(release_transform, gun->fire_b).value();
                                const auto bounded = [](const Fixed value, const Fixed first, const Fixed second) {
                                    return value.raw() >= std::min(first.raw(), second.raw()) - 64
                                        && value.raw() <= std::max(first.raw(), second.raw()) + 64;
                                };
                                expect(bounded(event.origin.x, a.x, b.x) && bounded(event.origin.y, a.y, b.y)
                                    && bounded(event.origin.z, a.z, b.z),
                                    "WWP-22/73: randomized release lies in the posed world box within Q24 rounding");
                            } else {
                                const auto offset = gun->has_fire_b ? std::min(offset_from(gun->fire_a), offset_from(gun->fire_b))
                                    : offset_from(gun->fire_a);
                                expect(offset.raw() <= 64, "WWP-73: release lies on a drawn fire bone within Q24 rounding");
                            }
                            const auto bank = session.roll_degrees(1).value_or(Fixed{});
                            if (std::abs(number(bank)) > 12.3) banked_release = true;
                            const Vec3 ray{Fixed::from_raw(event.aim.x.raw() - event.origin.x.raw()),
                                Fixed::from_raw(event.aim.y.raw() - event.origin.y.raw()),
                                Fixed::from_raw(event.aim.z.raw() - event.origin.z.raw())};
                            const auto component = [&](const std::size_t axis) {
                                return m::dot(Vec3{rows[0][axis], rows[1][axis], rows[2][axis]}, ray).value();
                            };
                            const Vec3 local{component(0), component(1), component(2)};
                            const auto& axes = *gun->fire_axes;
                            const auto x = m::dot(axes[0], local).value();
                            const auto y = m::dot(axes[1], local).value();
                            const auto z = m::dot(axes[2], local).value();
                            const auto yaw = bearing(Vec3{x, y, z});
                            const auto pitch = std::atan2(number(z), std::hypot(number(x), number(y))) * 180 / std::acos(-1.0);
                            expect(std::abs(yaw) <= number(gun->cone_width) / 2 + 0.5
                                && std::abs(pitch) <= number(gun->cone_height) / 2 + 0.5,
                                "W-07: release remains inside authored half angles (scatter tolerance)");
                            std::cout << "CONE_RELEASE " << name << " initial=" << angle << " mode=" << mode
                                << " turning=" << turning << " tick=" << event.tick << " weapon=" << event.weapon
                                << " hull_yaw=" << bearing(Vec3{rows[0][0], rows[1][0], rows[2][0]})
                                << " hull_target_yaw=" << bearing(hull_target)
                                << " bone_yaw=" << yaw << " bone_pitch=" << pitch << " bolt_yaw=" << bearing(ray)
                                << " bank=" << std::atan2(number(instance->fixed_transform.rows[2][1]),
                                    number(instance->fixed_transform.rows[2][2])) * 180 / std::acos(-1.0)
                                << " display_muzzle_offset=" << number(m::length(display_offset).value())
                                << " display_offset_xyz=" << number(display_offset.x) << ','
                                    << number(display_offset.y) << ',' << number(display_offset.z)
                                << " charged=" << ((event.outcome & t::fired_charged_shot) != 0) << '\n';
                            for (const auto& projectile : snapshot.projectiles()) {
                                if (projectile.shooter != 1 || projectile.weapon != event.weapon) continue;
                                std::cout << "CONE_PROJECTILE id=" << projectile.id << " homing=" << projectile.homing
                                    << " turn_rate=" << number(projectile.turn_rate) << " step_yaw=" << bearing(projectile.step) << '\n';
                            }
                        }
                    }
                    const bool expected = mode != 0 || name == "Acclamator_Assault_Ship";
                    if (turning) expect(banked_release, "WWP-73: stock turn releases at about 12.5 degrees bank");
                    expect(fired == (expected && (turning || angle == 0 || (name == "Acclamator_Assault_Ship" && angle <= 60))),
                        "W-07: stock off-bow firing requires its authored cone or a ship turn");
                    if ((mode == 2 || (mode == 3 && name == "The_Peacebringer")) && fired)
                        expect(charged, "WAD-37: successful BLAST case includes charged release");
                    expect(direction_changes == 0, "WAD-37: unopposed SPECIAL does not turn after release");
                    std::cout << "CONE_CASE " << name << " initial=" << angle << " mode=" << mode
                        << " turning=" << turning << " fired=" << fired << " direction_changes=" << direction_changes << '\n';
                }
            }
        }
    }
}

void foc_special_without_blast(eawr::units::LoadInput input) {
    namespace t = eawr::sim::tactical;
    input.types = {"Krayt_Class_Destroyer", "Skirmish_Underworld_Star_Base_3",
        "Skirmish_Underworld_Star_Base_4", "Skirmish_Underworld_Star_Base_5"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WAD-31/33/39: installed SPECIAL carriers load without BLAST");
    if (!loaded) return;
    const auto combat = eawr::units::combat_table(loaded.value());
    const auto abilities = eawr::units::ability_table(loaded.value(), std::vector<t::PlayerId>{1});
    const auto health = eawr::units::durability_table(loaded.value());
    expect(combat && abilities && health, "WAD-31/32: installed SPECIAL profiles bind");
    if (!combat || !abilities || !health) return;
    for (std::size_t index = 0; index < input.types.size(); ++index) {
        const auto& name = input.types[index];
        const auto id = eawr::assets::object_type_crc(name);
        const auto* gun_profile = combat.value().find(id);
        const auto* ability_profile = abilities.value().find(id);
        const auto* durability = health.value().find(id);
        expect(gun_profile && durability, "WAD-31: combat and durability present: " + name);
        expect(!ability_profile || !t::ability_slot(*ability_profile, t::AbilityKind::blast),
            "WAD-32/33: SPECIAL does not fabricate BLAST: " + name);
        expect(eawr::skirmish::roster_disabled_reason(name).empty(), "RG-02: reviewed SPECIAL carrier admitted: " + name);
        if (!gun_profile || !durability) continue;
        std::size_t specials{}, manual{};
        for (const auto& gun : gun_profile->weapons) if (gun.special) {
            ++specials;
            expect(gun.shot.has_value(), "WAD-31: installed SPECIAL has its authored projectile: " + name);
            if (!gun.shot) continue;
            expect(gun.hardpoint < durability->hardpoints.size()
                && durability->hardpoints[gun.hardpoint].role == t::HardpointRole::weapon,
                "WAD-31: SPECIAL remains an ordinary durability weapon: " + name);
            if (index == 0) {
                expect(!gun.requires_manual_target && !gun.opportunity_when_idle && !gun.opportunity_when_targeting
                    && gun.min_recharge_hundredths == 1000 && gun.max_recharge_hundredths == 1000
                    && gun.pulse_count == 1 && gun.range == Fixed::from_raw(raw(2200)),
                    "WAD-33/37: Krayt SPECIAL retains assigned-target gates and ten-second recharge");
                expect(gun.shot->damage == Fixed::from_raw(raw(gun.charged_shot ? 400 : 600))
                    && gun.shot->blast.damage == Fixed::from_raw(raw(gun.charged_shot ? 300 : 0))
                    && gun.shot->blast.radius == Fixed::from_raw(raw(gun.charged_shot ? 200 : 0))
                    && gun.shot->appearance_delay_frames == (gun.charged_shot ? 30U : 0U),
                    "WAD-33/35/37: Krayt binds ordinary direct/area damage and appearance, without charged selection");
            } else if (gun.requires_manual_target) {
                ++manual;
                expect(gun.manual_turret_required && gun.manual_turret && gun.manual_cooldown_frames == 3600
                    && gun.min_recharge_hundredths == 100 && gun.max_recharge_hundredths == 100
                    && gun.shot->appearance_delay_frames == 15
                    && gun.shot->damage == Fixed::from_raw(raw(6000))
                    && gun.shot->blast.damage == Fixed::from_raw(raw(1000))
                    && gun.shot->blast.radius == Fixed::from_raw(raw(300)),
                    "WAD-39/40: inherited manual cannon keeps turret, player clock and authored projectile: " + name);
            } else {
                expect(gun.min_recharge_hundredths == 300 && gun.max_recharge_hundredths == 400
                    && gun.pulse_count == 5 && gun.pulse_delay_frames == 18
                    && gun.shot->damage == Fixed::from_raw(raw(75)) && gun.shot->blast.damage.raw() == 0
                    && !gun.charged_shot,
                    "WAD-31/32: station plasma batteries use ordinary authored bursts, without BLAST/manual input: " + name);
            }
        }
        expect(specials == (index == 0 ? 2U : index) && manual == (index == 0 ? 0U : 1U),
            "WAD-31/39: expected ordinary/manual SPECIAL inventory: " + name);
    }
}

} // namespace unit_tables_test_support
