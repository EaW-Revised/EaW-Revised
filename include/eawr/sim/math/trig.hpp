#pragma once

#include "eawr/sim/math/fixed.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace eawr::sim::math {

[[nodiscard]] constexpr Fixed wrap_turn(Fixed angle) noexcept {
    constexpr std::int64_t half = Fixed::scale / 2;
    std::int64_t raw = angle.raw() % Fixed::scale;
    if (raw >= half) {
        raw -= Fixed::scale;
    } else if (raw < -half) {
        raw += Fixed::scale;
    }
    return Fixed::from_raw(raw);
}

[[nodiscard]] Fixed sin_turn(Fixed angle) noexcept;
[[nodiscard]] Fixed cos_turn(Fixed angle) noexcept;
// Both from one CORDIC run: {sin_turn(angle), cos_turn(angle)} (#520).
struct SinCos {
    Fixed sine;
    Fixed cosine;
};
[[nodiscard]] SinCos sin_cos_turn(Fixed angle) noexcept;

// Exact paired trig for recently used headings. Scratch only; never authoritative state.
class TrigCache final {
public:
    [[nodiscard]] SinCos sample(Fixed angle) noexcept;
    [[nodiscard]] std::uint64_t builds() const noexcept { return builds_; }
private:
    struct Entry { Fixed angle{}; SinCos value{}; };
    std::array<Entry, 8> entries_{};
    std::size_t size_{};
    std::size_t next_{};
    std::uint64_t builds_{};
};
[[nodiscard]] core::Result<Fixed> atan2_turn(Fixed y, Fixed x) noexcept;

} // namespace eawr::sim::math
