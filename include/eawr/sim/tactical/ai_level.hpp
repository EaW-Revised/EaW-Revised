#pragma once

#include <cstdint>
#include <string_view>

namespace eawr::sim::tactical {

// WTA-47: the difficulty level of a lobby AI player (Easy / Normal / Hard). Normal is the default: a human,
// an AI with no level set and every recording that carries no level are Normal, where every multiplier is 1.
// The level names the player type's Difficulty_Adjustments entry; the values are data (difficultyadjustments.xml).
enum class AiLevel : std::uint8_t { normal = 0, easy = 1, hard = 2 };

[[nodiscard]] constexpr std::string_view to_string(const AiLevel level) noexcept {
    switch (level) {
    case AiLevel::easy: return "Easy";
    case AiLevel::hard: return "Hard";
    case AiLevel::normal: break;
    }
    return "Normal";
}

} // namespace eawr::sim::tactical
