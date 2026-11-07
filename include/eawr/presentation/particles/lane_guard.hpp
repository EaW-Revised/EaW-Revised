#pragma once

#include <atomic>
#include <cstdint>

namespace eawr::presentation::particles::lane {

// L1.2: the particle lane's phase-exclusion guards. While a lane job is in flight (the "window": the kick up to the
// join) the main thread must not touch what the lane owns. Each main-thread entry point that does, calls
// `guard_main()`. A hit COUNTS and goes on: a guard must not abort a match (owner rule 10-26), the counter is read
// by the trace and the contracts. ParticleLane::kick (L1.3) opens the window; it stays closed, and the counter at 0,
// while the owners run inline (Safe, one particle worker, the lane off).
namespace detail {
inline std::atomic<bool> window_open{false};
inline std::atomic<std::uint32_t> violations{0};
inline thread_local bool on_lane_thread{false};
} // namespace detail

[[nodiscard]] inline bool window_open() noexcept { return detail::window_open.load(std::memory_order_acquire); }

// Called at the entry of a function only the main thread (outside the window) or the lane may run. Returns whether it
// counted a violation. Lock-free and cheap enough for a per-frame entry point.
inline bool guard_main() noexcept {
    if (!detail::window_open.load(std::memory_order_acquire) || detail::on_lane_thread) return false;
    detail::violations.fetch_add(1, std::memory_order_relaxed);
    return true;
}

[[nodiscard]] inline std::uint32_t violations() noexcept { return detail::violations.load(std::memory_order_relaxed); }
// Returns the count so far and starts over (the trace reads and resets once per frame).
inline std::uint32_t take_violations() noexcept { return detail::violations.exchange(0, std::memory_order_relaxed); }

// Main: open at the kick, close at the join. RAII so a failed frame cannot leave the window open.
class WindowScope final {
public:
    WindowScope() noexcept { detail::window_open.store(true, std::memory_order_release); }
    ~WindowScope() { close(); }
    WindowScope(const WindowScope&) = delete;
    WindowScope& operator=(const WindowScope&) = delete;
    void close() noexcept { detail::window_open.store(false, std::memory_order_release); }
};

// The lane thread marks itself for the length of a job: its own calls into the guarded owners are the owner's.
class LaneThreadScope final {
public:
    LaneThreadScope() noexcept { detail::on_lane_thread = true; }
    ~LaneThreadScope() { detail::on_lane_thread = false; }
    LaneThreadScope(const LaneThreadScope&) = delete;
    LaneThreadScope& operator=(const LaneThreadScope&) = delete;
};

} // namespace eawr::presentation::particles::lane
