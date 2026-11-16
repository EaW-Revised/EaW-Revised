#include "session_impl.hpp"
#include "session_tick.hpp"
#include "session_services.hpp"
#include "eawr/core/tick_profile.hpp"

#include <algorithm>

namespace eawr::sim::tactical {

// LC-104..LC-107 (G-11..G-13): the player free garrison in a land battle. The registered objects are the members of the start
// companies and of every company landed from a pending entry. Losing the last registered object, with nothing pending, starts
// the refill timer once; partial losses start nothing. When the timer ends the whole starting-force list becomes pending and,
// in land, each type is added to the player's reinforcement pool without charge (the pool entry is what the player lands).
// A player that quit keeps no refill. Objects inside a garrison host are still registered: entering a host is not a loss.
//
// The per-player check runs as one partitioned phase over disjoint garrison states; the pool additions are an ordered serial
// commit in ascending player id.
core::Result<void> session_detail::Tick::land_free_garrisons(const std::vector<LiveUnit>& live) {
    auto& garrisons = impl_->free_garrisons;
    if (garrisons.empty()) return core::Result<void>::success();
    core::tick_profile::Scope profile("serial.tactical.land-free-garrison");
    const auto& land = *impl_->land;
    const auto alive = [&](const EntityId id, const PlayerId player) {
        const auto found = std::lower_bound(live.begin(), live.end(), id,
            [](const LiveUnit& unit, const EntityId key) { return unit.state.entity_id < key; });
        if (found != live.end() && found->state.entity_id == id) return found->state.owner == player;
        const auto* entry = land.garrison_hiding(id);
        return entry != nullptr && std::any_of(entry->hidden.begin(), entry->hidden.end(), [&](const LiveUnit& unit) {
            return unit.state.entity_id == id && unit.state.owner == player;
        });
    };
    std::vector<std::uint8_t> matured(garrisons.size());
    const auto serviced = executor.execute_phase("land-free-garrisons", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, garrisons.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& state = garrisons[index];
            const auto player = state.setup.player;
            const bool quit = std::any_of(impl_->quits.begin(), impl_->quits.end(),
                [player](const PlayerQuit& value) { return value.player == player; })
                || std::any_of(pending_quits_.begin(), pending_quits_.end(),
                    [player](const PlayerQuit& value) { return value.player == player; });
            const bool held = !state.registered.empty();
            std::erase_if(state.registered, [&](const EntityId id) { return !alive(id, player); });
            if (quit) {
                state.pending.clear();
                state.due_frame.reset();
                continue;
            }
            // LC-104: only the loss of the last registered object starts a delay, and only with nothing pending.
            if (held && state.registered.empty() && state.pending.empty() && !state.due_frame)
                state.due_frame = tick + state.setup.delay_frames;
            if (state.due_frame && *state.due_frame <= tick) matured[index] = 1;
        }
    });
    if (!serviced) return core::Result<void>::failure(serviced.error());
    for (std::size_t index = 0; index < garrisons.size(); ++index) {
        if (matured[index] == 0) continue;
        auto& state = garrisons[index];
        state.due_frame.reset();
        // LC-106 (G-13): a list that is not empty keeps its entries; an empty one becomes the whole starting-force list.
        if (!state.pending.empty()) continue;
        state.pending = state.setup.templates;
        const auto account = std::find_if(impl_->ledgers.begin(), impl_->ledgers.end(),
            [&](const PlayerEconomy& entry) { return entry.player == state.setup.player; });
        if (account == impl_->ledgers.end()) continue;
        for (const auto type : state.setup.templates) {
            account->pool.push_back(type);
            account->pool_tokens.push_back(account->next_pool_token++);
            ++account->pool_version;
            ++account->pool_additions;
            account->pool_addition_frame = tick;
        }
    }
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
