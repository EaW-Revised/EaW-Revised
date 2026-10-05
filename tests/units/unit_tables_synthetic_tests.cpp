#include "unit_tables_support.hpp"
#include "eawr/skirmish/start.hpp"
#include "../../src/sim/tactical/blast_internal.hpp"

namespace unit_tables_test_support {

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

void print_rows(const std::string_view label, const std::vector<eawr::units::Unresolved>& rows) {
    for (const auto& row : rows) {
        std::cout << "  " << label << ' ' << row.owner << ' ' << row.field << " '" << row.value << "': " << row.reason << '\n';
    }
}

void mass_driver_type() {
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    std::string points{hardpoints_xml};
    const auto type = points.find("HARD_POINT_WEAPON_LASER");
    points.replace(type, std::string_view{"HARD_POINT_WEAPON_LASER"}.size(), "HARD_POINT_WEAPON_MASS_DRIVER");
    write(tree.root / "XML" / "eawr_hardpoints.xml", points);
    const auto loaded = load(tree.root, models(-40.0F));
    if (!loaded.tables) return;
    const auto& tables = *loaded.tables;
    const auto* unit = tables.find("Test_Frigate");
    expect(unit && !unit->hardpoints.empty()
            && unit->hardpoints[0].type == eawr::units::HardpointType::weapon_mass_driver
            && unit->hardpoints[0].weapon.has_value(), "MD-01: XML mass-driver type loads its weapon");
    expect(eawr::units::to_string(eawr::units::HardpointType::weapon_mass_driver) == "HARD_POINT_WEAPON_MASS_DRIVER",
        "MD-01: mass-driver type retains its XML name");
    auto combat = eawr::units::combat_table(tables);
    auto health = eawr::units::durability_table(tables);
    expect(combat && health, "MD-01: invented mass-driver carrier binds combat and durability");
    if (!combat || !health) return;
    const auto id = eawr::assets::object_type_crc("Test_Frigate");
    const auto* guns = combat.value().find(id);
    const auto* durability = health.value().find(id);
    expect(guns && unit && guns->hardpoint_meshes.size() == unit->hardpoints.size(),
        "WAD-20/21: mesh selector indices retain every authored hardpoint, including unresolved attachments");
    if (guns && unit && guns->hardpoint_meshes.size() == unit->hardpoints.size()) {
        for (std::size_t index = 0; index < unit->hardpoints.size(); ++index)
            expect(guns->hardpoint_meshes[index] == unit->hardpoints[index].collision_mesh,
                "WAD-20/21: authored mesh selectors survive the tactical profile conversion");
    }
    expect(guns && !guns->weapons.empty() && guns->weapons[0].hardpoint == 0 && guns->weapons[0].shot,
        "MD-01: invented mass-driver hardpoint becomes a firing slot");
    expect(durability && !durability->hardpoints.empty()
            && durability->hardpoints[0].role == eawr::sim::tactical::HardpointRole::weapon,
        "MD-01: invented mass-driver hardpoint has the weapon damage role");
}

void hero_company_tables() {
    for (const auto& [base_tag, variant_tag, expected] : {
        std::tuple{std::string{}, std::string{}, true},
        std::tuple{std::string{"<Is_Homogeneous>yes</Is_Homogeneous>"}, std::string{}, true},
        std::tuple{std::string{"<Is_Homogeneous>no</Is_Homogeneous>"}, std::string{}, false},
        std::tuple{std::string{"<Is_Homogeneous>no</Is_Homogeneous>"},
            std::string{"<Is_Homogeneous>yes</Is_Homogeneous>"}, true}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string base = "<Squadron Name=\"Test_Squadron\">";
        const std::string variant = "<Squadron Name=\"Test_Squadron_Pair\">";
        xml.insert(xml.find(base) + base.size(), base_tag);
        xml.insert(xml.find(variant) + variant.size(), variant_tag);
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables && loaded.tables->find("Test_Squadron_Pair")->homogeneous == expected,
            "WHE-SQ-01: default, inherited and overriding Is_Homogeneous control card folding");
        if (loaded.tables) {
            auto changed = *loaded.tables;
            for (auto& type : changed.units) type.homogeneous = !type.homogeneous;
            expect(eawr::units::content_identity(changed) == eawr::units::content_identity(*loaded.tables),
                "WHE-SQ-01: card folding is presentation metadata and preserves replay content identity");
        }
    }
    for (const bool fighter_locomotor : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        if (fighter_locomotor) {
            const auto locomotor = xml.find("SIMPLE_SPACE_LOCOMOTOR");
            xml.replace(locomotor, std::string_view{"SIMPLE_SPACE_LOCOMOTOR"}.size(), "FIGHTER_LOCOMOTOR");
        }
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        if (!loaded.tables) continue;
        const auto* type = loaded.tables->find("Test_Frigate");
        const auto motion = eawr::units::motion_table(*loaded.tables);
        const auto id = eawr::assets::object_type_crc("Test_Frigate");
        expect(type && type->kind == (fighter_locomotor ? UnitKind::craft : UnitKind::ship) && motion,
            "WHE-SQ-02: locomotor admission is independent of class and category names");
        if (motion) {
            expect((motion.value().find(id) == nullptr) == fighter_locomotor
                && (motion.value().squadrons.find_craft(id) != nullptr) == fighter_locomotor,
                "WHE-SQ-02: fighter locomotor gets fighter flight, simple locomotor gets ship motion");
            const auto* group = motion.value().squadrons.find_squadron(id);
            expect(!fighter_locomotor || (group && group->members == std::vector<eawr::sim::tactical::TypeId>{id}),
                "WHE-SQ-02: parentless fighter profile represents its own one-member flight group");
        }
    }
    for (const bool named_transport : {false, true}) for (const bool unique_first : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto station = xml.find("<StarBase Name=\"Skirmish_Test_Base\">");
        xml.insert(station + std::string_view{"<StarBase Name=\"Skirmish_Test_Base\">"}.size(),
            "<Tactical_Buildable_Objects_Multiplayer>Rebel, Test_Hero_Company</Tactical_Buildable_Objects_Multiplayer>");
        if (named_transport) {
            const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
            xml.insert(ship + std::string_view{"<SpaceUnit Name=\"Test_Frigate\">"}.size(), "<Is_Named_Hero>yes</Is_Named_Hero>");
        }
        const std::string definitions =
            "<HeroCompany Name=\"Test_Hero_Company\"><Affiliation>Rebel</Affiliation>"
            "<Is_Named_Hero>yes</Is_Named_Hero><Company_Transport_Unit>Test_Frigate</Company_Transport_Unit>"
            "<Company_Units>First_Hero, Second_Hero</Company_Units></HeroCompany>"
            "<HeroUnit Name=\"First_Hero\"><Affiliation>Rebel</Affiliation><Is_Generic_Hero>yes</Is_Generic_Hero>"
            + std::string(unique_first ? "<Unique_Space_Unit>Test_Fighter</Unique_Space_Unit>" : "") +
            "</HeroUnit><HeroUnit Name=\"Second_Hero\"><Affiliation>Rebel</Affiliation><Is_Named_Hero>yes</Is_Named_Hero>"
            "<Unique_Space_Unit>Test_Fighter</Unique_Space_Unit></HeroUnit>";
        xml.insert(xml.rfind("</"), definitions);
        write_fixture(tree.root, xml);
        write(tree.root / "XML" / "FactionFiles.xml", "<Faction_Files><File>hero_factions.xml</File></Faction_Files>");
        write(tree.root / "XML" / "hero_factions.xml", "<Factions><Faction Name=\"Rebel\" /></Factions>");
        const auto loaded = load(tree.root, models(-40.0F));
        if (!loaded.tables) continue;
        const auto* company = loaded.tables->find("Test_Hero_Company");
        expect(company && company->company_members.size() == 2, "WHE-05: ordered company members load from production menus");
        if (!company) continue;
        expect(company->deployed_space_type == (!named_transport && unique_first ? "Test_Fighter" : "Test_Frigate")
            && company->creates_carried_heroes == !named_transport,
            "WHE-49: named transport bypasses riders; first qualifying hero alone selects unique ship or fallback");
        const auto* ship = loaded.tables->find("Test_Frigate");
        expect(ship && ship->named_hero == named_transport && !ship->generic_hero,
            "WHE-01/02: company flags do not propagate to ship");
        if (!named_transport) {
            const auto* first = loaded.tables->find("First_Hero");
            expect(first && first->generic_hero && !first->named_hero && !first->hull && first->abilities.empty(),
                "WHE-01/07: carried generic hero keeps its identity without ground combat data in space");
        }
        auto altered = *loaded.tables;
        for (auto& unit : altered.units) if (unit.id == "Test_Hero_Company") unit.creates_carried_heroes = !unit.creates_carried_heroes;
        expect(eawr::units::content_identity(altered) != eawr::units::content_identity(*loaded.tables),
            "WHE-49: deployment policy is bound to replay content identity");
    }
}

void nested_special_tables() {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
    xml.insert(ship + std::string_view{"<SpaceUnit Name=\"Test_Frigate\">"}.size(), R"(
      <Abilities SubObjectList="Yes">
        <Tractor_Beam_Attack_Ability Name="first-handler">
          <Activation_Style>User_Input</Activation_Style><Initially_Enabled>Yes</Initially_Enabled><Causes_Despawn>Yes</Causes_Despawn>
          <Applicable_Unit_Types>Test_Fighter, Test_Fighter</Applicable_Unit_Types>
          <Applicable_Unit_Categories>Frigate</Applicable_Unit_Categories>
          <Excluded_Unit_Types>Test_Fighter</Excluded_Unit_Types><Excluded_Unit_Categories>Fighter</Excluded_Unit_Categories>
        </Tractor_Beam_Attack_Ability>
        <Reduce_Production_Price_Ability Name="unclassified-default">
          <Applicable_Unit_Categories>Frigate</Applicable_Unit_Categories>
        </Reduce_Production_Price_Ability>
      </Abilities>)");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    if (!loaded.tables) return;
    const auto* unit = loaded.tables->find("Test_Frigate");
    expect(unit && unit->special_abilities.size() == 2, "WHE-09/10: typed nested declarations load in authored order");
    if (!unit || unit->special_abilities.size() != 2) return;
    const auto& first = unit->special_abilities.front();
    expect(first.name == "first-handler" && first.service_interval == 1 && first.initially_enabled == true && first.causes_despawn,
        "WHE-09/10: authored gates remain separate from code-derived service intervals");
    expect(eawr::sim::tactical::special_type_matches(first.filter, eawr::assets::object_type_crc("Test_Fighter"), 0)
        && first.filter.applicable_types.size() == 1, "WHE-14: exact XML types are canonical and bypass authored exclusions");
    expect(unit->special_abilities.back().style == eawr::sim::tactical::SpecialActivationStyle::unspecified
        && unit->special_abilities.back().service_interval == 0 && !unit->special_abilities.back().initially_enabled,
        "U-05: missing activation style/default enablement are preserved, without inventing a space policy");
    const auto table = eawr::units::ability_table(*loaded.tables);
    const auto* profile = table ? table.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
    expect(profile && profile->special == unit->special_abilities, "WHE-10: nested profiles bind to session content");
    auto changed = *loaded.tables;
    for (auto& type : changed.units) if (type.id == unit->id) type.special_abilities[0].filter.excluded_categories ^= 1;
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "nested filter content binds to replay identity");
}

void concentrate_fire_tables() {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
    const auto begin = xml.find("<Unit_Abilities_Data", ship);
    const auto end = xml.find("</Unit_Abilities_Data>", begin);
    xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin, R"(
      <Unit_Abilities_Data SubObjectList="Yes"><Unit_Ability>
        <Type>CONCENTRATE_FIRE</Type><Effective_Radius>6000</Effective_Radius>
        <Expiration_Seconds>15</Expiration_Seconds><Recharge_Seconds>20</Recharge_Seconds>
        <GUI_Activated_Ability_Name>focus</GUI_Activated_Ability_Name>
      </Unit_Ability></Unit_Abilities_Data>
      <Abilities SubObjectList="Yes"><Concentrate_Fire_Attack_Ability Name="focus">
        <Activation_Style>User_Input</Activation_Style><Applicable_Unit_Categories>Frigate</Applicable_Unit_Categories>
        <Applicable_Unit_Types/><Stacking_Category>0</Stacking_Category>
        <Target_Damage_Increase_Percent>0.5</Target_Damage_Increase_Percent>
        <Target_Speed_Decrease_Percent>0</Target_Speed_Decrease_Percent>
      </Concentrate_Fire_Attack_Ability></Abilities>)");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "WHE-24/25: concentrate-fire authored data loads");
    if (!loaded.tables) return;
    const auto table = eawr::units::ability_table(*loaded.tables);
    const auto* profile = table ? table.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
    expect(profile && profile->abilities.size() == 1 && profile->special.size() == 1,
        "named concentrate-fire declaration binds to its ordinary timer slot");
    if (!profile || profile->abilities.empty() || profile->special.empty()) return;
    expect(profile->abilities[0].effective_radius == Fixed::from_raw(raw(6000))
        && profile->abilities[0].expiration_frames == 450 && profile->abilities[0].recharge_frames == 600
        && profile->abilities[0].gui_activated_ability_name == "focus"
        && profile->special[0].target_damage_increase == Fixed::from_decimal("0.5").value()
        && profile->special[0].target_speed_decrease == Fixed{}, "radius, binding, timers and target modifiers are data-driven");
    auto changed = *loaded.tables;
    for (auto& unit : changed.units) if (unit.id == "Test_Frigate") unit.abilities[0].effective_radius = Fixed::from_raw(raw(3000));
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "replay content identity binds concentrate-fire radius");
}

void rocket_variant_inputs() {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const std::string category = "<Projectile_Category>Laser</Projectile_Category>";
    xml.replace(xml.find(category), category.size(), "<Projectile_Category>ROCKET</Projectile_Category>");
    const std::string base = "<Projectile Name=\"Proj_Test_Generic\">";
    xml.insert(xml.find(base) + base.size(),
        "<Projectile_Rocket_Curve_Distance>500</Projectile_Rocket_Curve_Distance>"
        "<Projectile_Rocket_Curve_Offset>0</Projectile_Rocket_Curve_Offset>"
        "<Projectile_Rocket_Straight_Distance>500</Projectile_Rocket_Straight_Distance>"
        "<Explode_When_Reached_Target_Radius>Yes</Explode_When_Reached_Target_Radius>");
    const std::string variant = "<Projectile Name=\"Proj_Test_Small\">";
    xml.insert(xml.find(variant) + variant.size(), "<Max_Speed>12</Max_Speed>"
        "<Projectile_Max_Flight_Distance>3000</Projectile_Max_Flight_Distance>");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "RFL: inherited rocket fixture loads");
    if (!loaded.tables) return;
    auto combat = eawr::units::combat_table(*loaded.tables);
    expect(static_cast<bool>(combat), "RFL: inherited rocket profile binds combat");
    if (!combat) return;
    const auto* craft = combat.value().find(eawr::assets::object_type_crc("Test_Fighter"));
    expect(craft && !craft->weapons.empty() && craft->weapons[0].shot,
        "RFL: inherited rocket supplies an ordinary weapon shot");
    if (!craft || craft->weapons.empty() || !craft->weapons[0].shot) return;
    const auto& shot = *craft->weapons[0].shot;
    expect(!shot.homing && shot.speed == Fixed::from_raw(raw(12)) && shot.flight
        && shot.flight->kind == eawr::sim::tactical::FlightKind::rocket
        && shot.flight->authored_distance == Fixed::from_raw(raw(3000))
        && shot.flight->curve_distance == Fixed::from_raw(raw(500))
        && shot.flight->straight_distance == Fixed::from_raw(raw(500))
        && shot.flight->curve_offset == Fixed{} && shot.flight->target_radius,
        "WAD-04/38/RFL-01..05: variant changes speed/flight while retaining rocket construction, never homing");
    expect(shot.blast.damage == Fixed::from_raw(raw(12)) && shot.blast.radius == Fixed::from_raw(raw(80))
        && shot.blast.tiers == 3, "WAD-38: variant retains independent blast inputs");
    const auto identity = eawr::units::content_identity(*loaded.tables);
    auto changed = *loaded.tables;
    for (auto& projectile : changed.projectiles) {
        if (projectile.id == "Proj_Test_Small") projectile.rocket_straight_distance = Fixed::from_raw(raw(400));
    }
    expect(identity != eawr::units::content_identity(changed), "RFL-02: authored rocket geometry changes content identity");
}

void manual_weapon_loading() {
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    std::string points{hardpoints_xml};
    points.insert(points.find("</HardPoint>"),
        "<Requires_Manual_Target_Assignment>Yes</Requires_Manual_Target_Assignment>"
        "<Manual_Hardpoint_Firing_Cooldown_Secs>120</Manual_Hardpoint_Firing_Cooldown_Secs>"
        "<Projectile_Appearance_Delay_Frames>15</Projectile_Appearance_Delay_Frames>");
    write(tree.root / "XML" / "eawr_hardpoints.xml", points);
    auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "MC-08: synthetic manual hardpoint loads");
    if (!loaded.tables) return;
    auto combat = eawr::units::combat_table(*loaded.tables);
    expect(static_cast<bool>(combat), "MC-08: manual data binds ordinary weapon and player clock independently");
    if (!combat) return;
    const auto* guns = combat.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    const auto slot = std::find_if(guns->weapons.begin(), guns->weapons.end(), [](const auto& weapon) {
        return weapon.hardpoint == 0;
    });
    expect(slot != guns->weapons.end() && slot->requires_manual_target && slot->manual_cooldown_frames == 3600
        && slot->manual_min_range == Fixed{} && slot->shot
        && slot->shot->appearance_delay_frames == 15,
        "MC-08: 120-second player cooldown and 15-frame appearance delay retain authored values; stock minimum defaults zero");
    const auto identity = eawr::units::content_identity(*loaded.tables);
    auto changed = *loaded.tables;
    auto* unit = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
    unit->hardpoints.front().manual_cooldown_seconds = Fixed::from_raw(raw(121));
    expect(identity != eawr::units::content_identity(changed), "MC-08: manual cooldown enters conditional content identity");
    changed = *loaded.tables;
    unit = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
    unit->hardpoints.front().weapon->appearance_delay_frames = 30;
    expect(identity != eawr::units::content_identity(changed), "MC-08: appearance delay enters weapon identity independently");
    unit->hardpoints.front().weapon->appearance_delay_frames = 0;
    const auto zero_identity = eawr::units::content_identity(changed);
    unit->hardpoints.front().weapon->appearance_delay_frames.reset();
    expect(zero_identity == eawr::units::content_identity(changed), "MC-08: zero and absent appearance delay encode identically");
}

void special_weapon_classification() {
    for (const bool projectile : {false, true}) for (const bool manual : {false, true}) {
        TempTree tree;
        write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
        std::string points{hardpoints_xml};
        const std::string original = "HARD_POINT_WEAPON_LASER";
        points.replace(points.find(original), original.size(), "HARD_POINT_WEAPON_SPECIAL");
        if (!projectile) {
            for (const std::string_view tag : {"Fire_Projectile_Type", "Fire_Min_Recharge_Seconds",
                "Fire_Max_Recharge_Seconds", "Fire_Pulse_Count", "Fire_Range_Distance", "Fire_Cone_Width", "Fire_Cone_Height"}) {
                const std::string open = "<" + std::string(tag) + ">";
                const std::string close = "</" + std::string(tag) + ">";
                const auto begin = points.find(open); const auto end = points.find(close, begin);
                if (begin != std::string::npos && end != std::string::npos) points.erase(begin, end + close.size() - begin);
            }
        }
        if (manual) points.insert(points.find("</HardPoint>"),
            "<Requires_Manual_Target_Assignment>Yes</Requires_Manual_Target_Assignment>");
        write(tree.root / "XML" / "eawr_hardpoints.xml", points);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WAD-31: SPECIAL with or without projectile loads");
        if (!loaded.tables) continue;
        auto changed = *loaded.tables;
        auto* changed_unit = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
        if (changed_unit && !changed_unit->hardpoints.empty()) {
            changed_unit->hardpoints[0].requires_manual_target = !manual;
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                "WAD-39: the manual fire guard changes the content identity");
        }
        const auto* unit = loaded.tables->find("Test_Frigate");
        expect(unit && !unit->hardpoints.empty() && unit->hardpoints[0].type == eawr::units::HardpointType::weapon_special
            && unit->hardpoints[0].weapon.has_value() == projectile
            && unit->hardpoints[0].requires_manual_target == manual,
            "WAD-31/39: weapon classification is distinct from activation; projectile-free attachment invents no weapon");
        auto combat = eawr::units::combat_table(*loaded.tables);
        auto health = eawr::units::durability_table(*loaded.tables);
        expect(combat && health, "WAD-31/32: SPECIAL binds ordinary combat and durability");
        if (!combat || !health) continue;
        const auto id = eawr::assets::object_type_crc("Test_Frigate");
        const auto* guns = combat.value().find(id); const auto* durability = health.value().find(id);
        expect(durability && !durability->hardpoints.empty() && durability->hardpoints[0].role
            == eawr::sim::tactical::HardpointRole::weapon, "WAD-31: SPECIAL has ordinary weapon durability role");
        if (!guns) { expect(false, "WAD-32: SPECIAL carrier profile retained"); continue; }
        const auto slot = std::find_if(guns->weapons.begin(), guns->weapons.end(), [](const auto& weapon) {
            return weapon.hardpoint == 0;
        });
        if (!projectile) {
            expect(slot == guns->weapons.end(), "WAD-31: projectile-free leech adds no firing slot or default projectile");
        } else {
            expect(slot != guns->weapons.end() && slot->special && slot->requires_manual_target == manual && slot->shot
                && slot->pulse_count == 5 && slot->pulse_delay_frames == 6 && slot->range == Fixed::from_raw(raw(1100))
                && slot->cone_width == Fixed::from_raw(raw(175)) && slot->cone_height == Fixed::from_raw(raw(160))
                && slot->shot->damage == Fixed::from_raw(raw(40)), "WAD-32/37: SPECIAL uses authored ordinary weapon values");
        }
    }
    expect(eawr::units::to_string(eawr::units::HardpointType::weapon_special) == "HARD_POINT_WEAPON_SPECIAL"
        && eawr::units::HardpointType::weapon_special != eawr::units::HardpointType::enable_special_ability,
        "WAD-31: SPECIAL and ENABLE_SPECIAL_ABILITY remain distinct enums");
}

void barrage_inputs() {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const auto begin = xml.find("<Unit_Abilities_Data");
    const auto end = xml.find("</Unit_Abilities_Data>", begin) + std::string_view{"</Unit_Abilities_Data>"}.size();
    xml.replace(begin, end - begin, R"xml(<Unit_Abilities_Data><Unit_Ability>
        <Type>BARRAGE</Type><Expiration_Seconds>10</Expiration_Seconds><Recharge_Seconds>40</Recharge_Seconds>
        <Mod_Multiplier>FIRE_RATE_MULTIPLIER, 3f</Mod_Multiplier>
        <Projectile_Types_Override>Proj_Test_Small</Projectile_Types_Override>
        <Targeting_Fire_Inaccuracy_Fixed_Radius_Override>320</Targeting_Fire_Inaccuracy_Fixed_Radius_Override>
        <Target_Position_Z_Offset>-150</Target_Position_Z_Offset>
        </Unit_Ability></Unit_Abilities_Data>)xml");
    xml.insert(xml.rfind("</"), R"xml(<MiscObject Name="Dummy_Barrage_Target">
        <Model_Name>test_fighter.alo</Model_Name><SpaceBehavior>MARKER</SpaceBehavior>
        <Scale_Factor>100</Scale_Factor><CategoryMask>Corvette</CategoryMask>
        <Collidable_By_Projectile_Living>Yes</Collidable_By_Projectile_Living>
        <Influences_Capture_Point>No</Influences_Capture_Point><Immune_To_Damage>True</Immune_To_Damage>
        </MiscObject>)xml");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "WAD-38: barrage and marker data load without ordinary hull/locomotor requirements");
    if (!loaded.tables) return;
    auto ability = eawr::units::ability_table(*loaded.tables, {});
    auto combat = eawr::units::combat_table(*loaded.tables);
    auto health = eawr::units::durability_table(*loaded.tables);
    expect(ability && combat && health, "WAD-38: barrage binds existing ability/combat/durability tables");
    if (!ability || !combat || !health) return;
    const auto* profile = ability.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(profile && profile->abilities.size() == 1, "WAD-38: barrage is an existing ability slot");
    if (profile && !profile->abilities.empty()) {
        const auto& barrage = profile->abilities[0];
        expect(barrage.kind == eawr::sim::tactical::AbilityKind::barrage && barrage.expiration_frames == 300
            && barrage.recharge_frames == 1200 && barrage.modifiers.fire_rate == Fixed::from_raw(raw(3))
            && barrage.fixed_inaccuracy == Fixed::from_raw(raw(320)) && barrage.target_z_offset == Fixed::from_raw(raw(-150)),
            "WAD-38: duration, recharge, rate, accuracy and height are retained independently");
    }
    const auto* guns = combat.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(guns && !guns->weapons.empty() && guns->weapons[0].barrage_shot
        && guns->weapons[0].shot && guns->weapons[0].shot->speed != guns->weapons[0].barrage_shot->speed,
        "WAD-38: override has its own shot while normal weapon clocks are retained");
    const auto marker_id = eawr::assets::object_type_crc("Dummy_Barrage_Target");
    const auto* marker = combat.value().find(marker_id);
    expect(marker && marker->category_bits == 8 && marker->living_projectile_collision
        && !health.value().find(marker_id), "WAD-38: enemy marker retains category/collision gate and no damageable hull");
    auto changed = *loaded.tables;
    for (auto& unit : changed.units) for (auto& entry : unit.abilities) {
        if (entry.type == "BARRAGE") entry.fixed_inaccuracy = Fixed::from_raw(raw(321));
    }
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "WAD-38: override-only content changes participate in identity");
}

void synthetic_tables() {
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin,
            "<Create_Team_Type>Test_Squadron</Create_Team_Type><Redirect_Damage_To_Teammates>Yes</Redirect_Damage_To_Teammates>"
            "<Unit_Abilities_Data><Unit_Ability><Type>REPLENISH_WINGMEN</Type><Recharge_Seconds>60</Recharge_Seconds>"
            "<Particle_Effect>fixture_wingmen</Particle_Effect></Unit_Ability></Unit_Abilities_Data>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WHE-63/64 authored wingman fixture loads");
        if (loaded.tables) {
            const auto type = eawr::assets::object_type_crc("Test_Frigate");
            const auto abilities = eawr::units::ability_table(*loaded.tables);
            const auto combat = eawr::units::combat_table(*loaded.tables);
            const auto* bound = abilities ? abilities.value().find(type) : nullptr;
            const auto* damage = combat ? combat.value().find(type) : nullptr;
            expect(bound && bound->abilities.front().replenish_team == eawr::assets::object_type_crc("Test_Squadron")
                && bound->abilities.front().recharge_frames == 1800 && bound->abilities.front().replenish_particle == "fixture_wingmen",
                "WHE-63 creation type, recharge and member emitter are authored");
            expect(damage && damage->redirect_damage_to_teammates, "WHE-64 redirect flag reaches the ordinary combat profile");
            auto changed = *loaded.tables;
            for (auto& unit : changed.units) if (unit.id == "Test_Frigate") unit.redirect_damage_to_teammates = false;
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                "WHE-64 routing flag is replay content identity");
        }
    }
    for (const bool bomb : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        const std::string kind = bomb ? "HARMONIC_BOMB" : "WEAKEN_ENEMY";
        xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin,
            "<Unit_Abilities_Data><Unit_Ability><Type>" + kind + "</Type>"
            "<Spawned_Object_Type>Proj_Test_Small</Spawned_Object_Type><Bomb_Countdown_Seconds>1.5</Bomb_Countdown_Seconds>"
            "<Recharge_Seconds>20</Recharge_Seconds><Effective_Radius>1000</Effective_Radius>"
            "</Unit_Ability></Unit_Abilities_Data>");
        const std::string opening = "<Projectile Name=\"Proj_Test_Small\">";
        xml.insert(xml.find(opening) + opening.size(), R"(<Projectile_Weaken_Enemy_On_Detonation>Yes</Projectile_Weaken_Enemy_On_Detonation>
            <Projectile_Weaken_Enemy_Radius>300</Projectile_Weaken_Enemy_Radius>
            <Projectile_Weaken_Enemy_Duration_Seconds>15</Projectile_Weaken_Enemy_Duration_Seconds>
            <Projectile_Weaken_Enemy_Take_Damage_Increase_Percent>0</Projectile_Weaken_Enemy_Take_Damage_Increase_Percent>
            <Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent>0.2</Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent>
            <Projectile_Weaken_Enemy_Targets_Category_Mask>Frigate</Projectile_Weaken_Enemy_Targets_Category_Mask>
            <Projectile_Weaken_Enemy_Spawn_Effect>fixture_status</Projectile_Weaken_Enemy_Spawn_Effect>)");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "spawned hero authored fixture loads");
        if (!loaded.tables) continue;
        const auto abilities = eawr::units::ability_table(*loaded.tables);
        const auto* hero = abilities ? abilities.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        expect(hero && hero->abilities.size() == 1 && hero->abilities.front().spawned.has_value(), "spawned type binds to ordinary ability slot");
        if (!hero || hero->abilities.empty() || !hero->abilities.front().spawned) continue;
        const auto& profile = *hero->abilities.front().spawned;
        expect(profile.type == eawr::assets::object_type_crc("Proj_Test_Small") && profile.damage == Fixed::from_raw(raw(5))
            && profile.blast.damage == Fixed::from_raw(raw(12)) && hero->abilities.front().recharge_frames == 600
            && profile.countdown_frames == (bomb ? 45U : 0U) && profile.reach == (bomb ? Fixed{} : Fixed::from_raw(raw(1000)))
            && profile.weaken.on_detonation && profile.weaken.radius == Fixed::from_raw(raw(300))
            && profile.weaken.duration_frames == 450 && profile.weaken.cause_damage_reduction == Fixed::from_decimal("0.2").value()
            && profile.weaken.status_effect == "fixture_status", "spawned profile preserves variant damage, countdown, reach and status data");
        auto changed = *loaded.tables;
        changed.projectiles.front().weaken.on_detonation = true;
        changed.projectiles.front().weaken.duration_frames = 1;
        expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables), "spawned status data participates in identity");
    }
    for (const auto energy : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        const std::string kind = energy ? "ENERGY_WEAPON" : "TRACTOR_BEAM";
        const std::string handler = energy ? "Energy_Weapon_Attack_Ability" : "Tractor_Beam_Attack_Ability";
        xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin,
            "<Unit_Abilities_Data><Unit_Ability><Type>" + kind + "</Type><GUI_Activated_Ability_Name>beam</GUI_Activated_Ability_Name>"
            "<Expiration_Seconds>5</Expiration_Seconds><Recharge_Seconds>20</Recharge_Seconds>"
            "<Mod_Multiplier>SPEED_MULTIPLIER,0.6</Mod_Multiplier></Unit_Ability></Unit_Abilities_Data>"
            "<Abilities><" + handler + " Name=\"beam\"><Activation_Style>User_Input</Activation_Style>"
            "<Activation_Min_Range>5</Activation_Min_Range><Activation_Max_Range>800</Activation_Max_Range>"
            "<Applicable_Unit_Categories>Frigate</Applicable_Unit_Categories><Damage_Per_Frame>50</Damage_Per_Frame>"
            "<Stacking_Category>2</Stacking_Category><Target_Speed_Decrease_Percent>0.9</Target_Speed_Decrease_Percent>"
            "<Owner_Particle_Effect>invented_effect</Owner_Particle_Effect><Owner_Particle_Bone_Name>Engine</Owner_Particle_Bone_Name>"
            "</" + handler + "></Abilities>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WHE-26/27: synthetic authored beam data loads");
        if (!loaded.tables) continue;
        const auto table = eawr::units::ability_table(*loaded.tables);
        const auto* profile = table ? table.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        expect(profile && profile->abilities.size() == 1 && profile->special.size() == 1, "named beam binding produces ordinary timer and nested service");
        if (!profile || profile->abilities.empty() || profile->special.empty()) continue;
        const auto& nested = profile->special.front();
        expect(nested.beam_min_range == Fixed::from_raw(raw(5)) && nested.beam_max_range == Fixed::from_raw(raw(800))
            && nested.damage_per_frame == Fixed::from_raw(raw(50)) && nested.target_speed_decrease == Fixed::from_decimal("0.9").value()
            && nested.concentrate_stacking_category == 2 && nested.beam_owner_particle == "invented_effect"
            && nested.beam_owner_bone == "Engine" && profile->abilities.front().modifiers.speed == Fixed::from_decimal("0.6").value(),
            "beam ranges, damage, target reduction, category, particle, bone and source multiplier retain distinct XML values");
        auto changed = *loaded.tables;
        for (auto& unit : changed.units) if (unit.id == "Test_Frigate") unit.special_abilities.front().beam_max_range = Fixed::from_raw(raw(801));
        expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables), "beam content changes replay identity");
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto begin = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        expect(begin != std::string::npos, "WHE-22: synthetic hero hull exists");
        const auto old = xml.find("<Unit_Abilities_Data", begin);
        const auto end = xml.find("</Unit_Abilities_Data>", old);
        expect(old != std::string::npos && end != std::string::npos, "WHE-22: existing fixture ability is found");
        if (old != std::string::npos && end != std::string::npos) xml.replace(old,
            end + std::string_view{"</Unit_Abilities_Data>"}.size() - old, R"(
          <Unit_Abilities_Data SubObjectList="Yes">
            <Unit_Ability><Type>INVULNERABILITY</Type><Expiration_Seconds>6</Expiration_Seconds>
              <Recharge_Seconds>60</Recharge_Seconds><Mod_Multiplier>TAKE_DAMAGE_MULTIPLIER, 0</Mod_Multiplier></Unit_Ability>
            <Unit_Ability><Type>POWER_TO_WEAPONS</Type><Expiration_Seconds>8</Expiration_Seconds>
              <Recharge_Seconds>50</Recharge_Seconds><Mod_Multiplier>CAUSE_DAMAGE_MULTIPLIER, 2.5</Mod_Multiplier></Unit_Ability>
          </Unit_Abilities_Data>)");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WHE-22: zero take-damage modifier parses");
        if (loaded.tables) {
            const auto table = eawr::units::ability_table(*loaded.tables);
            const auto* hero = table ? table.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
            expect(hero && hero->abilities.size() == 2
                && hero->abilities[0].kind == eawr::sim::tactical::AbilityKind::invulnerability
                && hero->abilities[0].expiration_frames == 180 && hero->abilities[0].recharge_frames == 1800
                && hero->abilities[0].modifiers.take_damage == Fixed{}, "WHE-22: Falcon mode binds without loader rejection");
            expect(hero && hero->abilities.size() == 2 && hero->abilities[1].modifiers.cause_damage == Fixed::from_decimal("2.5").value(),
                "WHE-22: recognised POWER_TO_WEAPONS retains the cause-damage modifier");
        }
    }
    hero_company_tables();
    nested_special_tables();
    concentrate_fire_tables();
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string tags = "<Is_Asteroid_Field>Yes</Is_Asteroid_Field><Is_Ion_Storm>Yes</Is_Ion_Storm>"
            "<Is_Nebula>Yes</Is_Nebula><Is_Impassable_Asteroid>Yes</Is_Impassable_Asteroid>"
            "<Space_Obstacle_Offset>12 -7 3</Space_Obstacle_Offset>";
        const auto begin = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        expect(begin != std::string::npos, "WHZ-01: synthetic hazard profile target exists");
        if (begin != std::string::npos) {
            const std::string original = "<SpaceBehavior>TARGETING,REVEAL , HIDE_WHEN_FOGGED, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>";
            xml.replace(xml.find(original, begin), original.size(),
                "<SpaceBehavior>ASTEROID_FIELD_DAMAGE NEBULA SPACE_OBSTACLE</SpaceBehavior>");
            xml.insert(begin + std::string_view{"<SpaceUnit Name=\"Test_Frigate\">"}.size(), tags);
            write_fixture(tree.root, xml);
            const auto loaded = load(tree.root, models(-40.0F));
            if (loaded.tables) {
                const auto* unit = loaded.tables->find("Test_Frigate");
                expect(unit && unit->footprint.space_obstacle, "WHZ-01: SpaceBehavior opts into tracking");
                if (unit) {
                    const auto& hazard = unit->footprint.hazard;
                    expect(hazard.asteroid_field && hazard.ion_storm && hazard.nebula && hazard.impassable_asteroid,
                        "WHZ-01: overlapping hazard flags remain independent");
                    expect(hazard.asteroid_damage && hazard.nebula_service,
                        "WHZ-01: affected-unit service attachment is authored");
                    expect(hazard.obstacle_offset == point(raw(12), raw(-7), raw(3)), "WHZ-01: raw offset retains all axes");
                    auto changed = *loaded.tables;
                    changed.units[static_cast<std::size_t>(unit - loaded.tables->units.data())]
                        .footprint.hazard.ion_storm = false;
                    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                        "WHZ-01: independent semantic flags bind content identity");
                }
            }
        }
    }
    barrage_inputs();
    manual_weapon_loading();
    special_weapon_classification();
    rocket_variant_inputs();
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    const auto loaded = load(tree.root, models(-40.0F));
    if (!loaded.tables) return;
    const auto& tables = *loaded.tables;

    expect(tables.units.size() == 5, "pinned types then the squadron craft");
    if (tables.units.size() != 5) return;
    expect(tables.units[4].id == "Test_Fighter" && tables.units[4].kind == UnitKind::craft, "craft follow the pinned types");

    const auto& station = tables.units[0];
    expect(station.kind == UnitKind::station && station.xml_type == "StarBase", "StarBase is a station");
    expect(station.station_community_property && !station.community_property && !station.capture_point,
        "WSU-16: an authored community station is shared without capture-point behaviour");
    auto capture_tables = tables;
    capture_tables.units[1].capture_point = true;
    auto selection_only = capture_tables;
    selection_only.units[0].station_community_property = false;
    expect(eawr::units::content_identity(selection_only) == eawr::units::content_identity(capture_tables),
        "WSU-16: station selection metadata leaves simulation identity unchanged");
    selection_only.units[1].community_property = true;
    expect(eawr::units::content_identity(selection_only) != eawr::units::content_identity(capture_tables),
        "WBP-01: capture-point community admission still binds simulation identity");
    expect(!station.mouse_sensitive && !station.bar_admitted && tables.units[1].mouse_sensitive
        && tables.units[1].bar_admitted,
        "WSU-13/50: mouse and bar admission follow behaviour/data, independent of positive hull");
    expect(!station.locomotion && tables.units[1].selectable && tables.units[1].locomotion,
           "WSU-21: selection eligibility follows behaviours, not the object category or speed");
    expect(tables.units[2].selectable && tables.units[2].locomotion,
           "WSU-19: squadron selection eligibility comes from its resolved team container");
    expect(station.variant_chain == std::vector<std::string>{"Skirmish_Test_Base", "Test_Base"}, "variant chain");
    expect(station.hull && station.hull->raw() == raw(1600), "station hull through the variant");
    expect(station.victory_relevant, "station is victory relevant");
    expect(station.space_fow_reveal_range && station.space_fow_reveal_range->raw() == raw(2000),
           "sensor range through the variant");
    expect(tables.units[1].space_fow_reveal_range && tables.units[1].space_fow_reveal_range->raw() == raw(1200)
               && tables.units[4].space_fow_reveal_range && tables.units[4].space_fow_reveal_range->raw() == raw(500)
               && !tables.units[2].space_fow_reveal_range,
           "ship and craft authored ranges; a squadron authors none");
    // #271: REVEAL decides; a squadron reveals through its team container, `Team` by
    // default, and a container variant keeps the last of its base's two authored ranges.
    expect(station.reveal && tables.units[1].reveal && !tables.units[4].reveal, "REVEAL on the station and ship only");
    expect(eawr::units::sensor_range(station) == std::optional(Fixed::from_raw(raw(2000)))
               && eawr::units::sensor_range(tables.units[1]) == std::optional(Fixed::from_raw(raw(1200)))
               && !eawr::units::sensor_range(tables.units[4]),
           "sensor ranges: REVEAL types only; the craft has none");
    expect(tables.units[2].team_type == "Team" && eawr::units::sensor_range(tables.units[2]) == std::optional(Fixed::from_raw(raw(800))),
           "a squadron without Create_Team_Type reveals through Team at 800");
    expect(tables.units[3].team_type == "Test_Pair_Container"
               && eawr::units::sensor_range(tables.units[3]) == std::optional(Fixed::from_raw(raw(1000))),
           "a named container reveals with its own (inherited, last) range");
    expect(station.inactive_abilities == std::vector<std::string>{"Income_Stream_Ability Test_Income"},
           "station ability sub-objects are listed as inactive");
    // #530 PU-02: the income stream through the variant.
    expect(station.production.income.size() == 1 && station.production.income[0].name == "Test_Income"
               && station.production.income[0].base_value == Fixed::from_raw(raw(30))
               && station.production.income[0].interval_seconds == Fixed::from_raw(raw(10)),
           "the station's income stream");
    expect(station.spawner.has_value(), "SPAWN_SQUADRON station has a spawner");
    if (station.spawner) {
        const auto& spawner = *station.spawner;
        expect(spawner.starting.size() == 1 && spawner.starting[0].squadron == "Test_Squadron" &&
                   spawner.starting[0].count == 2 && spawner.starting[0].squadron_index == 2,
               "starting list resolves to the squadron table row");
        expect(spawner.delay_seconds && spawner.delay_seconds->raw() == raw(10), "spawn delay");
        expect(spawner.reserves.size() == 1 && spawner.reserves[0].count == -1 && !spawner.reserves_used,
               "reserves are read but flagged unused (SK-36)");
    }
    expect(station.hardpoints.size() == 1 && station.hardpoints[0].attachment.position == point(0, 0, raw(-150)),
           "station bay position from its bone");

    const auto& frigate = tables.units[1];
    expect(frigate.kind == UnitKind::ship && frigate.model_path == "data/art/models/test_frigate.alo", "frigate model path");
    expect(frigate.scale_factor && frigate.scale_factor->raw() == Fixed::from_decimal("0.70").value().raw(), "scale factor");
    expect(frigate.movement.acceleration && frigate.movement.acceleration->raw() == Fixed::from_decimal(".04").value().raw(),
           "acceleration");
    expect(frigate.movement.space_layer == "Frigate", "space layer is trimmed");
    expect(frigate.targeting_max_attack_distance && frigate.targeting_max_attack_distance->raw() == raw(1000),
           "the last of a duplicated single-value tag wins");
    expect(has_row(tables.notes, "Test_Frigate", "targeting_max_attack_distance"), "the duplicate is noted");
    expect(frigate.category_mask == std::vector<std::string>{"Frigate", "AntiCorvette"}, "category mask splits on |");
    expect(frigate.targeting_priority_set_index == 0 && tables.priority_sets.size() == 2,
           "only referenced priority sets load, in first-reference order");
    expect(frigate.category_bits == 0x210 && frigate.property_flags.empty() && frigate.property_bits == 0,
           "category bits from the enum file; no property flags");
    expect(tables.units[4].property_flags == std::vector<std::string>{"SmallShip"} && tables.units[4].property_bits == 1,
           "Property_Flags names and bits");
    expect(tables.categories.size() == 9 && tables.categories[5].name == "Capital" && tables.categories[5].value == 32 &&
               tables.categories[8].value == ~std::uint64_t{0} &&
               tables.properties.size() == 2,
           "dynamic enums load in file order, hexadecimal or decimal, 64 bits");
    if (tables.priority_sets.size() == 2) {
        using eawr::units::PriorityMatch;
        const auto& set = tables.priority_sets[0];
        const auto& entries = set.attack_priorities;
        expect(entries.size() == 5 && entries[0].match == PriorityMatch::category && entries[0].bits == 4 &&
                   entries[3].name == "test_fighter" && entries[3].match == PriorityMatch::type && entries[3].bits == 0 &&
                   entries[4].match == PriorityMatch::property && entries[4].bits == 1,
               "entries are categories, then properties, then exact types");
        expect(set.category_exclusions == std::vector<std::string>{"Capital", "Frigate", "AntiBomber"} &&
                   set.category_exclusion_bits == 0x130 && set.property_exclusion_bits == 2,
               "exclusion masks split on commas and |");
        expect(set.hard_point_priorities == std::vector<std::string>{"Shield_Generator", "WEAPON_LASER"} &&
                   set.hard_point_exclusions == std::vector<std::string>{"Fighter_Bay"},
               "hardpoint names are checked; the unknown one is dropped");
    }
    expect(frigate.lua_script == "ObjectScript_Test", "Lua_Script reference");
    expect(frigate.hardpoints.size() == 3, "every listed hardpoint has a row");
    if (frigate.hardpoints.size() == 3) {
        const auto& laser = frigate.hardpoints[0];
        expect(laser.type == eawr::units::HardpointType::weapon_laser && laser.targetable && laser.destroyable,
               "laser hardpoint flags");
        expect(laser.health && laser.health->raw() == raw(260), "hardpoint health");
        expect(laser.attachment.position == point(raw(10), raw(20), raw(30)), "attachment bone position");
        expect(laser.fire_a.position == point(raw(10), raw(22), raw(30)) && laser.fire_a.from_attached_model,
               "fire bone from the attached model, placed through the rotated attachment frame");
        expect(!laser.fire_b.position, "missing fire bone stays unresolved");
        expect(laser.weapon.has_value(), "weapon hardpoint has a weapon");
        if (laser.weapon) {
            const auto& weapon = *laser.weapon;
            expect(weapon.projectile == "Proj_Test_Turbo" && weapon.projectile_index != eawr::units::no_index &&
                       tables.projectiles[weapon.projectile_index].id == "Proj_Test_Turbo",
                   "weapon projectile index");
            expect(weapon.pulse_count == 5U && weapon.range && weapon.range->raw() == raw(1100), "pulse count and range");
            expect(weapon.inaccuracy.size() == 2 && weapon.inaccuracy[0].category == "Fighter" &&
                       weapon.inaccuracy[0].distance.raw() == raw(70),
                   "every inaccuracy row is kept");
            expect(weapon.damage_type == "Damage_Test" && weapon.opportunity_fire_when_targeting, "weapon damage type and flags");
            expect(weapon.damage && weapon.damage->raw() == raw(40), "a hardpoint's own damage (DG-25)");
        }
        expect(frigate.hardpoints[1].attachment.position == point(raw(-40), 0, raw(5) / 2),
               "bone names match without case");
        expect(frigate.hardpoints[2].id == "HP_Test_Missing" && frigate.hardpoints[2].type_name.empty(),
               "an unknown hardpoint keeps its row");
    }
    expect(frigate.target_bones.size() == 1 && frigate.target_bones[0].position == point(raw(-39), raw(2), raw(11) / 2),
           "target bone composes its parent chain");
    expect(frigate.abilities.size() == 1, "one unit ability");
    if (frigate.abilities.size() == 1) {
        const auto& ability = frigate.abilities[0];
        expect(ability.type == "DEFEND" && ability.authored_type == "defend", "ability type is upper-cased");
        expect(ability.modifiers.size() == 2 && ability.modifiers[0].value.raw() == Fixed::from_decimal("0.8").value().raw() &&
                   ability.modifiers[1].value.raw() == raw(-3),
               "C float suffixes are accepted on modifiers");
        expect(ability.supports_autofire && ability.recharge_seconds && ability.expiration_seconds, "ability timing");
    }

    const auto& squadron = tables.units[2];
    expect(squadron.kind == UnitKind::squadron && squadron.members.size() == 3, "every Squadron_Units line contributes");
    if (squadron.members.size() == 3) {
        expect(squadron.members[2].craft_index == 4 && squadron.members[2].offset == point(0, raw(-15), raw(1) / 2),
               "member offsets by order");
    }
    expect(squadron.abilities.size() == 1 && squadron.abilities[0].type == "HUNT", "squadron ability");
    const auto& pair = tables.units[3];
    expect(pair.members.size() == 2 && pair.members[1].offset == point(raw(-10), 0, 0) &&
               pair.abilities.size() == 1,
           "a list tag comes whole from the most-derived layer that authors it");
    expect(has_row(tables.notes, "Test_Squadron_Pair", "Squadron_Units", "Test_Squadron") &&
               has_row(tables.notes, "Test_Squadron_Pair", "Squadron_Offsets", "Test_Squadron"),
           "a base layer that also authors the list is noted");

    const auto& fighter = tables.units[4];
    expect(fighter.weapon.has_value(), "craft object weapon");
    if (fighter.weapon) {
        const auto index = fighter.weapon->projectile_index;
        expect(index != eawr::units::no_index && tables.projectiles[index].damage &&
                   tables.projectiles[index].damage->raw() == raw(5) && tables.projectiles[index].max_speed &&
                   tables.projectiles[index].max_speed->raw() == raw(11),
               "projectile values through its variant");
        if (index < tables.projectiles.size()) {
            const auto& blast = tables.projectiles[index].blast;
            expect(blast.damage.raw() == raw(12) && blast.radius.raw() == raw(80) && blast.dropoff
                && blast.tiers == 3 && blast.max_victims == 0 && blast.max_delay.raw() == 0,
                "WAD-01: blast tags inherit before a derived damage override; stock zero delay retained");
        }
        expect(fighter.weapon->min_recharge_seconds == fighter.weapon->max_recharge_seconds, "object weapon recharge");
        expect(fighter.weapon->inaccuracy.empty(),
               "the object weapon scatters by Targeting_Fire_Inaccuracy, not the unit's Fire_Inaccuracy_Distance (DG-24)");
    }

    expect(tables.constants.scalars.size() == 47, "every required combat scalar has a row");
    for (const auto& constant : tables.constants.scalars) {
        // Diminishing_Firepower is a list: it keeps its text (#74).
        expect(constant.value.has_value() || constant.tag == "Diminishing_Firepower", constant.tag);
    }
    expect(tables.constants.damage_to_armor.size() == 3, "only fleet damage/armor rows load");
    expect(has_row(tables.notes, "GameConstants", "Damage_To_Armor_Mod", "damage_fighter, armor_fighter"),
           "missing fleet pairs are noted");

    expect(tables.unresolved.size() == 3 && has_row(tables.unresolved, "Test_Frigate", "hardpoint", "HP_Test_Missing") &&
               has_row(tables.unresolved, "HP_Test_Laser", "Fire_Bone_B", "FP_Missing") &&
               has_row(tables.unresolved, "Frigate", "Hard_Point_Exclusions", "Engine_Typo"),
           "the scan lists exactly the missing hardpoint and bone and the unknown hardpoint name");
    if (tables.unresolved.size() != 3) print_rows("unresolved", tables.unresolved);
    expect(!tables.inputs.empty() && tables.inputs.front().logical_path < tables.inputs.back().logical_path,
           "input files are listed in path order");

    // #72 durability: hull and hardpoint health x Object_Max_Health_Multiplier_Space (1.5).
    expect(station.hardpoints.size() == 1 && station.hardpoints[0].repair_amount_per_frame &&
               station.hardpoints[0].repair_amount_per_frame->raw() == Fixed::from_decimal(".50").value().raw() &&
               station.hardpoints[0].repair_cost_per_frame &&
               station.hardpoints[0].repair_cost_per_frame->raw() == Fixed::from_decimal("1.5").value().raw(),
           "station hardpoint repair values");
    expect(frigate.hardpoints.size() == 3 && !frigate.hardpoints[0].repair_amount_per_frame &&
               frigate.destroyed_with_hardpoints && !station.destroyed_with_hardpoints,
           "the absent hardpoint-death tag defaults to yes; an authored No overrides it");
    const auto durability = eawr::units::durability_table(tables);
    expect(static_cast<bool>(durability), "durability table builds");
    if (!durability) return;
    const auto& table = durability.value();
    expect(table.rules.hull_vs_hardpoints.raw() == Fixed::from_decimal("0.2").value().raw() &&
               table.rules.engines_disabled_speed.raw() == Fixed::from_decimal("0.4").value().raw() &&
               table.rules.damaged_fraction.raw() == Fixed::from_decimal("0.33").value().raw(),
           "durability rules from gameconstants.xml");
    expect(table.profiles.size() == 3, "station, ship and craft have a hull; squadrons do not");
    if (table.damage) {
        // DG-12: an authored row, a negative row that reads as 1, and an unauthored pair at 1.
        const auto types = eawr::units::damage_type_index(tables);
        const auto multiplier = [&](const std::string_view damage, const std::string_view armor) {
            return eawr::sim::tactical::armor_multiplier(*table.damage, types.damage(damage), types.armor(armor)).raw();
        };
        expect(multiplier("Damage_Turbolaser", "Armor_Test") == Fixed::from_decimal("0.5").value().raw(),
               "an authored armor row keeps its multiplier");
        expect(multiplier("Damage_Turbolaser", "Armor_Fighter") == Fixed::scale, "a negative armor row reads as 1");
        expect(multiplier("Damage_Fighter", "Armor_Fighter") == Fixed::scale, "an unauthored armor pair multiplies by 1");
    } else {
        expect(false, "the fixture has damage rules");
    }
    const auto* frigate_profile = table.find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(frigate_profile != nullptr, "profiles are keyed by the object-type CRC");
    const auto* authored_no_profile = table.find(eawr::assets::object_type_crc("Skirmish_Test_Base"));
    if (authored_no_profile != nullptr) {
        auto health = eawr::sim::tactical::full_durability(*authored_no_profile);
        const auto hit = eawr::sim::tactical::apply_damage(*authored_no_profile, health, 0, Fixed::from_raw(raw(1500)));
        expect(!hit.unit_destroyed && health.hull.raw() > 0,
               "an authored No keeps the hull alive after the last hardpoint is destroyed");
    }
    if (frigate_profile != nullptr) {
        auto health = eawr::sim::tactical::full_durability(*frigate_profile);
        const auto first = eawr::sim::tactical::apply_damage(*frigate_profile, health, 0, Fixed::from_raw(raw(390)));
        const auto last = eawr::sim::tactical::apply_damage(*frigate_profile, health, 1, Fixed::from_raw(raw(255)));
        expect(!first.unit_destroyed && last.unit_destroyed && health.hull == Fixed{},
               "a ship without the tag dies when its last destroyable hardpoint is destroyed");
    }
    if (frigate_profile != nullptr) {
        using eawr::sim::tactical::HardpointRole;
        const auto& hp = frigate_profile->hardpoints;
        expect(frigate_profile->max_hull.raw() == raw(5400) && frigate_profile->max_speed &&
                   frigate_profile->max_speed->raw() == Fixed::from_decimal("2.2").value().raw(),
               "hull x 1.5 and the authored speed");
        expect(hp.size() == 3 && hp[0].role == HardpointRole::weapon && hp[0].destroyable && hp[0].max_health.raw() == raw(390) &&
                   hp[1].role == HardpointRole::engine && hp[1].max_health.raw() == raw(255) &&
                   hp[2].role == HardpointRole::other && !hp[2].destroyable && hp[2].max_health.raw() == 0,
               "hardpoint roles and health x 1.5");
        // DG-05 (#440): no unit XML tag turns this off; every M2 unit keeps FoC's default.
        expect(frigate_profile->allow_diminishing_firepower, "the diminishing-firepower flag defaults on");
    }
    const auto* station_profile = table.find(eawr::assets::object_type_crc("Skirmish_Test_Base"));
    expect(station_profile != nullptr && station_profile->max_hull.raw() == raw(2400) && !station_profile->max_speed &&
               station_profile->hardpoints.size() == 1 &&
               station_profile->hardpoints[0].role == eawr::sim::tactical::HardpointRole::fighter_bay &&
               station_profile->hardpoints[0].repair_cost_per_frame.raw() == Fixed::from_decimal("1.5").value().raw(),
           "station profile with its fighter bay and repair values");

    // #70 motion: speeds, accelerations and the rate of turn x Object_Max_Speed_Multiplier_Space (1.2).
    const auto motion = eawr::units::motion_table(tables);
    expect(static_cast<bool>(motion), "motion table builds");
    if (!motion) return;
    const auto times = [](const char* value) {
        return eawr::sim::math::multiply(Fixed::from_decimal(value).value(), Fixed::from_decimal("1.2").value()).value();
    };
    expect(motion.value().rules.arc_degrees == Fixed::from_raw(raw(15)) &&
               motion.value().rules.expansion_distance == Fixed::from_raw(raw(300)),
           "motion rules: 360 / MaxRotationsSpace and XYExpansionDistanceSpace");
    expect(motion.value().profiles.size() == 1, "only the ship moves by motion profile; craft and stations do not");
    const auto* moving = motion.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(moving != nullptr && moving->max_speed == times("2.2") && moving->acceleration == times(".04") &&
               moving->deceleration == moving->max_speed && moving->rate_of_turn == times("0.7") &&
               moving->turn_in_place_slowdown == Fixed::from_raw(raw(3)),
           "frigate: values x 1.2, deceleration defaults to the maximum speed, the frigate slowdown");
    // #351 BK-01: without Max_Rate_Of_Roll and Bank_Turn_Angle the engine defaults apply.
    expect(moving != nullptr && moving->roll_rate == times("2") && moving->bank_angle == Fixed::from_raw(raw(70)),
           "frigate without roll tags: roll rate 2 x 1.2, bank angle 70");
    for (const auto& [name, layer, slowdown] : {
            std::tuple{"corvette", eawr::sim::tactical::SpaceLayer::corvette, 2},
            std::tuple{"fRiGaTe", eawr::sim::tactical::SpaceLayer::frigate, 3},
            std::tuple{" CAPITAL ", eawr::sim::tactical::SpaceLayer::capital, 4},
            std::tuple{"supercapital", eawr::sim::tactical::SpaceLayer::super_capital, 4}}) {
        auto mixed = tables;
        for (auto& unit : mixed.units) if (unit.id == "Test_Frigate") unit.movement.space_layer = name;
        const auto rebound = eawr::units::motion_table(mixed);
        const auto id = eawr::assets::object_type_crc("Test_Frigate");
        const auto* profile = rebound ? rebound.value().find(id) : nullptr;
        const auto* shape = rebound ? rebound.value().footprint(id) : nullptr;
        expect(profile && shape && profile->turn_in_place_slowdown == Fixed::from_raw(raw(slowdown))
            && shape->layer == layer, "layer spelling uses the same slowdown and footprint layer");
    }
    auto delayed = motion.value();
    delayed.rules.reevaluation_frames = 10;
    delayed.avoidance->search_delay = 10;
    expect(!eawr::sim::tactical::validate_motion(delayed), "PC-08: equal landing and reevaluation frames rejected");
    delayed.avoidance->search_delay = 11;
    expect(!eawr::sim::tactical::validate_motion(delayed), "PC-08: late landing rejected");
    delayed.avoidance->search_delay = 9;
    expect(static_cast<bool>(eawr::sim::tactical::validate_motion(delayed)), "PC-08: landing before reevaluation accepted");
    // #71: the path finder's constants and a footprint per type with a Space_Layer.
    const auto& avoidance = motion.value().avoidance;
    expect(avoidance && avoidance->max_expansions == 3500 && avoidance->tries == 6 && avoidance->tracking_interval == 90
               && avoidance->tracking_windows == 45 && avoidance->wait_frames == Fixed::from_raw(raw(100))
               && avoidance->destination_search_increment == Fixed::from_raw(raw(50)),
           "avoidance rules from gameconstants.xml");
    const auto* footprint = motion.value().footprint(eawr::assets::object_type_crc("Test_Frigate"));
    expect(footprint != nullptr && footprint->layer == eawr::sim::tactical::SpaceLayer::frigate && !footprint->obstacle
               && footprint->radius == eawr::sim::math::multiply(Fixed::from_decimal("40").value(), Fixed::from_decimal("0.70").value()).value(),
           "the frigate is tracked in the frigate layer, soft radius 40 x 0.7");
}

// #270 ranking rules (space-targeting R-09) on invented sets; the FoC sets are checked in foc_fleet.
void priority_rules() {
    using eawr::units::attack_priority;
    using eawr::units::hard_point_priority;
    using eawr::units::HardpointType;
    using eawr::units::PriorityMatch;
    using eawr::units::unlisted_priority;
    const auto weight = [](const char* text) { return Fixed::from_decimal(text).value(); };
    const auto candidate = [](const char* id, const std::uint32_t categories, const std::uint32_t properties) {
        eawr::units::UnitType unit;
        unit.id = id;
        unit.category_bits = categories;
        unit.property_bits = properties;
        return unit;
    };
    // Categories: 1 Fighter, 2 Bomber, 4 Corvette, 8 Capital. Properties: 1 SmallShip, 2 NotOpportunityTarget.
    eawr::units::TargetingPrioritySet set;
    set.attack_priorities = {{"Fighter", weight("3.0"), PriorityMatch::category, 1},
                             {"Bomber", weight("2.0"), PriorityMatch::category, 2},
                             {"Hero_Ship", weight("0.5"), PriorityMatch::type, 0},
                             {"Ace_Fighter", weight("6.0"), PriorityMatch::type, 0},
                             {"SmallShip", weight("5.0"), PriorityMatch::property, 1}};
    set.unit_exclusions = {"Asteroid", "Hero_Ship"};
    set.category_exclusion_bits = 8;
    set.property_exclusion_bits = 2;

    const auto score = [&](const eawr::units::UnitType& unit) { return attack_priority(set, unit); };
    expect(score(candidate("X", 1, 0)) == weight("3.0"), "a category entry gives its weight");
    expect(score(candidate("XY", 3, 0)) == weight("2.0"), "several matching categories take the smallest weight");
    expect(score(candidate("Corvette", 4, 0)) == unlisted_priority, "an unlisted category still has a priority");
    expect(unlisted_priority > weight("1000000"), "unlisted ranks after every listed weight");
    expect(score(candidate("hero_ship", 8, 2)) == weight("0.5"),
           "an exact type entry wins over its categories and every exclusion");
    expect(score(candidate("Ace_Fighter", 1, 0)) == weight("6.0"), "an exact type entry wins over a smaller category weight");
    expect(!score(candidate("asteroid", 0, 0)), "a type exclusion gives no priority");
    expect(!score(candidate("Capital", 8, 0)), "a category exclusion gives no priority");
    expect(!score(candidate("Tagged_Fighter", 1, 2)), "a property exclusion gives no priority");
    expect(score(candidate("Small_Capital", 8, 1)) == weight("5.0"), "a listed property overrides a category exclusion");
    expect(score(candidate("Small_Fighter", 1, 1)) == weight("5.0"),
           "the first matching property entry replaces a smaller category score (FoC order rule)");
    set.attack_priorities.push_back({"Fighter", weight("1.0"), PriorityMatch::category, 1});
    expect(score(candidate("Small_Fighter", 1, 1)) == weight("1.0"), "a later category entry takes the minimum again");

    const eawr::units::TargetingPrioritySet empty;
    expect(attack_priority(empty, candidate("X", 1, 0)) == unlisted_priority, "an empty set ranks everything unlisted");

    set.hard_point_priorities = {"Shield_Generator", "engine", "Fighter_Bay"};
    set.hard_point_exclusions = {"Fighter_Bay", "Weapon_Laser"};
    expect(hard_point_priority(set, HardpointType::shield_generator) == weight("1.0") &&
               hard_point_priority(set, HardpointType::engine) == weight("2.0"),
           "hardpoint priority is the 1-based list position, without case");
    expect(hard_point_priority(set, HardpointType::fighter_bay) == weight("3.0"), "a listed hardpoint ignores its exclusion");
    expect(!hard_point_priority(set, HardpointType::weapon_laser), "an excluded hardpoint has no priority");
    expect(hard_point_priority(set, HardpointType::tractor_beam) == unlisted_priority, "an unlisted hardpoint ranks last");
}

void content_identity() {
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    const auto first = load(tree.root, models(-40.0F));
    const auto second = load(tree.root, models(-40.0F));
    if (!first.tables || !second.tables) return;
    const auto identity = eawr::units::content_identity(*first.tables);
    expect(identity == eawr::units::content_identity(*second.tables), "identity is stable across loads");

    const auto moved = load(tree.root, models(-41.0F));
    if (moved.tables) expect(identity != eawr::units::content_identity(*moved.tables), "a moved bone changes the identity");

    write_fixture(tree.root, units_xml("3600", "Unit_Select_Other"));
    const auto unread = load(tree.root, models(-40.0F));
    if (unread.tables) expect(identity == eawr::units::content_identity(*unread.tables), "unread tags do not change it");

    write_fixture(tree.root, units_xml("3601", "Unit_Select_Test"));
    const auto changed = load(tree.root, models(-40.0F));
    if (changed.tables) expect(identity != eawr::units::content_identity(*changed.tables), "a changed value changes it");

    eawr::sim::Replay replay;
    replay.tick_numerator = 1;
    replay.tick_denominator = 30;
    replay.content_identity = identity;
    auto bytes = eawr::sim::write_replay(replay);
    expect(static_cast<bool>(bytes), "replay with the unit identity writes");
    if (!bytes) return;
    auto parsed = eawr::sim::parse_replay(bytes.value());
    expect(parsed && parsed.value().content_identity == identity, "the replay header carries the unit identity");

    auto modifier_xml = units_xml("3600", "Unit_Select_Test");
    const std::string fighter_tag = "<SpaceUnit Name=\"Test_Fighter\">";
    const auto fighter_offset = modifier_xml.find(fighter_tag);
    expect(fighter_offset != std::string::npos, "income modifier fixture finds a loaded space type");
    if (fighter_offset == std::string::npos) return;
    const std::string modifier_tags = R"(
      <Previous_Upgrade_Level_Type>Test_Base</Previous_Upgrade_Level_Type>
      <Next_Upgrade_Level_Type> Test_Next_Upgrade </Next_Upgrade_Level_Type>
      <Abilities><Income_Stream_Mod_Ability Name="Fixture_Income_Mod">
        <Target_Stream_Source>Skirmish_Test_Base</Target_Stream_Source>
        <Activation_Style>Space_Automatic</Activation_Style>
        <Affects_All_Allied_Sources>Yes</Affects_All_Allied_Sources>
        <Stacking_Category>7</Stacking_Category><Income_Multiplier>1.2</Income_Multiplier>
        <Income_Additive_Value>3</Income_Additive_Value><Interval_Multiplier>0.5</Interval_Multiplier>
        <Reverse_Application_Logic>Yes</Reverse_Application_Logic>
      </Income_Stream_Mod_Ability></Abilities>)";
    modifier_xml.insert(fighter_offset + fighter_tag.size(), modifier_tags);
    write_fixture(tree.root, modifier_xml);
    const auto modifier_loaded = load(tree.root, models(-40.0F));
    expect(modifier_loaded.tables.has_value(), "WBP-25 modifier fields parse through validated unit loading");
    if (!modifier_loaded.tables) return;
    const auto* fighter = modifier_loaded.tables->find("Test_Fighter");
    expect(fighter && fighter->production.income_bonuses.size() == 1, "one authored modifier loads");
    if (!fighter || fighter->production.income_bonuses.size() != 1) return;
    const auto& bonus = fighter->production.income_bonuses.front();
    expect(bonus.name == "Fixture_Income_Mod" && bonus.target_source == "Skirmish_Test_Base"
        && bonus.activation_style == "Space_Automatic" && bonus.all_allied_sources && bonus.reverse
        && bonus.stacking_category == 7 && bonus.multiplier == Fixed::from_decimal("1.2").value()
        && bonus.additive == Fixed::from_integer(3).value()
        && bonus.interval_multiplier == Fixed::from_decimal("0.5").value(), "WBP-25 all modifier fields retain their authored values");
    expect(fighter->production.removes_previous == "Test_Base", "WBP-24 previous upgrade destruction defaults to true");
    expect(fighter->production.next_upgrade == "Test_Next_Upgrade", "WPR-63: authored menu successor is trimmed");
    auto menu_only = *modifier_loaded.tables;
    menu_only.units[static_cast<std::size_t>(fighter - modifier_loaded.tables->units.data())].production.next_upgrade.clear();
    expect(eawr::units::content_identity(menu_only) == eawr::units::content_identity(*modifier_loaded.tables),
        "WPR-63: menu successor metadata is outside replay content identity");
    const auto modifier_identity = eawr::units::content_identity(*modifier_loaded.tables);
    const auto fighter_index = static_cast<std::size_t>(fighter - modifier_loaded.tables->units.data());
    for (int field = 0; field < 5; ++field) {
        auto variant = *modifier_loaded.tables;
        auto& changed_bonus = variant.units[fighter_index].production.income_bonuses.front();
        switch (field) {
        case 0: changed_bonus.interval_multiplier = Fixed::from_integer(2).value(); break;
        case 1: changed_bonus.activation_style = "Ground_Automatic"; break;
        case 2: changed_bonus.stacking_category = 8; break;
        case 3: changed_bonus.all_allied_sources = false; break;
        default: changed_bonus.reverse = false; break;
        }
        expect(eawr::units::content_identity(variant) != modifier_identity, "each new runtime modifier field binds content identity");
    }
    modifier_xml.insert(fighter_offset + fighter_tag.size(), "<Destroy_Previous_Upgrade_Level>No</Destroy_Previous_Upgrade_Level>");
    write_fixture(tree.root, modifier_xml);
    const auto retained = load(tree.root, models(-40.0F));
    expect(retained.tables && retained.tables->find("Test_Fighter")->production.removes_previous.empty(),
        "WBP-24 explicit false preserves the previous upgrade level");
    auto command_xml = units_xml("3600", "Unit_Select_Test");
    command_xml.insert(command_xml.find(fighter_tag) + fighter_tag.size(), R"(
      <Unique_Space_Unit>Test_Frigate</Unique_Space_Unit>
      <Unique_Ground_Unit>Test_Fighter</Unique_Ground_Unit>
      <Abilities><Combat_Bonus_Ability Name="Fixture_Command">
        <Activation_Style>Space_Automatic</Activation_Style>
        <Specific_Faction>Empire</Specific_Faction><Stacking_Category>3</Stacking_Category>
        <Applicable_Unit_Types>Test_Frigate</Applicable_Unit_Types>
        <Excluded_Unit_Types>Test_Fighter</Excluded_Unit_Types>
        <Health_Bonus_Percentage>-0.25</Health_Bonus_Percentage>
      </Combat_Bonus_Ability></Abilities>)");
    write_fixture(tree.root, command_xml);
    const auto command = load(tree.root, models(-40.0F));
    expect(command.tables.has_value(), "WHE-53 command source fields load");
    if (!command.tables) return;
    const auto* source = command.tables->find("Test_Fighter");
    expect(source && source->production.combat_bonuses.size() == 1, "one passive command bonus loads");
    if (!source || source->production.combat_bonuses.size() != 1) return;
    const auto& passive = source->production.combat_bonuses.front();
    expect(passive.enabled && passive.specific_faction == "Empire" && passive.stacking_category == 3
        && passive.percentages[0] == Fixed::from_decimal("-0.25").value()
        && passive.excluded_containers == std::vector<eawr::sim::tactical::TypeId>{eawr::assets::object_type_crc("Test_Frigate"),
            eawr::assets::object_type_crc("Test_Fighter")}
        && passive.filter.excluded_types == std::vector<eawr::sim::tactical::TypeId>{eawr::assets::object_type_crc("Test_Fighter")},
        "WHE-13/14/55 faction, default enablement, container and signed percentage retain authored values");
    const auto command_identity = eawr::units::content_identity(*command.tables);
    const auto source_index = static_cast<std::size_t>(source - command.tables->units.data());
    for (unsigned field = 0; field < 4; ++field) {
        auto variant = *command.tables;
        auto& value = variant.units[source_index].production.combat_bonuses.front();
        if (field == 0) value.specific_faction = "Rebel";
        else if (field == 1) value.enabled = false;
        else if (field == 2) value.excluded_containers.clear();
        else value.filter.excluded_types.clear();
        expect(eawr::units::content_identity(variant) != command_identity, "command eligibility binds content identity");
    }
    eawr::skirmish::SkirmishStart command_start;
    command_start.setup.players = {{1, 0, 1, 1}};
    const auto compiled = eawr::skirmish::economy_rules(command_start, {}, *command.tables);
    expect(compiled && compiled.value().command_bonuses.size() == 1
        && compiled.value().command_bonuses.front().bonus.applicable.empty()
        && compiled.value().command_bonuses.front().specific_faction == eawr::assets::object_type_crc("Empire"),
        "WHE-13: unique container exclusion wins even over an explicitly allowed type");
    auto allowed_tables = *command.tables;
    auto& allowed = allowed_tables.units[source_index].production.combat_bonuses.front();
    allowed.excluded_containers.clear();
    allowed.filter.excluded_types.push_back(eawr::assets::object_type_crc("Test_Frigate"));
    std::sort(allowed.filter.excluded_types.begin(), allowed.filter.excluded_types.end());
    const auto exact = eawr::skirmish::economy_rules(command_start, {}, allowed_tables);
    expect(exact && exact.value().command_bonuses.front().bonus.applicable
        == std::vector<eawr::sim::tactical::TypeId>{eawr::assets::object_type_crc("Test_Frigate")},
        "WHE-14: exact allowed type bypasses ordinary exclusions");
    allowed.filter.applicable_types.clear();
    allowed.filter.applicable_categories = allowed_tables.find("Test_Frigate")->category_bits;
    const auto excluded = eawr::skirmish::economy_rules(command_start, {}, allowed_tables);
    expect(excluded && !std::binary_search(excluded.value().command_bonuses.front().bonus.applicable.begin(),
        excluded.value().command_bonuses.front().bonus.applicable.end(), eawr::assets::object_type_crc("Test_Frigate")),
        "WHE-14: category match retains exact type exclusions");
    allowed.enabled = false;
    const auto disabled = eawr::skirmish::economy_rules(command_start, {}, allowed_tables);
    expect(disabled && disabled.value().command_bonuses.empty(), "WHE-11: disabled passive source never registers");
}

void bind_frame_errors() {
    auto cycle = model("data/art/models/cycle.alo", {bone("A", 1, {0, 0, 0}), bone("B", 0, {0, 0, 0})});
    expect(!eawr::units::bind_frames(cycle), "a bone cycle fails");
    auto huge = model("data/art/models/huge.alo", {bone("A", -1, {std::numeric_limits<float>::infinity(), 0, 0})});
    expect(!eawr::units::bind_frames(huge), "a non-finite bone fails");
    auto chain = model("data/art/models/chain.alo", {bone("A", -1, {1, 0, 0}, true), bone("B", 0, {1, 0, 0}, true)});
    const auto frames = eawr::units::bind_frames(chain);
    expect(frames && frames.value().size() == 2 && frames.value()[1].rows[0][3].raw() == raw(1) &&
               frames.value()[1].rows[1][3].raw() == raw(1) && frames.value()[1].rows[0][0].raw() == -Fixed::scale,
           "frames compose root to bone");
}

// #71 collision footprint: each axis of the union box starts from its own bounds, so an X-wide
// mesh keeps its narrow Y (PR #343 review).

} // namespace unit_tables_test_support
