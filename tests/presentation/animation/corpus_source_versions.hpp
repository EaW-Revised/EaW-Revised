#pragma once

// Offline, test-only exact-source diagnosis of the frozen shocktrooper binding
// failures (P1-03 R4).  For the two selected model paths only, it lists every
// source version the effective VFS holds for that exact path (the winner and
// every shadowed layer/archive copy), reads each version's own bytes through
// an isolated single-source view, and evaluates the effective clip against
// each version with the unchanged strict Player predicates and four CPU
// samples.  It also lists every source version of the 132 exact ALA paths, for
// identity metadata only.
//
// Diagnosis only.  Nothing here changes selection, Player, parsers, the VFS,
// the v1 ledger or the association receipts.  A shadowed model version that
// binds is "shadow_compatible_unapproved": the effective (winning) model still
// fails, so the baseline failure is retained on every row.  No sibling, alias
// or other path is searched.

#include "corpus_associations.hpp"
#include "corpus_diagnostics.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace eawr::tests::animation_corpus::source_versions {

namespace audit = associations;

inline constexpr std::string_view schema = "eawr.animation-shocktrooper-source-versions";
inline constexpr int schema_version = 1;

struct TargetModel final {
    std::string_view path;
    std::size_t expected_rows{};
};

// The bounded R4 slice: exactly these two selected models and row counts.
inline constexpr std::array<TargetModel, 2> shocktrooper_targets{{
    {"data/art/models/ei_shocktrooper.alo", 70},
    {"data/art/models/ei_shocktrooper_heavy.alo", 62},
}};
inline constexpr std::size_t shocktrooper_rows = 132;

namespace shadow_status {
inline constexpr std::string_view compatible_unapproved = "shadow_compatible_unapproved";
inline constexpr std::string_view rejected = "shadow_rejected";
inline constexpr std::string_view parse_failed = "shadow_parse_failed";
} // namespace shadow_status

namespace row_status {
inline constexpr std::string_view no_shadow = "no_shadow";
inline constexpr std::string_view compatible_unapproved = shadow_status::compatible_unapproved;
inline constexpr std::string_view rejected = shadow_status::rejected;
inline constexpr std::string_view parse_failed = shadow_status::parse_failed;
} // namespace row_status

// How an archive version is mounted by its layer: named by MegaFiles.xml, or
// one of the engine's fixed SFX/patch slots.  Empty for loose versions.
namespace archive_slot {
inline constexpr std::string_view manifest_declared = "manifest_declared";
inline constexpr std::string_view conventional = "conventional_slot";
} // namespace archive_slot

// ---------------------------------------------------------------------------
// Row selection.
// ---------------------------------------------------------------------------

struct Selection final {
    std::vector<audit::BaselineFailure> rows;
    std::string error;
};

// Exactly the frozen binding failures whose selected model is a target, in
// frozen index order.  Any count or stage mismatch refuses.
[[nodiscard]] inline Selection select_rows(const audit::FrozenMetadata& frozen,
    const std::span<const TargetModel> targets = shocktrooper_targets) {
    Selection selection;
    std::map<std::string, std::size_t> counts;
    for (const TargetModel& target : targets) counts[std::string(target.path)] = 0;
    for (const auto& [index, failure] : frozen.failures) {
        const auto found = counts.find(failure.selected_model);
        if (found == counts.end()) continue;
        if (failure.stage != "binding") {
            selection.error = "frozen failure " + std::to_string(index) + " selects " + failure.selected_model
                + " but its stage is " + failure.stage + ", not binding";
            selection.rows.clear();
            return selection;
        }
        ++found->second;
        selection.rows.push_back(failure);
    }
    for (const TargetModel& target : targets) {
        const std::size_t count = counts[std::string(target.path)];
        if (count != target.expected_rows) {
            selection.error = "frozen metadata holds " + std::to_string(count) + " binding failures for "
                + std::string(target.path) + ", not " + std::to_string(target.expected_rows);
            selection.rows.clear();
            return selection;
        }
    }
    return selection;
}

// ---------------------------------------------------------------------------
// Exact-source reads.
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::string sha256_of(const std::span<const std::byte> bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

[[nodiscard]] inline audit::AssetIdentity identity_of(const vfs::AssetRecord& record, std::string sha256) {
    return {record.canonical_path, std::move(sha256), record.layer_id, std::string(vfs::to_string(record.origin)),
        record.source_id, record.original_path, record.size};
}

// Everything that distinguishes one source version from another.  Two
// versions with identical bytes but different sources stay distinct.
[[nodiscard]] inline std::string identity_key(const audit::AssetIdentity& identity) {
    std::string key;
    for (const std::string* field : {&identity.path, &identity.sha256, &identity.layer_id, &identity.origin,
             &identity.source_id, &identity.original_path}) {
        key += *field;
        key.push_back('\x1f');
    }
    return key + std::to_string(identity.size);
}

struct ExactRead final {
    std::optional<std::vector<std::byte>> bytes;
    std::string archive_slot;
    std::string error;
};

// Reads exactly one source version, never the winner by accident.  Vfs::open
// returns the highest-precedence version, so each read goes through a view
// holding only that version's source:
//   - archive: the layer's own matching ArchiveSpec, alone, over an empty
//     scratch data root (so no loose file or other archive can win);
//   - loose: the layer's original data root with no active archives.
// The view must report exactly the original AssetRecord, field for field,
// before anything is opened; anything else fails closed.
class ExactSourceReader final {
public:
    ExactSourceReader(std::vector<vfs::ManifestResolution> layers, std::filesystem::path empty_root)
        : layers_(std::move(layers)), empty_root_(std::move(empty_root)) {}

    [[nodiscard]] ExactRead read(const vfs::AssetRecord& record) {
        ExactRead result;
        const auto refuse = [&result, &record](const std::string& reason) {
            result.bytes.reset();
            result.error = "ambiguous source identity for " + record.canonical_path + " (" + record.source_id + "): " + reason;
            return result;
        };
        const vfs::ManifestResolution* layer = nullptr;
        for (const auto& candidate : layers_) {
            if (candidate.mount.layer_id != record.layer_id) continue;
            if (layer) return refuse("layer id is not unique");
            layer = &candidate;
        }
        if (!layer) return refuse("layer is not mounted");

        std::string key;
        vfs::MountSpec spec;
        spec.layer_id = layer->mount.layer_id;
        spec.loose_logical_prefix = layer->mount.loose_logical_prefix;
        if (record.origin == vfs::AssetOrigin::loose) {
            if (record.source_id != record.layer_id + ":loose:" + record.original_path)
                return refuse("loose source id does not name its original path");
            key = "loose\x1f" + record.layer_id;
            spec.data_root = layer->mount.data_root;
        } else {
            const vfs::ArchiveSpec* archive = nullptr;
            for (const auto& candidate : layer->mount.active_archives) {
                if (candidate.source_id != record.source_id) continue;
                if (archive) return refuse("more than one active archive has this source id");
                archive = &candidate;
            }
            if (!archive) return refuse("no active archive of the layer has this source id");
            key = "archive\x1f" + record.layer_id + "\x1f" + record.source_id;
            spec.data_root = empty_root_;
            spec.active_archives = {*archive};
            result.archive_slot = std::string(declared(*layer, record.source_id) ? archive_slot::manifest_declared
                                                                                  : archive_slot::conventional);
            std::error_code error;
            if (!std::filesystem::is_directory(empty_root_, error) || error
                || !std::filesystem::is_empty(empty_root_, error) || error)
                return refuse("scratch data root is not an empty directory");
        }

        auto [view, inserted] = views_.try_emplace(key);
        if (inserted) {
            const std::array<vfs::MountSpec, 1> specs{spec};
            auto mounted = vfs::Vfs::mount(specs);
            if (mounted) view->second.view = std::move(mounted.value());
            else view->second.error = mounted.error().code + ": " + mounted.error().message;
        }
        if (!view->second.view) return refuse("isolated view did not mount: " + view->second.error);
        const vfs::Vfs& isolated = *view->second.view;
        auto candidates = isolated.candidates(record.canonical_path);
        if (!candidates) return refuse("isolated view does not hold the path");
        if (candidates.value().size() != 1) return refuse("isolated view holds more than one version");
        auto stat = isolated.stat(record.canonical_path);
        if (!stat || !(stat.value() == record)) return refuse("isolated view record differs from the original AssetRecord");
        auto bytes = isolated.open(record.canonical_path);
        if (!bytes) return refuse("isolated read failed: " + bytes.error().code + ": " + bytes.error().message);
        if (bytes.value().size() != record.size) return refuse("isolated read size differs from the record");
        result.bytes = std::move(bytes.value());
        return result;
    }

private:
    struct View final {
        std::optional<vfs::Vfs> view;
        std::string error;
    };

    [[nodiscard]] static bool declared(const vfs::ManifestResolution& layer, const std::string& source_id) {
        const std::string prefix = layer.mount.layer_id + ":";
        if (!source_id.starts_with(prefix)) return false;
        const auto relative = vfs::canonicalize(std::string_view(source_id).substr(prefix.size()));
        if (!relative) return false;
        return std::any_of(layer.declared_archives.begin(), layer.declared_archives.end(), [&relative](const std::string& name) {
            const auto canonical = vfs::canonicalize(name);
            return canonical && canonical.value() == relative.value();
        });
    }

    std::vector<vfs::ManifestResolution> layers_;
    std::filesystem::path empty_root_;
    std::map<std::string, View> views_;
};

// One source version of an exact path.  Rank 0 is the effective winner.
struct SourceVersion final {
    std::size_t rank{};
    vfs::AssetRecord record;
    audit::AssetIdentity identity;
    std::string archive_slot;
    std::vector<std::byte> bytes;
};

struct VersionList final {
    std::vector<SourceVersion> versions;
    std::string error;
};

// Every source version of exactly `path`, in effective precedence order, each
// read through its own isolated view.  The winner must be rank 0 with the
// effective VFS's own record and bytes.  `keep_bytes` false keeps identity
// metadata only.
[[nodiscard]] inline VersionList enumerate_versions(const vfs::Vfs& effective, ExactSourceReader& reader,
    const std::string& path, const bool keep_bytes) {
    VersionList list;
    const auto refuse = [&list](std::string reason) {
        list.versions.clear();
        list.error = std::move(reason);
        return std::move(list);
    };
    auto candidates = effective.candidates(path);
    if (!candidates) return refuse(path + " is not in the effective VFS: " + candidates.error().message);
    const auto& records = candidates.value();
    for (std::size_t left = 0; left < records.size(); ++left)
        for (std::size_t right = left + 1; right < records.size(); ++right)
            if (records[left] == records[right]) return refuse("ambiguous source identity: " + path + " lists "
                + records[left].source_id + " twice");
    for (std::size_t rank = 0; rank < records.size(); ++rank) {
        auto read = reader.read(records[rank]);
        if (!read.bytes) return refuse(read.error);
        SourceVersion version;
        version.rank = rank;
        version.record = records[rank];
        version.identity = identity_of(records[rank], sha256_of(*read.bytes));
        version.archive_slot = std::move(read.archive_slot);
        if (keep_bytes) version.bytes = std::move(*read.bytes);
        list.versions.push_back(std::move(version));
    }
    auto winner = effective.stat(path);
    auto winner_bytes = effective.open(path);
    if (!winner || !winner_bytes || !(winner.value() == list.versions.front().record)
        || sha256_of(winner_bytes.value()) != list.versions.front().identity.sha256)
        return refuse("hash drift: " + path + " effective winner is not the rank-0 exact source");
    return list;
}

// ---------------------------------------------------------------------------
// Row evaluation (VFS-free).
// ---------------------------------------------------------------------------

struct SourceIdentity final {
    std::size_t rank{};
    audit::AssetIdentity identity;
    std::string archive_slot;
};

// One loaded model source version: the view evaluate_candidate consumes.
struct ModelSource final {
    SourceIdentity source;
    audit::CandidateModelView view;
};

struct ShadowResult final {
    SourceIdentity source;
    std::string status;
    bool bytes_identical_to_effective{};
    audit::CandidateResult result;
};

struct Row final {
    audit::BaselineFailure baseline;
    std::string status;
    // Always true: the effective model is re-evaluated and must still fail
    // binding with the frozen code and cause, whatever a shadow does.
    bool effective_failure_retained{};
    std::string clip_motion;
    std::size_t track_count{};
    std::vector<SourceIdentity> ala_versions;
    SourceIdentity effective_model;
    audit::CandidateResult effective;
    std::vector<ShadowResult> shadows;
};

struct RowResult final {
    std::optional<Row> row;
    std::string error;
};

[[nodiscard]] inline std::string classify_shadows(const std::vector<ShadowResult>& shadows) {
    if (shadows.empty()) return std::string(row_status::no_shadow);
    const auto any = [&shadows](const std::string_view status) {
        return std::any_of(shadows.begin(), shadows.end(), [status](const ShadowResult& item) { return item.status == status; });
    };
    if (any(shadow_status::compatible_unapproved)) return std::string(row_status::compatible_unapproved);
    if (any(shadow_status::rejected)) return std::string(row_status::rejected);
    return std::string(row_status::parse_failed);
}

// Evaluates one selected failure.  Refuses (error set, no row) unless the
// baseline is the frozen one, the rank-0 ALA and model are the frozen
// effective identities, and the effective model still fails binding with the
// frozen code and cause.
[[nodiscard]] inline RowResult evaluate_row(const audit::FrozenMetadata& frozen, const audit::BaselineFailure& baseline,
    const assets::Animation& clip, const std::vector<SourceIdentity>& ala_versions,
    const std::vector<ModelSource>& model_versions) {
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
    if (ala_versions.empty()) return refuse("ALA has no source version");
    if (const auto field = audit::identity_difference(baseline.animation, ala_versions.front().identity))
        return refuse("effective ALA " + *field + " differs from frozen metadata");
    for (std::size_t rank = 0; rank < ala_versions.size(); ++rank)
        if (ala_versions[rank].rank != rank || ala_versions[rank].identity.path != baseline.animation.path)
            return refuse("ALA source versions are not the exact path in rank order");
    if (model_versions.empty()) return refuse("selected model has no source version");
    for (std::size_t rank = 0; rank < model_versions.size(); ++rank)
        if (model_versions[rank].source.rank != rank || model_versions[rank].source.identity.path != baseline.selected_model
            || model_versions[rank].view.identity.path != baseline.selected_model)
            return refuse("model source versions are not the exact path in rank order");
    const ModelSource& winner = model_versions.front();
    if (const auto field = audit::identity_difference(winner.source.identity, winner.view.identity))
        return refuse("effective model view " + *field + " differs from its source");
    const auto identity = audit::check_model_identity(frozen, baseline.selected_model, winner.view, true);
    if (!identity.drift.empty()) return refuse("effective " + identity.drift);

    Row row;
    row.baseline = baseline;
    row.clip_motion = audit::clip_motion(clip);
    row.track_count = clip.tracks.size();
    row.ala_versions = ala_versions;
    row.effective_model = winner.source;
    row.effective = audit::evaluate_candidate(clip, winner.view);
    row.effective.frozen_identity = identity.status;
    if (row.effective.status != audit::candidate_status::rejected || row.effective.rejection_stage != "binding"
        || row.effective.player_code != baseline.code || row.effective.player_message != baseline.cause)
        return refuse("effective model no longer fails binding with the frozen code and cause");
    if (!row.effective.consistent_with_player) return refuse("effective diagnosis disagrees with strict Player");
    row.effective_failure_retained = true;
    for (std::size_t rank = 1; rank < model_versions.size(); ++rank) {
        ShadowResult shadow;
        shadow.source = model_versions[rank].source;
        shadow.bytes_identical_to_effective = shadow.source.identity.sha256 == winner.source.identity.sha256;
        shadow.result = audit::evaluate_candidate(clip, model_versions[rank].view);
        if (shadow.result.diagnosis && !shadow.result.consistent_with_player)
            return refuse("shadow rank " + std::to_string(rank) + " diagnosis disagrees with strict Player");
        if (shadow.result.status == audit::candidate_status::compatible_unapproved)
            shadow.status = std::string(shadow_status::compatible_unapproved);
        else if (shadow.result.status == audit::candidate_status::rejected)
            shadow.status = std::string(shadow_status::rejected);
        else if (shadow.result.status == audit::candidate_status::parse_failed)
            shadow.status = std::string(shadow_status::parse_failed);
        else return refuse("shadow rank " + std::to_string(rank) + " was not read");
        row.shadows.push_back(std::move(shadow));
    }
    row.status = classify_shadows(row.shadows);
    result.row = std::move(row);
    return result;
}

// ---------------------------------------------------------------------------
// Whole diagnosis over an effective VFS.
// ---------------------------------------------------------------------------

struct ParsedModel final {
    SourceIdentity source;
    std::optional<assets::Model> model;
    std::string error_code;
    std::string error_message;
};

struct Diagnosis final {
    std::vector<Row> rows;
    // Every source version of each target model, keyed by model path.
    std::map<std::string, std::vector<ParsedModel>> model_sources;
    std::string error;
};

// Runs the bounded diagnosis.  Model versions are parsed once per full source
// identity (path, hash, layer, origin, source id, original path, size).
[[nodiscard]] inline Diagnosis diagnose(const audit::FrozenMetadata& frozen, const std::vector<audit::BaselineFailure>& selected,
    const vfs::Vfs& effective, ExactSourceReader& reader) {
    Diagnosis diagnosis;
    const auto refuse = [&diagnosis](std::string reason) {
        diagnosis.rows.clear();
        diagnosis.model_sources.clear();
        diagnosis.error = std::move(reason);
        return std::move(diagnosis);
    };
    std::map<std::string, ParsedModel> cache;
    std::map<std::string, std::vector<std::string>> model_keys;
    for (const audit::BaselineFailure& baseline : selected) {
        if (model_keys.contains(baseline.selected_model)) continue;
        auto versions = enumerate_versions(effective, reader, baseline.selected_model, true);
        if (!versions.error.empty()) return refuse(versions.error);
        auto& keys = model_keys[baseline.selected_model];
        for (SourceVersion& version : versions.versions) {
            const std::string key = identity_key(version.identity);
            keys.push_back(key);
            auto [entry, inserted] = cache.try_emplace(key);
            if (!inserted) return refuse("ambiguous source identity: " + baseline.selected_model + " repeats a version");
            ParsedModel& parsed = entry->second;
            parsed.source = {version.rank, version.identity, version.archive_slot};
            auto loaded = assets::load_model(version.bytes, assets::source_from(version.record));
            if (loaded) parsed.model = std::move(loaded.value());
            else { parsed.error_code = loaded.error().code; parsed.error_message = loaded.error().message; }
        }
    }

    for (const audit::BaselineFailure& baseline : selected) {
        auto ala = enumerate_versions(effective, reader, baseline.animation.path, false);
        if (!ala.error.empty()) return refuse(ala.error);
        std::vector<SourceIdentity> ala_versions;
        for (const SourceVersion& version : ala.versions)
            ala_versions.push_back({version.rank, version.identity, version.archive_slot});
        // The effective clip: the winner's bytes, already proven equal to the
        // rank-0 exact source.
        auto record = effective.stat(baseline.animation.path);
        auto bytes = effective.open(baseline.animation.path);
        if (!record || !bytes) return refuse("effective ALA " + baseline.animation.path + " did not re-open");
        if (sha256_of(bytes.value()) != baseline.animation.sha256)
            return refuse("hash drift: effective ALA " + baseline.animation.path + " differs from frozen metadata");
        auto clip = assets::load_animation(bytes.value(), assets::source_from(record.value()));
        if (!clip) return refuse("effective ALA " + baseline.animation.path + " no longer parses");

        std::vector<ModelSource> models;
        for (const std::string& key : model_keys.at(baseline.selected_model)) {
            const ParsedModel& parsed = cache.at(key);
            audit::CandidateModelView view;
            view.found = true;
            view.identity = parsed.source.identity;
            view.model = parsed.model ? &*parsed.model : nullptr;
            view.error_code = parsed.error_code;
            view.error_message = parsed.error_message;
            models.push_back({parsed.source, std::move(view)});
        }
        auto row = evaluate_row(frozen, baseline, clip.value(), ala_versions, models);
        if (!row.row) return refuse(row.error);
        diagnosis.rows.push_back(std::move(*row.row));
    }
    for (const auto& [path, keys] : model_keys)
        for (const std::string& key : keys) diagnosis.model_sources[path].push_back(cache.at(key));
    return diagnosis;
}

// ---------------------------------------------------------------------------
// Receipt.
// ---------------------------------------------------------------------------

struct Header final {
    std::string profile{"remake-effective"};
    std::string tool_name{"animation_corpus_source_versions"};
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
    std::map<std::string, std::size_t> shadow_pairs_by_status;
    std::size_t rows{};
    std::size_t effective_failures_retained{};
    std::size_t shadow_pairs{};
    std::size_t shadow_pairs_identical_bytes{};
    std::size_t ala_rows_with_shadows{};
    std::size_t ala_shadow_versions{};
};

[[nodiscard]] inline Summary summarize(const std::vector<Row>& rows) {
    Summary summary;
    for (const std::string_view status : {row_status::no_shadow, row_status::compatible_unapproved, row_status::rejected,
             row_status::parse_failed})
        summary.rows_by_status[std::string(status)] = 0;
    for (const std::string_view status : {shadow_status::compatible_unapproved, shadow_status::rejected,
             shadow_status::parse_failed})
        summary.shadow_pairs_by_status[std::string(status)] = 0;
    summary.rows = rows.size();
    for (const Row& row : rows) {
        ++summary.rows_by_model[row.baseline.selected_model];
        ++summary.rows_by_status[row.status];
        if (row.effective_failure_retained) ++summary.effective_failures_retained;
        if (row.ala_versions.size() > 1) ++summary.ala_rows_with_shadows;
        summary.ala_shadow_versions += row.ala_versions.empty() ? 0 : row.ala_versions.size() - 1;
        for (const ShadowResult& shadow : row.shadows) {
            ++summary.shadow_pairs;
            ++summary.shadow_pairs_by_status[shadow.status];
            if (shadow.bytes_identical_to_effective) ++summary.shadow_pairs_identical_bytes;
        }
    }
    return summary;
}

inline void write_source_fields(std::ostream& output, const SourceIdentity& source) {
    output << "\"rank\": " << source.rank << ", \"role\": " << json_string(source.rank == 0 ? "effective" : "shadow") << ", ";
    audit::write_identity(output, source.identity);
    output << ", \"archive_slot\": " << json_string(source.archive_slot);
}

inline void write_source(std::ostream& output, const SourceIdentity& source) {
    output << '{';
    write_source_fields(output, source);
    output << '}';
}

// Rows are written sorted by ALA path (then frozen index); versions in rank
// order, which is effective precedence, not input order.
inline void write_receipt(std::ostream& output, const Header& header,
    const std::map<std::string, std::vector<ParsedModel>>& model_sources, std::vector<Row> rows) {
    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
        return left.baseline.animation.path != right.baseline.animation.path
            ? left.baseline.animation.path < right.baseline.animation.path
            : left.baseline.baseline_failure_index < right.baseline.baseline_failure_index;
    });
    const Summary summary = summarize(rows);
    output << "{\n  \"schema\": " << json_string(schema) << ",\n  \"schema_version\": " << schema_version
           << ",\n  \"profile\": " << json_string(header.profile)
           << ",\n  \"diagnosis_only\": true,\n  \"association_approved\": false,\n  \"associations_promoted\": 0"
           << ",\n  \"ledger_mutated\": false,\n  \"corpus_acceptance_claimed\": false,\n  \"retail_behaviour_claimed\": false"
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
    output << (header.mounts.empty() ? "" : "\n  ") << "],\n  \"counts\": {\"rows\": " << summary.rows
           << ", \"rows_by_selected_model\": ";
    audit::write_counts(output, summary.rows_by_model);
    output << ", \"rows_by_status\": ";
    audit::write_counts(output, summary.rows_by_status);
    output << ", \"effective_failures_retained\": " << summary.effective_failures_retained
           << ", \"shadow_pairs\": " << summary.shadow_pairs << ", \"shadow_pairs_by_status\": ";
    audit::write_counts(output, summary.shadow_pairs_by_status);
    output << ", \"shadow_pairs_identical_bytes\": " << summary.shadow_pairs_identical_bytes
           << ", \"ala_rows_with_shadow_versions\": " << summary.ala_rows_with_shadows
           << ", \"ala_shadow_versions\": " << summary.ala_shadow_versions << "},\n  \"model_sources\": [";
    bool first_model = true;
    for (const auto& [path, versions] : model_sources) {
        output << (first_model ? "\n" : ",\n") << "    {\"path\": " << json_string(path) << ", \"versions\": [";
        first_model = false;
        for (std::size_t index = 0; index < versions.size(); ++index) {
            const ParsedModel& version = versions[index];
            output << (index == 0 ? "{" : ", {");
            write_source_fields(output, version.source);
            output << ", \"parsed\": " << (version.model ? "true" : "false")
                   << ", \"bone_count\": "
                   << (version.model ? std::to_string(version.model->bones.size()) : std::string("null"))
                   << ", \"error_code\": " << json_string(version.error_code)
                   << ", \"error_message\": " << json_string(version.error_message) << '}';
        }
        output << "]}";
    }
    output << (model_sources.empty() ? "" : "\n  ") << "],\n  \"rows\": [";
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const Row& row = rows[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"animation\": {";
        audit::write_identity(output, row.baseline.animation);
        output << "}, \"baseline_failure_index\": " << row.baseline.baseline_failure_index
               << ", \"baseline_stage\": " << json_string(row.baseline.stage)
               << ", \"baseline_code\": " << json_string(row.baseline.code)
               << ", \"baseline_cause\": " << json_string(row.baseline.cause)
               << ", \"selected_model\": " << json_string(row.baseline.selected_model)
               << ", \"status\": " << json_string(row.status)
               << ", \"effective_failure_retained\": " << (row.effective_failure_retained ? "true" : "false")
               << ", \"approved\": false"
               << ", \"clip_motion\": " << json_string(row.clip_motion) << ", \"track_count\": " << row.track_count
               << ", \"ala_versions\": [";
        for (std::size_t item = 0; item < row.ala_versions.size(); ++item) {
            output << (item == 0 ? "" : ", ");
            write_source(output, row.ala_versions[item]);
        }
        output << "], \"effective_model\": {\"source\": ";
        write_source(output, row.effective_model);
        output << ", \"result\": ";
        audit::write_candidate(output, row.effective);
        output << "}, \"shadows\": [";
        for (std::size_t item = 0; item < row.shadows.size(); ++item) {
            const ShadowResult& shadow = row.shadows[item];
            output << (item == 0 ? "" : ", ") << "{\"source\": ";
            write_source(output, shadow.source);
            output << ", \"status\": " << json_string(shadow.status)
                   << ", \"bytes_identical_to_effective\": " << (shadow.bytes_identical_to_effective ? "true" : "false")
                   << ", \"result\": ";
            audit::write_candidate(output, shadow.result);
            output << '}';
        }
        output << "]}";
    }
    output << (rows.empty() ? "" : "\n  ") << "]\n}\n";
}

} // namespace eawr::tests::animation_corpus::source_versions
