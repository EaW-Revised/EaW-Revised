#pragma once

inline constexpr std::string_view schema = "eawr.animation-declaration-evidence";
inline constexpr int schema_version = 1;
inline constexpr std::string_view link_rule =
    "land-model-anim-override: effective Land_Model_Name (else Model_Name) == candidate and "
    "effective Land_Model_Anim_Override_Name == frozen R0-selected set";

inline constexpr std::string_view pairs_schema = "eawr.animation-declaration-pairs";
// corpus_declaration_pairs.tsv, derived by corpus_declaration_pairs.py from the
// fixed runner's association receipt (751723ed...).
inline constexpr std::string_view pinned_pairs_sha256 =
    "1825184a33b68a7015208cfb397d24d64f823f09cd80add51de1d9767b79e803";
inline constexpr std::string_view pinned_pairs_receipt_sha256 =
    "751723ed8f86757121f805668501b9d40c99ccfe919b451225b64db1994d927b";
inline constexpr std::size_t pinned_pair_count = 63;
inline constexpr std::size_t pinned_priority_count = 58;

inline constexpr std::string_view override_tag = "Land_Model_Anim_Override_Name";

namespace disposition {
inline constexpr std::string_view evidence_bearing = "evidence_bearing";
inline constexpr std::string_view no_explicit_evidence = "no_explicit_evidence";
inline constexpr std::string_view catalog_unresolved = "catalog_unresolved";
inline constexpr std::string_view conflicting_evidence = "conflicting_evidence";
} // namespace disposition

namespace observation {
// The candidate is named in a model field, without the clip's set.
inline constexpr std::string_view model_only = "model_only";
// The candidate is the land model, but the override names another set.
inline constexpr std::string_view override_other_set = "override_other_set";
// The clip's set is the override of an object whose land model is not the candidate.
inline constexpr std::string_view set_on_other_model = "set_on_other_model";
// A non-winning definition names the candidate or the set.
inline constexpr std::string_view shadowed_definition = "shadowed_definition";
// A non-model field carries the clip or set name as a token (SFX, icon, text).
inline constexpr std::string_view text_mention = "text_mention";
} // namespace observation

using associations::AssetIdentity;
using associations::FrozenMetadata;

// ---------------------------------------------------------------------------
// Pinned pairs.
// ---------------------------------------------------------------------------

struct PinnedPair final {
    AssetIdentity animation;
    std::string selected_model;
    std::string selected_sha256;
    AssetIdentity candidate;
    bool priority{};
};

struct PinnedPairs final {
    std::string sha256;
    std::string receipt_sha256;
    std::string frozen_metadata_sha256;
    std::vector<PinnedPair> pairs;
};

struct PinnedPairsLoad final {
    std::optional<PinnedPairs> pairs;
    std::string error;
};

[[nodiscard]] inline PinnedPairsLoad parse_pinned_pairs(const std::string_view text,
    const std::size_t expected_pairs = pinned_pair_count, const std::size_t expected_priority = pinned_priority_count) {
    namespace frozen = associations::frozen_detail;
    PinnedPairsLoad load;
    const auto fail = [&load](std::string message) {
        load.error = "pinned pairs are malformed: " + std::move(message);
        return load;
    };
    if (text.empty() || text.back() != '\n') return fail("missing final newline");
    auto lines = frozen::split(text.substr(0, text.size() - 1), '\n');
    if (lines.size() < 4) return fail("header is incomplete");
    const auto header = frozen::split(lines[0], '\t');
    if (header.size() != 2 || header[0] != pairs_schema || header[1] != "1") return fail("unexpected schema line");
    const auto receipt = frozen::split(lines[1], '\t');
    if (receipt.size() != 3 || receipt[0] != "source" || receipt[1] != "association_receipt_sha256"
        || !frozen::is_sha256(receipt[2]))
        return fail("unexpected receipt source line");
    const auto metadata = frozen::split(lines[2], '\t');
    if (metadata.size() != 3 || metadata[0] != "source" || metadata[1] != "frozen_metadata_sha256"
        || !frozen::is_sha256(metadata[2]))
        return fail("unexpected frozen metadata source line");
    const auto counts = frozen::split(lines[3], '\t');
    const auto pair_count = counts.size() == 3 ? frozen::parse_count(counts[1]) : std::nullopt;
    const auto priority_count = counts.size() == 3 ? frozen::parse_count(counts[2]) : std::nullopt;
    if (counts.size() != 3 || counts[0] != "counts" || !pair_count || !priority_count)
        return fail("unexpected counts line");

    PinnedPairs pairs;
    pairs.receipt_sha256 = std::string(receipt[2]);
    pairs.frozen_metadata_sha256 = std::string(metadata[2]);
    std::size_t priority{};
    for (std::size_t index = 4; index < lines.size(); ++index) {
        const auto fields = frozen::split(lines[index], '\t');
        // pair, ALA identity (7), selected path, selected sha256, candidate identity (7), priority
        if (fields.size() != 18 || fields[0] != "pair") return fail("line " + std::to_string(index + 1));
        auto animation = frozen::parse_identity(fields, 1);
        auto candidate = frozen::parse_identity(fields, 10);
        if (!animation || !candidate || fields[8].empty() || !frozen::is_sha256(fields[9])
            || (fields[17] != "0" && fields[17] != "1"))
            return fail("line " + std::to_string(index + 1));
        PinnedPair pair{std::move(*animation), std::string(fields[8]), std::string(fields[9]),
            std::move(*candidate), fields[17] == "1"};
        if (pair.candidate.path == pair.selected_model) return fail("candidate is the selected model");
        if (!pairs.pairs.empty() && !(pairs.pairs.back().animation.path < pair.animation.path))
            return fail("pairs are not strictly sorted by ALA path");
        priority += pair.priority ? 1U : 0U;
        pairs.pairs.push_back(std::move(pair));
    }
    if (pairs.pairs.size() != *pair_count || priority != *priority_count)
        return fail("counts line disagrees with the pairs");
    if (pairs.pairs.size() != expected_pairs || priority != expected_priority)
        return fail("expected " + std::to_string(expected_pairs) + " pairs with " + std::to_string(expected_priority)
            + " priority, found " + std::to_string(pairs.pairs.size()) + " with " + std::to_string(priority));
    load.pairs = std::move(pairs);
    return load;
}

// Authenticates, then parses, the pinned pair list.
[[nodiscard]] inline PinnedPairsLoad load_pinned_pairs(const std::optional<std::string>& bytes,
    const std::optional<std::string>& sha256, const std::string_view pinned_sha256 = pinned_pairs_sha256,
    const std::size_t expected_pairs = pinned_pair_count, const std::size_t expected_priority = pinned_priority_count) {
    PinnedPairsLoad load;
    if (!bytes || bytes->empty() || !sha256 || sha256->empty()) {
        load.error = "pinned pairs are missing or unreadable";
        return load;
    }
    if (*sha256 != pinned_sha256) {
        load.error = "pinned pairs are stale: sha256 " + *sha256 + " is not pinned " + std::string(pinned_sha256);
        return load;
    }
    load = parse_pinned_pairs(*bytes, expected_pairs, expected_priority);
    if (load.pairs) load.pairs->sha256 = *sha256;
    return load;
}

// Every pair must still be the frozen failure it was derived from: same ALA
// identity, same selected model and hash, and a candidate that is an
// additional member of the ordered R0 list (with its run-4 hash, when run-4
// recorded one).
[[nodiscard]] inline std::optional<std::string> check_pairs_against_frozen(
    const PinnedPairs& pairs, const FrozenMetadata& frozen) {
    if (pairs.frozen_metadata_sha256 != frozen.sha256)
        return "pinned pairs name frozen metadata " + pairs.frozen_metadata_sha256 + ", not " + frozen.sha256;
    for (const PinnedPair& pair : pairs.pairs) {
        const auto failure = std::find_if(frozen.failures.begin(), frozen.failures.end(),
            [&pair](const auto& item) { return item.second.animation.path == pair.animation.path; });
        const std::string& path = pair.animation.path;
        if (failure == frozen.failures.end()) return path + ": not a frozen failure";
        const auto& row = failure->second;
        if (const auto field = associations::identity_difference(row.animation, pair.animation))
            return path + ": ALA " + *field + " differs from the frozen failure";
        if (row.selected_model != pair.selected_model) return path + ": selected model differs from the frozen failure";
        const auto selected = frozen.models.find(pair.selected_model);
        if (selected == frozen.models.end() || selected->second.identity.sha256 != pair.selected_sha256)
            return path + ": selected model hash differs from the frozen metadata";
        if (std::find(row.r0_candidates.begin(), row.r0_candidates.end(), pair.candidate.path) == row.r0_candidates.end())
            return path + ": candidate " + pair.candidate.path + " is not in the frozen R0 list";
        const auto candidate = frozen.models.find(pair.candidate.path);
        if (candidate != frozen.models.end()) {
            if (const auto field = associations::identity_difference(candidate->second.identity, pair.candidate))
                return path + ": candidate " + *field + " differs from the frozen metadata";
        } else if (!frozen.is_unrecorded(pair.candidate.path)) {
            return path + ": candidate " + pair.candidate.path + " is neither recorded nor unrecorded";
        }
    }
    return std::nullopt;
}

// Current effective-VFS identity of a canonical path, or nullopt when the VFS
// cannot stat or open it.
using IdentityLookup = std::function<std::optional<AssetIdentity>(const std::string& canonical_path)>;

// Stale identities fail closed: the ALA, the selected model and the candidate
// must still hash to, and come from, what was pinned.  The pair list pins the
// selected model by path and hash only; its layer, origin, source ID, original
// path and size are bound to its run-4 record in the frozen metadata, which
// check_pairs_against_frozen has already tied to the pinned hash.
[[nodiscard]] inline std::optional<std::string> check_current_identities(
    const PinnedPairs& pairs, const FrozenMetadata& frozen, const IdentityLookup& lookup) {
    for (const PinnedPair& pair : pairs.pairs) {
        const auto animation = lookup(pair.animation.path);
        if (!animation) return pair.animation.path + ": ALA is missing from the effective VFS";
        if (const auto field = associations::identity_difference(pair.animation, *animation))
            return pair.animation.path + ": stale ALA " + *field;
        const auto recorded = frozen.models.find(pair.selected_model);
        if (recorded == frozen.models.end() || recorded->second.identity.path != pair.selected_model
            || recorded->second.identity.sha256 != pair.selected_sha256)
            return pair.animation.path + ": selected model has no frozen identity for the pinned hash";
        const auto selected = lookup(pair.selected_model);
        if (!selected) return pair.animation.path + ": selected model is missing from the effective VFS";
        if (const auto field = associations::identity_difference(recorded->second.identity, *selected))
            return pair.animation.path + ": stale selected model " + *field;
        const auto candidate = lookup(pair.candidate.path);
        if (!candidate) return pair.animation.path + ": candidate is missing from the effective VFS";
        if (const auto field = associations::identity_difference(pair.candidate, *candidate))
            return pair.animation.path + ": stale candidate " + *field;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Declared values.
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::string fold(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        const char folded = character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        result.push_back(folded == '\\' ? '/' : folded);
    }
    return result;
}

[[nodiscard]] inline std::string trimmed(const std::string_view value) {
    std::size_t first = 0;
    std::size_t last = value.size();
    const auto space = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (first < last && space(value[first])) ++first;
    while (last > first && space(value[last - 1])) --last;
    return std::string(value.substr(first, last - first));
}

// A declared model name as the canonical effective-VFS path the asset layer
// probes: folded, under data/art/models/, with the .alo suffix.  Empty for an
// empty declaration.  No other normalisation: a similar name stays different.
[[nodiscard]] inline std::string canonical_model_path(const std::string_view declared) {
    std::string name = fold(trimmed(declared));
    if (name.empty()) return {};
    if (!name.ends_with(".alo")) name += ".alo";
    return "data/art/models/" + name;
}

// data/art/models/<stem>.<ext> -> <stem>
[[nodiscard]] inline std::string path_stem(const std::string_view path) {
    const auto slash = path.find_last_of('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string_view::npos) name = name.substr(0, dot);
    return std::string(name);
}

[[nodiscard]] inline bool iequals(const std::string_view left, const std::string_view right) {
    return fold(left) == fold(right);
}

// Fields that name a model: Model_Name and every *_Model_Name tag.
[[nodiscard]] inline bool is_model_field(const std::string_view tag) {
    const std::string folded = fold(tag);
    return folded == "model_name" || folded.ends_with("_model_name");
}

[[nodiscard]] inline bool is_link_field(const std::string_view tag) {
    return is_model_field(tag) || iequals(tag, override_tag);
}

[[nodiscard]] inline std::string_view to_string(const data::ValueProvenance value) noexcept {
    switch (value) {
    case data::ValueProvenance::own: return "own";
    case data::ValueProvenance::inherited: return "inherited";
    case data::ValueProvenance::added: return "added";
    case data::ValueProvenance::overridden: return "overridden";
    case data::ValueProvenance::merged: return "merged";
    }
    return "own";
}

// SHA-256 of the effective bytes behind a catalog source's logical path.
using SourceHash = std::function<std::string(const std::string& logical_path)>;
