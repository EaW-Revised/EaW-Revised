#pragma once
#include "eawr/sim/galactic/session.hpp"
#include <algorithm>

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
} // namespace eawr::sim::galactic::session_detail
