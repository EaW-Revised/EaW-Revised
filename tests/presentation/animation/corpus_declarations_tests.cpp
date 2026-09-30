// Synthetic contracts of the declaration probe (corpus_declarations.hpp).  No
// private asset is used: every catalog is written to a temporary tree and
// loaded through the real VFS and XML catalog.

#include "corpus_declarations.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace corpus = eawr::tests::animation_corpus;
namespace audit = corpus::associations;
namespace probe = corpus::declarations;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] std::string sha256(const std::string_view bytes) {
    return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct TempTree {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-declaration-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path mod_root = root.string() + "-mod";
    TempTree() {
        std::filesystem::create_directories(root);
        std::filesystem::create_directories(mod_root);
    }
    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::remove_all(mod_root, ignored);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
};

void write(const std::filesystem::path& path, const std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

[[nodiscard]] audit::AssetIdentity identity(const std::string& path, const char digit) {
    return {path, std::string(64, digit), "mod", "loose", "mod:loose:" + path, path, 100};
}

// A pair whose clip `<set>_idle_00` the frozen R0 rule attributed to `<set>`.
[[nodiscard]] probe::PinnedPair pair(const std::string& clip, const std::string& set, const std::string& candidate,
    const bool priority = false) {
    return {identity("data/art/models/" + clip + ".ala", 'a'), "data/art/models/" + set + ".alo",
        std::string(64, 'b'), identity("data/art/models/" + candidate + ".alo", 'c'), priority};
}

[[nodiscard]] const probe::PairResult* find(const probe::Audit& result, const std::string& clip) {
    for (const auto& item : result.pairs)
        if (item.pair.animation.path == "data/art/models/" + clip + ".ala") return &item;
    return nullptr;
}

[[nodiscard]] bool observed(const probe::PairResult& result, const std::string_view kind, const std::string_view object) {
    return std::any_of(result.observations.begin(), result.observations.end(), [&](const probe::Observation& item) {
        return item.kind == kind && probe::iequals(item.object_id, object);
    });
}

[[nodiscard]] std::string render(const probe::Audit& result) {
    std::ostringstream output;
    probe::Header header;
    header.pairs_sha256 = std::string(64, 'd');
    header.animation_count = 7685;
    header.playback_passed = 7329;
    header.failure_count = 356;
    probe::write_audit(output, header, result);
    return output.str();
}

void registries(const std::filesystem::path& root, const std::string_view objects) {
    write(root / "XML" / "GameObjectFiles.xml", "<Game_Object_Files>" + std::string(objects) + "</Game_Object_Files>");
    write(root / "XML" / "HardpointDataFiles.xml", "<Hard_Point_Files></Hard_Point_Files>");
    write(root / "XML" / "FactionFiles.xml", "<Faction_Files></Faction_Files>");
    write(root / "XML" / "CampaignFiles.xml", "<Campaign_Files></Campaign_Files>");
    write(root / "XML" / "SFXEventFiles.xml", "<SFXEvent_Files><File>sfx.xml</File></SFXEvent_Files>");
}

void test_catalog_dispositions() {
    TempTree tree;
    registries(tree.root, "<File>base-units.xml</File><File>shadow.xml</File>");
    registries(tree.mod_root, "<File>base-units.xml</File><File>shadow.xml</File><File>mod-units.xml</File>");
    write(tree.root / "XML" / "base-units.xml", R"xml(<GameObjects>
<GroundInfantry Name="P1_BASE"><Land_Model_Name>P1.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="P1_CHILD"><Variant_Of_Existing_Type>P1_BASE</Variant_Of_Existing_Type>
  <Land_Model_Anim_Override_Name> p1_heavy </Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="P2_UNIT"><Land_Model_Name>P2.alo</Land_Model_Name>
  <Space_Model_Name>P2_Heavy.alo</Space_Model_Name></GroundInfantry>
<GroundInfantry Name="P3_UNIT"><Land_Model_Name>P3.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>P3_Heavy_Alt.ALO</Land_Model_Anim_Override_Name>
  <Icon_Name>p3_heavy</Icon_Name></GroundInfantry>
<GroundInfantry Name="P5_CHILD"><Variant_Of_Existing_Type>P5_MISSING</Variant_Of_Existing_Type>
  <Land_Model_Anim_Override_Name>P5_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="P6_UNIT"><Land_Model_Name>P6.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>P6_Other.ALO</Land_Model_Anim_Override_Name>
  <Land_Model_Anim_Override_Name>P6_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="P7_UNIT"><Land_Model_Name>P7.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>P7_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="P7_UNIT"><Land_Model_Name>P7.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="P8_UNIT"><Land_Model_Name>P8_Other.ALO</Land_Model_Name>
  <Space_Model_Name>P8.ALO</Space_Model_Name>
  <Land_Model_Anim_Override_Name>P8_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="P9_A"><Variant_Of_Existing_Type>P9_B</Variant_Of_Existing_Type>
  <Land_Model_Name>P9.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="P9_B"><Variant_Of_Existing_Type>P9_A</Variant_Of_Existing_Type>
  <Land_Model_Anim_Override_Name>P9_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="P13_UNIT"><Land_Model_Name>P13.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>P13_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="P13_ORPHAN"><Variant_Of_Existing_Type>P13_GONE</Variant_Of_Existing_Type>
  <Land_Model_Name>P13.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="P10_UNIT"><Model_Name>P10.ALO</Model_Name>
  <Land_Model_Anim_Override_Name>P10_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
</GameObjects>)xml");
    // A different file, so its definitions are shadowed by the mod layer's.
    write(tree.root / "XML" / "shadow.xml", R"xml(<GameObjects>
<GroundInfantry Name="P4_UNIT"><Land_Model_Name>P4.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>P4_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="P11_UNIT"><Land_Model_Name>P11.ALO</Land_Model_Name></GroundInfantry>
</GameObjects>)xml");
    write(tree.mod_root / "XML" / "mod-units.xml", R"xml(<GameObjects>
<GroundInfantry Name="P4_UNIT"><Land_Model_Name>P4.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="P11_UNIT"><Land_Model_Name>P11.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>P11_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
</GameObjects>)xml");
    write(tree.root / "XML" / "sfx.xml", R"xml(<SFXEvents>
<SFXEvent Name="SFX_P3"><Samples>p3_heavy_idle_00, Data\Audio\p3.wav</Samples></SFXEvent>
</SFXEvents>)xml");

    const std::array mounts{
        eawr::vfs::MountSpec{"mod", tree.mod_root, "data", {}},
        eawr::vfs::MountSpec{"base", tree.root, "data", {}},
    };
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "synthetic VFS mounts");
    if (!mounted) return;
    auto loaded = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::remake);
    expect(static_cast<bool>(loaded), "synthetic catalog loads");
    if (!loaded) return;
    const eawr::vfs::Vfs& vfs = mounted.value();
    const probe::SourceHash source_hash = [&vfs](const std::string& logical_path) {
        auto bytes = vfs.open(logical_path);
        if (!bytes) return std::string{};
        return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size()));
    };

    probe::PinnedPairs pinned;
    for (const auto& [clip, set, candidate] : std::vector<std::array<std::string, 3>>{
             {"p1_heavy_idle_00", "p1_heavy", "p1"}, {"p2_heavy_idle_00", "p2_heavy", "p2"},
             {"p3_heavy_idle_00", "p3_heavy", "p3"}, {"p4_heavy_idle_00", "p4_heavy", "p4"},
             {"p5_heavy_idle_00", "p5_heavy", "p5"}, {"p6_heavy_idle_00", "p6_heavy", "p6"},
             {"p7_heavy_idle_00", "p7_heavy", "p7"}, {"p8_heavy_idle_00", "p8_heavy", "p8"},
             {"p9_heavy_idle_00", "p9_heavy", "p9"}, {"p10_heavy_idle_00", "p10_heavy", "p10"},
             {"p11_heavy_idle_00", "p11_heavy", "p11"}, {"p12_heavy_idle_00", "p12_heavy", "p12"},
             {"p13_heavy_idle_00", "p13_heavy", "p13"}})
        pinned.pairs.push_back(pair(clip, set, candidate));
    std::sort(pinned.pairs.begin(), pinned.pairs.end(),
        [](const auto& left, const auto& right) { return left.animation.path < right.animation.path; });

    const auto result = probe::evaluate(pinned, &loaded.value().catalog, loaded.value().diagnostics, {}, source_hash);
    namespace disposition = probe::disposition;
    namespace observation = probe::observation;

    // Inherited land model plus own override: one evidence record, with the
    // provenance of each value.
    const auto* p1 = find(result, "p1_heavy_idle_00");
    expect(p1 != nullptr && p1->disposition == disposition::evidence_bearing, "inherited link is evidence-bearing");
    if (p1 != nullptr && p1->evidence.size() == 1) {
        const auto& evidence = p1->evidence.front();
        expect(evidence.object_id == "P1_CHILD", "evidence names the resolved object");
        expect(evidence.chain == std::vector<std::string>{"P1_CHILD", "P1_BASE"}, "evidence keeps the object chain");
        expect(evidence.model.provenance == "inherited" && evidence.model.source_object_id == "P1_BASE",
            "land model is inherited from the base");
        expect(evidence.animation_set.provenance == "added" && evidence.animation_set.source_object_id == "P1_CHILD",
            "override is added by the child");
        expect(evidence.model.source.logical_path == "data/xml/base-units.xml" && evidence.model.source.layer_id == "base"
                && evidence.model.source.line == 2 && !evidence.model.source_sha256.empty(),
            "evidence carries a source locator and hash");
        expect(evidence.animation_set.canonical == "data/art/models/p1_heavy.alo", "override is canonicalised exactly");
        expect(observed(*p1, observation::model_only, "P1_BASE"), "the base alone is recorded as model-only");
    } else {
        expect(false, "inherited link resolves exactly once");
    }

    // Model tags alone, in any model field, never link.
    const auto* p2 = find(result, "p2_heavy_idle_00");
    expect(p2 != nullptr && p2->disposition == disposition::no_explicit_evidence && p2->evidence.empty(),
        "model-only tags give no explicit evidence");
    expect(p2 != nullptr && observed(*p2, observation::model_only, "P2_UNIT"), "model-only tag is observed");

    // A similar-looking override, an icon token and an SFX sample do not link.
    const auto* p3 = find(result, "p3_heavy_idle_00");
    expect(p3 != nullptr && p3->disposition == disposition::no_explicit_evidence, "similar names do not promote");
    expect(p3 != nullptr && observed(*p3, observation::override_other_set, "P3_UNIT"), "other set is observed");
    expect(p3 != nullptr && observed(*p3, observation::text_mention, "SFX_P3"), "SFX clip token is observed");
    expect(p3 != nullptr && observed(*p3, observation::text_mention, "P3_UNIT"), "icon set token is observed");

    // The mod layer's winner drops the override: the shadowed link is recorded, not used.
    const auto* p4 = find(result, "p4_heavy_idle_00");
    expect(p4 != nullptr && p4->disposition == disposition::no_explicit_evidence, "shadowed link does not promote");
    expect(p4 != nullptr && observed(*p4, observation::shadowed_definition, "P4_UNIT"), "shadowed link is observed");
    // ... and a winner that adds the override is the effective declaration.
    const auto* p11 = find(result, "p11_heavy_idle_00");
    expect(p11 != nullptr && p11->disposition == disposition::evidence_bearing && p11->evidence.size() == 1
            && p11->evidence.front().animation_set.source.layer_id == "mod",
        "effective winner's link is evidence-bearing");

    // An absent inherited definition, and a cycle, fail closed.
    const auto* p5 = find(result, "p5_heavy_idle_00");
    expect(p5 != nullptr && p5->disposition == disposition::catalog_unresolved && p5->unresolved.size() == 1
            && p5->unresolved.front().code == eawr::data::diagnostic_codes::variant_missing_base,
        "missing variant base is catalog_unresolved");
    const auto* p9 = find(result, "p9_heavy_idle_00");
    expect(p9 != nullptr && p9->disposition == disposition::catalog_unresolved, "variant cycle is catalog_unresolved");

    // Equally authoritative disagreement never resolves to evidence.
    const auto* p6 = find(result, "p6_heavy_idle_00");
    expect(p6 != nullptr && p6->disposition == disposition::conflicting_evidence && !p6->evidence.empty(),
        "repeated override in one definition is conflicting even when the kept value links");
    const auto* p7 = find(result, "p7_heavy_idle_00");
    expect(p7 != nullptr && p7->disposition == disposition::conflicting_evidence,
        "same-file duplicates that disagree are conflicting");

    // Evidence elsewhere does not outweigh an unresolved object naming the candidate.
    const auto* p13 = find(result, "p13_heavy_idle_00");
    expect(p13 != nullptr && p13->disposition == disposition::catalog_unresolved && p13->evidence.size() == 1,
        "an unresolved object outranks evidence");

    // The set used on another land model is not a link, even when the
    // candidate is that object's space model.
    const auto* p8 = find(result, "p8_heavy_idle_00");
    expect(p8 != nullptr && p8->disposition == disposition::no_explicit_evidence, "set on other model does not promote");
    expect(p8 != nullptr && observed(*p8, observation::set_on_other_model, "P8_UNIT")
            && observed(*p8, observation::model_only, "P8_UNIT"),
        "set on other model and the space model are observed");

    // Model_Name is the land model when Land_Model_Name is absent, as in the scene layer.
    const auto* p10 = find(result, "p10_heavy_idle_00");
    expect(p10 != nullptr && p10->disposition == disposition::evidence_bearing, "Model_Name fallback links");

    // Nothing declared at all.
    const auto* p12 = find(result, "p12_heavy_idle_00");
    expect(p12 != nullptr && p12->disposition == disposition::no_explicit_evidence && p12->observations.empty(),
        "an undeclared pair has no evidence and no observation");

    expect(result.dispositions.at(std::string(disposition::evidence_bearing)) == 3, "three evidence-bearing pairs");
    expect(result.dispositions.at(std::string(disposition::conflicting_evidence)) == 2, "two conflicting pairs");
    expect(result.dispositions.at(std::string(disposition::catalog_unresolved)) == 3, "three unresolved pairs");
    expect(result.dispositions.at(std::string(disposition::no_explicit_evidence)) == 5, "five pairs without evidence");

    // The receipt never approves, and is byte-identical for any input order
    // and across a second catalog load.
    const std::string bytes = render(result);
    expect(bytes.find("\"approved\": true") == std::string::npos && bytes.find("\"associations_promoted\": 0") != std::string::npos
            && bytes.find("\"association_approved\": false") != std::string::npos,
        "receipt carries no approval");
    std::mt19937 random(24U);
    for (int round = 0; round < 16; ++round) {
        probe::PinnedPairs shuffled = pinned;
        std::shuffle(shuffled.pairs.begin(), shuffled.pairs.end(), random);
        auto reloaded = eawr::data::load_catalog(vfs, eawr::data::Profile::remake);
        if (!reloaded) {
            expect(false, "catalog reloads");
            break;
        }
        const auto again = probe::evaluate(shuffled, &reloaded.value().catalog, reloaded.value().diagnostics, {}, source_hash);
        expect(render(again) == bytes, "receipt bytes are independent of pair order and catalog load");
    }

    // A catalog that failed to load leaves every pair unresolved.
    const auto failed = probe::evaluate(pinned, nullptr, {}, "EAWR-XML-0003: synthetic", source_hash);
    expect(failed.dispositions.at(std::string(disposition::catalog_unresolved)) == pinned.pairs.size(),
        "no catalog means every pair is catalog_unresolved");
}

// Same-layer, same-file duplicates are equally authoritative.  Each is
// resolved with its own values over its own inheritance, never filled from the
// winner, and so is every duplicate of a variant base.  A way of resolving
// that disagrees with the winner on the link, or that cannot be followed,
// makes the pair conflicting.
void test_duplicate_inheritance() {
    TempTree tree;
    registries(tree.root, "<File>units.xml</File>");
    write(tree.root / "XML" / "units.xml", R"xml(<GameObjects>
<GroundInfantry Name="D1_UNIT"><Land_Model_Name>D1.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="D1_UNIT"><Land_Model_Name>D1.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>D1_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="D2_UNIT"><Land_Model_Name>D2.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>D2_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="D2_UNIT"><Land_Model_Name>D2.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="D3_LINKS"><Land_Model_Name>D3.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>D3_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="D3_PLAIN"><Land_Model_Name>D3.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="D3_UNIT"><Variant_Of_Existing_Type>D3_PLAIN</Variant_Of_Existing_Type></GroundInfantry>
<GroundInfantry Name="D3_UNIT"><Variant_Of_Existing_Type>D3_LINKS</Variant_Of_Existing_Type></GroundInfantry>
<GroundInfantry Name="D4_LINKS"><Land_Model_Name>D4.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>D4_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="D4_PLAIN"><Land_Model_Name>D4.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="D4_UNIT"><Variant_Of_Existing_Type>D4_LINKS</Variant_Of_Existing_Type></GroundInfantry>
<GroundInfantry Name="D4_UNIT"><Variant_Of_Existing_Type>D4_PLAIN</Variant_Of_Existing_Type></GroundInfantry>
<GroundInfantry Name="D5_A"><Land_Model_Name>D5.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>D5_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="D5_B"><Model_Name>D5.ALO</Model_Name>
  <Land_Model_Anim_Override_Name>D5_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="D5_UNIT"><Variant_Of_Existing_Type>D5_B</Variant_Of_Existing_Type></GroundInfantry>
<GroundInfantry Name="D5_UNIT"><Variant_Of_Existing_Type>D5_A</Variant_Of_Existing_Type></GroundInfantry>
<GroundInfantry Name="D6_BASE"><Land_Model_Anim_Override_Name>D6_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="D6_BASE"><Icon_Name>d6</Icon_Name></GroundInfantry>
<GroundInfantry Name="D6_UNIT"><Variant_Of_Existing_Type>D6_BASE</Variant_Of_Existing_Type>
  <Land_Model_Name>D6.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="D7_UNIT"><Variant_Of_Existing_Type>D7_GONE</Variant_Of_Existing_Type>
  <Land_Model_Name>D7.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="D7_UNIT"><Land_Model_Name>D7.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>D7_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="D8_UNIT"><Variant_Of_Existing_Type>D8_UNIT</Variant_Of_Existing_Type>
  <Land_Model_Name>D8.ALO</Land_Model_Name></GroundInfantry>
<GroundInfantry Name="D8_UNIT"><Land_Model_Name>D8.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>D8_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="D9_UNIT"><Land_Model_Name>D9.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>D9_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
<GroundInfantry Name="D9_UNIT"><Land_Model_Name>D9.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>D9_Heavy.ALO</Land_Model_Anim_Override_Name></GroundInfantry>
</GameObjects>)xml");
    write(tree.root / "XML" / "sfx.xml", "<SFXEvents></SFXEvents>");

    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "duplicate VFS mounts");
    if (!mounted) return;
    auto loaded = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::remake);
    expect(static_cast<bool>(loaded), "duplicate catalog loads");
    if (!loaded) return;

    probe::PinnedPairs pinned;
    for (int number = 1; number <= 9; ++number) {
        const std::string name = "d" + std::to_string(number);
        pinned.pairs.push_back(pair(name + "_heavy_idle_00", name + "_heavy", name));
    }
    std::sort(pinned.pairs.begin(), pinned.pairs.end(),
        [](const auto& left, const auto& right) { return left.animation.path < right.animation.path; });
    const auto result = probe::evaluate(pinned, &loaded.value().catalog, loaded.value().diagnostics, {}, {});
    namespace disposition = probe::disposition;
    const auto conflicting = [&result](const std::string& clip, const std::string_view message) {
        const auto* item = find(result, clip);
        expect(item != nullptr && item->disposition == disposition::conflicting_evidence, message);
        return item;
    };
    const auto names = [](const probe::PairResult* item, const std::string_view text) {
        return item != nullptr && !item->conflicts.empty()
            && item->conflicts.front().reason.find(text) != std::string::npos;
    };

    // An earlier model-only duplicate does not borrow the later winner's override.
    const auto* d1 = conflicting("d1_heavy_idle_00", "earlier model-only duplicate of a linking winner conflicts");
    expect(d1 != nullptr && d1->evidence.size() == 1, "the winner's link is still recorded as evidence");
    const auto* d2 = conflicting("d2_heavy_idle_00", "earlier linking duplicate of a model-only winner conflicts");
    expect(d2 != nullptr && d2->evidence.empty(), "the model-only winner does not link");
    // Distinct variant parents, in both orders.
    conflicting("d3_heavy_idle_00", "winner inheriting the link over a duplicate with another parent conflicts");
    conflicting("d4_heavy_idle_00", "duplicate inheriting the link under a winner with another parent conflicts");
    // Distinct parents that both link agree: no conflict.
    const auto* d5 = find(result, "d5_heavy_idle_00");
    expect(d5 != nullptr && d5->disposition == disposition::evidence_bearing && d5->conflicts.empty(),
        "duplicates with distinct parents that agree stay evidence-bearing");
    // A duplicate of a variant base is resolved too.
    const auto* d6 = conflicting("d6_heavy_idle_00", "a variant base duplicate that would link conflicts");
    expect(d6 != nullptr && d6->evidence.empty(), "the winning base does not link");
    // A duplicate that cannot be followed never lets the winner's link stand.
    const auto* d7 = conflicting("d7_heavy_idle_00", "a duplicate with a missing base conflicts with the winner's link");
    expect(names(d7, "missing variant base D7_GONE"), "the missing base is named");
    const auto* d8 = conflicting("d8_heavy_idle_00", "a duplicate naming its own ID as base conflicts");
    expect(names(d8, "variant cycle"), "the cycle is named");
    // Identical duplicates agree.
    const auto* d9 = find(result, "d9_heavy_idle_00");
    expect(d9 != nullptr && d9->disposition == disposition::evidence_bearing, "identical duplicates stay evidence-bearing");

    expect(result.dispositions.at(std::string(disposition::conflicting_evidence)) == 7, "seven conflicting duplicate pairs");
    expect(result.dispositions.at(std::string(disposition::evidence_bearing)) == 2, "two agreeing duplicate pairs");
    const std::string bytes = render(result);
    for (int round = 0; round < 4; ++round) {
        auto reloaded = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::remake);
        if (!reloaded) {
            expect(false, "duplicate catalog reloads");
            break;
        }
        const auto again = probe::evaluate(pinned, &reloaded.value().catalog, reloaded.value().diagnostics, {}, {});
        expect(render(again) == bytes, "duplicate receipt bytes are stable across catalog loads");
    }
}

[[nodiscard]] std::string pairs_text(const std::vector<probe::PinnedPair>& pairs, const std::size_t count,
    const std::size_t priority) {
    std::string text = "eawr.animation-declaration-pairs\t1\nsource\tassociation_receipt_sha256\t" + std::string(64, 'e')
        + "\nsource\tfrozen_metadata_sha256\t" + std::string(64, 'f') + "\ncounts\t" + std::to_string(count) + '\t'
        + std::to_string(priority) + '\n';
    for (const auto& item : pairs) {
        text += "pair";
        for (const auto* id : {&item.animation}) {
            text += '\t' + id->path + '\t' + id->sha256 + '\t' + id->layer_id + '\t' + id->origin + '\t' + id->source_id
                + '\t' + id->original_path + '\t' + std::to_string(id->size);
        }
        text += '\t' + item.selected_model + '\t' + item.selected_sha256;
        const auto& id = item.candidate;
        text += '\t' + id.path + '\t' + id.sha256 + '\t' + id.layer_id + '\t' + id.origin + '\t' + id.source_id + '\t'
            + id.original_path + '\t' + std::to_string(id.size) + '\t' + (item.priority ? "1" : "0") + '\n';
    }
    return text;
}

void test_pinned_pairs() {
    const std::vector<probe::PinnedPair> pairs{pair("a_heavy_idle_00", "a_heavy", "a", true), pair("b_heavy_idle_00", "b_heavy", "b")};
    const std::string good = pairs_text(pairs, 2, 1);
    const auto parsed = probe::parse_pinned_pairs(good, 2, 1);
    expect(parsed.pairs && parsed.pairs->pairs.size() == 2 && parsed.pairs->pairs.front().priority, "pinned pairs parse");
    expect(!probe::parse_pinned_pairs(good, 3, 1).pairs, "unexpected pair count is refused");
    expect(!probe::parse_pinned_pairs(pairs_text(pairs, 2, 0), 2, 1).pairs, "counts line mismatch is refused");
    expect(!probe::parse_pinned_pairs(pairs_text({pairs[1], pairs[0]}, 2, 1), 2, 1).pairs, "unsorted pairs are refused");
    expect(!probe::parse_pinned_pairs(pairs_text({pair("a_idle_00", "a", "a", true), pairs[1]}, 2, 1), 2, 1).pairs,
        "candidate equal to selected is refused");
    expect(!probe::parse_pinned_pairs(good.substr(0, good.size() - 1), 2, 1).pairs, "truncated pairs are refused");
    const auto sha = sha256(good);
    expect(static_cast<bool>(probe::load_pinned_pairs(good, sha, sha, 2, 1).pairs), "pinned hash loads");
    const auto stale = probe::load_pinned_pairs(good, sha, std::string(64, '0'), 2, 1);
    expect(!stale.pairs && stale.error.find("stale") != std::string::npos, "stale pinned hash is refused");
    expect(!probe::load_pinned_pairs(std::nullopt, std::nullopt, sha, 2, 1).pairs, "missing pairs are refused");
}

void test_frozen_and_current_identity() {
    audit::FrozenMetadata frozen;
    frozen.sha256 = std::string(64, 'f');
    probe::PinnedPairs pinned;
    pinned.frozen_metadata_sha256 = frozen.sha256;
    pinned.pairs.push_back(pair("a_heavy_idle_00", "a_heavy", "a"));
    audit::BaselineFailure failure;
    failure.animation = pinned.pairs[0].animation;
    failure.selected_model = pinned.pairs[0].selected_model;
    failure.r0_candidates = {pinned.pairs[0].selected_model, pinned.pairs[0].candidate.path};
    frozen.failures[0] = failure;
    frozen.models[pinned.pairs[0].selected_model] = {identity(pinned.pairs[0].selected_model, 'b'), 20};
    frozen.models[pinned.pairs[0].candidate.path] = {pinned.pairs[0].candidate, 24};
    expect(!probe::check_pairs_against_frozen(pinned, frozen), "matching frozen metadata passes");

    auto drifted = pinned;
    drifted.pairs[0].animation.sha256 = std::string(64, '9');
    expect(static_cast<bool>(probe::check_pairs_against_frozen(drifted, frozen)), "ALA hash drift is refused");
    drifted = pinned;
    drifted.pairs[0].candidate.source_id = "base:Data/Models.meg";
    expect(static_cast<bool>(probe::check_pairs_against_frozen(drifted, frozen)), "candidate provenance drift is refused");
    drifted = pinned;
    drifted.pairs[0].selected_sha256 = std::string(64, '8');
    expect(static_cast<bool>(probe::check_pairs_against_frozen(drifted, frozen)), "selected hash drift is refused");
    auto no_r0 = frozen;
    no_r0.failures[0].r0_candidates = {pinned.pairs[0].selected_model};
    expect(static_cast<bool>(probe::check_pairs_against_frozen(pinned, no_r0)), "candidate outside R0 is refused");
    auto unrecorded = frozen;
    unrecorded.models.erase(pinned.pairs[0].candidate.path);
    expect(static_cast<bool>(probe::check_pairs_against_frozen(pinned, unrecorded)), "unlisted candidate is refused");
    unrecorded.unrecorded = {pinned.pairs[0].candidate.path};
    expect(!probe::check_pairs_against_frozen(pinned, unrecorded), "unrecorded candidate is pinned by the pair list");

    const auto current = [&](const std::string& field) -> probe::IdentityLookup {
        return [&pinned, field](const std::string& path) -> std::optional<audit::AssetIdentity> {
            const auto& item = pinned.pairs[0];
            if (path == item.animation.path) return item.animation;
            if (path == item.selected_model) {
                // The same bytes, so the same SHA-256, but other provenance.
                if (field == "selected-missing") return std::nullopt;
                auto result = identity(path, 'b');
                if (field == "selected-layer") result.layer_id = "expansion";
                if (field == "selected-origin") result.origin = "archive";
                if (field == "selected-source") result.source_id = "mod:Data/Models.meg";
                if (field == "selected-original") result.original_path = "Data/Art/Models/A_HEAVY.ALO";
                if (field == "selected-size") result.size = 101;
                if (field == "selected-sha256") result.sha256 = std::string(64, '6');
                return result;
            }
            if (path != item.candidate.path || field == "missing") return std::nullopt;
            auto result = item.candidate;
            if (field == "sha256") result.sha256 = std::string(64, '7');
            if (field == "layer") result.layer_id = "expansion";
            return result;
        };
    };
    expect(!probe::check_current_identities(pinned, frozen, current("")), "current identities pass");
    for (const std::string field : {"sha256", "layer", "missing"})
        expect(static_cast<bool>(probe::check_current_identities(pinned, frozen, current(field))),
            "stale candidate fails: " + field);
    for (const std::string field : {"selected-layer", "selected-origin", "selected-source", "selected-original",
             "selected-size", "selected-sha256", "selected-missing"}) {
        const auto error = probe::check_current_identities(pinned, frozen, current(field));
        expect(error.has_value() && error->find("selected model") != std::string::npos,
            "same-hash or changed selected model provenance fails: " + field);
    }
    // The selected model's provenance is its frozen record, which must exist
    // and carry the pinned hash.
    auto no_selected = frozen;
    no_selected.models.erase(pinned.pairs[0].selected_model);
    expect(static_cast<bool>(probe::check_current_identities(pinned, no_selected, current(""))),
        "selected model without a frozen record fails");
    auto other_hash = frozen;
    other_hash.models[pinned.pairs[0].selected_model].identity.sha256 = std::string(64, '5');
    expect(static_cast<bool>(probe::check_current_identities(pinned, other_hash, current(""))),
        "selected model whose frozen record has another hash fails");
}

// The tracked pair list authenticates against its pin and the tracked frozen
// manifest, with exactly 63 pairs of which 58 are priority rows.
void test_tracked_inputs() {
    const auto pairs_bytes = read_file(EAWR_DECLARATION_PAIRS);
    const auto metadata_bytes = read_file(EAWR_ASSOCIATION_FROZEN_MANIFEST);
    expect(pairs_bytes.has_value() && metadata_bytes.has_value(), "tracked inputs are readable");
    if (!pairs_bytes || !metadata_bytes) return;
    const auto pinned = probe::load_pinned_pairs(pairs_bytes, sha256(*pairs_bytes));
    expect(pinned.pairs.has_value(), "tracked pair list matches its pin: " + pinned.error);
    const auto metadata = audit::load_frozen_metadata(metadata_bytes, sha256(*metadata_bytes));
    expect(metadata.metadata.has_value(), "tracked frozen manifest matches its pin");
    if (!pinned.pairs || !metadata.metadata) return;
    expect(pinned.pairs->pairs.size() == 63, "63 pinned pairs");
    expect(pinned.pairs->receipt_sha256 == probe::pinned_pairs_receipt_sha256, "pair list names the pinned receipt");
    const auto error = probe::check_pairs_against_frozen(*pinned.pairs, *metadata.metadata);
    expect(!error, "tracked pairs agree with the frozen manifest: " + error.value_or(""));
    const auto priority = std::count_if(pinned.pairs->pairs.begin(), pinned.pairs->pairs.end(), [](const auto& item) {
        return item.priority && item.selected_model == audit::priority_selected_model
            && item.candidate.path == "data/art/models/ei_armytrooper.alo";
    });
    expect(priority == 58, "58 priority pairs, all ei_armytrooper_heavy -> ei_armytrooper");
}

void test_receipt_escaping() {
    probe::Audit result;
    probe::PairResult item;
    item.pair = pair("q\"t\x01_idle_00", "q", "r");
    item.disposition = std::string(probe::disposition::no_explicit_evidence);
    result.pairs.push_back(item);
    const std::string bytes = render(result);
    expect(bytes.find("q\\\"t\\u0001_idle_00") != std::string::npos, "path is escaped");
    expect(std::none_of(bytes.begin(), bytes.end(), [](const char c) {
        return static_cast<unsigned char>(c) < 0x20U && c != '\n';
    }), "no raw control byte in the receipt");
}

void test_canonical_names() {
    expect(probe::canonical_model_path(" EI_Armytrooper.ALO ") == "data/art/models/ei_armytrooper.alo", "declared name folds");
    expect(probe::canonical_model_path("EI_Armytrooper") == "data/art/models/ei_armytrooper.alo", "suffix is added");
    expect(probe::canonical_model_path("EI_Armytrooper_Heavy.ALO") != probe::canonical_model_path("EI_Armytrooper.ALO"),
        "no prefix equivalence");
    expect(probe::canonical_model_path("").empty(), "empty declaration stays empty");
    expect(probe::is_model_field("Land_Model_Name") && probe::is_model_field("model_name")
            && !probe::is_model_field("Model_To_Attach") && !probe::is_model_field("Animation_Name"),
        "model fields are the *Model_Name tags only");
}

} // namespace

int main() {
    test_canonical_names();
    test_pinned_pairs();
    test_frozen_and_current_identity();
    test_tracked_inputs();
    test_receipt_escaping();
    test_catalog_dispositions();
    test_duplicate_inheritance();
    if (failures != 0) {
        std::cerr << failures << " declaration probe contract(s) failed\n";
        return 1;
    }
    std::cout << "declaration probe contracts passed\n";
    return 0;
}
