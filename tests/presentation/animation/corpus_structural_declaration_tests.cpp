// Synthetic contracts of the structural-lead declaration probe
// (corpus_structural_declaration.hpp).  No private asset is used: receipts are
// hand-written in the R2 receipt's line format, and every catalog is written to
// a temporary tree and loaded through the real VFS and XML catalog.

#include "corpus_structural_declaration.hpp"

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
#include <functional>
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
namespace declarations = corpus::declarations;
namespace lead_probe = corpus::structural_declaration;

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

[[nodiscard]] std::size_t count(const std::string_view text, const std::string_view needle) {
    std::size_t result{};
    for (auto at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) ++result;
    return result;
}

constexpr std::string_view clip_path = "data/art/models/ei_shocktrooper_attachflinchf_01.ala";
constexpr std::string_view selected_path = "data/art/models/ei_shocktrooper.alo";
constexpr std::string_view candidate_path = "data/art/models/go/ei_shocktrooper.alo";
constexpr std::string_view cause = "animation track does not map exactly to model bone name/index";

[[nodiscard]] audit::AssetIdentity clip_identity() {
    return {std::string(clip_path), std::string(64, 'a'), "mod", "loose",
        "mod:loose:data/Art/Models/EI_ShockTrooper_ATTACHFLINCHF_01.ala",
        "data/Art/Models/EI_ShockTrooper_ATTACHFLINCHF_01.ala", 3166};
}

[[nodiscard]] audit::AssetIdentity selected_identity() {
    return {std::string(selected_path), std::string(64, 'b'), "mod", "loose", "mod:loose:data/Art/Models/EI_Shocktrooper.ALO",
        "data/Art/Models/EI_Shocktrooper.ALO", 998390};
}

// A frozen manifest holding failure 79 as R2 found it, plus an unrelated failure.
[[nodiscard]] audit::FrozenMetadata synthetic_frozen() {
    audit::FrozenMetadata frozen;
    frozen.sha256 = std::string(64, 'f');
    frozen.animation_count = 7685;
    frozen.playback_passed = 7329;
    frozen.failure_count = 356;
    frozen.failures[79] = audit::BaselineFailure{clip_identity(), 79, "binding", "EAWR-ANIMATION-0002", std::string(cause),
        std::string(selected_path), {std::string(selected_path)}};
    frozen.failures[80] = audit::BaselineFailure{
        {"data/art/models/ei_shocktrooper_die_00.ala", std::string(64, 'd'), "mod", "loose", "mod:loose:x", "x", 1}, 80,
        "binding", "EAWR-ANIMATION-0002", std::string(cause), std::string(selected_path), {std::string(selected_path)}};
    frozen.models[std::string(selected_path)] = audit::FrozenModel{selected_identity(), 20};
    return frozen;
}

// The pinned lead, re-pinned to a synthetic receipt hash.
[[nodiscard]] lead_probe::LeadSpec spec(const std::string& receipt_sha = std::string(64, 'e')) {
    auto result = lead_probe::pinned_lead();
    result.receipt_sha256 = receipt_sha;
    return result;
}

[[nodiscard]] lead_probe::Lead synthetic_lead(const std::string& receipt_sha = std::string(64, 'e')) {
    auto load = lead_probe::lead_from_frozen(synthetic_frozen(), spec(receipt_sha));
    if (!load.lead) {
        expect(false, "synthetic lead derives: " + load.error);
        return {};
    }
    return *load.lead;
}

// ---------------------------------------------------------------------------
// A receipt in the R2 line format, written out literally.
// ---------------------------------------------------------------------------

struct ReceiptParts {
    std::string frozen_sha = std::string(64, 'f');
    std::string approved_header = "false";
    std::string rows_by_status = "{\"multiple_compatible_unapproved\": 0, \"single_compatible_unapproved\": 1, \"zero_compatible\": 1}";
    std::string compatible_pairs = "1";
    std::string failure_index = "79";
    std::string effective_model_sha = std::string(64, 'b');
    std::string candidate_sha = "a01bc8c278fb23dbccbe2ae186050b36f90038b0b60ae96e4e42859c8e33a35a";
    std::string candidate_source = "mod:loose:data/Art/Models/GO/EI_ShockTrooper.ALO";
    std::string candidate_status = "compatible_unapproved";
    std::string candidate_approved = "false";
    std::string frozen_identity = "not_in_frozen_metadata";
    std::string r0_candidate = "false";
    std::string retained = "true";
    std::string other_row_approved = "false";
    bool second_candidate{};
    bool second_lead_row{};
};

[[nodiscard]] std::string candidate_json(const ReceiptParts& parts, const std::string& path) {
    return "{\"selected_model\": false, \"r0_candidate\": " + parts.r0_candidate
        + ", \"bytes_identical_to_selected\": false, \"result\": {\"path\": \"" + path + "\", \"sha256\": \""
        + parts.candidate_sha + "\", \"layer_id\": \"mod\", \"origin\": \"loose\", \"source_id\": \"" + parts.candidate_source
        + "\", \"original_path\": \"data/Art/Models/GO/EI_ShockTrooper.ALO\", \"size\": 2809575, \"found\": true, "
          "\"status\": \"" + parts.candidate_status + "\", \"approved\": " + parts.candidate_approved
        + ", \"error_code\": \"\", \"error_message\": \"\", \"bone_count\": 27, \"diagnosis\": {\"signature\": \"none\", "
          "\"failures\": []}, \"samples\": [], \"sampled_pose_motion\": \"moving\", \"frozen_identity\": \""
        + parts.frozen_identity + "\"}}";
}

[[nodiscard]] std::string lead_row(const ReceiptParts& parts) {
    std::string candidates = candidate_json(parts, std::string(candidate_path));
    if (parts.second_candidate) candidates += ", " + candidate_json(parts, "data/art/models/zz/ei_shocktrooper.alo");
    return "    {\"animation\": {\"path\": \"data/art/models/ei_shocktrooper_attachflinchf_01.ala\", \"sha256\": \""
        + std::string(64, 'a') + "\", \"layer_id\": \"mod\", \"origin\": \"loose\", \"source_id\": "
          "\"mod:loose:data/Art/Models/EI_ShockTrooper_ATTACHFLINCHF_01.ala\", \"original_path\": "
          "\"data/Art/Models/EI_ShockTrooper_ATTACHFLINCHF_01.ala\", \"size\": 3166}, \"baseline_failure_index\": "
        + parts.failure_index + ", \"baseline_stage\": \"binding\", \"baseline_code\": \"EAWR-ANIMATION-0002\", "
          "\"baseline_cause\": \"animation track does not map exactly to model bone name/index\", \"selected_model\": "
          "\"data/art/models/ei_shocktrooper.alo\", \"r0_candidates\": [\"data/art/models/ei_shocktrooper.alo\"], "
          "\"status\": \"single_compatible_unapproved\", \"effective_failure_retained\": " + parts.retained
        + ", \"approved\": false, \"clip_motion\": \"moving\", \"track_count\": 26, \"requirement_count\": 26, "
          "\"effective_model\": {\"path\": \"data/art/models/ei_shocktrooper.alo\", \"sha256\": \"" + parts.effective_model_sha
        + "\", \"layer_id\": \"mod\", \"origin\": \"loose\", \"source_id\": \"mod:loose:data/Art/Models/EI_Shocktrooper.ALO\", "
          "\"original_path\": \"data/Art/Models/EI_Shocktrooper.ALO\", \"size\": 998390, \"found\": true, \"status\": "
          "\"rejected\", \"approved\": false, \"error_code\": \"\", \"error_message\": \"\", \"bone_count\": 20, "
          "\"max_track_bone_index\": 26, \"range_impossible\": true, \"rejection_stage\": \"binding\", \"player_accepts\": "
          "false, \"consistent_with_player\": true, \"player_code\": \"EAWR-ANIMATION-0002\", \"player_message\": \"x\", "
          "\"samples\": [], \"frozen_identity\": \"matched\"}, \"shortlist\": {\"structural_candidates\": 1, \"evaluated\": 1, "
          "\"compatible_unapproved\": 1, \"rejected_binding\": 0, \"rejected_sampling\": 0}, \"candidates\": ["
        + candidates + "]}";
}

[[nodiscard]] std::string receipt(const ReceiptParts& parts = {}) {
    std::string text = "{\n  \"schema\": \"eawr.animation-shocktrooper-structural-discovery\",\n  \"schema_version\": 1,\n"
                       "  \"profile\": \"remake-effective\",\n  \"diagnosis_only\": true,\n  \"association_approved\": "
        + parts.approved_header + ",\n  \"associations_promoted\": 0,\n  \"ledger_mutated\": false,\n"
          "  \"corpus_acceptance_claimed\": false,\n  \"retail_behaviour_claimed\": false,\n"
          "  \"baseline\": {\"ledger_schema\": \"eawr.animation-playback-corpus\", \"animation_count\": 7685, "
          "\"playback_passed\": 7329, \"failure_count\": 356},\n"
          "  \"frozen_metadata\": {\"schema\": \"eawr.animation-association-frozen-metadata\", \"pinned_sha256\": \""
        + parts.frozen_sha + "\", \"sha256\": \"" + parts.frozen_sha + "\", \"bytes\": 1},\n"
          "  \"model_inventory\": {\"entries\": [\n"
          "    {\"path\": \"data/art/models/go/ei_shocktrooper.alo\", \"sha256\": \"" + parts.candidate_sha + "\"}\n  ]},\n"
          "  \"counts\": {\"rows\": 2, \"rows_by_status\": " + parts.rows_by_status
        + ", \"compatible_unapproved_pairs\": " + parts.compatible_pairs + ", \"rejected_binding_pairs\": 0},\n"
          "  \"rows\": [\n" + lead_row(parts) + ",\n";
    if (parts.second_lead_row) text += lead_row(parts) + ",\n";
    text += "    {\"animation\": {\"path\": \"data/art/models/ei_shocktrooper_die_00.ala\"}, \"baseline_failure_index\": 80, "
            "\"status\": \"zero_compatible\", \"approved\": " + parts.other_row_approved + ", \"candidates\": []}\n  ]\n}\n";
    return text;
}

[[nodiscard]] std::optional<std::string> authenticate(const std::string& text, const std::optional<std::string>& pin = {}) {
    const auto lead = synthetic_lead(pin.value_or(sha256(text)));
    return lead_probe::authenticate_receipt(text, sha256(text), lead, synthetic_frozen());
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

void test_pinned_constants() {
    const auto lead = lead_probe::pinned_lead();
    expect(lead.receipt_sha256 == "cdf77df4593325032f437c7567e799a14e40db38beda40e9cc67b490b2267d2e", "pinned R2 receipt");
    expect(lead.failure_index == 79 && lead.animation_path == clip_path && lead.selected_model == selected_path,
        "pinned failure 79 and its selected model");
    expect(lead.candidate.path == candidate_path
            && lead.candidate.sha256 == "a01bc8c278fb23dbccbe2ae186050b36f90038b0b60ae96e4e42859c8e33a35a"
            && lead.candidate.layer_id == "mod" && lead.candidate.origin == "loose"
            && lead.candidate.source_id == "mod:loose:data/Art/Models/GO/EI_ShockTrooper.ALO"
            && lead.candidate.original_path == "data/Art/Models/GO/EI_ShockTrooper.ALO" && lead.candidate.size == 2809575,
        "pinned candidate identity");
    expect(declarations::canonical_model_path("GO/EI_ShockTrooper.ALO") == candidate_path
            && declarations::canonical_model_path("EI_Shocktrooper") == selected_path,
        "the declared names canonicalise to the lead");
}

// The tracked manifest yields the lead; the tracked 63 pairs still pass the
// unchanged R0 guard, which refuses the lead, alone or added to them.
void test_tracked_inputs() {
    const auto metadata_bytes = read_file(EAWR_ASSOCIATION_FROZEN_MANIFEST);
    const auto pairs_bytes = read_file(EAWR_DECLARATION_PAIRS);
    expect(metadata_bytes.has_value() && pairs_bytes.has_value(), "tracked inputs are readable");
    if (!metadata_bytes || !pairs_bytes) return;
    const auto metadata = audit::load_frozen_metadata(metadata_bytes, sha256(*metadata_bytes));
    expect(metadata.metadata.has_value(), "tracked frozen manifest matches its pin: " + metadata.error);
    if (!metadata.metadata) return;
    const audit::FrozenMetadata& frozen = *metadata.metadata;
    expect(frozen.playback_passed == 7329 && frozen.failure_count == 356 && frozen.animation_count == 7685,
        "ledger stays 7,329 / 356");
    const auto load = lead_probe::lead_from_frozen(frozen);
    expect(load.lead.has_value(), "the pinned lead matches the tracked manifest: " + load.error);
    if (load.lead) {
        const auto& lead = *load.lead;
        expect(lead.failure.animation.path == clip_path && lead.failure.stage == "binding"
                && lead.failure.code == "EAWR-ANIMATION-0002" && lead.failure.r0_candidates.size() == 1,
            "failure 79 is the one-model binding failure");
        expect(lead.selected.path == selected_path && lead.selected_bone_count == std::optional<std::size_t>{20},
            "selected model has its frozen 20-bone identity");
        expect(lead.r0_guard.find("is not in the frozen R0 list") != std::string::npos, "the R0 guard refuses the lead");
        expect(lead.pinned.pairs.size() == 1 && lead.pinned.pairs.front().candidate.path == candidate_path,
            "exactly one pair is evaluated");
    }
    const auto pinned = declarations::load_pinned_pairs(pairs_bytes, sha256(*pairs_bytes));
    expect(pinned.pairs.has_value() && pinned.pairs->pairs.size() == 63, "the 63 pinned pairs still load");
    if (!pinned.pairs) return;
    expect(!declarations::check_pairs_against_frozen(*pinned.pairs, frozen), "the 63 pairs still pass the R0 guard");
    if (load.lead) {
        auto extended = *pinned.pairs;
        extended.pairs.push_back(load.lead->pinned.pairs.front());
        const auto error = declarations::check_pairs_against_frozen(extended, frozen);
        expect(error.has_value() && error->find("not in the frozen R0 list") != std::string::npos,
            "the R0 guard refuses the lead added to the 63 pairs");
        expect(!declarations::parse_pinned_pairs(*pairs_bytes, 64, 58).pairs, "the pinned pair count stays 63");
    }
}

void test_lead_from_frozen_refusals() {
    const auto frozen = synthetic_frozen();
    expect(lead_probe::lead_from_frozen(frozen, spec()).lead.has_value(), "synthetic lead derives");
    const auto refused = [](const audit::FrozenMetadata& metadata, const lead_probe::LeadSpec& item,
                             const std::string_view message, const std::string_view reason = {}) {
        const auto load = lead_probe::lead_from_frozen(metadata, item);
        expect(!load.lead && !load.error.empty() && load.error.find(reason) != std::string::npos,
            std::string(message) + ": " + load.error);
    };
    auto other = spec();
    other.failure_index = 81;
    refused(frozen, other, "absent failure index refuses");
    other = spec();
    other.failure_index = 80;
    refused(frozen, other, "another failure refuses");
    other = spec();
    other.selected_model = "data/art/models/ei_shocktrooper_heavy.alo";
    refused(frozen, other, "another selected model refuses");
    other = spec();
    other.candidate.path = std::string(selected_path);
    refused(frozen, other, "candidate equal to the selected model refuses");
    auto in_r0 = frozen;
    in_r0.failures[79].r0_candidates.push_back(std::string(candidate_path));
    refused(in_r0, spec(), "a candidate inside R0 refuses", "candidate is in the frozen R0 list");
    auto recorded = frozen;
    recorded.models[std::string(candidate_path)] = audit::FrozenModel{lead_probe::pinned_lead().candidate, 27};
    refused(recorded, spec(), "a candidate with a frozen record refuses", "named by the frozen metadata");
    auto unrecorded = frozen;
    unrecorded.unrecorded = {std::string(candidate_path)};
    refused(unrecorded, spec(), "an unrecorded frozen candidate refuses", "named by the frozen metadata");
    auto no_selected = frozen;
    no_selected.models.clear();
    refused(no_selected, spec(), "a selected model without frozen identity refuses");
    auto parser = frozen;
    parser.failures[79].stage = "parse";
    refused(parser, spec(), "a non-binding failure refuses");
}

void test_receipt_authentication() {
    const std::string good = receipt();
    const auto passed = authenticate(good);
    expect(!passed, "the synthetic R2 receipt authenticates: " + passed.value_or(""));

    const auto stale = authenticate(good, std::string(64, '0'));
    expect(stale.has_value() && stale->find("stale") != std::string::npos, "a stale receipt hash refuses");
    const auto lead = synthetic_lead();
    const auto missing = lead_probe::authenticate_receipt(std::nullopt, std::nullopt, lead, synthetic_frozen());
    expect(missing.has_value() && missing->find("missing") != std::string::npos, "a missing receipt refuses");
    const auto empty = lead_probe::authenticate_receipt(std::string{}, sha256(""), lead, synthetic_frozen());
    expect(empty.has_value(), "an empty receipt refuses");

    const auto tampered = [](const std::function<void(ReceiptParts&)>& change, const std::string_view message) {
        ReceiptParts parts;
        change(parts);
        const std::string text = receipt(parts);
        // Pinned to its own hash: only the content checks can refuse it.
        const auto error = authenticate(text);
        expect(error.has_value(), message);
    };
    tampered([](ReceiptParts& parts) { parts.frozen_sha = std::string(64, '9'); }, "another frozen manifest refuses");
    tampered([](ReceiptParts& parts) { parts.approved_header = "true"; }, "an approved receipt refuses");
    tampered([](ReceiptParts& parts) { parts.candidate_approved = "true"; }, "an approved candidate refuses");
    tampered([](ReceiptParts& parts) { parts.other_row_approved = "true"; }, "an approval on any other row refuses");
    tampered([](ReceiptParts& parts) {
        parts.rows_by_status = "{\"multiple_compatible_unapproved\": 1, \"single_compatible_unapproved\": 0, \"zero_compatible\": 1}";
    }, "a multiple-candidate count refuses");
    tampered([](ReceiptParts& parts) { parts.compatible_pairs = "2"; }, "two compatible pairs refuse");
    tampered([](ReceiptParts& parts) { parts.failure_index = "78"; }, "another failure index refuses");
    tampered([](ReceiptParts& parts) { parts.effective_model_sha = std::string(64, '8'); },
        "a selected model other than the frozen one refuses");
    tampered([](ReceiptParts& parts) { parts.candidate_sha = std::string(64, '7'); }, "another candidate hash refuses");
    tampered([](ReceiptParts& parts) { parts.candidate_source = "base:Data/Models.meg"; },
        "another candidate source refuses");
    tampered([](ReceiptParts& parts) { parts.candidate_status = "rejected"; }, "a rejected candidate refuses");
    tampered([](ReceiptParts& parts) { parts.frozen_identity = "matched"; }, "a frozen-recorded candidate refuses");
    tampered([](ReceiptParts& parts) { parts.r0_candidate = "true"; }, "an R0 candidate refuses");
    tampered([](ReceiptParts& parts) { parts.retained = "false"; }, "a failure no longer retained refuses");
    tampered([](ReceiptParts& parts) { parts.second_candidate = true; }, "two candidates in the row refuse");
    tampered([](ReceiptParts& parts) { parts.second_lead_row = true; }, "two rows with candidates refuse");
}

// The receipt-fragment construction matches the real R2 format for the real
// lead: this row head is copied from the retained receipt (line 7385).
void test_real_row_format() {
    const auto metadata_bytes = read_file(EAWR_ASSOCIATION_FROZEN_MANIFEST);
    if (!metadata_bytes) return;
    const auto metadata = audit::load_frozen_metadata(metadata_bytes, sha256(*metadata_bytes));
    if (!metadata.metadata) return;
    const auto load = lead_probe::lead_from_frozen(*metadata.metadata);
    if (!load.lead) return;
    const std::string real_head =
        R"(    {"animation": {"path": "data/art/models/ei_shocktrooper_attachflinchf_01.ala", "sha256": "c2f17bc0320fc92f2e078d8526eab762ef19249c0bcf43f7547ae022d6f6ec43", "layer_id": "mod", "origin": "loose", "source_id": "mod:loose:data/Art/Models/EI_ShockTrooper_ATTACHFLINCHF_01.ala", "original_path": "data/Art/Models/EI_ShockTrooper_ATTACHFLINCHF_01.ala", "size": 3166}, "baseline_failure_index": 79, "baseline_stage": "binding", "baseline_code": "EAWR-ANIMATION-0002", "baseline_cause": "animation track does not map exactly to model bone name/index", "selected_model": "data/art/models/ei_shocktrooper.alo", "r0_candidates": ["data/art/models/ei_shocktrooper.alo"], "status": "single_compatible_unapproved", "effective_failure_retained": true, "approved": false,)";
    const std::string real_effective =
        R"("effective_model": {"path": "data/art/models/ei_shocktrooper.alo", "sha256": "42921992ce62a6cca30d593c24b141d8d084527f51c5b3ce065a9b48e9895510", "layer_id": "mod", "origin": "loose", "source_id": "mod:loose:data/Art/Models/EI_Shocktrooper.ALO", "original_path": "data/Art/Models/EI_Shocktrooper.ALO", "size": 998390, "found": true, "status": "rejected", "approved": false,)";
    const std::string real_candidate =
        R"("candidates": [{"selected_model": false, "r0_candidate": false, "bytes_identical_to_selected": false, "result": {"path": "data/art/models/go/ei_shocktrooper.alo", "sha256": "a01bc8c278fb23dbccbe2ae186050b36f90038b0b60ae96e4e42859c8e33a35a", "layer_id": "mod", "origin": "loose", "source_id": "mod:loose:data/Art/Models/GO/EI_ShockTrooper.ALO", "original_path": "data/Art/Models/GO/EI_ShockTrooper.ALO", "size": 2809575, "found": true, "status": "compatible_unapproved", "approved": false,)";
    const auto& lead = *load.lead;
    expect(("    {\"animation\": " + lead_probe::detail::identity_fields(lead.failure.animation)).size() < real_head.size()
            && real_head.starts_with("    {\"animation\": " + lead_probe::detail::identity_fields(lead.failure.animation) + "}"),
        "frozen ALA identity renders as in the R2 receipt");
    expect(real_effective.starts_with("\"effective_model\": " + lead_probe::detail::identity_fields(lead.selected) + ", "),
        "frozen selected identity renders as in the R2 receipt");
    expect(real_candidate.find(lead_probe::detail::identity_fields(lead.spec.candidate) + ", \"found\": true")
            != std::string::npos,
        "pinned candidate identity renders as in the R2 receipt");
}

void test_current_identity() {
    const auto frozen = synthetic_frozen();
    const auto lead = synthetic_lead();
    const auto lookup = [&lead](const std::string& field) -> declarations::IdentityLookup {
        return [&lead, field](const std::string& path) -> std::optional<audit::AssetIdentity> {
            if (path == clip_path) {
                if (field == "ala-missing") return std::nullopt;
                auto result = clip_identity();
                if (field == "ala-sha256") result.sha256 = std::string(64, '1');
                if (field == "ala-layer") result.layer_id = "base";
                return result;
            }
            if (path == selected_path) {
                auto result = selected_identity();
                if (field == "selected-source") result.source_id = "mod:Data/Models.meg";
                if (field == "selected-size") result.size = 1;
                return result;
            }
            if (path != candidate_path || field == "missing") return std::nullopt;
            auto result = lead.spec.candidate;
            if (field == "sha256") result.sha256 = std::string(64, '7');
            if (field == "layer") result.layer_id = "expansion";
            if (field == "origin") result.origin = "archive";
            if (field == "source") result.source_id = "mod:loose:data/Art/Models/Other/EI_ShockTrooper.ALO";
            if (field == "original") result.original_path = "data/Art/Models/Other/EI_ShockTrooper.ALO";
            if (field == "size") result.size = 2809576;
            return result;
        };
    };
    const auto current = lead_probe::check_current(lead, frozen, lookup(""));
    expect(!current, "current identities pass: " + current.value_or(""));
    for (const std::string field : {"sha256", "layer", "origin", "source", "original", "size", "missing", "ala-missing",
             "ala-sha256", "ala-layer", "selected-source", "selected-size"})
        expect(lead_probe::check_current(lead, frozen, lookup(field)).has_value(), "stale current identity refuses: " + field);
}

// ---------------------------------------------------------------------------
// Catalog.
// ---------------------------------------------------------------------------

struct TempTree {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-structural-declaration-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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

void registries(const std::filesystem::path& root, const std::string_view objects) {
    write(root / "XML" / "GameObjectFiles.xml", "<Game_Object_Files>" + std::string(objects) + "</Game_Object_Files>");
    write(root / "XML" / "HardpointDataFiles.xml", "<Hard_Point_Files></Hard_Point_Files>");
    write(root / "XML" / "FactionFiles.xml", "<Faction_Files></Faction_Files>");
    write(root / "XML" / "CampaignFiles.xml", "<Campaign_Files></Campaign_Files>");
    write(root / "XML" / "SFXEventFiles.xml", "<SFXEvent_Files><File>sfx.xml</File></SFXEvent_Files>");
}

struct Run {
    lead_probe::Result result;
    std::string bytes;
};

[[nodiscard]] std::string render(const lead_probe::Result& result) {
    std::ostringstream output;
    lead_probe::Header header;
    header.compiler = "synthetic";
    header.config = "test";
    lead_probe::write_receipt(output, header, synthetic_lead(), synthetic_frozen(), result);
    return output.str();
}

// Loads a two-layer catalog: `base` units under the base layer, `mod` units
// (if any) under the mod layer, and one SFX file.
[[nodiscard]] std::optional<Run> run(const std::string_view base, const std::string_view mod = {},
    const std::string_view sfx = "<SFXEvents></SFXEvents>", const std::string_view extra_registry = {}) {
    TempTree tree;
    registries(tree.root, "<File>units.xml</File>" + std::string(extra_registry));
    write(tree.root / "XML" / "units.xml", "<GameObjects>" + std::string(base) + "</GameObjects>");
    write(tree.root / "XML" / "sfx.xml", sfx);
    if (!mod.empty()) {
        registries(tree.mod_root, "<File>units.xml</File><File>mod-units.xml</File>" + std::string(extra_registry));
        write(tree.mod_root / "XML" / "mod-units.xml", "<GameObjects>" + std::string(mod) + "</GameObjects>");
    }
    const std::array mounts{
        eawr::vfs::MountSpec{"mod", tree.mod_root, "data", {}},
        eawr::vfs::MountSpec{"base", tree.root, "data", {}},
    };
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "synthetic VFS mounts");
    if (!mounted) return std::nullopt;
    const eawr::vfs::Vfs& vfs = mounted.value();
    const declarations::SourceHash source_hash = [&vfs](const std::string& logical_path) {
        auto bytes = vfs.open(logical_path);
        if (!bytes) return std::string{};
        return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size()));
    };
    auto loaded = eawr::data::load_catalog(vfs, eawr::data::Profile::remake);
    expect(static_cast<bool>(loaded), "synthetic catalog loads");
    if (!loaded) return std::nullopt;
    const auto lead = synthetic_lead();
    Run first{lead_probe::diagnose(lead, &loaded.value().catalog, loaded.value().diagnostics, {}, source_hash), {}};
    expect(first.result.error.empty(), "diagnosis is consistent: " + first.result.error);
    first.bytes = render(first.result);
    // Reloaded, with the catalog diagnostics shuffled: the same bytes.
    std::mt19937 random(79U);
    for (int round = 0; round < 4; ++round) {
        auto reloaded = eawr::data::load_catalog(vfs, eawr::data::Profile::remake);
        if (!reloaded) {
            expect(false, "synthetic catalog reloads");
            break;
        }
        auto diagnostics = reloaded.value().diagnostics;
        std::shuffle(diagnostics.begin(), diagnostics.end(), random);
        const auto again = lead_probe::diagnose(lead, &reloaded.value().catalog, diagnostics, {}, source_hash);
        expect(render(again) == first.bytes, "receipt bytes are independent of load and diagnostic order");
    }
    return first;
}

[[nodiscard]] bool observed(const Run& item, const std::string_view kind, const std::string_view object = {}) {
    const auto& observations = item.result.audit.pairs.front().observations;
    return std::any_of(observations.begin(), observations.end(), [&](const declarations::Observation& observation) {
        return observation.kind == kind && (object.empty() || declarations::iequals(observation.object_id, object));
    });
}

[[nodiscard]] const std::string& outcome(const Run& item) { return item.result.outcome; }

void no_approval(const Run& item, const std::string_view label) {
    const std::string& bytes = item.bytes;
    expect(count(bytes, "\"approved\": true") == 0 && count(bytes, "\"approved\": false") == 2
            && count(bytes, "\"association_approved\": false") == 1 && count(bytes, "\"associations_promoted\": 0") == 1
            && count(bytes, "\"lead_approved\": false") == 1 && count(bytes, "\"lead_promoted\": false") == 1
            && count(bytes, "\"r0_extended\": false") == 1 && count(bytes, "\"ledger_mutated\": false") == 1
            && count(bytes, "\"playback_passed\": 7329, \"failure_count\": 356, \"unchanged\": true") == 1
            && count(bytes, "\"effective_failure_retained\": true") == 1,
        std::string("receipt carries no approval and an unchanged ledger: ") + std::string(label));
    for (const std::string_view key : {"\"conflicts\": ", "\"unresolved\": ", "\"evidence\": ", "\"observations\": ",
             "\"diagnostics\": ", "\"outcome\": ", "\"disposition\": "})
        expect(count(bytes, key) == 1, "receipt key is unique: " + std::string(key));
    expect(std::none_of(bytes.begin(), bytes.end(), [](const char c) {
        return static_cast<unsigned char>(c) < 0x20U && c != '\n';
    }), "no raw control byte in the receipt");
}

constexpr std::string_view go_link = R"xml(<GroundInfantry Name="EI_SHOCKTROOPER_GO">
  <Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name>
  <Land_Model_Anim_Override_Name>EI_Shocktrooper</Land_Model_Anim_Override_Name></GroundInfantry>)xml";

void test_catalog_evidence() {
    namespace outcome_name = lead_probe::outcome;
    // The GO directory is the candidate: evidence, with provenance.
    if (const auto item = run(go_link)) {
        expect(outcome(*item) == outcome_name::evidence_bearing, "GO land model plus exact override is evidence-bearing");
        const auto& pair = item->result.audit.pairs.front();
        expect(pair.evidence.size() == 1 && pair.evidence.front().object_id == "EI_SHOCKTROOPER_GO"
                && pair.evidence.front().model.tag == "Land_Model_Name"
                && pair.evidence.front().model.declared == "GO/EI_ShockTrooper.ALO"
                && pair.evidence.front().model.source.logical_path == "data/xml/units.xml"
                && pair.evidence.front().model.source.layer_id == "base" && pair.evidence.front().model.source.line == 2
                && pair.evidence.front().model.source_sha256.size() == 64
                && pair.evidence.front().animation_set.canonical == selected_path,
            "evidence carries the winning definition, field, source and XML hash");
        expect(item->bytes.find("\"outcome\": \"evidence_bearing_research_lead_only\"") != std::string::npos,
            "the outcome is a research lead only");
        no_approval(*item, "evidence");
    }
    // A backslash separator is the same effective path.
    if (const auto item = run(R"xml(<GroundInfantry Name="U"><Land_Model_Name>GO\EI_ShockTrooper.ALO</Land_Model_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper</Land_Model_Anim_Override_Name></GroundInfantry>)xml"))
        expect(outcome(*item) == outcome_name::evidence_bearing, "GO\\ spelling links");

    // The same basename in the root or in another directory is another model.
    if (const auto item = run(R"xml(<GroundInfantry Name="ROOT_UNIT"><Land_Model_Name>EI_ShockTrooper.ALO</Land_Model_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper</Land_Model_Anim_Override_Name></GroundInfantry>
      <GroundInfantry Name="OTHER_UNIT"><Land_Model_Name>Other/EI_ShockTrooper.ALO</Land_Model_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper</Land_Model_Anim_Override_Name></GroundInfantry>
      <GroundInfantry Name="GO_PREFIX"><Land_Model_Name>GO/EI_ShockTrooper_B.ALO</Land_Model_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper</Land_Model_Anim_Override_Name></GroundInfantry>)xml")) {
        expect(outcome(*item) == outcome_name::no_explicit_evidence && item->result.audit.pairs.front().evidence.empty(),
            "the same basename outside GO never links");
        expect(observed(*item, declarations::observation::set_on_other_model, "ROOT_UNIT")
                && observed(*item, declarations::observation::set_on_other_model, "OTHER_UNIT")
                && observed(*item, declarations::observation::set_on_other_model, "GO_PREFIX")
                && !observed(*item, declarations::observation::model_only),
            "the set on another model is an observation only");
        no_approval(*item, "basename");
    }

    // Inherited GO model, exact override on the child.
    if (const auto item = run(R"xml(<GroundInfantry Name="GO_BASE"><Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name></GroundInfantry>
      <GroundInfantry Name="GO_CHILD"><Variant_Of_Existing_Type>GO_BASE</Variant_Of_Existing_Type>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper</Land_Model_Anim_Override_Name></GroundInfantry>)xml")) {
        const auto& pair = item->result.audit.pairs.front();
        expect(outcome(*item) == outcome_name::evidence_bearing && pair.evidence.size() == 1
                && pair.evidence.front().object_id == "GO_CHILD"
                && pair.evidence.front().chain == std::vector<std::string>{"GO_CHILD", "GO_BASE"}
                && pair.evidence.front().model.provenance == "inherited"
                && pair.evidence.front().model.source_object_id == "GO_BASE"
                && pair.evidence.front().animation_set.provenance == "added",
            "inherited GO model plus the child's exact override is evidence with inheritance provenance");
        expect(observed(*item, declarations::observation::model_only, "GO_BASE"), "the base alone is model-only");
    }

    // Model_Name is the land model only without Land_Model_Name.
    if (const auto item = run(R"xml(<GroundInfantry Name="U"><Model_Name>GO/EI_ShockTrooper.ALO</Model_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper</Land_Model_Anim_Override_Name></GroundInfantry>)xml")) {
        expect(outcome(*item) == outcome_name::evidence_bearing
                && item->result.audit.pairs.front().evidence.front().model.tag == "Model_Name",
            "Model_Name fallback links");
    }
    if (const auto item = run(R"xml(<GroundInfantry Name="U"><Model_Name>GO/EI_ShockTrooper.ALO</Model_Name>
      <Land_Model_Name>EI_ShockTrooper.ALO</Land_Model_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper</Land_Model_Anim_Override_Name></GroundInfantry>)xml")) {
        expect(outcome(*item) == outcome_name::no_explicit_evidence && observed(*item, declarations::observation::model_only, "U"),
            "Model_Name under another Land_Model_Name does not link");
    }
}

void test_catalog_nonqualification() {
    namespace outcome_name = lead_probe::outcome;
    namespace observation = declarations::observation;
    // Model tag alone.
    if (const auto item = run(R"xml(<GroundInfantry Name="U"><Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name>
      <Space_Model_Name>GO/EI_ShockTrooper.ALO</Space_Model_Name></GroundInfantry>)xml")) {
        expect(outcome(*item) == outcome_name::no_explicit_evidence && observed(*item, observation::model_only, "U"),
            "model-only tags are observations only");
        no_approval(*item, "model-only");
    }
    // Wrong set.
    if (const auto item = run(R"xml(<GroundInfantry Name="U"><Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper_Heavy</Land_Model_Anim_Override_Name></GroundInfantry>)xml")) {
        expect(outcome(*item) == outcome_name::no_explicit_evidence && observed(*item, observation::override_other_set, "U"),
            "another set is an observation only");
    }
    // Clip mentions in an SFX sample and an unknown clip field, with the GO model.
    if (const auto item = run(R"xml(<GroundInfantry Name="U"><Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name>
      <Unknown_Clip_Field>EI_ShockTrooper_ATTACHFLINCHF_01</Unknown_Clip_Field>
      <Animation_Name>EI_ShockTrooper_ATTACHFLINCHF_01.ALA</Animation_Name></GroundInfantry>)xml", {},
            R"xml(<SFXEvents><SFXEvent Name="SFX_FLINCH"><Samples>EI_ShockTrooper_ATTACHFLINCHF_01, Data\Audio\f.wav</Samples></SFXEvent></SFXEvents>)xml")) {
        expect(outcome(*item) == outcome_name::no_explicit_evidence && item->result.audit.pairs.front().evidence.empty(),
            "clip mentions do not qualify");
        expect(observed(*item, observation::text_mention, "SFX_FLINCH") && observed(*item, observation::text_mention, "U"),
            "SFX and unknown-field clip mentions are observed");
        // The unknown field is a schema note: counted and hashed, not listed.
        const auto& exclusions = item->result.exclusions;
        const auto notes = std::count_if(exclusions.diagnostics.begin(), exclusions.diagnostics.end(),
            [](const auto& diagnostic) { return lead_probe::schema_note(diagnostic); });
        expect(notes > 0 && exclusions.diagnostics_sha256.size() == 64
                && item->bytes.find("\"schema_notes_counted_not_listed\": " + std::to_string(notes)) != std::string::npos
                && item->bytes.find("\"code\": \"EAWR-XML-0010\"") == std::string::npos
                && item->result.audit.catalog.diagnostics_by_code.contains("EAWR-XML-0010"),
            "schema notes are counted, hashed and not listed");
    }
    // A shadowed base-layer definition declares the link; the mod winner does not.
    if (const auto item = run(R"xml(<GroundInfantry Name="EI_SHOCKTROOPER_GO"><Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper</Land_Model_Anim_Override_Name></GroundInfantry>)xml",
            R"xml(<GroundInfantry Name="EI_SHOCKTROOPER_GO"><Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name></GroundInfantry>)xml")) {
        expect(outcome(*item) == outcome_name::no_explicit_evidence
                && observed(*item, observation::shadowed_definition, "EI_SHOCKTROOPER_GO"),
            "a shadowed definition is an observation only");
    }
    // ... and the reverse: the mod winner declares it.
    if (const auto item = run(R"xml(<GroundInfantry Name="EI_SHOCKTROOPER_GO"><Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name></GroundInfantry>)xml",
            go_link)) {
        const auto& pair = item->result.audit.pairs.front();
        expect(outcome(*item) == outcome_name::evidence_bearing && pair.evidence.front().animation_set.source.layer_id == "mod",
            "the effective mod winner's link is evidence-bearing");
    }
    // Nothing at all.
    if (const auto item = run(R"xml(<GroundInfantry Name="U"><Land_Model_Name>Unrelated.ALO</Land_Model_Name></GroundInfantry>)xml")) {
        expect(outcome(*item) == outcome_name::no_explicit_evidence
                && item->result.audit.pairs.front().observations.empty(),
            "an unrelated catalog has no evidence and no observation");
        no_approval(*item, "empty");
    }
}

void test_catalog_precedence() {
    namespace outcome_name = lead_probe::outcome;
    // An unresolved object naming the candidate outranks evidence elsewhere.
    if (const auto item = run(std::string(go_link) + R"xml(<GroundInfantry Name="ORPHAN">
      <Variant_Of_Existing_Type>GONE</Variant_Of_Existing_Type>
      <Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name></GroundInfantry>)xml")) {
        const auto& pair = item->result.audit.pairs.front();
        expect(outcome(*item) == outcome_name::unresolved && pair.evidence.size() == 1 && pair.unresolved.size() == 1,
            "an unresolved object outranks evidence");
        no_approval(*item, "unresolved");
    }
    // A repeated override in one definition conflicts even when the kept value links.
    if (const auto item = run(R"xml(<GroundInfantry Name="U"><Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper_Heavy</Land_Model_Anim_Override_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper</Land_Model_Anim_Override_Name></GroundInfantry>)xml")) {
        const auto& pair = item->result.audit.pairs.front();
        expect(outcome(*item) == outcome_name::conflicting && !pair.evidence.empty(), "a conflict outranks evidence");
    }
    // Same-file duplicates that disagree conflict.
    if (const auto item = run(std::string(go_link)
            + R"xml(<GroundInfantry Name="EI_SHOCKTROOPER_GO"><Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name></GroundInfantry>)xml"))
        expect(outcome(*item) == outcome_name::conflicting, "disagreeing same-file duplicates conflict");
    // Conflict outranks an unresolved object.
    if (const auto item = run(R"xml(<GroundInfantry Name="U"><Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper_Heavy</Land_Model_Anim_Override_Name>
      <Land_Model_Anim_Override_Name>EI_Shocktrooper</Land_Model_Anim_Override_Name></GroundInfantry>
      <GroundInfantry Name="ORPHAN"><Variant_Of_Existing_Type>GONE</Variant_Of_Existing_Type>
      <Land_Model_Name>GO/EI_ShockTrooper.ALO</Land_Model_Name></GroundInfantry>)xml"))
        expect(outcome(*item) == outcome_name::conflicting, "a conflict outranks an unresolved object");
    // A missing registry include is a recorded exclusion.
    if (const auto item = run(go_link, {}, "<SFXEvents></SFXEvents>", "<File>absent-units.xml</File>")) {
        const auto& excluded = item->result.exclusions.registry_files_not_loaded;
        expect(std::any_of(excluded.begin(), excluded.end(),
                   [](const auto& file) { return file.included_path == "data/xml/absent-units.xml"; })
                && item->bytes.find("\"included_path\": \"data/xml/absent-units.xml\"") != std::string::npos
                && item->result.audit.catalog.diagnostics_by_code.contains(
                    std::string(eawr::data::diagnostic_codes::registry_include_missing)),
            "a registry include that did not load is listed");
    }
    // A catalog that failed to load is unresolved.
    const auto lead = synthetic_lead();
    const auto failed = lead_probe::diagnose(lead, nullptr, {}, "EAWR-XML-0003: synthetic", {});
    expect(failed.error.empty() && failed.outcome == outcome_name::unresolved, "no catalog is catalog_unresolved");
    const std::string bytes = render(failed);
    expect(bytes.find("\"loaded\": false, \"load_error\": \"EAWR-XML-0003: synthetic\"") != std::string::npos,
        "the load failure is recorded");
}

} // namespace

int main() {
    test_pinned_constants();
    test_tracked_inputs();
    test_lead_from_frozen_refusals();
    test_receipt_authentication();
    test_real_row_format();
    test_current_identity();
    test_catalog_evidence();
    test_catalog_nonqualification();
    test_catalog_precedence();
    if (failures != 0) {
        std::cerr << failures << " structural declaration contract(s) failed\n";
        return 1;
    }
    std::cout << "structural declaration contracts passed\n";
    return 0;
}
