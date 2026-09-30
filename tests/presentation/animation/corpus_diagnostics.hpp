#pragma once

// Offline, test-only diagnostics for the animation corpus runner.  Nothing in
// this header changes how Player binds a clip: it re-evaluates each predicate
// of Player::create independently so a rejected pair can be explained, and it
// also reports which track and message Player itself would stop on so the
// runner can prove the explanation agrees with the strict implementation.

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/animation/animation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace eawr::tests::animation_corpus {

// ---------------------------------------------------------------------------
// Receipt JSON strings.
// ---------------------------------------------------------------------------

// Quoted JSON string for ledger and receipt fields.  Quote, backslash and every
// byte below 0x20 are escaped (short form where JSON has one, otherwise
// \u00XX), so the output is strict and lossless.  All other bytes, including
// UTF-8 sequences, '/' and DEL, pass through unchanged.
[[nodiscard]] inline std::string json_string(const std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result{"\""};
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '\"': result.append("\\\""); break;
        case '\\': result.append("\\\\"); break;
        case '\b': result.append("\\b"); break;
        case '\f': result.append("\\f"); break;
        case '\n': result.append("\\n"); break;
        case '\r': result.append("\\r"); break;
        case '\t': result.append("\\t"); break;
        default:
            if (byte < 0x20U) {
                result.append("\\u00");
                result.push_back(hex[byte >> 4U]);
                result.push_back(hex[byte & 0x0FU]);
            } else {
                result.push_back(character);
            }
        }
    }
    result.push_back('\"');
    return result;
}

// ---------------------------------------------------------------------------
// R0: same-directory longest basename-prefix candidate (unchanged heuristic).
// ---------------------------------------------------------------------------

inline constexpr std::string_view r0_rule_id = "R0-same-directory-longest-underscore-prefix";

[[nodiscard]] inline std::string directory(const std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string{} : std::string(path.substr(0, slash + 1));
}

[[nodiscard]] inline std::string stem(const std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t begin = slash == std::string_view::npos ? 0 : slash + 1;
    const std::size_t dot = path.find_last_of('.');
    return std::string(path.substr(begin, dot == std::string_view::npos ? path.size() - begin : dot - begin));
}

// Directory (with trailing slash) -> (model stem, model canonical path).
using ModelIndex = std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>;

inline void add_model(ModelIndex& models, const std::string& canonical_path) {
    models[directory(canonical_path)].push_back({stem(canonical_path), canonical_path});
}

[[nodiscard]] inline bool r0_qualifies(const std::string_view animation_stem, const std::string_view model_stem) {
    return animation_stem.size() > model_stem.size() && animation_stem.starts_with(model_stem)
        && animation_stem[model_stem.size()] == '_';
}

[[nodiscard]] inline std::optional<std::string> matching_model(
    const std::string_view animation_path, const ModelIndex& models) {
    const auto found = models.find(directory(animation_path));
    if (found == models.end()) return std::nullopt;
    const std::string animation_stem = stem(animation_path);
    const std::pair<std::string, std::string>* best{};
    for (const auto& candidate : found->second) {
        if (!r0_qualifies(animation_stem, candidate.first)) continue;
        if (!best || candidate.first.size() > best->first.size()) best = &candidate;
    }
    return best ? std::optional(best->second) : std::nullopt;
}

// Every same-directory model that satisfies the R0 prefix test, longest stem
// first then path.  This is a candidate list only; it is not an association.
[[nodiscard]] inline std::vector<std::string> r0_qualifying_models(
    const std::string_view animation_path, const ModelIndex& models) {
    std::vector<std::pair<std::string, std::string>> qualifying;
    const auto found = models.find(directory(animation_path));
    if (found == models.end()) return {};
    const std::string animation_stem = stem(animation_path);
    for (const auto& candidate : found->second)
        if (r0_qualifies(animation_stem, candidate.first)) qualifying.push_back(candidate);
    std::sort(qualifying.begin(), qualifying.end(), [](const auto& left, const auto& right) {
        return left.first.size() != right.first.size() ? left.first.size() > right.first.size()
                                                         : left.second < right.second;
    });
    std::vector<std::string> result;
    result.reserve(qualifying.size());
    for (auto& candidate : qualifying) result.push_back(std::move(candidate.second));
    return result;
}

// ---------------------------------------------------------------------------
// Binding diagnosis mirroring Player::create.
// ---------------------------------------------------------------------------

namespace predicates {
inline constexpr std::string_view model_hierarchy = "model_hierarchy_parent_first";
inline constexpr std::string_view model_bind_finite = "model_bind_transform_finite";
inline constexpr std::string_view duration_metadata = "animation_duration_metadata";
inline constexpr std::string_view duration_consistent = "animation_duration_matches_frames";
inline constexpr std::string_view bone_index_in_range = "track_bone_index_in_range";
inline constexpr std::string_view bone_name_equal = "track_bone_name_equals_model_bone";
inline constexpr std::string_view sample_count = "track_sample_count_equals_stored_frames";
inline constexpr std::string_view unique_bone_index = "track_bone_index_unique";
inline constexpr std::string_view translation_interpolation = "track_translation_interpolation_supported";
inline constexpr std::string_view scale_interpolation = "track_scale_interpolation_supported";
inline constexpr std::string_view rotation_interpolation = "track_rotation_interpolation_supported";
inline constexpr std::string_view visibility_interpolation = "track_visibility_interpolation_step";
inline constexpr std::string_view samples_finite = "track_samples_finite";
} // namespace predicates

inline constexpr std::string_view compound_track_message =
    "animation track does not map exactly to model bone name/index";
inline constexpr std::string_view non_finite_track_message = "animation track has non-finite sample";

struct PredicateFailure final {
    std::string predicate;
    // Track fields are absent for model/metadata predicates.
    std::optional<std::size_t> track_ordinal;
    std::optional<std::uint32_t> track_bone_index;
    std::optional<std::string> track_bone_name;
    // Model bone name at track_bone_index; absent together with
    // index_out_of_range == true when the index is past the model hierarchy.
    std::optional<std::string> model_bone_name;
    bool index_out_of_range{};
    std::string actual;
    std::string expected;
};

struct BindingDiagnosis final {
    std::size_t model_bone_count{};
    std::size_t track_count{};
    // Every failed predicate, in evaluation order (model, metadata, then each
    // track's predicates in Player order).  Unlike Player this does not stop.
    std::vector<PredicateFailure> failures;
    // What strict Player::create is expected to do with the same inputs.
    bool player_accepts{true};
    std::string player_code;
    std::string player_message;
    std::optional<std::size_t> player_failing_track;
    std::optional<std::size_t> player_failing_bone;
};

[[nodiscard]] inline std::string_view interpolation_name(const assets::Interpolation value) {
    switch (value) {
    case assets::Interpolation::step: return "step";
    case assets::Interpolation::linear: return "linear";
    case assets::Interpolation::spherical: return "spherical";
    }
    return "unsupported";
}

[[nodiscard]] inline std::string interpolation_text(const assets::Interpolation value) {
    const std::string_view name = interpolation_name(value);
    if (name != "unsupported") return std::string(name);
    return "unsupported(" + std::to_string(static_cast<unsigned>(value)) + ")";
}

[[nodiscard]] inline bool supported_interpolation(const assets::Interpolation value) {
    return value == assets::Interpolation::step || value == assets::Interpolation::linear
        || value == assets::Interpolation::spherical;
}

[[nodiscard]] inline bool finite_sample(const assets::AnimationSample& sample) {
    const auto f = [](const float value) { return std::isfinite(value); };
    return f(sample.translation.x) && f(sample.translation.y) && f(sample.translation.z)
        && f(sample.scale.x) && f(sample.scale.y) && f(sample.scale.z)
        && f(sample.rotation.x) && f(sample.rotation.y) && f(sample.rotation.z) && f(sample.rotation.w);
}

[[nodiscard]] inline BindingDiagnosis diagnose_binding(
    const assets::Model& model, const assets::Animation& animation) {
    namespace codes = presentation::animation::diagnostic_codes;
    BindingDiagnosis result;
    result.model_bone_count = model.bones.size();
    result.track_count = animation.tracks.size();
    const auto reject = [&result](const std::string_view code, const std::string_view message) {
        if (!result.player_accepts) return;
        result.player_accepts = false;
        result.player_code = std::string(code);
        result.player_message = std::string(message);
    };

    for (std::size_t index = 0; index < model.bones.size(); ++index) {
        const auto& bone = model.bones[index];
        if (bone.parent < -1 || bone.parent >= static_cast<std::int32_t>(index)) {
            result.failures.push_back({std::string(predicates::model_hierarchy), {}, {}, {}, bone.name, false,
                "bone " + std::to_string(index) + " parent=" + std::to_string(bone.parent),
                "-1 <= parent < " + std::to_string(index)});
            reject(codes::invalid_model, "model hierarchy must be parent-first and acyclic");
        }
        bool finite = true;
        for (const float value : bone.relative_transform) finite = finite && std::isfinite(value);
        if (!finite) {
            result.failures.push_back({std::string(predicates::model_bind_finite), {}, {}, {}, bone.name, false,
                "bone " + std::to_string(index) + " non-finite", "finite"});
            reject(codes::invalid_model, "model bind transform is non-finite");
        }
    }

    const bool metadata_valid = std::isfinite(animation.frames_per_second) && animation.frames_per_second > 0.0F
        && std::isfinite(animation.duration_seconds)
        && animation.playable_frame_count < animation.stored_frame_count;
    if (!metadata_valid) {
        result.failures.push_back({std::string(predicates::duration_metadata), {}, {}, {}, {}, false,
            "fps=" + std::to_string(animation.frames_per_second) + " duration="
                + std::to_string(animation.duration_seconds) + " playable="
                + std::to_string(animation.playable_frame_count) + " stored="
                + std::to_string(animation.stored_frame_count),
            "finite fps>0, finite duration, playable<stored"});
        reject(codes::invalid_animation, "animation duration/frame metadata is invalid");
    } else {
        const float expected_duration = static_cast<float>(animation.playable_frame_count) / animation.frames_per_second;
        const float tolerance = std::max(0.00001F, expected_duration * 0.00001F);
        if (std::abs(animation.duration_seconds - expected_duration) > tolerance) {
            result.failures.push_back({std::string(predicates::duration_consistent), {}, {}, {}, {}, false,
                std::to_string(animation.duration_seconds), std::to_string(expected_duration)});
            reject(codes::invalid_animation, "animation duration does not match playable frames and frame rate");
        }
    }

    // Player claims a bone index only when index, name and sample count pass
    // (short-circuit of its compound predicate).  The diagnosis reports a
    // duplicate against any earlier track, and tracks Player's own claims
    // separately so its first-failing track is reproduced exactly.
    std::unordered_map<std::uint32_t, std::size_t> first_track_for_index;
    std::unordered_set<std::size_t> player_claimed;
    for (std::size_t ordinal = 0; ordinal < animation.tracks.size(); ++ordinal) {
        const assets::AnimationTrack& track = animation.tracks[ordinal];
        const bool in_range = track.bone_index < model.bones.size();
        const std::optional<std::string> model_name = in_range
            ? std::optional<std::string>(model.bones[track.bone_index].name) : std::nullopt;
        const auto add = [&](const std::string_view predicate, std::string actual, std::string expected) {
            result.failures.push_back({std::string(predicate), ordinal, track.bone_index, track.bone_name,
                model_name, !in_range, std::move(actual), std::move(expected)});
        };
        bool prefix_ok = true;
        if (!in_range) {
            prefix_ok = false;
            add(predicates::bone_index_in_range, std::to_string(track.bone_index),
                "< " + std::to_string(model.bones.size()));
        } else if (track.bone_name != *model_name) {
            prefix_ok = false;
            add(predicates::bone_name_equal, track.bone_name, *model_name);
        }
        if (track.samples.size() != animation.stored_frame_count) {
            prefix_ok = false;
            add(predicates::sample_count, std::to_string(track.samples.size()),
                std::to_string(animation.stored_frame_count));
        }
        const auto earlier = first_track_for_index.find(track.bone_index);
        if (earlier != first_track_for_index.end())
            add(predicates::unique_bone_index, "also track " + std::to_string(earlier->second), "unique");
        else first_track_for_index.emplace(track.bone_index, ordinal);
        const bool player_duplicate = prefix_ok && !player_claimed.insert(track.bone_index).second;
        bool interpolation_ok = true;
        const auto check_interpolation = [&](const std::string_view predicate, const assets::Interpolation value) {
            if (supported_interpolation(value)) return;
            interpolation_ok = false;
            add(predicate, interpolation_text(value), "step|linear|spherical");
        };
        check_interpolation(predicates::translation_interpolation, track.translation_interpolation);
        check_interpolation(predicates::scale_interpolation, track.scale_interpolation);
        check_interpolation(predicates::rotation_interpolation, track.rotation_interpolation);
        if (track.visibility_interpolation != assets::Interpolation::step) {
            interpolation_ok = false;
            add(predicates::visibility_interpolation, interpolation_text(track.visibility_interpolation), "step");
        }
        std::optional<std::size_t> first_non_finite;
        for (std::size_t sample = 0; sample < track.samples.size() && !first_non_finite; ++sample)
            if (!finite_sample(track.samples[sample])) first_non_finite = sample;
        if (first_non_finite) add(predicates::samples_finite, "sample " + std::to_string(*first_non_finite), "finite");

        if (!result.player_accepts) continue;
        if (!prefix_ok || player_duplicate || !interpolation_ok) {
            reject(codes::invalid_animation, compound_track_message);
            result.player_failing_track = ordinal;
        } else if (first_non_finite) {
            reject(codes::invalid_animation, non_finite_track_message);
            result.player_failing_track = ordinal;
        }
        if (result.player_failing_track) result.player_failing_bone = track.bone_index;
    }
    return result;
}

// Stable signature of a diagnosis: sorted distinct predicate names joined by
// '+'.  Used only for grouping in receipts, never as an association input.
[[nodiscard]] inline std::string predicate_signature(const BindingDiagnosis& diagnosis) {
    std::vector<std::string> names;
    for (const auto& failure : diagnosis.failures) names.push_back(failure.predicate);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::string result;
    for (const auto& name : names) { if (!result.empty()) result.push_back('+'); result += name; }
    return result.empty() ? std::string("none") : result;
}

} // namespace eawr::tests::animation_corpus
