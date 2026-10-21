#include "eawr/sim/galactic/raid.hpp"
#include "session_impl.hpp"
#include "native_action_shadow_data.hpp"
#include "native_force_validation.hpp"
#include "native_fleet_admission.hpp"
#include "native_control_admission.hpp"
#include "native_production_admission.hpp"
#include "native_movement_admission.hpp"
#include "native_population_projection.hpp"
#include "native_victory_projection.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/control.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/core/gc_profile.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace eawr::sim::galactic {
using session_detail::invalid;
using session_detail::ordered_births;
using session_detail::validate_native_force;
void GalacticSession::NativeActionShadow::Data::inherit_reads(const std::shared_ptr<const Data>& previous) {
    // This is a storage/lookup bound, not a limit on authored objects or actions.
    // Freeze only the sparse written frontier after 32 links; the replay chain
    // remains intact and preserves every immediate transaction's ordering.
    constexpr std::size_t max_read_links = 32;
    if (previous->read_depth + 1 < max_read_links) {
        read_parent = previous; read_depth = previous->read_depth + 1;
        return;
    }
    auto checkpoint = std::make_shared<Data>(); checkpoint->base = previous->base;
    const auto merge_newest = [](auto& into, const auto& from) {
        for (const auto& [key, value] : from) into.try_emplace(key, value);
    };
    for (auto layer = previous.get(); layer; layer = layer->read_parent.get()) {
        merge_newest(checkpoint->fleet_rows, layer->fleet_rows);
        merge_newest(checkpoint->actor_rows, layer->actor_rows);
        merge_newest(checkpoint->detached_rows, layer->detached_rows);
        merge_newest(checkpoint->fleet_blockers, layer->fleet_blockers);
        merge_newest(checkpoint->accounts, layer->accounts);
        merge_newest(checkpoint->locations, layer->locations);
        merge_newest(checkpoint->invasion_rows, layer->invasion_rows);
        if (!layer->new_invasion_queue.empty() && (checkpoint->new_invasion_queue.empty()
            || layer->new_invasion_queue.front() < checkpoint->new_invasion_queue.front()))
            checkpoint->new_invasion_queue.assign(1, layer->new_invasion_queue.front());
        checkpoint->forces.insert(checkpoint->forces.end(), layer->forces.begin(), layer->forces.end());
    }
    work.copied_fleet_rows += checkpoint->fleet_rows.size();
    work.copied_actor_rows += checkpoint->actor_rows.size();
    read_parent = std::move(checkpoint); read_depth = 1; ++read_checkpoints;
}
std::shared_ptr<const GalacticSnapshot> GalacticSession::NativeActionShadow::Data::view() const {
    if (publication) return publication;
    const auto inherited = parent ? parent->view() : base;
    gc_profile::Scope profile_scope(gc_profile::Phase::shadow_view);
    NativeSnapshotChanges changes;
    changes.touched_planets.emplace();
    auto& touched = *changes.touched_planets;
    for (const auto& [id, row] : fleet_rows) {
        if (row) touched.push_back(row->orbit);
        if (const auto* old = session_detail::native_find_row(inherited->fleets(), id, [](const auto& item) { return item.id; })) touched.push_back(old->orbit);
    }
    for (const auto& [id, row] : actor_rows) {
        if (row) touched.push_back(row->planet);
        if (const auto* old = session_detail::native_find_row(inherited->actor_records(), id, [](const auto& item) { return item.id; })) touched.push_back(old->planet);
    }
    for (const auto& [id, row] : detached_rows) {
        if (row) touched.push_back(row->planet);
        if (inherited->fleet_control()) if (const auto* old = session_detail::native_find_row(
            inherited->fleet_control()->detached_units, id, [](const auto& item) { return item.member.id; })) touched.push_back(old->planet);
    }
    for (const auto& [id, row] : locations) { (void)row; touched.push_back(id); }
    for (const auto& [id, row] : control_rows) { (void)row; touched.push_back(id); }
    for (const auto& [id, row] : production_rows) { (void)row; touched.push_back(id); }
    for (const auto& force : forces) touched.push_back(force->request.planet);
    for (const auto& [id, row] : fleet_blockers) {
        (void)row;
        if (const auto* fleet = this->fleet(id)) touched.push_back(fleet->orbit);
    }
    if (production_projection && inherited->production()) {
        changes.production = *inherited->production(); changes.production->players = *production_projection;
    }
    if (population_projection) changes.population = *population_projection;
    const auto patch = []<class T, class Key>(std::vector<T>& rows, const T& row, Key key) {
        const auto at = std::lower_bound(rows.begin(), rows.end(), key(row), [&](const auto& item, auto id) { return key(item) < id; });
        if (at != rows.end() && key(*at) == key(row)) *at = row;
        else rows.insert(at, row);
    };
    if (!fleet_rows.empty()) {
        changes.fleets.emplace(inherited->fleets().begin(), inherited->fleets().end());
        for (const auto& [id, row] : fleet_rows) {
            if (row) patch(*changes.fleets, *row, [](const auto& item) { return item.id; });
            else std::erase_if(*changes.fleets, [id](const auto& item) { return item.id == id; });
        }
    }
    if (!actor_rows.empty()) {
        auto rows = std::make_shared<std::vector<CensusObject>>(inherited->actor_records().begin(), inherited->actor_records().end());
        for (const auto& [id, row] : actor_rows) {
            if (row) patch(*rows, *row, [](const auto& item) { return item.id; });
            else std::erase_if(*rows, [id](const auto& item) { return item.id == id; });
        }
        changes.actors = std::move(rows);
    }
    if (!detached_rows.empty() && inherited->fleet_control()) {
        changes.fleet_control = *inherited->fleet_control();
        for (const auto& [id, row] : detached_rows) {
            if (row) patch(changes.fleet_control->detached_units, *row, [](const auto& item) { return item.member.id; });
            else std::erase_if(changes.fleet_control->detached_units, [id](const auto& item) { return item.member.id == id; });
        }
    }
    if (!accounts.empty() && inherited->player_economy()) {
        changes.economy = *inherited->player_economy();
        for (auto& row : changes.economy->players)
            if (const auto at = accounts.find(row.player); at != accounts.end()) row.credits = at->second;
    }
    if (!locations.empty()) {
        if (inherited->control()) changes.control = *inherited->control();
        if (!changes.production && inherited->production()) changes.production = *inherited->production();
        for (const auto& [id, location] : locations) {
            (void)id;
            if (changes.control) patch(changes.control->planets, location->presence, [](const auto& row) { return row.planet; });
            if (changes.production) patch(changes.production->planets, location->production, [](const auto& row) { return row.planet; });
        }
    }
    if (!forces.empty()) {
        auto originals = std::make_shared<std::vector<CensusObject>>(inherited->commissioned_originals().begin(), inherited->commissioned_originals().end());
        for (const auto& force : forces) for (const auto& row : force->placement.originals)
            patch(*originals, row, [](const auto& item) { return item.id; });
        changes.originals = std::move(originals);
    }
    if (!production_rows.empty() && inherited->production()) {
        if (!changes.production) changes.production = *inherited->production();
        for (const auto& [id, row] : production_rows) { (void)id; patch(changes.production->planets, row, [](const auto& item) { return item.planet; }); }
    }
    if (!control_rows.empty() && inherited->control()) {
        if (!changes.control) changes.control = *inherited->control();
        for (const auto& [id, row] : control_rows) { (void)id; patch(changes.control->planets, row, [](const auto& item) { return item.planet; }); }
    }
    if (!actor_rows.empty() && inherited->control() && !inherited->control()->ability_types.empty()) {
        if (!changes.control) changes.control = *inherited->control();
        for (const auto& [id, row] : actor_rows) {
            if (row) update_control_actor(*changes.control, *row);
            else erase_control_actor(*changes.control, id);
        }
    }
    if (!observer_rows.empty() && inherited->observer()) {
        changes.observer = *inherited->observer();
        for (const auto& [id, row] : observer_rows) { (void)id; patch(changes.observer->planets, row, [](const auto& item) { return item.planet; }); }
    }
    if (!fleet_blockers.empty() && inherited->production() && inherited->production()->fleet_blockers) {
        if (!changes.production) changes.production = *inherited->production();
        for (auto& row : *changes.production->fleet_blockers)
            if (const auto at = fleet_blockers.find(row.fleet); at != fleet_blockers.end()) row.skips_space_combat = at->second;
        for (const auto id : created_blockers)
            if (std::none_of(changes.production->fleet_blockers->begin(), changes.production->fleet_blockers->end(), [id](const auto& row) { return row.fleet == id; }))
                changes.production->fleet_blockers->push_back({id, false, fleet_blockers.at(id), true});
    }
    if (!actor_rows.empty() && inherited->victory()) {
        auto projected = victory ? *victory : *inherited->victory();
        for (const auto& [id, row] : actor_rows) {
            if (row) static_cast<void>(session_detail::update_native_victory_actor(projected, *row));
            else static_cast<void>(session_detail::remove_native_victory_actor(projected, id));
        }
        changes.victory = std::move(projected);
    }
    // Immediate batches preserve their existing publication grouping: force
    // births first, then the scalar credit/victory projection.
    if (!accounts.empty() && victory && (immediate_batch || actor_rows.empty())) changes.victory = *victory;
    if (movement && movement.get() != inherited->movement()) changes.movement = movement;
    if (!events.empty()) {
        changes.events.emplace(inherited->events().begin(), inherited->events().end());
        changes.events->insert(changes.events->end(), events.begin(), events.end());
    }
    if (observer_generation != inherited->observer_input_generation()) {
        changes.observer_generation = observer_generation;
        if (inherited->observer()) {
            const auto fleets = changes.fleets ? std::span<const Fleet>(*changes.fleets) : inherited->fleets();
            const auto actors = changes.actors ? std::span<const CensusObject>(*changes.actors) : inherited->actor_records();
            changes.observations = std::make_shared<const std::vector<ObservedPlanet>>(
                observed_planets(changes.observer ? *changes.observer : *inherited->observer(), fleets, {}, movement ? movement.get() : inherited->movement(), actors));
        }
    }
    // Generations are derived invalidation keys, excluded from canonical bytes.
    const bool physical_changes = !actor_rows.empty() || !fleet_rows.empty() || !control_rows.empty() || !forces.empty()
        || !production_rows.empty() || production_projection || population_projection;
    if (inherited->economy_input_generation() && (physical_changes || !accounts.empty())) {
        auto generation = *inherited->economy_input_generation();
        if (last_generation) {
            generation.census = std::max(generation.census, last_generation->census);
            generation.accounts = std::max(generation.accounts, last_generation->accounts);
        }
        if (physical_changes) ++generation.census;
        if (!accounts.empty()) ++generation.accounts;
        if (physical_changes && fiscal_schema && base_income && income_edges) {
            const auto fleets = changes.fleets ? std::span<const Fleet>(*changes.fleets) : inherited->fleets();
            const auto actors = changes.actors ? std::span<const CensusObject>(*changes.actors) : inherited->actor_records();
            const auto* control = changes.control ? &*changes.control : (inherited->control() ? &*inherited->control() : nullptr);
            const auto* production = changes.production ? &*changes.production : (inherited->production() ? &*inherited->production() : nullptr);
            const auto* fleet_control = changes.fleet_control ? &*changes.fleet_control : (inherited->fleet_control() ? &*inherited->fleet_control() : nullptr);
            const auto detached = fleet_control ? std::span<const DetachedUnit>(fleet_control->detached_units) : std::span<const DetachedUnit>{};
            std::optional<IncomeSetup> inputs;
            std::vector<session_detail::NativeIncomeSourceCandidate> candidates;
            if (income_schema) {
                inputs = *income_schema;
                session_detail::project_native_income(*inputs, *income_schema, control, inherited->planets(), fleets, detached, production, actors, candidates);
            }
            const auto* income = inherited->income() ? &*inherited->income() : nullptr;
            const auto gambling = income ? std::span<const GamblingIncomeState>(income->gambling) : std::span<const GamblingIncomeState>{};
            const InlineExecutor executor;
            auto bonuses = session_detail::project_native_income_bonuses(inputs ? &*inputs : nullptr, gambling, inherited->planets(), *base_income, executor);
            generation.income.reset();
            if (bonuses) {
                auto contributions = session_detail::project_native_income_contributions(*fiscal_schema, inherited->planets(), *base_income, control,
                    inherited->economy_corruption() ? &*inherited->economy_corruption() : nullptr, income, bonuses.value(), *income_edges, executor);
                if (contributions) {
                    generation.income.emplace();
                    for (std::size_t i = 0; i < contributions.value().size(); ++i)
                        generation.income->emplace_back(fiscal_schema->planets[i].planet, contributions.value()[i].total);
                }
            }
        }
        last_generation = std::make_shared<const EconomyInputGeneration>(std::move(generation));
        changes.economy_generation = last_generation;
    }
    publication = GalacticSnapshot::native_view(inherited, std::move(changes));
    return publication;
}
const Fleet* GalacticSession::NativeActionShadow::Data::fleet(FleetId id) const {
    for (auto layer = this; layer; layer = layer->read_parent.get()) {
        if (const auto at = layer->fleet_rows.find(id); at != layer->fleet_rows.end())
            return at->second ? &*at->second : nullptr;
        for (const auto& force : layer->forces) for (const auto& row : force->placement.fleets)
            if (row.id == id) return &row;
    }
    const auto rows = base->fleets();
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
    return at != rows.end() && at->id == id ? &*at : nullptr;
}
const CensusObject* GalacticSession::NativeActionShadow::Data::actor(EntityId id) const {
    for (auto layer = this; layer; layer = layer->read_parent.get()) {
        if (const auto at = layer->actor_rows.find(id); at != layer->actor_rows.end())
            return at->second ? &*at->second : nullptr;
        for (const auto& force : layer->forces) for (const auto& row : force->placement.originals)
            if (row.id == id) return &row;
    }
    const auto rows = base->actor_records();
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
    return at != rows.end() && at->id == id ? &*at : nullptr;
}
const DetachedUnit* GalacticSession::NativeActionShadow::Data::detached(EntityId id) const {
    for (auto layer = this; layer; layer = layer->read_parent.get())
        if (const auto at = layer->detached_rows.find(id); at != layer->detached_rows.end())
            return at->second ? &*at->second : nullptr;
    if (base->fleet_control()) for (const auto& row : base->fleet_control()->detached_units)
        if (row.member.id == id) return &row;
    return nullptr;
}
const MovementContents* GalacticSession::NativeActionShadow::Data::contents(EntityId id) const {
    for (auto layer = this; layer; layer = layer->read_parent.get())
        for (const auto& force : layer->forces) for (const auto& row : force->placement.contents)
            if (row.container == id) return &row;
    if (base->movement()) {
        const auto& rows = base->movement()->contents;
        const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.container < key; });
        if (at != rows.end() && at->container == id) return &*at;
    }
    return nullptr;
}
// The kernel's owner adapter borrows immutable schemas and resolves only the
// command's actual fleet/actor/detached frontier. It has no ECS registry.
class GalacticSession::NativeActionShadow::Data::FleetOwner {
protected:
    Data& data_;
    std::map<FleetId, session_detail::FleetData> loaded_;
    std::set<FleetId> removed_;
    std::map<PlanetId, session_detail::PlanetData> loaded_planets_;
public:
    struct Schemas {
        const std::optional<FleetControlSetup>& fleet_control;
        const MovementSetup* movement{};
    } setup;
    std::span<const Seat> seats;
    const GalacticClock& clock;
    EntityId& next_id;
    std::size_t& live_object_count;
    std::shared_ptr<const MovementSetup> movement_inputs;
    bool population_stale{};
    std::array<bool, 32> dirty{};
    const PlanetGraph* graph{};
    NativeMovementListener native_movement_listener;
    struct Registry {
        FleetOwner& owner;
        template<class T> T& get(FleetId id) {
            static_assert(std::is_same_v<T, session_detail::FleetData> || std::is_same_v<T, session_detail::PlanetData>);
            if constexpr (std::is_same_v<T, session_detail::PlanetData>) {
                const auto rows = owner.data_.base->planets();
                const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
                return owner.loaded_planets_.try_emplace(id, session_detail::PlanetData{at->position, at->owner}).first->second;
            } else {
            auto at = owner.loaded_.find(id);
            if (at == owner.loaded_.end()) {
                const auto* row = owner.data_.fleet(id);
                ++owner.data_.work.copied_fleet_rows;
                at = owner.loaded_.emplace(id, session_detail::FleetData{row->owner, row->orbit, row->members,
                    row->orbit_slot, row->motion, row->raid_destination}).first;
            }
            return at->second;
            }
        }
        void destroy(FleetId id) { owner.loaded_.erase(id); owner.removed_.insert(id); }
    } registry;
    struct FleetIds {
        FleetOwner& owner;
        std::map<FleetId, FleetId> selected;
        auto find(FleetId id) {
            if (contains(id)) selected.try_emplace(id, id);
            else selected.erase(id);
            return selected.find(id);
        }
        auto end() { return selected.end(); }
        void erase(std::map<FleetId, FleetId>::iterator at) { selected.erase(at); }
        bool contains(FleetId id) const { return !owner.removed_.contains(id) && (owner.loaded_.contains(id) || owner.data_.fleet(id)); }
        FleetId at(FleetId id) const { return id; }
        void erase(FleetId) const {}
    } fleets;
    struct PlanetIds {
        const GalacticSnapshot& base;
        PlanetId at(PlanetId id) const { return id; }
        bool contains(PlanetId id) const {
            const auto rows = base.planets();
            const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
            return at != rows.end() && at->id == id;
        }
    } planets;
    class Detached {
        Data& data_;
        std::map<EntityId, DetachedUnit> loaded_;
        std::set<EntityId> removed_;
    public:
        explicit Detached(Data& data) : data_(data) {}
        auto find(EntityId id) {
            if (!removed_.contains(id) && !loaded_.contains(id))
                if (const auto* row = data_.detached(id)) loaded_.emplace(id, *row);
            return loaded_.find(id);
        }
        auto end() { return loaded_.end(); }
        DetachedUnit& at(EntityId id) { return find(id)->second; }
        void erase(EntityId id) { loaded_.erase(id); removed_.insert(id); }
        void erase(std::map<EntityId, DetachedUnit>::const_iterator at) { erase(at->first); }
        template<class... Args> auto emplace(EntityId id, Args&&... args) {
            find(id); removed_.erase(id); return loaded_.emplace(id, std::forward<Args>(args)...);
        }
        void finish() {
            for (const auto& [id, row] : loaded_) data_.detached_rows[id] = row;
            for (const auto id : removed_) data_.detached_rows[id] = std::nullopt;
        }
    } detached_units;
    explicit FleetOwner(Data& data)
        : data_(data), setup{data.base->fleet_control(), data.base->movement()}, seats(data.base->seats()),
          clock(data.base->clock()), next_id(data.next_id), live_object_count(data.live_objects),
          movement_inputs(data.movement),
          graph(data.graph.get()), native_movement_listener(data.movement_listener), registry{*this}, fleets{*this, {}}, planets{*data.base}, detached_units(data) {}
    bool raid_qualified(FleetId id, PlanetId destination) const {
        const auto view = data_.view(); const auto* fleet = data_.fleet(id);
        return fleet && view->movement() && view->control() && qualifies_raid(*fleet, destination,
            *view->movement(), *view->control(), view->actor_records(), view->planet_access());
    }
    const Seat* seat(SeatId id) const {
        const auto at = std::find_if(seats.begin(), seats.end(), [id](const auto& row) { return row.seat_id == id; });
        return at != seats.end() ? &*at : nullptr;
    }
    const CensusObject* movement_actor(EntityId id) const { return data_.actor(id); }
    std::vector<Planet> sorted_planets() const { return {data_.base->planets().begin(), data_.base->planets().end()}; }
    std::vector<Route> sorted_routes() const { return {data_.base->routes().begin(), data_.base->routes().end()}; }
    void invalidate_observer_projection() { ++data_.observer_generation; }
    void relocate_actor(EntityId id, PlayerId player, PlanetId planet, EntityId container) {
        if (const auto* before = data_.actor(id)) {
            auto row = *before; row.owner = player; row.planet = planet; row.container = container;
            if (row != *before) {
                data_.actor_rows[id] = std::move(row); ++data_.observer_generation;
                ++data_.work.copied_actor_rows;
                data_.actor_changes.push_back({NativeActorChange::Kind::changed, id, 0, {}, {}, {}});
            }
        }
        if (const auto* contents = data_.contents(id))
            for (const auto& child : contents->children) relocate_actor(child.id, player, planet, id);
    }
    void record_fleet_shell(FleetId id, PlayerId player, PlanetId planet) {
        if (!data_.can_create && !(data_.base->production() && data_.base->production()->nested_creation)) return;
        const auto type = data_.base->production() && data_.base->production()->nested_creation
            ? data_.base->production()->nested_creation->fleet_type : TypeId{};
        CensusObject row{id, type, player, planet, planet, false, false, type, 0, ActorCombatRole::unavailable, {}, {}};
        if (data_.base->production() && data_.base->production()->nested_creation) {
            const auto& types = data_.base->production()->nested_creation->types;
            const auto fact = std::lower_bound(types.begin(), types.end(), type, [](const auto& value, auto key) { return value.type < key; });
            if (fact != types.end() && fact->type == type && fact->combat_role != ActorCombatRole::unavailable) {
                row.combat_role = fact->combat_role; row.abilities = fact->abilities; row.garrison = false;
            }
        }
        data_.actor_rows[id] = std::move(row); ++data_.observer_generation;
        data_.actor_changes.push_back({NativeActorChange::Kind::created, id, 0, {}, {}, {}});
    }
    void lose_actor(EntityId id) {
        if (const auto* row = data_.contents(id)) for (const auto& child : row->children) lose_actor(child.id);
        if (!data_.actor(id)) return;
        data_.actor_rows[id] = std::nullopt;
        data_.actor_changes.push_back({NativeActorChange::Kind::lost, id, 0, {}, {}, {}});
    }
    void insert(const Fleet& row) {
        auto motion = row.motion;
        if (setup.movement && !motion.orbit_entry) {
            std::set<FleetId> ids;
            for (const auto& fleet : data_.base->fleets()) ids.insert(fleet.id);
            for (const Data* layer = &data_; layer; layer = layer->read_parent.get()) {
                for (const auto& [id, value] : layer->fleet_rows) { (void)value; ids.insert(id); }
                for (const auto& force : layer->forces) for (const auto& fleet : force->placement.fleets) ids.insert(fleet.id);
            }
            for (const auto& [id, value] : loaded_) { (void)value; ids.insert(id); }
            std::uint64_t latest{};
            for (const auto id : ids) if (fleets.contains(id)) {
                const auto at = loaded_.find(id);
                latest = std::max(latest, at != loaded_.end() ? at->second.motion.orbit_entry : data_.fleet(id)->motion.orbit_entry);
            }
            motion.orbit_entry = latest + 1;
        }
        loaded_[row.id] = {row.owner, row.orbit, row.members, row.orbit_slot, std::move(motion), row.raid_destination};
    }
    void refresh_fleet_combat_skip(FleetId id) {
        if (!data_.base->production() || !data_.base->production()->fleet_blockers || !fleets.contains(id)) return;
        const auto& blockers = *data_.base->production()->fleet_blockers;
        bool present = std::any_of(blockers.begin(), blockers.end(), [id](const auto& row) { return row.fleet == id; });
        for (const Data* layer = &data_; !present && layer; layer = layer->read_parent.get()) present = layer->fleet_blockers.contains(id);
        if (!present) return;
        const auto& row = registry.get<session_detail::FleetData>(id);
        std::vector<CensusObject> records;
        MovementSetup skip_movement;
        std::set<EntityId> visited;
        const auto gather = [&](auto&& self, EntityId actor) -> void {
            if (!visited.insert(actor).second) return;
            if (const auto* value = data_.actor(actor)) records.push_back(*value);
            if (const auto* value = data_.contents(actor)) {
                skip_movement.contents.push_back(*value);
                for (const auto& child : value->children) self(self, child.id);
            }
        };
        gather(gather, id);
        for (const auto& member : row.members) gather(gather, member.id);
        std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(skip_movement.contents.begin(), skip_movement.contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
        auto skip = fleet_combat_skip(Fleet{id, row.owner, row.orbit, row.members}, records, &skip_movement);
        if (skip) data_.fleet_blockers[id] = skip.value();
    }
    std::set<FleetId> movement_fleet_ids() const {
        std::set<FleetId> ids;
        for (const auto& fleet : data_.base->fleets()) ids.insert(fleet.id);
        for (const Data* layer = &data_; layer; layer = layer->read_parent.get()) {
            for (const auto& [id, row] : layer->fleet_rows) { (void)row; ids.insert(id); }
            for (const auto& force : layer->forces) for (const auto& fleet : force->placement.fleets) ids.insert(fleet.id);
        }
        return ids;
    }
    template<class Consume> void visit_movement_fleets(const std::set<FleetId>& ids, Consume consume) const {
        for (const auto id : ids) {
            if (removed_.contains(id)) continue;
            if (const auto at = loaded_.find(id); at != loaded_.end()) consume(id, at->second);
            else if (const auto* row = data_.fleet(id)) consume(id, *row);
        }
    }
    core::Result<bool> orbit_conflict(const Fleet& fleet) const {
        std::vector<CensusObject> records;
        MovementSetup contents;
        std::set<EntityId> visited;
        const auto gather = [&](auto&& self, EntityId id) -> void {
            if (!visited.insert(id).second) return;
            if (const auto* row = data_.actor(id)) records.push_back(*row);
            if (const auto* row = data_.contents(id)) {
                contents.contents.push_back(*row);
                for (const auto& child : row->children) self(self, child.id);
            }
        };
        gather(gather, fleet.id);
        for (const auto& member : fleet.members) gather(gather, member.id);
        bool has_actors = !records.empty();
        // Preserve the live owner's schema fallback only for a world without
        // actor facts. Missing facts in an otherwise qualified world refuse.
        if (!has_actors) {
            for (const auto& row : data_.base->actor_records()) if (data_.actor(row.id)) { has_actors = true; break; }
            for (const Data* layer = &data_; !has_actors && layer; layer = layer->read_parent.get()) {
                for (const auto& [id, row] : layer->actor_rows) if (row && data_.actor(id)) { has_actors = true; break; }
                for (const auto& force : layer->forces) for (const auto& row : force->placement.originals)
                    if (data_.actor(row.id)) { has_actors = true; break; }
            }
        }
        std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(contents.contents.begin(), contents.contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
        return session_detail::native_orbit_conflict(fleet, has_actors
            ? std::optional<std::span<const CensusObject>>(records) : std::nullopt, *movement_inputs, &contents);
    }
    void finish() {
        if (data_.independent_player && data_.next_id > data_.range_end)
            throw std::runtime_error("native player identity block exhausted");
        for (auto& [id, row] : loaded_) {
            const auto* previous = data_.fleet(id);
            if (previous && previous->owner == row.owner && previous->orbit == row.orbit
                && previous->members == row.members && previous->orbit_slot == row.orbit_slot && previous->motion == row.motion && previous->raid_destination == row.raid_destination)
                continue;
            data_.fleet_rows[id] = Fleet{id, row.owner, row.orbit, std::move(row.members), row.orbit_slot, std::move(row.motion), row.raid_destination};
        }
        for (const auto id : removed_) data_.fleet_rows[id] = std::nullopt;
        detached_units.finish();
        if (population_stale) data_.population_stale = true;
        data_.publication.reset();
    }
};
class GalacticSession::NativeActionShadow::Data::ControlOwner final : public FleetOwner {
    std::shared_ptr<const GalacticSnapshot> input_;
public:
    struct ControlView {
        std::uint32_t max_ground_forces{};
        std::span<const ControlPlayer> players;
        std::span<const ControlRelation> relations;
        std::span<const ControlType> types;
        std::vector<ControlPlanet> planets;
        std::span<const PlanetControl> state;
    };
    struct ObserverView { decltype(ObserverSetup{}.planets) planets; };
    std::optional<ControlView> control;
    std::optional<ObserverView> observer;
    const std::uint64_t completed_tick;
    const bool failed{};
    class Actors {
        const Data& data_;
        mutable std::map<EntityId, CensusObject> selected_;
    public:
        explicit Actors(const Data& data) : data_(data) {}
        auto find(EntityId id) const {
            if (!selected_.contains(id)) if (const auto* row = data_.actor(id)) selected_.emplace(id, *row);
            return selected_.find(id);
        }
        auto end() const { return selected_.end(); }
    } actor_records;
    bool native_invasions_enabled{}, native_applying{};
    std::uint64_t& next_native_invasion;
    const std::uint64_t native_invasion_range_end;
    std::size_t& native_invasion_transport_count;
    std::vector<std::uint64_t>& native_invasion_queue;
    struct Invasions {
        Data& data;
        std::size_t size() const { return data.invasion_records; }
        void emplace(std::uint64_t number, NativeInvasion request) {
            data.invasion_rows.emplace(number, std::move(request)); ++data.invasion_records;
        }
    } native_invasions;
    ControlOwner(Data& data, std::span<const PlanetId> selected)
        : FleetOwner(data), input_(data.view()), completed_tick(data.base->completed_tick()), actor_records(data), native_invasions_enabled(data.invasions_enabled),
          next_native_invasion(data.next_invasion),
          native_invasion_range_end(data.independent_player ? data.invasion_range_end : std::numeric_limits<std::uint64_t>::max()),
          native_invasion_transport_count(data.invasion_transports),
          native_invasion_queue(data.new_invasion_queue), native_invasions{data} {
        if (const auto& schema = input_->control()) {
            control.emplace(ControlView{schema->max_ground_forces, schema->players, schema->relations, schema->types, {}, schema->state});
            for (const auto id : selected)
                if (const auto* row = session_detail::native_find_row(schema->planets, id, [](const auto& item) { return item.planet; }))
                    control->planets.push_back(*row);
        }
        if (const auto& schema = input_->observer()) {
            observer.emplace();
            for (const auto id : selected)
                if (const auto* row = session_detail::native_find_row(schema->planets, id, [](const auto& item) { return item.planet; }))
                    observer->planets.push_back(*row);
        }
    }
    void publish_surface_arrivals(PlanetId planet, std::span<const SurfaceForce> arrivals) {
        if (!observer || arrivals.empty()) return;
        auto at = std::lower_bound(observer->planets.begin(), observer->planets.end(), planet,
            [](const auto& row, auto id) { return row.planet < id; });
        if (at == observer->planets.end() || at->planet != planet) return;
        for (const auto& row : arrivals) at->landed_transports.push_back({row.id, row.owner, false, false});
        dirty[static_cast<std::size_t>(StateSection::observer)] = true;
    }
    void finish() {
        FleetOwner::finish();
        if (control && dirty[static_cast<std::size_t>(StateSection::control)])
            for (auto& row : control->planets) data_.control_rows.insert_or_assign(row.planet, std::move(row));
        if (observer && dirty[static_cast<std::size_t>(StateSection::observer)])
            for (auto& row : observer->planets) data_.observer_rows.insert_or_assign(row.planet, std::move(row));
    }
};
core::Result<GalacticSession::NativeInvasionReceipt> GalacticSession::NativeActionShadow::admit_invasion(
    PlayerId player, FleetId fleet, std::span<const EntityId> transports, bool pending) {
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != player) || data_->work.requests >= data_->max_requests)
        return invalid<NativeInvasionReceipt>("invalid native invasion layer request or exhausted budget");
    const auto published = snapshot(); const auto* row = this->fleet(fleet);
    const std::array<PlanetId, 1> selected{row ? row->orbit : PlanetId{}};
    detach(); Data::ControlOwner owner(*data_, selected);
    auto result = session_detail::admit_native_invasion_rows(owner, player, fleet, transports, pending);
    owner.finish(); data_->publication = published; // invasion records have no snapshot publication
    if (!result) return result;
    ++data_->work.requests;
    data_->record_request(Data::InvasionRequest{
        false, player, fleet, std::vector<EntityId>(transports.begin(), transports.end()), pending, result.value(), 0});
    return result;
}
core::Result<GalacticSession::NativeInvasionReceipt> GalacticSession::stage_native_invasion(
    NativeActionShadow& shadow, PlayerId player, FleetId fleet, std::span<const EntityId> transports, bool pending) {
    using Result = core::Result<NativeInvasionReceipt>;
    auto result = shadow.admit_invasion(player, fleet, transports, pending);
    if (!result || !result.value().admitted) return result;
    const auto& receipt = result.value();
    std::vector<std::uint8_t> bytes;
    for (const auto word : std::array<std::uint64_t, 9>{1, transports.empty() ? 1U : 4U, player, fleet,
        static_cast<std::uint64_t>(pending), static_cast<std::uint64_t>(receipt.admitted), receipt.request,
        static_cast<std::uint64_t>(receipt.queued), static_cast<std::uint64_t>(receipt.reason)}) detail::put_u64(bytes, word);
    for (const auto transport : transports) detail::put_u64(bytes, transport);
    if (auto saved = append_native_effect(NativeEffectKind::invasion, std::move(bytes)); !saved) return Result::failure(saved.error());
    if (auto continued = advance_native_segment(); !continued) return Result::failure(continued.error());
    return result;
}
std::shared_ptr<const GalacticSnapshot> GalacticSession::NativeActionShadow::snapshot() const { return data_->player ? data_->view() : nullptr; }
const Fleet* GalacticSession::NativeActionShadow::fleet(FleetId id) const { return data_->fleet(id); }
const CensusObject* GalacticSession::NativeActionShadow::actor(EntityId id) const { return data_->actor(id); }
core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::NativeActionShadow::admit_fleet(
    PlayerId player, const CommandPayload& payload, bool pending_local_conflict) {
    if (!std::holds_alternative<CreateFleet>(payload) && !std::holds_alternative<AddFleetMembers>(payload)
        && !std::holds_alternative<RemoveFleetMembers>(payload) && !std::holds_alternative<MergeFleets>(payload)
        && !std::holds_alternative<SetFleetOrbitSlot>(payload) && !std::holds_alternative<TransferFleetMembers>(payload)
        && !std::holds_alternative<MoveFleet>(payload))
        return invalid<NativeFleetReceipt>("native fleet layer requires a fleet command");
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != player) || data_->work.requests >= data_->max_requests || encode_payload(payload).size() > galactic_replay_max_payload - 38
        || decode_payload(layer_of(payload), opcode_of(payload), encode_payload(payload)) != std::optional(payload))
        return invalid<NativeFleetReceipt>("invalid native fleet layer request or exhausted budget");
    const auto seats = data_->base->seats();
    const auto issuer = std::find_if(seats.begin(), seats.end(), [player](const auto& row) { return row.player == player && row.active; });
    if (issuer == seats.end()) return core::Result<NativeFleetReceipt>::success({false, 0, GalacticRejectReason::inactive_seat});
    const auto published = data_->view();
    detach(); data_->publication = published; // movement notification sees the pre-publication view
    Data::FleetOwner owner(*data_);
    const auto first = data_->events.size();
    const GameCommand command{{data_->base->completed_tick(), issuer->seat_id, 0}, payload};
    if (std::holds_alternative<MoveFleet>(payload))
        session_detail::execute_native_movement(owner, command, data_->base->completed_tick(), data_->events, pending_local_conflict, {});
    else session_detail::execute_native_fleet(owner, command, data_->base->completed_tick(), data_->events, pending_local_conflict, false);
    NativeFleetReceipt receipt;
    for (auto i = first; i < data_->events.size(); ++i) {
        const auto& event = data_->events[i];
        if (event.kind == GalacticEventKind::command_rejected) receipt.reason = event.reason;
        if (event.kind == GalacticEventKind::command_executed) { receipt.admitted = true; receipt.object = event.subject; }
    }
    owner.finish(); ++data_->work.requests;
    data_->record_request(Data::FleetRequest{player, payload, pending_local_conflict, receipt});
    return core::Result<NativeFleetReceipt>::success(receipt);
}
core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::NativeActionShadow::launch_transport(
    PlayerId player, EntityId id, FleetId target, bool pending_local_conflict) {
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != player) || data_->work.requests >= data_->max_requests)
        return invalid<NativeFleetReceipt>("invalid native launch layer request or exhausted budget");
    const auto before = data_->view();
    std::vector<PlanetId> selected;
    if (before->control()) for (const auto& row : before->control()->planets)
        if (std::any_of(row.landed.begin(), row.landed.end(), [id](const auto& unit) { return unit.id == id; })) {
            selected.push_back(row.planet); break;
        }
    detach(); ++data_->work.requests;
    Data::ControlOwner owner(*data_, selected);
    const std::array<EntityId, 1> units{id}; bool admission_ran{};
    auto launched = session_detail::launch_native_surface_units(owner, player, units, target, 0,
        [&](const CommandPayload& payload) {
            const auto issuer = std::find_if(owner.seats.begin(), owner.seats.end(),
                [player](const auto& row) { return row.player == player && row.active; });
            if (issuer == owner.seats.end()) return core::Result<NativeFleetReceipt>::success({false, 0, GalacticRejectReason::inactive_seat});
            const auto first = data_->events.size();
            session_detail::execute_native_fleet(owner, GameCommand{{data_->base->completed_tick(), issuer->seat_id, 0}, payload},
                data_->base->completed_tick(), data_->events, pending_local_conflict, false);
            NativeFleetReceipt receipt;
            for (auto i = first; i < data_->events.size(); ++i) {
                if (data_->events[i].kind == GalacticEventKind::command_rejected) receipt.reason = data_->events[i].reason;
                else if (data_->events[i].kind == GalacticEventKind::command_executed) {
                    receipt.admitted = true; receipt.object = data_->events[i].subject;
                }
            }
            return core::Result<NativeFleetReceipt>::success(receipt);
        }, admission_ran);
    owner.finish(); data_->last_admission_ran = admission_ran; data_->last_published = admission_ran;
    if (!launched) return launched;
    if (!admission_ran) data_->publication = before;
    data_->record_request(Data::ControlRequest{Data::ControlRequest::Kind::launch,
        player, id, target, pending_local_conflict, {}, launched.value(), false});
    return launched;
}
core::Result<void> GalacticSession::NativeActionShadow::recheck_orbit(PlayerId player, FleetId id) {
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != player)
        || data_->work.requests >= data_->max_requests || !data_->movement)
        return invalid<void>("invalid native orbit recheck layer request or exhausted budget");
    const auto* fleet = data_->fleet(id);
    if (!fleet || fleet->owner != player || !fleet->orbit || fleet->motion.transit)
        return invalid<void>("native orbit recheck differs from its owned planet parent");
    const auto destination = fleet->orbit;
    const auto before = data_->view();
    detach(); data_->publication = before; ++data_->work.requests;
    Data::FleetOwner owner(*data_);
    auto conflict = owner.orbit_conflict(*fleet);
    if (!conflict) return core::Result<void>::failure(conflict.error());
    const auto ids = owner.movement_fleet_ids();
    const auto visit = [&](auto consume) { owner.visit_movement_fleets(ids, consume); };
    if (auto linked = session_detail::link_native_movement_orbit(owner, visit, id, destination,
        data_->base->completed_tick(), data_->events); !linked) return linked;
    if (conflict.value()) {
        if (auto gathered = session_detail::gather_native_movement_orbit(owner, visit, id, destination,
            data_->base->completed_tick(), data_->events); !gathered) return gathered;
        data_->events.push_back({data_->base->completed_tick(), GalacticEventKind::movement_conflict, {}, id, 0, destination, GalacticRejectReason::none});
        if (data_->movement_listener) data_->movement_listener(data_->events.back());
    }
    owner.finish();
    data_->record_request(Data::OrbitRecheckRequest{player, id});
    return core::Result<void>::success();
}
core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::NativeActionShadow::land_transports(
    PlayerId player, FleetId id, bool pending_local_conflict) {
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != player) || data_->work.requests >= data_->max_requests)
        return invalid<NativeFleetReceipt>("invalid native fleet landing layer request or exhausted budget");
    const auto before = data_->view();
    const auto* fleet = data_->fleet(id);
    const std::array<PlanetId, 1> selected{fleet ? fleet->orbit : PlanetId{}};
    detach(); ++data_->work.requests;
    Data::ControlOwner owner(*data_, selected);
    auto landed = session_detail::land_native_transports_rows(owner, player, id, pending_local_conflict, data_->events);
    owner.finish();
    if (!landed) return core::Result<NativeFleetReceipt>::failure(landed.error());
    data_->last_published = landed.value().published;
    if (!landed.value().published) data_->publication = before;
    const auto receipt = landed.value().receipt;
    data_->record_request(Data::ControlRequest{Data::ControlRequest::Kind::fleet_land,
        player, id, 0, pending_local_conflict, {}, receipt, false});
    return core::Result<NativeFleetReceipt>::success(receipt);
}
core::Result<GalacticSession::NativeTransportLandingReceipt> GalacticSession::NativeActionShadow::land_transport(
    PlayerId player, EntityId actor, PlanetId planet, bool pending_local_conflict, std::optional<std::uint32_t> live_landed) {
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != player) || data_->work.requests >= data_->max_requests)
        return invalid<NativeTransportLandingReceipt>("invalid native actor landing layer request or exhausted budget");
    const auto before = data_->view();
    const std::array<PlanetId, 1> selected{planet};
    detach(); ++data_->work.requests;
    Data::ControlOwner owner(*data_, selected);
    auto landed = session_detail::land_native_transport_rows(owner, player, actor, planet, pending_local_conflict, live_landed, data_->events);
    owner.finish();
    if (!landed) return core::Result<NativeTransportLandingReceipt>::failure(landed.error());
    data_->last_published = landed.value().published;
    if (!landed.value().published) data_->publication = before;
    const auto receipt = landed.value().receipt;
    data_->record_request(Data::ControlRequest{Data::ControlRequest::Kind::actor_land,
        player, actor, planet, pending_local_conflict, live_landed, receipt.event, receipt.linked});
    return core::Result<NativeTransportLandingReceipt>::success(receipt);
}
core::Result<GalacticSession::NativeProductionReceipt> GalacticSession::NativeActionShadow::admit_production(
    const NativeProductionRequest& request, const PartitionExecutor& executor, const NativeProductionHandler& notify,
    std::span<const TypeId> local_unlocks) {
    using Result = core::Result<NativeProductionReceipt>;
    if (!data_->player || (!data_->tail && !data_->prelude && data_->player != request.player) || data_->work.requests >= data_->max_requests)
        return invalid<NativeProductionReceipt>("invalid native production layer request or exhausted budget");
    const auto before = data_->view();
    if (!before->production() || !before->player_economy() || !planet(request.planet))
        return invalid<NativeProductionReceipt>("native production requires a live producer");
    detach(); ++data_->work.requests;
    auto current = data_->view();
    if (data_->population_schema && data_->population_stale) {
        const auto detached = current->fleet_control() ? std::span<const DetachedUnit>(current->fleet_control()->detached_units) : std::span<const DetachedUnit>{};
        auto projected = session_detail::project_native_population(*data_->population_schema, current->planets(), current->fleets(), detached,
            current->control() ? &*current->control() : nullptr, current->movement(), current->commissioned_originals(), *current->production(), executor);
        if (!projected) { data_->publication = before; return Result::failure(projected.error()); }
        auto result = std::move(projected).value();
        auto production = std::make_shared<std::vector<ProductionPlayer>>(current->production()->players);
        static_cast<void>(session_detail::apply_native_population(*production, result.totals, result.resolved_counts));
        data_->production_projection = std::move(production);
        data_->population_projection = std::make_shared<const std::vector<PopulationTotal>>(std::move(result.totals));
        data_->population_stale = false; data_->publication.reset(); current = data_->view();
    }
    std::optional<IncomeSetup> sources;
    std::vector<session_detail::NativeIncomeSourceCandidate> candidates;
    if (current->income() && data_->income_schema) {
        sources = *data_->income_schema;
        const auto detached = current->fleet_control() ? std::span<const DetachedUnit>(current->fleet_control()->detached_units) : std::span<const DetachedUnit>{};
        session_detail::project_native_income(*sources, *data_->income_schema, current->control() ? &*current->control() : nullptr,
            current->planets(), current->fleets(), detached, &*current->production(), current->actor_records(), candidates);
    }
    auto checked = session_detail::prepare_native_production(request, *current->production(), *current->player_economy(), current->planets(),
        current->fleets(), current->population(), bool(data_->population_schema), sources ? &*sources : nullptr,
        data_->next_id, data_->live_objects, local_unlocks);
    // A refused offer retains the refreshed private census, but no publication.
    data_->publication = before;
    if (!checked) return Result::failure(checked.error());
    const auto& prepared = checked.value();
    if (!prepared.offer || prepared.admission != ProductionAdmission::allowed) {
        const NativeProductionReceipt receipt{prepared.admission, 0, prepared.offer ? prepared.offer->price : math::Fixed{}};
        data_->record_request(Data::ProductionRequest{request, receipt});
        return Result::success(receipt);
    }
    if (!notify) return invalid<NativeProductionReceipt>("native production requires its ledger handler");
    if (!request.mark_cost || request.mark_cost->raw() < 0 || request.mark_cost->raw() > (std::int64_t{1} << 48)
        || request.mark_cost->raw() % math::Fixed::scale != 0)
        return invalid<NativeProductionReceipt>("native mark requires the resolved null-location integer type cost");
    const auto& offer = *prepared.offer; const auto id = data_->next_id;
    if (data_->independent_player && id >= data_->range_end)
        return invalid<NativeProductionReceipt>("native player identity block exhausted");
    if (auto marked = notify({NativeProductionNotice::Kind::marked, request.player, id, *request.mark_cost, offer.price, offer.seconds, prepared.owner}); !marked)
        return Result::failure(marked.error());
    auto row = current->production()->planets[prepared.planet_index];
    row.queues[static_cast<std::size_t>(offer.domain)].push_back({offer.type, request.player, id, offer.price, offer.seconds, offer.seconds,
        data_->base->clock().galactic_frame, production_frames(offer.seconds).value(), true});
    data_->production_rows.insert_or_assign(request.planet, std::move(row)); ++data_->next_id;
    data_->population_stale = true; data_->publication.reset();
    data_->events.push_back({data_->base->completed_tick(), GalacticEventKind::production_started, {}, request.planet, id, request.type, GalacticRejectReason::none});
    const NativeProductionReceipt receipt{ProductionAdmission::allowed, id, offer.price};
    data_->record_request(Data::ProductionRequest{request, receipt});
    return Result::success(receipt);
}
void GalacticSession::NativeActionShadow::detach() {
    if (data_.use_count() != 1) {
        data_ = std::make_shared<Data>(*data_);
        data_->work.copied_fleet_rows += data_->fleet_rows.size();
        data_->work.copied_actor_rows += data_->actor_rows.size();
    }
    data_->publication.reset();
}
EntityId GalacticSession::NativeActionShadow::next_entity_id() const noexcept { return data_->next_id; }
std::uint64_t GalacticSession::NativeActionShadow::next_native_invasion_id() const noexcept { return data_->next_invasion; }
PlayerId GalacticSession::NativeActionShadow::player() const noexcept { return data_->player; }
const GalacticSession::NativeActionShadow::IntentSink& GalacticSession::NativeActionShadow::intent_sink() const noexcept {
    return data_->intent_sink;
}
std::span<const GalacticSession::NativeActionShadow::IntentRecord> GalacticSession::NativeActionShadow::intents() const noexcept {
    return data_->action_requests;
}
core::Result<GalacticSession::NativeActionShadow> GalacticSession::NativeActionShadow::for_player(
    PlayerId player, std::size_t max_requests) const {
    if (data_->independent_player || data_->tail || !data_->player || !player || player <= data_->player || !max_requests || max_requests > galactic_max_objects
        || std::none_of(data_->base->players().begin(), data_->base->players().end(),
            [player](const auto& row) { return row.player_id == player; }))
        return invalid<NativeActionShadow>("native player layers require ascending known players and bounded requests");
    auto state = std::make_shared<Data>();
    state->owner = data_->owner; state->base = data_->base; state->parent = data_;
    state->inherit_reads(data_);
    state->player = player; state->first_id = state->next_id = data_->next_id;
    state->live_objects = data_->live_objects; state->max_requests = max_requests;
    state->can_create = data_->can_create; state->victory = data_->victory;
    state->graph = data_->graph; state->movement = data_->movement; state->movement_listener = data_->movement_listener;
    state->population_schema = data_->population_schema; state->income_schema = data_->income_schema;
    state->fiscal_schema = data_->fiscal_schema; state->base_income = data_->base_income; state->income_edges = data_->income_edges;
    state->production_projection = data_->production_projection; state->population_projection = data_->population_projection;
    state->observer_generation = data_->observer_generation; state->population_stale = data_->population_stale;
    state->invasion_records = data_->invasion_records; state->invasion_transports = data_->invasion_transports;
    state->first_invasion = state->next_invasion = data_->next_invasion; state->invasions_enabled = data_->invasions_enabled;
    state->publication = data_->view(); // freeze the predecessor publication before sharing it
    if (state->publication->victory()) state->victory = std::shared_ptr<const VictoryState>(state->publication, &*state->publication->victory());
    return core::Result<NativeActionShadow>::success(NativeActionShadow(std::move(state)));
}
core::Result<GalacticSession::NativeActionShadow> GalacticSession::NativeActionShadow::fork_player(
    PlayerId player, EntityId first_id, std::size_t max_requests, std::optional<std::uint64_t> first_invasion, IntentSink sink) const {
    const auto invasion = first_invasion.value_or(data_->next_invasion);
    if (data_->independent_player || data_->tail || !data_->player || !player || !max_requests || max_requests > galactic_max_objects
        || first_id < data_->next_id || first_id >= std::numeric_limits<EntityId>::max() - galactic_max_objects
        || invasion < data_->next_invasion || invasion >= std::numeric_limits<std::uint64_t>::max() - galactic_max_objects
        || std::none_of(data_->base->players().begin(), data_->base->players().end(),
            [player](const auto& row) { return row.player_id == player; }))
        return invalid<NativeActionShadow>("native root forks require known players, bounded requests and a reserved identity block");
    auto state = std::make_shared<Data>();
    state->owner = data_->owner; state->base = data_->base; state->parent = data_;
    state->inherit_reads(data_);
    state->player = player; state->first_id = state->next_id = first_id;
    state->range_end = first_id + galactic_max_objects; state->independent_player = true;
    state->live_objects = data_->live_objects; state->max_requests = max_requests;
    state->can_create = data_->can_create; state->victory = data_->victory;
    state->graph = data_->graph; state->movement = data_->movement; state->movement_listener = data_->movement_listener;
    state->population_schema = data_->population_schema; state->income_schema = data_->income_schema;
    state->fiscal_schema = data_->fiscal_schema; state->base_income = data_->base_income; state->income_edges = data_->income_edges;
    state->production_projection = data_->production_projection; state->population_projection = data_->population_projection;
    state->observer_generation = data_->observer_generation; state->population_stale = data_->population_stale;
    state->invasion_records = data_->invasion_records; state->invasion_transports = data_->invasion_transports;
    state->first_invasion = state->next_invasion = invasion; state->intent_sink = std::move(sink); state->invasion_range_end = invasion + galactic_max_objects;
    state->invasions_enabled = data_->invasions_enabled;
    state->publication = data_->view();
    if (state->publication->victory()) state->victory = std::shared_ptr<const VictoryState>(state->publication, &*state->publication->victory());
    return core::Result<NativeActionShadow>::success(NativeActionShadow(std::move(state)));
}
const GalacticSession::NativeActionShadow::Work& GalacticSession::NativeActionShadow::work() const noexcept { return data_->work; }
std::span<const GalacticSession::NativeActorChange> GalacticSession::NativeActionShadow::actor_changes() const noexcept { return data_->actor_changes; }
const Planet* GalacticSession::NativeActionShadow::planet(PlanetId id) const {
    const auto rows = data_->base->planets();
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.id < key; });
    return at != rows.end() && at->id == id ? &*at : nullptr;
}
std::optional<math::Fixed> GalacticSession::NativeActionShadow::credits(PlayerId id) const {
    for (const Data* layer = data_.get(); layer; layer = layer->read_parent.get()) {
        const auto changed = layer->accounts.find(id);
        if (changed != layer->accounts.end()) return changed->second;
    }
    if (!data_->base->player_economy()) return {};
    const auto& rows = data_->base->player_economy()->players;
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.player < key; });
    return at != rows.end() && at->player == id ? std::optional(at->credits) : std::nullopt;
}
const CensusObject* GalacticSession::NativeActionShadow::created_object(EntityId id) const {
    for (const Data* layer = data_.get(); layer; layer = layer->read_parent.get()) for (const auto& force : layer->forces) {
        const auto& rows = force->placement.originals;
        const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; });
        if (at != rows.end()) return &*at;
    }
    return nullptr;
}
core::Result<math::Fixed> GalacticSession::NativeActionShadow::add_credits(PlayerId id, math::Fixed delta, math::Fixed multiplier) {
    auto before = credits(id);
    if (!before || data_->work.requests >= data_->max_requests)
        return invalid<math::Fixed>("native cash needs a known account within the request budget");
    const auto& accounts = data_->base->player_economy()->players;
    const auto at = std::lower_bound(accounts.begin(), accounts.end(), id, [](const auto& row, auto key) { return row.player < key; });
    auto balance = adjusted_credits(*before, delta, multiplier, at->credit_cap);
    if (!balance) return balance;
    auto actual = math::subtract(balance.value(), *before);
    if (!actual) return actual;
    std::shared_ptr<const VictoryState> staged_victory = data_->victory;
    if (data_->player && !data_->immediate_batch) {
        const auto current = data_->view();
        if (current->victory()) staged_victory = std::shared_ptr<const VictoryState>(current, &*current->victory());
    }
    bool victory_pending = false;
    // WGEP-16 / WGS-18: same scalar credit-condition admission as add_credits.
    if (staged_victory && (!staged_victory->pending || staged_victory->waits)) {
        auto victory = std::make_shared<VictoryState>(*staged_victory);
        VictoryContext context;
        context.multiplayer = victory->context.multiplayer;
        context.tutorial_delayed = victory->context.tutorial_delayed;
        context.players = victory->context.players;
        for (auto& player : context.players) {
            if (player.player == id) player.credits = balance.value();
            else if (auto value = credits(player.player)) player.credits = *value;
            if (data_->base->seat_control()) for (const auto& owner : data_->base->seat_control()->control.controllers) if (owner.player == player.player) {
                player.ai = owner.kind == FactionControl::ai; player.human = owner.kind == FactionControl::human;
            }
        }
        const InlineExecutor scalar;
        auto tested = test_victory(*victory, {VictoryKind::credits, id}, context, scalar, data_->base->clock().galactic_frame);
        if (!tested) return core::Result<math::Fixed>::failure(tested.error());
        victory_pending = tested.value();
        staged_victory = std::move(victory);
    }
    detach(); ++data_->work.requests; ++data_->work.copied_accounts;
    data_->accounts[id] = balance.value();
    data_->victory = std::move(staged_victory);
    if (victory_pending) data_->events.push_back({data_->base->completed_tick(), GalacticEventKind::victory_pending, {}, id, 0,
        static_cast<std::uint64_t>(VictoryKind::credits), GalacticRejectReason::none});
    if (data_->player && (!data_->immediate_batch || data_->independent_player) && !data_->suppress_cash_request) data_->record_request(Data::CashRequest{id, delta, multiplier, actual.value()});
    return actual;
}
core::Result<std::vector<EntityId>> GalacticSession::NativeActionShadow::create_force(const NativeProductionRequest& request,
    const NativeShadowForceBuilder& builder, const PartitionExecutor& executor) {
    using Result = core::Result<std::vector<EntityId>>;
    const auto current = data_->player ? data_->view() : data_->base;
    const auto& base = *current;
    if (!builder || !data_->can_create || !request.type || !base.production() || !base.production()->nested_creation || !base.control() || !base.movement()
        || !planet(request.planet) || data_->work.requests >= data_->max_requests
        || std::none_of(base.players().begin(), base.players().end(), [&](const auto& row) { return row.player_id == request.player; }))
        return invalid<std::vector<EntityId>>("native placement needs a qualified local provider and request budget");
    detach();
    std::shared_ptr<Data::Location> location;
    for (const Data* layer = data_->player ? nullptr : data_.get(); layer && !location; layer = layer->read_parent.get()) {
        const auto existing = layer->locations.find(request.planet);
        if (existing != layer->locations.end()) location = std::make_shared<Data::Location>(*existing->second);
    }
    if (!location) {
        const auto& planets = base.production()->planets;
        const auto production = std::lower_bound(planets.begin(), planets.end(), request.planet, [](const auto& row, auto id) { return row.planet < id; });
        const auto& presence = base.control()->planets;
        const auto control = std::lower_bound(presence.begin(), presence.end(), request.planet, [](const auto& row, auto id) { return row.planet < id; });
        if (production == planets.end() || production->planet != request.planet || control == presence.end() || control->planet != request.planet)
            return invalid<std::vector<EntityId>>("native placement location has no presence");
        location = std::make_shared<Data::Location>(); location->planet = *planet(request.planet);
        location->presence = *control; location->production = *production;
        // Read-only scene selection stays partitioned. Only this planet's
        // orbit is copied; neither its neighbours nor an ECS registry clone.
        std::vector<std::vector<Fleet>> selected(tick_partition_count);
        auto gathered = executor.execute_phase("native-force-local-inputs", tick_partition_count, [&](std::size_t partition) {
            const auto range = partition_range(partition, base.fleets().size());
            for (auto index = range.begin; index < range.end; ++index) if (base.fleets()[index].orbit == request.planet)
                selected[partition].push_back(base.fleets()[index]);
        });
        if (!gathered) return Result::failure(gathered.error());
        auto orbit = std::make_shared<std::vector<Fleet>>();
        for (auto& rows : selected) for (auto& row : rows) orbit->push_back(std::move(row));
        auto contents = std::make_shared<std::vector<MovementContents>>();
        std::vector<EntityId> pending;
        for (const auto& fleet : *orbit) for (const auto& member : fleet.members) pending.push_back(member.id);
        std::set<EntityId> visited;
        while (!pending.empty()) {
            std::sort(pending.begin(), pending.end());
            pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
            std::erase_if(pending, [&](auto id) { return !visited.insert(id).second; });
            if (pending.empty()) break;
            std::vector<std::vector<MovementContents>> nested(tick_partition_count);
            auto collected = executor.execute_phase("native-force-local-contents", tick_partition_count, [&](std::size_t partition) {
                const auto range = partition_range(partition, pending.size());
                const auto& rows = base.movement()->contents;
                for (auto index = range.begin; index < range.end; ++index) {
                    const auto id = pending[index];
                    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.container < key; });
                    if (at != rows.end() && at->container == id) nested[partition].push_back(*at);
                }
            });
            if (!collected) return Result::failure(collected.error());
            pending.clear();
            for (auto& rows : nested) for (auto& row : rows) {
                for (const auto& child : row.children) pending.push_back(child.id);
                contents->push_back(std::move(row));
            }
        }
        std::sort(contents->begin(), contents->end(), [](const auto& a, const auto& b) { return a.container < b.container; });
        data_->work.copied_orbital_objects += orbit->size() + visited.size();
        location->orbit = std::move(orbit); location->contents = std::move(contents);
    }
    const auto skip_reader = [&](const Fleet& fleet) -> core::Result<bool> {
        std::vector<CensusObject> records;
        MovementSetup movement;
        std::vector<EntityId> pending;
        for (const auto& member : fleet.members) pending.push_back(member.id);
        std::set<EntityId> visited;
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (!visited.insert(id).second) return invalid<bool>("actor_combat_skip_container_cycle");
            const auto* actor = data_->player ? this->actor(id) : created_object(id);
            if (!actor) {
                const auto rows = base.actor_records();
                const auto at = std::lower_bound(rows.begin(), rows.end(), id,
                    [](const auto& row, auto key) { return row.id < key; });
                if (at == rows.end() || at->id != id) return invalid<bool>("actor_combat_skip_unavailable");
                actor = &*at;
            }
            records.push_back(*actor);
            if (actor->delete_pending || actor->combat_role == ActorCombatRole::ordinary) continue;
            const auto& contents = *location->contents;
            const auto at = std::lower_bound(contents.begin(), contents.end(), id,
                [](const auto& row, auto key) { return row.container < key; });
            if (at == contents.end() || at->container != id) return invalid<bool>("actor_payload_unavailable");
            movement.contents.push_back(*at);
            for (const auto& child : at->children) pending.push_back(child.id);
        }
        std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(movement.contents.begin(), movement.contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
        return fleet_combat_skip(fleet, records, &movement);
    };
    NativeForceView view{location->planet, location->presence, location->production, *base.production()->nested_creation,
        base.control()->max_ground_forces, *location->orbit, *location->contents, skip_reader};
    auto made = builder(view, data_->next_id);
    if (!made) return Result::failure(made.error());
    if (data_->independent_player && made.value().next_id > data_->range_end)
        return invalid<std::vector<EntityId>>("native player identity block exhausted");
    auto actors = validate_native_force(request, made.value(), data_->next_id, data_->live_objects,
        *base.production()->nested_creation, [&](EntityId id) {
            const auto* private_actor = created_object(id);
            return private_actor ? private_actor->current_type != 0 : bool(base.current_type(id));
        });
    if (!actors) return actors;
    const auto& placement = made.value();
    auto orbit = std::make_shared<std::vector<Fleet>>(*location->orbit);
    orbit->insert(orbit->end(), placement.fleets.begin(), placement.fleets.end());
    std::sort(orbit->begin(), orbit->end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    auto contents = std::make_shared<std::vector<MovementContents>>(*location->contents);
    contents->insert(contents->end(), placement.contents.begin(), placement.contents.end());
    std::sort(contents->begin(), contents->end(), [](const auto& a, const auto& b) { return a.container < b.container; });
    location->orbit = std::move(orbit); location->contents = std::move(contents);
    auto& presence = location->presence; auto& production = location->production;
    presence.landed.insert(presence.landed.end(), placement.landed.begin(), placement.landed.end());
    production.landed_transports = static_cast<std::int64_t>(presence.landed.size());
    production.objects.insert(production.objects.end(), placement.structures.begin(), placement.structures.end());
    if (!placement.structures.empty()) presence.special_control = placement.special_control;
    if (placement.ground_base) { presence.ground_base = *placement.ground_base; production.ground_level = placement.ground_base->level; }
    if (placement.starbase) { presence.starbase = *placement.starbase; production.space_level = placement.starbase->level;
        production.starbase_owner = placement.starbase->owner; production.starbase_type = placement.starbase_type; }
    ++data_->work.requests; ++data_->work.copied_planets;
    data_->work.prepared_objects += placement.next_id - data_->next_id;
    data_->live_objects += static_cast<std::size_t>(placement.next_id - data_->next_id);
    data_->next_id = placement.next_id;
    if (data_->player) {
        data_->control_rows.insert_or_assign(request.planet, location->presence);
        data_->production_rows.insert_or_assign(request.planet, location->production);
        auto movement = std::make_shared<MovementSetup>(*data_->movement);
        movement->contents.insert(movement->contents.end(), placement.contents.begin(), placement.contents.end());
        std::sort(movement->contents.begin(), movement->contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
        data_->movement = std::move(movement);
        Data::FleetOwner owner(*data_);
        for (const auto& fleet : placement.fleets) owner.insert(fleet);
        owner.finish();
        auto projected_victory = current->victory();
        const auto ordered = ordered_births(placement.originals).value();
        std::map<EntityId, const CensusObject*> originals;
        for (const auto& row : placement.originals) originals.emplace(row.id, &row);
        for (const auto& birth : ordered) {
            auto row = *originals.at(birth.actor);
            if (row.combat_role == ActorCombatRole::unavailable) {
                const auto& types = base.production()->nested_creation->types;
                const auto fact = std::lower_bound(types.begin(), types.end(), row.current_type, [](const auto& value, auto id) { return value.type < id; });
                if (fact != types.end() && fact->type == row.current_type && fact->combat_role != ActorCombatRole::unavailable) {
                    row.combat_role = fact->combat_role; row.abilities = fact->abilities; row.garrison = false;
                }
            }
            if (projected_victory) session_detail::update_native_victory_actor(*projected_victory, row);
            data_->actor_rows.insert_or_assign(row.id, std::move(row)); ++data_->observer_generation;
        }
        if (projected_victory && !data_->immediate_batch)
            data_->victory = std::make_shared<const VictoryState>(std::move(*projected_victory));
        if (base.observer() && !placement.landed.empty()) {
            const auto* shown = session_detail::native_find_row(base.observer()->planets, request.planet, [](const auto& row) { return row.planet; });
            if (shown) {
                auto row = *shown;
                for (const auto& unit : placement.landed) row.landed_transports.push_back({unit.id, unit.owner, false, false});
                data_->observer_rows.insert_or_assign(request.planet, std::move(row));
            }
        }
        ++data_->observer_generation; data_->population_stale = true; data_->publication.reset();
        if (base.production()->fleet_blockers) {
            const auto projected = data_->view();
            for (const auto& fleet : placement.fleets) {
                auto skip = fleet_combat_skip(*data_->fleet(fleet.id), projected->actor_records(), data_->movement.get());
                if (!skip) return Result::failure(skip.error());
                data_->fleet_blockers[fleet.id] = skip.value(); data_->created_blockers.push_back(fleet.id);
            }
        }
        data_->publication.reset();
    }
    data_->locations[request.planet] = std::move(location);
    const auto births = ordered_births(placement.originals).value();
    data_->actor_changes.insert(data_->actor_changes.end(), births.begin(), births.end());
    data_->forces.push_back(std::make_shared<const Data::Force>(Data::Force{request, std::move(made).value()}));
    if (data_->player && (!data_->immediate_batch || data_->independent_player)) data_->record_request(Data::ForceRequest{data_->forces.back(), actors.value()});
    data_->publication.reset();
    return actors;
}
} // namespace eawr::sim::galactic
