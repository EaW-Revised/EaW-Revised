#pragma once

#include "eawr/campaign/galactic_ai_services.hpp"
#include "eawr/core/load_note.hpp"
#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/script/numeric/lua_number.hpp"
#include "../script/foc/galactic_ai_actions.hpp"
#include "../script/foc/galactic_ai_intent_refusal.hpp"
#include "../script/foc/galactic_ai_outbox.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
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
// #6917: a story reward changed one AI player's type (the control-switch reward). It waits here like every other world
// notice and is applied at the next window start, so the AI's own state never changes from the story service.
struct GalacticAiTypeSwitch {
    sim::galactic::PlayerId player{};
    std::string type; // the authored AI player type name, as the reward carried it
    bool operator==(const GalacticAiTypeSwitch&) const = default;
};
class GalacticAiLaneInbox {
public:
    using Entry = std::variant<script::foc::galactic_ai::NativeActions::Delivery, GalacticAiBattleFacts,
        sim::galactic::GalacticSession::CreditNotification, GalacticAiRecordedBill, GalacticAiTypeSwitch,
        script::foc::galactic_ai::refusal::Consequence>;
    void push(Entry entry) { entries_.push_back(std::move(entry)); ++generation_; }
    // Hands the queue to the caller and empties it (a failed application never leaves entries behind to apply twice).
    [[nodiscard]] std::vector<Entry> take() { ++generation_; auto taken = std::move(entries_); entries_.clear(); return taken; }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    // #6917: the AI player types the story's control switches have set (player -> type, upper case), as the sim side applied them at a
    // window's pre-cut. The sim-side readers of a player's difficulty (credit multipliers) derive from it, so they follow a restore
    // without reading the AI's own state. Saved with the block (the block is absent while this and the queue are both empty).
    void note_type(sim::galactic::PlayerId player, std::string type) { applied_types_.insert_or_assign(player, std::move(type)); ++generation_; }
    [[nodiscard]] const std::map<sim::galactic::PlayerId, std::string>& applied_types() const noexcept { return applied_types_; }
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
    std::map<sim::galactic::PlayerId, std::string> applied_types_;
    std::uint64_t generation_{};
};

// #6337: the actors the AI lost through its own window (the async stage) that the world has not applied yet. The stage tells the AI
// freestore at once, but the slice that really removes the actor applies D ticks later, and in between the world goes on writing
// the live actor (a move, a relocation): that `changed` reaches an AI that already dropped it. Each entry names the slice that
// holds the loss (the window's snapshot tick and the virtual tick it was decided in). When a world delivery is applied:
//   - a world `lost` for an entry clears it (the freestore already lacks the actor: a no-op);
//   - a world `changed` while that slice is still pending is ignored (the loss lands later, and its `lost` follows);
//   - a world `changed` after that slice applied means the world refused the loss and kept the actor: the actor is born again
//     in the AI as a fresh actor from the delivery's facts (its AI-usage writes follow as a usage-only change), and the entry clears;
//   - a world `created` for an entry's actor clears it and applies as sent (the world made the actor itself).
// The set never holds more than its capacity (default: the live-object bound); over it the oldest source is evicted.
// Nothing else is relaxed: an unknown actor without an entry still refuses. Saved after the story globals, hashed with the lane;
// an empty set adds no bytes.
class GalacticAiStageLosses {
public:
    // The set never holds more entries than `capacity` (default: the live-object bound): a loss the world answers with no notice at
    // all would otherwise stay forever. Over the bound the oldest source is evicted (ordered by snapshot tick, virtual tick, actor).
    explicit GalacticAiStageLosses(std::size_t capacity = sim::galactic::galactic_max_objects) : capacity_(capacity) {}
    using Change = sim::galactic::GalacticSession::NativeActorChange;
    struct Source {
        std::uint64_t snapshot_tick{}; // the window's snapshot tick s
        std::uint32_t virtual_tick{};  // slice j of that batch
        bool operator==(const Source&) const = default;
    };
    // Whether the slice named by the source still waits in the outbox.
    class Pending {
    public:
        // Non-owning view of a callable: it must outlive the call it is passed to (a temporary argument does).
        template<class F> Pending(const F& test) noexcept
            : context_(&test), call_([](const void* context, const Source& source) { return (*static_cast<const F*>(context))(source); }) {}
        [[nodiscard]] bool operator()(const Source& source) const { return call_(context_, source); }
    private:
        const void* context_;
        bool (*call_)(const void*, const Source&);
    };
    [[nodiscard]] bool empty() const noexcept { return losses_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return losses_.size(); }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const std::map<sim::EntityId, Source>& entries() const noexcept { return losses_; }
    // A `lost` the window delivered: the newest source wins (a later window loses the same actor again while the first slice waits).
    void record(sim::EntityId actor, Source source);
    // A window's attempt: what it recorded is undone when the attempt is abandoned (its freestore rolls back with it) and kept when the
    // batch ends. Never saved: no save happens inside a window.
    void begin_batch() { journal_.clear(); }
    void end_batch() { journal_.clear(); }
    void abandon_batch();
    // The changes of one world delivery, rewritten as described above. Mutating: clears the entries it settles.
    [[nodiscard]] std::vector<Change> reconcile(std::span<const Change> changes, const Pending& pending);
    // The same rewrite on a copy: the pending-replay view of deliveries that are still queued.
    [[nodiscard]] std::vector<Change> reconciled(std::span<const Change> changes, const Pending& pending) const;
    void encode(std::vector<std::uint8_t>& bytes) const;
    [[nodiscard]] core::Result<std::size_t> restore_prefix(std::span<const std::uint8_t> bytes);
private:
    static std::vector<Change> rewrite(std::map<sim::EntityId, Source>& losses, std::span<const Change> changes, const Pending& pending);
    std::size_t capacity_;
    std::map<sim::EntityId, Source> losses_;
    std::vector<std::pair<sim::EntityId, std::optional<Source>>> journal_; // the open attempt's records, for its rollback
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
// Says an isolated fiscal refusal once per AI player and refusal for the session (not saved: a loaded session says it again).
class IsolatedFaultOnce {
public:
    // True the first time this player is refused with this diagnostic.
    [[nodiscard]] bool first(sim::galactic::PlayerId player, const core::Diagnostic& fault) {
        return seen_.emplace(player, fault.code + ": " + fault.message).second;
    }
private:
    std::set<std::pair<sim::galactic::PlayerId, std::string>> seen_;
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
    // A fiscal cycle the view's ledger refuses is that AI player's: it is skipped for that player (the ledger keeps its previous
    // state), reported to the sink given here, and the credit still succeeds, as in the AI's own funding owner.
    using FaultObserver = script::foc::ai::GalacticFunding::FaultObserver;
    void bind_fault_observer(std::weak_ptr<FaultObserver> observer) { fault_observer_ = std::move(observer); }
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
    // Step 2c: what the per-tick AI scheduler hash can read. none_visible: no window publication is visible yet (the bootstrap ticks);
    // published: the visible publication's digest; missing: a visible publication without its digest (an engine invariant the
    // lane keeps; the world degrades to a fixed marker and reports it, it never reads the live scheduler).
    enum class SchedulerDigest { none_visible, published, missing };
    [[nodiscard]] SchedulerDigest scheduler_digest_state() const noexcept {
        if (!base_) return SchedulerDigest::none_visible;
        return base_->scheduler_digest.empty() ? SchedulerDigest::missing : SchedulerDigest::published;
    }
    // The deterministic stand-in every peer hashes when the digest is missing.
    static constexpr std::string_view missing_scheduler_digest_marker = "eawr-gcai-no-published-scheduler-digest";
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
    // `fault` (optional) receives the refusal of a fiscal credit that was skipped for its player.
    static core::Result<void> replay(const LogEntry&, Ledgers& budgets, Rows& billing_rows, std::optional<core::Diagnostic>* fault = nullptr);
    core::Result<void> rebuild();
    std::optional<GalacticAiBillingPublication> base_;
    std::vector<GalacticAiBillingPublication> pending_;
    std::vector<LogEntry> log_;
    std::uint64_t next_sequence_{1}, generation_{};
    Ledgers ledgers_;
    Rows rows_;
    std::weak_ptr<FaultObserver> fault_observer_; // never saved
};
// Async step 3b: a window the lane finished and the world has not adopted yet. The world adopts it at the start of the AI tick
// `adopt_at` (H), before that tick applies or publishes anything: the batch joins the outbox, the publication (with the window's
// diagnostics and load notes) the billing view. With no adoption delay the world adopts each part as the window ends and none
// waits. Saved after the stage losses; nothing while no window waits.
struct GalacticAiFinishedWindow {
    std::uint64_t adopt_at{};
    script::foc::galactic_ai::OutboxBatch::Contents batch;
    std::optional<GalacticAiBillingPublication> publication;
};
[[nodiscard]] core::Result<void> encode_finished_window(std::vector<std::uint8_t>& bytes, std::uint64_t adopt_at,
    const script::foc::galactic_ai::OutboxBatch::Contents& batch, const std::optional<GalacticAiBillingPublication>& publication);
// Reads its own block from the front (none: no window waits) and says how many bytes it used.
[[nodiscard]] core::Result<std::size_t> restore_finished_window_prefix(std::span<const std::uint8_t> bytes,
    const script::foc::galactic_ai::AsyncSchedule& schedule, std::optional<GalacticAiFinishedWindow>& out);
} // namespace eawr::campaign
