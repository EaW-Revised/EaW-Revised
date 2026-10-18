#pragma once

// #4043: the network half of the performance overlay (docs/ui/perf-overlay.md). Per-peer link
// quality, the ledger of ticks the simulation spent waiting for a remote seat's inputs, and a
// one-line verdict that tells the owner whether an online match chugs because of the game or
// because of the network. Presentation state read from wall-clock timers and local transport
// observations; it never enters a command, a snapshot, the replay, lockstep or a hash.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace eawr::presentation::ui {

// The ledger keeps this much history for "longest wait".
inline constexpr std::uint64_t net_perf_history_ms = 10000;
// A driver that polls faster than the clock ticks must not grow or rescan the ledger without bound.
inline constexpr std::size_t net_perf_max_episodes = 512;
// The per-second figures cover this window.
inline constexpr std::uint64_t net_perf_second_ms = 1000;
// More waiting than this in the last second (or a wait this long right now) blames the network.
inline constexpr std::uint64_t net_perf_blame_ms = 100;
// A frame or tick slower than this (one 30 Hz tick plus half a millisecond) blames the game.
inline constexpr double net_perf_game_ms = 1000.0 / 30.0 + 0.5;
// The godot.log summary line repeats this often while a match runs.
inline constexpr double net_perf_log_seconds = 5.0;

struct NetPeerLink final {
    std::uint32_t peer{};
    bool measured{};        // false until the transport has a round-trip sample
    double rtt_ms{};
    double jitter_ms{};
    double loss_pct{};      // packets lost in the recent window, 0 when the transport cannot tell
};

// What the ledger knows at one instant.
struct NetStallSnapshot final {
    bool waiting{};
    std::uint32_t wait_peer{};           // the first seat the simulation waits for (valid while waiting)
    std::uint64_t wait_ms{};             // how long this wait has lasted so far
    std::uint32_t stalls_last_second{};  // waits that were running at any point in the last second
    std::uint64_t stalled_ms_last_second{};
    std::uint64_t longest_ms_last_second{};
    std::uint32_t longest_peer_last_second{};
    std::uint64_t longest_ms_10s{};      // the longest wait in the last 10 s, the running one included
    std::uint32_t longest_peer_10s{};
};

// Counts every wait, however short: the stall-start log line only fires after a second.
class NetStallLedger final {
public:
    // Called once per driver poll with the monotonic clock in milliseconds. `peer` is the seat
    // the simulation waits for first and is ignored while `waiting` is false.
    void observe(std::uint64_t now_ms, bool waiting, std::uint32_t peer);
    [[nodiscard]] NetStallSnapshot snapshot(std::uint64_t now_ms) const;
    void reset();

private:
    struct Episode final {
        std::uint64_t start{};
        std::uint64_t end{};
        std::uint32_t peer{};
    };
    void trim(std::uint64_t now_ms);
    std::deque<Episode> episodes_;
    bool waiting_{};
    std::uint64_t since_{};
    std::uint32_t peer_{};
};

struct NetPerfReport final {
    std::uint32_t seat{};
    std::vector<NetPeerLink> peers;
    NetStallSnapshot stall;
    double frame_ms{};  // the overlay's frame time (window average)
    double sim_ms{};    // the simulation's cost per tick (window average)
};

enum class NetBlame { ok, network, game };

struct NetVerdict final {
    NetBlame blame{NetBlame::ok};
    std::string text;  // "network: waiting on peer 1 640 ms", "game: frame 48 ms, sim 31 ms" or "ok"
};

[[nodiscard]] NetVerdict net_verdict(const NetPerfReport& report);
// One stable line for godot.log: "EAWR net summary seat=0 verdict=ok wait_peer=- ...".
[[nodiscard]] std::string net_summary_line(const NetPerfReport& report);
// The `net_perf` object of the end-of-match report.
[[nodiscard]] std::string net_report_json(const NetPerfReport& report);

} // namespace eawr::presentation::ui
