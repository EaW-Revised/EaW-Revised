#include "eawr/campaign/tactical_return.hpp"
#include "eawr/sim/galactic/control.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace eawr::campaign {
namespace gc = sim::galactic;
namespace {
template<typename T> core::Result<T> fail(std::string message) {
    core::Diagnostic diagnostic; diagnostic.code = std::string(gc::diagnostic_codes::invalid_setup);
    diagnostic.message = std::move(message); return core::Result<T>::failure(std::move(diagnostic));
}
bool destroyed(const ForceResult& row) {
    return row.disposition == ForceDisposition::destroyed || row.disposition == ForceDisposition::destroy_after_tactical;
}
core::Result<gc::PlanetId> destination(const gc::GalacticSnapshot& snapshot, const gc::PlanetId source,
    const gc::PlayerId owner, const bool surface) {
    const auto src = std::find_if(snapshot.planets().begin(), snapshot.planets().end(), [&](const auto& row) { return row.id == source; });
    if (src == snapshot.planets().end()) return fail<gc::PlanetId>("retreat source is absent");
    const auto orbital = [&](const auto id) { return std::any_of(snapshot.fleets().begin(), snapshot.fleets().end(),
        [&](const auto& fleet) { return fleet.orbit == id && fleet.owner == owner && !fleet.motion.transit; }); };
    const auto base = [&](const auto id) { return snapshot.control() && std::any_of(snapshot.control()->planets.begin(), snapshot.control()->planets.end(),
        [&](const auto& row) { return row.planet == id && (row.starbase.owner == owner || row.ground_base.owner == owner); }); };
    const auto neutral = [&](const auto player) { return snapshot.control() && std::any_of(snapshot.control()->players.begin(), snapshot.control()->players.end(),
        [&](const auto& row) { return row.player == player && row.neutral; }); };
    for (unsigned tier = 0; tier < 4; ++tier) {
        std::optional<sim::math::Fixed> nearest; gc::PlanetId selected{}; bool tied{};
        for (const auto& planet : snapshot.planets()) {
            if (planet.id == source || (tier == 0 && !(surface ? base(planet.id) : orbital(planet.id)))
                || (tier == 1 && !(surface ? orbital(planet.id) : base(planet.id)))
                || (tier == 2 && !neutral(planet.owner))) continue;
            const auto x = sim::math::subtract(planet.position.x, src->position.x);
            const auto y = sim::math::subtract(planet.position.y, src->position.y);
            const auto z = sim::math::subtract(planet.position.z, src->position.z);
            if (!x || !y || !z) return fail<gc::PlanetId>("retreat destination distance overflow");
            const auto distance = sim::math::length(sim::math::Vec3{x.value(), y.value(), z.value()});
            if (!distance) return core::Result<gc::PlanetId>::failure(distance.error());
            if (!nearest || distance.value() < *nearest) { nearest = distance.value(); selected = planet.id; tied = false; }
            else if (distance.value() == *nearest) tied = true;
        }
        if (selected) {
            if (tied) return fail<gc::PlanetId>("unqualified equal-distance retreat destinations (WGM-U03)");
            return core::Result<gc::PlanetId>::success(selected);
        }
    }
    return core::Result<gc::PlanetId>::success(source); // WGM-99 orbital fallback
}
void put(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned size = 8) {
    for (unsigned i = 0; i < size; ++i) { bytes.push_back(static_cast<std::uint8_t>(value)); value >>= 8U; }
}
}
bool tactical_force_alive(const ConflictForce& force, const std::span<const sim::tactical::UnitState> units) {
    if (force.role == ForceRole::retained) return true;
    const auto carried = [&](auto&& self, const auto& riders) -> bool {
        for (const auto& rider : riders) if (rider.entity_id == force.id || self(self, rider.members)) return true;
        return false;
    };
    for (const auto& unit : units) if (unit.entity_id == force.id || carried(carried, unit.contained)) return true;
    return false;
}
core::Result<std::vector<MidBattleObject>> collect_midbattle_objects(const ConflictRecord& conflict,
    const std::span<const sim::tactical::UnitState> units, const MidBattleReserveGather& reserves) {
    if (!reserves) return fail<std::vector<MidBattleObject>>("mid-battle solve needs current reserve identities (WAR-65)");
    std::vector<MidBattleObject> result; std::set<sim::EntityId> ids;
    const auto original = [&](const sim::EntityId id) {
        const auto at = std::find_if(conflict.forces.begin(), conflict.forces.end(), [&](const auto& row) { return row.id == id; });
        return at == conflict.forces.end() ? sim::galactic::TypeId{} : at->type;
    };
    const auto add = [&](const sim::tactical::UnitState& unit) -> bool {
        if (!unit.entity_id || !ids.insert(unit.entity_id).second) return false;
        result.push_back({unit, original(unit.entity_id), false}); return true;
    };
    for (const auto& unit : units) {
        if (!add(unit)) return fail<std::vector<MidBattleObject>>("duplicate live tactical identity");
        const auto add_carried = [&](auto&& self, const auto& children) -> bool {
            for (const auto& child : children) {
                sim::tactical::UnitState carried; carried.entity_id = child.entity_id; carried.type_id = child.type_id;
                carried.owner = unit.owner; carried.position = unit.position; carried.contained = child.members;
                if (!add(carried) || !self(self, child.members)) return false;
            }
            return true;
        };
        if (!add_carried(add_carried, unit.contained)) return fail<std::vector<MidBattleObject>>("duplicate carried tactical identity");
    }
    auto current = reserves(); if (!current) return core::Result<std::vector<MidBattleObject>>::failure(current.error());
    for (const auto& row : current.value()) {
        if (!row.id || !row.type || row.role != ForceRole::reinforcement || !ids.insert(row.id).second
            || (row.owner != conflict.invader && row.owner != conflict.defender))
            return fail<std::vector<MidBattleObject>>("invalid current reserve identity");
        sim::tactical::UnitState unit; unit.entity_id = row.id; unit.owner = row.owner;
        unit.type_id = row.tactical_type ? row.tactical_type : row.type; unit.purchase_type = row.purchase_type;
        result.push_back({std::move(unit), row.type, true});
    }
    return core::Result<std::vector<MidBattleObject>>::success(std::move(result));
}
core::Result<TacticalReturnPlan> plan_tactical_return(const ConflictRecord& conflict,
    BattleResult result, const gc::GalacticSnapshot& snapshot) {
    if (result.conflict != conflict.id || result.forces.size() != conflict.forces.size()
        || (result.winner != conflict.invader && result.winner != conflict.defender)
        || (result.retreat_player && *result.retreat_player != conflict.invader && *result.retreat_player != conflict.defender))
        return fail<TacticalReturnPlan>("tactical result does not match its conflict");
    std::map<sim::EntityId, gc::PlayerId> losses;
    for (std::size_t i = 0; i < conflict.forces.size(); ++i) {
        if (result.forces[i].id != conflict.forces[i].id) return fail<TacticalReturnPlan>("tactical return force order differs");
        if (destroyed(result.forces[i])) losses.emplace(result.forces[i].id, result.forces[i].final_blow_player);
    }
    // WGM-55..57/WAR-87: ordinary container death closes over persistent contents.
    // This bounded return commit is ordered; it is not a per-entity tick service.
    if (snapshot.movement()) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& contents : snapshot.movement()->contents) if (losses.contains(contents.container))
                for (const auto& child : contents.children) {
                    const auto resolved = std::find_if(conflict.forces.begin(), conflict.forces.end(), [&](const auto& row) { return row.id == child.id; });
                    if (resolved != conflict.forces.end() && resolved->role == ForceRole::rider
                        && resolved->carrier != contents.container && !losses.contains(resolved->carrier)) continue;
                    changed |= losses.emplace(child.id, losses.at(contents.container)).second;
                }
        }
    }
    for (auto& row : result.forces) if (losses.contains(row.id) && row.disposition != ForceDisposition::destroy_after_tactical) {
        row.disposition = ForceDisposition::destroyed;
        const auto force = std::find_if(conflict.forces.begin(), conflict.forces.end(), [&](const auto& input) { return input.id == row.id; });
        row.hero_defeated = force != conflict.forces.end() && force->hero;
        if (!row.final_blow_player) row.final_blow_player = losses.at(row.id);
    }
    TacticalReturnPlan out; out.result = std::move(result);
    // WGT-115: copies share one parent. Source destruction is committed once;
    // temporary escorts only consume child IDs, never a permanent structure identity.
    std::set<sim::EntityId> parent_losses;
    for (const auto& [id, killer] : losses) {
        (void)killer;
        const auto at = std::find_if(conflict.forces.begin(), conflict.forces.end(), [id](const auto& f) { return f.id == id; });
        parent_losses.insert(at != conflict.forces.end() && at->role == ForceRole::orbital ? at->parent : id);
    }
    out.destroyed.assign(parent_losses.begin(), parent_losses.end());
    {
        // WGP-98: an orbital parent shares its destroyed child's final blow.
        std::map<sim::EntityId, gc::PlayerId> blows;
        for (const auto& [id, killer] : losses) {
            const auto at = std::find_if(conflict.forces.begin(), conflict.forces.end(), [id = id](const auto& f) { return f.id == id; });
            blows.emplace(at != conflict.forces.end() && at->role == ForceRole::orbital ? at->parent : id, killer);
        }
        for (const auto& [id, killer] : blows) out.killers.push_back({id, killer});
    }
    const bool losing_raid = conflict.layer == BattleLayer::land && conflict.raid && out.result.winner != conflict.invader;
    if (losing_raid) out.result.retreat_player = conflict.invader;
    // WGM-201 / WAR-57: a surface loss leaves unrelated space fleets in orbit.
    if (out.result.retreat_player && (conflict.layer == BattleLayer::space || losing_raid))
        for (auto at = snapshot.fleets().rbegin(); at != snapshot.fleets().rend(); ++at)
        if (at->owner == *out.result.retreat_player && at->orbit == conflict.planet && !at->motion.transit)
            out.retreat_fleets.push_back(at->id);
    return core::Result<TacticalReturnPlan>::success(std::move(out));
}
core::Result<void> apply_tactical_return(gc::GalacticSession& galaxy, const ConflictRecord& conflict,
    const TacticalReturnPlan& plan, TacticalReturnState& state, const sim::PartitionExecutor& executor) {
    auto applied = galaxy.apply_battle_losses(plan.destroyed, plan.killers);
    if (!applied) return applied;
    std::vector<std::pair<gc::PlayerId, sim::math::Fixed>> credits;
    for (const auto& row : plan.result.credits) credits.emplace_back(row.player, row.credits);
    if (!credits.empty()) { applied = galaxy.apply_battle_credits(credits); if (!applied) return applied; }
    if (plan.result.retreat_player) {
        if (auto refreshed = galaxy.refresh_movement_access(executor); !refreshed) return refreshed;
        const auto snapshot = galaxy.snapshot();
        const bool surface = conflict.layer == BattleLayer::land;
        const auto selected = destination(*snapshot, conflict.planet, *plan.result.retreat_player, surface);
        if (!selected) return core::Result<void>::failure(selected.error());
        std::vector<gc::FleetId> live;
        for (const auto id : plan.retreat_fleets)
            if (std::any_of(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& fleet) { return fleet.id == id; })) live.push_back(id);
        applied = galaxy.apply_retreat_return(*plan.result.retreat_player, conflict.planet, selected.value(), surface, live);
        if (!applied) return applied;
    }
    if (conflict.tactical.campaign) {
        std::erase_if(state.upgrades, [&](const auto& row) { return row.planet == conflict.planet; });
        for (auto upgrade : plan.result.upgrades) {
            if (upgrade.holder == conflict.station) upgrade.holder = conflict.planet;
            state.upgrades.push_back({conflict.planet, upgrade});
        }
    }
    return core::Result<void>::success();
}
std::vector<InvasionOffer> tactical_invasion_offers(const ConflictRecord& conflict, const BattleResult& result,
    const gc::GalacticSnapshot& snapshot) {
    std::vector<InvasionOffer> out;
    if (conflict.layer != BattleLayer::space || !result.winner
        || !snapshot.control() || !snapshot.movement()) return out;
    const auto planet = std::find_if(snapshot.control()->planets.begin(), snapshot.control()->planets.end(),
        [&](const auto& row) { return row.planet == conflict.planet; });
    if (planet == snapshot.control()->planets.end() || !planet->surface_accessible || planet->destroyed) return out;
    const auto location = std::find_if(snapshot.planets().begin(), snapshot.planets().end(),
        [&](const auto& row) { return row.id == conflict.planet; });
    if (location == snapshot.planets().end() || !snapshot.fleet_control()) return out;
    for (const auto& fleet : snapshot.fleets()) {
        if (fleet.owner != result.winner || fleet.orbit != conflict.planet || fleet.motion.transit) continue;
        InvasionOffer offer{conflict.planet, fleet.id, fleet.owner, {}};
        for (const auto& member : fleet.members) {
            const auto& types = snapshot.fleet_control()->types;
            const auto transport = std::find_if(types.begin(), types.end(), [&](const auto& row) { return row.type == member.type; });
            const auto& contents = snapshot.movement()->contents;
            const auto cargo = std::find_if(contents.begin(), contents.end(), [&](const auto& row) { return row.container == member.id; });
            if (transport != types.end() && transport->transport && cargo != contents.end() && !cargo->children.empty())
                offer.transports.push_back(member.id);
        }
        if (!offer.transports.empty()) out.push_back(std::move(offer));
    }
    return out;
}
std::vector<std::uint8_t> encode_tactical_return_state(const TacticalReturnState& state) {
    std::vector<std::uint8_t> bytes; put(bytes, 1, 4);
    const auto heroes = gc::encode_hero_returns(state.heroes);
    put(bytes, heroes.size()); bytes.insert(bytes.end(), heroes.begin(), heroes.end());
    put(bytes, state.structure_markers.size());
    for (const auto& [id, marker] : state.structure_markers) { put(bytes, id); put(bytes, marker, 4); }
    put(bytes, state.upgrades.size());
    for (const auto& row : state.upgrades) {
        put(bytes, row.planet); put(bytes, row.upgrade.type); put(bytes, row.upgrade.holder); put(bytes, row.upgrade.owner, 4);
    }
    return bytes;
}
core::Result<TacticalReturnState> decode_tactical_return_state(const std::span<const std::uint8_t> bytes) {
    std::size_t cursor{};
    const auto read = [&](const unsigned size = 8) -> std::optional<std::uint64_t> {
        if (size > bytes.size() - cursor) return {};
        std::uint64_t value{};
        for (unsigned i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(bytes[cursor++]) << (i * 8U);
        return value;
    };
    const auto version = read(4), hero_size = read();
    if (!version || *version != 1 || !hero_size || *hero_size > bytes.size() - cursor)
        return fail<TacticalReturnState>("malformed tactical return state");
    auto heroes = gc::decode_hero_returns(bytes.subspan(cursor, static_cast<std::size_t>(*hero_size)));
    if (!heroes) return core::Result<TacticalReturnState>::failure(heroes.error());
    cursor += static_cast<std::size_t>(*hero_size);
    TacticalReturnState state; state.heroes = std::move(heroes).value();
    const auto markers = read();
    if (!markers || *markers > gc::galactic_max_objects || *markers > (bytes.size() - cursor) / 12)
        return fail<TacticalReturnState>("invalid structure marker count");
    sim::EntityId previous{};
    for (std::uint64_t i = 0; i < *markers; ++i) {
        const auto id = read(), marker = read(4);
        if (!id || *id <= previous || !marker) return fail<TacticalReturnState>("invalid structure marker order");
        previous = *id; state.structure_markers.emplace_back(*id, static_cast<std::uint32_t>(*marker));
    }
    const auto upgrades = read();
    if (!upgrades || *upgrades > gc::galactic_max_objects || *upgrades > (bytes.size() - cursor) / 28)
        return fail<TacticalReturnState>("invalid planet upgrade count");
    std::set<std::tuple<gc::PlanetId, gc::PlayerId, sim::EntityId, sim::tactical::TypeId>> unique;
    for (std::uint64_t i = 0; i < *upgrades; ++i) {
        const auto planet = read(), type = read(), holder = read(), owner = read(4);
        if (!planet || !*planet || !type || !*type || !holder || !*holder || !owner || !*owner
            || !unique.emplace(*planet, static_cast<gc::PlayerId>(*owner), *holder, *type).second)
            return fail<TacticalReturnState>("invalid planet upgrade identity");
        state.upgrades.push_back({*planet, {*type, *holder, static_cast<gc::PlayerId>(*owner)}});
    }
    if (cursor != bytes.size()) return fail<TacticalReturnState>("trailing tactical return bytes");
    return core::Result<TacticalReturnState>::success(std::move(state));
}
} // namespace eawr::campaign
