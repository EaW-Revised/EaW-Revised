#include "corpus_structural_discovery.hpp"

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
namespace discovery = eawr::tests::animation_corpus::structural_discovery;

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

const std::vector<std::string> selected_bones{"root", "backpack", "hand"};
const std::vector<std::pair<std::uint32_t, std::string>> idle_tracks{{0, "root"}, {1, "arm"}};
const std::vector<std::pair<std::uint32_t, std::string>> move_tracks{{0, "root"}, {1, "absent_bone"}};
const std::vector<std::pair<std::uint32_t, std::string>> heavy_tracks{{0, "root"}, {1, "gun"}};

constexpr std::string_view model_path = "data/art/models/ei_shocktrooper.alo";
constexpr std::string_view heavy_path = "data/art/models/ei_shocktrooper_heavy.alo";
constexpr std::array<sources::TargetModel, 2> fixture_targets{{{model_path, 2}, {heavy_path, 1}}};

const std::vector<std::string> fixture_alas{
    "data/art/models/ei_shocktrooper_idle_00.ala",
    "data/art/models/ei_shocktrooper_move_00.ala",
    "data/art/models/ei_shocktrooper_heavy_idle_00.ala",
};

struct TempTree final {
    std::filesystem::path root;
    TempTree() {
        root = std::filesystem::temp_directory_path()
            / ("eawr-structural-discovery-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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

// The discovery fixture.  Model names are chosen so that no name hints at the
// result: an unrelated name matches, a similar name does not.
void fixture(const TempTree& tree) {
    for (const std::string layer : {"mod", "expansion", "base"}) write(tree.data(layer) / "MegaFiles.xml", manifest({"Data/Models.meg"}));
    const auto models = tree.models("mod");
    write(models / "EI_Shocktrooper.ALO", alo(selected_bones));
    write(models / "EI_Shocktrooper_Heavy.ALO", alo(selected_bones));
    write(models / "EI_Shocktrooper_idle_00.ala", ala(idle_tracks));
    write(models / "EI_Shocktrooper_move_00.ala", ala(move_tracks));
    write(models / "EI_Shocktrooper_Heavy_idle_00.ala", ala(heavy_tracks));
    write(models / "ZZ_Unrelated.ALO", alo({"root", "arm"}));                      // unrelated name, exact match
    write(models / "EI_Shocktrooper_B.ALO", alo({"root", "arms"}));                // similar name, mismatch
    write(models / "Case.ALO", alo({"root", "Arm"}));                              // case differs
    write(models / "Shift.ALO", alo({"arm", "root"}));                             // names at other indices
    write(models / "Short.ALO", alo({"root"}));                                    // index out of range
    write(tree.data("mod") / "Art" / "Other" / "Extra.ALO", alo({"root", "arm", "hand", "extra"}));  // extra bones
    write(models / "Dup_A.ALO", alo({"root", "arm", "dup"}));                      // identical bytes,
    write(models / "Dup_B.ALO", alo({"root", "arm", "dup"}));                      // distinct paths
    write(models / "Hierarchy.ALO", alo_with_parents({{"root", 1}, {"arm", -1}})); // parses, Player rejects
    write(models / "Broken.ALO", Bytes{std::byte{1}, std::byte{2}});               // parser exclusion
    write(models / "Gun.ALO", alo({"root", "gun", "x"}));                          // the only heavy match
    write(models / "ZZ_Shadowed.ALO", alo({"root", "nope"}));
    // Compatible, but shadowed by the mod's loose winners: never candidates.
    write(tree.data("base") / "Models.meg", meg({{"DATA\\ART\\MODELS\\ZZ_SHADOWED.ALO", alo({"root", "arm"})},
        {"DATA\\ART\\MODELS\\EI_SHOCKTROOPER.ALO", alo({"root", "arm"})}}));
}

// Frozen metadata recording exactly the current effective state.
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
        auto model_bytes = effective.open(failure.selected_model);
        auto parsed = assets::load_model(model_bytes.value(), assets::source_from(effective.stat(failure.selected_model).value()));
        expect(clip && parsed, "fixture clip and selected model parse");
        if (clip && parsed) {
            auto player = presentation::animation::Player::create(parsed.value(), &clip.value());
            expect(!player, "fixture selected model fails binding");
            if (!player) { failure.code = player.error().code; failure.cause = player.error().message; }
        }
        frozen.failures.emplace(failure.baseline_failure_index, std::move(failure));
    }
    return frozen;
}

struct Run final {
    discovery::Inventory inventory;
    discovery::Diagnosis diagnosis;
    std::string receipt;
};

std::string receipt_of(const audit::FrozenMetadata& frozen, const discovery::Inventory& inventory, std::vector<discovery::Row> rows) {
    std::ostringstream output;
    discovery::Header header;
    header.animation_count = frozen.animation_count;
    header.playback_passed = frozen.playback_passed;
    header.failure_count = frozen.failure_count;
    discovery::write_receipt(output, header, inventory, std::move(rows));
    return output.str();
}

Run run(const vfs::Vfs& effective, const audit::FrozenMetadata& frozen, std::vector<audit::BaselineFailure> selected = {}) {
    if (selected.empty()) {
        auto selection = sources::select_rows(frozen, fixture_targets);
        expect(selection.error.empty(), "fixture selection: " + selection.error);
        selected = std::move(selection.rows);
    }
    Run result;
    result.inventory = discovery::build_inventory(effective);
    const auto index = discovery::build_index(result.inventory);
    result.diagnosis = discovery::diagnose(frozen, selected, effective, result.inventory, index);
    if (result.diagnosis.error.empty()) result.receipt = receipt_of(frozen, result.inventory, result.diagnosis.rows);
    return result;
}

const discovery::Row* row_for(const discovery::Diagnosis& diagnosis, const std::string& ala_path) {
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

std::size_t count(const std::string& haystack, const std::string_view needle) {
    std::size_t result = 0;
    for (auto at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) ++result;
    return result;
}

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
    auto selection = sources::select_rows(*load.metadata);
    expect(selection.error.empty() && selection.rows.size() == 132, "tracked metadata selects exactly 132 rows");
    std::map<std::string, std::size_t> by_model;
    for (const auto& row : selection.rows) {
        ++by_model[row.selected_model];
        expect(row.stage == "binding" && row.code == "EAWR-ANIMATION-0002", "every selected row is a frozen binding failure");
    }
    expect(by_model[std::string(model_path)] == 70 && by_model[std::string(heavy_path)] == 62, "70 ordinary and 62 heavy rows");
    expect(load.metadata->playback_passed == 7329 && load.metadata->failure_count == 356,
        "the frozen ledger counts stay 7329 passed / 356 retained");
    auto tampered = bytes;
    tampered.back() = ' ';
    const auto tampered_digest = sources::sha256_of(std::as_bytes(std::span<const char>(tampered.data(), tampered.size())));
    expect(!audit::load_frozen_metadata(tampered, tampered_digest).metadata, "a stale frozen manifest refuses");
#else
    expect(false, "EAWR_ASSOCIATION_FROZEN_MANIFEST is not defined");
#endif
}

// Name-blind exact discovery: unrelated names match, similar names, case,
// index and range mismatches do not, extra bones are allowed, identical bytes
// at two paths stay two candidates, a structural candidate that strict Player
// rejects is rejected, shadowed versions are never searched and parser
// failures are visible.
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
    expect(inventory.models.size() == 14, "fourteen effective models, got " + std::to_string(inventory.models.size()));
    expect(inventory.raw_versions == 16 && inventory.shadowed_versions_excluded == 2, "two shadowed versions are excluded");
    const auto* broken = inventory.find("data/art/models/broken.alo");
    expect(broken && broken->status == discovery::model_status::parse_failed && !broken->error_code.empty() && !broken->skeleton,
        "the malformed model is a visible parse exclusion with its parser code");
    const auto* shadowed = inventory.find("data/art/models/zz_shadowed.alo");
    expect(shadowed && shadowed->identity.source_id == "mod:loose:data/Art/Models/ZZ_Shadowed.ALO",
        "only the effective winner of a shadowed path is inventoried");

    const discovery::Row* idle = row_for(result.diagnosis, fixture_alas[0]);
    expect(idle != nullptr, "the idle row is present");
    if (idle) {
        const std::vector<std::string> expected{"data/art/models/dup_a.alo", "data/art/models/dup_b.alo",
            "data/art/models/hierarchy.alo", "data/art/models/zz_unrelated.alo", "data/art/other/extra.alo"};
        expect(candidate_paths(*idle) == expected, "exactly the exact-skeleton models are shortlisted");
        expect(idle->shortlisted == 5 && idle->evaluated == 5 && idle->compatible == 4 && idle->rejected_binding == 1,
            "five shortlisted and evaluated, four compatible, one rejected at binding");
        expect(idle->status == discovery::row_status::multiple_compatible, "several compatible models are multiple_compatible_unapproved");
        expect(idle->effective_failure_retained && idle->effective.status == audit::candidate_status::rejected
            && idle->effective.rejection_stage == "binding" && idle->effective.player_code == idle->baseline.code
            && idle->effective.player_message == idle->baseline.cause, "the original failure is retained with its code and cause");
        const auto* unrelated = candidate_for(*idle, "data/art/models/zz_unrelated.alo");
        expect(unrelated && unrelated->result.status == audit::candidate_status::compatible_unapproved
            && unrelated->result.samples.size() == 4 && !unrelated->r0_candidate && !unrelated->selected_model
            && unrelated->result.frozen_identity == discovery::not_in_frozen_metadata,
            "an unrelated name that matches exactly is compatible but unapproved, outside R0, with four samples");
        const auto* extra = candidate_for(*idle, "data/art/other/extra.alo");
        expect(extra && extra->result.status == audit::candidate_status::compatible_unapproved
            && extra->result.bone_count == std::optional<std::size_t>(4), "extra untracked bones are allowed, in any directory");
        const auto* dup_a = candidate_for(*idle, "data/art/models/dup_a.alo");
        const auto* dup_b = candidate_for(*idle, "data/art/models/dup_b.alo");
        expect(dup_a && dup_b && dup_a->result.identity.sha256 == dup_b->result.identity.sha256
            && dup_a->result.identity.source_id != dup_b->result.identity.source_id
            && dup_a->result.status == audit::candidate_status::compatible_unapproved
            && dup_b->result.status == audit::candidate_status::compatible_unapproved,
            "identical bytes at two paths stay two distinct compatible_unapproved candidates");
        const auto* hierarchy = candidate_for(*idle, "data/art/models/hierarchy.alo");
        expect(hierarchy && hierarchy->result.status == audit::candidate_status::rejected
            && hierarchy->result.rejection_stage == "binding" && !hierarchy->result.player_accepts
            && hierarchy->result.consistent_with_player && hierarchy->result.samples.empty(),
            "a shortlisted model that strict Player rejects is rejected");
        for (const std::string& rejected : std::vector<std::string>{"data/art/models/ei_shocktrooper_b.alo", "data/art/models/case.alo",
                 "data/art/models/shift.alo", "data/art/models/short.alo", "data/art/models/zz_shadowed.alo",
                 "data/art/models/broken.alo", std::string(model_path)})
            expect(!candidate_for(*idle, rejected), "not shortlisted: " + rejected);
    }
    const discovery::Row* move = row_for(result.diagnosis, fixture_alas[1]);
    expect(move && move->status == discovery::row_status::zero_compatible && move->shortlisted == 0 && move->candidates.empty()
        && move->effective_failure_retained, "a clip no model satisfies is zero_compatible and keeps its failure");
    const discovery::Row* heavy = row_for(result.diagnosis, fixture_alas[2]);
    expect(heavy && heavy->status == discovery::row_status::single_compatible && candidate_paths(*heavy)
        == std::vector<std::string>{"data/art/models/gun.alo"}, "one compatible model is single_compatible_unapproved");

    const std::string& receipt = result.receipt;
    expect(receipt.find("\"approved\": true") == std::string::npos, "nothing is approved");
    expect(count(receipt, "\"approved\": false") == 3 + 3 + 6, "every row, effective model and candidate is unapproved");
    expect(receipt.find("\"association_approved\": false") != std::string::npos
        && receipt.find("\"ledger_mutated\": false") != std::string::npos, "the receipt disclaims approval and ledger mutation");
    expect(receipt.find("\"playback_passed\": 7,") != std::string::npos, "the baseline pass count is copied");
    expect(receipt.find("\"entries_sha256\": \"" + discovery::inventory_sha256(inventory) + "\"") != std::string::npos,
        "the receipt carries the inventory digest");
    expect(receipt.find("\"shadowed_versions_excluded\": 2") != std::string::npos, "the receipt counts shadowed exclusions");
    expect(receipt.find("\"exclusions\": [\n    " + discovery::inventory_entry_json(*broken)) != std::string::npos,
        "the parse exclusion is listed with its identity and error");
    expect(receipt.find("\"rows_by_status\": {\"multiple_compatible_unapproved\": 1, \"single_compatible_unapproved\": 1, "
                        "\"zero_compatible\": 1}") != std::string::npos, "row statuses are counted");
    expect(receipt.find("\"shortlisted_pairs\": 6, \"evaluated_pairs\": 6, \"compatible_unapproved_pairs\": 5") != std::string::npos,
        "pair counts are recorded");
}

// The indexed intersection equals a brute-force scan on random inventories.
void test_index_equals_brute_force() {
    std::mt19937 random(24U);
    const std::array<std::string, 6> names{"a", "A", "b", "root", "Root", "c"};
    discovery::Inventory inventory;
    for (int model = 0; model < 200; ++model) {
        std::vector<std::string> bones;
        const auto size = random() % 6U;
        for (std::uint32_t bone = 0; bone < size; ++bone) bones.push_back(names[random() % names.size()]);
        vfs::AssetRecord record{"data/m/" + std::to_string(1000 + model) + ".alo", "M.alo", "mod", vfs::AssetOrigin::loose, 0,
            "mod:loose:M" + std::to_string(model)};
        const Bytes bytes = model % 37 == 0 ? Bytes{std::byte{9}} : alo(bones);
        record.size = bytes.size();
        inventory.models.push_back(discovery::make_inventory_model(record, &bytes));
    }
    vfs::AssetRecord unread{"data/m/9999.alo", "U.alo", "mod", vfs::AssetOrigin::loose, 4, "mod:loose:U"};
    inventory.models.push_back(discovery::make_inventory_model(unread, nullptr, "EAWR-VFS-0003", "cannot open asset source"));
    expect(inventory.models.back().status == discovery::model_status::read_failed && inventory.models.back().identity.sha256.empty(),
        "an unreadable model is a read_failed exclusion");
    const auto index = discovery::build_index(inventory);
    std::size_t nonempty = 0;
    for (int round = 0; round < 600; ++round) {
        // The ALA parser refuses duplicate track bone indices.
        std::array<std::uint32_t, 6> indices{0, 1, 2, 3, 4, 5};
        std::shuffle(indices.begin(), indices.end(), random);
        std::vector<std::pair<std::uint32_t, std::string>> tracks;
        const auto size = random() % 4U;
        for (std::uint32_t track = 0; track < size; ++track) tracks.emplace_back(indices[track], names[random() % names.size()]);
        auto clip = assets::load_animation(ala(tracks), {"c.ala", "fixture", "mod", vfs::AssetOrigin::loose, 0});
        expect(static_cast<bool>(clip), "random clip parses");
        if (!clip) continue;
        const auto indexed = discovery::shortlist(index, clip.value());
        expect(indexed == discovery::brute_force_shortlist(inventory, clip.value()), "indexed shortlist equals brute force");
        if (!indexed.empty() && !tracks.empty()) ++nonempty;
    }
    expect(nonempty > 20, "the random rounds exercise non-empty shortlists");
}

// The skeleton kept for evaluation gives exactly the full model's result.
void test_skeleton_equivalence() {
    const auto clip = assets::load_animation(ala(idle_tracks), {"c.ala", "fixture", "mod", vfs::AssetOrigin::loose, 0});
    for (const Bytes& bytes : {alo({"root", "arm", "hand"}), alo(selected_bones), alo_with_parents({{"root", 1}, {"arm", -1}})}) {
        auto full = assets::load_model(bytes, {"m.alo", "fixture", "mod", vfs::AssetOrigin::loose, 0});
        auto again = assets::load_model(bytes, {"m.alo", "fixture", "mod", vfs::AssetOrigin::loose, 0});
        expect(clip && full && again, "equivalence fixtures parse");
        if (!clip || !full || !again) continue;
        const assets::Model skeleton = discovery::skeleton_of(std::move(again.value()));
        audit::CandidateModelView full_view; full_view.found = true; full_view.model = &full.value();
        audit::CandidateModelView skeleton_view; skeleton_view.found = true; skeleton_view.model = &skeleton;
        std::ostringstream left;
        std::ostringstream right;
        audit::write_candidate(left, audit::evaluate_candidate(clip.value(), full_view));
        audit::write_candidate(right, audit::evaluate_candidate(clip.value(), skeleton_view));
        expect(left.str() == right.str(), "skeleton and full model evaluate identically");
    }
}

// Stale baselines, identities and failures refuse.
void test_refusals() {
    TempTree tree;
    fixture(tree);
    const auto effective = mount(tree);
    if (!effective) return;
    const auto frozen = frozen_of(*effective);
    const auto rows = sources::select_rows(frozen, fixture_targets).rows;
    const auto refused = [&effective](const audit::FrozenMetadata& metadata, std::vector<audit::BaselineFailure> selected) {
        return !run(*effective, metadata, std::move(selected)).diagnosis.error.empty();
    };

    auto rebaselined = rows; rebaselined.front().cause = "another cause";
    expect(refused(frozen, rebaselined), "a baseline that differs from frozen refuses");
    auto stale_model = frozen; stale_model.models.at(std::string(model_path)).identity.sha256 = std::string(64, 'a');
    expect(refused(stale_model, rows), "a stale selected-model hash refuses");
    auto bones = frozen; bones.models.at(std::string(model_path)).bone_count = 99;
    expect(refused(bones, rows), "a stale selected-model bone count refuses");
    auto stale_clip = frozen; stale_clip.failures.at(1).animation.sha256 = std::string(64, 'b');
    auto stale_rows = rows; stale_rows.front().animation.sha256 = std::string(64, 'b');
    expect(refused(stale_clip, stale_rows), "a stale ALA hash refuses");
    auto relayered = frozen; relayered.failures.at(1).animation.layer_id = "base";
    auto relayered_rows = rows; relayered_rows.front().animation.layer_id = "base";
    expect(refused(relayered, relayered_rows), "an ALA identity from another layer refuses");
    auto now_binds = frozen;
    for (auto& [index, failure] : now_binds.failures) failure.code = "EAWR-ANIMATION-0001";
    expect(refused(now_binds, sources::select_rows(now_binds, fixture_targets).rows), "a failure that no longer reproduces refuses");
    auto twice = rows; twice.push_back(rows.front());
    expect(refused(frozen, twice), "a row selected twice refuses");

    // A discovered model the frozen metadata records must still match it.
    auto recorded = frozen;
    auto record = effective->stat("data/art/models/zz_unrelated.alo");
    auto bytes = effective->open("data/art/models/zz_unrelated.alo");
    recorded.models["data/art/models/zz_unrelated.alo"] = {sources::identity_of(record.value(), sources::sha256_of(bytes.value())), 2};
    const Run matched = run(*effective, recorded);
    const discovery::Row* idle = matched.diagnosis.error.empty() ? row_for(matched.diagnosis, fixture_alas[0]) : nullptr;
    const auto* unrelated = idle ? candidate_for(*idle, "data/art/models/zz_unrelated.alo") : nullptr;
    expect(unrelated && unrelated->result.frozen_identity == audit::frozen_identity::matched,
        "a recorded discovered model is frozen-matched");
    auto drifted = recorded; drifted.models.at("data/art/models/zz_unrelated.alo").identity.sha256 = std::string(64, 'c');
    expect(refused(drifted, rows), "a recorded discovered model whose identity drifted refuses");

    // An index that disagrees with the brute-force scan refuses rather than
    // silently dropping a candidate.
    const auto inventory = discovery::build_inventory(*effective);
    auto tampered = discovery::build_index(inventory);
    auto& arm = tampered.postings.at({1U, "arm"});
    expect(arm.size() > 1, "several models hold bone 1 'arm'");
    arm.pop_back();
    const auto tampered_run = discovery::diagnose(frozen, rows, *effective, inventory, tampered);
    expect(tampered_run.error.find("brute-force") != std::string::npos,
        "an indexed shortlist that differs from brute force refuses: " + tampered_run.error);

    discovery::Inventory broken;
    broken.error = "effective enumeration record is not the stat winner";
    expect(!discovery::diagnose(frozen, rows, *effective, broken, discovery::build_index(broken)).error.empty(),
        "an inconsistent inventory refuses");
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
    auto selected = sources::select_rows(frozen, fixture_targets).rows;
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
    test_index_equals_brute_force();
    test_skeleton_equivalence();
    test_refusals();
    test_order_invariant();
    if (failures != 0) {
        std::cerr << failures << " structural-discovery check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "animation structural-discovery checks passed\n";
    return EXIT_SUCCESS;
}
