#include "eawr/sim/galactic/snapshot.hpp"

#include "eawr/core/sha256.hpp"

#include "encoding.hpp"
#include "eawr/sim/galactic/victory.hpp"

#include <utility>
#include <algorithm>

namespace eawr::sim::galactic {

std::shared_ptr<const GalacticSnapshot> GalacticSnapshot::native_view(
    std::shared_ptr<const GalacticSnapshot> base, NativeSnapshotChanges changes) {
    if (!base) return {};
    auto view = std::shared_ptr<GalacticSnapshot>(new GalacticSnapshot());
    if (changes.touched_planets && (!base->native_base_ || base->native_planet_revisions_)) {
        auto& ids = *changes.touched_planets;
        std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        if (!ids.empty() && !ids.front()) ids.erase(ids.begin());
        if (ids.empty() && base->native_planet_revisions_) view->native_planet_revisions_ = base->native_planet_revisions_;
        else {
            auto revisions = base->native_planet_revisions_ ? *base->native_planet_revisions_ : NativePlanetRevisions{};
            auto touched = std::make_shared<const std::vector<PlanetId>>(std::move(ids));
            for (const auto planet : *touched) revisions[planet] = touched;
            view->native_planet_revisions_ = std::make_shared<const NativePlanetRevisions>(std::move(revisions));
        }
    }
    view->native_columns_ = base->native_columns_;
    view->native_base_ = base->native_base_ ? base->native_base_ : std::move(base);
    view->native_changes_ = std::make_shared<const NativeSnapshotChanges>(std::move(changes));
    if (view->native_changes_->fleets) view->native_columns_[static_cast<std::size_t>(NativeColumn::fleets)] = view->native_changes_;
    if (view->native_changes_->events) view->native_columns_[static_cast<std::size_t>(NativeColumn::events)] = view->native_changes_;
    if (view->native_changes_->fleet_control) view->native_columns_[static_cast<std::size_t>(NativeColumn::fleet_control)] = view->native_changes_;
    if (view->native_changes_->movement) view->native_columns_[static_cast<std::size_t>(NativeColumn::movement)] = view->native_changes_;
    if (view->native_changes_->economy) view->native_columns_[static_cast<std::size_t>(NativeColumn::economy)] = view->native_changes_;
    if (view->native_changes_->production) view->native_columns_[static_cast<std::size_t>(NativeColumn::production)] = view->native_changes_;
    if (view->native_changes_->control) view->native_columns_[static_cast<std::size_t>(NativeColumn::control)] = view->native_changes_;
    if (view->native_changes_->population) view->native_columns_[static_cast<std::size_t>(NativeColumn::population)] = view->native_changes_;
    if (view->native_changes_->observer) view->native_columns_[static_cast<std::size_t>(NativeColumn::observer)] = view->native_changes_;
    if (view->native_changes_->victory) view->native_columns_[static_cast<std::size_t>(NativeColumn::victory)] = view->native_changes_;
    if (view->native_changes_->production_offers) view->native_columns_[static_cast<std::size_t>(NativeColumn::production_offers)] = view->native_changes_;
    if (view->native_changes_->observations) view->native_columns_[static_cast<std::size_t>(NativeColumn::observations)] = view->native_changes_;
    if (view->native_changes_->economy_forecast) view->native_columns_[static_cast<std::size_t>(NativeColumn::economy_forecast)] = view->native_changes_;
    if (view->native_changes_->originals) view->native_columns_[static_cast<std::size_t>(NativeColumn::originals)] = view->native_changes_;
    if (view->native_changes_->actors) view->native_columns_[static_cast<std::size_t>(NativeColumn::actors)] = view->native_changes_;
    if (view->native_changes_->observer_generation) view->native_columns_[static_cast<std::size_t>(NativeColumn::observer_generation)] = view->native_changes_;
    if (view->native_changes_->economy_generation) view->native_columns_[static_cast<std::size_t>(NativeColumn::economy_generation)] = view->native_changes_;
    if (view->native_changes_->ability_services) view->native_columns_[static_cast<std::size_t>(NativeColumn::ability_services)] = view->native_changes_;
    return view;
}

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
    std::shared_ptr<const std::vector<CensusType>> sale_types,
    std::shared_ptr<const std::vector<LiveCorruptionTransition>> corruption_transitions,
    std::shared_ptr<const std::vector<ObservedProductionOffer>> shared_production_offers,
    std::shared_ptr<const std::optional<ProductionSetup>> shared_production,
    SharedSnapshotSections shared_sections, const PlayerId view_player)
    : completed_tick_(completed_tick),
      clock_(clock),
      commissioned_originals_(std::move(commissioned_originals)),
      actor_records_(std::move(actor_records)),
      players_(shared_sections.players ? std::move(shared_sections.players)
          : std::make_shared<const std::vector<GalacticPlayer>>(std::move(players))),
      planets_(shared_sections.planets ? std::move(shared_sections.planets)
          : std::make_shared<const std::vector<Planet>>(std::move(planets))),
      routes_(shared_sections.routes ? std::move(shared_sections.routes)
          : std::make_shared<const std::vector<Route>>(std::move(routes))),
      fleets_(shared_sections.fleets ? std::move(shared_sections.fleets)
          : std::make_shared<const std::vector<Fleet>>(std::move(fleets))),
      events_(std::move(events)), maximum_movement_distance_(maximum_movement_distance),
      fleet_control_(shared_sections.fleet_control ? std::move(shared_sections.fleet_control)
          : std::make_shared<const std::optional<FleetControlSetup>>(std::move(fleet_control))),
      movement_(std::move(movement)), seat_control_(std::move(seat_control)), seats_(std::move(seats)),
      player_economy_(shared_sections.player_economy ? std::move(shared_sections.player_economy)
          : std::make_shared<const std::optional<PlayerEconomySetup>>(std::move(player_economy))),
      control_(shared_sections.control ? std::move(shared_sections.control)
          : std::make_shared<const std::optional<PoliticalControlSetup>>(std::move(control))),
      control_effects_(std::move(control_effects)), population_(std::move(population)),
      observer_(shared_sections.observer ? std::move(shared_sections.observer)
          : std::make_shared<const std::optional<ObserverSetup>>(std::move(observer))),
      income_(shared_sections.income ? std::move(shared_sections.income)
          : std::make_shared<const std::optional<IncomeState>>(std::move(income))),
      victory_(shared_sections.victory ? std::move(shared_sections.victory)
          : std::make_shared<const std::optional<VictoryState>>(std::move(victory))) {
    history_ = std::move(shared_sections.history);
    view_player_ = view_player ? view_player : *observer_ ? (*observer_)->solo_reference_human : 0;
    if (*observer_) observations_ = observations ? std::move(observations)
        : std::make_shared<const std::vector<ObservedPlanet>>(observed_planets(**observer_, *fleets_, {view_player}, movement_.get(), this->actor_records()));
    production_ = shared_production ? std::move(shared_production)
        : std::make_shared<const std::optional<ProductionSetup>>(std::move(production));
    production_offers_ = shared_production_offers ? std::move(shared_production_offers)
        : std::make_shared<const std::vector<ObservedProductionOffer>>(std::move(production_offers));
    planet_access_ = std::move(planet_access);
    observer_input_generation_ = observer_input_generation;
    economy_input_generation_ = std::move(economy_input_generation);
    economy_corruption_ = shared_sections.economy_corruption ? std::move(shared_sections.economy_corruption)
        : std::make_shared<const std::optional<EconomyCorruptionState>>(std::move(economy_corruption));
    sale_types_ = std::move(sale_types);
    live_corruption_menu_ = std::move(live_corruption_menu);
    black_market_menus_ = std::move(black_market_menus);
    ability_results_ = std::move(ability_results);
    ability_services_ = std::move(shared_sections.ability_services);
    corruption_transitions_ = std::move(corruption_transitions);
    live_hero_action_menus_ = std::move(live_hero_action_menus);
    if (!black_market_menus_ && *economy_corruption_ && *player_economy_ && *observer_)
        black_market_menus_ = std::make_shared<const std::vector<ObservedBlackMarketMenu>>(
            observed_black_market_menus(**economy_corruption_, **player_economy_, view_player_, *observations_));
    if (corruption_menu && *observer_ && corruption_menu->player == view_player_
        && std::any_of(observations_->begin(), observations_->end(), [&](const auto& row) {
            return row.player == corruption_menu->player && row.planet == corruption_menu->planet && row.discovered;
        })) corruption_menu_ = std::move(corruption_menu);
    if (hero_action_menus && *observer_ && hero_action_menus->player == view_player_
        && std::any_of(observations_->begin(), observations_->end(), [&](const auto& row) {
            return row.player == hero_action_menus->player && row.planet == hero_action_menus->planet && row.discovered;
        })) hero_action_menus_ = std::move(hero_action_menus);
    if (economy_forecast && *observer_ && economy_forecast->player == view_player_) {
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

std::uint64_t GalacticSnapshot::completed_tick() const noexcept {
    if (native_base_) return native_base_->completed_tick();
    return completed_tick_;
}
const GalacticClock& GalacticSnapshot::clock() const noexcept {
    if (native_base_) return native_base_->clock();
    return clock_;
}
std::span<const GalacticPlayer> GalacticSnapshot::players() const noexcept {
    if (native_base_) return native_base_->players();
    return *players_;
}
std::span<const Planet> GalacticSnapshot::planets() const noexcept {
    if (native_base_) return native_base_->planets();
    return *planets_;
}
std::span<const Route> GalacticSnapshot::routes() const noexcept {
    if (native_base_) return native_base_->routes();
    return *routes_;
}
std::span<const Fleet> GalacticSnapshot::fleets() const noexcept {
    if (native_base_) {
        if (const auto* changes = native_column(NativeColumn::fleets)) return std::span<const Fleet>(*changes->fleets);
        return native_base_->fleets();
    }
    return *fleets_;
}
std::span<const GalacticEvent> GalacticSnapshot::events() const noexcept {
    if (native_base_) {
        if (const auto* changes = native_column(NativeColumn::events)) return std::span<const GalacticEvent>(*changes->events);
        return native_base_->events();
    }
    return events_;
}

std::vector<std::uint8_t> GalacticSnapshot::canonical_bytes() const {
    std::vector<std::uint8_t> bytes(detail::snapshot_magic.begin(), detail::snapshot_magic.end());
    detail::put_u32(bytes, 7); // immutable observer-authorized History publication
    detail::put_u64(bytes, completed_tick());
    detail::put_u64(bytes, clock().session_tick);
    detail::put_u64(bytes, clock().galactic_frame);
    detail::put_u64(bytes, clock().tactical_frame);
    detail::put_u32(bytes, clock().pause_depth);
    detail::put_u64(bytes, clock().fiscal_start);
    detail::put_u64(bytes, clock().fiscal_end);
    detail::put_u64(bytes, clock().fiscal_cycle);
    detail::put_i64(bytes, clock().strategic_speed.raw());
    detail::put_i64(bytes, clock().tactical_speed.raw());
    detail::encode_players(bytes, players());
    if (player_economy()) detail::encode_player_economy(bytes, *player_economy());
    detail::put_u32(bytes, static_cast<std::uint32_t>(planets().size()));
    for (const auto& planet : planets()) detail::encode_planet(bytes, planet);
    detail::put_u32(bytes, static_cast<std::uint32_t>(routes().size()));
    for (const auto& route : routes()) detail::encode_route(bytes, route);
    detail::put_u32(bytes, static_cast<std::uint32_t>(fleets().size()));
    for (const auto& fleet : fleets()) detail::encode_fleet(bytes, fleet);
    detail::put_u32(bytes, static_cast<std::uint32_t>(events().size()));
    for (const auto& event : events()) detail::encode_event(bytes, event);
    if (maximum_movement_distance()) {
        detail::put_u16(bytes, 1); // GC graph extension tag
        detail::put_i64(bytes, maximum_movement_distance()->raw());
        detail::put_u32(bytes, static_cast<std::uint32_t>(routes().size()));
        for (const auto& route : routes()) {
            detail::put_u64(bytes, route.id);
            detail::put_i64(bytes, route.hs_speed_factor.raw());
        }
    }
    if (fleet_control()) {
        detail::put_u16(bytes, 2);
        detail::encode_fleet_control(bytes, *fleet_control(), fleets());
    }
    if (movement()) { detail::put_u16(bytes, 4); detail::encode_movement(bytes, *movement(), fleets()); }
    if (seat_control()) {
        detail::put_u16(bytes, 5); // coordinator-reserved
        detail::encode_seat_control_state(bytes, *seat_control());
        detail::put_u32(bytes, static_cast<std::uint32_t>(seats().size()));
        for (const auto& seat : seats()) {
            detail::put_u32(bytes, seat.seat_id.value); detail::put_u32(bytes, seat.player);
            detail::put_u8(bytes, static_cast<std::uint8_t>(seat.kind)); detail::put_u32(bytes, seat.lobby_slot);
            detail::put_u8(bytes, seat.active ? 1 : 0); detail::put_u32(bytes, seat.human_side_id);
        }
    }
    if (production()) {
        detail::put_u16(bytes, 7);
        detail::encode_production(bytes, *production());
    }
    if (control()) {
        detail::put_u16(bytes, 8); detail::encode_control(bytes, *control());
        detail::put_u32(bytes, static_cast<std::uint32_t>(control_effects().size()));
        for (const auto& effect : control_effects()) {
            detail::put_u64(bytes, effect.planet); detail::put_u32(bytes, effect.old_owner); detail::put_u32(bytes, effect.new_owner);
            detail::put_u8(bytes, effect.owner_changed ? 1 : 0); detail::put_u8(bytes, effect.stop_production ? 1 : 0);
            detail::put_u8(bytes, effect.refresh_visibility ? 1 : 0); detail::put_u8(bytes, effect.first_capture ? 1 : 0);
            detail::put_i64(bytes, effect.capture_reward.raw());
        }
    }
    if (!population().empty()) {
        detail::put_u16(bytes, 10);
        detail::encode_population(bytes, population());
    }
    if (observer()) { detail::put_u16(bytes, 12); detail::encode_observer(bytes, *observer()); }
    if (income()) { detail::put_u16(bytes, 13); detail::encode_income_state(bytes, *income()); }
    if (victory()) { detail::put_u16(bytes, 14); detail::encode_victory(bytes, *victory()); }
    if (economy_corruption()) { detail::put_u16(bytes, 15); detail::encode_economy_corruption(bytes, *economy_corruption()); }
    if (planet_access()) { detail::put_u16(bytes, 19); detail::encode_planet_access(bytes, *planet_access()); }
    PopulationCensus records;
    records.objects.assign(actor_records().begin(), actor_records().end());
    detail::encode_census(bytes, records);
    detail::put_u8(bytes, history() ? 1 : 0);
    if (history()) detail::encode_history_snapshot(bytes, *history());
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
    const auto at = std::lower_bound(fleets().begin(), fleets().end(), id,
        [](const auto& row, auto key) { return row.id < key; });
    if (at == fleets().end() || at->id != id) return missing_actor_fact("fleet_combat_skip_unavailable");
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
