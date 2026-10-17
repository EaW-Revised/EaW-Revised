#include "galactic_ai_reader.hpp"

#include <algorithm>
#include <limits>

namespace eawr::script::foc::ai {
namespace {
template<class Rows, class Id, class Key>
const auto* find_row(const Rows& rows, Id id, Key key) {
    const auto found = std::lower_bound(rows.begin(), rows.end(), id,
        [&](const auto& row, Id value) { return key(row) < value; });
    return found != rows.end() && key(*found) == id ? &*found : nullptr;
}

} // namespace

const ReaderObject* StructuralIndex::find(sim::EntityId id) const noexcept {
    const auto found = std::lower_bound(objects.begin(), objects.end(), id,
        [](const ReaderObject& row, sim::EntityId key) { return row.id < key; });
    return found != objects.end() && found->id == id ? &*found : nullptr;
}
bool StructuralIndex::arriving(sim::EntityId id) const noexcept {
    return std::binary_search(transit.begin(), transit.end(), id);
}

StructuralIndex build_structural_index(std::span<const ReaderPlanet> planets, const sim::galactic::GalacticSnapshot& snapshot) {
    namespace galactic = sim::galactic;
    std::size_t expected = planets.size() + snapshot.fleets().size() * 4;
    if (snapshot.control()) for (const auto& control : snapshot.control()->planets) expected += control.landed.size();
    std::vector<ReaderObject> objects;
    objects.reserve(expected);
    FlatIdMap identities(expected);
    FlatIdMap transit(snapshot.fleets().size() + 16);
    const auto add = [&](sim::EntityId id, sim::tactical::TypeId type, sim::tactical::PlayerId owner,
                         const sim::math::Vec3& position, galactic::PlanetId containing,
                         galactic::IntelligenceContainer container = galactic::IntelligenceContainer::none, bool structure = false) {
        if (!identities.insert(id).second) return;
        objects.push_back({id, type, owner, position, containing, container, structure});
    };
    // Ordered roster indexing resolves shared persistent identities. This traversal never services world entities.
    for (const auto& row : planets) add(row.id, row.type, row.owner, row.position, row.id);
    for (const auto& fleet : snapshot.fleets()) {
        // In transit a fleet has no planet parent. Do not retain its former orbit as containment.
        const auto* at = find_row(planets, fleet.orbit, [](const auto& p) { return p.id; });
        const auto containing = fleet.motion.transit ? galactic::PlanetId{} : fleet.orbit;
        const auto position = fleet.motion.transit || !at ? fleet.motion.position : at->position;
        add(fleet.id, 0, fleet.owner, position, containing, galactic::IntelligenceContainer::orbit);
        for (const auto& member : fleet.members) {
            add(member.id, member.type, fleet.owner, position, containing, galactic::IntelligenceContainer::orbit);
            if (fleet.motion.transit) transit.insert(member.id);
        }
        if (fleet.motion.transit) transit.insert(fleet.id);
    }
    if (snapshot.control()) for (const auto& control : snapshot.control()->planets) {
        const auto* at = find_row(planets, control.planet, [](const auto& p) { return p.id; });
        if (!at) continue;
        for (const auto& force : control.landed) add(force.id, force.type, force.owner, at->position, control.planet, galactic::IntelligenceContainer::ground);
    }
    for (const auto& row : planets) if (row.production) for (const auto& object : row.production->objects)
        add(object.id, object.type, object.owner, row.position, row.id,
            object.domain == galactic::ProductionDomain::space ? galactic::IntelligenceContainer::orbit : galactic::IntelligenceContainer::ground,
            object.kind == galactic::ProductionKind::structure);
    // Nested payloads inherit the resolved parent's location, never a type-based guess.
    if (snapshot.movement()) {
        std::vector<const galactic::MovementContents*> nested;
        nested.reserve(snapshot.movement()->contents.size());
        for (const auto& contents : snapshot.movement()->contents) nested.push_back(&contents);
        // The first row of a duplicated container wins.
        std::stable_sort(nested.begin(), nested.end(), [](const auto* a, const auto* b) { return a->container < b->container; });
        nested.erase(std::unique(nested.begin(), nested.end(), [](const auto* a, const auto* b) { return a->container == b->container; }), nested.end());
        for (std::size_t index = 0; index < objects.size(); ++index) {
            const auto parent = objects[index];
            const auto found = std::lower_bound(nested.begin(), nested.end(), parent.id,
                [](const auto* row, sim::EntityId key) { return row->container < key; });
            if (found == nested.end() || (*found)->container != parent.id) continue;
            for (const auto& member : (*found)->children) {
                if (identities.contains(member.id)) continue;
                add(member.id, member.type, parent.owner, parent.position, parent.planet, parent.container);
                if (transit.contains(parent.id)) transit.insert(member.id);
            }
        }
    }
    StructuralIndex index;
    index.objects = std::move(objects);
    std::sort(index.objects.begin(), index.objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    index.transit = transit.keys();
    std::sort(index.transit.begin(), index.transit.end());
    return index;
}

core::Result<void> fill_view_units(std::vector<detail::ViewUnit>& units, const StructuralIndex& index,
    const ReaderFog& fog, const sim::PartitionExecutor& executor) {
    namespace galactic = sim::galactic;
    // Reused slots only ever carry the galactic fields below; a fresh slot is value-initialized.
    units.resize(index.objects.size());
    const auto* snapshot = fog.snapshot;
    const auto* observer = snapshot && snapshot->observer() ? &*snapshot->observer() : nullptr;
    const auto observations = snapshot ? snapshot->observations() : std::span<const galactic::ObservedPlanet>{};
    // Per viewer: the observer row and the ascending sub-range of observations that belong to it.
    struct Viewer {
        const galactic::ObserverPlayer* row{};
        std::size_t begin{}, end{};
    };
    std::vector<Viewer> viewers(fog.viewers.size());
    const auto key = [](const galactic::ObservedPlanet& row) { return std::pair{row.player, row.planet}; };
    bool ordered = true;
    if (observer && fog.uses_fog) {
        // The lookup below needs the table ordered exactly as find_row requires; otherwise keep the whole-table search.
        ordered = std::is_sorted(observations.begin(), observations.end(),
            [&](const auto& a, const auto& b) { return key(a) < key(b); });
        for (std::size_t index_of = 0; index_of < viewers.size(); ++index_of) {
            const auto id = fog.viewers[index_of].id;
            viewers[index_of].row = find_row(observer->players, id, [](const auto& row) { return row.player; });
            const auto first = std::lower_bound(observations.begin(), observations.end(), id,
                [](const auto& row, auto player) { return row.player < player; });
            const auto last = std::upper_bound(first, observations.end(), id,
                [](auto player, const auto& row) { return player < row.player; });
            viewers[index_of].begin = static_cast<std::size_t>(first - observations.begin());
            viewers[index_of].end = static_cast<std::size_t>(last - observations.begin());
        }
    }
    const auto known_for = [&](std::size_t viewer, sim::galactic::PlanetId planet) -> const galactic::ObservedPlanet* {
        if (!ordered) return find_row(observations, std::pair{fog.viewers[viewer].id, planet}, key);
        const auto first = observations.begin() + static_cast<std::ptrdiff_t>(viewers[viewer].begin);
        const auto last = observations.begin() + static_cast<std::ptrdiff_t>(viewers[viewer].end);
        const auto found = std::lower_bound(first, last, planet, [](const auto& row, auto value) { return row.planet < value; });
        return found != last && found->planet == planet ? &*found : nullptr;
    };
    const auto phase = executor.execute_phase("galactic-ai-objects", sim::tick_partition_count, [&](std::size_t partition) {
        const auto range = sim::partition_range(partition, units.size());
        for (auto at = range.begin; at < range.end; ++at) {
            const auto& source = index.objects[at]; auto& unit = units[at];
            unit.id = source.id; unit.type = source.type; unit.owner = source.owner; unit.position = source.position;
            unit.team = {};
            if (const auto* owner_row = find_row(fog.teams, source.owner, [](const auto& row) { return row.player; }))
                unit.team = owner_row->team;
            unit.visible_to = 0;
            if (!fog.uses_fog) unit.visible_to = ~std::uint64_t{}; // WGA-08
            else if (observer) {
                const ReaderTypeTraits* traits{};
                bool traits_found{};
                const galactic::ObserverType* live{};
                bool live_found{};
                for (std::size_t viewer = 0; viewer < fog.viewers.size(); ++viewer) {
                    const auto& row = viewers[viewer].row;
                    const bool allied_owner = source.owner == fog.viewers[viewer].id || (row
                        && std::binary_search(row->allies.begin(), row->allies.end(), source.owner));
                    bool visible = allied_owner;
                    if (!visible) if (const auto* known = known_for(viewer, source.planet)) {
                        // WGUI-18: scalar ownership/presence/count is not an
                        // object-details grant. The planet target is separate.
                        if (source.id == source.planet) visible = known->knowledge.ownership;
                        else {
                            if (!traits_found) { traits = find_row(fog.traits, source.type, [](const auto& t) { return t.type; }); traits_found = true; }
                            if (source.type == 0 || source.structure || (traits && traits->present)) {
                                if (!live_found) { live = find_row(observer->types, source.type, [](const auto& t) { return t.type; }); live_found = true; }
                                galactic::ObjectIntelligenceContext object;
                                object.planetary_system = source.planet != 0; object.structure = source.structure;
                                object.container = source.container;
                                if (traits && traits->present) { object.hero = traits->hero; object.named_hero = traits->named_hero; }
                                object.stealth_hero = live && live->stealth_hero;
                                visible = galactic::object_details_known(object, known->knowledge);
                            }
                        }
                    }
                    if (visible) unit.visible_to |= std::uint64_t{1} << fog.viewers[viewer].snapshot_index;
                }
            }
            else for (const auto& observed : fog.legacy) {
                // Explicit legacy fixture visibility is a separate input.
                if (observed.planet != source.planet) continue;
                for (const auto& viewer : fog.viewers) if (viewer.id == observed.player) {
                    unit.visible_to |= std::uint64_t{1} << viewer.snapshot_index; break;
                }
            }
        }
    });
    return phase;
}

std::shared_ptr<detail::WorldView> AiWorldReader::take_buffer(bool& aligned) {
    aligned = false;
    if (spare_ && spare_.use_count() == 1) {
        aligned = view_ && spare_epoch_ == view_epoch_ && spare_->units.size() == view_->units.size();
        return std::move(spare_);
    }
    // A reader that still holds the older publication keeps its own copy.
    spare_.reset();
    return std::make_shared<detail::WorldView>();
}

void AiWorldReader::publish(std::shared_ptr<detail::WorldView> view) {
    spare_ = std::move(view_); spare_epoch_ = view_epoch_;
    view_ = std::move(view);
}

void AiWorldReader::publish(std::shared_ptr<detail::WorldView> view, std::shared_ptr<const StructuralIndex> index) {
    publish(std::move(view));
    index_ = std::move(index); view_epoch_ = ++epochs_;
}

} // namespace eawr::script::foc::ai
