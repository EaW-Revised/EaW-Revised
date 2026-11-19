#pragma once

#include "eawr/sim/tactical/combat.hpp"

#include <algorithm>
#include <cstdint>

namespace eawr::sim::tactical::detail {

// The existing world's transactional stream, consumed only by ordered death hooks.
inline std::uint64_t survivor_word(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    auto word = state;
    word = (word ^ (word >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    word = (word ^ (word >> 27U)) * 0x94d049bb133111ebULL;
    return word ^ (word >> 31U);
}

inline std::uint64_t survivor_below(std::uint64_t& state, const std::uint64_t width) noexcept {
    const auto threshold = (std::uint64_t{} - width) % width;
    auto word = survivor_word(state);
    while (word < threshold) word = survivor_word(state);
    return word % width;
}

inline std::int32_t survivor_count(std::int32_t low, std::int32_t high, std::uint64_t& state) noexcept {
    // DS-02: signed reversed bounds swap; equal bounds consume no word. The
    // native helper caps the inclusive sampled width at 32766, not the count.
    if (high < low) std::swap(low, high);
    if (low == high) return low;
    const auto width = std::min<std::uint64_t>(static_cast<std::uint64_t>(std::int64_t{high} - low), 32766);
    std::uint64_t mask = 1;
    while (mask <= width) mask <<= 1;
    std::uint64_t word;
    do { word = survivor_word(state) & (mask - 1); } while (word > width);
    return static_cast<std::int32_t>(std::int64_t{low} + static_cast<std::int64_t>(word));
}

inline const DestructionSurvivorChoice* survivor_choice(const DestructionSurvivorProfile& profile,
    std::uint64_t& state) noexcept {
    if (profile.choices.empty()) return nullptr;
    std::uint64_t total = 0;
    for (const auto& choice : profile.choices) total += static_cast<std::uint64_t>(choice.weight.raw());
    const auto admission = survivor_below(state, static_cast<std::uint64_t>(math::Fixed::scale));
    if (total == 0 || (total < static_cast<std::uint64_t>(math::Fixed::scale)
            && admission < static_cast<std::uint64_t>(math::Fixed::scale) - total)) return nullptr;
    auto sample = survivor_below(state, total);
    for (const auto& choice : profile.choices) {
        const auto weight = static_cast<std::uint64_t>(choice.weight.raw());
        if (sample < weight) return &choice;
        sample -= weight;
    }
    return nullptr;
}

} // namespace eawr::sim::tactical::detail
