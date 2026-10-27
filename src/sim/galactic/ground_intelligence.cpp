#include "eawr/sim/galactic/ground_intelligence.hpp"
#include <algorithm>
#include <map>

namespace eawr::sim::galactic {
std::vector<ObservedGroundDisplay> observed_ground_display(const GalacticSnapshot& snapshot,
    PlayerId local, const std::function<std::optional<GroundDisplayType>(TypeId)>& traits) {
    std::vector<ObservedGroundDisplay> output;
    if (!local || !snapshot.observer() || !snapshot.control()
        || (snapshot.view_player() && snapshot.view_player() != local)) return output;
    const auto& observer = *snapshot.observer();
    if (std::none_of(observer.players.begin(), observer.players.end(),
        [local](const auto& row) { return row.player == local; })) return output;
    std::map<PlayerId, FactionId> factions;
    for (const auto& row : snapshot.players()) factions.emplace(row.player_id, row.faction_id);
    std::map<PlanetId, const ObserverPlanet*> planets;
    for (const auto& row : observer.planets) planets.emplace(row.planet, &row);
    std::map<PlanetId, const ControlPlanet*> control;
    std::map<std::pair<PlanetId, EntityId>, const SurfaceForce*> forces;
    for (const auto& row : snapshot.control()->planets) {
        control.emplace(row.planet, &row);
        for (const auto& force : row.landed) forces.emplace(std::pair{row.planet, force.id}, &force);
    }
    std::map<EntityId, const CensusObject*> actors;
    std::map<EntityId, std::vector<FleetMember>> children;
    for (const auto& row : snapshot.actor_records()) {
        actors.emplace(row.id, &row);
        if (row.container) children[row.container].push_back({row.id, row.current_type});
    }
    // The movement owner retains membership order; actor order is a provider
    // fallback only when that owner has no contents row.
    if (snapshot.movement()) for (const auto& row : snapshot.movement()->contents)
        children[row.container] = row.children;
    const auto live = [&](EntityId id) {
        const auto at = actors.find(id);
        return at != actors.end() && !at->second->dead && !at->second->delete_pending;
    };
    const auto current = [&](const FleetMember& member) {
        const auto at = actors.find(member.id);
        return at == actors.end() ? TypeId{} : at->second->current_type;
    };
    std::map<TypeId, std::optional<GroundDisplayType>> type_cache;
    const auto look = [&](TypeId type) {
        const auto [at, inserted] = type_cache.try_emplace(type);
        if (inserted && traits && type) at->second = traits(type);
        return at->second;
    };
    // GGS-09: first contained named hero in live membership order.
    const auto named = [&](auto&& self, const FleetMember& member, unsigned depth) -> std::optional<TypeId> {
        if (depth > 32 || !live(member.id)) return {};
        const auto type = current(member);
        if (const auto art = look(type); art && art->named_hero) return type;
        if (const auto at = children.find(member.id); at != children.end())
            for (const auto& child : at->second) if (const auto found = self(self, child, depth + 1)) return found;
        return {};
    };
    for (const auto& knowledge : snapshot.observations()) {
        if (knowledge.player != local || !knowledge.discovered
            || (!knowledge.knowledge.ground_count && !knowledge.knowledge.ground_contents)) continue;
        const auto p = planets.find(knowledge.planet);
        const auto c = control.find(knowledge.planet);
        if (p == planets.end() || c == control.end() || !c->second->surface_accessible || c->second->destroyed) continue;
        const auto faction = factions.find(p->second->allegiance);
        if (faction == factions.end()) continue;
        ObservedGroundDisplay display;
        display.planet = knowledge.planet; display.owner = p->second->allegiance;
        std::optional<std::uint32_t> health;
        std::optional<std::int64_t> hero_rank;
        const auto rank_hero = [&](TypeId type, const GroundDisplayType& art) {
            if (!hero_rank || art.hero_rank < *hero_rank) { display.hero = type; hero_rank = art.hero_rank; }
        };
        const auto rank_health = [&](TypeId type, bool hero_tie) {
            const auto art = look(type);
            if (art && art->health && (!health || *art->health > *health || (hero_tie && *art->health == *health))) {
                display.strongest = type; health = art->health;
            }
        };
        for (const auto& transport : p->second->landed_transports) {
            const auto owner = factions.find(transport.owner);
            if (!transport.id || transport.delete_pending
                || owner == factions.end() || owner->second != faction->second) continue;
            const auto root = actors.find(transport.id);
            if (root != actors.end() && !live(transport.id)) continue;
            ++display.count;
            // GGS-09: the observer's size fact survives a missing actor row;
            // artwork does not. Membership alone never proves a live payload.
            if (!live(transport.id) || !root->second->current_type) continue;
            const auto force = forces.find({knowledge.planet, transport.id});
            if (force == forces.end() || force->second->owner != transport.owner) continue;
            FleetMember member{force->second->id, force->second->type};
            const auto& actor = *root->second;
            if (actor.owner != transport.owner) continue;
            TypeId type = actor.original_type;
            if (const auto hero = named(named, member, 0)) {
                type = *hero;
                if (const auto art = look(type)) rank_hero(type, *art);
            } else if (const auto art = look(current(member)); art && art->generic_hero && art->has_icon) {
                type = current(member);
            }
            const auto art = look(type);
            if (art && (art->named_hero || art->generic_hero)) {
                rank_hero(type, *art); rank_health(type, true);
            } else if (actor.combat_role == ActorCombatRole::ordinary) {
                rank_health(current(member), false);
            } else if (actor.combat_role == ActorCombatRole::fleet) {
                rank_health(type, false);
            } else if (const auto payload = children.find(member.id); payload != children.end()) {
                for (const auto& child : payload->second) if (live(child.id)) rank_health(current(child), false);
            }
            // GGS-U02: no candidate without a qualified live representation.
            // In particular, never expand template members or guess company art.
        }
        if (display.count) output.push_back(display);
    }
    return output;
}
} // namespace eawr::sim::galactic
