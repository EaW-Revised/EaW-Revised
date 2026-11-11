#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/session.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional> // include-guard: keep
#include <span>
#include <vector>

namespace eawr::script::foc::galactic_ai {

// GFS-A22 / S9b-2: the ordered commit of the AI intents. The players decide as independent
// partitions over one frozen root and retain value-owned intents; this file is the one place
// that replays them against the merged world, in ascending player ID, through the
// `readmit_native_*` admission rules, and hands every refusal back to its player.
//
// The caller (the independent-players barrier) owns the fan-out and runs commit_ordered_intents
// as the `commit` callback of finish_independent_players. Nothing here depends on how many
// workers produced the intents or in which order the partitions finished.
using IntentRecord = sim::galactic::GalacticSession::NativeActionShadow::IntentRecord;
using IntentSink = sim::galactic::GalacticSession::NativeActionShadow::IntentSink;

enum class IntentKind : std::uint8_t {
    fleet, control, cash, force, production, finish, invasion, invasion_release, journal, orbit_recheck
};
[[nodiscard]] IntentKind intent_kind(const IntentRecord&) noexcept;
[[nodiscard]] const char* intent_kind_name(IntentKind) noexcept;

enum class IntentFate : std::uint8_t {
    admitted,   // the merged world took the effect as the player projected it
    adjusted,   // cash only: admitted, but the canonical cap changed the actual delta
    refused,    // the merged world refused it: the player's on_refused ran
    journaled,  // a derived journal row was appended
    dropped,    // a derived journal row was not appended (refused predecessor, or the world journaled it)
};

struct IntentOutcome {
    sim::galactic::PlayerId player{};
    std::size_t index{};                // position in the player's own stream
    IntentKind kind{};
    IntentFate fate{};
    sim::galactic::GalacticRejectReason reason{};   // fleet, control, invasion
    sim::galactic::ProductionAdmission admission{}; // production
    sim::EntityId object{};             // fleet/control object, production build
    std::uint64_t request{};            // invasion request identity
    sim::math::Fixed actual{};          // cash: actual delta; production: price
    std::vector<sim::EntityId> actors;  // force
    bool consequence{};                 // on_refused changed persisted AI state
    bool stale_identity{};              // refused without readmission: the world had passed the intent's identities
    friend bool operator==(const IntentOutcome&, const IntentOutcome&) = default;
};

// Closures created at the player's turn, over the player's own row. They run in the commit, on
// the commit thread, in ascending player order; none of them may start a new decision.
struct IntentHooks {
    // Returns whether a persisted consequence was applied. Errors abort the commit.
    std::function<core::Result<bool>(const IntentOutcome&)> on_refused;
    std::function<core::Result<void>(const IntentOutcome&)> on_admitted;
    // Required for a force intent: rebuilds the placement against the merged world.
    sim::galactic::GalacticSession::NativeReconciledForceBuilder force_builder;
    // Optional for a production intent: pure check of the player's current goal/ledger.
    sim::galactic::GalacticSession::NativeProductionPreflight production_preflight;
};

struct PlayerIntent {
    sim::galactic::PlayerId player{};
    IntentRecord record;
    IntentHooks hooks;
};

// Collects the intents of one barrier. Every player appends only to its own slot, so the sink is
// safe to call concurrently from distinct players' partitions without a lock; the slot table is
// fixed at construction. Registration order is irrelevant: take_ordered() sorts by player ID.
class IntentLedger {
public:
    // Builds the hooks of one retained intent at the moment the player's turn emits it.
    using HookFactory = std::function<IntentHooks(const IntentRecord&, std::size_t index)>;
    explicit IntentLedger(std::span<const sim::galactic::PlayerId> players);
    IntentLedger(const IntentLedger&) = delete;
    IntentLedger& operator=(const IntentLedger&) = delete;
    // Call before the fan-out, or from the player's own partition.
    void bind_hooks(sim::galactic::PlayerId, HookFactory);
    [[nodiscard]] IntentSink sink();
    void submit(sim::galactic::PlayerId, IntentRecord, IntentHooks = {});
    [[nodiscard]] std::size_t size() const noexcept;
    // Intents from players this ledger does not know; their presence fails the commit.
    [[nodiscard]] std::size_t unknown_players() const noexcept { return unknown_.load(); }
    // Drains the ledger: ascending player ID, each player's own order preserved.
    [[nodiscard]] std::vector<PlayerIntent> take_ordered();
private:
    struct Slot {
        sim::galactic::PlayerId player{};
        HookFactory factory;
        std::vector<PlayerIntent> intents;
    };
    [[nodiscard]] Slot* find(sim::galactic::PlayerId) noexcept;
    std::vector<Slot> slots_;
    std::atomic<std::size_t> unknown_{};
};

struct IntentCommitOptions {
    // Append the economic cash row with the ACTUAL delta right after the readmission (the canonical
    // cap can differ from the projection) and drop the player's derived cash row. Off only when
    // the economic journal owner records the returned delta itself.
    bool journal_cash{true};
    // Async lane: the intents were decided ticks ago, so the world may have allocated the identities a
    // new object was projected under. Such an intent is a refusal (its player's on_refused runs); without
    // this option it is a world error, which is right for a commit whose intents were decided this tick.
    bool refuse_stale_identities{};
    // Async lane: identities from this one up belong to the lane (the world's counter already moved past them); an
    // allocating intent below it is stale. 0 judges against the world's counter, as for a commit decided this tick.
    std::uint64_t identity_floor{};
    // The same for native invasion handles. Non-zero also means the lane's invasion block is reserved in the world, so an
    // invasion intent is readmitted with the world's invasion counter rewound to its own handle.
    std::uint64_t invasion_floor{};
};

struct IntentCommitReport {
    std::vector<IntentOutcome> outcomes; // ascending player, then the player's own order
    std::size_t admitted{}, adjusted{}, refused{}, journaled{}, dropped{}, consequences{}, stale{};
    friend bool operator==(const IntentCommitReport&, const IntentCommitReport&) = default;
};

// The one order in which a slice's intents take effect: ascending player, each player's own order kept. The ordered commit and
// the window overlay both use it, so the overlaid base is built in the order the apply will use.
void order_slice_intents(std::vector<PlayerIntent>& intents);

// One planned step of a slice: the intent and its 0-based position in its player's own order (the per-player Gate resets
// at first_of_player). Shared by the ordered commit and the window overlay.
struct PlannedIntent {
    const PlayerIntent* intent{};
    std::size_t index{};
    bool first_of_player{};
};
// Whether the apply refuses `record` wholesale as stale (identity floor). Judged at the step, against the session as it is then,
// by the commit and the overlay alike, so the overlay never admits a record the apply refuses.
[[nodiscard]] bool slice_step_stale(const sim::galactic::GalacticSession&, const IntentRecord&, const IntentCommitOptions&);
// The plan over `intents` in the order of order_slice_intents; it points into `intents`, which stay as they are.
[[nodiscard]] std::vector<PlannedIntent> plan_slice(const std::vector<PlayerIntent>& intents);

// Replays the intents against `session` (canonical world access: the commit callback of
// finish_independent_players). A world error stops the commit and is returned as is. A refusal
// is an ordinary outcome: it never retries, never reruns a chain, and journals nothing.
[[nodiscard]] core::Result<IntentCommitReport> commit_ordered_intents(sim::galactic::GalacticSession&,
    std::vector<PlayerIntent> intents, const sim::PartitionExecutor&, const IntentCommitOptions& = {});
[[nodiscard]] core::Result<IntentCommitReport> commit_ordered_intents(sim::galactic::GalacticSession&,
    IntentLedger&, const sim::PartitionExecutor&, const IntentCommitOptions& = {});

} // namespace eawr::script::foc::galactic_ai
