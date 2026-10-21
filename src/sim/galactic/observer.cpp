#include "eawr/sim/galactic/observer.hpp"

#include "encoding.hpp"
#include "observer_internal.hpp"

#include <limits>
#include <set>
#include <tuple>

namespace eawr::sim::galactic {

PlanetKnowledge planet_knowledge(const IntelligenceContext& c) noexcept {
    const auto has = [&](Intelligence value) { return (c.mask & bit(value)) != 0; };
    const bool corruption = c.corrupted && c.corruption_benefit;
    // WGUI-10: base stealth is independent for the two content gates.
    const bool space = !c.space_stealth || c.space_owner == 0 || c.allied_space_owner;
    const bool ground = !c.ground_stealth || c.ground_owner == 0 || c.allied_ground_owner;
    PlanetKnowledge k;
    k.space_level = corruption || (space && (has(Intelligence::space_level)
        || c.allied_space_owner || c.space_level >= c.minimum_visible_level));
    k.ground_level = corruption || (ground && (has(Intelligence::ground_level)
        || c.allied_ground_owner || c.ground_level >= c.minimum_visible_level));
    k.ground_contents = corruption || (ground && has(Intelligence::ground_contents));
    k.ground_count = corruption || (ground && (has(Intelligence::ground_count) || has(Intelligence::ground_contents)));
    // WGUI-12: the local-ID exception is preserved even for another observer.
    k.ownership = corruption || c.allegiance == c.local_player
        || (!c.space_stealth && !c.ground_stealth && has(Intelligence::ownership))
        || (!c.ground_stealth && c.own_orbit)
        || (k.space_level && c.space_level != 0) || (k.ground_level && c.ground_level != 0)
        || (k.ground_contents && c.landed_transport_count != 0);
    k.income = has(Intelligence::income) || has(Intelligence::income_breakdown);
    k.income_breakdown = has(Intelligence::income_breakdown); // WGUI-13: no corruption bypass.
    k.strongest_ship = space && has(Intelligence::strongest_ship);
    k.superweapon = true; // WGUI-17: deliberately unconditional.
    k.fleet_contents = corruption || (space && has(Intelligence::fleet_contents));
    k.fleet_count = corruption || (space && (has(Intelligence::fleet_count)
        || k.strongest_ship || c.attached_superweapon || k.fleet_contents));
    k.fleet_presence = corruption || (space && (has(Intelligence::fleet_presence) || k.fleet_count));
    k.structures = corruption || (space && ground && has(Intelligence::structures));
    k.political_control = corruption || has(Intelligence::political_control) || has(Intelligence::political_breakdown);
    k.political_breakdown = corruption || has(Intelligence::political_breakdown);
    k.production = corruption || has(Intelligence::production);
    k.minor_stealth = corruption || has(Intelligence::minor_stealth);
    k.major_stealth = corruption || has(Intelligence::major_stealth);
    k.force_sensitive = has(Intelligence::force_sensitive);
    return k;
}

bool object_details_known(const ObjectIntelligenceContext& object, const PlanetKnowledge& k) noexcept {
    if (object.allied) return true;
    if (!object.planetary_system) return false;
    if (object.structure) return k.structures;
    if (object.hero) {
        const bool detection = object.named_hero ? k.major_stealth : k.minor_stealth;
        if (object.stealth_hero) return detection;
        if (detection) return true;
    }
    if (object.container == IntelligenceContainer::orbit) return k.fleet_contents;
    if (object.container == IntelligenceContainer::ground) return k.ground_contents;
    return false;
}

SpyIntelligence validated_spy(SpyIntelligence spy) noexcept {
    const auto zero = math::Fixed{};
    if (spy.style == SpyStyle::galactic_automatic && spy.duration >= zero)
        spy.duration = math::Fixed::from_raw(-math::Fixed::scale);
    if (spy.style == SpyStyle::ground_activated && spy.duration <= zero)
        spy.duration = math::Fixed::from_raw(30 * math::Fixed::scale);
    if (spy.style != SpyStyle::galactic_automatic && spy.style != SpyStyle::ground_activated)
        spy.style = SpyStyle::disabled;
    if (spy.mask & (bit(Intelligence::strongest_ship) | bit(Intelligence::fleet_contents)))
        spy.mask |= bit(Intelligence::fleet_count);
    if (spy.mask & bit(Intelligence::fleet_count)) spy.mask |= bit(Intelligence::fleet_presence);
    if (spy.mask & (bit(Intelligence::ground_level) | bit(Intelligence::space_level)))
        spy.mask |= bit(Intelligence::ground_level) | bit(Intelligence::space_level);
    if (spy.mask & bit(Intelligence::ground_contents)) spy.mask |= bit(Intelligence::ground_count);
    if (spy.mask & bit(Intelligence::political_breakdown)) spy.mask |= bit(Intelligence::political_control);
    if (spy.mask & bit(Intelligence::income_breakdown)) spy.mask |= bit(Intelligence::income);
    return spy;
}

core::Result<std::uint64_t> intelligence_expiry(const math::Fixed seconds, const std::uint64_t frame, const std::uint32_t fps) {
    const auto invalid = [] { return core::Result<std::uint64_t>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "intelligence expiry overflows or has zero logical FPS")); };
    if (!fps) return invalid();
    if (seconds == math::Fixed::from_raw(-math::Fixed::scale))
        return core::Result<std::uint64_t>::success(std::numeric_limits<std::uint64_t>::max());
    if (seconds <= math::Fixed{}) return core::Result<std::uint64_t>::success(0);
    const auto raw = static_cast<std::uint64_t>(seconds.raw());
    constexpr auto scale = static_cast<std::uint64_t>(math::Fixed::scale);
    const auto whole = raw / scale;
    const auto remainder = raw % scale;
    const auto fractional_frames = (remainder * fps + scale / 2) / scale;
    if (whole > (std::numeric_limits<std::uint64_t>::max() - fractional_frames) / fps) return invalid();
    const auto frames = whole * fps + fractional_frames;
    if (frame > std::numeric_limits<std::uint64_t>::max() - frames)
        return invalid();
    return core::Result<std::uint64_t>::success(frame + frames);
}

core::Result<void> initialize_campaign_observer(GalacticSetup& setup, const std::optional<PlanetId> local_home) {
    if (!setup.observer || !setup.observer->solo_reference_human)
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "singleton observer initialization requires a solo reference human"));
    const ObserverHome home{setup.observer->solo_reference_human, local_home};
    return initialize_campaign_observers(setup, std::span<const ObserverHome>(&home, 1));
}
core::Result<void> initialize_campaign_observers(GalacticSetup& setup, const std::span<const ObserverHome> homes) {
    if (!setup.observer || !setup.observer->records.empty())
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "campaign observer requires sourced facts without retained records"));
    auto staged = setup;
    auto& observer = *staged.observer;
    if (observer.players.size() > galactic_max_objects || observer.planets.size() > 4096
        || (!observer.planets.empty() && observer.players.size() > galactic_max_objects / observer.planets.size()))
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "campaign observer record table exceeds limits"));
    // WGUI-99: each player/planet starts undiscovered with zero counter/mask/apparent fields.
    for (const auto& player : observer.players) for (const auto& planet : observer.planets) {
        ObserverRecord record; record.player = player.player; record.planet = planet.planet;
        observer.records.push_back(std::move(record));
    }
    if (auto valid = validate_observer(staged); !valid) return valid;
    for (const auto& planet : staged.planets)
        observer_detail::planet_spy_transition(observer, staged.planets, planet.id, 0, planet.owner, 0,
            staged.fleets, staged.movement ? &*staged.movement : nullptr,
            staged.census ? std::span<const CensusObject>(staged.census->objects) : std::span<const CensusObject>{});
    const auto table = observer_detail::presence(observer, staged.fleets, staged.movement ? &*staged.movement : nullptr,
        staged.census ? std::span<const CensusObject>(staged.census->objects) : std::span<const CensusObject>{});
    if (homes.size() != observer.human_observers.size())
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "observer homes do not match human roster"));
    for (std::size_t index = 0; index < homes.size(); ++index) {
        const auto& home = homes[index];
        if (home.player != observer.human_observers[index])
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "observer homes are not in human roster order"));
        if (!home.planet) continue;
        const auto planet = observer_detail::find(observer.planets, *home.planet, [](const auto& row) { return row.planet; });
        if (!planet) return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "campaign observer home is not a retained planet"));
        const auto player = observer_detail::find(observer.players, home.player, [](const auto& row) { return row.player; });
        auto& record = observer.records[static_cast<std::size_t>(player - observer.players.data()) * observer.planets.size()
            + static_cast<std::size_t>(planet - observer.planets.data())];
        record.discovered = true;
        observer_detail::refresh(record, *player, *planet, table, 0, true);
    }
    if (auto valid = validate_observer(staged); !valid) return valid;
    setup.observer = std::move(observer);
    return core::Result<void>::success();
}

core::Result<void> validate_observer(const GalacticSetup& setup) {
    if (!setup.observer) return core::Result<void>::success();
    const auto& o = *setup.observer;
    const auto invalid = [](std::string message) { return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, std::move(message))); };
    if (setup.planets.size() > 4096 || o.players.size() != setup.players.size() || o.planets.size() != setup.planets.size()
        || o.records.size() != o.players.size() * o.planets.size() || o.records.size() > galactic_max_objects
        || o.types.size() > galactic_max_objects || !o.logical_fps || o.reveal_delay_seconds < math::Fixed{} || o.automatic_reveal_range < math::Fixed{})
        return invalid("observer tables or ranges are invalid");
    const auto listed = [&](PlayerId id) { return observer_detail::find(setup.players, id, [](const auto& p) { return p.player_id; }) != nullptr; };
    if (o.history_players.size() > galactic_max_players) return invalid("too many configured History players");
    for (const auto id : o.history_players) if (!listed(id)) return invalid("configured History player is not listed");
    if (o.planet_spies.size() > galactic_max_objects) return invalid("too many planet spy abilities");
    std::set<std::pair<EntityId, std::uint64_t>> spy_ids;
    for (const auto& spy : o.planet_spies) {
        if (!observer_detail::find(setup.planets, spy.planet, [](const auto& p) { return p.id; })
            || !spy.ability || !spy_ids.emplace(spy.planet, spy.ability).second
            || (spy.mask & ~all_intelligence) || spy.text_identity.size() > 1024
            || static_cast<unsigned>(spy.style) > static_cast<unsigned>(SpyStyle::ground_activated))
            return invalid("invalid planet spy ability");
    }
    if (o.human_observers.empty() || o.human_observers.size() > o.players.size()
        || !std::is_sorted(o.human_observers.begin(), o.human_observers.end())
        || std::adjacent_find(o.human_observers.begin(), o.human_observers.end()) != o.human_observers.end())
        return invalid("observer human roster is empty, unordered or duplicated");
    for (const auto id : o.human_observers) if (!listed(id)) return invalid("observer human is not listed");
    if (o.solo_reference_human && (o.human_observers.size() != 1 || o.human_observers.front() != o.solo_reference_human))
        return invalid("solo reference human does not match singleton roster");
    if (!intelligence_expiry(o.reveal_delay_seconds, 0, o.logical_fps)) return invalid("observer reveal threshold overflows");
    const auto ids = [&](const auto& values, auto get) {
        for (std::size_t i = 0; i < values.size(); ++i) if (get(values[i]) == 0 || (i && get(values[i-1]) >= get(values[i]))) return false;
        return true;
    };
    for (std::size_t i = 0; i < o.players.size(); ++i) {
        const auto& p = o.players[i];
        if (p.player != setup.players[i].player_id || p.minimum_visible_base_level < 0 || (p.default_intelligence & ~all_intelligence)
            || p.allies.size() > o.players.size() || !ids(p.allies, [](auto id) { return id; })) return invalid("invalid observer faction facts");
        for (auto id : p.allies) if (!listed(id)) return invalid("unknown observer ally");
    }
    std::set<EntityId> landed_ids;
    for (std::size_t i = 0; i < o.planets.size(); ++i) {
        const auto& p = o.planets[i];
        if (p.planet != setup.planets[i].id || !listed(p.allegiance) || !listed(p.target_owner)
            || !listed(p.current_owner) || !listed(p.current_controller)
            || (p.space_owner && !listed(p.space_owner)) || (p.ground_owner && !listed(p.ground_owner))
            || p.space_level < 0 || p.ground_level < 0 || p.landed_transports.size() > galactic_max_objects
            || p.political_control.raw() % math::Fixed::scale != 0)
            return invalid("invalid observer planet facts");
        for (const auto& t : p.landed_transports) if (t.id && (!listed(t.owner)
            || t.id == std::numeric_limits<EntityId>::max() || !landed_ids.insert(t.id).second)) return invalid("invalid landed transport reference");
    }
    if (!ids(o.types, [](const auto& t) { return t.type; })) return invalid("observer type rows are unordered");
    const auto known = [&](TypeId id) { return observer_detail::find(o.types, id, [](const auto& t) { return t.type; }) != nullptr; };
    for (const auto& f : setup.fleets) for (const auto& m : f.members) if (!known(m.type)) return invalid("missing observer fleet traits");
    if (setup.movement) for (const auto& c : setup.movement->contents) for (const auto& m : c.children) if (!known(m.type)) return invalid("missing observer live child traits");
    std::size_t source_count{};
    for (std::size_t i = 0; i < o.records.size(); ++i) {
        const auto& r = o.records[i];
        if (r.player != o.players[i / o.planets.size()].player || r.planet != o.planets[i % o.planets.size()].planet
            || (r.mask & ~all_intelligence) || r.apparent_owner < -1 || r.apparent_control.raw() % math::Fixed::scale != 0
            || (r.apparent_owner > 0 && (static_cast<std::uint64_t>(r.apparent_owner) > std::numeric_limits<PlayerId>::max()
                || !listed(static_cast<PlayerId>(r.apparent_owner))))) return invalid("invalid observer record or apparent owner");
        source_count += r.sources.size();
        if (source_count > galactic_max_objects) return invalid("too many intelligence sources");
        for (const auto& s : r.sources) if (!s.source || !s.ability || s.text_identity.size() > 1024 || (s.mask & ~all_intelligence))
            return invalid("invalid intelligence source");
    }
    return core::Result<void>::success();
}

std::vector<ObservedPlanet> observed_planets(const ObserverSetup& inputs, const std::span<const Fleet> fleets,
    const ObserverViewContext view, const MovementSetup* movement, const std::span<const CensusObject> actors) {
    const auto table = observer_detail::presence(inputs, fleets, movement, actors);
    std::vector<ObservedPlanet> result; result.reserve(view.player ? inputs.planets.size() : inputs.records.size());
    for (std::size_t i = 0; i < inputs.records.size(); ++i) {
        const auto& record = inputs.records[i];
        if (view.player && record.player != view.player) continue;
        const auto& player = inputs.players[i / inputs.planets.size()];
        const auto& planet = inputs.planets[i % inputs.planets.size()];
        ObservedPlanet row; row.player = record.player; row.planet = record.planet; row.discovered = record.discovered;
        row.knowledge = planet_knowledge(observer_detail::context(inputs, view, player, planet, record, table));
        if (row.knowledge.ownership) { row.known_owner = planet.current_owner; row.known_allegiance = planet.allegiance; }
        if (row.knowledge.political_control) row.known_political_control = planet.political_control;
        row.apparent_owner = row.known_owner;
        if (!row.apparent_owner && record.apparent_owner > 0) row.apparent_owner = static_cast<PlayerId>(record.apparent_owner);
        const bool own_orbit = table.contains({player.player, planet.planet});
        // WGUI-100: the apparent allegiance getter has a distinct orbit branch.
        if (row.knowledge.political_control || (own_orbit && (!player.canonical_empire || planet.political_control < math::Fixed::from_raw(math::Fixed::scale)))) {
            row.apparent_political_control = planet.political_control;
            row.apparent_allegiance_owner = planet.current_controller;
        } else if (own_orbit) {
            row.apparent_political_control = {};
            row.apparent_allegiance_owner.reset(); // canonical Empire at control >= 1 returns 0/-1
        } else {
            row.apparent_political_control = record.apparent_control;
            if (record.apparent_owner > 0) row.apparent_allegiance_owner = static_cast<PlayerId>(record.apparent_owner);
        }
        result.push_back(row);
    }
    return result;
}

} // namespace eawr::sim::galactic
