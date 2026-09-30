#pragma once

inline constexpr std::string_view schema = "eawr.animation-association-candidates";
inline constexpr int schema_version = 2;

// The only frozen v1 playback ledger this audit accepts as its baseline.
inline constexpr std::string_view pinned_frozen_ledger_sha256 =
    "d49aa6cd1d718de39d255f3eee4740a695cc6f13a1d9d64a870895a5f7bbd5a8";

// The only frozen metadata manifest this audit accepts: the tracked,
// metadata-only corpus_association_frozen.tsv, generated deterministically by
// corpus_association_frozen.py from the retained run-4 diagnostic receipt and
// its ledger.  It freezes what the v1 ledger does not: ALA provenance, the
// selected model's identity and the full R0 candidate list of every failure.
inline constexpr std::string_view frozen_metadata_schema = "eawr.animation-association-frozen-metadata";
inline constexpr std::string_view pinned_frozen_metadata_sha256 =
    "2c3456374eed6a5dff02cecbfd727c0ad0942ca8e7d932b8fb5142e52a5678e2";
inline constexpr std::string_view pinned_frozen_metadata_source_sha256 =
    "0bd26f2e297f1a571b32ab80c0b6beeb43395534988a49142391c5f9ed81e1f3";

// The bounded priority group named by the P1-03 association slice.
inline constexpr std::string_view priority_selected_model = "data/art/models/ei_armytrooper_heavy.alo";

namespace candidate_status {
inline constexpr std::string_view compatible_unapproved = "compatible_unapproved";
inline constexpr std::string_view rejected = "rejected";
inline constexpr std::string_view parse_failed = "parse_failed";
inline constexpr std::string_view missing = "missing";
} // namespace candidate_status

// How a candidate's current identity relates to the frozen metadata.
namespace frozen_identity {
inline constexpr std::string_view matched = "matched";
// Named by the frozen R0 list, but run-4 never recorded its identity because
// no row selected it.  Its identity is observed for the first time here.
inline constexpr std::string_view unrecorded = "unrecorded";
} // namespace frozen_identity

namespace row_status {
inline constexpr std::string_view single_compatible = "single_compatible_unapproved";
inline constexpr std::string_view ambiguous = "ambiguous_compatible_unapproved";
inline constexpr std::string_view no_compatible = "no_compatible_alternative";
inline constexpr std::string_view no_additional_candidate = "no_additional_candidate";
inline constexpr std::string_view retained_model_match = "retained_model_match";
inline constexpr std::string_view retained_parser_owned = "retained_parser_owned";
inline constexpr std::string_view retained_other = "retained_baseline_stage";
inline constexpr std::string_view baseline_drift = "baseline_drift";
} // namespace row_status

// ---------------------------------------------------------------------------
// Frozen-input gate.
// ---------------------------------------------------------------------------

// Returns an error when the frozen ledger is missing, is not the pinned v1
// ledger, or differs from the ledger this run re-emitted.  Any error means the
// audit must not write a receipt.
[[nodiscard]] inline std::optional<std::string> check_frozen_input(
    const std::optional<std::string>& frozen_sha256, const std::string_view fresh_sha256,
    const std::string_view pinned_sha256 = pinned_frozen_ledger_sha256) {
    if (!frozen_sha256 || frozen_sha256->empty()) return std::string("frozen ledger is missing or unreadable");
    if (*frozen_sha256 != pinned_sha256)
        return "frozen ledger is stale: sha256 " + *frozen_sha256 + " is not pinned " + std::string(pinned_sha256);
    if (fresh_sha256 != *frozen_sha256)
        return "baseline drift: re-emitted ledger sha256 " + std::string(fresh_sha256)
            + " differs from frozen " + *frozen_sha256;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Inputs.
// ---------------------------------------------------------------------------

// Metadata identity of an asset opened through the effective VFS.
struct AssetIdentity final {
    std::string path;
    std::string sha256;
    std::string layer_id;
    std::string origin;
    std::string source_id;
    std::string original_path;
    std::uint64_t size{};
};

// What the caller's effective-profile model cache holds for one candidate.
struct CandidateModelView final {
    // stat and open both succeeded through the effective VFS.
    bool found{};
    AssetIdentity identity;
    // Null when missing or when the ALO parser rejected the bytes.
    const assets::Model* model{};
    std::string error_code;
    std::string error_message;
};

using ModelLookup = std::function<CandidateModelView(const std::string& canonical_path)>;

// One frozen v1 failure, as re-derived by the unchanged baseline runner.
struct BaselineFailure final {
    AssetIdentity animation;
    std::size_t baseline_failure_index{};
    std::string stage;
    std::string code;
    std::string cause;
    std::string selected_model;
    // Every R0-qualifying model, longest stem first (baseline order).
    std::vector<std::string> r0_candidates;
};

// ---------------------------------------------------------------------------
// Frozen metadata.
// ---------------------------------------------------------------------------

struct FrozenModel final {
    AssetIdentity identity;
    // Null when run-4 could not parse the model.
    std::optional<std::size_t> bone_count;
};

// The parsed frozen metadata manifest.  Every R0 candidate that any frozen
// failure names is exactly one of `models` (identity recorded by run-4) or
// `unrecorded` (never selected, so run-4 holds no identity for it).
struct FrozenMetadata final {
    std::string sha256;
    std::uint64_t bytes{};
    std::string source_diagnostics_sha256;
    std::string source_ledger_sha256;
    std::size_t animation_count{};
    std::size_t playback_passed{};
    std::size_t failure_count{};
    std::map<std::size_t, BaselineFailure> failures;
    std::map<std::string, FrozenModel> models;
    std::vector<std::string> unrecorded;

    [[nodiscard]] bool is_unrecorded(const std::string& path) const {
        return std::binary_search(unrecorded.begin(), unrecorded.end(), path);
    }
};

struct FrozenMetadataLoad final {
    std::optional<FrozenMetadata> metadata;
    std::string error;
};

namespace frozen_detail {

[[nodiscard]] inline std::vector<std::string_view> split(const std::string_view text, const char separator) {
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index == text.size() || text[index] == separator) {
            parts.push_back(text.substr(begin, index - begin));
            begin = index + 1;
        }
    }
    return parts;
}

[[nodiscard]] inline bool is_sha256(const std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

// Canonical unsigned decimal: no sign, no leading zero, no overflow.
[[nodiscard]] inline std::optional<std::uint64_t> parse_count(const std::string_view value) {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) return std::nullopt;
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return result;
}

[[nodiscard]] inline std::optional<AssetIdentity> parse_identity(const std::vector<std::string_view>& fields,
    const std::size_t first) {
    // path, sha256, layer_id, origin, source_id, original_path, size
    const auto size = parse_count(fields[first + 6]);
    if (fields[first].empty() || !is_sha256(fields[first + 1]) || fields[first + 2].empty()
        || fields[first + 3].empty() || fields[first + 4].empty() || fields[first + 5].empty() || !size)
        return std::nullopt;
    return AssetIdentity{std::string(fields[first]), std::string(fields[first + 1]), std::string(fields[first + 2]),
        std::string(fields[first + 3]), std::string(fields[first + 4]), std::string(fields[first + 5]), *size};
}

} // namespace frozen_detail

// Strictly parses the manifest bytes.  Any deviation from the generator's
// canonical form is an error: this is authenticated input, not a format to be
// lenient with.
[[nodiscard]] inline FrozenMetadataLoad parse_frozen_metadata(const std::string_view text) {
    using namespace frozen_detail;
    FrozenMetadataLoad load;
    const auto refuse = [&load](const std::size_t line, const std::string& reason) {
        load.error = "frozen metadata is malformed: line " + std::to_string(line) + ": " + reason;
        load.metadata.reset();
        return load;
    };
    if (text.empty() || text.back() != '\n') return refuse(0, "missing final LF");
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte < 0x20U && character != '\t' && character != '\n') || byte == 0x7FU)
            return refuse(0, "control byte other than TAB/LF");
    }
    const auto lines = split(text.substr(0, text.size() - 1), '\n');
    if (lines.size() < 5) return refuse(lines.size(), "truncated header");
    FrozenMetadata metadata;
    const auto header = [&lines](const std::size_t index) { return split(lines[index], '\t'); };
    if (header(0) != std::vector<std::string_view>{frozen_metadata_schema, "1"}) return refuse(1, "schema");
    if (header(1) != std::vector<std::string_view>{"rule", r0_rule_id}) return refuse(2, "association rule");
    const auto diagnostics = header(2);
    const auto ledger = header(3);
    if (diagnostics.size() != 3 || diagnostics[0] != "source" || diagnostics[1] != "diagnostics_sha256"
        || !is_sha256(diagnostics[2]))
        return refuse(3, "diagnostics source");
    if (ledger.size() != 3 || ledger[0] != "source" || ledger[1] != "ledger_sha256" || !is_sha256(ledger[2]))
        return refuse(4, "ledger source");
    metadata.source_diagnostics_sha256 = std::string(diagnostics[2]);
    metadata.source_ledger_sha256 = std::string(ledger[2]);
    const auto counts = header(4);
    if (counts.size() != 4 || counts[0] != "counts") return refuse(5, "counts");
    const auto animations = parse_count(counts[1]);
    const auto passed = parse_count(counts[2]);
    const auto failed = parse_count(counts[3]);
    if (!animations || !passed || !failed || *passed + *failed != *animations) return refuse(5, "counts");
    metadata.animation_count = static_cast<std::size_t>(*animations);
    metadata.playback_passed = static_cast<std::size_t>(*passed);
    metadata.failure_count = static_cast<std::size_t>(*failed);

    // Sections appear in order: failures, then models, then unrecorded.
    int section = 0;
    std::vector<std::string> named;
    for (std::size_t index = 5; index < lines.size(); ++index) {
        const std::size_t line = index + 1;
        const auto fields = split(lines[index], '\t');
        if (fields[0] == "failure") {
            if (section > 0 || fields.size() < 13) return refuse(line, "failure record");
            BaselineFailure failure;
            const auto failure_index = parse_count(fields[1]);
            if (!failure_index || *failure_index != metadata.failures.size() + 1) return refuse(line, "failure index");
            failure.baseline_failure_index = static_cast<std::size_t>(*failure_index);
            failure.stage = std::string(fields[2]);
            failure.code = std::string(fields[3]);
            failure.cause = std::string(fields[4]);
            const auto animation = parse_identity(fields, 5);
            if (failure.stage.empty() || failure.code.empty() || failure.cause.empty() || !animation)
                return refuse(line, "failure identity");
            failure.animation = *animation;
            failure.selected_model = std::string(fields[12]);
            for (std::size_t item = 13; item < fields.size(); ++item) {
                if (fields[item].empty()) return refuse(line, "empty R0 candidate");
                failure.r0_candidates.emplace_back(fields[item]);
            }
            auto sorted = failure.r0_candidates;
            std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) return refuse(line, "duplicate R0 candidate");
            // Only a model_match failure has no selected model; any other
            // selected model is the first (longest-stem) R0 candidate.
            if (failure.selected_model.empty() != (failure.stage == "model_match"))
                return refuse(line, "selected model presence does not match the stage");
            if (!failure.selected_model.empty()
                && (failure.r0_candidates.empty() || failure.r0_candidates.front() != failure.selected_model))
                return refuse(line, "selected model is not the first R0 candidate");
            named.insert(named.end(), failure.r0_candidates.begin(), failure.r0_candidates.end());
            metadata.failures.emplace(failure.baseline_failure_index, std::move(failure));
        } else if (fields[0] == "model") {
            if (section > 1 || fields.size() != 9) return refuse(line, "model record");
            section = 1;
            const auto identity = parse_identity(fields, 1);
            if (!identity) return refuse(line, "model identity");
            FrozenModel model{*identity, std::nullopt};
            if (fields[8] != "null") {
                const auto bones = parse_count(fields[8]);
                if (!bones) return refuse(line, "bone count");
                model.bone_count = static_cast<std::size_t>(*bones);
            }
            if (!metadata.models.empty() && !(metadata.models.rbegin()->first < identity->path))
                return refuse(line, "models not strictly sorted");
            metadata.models.emplace(identity->path, std::move(model));
        } else if (fields[0] == "unrecorded") {
            if (fields.size() != 2 || fields[1].empty()) return refuse(line, "unrecorded record");
            section = 2;
            const std::string path(fields[1]);
            if (!metadata.unrecorded.empty() && !(metadata.unrecorded.back() < path))
                return refuse(line, "unrecorded not strictly sorted");
            if (metadata.models.contains(path)) return refuse(line, "unrecorded model has a recorded identity");
            metadata.unrecorded.push_back(path);
        } else {
            return refuse(line, "unknown record");
        }
    }
    if (metadata.failures.size() != metadata.failure_count) return refuse(lines.size(), "failure count");
    // Every named candidate is covered exactly once, and nothing else is.
    std::sort(named.begin(), named.end());
    named.erase(std::unique(named.begin(), named.end()), named.end());
    std::vector<std::string> covered = metadata.unrecorded;
    for (const auto& [path, model] : metadata.models) covered.push_back(path);
    std::sort(covered.begin(), covered.end());
    if (covered != named) return refuse(lines.size(), "model records do not cover exactly the named R0 candidates");
    load.metadata = std::move(metadata);
    return load;
}

// Authenticates, then parses, the frozen metadata.  `sha256` is the digest of
// exactly `bytes`, computed by the caller.  Any error means the audit must not
// write a receipt.
[[nodiscard]] inline FrozenMetadataLoad load_frozen_metadata(const std::optional<std::string>& bytes,
    const std::optional<std::string>& sha256, const std::string_view pinned_sha256 = pinned_frozen_metadata_sha256,
    const std::string_view pinned_source_sha256 = pinned_frozen_metadata_source_sha256,
    const std::string_view pinned_ledger_sha256 = pinned_frozen_ledger_sha256) {
    FrozenMetadataLoad load;
    if (!bytes || bytes->empty() || !sha256 || sha256->empty()) {
        load.error = "frozen metadata is missing or unreadable";
        return load;
    }
    if (*sha256 != pinned_sha256) {
        load.error = "frozen metadata is stale: sha256 " + *sha256 + " is not pinned " + std::string(pinned_sha256);
        return load;
    }
    load = parse_frozen_metadata(*bytes);
    if (!load.metadata) return load;
    if (load.metadata->source_diagnostics_sha256 != pinned_source_sha256
        || load.metadata->source_ledger_sha256 != pinned_ledger_sha256) {
        load.metadata.reset();
        load.error = "frozen metadata was not derived from the pinned run-4 diagnostics and ledger";
        return load;
    }
    load.metadata->sha256 = *sha256;
    load.metadata->bytes = bytes->size();
    return load;
}

// The first identity field that differs, or nothing.
[[nodiscard]] inline std::optional<std::string> identity_difference(const AssetIdentity& frozen, const AssetIdentity& current) {
    if (frozen.path != current.path) return std::string("path");
    if (frozen.sha256 != current.sha256) return std::string("sha256");
    if (frozen.layer_id != current.layer_id) return std::string("layer_id");
    if (frozen.origin != current.origin) return std::string("origin");
    if (frozen.source_id != current.source_id) return std::string("source_id");
    if (frozen.original_path != current.original_path) return std::string("original_path");
    if (frozen.size != current.size) return std::string("size");
    return std::nullopt;
}

// The first field in which a re-derived failure differs from its frozen
// record, or nothing.
[[nodiscard]] inline std::optional<std::string> failure_difference(const BaselineFailure& frozen, const BaselineFailure& current) {
    if (frozen.baseline_failure_index != current.baseline_failure_index) return std::string("baseline_failure_index");
    if (const auto field = identity_difference(frozen.animation, current.animation)) return "animation." + *field;
    if (frozen.stage != current.stage) return std::string("stage");
    if (frozen.code != current.code) return std::string("code");
    if (frozen.cause != current.cause) return std::string("cause");
    if (frozen.selected_model != current.selected_model) return std::string("selected_model");
    if (frozen.r0_candidates != current.r0_candidates) return std::string("r0_candidates");
    return std::nullopt;
}

// The re-derived failure set must be exactly the frozen one, field for field,
// before any failure is audited.
[[nodiscard]] inline std::optional<std::string> check_frozen_failures(
    const std::vector<BaselineFailure>& current, const FrozenMetadata& frozen) {
    if (current.size() != frozen.failures.size())
        return "baseline drift: " + std::to_string(current.size()) + " failures differ from frozen "
            + std::to_string(frozen.failures.size());
    std::vector<bool> seen(frozen.failures.size() + 1);
    for (const BaselineFailure& failure : current) {
        const auto found = frozen.failures.find(failure.baseline_failure_index);
        if (found == frozen.failures.end() || seen[failure.baseline_failure_index])
            return "baseline drift: failure index " + std::to_string(failure.baseline_failure_index)
                + " is not a unique frozen failure";
        seen[failure.baseline_failure_index] = true;
        if (const auto field = failure_difference(found->second, failure))
            return "baseline drift: failure " + std::to_string(failure.baseline_failure_index) + " ("
                + failure.animation.path + ") " + *field + " differs from frozen metadata";
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Results.
// ---------------------------------------------------------------------------
