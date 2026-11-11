#pragma once

#include "galactic_ai_outbox.hpp"

#include "eawr/core/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace eawr::script::foc::galactic_ai {

// Async AI A2.4: the saved form of the lane's pending work (native component "galactic-async").
//
// A slice in flight is value-owned data: the intents of one virtual tick (player, request, receipts and
// identity spans) and the script commands the AI scripts issued in it. Everything a closure carries is
// re-derived from the request when the state is loaded (the outbox's hook factory), so no closure is
// saved. A force intent keeps its request and actors only: the readmission rebuilds the placement
// against the world it is applied to, the lane's projected placement is never read again.
//
// The state is a function of the tick alone, never of timing: the inline batch driver has no work in
// flight between ticks, and the threaded lane joins before a save.
inline constexpr std::uint32_t outbox_state_version = 2;

struct DecodedOutboxState {
    AsyncSchedule schedule;
    std::uint64_t ai_ticks{};
    LaneIdentities frontier;
    std::uint64_t floor{};
    std::vector<OutboxSlice> slices;
    // Bytes after the outbox state, kept for the owner of a block saved behind it (the runtime's inbox); empty when there are none.
    std::vector<std::uint8_t> tail;
};

[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_outbox_state(const IntentOutbox&, std::uint64_t ai_ticks);
// allow_tail: bytes after the outbox state are returned in `tail` instead of refused.
[[nodiscard]] core::Result<DecodedOutboxState> decode_outbox_state(std::span<const std::uint8_t>, bool allow_tail = false);

} // namespace eawr::script::foc::galactic_ai
