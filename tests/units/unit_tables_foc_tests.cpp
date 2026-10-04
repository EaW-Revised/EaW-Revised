#include "unit_tables_support.hpp"
#include "eawr/skirmish/start.hpp"

namespace unit_tables_test_support {

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

// W-09: load the stock types affected by an omitted Fires_Forward. Keep StarViper
// movement out of this contract so the test observes its cone before it turns.
void foc_object_weapon_defaults(eawr::units::LoadInput input) {
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
             std::pair{point(raw(300), raw(300), 0), true},
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
                expect(!hazard.asteroid_field && obstacle->space_layer.empty(),
                    "WHZ-01: noncolliding junk remains authored scenery");
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

void foc_fleet() {
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
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    foc_super_capitals(input);
    foc_hazard_maps(input);
    foc_mass_drivers(input);
    auto loaded = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(loaded), "FoC unit tables load");
    if (!loaded) return;
    const auto& tables = loaded.value();
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
    expect(static_cast<bool>(ion_combat), "ion production combat table loads");
    if (ion_combat) {
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
    expect(tables.constants.scalars.size() == 49, "combat, hazard and multiplayer tech scalars");
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
            const auto slot = hero ? eawr::sim::tactical::ability_slot(*hero, kind) : std::nullopt;
            expect(slot.has_value(), std::string("WHE-22: loaded hero damage mode: ") + id);
            if (!slot) continue;
            const auto& mode = hero->abilities[*slot];
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
            const auto slot = hero ? eawr::sim::tactical::ability_slot(*hero, AbilityKind::tractor_beam) : std::nullopt;
            expect(slot.has_value(), std::string("WHE-27: stock tractor is bound: ") + id);
            if (!slot) continue;
            const auto nested = std::find_if(hero->special.begin(), hero->special.end(), [](const auto& value) {
                return value.kind == eawr::sim::tactical::SpecialAbilityKind::tractor_beam;
            });
            expect(nested != hero->special.end() && nested->beam_max_range == Fixed::from_raw(raw(range))
                && nested->target_speed_decrease == decimal(decrease) && hero->abilities[*slot].recharge_frames == recharge,
                "WHE-27/60: stock tractor range, reduction and recharge stay distinct");
        }
        const auto* accuser = table.find(eawr::assets::object_type_crc("Accuser_Star_Destroyer"));
        const auto energy = accuser ? eawr::sim::tactical::ability_slot(*accuser, AbilityKind::energy_weapon) : std::nullopt;
        expect(energy.has_value(), "WHE-26: Accuser energy weapon is bound");
        if (energy) {
            const auto nested = std::find_if(accuser->special.begin(), accuser->special.end(), [](const auto& value) {
                return value.kind == eawr::sim::tactical::SpecialAbilityKind::energy_weapon;
            });
            expect(nested != accuser->special.end() && nested->beam_max_range == Fixed::from_raw(raw(800))
                && nested->damage_per_frame == Fixed::from_raw(raw(50)) && accuser->abilities[*energy].expiration_frames == 150
                && accuser->abilities[*energy].recharge_frames == 3600 && nested->beam_owner_bone == "HP_trac_bone",
                "WHE-26/59: stock beam damage per frame, duration, recharge and owner attachment bind from XML");
        }
        const auto* slave = table.find(eawr::assets::object_type_crc("Slave_I"));
        const auto bomb = slave ? eawr::sim::tactical::ability_slot(*slave, AbilityKind::harmonic_bomb) : std::nullopt;
        expect(bomb && slave->abilities[*bomb].spawned && slave->abilities[*bomb].spawned->countdown_frames == 45
            && slave->abilities[*bomb].recharge_frames == 600, "stock harmonic bomb binds the authored countdown and recharge");
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
        for (const char* id : {"Rebel_X-Wing_Squadron", "TIE_Fighter", "TIE_Fighter_Squadron"}) {
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
    std::ifstream pinned(EAWR_UNIT_PINNED_REPLAY, std::ios::binary);
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
    foc_living_collision(input);
}

} // namespace unit_tables_test_support
