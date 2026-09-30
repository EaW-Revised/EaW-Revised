#pragma once

// Offline, test-only exact-skeleton discovery for the 22 frozen model_match
// failures (P1-03, issue #24): the 20 ri_padowan_* clips (spelled as the
// assets spell them), underworld_hutt_privateer_die_00 and
// w_allshaders_idle_00.  These rows never reached binding because R0 names no
// model for them, so they have no selected model and no R0 candidate.  For
// each clip it asks which models of the whole effective VFS could satisfy the
// clip's track requirements exactly, whatever their file names:
//
//   - the frozen selection is exactly the 22 authenticated rows: stage
//     model_match, code EAWR-ANIMATION-CORPUS-0001, the frozen cause, no
//     selected model, no R0 candidate; any other model_match row refuses;
//   - the original failure is reproduced first: R0 is re-run against EVERY
//     effective .alo path, parsed or not, and must still name nothing.  A new
//     same-directory prefix model is baseline drift even if it fails to parse;
//   - the complete frozen ALA identity (path, hash, layer, origin, source,
//     original path, size) is re-checked before the clip is parsed;
//   - the inventory, exact (bone index, bone name) shortlist, brute-force
//     cross-check and strict Player/four-sample evaluation are the unchanged
//     R2 ones of corpus_structural_discovery.hpp.
//
// Diagnosis only.  A compatible model is "compatible_unapproved", a
// diagnostic candidate and never an association; every row keeps its original
// model_match failure.  Nothing here changes selection, Player, parsers, the
// VFS, the v1 ledger, the frozen metadata or any receipt.

#include "corpus_associations.hpp"
#include "corpus_diagnostics.hpp"
#include "corpus_source_versions.hpp"
#include "corpus_structural_discovery.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::tests::animation_corpus::model_match_discovery {

namespace audit = associations;
namespace sources = source_versions;
namespace structural = structural_discovery;
namespace model_status = structural::model_status;
namespace row_status = structural::row_status;

inline constexpr std::string_view schema = "eawr.animation-model-match-structural-discovery";
inline constexpr int schema_version = 1;

// The original failure every selected row must carry, byte for byte.
inline constexpr std::string_view model_match_stage = "model_match";
inline constexpr std::string_view model_match_code = "EAWR-ANIMATION-CORPUS-0001";
inline constexpr std::string_view model_match_cause = "no same-directory ALO basename is a prefix of the ALA basename";

// The frozen v1 baseline counts this diagnosis must leave untouched.
inline constexpr std::size_t baseline_animations = 7685;
inline constexpr std::size_t baseline_passed = 7329;
inline constexpr std::size_t baseline_failures = 356;

namespace directory_relation {
inline constexpr std::string_view same = "same_directory";
inline constexpr std::string_view other = "other_directory";
} // namespace directory_relation

struct TargetRow final {
    std::size_t index{};
    std::string_view path;
};

// The bounded slice: exactly these frozen failure indices and ALA paths.
inline constexpr std::array<TargetRow, 22> model_match_targets{{
    {273, "data/art/models/ri_padowan_attack_00.ala"},
    {274, "data/art/models/ri_padowan_block_blaster_00.ala"},
    {275, "data/art/models/ri_padowan_block_blaster_01.ala"},
    {276, "data/art/models/ri_padowan_block_blaster_02.ala"},
    {277, "data/art/models/ri_padowan_block_blaster_03.ala"},
    {278, "data/art/models/ri_padowan_force_healing_00.ala"},
    {279, "data/art/models/ri_padowan_force_run_00.ala"},
    {280, "data/art/models/ri_padowan_hc_draw_00.ala"},
    {281, "data/art/models/ri_padowan_hc_lose_00.ala"},
    {282, "data/art/models/ri_padowan_hc_win_00.ala"},
    {283, "data/art/models/ri_padowan_idle_00.ala"},
    {284, "data/art/models/ri_padowan_idle_blockblaster_00.ala"},
    {285, "data/art/models/ri_padowan_idle_blockblaster_01.ala"},
    {286, "data/art/models/ri_padowan_idle_blockblaster_02.ala"},
    {287, "data/art/models/ri_padowan_move_00.ala"},
    {288, "data/art/models/ri_padowan_move_01.ala"},
    {289, "data/art/models/ri_padowan_redirect_blaster_00.ala"},
    {290, "data/art/models/ri_padowan_redirect_blaster_01.ala"},
    {291, "data/art/models/ri_padowan_redirect_blaster_02.ala"},
    {292, "data/art/models/ri_padowan_redirect_blaster_03.ala"},
    {337, "data/art/models/underworld_hutt_privateer_die_00.ala"},
    {345, "data/art/models/w_allshaders_idle_00.ala"},
}};

// ---------------------------------------------------------------------------
// Frozen selection.
// ---------------------------------------------------------------------------

// The first way a failure differs from the original model_match failure.
[[nodiscard]] inline std::optional<std::string> original_failure_difference(const audit::BaselineFailure& failure) {
    if (failure.stage != model_match_stage) return "stage is " + failure.stage + ", not model_match";
    if (failure.code != model_match_code) return "code is " + failure.code + ", not " + std::string(model_match_code);
    if (failure.cause != model_match_cause) return std::string("cause differs from the frozen model_match cause");
    if (!failure.selected_model.empty()) return "has selected model " + failure.selected_model;
    if (!failure.r0_candidates.empty()) return std::string("has R0 candidates");
    return std::nullopt;
}

struct Selection final {
    std::vector<audit::BaselineFailure> rows;
    std::string error;
};

// Exactly the target rows, in target order.  Every frozen model_match failure
// must be a target and every target a frozen model_match failure with the
// target path; anything else refuses.
[[nodiscard]] inline Selection select_rows(const audit::FrozenMetadata& frozen,
    const std::span<const TargetRow> targets = model_match_targets) {
    Selection selection;
    const auto refuse = [&selection](std::string reason) {
        selection.rows.clear();
        selection.error = std::move(reason);
        return std::move(selection);
    };
    std::set<std::size_t> wanted;
    for (const TargetRow& target : targets)
        if (!wanted.insert(target.index).second) return refuse("target failure " + std::to_string(target.index) + " is listed twice");
    for (const auto& [index, failure] : frozen.failures)
        if (failure.stage == model_match_stage && !wanted.contains(index))
            return refuse("frozen failure " + std::to_string(index) + " (" + failure.animation.path
                + ") is a model_match row outside the selection");
    for (const TargetRow& target : targets) {
        const auto found = frozen.failures.find(target.index);
        const std::string label = "frozen failure " + std::to_string(target.index);
        if (found == frozen.failures.end()) return refuse(label + " is absent");
        if (found->second.animation.path != target.path)
            return refuse(label + " is " + found->second.animation.path + ", not " + std::string(target.path));
        if (const auto difference = original_failure_difference(found->second)) return refuse(label + " " + *difference);
        selection.rows.push_back(found->second);
    }
    return selection;
}

[[nodiscard]] inline std::optional<std::string> check_baseline_counts(const audit::FrozenMetadata& frozen) {
    if (frozen.animation_count != baseline_animations || frozen.playback_passed != baseline_passed
        || frozen.failure_count != baseline_failures)
        return "frozen baseline counts " + std::to_string(frozen.animation_count) + "/" + std::to_string(frozen.playback_passed)
            + "/" + std::to_string(frozen.failure_count) + " are not " + std::to_string(baseline_animations) + "/"
            + std::to_string(baseline_passed) + "/" + std::to_string(baseline_failures);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// R0 reproduction.
// ---------------------------------------------------------------------------

// R0 over every effective model path, whatever its read or parse status, as
// the corpus runner builds it from the effective .alo enumeration.
[[nodiscard]] inline ModelIndex r0_index_of(const structural::Inventory& inventory) {
    ModelIndex index;
    for (const structural::InventoryModel& model : inventory.models) add_model(index, model.record.canonical_path);
    return index;
}

struct R0Reproduction final {
    std::string directory;
    std::string stem;
    // Effective models in the ALA's directory, and how many of them did not
    // parse or read; all of them were tested against the R0 prefix rule.
    std::size_t same_directory_models{};
    std::size_t same_directory_unparsed{};
    // Must stay empty: any entry is baseline drift.
    std::vector<std::string> qualifying;
};

[[nodiscard]] inline R0Reproduction reproduce_r0(const std::string& ala_path, const structural::Inventory& inventory,
    const ModelIndex& r0_index) {
    R0Reproduction result;
    result.directory = directory(ala_path);
    result.stem = stem(ala_path);
    for (const structural::InventoryModel& model : inventory.models) {
        if (directory(model.record.canonical_path) != result.directory) continue;
        ++result.same_directory_models;
        if (!model.skeleton) ++result.same_directory_unparsed;
    }
    result.qualifying = r0_qualifying_models(ala_path, r0_index);
    return result;
}

// ---------------------------------------------------------------------------
// Row evaluation.
// ---------------------------------------------------------------------------

struct Candidate final {
    audit::CandidateResult result;
    std::string directory_relation;
};

struct Row final {
    audit::BaselineFailure baseline;
    std::string status;
    // Always true: R0 still names no model, so the model_match failure stands
    // whatever is discovered.
    bool original_failure_retained{};
    R0Reproduction r0;
    std::string clip_motion;
    std::size_t track_count{};
    std::size_t requirement_count{};
    std::optional<std::uint32_t> max_track_bone_index;
    std::uint32_t stored_frame_count{};
    std::uint32_t playable_frame_count{};
    float frames_per_second{};
    float duration_seconds{};
    std::size_t shortlisted{};
    std::size_t evaluated{};
    std::size_t compatible{};
    std::size_t rejected_binding{};
    std::size_t rejected_sampling{};
    std::vector<Candidate> candidates;
};

struct RowResult final {
    std::optional<Row> row;
    std::string error;
};

// Evaluates one selected failure.  Refuses (error set, no row) unless the row
// is the frozen model_match failure, the effective ALA is the frozen identity,
// R0 still names no model, the indexed shortlist equals the brute-force
// reference, every shortlisted model the frozen metadata records still has
// that identity, and every evaluation agrees with strict Player.
[[nodiscard]] inline RowResult evaluate_row(const audit::FrozenMetadata& frozen, const audit::BaselineFailure& baseline,
    const assets::Animation& clip, const audit::AssetIdentity& effective_ala, const structural::Inventory& inventory,
    const structural::StructuralIndex& index, const ModelIndex& r0_index) {
    RowResult result;
    const auto refuse = [&result, &baseline](const std::string& reason) {
        result.row.reset();
        result.error = "failure " + std::to_string(baseline.baseline_failure_index) + " (" + baseline.animation.path + "): " + reason;
        return result;
    };
    const auto frozen_row = frozen.failures.find(baseline.baseline_failure_index);
    if (frozen_row == frozen.failures.end()) return refuse("not in the frozen metadata");
    if (const auto field = audit::failure_difference(frozen_row->second, baseline))
        return refuse("baseline drift: " + *field + " differs from frozen metadata");
    if (const auto difference = original_failure_difference(baseline)) return refuse("baseline drift: " + *difference);
    if (const auto field = audit::identity_difference(baseline.animation, effective_ala))
        return refuse("effective ALA " + *field + " differs from frozen metadata");

    Row row;
    row.baseline = baseline;
    row.r0 = reproduce_r0(baseline.animation.path, inventory, r0_index);
    if (!row.r0.qualifying.empty())
        return refuse("baseline drift: R0 now names " + row.r0.qualifying.front() + " (the model_match failure no longer reproduces)");
    row.original_failure_retained = true;
    row.clip_motion = audit::clip_motion(clip);
    row.track_count = clip.tracks.size();
    row.requirement_count = structural::requirements(clip).size();
    for (const auto& track : clip.tracks)
        row.max_track_bone_index = std::max(row.max_track_bone_index.value_or(0U), track.bone_index);
    row.stored_frame_count = clip.stored_frame_count;
    row.playable_frame_count = clip.playable_frame_count;
    row.frames_per_second = clip.frames_per_second;
    row.duration_seconds = clip.duration_seconds;

    const std::vector<std::size_t> indexed = structural::shortlist(index, clip);
    if (indexed != structural::brute_force_shortlist(inventory, clip))
        return refuse("indexed shortlist differs from the brute-force reference");
    row.shortlisted = indexed.size();
    for (const std::size_t item : indexed) {
        const structural::InventoryModel& model = inventory.models[item];
        const std::string& path = model.record.canonical_path;
        const audit::CandidateModelView view = structural::view_of(model);
        std::string frozen_status;
        if (frozen.models.contains(path)) {
            const auto recorded = audit::check_model_identity(frozen, path, view, false);
            if (!recorded.drift.empty()) return refuse("discovered " + recorded.drift);
            frozen_status = recorded.status;
        } else {
            frozen_status = std::string(frozen.is_unrecorded(path) ? audit::frozen_identity::unrecorded
                                                                   : structural::not_in_frozen_metadata);
        }
        Candidate candidate;
        candidate.result = audit::evaluate_candidate(clip, view);
        candidate.result.frozen_identity = std::move(frozen_status);
        candidate.directory_relation = std::string(directory(path) == row.r0.directory ? directory_relation::same
                                                                                        : directory_relation::other);
        const auto& outcome = candidate.result;
        if (!outcome.diagnosis || !outcome.consistent_with_player)
            return refuse("candidate " + path + " diagnosis disagrees with strict Player");
        const bool structural_failure = outcome.range_impossible
            || std::any_of(outcome.diagnosis->failures.begin(), outcome.diagnosis->failures.end(), [](const auto& failure) {
                   return failure.predicate == predicates::bone_index_in_range || failure.predicate == predicates::bone_name_equal;
               });
        if (structural_failure) return refuse("shortlisted candidate " + path + " fails an exact bone index/name requirement");
        ++row.evaluated;
        if (outcome.status == audit::candidate_status::compatible_unapproved) ++row.compatible;
        else if (outcome.status == audit::candidate_status::rejected && outcome.rejection_stage == "binding") ++row.rejected_binding;
        else if (outcome.status == audit::candidate_status::rejected && outcome.rejection_stage == "sampling") ++row.rejected_sampling;
        else return refuse("candidate " + path + " was not evaluated");
        row.candidates.push_back(std::move(candidate));
    }
    row.status = structural::classify(row.compatible);
    result.row = std::move(row);
    return result;
}

// ---------------------------------------------------------------------------
// Whole diagnosis over an effective VFS.
// ---------------------------------------------------------------------------

struct Diagnosis final {
    std::vector<Row> rows;
    std::string error;
};

// For each selected row: re-runs R0 over every effective model path, reads
// the ALA through the effective VFS, re-checks its complete frozen identity
// before parsing, then evaluates the row.
[[nodiscard]] inline Diagnosis diagnose(const audit::FrozenMetadata& frozen, const std::vector<audit::BaselineFailure>& selected,
    const vfs::Vfs& effective, const structural::Inventory& inventory, const structural::StructuralIndex& index) {
    Diagnosis diagnosis;
    const auto refuse = [&diagnosis](std::string reason) {
        diagnosis.rows.clear();
        diagnosis.error = std::move(reason);
        return std::move(diagnosis);
    };
    if (!inventory.error.empty()) return refuse(inventory.error);
    const ModelIndex r0_index = r0_index_of(inventory);
    std::set<std::size_t> seen;
    for (const audit::BaselineFailure& baseline : selected) {
        const std::string label = "failure " + std::to_string(baseline.baseline_failure_index) + " (" + baseline.animation.path + ")";
        if (!seen.insert(baseline.baseline_failure_index).second) return refuse(label + " is selected twice");
        const auto frozen_row = frozen.failures.find(baseline.baseline_failure_index);
        if (frozen_row == frozen.failures.end()) return refuse(label + " is not in the frozen metadata");
        if (const auto field = audit::failure_difference(frozen_row->second, baseline))
            return refuse(label + ": baseline drift: " + *field + " differs from frozen metadata");
        // The original failure is reproduced before the clip is touched.
        const auto r0 = r0_qualifying_models(baseline.animation.path, r0_index);
        if (!r0.empty())
            return refuse(label + ": baseline drift: R0 now names " + r0.front() + " (the model_match failure no longer reproduces)");
        auto record = effective.stat(baseline.animation.path);
        auto bytes = effective.open(baseline.animation.path);
        if (!record || !bytes) return refuse(label + ": effective ALA did not open");
        if (bytes.value().size() != record.value().size) return refuse(label + ": effective ALA read differs from its record size");
        const audit::AssetIdentity ala = sources::identity_of(record.value(), sources::sha256_of(bytes.value()));
        if (const auto field = audit::identity_difference(frozen_row->second.animation, ala))
            return refuse(label + ": " + (*field == "sha256" ? std::string("hash") : std::string("provenance"))
                + " drift: effective ALA " + *field + " differs from frozen metadata");
        auto clip = assets::load_animation(bytes.value(), assets::source_from(record.value()));
        if (!clip) return refuse(label + ": effective ALA no longer parses");
        auto row = evaluate_row(frozen, baseline, clip.value(), ala, inventory, index, r0_index);
        if (!row.row) return refuse(row.error);
        diagnosis.rows.push_back(std::move(*row.row));
    }
    return diagnosis;
}

// ---------------------------------------------------------------------------
// Receipt.
// ---------------------------------------------------------------------------

struct Header final {
    std::string profile{"remake-effective"};
    std::string tool_name{"animation_corpus_model_match_discovery"};
    std::string compiler;
    std::string config;
    std::vector<std::pair<std::string, std::string>> tool_sources;
    std::vector<audit::MountSummary> mounts;
    std::string frozen_metadata_sha256;
    std::uint64_t frozen_metadata_bytes{};
    std::size_t animation_count{};
    std::size_t playback_passed{};
    std::size_t failure_count{};
};

struct Summary final {
    std::map<std::string, std::size_t> rows_by_status;
    std::map<std::string, std::size_t> inventory_by_status;
    std::map<std::string, std::size_t> candidates_by_directory_relation;
    std::size_t rows{};
    std::size_t original_failures_retained{};
    std::size_t r0_absent_reproduced{};
    std::size_t rows_with_structural_candidates{};
    std::size_t shortlisted_pairs{};
    std::size_t evaluated_pairs{};
    std::size_t compatible_pairs{};
    std::size_t rejected_binding_pairs{};
    std::size_t rejected_sampling_pairs{};
    std::size_t max_shortlist{};
    std::size_t distinct_structural_models{};
    std::size_t distinct_compatible_models{};
};

[[nodiscard]] inline Summary summarize(const std::vector<Row>& rows, const structural::Inventory& inventory) {
    Summary summary;
    for (const std::string_view status : {row_status::zero_compatible, row_status::single_compatible, row_status::multiple_compatible})
        summary.rows_by_status[std::string(status)] = 0;
    for (const std::string_view status : {model_status::parsed, model_status::parse_failed, model_status::read_failed})
        summary.inventory_by_status[std::string(status)] = 0;
    for (const std::string_view relation : {directory_relation::same, directory_relation::other})
        summary.candidates_by_directory_relation[std::string(relation)] = 0;
    for (const structural::InventoryModel& model : inventory.models) ++summary.inventory_by_status[model.status];
    std::set<std::string> structural_models;
    std::set<std::string> compatible_models;
    summary.rows = rows.size();
    for (const Row& row : rows) {
        ++summary.rows_by_status[row.status];
        if (row.original_failure_retained) ++summary.original_failures_retained;
        if (row.r0.qualifying.empty()) ++summary.r0_absent_reproduced;
        if (row.shortlisted > 0) ++summary.rows_with_structural_candidates;
        summary.shortlisted_pairs += row.shortlisted;
        summary.evaluated_pairs += row.evaluated;
        summary.compatible_pairs += row.compatible;
        summary.rejected_binding_pairs += row.rejected_binding;
        summary.rejected_sampling_pairs += row.rejected_sampling;
        summary.max_shortlist = std::max(summary.max_shortlist, row.shortlisted);
        for (const Candidate& candidate : row.candidates) {
            structural_models.insert(candidate.result.identity.path);
            ++summary.candidates_by_directory_relation[candidate.directory_relation];
            if (candidate.result.status == audit::candidate_status::compatible_unapproved)
                compatible_models.insert(candidate.result.identity.path);
        }
    }
    summary.distinct_structural_models = structural_models.size();
    summary.distinct_compatible_models = compatible_models.size();
    return summary;
}

// Rows are written sorted by ALA path (then frozen index), candidates by model
// path, so the receipt is independent of input and evaluation order.
inline void write_receipt(std::ostream& output, const Header& header, const structural::Inventory& inventory,
    std::vector<Row> rows) {
    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
        return left.baseline.animation.path != right.baseline.animation.path
            ? left.baseline.animation.path < right.baseline.animation.path
            : left.baseline.baseline_failure_index < right.baseline.baseline_failure_index;
    });
    for (Row& row : rows)
        std::sort(row.candidates.begin(), row.candidates.end(), [](const Candidate& left, const Candidate& right) {
            return left.result.identity.path != right.result.identity.path ? left.result.identity.path < right.result.identity.path
                                                                           : left.result.identity.source_id < right.result.identity.source_id;
        });
    const Summary summary = summarize(rows, inventory);
    output << "{\n  \"schema\": " << json_string(schema) << ",\n  \"schema_version\": " << schema_version
           << ",\n  \"profile\": " << json_string(header.profile) << ",\n  \"match_rule\": " << json_string(structural::match_rule)
           << ",\n  \"r0_rule\": " << json_string(r0_rule_id)
           << ",\n  \"diagnosis_only\": true,\n  \"association_approved\": false,\n  \"associations_promoted\": 0"
           << ",\n  \"ledger_mutated\": false,\n  \"corpus_acceptance_claimed\": false,\n  \"retail_behaviour_claimed\": false"
           << ",\n  \"search\": {\"exact_case_sensitive\": true, \"every_track_required\": true, \"extra_bones_allowed\": true"
           << ", \"aliases\": false, \"renaming\": false, \"index_shift\": false, \"reordering\": false, \"retargeting\": false"
           << ", \"bone_count_equality\": false, \"shortlist_cap\": null, \"shadow_versions_searched\": false"
           << ", \"brute_force_reference_checked\": true, \"r0_reproduced_over_all_effective_models\": true"
           << ", \"ala_identity_checked_before_parse\": true}"
           << ",\n  \"baseline\": {\"ledger_schema\": \"eawr.animation-playback-corpus\", \"ledger_schema_version\": 1"
           << ", \"pinned_ledger_sha256\": " << json_string(audit::pinned_frozen_ledger_sha256)
           << ", \"animation_count\": " << header.animation_count << ", \"playback_passed\": " << header.playback_passed
           << ", \"failure_count\": " << header.failure_count << "},\n  \"frozen_metadata\": {\"schema\": "
           << json_string(audit::frozen_metadata_schema)
           << ", \"pinned_sha256\": " << json_string(audit::pinned_frozen_metadata_sha256)
           << ", \"sha256\": " << json_string(header.frozen_metadata_sha256)
           << ", \"bytes\": " << header.frozen_metadata_bytes << "},\n  \"selection\": {\"stage\": "
           << json_string(model_match_stage) << ", \"code\": " << json_string(model_match_code)
           << ", \"cause\": " << json_string(model_match_cause) << ", \"rows\": " << rows.size() << ", \"failure_indices\": [";
    std::vector<std::size_t> indices;
    for (const Row& row : rows) indices.push_back(row.baseline.baseline_failure_index);
    std::sort(indices.begin(), indices.end());
    for (std::size_t item = 0; item < indices.size(); ++item) output << (item == 0 ? "" : ", ") << indices[item];
    output << "]},\n  \"tool\": {\"name\": " << json_string(header.tool_name) << ", \"compiler\": " << json_string(header.compiler)
           << ", \"config\": " << json_string(header.config) << ", \"source_sha256\": {";
    for (std::size_t item = 0; item < header.tool_sources.size(); ++item)
        output << (item == 0 ? "" : ", ") << json_string(header.tool_sources[item].first) << ": "
               << json_string(header.tool_sources[item].second);
    output << "}},\n  \"mounts\": [";
    for (std::size_t item = 0; item < header.mounts.size(); ++item) {
        const auto& mount = header.mounts[item];
        output << (item == 0 ? "\n" : ",\n") << "    {\"layer_id\": " << json_string(mount.layer_id)
               << ", \"manifest_source_id\": " << json_string(mount.manifest_source_id) << ", \"active_archives\": [";
        for (std::size_t archive = 0; archive < mount.active_archives.size(); ++archive)
            output << (archive == 0 ? "" : ", ") << json_string(mount.active_archives[archive]);
        output << "]}";
    }
    output << (header.mounts.empty() ? "" : "\n  ") << "],\n  \"model_inventory\": {\"effective_models\": "
           << inventory.models.size() << ", \"by_status\": ";
    audit::write_counts(output, summary.inventory_by_status);
    output << ", \"raw_versions\": " << inventory.raw_versions
           << ", \"shadowed_versions_excluded\": " << inventory.shadowed_versions_excluded
           << ", \"entries_sha256\": " << json_string(structural::inventory_sha256(inventory)) << ", \"exclusions\": [";
    bool first_exclusion = true;
    for (const structural::InventoryModel& model : inventory.models) {
        if (model.skeleton) continue;
        output << (first_exclusion ? "\n    " : ",\n    ") << structural::inventory_entry_json(model);
        first_exclusion = false;
    }
    output << (first_exclusion ? "" : "\n  ") << "],\n  \"entries\": [";
    for (std::size_t item = 0; item < inventory.models.size(); ++item)
        output << (item == 0 ? "\n    " : ",\n    ") << structural::inventory_entry_json(inventory.models[item]);
    output << (inventory.models.empty() ? "" : "\n  ") << "]},\n  \"counts\": {\"rows\": " << summary.rows
           << ", \"rows_by_status\": ";
    audit::write_counts(output, summary.rows_by_status);
    output << ", \"original_failures_retained\": " << summary.original_failures_retained
           << ", \"r0_absent_reproduced\": " << summary.r0_absent_reproduced
           << ", \"rows_with_structural_candidates\": " << summary.rows_with_structural_candidates
           << ", \"shortlisted_pairs\": " << summary.shortlisted_pairs
           << ", \"evaluated_pairs\": " << summary.evaluated_pairs
           << ", \"compatible_unapproved_pairs\": " << summary.compatible_pairs
           << ", \"rejected_binding_pairs\": " << summary.rejected_binding_pairs
           << ", \"rejected_sampling_pairs\": " << summary.rejected_sampling_pairs
           << ", \"max_shortlist\": " << summary.max_shortlist
           << ", \"distinct_structural_models\": " << summary.distinct_structural_models
           << ", \"distinct_compatible_models\": " << summary.distinct_compatible_models
           << ", \"candidates_by_directory_relation\": ";
    audit::write_counts(output, summary.candidates_by_directory_relation);
    output << "},\n  \"rows\": [";
    for (std::size_t item = 0; item < rows.size(); ++item) {
        const Row& row = rows[item];
        output << (item == 0 ? "\n" : ",\n") << "    {\"animation\": {";
        audit::write_identity(output, row.baseline.animation);
        output << "}, \"baseline_failure_index\": " << row.baseline.baseline_failure_index
               << ", \"original_failure\": {\"stage\": " << json_string(row.baseline.stage)
               << ", \"code\": " << json_string(row.baseline.code) << ", \"cause\": " << json_string(row.baseline.cause)
               << ", \"selected_model\": " << json_string(row.baseline.selected_model) << ", \"r0_candidates\": [";
        for (std::size_t candidate = 0; candidate < row.baseline.r0_candidates.size(); ++candidate)
            output << (candidate == 0 ? "" : ", ") << json_string(row.baseline.r0_candidates[candidate]);
        output << "]}, \"original_failure_retained\": " << (row.original_failure_retained ? "true" : "false")
               << ", \"r0_reproduction\": {\"directory\": " << json_string(row.r0.directory)
               << ", \"stem\": " << json_string(row.r0.stem)
               << ", \"same_directory_models\": " << row.r0.same_directory_models
               << ", \"same_directory_unparsed\": " << row.r0.same_directory_unparsed << ", \"qualifying\": [";
        for (std::size_t model = 0; model < row.r0.qualifying.size(); ++model)
            output << (model == 0 ? "" : ", ") << json_string(row.r0.qualifying[model]);
        output << "]}, \"status\": " << json_string(row.status) << ", \"approved\": false"
               << ", \"clip\": {\"motion\": " << json_string(row.clip_motion) << ", \"track_count\": " << row.track_count
               << ", \"requirement_count\": " << row.requirement_count
               << ", \"max_track_bone_index\": " << audit::json_opt(row.max_track_bone_index)
               << ", \"stored_frame_count\": " << row.stored_frame_count
               << ", \"playable_frame_count\": " << row.playable_frame_count
               << ", \"frames_per_second\": " << audit::json_float(row.frames_per_second)
               << ", \"duration_seconds\": " << audit::json_float(row.duration_seconds) << '}'
               << ", \"shortlist\": {\"structural_candidates\": " << row.shortlisted << ", \"evaluated\": " << row.evaluated
               << ", \"compatible_unapproved\": " << row.compatible << ", \"rejected_binding\": " << row.rejected_binding
               << ", \"rejected_sampling\": " << row.rejected_sampling << "}, \"candidates\": [";
        for (std::size_t candidate = 0; candidate < row.candidates.size(); ++candidate) {
            output << (candidate == 0 ? "" : ", ") << "{\"directory_relation\": "
                   << json_string(row.candidates[candidate].directory_relation) << ", \"result\": ";
            audit::write_candidate(output, row.candidates[candidate].result);
            output << '}';
        }
        output << "]}";
    }
    output << (rows.empty() ? "" : "\n  ") << "]\n}\n";
}

} // namespace eawr::tests::animation_corpus::model_match_discovery
