#include "session_impl.hpp"
#include "observer_internal.hpp"

#include <algorithm>
#include <set>

namespace eawr::sim::galactic {
namespace od = observer_detail;

// WGUI-19/21/22/101: a Galactic_Automatic spy applies the moment its source object is present at a planet and
// is removed the moment it is not (death, deletion, departure, type change). The source's CURRENT owner is the
// recipient, so an owner change moves the effect. Everything is derived from the census row and compared with
// the retained per-player source lists, so the call is idempotent and adds no persisted state.
void GalacticSession::Impl::sync_auto_spies(EntityId id, TypeId type, const CensusObject* live) {
    if (!observer || !setup.ability_host) return;
    const auto* profile = od::find(setup.ability_host->profiles, type, [](const auto& row) { return row.type; });
    if (!profile || profile->auto_spies.empty()) return;
    const auto player_count = observer->players.size(), planet_count = observer->planets.size();
    if (!planet_count || observer->records.size() != player_count * planet_count) return;
    const auto world = sorted_planets();
    const bool present = live && !live->dead && !live->delete_pending && live->planet;
    const auto* center = present ? od::find(world, live->planet, [](const auto& p) { return p.id; }) : nullptr;
    std::set<PlanetId> touched;
    for (const auto& spy : profile->auto_spies) {
        std::vector<bool> wanted_planet(planet_count, false);
        const auto expiry = intelligence_expiry(spy.duration, clock.galactic_frame, observer->logical_fps);
        if (center && expiry) {
            for (std::size_t t = 0; t < planet_count; ++t) {
                const auto target = observer->planets[t].planet;
                const auto* at = od::find(world, target, [](const auto& p) { return p.id; });
                // WGUI-24/102: nonpositive range stays local; a positive range is the nearby-planet query.
                wanted_planet[t] = at && (spy.influence_range <= math::Fixed{} ? target == live->planet
                    : od::nearby(center->position, at->position, spy.influence_range));
            }
        }
        for (std::size_t p = 0; p < player_count; ++p) for (std::size_t t = 0; t < planet_count; ++t) {
            auto& record = observer->records[p * planet_count + t];
            const bool wanted = center && expiry && wanted_planet[t] && record.player == live->owner;
            const auto same = [&](const auto& row) { return row.source == id && row.ability == spy.identity; };
            const bool has = std::any_of(record.sources.begin(), record.sources.end(), same);
            if (wanted && !has) {
                record.sources.push_back({id, spy.identity, spy.text_identity, spy.mask, expiry.value()});
                touched.insert(record.planet);
            } else if (!wanted && has) {
                std::erase_if(record.sources, same);
                touched.insert(record.planet);
            }
        }
    }
    if (touched.empty()) return;
    const auto table = od::presence(*observer, sorted_fleets(), movement_inputs.get(), actor_inputs());
    // WGUI-21/22: every successful insertion or removal refreshes all recipients at the affected planets.
    for (std::size_t i = 0; i < observer->records.size(); ++i) if (touched.contains(observer->records[i].planet))
        od::refresh(observer->records[i], observer->players[i / planet_count], observer->planets[i % planet_count],
            table, clock.galactic_frame);
    dirty[static_cast<std::size_t>(StateSection::observer)] = true;
    invalidate_observer_projection(false);
}
} // namespace eawr::sim::galactic
