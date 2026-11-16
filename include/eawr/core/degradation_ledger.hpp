#pragma once

// The degradation ledger: one process-wide tally of everything the runtime dropped, skipped or substituted and then
// carried on (docs/degradation-ledger.md). It is OBSERVATION ONLY: nothing in the simulation, the AI, replays,
// checkpoints, saves or hashes may read it, and no `record` call may change control flow. The summary is never part of
// a content digest.

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/load_note.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::core::degradation {

inline constexpr int summary_schema_version = 1;
// The one log line per session: "EAWR-DEGRADE-0001 N items degraded in M classes (see degradation-summary.json)".
inline constexpr std::string_view summary_line_code = "EAWR-DEGRADE-0001";
inline constexpr std::string_view summary_file_name = "degradation-summary.json";
inline constexpr std::string_view unknown_source = "unknown";
inline constexpr std::string_view stock_source = "stock";

// Bounds on what one record keeps (the rest of the text is cut, absolute paths are redacted).
inline constexpr std::size_t code_limit = 64;
inline constexpr std::size_t source_limit = 128;
inline constexpr std::size_t object_limit = 256;
inline constexpr std::size_t detail_limit = 512;

enum class Ended : std::uint8_t { ok, failed, aborted };
[[nodiscard]] std::string_view to_string(Ended value) noexcept;

// What identifies the session a summary belongs to. Empty fields are written as empty strings.
struct SessionInfo {
    std::string session_id;        // empty: generated ("<UTC yyyymmddThhmmssZ>-<8 hex>")
    std::string mode;              // "galactic" | "skirmish" | "replay" | "scenario" | "viewer" ...
    std::string run;               // the campaign id (galactic) or the map name (skirmish) the run played; empty when unknown
    std::string build;             // core::build_identity(); filled in when empty
    std::string commit;            // the commit inside the build identity; derived from `build` when empty
    std::string chain_fingerprint; // the loadcheck chain fingerprint when the caller has one
    std::vector<std::string> mods; // mounted mod ids (profile layer key or manual-folder key), in chain order (no paths)
    // The layer ids the load notes of this process carry for those mods, parallel to `mods` (the mount chain names its
    // layers "mod", "mod-parent-1", ...). `record_load_notes` maps a note's layer id to the mod id at the same index, so
    // a note counts against the real mod. Empty: the notes already carry the mod ids (the viewer binds them itself).
    std::vector<std::string> mod_layers;
};

// One (code, source) class. `objects` counts distinct objects (the gate compares this); `occurrences` counts every
// record call and can differ with frame rate. The example is the lexically smallest (object, detail) pair seen, so it
// does not depend on the order the records arrived in.
struct ClassEntry {
    std::string code;
    std::string source;
    std::uint64_t objects{};
    std::uint64_t occurrences{};
    std::string example_object;
    std::string example_detail;
    friend bool operator==(const ClassEntry&, const ClassEntry&) = default;
};

struct LedgerSnapshot {
    SessionInfo session;
    Ended ended{Ended::ok};
    std::string written_at; // the only wall-clock value; empty until a writer stamps it
    std::vector<ClassEntry> classes; // sorted by (code, source), bytewise
    [[nodiscard]] std::uint64_t items() const noexcept;   // sum of `objects`
    [[nodiscard]] std::uint64_t unattributed() const noexcept; // sum of `objects` whose source is "unknown"
};

// Records one degraded object. Thread safe (one short mutex); never call it from a sim worker's tick: sim/partition code
// keeps using LoadNoteBuffer and the owner records at the merge/publish point. `source` is the mod id or chain layer
// ("stock" for retail data, "unknown" when the site cannot tell). An empty code is ignored.
void record(std::string_view code, std::string_view source, std::string_view object, std::string_view detail);

// Load-note adapter: records every note that is a degradation (class C, or a skipped object/placement or runtime
// degradation that is not a refusal). Call it where notes are published, once the layer ids are final. Layers
// "stock", "base", "expansion" and "" become source "stock"; a layer id listed in the session's `mod_layers` becomes the
// mod id at the same index of `mods`. Returns how many notes were recorded.
std::size_t record_load_notes(std::span<const LoadNote> notes);
[[nodiscard]] std::string source_for_layer(std::string_view layer_id);
// `source_for_layer` with the session's layer-to-mod mapping applied: what `record_load_notes` uses for a note's layer.
[[nodiscard]] std::string source_for_session_layer(std::string_view layer_id);

// The source of a degradation the runtime raises without a layer of its own (a native AI provider retiring, a refusal a
// player's plan met): the campaign mod when exactly one mod is mounted, "stock" when none is, "unknown" when several are
// (the site cannot say which one authored the thing that failed). Counted, never guessed.
[[nodiscard]] std::string campaign_source();

// True for the story-script fault codes the ledger counts: EAWR-SCRIPT-0211 (error), -0215 (missing engine function),
// -0216 (stand-in result) and -0217 (warning).
[[nodiscard]] bool is_script_fault_code(std::string_view code) noexcept;

// Script-fault adapter (design rows 9 and 10). Records one fault the scripts ran into and the session carried on from.
// The object is the fault text without the tick it happened at (the same fault from many ticks is one object); the detail
// is the whole message. `source` is the mod id or "stock" the failing script comes from; empty is counted as "unknown".
// Returns false (recording nothing) for a diagnostic whose code is not a script-fault code.
bool record_script_fault(const Diagnostic& fault, std::string_view source);
// The same for a runtime line of the form "<code> [<severity>]: <text>" (the native AI's refusal lines): records it under
// `source` when the code is a script-fault code or starts with "EAWR-GCAI-" or "EAWR-GCFUND-". Returns whether it did.
bool record_runtime_line(std::string_view line, std::string_view source);

// Starts a new session: resets every record, takes `info` and generates the missing session id, build and commit.
void begin_session(SessionInfo info);
// Updates the descriptive fields of the current session without resetting the records (a process that records before
// it knows its mode, such as the viewer). Non-empty fields replace; `mods` and `mod_layers` replace when non-empty.
void describe_session(const SessionInfo& info);
void set_ended(Ended value);
// The current session's id (generated on first use, replaced by begin_session).
[[nodiscard]] std::string session_id();

[[nodiscard]] LedgerSnapshot snapshot();
// Forgets every record and the session fields. For tests.
void reset_for_tests();

// The summary body: stable ordering, no wall-clock value except `written_at` (copied from the snapshot).
[[nodiscard]] std::string summary_json(const LedgerSnapshot& snapshot);
// "EAWR-DEGRADE-0001 N items degraded in M classes (see degradation-summary.json)".
[[nodiscard]] std::string summary_line(const LedgerSnapshot& snapshot);

// The one short line the viewer's F3 overlay shows: "degraded 12 in 3 classes" (items, classes), with ", 2 unknown" appended
// when some items have no source. Empty when nothing is degraded, so the overlay draws no row. Items are distinct objects,
// like the summary's totals, so the line does not depend on frame rate.
[[nodiscard]] std::string overlay_line(const LedgerSnapshot& snapshot);

// UTC "now" as ISO 8601 ("2026-11-13T09:30:00Z"); the writers stamp `written_at` with it.
[[nodiscard]] std::string utc_iso_now();

// The commit part of a build identity ("<version>+<hash>[+dirty]"): the 40-hex token, or empty.
[[nodiscard]] std::string commit_of_build(std::string_view build);

} // namespace eawr::core::degradation
