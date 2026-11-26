#include "unit_tables_support.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/pathfind.hpp"
#include "eawr/units/content_selection.hpp"
#include "unit_tables_synthetic_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace unit_tables_test_support {
namespace {

void template_durability_tables() {
    // SG-CLS-02: a mod label does not interrupt inherited body health or armor.
    for (const bool override_values : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        xml.insert(xml.rfind("</Objects>"), std::string{R"xml(
<Template Name="Hull_Template">
  <Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>
  <Tactical_Health>80</Tactical_Health><Armor_Type>Armor_Test</Armor_Type>
</Template>
<SpaceUnit Name="Template_Body">
  <Variant_Of_Existing_Type>Hull_Template</Variant_Of_Existing_Type>
)xml"} + (override_values
            ? "<Tactical_Health>120</Tactical_Health><Armor_Type>Armor_Fighter</Armor_Type>" : "") + R"xml(
</SpaceUnit>
<Template Name="Unclassed_Body">
  <Tactical_Health>40</Tactical_Health><Armor_Type>Armor_Test</Armor_Type>
</Template>
<TransportUnit Name="Transport_Body">
  <Variant_Of_Existing_Type>Hull_Template</Variant_Of_Existing_Type>
  <Tactical_Health>100</Tactical_Health>
</TransportUnit>
<Template Name="Template_Squadron">
  <Variant_Of_Existing_Type>Hull_Template</Variant_Of_Existing_Type>
  <Squadron_Units>Test_Fighter, 2</Squadron_Units>
</Template>)xml");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F), "Normal_Default",
            eawr::sim::tactical::BattleDomain::space,
            {"Hull_Template", "Template_Body", "Unclassed_Body", "Transport_Body", "Template_Squadron"});
        expect(loaded.tables.has_value(), "SG-CLS-02: custom-label durability fixture loads");
        if (!loaded.tables) continue;
        const auto durability = eawr::units::durability_table(*loaded.tables);
        expect(durability && durability.value().damage, "SG-CLS-02: authored durability and armor rules bind");
        if (!durability || !durability.value().damage) continue;
        const auto indices = eawr::units::damage_type_index(*loaded.tables);
        for (const std::string_view id : {"Hull_Template", "Template_Body", "Unclassed_Body", "Transport_Body"}) {
            const auto* profile = durability.value().find(eawr::assets::object_type_crc(id));
            expect(profile != nullptr, "SG-CLS-02: admitted body has durability independent of its element label");
            if (!profile) continue;
            const bool overridden = id == "Template_Body" && override_values;
            const auto health = id == "Unclassed_Body" ? 60 : id == "Transport_Body" ? 150 : overridden ? 180 : 120;
            expect(profile->max_hull == Fixed::from_raw(raw(health)),
                "SG-CLS-02: inherited or overridden health reaches the scaled live hull maximum");
            expect(profile->armor_type == indices.armor(overridden ? "Armor_Fighter" : "Armor_Test"),
                "SG-CLS-02: inherited or overridden armor reaches the live profile");
            auto state = eawr::sim::tactical::full_durability(*profile);
            eawr::sim::tactical::Hit hit;
            hit.amount = Fixed::from_raw(raw(10));
            hit.damage_type = indices.damage("Damage_Turbolaser");
            hit.projectile = true;
            hit.shield_damage = false;
            hit.allow_diminishing_firepower = false;
            const auto result = eawr::sim::tactical::apply_hit(*profile, *durability.value().damage, state, hit, 1);
            expect(result && state.hull == Fixed::from_raw(raw(health - (overridden ? 10 : 5))),
                "SG-CLS-02: the authored armor changes actual projectile hull damage");
        }
        expect(!durability.value().find(eawr::assets::object_type_crc("Template_Squadron")),
            "SG-CLS-02: a template resolving as Squadron still has no pooled hull profile");
    }
}

void subobject_list_tables() {
    const auto blocks = [](const std::string_view attribute, const int value) {
        const auto text = std::to_string(value);
        return "<Unit_Abilities_Data" + std::string(attribute) + "><Unit_Ability><Type>HUNT</Type>"
            "<Recharge_Seconds>" + text + "</Recharge_Seconds></Unit_Ability></Unit_Abilities_Data>"
            "<Abilities" + std::string(attribute) + "><Income_Stream_Ability Name=\"Stream_" + text + "\">"
            "<Base_Income_Value>" + text + "</Base_Income_Value><Base_Interval_In_Secs>10</Base_Interval_In_Secs>"
            "</Income_Stream_Ability></Abilities>";
    };
    const auto fixture = [](const std::string& base, const std::string& variant) {
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto begin = xml.find("<Unit_Abilities_Data", xml.find("<SpaceUnit Name=\"Test_Frigate\">"));
        const auto end = xml.find("</Unit_Abilities_Data>", begin) + std::string_view{"</Unit_Abilities_Data>"}.size();
        xml.replace(begin, end - begin, base);
        xml.insert(xml.rfind("</Objects>"), "<UniqueUnit Name=\"List_Variant\">"
            "<Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>" + variant + "</UniqueUnit>");
        return xml;
    };
    for (const std::string_view attribute : {"", " SubObjectList=\"Yes\"", " SubObjectList=\"yes\"",
        " SubObjectList=\"YES\"", " SubObjectList=\"True\"", " SubObjectList=\"1\"",
        " SubObjectList=\"No\"", " SubObjectList=\" Yes \""}) {
        TempTree tree;
        write_fixture(tree.root, fixture(blocks(attribute, 3), {}));
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* unit = loaded.tables ? loaded.tables->find("Test_Frigate") : nullptr;
        const bool enabled = attribute == " SubObjectList=\"Yes\"";
        expect(unit && unit->abilities.size() == (enabled ? 1U : 0U)
            && unit->production.income.size() == (enabled ? 1U : 0U),
            "WABL-01: both ability containers require the exact Yes attribute to decode nested rows");
        if (enabled && unit && !unit->abilities.empty() && !unit->production.income.empty()) {
            expect(unit->abilities.front().recharge_seconds == Fixed::from_raw(raw(3))
                && unit->production.income.front().base_value == Fixed::from_raw(raw(3)),
                "WABL-01: dispatched rows reach the timer and income consumers");
        }
    }
    for (const bool marked : {false, true}) {
        TempTree tree;
        auto authored = blocks(marked ? " SubObjectList=\"Yes\"" : "", 3);
        for (const std::string_view tag : {"<Unit_Abilities_Data", "<Abilities"}) {
            authored.insert(authored.find('>', authored.find(tag)) + 1, "dispatch");
        }
        write_fixture(tree.root, fixture(authored, {}));
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* unit = loaded.tables ? loaded.tables->find("Test_Frigate") : nullptr;
        const auto count = marked ? 2U : 1U;
        expect(unit && unit->abilities.size() == count && unit->production.income.size() == count,
            "WABL-01: scalar and nested-list dispatch are independent calls in an ordinary type");
    }
    for (const std::string& variant : {std::string{}, blocks("", 7), blocks(" SubObjectList=\"No\"", 7),
        std::string{"<Abilities SubObjectList=\"Yes\"/><Unit_Abilities_Data SubObjectList=\"Yes\"/>"}}) {
        TempTree tree;
        write_fixture(tree.root, fixture(blocks(" SubObjectList=\"Yes\"", 3), variant));
        const auto loaded = load(tree.root, models(-40.0F), "Normal_Default",
            eawr::sim::tactical::BattleDomain::space, {"List_Variant"});
        const auto* unit = loaded.tables ? loaded.tables->find("List_Variant") : nullptr;
        expect(unit && unit->abilities.size() == 1 && unit->production.income.size() == 1,
            "WABL-02: omitted, unmarked, false and childless variant blocks preserve inherited abilities");
        if (unit && !unit->abilities.empty() && !unit->production.income.empty()) {
            expect(unit->abilities.front().recharge_seconds == Fixed::from_raw(raw(3))
                && unit->production.income.front().base_value == Fixed::from_raw(raw(3)),
                "WABL-02: ignored variant contents cannot replace inherited data");
        }
    }
    for (const bool marked_wrapper : {false, true}) {
        TempTree tree;
        const auto wrapper = std::string{"<Wrapper"} + (marked_wrapper ? " SubObjectList=\"Yes\">" : ">")
            + blocks(" SubObjectList=\"Yes\"", 3) + "</Wrapper>";
        write_fixture(tree.root, fixture(wrapper, {}));
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* unit = loaded.tables ? loaded.tables->find("Test_Frigate") : nullptr;
        expect(unit && unit->abilities.size() == (marked_wrapper ? 0U : 1U)
            && unit->production.income.size() == (marked_wrapper ? 0U : 1U),
            "WABL-01: ordinary wrappers recurse but another marked list owns its children");
    }
    {
        TempTree tree;
        write_fixture(tree.root, fixture(blocks(" SubObjectList=\"Yes\"", 3)
                + blocks(" SubObjectList=\"Yes\"", 5),
            blocks(" SubObjectList=\"Yes\"", 7) + blocks(" SubObjectList=\"Yes\"", 9)));
        const auto loaded = load(tree.root, models(-40.0F), "Normal_Default",
            eawr::sim::tactical::BattleDomain::space, {"List_Variant"});
        const auto* base = loaded.tables ? loaded.tables->find("Test_Frigate") : nullptr;
        const auto* variant = loaded.tables ? loaded.tables->find("List_Variant") : nullptr;
        expect(base && base->abilities.size() == 2 && base->production.income.size() == 2,
            "WABL-02: ordinary repeated list blocks append in authored order");
        expect(variant && variant->abilities.size() == 1 && variant->production.income.size() == 1,
            "WABL-02: each dispatched variant block replaces the previous list");
        if (variant && !variant->abilities.empty() && !variant->production.income.empty()) {
            expect(variant->abilities.front().recharge_seconds == Fixed::from_raw(raw(9))
                && variant->production.income.front().base_value == Fixed::from_raw(raw(9)),
                "WABL-02: the last dispatched variant block supplies both lists");
        }
    }
}

void structure_soft_radius_tables() {
    // AV-05 / WWP-21: selected layered space bodies share the model-backed soft radius.
    for (const std::string_view xml_class : {"SpaceBuildable", "StarBase", "SpecialStructure"}) {
        for (const auto& [override_value, expected_radius] : std::array{
            std::pair{"", 18}, std::pair{"4", 8}, std::pair{"0", 40}}) {
            TempTree tree;
            auto xml = units_xml("3600", "Unit_Select_Test");
            const std::string original_radius = "<Custom_Soft_Footprint_Radius> 40.0 </Custom_Soft_Footprint_Radius>";
            xml.erase(xml.find(original_radius), original_radius.size());
            const std::string scale = "<Scale_Factor>0.70</Scale_Factor>";
            xml.replace(xml.find(scale), scale.size(), "<Scale_Factor>2</Scale_Factor>");
            const std::string opening = "<SpaceUnit Name=\"Test_Frigate\">";
            const auto start = xml.find(opening);
            xml.replace(xml.find("</SpaceUnit>", start), std::string_view{"</SpaceUnit>"}.size(),
                "</" + std::string(xml_class) + ">");
            xml.replace(start, opening.size(), "<" + std::string(xml_class) + " Name=\"Test_Frigate\">"
                "<Variant_Of_Existing_Type>Soft_Radius_Parent</Variant_Of_Existing_Type>"
                + (std::string_view{override_value}.empty() ? std::string{} :
                    "<Custom_Soft_Footprint_Radius>" + std::string(override_value) + "</Custom_Soft_Footprint_Radius>"));
            xml.insert(xml.find('>') + 1, "<" + std::string(xml_class) + " Name=\"Soft_Radius_Parent\">"
                "<Custom_Soft_Footprint_Radius>9</Custom_Soft_Footprint_Radius></" + std::string(xml_class) + ">");
            const std::string behavior = "<SpaceBehavior>TARGETING,REVEAL , HIDE_WHEN_FOGGED, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>";
            xml.replace(xml.find(behavior), behavior.size(), "<SpaceBehavior>SPACE_OBSTACLE REVEAL</SpaceBehavior>");
            write_fixture(tree.root, xml);
            auto assets = models(-40.0F);
            eawr::assets::Mesh mesh;
            mesh.name = "COLLISIONBOX";
            mesh.collidable = true;
            mesh.bone = 0;
            mesh.bounds_min = {-10, -20, -5};
            mesh.bounds_max = {10, 20, 5};
            assets.at("data/art/models/test_frigate.alo").meshes.push_back(mesh);
            const auto loaded = load(tree.root, assets);
            expect(loaded.tables.has_value(), "AV-05: structure soft-radius fixture loads");
            if (!loaded.tables) continue;
            const auto* unit = loaded.tables->find("Test_Frigate");
            expect(unit && unit->xml_type == xml_class, "AV-05: each selected structure class retains its body");
            const auto motion = eawr::units::motion_table(*loaded.tables);
            const auto* footprint = motion ? motion.value().footprint(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
            expect(footprint && footprint->obstacle && footprint->radius == Fixed::from_raw(raw(expected_radius))
                && footprint->x_extent == Fixed::from_raw(raw(20)) && footprint->y_extent == Fixed::from_raw(raw(40)),
                "AV-05: inherited/overridden soft radius scales once; explicit zero falls back to model half extents");
            if (!footprint) continue;
            const auto tracked = eawr::sim::tactical::tracking_leaf(1, *footprint, {}, {});
            expect(tracked && tracked.value().reach == Fixed::from_raw(raw(expected_radius)),
                "WHZ-03: selected structure soft radius reaches obstacle tracking");
            if (!tracked) continue;
            eawr::sim::tactical::TrackingLayerView layer;
            layer.windows = {{tracked.value()}};
            eawr::sim::tactical::LinearQuery query;
            query.start = {Fixed::from_raw(raw(12)), Fixed{}};
            query.end = query.start;
            query.end_frame = Fixed::from_raw(raw(1));
            query.facing = {Fixed::from_raw(raw(1)), Fixed{}};
            const auto hit = eawr::sim::tactical::find_linear_collision(layer, 90, query);
            expect(hit && hit.value() == (expected_radius > 12 ? eawr::sim::tactical::collision_static : 0),
                "AV-05: structure soft-radius authoring changes the live obstacle collision verdict");
        }
    }
}

void signed_weaken_reduction_tables() {
    for (const auto reduction : {"-0.15", "0", "0.2", "-1.5", "1.5"}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin,
            "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>WEAKEN_ENEMY</Type>"
            "<Spawned_Object_Type>Proj_Test_Small</Spawned_Object_Type>"
            "<Recharge_Seconds>20</Recharge_Seconds><Effective_Radius>1000</Effective_Radius>"
            "</Unit_Ability></Unit_Abilities_Data>");
        const std::string opening = "<Projectile Name=\"Proj_Test_Small\">";
        xml.insert(xml.find(opening) + opening.size(),
            "<Projectile_Weaken_Enemy_On_Detonation>Yes</Projectile_Weaken_Enemy_On_Detonation>"
            "<Projectile_Weaken_Enemy_Radius>300</Projectile_Weaken_Enemy_Radius>"
            "<Projectile_Weaken_Enemy_Duration_Seconds>15</Projectile_Weaken_Enemy_Duration_Seconds>"
            "<Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent>" + std::string(reduction)
            + "</Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent>"
            "<Projectile_Weaken_Enemy_Targets_Category_Mask>Frigate</Projectile_Weaken_Enemy_Targets_Category_Mask>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WHE-62: signed weaken reduction XML loads");
        if (!loaded.tables) continue;
        const auto abilities = eawr::units::ability_table(*loaded.tables);
        const auto* hero = abilities ? abilities.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        expect(hero && hero->abilities.size() == 1 && hero->abilities.front().spawned.has_value(),
            "WHE-62: signed reduction retains its owning unit and ability");
        if (!hero || hero->abilities.empty() || !hero->abilities.front().spawned) continue;
        expect(hero->abilities.front().spawned->weaken.cause_damage_reduction == Fixed::from_decimal(reduction).value(),
            "WHE-62: authored reduction keeps its sign without clamping");
    }
}
// #7643 audit: the space loader reads a type's reveal, targeting and object-weapon tags from the shared object body,
// so every class that can stand in a space skirmish decodes them the same way (SpaceUnit is the reference).
void class_agnostic_reveal_targeting_weapon_tables() {
    std::optional<Fixed> reveal, max_distance;
    std::string priority_set;
    for (const std::string_view xml_class : {"SpaceUnit", "UniqueUnit", "HeroUnit", "GenericHeroUnit", "SpecialStructure",
                                            "SecondaryStructure", "SpaceBuildable", "StarBase", "Freighter"}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string opening = "<SpaceUnit Name=\"Test_Frigate\">";
        const auto start = xml.find(opening);
        const std::string class_name{xml_class};
        if (xml_class != "SpaceUnit") {
            xml.replace(xml.find("</SpaceUnit>", start), std::string_view{"</SpaceUnit>"}.size(), "</" + class_name + ">");
            xml.replace(start, opening.size(), "<" + class_name + " Name=\"Test_Frigate\">");
        }
        const std::string class_opening = "<" + class_name + " Name=\"Test_Frigate\">";
        xml.insert(xml.find(class_opening) + class_opening.size(),
            "<Dense_FOW_Reveal_Range_Multiplier>0.25</Dense_FOW_Reveal_Range_Multiplier>"
            "<Projectile_Types>Proj_Test_Small</Projectile_Types>"
            "<Projectile_Fire_Recharge_Seconds>7</Projectile_Fire_Recharge_Seconds>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(static_cast<bool>(loaded.tables), "#7643 audit: class fixture loads");
        if (!loaded.tables) continue;
        const auto* body = loaded.tables->find("Test_Frigate");
        expect(body && body->xml_type == xml_class, "#7643 audit: each class under test keeps its own body");
        if (!body) continue;
        expect(body->dense_fow_multiplier == Fixed::from_decimal("0.25").value(),
            "V-22: the dense reveal multiplier is read for every space class");
        expect(body->weapon && body->weapon->projectile == "Proj_Test_Small"
            && body->weapon->min_recharge_seconds && body->weapon->min_recharge_seconds.value() == Fixed::from_decimal("7").value(),
            "EN-05: the object weapon projectile and recharge are read for every space class");
        if (xml_class == "SpaceUnit") {
            reveal = body->fow_reveal_range; max_distance = body->targeting_max_attack_distance;
            priority_set = body->targeting_priority_set;
            expect(reveal && max_distance && !priority_set.empty(), "#7643 audit: the reference class authors reveal and targeting");
            continue;
        }
        expect(body->fow_reveal_range == reveal && body->targeting_max_attack_distance == max_distance
            && body->targeting_priority_set == priority_set,
            "S-26: reveal range, targeting reach and priority set decode like the reference class");
    }
}

// WRB-01 (debug build: the redirect apply effect tests no object class): SpaceUnit and UniqueUnit carriers keep the
// nested handler; the land hero and structure classes that author it in the stock data stay out of the space content.
void redirect_carrier_class_tables() {
    const std::array<std::pair<std::string_view, bool>, 4> carriers{{
        {"SpaceUnit", true}, {"UniqueUnit", true}, {"HeroUnit", false}, {"SpecialStructure", false}}};
    for (const auto& [xml_class, admitted] : carriers) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string opening = "<SpaceUnit Name=\"Test_Frigate\">";
        const auto start = xml.find(opening);
        const std::string class_name{xml_class};
        if (xml_class != "SpaceUnit") {
            xml.replace(xml.find("</SpaceUnit>", start), std::string_view{"</SpaceUnit>"}.size(), "</" + class_name + ">");
            xml.replace(start, opening.size(), "<" + class_name + " Name=\"Test_Frigate\">");
        }
        const std::string class_opening = "<" + class_name + " Name=\"Test_Frigate\">";
        xml.insert(xml.find(class_opening) + class_opening.size(),
            "<Abilities SubObjectList=\"Yes\"><Redirect_Blaster_Ability Name=\"Test_Redirect\">"
            "<Initially_Enabled>No</Initially_Enabled><Activation_Style>Take_Damage</Activation_Style>"
            "<Redirect_Chance>0.2</Redirect_Chance><Block_Chance>0.3</Block_Chance>"
            "<Reaction_Arc_In_Degrees>360</Reaction_Arc_In_Degrees>"
            "<Max_Projectile_Redirection_Angle_In_Degrees>30</Max_Projectile_Redirection_Angle_In_Degrees>"
            "</Redirect_Blaster_Ability></Abilities>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(static_cast<bool>(loaded.tables), "WRB-01: carrier class fixture loads");
        if (!loaded.tables) continue;
        const auto abilities = eawr::units::ability_table(*loaded.tables);
        const auto* profile = abilities ? abilities.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        const bool has_handler = profile != nullptr && std::any_of(profile->special.begin(), profile->special.end(),
            [](const auto& special) { return special.kind == eawr::sim::tactical::SpecialAbilityKind::redirect_blaster; });
        expect(has_handler == admitted, std::string("WRB-01: a ") + class_name + " carrier "
            + (admitted ? "keeps" : "does not keep") + " its redirect handler");
    }
}

// WRB-01: a UniqueUnit that inherits the nested handler from its parent type keeps it, and one that authors its own
// handler under the same name replaces the inherited one (the class check runs on the final type).
void redirect_carrier_inheritance_tables() {
    const auto handler_xml = [](const std::string_view chance) {
        return "<Abilities SubObjectList=\"Yes\"><Redirect_Blaster_Ability Name=\"Test_Redirect\">"
               "<Initially_Enabled>No</Initially_Enabled><Activation_Style>Take_Damage</Activation_Style>"
               "<Redirect_Chance>" + std::string{chance} + "</Redirect_Chance><Block_Chance>0.3</Block_Chance>"
               "<Reaction_Arc_In_Degrees>360</Reaction_Arc_In_Degrees>"
               "<Max_Projectile_Redirection_Angle_In_Degrees>30</Max_Projectile_Redirection_Angle_In_Degrees>"
               "</Redirect_Blaster_Ability></Abilities>";
    };
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const std::string opening = "<SpaceUnit Name=\"Test_Frigate\">";
    xml.insert(xml.find(opening) + opening.size(), handler_xml("0.2"));
    xml.insert(xml.rfind("</Objects>"),
        "<UniqueUnit Name=\"Redirect_Inherits\"><Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>"
        "</UniqueUnit>"
        "<UniqueUnit Name=\"Redirect_Overrides\"><Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>"
        + handler_xml("0.7") + "</UniqueUnit>");
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F), "Normal_Default", eawr::sim::tactical::BattleDomain::space,
        {"Redirect_Inherits", "Redirect_Overrides"});
    expect(static_cast<bool>(loaded.tables), "WRB-01: inheritance fixture loads");
    if (!loaded.tables) return;
    const auto abilities = eawr::units::ability_table(*loaded.tables);
    const auto handlers = [&](const char* name) {
        std::vector<const eawr::sim::tactical::SpecialAbilityProfile*> found;
        const auto* profile = abilities ? abilities.value().find(eawr::assets::object_type_crc(name)) : nullptr;
        if (profile != nullptr)
            for (const auto& special : profile->special)
                if (special.kind == eawr::sim::tactical::SpecialAbilityKind::redirect_blaster) found.push_back(&special);
        return found;
    };
    const auto parent = handlers("Test_Frigate");
    const auto inherited = handlers("Redirect_Inherits");
    const auto overridden = handlers("Redirect_Overrides");
    expect(parent.size() == 1, "WRB-01: the SpaceUnit parent keeps its handler");
    expect(inherited.size() == 1 && parent.size() == 1 && inherited.front()->redirect_chance == parent.front()->redirect_chance,
        "WRB-01: a UniqueUnit inheriting the handler from its parent keeps it unchanged");
    expect(overridden.size() == 1 && parent.size() == 1
            && overridden.front()->redirect_chance != parent.front()->redirect_chance,
        "WRB-01: a UniqueUnit authoring its own handler replaces the inherited one");
}
} // namespace

// TCFA-16: effective data reaches the shared space AI and purchase consumers across class spellings.
static void space_ai_class_consumers() {
    namespace t = eawr::sim::tactical;
    const auto q = [](const int value) { return Fixed::from_integer(value).value(); };
    const auto target_id = eawr::assets::object_type_crc("Ai_Class_Target");
    const auto base_id = eawr::assets::object_type_crc("Skirmish_Test_Base");
    for (const std::string spelling : {"FlagshipTemplate", "Template", "Dummy", "DummyStructure", "SpaceBuildable",
        "Space_Indigenous_Unit", "SpaceHero", "Freighter", "SpaceFauna", "Mission_Object", "UnderConstructionShip",
        "Props_Wave", "SkirmishSpaceUnit", "FlagshipUnit", "SurvivalSpaceUnit", "Space_Unit", "TransportUnit", "HeroUnit"}) {
        for (const bool generic_flags : {false, true}) for (const bool overridden : {false, true}) {
            TempTree tree;
            auto xml = units_xml("3600", "Unit_Select_Test");
            const std::string parent_flags = "SIMPLE_SPACE_LOCOMOTOR, TARGETING, REVEAL";
            const std::string child_flags = parent_flags + ", UNIT_AI";
            const std::string parent_fields =
                "<AI_Combat_Power>123</AI_Combat_Power><Tactical_Production_Queue>Tactical_Units</Tactical_Production_Queue>"
                "<SpaceBehavior>" + (generic_flags ? std::string{"HIDE_WHEN_FOGGED"} : parent_flags)
                + "</SpaceBehavior><Behavior>" + (generic_flags ? parent_flags : std::string{"HIDE_WHEN_FOGGED"}) + "</Behavior>";
            const std::string child_fields =
                "<AI_Combat_Power>321</AI_Combat_Power><Tactical_Production_Queue>Tactical_Upgrades</Tactical_Production_Queue>"
                "<SpaceBehavior>" + (generic_flags ? std::string{"HIDE_WHEN_FOGGED"} : child_flags)
                + "</SpaceBehavior><Behavior>" + (generic_flags ? child_flags + ", SELECTABLE" : std::string{"SELECTABLE"}) + "</Behavior>";
            xml.insert(xml.rfind("</Objects>"), "<Template Name=\"Ai_Class_Parent\">"
                "<Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>" + parent_fields + "</Template>"
                "<" + spelling + " Name=\"Ai_Class_Target\"><Variant_Of_Existing_Type>Ai_Class_Parent</Variant_Of_Existing_Type>"
                "<Tactical_Build_Cost_Multiplayer>10</Tactical_Build_Cost_Multiplayer>"
                "<Tactical_Build_Time_Seconds>1</Tactical_Build_Time_Seconds>"
                + (overridden ? child_fields : "") + "</" + spelling + ">");
            const std::string station = "<StarBase Name=\"Skirmish_Test_Base\">";
            xml.insert(xml.find(station) + station.size(),
                "<Tactical_Buildable_Objects_Multiplayer>Rebel, Ai_Class_Target</Tactical_Buildable_Objects_Multiplayer>");
            write_fixture(tree.root, xml);
            write(tree.root / "XML" / "FactionFiles.xml", "<Faction_Files><File>Ai_Class_Factions.xml</File></Faction_Files>");
            write(tree.root / "XML" / "Ai_Class_Factions.xml", "<Factions><Faction Name=\"Rebel\" /></Factions>");
            const auto loaded = load(tree.root, models(-40.0F));
            expect(loaded.tables.has_value(), "TCFA-16: registered-class AI and queue fixture loads");
            if (!loaded.tables) continue;
            eawr::skirmish::SkirmishStart start;
            const auto faction = eawr::skirmish::faction_id("Rebel");
            start.setup.players = {{1, 1, faction, t::player_flag_commandable}};
            start.setup.units = {{1, base_id, 1, {}, eawr::sim::math::identity_quat(), {}}};
            const auto ai = eawr::skirmish::ai_setup(start, {}, *loaded.tables);
            const auto type = std::find_if(ai.content.types.begin(), ai.content.types.end(), [&](const auto& entry) {
                return entry.type_id == target_id;
            });
            expect(type != ai.content.types.end() && type->combat_power == eawr::script::numeric::LuaNumber(overridden ? 321 : 123)
                && type->unit_ai == overridden && type->selectable == overridden && type->locomotor,
                "TCFA-16: inherited and overridden power and behavior flags reach space AI inputs");
            auto rules = eawr::skirmish::economy_rules(start, {}, *loaded.tables);
            expect(static_cast<bool>(rules), "TCFA-16: ordinary space purchase menu compiles");
            if (!rules) continue;
            const auto queue = overridden ? t::BuildQueue::upgrades : t::BuildQueue::units;
            const auto* menu = rules.value().menu(base_id, faction);
            const auto* option = menu ? menu->find(target_id) : nullptr;
            expect(option && option->available && option->queue == queue,
                "TCFA-16: inherited and overridden queue reaches an available offer");
            if (!option || !option->available) continue;
            rules.value().players = {{1, q(100), 100, false, {}}};
            std::vector<std::string> reference;
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                auto session = t::TacticalSession::create(start.setup, {}, {}, {}, {}, {}, {}, {}, rules.value());
                expect(static_cast<bool>(session), "TCFA-16: purchase queue session creates");
                if (!session) continue;
                expect(static_cast<bool>(session.value().submit({{0, 1, 0}, {1}, t::BuyPayload{target_id}})),
                    "TCFA-16: ordinary purchase command submits");
                const eawr::platform::ThreadWorkerAdapter executor(workers);
                const auto step = session.value().step(executor);
                expect(static_cast<bool>(step), "TCFA-16: purchase command steps");
                const auto& ledger = session.value().ledgers().front();
                const auto chosen = static_cast<std::size_t>(queue);
                expect(ledger.queues[chosen].size() == 1 && ledger.queues[chosen].front().type == target_id
                    && ledger.queues[1U - chosen].empty(), "TCFA-16: purchase enters only the authored queue");
                if (step) {
                    const std::vector<std::string> hashes{step.value().state_sha256};
                    if (reference.empty()) reference = hashes;
                    else expect(hashes == reference, "TCFA-16: authored purchase queue hashes equal on 1/2/4/8 workers");
                }
            }
        }
    }
}

// WGCH-205 / WBP-31: the bribe price and sale percentage read the effective object, not its class spelling.
static void space_bribe_and_sale_class_consumers() {
    namespace t = eawr::sim::tactical;
    const auto q = [](const int value) { return Fixed::from_integer(value).value(); };
    for (const std::string spelling : {"SpecialStructure", "Template", "DummyStructure", "Freighter", "GenericHeroUnit",
        "HeroUnit", "Mission_Object", "Props_Wave", "SpaceFauna", "TransportUnit"}) {
        for (const bool overridden : {false, true}) {
            TempTree tree;
            auto xml = units_xml("3600", "Unit_Select_Test");
            xml.insert(xml.rfind("</Objects>"), "<Template Name=\"Price_Parent\">"
                "<Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>"
                "<Behavior>SELECTABLE, TACTICAL_SELL</Behavior>"
                "<Tactical_Bribe_Cost>100</Tactical_Bribe_Cost>"
                "<Bribe_Modifier_On_Corrupted_Planets>0.5</Bribe_Modifier_On_Corrupted_Planets>"
                "<Bribe_Modifier_On_Uncorrupted_Planets>1.5</Bribe_Modifier_On_Uncorrupted_Planets>"
                "<Tactical_Sell_Percentage>0.25</Tactical_Sell_Percentage></Template>"
                "<" + spelling + " Name=\"Price_Target\"><Variant_Of_Existing_Type>Price_Parent</Variant_Of_Existing_Type>"
                + (overridden ? "<Tactical_Bribe_Cost>400</Tactical_Bribe_Cost>"
                    "<Tactical_Sell_Percentage>0.75</Tactical_Sell_Percentage>" : "")
                + "</" + spelling + ">");
            write_fixture(tree.root, xml);
            const auto loaded = load(tree.root, models(-40.0F), "Normal_Default",
                eawr::sim::tactical::BattleDomain::space, {"Price_Target"});
            const auto* unit = loaded.tables ? loaded.tables->find("Price_Target") : nullptr;
            expect(unit != nullptr, "WGCH-205: bribe and sale fixture loads each class spelling: " + spelling);
            if (!unit) continue;
            const auto price = overridden ? 400 : 100;
            expect(unit->tactical_bribe_cost == q(price) && unit->tactical_sale
                && unit->tactical_sell_percentage == Fixed::from_decimal(overridden ? "0.75" : "0.25").value(),
                "WGCH-205/WBP-31: inherited and overridden price and sale percentage decode for " + spelling);
            const auto corrupted = eawr::units::tactical_bribe_cost(*unit, *unit, true);
            const auto clean = eawr::units::tactical_bribe_cost(*unit, *unit, false);
            expect(corrupted && clean && corrupted.value() == q(price / 2) && clean.value() == q(price * 3 / 2),
                "WGCH-205: the cost query applies the authored corrupted and uncorrupted modifiers for " + spelling);
            // The sale percentage reaches the skirmish economy table through the same loaded type.
            eawr::skirmish::SkirmishStart start;
            start.setup.players = {{1, 1, eawr::skirmish::faction_id("Rebel"), t::player_flag_commandable}};
            const auto rules = eawr::skirmish::economy_rules(start, {}, *loaded.tables);
            expect(static_cast<bool>(rules), "WBP-31: sale fixture economy binds for " + spelling);
            if (!rules) continue;
            const auto target_id = eawr::assets::object_type_crc("Price_Target");
            const auto sale = std::find_if(rules.value().pad_sales.begin(), rules.value().pad_sales.end(),
                [&](const auto& entry) { return entry.type == target_id; });
            expect(sale != rules.value().pad_sales.end()
                && sale->percentage == Fixed::from_decimal(overridden ? "0.75" : "0.25").value(),
                "WBP-31: the authored sale percentage reaches the economy sale table for " + spelling);
        }
    }
}

// WSU-12 / AV-05: the pick sphere and the hard-extent offsets are fields of the shared object type, read by the
// space body loader whatever the element is called. Element names that are not FoC classes settle to the nearest
// class on the variant chain (SG-CLS-01), so the loaded class equals the authored name only for known classes.
void space_body_collision_bound_tables() {
    for (const std::string_view xml_class : {"SpaceUnit", "TransportUnit", "SpaceBuildable", "StarBase",
            "SpecialStructure", "UniqueUnit", "HeroUnit", "GenericHeroUnit", "SpaceFauna", "DummyStructure",
            "Template", "SpaceProp_Obstacle", "UnderConstructionShip"}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        std::string carrier = "<" + std::string(xml_class) + " Name=\"Bound_Carrier\">"
            "<Variant_Of_Existing_Type>Test_Base</Variant_Of_Existing_Type>";
        // WHE-SQ-02: a hero without its own space locomotor is a carried identity with no body.
        if (xml_class == "HeroUnit" || xml_class == "GenericHeroUnit") {
            carrier += "<SpaceBehavior>SIMPLE_SPACE_LOCOMOTOR, SPAWN_SQUADRON</SpaceBehavior>";
        }
        carrier += "<Mouse_Collide_Override_Sphere_Radius>37</Mouse_Collide_Override_Sphere_Radius>"
            "<Custom_Hard_XExtent_Offset>3</Custom_Hard_XExtent_Offset>"
            "<Custom_Hard_YExtent_Offset>-4</Custom_Hard_YExtent_Offset></" + std::string(xml_class) + ">";
        xml.insert(xml.rfind("</Objects>"), carrier);
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F), "Normal_Default",
            eawr::sim::tactical::BattleDomain::space, {"Bound_Carrier"});
        const auto* unit = loaded.tables ? loaded.tables->find("Bound_Carrier") : nullptr;
        const bool known_class = eawr::data::is_foc_object_class(xml_class);
        const std::string detail = " [" + std::string(xml_class) + " type="
            + (unit ? unit->xml_type : std::string("none")) + "]";
        expect(unit && (!known_class || unit->xml_type == xml_class),
            "WSU-12: the bound carrier loads as a space body of its class" + detail);
        if (!unit) continue;
        expect(unit->mouse_collide_sphere_radius && *unit->mouse_collide_sphere_radius == Fixed::from_raw(raw(37)),
            "WSU-12: the authored pick sphere reaches the space body type" + detail);
        expect(unit->fog_box_offset.x == Fixed::from_raw(raw(3)) && unit->fog_box_offset.y == Fixed::from_raw(raw(-4)),
            "AV-05: the authored hard-extent offsets reach the space body fog box" + detail);
    }
}

void duplicate_hangar_starting_counts() {
    namespace tactical = eawr::sim::tactical;
    for (const unsigned tier : {0U, 3U}) for (const int first : {0, 2}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto replace = [&](const std::string& from, const std::string& to) {
            const auto at = xml.find(from);
            if (at == std::string::npos) return false;
            xml.replace(at, from.size(), to);
            return true;
        };
        const std::string suffix = std::to_string(tier);
        const std::string reserve_suffix = tier == 0 ? "0" : "2";
        const int reserve = tier == 0 ? 1 : -2;
        const bool authored = replace("<Space_Model_Name>TEST_FRIGATE.ALO</Space_Model_Name>",
                "<Space_Model_Name>Test_Station.alo</Space_Model_Name>")
            && replace("<SpaceBehavior>TARGETING,REVEAL , HIDE_WHEN_FOGGED, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>",
                "<SpaceBehavior>SPAWN_SQUADRON, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>"
                "<Starting_Spawned_Units_Tech_" + suffix + ">Test_Squadron, " + std::to_string(first)
                + "</Starting_Spawned_Units_Tech_" + suffix + ">"
                "<Starting_Spawned_Units_Tech_" + suffix + ">Test_Squadron_Pair, 3</Starting_Spawned_Units_Tech_" + suffix + ">"
                "<Starting_Spawned_Units_Tech_" + suffix + ">test_squadron, 7</Starting_Spawned_Units_Tech_" + suffix + ">"
                "<Reserve_Spawned_Units_Tech_" + reserve_suffix + ">Test_Squadron, " + std::to_string(reserve)
                + "</Reserve_Spawned_Units_Tech_" + reserve_suffix + ">"
                "<Reserve_Spawned_Units_Tech_" + reserve_suffix + ">Test_Squadron, 9</Reserve_Spawned_Units_Tech_" + reserve_suffix + ">"
                "<Spawned_Squadron_Delay_Seconds>0</Spawned_Squadron_Delay_Seconds>")
            && replace("<HardPoints> HP_Test_Laser, HP_Test_Engine, HP_Test_Missing </HardPoints>",
                "<HardPoints>HP_Test_Bay</HardPoints>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* carrier = loaded.tables ? loaded.tables->find("Test_Frigate") : nullptr;
        expect(authored && carrier && carrier->spawner
            && carrier->spawner->campaign_starting[tier].size() == 3
            && carrier->spawner->campaign_starting[tier][2].count == 7,
            "FL-22: XML keeps the ordered repeated starting row and its authored count");
        if (!carrier || !carrier->spawner || !loaded.tables) continue;
        const auto motion = eawr::units::motion_table(*loaded.tables);
        expect(static_cast<bool>(motion), "FL-22: repeated starting rows remain valid hangar content");
        if (!motion) continue;
        const auto type = eawr::assets::object_type_crc("Test_Frigate");
        const auto fighter = eawr::assets::object_type_crc("Test_Squadron");
        const auto bomber = eawr::assets::object_type_crc("Test_Squadron_Pair");
        for (const unsigned tech : {tier, 5U}) {
            const auto* profile = motion.value().squadrons.find_spawner_at_tech(type, 1, tech);
            expect(profile && profile->entries.size() == 3 && !profile->bays.empty(),
                "FL-22: selected and fallback tiers retain all three starting rows");
            if (!profile || profile->entries.size() != 3 || profile->bays.empty()) continue;
            expect(profile->entries[0].squadron == fighter && profile->entries[0].starting == first
                && profile->entries[1].squadron == bomber && profile->entries[1].starting == 3
                && profile->entries[2].squadron == fighter && profile->entries[2].starting == first
                && profile->entries[0].reserve == reserve && profile->entries[1].reserve == 0
                && profile->entries[2].reserve == reserve,
                "FL-22: first matching counts win independently for repeated starting and reserve types");
            tactical::SpawnerState state;
            const std::vector<bool> intact(profile->bays.size(), true);
            unsigned launches = 0;
            for (std::uint64_t frame = 0; frame < 600; frame += 30)
                if (tactical::service_spawner(*profile, state, 73, frame, 1, intact)) ++launches;
            expect(launches == static_cast<unsigned>(2 * first + 3)
                && state.entries.size() == 3 && state.entries[0].alive == first
                && state.entries[1].alive == 3 && state.entries[2].alive == first,
                "FL-22: live hangar fills the first-count caps, including a zero first count");
            if (state.entries.size() == 3) expect(state.entries[0].remaining
                    == (reserve < 0 ? tactical::unlimited_reserve : reserve)
                && state.entries[2].remaining == state.entries[0].remaining,
                "FL-22: finite and signed unlimited reserves retain their launch budget");
        }
    }
}

void synthetic_tables() {
    space_body_collision_bound_tables();
    space_ai_class_consumers();
    space_bribe_and_sale_class_consumers();
    template_durability_tables();
    // WBR-01: renamed live bodies own the automatic reveal source, not a purchase template.
    for (const auto& multiplier : {"1.75", "0.25", "1", "0", "-2"}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string ability = std::string("<Abilities SubObjectList=\"Yes\"><Battlefield_Modifier_Ability Name=\"Mod_Reveal\">")
            + "<Activation_Style>Space_Automatic</Activation_Style><Initially_Enabled>No</Initially_Enabled>"
            + "<Apply_To_All_Allies>Yes</Apply_To_All_Allies><Stacking_Category>7</Stacking_Category>"
            + "<FOW_Reveal_Range_Multiplier>" + multiplier + "</FOW_Reveal_Range_Multiplier></Battlefield_Modifier_Ability></Abilities>";
        std::vector<std::string> names;
        const auto append = [&](const std::string& kind, const std::string& name, const std::string& parent, std::string effect) {
            names.push_back(name);
            xml.insert(xml.rfind("</Objects>"), "<" + kind + " Name=\"" + name + "\">"
                + (parent.empty() ? "" : "<Variant_Of_Existing_Type>" + parent + "</Variant_Of_Existing_Type>")
                + effect + "</" + kind + ">");
        };
        append("SpaceUnit", "Mod_Reveal_Body", "Test_Frigate", ability);
        append("UniqueUnit", "Mod_Reveal_Unique", "Test_Frigate", ability);
        append("SpaceUnit", "Mod_Reveal_Parent", "Test_Frigate", ability);
        append("UniqueUnit", "Mod_Reveal_Inherited", "Mod_Reveal_Parent", "");
        auto overridden = ability;
        const auto value_begin = overridden.find("<FOW_Reveal_Range_Multiplier>") + std::string("<FOW_Reveal_Range_Multiplier>").size();
        overridden.replace(value_begin, overridden.find("</FOW_Reveal_Range_Multiplier>") - value_begin, "0.5");
        append("UniqueUnit", "Mod_Reveal_Override", "Mod_Reveal_Parent", overridden);
        append("SpaceUnit", "Mod_Reveal_Defaults", "Test_Frigate",
            "<Abilities SubObjectList=\"Yes\"><Battlefield_Modifier_Ability Name=\"Defaults\">"
            "<Activation_Style>Space_Automatic</Activation_Style><FOW_Reveal_Range_Multiplier>2</FOW_Reveal_Range_Multiplier>"
            "</Battlefield_Modifier_Ability></Abilities>");
        append("UpgradeObject", "Mod_Reveal_Upgrade", "", "<Behavior>DUMMY_UPGRADE</Behavior>" + ability);
        append("GenericHeroUnit", "Mod_Reveal_Carried", "", ability);
        append("Squadron", "Mod_Reveal_Purchase", "Test_Squadron", ability);
        auto manual = ability;
        const auto at = manual.find("Space_Automatic"); manual.replace(at, std::string("Space_Automatic").size(), "User_Input");
        append("SpaceUnit", "Mod_Reveal_Manual", "Test_Frigate", manual);
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F), "Normal_Default",
            eawr::sim::tactical::BattleDomain::space, names);
        expect(loaded.tables.has_value(), "WBR-01: automatic reveal XML loads");
        if (!loaded.tables) continue;
        const bool active = std::string_view(multiplier) == "1.75" || std::string_view(multiplier) == "0.25";
        const auto* defaults = loaded.tables->find("Mod_Reveal_Defaults");
        expect(defaults && defaults->production.battlefield_bonuses.size() == 1
            && defaults->production.battlefield_bonuses.front().enabled
            && !defaults->production.battlefield_bonuses.front().all_allies
            && defaults->production.battlefield_bonuses.front().category == 0,
            "WBR-01: omitted enabled/alliance/category fields use original defaults");
        for (const auto* name : {"Mod_Reveal_Body", "Mod_Reveal_Unique", "Mod_Reveal_Inherited"}) {
            const auto* body = loaded.tables->find(name);
            expect(body && body->production.battlefield_bonuses.size() == (active ? 1U : 0U),
                "WBR-01: neutral and nonpositive multipliers contribute no source");
            if (body && !body->production.battlefield_bonuses.empty()) {
                const auto& bonus = body->production.battlefield_bonuses.front();
                expect(bonus.name == "Mod_Reveal" && bonus.category == 7 && bonus.all_allies && !bonus.enabled
                    && bonus.defence == Fixed{} && bonus.reveal_delta == Fixed::from_raw(
                        std::string_view(multiplier) == "1.75" ? raw(3) / 4 : -raw(3) / 4),
                    "WBR-01: reveal authoring retains sign, category, alliance and enabled state");
            }
        }
        const auto* replaced = loaded.tables->find("Mod_Reveal_Override");
        expect(replaced && replaced->production.battlefield_bonuses.size() == 1
            && replaced->production.battlefield_bonuses.front().reveal_delta == Fixed::from_raw(-raw(1) / 2),
            "WBR-01/WABL-02: child ability block replaces the parent's reveal value without adding a second source");
        for (const auto* name : {"Mod_Reveal_Upgrade", "Mod_Reveal_Carried", "Mod_Reveal_Purchase", "Mod_Reveal_Manual"}) {
            const auto* body = loaded.tables->find(name);
            expect(body && body->production.battlefield_bonuses.empty(), "WBR-01: unadmitted source interfaces remain separate");
        }
    }
    // WBR-02..04: a live skirmish source runs under every style that is automatic in a multiplayer tactical game
    // (Space, Skirmish, Global, Combat); an absent style is invalid and a ground or manual style never runs. The
    // stealth, capture-time and special-weapon fields ride the same row; a reversed handler is kept but flagged.
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        std::vector<std::string> names;
        const auto append = [&](const std::string& name, const std::string& style, const std::string& effects) {
            names.push_back(name);
            xml.insert(xml.rfind("</Objects>"), "<SpaceUnit Name=\"" + name + "\"><Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>"
                "<Abilities SubObjectList=\"Yes\"><Battlefield_Modifier_Ability Name=\"Mod_Effect\">"
                + (style.empty() ? std::string() : "<Activation_Style>" + style + "</Activation_Style>") + effects
                + "</Battlefield_Modifier_Ability></Abilities></SpaceUnit>");
        };
        const std::string effects = "<Stealth_Duration_Multiplier>1.5</Stealth_Duration_Multiplier>"
            "<Capture_Point_Time_Multiplier>0.5</Capture_Point_Time_Multiplier><Enable_Special_Weapons>Yes</Enable_Special_Weapons>"
            "<Stacking_Category>2</Stacking_Category>";
        const std::array<std::pair<const char*, bool>, 7> styles{{{"Space_Automatic", true}, {"Skirmish_Automatic", true},
            {"Global_Automatic", true}, {"Combat_Automatic", true}, {"Ground_Automatic", false}, {"User_Input", false}, {"", false}}};
        for (std::size_t index = 0; index < styles.size(); ++index) append("Mod_Style_" + std::to_string(index), styles[index].first, effects);
        append("Mod_Style_Reverse", "Space_Automatic", effects + "<Reverse_Application_Logic>Yes</Reverse_Application_Logic>");
        append("Mod_Style_Neutral", "Space_Automatic", "<Stealth_Duration_Multiplier>1</Stealth_Duration_Multiplier>"
            "<Capture_Point_Time_Multiplier>-1</Capture_Point_Time_Multiplier>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F), "Normal_Default", eawr::sim::tactical::BattleDomain::space, names);
        expect(loaded.tables.has_value(), "WBR-02: ledger-effect XML loads");
        if (loaded.tables) {
            for (std::size_t index = 0; index < styles.size(); ++index) {
                const auto* body = loaded.tables->find("Mod_Style_" + std::to_string(index));
                expect(body && body->production.battlefield_bonuses.size() == (styles[index].second ? 1U : 0U),
                    "WBR-02: the activation style gate follows the game mode");
                if (body && styles[index].second) {
                    const auto& bonus = body->production.battlefield_bonuses.front();
                    expect(bonus.reveal_delta == Fixed{} && bonus.stealth_delta == Fixed::from_raw(raw(1) / 2)
                        && bonus.capture_delta == Fixed::from_raw(-raw(1) / 2) && bonus.special_weapons && !bonus.reverse
                        && bonus.category == 2, "WBR-02/03: stealth and capture deltas are multiplier minus one; special weapons flag");
                }
            }
            const auto* reversed = loaded.tables->find("Mod_Style_Reverse");
            expect(reversed && reversed->production.battlefield_bonuses.size() == 1 && reversed->production.battlefield_bonuses.front().reverse,
                "WBR-04: a reversed handler keeps its flag");
            const auto* neutral = loaded.tables->find("Mod_Style_Neutral");
            expect(neutral && neutral->production.battlefield_bonuses.empty(),
                "WBR-02: a neutral or nonpositive multiplier writes no entry");
            // WBR-02..04 sim path: each admitted style (Space, Skirmish, Global, Combat) reaches the live player
            // ledger of a standalone space battle through the real binding; a reversed handler adds nothing while
            // its source stands and nothing when the source is removed; the owner alone gains the permission.
            namespace tactical = eawr::sim::tactical;
            for (const std::string name : {"Mod_Style_0", "Mod_Style_1", "Mod_Style_2", "Mod_Style_3", "Mod_Style_Reverse"}) {
                const bool reversed = name == "Mod_Style_Reverse";
                eawr::skirmish::SkirmishStart start;
                start.setup.seed = 7696;
                start.setup.players = {{1, 0, 1, 1}, {2, 1, 2, 1}};
                const auto type = eawr::skirmish::type_id(name);
                start.setup.units = {{1, type, 1, {}, eawr::sim::math::identity_quat(), {}}};
                auto compiled = eawr::skirmish::economy_rules(start, {}, *loaded.tables);
                auto abilities = eawr::units::ability_table(*loaded.tables);
                expect(compiled && abilities, "WBR-02..04: ledger-effect content compiles");
                if (!compiled || !abilities) continue;
                const auto ledger_rows = static_cast<std::size_t>(std::count_if(compiled.value().command_bonuses.begin(),
                    compiled.value().command_bonuses.end(), [&](const auto& row) { return row.battlefield && row.type == type; }));
                expect(ledger_rows == (reversed ? 0U : 1U), "WBR-04: a reversed handler binds no ledger row");
                for (const auto& row : compiled.value().command_bonuses) {
                    if (!row.battlefield || row.type != type) continue;
                    expect(row.stealth_delta == Fixed::from_raw(raw(1) / 2) && row.capture_delta == Fixed::from_raw(-raw(1) / 2)
                        && row.special_weapons && row.apply_to_self && !row.all_allies && row.bonus.stacking_category == 2,
                        "WBR-02/03: the bound row carries the authored deltas");
                }
                compiled.value().players = {{1, {}, 100, false, {}}, {2, {}, 100, false, {}}};
                auto created = tactical::TacticalSession::create(start.setup, {}, {}, {}, {}, {}, {}, abilities.value(), compiled.value());
                expect(static_cast<bool>(created), "WBR-02..04: ledger-effect session creates");
                if (!created) continue;
                auto& world = created.value();
                const auto permission = [&](const std::size_t index) { return world.snapshot()->economy()[index].special_weapons_enabled; };
                expect(permission(0) == !reversed && !permission(1),
                    "WBR-03: a live source gives its owner the permission, a reversed one nothing, and never the enemy");
                const eawr::platform::ThreadWorkerAdapter executor(1);
                expect(static_cast<bool>(world.stage_remove(1)) && static_cast<bool>(world.step(executor)),
                    "WBR-03: source removal commits");
                expect(!permission(0) && !permission(1), "WBR-03/04: removal leaves no permission, reversed or not");
            }
        }
    }
    subobject_list_tables();
    structure_soft_radius_tables();
    signed_weaken_reduction_tables();
    duplicate_hangar_starting_counts();
    redirect_carrier_class_tables();
    redirect_carrier_inheritance_tables();
    class_agnostic_reveal_targeting_weapon_tables();
    // AV-05: XML overrides must reach the collision query, independently on each axis. The hard extents are
    // fields of the shared object type, so every class that can stand in a space battle takes the same path.
    for (const std::string_view xml_class : {"SpaceUnit", "TransportUnit", "StarBase", "SpaceBuildable", "HeroUnit",
            "SpaceHero", "Freighter", "UnderConstructionShip", "Template", "DummyStructure"}) {
        for (const auto& [tags, expected_x, expected_y] : std::array{
            std::tuple{"", 20, 40},
            std::tuple{"<Custom_Hard_XExtent>3</Custom_Hard_XExtent>", 6, 40},
            std::tuple{"<Custom_Hard_YExtent>4</Custom_Hard_YExtent>", 20, 8},
            std::tuple{"<Custom_Hard_XExtent>3</Custom_Hard_XExtent><Custom_Hard_YExtent>4</Custom_Hard_YExtent>", 6, 8}}) {
            TempTree tree;
            auto xml = units_xml("3600", "Unit_Select_Test");
            const std::string scale = "<Scale_Factor>0.70</Scale_Factor>";
            xml.replace(xml.find(scale), scale.size(), "<Scale_Factor>2</Scale_Factor>");
            const std::string opening = "<SpaceUnit Name=\"Test_Frigate\">";
            const auto start = xml.find(opening);
            const std::string class_name{xml_class};
            // SG-CLS-01: an element name that is no FoC class settles to the nearest class on its variant chain, so
            // the parent keeps the SpaceUnit class for those names and only the child carries the authored name.
            const bool known_class = eawr::data::is_foc_object_class(xml_class);
            const std::string parent_class = known_class ? class_name : std::string("SpaceUnit");
            if (xml_class != "SpaceUnit") {
                xml.replace(xml.find("</SpaceUnit>", start), std::string_view{"</SpaceUnit>"}.size(), "</" + class_name + ">");
                xml.replace(start, opening.size(), "<" + class_name + " Name=\"Test_Frigate\">");
            }
            const std::string class_opening = "<" + class_name + " Name=\"Test_Frigate\">";
            xml.insert(xml.find(class_opening) + class_opening.size(),
                "<Variant_Of_Existing_Type>Hard_Extent_Parent</Variant_Of_Existing_Type>");
            xml.insert(xml.find('>') + 1, "<" + parent_class + " Name=\"Hard_Extent_Parent\">" + std::string(tags) + "</" + parent_class + ">");
            write_fixture(tree.root, xml);
            auto assets = models(-40.0F);
            eawr::assets::Mesh mesh;
            mesh.name = "COLLISIONBOX";
            mesh.collidable = true;
            mesh.bone = 0;
            mesh.bounds_min = {-10, -20, -5};
            mesh.bounds_max = {10, 20, 5};
            assets.at("data/art/models/test_frigate.alo").meshes.push_back(mesh);
            const auto loaded = load(tree.root, assets);
            expect(static_cast<bool>(loaded.tables), "AV-05: hard-extent fixture loads");
            if (!loaded.tables) continue;
            const auto* body = loaded.tables->find("Test_Frigate");
            expect(body && (!known_class || body->xml_type == xml_class),
                "AV-05: each space class under test keeps its own body [" + class_name + "]");
            const auto motion = eawr::units::motion_table(*loaded.tables);
            const auto* footprint = motion ? motion.value().footprint(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
            expect(footprint && footprint->x_extent == Fixed::from_raw(raw(expected_x))
                && footprint->y_extent == Fixed::from_raw(raw(expected_y))
                && footprint->radius == Fixed::from_raw(raw(80)),
                "AV-05: inherited hard extents override each model axis, scale once and preserve the soft radius");
            if (!footprint) continue;
            const auto tracked = eawr::sim::tactical::tracking_leaf(1, *footprint, {}, {});
            expect(static_cast<bool>(tracked), "AV-05: XML hard extents enter live tracking");
            if (!tracked) continue;
            eawr::sim::tactical::TrackingLayerView layer;
            layer.windows = {{tracked.value()}};
            for (const bool x_axis : {true, false}) {
                eawr::sim::tactical::LinearQuery query;
                query.start = {Fixed::from_raw(raw(x_axis ? 12 : 0)), Fixed::from_raw(raw(x_axis ? 0 : 12))};
                query.end = query.start;
                query.end_frame = Fixed::from_raw(raw(1));
                query.facing = {Fixed::from_raw(raw(1)), Fixed{}};
                const auto hit = eawr::sim::tactical::find_linear_collision(layer, 90, query);
                const auto extent = x_axis ? expected_x : expected_y;
                expect(hit && hit.value() == (extent > 12 ? eawr::sim::tactical::collision_static : 0),
                    "AV-05: authored X and Y extents independently change the live collision verdict");
            }
        }
    }
    for (const bool structure : {false, true}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string marker = "<SpaceUnit Name=\"Test_Frigate\">";
        const auto start = xml.find(marker);
        xml.insert(start + marker.size(),
            "<Destruction_Survivors>Survivor_Extra, 0.25, Survivor_Land_Only, 0.5, Test_Squadron, 0.25</Destruction_Survivors>"
            "<Minimum_Destruction_Survivor_Count>4</Minimum_Destruction_Survivor_Count>"
            "<Maximum_Destruction_Survivor_Count>2</Maximum_Destruction_Survivor_Count>"
            "<Spawned_Squadron_Location_Bones>HP_E_BONE, Unused_Bone</Spawned_Squadron_Location_Bones>"
            "<Spawned_Squadron_Location_Flyout_Distances>123, 987</Spawned_Squadron_Location_Flyout_Distances>");
        if (structure) {
            const auto end = xml.find("</SpaceUnit>", start);
            xml.replace(end, std::string_view{"</SpaceUnit>"}.size(), "</SpecialStructure>");
            xml.replace(start, marker.size(), "<SpecialStructure Name=\"Test_Frigate\">");
        }
        xml.insert(xml.find("</Objects>"),
            "<SpaceUnit Name=\"Survivor_Extra\"><Variant_Of_Existing_Type>Test_Fighter</Variant_Of_Existing_Type></SpaceUnit>"
            "<GroundInfantry Name=\"Survivor_Land_Only\"><Land_Model_Name>unused</Land_Model_Name></GroundInfantry>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "DS-01/05: space survivor dependency fixture loads");
        if (!loaded.tables) continue;
        const auto* source = loaded.tables->find("Test_Frigate");
        const auto* extra = loaded.tables->find("Survivor_Extra");
        const auto combat = eawr::units::combat_table(*loaded.tables);
        const auto* profile = combat ? combat.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        expect(source && extra && source->destruction_survivors.size() == 3 && profile && profile->destruction_survivors,
            "DS-01: both assigned classes resolve survivor-only dependencies");
        if (source && profile && profile->destruction_survivors) {
            const auto& survivors = *profile->destruction_survivors;
            expect(survivors.minimum == 4 && survivors.maximum == 2 && survivors.choices.size() == 3
                && survivors.choices[0].type == eawr::assets::object_type_crc("Survivor_Extra")
                && survivors.choices[1].type == 0 && survivors.choices[1].weight == Fixed::from_raw(raw(1) / 2),
                "DS-01/02/05: signed bounds and unplaceable weight survive binding");
            expect(survivors.choices[2].company && survivors.choices[2].members.size() == 3,
                "DS-05: company expansion resolves mode-placeable member bodies");
            expect(survivors.flyout_distance == Fixed::from_raw(raw(123)) && survivors.bay_direction.has_value(),
                "DS-07: first authored flyout distance and bone bind to the sim");
            auto changed = *loaded.tables;
            for (auto& unit : changed.units) if (unit.id == "Test_Frigate") unit.survivor_maximum = 5;
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                "survivor authoring participates in optional content identity");
        }
    }
    // PL-01: optional immutable presentation input, with the viewer's existing boolean tokens.
    for (const std::string_view value : {"", "false", "true", "YES", "1", "0"}) {
        TempTree tree;
        write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
        const auto path = tree.root / "XML" / "GameConstants.xml";
        std::ifstream input(path);
        std::string constants{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        input.close();
        if (!value.empty()) constants.insert(constants.find("</GameConstants>"),
            "<ShouldDisplayPredictionPaths>" + std::string(value) + "</ShouldDisplayPredictionPaths>");
        write(path, constants);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "PL-01: presentation constant fixture loads");
        if (!loaded.tables) continue;
        const auto bound = eawr::units::motion_table(*loaded.tables);
        const bool enabled = value == "true" || value == "YES" || value == "1";
        expect(bound && bound.value().prediction_paths == enabled,
            "PL-01: absent/false skips predictions and authored true enables them");
        auto toggled = *loaded.tables;
        toggled.constants.prediction_paths = !enabled;
        expect(eawr::units::content_identity(toggled) == eawr::units::content_identity(*loaded.tables),
            "PL-01: presentation opt-in does not alter simulation content identity");
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        xml.replace(xml.rfind("</Objects>"), 10, R"xml(
<SpaceProp Name="Test_Solid_Asteroid">
  <Space_Model_Name>test_frigate.alo</Space_Model_Name>
  <Behavior>SPACE_OBSTACLE</Behavior><Is_Impassable_Asteroid>Yes</Is_Impassable_Asteroid>
  <Space_Layer>StaticObject</Space_Layer><Space_Obstacle_Radius>150</Space_Obstacle_Radius>
</SpaceProp>
<SpaceProp Name="Test_Shapeless_Asteroid">
  <Behavior>SPACE_OBSTACLE</Behavior><Is_Impassable_Asteroid>Yes</Is_Impassable_Asteroid>
  <Space_Layer>StaticObject</Space_Layer><Space_Obstacle_Radius>150</Space_Obstacle_Radius>
</SpaceProp>
</Objects>)xml");
        write_fixture(tree.root, xml);
        auto assets = models(-40.0F);
        auto& model = assets.at("data/art/models/test_frigate.alo");
        eawr::assets::Mesh box;
        box.name = "COLLISIONBOX"; box.bone = 0;
        box.bounds_min = {-75, -75, -75}; box.bounds_max = {75, 75, 75};
        model.meshes.push_back(box);
        const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
        auto filesystem = eawr::vfs::Vfs::mount(mounts);
        expect(static_cast<bool>(filesystem), "FD-10: solid-prop fixture mounts");
        if (!filesystem) return;
        auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
        expect(static_cast<bool>(catalog), "FD-10: solid-prop fixture catalog loads");
        if (!catalog) return;
        eawr::units::LoadInput input;
        input.catalog = &catalog.value().catalog; input.filesystem = &filesystem.value();
        input.types = {"Test_Frigate"};
        input.obstacles = {"Test_Solid_Asteroid", "Test_Shapeless_Asteroid"};
        input.model = [&](const std::string_view path) -> const eawr::assets::Model* {
            const auto found = assets.find(std::string(path));
            return found == assets.end() ? nullptr : &found->second;
        };
        const auto loaded = eawr::units::load_unit_tables(input);
        expect(static_cast<bool>(loaded), "FD-10/WHZ-08: solid map props load through the obstacle path");
        if (loaded) {
            std::vector<eawr::core::Diagnostic> diagnostics;
            const auto combat = eawr::units::combat_table(loaded.value(), eawr::units::ContentErrors::strict, &diagnostics);
            const auto solid_id = eawr::assets::object_type_crc("Test_Solid_Asteroid");
            const auto empty_id = eawr::assets::object_type_crc("Test_Shapeless_Asteroid");
            const auto* solid = combat ? combat.value().find_avoidance_type(solid_id) : nullptr;
            const auto* empty = combat ? combat.value().find_avoidance_type(empty_id) : nullptr;
            expect(solid && solid->model_collision_box && solid->model_collision_radius.raw() > 0
                && !combat.value().find_target_type(solid_id),
                "FD-10: solid prop model geometry binds without adding combat target admission");
            expect(empty && !empty->model_collision_box && empty->meshes.empty(),
                "FD-10: a shapeless solid prop retains the footprint fallback admission");
            expect(std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& note) {
                return note.severity == eawr::core::Severity::warning
                    && note.message.find("Test_Shapeless_Asteroid") != std::string::npos
                    && note.message.find("footprint sphere") != std::string::npos;
            }) == 0, "FD-10: combat binding does not repeat the shapeless solid prop's loader note");
            expect(std::count_if(loaded.value().notes.begin(), loaded.value().notes.end(), [](const auto& note) {
                return note.owner == "Test_Shapeless_Asteroid" && note.reason.find("footprint sphere") != std::string::npos;
            }) == 1, "FD-10: the shapeless solid prop emits one fallback note at load");
            // Hidden-only and shield-only models have no ordinary avoidance surface.
            // Failed placement and failed mesh validation must keep the same fallback profile.
            for (const int mode : {0, 1, 2, 3, 4}) {
                auto altered = loaded.value();
                auto& obstacle = *std::find_if(altered.obstacles.begin(), altered.obstacles.end(), [](const auto& value) {
                    return value.id == "Test_Solid_Asteroid";
                });
                obstacle.model_collision_box.reset();
                obstacle.scale_factor = Fixed::from_raw(Fixed::scale);
                obstacle.collision = eawr::units::CollisionBounds{
                    point(raw(-5), raw(-5), raw(-5)), point(raw(5), raw(5), raw(5))};
                eawr::units::CollisionMesh surface;
                surface.name = mode == 1 ? "shield" : "hull";
                surface.pose_visible = mode != 0;
                if (mode != 3) surface.triangles = eawr::units::IndexedCollisionTriangles(
                    {point(raw(-5), 0, 0), point(raw(5), 0, 0), point(0, raw(5), 0)}, {{0, 1, 2}});
                obstacle.collision_meshes = {std::move(surface)};
                if (mode == 2) {
                    obstacle.scale_factor = Fixed::from_raw(2 * Fixed::scale);
                    obstacle.model_collision_box = eawr::units::CollisionBounds{
                        point(0, 0, 0), point(std::numeric_limits<std::int64_t>::max(), 0, 0)};
                }
                const bool broken = mode == 2 || mode == 3;
                expect(static_cast<bool>(eawr::units::combat_table(altered)) != broken,
                    "FD-10: strict solid-prop binding refuses malformed geometry but accepts hidden and shield meshes");
                diagnostics.clear();
                const auto bound = eawr::units::combat_table(altered, eawr::units::ContentErrors::skip_object, &diagnostics);
                const auto* prop = bound ? bound.value().find_avoidance_type(solid_id) : nullptr;
                expect(prop && !bound.value().find_target_type(solid_id)
                    && !prop->model_collision_box && (prop->meshes.empty() == (mode != 4))
                    && (mode == 4 || (!prop->collision && !prop->mesh_bounds && prop->model_collision_radius.raw() == 0)),
                    "FD-10: unusable or failed solid geometry keeps the shapeless fallback; visible hulls retain their shape");
                expect(std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& note) {
                    return note.severity == eawr::core::Severity::warning
                        && note.message.find("Test_Solid_Asteroid") != std::string::npos
                        && note.message.find("footprint sphere") != std::string::npos;
                }) == (mode == 4 ? 0 : 1), "FD-10: each unusable solid-prop shape reports its footprint fallback");
            }
        }
    }
    {
        // FD-10: explicit model boxes are independent of collision masks and visibility;
        // ordinary mesh eligibility retains both its mask and reference-pose visibility.
        TempTree tree;
        write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
        auto assets = models(-40.0F);
        auto& model = assets.at("data/art/models/test_frigate.alo");
        eawr::assets::Mesh box;
        box.name = "collisionbox"; box.bone = 0; box.visible = false;
        box.bounds_min = {-2, -3, -4}; box.bounds_max = {5, 6, 7};
        model.meshes.push_back(box);
        eawr::assets::Mesh hull;
        hull.name = "hull"; hull.bone = 0; hull.collidable = true; hull.visible = false;
        hull.bounds_min = {-10, -10, -10}; hull.bounds_max = {10, 10, 10};
        eawr::assets::Submesh surface;
        surface.vertices.resize(3);
        surface.vertices[0].position = {-1, -1, 0};
        surface.vertices[1].position = {1, -1, 0};
        surface.vertices[2].position = {0, 1, 0};
        surface.indices = {0, 1, 2}; hull.submeshes.push_back(surface);
        model.meshes.push_back(hull);
        model.bones.push_back(bone("Hidden_Collision", 0, {0, 0, 0}));
        model.bones.back().visible = false;
        hull.name = "hidden-hull"; hull.visible = true;
        hull.bone = static_cast<std::int32_t>(model.bones.size() - 1); model.meshes.push_back(hull);
        hull.name = "non-collider"; hull.collidable = false; model.meshes.push_back(hull);
        const auto loaded = load(tree.root, assets);
        expect(loaded.tables.has_value(), "FD-10: authored collision model loads");
        if (loaded.tables) {
            const auto* unit = loaded.tables->find("Test_Frigate");
            expect(unit && unit->model_collision_box && unit->model_collision_box->min == point(raw(-2), raw(-3), raw(-4))
                && unit->model_collision_box->max == point(raw(5), raw(6), raw(7)),
                "FD-10: the first named model box survives despite its disabled renderable");
            expect(unit && unit->collision_meshes.size() == 2 && unit->collision_meshes[0].pose_visible
                && !unit->collision_meshes[1].pose_visible,
                "FD-10: a draw-hidden collision proxy remains eligible, but a hidden pose does not");
            const auto combat = eawr::units::combat_table(*loaded.tables);
            const auto* profile = combat ? combat.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
            expect(profile && profile->model_collision_box && profile->model_collision_radius.raw() > 0,
                "FD-10: model shape and conservative broad radius bind");
            if (profile && profile->model_collision_box) {
                const auto scale = unit->scale_factor.value();
                const auto scaled = [&](const int value) { return eawr::sim::math::multiply(Fixed::from_raw(raw(value)), scale).value(); };
                expect(profile->model_collision_box->min == Vec3{scaled(-6), scaled(-2), scaled(-4)}
                    && profile->model_collision_box->max == Vec3{scaled(3), scaled(5), scaled(7)},
                    "FD-10: authored type scale and the model-axis turn apply to the model box");
                expect(profile->meshes.size() == 2 && profile->meshes[0].pose_visible && !profile->meshes[1].pose_visible,
                    "FD-10: immutable placed geometry retains hidden mesh eligibility");
            }
            auto changed = *loaded.tables;
            auto* edit = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
            edit->collision_meshes[1].pose_visible = true;
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                "FD-10: collision visibility participates in content identity");
            changed = *loaded.tables;
            edit = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
            edit->model_collision_box.reset();
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                "FD-10: explicit model boxes participate in content identity");
        }
    }
    // #5704 / FD-10: oversized named boxes must not drop a type during a lenient load,
    // including a box-only model with no collidable mesh.
    for (const bool collidable : {false, true}) {
        TempTree tree;
        write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
        auto assets = models(-40.0F);
        auto& model = assets.at("data/art/models/test_frigate.alo");
        model.meshes.clear();
        eawr::assets::Mesh box;
        box.name = "COLLISIONBOX"; box.bone = 0; box.collidable = collidable;
        box.bounds_min = {-500000, -500000, -500000};
        box.bounds_max = {500000, 500000, 500000};
        model.meshes.push_back(box);
        const auto loaded = load(tree.root, assets);
        expect(loaded.tables.has_value(), "FD-10: oversized model box loads into the source unit tables");
        if (!loaded.tables) continue;
        const auto* unit = loaded.tables->find("Test_Frigate");
        expect(unit && unit->model_collision_box && unit->collision.has_value() == collidable,
            "FD-10: the oversized named-box control covers both box-only and collidable models");
        expect(!eawr::units::combat_table(*loaded.tables), "FD-10: strict loading refuses an oversized named box");
        std::vector<eawr::core::Diagnostic> notes;
        const auto combat = eawr::units::combat_table(*loaded.tables, eawr::units::ContentErrors::skip_object, &notes);
        const auto* profile = combat ? combat.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        expect(profile && !profile->collision && !profile->model_collision_box && profile->model_collision_radius.raw() == 0
            && profile->meshes.empty() && !profile->mesh_bounds,
            "FD-10: lenient loading keeps the type while clearing every oversized collision shape");
        expect(std::any_of(notes.begin(), notes.end(), [](const auto& note) {
            return note.severity == eawr::core::Severity::warning
                && note.message.find("collision box exceeds the supported combat domain") != std::string::npos;
        }), "FD-10: oversized named boxes retain the existing collision warning");
    }
    // WCC-41: vary the authored scalar and behavior through the real XML loader.
    for (const std::string_view authored : {"2", "3.5", "0", "-1", "300", "", "invalid"}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto start = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        xml.insert(xml.find("</SpaceUnit>", start),
            "<SpaceBehavior>TACTICAL_UNDER_CONSTRUCTION</SpaceBehavior><Tactical_Buildable_Constructed>Test_Base</Tactical_Buildable_Constructed>");
        write_fixture(tree.root, xml);
        const auto path = tree.root / "XML" / "GameConstants.xml";
        std::ifstream input(path);
        std::string constants{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        input.close();
        if (!authored.empty()) constants.insert(constants.find("</GameConstants>"),
            "<Under_Construction_Damage_Multiplier>" + std::string(authored) + "</Under_Construction_Damage_Multiplier>");
        write(path, constants);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WCC-41: construction override fixture loads");
        if (!loaded.tables) continue;
        eawr::core::LoadNoteBuffer notes;
        const auto bound = eawr::units::durability_table(*loaded.tables, nullptr, &notes);
        expect(bound && bound.value().damage, "WCC-41: construction damage rules bind");
        if (!bound || !bound.value().damage) continue;
        if (authored.empty() || authored == "invalid" || authored == "-1" || authored == "300") {
            eawr::core::LoadNoteCollector collector;
            collector.merge(notes);
            const auto reported = collector.snapshot();
            expect(std::any_of(reported.begin(), reported.end(), [](const auto& note) {
                return note.tag == "Under_Construction_Damage_Multiplier"
                    && note.classification == eawr::core::LoadCategory::unknown_degrade;
            }), "WCC-41: fallback or arithmetic clamping reports a project degradation");
        }
        const auto& rules = *bound.value().damage;
        const auto* source = bound.value().find(eawr::assets::object_type_crc("Test_Frigate"));
        expect(source != nullptr, "WCC-41: construction recipient retains its durability profile");
        if (!source) continue;
        auto profile = *source;
        profile.max_shields = {};
        profile.destroyed_with_hardpoints = false;
        const auto multiplier = Fixed::from_decimal(authored.empty() || authored == "invalid" ? "1"
            : authored == "-1" ? "0" : authored == "300" ? "256" : authored).value();
        auto state = eawr::sim::tactical::full_durability(profile);
        eawr::sim::tactical::Hit hit;
        hit.amount = Fixed::from_raw(raw(10));
        const auto applied = eawr::sim::tactical::apply_hit(profile, rules, state, hit, 1);
        expect(applied && state.hull.raw() == profile.max_hull.raw() - eawr::sim::math::multiply(hit.amount, multiplier).value().raw(),
            "WCC-41: XML multiplier changes actual damage to construction behavior");
        profile.type_id = eawr::assets::object_type_crc("Test_Base");
        state = eawr::sim::tactical::full_durability(profile);
        const auto ordinary = eawr::sim::tactical::apply_hit(profile, rules, state, hit, 1);
        expect(ordinary && state.hull.raw() == profile.max_hull.raw() - hit.amount.raw(),
            "WCC-41: completed structure does not inherit construction scaling");
    }
    // AB-70: XML-authored ion filters survive both ordinary and Create_Team_Type binding.
    for (const std::string_view style : {"User_Input", "Space_Automatic", "Ground_Automatic", "Special_Attack", ""}) {
        for (const std::string_view category : {"Frigate", "Fighter", ""}) {
            TempTree tree;
            auto xml = units_xml("3600", "Unit_Select_Test");
            const std::string binding =
                "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>ION_CANNON_SHOT</Type>"
                "<GUI_Activated_Ability_Name>Mixed_Case_Ion</GUI_Activated_Ability_Name>"
                "<Supports_Autofire>True</Supports_Autofire></Unit_Ability></Unit_Abilities_Data>"
                "<Abilities SubObjectList=\"Yes\"><Ion_Cannon_Shot_Attack_Ability Name=\"Mixed_Case_Ion\">"
                + (style.empty() ? std::string{} : "<Activation_Style>" + std::string(style) + "</Activation_Style>")
                + (category.empty() ? std::string{} : "<Applicable_Unit_Categories>" + std::string(category)
                    + "</Applicable_Unit_Categories>")
                + "<Applicable_Unit_Types>Test_Fighter</Applicable_Unit_Types>"
                "<Excluded_Unit_Categories>Bomber</Excluded_Unit_Categories>"
                "<Excluded_Unit_Types>Test_Frigate</Excluded_Unit_Types>"
                "</Ion_Cannon_Shot_Attack_Ability></Abilities>";
            const auto frigate = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
            const auto ordinary = xml.find("<Unit_Abilities_Data", frigate);
            const auto end = xml.find("</Unit_Abilities_Data>", ordinary);
            expect(ordinary != std::string::npos && end != std::string::npos, "AB-70: replace the original ordinary row");
            if (ordinary != std::string::npos && end != std::string::npos)
                xml.erase(ordinary, end + std::string_view{"</Unit_Abilities_Data>"}.size() - ordinary);
            for (const std::string_view start : {"<Container Name=\"Test_Pair_Base_Container\">",
                    "<SpaceUnit Name=\"Test_Frigate\">"}) {
                const auto at = xml.find(start);
                expect(at != std::string::npos, "AB-70: fixture insertion point exists");
                if (at != std::string::npos) xml.insert(at + start.size(), binding);
            }
            write_fixture(tree.root, xml);
            const auto loaded = load(tree.root, models(-40.0F));
            expect(loaded.tables.has_value(), "AB-70: authored nested ion XML loads");
            if (!loaded.tables) continue;
            const auto compiled = eawr::units::ability_table(*loaded.tables);
            expect(static_cast<bool>(compiled), "AB-70: nested ion profiles compile");
            if (!compiled) continue;
            for (const std::string_view id : {"Test_Frigate", "Test_Squadron_Pair"}) {
                const auto* profile = compiled.value().find(eawr::assets::object_type_crc(id));
                const auto slot = profile ? eawr::sim::tactical::ability_slot(*profile,
                    eawr::sim::tactical::AbilityKind::ion_cannon_shot) : std::nullopt;
                expect(profile && slot && profile->special.size() == 1, "AB-70: team and unit retain a named ion handler");
                if (!profile || !slot || profile->special.size() != 1) continue;
                const auto& handler = profile->special.front();
                const std::uint64_t mask = category == "Frigate" ? 0x10U : category == "Fighter" ? 1U : 0U;
                expect(profile->abilities[*slot].gui_activated_ability_name == "Mixed_Case_Ion"
                    && handler.name == "Mixed_Case_Ion"
                    && handler.kind == eawr::sim::tactical::special_ability_kind("Ion_Cannon_Shot_Attack_Ability")
                    && handler.style == eawr::sim::tactical::special_activation_style(style.empty() ? "Special_Attack" : style)
                    && handler.filter.applicable_categories == mask
                    && handler.filter.applicable_types == std::vector<eawr::sim::tactical::TypeId>{eawr::assets::object_type_crc("Test_Fighter")}
                    && handler.filter.excluded_categories == 2
                    && handler.filter.excluded_types == std::vector<eawr::sim::tactical::TypeId>{eawr::assets::object_type_crc("Test_Frigate")},
                    "AB-70: authored style, default empty categories, names and exact filters survive the team loader");
            }
        }
    }
    for (const std::string_view starting : {"", "<Starting_Spawned_Units_Tech_0></Starting_Spawned_Units_Tech_0>"}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string tag = "<Starting_Spawned_Units_Tech_0>Test_Squadron, 2</Starting_Spawned_Units_Tech_0>";
        const auto at = xml.find(tag);
        const auto base = xml.find("<StarBase Name=\"Test_Base\">");
        if (at != std::string::npos) xml.replace(at, tag.size(), starting);
        if (base != std::string::npos) xml.insert(base + std::string_view{"<StarBase Name=\"Test_Base\">"}.size(),
            "<Behavior>DUMMY_STAR_BASE</Behavior>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        if (!loaded.tables) {
            expect(false, "WSL-26: empty authored station hangar loads");
            continue;
        }
        const auto* station = loaded.tables->find("Skirmish_Test_Base");
        const auto motion = eawr::units::motion_table(*loaded.tables);
        const auto id = eawr::assets::object_type_crc("Skirmish_Test_Base");
        bool empty = at != std::string::npos && base != std::string::npos && station && station->spawner
            && station->spawner->starting.empty() && motion;
        for (unsigned tech = 0; tech < 6 && empty; ++tech) {
            const auto* profile = motion.value().squadrons.find_spawner_at_tech(id, 1, tech);
            empty = profile && profile->starbase && !profile->bays.empty() && profile->entries.empty();
        }
        expect(empty, "WSL-26: omitted or empty XML starting garrison binds an empty station hangar at every tech");
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string tag = "<Starting_Spawned_Units_Tech_0>Test_Squadron, 2</Starting_Spawned_Units_Tech_0>";
        const auto at = xml.find(tag);
        if (at != std::string::npos) xml.insert(at + tag.size(),
            "<Starting_Spawned_Units_Tech_3>Test_Squadron_Pair, 4</Starting_Spawned_Units_Tech_3>"
            "<Reserve_Spawned_Units_Tech_2>Test_Squadron_Pair, 7</Reserve_Spawned_Units_Tech_2>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* station = loaded.tables ? loaded.tables->find("Skirmish_Test_Base") : nullptr;
        expect(at != std::string::npos && station && station->spawner
            && station->spawner->campaign_starting[3].size() == 1
            && station->spawner->campaign_starting[3][0].squadron == "Test_Squadron_Pair"
            && station->spawner->campaign_starting[3][0].count == 4
            && station->spawner->campaign_reserves[2].size() == 1
            && station->spawner->campaign_reserves[2][0].count == 7,
            "WSL-11: skirmish loading retains higher starting and reserve tiers independently");
        if (loaded.tables) {
            const auto motion = eawr::units::motion_table(*loaded.tables);
            const auto id = eawr::assets::object_type_crc("Skirmish_Test_Base");
            const auto* low = motion ? motion.value().squadrons.find_spawner_at_tech(id, 1, 1) : nullptr;
            const auto* high = motion ? motion.value().squadrons.find_spawner_at_tech(id, 1, 4) : nullptr;
            expect(low && low->entries.size() == 1 && low->entries[0].starting == 2
                && low->entries[0].reserve == -1,
                "WSL-11: tech 1 falls back to both tech 0 rows");
            expect(high && high->entries.size() == 1 && high->entries[0].squadron == eawr::assets::object_type_crc("Test_Squadron_Pair")
                && high->entries[0].starting == 4 && high->entries[0].reserve == 7,
                "WSL-11: tech 4 selects starting tier 3 and reserve tier 2 independently");
        }
    }
    {
        // A mobile carrier with no Tech_0 row must still bind and launch its Tech_1 complement.
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto replace = [&](const std::string& from, const std::string& to) {
            const auto at = xml.find(from);
            if (at == std::string::npos) return false;
            xml.replace(at, from.size(), to);
            return true;
        };
        const bool authored = replace("<Space_Model_Name>TEST_FRIGATE.ALO</Space_Model_Name>",
                "<Space_Model_Name>Test_Station.alo</Space_Model_Name>")
            && replace("<SpaceBehavior>TARGETING,REVEAL , HIDE_WHEN_FOGGED, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>",
                "<SpaceBehavior>SPAWN_SQUADRON, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>"
                "<Starting_Spawned_Units_Tech_1>Test_Squadron, 3</Starting_Spawned_Units_Tech_1>"
                "<Starting_Spawned_Units_Tech_1>Test_Squadron_Pair, 2</Starting_Spawned_Units_Tech_1>"
                "<Spawned_Squadron_Delay_Seconds>0</Spawned_Squadron_Delay_Seconds>")
            && replace("<HardPoints> HP_Test_Laser, HP_Test_Engine, HP_Test_Missing </HardPoints>",
                "<HardPoints>HP_Test_Bay</HardPoints>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* carrier = loaded.tables ? loaded.tables->find("Test_Frigate") : nullptr;
        const auto id = eawr::assets::object_type_crc("Test_Frigate");
        const auto fighter = eawr::assets::object_type_crc("Test_Squadron");
        const auto bomber = eawr::assets::object_type_crc("Test_Squadron_Pair");
        expect(authored && carrier && carrier->spawner && carrier->spawner->starting.empty()
            && carrier->spawner->campaign_starting[1].size() == 2,
            "WSL-11: mobile carrier loads Tech_1-only starting rows");
        if (loaded.tables) {
            const auto motion = eawr::units::motion_table(*loaded.tables);
            const auto* profile = motion ? motion.value().squadrons.find_spawner_at_tech(id, 1, 1) : nullptr;
            expect(profile && profile->mobile && !profile->bays.empty() && profile->entries.size() == 2
                && profile->entries[0].squadron == fighter && profile->entries[0].starting == 3
                && profile->entries[1].squadron == bomber && profile->entries[1].starting == 2,
                "WSL-11: Tech_1-only carrier binds its complete mobile spawner");
            const std::array<unsigned, 2> higher_techs{2, 3};
            expect(std::all_of(higher_techs.begin(), higher_techs.end(), [&](const unsigned tech) {
                const auto* higher = motion ? motion.value().squadrons.find_spawner_at_tech(id, 1, tech) : nullptr;
                return higher && higher->mobile && !higher->bays.empty() && higher->entries.size() == 2
                    && higher->entries[0].squadron == fighter && higher->entries[0].starting == 3
                    && higher->entries[1].squadron == bomber && higher->entries[1].starting == 2;
            }), "WSL-11: tech 2 and 3 fall back to the complete Tech_1-only mobile carrier complement");
            if (profile && profile->entries.size() == 2) {
                namespace tactical = eawr::sim::tactical;
                tactical::TacticalSetup setup;
                setup.seed = 540;
                setup.players = {{1, 0, 100, 1}, {2, 1, 200, 1}};
                setup.units = {{1, id, 1, {}, eawr::sim::math::identity_quat(), {}}};
                tactical::EconomyRules economy;
                economy.players = {{1, {}, 25, false, {}, 1, 5}, {2, {}, 25, false, {}, 1, 5}};
                auto session = tactical::TacticalSession::create(setup, {}, {}, motion.value(), std::nullopt, {}, {}, {}, economy);
                expect(static_cast<bool>(session), "WSL-11: Tech_1-only carrier session creates");
                if (session) {
                    const eawr::sim::InlineExecutor executor;
                    bool stepped = true;
                    for (unsigned tick = 0; tick < 180 && stepped; ++tick)
                        stepped = static_cast<bool>(session.value().step(executor));
                    unsigned fighters = 0, bombers = 0;
                    for (const auto& squadron : session.value().squadrons()) {
                        const auto state = session.value().squadron_state(squadron.container);
                        if (!state || state->spawner != 1) continue;
                        fighters += state->squadron_type == fighter ? 1U : 0U;
                        bombers += state->squadron_type == bomber ? 1U : 0U;
                    }
                    expect(stepped && fighters == 3 && bombers == 2,
                        "WSL-11: Tech_1-only carrier launches three and two starting squadrons");
                }
            }
        }
    }
    // FL-05: class names do not bypass the selected space body's spawn-delay consumer.
    for (const std::string_view xml_class : {"DummyStructure", "HeroUnit", "Mission_Object", "Props_Wave",
            "SkirmishSpaceUnit", "SpaceBuildable", "SpaceHero", "SpaceProp", "Space_Unit",
            "SurvivalSpaceUnit", "Template", "TransportUnit", "UnderConstructionShip"}) {
        struct DelayCase {
            std::string_view override_seconds;
            std::uint32_t frames{};
            std::uint64_t second_launch{};
        };
        for (const auto& delay : std::array<DelayCase, 3>{{{"", 300, 300}, {"4.5", 135, 150}, {"0", 0, 30}}}) {
            TempTree tree;
            auto xml = units_xml("3600", "Unit_Select_Test");
            std::string carrier = "<" + std::string(xml_class) + " Name=\"Delay_Carrier\">"
                "<Variant_Of_Existing_Type>Test_Base</Variant_Of_Existing_Type>";
            // WHE-SQ-02: a hero without its own space locomotor is a carried identity with no body; give the
            // hero carrier a locomotor so it is the deployable space body this case qualifies.
            if (xml_class == "HeroUnit") carrier += "<SpaceBehavior>SIMPLE_SPACE_LOCOMOTOR, SPAWN_SQUADRON</SpaceBehavior>";
            if (!delay.override_seconds.empty()) carrier += "<Spawned_Squadron_Delay_Seconds>"
                + std::string(delay.override_seconds) + "</Spawned_Squadron_Delay_Seconds>";
            carrier += "</" + std::string(xml_class) + ">";
            xml.insert(xml.rfind("</Objects>"), carrier);
            write_fixture(tree.root, xml);
            const auto loaded = load(tree.root, models(-40.0F), "Normal_Default",
                eawr::sim::tactical::BattleDomain::space, {"Delay_Carrier"});
            const auto* unit = loaded.tables ? loaded.tables->find("Delay_Carrier") : nullptr;
            // SG-CLS-01: an element name that is no FoC class settles its class from the variant chain, so the
            // loaded class equals the authored element name only for names the registry knows.
            const bool known_class = eawr::data::is_foc_object_class(xml_class);
            const std::string detail = " [" + std::string(xml_class) + " type="
                + (unit ? unit->xml_type : std::string("none")) + " spawner=" + (unit && unit->spawner ? "yes" : "no")
                + " delay=" + (unit && unit->spawner && unit->spawner->delay_seconds ? "set" : "unset") + "]";
            expect(unit && (!known_class || unit->xml_type == xml_class) && unit->spawner && unit->spawner->delay_seconds,
                "FL-05: selected space spawner loads its inherited or overridden delay for each XML class" + detail);
            if (!loaded.tables) continue;
            const auto motion = eawr::units::motion_table(*loaded.tables);
            const auto* profile = motion ? motion.value().squadrons.find_spawner_at_tech(
                eawr::assets::object_type_crc("Delay_Carrier"), 1, 1) : nullptr;
            expect(profile && profile->delay_frames == delay.frames && profile->entries.size() == 1
                && profile->entries.front().starting == 2 && !profile->bays.empty(),
                "FL-05: inherited ten seconds, fractional override and explicit zero reach the bound hangar" + detail
                    + (profile ? " frames=" + std::to_string(profile->delay_frames) + " entries="
                        + std::to_string(profile->entries.size()) + " bays=" + std::to_string(profile->bays.size())
                        : std::string(" no-profile")));
            if (!profile || profile->entries.size() != 1 || profile->bays.empty()) continue;
            namespace tactical = eawr::sim::tactical;
            tactical::SpawnerState state;
            const std::vector<bool> intact(profile->bays.size(), true);
            std::vector<std::uint64_t> launches;
            for (std::uint64_t tick = 0; tick <= 300; ++tick) {
                if (tactical::service_spawner(*profile, state, 540, tick, 1, intact)) launches.push_back(tick);
            }
            expect(launches == std::vector<std::uint64_t>{0, delay.second_launch},
                "FL-01/05: the bound delay spaces launches on the first due thirty-frame service, including equality");
        }
    }
    // HD-06: omission, zero and malformed Health retain every authored hardpoint.
    for (const std::string_view value : {"", "<Health></Health>", "<Health>   </Health>",
            "<Health>0</Health>", "<Health>-1</Health>",
            "<Health>1e99</Health>", "<Health>broken</Health>", "<Health>42junk</Health>",
            // The Remake Viscount shield hardpoints: an invisible U+202C before ".0" (AU-82: leading number kept).
            "<Health>14500\xE2\x80\xAC.0</Health>",
            // A fractional leading number keeps its fraction (not truncated to "0" / "14500").
            "<Health>0.5x</Health>", "<Health>14500.5x</Health>"}) {
        TempTree tree;
        write_fixture(tree.root, units_xml("3600", "Unit_Select_Test"));
        auto points = std::string(hardpoints_xml);
        const std::string_view laser_health = "<Health>260.0</Health>";
        const std::string_view engine_health = "<Health>170.0</Health>";
        points.replace(points.find(laser_health), laser_health.size(), value);
        points.replace(points.find(engine_health), engine_health.size(), value);
        write(tree.root / "XML" / "eawr_hardpoints.xml", points);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "HD-06: hardpoint Health fixture loads");
        if (!loaded.tables) continue;
        if (value.empty()) {
            auto missing = *loaded.tables;
            auto& scalars = missing.constants.scalars;
            scalars.erase(std::remove_if(scalars.begin(), scalars.end(), [](const auto& scalar) {
                return scalar.tag == "ShieldRechargeIntervalInSecs";
            }), scalars.end());
            eawr::core::LoadNoteBuffer load_notes;
            auto recovered = eawr::units::durability_table(missing, nullptr, &load_notes);
            eawr::core::LoadNoteCollector collector; collector.merge(load_notes);
            const auto captured = collector.snapshot();
            expect(recovered && recovered.value().damage && recovered.value().damage->shield_recharge_frames == 90,
                "missing shield recharge interval uses stock three seconds");
            expect(std::any_of(captured.begin(), captured.end(), [](const auto& note) {
                return note.code == "EAWR-LOAD-0120" && note.tag == "ShieldRechargeIntervalInSecs"
                    && note.action == eawr::core::LoadAction::default_used
                    && note.classification == eawr::core::LoadCategory::retail_accepts && note.retail == "LSA-P5";
            }), "missing shield recharge interval records a default note");
            if (recovered) {
                eawr::sim::tactical::TacticalSetup setup;
                setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
                    {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
                auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, recovered.value());
                expect(static_cast<bool>(session), "stock fallback session creates");
                if (session) {
                    const eawr::sim::InlineExecutor executor;
                    expect(static_cast<bool>(session.value().step(executor)), "stock fallback session steps");
                }
            }
            auto extreme = *loaded.tables;
            for (auto& unit : extreme.units) {
                if (unit.hull && unit.hull->raw() > 0) {
                    unit.hull = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
                    break;
                }
            }
            for (auto& constant : extreme.constants.scalars) {
                if (constant.tag == "ShieldRechargeIntervalInSecs") {
                    constant.value = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
                    constant.text = "549755813887";
                }
            }
            eawr::core::LoadNoteBuffer extreme_notes;
            const auto bounded = eawr::units::durability_table(extreme, nullptr, &extreme_notes);
            eawr::core::LoadNoteCollector extreme_collector;
            extreme_collector.merge(extreme_notes);
            const auto extreme_captured = extreme_collector.snapshot();
            expect(static_cast<bool>(bounded), "extreme health and recharge load");
            expect(std::any_of(extreme_captured.begin(), extreme_captured.end(), [](const auto& note) {
                return note.code == "EAWR-LOAD-0121" && note.tag == "Tactical_Health";
            }), "health multiplication saturation records its tag");
            expect(std::any_of(extreme_captured.begin(), extreme_captured.end(), [](const auto& note) {
                return note.code == "EAWR-LOAD-0122" && note.authored_value == "549755813887";
            }), "service interval saturation records authored text");

        }
        std::vector<eawr::core::Diagnostic> notes;
        eawr::core::LoadNoteBuffer load_notes;
        const auto health = eawr::units::durability_table(*loaded.tables, &notes, &load_notes);
        const bool malformed = value == "<Health>1e99</Health>" || value == "<Health>broken</Health>"
            || value == "<Health>42junk</Health>" || value == "<Health>14500\xE2\x80\xAC.0</Health>"
            || value == "<Health>0.5x</Health>" || value == "<Health>14500.5x</Health>";
        // AU-82: trailing text after a leading number keeps the number; every other malformed value is zero.
        const bool keeps_number = value == "<Health>42junk</Health>" || value == "<Health>14500\xE2\x80\xAC.0</Health>"
            || value == "<Health>0.5x</Health>" || value == "<Health>14500.5x</Health>";
        const std::string_view expected_effective = value == "<Health>0.5x</Health>" ? "0.5"
            : value == "<Health>14500.5x</Health>" ? "14500.5" : value.starts_with("<Health>42") ? "42"
            : keeps_number ? "14500" : "0";
        eawr::core::LoadNoteCollector collector;
        collector.merge(load_notes);
        const auto captured = collector.snapshot();
        const auto recovered = std::count_if(captured.begin(), captured.end(), [](const auto& note) {
            return note.code == "EAWR-LOAD-0125";
        });
        expect(recovered == (malformed ? 2 : 0), "HD-06: only malformed authored Health emits a recovery LoadNote");
        if (malformed) {
            const auto authored = value.substr(std::string_view{"<Health>"}.size(),
                value.size() - std::string_view{"<Health></Health>"}.size());
            expect(std::all_of(captured.begin(), captured.end(), [&](const auto& note) {
                return note.code != "EAWR-LOAD-0125" || (note.diagnostic_code == "EAWR-LOAD-0125"
                    && note.tag == "Health" && note.authored_value == authored
                    && note.action == eawr::core::LoadAction::default_used
                    && note.effective_value == expected_effective
                    && (note.message.find("trailing text") != std::string::npos) == keeps_number
                    && note.classification == eawr::core::LoadCategory::unknown_degrade
                    && note.file == "data/xml/eawr_hardpoints.xml" && note.line && *note.line > 1
                    && note.layer.mod_id == "base" && note.object.starts_with("Test_Frigate/"));
            }), "HD-06: recovery notes retain authored text, object and source provenance");
            expect(!has_row(loaded.tables->unresolved, "HP_Test_Laser", "Health")
                && !has_row(loaded.tables->unresolved, "HP_Test_Engine", "Health"),
                "HD-06: accepted malformed Health is not an unresolved input");
            auto diagnostics_cleared = *loaded.tables;
            diagnostics_cleared.hardpoint_health_defaults.clear();
            expect(eawr::units::content_identity(diagnostics_cleared) == eawr::units::content_identity(*loaded.tables),
                "HD-06: recovery provenance is outside replay content identity");
        }
        expect(static_cast<bool>(health), "HD-06: omitted, zero, negative and malformed Health bind durability");
        if (!health) continue;
        const auto* ship = health.value().find(eawr::assets::object_type_crc("Test_Frigate"));
        expect(ship && ship->hardpoints.size() == 3 && ship->hardpoints[0].destroyable
            && ship->hardpoints[0].role == eawr::sim::tactical::HardpointRole::weapon
            && ship->hardpoints[1].destroyable
            && ship->hardpoints[1].role == eawr::sim::tactical::HardpointRole::engine
            && (ship->hardpoints[0].max_health.raw() == 0) == !keeps_number
            && (ship->hardpoints[1].max_health.raw() == 0) == !keeps_number,
            "HD-06: zero-health rows retain their flags, roles and authored order");
        if (keeps_number) {
            expect(notes.empty(), "AU-82: a Health that keeps its leading number is not a zero-health type");
            continue;
        }
        if (value == "<Health>-1</Health>") continue;
        expect(notes.size() == 1 && notes[0].code == "EAWR-UNITS-0003"
            && notes[0].severity == eawr::core::Severity::warning
            && notes[0].message.find("Test_Frigate") != std::string::npos,
            "HD-06: two zero-health hardpoints produce one load note for their type");
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string_view token = "HP_Test_Laser, HP_Test_Engine";
        xml.replace(xml.find(token), token.size(), "HP_Test_Laser, HP_Test_Laser, HP_Test_Engine");
        write_fixture(tree.root, xml);
        auto points = std::string(hardpoints_xml);
        const std::string_view health = "<Health>260.0</Health>";
        points.replace(points.find(health), health.size(), "<Health>broken</Health>");
        write(tree.root / "XML" / "eawr_hardpoints.xml", points);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables && loaded.tables->hardpoint_health_defaults.size() == 1,
            "HD-06: repeated hardpoint references retain one recovery provenance record");
        if (loaded.tables) {
            auto tables = *loaded.tables;
            constexpr std::string_view loser = "SYNTH_OIVRHNPFF4K4";
            constexpr std::string_view winner = "SYNTH_QLU1MOI5PTT3";
            expect(eawr::assets::object_type_crc(loser) == eawr::assets::object_type_crc(winner),
                "HD-06: dropped Health fixture uses colliding type IDs");
            const auto source = std::find_if(tables.units.begin(), tables.units.end(), [](const auto& unit) {
                return unit.id == "Test_Frigate";
            });
            expect(source != tables.units.end(), "HD-06: dropped Health fixture has its source unit");
            if (source != tables.units.end()) {
                source->id = loser;
                source->table_order = 9;
                auto selected = *source;
                selected.id = winner;
                selected.table_order = 2;
                selected.hardpoints.clear();
                tables.units.push_back(std::move(selected));
                for (auto& recovery : tables.hardpoint_health_defaults) {
                    if (recovery.object == "Test_Frigate") recovery.object = loser;
                }
                eawr::core::LoadNoteBuffer load_notes;
                const auto selected_health = eawr::units::durability_table(tables, nullptr, &load_notes);
                eawr::core::LoadNoteCollector collector;
                collector.merge(load_notes);
                const auto captured = collector.snapshot();
                expect(selected_health && selected_health.value().find(eawr::assets::object_type_crc(winner)),
                    "HD-06: the collision winner still binds durability");
                expect(std::any_of(captured.begin(), captured.end(), [&](const auto& note) {
                    return note.code == "EAWR-LOAD-0251" && note.object == loser;
                }), "HD-06: the malformed Health unit is dropped by type selection");
                expect(std::none_of(captured.begin(), captured.end(), [](const auto& note) {
                    return note.code == "EAWR-LOAD-0125";
                }), "HD-06: dropped malformed Health emits no recovery note");
            }
        }
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto opening = std::string_view{"<SpaceUnit Name=\"Test_Frigate\">"};
        const auto begin = xml.find(opening);
        const auto end = xml.find("</SpaceUnit>", begin);
        xml.replace(end, std::string_view{"</SpaceUnit>"}.size(), "</SecondaryStructure>");
        xml.replace(begin, opening.size(), "<SecondaryStructure Name=\"Test_Frigate\"><Base_Level>4</Base_Level>");
        const auto behavior = std::string_view{"<Behavior>SELECTABLE</Behavior>"};
        xml.replace(xml.find(behavior), behavior.size(),
            "<Behavior>SELECTABLE, DUMMY_STAR_BASE, CAPTURE_POINT, UNIT_AI</Behavior>");
        const auto locomotion = std::string_view{"SIMPLE_SPACE_LOCOMOTOR"};
        xml.replace(xml.find(locomotion), locomotion.size(), "SPACE_OBSTACLE");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* depot = loaded.tables ? loaded.tables->find("Test_Frigate") : nullptr;
        expect(depot && depot->xml_type == "SecondaryStructure" && depot->kind != UnitKind::station && depot->star_base
            && depot->capture_point && depot->unit_ai && depot->base_level == 4,
            "SAE-02: a capturable secondary structure carries its authored station behavior independently of class");
        if (loaded.tables) {
            const auto setup = eawr::skirmish::ai_setup({}, {}, *loaded.tables);
            const auto entry = std::find_if(setup.content.types.begin(), setup.content.types.end(),
                [](const auto& type) { return type.name == "TEST_FRIGATE"; });
            expect(entry != setup.content.types.end() && entry->star_base && entry->capture_point
                && entry->unit_ai && entry->base_level == 4 && !entry->locomotor,
                "SAE-02: the shared AI builder retains the secondary structure's station and capture roles");
        }
    }
    for (const int mode : {0, 1, 2}) {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto opening = std::string_view{"<StarBase Name=\"Test_Base\">"};
        xml.insert(xml.find(opening) + opening.size(),
            "<Behavior>DUMMY_STAR_BASE</Behavior><Base_Level>3</Base_Level>"
            "<Prev_Level_Base>Metadata_Base_Two</Prev_Level_Base>");
        const std::string last = mode == 1 ? "Missing_Base" : mode == 2 ? "Metadata_Base_Two" : "Metadata_Base_One";
        xml.insert(xml.rfind("</Objects>"),
            "<StarBase Name=\"Metadata_Base_Two\"><Base_Level>2</Base_Level><Prev_Level_Base>" + last +
            "</Prev_Level_Base></StarBase><StarBase Name=\"Metadata_Base_One\"><Base_Level>1</Base_Level></StarBase>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        const auto* station = loaded.tables ? loaded.tables->find("Skirmish_Test_Base") : nullptr;
        expect(station && station->previous_base_levels == (mode == 0
            ? std::vector<std::uint32_t>{2, 1} : std::vector<std::uint32_t>{2}),
            "SAE-02: station predecessor levels follow authored references and inherited tags");
        expect(loaded.tables && loaded.tables->find("Metadata_Base_Two") == nullptr
            && loaded.tables->find("Metadata_Base_One") == nullptr,
            "SAE-02: predecessor metadata does not add physical unit or model entries");
        if (loaded.tables) {
            const auto setup = eawr::skirmish::ai_setup({}, {}, *loaded.tables);
            const auto entry = std::find_if(setup.content.types.begin(), setup.content.types.end(),
                [](const auto& type) { return type.name == "SKIRMISH_TEST_BASE"; });
            expect(entry != setup.content.types.end() && entry->star_base
                && entry->previous_base_levels == station->previous_base_levels,
                "SAE-02: the shared tactical builder carries the authored station role and predecessor chain");
            auto changed = *loaded.tables;
            for (auto& unit : changed.units) unit.previous_base_levels.clear();
            expect(eawr::units::content_identity(changed) == eawr::units::content_identity(*loaded.tables),
                "SAE-02: AI predecessor metadata leaves physical replay content identity unchanged");
            for (auto& unit : changed.units) unit.star_base = false;
            const auto unqualified = eawr::skirmish::ai_setup({}, {}, changed);
            expect(std::none_of(unqualified.content.types.begin(), unqualified.content.types.end(),
                [](const auto& type) { return type.star_base; }),
                "SAE-02: a station class alone does not invent the authored starbase behavior");
        }
        if (mode != 0) expect(loaded.tables && has_row(loaded.tables->unresolved,
            "Skirmish_Test_Base", "Prev_Level_Base", last),
            "SAE-02: unresolved and cyclic station predecessors retain a diagnostic");
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto begin = xml.find("<Unit_Abilities_Data");
        const auto end = xml.find("</Unit_Abilities_Data>", begin) + std::string_view{"</Unit_Abilities_Data>"}.size();
        xml.replace(begin, end - begin, R"xml(<Unit_Abilities_Data SubObjectList="Yes"><Unit_Ability>
            <Type>TURBO</Type><Expiration_Seconds>10</Expiration_Seconds><Recharge_Seconds>40</Recharge_Seconds>
            <Mod_Multiplier>UNKNOWN_MULTIPLIER, 900</Mod_Multiplier>
            <Mod_Multiplier>speed_multiplier, 2</Mod_Multiplier>
            <Mod_Multiplier>target_speed_multiplier, 0.5</Mod_Multiplier>
            <Mod_Multiplier>TARGET_TAKE_DAMAGE_MULTIPLIER, 3</Mod_Multiplier>
            <Mod_Flag>weapons_enabled_flag, false</Mod_Flag>
            <Mod_Flag>CAN_BE_SQUASHED, true</Mod_Flag>
            <Mod_Flag>ALLOW_TARGET_DIMINISHING_FIREPOWER, false</Mod_Flag>
            <Mod_Flag>UNKNOWN_FLAG, true</Mod_Flag>
            <Mod_Flag>CAN_BE_SQUASHED, false</Mod_Flag>
            </Unit_Ability></Unit_Abilities_Data>)xml");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "AB-26: mixed modifier data loads");
        if (loaded.tables) {
            const auto& tables = *loaded.tables;
            expect(has_row(tables.notes, "Test_Frigate", "Mod_Multiplier", "UNKNOWN_MULTIPLIER, 900")
                && has_row(tables.notes, "Test_Frigate", "Mod_Flag", "UNKNOWN_FLAG, true")
                && has_row(tables.notes, "Test_Frigate", "Mod_Multiplier", "target_speed_multiplier, 0.5")
                && has_row(tables.notes, "Test_Frigate", "Mod_Flag", "weapons_enabled_flag, false"),
                "AB-26: unknown, ignored and retained unmodelled names have informational notes");
            for (const std::string_view value : {"target_speed_multiplier, 0.5", "TARGET_TAKE_DAMAGE_MULTIPLIER, 3"}) {
                expect(std::any_of(tables.notes.begin(), tables.notes.end(), [&](const auto& note) {
                    return note.owner == "Test_Frigate" && note.field == "Mod_Multiplier" && note.value == value
                        && note.reason == "original ignores recognized modifier (AB-26)";
                }) && !has_row(tables.unresolved, "Test_Frigate", "Mod_Multiplier", value),
                    "AB-26: original-ignored target names are informational, not unresolved behavior");
            }
            const auto built = eawr::units::ability_table(tables);
            const auto* profile = built ? built.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
            expect(profile && profile->abilities.size() == 1, "AB-26: unknown names preserve the ability");
            if (profile && profile->abilities.size() == 1) {
                const auto& m = profile->abilities[0].modifiers;
                expect(m.speed == Fixed::from_raw(raw(2)) && m.target_speed == Fixed::from_decimal("0.5").value()
                    && m.target_take_damage == Fixed::from_raw(raw(3))
                    && m.weapons_enabled == false && m.can_be_squashed == false
                    && m.allow_target_diminishing_firepower == false,
                    "AB-26: known values retained case-insensitively, duplicate flag takes last value");
            }
            auto changed = tables;
            auto* unit = const_cast<eawr::units::UnitType*>(changed.find("Test_Frigate"));
            expect(unit && unit->abilities[0].modifiers.size() == 3 && unit->abilities[0].modifier_flags.size() == 4,
                "AB-26: unknown values are not stored");
            if (unit) {
                unit->abilities[0].modifiers[1].value = Fixed::from_decimal("0.1").value();
                expect(eawr::units::content_identity(changed) != eawr::units::content_identity(tables),
                    "AB-26: ignored target modifiers still participate in content identity");
                unit->abilities[0].modifiers[1].value = Fixed::from_decimal("0.5").value();
                unit->abilities[0].modifier_flags[0].value = true;
                expect(eawr::units::content_identity(changed) != eawr::units::content_identity(tables),
                    "AB-26: retained flags participate in content identity");
            }
        }
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        xml.insert(xml.find("</Objects>"), R"xml(
          <SpecialStructure Name="Test_Planetary_Source">
            <Affiliation>Rebel</Affiliation><SpaceBehavior>SPECIAL_WEAPON</SpaceBehavior>
            <Special_Weapon_Index>0</Special_Weapon_Index><Special_Weapon_Target_Action_Index>1</Special_Weapon_Target_Action_Index>
            <Special_Weapon_Valid_Targets>Transport | Frigate</Special_Weapon_Valid_Targets>
            <Projectile_Types>Proj_Test_Generic</Projectile_Types>
            <Projectile_Fire_Recharge_Seconds>12.52</Projectile_Fire_Recharge_Seconds>
            <Projectile_Fire_Pulse_Count>3</Projectile_Fire_Pulse_Count>
            <Projectile_Fire_Pulse_Delay_Seconds>0.52</Projectile_Fire_Pulse_Delay_Seconds>
          </SpecialStructure>)xml");
        const auto station = xml.find("<StarBase Name=\"Skirmish_Test_Base\">");
        expect(station != std::string::npos, "WSW: synthetic station opening exists");
        if (station != std::string::npos) xml.insert(station + std::string_view{"<StarBase Name=\"Skirmish_Test_Base\">"}.size(),
            "<Tactical_Buildable_Objects_Multiplayer>Rebel, Test_Use_Unlock</Tactical_Buildable_Objects_Multiplayer>");
        xml.insert(xml.find("</Objects>"), R"xml(<UpgradeObject Name="Test_Use_Unlock">
          <Behavior>DUMMY_UPGRADE, DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED</Behavior>
          <Affiliation>Rebel</Affiliation><CategoryMask>Upgrade</CategoryMask>
          <Tactical_Build_Cost_Multiplayer>1</Tactical_Build_Cost_Multiplayer>
          <Tactical_Build_Time_Seconds>1</Tactical_Build_Time_Seconds><Tactical_Production_Queue>Tactical_Upgrades</Tactical_Production_Queue>
          <Abilities SubObjectList="Yes"><Battlefield_Modifier_Ability Name="Test_Unlock_Effect">
            <Activation_Style>Space_Automatic</Activation_Style><Enable_Special_Weapons>Yes</Enable_Special_Weapons>
            <Apply_To_All_Allies>Yes</Apply_To_All_Allies>
          </Battlefield_Modifier_Ability></Abilities></UpgradeObject>)xml");
        write_fixture(tree.root, xml);
        write(tree.root / "XML" / "FactionFiles.xml", "<Faction_Files><File>test_factions.xml</File></Faction_Files>");
        write(tree.root / "XML" / "test_factions.xml", R"xml(<Factions><Faction Name="Rebel">
          <Is_Playable>yes</Is_Playable><Standalone_Space_Maps_Special_Weapon_A>Test_Planetary_Source</Standalone_Space_Maps_Special_Weapon_A>
          <Standalone_Space_Maps_Special_Weapon_B /></Faction></Factions>)xml");
        const auto loaded = load(tree.root, models(1.0F));
        expect(loaded.tables.has_value(), "WSW-01: faction mapping expands hidden source data without a space model");
        if (loaded.tables) {
            const auto* unlock = loaded.tables->find("Test_Use_Unlock");
            expect(unlock && unlock->consume_after_special_fire && unlock->special_unlocks.size() == 1
                && unlock->special_unlocks.front().enable && unlock->special_unlocks.front().all_allies,
                "WSW-07/08/46: active automatic unlock metadata and consume marker load independently");
            const auto* unit = loaded.tables->find("Test_Planetary_Source");
            expect(unit && unit->planetary_weapon && !unit->hull && unit->model_path.empty() && !unit->locomotion,
                "WSW-01: source remains separate from ground model and ship durability");
            expect(!has_row(loaded.tables->unresolved, "Test_Planetary_Source", "Space_Model_Name"),
                "WSW-01: no invented source space-model requirement");
            const auto combat = eawr::units::combat_table(*loaded.tables);
            const auto* profile = combat ? combat.value().find(eawr::assets::object_type_crc("Test_Planetary_Source")) : nullptr;
            expect(profile && profile->planetary_weapon && profile->planetary_weapon->index == 0
                && profile->planetary_weapon->target_action == 1 && profile->planetary_weapon->valid_targets == 20
                && profile->planetary_weapon->recharge_frames == 376 && profile->planetary_weapon->pulse_delay_frames == 16,
                "WSW-14/19: loaded source carries authored selectors and nearest-frame timing");
            expect(profile && profile->weapons.size() == 1 && profile->weapons.front().shot
                && profile->weapons.front().cone_width == Fixed::from_raw(raw(360))
                && profile->weapons.front().cone_height == Fixed::from_raw(raw(180)),
                "WSW-21/45: shared projectile profile retains source cone defaults");
        }
    }
    {
        // WPCM-02..04: every Projectile_Combat_Mod_* tag reaches the projectile's profile; the radius tag is ignored.
        for (const auto* stat : {"COMBAT_MOD_HEALTH", "COMBAT_MOD_DAMAGE", "COMBAT_MOD_ENERGY", "COMBAT_MOD_SHIELD",
                                 "COMBAT_MOD_DEFENSE", "COMBAT_MOD_SPEED", "COMBAT_MOD_FIRE_RANGE", "COMBAT_MOD_FIRE_RATE"}) {
            TempTree tree;
            auto xml = units_xml("3600", "Unit_Select_Test");
            const std::string opening = "<Projectile Name=\"Proj_Test_Generic\">";
            xml.insert(xml.find(opening) + opening.size(),
                std::string("<Projectile_Combat_Mod>") + stat + "</Projectile_Combat_Mod>"
                "<Projectile_Combat_Mod_Radius>5000</Projectile_Combat_Mod_Radius>"
                "<Projectile_Combat_Mod_Duration>14</Projectile_Combat_Mod_Duration>"
                "<Projectile_Combat_Mod_Value>-0.25</Projectile_Combat_Mod_Value>"
                "<Projectile_Combat_Mod_Effect>fixture_burn</Projectile_Combat_Mod_Effect>"
                "<Projectile_Combat_Mod_Category_Mask>Frigate</Projectile_Combat_Mod_Category_Mask>");
            write_fixture(tree.root, xml);
            const auto loaded = load(tree.root, models(-40.0F));
            expect(loaded.tables.has_value(), "WPCM-02: combat modifier XML loads");
            if (!loaded.tables) continue;
            const bool supported = std::string_view{stat} != "COMBAT_MOD_FIRE_RATE";
            const auto found = std::find_if(loaded.tables->projectiles.begin(), loaded.tables->projectiles.end(),
                [](const auto& entry) { return entry.combat_mod.enabled; });
            if (!supported) {
                expect(found == loaded.tables->projectiles.end(), "WPCM-03: a statistic without a bonus slot is reported, not applied");
                continue;
            }
            if (found == loaded.tables->projectiles.end()) { expect(false, "WPCM-02: the modifier is decoded"); continue; }
            const auto& mod = found->combat_mod;
            expect(mod.value == Fixed::from_decimal("-0.25").value(), "WPCM-02: the authored value is decoded");
            expect(mod.duration_frames == 14 * 30, "WPCM-02: the duration is in logical frames");
            expect(mod.categories != 0, "WPCM-02: the category mask is decoded");
            expect(mod.key == eawr::assets::object_type_crc(found->id), "WPCM-03: the key is the projectile type name CRC");
            expect(found->combat_mod_effect == "fixture_burn", "WPCM-04: the effect name is kept");
            auto changed = *loaded.tables;
            for (auto& entry : changed.projectiles) if (entry.combat_mod.enabled) entry.combat_mod.value = Fixed::from_decimal("-0.5").value();
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                "WPCM-02: the modifier value changes replay content identity");
        }
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto marker = xml.find("<SpaceUnit Name=\"Test_Frigate\">") + std::string_view{"<SpaceUnit Name=\"Test_Frigate\">"}.size();
        xml.insert(marker, "<Death_Projectiles>Proj_Test_Small, Proj_Test_Turbo</Death_Projectiles>"
            "<Ranged_Target_Z_Adjust>30</Ranged_Target_Z_Adjust>");
        const auto projectile = xml.find("<Projectile Name=\"Proj_Test_Generic\">")
            + std::string_view{"<Projectile Name=\"Proj_Test_Generic\">"}.size();
        xml.insert(projectile, "<Projectile_Damage_Delay_Secs>0.5</Projectile_Damage_Delay_Secs>"
            "<Projectile_Damages_Random_Hard_Points>Yes</Projectile_Damages_Random_Hard_Points>");
        write_fixture(tree.root, xml);
        const auto loaded = load(tree.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WNO-29/32: synthetic inherited payload fixture loads");
        if (loaded.tables) {
            const auto* unit = loaded.tables->find("Test_Frigate");
            const auto combat = eawr::units::combat_table(*loaded.tables);
            const auto* profile = combat ? combat.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
            expect(unit && unit->death_projectiles == std::vector<std::string>{"Proj_Test_Small", "Proj_Test_Turbo"}
                && unit->death_projectile_indices.size() == 2 && profile && profile->death_projectiles.size() == 2,
                "WNO-29: loader resolves all authored entries in order");
            if (profile && profile->death_projectiles.size() == 2) {
                expect(profile->ranged_target_z_adjust == Fixed::from_raw(raw(30))
                    && profile->death_projectiles.front().damage_delay == Fixed::from_raw(raw(1) / 2),
                    "WNO-30/32: typed height and inherited explicit delay reach the sim");
                // WAD-27: the inherited random-hardpoint flag reaches the shot profile and the content identity.
                expect(profile->death_projectiles.front().damages_random_hardpoints,
                    "WAD-27: inherited Projectile_Damages_Random_Hard_Points reaches the sim shot profile");
                auto cleared = *loaded.tables;
                for (auto& shot : cleared.projectiles) shot.damages_random_hardpoints = false;
                expect(eawr::units::content_identity(cleared) != eawr::units::content_identity(*loaded.tables),
                    "WAD-27: the random-hardpoint flag is part of the replay content identity");
            }
            auto changed = *loaded.tables;
            for (auto& ship : changed.units) if (ship.id == "Test_Frigate") ship.death_projectiles.clear();
            expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*loaded.tables),
                "WNO-29: death payload authoring changes replay content identity");
        }
    }
    {
        TempTree tree;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        xml.replace(begin, end + std::string_view{"</Unit_Abilities_Data>"}.size() - begin,
            "<Create_Team_Type>Test_Squadron</Create_Team_Type><Redirect_Damage_To_Teammates>Yes</Redirect_Damage_To_Teammates>"
            "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>REPLENISH_WINGMEN</Type><Recharge_Seconds>60</Recharge_Seconds>"
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
            "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>" + kind + "</Type>"
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
            "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>" + kind + "</Type><GUI_Activated_Ability_Name>beam</GUI_Activated_Ability_Name>"
            "<Expiration_Seconds>5</Expiration_Seconds><Recharge_Seconds>20</Recharge_Seconds>"
            "<Mod_Multiplier>SPEED_MULTIPLIER,0.6</Mod_Multiplier></Unit_Ability></Unit_Abilities_Data>"
            "<Abilities SubObjectList=\"Yes\"><" + handler + " Name=\"beam\"><Activation_Style>User_Input</Activation_Style>"
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
    // WST-09: optional GameConstants seconds truncate to logical frames.
    for (const std::string value : {"", "2.25", "0", "-1", "oops", "3601"}) {
        TempTree fixture;
        write_fixture(fixture.root, units_xml("3600", "Unit_Select_Test"));
        const auto path = fixture.root / "XML" / "GameConstants.xml";
        if (!value.empty()) {
            std::ifstream stream(path);
            std::string constants((std::istreambuf_iterator<char>(stream)), {});
            constants.insert(constants.find("</GameConstants>"),
                "<Stealth_Detection_Time>" + value + "</Stealth_Detection_Time>");
            write(path, constants);
        }
        const auto loaded = load(fixture.root, models(-40.0F));
        expect(loaded.tables.has_value(), "WST-09: optional detection constant fixture loads");
        if (!loaded.tables) continue;
        const bool valid = value.empty() || value == "2.25" || value == "0";
        expect(has_row(loaded.tables->unresolved, "GameConstants", "Stealth_Detection_Time") == !valid,
            "WST-09: invalid detection seconds are reported");
        auto tables = *loaded.tables;
        auto& owner = tables.units.front();
        eawr::units::Ability cloak;
        cloak.type = "STEALTH";
        cloak.expiration_seconds = Fixed::from_raw(raw(10));
        cloak.gui_activated_ability_name = "Test_Stealth";
        owner.abilities = {cloak};
        eawr::sim::tactical::SpecialAbilityProfile handler;
        handler.name = "Test_Stealth";
        handler.kind = eawr::sim::tactical::SpecialAbilityKind::stealth;
        handler.style = eawr::sim::tactical::SpecialActivationStyle::user_input;
        handler.service_interval = 1;
        owner.special_abilities = {handler};
        const auto bound = eawr::units::ability_table(tables);
        expect(static_cast<bool>(bound), "WST-09: detection constant binds through ability table");
        const auto* profile = bound ? bound.value().find(eawr::assets::object_type_crc(owner.id)) : nullptr;
        expect(profile && profile->stealth_detection_frames == (value == "2.25" ? 67U : value == "0" ? 0U : 30U),
            "WST-09: authored 2.25 seconds truncate to 67 frames; absent defaults to 30");
        if (value == "2.25") {
            auto absent = tables;
            std::erase_if(absent.constants.scalars, [](const auto& constant) { return constant.tag == "Stealth_Detection_Time"; });
            expect(eawr::units::content_identity(absent) != eawr::units::content_identity(tables),
                "WST-09: authored detection time enters content identity");
        }
    }
    barrage_inputs();
    cluster_bomb_inputs();
    manual_weapon_loading();
    randomized_fire_bone_loading();
    hardpoint_min_range_loading();
    hardpoint_turret_loading();
    special_weapon_classification();
    rocket_variant_inputs();
    unknown_inaccuracy_categories();
    unknown_restriction_categories();
    restriction_list_edges();
    ion_stun_content_errors();
    ability_limits();
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
    expect(station.ai_community_property, "PE-34: AI community metadata is read for non-capture originals");
    auto capture_tables = tables;
    capture_tables.units[1].capture_point = true;
    auto selection_only = capture_tables;
    selection_only.units[0].ai_community_property = false;
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
    expect(station.fow_reveal_range && station.fow_reveal_range->raw() == raw(2000),
           "sensor range through the variant");
    expect(tables.units[1].fow_reveal_range && tables.units[1].fow_reveal_range->raw() == raw(1200)
               && tables.units[4].fow_reveal_range && tables.units[4].fow_reveal_range->raw() == raw(500)
               && !tables.units[2].fow_reveal_range,
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
        expect(laser.attachment.owner_bone_index.has_value() && !laser.fire_a.owner_bone_index,
               "BCAM-21: only owner-model bones expose an index for posed owner lookup");
        auto metadata_changed = tables;
        metadata_changed.units[1].hardpoints[0].attachment.owner_bone_index.reset();
        expect(eawr::units::content_identity(metadata_changed) == eawr::units::content_identity(tables),
               "BCAM-21: presentation bone indices do not change content identity");
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
           "ML-26: placeholder-first variant replaces the base and keeps every member occurrence");
    expect(has_row(tables.notes, "Test_Squadron_Pair", "Squadron_Offsets", "Test_Squadron"),
           "a base layer that also authors the offsets is noted");

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

    expect(tables.constants.scalars.size() == 51, "every required combat scalar has a row");
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

    // WMV-53: a selected TransportUnit is a body, not carried metadata. Its
    // inherited locomotor decides ship versus solo craft independently of XML class.
    for (const bool fighter : {false, true}) {
        std::optional<Fixed> previous_end;
        for (const auto maximum : {"3", "6"}) {
            TempTree transport_tree;
            auto xml = units_xml("3600", "Unit_Select_Test");
            const std::string opening = "<SpaceUnit Name=\"Test_Frigate\">";
            const auto begin = xml.find(opening);
            const auto end = xml.find("</SpaceUnit>", begin);
            auto body = xml.substr(begin + opening.size(), end - begin - opening.size());
            if (fighter) {
                const std::string simple = "SIMPLE_SPACE_LOCOMOTOR";
                body.replace(body.find(simple), simple.size(), "FIGHTER_LOCOMOTOR");
            }
            body += "<OverrideAcceleration>0.5</OverrideAcceleration><OverrideDeceleration>0.25</OverrideDeceleration>"
                "<Max_Rate_Of_Roll>1.5</Max_Rate_Of_Roll><Bank_Turn_Angle>35</Bank_Turn_Angle>"
                "<Min_Speed>0.5</Min_Speed><Max_Thrust>0.125</Max_Thrust><Layer_Z_Adjust>7</Layer_Z_Adjust>";
            const auto replacement = "<TransportUnit Name=\"Test_Transport_Base\">" + body + "</TransportUnit>"
                "<TransportUnit Name=\"Test_Frigate\"><Variant_Of_Existing_Type>Test_Transport_Base</Variant_Of_Existing_Type>"
                "<Max_Speed>" + std::string(maximum) + "</Max_Speed></TransportUnit>";
            xml.replace(begin, end + std::string("</SpaceUnit>").size() - begin, replacement);
            write_fixture(transport_tree.root, xml);
            const auto loaded_transport = load(transport_tree.root, models(-40.0F));
            expect(loaded_transport.tables.has_value(), "WMV-53: selected transport body loads");
            if (!loaded_transport.tables) continue;
            const auto* type = loaded_transport.tables->find("Test_Frigate");
            expect(type && type->xml_type == "TransportUnit" && type->locomotion
                && type->kind == (fighter ? UnitKind::craft : UnitKind::ship),
                "WMV-53: inherited space locomotor selects transport ship or solo craft");
            const auto bound = eawr::units::motion_table(*loaded_transport.tables);
            expect(bool(bound), "WMV-53: transport motion binds");
            if (!bound) continue;
            const auto id = eawr::assets::object_type_crc("Test_Frigate");
            const auto scaled = [](const char* value) {
                return eawr::sim::math::multiply(Fixed::from_decimal(value).value(),
                    Fixed::from_decimal("1.2").value()).value();
            };
            namespace tactical = eawr::sim::tactical;
            if (!fighter) {
                const auto* profile = bound.value().find(id);
                const auto* footprint = bound.value().footprint(id);
                expect(profile && profile->max_speed == scaled(maximum)
                    && profile->rate_of_turn == scaled("0.7") && profile->acceleration == scaled("0.5")
                    && profile->deceleration == scaled("0.25") && profile->roll_rate == scaled("1.5")
                    && profile->bank_angle == Fixed::from_decimal("35").value()
                    && footprint && footprint->layer == tactical::SpaceLayer::frigate,
                    "WMV-53: transport overrides and inherited movement inputs reach the ship consumer");
                if (!profile) continue;
                const auto plan = tactical::plan_move(*profile, bound.value().rules, 0, {}, {}, {}, point(raw(600), 0, 0));
                expect(plan && plan.value().kind == tactical::MotionKind::path && !plan.value().nodes.empty(),
                    "WMV-53: selected transport produces a movement path");
                if (plan && !plan.value().nodes.empty()) {
                    const auto finish = plan.value().nodes.back().frame;
                    if (previous_end) expect(finish < *previous_end,
                        "WMV-53: changing only the transport override changes actual path duration");
                    previous_end = finish;
                }
            } else {
                const auto* profile = bound.value().squadrons.find_craft(id);
                expect(profile && !bound.value().find(id) && profile->max_speed == scaled(maximum)
                    && profile->rate_of_turn == scaled("0.7") && profile->roll_rate == scaled("1.5")
                    && profile->bank_angle == Fixed::from_decimal("35").value()
                    && profile->min_speed == scaled("0.5") && profile->thrust == Fixed::from_decimal("0.125").value()
                    && profile->layer_z == Fixed::from_decimal("7").value(),
                    "WMV-53: craft transport minimum, raw thrust and height reach the craft consumer");
                const auto* formation = bound.value().squadrons.find_squadron(id);
                expect(formation && formation->formation_deceleration == scaled("0.25"),
                    "WMV-53: solo transport formation upkeep separately consumes its deceleration override");
                if (!profile) continue;
                tactical::CraftView self{1, point(0, 0, raw(7)), {}, profile};
                tactical::CraftFrame frame;
                frame.self = &self; frame.leader = &self; frame.moving = true;
                frame.hold = point(raw(1000), 0, raw(7));
                const auto step = tactical::step_craft(frame);
                expect(step && step.value().position.x > self.position.x
                    && step.value().state.velocity.x == profile->thrust,
                    "WMV-53: solo transport flight accelerates by its authored thrust");
                self.state.velocity = {profile->min_speed, {}, {}};
                frame.moving = false;
                const auto idle = tactical::step_craft(frame);
                expect(idle && idle.value().state.velocity.x == profile->min_speed,
                    "WMV-53: solo transport idle flight retains its authored minimum");
            }
        }
    }

    // #70 motion: speeds, accelerations and the rate of turn x Object_Max_Speed_Multiplier_Space (1.2).
    const auto motion = eawr::units::motion_table(tables);
    expect(static_cast<bool>(motion), "motion table builds");
    if (!motion) return;
    {
        TempTree authored_team;
        auto xml = units_xml("3600", "Unit_Select_Test");
        const std::string opening = "<Container Name=\"Test_Pair_Base_Container\">";
        xml.insert(xml.find(opening) + opening.size(),
            "<Max_Speed>4</Max_Speed><OverrideDeceleration>0.5</OverrideDeceleration>");
        write_fixture(authored_team.root, xml);
        const auto loaded_team = load(authored_team.root, models(-40.0F));
        const auto* parent = loaded_team.tables ? loaded_team.tables->find("Test_Squadron_Pair") : nullptr;
        expect(parent && parent->team_movement.max_speed == Fixed::from_raw(raw(4))
            && parent->team_movement.deceleration == Fixed::from_raw(raw(1) / 2),
            "WMV-44: inherited Create_Team_Type supplies the actual container movement inputs");
        expect(loaded_team.tables && eawr::units::content_identity(*loaded_team.tables) != eawr::units::content_identity(tables),
            "WMV-44: authored container formation inputs change content identity");
    }
    {
        auto changed = tables;
        for (auto& item : changed.units) {
            if (item.kind == UnitKind::craft) item.movement.deceleration = Fixed::from_raw(raw(7));
            if (item.kind == UnitKind::squadron) { item.team_movement.deceleration.reset(); item.movement.deceleration = Fixed::from_raw(raw(9)); }
        }
        const auto defaults = eawr::units::motion_table(changed);
        const auto parent_id = eawr::assets::object_type_crc("Test_Squadron");
        const auto* parent = defaults ? defaults.value().squadrons.find_squadron(parent_id) : nullptr;
        expect(parent && parent->formation_deceleration == Fixed{},
            "WMV-44: unauthored container deceleration stays zero despite child overrides");
        for (auto& item : changed.units) {
            if (item.id == "Test_Squadron") item.team_movement.deceleration = Fixed::from_raw(raw(1) / 4);
        }
        const auto authored = eawr::units::motion_table(changed);
        parent = authored ? authored.value().squadrons.find_squadron(parent_id) : nullptr;
        expect(parent && parent->formation_deceleration == Fixed::from_decimal("0.3").value(),
            "WMV-44: an omitted maximum retains the nonzero type default and speed ratio");
        for (auto& item : changed.units) {
            if (item.id == "Test_Squadron") item.team_movement.max_speed = Fixed{};
        }
        const auto zero_maximum = eawr::units::motion_table(changed);
        parent = zero_maximum ? zero_maximum.value().squadrons.find_squadron(parent_id) : nullptr;
        expect(parent && parent->formation_deceleration == Fixed::from_raw(raw(1) / 4),
            "WMV-44: an explicitly zero maximum returns its own override unscaled");
        for (auto& item : changed.units) {
            if (item.id == "Test_Squadron") item.team_movement.max_speed = Fixed::from_raw(raw(3));
        }
        const auto scaled_parent = eawr::units::motion_table(changed);
        parent = scaled_parent ? scaled_parent.value().squadrons.find_squadron(parent_id) : nullptr;
        expect(parent && parent->formation_deceleration == Fixed::from_decimal("0.3").value(),
            "WMV-44: a member with authored maximum speed scales its own override");
        const auto* craft = authored ? authored.value().squadrons.find_craft(eawr::assets::object_type_crc("Test_Fighter")) : nullptr;
        const auto* original = motion.value().squadrons.find_craft(eawr::assets::object_type_crc("Test_Fighter"));
        expect(craft && original && craft->thrust == original->thrust,
            "WMV-42: deceleration overrides do not replace craft flight thrust");
    }
    for (const auto& [general, space, station_role] : {
            std::tuple{"DUMMY_STAR_BASE", "SPAWN_SQUADRON", true},
            std::tuple{"SELECTABLE", "dummy_star_base", true},
            std::tuple{"STARBASE", "SPAWN_SQUADRON", false},
            std::tuple{"DUMMY_GROUND_STRUCTURE", "SPAWN_SQUADRON", false},
            std::tuple{"SELECTABLE", "SPAWN_SQUADRON", false}}) {
        auto changed = tables;
        for (auto& unit : changed.units) if (unit.id == "Skirmish_Test_Base") {
            unit.footprint.hazard.behavior = {general};
            unit.footprint.hazard.space_behavior = {space};
            unit.spawner->starting.clear();
            for (auto& tier : unit.spawner->campaign_starting) tier.clear();
        }
        const auto rebound = eawr::units::motion_table(changed);
        const auto* profile = rebound ? rebound.value().squadrons.find_spawner(
            eawr::assets::object_type_crc("Skirmish_Test_Base")) : nullptr;
        expect(rebound && (profile != nullptr) == station_role
            && (!profile || (profile->starbase && profile->entries.empty() && !profile->bays.empty())),
            "WSL-26: station behavior in either list admits an empty authored hangar; kind alone does not");
    }
    const auto times = [](const char* value) {
        return eawr::sim::math::multiply(Fixed::from_decimal(value).value(), Fixed::from_decimal("1.2").value()).value();
    };
    expect(motion.value().rules.arc_degrees == Fixed::from_raw(raw(15)) &&
               motion.value().rules.expansion_distance == Fixed::from_raw(raw(300)),
           "motion rules: 360 / MaxRotationsSpace and XYExpansionDistanceSpace");
    // space-targeting R-01: a weapon hardpoint whose authored maximum recharge is within
    // Hardpoint_Recharge_Cutoff_For_Opportunity_Fire is forced on for an AI owner; the comparison
    // uses the authored seconds, not the truncated hundredths (perturbation of both values).
    for (const auto& [recharge, cutoff, forced] : {std::tuple{"3.0", "3.0", true}, std::tuple{"3.005", "3.0", false},
                                                    std::tuple{"2.0", "1.5", false}, std::tuple{"2.0", "2.0", true}}) {
        auto changed = tables;
        for (auto& unit : changed.units) {
            for (auto& hardpoint : unit.hardpoints) {
                if (hardpoint.weapon) hardpoint.weapon->max_recharge_seconds = Fixed::from_decimal(recharge).value();
            }
        }
        for (auto& constant : changed.constants.scalars) {
            if (constant.tag == "Hardpoint_Recharge_Cutoff_For_Opportunity_Fire") {
                constant.value = Fixed::from_decimal(cutoff).value();
            }
        }
        const auto rebound = eawr::units::combat_table(changed);
        bool any = false;
        bool all = true;
        if (rebound) {
            for (const auto& profile : rebound.value().profiles) {
                for (const auto& weapon : profile.weapons) {
                    if (weapon.hardpoint == eawr::sim::tactical::object_weapon) continue;
                    any = true;
                    all = all && weapon.ai_opportunity_forced == forced;
                }
            }
        }
        expect(rebound && any && all, "R-01: the AI opportunity cutoff compares the authored recharge seconds");
    }
    // OR-06, OR-14: the approach re-check interval and the guard range come from gameconstants.xml
    // (the fixture authors FoC's 10 frames and 750 units); each authored value moves the rule.
    expect(motion.value().rules.reevaluation_frames == 10 && motion.value().rules.guard_range == Fixed::from_raw(raw(750)) && motion.value().rules.max_waypoints == 6,
           "OR-06/OR-14: the fixture's stock values reach the motion rules");

    // HCB-58 (#4992): FoC reads the constant with atoi into 32 bits and compares the queue count as unsigned.
    for (const auto& [value, capacity] : {std::pair{"0", 0UL}, std::pair{"2", 2UL}, std::pair{"9", 9UL}, std::pair{"2.5", 2UL},
             std::pair{"-0.5", 0UL}, std::pair{"-1", 4294967295UL}, std::pair{"-2", 4294967294UL}, std::pair{"65536", 65536UL},
             std::pair{"65537", 65537UL}, std::pair{"70000", 70000UL}, std::pair{"3000000000", 2147483647UL}}) {
        auto changed = tables;
        for (auto& constant : changed.constants.scalars) {
            if (constant.tag == "MaxWaypointsPerPath") constant.value = Fixed::from_decimal(value).value();
        }
        const auto rebound = eawr::units::motion_table(changed);
        expect(rebound && rebound.value().rules.max_waypoints == capacity,
            std::string{"HCB-58: authored queue capacity "} + value + " reads as the retail unsigned 32-bit value");
    }
    // #4992: the authored text reaches the motion rules the way FoC's atoi reads it (Alliance at War authors -1).
    for (const auto& [text, capacity] : {std::pair{" -1 ", 4294967295UL}, std::pair{"0", 0UL}, std::pair{"6", 6UL},
             std::pair{"65536", 65536UL}, std::pair{"65537", 65537UL}, std::pair{"2.9", 2UL}, std::pair{"lots", 0UL},
             std::pair{"7 paths", 7UL}}) {
        TempTree constants_tree;
        write_fixture(constants_tree.root, units_xml("3600", "Unit_Select_Test"));
        const auto path = constants_tree.root / "XML" / "GameConstants.xml";
        std::string authored;
        {
            std::ifstream in(path, std::ios::binary);
            authored.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::string_view stock = "<MaxWaypointsPerPath> 6 </MaxWaypointsPerPath>";
        const auto at = authored.find(stock);
        expect(at != std::string::npos, "HCB-58: the fixture authors MaxWaypointsPerPath");
        if (at == std::string::npos) continue;
        authored.replace(at, stock.size(), "<MaxWaypointsPerPath>" + std::string{text} + "</MaxWaypointsPerPath>");
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << authored;
        }
        const auto reloaded = load(constants_tree.root, models(-40.0F));
        std::optional<std::uint64_t> got;
        if (reloaded.tables) {
            if (const auto rebound = eawr::units::motion_table(*reloaded.tables)) got = rebound.value().rules.max_waypoints;
        }
        expect(got == capacity, std::string{"HCB-58: authored text '"} + text + "' loads as the retail unsigned queue capacity");
    }
    // Zero wait legs remain valid; negative values degrade with a note.
    for (const auto& [frames, cost, valid] : {std::tuple{"0", "0.0", true}, std::tuple{"0", "0.8", true},
             std::tuple{"-1", "0.8", true}, std::tuple{"100", "-0.5", true}}) {
        auto changed = tables;
        for (auto& constant : changed.constants.scalars) {
            if (constant.tag == "WaitOperatorBaseFrameTime") constant.value = Fixed::from_decimal(frames).value();
            if (constant.tag == "WaitOperatorCostCoefficient") constant.value = Fixed::from_decimal(cost).value();
        }
        expect(static_cast<bool>(eawr::units::motion_table(changed)) == valid,
            std::string{"PC-08: wait leg frame time "} + frames + " and cost " + cost + (valid ? " accepted" : " refused"));
    }
    const auto set_motion_constant = [](auto& changed, const std::string_view tag, const std::string_view text) {
        for (auto& constant : changed.constants.scalars) {
            if (constant.tag != tag) continue;
            constant.value = Fixed::from_decimal(text).value();
            constant.text = std::string(text);
            return;
        }
        changed.constants.scalars.push_back({std::string(tag), Fixed::from_decimal(text).value(), std::string(text)});
    };
    const auto count_motion_notes = [](const auto& captured, const std::string_view code, const std::string_view tag) {
        return std::count_if(captured.begin(), captured.end(), [&](const auto& note) {
            return note.code == code && note.tag == tag;
        });
    };
    for (const auto tag : {"MovementReevaluationFrameCount", "Space_Guard_Range", "Nebula_Ability_Disable_Time"}) {
        auto changed = tables;
        auto& constants = changed.constants.scalars;
        constants.erase(std::remove_if(constants.begin(), constants.end(), [&](const auto& entry) {
            return entry.tag == tag;
        }), constants.end());
        eawr::core::LoadNoteBuffer notes;
        const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &notes);
        eawr::core::LoadNoteCollector collector;
        collector.merge(notes);
        const auto captured = collector.snapshot();
        expect(recovered && recovered.value().rules.reevaluation_frames == 10
            && recovered.value().rules.guard_range == Fixed::from_raw(raw(750))
            && recovered.value().nebula_disable_seconds == Fixed::from_raw(raw(5)),
            "omitted interval, guard and nebula globals use their stock values");
        expect(count_motion_notes(captured, "EAWR-LOAD-0150", tag) == 1
            && std::any_of(captured.begin(), captured.end(), [&](const auto& note) {
                return note.tag == tag && note.classification == eawr::core::LoadCategory::retail_accepts
                    && note.action == eawr::core::LoadAction::default_used && note.retail == "LSA-P5";
            }), "each missing global emits exactly one class B stock-default note");
        if (!recovered) continue;
        eawr::sim::tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
            {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
        auto live = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, recovered.value());
        expect(static_cast<bool>(live), "missing-global motion session creates");
        if (live) {
            const eawr::sim::InlineExecutor executor;
            expect(static_cast<bool>(live.value().step(executor)), "missing-global motion session steps");
        }
    }
    for (const auto& [text, effective, changed_value] : {std::tuple{"-1", "2", true},
             std::tuple{"400", "360", true}, std::tuple{"2", "2", false}, std::tuple{"360", "360", false}}) {
        auto changed = tables;
        set_motion_constant(changed, "MaxRotationsSpace", text);
        eawr::core::LoadNoteBuffer notes;
        const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &notes);
        eawr::core::LoadNoteCollector collector;
        collector.merge(notes);
        const auto captured = collector.snapshot();
        const auto rotations = Fixed::from_decimal(effective).value();
        expect(recovered && recovered.value().avoidance->max_rotations == rotations
            && recovered.value().rules.arc_degrees == eawr::sim::math::divide(Fixed::from_raw(raw(360)), rotations).value(),
            "0151: rotations clamp before division at their exact bounds");
        expect(count_motion_notes(captured, "EAWR-LOAD-0151", "MaxRotationsSpace") == (changed_value ? 1 : 0),
            "0151: rotations emit one note only when changed");
    }
    for (const auto& [text, effective, changed_value] : {std::tuple{"0", 2U, true}, std::tuple{"2.5", 2U, true},
             std::tuple{"70000", 65536U, true}, std::tuple{"-3", 2U, true}, std::tuple{"65536", 65536U, false}}) {
        auto changed = tables;
        set_motion_constant(changed, "MovementReevaluationFrameCount", text);
        eawr::core::LoadNoteBuffer notes;
        const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &notes);
        eawr::core::LoadNoteCollector collector;
        collector.merge(notes);
        const auto captured = collector.snapshot();
        expect(recovered && recovered.value().rules.reevaluation_frames == effective,
            "0152: interval truncation and bounds have exact effective frame counts");
        expect(count_motion_notes(captured, "EAWR-LOAD-0152", "MovementReevaluationFrameCount") == (changed_value ? 1 : 0),
            "0152: interval conversion emits one note only when changed");
        for (const auto& note : captured)
            if (note.code == "EAWR-LOAD-0152") expect(note.authored_value == text
                && note.effective_value == std::to_string(effective), "0152: record keeps authored spelling and final frames");
    }
    for (const auto& [text, effective, changed_value] : {std::tuple{"16", 16U, false}, std::tuple{"17", 16U, true},
             std::tuple{"6.9", 6U, true}, std::tuple{"4294967297", 16U, true}}) {
        auto changed = tables;
        set_motion_constant(changed, "SpacePathingTries", text);
        eawr::core::LoadNoteBuffer notes;
        const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &notes);
        eawr::core::LoadNoteCollector collector;
        collector.merge(notes);
        const auto captured = collector.snapshot();
        expect(recovered && recovered.value().avoidance->tries == effective, "0158: count truncation and saturation are bounded");
        expect(count_motion_notes(captured, "EAWR-LOAD-0158", "SpacePathingTries") == (changed_value ? 1 : 0),
            "0158: count conversion emits exactly one note on change");
        for (const auto& note : captured)
            if (note.tag == "SpacePathingTries") expect(note.effective_value == std::to_string(effective),
                "0158: even a count above uint32 reports the final effective count");
    }
    for (const auto extent : {Fixed::from_raw(raw(262144)), Fixed::from_raw(raw(300000)), Fixed::from_raw(INT64_MAX)}) {
        auto changed = tables;
        for (auto& unit : changed.units) {
            if (unit.id != "Test_Frigate") continue;
            unit.scale_factor = Fixed::from_raw(raw(extent.raw() == INT64_MAX ? 2 : 1));
            unit.footprint.custom_hard_x = extent;
            unit.footprint.custom_hard_y = Fixed::from_raw(raw(40));
        }
        for (const bool combat_path : {false, true}) {
            eawr::core::LoadNoteBuffer notes;
            bool accepted = false;
            Fixed effective;
            if (combat_path) {
                const auto recovered = eawr::units::combat_table(changed, eawr::units::ContentErrors::strict, nullptr, &notes);
                accepted = static_cast<bool>(recovered);
                if (recovered) for (const auto& footprint : recovered.value().footprints)
                    if (footprint.type_id == eawr::assets::object_type_crc("Test_Frigate")) effective = footprint.x_extent;
            } else {
                const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &notes);
                accepted = static_cast<bool>(recovered);
                if (recovered) effective = recovered.value().footprint(eawr::assets::object_type_crc("Test_Frigate"))->x_extent;
            }
            eawr::core::LoadNoteCollector collector;
            collector.merge(notes);
            const auto captured = collector.snapshot();
            expect(accepted && effective == Fixed::from_raw(raw(262144)), "0157: combat and motion accept bounded target geometry");
            expect(count_motion_notes(captured, "EAWR-LOAD-0157", "Custom_Hard_XExtent")
                == (extent == Fixed::from_raw(raw(262144)) ? 0 : 1), "0157: combat and motion retain exactly one extent note");
        }
    }
    // Missing globals are class B; unsupported authored motion is class C. Both bind and step.
    for (const bool missing : {true, false}) {
        auto changed = tables;
        if (missing) {
            auto& constants = changed.constants.scalars;
            constants.erase(std::remove_if(constants.begin(), constants.end(), [](const auto& entry) {
                return entry.tag == "Object_Max_Speed_Multiplier_Space" || entry.tag == "MaxRotationsSpace"
                    || entry.tag == "XYExpansionDistanceSpace" || entry.tag.starts_with("TurnInPlaceSlowdown")
                    || entry.tag == "WaitOperatorSpeedCoefficient";
            }), constants.end());
        } else {
            for (auto& constant : changed.constants.scalars) {
                if (constant.tag == "MaxRotationsSpace" || constant.tag == "XYExpansionDistanceSpace"
                    || constant.tag == "Space_Guard_Range" || constant.tag == "WaitOperatorSpeedCoefficient"
                    || constant.tag == "TurnInPlaceSlowdownFrigate" || constant.tag == "Nebula_Ability_Disable_Time") {
                    constant.value = Fixed::from_decimal("-1").value();
                    constant.text = "-1";
                }
                if (constant.tag == "MovementReevaluationFrameCount" || constant.tag == "SpacePathingTries") {
                    constant.value = Fixed::from_decimal("2.5").value();
                    constant.text = "2.5";
                }
            }
            for (auto& unit : changed.units) {
                if (unit.id != "Test_Frigate") continue;
                unit.movement.max_speed = Fixed::from_raw(INT64_MAX);
                unit.movement.acceleration = Fixed{};
                unit.movement.deceleration = Fixed{};
                unit.movement.max_rate_of_turn = Fixed{};
                unit.movement.max_rate_of_roll = Fixed::from_raw(-Fixed::scale);
                unit.footprint.custom_soft_radius = Fixed::from_raw(1);
                unit.footprint.custom_hard_x = Fixed::from_raw(INT64_MAX);
                unit.footprint.custom_hard_y = Fixed::from_raw(INT64_MAX);
                unit.scale_factor = Fixed::from_raw(2 * Fixed::scale);
            }
        }
        eawr::core::LoadNoteBuffer buffer;
        const auto recovered = eawr::units::motion_table(changed, {}, nullptr, &buffer);
        eawr::core::LoadNoteCollector collector;
        collector.merge(buffer);
        const auto captured = collector.snapshot();
        expect(static_cast<bool>(recovered), "motion loader normalizes before its former abort paths");
        expect(std::any_of(captured.begin(), captured.end(), [&](const auto& note) {
            return note.code == (missing ? "EAWR-LOAD-0150" : "EAWR-LOAD-0155")
                && note.classification == (missing ? eawr::core::LoadCategory::retail_accepts
                                                  : eawr::core::LoadCategory::unknown_degrade)
                && (missing ? note.retail == "LSA-P5" : note.object == "Test_Frigate");
        }), "motion family notes carry classification and source object");
        if (!recovered) continue;
        if (missing) {
            expect(recovered.value().rules.arc_degrees == Fixed::from_raw(15 * Fixed::scale),
                "missing rotations use the stock default");
        } else {
            const auto* profile = recovered.value().find(eawr::assets::object_type_crc("Test_Frigate"));
            expect(profile && profile->max_speed == Fixed::from_raw(1024 * Fixed::scale)
                && profile->acceleration == Fixed::from_raw(Fixed::scale / 4096),
                "overflow saturates and profile fields clamp to supported bounds");
            expect(recovered.value().rules.guard_range == Fixed{} && recovered.value().rules.reevaluation_frames == 2,
                "negative guard range clamps and fractional frame count truncates");
        }
        eawr::sim::tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
            {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
        auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, recovered.value());
        expect(static_cast<bool>(session), "degraded motion session creates");
        if (session) {
            const eawr::sim::InlineExecutor executor;
            expect(static_cast<bool>(session.value().step(executor)), "degraded motion session steps");
        }
    }
    const auto with_constants = [&](const char* interval, const char* guard) {
        auto changed = tables;
        for (auto& constant : changed.constants.scalars) {
            if (constant.tag == "MovementReevaluationFrameCount") constant.value = Fixed::from_decimal(interval).value();
            if (constant.tag == "Space_Guard_Range") constant.value = Fixed::from_decimal(guard).value();
        }
        return eawr::units::motion_table(changed);
    };
    for (const auto& [interval, guard] : {std::pair{"25", "1200.5"}, std::pair{"12", "0"}}) {
        const auto rebound = with_constants(interval, guard);
        expect(rebound && rebound.value().rules.reevaluation_frames == static_cast<std::uint32_t>(std::stoul(interval))
                   && rebound.value().rules.guard_range == Fixed::from_decimal(guard).value(),
               std::string{"OR-06/OR-14: authored MovementReevaluationFrameCount and Space_Guard_Range reach the motion rules ("}
                   + interval + ", " + guard + (rebound ? ")" : "): " + rebound.error().message));
    }
    for (const char* bad : {"0", "2.5", "70000", "-3"}) {
        expect(static_cast<bool>(with_constants(bad, "750")), "OR-06: unsupported intervals degrade before validation");
    }
    expect(static_cast<bool>(with_constants("10", "-1")), "OR-14: a negative guard range degrades to zero");
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
    {
        auto collision = tables;
        auto unit = std::find_if(collision.units.begin(), collision.units.end(), [](const auto& item) {
            return item.id == "Test_Frigate";
        });
        if (unit != collision.units.end()) {
            unit->id = "SYNTH_QLU1MOI5PTT3";
            unit->table_order = 10;
            eawr::units::ObstacleType obstacle;
            obstacle.id = "SYNTH_OIVRHNPFF4K4";
            obstacle.table_order = 1;
            obstacle.space_layer = "Capital";
            collision.obstacles.push_back(obstacle);
            eawr::core::LoadNoteBuffer notes;
            const auto result = eawr::units::motion_table(collision, {}, nullptr, &notes);
            eawr::core::LoadNoteCollector collector;
            collector.merge(notes);
            const auto captured = collector.snapshot();
            const auto* shape = result ? result.value().footprint(eawr::assets::object_type_crc(unit->id)) : nullptr;
            expect(shape && shape->layer == eawr::sim::tactical::SpaceLayer::capital,
                "earlier obstacle definition wins a unit footprint CRC collision");
            expect(std::any_of(captured.begin(), captured.end(), [&](const auto& note) {
                return note.code == "EAWR-LOAD-0454" && note.object == unit->id && note.effective_value == obstacle.id;
            }), "footprint collision reports the omitted definition and winner");
            if (result) {
                eawr::sim::tactical::TacticalSetup setup;
                setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
                    {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
                auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, result.value());
                expect(static_cast<bool>(session), "collision-degraded motion session creates");
                if (session) {
                    const eawr::sim::InlineExecutor executor;
                    expect(static_cast<bool>(session.value().step(executor)), "collision-degraded motion session steps");
                }
            }
            auto second_obstacle = obstacle;
            second_obstacle.id = unit->id;
            second_obstacle.table_order = 2;
            second_obstacle.space_layer = "Corvette";
            collision.obstacles.push_back(second_obstacle);
            const eawr::units::FootprintTypeSelection selection(collision);
            expect(selection.find(unit->id) && selection.find(unit->id)->obstacle == &collision.obstacles.front(),
                "shared consumer selection retains the same earliest definition");
            const auto obstacle_pair = eawr::units::motion_table(collision);
            shape = obstacle_pair ? obstacle_pair.value().footprint(eawr::assets::object_type_crc(unit->id)) : nullptr;
            expect(shape && shape->layer == eawr::sim::tactical::SpaceLayer::capital,
                "earlier obstacle wins an obstacle footprint CRC collision");
            collision.obstacles.pop_back();
            collision.obstacles.back().space_layer.clear();
            const auto no_shape = eawr::units::motion_table(collision);
            expect(no_shape && !no_shape.value().footprint(eawr::assets::object_type_crc(unit->id)),
                "a winning definition without a space layer does not admit the losing footprint");
            collision.obstacles.back().space_layer = "Capital";
            collision.obstacles.back().table_order = 20;
            const auto unit_first = eawr::units::motion_table(collision);
            shape = unit_first ? unit_first.value().footprint(eawr::assets::object_type_crc(unit->id)) : nullptr;
            expect(shape && shape->layer == eawr::sim::tactical::SpaceLayer::frigate,
                "earlier unit definition wins a unit obstacle CRC collision");
        }
    }
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


// #4985: community selection follows the object's behaviour, not its XML class. A mod's shipyard is a
// SpaceBuildable; a mover or a capture point does not take the station flag.
void community_structure_tables() {
    auto xml = units_xml("3600", "Unit_Select_Test");
    const std::string closing = "</Objects>";
    xml.replace(xml.rfind(closing), closing.size(), R"xml(
<SpaceBuildable Name="Test_Mod_Shipyard">
  <Variant_Of_Existing_Type>Test_Base</Variant_Of_Existing_Type>
  <Is_Community_Property>Yes</Is_Community_Property>
</SpaceBuildable>
<SpaceBuildable Name="Test_Mod_Shipyard_Variant">
  <Variant_Of_Existing_Type>Test_Mod_Shipyard</Variant_Of_Existing_Type>
</SpaceBuildable>
<SpaceBuildable Name="Test_Mod_Private_Shipyard">
  <Variant_Of_Existing_Type>Test_Base</Variant_Of_Existing_Type>
</SpaceBuildable>
<SpaceBuildable Name="Test_Mod_Capture_Shipyard">
  <Variant_Of_Existing_Type>Test_Base</Variant_Of_Existing_Type>
  <Behavior>CAPTURE_POINT</Behavior>
  <Is_Community_Property>Yes</Is_Community_Property>
</SpaceBuildable>
<SpaceUnit Name="Test_Mod_Community_Mover">
  <Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>
  <SpaceBehavior>SPACE_OBSTACLE, SIMPLE_SPACE_LOCOMOTOR</SpaceBehavior>
  <Is_Community_Property>Yes</Is_Community_Property>
</SpaceUnit>
</Objects>)xml");
    TempTree tree;
    write_fixture(tree.root, xml);
    const auto loaded = load(tree.root, models(-40.0F), "Normal_Default", eawr::sim::tactical::BattleDomain::space,
        {"Test_Mod_Shipyard", "Test_Mod_Shipyard_Variant", "Test_Mod_Private_Shipyard", "Test_Mod_Capture_Shipyard",
         "Test_Mod_Community_Mover"});
    if (!loaded.tables) return;
    const auto flag = [&](const std::string_view id) {
        const auto* unit = loaded.tables->find(id);
        expect(unit != nullptr, "community structure fixture type loads");
        return unit != nullptr && unit->station_community_property;
    };
    const auto* shipyard = loaded.tables->find("Test_Mod_Shipyard");
    expect(shipyard != nullptr && shipyard->kind != UnitKind::station && shipyard->xml_type == "SpaceBuildable",
        "#4985: a mod shipyard is a SpaceBuildable, not a station");
    expect(flag("Test_Mod_Shipyard"), "#4985: an immobile SpaceBuildable with Is_Community_Property is ally-selectable");
    expect(flag("Test_Mod_Shipyard_Variant"), "#4985: the community flag survives a variant of the shipyard");
    expect(!flag("Test_Mod_Private_Shipyard"), "#4985: an immobile structure without the tag stays private");
    expect(!flag("Test_Mod_Community_Mover"), "#4985: a mover with the tag and SPACE_OBSTACLE is not an ally-selectable structure");
    const auto* capture = loaded.tables->find("Test_Mod_Capture_Shipyard");
    expect(capture != nullptr && capture->capture_point && capture->community_property && !capture->station_community_property,
        "#4985: a capture point keeps its own community flag, not the station flag");
}

} // namespace unit_tables_test_support
