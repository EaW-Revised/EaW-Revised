#include "session_impl.hpp"
#include "observer_internal.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace eawr::sim::galactic {
namespace od = observer_detail;

void GalacticSession::Impl::transfer_planet_spies(PlanetId planet, PlayerId old_owner, PlayerId new_owner) {
    if (!observer || old_owner == new_owner
        || std::none_of(observer->planet_spies.begin(), observer->planet_spies.end(), [&](const auto& spy) {
            return spy.planet == planet && spy.enabled && spy.style == SpyStyle::galactic_automatic;
        })) return;
    // GFD-02/06: synchronous ownership effects precede later readers.
    od::planet_spy_transition(*observer, sorted_planets(), planet, old_owner, new_owner,
        clock.galactic_frame, sorted_fleets(), movement_inputs.get(), actor_inputs());
    dirty[static_cast<std::size_t>(StateSection::observer)] = true;
    invalidate_observer_projection(false);
}

namespace observer_detail {
void planet_spy_transition(ObserverSetup& observer, std::span<const Planet> planets, PlanetId source,
    PlayerId old_owner, PlayerId new_owner, std::uint64_t frame,
    std::span<const Fleet> fleets, const MovementSetup* movement, std::span<const CensusObject> actors) {
    const auto center = std::find_if(planets.begin(), planets.end(), [&](const auto& p) { return p.id == source; });
    if (center == planets.end()) return;
    std::set<PlanetId> changed;
    for (const auto& definition : observer.planet_spies) {
        if (definition.planet != source || !definition.enabled || definition.style != SpyStyle::galactic_automatic) continue;
        const auto spy = validated_spy({definition.style, definition.duration, definition.mask});
        const auto expiry = intelligence_expiry(spy.duration, frame, observer.logical_fps);
        if (!expiry) continue; // automatic validation makes this nonpositive; setup validated.
        for (std::size_t target_index = 0; target_index < planets.size(); ++target_index) {
            const auto& target = planets[target_index];
            if (definition.influence_range <= math::Fixed{} ? target.id != source
                : !nearby(center->position, target.position, definition.influence_range)) continue;
            for (std::size_t player_index = 0; player_index < observer.players.size(); ++player_index) {
                auto& record = observer.records[player_index * planets.size() + target_index];
                if (old_owner && record.player == old_owner) {
                    const auto count = record.sources.size();
                    std::erase_if(record.sources, [&](const auto& row) { return row.source == source && row.ability == definition.ability; });
                    if (count != record.sources.size()) changed.insert(target.id);
                }
                if (new_owner && record.player == new_owner) {
                    record.sources.push_back({source, definition.ability, definition.text_identity, spy.mask, expiry.value()});
                    changed.insert(target.id);
                }
            }
        }
    }
    if (changed.empty()) return;
    const auto table = presence(observer, fleets, movement, actors);
    // WGUI-21/22: refresh all recipients after successful insertion/removal.
    for (std::size_t i = 0; i < observer.records.size(); ++i) if (changed.contains(observer.records[i].planet))
        refresh(observer.records[i], observer.players[i / observer.planets.size()],
            observer.planets[i % observer.planets.size()], table, frame);
}
} // namespace observer_detail

void GalacticSession::Impl::publish_surface_arrivals(const PlanetId planet,
    const std::span<const SurfaceForce> arrivals) {
    if (!observer || arrivals.empty()) return;
    const auto target = std::lower_bound(observer->planets.begin(), observer->planets.end(), planet,
        [](const auto& row, auto id) { return row.planet < id; });
    if (target == observer->planets.end() || target->planet != planet) return;
    // WGEP-41 / WGUI-41/103: the ordered placement commit creates these live
    // surface links. Publish the same identities before the next observation;
    // the observer's content/count gates still decide what a player can see.
    for (const auto& row : arrivals)
        target->landed_transports.push_back({row.id, row.owner, false, false});
    dirty[static_cast<std::size_t>(StateSection::observer)] = true;
}

void GalacticSession::Impl::sync_observer_control(const std::size_t control_index) {
    if (!observer || !control || control_index >= control->planets.size() || control_index >= control->state.size()) return;
    const auto& presence = control->planets[control_index];
    const auto& state = control->state[control_index];
    if (!state.initialized) return;
    const auto at = std::lower_bound(observer->planets.begin(), observer->planets.end(), presence.planet,
        [](const auto& row, auto id) { return row.planet < id; });
    if (at == observer->planets.end() || at->planet != presence.planet) return;
    // WGUI-100 supports live facts; its zero-controller fallback is an unverified adapter.
    // Observer setup accepts only listed controllers, while later control commits may have no cap controller.
    const PlayerId controller = state.controller ? state.controller : state.current_player;
    const bool target_changed = at->target_owner != state.target_player;
    const auto political = math::Fixed::from_raw(state.current.raw() / math::Fixed::scale * math::Fixed::scale); // integer getter
    if (at->allegiance == state.current_player && at->target_owner == state.target_player
        && at->current_owner == state.current_player && at->current_controller == controller
        && at->political_control == political
        && at->space_owner == presence.starbase.owner && at->space_level == presence.starbase.level
        && at->ground_owner == presence.ground_base.owner && at->ground_level == presence.ground_base.level) return;
    at->allegiance = state.current_player; at->target_owner = state.target_player;
    at->current_owner = state.current_player; at->current_controller = controller;
    at->political_control = political;
    at->space_owner = presence.starbase.owner; at->space_level = presence.starbase.level;
    at->ground_owner = presence.ground_base.owner; at->ground_level = presence.ground_base.level;
    if (target_changed) {
        // WGUI-06/07/08: the ownership commit replaces the default intelligence
        // for an already discovered planet too. Reprojecting a stale mask alone
        // reveals allegiance but leaves production and surface contents hidden.
        const auto table = od::presence(*observer, sorted_fleets(), movement_inputs.get(), actor_inputs());
        const auto planet_index = static_cast<std::size_t>(at - observer->planets.begin());
        for (std::size_t player_index = 0; player_index < observer->players.size(); ++player_index)
            od::refresh(observer->records[player_index * observer->planets.size() + planet_index],
                observer->players[player_index], *at, table, clock.galactic_frame);
    }
    dirty[static_cast<std::size_t>(StateSection::observer)] = true;
    invalidate_observer_projection(false); // observations/HUD facts only; no income input moved
}

void GalacticSession::Impl::unpublish_surface_departures(const PlanetId planet,
    const std::span<const EntityId> departures) {
    if (!observer || departures.empty()) return;
    const auto target = std::lower_bound(observer->planets.begin(), observer->planets.end(), planet,
        [](const auto& row, auto id) { return row.planet < id; });
    if (target == observer->planets.end() || target->planet != planet) return;
    // The matching un-publish of publish_surface_arrivals: the live surface link
    // left the control list, so the observer's reference leaves with it.
    const auto count = target->landed_transports.size();
    std::erase_if(target->landed_transports, [&](const auto& row) {
        return std::find(departures.begin(), departures.end(), row.id) != departures.end();
    });
    if (target->landed_transports.size() != count) dirty[static_cast<std::size_t>(StateSection::observer)] = true;
}

std::optional<PlayerId> GalacticSession::Impl::intelligence_source_owner(EntityId source) const {
    if (const auto actor = actor_records.find(source); actor != actor_records.end() && !actor->second.dead && !actor->second.delete_pending) return actor->second.owner;
    if (planets.contains(source)) return registry.get<session_detail::PlanetData>(planets.at(source)).owner;
    if (fleets.contains(source)) return registry.get<session_detail::FleetData>(fleets.at(source)).owner;
    if (const auto unit = detached_units.find(source); unit != detached_units.end()) return unit->second.owner;
    std::optional<PlayerId> owner;
    for (const auto& fleet : sorted_fleets()) {
        const auto inspect = [&](auto&& self, const FleetMember& member, unsigned depth) -> void {
            if (member.id == source) owner = fleet.owner;
            if (depth > 32 || !movement_inputs) return;
            if (const auto* contents = od::find(movement_inputs->contents, member.id, [](const auto& row) { return row.container; }))
                for (const auto& child : contents->children) self(self, child, depth + 1);
        };
        for (const auto& member : fleet.members) inspect(inspect, member, 0);
    }
    if (owner) return owner;
    if (production) for (const auto& planet : production->planets) for (const auto& object : planet.objects)
        if (object.id == source) return object.owner;
    if (observer) for (const auto& planet : observer->planets) for (const auto& transport : planet.landed_transports)
        if (transport.id == source && source != 0) return transport.owner;
    return {};
}

void GalacticSession::Impl::refresh_observers_for_load(const std::uint64_t frame, const std::uint32_t logical_fps) {
    if (!observer) return;
    const auto previous_records = observer->records;
    const auto table = od::presence(*observer, sorted_fleets(), movement_inputs.get(), actor_inputs());
    // WGUI-102: player-load refresh is ordinary and precedes mode fix-up. Exact
    // runtime/resync restoration does not execute a player-load fix-up.
    // WGUI-102: the campaign supplies authoritative active-mode time after child
    // restoration. Pacing and presentation clocks do not participate.
    observer->logical_fps = logical_fps;
    for (std::size_t i = 0; i < observer->records.size(); ++i) {
        if (!std::binary_search(observer->human_observers.begin(), observer->human_observers.end(), observer->records[i].player)) continue;
        od::refresh(observer->records[i], observer->players[i / observer->planets.size()],
            observer->planets[i % observer->planets.size()], table, frame, true);
    }
    dirty[static_cast<std::size_t>(StateSection::observer)] = true;
    if (observer->records != previous_records) invalidate_observer_projection(false);
}

void GalacticSession::Impl::initialize_observer() {
    if (!observer) return;
    const auto positions = sorted_planets(); const auto links = sorted_routes();
    std::map<PlanetId, std::set<PlanetId>> routes_by_planet;
    for (const auto& r : links) { routes_by_planet[r.from].insert(r.to); routes_by_planet[r.to].insert(r.from); }
    observer_neighbours.resize(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        auto& targets = observer_neighbours[i];
        for (std::size_t j = 0; j < positions.size(); ++j) {
            if (i == j) continue;
            const bool route = routes_by_planet[positions[i].id].contains(positions[j].id);
            if (route || od::nearby(positions[i].position, positions[j].position, observer->automatic_reveal_range)) targets.push_back(j);
        }
    }
}

core::Result<void> GalacticSession::Impl::service_discovery(const PartitionExecutor& executor,
    const std::uint64_t tick, std::vector<GalacticEvent>& events) {
    discovery_records = discovery_targets = 0;
    if (!observer) return core::Result<void>::success();
    const auto& inputs = *observer;
    const auto live_fleets = sorted_fleets();
    const auto actors = actor_inputs();
    std::vector<od::PresenceTable> fleet_stages(live_fleets.size());
    const auto presence_phase = executor.execute_phase("galactic-observer-presence", tick_partition_count, [&](std::size_t partition) {
        const auto range = partition_range(partition, live_fleets.size());
        for (auto i = range.begin; i < range.end; ++i)
            fleet_stages[i] = od::presence(inputs, std::span<const Fleet>(&live_fleets[i], 1), movement_inputs.get(), actors);
    });
    if (!presence_phase) return presence_phase;
    od::PresenceTable table;
    for (const auto& stage : fleet_stages) {
        for (const auto& [key, value] : stage) {
            auto& target = table[key]; target.fleet |= value.fleet; target.reveal_force |= value.reveal_force; target.superweapon |= value.superweapon;
        }
        table.superweapon_planets.insert(stage.superweapon_planets.begin(), stage.superweapon_planets.end());
    }
    struct Stage { std::uint64_t counter{}; bool qualifies{}; };
    std::vector<Stage> stages(inputs.records.size());
    const auto delay = static_cast<std::uint64_t>(inputs.reveal_delay_seconds.raw());
    const auto scale = static_cast<std::uint64_t>(math::Fixed::scale);
    const auto converted = intelligence_expiry(inputs.reveal_delay_seconds, 0, inputs.logical_fps);
    if (!converted) return core::Result<void>::failure(converted.error());
    // Discovery compares the unrounded seconds*FPS threshold, unlike expiry rounding.
    const auto whole = delay / scale;
    const auto threshold = whole * inputs.logical_fps + (delay % scale) * inputs.logical_fps / scale;
    const auto phase = executor.execute_phase("galactic-discovery", tick_partition_count, [&](std::size_t partition) {
        const auto range = partition_range(partition, stages.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& record = inputs.records[i];
            const auto& player = inputs.players[i / inputs.planets.size()];
            const auto& planet = inputs.planets[i % inputs.planets.size()];
            auto& stage = stages[i]; stage.counter = record.reveal_counter;
            if (player.ai) continue; // AI discovery initialization is an explicit producer seam.
            if (od::allied(player, planet.allegiance)) stage.qualifies = true;
            else {
                const auto found = table.find({player.player, planet.planet});
                if (clock.pause_depth == 0 && found != table.end() && found->second.reveal_force) {
                    if (record.reveal_counter <= threshold) stage.counter = record.reveal_counter + 1;
                    else stage.qualifies = true; // WGUI-02: compare the OLD counter, strict greater.
                } else stage.counter = 0;
            }
            stage.qualifies |= planet.corrupted && player.benefits_from_corruption;
        }
    });
    if (!phase) return phase;
    discovery_records = stages.size();
    bool dirty_state{}, projection_changed{};
    std::vector<std::size_t> undiscovered(inputs.players.size());
    for (std::size_t i = 0; i < inputs.records.size(); ++i) if (!inputs.records[i].discovered) ++undiscovered[i / inputs.planets.size()];
    std::set<PlayerId> feedback;
    const auto reveal = [&](std::size_t index) {
        auto& r = observer->records[index];
        if (r.discovered) return;
        r.discovered = true; dirty_state = projection_changed = true; feedback.insert(r.player);
        --undiscovered[index / inputs.planets.size()];
        od::refresh(r, inputs.players[index / inputs.planets.size()], inputs.planets[index % inputs.planets.size()], table, r.refresh_frame);
        events.push_back({tick, GalacticEventKind::planet_discovered, {}, r.planet, 0, r.player, GalacticRejectReason::none});
    };
    // WGUI-107: only staged eligible seeds expand, including already discovered
    // seeds; newly revealed endpoints do not acquire eligibility from their flag.
    for (std::size_t i = 0; i < stages.size(); ++i) {
        auto& r = observer->records[i];
        if (r.reveal_counter != stages[i].counter) { r.reveal_counter = stages[i].counter; dirty_state = true; }
        if (!stages[i].qualifies) continue;
        reveal(i);
        const auto planet_index = i % inputs.planets.size();
        if (!inputs.planets[planet_index].automatic_reveal) continue;
        const auto player_index = i / inputs.planets.size();
        if (undiscovered[player_index] == 0) continue;
        for (const auto target : observer_neighbours[planet_index]) {
            ++discovery_targets;
            // WGUI-04: locally owned candidates are excluded, not required.
            if (registry.get<session_detail::PlanetData>(planets.at(inputs.planets[target].planet)).owner == r.player) continue;
            reveal(player_index * inputs.planets.size() + target);
        }
    }
    if (tick > 0) for (const auto player : feedback)
        events.push_back({tick, GalacticEventKind::discovery_feedback, {}, 0, 0, player, GalacticRejectReason::none});
    if (dirty_state) {
        dirty[static_cast<std::size_t>(StateSection::observer)] = true;
        // Discovery counters are saved, but no projected query consumes them.
        // Only newly discovered rows change this service's observer projection.
        if (projection_changed) invalidate_observer_projection(false);
    }
    return core::Result<void>::success();
}

void GalacticSession::Impl::execute_observer(const GameCommand& command, std::uint64_t tick,
    std::vector<GalacticEvent>& events) {
    const auto* issuer = seat(command.key.seat);
    if (!issuer || !issuer->active || issuer->kind != SeatKind::script) {
        const auto& request = std::get<ObserverCommand>(command.payload);
        events.push_back({tick, GalacticEventKind::command_rejected, command.key.seat, request.planet, command.key.sequence, request.player,
            !issuer || !issuer->active ? GalacticRejectReason::inactive_seat : GalacticRejectReason::not_authorized});
        return;
    }
    apply_observer(command, tick, events);
}
// Private ability service entry point; player commands cannot supply observer facts.
void GalacticSession::Impl::apply_observer(const GameCommand& command, std::uint64_t tick,
    std::vector<GalacticEvent>& events) {
    auto c = std::get<ObserverCommand>(command.payload);
    GalacticEvent event{tick, GalacticEventKind::command_executed, command.key.seat, c.planet, command.key.sequence, c.player, GalacticRejectReason::none};
    const auto reject = [&](GalacticRejectReason reason) { event.kind = GalacticEventKind::command_rejected; event.reason = reason; events.push_back(event); };
    if (!observer) return reject(GalacticRejectReason::observer_input);
    bool termination_cleanup{};
    // WGUI-101: only owner-destroyed termination of a despawning ability skips cleanup.
    if (c.action == ObserverAction::terminate) {
        for (const auto& spy : observer->planet_spies)
            if (spy.planet == c.source.source && spy.ability == c.source.ability) c.despawning = spy.despawning;
        if (c.despawning && c.owner_destroyed) {
            for (auto& spy : observer->planet_spies) if (spy.planet == c.source.source && spy.ability == c.source.ability) spy.enabled = false;
            dirty[static_cast<std::size_t>(StateSection::observer)] = true;
            events.push_back(event); return;
        }
        termination_cleanup = true; c.action = ObserverAction::remove; c.planet = 0;
    }
    if (c.action == ObserverAction::influence_insert || c.action == ObserverAction::influence_remove) {
        if (!c.enabled || c.style != SpyStyle::galactic_automatic) { events.push_back(event); return; }
        const auto source_owner = intelligence_source_owner(c.source.source);
        if (!source_owner) return reject(GalacticRejectReason::observer_input);
        const auto spy = validated_spy({c.style, c.duration, c.source.mask});
        c.player = *source_owner; c.duration = spy.duration; c.source.mask = spy.mask; c.spy_effect = true;
        c.action = c.action == ObserverAction::influence_insert ? ObserverAction::add : ObserverAction::remove;
        event.value = c.player;
    }
    if (c.spy_effect && (c.action == ObserverAction::add || c.action == ObserverAction::remove)) {
        // WGUI-101: live source ownership selects the recipient; a destroyed
        // source's callback supplies its retained current-owner fact for cleanup.
        if (const auto owner = intelligence_source_owner(c.source.source)) c.player = *owner;
        else if (c.action == ObserverAction::add) return reject(GalacticRejectReason::observer_input);
        event.value = c.player;
    }
    auto staged = *observer; // transactional source/control update; no partial writes on failure
    if (termination_cleanup) for (auto& spy : staged.planet_spies)
        if (spy.planet == c.source.source && spy.ability == c.source.ability) spy.enabled = false;
    if (c.action == ObserverAction::refresh && !c.player && !c.planet) staged.logical_fps = c.logical_fps;
    const auto* player = od::find(staged.players, c.player, [](const auto& p) { return p.player; });
    const auto* planet = od::find(staged.planets, c.planet, [](const auto& p) { return p.planet; });
    if ((c.player && !player) || (c.planet && !planet)) return reject(GalacticRejectReason::observer_input);
    if (c.action != ObserverAction::refresh && (!player || (!planet && !termination_cleanup))) return reject(GalacticRejectReason::observer_input);
    if (c.source.final_frame != 0) return reject(GalacticRejectReason::observer_input);
    auto expiry = intelligence_expiry(c.duration, c.gameplay_frame, c.logical_fps);
    if (!expiry) return reject(GalacticRejectReason::observer_input);
    if (c.action == ObserverAction::add || c.action == ObserverAction::remove || c.action == ObserverAction::transfer) {
        if (!c.source.source || !c.source.ability || (c.source.mask & ~all_intelligence)) return reject(GalacticRejectReason::observer_input);
        // WGUI-21: source existence is required, not just a nonzero identity.
        const bool source_exists = intelligence_source_owner(c.source.source).has_value();
        // Removal is also valid after source destruction; the action owns cleanup.
        if (c.action != ObserverAction::remove && !source_exists) return reject(GalacticRejectReason::observer_input);
    }
    const auto current_planets = sorted_planets();
    std::set<PlanetId> targets;
    if (c.planet) targets.insert(c.planet);
    if (termination_cleanup) for (const auto& p : staged.planets) targets.insert(p.planet);
    if (c.action == ObserverAction::transfer) {
        if (c.source.source != c.planet || !od::find(staged.players, c.previous_player, [](const auto& p) { return p.player; }))
            return reject(GalacticRejectReason::observer_input);
        if (c.influence_range > math::Fixed{}) {
            targets.clear();
            const auto* center = od::find(current_planets, c.planet, [](const auto& p) { return p.id; });
            for (const auto& p : current_planets) if (od::nearby(center->position, p.position, c.influence_range)) targets.insert(p.id);
        }
    }
    if (c.action == ObserverAction::update_planet) {
        if (c.facts.planet != c.planet) return reject(GalacticRejectReason::observer_input);
        staged.planets[static_cast<std::size_t>(planet - staged.planets.data())] = c.facts;
    }
    std::set<PlanetId> changed_targets;
    for (auto& r : staged.records) {
        if (c.action == ObserverAction::refresh) {
            if ((c.player && r.player != c.player) || (c.planet && r.planet != c.planet)) continue;
        } else if (!targets.contains(r.planet)) continue;
        if (!c.spy_effect && c.action != ObserverAction::update_planet && r.player != c.player) continue;
        // WGUI-102: a linked tactical refresh can use a child clock, which need
        // not be monotone relative to the retained galaxy's clock.
        const auto remove = [&] {
            const auto count = r.sources.size();
            std::erase_if(r.sources, [&](const auto& s) { return s.source == c.source.source && s.ability == c.source.ability; });
            if (r.sources.size() != count) changed_targets.insert(r.planet);
        };
        if ((c.action == ObserverAction::remove && r.player == c.player)
            || (c.action == ObserverAction::transfer && r.player == c.previous_player)) remove();
        if ((c.action == ObserverAction::add || c.action == ObserverAction::transfer) && r.player == c.player) {
            auto source = c.source; source.final_frame = expiry.value(); r.sources.push_back(std::move(source)); changed_targets.insert(r.planet);
        }
    }
    auto checked = setup; checked.observer = staged;
    if (!validate_observer(checked)) return reject(GalacticRejectReason::observer_input);
    const auto table = od::presence(staged, sorted_fleets(), movement_inputs.get(), actor_inputs());
    std::vector<GalacticEvent> notifications;
    for (std::size_t i = 0; i < staged.records.size(); ++i) {
        auto& r = staged.records[i];
        const bool targeted = (!c.planet || targets.contains(r.planet))
            && (c.action != ObserverAction::refresh || !c.player || c.player == r.player)
            && (c.spy_effect || c.action == ObserverAction::update_planet || c.player == r.player);
        if (!targeted || ((c.action == ObserverAction::remove) && !changed_targets.contains(r.planet))) continue;
        // WGUI-21/22: successful spy changes refresh every observer at each affected planet.
        od::refresh(r, staged.players[i / staged.planets.size()], staged.planets[i % staged.planets.size()], table, c.gameplay_frame, c.clear_local);
        notifications.push_back({tick, GalacticEventKind::intelligence_refreshed, command.key.seat, r.planet, command.key.sequence, r.player, GalacticRejectReason::none});
    }
    if (*observer != staged) invalidate_observer_projection(false);
    *observer = std::move(staged); dirty[static_cast<std::size_t>(StateSection::observer)] = true;
    events.push_back(event); events.insert(events.end(), notifications.begin(), notifications.end());
}
} // namespace eawr::sim::galactic
