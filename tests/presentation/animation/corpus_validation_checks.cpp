#include "corpus_validation_support.hpp"

namespace eawr_validation {
namespace {

namespace audit = corpus::associations;

struct CommandLine final {
    std::vector<std::string> positional;
    std::optional<std::array<std::filesystem::path, 3>> audit_arguments;
};

// Positional arguments, then an optional association audit.  The audit is
// metadata only: it never changes selection, the ledger or the receipt.
std::optional<CommandLine> parse_command_line(int argc, char** argv) {
    std::vector<std::string> positional;
    std::optional<std::array<std::filesystem::path, 3>> audit_arguments;
    bool usage_error = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--association-audit") {
            if (audit_arguments || index + 3 >= argc) {
                usage_error = true;
                break;
            }
            audit_arguments = std::array<std::filesystem::path, 3>{argv[index + 1], argv[index + 2], argv[index + 3]};
            index += 3;
        } else positional.emplace_back(argument);
    }
    if (usage_error || (positional.size() != 3 && positional.size() != 4)) return std::nullopt;
    return CommandLine{std::move(positional), audit_arguments};
}

// Before anything is read or written: no output may name the same file as
// a frozen input or another output, by spelling or by file identity.
bool outputs_distinct(const std::filesystem::path& report_path, const std::vector<std::string>& positional,
                      const std::optional<std::array<std::filesystem::path, 3>>& audit_arguments) {
    std::vector<audit::PathRole> roles{{"report", report_path}};
    if (positional.size() == 4) roles.push_back({"diagnostics", positional[3]});
    if (audit_arguments) {
        roles.push_back({"frozen ledger", (*audit_arguments)[0]});
        roles.push_back({"frozen metadata", (*audit_arguments)[1]});
        roles.push_back({"audit", (*audit_arguments)[2]});
    }
    if (const auto error = audit::check_distinct_paths(roles)) {
        std::cerr << "refused: " << *error << '\n';
        return false;
    }
    return true;
}

// Authenticate both frozen inputs before the run: no output is written
// when either is missing, stale or malformed.
std::optional<AuditInputs> authenticate_audit_inputs(const std::array<std::filesystem::path, 3>& arguments) {
    AuditInputs inputs{arguments[0], arguments[1], arguments[2], {}, {}, {}};
    const auto metadata_bytes = read_file(inputs.metadata_path);
    auto load = audit::load_frozen_metadata(metadata_bytes, bytes_sha256(metadata_bytes));
    if (!load.metadata) {
        std::cerr << "association audit refused: " << load.error << '\n';
        return std::nullopt;
    }
    inputs.metadata = std::move(*load.metadata);
    const auto ledger_bytes = read_file(inputs.ledger_path);
    const auto ledger_sha = bytes_sha256(ledger_bytes);
    // Pin check only; the re-emitted ledger is compared after the run.
    if (const auto error = audit::check_frozen_input(ledger_sha, ledger_sha.value_or(std::string{}))) {
        std::cerr << "association audit refused: " << *error << '\n';
        return std::nullopt;
    }
    inputs.ledger_sha256 = *ledger_sha;
    inputs.ledger_bytes = ledger_bytes->size();
    return inputs;
}

std::optional<eawr::vfs::Vfs> mount_corpus(const std::filesystem::path& game_root,
                                           const std::filesystem::path& mod_root,
                                           std::vector<eawr::vfs::ManifestResolution>& resolutions) {
    const std::array<std::pair<std::string, std::filesystem::path>, 3> roots{{
        {"mod", data_root(mod_root)}, {"expansion", game_root / "corruption" / "Data"},
        {"base", game_root / "GameData" / "Data"}}};
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, root] : roots) {
        auto resolved = eawr::vfs::resolve_manifest_mount(id, root);
        if (!resolved) {
            std::cerr << resolved.error().message << '\n';
            return std::nullopt;
        }
        resolutions.push_back(resolved.value());
        specs.push_back(std::move(resolved.value().mount));
    }
    auto mounted = eawr::vfs::Vfs::mount(specs);
    if (!mounted) {
        std::cerr << mounted.error().message << '\n';
        return std::nullopt;
    }
    return std::move(mounted.value());
}

struct Playback final {
    std::vector<Failure> failures;
    std::vector<Row> rows;
    std::size_t passed{};
    std::size_t inconsistencies{};
    // Keyed by canonical path: load_model(vfs, path) is stat + open + parse,
    // reproduced here so the exact parsed bytes are also hashed.
    std::unordered_map<std::string, ModelEntry> models;
};

ModelEntry& load_model_entry(std::unordered_map<std::string, ModelEntry>& models, const eawr::vfs::Vfs& mounted,
                             const std::string& model_path) {
    auto [entry_it, inserted] = models.try_emplace(model_path);
    ModelEntry& entry = entry_it->second;
    if (inserted) {
        auto model_record = mounted.stat(model_path);
        auto model_bytes = model_record ? mounted.open(model_path)
            : eawr::core::Result<std::vector<std::byte>>::failure(model_record.error());
        if (!model_record) entry.error = model_record.error();
        else if (!model_bytes) { entry.record = model_record.value(); entry.error = model_bytes.error(); }
        else {
            entry.record = model_record.value();
            entry.sha256 = hash(model_bytes.value());
            auto loaded = eawr::assets::load_model(model_bytes.value(), eawr::assets::source_from(model_record.value()));
            if (loaded) entry.model = std::move(loaded.value());
            else entry.error = loaded.error();
        }
    }
    return entry;
}

void record_failure(std::vector<Failure>& failures, Row& row, Failure failure) {
    row.stage = failure.stage;
    row.code = failure.code;
    row.cause = failure.cause;
    failures.push_back(std::move(failure));
    row.baseline_failure_index = failures.size();
}

void play_animation(Playback& playback, const eawr::vfs::Vfs& mounted, const corpus::ModelIndex& model_index,
                    const eawr::vfs::AssetRecord& record) {
    Row& row = playback.rows.emplace_back();
    row.record = record;
    row.qualifying_models = corpus::r0_qualifying_models(record.canonical_path, model_index);
    auto bytes = mounted.open(record.canonical_path);
    if (!bytes) {
        record_failure(playback.failures, row, {record.canonical_path, {}, {}, "animation_io", bytes.error().code,
            bytes.error().message, "Repair the VFS source before retrying playback."});
        return;
    }
    const std::string digest = hash(bytes.value());
    row.sha256 = digest;
    auto source = eawr::assets::source_from(record);
    auto animation = eawr::assets::load_animation(bytes.value(), std::move(source));
    if (!animation) {
        record_failure(playback.failures, row, {record.canonical_path, digest, {}, "animation_parse",
            animation.error().code, animation.error().message,
            "Retain this P0-06 parser failure; playback did not run."});
        return;
    }
    const auto& clip = animation.value();
    row.animation = AnimationFacts{static_cast<unsigned>(clip.version), clip.stored_frame_count,
        clip.playable_frame_count, clip.frames_per_second, clip.duration_seconds, clip.tracks.size()};
    const auto model_path = corpus::matching_model(record.canonical_path, model_index);
    if (!model_path) {
        record_failure(playback.failures, row, {record.canonical_path, digest, {}, "model_match",
            "EAWR-ANIMATION-CORPUS-0001", "no same-directory ALO basename is a prefix of the ALA basename",
            "Add an explicit licensed animation-to-model association; do not guess by track count."});
        return;
    }
    row.model_path = *model_path;
    ModelEntry& entry = load_model_entry(playback.models, mounted, *model_path);
    if (!entry.model) {
        record_failure(playback.failures, row, {record.canonical_path, digest, *model_path, "model_parse",
            entry.error ? entry.error->code : "EAWR-ANIMATION-CORPUS-0002",
            entry.error ? entry.error->message : "model load failed without a diagnostic",
            "Retain the model parser failure and retry playback after its owning format work."});
        return;
    }
    auto player = eawr::presentation::animation::Player::create(*entry.model, &clip);
    row.binding = corpus::diagnose_binding(*entry.model, clip);
    const bool consistent = row.binding->player_accepts == static_cast<bool>(player)
        && (player || (row.binding->player_code == player.error().code
            && row.binding->player_message == player.error().message));
    row.binding_consistent = consistent;
    if (!consistent) ++playback.inconsistencies;
    if (!player) {
        record_failure(playback.failures, row, {record.canonical_path, digest, *model_path, "binding",
            player.error().code, player.error().message,
            "Record an explicit model association if filename matching was wrong; otherwise repair the track contract."});
        return;
    }
    bool sampled = true;
    eawr::core::Diagnostic sample_error;
    for (const float time : {0.0F, clip.duration_seconds * 0.5F, clip.duration_seconds}) {
        auto pose = player.value().sample({time,
            eawr::presentation::animation::PlaybackMode::loop, 0.0F});
        if (!pose) {
            sampled = false;
            sample_error = pose.error();
            break;
        }
        ++row.samples_passed;
    }
    if (!sampled) {
        record_failure(playback.failures, row, {record.canonical_path, digest, *model_path, "sampling",
            sample_error.code, sample_error.message,
            "Repair presentation sampling; the accepted parser already loaded this ALA."});
        return;
    }
    ++playback.passed;
}

bool write_report(const std::filesystem::path& report_path, const std::size_t animation_count,
                  const std::vector<Failure>& failures, const std::size_t passed) {
    if (report_path.has_parent_path()) std::filesystem::create_directories(report_path.parent_path());
    std::ofstream output(report_path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << "{\n  \"schema\": \"eawr.animation-playback-corpus\",\n  \"schema_version\": 1,\n"
           << "  \"profile\": \"remake-effective\",\n  \"animation_count\": "
           << animation_count << ",\n  \"playback_passed\": " << passed
           << ",\n  \"failure_count\": " << failures.size() << ",\n  \"failures\": [\n";
    for (std::size_t index = 0; index < failures.size(); ++index) {
        const Failure& failure = failures[index];
        output << "    {\"path\": " << json(failure.path)
               << ", \"sha256\": " << json(failure.sha256)
               << ", \"model_path\": " << json(failure.model_path)
               << ", \"stage\": " << json(failure.stage)
               << ", \"code\": " << json(failure.code)
               << ", \"cause\": " << json(failure.cause)
               << ", \"follow_up\": " << json(failure.follow_up) << "}"
               << (index + 1 == failures.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    output.close();
    return static_cast<bool>(output);
}

bool write_diagnostics_file(const std::filesystem::path& diagnostics_path, const Playback& playback,
                            const std::vector<eawr::vfs::ManifestResolution>& resolutions) {
    if (diagnostics_path.has_parent_path()) std::filesystem::create_directories(diagnostics_path.parent_path());
    std::ofstream diagnostics(diagnostics_path, std::ios::binary | std::ios::trunc);
    if (!diagnostics) return false;
    write_diagnostics(diagnostics, playback.rows, resolutions, playback.passed, playback.failures.size(),
                      playback.models, playback.inconsistencies);
    diagnostics.close();
    return static_cast<bool>(diagnostics);
}

int refuse_audit(const std::string& reason) {
    std::cerr << "association audit refused: " << reason << '\n';
    return 4;
}

bool frozen_inputs_intact(const AuditInputs& audit_inputs) {
    return file_sha256(audit_inputs.ledger_path) == std::optional<std::string>(audit_inputs.ledger_sha256)
        && file_sha256(audit_inputs.metadata_path) == std::optional<std::string>(audit_inputs.metadata.sha256);
}

std::vector<audit::BaselineFailure> baseline_failures(const std::vector<Row>& rows) {
    std::vector<audit::BaselineFailure> baselines;
    for (const Row& row : rows) {
        if (!row.baseline_failure_index) continue;
        baselines.push_back({asset_identity(row.record, row.sha256), *row.baseline_failure_index,
            row.stage, row.code, row.cause, row.model_path, row.qualifying_models});
    }
    return baselines;
}

audit::CandidateModelView candidate_view(const ModelEntry& entry, const std::string& path) {
    audit::CandidateModelView view;
    view.found = entry.record.has_value() && !entry.sha256.empty();
    view.identity = entry.record ? asset_identity(*entry.record, entry.sha256) : missing_identity(path);
    view.model = entry.model ? &*entry.model : nullptr;
    if (entry.error) { view.error_code = entry.error->code; view.error_message = entry.error->message; }
    return view;
}

std::vector<audit::AuditRow> audit_failures(
    const std::vector<Row>& rows, const std::vector<audit::BaselineFailure>& baselines,
    const eawr::vfs::Vfs& mounted, const audit::ModelLookup& lookup, const audit::FrozenMetadata& frozen) {
    std::vector<audit::AuditRow> audit_rows;
    for (std::size_t index = 0, failure = 0; index < rows.size(); ++index) {
        const Row& row = rows[index];
        if (!row.baseline_failure_index) continue;
        const audit::BaselineFailure& baseline = baselines[failure++];
        std::optional<eawr::assets::Animation> clip;
        if (row.stage == "binding") {
            // Re-open through the effective VFS; the bytes must still be
            // the ones the baseline hashed, otherwise the row drifts.
            auto bytes = mounted.open(row.record.canonical_path);
            if (bytes && hash(bytes.value()) == row.sha256) {
                auto parsed = eawr::assets::load_animation(bytes.value(), eawr::assets::source_from(row.record));
                if (parsed) clip = std::move(parsed.value());
            }
        }
        audit_rows.push_back(audit::audit_failure(baseline, clip ? &*clip : nullptr, lookup, frozen));
    }
    return audit_rows;
}

audit::AuditHeader audit_header(const AuditInputs& audit_inputs,
                                const std::vector<eawr::vfs::ManifestResolution>& resolutions,
                                const std::string& fresh_sha, const std::size_t animation_count,
                                const std::size_t passed, const std::size_t failure_count) {
    const audit::FrozenMetadata& frozen = audit_inputs.metadata;
    audit::AuditHeader header;
    header.tool_name = "animation_corpus_validation --association-audit";
    header.compiler = compiler_identity();
#if defined(NDEBUG)
    header.config = "release";
#else
    header.config = "debug";
#endif
#if defined(EAWR_CORPUS_TOOL_IDENTITY)
    for (const auto& [source, digest] : eawr_corpus_tool::source_hashes)
        header.tool_sources.emplace_back(std::string(source), std::string(digest));
#endif
    for (const auto& resolution : resolutions) {
        audit::MountSummary mount{resolution.mount.layer_id, resolution.manifest_source_id, {}};
        for (const auto& archive : resolution.mount.active_archives) mount.active_archives.push_back(archive.source_id);
        header.mounts.push_back(std::move(mount));
    }
    header.frozen_ledger_sha256 = audit_inputs.ledger_sha256;
    header.frozen_ledger_bytes = audit_inputs.ledger_bytes;
    header.fresh_ledger_sha256 = fresh_sha;
    header.frozen_metadata_sha256 = frozen.sha256;
    header.frozen_metadata_bytes = frozen.bytes;
    header.frozen_metadata_source_sha256 = frozen.source_diagnostics_sha256;
    header.frozen_model_records = frozen.models.size();
    header.frozen_unrecorded_models = frozen.unrecorded.size();
    header.animation_count = animation_count;
    header.playback_passed = passed;
    header.failure_count = failure_count;
    return header;
}

std::size_t status_count(const audit::AuditSummary& summary, const std::string_view key) {
    const auto found = summary.alternatives_by_status.find(std::string(key));
    return found == summary.alternatives_by_status.end() ? std::size_t{} : found->second;
}

std::size_t identity_count(const audit::AuditSummary& summary, const std::string_view key) {
    const auto found = summary.alternatives_by_frozen_identity.find(std::string(key));
    return found == summary.alternatives_by_frozen_identity.end() ? std::size_t{} : found->second;
}

void print_audit_summary(const audit::AuditSummary& summary, const audit::FrozenMetadata& frozen) {
    std::cout << "association_audit rows=" << summary.rows << " evaluated_rows=" << summary.evaluated_rows
              << " rows_with_alternatives=" << summary.rows_with_alternatives
              << " alternative_pairs=" << summary.alternative_pairs
              << " compatible_unapproved=" << status_count(summary, audit::candidate_status::compatible_unapproved)
              << " rejected=" << status_count(summary, audit::candidate_status::rejected)
              << " parse_failed=" << status_count(summary, audit::candidate_status::parse_failed)
              << " missing=" << status_count(summary, audit::candidate_status::missing)
              << " range_impossible=" << summary.range_impossible_pairs
              << " priority_evaluated=" << summary.priority_rows_evaluated << '\n'
              << "frozen_metadata sha256=" << frozen.sha256
              << " selected_identity_matched=" << summary.selected_frozen_identity_matched
              << " alternative_identity_matched=" << identity_count(summary, audit::frozen_identity::matched)
              << " alternative_identity_unrecorded="
              << identity_count(summary, audit::frozen_identity::unrecorded) << '\n';
}

// Fail closed: no audit receipt unless the frozen inputs still hold
// their authenticated bytes, this run re-emitted the pinned ledger byte
// for byte, and every failure, R0 list and model identity matches the
// frozen metadata.
int run_audit(const AuditInputs& audit_inputs, Playback& playback, const eawr::vfs::Vfs& mounted,
              const std::vector<eawr::vfs::ManifestResolution>& resolutions,
              const std::filesystem::path& report_path, const std::size_t animation_count) {
    if (!frozen_inputs_intact(audit_inputs)) return refuse_audit("a frozen input changed during the run");
    const auto fresh_sha = file_sha256(report_path);
    if (const auto error = audit::check_frozen_input(audit_inputs.ledger_sha256, fresh_sha.value_or(std::string{})))
        return refuse_audit(*error);
    const audit::FrozenMetadata& frozen = audit_inputs.metadata;
    if (frozen.animation_count != animation_count || frozen.playback_passed != playback.passed
        || frozen.failure_count != playback.failures.size())
        return refuse_audit("baseline drift: counts differ from frozen metadata");
    const std::vector<audit::BaselineFailure> baselines = baseline_failures(playback.rows);
    if (const auto error = audit::check_frozen_failures(baselines, frozen)) return refuse_audit(*error);

    const audit::ModelLookup lookup = [&playback, &mounted](const std::string& path) {
        return candidate_view(load_model_entry(playback.models, mounted, path), path);
    };
    std::vector<audit::AuditRow> audit_rows = audit_failures(playback.rows, baselines, mounted, lookup, frozen);
    const audit::AuditSummary summary = audit::summarize(audit_rows);
    if (summary.drift_rows != 0 || summary.player_diagnosis_inconsistencies != 0) {
        for (const auto& row : audit_rows)
            if (row.status == audit::row_status::baseline_drift)
                std::cerr << "baseline drift: " << row.baseline.animation.path << ": " << row.drift_reason << '\n';
        return refuse_audit("drift_rows=" + std::to_string(summary.drift_rows)
            + " player_diagnosis_inconsistencies=" + std::to_string(summary.player_diagnosis_inconsistencies));
    }
    const audit::AuditHeader header = audit_header(audit_inputs, resolutions, *fresh_sha, animation_count,
                                                   playback.passed, playback.failures.size());
    const std::filesystem::path& audit_path = audit_inputs.output_path;
    if (audit_path.has_parent_path()) std::filesystem::create_directories(audit_path.parent_path());
    std::ofstream audit_output(audit_path, std::ios::binary | std::ios::trunc);
    if (!audit_output) return 1;
    audit::write_audit(audit_output, header, std::move(audit_rows));
    audit_output.close();
    if (!audit_output) return 1;
    if (!frozen_inputs_intact(audit_inputs)) return refuse_audit("a frozen input changed while the audit was written");
    print_audit_summary(summary, frozen);
    return 0;
}

} // namespace

int run_validation(int argc, char** argv) {
    const auto command = parse_command_line(argc, argv);
    if (!command) {
        std::cerr << "usage: animation_corpus_validation <game-root> <mod-root> <report.json> [diagnostics.json]"
                     " [--association-audit <frozen-ledger.json> <frozen-metadata.tsv> <audit.json>]\n";
        return 2;
    }
    const std::vector<std::string>& positional = command->positional;
    const auto& audit_arguments = command->audit_arguments;
    const std::filesystem::path report_path = positional[2];
    if (!outputs_distinct(report_path, positional, audit_arguments)) return 2;
    std::optional<AuditInputs> audit_inputs;
    if (audit_arguments) {
        audit_inputs = authenticate_audit_inputs(*audit_arguments);
        if (!audit_inputs) return 4;
    }
    const std::filesystem::path game_root = positional[0];
    const std::filesystem::path mod_root = positional[1];
    std::vector<eawr::vfs::ManifestResolution> resolutions;
    const auto mounted = mount_corpus(game_root, mod_root, resolutions);
    if (!mounted) return 1;
    auto alo_records = mounted.value().enumerate({}, ".alo");
    auto ala_records = mounted.value().enumerate({}, ".ala");
    if (!alo_records || !ala_records) {
        std::cerr << (!alo_records ? alo_records.error().message : ala_records.error().message) << '\n';
        return 1;
    }
    corpus::ModelIndex model_index;
    for (const eawr::vfs::AssetRecord& record : alo_records.value())
        corpus::add_model(model_index, record.canonical_path);

    Playback playback;
    playback.rows.reserve(ala_records.value().size());
    for (const eawr::vfs::AssetRecord& record : ala_records.value())
        play_animation(playback, mounted.value(), model_index, record);

    if (!write_report(report_path, ala_records.value().size(), playback.failures, playback.passed)) return 1;
    if (positional.size() == 4 && !write_diagnostics_file(positional[3], playback, resolutions)) return 1;
    std::cout << "animations=" << ala_records.value().size() << " passed=" << playback.passed
              << " failures=" << playback.failures.size()
              << " diagnosis_inconsistencies=" << playback.inconsistencies << '\n';

    if (audit_inputs) {
        const int audit_result = run_audit(*audit_inputs, playback, mounted.value(), resolutions, report_path,
                                           ala_records.value().size());
        if (audit_result != 0) return audit_result;
    }
    // Ledger failures are data, not a tool failure; a diagnosis that disagrees
    // with strict Player is a tool defect and must not produce a quiet receipt.
    return playback.inconsistencies == 0 ? 0 : 3;
}
} // namespace eawr_validation
