#include "../support/temp_dir.hpp"
#include "eawr/vfs/vfs.hpp"
#include "data_test_support.hpp"
#include "eawr/data/xml.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <ios>
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
        eawr::testing::remove_temp_directory(root);
        eawr::testing::remove_temp_directory(mod_root);
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

// Parse failures are reported and skipped while neighboring definitions remain available.
// Other registry and read failures retain their existing health treatment.
void load_health_contracts() {
    struct Variant {
        std::string hardpoint_registry =
            "<Hard_Point_Files><File>hardpoints-a.xml</File><File>hardpoints-b.xml</File></Hard_Point_Files>";
        std::string object_registry = "<Game_Object_Files><File>objects-a.xml</File></Game_Object_Files>";
        std::string hardpoints_a = "<HardPoints><HardPoint Name=\"HP_A\"><Damage>1</Damage></HardPoint></HardPoints>";
        std::string hardpoints_b = "<HardPoints><HardPoint Name=\"HP_B\"><Damage>2</Damage></HardPoint></HardPoints>";
        std::string objects_a = "<Objects><SpaceUnit Name=\"UNIT_A\"><Tactical_Health>5</Tactical_Health></SpaceUnit></Objects>";
        bool drop_hardpoints_b = false;
        bool drop_faction_registry = false;
        bool stray_malformed_file = false;
    };
    const auto load = [](const Variant& variant, const bool strict) {
        TempTree tree;
        registries(tree.root);
        write(tree.root / "XML" / "HardpointDataFiles.xml", variant.hardpoint_registry);
        write(tree.root / "XML" / "GameObjectFiles.xml", variant.object_registry);
        write(tree.root / "XML" / "hardpoints-a.xml", variant.hardpoints_a);
        if (!variant.drop_hardpoints_b) write(tree.root / "XML" / "hardpoints-b.xml", variant.hardpoints_b);
        write(tree.root / "XML" / "objects-a.xml", variant.objects_a);
        write(tree.root / "XML" / "factions.xml", "<Factions><Faction Name=\"FACTION_A\"/></Factions>");
        write(tree.root / "XML" / "campaigns.xml", "<Campaigns><Campaign Name=\"CAMPAIGN_A\"/></Campaigns>");
        write(tree.root / "XML" / "sfx.xml", "<SFXEvents><SFXEvent Name=\"SFX_A\"/></SFXEvents>");
        if (variant.drop_faction_registry) std::filesystem::remove(tree.root / "XML" / "FactionFiles.xml");
        if (variant.stray_malformed_file) write(tree.root / "XML" / "unreferenced.xml", "<Broken><Open></Broken>");
        const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
        auto mounted = eawr::vfs::Vfs::mount(mounts);
        expect(static_cast<bool>(mounted), "load-health tree mounts");
        if (!mounted) return eawr::core::Result<eawr::data::LoadResult>::failure(mounted.error());
        return strict ? eawr::data::load_catalog_strict(mounted.value(), eawr::data::Profile::eaw)
                      : eawr::data::load_catalog(mounted.value(), eawr::data::Profile::eaw);
    };

    {
        Variant clean;
        clean.stray_malformed_file = true; // an unreferenced malformed file is a diagnostic, not a health problem
        auto loaded = load(clean, true);
        expect(static_cast<bool>(loaded) && loaded.value().health.clean() && !loaded.value().health.failed(),
               "a clean tree has a clean health and passes the strict load");
        expect(loaded && has_code(loaded.value().diagnostics, eawr::data::diagnostic_codes::malformed_xml),
               "the unreferenced malformed file stays a diagnostic");
        // XML R-12: the original reader never opens a file no registry names, so the stray file leaves no load note.
        expect(loaded && std::none_of(loaded.value().notes.begin(), loaded.value().notes.end(), [](const auto& note) {
                   return note.code == eawr::core::load_note_codes::skipped_xml_file;
               }), "an unreferenced malformed file leaves a diagnostic but no skipped-file load note");
        expect(loaded && eawr::data::describe_load_health(loaded.value().health).empty(), "a clean load describes nothing");
    }
    {
        Variant sloppy;
        sloppy.hardpoints_a = "<?xmlversion=\"1.0\"?><HardPoints><HardPoint Name=\"HP_A\"/></HardPoints>";
        auto loaded = load(sloppy, true);
        expect(loaded && loaded.value().catalog.find("HP_A", eawr::data::Category::hardpoint),
               "missing declaration space is accepted in a hardpoint include");
        expect(loaded && std::any_of(loaded.value().notes.begin(), loaded.value().notes.end(), [](const auto& note) {
            return note.code == eawr::core::load_note_codes::compatible_xml_header &&
                   note.file == "data/xml/hardpoints-a.xml" && note.line == 1;
        }), "accepted declaration produces a file-specific load note");
        expect(loaded && !has_code(loaded.value().diagnostics, eawr::data::diagnostic_codes::malformed_xml),
               "accepted declaration produces no malformed XML diagnostic");
        for (const std::string header : {"<?xmlversion=\"unusual\"?>", "<?xmlencoding=\"odd\"?>",
                                         " \n<?xmlversion=\"1.0\"?>", "  <?xmlversion=\"1.0\"  ?>", "\xef\xbb\xbf<?xmlversion=\"1.0\"?>"}) {
            sloppy.hardpoints_a = header + "<HardPoints><HardPoint Name=\"HP_A\"/></HardPoints>";
            auto variant = load(sloppy, true);
            expect(variant && variant.value().catalog.find("HP_A", eawr::data::Category::hardpoint),
                   "compatible XML header variant loads");
            expect(variant && std::any_of(variant.value().notes.begin(), variant.value().notes.end(), [](const auto& note) {
                return note.code == eawr::core::load_note_codes::compatible_xml_header;
            }), "compatible XML header variant emits a note");
        }
        sloppy.hardpoints_a = "<?xmlversion=\"1.0\"<HardPoints";
        auto unclosed = load(sloppy, true);
        expect(unclosed && has_code(unclosed.value().diagnostics, eawr::data::diagnostic_codes::malformed_xml),
               "unterminated header remains a parse failure");
        sloppy.hardpoints_a = "<?xmlversion=\"1.0\"?><HardPoints><Broken></HardPoints>";
        auto broken = load(sloppy, true);
        expect(broken && has_code(broken.value().diagnostics, eawr::data::diagnostic_codes::malformed_xml) &&
                   broken.value().catalog.find("HP_A", eawr::data::Category::hardpoint) == nullptr,
               "a malformed document is skipped after the compatibility retry");
    }
    {
        Variant malformed;
        malformed.hardpoints_b = "<HardPoints><HardPoint Name=\"HP_B\"></HardPoints>";
        auto lenient = load(malformed, false);
        expect(static_cast<bool>(lenient), "load_catalog still returns the catalog and its diagnostics");
        if (lenient) {
            const auto& health = lenient.value().health;
            expect(!health.failed() && health.hardpoint_include_failed == 1 && health.include_failed == 1 &&
                       health.registry_invalid == 0 && health.include_missing == 0,
                   "a malformed hardpoint include is counted without failing the load");
            expect(lenient.value().catalog.find("HP_A", eawr::data::Category::hardpoint) != nullptr,
                   "the neighbouring hardpoint include still loads");
            expect(eawr::data::describe_load_health(health) == "include_failed=1 hardpoint_include_failed=1",
                   "the health line names each non-zero count");
        }
        auto strict = load(malformed, true);
        expect(strict && strict.value().catalog.find("HP_A", eawr::data::Category::hardpoint) != nullptr &&
                   strict.value().catalog.find("HP_B", eawr::data::Category::hardpoint) == nullptr &&
                   std::any_of(strict.value().notes.begin(), strict.value().notes.end(), [](const auto& note) {
                       return note.code == eawr::core::load_note_codes::skipped_xml_file &&
                              note.file == "data/xml/hardpoints-b.xml";
                   }), "strict load skips and notes the malformed file while keeping its neighbor");
        expect(strict && std::count_if(strict.value().notes.begin(), strict.value().notes.end(), [](const auto& note) {
                   return note.code == eawr::core::load_note_codes::skipped_xml_file &&
                          note.file == "data/xml/hardpoints-b.xml";
               }) == 1, "a registered malformed file leaves exactly one skipped-file note");
    }
    {
        TempTree tree;
        registries(tree.root);
        write(tree.root / "XML" / "HardpointDataFiles.xml",
              "<Hard_Point_Files><File>hardpoints-a.xml</File><File>hardpoints-b.xml</File></Hard_Point_Files>");
        write(tree.root / "XML" / "hardpoints-a.xml", "<HardPoints><HardPoint Name=\"HP_A\"/></HardPoints>");
        write(tree.root / "XML" / "hardpoints-b.xml", "<HardPoints><HardPoint Name=\"HP_B\"/></HardPoints>");
        write(tree.mod_root / "XML" / "hardpoints-b.xml", "<HardPoints><HardPoint></HardPoints>");
        const std::array mounts{eawr::vfs::MountSpec{"mod", tree.mod_root, "data", {}},
                                eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
        auto mounted = eawr::vfs::Vfs::mount(mounts);
        expect(static_cast<bool>(mounted), "malformed mod chain mounts");
        if (mounted) {
            auto loaded = eawr::data::load_catalog_strict(mounted.value(), eawr::data::Profile::eaw);
            expect(loaded && loaded.value().catalog.find("HP_A", eawr::data::Category::hardpoint) &&
                       !loaded.value().catalog.find("HP_B", eawr::data::Category::hardpoint),
                   "malformed winning mod file is skipped and its neighbor loads");
            expect(loaded && std::any_of(loaded.value().notes.begin(), loaded.value().notes.end(), [](const auto& note) {
                return note.code == eawr::core::load_note_codes::skipped_xml_file &&
                       note.layer.mod_id == "mod" && note.file == "data/xml/hardpoints-b.xml";
            }), "skipped mod file is named in a load note");
        }
    }
    {
        Variant missing;
        missing.hardpoint_registry =
            "<Hard_Point_Files><File>hardpoints-a.xml</File><File>absent.xml</File><File>hardpoints-b.xml</File></Hard_Point_Files>";
        auto strict = load(missing, true);
        expect(static_cast<bool>(strict), "a missing include is omitted with a warning, not a failure (R-10, R-13)");
        if (strict) {
            const auto& health = strict.value().health;
            expect(!health.failed() && health.include_missing == 1 && health.include_failed == 0 && !health.clean(),
                   "a missing include is counted as a warning only");
            expect(has_code(strict.value().diagnostics, eawr::data::diagnostic_codes::registry_include_missing),
                   "the missing include keeps its diagnostic");
            expect(std::any_of(strict.value().diagnostics.begin(), strict.value().diagnostics.end(), [](const auto& d) {
                return d.code == eawr::data::diagnostic_codes::registry_include_missing &&
                       d.severity == eawr::core::Severity::warning;
            }), "a missing include is a warning, never an error (mod data mistake, R-10)");
            expect(strict.value().catalog.find("HP_A", eawr::data::Category::hardpoint) != nullptr &&
                       strict.value().catalog.find("HP_B", eawr::data::Category::hardpoint) != nullptr,
                   "both neighbours of a missing include are present");
            expect(eawr::data::describe_load_health(health) == "include_missing=1", "the warning line names the missing include");
        }
    }
    {
        Variant object_include;
        object_include.objects_a = "<Objects><SpaceUnit Name=\"UNIT_A\"></Objects>";
        auto strict = load(object_include, true);
        expect(static_cast<bool>(strict) && strict.value().health.include_failed == 1 &&
                   strict.value().health.hardpoint_include_failed == 0 && !strict.value().health.failed(),
               "a malformed game-object include is counted and reported but not fatal (the issue's scope is hardpoints)");
    }
    {
        Variant wrong_root;
        wrong_root.hardpoint_registry = "<Wrong_Root><File>hardpoints-a.xml</File></Wrong_Root>";
        auto strict = load(wrong_root, true);
        expect(!strict && strict.error().code == eawr::data::diagnostic_codes::registry_invalid,
               "a registry with the wrong root fails the strict load");
        auto lenient = load(wrong_root, false);
        expect(lenient && lenient.value().health.registry_invalid == 1 && lenient.value().health.failed(),
               "a wrong registry root is counted");
        Variant unparsable;
        unparsable.hardpoint_registry = "<Hard_Point_Files><File>x</Hard_Point_Files";
        auto broken = load(unparsable, true);
        expect(broken && has_code(broken.value().diagnostics, eawr::data::diagnostic_codes::registry_invalid) &&
                   std::any_of(broken.value().notes.begin(), broken.value().notes.end(), [](const auto& note) {
                       return note.code == eawr::core::load_note_codes::skipped_xml_file;
                   }), "an unparsable registry is skipped with a load note");
        Variant gone;
        gone.drop_faction_registry = true;
        auto absent = load(gone, true);
        expect(!absent && absent.error().code == eawr::data::diagnostic_codes::registry_invalid,
               "a missing registry file fails the strict load");
    }
}

// SG-CLS-01: FoC reads no class from an element name. A mod's own element name takes the class its variant chain
// names, else the class its effective tags imply; the origin is recorded so a loader can log the row.
void object_class_contracts() {
    TempTree tree;
    registries(tree.root);
    write(tree.root / "XML" / "objects-a.xml", R"xml(<Objects>
<SpaceUnit Name="STOCK_SHIP"><Tactical_Health>10</Tactical_Health></SpaceUnit>
<Space_Unit Name="CUSTOM_SHIP"><Variant_Of_Existing_Type>stock_ship</Variant_Of_Existing_Type></Space_Unit>
<Template Name="TEMPLATE_SQUAD"><Squadron_Units>stock_ship, 3</Squadron_Units></Template>
<Template Name="TEMPLATE_HERO"><Is_Named_Hero>Yes</Is_Named_Hero></Template>
<Template Name="TEMPLATE_PLAIN"><Tactical_Health>5</Tactical_Health></Template>
<Template Name="TEMPLATE_DUMMY_BASE"><Behavior>SELECTABLE, DUMMY_STAR_BASE</Behavior></Template>
<Template Name="TEMPLATE_HERO_COMPANY"><Company_Units>stock_ship, 2</Company_Units><Is_Named_Hero>Yes</Is_Named_Hero></Template>
<Template Name="TEMPLATE_UPGRADE"><CategoryMask>Upgrade</CategoryMask></Template>
<Deathclone Name="CLONE_SQUAD"><Variant_Of_Existing_Type>template_squad</Variant_Of_Existing_Type></Deathclone>
<SpaceHero Name="LONE_HERO"><Variant_Of_Existing_Type>template_hero</Variant_Of_Existing_Type></SpaceHero>
<Deathclone Name="LONE_PLAIN"><Variant_Of_Existing_Type>template_plain</Variant_Of_Existing_Type></Deathclone>
<Ground_Structure_Tauri Name="LONE_BASE"><Variant_Of_Existing_Type>template_dummy_base</Variant_Of_Existing_Type></Ground_Structure_Tauri>
<Props_Wave Name="LONE_COMPANY"><Company_Units>stock_ship, 2</Company_Units></Props_Wave>
<Props_Wave Name="LONE_HERO_COMPANY"><Variant_Of_Existing_Type>template_hero_company</Variant_Of_Existing_Type></Props_Wave>
<Props_Wave Name="LONE_UPGRADE"><Variant_Of_Existing_Type>template_upgrade</Variant_Of_Existing_Type></Props_Wave>
<Space_Unit Name="ANCESTOR_BEATS_TAGS"><Variant_Of_Existing_Type>stock_ship</Variant_Of_Existing_Type><Squadron_Units>stock_ship, 3</Squadron_Units></Space_Unit>
<Squadron Name="REAL_SQUAD"><Squadron_Units>stock_ship, 3</Squadron_Units></Squadron>
<SpaceUnit Name="SQUAD_VARIANT_LABELLED_SHIP"><Variant_Of_Existing_Type>real_squad</Variant_Of_Existing_Type><Affiliation>Rebel</Affiliation></SpaceUnit>
<SpaceUnit Name="SQUAD_VARIANT_OF_VARIANT"><Variant_Of_Existing_Type>squad_variant_labelled_ship</Variant_Of_Existing_Type></SpaceUnit>
<SpaceUnit Name="SHIP_VARIANT_WITH_OWN_UNITS"><Variant_Of_Existing_Type>stock_ship</Variant_Of_Existing_Type><Squadron_Units>stock_ship, 3</Squadron_Units></SpaceUnit>
<SpaceUnit Name="BOTH_LISTS_KEEP_CLASS"><Variant_Of_Existing_Type>stock_ship</Variant_Of_Existing_Type><Squadron_Units>stock_ship, 3</Squadron_Units><Company_Units>stock_ship, 2</Company_Units></SpaceUnit>
<Space_Unit Name="LABEL_OVER_SHIP_OVER_SQUAD"><Variant_Of_Existing_Type>squad_variant_labelled_ship</Variant_Of_Existing_Type></Space_Unit>
</Objects>)xml");
    write(tree.root / "XML" / "objects-b.xml", "<Objects/>");
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "class fixture mounts");
    if (!mounted) return;
    auto loaded = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::eaw);
    expect(static_cast<bool>(loaded), "class fixture loads");
    if (!loaded) return;
    const auto resolve = [&](const char* id) { return loaded.value().catalog.resolve(id, eawr::data::Category::game_object); };
    const auto check = [&](const char* id, const char* type, const char* origin) {
        const auto object = resolve(id);
        expect(static_cast<bool>(object), std::string(id) + " resolves");
        if (!object) return;
        expect(object.value().type_name == type, std::string(id) + " class is " + type + ", got " + object.value().type_name);
        expect(object.value().class_origin == origin, std::string(id) + " origin is " + origin + ", got " + object.value().class_origin);
    };
    check("STOCK_SHIP", "SpaceUnit", "");
    check("CUSTOM_SHIP", "SpaceUnit", "ancestor:SpaceUnit");
    check("CLONE_SQUAD", "Squadron", "tags:Squadron");
    check("LONE_HERO", "HeroUnit", "tags:HeroUnit");
    check("LONE_PLAIN", "Deathclone", "unclassed");
    check("LONE_BASE", "StarBase", "tags:StarBase");
    check("LONE_COMPANY", "GroundCompany", "tags:GroundCompany");
    check("LONE_HERO_COMPANY", "HeroCompany", "tags:HeroCompany");
    check("LONE_UPGRADE", "UpgradeObject", "tags:UpgradeObject");
    // SG-CLS-01: the effective Squadron_Units decide, not the element name and not an ancestor.
    check("ANCESTOR_BEATS_TAGS", "Squadron", "tags:Squadron");
    // A FoC class name on the element is a label too: the object creator places a squadron from Squadron_Units alone, so
    // any non-company object that lists squadron units is a Squadron (a mod's SpaceUnit variant of a squadron template
    // carries no hull of its own, its craft do). A company keeps its class: a type with both lists uses one per mode.
    check("REAL_SQUAD", "Squadron", "");
    check("SQUAD_VARIANT_LABELLED_SHIP", "Squadron", "tags:Squadron");
    check("SQUAD_VARIANT_OF_VARIANT", "Squadron", "tags:Squadron");
    check("SHIP_VARIANT_WITH_OWN_UNITS", "Squadron", "tags:Squadron");
    check("BOTH_LISTS_KEEP_CLASS", "SpaceUnit", "");
    check("LABEL_OVER_SHIP_OVER_SQUAD", "Squadron", "tags:Squadron");
    expect(eawr::data::is_foc_object_class("spaceUNIT") && !eawr::data::is_foc_object_class("Space_Unit"),
           "FoC class names match without case, a mod label does not");
}

void primary_sfx_repeats_unchanged_contract() {
    TempTree tree;
    registries(tree.root);
    write(tree.root / "XML" / "SFXEventFiles.xml",
        "<SFXEvent_Files><File>sfx.xml</File><File>sfx.xml</File></SFXEvent_Files>");
    write(tree.root / "XML" / "sfx.xml", "<SFXEvents><SFXEvent Name=\"Repeated\"/></SFXEvents>");
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "SH-03 primary repeats fixture mounts");
    if (!filesystem) return;
    auto loaded = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(loaded && loaded.value().catalog.find_all("Repeated").size() == 2,
        "SH-03 without additions the primary registry keeps main's repeated include behavior");
}

void additive_sfx_contracts() {
    TempTree tree;
    registries(tree.root);
    write(tree.root / "XML" / "sfx.xml", R"xml(<SFXEvents>
<SFXEvent Name="Existing"><Samples>base.wav</Samples></SFXEvent>
<SFXEvent Name="Shared"><Samples>base.wav</Samples></SFXEvent></SFXEvents>)xml");
    write(tree.mod_root / "XML" / "SFXEventFilesAdditions.xml",
        "<SFXEvent_Files><File>revised.xml</File><File>sfx.xml</File></SFXEvent_Files>");
    write(tree.mod_root / "XML" / "revised.xml", R"xml(<SFXEvents>
<SFXEvent Name="Added"><Samples>added.wav</Samples></SFXEvent>
<SFXEvent Name="Shared"><Samples>revised.wav</Samples></SFXEvent></SFXEvents>)xml");
    const auto base = tree.root / "older-base";
    write(base / "XML" / "SFXEventFiles.xml", "<SFXEvent_Files><File>removed.xml</File></SFXEvent_Files>");
    write(base / "XML" / "removed.xml", "<SFXEvents><SFXEvent Name=\"RemovedByExpansion\"/></SFXEvents>");
    const std::array mounts{eawr::vfs::MountSpec{"mod", tree.mod_root, "data", {}},
        eawr::vfs::MountSpec{"expansion", tree.root, "data", {}},
        eawr::vfs::MountSpec{"base", base, "data", {}}};
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "SH-03 additive SFX fixture mounts");
    if (!filesystem) return;
    auto loaded = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(loaded), "SH-03 additive catalog loads");
    if (!loaded) return;
    const auto& catalog = loaded.value().catalog;
    const auto* shared = catalog.find("Shared", eawr::data::Category::sfx);
    expect(catalog.find("Existing", eawr::data::Category::sfx)
        && catalog.find("Added", eawr::data::Category::sfx)
        && shared && shared->root.source.layer_id == "mod", "SH-03 layer events preserve base events and obey priority");
    expect(!catalog.find("RemovedByExpansion", eawr::data::Category::sfx),
        "SH-03 additions do not revive omitted base primary registry events");
    expect(catalog.find_all("Existing").size() == 1, "SH-03 repeated registry includes load once");
}

void shadowed_definitions_contract() {
    TempTree tree;
    registries(tree.root);
    write(tree.mod_root / "XML" / "GameObjectFiles.xml",
          "<Game_Object_Files><File>objects-a.xml</File></Game_Object_Files>");
    write(tree.root / "XML" / "objects-a.xml",
          "<Objects>\n  <SpaceUnit Name=\"A\"><Tactical_Health>1</Tactical_Health></SpaceUnit>\n"
          "  <SpaceUnit Name=\"A\"><Tactical_Health>2</Tactical_Health></SpaceUnit></Objects>");
    write(tree.root / "XML" / "objects-b.xml", "<Objects><SpaceUnit Name=\"Dropped\"/></Objects>");
    write(tree.mod_root / "XML" / "objects-a.xml", "<Objects><SpaceUnit Name=\"A\"/></Objects>");

    // A single MEG-v1 entry in the mod layer, shadowed by its loose file.
    const std::string name = "DATA/XML/objects-a.xml";
    const std::string payload = "<Objects>\n <SpaceUnit Name=\"Archive\"/></Objects>";
    std::vector<std::byte> meg;
    const auto u32 = [&meg](std::uint32_t n) {
        for (unsigned shift = 0; shift < 32; shift += 8) meg.push_back(static_cast<std::byte>((n >> shift) & 255U));
    };
    u32(1); u32(1);
    meg.push_back(static_cast<std::byte>(name.size())); meg.push_back(std::byte{0});
    for (const char ch : name) meg.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    u32(0); u32(0); u32(static_cast<std::uint32_t>(payload.size()));
    u32(static_cast<std::uint32_t>(8 + 2 + name.size() + 20)); u32(0);
    for (const char ch : payload) meg.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    const auto archive = tree.mod_root / "objects.meg";
    { std::ofstream out(archive, std::ios::binary); out.write(reinterpret_cast<const char*>(meg.data()), static_cast<std::streamsize>(meg.size())); }
    const std::array mounts{eawr::vfs::MountSpec{"mod", tree.mod_root, "data", {{archive, "mod-archive", ""}}},
                            eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "shadow fixture mounts");
    if (!mounted) return;
    auto loaded = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    auto baseline = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    expect(loaded && baseline, "shadow fixture loads");
    if (!loaded || !baseline) return;
    auto& catalog = loaded.value().catalog;
    auto shadow = eawr::data::load_shadowed_definitions(mounted.value(), catalog);
    expect(static_cast<bool>(shadow), "shadow diagnostic loads");
    if (!shadow) return;
    const auto& defs = shadow.value().definitions;
    expect(defs.size() == 3, "archive and duplicate lower definitions retained");
    if (defs.size() == 3) {
        expect(defs[0].id == "Archive" && defs[0].root.source.layer_id == "mod" &&
            defs[0].root.source.source_id == "mod-archive" && defs[0].root.source.logical_path == "data/xml/objects-a.xml" &&
            defs[0].root.source.line == 2 && defs[0].root.source.column == 3, "archive candidate attribution and order");
        expect(defs[1].id == "A" && defs[2].id == "A" && defs[1].root.source.layer_id == "base" &&
            defs[1].root.source.line == 2 && defs[1].root.source.column == 4 && defs[2].definition_order == 1 &&
            defs[1].registry_order == 0 && defs[2].registry_order == 0, "lower duplicates retain location and order");
    }
    expect(catalog.find("Dropped") == nullptr && std::none_of(defs.begin(), defs.end(),
        [](const auto& d) { return d.id == "Dropped"; }), "registry replacement excludes inactive lower include");
    const auto* winner = catalog.find("A");
    const auto* original = baseline.value().catalog.find("A");
    expect(winner && original && winner->root.children.empty() && original->root.children.empty() &&
        winner->root.source.source_id == original->root.source.source_id &&
        catalog.definitions().size() == baseline.value().catalog.definitions().size() &&
        catalog.find_all("A").size() == baseline.value().catalog.find_all("A").size(),
        "empty override and effective catalog match independent load");
}

void run_contracts() {
    shadowed_definitions_contract();
    primary_sfx_repeats_unchanged_contract();
    additive_sfx_contracts();
    locale_contracts();
    object_class_contracts();
    load_health_contracts();
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
  <Death_Clone>Damage_Fire, Base_Clone</Death_Clone><SFXEvent_Select>Unit_Select_Base</SFXEvent_Select><SFXEvent_Lone>TBD</SFXEvent_Lone><SFXEvent_Lower>base_lower</SFXEvent_Lower>
</SpaceUnit>
<SpaceUnit Name="MID"><Variant_Of_Existing_Type>base</Variant_Of_Existing_Type>
  <ReplaceTag>mid</ReplaceTag><MergeTag>B</MergeTag><Tuple>three, 3</Tuple>
  <Ignored>mid-ignored</Ignored><Death_Clone>Damage_Fire, Mid_Clone</Death_Clone><SFXEvent_Select>TBD</SFXEvent_Select>
</SpaceUnit>
<SpaceUnit Name="TOP"><Variant_Of_Existing_Type>mId</Variant_Of_Existing_Type>
  <ReplaceTag>top</ReplaceTag><MergeTag>C</MergeTag><Empty></Empty><IgnoredNew>derived-only</IgnoredNew><SFXEvent_Select> </SFXEvent_Select><SFXEvent_Lower>tbd</SFXEvent_Lower>
  <Nested mode="top"><Leaf><Deep>top</Deep></Leaf></Nested><TopOnly>new</TopOnly>
</SpaceUnit>
<SpaceUnit Name="MISSING"><Variant_Of_Existing_Type>NO_BASE</Variant_Of_Existing_Type></SpaceUnit>
<SpaceUnit Name="CYCLE_A"><Variant_Of_Existing_Type>CYCLE_B</Variant_Of_Existing_Type></SpaceUnit>
<SpaceUnit Name="CYCLE_B"><Variant_Of_Existing_Type>cycle_a</Variant_Of_Existing_Type></SpaceUnit>
<SpaceUnit Name="DUP"><ReplaceTag>first</ReplaceTag></SpaceUnit>
<SpaceUnit Name="DUP_FIRST"><ReplaceTag>base-first-in-registry</ReplaceTag></SpaceUnit>
<SpaceUnit Name="DUP_SAME"><ReplaceTag>same-file-first</ReplaceTag></SpaceUnit>
<SpaceUnit Name="DUP_SAME"><ReplaceTag>same-file-second</ReplaceTag></SpaceUnit>
<SpaceUnit Name="ABILITY_OWNER"><Abilities><Lucky_Shot_Attack_Ability Name="LUCKY"><Damage>9</Damage><Definitely_Unknown_Ability_Field>raw</Definitely_Unknown_Ability_Field></Lucky_Shot_Attack_Ability></Abilities></SpaceUnit>
</Objects>)xml");
    write(tree.root / "XML" / "objects-b.xml", R"xml(<Objects>
<SpaceUnit Name="dup"><ReplaceTag>second</ReplaceTag></SpaceUnit>
<SpaceUnit Name="DUP_LAYER"><ReplaceTag>base-later-include</ReplaceTag></SpaceUnit>
<SpaceUnit Name="DUP_FIRST"><ReplaceTag>base-later-file</ReplaceTag></SpaceUnit>
</Objects>)xml");
    write(tree.mod_root / "XML" / "GameObjectFiles.xml",
          "<Game_Object_Files><File>mod-objects.xml</File><File>objects-a.xml</File><File>objects-b.xml</File><File>numeric-root.xml</File></Game_Object_Files>");
    write(tree.mod_root / "XML" / "mod-objects.xml",
          "<Objects><SpaceUnit Name=\"MOD_ONLY\"><ReplaceTag>mod</ReplaceTag></SpaceUnit></Objects>");
    write(tree.mod_root / "XML" / "numeric-root.xml",
          "<?xml version=\"1.0\"?><181st><SpaceUnit Name=\"NUMERIC_ROOT\"><Value>accepted</Value></SpaceUnit><SpaceUnit Name=\"DUP_LAYER\"><ReplaceTag>mod-stronger-layer</ReplaceTag></SpaceUnit></181st>");
    write(tree.root / "XML" / "hardpoints.xml",
          "<HardPoints><HardPoint Name=\"HP_A\"><Damage>1</Damage></HardPoint></HardPoints>");
    write(tree.mod_root / "XML" / "hardpoints.xml",
          "<HardPoints><HardPoint Name=\"HP_A\"><Damage>1</Damage></HardPoint>"
          "<HardPoint Name=\"HP_A\"><Damage>2</Damage></HardPoint></HardPoints>");
    write(tree.root / "XML" / "factions.xml", R"xml(<Factions><Faction Name="FACTION_A">
<Standalone_Space_Maps_Special_Weapon_B>OLD</Standalone_Space_Maps_Special_Weapon_B>
</Faction><Faction Name="FACTION_DUP"><Color>1 1 1</Color></Faction><Faction Name="FACTION_DUP"><Color>2 2 2</Color></Faction><Faction Name="FACTION_LAYER"><Color>1 1 1</Color></Faction></Factions>)xml");
    write(tree.mod_root / "XML" / "FactionFiles.xml",
          "<Faction_Files><File>factions.xml</File><File>mod-factions.xml</File></Faction_Files>");
    write(tree.mod_root / "XML" / "mod-factions.xml",
          "<Factions><Faction Name=\"FACTION_LAYER\"><Color>9 9 9</Color></Faction></Factions>");
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
    const auto* hp = catalog.find("HP_A", eawr::data::Category::hardpoint);
    expect(hp && hp->namespace_winner && hp->root.children[0].raw_text == "2",
           "ML-18: a mod's later hardpoint binding replaces its earlier binding");
    expect(catalog.find("HP_A") == hp, "untyped hardpoint lookup agrees with the typed binding");
    expect(catalog.find("top") != nullptr && catalog.find("ToP") == catalog.find("TOP"),
           "object IDs resolve case-insensitively");
    expect(catalog.find("DUP") != nullptr && catalog.find_all("dup").size() == 2,
           "duplicates remain available to the raw API");
    expect(catalog.find("DUP") && catalog.find("DUP")->root.children[0].raw_text == "first",
           "the first registered duplicate answers name lookups (#3741)");
    expect(catalog.find_all("dup").size() == 2 && catalog.find_all("dup")[0]->root.children[0].raw_text == "first" &&
               catalog.find_all("dup")[1]->root.children[0].raw_text == "second",
           "every duplicate definition stays, in load order");
    expect(catalog.find("DUP_FIRST") &&
               catalog.find("DUP_FIRST")->root.children[0].raw_text == "base-first-in-registry" &&
               catalog.find_all("DUP_FIRST").size() == 2,
           "two same-layer files: the definition in the file registered first answers name lookups (#3741)");
    {
        // The faction selection paths (scene_build.cpp faction_colours, skirmish/inputs.cpp faction_definition) take the
        // first active faction definition find_all lists: a mod faction listed later still beats the vanilla one.
        const auto first_faction = [&](const std::string_view id) -> const eawr::data::Definition* {
            for (const auto* definition : catalog.find_all(id))
                if (definition->category == eawr::data::Category::faction && definition->active) return definition;
            return nullptr;
        };
        const auto* layered = first_faction("FACTION_LAYER");
        expect(catalog.find_all("FACTION_LAYER").size() == 2 && layered && layered->root.children[0].raw_text == "9 9 9",
               "a mod faction overrides the vanilla one by layer for the faction selection paths");
        const auto* repeated = first_faction("FACTION_DUP");
        expect(repeated && repeated->root.children[0].raw_text == "1 1 1",
               "within one layer the first registered faction definition is the one selected");
    }
    {
        const auto factions = catalog.find_all("FACTION_DUP");
        expect(factions.size() == 2 && factions[0]->root.children[0].raw_text == "1 1 1",
               "a repeated faction Name keeps both, the first registered one leads find_all");
    }
    expect(catalog.find("DUP_SAME") &&
               catalog.find("DUP_SAME")->root.children[0].raw_text == "same-file-first" &&
               catalog.find_all("DUP_SAME").size() == 2,
           "two definitions in one file: the first in the file answers name lookups (#3741)");
    {
        std::size_t duplicate_notes = 0;
        for (const auto& note : loaded.value().notes)
            if (note.code == eawr::core::load_note_codes::duplicate_object_name && note.object == "DUP") ++duplicate_notes;
        expect(duplicate_notes == 1, "a duplicate Name emits one load note");
        std::size_t layer_notes = 0;
        for (const auto& note : loaded.value().notes)
            if (note.code == eawr::core::load_note_codes::duplicate_object_name && note.object == "DUP_LAYER") ++layer_notes;
        expect(layer_notes == 0, "a mod overriding a vanilla object emits no duplicate-name note");
    }
    expect(catalog.find("DUP_LAYER") &&
               catalog.find("DUP_LAYER")->root.children[0].raw_text == "mod-stronger-layer",
           "the stronger layer (mod) overrides vanilla even when its file is registered later");
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

    // #5197: a player's start skips the second parse of every XML outside the registries; the definitions, the registry
    // records and the load health are those of the full load, and only the inventory records say they were not parsed.
    {
        auto lean_options = options;
        lean_options.parse_unregistered_xml = false;
        auto lean = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::eaw, lean_options);
        expect(static_cast<bool>(lean), "catalog loads without the unregistered-XML inventory parse");
        if (lean) {
            const auto& skipped = lean.value().catalog;
            expect(skipped.definitions().size() == catalog.definitions().size() &&
                       skipped.registry_files().size() == catalog.registry_files().size() &&
                       skipped.physical_inventory().size() == catalog.physical_inventory().size(),
                   "skipping the inventory parse changes no definition, registry file or inventory record");
            expect(lean.value().health.clean() == loaded.value().health.clean() &&
                       lean.value().health.registry_invalid == loaded.value().health.registry_invalid &&
                       lean.value().health.include_failed == loaded.value().health.include_failed,
                   "skipping the inventory parse keeps the load health");
            const auto lean_parsed = std::count_if(skipped.physical_inventory().begin(), skipped.physical_inventory().end(),
                                                   [](const auto& file) { return file.parsed; });
            const auto full_parsed = std::count_if(catalog.physical_inventory().begin(), catalog.physical_inventory().end(),
                                                   [](const auto& file) { return file.parsed; });
            const auto lean_registry_parsed = std::count_if(skipped.physical_inventory().begin(), skipped.physical_inventory().end(),
                                                            [](const auto& file) { return file.parsed && file.active_registry_file; });
            expect(lean_parsed == lean_registry_parsed && lean_parsed <= full_parsed &&
                       !has_code(lean.value().diagnostics, eawr::data::diagnostic_codes::forbidden_doctype),
                   "only the active registry files are parsed, so the parse diagnostics of unregistered XML are not produced");
        }
    }

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
               categories(eawr::data::Category::hardpoint) == 2 &&
               categories(eawr::data::Category::ability) == 1 &&
               categories(eawr::data::Category::faction) == 5 &&
               categories(eawr::data::Category::campaign) == 1 &&
               categories(eawr::data::Category::sfx) == 1,
           "all six requested active catalog categories are populated");
    const auto* lucky = catalog.find("lucky");
    expect(lucky && lucky->category == eawr::data::Category::ability && lucky->root.source.line > 0,
           "nested ability is indexed with line provenance");
}
} // namespace eawr::tests::data_contracts
