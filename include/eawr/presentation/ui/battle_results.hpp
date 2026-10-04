#pragma once

#include "eawr/sim/tactical/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace eawr::presentation::ui {

inline constexpr std::size_t battle_loss_visible_entries = 12; // WBF-45

struct ResultLoss {
    sim::tactical::TypeId type{};
    std::string name;
    std::uint64_t count{};
    friend bool operator==(const ResultLoss&, const ResultLoss&) = default;
};

struct BattleResults {
    std::array<std::vector<ResultLoss>, 2> losses; // local owner, then enemy owners
    std::array<std::vector<ResultLoss>, 2> heroes;
    std::array<std::uint64_t, 2> totals{};
    std::array<double, 2> score_cost{}, combat_power{}; // derived display statistics, never replay arithmetic
    std::array<std::string, 2> scores{}; // authored Lua control results, not credits spent
    std::array<std::map<std::string, std::string>, 2> statistics;
    std::uint64_t elapsed_milliseconds{}; // WBF-44, excluded from replay state
    std::string scoring_diagnostic;
    friend bool operator==(const BattleResults&, const BattleResults&) = default;
};

struct ResultType {
    std::string name;
    bool scored{};
    bool hero{};
    sim::math::Fixed score_cost{};
    double combat_power{};
};

[[nodiscard]] BattleResults battle_results(const sim::tactical::TacticalSnapshot& snapshot,
    sim::tactical::PlayerId local_player, const std::function<ResultType(sim::tactical::TypeId)>& type_info);
[[nodiscard]] std::size_t loss_scroll_max(std::size_t count) noexcept;
[[nodiscard]] std::string battle_time_text(std::uint64_t milliseconds);

} // namespace eawr::presentation::ui
