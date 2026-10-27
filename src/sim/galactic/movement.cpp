#include "eawr/sim/galactic/raid.hpp"
#include "eawr/sim/galactic/movement.hpp"
#include "session_impl.hpp"
#include "encoding.hpp"
#include "eawr/sim/galactic/corruption_model.hpp"
#include "native_movement_admission.hpp"
#include "native_control_admission.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace eawr::sim::galactic {
core::Result<void> GalacticSession::apply_battle_credits(const std::span<const std::pair<PlayerId, math::Fixed>> balances) {
    if (impl_->failed || !impl_->economy) return core::Result<void>::failure(detail::diagnostic(
        diagnostic_codes::invalid_setup, "tactical credit return requires a campaign ledger"));
    auto players = impl_->economy->players;
    PlayerId previous{};
    for (const auto& [player, credits] : balances) {
        const auto at = std::find_if(players.begin(), players.end(), [&](const auto& row) { return row.player == player; });
        if (!player || player <= previous || at == players.end() || credits.raw() < 0)
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "invalid tactical credit return"));
        previous = player; at->credits = std::min(credits, at->credit_cap);
    }
    impl_->economy->players = std::move(players);
    impl_->dirty[static_cast<std::size_t>(StateSection::players)] = true;
    const auto events = impl_->snapshot->events();
    impl_->snapshot = impl_->make_snapshot(std::vector<GalacticEvent>(events.begin(), events.end()));
    return core::Result<void>::success();
}
namespace {
using math::Fixed;
using math::Vec3;
const auto one = Fixed::from_raw(Fixed::scale);
struct InvalidMovement { core::Diagnostic diagnostic; };
template<class T> T checked(core::Result<T> result) {
    if (!result) throw InvalidMovement{result.error()};
    return std::move(result).value();
}
[[noreturn]] void invalid(const char* message) {
    throw InvalidMovement{detail::diagnostic(diagnostic_codes::invalid_setup, message)};
}
template<class T, class K, class Key> const T& row(const std::vector<T>& rows, K id, Key key) {
    const auto found = std::lower_bound(rows.begin(), rows.end(), id, [&](const T& item, K value) { return key(item) < value; });
    if (found == rows.end() || key(*found) != id) invalid("missing explicit movement input");
    return *found;
}
const MovementType& type(const MovementSetup& s, TypeId id) { return row(s.types, id, [](const auto& r) { return r.type; }); }
const MovementPlayer& player(const MovementSetup& s, PlayerId id) { return row(s.players, id, [](const auto& r) { return r.player; }); }
const MovementPlanet& planet(const MovementSetup& s, PlanetId id) { return row(s.planets, id, [](const auto& r) { return r.planet; }); }
const MovementAccess& access(const MovementSetup& s, PlayerId owner, PlanetId id) {
    return row(s.access, std::pair(owner, id), [](const auto& r) { return std::pair(r.player, r.planet); });
}
Vec3 subtract(Vec3 a, Vec3 b) { return {checked(math::subtract(a.x, b.x)), checked(math::subtract(a.y, b.y)), checked(math::subtract(a.z, b.z))}; }
Vec3 add(Vec3 a, Vec3 b) { return {checked(math::add(a.x, b.x)), checked(math::add(a.y, b.y)), checked(math::add(a.z, b.z))}; }
Vec3 scale(Vec3 v, Fixed factor) { return {checked(math::multiply(v.x, factor)), checked(math::multiply(v.y, factor)), checked(math::multiply(v.z, factor))}; }
const std::vector<FleetMember>& children(const MovementSetup& s, EntityId id) {
    return row(s.contents, id, [](const auto& r) { return r.container; }).children;
}
template<class Visit> void flatten(const MovementSetup& s, const FleetMember& m, Visit visit, unsigned depth = 0) {
    if (depth > 32) invalid("live movement container nesting exceeds limit");
    const auto& t = type(s, m.type);
    if (t.squadron || t.fleet_container) { for (const auto& child : children(s, m.id)) flatten(s, child, visit, depth + 1); }
    else visit(m, t);
}
bool skips_combat(const MovementSetup& s, const std::vector<FleetMember>& members) {
    bool any = false, all = true;
    const auto visit = [&](auto&& self, const FleetMember& m, unsigned depth) -> void {
        if (depth > 32) invalid("live movement container nesting exceeds limit");
        const auto& t = type(s, m.type);
        if (t.squadron || t.fleet_container || t.transport) {
            const auto& contents = children(s, m.id);
            // WGS-92 / WGM-38: empty transports defeat fleet-wide skipping.
            if (t.transport && contents.empty()) { any = true; all = false; }
            for (const auto& child : contents) self(self, child, depth + 1);
        } else { any = true; all = all && t.skips_combat; }
    };
    for (const auto& m : members) visit(visit, m, 0);
    return any && all; // WGM-38: nonempty and every member
}
void install_hop(FleetMotion& motion, const MovementSetup& s, PlayerId owner,
    const std::map<PlanetId, Vec3>& positions, std::span<const Route> routes) {
    auto& transit = *motion.transit;
    const auto from = transit.path.at(transit.hop), to = transit.path.at(transit.hop + 1);
    const auto direction = checked(math::normalize(subtract(positions.at(to), positions.at(from))));
    transit.desired_facing = direction;
    // WGM-28: offsets use model bounds along the connection, not the path cost.
    motion.position = add(positions.at(from), scale(direction, planet(s, from).bounds_half_z));
    transit.desired = subtract(positions.at(to), scale(direction, planet(s, to).bounds_half_z));
    auto multiplier = one;
    const auto& p = player(s, owner);
    if (p.benefits_from_corruption && planet(s, from).corrupted && planet(s, to).corrupted)
        multiplier = std::max(multiplier, p.corruption_bonus);
    for (const auto& route : routes) if ((route.from == from && route.to == to) || (route.from == to && route.to == from)) {
        multiplier = std::max(multiplier, route.hs_speed_factor); break;
    }
    transit.speed = checked(math::multiply(transit.base_speed, multiplier));
    transit.velocity = {};
    if (transit.speed.raw() <= 0) invalid("nonpositive hop speed");
}

template<class T, class Key> bool ordered(const std::vector<T>& rows, Key key) {
    return std::adjacent_find(rows.begin(), rows.end(), [&](const T& a, const T& b) { return !(key(a) < key(b)); }) == rows.end();
}
} // namespace

void GalacticSession::Impl::notify_fleet_approach(FleetId id, PlanetId destination,
    bool notify, std::size_t preceding_events) {
    // WGUI-114/122: committed enroute assignment, never an arrival/snapshot delta.
    if (!notify || !destination || !feedback_notice_handler || !observer || !control
        || !production || !production->nested_creation || !fleets.contains(id)) return;
    const auto at = std::lower_bound(control->state.begin(), control->state.end(), destination,
        [](const auto& state, PlanetId planet) { return state.planet < planet; });
    if (at == control->state.end() || at->planet != destination) return;
    const auto local = at->current_player;
    if (!std::binary_search(observer->human_observers.begin(), observer->human_observers.end(), local)) return;
    const auto& fleet = registry.get<session_detail::FleetData>(fleets.at(id));
    const auto& relations = production->nested_creation->relations;
    if (std::none_of(relations.begin(), relations.end(), [&](const auto& r) {
        return r.player == fleet.owner && r.other == local && r.enemy;
    })) return;
    const auto actors = actor_inputs();
    if (actors.empty()) {
        if (!movement_inputs || skips_combat(*movement_inputs, fleet.members)) return;
    } else {
        const auto skip = fleet_combat_skip(Fleet{id, fleet.owner, 0, fleet.members}, actors, movement_inputs.get());
        if (!skip || skip.value()) return;
    }
    const bool own_orbit = std::any_of(fleets.begin(), fleets.end(), [&](const auto& entry) {
        const auto& other = registry.get<session_detail::FleetData>(entry.second);
        return other.owner == local && other.orbit == destination && !other.motion.transit;
    });
    if (own_orbit) feedback_notice_handler({GalacticFeedbackNotice::Kind::enemy_approaching,
        local, destination, preceding_events});
}

core::Result<FleetDeparture> prepare_fleet_departure(const Fleet& fleet, const MovementSetup& inputs) {
    try {
        FleetDeparture out;
        struct Candidate { EntityId id; Fixed cost, speed; };
        std::vector<Candidate> slow, fast;
        std::uint64_t holds = 0;
        for (const auto& member : fleet.members) {
            const auto& t = type(inputs, member.type);
            // WGM-87: only direct non-container capacities count.
            if (!t.squadron && !t.fleet_container) holds += t.squadron_capacity;
            if (t.squadron) {
                // WGM-118/151: first fighter classifies; current squadron type ranks.
                const auto& fighters = children(inputs, member.id);
                if (fighters.empty()) invalid("departure squadron has no first fighter");
                const auto& first = type(inputs, fighters.front().type);
                (first.hyperspace_eligible ? fast : slow).push_back({member.id, t.build_cost, t.hyperspace_speed});
            }
        }
        // WGM-92: zero holds bypasses packing AND the left-behind branch.
        if (holds != 0) {
            // WGM-118: equal keys retain encounter order in the candidate list;
            // cost packing traverses it backwards, speed packing forwards.
            std::stable_sort(slow.begin(), slow.end(), [](const auto& a, const auto& b) { return a.cost < b.cost; });
            std::reverse(slow.begin(), slow.end());
            std::stable_sort(fast.begin(), fast.end(), [](const auto& a, const auto& b) { return a.speed < b.speed; });
            const auto pack = [&](const auto& candidates, const bool by_cost) {
                for (const auto& c : candidates) {
                    if (holds != 0) { --holds; out.packed.push_back(c.id); }
                    else if (by_cost) out.left_behind.push_back(c.id);
                }
            };
            pack(slow, true); pack(fast, false);
        }
        std::optional<Fixed> minimum;
        std::size_t departing = 0;
        for (const auto& member : fleet.members) {
            if (std::find(out.left_behind.begin(), out.left_behind.end(), member.id) != out.left_behind.end()) continue;
            ++departing;
            if (std::find(out.packed.begin(), out.packed.end(), member.id) != out.packed.end()) continue;
            flatten(inputs, member, [&](const auto&, const auto& t) {
                if (t.hyperspace_eligible && (!minimum || t.hyperspace_speed < *minimum)) minimum = t.hyperspace_speed;
            });
        }
        if (departing == 0) invalid("empty departing fleet");
        // LSA-10/11/13: accept authored signs. A nonpositive speed input cannot
        // advance a forward leg; clamp at consumption so two negatives cannot make a positive speed.
        out.base_speed = checked(math::multiply(checked(math::multiply(std::max(minimum.value_or(Fixed{}), Fixed{}),
            std::max(player(inputs, fleet.owner).hyperspace_factor, Fixed{}))), std::max(inputs.mode_modifier, Fixed{})));
        if (out.base_speed.raw() <= 0) invalid("fleet has no positive base hyperspace speed");
        // These are membership sets, not transfer order. Canonicalize equal-key groups
        // whose whole group was packed or left behind; only cut-off ties are unverified.
        std::sort(out.packed.begin(), out.packed.end());
        std::sort(out.left_behind.begin(), out.left_behind.end());
        return core::Result<FleetDeparture>::success(std::move(out));
    } catch (const InvalidMovement& error) { return core::Result<FleetDeparture>::failure(error.diagnostic); }
}

core::Result<void> validate_movement_setup(const GalacticSetup& setup) {
    if (!setup.movement) {
        for (const auto& f : setup.fleets) if (f.motion != FleetMotion{})
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "fleet motion requires movement inputs"));
        return core::Result<void>::success();
    }
    try {
        if (!setup.fleet_control || !setup.rules.maximum_fleet_movement_distance) invalid("movement requires fleet control and graph rules");
        const auto& s = *setup.movement;
        if (s.types.size() > galactic_max_objects || s.contents.size() > galactic_max_objects
            || s.access.size() > galactic_max_players * 4096
            || !ordered(s.types, [](const auto& r) { return r.type; })
            || !ordered(s.contents, [](const auto& r) { return r.container; })
            || !ordered(s.players, [](const auto& r) { return r.player; })
            || !ordered(s.planets, [](const auto& r) { return r.planet; })
            || !ordered(s.access, [](const auto& r) { return std::pair(r.player, r.planet); })) invalid("invalid movement table order or limit");
        if (s.players.size() != setup.players.size() || s.planets.size() != setup.planets.size()
            || s.access.size() != setup.players.size() * setup.planets.size()) invalid("movement observer tables must be complete");
        for (const auto& p : setup.players) {
            const auto& r = player(s, p.player_id);
            (void)r; // LSA-11/12: retain signed authored factors; movement consumes them below.
            for (const auto& q : setup.planets) (void)access(s, p.player_id, q.id);
        }
        for (const auto& p : setup.planets) if (planet(s, p.id).bounds_half_z.raw() < 0) invalid("negative planet movement bounds");
        for (const auto& t : s.types) if (t.type == 0
            || (t.squadron && t.fleet_container) || (t.transport && (t.squadron || t.fleet_container))) invalid("invalid authored movement type");
        if (s.raid) {
            if (!setup.control || s.raid->types.size() != s.types.size()
                || !ordered(s.raid->types, [](const auto& r) { return r.type; })) invalid("raid requires complete ordered type policy and control");
            for (std::size_t i = 0; i < s.types.size(); ++i)
                if (s.raid->types[i].type != s.types[i].type) invalid("raid type identities differ from movement");
        }
        std::set<EntityId> identities, containers;
        for (const auto& p : setup.planets) identities.insert(p.id);
        for (const auto& r : setup.routes) identities.insert(r.id);
        for (const auto& f : setup.fleets) { identities.insert(f.id); for (const auto& m : f.members) identities.insert(m.id); }
        for (const auto& u : setup.fleet_control->detached_units) identities.insert(u.member.id);
        std::size_t objects = identities.size();
        for (const auto& c : s.contents) for (const auto& child : c.children) {
            if (child.id == 0 || child.id == std::numeric_limits<EntityId>::max() || !identities.insert(child.id).second || ++objects > galactic_max_objects)
                invalid("invalid live movement child identity or limit");
            (void)type(s, child.type);
        }
        const auto inspect = [&](const auto& self, const FleetMember& m, unsigned depth) -> void {
            if (depth > 32) invalid("live movement container depth exceeds limit");
            const auto& t = type(s, m.type);
            if (t.squadron || t.fleet_container || t.transport) {
                if (!containers.insert(m.id).second) invalid("live movement container cycle or duplicate parent");
                for (const auto& child : children(s, m.id)) self(self, child, depth + 1);
            }
        };
        for (const auto& f : setup.fleets) {
            if (f.motion.transit || f.motion.orbit_entry != 0) invalid("tick-zero fleets must be in orbit without runtime orbit order; transit restores from checkpoint history");
            for (const auto& m : f.members) inspect(inspect, m, 0);
        }
        for (const auto& u : setup.fleet_control->detached_units) inspect(inspect, u.member, 0);
        if (setup.control) for (const auto& planet : setup.control->planets) for (const auto& force : planet.landed)
            inspect(inspect, FleetMember{force.id, force.type}, 0);
        if (containers.size() != s.contents.size()) invalid("orphan live movement contents");
        return core::Result<void>::success();
    } catch (const InvalidMovement& error) { return core::Result<void>::failure(error.diagnostic); }
}

session_detail::NativeBribeQuote session_detail::quote_native_bribe(PlayerId mover, std::span<const FleetMember> departing,
    std::span<const PlanetId> path, const MovementSetup& movement,
    const std::function<std::pair<PlayerId, PlayerId>(PlanetId)>& planet_state) {
    NativeBribeQuote quote;
    try {
        if (path.size() < 3 || !player(movement, mover).benefits_from_corruption) return quote;
        std::map<PlayerId, Fixed> owed;
        for (std::size_t i = 1; i + 1 < path.size(); ++i) {
            const auto [owner, occupant] = planet_state(path[i]);
            if (!owner || occupant == mover) continue; // WGM-85: the mover's own orbit is free; nobody to pay on an unowned gap
            const bool cheap = player(movement, owner).easily_bribed;
            Fixed cost{};
            for (const auto& member : departing) flatten(movement, member, [&](const FleetMember&, const MovementType& leaf) {
                cost = checked(math::add(cost, cheap ? leaf.cheap_bribe : leaf.expensive_bribe));
            });
            if (cost <= Fixed{}) continue;
            auto& slot = owed[owner];
            slot = checked(math::add(slot, cost));
            quote.total = checked(math::add(quote.total, cost));
        }
        quote.recipients.assign(owed.begin(), owed.end());
    } catch (const InvalidMovement&) { quote.invalid = true; }
    return quote;
}

session_detail::NativeMovementAdmission session_detail::prepare_native_movement(
    const Fleet& f, const GameCommand& command, const MovementSetup& movement, const FleetControlSetup& traits,
    const PlanetGraph& graph, std::span<const Planet> planets, std::span<const Route> routes,
    EntityId next_id, std::size_t live_object_count, bool returning_player) {
    const auto& move = std::get<MoveFleet>(command.payload);
    if (move.destination_slot) {
        bool qualifying = false;
        try {
            const auto qualifies = [&](const FleetMember& unit) {
                const auto& trait = row(traits.types, unit.type, [](const auto& r) { return r.type; });
                return !trait.transport && trait.can_hyperspace_without_ability;
            };
            for (const auto& member : f.members) flatten(movement, member, [&](const auto& leaf, const auto& leaf_type) {
                qualifying = qualifying || qualifies(leaf);
                // WGM-83 (debug build): the fleet breakdown also lists a transport's carried units, one level.
                if (leaf_type.transport) for (const auto& carried : children(movement, leaf.id))
                    qualifying = qualifying || qualifies(carried);
            });
        } catch (const InvalidMovement&) { return {GalacticRejectReason::invalid_membership, {}, {}}; }
        if (!qualifying) return {GalacticRejectReason::movement_ability_required, {}, {}};
    }
    if (move.path.size() < 2 || move.path.front() != f.orbit || move.path.back() == f.orbit) return {GalacticRejectReason::invalid_path, {}, {}};
    // WGM-198: retreat supplies its selected endpoints directly, outside the
    // ordinary player path search and its authored graph-distance constraint.
    for (std::size_t i = 1; !returning_player && i < move.path.size(); ++i) {
        const auto* node = graph.node(move.path[i-1]);
        if (!node || std::none_of(node->edges.begin(), node->edges.end(), [&](const auto& e) { return e.to == move.path[i]; })) return {GalacticRejectReason::invalid_path, {}, {}};
    }
    const auto prepared = prepare_fleet_departure({move.fleet, f.owner, f.orbit, f.members, f.orbit_slot, f.motion}, movement);
    if (!prepared) return {GalacticRejectReason::cannot_depart, {}, {}};
    if (!prepared.value().left_behind.empty() && (next_id == std::numeric_limits<EntityId>::max() || live_object_count >= galactic_max_objects)) return {GalacticRejectReason::resource_limit, {}, {}};
    auto motion = f.motion;
    motion.transit = FleetTransit{move.path, 0, {}, {}, prepared.value().base_speed, {}, prepared.value().packed, move.destination_slot, command.key.seat, command.key.sequence};
    motion.transit->destination_actor = move.destination_actor;
    try {
        std::map<PlanetId, Vec3> positions; for (const auto& p : planets) positions.emplace(p.id, p.position);
        install_hop(motion, movement, f.owner, positions, routes);
    } catch (const InvalidMovement&) { return {GalacticRejectReason::cannot_depart, {}, {}}; }
    return {GalacticRejectReason::none, prepared.value(), std::move(motion)};
}

void GalacticSession::Impl::accrue_departure_piracy(FleetId id, PlayerId fleet_owner, PlanetId source,
    const std::vector<FleetMember>& departing) {
    if (!economy_corruption || !movement_inputs) return;
    auto row = std::lower_bound(economy_corruption->planets.begin(), economy_corruption->planets.end(), source,
        [](const auto& at, PlanetId value) { return at.planet < value; });
    if (row == economy_corruption->planets.end() || row->planet != source
        || row->corruption != corruption_fixture::Type::piracy) return;
    // WGEP-113/WGCH-161: the current first corruption beneficiary, in player-list order.
    // WGEP-112: the beneficiary must also be fiscally eligible (rows are parallel to the fiscal player list).
    const auto& beneficiaries = economy_corruption->players;
    const auto beneficiary = std::find_if(beneficiaries.begin(), beneficiaries.end(), [&](const auto& player) {
        const auto index = static_cast<std::size_t>(&player - beneficiaries.data());
        return player.benefits && setup.fiscal && index < setup.fiscal->players.size()
            && setup.fiscal->players[index].earns_planet_income;
    });
    if (beneficiary == beneficiaries.end()) return;
    std::vector<Fixed> values;
    for (const auto& member : departing)
        flatten(*movement_inputs, member, [&](const FleetMember&, const MovementType& leaf) { values.push_back(leaf.piracy_value); });
    if (std::none_of(values.begin(), values.end(), [](Fixed value) { return value > Fixed{}; })) return;
    bool allied = fleet_owner == beneficiary->player;
    if (!allied && control) {
        const auto relation = std::find_if(control->relations.begin(), control->relations.end(),
            [&](const auto& pair) { return pair.player == fleet_owner && pair.other == beneficiary->player; });
        allied = relation != control->relations.end() && relation->allied;
    }
    if (allied) return;
    // WGM-38: stealth is the same live combat-skip predicate.
    const auto actors = actor_inputs();
    const bool stealth = actors.empty() ? skips_combat(*movement_inputs, departing)
        : checked(fleet_combat_skip(Fleet{id, fleet_owner, 0, departing}, actors, movement_inputs.get()));
    const auto accrued = checked(corruption_fixture::accrue_piracy(row->corruption, allied, stealth, row->piracy, values));
    if (accrued == row->piracy) return;
    row->piracy = accrued;
    dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
}

std::pair<PlayerId, PlayerId> GalacticSession::Impl::bribe_planet_state(PlanetId id) const {
    const auto at = planets.find(id);
    if (at == planets.end()) return {};
    PlayerId occupant{};
    if (control) {
        const auto row = std::lower_bound(control->state.begin(), control->state.end(), id, [](const auto& r, PlanetId v) { return r.planet < v; });
        if (row != control->state.end() && row->planet == id) occupant = row->orbital_player;
    } else if (production) {
        const auto row = std::lower_bound(production->planets.begin(), production->planets.end(), id, [](const auto& r, PlanetId v) { return r.planet < v; });
        if (row != production->planets.end() && row->planet == id) occupant = row->orbit_occupier;
    }
    return {registry.get<session_detail::PlanetData>(at->second).owner, occupant};
}

GalacticRejectReason GalacticSession::Impl::settle_movement_bribe(PlayerId payer,
    const std::vector<std::pair<PlayerId, Fixed>>& recipients, Fixed total) {
    if (!economy) return GalacticRejectReason::insufficient_credits;
    auto accounts = economy->players;
    const auto find = [&](PlayerId id) { return std::find_if(accounts.begin(), accounts.end(), [id](const auto& r) { return r.player == id; }); };
    const auto from = find(payer);
    if (from == accounts.end() || from->credits < total) return GalacticRejectReason::insufficient_credits;
    auto debited = math::subtract(from->credits, total);
    if (!debited) return GalacticRejectReason::insufficient_credits;
    from->credits = debited.value();
    for (const auto& [id, amount] : recipients) {
        const auto to = find(id);
        if (to == accounts.end()) continue; // a recipient without an account just absorbs the payment
        auto credited = math::add(to->credits, amount);
        if (!credited) return GalacticRejectReason::insufficient_credits;
        to->credits = credited.value();
    }
    economy->players = std::move(accounts);
    dirty[static_cast<std::size_t>(StateSection::players)] = true;
    return GalacticRejectReason::none;
}

bool GalacticSession::Impl::raid_qualified(FleetId id, PlanetId destination) const {
    const auto& f = registry.get<session_detail::FleetData>(fleets.at(id));
    return control && movement_inputs && qualifies_raid(
        Fleet{id, f.owner, f.orbit, f.members, f.orbit_slot, f.motion, f.raid_destination}, destination,
        *movement_inputs, *control, actor_inputs(), planet_access.get());
}

void GalacticSession::Impl::execute_movement(const GameCommand& command, std::uint64_t tick,
    std::vector<GalacticEvent>& events, bool pending_local_conflict, std::optional<PlayerId> returning_player) {
    session_detail::execute_native_movement(*this, command, tick, events, pending_local_conflict, returning_player);
    // WGUI-122: only the accepted departure assigns its final enroute identity
    // with notify=true. Shadow admissions have no external feedback side effects;
    // their eventual ordered native commit enters this same owner.
    if (!events.empty() && events.back().kind == GalacticEventKind::movement_begin) {
        const auto& move = std::get<MoveFleet>(command.payload);
        notify_fleet_approach(move.fleet, move.path.back(), true, events.size());
    }
}

core::Result<void> GalacticSession::apply_retreat_return(PlayerId player, PlanetId source,
    PlanetId destination, bool surface, std::span<const FleetId> orbital_fleets, std::span<const EntityId> confirmed_destroyed) {
    auto& impl = *impl_;
    const auto invalid_return = [](std::string message) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, std::move(message)));
    };
    if (impl.failed || !impl.setup.movement || !impl.setup.fleet_control || !impl.graph
        || !impl.planets.contains(source) || !impl.planets.contains(destination)
        || std::none_of(impl.setup.players.begin(), impl.setup.players.end(), [&](const auto& row) { return row.player_id == player; }))
        return invalid_return("retreat requires live movement, endpoints and player");
    std::set<FleetId> unique;
    for (const auto id : orbital_fleets) {
        if (!unique.insert(id).second || !impl.fleets.contains(id)) return invalid_return("invalid retreat fleet identity");
        const auto& fleet = impl.registry.get<session_detail::FleetData>(impl.fleets.at(id));
        if (fleet.owner != player || fleet.orbit != source || fleet.motion.transit)
            return invalid_return("retreat fleet differs from its source orbit");
    }
    if (confirmed_destroyed.size() > galactic_max_objects || !std::is_sorted(confirmed_destroyed.begin(), confirmed_destroyed.end())
        || std::adjacent_find(confirmed_destroyed.begin(), confirmed_destroyed.end()) != confirmed_destroyed.end()
        || (!confirmed_destroyed.empty() && !confirmed_destroyed.front()))
        return invalid_return("invalid confirmed retreat casualty view");
    const auto living = [&](const SurfaceForce& force) {
        return force.owner == player && !std::binary_search(confirmed_destroyed.begin(), confirmed_destroyed.end(), force.id);
    };
    std::vector<FleetId> departing(orbital_fleets.begin(), orbital_fleets.end());
    std::vector<GalacticEvent> events(impl.snapshot->events().begin(), impl.snapshot->events().end());
    if (surface) {
        if (!impl.control) return invalid_return("surface retreat needs control presence");
        auto planet = std::find_if(impl.control->planets.begin(), impl.control->planets.end(), [&](const auto& row) { return row.planet == source; });
        if (planet == impl.control->planets.end()) return invalid_return("surface retreat needs its planet presence");
        Fleet fleet; fleet.id = impl.next_id; fleet.owner = player; fleet.orbit = source;
        for (const auto& force : planet->landed) if (living(force)) fleet.members.push_back({force.id, force.type});
        // WGM-U10: an isolated surface fallback has not been qualified.
        if (!fleet.members.empty() && source == destination) return invalid_return("unqualified surface retreat without alternative destination (WGM-U10)");
        if (!fleet.members.empty()) {
            if (impl.next_id == std::numeric_limits<EntityId>::max() || impl.live_object_count >= galactic_max_objects)
                return invalid_return("surface retreat allocation limit");
            impl.insert(fleet); ++impl.next_id; ++impl.live_object_count;
            impl.record_fleet_shell(fleet.id, player, source);
            for (const auto& member : fleet.members) impl.relocate_actor(member.id, player, source, fleet.id);
            std::erase_if(planet->landed, living);
            std::vector<EntityId> departed;
            for (const auto& member : fleet.members) departed.push_back(member.id);
            impl.unpublish_surface_departures(source, departed);
            departing.insert(departing.begin(), fleet.id);
            events.push_back({impl.completed_tick, GalacticEventKind::fleet_created, {}, fleet.id, 0, source, GalacticRejectReason::none});
            impl.dirty[static_cast<std::size_t>(StateSection::control)] = true;
            impl.dirty[static_cast<std::size_t>(StateSection::session)] = true;
            impl.dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
            impl.population_stale = true;
        }
    }
    for (const auto id : departing) {
        // WGM-61/99: two endpoints, including the sourced orbital fallback.
        GameCommand command; command.payload = MoveFleet{id, {source, destination}, {}};
        impl.execute_movement(command, impl.completed_tick, events, false, player);
    }
    impl.invalidate_observer_projection();
    impl.snapshot = impl.make_snapshot(std::move(events));
    return impl.deliver_actor_changes();
}

void GalacticSession::set_movement_access_provider(MovementAccessProvider provider) {
    impl_->movement_access_provider = std::move(provider);
    impl_->movement_access_generation = std::numeric_limits<std::uint64_t>::max();
}
void GalacticSession::set_native_movement_listener(NativeMovementListener listener) {
    impl_->native_movement_listener = std::move(listener);
}
void GalacticSession::Impl::link_movement_orbit(FleetId id, PlanetId destination,
    std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto visit = [&](auto consume) {
        for (const auto& [other_id, handle] : fleets) consume(other_id, registry.get<session_detail::FleetData>(handle));
    };
    checked(session_detail::link_native_movement_orbit(*this, visit, id, destination, tick, events));
}
void GalacticSession::Impl::gather_movement_orbit(FleetId id, PlanetId destination,
    std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto visit = [&](auto consume) {
        for (const auto& [other_id, handle] : fleets) consume(other_id, registry.get<session_detail::FleetData>(handle));
    };
    checked(session_detail::gather_native_movement_orbit(*this, visit, id, destination, tick, events));
}
core::Result<bool> session_detail::native_orbit_conflict(const Fleet& fleet,
    std::optional<std::span<const CensusObject>> actors, const MovementSetup& movement, const MovementSetup* contents) {
    try {
        const auto& permitted = access(movement, fleet.owner, fleet.orbit);
        const auto skip = !actors ? skips_combat(movement, fleet.members)
            : checked(fleet_combat_skip(fleet, *actors, contents ? contents : &movement));
        return core::Result<bool>::success(permitted.accessible && permitted.potential_conflict && !skip);
    } catch (const InvalidMovement& error) { return core::Result<bool>::failure(error.diagnostic); }
}

core::Result<void> GalacticSession::Impl::recheck_movement_orbit(EntityId target,
    std::uint64_t tick, std::vector<GalacticEvent>& events) {
    // WGM-154: planets select the first live orbit-list entry, fleets need a planet parent.
    FleetId id = target;
    if (planets.contains(target)) {
        id = 0; auto first = std::numeric_limits<std::uint64_t>::max();
        for (const auto& [candidate, handle] : fleets) {
            const auto& fleet = registry.get<session_detail::FleetData>(handle);
            if (fleet.orbit == target && !fleet.motion.transit && fleet.motion.orbit_entry < first) {
                id = candidate; first = fleet.motion.orbit_entry;
            }
        }
        if (!id) return core::Result<void>::success();
    } else if (!fleets.contains(id)) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "orbit recheck requires a fleet or planet"));
    }
    auto& fleet = registry.get<session_detail::FleetData>(fleets.at(id));
    if (!fleet.orbit || fleet.motion.transit || fleet.raid_destination == fleet.orbit) return core::Result<void>::success();
    if (!movement_inputs) return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "orbit recheck requires movement inputs"));
    try {
        const auto destination = fleet.orbit;
        const auto actors = actor_inputs();
        const auto conflict = checked(session_detail::native_orbit_conflict(
            Fleet{id, fleet.owner, fleet.orbit, fleet.members}, actors.empty() ? std::nullopt
                : std::optional<std::span<const CensusObject>>(actors), *movement_inputs));
        link_movement_orbit(id, destination, tick, events);
        if (conflict) {
            gather_movement_orbit(id, destination, tick, events);
            events.push_back({tick, GalacticEventKind::movement_conflict, {}, id, 0, destination, GalacticRejectReason::none});
            if (native_movement_listener) native_movement_listener(events.back());
        }
        return core::Result<void>::success();
    } catch (const InvalidMovement& error) { return core::Result<void>::failure(error.diagnostic); }
}

core::Result<void> GalacticSession::recheck_native_orbit(PlayerId owner, FleetId id) {
    if (auto* shadow = native_shadow()) return stage_native_orbit_recheck(*shadow, owner, id);
    auto& impl = *impl_;
    if (impl.failed || !impl.movement_inputs || !impl.fleets.contains(id))
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "native orbit recheck requires its live movement actor"));
    auto& fleet = impl.registry.get<session_detail::FleetData>(impl.fleets.at(id));
    if (fleet.owner != owner || !fleet.orbit || fleet.motion.transit)
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "native orbit recheck differs from its owned planet parent"));
    std::vector<GalacticEvent> events(impl.snapshot->events().begin(), impl.snapshot->events().end());
    if (auto checked = impl.recheck_movement_orbit(id, impl.completed_tick, events); !checked) return checked;
    impl.snapshot = impl.publish_native_snapshot(std::move(events));
    if (!impl.native_applying) if (auto delivered = impl.deliver_actor_changes(); !delivered) return delivered;
    return impl.journal_native_receipt(*this, NativeEffectKind::orbit_recheck, owner, id, 0, false, nullptr, {true, id});
}
core::Result<void> GalacticSession::refresh_movement_access(const PartitionExecutor& executor) {
    auto& impl = *impl_;
    if (!impl.movement_access_provider || !impl.movement_inputs) return core::Result<void>::success();
    if (impl.movement_access_generation == impl.observer_input_generation) return core::Result<void>::success();
    // Current immutable presence, without observer/forecast publication work.
    GalacticSnapshot snapshot(impl.completed_tick, impl.clock, impl.setup.players,
        impl.sorted_planets(), impl.sorted_routes(), impl.sorted_fleets(), {},
        impl.setup.rules.maximum_fleet_movement_distance, impl.fleet_control_state(),
        impl.movement_inputs, impl.seat_control, impl.seats, impl.economy, impl.production, impl.control);
    auto access = impl.movement_access_provider(snapshot, executor);
    if (!access) return core::Result<void>::failure(access.error());
    const auto& previous = impl.movement_inputs->access;
    if (access.value().size() != previous.size()) return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "incomplete runtime movement access"));
    for (std::size_t i = 0; i < previous.size(); ++i)
        if (previous[i].player != access.value()[i].player || previous[i].planet != access.value()[i].planet)
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "runtime movement access changed its identity table"));
    if (access.value() != previous) {
        auto movement = std::make_shared<MovementSetup>(*impl.movement_inputs);
        movement->access = std::move(access).value(); impl.movement_inputs = std::move(movement);
        impl.dirty[static_cast<std::size_t>(StateSection::movement)] = true;
    }
    impl.movement_access_generation = impl.observer_input_generation;
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::Impl::service_movement(std::span<const session_detail::GatheredObject> gathered,
    const PartitionExecutor& executor, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    if (!setup.movement) return core::Result<void>::success();
    const auto& inputs = *movement_inputs;
    struct Stage { FleetMotion motion; bool active{}, arrival{}; std::optional<core::Diagnostic> error; };
    std::vector<Stage> staged(gathered.size());
    // Immutable copied gather inputs; each worker owns one output slot. No registry writes.
    const auto phase = executor.execute_phase("galactic-locomotion", tick_partition_count, [&](std::size_t partition) {
        const auto range = partition_range(partition, gathered.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& object = gathered[i]; auto& out = staged[i];
            if (object.kind != session_detail::ObjectKind::fleet || !object.fleet.motion.transit) continue;
            out.active = true; out.motion = object.fleet.motion;
            try {
                auto& transit = *out.motion.transit;
                const auto delta = subtract(transit.desired, out.motion.position);
                const auto length = checked(math::length(delta));
                // Reached exactly on the previous advance: arrival still uses WGM-30's
                // strict threshold and retained velocity rather than normalizing zero.
                const auto direction = length.raw() == 0 ? out.motion.facing : checked(math::normalize(delta));
                if (transit.desired_facing != out.motion.facing) { out.motion.facing = transit.desired_facing; continue; } // WGM-29
                transit.velocity = scale(direction, transit.speed);
                out.motion.position = add(out.motion.position, transit.velocity);
                const auto remaining = checked(math::length(subtract(transit.desired, out.motion.position)));
                const auto threshold = checked(math::multiply(checked(math::length(transit.velocity)), Fixed::from_raw(2 * Fixed::scale)));
                if (remaining < threshold) { out.motion.position = transit.desired; out.arrival = true; }
            } catch (const InvalidMovement& error) { out.error = error.diagnostic; }
        }
    });
    if (!phase) return phase;
    for (const auto& s : staged) if (s.error) return core::Result<void>::failure(*s.error);
    std::map<PlanetId, Vec3> positions; for (const auto& p : sorted_planets()) positions.emplace(p.id, p.position);
    const auto route_rows = sorted_routes();
    const auto notify = [&](const FleetId id, const FleetTransit& t, GalacticEventKind kind, PlanetId p) {
        events.push_back({tick, kind, t.producer, id, t.sequence, p, GalacticRejectReason::none});
        if (native_movement_listener) native_movement_listener(events.back());
    };
    // Ordered serial commit: a conflicting arrival can finish other staged fleets. Those
    // outputs are then discarded, so stale motion cannot resurrect an already linked fleet.
    try {
        for (std::size_t i = 0; i < gathered.size(); ++i) {
            const auto& out = staged[i]; if (!out.active) continue;
            const auto id = gathered[i].id; if (!fleets.contains(id)) continue;
            auto& f = registry.get<session_detail::FleetData>(fleets.at(id));
            if (!f.motion.transit) continue;
            f.motion = out.motion;
            dirty[static_cast<std::size_t>(StateSection::movement)] = true;
            if (!out.arrival) continue;
            auto& t = *f.motion.transit; const auto destination = t.path.at(t.hop + 1);
            const bool final = t.hop + 2 == t.path.size();
            const auto& a = access(inputs, f.owner, destination);
            if (!a.accessible) {
                if (final) {
                    notify(id, t, GalacticEventKind::movement_bounced, destination);
                    t.path = {destination, t.path.at(t.hop)}; t.hop = 0; t.destination_slot.reset(); t.destination_actor = 0;
                    if (f.raid_destination) { f.raid_destination.reset(); dirty[static_cast<std::size_t>(StateSection::fleets)] = true; }
                } else ++t.hop;
                install_hop(f.motion, inputs, f.owner, positions, route_rows); continue;
            }
            const auto& p = player(inputs, f.owner);
            const auto actors = actor_inputs();
            const auto live_skip = actors.empty() ? skips_combat(inputs, f.members)
                : checked(fleet_combat_skip(Fleet{id, f.owner, f.orbit, f.members}, actors, &inputs));
            const bool normal_conflict = a.potential_conflict && !live_skip && (final || !p.benefits_from_corruption);
            const bool retained_raid = final && f.raid_destination == destination;
            const bool raid_arrival = retained_raid && control && qualifies_raid(
                Fleet{id, f.owner, f.orbit, f.members}, destination, inputs, *control, actors, planet_access.get());
            // WGM-34/39: current qualification is checked again at arrival. A
            // stale raid intent follows the ordinary orbital arrival path.
            if (retained_raid && !raid_arrival) {
                f.raid_destination.reset();
                dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
            }
            const bool conflict = !raid_arrival && normal_conflict;
            if (raid_arrival) {
                f.motion.land_on_arrival = false; // WGM-34: raid dispatch owns this landing.
                const auto completed = t;
                link_movement_orbit(id, destination, tick, events);
                std::vector<EntityId> selected;
                for (const auto& member : f.members) {
                    const auto trait = std::lower_bound(setup.fleet_control->types.begin(), setup.fleet_control->types.end(), member.type,
                        [](const auto& row, auto type_id) { return row.type < type_id; });
                    if (trait != setup.fleet_control->types.end() && trait->type == member.type && trait->transport) selected.push_back(member.id);
                }
                auto admitted = session_detail::admit_native_invasion_rows(*this, f.owner, id, selected, false);
                if (!admitted) return core::Result<void>::failure(admitted.error());
                if (!admitted.value().admitted) {
                    f.raid_destination.reset();
                    if (normal_conflict) {
                        gather_movement_orbit(id, destination, tick, events);
                        notify(id, completed, GalacticEventKind::movement_conflict, destination);
                    }
                    dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
                    invalidate_observer_projection();
                    dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
                    continue;
                }
                // The retained explicit list is consumed by the campaign land owner;
                // no orbital conflict or gather/merge runs for this destination.
                native_invasions.at(admitted.value().request).reader_released = true;
                if (!admitted.value().queued) {
                    native_invasions.erase(admitted.value().request);
                    f.raid_destination.reset();
                    execute_landing({{tick, completed.producer, completed.sequence}, LandFleet{id, destination}}, tick, events, false);
                }
                dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
                continue;
            }
            if (conflict) {
                f.motion.land_on_arrival = false; // GGS-06 / WGM-34: conflict consumes the request.
                const auto transit = t;
                link_movement_orbit(id, destination, tick, events);
                gather_movement_orbit(id, destination, tick, events);
                notify(id, transit, GalacticEventKind::movement_conflict, destination);
                dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
                invalidate_observer_projection();
                dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true; continue;
            }
            if (!final) { ++t.hop; install_hop(f.motion, inputs, f.owner, positions, route_rows); continue; }
            const auto requested = t.destination_slot;
            const auto completed = t;
            link_movement_orbit(id, destination, tick, events); dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
            if (requested) f.orbit_slot = *requested;
            // WGM-35: explicit slot zero is a notification seam, never an auto-merge.
            if (requested && *requested != 0) {
                FleetId target = 0;
                std::uint64_t first_entry = std::numeric_limits<std::uint64_t>::max();
                for (const auto& [other_id, handle] : fleets) {
                    const auto& other = registry.get<session_detail::FleetData>(handle);
                    if (other_id != id && other.orbit == destination && other.orbit_slot == *requested && other.motion.orbit_entry < first_entry) {
                        target = other_id; first_entry = other.motion.orbit_entry;
                    }
                }
                if (target && registry.get<session_detail::FleetData>(fleets.at(target)).owner == f.owner) {
                    auto& other = registry.get<session_detail::FleetData>(fleets.at(target));
                    while (!f.members.empty()) {
                        const auto member = f.members.back(); other.members.push_back(member); f.members.pop_back();
                        relocate_actor(member.id, other.owner, destination, target);
                        events.push_back({tick, GalacticEventKind::member_detached, completed.producer, member.id, completed.sequence, id, GalacticRejectReason::none});
                        events.push_back({tick, GalacticEventKind::member_attached, completed.producer, member.id, completed.sequence, target, GalacticRejectReason::none});
                    }
                    registry.destroy(fleets.at(id)); fleets.erase(id); --live_object_count;
                    lose_actor(id);
                    events.push_back({tick, GalacticEventKind::fleet_destroyed, {}, id, 0, 0, GalacticRejectReason::none});
                }
            }
            // WGM-34: final nonconflicting arrival only; relink/merge has refreshed live containers.
            // Transit was consumed by relink, so failure or actor loss cannot replay the action.
            if (completed.destination_actor) dispatch_ground_ability(completed.destination_actor, destination,
                completed.producer, completed.sequence, tick, events);
            // GGS-06/07: one attempt through the current surface owner, after orbital
            // arrival. Refusal or partial capacity leaves the remainder in orbit.
            if (fleets.contains(id)
                && registry.get<session_detail::FleetData>(fleets.at(id)).motion.land_on_arrival) {
                registry.get<session_detail::FleetData>(fleets.at(id)).motion.land_on_arrival = false;
                execute_landing({{tick, completed.producer, completed.sequence}, LandFleet{id, destination}}, tick, events, false);
            }
            invalidate_observer_projection(); // after the final slot/member/merge mutation
            dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
        }
    } catch (const InvalidMovement& error) { return core::Result<void>::failure(error.diagnostic); }
    return core::Result<void>::success();
}
} // namespace eawr::sim::galactic
