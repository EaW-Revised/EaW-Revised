#pragma once

#include "galactic_ai_intent_commit.hpp"

#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/foc/galactic_ai.hpp"

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

class IntentOutbox {
public:
    // Builds the hooks of one retained intent at the moment the player's turn emits it.
    using HookFactory = std::function<IntentHooks(sim::galactic::PlayerId, const IntentRecord&)>;

    explicit IntentOutbox(AsyncSchedule schedule) : schedule_(schedule) {}
    [[nodiscard]] const AsyncSchedule& schedule() const noexcept { return schedule_; }
    void bind_hooks(HookFactory factory) { factory_ = std::move(factory); }

    // Lane side. begin_batch returns where the batch's provisional identities start; the batch ends
    // with end_batch (success) or abandon_batch (the attempt faulted: its slices are dropped and the
    // lane frontier returns to what it was).
    [[nodiscard]] LaneIdentities begin_batch(std::uint64_t snapshot_tick, LaneIdentities live);
    void begin_virtual_tick(std::uint32_t virtual_tick);
    // Appends to the open virtual tick; usable as the barrier's intent sink.
    [[nodiscard]] IntentSink sink();
    // Commands of the open virtual tick, in issue order. False (nothing retained) outside an open batch.
    [[nodiscard]] bool add_commands(std::vector<authoritative::ScriptCommand>&);
    void end_batch();
    void abandon_batch();
    [[nodiscard]] bool open() const noexcept { return open_; }

    // World side: every slice due at `tick` or earlier, in ascending apply tick.
    [[nodiscard]] std::vector<OutboxSlice> take_due(std::uint64_t tick);

    [[nodiscard]] LaneIdentities frontier() const noexcept { return frontier_; }
    // The world's entity counter when the oldest pending batch began; 0 while nothing is pending. Identities at or above
    // it belong to the lane (the world reserves the range once the batch has ended), those below were the sim's.
    [[nodiscard]] std::uint64_t lane_floor() const noexcept { return floor_; }
    // The same floor for native invasion handles (the world reserves the invasion block too): the lowest pending handle,
    // 0 while nothing is pending.
    [[nodiscard]] std::uint64_t lane_invasion_floor() const noexcept;
    // Whether a pending intent holds this invasion handle: the AI decided it, its slice has not applied yet, so the world has no
    // row for it. The world's counter cannot tell: it already stands at the end of the reserved block.
    [[nodiscard]] bool holds_invasion(std::uint64_t handle) const noexcept;
    [[nodiscard]] const std::vector<OutboxSlice>& pending_slices() const noexcept { return slices_; }
    // Changes whenever the pending work or the lane frontier changes (never read by gameplay).
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    // Load: replaces the pending work; the hooks of every retained intent are derived again.
    void restore(LaneIdentities frontier, std::uint64_t floor, std::vector<OutboxSlice> slices);
    [[nodiscard]] std::size_t slices() const noexcept { return slices_.size(); }
    [[nodiscard]] std::size_t pending_intents() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> next_apply_tick() const noexcept;

private:
    void add(sim::galactic::PlayerId, IntentRecord);
    [[nodiscard]] OutboxSlice* open_slice();

    AsyncSchedule schedule_;
    HookFactory factory_;
    std::vector<OutboxSlice> slices_;   // ascending apply tick
    LaneIdentities frontier_{};         // monotone across batches
    std::uint64_t floor_{};
    LaneIdentities before_batch_{};
    std::size_t batch_first_slice_{};
    std::uint64_t snapshot_tick_{};
    std::uint32_t virtual_tick_{};
    bool open_{};
    std::uint64_t generation_{};
};

} // namespace eawr::script::foc::galactic_ai
