#pragma once

// Offline, test-only declaration diagnosis for the one structural lead of the
// exact-skeleton discovery (P1-03 R2, issue #24).  R2 found exactly one clip,
// frozen failure 79 (ei_shocktrooper_attachflinchf_01.ala), with exactly one
// structurally compatible model, GO/EI_ShockTrooper.ALO, which is outside the
// frozen R0 list.  This probe asks the effective Remake XML catalog one
// question about that single pair: does an active, winning, resolved object
// have the effective land model (Land_Model_Name, else Model_Name)
// GO/EI_ShockTrooper.ALO and the effective Land_Model_Anim_Override_Name
// EI_Shocktrooper, the set the frozen R0 rule attributed the clip to?
//
// The lead is outside R0, so the 63-pair guard (check_pairs_against_frozen)
// must keep refusing it; this probe authenticates its single input
// separately, from the retained R2 receipt and the frozen manifest, and then
// reuses the unchanged declarations::evaluate.  Metadata only: nothing here
// approves, promotes or reassociates, and "evidence_bearing" is a research
// lead, never an association.  The frozen failure and the 7,329 / 356 v1
// ledger stay unchanged.

#include "corpus_associations.hpp"
#include "corpus_declarations.hpp"
#include "corpus_diagnostics.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/sim/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace eawr::tests::animation_corpus::structural_declaration {

inline constexpr std::string_view schema = "eawr.animation-shocktrooper-structural-declaration";
inline constexpr int schema_version = 1;

// The retained R2 receipt (run-1 = run-2 = root run, byte-identical).
inline constexpr std::string_view structural_schema = "eawr.animation-shocktrooper-structural-discovery";
inline constexpr std::string_view pinned_structural_receipt_sha256 =
    "cdf77df4593325032f437c7567e799a14e40db38beda40e9cc67b490b2267d2e";

inline constexpr std::string_view link_rule =
    "structural-lead land-model-anim-override: effective Land_Model_Name (else Model_Name) == structural candidate "
    "and effective Land_Model_Anim_Override_Name == frozen R0-selected set";

namespace outcome {
inline constexpr std::string_view evidence_bearing = "evidence_bearing_research_lead_only";
inline constexpr std::string_view no_explicit_evidence = "no_explicit_evidence_in_effective_loaded_catalog";
inline constexpr std::string_view conflicting = "conflicting_evidence";
inline constexpr std::string_view unresolved = "catalog_unresolved";
} // namespace outcome

using associations::AssetIdentity;
using associations::BaselineFailure;
using associations::FrozenMetadata;

// What the lead is, independently of any receipt.  The candidate identity is
// pinned in full; the clip and the selected model are pinned by path and take
// their identity from the frozen manifest.
struct LeadSpec final {
    std::string receipt_sha256;
    std::size_t failure_index{};
    std::string animation_path;
    std::string selected_model;
    AssetIdentity candidate;
};

[[nodiscard]] inline LeadSpec pinned_lead() {
    return LeadSpec{std::string(pinned_structural_receipt_sha256), 79,
        "data/art/models/ei_shocktrooper_attachflinchf_01.ala", "data/art/models/ei_shocktrooper.alo",
        AssetIdentity{"data/art/models/go/ei_shocktrooper.alo",
            "a01bc8c278fb23dbccbe2ae186050b36f90038b0b60ae96e4e42859c8e33a35a", "mod", "loose",
            "mod:loose:data/Art/Models/GO/EI_ShockTrooper.ALO", "data/Art/Models/GO/EI_ShockTrooper.ALO", 2809575}};
}

struct Lead final {
    LeadSpec spec;
    std::string frozen_metadata_sha256;
    BaselineFailure failure;
    AssetIdentity selected;
    std::optional<std::size_t> selected_bone_count;
    // The one pair, in the shape declarations::evaluate reads.
    declarations::PinnedPairs pinned;
    // What the unchanged 63-pair guard says about this pair (it must refuse).
    std::string r0_guard;
};

struct LeadLoad final {
    std::optional<Lead> lead;
    std::string error;
};

// ---------------------------------------------------------------------------
// Frozen manifest.
// ---------------------------------------------------------------------------

// The lead must be exactly the frozen failure it names, its selected model
// the frozen R0 selection with a recorded identity, and its candidate a model
// the frozen metadata knows nothing about: outside the R0 list, neither
// recorded nor unrecorded.  The unchanged check_pairs_against_frozen must
// refuse the pair because it is outside R0; if it ever accepted it, this
// would not be the R2 lead and the probe refuses.
[[nodiscard]] inline LeadLoad lead_from_frozen(const FrozenMetadata& frozen, const LeadSpec& spec = pinned_lead()) {
    LeadLoad load;
    const auto fail = [&load](std::string message) {
        load.error = "structural lead does not match the frozen manifest: " + std::move(message);
        return load;
    };
    const auto found = frozen.failures.find(spec.failure_index);
    if (found == frozen.failures.end()) return fail("no frozen failure " + std::to_string(spec.failure_index));
    const BaselineFailure& failure = found->second;
    if (failure.animation.path != spec.animation_path)
        return fail("failure " + std::to_string(spec.failure_index) + " is " + failure.animation.path);
    if (failure.stage != "binding" || failure.code != "EAWR-ANIMATION-0002")
        return fail("failure " + std::to_string(spec.failure_index) + " is not a binding failure");
    if (failure.selected_model != spec.selected_model) return fail("selected model is " + failure.selected_model);
    if (std::find(failure.r0_candidates.begin(), failure.r0_candidates.end(), spec.selected_model)
        == failure.r0_candidates.end())
        return fail("selected model is not in its R0 list");
    const auto selected = frozen.models.find(spec.selected_model);
    if (selected == frozen.models.end() || selected->second.identity.path != spec.selected_model)
        return fail("selected model has no frozen identity");
    if (spec.candidate.path.empty() || spec.candidate.path == spec.selected_model)
        return fail("candidate is the selected model");
    if (std::find(failure.r0_candidates.begin(), failure.r0_candidates.end(), spec.candidate.path)
        != failure.r0_candidates.end())
        return fail("candidate is in the frozen R0 list");
    if (frozen.models.contains(spec.candidate.path) || frozen.is_unrecorded(spec.candidate.path))
        return fail("candidate is named by the frozen metadata");

    Lead lead;
    lead.spec = spec;
    lead.frozen_metadata_sha256 = frozen.sha256;
    lead.failure = failure;
    lead.selected = selected->second.identity;
    lead.selected_bone_count = selected->second.bone_count;
    lead.pinned.frozen_metadata_sha256 = frozen.sha256;
    lead.pinned.pairs.push_back(declarations::PinnedPair{
        failure.animation, spec.selected_model, selected->second.identity.sha256, spec.candidate, false});
    const auto guard = declarations::check_pairs_against_frozen(lead.pinned, frozen);
    if (!guard) return fail("the 63-pair R0 guard accepts the lead");
    if (guard->find("is not in the frozen R0 list") == std::string::npos)
        return fail("the 63-pair R0 guard refuses the lead for another reason: " + *guard);
    lead.r0_guard = *guard;
    load.lead = std::move(lead);
    return load;
}

// ---------------------------------------------------------------------------
// Retained R2 receipt.
// ---------------------------------------------------------------------------

namespace detail {

// An identity as the R2 receipt writes it, without the closing brace, so it
// can be followed by the per-model fields.
[[nodiscard]] inline std::string identity_fields(const AssetIdentity& identity) {
    return "{\"path\": " + json_string(identity.path) + ", \"sha256\": " + json_string(identity.sha256)
        + ", \"layer_id\": " + json_string(identity.layer_id) + ", \"origin\": " + json_string(identity.origin)
        + ", \"source_id\": " + json_string(identity.source_id)
        + ", \"original_path\": " + json_string(identity.original_path) + ", \"size\": " + std::to_string(identity.size);
}

[[nodiscard]] inline std::size_t occurrences(const std::string_view text, const std::string_view needle) {
    std::size_t count{};
    for (auto at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) ++count;
    return count;
}

} // namespace detail

// The receipt must be the retained R2 receipt (pinned SHA-256), for the same
// frozen manifest and baseline, with nothing approved, exactly one row with a
// candidate and exactly one compatible pair, and that row must be the lead:
// the frozen ALA identity, failure and R0 list, the selected model's frozen
// identity retaining its binding failure, and the one pinned candidate,
// compatible, unapproved, outside R0 and not in the frozen metadata.
[[nodiscard]] inline std::optional<std::string> authenticate_receipt(const std::optional<std::string>& bytes,
    const std::optional<std::string>& sha256, const Lead& lead, const FrozenMetadata& frozen) {
    if (!bytes || bytes->empty() || !sha256 || sha256->empty())
        return std::string("structural receipt is missing or unreadable");
    if (*sha256 != lead.spec.receipt_sha256)
        return "structural receipt is stale: sha256 " + *sha256 + " is not pinned " + lead.spec.receipt_sha256;
    const std::string_view text = *bytes;
    const auto fail = [](std::string message) -> std::optional<std::string> {
        return "structural receipt does not carry the lead: " + std::move(message);
    };
    const auto frozen_sha = json_string(frozen.sha256);
    for (const std::string& line : {"  \"schema\": " + json_string(structural_schema) + ",\n",
             std::string("  \"schema_version\": 1,\n"), std::string("  \"diagnosis_only\": true,\n"),
             std::string("  \"association_approved\": false,\n"), std::string("  \"associations_promoted\": 0,\n"),
             std::string("  \"ledger_mutated\": false,\n"), std::string("  \"corpus_acceptance_claimed\": false,\n"),
             std::string("  \"retail_behaviour_claimed\": false,\n"),
             "\"frozen_metadata\": {\"schema\": " + json_string(associations::frozen_metadata_schema)
                 + ", \"pinned_sha256\": " + frozen_sha + ", \"sha256\": " + frozen_sha + ",",
             "\"animation_count\": " + std::to_string(frozen.animation_count) + ", \"playback_passed\": "
                 + std::to_string(frozen.playback_passed) + ", \"failure_count\": " + std::to_string(frozen.failure_count)
                 + "}",
             std::string("\"rows_by_status\": {\"multiple_compatible_unapproved\": 0, \"single_compatible_unapproved\": 1,"),
             std::string(", \"compatible_unapproved_pairs\": 1,")}) {
        if (detail::occurrences(text, line) != 1) return fail("expected exactly once: " + line);
    }
    if (text.find("\"approved\": true") != std::string_view::npos) return fail("an approval is recorded");

    // Rows are one line each.
    constexpr std::string_view row_prefix = "    {\"animation\": ";
    std::vector<std::string_view> leads;
    std::size_t rows{};
    for (std::size_t begin = 0; begin < text.size();) {
        const auto end = std::min(text.find('\n', begin), text.size());
        const std::string_view line = text.substr(begin, end - begin);
        if (line.starts_with(row_prefix)) {
            ++rows;
            if (line.find("\"candidates\": []") == std::string_view::npos) leads.push_back(line);
        }
        begin = end + 1;
    }
    if (rows == 0) return fail("no rows");
    if (leads.size() != 1) return fail(std::to_string(leads.size()) + " rows carry candidates, expected 1");
    const std::string_view row = leads.front();
    const BaselineFailure& failure = lead.failure;
    std::string r0 = "[";
    for (std::size_t index = 0; index < failure.r0_candidates.size(); ++index)
        r0 += (index == 0 ? "" : ", ") + json_string(failure.r0_candidates[index]);
    r0 += "]";
    const std::string head = std::string(row_prefix) + detail::identity_fields(failure.animation)
        + "}, \"baseline_failure_index\": " + std::to_string(lead.spec.failure_index)
        + ", \"baseline_stage\": " + json_string(failure.stage) + ", \"baseline_code\": " + json_string(failure.code)
        + ", \"baseline_cause\": " + json_string(failure.cause) + ", \"selected_model\": "
        + json_string(failure.selected_model) + ", \"r0_candidates\": " + r0
        + ", \"status\": \"single_compatible_unapproved\", \"effective_failure_retained\": true, \"approved\": false,";
    if (!row.starts_with(head)) return fail("the candidate row is not frozen failure " + std::to_string(lead.spec.failure_index));
    for (const std::string& fragment :
        {"\"effective_model\": " + detail::identity_fields(lead.selected)
                + ", \"found\": true, \"status\": \"rejected\", \"approved\": false,",
            "\"rejection_stage\": \"binding\", \"player_accepts\": false, \"consistent_with_player\": true, "
            "\"player_code\": " + json_string(failure.code) + ",",
            std::string("\"shortlist\": {\"structural_candidates\": 1, \"evaluated\": 1, \"compatible_unapproved\": 1, "
                        "\"rejected_binding\": 0, \"rejected_sampling\": 0}"),
            "\"candidates\": [{\"selected_model\": false, \"r0_candidate\": false, \"bytes_identical_to_selected\": false, "
            "\"result\": " + detail::identity_fields(lead.spec.candidate)
                + ", \"found\": true, \"status\": \"compatible_unapproved\", \"approved\": false,",
            std::string("\"frozen_identity\": \"not_in_frozen_metadata\"}}]}")}) {
        if (detail::occurrences(row, fragment) != 1) return fail("the candidate row lacks: " + fragment);
    }
    if (detail::occurrences(row, "\"result\": {") != 1) return fail("the candidate row has more than one candidate");
    return std::nullopt;
}

// Stale identities fail closed, through the unchanged declaration check: the
// ALA, the selected model (against its frozen record) and the candidate must
// still be what was pinned, on the manifest-resolved effective VFS.
[[nodiscard]] inline std::optional<std::string> check_current(
    const Lead& lead, const FrozenMetadata& frozen, const declarations::IdentityLookup& lookup) {
    return declarations::check_current_identities(lead.pinned, frozen, lookup);
}

// ---------------------------------------------------------------------------
// Diagnosis.
// ---------------------------------------------------------------------------

struct CatalogExclusions final {
    // Active registry includes that did not load (missing or unparsable).
    std::vector<data::RegistryFile> registry_files_not_loaded;
    // Effective physical XML that could not be read or parsed.
    std::vector<data::InventoryFile> xml_not_parsed;
    std::size_t shadowed_xml{};
    // Every catalog diagnostic, sorted.  The receipt lists all of them except
    // the per-field schema notes (unknown and deprecated fields), which it
    // counts by code and pins, with the rest, through `diagnostics_sha256`.
    std::vector<core::Diagnostic> diagnostics;
    std::string diagnostics_sha256;
};

[[nodiscard]] inline bool schema_note(const core::Diagnostic& diagnostic) {
    return diagnostic.code == data::diagnostic_codes::unknown_field
        || diagnostic.code == data::diagnostic_codes::deprecated_field;
}

namespace detail {

inline void write_diagnostic(std::ostream& output, const core::Diagnostic& item) {
    output << "{\"code\": " << json_string(item.code) << ", \"logical_path\": " << json_string(item.logical_path.value_or(""))
           << ", \"line\": " << item.line.value_or(0) << ", \"column\": " << item.column.value_or(0)
           << ", \"source_id\": " << json_string(item.source_id.value_or(""))
           << ", \"message\": " << json_string(item.message) << '}';
}

} // namespace detail

struct Result final {
    declarations::Audit audit;
    CatalogExclusions exclusions;
    std::string outcome;
    std::string error;
};

inline constexpr std::string_view shadowed_outcome_prefix = "shadowed/inactive physical record";

[[nodiscard]] inline CatalogExclusions exclusions_of(
    const data::Catalog* catalog, const std::vector<core::Diagnostic>& diagnostics) {
    CatalogExclusions result;
    result.diagnostics = diagnostics;
    std::sort(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& left, const auto& right) {
        return std::tie(left.code, left.logical_path, left.line, left.column, left.source_id, left.message)
            < std::tie(right.code, right.logical_path, right.line, right.column, right.source_id, right.message);
    });
    // SHA-256 of every diagnostic's receipt form, each followed by LF.
    std::ostringstream lines;
    for (const core::Diagnostic& item : result.diagnostics) {
        detail::write_diagnostic(lines, item);
        lines << '\n';
    }
    const std::string bytes = lines.str();
    result.diagnostics_sha256 = sim::sha256_hex(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    if (catalog == nullptr) return result;
    for (const auto& file : catalog->registry_files())
        if (!file.loaded) result.registry_files_not_loaded.push_back(file);
    std::sort(result.registry_files_not_loaded.begin(), result.registry_files_not_loaded.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.registry_path, left.include_order, left.included_path)
                < std::tie(right.registry_path, right.include_order, right.included_path);
        });
    for (const auto& file : catalog->physical_inventory()) {
        if (file.parsed) continue;
        if (file.outcome && file.outcome->starts_with(shadowed_outcome_prefix)) {
            ++result.shadowed_xml;
            continue;
        }
        result.xml_not_parsed.push_back(file);
    }
    std::sort(result.xml_not_parsed.begin(), result.xml_not_parsed.end(), [](const auto& left, const auto& right) {
        return std::tie(left.record.canonical_path, left.record.source_id)
            < std::tie(right.record.canonical_path, right.record.source_id);
    });
    return result;
}

// Evaluates the one lead pair with the unchanged declarations::evaluate, then
// re-checks that every evidence record is exactly the qualifying link: the
// effective land model (Land_Model_Name, or Model_Name when the object has no
// Land_Model_Name) naming the candidate, and the effective override naming the
// selected set.  Everything else evaluate records is an observation.
[[nodiscard]] inline Result diagnose(const Lead& lead, const data::Catalog* catalog,
    const std::vector<core::Diagnostic>& diagnostics, const std::string& load_error,
    const declarations::SourceHash& source_hash) {
    Result result;
    result.audit = declarations::evaluate(lead.pinned, catalog, diagnostics, load_error, source_hash);
    result.exclusions = exclusions_of(catalog, diagnostics);
    if (result.audit.pairs.size() != 1) {
        result.error = "evaluate returned " + std::to_string(result.audit.pairs.size()) + " pairs";
        return result;
    }
    const declarations::PairResult& pair = result.audit.pairs.front();
    for (const declarations::Evidence& evidence : pair.evidence) {
        const bool land = evidence.model.tag == "Land_Model_Name";
        const bool fallback = evidence.model.tag == "Model_Name";
        if ((!land && !fallback) || evidence.model.canonical != lead.spec.candidate.path
            || !declarations::iequals(evidence.animation_set.tag, declarations::override_tag)
            || evidence.animation_set.canonical != lead.spec.selected_model) {
            result.error = "evidence of " + evidence.object_id + " is not the qualifying link";
            return result;
        }
        if (fallback && catalog != nullptr) {
            auto resolved = catalog->resolve(evidence.object_id);
            if (!resolved || resolved.value().value("Land_Model_Name") != nullptr) {
                result.error = "evidence of " + evidence.object_id + " uses Model_Name over a Land_Model_Name";
                return result;
            }
        }
    }
    namespace disposition = declarations::disposition;
    if (pair.disposition == disposition::evidence_bearing) result.outcome = outcome::evidence_bearing;
    else if (pair.disposition == disposition::no_explicit_evidence) result.outcome = outcome::no_explicit_evidence;
    else if (pair.disposition == disposition::conflicting_evidence) result.outcome = outcome::conflicting;
    else if (pair.disposition == disposition::catalog_unresolved) result.outcome = outcome::unresolved;
    else result.error = "unknown disposition " + pair.disposition;
    return result;
}

// ---------------------------------------------------------------------------
// Receipt.
// ---------------------------------------------------------------------------

struct Header final {
    std::string compiler;
    std::string config;
    std::vector<std::pair<std::string, std::string>> tool_sources;
    std::vector<associations::MountSummary> mounts;
};

inline void write_receipt(std::ostream& output, const Header& header, const Lead& lead, const FrozenMetadata& frozen,
    const Result& result) {
    namespace write = declarations::detail;
    const declarations::PairResult& pair = result.audit.pairs.front();
    output << "{\n  \"schema\": " << json_string(schema) << ",\n  \"schema_version\": " << schema_version
           << ",\n  \"profile\": \"remake-effective\",\n  \"link_rule\": " << json_string(link_rule)
           << ",\n  \"diagnosis_only\": true,\n  \"research_lead_only\": true,\n  \"association_approved\": false"
           << ",\n  \"associations_promoted\": 0,\n  \"lead_approved\": false,\n  \"lead_promoted\": false"
           << ",\n  \"r0_extended\": false,\n  \"ledger_mutated\": false,\n  \"corpus_acceptance_claimed\": false"
           << ",\n  \"retail_behaviour_claimed\": false"
           << ",\n  \"baseline\": {\"pinned_ledger_sha256\": " << json_string(associations::pinned_frozen_ledger_sha256)
           << ", \"animation_count\": " << frozen.animation_count << ", \"playback_passed\": " << frozen.playback_passed
           << ", \"failure_count\": " << frozen.failure_count << ", \"unchanged\": true}"
           << ",\n  \"inputs\": {\"frozen_metadata_sha256\": " << json_string(frozen.sha256)
           << ", \"pinned_frozen_metadata_sha256\": " << json_string(associations::pinned_frozen_metadata_sha256)
           << ", \"structural_receipt_sha256\": " << json_string(lead.spec.receipt_sha256)
           << ", \"pinned_structural_receipt_sha256\": " << json_string(pinned_structural_receipt_sha256)
           << ", \"structural_receipt_schema\": " << json_string(structural_schema) << '}'
           << ",\n  \"tool\": {\"name\": \"animation_corpus_structural_declaration\", \"compiler\": "
           << json_string(header.compiler) << ", \"config\": " << json_string(header.config) << ", \"source_sha256\": {";
    for (std::size_t index = 0; index < header.tool_sources.size(); ++index)
        output << (index == 0 ? "" : ", ") << json_string(header.tool_sources[index].first) << ": "
               << json_string(header.tool_sources[index].second);
    output << "}},\n  \"mounts\": [";
    for (std::size_t index = 0; index < header.mounts.size(); ++index) {
        const auto& mount = header.mounts[index];
        output << (index == 0 ? "" : ", ") << "{\"layer_id\": " << json_string(mount.layer_id)
               << ", \"manifest_source_id\": " << json_string(mount.manifest_source_id) << ", \"active_archives\": ";
        write::write_list(output, mount.active_archives, [&output](const std::string& item) { output << json_string(item); });
        output << '}';
    }

    const BaselineFailure& failure = lead.failure;
    output << "],\n  \"lead\": {\"baseline_failure_index\": " << lead.spec.failure_index << ", \"animation\": ";
    write::write_identity(output, failure.animation);
    output << ", \"baseline_stage\": " << json_string(failure.stage) << ", \"baseline_code\": " << json_string(failure.code)
           << ", \"baseline_cause\": " << json_string(failure.cause) << ", \"effective_failure_retained\": true"
           << ", \"selected_model\": ";
    write::write_identity(output, lead.selected);
    output << ", \"selected_bone_count\": ";
    if (lead.selected_bone_count) output << *lead.selected_bone_count;
    else output << "null";
    output << ", \"r0_candidates\": ";
    write::write_list(output, failure.r0_candidates, [&output](const std::string& item) { output << json_string(item); });
    output << ", \"candidate\": ";
    write::write_identity(output, lead.spec.candidate);
    output << ", \"candidate_in_r0\": false, \"candidate_in_frozen_metadata\": false"
           << ", \"structural_status\": \"compatible_unapproved\", \"frozen_identity\": \"not_in_frozen_metadata\""
           << ", \"r0_guard\": {\"check\": \"check_pairs_against_frozen\", \"refused\": true, \"reason\": "
           << json_string(lead.r0_guard) << "}, \"current_identities_verified\": true, \"animation_set\": "
           << json_string(pair.animation_set) << ", \"approved\": false}";

    const declarations::CatalogSummary& catalog = result.audit.catalog;
    output << ",\n  \"catalog\": {\"loaded\": " << (catalog.loaded ? "true" : "false")
           << ", \"load_error\": " << json_string(catalog.load_error) << ", \"profile\": " << json_string(catalog.profile)
           << ", \"definitions\": " << catalog.definitions << ", \"objects\": " << catalog.objects
           << ", \"resolved\": " << catalog.resolved << ", \"unresolved_by_code\": ";
    write::write_counts(output, catalog.unresolved_by_code);
    output << ", \"diagnostics_by_code\": ";
    write::write_counts(output, catalog.diagnostics_by_code);
    output << ", \"registry_files\": " << catalog.registry_files
           << ", \"registry_files_loaded\": " << catalog.registry_files_loaded
           << ", \"shadowed_xml_not_read\": " << result.exclusions.shadowed_xml << "}";
    output << ",\n  \"catalog_exclusions\": {\"registry_files_not_loaded\": ";
    write::write_list(output, result.exclusions.registry_files_not_loaded, [&output](const data::RegistryFile& file) {
        output << "{\"category\": " << json_string(data::to_string(file.category))
               << ", \"registry_path\": " << json_string(file.registry_path)
               << ", \"included_path\": " << json_string(file.included_path) << ", \"include_order\": " << file.include_order
               << ", \"source_id\": " << json_string(file.source ? file.source->source_id : std::string{}) << '}';
    });
    output << ",\n    \"xml_not_parsed\": ";
    write::write_list(output, result.exclusions.xml_not_parsed, [&output](const data::InventoryFile& file) {
        output << "{\"path\": " << json_string(file.record.canonical_path)
               << ", \"layer_id\": " << json_string(file.record.layer_id)
               << ", \"source_id\": " << json_string(file.record.source_id)
               << ", \"active_registry_file\": " << (file.active_registry_file ? "true" : "false")
               << ", \"outcome\": " << json_string(file.outcome.value_or("")) << '}';
    });
    std::size_t notes{};
    for (const core::Diagnostic& item : result.exclusions.diagnostics) notes += schema_note(item) ? 1U : 0U;
    output << ",\n    \"diagnostics_count\": " << result.exclusions.diagnostics.size()
           << ", \"diagnostics_sha256\": " << json_string(result.exclusions.diagnostics_sha256)
           << ", \"schema_notes_counted_not_listed\": " << notes << ",\n    \"diagnostics\": [";
    bool first = true;
    for (const core::Diagnostic& item : result.exclusions.diagnostics) {
        if (schema_note(item)) continue;
        output << (first ? "\n      " : ",\n      ");
        detail::write_diagnostic(output, item);
        first = false;
    }
    output << (first ? "]}" : "\n    ]}");

    std::map<std::string, std::size_t> observations;
    for (const auto& item : pair.observations) ++observations[item.kind];
    output << ",\n  \"result\": {\"outcome\": " << json_string(result.outcome)
           << ", \"disposition\": " << json_string(pair.disposition) << ", \"approved\": false"
           << ", \"evidence_records\": " << pair.evidence.size() << ", \"conflict_records\": " << pair.conflicts.size()
           << ", \"unresolved_records\": " << pair.unresolved.size() << ", \"observations_by_kind\": ";
    write::write_counts(output, observations);
    output << ",\n    \"evidence\": ";
    write::write_list(output, pair.evidence, [&output](const declarations::Evidence& item) {
        output << "\n      {\"object_id\": " << json_string(item.object_id) << ", \"type_name\": " << json_string(item.type_name)
               << ", \"category\": " << json_string(item.category) << ", \"chain\": ";
        write::write_list(output, item.chain, [&output](const std::string& link) { output << json_string(link); });
        output << ", \"model\": ";
        write::write_site(output, item.model);
        output << ", \"animation_set\": ";
        write::write_site(output, item.animation_set);
        output << '}';
    });
    output << ",\n    \"conflicts\": ";
    write::write_list(output, pair.conflicts, [&output](const declarations::Conflict& item) {
        output << "\n      {\"object_id\": " << json_string(item.object_id) << ", \"reason\": " << json_string(item.reason)
               << ", \"sites\": ";
        write::write_list(output, item.sites, [&output](const declarations::ValueSite& site) { write::write_site(output, site); });
        output << '}';
    });
    output << ",\n    \"unresolved\": ";
    write::write_list(output, pair.unresolved, [&output](const declarations::Unresolved& item) {
        output << "\n      {\"object_id\": " << json_string(item.object_id) << ", \"code\": " << json_string(item.code)
               << ", \"message\": " << json_string(item.message) << ", \"sites\": ";
        write::write_list(output, item.sites, [&output](const declarations::ValueSite& site) { write::write_site(output, site); });
        output << '}';
    });
    output << ",\n    \"observations\": ";
    write::write_list(output, pair.observations, [&output](const declarations::Observation& item) {
        output << "\n      {\"kind\": " << json_string(item.kind) << ", \"object_id\": " << json_string(item.object_id)
               << ", \"winner\": " << (item.winner ? "true" : "false") << ", \"qualifying\": false, \"site\": ";
        write::write_site(output, item.site);
        output << '}';
    });
    output << "}\n}\n";
}

} // namespace eawr::tests::animation_corpus::structural_declaration
