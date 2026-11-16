#pragma once

#include "eawr/core/degradation_ledger.hpp"
#include "eawr/platform/publish_files.hpp" // std::filesystem::path, as its API uses it

#include <map>
#include <memory>
#include <span>
#include <string>

namespace eawr::vfs { class Vfs; }

namespace eawr::platform {

// Stamps `written_at` when empty and publishes the snapshot atomically as `file` (temp + rename; parent folders are
// created). A failed write is reported in the result, never thrown: it must not fail the session.
[[nodiscard]] PublishReport write_degradation_summary(const std::filesystem::path& file, core::degradation::LedgerSnapshot snapshot);

// The live ledger: sets the session's end state, takes the snapshot and writes it. `written` (optional) receives the
// snapshot, so the caller can print the one log line (`core::degradation::summary_line`).
[[nodiscard]] PublishReport write_degradation_summary(const std::filesystem::path& file, core::degradation::Ended ended,
                                                      core::degradation::LedgerSnapshot* written = nullptr);

// Counts the script faults a campaign step published (design rows 9 and 10) in the live ledger, under the layer the failing
// script is mounted from. The layer is looked up once per logical path through the mounted data. A fault whose script the
// data cannot place (no path, no data, a path the mounts do not hold) is counted as "unknown". Observation only; owned by
// one thread, called outside worker ticks (the owner side of a completed step).
class ScriptFaultLedger final {
public:
    explicit ScriptFaultLedger(std::shared_ptr<const vfs::Vfs> files = {});
    // Records every row whose code is a script-fault code; other rows are ignored.
    void record(std::span<const core::Diagnostic> rows);
    // The ledger source of one fault (what `record` files it under).
    [[nodiscard]] std::string source_of(const core::Diagnostic& fault);

private:
    std::shared_ptr<const vfs::Vfs> files_;
    std::map<std::string, std::string> sources_;
};

} // namespace eawr::platform
