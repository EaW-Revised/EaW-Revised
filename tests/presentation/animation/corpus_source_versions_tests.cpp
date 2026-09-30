#include "corpus_source_versions.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace eawr;
namespace audit = eawr::tests::animation_corpus::associations;
namespace sources = eawr::tests::animation_corpus::source_versions;

int failures{};
void expect(const bool condition, const std::string_view message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; }
}

// ---------------------------------------------------------------------------
// Original byte fixtures, laid out by hand from the public chunk grammar
// rather than serialised by production code.
// ---------------------------------------------------------------------------

using Bytes = std::vector<std::byte>;

void u8(Bytes& out, const std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }
void u16(Bytes& out, const std::uint16_t value) { u8(out, static_cast<std::uint8_t>(value & 0xffU)); u8(out, static_cast<std::uint8_t>(value >> 8U)); }
void u32(Bytes& out, const std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffU)); }
void i16(Bytes& out, const std::int16_t value) { u16(out, static_cast<std::uint16_t>(value)); }
void f32(Bytes& out, const float value) { u32(out, std::bit_cast<std::uint32_t>(value)); }
void add(Bytes& out, const Bytes& value) { out.insert(out.end(), value.begin(), value.end()); }
void text(Bytes& out, const std::string_view value) { for (const char c : value) u8(out, static_cast<std::uint8_t>(c)); u8(out, 0); }
Bytes chunk(const std::uint32_t type, const Bytes& payload, const bool group = false) {
    Bytes out; u32(out, type); u32(out, static_cast<std::uint32_t>(payload.size()) | (group ? 0x80000000U : 0U)); add(out, payload); return out;
}
void mini(Bytes& out, const std::uint8_t type, const Bytes& payload) { u8(out, type); u8(out, static_cast<std::uint8_t>(payload.size())); add(out, payload); }
Bytes integer(const std::uint32_t value) { Bytes out; u32(out, value); return out; }
Bytes vec3() { Bytes out; f32(out, 0.0F); f32(out, 0.0F); f32(out, 0.0F); return out; }

// Minimal ALO: a root plus children of the root.
Bytes alo(const std::vector<std::string>& names) {
    Bytes skeleton;
    add(skeleton, chunk(0x201, integer(static_cast<std::uint32_t>(names.size()))));
    for (std::size_t index = 0; index < names.size(); ++index) {
        Bytes name; text(name, names[index]);
        Bytes data; u32(data, std::bit_cast<std::uint32_t>(index == 0 ? std::int32_t{-1} : std::int32_t{0})); u32(data, 1);
        for (int item = 0; item < 12; ++item) f32(data, (item == 0 || item == 4 || item == 8) ? 1.0F : 0.0F);
        Bytes bone; add(bone, chunk(0x203, name)); add(bone, chunk(0x205, data));
        add(skeleton, chunk(0x202, bone, true));
    }
    Bytes header; mini(header, 1, integer(0)); mini(header, 4, integer(0));
    Bytes connections; add(connections, chunk(0x601, header));
    Bytes file; add(file, chunk(0x200, skeleton, true)); add(file, chunk(0x600, connections, true));
    return file;
}

// Minimal v1 ALA: three frames, one constant identity rotation per track.
Bytes ala(const std::vector<std::pair<std::uint32_t, std::string>>& tracks) {
    Bytes info; mini(info, 1, integer(3)); Bytes rate; f32(rate, 1.0F); mini(info, 2, rate);
    mini(info, 3, integer(static_cast<std::uint32_t>(tracks.size())));
    Bytes root; add(root, chunk(0x1001, info));
    for (const auto& [index, name] : tracks) {
        Bytes minis; Bytes label; text(label, name); mini(minis, 4, label); mini(minis, 5, integer(index));
        for (std::uint8_t type = 6; type <= 9; ++type) mini(minis, type, vec3());
        Bytes rotation; i16(rotation, 0); i16(rotation, 0); i16(rotation, 0); i16(rotation, 32767);
        Bytes track; add(track, chunk(0x1003, minis)); add(track, chunk(0x1006, rotation));
        add(root, chunk(0x1002, track, true));
    }
    return chunk(0x1000, root, true);
}

// MEG v1 with any number of entries (name, bytes).
Bytes meg(const std::vector<std::pair<std::string, Bytes>>& entries) {
    Bytes names;
    for (const auto& [name, data] : entries) {
        u16(names, static_cast<std::uint16_t>(name.size()));
        for (const char c : name) u8(names, static_cast<std::uint8_t>(c));
    }
    std::uint32_t offset = static_cast<std::uint32_t>(8 + names.size() + 20 * entries.size());
    Bytes table;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        u32(table, 0x1234U + static_cast<std::uint32_t>(index)); u32(table, static_cast<std::uint32_t>(index));
        u32(table, static_cast<std::uint32_t>(entries[index].second.size())); u32(table, offset);
        u32(table, static_cast<std::uint32_t>(index));
        offset += static_cast<std::uint32_t>(entries[index].second.size());
    }
    Bytes out; u32(out, static_cast<std::uint32_t>(entries.size())); u32(out, static_cast<std::uint32_t>(entries.size()));
    add(out, names); add(out, table);
    for (const auto& entry : entries) add(out, entry.second);
    return out;
}

void write(const std::filesystem::path& path, const Bytes& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
void write(const std::filesystem::path& path, const std::string& value) {
    write(path, Bytes(reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data()) + value.size()));
}
std::string manifest(const std::vector<std::string>& archives) {
    std::string xml = "<?xml version=\"1.0\"?>\n<Mega_Files>\n";
    for (const auto& archive : archives) xml += "  <File>" + archive + "</File>\n";
    return xml + "</Mega_Files>\n";
}

const std::vector<std::string> good_bones{"root", "arm", "hand"};
const std::vector<std::string> bad_bones{"root", "backpack", "hand"};
const std::vector<std::pair<std::uint32_t, std::string>> clip_tracks{{0, "root"}, {1, "arm"}};

constexpr std::string_view model_path = "data/art/models/ei_shocktrooper.alo";
constexpr std::string_view heavy_path = "data/art/models/ei_shocktrooper_heavy.alo";
constexpr std::array<sources::TargetModel, 2> fixture_targets{{{model_path, 2}, {heavy_path, 1}}};

struct TempTree final {
    std::filesystem::path root;
    TempTree() {
        root = std::filesystem::temp_directory_path()
            / ("eawr-source-versions-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "empty");
    }
    ~TempTree() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    [[nodiscard]] std::filesystem::path data(const std::string& layer) const { return root / layer / "Data"; }
};

// A mounted three-layer installation plus the layers' manifest resolutions.
struct Mounted final {
    std::vector<vfs::ManifestResolution> layers;
    std::optional<vfs::Vfs> vfs;
};

Mounted mount(const TempTree& tree) {
    Mounted mounted;
    std::vector<vfs::MountSpec> specs;
    for (const std::string layer : {"mod", "expansion", "base"}) {
        auto resolved = vfs::resolve_manifest_mount(layer, tree.data(layer));
        expect(static_cast<bool>(resolved), "fixture layer resolves: " + layer);
        if (!resolved) return mounted;
        specs.push_back(resolved.value().mount);
        mounted.layers.push_back(std::move(resolved.value()));
    }
    auto effective = vfs::Vfs::mount(specs);
    expect(static_cast<bool>(effective), "fixture installation mounts");
    if (effective) mounted.vfs = std::move(effective.value());
    return mounted;
}

// The standard fixture: the mod's loose models, clips for both targets, and an
// empty manifest-declared archive in every layer.  Tests add shadows.
void base_fixture(const TempTree& tree) {
    for (const std::string layer : {"mod", "expansion", "base"}) write(tree.data(layer) / "MegaFiles.xml", manifest({"Data/Models.meg"}));
    write(tree.data("mod") / "Art" / "Models" / "EI_Shocktrooper.ALO", alo(bad_bones));
    write(tree.data("mod") / "Art" / "Models" / "EI_Shocktrooper_Heavy.ALO", alo(bad_bones));
    write(tree.data("mod") / "Art" / "Models" / "EI_Shocktrooper_idle_00.ala", ala(clip_tracks));
    write(tree.data("mod") / "Art" / "Models" / "EI_Shocktrooper_move_00.ala", ala(clip_tracks));
    write(tree.data("mod") / "Art" / "Models" / "EI_Shocktrooper_Heavy_idle_00.ala", ala(clip_tracks));
}

const std::vector<std::string> fixture_alas{
    "data/art/models/ei_shocktrooper_idle_00.ala",
    "data/art/models/ei_shocktrooper_move_00.ala",
    "data/art/models/ei_shocktrooper_heavy_idle_00.ala",
};

// Frozen metadata recording exactly the current effective state: the
// "nothing drifted" baseline each test then perturbs.
audit::FrozenMetadata frozen_of(const vfs::Vfs& effective) {
    audit::FrozenMetadata frozen;
    frozen.animation_count = 10;
    frozen.playback_passed = 7;
    frozen.failure_count = fixture_alas.size();
    for (const std::string& model : {std::string(model_path), std::string(heavy_path)}) {
        auto record = effective.stat(model);
        auto bytes = effective.open(model);
        auto parsed = assets::load_model(bytes.value(), assets::source_from(record.value()));
        frozen.models[model] = {sources::identity_of(record.value(), sources::sha256_of(bytes.value())),
            parsed ? std::optional<std::size_t>(parsed.value().bones.size()) : std::nullopt};
    }
    for (std::size_t index = 0; index < fixture_alas.size(); ++index) {
        const std::string& path = fixture_alas[index];
        auto record = effective.stat(path);
        auto bytes = effective.open(path);
        audit::BaselineFailure failure;
        failure.animation = sources::identity_of(record.value(), sources::sha256_of(bytes.value()));
        failure.baseline_failure_index = index + 1;
        failure.stage = "binding";
        failure.selected_model = std::string(path.find("heavy") == std::string::npos ? model_path : heavy_path);
        failure.r0_candidates = {failure.selected_model};
        auto clip = assets::load_animation(bytes.value(), assets::source_from(record.value()));
        expect(static_cast<bool>(clip), "fixture clip parses");
        auto model_bytes = effective.open(failure.selected_model);
        auto parsed = assets::load_model(model_bytes.value(), assets::source_from(effective.stat(failure.selected_model).value()));
        expect(static_cast<bool>(parsed), "fixture effective model parses");
        if (clip && parsed) {
            auto player = presentation::animation::Player::create(parsed.value(), &clip.value());
            expect(!player, "fixture effective model fails binding");
            if (!player) { failure.code = player.error().code; failure.cause = player.error().message; }
        }
        frozen.failures.emplace(failure.baseline_failure_index, std::move(failure));
    }
    return frozen;
}

struct Run final {
    sources::Diagnosis diagnosis;
    std::string receipt;
};

Run run(const Mounted& mounted, const audit::FrozenMetadata& frozen, const std::filesystem::path& empty,
    std::vector<audit::BaselineFailure> selected = {}) {
    if (selected.empty()) {
        auto selection = sources::select_rows(frozen, fixture_targets);
        expect(selection.error.empty(), "fixture selection: " + selection.error);
        selected = std::move(selection.rows);
    }
    sources::ExactSourceReader reader(mounted.layers, empty);
    Run result;
    result.diagnosis = sources::diagnose(frozen, selected, *mounted.vfs, reader);
    if (result.diagnosis.error.empty()) {
        std::ostringstream output;
        sources::Header header;
        header.animation_count = frozen.animation_count;
        header.playback_passed = frozen.playback_passed;
        header.failure_count = frozen.failure_count;
        sources::write_receipt(output, header, result.diagnosis.model_sources, result.diagnosis.rows);
        result.receipt = output.str();
    }
    return result;
}

const sources::Row* row_for(const sources::Diagnosis& diagnosis, const std::string& ala_path) {
    for (const auto& row : diagnosis.rows) if (row.baseline.animation.path == ala_path) return &row;
    return nullptr;
}

std::string sha(const Bytes& bytes) { return sources::sha256_of(bytes); }

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

void test_tracked_selection() {
#if defined(EAWR_ASSOCIATION_FROZEN_MANIFEST)
    std::ifstream input(EAWR_ASSOCIATION_FROZEN_MANIFEST, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto digest = sources::sha256_of(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    auto load = audit::load_frozen_metadata(bytes, digest);
    expect(static_cast<bool>(load.metadata), "tracked frozen metadata authenticates: " + load.error);
    if (!load.metadata) return;
    const auto before = *load.metadata;
    auto selection = sources::select_rows(*load.metadata);
    expect(selection.error.empty(), "tracked metadata selects the shocktrooper rows: " + selection.error);
    expect(selection.rows.size() == sources::shocktrooper_rows, "exactly 132 rows are selected");
    std::map<std::string, std::size_t> by_model;
    for (const auto& row : selection.rows) {
        ++by_model[row.selected_model];
        expect(row.stage == "binding", "every selected row is a binding failure");
        expect(row.code == "EAWR-ANIMATION-0002", "every selected row has the frozen binding code");
    }
    expect(by_model[std::string(model_path)] == 70, "70 rows select ei_shocktrooper.alo");
    expect(by_model[std::string(heavy_path)] == 62, "62 rows select ei_shocktrooper_heavy.alo");
    expect(load.metadata->playback_passed == 7329 && load.metadata->failure_count == 356,
        "the frozen ledger counts stay 7329 passed / 356 retained");
    // Selection never mutates the frozen metadata.
    expect(load.metadata->failures.size() == before.failures.size(), "selection keeps every frozen failure");
    for (const auto& [index, failure] : before.failures)
        expect(!audit::failure_difference(failure, load.metadata->failures.at(index)), "selection leaves failures unchanged");

    // Count and stage drift refuse.
    auto fewer = *load.metadata;
    for (auto it = fewer.failures.begin(); it != fewer.failures.end(); ++it)
        if (it->second.selected_model == heavy_path) { fewer.failures.erase(it); break; }
    expect(!sources::select_rows(fewer).error.empty(), "61 heavy rows refuse");
    auto restaged = *load.metadata;
    for (auto& [index, failure] : restaged.failures)
        if (failure.selected_model == model_path) { failure.stage = "model_parse"; break; }
    expect(!sources::select_rows(restaged).error.empty(), "a non-binding shocktrooper row refuses");
#else
    expect(false, "EAWR_ASSOCIATION_FROZEN_MANIFEST is not defined");
#endif
}

// Compatible base archive shadowed by an incompatible mod loose model: the
// effective failure is retained, the shadow is compatible but unapproved.
void test_compatible_base_shadowed() {
    TempTree tree;
    base_fixture(tree);
    const Bytes good = alo(good_bones);
    write(tree.data("base") / "Models.meg", meg({{"DATA\\ART\\MODELS\\EI_SHOCKTROOPER.ALO", good}}));
    const Mounted mounted = mount(tree);
    if (!mounted.vfs) return;
    const auto frozen = frozen_of(*mounted.vfs);
    const Run result = run(mounted, frozen, tree.root / "empty");
    expect(result.diagnosis.error.empty(), "compatible-shadow diagnosis runs: " + result.diagnosis.error);
    expect(result.diagnosis.rows.size() == 3, "all three fixture rows are diagnosed");
    const sources::Row* row = row_for(result.diagnosis, fixture_alas[0]);
    expect(row != nullptr, "the idle row is present");
    if (!row) return;
    expect(row->status == sources::row_status::compatible_unapproved, "the row is shadow_compatible_unapproved");
    expect(row->effective_failure_retained, "the effective failure is retained");
    expect(row->effective.status == audit::candidate_status::rejected && row->effective.rejection_stage == "binding",
        "the effective model still fails binding");
    expect(row->effective_model.identity.source_id == "mod:loose:data/Art/Models/EI_Shocktrooper.ALO",
        "the effective model is the mod loose file");
    expect(row->shadows.size() == 1, "one shadow version");
    if (row->shadows.size() == 1) {
        const auto& shadow = row->shadows.front();
        expect(shadow.status == sources::shadow_status::compatible_unapproved, "the base shadow binds");
        expect(shadow.source.rank == 1 && shadow.source.identity.layer_id == "base"
            && shadow.source.identity.origin == "archive" && shadow.source.identity.source_id == "base:Data/Models.meg",
            "the shadow is the base archive version");
        expect(shadow.source.archive_slot == sources::archive_slot::manifest_declared, "the base archive is manifest-declared");
        expect(shadow.source.identity.sha256 == sha(good), "the shadow's own archive bytes were read");
        expect(shadow.result.samples.size() == 4, "four CPU samples");
        expect(!shadow.bytes_identical_to_effective, "shadow bytes differ from the effective model");
    }
    const sources::Row* heavy = row_for(result.diagnosis, fixture_alas[2]);
    expect(heavy && heavy->status == sources::row_status::no_shadow && heavy->shadows.empty(), "the heavy model has no shadow");
    expect(heavy && heavy->effective_failure_retained, "the heavy failure is retained");
    expect(result.receipt.find("\"approved\": true") == std::string::npos, "nothing is approved");
    expect(result.receipt.find("\"association_approved\": false") != std::string::npos, "the receipt disclaims approval");
    expect(result.receipt.find("\"ledger_mutated\": false") != std::string::npos, "the receipt disclaims ledger mutation");
    expect(result.receipt.find("\"playback_passed\": 7,") != std::string::npos, "the baseline pass count is copied, not changed");
    expect(result.receipt.find("\"effective_failures_retained\": 3") != std::string::npos, "every effective failure is retained");
}

// Every source version is read from its own source even when a loose file of
// the same layer wins, several archives hold the path, and the archives hold
// other entries too.
void test_archive_shadows_read_exactly() {
    TempTree tree;
    base_fixture(tree);
    write(tree.data("mod") / "MegaFiles.xml", manifest({"Data/Models.meg", "Data/ModArt.meg"}));
    write(tree.data("expansion") / "MegaFiles.xml", manifest({"Data/A.meg", "Data/B.meg"}));
    const Bytes mod_archive = alo({"root", "arm", "hand", "mod_archive"});
    const Bytes expansion_a = alo({"root", "arm", "hand", "expansion_a"});
    const Bytes expansion_b = alo({"root", "arm", "hand", "expansion_b"});
    const Bytes base_archive = alo({"root", "arm", "hand", "base_archive"});
    const Bytes patch = alo({"root", "arm", "hand", "patch"});
    write(tree.data("mod") / "ModArt.meg", meg({{"DATA\\ART\\MODELS\\OTHER.ALO", alo(good_bones)},
        {"DATA\\ART\\MODELS\\EI_SHOCKTROOPER.ALO", mod_archive}, {"DATA\\XML\\X.XML", Bytes{std::byte{'x'}}}}));
    write(tree.data("expansion") / "A.meg", meg({{"Data/Art/Models/EI_Shocktrooper.alo", expansion_a}}));
    write(tree.data("expansion") / "B.meg", meg({{"XML/Y.XML", Bytes{std::byte{'y'}}}, {"data\\art\\models\\ei_shocktrooper.ALO", expansion_b}}));
    write(tree.data("base") / "Models.meg", meg({{"DATA\\ART\\MODELS\\EI_SHOCKTROOPER.ALO", base_archive}}));
    write(tree.data("base") / "Patch.meg", meg({{"DATA\\ART\\MODELS\\EI_SHOCKTROOPER.ALO", patch}}));
    write(tree.data("base") / "Art" / "Models" / "EI_Shocktrooper.ALO", alo(good_bones));
    const Mounted mounted = mount(tree);
    if (!mounted.vfs) return;
    const auto frozen = frozen_of(*mounted.vfs);
    const Run result = run(mounted, frozen, tree.root / "empty");
    expect(result.diagnosis.error.empty(), "multi-source diagnosis runs: " + result.diagnosis.error);
    const auto found = result.diagnosis.model_sources.find(std::string(model_path));
    expect(found != result.diagnosis.model_sources.end(), "the model's versions are listed");
    if (found == result.diagnosis.model_sources.end()) return;
    const auto& versions = found->second;
    // Precedence: layer order; loose before archives; later archives first.
    const std::vector<std::pair<std::string, std::string>> expected{
        {"mod:loose:data/Art/Models/EI_Shocktrooper.ALO", sha(alo(bad_bones))},
        {"mod:Data/ModArt.meg", sha(mod_archive)},
        {"expansion:Data/B.meg", sha(expansion_b)},
        {"expansion:Data/A.meg", sha(expansion_a)},
        {"base:loose:data/Art/Models/EI_Shocktrooper.ALO", sha(alo(good_bones))},
        {"base:Data/Patch.meg", sha(patch)},
        {"base:Data/Models.meg", sha(base_archive)},
    };
    expect(versions.size() == expected.size(), "seven source versions of the exact path");
    for (std::size_t index = 0; index < std::min(versions.size(), expected.size()); ++index) {
        expect(versions[index].source.rank == index, "versions are in rank order");
        expect(versions[index].source.identity.source_id == expected[index].first,
            "version " + std::to_string(index) + " source is " + expected[index].first + ", got "
                + versions[index].source.identity.source_id);
        expect(versions[index].source.identity.sha256 == expected[index].second,
            "version " + std::to_string(index) + " bytes are its own source's bytes");
        expect(versions[index].model.has_value(), "version " + std::to_string(index) + " parses");
    }
    if (versions.size() == expected.size()) {
        expect(versions[5].source.archive_slot == sources::archive_slot::conventional, "Patch.meg is a conventional slot");
        expect(versions[6].source.archive_slot == sources::archive_slot::manifest_declared, "Models.meg is declared");
        expect(versions[4].source.archive_slot.empty(), "a loose version has no archive slot");
    }
    const sources::Row* row = row_for(result.diagnosis, fixture_alas[0]);
    expect(row && row->shadows.size() == 6, "six shadows are evaluated");
    expect(row && row->status == sources::row_status::compatible_unapproved, "compatible shadows mark the row");
    if (row) {
        std::size_t compatible = 0;
        for (const auto& shadow : row->shadows) if (shadow.status == sources::shadow_status::compatible_unapproved) ++compatible;
        expect(compatible == 6, "every shadow whose index 1 is 'arm' binds");
    }
}

// No, malformed, multiple and identical-byte shadows stay distinct.
void test_shadow_kinds() {
    TempTree tree;
    base_fixture(tree);
    write(tree.data("expansion") / "Models.meg", meg({{"DATA\\ART\\MODELS\\EI_SHOCKTROOPER.ALO", Bytes{std::byte{1}, std::byte{2}}}}));
    write(tree.data("base") / "Models.meg", meg({{"DATA\\ART\\MODELS\\EI_SHOCKTROOPER.ALO", alo(bad_bones)}}));
    write(tree.data("base") / "Art" / "Models" / "EI_Shocktrooper.ALO", alo(bad_bones));
    const Mounted mounted = mount(tree);
    if (!mounted.vfs) return;
    const auto frozen = frozen_of(*mounted.vfs);
    const Run result = run(mounted, frozen, tree.root / "empty");
    expect(result.diagnosis.error.empty(), "shadow-kind diagnosis runs: " + result.diagnosis.error);
    const sources::Row* row = row_for(result.diagnosis, fixture_alas[0]);
    expect(row && row->shadows.size() == 3, "three shadows");
    if (!row || row->shadows.size() != 3) return;
    expect(row->shadows[0].status == sources::shadow_status::parse_failed, "the malformed expansion version fails to parse");
    expect(!row->shadows[0].result.error_code.empty(), "the parser failure keeps its code");
    expect(row->shadows[1].status == sources::shadow_status::rejected && row->shadows[2].status == sources::shadow_status::rejected,
        "identical-byte base versions are rejected");
    expect(row->shadows[1].bytes_identical_to_effective && row->shadows[2].bytes_identical_to_effective,
        "identical bytes are flagged");
    expect(row->shadows[1].source.identity.sha256 == row->shadows[2].source.identity.sha256
        && row->shadows[1].source.identity.source_id != row->shadows[2].source.identity.source_id,
        "identical-byte versions from two sources stay distinct");
    expect(row->status == sources::row_status::rejected, "rejected outranks parse_failed");
    expect(result.diagnosis.model_sources.at(std::string(model_path)).size() == 4, "each source is parsed and listed once");

    // Only a malformed shadow.
    TempTree only;
    base_fixture(only);
    write(only.data("base") / "Models.meg", meg({{"DATA\\ART\\MODELS\\EI_SHOCKTROOPER.ALO", Bytes{std::byte{7}}}}));
    const Mounted malformed = mount(only);
    if (!malformed.vfs) return;
    const Run parse_only = run(malformed, frozen_of(*malformed.vfs), only.root / "empty");
    const sources::Row* parse_row = row_for(parse_only.diagnosis, fixture_alas[0]);
    expect(parse_row && parse_row->status == sources::row_status::parse_failed, "a lone malformed shadow is shadow_parse_failed");
    expect(parse_row && parse_row->effective_failure_retained, "and the effective failure is retained");
}

// Row order and model-cache warm-up order do not change the receipt.
void test_order_invariant() {
    TempTree tree;
    base_fixture(tree);
    write(tree.data("base") / "Models.meg", meg({{"DATA\\ART\\MODELS\\EI_SHOCKTROOPER.ALO", alo(good_bones)},
        {"DATA\\ART\\MODELS\\EI_SHOCKTROOPER_HEAVY.ALO", alo(good_bones)}}));
    const Mounted mounted = mount(tree);
    if (!mounted.vfs) return;
    const auto frozen = frozen_of(*mounted.vfs);
    const Run reference = run(mounted, frozen, tree.root / "empty");
    expect(reference.diagnosis.error.empty() && !reference.receipt.empty(), "reference receipt written");
    auto selected = sources::select_rows(frozen, fixture_targets).rows;
    std::mt19937 random(20260923U);
    for (int round = 0; round < 8; ++round) {
        std::shuffle(selected.begin(), selected.end(), random);
        const Run shuffled = run(mounted, frozen, tree.root / "empty", selected);
        expect(shuffled.receipt == reference.receipt, "shuffled input order gives a byte-identical receipt");
        auto rows = shuffled.diagnosis.rows;
        std::shuffle(rows.begin(), rows.end(), random);
        std::ostringstream output;
        sources::Header header;
        header.animation_count = frozen.animation_count;
        header.playback_passed = frozen.playback_passed;
        header.failure_count = frozen.failure_count;
        sources::write_receipt(output, header, shuffled.diagnosis.model_sources, rows);
        expect(output.str() == reference.receipt, "shuffled row order gives a byte-identical receipt");
    }
    for (const char byte : reference.receipt)
        expect(static_cast<unsigned char>(byte) >= 0x20U || byte == '\n', "the receipt holds no raw control byte");
}

// Identity mismatch, hash drift and ambiguous sources refuse.
void test_refusals() {
    TempTree tree;
    base_fixture(tree);
    write(tree.data("base") / "Models.meg", meg({{"DATA\\ART\\MODELS\\EI_SHOCKTROOPER.ALO", alo(good_bones)}}));
    const Mounted mounted = mount(tree);
    if (!mounted.vfs) return;
    const auto frozen = frozen_of(*mounted.vfs);
    const auto empty = tree.root / "empty";

    auto stale_model = frozen;
    stale_model.models.at(std::string(model_path)).identity.sha256 = std::string(64, 'a');
    expect(!run(mounted, stale_model, empty).diagnosis.error.empty(), "a stale effective-model hash refuses");
    auto moved_model = frozen;
    moved_model.models.at(std::string(model_path)).identity.layer_id = "base";
    expect(!run(mounted, moved_model, empty).diagnosis.error.empty(), "an effective model from another layer refuses");
    auto stale_clip = frozen;
    stale_clip.failures.at(1).animation.sha256 = std::string(64, 'b');
    auto stale_rows = sources::select_rows(frozen, fixture_targets).rows;
    stale_rows.front().animation.sha256 = std::string(64, 'b');
    expect(!run(mounted, stale_clip, empty, stale_rows).diagnosis.error.empty(), "a stale ALA hash refuses");
    auto rebaselined = sources::select_rows(frozen, fixture_targets).rows;
    rebaselined.front().cause = "another cause";
    expect(!run(mounted, frozen, empty, rebaselined).diagnosis.error.empty(), "a baseline that differs from frozen refuses");
    auto now_binds = frozen;
    for (auto& [index, failure] : now_binds.failures) failure.code = "EAWR-ANIMATION-0001";
    auto now_rows = sources::select_rows(now_binds, fixture_targets).rows;
    expect(!run(mounted, now_binds, empty, now_rows).diagnosis.error.empty(), "a failure that no longer reproduces refuses");

    // The reader itself.
    sources::ExactSourceReader reader(mounted.layers, empty);
    auto records = mounted.vfs->candidates(model_path);
    expect(records && records.value().size() == 2, "two versions of the model");
    if (!records || records.value().size() != 2) return;
    const auto archive = records.value()[1];
    expect(reader.read(archive).bytes.has_value(), "the base archive version reads");
    auto resized = archive; resized.size += 1;
    expect(!reader.read(resized).bytes, "a record whose size differs refuses");
    auto renamed = archive; renamed.original_path = "data/art/models/EI_SHOCKTROOPER.ALO";
    expect(!reader.read(renamed).bytes, "a record whose original path differs refuses");
    auto unknown = archive; unknown.source_id = "base:Data/Other.meg";
    expect(!reader.read(unknown).bytes, "an archive source the layer does not mount refuses");
    auto relayered = archive; relayered.layer_id = "patch";
    expect(!reader.read(relayered).bytes, "an unmounted layer refuses");
    auto loose = records.value()[0]; loose.source_id = "mod:loose:data/Art/Models/other.alo";
    expect(!reader.read(loose).bytes, "a loose source id that does not name the file refuses");

    write(tree.root / "dirty" / "Art" / "Models" / "EI_Shocktrooper.ALO", alo(bad_bones));
    sources::ExactSourceReader dirty(mounted.layers, tree.root / "dirty");
    expect(!dirty.read(archive).bytes, "a non-empty scratch root refuses archive reads");

    auto duplicate_layers = mounted.layers;
    duplicate_layers.push_back(mounted.layers.back());
    sources::ExactSourceReader duplicate(duplicate_layers, empty);
    expect(!duplicate.read(archive).bytes, "a layer id that is not unique refuses");
    auto duplicate_archives = mounted.layers;
    duplicate_archives.back().mount.active_archives.push_back(duplicate_archives.back().mount.active_archives.front());
    sources::ExactSourceReader twice(duplicate_archives, empty);
    expect(!twice.read(archive).bytes, "two active archives with one source id refuse");

    // A reader over another installation whose records match field for field
    // but whose winning bytes differ (same size, one byte changed): only the
    // winner-versus-rank-0 hash check can catch it.
    TempTree other;
    base_fixture(other);
    write(other.data("mod") / "Art" / "Models" / "EI_Shocktrooper.ALO", alo({"root", "backpacc", "hand"}));
    write(other.data("base") / "Models.meg", meg({{"DATA\\ART\\MODELS\\EI_SHOCKTROOPER.ALO", alo(good_bones)}}));
    const Mounted drifted = mount(other);
    if (!drifted.vfs) return;
    sources::ExactSourceReader cross(drifted.layers, empty);
    const auto list = sources::enumerate_versions(*mounted.vfs, cross, std::string(model_path), false);
    expect(list.error.find("hash drift") != std::string::npos,
        "same-record exact bytes that differ from the effective winner refuse as hash drift: " + list.error);
    const auto base_only = sources::enumerate_versions(*mounted.vfs, cross, fixture_alas[0], false);
    expect(base_only.error.empty(), "the unchanged clip still reads through the other installation");
}

// Pure row evaluation with in-memory views.
void test_evaluate_row() {
    const auto bad = assets::load_model(alo(bad_bones), {"m.alo", "fixture", "mod", vfs::AssetOrigin::loose, 0});
    const auto good = assets::load_model(alo(good_bones), {"m.alo", "fixture", "mod", vfs::AssetOrigin::loose, 0});
    const auto clip = assets::load_animation(ala(clip_tracks), {"c.ala", "fixture", "mod", vfs::AssetOrigin::loose, 0});
    expect(bad && good && clip, "in-memory fixtures parse");
    if (!bad || !good || !clip) return;
    const std::string model(model_path);
    const audit::AssetIdentity model_identity{model, std::string(64, '1'), "mod", "loose", "mod:loose:x", "x", 10};
    const audit::AssetIdentity clip_identity{"data/art/models/ei_shocktrooper_idle_00.ala", std::string(64, '2'), "mod",
        "loose", "mod:loose:y", "y", 20};
    auto player = presentation::animation::Player::create(bad.value(), &clip.value());
    audit::FrozenMetadata frozen;
    audit::BaselineFailure baseline{clip_identity, 1, "binding", player ? "" : player.error().code,
        player ? "" : player.error().message, model, {model}};
    frozen.failures.emplace(1, baseline);
    frozen.models[model] = {model_identity, bad.value().bones.size()};
    const auto view = [](const audit::AssetIdentity& identity, const assets::Model* parsed) {
        audit::CandidateModelView value;
        value.found = true; value.identity = identity; value.model = parsed;
        if (!parsed) { value.error_code = "EAWR-ASSET-0001"; value.error_message = "malformed"; }
        return value;
    };
    auto shadow_identity = model_identity; shadow_identity.layer_id = "base"; shadow_identity.source_id = "base:Data/M.meg";
    shadow_identity.origin = "archive"; shadow_identity.sha256 = std::string(64, '3');
    const std::vector<sources::SourceIdentity> alas{{0, clip_identity, ""}};
    const std::vector<sources::ModelSource> none{{{0, model_identity, ""}, view(model_identity, &bad.value())}};
    const auto plain = sources::evaluate_row(frozen, baseline, clip.value(), alas, none);
    expect(plain.row && plain.row->status == sources::row_status::no_shadow, "no shadow is no_shadow");
    auto parse = none; parse.push_back({{1, shadow_identity, "manifest_declared"}, view(shadow_identity, nullptr)});
    const auto parsed = sources::evaluate_row(frozen, baseline, clip.value(), alas, parse);
    expect(parsed.row && parsed.row->status == sources::row_status::parse_failed, "a malformed shadow is shadow_parse_failed");
    auto compatible = parse; compatible.push_back({{2, shadow_identity, ""}, view(shadow_identity, &good.value())});
    compatible.back().source.identity.source_id = compatible.back().view.identity.source_id = "base:Data/N.meg";
    const auto mixed = sources::evaluate_row(frozen, baseline, clip.value(), alas, compatible);
    expect(mixed.row && mixed.row->status == sources::row_status::compatible_unapproved && mixed.row->shadows.size() == 2,
        "compatible outranks parse_failed and every shadow is kept");
    expect(mixed.row && mixed.row->effective_failure_retained, "the effective failure is retained");

    auto rejected = none; rejected.push_back({{1, shadow_identity, ""}, view(shadow_identity, &bad.value())});
    const auto rejected_row = sources::evaluate_row(frozen, baseline, clip.value(), alas, rejected);
    expect(rejected_row.row && rejected_row.row->status == sources::row_status::rejected, "a rejected shadow is shadow_rejected");
    auto both = rejected; both.push_back({{2, shadow_identity, ""}, view(shadow_identity, &good.value())});
    both.back().source.identity.source_id = both.back().view.identity.source_id = "base:Data/N.meg";
    const auto both_row = sources::evaluate_row(frozen, baseline, clip.value(), alas, both);
    expect(both_row.row && both_row.row->status == sources::row_status::compatible_unapproved,
        "compatible outranks rejected");
    expect(both_row.row && both_row.row->shadows.size() == 2 && both_row.row->shadows[0].status == sources::shadow_status::rejected,
        "the rejected shadow keeps its own status");

    auto misordered = compatible; std::swap(misordered[1], misordered[2]);
    expect(!sources::evaluate_row(frozen, baseline, clip.value(), alas, misordered).row, "versions out of rank order refuse");
    auto effective_binds = none; effective_binds[0].view.model = &good.value();
    expect(!sources::evaluate_row(frozen, baseline, clip.value(), alas, effective_binds).row,
        "an effective model that binds refuses (the failure did not reproduce)");
    auto other_clip = alas; other_clip[0].identity.source_id = "base:Data/C.meg";
    expect(!sources::evaluate_row(frozen, baseline, clip.value(), other_clip, none).row, "an effective ALA of another source refuses");
    expect(!sources::evaluate_row(frozen, baseline, clip.value(), {}, none).row, "no ALA version refuses");
    expect(!sources::evaluate_row(frozen, baseline, clip.value(), alas, {}).row, "no model version refuses");
    auto sibling = none; sibling[0].source.identity.path = sibling[0].view.identity.path = "data/art/models/ei_shocktrooper_heavy.alo";
    expect(!sources::evaluate_row(frozen, baseline, clip.value(), alas, sibling).row, "a sibling path is never evaluated");
}

} // namespace

int main() {
    test_tracked_selection();
    test_evaluate_row();
    test_compatible_base_shadowed();
    test_archive_shadows_read_exactly();
    test_shadow_kinds();
    test_order_invariant();
    test_refusals();
    if (failures != 0) {
        std::cerr << failures << " source-version check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "animation source-version diagnosis checks passed\n";
    return EXIT_SUCCESS;
}
