#include "restore_validation_internal.hpp"

#include "eawr/sim/galactic/economy_corruption.hpp"
#include "eawr/sim/galactic/observer.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace eawr::sim::galactic::restore_detail {
namespace {

using Problem = std::optional<std::string>;

template <class Rows, class Key> bool strictly_ascending(const Rows& rows, Key key) {
    return std::adjacent_find(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return !(key(a) < key(b)); }) == rows.end();
}
bool listed(const GalacticSetup& setup, PlayerId id) {
    return std::any_of(setup.players.begin(), setup.players.end(), [&](const auto& row) { return row.player_id == id; });
}
bool listed_or_none(const GalacticSetup& setup, PlayerId id) { return id == 0 || listed(setup, id); }
bool known_planet(const GalacticSetup& setup, PlanetId id) {
    const auto at = std::lower_bound(setup.planets.begin(), setup.planets.end(), id,
        [](const auto& row, auto key) { return row.id < key; });
    return at != setup.planets.end() && at->id == id;
}
constexpr std::int64_t player_scalar_bound = std::int64_t{1} << 48;

} // namespace

Problem production_problem(const GalacticSetup& setup, const ProductionSetup& state) {
    const auto& authored = *setup.production;
    // Authored registries never change after setup (no writer exists), so the restored copy must equal the setup's.
    if (state.maximum_human_queue != authored.maximum_human_queue) return "restored production queue limit differs from setup";
    if (state.technology_types != authored.technology_types) return "restored technology registry differs from setup";
    if (state.nested_creation != authored.nested_creation) return "restored nested creation inputs differ from setup";
    if (state.fleet_blockers.has_value() != authored.fleet_blockers.has_value())
        return "restored fleet blocker declarations differ from setup";
    if (state.fleet_blockers && (state.fleet_blockers->size() > galactic_max_objects
        || !strictly_ascending(*state.fleet_blockers, [](const auto& row) { return row.fleet; })))
        return "restored fleet blocker declarations are unordered or exceed the object bound";
    for (std::size_t i = 0; i < state.players.size(); ++i) {
        const auto& row = state.players[i];
        // Seat kind and payment policy are authored at setup; the population-cap flag is derived from them every tick.
        if (row.human != authored.players[i].human || row.pay_as_you_go != authored.players[i].pay_as_you_go
            || (row.human && !row.uses_population_cap) || (!row.human && !row.pay_as_you_go))
            return "restored player payment or population policy differs from setup";
        if (row.population_used.raw() < 0 || row.population_used.raw() > player_scalar_bound
            || !strictly_ascending(row.counts, [](const auto& count) { return count.type; })
            || !strictly_ascending(row.locked, [](auto id) { return id; })
            || !strictly_ascending(row.unlocked, [](auto id) { return id; }))
            return "restored player census or lock table is unordered or out of range";
        for (const auto& count : row.counts)
            if (count.type == 0 || count.owned > galactic_max_objects || count.historical > galactic_max_commands)
                return "restored player census count is out of range";
    }
    std::size_t total_objects{};
    for (std::size_t i = 0; i < state.planets.size(); ++i) {
        const auto& row = state.planets[i];
        const auto& original = authored.planets[i];
        // Offers are filtered once at setup and never rewritten.
        if (row.offers.rows() != original.offers.rows()) return "restored production offers differ from setup";
        if (!listed_or_none(setup, row.target_owner) || !listed_or_none(setup, row.starbase_owner)
            || !listed_or_none(setup, row.ground_occupier) || !listed_or_none(setup, row.orbit_occupier)
            || row.ground_level < 0 || row.ground_level > 5 || row.space_level < 0 || row.space_level > 5
            || row.maximum_space_level < 0 || row.maximum_space_level > 5 || row.ground_slots < 0 || row.space_slots < 0
            || row.landed_transports < 0 || row.maximum_ground_forces < 0)
            return "restored production planet gates are out of range";
        if (!strictly_ascending(row.objects, [](const auto& object) { return object.id; }))
            return "restored production objects are unordered";
        total_objects += row.objects.size();
        if (total_objects > galactic_max_objects) return "restored production objects exceed the object bound";
        for (const auto& object : row.objects)
            if (object.id == 0 || object.type == 0 || !listed_or_none(setup, object.owner)
                || object.paid_cost.raw() < 0 || object.paid_cost.raw() > player_scalar_bound
                || static_cast<unsigned>(object.domain) > 1 || static_cast<unsigned>(object.kind) > 1)
                return "restored production object identity or cost is invalid";
        for (const auto& queue : row.queues) for (const auto& entry : queue)
            if (entry.type == 0 || !listed_or_none(setup, entry.builder) || entry.price.raw() < 0 || entry.full_seconds.raw() < 0)
                return "restored production queue entry is invalid";
    }
    return std::nullopt;
}

Problem income_problem(const GalacticSetup& setup, const IncomeState& state, const std::uint64_t blocker_next_expiry) {
    const auto& authored = *setup.income;
    const auto& inputs = state.inputs;
    // The gather starts from the setup tables, so the authored profiles and offer basis are fixed.
    if (inputs.types != authored.types) return "restored income profiles differ from setup";
    if (inputs.offers != authored.offers) return "restored income offer basis differs from setup";
    if (inputs.sources.size() > galactic_max_objects
        || !strictly_ascending(inputs.sources, [](const auto& row) { return row.id; }))
        return "restored income sources are unordered or exceed the object bound";
    for (const auto& source : inputs.sources) {
        const auto profile = std::lower_bound(inputs.types.begin(), inputs.types.end(), source.type,
            [](const auto& row, auto key) { return row.type < key; });
        if (!source.id || profile == inputs.types.end() || profile->type != source.type
            || !listed_or_none(setup, source.owner))
            return "restored income source has no identity, profile or owner";
    }
    // Gambling rows are looked up by source with a binary search: the order must be strictly ascending.
    if (state.gambling.size() > galactic_max_objects
        || !strictly_ascending(state.gambling, [](const auto& row) { return row.source; }))
        return "restored gambling income rows are unordered or duplicated";
    if (std::any_of(state.gambling.begin(), state.gambling.end(), [](const auto& row) { return !row.source; }))
        return "restored gambling income row has no source identity";
    // Blockers are searched by (planet, source) and one planet may hold several rows.
    if (state.blockers.size() > galactic_max_objects
        || !strictly_ascending(state.blockers, [](const auto& row) { return std::pair{row.planet, row.source}; }))
        return "restored income blockers are unordered or duplicated";
    auto earliest = std::numeric_limits<std::uint64_t>::max();
    for (const auto& row : state.blockers) {
        if (!row.source || !known_planet(setup, row.planet) || row.first_frame > row.first_expired_frame)
            return "restored income blocker has no source, no planet or a reversed frame range";
        earliest = std::min(earliest, row.first_expired_frame);
    }
    if (earliest != blocker_next_expiry) return "restored income blocker expiry differs from its rows";
    return std::nullopt;
}

Problem corruption_problem(const GalacticSetup& setup, const EconomyCorruptionState& state) {
    const auto& authored = *setup.economy_corruption;
    if (state.income_factors != authored.income_factors || state.factor_min != authored.factor_min
        || state.factor_max != authored.factor_max || state.items != authored.items)
        return "restored corruption economy inputs differ from setup";
    if (const auto valid = validate_economy_corruption_tables(setup, state); !valid) return valid.error().message;
    return std::nullopt;
}

Problem observer_problem(const GalacticSetup& setup, const ObserverSetup& o) {
    if (o.records.size() > galactic_max_objects || o.types.size() > galactic_max_objects || !o.logical_fps
        || o.reveal_delay_seconds < math::Fixed{} || !intelligence_expiry(o.reveal_delay_seconds, 0, o.logical_fps))
        return "restored observer ranges are invalid";
    if (o.history_players.size() > galactic_max_players
        || std::any_of(o.history_players.begin(), o.history_players.end(), [&](auto id) { return !listed(setup, id); }))
        return "restored observer History players are not listed";
    if (o.planet_spies.size() > galactic_max_objects) return "restored observer has too many planet spies";
    std::set<std::pair<EntityId, std::uint64_t>> spies;
    for (const auto& spy : o.planet_spies)
        if (!known_planet(setup, spy.planet) || !spy.ability || !spies.emplace(spy.planet, spy.ability).second
            || (spy.mask & ~all_intelligence) || spy.text_identity.size() > 1024
            || static_cast<unsigned>(spy.style) > static_cast<unsigned>(SpyStyle::ground_activated))
            return "restored observer planet spy is invalid";
    if (o.human_observers.empty() || o.human_observers.size() > o.players.size()
        || !strictly_ascending(o.human_observers, [](auto id) { return id; })
        || std::any_of(o.human_observers.begin(), o.human_observers.end(), [&](auto id) { return !listed(setup, id); }))
        return "restored observer human roster is empty, unordered or not listed";
    if (o.solo_reference_human && (o.human_observers.size() != 1 || o.human_observers.front() != o.solo_reference_human))
        return "restored observer solo reference differs from its roster";
    for (const auto& player : o.players)
        if (player.minimum_visible_base_level < 0 || (player.default_intelligence & ~all_intelligence)
            || player.allies.size() > o.players.size()
            || !strictly_ascending(player.allies, [](auto id) { return id; })
            || std::any_of(player.allies.begin(), player.allies.end(), [&](auto id) { return !id || !listed(setup, id); }))
            return "restored observer faction facts are invalid";
    std::set<EntityId> landed;
    for (const auto& planet : o.planets) {
        if (!listed_or_none(setup, planet.allegiance) || !listed_or_none(setup, planet.target_owner)
            || !listed_or_none(setup, planet.current_owner) || !listed_or_none(setup, planet.current_controller)
            || !listed_or_none(setup, planet.space_owner) || !listed_or_none(setup, planet.ground_owner)
            || planet.space_level < 0 || planet.ground_level < 0 || planet.landed_transports.size() > galactic_max_objects)
            return "restored observer planet facts are invalid";
        for (const auto& transport : planet.landed_transports)
            if (transport.id && (!listed(setup, transport.owner) || transport.id == std::numeric_limits<EntityId>::max()
                || !landed.insert(transport.id).second))
                return "restored observer landed transport reference is invalid";
    }
    if (!strictly_ascending(o.types, [](const auto& row) { return row.type; })
        || std::any_of(o.types.begin(), o.types.end(), [](const auto& row) { return row.type == 0; }))
        return "restored observer type rows are unordered";
    // The trait registry is authored once at setup and has no later writer.
    if (setup.observer && o.types != setup.observer->types) return "restored observer trait registry differs from setup";
    std::size_t sources{};
    for (const auto& record : o.records) {
        if ((record.mask & ~all_intelligence) || record.apparent_owner < -1
            || (record.apparent_owner > 0 && (static_cast<std::uint64_t>(record.apparent_owner) > std::numeric_limits<PlayerId>::max()
                || !listed(setup, static_cast<PlayerId>(record.apparent_owner)))))
            return "restored observer record or apparent owner is invalid";
        sources += record.sources.size();
        if (sources > galactic_max_objects) return "restored observer has too many intelligence sources";
        for (const auto& source : record.sources)
            if (!source.source || !source.ability || source.text_identity.size() > 1024 || (source.mask & ~all_intelligence))
                return "restored observer intelligence source is invalid";
    }
    return std::nullopt;
}

} // namespace eawr::sim::galactic::restore_detail
