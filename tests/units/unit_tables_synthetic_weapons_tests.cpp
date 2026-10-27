#include "unit_tables_support.hpp"
#include "unit_tables_synthetic_fixtures.hpp"
#include "eawr/skirmish/start.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace unit_tables_test_support {
void randomized_fire_bone_loading() {
    std::optional<eawr::units::UnitTables> defaults;
    for (const std::string value : {"", "Yes", "No", "invalid", "TBD"}) {
        TempTree tree;
        write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
        std::string points{hardpoints_xml};
        if (!value.empty()) points.insert(points.find("</HardPoint>"),
            "<Randomize_Between_Fire_Bones>" + value + "</Randomize_Between_Fire_Bones>");
        write(tree.root / "XML" / "eawr_hardpoints.xml", points);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WWP-22: authored fire-bone fixture loads");
        if (!loaded.tables) continue;
        const auto* type = loaded.tables->find("Test_Frigate");
        expect(type && !type->hardpoints.empty(), "WWP-22: fixture has an ordinary hardpoint");
        if (!type || type->hardpoints.empty()) continue;
        // AU-82: nonempty tokens use the retail first-character rule; TBD is absent.
        const bool randomized = value != "No" && value != "invalid";
        expect(type->hardpoints.front().randomize_between_fire_bones == randomized,
            "WWP-22: absent and explicit true use the retail default; false opts out");
        auto combat = eawr::units::combat_table(*loaded.tables);
        expect(static_cast<bool>(combat), "WWP-22: authored data binds to the combat profile");
        if (combat) {
            const auto* profile = combat.value().find(eawr::assets::object_type_crc("Test_Frigate"));
            expect(profile && !profile->weapons.empty()
                && profile->weapons.front().randomize_between_fire_bones == randomized,
                "WWP-22: profile retains the validated flag");
        }
        if (value.empty()) defaults = *loaded.tables;
        if (defaults && (value == "No" || value == "Yes"))
            expect((eawr::units::content_identity(*defaults) == eawr::units::content_identity(*loaded.tables))
                    == (value == "Yes"), "WWP-22: semantic flag value binds replay content identity");
        if (value == "invalid")
            expect(!has_row(loaded.tables->unresolved, "HP_Test_Laser", "Randomize_Between_Fire_Bones"),
                "AU-82: a noncanonical false token is accepted by the retail boolean rule");
        if (value == "TBD")
            expect(!has_row(loaded.tables->unresolved, "HP_Test_Laser", "Randomize_Between_Fire_Bones"),
                "ML-24/WWP-22: a placeholder retains the default without a diagnostic");
    }
}

void unknown_inaccuracy_categories() {
    for (const std::string tag : {"Fire_Inaccuracy_Distance", "Targeting_Fire_Inaccuracy"}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string rows = "<" + tag + "> fIgHtEr|Bomber, 7 </" + tag + ">"
            "<" + tag + "> SpaceStructure, 15 </" + tag + ">"
            "<" + tag + "> Fighter|SpaceStructure, 19 </" + tag + ">"
            "<" + tag + "> Corvette, 3 </" + tag + ">";
        std::string points{hardpoints_xml};
        if (tag == "Fire_Inaccuracy_Distance") {
            const auto first = points.find("<Fire_Inaccuracy_Distance>");
            const auto last = points.rfind("</Fire_Inaccuracy_Distance>");
            points.replace(first, last + std::string_view{"</Fire_Inaccuracy_Distance>"}.size() - first, rows);
        } else {
            const std::string marker = "<SpaceUnit Name=\"Test_Fighter\">";
            xml.insert(xml.find(marker) + marker.size(), rows);
        }
        write_fixture(tree.root, xml);
        write(tree.root / "XML" / "eawr_hardpoints.xml", points);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "ML-51: an unknown inaccuracy category retains the object");
        if (!loaded.tables) continue;
        const auto& tables = *loaded.tables;
        const auto* unit = tables.find(tag == "Fire_Inaccuracy_Distance" ? "Test_Frigate" : "Test_Fighter");
        if (!unit) { expect(false, "ML-51: fixture owner remains loaded"); continue; }
        const auto& selected = tag == "Fire_Inaccuracy_Distance" ? unit->hardpoints.front().weapon : unit->weapon;
        expect(selected.has_value(), "ML-51: fixture weapon remains loaded");
        if (!selected) continue;
        const auto* weapon = &*selected;
        expect(weapon->inaccuracy.size() == 2 && weapon->inaccuracy[0].distance == Fixed::from_raw(raw(7))
                && weapon->inaccuracy[1].distance == Fixed::from_raw(raw(3)),
            "ML-51: case-insensitive valid rows preserve order and values around the omitted unknown");
        expect(has_row(tables.unresolved, tag == "Fire_Inaccuracy_Distance" ? "HP_Test_Laser" : "Test_Fighter",
                tag, "SpaceStructure"), "ML-51: unknown names remain visible to strict stock-data checks");
        expect(has_row(tables.unresolved, tag == "Fire_Inaccuracy_Distance" ? "HP_Test_Laser" : "Test_Fighter",
                tag, "Fighter|SpaceStructure"), "ML-51: any unknown member diagnoses and omits the whole mask");
        auto combat = eawr::units::combat_table(tables);
        expect(static_cast<bool>(combat), "ML-51: an unknown row does not refuse the combat table");
        if (!combat) continue;
        const auto* profile = combat.value().find(eawr::assets::object_type_crc(unit->id));
        expect(profile && !profile->weapons.empty() && profile->weapons.front().shot
                && profile->weapons.front().shot->inaccuracy.size() == 2
                && profile->weapons.front().shot->inaccuracy[0].category_bits == 3,
            "ML-51: valid compact category masks OR their bits and both surviving rows reach the firing profile");
        auto direct = tables;
        for (auto& type : direct.units) {
            if (type.weapon) type.weapon->inaccuracy.push_back({"UnknownSyntheticCategory", Fixed::from_raw(raw(99))});
            for (auto& hardpoint : type.hardpoints)
                if (hardpoint.weapon) hardpoint.weapon->inaccuracy.push_back({"UnknownSyntheticCategory", Fixed::from_raw(raw(99))});
        }
        expect(static_cast<bool>(eawr::units::combat_table(direct)),
            "ML-51: directly supplied tables also omit unknown inaccuracy categories");
        // Names are drawn from the loaded dictionary, including names absent from stock data.
        direct.categories.push_back({"UnknownSyntheticCategory", 0x400});
        auto custom = eawr::units::combat_table(direct);
        expect(static_cast<bool>(custom), "ML-51: a mod-defined category binds without a fixed name list");
        if (custom) {
            const auto* custom_profile = custom.value().find(eawr::assets::object_type_crc(unit->id));
            expect(custom_profile && !custom_profile->weapons.empty() && custom_profile->weapons.front().shot
                    && !custom_profile->weapons.front().shot->inaccuracy.empty()
                    && custom_profile->weapons.front().shot->inaccuracy.back().category_bits == 0x400,
                "ML-51: a newly defined category retains its authored scatter mask");
        }
        for (auto& type : direct.units)
            for (auto& hardpoint : type.hardpoints)
                if (hardpoint.weapon) hardpoint.weapon->category_restrictions = {"AnotherUnknownCategory"};
        expect(static_cast<bool>(eawr::units::combat_table(direct)),
            "ML-53: a directly supplied unknown Fire_Category_Restrictions name no longer refuses the table");
    }
}

void unknown_restriction_categories() {
    TempTree tree;
    auto points = std::string{hardpoints_xml};
    const std::string rows = "<Fire_Category_Restrictions> fIgHtEr, Vehicle_AI, Bomber, vehicle_ai </Fire_Category_Restrictions>";
    points.insert(points.find("<Fire_Inaccuracy_Distance>"), rows);
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    write(tree.root / "XML" / "eawr_hardpoints.xml", points);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "ML-53: an unknown Fire_Category_Restrictions name retains the object");
    if (!loaded.tables) return;
    const auto& tables = *loaded.tables;
    expect(has_row(tables.unresolved, "HP_Test_Laser", "Fire_Category_Restrictions", "Vehicle_AI"),
        "ML-53: the unknown name stays visible to strict stock-data checks");
    expect(std::any_of(tables.unresolved.begin(), tables.unresolved.end(), [](const eawr::units::Unresolved& row) {
        return row.owner == "HP_Test_Laser" && row.field == "Fire_Category_Restrictions"
            && row.reason == "unknown category; name ignored (ML-53)";
    }), "ML-53: the unresolved row carries the reason text");
    expect(std::count_if(tables.unresolved.begin(), tables.unresolved.end(), [](const eawr::units::Unresolved& row) {
        return row.owner == "HP_Test_Laser" && row.field == "Fire_Category_Restrictions";
    }) == 1, "ML-53: a repeated unknown name is reported once per owner");
    const auto bit = [&](const std::string_view name) {
        for (const auto& entry : tables.categories) if (entry.name == name) return entry.value;
        return std::uint64_t{0};
    };
    // Categories-first ordering guard: names resolve at hardpoint parse, so "fIgHtEr" and "Bomber" only keep their
    // bits below if the category dictionary is loaded before the hardpoints; a reordered loader zeroes the mask.
    auto combat = eawr::units::combat_table(tables);
    expect(static_cast<bool>(combat), "ML-53: the combat table binds with an unknown restriction name");
    if (!combat) return;
    const auto* unit = tables.find("Test_Frigate");
    const auto* profile = unit ? combat.value().find(eawr::assets::object_type_crc(unit->id)) : nullptr;
    expect(profile && !profile->weapons.empty()
            && profile->weapons.front().category_restrictions == (bit("Fighter") | bit("Bomber")),
        "ML-53: known names keep their bits, the unknown name adds none");
}

// ML-53: an empty list and a list of only unknown names both load, restrict nothing and (for the second) report each name.
void restriction_list_edges() {
    for (const bool unknown : {false, true}) {
        TempTree tree;
        auto points = std::string{hardpoints_xml};
        const std::string rows = unknown
            ? "<Fire_Category_Restrictions> Nope_One, nope_two </Fire_Category_Restrictions>"
            : "<Fire_Category_Restrictions> </Fire_Category_Restrictions>";
        points.insert(points.find("<Fire_Inaccuracy_Distance>"), rows);
        write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
        write(tree.root / "XML" / "eawr_hardpoints.xml", points);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "ML-53: an empty or all-unknown restriction list retains the object");
        if (!loaded.tables) return;
        const auto count = std::count_if(loaded.tables->unresolved.begin(), loaded.tables->unresolved.end(),
            [](const eawr::units::Unresolved& row) { return row.field == "Fire_Category_Restrictions"; });
        expect(count == (unknown ? 2 : 0), "ML-53: each distinct unknown name is reported, an empty list reports none");
        auto combat = eawr::units::combat_table(*loaded.tables);
        expect(static_cast<bool>(combat), "ML-53: the combat table binds");
        if (!combat) return;
        const auto* unit = loaded.tables->find("Test_Frigate");
        const auto* profile = unit ? combat.value().find(eawr::assets::object_type_crc(unit->id)) : nullptr;
        expect(profile && !profile->weapons.empty() && profile->weapons.front().category_restrictions == 0,
            "ML-53: no restriction bits remain");
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
    for (const auto hero_class : {std::string_view{"HeroUnit"}, std::string_view{"GenericHeroUnit"}}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto station = xml.find("<StarBase Name=\"Skirmish_Test_Base\">");
        xml.insert(xml.find('>', station) + 1,
            "<Tactical_Buildable_Objects_Multiplayer>Rebel, Capital_Hero, Fighter_Hero, Fighter_Company, Ground_Hero"
            "</Tactical_Buildable_Objects_Multiplayer>");
        const std::string purchase =
            "<Tactical_Build_Cost_Multiplayer>100</Tactical_Build_Cost_Multiplayer>"
            "<Tactical_Build_Time_Seconds>1</Tactical_Build_Time_Seconds>"
            "<Tactical_Production_Queue>Tactical_Units</Tactical_Production_Queue>"
            "<Build_Limit_Current_Per_Player>1</Build_Limit_Current_Per_Player>"
            "<Build_Limit_Lifetime_Per_Player>-1</Build_Limit_Lifetime_Per_Player>";
        const std::string open = "<" + std::string(hero_class) + " Name=\"";
        const std::string close = "</" + std::string(hero_class) + ">";
        xml.insert(xml.rfind("</"),
            open + "Capital_Hero_Base\"><Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>"
            "<Is_Named_Hero>yes</Is_Named_Hero><CategoryMask>Capital</CategoryMask>" + purchase + close
            + open + "Capital_Hero\"><Variant_Of_Existing_Type>Capital_Hero_Base</Variant_Of_Existing_Type>" + close
            + open + "Fighter_Hero\"><Variant_Of_Existing_Type>Test_Fighter</Variant_Of_Existing_Type>"
            "<Is_Named_Hero>yes</Is_Named_Hero><SpaceBehavior>FIGHTER_LOCOMOTOR, TARGETING, REVEAL</SpaceBehavior>"
            + purchase + close
            + "<HeroCompany Name=\"Fighter_Company\"><Affiliation>Rebel</Affiliation><Is_Named_Hero>yes</Is_Named_Hero>"
            "<Company_Transport_Unit>Test_Fighter</Company_Transport_Unit><Company_Units>Ground_Hero</Company_Units>"
            + purchase + "</HeroCompany>"
            + open + "Ground_Hero\"><Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>"
            "<Is_Named_Hero>yes</Is_Named_Hero><SpaceBehavior>REVEAL</SpaceBehavior>" + purchase + close);
        write_fixture(tree.root, xml);
        write(tree.root / "XML" / "FactionFiles.xml", "<Faction_Files><File>hero_factions.xml</File></Faction_Files>");
        write(tree.root / "XML" / "hero_factions.xml", "<Factions><Faction Name=\"Rebel\" /></Factions>");
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WPR-33: standalone capital and fighter hero XML loads");
        if (!loaded.tables) continue;
        const auto* capital = loaded.tables->find("Capital_Hero");
        const auto* fighter = loaded.tables->find("Fighter_Hero");
        const auto* rider = loaded.tables->find("Ground_Hero");
        const auto* company = loaded.tables->find("Fighter_Company");
        expect(capital && capital->hull == Fixed::from_raw(raw(3600)) && capital->kind == UnitKind::ship
            && !capital->hardpoints.empty() && capital->named_hero,
            "WHE-SQ-02: inherited standalone hero keeps its ship hull, weapons and hero identity without SpaceHero category");
        expect(fighter && fighter->hull == Fixed::from_raw(raw(60)) && fighter->kind == UnitKind::craft,
            "WHE-SQ-02: directly purchased fighter hero keeps its fighter body");
        expect(rider && !rider->hull && company && company->deployed_space_type == "Test_Fighter"
            && company->creates_carried_heroes,
            "WHE-07/49: hero without a space locomotor remains carried even with an inherited model and health");
        namespace t = eawr::sim::tactical;
        namespace sk = eawr::skirmish;
        const auto motion = eawr::units::motion_table(*loaded.tables);
        const auto combat = eawr::units::combat_table(*loaded.tables);
        expect(motion && combat && motion.value().find(sk::type_id("Capital_Hero"))
            && motion.value().squadrons.find_craft(sk::type_id("Fighter_Hero"))
            && combat.value().find(sk::type_id("Capital_Hero")) && combat.value().find(sk::type_id("Fighter_Hero")),
            "WHE-SQ-02: both standalone hero bodies bind usable motion and combat profiles");
        sk::SkirmishStart start;
        start.setup.players = {{1, 1, sk::faction_id("Rebel"), 1}};
        start.setup.units = {{1, sk::type_id("Skirmish_Test_Base"), 1, {}, eawr::sim::math::identity_quat(), {}}};
        auto economy = sk::economy_rules(start, {}, *loaded.tables);
        expect(static_cast<bool>(economy), "WPR-33: paired capital/fighter hero menu binds");
        if (!economy) continue;
        for (const auto& menu : economy.value().menus) if (menu.station == sk::type_id("Skirmish_Test_Base")) {
            for (const auto name : {"Capital_Hero", "Fighter_Hero", "Fighter_Company", "Ground_Hero"}) {
                const auto* option = menu.find(sk::type_id(name));
                const bool expected = std::string_view{name} != "Ground_Hero";
                expect(option && option->available == expected,
                    "WPR-33: only deployable bodies and the existing company conversion are purchasable");
                std::cout << "hero-purchase " << hero_class << ' ' << name << " available="
                    << (option && option->available) << '\n';
            }
        }
        economy.value().income.clear();
        economy.value().players = {{1, Fixed::from_raw(raw(10000)), 100, false, {}}};
        auto session = t::TacticalSession::create(start.setup, {}, {}, {}, {}, {}, {}, {}, economy.value());
        expect(static_cast<bool>(session), "WPR-33: hero purchase session creates");
        if (!session) { std::cerr << session.error().message << '\n'; continue; }
        std::uint64_t sequence{};
        for (const auto name : {"Capital_Hero", "Capital_Hero", "Fighter_Hero", "Fighter_Company", "Ground_Hero"}) {
            expect(static_cast<bool>(session.value().submit({{0, 1, sequence++}, {1}, t::BuyPayload{sk::type_id(name)}})),
                "WPR-33: hero purchase request submits");
        }
        const eawr::sim::InlineExecutor executor;
        for (unsigned frame = 0; frame < 95; ++frame) {
            const auto stepped = session.value().step(executor);
            expect(static_cast<bool>(stepped), "WPR-33: paired hero purchases complete");
            if (!stepped) break;
        }
        const auto ledgers = session.value().ledgers();
        expect(ledgers.size() == 1 && ledgers.front().credits == Fixed::from_raw(raw(9700))
            && ledgers.front().pool.size() == 3,
            "WPR-33: both body kinds and fighter company are paid and pooled; duplicate limit and carried-body refusal remain");
        if (ledgers.size() == 1) for (const auto name : {"Capital_Hero", "Fighter_Hero", "Fighter_Company"}) {
            expect(std::count(ledgers.front().pool.begin(), ledgers.front().pool.end(), sk::type_id(name)) == 1,
                "WPR-33: each eligible hero purchase completes once under its logical identity");
        }
    }
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
        <Enable_Radar_Ability Name="comm-radar">
          <Activation_Style>Skirmish_Automatic</Activation_Style>
          <Initially_Enabled>No</Initially_Enabled><Affects_All_Allies>True</Affects_All_Allies>
        </Enable_Radar_Ability>
      </Abilities>)");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    if (!loaded.tables) return;
    const auto* unit = loaded.tables->find("Test_Frigate");
    expect(unit && unit->special_abilities.size() == 3, "WHE-09/10: typed nested declarations load in authored order");
    if (!unit || unit->special_abilities.size() != 3) return;
    const auto& first = unit->special_abilities.front();
    expect(first.name == "first-handler" && first.service_interval == 1 && first.initially_enabled == true && first.causes_despawn,
        "WHE-09/10: authored gates remain separate from code-derived service intervals");
    expect(eawr::sim::tactical::special_type_matches(first.filter, eawr::assets::object_type_crc("Test_Fighter"), 0)
        && first.filter.applicable_types.size() == 1, "WHE-14: exact XML types are canonical and bypass authored exclusions");
    expect(unit->special_abilities[1].style == eawr::sim::tactical::SpecialActivationStyle::unspecified
        && unit->special_abilities[1].service_interval == 0 && !unit->special_abilities[1].initially_enabled,
        "U-05: missing activation style/default enablement are preserved, without inventing a space policy");
    const auto table = eawr::units::ability_table(*loaded.tables);
    const auto& radar = unit->special_abilities.back();
    expect(radar.kind == eawr::sim::tactical::SpecialAbilityKind::enable_radar
        && radar.style == eawr::sim::tactical::SpecialActivationStyle::skirmish_automatic
        && radar.initially_enabled == false && radar.affects_all_allies && radar.service_interval == 0,
        "WRAD-01/04/FC-75: named skirmish radar binds allied recipients, disabled authoring and no polling");
    const auto* profile = table ? table.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
    expect(profile && profile->special == unit->special_abilities, "WHE-10: nested profiles bind to session content");
    auto changed = *loaded.tables;
    for (auto& type : changed.units) if (type.id == unit->id) type.special_abilities[0].filter.excluded_categories ^= 1;
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "nested filter content binds to replay identity");
    changed = *loaded.tables;
    for (auto& type : changed.units) if (type.id == unit->id) type.special_abilities.back().affects_all_allies = false;
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "WRAD-01: allied radar authoring binds replay content identity");
}

void projectile_defence_tables() {
    for (const auto kind : {"MISSILE_SHIELD", "sensor_jamming"}) for (const bool radius_first : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        const auto type = "<Type>" + std::string(kind) + "</Type>";
        const std::string radius = "<Effective_Radius>750</Effective_Radius>";
        xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin,
            "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability>"
                + (radius_first ? radius + type : type + radius)
                + "<Expiration_Seconds>30</Expiration_Seconds><Recharge_Seconds>75</Recharge_Seconds>"
                + "</Unit_Ability></Unit_Abilities_Data>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WPJ-17/WHE-32: defence radius XML loads in either child order");
        if (!loaded.tables) continue;
        const auto* unit = loaded.tables->find("Test_Frigate");
        expect(unit && unit->abilities.size() == 1 && unit->abilities[0].effective_radius == Fixed::from_raw(raw(750)),
            "WPJ-17/WHE-32: loader retains authored defence radius before or after Type");
        const auto table = eawr::units::ability_table(*loaded.tables);
        const auto* profile = table ? table.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        expect(profile && profile->abilities.size() == 1
            && profile->abilities[0].kind == eawr::sim::tactical::ability_kind(kind)
            && profile->abilities[0].effective_radius == Fixed::from_raw(raw(750))
            && profile->abilities[0].expiration_frames == 900 && profile->abilities[0].recharge_frames == 2250,
            "WPJ-17/WHE-31/32: XML radius and timers bind to the production ability profile");
    }
}

void concentrate_fire_tables() {
    projectile_defence_tables();
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

    // RFL-03: synthetic inputs match the projectile reached by UM06_EclipseDefensePad.
    TempTree authored_tree;
    auto authored = xml;
    const auto replace = [&](const std::string& before, const std::string& after) {
        authored.replace(authored.find(before), before.size(), after);
    };
    replace("<Projectile_Rocket_Curve_Distance>500", "<Projectile_Rocket_Curve_Distance>400");
    replace("<Projectile_Rocket_Curve_Offset>0", "<Projectile_Rocket_Curve_Offset>180");
    replace("<Projectile_Rocket_Straight_Distance>500", "<Projectile_Rocket_Straight_Distance>120");
    replace("<Max_Speed>12", "<Max_Speed>20");
    write_fixture(authored_tree.root, authored);
    const auto authored_loaded = load(authored_tree.root, models(-40.0F));
    expect(authored_loaded.tables.has_value(), "RFL-03: authored nonzero rocket inputs load from synthetic XML");
    if (!authored_loaded.tables) return;
    const auto accepted = eawr::units::combat_table(*authored_loaded.tables);
    expect(static_cast<bool>(accepted), "RFL-03: UM06_EclipseDefensePad's reachable rocket profile now passes combat validation");
    if (!accepted) return;
    const auto* accepted_craft = accepted.value().find(eawr::assets::object_type_crc("Test_Fighter"));
    expect(accepted_craft && !accepted_craft->weapons.empty() && accepted_craft->weapons[0].shot
        && accepted_craft->weapons[0].shot->flight && accepted_craft->weapons[0].shot->speed == Fixed::from_raw(raw(20))
        && accepted_craft->weapons[0].shot->flight->curve_offset == Fixed::from_raw(raw(180))
        && accepted_craft->weapons[0].shot->flight->curve_distance == Fixed::from_raw(raw(400))
        && accepted_craft->weapons[0].shot->flight->straight_distance == Fixed::from_raw(raw(120))
        && accepted_craft->weapons[0].shot->flight->authored_distance == Fixed::from_raw(raw(3000)),
        "RFL-02/03/05: accepted loader profile retains all authored route values");

    // ML-54: an absent rocket path input takes the object-type default, each one independently.
    for (const bool all : {true, false}) {
        TempTree defaulted_tree;
        auto defaulted = xml;
        const auto erase = [&](const std::string& tag) {
            const auto begin = defaulted.find("<" + tag + ">");
            const auto close = "</" + tag + ">";
            const auto end = defaulted.find(close, begin);
            if (begin == std::string::npos || end == std::string::npos) { expect(false, "ML-54: fixture tag " + tag); return; }
            defaulted.erase(begin, end + close.size() - begin);
        };
        erase("Projectile_Rocket_Curve_Offset");
        if (all) {
            erase("Projectile_Rocket_Curve_Distance");
            erase("Projectile_Rocket_Straight_Distance");
        }
        write_fixture(defaulted_tree.root, defaulted);
        const auto defaulted_loaded = load(defaulted_tree.root, models(-40.0F));
        expect(defaulted_loaded.tables.has_value(), "ML-54: a rocket without path inputs loads");
        if (!defaulted_loaded.tables) continue;
        std::vector<eawr::core::Diagnostic> notes;
        const auto bound = eawr::units::combat_table(*defaulted_loaded.tables, eawr::units::ContentErrors::skip_object, &notes);
        expect(static_cast<bool>(bound), "ML-54: a rocket without authored path inputs binds");
        if (!bound) continue;
        const auto* defaulted_craft = bound.value().find(eawr::assets::object_type_crc("Test_Fighter"));
        const auto* flight = defaulted_craft && !defaulted_craft->weapons.empty() && defaulted_craft->weapons[0].shot
            && defaulted_craft->weapons[0].shot->flight ? &*defaulted_craft->weapons[0].shot->flight : nullptr;
        expect(flight && flight->curve_offset == Fixed::from_raw(raw(30))
                && flight->curve_distance == (all ? Fixed::from_raw(raw(50)) : Fixed::from_raw(raw(500)))
                && flight->straight_distance == (all ? Fixed::from_raw(raw(40)) : Fixed::from_raw(raw(500))),
            "ML-54: absent inputs take 40/50/30 and authored ones stay");
        expect(std::count_if(notes.begin(), notes.end(), [](const eawr::core::Diagnostic& note) {
            return note.message.find("(ML-54)") != std::string::npos;
        }) == (all ? 3 : 1), "ML-54: each defaulted input warns once with the projectile id");
    }
}

// #4589, IS-10: an ion stun reduction is not range-checked (FoC floors the factor, never refuses), and a
// content error in one unit type is skipped in a lenient load and refused in a strict one.
void ion_stun_content_errors() {
    TempTree tree;
    write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "IS-10: ion stun fixture loads");
    if (!loaded.tables) return;
    auto changed = *loaded.tables;
    std::string stunner;
    for (auto& projectile : changed.projectiles) {
        projectile.ion_stun = true;
        projectile.ion_stun_duration = Fixed::from_raw(raw(1));
        projectile.ion_stun_shot_rate_reduction = Fixed::from_raw(raw(100)); // Republic at War: 100.00
        projectile.ion_stun_speed_reduction = Fixed::from_raw(-raw(2));      // negative: faster
        stunner = projectile.id;
    }
    expect(!stunner.empty(), "IS-10: fixture has projectiles");
    auto combat = eawr::units::combat_table(changed);
    expect(static_cast<bool>(combat), "IS-10: reductions of 100 and -2 bind (FoC range-checks neither)");
    if (!combat) return;
    bool found = false;
    for (const auto& profile : combat.value().profiles) {
        for (const auto& weapon : profile.weapons) {
            if (!weapon.shot || !weapon.shot->ion_stun) continue;
            found = true;
            const auto stun = eawr::sim::tactical::ion_stun(std::nullopt, *weapon.shot->ion_stun, 10);
            expect(eawr::sim::tactical::ion_fire_rate(stun, 11) == Fixed::from_raw((raw(1) + 50) / 100)
                    && eawr::sim::tactical::ion_speed_factor(stun, 11) == Fixed::from_raw(raw(3)),
                "IS-10: a reduction of 100 floors the fire rate at 0.01; -2 triples the speed");
        }
    }
    expect(found, "IS-10: the stunning shot reaches a weapon");

    // An authored magnitude beyond the saturation limit still binds; a lenient load says so once per projectile.
    auto absurd = changed;
    for (auto& projectile : absurd.projectiles) projectile.ion_stun_speed_reduction = Fixed::from_raw(raw(5000));
    std::vector<eawr::core::Diagnostic> notes;
    expect(static_cast<bool>(eawr::units::combat_table(absurd, eawr::units::ContentErrors::skip_object, &notes)),
        "IS-10: a reduction beyond the saturation limit still binds");
    expect(!notes.empty() && notes.front().severity == eawr::core::Severity::warning
            && notes.front().message.find("saturates") != std::string::npos
            && notes.front().message.find("skipped unit type") == std::string::npos,
        "IS-10: saturation leaves a warning that is not a skipped type");

    // One bad object: a radius stun is refused (IS-08) on every projectile.
    for (auto& projectile : changed.projectiles) {
        projectile.ion_stun_radius = Fixed::from_raw(raw(50));
    }
    expect(!eawr::units::combat_table(changed), "content errors: strict (default) refuses the table");
    std::vector<eawr::core::Diagnostic> skipped;
    auto lenient = eawr::units::combat_table(changed, eawr::units::ContentErrors::skip_object, &skipped);
    expect(static_cast<bool>(lenient) && !skipped.empty(),
        "content errors: a lenient load skips the bad types and reports them");
    if (!lenient || skipped.empty()) return;
    expect(skipped.front().message.find("skipped unit type") != std::string::npos
            && skipped.front().message.find("IS-08") != std::string::npos,
        "content errors: the report names the type and the cause");
    expect(lenient.value().profiles.size() + skipped.size() <= changed.units.size()
            && lenient.value().profiles.size() < combat.value().profiles.size(),
        "content errors: a skipped type has no profile; the rest stay");
}

void hardpoint_turret_loading() {
    for (const bool attached : {false, true}) for (const bool missing : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string old = "HP_Test_Laser";
        for (auto at = xml.find(old); at != std::string::npos; at = xml.find(old, at + 1))
            xml.replace(at, old.size(), "HP_Renamed_Turret");
        write_fixture(tree.root, xml);
        std::string points{hardpoints_xml};
        if (!attached) {
            const std::string model_tag = "<Model_To_Attach>Test_Turret.alo</Model_To_Attach>";
            points.erase(points.find(model_tag), model_tag.size());
        }
        points.insert(points.find("</HardPoints>"),
            "<HardPoint Name=\"HP_Renamed_Turret\"><Variant_Of_Existing_Type>HP_Test_Laser</Variant_Of_Existing_Type>"
            "<Is_Turret>Yes</Is_Turret><Turret_Bone_Name>Yaw_Test</Turret_Bone_Name>"
            "<Barrel_Bone_Name>Pitch_Test</Barrel_Bone_Name><Turret_Rotate_Speed>2</Turret_Rotate_Speed>"
            "<Turret_Rotate_Extent_Degrees>135</Turret_Rotate_Extent_Degrees>"
            "<Turret_Elevate_Extent_Degrees>40</Turret_Elevate_Extent_Degrees>"
            "<Turret_Rest_Angle>0, 0, 3</Turret_Rest_Angle>"
            "<Turret_Rotation_Offset>0, 0, 7</Turret_Rotation_Offset></HardPoint>");
        write(tree.root / "XML" / "eawr_hardpoints.xml", points);
        auto assets = models(-40.0F);
        auto& parent = assets.at("data/art/models/test_frigate.alo");
        // Colliding names on the owner must never steal an attached model's binding.
        parent.bones.push_back(bone("Yaw_Test", 0, {90, 0, 0}));
        parent.bones.push_back(bone("Pitch_Test", 0, {91, 0, 0}));
        parent.bones.push_back(bone("FP_F-L_00", 0, {92, 0, 0}));
        auto& chosen = attached ? assets.at("data/art/models/test_turret.alo") : parent;
        if (attached) {
            chosen.bones.push_back(bone("Yaw_Test", 0, {4, 0, 0}));
            chosen.bones.push_back(bone("Pitch_Test", 0, {5, 0, 0}));
        }
        if (missing) {
            for (auto& value : chosen.bones) if (value.name == "Yaw_Test") value.name = "Missing_Yaw";
        }
        auto loaded = load(tree.root, assets);
        expect(loaded.tables.has_value(), "HPT-C09: inherited renamed ordinary turret XML loads");
        if (!loaded.tables) continue;
        const auto* type = loaded.tables->find("Test_Frigate");
        if (!type) continue;
        const auto& hp = type->hardpoints.front();
        expect(hp.manual_is_turret && !hp.requires_manual_target && hp.id == "HP_Renamed_Turret"
            && hp.manual_rotate_speed == Fixed::from_raw(raw(2))
            && hp.manual_yaw_extent == Fixed::from_raw(raw(135))
            && hp.manual_pitch_extent == Fixed::from_raw(raw(40))
            && hp.manual_rest.z == Fixed::from_raw(raw(3)) && hp.manual_offset.z == Fixed::from_raw(raw(7)),
            "HPT-C09: ordinary turret inherits all authored angles/rate without manual-target flag");
        expect(hp.manual_turret.position.has_value() == !missing,
            "HPT-C10: missing required yaw bone has no fallback to the other model");
        if (!missing) expect(hp.manual_turret.from_attached_model == attached && hp.fire_a.from_attached_model == attached,
            "HPT-C10: turret and muzzle bind to the selected parent or attached model");
        auto combat = eawr::units::combat_table(*loaded.tables);
        expect(static_cast<bool>(combat), "HPT-C09: ordinary turret combat profile binds");
        if (combat) {
            const auto* profile = combat.value().find(eawr::assets::object_type_crc(type->id));
            expect(profile && !profile->weapons.empty() && profile->weapons.front().manual_turret_required
                && profile->weapons.front().manual_turret.has_value() == !missing,
                "HPT-C10: missing binding retains required-turret admission instead of fixed fire");
            if (!missing) {
                for (const int extent : {800, 999}) {
                    auto wide = *loaded.tables;
                    auto* wide_type = const_cast<eawr::units::UnitType*>(wide.find(type->id));
                    auto& hardpoint = wide_type->hardpoints.front();
                    hardpoint.manual_yaw_extent = Fixed::from_raw(raw(extent));
                    hardpoint.manual_pitch_extent = Fixed::from_raw(raw(extent));
                    const auto bound = eawr::units::combat_table(wide);
                    expect(static_cast<bool>(bound), "HPT-10: strict ordinary multi-turn extents bind");
                    if (bound) {
                        const auto* owner = bound.value().find(eawr::assets::object_type_crc(type->id));
                        const auto& turret = owner->weapons.front().manual_turret;
                        expect(turret && turret->yaw_extent == *hardpoint.manual_yaw_extent
                            && turret->pitch_extent == *hardpoint.manual_pitch_extent,
                            "HPT-10: ordinary binding preserves authored multi-turn extents");
                        const auto state = eawr::sim::tactical::initial_combat(*owner, 1, 0, 1);
                        expect(state.weapons.front().manual.has_value(),
                            "HPT-10: admitted ordinary multi-turn turret creates mechanical state");
                    }
                    hardpoint.requires_manual_target = true;
                    expect(!eawr::units::combat_table(wide),
                        "HPT-10: manual-assignment multi-turn extents retain strict refusal");
                }
                auto sheared = *loaded.tables;
                auto* altered = const_cast<eawr::units::UnitType*>(sheared.find(type->id));
                // A pure Y-on-X shear corrects exactly back to this invented bind frame.
                for (auto* bone : {&altered->hardpoints.front().attachment,
                         &altered->hardpoints.front().manual_turret, &altered->hardpoints.front().manual_barrel}) {
                    if (!bone->axes) continue;
                    const auto& x = (*bone->axes)[0];
                    auto& y = (*bone->axes)[1];
                    y = {Fixed::from_raw(y.x.raw() + x.x.raw() / 1024),
                        Fixed::from_raw(y.y.raw() + x.y.raw() / 1024),
                        Fixed::from_raw(y.z.raw() + x.z.raw() / 1024)};
                }
                std::vector<eawr::core::Diagnostic> notes;
                const auto corrected = eawr::units::combat_table(sheared, eawr::units::ContentErrors::strict, &notes);
                expect(static_cast<bool>(corrected), "HPT-09: slightly sheared loaded hardpoint frames bind");
                if (corrected) {
                    const auto* fixed = corrected.value().find(eawr::assets::object_type_crc(type->id));
                    expect(fixed && fixed->weapons.front().manual_turret == profile->weapons.front().manual_turret,
                        "HPT-09: fixed-order Gram-Schmidt recovers the orthonormal articulation frame exactly");
                }
                expect(notes.size() == 1 && notes.front().severity == eawr::core::Severity::info
                    && notes.front().message.find("HPT-09") != std::string::npos,
                    "HPT-09: one named informational load note per corrected hardpoint");
                (*altered->hardpoints.front().manual_turret.axes)[1]
                    = (*altered->hardpoints.front().manual_turret.axes)[0];
                const auto refused = eawr::units::combat_table(sheared);
                expect(!refused && refused.error().message.find("HPT-09: authored turret frame shear exceeds 1/256")
                    != std::string::npos, "HPT-09: above-limit authored frame refuses with its named safety reason");
                auto inert = combat.value();
                for (auto& owner : inert.profiles) for (auto& weapon : owner.weapons) {
                    weapon.requires_manual_target = false;
                    weapon.manual_turret_required = false;
                }
                expect(static_cast<bool>(eawr::sim::tactical::validate_combat(inert)),
                    "loaded synthetic turret geometry remains legal when its consumers are disabled");
                const auto* owner = inert.find(eawr::assets::object_type_crc(type->id));
                if (owner) {
                    const auto state = eawr::sim::tactical::initial_combat(*owner, 1, 0, 1);
                    expect(!state.weapons.front().manual,
                        "inert loaded turret geometry creates no manual or mechanical turret state");
                }
            }
        }
        auto changed = *loaded.tables;
        auto* changed_type = const_cast<eawr::units::UnitType*>(changed.find(type->id));
        changed_type->hardpoints.front().manual_rotate_speed = Fixed::from_raw(raw(3));
        expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
            "HPT-C09: ordinary turret mechanics enter content identity");
    }
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

void cluster_bomb_inputs() {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const auto begin = xml.find("<Unit_Abilities_Data");
    const auto end = xml.find("</Unit_Abilities_Data>", begin) + std::string_view{"</Unit_Abilities_Data>"}.size();
    xml.replace(begin, end - begin, R"xml(<Unit_Abilities_Data><Unit_Ability>
        <Type>CLUSTER_BOMB</Type><Recharge_Seconds>50</Recharge_Seconds><Supports_Autofire>True</Supports_Autofire>
        <GUI_Activated_Ability_Name>Test_Cluster</GUI_Activated_Ability_Name><Effective_Radius>999</Effective_Radius>
        </Unit_Ability></Unit_Abilities_Data><Abilities><Cluster_Bomb_Ability Name="Test_Cluster">
        <Activation_Style>User_Input</Activation_Style><Bomb_Type>Proj_Test_Small</Bomb_Type>
        <Number_Of_Bombs>12</Number_Of_Bombs><Detonation_Time_In_Secs>1.5</Detonation_Time_In_Secs>
        </Cluster_Bomb_Ability></Abilities>)xml");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "WCB-01: nested ring data loads");
    if (!loaded.tables) return;
    const auto built = eawr::units::ability_table(*loaded.tables, {});
    expect(static_cast<bool>(built), "WCB-01: named ring handler binds");
    if (!built) return;
    const auto* profile = built.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    const auto slot = profile ? eawr::sim::tactical::ability_slot(*profile, eawr::sim::tactical::AbilityKind::cluster_bomb) : std::nullopt;
    expect(slot.has_value(), "WCB-01: cluster slot is exposed");
    if (slot) {
        const auto& ability = profile->abilities[*slot];
        expect(ability.spawned && ability.spawned->ring_count == 12 && ability.spawned->countdown_frames == 45
            && ability.recharge_frames == 1500 && ability.supports_autofire && ability.spawned->reach.raw() == 0,
            "WCB-01..03: ring uses nested count/countdown, never the unit ability radius");
    }
    auto changed = *loaded.tables;
    const auto unit = std::find_if(changed.units.begin(), changed.units.end(), [](const auto& type) { return type.cluster_bomb.has_value(); });
    if (unit == changed.units.end()) return;
    unit->cluster_bomb->count = 8;
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
        "WCB-01: authored ring count participates in content identity");
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


void object_script_inheritance() {
    for (const bool blank : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto add = [&](std::string_view opening, std::string_view tags) {
            const auto position = xml.find(opening);
            expect(position != std::string::npos, "SOS-01: fixture object exists");
            if (position != std::string::npos) xml.insert(position + opening.size(), tags);
        };
        add("<StarBase Name=\"Test_Base\">", "<Lua_Script>ObjectScript_MillenniumFalcon</Lua_Script>");
        add("<Container Name=\"Test_Pair_Base_Container\">", "<Lua_Script>ObjectScript_VaderTIESquadron</Lua_Script>");
        // ML-24/SOS-01: a blank or TBD Lua_Script on a variant changes nothing; the parent's script stays.
        if (blank) {
            add("<StarBase Name=\"Skirmish_Test_Base\">", "<Lua_Script/>");
            add("<Container Name=\"Test_Pair_Container\">", "<Lua_Script></Lua_Script>");
        }
        // The purchase type's script cannot override the actual created team.
        add("<Squadron Name=\"Test_Squadron_Pair\">", "<Lua_Script>PurchaseOnly</Lua_Script>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "SOS-01: inherited object-script tables load");
        if (!loaded.tables) continue;
        const auto* ship = loaded.tables->find("Skirmish_Test_Base");
        const auto* squadron = loaded.tables->find("Test_Squadron_Pair");
        expect(ship && ship->lua_script == "ObjectScript_MillenniumFalcon",
            "SOS-01: object attachment resolves variant; a blank override keeps the inherited script");
        expect(squadron && squadron->lua_script == "ObjectScript_VaderTIESquadron",
            "SOS-01/19: runtime team attachment resolves inherited container; a blank override keeps it");
    }
    // An unresolved team is reported and attaches nothing; the purchase script is not substituted.
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const std::string team = "<Create_Team_Type> Test_Pair_Container </Create_Team_Type>";
    const auto position = xml.find(team);
    expect(position != std::string::npos, "SOS-01: fixture team reference exists");
    if (position == std::string::npos) return;
    xml.replace(position, team.size(), "<Create_Team_Type>Missing_Test_Team</Create_Team_Type><Lua_Script>PurchaseOnly</Lua_Script>");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F));
    expect(loaded.tables.has_value(), "SOS-01: unresolved team still loads");
    if (!loaded.tables) return;
    const auto* squadron = loaded.tables->find("Test_Squadron_Pair");
    expect(squadron && squadron->lua_script.empty(), "SOS-01: unresolved team attaches no script");
    expect(std::any_of(loaded.tables->unresolved.begin(), loaded.tables->unresolved.end(), [](const auto& row) {
        return row.owner == "Test_Squadron_Pair" && row.field == "Create_Team_Type";
    }), "SOS-01: unresolved team is reported");
}


} // namespace unit_tables_test_support
