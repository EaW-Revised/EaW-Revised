#include "session_impl.hpp"
#include "encoding.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "observer_internal.hpp"

#include <algorithm>

// WGCH-67/140..142/143/146/147: the shared live ruins owner. One adapter serves hacking, strategic
// destruction and any later trusted caller; native effect 17 (coordinator-reserved) journals the request.
namespace eawr::sim::galactic {
namespace {
using math::Fixed;
template<class Rows, class Id, class Key> auto find(Rows& rows, Id id, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), id,
        [&](const auto& row, auto value) { return key(row) < value; });
    return at != rows.end() && key(*at) == id ? &*at : nullptr;
}
core::Result<bool> refused() { return core::Result<bool>::success(false); }
}

Fixed GalacticSession::Impl::planet_base_credits(const std::size_t index) const {
    const auto& base = setup.fiscal->planets[index];
    if (!control || !setup.ability_host) return base.credits;
    const auto* surface = find(control->planets, base.planet, [](const auto& r) { return r.planet; });
    if (!surface || !surface->destroyed) return base.credits;
    // WGCH-143: the base income reader selects the authored destroyed value; fiscal modifiers still follow WGEP-05.
    const auto* ruined = find(setup.ability_host->ruined_income, base.planet, [](const auto& r) { return r.planet; });
    return ruined ? ruined->credits : base.credits;
}

void GalacticSession::Impl::detach_observer_sources(const std::span<const EntityId> sources) {
    if (!observer || sources.empty()) return;
    const std::set<EntityId> gone(sources.begin(), sources.end());
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < observer->records.size(); ++i) {
        auto& rows = observer->records[i].sources;
        if (std::erase_if(rows, [&](const auto& source) { return gone.contains(source.source); })) changed.push_back(i);
    }
    if (changed.empty()) return;
    const auto presence = observer_detail::presence(*observer, sorted_fleets(), movement_inputs.get(), actor_inputs());
    for (const auto index : changed) {
        auto& record = observer->records[index];
        const auto* player = find(observer->players, record.player, [](const auto& r) { return r.player; });
        const auto* location = find(observer->planets, record.planet, [](const auto& r) { return r.planet; });
        if (player && location) observer_detail::refresh(record, *player, *location, presence, clock.galactic_frame);
    }
    dirty[static_cast<std::size_t>(StateSection::observer)] = true;
}

bool GalacticSession::Impl::plan_container_loss(const EntityId container, const TypeId type, const PlayerId owner,
    std::vector<HeroDefeat>& losses, const std::span<const EntityId> excluded) {
    if (!setup.ability_host) return false;
    std::set<EntityId> requested(excluded.begin(), excluded.end()); // already-leaving identities count as dead
    // WGCH-141: contained named heroes are found in stored order; each requests its respawn with no explicit
    // planet, zero requested seconds and no admission bypass, then is destroyed before the next is found.
    const auto visit = [&](auto&& self, const EntityId parent, const std::size_t depth) -> bool {
        if (depth > 32 || !movement_inputs) return depth <= 32;
        const auto* contents = find(movement_inputs->contents, parent, [](const auto& r) { return r.container; });
        if (!contents) return true;
        for (const auto& child : contents->children) {
            const auto record = actor_records.find(child.id);
            if (record == actor_records.end() || record->second.dead || record->second.delete_pending
                || std::find(excluded.begin(), excluded.end(), child.id) != excluded.end()) continue;
            const auto* profile = find(setup.ability_host->profiles, record->second.original_type, [](const auto& r) { return r.type; });
            if (profile && profile->named) {
                auto defeat = prepare_ability_defeat(record->second, Fixed{}, false);
                if (!defeat) return false; // named parent-classification seam: refuse before any mutation
                defeat->current_system = defeat->planet; // WGM-80: supplied planet, otherwise current system
                defeat->planet = 0;
                for (auto& companion : defeat->companions)
                    if (requested.contains(companion.id)) companion.dead = true; // earlier requests already destroyed them
                requested.insert(child.id);
                losses.push_back(std::move(*defeat));
                continue;
            }
            if (!self(self, child.id, depth + 1)) return false;
        }
        return true;
    };
    if (!visit(visit, container, 0)) return false;
    // Ordinary destruction owns the remaining contained units; the carrier itself never schedules a return.
    HeroDefeat carrier; carrier.id = container; carrier.type = carrier.respawn_type = type; carrier.owner = owner;
    std::vector<EntityId> work{container}; std::set<EntityId> visited;
    while (!work.empty()) {
        const auto id = work.back(); work.pop_back();
        if (!visited.insert(id).second) continue;
        if (visited.size() > galactic_max_objects) return false;
        if (id != container && !requested.contains(id)) carrier.removed_objects.push_back(id);
        const auto* contents = movement_inputs ? find(movement_inputs->contents, id, [](const auto& r) { return r.container; }) : nullptr;
        if (contents) for (const auto& child : contents->children) work.push_back(child.id);
    }
    losses.push_back(std::move(carrier));
    return true;
}

std::optional<GalacticSession::Impl::RuinsPlan> GalacticSession::Impl::plan_planet_ruins(const PlanetId planet, const bool destroy,
    const std::span<const EntityId> excluded) {
    if (!control || !production || !economy_corruption || !ability_host || !setup.ability_host || !planet || failed)
        return std::nullopt;
    const auto* surface = find(control->planets, planet, [](const auto& r) { return r.planet; });
    if (!surface) return std::nullopt;
    const auto index = static_cast<std::size_t>(surface - control->planets.data());
    const auto* economic = find(economy_corruption->planets, planet, [](const auto& r) { return r.planet; });
    if (index >= production->planets.size() || production->planets[index].planet != planet || !economic || !planets.contains(planet))
        return std::nullopt;
    if (surface->destroyed || economic->destroyed) return std::nullopt; // an already ruined planet is never re-cleaned
    RuinsPlan plan; plan.planet = planet; plan.destroy = destroy;
    // WGCH-141: the stored transport list is traversed in reverse, every faction included.
    for (auto at = surface->landed.rbegin(); at != surface->landed.rend(); ++at) {
        if (!plan_container_loss(at->id, at->type, at->owner, plan.losses, excluded)) return std::nullopt;
        plan.landed.push_back(at->id);
    }
    if (!plan.losses.empty() && !ability_consumption_handler) return std::nullopt;
    return plan;
}

core::Result<void> GalacticSession::Impl::execute_planet_ruins(const RuinsPlan& plan) {
    const auto planet = plan.planet;
    const auto control_at = find(control->planets, planet, [](const auto& r) { return r.planet; });
    const auto index = static_cast<std::size_t>(control_at - control->planets.data());
    auto* economic = find(economy_corruption->planets, planet, [](const auto& r) { return r.planet; });
    // WGCH-140: the destroyed-planet story callback precedes every cleanup mutation and the flag write.
    if (plan.destroy && planet_ruins_story_handler) {
        auto noticed = planet_ruins_story_handler(planet);
        if (!noticed) return noticed;
    }
    // Siphon termination applies regardless of the destroy flag (WGCH-67).
    if (economic->siphon.active) { economic->siphon.active = false; dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true; }
    // WGCH-141: canonical loss and hero scheduling for landed transports (reverse order), via the shared owner.
    for (const auto& loss : plan.losses) {
        auto lost = ability_consumption_handler(loss);
        if (!lost) { failed = true; return lost; }
    }
    // WGCH-142: ground special structures leave first, with their planetary influence; ruins supplies no final-blow player.
    std::vector<EntityId> detached(plan.landed.begin(), plan.landed.end());
    clear_control_structures(index, ProductionDomain::ground, &detached);
    if (plan.destroy) detached.push_back(planet); // the planet's own abilities terminate in a galactic mode
    {
        auto& surface = control->planets[index];
        surface.landed.clear();
        surface.ground_base = {}; // the ground base level refreshes after clearing
        surface.transition_modifiers.clear(); // WGCH-67: political modifiers clear regardless of the flag
        dirty[static_cast<std::size_t>(StateSection::control)] = true;
    }
    std::vector<GalacticEvent> discarded;
    update_production_control(index, completed_tick, discarded, false);
    if (plan.destroy) {
        production->planets[index].ground_slots = 0; // WGCH-67: land special-structure capacity zero
        dirty[static_cast<std::size_t>(StateSection::production)] = true;
    }
    // WGCH-140: capture the old corruption type, abort its transition, then apply the existing cleanup path.
    const auto old = economic->corruption;
    if (auto* row = find(ability_host->transitions, planet, [](const auto& r) { return r.planet; })) {
        const auto due = corruption_due.find(row->transition.end);
        if (due != corruption_due.end()) { std::erase(due->second, planet); if (due->second.empty()) corruption_due.erase(due); }
        row->transition = {corruption_fixture::Type::none, corruption_fixture::Type::none, false, false, 0, 0, clock.galactic_frame};
        touch_abilities();
    }
    if (old != corruption_fixture::Type::none) {
        auto cleaned = apply_corruption_completion(planet, old, corruption_fixture::Type::none);
        if (!cleaned) { failed = true; return cleaned; }
    }
    // Observer facts and sources follow the same ordered commit; the observer owner remains the refresh authority.
    if (observer) {
        if (auto* location = find(observer->planets, planet, [](const auto& r) { return r.planet; })) {
            location->landed_transports.clear(); location->ground_level = 0; location->ground_owner = 0;
            dirty[static_cast<std::size_t>(StateSection::observer)] = true;
        }
    }
    detach_observer_sources(detached);
    if (observer) {
        const auto presence = observer_detail::presence(*observer, sorted_fleets(), movement_inputs.get(), actor_inputs());
        const auto* location = find(observer->planets, planet, [](const auto& r) { return r.planet; });
        if (location) for (auto& record : observer->records) if (record.planet == planet) {
            const auto* player = find(observer->players, record.player, [](const auto& r) { return r.player; });
            if (player) observer_detail::refresh(record, *player, *location, presence, clock.galactic_frame);
        }
    }
    if (plan.destroy) {
        // WGCH-143/147: the surface reader returns false regardless of the authored flag; every owner reads these.
        control->planets[index].surface_accessible = false;
        production->planets[index].surface_accessible = false;
        dirty[static_cast<std::size_t>(StateSection::production)] = true;
        // WGCH-140: the destroyed flag is written LAST.
        control->planets[index].destroyed = true;
        economic->destroyed = true;
        dirty[static_cast<std::size_t>(StateSection::control)] = true;
        dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    }
    // WGEP-170: ruins terminates abilities even when the destroyed flag is not requested.
    if (auto refreshed = refresh_planet_technology_price(planet, false, true); !refreshed) { failed = true; return refreshed; }
    population_stale = true;
    ++income_control_generation; // the ruined flag selects the base value read by cached contributions
    invalidate_observer_projection();
    const auto events = snapshot->events();
    snapshot = make_snapshot(std::vector<GalacticEvent>(events.begin(), events.end()));
    auto delivered = deliver_actor_changes();
    if (!delivered) failed = true;
    return delivered;
}

void GalacticSession::set_planet_ruins_story_handler(PlanetRuinsStoryHandler handler) {
    impl_->planet_ruins_story_handler = std::move(handler);
}

core::Result<bool> GalacticSession::request_planet_ruins(const PlanetId planet, const bool destroy) {
    auto& impl = *impl_;
    auto plan = impl.plan_planet_ruins(planet, destroy);
    if (!plan) return refused(); // nothing is journaled or mutated for an unavailable request
    if (!impl.native_replaying) {
        std::vector<std::uint8_t> payload; detail::put_u8(payload, 0); detail::put_u64(payload, planet);
        detail::put_u8(payload, destroy ? 1 : 0);
        auto recorded = append_native_effect(NativeEffectKind::planet_ruins, std::move(payload));
        if (!recorded) return core::Result<bool>::failure(recorded.error());
    }
    auto executed = impl.execute_planet_ruins(*plan);
    if (!executed) return core::Result<bool>::failure(executed.error());
    return core::Result<bool>::success(true);
}
} // namespace eawr::sim::galactic
