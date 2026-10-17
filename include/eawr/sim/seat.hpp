#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace eawr::sim {

// ADR-023 (2026-10-15): each skirmish human has its own owner; teams are independent.
inline constexpr std::size_t max_match_seats = 9;
inline constexpr std::size_t max_match_human_sides = max_match_seats;

// ADR-020 A1: controller identity is distinct from the faction that owns assets.
struct SeatId {
    std::uint32_t value{};
    friend constexpr auto operator<=>(const SeatId&, const SeatId&) noexcept = default;
};

// Stable controller kinds shared by session, checkpoint and admission consumers.
// The versioned replay/checkpoint encoders write these existing byte values.
enum class SeatKind : std::uint8_t { human = 1, ai = 2, script = 3 };

struct SeatCommandKey {
    std::uint64_t tick{};
    SeatId seat{};
    std::uint64_t sequence{};
    friend constexpr auto operator<=>(const SeatCommandKey&, const SeatCommandKey&) noexcept = default;
};

} // namespace eawr::sim
