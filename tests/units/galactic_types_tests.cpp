#include "../support/env.hpp"
#include "../support/temp_dir.hpp"
#include "../support/check.hpp"
#include "eawr/units/galactic_types.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using eawr::sim::math::Fixed;
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(const bool value, const std::string_view message) {
    if (!value) local_checker.check_before_print(value, message, std::cerr, "FAIL: ");
}
void override_free_identity() {
    eawr::units::GalacticType unchanged;
    unchanged.name = "Unchanged"; unchanged.object_class = "SpaceUnit";
    eawr::units::GalacticTypeTable table; table.types.push_back(unchanged);
    // Independent v6 fixture digest: no space override was authored. Re-pinned for the
    // Piracy_Value_Credits field added to the identity (#b92e9cbcf, WGM-150): one more absent-optional byte.
    constexpr std::array<std::uint8_t, 32> baseline{
        0xf2, 0xc9, 0x71, 0xcb, 0xe7, 0xfc, 0xb6, 0x25,
        0x11, 0x03, 0xeb, 0xb7, 0xd2, 0xe8, 0x63, 0x21,
        0xeb, 0x1d, 0xa9, 0x59, 0xa1, 0x73, 0x22, 0x81,
        0xc8, 0x33, 0x46, 0xd9, 0x03, 0x76, 0x9c, 0x06};
    const auto current = eawr::units::galactic_content_identity(table);
    expect(current == baseline, "WR-85 v6 content identity matches the reviewed fixture bytes");
    table.types.front().space_override_population_value = Fixed{};
    expect(eawr::units::galactic_content_identity(table) != current,
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
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{root};
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
        // EAWR recovery policy under RGC-154: an undefined roster member is skipped per slot and reported; the timeline stays
        // available, so every type keeps its flags and the owner is unchanged. The debug build's behaviour is undefined here.
        const auto value = invalid.value().member_flags("Base");
        expect(value && !value.value().ground_company_member && value.value().squadron_member,
            "an undefined roster member does not make the member timeline unavailable");
        const auto& notes = invalid.value().member_roster_notes();
        expect(notes.size() == 1 && notes[0].owner == "Marker" && notes[0].tag == "Company_Units" && notes[0].slot == 0
            && notes[0].name == "Missing", "the skipped slot is reported with owner, roster, slot and name");
        expect(catalog.member_roster_notes().empty(), "the override does not report into the shared load");
        const auto recovered = eawr::units::galactic_type_table(invalid.value());
        const auto* marker = recovered.find("Marker");
        expect(marker && marker->ground_company_member == false && marker->squadron_member == false,
            "the owner keeps real flags after a skipped slot");
        expect(std::any_of(recovered.issues.begin(), recovered.issues.end(), [](const auto& issue) {
            return issue.owner == "Marker" && issue.tag == "Company_Units" && issue.reason.find("slot 0") != std::string::npos
                && issue.reason.find("Missing") != std::string::npos; }), "the type table lists the skipped slot as an issue");
        const std::array more{eawr::data::ValueOverride{"Marker", "Company_Units", {"Base Ghost Pending"}}};
        const auto middle = eawr::data::with_overrides(catalog, more);
        expect(middle && middle.value().member_roster_notes().size() == 1 && middle.value().member_roster_notes()[0].slot == 1
            && middle.value().member_flags("Pending") && middle.value().member_flags("Pending").value().ground_company_member
            && middle.value().member_flags("Base").value().ground_company_member,
            "siblings of a skipped middle slot are still marked");
    }
    check("Base", true, true); // override does not mutate the shared load

    // RGC-180..187: a duplicate game-object name does not abort the member pass; the duplicate keeps its own slot. Roster
    // references resolve to the same entry catalog lookups answer (the strongest layer's, then the last registered one;
    // the debug build's first-registered rule is the follow-up #3741).
    write(root / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>A.xml</File><File>Dup.xml</File></Game_Object_Files>");
    write(root / "XML/Dup.xml", R"xml(<Types>
      <GroundUnit Name="Twin"/>
      <GroundUnit Name="Twin"/>
      <GroundCompany Name="Holder"><Company_Units>Twin</Company_Units></GroundCompany>
      <GroundUnit Name="Selfie"><Variant_Of_Existing_Type>Selfie</Variant_Of_Existing_Type></GroundUnit>
    </Types>)xml");
    const auto duplicate_filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(duplicate_filesystem), "duplicate-name fixture mounts");
    if (duplicate_filesystem) {
        const auto duplicated = eawr::data::load_catalog(duplicate_filesystem.value(), eawr::data::Profile::foc);
        expect(duplicated && duplicated.value().catalog.member_flags("Base")
            && duplicated.value().catalog.member_flags("Twin") && duplicated.value().catalog.member_flags("Holder")
            && duplicated.value().catalog.member_flags("Selfie"),
            "a duplicate name and a variant naming itself as base keep the member timeline available for every type");
        // Pinned (#3741): the roster reference marks the entry that lookups answer, here the last registered duplicate.
        expect(duplicated && duplicated.value().catalog.member_flags("Twin")
            && duplicated.value().catalog.member_flags("Twin").value().ground_company_member,
            "a roster reference marks the duplicate that catalog lookups answer");
    }

    {
        // CGS-13: a variant whose base is absent keeps its own tags and never completes; that alone leaves the timeline available.
        const std::array missing{eawr::data::ValueOverride{"Early", "Variant_Of_Existing_Type", {"Missing"}}};
        const auto unresolved = eawr::data::with_overrides(catalog, missing);
        expect(unresolved && unresolved.value().member_flags("Base") && unresolved.value().member_flags("Early")
            && unresolved.value().member_flags("EarlyChain"), "a variant with an absent base does not make the member timeline unavailable");
        // A cyclic chain still cannot qualify.
        const std::array cycle{eawr::data::ValueOverride{"Base", "Variant_Of_Existing_Type", {"Early"}}};
        const auto cyclic = eawr::data::with_overrides(catalog, cycle);
        expect(cyclic && !cyclic.value().member_flags("Base"), "a cyclic base cannot qualify member flags");
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

// A normal cross-layer override: the base layer defines Shared, a stronger layer redefines it in another file. The stronger
// layer's roster must drive the member flags, never the base layer's (the duplicate rule is not a first-layer-wins rule).
void member_timeline_cross_layer() {
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto base_root = std::filesystem::temp_directory_path() / ("eawr-member-base-" + stamp);
    const auto mod_root = std::filesystem::temp_directory_path() / ("eawr-member-mod-" + stamp);
    std::filesystem::create_directories(base_root / "XML");
    std::filesystem::create_directories(mod_root / "XML");
    struct Cleanup { std::filesystem::path a, b; ~Cleanup() { eawr::testing::remove_temp_directory(a); eawr::testing::remove_temp_directory(b); } }
        cleanup{base_root, mod_root};
    write(base_root / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>Objects.xml</File></Game_Object_Files>");
    write(base_root / "XML/HardpointDataFiles.xml", "<Hard_Point_Files/>");
    write(base_root / "XML/FactionFiles.xml", "<Faction_Files/>");
    write(base_root / "XML/CampaignFiles.xml", "<Campaign_Files/>");
    write(base_root / "XML/SFXEventFiles.xml", "<SFXEvent_Files/>");
    write(base_root / "XML/Objects.xml", R"xml(<Types>
      <GroundUnit Name="OnlyBase"/>
      <GroundUnit Name="OnlyTop"/>
      <GroundCompany Name="Shared"><Company_Units>OnlyBase</Company_Units></GroundCompany>
    </Types>)xml");
    write(mod_root / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>Objects.xml</File><File>Override.xml</File></Game_Object_Files>");
    write(mod_root / "XML/Override.xml", R"xml(<Types>
      <GroundCompany Name="Shared"><Company_Units>OnlyTop</Company_Units></GroundCompany>
    </Types>)xml");
    // Mounts are supplied highest-precedence first.
    const std::array mounts{eawr::vfs::MountSpec{"mod", mod_root, "data", {}}, eawr::vfs::MountSpec{"base", base_root, "data", {}}};
    const auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "cross-layer fixture mounts");
    if (!filesystem) return;
    const auto loaded = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(loaded), "cross-layer fixture loads");
    if (!loaded) return;
    const auto& catalog = loaded.value().catalog;
    const auto top = catalog.member_flags("OnlyTop");
    const auto bottom = catalog.member_flags("OnlyBase");
    expect(top && top.value().ground_company_member && bottom && !bottom.value().ground_company_member,
        "a stronger layer's redefinition decides the member flags, not the base layer's");
}

void retail_numeric_families() {
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-retail-gc-numbers-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{root};
    write(root / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>Objects.xml</File></Game_Object_Files>");
    write(root / "XML/HardpointDataFiles.xml", "<Hard_Point_Files/>");
    write(root / "XML/FactionFiles.xml", "<Faction_Files/>");
    write(root / "XML/CampaignFiles.xml", "<Campaign_Files/>");
    write(root / "XML/SFXEventFiles.xml", "<SFXEvent_Files/>");
    write(root / "XML/Objects.xml", R"xml(<Types>
      <SpaceUnit Name="Fraction"><Hyperspace_Speed>-1.25tail</Hyperspace_Speed><Build_Cost_Credits>-2.9</Build_Cost_Credits><Build_Time_Seconds>2.9</Build_Time_Seconds></SpaceUnit>
      <SpaceUnit Name="Negative"><Hyperspace_Speed>-2</Hyperspace_Speed><Build_Cost_Credits>-1</Build_Cost_Credits><Build_Time_Seconds>-1</Build_Time_Seconds></SpaceUnit>
      <SpaceUnit Name="Text"><Hyperspace_Speed>nonsense</Hyperspace_Speed><Build_Cost_Credits>text</Build_Cost_Credits><Build_Time_Seconds>text</Build_Time_Seconds></SpaceUnit>
      <SpaceUnit Name="Overflow"><Hyperspace_Speed>1e99</Hyperspace_Speed><Build_Cost_Credits>9999999999999999999999</Build_Cost_Credits><Build_Time_Seconds>-99999999999999999</Build_Time_Seconds></SpaceUnit>
      <SpaceUnit Name="Empty"><Variant_Of_Existing_Type>Negative</Variant_Of_Existing_Type><Build_Cost_Credits/><Build_Time_Seconds>TBD</Build_Time_Seconds></SpaceUnit>
    </Types>)xml");
    const std::array mounts{eawr::vfs::MountSpec{"test", root, "data", {}}};
    const auto filesystem = eawr::vfs::Vfs::mount(mounts).value();
    const auto catalog = eawr::data::load_catalog(filesystem, eawr::data::Profile::foc).value();
    const auto table = eawr::units::galactic_type_table(catalog.catalog);
    const auto fraction = eawr::units::resolve_initial_galactic_type(table, "Fraction").value();
    const auto negative = eawr::units::resolve_initial_galactic_type(table, "Negative").value();
    const auto text = eawr::units::resolve_initial_galactic_type(table, "Text").value();
    const auto overflow = eawr::units::resolve_initial_galactic_type(table, "Overflow").value();
    const auto empty = eawr::units::resolve_initial_galactic_type(table, "Empty").value();
    expect(fraction.hyperspace_speed == Fixed::from_ratio(-5, 4).value()
        && fraction.build_cost_credits == Fixed::from_integer(-2).value()
        && fraction.build_time_seconds == Fixed::from_integer(2).value(), "float fractions retained; integer price/time stop at decimal point");
    expect(negative.hyperspace_speed == Fixed::from_integer(-2).value()
        && negative.build_cost_credits == Fixed::from_integer(-1).value()
        && negative.build_time_seconds == Fixed::from_integer(4294967295LL).value(), "negative sign retained or unsigned-reinterpreted by parser kind");
    expect(text.hyperspace_speed == Fixed{} && text.build_cost_credits == Fixed{} && text.build_time_seconds == Fixed{}, "text is numeric zero, never a missing-value load error");
    expect(overflow.hyperspace_speed == Fixed::from_raw(std::numeric_limits<std::int64_t>::max())
        && overflow.build_cost_credits == Fixed::from_integer(2147483647).value()
        && overflow.build_time_seconds == Fixed::from_integer(2147483648LL).value(), "integer overflow saturates before unsigned reinterpretation; float overflow uses representable endpoint");
    expect(empty.build_cost_credits == negative.build_cost_credits && empty.build_time_seconds == negative.build_time_seconds,
        "empty and TBD keep inherited values");
}

void synthetic() {
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-gc-types-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{root};
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
        <Abilities><Galactic_Stealth_Ability Name="Initial"><Activation_Style>Galactic_Automatic</Activation_Style></Galactic_Stealth_Ability>
          <Political_Control_Protection_Ability Name="Mod_Shield"/>
          <Political_Transition_Bonus_Ability Name="Mod_Rate"><Time_Reduction_Percentage>0.375</Time_Reduction_Percentage><Stacking_Category>4</Stacking_Category></Political_Transition_Bonus_Ability>
        </Abilities></SpaceUnit>
      <SpaceUnit Name="Variant"><Variant_Of_Existing_Type>Carrier</Variant_Of_Existing_Type><Hyperspace_Speed>TBD</Hyperspace_Speed>
        <Hyperspace>No</Hyperspace>
        <Stay_In_Transport_During_Ground_Battle>No</Stay_In_Transport_During_Ground_Battle>
        <Build_Cost_Credits>600</Build_Cost_Credits><Build_Initially_Locked>nope</Build_Initially_Locked></SpaceUnit>
      <Squadron Name="Flight"><Company_Transport_Unit>Variant</Company_Transport_Unit><Squadron_Units>Carrier</Squadron_Units><Squadron_Units>Variant</Squadron_Units></Squadron>
      <Squadron Name="Extra_Flight"><Variant_Of_Existing_Type>Flight</Variant_Of_Existing_Type><Squadron_Units>Carrier</Squadron_Units></Squadron>
      <Squadron Name="Explicit_No"><Hyperspace>No</Hyperspace><Squadron_Units>Variant Carrier</Squadron_Units></Squadron>
      <Squadron Name="Explicit_Yes"><Hyperspace>Yes</Hyperspace><Squadron_Units>Carrier Variant</Squadron_Units></Squadron>
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
    expect(table.types.size() == 18 && !table.find("Planet") && !table.find("Shot"), "GC types exclude planets and unrelated projectiles");
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
    expect(carrier && variant && carrier->control_abilities.size() == 2 && variant->control_abilities.size() == 2
        && carrier->control_abilities[0].name == "Mod_Shield" && carrier->control_abilities[0].protection
        && carrier->control_abilities[0].initially_enabled == true
        && carrier->control_abilities[0].activation_style == "Galactic_Automatic"
        && carrier->control_abilities[1].multiplier == Fixed::from_ratio(3, 8).value()
        && carrier->control_abilities[1].category == 4,
        "renamed nonhero control abilities retain authored values, constructor defaults and variant inheritance");
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
    expect(table.find("Flight") && table.find("Flight")->hyperspace_capable == false
        && flight && flight->hyperspace_capable == true,
        "WR-85 omitted squadron capability follows the last craft after variant composition replacement");
    expect(table.find("Explicit_No") && table.find("Explicit_No")->hyperspace_capable == true
        && table.find("Explicit_Yes") && table.find("Explicit_Yes")->hyperspace_capable == false,
        "WR-85 the last craft replaces both explicit squadron flag values, rather than any/all/first craft");
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
    expect(bad && bad->build_cost_credits == Fixed{} && !bad->squadron_capacity, "text cost follows integer zero while invalid capacity stays absent");
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

// WGEP-163: the credit-halt sabotage profile follows the authored ability data; the stock files
// never author these two tags, so mods are the only source.
void credit_halt_authoring() {
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-credit-halt-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    write(root / "XML/TradeRouteFiles.xml", "<Trade_Route_Files/>");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{root};
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
      <GenericHeroUnit Name="Mod_Scout"><Abilities SubObjectList="Yes">
        <System_Spy_Ability Name="Mod_Scout_Spy"><Initially_Enabled>No</Initially_Enabled>
        <Activation_Style>Ground_Activated</Activation_Style><Causes_Despawn>Yes</Causes_Despawn>
        <See_Fleet_Contents>Yes</See_Fleet_Contents><Duration_In_Secs>17</Duration_In_Secs>
        </System_Spy_Ability></Abilities></GenericHeroUnit>
      <GenericHeroUnit Name="Scout_Variant"><Variant_Of_Existing_Type>Mod_Scout</Variant_Of_Existing_Type></GenericHeroUnit>
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
    for (const auto name : {"Mod_Scout", "Scout_Variant"}) {
        const auto scout = find(name);
        expect(scout != table.types.end() && scout->system_spies.size() == 1
            && scout->system_spies[0].activation_style == "Ground_Activated"
            && scout->system_spies[0].duration == Fixed::from_integer(17).value()
            && scout->system_spies[0].mask == 64U && scout->system_spies[0].causes_despawn
            && !scout->system_spies[0].initially_enabled,
            "unit and inherited variant retain authored ground-spy flags without a planet-class gate");
    }
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
void gambling_authoring() {
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-gambling-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{root};
    write(root / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>Objects.xml</File></Game_Object_Files>");
    for (const auto& [file, element] : {std::pair{"HardpointDataFiles", "Hard_Point_Files"},
            std::pair{"FactionFiles", "Faction_Files"}, std::pair{"CampaignFiles", "Campaign_Files"}, std::pair{"SFXEventFiles", "SFXEvent_Files"}})
        write(root / (std::string("XML/") + file + ".xml"), std::string("<") + element + "/>");
    // AaW Ground_Cantina_* authors a positive minimum; stock arenas author -0.20.
    write(root / "XML/Objects.xml", R"xml(<GameObjectData>
      <SpecialStructure Name="Cantina"><Abilities SubObjectList="Yes"><Planet_Income_Gambling_Ability Name="Bet">
        <Evaluation_Interval_In_Secs>30.0</Evaluation_Interval_In_Secs><Min_Income_Modifier>0.05</Min_Income_Modifier>
        <Max_Income_Modifier>0.80</Max_Income_Modifier><Chance_To_Win>0.80</Chance_To_Win>
      </Planet_Income_Gambling_Ability></Abilities></SpecialStructure>
      <SpecialStructure Name="Arena"><Abilities SubObjectList="Yes"><Planet_Income_Gambling_Ability Name="Bet">
        <Evaluation_Interval_In_Secs>-1</Evaluation_Interval_In_Secs><Min_Income_Modifier>-0.20</Min_Income_Modifier>
        <Max_Income_Modifier>-5.0</Max_Income_Modifier><Chance_To_Win>1.5</Chance_To_Win>
      </Planet_Income_Gambling_Ability></Abilities></SpecialStructure>
    </GameObjectData>)xml");
    const std::array mounts{eawr::vfs::MountSpec{"test", root, "data", {}}};
    const auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "gambling fixture mounts"); if (!filesystem) return;
    const auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "gambling fixture loads"); if (!catalog) return;
    const auto table = eawr::units::galactic_economic_table(catalog.value().catalog);
    const auto row = [&](std::string_view name) -> const eawr::units::GalacticEconomicType& {
        const auto at = std::find_if(table.types.begin(), table.types.end(), [&](const auto& value) { return value.name == name; });
        if (at == table.types.end()) throw std::runtime_error("missing gambling fixture " + std::string(name));
        return *at;
    };
    const auto& cantina = row("Cantina");
    expect(cantina.gambling && cantina.gambling_minimum == Fixed{} && cantina.gambling_maximum == Fixed::from_decimal("0.80").value()
        && cantina.gambling_chance == Fixed::from_decimal("0.80").value(), "WGEP-11: a minimum above 0 resets to 0, the rest stays");
    const auto& arena = row("Arena");
    expect(arena.gambling_minimum == Fixed::from_decimal("-0.20").value() && arena.gambling_maximum == Fixed{}
        && arena.gambling_chance == Fixed{} && arena.gambling_interval == Fixed{},
        "WGEP-11: a negative maximum or interval and a chance above 1 reset to 0");
    const auto reported = std::count_if(table.issues.begin(), table.issues.end(), [](const auto& issue) {
        return (issue.owner == "Cantina" || issue.owner == "Arena") && issue.reason.find("reset to 0") != std::string::npos; });
    expect(reported == 4, "each reset is reported");
}
void technology_price_authoring() {
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-technology-price-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{root};
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
    const auto root = eawr::testing::game_root();
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
    expect(table.find("Rebel_X-Wing_Squadron") && table.find("Rebel_X-Wing_Squadron")->hyperspace_capable == true,
        "WR-85 stock bought X-wing squadron derives hyperspace from its craft");
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
        const auto root = eawr::testing::game_root();
        if (!root || root->empty()) return 77;
        installed();
    } else if (argc == 1) {
        override_free_identity(); production_domain(); retail_numeric_families(); synthetic(); member_timeline(); member_timeline_cross_layer();
        credit_halt_authoring(); technology_price_authoring(); gambling_authoring();
    } else return 2;
    return failures == 0 ? 0 : 1;
}
