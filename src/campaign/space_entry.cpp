#include "eawr/campaign/space_entry.hpp"
#include "eawr/script/authoritative/clock_rng.hpp"
#include "eawr/sim/math/trig.hpp"
#include "eawr/sim/tactical/motion.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace eawr::campaign {
namespace {
namespace gc = sim::galactic;
namespace tt = sim::tactical;
using sim::math::Fixed;
using sim::math::Vec3;
template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic d;
    d.code = std::string(tt::diagnostic_codes::invalid_setup);
    d.message = "space entry: " + std::move(message);
    return core::Result<T>::failure(std::move(d));
}
std::string folded(std::string_view value) {
    std::string out(value);
    for (auto& c : out) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}
const SpaceEntryType* type_of(const SpaceEntryInputs& input, gc::TypeId id) {
    const auto at = std::find_if(input.types.begin(), input.types.end(), [id](const auto& t) { return t.galactic == id; });
    return at == input.types.end() ? nullptr : &*at;
}
const SpaceEntryType* named_type(const SpaceEntryInputs& input, std::string_view name) {
    const auto at = std::find_if(input.types.begin(), input.types.end(), [&](const auto& t) {
        return folded(t.tactical) == folded(name);
    });
    return at == input.types.end() ? nullptr : &*at;
}
std::vector<const skirmish::MapPlacement*> markers(const skirmish::StartInputs& map, std::string_view name) {
    std::vector<const skirmish::MapPlacement*> result;
    for (const auto& p : map.placements) if (p.marker && folded(p.type) == folded(name)) result.push_back(&p);
    return result;
}
bool usable(const skirmish::MapPlacement& p) {
    return p.position && p.orientation_degrees && p.orientation_degrees->x == Fixed{}
        && p.orientation_degrees->y == Fixed{};
}
core::Result<Fixed> distance(Vec3 a, Vec3 b) {
    Vec3 d;
    if (!sim::math::try_subtract(a.x, b.x, d.x) || !sim::math::try_subtract(a.y, b.y, d.y)
        || !sim::math::try_subtract(a.z, b.z, d.z)) return fail<Fixed>("marker distance overflow");
    return sim::math::length(d);
}
// WGT-07/08: stable type-identifier buckets, last-listed member first. This load-time
// pass is serial because each admission consumes the same population budget.
core::Result<std::vector<sim::EntityId>> split(const ConflictRecord& record,
    const SpaceEntryInputs& input, const SpaceEntrySide& side) {
    if (side.cap < Fixed{}) return fail<std::vector<sim::EntityId>>("negative faction cap");
    std::vector<sim::EntityId> selected;
    std::map<gc::TypeId, std::vector<const ConflictForce*>> buckets;
    auto cap = side.cap;
    std::int64_t transports{};
    std::vector<const ConflictForce*> roster;
    for (const auto id : input.roster_order) {
        const auto at = std::find_if(record.forces.begin(), record.forces.end(), [id](const auto& force) { return force.id == id; });
        if (at == record.forces.end() || std::find(roster.begin(), roster.end(), &*at) != roster.end())
            return fail<std::vector<sim::EntityId>>("invalid source roster order");
        roster.push_back(&*at);
    }
    for (const auto& force : record.forces) if (std::find(roster.begin(), roster.end(), &force) == roster.end()) roster.push_back(&force);
    for (const auto* row : roster) {
        const auto& force = *row;
        if (force.owner != side.player || force.id == record.station || force.role == ForceRole::escort) continue;
        const auto* type = type_of(input, force.type);
        if (!type || type->population < Fixed{}) return fail<std::vector<sim::EntityId>>("missing type or negative population");
        if (!type->space_allowed) continue;
        if (type->superweapon) { selected.push_back(force.id); continue; }
        if (type->transport) {
            if (transports < side.cap.trunc_to_integer()) { selected.push_back(force.id); ++transports; }
        } else buckets[force.type].push_back(&force);
    }
    const auto take = [&](auto& bucket) {
        selected.push_back(bucket.back()->id); bucket.pop_back();
    };
    for (auto& [id, bucket] : buckets) {
        const auto population = type_of(input, id)->population;
        if (population < Fixed::from_raw(Fixed::scale)) while (!bucket.empty()) take(bucket);
        else if (population <= cap) { take(bucket); cap = sim::math::subtract(cap, population).value(); }
    }
    std::size_t remaining{};
    for (const auto& [id, bucket] : buckets) { (void)id; remaining += bucket.size(); }
    if (remaining) {
        Fixed used{};
        for (auto& [id, bucket] : buckets) {
            if (bucket.empty()) continue;
            const auto population = type_of(input, id)->population;
            // Round the exact rational once. Quantizing size/remaining before multiplying
            // would turn an exact half point into less than a half for some bucket sizes.
            const auto denominator = static_cast<std::int64_t>(remaining) * Fixed::scale;
            if (cap.raw() > (std::numeric_limits<std::int64_t>::max() - denominator / 2)
                / static_cast<std::int64_t>(bucket.size())) return fail<std::vector<sim::EntityId>>("split quota exceeds bounded content arithmetic");
            const auto quota = (cap.raw() * static_cast<std::int64_t>(bucket.size()) + denominator / 2) / denominator;
            auto integral = Fixed::from_integer(quota);
            if (!integral) return core::Result<std::vector<sim::EntityId>>::failure(integral.error());
            auto count = sim::math::divide(integral.value(), population);
            if (!count) return core::Result<std::vector<sim::EntityId>>::failure(count.error());
            const auto starts = std::min(bucket.size(), static_cast<std::size_t>(std::max<std::int64_t>(0, count.value().trunc_to_integer())));
            for (std::size_t i = 0; i < starts; ++i) {
                take(bucket);
                if (!sim::math::try_add(used, population, used)) return fail<std::vector<sim::EntityId>>("population total overflow");
            }
        }
        if (!sim::math::try_subtract(cap, used, cap)) return fail<std::vector<sim::EntityId>>("population budget overflow");
    }
    for (auto& [id, bucket] : buckets) {
        const auto population = type_of(input, id)->population;
        while (!bucket.empty() && population <= cap) { take(bucket); cap = sim::math::subtract(cap, population).value(); }
    }
    return core::Result<std::vector<sim::EntityId>>::success(std::move(selected));
}
} // namespace

core::Result<SpaceEntryPlan> prepare_space_entry(const ConflictRecord& record,
    const SpaceEntryInputs& input, sim::EntityId first_free, std::uint64_t session_seed) {
    if (!input.map.tables || record.layer != BattleLayer::space || !record.id || first_free <= record.id
        || record.invader == 0 || record.defender == 0 || record.invader == record.defender
        || record.map_identity != input.map.map || record.forces.size() > gc::galactic_max_objects)
        return fail<SpaceEntryPlan>("invalid admitted space conflict or authored map");
    SpaceEntryPlan plan;
    plan.conflict = record; plan.map = input.map; plan.first_free = first_free;
    plan.fixture.map = input.map.map; plan.fixture.map_sha256 = input.map.map_sha256;
    plan.fixture.free_starting_units = false; plan.fixture.pre_built_base = false;
    plan.fixture.victory_condition = tt::VictoryCondition::all_enemy_units_destroyed;
    std::map<gc::PlayerId, const SpaceEntrySide*> sides;
    for (const auto& side : input.sides) {
        if (!sides.emplace(side.player, &side).second) return fail<SpaceEntryPlan>("duplicate side binding");
    }
    for (const auto owner : {record.invader, record.defender}) if (!sides.contains(owner))
        return fail<SpaceEntryPlan>("both owners need explicit faction/cap bindings");
    sim::EntityId previous{};
    for (const auto& force : record.forces) {
        if (force.id <= previous || force.id >= first_free || force.id == record.id
            || (force.owner != record.invader && force.owner != record.defender) || !type_of(input, force.type))
            return fail<SpaceEntryPlan>("invalid persistent roster identity or binding");
        previous = force.id;
    }
    auto defenders = markers(input.map, "Defending Forces Position");
    auto attackers = markers(input.map, "Attacker Entry Position");
    for (const auto* p : defenders) if (!usable(*p)) return fail<SpaceEntryPlan>("invalid defender marker pose");
    for (const auto* p : attackers) if (!usable(*p)) return fail<SpaceEntryPlan>("invalid attacker marker pose");
    // WGT-18/42: ordinary closest-defender branch. The prevention-source qualification
    // is retained explicitly; no ground-scene lookup is guessed from galactic structures.
    const auto* attack = attackers.empty() ? nullptr : attackers.front();
    if (attack && !defenders.empty()) {
        auto nearest = distance(*attack->position, *defenders.front()->position);
        if (!nearest) return core::Result<SpaceEntryPlan>::failure(nearest.error());
        for (const auto* p : attackers) {
            auto d = distance(*p->position, *defenders.front()->position);
            if (!d) return core::Result<SpaceEntryPlan>::failure(d.error());
            if (d.value() < nearest.value()) { nearest = d; attack = p; }
        }
    }
    std::uint32_t creation{};
    const auto allocate = [&]() -> std::optional<sim::EntityId> {
        if (plan.first_free == std::numeric_limits<sim::EntityId>::max()) return {};
        return plan.first_free++;
    };
    const auto append = [&](const SpaceEntryType& type, gc::PlayerId owner, std::uint32_t marker,
        sim::EntityId id, bool temporary) {
        ConflictForce force{id, type.galactic, owner, 0, ForceRole::escort};
        force.destroy_after_tactical = temporary;
        plan.conflict.forces.push_back(force);
        plan.placements.push_back({id, marker, true, ++creation});
    };
    auto stations = markers(input.map, "Space Station Position");
    script::authoritative::RandomStream random(child_seed(session_seed, record.id), 0, 0);
    // WGT-09: one draw only for multiple station markers, even if no station exists.
    const auto station_index = stations.size() > 1 ? random.next_below(stations.size()) : 0;
    plan.marker_draws = random.draws();
    if (stations.empty()) { plan.conflict.station = 0; plan.conflict.station_type = 0; }
    if (record.station_level && !stations.empty()) {
        if (!usable(*stations[station_index])) return fail<SpaceEntryPlan>("invalid station marker pose");
        const auto* station = type_of(input, input.station_galactic_type);
        if (!station || station->tactical != input.station_type || !input.map.tables->find(station->tactical)
            || input.map.tables->find(station->tactical)->kind != units::UnitKind::station)
            return fail<SpaceEntryPlan>("missing current-level station binding");
        if (!plan.conflict.station) {
            const auto id = allocate(); if (!id) return fail<SpaceEntryPlan>("station allocator exhausted");
            plan.conflict.station = *id; plan.conflict.station_type = station->galactic;
            append(*station, record.defender, stations[station_index]->record, *id, true);
        } else plan.placements.push_back({record.station, stations[station_index]->record, true, ++creation});
    }
    auto orbitals = markers(input.map, "Orbital Special Structure Position");
    const bool has_specific = std::any_of(input.map.placements.begin(), input.map.placements.end(), [](const auto& p) {
        return p.marker && !p.marker_for.empty();
    });
    std::set<std::uint32_t> used;
    std::vector<SpaceEntryStructure> structures = input.structures;
    for (const auto& s : input.structures) {
        const auto* type = type_of(input, s.type);
        if (!type) return fail<SpaceEntryPlan>("missing structure binding");
        if (!type->additional_structure.empty()) {
            const auto* added = named_type(input, type->additional_structure);
            if (!added) return fail<SpaceEntryPlan>("missing additional structure type");
            if (std::none_of(structures.begin(), structures.end(), [&](const auto& row) { return row.type == added->galactic; }))
                structures.push_back({0, added->galactic, s.owner, {}});
        }
    }
    if (!orbitals.empty() || has_specific) {
        std::stable_sort(structures.begin(), structures.end(), [&](const auto& a, const auto& b) {
            const auto pass = [&](const auto& row) {
                return !type_of(input, row.type)->every_marker_type.empty() ? 0 : row.remembered_marker ? 1 : 2;
            };
            return pass(a) < pass(b);
        });
        for (const auto& s : structures) {
            const auto* type = type_of(input, s.type);
            if (type->orbital) {
                std::vector<const skirmish::MapPlacement*> candidates;
                for (const auto& p : input.map.placements) if (p.marker && !used.contains(p.record)
                    && std::any_of(p.marker_for.begin(), p.marker_for.end(), [&](const auto& c) {
                        return folded(c.type) == folded(type->every_marker_type.empty() ? type->tactical : type->every_marker_type);
                    })) candidates.push_back(&p);
                if (type->every_marker_type.empty()) {
                    if (s.remembered_marker) {
                        const auto remembered = std::find_if(input.map.placements.begin(), input.map.placements.end(), [&](const auto& p) {
                            return p.record == *s.remembered_marker && p.marker && !used.contains(p.record);
                        });
                        if (remembered != input.map.placements.end()) candidates.insert(candidates.begin(), &*remembered);
                    }
                    if (candidates.empty() && folded(type->base_position) == "ORBITAL")
                        for (const auto* p : orbitals) if (!used.contains(p->record)) candidates.push_back(p);
                    if (candidates.size() > 1) candidates.resize(1);
                }
                const auto* placed = type->every_marker_type.empty() ? type : named_type(input, type->every_marker_type);
                if (!placed) return fail<SpaceEntryPlan>("missing every-marker type");
                for (const auto* p : candidates) {
                    if (!usable(*p)) return fail<SpaceEntryPlan>("invalid orbital marker pose");
                    const auto id = allocate(); if (!id) return fail<SpaceEntryPlan>("structure allocator exhausted");
                    append(*placed, type->every_marker_type.empty() ? record.defender : s.owner, p->record, *id, !s.id);
                    if (s.id) {
                        auto& copy = plan.conflict.forces.back();
                        copy.role = ForceRole::orbital; copy.parent = s.id;
                    }
                    plan.fixed_poses.emplace_back(*id, *p);
                    if (s.id && type->every_marker_type.empty()) plan.structure_markers.emplace_back(s.id, p->record);
                    used.insert(p->record);
                }
            }
            if (type->special_weapon) {
                // WGT-13: first source, or the origin without one. Synthesize a marker from
                // its copied pose; source backdrops need not be designer markers.
                skirmish::MapPlacement source;
                const auto at = std::find_if(input.map.placements.begin(), input.map.placements.end(), [](const auto& p) { return p.special_weapon_source; });
                source.position = Vec3{}; source.orientation_degrees = Vec3{};
                if (at != input.map.placements.end()) source = *at;
                source.type = "campaign weapon source"; source.marker = true;
                source.record = 1;
                for (const auto& p : plan.map.placements) {
                    if (p.record == std::numeric_limits<std::uint32_t>::max()) return fail<SpaceEntryPlan>("map marker ordinal exhausted");
                    source.record = std::max(source.record, p.record + 1);
                }
                if (!source.position || !source.orientation_degrees) return fail<SpaceEntryPlan>("invalid weapon source pose");
                // The shared selected-force builder accepts yaw-only marker poses.
                auto yaw_only = source;
                yaw_only.orientation_degrees->x = {}; yaw_only.orientation_degrees->y = {};
                plan.map.placements.push_back(yaw_only);
                const auto id = allocate(); if (!id) return fail<SpaceEntryPlan>("weapon allocator exhausted");
                append(*type, input.planet_owner, source.record, *id, !s.id);
                if (s.id) { auto& copy = plan.conflict.forces.back(); copy.role = ForceRole::orbital; copy.parent = s.id; }
                plan.fixed_poses.emplace_back(*id, source);
            }
        }
    }
    auto durability_table = units::durability_table(*input.map.tables);
    if (!durability_table) return core::Result<SpaceEntryPlan>::failure(durability_table.error());
    std::map<gc::PlayerId, std::vector<sim::EntityId>> initial_forces;
    std::map<gc::PlayerId, std::pair<Fixed, sim::EntityId>> flagships;
    for (const auto owner : {record.defender, record.invader}) {
        auto initial = split(record, input, *sides.at(owner));
        if (!initial) return core::Result<SpaceEntryPlan>::failure(initial.error());
        initial_forces.emplace(owner, std::move(initial).value());
    }
    std::vector<const ConflictForce*> source_roster;
    for (const auto id : input.roster_order) {
        const auto at = std::find_if(record.forces.begin(), record.forces.end(), [id](const auto& f) { return f.id == id; });
        if (at != record.forces.end()) source_roster.push_back(&*at);
    }
    for (const auto& f : record.forces) if (std::find(source_roster.begin(), source_roster.end(), &f) == source_roster.end()) source_roster.push_back(&f);
    // WGT-119: a zero sentinel and strict positive replacement; source context wins.
    for (const auto* force : source_roster) {
        const auto* type = type_of(input, force->type);
        const auto& initial = initial_forces.at(force->owner);
        if (type->transport || type->superweapon || std::find(initial.begin(), initial.end(), force->id) == initial.end()) continue;
        const auto* profile = durability_table.value().find(type->purchase_type ? type->purchase_type : skirmish::type_id(type->tactical));
        if (!type->source_flagship_rating && !profile) return fail<SpaceEntryPlan>("flagship source durability profile is absent for " + type->tactical);
        auto rating = type->source_flagship_rating ? core::Result<Fixed>::success(*type->source_flagship_rating)
            : sim::math::add(profile ? profile->max_hull : Fixed{}, profile ? profile->max_shields : Fixed{});
        if (!rating) return core::Result<SpaceEntryPlan>::failure(rating.error());
        auto& best = flagships[force->owner];
        if (rating.value() > best.first) best = {rating.value(), force->id};
    }
    // WGT-05: defender split is committed before invader split.
    for (const auto owner : {record.defender, record.invader}) {
        const auto& initial = initial_forces.at(owner);
        // WGT-52/53: absent markers skip this side's initial objects and escorts.
        // No defender source preserves the authored attacker sequence in plan.map.
        const auto* source = owner == record.defender ? (defenders.empty() ? nullptr : defenders.front()) : attack;
        std::vector<const skirmish::MapPlacement*> sequence;
        if (owner == record.defender) sequence = defenders;
        else if (defenders.empty()) sequence = attackers;
        else if (source) sequence.push_back(source);
        std::size_t cursor{};
        for (auto& force : plan.conflict.forces) if (force.owner == owner && force.role != ForceRole::escort && force.role != ForceRole::orbital) {
            const auto* type = type_of(input, force.type);
            force.role = !type->space_allowed ? ForceRole::retained
                : std::find(initial.begin(), initial.end(), force.id) != initial.end()
                    ? (source ? ForceRole::deployed : ForceRole::retained) : ForceRole::reinforcement;
        }
        if (!source) continue;
        for (const auto id : initial) {
            const auto force = std::find_if(record.forces.begin(), record.forces.end(), [id](const auto& f) { return f.id == id; });
            const auto* type = type_of(input, force->type);
            const auto marker = sequence[cursor]->record;
            auto selected_marker = marker;
            if (type->superweapon) {
                const auto death_star = markers(input.map, "Death_Star_Position");
                if (death_star.empty() || !usable(*death_star.front())) return fail<SpaceEntryPlan>("superweapon marker unavailable");
                selected_marker = death_star.front()->record; plan.conflict.superweapon = id;
            }
            plan.placements.push_back({id, selected_marker, true, ++creation});
            // WGT-114: advance once for a qualifying transport, after all passengers.
            if (type->transport && std::any_of(input.riders.begin(), input.riders.end(), [&](const auto& rider) {
                const auto* passenger = type_of(input, rider.force.type);
                return rider.transport == id && passenger && (!passenger->unique_space_unit.empty()
                    || !passenger->attach_to_flagship || !flagships[owner].second);
            })) cursor = (cursor + 1) % sequence.size();
            // WGT-17/37: escort objects have child-only identities and are discarded on return.
            for (const auto& name : type->escorts) {
                const auto* escort = named_type(input, name);
                if (!escort) return fail<SpaceEntryPlan>("missing escort type binding");
                const auto escort_id = allocate(); if (!escort_id) return fail<SpaceEntryPlan>("escort allocator exhausted");
                append(*escort, owner, marker, *escort_id, true);
            }
        }
    }
    // WGT-07/08: transport overflow retains source order; ordinary leftovers
    // walk each type bucket forward after deployment has removed its back.
    // Keep the identity manifest canonical and record the pool order separately.
    for (const auto owner : {record.defender, record.invader}) {
        std::map<gc::TypeId, std::vector<sim::EntityId>> leftovers;
        for (const auto* source : source_roster) {
            const auto force = std::find_if(plan.conflict.forces.begin(), plan.conflict.forces.end(),
                [&](const auto& f) { return f.id == source->id; });
            if (force->owner != owner || force->role != ForceRole::reinforcement || force->carrier) continue;
            if (type_of(input, force->type)->transport) plan.reserve_order.push_back(force->id);
            else leftovers[force->type].push_back(force->id);
        }
        for (const auto& [type, ids] : leftovers) {
            (void)type;
            plan.reserve_order.insert(plan.reserve_order.end(), ids.begin(), ids.end());
        }
    }
    std::set<sim::EntityId> stowed;
    std::map<sim::EntityId, std::uint64_t> passenger_counts;
    for (const auto& source : input.riders) {
        auto rider = source;
        const auto* type = type_of(input, rider.force.type);
        if (!type || rider.force.id == 0 || rider.force.id >= first_free || rider.force.id == record.id)
            return fail<SpaceEntryPlan>("invalid passenger identity or binding");
        const auto transport = std::find_if(plan.conflict.forces.begin(), plan.conflict.forces.end(), [&](const auto& f) { return f.id == rider.transport; });
        if (transport == plan.conflict.forces.end() || transport->owner != rider.force.owner)
            return fail<SpaceEntryPlan>("passenger has no owning transport");
        rider.force.tactical_type = skirmish::type_id(rider.tactical);
        rider.force.carrier = rider.transport;
        rider.force.role = transport->role == ForceRole::retained ? ForceRole::retained : ForceRole::reinforcement;
        if (transport->role == ForceRole::deployed) {
            rider.force.role = ForceRole::rider;
            const auto owner = transport->owner;
            const auto placement = std::find_if(plan.placements.begin(), plan.placements.end(), [&](const auto& p) { return p.force == rider.transport; });
            const auto marker = placement->marker;
            const auto passenger_order_wide = static_cast<std::uint64_t>(placement->creation_order) + 1 + passenger_counts[rider.transport];
            if (passenger_order_wide > std::numeric_limits<std::uint32_t>::max()) return fail<SpaceEntryPlan>("passenger creation order exhausted");
            const auto passenger_order = static_cast<std::uint32_t>(passenger_order_wide);
            if (!type->unique_space_unit.empty()) {
                const auto* container = named_type(input, type->unique_space_unit);
                if (!container) return fail<SpaceEntryPlan>("passenger unique container has no binding");
                const auto id = allocate(); if (!id) return fail<SpaceEntryPlan>("passenger container allocator exhausted");
                append(*container, owner, marker, *id, true);
                for (auto& p : plan.placements) if (p.force != *id && p.creation_order >= passenger_order) ++p.creation_order;
                plan.placements.back().creation_order = passenger_order;
                ++passenger_counts[rider.transport];
                rider.force.carrier = *id;
            } else if (type->attach_to_flagship && flagships[owner].second) rider.force.carrier = flagships[owner].second;
            if (rider.force.carrier == rider.transport) stowed.insert(rider.transport);
            plan.riders.push_back(rider);
        }
        if (std::any_of(plan.conflict.forces.begin(), plan.conflict.forces.end(), [&](const auto& f) { return f.id == rider.force.id; }))
            return fail<SpaceEntryPlan>("passenger identity already exists in the flattened roster");
        plan.conflict.forces.push_back(rider.force);
        for (auto member : rider.member_forces) {
            if (!member.id || member.id >= first_free || member.id == record.id || !type_of(input, member.type)
                || member.owner != rider.force.owner || !member.carrier
                || std::any_of(plan.conflict.forces.begin(), plan.conflict.forces.end(), [&](const auto& f) { return f.id == member.id; }))
                return fail<SpaceEntryPlan>("invalid nested passenger identity or binding");
            member.role = rider.force.role;
            plan.conflict.forces.push_back(member);
        }
    }
    for (auto& force : plan.conflict.forces) if (force.role == ForceRole::deployed
        && type_of(input, force.type)->transport && !stowed.contains(force.id)) {
        force.role = ForceRole::retained; // WGT-21: removal without galactic death, never a reserve offer.
        std::erase_if(plan.placements, [&](const auto& p) { return p.force == force.id; });
    }
    for (auto& force : plan.conflict.forces) {
        if (force.role == ForceRole::rider || force.carrier) continue; // passenger type is its contained identity
        const auto* type = type_of(input, force.type);
        if (!type) return fail<SpaceEntryPlan>("missing final tactical type binding");
        force.tactical_type = skirmish::type_id(type->tactical);
        force.purchase_type = type->purchase_type;
    }
    // WGT-113/114: allocate child-only objects in the final creation sequence,
    // including unique passenger ships created inside their transport entry.
    auto ordered = plan.placements;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.creation_order < b.creation_order; });
    std::map<sim::EntityId, sim::EntityId> remap;
    auto next = first_free;
    for (const auto& p : ordered) if (p.force >= first_free) remap.emplace(p.force, next++);
    const auto translated = [&](sim::EntityId id) { const auto at = remap.find(id); return at == remap.end() ? id : at->second; };
    for (auto& force : plan.conflict.forces) { force.id = translated(force.id); force.carrier = translated(force.carrier); }
    for (auto& p : plan.placements) p.force = translated(p.force);
    for (auto& pose : plan.fixed_poses) pose.first = translated(pose.first);
    for (auto& rider : plan.riders) rider.force.carrier = translated(rider.force.carrier);
    plan.conflict.station = translated(plan.conflict.station);
    plan.conflict.superweapon = translated(plan.conflict.superweapon);
    // Modifier IDs begin after this reserved source/placement boundary. Parents
    // and carriers must already name reserved persistent or temporary identities.
    if (std::any_of(plan.conflict.forces.begin(), plan.conflict.forces.end(), [&](const auto& force) {
        return force.id >= plan.first_free || force.parent >= plan.first_free || force.carrier >= plan.first_free;
    }) || std::any_of(plan.placements.begin(), plan.placements.end(), [&](const auto& placement) { return placement.force >= plan.first_free; }))
        return fail<SpaceEntryPlan>("force, parent, carrier or placement exceeds the reserved scene boundary");
    std::sort(plan.conflict.forces.begin(), plan.conflict.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    plan.conflict.structure_markers = plan.structure_markers;
    std::sort(plan.conflict.structure_markers.begin(), plan.conflict.structure_markers.end());
    // WGT-123: only scene owners need a relation-class label. Unused galactic
    // players do not become tactical sides; neutral scenery retains its binding.
    std::set<gc::PlayerId> active{record.invader, record.defender};
    std::set<gc::PlayerId> legacy_map_owners;
    for (const auto& force : plan.conflict.forces) active.insert(force.owner);
    const auto neutral = [&](gc::PlayerId owner) {
        const auto side = sides.find(owner);
        if (side == sides.end()) return false;
        const auto faction = std::find_if(input.map.factions.begin(), input.map.factions.end(), [&](const auto& f) {
            return folded(f.name) == folded(side->second->faction);
        });
        return faction != input.map.factions.end() && faction->neutral;
    };
    for (const auto& [owner, side] : sides) { (void)side; if (neutral(owner)) active.insert(owner); }
    // WGT-121: an admitted authored structure needs its original owner binding
    // before the shared map builder can resolve ownership by faction.
    for (const auto& placement : input.map.placements) {
        if (!skirmish::is_map_object_placement(placement) || !placement.owner_index || *placement.owner_index < 0
            || static_cast<std::size_t>(*placement.owner_index) >= input.map.factions.size()
            || static_cast<std::int64_t>(placement.enable_at_level) > static_cast<std::int64_t>(record.station_level)) continue;
        const auto& faction = input.map.factions[static_cast<std::size_t>(*placement.owner_index)];
        if (faction.neutral) continue;
        const auto owner = std::find_if(sides.begin(), sides.end(), [&](const auto& row) {
            return folded(row.second->faction) == folded(faction.name);
        });
        const bool always = !placement.branched_discardable || !faction.can_control_planets || placement.marker_behavior;
        // The shared map path owns unbound factions: it may create a phantom
        // nonplayable owner, use neutral scenery ownership, or remove the object.
        if (owner == sides.end()) continue;
        if (always || owner->first == record.defender
            || (owner->first == record.invader && (placement.loads_when_faction_present || record.corruption_mission))) {
            if (!active.contains(owner->first) && !faction.playable && faction.multiplayer_player) legacy_map_owners.insert(owner->first);
            active.insert(owner->first);
        }
    }
    for (const auto& retained : record.upgrades) {
        const auto type = std::find_if(input.map.tables->units.begin(), input.map.tables->units.end(), [&](const auto& t) {
            return skirmish::type_id(t.id) == retained.type;
        });
        if (type != input.map.tables->units.end() && type->production.upgrade_object) {
            active.insert(retained.owner); legacy_map_owners.erase(retained.owner);
        }
    }
    for (const auto& player : input.upgrade_players) for (const auto& name : player.planet) {
        const auto* type = input.map.tables->find(name);
        if (!type || !type->production.upgrade_object) continue;
        std::string affiliations = type->affiliation;
        std::replace(affiliations.begin(), affiliations.end(), ',', ' ');
        std::replace(affiliations.begin(), affiliations.end(), ';', ' ');
        std::istringstream stream(affiliations); std::string faction;
        while (stream >> faction) if (folded(faction) == folded(player.faction)) {
            active.insert(player.player); legacy_map_owners.erase(player.player); break;
        }
    }
    const auto bind = [&]() -> core::Result<std::vector<skirmish::LobbySlot>> {
        for (const auto owner : active) if (!sides.contains(owner))
            return fail<std::vector<skirmish::LobbySlot>>("WGT-123 relationship seam: scene owner has no faction binding");
        if (active.size() > 2 && input.relations.empty())
            return fail<std::vector<skirmish::LobbySlot>>("WGT-123 relationship seam: complete scene-owner relationships are required");
        std::map<std::pair<gc::PlayerId, gc::PlayerId>, const SpaceEntryRelation*> relations;
        for (const auto& relation : input.relations) {
            if (!relations.emplace(std::pair{relation.player, relation.other}, &relation).second)
                return fail<std::vector<skirmish::LobbySlot>>("WGT-123 relationship seam: duplicate directed relationship");
        }
        std::map<gc::PlayerId, gc::PlayerId> roots;
        for (const auto owner : active) roots.emplace(owner, owner);
        const auto root = [&](gc::PlayerId owner) { while (roots.at(owner) != owner) owner = roots.at(owner); return owner; };
        const auto combatant_pair = [&](gc::PlayerId a, gc::PlayerId b) {
            return a != b && (a == record.invader || a == record.defender) && (b == record.invader || b == record.defender);
        };
        for (const auto a : active) for (const auto b : active) {
            // WGT-123 qualifies new owners. The admitted combatant pair retains its
            // existing opposing-side contract; directed combatant relations are a
            // separate tactical-model seam, not a new entry refusal.
            if (input.relations.empty() || combatant_pair(a, b)) continue;
            const auto relation = relations.find({a, b});
            if (relation == relations.end())
                return fail<std::vector<skirmish::LobbySlot>>("WGT-123 relationship seam: missing directed scene-owner relationship");
            if (relation->second->allied && !neutral(a) && !neutral(b)) {
                const auto x = root(a), y = root(b);
                roots.at(std::max(x, y)) = std::min(x, y);
            }
        }
        if (root(record.invader) == root(record.defender))
            return fail<std::vector<skirmish::LobbySlot>>("WGT-123 relationship seam: combatants are in one allied class");
        std::map<gc::PlayerId, tt::TeamId> labels{{root(record.invader), 0U}, {root(record.defender), 1U}};
        tt::TeamId next_label = 2;
        for (const auto owner : active) if (!labels.contains(root(owner))) labels.emplace(root(owner), next_label++);
        for (const auto a : active) for (const auto b : active) {
            if (input.relations.empty() || combatant_pair(a, b)) continue;
            const auto& relation = *relations.at({a, b});
            const bool allied = a == b || (!neutral(a) && !neutral(b) && labels.at(root(a)) == labels.at(root(b)));
            const bool enemy = !neutral(a) && !neutral(b) && labels.at(root(a)) != labels.at(root(b));
            if (relation.allied != allied || relation.enemy != enemy)
                return fail<std::vector<skirmish::LobbySlot>>("WGT-123 relationship seam: directed allied/enemy/neutral graph is not representable");
        }
        std::vector<skirmish::LobbySlot> slots;
        for (const auto owner : active) {
            const auto& side = *sides.at(owner);
            slots.push_back({owner, side.faction, labels.at(root(owner)),
                side.human && (owner == record.invader || owner == record.defender), {}});
        }
        return core::Result<std::vector<skirmish::LobbySlot>>::success(std::move(slots));
    };
    auto binding = bind();
    if (!binding && !legacy_map_owners.empty()) {
        // Map-only NPCs retain the shared phantom path when their graph cannot
        // be projected. Required modifier/galactic owners never use this fallback.
        for (const auto owner : legacy_map_owners) active.erase(owner);
        binding = bind();
    }
    if (!binding) return core::Result<SpaceEntryPlan>::failure(binding.error());
    plan.fixture.slots = std::move(binding).value();
    return core::Result<SpaceEntryPlan>::success(std::move(plan));
}

namespace {
// One bounded scene transaction; arithmetic errors are retained until its ordered commit.
struct SceneMath {
    std::optional<core::Diagnostic> error;
    template<class T> T take(core::Result<T> value) {
        if (value) return std::move(value).value();
        if (!error) error = value.error();
        return {};
    }
    Fixed number(std::int64_t n) { return take(Fixed::from_integer(n)); }
    Fixed add(Fixed a, Fixed b) { return take(sim::math::add(a, b)); }
    Fixed sub(Fixed a, Fixed b) { return take(sim::math::subtract(a, b)); }
    Fixed mul(Fixed a, Fixed b) { return take(sim::math::multiply(a, b)); }
    Fixed div(Fixed a, Fixed b) { return take(sim::math::divide(a, b)); }
    Fixed half(Fixed a) { return Fixed::from_raw(a.raw() / 2); }
};
}
core::Result<SpaceFormationResult> form_space_entry(skirmish::SkirmishStart& scene,
    const SpaceEntryInputs& input, const SpaceEntryPlan& plan, std::optional<Vec3> invader_target) {
    using Result = core::Result<SpaceFormationResult>;
    SceneMath math;
    auto staged = scene.units;
    const auto find = [&](sim::EntityId id) -> skirmish::StartUnit* {
        const auto at = std::find_if(staged.begin(), staged.end(), [id](const auto& u) { return u.state.entity_id == id; });
        return at == staged.end() ? nullptr : &*at;
    };
    const auto extent = [&](const units::UnitType& type, sim::math::Quat rotation) {
        // WGT-46 / EGT-S3-Q03: collision model box, transformed by the current pose.
        const auto box = type.collision.value_or(units::CollisionBounds{{math.number(-1), math.number(-1), math.number(-1)}, {math.number(1), math.number(1), math.number(1)}});
        const auto scale = type.scale_factor.value_or(math.number(1));
        const Vec3 local{math.mul(math.half(math.sub(box.max.x, box.min.x)),scale),
            math.mul(math.half(math.sub(box.max.y, box.min.y)),scale),
            math.mul(math.half(math.sub(box.max.z, box.min.z)),scale)};
        const auto frame=math.take(sim::math::to_matrix(rotation,{}));
        const auto absolute=[&](Fixed value){return value < Fixed{} ? math.take(sim::math::negate(value)) : value;};
        const auto dimension=[&](unsigned axis){return math.add(math.add(math.mul(absolute(frame.rows[axis][0]),local.x),math.mul(absolute(frame.rows[axis][1]),local.y)),math.mul(absolute(frame.rows[axis][2]),local.z));};
        return Vec3{dimension(0),dimension(1),{}};
    };
    const auto bounds = [&](const auto& objects) {
        Vec3 maximum;
        for (const auto* object : objects) {
            const auto* type = input.map.tables->find(object->type);
            if (!type) { if (!math.error) math.error = fail<void>("formation type absent").error(); continue; }
            auto box = extent(*type,object->state.rotation);
            if (type->kind == units::UnitKind::squadron) {
                Vec3 low,high;
                const auto team=std::find_if(scene.setup.squadrons.begin(),scene.setup.squadrons.end(),[&](const auto& binding){return binding.container==object->state.entity_id;});
                for (std::size_t i=0; i<type->members.size(); ++i) {
                    const auto& member=type->members[i]; const auto* craft=input.map.tables->find(member.craft);
                    if (!craft) { if (!math.error) math.error=fail<void>("formation craft absent").error(); continue; }
                    const auto* live=team!=scene.setup.squadrons.end() && i<team->members.size() ? find(team->members[i]) : nullptr;
                    const auto member_box=extent(*craft,live?live->state.rotation:object->state.rotation);
                    const auto offset=member.offset.value_or(Vec3{});
                    low.x=std::min(low.x,math.sub(offset.x,member_box.x)); high.x=std::max(high.x,math.add(offset.x,member_box.x));
                    low.y=std::min(low.y,math.sub(offset.y,member_box.y)); high.y=std::max(high.y,math.add(offset.y,member_box.y));
                }
                box={math.half(math.sub(high.x,low.x)),math.half(math.sub(high.y,low.y)),{}};
            }
            maximum.x = std::max(maximum.x, box.x); maximum.y = std::max(maximum.y, box.y);
        }
        return Vec3{math.mul(maximum.y, math.number(2)), math.mul(maximum.x, math.number(2)), {}};
    };
    bool attacker{};
    std::set<sim::EntityId> registered;
    for (const auto& marker : plan.map.placements) {
        if (!marker.marker || !marker.position || !marker.orientation_degrees) continue;
        std::array<std::vector<skirmish::StartUnit*>, 6> buckets; // capital, medium, empty screen, small, transports, super
        auto placements = plan.placements;
        std::stable_sort(placements.begin(), placements.end(), [](const auto& a, const auto& b) { return a.creation_order < b.creation_order; });
        for (const auto& placement : placements) {
            if (placement.marker != marker.record) continue;
            const auto force = std::find_if(plan.conflict.forces.begin(), plan.conflict.forces.end(), [&](const auto& f) { return f.id == placement.force; });
            if (force == plan.conflict.forces.end() || force->id == plan.conflict.station || force->role == ForceRole::orbital
                || force->role == ForceRole::retained
                || std::any_of(plan.fixed_poses.begin(), plan.fixed_poses.end(), [&](const auto& pose) { return pose.first == force->id; })) continue;
            auto* object = find(force->id); if (!object || !registered.insert(force->id).second) continue;
            const auto* binding = type_of(input, force->type); const auto* type = input.map.tables->find(object->type);
            if (!binding || !type) return fail<SpaceFormationResult>("formation registration is unbound");
            const auto* leader = type;
            if (type->kind == units::UnitKind::squadron && !type->members.empty()) leader = input.map.tables->find(type->members.front().craft);
            if (!leader) return fail<SpaceFormationResult>("formation team leader is absent");
            const auto ship_class = folded(leader->campaign_ship_class);
            if (ship_class != "CAPITAL" && ship_class != "SUPER" && ship_class != "FRIGATE" && ship_class != "CORVETTE"
                && ship_class != "FIGHTER" && ship_class != "BOMBER" && ship_class != "TRANSPORT")
                return fail<SpaceFormationResult>("initial force Ship_Class is not qualified: " + leader->id);
            const auto bucket = ship_class == "SUPER" ? 5U : ship_class == "TRANSPORT" ? 4U
                : ship_class == "CAPITAL" ? 0U
                : ship_class == "FIGHTER" || ship_class == "BOMBER" ? 3U : 1U;
            buckets[bucket].push_back(object);
            attacker |= force->owner == plan.conflict.invader;
        }
        auto& small = buckets[3];
        // WGT-45: balanced interleave, retaining registration order within each kind.
        std::vector<skirmish::StartUnit*> fighters, bombers;
        for (auto* object : small) {
            const auto* type = input.map.tables->find(object->type);
            if (type->kind == units::UnitKind::squadron && !type->members.empty()) type = input.map.tables->find(type->members.front().craft);
            (folded(type->campaign_ship_class) == "FIGHTER" ? fighters : bombers).push_back(object);
        }
        if (!fighters.empty() && !bombers.empty()) {
            small.clear();
            std::size_t fighter{}, bomber{};
            const auto pairs = [&](std::size_t count) { for (std::size_t i=0; i<count; ++i) { small.push_back(bombers[bomber++]); small.push_back(fighters[fighter++]); } };
            if (bombers.size() > fighters.size()) {
                const auto leading = (bombers.size()-fighters.size())/2;
                while (bomber < leading) small.push_back(bombers[bomber++]);
                pairs(fighters.size());
                while (bomber < bombers.size()) small.push_back(bombers[bomber++]);
            } else {
                pairs(bombers.size()/2);
                for (std::size_t i=0; i<fighters.size()-bombers.size(); ++i) small.push_back(fighters[fighter++]);
                pairs(bombers.size()-bomber);
            }
        }
        const auto yaw = marker.orientation_degrees->z;
        const auto basis = sim::math::sin_cos_turn(math.div(math.add(yaw, math.number(270)), math.number(360)));
        const auto facing = math.take(tt::yaw_rotation(yaw));
        const auto place = [&](skirmish::StartUnit& object, Fixed x, Fixed y) {
            object.state.position.x = math.add(marker.position->x, math.sub(math.mul(x, basis.cosine), math.mul(y, basis.sine)));
            object.state.position.y = math.add(marker.position->y, math.add(math.mul(x, basis.sine), math.mul(y, basis.cosine)));
            object.state.rotation = facing; object.state.order = {}; object.yaw_degrees = yaw;
            const auto team = std::find_if(scene.setup.squadrons.begin(), scene.setup.squadrons.end(), [&](const auto& s) { return s.container == object.state.entity_id; });
            const auto* type = input.map.tables->find(object.type);
            if (team == scene.setup.squadrons.end() || !type) return;
            const auto rotation = sim::math::sin_cos_turn(math.div(yaw, math.number(360)));
            for (std::size_t i = 0; i < team->members.size() && i < type->members.size(); ++i) if (auto* craft = find(team->members[i])) {
                const auto offset = type->members[i].offset.value_or(Vec3{});
                craft->state.position.x = math.add(object.state.position.x, math.sub(math.mul(offset.x, rotation.cosine), math.mul(offset.y, rotation.sine)));
                craft->state.position.y = math.add(object.state.position.y, math.add(math.mul(offset.x, rotation.sine), math.mul(offset.y, rotation.cosine)));
                craft->state.rotation = facing; craft->state.order = {}; craft->yaw_degrees = yaw;
            }
        };
        const auto grid = [&](auto& objects, bool zero) {
            Vec3 footprint; if (objects.empty()) return footprint;
            auto spacing = bounds(objects); if (zero) spacing = {};
            const auto n = objects.size(); const std::size_t rows = n <= 2 ? 1 : n <= 6 ? 2 : n <= 9 ? 3 : n <= 16 ? 4 : n <= 25 ? 5 : 6;
            std::array<std::size_t, 6> counts{}; std::fill_n(counts.begin(), rows, n / rows);
            static constexpr int extras[6][5][5] = {
                {{0},{0},{0},{0},{0}}, {{0},{0},{0},{0},{0}}, {{1},{0,2},{0},{0},{0}},
                {{0},{0,2},{0,1,2},{0},{0}}, {{0},{1,3},{0,2,4},{0,1,2,3},{0}},
                {{0},{2,3},{1,3,5},{1,2,3,4},{0,1,2,3,4}}};
            const auto remainder = n % rows;
            if (remainder) for (std::size_t i = 0; i < remainder; ++i) ++counts[static_cast<std::size_t>(extras[rows-1][remainder-1][i])];
            Fixed shift{}; bool toggle{}; std::size_t index{}, previous{};
            auto depth = math.half(math.mul(math.number(static_cast<std::int64_t>(rows - 1)), spacing.y));
            for (std::size_t row = 0; row < rows; ++row) {
                if (row && counts[row] == previous) { if (!toggle) shift = math.add(shift, spacing.x); toggle = !toggle; }
                else if (toggle) { shift = math.sub(shift, spacing.x); toggle = false; }
                const auto start = math.sub(math.half(shift), math.half(math.mul(math.number(static_cast<std::int64_t>(counts[row] - 1)), spacing.x)));
                for (std::size_t col = 0; col < counts[row]; ++col) place(*objects[index++], math.add(start, math.mul(math.number(static_cast<std::int64_t>(col)), spacing.x)), depth);
                depth = math.sub(depth, spacing.y); previous = counts[row];
            }
            const auto columns = *std::max_element(counts.begin(), counts.begin() + static_cast<std::ptrdiff_t>(rows));
            footprint.x = columns == 1 ? math.half(spacing.x) : math.half(math.mul(math.number(static_cast<std::int64_t>(columns - 1)), spacing.x));
            footprint.y = footprint.x; return footprint;
        };
        grid(buckets[5], true);
        std::size_t central{}; while (central < 4 && buckets[central].empty()) ++central;
        if (central == 4) { grid(buckets[4], false); continue; }
        const auto footprint = grid(buckets[central], false);
        auto radius = math.take(sim::math::length(footprint));
        for (std::size_t bucket = central + 1; bucket < 4; ++bucket) {
            auto& objects = buckets[bucket]; if (objects.empty()) continue;
            const auto D = math.take(sim::math::length(bounds(objects)));
            if (D <= Fixed{}) return fail<SpaceFormationResult>("formation ring has zero model bounds");
            const bool screen = bucket == central + 1;
            auto ring = math.add(radius, math.half(D)); auto capacity_radius = ring; std::size_t index{};
            while (index < objects.size()) {
                const auto numerator = screen ? std::max(math.mul(capacity_radius, math.take(Fixed::from_decimal("3.926991"))), math.mul(D, math.number(3)))
                    : std::max(math.mul(capacity_radius, math.number(2)), math.mul(D, math.number(2)));
                const auto capacity = math.div(numerator, screen ? math.half(D) : D).trunc_to_integer() + (screen ? 0 : 1);
                const auto count = std::min(objects.size() - index, static_cast<std::size_t>(std::max<std::int64_t>(1, capacity)));
                for (std::size_t i = 0; i < count; ++i) {
                    const auto angle = count == 1 ? math.number(90) : math.add(screen ? math.take(Fixed::from_decimal("337.5")) : math.number(25),
                        math.div(math.mul(math.number(static_cast<std::int64_t>(i)), screen ? math.number(225) : math.number(130)), math.number(static_cast<std::int64_t>(count - 1))));
                    const auto direction = sim::math::sin_cos_turn(math.div(angle, math.number(360)));
                    place(*objects[index++], math.mul(ring, direction.cosine), math.mul(ring, direction.sine));
                }
                ring = math.add(ring, D); capacity_radius = math.add(capacity_radius, screen ? math.half(D) : D);
            }
            radius = capacity_radius;
        }
        auto& transports = buckets[4];
        if (!transports.empty()) {
            const auto spacing = bounds(transports);
            const auto root = math.take(sim::math::sqrt(math.number(static_cast<std::int64_t>(transports.size()))));
            const auto columns = static_cast<std::size_t>(std::max<std::int64_t>(2,
                math.add(root, math.take(Fixed::from_decimal("0.99"))).trunc_to_integer()));
            std::size_t index{}; auto depth = math.sub(math.sub(Fixed{}, footprint.y), math.number(200));
            while (index < transports.size()) {
                const auto count = std::min(columns, transports.size() - index);
                const auto width = math.add(spacing.x, math.number(20));
                const auto start = math.take(sim::math::negate(math.half(math.mul(math.number(static_cast<std::int64_t>(count - 1)), width))));
                for (std::size_t col = 0; col < count; ++col) place(*transports[index++], math.add(start, math.mul(math.number(static_cast<std::int64_t>(col)), width)), depth);
                depth = math.sub(depth, math.add(spacing.y, math.number(20)));
            }
        }
    }
    if (math.error) return Result::failure(*math.error);
    scene.units = std::move(staged);
    return Result::success({attacker && invader_target.has_value()});
}

std::vector<ReturnedUpgrade> space_entry_unbound_upgrades(const ConflictRecord& record, const units::UnitTables& tables) {
    std::vector<ReturnedUpgrade> unbound;
    for (const auto& retained : record.upgrades) {
        const auto type = std::find_if(tables.units.begin(), tables.units.end(), [&](const auto& row) { return skirmish::type_id(row.id) == retained.type; });
        if (type == tables.units.end() || !type->production.upgrade_object) unbound.push_back(retained);
    }
    return unbound;
}

core::Result<std::vector<tt::CampaignUpgrade>> space_entry_upgrades(const ConflictRecord& record,
    const SpaceEntryInputs& input, const SpaceEntryPlan& plan, skirmish::SkirmishStart& scene) {
    using Result = core::Result<std::vector<tt::CampaignUpgrade>>;
    std::vector<tt::CampaignUpgrade> result;
    const auto& tables = *input.map.tables;
    auto players = input.upgrade_players;
    if (players.empty()) for (const auto& side : input.sides) players.push_back({side.player, side.faction, true, {side.player}, {}, {}, {}});
    const auto affiliation = [&](const auto& type, const auto& player) {
        auto value = type.affiliation;
        for (auto& c : value) if (c == ',' || c == ';') c = ' ';
        std::istringstream stream(value); std::string token;
        while (stream >> token) if (folded(token) == folded(player.faction)) return true;
        return false;
    };
    const auto preforce = [&](const auto& unit) {
        return unit.role == skirmish::UnitRole::map_object || unit.state.entity_id == plan.conflict.station
            || std::any_of(plan.fixed_poses.begin(), plan.fixed_poses.end(), [&](const auto& pose) { return pose.first == unit.state.entity_id; });
    };
    std::vector<skirmish::StartUnit> frozen;
    for (const auto& unit : scene.units) if (preforce(unit)) frozen.push_back(unit);
    std::vector<const skirmish::StartUnit*> existing;
    for (const auto& unit : frozen) existing.push_back(&unit);
    // Map load first, then prepared station/structure registration order, never ID sorting.
    std::stable_sort(existing.begin(), existing.end(), [&](const auto* a, const auto* b) {
        const auto order = [&](const auto* u) {
            if (u->role == skirmish::UnitRole::map_object) return std::pair{0U, u->record};
            const auto at = std::find_if(plan.placements.begin(), plan.placements.end(), [&](const auto& p) { return p.force == u->state.entity_id; });
            return std::pair{1U, at == plan.placements.end() ? 0U : at->creation_order};
        };
        return order(a) < order(b);
    });
    for (const auto& retained : record.upgrades) {
        const auto type = std::find_if(tables.units.begin(), tables.units.end(), [&](const auto& row) { return skirmish::type_id(row.id) == retained.type; });
        // WGT-14, WGCH-163: an entry with no upgrade object in this layer (planetary corruption markers, a mod's
        // galactic-only upgrade) is not admitted and is never a reason to refuse the battle.
        if (type == tables.units.end() || !type->production.upgrade_object) continue;
        const auto owner = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.player == retained.owner; });
        if (owner == players.end()) return fail<std::vector<tt::CampaignUpgrade>>("retained upgrade owner is absent from player traversal");
        sim::EntityId holder{};
        for (const auto& candidate_player : players) {
            if (!candidate_player.playable || (candidate_player.player != owner->player
                && std::find(owner->allies.begin(), owner->allies.end(), candidate_player.player) == owner->allies.end())) continue;
            for (const auto* object : existing) {
                if (object->state.owner != candidate_player.player) continue;
                const auto* candidate = tables.find(object->type);
                if (!candidate) continue;
                if (std::any_of(candidate->production.buildable_campaign.begin(), candidate->production.buildable_campaign.end(), [&](const auto& group) {
                    return folded(group.faction) == folded(owner->faction) && std::any_of(group.types.begin(), group.types.end(), [&](const auto& name) { return folded(name) == folded(type->id); });
                })) { holder = object->state.entity_id; break; }
            }
            if (holder) break;
        }
        if (holder) result.push_back({retained.type, holder, retained.owner}); // no previous-holder preference
    }
    const auto origin = [&](const auto& type, const auto& owner) -> core::Result<void> {
        auto& next = *scene.setup.first_free_entity_id;
        if (next == std::numeric_limits<sim::EntityId>::max()) return fail<void>("upgrade allocator exhausted");
        if (std::none_of(scene.setup.players.begin(), scene.setup.players.end(), [&](const auto& p) { return p.player_id == owner.player; }))
            return fail<void>("origin upgrade owner needs an explicit scene player/team binding");
        skirmish::StartUnit unit;
        unit.state.entity_id = next++; unit.state.type_id = skirmish::type_id(type.id); unit.state.owner = owner.player;
        unit.type = type.id; unit.role = skirmish::UnitRole::map_object;
        scene.units.push_back(unit);
        result.push_back({unit.state.type_id, unit.state.entity_id, owner.player});
        return core::Result<void>::success();
    };
    // WGT-117: invader before defender; first-level admission, all same-owner scene objects.
    for (const auto owner_id : {record.invader, record.defender}) {
        const auto owner = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.player == owner_id; });
        if (owner == players.end()) continue;
        for (const auto& name : owner->galaxy) {
            const auto* type = tables.find(name);
            if (!type) return fail<std::vector<tt::CampaignUpgrade>>("missing galaxy upgrade binding");
            if (!affiliation(*type, *owner) || std::any_of(owner->nonfirst_galaxy.begin(), owner->nonfirst_galaxy.end(), [&](const auto& name) { return folded(name) == folded(type->id); }) || std::any_of(tables.units.begin(), tables.units.end(), [&](const auto& previous) { return folded(previous.production.next_upgrade) == folded(type->id); })) continue;
            bool owned{}; std::set<std::string> visited;
            for (const auto* level = type; level && visited.insert(folded(level->id)).second;
                level = level->production.next_upgrade.empty() ? nullptr : tables.find(level->production.next_upgrade)) {
                const auto id = skirmish::type_id(level->id);
                owned |= std::any_of(existing.begin(), existing.end(), [&](const auto* u) { return u->state.owner == owner_id && u->state.type_id == id; });
                owned |= std::any_of(result.begin(), result.end(), [&](const auto& u) { return u.owner == owner_id && u.type == id; });
            }
            if (!owned) { auto added = origin(*type, *owner); if (!added) return Result::failure(added.error()); }
        }
    }
    // WGT-118: no first-level or ownership-chain deduplication, including duplicate entries.
    for (const auto& owner : players) for (const auto& name : owner.planet) {
        const auto* type = tables.find(name);
        if (!type) return fail<std::vector<tt::CampaignUpgrade>>("missing planet upgrade binding");
        if (!affiliation(*type, owner) || !type->production.upgrade_object) continue;
        auto added = origin(*type, owner); if (!added) return Result::failure(added.error());
    }
    std::sort(scene.units.begin(), scene.units.end(), [](const auto& a, const auto& b) { return a.state.entity_id < b.state.entity_id; });
    scene.setup.units.clear(); for (const auto& u : scene.units) scene.setup.units.push_back(u.state);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return std::tuple{a.owner, a.holder, a.type} < std::tuple{b.owner, b.holder, b.type}; });
    return Result::success(std::move(result));
}

core::Result<ConflictEntry> build_space_entry(const ConflictRecord& record, const SpaceEntryInputs& input,
    sim::EntityId first_free, std::uint64_t session_seed) {
    auto prepared = prepare_space_entry(record, input, first_free, session_seed);
    if (!prepared) return core::Result<ConflictEntry>::failure(prepared.error());
    const auto& plan = prepared.value();
    // WGT-113: map load -> station/structure registration -> retained, galaxy,
    // all-player planet modifiers -> defender/invader forces and escorts.
    // Both builds are pure scene DTO assembly; no child services run until staging.
    auto prefix = plan.conflict; prefix.superweapon = 0;
    const auto preforce = [&](sim::EntityId id) {
        return id == prefix.station || std::any_of(plan.fixed_poses.begin(), plan.fixed_poses.end(), [&](const auto& pose) { return pose.first == id; });
    };
    std::erase_if(prefix.forces, [&](const auto& force) { return !preforce(force.id); });
    std::vector<ForcePlacement> prefix_placements;
    for (const auto& placement : plan.placements) if (preforce(placement.force)) prefix_placements.push_back(placement);
    auto prefix_scene = build_conflict_entry(prefix, plan.fixture, plan.map, prefix_placements, plan.first_free, session_seed);
    if (!prefix_scene) return prefix_scene;
    const auto modifier_first = *prefix_scene.value().start.setup.first_free_entity_id;
    auto upgrades = space_entry_upgrades(record, input, plan, prefix_scene.value().start);
    if (!upgrades) return core::Result<ConflictEntry>::failure(upgrades.error());
    const auto modifier_count = *prefix_scene.value().start.setup.first_free_entity_id - modifier_first;
    auto built = build_conflict_entry(plan.conflict, plan.fixture, plan.map, plan.placements, plan.first_free, session_seed);
    if (!built) return built;
    auto& entry = built.value();
    entry.scene_upgrades = std::move(upgrades).value();
    if (modifier_count) {
        const auto translate = [&](sim::EntityId id) { return id >= modifier_first ? id + modifier_count : id; };
        if (*entry.start.setup.first_free_entity_id > std::numeric_limits<sim::EntityId>::max() - modifier_count)
            return fail<ConflictEntry>("modifier/craft allocator exhausted");
        for (auto& unit : entry.start.units) unit.state.entity_id = translate(unit.state.entity_id);
        for (auto& squadron : entry.start.setup.squadrons) {
            squadron.container = translate(squadron.container);
            for (auto& member : squadron.members) member = translate(member);
        }
        for (auto& launch : entry.start.launches) launch.spawner = translate(launch.spawner);
        for (const auto& modifier : prefix_scene.value().start.units)
            if (modifier.state.entity_id >= modifier_first) entry.start.units.push_back(modifier);
        *entry.start.setup.first_free_entity_id += modifier_count;
        std::sort(entry.start.units.begin(), entry.start.units.end(), [](const auto& a, const auto& b) { return a.state.entity_id < b.state.entity_id; });
    }
    for (auto& unit : entry.start.units) {
        const auto force = std::find_if(plan.conflict.forces.begin(), plan.conflict.forces.end(), [&](const auto& f) { return f.id == unit.state.entity_id; });
        if (force == plan.conflict.forces.end()) continue;
        const auto* type = type_of(input, force->type);
        const auto pose = std::find_if(plan.fixed_poses.begin(), plan.fixed_poses.end(), [&](const auto& p) { return p.first == unit.state.entity_id; });
        if (pose != plan.fixed_poses.end()) {
            const auto& source = pose->second;
            unit.state.position = *source.position;
            const auto* table = input.map.tables->find(type->tactical);
            const auto height = table->movement.layer_z_adjust.value_or(Fixed{});
            if (!sim::math::try_add(unit.state.position.z, height, unit.state.position.z)
                || !sim::math::try_add(unit.state.position.z, source.layer_z_adjust.value_or(Fixed{}), unit.state.position.z))
                return fail<ConflictEntry>("fixed object height overflow");
            auto yaw = tt::yaw_rotation(source.orientation_degrees->z);
            auto pitch = tt::yaw_rotation(source.orientation_degrees->y);
            auto roll = tt::yaw_rotation(source.orientation_degrees->x);
            if (!yaw || !pitch || !roll) return fail<ConflictEntry>("fixed object facing invalid");
            auto facing = sim::math::compose(yaw.value(), {Fixed{}, pitch.value().z, Fixed{}, pitch.value().w});
            if (!facing) return core::Result<ConflictEntry>::failure(facing.error());
            facing = sim::math::compose(facing.value(), {roll.value().z, Fixed{}, Fixed{}, roll.value().w});
            if (!facing) return core::Result<ConflictEntry>::failure(facing.error());
            auto normalized = sim::math::normalize(facing.value());
            if (!normalized) return core::Result<ConflictEntry>::failure(normalized.error());
            unit.state.rotation = normalized.value();
            if (type->special_weapon) unit.role = skirmish::UnitRole::special_weapon;
        } else if (type->hover_offset && !type->squadron && force->id != plan.conflict.station) {
            const auto* table = input.map.tables->find(type->tactical);
            if (!sim::math::try_subtract(unit.state.position.z, table->movement.layer_z_adjust.value_or(Fixed{}), unit.state.position.z)
                || !sim::math::try_add(unit.state.position.z, *type->hover_offset, unit.state.position.z))
                return fail<ConflictEntry>("fleet hover offset overflow");
        }
    }
    for (const auto& rider : plan.riders) {
        const auto* type = type_of(input, rider.force.type);
        auto host = std::find_if(entry.start.units.begin(), entry.start.units.end(), [&](const auto& u) {
            return u.state.entity_id == rider.force.carrier;
        });
        if (host == entry.start.units.end()) return fail<ConflictEntry>("resolved rider carrier is absent");
        tt::CarriedObject carried;
        carried.entity_id = rider.force.id; carried.type_id = rider.force.tactical_type;
        carried.named_hero = type->named_hero; carried.generic_hero = type->generic_hero;
        carried.members = rider.members;
        if (!tt::contain_object(host->state, carried, false)) return fail<ConflictEntry>("rider attachment failed");
    }
    // WGT-112: all initial objects and deferred passengers exist before final form-up.
    auto formed = form_space_entry(entry.start, input, plan, input.invader_target);
    if (!formed) return core::Result<ConflictEntry>::failure(formed.error());
    entry.start.campaign_intro_admitted = formed.value().attacker_cinematic_admitted;
    entry.start.setup.units.clear();
    for (const auto& u : entry.start.units) entry.start.setup.units.push_back(u.state);
    entry.conflict.tactical = entry.start.setup;
    if (auto valid = tt::validate_setup(entry.conflict.tactical); !valid)
        return core::Result<ConflictEntry>::failure(valid.error());
    auto content = space_entry_content(entry.conflict.tactical, plan.map);
    if (!content) return core::Result<ConflictEntry>::failure(content.error());
    entry.content = std::move(content).value();
    return built;
}

core::Result<tt::TacticalContent> space_entry_content(const tt::TacticalSetup& setup, const skirmish::StartInputs& inputs) {
    auto content = conflict_entry_content(setup, inputs);
    if (!content) return content;
    // WGT-10/WSL-10..18: campaign objects use authored reserve counts, including
    // unlimited (-1). The shared skirmish table deliberately leaves these at zero.
    for (auto& profile : content.value().motion.squadrons.spawners) {
        const auto unit = std::find_if(inputs.tables->units.begin(), inputs.tables->units.end(), [&](const auto& row) { return skirmish::type_id(row.id) == profile.type_id; });
        if (unit == inputs.tables->units.end() || !unit->spawner) continue;
        for (auto& entry : profile.entries) {
            const auto reserve = std::find_if(unit->spawner->reserves.begin(), unit->spawner->reserves.end(), [&](const auto& row) { return skirmish::type_id(row.squadron) == entry.squadron; });
            if (reserve != unit->spawner->reserves.end()) entry.reserve = reserve->count;
        }
    }
    if (setup.campaign) for (const auto& owner : setup.campaign->players) {
        if (owner.start_tech > 5) return fail<tt::TacticalContent>("garrison owner tech outside 0..5");
        auto selected = *inputs.tables;
        for (auto& type : selected.units) if (type.spawner) {
            const auto select = [&](const auto& tiers, const auto& fallback) {
                const bool authored = std::any_of(tiers.begin(), tiers.end(), [](const auto& rows) { return !rows.empty(); });
                if (!authored) return fallback; // explicit synthetic Tech0 profiles
                for (auto tech = static_cast<int>(owner.start_tech); tech >= 0; --tech)
                    if (!tiers[static_cast<std::size_t>(tech)].empty()) return tiers[static_cast<std::size_t>(tech)];
                return std::vector<units::SpawnEntry>{};
            };
            type.spawner->starting = select(type.spawner->campaign_starting, type.spawner->starting);
            type.spawner->reserves = select(type.spawner->campaign_reserves, type.spawner->reserves);
        }
        auto adapted = units::motion_table(selected); if (!adapted) return core::Result<tt::TacticalContent>::failure(adapted.error());
        for (auto profile : adapted.value().squadrons.spawners) {
            const auto type = std::find_if(selected.units.begin(), selected.units.end(), [&](const auto& row) { return skirmish::type_id(row.id) == profile.type_id; });
            if (type==selected.units.end() || !type->spawner) return fail<tt::TacticalContent>("selected garrison profile has no source spawner");
            for (auto& entry : profile.entries) {
                const auto reserve = std::find_if(type->spawner->reserves.begin(), type->spawner->reserves.end(), [&](const auto& row) { return skirmish::type_id(row.squadron) == entry.squadron; });
                entry.reserve = reserve == type->spawner->reserves.end() ? 0 : reserve->count;
            }
            content.value().motion.squadrons.campaign_spawners.emplace(std::pair{profile.type_id, owner.player}, std::move(profile));
        }
        // Empty selected starting lists must override a populated lower-tier base profile too.
        for (const auto& profile : content.value().motion.squadrons.spawners)
            if (!content.value().motion.squadrons.campaign_spawners.contains({profile.type_id, owner.player})) {
                auto empty = profile; empty.entries.clear();
                content.value().motion.squadrons.campaign_spawners.emplace(std::pair{profile.type_id, owner.player}, std::move(empty));
            }
    }
    return content;
}

} // namespace eawr::campaign
