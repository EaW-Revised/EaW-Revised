#pragma once

// Offline, test-only exact-skeleton discovery for the frozen shocktrooper
// binding failures (P1-03 R2, issue #24).  For each of the 132 selected clips
// it asks which models of the whole effective VFS could satisfy the clip's
// track requirements exactly, whatever their file names:
//
//   - the inventory is every effective .alo winner (shadowed versions are
//     never candidates), each read and parsed exactly once; unreadable or
//     unparsable models are kept as visible exclusions;
//   - a model is a structural candidate when, for EVERY track of the clip,
//     track.bone_index < bone count and model.bones[track.bone_index].name is
//     byte-for-byte (case-sensitive) track.bone_name.  There is no alias,
//     renaming, case folding, index shift, reordering, retargeting, bone-count
//     equality or shortlist cap; untracked extra bones are allowed;
//   - the indexed shortlist must equal a brute-force scan of the inventory;
//   - every structural candidate is then evaluated with the unchanged
//     evaluate_candidate: strict Player::create, the diagnose_binding mirror
//     and four CPU samples.
//
// Diagnosis only.  A compatible model is "compatible_unapproved", a
// diagnostic candidate and never an association; the effective selected model
// is re-evaluated and must still fail binding with the frozen code and cause,
// so the original failure is retained on every row.  Nothing here changes
// selection, Player, parsers, the VFS, the v1 ledger or the receipts.

#include "corpus_associations.hpp"
#include "corpus_diagnostics.hpp"
#include "corpus_source_versions.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::tests::animation_corpus::structural_discovery {

namespace audit = associations;
namespace sources = source_versions;

inline constexpr std::string_view schema = "eawr.animation-shocktrooper-structural-discovery";
inline constexpr int schema_version = 1;
inline constexpr std::string_view match_rule = "R2-exact-skeleton-every-track-bone-index-and-name";

namespace model_status {
inline constexpr std::string_view parsed = "parsed";
inline constexpr std::string_view parse_failed = "parse_failed";
inline constexpr std::string_view read_failed = "read_failed";
} // namespace model_status

namespace row_status {
inline constexpr std::string_view zero_compatible = "zero_compatible";
inline constexpr std::string_view single_compatible = "single_compatible_unapproved";
inline constexpr std::string_view multiple_compatible = "multiple_compatible_unapproved";
} // namespace row_status

// A discovered candidate the frozen metadata does not name at all.
inline constexpr std::string_view not_in_frozen_metadata = "not_in_frozen_metadata";

// ---------------------------------------------------------------------------
// Model inventory.
// ---------------------------------------------------------------------------

struct InventoryModel final {
    vfs::AssetRecord record;
    // sha256 is empty when the bytes could not be read.
    audit::AssetIdentity identity;
    std::string status;
    // The parsed model reduced to what Player::create and diagnose_binding
    // read (source and bones); meshes are dropped so the whole corpus fits.
    std::optional<assets::Model> skeleton;
    std::string error_code;
    std::string error_message;
};

struct Inventory final {
    // One entry per effective path, strictly sorted by canonical path.
    std::vector<InventoryModel> models;
    std::size_t raw_versions{};
    std::size_t shadowed_versions_excluded{};
    std::string error;

    [[nodiscard]] const InventoryModel* find(const std::string& path) const {
        const auto found = std::lower_bound(models.begin(), models.end(), path,
            [](const InventoryModel& model, const std::string& value) { return model.record.canonical_path < value; });
        return found != models.end() && found->record.canonical_path == path ? &*found : nullptr;
    }
};

[[nodiscard]] inline assets::Model skeleton_of(assets::Model&& model) {
    assets::Model skeleton;
    skeleton.source = std::move(model.source);
    skeleton.bones = std::move(model.bones);
    return skeleton;
}

// Parses one effective model exactly once.  `bytes` is null when the read
// failed; `read_code`/`read_message` then carry the VFS diagnostic.
[[nodiscard]] inline InventoryModel make_inventory_model(const vfs::AssetRecord& record,
    const std::vector<std::byte>* bytes, std::string read_code = {}, std::string read_message = {}) {
    InventoryModel model;
    model.record = record;
    if (!bytes) {
        model.identity = sources::identity_of(record, {});
        model.status = std::string(model_status::read_failed);
        model.error_code = std::move(read_code);
        model.error_message = std::move(read_message);
        return model;
    }
    model.identity = sources::identity_of(record, sources::sha256_of(*bytes));
    auto loaded = assets::load_model(*bytes, assets::source_from(record));
    if (loaded) {
        model.status = std::string(model_status::parsed);
        model.skeleton = skeleton_of(std::move(loaded.value()));
    } else {
        model.status = std::string(model_status::parse_failed);
        model.error_code = loaded.error().code;
        model.error_message = loaded.error().message;
    }
    return model;
}

// Every effective .alo winner of `effective`, each read and parsed once.
// Enumeration records must be the stat winners; shadowed versions are counted
// but never read or searched.
[[nodiscard]] inline Inventory build_inventory(const vfs::Vfs& effective) {
    Inventory inventory;
    const auto refuse = [&inventory](std::string reason) {
        inventory.models.clear();
        inventory.error = std::move(reason);
        return std::move(inventory);
    };
    auto records = effective.enumerate({}, ".alo");
    auto raw = effective.enumerate_raw({}, ".alo");
    if (!records || !raw) return refuse("model enumeration failed: " + (!records ? records.error().message : raw.error().message));
    if (raw.value().size() < records.value().size()) return refuse("raw model enumeration is smaller than the effective one");
    for (const vfs::AssetRecord& record : records.value()) {
        if (!inventory.models.empty() && !(inventory.models.back().record.canonical_path < record.canonical_path))
            return refuse("effective model enumeration is not strictly sorted at " + record.canonical_path);
        auto winner = effective.stat(record.canonical_path);
        if (!winner || !(winner.value() == record))
            return refuse("effective enumeration record of " + record.canonical_path + " is not the stat winner");
        auto bytes = effective.open(record.canonical_path);
        if (bytes && bytes.value().size() != record.size)
            return refuse("effective read of " + record.canonical_path + " differs from its record size");
        inventory.models.push_back(bytes ? make_inventory_model(record, &bytes.value())
                                         : make_inventory_model(record, nullptr, bytes.error().code, bytes.error().message));
    }
    inventory.raw_versions = raw.value().size();
    inventory.shadowed_versions_excluded = raw.value().size() - records.value().size();
    return inventory;
}

// One inventory entry as the receipt writes it; the inventory digest is the
// SHA-256 of these lines, each followed by LF, in path order.
[[nodiscard]] inline std::string inventory_entry_json(const InventoryModel& model) {
    std::ostringstream output;
    output << '{';
    audit::write_identity(output, model.identity);
    output << ", \"status\": " << json_string(model.status) << ", \"bone_count\": "
           << (model.skeleton ? std::to_string(model.skeleton->bones.size()) : std::string("null"))
           << ", \"error_code\": " << json_string(model.error_code)
           << ", \"error_message\": " << json_string(model.error_message) << '}';
    return output.str();
}

[[nodiscard]] inline std::string inventory_sha256(const Inventory& inventory) {
    std::string text;
    for (const InventoryModel& model : inventory.models) text += inventory_entry_json(model) + "\n";
    return sources::sha256_of(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

[[nodiscard]] inline audit::CandidateModelView view_of(const InventoryModel& model) {
    audit::CandidateModelView view;
    view.found = model.status != model_status::read_failed;
    view.identity = model.identity;
    view.model = model.skeleton ? &*model.skeleton : nullptr;
    view.error_code = model.error_code;
    view.error_message = model.error_message;
    return view;
}

// ---------------------------------------------------------------------------
// Exact structural shortlist.
// ---------------------------------------------------------------------------

using Requirement = std::pair<std::uint32_t, std::string>;

// The distinct exact (bone index, bone name) pairs the clip's tracks require.
[[nodiscard]] inline std::set<Requirement> requirements(const assets::Animation& clip) {
    std::set<Requirement> result;
    for (const auto& track : clip.tracks) result.emplace(track.bone_index, track.bone_name);
    return result;
}

// Posting lists: for every (bone index, bone name) any parsed model holds, the
// ascending inventory indices of the models that hold it.
struct StructuralIndex final {
    std::map<Requirement, std::vector<std::size_t>> postings;
    std::vector<std::size_t> parsed;
};

[[nodiscard]] inline StructuralIndex build_index(const Inventory& inventory) {
    StructuralIndex index;
    for (std::size_t model = 0; model < inventory.models.size(); ++model) {
        const auto& skeleton = inventory.models[model].skeleton;
        if (!skeleton) continue;
        index.parsed.push_back(model);
        for (std::size_t bone = 0; bone < skeleton->bones.size(); ++bone)
            index.postings[{static_cast<std::uint32_t>(bone), skeleton->bones[bone].name}].push_back(model);
    }
    return index;
}

// Every parsed model satisfying every requirement: the intersection of the
// requirements' posting lists.  A clip with no track requires nothing.
[[nodiscard]] inline std::vector<std::size_t> shortlist(const StructuralIndex& index, const assets::Animation& clip) {
    const auto required = requirements(clip);
    if (required.empty()) return index.parsed;
    std::vector<const std::vector<std::size_t>*> lists;
    for (const Requirement& requirement : required) {
        const auto found = index.postings.find(requirement);
        if (found == index.postings.end()) return {};
        lists.push_back(&found->second);
    }
    std::sort(lists.begin(), lists.end(), [](const auto* left, const auto* right) { return left->size() < right->size(); });
    std::vector<std::size_t> result = *lists.front();
    for (std::size_t list = 1; list < lists.size() && !result.empty(); ++list) {
        std::vector<std::size_t> next;
        std::set_intersection(result.begin(), result.end(), lists[list]->begin(), lists[list]->end(), std::back_inserter(next));
        result = std::move(next);
    }
    return result;
}

// The reference the index must equal: a direct scan of every parsed model.
[[nodiscard]] inline std::vector<std::size_t> brute_force_shortlist(const Inventory& inventory, const assets::Animation& clip) {
    std::vector<std::size_t> result;
    for (std::size_t model = 0; model < inventory.models.size(); ++model) {
        const auto& skeleton = inventory.models[model].skeleton;
        if (!skeleton) continue;
        const bool every = std::all_of(clip.tracks.begin(), clip.tracks.end(), [&skeleton](const assets::AnimationTrack& track) {
            return track.bone_index < skeleton->bones.size() && skeleton->bones[track.bone_index].name == track.bone_name;
        });
        if (every) result.push_back(model);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Row evaluation.
// ---------------------------------------------------------------------------

struct Candidate final {
    audit::CandidateResult result;
    bool selected_model{};
    bool r0_candidate{};
    bool bytes_identical_to_selected{};
};

struct Row final {
    audit::BaselineFailure baseline;
    std::string status;
    // Always true: the effective selected model is re-evaluated and must still
    // fail binding with the frozen code and cause, whatever is discovered.
    bool effective_failure_retained{};
    std::string clip_motion;
    std::size_t track_count{};
    std::size_t requirement_count{};
    audit::CandidateResult effective;
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

[[nodiscard]] inline std::string classify(const std::size_t compatible) {
    if (compatible == 0) return std::string(row_status::zero_compatible);
    return std::string(compatible == 1 ? row_status::single_compatible : row_status::multiple_compatible);
}

// Evaluates one selected failure.  Refuses (error set, no row) unless the
// baseline is the frozen one, the effective ALA and selected model are the
// frozen identities, the selected model still fails binding with the frozen
// code and cause, the indexed shortlist equals the brute-force reference, and
// every discovered model the frozen metadata records still has that identity.
[[nodiscard]] inline RowResult evaluate_row(const audit::FrozenMetadata& frozen, const audit::BaselineFailure& baseline,
    const assets::Animation& clip, const audit::AssetIdentity& effective_ala, const Inventory& inventory,
    const StructuralIndex& index) {
    RowResult result;
    const auto refuse = [&result, &baseline](const std::string& reason) {
        result.row.reset();
        result.error = "failure " + std::to_string(baseline.baseline_failure_index) + " (" + baseline.animation.path + "): " + reason;
        return result;
    };
    const auto frozen_row = frozen.failures.find(baseline.baseline_failure_index);
    if (frozen_row == frozen.failures.end()) return refuse("not in the frozen metadata");
    if (const auto field = audit::failure_difference(frozen_row->second, baseline))
        return refuse(*field + " differs from frozen metadata");
    if (baseline.stage != "binding") return refuse("stage is not binding");
    if (const auto field = audit::identity_difference(baseline.animation, effective_ala))
        return refuse("effective ALA " + *field + " differs from frozen metadata");
    const InventoryModel* selected = inventory.find(baseline.selected_model);
    if (!selected) return refuse("selected model is not in the effective inventory");
    const audit::CandidateModelView selected_view = view_of(*selected);
    const auto identity = audit::check_model_identity(frozen, baseline.selected_model, selected_view, true);
    if (!identity.drift.empty()) return refuse("selected " + identity.drift);

    Row row;
    row.baseline = baseline;
    row.clip_motion = audit::clip_motion(clip);
    row.track_count = clip.tracks.size();
    row.requirement_count = requirements(clip).size();
    row.effective = audit::evaluate_candidate(clip, selected_view);
    row.effective.frozen_identity = identity.status;
    if (row.effective.status != audit::candidate_status::rejected || row.effective.rejection_stage != "binding"
        || row.effective.player_code != baseline.code || row.effective.player_message != baseline.cause)
        return refuse("effective model no longer fails binding with the frozen code and cause");
    if (!row.effective.consistent_with_player) return refuse("effective diagnosis disagrees with strict Player");
    row.effective_failure_retained = true;

    const std::vector<std::size_t> indexed = shortlist(index, clip);
    if (indexed != brute_force_shortlist(inventory, clip)) return refuse("indexed shortlist differs from the brute-force reference");
    row.shortlisted = indexed.size();
    for (const std::size_t item : indexed) {
        const InventoryModel& model = inventory.models[item];
        const std::string& path = model.record.canonical_path;
        const audit::CandidateModelView view = view_of(model);
        std::string frozen_status;
        if (frozen.models.contains(path)) {
            const auto recorded = audit::check_model_identity(frozen, path, view, false);
            if (!recorded.drift.empty()) return refuse("discovered " + recorded.drift);
            frozen_status = recorded.status;
        } else {
            frozen_status = std::string(frozen.is_unrecorded(path) ? audit::frozen_identity::unrecorded : not_in_frozen_metadata);
        }
        Candidate candidate;
        candidate.result = audit::evaluate_candidate(clip, view);
        candidate.result.frozen_identity = std::move(frozen_status);
        candidate.selected_model = path == baseline.selected_model;
        candidate.r0_candidate = std::find(baseline.r0_candidates.begin(), baseline.r0_candidates.end(), path)
            != baseline.r0_candidates.end();
        candidate.bytes_identical_to_selected = model.identity.sha256 == selected->identity.sha256;
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
    row.status = classify(row.compatible);
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

[[nodiscard]] inline Diagnosis diagnose(const audit::FrozenMetadata& frozen, const std::vector<audit::BaselineFailure>& selected,
    const vfs::Vfs& effective, const Inventory& inventory, const StructuralIndex& index) {
    Diagnosis diagnosis;
    const auto refuse = [&diagnosis](std::string reason) {
        diagnosis.rows.clear();
        diagnosis.error = std::move(reason);
        return std::move(diagnosis);
    };
    if (!inventory.error.empty()) return refuse(inventory.error);
    std::set<std::size_t> seen;
    for (const audit::BaselineFailure& baseline : selected) {
        if (!seen.insert(baseline.baseline_failure_index).second)
            return refuse("failure " + std::to_string(baseline.baseline_failure_index) + " is selected twice");
        auto record = effective.stat(baseline.animation.path);
        auto bytes = effective.open(baseline.animation.path);
        if (!record || !bytes) return refuse("effective ALA " + baseline.animation.path + " did not open");
        const audit::AssetIdentity ala = sources::identity_of(record.value(), sources::sha256_of(bytes.value()));
        if (ala.sha256 != baseline.animation.sha256)
            return refuse("hash drift: effective ALA " + baseline.animation.path + " differs from frozen metadata");
        auto clip = assets::load_animation(bytes.value(), assets::source_from(record.value()));
        if (!clip) return refuse("effective ALA " + baseline.animation.path + " no longer parses");
        auto row = evaluate_row(frozen, baseline, clip.value(), ala, inventory, index);
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
    std::string tool_name{"animation_corpus_structural_discovery"};
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
    std::map<std::string, std::size_t> rows_by_model;
    std::map<std::string, std::size_t> rows_by_status;
    std::map<std::string, std::map<std::string, std::size_t>> rows_by_model_and_status;
    std::map<std::string, std::size_t> inventory_by_status;
    std::size_t rows{};
    std::size_t effective_failures_retained{};
    std::size_t rows_with_structural_candidates{};
    std::size_t shortlisted_pairs{};
    std::size_t evaluated_pairs{};
    std::size_t compatible_pairs{};
    std::size_t rejected_binding_pairs{};
    std::size_t rejected_sampling_pairs{};
    std::size_t max_shortlist{};
    std::size_t distinct_structural_models{};
    std::size_t distinct_compatible_models{};
    std::size_t pairs_outside_r0{};
};

[[nodiscard]] inline Summary summarize(const std::vector<Row>& rows, const Inventory& inventory) {
    Summary summary;
    for (const std::string_view status : {row_status::zero_compatible, row_status::single_compatible, row_status::multiple_compatible})
        summary.rows_by_status[std::string(status)] = 0;
    for (const std::string_view status : {model_status::parsed, model_status::parse_failed, model_status::read_failed})
        summary.inventory_by_status[std::string(status)] = 0;
    for (const InventoryModel& model : inventory.models) ++summary.inventory_by_status[model.status];
    std::set<std::string> structural;
    std::set<std::string> compatible;
    summary.rows = rows.size();
    for (const Row& row : rows) {
        ++summary.rows_by_model[row.baseline.selected_model];
        ++summary.rows_by_status[row.status];
        ++summary.rows_by_model_and_status[row.baseline.selected_model][row.status];
        if (row.effective_failure_retained) ++summary.effective_failures_retained;
        if (row.shortlisted > 0) ++summary.rows_with_structural_candidates;
        summary.shortlisted_pairs += row.shortlisted;
        summary.evaluated_pairs += row.evaluated;
        summary.compatible_pairs += row.compatible;
        summary.rejected_binding_pairs += row.rejected_binding;
        summary.rejected_sampling_pairs += row.rejected_sampling;
        summary.max_shortlist = std::max(summary.max_shortlist, row.shortlisted);
        for (const Candidate& candidate : row.candidates) {
            structural.insert(candidate.result.identity.path);
            if (candidate.result.status == audit::candidate_status::compatible_unapproved)
                compatible.insert(candidate.result.identity.path);
            if (!candidate.r0_candidate) ++summary.pairs_outside_r0;
        }
    }
    summary.distinct_structural_models = structural.size();
    summary.distinct_compatible_models = compatible.size();
    return summary;
}

// Rows are written sorted by ALA path (then frozen index), candidates by model
// path, so the receipt is independent of input and evaluation order.
inline void write_receipt(std::ostream& output, const Header& header, const Inventory& inventory, std::vector<Row> rows) {
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
           << ",\n  \"profile\": " << json_string(header.profile) << ",\n  \"match_rule\": " << json_string(match_rule)
           << ",\n  \"diagnosis_only\": true,\n  \"association_approved\": false,\n  \"associations_promoted\": 0"
           << ",\n  \"ledger_mutated\": false,\n  \"corpus_acceptance_claimed\": false,\n  \"retail_behaviour_claimed\": false"
           << ",\n  \"search\": {\"exact_case_sensitive\": true, \"every_track_required\": true, \"extra_bones_allowed\": true"
           << ", \"aliases\": false, \"renaming\": false, \"index_shift\": false, \"reordering\": false, \"retargeting\": false"
           << ", \"bone_count_equality\": false, \"shortlist_cap\": null, \"shadow_versions_searched\": false"
           << ", \"brute_force_reference_checked\": true}"
           << ",\n  \"baseline\": {\"ledger_schema\": \"eawr.animation-playback-corpus\", \"ledger_schema_version\": 1"
           << ", \"pinned_ledger_sha256\": " << json_string(audit::pinned_frozen_ledger_sha256)
           << ", \"animation_count\": " << header.animation_count << ", \"playback_passed\": " << header.playback_passed
           << ", \"failure_count\": " << header.failure_count << "},\n  \"frozen_metadata\": {\"schema\": "
           << json_string(audit::frozen_metadata_schema)
           << ", \"pinned_sha256\": " << json_string(audit::pinned_frozen_metadata_sha256)
           << ", \"sha256\": " << json_string(header.frozen_metadata_sha256)
           << ", \"bytes\": " << header.frozen_metadata_bytes << "},\n  \"tool\": {\"name\": "
           << json_string(header.tool_name) << ", \"compiler\": " << json_string(header.compiler)
           << ", \"config\": " << json_string(header.config) << ", \"source_sha256\": {";
    for (std::size_t index = 0; index < header.tool_sources.size(); ++index)
        output << (index == 0 ? "" : ", ") << json_string(header.tool_sources[index].first) << ": "
               << json_string(header.tool_sources[index].second);
    output << "}},\n  \"mounts\": [";
    for (std::size_t index = 0; index < header.mounts.size(); ++index) {
        const auto& mount = header.mounts[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"layer_id\": " << json_string(mount.layer_id)
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
           << ", \"entries_sha256\": " << json_string(inventory_sha256(inventory)) << ", \"exclusions\": [";
    bool first_exclusion = true;
    for (const InventoryModel& model : inventory.models) {
        if (model.skeleton) continue;
        output << (first_exclusion ? "\n    " : ",\n    ") << inventory_entry_json(model);
        first_exclusion = false;
    }
    output << (first_exclusion ? "" : "\n  ") << "],\n  \"entries\": [";
    for (std::size_t index = 0; index < inventory.models.size(); ++index)
        output << (index == 0 ? "\n    " : ",\n    ") << inventory_entry_json(inventory.models[index]);
    output << (inventory.models.empty() ? "" : "\n  ") << "]},\n  \"counts\": {\"rows\": " << summary.rows
           << ", \"rows_by_selected_model\": ";
    audit::write_counts(output, summary.rows_by_model);
    output << ", \"rows_by_status\": ";
    audit::write_counts(output, summary.rows_by_status);
    output << ", \"rows_by_selected_model_and_status\": {";
    bool first_model = true;
    for (const auto& [model, counts] : summary.rows_by_model_and_status) {
        output << (first_model ? "" : ", ") << json_string(model) << ": ";
        audit::write_counts(output, counts);
        first_model = false;
    }
    output << "}, \"effective_failures_retained\": " << summary.effective_failures_retained
           << ", \"rows_with_structural_candidates\": " << summary.rows_with_structural_candidates
           << ", \"shortlisted_pairs\": " << summary.shortlisted_pairs
           << ", \"evaluated_pairs\": " << summary.evaluated_pairs
           << ", \"compatible_unapproved_pairs\": " << summary.compatible_pairs
           << ", \"rejected_binding_pairs\": " << summary.rejected_binding_pairs
           << ", \"rejected_sampling_pairs\": " << summary.rejected_sampling_pairs
           << ", \"max_shortlist\": " << summary.max_shortlist
           << ", \"distinct_structural_models\": " << summary.distinct_structural_models
           << ", \"distinct_compatible_models\": " << summary.distinct_compatible_models
           << ", \"pairs_outside_r0\": " << summary.pairs_outside_r0 << "},\n  \"rows\": [";
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const Row& row = rows[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"animation\": {";
        audit::write_identity(output, row.baseline.animation);
        output << "}, \"baseline_failure_index\": " << row.baseline.baseline_failure_index
               << ", \"baseline_stage\": " << json_string(row.baseline.stage)
               << ", \"baseline_code\": " << json_string(row.baseline.code)
               << ", \"baseline_cause\": " << json_string(row.baseline.cause)
               << ", \"selected_model\": " << json_string(row.baseline.selected_model) << ", \"r0_candidates\": [";
        for (std::size_t item = 0; item < row.baseline.r0_candidates.size(); ++item)
            output << (item == 0 ? "" : ", ") << json_string(row.baseline.r0_candidates[item]);
        output << "], \"status\": " << json_string(row.status)
               << ", \"effective_failure_retained\": " << (row.effective_failure_retained ? "true" : "false")
               << ", \"approved\": false"
               << ", \"clip_motion\": " << json_string(row.clip_motion) << ", \"track_count\": " << row.track_count
               << ", \"requirement_count\": " << row.requirement_count << ", \"effective_model\": ";
        audit::write_candidate(output, row.effective);
        output << ", \"shortlist\": {\"structural_candidates\": " << row.shortlisted << ", \"evaluated\": " << row.evaluated
               << ", \"compatible_unapproved\": " << row.compatible << ", \"rejected_binding\": " << row.rejected_binding
               << ", \"rejected_sampling\": " << row.rejected_sampling << "}, \"candidates\": [";
        for (std::size_t item = 0; item < row.candidates.size(); ++item) {
            const Candidate& candidate = row.candidates[item];
            output << (item == 0 ? "" : ", ") << "{\"selected_model\": " << (candidate.selected_model ? "true" : "false")
                   << ", \"r0_candidate\": " << (candidate.r0_candidate ? "true" : "false")
                   << ", \"bytes_identical_to_selected\": " << (candidate.bytes_identical_to_selected ? "true" : "false")
                   << ", \"result\": ";
            audit::write_candidate(output, candidate.result);
            output << '}';
        }
        output << "]}";
    }
    output << (rows.empty() ? "" : "\n  ") << "]\n}\n";
}

} // namespace eawr::tests::animation_corpus::structural_discovery
