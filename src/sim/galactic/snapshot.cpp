#include "eawr/sim/galactic/snapshot.hpp"

#include "eawr/core/sha256.hpp"

#include "encoding.hpp"
#include "eawr/sim/galactic/victory.hpp"

#include <utility>
#include <algorithm>

namespace eawr::sim::galactic {

GalacticSnapshot::GalacticSnapshot(const std::uint64_t completed_tick, const GalacticClock clock,
    std::vector<GalacticPlayer> players, std::vector<Planet> planets, std::vector<Route> routes, std::vector<Fleet> fleets,
    std::vector<GalacticEvent> events, std::optional<math::Fixed> maximum_movement_distance, std::optional<FleetControlSetup> fleet_control, std::shared_ptr<const MovementSetup> movement, std::optional<SeatControlState> seat_control, std::vector<Seat> seats, std::optional<PlayerEconomySetup> player_economy, std::optional<ProductionSetup> production, std::optional<PoliticalControlSetup> control,
    std::vector<PoliticalControlEffect> control_effects, std::vector<PopulationTotal> population, std::optional<ObserverSetup> observer, std::optional<IncomeState> income, std::optional<VictoryState> victory,
    std::vector<ObservedProductionOffer> production_offers,
    std::shared_ptr<const std::vector<ObservedPlanet>> observations, std::optional<EconomyForecast> economy_forecast, std::shared_ptr<const std::vector<CensusObject>> commissioned_originals,
    std::optional<HeroActionMenus> hero_action_menus, std::optional<CorruptionMenu> corruption_menu,
    std::optional<EconomyCorruptionState> economy_corruption,
    std::shared_ptr<const PlanetAccessState> planet_access, std::uint64_t observer_input_generation,
    std::shared_ptr<const EconomyInputGeneration> economy_input_generation,
    std::shared_ptr<const std::vector<CensusObject>> actor_records,
    std::shared_ptr<const std::vector<ObservedBlackMarketMenu>> black_market_menus,
    std::shared_ptr<const std::vector<AbilityResult>> ability_results,
    std::shared_ptr<const HeroActionMenus> live_hero_action_menus,
    std::shared_ptr<const CorruptionMenu> live_corruption_menu,
    std::shared_ptr<const std::vector<CensusType>> sale_types)
    : completed_tick_(completed_tick),
      clock_(clock),
      commissioned_originals_(std::move(commissioned_originals)),
      actor_records_(std::move(actor_records)),
      players_(std::move(players)),
      planets_(std::move(planets)),
      routes_(std::move(routes)),
      fleets_(std::move(fleets)),
      events_(std::move(events)), maximum_movement_distance_(maximum_movement_distance), fleet_control_(std::move(fleet_control)), movement_(std::move(movement)), seat_control_(std::move(seat_control)), seats_(std::move(seats)), player_economy_(std::move(player_economy)), production_(std::move(production)), control_(std::move(control)), control_effects_(std::move(control_effects)), population_(std::move(population)), observer_(std::move(observer)), income_(std::move(income)), victory_(std::move(victory)) {
    if (observer_) observations_ = observations ? std::move(observations)
        : std::make_shared<const std::vector<ObservedPlanet>>(observed_planets(*observer_, fleets_, movement_.get(), this->actor_records()));
    production_offers_ = std::move(production_offers);
    planet_access_ = std::move(planet_access);
    observer_input_generation_ = observer_input_generation;
    economy_input_generation_ = std::move(economy_input_generation);
    economy_corruption_ = std::move(economy_corruption);
    sale_types_ = std::move(sale_types);
    live_corruption_menu_ = std::move(live_corruption_menu);
    black_market_menus_ = std::move(black_market_menus);
    ability_results_ = std::move(ability_results);
    live_hero_action_menus_ = std::move(live_hero_action_menus);
    if (!black_market_menus_ && economy_corruption_ && player_economy_ && observer_)
        black_market_menus_ = std::make_shared<const std::vector<ObservedBlackMarketMenu>>(
            observed_black_market_menus(*economy_corruption_, *player_economy_, observer_->local_player, *observations_));
    if (corruption_menu && observer_ && corruption_menu->player == observer_->local_player
        && std::any_of(observations_->begin(), observations_->end(), [&](const auto& row) {
            return row.player == corruption_menu->player && row.planet == corruption_menu->planet && row.discovered;
        })) corruption_menu_ = std::move(corruption_menu);
    if (hero_action_menus && observer_ && hero_action_menus->player == observer_->local_player
        && std::any_of(observations_->begin(), observations_->end(), [&](const auto& row) {
            return row.player == hero_action_menus->player && row.planet == hero_action_menus->planet && row.discovered;
        })) hero_action_menus_ = std::move(hero_action_menus);
    if (economy_forecast && observer_ && economy_forecast->player == observer_->local_player) {
        // WGUI-13: discovered planets expose basic income; actual income
        // requires its own knowledge predicate, independently of ownership.
        std::vector<PlanetIncomeForecast> admitted;
        for (const auto& observation : *observations_) {
            if (observation.player != economy_forecast->player || !observation.discovered) continue;
            const auto row = std::lower_bound(economy_forecast->planets.begin(), economy_forecast->planets.end(),
                observation.planet, [](const auto& value, PlanetId id) { return value.planet < id; });
            if (row == economy_forecast->planets.end() || row->planet != observation.planet) continue;
            admitted.push_back({row->planet, row->basic, {}, {}, {}});
            auto& value = admitted.back();
            if (observation.knowledge.income || observation.knowledge.income_breakdown) value.actual = row->actual;
            if (observation.knowledge.income_breakdown) { value.trade = row->trade; value.modifiers = row->modifiers; }
        }
        // Observer input order is unconstrained; readers use binary lookup.
        std::sort(admitted.begin(), admitted.end(), [](const auto& a, const auto& b) { return a.planet < b.planet; });
        economy_forecast->planets = std::move(admitted);
        economy_forecast_ = std::move(economy_forecast);
    }
}

std::uint64_t GalacticSnapshot::completed_tick() const noexcept { return completed_tick_; }
const GalacticClock& GalacticSnapshot::clock() const noexcept { return clock_; }
std::span<const GalacticPlayer> GalacticSnapshot::players() const noexcept { return players_; }
std::span<const Planet> GalacticSnapshot::planets() const noexcept { return planets_; }
std::span<const Route> GalacticSnapshot::routes() const noexcept { return routes_; }
std::span<const Fleet> GalacticSnapshot::fleets() const noexcept { return fleets_; }
std::span<const GalacticEvent> GalacticSnapshot::events() const noexcept { return events_; }

std::vector<std::uint8_t> GalacticSnapshot::canonical_bytes() const {
    std::vector<std::uint8_t> bytes(detail::snapshot_magic.begin(), detail::snapshot_magic.end());
    detail::put_u32(bytes, 3); // WGM-169/GAG-03: canonical live actor facts
    detail::put_u64(bytes, completed_tick_);
    detail::put_u64(bytes, clock_.session_tick);
    detail::put_u64(bytes, clock_.galactic_frame);
    detail::put_u64(bytes, clock_.tactical_frame);
    detail::put_u32(bytes, clock_.pause_depth);
    detail::put_u64(bytes, clock_.fiscal_start);
    detail::put_u64(bytes, clock_.fiscal_end);
    detail::put_u64(bytes, clock_.fiscal_cycle);
    detail::put_i64(bytes, clock_.strategic_speed.raw());
    detail::put_i64(bytes, clock_.tactical_speed.raw());
    detail::encode_players(bytes, players_);
    if (player_economy_) detail::encode_player_economy(bytes, *player_economy_);
    detail::put_u32(bytes, static_cast<std::uint32_t>(planets_.size()));
    for (const auto& planet : planets_) detail::encode_planet(bytes, planet);
    detail::put_u32(bytes, static_cast<std::uint32_t>(routes_.size()));
    for (const auto& route : routes_) detail::encode_route(bytes, route);
    detail::put_u32(bytes, static_cast<std::uint32_t>(fleets_.size()));
    for (const auto& fleet : fleets_) detail::encode_fleet(bytes, fleet);
    detail::put_u32(bytes, static_cast<std::uint32_t>(events_.size()));
    for (const auto& event : events_) detail::encode_event(bytes, event);
    if (maximum_movement_distance_) {
        detail::put_u16(bytes, 1); // GC graph extension tag
        detail::put_i64(bytes, maximum_movement_distance_->raw());
        detail::put_u32(bytes, static_cast<std::uint32_t>(routes_.size()));
        for (const auto& route : routes_) {
            detail::put_u64(bytes, route.id);
            detail::put_i64(bytes, route.hs_speed_factor.raw());
        }
    }
    if (fleet_control_) {
        detail::put_u16(bytes, 2);
        detail::encode_fleet_control(bytes, *fleet_control_, fleets_);
    }
    if (movement_) { detail::put_u16(bytes, 4); detail::encode_movement(bytes, *movement_, fleets_); }
    if (seat_control_) {
        detail::put_u16(bytes, 5); // coordinator-reserved
        detail::encode_seat_control_state(bytes, *seat_control_);
        detail::put_u32(bytes, static_cast<std::uint32_t>(seats_.size()));
        for (const auto& seat : seats_) {
            detail::put_u32(bytes, seat.seat_id.value); detail::put_u32(bytes, seat.player);
            detail::put_u8(bytes, static_cast<std::uint8_t>(seat.kind)); detail::put_u32(bytes, seat.lobby_slot);
            detail::put_u8(bytes, seat.active ? 1 : 0); detail::put_u32(bytes, seat.human_side_id);
        }
    }
    if (production_) {
        detail::put_u16(bytes, 7);
        detail::encode_production(bytes, *production_);
    }
    if (control_) {
        detail::put_u16(bytes, 8); detail::encode_control(bytes, *control_);
        detail::put_u32(bytes, static_cast<std::uint32_t>(control_effects_.size()));
        for (const auto& effect : control_effects_) {
            detail::put_u64(bytes, effect.planet); detail::put_u32(bytes, effect.old_owner); detail::put_u32(bytes, effect.new_owner);
            detail::put_u8(bytes, effect.owner_changed ? 1 : 0); detail::put_u8(bytes, effect.stop_production ? 1 : 0);
            detail::put_u8(bytes, effect.refresh_visibility ? 1 : 0); detail::put_u8(bytes, effect.first_capture ? 1 : 0);
            detail::put_i64(bytes, effect.capture_reward.raw());
        }
    }
    if (!population_.empty()) {
        detail::put_u16(bytes, 10);
        detail::encode_population(bytes, population_);
    }
    if (observer_) { detail::put_u16(bytes, 12); detail::encode_observer(bytes, *observer_); }
    if (income_) { detail::put_u16(bytes, 13); detail::encode_income_state(bytes, *income_); }
    if (victory_) { detail::put_u16(bytes, 14); detail::encode_victory(bytes, *victory_); }
    if (economy_corruption_) { detail::put_u16(bytes, 15); detail::encode_economy_corruption(bytes, *economy_corruption_); }
    if (planet_access_) { detail::put_u16(bytes, 19); detail::encode_planet_access(bytes, *planet_access_); }
    PopulationCensus records;
    records.objects.assign(actor_records().begin(), actor_records().end());
    detail::encode_census(bytes, records);
    return bytes;
}

core::Result<TypeId> GalacticSnapshot::current_type(EntityId id) const {
    const auto rows = actor_records();
    const auto at = std::lower_bound(rows.begin(), rows.end(), id,
        [](const auto& row, auto key) { return row.id < key; });
    if (at == rows.end() || at->id != id || !at->current_type)
        return core::Result<TypeId>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "current_representation_unavailable: actor has no qualified current type"));
    return core::Result<TypeId>::success(at->current_type);
}

namespace {
core::Result<bool> missing_actor_fact(const char* seam) {
    return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, seam));
}
core::Result<bool> skip_actor(std::span<const CensusObject> records, EntityId id,
    const MovementSetup* movement, std::span<const Fleet> fleets, unsigned depth) {
    if (depth > 32) return missing_actor_fact("actor_combat_skip_container_cycle");
    const auto at = std::lower_bound(records.begin(), records.end(), id,
        [](const auto& row, auto key) { return row.id < key; });
    if (at == records.end() || at->id != id) return missing_actor_fact("actor_combat_skip_unavailable");
    if (at->delete_pending) return core::Result<bool>::success(true); // WGM-163
    if (at->combat_role == ActorCombatRole::unavailable) return missing_actor_fact("actor_combat_role_unavailable");
    if (at->combat_role == ActorCombatRole::ordinary)
        return core::Result<bool>::success(at->abilities.present && at->abilities.combat_skip);
    std::span<const FleetMember> members;
    bool fleet_breakdown = false;
    if (at->combat_role == ActorCombatRole::fleet) {
        const auto fleet = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == id; });
        if (fleet != fleets.end()) { members = fleet->members; fleet_breakdown = true; }
        // WGM-38: a constructed nested fleet (including the literal squadron
        // container) owns its breakdown in the actual movement contents table.
        else if (!movement) return missing_actor_fact("actor_fleet_breakdown_unavailable");
    }
    if (!fleet_breakdown) {
        if (!movement) return missing_actor_fact("actor_payload_unavailable");
        const auto contents = std::lower_bound(movement->contents.begin(), movement->contents.end(), id,
            [](const auto& row, auto key) { return row.container < key; });
        if (contents == movement->contents.end() || contents->container != id)
            return missing_actor_fact("actor_payload_unavailable");
        members = contents->children;
    }
    bool all = !members.empty();
    for (const auto& member : members) {
        auto skip = skip_actor(records, member.id, movement, fleets, depth + 1);
        if (!skip) return skip;
        all = all && skip.value();
    }
    return core::Result<bool>::success(all);
}
}
core::Result<bool> actor_combat_skip(std::span<const CensusObject> records, EntityId id,
    const MovementSetup* movement, std::span<const Fleet> fleets) {
    return skip_actor(records, id, movement, fleets, 0);
}
core::Result<bool> fleet_combat_skip(const Fleet& fleet, std::span<const CensusObject> records,
    const MovementSetup* movement) {
    const auto owner = std::lower_bound(records.begin(), records.end(), fleet.id,
        [](const auto& row, auto key) { return row.id < key; });
    if (owner != records.end() && owner->id == fleet.id && owner->delete_pending)
        return core::Result<bool>::success(true);
    bool all = !fleet.members.empty();
    for (const auto& member : fleet.members) {
        auto skip = skip_actor(records, member.id, movement, std::span<const Fleet>(&fleet, 1), 0);
        if (!skip) return skip;
        all = all && skip.value();
    }
    return core::Result<bool>::success(all);
}
core::Result<bool> GalacticSnapshot::actor_combat_skip(EntityId id) const {
    return galactic::actor_combat_skip(actor_records(), id, movement(), fleets());
}
core::Result<bool> GalacticSnapshot::fleet_combat_skip(FleetId id) const {
    const auto at = std::lower_bound(fleets_.begin(), fleets_.end(), id,
        [](const auto& row, auto key) { return row.id < key; });
    if (at == fleets_.end() || at->id != id) return missing_actor_fact("fleet_combat_skip_unavailable");
    return galactic::fleet_combat_skip(*at, actor_records(), movement());
}
core::Result<bool> GalacticSnapshot::actor_garrison(EntityId id) const {
    const auto rows = actor_records();
    const auto at = std::lower_bound(rows.begin(), rows.end(), id,
        [](const auto& row, auto key) { return row.id < key; });
    if (at == rows.end() || at->id != id || !at->garrison) return missing_actor_fact("actor_garrison_unavailable");
    return core::Result<bool>::success(*at->garrison);
}
std::string GalacticSnapshot::sha256() const { return core::sha256_hex(canonical_bytes()); }

} // namespace eawr::sim::galactic
