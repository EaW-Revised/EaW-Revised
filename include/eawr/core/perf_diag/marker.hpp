#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include "eawr/core/perf_diag/ring.hpp"

namespace eawr::core::perf_diag {

// Event markers: (kind, tick or frame, small value) records for scheduled work that can explain a hitch. Kind ids are
// below 64 so a set of kinds fits one 64-bit mask (hitch records keep the markers within +-2 ticks that way).
enum class MarkerKind : std::uint8_t {
    save, autosave, prebattle_autosave, save_write, clock_reset, pause, resume, speed, galactic_day,
    ai_perception, ai_goals, ai_planning, ai_execution, ai_learning, ai_templates,
    battle_start, battle_end, hash_exchange, lua_memory_walk, lua_collect, fog, arrivals, pipeline_compile,
    degradation, overlay_rebuild, net_summary,
    count_
};
static_assert(static_cast<unsigned>(MarkerKind::count_) <= 64, "marker kinds must fit a 64-bit mask");

[[nodiscard]] constexpr std::string_view marker_name(const MarkerKind kind) noexcept {
    constexpr std::string_view names[] = {
        "save", "autosave", "prebattle-autosave", "save-write", "clock-reset", "pause", "resume", "speed", "galactic-day",
        "gc.ai.perception", "gc.ai.goals", "gc.ai.planning", "gc.ai.execution", "gc.ai.learning", "gc.ai.templates",
        "battle-start", "battle-end", "hash-exchange", "lua.memory-walk", "lua.collect", "fog", "arrivals",
        "pipeline-compile", "degradation", "overlay-rebuild", "net-summary"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<unsigned>(MarkerKind::count_));
    return static_cast<unsigned>(kind) < static_cast<unsigned>(MarkerKind::count_) ? names[static_cast<unsigned>(kind)]
                                                                                   : std::string_view{};
}

[[nodiscard]] constexpr std::uint64_t marker_bit(const MarkerKind kind) noexcept {
    return std::uint64_t{1} << static_cast<unsigned>(kind);
}

// 16 bytes, a multiple of 8 for the ring.
struct Marker final {
    std::uint64_t tick{};   // tick number for sim-side markers, frame number for frame-side markers
    std::int32_t value{};
    MarkerKind kind{MarkerKind::save};
    std::uint8_t frame_side{}; // 1 when tick is a frame number
    std::uint16_t reserved{};
};
static_assert(sizeof(Marker) == 16);

// The write-only emit side. Off (the default) costs one relaxed bool test: no clock, no allocation, no lock. A
// producer thread binds its own ring once; markers pushed while no ring is bound are ignored.
class MarkerSink final {
public:
    static void set_enabled(const bool on) noexcept { enabled_flag().store(on, std::memory_order_relaxed); }
    [[nodiscard]] static bool enabled() noexcept { return enabled_flag().load(std::memory_order_relaxed); }

    static void bind_thread(SpscRing<Marker>* ring) noexcept { thread_ring() = ring; }

    static void emit(const MarkerKind kind, const std::uint64_t tick, const std::int32_t value = 0,
                     const bool frame_side = false) noexcept {
        if (!enabled()) return;
        SpscRing<Marker>* const ring = thread_ring();
        if (ring == nullptr) return;
        ring->push(Marker{tick, value, kind, static_cast<std::uint8_t>(frame_side ? 1 : 0), 0});
    }

private:
    static std::atomic<bool>& enabled_flag() noexcept {
        static std::atomic<bool> flag{false};
        return flag;
    }
    static SpscRing<Marker>*& thread_ring() noexcept {
        thread_local SpscRing<Marker>* ring = nullptr;
        return ring;
    }
};

} // namespace eawr::core::perf_diag
