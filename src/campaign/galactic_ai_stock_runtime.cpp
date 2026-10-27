#include "galactic_ai_runtime.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "../script/foc/galactic_ai_parent_context_runtime.hpp"
#include "../script/foc/galactic_ai_battle_listener.hpp"
#include "../script/foc/galactic_ai_movement_runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>

namespace eawr::campaign {
namespace ga = script::foc::galactic_ai;
namespace {
template<class T> core::Result<T> absent(std::string_view name) {
    core::Diagnostic error; error.code = "EAWR-GCAI-RUNTIME-0001";
    error.message = "galactic AI stock provider absent: " + std::string(name);
    return core::Result<T>::failure(std::move(error));
}
const sim::galactic::CensusObject* actor_record(const sim::galactic::GalacticSnapshot& snapshot,
    sim::EntityId actor, const sim::galactic::GalacticSession::NativeActionShadow* shadow = nullptr) {
    if (shadow) if (const auto* created = shadow->created_object(actor)) return created;
    const auto records = snapshot.actor_records();
    const auto found = std::lower_bound(records.begin(), records.end(), actor,
        [](const auto& row, auto id) { return row.id < id; });
    return found != records.end() && found->id == actor ? &*found : nullptr;
}
core::Result<ga::FreestoreFacts> stock_actor_facts(const script::foc::ai::GalacticSelectionContent& content,
    const FreestoreMemberQualifier& members, const sim::galactic::GalacticSnapshot& snapshot, sim::EntityId actor,
    const sim::galactic::GalacticSession::NativeActionShadow* shadow = nullptr, unsigned depth = 0) {
    // Match the native 32-level container bound. The top-level fleet is
    // resolved directly, without another recursive actor-facts call.
    if (depth > 32) return absent<ga::FreestoreFacts>("bounded literal fleet parent hierarchy");
    const auto* actual = actor_record(snapshot, actor, shadow);
    if (!actual || !actual->current_type || !members) return absent<ga::FreestoreFacts>("actual typed actor/current representation");
    const auto original = content.types.find(actual->original_type), current = content.types.find(actual->current_type);
    if (original == content.types.end() || current == content.types.end() || !current->second.original)
        return absent<ga::FreestoreFacts>("immutable original/current actor traits");
    const auto member = members(actual->current_type);
    if (!member) return core::Result<ga::FreestoreFacts>::failure(member.error());
    const auto has = [](const auto& type, std::string_view name) {
        for (const auto* list : {&type.behavior, &type.space_behavior, &type.land_behavior})
            if (std::find(list->begin(), list->end(), name) != list->end()) return true;
        return false;
    };
    const auto planet = [&](auto id) {
        const auto planets = snapshot.planets();
        const auto found = std::lower_bound(planets.begin(), planets.end(), id, [](const auto& row, auto key) { return row.id < key; });
        return found != planets.end() && found->id == id;
    };
    ga::FreestoreFacts row;
    row.object.id = actor; row.object.type = actual->original_type; row.object.owner = actual->owner;
    row.current_type = actual->current_type; row.current_type_member = member.value();
    row.hero = original->second.value.hero; // WGA-312: original selection identity.
    row.dead = actual->dead; row.deleting = actual->delete_pending; row.qualified = true;
    row.parent = actual->container;
    const auto& traits = *current->second.original;
    row.object.transport = has(traits, "TRANSPORT");
    row.unit_ai = has(traits, "UNIT_AI");
    if (planet(actor)) { row.object.kind = ga::FreestoreKind::planet; row.object.planet = actor; }
    else if (has(traits, "FLEET")) row.object.kind = ga::FreestoreKind::fleet;
    else if (has(traits, "DUMMY_GROUND_STRUCTURE") || has(traits, "DUMMY_GROUND_BASE")) row.object.kind = ga::FreestoreKind::ground_structure;
    else if (has(traits, "DUMMY_ORBITAL_STRUCTURE") || has(traits, "DUMMY_STAR_BASE")) row.object.kind = ga::FreestoreKind::space_structure;
    if (row.parent && planet(row.parent)) {
        row.parent_is_planet = row.object.directly_parented_to_planet = true;
        row.object.planet = row.parent;
    } else if (row.parent) {
        const auto* parent = actor_record(snapshot, row.parent, shadow);
        const auto parent_type = parent ? content.types.find(parent->current_type) : content.types.end();
        if (!parent || parent_type == content.types.end() || !parent_type->second.original)
            return absent<ga::FreestoreFacts>("literal immediate actor parent/current type");
        if (has(*parent_type->second.original, "FLEET")) {
            const auto fleets = snapshot.fleets();
            const auto fleet = std::lower_bound(fleets.begin(), fleets.end(), row.parent,
                [](const auto& value, auto id) { return value.id < id; });
            if (fleet != fleets.end() && fleet->id == row.parent) {
                row.enroute = fleet->motion.transit.has_value();
                row.object.planet = row.enroute ? 0 : fleet->orbit;
            } else if (shadow && shadow->created_object(row.parent)) row.object.planet = parent->planet;
            else {
                // A nested fleet/squadron owns an explicit movement breakdown,
                // rather than a row in the top-level fleet owner. Preserve the
                // literal parent and inherit its qualified location/transit.
                const auto* movement = snapshot.movement();
                if (!movement) return absent<ga::FreestoreFacts>("actual immediate fleet location/transit");
                const auto nested = std::lower_bound(movement->contents.begin(), movement->contents.end(), row.parent,
                    [](const auto& value, auto id) { return value.container < id; });
                if (nested == movement->contents.end() || nested->container != row.parent)
                    return absent<ga::FreestoreFacts>("actual immediate fleet location/transit");
                const auto located = stock_actor_facts(content, members, snapshot, row.parent, shadow, depth + 1);
                if (!located) return core::Result<ga::FreestoreFacts>::failure(located.error());
                row.object.planet = located.value().object.planet; row.enroute = located.value().enroute;
            }
        }
    }
    const auto fleets = snapshot.fleets();
    const auto own_fleet = std::lower_bound(fleets.begin(), fleets.end(), actor, [](const auto& value, auto id) { return value.id < id; });
    if (own_fleet != fleets.end() && own_fleet->id == actor) row.enroute = own_fleet->motion.transit.has_value();
    row.object.transit = !row.object.planet || row.enroute;
    row.object.safe = !row.object.transport || row.enroute || row.parent_is_planet ? std::optional(true) : std::nullopt;
    return core::Result<ga::FreestoreFacts>::success(std::move(row));
}
}
core::Result<ga::FreestoreFacts> read_stock_galactic_ai_actor_facts(
    const script::foc::ai::GalacticSelectionContent& content, const FreestoreMemberQualifier& members,
    const sim::galactic::GalacticSnapshot& snapshot, sim::EntityId actor,
    const sim::galactic::GalacticSession::NativeActionShadow* shadow) {
    return stock_actor_facts(content, members, snapshot, actor, shadow);
}
core::Result<ga::NativeActions::FreestoreMoveInput> read_stock_galactic_ai_freestore_move(
    const GalacticAiUsability& usable, sim::galactic::PlayerId player, const ga::FreestoreFacts& facts,
    const sim::galactic::GalacticSnapshot& snapshot) {
    using Result = core::Result<ga::NativeActions::FreestoreMoveInput>;
    ga::NativeActions::FreestoreMoveInput out;
    const auto* actual = actor_record(snapshot, facts.object.id);
    out.actor_valid = actual && actual->owner == player && !actual->dead && !actual->delete_pending;
    if (!out.actor_valid) return Result::success(out);
    // WGA-400..404: no resolved source is not restricted and reads no planet
    // permission. Native preflight then rejects transit before containment/no-op.
    out.source_ai_usable = true;
    if (facts.object.planet) {
        if (!usable) return absent<ga::NativeActions::FreestoreMoveInput>("source-planet AI usability");
        const auto permitted = usable(player, snapshot, facts.object.planet);
        if (!permitted) return Result::failure(permitted.error());
        out.source_ai_usable = permitted.value();
    }
    if (!out.source_ai_usable || facts.object.transit) return Result::success(out);
    if (actual->container && !facts.parent_is_planet) {
        const auto* parent = actor_record(snapshot, actual->container);
        if (!parent) return absent<ga::NativeActions::FreestoreMoveInput>("literal freestore immediate parent");
        out.grandparent = parent->container;
    }
    return Result::success(out);
}
core::Result<ga::NativeActions::FreestorePathInput> copy_galactic_ai_freestore_path(
    const ga::ExecutionRoutes& routes, sim::galactic::PlayerId player,
    const sim::galactic::GalacticSnapshot& snapshot, sim::galactic::PlanetId source, sim::galactic::PlanetId destination) {
    using Result = core::Result<ga::NativeActions::FreestorePathInput>;
    auto cached = ga::cached_execution_path(routes, player, source, destination);
    if (!cached) return Result::failure(cached.error());
    ga::NativeActions::FreestorePathInput out;
    out.planets = std::move(cached.value().planets); out.threat = cached.value().threat;
    // WGA-386: safety precedes next-hop occupancy. A terminal cached route
    // does not consult a nonexistent hop. Reads never rebuild the matrix.
    if (out.threat > ga::Number(0) || out.planets.size() < 2) return Result::success(std::move(out));
    const auto hop = out.planets[1];
    if (snapshot.control()) {
        const auto& rows = snapshot.control()->state;
        const auto found = std::find_if(rows.begin(), rows.end(), [hop](const auto& row) { return row.planet == hop; });
        if (found != rows.end()) out.next_occupant = found->orbital_player;
    } else if (snapshot.production()) {
        const auto& rows = snapshot.production()->planets;
        const auto found = std::find_if(rows.begin(), rows.end(), [hop](const auto& row) { return row.planet == hop; });
        if (found != rows.end()) out.next_occupant = found->orbit_occupier;
    }
    if (!out.next_occupant) return absent<ga::NativeActions::FreestorePathInput>("WGM-66 qualified current next-hop orbital occupant");
    return Result::success(std::move(out));
}
void bind_galactic_ai_actor_changes(ga::NativeActions& actions,
    GalacticAiSnapshotActorFactsReader actor_facts, std::function<const sim::PartitionExecutor*()> executor) {
    actions.bind_actor_changes([actor_facts, executor]
        (ga::NativeActions& native, const sim::galactic::GalacticSnapshot& snapshot,
            std::span<const sim::galactic::GalacticSession::NativeActorChange> changes) {
        if (changes.empty()) return core::Result<void>::success();
        const auto* workers = executor();
        if (!workers || !actor_facts)
            return absent<void>("sparse actor facts/retained pool/admitted workers");
        auto& retained = native.qualified_freestore();
        auto staged = retained.clone();
        if (auto prepared = apply_galactic_ai_actor_changes(staged, changes,
            [&snapshot, actor_facts](auto actor) { return actor_facts(snapshot, actor); }, *workers); !prepared) return prepared;
        // WGA-307/163: unregister while the old selected-actor facts
        // still exist, before publishing owner/loss/deletion changes.
        for (const auto& change : changes) {
            const auto* previous = retained.facts(change.actor);
            const auto* next = staged.facts(change.actor);
            if (!previous || (next && !next->deleting && previous->object.owner == next->object.owner)) continue;
            if (auto retired = native.retire_freestore_movement(change.actor, !next); !retired) return retired;
            if (next && next->tracked_movement) {
                auto row = *next; row.tracked_movement = false;
                if (auto updated = staged.update(std::move(row)); !updated) return updated;
            }
        }
        retained.replace(staged);
        return core::Result<void>::success();
    });
}
core::Result<void> apply_galactic_ai_actor_changes(ga::QualifiedFreestore& owner,
    std::span<const sim::galactic::GalacticSession::NativeActorChange> changes,
    const GalacticAiActorFactsReader& read, const sim::PartitionExecutor& executor) {
    using Change = sim::galactic::GalacticSession::NativeActorChange;
    if (changes.empty()) return core::Result<void>::success();
    if (!read) return absent<void>("sparse current/original/parent/transit actor facts reader");
    std::vector<std::optional<ga::FreestoreFacts>> prepared(changes.size());
    std::vector<std::optional<core::Diagnostic>> errors(changes.size());
    auto staged_read = executor.execute_phase("galactic-ai-actor-changes", sim::tick_partition_count,
        [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, changes.size());
            for (auto i = range.begin; i < range.end; ++i) {
                if (changes[i].kind == Change::Kind::lost) continue;
                auto facts = read(changes[i].actor);
                if (facts) prepared[i] = std::move(facts).value();
                else errors[i] = facts.error();
            }
        });
    if (!staged_read) return staged_read;
    for (const auto& error : errors) if (error) return core::Result<void>::failure(*error);
    auto staged = owner.clone();
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const auto& change = changes[i];
        if (!change.actor || static_cast<unsigned>(change.kind) > static_cast<unsigned>(Change::Kind::lost))
            return absent<void>("valid sparse actor change identity/kind");
        if (change.kind == Change::Kind::lost) {
            if (change.source || change.dead || change.deleting || change.death_clone)
                return absent<void>("loss notification without unrelated actor mutations");
            staged.lost(change.actor); continue;
        }
        auto row = std::move(*prepared[i]);
        if (row.object.id != change.actor || !row.qualified)
            return absent<void>("qualified facts for the exact changed actor");
        const auto* previous = staged.facts(change.actor);
        if (change.kind == Change::Kind::created) {
            if (previous) return absent<void>("actual fresh actor constructor rather than a repeated birth");
            row = ga::ordinary_freestore_birth(std::move(row));
            if (change.source) {
                const auto* source = staged.facts(change.source);
                if (!source) return absent<void>("actual generated actor source facts");
                row.ai_usable = source->ai_usable; // WGA-315: source state before insertion.
            }
        } else {
            if (!previous || change.source) return absent<void>("retained changed actor without fresh source inheritance");
            // WGA-302/316: a representation/location/owner write never invokes
            // the ordinary constructor over an existing actor's saved flags.
            row.ai_usable = previous->ai_usable; row.dead = previous->dead;
            row.deleting = previous->deleting; row.death_clone = previous->death_clone;
            row.tracked_movement = previous->tracked_movement;
            row.prefer_ground = previous->prefer_ground; // WGA-409: retain across representation/return/owner writes.
        }
        if (change.dead) row.dead = *change.dead;
        if (change.deleting) row.deleting = *change.deleting;
        if (change.death_clone) row.death_clone = *change.death_clone;
        const bool new_owner = previous && previous->object.owner != row.object.owner;
        const auto previous_owner = previous ? previous->object.owner : row.object.owner;
        auto updated = new_owner ? staged.owner_changed(row, staged.initialized(row.object.owner)) : staged.update(row);
        if (!updated) return updated;
        if (row.deleting) staged.remove_membership(previous_owner, change.actor);
        if (change.kind == Change::Kind::created && staged.initialized(row.object.owner)) {
            auto offered = staged.offer(row.object.owner, change.actor);
            if (!offered) return core::Result<void>::failure(offered.error());
        }
    }
    owner.replace(staged);
    return core::Result<void>::success();
}
std::vector<std::string> describe_galactic_ai_goals(const GalacticAiRuntimeView& view, const GalacticAiRuntimeSetup& setup) {
    std::vector<std::string> lines;
    const auto engine = view.engine.lock(); const auto actions = view.actions.lock();
    if (!engine) return lines;
    const auto refusals = engine->galactic_refusals();
    for (const auto& selected : setup.lifecycle.players) {
        struct Row { std::size_t goals{}, active{}, planned{}, running{}; double largest{}; };
        std::map<std::string, Row> by_function;
        std::size_t total{};
        for (const auto& goal : engine->galactic_goal_observations(selected.player)) {
            auto& row = by_function[goal.function];
            ++row.goals; ++total;
            if (goal.active) ++row.active;
            if (goal.planned) ++row.planned;
            if (goal.running_plan) ++row.running;
            row.largest = std::max(row.largest, std::strtod(goal.desire.c_str(), nullptr));
        }
        std::uint64_t movements{};
        if (actions) for (const auto& [request, movement] : actions->movements()) {
            static_cast<void>(request);
            if (movement.player == selected.player) ++movements;
        }
        std::ostringstream line;
        line << "player " << selected.player << ' ' << selected.player_type << ": " << total << " goals, " << movements << " movement rows";
        for (const auto& [name, row] : by_function)
            line << "; " << name << " n=" << row.goals << " active=" << row.active << " planned=" << row.planned
                 << " running=" << row.running << " desire<=" << row.largest;
        std::string refused;
        for (const auto& [key, count] : refusals) if (key.first == selected.player) refused += (refused.empty() ? "" : ", ") + key.second + '=' + std::to_string(count);
        line << "; refusals: " << (refused.empty() ? "none" : refused);
        lines.push_back(line.str());
        for (const auto& goal : engine->galactic_goal_observations(selected.player)) {
            std::size_t move_requests{};
            if (actions && goal.running_plan) for (const auto& [request, movement] : actions->movements()) {
                static_cast<void>(request);
                if (movement.player == selected.player && movement.plan_id == goal.running_plan) ++move_requests;
            }
            std::ostringstream detail;
            detail << "player " << selected.player << " goal=" << goal.goal << " definition=" << goal.function
                   << " target=" << goal.target << " desire=" << goal.desire << " active=" << goal.active
                   << " plan=" << (goal.plan.empty() ? "none" : goal.plan) << " running=" << goal.running_plan
                   << " retained_move_requests=" << move_requests;
            lines.push_back(detail.str());
        }
    }
    return lines;
}
core::Result<GalacticAiRuntimeProviderFactory> stock_galactic_ai_providers(
    std::shared_ptr<const GalacticAiRuntimeSetup> setup, std::shared_ptr<const StartingForceInputs> starting,
    std::shared_ptr<const StockSpaceEntry> space, GalacticAiStockQueryFactory queries,
    std::function<void(GalacticAiRuntimeView)> observe,
    std::function<void(const GalacticAiBattleNotice&)> observe_battle,
    std::function<void(GalacticAiRuntimeView)> watch) {
    using Factory = GalacticAiRuntimeProviderFactory;
    if (!setup || !starting || !space || !queries) return absent<Factory>("immutable setup/placement/child/B query factory");
    Factory factory = [setup, starting, space, queries, observe, observe_battle, watch]() -> core::Result<GalacticAiRuntimeProviders> {
        auto fresh = queries(); if (!fresh) return core::Result<GalacticAiRuntimeProviders>::failure(fresh.error());
        auto query = std::move(fresh).value();
        if (!query.usability || (!query.connectivity && !query.connectivity_publication) || !query.movement || !query.external.contains("GALACTIC_USABILITY"))
            return absent<GalacticAiRuntimeProviders>("qualified B usability/connectivity/movement/native owner");
        GalacticAiRuntimeProviders out; out.external = std::move(query.external); out.observe = observe; out.watch = watch;
        if (!query.freestore_members) query.freestore_members = freestore_member_qualifier(setup->content);
        if (!query.build_budgets) query.build_budgets = [](const auto& engine, const auto& actions, const auto& funding) {
            return engine.galactic_build_budgets(actions, funding);
        };
        if (!query.freestore_selection) query.freestore_selection = [](auto pool, auto world, auto engine, auto snapshot) {
            static_cast<void>(snapshot);
            const auto owner = std::weak_ptr<const GalacticAiRuntimeProviders::Engine>(engine);
            auto selected = ga::qualified_freestore_batch_selection(std::move(pool), std::move(world), std::move(engine),
                [owner](auto player, auto target, const auto& plan) -> ga::FreestoreActorSelection {
                    const auto engine = owner.lock();
                    if (!engine) return [](sim::EntityId) { return absent<std::pair<script::foc::ai::Real, bool>>("retained stage selection owner"); };
                    return [engine, read = engine->galactic_freestore_selection_batch(player, target, plan)](sim::EntityId actor) { return read(actor); };
                });
            return core::Result<script::foc::ai::GalacticFreestoreSelectionProvider>::success(std::move(selected));
        };
        if (!query.freestore_current_type) query.freestore_current_type = [](const auto& snapshot, auto actor) { return snapshot.current_type(actor); };
        if (!query.freestore_actor_facts) query.freestore_actor_facts = [content = setup->content, members = query.freestore_members]
            (const auto& snapshot, auto actor) { return stock_actor_facts(*content, members, snapshot, actor); };
        if (!query.freestore_references) query.freestore_references = [reader = query.freestore_actor_facts]
            (const auto& snapshot, const ga::QualifiedFreestore& retained) {
            for (const auto& [actor, saved] : retained.inventory()) {
                auto actual = reader(snapshot, actor);
                if (!actual) return core::Result<void>::failure(actual.error());
                const auto& row = actual.value();
                if (saved.object.type != row.object.type || saved.current_type != row.current_type
                    || saved.object.owner != row.object.owner || saved.parent != row.parent
                    || saved.parent_is_planet != row.parent_is_planet || saved.current_type_member != row.current_type_member
                    || saved.unit_ai != row.unit_ai
                    || saved.hero != row.hero || saved.dead != row.dead || saved.deleting != row.deleting)
                    return absent<void>("saved freestore actor references match completed world facts");
            }
            return core::Result<void>::success();
        };
        out.prepare_freestore_selection = std::move(query.freestore_selection); out.execution_writers_tracked = true;
        out.build_budgets = std::move(query.build_budgets);
        out.execution_input = [setup, usable = query.usability](auto player, const auto& snapshot, const auto& world, const auto& engine, const auto* reconstruction) {
            return copy_stock_execution_input(*setup, player, snapshot, world, engine, usable, reconstruction);
        };
        out.execution_input_read_only = [setup, usable = query.usability](auto player, const auto& snapshot, const auto& world, const auto& engine) {
            return copy_stock_execution_input(*setup, player, snapshot, world, engine, usable, nullptr, true);
        };
        out.prepare_connectivity = std::move(query.connectivity);
        out.prepare_connectivity_publication = std::move(query.connectivity_publication);
        out.placement = [starting](const auto& request, const auto& snapshot, auto first_free) {
            return stock_native_force_builder(starting, {request.player, request.target, request.type, {}})(snapshot, first_free);
        };
        out.shadow_placement = [starting](const auto& request, const auto& view, auto first_free) {
            return stock_native_force_shadow_builder(starting, {request.player, request.target, request.type, {}})(view, first_free);
        };
        out.reconciliation_placement = [starting](const auto& request, const auto& view, auto first_free) {
            return stock_native_force_reconciliation_builder(starting, {request.player, request.target, request.type, {}})(view, first_free);
        };
        out.prepare_fresh_freestore = [reader = query.freestore_actor_facts]
            (auto& actions, const auto& snapshot, const auto& executor) -> core::Result<void> {
            using Change = sim::galactic::GalacticSession::NativeActorChange;
            std::vector<Change> births; births.reserve(snapshot.actor_records().size());
            for (const auto& actor : snapshot.actor_records())
                births.push_back({Change::Kind::created, actor.id, actor.generated_source, actor.dead, actor.delete_pending, {}});
            if (births.empty()) return absent<void>("fresh full typed world actor records");
            return apply_galactic_ai_actor_changes(actions.qualified_freestore(), births,
                [&snapshot, reader](auto actor) { return reader(snapshot, actor); }, executor);
        };
        out.initialize_freestore = [](auto player, auto& actions, const auto& snapshot, const auto&) {
            auto offers = ga::initial_freestore_offers(snapshot, actions.qualified_freestore());
            return offers ? actions.qualified_freestore().initialize(player, offers.value()) : core::Result<void>::failure(offers.error());
        };
        // WGA-438: Lua's planet predicate takes no player. The shared provider evaluates exactly that
        // predicate for any AI seat, so the first prepared AI seat answers it; none leaves it unbound.
        sim::galactic::PlayerId predicate_seat{};
        for (const auto& player : setup->perception.players) if (player.ai) { predicate_seat = player.player; break; }
        out.bind_actions = [usable = query.usability, movement = std::move(query.movement), references = std::move(query.freestore_references),
            actor_facts = std::move(query.freestore_actor_facts), content = setup->content, members = query.freestore_members,
            predicate_seat]
            (GameSession& game, auto& galaxy, auto& actions,
            auto engine, auto world, auto executor) {
            const auto owner = std::weak_ptr<GalacticAiRuntimeProviders::Engine>(engine);
            engine->set_galactic_stage_usability(usable);
            engine->set_galactic_stage_garrison([](auto, const auto& snapshot, auto actor) {
                return snapshot.actor_garrison(actor);
            });
            engine->set_galactic_stage_combat_skip([](auto, const auto& snapshot, auto actor) {
                return snapshot.actor_combat_skip(actor);
            });
            // WGA-485: original type (company transport when authored), faction,
            // logical tick rate and the active movement mode determine the estimate.
            engine->set_galactic_stage_speed([content](auto player, const auto& snapshot, auto type) {
                using namespace script::foc::ai;
                const auto object = content->types.find(type);
                const auto faction = content->faction_speed.find(player);
                if (object == content->types.end() || faction == content->faction_speed.end() || !snapshot.movement())
                    return absent<Real>("synchronized staging type/faction/mode speed");
                const auto speed = to_single(to_single(to_single(object->second.hyperspace_speed * faction->second)
                    * real(sim::tactical::logical_frames_per_second))
                    * script::numeric::from_fixed(snapshot.movement()->mode_modifier));
                return core::Result<Real>::success(speed);
            });
            // WGA-400..404: actual live identity, qualified containing source
            // and literal grandparent; absent source reads no permission.
            actions.bind_freestore_movement([usable](auto player, const auto& facts, const auto& snapshot) {
                return read_stock_galactic_ai_freestore_move(usable, player, facts, snapshot);
            });
            if (predicate_seat && usable)
                actions.bind_planet_usability([usable, predicate_seat](const auto& snapshot, auto planet) {
                    return usable(predicate_seat, snapshot, planet);
                });
            actions.bind_freestore_births([content, members](auto& retained, const auto& snapshot, const auto& shadow,
                auto changes, const auto& workers) {
                return apply_galactic_ai_actor_changes(retained, changes,
                    [&snapshot, &shadow, content, members](auto actor) { return stock_actor_facts(*content, members, snapshot, actor, &shadow); }, workers);
            });
            bind_galactic_ai_actor_changes(actions, actor_facts, executor);
            actions.bind_freestore_validation([parent = &galaxy, owner, references](const ga::QualifiedFreestore& decoded) {
                auto engine = owner.lock();
                if (!engine || !references) return absent<void>("qualified freestore actor/current/original/parent/lifecycle references");
                const auto snapshot = parent->snapshot();
                if (auto checked = references(*snapshot, decoded); !checked) return checked;
                for (const auto& player : snapshot->players()) if (const auto* held = decoded.reservations(player.player_id))
                    for (const auto& reservation : *held)
                        if (!engine->galactic_reservation_valid(player.player_id, reservation.first, reservation.second))
                            return absent<void>("actual Engine freestore potential-plan reservation");
                return core::Result<void>::success();
            });
            auto gates = game.native_conflict_readers();
            actions.bind_admission_gate(std::move(gates.admission));
            ga::bind_movement_references(actions, engine);
            const auto weak_engine = std::weak_ptr<GalacticAiRuntimeProviders::Engine>(engine);
            const auto weak_world = std::weak_ptr<GalacticAiRuntimeProviders::World>(world);
            actions.bind_movement_frontier([weak_world, executor, parent = &galaxy](auto player, auto snapshot) {
                const auto world = weak_world.lock(); const auto* workers = executor();
                if (!world || !workers) return absent<void>("native movement frontier owner/worker adapter");
                return world->bind_native_frontier(player, std::move(snapshot), *workers, parent->next_entity_id());
            });
            actions.bind_movement([movement, weak_world, weak_engine, executor](auto player, const auto& snapshot) {
                const auto* workers = executor();
                const auto world = weak_world.lock(); const auto engine = weak_engine.lock();
                if (!workers || !world || !engine) return absent<ga::NativeActions::MovementInput>("admitted movement owner/worker adapter");
                return movement(player, snapshot, *world, *engine, *workers);
            }, std::move(gates.global), [parent = &galaxy](auto player, auto actor) { return parent->recheck_native_orbit(player, actor); });
            return core::Result<void>::success();
        };
        out.prepare_child_scripts = [](auto engine, auto world, auto bind_timeline) {
            GalacticParentQueryFactory parent_queries = [engine, world, bind_timeline](const ConflictRecord& conflict) {
                const ModeId mode{SessionMode::battle, conflict.id};
                auto timeline = bind_timeline(mode);
                return GalacticParentQuery([engine, world, planet = conflict.planet, mode, timeline = std::move(timeline)]
                    (std::string_view equation, sim::tactical::PlayerId player) -> std::optional<script::numeric::LuaNumber> {
                    if (!timeline || !*timeline) return {};
                    const auto value = (*timeline)();
                    if (!value || value.value().mode != mode) return {};
                    return script::foc::ai::evaluate_in_galactic_context(
                        {engine.get(), world.get(), planet, true, {}, value.value().value}, equation, player);
                });
            };
            return core::Result<GalacticAiRuntimeProviders::ChildScripts>::success(
                {std::move(parent_queries)});
        };
        out.prepare_battle = [observe_battle](auto actions, auto engine, auto scheduler) {
            auto native = ga::retained_movement_battle_listener(std::move(actions), std::move(engine), std::move(scheduler));
            return GalacticAiRuntimeProviders::BattleListener([native = std::move(native), observe_battle](const auto& notice) {
                auto delivered = native(notice); if (!delivered) return delivered;
                if (observe_battle) observe_battle(notice);
                return core::Result<void>::success();
            });
        };
        for (const auto& [id, type] : setup->content->types) {
            const auto* original = type.original.get();
            if (!original) return absent<GalacticAiRuntimeProviders>("original freestore trait identity");
            const auto behaves = [&](std::string_view name) {
                for (const auto* list : {&original->behavior, &original->space_behavior, &original->land_behavior})
                    if (std::find(list->begin(), list->end(), name) != list->end()) return true;
                return false;
            };
            ga::FreestoreType trait; trait.type = id; trait.transport = behaves("TRANSPORT");
            if (behaves("FLEET")) trait.kind = ga::FreestoreKind::fleet;
            else if (behaves("DUMMY_GROUND_STRUCTURE") || behaves("DUMMY_GROUND_BASE")) trait.kind = ga::FreestoreKind::ground_structure;
            else if (behaves("DUMMY_ORBITAL_STRUCTURE") || behaves("DUMMY_STAR_BASE")) trait.kind = ga::FreestoreKind::space_structure;
            out.freestore_types.push_back(trait);
        }
        struct SafetyPublication {
            std::shared_ptr<const ga::FreestorePool> source, value;
            std::uint64_t generation{};
        };
        out.publish_freestore = [types = out.freestore_types, cache = std::make_shared<SafetyPublication>()]
            (const auto& actions, auto& input, const auto& snapshot, const auto& executor) {
            auto source = actions.qualified_freestore().publish(executor);
            if (!source) return core::Result<void>::failure(source.error());
            // World writers invalidate this generation for actor/control changes;
            // clock-only frames reuse the owning immutable safety publication.
            const auto generation = snapshot.observer_input_generation();
            if (cache->source != source.value() || cache->generation != generation || !cache->value) {
                auto derived = std::make_shared<ga::FreestorePool>(*source.value());
                const bool same_world = cache->source && cache->value && cache->generation == generation;
                if (same_world && cache->source->objects == source.value()->objects) {
                    // Reservation/callback-only writes reuse the safety projection.
                    derived->objects = cache->value->objects; derived->surfaces = cache->value->surfaces;
                    derived->work = cache->value->work;
                } else if (auto safety = ga::prepare_freestore_safety(*derived, snapshot, types, executor); !safety) return safety;
                for (const auto& [player, rows] : source.value()->players) {
                    std::shared_ptr<const ga::FreestorePool> value;
                    if (same_world) {
                        const auto old_source = cache->source->players.find(player), old_value = cache->value->players.find(player);
                        if (old_source != cache->source->players.end() && old_value != cache->value->players.end()
                            && old_source->second == rows) value = old_value->second;
                    }
                    if (!value) {
                        auto next = std::make_shared<ga::FreestorePool>(*rows);
                        // Surface safety is built once over the frozen world, then split into player views.
                        for (auto& actor : next->objects) {
                            const auto row = std::lower_bound(derived->objects.begin(), derived->objects.end(), actor.id,
                                [](const auto& a, auto id) { return a.id < id; });
                            actor.safe = row->safe;
                        }
                        next->surfaces = derived->surfaces; next->work.safety_rows = derived->work.safety_rows;
                        value = std::move(next);
                    }
                    derived->players[player] = std::move(value);
                }
                cache->source = source.value(); cache->generation = generation; cache->value = std::move(derived);
            }
            input.pool = cache->value; return core::Result<void>::success();
        };
        return core::Result<GalacticAiRuntimeProviders>::success(std::move(out));
    };
    return core::Result<Factory>::success(std::move(factory));
}
} // namespace eawr::campaign
