#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace eawr::script::foc {

// Async galactic AI, step 2 (close M1): the exit counter. Observer only: never saved, hashed or read by a decision.
// It counts writes to the AI's own state (the engine's native revision, the native action records, the tracked contributors:
// lifecycle manager and execution state) made while the
// async schedule's window body is not open, i.e. by the world side (loss notices and refusal hooks that apply at once
// while due slices apply). Step 2 is done when the count is zero over a stock day and the mature digest run.
class GalacticAiWriteProbe {
public:
    enum class Writer : unsigned { engine, actions, tracked, count }; // tracked: the lifecycle manager and the execution contributors
    // Armed only for the async schedule: the reference schedule writes at once by design.
    void arm(const bool value) noexcept { armed_.store(value, std::memory_order_relaxed); }
    void body(const bool open) noexcept { body_.store(open, std::memory_order_relaxed); }
    [[nodiscard]] bool body_open() const noexcept { return body_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool armed() const noexcept { return armed_.load(std::memory_order_relaxed); }
    void note(const Writer writer) noexcept {
        if (!armed()) return;
        const auto at = static_cast<std::size_t>(writer);
        // Inside the body (the hot, partitioned path) the probe only reads one flag; only the rare outside write touches a shared counter.
        if (!body_.load(std::memory_order_relaxed)) outside_[at].fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t outside(const Writer writer) const noexcept {
        return outside_[static_cast<std::size_t>(writer)].load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t outside_total() const noexcept {
        std::uint64_t total{};
        for (const auto& value : outside_) total += value.load(std::memory_order_relaxed);
        return total;
    }

private:
    std::atomic<bool> armed_{false}, body_{false};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Writer::count)> outside_{};
};

} // namespace eawr::script::foc
