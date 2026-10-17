#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/seat.hpp"

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <optional>
#include <span>
#include <vector>

namespace eawr::net {

using PeerIndex = std::uint32_t;
inline constexpr std::size_t max_match_seats = sim::max_match_seats;
inline constexpr std::size_t max_match_human_sides = sim::max_match_human_sides;
enum class LinkState : std::uint8_t { connecting, open, closed };
struct Received {
    PeerIndex from{};
    std::vector<std::uint8_t> frame;
};

// Local transport observations only; never part of sealed input or simulation state.
// Jitter is the smoothed absolute difference between consecutive RTT samples.
struct LinkStats {
    std::chrono::microseconds last_rtt{}, smoothed_rtt{}, jitter{};
    std::uint64_t sample_count{};
};

// One reliable, ordered stream per remote peer; the transport frames whole messages.
class MessageTransport {
public:
    virtual ~MessageTransport() = default;
    virtual core::Result<void> send(PeerIndex to, std::span<const std::uint8_t> frame) = 0;
    virtual std::optional<Received> poll() = 0;
    virtual LinkState link(PeerIndex peer) const = 0;
    virtual PeerIndex self() const = 0;
    virtual std::uint32_t peer_count() const = 0;
    // Query on the same I/O owner's thread as poll/link. Unknown until measured,
    // and for adapters without a measurement facility. Consumers can detect
    // stale observations when sample_count stops advancing.
    virtual std::optional<LinkStats> link_stats(PeerIndex) const { return std::nullopt; }
    // Changes only when a replacement authenticated stream is published. Zero
    // means the adapter has no stream-generation observation.
    virtual std::uint64_t edge_identity(PeerIndex) const { return 0; }
    // A replacement cannot recover its bounded input tail. Close this candidate
    // and expose final pair-failure evidence to the departure policy.
    virtual void fail_repair(PeerIndex, unsigned) {}
};

} // namespace eawr::net
