#include "unit_tables_support.hpp"
#include "eawr/skirmish/start.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"
#include "../../src/sim/tactical/blast_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace unit_tables_test_support {

void space_population() {
    // The inherited zero and negative sentinel are distinct from a positive override.
    for (const auto override_text : {std::string_view{""}, std::string_view{"3"},
        std::string_view{"0"}, std::string_view{"-1"}}) {
        TempTree tree;
        auto xml = units_xml("200", "Select_Test");
        const auto tag = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        expect(tag != std::string::npos, "population fixture has its ship");
        if (tag == std::string::npos) return;
        const auto insert = xml.find('>', tag) + 1;
        const auto override_tag = override_text.empty() ? std::string{}
            : "<Space_Override_Population_Value>" + std::string(override_text) + "</Space_Override_Population_Value>";
        xml.insert(insert, "<Variant_Of_Existing_Type>Population_Base</Variant_Of_Existing_Type>"
            "<Tactical_Build_Cost_Multiplayer>10</Tactical_Build_Cost_Multiplayer>"
            "<Tactical_Build_Time_Seconds>1</Tactical_Build_Time_Seconds>"
            "<Tactical_Production_Queue>Tactical_Units</Tactical_Production_Queue>");
        xml.insert(xml.find('>') + 1, "<SpaceUnit Name=\"Population_Base\"><Population_Value>2</Population_Value>"
            + override_tag + "</SpaceUnit>");
        const auto base = xml.find("<StarBase Name=\"Test_Base\">");
        xml.insert(xml.find('>', base) + 1,
            "<Tactical_Buildable_Objects_Multiplayer>Rebel, Test_Frigate</Tactical_Buildable_Objects_Multiplayer>");
        write_fixture(tree.root, xml);
        write(tree.root / "XML" / "FactionFiles.xml", "<Faction_Files><File>population_factions.xml</File></Faction_Files>");
        write(tree.root / "XML" / "population_factions.xml", "<Factions><Faction Name=\"Rebel\" /></Factions>");
        auto loaded = load(tree.root, models(0.0F));
        expect(loaded.tables.has_value(), "space population XML loads");
        if (!loaded.tables) continue;
        const auto* ship = loaded.tables->find("Test_Frigate");
        expect(ship != nullptr, "space population ship exists");
        if (!ship) continue;
        const auto expected = override_text == "3" ? 3U : override_text == "0" ? 0U : 2U;
        expect(ship->production.population_value == 2U
            && eawr::units::population_in_space(ship->production.population_value,
                ship->production.space_override_population_value) == expected,
            "space override preserves base and honors positive, zero, absent and negative sentinel");
        eawr::skirmish::SkirmishStart start;
        start.setup.players = {{1, 0, 1, 1}};
        const auto economy = eawr::skirmish::economy_rules(start, {}, *loaded.tables);
        expect(static_cast<bool>(economy), "space population skirmish economy binds");
        bool found{};
        if (economy) for (const auto& menu : economy.value().menus) {
            if (const auto* option = menu.find(eawr::skirmish::type_id("Test_Frigate"))) {
                found = true;
                expect(option->population == expected, "skirmish build/arrival cost uses the effective space population");
            }
        }
        expect(found, "space population fixture exposes its skirmish build option");
        auto changed = *loaded.tables;
        auto* mutable_ship = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
        mutable_ship->production.space_override_population_value = expected + 1;
        expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
            "effective space population participates in tactical content identity");
    }
    expect(eawr::units::population_in_space(std::optional<Fixed>{Fixed::from_integer(2).value()},
        std::optional<Fixed>{Fixed::from_integer(-1).value()}) == Fixed::from_integer(2).value(),
        "shared GC getter retains base for a negative space override");
}

void ship_suitability_content() {
    // WWP-21: combat geometry does not depend on Space_Layer or movement constants.
    for (const auto kind : {UnitKind::ship, UnitKind::craft, UnitKind::squadron}) {
        for (int source = 0; source < 6; ++source) {
            eawr::units::UnitTables tables;
            eawr::units::UnitType type;
            type.id = "Radius_Target";
            type.kind = kind;
            type.scale_factor = Fixed::from_raw(raw(2));
            type.footprint.collision_x = Fixed::from_raw(raw(-40));
            type.footprint.collision_y = Fixed::from_raw(raw(20));
            type.footprint.custom_hard_x = Fixed::from_raw(raw(3));
            type.footprint.custom_hard_y = Fixed::from_raw(raw(4));
            type.footprint.custom_soft_radius = Fixed::from_raw(raw(source == 0 || source == 4 ? 7 : -1));
            type.footprint.obstacle_radius = Fixed::from_raw(raw(source == 1 || source == 5 ? 9 : 0));
            if (source >= 3) {
                type.footprint.collision_x.reset();
                type.footprint.collision_y.reset();
            }
            tables.units.push_back(type);
            tables.constants.scalars.push_back({"Melee_Cutoff_Range", Fixed::from_raw(raw(51)), "51"});
            const auto unset = eawr::units::combat_table(tables);
            expect(unset && !unset.value().auto_rotate_for_targeting,
                "A-05: a table without Auto_Rotate_For_Space_Targeting keeps FoC's false");
            tables.constants.scalars.push_back({"Auto_Rotate_For_Space_Targeting", Fixed::from_raw(raw(1)), "True"});
            const auto converted = eawr::units::combat_table(tables);
            expect(converted && converted.value().auto_rotate_for_targeting,
                "A-05: Auto_Rotate_For_Space_Targeting loads into the combat table");
            const auto* geometry = converted ? converted.value().footprint(eawr::assets::object_type_crc(type.id)) : nullptr;
            const auto expected = source == 0 ? 14 : source == 1 ? 18 : source == 2 ? 80 : 0;
            expect(geometry && geometry->radius == Fixed::from_raw(raw(expected))
                && geometry->x_extent == Fixed::from_raw(raw(6)) && geometry->y_extent == Fixed::from_raw(raw(8))
                && converted.value().melee_cutoff == Fixed::from_raw(raw(51)),
                "WWP-21: model gate precedes custom/obstacle radius and scale; hard overrides stay separate");
            expect(converted && (converted.value().find(geometry->type_id) == nullptr) == (kind == UnitKind::squadron),
                "WWP-21: team geometry does not make a container a combatant");
        }
    }
    for (const bool override_valid : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto insert = [&](const std::string_view opening, const std::string& tags) {
            xml.insert(xml.find(opening) + opening.size(), tags);
        };
        insert("<StarBase Name=\"Test_Base\">", "<Is_Valid_Target>No</Is_Valid_Target>");
        if (override_valid) insert("<StarBase Name=\"Skirmish_Test_Base\">", "<Is_Valid_Target>Yes</Is_Valid_Target>");
        xml.replace(xml.find("<Behavior>SELECTABLE</Behavior>"), std::string_view{"<Behavior>SELECTABLE</Behavior>"}.size(),
            "<Behavior>SELECTABLE, SPECIAL_WEAPON, DUMMY_STAR_BASE</Behavior>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WCC-25: suitability XML loads");
        if (!loaded.tables) continue;
        const auto combat = eawr::units::combat_table(*loaded.tables);
        expect(static_cast<bool>(combat), "WCC-25: suitability content binds combat");
        if (!combat) continue;
        const auto* station = combat.value().find(eawr::assets::object_type_crc("Skirmish_Test_Base"));
        const auto* ship = combat.value().find(eawr::assets::object_type_crc("Test_Frigate"));
        expect(station && station->valid_target == override_valid && !station->star_base,
            "WCC-25: valid-target inheritance and override apply; XML class does not imply star-base behavior");
        expect(ship && ship->valid_target && ship->special_weapon && ship->star_base,
            "WCC-25: omitted valid-target defaults true; behavior flags bind independently of XML class");
        for (int field = 0; field < 3; ++field) {
            auto changed = *loaded.tables;
            auto* unit = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
            if (field == 0) unit->valid_target = false;
            if (field == 1) unit->special_weapon = false;
            if (field == 2) unit->star_base = false;
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                "WCC-25: each gameplay suitability flag enters content identity");
        }
    }
}


// These contracts use XML and existing combat interfaces so the identical tests run on BASE.
void height_adjusted_aim() {
    namespace t = eawr::sim::tactical;
    namespace d = t::detail;
    namespace m = eawr::sim::math;
    const auto at = [](const std::int64_t x, const std::int64_t y, const std::int64_t z) {
        return Vec3{Fixed::from_raw(raw(x)), Fixed::from_raw(raw(y)), Fixed::from_raw(raw(z))};
    };
    for (const bool structure : {false, true}) {
        for (const auto adjustment : {0, 35, 30, -20}) {
            TempTree tree;
            auto xml = units_xml("3600", "Unit_Select_Test");
            const std::string opening = "<SpaceUnit Name=\"Test_Frigate\">";
            const auto start = xml.find(opening);
            if (adjustment != 0) xml.insert(start + opening.size(),
                "<Ranged_Target_Z_Adjust>" + std::to_string(adjustment) + "</Ranged_Target_Z_Adjust>");
            if (structure) {
                const auto end = xml.find("</SpaceUnit>", start);
                xml.replace(end, std::string_view{"</SpaceUnit>"}.size(), "</SpaceStructure>");
                xml.replace(start, opening.size(), "<SpaceStructure Name=\"Test_Frigate\">");
            }
            write_fixture(tree.root, xml);
            const auto loaded = load(tree.root, models(-40.0F));
            if (!loaded.tables) continue;
            const auto combat = eawr::units::combat_table(*loaded.tables);
            expect(static_cast<bool>(combat), "WWP-72: adjusted XML binds combat profiles");
            if (!combat) continue;
            const auto* bound = combat.value().find(eawr::assets::object_type_crc("Test_Frigate"));
            expect(bound != nullptr, "WWP-72: both space object classes have a combat profile");
            if (!bound) continue;
            auto target_profile = *bound;
            target_profile.hardpoints.clear();
            target_profile.target_bones.clear();
            t::CombatProfile shooter_profile;
            t::WeaponProfile weapon;
            weapon.range = Fixed::from_raw(raw(1000));
            weapon.cone_width = weapon.cone_height = Fixed::from_raw(raw(360));
            weapon.pulse_count = 1;
            shooter_profile.weapons = {weapon};
            t::CombatState state;
            state.direct = true;
            state.attack_target = 2;
            state.weapons.resize(1);
            state.weapons.front().pulses_left = 1;
            const std::vector<t::Player> players{{1, 1, 1, t::player_flag_commandable},
                {2, 2, 2, t::player_flag_commandable}};
            const std::vector<t::SnapshotPlayer> relations{{1, 1, false}, {2, 2, false}};
            d::CombatUnit shooter;
            shooter.id = 1;
            shooter.owner = shooter.team = 1;
            shooter.profile = &shooter_profile;
            shooter.combat = &state;
            shooter.transform = m::identity_matrix();
            d::CombatUnit target;
            target.id = 2;
            target.owner = target.team = 2;
            target.visible_to = 1;
            target.position = target.previous_position = at(100, 0, 10);
            target.transform = m::to_matrix(m::identity_quat(), target.position).value();
            target.profile = &target_profile;
            d::CombatWorld world;
            world.players = players;
            world.relationships = relations;
            world.units = {shooter, target};
            for (const auto hardpoint : {0U, t::object_weapon}) {
                shooter_profile.weapons.front().hardpoint = hardpoint;
                const auto step = d::step_combat(world, world.units.front());
                expect(step && step.value().events.size() == 1,
                    "WWP-19/50: hardpoint and object fallback produce a shot");
                if (step && !step.value().events.empty()) expect(step.value().events.front().aim == at(100, 0, 10 + adjustment),
                    "WWP-19/50/72: fallback adds the XML adjustment in world Z without model scaling");
            }
            // The target's adjustment must not displace an explicit attachment aim.
            target_profile.hardpoints = {{0, at(0, 0, 0), true}, {1, at(0, 0, 60), true}};
            state.attack_hardpoint = 0;
            auto step = d::step_combat(world, world.units.front());
            expect(step && step.value().events.size() == 1 && step.value().events.front().aim == target.position,
                "WWP-19/50: ordered attachment is independent of target height adjustment");
            // Use the same loaded adjustment as the shooter's nearest-hardpoint reference.
            shooter_profile = *bound;
            shooter_profile.weapons = {weapon};
            shooter_profile.weapons.front().hardpoint = 0;
            world.units.front().position = at(0, 0, 10);
            world.units.front().transform = m::to_matrix(m::identity_quat(), world.units.front().position).value();
            state.attack_hardpoint = t::no_hardpoint;
            step = d::step_combat(world, world.units.front());
            const auto nearest = adjustment > 30 ? 1U : 0U;
            expect(step && step.value().events.size() == 1 && step.value().events.front().target_hardpoint == nearest,
                "WWP-16/72: nearest attachment uses shooter height, with authored order on an exact tie");
            target_profile.hardpoints.clear();
            d::CollectionTrees collection;
            world.projectile_collection = &collection;
            t::ShotProfile shot;
            shot.homing = true;
            shot.turn_rate = Fixed::from_raw(raw(360));
            shot.speed = Fixed::from_raw(raw(1));
            shot.max_travel = Fixed::from_raw(raw(1000));
            t::CombatEvent event;
            event.target = 2;
            event.target_hardpoint = t::no_hardpoint;
            event.aim = target.position;
            const auto missile = d::launch_projectile(event, shot, 1, false, 1, {});
            expect(static_cast<bool>(missile), "WWP-64: missile contract launches");
            if (!missile) continue;
            d::ProjectileScratch scratch;
            const auto steered = d::step_projectile(world, missile.value(), scratch);
            event.aim = at(100, 0, 10 + adjustment);
            const auto expected = d::launch_projectile(event, shot, 1, false, 2, {});
            expect(steered && expected && steered.value().projectile.pitch == expected.value().pitch
                && steered.value().projectile.step == expected.value().step,
                "WWP-64/72 MS-07: no-hardpoint missile pursues the height-adjusted point");
            expect(world.units.back().position == target.position && world.units.back().transform == target.transform,
                "WWP-72: aiming leaves position and collision/model transform unchanged");
        }
    }
}


void living_collision_admission() {
    namespace t = eawr::sim::tactical;
    namespace d = t::detail;
    const auto id = [](const char* name) { return eawr::assets::object_type_crc(name); };
    for (const auto& [team_tags, admitted] : {
        std::tuple{std::string{}, false},
        std::tuple{std::string{"<Collidable_By_Projectile_Living>yes</Collidable_By_Projectile_Living>"}, true},
        std::tuple{std::string{"<Collidable_By_Projectile_Living>no</Collidable_By_Projectile_Living>"}, false}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto insert = [&](const std::string_view opening, const std::string& tags) {
            xml.insert(xml.find(opening) + opening.size(), tags);
        };
        // WBP-51: the inherited team flag wins over a contradictory purchase-template flag.
        insert("<Squadron Name=\"Test_Squadron\">",
            "<Collidable_By_Projectile_Living>yes</Collidable_By_Projectile_Living>");
        insert("<Container Name=\"Test_Pair_Base_Container\">", team_tags);
        const auto ship_flag = xml.find("<Collidable_By_Projectile_Living>yes</Collidable_By_Projectile_Living>",
            xml.find("<SpaceUnit Name=\"Test_Frigate\">"));
        xml.erase(ship_flag, std::string_view{"<Collidable_By_Projectile_Living>yes</Collidable_By_Projectile_Living>"}.size());
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        if (!loaded.tables) continue;
        const auto& tables = *loaded.tables;
        const auto* team = tables.find("Test_Squadron_Pair");
        const auto* craft = tables.find("Test_Fighter");
        const auto* ship = tables.find("Test_Frigate");
        expect(team && craft && ship && team->living_projectile_collision == admitted
            && craft->living_projectile_collision && !ship->living_projectile_collision,
            "WBP-50/51: inherited team flags and craft admission are independent; omission is false");
        const auto combat = eawr::units::combat_table(tables);
        eawr::skirmish::SkirmishStart start;
        start.setup.players = {{1, 1, 1, 1}, {2, 2, 1, 1}, {3, 3, 1, 0}};
        const auto economy = eawr::skirmish::economy_rules(start, {}, tables);
        expect(combat && economy, "WBP-51: loaded collision data binds both consumer tables");
        if (!combat || !economy) continue;
        expect(!combat.value().find(id("Test_Squadron_Pair")),
            "WAD-18: the squadron container has no substitute combat damage pool");
        const auto* craft_profile = combat.value().find(id("Test_Fighter"));
        const auto* ship_profile = combat.value().find(id("Test_Frigate"));
        expect(craft_profile && ship_profile && craft_profile->living_projectile_collision
            && !ship_profile->living_projectile_collision, "WAD-14: combat applies resolved collision defaults");
        if (!craft_profile || !ship_profile || admitted) continue;
        const t::CaptureProfile pad{900, Fixed::from_raw(raw(100)), Fixed{}, {1}};
        const std::array<t::CaptureCandidate, 2> candidates{{
            {{1, 2, {}}, id("Test_Squadron_Pair")}, {{2, 1, {}}, id("Test_Fighter")}}};
        t::PadState state;
        expect(t::service_capture(pad, state, 3, 3, start.setup.players, candidates,
            economy.value().pads, 99, {}) == 1,
            "WBP-51: an enemy container cannot contest the craft's capture");
        expect(t::pad_construction_allowed(pad, {}, 1, start.setup.players,
            std::span{candidates}.first(1), economy.value().pads, 99, {}),
            "WBP-51: an enemy container cannot block construction");
        state = {};
        expect(t::service_capture(pad, state, 3, 3, start.setup.players,
            std::span{candidates}.first(1), economy.value().pads, 99, {}) == 3,
            "WBP-51: a container alone cannot capture");
        auto hostile_craft = candidates;
        hostile_craft[0].type = id("Test_Fighter");
        expect(!t::pad_construction_allowed(pad, {}, 1, start.setup.players,
            std::span{hostile_craft}.first(1), economy.value().pads, 99, {}),
            "WBP-07: an enemy craft still blocks construction");
        d::CombatWorld world;
        world.players = start.setup.players;
        const std::array<t::SnapshotPlayer, 3> relationships{{{1, 1, false}, {2, 2, false}, {3, 3, true}}};
        world.relationships = relationships;
        world.table = &combat.value();
        for (const auto& [entity, profile] : {
            std::pair{eawr::sim::EntityId{1}, static_cast<const t::CombatProfile*>(nullptr)},
            std::pair{eawr::sim::EntityId{2}, craft_profile}, std::pair{eawr::sim::EntityId{3}, ship_profile}}) {
            d::CombatUnit unit;
            unit.id = entity;
            unit.owner = 2;
            unit.profile = profile;
            world.units.push_back(unit);
        }
        const std::array<t::SpaceBody, 3> bodies{{{1, 2, {}}, {2, 2, {}}, {3, 2, {}}}};
        auto index = t::SpaceIndex::build(bodies);
        expect(static_cast<bool>(index), "WAD-18: consumer broad phase builds");
        if (!index) continue;
        world.index = std::move(index).value();
        t::Projectile projectile;
        projectile.owner = 1;
        projectile.blast.damage = Fixed::from_raw(raw(12));
        projectile.blast.radius = Fixed::from_raw(raw(80));
        projectile.blast.max_delay = {};
        const auto blast = d::prepare_blast(world, projectile, {});
        expect(blast && blast.value().players[1].recipients.size() == 1
            && blast.value().players[1].recipients.front().id == 2
            && blast.value().players[1].recipients.front().amount == projectile.blast.damage,
            "WAD-14/18: blast hits craft once, excludes the container and omitted ship flag");
        for (const bool craft_near : {false, true}) {
            auto setup = start.setup;
            setup.units = {{1, id("Test_Squadron_Pair"), 2, {}, eawr::sim::math::identity_quat(), {}},
                {2, id("Test_Fighter"), 1, {Fixed::from_raw(raw(craft_near ? 0 : 500)), {}, {}},
                    eawr::sim::math::identity_quat(), {}},
                {99, pad.type, 3, {}, eawr::sim::math::identity_quat(), {}}};
            t::EconomyRules rules;
            rules.pads = economy.value().pads;
            rules.pads.neutral = 3;
            rules.pads.capture = {pad};
            std::vector<std::string> reference;
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                auto session = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, rules);
                expect(static_cast<bool>(session), "WBP-51: craft/container capture session binds");
                if (!session) { std::cerr << session.error().message << '\n'; continue; }
                const eawr::platform::ThreadWorkerAdapter executor(workers);
                std::vector<std::string> hashes;
                for (unsigned frame = 0; frame < 90; ++frame) {
                    const auto stepped = session.value().step(executor);
                    expect(static_cast<bool>(stepped), "WBP-51: capture session advances");
                    if (!stepped) break;
                    hashes.push_back(stepped.value().state_sha256 + stepped.value().snapshot->sha256());
                }
                const auto live = session.value().units();
                const auto point = std::find_if(live.begin(), live.end(), [](const auto& unit) { return unit.entity_id == 99; });
                expect(point != live.end() && point->owner == (craft_near ? 1U : 3U),
                    "WBP-51: craft capture a pad in session; a nearby team alone never captures");
                if (workers == 1) reference = hashes;
                else expect(hashes == reference, "WBP-51: capture hashes match at 1/2/4/8 workers");
            }
        }
    }
}

// AU-82 boolean and spawned-pair parsing. M2 single/list inheritance remains unchanged.
void retail_value_rules() {
    for (const auto style : {std::string_view{}, std::string_view{"Space_Automatic"}, std::string_view{"User_Input"}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string_view tag = "<SpaceUnit Name=\"Test_Frigate\">";
        const auto position = xml.find(tag);
        expect(position != std::string::npos, "WAB-36: target-handler fixture has its unit");
        if (position == std::string::npos) continue;
        const std::string fields = R"(<Unit_Ability>
          <Type>TRACTOR_BEAM</Type><Recharge_Seconds>20</Recharge_Seconds>
          <GUI_Activated_Ability_Name>Test_Beam</GUI_Activated_Ability_Name>
          </Unit_Ability>)";
        const auto list_end = xml.find("</Unit_Abilities_Data>", position);
        expect(list_end != std::string::npos, "WAB-36: target-handler fixture has its ability list");
        if (list_end == std::string::npos) continue;
        xml.insert(list_end, fields);
        if (!style.empty()) xml.insert(position + tag.size(), "<Abilities SubObjectList=\"Yes\"><Tractor_Beam_Attack_Ability Name=\"Test_Beam\">"
            "<Activation_Style>" + std::string(style) + "</Activation_Style><Activation_Max_Range>500</Activation_Max_Range>"
            "<Target_Speed_Decrease_Percent>0.5</Target_Speed_Decrease_Percent></Tractor_Beam_Attack_Ability></Abilities>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WAB-36: target-handler fixture loads");
        if (!loaded.tables) continue;
        const auto* ship = loaded.tables->find("Test_Frigate");
        const bool retained = ship && std::any_of(ship->abilities.begin(), ship->abilities.end(), [](const auto& ability) {
            return ability.type == "TRACTOR_BEAM";
        });
        expect(ship && retained == !style.empty(), "WAB-36: only the unresolved ability row is skipped; its unit remains");
        expect(ship && std::any_of(ship->abilities.begin(), ship->abilities.end(), [](const auto& ability) {
            return eawr::sim::tactical::ability_kind(ability.type) == eawr::sim::tactical::AbilityKind::defend;
        }), "WAB-36: an independent valid ability remains on the same unit");
        expect(static_cast<bool>(eawr::units::ability_table(*loaded.tables)),
            "WAB-36: missing or space-automatic handlers never refuse the whole loaded closure");
        if (style.empty()) expect(std::any_of(loaded.tables->notes.begin(), loaded.tables->notes.end(), [](const auto& row) {
            return row.field == "GUI_Activated_Ability_Name" && row.value == "Test_Beam"
                && row.reason.find("unresolved") != std::string::npos;
        }), "WAB-36: skipping an unresolved row emits a counted diagnostic");
    }
    // ML-24: single values use the last real occurrence, preserving inherited values.
    for (const auto& [base_value, variant_value, expected] : {
        std::tuple{std::string{"Frigate"}, std::string{" \t\n"}, std::string{"Frigate"}},
        std::tuple{std::string{"Frigate"}, std::string{" TBD "}, std::string{"Frigate"}},
        std::tuple{std::string{" "}, std::string{"TBD"}, std::string{}},
        std::tuple{std::string{"Frigate"}, std::string{"Corvette"}, std::string{"Corvette"}},
        std::tuple{std::string{"Frigate"}, std::string{"0"}, std::string{"0"}},
        std::tuple{std::string{"Frigate"}, std::string{"None"}, std::string{"None"}},
        std::tuple{std::string{"Frigate"}, std::string{"No"}, std::string{"No"}},
        std::tuple{std::string{"Frigate"}, std::string{"tbd"}, std::string{"tbd"}}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string layer = "<Space_Layer> Frigate </Space_Layer>";
        xml.replace(xml.find(layer), layer.size(),
            "<Space_Layer>" + variant_value + "</Space_Layer>");
        const std::string opening = "<SpaceUnit Name=\"Test_Frigate\">";
        xml.insert(xml.find(opening) + opening.size(),
            "<Variant_Of_Existing_Type>Layer_Base</Variant_Of_Existing_Type>");
        xml.insert(xml.find('>') + 1, "<SpaceUnit Name=\"Layer_Base\"><Space_Layer>"
            + base_value + "</Space_Layer></SpaceUnit>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* ship = loaded.tables ? loaded.tables->find("Test_Frigate") : nullptr;
        expect(ship && ship->movement.space_layer == expected,
            "ML-24: blank/TBD variants inherit, blank base stays absent, real override wins");
    }
    {
        TempTree tree;
        write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
        const auto loaded = load(tree.root, models(-40.0F));
        expect(static_cast<bool>(loaded.tables), "ML-24: scenery fixture loads");
        if (loaded.tables) {
            auto tables = *loaded.tables;
            eawr::units::ObstacleType scenery;
            scenery.id = "Layer_Only_Scenery";
            scenery.space_layer = "StaticObject";
            scenery.footprint.obstacle_radius = Fixed{};
            scenery.footprint.collision_x = Fixed::from_raw(raw(1));
            scenery.footprint.collision_y = Fixed::from_raw(raw(1));
            tables.obstacles.push_back(scenery);
            const auto motion = eawr::units::motion_table(tables);
            expect(static_cast<bool>(motion), "ML-24: zero-radius static scenery binds motion");
            if (motion) {
                const auto id = eawr::assets::object_type_crc(scenery.id);
                const auto* footprint = motion.value().footprint(id);
                expect(footprint && footprint->layer == eawr::sim::tactical::SpaceLayer::static_object
                    && !footprint->obstacle && footprint->radius == Fixed{}
                    && motion.value().find(id) == nullptr,
                    "ML-24: a scenery layer adds neither collision radius nor obstacle/motion participation");
                expect(static_cast<bool>(eawr::sim::tactical::validate_motion(motion.value())),
                    "ML-24: zero-radius static scenery bypasses destination-search rings");
                tables.obstacles.back().scale_factor = Fixed::from_raw(raw(2));
                tables.obstacles.back().footprint.custom_soft_radius = Fixed::from_raw(raw(40));
                tables.obstacles.back().footprint.custom_hard_x = Fixed::from_raw(raw(12));
                tables.obstacles.back().footprint.custom_hard_y = Fixed::from_raw(raw(18));
                const auto custom = eawr::units::motion_table(tables);
                expect(static_cast<bool>(custom), "ML-24: authored scenery footprints bind motion");
                const auto* authored = custom ? custom.value().footprint(id) : nullptr;
                expect(authored && authored->radius == Fixed::from_raw(raw(80)),
                    "ML-24: scenery retains custom soft radius times scale");
                expect(authored && authored->x_extent == Fixed::from_raw(raw(24))
                    && authored->y_extent == Fixed::from_raw(raw(36)),
                    "ML-24: scenery retains the custom hard pair times scale");
                tables.obstacles.back() = scenery;
                tables.obstacles.back().footprint.space_obstacle = true;
                expect(!eawr::units::motion_table(tables),
                    "ML-24: SPACE_OBSTACLE retains the model fallback and its small-radius rejection");
            }
        }
    }
    // ML-26: exercise inheritance through the skirmish loader, including placeholder-only overlays.
    for (const auto& authored : {std::string{}, std::string{"<Squadron_Units> </Squadron_Units>"},
                                std::string{"<Squadron_Units>TBD</Squadron_Units>"}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto opening = xml.find("<Squadron Name=\"Test_Squadron_Pair\">");
        const auto end = xml.find("</Squadron>", opening);
        const auto first = xml.find("  <Squadron_Units>", opening);
        const auto offsets = xml.find("  <Squadron_Offsets>", first);
        expect(first < offsets && offsets < end, "ML-26: variant fixture member range exists");
        if (first >= offsets || offsets >= end) continue;
        xml.replace(first, offsets - first, authored);
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* pair = loaded.tables ? loaded.tables->find("Test_Squadron_Pair") : nullptr;
        expect(pair && pair->members.size() == 3 && pair->members[0].craft == "Test_Fighter"
            && pair->members[2].craft == "Test_Fighter",
            "ML-26: omitted, empty and TBD variants inherit all base squadron members");
    }
    const std::string variant = "<Squadron Name=\"Test_Squadron_Pair\">";
    const std::string base = "<Squadron Name=\"Test_Squadron\">";
    const auto with = [&](const std::vector<std::pair<std::string, std::string>>& inserts) {
        auto xml = units_xml("3600", "Unit_Select_Test");
        for (const auto& [opening, tags] : inserts) xml.insert(xml.find(opening) + opening.size(), tags);
        return xml;
    };
    // Boolean spellings read Y, T or 1 as true and other non-placeholder values as false.
    for (const auto& [base_tag, variant_tag, expected] : {
        std::tuple{std::string{"<Is_Homogeneous>no</Is_Homogeneous>"}, std::string{"<Is_Homogeneous>Y</Is_Homogeneous>"}, true},
        std::tuple{std::string{"<Is_Homogeneous>no</Is_Homogeneous>"}, std::string{"<Is_Homogeneous>trUE</Is_Homogeneous>"}, true},
        std::tuple{std::string{}, std::string{"<Is_Homogeneous>Nope</Is_Homogeneous>"}, false},
        std::tuple{std::string{}, std::string{"<Is_Homogeneous>0</Is_Homogeneous>"}, false}}) {
        TempTree tree;
        write_fixture(tree.root, with({{base, base_tag}, {variant, variant_tag}}));
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* pair = loaded.tables ? loaded.tables->find("Test_Squadron_Pair") : nullptr;
        expect(pair && pair->homogeneous == expected && pair->affiliation == "Rebel",
            "AU-82: " + base_tag + " then " + variant_tag);
        expect(loaded.tables && !has_row(loaded.tables->unresolved, "Test_Squadron_Pair", "Is_Homogeneous"),
            "AU-82: any boolean spelling reads without an unresolved row");
    }
    {
        // Each spawned-unit occurrence reads its first type and count pair.
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string authored = "<Starting_Spawned_Units_Tech_0>Test_Squadron, 2</Starting_Spawned_Units_Tech_0>";
        const auto at = xml.find(authored);
        expect(at != std::string::npos, "AU-82: the station authors one starting list");
        if (at == std::string::npos) return;
        xml.replace(at, authored.size(), "<Starting_Spawned_Units_Tech_0>Test_Squadron, 2, Test_Fighter, 5"
            "</Starting_Spawned_Units_Tech_0><Starting_Spawned_Units_Tech_0>Test_Squadron, 1</Starting_Spawned_Units_Tech_0>"
            "<Starting_Spawned_Units_Tech_0>TBD</Starting_Spawned_Units_Tech_0>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* station = loaded.tables ? loaded.tables->find("Skirmish_Test_Base") : nullptr;
        const auto* spawner = station && station->spawner ? &*station->spawner : nullptr;
        expect(spawner && spawner->starting.size() == 2 && spawner->starting[0].squadron == "Test_Squadron"
            && spawner->starting[0].count == 2 && spawner->starting[1].count == 1,
            "AU-82: one pair per spawned-unit occurrence; malformed pairs stay out");
        expect(loaded.tables && has_row(loaded.tables->notes, "Skirmish_Test_Base", "Starting_Spawned_Units_Tech_0"),
            "AU-82: the ignored pairs are noted");
        expect(loaded.tables && has_row(loaded.tables->unresolved, "Skirmish_Test_Base", "Starting_Spawned_Units_Tech_0", "TBD"),
            "M2: malformed spawned pairs remain diagnosed under existing list selection");
    }
}

// WSQ-60: container type defaults and inherited overrides are independent of the roster.
void squadron_container_health() {
    for (const auto& [tags, hull, shield] : {
        std::tuple{std::string{}, 100, std::optional<Fixed>{}},
        std::tuple{std::string{"<Tactical_Health>240</Tactical_Health><Shield_Points>25</Shield_Points>"},
            240, std::optional{Fixed::from_raw(raw(25))}},
        std::tuple{std::string{"<Tactical_Health>0</Tactical_Health><Shield_Points>0</Shield_Points>"},
            0, std::optional{Fixed{}}}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto begin = xml.find("<Container Name=\"Test_Pair_Base_Container\">");
        xml.insert(begin + std::string_view{"<Container Name=\"Test_Pair_Base_Container\">"}.size(), tags);
        // Dummy squadron health is not the health of the spawned container.
        const auto company = xml.find("<Squadron Name=\"Test_Squadron_Pair\">");
        xml.insert(company + std::string_view{"<Squadron Name=\"Test_Squadron_Pair\">"}.size(),
            "<Tactical_Health>999</Tactical_Health><Shield_Points>999</Shield_Points>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        if (!loaded.tables) continue;
        const auto* team = loaded.tables->find("Test_Squadron_Pair");
        expect(team && team->team_hull == std::optional{Fixed::from_raw(raw(hull))}
            && team->team_shield_points == shield && !team->hull,
            "WSQ-60: container defaults, inherited values and zero override the dummy company");
        const auto durability = eawr::units::durability_table(*loaded.tables);
        expect(durability && !durability.value().find(eawr::assets::object_type_crc("Test_Squadron_Pair"))
            && durability.value().find(eawr::assets::object_type_crc("Test_Fighter")),
            "WSQ-60: container reporting does not give the team a second combat damage pool");
        auto metadata = *loaded.tables;
        metadata.units[2].team_hull = Fixed::from_raw(raw(42));
        expect(eawr::units::content_identity(metadata) == eawr::units::content_identity(*loaded.tables),
            "WSQ-60: reporting metadata leaves the authoritative replay identity unchanged");
    }
}

// W-09: omitted Fires_Forward still applies authored extents, and each omitted extent
// independently uses its debug-build default. Explicit yes keeps the facing-only path.
void object_weapon_defaults() {
    for (const auto& [tags, yaw, pitch] : {
             std::tuple{std::string{"<Turret_Rotate_Extent_Degrees>45</Turret_Rotate_Extent_Degrees>"
                                   "<Turret_Elevate_Extent_Degrees>45</Turret_Elevate_Extent_Degrees>"}, 45, 45},
             std::tuple{std::string{}, 360, 180},
             std::tuple{std::string{"<Turret_Rotate_Extent_Degrees>45</Turret_Rotate_Extent_Degrees>"}, 45, 180},
             std::tuple{std::string{"<Turret_Elevate_Extent_Degrees>45</Turret_Elevate_Extent_Degrees>"}, 360, 45},
             std::tuple{std::string{"<Fires_Forward>no</Fires_Forward>"}, 360, 180},
             std::tuple{std::string{"<Fires_Forward>yes</Fires_Forward>"
                                   "<Turret_Rotate_Extent_Degrees>45</Turret_Rotate_Extent_Degrees>"
                                   "<Turret_Elevate_Extent_Degrees>45</Turret_Elevate_Extent_Degrees>"}, 0, 0}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto begin = xml.find("<SpaceUnit Name=\"Test_Fighter\">");
        xml.insert(begin + std::string_view{"<SpaceUnit Name=\"Test_Fighter\">"}.size(), tags);
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        if (!loaded.tables) continue;
        const auto* fighter = loaded.tables->find("Test_Fighter");
        expect(fighter && fighter->weapon, "W-09: default test has an object weapon");
        if (!fighter || !fighter->weapon) continue;
        const auto& weapon = *fighter->weapon;
        expect(weapon.cone_width_degrees == (yaw == 0 ? std::nullopt : std::optional{Fixed::from_raw(raw(yaw))})
                   && weapon.cone_height_degrees == (pitch == 0 ? std::nullopt : std::optional{Fixed::from_raw(raw(pitch))}),
               "W-09: absent Fires_Forward and extents use independent defaults; explicit yes skips the cone");
    }
}

// WWP-47/48 (walks/weapons.md U-06): the object weapon's muzzle bones are the prefix with _00 and
// consecutive two-digit suffixes; a gap ends the list, a missing _00 empties it, and the shot's
// travel is the type's Targeting_Max_Attack_Distance (450 here, the projectile authors 500).
void object_weapon_muzzles() {
    struct Case {
        std::string tag;
        std::vector<std::string> bones;
        std::size_t expected;
    };
    const std::vector<Case> cases{
        {"", {"MuzzleA_00", "MuzzleA_01", "MuzzleA_02", "MuzzleA_03"}, 4},
        {"", {"MUZZLEA_00", "MUZZLEA_01"}, 2},
        {"", {"MuzzleA_00", "MuzzleA_01", "MuzzleA_03"}, 2},
        {"", {"MuzzleA_01", "MuzzleA_02"}, 0},
        {"", {"MuzzleB_00", "MuzzleB_01"}, 0},
        {"<Muzzle_Bone_Name>Gun</Muzzle_Bone_Name>", {"Gun_00", "Gun_01", "MuzzleA_00"}, 2},
        {"", {}, 0}};
    for (const auto& item : cases) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string opening = "<SpaceUnit Name=\"Test_Fighter\">";
        xml.insert(xml.find(opening) + opening.size(), item.tag);
        write_fixture(tree.root, xml);
        auto assets = models(-40.0F);
        std::vector<eawr::assets::Bone> bones{bone("Root", -1, {0, 0, 0})};
        float offset = 3.0F;
        for (const auto& name : item.bones) bones.push_back(bone(name, 0, {offset++, 1.0F, 0.0F}));
        assets.insert_or_assign("data/art/models/test_fighter.alo", model("data/art/models/test_fighter.alo", bones));
        const auto loaded = load(tree.root, assets);
        expect(loaded.tables.has_value(), "WWP-47: muzzle fixture loads");
        if (!loaded.tables) continue;
        const auto* fighter = loaded.tables->find("Test_Fighter");
        expect(fighter != nullptr && fighter->muzzle_bones.size() == item.expected,
            "WWP-47/U-06: muzzle bones are the prefix with consecutive _00.. suffixes, stopping at the first gap");
        const auto combat = eawr::units::combat_table(*loaded.tables);
        expect(static_cast<bool>(combat), "WWP-47: muzzle content binds combat");
        if (!combat || fighter == nullptr) continue;
        const auto* profile = combat.value().find(eawr::assets::object_type_crc("Test_Fighter"));
        expect(profile != nullptr && !profile->weapons.empty() && profile->weapons.back().hardpoint == eawr::sim::tactical::object_weapon
                && profile->weapons.back().muzzles.size() == item.expected,
            "WWP-47: the object weapon profile carries the discovered muzzles in order");
        if (profile != nullptr && !profile->weapons.empty() && item.expected > 1) {
            const auto& muzzles = profile->weapons.back().muzzles;
            expect(muzzles[0] != muzzles[1], "WWP-47: successive muzzle bones are distinct points");
        }
        expect(profile != nullptr && !profile->weapons.empty() && profile->weapons.back().shot
                && profile->weapons.back().shot->max_travel == Fixed::from_raw(raw(450)),
            "WWP-48: the unit weapon's travel is Targeting_Max_Attack_Distance, not Projectile_Max_Flight_Distance");
    }
    // The muzzle list is gameplay content: it enters content identity.
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    auto assets = models(-40.0F);
    assets.insert_or_assign("data/art/models/test_fighter.alo",
        model("data/art/models/test_fighter.alo", {bone("Root", -1, {0, 0, 0}), bone("MuzzleA_00", 0, {3, 1, 0})}));
    const auto with = load(tree.root, assets);
    const auto without = load(tree.root, models(-40.0F));
    expect(with.tables && without.tables
            && eawr::units::content_identity(*with.tables) != eawr::units::content_identity(*without.tables),
        "WWP-47: muzzle bones enter content identity");
}

void presentation_admission() {
    // WSU-13: inherited map-prop admission preserves impassable and behaviour opt-ins.
    for (const auto& [tags, selectable, mouse] : {
        std::tuple{std::string{}, false, false},
        std::tuple{std::string{"<Is_Asteroid_Field>yes</Is_Asteroid_Field>"}, false, false},
        std::tuple{std::string{"<Is_Impassable_Asteroid>yes</Is_Impassable_Asteroid>"
            "<Is_Valid_Target>no</Is_Valid_Target>"}, false, true},
        std::tuple{std::string{"<Collidable_By_Projectile_Living>yes</Collidable_By_Projectile_Living>"}, false, true},
        std::tuple{std::string{"<Collidable_By_Projectile_Living>yes</Collidable_By_Projectile_Living>"
            "<Is_Valid_Target>no</Is_Valid_Target>"}, false, false},
        std::tuple{std::string{"<Behavior>SELECTABLE</Behavior>"}, true, true},
        std::tuple{std::string{"<Behavior>TACTICAL_BUILD_OBJECTS</Behavior>"}, false, true},
        std::tuple{std::string{"<Behavior>DUMMY_TOOLTIP</Behavior>"}, false, true}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        xml.insert(xml.rfind("</Objects>"), "<SpaceProp Name=\"Test_Mouse_Field_Base\">" + tags
            + "</SpaceProp><SpaceProp Name=\"Test_Mouse_Field\">"
            "<Variant_Of_Existing_Type>Test_Mouse_Field_Base</Variant_Of_Existing_Type></SpaceProp>");
        write_fixture(tree.root, xml);
        const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
        auto filesystem = eawr::vfs::Vfs::mount(mounts);
        expect(static_cast<bool>(filesystem), "map-prop admission fixture mounts");
        if (!filesystem) continue;
        auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
        expect(static_cast<bool>(catalog), "map-prop admission catalog loads");
        if (!catalog) continue;
        eawr::units::LoadInput input;
        input.catalog = &catalog.value().catalog;
        input.filesystem = &filesystem.value();
        input.types = {"Test_Frigate"};
        input.obstacles = {"Test_Mouse_Field"};
        auto loaded = eawr::units::load_unit_tables(input);
        expect(loaded && loaded.value().obstacles.size() == 1, "map-prop admission profile loads");
        if (!loaded || loaded.value().obstacles.size() != 1) continue;
        const auto& obstacle = loaded.value().obstacles.front();
        expect(obstacle.selectable == selectable && obstacle.mouse_sensitive == mouse,
            "WSU-13: neutral fields pass through, while impassable and opted-in props stay pickable");
        auto changed = loaded.value();
        changed.obstacles.front().selectable = !selectable;
        changed.obstacles.front().mouse_sensitive = !mouse;
        expect(eawr::units::content_identity(changed) == eawr::units::content_identity(loaded.value()),
            "map-prop mouse admission is presentation-only and leaves replay identity unchanged");
    }
    // WSU-13/50: positive hull alone admits neither a mouse contact nor a hover bar.
    for (const auto& [tags, mouse, bars] : {
        std::tuple{std::string{}, false, false},
        std::tuple{std::string{"<Is_Impassable_Asteroid>yes</Is_Impassable_Asteroid>"
            "<Is_Valid_Target>no</Is_Valid_Target>"}, true, false},
        std::tuple{std::string{"<Collidable_By_Projectile_Living>yes</Collidable_By_Projectile_Living>"}, true, false},
        std::tuple{std::string{"<Collidable_By_Projectile_Living>yes</Collidable_By_Projectile_Living>"
            "<Is_Valid_Target>no</Is_Valid_Target>"}, false, false},
        std::tuple{std::string{"<Behavior>TACTICAL_BUILD_OBJECTS</Behavior>"}, true, false},
        std::tuple{std::string{"<Behavior>DUMMY_TOOLTIP</Behavior>"}, true, false},
        std::tuple{std::string{"<Is_Generic_Hero>yes</Is_Generic_Hero>"}, false, true},
        std::tuple{std::string{"<Is_Named_Hero>yes</Is_Named_Hero>"}, false, true},
        std::tuple{std::string{"<Behavior>TACTICAL_UNDER_CONSTRUCTION</Behavior>"}, false, true}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto begin = xml.find("<StarBase Name=\"Test_Base\">");
        xml.insert(begin + std::string_view{"<StarBase Name=\"Test_Base\">"}.size(), tags);
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        if (!loaded.tables) continue;
        const auto* station = loaded.tables->find("Skirmish_Test_Base");
        expect(station && station->mouse_sensitive == mouse && station->bar_admitted == bars,
            "WSU-13/50: inherited data and verified defaults distinguish mouse contact from bar admission");
    }
}


} // namespace unit_tables_test_support
