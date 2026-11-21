#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace eawr::core::perf_diag {

// Phase names are interned to small integer ids once, at setup or at the first use of a name; the per-tick path then
// carries ids only (no string building). Names must have static storage duration (string literals): the table keeps the
// view, it never copies. Fixed capacity, no allocation. intern() is for one thread at a time (the registering side);
// name() may be read from any thread once the id has been handed out.
using PhaseId = std::uint16_t;
inline constexpr PhaseId no_phase = 0xFFFF;

template <std::size_t Capacity = 256>
class PhaseTable final {
    static_assert(Capacity > 0 && Capacity < no_phase, "ids must fit PhaseId below the sentinel");

public:
    // Returns the id of name, adding it when new; no_phase when the table is full.
    [[nodiscard]] PhaseId intern(const std::string_view name) noexcept {
        const std::size_t count = count_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < count; ++i)
            if (names_[i] == name) return static_cast<PhaseId>(i);
        if (count == Capacity) return no_phase;
        names_[count] = name;
        count_.store(count + 1, std::memory_order_release);
        return static_cast<PhaseId>(count);
    }
    [[nodiscard]] std::string_view name(const PhaseId id) const noexcept {
        return id < count_.load(std::memory_order_acquire) ? names_[id] : std::string_view{};
    }
    [[nodiscard]] std::size_t size() const noexcept { return count_.load(std::memory_order_acquire); }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    std::array<std::string_view, Capacity> names_{};
    std::atomic<std::size_t> count_{0};
};

} // namespace eawr::core::perf_diag
