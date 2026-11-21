#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace eawr::core::perf_diag {

// Single-producer / single-consumer diagnostics ring. Fixed capacity (a power of two), preallocated at construction,
// the producer never blocks, waits or allocates. When the consumer falls a full capacity behind the oldest records are
// overwritten and counted in dropped(). Each slot is a seqlock over relaxed atomic words, so a consumer that reads a
// slot while the producer overwrites it detects that and skips the record instead of returning a torn value.
// Write-only diagnostics: nothing in the simulation reads these records.
template <class T>
class SpscRing final {
    static_assert(std::is_trivially_copyable_v<T>, "ring records are copied word by word");
    static_assert(sizeof(T) % sizeof(std::uint64_t) == 0, "pad ring records to a multiple of 8 bytes");

public:
    static constexpr std::size_t words = sizeof(T) / sizeof(std::uint64_t);

    // capacity is rounded up to a power of two, at least 2.
    explicit SpscRing(const std::size_t capacity)
        : capacity_(round_up(capacity)), mask_(capacity_ - 1), slots_(new Slot[capacity_]) {}
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Producer thread only. Wait-free, no allocation.
    void push(const T& value) noexcept {
        const std::uint64_t index = head_.load(std::memory_order_relaxed);
        Slot& slot = slots_[index & mask_];
        slot.seq.store(2 * index + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::uint64_t buffer[words];
        std::memcpy(buffer, &value, sizeof(T));
        for (std::size_t i = 0; i < words; ++i) slot.data[i].store(buffer[i], std::memory_order_relaxed);
        slot.seq.store(2 * index + 2, std::memory_order_release);
        head_.store(index + 1, std::memory_order_release);
    }

    // Consumer thread only. Returns false when nothing new is published.
    [[nodiscard]] bool pop(T& out) noexcept {
        for (;;) {
            const std::uint64_t head = head_.load(std::memory_order_acquire);
            if (tail_ == head) return false;
            if (head - tail_ > capacity_) skip_to(head - capacity_);
            Slot& slot = slots_[tail_ & mask_];
            const std::uint64_t before = slot.seq.load(std::memory_order_acquire);
            if (before == 2 * tail_ + 2) {
                std::uint64_t buffer[words];
                for (std::size_t i = 0; i < words; ++i) buffer[i] = slot.data[i].load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (slot.seq.load(std::memory_order_relaxed) == before) {
                    std::memcpy(&out, buffer, sizeof(T));
                    ++tail_;
                    return true;
                }
            }
            // The producer lapped this record while it was read (or is writing it): it is lost.
            dropped_.fetch_add(1, std::memory_order_relaxed);
            ++tail_;
        }
    }

    [[nodiscard]] std::uint64_t pushed() const noexcept { return head_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    // Records published and not yet consumed (consumer thread).
    [[nodiscard]] std::uint64_t pending() const noexcept { return head_.load(std::memory_order_acquire) - tail_; }

private:
    struct Slot final {
        std::atomic<std::uint64_t> seq{0};
        std::atomic<std::uint64_t> data[words]{};
    };

    static std::size_t round_up(const std::size_t value) noexcept {
        std::size_t result = 2;
        while (result < value) result <<= 1;
        return result;
    }
    void skip_to(const std::uint64_t target) noexcept {
        dropped_.fetch_add(target - tail_, std::memory_order_relaxed);
        tail_ = target;
    }

    std::size_t capacity_;
    std::size_t mask_;
    std::unique_ptr<Slot[]> slots_;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::uint64_t tail_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

} // namespace eawr::core::perf_diag
