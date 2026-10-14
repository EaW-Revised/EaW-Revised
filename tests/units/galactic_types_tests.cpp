#include "eawr/units/galactic_types.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>

namespace {
using eawr::sim::math::Fixed;
int failures = 0;
void expect(const bool value, const std::string_view message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void override_free_identity() {
    eawr::units::GalacticType unchanged;
    unchanged.name = "Unchanged"; unchanged.object_class = "SpaceUnit";
    eawr::units::GalacticTypeTable table; table.types.push_back(unchanged);
    // Independent BASE v5 fixture digest: no space override was authored.
    constexpr std::array<std::uint8_t, 32> baseline{
        0x47, 0xb9, 0x02, 0xc3, 0x78, 0x3a, 0x4e, 0x5d,
        0xa2, 0x66, 0xd1, 0x19, 0x2e, 0xe8, 0xd0, 0x1e,
        0xcc, 0xc5, 0xc8, 0x14, 0x0c, 0x49, 0x3c, 0x69,
        0xfb, 0x20, 0x92, 0xef, 0x38, 0xb2, 0x11, 0xe8};
    expect(eawr::units::galactic_content_identity(table) == baseline,
        "override-free galactic tables retain their BASE content identity");
    table.types.front().space_override_population_value = Fixed{};
    expect(eawr::units::galactic_content_identity(table) != baseline,
        "an explicit zero space override contributes to galactic identity");
}
void write(const std::filesystem::path& path, const std::string_view text) { std::ofstream out(path); out << text; }

void production_domain() {
    eawr::units::GalacticType renamed;
    renamed.name = "Mod_Orbital_Company";
    renamed.behavior = {"DUMMY_GROUND_COMPANY"};
    renamed.build_cost_credits = Fixed::from_integer(30).value();
    renamed.required_star_base_level = 1;
    renamed.always_spawn_in_orbit = true;
    expect(!eawr::units::built_on_ground(renamed), "renamed space-priced company selects the orbital build lane");
    renamed.required_ground_base_level = 1;
    expect(eawr::units::built_on_ground(renamed), "nonzero ground requirement wins even with a starbase requirement and orbit spawn");
    renamed.build_cost_credits = Fixed::from_raw(Fixed::scale - 1);
    expect(!eawr::units::built_on_ground(renamed), "lane uses authored cost at least one before location modifiers");
    renamed.build_cost_credits = Fixed::from_integer(1).value();
    renamed.required_ground_base_level = 0;
    renamed.behavior = {"dummy_ground_structure"};
    expect(eawr::units::built_on_ground(renamed), "ground structure role selects ground with zero required level");
    renamed.behavior = {"DUMMY_GROUND_BASE"};
    expect(!eawr::units::built_on_ground(renamed), "ground-base representation alone does not select the lane");
    renamed.build_cost_credits.reset();
    renamed.required_ground_base_level = 1;
    expect(!eawr::units::built_on_ground(renamed), "omitted cost does not select ground");
}

void member_timeline() {
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-member-timeline-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
    write(root / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>A.xml</File><File>B.xml</File><File>C.xml</File></Game_Object_Files>");
    write(root / "XML/HardpointDataFiles.xml", "<Hard_Point_Files/>");
    write(root / "XML/FactionFiles.xml", "<Faction_Files/>");
    write(root / "XML/CampaignFiles.xml", "<Campaign_Files/>");
    write(root / "XML/SFXEventFiles.xml", "<SFXEvent_Files/>");
    write(root / "XML/A.xml", R"xml(<Types>
      <GroundUnit Name="Base"/>
      <GroundUnit Name="Early"><Variant_Of_Existing_Type>Base</Variant_Of_Existing_Type></GroundUnit>
      <GroundUnit Name="EarlyChain"><Variant_Of_Existing_Type>Early</Variant_Of_Existing_Type></GroundUnit>
      <GroundCompany Name="Marker"><Company_Units>Base Pending Replayed</Company_Units><Squadron_Units>base Pending Replayed</Squadron_Units></GroundCompany>
      <GroundUnit Name="Untouched"/>
    </Types>)xml");
    write(root / "XML/B.xml", R"xml(<Types>
      <GroundUnit Name="Late"><Variant_Of_Existing_Type>Pending</Variant_Of_Existing_Type></GroundUnit>
      <GroundUnit Name="Pending"><Variant_Of_Existing_Type>LaterBase</Variant_Of_Existing_Type></GroundUnit>
      <GroundUnit Name="Replayed"><Variant_Of_Existing_Type>Untouched</Variant_Of_Existing_Type></GroundUnit>
      <GroundUnit Name="LateFromBase"><Variant_Of_Existing_Type>Base</Variant_Of_Existing_Type></GroundUnit>
    </Types>)xml");
    write(root / "XML/C.xml", "<Types><GroundUnit Name=\"LaterBase\"/></Types>");
    const std::array mounts{eawr::vfs::MountSpec{"test", root, "data", {}}};
    const auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "member timeline mounts");
    if (!filesystem) return;
    const auto loaded = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(loaded), "member timeline loads");
    if (!loaded) return;
    const auto& catalog = loaded.value().catalog;
    const auto check = [&](const std::string_view name, const bool ground, const bool squadron) {
        const auto value = catalog.member_flags(name);
        expect(value && value.value() == eawr::data::TypeMemberFlags{ground, squadron}, name);
    };
    check("Base", true, true);
    check("Early", false, false);
    check("EarlyChain", false, false);
    check("Late", true, true); // copied Pending's first-pass marks before its own overlay
    check("LateFromBase", true, true); // complete variant replayed with an incomplete sibling
    check("Pending", true, true); // copied false, then marked again by the next fix-up
    check("Replayed", true, true); // overwritten marks are re-applied each pass
    check("Marker", false, false); // owning a roster does not mark the containing type
    check("Untouched", false, false);
    expect(!catalog.member_flags("Missing"), "unknown member type refuses");
    const auto table = eawr::units::galactic_type_table(catalog);
    const auto* base = table.find("Base");
    expect(base && base->ground_company_member == true && base->squadron_member == true,
        "bare referenced member gets a typed galactic row");
    const std::array overrides{eawr::data::ValueOverride{"Marker", "Company_Units", {"Missing"}}};
    const auto invalid = eawr::data::with_overrides(catalog, overrides);
    expect(static_cast<bool>(invalid), "member perturbation overlay loads");
    if (invalid) {
        const auto value = invalid.value().member_flags("Base");
        expect(!value && value.error().code == eawr::data::diagnostic_codes::member_timeline_unavailable,
            "unresolved roster asserts become a named unavailable timeline seam");
        const auto unavailable = eawr::units::galactic_type_table(invalid.value());
        const auto* marker = unavailable.find("Marker");
        expect(marker && !marker->ground_company_member && !marker->squadron_member,
            "unavailable timeline is not a fabricated false");
    }
    check("Base", true, true); // override does not mutate the shared load

    for (const auto& change : {
            eawr::data::ValueOverride{"Early", "Variant_Of_Existing_Type", {"Missing"}},
            eawr::data::ValueOverride{"Base", "Variant_Of_Existing_Type", {"Early"}}}) {
        const std::array changes{change};
        const auto unresolved = eawr::data::with_overrides(catalog, changes);
        expect(unresolved && !unresolved.value().member_flags("Base"), "missing/cyclic base cannot qualify member flags");
    }

    std::string registry = "<Game_Object_Files>";
    for (int index = 0; index < 12; ++index) {
        const auto file = "Chain" + std::to_string(index) + ".xml";
        registry += "<File>" + file + "</File>";
        const auto link = index == 11 ? std::string{} : "<Variant_Of_Existing_Type>Type"
            + std::to_string(index + 1) + "</Variant_Of_Existing_Type>";
        write(root / "XML" / file, "<Types><GroundUnit Name=\"Type" + std::to_string(index) + "\">" + link + "</GroundUnit></Types>");
    }
    write(root / "XML/GameObjectFiles.xml", registry + "</Game_Object_Files>");
    const auto chain_filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(chain_filesystem), "long chain mounts");
    if (!chain_filesystem) return;
    const auto long_chain = eawr::data::load_catalog(chain_filesystem.value(), eawr::data::Profile::foc);
    expect(long_chain && !long_chain.value().catalog.member_flags("Type0"), "ten-pass limit refuses incomplete chains");
}

void synthetic() {
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-gc-types-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
    write(root / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>Objects.xml</File></Game_Object_Files>");
    write(root / "XML/HardpointDataFiles.xml", "<Hard_Point_Files/>");
    write(root / "XML/FactionFiles.xml", "<Faction_Files><File>Factions.xml</File></Faction_Files>");
    write(root / "XML/CampaignFiles.xml", "<Campaign_Files/>");
    write(root / "XML/SFXEventFiles.xml", "<SFXEvent_Files/>");
    write(root / "XML/Factions.xml", "<FactionList><Faction Name=\"Custom_Side\"><Is_Playable>Yes</Is_Playable></Faction></FactionList>");
    write(root / "XML/Objects.xml", R"xml(<GameObjectData>
      <SpaceUnit Name="Carrier"><Is_Stealth_Company>Yes</Is_Stealth_Company><Can_Hyperspace_Without_Activating_Ability>True</Can_Hyperspace_Without_Activating_Ability><Hyperspace_Speed>1.5f</Hyperspace_Speed><Population_Value>2.5</Population_Value><Space_Override_Population_Value>3</Space_Override_Population_Value>
        <Hyperspace>Yes</Hyperspace><Always_Spawn_In_Orbit>Yes</Always_Spawn_In_Orbit><Create_Additional_Object>Variant</Create_Additional_Object>
        <Stay_In_Transport_During_Ground_Battle>Yes</Stay_In_Transport_During_Ground_Battle>
        <Squadron_Capacity>4</Squadron_Capacity><Political_Control>1</Political_Control><Cheap_Bribe_Cost>50</Cheap_Bribe_Cost>
        <Expensive_Bribe_Cost>150</Expensive_Bribe_Cost><Build_Cost_Credits>500</Build_Cost_Credits><Build_Time_Seconds>30</Build_Time_Seconds>
        <Build_Initially_Locked>Yep</Build_Initially_Locked><Required_Ground_Base_Level>2</Required_Ground_Base_Level>
        <Required_Special_Structures>Factory | Research</Required_Special_Structures><Affiliation>Custom_Side</Affiliation>
        <Abilities><Galactic_Stealth_Ability Name="Initial"><Activation_Style>Galactic_Automatic</Activation_Style></Galactic_Stealth_Ability></Abilities></SpaceUnit>
      <SpaceUnit Name="Variant"><Variant_Of_Existing_Type>Carrier</Variant_Of_Existing_Type><Hyperspace_Speed>TBD</Hyperspace_Speed>
        <Hyperspace>No</Hyperspace>
        <Stay_In_Transport_During_Ground_Battle>No</Stay_In_Transport_During_Ground_Battle>
        <Build_Cost_Credits>600</Build_Cost_Credits><Build_Initially_Locked>nope</Build_Initially_Locked></SpaceUnit>
      <Squadron Name="Flight"><Company_Transport_Unit>Variant</Company_Transport_Unit><Squadron_Units>Carrier</Squadron_Units><Squadron_Units>Variant</Squadron_Units></Squadron>
      <Squadron Name="Extra_Flight"><Variant_Of_Existing_Type>Flight</Variant_Of_Existing_Type><Squadron_Units>Carrier</Squadron_Units></Squadron>
      <GroundCompany Name="Company"><Behavior>DUMMY_GROUND_COMPANY</Behavior><Company_Units>Carrier Variant</Company_Units><Population_Value>1</Population_Value><Build_Cost_Credits>30</Build_Cost_Credits><Required_Star_Base_Level>1</Required_Star_Base_Level><Build_Tab_Heroes>Yes</Build_Tab_Heroes></GroundCompany>
      <GroundCompany Name="Renamed_Orbital_Team"><Variant_Of_Existing_Type>Company</Variant_Of_Existing_Type><Build_Tab_Space_Units>Yes</Build_Tab_Space_Units></GroundCompany>
      <StarBase Name="Base"><Base_Level>2</Base_Level><Next_Level_Base>Missing_Base</Next_Level_Base><Additional_Population_Capacity>10</Additional_Population_Capacity></StarBase>
      <SpaceUnit Name="Bad"><Build_Cost_Credits>lots</Build_Cost_Credits><Squadron_Capacity>2.5</Squadron_Capacity><Bogus_GC_Tag>1</Bogus_GC_Tag></SpaceUnit>
      <UpgradeObject Name="Tech2"><Build_Advances_Tech_Level>2</Build_Advances_Tech_Level></UpgradeObject>
      <UpgradeObject Name="Tech3"><Build_Advances_Tech_Level>3</Build_Advances_Tech_Level></UpgradeObject>
      <UpgradeObject Name="Tech4"><Build_Advances_Tech_Level>4</Build_Advances_Tech_Level></UpgradeObject>
      <UpgradeObject Name="Tech5"><Build_Advances_Tech_Level>5</Build_Advances_Tech_Level></UpgradeObject>
      <UpgradeObject Name="BadTech"><Build_Advances_Tech_Level>unknown</Build_Advances_Tech_Level></UpgradeObject>
      <UpgradeObject Name="NegativeTech"><Build_Advances_Tech_Level>-1</Build_Advances_Tech_Level></UpgradeObject>
      <UpgradeObject Name="MaxTech"><Build_Advances_Tech_Level>2147483647</Build_Advances_Tech_Level></UpgradeObject>
      <UpgradeObject Name="OverflowTech"><Build_Advances_Tech_Level>2147483648</Build_Advances_Tech_Level></UpgradeObject>
      <Planet Name="Planet"><Population_Value>20</Population_Value></Planet>
      <Projectile Name="Shot"><Max_Speed>10</Max_Speed></Projectile>
    </GameObjectData>)xml");
    const std::array mounts{eawr::vfs::MountSpec{"test", root, "data", {}}};
    const auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "mount synthetic GC types");
    if (!filesystem) return;
    const auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "load synthetic GC catalog");
    if (!catalog) return;
    const auto table = eawr::units::galactic_type_table(catalog.value().catalog);
    expect(table.types.size() == 16 && !table.find("Planet") && !table.find("Shot"), "GC types exclude planets and unrelated projectiles");
    for (const auto name : {"Company", "Renamed_Orbital_Team"}) {
        const auto* team = table.find(name);
        expect(team && !eawr::units::built_on_ground(*team), "hero and space category companies inherit an orbital production lane");
    }
    for (std::int64_t target = 2; target <= 5; ++target) {
        const auto* technology = table.find("Tech" + std::to_string(target));
        expect(technology && technology->build_advances_tech_level == target,
            "WGEP-42 technology target 2-5 is numeric rather than Boolean");
    }
    expect(!eawr::units::resolve_initial_galactic_type(table, "BadTech")
        && !eawr::units::resolve_initial_galactic_type(table, "NegativeTech"), "invalid authored technology targets refuse");
    expect(eawr::units::resolve_initial_galactic_type(table, "MaxTech")
        && !eawr::units::resolve_initial_galactic_type(table, "OverflowTech"), "technology resolver agrees with the simulation signed-32 target bound");
    const auto* carrier = table.find("carrier");
    const auto* variant = table.find("Variant");
    expect(carrier && variant && carrier->space_override_population_value == Fixed::from_integer(3).value()
        && variant->space_override_population_value == carrier->space_override_population_value
        && eawr::units::population_in_space(variant->population_value, variant->space_override_population_value) == Fixed::from_integer(3).value(),
        "space population override inherits without changing the galactic base population");
    for (const auto& value : {std::string{"0"}, std::string{"-1"}}) {
        const std::array changes{eawr::data::ValueOverride{"Variant", "Space_Override_Population_Value", {value}}};
        const auto changed = eawr::data::with_overrides(catalog.value().catalog, changes);
        expect(static_cast<bool>(changed), "space population variant override applies");
        if (!changed) continue;
        const auto changed_types = eawr::units::galactic_type_table(changed.value());
        const auto* changed_variant = changed_types.find("Variant");
        const auto expected = value == "0" ? Fixed{} : Fixed::from_decimal("2.5").value();
        expect(changed_variant && eawr::units::population_in_space(changed_variant->population_value,
            changed_variant->space_override_population_value) == expected,
            "zero space override clears population; negative space override falls back to base");
        expect(eawr::units::galactic_content_identity(changed_types) != eawr::units::galactic_content_identity(table),
            "space population override is retained by galactic content identity");
    }
    expect(carrier && variant && carrier->stays_in_transport == true && variant->stays_in_transport == false,
        "current transport-stay authoring inherits and supports explicit override");
    expect(carrier && carrier->hyperspace_speed == Fixed::from_decimal("1.5").value()
        && carrier->population_value == Fixed::from_decimal("2.5").value() && carrier->squadron_capacity == 4
        && carrier->cheap_bribe_cost == Fixed::from_decimal("50").value() && carrier->build_initially_locked == true,
        "movement/population/bribe fields and retail first-letter boolean");
    expect(carrier && variant && variant->hyperspace_speed == carrier->hyperspace_speed && variant->squadron_capacity == 4
        && variant->build_cost_credits == Fixed::from_decimal("600").value() && variant->build_initially_locked == false,
        "variant inheritance, placeholder no-op and explicit overrides");
    expect(carrier && variant && carrier->can_hyperspace_without_ability == true && variant->can_hyperspace_without_ability == true,
        "WGM-83 original-type flag loads and inherits");
    const auto* flight = table.find("Extra_Flight");
    expect(carrier && variant && carrier->initial_stealth.size() == 1 && variant->initial_stealth.size() == 1
        && !variant->initial_stealth[0].initially_enabled
        && variant->initial_stealth[0].activation_style == "Galactic_Automatic",
        "initial stealth authoring inherits while constructor defaults remain a consumer decision");
    expect(carrier && variant && carrier->hyperspace_capable == true && variant->hyperspace_capable == false
        && variant->always_spawn_in_orbit == true && variant->additional_creation_type == "Variant",
        "starting-force flags and additional creation inherit with explicit false taking precedence");
    eawr::units::GalacticType omitted; omitted.name = "Omitted";
    auto defaults_table = table; defaults_table.types.push_back(omitted);
    const auto defaults = eawr::units::resolve_initial_galactic_type(defaults_table, "Omitted");
    expect(defaults && defaults.value().hyperspace_speed == Fixed{} && defaults.value().political_control == Fixed{}
        && defaults.value().squadron_capacity == 0 && defaults.value().hyperspace_capable == false
        && defaults.value().always_spawn_in_orbit == false, "WGS-92 shared omission defaults");
    expect(defaults && !defaults.value().build_cost_credits && !defaults.value().build_time_seconds
        && !defaults.value().can_hyperspace_without_ability,
        "content-bearing originals retain omissions for consumer defaults");
    expect(!eawr::units::resolve_initial_galactic_type(table, "Bad"), "invalid capacity cannot become an omission default");
    expect(carrier && variant && carrier->stealth_company == true && variant->stealth_company == true,
        "stealth company metadata retains inherited authoring without fabricating live action traits");
    expect(flight && flight->squadron_units == std::vector<std::string>{"Carrier"},
        "WAR-84 first nonempty variant squadron occurrence replaces inherited membership");
    expect(flight && flight->company_transport == "Variant", "WGEP-78 original company transport loads and inherits");
    const auto* bad = table.find("Bad");
    expect(bad && !bad->build_cost_credits && !bad->squadron_capacity, "invalid numeric values stay absent");
    expect(carrier && carrier->required_special_structures == "Factory | Research", "preserve production prerequisite group grammar");
    bool trailing = false, unknown = false, dangling = false;
    for (const auto& row : table.issues) {
        trailing = trailing || row.kind == eawr::data::galactic::IssueKind::trailing_text;
        unknown = unknown || row.kind == eawr::data::galactic::IssueKind::unknown_tag;
        dangling = dangling || row.kind == eawr::data::galactic::IssueKind::dangling_reference;
        expect(!row.file.empty() && !row.owner.empty() && !row.tag.empty(), "diagnostics retain named provenance");
    }
    expect(trailing && unknown && dangling, "trailing text, unknown tag and dangling base reference diagnosed");
    const auto identity = eawr::units::galactic_content_identity(table);
    auto changed_members = table;
    changed_members.types.front().ground_company_member = !changed_members.types.front().ground_company_member.value_or(false);
    expect(identity != eawr::units::galactic_content_identity(changed_members), "derived ground membership contributes to content identity");
    changed_members = table;
    changed_members.types.front().squadron_member.reset();
    expect(identity != eawr::units::galactic_content_identity(changed_members), "unknown squadron membership differs from known false or true in identity");
    expect(identity == eawr::units::galactic_content_identity(eawr::units::galactic_type_table(catalog.value().catalog)), "deterministic GC identity");
    auto changed = table;
    changed.issues.clear();
    changed.types[0].file = "other.xml";
    expect(identity == eawr::units::galactic_content_identity(changed), "provenance and issues excluded from identity");
    changed.types[0].population_value = Fixed::from_decimal("3").value();
    expect(identity != eawr::units::galactic_content_identity(changed), "GC population contributes to identity");
    changed = table; changed.types[0].company_transport = "Carrier";
    expect(identity != eawr::units::galactic_content_identity(changed), "WGEP-78 static transport link contributes to GC content identity");
    changed = table; changed.types[0].initial_stealth.front().initially_enabled = true;
    expect(identity != eawr::units::galactic_content_identity(changed), "nonempty authored stealth inputs contribute to identity");
}

std::optional<std::string> game_root() {
#if defined(_MSC_VER)
    char* text = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&text, &length, "EAWR_EAW_GAME_ROOT") != 0 || !text) return std::nullopt;
    std::string result(text);
    std::free(text);
    return result.empty() ? std::nullopt : std::optional<std::string>(result);
#else
    const auto* text = std::getenv("EAWR_EAW_GAME_ROOT");
    return text && *text ? std::optional<std::string>(text) : std::nullopt;
#endif
}

// WGEP-163: the credit-halt sabotage profile follows the authored ability data; the stock files
// never author these two tags, so mods are the only source.
void credit_halt_authoring() {
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-credit-halt-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    write(root / "XML/TradeRouteFiles.xml", "<Trade_Route_Files/>");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
    write(root / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>Objects.xml</File></Game_Object_Files>");
    write(root / "XML/HardpointDataFiles.xml", "<Hard_Point_Files/>");
    write(root / "XML/FactionFiles.xml", "<Faction_Files/>");
    write(root / "XML/CampaignFiles.xml", "<Campaign_Files/>");
    write(root / "XML/SFXEventFiles.xml", "<SFXEvent_Files/>");
    write(root / "XML/Objects.xml", R"xml(<GameObjectData>
      <HeroUnit Name="Halter"><Abilities SubObjectList="Yes"><Galactic_Sabotage_Ability Name="Halter_Sabotage">
        <Can_Halt_Credit_Production>Yes</Can_Halt_Credit_Production><Duration_Of_Credit_Halt>12.5</Duration_Of_Credit_Halt>
        <Can_Destroy_Special_Structure>True</Can_Destroy_Special_Structure></Galactic_Sabotage_Ability></Abilities></HeroUnit>
      <HeroUnit Name="Inherits"><Variant_Of_Existing_Type>Halter</Variant_Of_Existing_Type></HeroUnit>
      <HeroUnit Name="Structure_Only"><Abilities SubObjectList="Yes"><Galactic_Sabotage_Ability Name="Plain_Sabotage">
        <Can_Destroy_Special_Structure>True</Can_Destroy_Special_Structure></Galactic_Sabotage_Ability></Abilities></HeroUnit>
      <HeroUnit Name="Disabled"><Abilities SubObjectList="Yes"><Galactic_Sabotage_Ability Name="Off_Sabotage">
        <Can_Halt_Credit_Production>No</Can_Halt_Credit_Production><Duration_Of_Credit_Halt>9</Duration_Of_Credit_Halt>
        </Galactic_Sabotage_Ability></Abilities></HeroUnit>
      <HeroUnit Name="No_Duration"><Abilities SubObjectList="Yes"><Galactic_Sabotage_Ability Name="Open_Sabotage">
        <Can_Halt_Credit_Production>Yes</Can_Halt_Credit_Production></Galactic_Sabotage_Ability></Abilities></HeroUnit>
      <Planet Name="Arbitrary_Network"><Galactic_Position>0,0,0</Galactic_Position><Behavior>PLANET</Behavior>
        <Galactic_Influence_Range>23</Galactic_Influence_Range><Abilities SubObjectList="Yes">
        <System_Spy_Ability Name="Mod_Network"><Initially_Enabled>Yes</Initially_Enabled>
        <Activation_Style>Galactic_Automatic</Activation_Style><See_Fleet_Contents>Yes</See_Fleet_Contents>
        <See_Credit_Income_Breakdown>Yes</See_Credit_Income_Breakdown><Duration_In_Secs>7</Duration_In_Secs>
        </System_Spy_Ability></Abilities></Planet>
      <Planet Name="Renamed_Network"><Variant_Of_Existing_Type>Arbitrary_Network</Variant_Of_Existing_Type>
        <Galactic_Influence_Range>0</Galactic_Influence_Range></Planet>
      <Planet Name="Cleared_Network"><Variant_Of_Existing_Type>Arbitrary_Network</Variant_Of_Existing_Type>
        <Abilities SubObjectList="Yes"/></Planet>
    </GameObjectData>)xml");
    const std::array mounts{eawr::vfs::MountSpec{"test", root, "data", {}}};
    const auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "credit-halt fixture mounts");
    if (!filesystem) return;
    const auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "credit-halt fixture loads");
    if (!catalog) return;
    const auto table = eawr::units::galactic_economic_table(catalog.value().catalog);
    const auto find = [&](std::string_view name) {
        return std::find_if(table.types.begin(), table.types.end(), [&](const auto& row) { return row.name == name; });
    };
    const auto halter = find("Halter");
    expect(halter != table.types.end() && halter->credit_halt == std::optional<bool>(true)
        && halter->credit_halt_seconds == Fixed::from_decimal("12.5").value(), "authored halt flag and duration load");
    const auto inherited = find("Inherits");
    expect(inherited != table.types.end() && inherited->credit_halt == std::optional<bool>(true)
        && inherited->credit_halt_seconds == Fixed::from_decimal("12.5").value(), "variant inherits the authored halt");
    const auto structure = find("Structure_Only");
    expect(structure != table.types.end() && !structure->credit_halt && !structure->credit_halt_seconds,
        "other sabotage tags author no halt profile");
    const auto disabled = find("Disabled");
    expect(disabled != table.types.end() && disabled->credit_halt == std::optional<bool>(false),
        "an authored false flag is retained as not a halt ability");
    const auto open = find("No_Duration");
    expect(open != table.types.end() && open->credit_halt == std::optional<bool>(true) && !open->credit_halt_seconds,
        "an absent duration stays absent for the setup to qualify");
    const auto renamed = find("Renamed_Network");
    const auto authored = find("Arbitrary_Network");
    expect(authored != table.types.end() && authored->system_spies.size() == 1
        && authored->system_spies[0].influence_range == Fixed::from_raw(23 * Fixed::scale)
        && authored->system_spies[0].mask == (64U | 32768U), "economic loader retains a mod-authored nonzero range and distinct flags");
    expect(renamed != table.types.end() && renamed->system_spies.size() == 1
        && renamed->system_spies[0].mask == (64U | 32768U) && renamed->system_spies[0].duration == Fixed::from_raw(7 * Fixed::scale)
        && renamed->system_spies[0].influence_range == Fixed{}, "economic loader inherits renamed planet flags/duration with an effective zero range");
    const auto cleared = find("Cleared_Network");
    expect(cleared != table.types.end() && cleared->system_spies.empty(), "authored variant ability list replaces inherited spy definitions");
    const auto galaxy = eawr::data::galactic::load_galactic_data(filesystem.value(), catalog.value().catalog);
    if (!galaxy) std::cerr << galaxy.error().message << '\n';
    expect(static_cast<bool>(galaxy), "galactic spy fixture loads");
    if (galaxy) {
        const auto* planet = galaxy.value().planet("Renamed_Network");
        expect(planet && planet->system_spies.size() == 1 && planet->system_spies[0].mask == (64U | 32768U)
            && planet->system_spies[0].influence_range == Fixed{}, "galactic and economic loaders share the same effective spy profile");
    }
}
void technology_price_authoring() {
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-technology-price-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
    write(root / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>Objects.xml</File></Game_Object_Files>");
    for (const auto& [file, element] : {std::pair{"HardpointDataFiles", "Hard_Point_Files"},
            std::pair{"FactionFiles", "Faction_Files"}, std::pair{"CampaignFiles", "Campaign_Files"}, std::pair{"SFXEventFiles", "SFXEvent_Files"}})
        write(root / (std::string("XML/") + file + ".xml"), std::string("<") + element + "/>");
    write(root / "XML/Objects.xml", R"xml(<GameObjectData>
      <Planet Name="Renamed_Bonus"><Abilities SubObjectList="Yes">
        <Reduce_Technology_Price_Ability Name="Source"><Price_Reduction_Percentage>0.37</Price_Reduction_Percentage></Reduce_Technology_Price_Ability>
      </Abilities></Planet>
      <Planet Name="Inherited_Bonus"><Variant_Of_Existing_Type>Renamed_Bonus</Variant_Of_Existing_Type></Planet>
      <Planet Name="Replaced_Bonus"><Variant_Of_Existing_Type>Renamed_Bonus</Variant_Of_Existing_Type><Abilities SubObjectList="Yes">
        <Reduce_Technology_Price_Ability Name="Source"><Price_Reduction_Percentage>-0.25</Price_Reduction_Percentage></Reduce_Technology_Price_Ability>
      </Abilities></Planet>
      <Planet Name="Cleared_Bonus"><Variant_Of_Existing_Type>Renamed_Bonus</Variant_Of_Existing_Type><Abilities SubObjectList="Yes"/></Planet>
      <Planet Name="Defaults"><Abilities SubObjectList="Yes"><Reduce_Technology_Price_Ability Name="Default"/></Abilities></Planet>
      <Planet Name="Malformed"><Abilities SubObjectList="Yes"><Reduce_Technology_Price_Ability Name="Bad"><Price_Reduction_Percentage>bad</Price_Reduction_Percentage></Reduce_Technology_Price_Ability></Abilities></Planet>
      <Planet Name="Duplicate"><Abilities SubObjectList="Yes">
        <Reduce_Technology_Price_Ability Name="Same"><Price_Reduction_Percentage>0.1</Price_Reduction_Percentage></Reduce_Technology_Price_Ability>
        <Reduce_Technology_Price_Ability Name="Same"><Price_Reduction_Percentage>0.2</Price_Reduction_Percentage></Reduce_Technology_Price_Ability>
        <Reduce_Technology_Price_Ability Name="Other"><Price_Reduction_Percentage>0.3</Price_Reduction_Percentage></Reduce_Technology_Price_Ability>
      </Abilities></Planet>
    </GameObjectData>)xml");
    const std::array mounts{eawr::vfs::MountSpec{"test", root, "data", {}}};
    const auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "technology-price fixture mounts"); if (!filesystem) return;
    const auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "technology-price fixture loads"); if (!catalog) return;
    const auto table = eawr::units::galactic_economic_table(catalog.value().catalog);
    const auto row = [&](std::string_view name) -> const eawr::units::GalacticEconomicType& {
        const auto at = std::find_if(table.types.begin(), table.types.end(), [&](const auto& value) { return value.name == name; });
        if (at == table.types.end()) throw std::runtime_error("missing technology-price fixture " + std::string(name));
        return *at;
    };
    expect(row("Renamed_Bonus").technology_prices[0].reduction == Fixed::from_decimal("0.37").value()
        && row("Inherited_Bonus").technology_prices[0].reduction == Fixed::from_decimal("0.37").value(), "arbitrary planet names and inheritance retain authored technology price");
    expect(row("Replaced_Bonus").technology_prices[0].reduction == Fixed::from_decimal("-0.25").value()
        && row("Cleared_Bonus").technology_prices.empty(), "variant block replaces or clears the inherited ability");
    const auto& defaults = row("Defaults").technology_prices[0];
    expect(defaults.initially_enabled == true && defaults.activation_style == "Galactic_Automatic"
        && defaults.reduction == Fixed{}, "specific technology-price constructor defaults are sourced");
    expect(!row("Malformed").technology_prices[0].reduction, "malformed present percentage does not use the omission default");
    expect(row("Duplicate").technology_prices.size() == 2
        && row("Duplicate").technology_prices[0].reduction == Fixed::from_decimal("0.2").value(), "same authored source replaces; distinct names remain independent");
}
void installed() {
    const auto root = game_root();
    if (!root) { std::cout << "GC type census skipped (set EAWR_EAW_GAME_ROOT)\n"; return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& folder : {"corruption", "GameData"}) {
        const auto manifest = eawr::vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "installed type manifest");
        if (!manifest) return;
        mounts.push_back(manifest.value().mount);
    }
    const auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "installed type VFS");
    if (!filesystem) return;
    const auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "installed type catalog");
    if (!catalog) return;
    const auto table = eawr::units::galactic_type_table(catalog.value().catalog);
    const auto* transport = table.find("Imperial_Landing_Craft");
    expect(transport && transport->hyperspace_speed == Fixed::from_decimal("1.5").value(), "WGM-26 stock transport speed");
    bool movement_flag = false;
    for (const auto& type : table.types) movement_flag = movement_flag || type.can_hyperspace_without_ability.has_value();
    expect(movement_flag, "installed WGM-83 original-type reader");
    expect(!table.types.empty(), "installed galactic type census is nonempty");
    std::map<std::string, std::size_t> classes;
    for (const auto& type : table.types) ++classes[type.object_class];
    const auto identity = eawr::units::galactic_content_identity(table);
    std::string digest;
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : identity) { digest += hex[byte >> 4U]; digest += hex[byte & 15U]; }
    std::cout << "GC type census: types=" << table.types.size() << " issues=" << table.issues.size()
        << " identity=" << digest << '\n';
    std::map<std::string, eawr::data::TypeMemberFlags> member_rows;
    std::size_t ground = 0, squadron = 0;
    for (const auto& definition : catalog.value().catalog.definitions()) {
        if (definition.category != eawr::data::Category::game_object || !definition.namespace_winner) continue;
        const auto flags = catalog.value().catalog.member_flags(definition.id);
        expect(static_cast<bool>(flags), "every stock current-type member flag is qualified");
        if (!flags) continue;
        auto key = definition.id;
        for (char& ch : key) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
        member_rows.emplace(key, flags.value());
        ground += flags.value().ground_company_member ? 1U : 0U;
        squadron += flags.value().squadron_member ? 1U : 0U;
        if (const auto* type = table.find(definition.id)) {
            expect(type->ground_company_member == flags.value().ground_company_member
                && type->squadron_member == flags.value().squadron_member, "typed table preserves both loader flags");
        } else expect(!flags.value().ground_company_member && !flags.value().squadron_member,
            "every stock company/squadron member has a typed row");
    }
    std::string member_bytes;
    for (const auto& [name, flags] : member_rows) {
        member_bytes += name + ':';
        member_bytes += flags.ground_company_member ? '1' : '0';
        member_bytes += flags.squadron_member ? '1' : '0';
        member_bytes += '\n';
    }
    // Independent stock XML audit under WGA-363/364, including all unmarked variants.
    expect(member_rows.size() == 3345 && ground == 186 && squadron == 40,
        "stock member counts match the independent parse-timeline audit");
    expect(eawr::core::sha256_hex({reinterpret_cast<const std::uint8_t*>(member_bytes.data()), member_bytes.size()})
        == "7bf52f2dab27abe947c6cab8efeb50948749e8815c685af53fbbfe2f52709d88",
        "both flags for every stock game-object type match the independent audit");
    const auto stock_flags = [&](const std::string_view name, const eawr::data::TypeMemberFlags expected) {
        const auto value = catalog.value().catalog.member_flags(name);
        expect(value && value.value() == expected, name);
    };
    stock_flags("Scout_Trooper", {true, false});
    stock_flags("Scout_Trooper_Converted_To_Enemy", {false, false});
    stock_flags("Squad_Rebel_Trooper", {true, false});
    stock_flags("Squad_Rebel_Trooper_Converted_To_Enemy", {false, false});
    stock_flags("TIE_Fighter", {false, true});
    stock_flags("CIN_TIE_Fighter_Intro", {false, false});
    for (const auto& [name, count] : classes) std::cout << name << '=' << count << '\n';
    expect(identity == eawr::units::galactic_content_identity(eawr::units::galactic_type_table(catalog.value().catalog)), "installed GC table determinism");
}
} // namespace

int main(const int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--installed") {
        const auto root = game_root();
        if (!root || root->empty()) return 77;
        installed();
    } else if (argc == 1) {
        override_free_identity(); production_domain(); synthetic(); member_timeline();
        credit_halt_authoring(); technology_price_authoring();
    } else return 2;
    return failures == 0 ? 0 : 1;
}
