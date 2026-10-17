#include "unit_tables_support.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/presentation/ui/ability_buttons.hpp"
#include "eawr/presentation/ui/command_sink.hpp"

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

// WHZ-50/51, RO-2: the map's Hutt resource containers are live combat recipients.
void foc_resource_container(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"TIE_Bomber_Squadron", "TIE_Fighter_Squadron", "Tartan_Patrol_Cruiser", "Nebulon_B_Frigate", "Corellian_Corvette"};
    input.obstacles = {"Orbital_Resource_Container", "Skirmish_Merchant_Dock"};
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WHZ-50: damageable map objects load as live types");
    if (!loaded) return;
    const auto* container = loaded.value().find("Orbital_Resource_Container");
    expect(container && container->hull == Fixed::from_decimal("20").value()
        && container->living_projectile_collision && !container->has_space_evaluator,
        "RO-2: resource container retains authored hull, collision and evaluator admission");
    expect(container && container->ranged_target_z_adjust == Fixed::from_raw(raw(30)),
        "WWP-72: installed resource container retains its authored aim offset");
    const auto combat = eawr::units::combat_table(loaded.value());
    const auto durability = eawr::units::durability_table(loaded.value());
    const auto motion = eawr::units::motion_table(loaded.value());
    expect(combat && durability && motion, "WHZ-50: damageable map combat, durability and motion bind");
    if (!combat || !durability || !motion) return;
    const auto target = eawr::assets::object_type_crc("Orbital_Resource_Container");
    expect(combat.value().find(target) != nullptr && durability.value().find(target) != nullptr,
        "RO-2: a non-capture map object has target and damage profiles");
    expect(motion.value().find(target) == nullptr && container && container->footprint.space_obstacle,
        "WHZ-50: the admitted container retains its obstacle footprint without a locomotor");
    const auto* payload_profile = combat.value().find(target);
    expect(payload_profile && payload_profile->death_projectiles.size() == 1
        && payload_profile->ranged_target_z_adjust == Fixed::from_decimal("30").value(),
        "WNO-29/30: installed container resolves one death payload at authored height");
    if (payload_profile && !payload_profile->death_projectiles.empty()) {
        const auto& shot = payload_profile->death_projectiles.front();
        expect(shot.damage == Fixed{} && shot.blast.damage == Fixed::from_decimal("3000").value()
            && shot.blast.radius == Fixed::from_decimal("400").value()
            && shot.damage_delay == Fixed::from_decimal("0.5").value()
            && shot.blast.immune_faction == eawr::assets::object_type_crc("Rebel"),
            "WNO-30/32: installed detonation retains typed damage/radius/immunity/delay");
    }
    auto death_combat = combat.value();
    for (auto& profile : death_combat.profiles) if (!profile.planetary_weapon) profile.weapons.clear();
    tactical::TacticalSetup death_setup;
    death_setup.seed = 1774;
    death_setup.players = {{1, 1, eawr::assets::object_type_crc("Empire"), tactical::player_flag_commandable},
        {2, 1, eawr::assets::object_type_crc("Rebel"), tactical::player_flag_commandable},
        {7, 7, eawr::assets::object_type_crc("Hutts"), 0}};
    death_setup.units = {{1, target, 7, {}},
        {2, eawr::assets::object_type_crc("Tartan_Patrol_Cruiser"), 1, point(raw(150), raw(50), 0)},
        {3, eawr::assets::object_type_crc("Corellian_Corvette"), 2, point(raw(150), raw(-50), 0)}};
    auto death_session = tactical::TacticalSession::create(death_setup, {}, durability.value(), motion.value(),
        std::nullopt, death_combat);
    expect(static_cast<bool>(death_session), "WNO-31: installed container-death witness starts");
    if (death_session) {
        const auto empire_before = death_session.value().durability_state(2);
        const auto rebel_before = death_session.value().durability_state(3);
        const auto source_before = death_session.value().durability_state(1);
        expect(source_before.has_value(), "WNO-29: stock container has scaled runtime health");
        const auto lethal = source_before ? source_before->hull : Fixed{};
        expect(static_cast<bool>(death_session.value().submit({{0, 1, 0}, {1}, tactical::DamagePayload{lethal}})),
            "WNO-29: installed container ordinary death is submitted");
        eawr::sim::InlineExecutor executor;
        std::uint64_t detonations{};
        for (int frame = 0; frame < 40; ++frame) {
            const auto step = death_session.value().step(executor);
            expect(static_cast<bool>(step), "WNO-31: installed container-death witness steps");
            if (!step) break;
            detonations += step.value().blast_detonations;
        }
        const auto empire_after = death_session.value().durability_state(2);
        const auto rebel_after = death_session.value().durability_state(3);
        expect(detonations == 1 && !death_session.value().durability_state(1),
            "WNO-29/31: removed installed container produces exactly one detonation");
        expect(empire_before && (!empire_after || empire_after->hull < empire_before->hull
            || empire_after->shields < empire_before->shields || empire_after->hardpoints != empire_before->hardpoints),
            "WNO-31: nearby installed non-Rebel ship receives qualifying damage");
        expect(rebel_before && rebel_after && rebel_after->hull == rebel_before->hull
            && rebel_after->shields == rebel_before->shields && rebel_after->hardpoints == rebel_before->hardpoints,
            "WNO-31: nearby allied Rebel ship remains untouched");
    }
    const auto print_point = [](const Vec3& p) {
        std::cout << ',' << p.x.raw() << ',' << p.y.raw() << ',' << p.z.raw();
    };
    const auto* target_profile = combat.value().find(target);
    expect(target_profile && target_profile->ranged_target_z_adjust == Fixed::from_raw(raw(30)),
        "WWP-72: installed aim offset reaches the resource combat profile");
    if (target_profile) {
        for (const auto& mesh : target_profile->meshes) {
            for (const auto& triangle : mesh.triangles) {
                std::cout << "resource-mesh";
                print_point(triangle.a); print_point(triangle.b); print_point(triangle.c);
                std::cout << '\n';
            }
        }
    }
    for (const auto& team_name : {"TIE_Bomber_Squadron", "TIE_Fighter_Squadron", "Tartan_Patrol_Cruiser", "Nebulon_B_Frigate"}) {
        const auto* team = loaded.value().find(team_name);
        expect(team != nullptr, "RO-2: the attacking type loads");
        if (!team) continue;
        const auto shooter = eawr::assets::object_type_crc(team_name);
        for (const bool sustained : {false, true}) {
            const auto trace_name = std::string(team_name) + (sustained ? ":sustained" : ":stock");
            auto health = durability.value();
            // Diagnostic fixture only: retain the installed geometry and weapons while preventing
            // 20-HP overkill from turning later arrivals into apparent aiming misses.
            if (sustained) {
                for (auto& profile : health.profiles) {
                    if (profile.type_id == target) profile.max_hull = Fixed::from_raw(raw(100000));
                }
            }
            std::vector<std::string> reference;
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                tactical::TacticalSetup setup;
                setup.seed = 1728;
                setup.players = {{1, 1, eawr::assets::object_type_crc("Empire"), tactical::player_flag_commandable},
                    {7, 7, eawr::assets::object_type_crc("Hutts"), 0}};
                setup.units = {{1, shooter, 1, Vec3{}, eawr::sim::math::identity_quat(), {}},
                    {2, target, 7, point(raw(400), 0, 0), eawr::sim::math::identity_quat(), {}}};
                if (!team->members.empty()) setup.squadrons = {{1, {}}};
                std::vector<tactical::SensorProfile> sensors{{shooter, Fixed::from_raw(raw(2000))}};
                for (const auto& member : team->members) {
                    const auto craft_type = eawr::assets::object_type_crc(member.craft);
                    sensors.push_back({craft_type, Fixed::from_raw(raw(2000))});
                    const auto craft_id = static_cast<eawr::sim::EntityId>(setup.units.size() + 1);
                    setup.units.push_back({craft_id, craft_type, 1, Vec3{}, eawr::sim::math::identity_quat(), {}});
                    setup.squadrons.front().members.push_back(craft_id);
                }
                std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
                sensors.erase(std::unique(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) {
                    return a.type_id == b.type_id;
                }), sensors.end());
                auto session = tactical::TacticalSession::create(setup, sensors, health, motion.value(), std::nullopt, combat.value());
                expect(static_cast<bool>(session), "RO-2: Hutt resource firing world starts");
                if (!session) continue;
                expect(static_cast<bool>(session.value().submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})),
                    "RO-2: submit an ordinary attack on the Hutt container");
                std::size_t shots = 0;
                std::size_t hits = 0;
                std::vector<std::string> hashes;
                const eawr::platform::ThreadWorkerAdapter executor(workers);
                // WWP-33: allow the authored bomber opening recharge and its approach.
                for (std::uint64_t tick = 0; tick < 600; ++tick) {
                    auto result = session.value().step(executor);
                    expect(static_cast<bool>(result), "RO-2: Hutt resource firing step succeeds");
                    if (!result) break;
                    hashes.push_back(result.value().state_sha256);
                    if (workers == 1) {
                        for (const auto& projectile : result.value().snapshot->projectiles()) {
                            if (projectile.target != 2) continue;
                            std::cout << "resource-path," << trace_name << ',' << tick << ',' << projectile.id
                                << ',' << projectile.shooter << ',' << projectile.weapon;
                            print_point(projectile.position); print_point(projectile.step);
                            std::cout << '\n';
                        }
                    }
                    for (const auto& event : result.value().snapshot->combat_events()) {
                        if (event.kind == tactical::CombatEventKind::projectile_expired && workers == 1
                            && event.selected_target == 2) {
                            std::cout << "resource-expired," << trace_name << ',' << tick << ',' << event.target;
                            print_point(event.aim); std::cout << '\n';
                        }
                        if (event.target != 2 || event.shooter == 2) continue;
                        shots += event.kind == tactical::CombatEventKind::weapon_fired ? 1U : 0U;
                        hits += event.kind == tactical::CombatEventKind::projectile_hit ? 1U : 0U;
                        if (workers == 1 && (event.kind == tactical::CombatEventKind::weapon_fired
                            || event.kind == tactical::CombatEventKind::projectile_hit)) {
                            std::cout << "resource-event," << trace_name << ',' << tick << ',' << tactical::to_string(event.kind)
                                << ',' << event.shooter << ',' << event.weapon;
                            print_point(event.origin); print_point(event.aim); std::cout << '\n';
                        }
                    }
                }
                const auto flying = static_cast<std::size_t>(std::count_if(session.value().projectiles().begin(),
                    session.value().projectiles().end(), [](const auto& projectile) { return projectile.target == 2; }));
                std::cout << "resource-attack," << trace_name << ',' << workers << ',' << shots << ',' << hits << ',' << flying << '\n';
                const auto remaining = session.value().durability_state(2);
                std::cout << "resource-health," << trace_name << ',' << workers << ','
                    << (remaining ? remaining->hull.raw() : 0) << '\n';
                if (sustained) expect(remaining && remaining->hull.raw() > 0,
                    "RO-2: the diagnostic target remains alive for the entire aiming measurement");
                expect(shots > 0, std::string("RO-2: an admitted container attack actually fires: ") + team_name);
                // Survey the existing aiming behavior; this contract also runs before the height fix.
                if (std::string_view(team_name) == "TIE_Fighter_Squadron") {
                    expect(hits > 0, "RO-2: fighter projectiles hit the admitted container");
                }
                if (workers == 1) reference = hashes;
                else expect(hashes == reference, "RO-2: resource firing hashes equal on 1/2/4/8 workers");
            }
        }
    }
}

// #1873, WNO-09/10: automatic fire never picks the NotOpportunityTarget container through a
// priority set. Every stock shooter with a set idles, guards and attack-moves next to one without
// firing; a parent without a set (R-09) fires at it; an explicit attack still destroys it.
void foc_container_not_opportunity(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    const std::vector<std::string> shooters{"TIE_Fighter_Squadron", "TIE_Bomber_Squadron", "TIE_Interceptor_Squadron",
        "Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Victory_Destroyer", "Broadside_Class_Cruiser", "Star_Destroyer",
        "Rebel_X-Wing_Squadron", "Y-Wing_Squadron", "Corellian_Corvette", "Nebulon_B_Frigate", "Calamari_Cruiser",
        "StarViper_Squadron", "Crusader_Gunship", "Interceptor4_Frigate", "Vengeance_Frigate", "IG-2000", "Slave_I",
        "Skirmish_Empire_Star_Base_1", "Skirmish_Rebel_Star_Base_1", "Skirmish_Underworld_Star_Base_1",
        "Empire_Defense_Satellite_Missile"};
    input.types = shooters;
    input.obstacles = {"Orbital_Resource_Container"};
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "#1873: shooter and container tables load");
    if (!loaded) return;
    const auto combat = eawr::units::combat_table(loaded.value());
    const auto durability = eawr::units::durability_table(loaded.value());
    const auto motion = eawr::units::motion_table(loaded.value());
    expect(combat && durability && motion, "#1873: shooter and container tables bind");
    if (!combat || !durability || !motion) return;
    const auto target = eawr::assets::object_type_crc("Orbital_Resource_Container");
    const std::set<std::string> explicit_kills{"TIE_Fighter_Squadron", "Tartan_Patrol_Cruiser", "Corellian_Corvette",
        "Rebel_X-Wing_Squadron", "Crusader_Gunship", "StarViper_Squadron"};
    // R-09 (AU-06..09): these parents author no priority set, so every candidate scores 1.0 and
    // the container is an ordinary automatic target for them, as in FoC (owner's Slave I report).
    const std::set<std::string> setless_fire{"Slave_I", "IG-2000", "Skirmish_Empire_Star_Base_1",
        "Empire_Defense_Satellite_Missile"};
    enum class Order { idle, guard, attack_move, attack };
    for (const auto& name : shooters) {
        const auto* team = loaded.value().find(name);
        expect(team != nullptr, "#1873: the shooter type loads: " + name);
        if (team == nullptr) continue;
        const auto shooter = eawr::assets::object_type_crc(name);
        for (const auto order : {Order::idle, Order::guard, Order::attack_move, Order::attack}) {
            const char* order_name = order == Order::idle ? "idle" : order == Order::guard ? "guard"
                : order == Order::attack_move ? "attack-move" : "attack";
            tactical::TacticalSetup setup;
            setup.seed = 1873;
            setup.players = {{1, 1, eawr::assets::object_type_crc("Empire"), tactical::player_flag_commandable},
                {7, 7, eawr::assets::object_type_crc("Hutts"), 0}};
            setup.units = {{1, shooter, 1, Vec3{}, eawr::sim::math::identity_quat(), {}},
                {2, target, 7, point(raw(300), 0, 0), eawr::sim::math::identity_quat(), {}}};
            if (!team->members.empty()) setup.squadrons = {{1, {}}};
            std::vector<tactical::SensorProfile> sensors{{shooter, Fixed::from_raw(raw(4000))}};
            for (const auto& member : team->members) {
                const auto craft_type = eawr::assets::object_type_crc(member.craft);
                sensors.push_back({craft_type, Fixed::from_raw(raw(4000))});
                const auto craft_id = static_cast<eawr::sim::EntityId>(setup.units.size() + 1);
                setup.units.push_back({craft_id, craft_type, 1, Vec3{}, eawr::sim::math::identity_quat(), {}});
                setup.squadrons.front().members.push_back(craft_id);
            }
            std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
            sensors.erase(std::unique(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) {
                return a.type_id == b.type_id;
            }), sensors.end());
            auto session = tactical::TacticalSession::create(setup, sensors, durability.value(), motion.value(),
                std::nullopt, combat.value());
            expect(static_cast<bool>(session), "#1873: container world starts: " + name);
            if (!session) continue;
            const bool mobile = motion.value().find(shooter) != nullptr || !team->members.empty();
            if (order == Order::guard && mobile) {
                expect(static_cast<bool>(session.value().submit({{0, 1, 0}, {1}, tactical::GuardPayload{point(raw(200), raw(100), 0)}})),
                    "#1873: guard order submits: " + name);
            } else if (order == Order::attack_move && mobile) {
                expect(static_cast<bool>(session.value().submit({{0, 1, 0}, {1}, tactical::AttackMovePayload{point(raw(900), raw(150), 0)}})),
                    "#1873: attack-move order submits: " + name);
            } else if (order == Order::attack) {
                expect(static_cast<bool>(session.value().submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})),
                    "#1873: explicit attack submits: " + name);
            } else if (order != Order::idle) {
                continue; // a station takes no move-type order
            }
            std::size_t shots = 0;
            std::size_t hits = 0;
            std::set<eawr::sim::EntityId> sources;
            eawr::sim::InlineExecutor executor;
            for (std::uint64_t tick = 0; tick < 450; ++tick) {
                auto result = session.value().step(executor);
                expect(static_cast<bool>(result), "#1873: container world steps: " + name);
                if (!result) break;
                for (const auto& event : result.value().snapshot->combat_events()) {
                    if (event.target != 2 || event.shooter == 2) continue;
                    if (event.kind == tactical::CombatEventKind::weapon_fired) {
                        ++shots;
                        sources.insert(event.shooter);
                    }
                    hits += event.kind == tactical::CombatEventKind::projectile_hit ? 1U : 0U;
                }
            }
            const auto remaining = session.value().durability_state(2);
            std::cout << "container-opportunity," << name << ',' << order_name << ',' << shots << ',' << hits << ','
                      << (remaining ? remaining->hull.raw() : 0) << '\n';
            if (order == Order::attack) {
                // WNO-10: an explicit object order still fires on the container and destroys it.
                if (explicit_kills.contains(name)) {
                    expect(shots > 0 && hits > 0 && !remaining, "WNO-10: an explicit attack destroys the container: " + name);
                }
                continue;
            }
            // R-09 (AU-06..09): a parent without a priority set scores every candidate alike;
            // only the set's property exclusion keeps the container out of automatic fire.
            bool setless = team->members.empty() && team->targeting_priority_set.empty();
            for (const auto& member : team->members) {
                const auto* craft = loaded.value().find(member.craft);
                setless = setless || craft == nullptr || craft->targeting_priority_set.empty();
            }
            if (setless) {
                if (order == Order::idle && setless_fire.contains(name)) {
                    expect(shots > 0, "R-09: a parent without a priority set fires at the container on its own: " + name);
                }
                if (order == Order::idle && name == "Slave_I") {
                    // WNO-29..32, WAD-09: the Rebel-immune 3000 x 400 payload kills the Empire
                    // Slave I that shot the container from 300 away (150 shield, 1500 hull).
                    expect(!remaining && !session.value().durability_state(1),
                        "WNO-31: the container's death payload destroys its nearby non-Rebel shooter: Slave_I");
                }
                continue;
            }
            expect(!setless_fire.contains(name), "R-09: the set-less list names only set-less shooters: " + name);
            expect(shots == 0 && hits == 0 && remaining && remaining->hull == durability.value().find(target)->max_hull,
                std::string("WNO-09/10: no automatic fire at the container: ") + name + ' ' + order_name);
        }
    }
}

void foc_living_collision(eawr::units::LoadInput input) {
    input.types = {"Y-Wing_Squadron", "Darth_Vader_TIE_Fighter_Squadron", "StarViper_Squadron",
        "RS_Increased_Supplies_L1_Upgrade", "UL_Extort_Cash_L1_Upgrade"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WBP-51: stock team/craft/upgrade admission closure loads");
    if (!loaded) return;
    const auto combat = eawr::units::combat_table(loaded.value());
    expect(static_cast<bool>(combat), "WAD-14/18: stock admission binds combat profiles");
    if (!combat) return;
    bool craft_seen = false;
    bool upgrade_seen = false;
    for (const auto& unit : loaded.value().units) {
        if (unit.kind == UnitKind::squadron) {
            expect(!unit.living_projectile_collision && !combat.value().find(eawr::assets::object_type_crc(unit.id)),
                "WBP-51/WAD-18: stock team is absent from collision and craft damage: " + unit.id);
            for (const auto& member : unit.members) {
                const auto* craft = loaded.value().find(member.craft);
                expect(craft && craft->living_projectile_collision,
                    "WBP-51: every stock member craft independently opts in: " + member.craft);
                craft_seen = true;
            }
        } else if (unit.xml_type == "UpgradeObject") {
            expect(!unit.living_projectile_collision, "WBP-51: a stock upgrade never becomes a capture/blast recipient: " + unit.id);
            upgrade_seen = true;
        }
    }
    expect(craft_seen && upgrade_seen, "WBP-51: installed contract exercises craft and live upgrades");
}

// #384: Corellian_Gunboat's HP_Corellian_Gunship_04 fires along MuzzleA_03, whose z axis is
// horizontal. A TIE fighter on that axis from the weapon midpoint is at the cone's pole: the tick
// must not fail, and the 45-degree cone height leaves its 90-degree pitch outside (W-07). Its
// combat table is validated too, fire frames included.
void foc_gunboat_pole(eawr::units::LoadInput input) {
    namespace tactical = eawr::sim::tactical;
    input.types = {"Corellian_Gunboat", "TIE_Fighter"};
    input.obstacles.clear();
    auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "gunboat tables load");
    if (!loaded) return;
    auto combat = eawr::units::combat_table(loaded.value());
    expect(static_cast<bool>(combat), "the gunboat's combat table (fire frames) validates");
    if (!combat) return;
    const auto* gunboat_type = loaded.value().find("Corellian_Gunboat");
    const auto* gunboat = combat.value().find(eawr::assets::object_type_crc("Corellian_Gunboat"));
    expect(gunboat_type != nullptr && gunboat != nullptr, "gunboat profile");
    if (gunboat_type == nullptr || gunboat == nullptr) return;
    std::optional<std::uint32_t> slot;
    for (std::uint32_t index = 0; index < gunboat->weapons.size(); ++index) {
        const auto hardpoint = gunboat->weapons[index].hardpoint;
        if (hardpoint < gunboat_type->hardpoints.size() && gunboat_type->hardpoints[hardpoint].id == "HP_Corellian_Gunship_04") {
            slot = index;
        }
    }
    expect(slot && gunboat->weapons[*slot].fire_axes.has_value(), "HP_Corellian_Gunship_04 has a fire frame");
    if (!slot || !gunboat->weapons[*slot].fire_axes) return;
    const auto& weapon = gunboat->weapons[*slot];
    // The loaded axes are orthonormal only to a few raw units, so 300 x z misses the Q24 pole;
    // 300 x (x cross y) is where both the x and y projections round to zero.
    const auto pole = eawr::sim::math::cross((*weapon.fire_axes)[0], (*weapon.fire_axes)[1]);
    expect(static_cast<bool>(pole), "the gunboat frame's pole");
    if (!pole) return;
    const auto on_pole = [&](const Fixed mid_a, const Fixed mid_b, const Fixed axis) {
        return Fixed::from_raw((mid_a.raw() + mid_b.raw()) / 2 + 300 * axis.raw());
    };
    const Vec3 target{on_pole(weapon.fire_a.x, weapon.fire_b.x, pole.value().x),
        on_pole(weapon.fire_a.y, weapon.fire_b.y, pole.value().y), on_pole(weapon.fire_a.z, weapon.fire_b.z, pole.value().z)};

    tactical::TacticalSetup setup;
    setup.seed = 384;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{1, gunboat->type_id, 1, Vec3{}, eawr::sim::math::identity_quat(), {}},
        {2, eawr::assets::object_type_crc("TIE_Fighter"), 2, target, eawr::sim::math::identity_quat(), {}}};
    std::vector<tactical::SensorProfile> sensors{
        {setup.units[0].type_id, Fixed::from_raw(Fixed::scale * 2000)}, {setup.units[1].type_id, Fixed::from_raw(Fixed::scale * 2000)}};
    std::sort(sensors.begin(), sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    auto session = tactical::TacticalSession::create(setup, sensors, {}, {}, std::nullopt, combat.value());
    expect(static_cast<bool>(session), "gunboat session is created");
    if (!session) {
        std::cerr << eawr::core::format_diagnostic(session.error()) << '\n';
        return;
    }
    const eawr::sim::InlineExecutor executor;
    bool pole_shot = false;
    for (int tick = 0; tick < 150; ++tick) {
        auto stepped = session.value().step(executor);
        expect(static_cast<bool>(stepped), "a target on HP_Corellian_Gunship_04's bone z axis does not fail the step");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            pole_shot = pole_shot || (event.kind == tactical::CombatEventKind::weapon_fired && event.weapon == *slot);
        }
    }
    expect(!pole_shot, "HP_Corellian_Gunship_04's 45-degree cone height does not reach its pole");
}

// WWP-47/48: the X-wing's four wingtip muzzle bones in firing order, and its shot's travel, its
// Targeting_Max_Attack_Distance (450) rather than the laser's Projectile_Max_Flight_Distance (500).
void foc_fighter_muzzles(eawr::units::LoadInput input) {
    input.types = {"X-Wing", "TIE_Fighter"};
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WWP-47: the stock fighters load");
    if (!loaded) return;
    const auto* xwing = loaded.value().find("X-Wing");
    expect(xwing != nullptr && xwing->muzzle_bones.size() == 4,
        "WWP-47: the X-wing has the four consecutive wingtip muzzle bones MuzzleA_00 to _03");
    if (xwing != nullptr && xwing->muzzle_bones.size() == 4) {
        for (std::size_t index = 0; index < 4; ++index) {
            expect(xwing->muzzle_bones[index].position.has_value(), "WWP-47: each X-wing muzzle bone has a bind position");
        }
    }
    const auto combat = eawr::units::combat_table(loaded.value());
    expect(static_cast<bool>(combat), "WWP-47: the stock fighters bind combat");
    if (!combat) return;
    const auto* profile = combat.value().find(eawr::assets::object_type_crc("X-Wing"));
    expect(profile != nullptr && !profile->weapons.empty() && profile->weapons.back().hardpoint == eawr::sim::tactical::object_weapon
            && profile->weapons.back().muzzles.size() == 4 && profile->weapons.back().shot
            && profile->weapons.back().shot->max_travel == Fixed::from_raw(raw(450)),
        "WWP-47/48: the X-wing's own weapon fires from four muzzles and travels its attack distance");
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

void foc_asteroid_contacts(const eawr::units::UnitTables& tables,
    const eawr::skirmish::SkirmishStart& start, const std::string_view map_name) {
    namespace tactical = eawr::sim::tactical;
    const auto motion = eawr::units::motion_table(tables);
    const auto health = eawr::units::durability_table(tables);
    const auto combat = eawr::units::combat_table(tables);
    expect(motion && health && combat, "WHZ-10..14: stock map service content binds");
    if (!motion || !health || !combat) return;
    const auto frigate = eawr::assets::object_type_crc("Nebulon_B_Frigate");
    const auto* footprint = motion.value().footprint(frigate);
    expect(footprint && footprint->asteroid_damage && footprint->locomotor,
        "WHZ-10: stock frigate opts into the translating-unit service");
    if (!footprint || !footprint->asteroid_damage || !footprint->locomotor) return;
    tactical::TacticalSetup setup;
    setup.seed = 924;
    setup.content_identity = eawr::units::content_identity(tables);
    setup.players = start.setup.players;
    // Use the admitted TED fields with their real type, owner, position and facing.
    for (const auto& unit : start.units) {
        const auto* field = motion.value().footprint(unit.state.type_id);
        if (unit.role == eawr::skirmish::UnitRole::map_object && field && field->asteroid_field) {
            setup.units.push_back(unit.state);
        }
    }
    expect(!setup.units.empty(), "WHZ-11: each sourced map supplies authored asteroid fields");
    if (setup.units.empty()) return;
    const auto actor = setup.units.back().entity_id + 1;
    const auto owner = std::find_if(setup.players.begin(), setup.players.end(),
        [](const auto& player) { return player.commandable(); });
    if (owner == setup.players.end()) return;
    auto position = setup.units.front().position;
    position.z = Fixed::from_raw(raw(8000)); // WHZ-11: height must not exclude a center-XY contact.
    auto destination = position;
    destination.x = eawr::sim::math::add(position.x, Fixed::from_raw(raw(1000))).value();
    setup.units.push_back({actor, frigate, owner->player_id, position, eawr::sim::math::identity_quat(), {}});
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(setup, {}, health.value(), motion.value(),
            std::nullopt, combat.value());
        expect(static_cast<bool>(created), "WHZ-10..14: stock field session validates");
        if (!created) { std::cerr << map_name << ": " << created.error().message << '\n'; continue; }
        auto& session = created.value();
        expect(static_cast<bool>(session.submit({{0, owner->player_id, 0}, {actor}, tactical::MovePayload{destination}})),
            "WHZ-10: stock frigate translation submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        bool contact = false;
        std::size_t impacts = 0;
        for (unsigned frame = 0; frame < 32; ++frame) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "WHZ-02: actual-map asteroid service advances per frame");
            if (!stepped) { std::cerr << map_name << ": " << stepped.error().message << '\n'; break; }
            hashes.push_back(stepped.value().state_sha256 + stepped.value().snapshot->sha256());
            for (const auto& instance : stepped.value().snapshot->instances()) {
                if (instance.entity_id == actor) contact = contact || instance.in_asteroid_field;
            }
            impacts += stepped.value().asteroid_impacts.size();
            for (const auto& impact : stepped.value().asteroid_impacts) {
                expect(impact.target == actor && impact.hit.kind == tactical::HitKind::asteroid
                    && std::any_of(setup.units.begin(), setup.units.end(), [&](const auto& unit) {
                        return unit.entity_id == impact.hit.source && unit.entity_id != actor;
                    }), "WHZ-12/14: stock fields produce attributed ordinary damage hits");
            }
        }
        expect(contact && impacts > 0, "WHZ-11..14: actual map fields contact and damage the moving stock frigate");
        if (!contact || impacts == 0) std::cerr << map_name << ": contact=" << contact << " impacts=" << impacts << '\n';
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WHZ-02/13: actual five-map state and snapshots agree on 1/2/4/8 workers");
    }
}

// Read-only load of the pinned FoC fleet (#64 unit table, SK-20 to SK-24).
void foc_hazard_maps(eawr::units::LoadInput input) {
    for (const auto& [name, fields, solids, nebulas, storms] : {
            std::tuple{"bespin", 5U, 0U, 0U, 0U}, std::tuple{"endor", 5U, 0U, 3U, 3U},
            std::tuple{"geonosis", 15U, 3U, 1U, 0U}, std::tuple{"kessel", 7U, 0U, 0U, 0U},
            std::tuple{"polus", 9U, 13U, 0U, 0U}}) {
        input.space_map = "data/art/maps/_mp_space_" + std::string(name) + ".ted";
        auto loaded = eawr::units::load_unit_tables(input);
        expect(static_cast<bool>(loaded), "WHZ-01: selected-map hazard profiles load");
        if (!loaded) continue;
        const auto motion = eawr::units::motion_table(loaded.value());
        const auto durability = eawr::units::durability_table(loaded.value());
        expect(durability && durability.value().damage && durability.value().damage->ion_storm_disable_seconds == Fixed::from_raw(5 * Fixed::scale),
            "WHZ-31: storm predicate window comes from the loaded constants");
        expect(motion && motion.value().nebula_disable_seconds == Fixed::from_raw(5 * Fixed::scale),
            "WHZ-21: nebula cache window comes from the loaded constants");
        if (motion) {
            for (const auto& unit : loaded.value().units) {
                const bool enabled = std::binary_search(motion.value().nebula_service_types.begin(),
                    motion.value().nebula_service_types.end(), eawr::assets::object_type_crc(unit.id));
                expect(enabled == unit.footprint.hazard.nebula_service,
                    "WHZ-20: craft and ship opt-ins follow each effective behavior list");
            }
        }
        auto map = eawr::assets::load_map(*input.filesystem, input.space_map,
                                        eawr::assets::object_type_catalog(*input.catalog));
        expect(static_cast<bool>(map), "WHZ-01: actual selected-map placements decode");
        if (!map) continue;
        std::array<unsigned, 4> counts{};
        for (const auto& placement : map.value().placements) {
            expect(placement.type_resolution == eawr::assets::TypeResolution::unique,
                "WHZ-01: every selected placement resolves uniquely");
            if (placement.type_candidates.empty()) continue;
            const auto& type = placement.type_candidates.front().logical_name;
            const auto obstacle = std::find_if(loaded.value().obstacles.begin(), loaded.value().obstacles.end(),
                [&](const auto& value) { return value.id == type; });
            if (obstacle == loaded.value().obstacles.end()) continue;
            const auto& hazard = obstacle->footprint.hazard;
            counts[0] += hazard.asteroid_field;
            counts[1] += hazard.impassable_asteroid;
            counts[2] += hazard.nebula;
            counts[3] += hazard.ion_storm;
            if (type == "Space_Junk_Small") {
                expect(hazard.asteroid_field, "WHZ-01: small junk participates through its effective flag");
            }
            if (type == "Space_Junk_Large_NO_Collision") {
                expect(!hazard.asteroid_field && !obstacle->footprint.space_obstacle
                    && obstacle->space_layer == "StaticObject",
                    "WHZ-01/ML-24: noncolliding junk inherits its layer without obstacle participation");
            }
        }
        if (counts != std::array<unsigned, 4>{fields, solids, nebulas, storms}) {
            std::cerr << "WHZ-01 " << name << ": " << counts[0] << ',' << counts[1] << ',' << counts[2]
                      << ',' << counts[3] << '\n';
        }
        expect(counts == std::array<unsigned, 4>{fields, solids, nebulas, storms},
            "WHZ-01: selected-map inventory matches sourced placement counts");
        eawr::skirmish::FixtureOptions options;
        options.map = input.space_map;
        const auto fixture = eawr::skirmish::fixture_from_options(options, *input.filesystem, *input.catalog);
        expect(static_cast<bool>(fixture), "WHZ-01: selected-map start fixture resolves");
        if (!fixture) continue;
        const auto inputs = eawr::skirmish::read_start_inputs(fixture.value(), *input.filesystem, *input.catalog, loaded.value());
        expect(static_cast<bool>(inputs), "WHZ-01: selected-map start inputs retain hazard semantics");
        if (!inputs) continue;
        const auto started = eawr::skirmish::build_start(fixture.value(), inputs.value());
        expect(static_cast<bool>(started), "WHZ-01: authored hazards join the selected-map start");
        if (!started) continue;
        unsigned admitted = 0;
        for (const auto& placement : inputs.value().placements) {
            if (!placement.space_hazard) continue;
            const auto unit = std::find_if(started.value().units.begin(), started.value().units.end(), [&](const auto& value) {
                return value.role == eawr::skirmish::UnitRole::map_object && value.record == placement.record;
            });
            expect(unit != started.value().units.end(), "WHZ-01: every flagged placed hazard is admitted");
            if (unit == started.value().units.end()) continue;
            ++admitted;
            auto position = placement.position;
            if (position && placement.layer_z_adjust) position->z = eawr::sim::math::add(position->z, *placement.layer_z_adjust).value();
            expect(position && unit->state.position == *position, "WHZ-01: hazard placement position is retained");
            expect(placement.orientation_degrees && unit->yaw_degrees == placement.orientation_degrees->z,
                "WHZ-01: hazard placement yaw is retained");
            const auto player = std::find_if(started.value().players.begin(), started.value().players.end(), [&](const auto& value) {
                return value.player.player_id == unit->state.owner;
            });
            expect(player != started.value().players.end()
                && (placement.owner_faction.empty() ? player->faction == "Neutral" : player->faction == placement.owner_faction),
                "WHZ-01: authored hazard owner maps through the existing faction rule");
        }
        expect(admitted == fields + solids + nebulas, "WHZ-01: all five-map environmental placements are admitted");
        foc_asteroid_contacts(loaded.value(), started.value(), name);
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
                            const auto offset = gun->has_fire_b ? std::min(offset_from(gun->fire_a), offset_from(gun->fire_b))
                                : offset_from(gun->fire_a);
                            expect(offset.raw() <= 64, "WWP-73: release lies on a drawn fire bone within Q24 rounding");
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

void foc_planetary_sources(const eawr::units::UnitTables& tables, eawr::units::LoadInput input) {
    // The pinned M2 closure has Rebel/Empire producers. Load all three use
    // objects explicitly without expanding that fixture's roster or identity.
    input.types = {"RS_Ion_Cannon_Use_Upgrade", "ES_Hypervelocity_Gun_Use_Upgrade", "US_Plasma_Cannon_Use_Upgrade"};
    input.obstacles.clear();
    const auto upgrades = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(upgrades), "WSW-07/08: three-faction use-upgrade closure loads");
    if (!upgrades) return;
    for (const auto& [name, consumed] : {
        std::pair{"RS_Ion_Cannon_Use_Upgrade", true}, std::pair{"ES_Hypervelocity_Gun_Use_Upgrade", true},
        std::pair{"US_Plasma_Cannon_Use_Upgrade", false}}) {
        const auto* upgrade = upgrades.value().find(name);
        expect(upgrade && upgrade->consume_after_special_fire == consumed && upgrade->special_unlocks.size() == 1
            && upgrade->special_unlocks.front().enable && !upgrade->special_unlocks.front().all_allies,
            "WSW-07/08: installed unlock and faction-specific consumption metadata: " + std::string(name));
    }
    const auto combat = eawr::units::combat_table(tables);
    const auto health = eawr::units::durability_table(tables);
    const auto motion = eawr::units::motion_table(tables);
    expect(combat && health && motion, "WSW-01: installed source content binds");
    if (!combat || !health || !motion) return;
    for (const auto* name : {"Skirmish_Rebel_Star_Base_1", "Skirmish_Empire_Star_Base_1"}) {
        const auto* station = health.value().find(eawr::assets::object_type_crc(name));
        expect(station && std::any_of(station->hardpoints.begin(), station->hardpoints.end(),
            [](const auto& point) { return point.allows_special_weapon_use; }),
            "WSW-48: installed station binds its required special-weapon hardpoint: " + std::string(name));
    }
    for (const auto& [name, index, action, recharge, count, delay] : {
        std::tuple{"Ground_Ion_Cannon", 0U, 1U, 900U, 3U, 15U},
        std::tuple{"Ground_Empire_Hypervelocity_Gun", 1U, 0U, 1350U, 10U, 3U}}) {
        const auto type = eawr::assets::object_type_crc(name);
        const auto* unit = tables.find(name);
        const auto* profile = combat.value().find(type);
        expect(unit && unit->planetary_weapon && unit->model_path.empty() && !unit->hull
            && !health.value().find(type) && !motion.value().find(type) && !motion.value().footprint(type),
            "WSW-01/02: active planetary source loads without ship/model/locomotor: " + std::string(name));
        expect(profile && profile->planetary_weapon && profile->planetary_weapon->index == index
            && profile->planetary_weapon->target_action == action && profile->planetary_weapon->recharge_frames == recharge
            && profile->planetary_weapon->pulse_delay_frames == delay && profile->weapons.size() == 1
            && profile->weapons.front().pulse_count == count && profile->weapons.front().shot,
            "WSW-14/19/21: active source firing content matches authored data: " + std::string(name));
    }
    expect(!tables.find("Space_Mode_Ground_Rebel_Ion_Cannon") && !tables.find("Space_Mode_Ground_Empire_Hypervelocity_Gun"),
        "WSW-02: commented-out source types are absent");
}

// WCC-18: the installed Nebulon-B's default set (Frigate) lists no hardpoints, so every hardpoint type
// ranks unlisted and its best hardpoint is the nearest destroyable one; its hardpoints carry their types.
void foc_hardpoint_ranks(eawr::units::LoadInput input) {
    input.types = {"Nebulon_B_Frigate"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WCC-18: the installed Nebulon-B loads");
    if (!loaded) return;
    const auto combat = eawr::units::combat_table(loaded.value());
    expect(static_cast<bool>(combat), "WCC-18: the installed Nebulon-B binds");
    if (!combat) return;
    const auto* frigate = combat.value().find(eawr::assets::object_type_crc("Nebulon_B_Frigate"));
    expect(frigate && frigate->priority_set && *frigate->priority_set < combat.value().priority_sets.size(),
        "WCC-18: the Nebulon-B has its default priority set");
    if (!frigate || !frigate->priority_set || *frigate->priority_set >= combat.value().priority_sets.size()) return;
    const auto& set = combat.value().priority_sets[*frigate->priority_set];
    expect(!set.hardpoint_ranks.empty() && std::all_of(set.hardpoint_ranks.begin(), set.hardpoint_ranks.end(),
        [](const auto& rank) { return rank == eawr::units::unlisted_priority; }),
        "WCC-18: the Frigate set lists and excludes no hardpoint type");
    expect(set.hardpoint_ranks.size() > static_cast<std::size_t>(eawr::units::HardpointType::weapon_special),
        "WCC-18: every hardpoint type has a rank");
    expect(std::any_of(frigate->hardpoints.begin(), frigate->hardpoints.end(), [](const auto& hardpoint) {
        return hardpoint.type == static_cast<std::uint8_t>(eawr::units::HardpointType::engine);
    }), "WCC-18: the Nebulon-B's engine hardpoint carries its type");
}

// WMV-42/44: installed no-layer group, including a self-represented Falcon.
void foc_formation_deceleration(eawr::units::LoadInput input) {
    namespace t = eawr::sim::tactical;
    input.types = {"Millennium_Falcon", "X-Wing", "Slave_I", "Nebulon_B_Frigate", "Corellian_Corvette"};
    input.obstacles.clear();
    const auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "WMV-44: stock formation types load");
    if (!loaded) return;
    const auto motion = eawr::units::motion_table(loaded.value());
    expect(static_cast<bool>(motion), "WMV-44: stock formation profiles bind");
    if (!motion) return;
    const auto falcon = eawr::assets::object_type_crc("Millennium_Falcon");
    const auto* profile = motion.value().squadrons.find_squadron(falcon);
    const auto* craft = motion.value().squadrons.find_craft(falcon);
    expect(profile && craft && profile->formation_deceleration == Fixed::from_decimal("0.36").value(),
        "WMV-44: standalone Falcon retains its own scaled 0.3 deceleration");
    if (!profile || !craft) return;
    const auto number = [](Fixed value) { return static_cast<double>(value.raw()) / Fixed::scale; };
    for (const bool ships : {false, true}) {
        t::TacticalReplay replay;
        replay.setup.players = {{1, 1, 1, t::player_flag_commandable}};
        const std::vector<std::string> names = ships ? std::vector<std::string>{"Corellian_Corvette", "Nebulon_B_Frigate"}
                                                   : std::vector<std::string>{"Millennium_Falcon", "X-Wing", "Slave_I"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            replay.setup.units.push_back({static_cast<eawr::sim::EntityId>(i + 1),
                eawr::assets::object_type_crc(names[i]), 1,
                point(raw(i == 0 ? -200 : i == 1 ? 0 : -350), raw(i == 0 ? -60 : i == 1 ? 60 : 120), 0),
                t::yaw_rotation(Fixed{}).value(), {}});
        }
        replay.commands.push_back({{0, 1, 0}, ships ? std::vector<eawr::sim::EntityId>{1, 2}
                                                  : std::vector<eawr::sim::EntityId>{1, 2, 3},
            t::MovePayload{point(raw(2000), 0, 0)}});
        replay.final_tick_count = 180;
        std::array<std::vector<Vec3>, 2> positions;
        for (const bool legacy : {true, false}) {
            auto table = motion.value();
            // The old routine had no deceleration ceilings. A nonbinding ceiling reproduces
            // its algebra for this finite trace without changing ordinary craft thrust.
            if (legacy) for (auto& member : table.squadrons.squadrons)
                member.formation_deceleration = Fixed::from_raw(raw(1000));
            auto session = t::TacticalSession::from_replay(replay, {}, {}, table);
            expect(static_cast<bool>(session), "WMV-44: stock group session creates");
            if (!session) return;
            const eawr::platform::ThreadWorkerAdapter executor(1);
            bool grouped = false;
            for (unsigned tick = 0; tick < 180; ++tick) {
                const auto stepped = session.value().step(executor);
                expect(static_cast<bool>(stepped), "WMV-44: stock group step succeeds");
                if (!stepped) return;
                const auto unit = session.value().units().front();
                positions[legacy ? 0 : 1].push_back(unit.position);
                if (!ships && tick == 0) {
                    const auto first = session.value().squadron_state(1);
                    const auto second = session.value().squadron_state(2);
                    const auto third = session.value().squadron_state(3);
                    grouped = first && second && first->lane && second->lane
                        && first->lane->formation == second->lane->formation && third && third->lane
                        && first->lane->formation == third->lane->formation;
                }
                if (tick % 10 == 0) {
                    std::cout << "FORMATION_TRACE " << (ships ? "layered-ships" : "standalone-Falcon")
                        << ' ' << (legacy ? "before-nonbinding" : "after") << " tick=" << tick + 1
                        << " x=" << number(unit.position.x) << " y=" << number(unit.position.y);
                    const auto mind = session.value().squadron_state(1);
                    if (mind && mind->lane) {
                        const auto& lane = *mind->lane;
                        const double dx = number(unit.position.x) - number(lane.origin.x);
                        const double dy = number(unit.position.y) - number(lane.origin.y);
                        std::cout << " ahead=" << dx * number(lane.direction.x) + dy * number(lane.direction.y) - number(lane.ahead)
                            << " side_error=" << number(lane.aside) - (dy * number(lane.direction.x) - dx * number(lane.direction.y));
                    }
                    std::cout << '\n';
                }
            }
            if (!ships) expect(grouped, "WMV-44: standalone members entered the group path");
        }
        if (ships) expect(positions[0] == positions[1], "WMV-44: layered ordinary ship move is unchanged");
        else std::cout << "FORMATION_TRACE_NOTE stock Falcon reachability checked; the correction ceiling did not bind "
            "in this 180-tick group move. Binding limits are checked by the synthetic contracts.\n";
    }
}

void foc_fleet(const bool formation_only) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "FoC fleet: skipped (set EAWR_EAW_GAME_ROOT)\n";
        return;
    }
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return;
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "FoC catalog loads");
    if (!catalog) return;
    // #793 (R-10, R-13, C-06): the installed FoC profile loads with no failed registry and no failed include.
    expect(catalog.value().health.registry_invalid == 0 && catalog.value().health.include_failed == 0 &&
               !catalog.value().health.failed(),
           ("FoC catalog load health: " + eawr::data::describe_load_health(catalog.value().health)).c_str());
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    if (formation_only) {
        foc_formation_deceleration(input);
        return;
    }
    foc_bomber_torpedo_volley(input);
    foc_passive_projectile_defence(input);
    foc_active_projectile_defence(input);
    foc_space_healing(input);
    foc_stealth(input);
    foc_laser_defence(input);
    foc_redirect_blaster(input);
    foc_super_capitals(input);
    foc_hazard_maps(input);
    foc_mass_drivers(input);
    foc_diamond_boron_cluster(input);
    foc_barrage_buttons(input);
    foc_full_salvo(input);
    foc_self_destruct(input);
    foc_corrupt_systems(input);
    foc_peacebringer_blast(input);
    foc_asymmetric_release_geometry(input);
    foc_special_without_blast(input);
    foc_leech_shields(input);
    foc_lure(input);
    foc_hardpoint_ranks(input);
    auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "FoC unit tables load");
    if (!loaded) return;
    const auto& tables = loaded.value();
    foc_planetary_sources(tables, input);
    // WWP-72: only the Nebulon-B in the pinned ship/craft roster has nonzero aim height.
    // Live map structures are checked separately by the resource-container fixture.
    for (const auto& id : eawr::units::pinned_m2_types()) {
        const auto* type = tables.find(id);
        const auto expected = id == "Nebulon_B_Frigate" ? raw(35) : 0;
        expect(type && type->ranged_target_z_adjust.value_or(Fixed{}).raw() == expected,
            "WWP-72: pinned M2 aim adjustment for " + std::string{id});
        if (!type) continue;
        for (const auto& member : type->members) {
            const auto* craft = tables.find(member.craft);
            expect(craft && craft->ranged_target_z_adjust.value_or(Fixed{}).raw() == 0,
                "WWP-72: pinned M2 member craft has zero/absent aim adjustment: " + member.craft);
        }
    }
    // WHE-04/49: all eleven authored skirmish purchases and five conversions load.
    auto hero_input = input;
    hero_input.types = {"Han_Solo_Team_Space_MP", "Sundered_Heart", "Rogue_Squadron_Space", "Home_One",
            "Boba_Fett_Team_Space_MP", "Darth_Team_Space_MP", "Admonitor_Star_Destroyer", "Accuser_Star_Destroyer",
            "Bossk_Team_Space_MP", "IG88_Team_Space_MP", "The_Peacebringer"};
    // The pinned M2 roster has Rebel/Empire stations; an explicit purchase closure
    // also covers the Underworld companies without expanding that fixture's roster.
    const auto hero_loaded = eawr::units::load_unit_tables(hero_input);
    expect(static_cast<bool>(hero_loaded), "WHE-04: three-faction hero purchase closure loads");
    for (const auto& name : hero_input.types)
        expect(hero_loaded && hero_loaded.value().find(name) != nullptr, std::string("WHE-04: menu purchase loaded: ") + name);
    for (const auto& [purchase, deployment, riders] : {
            std::tuple{"Han_Solo_Team_Space_MP", "Millennium_Falcon", false},
            std::tuple{"Boba_Fett_Team_Space_MP", "Slave_I", true},
            std::tuple{"Darth_Team_Space_MP", "Darth_Vader_TIE_Fighter_Squadron", true},
            std::tuple{"Bossk_Team_Space_MP", "HoundsTooth", false},
            std::tuple{"IG88_Team_Space_MP", "IG-2000", false}}) {
        const auto* company = hero_loaded ? hero_loaded.value().find(purchase) : nullptr;
        expect(company && company->deployed_space_type == deployment && company->creates_carried_heroes == riders,
            std::string("WHE-49: company conversion: ") + purchase);
    }
    if (hero_loaded) {
        bool concentrate_found = false;
        for (const auto& unit : hero_loaded.value().units) for (const auto& special : unit.special_abilities) {
            if (special.kind != eawr::sim::tactical::SpecialAbilityKind::concentrate_fire) continue;
            concentrate_found = true;
            expect(special.target_speed_decrease.raw() == 0,
                "WHE-25: installed FoC retains its strict zero target-speed contribution");
        }
        expect(concentrate_found, "WHE-25: installed hero closure qualifies the concentrate-fire handler");
        const auto hero_motion = eawr::units::motion_table(hero_loaded.value());
        expect(static_cast<bool>(hero_motion), "WHE-SQ-02: hero locomotors bind");
        for (const auto* name : {"Slave_I", "Millennium_Falcon", "HoundsTooth", "IG-2000"}) {
            const auto* fighter = hero_loaded.value().find(name);
            const auto id = eawr::assets::object_type_crc(name);
            expect(fighter && fighter->kind == UnitKind::craft && hero_motion
                && hero_motion.value().find(id) == nullptr && hero_motion.value().squadrons.find_craft(id)
                && hero_motion.value().squadrons.find_squadron(id),
                std::string("WHE-SQ-02: hero uses fighter flight and self-represented group: ") + name);
        }
    }
    for (const auto* name : {"Darth_Vader_TIE_Fighter_Squadron", "Rogue_Squadron_Space", "TIE_Fighter_Squadron"}) {
        const auto* team = hero_loaded ? hero_loaded.value().find(name) : nullptr;
        // The normal TIE squadron is in the pinned tables, independent of the hero closure.
        if (team == nullptr) team = tables.find(name);
        expect(team && team->homogeneous, std::string("WHE-SQ-01: one authored/default team card: ") + name);
        if (team && std::string_view{name} == "Darth_Vader_TIE_Fighter_Squadron") {
            expect(team->members.size() == 7 && team->members.front().craft == "TIE_Prototype"
                && std::all_of(team->members.begin() + 1, team->members.end(),
                    [](const auto& member) { return member.craft == "Escort_TIE_Fighter"; }),
                "WHE-SQ-01: Vader has one ordered leader and six escorts under one authored team");
        }
        if (team && std::string_view{name} == "Rogue_Squadron_Space")
            expect(team->members.size() == 6, "WHE-SQ-01: Rogue retains all six authored craft");
    }
    // EN-07/08: production and station ion weapons retain their authored projectile flags.
    const auto ion_combat = eawr::units::combat_table(tables);
    expect(std::none_of(tables.notes.begin(), tables.notes.end(), [](const auto& note) {
        return note.reason.find("WAB-36: targeted ability row skipped") != std::string::npos;
    }), "WAB-36: installed FoC keeps strict named-handler resolution without skipped rows");
    expect(static_cast<bool>(ion_combat), "ion production combat table loads");
    if (ion_combat) {
        // Keep the installed FoC corpus under the stricter stock-frame audit even though
        // W-07 admits mod-authored nonorthogonal fire frames at runtime.
        for (const auto& profile : ion_combat.value().profiles) {
            for (const auto& weapon : profile.weapons) {
                if (!weapon.fire_axes) continue;
                const auto& axes = *weapon.fire_axes;
                for (std::size_t left = 0; left < axes.size(); ++left) {
                    for (std::size_t right = left; right < axes.size(); ++right) {
                        const auto product = eawr::sim::math::dot(axes[left], axes[right]);
                        const auto expected = left == right ? Fixed::scale : 0;
                        expect(product && std::llabs(product.value().raw() - expected)
                                <= eawr::sim::tactical::frame_tolerance_raw,
                            "W-07: installed FoC fire frames still pass strict orthogonality validation");
                    }
                }
            }
        }
        for (const auto* name : {"Nebulon_B_Frigate", "Corellian_Corvette", "Skirmish_Empire_Star_Base_1", "TIE_Fighter_Squadron"}) {
            const auto* type = tables.find(name);
            expect(type && type->unit_ai && std::binary_search(ion_combat.value().unit_ai_types.begin(),
                ion_combat.value().unit_ai_types.end(), eawr::assets::object_type_crc(name)),
                std::string("WTA-29: installed UNIT_AI admission includes ship, station and runtime team: ") + name);
        }
        const auto team_id = eawr::assets::object_type_crc("TIE_Fighter_Squadron");
        const auto& companies = ion_combat.value().retaliation_team_types;
        const auto company = std::find_if(companies.begin(), companies.end(),
            [team_id](const auto& row) { return row.type_id == team_id; });
        const auto* purchase = tables.find("TIE_Fighter_Squadron");
        expect(purchase && company != companies.end() && company->category_bits == purchase->category_bits
            && !ion_combat.value().find(team_id)
            && ion_combat.value().find_target_type(team_id) == &*company,
            "WTA-29: company category metadata is bound without an active team combat profile");
        for (const auto& priorities : ion_combat.value().priority_sets) {
            expect(std::any_of(priorities.rows.begin(), priorities.rows.end(),
                [team_id](const auto& row) { return row.type_id == team_id; }),
                "WTA-29: normalized company attackers have resolved priority rows");
        }
        auto changed = tables;
        const auto admitted = std::find_if(changed.units.begin(), changed.units.end(), [](const auto& unit) { return unit.unit_ai; });
        if (admitted != changed.units.end()) {
            admitted->unit_ai = false;
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(tables),
                "WTA-29: unit-AI behavior contributes to bound content identity");
        }
        for (const auto* faction : {"Rebel", "Empire"}) for (unsigned level = 3; level <= 5; ++level) {
            const auto name = std::string("Skirmish_") + faction + "_Star_Base_" + std::to_string(level);
            const auto* station = ion_combat.value().find(eawr::assets::object_type_crc(name));
            std::size_t count = 0;
            if (station != nullptr) for (const auto& weapon : station->weapons) {
                if (!weapon.shot || !weapon.shot->energy_damage) continue;
                ++count;
                expect(!weapon.shot->hitpoint_damage && !weapon.shot->ion_stun && !weapon.shot->disable_engines_frames
                    && weapon.shot->damage.raw() == raw(20), "EN-07: station ion drains without hull damage/stun: " + name);
            }
            expect(count == level - 2, "station ion count grows from one to three: " + name);
        }
        for (const auto* name : {"B-Wing", "TIE_Defender"}) {
            const auto* craft = ion_combat.value().find(eawr::assets::object_type_crc(name));
            std::size_t count = 0;
            if (craft != nullptr) for (const auto& weapon : craft->weapons) {
                if (!weapon.shot || !weapon.shot->energy_damage) continue;
                ++count;
                expect(weapon.shot->disable_engines_frames == 600U && !weapon.shot->hitpoint_damage && !weapon.shot->ion_stun,
                    std::string("EN-08: stock small ion carries a 20-second engine disable: ") + name);
                if (weapon.ability_shot) expect(!weapon.ability_shot->disable_engines_frames && weapon.ability_shot->ion_stun,
                    "EN-08: targeted override does not inherit ordinary small-ion engine disable");
            }
            expect(count == 2, std::string("two ordinary small-ion weapons: ") + name);
        }
        auto changed_duration = tables;
        for (auto& projectile : changed_duration.projectiles) {
            if (projectile.disables_engines_when_power_drained) projectile.disable_engines_duration = Fixed::from_raw(raw(21));
        }
        expect(eawr::units::content_identity(changed_duration) != eawr::units::content_identity(tables),
            "EN-08: engine-disable duration contributes to content identity");
    }

    const std::vector<std::string> ids{"Skirmish_Rebel_Star_Base_1", "Skirmish_Empire_Star_Base_1", "Corellian_Corvette",
        "Nebulon_B_Frigate", "Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Calamari_Cruiser", "Rebel_X-Wing_Squadron",
        "Y-Wing_Squadron",
        "TIE_Interceptor_Squadron", "TIE_Fighter_Squadron", "TIE_Bomber_Squadron", "X-Wing", "Y-Wing", "TIE_Interceptor",
        "TIE_Fighter", "TIE_Bomber"};
    expect(tables.units.size() > ids.size(), "pinned fleet plus five craft and live capture/construction types");
    for (const auto name : {"Defense_Satellite_Laser_Pad", "Mineral_Extractor_Pad", "Skirmish_Merchant_Dock"}) {
        const auto* point = tables.find(name);
        expect(point != nullptr && point->capture_point && point->capture_radius && point->capture_seconds,
            "WBP-01/02: map capture type has live capture data");
        expect(point != nullptr && !point->locomotion, "WSU-21: live pads and the merchant lack locomotion");
    }
    for (const auto name : {"UC_Empire_Mineral_Extractor", "UC_Rebel_Mineral_Extractor", "UC_Underworld_Mineral_Extractor"}) {
        const auto* child = tables.find(name);
        expect(child != nullptr && child->under_construction && tables.find(child->constructed_type) != nullptr,
            "WBP-06/18: pad menus load UC and completed types");
        if (child != nullptr) {
            const auto* completed = tables.find(child->constructed_type);
            expect(!child->locomotion && completed != nullptr && !completed->locomotion,
                "WSU-21: UC and completed pad structures lack locomotion");
        }
    }
    for (std::size_t index = 0; index < ids.size() && index < tables.units.size(); ++index) {
        expect(tables.units[index].id == ids[index], "unit order: " + ids[index]);
    }
    for (const auto& type : tables.units) {
        if (type.id.find("Defense_Satellite_") == std::string::npos || type.id.find("Pad") != std::string::npos) continue;
        expect(type.tactical_sale != type.under_construction,
            "WBP-30 only completed satellite types author tactical-sale behavior");
        if (type.tactical_sale) expect(!type.tactical_sell_percentage,
            "WBP-31 stock satellites use the verified half-cost sale default");
    }
    const std::span fleet(tables.units.data(), std::min(ids.size(), tables.units.size()));
    const auto unit = [&](const std::string_view id) { return tables.find(id); };
    const auto power = [&](const std::string_view id) -> std::int64_t {
        const auto* found = unit(id);
        if (found == nullptr) return 0;
        if (found->kind != UnitKind::squadron) return found->ai_combat_power ? found->ai_combat_power->raw() / Fixed::scale : 0;
        std::int64_t sum = 0;
        for (const auto& member : found->members) {
            const auto* craft = unit(member.craft);
            if (craft != nullptr && craft->ai_combat_power) sum += craft->ai_combat_power->raw() / Fixed::scale;
        }
        return sum;
    };
    expect(power("Skirmish_Rebel_Star_Base_1") + 2 * power("Rebel_X-Wing_Squadron") + power("Y-Wing_Squadron") +
                   power("Corellian_Corvette") + power("Nebulon_B_Frigate") + power("Calamari_Cruiser") == 13525,
           "SK-24 Rebel tick-zero power");
    expect(power("Skirmish_Empire_Star_Base_1") + 2 * power("TIE_Interceptor_Squadron") + power("Tartan_Patrol_Cruiser") +
                   power("Acclamator_Assault_Ship") == 9590,
           "SK-24 Empire tick-zero power");

    const std::map<std::string, std::pair<std::size_t, std::size_t>> hardpoints{
        {"Corellian_Corvette", {8, 0}}, {"Nebulon_B_Frigate", {5, 5}}, {"Tartan_Patrol_Cruiser", {5, 0}},
        {"Acclamator_Assault_Ship", {8, 8}}, {"Calamari_Cruiser", {7, 7}}, {"Skirmish_Rebel_Star_Base_1", {7, 7}}, {"Skirmish_Empire_Star_Base_1", {7, 7}}};
    for (const auto& [id, counts] : hardpoints) {
        const auto* found = unit(id);
        if (found == nullptr) {
            expect(false, "hardpoint owner loads: " + id);
            continue;
        }
        std::size_t targetable = 0;
        for (const auto& hardpoint : found->hardpoints) {
            targetable += hardpoint.targetable ? 1U : 0U;
            if (hardpoint.targetable) expect(hardpoint.attachment.position.has_value(), "targetable hardpoint has a position: " + hardpoint.id);
        }
        expect(found->hardpoints.size() == counts.first && targetable == counts.second, "hardpoint counts: " + id);
    }
    const std::map<std::string, std::size_t> members{{"Rebel_X-Wing_Squadron", 5}, {"Y-Wing_Squadron", 3},
        {"TIE_Interceptor_Squadron", 7}, {"TIE_Fighter_Squadron", 7}, {"TIE_Bomber_Squadron", 4}};
    for (const auto& [id, count] : members) {
        const auto* found = unit(id);
        expect(found != nullptr && found->members.size() == count, "squadron composition: " + id);
        expect(found && found->team_hull == std::optional{Fixed::from_raw(raw(100))}
            && !found->team_shield_points && !found->hull,
            "WSQ-60: every M2 container has its own default hull and no authored shield: " + id);
    }
    const auto* nebulon = unit("Nebulon_B_Frigate");
    expect(nebulon != nullptr && nebulon->lua_script == "ObjectScript_PowerToShields", "SK-47 Lua_Script reference");
    expect(nebulon != nullptr && nebulon->abilities.size() == 1 && nebulon->abilities[0].type == "DEFEND" &&
               nebulon->abilities[0].supports_autofire,
           "Nebulon-B DEFEND supports autofire");
    // SK-22 (owner, Q1 amended 2026-09-28): the Rebel MC80. SK-47's script and DEFEND autofire, the
    // 8500 hull and 2000 shield, and no hangar (SK-23).
    const auto* mc80 = unit("Calamari_Cruiser");
    expect(mc80 != nullptr && mc80->lua_script == "ObjectScript_PowerToShields" && !mc80->spawner &&
               mc80->abilities.size() == 1 && mc80->abilities[0].type == "DEFEND" && mc80->abilities[0].supports_autofire,
           "SK-22 MC80: SK-47 script, DEFEND autofire, no hangar");
    if (mc80 != nullptr) {
        std::size_t ion = 0;
        std::size_t laser = 0;
        std::size_t engines = 0;
        for (const auto& hardpoint : mc80->hardpoints) {
            ion += hardpoint.type == eawr::units::HardpointType::weapon_ion_cannon ? 1U : 0U;
            laser += hardpoint.type == eawr::units::HardpointType::weapon_laser ? 1U : 0U;
            engines += hardpoint.type == eawr::units::HardpointType::engine ? 1U : 0U;
        }
        expect(ion == 2 && laser == 4 && engines == 1, "SK-22 MC80: 2 ion cannons, 4 lasers and the engines");
    }
    const auto* acclamator = unit("Acclamator_Assault_Ship");
    expect(acclamator != nullptr && acclamator->spawner && acclamator->spawner->starting.size() == 2 &&
               acclamator->spawner->delay_seconds && acclamator->spawner->delay_seconds->raw() == raw(5) &&
               acclamator->spawner->reserves.size() == 1 && !acclamator->spawner->reserves_used,
           "SK-23 Acclamator hangar; SK-36 reserves unused");
    expect(acclamator != nullptr && acclamator->abilities.size() == 1 && acclamator->abilities[0].type == "POWER_TO_WEAPONS" &&
               acclamator->abilities[0].authored_type == "power_to_weapons",
           "lower-case ability type");
    for (const auto* id : {"Skirmish_Rebel_Star_Base_1", "Skirmish_Empire_Star_Base_1"}) {
        const auto* station = unit(id);
        expect(station != nullptr && station->spawner && station->spawner->starting.size() == 2 &&
                   station->spawner->starting[0].count == 2 && station->spawner->delay_seconds &&
                   station->spawner->delay_seconds->raw() == raw(10),
               std::string("SK-23 station launches: ") + id);
    }
    // #68 authored ranges (Space_FOW_Reveal_Range); Skirmish_Rebel_Star_Base_1 inherits its
    // 2000 from Rebel_Star_Base_1, Corellian_Corvette authors 1200 then 1000 (the last
    // wins, as for every single-value tag).
    const std::map<std::string, std::int64_t> authored{{"Skirmish_Rebel_Star_Base_1", 2000},
        {"Skirmish_Empire_Star_Base_1", 2000}, {"Corellian_Corvette", 1000}, {"Nebulon_B_Frigate", 1200},
        {"Tartan_Patrol_Cruiser", 1200}, {"Acclamator_Assault_Ship", 1200}, {"Calamari_Cruiser", 1200}, {"X-Wing", 500}, {"TIE_Fighter", 500},
        {"TIE_Interceptor", 500}, {"Y-Wing", 600}, {"TIE_Bomber", 600}};
    // #271 sensors (V-01, V-03): REVEAL types with their own range; squadrons through their
    // team container (`Team` 800; Y-Wing_Squadron names Y_Wing_Squadron_Container, a variant
    // of Darth_Vader_TIE_Fighter_Container, 800 then 1000). Of the craft only the Y-Wing
    // authors REVEAL.
    const std::map<std::string, std::int64_t> sensors{{"Skirmish_Rebel_Star_Base_1", 2000},
        {"Skirmish_Empire_Star_Base_1", 2000}, {"Corellian_Corvette", 1000}, {"Nebulon_B_Frigate", 1200},
        {"Tartan_Patrol_Cruiser", 1200}, {"Acclamator_Assault_Ship", 1200}, {"Calamari_Cruiser", 1200}, {"Y-Wing", 600},
        {"Rebel_X-Wing_Squadron", 800}, {"TIE_Interceptor_Squadron", 800}, {"TIE_Fighter_Squadron", 800},
        {"TIE_Bomber_Squadron", 800}, {"Y-Wing_Squadron", 1000}};
    for (const auto& item : fleet) {
        const auto expected = authored.find(item.id);
        expect(item.selectable && !item.decoration, "WSU-15: the FoC fleet is selectable and not decoration: " + item.id);
        expect(item.locomotion == (item.kind != UnitKind::station), "WSU-21: stations lack locomotion; ships, craft and teams have it: " + item.id);
        if (expected == authored.end()) {
            expect(item.kind == UnitKind::squadron && !item.space_fow_reveal_range, "squadron authors no range: " + item.id);
        } else {
            expect(item.space_fow_reveal_range && item.space_fow_reveal_range->raw() == raw(expected->second),
                   "authored range: " + item.id);
        }
        const auto sensor = sensors.find(item.id);
        const auto range = eawr::units::sensor_range(item);
        expect(sensor == sensors.end() ? !range : range && range->raw() == raw(sensor->second), "sensor range: " + item.id);
    }
    // #665 (walk WSU-12): every M2 craft sets Mouse_Collide_Override_Sphere_Radius 50; ships and
    // squadron containers set none and are picked by their collision meshes.
    const std::set<std::string> pick_spheres{"X-Wing", "Y-Wing", "TIE_Fighter", "TIE_Interceptor", "TIE_Bomber"};
    for (const auto& item : fleet) {
        const bool sphere = pick_spheres.contains(item.id);
        expect(sphere ? item.mouse_collide_sphere_radius && item.mouse_collide_sphere_radius->raw() == raw(50)
                      : !item.mouse_collide_sphere_radius,
               "pick sphere (WSU-12): " + item.id);
    }
    std::size_t spawners = 0;
    for (const auto& item : fleet) spawners += item.spawner ? 1U : 0U;
    expect(spawners == 3, "no other roster ship spawns squadrons (SK-23)");
    expect(tables.priority_sets.size() >= 5, "Corvette, Frigate, Capital, Fighter and Bomber priority sets");
    // #270: the FoC space sets (SpaceUnitTargetingPriorities.xml) and R-09 on the fleet.
    for (const auto& set : tables.priority_sets) {
        bool categories = !set.attack_priorities.empty();
        for (const auto& entry : set.attack_priorities) {
            categories = categories && entry.match == eawr::units::PriorityMatch::category && entry.bits != 0;
        }
        expect(categories, "every FoC space entry is a category: " + set.id);
        expect(set.property_exclusions == std::vector<std::string>{"NotOpportunityTarget"} &&
                   set.property_exclusion_bits == 0x100 && set.category_exclusion_bits == 0 &&
                   set.unit_exclusions == std::vector<std::string>{"Destroyable_Asteroid_Small",
                       "Destroyable_Asteroid_Medium", "Destroyable_Asteroid_Large", "Destroyable_Asteroid_Huge"} &&
                   set.hard_point_priorities.empty() && set.hard_point_exclusions.empty(),
               "every FoC space set excludes NotOpportunityTarget and the destroyable asteroids: " + set.id);
    }
    const auto set_of = [&](const std::string_view id) -> const eawr::units::TargetingPrioritySet* {
        for (const auto& set : tables.priority_sets) {
            if (set.id == id) return &set;
        }
        return nullptr;
    };
    const auto* fighter_set = set_of("Fighter");
    const auto* x_wing = unit("X-Wing");
    const auto* y_wing = unit("Y-Wing");
    const auto* corellian = unit("Corellian_Corvette");
    const auto* frigate = unit("Nebulon_B_Frigate");
    expect(fighter_set != nullptr && x_wing != nullptr && y_wing != nullptr && corellian != nullptr && frigate != nullptr,
           "the Fighter set and the ranked fleet types load");
    if (fighter_set != nullptr && x_wing != nullptr && y_wing != nullptr && corellian != nullptr && frigate != nullptr) {
        const auto& entries = fighter_set->attack_priorities;
        expect(entries.size() == 3 && entries[0].name == "Transport" && entries[0].weight.raw() == raw(1) &&
                   entries[1].name == "Bomber" && entries[1].weight.raw() == raw(2) && entries[2].name == "Fighter" &&
                   entries[2].weight.raw() == raw(3),
               "FoC Fighter set: Transport 1, Bomber 2, Fighter 3");
        const auto y_score = eawr::units::attack_priority(*fighter_set, *y_wing);
        const auto x_score = eawr::units::attack_priority(*fighter_set, *x_wing);
        expect(y_score && x_score && *y_score < *x_score && x_score->raw() == raw(3),
               "S-01: the Fighter set ranks a Y-Wing (Bomber) above an X-Wing (Fighter)");
        expect(eawr::units::attack_priority(*fighter_set, *corellian) == eawr::units::unlisted_priority,
               "the Fighter set does not list Corvette: ranked last, not excluded");
        auto flagged = *frigate;
        flagged.property_bits |= 0x100;
        expect(!eawr::units::attack_priority(*fighter_set, flagged), "a NotOpportunityTarget type has no priority");
        auto asteroid = *frigate;
        asteroid.id = "Destroyable_Asteroid_Large";
        expect(!eawr::units::attack_priority(*fighter_set, asteroid), "a destroyable asteroid has no priority");
    }
    expect(tables.categories.size() == 30 && tables.properties.size() == 14, "FoC category and property enums");
    for (const auto& item : fleet) {
        // Squadrons are containers; only Rebel_X-Wing_Squadron authors a CategoryMask.
        expect((item.production.upgrade_object || item.kind == UnitKind::squadron || item.category_bits != 0) && (item.property_bits & 0x100) == 0,
               "fleet type has categories and is an opportunity target: " + item.id);
    }
    expect(tables.constants.scalars.size() == 55, "combat, hazard and multiplayer tech scalars");
    for (const auto& [tag, expected] : {
            std::pair{std::string_view{"Asteroid_Field_Damage"}, Fixed::from_raw(raw(20))},
            std::pair{std::string_view{"Asteroid_Field_Damage_Rate"}, Fixed::from_decimal("0.20").value()}}) {
        const auto constant = std::find_if(tables.constants.scalars.begin(), tables.constants.scalars.end(),
            [&](const auto& value) { return value.tag == tag; });
        expect(constant != tables.constants.scalars.end() && constant->value == expected,
            "WHZ-11/12: stock asteroid damage and per-frame probability are retained");
    }
    // #70: the five roster ships move; Max_Speed, OverrideAcceleration/Deceleration and
    // Max_Rate_Of_Turn x 1.2 (docs/behaviour/space-movement.md MV-01).
    const auto motion = eawr::units::motion_table(tables);
    expect(motion && motion.value().profiles.size() >= 5 && motion.value().rules.arc_degrees == Fixed::from_raw(raw(15)) &&
               motion.value().rules.expansion_distance == Fixed::from_raw(raw(300)),
           "FoC motion table: five ships, 15 degree arcs, 300 unit expansion distance");
    // OR-06, OR-14: the stock gameconstants.xml authors 10 frames and 750 units.
    expect(motion && motion.value().rules.reevaluation_frames == 10
               && motion.value().rules.guard_range == Fixed::from_raw(raw(750)) && motion.value().rules.max_waypoints == 6,
           "FoC motion rules: MovementReevaluationFrameCount 10, Space_Guard_Range 750");
    // #599 (space-fighters FO-11): the group move's lane steer reads FormationMinimumSideError
    // and FormationMaximumSideError.
    expect(motion && motion.value().squadrons.side_error_max == Fixed::from_raw(raw(30))
               && motion.value().squadrons.side_error_min.raw() > 0
               && motion.value().squadrons.side_error_min < Fixed::from_raw(raw(1)),
           "FoC squadron table: side errors 0.1 and 30");
    if (motion) {
        const auto* corvette = motion.value().find(eawr::assets::object_type_crc("Corellian_Corvette"));
        const auto* assault = motion.value().find(eawr::assets::object_type_crc("Acclamator_Assault_Ship"));
        const auto near = [](const Fixed value, const char* expected) {
            const auto difference = value.raw() - Fixed::from_decimal(expected).value().raw();
            return difference >= -2 && difference <= 2;
        };
        expect(corvette != nullptr && near(corvette->max_speed, "3.72") && near(corvette->acceleration, "0.06") &&
                   near(corvette->deceleration, "0.06") && corvette->rate_of_turn == Fixed::from_decimal("1.5").value() &&
                   corvette->turn_in_place_slowdown == Fixed::from_raw(raw(2)),
               "Corellian corvette: 3.72 per frame, 0.06 per frame squared, 1.5 degrees per frame, slowdown 2");
        expect(assault != nullptr && near(assault->max_speed, "2.64") && near(assault->rate_of_turn, "0.6") &&
                   assault->turn_in_place_slowdown == Fixed::from_raw(raw(3)),
               "Acclamator: 2.64 per frame, 0.6 degrees per frame, frigate slowdown 3");
        // #351 BK-01: Max_Rate_Of_Roll 0.2 x 1.2 for all four; Bank_Turn_Angle as authored.
        const auto* nebulon_motion = motion.value().find(eawr::assets::object_type_crc("Nebulon_B_Frigate"));
        const auto* tartan = motion.value().find(eawr::assets::object_type_crc("Tartan_Patrol_Cruiser"));
        const auto banks = [&near](const eawr::sim::tactical::MotionProfile* profile, const std::int64_t angle) {
            return profile != nullptr && near(profile->roll_rate, "0.24") && profile->bank_angle == Fixed::from_raw(raw(angle));
        };
        expect(banks(corvette, 15) && banks(tartan, 15) && banks(nebulon_motion, 5) && banks(assault, 20),
               "banking: roll rate 0.24 per frame; bank 15 (corvette, Tartan), 5 (Nebulon-B), 20 (Acclamator)");
        // #447 SP-01: the M2 craft spin away on death, rebel 0.2 and imperial 0.4, for 2 s.
        const auto spins = [&](const char* id, const char* chance) {
            const auto* craft = motion.value().squadrons.find_craft(eawr::assets::object_type_crc(id));
            return craft != nullptr && craft->spin_away && near(craft->spin_away->chance, chance)
                && craft->spin_away->time == Fixed::from_raw(raw(2));
        };
        expect(spins("X-Wing", "0.2") && spins("Y-Wing", "0.2") && spins("TIE_Fighter", "0.4")
                   && spins("TIE_Interceptor", "0.4") && spins("TIE_Bomber", "0.4"),
               "spin-away: X-wing and Y-wing 0.2, TIE fighter, interceptor and bomber 0.4, all 2 s");
        // #71 footprints (docs/behaviour/space-movement.md AV-05, docs/unit-data.md).
        using eawr::sim::tactical::SpaceLayer;
        const auto close = [](const Fixed value, const char* expected) {
            const auto difference = value.raw() - Fixed::from_decimal(expected).value().raw();
            return difference >= -(std::int64_t{1} << 14) && difference <= (std::int64_t{1} << 14);
        };
        const auto print = [&](const char* id) {
            const auto* footprint = motion.value().footprint(eawr::assets::object_type_crc(id));
            if (footprint == nullptr) {
                std::cout << "footprint " << id << ": none" << std::endl;
                return footprint;
            }
            std::cout << "footprint " << id << ": layer " << static_cast<int>(footprint->layer) << " x "
                      << footprint->x_extent.raw() << " y " << footprint->y_extent.raw() << " radius "
                      << footprint->radius.raw() << (footprint->obstacle ? " obstacle" : "") << std::endl;
            return footprint;
        };
        const auto* corvette_print = print("Corellian_Corvette");
        const auto* nebulon_print = print("Nebulon_B_Frigate");
        print("Tartan_Patrol_Cruiser");
        print("Acclamator_Assault_Ship");
        print("Skirmish_Rebel_Star_Base_1");
        print("Skirmish_Empire_Star_Base_1");
        const auto* pad = print("Mineral_Extractor_Pad");
        print("Defense_Satellite_Laser_Pad");
        print("N_Gravity_Well_Station");
        print("Skirmish_Merchant_Dock");
        const auto* container = print("Orbital_Resource_Container");
        expect(corvette_print != nullptr && corvette_print->layer == SpaceLayer::corvette
                   && close(corvette_print->x_extent, "17.755") && close(corvette_print->y_extent, "41.822")
                   && corvette_print->radius == corvette_print->y_extent,
               "corvette footprint: the RV_CORVETTE.ALO collision box x 0.5");
        expect(nebulon_print != nullptr && nebulon_print->layer == SpaceLayer::frigate && close(nebulon_print->x_extent, "23.48")
                   && close(nebulon_print->y_extent, "109.949"),
               "Nebulon-B footprint: the RV_NEBULONB.ALO collision box x 0.7");
        expect(pad != nullptr && pad->layer == SpaceLayer::static_object && pad->obstacle
                   && pad->radius == Fixed::from_decimal("206.25").value(),
               "the mining pad is a static obstacle of 275 x 0.75");
        expect(container == nullptr, "a type without Space_Layer is not tracked");
        // #372 review: validate_motion rejects a radius whose outer destination search ring holds
        // more than 2^13 points; the original M2 fleet's corvette puts 245 there.
        const eawr::sim::tactical::Footprint* smallest = nullptr;
        const eawr::sim::tactical::Footprint* fixture_smallest = nullptr;
        for (const auto& footprint : motion.value().footprints) {
            if (footprint.radius.raw() > 0 && (smallest == nullptr || footprint.radius < smallest->radius)) smallest = &footprint;
            const bool fixture_type = std::any_of(fleet.begin(), fleet.end(), [&](const auto& type) {
                return eawr::assets::object_type_crc(type.id) == footprint.type_id;
            });
            if (fixture_type && footprint.radius.raw() > 0
                && (fixture_smallest == nullptr || footprint.radius < fixture_smallest->radius)) fixture_smallest = &footprint;
        }
        if (smallest != nullptr) {
            std::string name;
            for (const auto& type : tables.units) {
                if (eawr::assets::object_type_crc(type.id) == smallest->type_id) name = type.id;
            }
            for (const auto& obstacle : tables.obstacles) {
                if (eawr::assets::object_type_crc(obstacle.id) == smallest->type_id) name = obstacle.id;
            }
            std::cout << "smallest soft radius: " << name << " radius " << smallest->radius.raw() << std::endl;
        }
        expect(fixture_smallest != nullptr && fixture_smallest == corvette_print,
               "the corvette has the original M2 fixture's smallest soft radius");
        expect(smallest != nullptr && smallest->type_id == eawr::assets::object_type_crc("Underworld_Pirate_IPV")
                   && close(smallest->radius, "35.81762"),
               "expanded production closure retains the pirate IPV's smaller soft radius");
        expect(motion.value().avoidance.has_value(), "FoC avoidance rules load");
    }

    // #76 abilities from FoC data (docs/behaviour/space-abilities.md AB-01 to AB-04): the four
    // modelled kinds on six types, squadron types and the cut HUNT left out; #561 adds the Y-wing
    // squadron's ION_CANNON_SHOT (AB-60) on the squadron and its craft. The MC80 (SK-22) carries the
    // Nebulon-B's DEFEND multipliers and script, with a 40 s recharge.
    const std::vector<eawr::sim::tactical::PlayerId> humans{1};
    const auto abilities = eawr::units::ability_table(tables, humans);
    expect(static_cast<bool>(abilities), "FoC ability table builds");
    if (!abilities) std::cerr << abilities.error().message << "\n";
    if (abilities) {
        using eawr::sim::tactical::AbilityKind;
        const auto& table = abilities.value();
        expect(table.profiles.size() >= 8 && table.humans == humans, "eight types carry a modelled ability");
        expect(table.autofire_defaults == humans, "AB-45: human profiles default to supported autofire");
        const auto manual = eawr::units::ability_table(tables, humans, nullptr, false);
        expect(manual && manual.value().autofire_defaults.empty(), "AB-45: the profile default can be disabled");
        const auto decimal = [](const char* text) { return Fixed::from_decimal(text).value(); };
        struct Expected {
            const char* id;
            AbilityKind kind;
            std::uint32_t expiration;
            std::uint32_t recharge;
            const char* weapon_delay;
            const char* shield_regen;
            const char* shield_interval;
            const char* energy_regen;
            const char* energy_interval;
            const char* speed;
            bool autofire;
            bool script;
        };
        const Expected rows[] = {
            {"Nebulon_B_Frigate", AbilityKind::defend, 450, 1800, "1", "1", "0.1", "3", "0.1", "0.8", true, true},
            {"Calamari_Cruiser", AbilityKind::defend, 450, 1200, "1", "1", "0.1", "3", "0.1", "0.8", true, true},
            {"Corellian_Corvette", AbilityKind::turbo, 600, 1500, "3", "0", "1", "1", "1", "2", false, false},
            {"Tartan_Patrol_Cruiser", AbilityKind::power_to_weapons, 210, 1800, "0.2", "-25", "1", "1", "1", "0.5", false, false},
            {"Acclamator_Assault_Ship", AbilityKind::power_to_weapons, 600, 1800, "0.5", "-3", "1", "1", "1", "0.5", false, false},
            {"X-Wing", AbilityKind::spoiler_lock, 0, 0, "3", "3", "1", "3", "1", "1.3", false, false},
        };
        for (const auto& row : rows) {
            const auto* found = table.find(eawr::assets::object_type_crc(row.id));
            const bool shaped = found != nullptr && found->abilities.size() == 1;
            expect(shaped, std::string("one modelled ability: ") + row.id);
            if (!shaped) continue;
            const auto& ability = found->abilities[0];
            const auto& m = ability.modifiers;
            expect(ability.kind == row.kind && ability.expiration_frames == row.expiration && ability.recharge_frames == row.recharge
                       && m.weapon_delay == decimal(row.weapon_delay) && m.shield_regen == decimal(row.shield_regen)
                       && m.shield_regen_interval == decimal(row.shield_interval) && m.energy_regen == decimal(row.energy_regen)
                       && m.energy_regen_interval == decimal(row.energy_interval) && m.speed == decimal(row.speed)
                       && ability.supports_autofire == row.autofire && found->defend_script == row.script,
                std::string("FoC ability values: ") + row.id);
        }
        // WHE-22/23: each craft retains its own inherited modes and timers.
        for (const char* id : {"Millennium_Falcon", "Admonitor_Star_Destroyer", "Wedge_XWing_Rogue",
                "Rogue_2_XWing", "Rogue_4_XWing", "Rogue_7_XWing", "Rogue_10_XWing", "Rogue_11_XWing"}) {
            const auto* hero = table.find(eawr::assets::object_type_crc(id));
            const auto kind = std::string_view(id) == "Millennium_Falcon" ? AbilityKind::invulnerability : AbilityKind::power_to_weapons;
            const auto slot = hero ? eawr::sim::tactical::ability_slot(*hero, kind).value_or(hero->abilities.size()) : 0;
            const bool bound = hero && slot < hero->abilities.size();
            expect(bound, std::string("WHE-22: loaded hero damage mode: ") + id);
            if (!bound) continue;
            const auto& mode = hero->abilities[slot];
            if (kind == AbilityKind::invulnerability) {
                expect(mode.expiration_frames == 180 && mode.recharge_frames == 1800 && mode.modifiers.take_damage == Fixed{},
                    "WHE-22: Falcon zero take damage, six seconds, sixty-second recharge");
            } else if (std::string_view(id) == "Admonitor_Star_Destroyer") {
                expect(mode.expiration_frames == 210 && mode.recharge_frames == 1500 && mode.modifiers.cause_damage == decimal("2"),
                    "WHE-22: Assault twice damage, seven seconds, fifty-second recharge");
            } else {
                expect(mode.expiration_frames == 300 && mode.recharge_frames == 1800 && mode.modifiers.cause_damage == decimal("2.5"),
                    "WHE-23: Rogue leader and inherited escorts use craft ten-second/sixty-second timers");
            }
        }
        for (const auto& [id, range, decrease, recharge] : {
                std::tuple{"Accuser_Star_Destroyer", 1100, "0.98", 600U},
                std::tuple{"Admonitor_Star_Destroyer", 800, "0.9", 750U}}) {
            const auto* hero = table.find(eawr::assets::object_type_crc(id));
            const auto slot = hero ? eawr::sim::tactical::ability_slot(*hero, AbilityKind::tractor_beam).value_or(hero->abilities.size()) : 0;
            const bool bound = hero && slot < hero->abilities.size();
            expect(bound, std::string("WHE-27: stock tractor is bound: ") + id);
            if (!bound) continue;
            const auto nested = std::find_if(hero->special.begin(), hero->special.end(), [](const auto& value) {
                return value.kind == eawr::sim::tactical::SpecialAbilityKind::tractor_beam;
            });
            expect(nested != hero->special.end() && nested->beam_max_range == Fixed::from_raw(raw(range))
                && nested->target_speed_decrease == decimal(decrease) && hero->abilities[slot].recharge_frames == recharge,
                "WHE-27/60: stock tractor range, reduction and recharge stay distinct");
        }
        const auto* accuser = table.find(eawr::assets::object_type_crc("Accuser_Star_Destroyer"));
        const auto energy = accuser ? eawr::sim::tactical::ability_slot(*accuser, AbilityKind::energy_weapon).value_or(accuser->abilities.size()) : 0;
        const bool energy_bound = accuser && energy < accuser->abilities.size();
        expect(energy_bound, "WHE-26: Accuser energy weapon is bound");
        if (energy_bound) {
            const auto nested = std::find_if(accuser->special.begin(), accuser->special.end(), [](const auto& value) {
                return value.kind == eawr::sim::tactical::SpecialAbilityKind::energy_weapon;
            });
            expect(nested != accuser->special.end() && nested->beam_max_range == Fixed::from_raw(raw(800))
                && nested->damage_per_frame == Fixed::from_raw(raw(50)) && accuser->abilities[energy].expiration_frames == 150
                && accuser->abilities[energy].recharge_frames == 3600 && nested->beam_owner_bone == "HP_trac_bone",
                "WHE-26/59: stock beam damage per frame, duration, recharge and owner attachment bind from XML");
        }
        const auto* slave = table.find(eawr::assets::object_type_crc("Slave_I"));
        auto ring_input = input;
        ring_input.types = {"MC30_Frigate", "TIE_Scout_Squadron"};
        ring_input.obstacles.clear();
        const auto ring_loaded = eawr::units::load_unit_tables(ring_input);
        expect(static_cast<bool>(ring_loaded), "WCB/WFP: stock MC30 and scout closure loads");
        if (ring_loaded) {
            const auto built = eawr::units::ability_table(ring_loaded.value(), humans);
            expect(static_cast<bool>(built), "WCB/WFP: stock ability profiles bind");
            if (built) {
                const auto* mc30 = built.value().find(eawr::assets::object_type_crc("MC30_Frigate"));
                const auto slot = mc30 ? eawr::sim::tactical::ability_slot(*mc30, AbilityKind::cluster_bomb) : std::nullopt;
                expect(slot && mc30->abilities[*slot].spawned, "WCB-01: MC30 has a cluster button");
                if (slot && mc30->abilities[*slot].spawned) {
                    const auto& ability = mc30->abilities[*slot];
                    const auto& spawn = *ability.spawned;
                    expect(ability.recharge_frames == 1500 && ability.supports_autofire && spawn.ring_count == 12
                        && spawn.countdown_frames == 45 && spawn.speed == Fixed::from_raw(raw(12))
                        && spawn.max_travel == Fixed::from_raw(raw(500)) && spawn.blast.damage == Fixed::from_raw(raw(150))
                        && spawn.blast.radius == Fixed::from_raw(raw(150)), "WCB-01: stock ring, flight, countdown and recharge bind");
                    expect(mc30->mc30_script, "FS-29: stock Lua_Script attaches the MC30 self-trigger");
                    const auto health = eawr::units::durability_table(ring_loaded.value());
                    const auto combat = eawr::units::combat_table(ring_loaded.value());
                    expect(health && combat, "WCB-02: stock bomb combat tables bind");
                    if (health && combat) {
                        eawr::sim::tactical::TacticalSetup initial;
                        initial.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
                            {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
                        initial.units = {{1, mc30->type_id, 1, {}, eawr::sim::math::identity_quat(), {}},
                            {2, mc30->type_id, 2, {Fixed::from_raw(raw(10000)), {}, {}}, eawr::sim::math::identity_quat(), {}}};
                        // FS-29/FD-47: no activation command; an AI MC30 consumes the preceding stock sample.
                        auto ai_initial = initial;
                        ai_initial.units[0].entity_id = 30;
                        ai_initial.units[0].owner = 2;
                        ai_initial.units[1].entity_id = 31;
                        ai_initial.units[1].owner = 1;
                        ai_initial.units[1].position = {Fixed::from_raw(raw(300)), {}, {}};
                        auto ai_session = eawr::sim::tactical::TacticalSession::create(ai_initial, {}, health.value(), {}, {}, combat.value(), {}, built.value());
                        expect(static_cast<bool>(ai_session), "FS-29: installed AI MC30 session creates");
                        if (ai_session) {
                            const eawr::platform::ThreadWorkerAdapter ai_executor(4);
                            for (unsigned frame = 0; frame <= 31; ++frame) {
                                const auto step = ai_session.value().step(ai_executor);
                                expect(static_cast<bool>(step), "FS-29: installed self-trigger step succeeds");
                                if (!step) break;
                                if (frame == 0 || frame == 30) expect(ai_session.value().projectiles().empty(),
                                    "FC-57: installed first empty update and equality skip do not fire");
                                if (frame == 31) expect(ai_session.value().projectiles().size() == 12
                                    && ai_session.value().ability_state(30)->slots[*slot].ready_tick == 1531,
                                    "FD-47: AI stock MC30 directly launches twelve bombs and starts recharge");
                            }
                        }
                        auto session = eawr::sim::tactical::TacticalSession::create(initial, {}, health.value(), {}, {}, combat.value(), {}, built.value());
                        expect(static_cast<bool>(session), "WCB-02: stock bomb session creates");
                        if (session) {
                            expect(static_cast<bool>(session.value().submit({{0, 1, 1}, {1},
                                eawr::sim::tactical::AbilityPayload{AbilityKind::cluster_bomb, eawr::sim::tactical::AbilityAction::activate}})),
                                "WCB-02: stock ring activates");
                            std::set<std::uint64_t> detonated;
                            const eawr::platform::ThreadWorkerAdapter executor(4);
                            for (unsigned tick = 0; tick < 48; ++tick) {
                                const auto step = session.value().step(executor);
                                expect(static_cast<bool>(step), "WCB-02: stock flight succeeds");
                                if (!step) break;
                                if (tick == 0) expect(session.value().projectiles().size() == 12, "WCB-02: twelve stock bombs launch");
                                for (const auto& event : step.value().snapshot->combat_events())
                                    if (event.kind == eawr::sim::tactical::CombatEventKind::projectile_expired && event.shooter == 1)
                                        detonated.insert(event.target);
                            }
                            expect(detonated.size() == 12 && session.value().projectiles().empty(),
                                "WCB-02: all twelve stock projectiles detonate, including travel expiry");
                        }
                    }
                }
                const auto* scout = built.value().find(eawr::assets::object_type_crc("TIE_Scout"));
                const auto ping = scout ? eawr::sim::tactical::ability_slot(*scout, AbilityKind::fow_reveal_ping) : std::nullopt;
                expect(ping && scout->abilities[*ping].spawned, "WFP-01: scout craft has the acting ping slot (AB-15)");
                if (ping && scout->abilities[*ping].spawned) {
                    const auto& ability = scout->abilities[*ping];
                    expect(ability.recharge_frames == 900 && ability.spawned->reveal_range == Fixed::from_raw(raw(1000))
                        && ability.spawned->speed.raw() == 0 && ability.spawned->max_travel.raw() == 0
                        && ability.spawned->dense_reveal_multiplier == Fixed::from_raw(raw(1)), "WFP-01: projectile radius replaces decal distance");
                    expect(!ability.spawned->lifetime, "WFP-03: stock ping has no injected or authored lifetime");
                    const auto health = eawr::units::durability_table(ring_loaded.value());
                    const auto combat = eawr::units::combat_table(ring_loaded.value());
                    expect(health && combat, "WFP-03: stock ping combat tables bind");
                    if (health && combat) {
                        namespace tactical = eawr::sim::tactical;
                        const eawr::sim::math::Vec3 click{Fixed::from_raw(raw(3000)), {}, {}};
                        tactical::TacticalSetup initial;
                        initial.players = {{1, 1, 1, tactical::player_flag_commandable},
                            {2, 2, 2, tactical::player_flag_commandable}};
                        initial.units = {{1, scout->type_id, 1, {}, eawr::sim::math::identity_quat(), {}}};
                        tactical::FogRules fog;
                        fog.map_left = Fixed::from_raw(raw(-2000)); fog.map_top = Fixed::from_raw(raw(2000));
                        fog.cell_size = Fixed::from_raw(raw(100)); fog.cells_wide = 80; fog.cells_tall = 40;
                        // Fast ordinary regrowth isolates the stock object's lifetime from FC-15 cadence.
                        fog.service_period = 1; fog.ramp_down_step = 238;
                        std::vector<std::string> reference;
                        for (const auto workers : {1U, 2U, 4U, 8U}) {
                            auto session = tactical::TacticalSession::create(initial, {}, health.value(), {}, fog,
                                combat.value(), {}, built.value());
                            expect(static_cast<bool>(session), "WFP-03: loaded stock ping session creates");
                            if (!session) continue;
                            expect(static_cast<bool>(session.value().submit({{0, 1, 1}, {1},
                                tactical::AbilityPayload{AbilityKind::fow_reveal_ping, tactical::AbilityAction::activate,
                                    eawr::sim::invalid_entity_id, tactical::no_hardpoint, click}})), "WFP-03: loaded stock ping activates");
                            const eawr::platform::ThreadWorkerAdapter executor(workers);
                            std::vector<std::string> hashes;
                            for (unsigned tick = 0; tick < 200; ++tick) {
                                const auto step = session.value().step(executor);
                                expect(static_cast<bool>(step), "WFP-03: loaded stock ping steps");
                                if (!step) break;
                                hashes.push_back(step.value().state_sha256);
                                if (tick == 0) expect(session.value().projectiles().size() == 1
                                    && session.value().fog_cells()->revealed(0, click)
                                    && !session.value().fog_cells()->revealed(1, click),
                                    "WFP-01/03: stock spawn reveals only for owner");
                                if (tick >= 1) expect(session.value().projectiles().empty(),
                                    "WFP-03: stock ping ends on first flight and never returns");
                                if (tick >= 8) expect(!session.value().fog_cells()->revealed(0, click),
                                    "WFP-03: ordinary fog regrows after stock ping expires");
                            }
                            if (reference.empty()) reference = hashes;
                            else expect(hashes == reference, "WFP-03: loaded stock expiry equals on 1/2/4/8 workers");
                        }
                    }
                }
            }
        }
        const auto bomb = slave ? eawr::sim::tactical::ability_slot(*slave, AbilityKind::harmonic_bomb) : std::nullopt;
        expect(bomb && slave->abilities[*bomb].spawned && slave->abilities[*bomb].spawned->countdown_frames == 45
            && slave->abilities[*bomb].recharge_frames == 600, "stock harmonic bomb binds the authored countdown and recharge");
        // WAB-70..74: the StarViper squadron's team container holds BUZZ_DROIDS with the nested
        // data of its team type and the Buzz_Droids object's own speed and valid targets. The pinned
        // M2 roster has no Underworld units, so the squadron loads on its own.
        auto viper_input = input;
        viper_input.types = {"StarViper_Squadron"};
        viper_input.obstacles.clear();
        const auto viper_loaded = eawr::units::load_unit_tables(viper_input);
        expect(static_cast<bool>(viper_loaded), "WAB-70: the StarViper squadron's tables load");
        std::optional<eawr::sim::tactical::AbilityTable> viper_abilities;
        if (viper_loaded) {
            if (auto built = eawr::units::ability_table(viper_loaded.value(), humans)) viper_abilities = std::move(built).value();
        }
        expect(viper_abilities.has_value(), "WAB-70: the StarViper squadron's ability table builds");
        const auto* viper = viper_abilities ? viper_abilities->find(eawr::assets::object_type_crc("StarViper_Squadron")) : nullptr;
        const auto buzz = viper ? eawr::sim::tactical::ability_slot(*viper, AbilityKind::buzz_droids) : std::nullopt;
        expect(buzz.has_value(), "WAB-70: the StarViper squadron's team holds buzz droids");
        if (buzz) {
            const auto& ability = viper->abilities[*buzz];
            const auto per_frame = [](const std::int64_t per_second) {
                return Fixed::from_raw((raw(per_second) + 15) / 30);
            };
            expect(ability.team && ability.recharge_frames == 1800 && ability.buzz
                && ability.buzz->droid_type == eawr::assets::object_type_crc("Buzz_Droids")
                && ability.buzz->min_range == Fixed::from_raw(raw(5)) && ability.buzz->max_range == Fixed::from_raw(raw(400))
                && ability.buzz->chase_radius == Fixed::from_raw(raw(300)) && ability.buzz->damage_radius == Fixed::from_raw(raw(175))
                && ability.buzz->enemy_damage == per_frame(23) && ability.buzz->own_damage == per_frame(3)
                && ability.buzz->activation_frames == 30 && ability.buzz->victim_categories != 0
                && ability.buzz->speed.raw() - decimal("4.2").raw() <= 2 && decimal("4.2").raw() - ability.buzz->speed.raw() <= 2,
                "WAB-70..74: stock buzz-droid reach, radii, damage, arming, speed and recharge bind from XML");
        }
        const auto* viper_craft = viper_abilities ? viper_abilities->find(eawr::assets::object_type_crc("Starviper_Fighter")) : nullptr;
        expect(viper_craft == nullptr || !eawr::sim::tactical::ability_slot(*viper_craft, AbilityKind::buzz_droids),
            "WAB-70: the StarViper craft carry no buzz-droid slot of their own (AB-60)");
        const auto* antilles = table.find(eawr::assets::object_type_crc("Sundered_Heart"));
        const auto weak = antilles ? eawr::sim::tactical::ability_slot(*antilles, AbilityKind::weaken_enemy) : std::nullopt;
        expect(weak && antilles->abilities[*weak].spawned && antilles->abilities[*weak].spawned->weaken.on_detonation
            && antilles->abilities[*weak].spawned->weaken.duration_frames == 450
            && antilles->abilities[*weak].spawned->weaken.radius == Fixed::from_raw(raw(300))
            && antilles->abilities[*weak].spawned->weaken.cause_damage_reduction == Fixed::from_decimal("0.2").value(),
            "stock weaken projectile binds radius, timed damage reduction and detonation gate");
        const auto* vader = table.find(eawr::assets::object_type_crc("TIE_Prototype"));
        const auto refill = vader ? eawr::sim::tactical::ability_slot(*vader, AbilityKind::replenish_wingmen) : std::nullopt;
        expect(refill && vader->abilities[*refill].replenish_team == eawr::assets::object_type_crc("Darth_Vader_TIE_Fighter_Squadron")
            && vader->abilities[*refill].recharge_frames == 1800 && !vader->abilities[*refill].replenish_particle.empty(),
            "WHE-63 Vader uses authored creation team, sixty-second recharge and particle");
        // AB-60: the team ability from Y_Wing_Squadron_Container (Recharge_Seconds 20, autofire),
        // and each craft's own slot of the kind.
        const auto* team = table.find(eawr::assets::object_type_crc("Y-Wing_Squadron"));
        expect(team != nullptr && team->abilities.size() == 1 && team->abilities[0].kind == AbilityKind::ion_cannon_shot
                   && team->abilities[0].team && team->abilities[0].recharge_frames == 600
                   && team->abilities[0].supports_autofire,
            "AB-60: the Y-wing squadron holds the team ION_CANNON_SHOT");
        const auto* craft = table.find(eawr::assets::object_type_crc("Y-Wing"));
        expect(craft != nullptr && craft->abilities.size() == 1 && craft->abilities[0].kind == AbilityKind::ion_cannon_shot
                   && !craft->abilities[0].team,
            "AB-60: each Y-wing has its own ION_CANNON_SHOT slot");
        const auto* hunter = table.find(eawr::assets::object_type_crc("TIE_Fighter"));
        expect(hunter != nullptr && hunter->abilities.size() == 1
                   && hunter->abilities[0].kind == AbilityKind::hunt && !hunter->abilities[0].team
                   && hunter->abilities[0].expiration_frames == 0 && hunter->abilities[0].recharge_frames == 0
                   && !hunter->abilities[0].supports_autofire
                   && hunter->hunt_reveal_range == Fixed::from_raw(raw(500)),
            "WAB-50/54: each stock TIE Fighter binds untimed Hunt and its authored reveal range");
        for (const char* id : {"Rebel_X-Wing_Squadron", "TIE_Fighter_Squadron"}) {
            expect(table.find(eawr::assets::object_type_crc(id)) == nullptr, std::string("no modelled ability profile: ") + id);
        }
    }

    // #72 durability from FoC data (docs/behaviour/space-hardpoints.md HD-01 to HD-05).
    const auto durability = eawr::units::durability_table(tables);
    expect(static_cast<bool>(durability), "FoC durability table builds");
    if (durability) {
        using eawr::sim::tactical::HardpointRole;
        const auto& table = durability.value();
        expect(table.rules.hull_vs_hardpoints.raw() == Fixed::from_decimal("0.2").value().raw() &&
                   table.rules.engines_disabled_speed.raw() == Fixed::from_decimal("0.4").value().raw() &&
                   table.rules.damaged_fraction.raw() == Fixed::from_decimal("0.33").value().raw(),
               "FoC durability rules 0.2, 0.4 and 0.33");
        expect(table.profiles.size() > 12, "original fleet hulls plus capture points, UC and completed structures");
        const auto profile = [&](const std::string_view id) { return table.find(eawr::assets::object_type_crc(id)); };
        for (const auto& [id, count] : members) {
            static_cast<void>(count);
            expect(profile(id) == nullptr, "WSQ-60: craft alone carry combat durability: " + id);
        }
        const auto roles = [](const eawr::sim::tactical::DurabilityProfile& item) {
            std::string text;
            for (const auto& hardpoint : item.hardpoints) {
                text += std::string(eawr::sim::tactical::to_string(hardpoint.role)) + (hardpoint.destroyable ? "+" : "-") +
                        std::to_string(hardpoint.max_health.raw() / Fixed::scale) + " ";
            }
            return text;
        };
        const auto* nebulon_profile = profile("Nebulon_B_Frigate");
        expect(nebulon_profile != nullptr && nebulon_profile->max_hull.raw() == raw(5400) &&
                   roles(*nebulon_profile) == "weapon+390 weapon+390 weapon+390 weapon+390 engine+390 ",
               "Nebulon-B: 3600 x 1.5 hull, 4 lasers and the engines at 260 x 1.5");
        const auto* acclamator_profile = profile("Acclamator_Assault_Ship");
        expect(acclamator_profile != nullptr && acclamator_profile->max_hull.raw() == raw(3000) &&
                   roles(*acclamator_profile) ==
                       "weapon+210 weapon+210 weapon+240 weapon+210 weapon+210 weapon+240 engine+255 fighter_bay+150 ",
               "Acclamator: 6 weapons, engines and the fighter bay");
        const auto* mc80_profile = profile("Calamari_Cruiser");
        expect(mc80_profile != nullptr && mc80_profile->max_hull.raw() == raw(12750) &&
                   roles(*mc80_profile) == "weapon+630 weapon+630 weapon+630 weapon+630 weapon+630 weapon+630 engine+735 ",
               "MC80: 8500 x 1.5 hull, 2 ion cannons and 4 lasers at 420 x 1.5, the engines at 490 x 1.5: "
                   + (mc80_profile == nullptr ? std::string("none")
                                              : std::to_string(mc80_profile->max_hull.raw() / Fixed::scale) + " " + roles(*mc80_profile)));
        for (const auto* id : {"Corellian_Corvette", "Tartan_Patrol_Cruiser"}) {
            const auto* corvette = profile(id);
            bool none_destroyable = corvette != nullptr && !corvette->hardpoints.empty();
            if (corvette != nullptr) {
                for (const auto& hardpoint : corvette->hardpoints) none_destroyable = none_destroyable && !hardpoint.destroyable;
            }
            expect(none_destroyable, std::string("corvette weapons are not destroyable: ") + id);
        }
        for (const auto* id : {"Skirmish_Rebel_Star_Base_1", "Skirmish_Empire_Star_Base_1"}) {
            const auto* station_profile = profile(id);
            std::size_t shield_generators = 0;
            bool repairable = station_profile != nullptr;
            if (station_profile != nullptr) {
                for (const auto& hardpoint : station_profile->hardpoints) {
                    if (hardpoint.role == HardpointRole::shield_generator) {
                        ++shield_generators;
                        repairable = repairable && hardpoint.max_health.raw() == raw(975) &&
                                     hardpoint.repair_amount_per_frame.raw() == Fixed::from_decimal(".50").value().raw() &&
                                     hardpoint.repair_cost_per_frame.raw() == Fixed::from_decimal("1.5").value().raw();
                    }
                }
            }
            expect(station_profile != nullptr && station_profile->max_hull.raw() == raw(2400) && shield_generators == 1 &&
                       repairable,
                   std::string("station: 1600 x 1.5 hull, one repairable shield generator at 650 x 1.5: ") + id);
        }
        for (const auto& item : fleet) {
            if (item.kind == UnitKind::squadron) continue;
            if (item.kind != UnitKind::station) {
                for (const auto& hardpoint : item.hardpoints) {
                    expect(!hardpoint.repair_amount_per_frame && !hardpoint.repair_cost_per_frame,
                           "no ship or craft hardpoint authors repair: " + hardpoint.id);
                }
            }
            expect(item.destroyed_with_hardpoints, "FoC fleet type defaults to death with its hardpoints: " + item.id);
        }
    }

    const auto identity = eawr::units::content_identity(tables);
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (const auto byte : identity) {
        hex.push_back(digits[byte >> 4U]);
        hex.push_back(digits[byte & 15U]);
    }
    std::cout << "FoC fleet: " << tables.units.size() << " units, " << tables.projectiles.size() << " projectiles, "
              << tables.constants.damage_to_armor.size() << " damage/armor rows, " << tables.unresolved.size()
              << " unresolved, " << tables.notes.size() << " notes, identity " << hex << std::endl;
    print_rows("unresolved", tables.unresolved);
    print_rows("note", tables.notes);
    expect(tables.projectiles.size() >= 13 && tables.constants.damage_to_armor.size() >= 168,
           "production closure retains the pinned projectile and damage rows");
    expect(tables.unresolved.size() == 5 && std::all_of(tables.unresolved.begin(), tables.unresolved.end(),
        [](const auto& row) {
            return (row.owner == "Empire_Defense_Satellite_Repair" || row.owner == "Rebel_Defense_Satellite_Repair")
                ? row.field == "Space_FOW_Reveal_Range"
                : ((row.owner == "Slave_I" || row.owner == "TIE_Prototype") && row.field == "Targeting_Priority_Set")
                  || (row.owner == "Darth_Vader_TIE_Fighter_Container" && row.field == "Create_Team_Type");
        }), "only authored repair reveal, hero targeting and container replenishment gaps remain unresolved");
    expect(std::none_of(tables.unresolved.begin(), tables.unresolved.end(), [](const auto& row) {
        return row.owner == "UL_Extort_Cash_L1_Upgrade" || row.owner == "UL_Extort_Cash_L2_Upgrade";
    }), "upgrade objects never pass through the required ship-body fields");
    const auto pin_path = environment("EAWR_UNIT_PINNED_REPLAY").value_or(EAWR_UNIT_PINNED_REPLAY);
    std::ifstream pinned(pin_path, std::ios::binary);
    const std::vector<std::uint8_t> pinned_bytes((std::istreambuf_iterator<char>(pinned)), std::istreambuf_iterator<char>());
    const auto replay_pin = eawr::sim::tactical::parse_replay(pinned_bytes);
    expect(replay_pin && replay_pin.value().setup.content_identity == identity,
           "FoC content identity matches the generated M2 replay pin");
    // WPR-50/52: every faction/level chain loads its menu's types through the same closure.
    for (const auto* faction : {"Rebel", "Empire"}) for (unsigned level = 1; level <= 5; ++level) {
        const auto name = std::string("Skirmish_") + faction + "_Star_Base_" + std::to_string(level);
        const auto* station = tables.find(name);
        expect(station != nullptr && !station->production.buildable.empty(), "station menu closure: " + name);
        if (station == nullptr) continue;
        expect(level == 5 || tables.find(station->production.next_level) != nullptr, "next station level resolves: " + name);
        for (const auto& list : station->production.buildable) for (const auto& type : list.types) {
            expect(tables.find(type) != nullptr, "buildable type resolves: " + type);
        }
    }

    // The Q24 root-to-bone composition agrees with a double-precision
    // composition of the same stored binary32 transforms.
    double worst = 0;
    std::size_t bones = 0;
    for (const auto& item : fleet) {
        if (item.model_path.empty()) continue;
        const auto* model = access.model(item.model_path);
        if (model == nullptr) continue;
        const auto frames = eawr::units::bind_frames(*model);
        expect(static_cast<bool>(frames), "bind frames: " + item.model_path);
        if (!frames) continue;
        std::vector<std::array<double, 12>> reference(model->bones.size());
        for (std::size_t index = 0; index < model->bones.size(); ++index) {
            std::array<double, 12> local{};
            for (std::size_t element = 0; element < 12; ++element) local[element] = model->bones[index].relative_transform[element];
            const auto parent = model->bones[index].parent;
            if (parent < 0) {
                reference[index] = local;
                continue;
            }
            const auto& p = reference[static_cast<std::size_t>(parent)];
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    double value = column == 3 ? p[row * 4 + 3] : 0.0;
                    for (std::size_t k = 0; k < 3; ++k) value += p[row * 4 + k] * local[k * 4 + column];
                    reference[index][row * 4 + column] = value;
                }
            }
        }
        for (std::size_t index = 0; index < model->bones.size(); ++index) {
            for (std::size_t row = 0; row < 3; ++row) {
                const double fixed = static_cast<double>(frames.value()[index].rows[row][3].raw()) / Fixed::scale;
                worst = std::max(worst, std::abs(fixed - reference[index][row * 4 + 3]));
            }
            ++bones;
        }
    }
    std::cout << "FoC fleet: " << bones << " bone translations, worst Q24 vs double difference " << worst << std::endl;
    expect(bones > 0 && worst < 1.0e-4, "Q24 bone composition matches the double reference");
    foc_gunboat_pole(input);
    foc_object_weapon_defaults(input);
    foc_fighter_muzzles(input);
    foc_living_collision(input);
    foc_resource_container(input);
    foc_container_not_opportunity(input);
}

} // namespace unit_tables_test_support
