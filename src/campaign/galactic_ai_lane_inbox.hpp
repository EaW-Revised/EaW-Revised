#pragma once

#include "eawr/campaign/galactic_ai_services.hpp"
#include "eawr/core/load_note.hpp"
#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/script/numeric/lua_number.hpp"
#include "../script/foc/galactic_ai_actions.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace eawr::campaign {
// Async AI A2.1: the sim->AI inbox. Outside an async window every world delivery to the AI (production outcome, movement event,
// actor lifecycle batch, battle notice) is queued here as owned plain data; the runtime applies the queue in arrival order at the
// next window start, so the window sees exactly the events of the world steps before it, however the window is run. The queue is
// part of the AI's saved state: an empty queue adds no bytes, so its saved form and digest stay those of the outbox alone.
// W1 S3: a bill decided outside the window against the billing view; the AI applies the recorded effect, it never decides again.
struct GalacticAiRecordedBill {
    sim::EntityId build{};
    script::foc::galactic_ai::BillEffect effect;
    bool operator==(const GalacticAiRecordedBill&) const = default;
};
class GalacticAiLaneInbox {
public:
    using Entry = std::variant<script::foc::galactic_ai::NativeActions::Delivery, GalacticAiBattleFacts,
        sim::galactic::GalacticSession::CreditNotification, GalacticAiRecordedBill>;
    void push(Entry entry) { entries_.push_back(std::move(entry)); ++generation_; }
    // Hands the queue to the caller and empties it (a failed application never leaves entries behind to apply twice).
    [[nodiscard]] std::vector<Entry> take() { ++generation_; auto taken = std::move(entries_); entries_.clear(); return taken; }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
    // Changes whenever the queue changes (never read by gameplay).
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    // The saved block: nothing when the queue is empty, else a tagged block appended after the outbox state.
    void encode(std::vector<std::uint8_t>& bytes) const;
    // Reads a block written by encode (bytes empty: an empty queue). Refuses unknown tags, bad framing and trailing bytes.
    [[nodiscard]] core::Result<void> restore(std::span<const std::uint8_t> bytes);
    // The same for an inbox block at the start of `bytes` (absent if another block starts there); returns the bytes it used.
    [[nodiscard]] core::Result<std::size_t> restore_prefix(std::span<const std::uint8_t> bytes);
private:
    std::vector<Entry> entries_;
    std::uint64_t generation_{};
};

// W1 S3: the AI budget as the world sees it between AI windows (async schedule). It is the AI's ledgers and the billing columns of
// its purchase rows as published by an earlier window (visible from that window's apply tick V = s + D), with every credit and bill
// the world logged since that window's drain replayed through the same pure funding functions. Bills outside the window are decided
// against it; the AI receives the same credits and recorded bills through the inbox and applies them at its next window start, so
// right after that drain the AI holds exactly the newest captured publication with the log replayed (checked by the runtime).
// W1 S5 (async lane): the story-owned GlobalValue store. Story scripts read and write it at once; the AI's own writes reach it when
// their window's publication becomes visible (merged in commit order); the AI's registry is rebuilt from it at each window start.
// Sim-owned: saved with the lane and hashed live.
class GalacticStoryGlobals {
public:
    using Scalar = std::variant<script::numeric::LuaNumber, std::string, bool>;
    using Values = std::map<std::string, Scalar, std::less<>>;
    using Write = std::pair<std::string, std::optional<Scalar>>; // no value = deleted
    [[nodiscard]] bool active() const noexcept { return active_; }
    void activate(Values initial) { active_ = true; values_ = std::move(initial); ++generation_; }
    [[nodiscard]] const Values& values() const noexcept { return values_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    // Story side: one script batch's GlobalValue.Set commands, admitted with the AI registry's archive quotas and notes.
    void commit(script::authoritative::ServiceReport&);
    // World side, at a publication's tick: the AI's writes in commit order; a write the quotas refuse becomes a note.
    void merge(std::span<const Write>, std::vector<core::LoadNote>& notes);
    // Saved after the billing view; nothing while inactive.
    void encode(std::vector<std::uint8_t>&) const;
    [[nodiscard]] core::Result<std::size_t> restore_prefix(std::span<const std::uint8_t>);
private:
    // Empty when admitted (and applied); otherwise the refusal.
    std::string admit(const std::string& key, const std::optional<Scalar>& value);
    bool active_{};
    Values values_;
    std::uint64_t generation_{};
};
struct GalacticAiBillingPublication {
    std::uint64_t visible_at{}; // AI tick from which the world bills against it
    std::uint64_t log_cut{};    // the last log entry it already contains
    std::map<sim::galactic::PlayerId, sim::galactic::funding::Ledger> ledgers;
    std::map<sim::EntityId, script::foc::galactic_ai::BillRow> rows;
    // W1 S4: taken at the same commit. The AI-owned native sync digests (component -> digest; the sim-owned lane is digested live)
    // and the AI scheduler's sync hash. From `visible_at` the per-tick hashes read these instead of the AI's live state.
    std::map<std::string, std::string, std::less<>> ai_digests;
    std::string scheduler_digest;
    // W1 S6: the story-plot edits the window's plan scripts made (Get_Story_Plot), in command order; the world applies them when
    // the publication becomes visible.
    std::vector<sim::galactic::story::PlotCommand> plot_commands;
    // W1 S6: the load diagnostics the window raised (route and path-table notes, a disabled freestore), appended at that tick too.
    std::vector<std::string> diagnostics;
    // W1 S6 (review #6425): the runtime load notes the window raised (its scripts' notes, battle-delivery and native provider
    // retirement notes), in order; the world appends them at that tick before the refusals of the plot edits that apply then.
    std::vector<core::LoadNote> load_notes;
    // W1 S5: the AI's GlobalValue writes in the window, in commit order (merged into the story store at `visible_at`).
    std::vector<GalacticStoryGlobals::Write> global_writes;
    bool operator==(const GalacticAiBillingPublication&) const = default;
};
// What the world takes over from the publications that became visible at a tick, oldest first (W1 S6).
struct GalacticAiDueOutput {
    std::vector<sim::galactic::story::PlotCommand> plot_commands;
    std::vector<std::string> diagnostics;
    std::vector<core::LoadNote> load_notes;
    std::vector<GalacticStoryGlobals::Write> global_writes;
};
class GalacticAiBillingView {
public:
    using Credit = sim::galactic::GalacticSession::CreditNotification;
    using Notice = sim::galactic::GalacticSession::NativeProductionNotice;
    using Ledgers = std::map<sim::galactic::PlayerId, sim::galactic::funding::Ledger>;
    using Rows = std::map<sim::EntityId, script::foc::galactic_ai::BillRow>;
    [[nodiscard]] bool active() const noexcept { return base_.has_value(); }
    [[nodiscard]] bool has_pending() const noexcept { return !pending_.empty(); }
    // World side. A bill decided against the view and logged (no value: the view has no row for that build, the caller decides
    // as before). A credit applied to the view and logged.
    [[nodiscard]] std::optional<core::Result<script::foc::galactic_ai::BillDecision>> bill(const Notice&,
        std::span<const sim::galactic::ProductionPlayer>);
    [[nodiscard]] core::Result<void> credit(const Credit&);
    // Lane side: a window's publication (taken after its commit), and the switch to the newest one whose tick has come.
    void capture(GalacticAiBillingPublication);
    // `due` (optional) receives the plot commands and diagnostics of every publication that became due, oldest first; the view
    // keeps none of them (a save after this does not hand them over again).
    [[nodiscard]] core::Result<void> publish_due(std::uint64_t ai_tick, GalacticAiDueOutput* due = nullptr);
    [[nodiscard]] std::uint64_t log_end() const noexcept { return next_sequence_ - 1; }
    // The newest captured publication with the log after its cut replayed: what the AI holds right after its drain.
    [[nodiscard]] core::Result<std::pair<Ledgers, Rows>> latest_replayed() const;
    [[nodiscard]] const Ledgers& ledgers() const noexcept { return ledgers_; }
    [[nodiscard]] const Rows& rows() const noexcept { return rows_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    // W1 S4: the newest visible publication (its digests feed the per-tick hashes); null while inactive.
    [[nodiscard]] const GalacticAiBillingPublication* visible() const noexcept { return base_ ? &*base_ : nullptr; }
    // Saved after the inbox block; nothing while inactive.
    [[nodiscard]] core::Result<void> encode(std::vector<std::uint8_t>& bytes) const;
    [[nodiscard]] core::Result<void> restore(std::span<const std::uint8_t> bytes);
    // Reads its own block from the front (none: inactive) and says how many bytes it used.
    [[nodiscard]] core::Result<std::size_t> restore_prefix(std::span<const std::uint8_t> bytes);
    // W1 S5: the AI's writes of the publications captured but not visible yet, oldest first (its registry replays them).
    [[nodiscard]] std::vector<GalacticStoryGlobals::Write> pending_global_writes() const;
private:
    struct LogEntry {
        std::uint64_t sequence{};
        std::variant<Credit, GalacticAiRecordedBill> entry;
    };
    static core::Result<void> replay(const LogEntry&, Ledgers& budgets, Rows& billing_rows);
    core::Result<void> rebuild();
    std::optional<GalacticAiBillingPublication> base_;
    std::vector<GalacticAiBillingPublication> pending_;
    std::vector<LogEntry> log_;
    std::uint64_t next_sequence_{1}, generation_{};
    Ledgers ledgers_;
    Rows rows_;
};
} // namespace eawr::campaign
