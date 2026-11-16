#include "eawr/platform/degradation_summary.hpp"

#include "eawr/vfs/vfs.hpp"

#include <system_error>
#include <utility>

namespace eawr::platform {

PublishReport write_degradation_summary(const std::filesystem::path& file, core::degradation::LedgerSnapshot snapshot) {
    if (file.empty()) return {.failure = PublishIssue{"", "empty degradation summary path"}, .leftovers = {}};
    if (snapshot.written_at.empty()) snapshot.written_at = core::degradation::utc_iso_now();
    std::error_code error;
    const auto parent = file.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return {.failure = PublishIssue{"", "cannot create degradation summary folder: " + error.message()}, .leftovers = {}};
    }
    return publish_path_files({{"degradation summary", file, core::degradation::summary_json(snapshot)}});
}

PublishReport write_degradation_summary(const std::filesystem::path& file, const core::degradation::Ended ended,
                                        core::degradation::LedgerSnapshot* written) {
    core::degradation::set_ended(ended);
    auto snapshot = core::degradation::snapshot();
    if (written) *written = snapshot;
    return write_degradation_summary(file, std::move(snapshot));
}

ScriptFaultLedger::ScriptFaultLedger(std::shared_ptr<const vfs::Vfs> files) : files_(std::move(files)) {}

std::string ScriptFaultLedger::source_of(const core::Diagnostic& fault) {
    if (!files_ || !fault.logical_path || fault.logical_path->empty()) return {};
    if (const auto cached = sources_.find(*fault.logical_path); cached != sources_.end()) return cached->second;
    std::string source;
    if (const auto found = files_->stat(*fault.logical_path); found)
        source = core::degradation::source_for_session_layer(found.value().layer_id);
    return sources_.emplace(*fault.logical_path, std::move(source)).first->second;
}

void ScriptFaultLedger::record(const std::span<const core::Diagnostic> rows) {
    for (const auto& row : rows)
        if (core::degradation::is_script_fault_code(row.code)) core::degradation::record_script_fault(row, source_of(row));
}

} // namespace eawr::platform
