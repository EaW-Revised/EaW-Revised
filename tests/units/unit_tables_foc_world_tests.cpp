#include "unit_tables_foc_fixtures.hpp"
#include "eawr/skirmish/start.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace unit_tables_test_support {


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

} // namespace unit_tables_test_support
