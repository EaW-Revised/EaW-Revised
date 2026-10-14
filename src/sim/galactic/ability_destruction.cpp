#include "session_impl.hpp"
#include "encoding.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "eawr/sim/galactic/snapshot.hpp"

#include <algorithm>

// WGCH-58/113..115/137/138/155..158: live strategic planet destruction on the shared ability host. The command
// carries the ORIGINAL supplied object and the target planet; everything else is resolved at execution.
namespace eawr::sim::galactic {
namespace {
using math::Fixed;
template<class Rows, class Id, class Key> auto find(Rows& rows, Id id, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), id,
        [&](const auto& row, auto value) { return key(row) < value; });
    return at != rows.end() && key(*at) == id ? &*at : nullptr;
}
template<class Rows, class Value, class Key> void put(Rows& rows, Value value, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), key(value),
        [&](const auto& row, auto id) { return key(row) < id; });
    if (at != rows.end() && key(*at) == key(value)) *at = std::move(value);
    else rows.insert(at, std::move(value));
}
}

// WGCH-155/156: a nontransport supplies itself; a transport scans its contents in stored order and stops at the
// FIRST object whose current type authors an enabled destruction ability. Nothing is pinned at preparation.
std::optional<CensusObject> GalacticSession::Impl::resolve_destruction_bearer(const CensusObject& supplied) const {
    const auto has_ability = [&](const CensusObject& row) {
        const auto* profile = find(setup.ability_host->profiles, row.current_type, [](const auto& r) { return r.type; });
        return profile && profile->destruction;
    };
    const bool transport = [&] {
        const auto* traits = movement_inputs ? find(movement_inputs->types, supplied.current_type, [](const auto& r) { return r.type; }) : nullptr;
        return traits && traits->transport;
    }();
    if (!transport) return has_ability(supplied) ? std::optional(supplied) : std::nullopt;
    const auto* contents = movement_inputs ? find(movement_inputs->contents, supplied.id, [](const auto& r) { return r.container; }) : nullptr;
    if (!contents) return std::nullopt;
    for (const auto& child : contents->children) {
        const auto row = actor_records.find(child.id);
        if (row != actor_records.end() && !row->second.dead && !row->second.delete_pending && has_ability(row->second))
            return row->second;
    }
    return std::nullopt;
}

// WGCH-113/137/138: preparation-time admission shared by prepare and execute. Contested orbit is NOT read here.
bool GalacticSession::Impl::destruction_admitted(const CensusObject& bearer, const PlanetId planet) const {
    const auto* profile = find(setup.ability_host->profiles, bearer.current_type, [](const auto& r) { return r.type; });
    const auto* service = find(ability_host->actors, bearer.id, [](const auto& r) { return r.actor; });
    const auto* surface = control ? find(control->planets, planet, [](const auto& r) { return r.planet; }) : nullptr;
    if (!profile || !profile->destruction || !service || !service->destruction_enabled || service->destruction_cancelled
        || !surface || surface->destroyed || !planets.contains(planet)) return false;
    // WGCH-138: an actor already contained at ANOTHER planet is refused; no containing planet is allowed.
    if (bearer.planet && bearer.planet != planet) return false;
    const auto& names = setup.ability_host->activated_destruction_abilities;
    if (std::find(names.begin(), names.end(), profile->destruction->name) == names.end()) return false;
    // WGCH-113: only the TARGET allegiance player gates, and only when it is a valid player.
    const auto index = static_cast<std::size_t>(surface - control->planets.data());
    const auto target = control->state[index].target_player;
    if (target) {
        if (target == bearer.owner) return false;
        for (const auto& relation : control->relations)
            if (relation.player == bearer.owner && relation.other == target && relation.allied) return false;
    }
    return true;
}

void GalacticSession::Impl::prepare_destruction(const GameCommand& command, const CensusObject& supplied,
    std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto target = std::get<PrepareGalacticAbility>(command.payload).planet;
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, supplied.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    const auto bearer = resolve_destruction_bearer(supplied);
    if (!bearer || !destruction_admitted(*bearer, target)) return reject();
    AbilityPreparation prepared; prepared.actor = supplied.id; prepared.planet = target;
    prepared.kind = AbilityKind::planet_destruction; prepared.open = true;
    prepared.offers.push_back({1, Fixed{}, false}); // WGCH-115: no price; one retained identity only
    prepared.id = ability_host->next_request++;
    event.kind = GalacticEventKind::ability_prepared; event.value = prepared.id;
    put(ability_host->preparations, std::move(prepared), [](const auto& p) { return p.actor; });
    touch_abilities(); events.push_back(event);
}

void GalacticSession::Impl::execute_destruction(const GameCommand& command, const CensusObject& supplied,
    const PreparedAbility& prepared, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, supplied.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    if (!control || !production || !economy_corruption || !ability_consumption_handler || prepared.planet == 0) return reject();
    const auto bearer = resolve_destruction_bearer(supplied);
    if (!bearer || !destruction_admitted(*bearer, prepared.planet)) return reject();
    const auto* surface = find(control->planets, prepared.planet, [](const auto& r) { return r.planet; });
    const auto index = static_cast<std::size_t>(surface - control->planets.data());
    const auto* service = find(ability_host->actors, bearer->id, [](const auto& r) { return r.actor; });
    // Everything that can refuse for an unresolved named seam is planned before the first mutation.
    auto ruins = plan_planet_ruins(prepared.planet, true);
    if (!ruins) return reject();
    auto* retained = find(ability_host->preparations, supplied.id, [](const auto& r) { return r.actor; });
    const auto settle = [&](AbilityOutcome outcome) {
        if (retained && retained->kind == AbilityKind::planet_destruction) retained->open = false;
        AbilityResult receipt{ability_host->next_request++, prepared.preparation, supplied.id, prepared.planet, supplied.owner, 0, outcome};
        receipt.kind = AbilityKind::planet_destruction;
        put(ability_host->results, std::move(receipt), [](const auto& r) { return r.actor; });
        touch_abilities();
        event.kind = GalacticEventKind::ability_settled; event.value = prepared.preparation; events.push_back(event);
    };
    // WGCH-58/158: with the contested-space flag set, any nonstealth nonallied orbital fleet refuses BEFORE the
    // pending victory request and ruins. The public receipt carries no fleet identity, type or count (#2792).
    if (!service->contested_check_disabled) {
        std::vector<Fleet> orbit = sorted_fleets();
        for (const auto& fleet : orbit) {
            if (fleet.orbit != prepared.planet || fleet.motion.transit) continue;
            bool allied = fleet.owner == bearer->owner;
            for (const auto& relation : control->relations)
                if (relation.player == bearer->owner && relation.other == fleet.owner && relation.allied) allied = true;
            if (allied) continue;
            const auto skips = fleet_combat_skip(fleet, actor_inputs(), movement_inputs.get());
            if (!skips) { failed = true; return; }
            if (!skips.value()) return settle(AbilityOutcome::refused);
        }
    }
    // WGCH-114: exact current-type match inside landed transport contents, read at the CURRENT controller.
    std::vector<TypeId> landed_types;
    for (const auto& force : surface->landed) {
        const auto* contents = movement_inputs ? find(movement_inputs->contents, force.id, [](const auto& r) { return r.container; }) : nullptr;
        if (!contents) continue;
        for (const auto& child : contents->children) {
            const auto row = actor_records.find(child.id);
            landed_types.push_back(row == actor_records.end() ? child.type : row->second.current_type);
        }
    }
    auto leader_kill = planet_destruction_victory(bearer->owner, control->state[index].current_player, landed_types);
    if (!leader_kill) { failed = true; return; }
    auto cleaned = execute_planet_ruins(*ruins);
    if (!cleaned) { failed = true; return; }
    // WGCH-115: no debit, actor consumption, weapon lock, orbit destruction, starbase clear or ownership change.
    settle(AbilityOutcome::purchased);
}

core::Result<void> GalacticSession::set_destruction_contested_check(const EntityId actor, const bool enabled) {
    auto& impl = *impl_;
    auto* state = impl.ability_host ? find(impl.ability_host->actors, actor, [](const auto& a) { return a.actor; }) : nullptr;
    const auto* profile = state ? find(impl.setup.ability_host->profiles, state->type, [](const auto& p) { return p.type; }) : nullptr;
    if (!profile || !profile->destruction)
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command, "contested-space check requires a live destruction actor"));
    if (!impl.native_replaying) {
        std::vector<std::uint8_t> bytes; detail::put_u8(bytes, 2); detail::put_u64(bytes, actor); detail::put_u8(bytes, enabled ? 1 : 0);
        auto recorded = append_native_effect(NativeEffectKind::ability_lifecycle, std::move(bytes));
        if (!recorded) return recorded;
    }
    state->contested_check_disabled = !enabled;
    impl.touch_abilities();
    return core::Result<void>::success();
}
} // namespace eawr::sim::galactic
