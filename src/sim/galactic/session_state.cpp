#include "eawr/sim/galactic/session.hpp"
#include "session_impl.hpp"
#include "session_diagnostic.hpp"
#include "encoding.hpp"
#include "../replay_internal.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/sim/galactic/economy_corruption.hpp"
#include "eawr/sim/galactic/ability_host.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <span>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace eawr::sim::galactic {
using session_detail::fail;
using session_detail::FleetData;
using session_detail::GatheredObject;
using session_detail::ObjectKind;
using session_detail::PlanetData;
using session_detail::RouteData;
using session_detail::StableId;

namespace {
bool hash_verification_requested() {
#ifdef _MSC_VER
    char* value{}; std::size_t size{};
    if (_dupenv_s(&value, &size, "EAWR_VERIFY_GALACTIC_HASH") || !value) return false;
    const bool enabled = std::string_view(value) == "1";
    std::free(value); return enabled;
#else
    const auto* value = std::getenv("EAWR_VERIFY_GALACTIC_HASH");
    return value && std::string_view(value) == "1";
#endif
}

// Adding fields must fail compilation until the field-wise snapshot copy below
// is updated. The never-invoked lambda checks aggregate arity without runtime work.
[[maybe_unused]] constexpr auto check_production_snapshot_fields = [](
    const ProductionSetup& production, const NestedCreationSetup& nested) {
    [[maybe_unused]] const auto& [maximum_human_queue, players, planets,
        fleet_blockers, nested_creation, technology_types] = production;
    [[maybe_unused]] const auto& [fleet_type, types, relations, squadron_type] = nested;
};

// Stage the nested deep copy at the existing publication boundary. Workers read
// the immutable live view and own disjoint rows; only the caller publishes it.
[[nodiscard]] std::optional<ProductionSetup> copy_production_snapshot(
    const std::optional<ProductionSetup>& source, const PartitionExecutor& executor) {
    if (!source) return std::nullopt;
    std::optional<ProductionSetup> staged(std::in_place);
    auto& target = *staged;
    target.maximum_human_queue = source->maximum_human_queue;
    // Allocate output slots before dispatch; no worker changes vector sizes.
    target.players.resize(source->players.size());
    target.planets.resize(source->planets.size());
    target.technology_types.resize(source->technology_types.size());
    if (source->fleet_blockers) target.fleet_blockers.emplace(source->fleet_blockers->size());
    if (source->nested_creation) {
        auto& nested = target.nested_creation.emplace();
        nested.fleet_type = source->nested_creation->fleet_type;
        nested.squadron_type = source->nested_creation->squadron_type;
        nested.types.resize(source->nested_creation->types.size());
        nested.relations.resize(source->nested_creation->relations.size());
    }
    const auto phase = executor.execute_phase("galactic-production-snapshot", tick_partition_count,
        [&](const std::size_t partition) {
            const auto copy_rows = [&](const auto& from, auto& to) {
                const auto range = partition_range(partition, from.size());
                for (auto i = range.begin; i < range.end; ++i) to[i] = from[i];
            };
            copy_rows(source->players, target.players);
            copy_rows(source->planets, target.planets);
            copy_rows(source->technology_types, target.technology_types);
            if (source->fleet_blockers) copy_rows(*source->fleet_blockers, *target.fleet_blockers);
            if (source->nested_creation) {
                copy_rows(source->nested_creation->types, target.nested_creation->types);
                copy_rows(source->nested_creation->relations, target.nested_creation->relations);
            }
        });
    // Like offer preparation, a failed worker phase must not publish partial
    // rows. The original complete serial copy remains the failure fallback.
    if (!phase) return source;
    return staged;
}

// SHA-256 over the encoding name and each section's ID, version and digest, in section order.
[[nodiscard]] std::string combine(const std::array<std::string, state_section_count>& section_hex, const std::vector<std::size_t>& ids) {
    std::vector<std::uint8_t> table(state_encoding_name.begin(), state_encoding_name.end());
    detail::put_u32(table, static_cast<std::uint32_t>(ids.size()));
    for (const auto section : ids) {
        detail::put_u16(table, static_cast<std::uint16_t>(section));
        detail::put_u16(table, detail::section_version);
        detail::put_bytes(table, detail::digest_bytes(section_hex[section]));
    }
    return core::sha256_hex(table);
}

} // namespace

void GalacticSession::Impl::insert(const Planet& planet) {
    const auto handle = registry.create();
    registry.emplace<StableId>(handle, planet.id);
    registry.emplace<PlanetData>(handle, planet.position, planet.owner);
    planets.emplace(planet.id, handle);
}

void GalacticSession::Impl::insert(const Route& route) {
    const auto handle = registry.create();
    registry.emplace<StableId>(handle, route.id);
    registry.emplace<RouteData>(handle, route.from, route.to, route.hs_speed_factor);
    routes.emplace(route.id, handle);
}

void GalacticSession::Impl::insert(const Fleet& fleet) {
    auto motion = fleet.motion;
    if (setup.movement && motion.orbit_entry == 0) {
        std::uint64_t latest = 0;
        for (const auto& [id, existing] : fleets) {
            (void)id; latest = std::max(latest, registry.get<FleetData>(existing).motion.orbit_entry);
        }
        motion.orbit_entry = latest + 1;
    }
    const auto handle = registry.create();
    registry.emplace<StableId>(handle, fleet.id);
    registry.emplace<FleetData>(handle, fleet.owner, fleet.orbit, fleet.members, fleet.orbit_slot, motion, fleet.raid_destination);
    fleets.emplace(fleet.id, handle);
}

void GalacticSession::Impl::rebuild(const bool reverse) {
    auto planet_rows = sorted_planets();
    auto route_rows = sorted_routes();
    auto fleet_rows = sorted_fleets();
    registry.clear();
    planets.clear();
    routes.clear();
    fleets.clear();
    if (reverse) {
        std::reverse(planet_rows.begin(), planet_rows.end());
        std::reverse(route_rows.begin(), route_rows.end());
        std::reverse(fleet_rows.begin(), fleet_rows.end());
        // Interleave the kinds the other way round too, so no storage keeps its old order.
        for (const auto& fleet : fleet_rows) insert(fleet);
        for (const auto& route : route_rows) insert(route);
        for (const auto& planet : planet_rows) insert(planet);
        return;
    }
    for (const auto& planet : planet_rows) insert(planet);
    for (const auto& route : route_rows) insert(route);
    for (const auto& fleet : fleet_rows) insert(fleet);
}

std::vector<Planet> GalacticSession::Impl::sorted_planets() const {
    std::vector<Planet> result;
    result.reserve(planets.size());
    for (const auto& [id, handle] : planets) {
        const auto& data = registry.get<PlanetData>(handle);
        result.push_back({id, data.position, data.owner});
    }
    return result;
}

std::vector<Route> GalacticSession::Impl::sorted_routes() const {
    std::vector<Route> result;
    result.reserve(routes.size());
    for (const auto& [id, handle] : routes) {
        const auto& data = registry.get<RouteData>(handle);
        result.push_back({id, data.from, data.to, data.hs_speed_factor});
    }
    return result;
}

std::vector<Fleet> GalacticSession::Impl::sorted_fleets() const {
    std::vector<Fleet> result;
    result.reserve(fleets.size());
    for (const auto& [id, handle] : fleets) {
        const auto& data = registry.get<FleetData>(handle);
        result.push_back({id, data.owner, data.orbit, data.members, data.orbit_slot, data.motion, data.raid_destination});
    }
    return result;
}

std::vector<std::pair<EntityId, ObjectKind>> GalacticSession::Impl::sorted_objects() const {
    std::vector<std::pair<EntityId, ObjectKind>> result;
    result.reserve(planets.size() + routes.size() + fleets.size());
    for (const auto& entry : planets) result.emplace_back(entry.first, ObjectKind::planet);
    for (const auto& entry : routes) result.emplace_back(entry.first, ObjectKind::route);
    for (const auto& entry : fleets) result.emplace_back(entry.first, ObjectKind::fleet);
    std::sort(result.begin(), result.end());
    return result;
}

GatheredObject GalacticSession::Impl::gather(const EntityId id, const ObjectKind kind) const {
    GatheredObject result{id, kind, {}, {}, {}};
    switch (kind) {
    case ObjectKind::planet: result.planet = registry.get<PlanetData>(planets.at(id)); break;
    case ObjectKind::route: result.route = registry.get<RouteData>(routes.at(id)); break;
    case ObjectKind::fleet: result.fleet = registry.get<FleetData>(fleets.at(id)); break;
    }
    return result;
}

std::vector<std::uint8_t> GalacticSession::Impl::encode_section(const StateSection section) const {
    std::vector<std::uint8_t> bytes;
    switch (section) {
    case StateSection::history:
        detail::encode_history(bytes, history_archive);
        break;
    case StateSection::campaign_scoring:
        detail::encode_score_ledger(bytes, score_ledger);
        break;
    case StateSection::ability_host:
        if (ability_host) { detail::encode_ability_setup(bytes, *setup.ability_host); detail::encode_ability_state(bytes, *ability_host); }
        break;
    case StateSection::session:
        detail::put_u64(bytes, setup.seed);
        detail::put_bytes(bytes, setup.content_identity);
        detail::put_bytes(bytes, setup.campaign_identity);
        detail::put_u64(bytes, completed_tick);
        detail::put_u32(bytes, setup.rules.fiscal_cycle_frames);
        detail::put_u64(bytes, clock.galactic_frame);
        detail::put_u64(bytes, clock.tactical_frame);
        detail::put_u32(bytes, clock.pause_depth);
        detail::put_u64(bytes, clock.fiscal_start);
        detail::put_u64(bytes, clock.fiscal_end);
        detail::put_u64(bytes, clock.fiscal_cycle);
        // The speed settings are pacing only (WGM-U02): session state, never hashed.
        detail::put_u64(bytes, next_id);
        detail::put_u8(bytes, setup.debug_commands ? 1U : 0U);
        detail::put_u32(bytes, static_cast<std::uint32_t>(seats.size()));
        for (const auto& row : seats) {
            detail::put_u32(bytes, row.seat_id.value);
            detail::put_u32(bytes, row.player);
            detail::put_u8(bytes, static_cast<std::uint8_t>(row.kind));
            detail::put_u32(bytes, row.lobby_slot);
            detail::put_u8(bytes, row.active ? 1U : 0U);
            detail::put_u32(bytes, row.human_side_id);
        }
        if (seat_control) {
            detail::put_u16(bytes, 5); // coordinator-reserved, opt-in policy tail
            detail::encode_seat_control_state(bytes, *seat_control);
        }
        break;
    case StateSection::players:
        detail::encode_players(bytes, setup.players);
        if (economy) detail::encode_player_economy(bytes, *economy);
        if (setup.fiscal) detail::encode_fiscal(bytes, *setup.fiscal);
        break;
    case StateSection::planets: {
        detail::put_u32(bytes, static_cast<std::uint32_t>(planets.size()));
        for (const auto& planet : sorted_planets()) detail::encode_planet(bytes, planet);
        break;
    }
    case StateSection::routes: {
        detail::put_u32(bytes, static_cast<std::uint32_t>(routes.size()));
        for (const auto& route : sorted_routes()) detail::encode_route(bytes, route);
        break;
    }
    case StateSection::graph: {
        detail::put_i64(bytes, setup.rules.maximum_fleet_movement_distance->raw());
        detail::put_u32(bytes, static_cast<std::uint32_t>(routes.size()));
        for (const auto& route : sorted_routes()) {
            detail::put_u64(bytes, route.id);
            detail::put_i64(bytes, route.hs_speed_factor.raw());
        }
        break;
    }
    case StateSection::victory:
        detail::encode_victory(bytes, *victory);
        break;
    case StateSection::control:
        detail::encode_control(bytes, *control);
        break;
    case StateSection::movement:
        if (movement_inputs) detail::encode_movement(bytes, *movement_inputs, sorted_fleets());
        break;
    case StateSection::fleet_control:
        detail::encode_fleet_control(bytes, fleet_control_state(), sorted_fleets());
        break;
    case StateSection::census: {
        auto live = *setup.census;
        live.objects.clear();
        for (const auto& [id, row] : actor_records) { (void)id; live.objects.push_back(row); }
        std::vector<std::uint8_t> encoded;
        detail::encode_census(encoded, live);
        detail::put_u32(bytes, static_cast<std::uint32_t>(encoded.size()));
        detail::put_bytes(bytes, encoded);
        detail::encode_population(bytes, population);
        break;
    }
    case StateSection::observer:
        detail::encode_observer(bytes, *observer);
        break;
    case StateSection::planet_access:
        detail::encode_planet_access(bytes, *planet_access);
        break;
    case StateSection::production:
        detail::encode_production(bytes, *production);
        break;
    case StateSection::income:
        detail::encode_income_state(bytes, *income);
        break;
    case StateSection::economy_corruption:
        detail::encode_economy_corruption(bytes, *economy_corruption);
        break;
    case StateSection::native_invasions:
        detail::put_u64(bytes, next_native_invasion);
        detail::put_u64(bytes, native_invasions.size());
        for (const auto& [id, row] : native_invasions) {
            detail::put_u64(bytes, id); detail::put_u8(bytes, static_cast<std::uint8_t>(row.state));
            detail::put_u32(bytes, row.invader); detail::put_u32(bytes, row.defender);
            detail::put_u64(bytes, row.fleet); detail::put_u64(bytes, row.planet);
            detail::put_u64(bytes, row.conflict); detail::put_u8(bytes, row.success);
            detail::put_u8(bytes, row.reader_released);
            detail::put_u64(bytes, row.transports.size());
            for (const auto& transport : row.transports) { detail::put_u64(bytes, transport.id); detail::put_u64(bytes, transport.type); }
        }
        detail::put_u64(bytes, native_invasion_queue.size());
        for (const auto id : native_invasion_queue) detail::put_u64(bytes, id);
        break;
    case StateSection::fleets: {
        detail::put_u32(bytes, static_cast<std::uint32_t>(fleets.size()));
        for (const auto& fleet : sorted_fleets()) detail::encode_fleet(bytes, fleet);
        break;
    }
    case StateSection::outcomes: {
        detail::put_u32(bytes, static_cast<std::uint32_t>(applied_outcomes));
        for (std::size_t i = 0; i < applied_outcomes; ++i) {
            const auto& entry = setup.outcomes[i];
            detail::put_u64(bytes, entry.completed_tick); detail::put_u64(bytes, entry.conflict);
            detail::put_u32(bytes, static_cast<std::uint32_t>(entry.payload.size()));
            detail::put_bytes(bytes, entry.payload);
        }
        break;
    }
    }
    return bytes;
}

std::vector<std::uint8_t> GalacticSession::Impl::encode_hash_section(StateSection section, bool reuse) const {
    if (section != StateSection::production) return encode_section(section);
    std::vector<std::uint8_t> bytes;
    if (reuse) {
        if (!production_offer_hashes) production_offer_hashes = detail::production_offer_digests(*production);
        detail::encode_production_hash(bytes, *production, *production_offer_hashes);
    } else {
        detail::encode_production_hash(bytes, *production, detail::production_offer_digests(*production));
    }
    return bytes;
}

void GalacticSession::Impl::refresh_credit_caps() {
    // GFD-05: ownership commits clamp accounts immediately, before fiscal collection.
    if (!setup.fiscal) return;
    // WGEP-17/60: counts are maintained by ownership commits, avoiding a planet
    // scan per player. This bounded player-list commit refreshes even zero owners.
    for (auto& row : economy->players) {
        const auto count = math::Fixed::from_integer(static_cast<std::int64_t>(std::max(std::size_t{1}, owned_planet_counts[row.player]))).value();
        row.credit_cap = math::multiply(count, setup.fiscal->credit_cap_per_planet).value();
        row.credits = std::min(row.credits, row.credit_cap);
    }
    dirty[static_cast<std::size_t>(StateSection::players)] = true;
}

std::span<const CensusObject> GalacticSession::Impl::actor_inputs() const {
    if (!published_actor_records) {
        std::vector<CensusObject> records; records.reserve(actor_records.size());
        for (const auto& [id, row] : actor_records) { (void)id; records.push_back(row); }
        published_actor_records = std::make_shared<const std::vector<CensusObject>>(std::move(records));
    }
    return *published_actor_records;
}
std::shared_ptr<const GalacticSnapshot> GalacticSession::Impl::publish_native_snapshot(std::vector<GalacticEvent> events) const {
    if (native_commit_publication) {
        ++native_commit_publications;
        NativeSnapshotChanges changes;
        // Cancellation has a refund publication before its removal event.
        // The staged final columns cannot supply this intermediate event prefix.
        changes.events = std::move(events);
        return GalacticSnapshot::native_view(native_commit_publication, std::move(changes));
    }
    return make_snapshot(std::move(events));
}
std::shared_ptr<const GalacticSnapshot> GalacticSession::Impl::make_snapshot(std::vector<GalacticEvent> events,
    const PartitionExecutor* executor, ObserverViewContext view) const {
    const auto view_player = view.player ? view.player : observer ? observer->solo_reference_human : 0;
    auto& cache = view_publications[view.player];
    auto& observer_projection_generation = cache.observer_projection_generation;
    auto& observer_projection = cache.observer_projection;
    auto& published_production_offers = cache.published_production_offers;
    auto& published_offer_inputs = cache.published_offer_inputs;
    auto& published_black_market_menus = cache.published_black_market_menus;
    auto& published_ability_results = cache.published_ability_results;
    auto& published_ability_services = cache.published_ability_services;
    auto& published_corruption_transitions = cache.published_corruption_transitions;
    auto& corruption_publication_key = cache.corruption_publication_key;
    auto& published_corruption_menu = cache.published_corruption_menu;
    auto& published_hero_action_menus = cache.published_hero_action_menus;
    auto& black_market_publication_key = cache.black_market_publication_key;
    gc_profile::Scope profile_scope(gc_profile::Phase::make_snapshot);
    gc_profile::Scope context_scope(gc_profile::Phase::snapshot_other, gc_profile::Phase::snapshot_ai);
    std::optional<gc_profile::Scope> part(std::in_place, gc_profile::Phase::snap_sorted);
    refresh_economy_input_generation();
    // A section copy is rebuilt only when its section changed since the last publication; otherwise the previous
    // immutable copy is shared. Verification recomputes every reused copy and refuses a difference.
    auto& rows = published_sections.rows;
    auto& keys = published_sections.keys;
    const auto generation = [&](StateSection section) { return section_generation[static_cast<std::size_t>(section)]; };
    const auto share = [&](auto& slot, std::uint64_t& key, const std::uint64_t now, auto&& build) {
        if (!slot || key != now) {
            using Value = std::remove_cvref_t<decltype(build())>;
            slot = std::make_shared<const Value>(build());
            key = now;
        } else if (verify_cached_hash && *slot != build())
            throw std::runtime_error("galactic published section differs from the live state");
    };
    // Fleet rows are hashed by three sections (in-transit motion marks only the movement one).
    share(rows.fleets, keys[0], generation(StateSection::fleets) + generation(StateSection::movement)
        + generation(StateSection::fleet_control), [&] { return sorted_fleets(); });
    const auto& live_fleets = *rows.fleets;
    part.emplace(gc_profile::Phase::snap_observer);
    if (observer && observer_projection_generation != observer_input_generation) {
        observer_projection = std::make_shared<const std::vector<ObservedPlanet>>(
            observed_planets(*observer, live_fleets, view, movement_inputs.get(), actor_inputs()));
        observer_projection_generation = observer_input_generation;
        observer_projection_records += observer->records.size();
    }
    part.emplace(gc_profile::Phase::snap_menus);
    auto published = clock;
    // WGEP-140/141: rebuild derived copied menus only when producer inputs change.
    // Reuse the same immutable vector across idle ticks and render frames.
    math::Fixed local_credits;
    std::uint64_t local_tech{};
    if (observer && economy) for (const auto& account : economy->players)
        if (account.player == view_player) { local_credits = account.credits; local_tech = static_cast<std::uint64_t>(account.current_tech); break; }
    const std::array<std::uint64_t, 5> market_key{ability_host ? ability_input_generation : economy_input_generation->accounts,
        observer_input_generation, observer ? view_player : 0,
        static_cast<std::uint64_t>(local_credits.raw()), local_tech};
    if (!published_black_market_menus || black_market_publication_key != market_key) {
        if (ability_host) ++ability_work.publications;
        // WGCH-152: copy local live service gates on producer invalidation, never per render frame.
        // This presentation column has no canonical/saved-state consumer.
        std::vector<AbilityActorState> services;
        if (ability_host && observer) for (const auto& service : ability_host->actors) {
            const auto actor = actor_records.find(service.actor);
            if (actor != actor_records.end() && actor->second.owner == view_player
                && !actor->second.dead && !actor->second.delete_pending) services.push_back(service);
        }
        published_ability_services = std::make_shared<const std::vector<AbilityActorState>>(std::move(services));
        auto menus =
            economy_corruption && economy && observer && observer_projection
                ? observed_black_market_menus(*economy_corruption, *economy, view_player, *observer_projection)
                : std::vector<ObservedBlackMarketMenu>{};
        if (ability_host) for (auto& menu : menus) {
            const auto at = std::lower_bound(ability_host->preparations.begin(), ability_host->preparations.end(), menu.buyer,
                [](const auto& row, auto id) { return row.actor < id; });
            if (at != ability_host->preparations.end() && at->actor == menu.buyer) menu.preparation = at->id;
        }
        published_black_market_menus = std::make_shared<const std::vector<ObservedBlackMarketMenu>>(std::move(menus));
        std::vector<AbilityResult> results;
        if (ability_host && observer && observer_projection) for (const auto& result : ability_host->results) {
            if (result.player == view_player && std::any_of(observer_projection->begin(), observer_projection->end(),
                [&](const auto& row) { return row.player == result.player && row.planet == result.planet && row.discovered; }))
                results.push_back(result); // coarse outcome only: no hidden refusal cause
        }
        published_ability_results = std::make_shared<const std::vector<AbilityResult>>(std::move(results));
        published_hero_action_menus.reset();
        published_corruption_menu = publish_corruption_menu(view_player, observer_projection
            ? std::span<const ObservedPlanet>(*observer_projection) : std::span<const ObservedPlanet>{});
        if (ability_host && observer && observer_projection) for (const auto& menu : ability_host->preparations) {
            if (!menu.open || (menu.kind != AbilityKind::technology_theft && menu.kind != AbilityKind::superweapon_hack
                && menu.kind != AbilityKind::planet_destruction && menu.kind != AbilityKind::structure_sabotage
                && menu.kind != AbilityKind::hero_neutralization)) continue;
            const auto actor = actor_records.find(menu.actor);
            if (actor == actor_records.end() || actor->second.owner != view_player || actor->second.dead || actor->second.delete_pending) continue;
            if (std::none_of(observer_projection->begin(), observer_projection->end(), [&](const auto& row) {
                return row.player == view_player && row.planet == menu.planet && row.discovered;
            })) continue;
            HeroActionMenus value; value.player = view_player; value.actor = menu.actor;
            value.planet = menu.planet; value.preparation = menu.id;
            if (menu.kind == AbilityKind::technology_theft) value.theft.emplace();
            if (economy) for (const auto& account : economy->players) if (account.player == value.player
                && account.current_tech >= 0 && account.current_tech <= std::numeric_limits<unsigned>::max())
                value.tech = static_cast<unsigned>(account.current_tech);
            if (menu.kind == AbilityKind::planet_destruction) {
                // WGCH-151/155: the supplied source and planet; refusal is an execution fact, never a preview gate.
                if (menu.actor > std::numeric_limits<hero_actions::Id>::max()) continue; // 32-bit display payload (named seam)
                const auto source = static_cast<hero_actions::Id>(menu.actor);
                value.destruction = DestructionPreparation{source, menu.planet, hero_actions::Refusal::none, source,
                    prepared_ability(menu, 0)};
            } else if (menu.kind == AbilityKind::superweapon_hack) {
                // WGCH-66/112: the weapon identity is display-only; affordability compares the local credits.
                const auto weapon = find_hack_weapon(menu.planet); const auto prepared = prepared_ability(menu, 0);
                if (!weapon || !setup.ability_host) continue;
                // The display payload is 32-bit; an identity that does not fit is never published (named seam).
                constexpr std::uint64_t limit = std::numeric_limits<hero_actions::Id>::max();
                if (menu.actor > limit || menu.planet > limit || weapon->id > limit || weapon->type > limit
                    || setup.ability_host->hack_required_type > limit) continue;
                value.hack = ObservedHackChoice{{static_cast<hero_actions::Id>(menu.actor), static_cast<hero_actions::Id>(menu.planet),
                    static_cast<hero_actions::Id>(setup.ability_host->hack_required_type), prepared.price},
                    static_cast<hero_actions::Id>(weapon->id), static_cast<hero_actions::Id>(weapon->type),
                    local_credits >= prepared.price, prepared};
            } else if (menu.kind == AbilityKind::structure_sabotage) {
                // WGCH-97: the structure type is display data; the identity and price travel in the typed capability.
                value.sabotage.emplace();
                const PlanetProduction* row = nullptr;
                if (production) for (const auto& r : production->planets) if (r.planet == menu.planet) row = &r;
                for (std::uint32_t i = 0; i < menu.target_choices.size(); ++i) {
                    const auto prepared = prepared_ability(menu, i);
                    TypeId type{};
                    if (row) for (const auto& object : row->objects) if (object.id == prepared.target) type = object.type;
                    value.sabotage->push_back({i, type, prepared.price, local_credits >= prepared.price, prepared});
                }
            } else if (menu.kind == AbilityKind::hero_neutralization) {
                value.neutralization.emplace();
                for (std::uint32_t i = 0; i < menu.target_choices.size(); ++i) {
                    const auto prepared = prepared_ability(menu, i);
                    const auto victim = actor_records.find(prepared.target);
                    if (victim == actor_records.end()) continue;
                    // The 32-bit display payload; an identity that does not fit is never published (named seam).
                    constexpr std::uint64_t limit = std::numeric_limits<hero_actions::Id>::max();
                    if (menu.actor > limit || menu.planet > limit || prepared.target > limit || prepared.company > limit
                        || victim->second.current_type > limit || victim->second.original_type > limit) continue;
                    ObservedNeutralizationChoice row;
                    row.prepared_index = i;
                    row.prepared = {static_cast<hero_actions::Id>(menu.actor), static_cast<hero_actions::Id>(menu.planet),
                        static_cast<hero_actions::Id>(prepared.target), static_cast<hero_actions::Id>(prepared.company),
                        false, prepared.valid, prepared.price, prepared.absence, prepared.target_absence};
                    row.current_type = static_cast<hero_actions::Id>(victim->second.current_type);
                    row.original_type = static_cast<hero_actions::Id>(victim->second.original_type);
                    row.source_type = row.current_type;
                    row.named = false;
                    if (setup.ability_host) for (const auto& profile : setup.ability_host->profiles)
                        if (profile.type == victim->second.original_type) row.named = profile.named;
                    row.affordable = local_credits >= prepared.price;
                    row.live_prepared = prepared;
                    value.neutralization->push_back(std::move(row));
                }
            } else for (std::uint32_t i = 0; i < menu.theft_choices.size(); ++i) {
                auto prepared = prepared_ability(menu, i);
                value.theft->push_back({i, {}, local_credits >= prepared.price, prepared});
            }
            published_hero_action_menus = std::make_shared<const HeroActionMenus>(std::move(value));
            break; // ordinary modal arbitration follows retained actor identity order
        }
        black_market_publication_key = market_key;
    }
    published.session_tick = completed_tick;
    part.emplace(gc_profile::Phase::snap_economy_input);
    share(rows.planets, keys[1], generation(StateSection::planets), [&] { return sorted_planets(); });
    const auto& current_planets = *rows.planets;
    if (published_economy_input_generation != economy_input_generation) {
        auto input = *economy_input_generation; input.income.reset();
        if (setup.fiscal) {
            InlineExecutor startup_executor;
            const auto& workers = executor ? *executor : static_cast<const PartitionExecutor&>(startup_executor);
            if (auto values = forecast_contributions(workers, current_planets); values) {
                input.income.emplace();
                for (std::size_t i = 0; i < values.value().size(); ++i)
                    input.income->emplace_back(setup.fiscal->planets[i].planet, values.value()[i].total);
            }
        }
        economy_input_generation = std::make_shared<const EconomyInputGeneration>(std::move(input));
        published_economy_input_generation = economy_input_generation;
    }
    part.emplace(gc_profile::Phase::snap_forecast);
    std::optional<EconomyForecast> forecast;
    if (setup.fiscal && observer) {
        InlineExecutor startup_executor;
        const auto& workers = executor ? *executor : static_cast<const PartitionExecutor&>(startup_executor);
        const auto& fiscal = *setup.fiscal;
        const auto player = std::lower_bound(fiscal.players.begin(), fiscal.players.end(), view_player,
            [](const auto& row, PlayerId id) { return row.player < id; });
        auto contributions = forecast_contributions(workers, current_planets); // cached pure fiscal provider
        if (player != fiscal.players.end() && player->player == view_player && contributions) {
            EconomyForecast value;
            value.player = player->player;
            if (fiscal.contributions) { value.trade_routes = math::Fixed{}; value.alignment = math::Fixed{}; }
            if (fiscal.contributions && control) value.political = math::Fixed{};
            bool complete = true;
            for (std::size_t i = 0; complete && i < fiscal.planets.size(); ++i) {
                const auto& row = contributions.value()[i];
                value.planets.push_back({fiscal.planets[i].planet, planet_base_credits(i), row.total, {}, {}});
                value.planets.back().trade = fiscal.contributions ? std::optional(row.trade) : std::nullopt;
                if (!player->earns_planet_income || current_planets[i].owner != player->player) continue;
                complete = math::try_add(value.base, row.base, value.base)
                    && math::try_add(value.bonus, row.bonus, value.bonus)
                    && math::try_add(value.gross_total, row.total, value.gross_total);
                if (value.trade_routes) complete = complete && math::try_add(*value.trade_routes, row.trade, *value.trade_routes);
                if (value.alignment) complete = complete && math::try_add(*value.alignment, row.alignment, *value.alignment);
                if (value.political) complete = complete && math::try_add(*value.political, row.political, *value.political);
            }
            if (complete && economy_corruption) {
                auto copied = *economy_corruption; // pure preview, never consumes piracy
                std::vector<math::Fixed> inputs;
                for (const auto& row : contributions.value()) inputs.push_back(row.total);
                auto preview = corruption_payments(copied, current_planets, fiscal.players, inputs,
                    clock.galactic_frame, workers, false);
                if (!preview) complete = false;
                else {
                    const auto gross = preview.value()[static_cast<std::size_t>(player - fiscal.players.begin())];
                    auto extra = math::subtract(gross, value.gross_total);
                    if (!extra) complete = false;
                    else { value.corruption = extra.value(); value.gross_total = gross; }
                }
            }
            if (complete) {
                // GREF-13: retain each admitted source before presentation rounding.
                // This read-only publication neither samples income nor changes its cadence.
                publish_income_modifiers(value, current_planets);
                if (economy) for (const auto& account : economy->players) {
                    if (account.player != value.player) continue;
                    auto adjusted = adjusted_credits(account.credits, value.gross_total,
                        player->positive_credit_multiplier, account.credit_cap);
                    if (adjusted) {
                        auto delta = math::subtract(adjusted.value(), account.credits);
                        if (delta) value.adjusted_payment = delta.value();
                    }
                }
                forecast = std::move(value);
            }
        }
    }
    // WGP-19/47: copy committed state only after observer admission. Timers are
    // retained values, so idle ticks share storage while the reader computes age.
    part.emplace(gc_profile::Phase::snap_corruption);
    const std::array<std::uint64_t, 3> corruption_key{ability_input_generation,
        observer_input_generation, observer ? view_player : 0};
    if (!published_corruption_transitions || corruption_publication_key != corruption_key) {
        std::vector<LiveCorruptionTransition> transitions;
        if (ability_host && observer && observer_projection) {
            std::set<PlanetId> discovered;
            for (const auto& seen : *observer_projection)
                if (seen.player == view_player && seen.discovered) discovered.insert(seen.planet);
            for (const auto& row : ability_host->transitions) {
                if (discovered.contains(row.planet)) transitions.push_back(row);
            }
        }
        published_corruption_transitions = std::make_shared<const std::vector<LiveCorruptionTransition>>(std::move(transitions));
        corruption_publication_key = corruption_key;
    }
    // Offer admission consumes this publication's current projection; it must
    // not bypass the generation cache with a second full observer projection.
    part.emplace(gc_profile::Phase::snap_offers);
    const auto observations_for_offers = observer_projection
        ? std::span<const ObservedPlanet>(*observer_projection) : std::span<const ObservedPlanet>{};
    // The key contains the exact admission inputs, excluding queue progress and other players' bills.
    // Hash verification independently recomputes every reused result.
    InlineExecutor offer_inline_executor;
    const auto& offer_workers = executor ? *executor : static_cast<const PartitionExecutor&>(offer_inline_executor);
    auto offer_inputs = production_offer_inputs(observations_for_offers, live_fleets, current_planets, view_player);
    if (!published_production_offers || published_offer_inputs != offer_inputs) {
        published_production_offers = std::make_shared<const std::vector<ObservedProductionOffer>>(
            observed_production_offers(offer_inputs, live_fleets, offer_workers, view_player));
        published_offer_inputs = std::move(offer_inputs);
        ++production_offer_rebuilds;
    } else if (verify_cached_hash && *published_production_offers != observed_production_offers(offer_inputs, live_fleets, offer_workers, view_player)) {
        throw std::runtime_error("galactic cached production offers differ from an independent recomputation");
    }
    const auto offers = published_production_offers;
    (void)actor_inputs();
    if (!published_production_originals) {
        auto originals = production_originals;
        std::sort(originals.begin(), originals.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        published_production_originals = std::make_shared<const std::vector<CensusObject>>(std::move(originals));
    }
    if (setup.census && !published_sale_types)
        published_sale_types = std::make_shared<const std::vector<CensusType>>(setup.census->types);
    part.emplace(gc_profile::Phase::snap_construct);
    if (!published_production || published_production_generation != production_generation) {
        auto staged = std::make_shared<const std::optional<ProductionSetup>>(
            copy_production_snapshot(production, offer_workers));
        if (verify_cached_hash && *staged != production)
            throw std::runtime_error("galactic staged production differs from the live production state");
        published_production = std::move(staged);
        published_production_generation = production_generation;
    } else if (verify_cached_hash && *published_production != production) {
        throw std::runtime_error("galactic published production differs from the live production state");
    }
    // The setup's player table never changes after creation.
    share(rows.players, keys[2], 1, [&] { return setup.players; });
    share(rows.routes, keys[3], generation(StateSection::routes), [&] { return sorted_routes(); });
    share(rows.fleet_control, keys[4], generation(StateSection::fleet_control),
        [&] { return setup.fleet_control ? std::optional(fleet_control_state()) : std::nullopt; });
    share(rows.player_economy, keys[5], generation(StateSection::players), [&] { return economy; });
    share(rows.control, keys[6], generation(StateSection::control), [&] { return control; });
    share(rows.observer, keys[7], generation(StateSection::observer), [&] { return observer; });
    share(rows.income, keys[8], generation(StateSection::income), [&] { return income; });
    share(rows.victory, keys[9], generation(StateSection::victory), [&] { return victory; });
    share(rows.economy_corruption, keys[10], generation(StateSection::economy_corruption), [&] { return economy_corruption; });
    auto projected_rows = rows;
    projected_rows.ability_services = published_ability_services;
    if (observer && std::binary_search(observer->human_observers.begin(), observer->human_observers.end(), view_player)) {
        const std::array key{generation(StateSection::history), generation(StateSection::observer)};
        if (!cache.history || cache.history_key != key) {
            cache.history = std::make_shared<const HistorySnapshot>(publish_history(history_archive,
                observer->history_players, planets.size(), setup.rules.fiscal_cycle_frames, view_player, multiplayer_flag()));
            cache.history_key = key;
        }
        projected_rows.history = cache.history;
    }
    return std::make_shared<const GalacticSnapshot>(
        completed_tick, published, std::vector<GalacticPlayer>{}, std::vector<Planet>{}, std::vector<Route>{}, std::vector<Fleet>{}, std::move(events), setup.rules.maximum_fleet_movement_distance, std::nullopt, movement_inputs, seat_control, seats, std::nullopt, std::nullopt, std::nullopt, control_effects, population, std::nullopt, std::nullopt, std::nullopt, std::vector<ObservedProductionOffer>{}, observer_projection, std::move(forecast), published_production_originals, std::nullopt, std::nullopt, std::nullopt, planet_access, observer_input_generation, economy_input_generation, published_actor_records, published_black_market_menus, published_ability_results, published_hero_action_menus, published_corruption_menu, published_sale_types, published_corruption_transitions, offers, published_production, projected_rows, view_player);
}

GalacticSession::GalacticSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GalacticSession::GalacticSession(GalacticSession&&) noexcept = default;
GalacticSession& GalacticSession::operator=(GalacticSession&&) noexcept = default;
GalacticSession::~GalacticSession() = default;

core::Result<GalacticSession> GalacticSession::create(const GalacticSetup& setup) {
    if (auto valid = validate_setup(setup); !valid) return core::Result<GalacticSession>::failure(valid.error());
    auto impl = std::make_unique<Impl>();
    impl->verify_cached_hash = hash_verification_requested();
    impl->setup = setup;
    impl->native_population_schema = session_detail::native_population_schema(setup);
    if (setup.income) impl->native_income_schema = std::make_shared<const IncomeSetup>(*setup.income);
    if (setup.fiscal) impl->native_fiscal_schema = std::make_shared<const FiscalSetup>(*setup.fiscal);
    if (setup.census) for (const auto& row : setup.census->objects) impl->actor_records.emplace(row.id, row);
    impl->economy = setup.player_economy;
    for (const auto& player : setup.players) impl->history_archive.emplace(player.player_id, std::vector<HistorySample>{});
    impl->victory = setup.victory;
    // WGS-24: initialize the sparse leader projection once, in active object order.
    if (setup.census) for (const auto& row : setup.census->objects) impl->update_victory_actor(row);
    impl->control = setup.control;
    if (impl->control) for (const auto& [id, row] : impl->actor_records) {
        (void)id; impl->refresh_control_actor(row);
    }
    if (impl->control && impl->control->state.empty())
        for (const auto& planet : setup.planets) {
            PlanetControl row; row.planet = planet.id; row.current_player = planet.owner;
            impl->control->state.push_back(row);
        }
    impl->production = setup.production;
    impl->observer = setup.observer;
    if (setup.observer) {
        auto access = std::make_shared<PlanetAccessState>();
        for (const auto& planet : setup.planets) access->planets.push_back({planet.id, false, {}});
        impl->planet_access = std::move(access);
    }
    if (setup.income) impl->income = IncomeState{*setup.income, {}, 0, {}};
    impl->economy_corruption = setup.economy_corruption;
    if (setup.ability_host) {
        impl->ability_host.emplace();
        for (const auto& p : setup.ability_host->corruption_planets) {
            const auto at = std::lower_bound(impl->economy_corruption->planets.begin(), impl->economy_corruption->planets.end(), p.planet,
                [](const auto& row, auto id) { return row.planet < id; });
            const auto current = at != impl->economy_corruption->planets.end() && at->planet == p.planet ? at->corruption : corruption_fixture::Type::none;
            impl->ability_host->transitions.push_back({p.planet, {current, {}, false, false, 0, 0, 0}});
        }
        for (const auto& p : setup.ability_host->corruption_players) impl->ability_host->corruption_usage.push_back({p.player, {}});
        for (const auto& [id, row] : impl->actor_records) { (void)id; impl->refresh_ability_actor(row); }
    }
    impl->rebuild_technology_price_expiry();
    impl->expire_technology_prices();
    if (setup.movement) impl->movement_inputs = std::make_shared<const MovementSetup>(*setup.movement);
    impl->seats = setup.seats;
    if (setup.seat_control) impl->seat_control = SeatControlState{*setup.seat_control, {}};
    if (setup.rules.maximum_fleet_movement_distance) {
        auto graph = PlanetGraph::build(setup.planets, setup.routes, *setup.rules.maximum_fleet_movement_distance);
        if (!graph) return core::Result<GalacticSession>::failure(graph.error());
        impl->graph = std::move(graph).value();
    }
    // WGEP-59: opt-in fiscal windows begin at frame zero, cycle one.
    // Legacy clock-only fixtures retain their historical encoding.
    if (setup.fiscal) impl->clock.fiscal_cycle = 1;
    impl->clock.fiscal_end = setup.rules.fiscal_cycle_frames;
    EntityId highest = 0;
    for (const auto& planet : setup.planets) {
        impl->insert(planet);
        highest = std::max(highest, planet.id);
    }
    for (const auto& route : setup.routes) {
        impl->insert(route);
        highest = std::max(highest, route.id);
    }
    for (const auto& fleet : setup.fleets) {
        impl->insert(fleet);
        highest = std::max(highest, fleet.id);
        for (const auto& member : fleet.members) highest = std::max(highest, member.id);
    }
    impl->live_object_count = setup.planets.size() + setup.routes.size() + setup.fleets.size();
    for (const auto& fleet : setup.fleets) impl->live_object_count += fleet.members.size();
    if (setup.fleet_control) for (const auto& unit : setup.fleet_control->detached_units) {
        impl->detached_units.emplace(unit.member.id, unit);
        highest = std::max(highest, unit.member.id);
        ++impl->live_object_count;
    }
    if (setup.production) for (const auto& planet : setup.production->planets) for (const auto& object : planet.objects) {
        highest = std::max(highest, object.id);
        ++impl->live_object_count;
    }
    if (setup.census) {
        std::set<EntityId> existing;
        for (const auto& object : impl->sorted_objects()) existing.insert(object.first);
        for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members) existing.insert(member.id);
        for (const auto& unit : setup.fleet_control->detached_units) existing.insert(unit.member.id);
        for (const auto& planet : setup.production->planets) for (const auto& object : planet.objects) existing.insert(object.id);
        if (setup.movement) for (const auto& row : setup.movement->contents) for (const auto& child : row.children) existing.insert(child.id);
        for (const auto& object : setup.census->objects) {
            highest = std::max(highest, object.id);
            if (existing.insert(object.id).second) ++impl->live_object_count;
        }
    }
    if (setup.movement) for (const auto& c : setup.movement->contents) for (const auto& child : c.children) {
        highest = std::max(highest, child.id); ++impl->live_object_count;
    }
    if (setup.control) for (const auto& planet : setup.control->planets) for (const auto& force : planet.landed) {
        highest = std::max(highest, force.id); ++impl->live_object_count;
    }
    impl->next_id = highest + 1;
    if (auto controlled = impl->initialize_control(); !controlled)
        return core::Result<GalacticSession>::failure(controlled.error());
    for (const auto& planet : impl->sorted_planets()) ++impl->owned_planet_counts[planet.owner];
    if (impl->control) impl->refresh_credit_caps();
    if (impl->control && impl->production) {
        std::vector<GalacticEvent> ignored_events;
        for (std::size_t i = 0; i < impl->control->planets.size(); ++i)
            impl->update_production_control(i, 0, ignored_events, false);
    }
    InlineExecutor inline_executor;
    if (auto initialized = impl->initialize_corruption(); !initialized) return core::Result<GalacticSession>::failure(initialized.error());
    if (impl->income) {
        // WGEP-166/167: object initialization, not a periodic ability service.
        for (const auto& source : impl->income->inputs.sources) if (source.planet_source)
            if (auto initialized = impl->refresh_planet_technology_price(source.planet); !initialized)
                return core::Result<GalacticSession>::failure(initialized.error());
        std::vector<GalacticEvent> ignored;
        if (auto initialized = impl->service_income(inline_executor, 0, ignored, false); !initialized)
            return core::Result<GalacticSession>::failure(initialized.error());
    }
    if (auto refreshed = impl->refresh_population(inline_executor); !refreshed) return core::Result<GalacticSession>::failure(refreshed.error());
    impl->dirty.fill(true);
    impl->initialize_observer();
    impl->snapshot = impl->make_snapshot({});
    const auto replay_fleet = [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) -> core::Result<void> {
        sim::detail::Reader input(effect.payload);
        PlayerId player{}; EntityId actor{}, target{}, expected_object{};
        std::uint8_t pending{}, admitted{}; std::uint16_t opcode{}, reason{}; std::uint32_t length{};
        if (!input.read_u32(player) || !input.read_u64(actor) || !input.read_u64(target)
            || !input.read_u8(pending) || pending > 1 || !input.read_u16(opcode)
            || !input.read_u32(length) || length > galactic_replay_max_payload || length > input.remaining())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid native fleet journal framing");
        std::vector<std::uint8_t> body(length);
        if (!input.read_bytes(body) || !input.read_u8(admitted) || admitted > 1 || !input.read_u64(expected_object)
            || !input.read_u16(reason) || input.remaining())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid native fleet journal receipt");
        core::Result<NativeFleetReceipt> result = core::Result<NativeFleetReceipt>::success({});
        if (effect.kind == NativeEffectKind::fleet) {
            if (actor || target) return fail<void>(diagnostic_codes::invalid_setup, "native fleet journal has extraneous identities");
            auto payload = decode_payload(CommandLayer::galactic, opcode, body);
            if (!payload || encode_payload(*payload) != body)
                return fail<void>(diagnostic_codes::invalid_setup, "invalid native fleet journal command");
            result = world.admit_native_fleet(player, *payload, pending != 0);
        } else {
            if (opcode || !body.empty()) return fail<void>(diagnostic_codes::invalid_setup, "native containment journal has extraneous command bytes");
            if (effect.kind == NativeEffectKind::launch) result = world.launch_native_transport(player, actor, target, pending != 0);
            else if (effect.kind == NativeEffectKind::land && !target) result = world.land_native_transports(player, actor, pending != 0);
            else if (effect.kind == NativeEffectKind::actor_land && target) {
                const auto landed = world.land_native_transport(player, actor, target, pending != 0);
                if (!landed) return core::Result<void>::failure(landed.error());
                result = core::Result<NativeFleetReceipt>::success(landed.value().event);
            }
            else if (effect.kind == NativeEffectKind::orbit_recheck && !target && !pending) {
                auto checked = world.recheck_native_orbit(player, actor);
                if (!checked) return checked;
                result = core::Result<NativeFleetReceipt>::success({true, actor});
            } else return fail<void>(diagnostic_codes::invalid_setup, "invalid native fleet journal owner");
        }
        if (!result) return core::Result<void>::failure(result.error());
        if (result.value().admitted != (admitted != 0) || result.value().object != expected_object
            || static_cast<std::uint16_t>(result.value().reason) != reason)
            return fail<void>(diagnostic_codes::invalid_setup, "native fleet reconstruction differs from its recorded receipt");
        return core::Result<void>::success();
    };
    for (const auto kind : {NativeEffectKind::fleet, NativeEffectKind::launch, NativeEffectKind::land, NativeEffectKind::orbit_recheck, NativeEffectKind::actor_land})
        impl->native_replayers.emplace(kind, replay_fleet);
    impl->native_replayers.emplace(NativeEffectKind::invasion, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        return world.impl_->replay_native_invasion(world, effect);
    });
    impl->native_replayers.emplace(NativeEffectKind::ability_lifecycle, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        sim::detail::Reader in(effect.payload); EntityId actor{}; std::uint8_t operation{};
        if (!in.read_u8(operation) || operation > 2 || !in.read_u64(actor) || !actor)
            return fail<void>(diagnostic_codes::invalid_setup, "invalid ability lifecycle journal");
        if (operation == 2) {
            std::uint8_t enabled{};
            if (!in.read_u8(enabled) || enabled > 1 || in.remaining())
                return fail<void>(diagnostic_codes::invalid_setup, "invalid contested-space journal");
            return world.set_destruction_contested_check(actor, enabled != 0);
        }
        if (operation == 1) {
            TypeId type{}, previous{};
            const auto before = world.impl_->actor_records.find(actor);
            if (!in.read_u64(type) || !type || !in.read_u64(previous) || in.remaining()
                || before == world.impl_->actor_records.end() || before->second.current_type != previous
                || !world.impl_->ability_host)
                return fail<void>(diagnostic_codes::invalid_setup, "invalid ability representation journal");
            return world.change_actor_current_type(actor, type);
        }
        std::uint8_t enabled{}, cancelled{}, kind{};
        if (!in.read_u8(enabled) || enabled > 1 || !in.read_u8(cancelled) || cancelled > 1
            || !in.read_u8(kind) || kind < 1 || kind > 8 || in.remaining())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid ability service journal");
        return world.set_ability_lifecycle(actor, enabled != 0, cancelled != 0, static_cast<AbilityKind>(kind));
    });
    impl->native_replayers.emplace(NativeEffectKind::corruption_lifecycle, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        sim::detail::Reader in(effect.payload); std::uint8_t operation{}, type{}; std::uint64_t id{};
        if (!in.read_u8(operation) || operation > 1 || !in.read_u64(id) || !id || !in.read_u8(type))
            return fail<void>(diagnostic_codes::invalid_setup, "invalid corruption lifecycle journal");
        if (operation == 1) {
            if (type < 1 || type > 3 || in.remaining()) return fail<void>(diagnostic_codes::invalid_setup, "invalid corruption mission result journal");
            return world.finish_corruption_mission(id, static_cast<CorruptionMissionState>(type));
        }
        std::int64_t seconds{};
        if (type > 8 || !in.read_i64(seconds) || seconds < 0 || in.remaining()) return fail<void>(diagnostic_codes::invalid_setup, "invalid corruption transition journal");
        auto started = world.begin_corruption(id, static_cast<corruption_fixture::Type>(type), math::Fixed::from_raw(seconds));
        return started ? core::Result<void>::success() : core::Result<void>::failure(started.error());
    });
    impl->native_replayers.emplace(NativeEffectKind::planet_ruins, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        sim::detail::Reader in(effect.payload); std::uint8_t operation{}, destroy{}; std::uint64_t planet{};
        if (!in.read_u8(operation) || operation != 0 || !in.read_u64(planet) || !planet || !in.read_u8(destroy) || destroy > 1 || in.remaining())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid planet ruins journal");
        auto applied = world.request_planet_ruins(planet, destroy != 0);
        if (!applied) return core::Result<void>::failure(applied.error());
        return applied.value() ? core::Result<void>::success()
            : fail<void>(diagnostic_codes::invalid_setup, "planet ruins reconstruction was refused");
    });
    impl->native_replayers.emplace(NativeEffectKind::technology, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        sim::detail::Reader in(effect.payload); TechnologyWrite write;
        std::uint8_t unlock{}, sound{}, mode{}; std::int64_t previous{}, maximum{};
        if (!in.read_u32(write.player) || !in.read_i64(write.target) || !in.read_u8(unlock) || unlock > 1
            || !in.read_u8(sound) || sound > 1 || !in.read_i64(previous) || !in.read_i64(maximum)
            || (in.remaining() && (!in.read_u8(mode) || mode < 1 || mode > 2)) || in.remaining() // story writes carry the maximum mode (WST-41)
            || !world.impl_->economy || !world.impl_->production)
            return fail<void>(diagnostic_codes::invalid_setup, "invalid technology journal");
        write.maximum = static_cast<TechnologyMaximum>(mode);
        const auto& accounts = world.impl_->economy->players;
        const auto account = std::find_if(accounts.begin(), accounts.end(), [&](const auto& row) { return row.player == write.player; });
        if (account == accounts.end() || account->current_tech != previous || account->maximum_tech != maximum)
            return fail<void>(diagnostic_codes::invalid_setup, "technology reconstruction differs from retained account");
        write.process_unlocks = unlock != 0; write.request_sound = sound != 0;
        auto written = world.set_technology(write);
        return written ? core::Result<void>::success() : core::Result<void>::failure(written.error());
    });
    impl->native_replayers.emplace(NativeEffectKind::production_lock, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        sim::detail::Reader in(effect.payload); ProductionLockWrite write; std::uint8_t check{}; std::int64_t previous{};
        if (!in.read_u32(write.player) || !in.read_u64(write.unlock) || !in.read_u64(write.lock) || !in.read_u8(check) || check > 1
            || !in.read_i64(previous) || in.remaining() || !world.impl_->economy)
            return fail<void>(diagnostic_codes::invalid_setup, "invalid production lock journal");
        const auto& accounts = world.impl_->economy->players;
        const auto account = std::find_if(accounts.begin(), accounts.end(), [&](const auto& row) { return row.player == write.player; });
        if (account == accounts.end() || account->current_tech != previous)
            return fail<void>(diagnostic_codes::invalid_setup, "production lock reconstruction differs from retained account");
        write.slicer_check = check != 0;
        auto applied = world.apply_production_lock(write);
        return applied ? core::Result<void>::success() : core::Result<void>::failure(applied.error());
    });
    impl->native_replayers.emplace(NativeEffectKind::tactical_built, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        sim::detail::Reader in(effect.payload); std::uint8_t operation{}; PlanetId planet{}; TacticalBuiltStructure record;
        std::int64_t x{}, y{}, z{}; PlayerId holder{};
        if (!in.read_u8(operation) || (operation != 1 && operation != 2) || !in.read_u64(planet) || !in.read_u64(record.type)
            || !in.read_i64(x) || !in.read_i64(y) || !in.read_i64(z) || !in.read_u32(record.enable_level)
            || !in.read_u32(record.owner) || !in.read_u32(holder) || in.remaining())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid tactical persistence journal");
        record.position = {math::Fixed::from_raw(x), math::Fixed::from_raw(y), math::Fixed::from_raw(z)};
        if (operation == 1) return world.add_tactical_built_structure(planet, record, holder);
        if (record.enable_level || record.owner || holder || !world.snapshot()->control())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid tactical removal journal");
        const auto& planets = world.snapshot()->control()->planets;
        const auto location = std::find_if(planets.begin(), planets.end(), [planet](const auto& p) { return p.planet == planet; });
        if (location == planets.end() || std::none_of(location->built_structures.begin(), location->built_structures.end(),
            [&](const auto& r) { return r.type == record.type && r.position == record.position; }))
            return fail<void>(diagnostic_codes::invalid_setup, "tactical removal reconstruction differs");
        return world.remove_tactical_built_structure(planet, record.type, record.position);
    });
    impl->native_replayers.emplace(NativeEffectKind::technology_price, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        return world.impl_->replay_technology_price(world, effect);
    });
    impl->native_replayers.emplace(NativeEffectKind::income_block, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        return world.impl_->replay_income_blocker(world, effect);
    });
    return core::Result<GalacticSession>::success(GalacticSession(std::move(impl)));
}

core::Result<GalacticSession> GalacticSession::from_replay(const GalacticReplay& replay) {
    if (auto valid = validate_replay(replay); !valid) return core::Result<GalacticSession>::failure(valid.error());
    auto created = create(replay.setup);
    if (!created) return created;
    for (const auto& command : replay.commands) {
        if (auto submitted = created.value().submit(command); !submitted) {
            return core::Result<GalacticSession>::failure(submitted.error());
        }
    }
    if (auto staged = created.value().stage_native_replay(replay.native_effects, replay.final_tick_count); !staged)
        return core::Result<GalacticSession>::failure(staged.error());
    return created;
}

std::vector<std::uint8_t> GalacticSession::canonical_state_bytes() const {
    std::vector<std::uint8_t> bytes(detail::state_magic.begin(), detail::state_magic.end());
    detail::put_u32(bytes, detail::state_encoding_version);
    detail::put_u32(bytes, static_cast<std::uint32_t>(impl_->section_count()));
    for (const auto section : impl_->section_ids()) {
        const auto encoded = impl_->encode_section(static_cast<StateSection>(section));
        detail::put_u16(bytes, static_cast<std::uint16_t>(section));
        detail::put_u16(bytes, detail::section_version);
        detail::put_u64(bytes, encoded.size());
        detail::put_bytes(bytes, encoded);
    }
    return bytes;
}

void GalacticSession::Impl::refresh_economy_input_generation() const {
    // Section copies shared by consecutive snapshots are keyed by these counts: a dirty flag is counted here
    // (every hash and every publication) before the hash consumes it, so no write can escape a later publication.
    // An optional section whose owner is absent keeps its initial dirty bit forever and is never counted.
    const auto present = [&](const std::size_t section) {
        switch (static_cast<StateSection>(section)) {
        case StateSection::fleet_control: return setup.fleet_control.has_value();
        case StateSection::movement: return setup.movement.has_value();
        case StateSection::control: return control.has_value();
        case StateSection::observer: return observer.has_value();
        case StateSection::income: return income.has_value();
        case StateSection::victory: return victory.has_value();
        case StateSection::economy_corruption: return economy_corruption.has_value();
        default: return true;
        }
    };
    for (std::size_t section = 0; section < state_section_count; ++section)
        if (dirty[section] && present(section)) ++section_generation[section];
    const auto changed = [&](StateSection section) { return dirty[static_cast<std::size_t>(section)]; };
    // Inactive optional sections retain their initial dirty bit: only present
    // native owners contribute inputs to this derived publication.
    if (production && changed(StateSection::production)) ++production_generation;
    const bool census = changed(StateSection::planets) || (production && changed(StateSection::production))
        || changed(StateSection::fleets) || (setup.movement && changed(StateSection::movement))
        || (control && changed(StateSection::control)) || (setup.census && changed(StateSection::census))
        || (observer && changed(StateSection::observer));
    const bool accounts = census || changed(StateSection::players)
        || (income && changed(StateSection::income))
        || (economy_corruption && changed(StateSection::economy_corruption))
        || (ability_host && changed(StateSection::ability_host));
    if (!census && !accounts) return;
    auto next = *economy_input_generation;
    if (census) ++next.census;
    if (accounts) ++next.accounts;
    economy_input_generation = std::make_shared<const EconomyInputGeneration>(next);
}

StateHash GalacticSession::Impl::refresh_hash(GalacticTick::Work* work) {
    // Preserve publication invalidation before the ordinary hash consumes flags.
    refresh_economy_input_generation();
    bool changed = !cached_hash_present;
    for (const auto section : section_ids()) {
        if (!dirty[section]) {
            if (work) ++work->sections_reused;
            continue;
        }
        auto bytes = encode_hash_section(static_cast<StateSection>(section));
        if (work && section == static_cast<std::size_t>(StateSection::production)) work->production_hash_bytes += bytes.size();
        digests[section] = hasher ? hasher->hash(std::move(bytes)) : StateHash(core::sha256_hex(bytes));
        // Only the ordinary world step consumes these flags: it also uses
        // them to invalidate population preparation. Barrier reads must leave
        // native writes visible to that next service.
        if (work) dirty[section] = false;
        changed = true;
        if (work) ++work->sections_encoded;
    }
    if (!changed) { verify_hash(); return cached_hash; }
    if (hasher) {
        // Queue the composition after every changed section job. Capture each
        // generation by value; a later native write cannot change this result.
        auto others = digests;
        const auto ids = section_ids();
        cached_hash = hasher->derive(digests[0], [others, ids](const std::string& session_hex) {
            std::array<std::string, state_section_count> hex;
            hex[0] = session_hex;
            for (const auto section : ids) if (section != 0) hex[section] = others[section].get();
            return combine(hex, ids);
        });
    } else {
        std::array<std::string, state_section_count> hex;
        for (const auto section : section_ids()) hex[section] = digests[section].get();
        cached_hash = StateHash(combine(hex, section_ids()));
    }
    cached_hash_present = true;
    verify_hash();
    return cached_hash;
}
void GalacticSession::Impl::verify_hash() const {
    if (!verify_cached_hash) return;
    std::array<std::string, state_section_count> hex;
    for (const auto section : section_ids()) hex[section] = core::sha256_hex(encode_hash_section(static_cast<StateSection>(section), false));
    if (cached_hash.get() != combine(hex, section_ids())) throw std::runtime_error("galactic cached hash differs from independent full state hash");
}
StateHash GalacticSession::cached_state_hash() const { return impl_->refresh_hash(); }
void GalacticSession::set_hash_verification(bool enabled) noexcept { impl_->verify_cached_hash = enabled; }
std::string GalacticSession::state_sha256() const {
    std::array<std::string, state_section_count> hex;
    for (const auto section : impl_->section_ids()) {
        hex[section] = core::sha256_hex(impl_->encode_hash_section(static_cast<StateSection>(section), false));
    }
    return combine(hex, impl_->section_ids());
}

} // namespace eawr::sim::galactic
