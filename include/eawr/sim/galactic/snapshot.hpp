#pragma once

#include "eawr/sim/galactic/events.hpp"
#include "eawr/sim/galactic/black_market_views.hpp"
#include "eawr/sim/galactic/corruption_views.hpp"
#include "eawr/sim/galactic/hero_action_views.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/types.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

// The published, read-only view of one completed galactic tick: the only galaxy-to-viewer
// boundary (ADR-011, ADR-019). Presentation reads this header's types and never the session.
namespace eawr::sim::galactic {

// WGA-297: derived publisher generations. A retained record also identifies its
// publisher across create/restore; these values never enter canonical bytes.
struct EconomyInputGeneration {
    std::uint64_t census{1}, accounts{1};
    // Actual per-planet income before observer presentation filtering.
    std::optional<std::vector<std::pair<PlanetId, math::Fixed>>> income;
};

// The galactic clock (ADR-013, docs/galactic-architecture.md section 2). The session tick advances
// on every step; the galactic frame only when the galaxy is serviced (not paused, no battle); the
// tactical frame is the battle's own. The fiscal window is [fiscal_start, fiscal_end) in galactic
// frames, `fiscal_cycle` the cycles completed. The speeds are pacing only.
struct GalacticClock {
    std::uint64_t session_tick{};
    std::uint64_t galactic_frame{};
    std::uint64_t tactical_frame{};
    std::uint32_t pause_depth{};
    std::uint64_t fiscal_start{};
    std::uint64_t fiscal_end{};
    std::uint64_t fiscal_cycle{};
    math::Fixed strategic_speed{math::Fixed::from_raw(math::Fixed::scale)};
    math::Fixed tactical_speed{math::Fixed::from_raw(math::Fixed::scale)};
    friend constexpr bool operator==(const GalacticClock&, const GalacticClock&) noexcept = default;
};

// WGUI-35 / WGEP-04: observer-local preview, excluded from canonical state.
// Missing WGEP-06..09 components (legacy #2365) and corruption (legacy #1130)
// remain unavailable rather than being represented as a known zero.
struct PlanetIncomeForecast {
    PlanetId planet{};
    math::Fixed basic;
    std::optional<math::Fixed> actual;
    // GREF-09/13: copied component values, distinct from actual total income.
    std::optional<math::Fixed> trade;
    struct Modifier {
        TypeId source_type{};
        math::Fixed income;
        friend bool operator==(const Modifier&, const Modifier&) = default;
    };
    std::optional<std::vector<Modifier>> modifiers;
    friend bool operator==(const PlanetIncomeForecast&, const PlanetIncomeForecast&) = default;
};
struct EconomyForecast {
    PlayerId player{};
    // GHUD-R01: the Total label is gross away from the WGEP-17 account cap.
    // "Income display at the credit cap" in gc-hud-inventory.md is capture-needed.
    math::Fixed base, bonus, gross_total;
    // Uses the publication-time balance; intervening debits can change the clamp.
    std::optional<math::Fixed> adjusted_payment;
    std::optional<math::Fixed> trade_routes, alignment, political, corruption;
    std::vector<PlanetIncomeForecast> planets;
    friend bool operator==(const EconomyForecast&, const EconomyForecast&) = default;
};

class GalacticSnapshot final {
public:
    GalacticSnapshot(std::uint64_t completed_tick, GalacticClock clock, std::vector<GalacticPlayer> players,
        std::vector<Planet> planets, std::vector<Route> routes, std::vector<Fleet> fleets,
        std::vector<GalacticEvent> events, std::optional<math::Fixed> maximum_movement_distance = std::nullopt,
        std::optional<FleetControlSetup> fleet_control = std::nullopt,
        std::shared_ptr<const MovementSetup> movement = {}, std::optional<SeatControlState> seat_control = std::nullopt, std::vector<Seat> seats = {}, std::optional<PlayerEconomySetup> player_economy = std::nullopt,
        std::optional<ProductionSetup> production = std::nullopt,

        std::optional<PoliticalControlSetup> control = std::nullopt,
        std::vector<PoliticalControlEffect> control_effects = {}, std::vector<PopulationTotal> population = {}, std::optional<ObserverSetup> observer = std::nullopt,
        std::optional<IncomeState> income = std::nullopt, std::optional<VictoryState> victory = std::nullopt,
        std::vector<ObservedProductionOffer> production_offers = {},
        std::shared_ptr<const std::vector<ObservedPlanet>> observations = {},
        std::optional<EconomyForecast> economy_forecast = std::nullopt,
        std::shared_ptr<const std::vector<CensusObject>> commissioned_originals = {},
        std::optional<HeroActionMenus> hero_action_menus = std::nullopt,
        std::optional<CorruptionMenu> corruption_menu = std::nullopt,
        std::optional<EconomyCorruptionState> economy_corruption = std::nullopt,
        std::shared_ptr<const PlanetAccessState> planet_access = {},
        std::uint64_t observer_input_generation = 0,
        std::shared_ptr<const EconomyInputGeneration> economy_input_generation = {},
        std::shared_ptr<const std::vector<CensusObject>> actor_records = {},
        std::shared_ptr<const std::vector<ObservedBlackMarketMenu>> black_market_menus = {},
        std::shared_ptr<const std::vector<AbilityResult>> ability_results = {},
        std::shared_ptr<const HeroActionMenus> live_hero_action_menus = {},
        std::shared_ptr<const CorruptionMenu> live_corruption_menu = {},
        std::shared_ptr<const std::vector<CensusType>> sale_types = {});

    [[nodiscard]] std::span<const CensusType> sale_types() const noexcept {
        return sale_types_ ? std::span<const CensusType>(*sale_types_) : std::span<const CensusType>{};
    }
    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    [[nodiscard]] const GalacticClock& clock() const noexcept;
    [[nodiscard]] std::span<const GalacticPlayer> players() const noexcept; // ascending ID
    [[nodiscard]] std::span<const Planet> planets() const noexcept;         // ascending ID
    [[nodiscard]] std::span<const Route> routes() const noexcept;           // ascending ID
    [[nodiscard]] std::optional<math::Fixed> maximum_movement_distance() const noexcept { return maximum_movement_distance_; }
    [[nodiscard]] const std::optional<FleetControlSetup>& fleet_control() const noexcept { return fleet_control_; }
    [[nodiscard]] const std::optional<PlayerEconomySetup>& player_economy() const noexcept { return player_economy_; }
    [[nodiscard]] const MovementSetup* movement() const noexcept { return movement_.get(); }
    [[nodiscard]] const std::optional<SeatControlState>& seat_control() const noexcept { return seat_control_; }
    [[nodiscard]] std::span<const Seat> seats() const noexcept { return seats_; }
    [[nodiscard]] const std::optional<ProductionSetup>& production() const noexcept { return production_; }
    [[nodiscard]] std::span<const ObservedProductionOffer> production_offers() const noexcept { return production_offers_; }
    [[nodiscard]] const CorruptionMenu* corruption_menu() const noexcept {
        return corruption_menu_ ? &*corruption_menu_ : live_corruption_menu_.get();
    }
    [[nodiscard]] const HeroActionMenus* hero_action_menus() const noexcept {
        return hero_action_menus_ ? &*hero_action_menus_ : live_hero_action_menus_.get();
    }
    [[nodiscard]] const std::optional<VictoryState>& victory() const noexcept { return victory_; }
    [[nodiscard]] std::span<const PopulationTotal> population() const noexcept { return population_; }
    [[nodiscard]] const IncomeState* income() const noexcept { return income_ ? &*income_ : nullptr; }
    [[nodiscard]] const EconomyCorruptionState* economy_corruption() const noexcept {
        return economy_corruption_ ? &*economy_corruption_ : nullptr;
    }
    [[nodiscard]] std::span<const ObservedBlackMarketMenu> black_market_menus() const noexcept {
        return black_market_menus_ ? std::span<const ObservedBlackMarketMenu>(*black_market_menus_)
            : std::span<const ObservedBlackMarketMenu>{};
    }
    [[nodiscard]] const std::shared_ptr<const std::vector<ObservedBlackMarketMenu>>& black_market_publication() const noexcept {
        return black_market_menus_;
    }
    [[nodiscard]] const EconomyForecast* economy_forecast() const noexcept {
        return economy_forecast_ ? &*economy_forecast_ : nullptr;
    }
    [[nodiscard]] const PoliticalControlSetup* control() const noexcept { return control_ ? &*control_ : nullptr; }
    [[nodiscard]] std::span<const PoliticalControlEffect> control_effects() const noexcept { return control_effects_; }
    [[nodiscard]] const std::optional<ObserverSetup>& observer() const noexcept { return observer_; }
    [[nodiscard]] std::span<const ObservedPlanet> observations() const noexcept {
        return observations_ ? std::span<const ObservedPlanet>(*observations_) : std::span<const ObservedPlanet>{};
    }
    [[nodiscard]] const PlanetAccessState* planet_access() const noexcept { return planet_access_.get(); }
    // Derived cache identity, deliberately excluded from canonical state.
    [[nodiscard]] std::uint64_t observer_input_generation() const noexcept { return observer_input_generation_; }
    [[nodiscard]] const std::shared_ptr<const EconomyInputGeneration>& economy_input_generation() const noexcept {
        return economy_input_generation_;
    }
    // Original identities of commissioned nested forces; locations remain in live presence.
    [[nodiscard]] std::span<const CensusObject> commissioned_originals() const noexcept { return commissioned_originals_ ? std::span<const CensusObject>(*commissioned_originals_) : std::span<const CensusObject>{}; }
    [[nodiscard]] std::span<const CensusObject> actor_records() const noexcept {
        return actor_records_ ? std::span<const CensusObject>(*actor_records_) : std::span<const CensusObject>{};
    }
    [[nodiscard]] core::Result<TypeId> current_type(EntityId) const;
    [[nodiscard]] core::Result<bool> actor_combat_skip(EntityId) const;
    [[nodiscard]] core::Result<bool> fleet_combat_skip(FleetId) const;
    [[nodiscard]] core::Result<bool> actor_garrison(EntityId) const;
    [[nodiscard]] std::span<const Fleet> fleets() const noexcept;           // ascending ID
    // This tick's events in commit order (canonical command order for command events).
    [[nodiscard]] std::span<const GalacticEvent> events() const noexcept;
    [[nodiscard]] std::span<const AbilityResult> ability_results() const noexcept {
        return ability_results_ ? std::span<const AbilityResult>(*ability_results_) : std::span<const AbilityResult>{};
    }
    [[nodiscard]] std::vector<std::uint8_t> canonical_bytes() const;
    [[nodiscard]] std::string sha256() const;

private:
    std::shared_ptr<const std::vector<AbilityResult>> ability_results_;
    std::uint64_t completed_tick_{};
    GalacticClock clock_{};
    std::shared_ptr<const std::vector<CensusType>> sale_types_;
    std::shared_ptr<const std::vector<CensusObject>> commissioned_originals_;
    std::shared_ptr<const std::vector<CensusObject>> actor_records_;
    std::vector<GalacticPlayer> players_;
    std::vector<Planet> planets_;
    std::vector<Route> routes_;
    std::vector<Fleet> fleets_;
    std::vector<GalacticEvent> events_;
    std::optional<math::Fixed> maximum_movement_distance_;
    std::optional<FleetControlSetup> fleet_control_;
    std::shared_ptr<const MovementSetup> movement_;
    std::optional<SeatControlState> seat_control_;
    std::vector<Seat> seats_;
    std::optional<PlayerEconomySetup> player_economy_;
    std::optional<ProductionSetup> production_;
    std::optional<PoliticalControlSetup> control_;
    std::vector<PoliticalControlEffect> control_effects_;
    std::vector<PopulationTotal> population_;
    std::optional<ObserverSetup> observer_;
    std::shared_ptr<const std::vector<ObservedPlanet>> observations_;
    std::optional<IncomeState> income_;
    std::optional<VictoryState> victory_;
    // Derived observer-local presentation data, excluded from canonical bytes.
    std::vector<ObservedProductionOffer> production_offers_;
    std::optional<EconomyForecast> economy_forecast_;
    // Derived observer-local prepared facts; the campaign host remains a seam.
    std::optional<HeroActionMenus> hero_action_menus_;
    std::shared_ptr<const HeroActionMenus> live_hero_action_menus_;
    std::optional<EconomyCorruptionState> economy_corruption_;
    std::shared_ptr<const std::vector<ObservedBlackMarketMenu>> black_market_menus_;
    std::optional<CorruptionMenu> corruption_menu_;
    std::shared_ptr<const CorruptionMenu> live_corruption_menu_;
    std::shared_ptr<const PlanetAccessState> planet_access_;
    std::uint64_t observer_input_generation_{};
    std::shared_ptr<const EconomyInputGeneration> economy_input_generation_;
};

// Immutable owning facts for partitioned consumers; no static type fallback.
[[nodiscard]] core::Result<bool> actor_combat_skip(std::span<const CensusObject>, EntityId,
    const MovementSetup*, std::span<const Fleet> = {});
[[nodiscard]] core::Result<bool> fleet_combat_skip(const Fleet&, std::span<const CensusObject>, const MovementSetup*);
} // namespace eawr::sim::galactic
