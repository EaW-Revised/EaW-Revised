#include "reference_pages.hpp"

#include <algorithm>
#include <map>

namespace eawr::viewer::galactic {
std::vector<SummaryPlanet> published_summary_planets(const sim::galactic::GalacticSnapshot& snapshot,
    std::uint32_t local_player, const std::map<ObjectId, PlanetHudFacts>& admitted,
    const std::map<std::uint64_t, ReferenceType>& types,
    const std::map<ObjectId, ReferenceType>& planet_types) {
    using namespace sim::galactic;
    std::vector<SummaryPlanet> result;
    if (!snapshot.observer() || snapshot.observer()->local_player != local_player) return result;
    const auto ally = [&](PlayerId owner) {
        if (owner == local_player) return true;
        for (const auto& player : snapshot.observer()->players) if (player.player == local_player)
            return std::find(player.allies.begin(), player.allies.end(), owner) != player.allies.end();
        return false;
    };
    std::map<sim::EntityId, TypeId> originals;
    for (const auto& object : snapshot.commissioned_originals()) originals.emplace(object.id, object.original_type);
    for (const auto& [id, facts] : admitted) {
        // Require the actual publication as well as the caller's display admission.
        const auto observation = std::find_if(snapshot.observations().begin(), snapshot.observations().end(),
            [&](const auto& row) { return row.player == local_player && row.planet == id; });
        if (observation == snapshot.observations().end() || !observation->discovered
            || observation->known_owner != local_player || !observation->knowledge.ownership
            || !facts.discovered || facts.observed_owner != local_player) continue;
        SummaryPlanet planet;
        planet.planet = id; planet.owner = local_player; planet.authorized = true; planet.name = facts.display_name;
        if (const auto metadata = planet_types.find(id); metadata != planet_types.end()) planet.icon = metadata->second.icon.icon;
        if (const auto* forecast = snapshot.economy_forecast(); forecast && forecast->player == local_player)
            for (const auto& row : forecast->planets) if (row.planet == id) {
                planet.basic = static_cast<double>(row.basic.raw()) / sim::math::Fixed::scale;
                if (row.trade) planet.trade = static_cast<double>(row.trade->raw()) / sim::math::Fixed::scale;
                if (row.modifiers) {
                    planet.modifiers.emplace();
                    for (const auto& modifier : *row.modifiers) {
                        const auto type = types.find(modifier.source_type);
                        if (type != types.end()) planet.modifiers->push_back({
                            static_cast<double>(modifier.income.raw()) / sim::math::Fixed::scale,
                            type->second.mining_facility, type->second.icon});
                        else planet.modifiers.reset();
                        if (!planet.modifiers) break;
                    }
                }
            }
        if (observation->knowledge.production) {
            if (facts.space_queue && !facts.space_queue->empty()) planet.fronts[0] = facts.space_queue->front();
            if (facts.ground_queue && !facts.ground_queue->empty()) planet.fronts[1] = facts.ground_queue->front();
        }
        if (observation->knowledge.structures && snapshot.production()) {
            const auto metadata = planet_types.find(id);
            if (metadata != planet_types.end()) planet.capacity = static_cast<double>(metadata->second.capacity);
            for (const auto& producer : snapshot.production()->planets) if (producer.planet == id) {
                auto add = [&](TypeId type) {
                    const auto entry = types.find(type);
                    if (entry == types.end()) planet.capacity.reset();
                    else if (planet.capacity) *planet.capacity += static_cast<double>(entry->second.capacity);
                };
                if (producer.starbase_type) add(producer.starbase_type);
                for (const auto& object : producer.objects) if (object.kind == ProductionKind::structure) add(object.type);
            }
        }
        auto icons = [&](const auto& members, bool orbital) {
            std::vector<ReferenceIcon> heroes;
            std::map<TypeId, ReferenceIcon> groups;
            for (const auto& member : members) {
                TypeId original = member.type;
                if (const auto at = originals.find(member.id); at != originals.end()) original = at->second;
                const auto entry = types.find(original);
                if (entry == types.end() || entry->second.hidden || entry->second.icon.icon.empty()
                    || (orbital && entry->second.container)) continue;
                if (entry->second.hero) heroes.push_back(entry->second.icon);
                else {
                    auto [at, inserted] = groups.try_emplace(original, entry->second.icon);
                    if (!inserted) ++at->second.count;
                }
            }
            for (const auto& [type, icon] : groups) { static_cast<void>(type); heroes.push_back(icon); }
            return heroes;
        };
        if (observation->knowledge.ground_contents && snapshot.control()) {
            for (const auto& row : snapshot.control()->planets) if (row.planet == id) {
                auto members = row.landed;
                std::erase_if(members, [&](const auto& member) { return !ally(member.owner); });
                planet.ground = icons(members, false);
            }
        }
        if (observation->knowledge.fleet_contents) {
            std::array<std::vector<FleetMember>, 3> members;
            for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == id && fleet.orbit_slot < 3 && ally(fleet.owner))
                members[fleet.orbit_slot].insert(members[fleet.orbit_slot].end(), fleet.members.begin(), fleet.members.end());
            for (std::size_t slot = 0; slot < 3; ++slot) {
                bool known = true;
                for (const auto& member : members[slot]) {
                    const auto type = types.find(member.type);
                    if (type == types.end() || !type->second.gui_visibility_known) { known = false; break; }
                }
                if (known) planet.orbit[slot] = icons(members[slot], true);
            }
        }
        result.push_back(std::move(planet));
    }
    return result;
}
} // namespace eawr::viewer::galactic
