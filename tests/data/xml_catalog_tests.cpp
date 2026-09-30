#include "data_test_support.hpp"

#include "eawr/data/xml.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::tests::data_contracts {
namespace {

struct TempTree {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-data-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path mod_root = root.string() + "-mod";
    TempTree() { std::filesystem::create_directories(root); std::filesystem::create_directories(mod_root); }
    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::remove_all(mod_root, ignored);
    }
};

void write(const std::filesystem::path& path, const std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void registries(const std::filesystem::path& root) {
    write(root / "XML" / "GameObjectFiles.xml",
          "<Game_Object_Files><File>objects-a.xml</File><File>objects-b.xml</File></Game_Object_Files>");
    write(root / "XML" / "HardpointDataFiles.xml",
          "<Hard_Point_Files><File>hardpoints.xml</File></Hard_Point_Files>");
    write(root / "XML" / "FactionFiles.xml",
          "<Faction_Files><File>factions.xml</File></Faction_Files>");
    write(root / "XML" / "CampaignFiles.xml",
          "<Campaign_Files><File>campaigns.xml</File></Campaign_Files>");
    write(root / "XML" / "SFXEventFiles.xml",
          "<SFXEvent_Files><File>sfx.xml</File></SFXEvent_Files>");
}

bool has_code(const std::vector<eawr::core::Diagnostic>& values, const std::string_view code) {
    return std::any_of(values.begin(), values.end(), [&](const auto& value) { return value.code == code; });
}

} // namespace


void locale_contracts() {
    // setlocale is process-global: this executable is single-threaded and restores it.
    const std::string previous = std::setlocale(LC_CTYPE, nullptr);
    struct RestoreLocale {
        std::string value;
        ~RestoreLocale() { std::setlocale(LC_CTYPE, value.c_str()); }
    } restore{previous};
    TempTree tree;
    registries(tree.root);
    const std::string id = std::string(1, static_cast<char>(0xa0)) + "ID" + static_cast<char>(0xa0);
    write(tree.root / "XML" / "objects-a.xml",
          "<Objects><SpaceUnit Name=\" \t" + id + " \t\"><Tactical_Health>1</Tactical_Health></SpaceUnit></Objects>");
    write(tree.root / "XML" / "objects-b.xml", "<Objects/>");
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "locale fixture mounts");
    if (!mounted) return;
    auto check = [&] {
        auto loaded = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::eaw);
        expect(static_cast<bool>(loaded), "locale fixture loads");
        if (!loaded) return;
        expect(loaded.value().catalog.find(id) != nullptr, "extended bytes adjacent to ID survive XML trimming");
        expect(loaded.value().catalog.find("ID") == nullptr, "extended byte is not XML whitespace");
    };
    std::setlocale(LC_CTYPE, "C");
    check();
    bool changed = false;
    for (const char* name : {".1252", "en_US.ISO8859-1", "C.UTF-8", "en_US.UTF-8", ".UTF8"}) {
        const char* selected = std::setlocale(LC_CTYPE, name);
        if (selected != nullptr && std::string_view(selected) != "C") {
            changed = true;
            check();
        }
    }
    expect(changed, "at least one non-C LC_CTYPE is exercised");
}

void run_contracts() {
    locale_contracts();
    TempTree tree;
    registries(tree.root);
    write(tree.root / "XML" / "objects-a.xml", R"xml(<Objects>
<SpaceUnit Name="BASE"
 zAttr="Z"
 aAttr="A">
  <ReplaceTag source="base">base</ReplaceTag>
  <MergeTag>A, A</MergeTag>
  <Tuple>one, 1</Tuple><Tuple>two, 2</Tuple><Tuple>one, 1</Tuple>
  <Empty>base</Empty><Ignored>base-only</Ignored>
  <Tactical_Health>100</Tactical_Health>
  <Nested mode="base"><Leaf>base</Leaf><Tactical_Health>999</Tactical_Health></Nested>
  <BaseOnly>kept</BaseOnly><Numeric> 1.5 </Numeric><NumericOverflow>999999999999999999999999</NumericOverflow>
  <Death_Clone>Damage_Fire, Base_Clone</Death_Clone>
</SpaceUnit>
<SpaceUnit Name="MID"><Variant_Of_Existing_Type>base</Variant_Of_Existing_Type>
  <ReplaceTag>mid</ReplaceTag><MergeTag>B</MergeTag><Tuple>three, 3</Tuple>
  <Ignored>mid-ignored</Ignored><Death_Clone>Damage_Fire, Mid_Clone</Death_Clone>
</SpaceUnit>
<SpaceUnit Name="TOP"><Variant_Of_Existing_Type>mId</Variant_Of_Existing_Type>
  <ReplaceTag>top</ReplaceTag><MergeTag>C</MergeTag><Empty></Empty><IgnoredNew>derived-only</IgnoredNew>
  <Nested mode="top"><Leaf><Deep>top</Deep></Leaf></Nested><TopOnly>new</TopOnly>
</SpaceUnit>
<SpaceUnit Name="MISSING"><Variant_Of_Existing_Type>NO_BASE</Variant_Of_Existing_Type></SpaceUnit>
<SpaceUnit Name="CYCLE_A"><Variant_Of_Existing_Type>CYCLE_B</Variant_Of_Existing_Type></SpaceUnit>
<SpaceUnit Name="CYCLE_B"><Variant_Of_Existing_Type>cycle_a</Variant_Of_Existing_Type></SpaceUnit>
<SpaceUnit Name="DUP"><ReplaceTag>first</ReplaceTag></SpaceUnit>
<SpaceUnit Name="ABILITY_OWNER"><Abilities><Lucky_Shot_Attack_Ability Name="LUCKY"><Damage>9</Damage><Definitely_Unknown_Ability_Field>raw</Definitely_Unknown_Ability_Field></Lucky_Shot_Attack_Ability></Abilities></SpaceUnit>
</Objects>)xml");
    write(tree.root / "XML" / "objects-b.xml", R"xml(<Objects>
<SpaceUnit Name="dup"><ReplaceTag>second</ReplaceTag></SpaceUnit>
<SpaceUnit Name="DUP_LAYER"><ReplaceTag>base-later-include</ReplaceTag></SpaceUnit>
</Objects>)xml");
    write(tree.mod_root / "XML" / "GameObjectFiles.xml",
          "<Game_Object_Files><File>mod-objects.xml</File><File>objects-a.xml</File><File>objects-b.xml</File><File>numeric-root.xml</File></Game_Object_Files>");
    write(tree.mod_root / "XML" / "mod-objects.xml",
          "<Objects><SpaceUnit Name=\"DUP_LAYER\"><ReplaceTag>mod-stronger-layer</ReplaceTag></SpaceUnit></Objects>");
    write(tree.mod_root / "XML" / "numeric-root.xml",
          "<?xml version=\"1.0\"?><181st><SpaceUnit Name=\"NUMERIC_ROOT\"><Value>accepted</Value></SpaceUnit></181st>");
    write(tree.root / "XML" / "hardpoints.xml",
          "<HardPoints><HardPoint Name=\"HP_A\"><Damage>1</Damage></HardPoint></HardPoints>");
    write(tree.root / "XML" / "factions.xml", R"xml(<Factions><Faction Name="FACTION_A">
<Standalone_Space_Maps_Special_Weapon_B>OLD</Standalone_Space_Maps_Special_Weapon_B>
</Faction></Factions>)xml");
    write(tree.root / "XML" / "campaigns.xml",
          "<Campaigns><Campaign Name=\"CAMPAIGN_A\"><Starting_Credits>Empire, 1000</Starting_Credits></Campaign></Campaigns>");
    write(tree.root / "XML" / "sfx.xml",
          "<SFXEvents><SFXEvent Name=\"SFX_A\"><Samples>a.wav</Samples></SFXEvent></SFXEvents>");
    write(tree.root / "XML" / "disabled-malformed.xml", "<Disabled><Broken></Disabled>");
    write(tree.root / "XML" / "disabled-numeric-child.xml", "<Disabled><1bad/></Disabled>");
    write(tree.root / "XML" / "disabled-doctype.xml", "<!DOCTYPE Disabled><Disabled/>");

    const std::array mounts{
        eawr::vfs::MountSpec{"mod", tree.mod_root, "data", {}},
        eawr::vfs::MountSpec{"base", tree.root, "data", {}},
    };
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "synthetic data VFS mounts");
    if (!mounted) return;

    eawr::data::LoadOptions options{{
        {"GameObjectType", "MergeTag", eawr::data::MergeMode::merge, false},
        {"GameObjectType", "Tuple", eawr::data::MergeMode::merge, true},
        {"GameObjectType", "Ignored", eawr::data::MergeMode::ignored, false},
        {"GameObjectType", "IgnoredNew", eawr::data::MergeMode::ignored, false},
        {"GameObjectType", "ReplaceTag", eawr::data::MergeMode::replace, false},
    }};
    auto loaded = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::eaw, options);
    expect(static_cast<bool>(loaded), "catalog loads with diagnostics separated from fatal VFS failure");
    if (!loaded) return;
    auto& catalog = loaded.value().catalog;
    expect(catalog.registry_roots().size() == 5, "each registry root has one owned attempt record");
    expect(catalog.registry_roots()[0].registry_path == "data/xml/gameobjectfiles.xml" &&
               catalog.registry_roots()[0].outcome == "loaded" &&
               catalog.registry_roots()[0].input_sha256.has_value() &&
               catalog.registry_roots()[0].source && catalog.registry_roots()[0].source->layer_id == "mod",
           "effective registry root retains its source and exact input digest");
    expect(!catalog.registry_files().empty() && catalog.registry_files()[0].outcome == "loaded" &&
               catalog.registry_files()[0].input_sha256.has_value() &&
               catalog.registry_files()[0].source && catalog.registry_files()[0].source->layer_id == "mod",
           "first include retains its winning source and digest");
    const auto manifest_records = std::count_if(catalog.physical_inventory().begin(),
        catalog.physical_inventory().end(), [](const auto& file) {
            return file.record.canonical_path == "data/xml/gameobjectfiles.xml";
        });
    const auto active_manifest_records = std::count_if(catalog.physical_inventory().begin(),
        catalog.physical_inventory().end(), [](const auto& file) {
            return file.record.canonical_path == "data/xml/gameobjectfiles.xml" &&
                   file.active_registry_file && file.parsed;
        });
    const auto shadow_manifest_records = std::count_if(catalog.physical_inventory().begin(),
        catalog.physical_inventory().end(), [](const auto& file) {
            return file.record.canonical_path == "data/xml/gameobjectfiles.xml" &&
                   !file.active_registry_file && !file.parsed && file.outcome &&
                   file.outcome->find("shadowed/inactive") != std::string::npos;
        });
    if (manifest_records != 2 || active_manifest_records != 1 || shadow_manifest_records != 1) {
        std::cerr << "physical inventory manifest records=" << manifest_records
                  << " active=" << active_manifest_records << " shadow=" << shadow_manifest_records << '\n';
        for (const auto& file : catalog.physical_inventory()) {
            if (file.record.canonical_path == "data/xml/gameobjectfiles.xml") {
                std::cerr << "  source=" << file.record.source_id << " active=" << file.active_registry_file
                          << " parsed=" << file.parsed << " outcome=" << file.outcome.value_or("<none>") << '\n';
            }
        }
    }
    expect(manifest_records == 2 && active_manifest_records == 1 && shadow_manifest_records == 1,
           "physical inventory separates effective parsed bytes from shadowed layer metadata");
    expect(catalog.find("top") != nullptr && catalog.find("ToP") == catalog.find("TOP"),
           "object IDs resolve case-insensitively");
    expect(catalog.find("DUP") != nullptr && catalog.find_all("dup").size() == 2,
           "duplicates remain available to the raw API");
    expect(catalog.find("DUP") && catalog.find("DUP")->root.children[0].raw_text == "second",
           "later same-layer registry include wins deterministically");
    expect(catalog.find("DUP_LAYER") &&
               catalog.find("DUP_LAYER")->root.children[0].raw_text == "mod-stronger-layer",
           "stronger VFS layer wins even when a weaker definition is later in registry order");
    if (!catalog.find("NUMERIC_ROOT")) {
        for (const auto& value : loaded.value().diagnostics) {
            if (value.logical_path && value.logical_path->find("numeric-root") != std::string::npos) {
                std::cerr << value.code << ": " << value.message << '\n';
            }
        }
    }
    expect(catalog.find("NUMERIC_ROOT") && catalog.find("NUMERIC_ROOT")->root.children[0].raw_text == "accepted",
           "measured numeric-leading document container compatibility retains child definitions");
    expect(has_code(loaded.value().diagnostics, eawr::data::diagnostic_codes::duplicate_id),
           "duplicate IDs produce a stable diagnostic");
    expect(has_code(loaded.value().diagnostics, eawr::data::diagnostic_codes::unknown_field),
           "unknown fields are diagnosed without rejection");
    expect(has_code(loaded.value().diagnostics, eawr::data::diagnostic_codes::deprecated_field),
           "deprecated pinned-schema field is diagnosed without rejection");
    expect(std::count_if(loaded.value().diagnostics.begin(), loaded.value().diagnostics.end(),
               [](const auto& value) {
                   return value.code == eawr::data::diagnostic_codes::unknown_field &&
                          value.message.find("Definitely_Unknown_Ability_Field") != std::string::npos;
               }) == 1,
           "nested definition schema diagnostics use their own boundary and are emitted once");
    expect(has_code(loaded.value().diagnostics, eawr::data::diagnostic_codes::malformed_xml),
           "malformed unreferenced physical XML remains visible");
    expect(has_code(loaded.value().diagnostics, eawr::data::diagnostic_codes::forbidden_doctype),
           "doctype input is rejected without entity processing");
    expect(std::any_of(loaded.value().diagnostics.begin(), loaded.value().diagnostics.end(),
               [](const auto& value) {
                   return value.code == eawr::data::diagnostic_codes::malformed_xml && value.logical_path &&
                          *value.logical_path == "data/xml/disabled-numeric-child.xml";
               }),
           "numeric-leading compatibility never repairs a child element");

    inheritance_contracts(catalog, mounted, options);
    if (mounted) override_contracts(catalog, mounted.value());
    category_contracts(catalog);
    schema_contracts(catalog);
}

void category_contracts(const eawr::data::Catalog& catalog) {
    const auto categories = [&](const eawr::data::Category category) {
        return std::count_if(catalog.definitions().begin(), catalog.definitions().end(),
                             [&](const auto& value) { return value.category == category; });
    };
    expect(categories(eawr::data::Category::game_object) > 0 &&
               categories(eawr::data::Category::hardpoint) == 1 &&
               categories(eawr::data::Category::ability) == 1 &&
               categories(eawr::data::Category::faction) == 1 &&
               categories(eawr::data::Category::campaign) == 1 &&
               categories(eawr::data::Category::sfx) == 1,
           "all six requested active catalog categories are populated");
    const auto* lucky = catalog.find("lucky");
    expect(lucky && lucky->category == eawr::data::Category::ability && lucky->root.source.line > 0,
           "nested ability is indexed with line provenance");
}
} // namespace eawr::tests::data_contracts
