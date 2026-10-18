#pragma once

#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstdint>
#include <optional>

namespace eawr::presentation::godot_backend {

namespace live_session {

struct ReinforcementFeedback final {
    enum class Kind : std::uint8_t { pane, placement, enroute, cancelled };
    Kind kind{};
    sim::tactical::TypeId type{};
};

struct EconomyRequests final {
    std::uint64_t buys{};
    std::uint64_t cancels{};
    std::uint64_t reinforcements{};
    std::uint64_t refused{};
};

struct PendingEconomy final {
    std::optional<sim::tactical::EconomyView> view;
    std::optional<std::uint64_t> tick{};
    std::uint64_t revision{};
};

} // namespace live_session
} // namespace eawr::presentation::godot_backend
