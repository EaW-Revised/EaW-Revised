#pragma once
#include "eawr/sim/galactic/session.hpp"
#include <algorithm>
#include <span>

namespace eawr::sim::galactic::session_detail {
struct NativeProductionAdmission {
    ProductionAdmission admission{ProductionAdmission::unavailable};
    std::size_t planet_index{}, player_index{};
    std::optional<ProductionOffer> offer;
    PlayerId owner{};
};
[[nodiscard]] core::Result<NativeProductionAdmission> prepare_native_production(
    const GalacticSession::NativeProductionRequest&, const ProductionSetup&, const PlayerEconomySetup&,
    std::span<const Planet>, std::span<const Fleet>, std::span<const PopulationTotal>, bool census,
    const IncomeSetup*, EntityId frontier, std::size_t live_objects, std::span<const TypeId> local_unlocks);
struct NativeProductionFinish {
    bool cancel{};
    std::size_t planet_index{}, domain_index{}, entry_index{};
    ProductionEntry entry;
    PlayerId owner{};
};
[[nodiscard]] core::Result<NativeProductionFinish> prepare_native_production_finish(
    const ProductionSetup&, std::span<const Planet>, EntityId build, bool salvage);
inline math::Fixed native_production_paid(const ProductionEntry& entry) {
    if (!entry.pay_as_you_go) return entry.price;
    if (entry.full_seconds.raw() == 0) return math::Fixed{};
    constexpr auto one = math::Fixed::from_raw(math::Fixed::scale);
    const auto fraction = math::subtract(one, math::divide(entry.last_remaining, entry.full_seconds).value()).value();
    return math::multiply(entry.price, std::clamp(fraction, math::Fixed{}, one)).value();
}
inline void native_production_start(ProductionEntry& entry, std::uint64_t frame) {
    entry.start_frame = frame; entry.duration_frames = production_frames(entry.full_seconds).value(); entry.last_remaining = entry.full_seconds;
}
// WGA-509 (AI planet build queue): the AI does not feed a planet queue in arrival order. When the slot is free
// it starts the waiting task with the least slack: its goal's production estimate minus the elapsed time minus the task's
// own build time. Elapsed time is the same for every task at one moment, so the order is the estimate minus the task's
// own build time. The retained estimate is not kept per entry here: a goal's orders are admitted in one service pass, so
// the waiting entries of one builder that share a creation frame stand for one goal, and its estimate on this queue is
// the work of those entries. A task's key is therefore the build time of the others of its order still waiting. Only AI
// entries take part; a queue whose next entry belongs to a human, or has nobody to compare with, stays in arrival order.
inline bool native_production_ai_entry(const ProductionEntry& entry, std::span<const ProductionPlayer> players) {
    const auto player = std::find_if(players.begin(), players.end(), [&](const auto& row) { return row.player == entry.builder; });
    return entry.pay_as_you_go && player != players.end() && !player->human;
}
inline void native_production_promote(std::vector<ProductionEntry>& queue, std::span<const ProductionPlayer> players) {
    if (queue.size() < 2 || !native_production_ai_entry(queue.front(), players)) return;
    std::size_t best = 0; std::int64_t best_key{};
    for (std::size_t i = 0; i < queue.size(); ++i) {
        if (!native_production_ai_entry(queue[i], players)) continue;
        std::int64_t key{};
        for (std::size_t j = 0; j < queue.size(); ++j)
            if (j != i && queue[j].builder == queue[i].builder && queue[j].start_frame == queue[i].start_frame) key += queue[j].full_seconds.raw();
        if (i == 0 || key < best_key) { best = i; best_key = key; }
    }
    if (best != 0) std::rotate(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(best), queue.begin() + static_cast<std::ptrdiff_t>(best) + 1);
}
inline void native_production_start_next(std::vector<ProductionEntry>& queue, std::span<const ProductionPlayer> players, std::uint64_t frame) {
    native_production_promote(queue, players);
    native_production_start(queue.front(), frame);
}
} // namespace eawr::sim::galactic::session_detail
