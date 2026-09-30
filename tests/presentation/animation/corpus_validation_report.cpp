#include "corpus_validation_support.hpp"

namespace eawr_validation {
[[nodiscard]] std::string json(const std::string_view value) {
    return corpus::json_string(value);
}

[[nodiscard]] std::string json_number(const float value) {
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{} || !std::isfinite(value)) return json(std::string_view("non-finite"));
    return std::string(buffer.data(), end);
}

template <typename T>
[[nodiscard]] std::string json_optional(const std::optional<T>& value) {
    if (!value) return "null";
    if constexpr (std::is_same_v<T, std::string>) return json(*value);
    else return std::to_string(*value);
}

[[nodiscard]] std::string record_json(const eawr::vfs::AssetRecord& record) {
    return "\"layer_id\": " + json(record.layer_id) + ", \"origin\": "
        + json(eawr::vfs::to_string(record.origin)) + ", \"source_id\": " + json(record.source_id)
        + ", \"original_path\": " + json(record.original_path) + ", \"size\": "
        + std::to_string(record.size);
}

[[nodiscard]] std::string compiler_identity() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_FULL_VER);
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

void write_diagnostics(std::ostream& output, const std::vector<Row>& rows,
    const std::vector<eawr::vfs::ManifestResolution>& mounts, const std::size_t passed,
    const std::size_t failure_count, const std::unordered_map<std::string, ModelEntry>& models,
    const std::size_t inconsistencies) {
    std::map<std::string, std::size_t> by_stage;
    std::map<std::string, std::size_t> signatures;
    std::map<std::string, std::size_t> predicate_rows;
    std::size_t binding_predicate_failures{};
    for (const Row& row : rows) {
        ++by_stage[row.stage];
        if (row.stage != "binding" || !row.binding) continue;
        ++signatures[corpus::predicate_signature(*row.binding)];
        binding_predicate_failures += row.binding->failures.size();
        std::vector<std::string> names;
        for (const auto& failure : row.binding->failures) names.push_back(failure.predicate);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        for (const auto& name : names) ++predicate_rows[name];
    }
    const auto write_counts = [&output](const std::map<std::string, std::size_t>& counts) {
        output << '{';
        bool first = true;
        for (const auto& [key, count] : counts) {
            output << (first ? "" : ", ") << json(key) << ": " << count;
            first = false;
        }
        output << '}';
    };

    output << "{\n  \"schema\": \"eawr.animation-corpus-diagnostics\",\n  \"schema_version\": 1,\n"
           << "  \"profile\": \"remake-effective\",\n"
           << "  \"baseline_ledger_schema\": \"eawr.animation-playback-corpus\",\n"
           << "  \"baseline_ledger_schema_version\": 1,\n"
           << "  \"association_rule\": " << json(corpus::r0_rule_id) << ",\n"
           << "  \"association_approved\": false,\n"
           << "  \"tool\": {\"name\": \"animation_corpus_validation\", \"compiler\": "
           << json(compiler_identity()) << ", \"config\": "
#if defined(NDEBUG)
           << json("release")
#else
           << json("debug")
#endif
           << ", \"source_sha256\": {";
#if defined(EAWR_CORPUS_TOOL_IDENTITY)
    for (std::size_t index = 0; index < eawr_corpus_tool::source_hashes.size(); ++index) {
        output << (index == 0 ? "" : ", ") << json(eawr_corpus_tool::source_hashes[index].first) << ": "
               << json(eawr_corpus_tool::source_hashes[index].second);
    }
#endif
    output << "}},\n  \"mounts\": [\n";
    for (std::size_t index = 0; index < mounts.size(); ++index) {
        const auto& mount = mounts[index];
        output << "    {\"layer_id\": " << json(mount.mount.layer_id) << ", \"manifest_source_id\": "
               << json(mount.manifest_source_id) << ", \"active_archives\": [";
        for (std::size_t archive = 0; archive < mount.mount.active_archives.size(); ++archive)
            output << (archive == 0 ? "" : ", ") << json(mount.mount.active_archives[archive].source_id);
        output << "], \"declared_archives\": [";
        for (std::size_t archive = 0; archive < mount.declared_archives.size(); ++archive)
            output << (archive == 0 ? "" : ", ") << json(mount.declared_archives[archive]);
        output << "], \"missing_archives\": [";
        for (std::size_t archive = 0; archive < mount.missing_archives.size(); ++archive)
            output << (archive == 0 ? "" : ", ") << json(mount.missing_archives[archive]);
        output << "]}" << (index + 1 == mounts.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"counts\": {\"animation_count\": " << rows.size() << ", \"playback_passed\": " << passed
           << ", \"failure_count\": " << failure_count << ", \"by_stage\": ";
    write_counts(by_stage);
    output << ", \"binding_predicate_failures\": " << binding_predicate_failures
           << ", \"binding_rows_by_predicate\": ";
    write_counts(predicate_rows);
    output << ", \"binding_rows_by_signature\": ";
    write_counts(signatures);
    output << ", \"player_diagnosis_inconsistencies\": " << inconsistencies << "},\n  \"rows\": [\n";

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const Row& row = rows[index];
        output << "    {\"ordinal\": " << index + 1 << ", \"path\": " << json(row.record.canonical_path)
               << ", \"sha256\": " << json(row.sha256) << ", " << record_json(row.record)
               << ", \"stage\": " << json(row.stage) << ", \"code\": " << json(row.code)
               << ", \"cause\": " << json(row.cause)
               << ", \"baseline_failure_index\": " << json_optional(row.baseline_failure_index);
        output << ", \"animation\": ";
        if (row.animation) {
            output << "{\"version\": " << row.animation->version << ", \"stored_frames\": "
                   << row.animation->stored_frames << ", \"playable_frames\": " << row.animation->playable_frames
                   << ", \"frames_per_second\": " << json_number(row.animation->frames_per_second)
                   << ", \"duration_seconds\": " << json_number(row.animation->duration_seconds)
                   << ", \"tracks\": " << row.animation->tracks << '}';
        } else output << "null";
        output << ", \"r0_qualifying_models\": [";
        for (std::size_t candidate = 0; candidate < row.qualifying_models.size(); ++candidate)
            output << (candidate == 0 ? "" : ", ") << json(row.qualifying_models[candidate]);
        output << "], \"model\": ";
        const auto model = row.model_path.empty() ? models.end() : models.find(row.model_path);
        if (model != models.end()) {
            output << "{\"path\": " << json(row.model_path) << ", \"sha256\": " << json(model->second.sha256);
            if (model->second.record) output << ", " << record_json(*model->second.record);
            output << ", \"bone_count\": "
                   << (model->second.model ? std::to_string(model->second.model->bones.size()) : "null") << '}';
        } else output << "null";
        output << ", \"binding\": ";
        if (row.binding) {
            const auto& binding = *row.binding;
            output << "{\"player_accepts\": " << (binding.player_accepts ? "true" : "false")
                   << ", \"consistent_with_player\": "
                   << (row.binding_consistent.value_or(false) ? "true" : "false")
                   << ", \"player_code\": " << json(binding.player_code)
                   << ", \"player_message\": " << json(binding.player_message)
                   << ", \"player_failing_track\": " << json_optional(binding.player_failing_track)
                   << ", \"player_failing_bone_index\": " << json_optional(binding.player_failing_bone)
                   << ", \"model_bone_count\": " << binding.model_bone_count
                   << ", \"track_count\": " << binding.track_count
                   << ", \"signature\": " << json(corpus::predicate_signature(binding))
                   << ", \"failures\": [";
            for (std::size_t item = 0; item < binding.failures.size(); ++item) {
                const auto& failure = binding.failures[item];
                output << (item == 0 ? "" : ", ") << "{\"predicate\": " << json(failure.predicate)
                       << ", \"track\": " << json_optional(failure.track_ordinal)
                       << ", \"bone_index\": " << json_optional(failure.track_bone_index)
                       << ", \"track_name\": " << json_optional(failure.track_bone_name)
                       << ", \"model_bone_name\": " << json_optional(failure.model_bone_name)
                       << ", \"index_out_of_range\": " << (failure.index_out_of_range ? "true" : "false")
                       << ", \"actual\": " << json(failure.actual)
                       << ", \"expected\": " << json(failure.expected) << '}';
            }
            output << "]}";
        } else output << "null";
        output << ", \"samples_passed\": " << row.samples_passed << '}'
               << (index + 1 == rows.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

} // namespace eawr_validation
