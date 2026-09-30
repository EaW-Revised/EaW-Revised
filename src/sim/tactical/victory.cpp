#include "eawr/sim/tactical/victory.hpp"

#include "tactical_internal.hpp"

#include <algorithm>
#include <string>

namespace eawr::sim::tactical {
namespace {

template <typename T>
[[nodiscard]] bool strictly_ascending(const std::vector<T>& values) {
    return std::adjacent_find(values.begin(), values.end(), [](const T left, const T right) { return left >= right; })
        == values.end();
}

[[nodiscard]] bool contains(const std::vector<PlayerId>& sorted, const PlayerId player) {
    return std::binary_search(sorted.begin(), sorted.end(), player);
}

[[nodiscard]] const Player* find_player(const std::span<const Player> players, const PlayerId id) {
    const auto found = std::find_if(players.begin(), players.end(), [&](const Player& player) { return player.player_id == id; });
    return found == players.end() ? nullptr : &*found;
}

} // namespace

core::Result<void> validate_victory(const VictoryRules& rules, const std::span<const Player> players) {
    const auto invalid = [](const std::string& message) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "victory rules: " + message));
    };
    if (rules.condition == VictoryCondition::none) {
        return core::Result<void>::success();
    }
    if (rules.condition != VictoryCondition::enemy_starbase_destroyed) {
        return invalid("unknown condition");
    }
    if (!strictly_ascending(rules.starbase_types) || !strictly_ascending(rules.contenders)
        || !strictly_ascending(rules.humans)) {
        return invalid("star base types, contenders and humans must strictly increase");
    }
    for (const auto contender : rules.contenders) {
        const auto* player = find_player(players, contender);
        if (player == nullptr) {
            return invalid("contender " + std::to_string(contender) + " is not a setup player");
        }
        if (!player->commandable()) {
            return invalid("contender " + std::to_string(contender) + " is not commandable");
        }
    }
    for (const auto human : rules.humans) {
        if (!contains(rules.contenders, human)) {
            return invalid("human " + std::to_string(human) + " is not a contender");
        }
    }
    if (rules.countdown_frames > max_ticks) {
        return invalid("countdown exceeds the tactical tick limit");
    }
    return core::Result<void>::success();
}

std::optional<PlayerId> starbase_destroyed_winner(const VictoryRules& rules, const std::span<const Player> players,
    const PlayerId owner, const std::span<const StarbaseEntry> remaining) {
    if (rules.condition != VictoryCondition::enemy_starbase_destroyed) {
        return std::nullopt;
    }
    const auto* lost = find_player(players, owner);
    if (lost == nullptr || !lost->commandable() || !contains(rules.contenders, owner)) {
        return std::nullopt;
    }
    const auto team_of = [&](const PlayerId id) {
        const auto* player = find_player(players, id);
        return player != nullptr ? std::optional(player->team_id) : std::nullopt;
    };
    for (const auto candidate : rules.contenders) {
        const auto* candidate_player = find_player(players, candidate);
        if (candidate_player == nullptr || !candidate_player->commandable()) {
            continue;
        }
        // VT-04: the enemies of the destroyed star base's owner, in ascending ID.
        if (candidate == owner || candidate_player->team_id == lost->team_id) {
            continue;
        }
        // VT-05: every counted star base the candidate does not own, allies' included.
        bool any = false;
        bool human_allied = false;
        for (const auto& base : remaining) {
            if (base.owner == candidate) {
                continue;
            }
            any = true;
            const auto base_team = team_of(base.owner);
            human_allied = human_allied
                || std::any_of(rules.humans.begin(), rules.humans.end(),
                    [&](const PlayerId human) { return base_team && team_of(human) == base_team; });
        }
        if (!any) {
            return candidate;
        }
        // VT-06: a player that is not human also wins once no such star base is a human's ally.
        if (!contains(rules.humans, candidate) && !human_allied) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string_view to_string(const VictoryCondition condition) noexcept {
    switch (condition) {
    case VictoryCondition::none:
        return "none";
    case VictoryCondition::enemy_starbase_destroyed:
        return "enemy_starbase_destroyed";
    }
    return "unknown";
}

} // namespace eawr::sim::tactical
