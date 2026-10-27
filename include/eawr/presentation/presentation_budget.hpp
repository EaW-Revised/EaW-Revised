#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace eawr::presentation {

enum class ResourceKind : std::uint8_t { cpu_effect, gpu_proxy };
enum class ReservationOutcome : std::uint8_t { reserved, budget_pressure, arithmetic_failure };

struct ResourceBudgetSnapshot final {
    std::optional<std::size_t> limit_bytes;
    std::size_t reserved_bytes{}, high_water_bytes{};
    // Unavailable means no device/process probe has supplied a usable snapshot.
    bool probe_available{};
};

// Presentation-thread ownership only. Reservations include idle reusable payload;
// callers evict idle entries before retrying, and release partial admissions on failure.
// The provider must outlive its consumers. GPU providers use a cached probe snapshot.
class PresentationBudget {
public:
    virtual ~PresentationBudget() = default;
    [[nodiscard]] virtual ReservationOutcome reserve(ResourceKind kind, std::size_t bytes) noexcept = 0;
    virtual void release(ResourceKind kind, std::size_t bytes) noexcept = 0;
    [[nodiscard]] virtual ResourceBudgetSnapshot snapshot(ResourceKind kind) const noexcept = 0;
};

// Default accounting has no invented device or process limit. A missing probe
// permits admission; actual allocation failures remain the caller's responsibility.
class PayloadBudget final : public PresentationBudget {
public:
    void set_limit(ResourceKind kind, std::optional<std::size_t> bytes, bool probe_available = false) noexcept {
        auto& state = states_[index(kind)];
        state.limit_bytes = bytes;
        state.probe_available = probe_available;
    }
    [[nodiscard]] ReservationOutcome reserve(ResourceKind kind, std::size_t bytes) noexcept override {
        auto& state = states_[index(kind)];
        if (bytes > std::numeric_limits<std::size_t>::max() - state.reserved_bytes)
            return ReservationOutcome::arithmetic_failure;
        const auto total = state.reserved_bytes + bytes;
        if (state.limit_bytes && total > *state.limit_bytes) return ReservationOutcome::budget_pressure;
        state.reserved_bytes = total;
        if (total > state.high_water_bytes) state.high_water_bytes = total;
        return ReservationOutcome::reserved;
    }
    void release(ResourceKind kind, std::size_t bytes) noexcept override {
        auto& state = states_[index(kind)];
        assert(bytes <= state.reserved_bytes && "presentation budget release exceeds reservation");
        state.reserved_bytes -= bytes < state.reserved_bytes ? bytes : state.reserved_bytes;
    }
    [[nodiscard]] ResourceBudgetSnapshot snapshot(ResourceKind kind) const noexcept override {
        return states_[index(kind)];
    }
private:
    [[nodiscard]] static constexpr std::size_t index(ResourceKind kind) noexcept {
        return static_cast<std::size_t>(kind);
    }
    std::array<ResourceBudgetSnapshot, 2> states_{};
};

} // namespace eawr::presentation
