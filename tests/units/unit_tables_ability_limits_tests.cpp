// #4666: a leaf layer's ability values reach the AbilityTable, a type keeps every ability it authors,
// and under skip_object one invalid ability entry skips only its own type.
#include "unit_tables_support.hpp"
#include "unit_tables_synthetic_fixtures.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace unit_tables_test_support {
namespace {

namespace t = eawr::sim::tactical;

bool replace_once(std::string& text, const std::string_view from, const std::string_view to) {
    const auto at = text.find(from);
    if (at == std::string::npos || text.find(from, at + 1) != std::string::npos) return false;
    text.replace(at, from.size(), to);
    return true;
}

// The fixture with the frigate's DEFEND authored as `expiration` seconds and a speed multiplier of `speed`.
std::string layer_xml(const std::string_view expiration, const std::string_view speed) {
    auto xml = units_xml("3600", "Unit_Select_Test");
    const bool changed = replace_once(xml, "<Expiration_Seconds>15</Expiration_Seconds>",
                             "<Expiration_Seconds>" + std::string(expiration) + "</Expiration_Seconds>")
        && replace_once(xml, "0.8f", std::string(speed) + "f");
    expect(changed, "#4666: the fixture authors one frigate DEFEND expiration and speed multiplier");
    return xml;
}

// A frigate variant that shares the frigate's data, for the "other types stay" check.
std::string with_second_frigate(std::string xml) {
    const auto at = xml.find("<Squadron Name=\"Test_Squadron\">");
    expect(at != std::string::npos, "#4666: the fixture places a squadron after the frigate");
    if (at != std::string::npos) {
        xml.insert(at, "<SpaceUnit Name=\"Test_Frigate_Two\"><Variant_Of_Existing_Type>Test_Frigate"
                       "</Variant_Of_Existing_Type></SpaceUnit>\n");
    }
    return xml;
}

// Mounts `roots` (strongest first) and loads the fixture's types plus the extra frigate.
std::optional<eawr::units::UnitTables> load_layers(const std::vector<std::filesystem::path>& roots) {
    std::vector<eawr::vfs::MountSpec> mounts;
    for (std::size_t index = 0; index < roots.size(); ++index)
        mounts.push_back({"layer" + std::to_string(index), roots[index], "data", {}});
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "#4666: layered fixture mounts");
    if (!mounted) return std::nullopt;
    auto catalog = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "#4666: layered catalog loads");
    if (!catalog) return std::nullopt;
    const auto assets = models(-40.0F);
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &mounted.value();
    input.model = [&](const std::string_view path) -> const eawr::assets::Model* {
        const auto found = assets.find(std::string(path));
        return found == assets.end() ? nullptr : &found->second;
    };
    input.types = {"Skirmish_Test_Base", "Test_Frigate", "Test_Frigate_Two", "Test_Squadron", "Test_Squadron_Pair"};
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "#4666: layered tables load");
    if (!tables) return std::nullopt;
    return std::move(tables).value();
}

const t::AbilityProfile* ability_of(const t::AbilityTable& table, const std::string_view type, const t::AbilityKind kind) {
    const auto* profile = table.find(eawr::assets::object_type_crc(std::string(type)));
    const auto slot = profile != nullptr ? t::ability_slot(*profile, kind) : std::nullopt;
    return slot ? &profile->abilities[*slot] : nullptr;
}

void container_replenishment_loading() {
    const std::string refill = "<Unit_Ability><Type>REPLENISH_WINGMEN</Type>"
        "<Recharge_Seconds>60</Recharge_Seconds><Particle_Effect>test_refill</Particle_Effect></Unit_Ability>";
    // WHE-70: effective TEAM behaviour controls dispatch, including a modded XML class.
    for (const bool container_class : {false, true}) {
        for (const bool authored_team : {false, true}) {
            TempTree tree;
            auto xml = with_second_frigate(layer_xml("15", "0.8"));
            const auto ability_end = xml.find("</Unit_Abilities_Data>");
            xml.insert(ability_end, refill);
            expect(replace_once(xml, "<Behavior>SELECTABLE</Behavior>",
                "<Behavior>SELECTABLE, TEAM</Behavior>"), "WHE-70: fixture authors TEAM behaviour");
            if (authored_team) expect(replace_once(xml, "<Lua_Script>ObjectScript_Test</Lua_Script>",
                "<Create_Team_Type>Test_Squadron</Create_Team_Type><Lua_Script>ObjectScript_Test</Lua_Script>"),
                "WHE-70: a container-local creation-team reference is not used for refill");
            if (container_class) {
                const auto start = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
                const auto end = xml.find("</SpaceUnit>", start);
                xml.replace(end, std::string_view("</SpaceUnit>").size(), "</Container>");
                xml.replace(start, std::string_view("<SpaceUnit").size(), "<Container");
            }
            write_fixture(tree.root, xml);
            const auto tables = load_layers({tree.root});
            if (!tables) continue;
            const auto* unit = tables->find("Test_Frigate");
            expect(unit && unit->replenish_via_members && unit->replenish_team.empty()
                && unit->abilities.size() == 2 && unit->abilities.back().type == "REPLENISH_WINGMEN",
                "WHE-70: container keeps authored display ability without inventing a team");
            expect(std::none_of(tables->unresolved.begin(), tables->unresolved.end(), [](const auto& row) {
                return row.owner == "Test_Frigate" && row.field == "Create_Team_Type";
            }), "WHE-70: container has no missing creation-team diagnostic");
            expect(std::count_if(tables->notes.begin(), tables->notes.end(), [](const auto& row) {
                return row.owner == "Test_Frigate" && row.reason.find("WHE-70") != std::string::npos;
            }) == 1, "WHE-70: member dispatch emits one load note per type");
            const auto before = eawr::units::content_identity(*tables);
            for (const auto policy : {eawr::units::ContentErrors::strict, eawr::units::ContentErrors::skip_object}) {
                std::vector<eawr::core::Diagnostic> diagnostics;
                const auto result = eawr::units::ability_table(*tables, {}, nullptr, true, policy, &diagnostics);
                expect(result && diagnostics.empty(), "WHE-70: strict and tolerant binding retain the container");
                if (!result) continue;
                expect(ability_of(result.value(), "Test_Frigate", t::AbilityKind::replenish_wingmen) == nullptr
                    && ability_of(result.value(), "Test_Frigate", t::AbilityKind::defend) != nullptr,
                    "WHE-70: no local refill executes and unrelated abilities survive");
            }
            expect(before == eawr::units::content_identity(*tables), "WHE-70: binding preserves authored identity");
            auto changed = *tables;
            for (auto& row : changed.units) if (row.id == "Test_Frigate") row.replenish_via_members = false;
            expect(before != eawr::units::content_identity(changed), "WHE-70: standalone dispatch affects mod identity");
        }
    }
    // The stock shape: display container is temporary metadata; only the leader binds a refill.
    TempTree tree;
    auto xml = with_second_frigate(layer_xml("15", "0.8"));
    xml.insert(xml.find("</Unit_Abilities_Data>"), refill);
    expect(replace_once(xml, "<Lua_Script>ObjectScript_Test</Lua_Script>",
        "<Create_Team_Type>Test_Squadron</Create_Team_Type><Lua_Script>ObjectScript_Test</Lua_Script>"),
        "WHE-63: leader authors its creation team");
    expect(replace_once(xml, "<Container Name=\"Team\">", "<Container Name=\"Team\">"
        "<Unit_Abilities_Data>" + refill + "</Unit_Abilities_Data>"), "WHE-70: display container authors refill");
    write_fixture(tree.root, xml);
    const auto tables = load_layers({tree.root});
    if (!tables) return;
    const auto result = eawr::units::ability_table(*tables);
    const auto* leader = result ? ability_of(result.value(), "Test_Frigate", t::AbilityKind::replenish_wingmen) : nullptr;
    const auto* squadron = tables->find("Test_Squadron");
    expect(leader && leader->replenish_team == eawr::assets::object_type_crc("Test_Squadron")
        && leader->recharge_frames == 1800 && leader->replenish_particle == "test_refill",
        "WHE-63: leader retains its authored team, recharge and effect");
    expect(squadron && !squadron->replenish_via_members && squadron->replenish_team.empty()
        && squadron->team_abilities.size() == 1 && squadron->team_abilities.front().type == "REPLENISH_WINGMEN",
        "WHE-70: stock-shaped squadron retains display data without standalone dispatch metadata");
    const auto bytes = eawr::units::canonical_encoding(*tables);
    const std::string marker = "member-replenishment-v1";
    expect(std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end()) == bytes.end(),
        "WHE-70: stock-shaped loading writes no dispatch extension bytes");
}

void rocket_hero_loading() {
    TempTree tree;
    auto xml = with_second_frigate(layer_xml("15", "0.8"));
    const auto first = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
    const auto last = xml.find("</SpaceUnit>", first);
    xml.replace(last, std::string_view("</SpaceUnit>").size(), "</HeroUnit>");
    xml.replace(first, std::string_view("<SpaceUnit").size(), "<HeroUnit");
    expect(replace_once(xml, "<Type>defend</Type>", "<Type>ROCKET_ATTACK</Type>"
        "<Targeting_Max_Attack_Distance>2500</Targeting_Max_Attack_Distance>"), "WRA-01: standalone hero authors rocket mode");
    write_fixture(tree.root, xml);
    const auto defaults = load_layers({tree.root});
    if (!defaults) return;
    const auto* original = defaults->find("Test_Frigate");
    expect(original && original->xml_type == "HeroUnit" && !original->hardpoints.empty()
        && original->hardpoints.front().weapon && original->hardpoints.front().weapon->fire_in_rocket_mode
        && original->hardpoints.front().weapon->fire_in_normal_mode,
        "WRA-02: standalone hero keeps weapons and omitted mode flags default to Yes");
    auto hardpoints = std::string(hardpoints_xml);
    expect(replace_once(hardpoints, "<Type> HARD_POINT_WEAPON_LASER </Type>",
        "<Type> HARD_POINT_WEAPON_LASER </Type><Fire_When_In_Rocket_Attack_Mode>Yes</Fire_When_In_Rocket_Attack_Mode>"
        "<Fire_When_In_Normal_Attack_Mode>No</Fire_When_In_Normal_Attack_Mode>"), "WRA-02: fixture authors rocket-only weapon");
    write(tree.root / "XML" / "eawr_hardpoints.xml", hardpoints);
    const auto tables = load_layers({tree.root});
    if (!tables) return;
    const auto abilities = eawr::units::ability_table(*tables, std::vector<t::PlayerId>{1});
    const auto combat = eawr::units::combat_table(*tables);
    const auto* rocket = abilities ? ability_of(abilities.value(), "Test_Frigate", t::AbilityKind::rocket_attack) : nullptr;
    expect(rocket && rocket->expiration_frames == 450 && rocket->recharge_frames == 1800 && rocket->supports_autofire
        && rocket->targeting_max_attack_distance == Fixed::from_decimal("2500").value(),
        "WRA-01/03/04: standalone XML hero keeps mode, authored timers, range and autofire in runtime table");
    const auto* profile = combat ? combat.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
    expect(profile && !profile->weapons.empty() && profile->weapons.front().fire_in_rocket_mode
        && !profile->weapons.front().fire_in_normal_mode, "WRA-02: hardpoint mode flags reach combat profile");
    expect(eawr::units::content_identity(*defaults) != eawr::units::content_identity(*tables),
        "WRA-02: weapon mode flags participate in content identity");
    auto changed = *tables;
    for (auto& unit : changed.units) if (unit.id == "Test_Frigate")
        unit.abilities.front().targeting_max_attack_distance = Fixed::from_decimal("2600").value();
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(*tables),
        "WRA-03: active targeting range participates in content identity");
}

void leaf_values_reach_the_table() {
    TempTree base, mid, leaf;
    write_fixture(base.root, with_second_frigate(layer_xml("15", "0.8")));
    write_fixture(mid.root, with_second_frigate(layer_xml("20", "0.5")));
    write_fixture(leaf.root, with_second_frigate(layer_xml("40", "1.5")));
    const auto tables = load_layers({leaf.root, mid.root, base.root});
    if (!tables) return;
    const auto abilities = eawr::units::ability_table(*tables, std::vector<t::PlayerId>{1});
    expect(static_cast<bool>(abilities), "#4666: the layered ability table builds");
    if (!abilities) return;
    for (const auto* type : {"Test_Frigate", "Test_Frigate_Two"}) {
        const auto* defend = ability_of(abilities.value(), type, t::AbilityKind::defend);
        expect(defend != nullptr, std::string("#4666: ") + type + " keeps DEFEND");
        if (defend == nullptr) continue;
        expect(defend->expiration_frames == 40 * t::logical_frames_per_second,
            std::string("#4666: ") + type + " holds the leaf Expiration_Seconds, not the mid or base value");
        expect(defend->modifiers.speed == eawr::units::Fixed::from_decimal("1.5").value(),
            std::string("#4666: ") + type + " holds the leaf SPEED_MULTIPLIER, not the mid or base value");
    }
}

void three_abilities_are_kept() {
    TempTree tree;
    auto xml = with_second_frigate(layer_xml("15", "0.8"));
    const auto ok = replace_once(xml, "<Supports_Autofire>True</Supports_Autofire>",
        "<Supports_Autofire>True</Supports_Autofire></Unit_Ability><Unit_Ability><Type>TURBO</Type>"
        "<Recharge_Seconds>30</Recharge_Seconds></Unit_Ability><Unit_Ability><Type>SPOILER_LOCK</Type>"
        "<Recharge_Seconds>45</Recharge_Seconds>");
    expect(ok, "#4666: the fixture frigate gains two more abilities");
    write_fixture(tree.root, xml);
    const auto tables = load_layers({tree.root});
    if (!tables) return;
    const auto abilities = eawr::units::ability_table(*tables, std::vector<t::PlayerId>{1});
    expect(static_cast<bool>(abilities), "#4666: a type with three abilities builds");
    if (!abilities) return;
    expect(static_cast<bool>(t::validate_abilities(abilities.value())), "#4666: three abilities validate");
    const auto* profile = abilities.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(profile != nullptr && profile->abilities.size() == 3, "#4666: the table keeps all three abilities");
    if (profile == nullptr) return;
    for (const auto kind : {t::AbilityKind::defend, t::AbilityKind::turbo, t::AbilityKind::spoiler_lock})
        expect(t::ability_slot(*profile, kind).has_value(), "#4666: each authored kind has a slot");
    expect(t::initial_abilities(*profile, true).slots.size() == 3, "#4666: the session state holds a slot per ability");

    // The same kind twice stays refused: slots are found by kind.
    auto twice = *profile;
    twice.abilities.push_back(twice.abilities.front());
    expect(!t::validate_ability_profile(twice), "#4666: two abilities of one kind are refused");
}

void bad_entry_skips_only_its_type() {
    TempTree tree;
    write_fixture(tree.root, with_second_frigate(layer_xml("15", "0.8")));
    auto tables = load_layers({tree.root});
    if (!tables) return;
    // Out of bounds (|multiplier| <= 64), as an authored Eclipse-style value is for its own type (#4629).
    const auto bad = std::find_if(tables->units.begin(), tables->units.end(), [](const auto& unit) { return unit.id == "Test_Frigate"; });
    expect(bad != tables->units.end() && !bad->abilities.empty(), "#4666: the frigate authors an ability to break");
    if (bad == tables->units.end() || bad->abilities.empty()) return;
    for (auto& modifier : bad->abilities.front().modifiers) modifier.value = eawr::units::Fixed::from_raw(raw(1000));

    expect(!eawr::units::ability_table(*tables, std::vector<t::PlayerId>{1}),
        "#4666: strict still refuses the table on one bad ability entry");
    std::vector<eawr::core::Diagnostic> skipped;
    const auto lenient = eawr::units::ability_table(*tables, std::vector<t::PlayerId>{1}, nullptr, true,
        eawr::units::ContentErrors::skip_object, &skipped);
    expect(static_cast<bool>(lenient), "#4666: skip_object builds the rest of the table");
    if (!lenient) return;
    expect(skipped.size() == 1 && skipped.front().message.find("skipped unit type 'Test_Frigate'") != std::string::npos,
        "#4666: the report names the one skipped type");
    expect(lenient.value().find(eawr::assets::object_type_crc("Test_Frigate")) == nullptr,
        "#4666: the bad type holds no abilities");
    expect(ability_of(lenient.value(), "Test_Frigate_Two", t::AbilityKind::defend) != nullptr,
        "#4666: the other type keeps its abilities");
    expect(static_cast<bool>(t::validate_abilities(lenient.value())), "#4666: the lenient table validates");
}

// #5696: a bare SELF_DESTRUCT row (no Expiration_Seconds) never starts in retail (WSD-01): the type loads
// without the row and says so; strict keeps refusing it.
void bare_stub_rows_are_inert() {
    for (const auto kind : {"SELF_DESTRUCT"}) {
        TempTree tree;
        auto xml = with_second_frigate(layer_xml("15", "0.8"));
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        xml.replace(begin, end + std::string_view("</Unit_Abilities_Data>").size() - begin,
            "<Unit_Abilities_Data><Unit_Ability><Type>" + std::string(kind) + "</Type></Unit_Ability>"
            "<Unit_Ability><Type>TURBO</Type><Recharge_Seconds>1</Recharge_Seconds></Unit_Ability></Unit_Abilities_Data>");
        write_fixture(tree.root, xml);
        const auto tables = load_layers({tree.root});
        expect(tables.has_value(), "#5696: the stub row fixture loads");
        if (!tables) continue;
        expect(!eawr::units::ability_table(*tables, {}, nullptr, true, eawr::units::ContentErrors::strict),
            "#5696: strict still refuses a row with no expiration");
        std::vector<eawr::core::Diagnostic> diagnostics;
        const auto lenient = eawr::units::ability_table(*tables, {}, nullptr, true,
            eawr::units::ContentErrors::skip_object, &diagnostics);
        expect(static_cast<bool>(lenient), "#5696: a lenient load builds the table");
        if (!lenient) continue;
        expect(ability_of(lenient.value(), "Test_Frigate", t::ability_kind(kind)) == nullptr,
            "#5696: the inert row is dropped");
        expect(ability_of(lenient.value(), "Test_Frigate", t::AbilityKind::turbo) != nullptr,
            "#5696: the type keeps its other abilities");
        expect(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& row) {
            return row.severity == eawr::core::Severity::warning && row.message.find("Expiration_Seconds") != std::string::npos;
        }) && std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& row) {
            return row.message.find("skipped unit type") != std::string::npos;
        }), "#5696: a load note, never a type skip");
        expect(static_cast<bool>(t::validate_abilities(lenient.value())), "#5696: the lenient table validates");
    }
}

bool has_warning(const std::vector<eawr::core::Diagnostic>& rows, const std::string_view text) {
    return std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
        return row.severity == eawr::core::Severity::warning && row.message.find(text) != std::string::npos;
    });
}

bool has_type_skip(const std::vector<eawr::core::Diagnostic>& rows) {
    return std::any_of(rows.begin(), rows.end(), [](const auto& row) {
        return row.message.find("skipped unit type") != std::string::npos;
    });
}

// #5696: a SELF_DESTRUCT naming an object that is no area blast (the stock contamination objects name the particle
// Small_Steam_Ground; effective miscobjects.xml l.75-87, l.149-157) keeps the ability and creates nothing; strict refuses.
void non_blast_self_destruct_is_kept() {
    TempTree tree;
    auto xml = with_second_frigate(layer_xml("15", "0.8"));
    const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
    const auto begin = xml.find("<Unit_Abilities_Data", ship);
    const auto end = xml.find("</Unit_Abilities_Data>", begin);
    xml.replace(begin, end + std::string_view("</Unit_Abilities_Data>").size() - begin,
        "<Unit_Abilities_Data><Unit_Ability><Type>SELF_DESTRUCT</Type><Expiration_Seconds>3</Expiration_Seconds>"
        "<Spawned_Object_Type>Proj_Test_Turbo</Spawned_Object_Type></Unit_Ability>"
        "<Unit_Ability><Type>TURBO</Type><Recharge_Seconds>1</Recharge_Seconds></Unit_Ability></Unit_Abilities_Data>");
    write_fixture(tree.root, xml);
    const auto tables = load_layers({tree.root});
    expect(tables.has_value(), "#5696: the non-blast self destruct fixture loads");
    if (!tables) return;
    const auto strict = eawr::units::ability_table(*tables, {}, nullptr, true, eawr::units::ContentErrors::strict);
    expect(!strict || !t::validate_abilities(strict.value()), "#5696: strict still refuses a non-blast self destruct object");
    std::vector<eawr::core::Diagnostic> diagnostics;
    const auto lenient = eawr::units::ability_table(*tables, {}, nullptr, true,
        eawr::units::ContentErrors::skip_object, &diagnostics);
    expect(lenient.has_value(), "#5696: a lenient load builds the table");
    if (!lenient) return;
    const auto* ability = ability_of(lenient.value(), "Test_Frigate", t::AbilityKind::self_destruct);
    expect(ability && !ability->spawned && ability->unresolved_spawn && *ability->unresolved_spawn == "Proj_Test_Turbo",
        "#5696: the self destruct is kept with its creation skipped");
    expect(ability_of(lenient.value(), "Test_Frigate", t::AbilityKind::turbo) != nullptr, "#5696: other abilities stay");
    expect(has_warning(diagnostics, "no area-blast projectile") && !has_type_skip(diagnostics),
        "#5696: a load note, never a type skip");
    expect(static_cast<bool>(t::validate_abilities(lenient.value())), "#5696: the lenient table validates");
}

// #5704: retail multiplies SPEED_MULTIPLIER unchecked, so an authored 0.0 (a mod freezes the unit) loads, even strict.
void zero_speed_multiplier_loads() {
    TempTree tree;
    write_fixture(tree.root, with_second_frigate(layer_xml("15", "0.0")));
    const auto tables = load_layers({tree.root});
    expect(tables.has_value(), "#5704: the zero speed multiplier fixture loads");
    if (!tables) return;
    const auto abilities = eawr::units::ability_table(*tables, std::vector<t::PlayerId>{1});
    expect(static_cast<bool>(abilities), "#5704: a zero speed multiplier no longer refuses the table");
    if (!abilities) return;
    const auto* defend = ability_of(abilities.value(), "Test_Frigate", t::AbilityKind::defend);
    expect(defend != nullptr && defend->modifiers.speed.raw() == 0, "#5704: the zero multiplier is kept as authored");
}

void concentrate_fire_large_damage_loads() {
    TempTree tree;
    auto xml = with_second_frigate(layer_xml("15", "0.8"));
    const auto begin = xml.find("<Unit_Abilities_Data");
    const auto end = xml.find("</Unit_Abilities_Data>", begin);
    expect(begin != std::string::npos && end != std::string::npos, "#5704: ability fixture exists");
    if (begin == std::string::npos || end == std::string::npos) return;
    xml.replace(begin, end + std::string_view("</Unit_Abilities_Data>").size() - begin, R"(
      <Unit_Abilities_Data><Unit_Ability><Type>CONCENTRATE_FIRE</Type><Effective_Radius>6000</Effective_Radius>
        <Expiration_Seconds>15</Expiration_Seconds><Recharge_Seconds>20</Recharge_Seconds>
        <GUI_Activated_Ability_Name>focus</GUI_Activated_Ability_Name></Unit_Ability></Unit_Abilities_Data>
      <Abilities><Concentrate_Fire_Attack_Ability Name="focus"><Activation_Style>User_Input</Activation_Style>
        <Applicable_Unit_Categories>Frigate</Applicable_Unit_Categories><Applicable_Unit_Types/>
        <Stacking_Category>0</Stacking_Category><Target_Damage_Increase_Percent>5</Target_Damage_Increase_Percent>
        <Target_Speed_Decrease_Percent>0</Target_Speed_Decrease_Percent>
      </Concentrate_Fire_Attack_Ability></Abilities>)");
    write_fixture(tree.root, xml);
    const auto tables = load_layers({tree.root});
    if (!tables) return;
    const auto abilities = eawr::units::ability_table(*tables);
    const auto* profile = abilities ? abilities.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
    expect(profile && !profile->special.empty() && profile->special.front().target_damage_increase == Fixed::from_decimal("5").value(),
        "#5704: concentrate-fire damage above four is retained");
}

void oversized_collision_loads_with_warning() {
    TempTree tree;
    write_fixture(tree.root, with_second_frigate(layer_xml("15", "0.8")));
    auto tables = load_layers({tree.root});
    if (!tables) return;
    auto unit = std::find_if(tables->units.begin(), tables->units.end(), [](const auto& row) { return row.id == "Test_Frigate"; });
    expect(unit != tables->units.end(), "#5704: collision fixture has a frigate");
    if (unit == tables->units.end()) return;
    unit->scale_factor = Fixed::from_raw(Fixed::scale);
    unit->collision = eawr::units::CollisionBounds{point(raw(-300000), raw(-1), raw(-1)), point(raw(300000), raw(1), raw(1))};
    unit->collision_meshes.clear();
    std::vector<eawr::core::Diagnostic> diagnostics;
    const auto combat = eawr::units::combat_table(*tables, eawr::units::ContentErrors::skip_object, &diagnostics);
    const auto* profile = combat ? combat.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
    expect(profile && !profile->collision && profile->meshes.empty() && !profile->mesh_bounds,
        "#5704: oversized collision geometry is omitted while the type remains");
    expect(has_warning(diagnostics, "collision box exceeds") && !has_type_skip(diagnostics),
        "#5704: oversized collision produces a warning without skipping the type");
}

// #5696: retail reads Projectile_Max_Lifetime unchecked; a lenient load keeps a 5000 s projectile, strict keeps 3600 s.
void long_projectile_lifetime_loads_leniently() {
    TempTree tree;
    auto xml = with_second_frigate(layer_xml("15", "0.8"));
    expect(replace_once(xml, "<Max_Speed>25.0</Max_Speed>",
        "<Max_Speed>25.0</Max_Speed><Projectile_Max_Lifetime>5000</Projectile_Max_Lifetime>"),
        "#5696: the fixture authors a 5000 s lifetime on the frigate's projectile");
    write_fixture(tree.root, xml);
    const auto tables = load_layers({tree.root});
    expect(tables.has_value(), "#5696: the long lifetime fixture loads");
    if (!tables) return;
    expect(!eawr::units::combat_table(*tables, eawr::units::ContentErrors::strict),
        "#5696: strict still refuses a lifetime above 3600 s");
    std::vector<eawr::core::Diagnostic> diagnostics;
    const auto lenient = eawr::units::combat_table(*tables, eawr::units::ContentErrors::skip_object, &diagnostics);
    expect(lenient.has_value() && !has_type_skip(diagnostics), "#5696: a lenient load keeps the type");
    if (!lenient) return;
    const auto* profile = lenient.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(profile != nullptr && !profile->weapons.empty(), "#5696: the frigate's weapons load");
    bool kept = false;
    if (profile != nullptr) for (const auto& weapon : profile->weapons)
        if (weapon.shot && weapon.shot->flight && weapon.shot->flight->lifetime
            && weapon.shot->flight->lifetime->raw() == 5000 * eawr::sim::math::Fixed::scale) kept = true;
    expect(kept, "#5696: the 5000 s lifetime is kept as authored");
}

// #5696 + WAR-108: a Death_Projectiles name that answers no projectile is dropped with a note in a lenient load, but
// the death draw still covers every authored entry; the table records the authored count and kept slots.
void unresolved_death_projectile_is_dropped() {
    for (const bool first : {false, true}) {
        TempTree tree;
        auto xml = with_second_frigate(layer_xml("15", "0.8"));
        const std::string_view marker = "<SpaceUnit Name=\"Test_Frigate\">";
        xml.insert(xml.find(marker) + marker.size(), first
            ? "<Death_Projectiles>Missing_Test_Projectile, Proj_Test_Small</Death_Projectiles>"
            : "<Death_Projectiles>Proj_Test_Small, Missing_Test_Projectile</Death_Projectiles>");
        write_fixture(tree.root, xml);
        const auto tables = load_layers({tree.root});
        expect(tables.has_value(), "#5696: the death projectile fixture loads");
        if (!tables) continue;
        expect(!eawr::units::combat_table(*tables, eawr::units::ContentErrors::strict),
            "#5696: strict still refuses an unresolved Death_Projectiles entry");
        std::vector<eawr::core::Diagnostic> diagnostics;
        const auto lenient = eawr::units::combat_table(*tables, eawr::units::ContentErrors::skip_object, &diagnostics);
        expect(lenient.has_value() && !has_type_skip(diagnostics) && has_warning(diagnostics, "Death_Projectiles"),
            "#5696: a lenient load keeps the type and notes the dropped entry");
        if (!lenient) continue;
        const auto* profile = lenient.value().find(eawr::assets::object_type_crc("Test_Frigate"));
        expect(profile && profile->death_projectiles.size() == 1 && profile->death_projectile_authored == 2
            && profile->death_projectile_slots == std::vector<std::uint32_t>{first ? 1U : 0U},
            "WAR-108: the draw still covers both authored entries and the kept one remembers its slot");
    }
    // A fully resolved list keeps the plain draw over its size (stock content is unchanged).
    TempTree tree;
    auto xml = with_second_frigate(layer_xml("15", "0.8"));
    const std::string_view marker = "<SpaceUnit Name=\"Test_Frigate\">";
    xml.insert(xml.find(marker) + marker.size(), "<Death_Projectiles>Proj_Test_Small, Proj_Test_Turbo</Death_Projectiles>");
    write_fixture(tree.root, xml);
    const auto tables = load_layers({tree.root});
    expect(tables.has_value(), "#5696: the resolved death projectile fixture loads");
    if (!tables) return;
    std::vector<eawr::core::Diagnostic> diagnostics;
    const auto lenient = eawr::units::combat_table(*tables, eawr::units::ContentErrors::skip_object, &diagnostics);
    const auto* profile = lenient ? lenient.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
    expect(profile && profile->death_projectiles.size() == 2 && profile->death_projectile_authored == 0
        && profile->death_projectile_slots.empty(), "WAR-108: a fully resolved list is unchanged");
}

// Replaces the frigate's ability block with `abilities` (and its nested handlers `handlers`, if any).
std::string with_abilities(std::string xml, const std::string_view abilities, const std::string_view handlers = {}) {
    const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
    const auto begin = xml.find("<Unit_Abilities_Data", ship);
    const auto end = xml.find("</Unit_Abilities_Data>", begin);
    expect(ship != std::string::npos && begin != std::string::npos && end != std::string::npos,
        "#5704: the fixture has a frigate ability block");
    xml.replace(begin, end + std::string_view("</Unit_Abilities_Data>").size() - begin,
        std::string(handlers) + "<Unit_Abilities_Data>" + std::string(abilities) + "</Unit_Abilities_Data>");
    return xml;
}

// WSPL-01/03/06: load the nested handler, bind it to the unit ability, and preserve retail colour defaults.
void super_laser_loading() {
    for (const bool authored_colour : {false, true}) {
        TempTree tree;
        const std::string colour = authored_colour ? "<Line_Color>12,34,56,78</Line_Color>" : "<Beam_Color>1,2,3,4</Beam_Color>";
        const std::string handler = "<Abilities SubObjectList=\"Yes\"><Super_Laser_Ability Name=\"Test_Beam\">"
            "<Activation_Style>User_Input</Activation_Style><Activation_Max_Range>300</Activation_Max_Range>"
            "<Damage_Per_Frame>20</Damage_Per_Frame><Fire_Delay>0.15</Fire_Delay>" + colour
            + "</Super_Laser_Ability></Abilities>";
        write_fixture(tree.root, with_second_frigate(with_abilities(layer_xml("15", "0.8"),
            "<Unit_Ability><Type>SUPER_LASER</Type><GUI_Activated_Ability_Name>Test_Beam</GUI_Activated_Ability_Name>"
            "<Expiration_Seconds>2</Expiration_Seconds><Recharge_Seconds>4</Recharge_Seconds></Unit_Ability>", handler)));
        const auto tables = load_layers({tree.root});
        if (!tables) continue;
        const auto abilities = eawr::units::ability_table(*tables, std::vector<t::PlayerId>{1});
        expect(static_cast<bool>(abilities), "WSPL-01: authored super laser binds");
        if (!abilities) continue;
        const auto* profile = abilities.value().find(eawr::assets::object_type_crc("Test_Frigate"));
        expect(profile && t::ability_slot(*profile, t::AbilityKind::super_laser).has_value(), "WSPL-01: ability kind loads");
        if (!profile || profile->special.empty()) continue;
        const auto& special = profile->special.front();
        expect(special.kind == t::SpecialAbilityKind::super_laser && special.fire_delay_frames == 4,
            "WSPL-03: delay seconds convert to truncated logical frames");
        const std::array<std::uint8_t, 4> expected = authored_colour ? std::array<std::uint8_t, 4>{12, 34, 56, 78}
                                                                     : std::array<std::uint8_t, 4>{255, 255, 255, 255};
        expect(special.leech_colour == expected, "WSPL-06: Line_Color applies and Beam_Color does not override white");
    }
}

// WSH-04 (#5696): Heal_Amount below 0 together with a Heal_Percent loads leniently (retail adds percent x maximum and
// amount); strict keeps refusing the combination.
void healing_negative_amount_with_percent() {
    TempTree tree;
    write_fixture(tree.root, with_second_frigate(with_abilities(layer_xml("15", "0.8"),
        "",
        "<Abilities SubObjectList=\"Yes\"><Force_Healing_Ability Name=\"Test_Heal\"><Activation_Style>Space_Automatic</Activation_Style>"
        "<Heal_Range>100.0</Heal_Range><Heal_Amount>-0.5</Heal_Amount><Heal_Percent>0.005</Heal_Percent>"
        "<Heal_Interval_In_Secs>0.1</Heal_Interval_In_Secs><Applicable_Unit_Categories>Corvette, Frigate, Capital</Applicable_Unit_Categories>"
        "</Force_Healing_Ability></Abilities>")));
    const auto tables = load_layers({tree.root});
    expect(tables.has_value(), "WSH-04: the healing fixture loads");
    if (!tables) return;
    const auto strict = eawr::units::ability_table(*tables, {}, nullptr, true, eawr::units::ContentErrors::strict);
    expect(!strict || !t::validate_abilities(strict.value()), "WSH-09: strict still refuses a negative amount with a percent");
    std::vector<eawr::core::Diagnostic> diagnostics;
    const auto lenient = eawr::units::ability_table(*tables, {}, nullptr, true,
        eawr::units::ContentErrors::skip_object, &diagnostics);
    expect(lenient.has_value() && !has_type_skip(diagnostics), "WSH-04: a lenient load keeps the type");
    if (!lenient) return;
    const auto* profile = lenient.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    bool heal = false;
    if (profile != nullptr) for (const auto& special : profile->special)
        if (special.kind == t::SpecialAbilityKind::force_healing)
            heal = special.heal_amount.raw() < 0 && special.heal_percent.raw() > 0;
    expect(heal, "WSH-04: the negative amount and the percent both stay");
    expect(static_cast<bool>(t::validate_abilities(lenient.value())), "WSH-04: the table validates");
}

// WAR-108 (#5696): when every Death_Projectiles entry is dropped the table keeps no projectile, only the authored count;
// the death service must read nothing from the empty list.
void all_death_projectiles_dropped() {
    TempTree tree;
    auto xml = with_second_frigate(layer_xml("15", "0.8"));
    const std::string_view marker = "<SpaceUnit Name=\"Test_Frigate\">";
    xml.insert(xml.find(marker) + marker.size(), "<Death_Projectiles>Missing_Test_A, Missing_Test_B</Death_Projectiles>");
    write_fixture(tree.root, xml);
    const auto tables = load_layers({tree.root});
    expect(tables.has_value(), "#5696: the all-dropped fixture loads");
    if (!tables) return;
    std::vector<eawr::core::Diagnostic> diagnostics;
    const auto lenient = eawr::units::combat_table(*tables, eawr::units::ContentErrors::skip_object, &diagnostics);
    expect(lenient.has_value() && !has_type_skip(diagnostics), "#5696: a lenient load keeps the type");
    if (!lenient) return;
    const auto* profile = lenient.value().find(eawr::assets::object_type_crc("Test_Frigate"));
    expect(profile && profile->death_projectiles.empty() && profile->death_projectile_authored == 2
        && profile->death_projectile_slots.empty(),
        "WAR-108: no projectile kept, authored count 2; the death service gates on the authored count, so nothing spawns");
}

// #5696 rule: Retreat_Self_Destruct_Explosion naming no projectile (FotR templates, 71 types) has no retail load check;
// strict refuses, a lenient load keeps the type with a note and no retreat blast. A resolved name still binds.
void unresolved_retreat_explosion_is_dropped() {
    for (const bool resolved : {false, true}) {
        TempTree tree;
        auto xml = with_second_frigate(layer_xml("15", "0.8"));
        const std::string_view marker = "<SpaceUnit Name=\"Test_Frigate\">";
        xml.insert(xml.find(marker) + marker.size(), resolved
            ? "<Retreat_Self_Destruct_Explosion>Proj_Test_Small</Retreat_Self_Destruct_Explosion>"
            : "<Retreat_Self_Destruct_Explosion>Missing_Test_Projectile</Retreat_Self_Destruct_Explosion>");
        write_fixture(tree.root, xml);
        const auto tables = load_layers({tree.root});
        expect(tables.has_value(), "#5696: the retreat explosion fixture loads");
        if (!tables) continue;
        if (!resolved)
            expect(!eawr::units::combat_table(*tables, eawr::units::ContentErrors::strict),
                "#5696: strict still refuses an unresolved Retreat_Self_Destruct_Explosion");
        std::vector<eawr::core::Diagnostic> diagnostics;
        const auto lenient = eawr::units::combat_table(*tables, eawr::units::ContentErrors::skip_object, &diagnostics);
        expect(lenient.has_value() && !has_type_skip(diagnostics), "#5696: a lenient load keeps the type");
        if (!lenient) continue;
        const auto* profile = lenient.value().find(eawr::assets::object_type_crc("Test_Frigate"));
        expect(profile && profile->retreat_explosion.has_value() == resolved
            && has_warning(diagnostics, "Retreat_Self_Destruct_Explosion") == !resolved,
            "#5696: the retreat blast binds only when the name resolves; otherwise a load note");
    }
}

// #5724: Proj_Ancient_Superlaser_Strong authors Projectile_Damage 9999999 (above the sim's hit damage bound). Strict still
// refuses; a lenient load keeps the type, saturates the damage at the bound and notes it.
void oversized_projectile_damage_saturates() {
    TempTree tree;
    auto xml = with_second_frigate(layer_xml("15", "0.8"));
    const auto projectile = xml.find("<Projectile Name=\"Proj_Test_Small\">");
    const std::string_view authored = "<Projectile_Damage>5.0</Projectile_Damage>";
    const auto at = xml.find(authored, projectile);
    expect(projectile != std::string::npos && at != std::string::npos, "#5724: the fixture projectile is found");
    if (at == std::string::npos) return;
    xml.replace(at, authored.size(), "<Projectile_Damage>9999999.0</Projectile_Damage>");
    write_fixture(tree.root, xml);
    const auto tables = load_layers({tree.root});
    expect(tables.has_value(), "#5724: the oversized damage fixture loads");
    if (!tables) return;
    expect(!eawr::units::combat_table(*tables, eawr::units::ContentErrors::strict),
        "#5724: strict still refuses a hit damage above the bound");
    std::vector<eawr::core::Diagnostic> diagnostics;
    const auto lenient = eawr::units::combat_table(*tables, eawr::units::ContentErrors::skip_object, &diagnostics);
    expect(lenient.has_value() && !has_type_skip(diagnostics), "#5724: a lenient load keeps the type");
    expect(has_warning(diagnostics, "Projectile_Damage"), "#5724: the saturation is noted");
}

// #5702: a sub-frame Expiration_Seconds (0.01) is an authored expiration above 0: the row stays, one frame long.
void sub_frame_expiration_is_one_frame() {
    for (const auto kind : {"SELF_DESTRUCT", "BARRAGE"}) {
        TempTree tree;
        auto xml = with_second_frigate(layer_xml("15", "0.8"));
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        xml.replace(begin, end + std::string_view("</Unit_Abilities_Data>").size() - begin,
            "<Unit_Abilities_Data><Unit_Ability><Type>" + std::string(kind)
            + "</Type><Expiration_Seconds>0.01</Expiration_Seconds></Unit_Ability></Unit_Abilities_Data>");
        write_fixture(tree.root, xml);
        const auto tables = load_layers({tree.root});
        expect(static_cast<bool>(tables), "#5702: the fixture loads");
        if (!tables) continue;
        const auto lenient = eawr::units::ability_table(*tables, {}, nullptr, true, eawr::units::ContentErrors::skip_object);
        expect(static_cast<bool>(lenient), "#5702: a lenient load builds the table");
        if (!lenient) continue;
        const auto* row = ability_of(lenient.value(), "Test_Frigate", t::ability_kind(kind));
        expect(row != nullptr && row->expiration_frames == 1, "#5702: a 0.01 s expiration is one frame, not dropped");
        expect(static_cast<bool>(t::validate_abilities(lenient.value())), "#5702: the table validates");
    }
}

} // namespace

void ability_limits() {
    container_replenishment_loading();
    rocket_hero_loading();
    // WHE-61: unknown and omitted references retain the ability and its sibling in both modes.
    for (const auto kind : {"HARMONIC_BOMB", "WEAKEN_ENEMY", "SELF_DESTRUCT", "FOW_REVEAL_PING"}) {
        for (const bool omitted : {false, true}) {
            TempTree tree;
            auto xml = with_second_frigate(layer_xml("15", "0.8"));
            const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
            const auto begin = xml.find("<Unit_Abilities_Data", ship);
            const auto end = xml.find("</Unit_Abilities_Data>", begin);
            xml.replace(begin, end + std::string_view("</Unit_Abilities_Data>").size() - begin,
                "<Unit_Abilities_Data><Unit_Ability><Type>" + std::string(kind) + "</Type>"
                + (omitted ? "" : "<Spawned_Object_Type>Missing_Test_Object</Spawned_Object_Type>")
                + "<Expiration_Seconds>3</Expiration_Seconds><Recharge_Seconds>20</Recharge_Seconds>"
                  "</Unit_Ability><Unit_Ability><Type>TURBO</Type><Recharge_Seconds>1</Recharge_Seconds>"
                  "</Unit_Ability></Unit_Abilities_Data>");
            write_fixture(tree.root, xml);
            const auto tables = load_layers({tree.root});
            if (!tables) continue;
            const auto before = eawr::units::content_identity(*tables);
            for (const auto policy : {eawr::units::ContentErrors::strict, eawr::units::ContentErrors::skip_object}) {
                std::vector<eawr::core::Diagnostic> diagnostics;
                const auto result = eawr::units::ability_table(*tables, {}, nullptr, true, policy, &diagnostics);
                expect(static_cast<bool>(result), "WHE-61: null reference loads in both policies");
                if (!result) continue;
                const auto* ability = ability_of(result.value(), "Test_Frigate", t::ability_kind(kind));
                expect(ability && !ability->spawned && ability->unresolved_spawn
                    && *ability->unresolved_spawn == (omitted ? "" : "Missing_Test_Object"),
                    "WHE-61: explicit unresolved state retains the authored reference");
                expect(ability_of(result.value(), "Test_Frigate", t::AbilityKind::turbo) != nullptr,
                    "WHE-61: an unrelated ability survives");
                expect(!diagnostics.empty() && std::all_of(diagnostics.begin(), diagnostics.end(), [](const auto& row) {
                    return row.severity == eawr::core::Severity::warning
                        && row.message.find("Spawned_Object_Type") != std::string::npos
                        && row.message.find("skipped unit type") == std::string::npos;
                }), "WHE-61: recoverable diagnostics are warnings, never type skips");
                expect(before == eawr::units::content_identity(*tables), "WHE-61: binding never mutates authored identity");
            }
            const auto bytes = eawr::units::canonical_encoding(*tables);
            const std::string marker = "unresolved-ability-spawn-v1";
            expect(std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end()) != bytes.end(),
                "WHE-61: unresolved semantic state is explicitly serialized into content identity");
            auto changed = *tables;
            for (auto& unit : changed.units) if (unit.id == "Test_Frigate")
                unit.abilities.front().spawned_object = "Another_Missing_Object";
            expect(before != eawr::units::content_identity(changed), "WHE-61: distinct missing references change identity");
        }
    }
    // WHE-58: XML values above the former ceiling remain authored distances.
    for (const auto range : {"65535", "65536", "65537", "500000"}) {
        TempTree tree;
        auto xml = with_second_frigate(layer_xml("15", "0.8"));
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        xml.replace(begin, end + std::string_view("</Unit_Abilities_Data>").size() - begin,
            "<Abilities><Energy_Weapon_Attack_Ability Name=\"Test_Beam\"><Activation_Style>User_Input</Activation_Style>"
            "<Activation_Max_Range>" + std::string(range) + "</Activation_Max_Range>"
            "<Activation_Min_Range>" + std::string(range) + "</Activation_Min_Range>"
            "</Energy_Weapon_Attack_Ability></Abilities><Unit_Abilities_Data><Unit_Ability><Type>ENERGY_WEAPON</Type>"
            "<GUI_Activated_Ability_Name>Test_Beam</GUI_Activated_Ability_Name></Unit_Ability></Unit_Abilities_Data>");
        write_fixture(tree.root, xml);
        const auto tables = load_layers({tree.root});
        if (!tables) continue;
        for (const auto policy : {eawr::units::ContentErrors::strict, eawr::units::ContentErrors::skip_object}) {
            std::vector<eawr::core::Diagnostic> diagnostics;
            const auto result = eawr::units::ability_table(*tables, {}, nullptr, true, policy, &diagnostics);
            expect(static_cast<bool>(result) && diagnostics.empty(), "WHE-58: large authored range never omits a type");
            const auto* profile = result ? result.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
            expect(profile && !profile->special.empty() && profile->special.front().beam_max_range
                == eawr::units::Fixed::from_decimal(range).value()
                && profile->special.front().beam_min_range == eawr::units::Fixed::from_decimal(range).value(),
                "WHE-58: authored ranges are unchanged");
        }
    }

    {
        TempTree tree;
        write_fixture(tree.root, with_second_frigate(layer_xml("15", "0.8")));
        const auto tables = load_layers({tree.root});
        if (tables) {
            const auto bytes = eawr::units::canonical_encoding(*tables);
            const std::string marker = "unresolved-ability-spawn-v1";
            expect(std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end()) == bytes.end(),
                "WHE-61: supported content serializes no null-spawn extension bytes");
        }
    }
    // AB-22/AB-23 (#4868): an authored regen interval multiplier of 0 loads; a negative one is still refused.
    for (const auto authored : {"0", "-0.5"}) {
        TempTree tree;
        auto xml = with_second_frigate(layer_xml("15", "0.8"));
        expect(replace_once(xml, "<Mod_Multiplier>SPEED_MULTIPLIER,",
            std::string("<Mod_Multiplier>SHIELD_REGEN_INTERVAL_MULTIPLIER, ") + authored
            + "</Mod_Multiplier><Mod_Multiplier>ENERGY_REGEN_INTERVAL_MULTIPLIER, " + authored
            + "</Mod_Multiplier><Mod_Multiplier>SPEED_MULTIPLIER,"), "AB-22: the fixture authors interval multipliers");
        write_fixture(tree.root, xml);
        const auto tables = load_layers({tree.root});
        if (!tables) continue;
        const bool zero = std::string_view(authored) == "0";
        const auto strict = eawr::units::ability_table(*tables, std::vector<t::PlayerId>{1});
        if (!zero) {
            expect(!strict, "AB-22: a negative interval multiplier is still refused");
            continue;
        }
        expect(static_cast<bool>(strict) && static_cast<bool>(t::validate_abilities(strict.value())),
            "AB-22: a zero interval multiplier loads and validates");
        if (!strict) continue;
        const auto* defend = ability_of(strict.value(), "Test_Frigate", t::AbilityKind::defend);
        expect(defend != nullptr && defend->modifiers.shield_regen_interval.raw() == 0
            && defend->modifiers.energy_regen_interval.raw() == 0, "AB-22: the authored 0 reaches the profile unchanged");
    }
    // WSH-09 (#4868): a negative Heal_Amount on an automatic healer loads (it is passive damage in retail) in both
    // policies; a negative Heal_Percent is reset to 0 as retail does.
    for (const auto policy : {eawr::units::ContentErrors::strict, eawr::units::ContentErrors::skip_object}) {
        TempTree tree;
        auto xml = with_second_frigate(layer_xml("15", "0.8"));
        const auto ship = xml.find("<SpaceUnit Name=\"Test_Frigate\">");
        const auto begin = xml.find("<Unit_Abilities_Data", ship);
        const auto end = xml.find("</Unit_Abilities_Data>", begin);
        xml.replace(begin, end + std::string_view("</Unit_Abilities_Data>").size() - begin,
            "<Abilities SubObjectList=\"Yes\"><Force_Healing_Ability Name=\"Test_Aura\">"
            "<Activation_Style>Space_Automatic</Activation_Style><Heal_Range>1000</Heal_Range>"
            "<Heal_Amount>-30</Heal_Amount><Heal_Percent>-0.5</Heal_Percent>"
            "<Heal_Interval_In_Secs>0.5</Heal_Interval_In_Secs>"
            "<Applicable_Unit_Categories>Corvette, Frigate, Capital</Applicable_Unit_Categories>"
            "</Force_Healing_Ability></Abilities>");
        write_fixture(tree.root, xml);
        const auto tables = load_layers({tree.root});
        if (!tables) continue;
        std::vector<eawr::core::Diagnostic> diagnostics;
        const auto result = eawr::units::ability_table(*tables, {}, nullptr, true, policy, &diagnostics);
        expect(static_cast<bool>(result) && static_cast<bool>(t::validate_abilities(result.value())),
            "WSH-09: a negative Heal_Amount loads and validates");
        // The fixture's damage-type list has no Damage_Force_Corruption: one warning, never a type skip.
        const auto missing_type = std::count_if(diagnostics.begin(), diagnostics.end(), [](const auto& row) {
            return row.message.find("Damage_Force_Corruption") != std::string::npos
                && row.severity == eawr::core::Severity::warning && row.message.find("skipped unit type") == std::string::npos;
        });
        expect(missing_type == 1 && diagnostics.size() == 1, "WSH-09: a missing force-corruption damage type warns once");
        expect(result && result.value().passive_damage_type == t::no_type_index, "WSH-09: the missing row stays unbound");
        const auto* profile = result ? result.value().find(eawr::assets::object_type_crc("Test_Frigate")) : nullptr;
        expect(profile && !profile->special.empty() && profile->special.front().kind == t::SpecialAbilityKind::force_healing
            && profile->special.front().heal_amount == eawr::units::Fixed::from_decimal("-30").value()
            && profile->special.front().heal_percent.raw() == 0 && profile->special.front().heal_interval_frames == 15,
            "WSH-09: the signed amount reaches the profile; a negative percent resets to 0");
    }
    super_laser_loading();
    leaf_values_reach_the_table();
    three_abilities_are_kept();
    bad_entry_skips_only_its_type();
    bare_stub_rows_are_inert();
    non_blast_self_destruct_is_kept();
    zero_speed_multiplier_loads();
    concentrate_fire_large_damage_loads();
    oversized_collision_loads_with_warning();
    long_projectile_lifetime_loads_leniently();
    unresolved_death_projectile_is_dropped();
    healing_negative_amount_with_percent();
    all_death_projectiles_dropped();
    unresolved_retreat_explosion_is_dropped();
    oversized_projectile_damage_saturates();
    sub_frame_expiration_is_one_frame();
}

} // namespace unit_tables_test_support
