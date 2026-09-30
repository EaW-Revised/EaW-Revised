#pragma once

struct SampleCheck final {
    std::string label;
    float time_seconds{};
    std::string mode;
    bool ok{};
    bool finite{};
    bool deterministic{};
    std::string error_code;
};

struct CandidateResult final {
    AssetIdentity identity;
    bool found{};
    std::string status;
    std::string error_code;
    std::string error_message;
    std::optional<std::size_t> bone_count;
    std::optional<std::uint32_t> max_track_bone_index;
    // Some track index is at or past the candidate's bone count, so strict
    // binding cannot succeed regardless of names.
    bool range_impossible{};
    std::optional<BindingDiagnosis> diagnosis;
    bool player_accepts{};
    bool consistent_with_player{};
    std::string player_code;
    std::string player_message;
    // "binding" or "sampling" for rejected candidates.
    std::string rejection_stage;
    std::vector<SampleCheck> samples;
    // Compatible candidates only: whether the four sampled poses differ.
    std::string sampled_pose_motion;
    // frozen_identity::matched or ::unrecorded once checked; empty otherwise.
    std::string frozen_identity;
};

struct AuditRow final {
    BaselineFailure baseline;
    bool priority{};
    std::string status;
    // "static" when the clip has zero duration or every track holds one
    // sample value; otherwise "moving".  Empty when the clip was not loaded.
    std::string clip_motion;
    std::optional<std::size_t> track_count;
    // The baseline-selected model, re-evaluated to prove it still fails.
    std::optional<CandidateResult> selected;
    std::vector<std::string> additional_candidates;
    bool alternatives_evaluated{};
    std::vector<CandidateResult> alternatives;
    std::string drift_reason;
};

// ---------------------------------------------------------------------------
// Evaluation.
// ---------------------------------------------------------------------------

[[nodiscard]] inline bool same_sample(const assets::AnimationSample& left, const assets::AnimationSample& right) {
    return left.translation.x == right.translation.x && left.translation.y == right.translation.y
        && left.translation.z == right.translation.z && left.scale.x == right.scale.x
        && left.scale.y == right.scale.y && left.scale.z == right.scale.z
        && left.rotation.x == right.rotation.x && left.rotation.y == right.rotation.y
        && left.rotation.z == right.rotation.z && left.rotation.w == right.rotation.w
        && left.visible == right.visible;
}

[[nodiscard]] inline std::string clip_motion(const assets::Animation& clip) {
    if (clip.duration_seconds == 0.0F) return "static";
    for (const auto& track : clip.tracks)
        for (const auto& sample : track.samples)
            if (!same_sample(sample, track.samples.front())) return "moving";
    return "static";
}

[[nodiscard]] inline bool pose_finite(const presentation::animation::Pose& pose) {
    for (const auto& bone : pose.bones) {
        for (const float value : bone.local_asset) if (!std::isfinite(value)) return false;
        for (const float value : bone.model_asset) if (!std::isfinite(value)) return false;
        for (const float value : bone.skin_asset) if (!std::isfinite(value)) return false;
    }
    return std::isfinite(pose.sampled_time_seconds);
}

[[nodiscard]] inline bool same_pose(const presentation::animation::Pose& left, const presentation::animation::Pose& right) {
    if (left.bones.size() != right.bones.size()) return false;
    for (std::size_t index = 0; index < left.bones.size(); ++index) {
        const auto& a = left.bones[index];
        const auto& b = right.bones[index];
        if (a.local_asset != b.local_asset || a.model_asset != b.model_asset || a.skin_asset != b.skin_asset
            || a.visible != b.visible)
            return false;
    }
    return true;
}

[[nodiscard]] inline CandidateResult evaluate_candidate(const assets::Animation& clip, const CandidateModelView& view) {
    namespace playback = presentation::animation;
    CandidateResult result;
    result.identity = view.identity;
    result.found = view.found;
    result.error_code = view.error_code;
    result.error_message = view.error_message;
    for (const auto& track : clip.tracks)
        result.max_track_bone_index = std::max(result.max_track_bone_index.value_or(0U), track.bone_index);
    if (!view.found) {
        result.status = std::string(candidate_status::missing);
        return result;
    }
    if (!view.model) {
        result.status = std::string(candidate_status::parse_failed);
        return result;
    }
    const assets::Model& model = *view.model;
    result.bone_count = model.bones.size();
    result.range_impossible = result.max_track_bone_index && *result.max_track_bone_index >= model.bones.size();

    auto player = playback::Player::create(model, &clip);
    result.diagnosis = diagnose_binding(model, clip);
    result.player_accepts = static_cast<bool>(player);
    if (!player) {
        result.player_code = player.error().code;
        result.player_message = player.error().message;
    }
    result.consistent_with_player = result.diagnosis->player_accepts == result.player_accepts
        && (result.player_accepts || (result.diagnosis->player_code == result.player_code
            && result.diagnosis->player_message == result.player_message));
    if (!player) {
        result.status = std::string(candidate_status::rejected);
        result.rejection_stage = "binding";
        return result;
    }

    struct Request final { std::string_view label; float time; playback::PlaybackMode mode; };
    const std::array<Request, 4> requests{{
        {"loop_start", 0.0F, playback::PlaybackMode::loop},
        {"loop_midpoint", clip.duration_seconds * 0.5F, playback::PlaybackMode::loop},
        {"loop_duration", clip.duration_seconds, playback::PlaybackMode::loop},
        {"clamp_terminal", clip.duration_seconds, playback::PlaybackMode::clamp},
    }};
    bool all_ok = true;
    std::vector<playback::Pose> poses;
    for (const auto& request : requests) {
        SampleCheck check;
        check.label = std::string(request.label);
        check.time_seconds = request.time;
        check.mode = request.mode == playback::PlaybackMode::loop ? "loop" : "clamp";
        auto first = player.value().sample({request.time, request.mode, 0.0F});
        auto second = player.value().sample({request.time, request.mode, 0.0F});
        if (!first || !second) {
            check.error_code = !first ? first.error().code : second.error().code;
        } else {
            check.finite = pose_finite(first.value()) && pose_finite(second.value());
            check.deterministic = same_pose(first.value(), second.value());
            check.ok = check.finite && check.deterministic;
            poses.push_back(std::move(first.value()));
        }
        all_ok = all_ok && check.ok;
        result.samples.push_back(std::move(check));
    }
    if (!all_ok) {
        result.status = std::string(candidate_status::rejected);
        result.rejection_stage = "sampling";
        return result;
    }
    result.status = std::string(candidate_status::compatible_unapproved);
    result.sampled_pose_motion = "static";
    for (const auto& pose : poses)
        if (!same_pose(pose, poses.front())) result.sampled_pose_motion = "moving";
    return result;
}

// Row status from alternative results only.  More than one compatible
// alternative is ambiguous; a single one is still unapproved.
[[nodiscard]] inline std::string classify_alternatives(const std::vector<CandidateResult>& alternatives) {
    if (alternatives.empty()) return std::string(row_status::no_additional_candidate);
    const auto compatible = std::count_if(alternatives.begin(), alternatives.end(), [](const CandidateResult& item) {
        return item.status == candidate_status::compatible_unapproved;
    });
    if (compatible == 0) return std::string(row_status::no_compatible);
    return std::string(compatible == 1 ? row_status::single_compatible : row_status::ambiguous);
}

struct FrozenIdentityCheck final {
    // frozen_identity::matched or ::unrecorded; empty when `drift` is set.
    std::string status;
    std::string drift;
};

// Compares a model's current effective-VFS identity and bone count with the
// frozen metadata.  A recorded model must match exactly; an unrecorded one is
// accepted only if the frozen metadata lists it as unrecorded and the caller
// does not require a record (a selected model always has one).
[[nodiscard]] inline FrozenIdentityCheck check_model_identity(const FrozenMetadata& frozen, const std::string& path,
    const CandidateModelView& view, const bool require_record) {
    const auto found = frozen.models.find(path);
    if (found == frozen.models.end()) {
        if (require_record || !frozen.is_unrecorded(path))
            return {{}, "model " + path + " has no frozen identity"};
        return {std::string(frozen_identity::unrecorded), {}};
    }
    if (!view.found) return {{}, "model " + path + " is no longer found; frozen sha256 " + found->second.identity.sha256};
    if (const auto field = identity_difference(found->second.identity, view.identity))
        return {{}, "model " + path + " " + *field + " differs from frozen metadata"};
    const std::optional<std::size_t> bones = view.model ? std::optional<std::size_t>(view.model->bones.size()) : std::nullopt;
    if (bones != found->second.bone_count) return {{}, "model " + path + " bone_count differs from frozen metadata"};
    return {std::string(frozen_identity::matched), {}};
}

// Audits one failure against its frozen record.  `clip` is the re-parsed ALA
// for binding rows and may be null for other stages.  Model-match and
// model-parse rows keep their original cause; their candidates are listed but
// not evaluated.  Any difference from the frozen metadata (failure identity,
// R0 list, selected or recorded candidate identity) is drift.
[[nodiscard]] inline AuditRow audit_failure(const BaselineFailure& baseline, const assets::Animation* clip,
    const ModelLookup& lookup, const FrozenMetadata& frozen) {
    AuditRow row;
    row.baseline = baseline;
    row.priority = baseline.selected_model == priority_selected_model;
    for (const auto& candidate : baseline.r0_candidates)
        if (candidate != baseline.selected_model) row.additional_candidates.push_back(candidate);
    std::sort(row.additional_candidates.begin(), row.additional_candidates.end());
    row.additional_candidates.erase(
        std::unique(row.additional_candidates.begin(), row.additional_candidates.end()), row.additional_candidates.end());
    const auto drift = [&row](std::string reason) {
        row.status = std::string(row_status::baseline_drift);
        row.drift_reason = std::move(reason);
        row.alternatives_evaluated = false;
        row.alternatives.clear();
        return row;
    };

    const auto frozen_row = frozen.failures.find(baseline.baseline_failure_index);
    if (frozen_row == frozen.failures.end()) return drift("failure is not in the frozen metadata");
    if (const auto field = failure_difference(frozen_row->second, baseline))
        return drift(*field + " differs from frozen metadata");

    if (baseline.stage == "model_match") {
        if (!baseline.r0_candidates.empty()) return drift("model_match row has R0 candidates");
        row.status = std::string(row_status::retained_model_match);
        return row;
    }
    if (baseline.stage == "model_parse") {
        // The selected model is not re-evaluated, but its identity is frozen.
        const auto identity = check_model_identity(frozen, baseline.selected_model, lookup(baseline.selected_model), true);
        if (!identity.drift.empty()) return drift("selected " + identity.drift);
        row.status = std::string(row_status::retained_parser_owned);
        return row;
    }
    if (baseline.stage != "binding") {
        row.status = std::string(row_status::retained_other);
        return row;
    }
    if (!clip) return drift("binding row clip did not re-load");
    row.clip_motion = clip_motion(*clip);
    row.track_count = clip->tracks.size();
    const CandidateModelView selected_view = lookup(baseline.selected_model);
    const auto selected_identity = check_model_identity(frozen, baseline.selected_model, selected_view, true);
    if (!selected_identity.drift.empty()) return drift("selected " + selected_identity.drift);
    row.selected = evaluate_candidate(*clip, selected_view);
    row.selected->frozen_identity = selected_identity.status;
    if (row.selected->status != candidate_status::rejected || row.selected->rejection_stage != "binding"
        || row.selected->player_code != baseline.code || row.selected->player_message != baseline.cause)
        return drift("selected model no longer fails binding with the frozen code and cause");
    row.alternatives_evaluated = true;
    for (const auto& candidate : row.additional_candidates) {
        const CandidateModelView view = lookup(candidate);
        const auto identity = check_model_identity(frozen, candidate, view, false);
        if (!identity.drift.empty()) return drift("alternative " + identity.drift);
        row.alternatives.push_back(evaluate_candidate(*clip, view));
        row.alternatives.back().frozen_identity = identity.status;
    }
    row.status = classify_alternatives(row.alternatives);
    return row;
}

// ---------------------------------------------------------------------------
// Output path roles.
// ---------------------------------------------------------------------------

// A named command-line path.  Outputs are opened for truncation, so no output
// may name the same file as any frozen input or any other output.
struct PathRole final {
    std::string name;
    std::filesystem::path path;
};

namespace path_detail {

// Absolute, symlink- and dot-resolved form of the existing prefix, lexically
// normalised beyond it.  On Windows `absolute` also applies Win32 path rules
// (separators, trailing dots and spaces).
[[nodiscard]] inline std::optional<std::filesystem::path> normalized(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) return std::nullopt;
    auto result = std::filesystem::weakly_canonical(absolute, error);
    if (error) return std::nullopt;
    result = result.lexically_normal();
    if (!result.has_filename()) result = result.parent_path();
    return result;
}

// File names compare case-insensitively where the default file system does.
[[nodiscard]] inline bool same_spelling(const std::filesystem::path& left, const std::filesystem::path& right) {
#if defined(_WIN32) || defined(__APPLE__)
    const auto& a = left.native();
    const auto& b = right.native();
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (std::towupper(static_cast<std::wint_t>(a[index])) != std::towupper(static_cast<std::wint_t>(b[index])))
            return false;
    }
    return true;
#else
    return left == right;
#endif
}

} // namespace path_detail

// Returns an error when any two roles name the same file, either by
// normalised spelling or, for existing files, by file identity (hard links,
// symbolic links, alternate names).  Call before any output is opened.
[[nodiscard]] inline std::optional<std::string> check_distinct_paths(const std::vector<PathRole>& roles) {
    std::vector<std::filesystem::path> normal;
    for (const PathRole& role : roles) {
        if (role.path.empty()) return role.name + " path is empty";
        auto value = path_detail::normalized(role.path);
        if (!value) return role.name + " path cannot be normalised";
        normal.push_back(std::move(*value));
    }
    for (std::size_t left = 0; left < roles.size(); ++left) {
        for (std::size_t right = left + 1; right < roles.size(); ++right) {
            std::error_code error;
            const bool same_file = std::filesystem::equivalent(roles[left].path, roles[right].path, error) && !error;
            if (same_file || path_detail::same_spelling(normal[left], normal[right]))
                return roles[left].name + " and " + roles[right].name + " name the same file";
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Receipt.
// ---------------------------------------------------------------------------

struct MountSummary final {
    std::string layer_id;
    std::string manifest_source_id;
    std::vector<std::string> active_archives;
};

struct AuditHeader final {
    std::string profile{"remake-effective"};
    std::string tool_name;
    std::string compiler;
    std::string config;
    std::vector<std::pair<std::string, std::string>> tool_sources;
    std::vector<MountSummary> mounts;
    std::string frozen_ledger_sha256;
    std::uint64_t frozen_ledger_bytes{};
    std::string fresh_ledger_sha256;
    std::string frozen_metadata_sha256;
    std::uint64_t frozen_metadata_bytes{};
    std::string frozen_metadata_source_sha256;
    std::size_t frozen_model_records{};
    std::size_t frozen_unrecorded_models{};
    std::size_t animation_count{};
    std::size_t playback_passed{};
    std::size_t failure_count{};
};

struct AuditSummary final {
    std::map<std::string, std::size_t> rows_by_baseline_stage;
    std::map<std::string, std::size_t> rows_by_status;
    std::map<std::string, std::size_t> alternatives_by_status;
    std::map<std::string, std::size_t> priority_rows_by_status;
    std::map<std::string, std::size_t> alternatives_by_frozen_identity;
    std::size_t rows{};
    std::size_t evaluated_rows{};
    std::size_t rows_with_alternatives{};
    std::size_t alternative_pairs{};
    std::size_t range_impossible_pairs{};
    std::size_t range_impossible_rejected{};
    std::size_t priority_rows{};
    std::size_t priority_rows_evaluated{};
    std::size_t selected_rechecked_rejected{};
    std::size_t selected_frozen_identity_matched{};
    std::size_t player_diagnosis_inconsistencies{};
    std::size_t drift_rows{};
};

[[nodiscard]] inline AuditSummary summarize(const std::vector<AuditRow>& rows) {
    AuditSummary summary;
    summary.rows = rows.size();
    for (const std::string_view status : {candidate_status::compatible_unapproved, candidate_status::rejected,
             candidate_status::parse_failed, candidate_status::missing})
        summary.alternatives_by_status[std::string(status)] = 0;
    for (const std::string_view identity : {frozen_identity::matched, frozen_identity::unrecorded})
        summary.alternatives_by_frozen_identity[std::string(identity)] = 0;
    for (const AuditRow& row : rows) {
        ++summary.rows_by_baseline_stage[row.baseline.stage];
        ++summary.rows_by_status[row.status];
        if (row.status == row_status::baseline_drift) ++summary.drift_rows;
        if (row.priority) {
            ++summary.priority_rows;
            ++summary.priority_rows_by_status[row.status];
            if (row.alternatives_evaluated) ++summary.priority_rows_evaluated;
        }
        if (row.selected) {
            if (row.selected->status == candidate_status::rejected) ++summary.selected_rechecked_rejected;
            if (row.selected->frozen_identity == frozen_identity::matched) ++summary.selected_frozen_identity_matched;
            if (row.selected->diagnosis && !row.selected->consistent_with_player) ++summary.player_diagnosis_inconsistencies;
        }
        if (!row.alternatives_evaluated) continue;
        ++summary.evaluated_rows;
        if (!row.alternatives.empty()) ++summary.rows_with_alternatives;
        for (const auto& alternative : row.alternatives) {
            ++summary.alternative_pairs;
            ++summary.alternatives_by_status[alternative.status];
            ++summary.alternatives_by_frozen_identity[alternative.frozen_identity];
            if (alternative.range_impossible) {
                ++summary.range_impossible_pairs;
                if (alternative.status == candidate_status::rejected) ++summary.range_impossible_rejected;
            }
            if (alternative.diagnosis && !alternative.consistent_with_player) ++summary.player_diagnosis_inconsistencies;
        }
    }
    return summary;
}

[[nodiscard]] inline std::string json_float(const float value) {
    if (!std::isfinite(value)) return json_string("non-finite");
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) return json_string("unformattable");
    return std::string(buffer.data(), end);
}

template <typename T>
[[nodiscard]] std::string json_opt(const std::optional<T>& value) {
    if (!value) return "null";
    if constexpr (std::is_same_v<T, std::string>) return json_string(*value);
    else return std::to_string(*value);
}

inline void write_counts(std::ostream& output, const std::map<std::string, std::size_t>& counts) {
    output << '{';
    bool first = true;
    for (const auto& [key, count] : counts) {
        output << (first ? "" : ", ") << json_string(key) << ": " << count;
        first = false;
    }
    output << '}';
}

inline void write_identity(std::ostream& output, const AssetIdentity& identity) {
    output << "\"path\": " << json_string(identity.path) << ", \"sha256\": " << json_string(identity.sha256)
           << ", \"layer_id\": " << json_string(identity.layer_id) << ", \"origin\": " << json_string(identity.origin)
           << ", \"source_id\": " << json_string(identity.source_id)
           << ", \"original_path\": " << json_string(identity.original_path) << ", \"size\": " << identity.size;
}

inline void write_candidate(std::ostream& output, const CandidateResult& candidate) {
    output << '{';
    write_identity(output, candidate.identity);
    output << ", \"found\": " << (candidate.found ? "true" : "false")
           << ", \"status\": " << json_string(candidate.status)
           << ", \"approved\": false"
           << ", \"error_code\": " << json_string(candidate.error_code)
           << ", \"error_message\": " << json_string(candidate.error_message)
           << ", \"bone_count\": " << json_opt(candidate.bone_count)
           << ", \"max_track_bone_index\": " << json_opt(candidate.max_track_bone_index)
           << ", \"range_impossible\": " << (candidate.range_impossible ? "true" : "false")
           << ", \"rejection_stage\": " << json_string(candidate.rejection_stage)
           << ", \"player_accepts\": " << (candidate.player_accepts ? "true" : "false")
           << ", \"consistent_with_player\": " << (candidate.consistent_with_player ? "true" : "false")
           << ", \"player_code\": " << json_string(candidate.player_code)
           << ", \"player_message\": " << json_string(candidate.player_message)
           << ", \"diagnosis\": ";
    if (candidate.diagnosis) {
        const auto& diagnosis = *candidate.diagnosis;
        output << "{\"signature\": " << json_string(predicate_signature(diagnosis))
               << ", \"predicted_failing_track\": " << json_opt(diagnosis.player_failing_track)
               << ", \"predicted_failing_bone_index\": " << json_opt(diagnosis.player_failing_bone)
               << ", \"model_bone_count\": " << diagnosis.model_bone_count
               << ", \"track_count\": " << diagnosis.track_count << ", \"failures\": [";
        for (std::size_t item = 0; item < diagnosis.failures.size(); ++item) {
            const auto& failure = diagnosis.failures[item];
            output << (item == 0 ? "" : ", ") << "{\"predicate\": " << json_string(failure.predicate)
                   << ", \"track\": " << json_opt(failure.track_ordinal)
                   << ", \"bone_index\": " << json_opt(failure.track_bone_index)
                   << ", \"track_name\": " << json_opt(failure.track_bone_name)
                   << ", \"model_bone_name\": " << json_opt(failure.model_bone_name)
                   << ", \"index_out_of_range\": " << (failure.index_out_of_range ? "true" : "false")
                   << ", \"actual\": " << json_string(failure.actual)
                   << ", \"expected\": " << json_string(failure.expected) << '}';
        }
        output << "]}";
    } else output << "null";
    output << ", \"samples\": [";
    for (std::size_t item = 0; item < candidate.samples.size(); ++item) {
        const auto& sample = candidate.samples[item];
        output << (item == 0 ? "" : ", ") << "{\"label\": " << json_string(sample.label)
               << ", \"time_seconds\": " << json_float(sample.time_seconds)
               << ", \"mode\": " << json_string(sample.mode) << ", \"ok\": " << (sample.ok ? "true" : "false")
               << ", \"finite\": " << (sample.finite ? "true" : "false")
               << ", \"deterministic\": " << (sample.deterministic ? "true" : "false")
               << ", \"error_code\": " << json_string(sample.error_code) << '}';
    }
    output << "], \"sampled_pose_motion\": " << json_string(candidate.sampled_pose_motion)
           << ", \"frozen_identity\": " << json_string(candidate.frozen_identity) << '}';
}

// Rows are written sorted by ALA path, alternatives by candidate path, so the
// receipt is independent of enumeration and evaluation order.
inline void write_audit(std::ostream& output, const AuditHeader& header, std::vector<AuditRow> rows) {
    std::sort(rows.begin(), rows.end(), [](const AuditRow& left, const AuditRow& right) {
        return left.baseline.animation.path != right.baseline.animation.path
            ? left.baseline.animation.path < right.baseline.animation.path
            : left.baseline.baseline_failure_index < right.baseline.baseline_failure_index;
    });
    for (AuditRow& row : rows)
        std::sort(row.alternatives.begin(), row.alternatives.end(),
            [](const CandidateResult& left, const CandidateResult& right) { return left.identity.path < right.identity.path; });
    const AuditSummary summary = summarize(rows);

    output << "{\n  \"schema\": " << json_string(schema) << ",\n  \"schema_version\": " << schema_version
           << ",\n  \"profile\": " << json_string(header.profile)
           << ",\n  \"association_rule\": " << json_string(r0_rule_id)
           << ",\n  \"association_approved\": false,\n  \"associations_promoted\": 0"
           << ",\n  \"corpus_acceptance_claimed\": false"
           << ",\n  \"baseline\": {\"ledger_schema\": \"eawr.animation-playback-corpus\", \"ledger_schema_version\": 1"
           << ", \"pinned_sha256\": " << json_string(pinned_frozen_ledger_sha256)
           << ", \"frozen_sha256\": " << json_string(header.frozen_ledger_sha256)
           << ", \"frozen_bytes\": " << header.frozen_ledger_bytes
           << ", \"reemitted_sha256\": " << json_string(header.fresh_ledger_sha256)
           << ", \"animation_count\": " << header.animation_count << ", \"playback_passed\": " << header.playback_passed
           << ", \"failure_count\": " << header.failure_count << "},\n  \"frozen_metadata\": {\"schema\": "
           << json_string(frozen_metadata_schema) << ", \"pinned_sha256\": " << json_string(pinned_frozen_metadata_sha256)
           << ", \"sha256\": " << json_string(header.frozen_metadata_sha256)
           << ", \"bytes\": " << header.frozen_metadata_bytes
           << ", \"source_diagnostics_sha256\": " << json_string(header.frozen_metadata_source_sha256)
           << ", \"model_records\": " << header.frozen_model_records
           << ", \"unrecorded_models\": " << header.frozen_unrecorded_models << "},\n  \"tool\": {\"name\": "
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
           << ", \"rows_by_baseline_stage\": ";
    write_counts(output, summary.rows_by_baseline_stage);
    output << ", \"rows_by_status\": ";
    write_counts(output, summary.rows_by_status);
    output << ", \"evaluated_rows\": " << summary.evaluated_rows
           << ", \"rows_with_alternatives\": " << summary.rows_with_alternatives
           << ", \"alternative_pairs\": " << summary.alternative_pairs << ", \"alternatives_by_status\": ";
    write_counts(output, summary.alternatives_by_status);
    output << ", \"ambiguous_rows\": "
           << (summary.rows_by_status.contains(std::string(row_status::ambiguous))
                   ? summary.rows_by_status.at(std::string(row_status::ambiguous)) : 0U)
           << ", \"range_impossible_pairs\": " << summary.range_impossible_pairs
           << ", \"range_impossible_rejected\": " << summary.range_impossible_rejected
           << ", \"priority_selected_model\": " << json_string(priority_selected_model)
           << ", \"priority_rows\": " << summary.priority_rows
           << ", \"priority_rows_evaluated\": " << summary.priority_rows_evaluated << ", \"priority_rows_by_status\": ";
    write_counts(output, summary.priority_rows_by_status);
    output << ", \"alternatives_by_frozen_identity\": ";
    write_counts(output, summary.alternatives_by_frozen_identity);
    output << ", \"selected_rechecked_rejected\": " << summary.selected_rechecked_rejected
           << ", \"selected_frozen_identity_matched\": " << summary.selected_frozen_identity_matched
           << ", \"player_diagnosis_inconsistencies\": " << summary.player_diagnosis_inconsistencies
           << ", \"drift_rows\": " << summary.drift_rows << "},\n  \"rows\": [";

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const AuditRow& row = rows[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"animation\": {";
        write_identity(output, row.baseline.animation);
        output << "}, \"baseline_failure_index\": " << row.baseline.baseline_failure_index
               << ", \"baseline_stage\": " << json_string(row.baseline.stage)
               << ", \"baseline_code\": " << json_string(row.baseline.code)
               << ", \"baseline_cause\": " << json_string(row.baseline.cause)
               << ", \"baseline_selected_model\": " << json_string(row.baseline.selected_model)
               << ", \"r0_candidates\": [";
        for (std::size_t item = 0; item < row.baseline.r0_candidates.size(); ++item)
            output << (item == 0 ? "" : ", ") << json_string(row.baseline.r0_candidates[item]);
        output << "], \"priority\": " << (row.priority ? "true" : "false")
               << ", \"status\": " << json_string(row.status)
               << ", \"drift_reason\": " << json_string(row.drift_reason)
               << ", \"clip_motion\": " << json_string(row.clip_motion)
               << ", \"track_count\": " << json_opt(row.track_count) << ", \"selected\": ";
        if (row.selected) write_candidate(output, *row.selected);
        else output << "null";
        output << ", \"additional_candidates\": [";
        for (std::size_t item = 0; item < row.additional_candidates.size(); ++item)
            output << (item == 0 ? "" : ", ") << json_string(row.additional_candidates[item]);
        output << "], \"alternatives_evaluated\": " << (row.alternatives_evaluated ? "true" : "false")
               << ", \"alternatives\": [";
        for (std::size_t item = 0; item < row.alternatives.size(); ++item) {
            output << (item == 0 ? "" : ", ");
            write_candidate(output, row.alternatives[item]);
        }
        output << "]}";
    }
    output << (rows.empty() ? "" : "\n  ") << "]\n}\n";
}
