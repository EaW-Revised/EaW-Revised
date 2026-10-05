#include "eawr/presentation/ui/battle_results.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace eawr::presentation::ui {

BattleResults battle_results(const sim::tactical::TacticalSnapshot& snapshot,
    const sim::tactical::PlayerId local_player,
    const std::function<ResultType(sim::tactical::TypeId)>& type_info) {
    BattleResults result;
    const auto players = snapshot.players();
    const auto local = std::find_if(players.begin(), players.end(),
        [local_player](const auto& player) { return player.player_id == local_player; });
    if (local == players.end()) return result;
    std::array<std::map<sim::tactical::TypeId, std::uint64_t>, 2> grouped;
    for (const auto& loss : snapshot.losses()) {
        const auto info = type_info(loss.type);
        if (!info.scored) continue;
        const auto owner = std::find_if(players.begin(), players.end(),
            [&](const auto& player) { return player.player_id == loss.owner; });
        if (owner == players.end() || owner->neutral) continue;
        // WBF-45: the local pane is the local owner; allied owners belong to neither pane.
        if (owner->player_id != local_player && owner->team_id == local->team_id) continue;
        const std::size_t side = owner->player_id == local_player ? 0 : 1;
        grouped[side][loss.type] += loss.count;
        result.totals[side] += loss.count;
        result.score_cost[side] += static_cast<double>(info.score_cost.raw()) / sim::math::Fixed::scale * static_cast<double>(loss.count);
        result.combat_power[side] += info.combat_power * static_cast<double>(loss.count);
    }
    for (std::size_t side = 0; side < grouped.size(); ++side) {
        for (const auto& [type, count] : grouped[side]) {
            const auto info = type_info(type);
            auto& rows = info.hero ? result.heroes[side] : result.losses[side];
            rows.push_back({type, info.name, count});
        }
    }
    return result;
}

std::size_t loss_scroll_max(const std::size_t count) noexcept {
    return count > battle_loss_visible_entries ? count - battle_loss_visible_entries : 0;
}

std::string battle_time_text(const std::uint64_t milliseconds) {
    const auto seconds = milliseconds / 1000U;
    std::ostringstream text;
    text << std::setfill('0') << std::setw(2) << seconds / 3600U << ':'
         << std::setw(2) << seconds / 60U % 60U << ':' << std::setw(2) << seconds % 60U;
    return text.str();
}

} // namespace eawr::presentation::ui
