#include "corpus_model_match_discovery.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
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
namespace corpus = eawr::tests::animation_corpus;
namespace audit = eawr::tests::animation_corpus::associations;
namespace sources = eawr::tests::animation_corpus::source_versions;
namespace structural = eawr::tests::animation_corpus::structural_discovery;
namespace discovery = eawr::tests::animation_corpus::model_match_discovery;

int failures{};
void expect(const bool condition, const std::string_view message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; }
}
bool contains(const std::string& haystack, const std::string_view needle) { return haystack.find(needle) != std::string::npos; }

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

// Minimal ALO with explicit parents.
Bytes alo_with_parents(const std::vector<std::pair<std::string, std::int32_t>>& bones) {
    Bytes skeleton;
    add(skeleton, chunk(0x201, integer(static_cast<std::uint32_t>(bones.size()))));
    for (const auto& [bone_name, parent] : bones) {
        Bytes name; text(name, bone_name);
        Bytes data; u32(data, std::bit_cast<std::uint32_t>(parent)); u32(data, 1);
        for (int item = 0; item < 12; ++item) f32(data, (item == 0 || item == 4 || item == 8) ? 1.0F : 0.0F);
        Bytes bone; add(bone, chunk(0x203, name)); add(bone, chunk(0x205, data));
        add(skeleton, chunk(0x202, bone, true));
    }
    Bytes header; mini(header, 1, integer(0)); mini(header, 4, integer(0));
    Bytes connections; add(connections, chunk(0x601, header));
    Bytes file; add(file, chunk(0x200, skeleton, true)); add(file, chunk(0x600, connections, true));
    return file;
}

// Minimal ALO: a root plus children of the root.
Bytes alo(const std::vector<std::string>& names) {
    std::vector<std::pair<std::string, std::int32_t>> bones;
    for (std::size_t index = 0; index < names.size(); ++index) bones.emplace_back(names[index], index == 0 ? -1 : 0);
    return alo_with_parents(bones);
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

const std::vector<std::pair<std::uint32_t, std::string>> idle_tracks{{0, "root"}, {1, "arm"}};
const std::vector<std::pair<std::uint32_t, std::string>> die_tracks{{0, "root"}, {1, "absent_bone"}};
const std::vector<std::pair<std::uint32_t, std::string>> single_tracks{{0, "root"}, {1, "gun"}};

constexpr std::string_view idle_path = "data/art/models/ri_padowan_idle_00.ala";
constexpr std::string_view die_path = "data/art/models/underworld_test_die_00.ala";
constexpr std::string_view single_path = "data/art/other/w_single_idle_00.ala";
constexpr std::string_view gun_path = "data/art/models/gun.alo";
constexpr std::array<discovery::TargetRow, 3> fixture_targets{{{1, idle_path}, {2, die_path}, {3, single_path}}};

struct TempTree final {
    std::filesystem::path root;
    TempTree() {
        root = std::filesystem::temp_directory_path()
            / ("eawr-model-match-discovery-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
    }
    ~TempTree() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    [[nodiscard]] std::filesystem::path data(const std::string& layer) const { return root / layer / "Data"; }
    [[nodiscard]] std::filesystem::path models(const std::string& layer) const { return data(layer) / "Art" / "Models"; }
};

std::optional<vfs::Vfs> mount(const TempTree& tree) {
    std::vector<vfs::MountSpec> specs;
    for (const std::string layer : {"mod", "expansion", "base"}) {
        auto resolved = vfs::resolve_manifest_mount(layer, tree.data(layer));
        expect(static_cast<bool>(resolved), "fixture layer resolves: " + layer);
        if (!resolved) return std::nullopt;
        specs.push_back(resolved.value().mount);
    }
    auto effective = vfs::Vfs::mount(specs);
    expect(static_cast<bool>(effective), "fixture installation mounts");
    if (!effective) return std::nullopt;
    return std::move(effective.value());
}

// The discovery fixture.  No model name is an R0 prefix of any clip, and no
// name hints at the result: an unrelated name matches, a similar one does not.
void fixture(const TempTree& tree) {
    for (const std::string layer : {"mod", "expansion", "base"}) write(tree.data(layer) / "MegaFiles.xml", manifest({"Data/Models.meg"}));
    const auto models = tree.models("mod");
    write(models / "RI_Padowan_Idle_00.ALA", ala(idle_tracks));
    write(models / "Underworld_Test_Die_00.ALA", ala(die_tracks));
    write(models / "ZZ_Unrelated.ALO", alo({"root", "arm"}));                      // unrelated name, exact match
    write(models / "Similar.ALO", alo({"root", "arms"}));                          // wrong bone name
    write(models / "Case.ALO", alo({"root", "Arm"}));                              // case differs
    write(models / "Shift.ALO", alo({"arm", "root"}));                             // permuted indices
    write(models / "Short.ALO", alo({"root"}));                                    // index out of range
    write(models / "Hierarchy.ALO", alo_with_parents({{"root", 1}, {"arm", -1}})); // parses, Player rejects
    write(models / "Broken.ALO", Bytes{std::byte{1}, std::byte{2}});               // parser exclusion
    write(models / "Gun.ALO", alo({"root", "gun", "x"}));                          // the only single-row match
    write(models / "ZZ_Shadowed.ALO", alo({"root", "nope"}));
    write(tree.data("mod") / "Art" / "Other" / "Extra.ALO", alo({"root", "arm", "hand", "extra"}));  // other directory, extra bones
    // The single-row clip is a base archive entry.  The base copy of
    // ZZ_Shadowed matches the idle clip but is shadowed by the mod's loose
    // winner: never a candidate.
    write(tree.data("base") / "Models.meg", meg({{"DATA\\ART\\OTHER\\W_SINGLE_IDLE_00.ALA", ala(single_tracks)},
        {"DATA\\ART\\MODELS\\ZZ_SHADOWED.ALO", alo({"root", "arm"})}}));
}

audit::AssetIdentity identity_at(const vfs::Vfs& effective, const std::string& path) {
    auto record = effective.stat(path);
    auto bytes = effective.open(path);
    expect(record && bytes, "fixture asset opens: " + path);
    if (!record || !bytes) return {};
    return sources::identity_of(record.value(), sources::sha256_of(bytes.value()));
}

// Frozen metadata recording exactly the current effective state: three
// model_match failures, plus one binding failure the selection must ignore
// whose selected model is also a discovered candidate.
audit::FrozenMetadata frozen_of(const vfs::Vfs& effective) {
    audit::FrozenMetadata frozen;
    frozen.animation_count = 10;
    frozen.playback_passed = 6;
    frozen.failure_count = 4;
    for (const auto& target : fixture_targets) {
        audit::BaselineFailure failure;
        failure.animation = identity_at(effective, std::string(target.path));
        failure.baseline_failure_index = target.index;
        failure.stage = std::string(discovery::model_match_stage);
        failure.code = std::string(discovery::model_match_code);
        failure.cause = std::string(discovery::model_match_cause);
        frozen.failures.emplace(target.index, std::move(failure));
    }
    audit::BaselineFailure binding;
    binding.animation = identity_at(effective, std::string(idle_path));
    binding.baseline_failure_index = 4;
    binding.stage = "binding";
    binding.code = "EAWR-ANIMATION-0002";
    binding.cause = "animation track does not map exactly to model bone name/index";
    binding.selected_model = std::string(gun_path);
    binding.r0_candidates = {binding.selected_model};
    frozen.failures.emplace(4, std::move(binding));
    frozen.models[std::string(gun_path)] = {identity_at(effective, std::string(gun_path)), 3};
    return frozen;
}

std::vector<audit::BaselineFailure> selected_of(const audit::FrozenMetadata& frozen) {
    auto selection = discovery::select_rows(frozen, fixture_targets);
    expect(selection.error.empty() && selection.rows.size() == 3, "fixture selection: " + selection.error);
    return std::move(selection.rows);
}

struct Run final {
    structural::Inventory inventory;
    discovery::Diagnosis diagnosis;
    std::string receipt;
};

std::string receipt_of(const audit::FrozenMetadata& frozen, const structural::Inventory& inventory, std::vector<discovery::Row> rows) {
    std::ostringstream output;
    discovery::Header header;
    header.animation_count = frozen.animation_count;
    header.playback_passed = frozen.playback_passed;
    header.failure_count = frozen.failure_count;
    discovery::write_receipt(output, header, inventory, std::move(rows));
    return output.str();
}

Run run(const vfs::Vfs& effective, const audit::FrozenMetadata& frozen, std::vector<audit::BaselineFailure> selected = {}) {
    if (selected.empty()) selected = selected_of(frozen);
    Run result;
    result.inventory = structural::build_inventory(effective);
    const auto index = structural::build_index(result.inventory);
    result.diagnosis = discovery::diagnose(frozen, selected, effective, result.inventory, index);
    if (result.diagnosis.error.empty()) result.receipt = receipt_of(frozen, result.inventory, result.diagnosis.rows);
    return result;
}

const discovery::Row* row_for(const discovery::Diagnosis& diagnosis, const std::string_view ala_path) {
    for (const auto& row : diagnosis.rows) if (row.baseline.animation.path == ala_path) return &row;
    return nullptr;
}

std::vector<std::string> candidate_paths(const discovery::Row& row) {
    std::vector<std::string> paths;
    for (const auto& candidate : row.candidates) paths.push_back(candidate.result.identity.path);
    std::sort(paths.begin(), paths.end());
    return paths;
}

const discovery::Candidate* candidate_for(const discovery::Row& row, const std::string& path) {
    for (const auto& candidate : row.candidates) if (candidate.result.identity.path == path) return &candidate;
    return nullptr;
}

bool has_predicate(const audit::CandidateResult& result, const std::string_view predicate) {
    return result.diagnosis && std::any_of(result.diagnosis->failures.begin(), result.diagnosis->failures.end(),
        [predicate](const auto& failure) { return failure.predicate == predicate; });
}

std::size_t count(const std::string& haystack, const std::string_view needle) {
    std::size_t result = 0;
    for (auto at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) ++result;
    return result;
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

// The tracked manifest selects exactly the 22 frozen rows, and any drift in
// them, or any other model_match row, refuses.
void test_tracked_selection() {
#if defined(EAWR_ASSOCIATION_FROZEN_MANIFEST)
    std::ifstream input(EAWR_ASSOCIATION_FROZEN_MANIFEST, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto digest = sources::sha256_of(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    expect(digest == "2c3456374eed6a5dff02cecbfd727c0ad0942ca8e7d932b8fb5142e52a5678e2", "the tracked manifest is the pinned one");
    auto load = audit::load_frozen_metadata(bytes, digest);
    expect(static_cast<bool>(load.metadata), "tracked frozen metadata authenticates: " + load.error);
    if (!load.metadata) return;
    const audit::FrozenMetadata& frozen = *load.metadata;
    expect(!discovery::check_baseline_counts(frozen), "the frozen baseline stays 7685 / 7329 / 356");
    const auto selection = discovery::select_rows(frozen);
    expect(selection.error.empty() && selection.rows.size() == 22, "tracked metadata selects exactly 22 rows: " + selection.error);
    std::vector<std::size_t> indices;
    std::size_t padowan = 0;
    for (const auto& row : selection.rows) {
        indices.push_back(row.baseline_failure_index);
        if (row.animation.path.starts_with("data/art/models/ri_padowan_")) ++padowan;
        expect(!contains(row.animation.path, "padawan"), "the asset spelling padowan is preserved");
        expect(row.stage == "model_match" && row.code == "EAWR-ANIMATION-CORPUS-0001" && row.selected_model.empty()
            && row.r0_candidates.empty(), "every selected row is an original model_match failure");
    }
    std::vector<std::size_t> expected;
    for (std::size_t index = 273; index <= 292; ++index) expected.push_back(index);
    expected.push_back(337);
    expected.push_back(345);
    expect(indices == expected, "the selected indices are 273-292, 337 and 345");
    expect(padowan == 20, "twenty ri_padowan rows");
    expect(selection.rows.size() == 22 && selection.rows[20].animation.path == "data/art/models/underworld_hutt_privateer_die_00.ala"
        && selection.rows[21].animation.path == "data/art/models/w_allshaders_idle_00.ala", "the two other rows");

    const auto refused = [](const audit::FrozenMetadata& metadata) { return !discovery::select_rows(metadata).error.empty(); };
    const auto mutated = [&frozen](const std::function<void(audit::FrozenMetadata&)>& change) {
        auto copy = frozen;
        change(copy);
        return copy;
    };
    expect(refused(mutated([](auto& m) { m.failures.at(273).code = "EAWR-ANIMATION-0002"; })), "a different code refuses");
    expect(refused(mutated([](auto& m) { m.failures.at(337).cause = "another cause"; })), "a different cause refuses");
    expect(refused(mutated([](auto& m) { m.failures.at(345).animation.path = "data/art/models/w_other_idle_00.ala"; })),
        "a different path refuses");
    expect(refused(mutated([](auto& m) { m.failures.erase(292); })), "a missing row refuses");
    expect(refused(mutated([](auto& m) { m.failures.at(283).stage = "binding"; })), "a target that is not model_match refuses");
    expect(refused(mutated([](auto& m) { m.failures.at(280).selected_model = "data/art/models/ri_padowan.alo"; })),
        "a target with a selected model refuses");
    expect(refused(mutated([](auto& m) { m.failures.at(281).r0_candidates = {"data/art/models/ri_padowan.alo"}; })),
        "a target with an R0 candidate refuses");
    expect(refused(mutated([](auto& m) {
        auto& other = m.failures.at(1);
        other.stage = "model_match"; other.selected_model.clear(); other.r0_candidates.clear();
    })), "an extra model_match row outside the selection refuses");
    const std::array<discovery::TargetRow, 2> twice{{{273, "data/art/models/ri_padowan_attack_00.ala"},
        {273, "data/art/models/ri_padowan_attack_00.ala"}}};
    expect(!discovery::select_rows(frozen, twice).error.empty(), "a target listed twice refuses");
    expect(static_cast<bool>(discovery::check_baseline_counts(mutated([](auto& m) { m.playback_passed = 7330; }))),
        "a different baseline pass count refuses");
    auto tampered = bytes;
    tampered.back() = ' ';
    const auto tampered_digest = sources::sha256_of(std::as_bytes(std::span<const char>(tampered.data(), tampered.size())));
    expect(!audit::load_frozen_metadata(tampered, tampered_digest).metadata, "a stale frozen manifest refuses");
#else
    expect(false, "EAWR_ASSOCIATION_FROZEN_MANIFEST is not defined");
#endif
}

// Name-blind exact discovery over the effective VFS with zero, one and
// several compatible models, including models in other directories.
void test_discovery_rules() {
    TempTree tree;
    fixture(tree);
    const auto effective = mount(tree);
    if (!effective) return;
    const auto frozen = frozen_of(*effective);
    const Run result = run(*effective, frozen);
    expect(result.diagnosis.error.empty(), "discovery runs: " + result.diagnosis.error);
    expect(result.diagnosis.rows.size() == 3, "all three rows are diagnosed");

    const auto& inventory = result.inventory;
    expect(inventory.models.size() == 10, "ten effective models, got " + std::to_string(inventory.models.size()));
    expect(inventory.raw_versions == 11 && inventory.shadowed_versions_excluded == 1, "one shadowed version is excluded");
    const auto* broken = inventory.find("data/art/models/broken.alo");
    expect(broken && broken->status == discovery::model_status::parse_failed && !broken->error_code.empty() && !broken->skeleton,
        "the malformed model is a visible parse exclusion with its parser code");
    const auto* shadowed = inventory.find("data/art/models/zz_shadowed.alo");
    expect(shadowed && shadowed->identity.source_id == "mod:loose:data/Art/Models/ZZ_Shadowed.ALO",
        "only the effective winner of a shadowed path is inventoried");

    for (const auto& row : result.diagnosis.rows) {
        expect(row.original_failure_retained && row.r0.qualifying.empty() && row.baseline.stage == "model_match"
            && row.baseline.selected_model.empty() && row.baseline.r0_candidates.empty(),
            "every row retains its original model_match failure with R0 still empty");
    }

    const discovery::Row* idle = row_for(result.diagnosis, idle_path);
    expect(idle != nullptr, "the idle row is present");
    if (idle) {
        const std::vector<std::string> expected{"data/art/models/hierarchy.alo", "data/art/models/zz_unrelated.alo",
            "data/art/other/extra.alo"};
        expect(candidate_paths(*idle) == expected, "exactly the exact-skeleton models are shortlisted");
        expect(idle->shortlisted == 3 && idle->evaluated == 3 && idle->compatible == 2 && idle->rejected_binding == 1,
            "three shortlisted and evaluated, two compatible, one rejected at binding");
        expect(idle->status == discovery::row_status::multiple_compatible, "several compatible models are multiple_compatible_unapproved");
        expect(idle->r0.directory == "data/art/models/" && idle->r0.stem == "ri_padowan_idle_00"
            && idle->r0.same_directory_models == 9 && idle->r0.same_directory_unparsed == 1,
            "R0 was re-run over all nine same-directory models, including the unparsed one");
        const auto* unrelated = candidate_for(*idle, "data/art/models/zz_unrelated.alo");
        expect(unrelated && unrelated->result.status == audit::candidate_status::compatible_unapproved
            && unrelated->result.samples.size() == 4 && unrelated->directory_relation == discovery::directory_relation::same
            && unrelated->result.frozen_identity == structural::not_in_frozen_metadata,
            "an unrelated name that matches exactly is compatible but unapproved, with four samples");
        const auto* extra = candidate_for(*idle, "data/art/other/extra.alo");
        expect(extra && extra->result.status == audit::candidate_status::compatible_unapproved
            && extra->result.bone_count == std::optional<std::size_t>(4)
            && extra->directory_relation == discovery::directory_relation::other,
            "extra untracked bones are allowed, in another directory");
        const auto* hierarchy = candidate_for(*idle, "data/art/models/hierarchy.alo");
        expect(hierarchy && hierarchy->result.status == audit::candidate_status::rejected
            && hierarchy->result.rejection_stage == "binding" && !hierarchy->result.player_accepts
            && hierarchy->result.consistent_with_player && hierarchy->result.samples.empty()
            && has_predicate(hierarchy->result, corpus::predicates::model_hierarchy),
            "a shortlisted model that strict Player rejects is rejected with its predicate");
        for (const std::string& rejected : std::vector<std::string>{"data/art/models/similar.alo", "data/art/models/case.alo",
                 "data/art/models/shift.alo", "data/art/models/short.alo", "data/art/models/zz_shadowed.alo",
                 "data/art/models/broken.alo", std::string(gun_path)})
            expect(!candidate_for(*idle, rejected), "not shortlisted: " + rejected);
    }
    const discovery::Row* die = row_for(result.diagnosis, die_path);
    expect(die && die->status == discovery::row_status::zero_compatible && die->shortlisted == 0 && die->candidates.empty()
        && die->original_failure_retained, "a clip no model satisfies is zero_compatible and keeps its failure");
    const discovery::Row* single = row_for(result.diagnosis, single_path);
    expect(single && single->status == discovery::row_status::single_compatible
        && candidate_paths(*single) == std::vector<std::string>{std::string(gun_path)},
        "one compatible model is single_compatible_unapproved");
    if (single) {
        expect(single->baseline.animation.origin == "archive" && single->r0.directory == "data/art/other/"
            && single->r0.same_directory_models == 1, "the archive clip's R0 directory holds one model");
        const auto* gun = candidate_for(*single, std::string(gun_path));
        expect(gun && gun->directory_relation == discovery::directory_relation::other
            && gun->result.frozen_identity == audit::frozen_identity::matched,
            "a candidate in another directory that the frozen metadata records is frozen-matched");
    }

    const std::string& receipt = result.receipt;
    expect(contains(receipt, "\"schema\": \"eawr.animation-model-match-structural-discovery\",\n  \"schema_version\": 1"),
        "the receipt names its schema");
    expect(!contains(receipt, "\"approved\": true"), "nothing is approved");
    expect(count(receipt, "\"approved\": false") == 3 + 4, "every row and candidate is unapproved");
    for (const std::string_view flag : {"\"diagnosis_only\": true", "\"association_approved\": false", "\"associations_promoted\": 0",
             "\"ledger_mutated\": false", "\"corpus_acceptance_claimed\": false", "\"retail_behaviour_claimed\": false"})
        expect(contains(receipt, flag), "the receipt carries " + std::string(flag));
    expect(contains(receipt, "\"animation_count\": 10, \"playback_passed\": 6, \"failure_count\": 4"), "the baseline counts are copied");
    expect(contains(receipt, "\"failure_indices\": [1, 2, 3]"), "the selection lists its frozen indices");
    expect(contains(receipt, "\"original_failure\": {\"stage\": \"model_match\", \"code\": \"EAWR-ANIMATION-CORPUS-0001\", "
                             "\"cause\": \"no same-directory ALO basename is a prefix of the ALA basename\", "
                             "\"selected_model\": \"\", \"r0_candidates\": []}, \"original_failure_retained\": true"),
        "each row carries its original failure");
    expect(contains(receipt, "\"entries_sha256\": \"" + structural::inventory_sha256(inventory) + "\""), "the inventory digest is recorded");
    expect(contains(receipt, "\"exclusions\": [\n    " + structural::inventory_entry_json(*broken)), "the parse exclusion is listed");
    expect(contains(receipt, "\"rows_by_status\": {\"multiple_compatible_unapproved\": 1, \"single_compatible_unapproved\": 1, "
                             "\"zero_compatible\": 1}"), "row statuses are counted");
    expect(contains(receipt, "\"shortlisted_pairs\": 4, \"evaluated_pairs\": 4, \"compatible_unapproved_pairs\": 3"),
        "pair counts are recorded");
    expect(contains(receipt, "\"candidates_by_directory_relation\": {\"other_directory\": 2, \"same_directory\": 2}"),
        "directory relations are counted");
    expect(contains(receipt, "\"predicate\": \"model_hierarchy_parent_first\""), "rejection predicates are recorded");
    expect(contains(receipt, "\"label\": \"clamp_terminal\""), "sample evidence is recorded");
}

// A structural match is still evaluated in full: clip-level duration,
// interpolation, sample-count and non-finite defects reject at binding, and a
// pose that overflows rejects at sampling.
void test_structural_match_rejections() {
    TempTree tree;
    fixture(tree);
    const auto effective = mount(tree);
    if (!effective) return;
    const auto frozen = frozen_of(*effective);
    const auto inventory = structural::build_inventory(*effective);
    const auto index = structural::build_index(inventory);
    const auto r0_index = discovery::r0_index_of(inventory);
    const auto baseline = frozen.failures.at(1);
    auto bytes = effective->open(std::string(idle_path));
    auto parsed = assets::load_animation(bytes.value(), assets::source_from(effective->stat(std::string(idle_path)).value()));
    const bool usable = parsed && parsed.value().tracks.size() == 2 && parsed.value().tracks[1].samples.size() >= 2;
    expect(usable, "the idle clip parses with two tracks of several samples");
    if (!usable) return;

    const auto evaluate = [&](const std::function<void(assets::Animation&)>& change) {
        auto clip = parsed.value();
        change(clip);
        return discovery::evaluate_row(frozen, baseline, clip, baseline.animation, inventory, index, r0_index);
    };
    struct Case final { std::string_view label; std::function<void(assets::Animation&)> change; std::string_view predicate; };
    const std::vector<Case> cases{
        {"duration", [](assets::Animation& clip) { clip.duration_seconds += 5.0F; }, corpus::predicates::duration_consistent},
        {"interpolation", [](assets::Animation& clip) { clip.tracks[1].translation_interpolation = static_cast<assets::Interpolation>(9); },
            corpus::predicates::translation_interpolation},
        {"sample count", [](assets::Animation& clip) { clip.tracks[1].samples.pop_back(); }, corpus::predicates::sample_count},
        {"non-finite sample", [](assets::Animation& clip) { clip.tracks[1].samples[1].translation.x = std::numeric_limits<float>::infinity(); },
            corpus::predicates::samples_finite},
    };
    for (const Case& item : cases) {
        const auto result = evaluate(item.change);
        const std::string label(item.label);
        expect(result.row.has_value(), label + ": the row is evaluated: " + result.error);
        if (!result.row) continue;
        const auto& row = *result.row;
        expect(row.shortlisted == 3 && row.evaluated == 3 && row.compatible == 0 && row.rejected_binding == 3,
            label + ": every structural match is rejected at binding");
        expect(row.status == discovery::row_status::zero_compatible && row.original_failure_retained,
            label + ": the row is zero_compatible and keeps its failure");
        for (const auto& candidate : row.candidates) {
            if (candidate.result.identity.path == "data/art/models/hierarchy.alo") continue;
            expect(has_predicate(candidate.result, item.predicate) && candidate.result.consistent_with_player
                && candidate.result.samples.empty(), label + ": the rejecting predicate is recorded for " + candidate.result.identity.path);
        }
    }

    const auto overflow = evaluate([](assets::Animation& clip) {
        for (auto& sample : clip.tracks[0].samples) sample.scale = {1.0e30F, 1.0e30F, 1.0e30F};
        for (auto& sample : clip.tracks[1].samples) sample.translation = {1.0e30F, 1.0e30F, 1.0e30F};
    });
    expect(overflow.row.has_value(), "overflow: the row is evaluated: " + overflow.error);
    if (overflow.row) {
        expect(overflow.row->rejected_sampling == 2 && overflow.row->rejected_binding == 1 && overflow.row->compatible == 0,
            "a pose that overflows rejects both Player-accepted matches at sampling");
        for (const auto& candidate : overflow.row->candidates) {
            if (candidate.result.rejection_stage != "sampling") continue;
            expect(candidate.result.player_accepts && candidate.result.samples.size() == 4
                && std::any_of(candidate.result.samples.begin(), candidate.result.samples.end(), [](const auto& s) { return !s.finite; }),
                "sampling rejection keeps its four sample checks");
        }
    }
}

// R0 is re-run over every effective model path.  A new same-directory prefix
// model is baseline drift even when it fails to parse; a prefix model in
// another directory is not.
void test_r0_drift() {
    const auto outcome = [](const std::function<void(const TempTree&)>& change) {
        TempTree tree;
        fixture(tree);
        change(tree);
        const auto effective = mount(tree);
        if (!effective) return std::string("mount failed");
        const auto frozen = frozen_of(*effective);
        const Run result = run(*effective, frozen);
        return result.diagnosis.error.empty() ? std::string() : result.diagnosis.error;
    };
    const std::string unparsable = outcome([](const TempTree& tree) {
        write(tree.models("mod") / "RI_Padowan.ALO", Bytes{std::byte{7}});
    });
    expect(contains(unparsable, "baseline drift: R0 now names data/art/models/ri_padowan.alo"),
        "a new unparsable R0 prefix model is baseline drift: " + unparsable);
    const std::string parsed = outcome([](const TempTree& tree) {
        write(tree.models("expansion") / "Underworld_Test.ALO", alo({"root", "absent_bone"}));
    });
    expect(contains(parsed, "baseline drift: R0 now names data/art/models/underworld_test.alo"),
        "a new parsed R0 prefix model in another layer is baseline drift: " + parsed);
    const std::string elsewhere = outcome([](const TempTree& tree) {
        write(tree.data("mod") / "Art" / "Other" / "RI_Padowan.ALO", Bytes{std::byte{7}});
    });
    expect(elsewhere.empty(), "a prefix model in another directory is not R0: " + elsewhere);

    // R0 is reproduced first: with both an R0 prefix model and a stale ALA
    // hash, the refusal is the R0 drift, before the clip is even read.
    TempTree tree;
    fixture(tree);
    write(tree.models("mod") / "RI_Padowan.ALO", Bytes{std::byte{7}});
    const auto effective = mount(tree);
    if (!effective) return;
    auto stale = frozen_of(*effective);
    stale.failures.at(1).animation.sha256 = std::string(64, 'd');
    const std::string first = run(*effective, stale).diagnosis.error;
    expect(contains(first, "R0 now names") && !contains(first, "hash drift"), "R0 is reproduced before the ALA is checked: " + first);
}

// The complete frozen ALA identity is checked before the clip is parsed.
void test_ala_drift() {
    TempTree tree;
    fixture(tree);
    auto effective = mount(tree);
    if (!effective) return;
    const auto frozen = frozen_of(*effective);
    const auto both = [&effective](const audit::FrozenMetadata& reference, const std::function<void(audit::AssetIdentity&)>& change) {
        auto metadata = reference;
        change(metadata.failures.at(1).animation);
        return run(*effective, metadata, selected_of(metadata)).diagnosis.error;
    };
    expect(contains(both(frozen, [](auto& id) { id.sha256 = std::string(64, 'b'); }), "hash drift"), "a stale ALA hash refuses");
    for (const auto& [field, change] : std::vector<std::pair<std::string, std::function<void(audit::AssetIdentity&)>>>{
             {"layer_id", [](auto& id) { id.layer_id = "base"; }},
             {"origin", [](auto& id) { id.origin = "archive"; }},
             {"source_id", [](auto& id) { id.source_id = "base:Data/Models.meg"; }},
             {"original_path", [](auto& id) { id.original_path = "DATA\\ART\\MODELS\\RI_PADOWAN_IDLE_00.ALA"; }},
             {"size", [](auto& id) { id.size += 1; }}}) {
        const std::string error = both(frozen, change);
        expect(contains(error, "provenance drift: effective ALA " + field), "ALA " + field + " drift refuses: " + error);
    }
    // Rewritten bytes that would not even parse are refused as hash drift,
    // proving the identity is checked before parsing.
    write(tree.models("mod") / "RI_Padowan_Idle_00.ALA", Bytes{std::byte{0x55}, std::byte{0x66}, std::byte{0x77}});
    effective = mount(tree);
    if (!effective) return;
    const std::string rewritten = run(*effective, frozen).diagnosis.error;
    expect(contains(rewritten, "hash drift") && !contains(rewritten, "parses"), "rewritten ALA bytes are hash drift: " + rewritten);
}

// Frozen-row, candidate-identity and consistency refusals.
void test_row_refusals() {
    TempTree tree;
    fixture(tree);
    const auto effective = mount(tree);
    if (!effective) return;
    const auto frozen = frozen_of(*effective);
    const auto rows = selected_of(frozen);
    const auto error = [&effective](const audit::FrozenMetadata& metadata, std::vector<audit::BaselineFailure> selected) {
        return run(*effective, metadata, std::move(selected)).diagnosis.error;
    };

    auto rebaselined = rows; rebaselined.front().cause = "another cause";
    expect(contains(error(frozen, rebaselined), "baseline drift: cause"), "a row that differs from frozen refuses");
    auto twice = rows; twice.push_back(rows.front());
    expect(contains(error(frozen, twice), "selected twice"), "a row selected twice refuses");
    auto with_r0 = frozen; with_r0.failures.at(2).r0_candidates = {"data/art/models/underworld.alo"};
    auto with_r0_rows = rows; with_r0_rows[1].r0_candidates = with_r0.failures.at(2).r0_candidates;
    expect(!error(with_r0, with_r0_rows).empty(), "a frozen row with R0 candidates refuses");
    auto unknown = rows; unknown.front().baseline_failure_index = 99;
    expect(contains(error(frozen, unknown), "not in the frozen metadata"), "a row outside the frozen metadata refuses");

    auto drifted = frozen; drifted.models.at(std::string(gun_path)).identity.sha256 = std::string(64, 'c');
    expect(contains(error(drifted, rows), "discovered model data/art/models/gun.alo sha256"),
        "a recorded discovered model whose identity drifted refuses");
    auto bones = frozen; bones.models.at(std::string(gun_path)).bone_count = 99;
    expect(contains(error(bones, rows), "bone_count"), "a recorded discovered model whose bone count drifted refuses");

    const auto inventory = structural::build_inventory(*effective);
    auto tampered = structural::build_index(inventory);
    auto& arm = tampered.postings.at({1U, "arm"});
    expect(arm.size() > 1, "several models hold bone 1 'arm'");
    arm.pop_back();
    const auto tampered_run = discovery::diagnose(frozen, rows, *effective, inventory, tampered);
    expect(contains(tampered_run.error, "brute-force"), "an indexed shortlist that differs from brute force refuses: " + tampered_run.error);

    structural::Inventory broken;
    broken.error = "effective enumeration record is not the stat winner";
    expect(!discovery::diagnose(frozen, rows, *effective, broken, structural::build_index(broken)).error.empty(),
        "an inconsistent inventory refuses");
}

// Parsed, parse-failed and read-failed models are all accounted for; the
// unparsed ones are visible exclusions and still take part in R0.
void test_inventory_accounting() {
    structural::Inventory inventory;
    const auto record = [](const std::string& path, const std::uint64_t size) {
        return vfs::AssetRecord{path, path, "mod", vfs::AssetOrigin::loose, size, "mod:loose:" + path};
    };
    const Bytes parsed_a = alo({"root", "arm"});
    const Bytes parsed_b = alo({"root"});
    const Bytes broken = Bytes{std::byte{3}};
    inventory.models.push_back(structural::make_inventory_model(record("data/a/a.alo", parsed_a.size()), &parsed_a));
    inventory.models.push_back(structural::make_inventory_model(record("data/a/b.alo", parsed_b.size()), &parsed_b));
    inventory.models.push_back(structural::make_inventory_model(record("data/a/c.alo", broken.size()), &broken));
    inventory.models.push_back(structural::make_inventory_model(record("data/a/d.alo", 9), nullptr, "EAWR-VFS-0003", "cannot open asset source"));
    inventory.raw_versions = 5;
    inventory.shadowed_versions_excluded = 1;
    const auto summary = discovery::summarize({}, inventory);
    expect(summary.inventory_by_status.at("parsed") == 2 && summary.inventory_by_status.at("parse_failed") == 1
        && summary.inventory_by_status.at("read_failed") == 1, "every inventory status is counted");
    const auto parse_view = structural::view_of(inventory.models[2]);
    const auto read_view = structural::view_of(inventory.models[3]);
    expect(parse_view.found && !parse_view.model && read_view.found == false && read_view.identity.sha256.empty(),
        "a parse failure is found without a model; a read failure is not found and has no hash");
    const auto r0 = discovery::reproduce_r0("data/a/c_idle_00.ala", inventory, discovery::r0_index_of(inventory));
    expect(r0.same_directory_models == 4 && r0.same_directory_unparsed == 2
        && r0.qualifying == std::vector<std::string>{"data/a/c.alo"}, "unparsed models take part in R0");
    std::ostringstream output;
    discovery::write_receipt(output, {}, inventory, {});
    const std::string receipt = output.str();
    expect(contains(receipt, "\"by_status\": {\"parse_failed\": 1, \"parsed\": 2, \"read_failed\": 1}, \"raw_versions\": 5, "
                             "\"shadowed_versions_excluded\": 1"), "the receipt counts the inventory");
    expect(contains(receipt, "\"exclusions\": [\n    " + structural::inventory_entry_json(inventory.models[2]) + ",\n    "
                                 + structural::inventory_entry_json(inventory.models[3]) + "\n  ]"),
        "both unparsed models are listed as exclusions, in path order");
}

// Input, row and candidate order do not change the receipt.
void test_order_invariant() {
    TempTree tree;
    fixture(tree);
    const auto effective = mount(tree);
    if (!effective) return;
    const auto frozen = frozen_of(*effective);
    const Run reference = run(*effective, frozen);
    expect(reference.diagnosis.error.empty() && !reference.receipt.empty(), "reference receipt written");
    auto selected = selected_of(frozen);
    std::mt19937 random(20260923U);
    for (int round = 0; round < 8; ++round) {
        std::shuffle(selected.begin(), selected.end(), random);
        const Run shuffled = run(*effective, frozen, selected);
        expect(shuffled.receipt == reference.receipt, "shuffled input order gives a byte-identical receipt");
        auto rows = shuffled.diagnosis.rows;
        std::shuffle(rows.begin(), rows.end(), random);
        for (auto& row : rows) std::shuffle(row.candidates.begin(), row.candidates.end(), random);
        expect(receipt_of(frozen, shuffled.inventory, rows) == reference.receipt, "shuffled rows and candidates give a byte-identical receipt");
    }
    for (const char byte : reference.receipt)
        expect(static_cast<unsigned char>(byte) >= 0x20U || byte == '\n', "the receipt holds no raw control byte");
}

} // namespace

int main() {
    test_tracked_selection();
    test_discovery_rules();
    test_structural_match_rejections();
    test_r0_drift();
    test_ala_drift();
    test_row_refusals();
    test_inventory_accounting();
    test_order_invariant();
    if (failures != 0) {
        std::cerr << failures << " model-match discovery check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "animation model-match discovery checks passed\n";
    return EXIT_SUCCESS;
}
