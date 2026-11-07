#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace eawr::presentation::particles {

// PF lane-serial (a): one published unit of claimable work that an otherwise idle thread may help with. The particle
// lane publishes the ordered batch it is running (StepExecutor::run_ordered); the main thread, waiting for the lane
// at the join, builds slices of it nobody has claimed yet. The publisher stays the only committer, so the output is
// the one the batch has without help. Lock-free; one publisher, any number of helpers.
class HelpBoard final {
public:
    // `work(context)` claims and runs pieces until none is left and returns how many it ran. It must be safe to call
    // from any thread while published, and after every piece is claimed (it then runs nothing).
    struct Entry final {
        std::size_t (*work)(void* context){};
        void* context{};
    };

    // The publisher: `entry` must stay alive until retract() returned. One entry at a time.
    void publish(const Entry* entry) noexcept {
        entry_.store(entry, std::memory_order_seq_cst);
        wake();
    }

    // The publisher: after this returns no helper is inside the entry's work, and none enters it again.
    void retract() noexcept {
        entry_.store(nullptr, std::memory_order_seq_cst);
        for (auto inside = active_.load(std::memory_order_seq_cst); inside != 0;
             inside = active_.load(std::memory_order_seq_cst)) {
            active_.wait(inside, std::memory_order_seq_cst);
        }
    }

    // A helper: runs the published entry's work once if there is one; returns the pieces it ran (0: nothing to do).
    // The count goes up before the entry is read, so retract() either sees this helper inside or this helper sees
    // the entry gone.
    std::size_t help_once() noexcept {
        active_.fetch_add(1, std::memory_order_seq_cst);
        std::size_t ran = 0;
        if (const Entry* const entry = entry_.load(std::memory_order_seq_cst); entry != nullptr) {
            ran = entry->work(entry->context);
        }
        if (active_.fetch_sub(1, std::memory_order_seq_cst) == 1) active_.notify_all();
        return ran;
    }

    // Wake-ups for a helper that waits for something to help with (a publish) or for its own reason to stop (the
    // owner calls wake() then): read generation(), check, then wait_change() with what was read.
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    void wait_change(const std::uint32_t seen) const noexcept { generation_.wait(seen, std::memory_order_acquire); }
    void wake() noexcept {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        generation_.notify_all();
    }

private:
    std::atomic<const Entry*> entry_{nullptr};
    std::atomic<std::uint32_t> active_{0};
    std::atomic<std::uint32_t> generation_{0};
};

} // namespace eawr::presentation::particles
