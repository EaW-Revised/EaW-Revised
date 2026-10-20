#include "eawr/core/gc_profile.hpp"
#include "galactic_ai_execution.hpp"
#include "galactic_ai_freestore.hpp"
#include "galactic_ai_estimates.hpp"
#include "host.hpp"
#include "mode_time.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace eawr::script::foc::galactic_ai {
namespace auth = authoritative;
namespace {
core::Diagnostic error(std::string_view code, std::string message) {
    core::Diagnostic out; out.code = std::string(code); out.message = std::move(message); return out;
}
const FreestoreObject* object_of(const FreestorePool& pool, const auth::Value& value) {
    const auto* handle = detail::as_handle(value, handle_game_object);
    if (!handle) return nullptr;
    const auto it = std::lower_bound(pool.objects.begin(), pool.objects.end(), handle->id,
        [](const auto& row, auto id) { return row.id < id; });
    return it != pool.objects.end() && it->id == handle->id ? &*it : nullptr;
}
// WGA-436: a handle to a lost, dead or deleting actor is a null wrapper.
bool is_removed(const FreestorePool& pool, const auth::Value& value) {
    const auto* handle = detail::as_handle(value, handle_game_object);
    return handle && std::binary_search(pool.removed.begin(), pool.removed.end(), handle->id);
}
bool allied(const gc::PoliticalControlSetup& control, gc::PlayerId a, gc::PlayerId b) {
    if (a == b) return true;
    const auto it = std::find_if(control.relations.begin(), control.relations.end(), [&](const auto& row) {
        return row.player == a && row.other == b;
    });
    return it != control.relations.end() && it->allied;
}
core::Result<void> surface_safety(FreestorePool& out, const gc::GalacticSnapshot& snapshot,
    const sim::PartitionExecutor& executor) {
    out.work.safety_rows = 0;
    const auto* control = snapshot.control();
    out.surfaces.clear();
    struct SafetyRow { bool accessible{}; std::uint32_t landed{}; std::vector<gc::PlayerId> owners; };
    std::vector<SafetyRow> surfaces(control ? control->planets.size() : 0);
    if (control) {
        const auto surface_stage = executor.execute_phase("galactic-ai-freestore-surfaces", sim::tick_partition_count, [&](const std::size_t part) {
            const auto range = sim::partition_range(part, surfaces.size());
            for (auto i = range.begin; i < range.end; ++i) {
                auto& row = surfaces[i]; const auto& planet = control->planets[i]; row.accessible = planet.surface_accessible;
                for (const auto& unit : planet.landed) {
                    if (!unit.id) continue;
                    // WGA-410: deletion filters capacity only, never the safety ownership scan.
                    row.owners.push_back(unit.owner);
                    const auto actors = snapshot.actor_records();
                    const auto actor = std::lower_bound(actors.begin(), actors.end(), unit.id,
                        [](const auto& value, auto id) { return value.id < id; });
                    if (actor == actors.end() || actor->id != unit.id || !actor->delete_pending) ++row.landed;
                }
            }
        });
        if (!surface_stage) return core::Result<void>::failure(surface_stage.error());
    }
    std::map<gc::PlanetId, std::pair<gc::PlayerId, const SafetyRow*>> safety_index;
    if (control) for (std::size_t i = 0; i < control->planets.size(); ++i) {
        const auto id = control->planets[i].planet;
        const auto planet = std::lower_bound(snapshot.planets().begin(), snapshot.planets().end(), id,
            [](const auto& p, auto value) { return p.id < value; });
        if (planet != snapshot.planets().end() && planet->id == id) safety_index.emplace(id, std::pair{planet->owner, &surfaces[i]});
        out.surfaces.push_back({id, surfaces[i].landed, control->max_ground_forces, surfaces[i].accessible});
        out.work.safety_rows += control->planets[i].landed.size();
    }
    const auto staged = executor.execute_phase("galactic-ai-freestore", sim::tick_partition_count, [&](const std::size_t part) {
        const auto range = sim::partition_range(part, out.objects.size());
        for (auto i = range.begin; i < range.end; ++i) {
            auto& row = out.objects[i];
            if (row.transit || !row.transport || !row.planet || row.directly_parented_to_planet) { row.safe = true; continue; }
            row.safe.reset();
            if (!control) continue;
            const auto found = safety_index.find(row.planet); if (found == safety_index.end()) continue;
            const auto& surface = *found->second.second;
            const bool enemy = std::any_of(surface.owners.begin(), surface.owners.end(), [&](auto owner) { return !allied(*control, row.owner, owner); });
            row.safe = allied(*control, row.owner, found->second.first) && surface.accessible
                && surface.landed < control->max_ground_forces && !enemy;
        }
    });
    if (!staged) return core::Result<void>::failure(staged.error());
    out.work.objects = out.objects.size();
    return core::Result<void>::success();
}
}
core::Result<void> prepare_freestore_safety(FreestorePool& pool, const gc::GalacticSnapshot& snapshot,
    std::span<const FreestoreType> types, const sim::PartitionExecutor& executor) {
    std::map<gc::TypeId, FreestoreType> type_index;
    for (const auto& type : types) if (!type.type || !type_index.emplace(type.type, type).second)
        return core::Result<void>::failure(error(auth::codes::invalid_request, "freestore traits require unique nonzero type identities"));
    return surface_safety(pool, snapshot, executor);
}
core::Result<FreestorePool> copy_freestore(const gc::GalacticSnapshot& snapshot,
    const std::span<const FreestoreType> types, const sim::PartitionExecutor& executor) {
    FreestorePool out;
    std::map<gc::TypeId, FreestoreType> type_index;
    for (const auto& type : types) if (!type.type || !type_index.emplace(type.type, type).second)
        return core::Result<FreestorePool>::failure(error(auth::codes::invalid_request, "freestore traits require unique nonzero type identities"));
    std::map<sim::EntityId, std::size_t> identities;
    const auto add = [&](sim::EntityId id, gc::TypeId type, gc::PlayerId owner, gc::PlanetId planet,
        bool transit, bool member, bool direct, bool fleet) {
        if (const auto found = identities.find(id); found != identities.end()) {
            if (member) out.objects[found->second].kind = FreestoreKind::member;
            return; // roster/reference overlap is one persistent actor
        }
        identities.emplace(id, out.objects.size());
        const auto traits = type_index.find(type);
        FreestoreObject row; row.id = id; row.type = type; row.owner = owner; row.planet = planet;
        row.transit = transit; row.directly_parented_to_planet = direct;
        row.kind = fleet ? FreestoreKind::fleet : member ? FreestoreKind::member
            : traits != type_index.end() ? traits->second.kind : FreestoreKind::member; // unknown is excluded
        row.transport = traits != type_index.end() && traits->second.transport;
        out.objects.push_back(row);
    };
    // Presence copying is bounded barrier glue; per-object safety is partitioned below.
    for (const auto& planet : snapshot.planets()) add(planet.id, 0, planet.owner, planet.id, false, false, true, false);
    for (auto& row : out.objects) row.kind = FreestoreKind::planet;
    for (const auto& fleet : snapshot.fleets()) {
        const bool transit = fleet.motion.transit.has_value();
        const auto planet = transit ? gc::PlanetId{} : fleet.orbit;
        add(fleet.id, 0, fleet.owner, planet, transit, false, !transit && fleet.orbit != 0, true);
        for (const auto& member : fleet.members) add(member.id, member.type, fleet.owner, planet, transit, false, false, false);
    }
    if (snapshot.fleet_control()) for (const auto& unit : snapshot.fleet_control()->detached_units)
        add(unit.member.id, unit.member.type, unit.owner, unit.planet, false, false, true, false);
    if (const auto* control = snapshot.control()) for (const auto& planet : control->planets)
        for (const auto& unit : planet.landed) add(unit.id, unit.type, unit.owner, planet.planet, false, false, true, false);
    if (snapshot.production()) for (const auto& planet : snapshot.production()->planets)
        for (const auto& unit : planet.objects) add(unit.id, unit.type, unit.owner, planet.planet, false, false, true, false);
    if (const auto* movement = snapshot.movement()) {
        std::map<sim::EntityId, const gc::MovementContents*> contents_index;
        for (const auto& contents : movement->contents) contents_index.emplace(contents.container, &contents);
        for (std::size_t index = 0; index < out.objects.size(); ++index) {
            const auto parent = out.objects[index];
            const auto contents = contents_index.find(parent.id); if (contents == contents_index.end()) continue;
            for (const auto& child : contents->second->children)
                add(child.id, child.type, parent.owner, parent.planet, parent.transit, true, false, false);
        }
    }
    std::sort(out.objects.begin(), out.objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    if (auto safe = surface_safety(out, snapshot, executor); !safe)
        return core::Result<FreestorePool>::failure(safe.error());
    return core::Result<FreestorePool>::success(std::move(out));
}
core::Result<void> register_freestore_object_methods(auth::ScriptScheduler& scripts, std::shared_ptr<const FreestoreInput> input,
    bool unqualified_fixture) {
    if (!input) return core::Result<void>::failure(error(auth::codes::invalid_request, "freestore input absent"));
    // WGA-436..439: the GameObject callbacks the stock redistribution helpers call before any native
    // movement. Their inputs are the published pool (actor current representation, containing planet,
    // surface capacity) and the authored planet lists; a missing input is unavailable, never a guess.
    const auto actor_row = [input, unqualified_fixture](std::string_view name, const auth::ValueList& args, bool null_answer = false)
        -> core::Result<std::pair<std::shared_ptr<const FreestorePool>, const FreestoreObject*>> {
        using Out = core::Result<std::pair<std::shared_ptr<const FreestorePool>, const FreestoreObject*>>;
        const auto pool = input->pool;
        if (!pool) return Out::failure(error(auth::codes::missing_api, std::string(name) + ": freestore pool unavailable"));
        if (!pool->qualified && !unqualified_fixture) return Out::failure(error(auth::codes::missing_api,
            std::string(name) + ": campaign freestore requires qualified current representation (#2735)"));
        if (args.empty() || !detail::as_handle(args[0], handle_game_object))
            return Out::failure(error(auth::codes::invalid_request, std::string(name) + " requires a game object"));
        const auto* row = object_of(*pool, args[0]);
        if (!row && null_answer && is_removed(*pool, args[0])) return Out::success({pool, nullptr});
        if (!row) return Out::failure(error(auth::codes::missing_api,
            std::string(name) + ": actor facts are not published for this object"));
        return Out::success({pool, row});
    };
    auto result = scripts.register_method(handle_game_object, "Is_Transport", [actor_row](auth::BindingContext&, const auth::ValueList& args) {
        // WGA-436: current-representation transport behavior; never original type or cargo.
        // A removed object is a null wrapper: false, no script error.
        const auto found = actor_row("Is_Transport", args, true);
        if (!found) return core::Result<auth::ValueList>::failure(found.error());
        return detail::one(detail::boolean(found.value().second && found.value().second->transport));
    });
    if (!result) return result;
    result = scripts.register_method(handle_game_object, "Get_Planet_Location", [actor_row](auth::BindingContext&, const auth::ValueList& args) {
        // WGA-437: the first planet at or above the object; nothing when it has no containing planet.
        const auto found = actor_row("Get_Planet_Location", args);
        if (!found) return core::Result<auth::ValueList>::failure(found.error());
        const auto planet = found.value().second->planet;
        return planet ? detail::one(detail::handle(handle_game_object, planet)) : detail::none();
    });
    if (!result) return result;
    result = scripts.register_method(handle_game_object, "Can_Land_On_Planet", [input, actor_row](auth::BindingContext&, const auth::ValueList& args)
        -> core::Result<auth::ValueList> {
        // WGA-165/439: transport, accessible surface, no authored restriction on the original type,
        // and landed transports strictly below the configured maximum.
        if (args.size() != 2) return detail::fail("Can_Land_On_Planet -- invalid number of parameters. Expected 1, got "
            + std::to_string(args.empty() ? 0 : args.size() - 1) + ".");
        const auto* planet = detail::as_handle(args[1], handle_game_object);
        if (!planet) return detail::fail("Can_Land_On_Planet -- invalid type for parameter 1. Expected game object.");
        const auto found = actor_row("Can_Land_On_Planet", args);
        if (!found) return core::Result<auth::ValueList>::failure(found.error());
        // WGA-439: an absent (removed) parameter object is not a planet: false.
        if (is_removed(*found.value().first, args[1])) return detail::one(detail::boolean(false));
        const auto& pool = *found.value().first; const auto& self = *found.value().second;
        if (const auto* other = object_of(pool, args[1]); other && other->kind != FreestoreKind::planet)
            return detail::one(detail::boolean(false));
        if (!self.transport) return detail::one(detail::boolean(false));
        const auto surface = std::lower_bound(pool.surfaces.begin(), pool.surfaces.end(), planet->id,
            [](const auto& row, auto id) { return row.planet < id; });
        if (surface == pool.surfaces.end() || surface->planet != planet->id)
            return core::Result<auth::ValueList>::failure(error(auth::codes::missing_api,
                "Can_Land_On_Planet: planet surface capacity input unavailable"));
        if (!surface->accessible) return detail::one(detail::boolean(false));
        const auto lists = input->planet_restrictions.find(planet->id);
        const auto category = input->type_categories.find(self.type);
        if (lists == input->planet_restrictions.end() || category == input->type_categories.end())
            return core::Result<auth::ValueList>::failure(error(auth::codes::missing_api,
                "Can_Land_On_Planet: authored planet restriction input unavailable"));
        const bool restricted = (category->second & lists->second.categories) != 0
            || std::find(lists->second.types.begin(), lists->second.types.end(), self.type) != lists->second.types.end();
        return detail::one(detail::boolean(!restricted && surface->landed < surface->maximum));
    });
    return result;
}
core::Result<void> register_planet_usability_method(auth::ScriptScheduler& scripts) {
    return scripts.register_method(handle_game_object, "Get_Is_Planet_AI_Usable", [](auth::BindingContext& context, const auth::ValueList& args)
        -> core::Result<auth::ValueList> {
        // WGA-438: planetary predicate of the receiving planet; the native owner validates the receiver.
        if (args.empty() || !detail::as_handle(args[0], handle_game_object))
            return detail::fail("Get_Is_Planet_AI_Usable requires a game object");
        const auto answered = context.query_immediate("galactic.planet_ai_usable", {args[0]});
        if (!answered) return core::Result<auth::ValueList>::failure(error(answered.error().code,
            "Get_Is_Planet_AI_Usable unavailable: " + answered.error().message));
        if (answered.value().size() != 1 || !std::get_if<bool>(&answered.value().front().data))
            return detail::fail("Get_Is_Planet_AI_Usable native predicate returned an invalid answer");
        return detail::one(answered.value().front());
    });
}
core::Result<void> register_freestore_bindings(auth::ScriptScheduler& scripts, std::shared_ptr<const FreestoreInput> input, bool unqualified_fixture) {
    if (!input) return core::Result<void>::failure(error(auth::codes::invalid_request, "freestore input absent"));
    const auto bind = [&](std::string_view name, auth::Binding function) { return scripts.register_method(handle_galactic_freestore, name, std::move(function)); };
    auto result = scripts.register_binding("Find_First_Object", [input](auth::BindingContext&, const auth::ValueList& args) {
        if (args.size() != 1 || !detail::as_text(args[0])) return detail::fail("Find_First_Object expects an original type name");
        if (!input->pool) return detail::fail("freestore presence unavailable");
        const auto found = input->types_by_name.find(detail::upper(*detail::as_text(args[0])));
        if (found == input->types_by_name.end()) return detail::none();
        for (const auto& row : input->pool->objects) if (row.type == found->second)
            return detail::one(detail::handle(handle_game_object, row.id));
        return detail::none();
    });
    if (!result) return result;
    result = bind("Get_Object_Count", [input, unqualified_fixture](auth::BindingContext& context, const auth::ValueList& args) {
        const auto pool = input->pool;
        if (!pool) return detail::fail("freestore pool unavailable");
        if (args.empty() || args.size() > 2) return detail::fail("Get_Object_Count expects an optional domain Boolean");
        const auto* store = detail::as_handle(args[0], handle_galactic_freestore);
        const auto* nontransport = args.size() == 2 ? std::get_if<bool>(&args[1].data) : nullptr;
        if (!store || (args.size() == 2 && !nontransport)) return detail::fail("Get_Object_Count requires a freestore and Boolean");
        const auto immediate = context.query_immediate("galactic.freestore_count", args);
        if (immediate) return immediate;
        if (!pool->qualified && !unqualified_fixture) return core::Result<auth::ValueList>::failure(error(auth::codes::missing_api,
            "campaign freestore requires qualified current-type membership (#2725/#2735)"));
        if (pool->qualified && !pool->qualified_players.contains(static_cast<gc::PlayerId>(store->id)))
            return core::Result<auth::ValueList>::failure(error(auth::codes::missing_api, "freestore owning manager is uninitialized"));
        std::int64_t count = 0;
        for (const auto& row : pool->objects) if ((pool->qualified ? row.owner == store->id : freestore_eligible(row, static_cast<gc::PlayerId>(store->id)))
            && (!nontransport || *nontransport != row.transport)) ++count;
        return detail::one(detail::number(Number(count)));
    });
    if (!result) return result;
    result = bind("Is_Unit_In_Transit", [input, unqualified_fixture](auth::BindingContext&, const auth::ValueList& args) {
        const auto pool = input->pool;
        if (!pool) return detail::fail("freestore pool unavailable");
        if (!pool->qualified && !unqualified_fixture) return core::Result<auth::ValueList>::failure(error(auth::codes::missing_api,
            "campaign freestore requires qualified current representation (#2735)"));
        if (args.size() != 2) return detail::fail("Is_Unit_In_Transit expects one object");
        const auto* row = object_of(*pool, args[1]); return detail::one(detail::boolean(row && row->transit));
    });
    if (!result) return result;
    result = bind("Is_Unit_Safe", [input, unqualified_fixture](auth::BindingContext&, const auth::ValueList& args) -> core::Result<auth::ValueList> {
        const auto pool = input->pool;
        if (!pool) return detail::fail("freestore pool unavailable");
        if (!pool->qualified && !unqualified_fixture) return core::Result<auth::ValueList>::failure(error(auth::codes::missing_api,
            "campaign freestore requires qualified current representation (#2735)"));
        if (args.size() != 2) return detail::fail("Is_Unit_Safe expects one object");
        const auto* row = object_of(*pool, args[1]);
        if (!row || !row->safe) return core::Result<auth::ValueList>::failure(error(auth::codes::missing_api, "freestore transport safety input unavailable"));
        return detail::one(detail::boolean(*row->safe));
    });
    if (!result) return result;
    if (auto registered = register_freestore_object_methods(scripts, input, unqualified_fixture); !registered) return registered;
    if (auto registered = register_planet_usability_method(scripts); !registered) return registered;
    return bind("Move_Object", [](auth::BindingContext& context, const auth::ValueList& args) {
        // WGA-309/313: preflight reads the completed native owner/private pool.
        // Stock callbacks have no post-call moved-state reader; native writes
        // are committed in command order after partitioned script service.
        const auto decision = context.query_immediate("galactic.freestore_move", args);
        if (!decision && decision.error().code == auth::codes::missing_api && decision.error().message == freestore_move_reader_seam) {
            // Project seam policy: unknown reader is silent false with a saved
            // counter, avoiding repeated campaign diagnostics. WGA-309 still
            // returns destination-found and consumes its authored quota.
            const auto counted = context.apply_immediate("galactic.freestore_move_unbound", {});
            if (!counted) return core::Result<auth::ValueList>::failure(counted.error());
            return detail::one(detail::boolean(false));
        }
        if (!decision) return core::Result<auth::ValueList>::failure(error(decision.error().code,
            "freestore movement unavailable: " + decision.error().message + "; " + std::string(freestore_move_reader_seam)));
        if (decision.value().size() != 1 || !std::get_if<Number>(&decision.value().front().data))
            return detail::fail("freestore movement preflight returned an invalid decision");
        const auto state = std::get<Number>(decision.value().front().data);
        if (state == Number(2)) context.issue_command(std::string(verb_freestore_move), args);
        return detail::one(detail::boolean(state != Number(0)));
    });
}
namespace {
constexpr std::uint64_t first_service_sequence = 6;
core::Result<void> events(auth::ScriptScheduler& scripts, FreestoreAttachment attachment,
    std::vector<auth::ScriptEvent> rows, std::uint64_t sequence) {
    for (auto& row : rows) {
        row.key = {scripts.completed_tick() + 1, auth::first_simulation_producer, attachment.instance, sequence++};
        row.target = attachment.instance;
        if (auto result = scripts.submit_event(std::move(row)); !result) return result;
    }
    return core::Result<void>::success();
}
auth::ScriptEvent assign(std::string name, auth::Value value) {
    auth::ScriptEvent row; row.kind = auth::ScriptEvent::Kind::assign; row.name = std::move(name); row.parameter = std::move(value); return row;
}
}
core::Result<void> attach_freestore(auth::ScriptScheduler& scripts, FreestoreAttachment attachment, bool fresh) {
    std::vector<auth::ScriptEvent> rows;
    if (fresh) {
        auth::ScriptEvent base; base.kind = auth::ScriptEvent::Kind::call; base.name = "Base_Definitions"; rows.push_back(base);
        auth::ScriptEvent main; main.kind = auth::ScriptEvent::Kind::start_thread; main.name = "main"; rows.push_back(main);
    }
    rows.push_back(assign("FreeStore", detail::handle(handle_galactic_freestore, attachment.player)));
    rows.push_back(assign("PlayerObject", detail::handle(handle_player, attachment.player)));
    rows.push_back(assign("LastService", detail::number(Number(0))));
    rows.push_back(assign("LastUnitService", detail::number(Number(0))));
    // Four assignments, plus two fresh initialization events, occupy 0..5.
    assert(rows.size() <= first_service_sequence);
    return events(scripts, attachment, std::move(rows), 0);
}
core::Result<void> service_freestore(auth::ScriptScheduler& scripts, FreestoreAttachment attachment,
    const FreestorePool& pool, std::uint64_t mode_frame, QualifiedFreestore* owner, bool unqualified_fixture) {
    if ((!pool.qualified && !unqualified_fixture) || (pool.qualified && !pool.qualified_players.contains(attachment.player)))
        return core::Result<void>::failure(error(auth::codes::missing_api, "campaign freestore service requires initialized qualified manager (#2725/#2735)"));
    const auto now = tactical_ai_detail::mode_seconds(mode_frame);
    const auto due = [&](std::string_view rate_name, std::string_view last_name) -> core::Result<bool> {
        auto rate = scripts.read_global(attachment.instance, rate_name); auto last = scripts.read_global(attachment.instance, last_name);
        if (!rate) { return core::Result<bool>::failure(rate.error()); }
        if (!last) { return core::Result<bool>::failure(last.error()); }
        const auto* period = rate.value() ? std::get_if<Number>(&rate.value()->data) : nullptr;
        const auto* stamp = last.value() ? std::get_if<Number>(&last.value()->data) : nullptr;
        return core::Result<bool>::success(period && (!stamp || now - *stamp > *period));
    };
    auto main = due("ServiceRate", "LastService"); auto unit = due("UnitServiceRate", "LastUnitService");
    if (!main) { return core::Result<void>::failure(main.error()); }
    if (!unit) { return core::Result<void>::failure(unit.error()); }
    std::vector<auth::ScriptEvent> rows;
    const auto additions = owner ? owner->additions(attachment.player) : std::span<const sim::EntityId>{};
    const auto added_count = additions.size();
    for (const auto id : additions) {
        auth::ScriptEvent call; call.kind = auth::ScriptEvent::Kind::call; call.name = "On_Unit_Added";
        call.arguments.push_back(detail::handle(handle_game_object, id)); rows.push_back(std::move(call));
    }
    if (main.value()) {
        auth::ScriptEvent pump; pump.kind = auth::ScriptEvent::Kind::pump; rows.push_back(pump);
        rows.push_back(assign("LastService", detail::number(now)));
    }
    if (unit.value()) {
        for (const auto& row : pool.objects) if ((pool.qualified ? row.owner == attachment.player : freestore_eligible(row, attachment.player)) && !pool.reservations.contains(row.id)) {
            auth::ScriptEvent call; call.kind = auth::ScriptEvent::Kind::call; call.name = "On_Unit_Service";
            call.arguments.push_back(detail::handle(handle_game_object, row.id)); rows.push_back(std::move(call));
        }
        rows.push_back(assign("LastUnitService", detail::number(now)));
    }
    // Attachment and the first admitted execution service can share a tick.
    // Reserve the six fresh attachment keys before service callbacks without
    // changing this producer's ordering relative to story or other instances.
    // The lifecycle admits one service batch per freestore instance per tick.
    const auto submitted = events(scripts, attachment, std::move(rows), first_service_sequence);
    if (!submitted) return submitted;
    return owner ? owner->consume_additions(attachment.player, added_count) : core::Result<void>::success();
}
} // namespace eawr::script::foc::galactic_ai

namespace eawr::script::foc::galactic_ai {
namespace {
template<class T> core::Result<T> unavailable_store(std::string message) {
    return core::Result<T>::failure(error(authoritative::codes::missing_api, std::move(message)));
}
bool valid_facts(const FreestoreFacts& row) {
    return row.object.id && row.object.owner && row.qualified
        && static_cast<unsigned>(row.object.kind) <= static_cast<unsigned>(FreestoreKind::space_structure)
        && (row.object.kind == FreestoreKind::planet || (row.object.type && row.current_type))
        && (!row.parent_is_planet || (row.parent && row.parent == row.object.planet))
        && row.object.directly_parented_to_planet == row.parent_is_planet;
}
void store_word(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
}
bool freestore_admission(const FreestoreFacts& row, gc::PlayerId player) noexcept {
    if (!row.qualified || !row.object.id || !player || row.object.owner != player
        || !row.ai_usable || row.dead || row.deleting || row.death_clone || row.current_type_member) return false;
    if (row.object.kind == FreestoreKind::planet || row.object.kind == FreestoreKind::ground_structure
        || row.object.kind == FreestoreKind::space_structure) return false;
    return row.object.kind != FreestoreKind::fleet || (row.parent && !row.parent_is_planet);
}
FreestoreFacts ordinary_freestore_birth(FreestoreFacts row) {
    row.ai_usable = row.qualified = true; row.dead = row.deleting = row.death_clone = false;
    row.prefer_ground = true;
    return row;
}
const FreestoreFacts* QualifiedFreestore::facts(sim::EntityId id) const noexcept {
    const auto found = facts_.find(id); return found == facts_.end() ? nullptr : &found->second;
}
core::Result<void> QualifiedFreestore::update(FreestoreFacts row) {
    if (!valid_facts(row)) return unavailable_store<void>("freestore authoritative actor facts are incomplete");
    row.object.transit = !row.object.planet || row.enroute || row.tracked_movement;
    const auto old = facts_.find(row.object.id);
    if (old != facts_.end() && old->second.object.owner != row.object.owner)
        return unavailable_store<void>("freestore ownership must use the owner-change operation");
    if (old == facts_.end() || old->second != row) { facts_[row.object.id] = row; update_fact_record(row.object.id); changed(row.object.owner); }
    return core::Result<void>::success();
}
core::Result<void> QualifiedFreestore::initialize(gc::PlayerId player, std::span<const sim::EntityId> offers) {
    if (!player || stores_.contains(player)) return unavailable_store<void>("freestore initialization requires a fresh player owner");
    // Validate the full ordered offer frontier before any native publication.
    for (const auto id : offers) if (!facts(id)) return unavailable_store<void>("initial freestore offer has no authoritative actor facts");
    stores_.emplace(player, std::make_shared<Store>()); update_store_record(player); changed(player);
    for (const auto id : offers) { auto added = offer(player, id); if (!added) return core::Result<void>::failure(added.error()); }
    return core::Result<void>::success();
}
core::Result<bool> QualifiedFreestore::offer(gc::PlayerId player, sim::EntityId id) {
    const auto found = stores_.find(player); const auto* row = facts(id);
    if (found == stores_.end() || !row) return unavailable_store<bool>("freestore offer requires its initialized manager and actor facts");
    if (!freestore_admission(*row, player) || found->second->members.contains(id)) return core::Result<bool>::success(false);
    auto& store = write_store(player); store.members.insert(id); store.additions.push_back(id); update_store_record(player); changed(player);
    return core::Result<bool>::success(true);
}
void QualifiedFreestore::remove(gc::PlayerId player, sim::EntityId id) {
    const auto found = stores_.find(player); if (found == stores_.end()) return;
    auto& store = write_store(player);
    const bool removed = store.members.erase(id) != 0;
    const bool released = store.reservations.erase(id) != 0;
    const auto old_size = store.additions.size(); std::erase(store.additions, id);
    if (removed || released || old_size != store.additions.size()) { update_store_record(player); changed(player); }
}
core::Result<void> QualifiedFreestore::owner_changed(FreestoreFacts row, bool initialized) {
    if (!valid_facts(row) || !facts(row.object.id)) return unavailable_store<void>("freestore owner change requires qualified retained actor facts");
    if (initialized && !stores_.contains(row.object.owner)) return unavailable_store<void>("freestore new owner manager is absent");
    remove(facts(row.object.id)->object.owner, row.object.id);
    row.object.transit = !row.object.planet || row.enroute || row.tracked_movement;
    records_.erase(facts_.at(row.object.id).object.owner, 1, row.object.id);
    facts_[row.object.id] = row; update_fact_record(row.object.id); changed(row.object.owner);
    if (initialized) { auto added = offer(row.object.owner, row.object.id); if (!added) return core::Result<void>::failure(added.error()); }
    return core::Result<void>::success();
}
void QualifiedFreestore::lost(sim::EntityId id) {
    const auto* row = facts(id); if (!row) return;
    const auto player = row->object.owner; remove(player, id); records_.erase(player, 1, id); facts_.erase(id); lost_.insert(id);
    core::RecordDigest::Bytes key; core::RecordDigest::append(key, id); records_.set(0, 3, id, key); changed(player);
}
core::Result<void> QualifiedFreestore::prevent_ai_usage(gc::PlayerId player, sim::EntityId id, bool prevent) {
    const auto* row = facts(id);
    if (!row || row->object.owner != player || !stores_.contains(player))
        return unavailable_store<void>("Prevent_AI_Usage requires the initialized active-mode owning pool");
    if (prevent) remove(player, id); // WGA-311: no false flag write.
    else {
        auto updated = *row; updated.ai_usable = true;
        if (auto result = update(updated); !result) return result;
        auto added = offer(player, id); if (!added) return core::Result<void>::failure(added.error());
    }
    return core::Result<void>::success();
}
core::Result<void> QualifiedFreestore::set_prefer_ground(sim::EntityId id, bool value) {
    const auto* row = facts(id);
    if (!row || !row->unit_ai || row->dead || row->deleting)
        return unavailable_store<void>("Set_Prefer_Ground_Over_Space requires a live actor with unit-AI behavior");
    auto updated = *row; updated.prefer_ground = value;
    return update(std::move(updated));
}
LandingGate automatic_landing_gate(bool human, const FreestoreObject& actor,
    const FreestoreFacts& facts, const FreestoreFacts* parent, const FreestoreSurface* surface) noexcept {
    if (human) return LandingGate::human;
    if (actor.safe != true) return LandingGate::safety;
    if (actor.transit) return LandingGate::transit;
    if (!actor.transport || !facts.parent) return LandingGate::transport_parent;
    if (facts.parent_is_planet) return LandingGate::parent_planet;
    if (facts.unit_ai && !facts.prefer_ground) return LandingGate::preference;
    if (!parent || !parent->parent_is_planet || !parent->parent || !surface || surface->planet != parent->parent)
        return LandingGate::grandparent_planet;
    if (surface->landed >= surface->maximum) return LandingGate::capacity;
    if (!surface->accessible) return LandingGate::surface;
    return LandingGate::admitted;
}
bool QualifiedFreestore::contains(gc::PlayerId player, sim::EntityId id) const noexcept {
    const auto found = stores_.find(player); return found != stores_.end() && found->second->members.contains(id);
}
std::uint64_t QualifiedFreestore::object_count(gc::PlayerId player, std::optional<bool> nontransport) const {
    const auto found = stores_.find(player); if (found == stores_.end()) return 0;
    std::uint64_t count{};
    for (const auto id : found->second->members) { const auto* row = facts(id);
        if (row && (!nontransport || *nontransport != row->object.transport)) ++count;
    }
    return count;
}
QualifiedFreestore QualifiedFreestore::clone() const {
    QualifiedFreestore out; out.facts_ = facts_; out.stores_ = stores_; out.lost_ = lost_;
    out.published_ = published_; out.records_ = records_; return out;
}
void QualifiedFreestore::replace(const QualifiedFreestore& other) {
    const bool same_stores = stores_.size() == other.stores_.size()
        && std::equal(stores_.begin(), stores_.end(), other.stores_.begin(), [](const auto& a, const auto& b) {
            return a.first == b.first && (a.second == b.second || *a.second == *b.second);
        });
    if (facts_ == other.facts_ && same_stores && lost_ == other.lost_) return;
    facts_ = other.facts_; stores_ = other.stores_; lost_ = other.lost_;
    published_ = other.published_; records_ = other.records_;
    if (changed_) changed_();
}
core::Result<bool> QualifiedFreestore::reserve(gc::PlayerId player, sim::EntityId id, std::uint64_t plan) {
    if (!plan || !contains(player, id)) return unavailable_store<bool>("freestore reservation requires retained membership and a plan identity");
    auto& held = write_store(player).reservations;
    if (held.contains(id)) return core::Result<bool>::success(false);
    held.emplace(id, plan); update_store_record(player); changed(player); return core::Result<bool>::success(true);
}
void QualifiedFreestore::release(gc::PlayerId player, std::uint64_t plan) {
    const auto found = stores_.find(player); if (found == stores_.end() || !plan) return;
    auto& held = write_store(player).reservations;
    const auto old = held.size(); std::erase_if(held, [&](const auto& row) { return row.second == plan; });
    if (old != held.size()) { update_store_record(player); changed(player); }
}
void QualifiedFreestore::release_object(gc::PlayerId player, sim::EntityId id, std::uint64_t plan) {
    const auto store = stores_.find(player); if (store == stores_.end() || !plan) return;
    // WGA-163: assignment removes membership; task return attempts ordinary
    // admission when the actor is no longer present to hold a reservation.
    if (!store->second->members.contains(id)) {
        if (facts(id)) {
            const auto offered = offer(player, id);
            if (!offered) throw std::runtime_error(offered.error().message);
        }
        return;
    }
    const auto held = store->second->reservations.find(id);
    if (held != store->second->reservations.end() && held->second == plan) {
        write_store(player).reservations.erase(id); update_store_record(player); changed(player);
    }
}
FreestoreAssignment QualifiedFreestore::assign(gc::PlayerId player, sim::EntityId id, std::uint64_t plan) {
    if (!contains(player, id)) return FreestoreAssignment::failed;
    const auto& held = stores_.at(player)->reservations;
    const auto reservation = held.find(id);
    if ((reservation == held.end() ? 0 : reservation->second) != plan) return FreestoreAssignment::failed;
    const auto* row = facts(id);
    if (!row || row->object.transit || !row->object.planet) return FreestoreAssignment::waiting;
    remove(player, id); return FreestoreAssignment::assigned;
}
core::Result<std::shared_ptr<const FreestorePool>> QualifiedFreestore::publish(gc::PlayerId player,
    const sim::PartitionExecutor& executor) const {
    using Result = core::Result<std::shared_ptr<const FreestorePool>>;
    const auto found = stores_.find(player);
    if (found == stores_.end()) return unavailable_store<std::shared_ptr<const FreestorePool>>("freestore publication requires an initialized player");
    if (found->second->published) return Result::success(found->second->published);
    auto pool = std::make_shared<FreestorePool>(); pool->qualified = true; pool->qualified_players.insert(player);
    std::vector<const FreestoreFacts*> rows;
    for (const auto id : found->second->members) {
        const auto* row = facts(id);
        if (!row || row->object.owner != player || !row->qualified)
            return unavailable_store<std::shared_ptr<const FreestorePool>>("retained freestore identity facts are unavailable");
        rows.push_back(row);
    }
    pool->reservations = found->second->reservations;
    pool->objects.resize(rows.size());
    const auto stage = executor.execute_phase("galactic-ai-qualified-freestore", sim::tick_partition_count, [&](std::size_t part) {
        const auto range = sim::partition_range(part, rows.size());
        for (auto i = range.begin; i < range.end; ++i) {
            pool->objects[i] = rows[i]->object;
            // WGA-301: arbitrary nesting is not the current-type member bit.
            if (pool->objects[i].kind == FreestoreKind::member) pool->objects[i].kind = FreestoreKind::unit;
        }
    });
    if (!stage) return Result::failure(stage.error());
    pool->work.objects = rows.size(); found->second->published = pool; return Result::success(std::move(pool));
}
core::Result<std::shared_ptr<const FreestorePool>> QualifiedFreestore::publish(const sim::PartitionExecutor& executor) const {
    gc_profile::Scope profile_scope(gc_profile::Phase::freestore_publish);
    using Result = core::Result<std::shared_ptr<const FreestorePool>>;
    if (published_) return Result::success(published_);
    auto pool = std::make_shared<FreestorePool>(); pool->qualified = true;
    for (const auto& [player, store] : stores_) {
        static_cast<void>(store);
        auto rows = publish(player, executor);
        if (!rows) return rows;
        pool->players.emplace(player, rows.value());
        pool->qualified_players.insert(player);
        pool->objects.insert(pool->objects.end(), rows.value()->objects.begin(), rows.value()->objects.end());
        pool->reservations.insert(rows.value()->reservations.begin(), rows.value()->reservations.end());
    }
    std::sort(pool->objects.begin(), pool->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    // WGA-436: only the combined object facade needs globally removed handles.
    std::set<sim::EntityId> removed = lost_;
    for (const auto& [id, row] : facts_) if (row.dead || row.deleting) removed.insert(id);
    pool->removed.assign(removed.begin(), removed.end());
    pool->work.objects = pool->objects.size(); published_ = std::move(pool); return Result::success(published_);
}
std::span<const sim::EntityId> QualifiedFreestore::additions(gc::PlayerId player) const noexcept {
    const auto found = stores_.find(player); return found == stores_.end() ? std::span<const sim::EntityId>{} : std::span<const sim::EntityId>(found->second->additions);
}
core::Result<void> QualifiedFreestore::consume_additions(gc::PlayerId player, std::size_t count) {
    const auto found = stores_.find(player);
    if (found == stores_.end() || count > found->second->additions.size()) return unavailable_store<void>("freestore callback frontier is invalid");
    if (count) {
        auto& additions = write_store(player).additions;
        additions.erase(additions.begin(), additions.begin() + static_cast<std::ptrdiff_t>(count));
        update_store_record(player);
        // Callback consumption changes native order, not the pool or its safety inputs.
        if (changed_) changed_();
    }
    return core::Result<void>::success();
}
void QualifiedFreestore::update_fact_record(sim::EntityId id) {
    const auto& fact = facts_.at(id); QualifiedFreestore row; row.facts_.emplace(id, fact);
    records_.set(fact.object.owner, 1, id, row.encode());
}
void QualifiedFreestore::update_store_record(gc::PlayerId player) {
    QualifiedFreestore row; row.stores_.emplace(player, stores_.at(player)); records_.set(player, 2, 0, row.encode());
}
core::RecordDigest QualifiedFreestore::rebuild_records() const {
    core::RecordDigest rebuilt;
    for (const auto& [id, fact] : facts_) {
        QualifiedFreestore row; row.facts_.emplace(id, fact); rebuilt.set(fact.object.owner, 1, id, row.encode());
    }
    for (const auto& [player, store] : stores_) {
        QualifiedFreestore row; row.stores_.emplace(player, store); rebuilt.set(player, 2, 0, row.encode());
    }
    for (const auto id : lost_) { core::RecordDigest::Bytes key; core::RecordDigest::append(key, id); rebuilt.set(0, 3, id, key); }
    return rebuilt;
}
std::string QualifiedFreestore::record_sync_hash(bool cold) const {
    return cold ? rebuild_records().digest("qualified-freestore-v3") : records_.digest("qualified-freestore-v3");
}
std::vector<std::uint8_t> QualifiedFreestore::encode() const {
    std::vector<std::uint8_t> bytes; store_word(bytes, 3); store_word(bytes, facts_.size());
    for (const auto& [id, row] : facts_) {
        store_word(bytes, id); store_word(bytes, row.object.type); store_word(bytes, row.object.owner); store_word(bytes, row.object.planet);
        store_word(bytes, static_cast<std::uint64_t>(row.object.kind)); store_word(bytes, row.object.transport); store_word(bytes, row.object.transit);
        store_word(bytes, row.object.directly_parented_to_planet); store_word(bytes, row.object.safe ? (*row.object.safe ? 2 : 1) : 0);
        store_word(bytes, row.current_type); store_word(bytes, row.parent); store_word(bytes, row.parent_is_planet); store_word(bytes, row.current_type_member);
        store_word(bytes, row.hero); store_word(bytes, row.ai_usable); store_word(bytes, row.dead); store_word(bytes, row.deleting);
        store_word(bytes, row.death_clone); store_word(bytes, row.qualified); store_word(bytes, row.enroute); store_word(bytes, row.tracked_movement);
        store_word(bytes, row.unit_ai); bytes.push_back(row.prefer_ground ? 1 : 0);
    }
    store_word(bytes, stores_.size());
    for (const auto& [player, store] : stores_) {
        store_word(bytes, player); store_word(bytes, store->members.size()); for (const auto id : store->members) store_word(bytes, id);
        store_word(bytes, store->reservations.size()); for (const auto& [id, plan] : store->reservations) { store_word(bytes, id); store_word(bytes, plan); }
        store_word(bytes, store->additions.size()); for (const auto id : store->additions) store_word(bytes, id);
    }
    store_word(bytes, lost_.size()); for (const auto id : lost_) store_word(bytes, id);
    return bytes;
}
core::Result<void> QualifiedFreestore::restore(std::span<const std::uint8_t> bytes, sim::EntityId next_id, std::span<const gc::GalacticPlayer> players) {
    QualifiedFreestore staged; std::size_t at{}; bool valid = true;
    const auto read = [&]() { std::uint64_t value{}; if (bytes.size() - at < 8) { valid = false; return value; }
        for (unsigned i = 0; i < 8; ++i) { value |= static_cast<std::uint64_t>(bytes[at++]) << (8 * i); }
        return value; };
    const auto flag = [&]() { const auto value = read(); if (value > 1) valid = false; return value != 0; };
    const auto owner = [&](gc::PlayerId player) { return std::any_of(players.begin(), players.end(), [&](const auto& p) { return p.player_id == player; }); };
    if (read() != 3) return unavailable_store<void>("freestore state version is invalid");
    const auto count = read(); if (!valid || count > (bytes.size() - at) / 177) return unavailable_store<void>("freestore actor framing is invalid");
    sim::EntityId last{};
    for (std::uint64_t i = 0; i < count; ++i) {
        FreestoreFacts row; row.object.id = read(); row.object.type = read(); const auto player = read(); row.object.planet = read();
        const auto kind = read(); row.object.kind = static_cast<FreestoreKind>(kind);
        row.object.transport = flag(); row.object.transit = flag(); row.object.directly_parented_to_planet = flag(); const auto safe = read();
        if (safe) row.object.safe = safe == 2;
        row.current_type = read(); row.parent = read(); row.parent_is_planet = flag(); row.current_type_member = flag(); row.hero = flag();
        row.ai_usable = flag(); row.dead = flag(); row.deleting = flag(); row.death_clone = flag(); row.qualified = flag(); row.enroute = flag(); row.tracked_movement = flag();
        row.unit_ai = flag();
        if (at == bytes.size() || bytes[at] > 1) return unavailable_store<void>("freestore preference byte is invalid");
        row.prefer_ground = bytes[at++] != 0;
        if (player > std::numeric_limits<gc::PlayerId>::max()) valid = false;
        row.object.owner = static_cast<gc::PlayerId>(player);
        if (!valid || kind > static_cast<std::uint64_t>(FreestoreKind::space_structure) || safe > 2 || row.object.id <= last
            || row.object.id >= next_id || row.parent >= next_id || row.object.planet >= next_id || !owner(row.object.owner) || !valid_facts(row)
            || row.object.transit != (!row.object.planet || row.enroute || row.tracked_movement)) return unavailable_store<void>("freestore actor identities or flags are invalid");
        staged.facts_.emplace(row.object.id, row); last = row.object.id;
    }
    const auto stores = read(); if (!valid || stores > players.size()) return unavailable_store<void>("freestore manager count is invalid");
    gc::PlayerId prior{};
    for (std::uint64_t i = 0; i < stores; ++i) {
        const auto player = read(); const auto members = read();
        if (!valid || player > std::numeric_limits<gc::PlayerId>::max() || player <= prior || !owner(static_cast<gc::PlayerId>(player)) || members > (bytes.size() - at) / 8)
            return unavailable_store<void>("freestore membership framing is invalid");
        auto& slot = staged.stores_[static_cast<gc::PlayerId>(player)]; slot = std::make_shared<Store>();
        auto& store = *slot; last = 0;
        for (std::uint64_t m = 0; m < members; ++m) {
            const auto id = read(); const auto* row = staged.facts(id);
            if (!valid || id <= last || !row || row->object.owner != player) return unavailable_store<void>("freestore membership owner reference is invalid");
            store.members.insert(id); last = id;
        }
        const auto reservations = read(); if (!valid || reservations > members || reservations > (bytes.size() - at) / 16) return unavailable_store<void>("freestore reservation framing is invalid");
        last = 0;
        for (std::uint64_t r = 0; r < reservations; ++r) { const auto id = read(), plan = read();
            if (!valid || id <= last || !plan || !store.members.contains(id)) return unavailable_store<void>("freestore reservation reference is invalid");
            store.reservations.emplace(id, plan); last = id;
        }
        const auto additions = read(); if (!valid || additions > members || additions > (bytes.size() - at) / 8) return unavailable_store<void>("freestore added callback framing is invalid");
        std::set<sim::EntityId> seen;
        for (std::uint64_t a = 0; a < additions; ++a) { const auto id = read();
            if (!valid || !store.members.contains(id) || !seen.insert(id).second) return unavailable_store<void>("freestore added callback reference is invalid");
            store.additions.push_back(id);
        }
        prior = static_cast<gc::PlayerId>(player);
    }
    const auto lost = read(); if (!valid || lost > (bytes.size() - at) / 8) return unavailable_store<void>("freestore lost identity framing is invalid");
    last = 0;
    for (std::uint64_t i = 0; i < lost; ++i) {
        const auto id = read();
        if (!valid || id <= last || id >= next_id || staged.facts_.contains(id)) return unavailable_store<void>("freestore lost identity is invalid");
        staged.lost_.insert(id); last = id;
    }
    if (!valid || at != bytes.size()) return unavailable_store<void>("freestore state has trailing or truncated bytes");
    facts_ = std::move(staged.facts_); stores_ = std::move(staged.stores_); lost_ = std::move(staged.lost_); records_ = rebuild_records(); changed(); return core::Result<void>::success();
}
core::Result<std::vector<sim::EntityId>> initial_freestore_offers(const gc::GalacticSnapshot& snapshot, std::span<const FreestoreType> types) {
    std::map<gc::TypeId, bool> transport;
    for (const auto& row : types) if (!row.type || !transport.emplace(row.type, row.transport).second)
        return unavailable_store<std::vector<sim::EntityId>>("initial freestore needs unique original-type traits");
    std::vector<sim::EntityId> out;
    for (const auto& planet : snapshot.planets()) {
        if (const auto* control = snapshot.control()) for (const auto& surface : control->planets) if (surface.planet == planet.id)
            for (const auto& unit : surface.landed) { const auto found = transport.find(unit.type);
                if (found == transport.end()) return unavailable_store<std::vector<sim::EntityId>>("landed actor transport trait is absent");
                if (found->second) out.push_back(unit.id);
            }
        for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == planet.id && !fleet.motion.transit)
            for (const auto& member : fleet.members) out.push_back(member.id);
    }
    return core::Result<std::vector<sim::EntityId>>::success(std::move(out));
}
core::Result<std::vector<sim::EntityId>> initial_freestore_offers(const gc::GalacticSnapshot& snapshot,
    const QualifiedFreestore& owner) {
    std::vector<sim::EntityId> out;
    for (const auto& planet : snapshot.planets()) {
        if (const auto* control = snapshot.control()) for (const auto& surface : control->planets) if (surface.planet == planet.id)
            for (const auto& unit : surface.landed) {
                const auto* row = owner.facts(unit.id);
                if (!row || !row->qualified) return unavailable_store<std::vector<sim::EntityId>>("initial freestore landed current representation is absent");
                if (row->object.transport) out.push_back(unit.id);
            }
        for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == planet.id && !fleet.motion.transit)
            for (const auto& member : fleet.members) {
                const auto* row = owner.facts(member.id);
                if (!row || !row->qualified) return unavailable_store<std::vector<sim::EntityId>>("initial freestore fleet actor facts are absent");
                out.push_back(member.id);
            }
    }
    return core::Result<std::vector<sim::EntityId>>::success(std::move(out));
}
namespace {
ai::GalacticFreestoreSelectionProvider freestore_selection_provider(std::shared_ptr<const FreestoreInput> input,
    std::shared_ptr<const ai::GalacticWorld> world, std::weak_ptr<const ai::Engine> manager, FreestoreSelectionBatch batch) {
    return [input = std::move(input), world = std::move(world), manager = std::move(manager), batch = std::move(batch)]
        (gc::PlayerId player, gc::PlanetId target, const ai::PlanDef& plan) -> core::Result<std::vector<ai::GalacticSelectionObject>> {
        if (!input || !input->pool || !input->pool->qualified || !input->pool->qualified_players.contains(player)
            || !world || !manager.lock() || !batch)
            return unavailable_store<std::vector<ai::GalacticSelectionObject>>("qualified freestore membership, assignment, path and garrison producers are required");
        std::vector<ai::GalacticSelectionObject> out;
        const auto shard = input->pool->players.find(player);
        const auto& objects = shard == input->pool->players.end() ? input->pool->objects : shard->second->objects;
        const auto selection = batch(player, target, plan);
        for (const auto& row : objects) if (row.owner == player) {
            const auto copied = selection(row.id);
            if (!copied) return core::Result<std::vector<ai::GalacticSelectionObject>>::failure(copied.error());
            const auto cost = copied.value().first;
            if ((cost.repr & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL || cost < Number(0))
                return unavailable_store<std::vector<ai::GalacticSelectionObject>>("freestore selection cost must be qualified finite and nonnegative");
            // BTP-S02: failed staging rejects the actor before allocation and
            // WGA-60 path normalization, including an actor already reserved.
            if (cost == ai::galactic_goals::rejected_estimate()) continue;
            out.push_back({row.id, cost, copied.value().second, row.type, row.transit});
        }
        return core::Result<std::vector<ai::GalacticSelectionObject>>::success(std::move(out));
    };
}
}
ai::GalacticFreestoreSelectionProvider qualified_freestore_selection(std::shared_ptr<const FreestoreInput> input,
    std::shared_ptr<const ai::GalacticWorld> world, std::shared_ptr<const ai::Engine> engine, FreestoreSelectionFacts selection) {
    // A per-actor reader has nothing to hoist: each actor's read is the whole batch.
    FreestoreSelectionBatch batch;
    if (selection) batch = [selection = std::move(selection)](gc::PlayerId player, gc::PlanetId target, const ai::PlanDef& plan) -> FreestoreActorSelection {
        return [selection, player, target, &plan](sim::EntityId actor) { return selection(player, target, actor, plan); };
    };
    return freestore_selection_provider(std::move(input), std::move(world), std::weak_ptr<const ai::Engine>(engine), std::move(batch));
}
ai::GalacticFreestoreSelectionProvider qualified_freestore_batch_selection(std::shared_ptr<const FreestoreInput> input,
    std::shared_ptr<const ai::GalacticWorld> world, std::shared_ptr<const ai::Engine> engine, FreestoreSelectionBatch batch) {
    return freestore_selection_provider(std::move(input), std::move(world), std::weak_ptr<const ai::Engine>(engine), std::move(batch));
}
core::Result<void> register_qualified_freestore_bindings(auth::ScriptScheduler& scripts, std::weak_ptr<QualifiedFreestore> owner) {
    return register_qualified_freestore_bindings(scripts, [owner] { return owner.lock(); });
}
core::Result<void> register_qualified_freestore_bindings(auth::ScriptScheduler& scripts,
    std::function<std::shared_ptr<QualifiedFreestore>()> owner) {
    auto result = scripts.register_method(handle_game_object, "Prevent_AI_Usage", [owner](auth::BindingContext& context, const auth::ValueList& args) {
        const auto retained = owner ? owner() : nullptr;
        const auto* actor = args.size() == 2 ? detail::as_handle(args[0], handle_game_object) : nullptr;
        const auto* prevent = args.size() == 2 ? std::get_if<bool>(&args[1].data) : nullptr;
        if (!retained || !actor || !prevent) return detail::fail("Prevent_AI_Usage requires a retained galactic actor and Boolean");
        // Private transactional shadow; partitioned Lua never mutates the owner.
        return context.apply_immediate("galactic.prevent_ai_usage", args);
    });
    if (!result) return result;
    return scripts.register_method(handle_game_object, "Set_Prefer_Ground_Over_Space", [owner](auth::BindingContext& context, const auth::ValueList& args) {
        const auto retained = owner ? owner() : nullptr;
        // WGA-409 consumes the first Boolean; unlike Prevent_AI_Usage,
        // additional Lua arguments are ignored before the canonical effect.
        const auto* actor = args.size() >= 2 ? detail::as_handle(args[0], handle_game_object) : nullptr;
        const auto* prefer = args.size() >= 2 ? std::get_if<bool>(&args[1].data) : nullptr;
        if (!retained || !actor || !prefer) return detail::fail("Set_Prefer_Ground_Over_Space requires a live actor and Boolean");
        return context.apply_immediate("galactic.prefer_ground", {args[0], args[1]});
    });
}
} // namespace eawr::script::foc::galactic_ai
