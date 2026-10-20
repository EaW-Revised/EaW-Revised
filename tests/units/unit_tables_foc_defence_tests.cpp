#include "unit_tables_foc_fixtures.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/roster_gate.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace unit_tables_test_support {

void foc_space_healing(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"Rebel_Defense_Satellite_Repair", "Empire_Defense_Satellite_Repair", "Corellian_Corvette"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WSH-01: installed repair satellites load"); if (!loaded) return;
    const auto abilities = eawr::units::ability_table(loaded.value(), std::vector<tactical::PlayerId>{1});
    auto combat = eawr::units::combat_table(loaded.value());
    const auto health = eawr::units::durability_table(loaded.value());
    const auto motion = eawr::units::motion_table(loaded.value());
    expect(abilities && combat && health && motion, "installed healer profiles bind");
    if (!abilities || !combat || !health || !motion) return;
    for (auto& profile : combat.value().profiles) if (!profile.planetary_weapon) profile.weapons.clear();
    const auto ship = eawr::assets::object_type_crc("Corellian_Corvette");
    const auto* ship_health = health.value().find(ship);
    expect(ship_health != nullptr, "installed corvette health profile binds"); if (!ship_health) return;
    const auto point_to_damage = std::find_if(ship_health->hardpoints.begin(), ship_health->hardpoints.end(),
        [](const auto& point) { return point.destroyable; });
    const auto damaged_point = point_to_damage == ship_health->hardpoints.end() ? tactical::hull_target
        : static_cast<std::uint32_t>(point_to_damage - ship_health->hardpoints.begin());
    // Damage commands traverse shields first; leave a live, visibly damaged healing pool.
    const auto maximum = damaged_point == tactical::hull_target ? ship_health->max_hull : point_to_damage->max_health;
    const auto damage = Fixed::from_raw(ship_health->max_shields.raw() + maximum.raw() / 2);
    for (const auto* name : {"Rebel_Defense_Satellite_Repair", "Empire_Defense_Satellite_Repair"}) {
        const auto satellite = eawr::assets::object_type_crc(name);
        const auto* profile = abilities.value().find(satellite);
        expect(profile && !profile->special.empty(), "inherited satellite healer binds"); if (!profile || profile->special.empty()) continue;
        const auto& heal = profile->special.front();
        expect(heal.kind == tactical::SpecialAbilityKind::force_healing && heal.heal_range == Fixed::from_integer(550).value()
            && heal.heal_percent == Fixed::from_decimal("0.1").value() && heal.heal_interval_frames == 60
            && heal.single_target_heal && heal.heal_sfx == "Unit_Force_Healing_Pulse"
            && heal.heal_particle == "Shield_Sparks_Medium" && heal.heal_target_bones.size() == 3
            && heal.heal_lightning == "Repair_Facility_Effect", "stock healer cadence, amount, range and presentation data apply");
        tactical::TacticalSetup setup; setup.seed = 2275;
        setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
        setup.units = {{1, satellite, 1, point(0, 0, 0), eawr::sim::math::identity_quat(), {}},
            {2, ship, 1, point(raw(200), 0, 0), eawr::sim::math::identity_quat(), {}}};
        auto session = tactical::TacticalSession::create(setup, {}, health.value(), motion.value(), std::nullopt, combat.value(), {}, abilities.value());
        if (!session) std::cerr << "installed satellite session: " << session.error().message << '\n';
        expect(static_cast<bool>(session), "installed satellite session creates"); if (!session) continue;
        expect(static_cast<bool>(session.value().submit({{2, 1, 0}, {2},
            tactical::DamagePayload{damage, damaged_point}})), "installed ship damage submits");
        const eawr::platform::ThreadWorkerAdapter executor(2);
        bool saw_pulse = false; Fixed damaged{};
        for (unsigned tick = 0; tick < 125; ++tick) {
            const auto stepped = session.value().step(executor);
            expect(static_cast<bool>(stepped), "installed healer advances"); if (!stepped) break;
            if (tick == 2) damaged = tactical::healing_fraction(*ship_health, *session.value().durability_state(2));
            for (const auto& event : stepped.value().snapshot->combat_events())
                saw_pulse = saw_pulse || (event.kind == tactical::CombatEventKind::heal_pulse && event.target == 2);
        }
        const auto healed = tactical::healing_fraction(*ship_health, *session.value().durability_state(2));
        expect(damaged.raw() < Fixed::scale && saw_pulse && healed > damaged,
            "installed damaged corvette near stock repair satellite heals");
        std::cout << "stock-healing," << name << ',' << saw_pulse << ',' << damaged.raw() << ','
            << healed.raw() << '\n';
    }
}

void foc_active_projectile_defence(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    using Kind = tactical::AbilityKind;
    input.types = {"Y-Wing", "Nebulon_B_Frigate", "Interdictor_Cruiser", "Houndstooth"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WPJ-17/WHE-32: installed active defence types load");
    if (!loaded) return;
    auto combat = eawr::units::combat_table(loaded.value());
    const auto health = eawr::units::durability_table(loaded.value());
    const auto movement = eawr::units::motion_table(loaded.value());
    const auto abilities = eawr::units::ability_table(loaded.value(), std::vector<tactical::PlayerId>{1, 2});
    expect(combat && health && movement && abilities, "WPJ-17/WHE-32: installed defence profiles bind");
    if (!combat || !health || !movement || !abilities) return;
    const auto shooter = eawr::assets::object_type_crc("Y-Wing");
    const auto victim = eawr::assets::object_type_crc("Nebulon_B_Frigate");
    const auto shield = eawr::assets::object_type_crc("Interdictor_Cruiser");
    const auto jammer = eawr::assets::object_type_crc("Houndstooth");
    auto fixture_motion = movement.value();
    // Keep actual ship locomotors for jammer recipient qualification, while
    // holding the bomber at its firing pose as in the passive flight witness.
    std::erase_if(fixture_motion.squadrons.craft, [shooter](const auto& profile) {
        return profile.type_id == shooter;
    });
    std::erase_if(fixture_motion.squadrons.squadrons, [shooter](const auto& profile) {
        return profile.type_id == shooter;
    });
    const auto* shield_abilities = abilities.value().find(shield);
    const auto* jam_abilities = abilities.value().find(jammer);
    if (!shield_abilities || !jam_abilities) { expect(false, "installed defence ability owners bind"); return; }
    const auto shield_slot = tactical::ability_slot(*shield_abilities, Kind::missile_shield);
    const auto well_slot = tactical::ability_slot(*shield_abilities, Kind::interdict);
    expect(well_slot && shield_slot, "WIN/WMS: both installed Interdictor abilities bind");
    if (well_slot && shield_slot) {
        expect(shield_abilities->abilities[*well_slot].disable_movement
            && shield_abilities->abilities[*shield_slot].disable_movement,
            "WIN-01/WMS-01: both deployment locks read Disable_Movement");
        expect(shield_abilities->abilities[*well_slot].expiration_frames == 0
            && shield_abilities->abilities[*shield_slot].expiration_frames == 0,
            "WIN-01/WMS-01: installed deployments have no duration");
        expect(!shield_abilities->gravity_well_hardpoints.empty(), "WIN-02: installed gravity-well indices bind");
        expect(eawr::skirmish::roster_ability_reason("Interdictor_Cruiser", "INTERDICT").empty()
            && eawr::skirmish::roster_ability_reason("Interdictor_Cruiser", "MISSILE_SHIELD").empty(),
            "RG-03: both Interdictor buttons are admitted");
        auto unlocked = loaded.value();
        for (auto& unit : unlocked.units) if (unit.id == "Interdictor_Cruiser")
            for (auto& ability : unit.abilities) ability.disable_movement = false;
        expect(eawr::units::content_identity(unlocked) != eawr::units::content_identity(loaded.value()),
            "WIN/WMS: consumed deployment locks bind multiplayer content identity");
    }
    const auto jam_slot = tactical::ability_slot(*jam_abilities, Kind::sensor_jamming);
    expect(shield_slot && jam_slot, "WPJ-17/WHE-32: installed shield and jammer are activatable");
    if (!shield_slot || !jam_slot) return;
    expect(shield_abilities->abilities[*shield_slot].effective_radius == Fixed::from_raw(raw(750))
        && jam_abilities->abilities[*jam_slot].effective_radius == Fixed::from_raw(raw(900))
        && jam_abilities->abilities[*jam_slot].expiration_frames == 900
        && jam_abilities->abilities[*jam_slot].recharge_frames == 2250,
        "WPJ-17/WHE-31/32: installed authored radii and jammer duration/recharge apply");
    const auto* bomber = combat.value().find(shooter);
    if (!bomber) { expect(false, "installed defence shooter profile exists"); return; }
    const auto torpedo = std::find_if(bomber->weapons.begin(), bomber->weapons.end(), [](const auto& weapon) {
        return weapon.shot && weapon.shot->homing && weapon.shot->turn_rate.raw() > 0;
    });
    if (torpedo == bomber->weapons.end()) { expect(false, "installed defence torpedo exists"); return; }
    for (auto& profile : combat.value().profiles) {
        if (profile.type_id == shooter) profile.weapons = {*torpedo};
        else if (!profile.planetary_weapon) profile.weapons.clear();
        if (profile.type_id == shield || profile.type_id == jammer) {
            profile.collision.reset(); profile.meshes.clear(); profile.mesh_bounds.reset();
        }
    }
    std::vector<std::vector<tactical::Projectile>> active_trace, switched_trace;
    for (const unsigned mode : {0U, 1U, 2U}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = 1791; setup.players = {{1, 1, 1, tactical::player_flag_commandable},
                {2, 2, 2, tactical::player_flag_commandable}};
            // Low-ID jammer activates after the high-ID shield's creation registration.
            // Opposite source offsets make the first source's deflection observable.
            setup.units = {{1, jammer, 2, point(raw(200), raw(600), 0), eawr::sim::math::identity_quat(), {}},
                {2, victim, 2, point(raw(400), 0, 0), eawr::sim::math::identity_quat(), {}},
                {3, shield, 2, point(raw(200), raw(-600), 0), eawr::sim::math::identity_quat(), {}},
                {4, shooter, 1, {}, eawr::sim::math::identity_quat(), {}}};
            std::vector<tactical::SensorProfile> sensors;
            for (const auto type : {shooter, victim, shield, jammer}) sensors.push_back({type, Fixed::from_raw(raw(2000))});
            std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
            auto created = tactical::TacticalSession::create(setup, sensors, health.value(), fixture_motion,
                std::nullopt, combat.value(), {}, abilities.value());
            expect(static_cast<bool>(created), "WPJ-17/WHE-32: installed active defence session creates");
            if (!created) continue;
            auto session = std::move(created).value();
            const auto submit = [&](const std::uint64_t frame, const tactical::PlayerId owner,
                const eawr::sim::EntityId id, tactical::CommandPayload payload) {
                expect(static_cast<bool>(session.submit({{frame, owner, 0}, {id}, std::move(payload)})),
                    "WPJ-17/WHE-32: installed defence lifecycle command submits");
            };
            submit(0, 1, 4, tactical::AttackPayload{2});
            submit(0, 2, 3, tactical::AbilityPayload{Kind::missile_shield, tactical::AbilityAction::activate});
            submit(2, 2, 1, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate});
            if (mode > 0) submit(245, 2, 3, tactical::AbilityPayload{Kind::missile_shield, tactical::AbilityAction::deactivate});
            if (mode == 2) submit(251, 2, 1, tactical::DamagePayload{Fixed::from_raw(raw(100000)), tactical::hull_target});
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            bool ordered_deflection = false, removal_changes_flight = false;
            std::optional<std::uint64_t> first_torpedo;
            bool examined_first_service = false;
            std::size_t fired = 0;
            for (std::size_t frame = 0; frame < 300; ++frame) {
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "WPJ-17/WHE-32: installed active defence tick succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
                for (const auto& event : step.value().snapshot->combat_events())
                    fired += event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 4;
                for (const auto& projectile : session.projectiles()) {
                    if (!projectile.homing || projectile.shooter != 4) continue;
                    // Capture the first service of the first launched torpedo, before
                    // later guidance or scatter could accidentally produce the same sign.
                    if (!first_torpedo) first_torpedo = projectile.id;
                    else if (!examined_first_service && projectile.id == *first_torpedo) {
                        ordered_deflection = projectile.step.y.raw() > 0;
                        examined_first_service = true;
                    }
                    const auto& comparison = mode == 1 ? active_trace : switched_trace;
                    if (mode > 0 && frame >= (mode == 1 ? 246U : 252U) && frame < comparison.size()) {
                        const auto matching = std::find_if(comparison[frame].begin(), comparison[frame].end(),
                            [&](const auto& other) { return other.id == projectile.id; });
                        removal_changes_flight = removal_changes_flight || (matching != comparison[frame].end()
                            && matching->step != projectile.step);
                    }
                }
                if (workers == 1 && mode < 2) {
                    auto& trace = mode == 0 ? active_trace : switched_trace;
                    trace.emplace_back(session.projectiles().begin(), session.projectiles().end());
                }
                if (mode == 2 && frame == 251) expect(!session.ability_state(1), "WPJ-17: installed jammer death removes owner");
            }
            expect(fired > 0 && examined_first_service && ordered_deflection,
                "WPJ-17: high-ID creation source wins over later low-ID jammer activation");
            if (mode > 0) expect(removal_changes_flight, "WPJ-17/WPJ-10: installed deactivation/death changes live missile flight");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WPJ-17/WHE-32: installed activation/deactivation/death equal at 1/2/4/8 workers");
            std::cout << "active-defence," << mode << ',' << workers << ',' << fired << ','
                << ordered_deflection << ',' << removal_changes_flight << '\n';
        }
    }
}

// WST-01 to WST-11: the installed Vengeance's cloak loads with its authored numbers, an installed A-wing
// (Target_Stealth_Units) detects it inside its reveal range, and the session is equal at 1, 2, 4 and 8 workers.
void foc_stealth(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    using Kind = tactical::AbilityKind;
    input.types = {"Vengeance_Frigate", "A-Wing", "Nebulon_B_Frigate"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WST-01: installed stealth types load");
    if (!loaded) return;
    const auto combat = eawr::units::combat_table(loaded.value());
    const auto health = eawr::units::durability_table(loaded.value());
    const auto movement = eawr::units::motion_table(loaded.value());
    const auto abilities = eawr::units::ability_table(loaded.value(), std::vector<tactical::PlayerId>{1, 2});
    expect(combat && health && movement && abilities, "WST-01: installed stealth profiles bind");
    if (!combat || !health || !movement || !abilities) return;
    const auto vengeance = eawr::assets::object_type_crc("Vengeance_Frigate");
    const auto awing = eawr::assets::object_type_crc("A-Wing");
    const auto frigate = eawr::assets::object_type_crc("Nebulon_B_Frigate");
    const auto* owner = abilities.value().find(vengeance);
    const auto slot = owner ? tactical::ability_slot(*owner, Kind::stealth) : std::nullopt;
    expect(owner && slot, "WST-01: the installed Vengeance has the STEALTH slot");
    if (!owner || !slot) return;
    expect(owner->abilities[*slot].expiration_frames == 1800 && owner->abilities[*slot].recharge_frames == 450,
        "WST-02/03: installed 60 s cloak, 15 s recharge");
    const auto handler = std::find_if(owner->special.begin(), owner->special.end(), [](const auto& entry) {
        return entry.kind == tactical::SpecialAbilityKind::stealth;
    });
    expect(handler != owner->special.end() && handler->stealth_transition == Fixed::from_raw(Fixed::scale / 2)
        && handler->stealth_colour == std::array<std::uint8_t, 4>{255, 255, 255, 255},
        "WST-12: installed transition 0.5 s and white tint are read");
    const auto* detector = combat.value().find(awing);
    const auto* cloaked = combat.value().find(vengeance);
    expect(detector && detector->target_stealth && detector->stealth_detection_range == Fixed::from_raw(raw(600))
        && cloaked && !cloaked->target_stealth && cloaked->stealth_detection_range.raw() == 0,
        "WST-07/08: the installed A-wing targets stealth within its 600 reveal range; the Vengeance does not");
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.seed = 1887; setup.players = {{1, 1, 1, tactical::player_flag_commandable},
            {2, 2, 2, tactical::player_flag_commandable}};
        setup.units = {{1, vengeance, 1, {}, eawr::sim::math::identity_quat(), {}},
            {2, frigate, 2, point(raw(1500), 0, 0), eawr::sim::math::identity_quat(), {}}};
        std::vector<tactical::SensorProfile> sensors;
        for (const auto type : {vengeance, awing, frigate}) sensors.push_back({type, Fixed::from_raw(raw(2000))});
        std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
        auto created = tactical::TacticalSession::create(setup, sensors, health.value(), movement.value(),
            std::nullopt, combat.value(), {}, abilities.value());
        expect(static_cast<bool>(created), "WST: installed stealth session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit({{40, 1, 0}, {1}, tactical::AbilityPayload{Kind::stealth,
            tactical::AbilityAction::activate}})), "WST-02: the Vengeance cloaks");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        bool hidden = false, revealed = false;
        for (std::size_t frame = 0; frame < 400; ++frame) {
            if (frame == 150) expect(static_cast<bool>(session.stage_spawn({3, awing, 2, point(raw(400), 0, 0),
                eawr::sim::math::identity_quat(), {}})), "WST-08: an A-wing arrives within 600");
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "WST: installed stealth tick succeeds");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            for (const auto& instance : step.value().snapshot->instances()) {
                if (instance.entity_id != 1) continue;
                hidden = hidden || (frame < 100 && instance.stealth_invisible);
                revealed = revealed || (hidden && instance.stealth_on && !instance.stealth_invisible);
            }
        }
        expect(hidden && revealed, "WST-08/09: hidden without a detector, revealed once an A-wing is within 600");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WST: installed stealth equal at 1/2/4/8 workers");
    }
}

// WLD-01 to WLD-12: the installed Crusader's point defence loads with its authored numbers, and shoots down a
// bomber's torpedo identically at 1, 2, 4 and 8 workers.
void foc_laser_defence(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    using Kind = tactical::AbilityKind;
    input.types = {"Y-Wing", "Nebulon_B_Frigate", "Crusader_Gunship"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WLD-01: installed laser defence types load");
    if (!loaded) return;
    auto combat = eawr::units::combat_table(loaded.value());
    const auto health = eawr::units::durability_table(loaded.value());
    const auto movement = eawr::units::motion_table(loaded.value());
    const auto abilities = eawr::units::ability_table(loaded.value(), std::vector<tactical::PlayerId>{1, 2});
    expect(combat && health && movement && abilities, "WLD-01: installed laser defence profiles bind");
    if (!combat || !health || !movement || !abilities) return;
    const auto shooter = eawr::assets::object_type_crc("Y-Wing");
    const auto victim = eawr::assets::object_type_crc("Nebulon_B_Frigate");
    const auto crusader = eawr::assets::object_type_crc("Crusader_Gunship");
    const auto* owner = abilities.value().find(crusader);
    const auto slot = owner ? tactical::ability_slot(*owner, Kind::laser_defense) : std::nullopt;
    expect(owner && slot, "WLD-01: the installed Crusader has the LASER_DEFENSE slot");
    if (!owner || !slot) return;
    const auto& ability = owner->abilities[*slot];
    expect(ability.expiration_frames == 1050 && ability.recharge_frames == 1800 && ability.recharge_at_activation,
        "WLD-02/WLD-06: installed 35 s run, 60 s recharge from the switch-on");
    const auto handler = std::find_if(owner->special.begin(), owner->special.end(), [](const auto& entry) {
        return entry.kind == tactical::SpecialAbilityKind::laser_defense;
    });
    expect(handler != owner->special.end(), "WLD-01: the installed Crusader has the nested handler");
    if (handler == owner->special.end()) return;
    expect(handler->defence_radius == Fixed::from_raw(raw(500)) && handler->defence_min_travel == Fixed::from_raw(raw(100))
        && handler->defence_look_frames == 15 && handler->defence_duration_frames == 1050
        && handler->defence_categories == (tactical::defence_category_missile | tactical::defence_category_rocket
            | tactical::defence_category_mptl_rocket),
        "WLD-02 to WLD-04/WLD-08: installed reach, minimum flight, look cadence, run and categories apply");
    expect(!handler->defence_zap_sfx.empty() && !handler->defence_beam_texture.empty() && !handler->defence_bones.empty()
        && handler->defence_beam_frames > 0, "WLD-12: installed beam texture, bones, frames and zap sound are read");
    const auto* bomber = combat.value().find(shooter);
    if (!bomber) { expect(false, "WLD-05: installed bomber profile exists"); return; }
    const auto torpedo = std::find_if(bomber->weapons.begin(), bomber->weapons.end(), [](const auto& weapon) {
        return weapon.shot && weapon.shot->homing && weapon.shot->turn_rate.raw() > 0;
    });
    if (torpedo == bomber->weapons.end()) { expect(false, "WLD-05: installed torpedo exists"); return; }
    for (auto& profile : combat.value().profiles) {
        if (profile.type_id == shooter) profile.weapons = {*torpedo};
        else if (!profile.planetary_weapon) profile.weapons.clear();
    }
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.seed = 1876; setup.players = {{1, 1, 1, tactical::player_flag_commandable},
            {2, 2, 2, tactical::player_flag_commandable}};
        setup.units = {{1, victim, 2, point(raw(900), 0, 0), eawr::sim::math::identity_quat(), {}},
            {2, crusader, 2, point(raw(450), raw(60), 0), eawr::sim::math::identity_quat(), {}},
            {3, shooter, 1, {}, eawr::sim::math::identity_quat(), {}}};
        std::vector<tactical::SensorProfile> sensors;
        for (const auto type : {shooter, victim, crusader}) sensors.push_back({type, Fixed::from_raw(raw(2000))});
        std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
        auto created = tactical::TacticalSession::create(setup, sensors, health.value(), movement.value(),
            std::nullopt, combat.value(), {}, abilities.value());
        expect(static_cast<bool>(created), "WLD-05: installed laser defence session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit({{0, 1, 0}, {3}, tactical::AttackPayload{1}})), "WLD-05: the bomber attacks");
        expect(static_cast<bool>(session.submit({{0, 2, 0}, {2}, tactical::AbilityPayload{Kind::laser_defense,
            tactical::AbilityAction::activate}})), "WLD-02: the Crusader switches its laser defence on");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::size_t fired = 0, zapped = 0;
        for (std::size_t frame = 0; frame < 300; ++frame) {
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "WLD-05: installed laser defence tick succeeds");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            for (const auto& event : step.value().snapshot->combat_events()) {
                fired += event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 3;
                zapped += event.kind == tactical::CombatEventKind::point_defence_zap && event.shooter == 2;
            }
        }
        expect(fired > 0 && zapped > 0, "WLD-05: the installed Crusader shoots down the bomber's torpedoes");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WLD-12: installed laser defence equal at 1/2/4/8 workers");
        std::cout << "laser-defence," << workers << ',' << fired << ',' << zapped << '\n';
    }
}

// WRB-01 to WRB-06: the installed Underworld carriers load their Redirect_Blaster handlers, and the three
// armour upgrades enable them per level, for the right types, from the right stations.
void foc_redirect_blaster(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"Starviper_Fighter", "Crusader_Gunship", "Kedalbe_Battleship", "Y-Wing",
        "US_Magnetically_Sealed_Armor_L1_Upgrade", "US_Magnetically_Sealed_Armor_L2_Upgrade",
        "US_Magnetically_Sealed_Armor_L3_Upgrade", "Skirmish_Underworld_Star_Base_5"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WRB-01: installed redirect blaster types load");
    if (!loaded) return;
    const auto abilities = eawr::units::ability_table(loaded.value(), std::vector<tactical::PlayerId>{1, 2});
    auto combat = eawr::units::combat_table(loaded.value());
    expect(abilities && combat, "WRB-01: installed redirect blaster profiles bind");
    if (!abilities || !combat) return;
    const struct { const char* type; std::vector<std::pair<const char*, std::int64_t>> levels; } carriers[]{
        {"Starviper_Fighter", {{"US_Magnetically_Sealed_Armor_Bonus_L1", 250}, {"US_Magnetically_Sealed_Armor_Bonus_L2", 300},
                               {"US_Magnetically_Sealed_Armor_Bonus_L3", 300}}},
        {"Crusader_Gunship", {{"US_Magnetically_Sealed_Armor_Bonus_L2", 300}, {"US_Magnetically_Sealed_Armor_Bonus_L3", 300}}},
        {"Kedalbe_Battleship", {{"US_Magnetically_Sealed_Armor_Bonus_L3", 300}}},
    };
    for (const auto& carrier : carriers) {
        const auto* profile = abilities.value().find(eawr::assets::object_type_crc(carrier.type));
        expect(profile != nullptr, std::string("WRB-01: ") + carrier.type + " has an ability profile");
        if (profile == nullptr) continue;
        std::vector<const tactical::SpecialAbilityProfile*> handlers;
        for (const auto& special : profile->special) {
            if (special.kind == tactical::SpecialAbilityKind::redirect_blaster) handlers.push_back(&special);
        }
        expect(handlers.size() == carrier.levels.size(), std::string("WRB-01: ") + carrier.type + " has its authored levels");
        for (std::size_t index = 0; index < handlers.size() && index < carrier.levels.size(); ++index) {
            const auto& handler = *handlers[index];
            const auto permille = carrier.levels[index].second;
            expect(handler.name == carrier.levels[index].first
                && std::llabs(handler.block_chance.raw() * 1000 - permille * Fixed::scale) < 1000
                && handler.redirect_chance.raw() == 0 && handler.reaction_arc == Fixed::from_raw(raw(360))
                && handler.max_redirect_angle == Fixed::from_raw(raw(30)) && handler.initially_enabled == std::optional<bool>(false)
                && !handler.turn_to_face_unblockable && handler.filter.applicable_categories != 0
                && !handler.redirect_activate_sfx.empty(), std::string("WRB-04: ") + carrier.type + " level "
                + std::to_string(index + 1) + " applies its authored numbers");
        }
    }
    // WRB-06: a laser-like shot of the Y-Wing is deflectable, its torpedo is not.
    const auto* bomber = combat.value().find(eawr::assets::object_type_crc("Y-Wing"));
    expect(bomber != nullptr, "WRB-06: the Y-Wing has a combat profile");
    if (bomber != nullptr) {
        bool blockable = false, refused = false;
        for (const auto& weapon : bomber->weapons) {
            if (!weapon.shot) continue;
            (weapon.shot->redirect_blockable ? blockable : refused) = true;
            if (weapon.shot->homing) expect(!weapon.shot->redirect_blockable, "WRB-06: a homing torpedo is never deflected");
        }
        expect(blockable && refused, "WRB-06: the Y-Wing has both a deflectable laser and a refused torpedo");
    }
    // WRB-05: the upgrade objects' Enable_Ability rows, and their place in the Underworld menus.
    auto rules = eawr::skirmish::economy_rules({}, {}, loaded.value());
    expect(static_cast<bool>(rules), "WRB-05: installed upgrade rules build");
    if (!rules) return;
    const struct { const char* upgrade; const char* ability; std::vector<const char*> types; } upgrades[]{
        {"US_Magnetically_Sealed_Armor_L1_Upgrade", "us_magnetically_sealed_armor_bonus_l1", {"Starviper_Fighter"}},
        {"US_Magnetically_Sealed_Armor_L2_Upgrade", "us_magnetically_sealed_armor_bonus_l2", {"Starviper_Fighter", "Crusader_Gunship"}},
        {"US_Magnetically_Sealed_Armor_L3_Upgrade", "us_magnetically_sealed_armor_bonus_l3",
            {"Starviper_Fighter", "Crusader_Gunship", "Kedalbe_Battleship"}},
    };
    const auto faction = eawr::skirmish::faction_id("Underworld");
    const auto* menu = rules.value().menu(eawr::skirmish::type_id("Skirmish_Underworld_Star_Base_5"), faction);
    for (const auto& entry : upgrades) {
        const auto type = eawr::skirmish::type_id(entry.upgrade);
        const auto* profile = rules.value().upgrade(type);
        expect(profile != nullptr && profile->ability_enables.size() == 1, std::string("WRB-05: ") + entry.upgrade + " has its Enable_Ability");
        if (profile == nullptr || profile->ability_enables.size() != 1) continue;
        const auto& rule = profile->ability_enables.front();
        std::vector<tactical::TypeId> expected;
        for (const auto* name : entry.types) expected.push_back(eawr::assets::object_type_crc(name));
        std::sort(expected.begin(), expected.end());
        expect(rule.ability_name == entry.ability && rule.applicable == expected && rule.all_allies,
            std::string("WRB-05: ") + entry.upgrade + " names its level's handler for its types, for all allies");
        const auto* option = menu != nullptr ? menu->find(type) : nullptr;
        expect(option != nullptr && option->available && option->kind == tactical::BuildKind::upgrade,
            std::string("WRB-05: ") + entry.upgrade + " is purchasable at a level-5 Underworld station");
    }
}

// WPJ-17: the installed sensor satellite is a real always-on missile-shield source.
void foc_passive_projectile_defence(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"Y-Wing", "Nebulon_B_Frigate", "Underworld_Defense_Satellite_Sensor"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WPJ-17: installed passive shield source loads");
    if (!loaded) return;
    auto combat = eawr::units::combat_table(loaded.value());
    auto durability = eawr::units::durability_table(loaded.value());
    expect(combat && durability, "WPJ-17: installed shield and projectile profiles bind");
    if (!combat || !durability) return;
    const auto shooter = eawr::assets::object_type_crc("Y-Wing");
    const auto victim = eawr::assets::object_type_crc("Nebulon_B_Frigate");
    const auto shield = eawr::assets::object_type_crc("Underworld_Defense_Satellite_Sensor");
    const auto* source = combat.value().find(shield);
    expect(source && source->passive_missile_shield_radius == Fixed::from_raw(raw(1000)),
        "WPJ-17: installed satellite applies its authored 1000-unit passive radius");
    const auto* bomber = combat.value().find(shooter);
    if (!bomber) { expect(false, "WPJ-17: installed bomber profile exists"); return; }
    const auto torpedo = std::find_if(bomber->weapons.begin(), bomber->weapons.end(), [](const auto& weapon) {
        return weapon.shot && weapon.shot->homing && weapon.shot->turn_rate.raw() > 0;
    });
    expect(torpedo != bomber->weapons.end(), "WPJ-17: installed torpedo has authored homing and turn rate");
    if (torpedo == bomber->weapons.end()) return;
    std::vector<std::string> reference;
    std::vector<std::vector<tactical::Projectile>> defended_trace;
    for (const bool defended : {true, false}) {
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            if (!defended && workers != 1) continue;
            auto table = combat.value();
            for (auto& profile : table.profiles) {
                if (profile.type_id == shooter) profile.weapons = {*torpedo};
                else if (!profile.planetary_weapon) profile.weapons.clear();
                if (!defended) profile.passive_missile_shield_radius = {};
            }
            tactical::TacticalSetup setup;
            setup.seed = 1761; setup.players = {{1, 1, 1, tactical::player_flag_commandable},
                {2, 2, 2, tactical::player_flag_commandable}};
            setup.units = {{1, shooter, 1, {}, eawr::sim::math::identity_quat(), {}},
                {2, victim, 2, point(raw(400), 0, 0), eawr::sim::math::identity_quat(), {}},
                {3, shield, 2, point(raw(200), 0, 0), eawr::sim::math::identity_quat(), {}}};
            std::vector<tactical::SensorProfile> sensors{{shooter, Fixed::from_raw(raw(2000))},
                {victim, Fixed::from_raw(raw(2000))}, {shield, Fixed::from_raw(raw(2000))}};
            std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
            auto created = tactical::TacticalSession::create(setup, sensors, durability.value(), {}, {}, table);
            expect(static_cast<bool>(created), "WPJ-17: installed passive source session creates");
            if (!created) continue;
            auto session = std::move(created).value();
            expect(static_cast<bool>(session.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})),
                "WPJ-17: installed bomber orders a missile attack");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            bool changed = false;
            std::size_t fired = 0, live_torpedo_frames = 0;
            // WWP-33: spawn readiness spans the initial eight-second recharge.
            // Include that complete interval and two seconds of actual flight.
            for (std::size_t frame = 0; frame < 300; ++frame) {
                auto step = session.step(executor);
                expect(static_cast<bool>(step), "WPJ-17: installed missile shield tick succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
                for (const auto& event : step.value().snapshot->combat_events()) {
                    fired += event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1;
                }
                for (const auto& projectile : session.projectiles()) {
                    live_torpedo_frames += projectile.shooter == 1 && projectile.homing;
                }
                if (defended && workers == 1) defended_trace.emplace_back(session.projectiles().begin(), session.projectiles().end());
                if (!defended && frame < defended_trace.size()) {
                    for (const auto& projectile : session.projectiles()) {
                        const auto& before = defended_trace[frame];
                        const auto matching = std::find_if(before.begin(), before.end(), [&](const auto& p) { return p.id == projectile.id; });
                        changed = changed || (matching != before.end() && projectile.homing && matching->step != projectile.step);
                    }
                }
            }
            expect(fired > 0 && live_torpedo_frames > 0, "WPJ-17: installed torpedo fires and remains in flight");
            std::cout << "passive-shield," << workers << ',' << defended << ',' << fired << ','
                << live_torpedo_frames << ',' << changed << '\n';
            if (defended) {
                if (workers == 1) reference = hashes;
                else expect(hashes == reference, "WPJ-17: installed source flight hashes equal at 1/2/4/8 workers");
            } else expect(changed, "WPJ-17: production passive satellite changes real torpedo flight");
        }
    }
}

// WLS-01..05: the installed named handler binds its admission, service and presentation fields.
void foc_leech_shields(eawr::units::LoadInput input) {
    namespace t = eawr::sim::tactical;
    input.types = {"Kedalbe_Battleship"}; input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WLS-01: installed Keldabe loads");
    if (!loaded) return;
    const auto table = eawr::units::ability_table(loaded.value(), std::vector<t::PlayerId>{1});
    expect(static_cast<bool>(table), "WLS-01: Keldabe ordinary/nested abilities bind");
    if (!table) return;
    const auto* profile = table.value().find(eawr::assets::object_type_crc("Kedalbe_Battleship"));
    const auto slot = profile ? t::ability_slot(*profile, t::AbilityKind::leech_shields) : std::nullopt;
    expect(slot.has_value(), "WLS-01: black-market authorship does not gate skirmish");
    if (!slot) return;
    const auto& ability = profile->abilities[*slot];
    expect(ability.expiration_frames == 750 && ability.recharge_frames == 1650 && ability.recharge_at_activation
        && ability.supports_autofire, "WLS-03: installed duration, activation recharge and autofire flag");
    const auto owner = std::find_if(loaded.value().units.begin(), loaded.value().units.end(), [](const auto& unit) {
        return unit.id == "Kedalbe_Battleship";
    });
    expect(ability.requires_hardpoint && ability.required_hardpoints.size() == 1 && owner != loaded.value().units.end()
        && ability.required_hardpoints.front() < owner->hardpoints.size()
        && owner->hardpoints[ability.required_hardpoints.front()].id == "HP_KEDALBE_SHIELD_LEECH_00",
        "WHD-01: stock leech resolves its SPECIAL dependency");
    const auto handler = std::find_if(profile->special.begin(), profile->special.end(), [](const auto& nested) {
        return nested.kind == t::SpecialAbilityKind::leech_shields;
    });
    expect(handler != profile->special.end(), "WLS-01: named handler resolves");
    if (handler == profile->special.end()) return;
    expect(handler->style == t::SpecialActivationStyle::user_input && handler->initially_enabled != false
        && handler->filter.applicable_categories != 0, "WLS-01: installed target handler is enabled and filtered");
    expect(handler->beam_min_range == Fixed::from_raw(raw(10)) && handler->beam_max_range == Fixed::from_raw(raw(2000))
        && handler->damage_per_frame == Fixed::from_raw((raw(100) + 15) / 30)
        && handler->leech_damage_bonus == Fixed::from_raw(raw(3)), "WLS-01/02: installed ranges, shield drain and damage contribution");
    expect(handler->leech_beam_frames == 10 && handler->leech_texture == "tractor_beam00.tga"
        && handler->leech_effect == "Leech_Effect", "WLS-05: authored beam and target effect bind");
}

// WLU-01/04/08: the installed A-wing binds LURE's pulse, its listed types and the damage share; the
// squadron's own entry adds no profile (AB-15).
void foc_lure(eawr::units::LoadInput input) {
    namespace t = eawr::sim::tactical;
    input.types = {"A_Wing_Squadron"}; input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WLU-01: installed A-wing squadron loads");
    if (!loaded) return;
    const auto table = eawr::units::ability_table(loaded.value(), std::vector<t::PlayerId>{1});
    expect(static_cast<bool>(table) && static_cast<bool>(t::validate_abilities(table.value())), "WLU-01: A-wing LURE binds and validates");
    if (!table) return;
    const auto* team = table.value().find(eawr::assets::object_type_crc("A_Wing_Squadron"));
    expect(team == nullptr || !t::ability_slot(*team, t::AbilityKind::lure), "WLU-01: the squadron entry is not a team ability");
    const auto* profile = table.value().find(eawr::assets::object_type_crc("A-Wing"));
    const auto slot = profile ? t::ability_slot(*profile, t::AbilityKind::lure) : std::nullopt;
    expect(slot.has_value(), "WLU-01: the A-wing craft holds LURE");
    if (!slot) return;
    const auto& ability = profile->abilities[*slot];
    expect(ability.expiration_frames == 480 && ability.recharge_frames == 1350 && !ability.supports_autofire,
        "WLU-02: installed 16 s limit and 45 s recharge, no autofire");
    expect(ability.modifiers.take_damage == Fixed::from_decimal("0.7").value() && ability.modifiers.cause_damage == Fixed::from_raw(raw(1)),
        "WLU-08: the installed damage share is 0.7");
    expect(ability.lure && ability.lure->radius == Fixed::from_raw(raw(350)) && ability.lure->pulse_frames == 15
        && ability.lure->max_pulses == 40 && ability.lure->particle == "Lure_Ability_Particle",
        "WLU-03/09: installed radius, pulse period and count, particle");
    if (!ability.lure) return;
    const auto& types = ability.lure->target_types;
    for (const auto* name : {"TIE_Fighter", "TIE_Fighter_Squadron", "TIE_Interceptor", "Rebel_X-Wing_Squadron", "A_Wing_Squadron",
             "StarViper_Squadron", "Skipray_Squadron", "Pirate_Fighter_Squadron", "Z95_Headhunter_Rebel_Squadron"}) {
        expect(std::binary_search(types.begin(), types.end(), eawr::assets::object_type_crc(name)),
            std::string("WLU-04: installed target types list ") + name);
    }
    expect(!std::binary_search(types.begin(), types.end(), eawr::assets::object_type_crc("TIE_Bomber_Squadron"))
        && types.size() == 23, "WLU-04: bombers are not listed; the 23 authored names are kept");
}

void foc_corrupt_systems(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"IG-2000"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WHE-33: installed IG-2000 loads");
    if (!loaded) return;
    const auto abilities = eawr::units::ability_table(loaded.value(), std::vector<tactical::PlayerId>{1});
    expect(static_cast<bool>(abilities), "WHE-33: IG-2000's named handler binds");
    if (!abilities) return;
    const auto* profile = abilities.value().find(eawr::assets::object_type_crc("IG-2000"));
    const auto slot = profile ? tactical::ability_slot(*profile, tactical::AbilityKind::corrupt_systems) : std::nullopt;
    expect(slot.has_value(), "WHE-33: IG-2000's corrupt systems is exposed");
    if (!slot) return;
    const auto& ability = profile->abilities[*slot];
    expect(ability.recharge_frames == 1800 && ability.effective_radius == Fixed::from_raw(raw(400))
        && ability.gui_activated_ability_name == "IG2000_Corrupt_Systems", "WHE-33: XML supplies area radius and sixty-second recharge");
    const auto handler = std::find_if(profile->special.begin(), profile->special.end(), [](const auto& special) {
        return special.kind == tactical::SpecialAbilityKind::corrupt_systems;
    });
    expect(handler != profile->special.end(), "WHE-33: nested handler exists");
    if (handler == profile->special.end()) return;
    expect(handler->corrupt_min_range == Fixed::from_raw(raw(20)) && handler->corrupt_max_range == Fixed::from_raw(raw(350))
        && handler->corrupt_duration_frames == 360 && handler->corrupt_drain_per_frame == Fixed::from_raw(raw(2))
        && handler->corrupt_inaccuracy == Fixed::from_raw(raw(500)), "WHE-33/34/66: nested XML supplies ranges, duration, drain and inaccuracy");
    expect(handler->corrupt_target_particle == "Corrupt_Systems_Effect" && handler->corrupt_owner_particle == "Corrupt_Systems_Flare_Effect"
        && handler->corrupt_activate_sfx == "Unit_IG88_Corrupt_Systems", "WHE-33: authored particles and activation sound bind");
}

} // namespace unit_tables_test_support
