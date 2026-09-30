#pragma once

#include "corpus_associations.hpp"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace association_contracts {
using namespace eawr;
namespace corpus = eawr::tests::animation_corpus;
namespace audit = eawr::tests::animation_corpus::associations;

inline int failures{};
inline void expect(const bool condition, const std::string_view message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; }
}

inline assets::Bone bone(const std::string_view name, const std::int32_t parent) {
    assets::Bone value;
    value.name = name; value.parent = parent; value.visible = true;
    value.relative_transform = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    return value;
}
inline assets::Model model_of(const std::vector<std::string>& names) {
    assets::Model value;
    for (std::size_t index = 0; index < names.size(); ++index)
        value.bones.push_back(bone(names[index], index == 0 ? -1 : 0));
    return value;
}
inline assets::AnimationTrack track(const std::uint32_t index, const std::string_view name) {
    assets::AnimationTrack value;
    value.bone_index = index; value.bone_name = name;
    value.samples = {
        {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
        {{1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
        {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, true},
    };
    return value;
}
inline assets::Animation clip_of(const std::vector<std::pair<std::uint32_t, std::string>>& tracks) {
    assets::Animation value;
    value.stored_frame_count = 3; value.playable_frame_count = 2;
    value.frames_per_second = 1.0F; value.duration_seconds = 2.0F;
    for (const auto& [index, name] : tracks) value.tracks.push_back(track(index, name));
    return value;
}

// The trooper-shaped fixture: the heavy model disagrees at index 1, the
// shorter-stem model binds, the "elite" model disagrees at index 2.
inline const std::vector<std::string> base_bones{"root", "arm", "hand", "tail"};
inline const std::vector<std::string> heavy_bones{"root", "backpack", "hand", "tail"};
inline const std::vector<std::string> elite_bones{"root", "arm", "gun", "tail"};

inline audit::AssetIdentity identity(const std::string& path) {
    return {path, "sha-" + path, "mod", "loose", "mod:loose:" + path, path, 100};
}

struct Fixture final {
    std::map<std::string, assets::Model> models;
    std::map<std::string, audit::CandidateModelView> special;
    audit::ModelLookup lookup() const {
        return [this](const std::string& path) {
            if (const auto found = special.find(path); found != special.end()) return found->second;
            audit::CandidateModelView view;
            const auto found = models.find(path);
            if (found == models.end()) { view.identity.path = path; return view; }
            view.found = true;
            view.identity = identity(path);
            view.model = &found->second;
            return view;
        };
    }
};

inline audit::BaselineFailure binding_failure(const std::string& ala, const std::vector<std::string>& model_paths,
    const std::size_t index = 1) {
    corpus::ModelIndex model_index;
    for (const auto& path : model_paths) corpus::add_model(model_index, path);
    audit::BaselineFailure failure;
    failure.animation = identity(ala);
    failure.baseline_failure_index = index;
    failure.stage = "binding";
    failure.code = std::string(presentation::animation::diagnostic_codes::invalid_animation);
    failure.cause = std::string(corpus::compound_track_message);
    failure.selected_model = corpus::matching_model(ala, model_index).value_or(std::string{});
    failure.r0_candidates = corpus::r0_qualifying_models(ala, model_index);
    return failure;
}

inline std::size_t count_status(const std::vector<audit::CandidateResult>& results, const std::string_view status) {
    return static_cast<std::size_t>(std::count_if(results.begin(), results.end(),
        [status](const audit::CandidateResult& item) { return item.status == status; }));
}

inline std::size_t count_of(const std::map<std::string, std::size_t>& counts, const std::string_view key) {
    const auto found = counts.find(std::string(key));
    return found == counts.end() ? 0U : found->second;
}

inline std::string receipt(const std::vector<audit::AuditRow>& rows) {
    std::ostringstream output;
    audit::write_audit(output, audit::AuditHeader{}, rows);
    return output.str();
}

// Frozen metadata recording exactly the current state `lookup` reports for
// these failures: the "nothing drifted" baseline each test then perturbs.
inline audit::FrozenMetadata frozen_of(const std::vector<audit::BaselineFailure>& baselines, const audit::ModelLookup& lookup) {
    audit::FrozenMetadata frozen;
    std::vector<std::string> named;
    for (const auto& failure : baselines) {
        frozen.failures[failure.baseline_failure_index] = failure;
        named.insert(named.end(), failure.r0_candidates.begin(), failure.r0_candidates.end());
    }
    std::sort(named.begin(), named.end());
    named.erase(std::unique(named.begin(), named.end()), named.end());
    for (const auto& path : named) {
        const auto view = lookup(path);
        if (!view.found) { frozen.unrecorded.push_back(path); continue; }
        frozen.models[path] = {view.identity,
            view.model ? std::optional<std::size_t>(view.model->bones.size()) : std::nullopt};
    }
    frozen.failure_count = baselines.size();
    return frozen;
}

inline audit::AuditRow audit_row(const audit::BaselineFailure& baseline, const assets::Animation* clip,
    const audit::ModelLookup& lookup) {
    return audit::audit_failure(baseline, clip, lookup, frozen_of({baseline}, lookup));
}

inline bool contains(const std::string& text, const std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

void test_shorter_prefix_passes_but_is_not_promoted();
void test_both_pass_is_ambiguous();
void test_neither_passes();
void test_unsupported_and_missing_candidates();
void test_zero_candidates_and_retained_stages();
void test_twenty_bone_candidate_rejects_index_twenty();
void test_selected_drift_fails_closed();
void test_frozen_input_gate();
void test_static_clip_disposition();
void test_shuffled_enumeration_is_deterministic();
void test_receipt_escaping();
void test_frozen_identity_unchanged_is_accepted();
void test_changed_selected_model_same_error_is_drift();
void test_selected_provenance_drift();
void test_alternative_identity_drift_and_unrecorded();
void test_candidate_enumeration_drift();
void test_failure_set_drift();
void test_frozen_metadata_parsing();
void test_tracked_manifest_parses();
void test_output_path_aliasing();
} // namespace association_contracts
