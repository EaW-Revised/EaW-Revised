#pragma once

#include "galactic_ai_intent_commit.hpp"

#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/foc/galactic_ai.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional> // include-guard: keep
#include <optional>
#include <vector>

namespace eawr::script::foc::galactic_ai {

// Async AI A2.2: the lane's outbox. A batch of virtual ticks decides over one frozen world and
// retains its intents (value-owned IntentRecords plus the hooks of the player's turn) per virtual
// tick. Slice j of the batch snapshotted at tick s is applied at apply_tick(schedule, s, j) through
// commit_ordered_intents against the live world; the outbox only keeps and releases the slices, it
// never touches the world.
//
// Identities. The readmit_native_* functions accept a first identity at or above the live
// frontier only. The lane therefore starts every batch at max(live frontier + gap, the lane's own
// frontier): its provisional identities are monotone across batches, whatever the world did
// meanwhile. The gap absorbs the identities the world allocates between the snapshot and the apply
// tick; an intent the world has reached anyway is refused in the commit (never merged). The
// synchronous reference applies in the same tick it decided in, so it needs no gap and allocates
// exactly the identities the unbatched barrier did.
inline constexpr std::uint64_t async_identity_gap = 65'536;
[[nodiscard]] constexpr std::uint64_t identity_gap(const AsyncSchedule& schedule) noexcept {
    return schedule.apply_delay == 0 ? 0 : async_identity_gap;
}

// Fixed identity block. An asynchronous window owns [start, start + async_identity_block) of each counter.
// The world reserves the entity block at window start, so entity allocation does not depend on how much the
// window used or how fast it ran; the invasion block is reserved the same way. The block exceeds
// galactic_max_objects, the bound on live objects, so a window cannot reach its end except through a runaway; past the end
// a command is refused deterministically. Identities are 64-bit, so burning the unused part of a block is free.
inline constexpr std::uint64_t async_identity_block = 1'048'576;
[[nodiscard]] constexpr std::uint64_t identity_block(const AsyncSchedule& schedule) noexcept {
    return schedule.apply_delay == 0 ? 0 : async_identity_block;
}

// The lane's two identity frontiers (entities, native invasion requests).
struct LaneIdentities {
    sim::EntityId id{};
    std::uint64_t invasion{};
    friend bool operator==(const LaneIdentities&, const LaneIdentities&) = default;
};

// One virtual tick's decisions: the players' intents in stream order (player ascending, each
// player's own order), ready for commit_ordered_intents.
struct OutboxSlice {
    std::uint64_t snapshot_tick{};
    std::uint32_t virtual_tick{}; // 1..batch_ticks
    std::uint64_t apply_tick{};
    std::vector<PlayerIntent> intents;
    // The commands the AI scripts of this virtual tick issued: they leave the AI at the apply tick, like the intents.
    std::vector<authoritative::ScriptCommand> commands;
};

// Builds the hooks of one retained intent at the moment the player's turn emits it.
using IntentHookFactory = std::function<IntentHooks(sim::galactic::PlayerId, const IntentRecord&)>;

// Async step 3b: one window's batch. The lane builds it (the world never writes it) and the outbox adopts its contents at the
// adoption tick H (IntentOutbox::adopt); until then a closed batch is "finished, not adopted". begin() starts over the frontier
// and floor the outbox had adopted so far (copied at the window start), so the identities are those of a batch opened on the
// outbox itself.
class OutboxBatch {
public:
    struct Contents {
        std::uint64_t snapshot_tick{};
        LaneIdentities frontier{};       // the end of the batch's identity block: the outbox frontier once adopted
        std::uint64_t floor{};           // the outbox lane floor once adopted (the live entity counter when nothing was pending)
        std::vector<OutboxSlice> slices; // ascending virtual tick
    };
    explicit OutboxBatch(AsyncSchedule schedule = synchronous_reference_schedule) : schedule_(schedule) {}
    void bind_hooks(IntentHookFactory factory) { factory_ = std::move(factory); }
    // Where the batch's provisional identities start (see IntentOutbox: monotone across batches, the gap above the live frontier).
    [[nodiscard]] LaneIdentities begin(std::uint64_t snapshot_tick, LaneIdentities adopted_frontier, std::uint64_t adopted_floor,
        LaneIdentities live);
    void begin_virtual_tick(std::uint32_t virtual_tick) { virtual_tick_ = virtual_tick; }
    // Appends to the open virtual tick; usable as the barrier's intent sink.
    [[nodiscard]] IntentSink sink();
    // Commands of the open virtual tick, in issue order. False (nothing retained) outside an open batch.
    [[nodiscard]] bool add_commands(std::vector<authoritative::ScriptCommand>&);
    // The batch is final: its contents wait for adoption (take()).
    void end() { if (open_) { open_ = false; closed_ = true; virtual_tick_ = 0; } }
    // An attempt faulted (open) or the batch is dropped (closed): nothing reaches the outbox.
    void abandon() noexcept { open_ = closed_ = false; virtual_tick_ = 0; contents_ = {}; }
    [[nodiscard]] bool open() const noexcept { return open_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] bool active() const noexcept { return open_ || closed_; }
    [[nodiscard]] std::uint64_t snapshot_tick() const noexcept { return contents_.snapshot_tick; }
    // The virtual tick being decided (0 outside one): what a stage loss is filed under.
    [[nodiscard]] std::uint32_t virtual_tick() const noexcept { return virtual_tick_; }
    [[nodiscard]] const Contents& contents() const noexcept { return contents_; }
    [[nodiscard]] bool holds_slice(std::uint64_t snapshot, std::uint32_t virtual_tick_index) const noexcept;
    [[nodiscard]] bool holds_invasion(std::uint64_t handle) const noexcept;
    // Whether an intent of the batch finishes (cancels) this queued build: the AI already failed its purchase row, the world still
    // shows the queue entry until the slice applies.
    [[nodiscard]] bool holds_finish(sim::EntityId build) const noexcept;
    // The lowest invasion handle an intent of the batch holds (0: none).
    [[nodiscard]] std::uint64_t lowest_invasion() const noexcept;
    [[nodiscard]] std::size_t intents() const noexcept;
    // The closed batch's contents, for adoption; the batch is empty afterwards.
    [[nodiscard]] Contents take();
    // Load: a closed batch saved before its adoption; the hooks of every intent are derived again.
    void restore_closed(Contents);
private:
    [[nodiscard]] OutboxSlice* open_slice();
    AsyncSchedule schedule_;
    IntentHookFactory factory_;
    Contents contents_;
    std::uint32_t virtual_tick_{};
    bool open_{}, closed_{};
};

class IntentOutbox {
public:
    using HookFactory = IntentHookFactory;

    explicit IntentOutbox(AsyncSchedule schedule) : schedule_(schedule), own_(schedule) {}
    [[nodiscard]] const AsyncSchedule& schedule() const noexcept { return schedule_; }
    void bind_hooks(HookFactory factory) { factory_ = factory; own_.bind_hooks(std::move(factory)); }

    // World side (async step 3b): a lane batch's contents become pending work (the adoption tick H).
    void adopt(OutboxBatch::Contents);

    // A batch built on the outbox itself and adopted at its end (the inline form; tests). begin_batch returns where the batch's
    // provisional identities start; the batch ends with end_batch (success) or abandon_batch (the attempt faulted: its slices are
    // dropped and the lane frontier returns to what it was). While it is open the readers below include it.
    [[nodiscard]] LaneIdentities begin_batch(std::uint64_t snapshot_tick, LaneIdentities live);
    void begin_virtual_tick(std::uint32_t virtual_tick);
    // Appends to the open virtual tick; usable as the barrier's intent sink.
    [[nodiscard]] IntentSink sink();
    // Commands of the open virtual tick, in issue order. False (nothing retained) outside an open batch.
    [[nodiscard]] bool add_commands(std::vector<authoritative::ScriptCommand>&);
    void end_batch();
    void abandon_batch();
    [[nodiscard]] bool open() const noexcept { return own_.open(); }
    // The open batch's snapshot tick and virtual tick being decided (0 outside a virtual tick): what a stage loss is filed under.
    [[nodiscard]] std::uint64_t snapshot_tick() const noexcept { return own_.snapshot_tick(); }
    [[nodiscard]] std::uint32_t virtual_tick() const noexcept { return own_.virtual_tick(); }
    // Whether slice j of the batch snapshotted at s still waits to be applied.
    [[nodiscard]] bool holds_slice(std::uint64_t snapshot, std::uint32_t virtual_tick_index) const noexcept {
        return own_.holds_slice(snapshot, virtual_tick_index) || std::any_of(slices_.begin(), slices_.end(), [&](const OutboxSlice& slice) {
            return slice.snapshot_tick == snapshot && slice.virtual_tick == virtual_tick_index;
        });
    }

    // World side: every slice due at `tick` or earlier, in ascending apply tick.
    [[nodiscard]] std::vector<OutboxSlice> take_due(std::uint64_t tick);

    [[nodiscard]] LaneIdentities frontier() const noexcept { return own_.active() ? own_.contents().frontier : frontier_; }
    // The world's entity counter when the oldest pending batch began; 0 while nothing is pending. Identities at or above
    // it belong to the lane (the world reserves the range once the batch has ended), those below were the sim's.
    [[nodiscard]] std::uint64_t lane_floor() const noexcept { return own_.active() ? own_.contents().floor : floor_; }
    // The same floor for native invasion handles (the world reserves the invasion block too): the lowest pending handle,
    // 0 while nothing is pending.
    [[nodiscard]] std::uint64_t lane_invasion_floor() const noexcept;
    // Whether a pending intent holds this invasion handle: the AI decided it, its slice has not applied yet, so the world has no
    // row for it. The world's counter cannot tell: it already stands at the end of the reserved block.
    [[nodiscard]] bool holds_invasion(std::uint64_t handle) const noexcept;
    // Whether a pending intent finishes (cancels) this queued build: see OutboxBatch::holds_finish.
    [[nodiscard]] bool holds_finish(sim::EntityId build) const noexcept;
    [[nodiscard]] const std::vector<OutboxSlice>& pending_slices() const noexcept { return slices_; }
    // Changes whenever the pending work or the lane frontier changes (never read by gameplay).
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    // Load: replaces the pending work; the hooks of every retained intent are derived again.
    void restore(LaneIdentities frontier, std::uint64_t floor, std::vector<OutboxSlice> slices);
    [[nodiscard]] std::size_t slices() const noexcept { return slices_.size() + own_.contents().slices.size(); }
    [[nodiscard]] std::size_t pending_intents() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> next_apply_tick() const noexcept;

private:
    AsyncSchedule schedule_;
    HookFactory factory_;
    std::vector<OutboxSlice> slices_;   // ascending apply tick
    LaneIdentities frontier_{};         // monotone across batches
    std::uint64_t floor_{};
    OutboxBatch own_;                   // the inline batch (begin_batch .. end_batch)
    std::uint64_t generation_{};
};

} // namespace eawr::script::foc::galactic_ai
