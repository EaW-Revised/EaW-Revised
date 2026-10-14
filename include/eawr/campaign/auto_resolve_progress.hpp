#pragma once
#include "eawr/sim/math/fixed.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace eawr::campaign {
struct AutoResolveProgress {
    std::uint64_t conflict{};
    sim::math::Fixed elapsed_mode_seconds, display_seconds;
    struct Combatant { std::uint32_t owner{}; std::string original_type, current_type; };
    std::array<Combatant, 2> combatants;
    // WGM-181: a complete source distinguishes current, escort and queue.
    // WAR-112..125 qualify the aggregate queue; current/escort remain empty.
    struct SideRoster {
        std::uint32_t owner{};
        std::optional<Combatant> current, escort;
        std::vector<Combatant> queue;
    };
    std::optional<std::array<SideRoster, 2>> roster;
    bool initiated{}, withdraw_admitted{};
    std::uint32_t retreating_player{};
};
} // namespace eawr::campaign
