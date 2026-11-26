#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "eawr/core/perf_diag/ring.hpp"

namespace eawr::core::perf_diag {

// Frame side records (#8058 P3). A frame is one rendered picture; its numbers come from three places that never
// agree on "now": the main thread (our own frame work), the render thread (CPU time to record the draw) and the GPU.
// FrameMatcher joins CPU records by engine frame number, never arrival order; GPU source frames are unknown.

// Flags of MainFrame.
inline constexpr std::uint32_t frame_flag_stale = 1U;     // the frame showed no new simulation snapshot
inline constexpr std::uint32_t frame_flag_running = 2U;   // a running (unpaused) live session was presented

inline constexpr void count_stale_frame(std::uint64_t& count, const std::uint32_t flags) noexcept {
    if ((flags & frame_flag_stale) != 0U) ++count;
}

// Pushed by the main thread when its frame work ends. 40 bytes.
struct MainFrame final {
    std::uint64_t frame{};      // engine frame number of this frame
    std::int64_t entry_us{};    // steady_clock microseconds at frame entry
    float frame_ms{};           // entry to entry (previous entry to this one)
    float viewer_main_ms{};     // our own frame work
    float outside_ms{};         // previous frame exit to this entry: engine draw submit, render sync, limiter, present
    float lock_wait_ms{};       // waited for the live session's mutex (frame(), tick_costs_after(), accessors)
    std::uint32_t flags{};
    std::uint32_t reserved{};
};
static_assert(sizeof(MainFrame) == 40);

// Pushed from inside the render-thread callback queued by the main thread during frame `tag`. The callback runs on
// the render thread before that frame's draw. CPU describes the preceding draw. GPU is the latest available
// measurement, whose source frame is unknown; it must not be joined to a guessed frame.
struct RenderSample final {
    std::uint64_t tag{};        // engine frame number at queue time
    float cpu_ms{};             // viewport render CPU time (latest completed draw)
    float gpu_ms{};             // viewport GPU time (latest available)
    float setup_ms{};           // frame setup CPU time
    std::uint32_t flags{};
    std::uint32_t draw_calls{}; // the viewport's draw calls of the preceding frame (read on the render thread)
    std::uint32_t objects{};    // the viewport's visible objects of the preceding frame
};
static_assert(sizeof(RenderSample) == 32);

// CPU draw preceding the callback tag; the GPU getter does not identify its source frame.
struct FrameLags final {
    std::uint32_t cpu{1};

};

// One frame with all its parts. have_* say which parts were found before the frame left the window.
struct FrameSample final {
    MainFrame main{};
    float render_cpu_ms{};
    float render_gpu_ms{};
    float render_setup_ms{};
    std::uint32_t render_draw_calls{};
    std::uint32_t render_objects{};
    bool have_main{};
    bool have_cpu{};
    bool have_gpu{};
    [[nodiscard]] bool complete() const noexcept { return have_main && have_cpu; }
};

// Joins MainFrame and RenderSample records by engine frame number in a fixed window (no allocation after
// construction). A frame is emitted when its main and CPU parts are present, or when the window moves past it (then with
// the missing parts marked absent and counted in incomplete()). Out of order arrival is fine; a record for a frame
// already left or older than the window is counted in late().
template <std::size_t Window = 64>
class FrameMatcher final {
    static_assert(Window >= 8 && (Window & (Window - 1)) == 0, "window is a power of two");

public:
    explicit FrameMatcher(const FrameLags lags = {}) noexcept : lags_(lags) {}

    // Consumers receive completed or evicted frames through the callable (const FrameSample&).
    template <class Sink>
    void add_main(const MainFrame& record, Sink&& sink) {
        Slot* slot = claim(record.frame, sink);
        if (slot == nullptr) return;
        slot->sample.main = record;
        slot->sample.have_main = true;
        finish(*slot, sink);
    }

    template <class Sink>
    void add_render(const RenderSample& record, Sink&& sink) {
        if (record.tag >= lags_.cpu) {
            if (Slot* slot = claim(record.tag - lags_.cpu, sink)) {
                slot->sample.render_cpu_ms = record.cpu_ms;
                slot->sample.render_setup_ms = record.setup_ms;
                slot->sample.render_draw_calls = record.draw_calls;
                slot->sample.render_objects = record.objects;
                slot->sample.have_cpu = true;
                finish(*slot, sink);
            }
        }

    }

    // Emits every frame still waiting (end of a run).
    template <class Sink>
    void flush(Sink&& sink) {
        for (std::size_t i = 0; i < Window; ++i) {
            Slot& slot = slots_[i];
            if (slot.used) evict(slot, sink);
        }
    }

    [[nodiscard]] std::uint64_t incomplete() const noexcept { return incomplete_; }
    [[nodiscard]] std::uint64_t late() const noexcept { return late_; }
    [[nodiscard]] std::uint64_t emitted() const noexcept { return emitted_; }
    [[nodiscard]] const FrameLags& lags() const noexcept { return lags_; }

private:
    struct Slot final {
        FrameSample sample{};
        std::uint64_t frame{};
        bool used{};
    };

    template <class Sink>
    Slot* claim(const std::uint64_t frame, Sink& sink) {
        if (started_ && frame + Window <= newest_) {   // older than the window: its slot is gone
            ++late_;
            return nullptr;
        }
        Slot& slot = slots_[frame & (Window - 1)];
        if (slot.used && slot.frame != frame) {
            if (slot.frame > frame) { ++late_; return nullptr; }
            evict(slot, sink);
        }
        if (!slot.used) {
            slot = Slot{};
            slot.used = true;
            slot.frame = frame;
        }
        if (!started_ || frame > newest_) newest_ = frame;
        started_ = true;
        return &slot;
    }

    template <class Sink>
    void finish(Slot& slot, Sink& sink) {
        if (!slot.sample.complete()) return;
        ++emitted_;
        sink(slot.sample);
        slot.used = false;
    }

    template <class Sink>
    void evict(Slot& slot, Sink& sink) {
        if (!slot.used) return;
        if (slot.sample.have_main) {
            ++incomplete_;
            ++emitted_;
            sink(slot.sample);
        } else {
            ++incomplete_;
        }
        slot.used = false;
    }

    FrameLags lags_;
    std::array<Slot, Window> slots_{};
    std::uint64_t newest_{};
    bool started_{};
    std::uint64_t incomplete_{};
    std::uint64_t late_{};
    std::uint64_t emitted_{};
};

// The producers' rings: the main thread owns the first, the render-thread callback the second. Single producer,
// single consumer (the main thread drains both next frame).
struct FrameRings final {
    SpscRing<MainFrame> main;
    SpscRing<RenderSample> render;
    explicit FrameRings(const std::size_t capacity = 1024) : main(capacity), render(capacity) {}
};

} // namespace eawr::core::perf_diag
