#include "game_session_impl.hpp"
#include "game_session_diagnostic.hpp"
#include "checkpoint_owner_internal.hpp"
#include "battle_return_internal.hpp"
#include "eawr/campaign/invasion_offer.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/sim/galactic/victory.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <set>
#include <tuple>
#include <utility>

namespace eawr::campaign {
using game_session_detail::fail;
namespace {
core::Result<tactical::PlayerCommand> decode_order(const galactic::TacticalLayerCommand& input,
    const tactical::TacticalSetup& setup, const sim::CommandKey key) {
    // Reuse every ordinary tactical opcode's shape checks, wrapping only the small player
    // table around the body. Never copy the world's entities/content for command routing.
    if (input.body.size() < 32 || input.body.size() > 16384 || input.body[20] != input.opcode)
        return fail<tactical::PlayerCommand>("invalid tactical command body or opcode");
    tactical::TacticalReplay wrapper;
    wrapper.setup.players = setup.players;
    wrapper.setup.queue_identities = setup.queue_identities;
    wrapper.final_tick_count = 1;
    auto encoded = tactical::write_replay(wrapper);
    if (!encoded) return core::Result<tactical::PlayerCommand>::failure(encoded.error());
    auto bytes = std::move(encoded).value();
    overwrite(bytes, 64, 1, 8); // header command count
    auto body = input.body;
    // The untrusted common key is replaced by the authenticated seat-derived key.
    overwrite(body, 0, 0, 8);
    overwrite(body, 8, key.player_id, 4);
    overwrite(body, 12, key.sequence, 8);
    put(bytes, body.size(), 4);
    bytes.insert(bytes.end(), body.begin(), body.end());
    auto decoded = tactical::parse_replay(bytes);
    if (!decoded) return core::Result<tactical::PlayerCommand>::failure(decoded.error());
    auto command = std::move(decoded.value().commands.front());
    command.key = key;
    return core::Result<tactical::PlayerCommand>::success(std::move(command));
}
template <class A, class T> void checkpoint_game_owner(A& a, T& v) {
    a(v.tick, v.pending, v.last, v.executed, v.ended, v.last_battle,
        v.conflict_services, v.admission, v.queued_admissions, v.waiting_admissions,
        v.admissions, v.replay_admissions, v.votes, v.withdrawing_player, v.conflict_pause,
        v.conflicts, v.load_fixups, v.replay_fixups, v.replay_conflicts, v.replay_selected, v.history,
        v.timelines->galaxy, v.timelines->child, v.timelines->historical,
        v.interactive_seat.value, v.summary_conflict);
}
} // namespace

core::Result<void> GameSession::stage_battle(ConflictRecord record, const tactical::TacticalContent& content,
    ScriptRequirement requirement) {
    auto scripts = impl_->scripts_for(record, requirement);
    if (!scripts) return core::Result<void>::failure(scripts.error());
    return stage_battle(std::move(record), content, std::move(scripts).value());
}
core::Result<void> GameSession::stage_battle(ConflictRecord record, const tactical::TacticalContent& content,
    authoritative::ScriptScheduler scripts) {
    if (impl_->failure || impl_->child || impl_->staged || record.id == 0 || impl_->ended.contains(record.id))
        return fail<void>("a conflict needs a fresh ID and an idle, healthy game session");
    if (record.forces.size() > galactic::galactic_max_objects || record.seats.size() > galactic::galactic_max_seats)
        return fail<void>("conflict record exceeds the session resource limits", galactic::diagnostic_codes::resource_limit);
    const auto snapshot = impl_->world().snapshot();
    if (record.invader == record.defender || record.invader == 0 || record.defender == 0
        || std::none_of(snapshot->planets().begin(), snapshot->planets().end(), [&](const auto& p) { return p.id == record.planet; }))
        return fail<void>("conflict needs two different owners and an existing planet");
    const bool reserved_admission = impl_->admission && impl_->admission->conflict.id == record.id;
    if (reserved_admission && record.tactical.first_free_entity_id)
        record.tactical.first_free_entity_id = std::max(*record.tactical.first_free_entity_id, impl_->world().next_entity_id());
    const auto first = record.tactical.first_free_entity_id;
    if (!first || *first <= record.id || impl_->world().next_entity_id() == sim::invalid_entity_id
        || (!reserved_admission && record.id < impl_->world().next_entity_id()))
        return fail<void>("conflict setup must reserve its ID and the shared allocator boundary");
    record.tactical.seed = child_seed(impl_->world().setup().seed, record.id);
    if (auto valid = tactical::validate_setup(record.tactical); !valid) return valid;
    if (content.retreat) tactical::resolve_cinematic_seats(record.tactical, content.economy.players);
    for (const auto owner : {record.invader, record.defender}) {
        if (std::none_of(snapshot->players().begin(), snapshot->players().end(), [owner](const auto& p) { return p.player_id == owner; })
            || std::none_of(record.tactical.players.begin(), record.tactical.players.end(), [owner](const auto& p) { return p.player_id == owner && p.commandable(); }))
            return fail<void>("both conflict owners must be known commandable tactical players");
    }
    sim::EntityId previous{};
    for (const auto& force : record.forces) {
        if (force.id <= previous || force.id >= *first || force.id == record.id || force.type == 0
            || static_cast<unsigned>(force.role) > static_cast<unsigned>(ForceRole::orbital)
            || (force.owner != record.invader && force.owner != record.defender && (force.role != ForceRole::orbital
                || std::none_of(snapshot->players().begin(), snapshot->players().end(), [&](const auto& p) { return p.player_id == force.owner; }))))
            return fail<void>("persistent force IDs and owners are invalid");
        previous = force.id;
        if (force.role == ForceRole::orbital) {
            bool matched{};
            if (snapshot->production()) for (const auto& planet : snapshot->production()->planets)
                if (planet.planet == record.planet) for (const auto& object : planet.objects)
                    if (object.id == force.parent) matched = true;
            if (!matched || !force.parent || force.destroy_after_tactical)
                return fail<void>("orbital copy must retain its live source structure");
        } else if (force.role != ForceRole::escort) {
            const auto fleet = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& f) { return f.id == force.parent && f.owner == force.owner; });
            std::vector<galactic::FleetMember> members;
            if (fleet != snapshot->fleets().end()) members = fleet->members;
            std::set<sim::EntityId> visited;
            bool matched = false;
            while (!members.empty() && visited.size() <= galactic::galactic_max_objects) {
                const auto member = members.back(); members.pop_back();
                if (!visited.insert(member.id).second) continue;
                if (member.id == force.id && member.type == force.type) { matched = true; break; }
                if (const auto* movement = snapshot->movement()) {
                    const auto contents = std::find_if(movement->contents.begin(), movement->contents.end(),
                        [&](const auto& row) { return row.container == member.id; });
                    if (contents != movement->contents.end())
                        members.insert(members.end(), contents->children.begin(), contents->children.end());
                }
            }
            if (!matched)
                return fail<void>("persistent forces must match their galactic parent and owner");
        }
        const auto unit = std::lower_bound(record.tactical.units.begin(), record.tactical.units.end(), force.id,
            [](const auto& row, const auto id) { return row.entity_id < id; });
        const bool deployed = unit != record.tactical.units.end() && unit->entity_id == force.id;
        if ((!deployed && force.role != ForceRole::reinforcement && force.role != ForceRole::retained && force.role != ForceRole::rider)
            || (deployed && (unit->type_id != (force.tactical_type ? force.tactical_type : force.type) || unit->owner != force.owner))
            || (deployed && force.purchase_type && tactical::purchase_identity(*unit) != force.purchase_type)
            || (force.role == ForceRole::rider && !tactical_force_alive(force, record.tactical.units)))
            return fail<void>("deployed persistent force must retain its ID, type and owner");
    }
    galactic::SeatId previous_seat{};
    for (const auto& binding : record.seats) {
        const auto* issuer = impl_->seat(binding.seat);
        if (binding.seat <= previous_seat || issuer == nullptr || !issuer->active
            || (binding.player != 0 && (binding.player != issuer->player
                || std::none_of(record.tactical.players.begin(), record.tactical.players.end(), [&](const auto& p) { return p.player_id == binding.player && p.commandable(); }))))
            return fail<void>("battle seats must be active, ordered and retain their owner; temporary roles await participation policy");
        previous_seat = binding.seat;
    }
    auto world = tactical::TacticalSession::create(record.tactical, content);
    if (!world) return core::Result<void>::failure(world.error());
    auto child = authoritative::ScriptedTacticalSession::create(std::move(world).value(), std::move(scripts));
    if (!child) return core::Result<void>::failure(child.error());
    child.value().set_state_hasher(impl_->hasher);
    if (impl_->services(record).configure) {
        if (auto configured = impl_->services(record).configure(child.value()); !configured) return configured;
    }
    child.value().set_authoritative_hash(true);
    auto script_hash = child.value().script_state_hash();
    if (!script_hash) return core::Result<void>::failure(script_hash.error());
    const auto world_hash = sim::StateHash(child.value().world().state_sha256());
    auto record_hash = conflict_hash(record);
    if (!record_hash) return core::Result<void>::failure(record_hash.error());
    child.value().set_shared_mode_service([owner = impl_.get()] { return owner->update_child_timeline(); });
    impl_->staged.emplace(Impl::Child{std::move(record), std::move(child).value(), {}, {}, {}, std::move(script_hash).value(), std::move(record_hash).value(), world_hash});
    return core::Result<void>::success();
}
core::Result<void> GameSession::queue_conflict(ConflictRecord record, Arbitration arbitration) { return impl_->queue_conflict(std::move(record), arbitration); }
core::Result<void> GameSession::Impl::queue_conflict(ConflictRecord record, Arbitration arbitration) {
    auto& impl = *this;
    if (impl.failure || impl.child || impl.staged || impl.queued_admissions.size() + impl.waiting_admissions.size() >= galactic::galactic_max_objects
        || !impl.services(record).content || !impl.conflict_services
        || !record.id || impl.ended.contains(record.id)
        || record.id < impl.world().next_entity_id() || !record.tactical.first_free_entity_id
        || *record.tactical.first_free_entity_id <= record.id || record.invader == record.defender
        || !record.invader || !record.defender
        || static_cast<std::uint8_t>(arbitration.route) > static_cast<std::uint8_t>(ConflictRoute::tactical))
        return fail<void>("conflict admission needs a fresh identity, pinned content and an idle service");
    // The host's live request is the only way a result can settle; a flagged conflict without one would fail at its result.
    if (record.corruption_mission != (record.corruption_request != 0))
        return fail<void>("corruption mission conflicts carry exactly their live host request");
    if (auto valid = tactical::validate_setup(record.tactical); !valid) return valid;
    const auto snapshot = impl.world().snapshot();
    if (std::none_of(snapshot->planets().begin(), snapshot->planets().end(), [&](const auto& p) { return p.id == record.planet; }))
        return fail<void>("conflict planet is absent");
    for (const auto owner : {record.invader, record.defender})
        if (std::none_of(snapshot->players().begin(), snapshot->players().end(), [&](const auto& p) { return p.player_id == owner; }))
            return fail<void>("conflict owner is absent");
    sim::EntityId previous{};
    for (const auto& force : record.forces) {
        if (force.id <= previous || force.id >= *record.tactical.first_free_entity_id || force.id == record.id || !force.type
            || static_cast<unsigned>(force.role) > static_cast<unsigned>(ForceRole::orbital)
            || (force.owner != record.invader && force.owner != record.defender && (force.role != ForceRole::orbital
                || std::none_of(snapshot->players().begin(), snapshot->players().end(), [&](const auto& p) { return p.player_id == force.owner; }))))
            return fail<void>("invalid admitted force identities");
        previous = force.id;
    }
    if ((impl.admission && impl.admission->conflict.id == record.id)
        || (!impl.queued_admissions.empty() && record.id < *impl.queued_admissions.back().conflict.tactical.first_free_entity_id)
        || std::any_of(impl.queued_admissions.begin(), impl.queued_admissions.end(), [&](const auto& e) { return e.conflict.id == record.id; })
        || std::any_of(impl.waiting_admissions.begin(), impl.waiting_admissions.end(), [&](const auto& e) { return e.conflict.id == record.id; }))
        return fail<void>("duplicate admitted conflict identity");
    impl.queued_admissions.push_back(RecordedAdmission{impl.tick, std::move(record), arbitration});
    return core::Result<void>::success();
}
core::Result<void> GameSession::select_pending_battle(bool fight, const sim::PartitionExecutor& executor) {
    auto& impl = *impl_;
    if (!impl.admission) return fail<void>("no pending conflict");
    const auto& entry = *impl.admission;
    if (fight && entry.conflict.layer == BattleLayer::land) {
        // M3 scope (gc-tactical evaluation boundary): land uses the solver seam.
        // A sourced forcing story/tutorial restriction cannot be silently bypassed.
        if (entry.arbitration.forced_tactical) return fail<void>("forced land tactical selection requires the M4 entry service");
        fight = false;
    }
    if (fight) {
        auto conflict = entry.conflict;
        const auto selected = impl.replay_selected.find(conflict.id);
        const bool recorded_selection = selected != impl.replay_selected.end();
        if (recorded_selection) {
            if (selected->second.entry_tick != impl.tick) return fail<void>("recorded child selection tick differs");
            conflict = selected->second.conflict;
        }
        if (!recorded_selection) conflict.structure_markers = impl.return_state.structure_markers;
        if (!recorded_selection) {
            conflict.upgrades.clear();
            for (const auto& row : impl.return_state.upgrades) if (row.planet == conflict.planet) conflict.upgrades.push_back(row.upgrade);
        }
        auto prepared = [&]() -> core::Result<ResolvedConflictEntry> {
            if (!recorded_selection && !conflict.tactical.skirmish && impl.services(conflict).entry)
                return impl.services(conflict).entry(conflict, *impl.world().snapshot(), impl.world().next_entity_id());
            if (!impl.services(conflict).content) return fail<ResolvedConflictEntry>("tactical content provider is unavailable");
            auto content = impl.services(conflict).content(conflict);
            if (!content) return core::Result<ResolvedConflictEntry>::failure(content.error());
            return core::Result<ResolvedConflictEntry>::success({conflict, std::move(content).value()});
        }();
        if (!prepared) return core::Result<void>::failure(prepared.error());
        conflict = std::move(prepared.value().conflict);
        if (conflict.id != entry.conflict.id || conflict.layer != entry.conflict.layer
            || conflict.planet != entry.conflict.planet || conflict.invader != entry.conflict.invader
            || conflict.defender != entry.conflict.defender || conflict.seats != entry.conflict.seats)
            return fail<void>("entry resolver changed the admitted conflict identity or seats");
        auto staged = stage_battle(conflict, prepared.value().content);
        if (!staged) return staged;
        if (recorded_selection) impl.replay_selected.erase(selected);
    } else {
        if (!impl.auto_resolver) return core::Result<void>::success(); // #2023/2024 solver/return seam
        auto result = impl.auto_resolver(entry.conflict, *impl.world().snapshot(), executor);
        if (!result) return core::Result<void>::failure(result.error());
        if (!result.value()) return core::Result<void>::success();
        const auto& application = *result.value();
        const auto& resolved = application.result;
        if (application.planet != entry.conflict.planet || application.invader != entry.conflict.invader
            || application.defender != entry.conflict.defender || application.land != (entry.conflict.layer == BattleLayer::land)
            || resolved.conflict != entry.conflict.id
            || (resolved.winner != entry.conflict.invader && resolved.winner != entry.conflict.defender)
            || resolved.forces.size() != entry.conflict.forces.size()) return fail<void>("invalid auto-resolve outcome");
        for (std::size_t i = 0; i < resolved.forces.size(); ++i)
            if (resolved.forces[i].id != entry.conflict.forces[i].id) return fail<void>("auto-resolve force identities differ");
        // WAR-49..63: one journaled return, including control, losses and retreat.
        // Tactical return adapters use the same application services.
        auto staged = stage_auto_resolve_outcome(application);
        if (!staged) return staged;
        impl.ended.insert(entry.conflict.id);
        // The auto-resolver stays in the parent tail. GalacticSession seals this
        // logical tick once; it must not take the child's frontier-advance route.
        if (impl.conflict_pause) { auto resumed = impl.world().adjust_conflict_pause(false); if (!resumed) return resumed; }
        impl.conflict_pause = false;
    }
    impl.admission.reset(); impl.votes.clear(); impl.withdrawing_player = 0;
    return core::Result<void>::success();
}
core::Result<void> GameSession::submit(const galactic::GameCommand& command) {
    if (std::holds_alternative<galactic::PostLoadFixup>(command.payload)) return fail<void>("load fixups are internal restore commands");
    if (impl_->failure) return core::Result<void>::failure(*impl_->failure);
    const auto* issuer = impl_->seat(command.key.seat);
    if (issuer == nullptr) return fail<void>("unknown game-command seat", galactic::diagnostic_codes::unknown_issuer);
    if (command.key.tick < impl_->tick) return fail<void>("session tick already executed", galactic::diagnostic_codes::late_command);
    if (command.key.tick >= galactic::galactic_max_ticks || impl_->pending.size() + impl_->history.executed_dropped + impl_->executed.size() >= galactic::galactic_max_commands)
        return fail<void>("session command limit reached", galactic::diagnostic_codes::resource_limit);
    const std::pair order{command.key.tick, command.key.sequence};
    const auto old = impl_->last.find(command.key.seat);
    if (old != impl_->last.end() && !(old->second < order))
        return fail<void>("command does not follow this seat's previous submission", galactic::diagnostic_codes::command_order);
    // WGM-174/177, GCMP-04: a battle choice is admitted at execution from the agreed session state, never here
    // from a publication: the alert view carries the local seat, speech and wall-clock facts, which differ
    // between peers (process_session_commands).
    const auto layer = galactic::layer_of(command.payload);
    if (galactic::decode_payload(layer, galactic::opcode_of(command.payload), galactic::encode_payload(command.payload)) != std::optional(command.payload))
        return fail<void>("malformed game command");
    if (const auto* input = std::get_if<galactic::TacticalLayerCommand>(&command.payload)) {
        const auto player = impl_->player(command.key.seat);
        if (!impl_->child || impl_->role_pending() || impl_->child->conflict.id != input->conflict || player == 0 || !issuer->active)
            return fail<void>("no matching battle or this seat has no commandable battle side");
        auto decoded = decode_order(*input, impl_->child->conflict.tactical, {0, player, 1});
        if (!decoded) return core::Result<void>::failure(decoded.error());
        if (impl_->world().seat_control() && issuer->kind == galactic::SeatKind::human
            && !tactical::human_wire_payload(decoded.value().payload, impl_->reserve_admission(player))) return fail<void>("human battle seat cannot issue engine-only orders");
    } else if ((std::holds_alternative<galactic::DebugSetPlanetOwner>(command.payload)
        || std::holds_alternative<galactic::DebugSetFleetOrbit>(command.payload)) && !impl_->world().setup().debug_commands)
        return fail<void>("debug commands are off in this session");
    impl_->pending.emplace(command.key, command);
    impl_->last.insert_or_assign(command.key.seat, order);
    return core::Result<void>::success();
}
core::Result<void> GameSession::execute_invasion_accept(galactic::SeatId seat, sim::EntityId conflict, bool& summary_closed) {
    auto& impl = *impl_;
    const auto* issuer = impl.seat(seat);
    if (!issuer || !issuer->active || issuer->kind != galactic::SeatKind::human || !impl.latest_result
        || !impl.latest_context || impl.latest_result->conflict != conflict || impl.child || impl.admission || impl.staged
        || !impl.waiting_admissions.empty()) return fail<void>("invasion offer is no longer available");
    const auto snapshot = impl.world().snapshot();
    auto offer = observe_invasion_offer(*impl.latest_result, *impl.latest_context, *snapshot,
        issuer->player, impl.world().native_invasion_available(), impl.summary_conflict == conflict);
    if (!offer.accept_permitted) return fail<void>("invasion offer has no permitted surviving transports");
    // Native admission sees the same unpaused return boundary as ordinary Continue.
    // A refusal restores this level; an acceptance by the summary's own seat ends the hold with it (below).
    struct RestoreSummaryPause {
        galactic::GalacticSession& world; bool held;
        ~RestoreSummaryPause() { if (held) (void)world.adjust_conflict_pause(true); }
    } restore_pause{impl.world(), impl.summary_conflict == conflict};
    if (restore_pause.held) {
        if (auto released = impl.world().adjust_conflict_pause(false); !released) return released;
    }
    // WGP-136: resolve missing identities first, cap the event before ownership
    // filtering, and never backfill filtered entries from beyond that cap.
    constexpr std::size_t accepted_limit = 127;
    std::vector<sim::EntityId> resolved;
    for (const auto& prepared : impl.latest_result->invasion_offers) for (const auto id : prepared.transports) {
        if (resolved.size() == accepted_limit) break;
        const bool exists = std::any_of(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& fleet) {
            return std::any_of(fleet.members.begin(), fleet.members.end(), [&](const auto& member) { return member.id == id; });
        });
        if (exists) resolved.push_back(id);
    }
    bool admitted_any{};
    for (const auto& row : offer.candidates) {
        std::vector<sim::EntityId> selected;
        for (const auto id : resolved)
            if (std::find(row.transports.begin(), row.transports.end(), id) != row.transports.end()) selected.push_back(id);
        if (selected.empty()) continue;
        // The InvasionAccept command is the journal entry: no native effect is appended for it.
        auto admitted = impl.world().admit_native_invasion_by_command(issuer->player, row.fleet, selected);
        if (!admitted) return core::Result<void>::failure(admitted.error());
        if (!admitted.value().admitted) return fail<void>("retained invasion admission refused");
        for (auto& prepared : impl.latest_result->invasion_offers)
            if (prepared.fleet == row.fleet && prepared.planet == row.planet) prepared.transports.clear();
        // No presentation reader polls the queue receipt. Its conflict owner keeps
        // pending requests alive until actual completion, then prunes them.
        if (auto released = impl.world().retire_native_invasion_by_command(admitted.value().request); !released) return released;
        admitted_any = true;
    }
    if (!admitted_any) return fail<void>("capped invasion selection has no eligible transports");
    for (auto& prepared : impl.latest_result->invasion_offers) prepared.transports.clear();
    // The recorded acceptance also closes its own seat's summary, as the ordinary Continue does.
    if (restore_pause.held && seat == impl.interactive_seat) {
        restore_pause.held = false; impl.summary_conflict = 0; summary_closed = true;
    }
    return core::Result<void>::success();
}
struct GameSession::StepContext {
    GameTick out;
    sim::StateHash world_hash;
    std::vector<tactical::PlayerCommand> inputs;
    std::vector<galactic::GameCommand> galaxy_inputs;
    std::size_t published_conflict_events{};
    bool parent_mode_serviced{};
    bool controls_applied{};
    bool has_tick_input{};
    bool save_only{true};
};

core::Result<void> GameSession::process_session_commands(StepContext& tick_context, const sim::PartitionExecutor& executor) {
    auto& impl = *impl_;
    auto& out = tick_context.out;
    auto& inputs = tick_context.inputs;
    auto& galaxy_inputs = tick_context.galaxy_inputs;
    auto& controls_applied = tick_context.controls_applied;
    auto& has_tick_input = tick_context.has_tick_input;
    auto& save_only = tick_context.save_only;
    while (!impl.pending.empty() && impl.pending.begin()->first.tick == impl.tick) {
        auto command = std::move(impl.pending.begin()->second);
        impl.pending.erase(impl.pending.begin());
        has_tick_input = true;
        save_only = save_only && std::holds_alternative<galactic::SaveRequest>(command.payload);
        if (const auto* closed = std::get_if<galactic::BattleSummaryAcknowledge>(&command.payload)) {
            const auto* issuer = impl.seat(command.key.seat);
            if (!issuer || !issuer->active || issuer->kind != galactic::SeatKind::human
                || command.key.seat != impl.interactive_seat || closed->conflict != impl.summary_conflict)
                out.diagnostics.push_back(fail<void>("summary acknowledgement does not match its owning seat and conflict").error());
            else if (auto released = impl.release_summary(); !released) return released;
        } else if (impl.summary_conflict && (std::holds_alternative<galactic::PauseGalaxy>(command.payload)
            || std::holds_alternative<galactic::ResumeGalaxy>(command.payload))) {
            if (std::holds_alternative<galactic::ResumeGalaxy>(command.payload) && impl.world().clock().pause_depth <= 1)
                out.diagnostics.push_back(fail<void>("summary pause requires its matching acknowledgement").error());
            else {
                auto controlled = impl.world().apply_session_controls(std::span(&command, 1), false, controls_applied);
                controls_applied = true;
                if (!controlled) return controlled;
            }
        } else if (const auto* order = std::get_if<galactic::TacticalLayerCommand>(&command.payload)) {
            const auto player = impl.player(command.key.seat);
            const auto* issuer = impl.seat(command.key.seat);
            if (!impl.child || impl.role_pending() || !issuer || !issuer->active || order->conflict != impl.child->conflict.id || player == 0)
                out.diagnostics.push_back(fail<void>("queued command's battle ended before execution").error());
            else {
                auto decoded = decode_order(*order, impl.child->conflict.tactical,
                    {impl.child->session.completed_tick(), player, ++impl.child->sequences[player]});
                if (!decoded) out.diagnostics.push_back(decoded.error());
                else if (impl.world().seat_control() && issuer->kind == galactic::SeatKind::human
                    && !tactical::human_wire_payload(decoded.value().payload, impl.reserve_admission(player)))
                    out.diagnostics.push_back(fail<void>("human battle seat cannot issue engine-only orders").error());
                else inputs.push_back(std::move(decoded).value());
            }
        } else if (const auto* choice = std::get_if<galactic::BattleChoice>(&command.payload)) {
            const auto diagnostics_before = out.diagnostics.size();
            const auto* issuer = impl.seat(command.key.seat);
            const auto human_side = [&](auto player) {
                return std::any_of(impl.world().setup().seats.begin(), impl.world().setup().seats.end(), [&](const auto& seat) {
                    return seat.active && seat.kind == galactic::SeatKind::human && seat.player == player;
                });
            };
            if (!impl.admission || !impl.conflict_pause || choice->conflict != impl.admission->conflict.id || !issuer || !issuer->active
                || issuer->kind != galactic::SeatKind::human
                || (impl.admission->arbitration.route != ConflictRoute::vote
                    && impl.admission->arbitration.route != ConflictRoute::pending_tactical)
                || (issuer->player != impl.admission->conflict.invader && issuer->player != impl.admission->conflict.defender)
                || (!choice->tactical && impl.admission->arbitration.forced_tactical))
                out.diagnostics.push_back(fail<void>("battle choice does not belong to this pending conflict or side").error());
            else if (human_side(impl.admission->conflict.invader) && human_side(impl.admission->conflict.defender)
                && impl.votes.contains(issuer->player))
                // WGM-176/177: a side that already voted waits for its opponent. Decided here from the recorded votes,
                // the same on every peer, not from a local alert publication at submit time.
                out.diagnostics.push_back(fail<void>("battle choice is unavailable while waiting for the opponent").error());
            else {
                impl.votes[issuer->player] = choice->tactical;
                const auto& conflict = impl.admission->conflict;
                std::optional<bool> selected;
                if (human_side(conflict.invader) && human_side(conflict.defender)) {
                    // The manager's vote progression/timeout owner decides; do not invent a
                    // majority rule or derive wall-clock time from logical frames (WGM-47).
                    if (impl.vote_progress) {
                        auto progress = impl.vote_progress(conflict, impl.votes);
                        if (!progress) { impl.failure = progress.error(); return core::Result<void>::failure(*impl.failure); }
                        selected = progress.value();
                    }
                } else selected = choice->tactical;
                if (selected && !*selected && impl.admission->arbitration.forced_tactical) {
                    out.diagnostics.push_back(fail<void>("vote result cannot bypass forced tactical selection").error());
                    selected.reset();
                }
                if (selected) {
                    if (!*selected || (conflict.layer == BattleLayer::land && !impl.admission->arbitration.forced_tactical))
                        impl.admission->arbitration.route = ConflictRoute::instant_auto;
                    else {
                        auto selection = select_pending_battle(true, executor);
                        if (!selection) out.diagnostics.push_back(selection.error());
                    }
                }
            }
            out.battle_choice_responses.push_back({choice->conflict, command.key.seat,
                out.diagnostics.size() == diagnostics_before ? std::string{} : out.diagnostics.back().message});
        } else if (const auto* accept = std::get_if<galactic::InvasionAccept>(&command.payload)) {
            bool summary_closed{};
            auto accepted = execute_invasion_accept(command.key.seat, accept->conflict, summary_closed);
            if (!accepted) out.diagnostics.push_back(accepted.error());
            out.invasion_accept_responses.push_back({accept->conflict, command.key.seat,
                accepted ? std::string{} : accepted.error().message, summary_closed});
        } else if (const auto* withdraw = std::get_if<galactic::AutoResolveWithdraw>(&command.payload)) {
            const auto* issuer = impl.seat(command.key.seat);
            const auto progress = auto_resolve_progress();
            if (!impl.admission || !impl.conflict_pause || !issuer || !issuer->active
                || issuer->kind != galactic::SeatKind::human || !progress
                || withdraw->conflict != impl.admission->conflict.id || progress->conflict != withdraw->conflict
                || impl.withdrawing_player || !impl.auto_withdraw || !impl.auto_withdraw(withdraw->conflict, issuer->player))
                out.diagnostics.push_back(fail<void>("withdrawal is unavailable for this conflict").error());
            else impl.withdrawing_player = issuer->player;
        } else if (impl.child) {
            if (std::holds_alternative<galactic::PauseGalaxy>(command.payload)
                || std::holds_alternative<galactic::ResumeGalaxy>(command.payload)
                || std::holds_alternative<galactic::SetSpeed>(command.payload)
                || std::holds_alternative<galactic::SaveRequest>(command.payload)
                || std::holds_alternative<galactic::SeatLeave>(command.payload)
                || std::holds_alternative<galactic::SeatRejoin>(command.payload)
                || std::holds_alternative<galactic::AssignBattleTeamRole>(command.payload)) {
                // Canonical seat order: a departure/choice takes effect before later inputs.
                auto controlled = impl.world().apply_session_controls(std::span(&command, 1), true, controls_applied);
                controls_applied = true;
                if (!controlled) { impl.failure = controlled.error(); return core::Result<void>::failure(*impl.failure); }
                if (auto refreshed = impl.refresh_battle_seats(); !refreshed) { impl.failure = refreshed.error(); return core::Result<void>::failure(*impl.failure); }
                impl.child->galaxy_hash = impl.galaxy_hash().value();
            }
            else out.diagnostics.push_back(fail<void>("galactic action is unavailable while a child is active").error());
        } else {
            auto local = command; local.key.tick = impl.world().completed_tick();
            if (impl.scripts) galaxy_inputs.push_back(local);
            else if (auto submitted = impl.world().submit(local); !submitted) out.diagnostics.push_back(submitted.error());
        }
        impl.executed.push_back(std::move(command));
    }
    return core::Result<void>::success();
}
core::Result<void> GameSession::step_child(StepContext& tick_context, const sim::PartitionExecutor& executor) {
    auto& impl = *impl_;
    auto& out = tick_context.out;
    auto& world_hash = tick_context.world_hash;
    auto& inputs = tick_context.inputs;
    auto& controls_applied = tick_context.controls_applied;
    auto& has_tick_input = tick_context.has_tick_input;
    auto& save_only = tick_context.save_only;
    if (controls_applied) for (const auto& event : impl.world().snapshot()->events())
        if (event.kind == galactic::GalacticEventKind::save_requested || event.kind == galactic::GalacticEventKind::save_rejected)
            out.save_events.push_back(event);
    if (impl.restoring_child || impl.role_pending() || (has_tick_input && save_only)) {
        // Recorded SaveRequest-only barriers reproduce paused-child saves without
        // advancing the child world or hosted Lua. Outer controls retain their tick.
        out.battle = impl.child->session.world().snapshot();
    } else {
    if (impl.world().seat_control()) impl.child->session.set_order_admission([this](const authoritative::TacticalOrder& order) {
        // WGS-P07: delegated/manual control suppresses AI gameplay orders, while
        // engine-only story/reward effects retain their separate authority.
        const bool engine_effect = tactical::payload_authority(order.payload) == tactical::PayloadAuthority::engine_only
            && !std::holds_alternative<tactical::AiReservationDebitPayload>(order.payload);
        return engine_effect || battle_controller(order.issuer) == galactic::FactionControl::ai;
    });
    auto stepped = impl.child->session.step(executor, inputs);
    if (!stepped) { impl.failure = stepped.error(); return core::Result<void>::failure(*impl.failure); }
    out.battle = stepped.value().world.snapshot;
    out.asteroid_impacts = stepped.value().world.asteroid_impacts;
    world_hash = stepped.value().world.state_hash;
    impl.child->world_hash = world_hash;
    impl.child->script_hash = std::move(stepped.value().script_sha256);
    out.battle_state_hash = world_hash;
    out.diagnostics.insert(out.diagnostics.end(), stepped.value().refused_input.begin(), stepped.value().refused_input.end());
    out.diagnostics.insert(out.diagnostics.end(), stepped.value().world.diagnostics.begin(), stepped.value().world.diagnostics.end());
    for (const auto& [id, killer] : stepped.value().world.deaths) impl.child->killers[id] = killer;
    const auto& outcome = impl.child->session.world().outcome();
    if (outcome && stepped.value().world.completed_tick > outcome->end_tick) {
        if (auto returned = reconcile_child_return(tick_context, executor); !returned) return returned;
    }
    }
    return core::Result<void>::success();
}
core::Result<void> GameSession::reconcile_child_return(StepContext& tick_context, const sim::PartitionExecutor& executor) {
    auto& impl = *impl_;
    auto& out = tick_context.out;
    out.result = impl.result();
    bool return_weapon_killed = false;
    const bool weapon_was_alive = !out.result->superweapon_destroyed;
    if (impl.world().victory() && impl.child->conflict.superweapon && weapon_was_alive) {
        const auto& conflict = impl.child->conflict;
        const auto weapon = std::find_if(conflict.forces.begin(), conflict.forces.end(), [&](const auto& force) { return force.id == conflict.superweapon; });
        if (weapon != conflict.forces.end() && weapon->owner != out.result->winner) {
            const auto& killers = impl.world().victory()->context.superweapon_killers;
            for (std::size_t i = 0; i < conflict.forces.size(); ++i)
                if (conflict.forces[i].owner == out.result->winner && out.result->forces[i].disposition == ForceDisposition::survived
                    && std::find(killers.begin(), killers.end(), conflict.forces[i].type) != killers.end()) { return_weapon_killed = true; break; }
            if (return_weapon_killed) {
                // WGS-28: feed the weapon casualty into the existing return commit once.
                const auto index = static_cast<std::size_t>(weapon - conflict.forces.begin());
                out.result->forces[index].disposition = ForceDisposition::destroyed;
                out.result->forces[index].final_blow_player = out.result->winner;
                out.result->superweapon_destroyed = true; out.result->superweapon_killer = out.result->winner;
            } else out.result->retreat_player = out.result->winner;
        }
    }
    const auto& context = impl.child->conflict;
    if (impl.services(context).decorate) {
        auto decorated = impl.services(context).decorate(context, impl.child->session.world(), *out.result);
        if (!decorated) { impl.failure = decorated.error(); return core::Result<void>::failure(*impl.failure); }
    }
    auto planned = context.corruption_mission ? core::Result<TacticalReturnPlan>::success({*out.result, {}, {}, {}})
        : plan_tactical_return(context, *out.result, *impl.world().snapshot());
    if (!planned) { impl.failure = planned.error(); return core::Result<void>::failure(*impl.failure); }
    out.result = planned.value().result;
    if (impl.hero_resolver && !context.corruption_mission) for (std::size_t i = 0; i < context.forces.size(); ++i) {
        if (!out.result->forces[i].hero_defeated) continue;
        auto defeat = impl.hero_resolver(context, context.forces[i], out.result->forces[i], *impl.world().snapshot());
        if (!defeat) { impl.failure = defeat.error(); return core::Result<void>::failure(*impl.failure); }
        if (!defeat.value()) continue;
        for (auto& companion : defeat.value()->companions) {
            companion.dead = std::binary_search(planned.value().destroyed.begin(), planned.value().destroyed.end(), companion.id);
            companion.deletion_pending = companion.dead;
        }
        auto scheduled = schedule_hero_defeat(*defeat.value(), executor);
        if (!scheduled) { impl.failure = scheduled.error(); return core::Result<void>::failure(*impl.failure); }
        planned.value().destroyed.insert(planned.value().destroyed.end(), scheduled.value().destroyed_companions.begin(), scheduled.value().destroyed_companions.end());
    }
    std::sort(planned.value().destroyed.begin(), planned.value().destroyed.end());
    planned.value().destroyed.erase(std::unique(planned.value().destroyed.begin(), planned.value().destroyed.end()), planned.value().destroyed.end());
    auto replay = impl.child->session.record();
    auto reconciled = impl.world().reconcile_battle(impl.child->session.world().next_entity_id(),
        impl.tick + 1, impl.child->session.completed_tick());
    if (reconciled && impl.world().setup().control && !impl.child->conflict.raid && !impl.child->conflict.corruption_mission) {
        const auto& conflict = impl.child->conflict;
        const auto parties = resolve_conflict_parties(nullptr, {.admitted = &conflict});
        // WGM-57: a space return without a tactical station clears the stored base.
        const auto station_level = conflict.layer != BattleLayer::land && !conflict.station ? 0 : out.result->station_level;
        auto returned = build_control_battle_return(conflict, *out.result, parties, station_level);
        for (std::size_t i = 0; i < conflict.forces.size(); ++i) {
            const auto& force = conflict.forces[i]; const auto& result = out.result->forces[i];
            if (result.disposition == ForceDisposition::destroyed || result.disposition == ForceDisposition::destroy_after_tactical)
                returned.destroyed.push_back(force.id);
            else if (returned.land && force.owner == conflict.invader && result.disposition == ForceDisposition::survived) {
                const auto& traits = impl.world().setup().fleet_control;
                const auto transport = traits && std::any_of(traits->types.begin(), traits->types.end(),
                    [&](const auto& row) { return row.type == force.type && row.transport; });
                if (transport) returned.surviving_invaders.push_back({force.id, force.type, force.owner});
            }
        }
        reconciled = impl.world().apply_control_battle(returned);
    }
    if (reconciled && !context.corruption_mission && impl.world().victory() && impl.child->conflict.superweapon && weapon_was_alive) {
        const auto& conflict = impl.child->conflict;
        const auto weapon = std::find_if(conflict.forces.begin(), conflict.forces.end(), [&](const auto& force) { return force.id == conflict.superweapon; });
        if (weapon != conflict.forces.end()) {
            const auto loser = out.result->winner == conflict.invader ? conflict.defender : conflict.invader;
            reconciled = impl.world().apply_superweapon_victory_return(weapon->owner, out.result->winner, loser, return_weapon_killed);
        }
    }
    if (reconciled && context.corruption_mission) reconciled = impl.world().finish_corruption_mission(context.corruption_request,
        out.result->winner == context.invader ? galactic::CorruptionMissionState::succeeded : galactic::CorruptionMissionState::failed);
    if (reconciled && !context.corruption_mission) reconciled = apply_tactical_return(impl.world(), context, planned.value(), impl.return_state, executor);
    if (reconciled && !context.corruption_mission && impl.services(context).apply) reconciled = impl.services(context).apply(impl.world(), *out.result);
    if (reconciled && !context.corruption_mission) reconciled = impl.world().finish_native_invasion(context.id, out.result->winner);
    if (!reconciled) { impl.failure = reconciled.error(); return core::Result<void>::failure(*impl.failure); }
    if (!context.corruption_mission) out.result->invasion_offers = tactical_invasion_offers(context, *out.result, *impl.world().snapshot());
    if (!context.corruption_mission) if (auto noticed = impl.battle_story(context.planet, context.layer, out.result->winner,
        out.result->winner == context.invader ? context.defender : context.invader); !noticed) {
        impl.failure = noticed.error(); return core::Result<void>::failure(*impl.failure);
    }
    if (auto noticed = impl.battle_notice(GalacticAiBattleKind::returned, context, &*out.result); !noticed) {
        impl.failure = noticed.error(); return core::Result<void>::failure(*impl.failure);
    }
    impl.latest_result = *out.result; impl.latest_context = context;
    if (!context.corruption_mission) impl.return_state.structure_markers = context.structure_markers;
    impl.last_battle = std::move(replay);
    impl.world().end_battle_roles();
    impl.timelines->historical = impl.timelines->child;
    impl.timelines->historical_slot = std::move(impl.timelines->child_slot);
    impl.timelines->historical_slot->retired = true;
    impl.timelines->child.reset();
    impl.ended.insert(impl.child->conflict.id); impl.child.reset();
    if (impl.conflict_pause) {
        auto resumed = impl.world().adjust_conflict_pause(false);
        if (!resumed) { impl.failure = resumed.error(); return core::Result<void>::failure(*impl.failure); }
        impl.conflict_pause = false;
    }
    if (auto held = impl.hold_summary(context.id, context.invader, context.defender); !held) return held;
    return core::Result<void>::success();
}
core::Result<void> GameSession::step_galaxy(StepContext& tick_context, const sim::PartitionExecutor& executor) {
    auto& impl = *impl_;
    auto& out = tick_context.out;
    auto& galaxy_inputs = tick_context.galaxy_inputs;
    auto& parent_mode_serviced = tick_context.parent_mode_serviced;
    auto& published_conflict_events = tick_context.published_conflict_events;
    struct TailReset {
        galactic::GalacticSession& world;
        ~TailReset() { world.set_conflict_tail_handler({}); }
    } tail_reset{impl.world()};
    parent_mode_serviced = true;
    impl.world().set_timeline_service([owner = &impl](const auto& inputs) { return owner->update_galaxy_timeline(inputs); });
    // GFS-15/GFD-26: the outer tail below owns the frame timer only for conflict services or a victory
    // monitor (the condition that advances it there, and the barrier lag hosted scripts see). A tail
    // installed only for pending hero returns leaves the timer with the galaxy step.
    const bool outer_timer = impl.conflict_services || impl.world().victory();
    if (impl.conflict_services || !impl.return_state.heroes.empty()) impl.world().set_conflict_tail_handler([&](galactic::GalacticSession&) -> core::Result<void> {
        auto placement = impl.hero_placement;
        auto structures = impl.hero_structures;
        if (impl.live_hero_returns.place) placement = [&](const auto& record, const auto planet) {
            return impl.live_hero_returns.place(impl.world(), record, planet, executor);
        };
        if (impl.live_hero_returns.structures) structures = [&](const auto& record) {
            return impl.live_hero_returns.structures(impl.world(), record, executor);
        };
        // Retain this snapshot while placement publishes replacement snapshots.
        const auto hero_snapshot = impl.world().snapshot();
        const auto hero_count = impl.return_state.heroes.size();
        if (auto serviced = galactic::service_hero_returns(impl.return_state.heroes, impl.world().clock().galactic_frame,
            impl.world().clock().pause_depth != 0, hero_snapshot->planets(), placement,
            structures); !serviced) return serviced;
        if (impl.return_state.heroes.size() != hero_count)
            impl.world().sync_victory_hero_returns(impl.return_state.heroes);
        if (!impl.conflict_services) return core::Result<void>::success();
        const auto tail_snapshot = impl.world().snapshot();
        // WGM-43 / GFD-18: the conflict tail receives the arrival in this frame,
        // announces it, and returns before the next eligible arbitration invocation.
        const auto commit_admissions = [&]() -> core::Result<void> {
            for (const auto& entry : impl.queued_admissions) {
                auto reserved = impl.world().reserve_conflict_entities(*entry.conflict.tactical.first_free_entity_id);
                if (!reserved) return reserved;
                impl.admissions.push_back(entry); impl.waiting_admissions.push_back(entry);
                for (const auto& force : entry.conflict.forces)
                    out.conflict_events.push_back({impl.tick, galactic::GalacticEventKind::combat_imminent, {},
                        force.id, 0, entry.conflict.id, galactic::GalacticRejectReason::none});
                out.conflict_events.push_back({impl.tick, galactic::GalacticEventKind::planet_combat_begin, {},
                    entry.conflict.planet, 0, entry.conflict.id, galactic::GalacticRejectReason::none});
            }
            impl.queued_admissions.clear(); return core::Result<void>::success();
        };
        const auto replay_entries = impl.replay_admissions.equal_range(impl.tick);
        const bool replaying_admission = replay_entries.first != replay_entries.second;
        for (auto replay = replay_entries.first; replay != replay_entries.second; ++replay) {
            auto queued = queue_conflict(replay->second.conflict, replay->second.arbitration);
            if (queued) queued = commit_admissions();
            if (!queued) { impl.failure = queued.error(); return core::Result<void>::failure(*impl.failure); }
        }
        impl.replay_admissions.erase(replay_entries.first, replay_entries.second);
        if (auto committed = commit_admissions(); !committed) { impl.failure = committed.error(); return core::Result<void>::failure(*impl.failure); }
        if (impl.world().replaying_native_effects()) {
            if (auto replayed = impl.world().replay_native_invasion_service(); !replayed) return replayed;
        } else if (impl.layer(BattleLayer::land).services.native_admission && !impl.admission && !impl.staged && impl.waiting_admissions.empty()) {
            const auto* invasion = impl.world().pending_native_invasion();
            if (invasion && invasion->state == galactic::GalacticSession::NativeInvasion::State::pending) {
                const auto request = invasion->request;
                auto candidate = impl.layer(BattleLayer::land).services.native_admission(*impl.world().snapshot(), *invasion, impl.world().next_entity_id());
                if (!candidate) return core::Result<void>::failure(candidate.error());
                sim::EntityId conflict{};
                if (candidate.value()) {
                    const auto& record = candidate.value()->conflict;
                    if (record.layer != BattleLayer::land || record.planet != invasion->planet || record.invader != invasion->invader || record.defender != invasion->defender)
                        return fail<void>("native invasion admission differs from its retained owners/location");
                    conflict = record.id;
                    if (auto queued = queue_conflict(record, candidate.value()->arbitration); !queued) return queued;
                    if (auto committed = commit_admissions(); !committed) return committed;
                }
                if (auto dispatched = impl.world().dispatch_native_invasion(request, conflict); !dispatched) return dispatched;
            }
        }
        if (!replaying_admission && !impl.staged) {
            for (const auto& event : tail_snapshot->events()) {
                if (event.kind != galactic::GalacticEventKind::movement_conflict
                    && event.kind != galactic::GalacticEventKind::land_conflict_requested) continue;
                const auto layer = event.kind == galactic::GalacticEventKind::land_conflict_requested ? BattleLayer::land : BattleLayer::space;
                const auto& admission = impl.layer(layer).services.admission;
                if (!admission) continue;
                auto candidate = admission(*tail_snapshot, event, impl.world().next_entity_id());
                if (!candidate) { impl.failure = candidate.error(); return core::Result<void>::failure(*impl.failure); }
                if (!candidate.value()) continue;
                auto queued = queue_conflict(candidate.value()->conflict, candidate.value()->arbitration);
                if (queued) queued = commit_admissions();
                if (!queued) { impl.failure = queued.error(); return core::Result<void>::failure(*impl.failure); }
            }
        }
        if (!impl.admission && !impl.waiting_admissions.empty()) {
            impl.admission = std::move(impl.waiting_admissions.front());
            impl.waiting_admissions.erase(impl.waiting_admissions.begin());
        }
        if (impl.admission && impl.tick > impl.admission->entry_tick && !impl.conflict_pause
            && (!impl.conflict_gate || impl.conflict_gate(*impl.world().snapshot()))) {
            auto paused = impl.world().adjust_conflict_pause(true);
            if (!paused) { impl.failure = paused.error(); return core::Result<void>::failure(*impl.failure); }
            impl.conflict_pause = true;
            const auto& conflict = impl.admission->conflict;
            out.pending_alerts.emplace(conflict.id, PendingConflictAlert{
                conflict.invader, conflict.defender, conflict.layer == BattleLayer::land});
            out.conflict_events.push_back({impl.tick, galactic::GalacticEventKind::conflict_pending, {},
                impl.admission->conflict.id, 0, impl.admission->conflict.planet, galactic::GalacticRejectReason::none});
        }
        if (impl.admission && impl.conflict_pause) {
            const auto route = impl.admission->arbitration.route;
            const auto automatic = route == ConflictRoute::instant_auto || route == ConflictRoute::dialog_auto;
            if (automatic || route == ConflictRoute::tactical) {
                auto selected = select_pending_battle(!automatic, executor);
                if (!selected) { impl.failure = selected.error(); return core::Result<void>::failure(*impl.failure); }
            }
        }
        impl.world().publish_conflict_events(out.conflict_events);
        published_conflict_events = out.conflict_events.size();
        return core::Result<void>::success();
    }, outer_timer);
    // WGS-99/142: the pending prompt (conflict pause held, nothing staged yet) is the only state that
    // admits the prebattle autosave; the live owner requests it one tick before the Begin choice runs.
    impl.world().set_prebattle_autosave_window(impl.admission.has_value() && impl.conflict_pause && !impl.staged && !impl.child);
    auto stepped = [&]() -> core::Result<galactic::GalacticTick> {
        if (!impl.scripts) return impl.world().step(executor, impl.staged.has_value() || impl.admission.has_value());
        auto hosted = impl.scripts->step(executor, galaxy_inputs, impl.staged.has_value() || impl.admission.has_value());
        if (!hosted) return core::Result<galactic::GalacticTick>::failure(hosted.error());
        out.diagnostics.insert(out.diagnostics.end(), hosted.value().refused_input.begin(), hosted.value().refused_input.end());
        for (const auto& routed : hosted.value().script_input) {
            if (routed.dropped) out.diagnostics.push_back(*routed.dropped);
            if (!routed.key) continue;
            // The bridge's accepted orders are part of the reconstruction envelope.
            auto recorded = *routed.accepted; recorded.key.tick = impl.tick;
            impl.executed.push_back(recorded);
            auto& last = impl.last[recorded.key.seat];
            last = std::max(last, std::pair(recorded.key.tick, recorded.key.sequence));
        }
        hosted.value().world.state_hash = hosted.value().state_hash;
        hosted.value().world.state_sha256 = impl.hasher ? std::string{} : hosted.value().state_hash.get();
        return core::Result<galactic::GalacticTick>::success(std::move(hosted).value().world);
    }();
    if (!stepped) { impl.failure = stepped.error(); return core::Result<void>::failure(*impl.failure); }
    out.galaxy_work = stepped.value().work;
    out.state_sha256 = std::move(stepped.value().state_sha256);
    out.state_hash = std::move(stepped.value().state_hash);
    for (const auto& event : stepped.value().snapshot->events())
        if (event.kind == galactic::GalacticEventKind::save_requested || event.kind == galactic::GalacticEventKind::save_rejected)
            out.save_events.push_back(event);
    if (impl.staged) {
        impl.conflicts.push_back({impl.tick, impl.staged->conflict});
        impl.child = std::move(impl.staged); impl.staged.reset();
        impl.timelines->child = ModeTimelineSnapshot{{SessionMode::battle, impl.child->conflict.id}, impl.tick + 1, 0};
        impl.timelines->child_slot = std::make_shared<Impl::ChildTimelinePublication>(Impl::ChildTimelinePublication{*impl.timelines->child, false});
        impl.child->owns_pause = impl.conflict_pause;
        if (auto noticed = impl.battle_notice(GalacticAiBattleKind::entered, impl.child->conflict); !noticed) {
            impl.failure = noticed.error(); return core::Result<void>::failure(*impl.failure);
        }
        if (impl.conflict_pause) out.conflict_events.push_back({impl.tick, galactic::GalacticEventKind::child_battle_selected,
            {}, impl.child->conflict.id, 0, impl.child->conflict.planet, galactic::GalacticRejectReason::none});
        auto role = impl.world().begin_battle_roles(impl.child->conflict.id, impl.child->conflict.invader, impl.child->conflict.defender);
        if (!role) { impl.failure = role.error(); return core::Result<void>::failure(*impl.failure); }
        if (auto refreshed = impl.refresh_battle_seats(); !refreshed) { impl.failure = refreshed.error(); return core::Result<void>::failure(*impl.failure); }
        impl.child->galaxy_hash = impl.galaxy_hash().value();
        out.battle = impl.child->session.world().snapshot();
        out.battle_state_hash = impl.child->world_hash;
    }
    return core::Result<void>::success();
}
core::Result<void> GameSession::apply_pending_returns(StepContext& tick_context, const sim::PartitionExecutor& executor) {
    auto& impl = *impl_;
    auto& out = tick_context.out;
    // GFD-25 / GFS-U07: explicitly selected campaign-return boundary. This does
    // not decide automatic conflict admission or add a galaxy frame service.
    while (!impl.world().pending_outcomes().empty()
        && impl.world().pending_outcomes().front().completed_tick <= impl.tick + 1) {
        if (impl.child || impl.staged) { impl.failure = fail<void>("auto-resolve return overlaps a child battle").error(); return core::Result<void>::failure(*impl.failure); }
        const auto entry = impl.world().pending_outcomes().front();
        auto application = decode_auto_resolve_outcome(entry.payload);
        if (!application || application.value().result.conflict != entry.conflict) {
            impl.failure = fail<void>("invalid recorded auto-resolve result").error(); return core::Result<void>::failure(*impl.failure);
        }
        auto committed = impl.world().commit_outcome(entry, [&](const auto&) {
            auto applied = apply_auto_resolve_outcome(impl.world(), application.value(), impl.outcome_services);
            if (!applied) return applied;
            return impl.world().finish_native_invasion(entry.conflict, application.value().result.winner);
        }, executor);
        if (!committed) { impl.failure = committed.error(); return core::Result<void>::failure(*impl.failure); }
        if (auto noticed = impl.battle_story(application.value().planet,
            application.value().land ? BattleLayer::land : BattleLayer::space, application.value().result.winner,
            application.value().result.winner == application.value().invader ? application.value().defender : application.value().invader); !noticed) {
            impl.failure = noticed.error(); return core::Result<void>::failure(*impl.failure);
        }
        if (impl.native_ai.battle) {
            const auto context = std::find_if(impl.admissions.begin(), impl.admissions.end(), [&](const auto& admission) {
                return admission.conflict.id == entry.conflict;
            });
            if (context == impl.admissions.end()) {
                impl.failure = fail<void>("native AI auto-resolve return needs its admitted conflict context").error();
                return core::Result<void>::failure(*impl.failure);
            }
            auto noticed = impl.battle_notice(GalacticAiBattleKind::returned, context->conflict, &application.value().result, true);
            if (!noticed) { impl.failure = noticed.error(); return core::Result<void>::failure(*impl.failure); }
        }
        out.result = application.value().result;
        impl.latest_auto_outcome = std::make_shared<const AutoResolveOutcomeApplication>(application.value());
        if (auto held = impl.hold_summary(entry.conflict, application.value().invader, application.value().defender); !held) return held;
        impl.outcome_history[impl.outcome_record_index] = std::move(application).value();
        impl.outcome_record_index = (impl.outcome_record_index + 1) % impl.outcome_history.size();
    }
    return core::Result<void>::success();
}
core::Result<GameTick> GameSession::finish_step(StepContext& tick_context) {
    auto& impl = *impl_;
    auto& out = tick_context.out;
    auto& world_hash = tick_context.world_hash;
    auto& parent_mode_serviced = tick_context.parent_mode_serviced;
    auto& published_conflict_events = tick_context.published_conflict_events;
    if (impl.summary_conflict && (impl.admission || impl.staged || impl.child || !impl.waiting_admissions.empty())) {
        if (auto released = impl.release_summary(); !released) return core::Result<GameTick>::failure(released.error());
    }
    impl.world().publish_conflict_events(std::span(out.conflict_events).subspan(published_conflict_events));
    // GFD-26/GFSC-18: query final active mode/pause after players and host scripts.
    // A selected child retains the parent; its transition-time timer reader is the
    // separately qualified scene/mode probe, not an assumed parent increment.
    // GFS-15/GFD-04: hosted AI/story finished; victory sees t before timer advance.
    if (parent_mode_serviced && !impl.child) if (auto served = impl.service_story(); !served) {
        impl.failure = served.error(); return core::Result<GameTick>::failure(*impl.failure);
    }
    if (parent_mode_serviced && !impl.child && impl.world().victory()) impl.world().finish_victory_frame();
    if (parent_mode_serviced && (impl.conflict_services || impl.world().victory()) && !impl.child) impl.world().finish_conflict_frame_timer();
    ++impl.tick; out.completed_tick = impl.tick; out.galaxy = impl.world().snapshot();
    gc_profile::Scope profile_scope(gc_profile::Phase::frame_end_hash_wall);
    if (impl.child) out.battle_seats = impl.child->conflict.seats;
    if (impl.child) {
        const auto& galaxy = impl.child->galaxy_hash;
        world_hash = impl.child->world_hash;
        auto native = impl.native_hash();
        if (!native) { impl.failure = native.error(); return core::Result<GameTick>::failure(*impl.failure); }
        if (impl.hasher) {
            out.state_sha256.clear();
            out.state_hash = impl.hasher->derive(world_hash, [galaxy, script = impl.child->script_hash, context = impl.child_context_hash(), ai = native.value()](const std::string& world) {
                return Impl::with_native(combined_hash(galaxy, world, script, context), ai);
            });
        } else {
            out.state_sha256 = Impl::with_native(combined_hash(galaxy, world_hash.get(), impl.child->script_hash, impl.child_context_hash()), native.value());
            out.state_hash = sim::StateHash(out.state_sha256);
        }
    } else if (out.mode == SessionMode::battle || impl.conflict_services || impl.world().victory() || impl.admission || !impl.waiting_admissions.empty() || impl.conflict_pause || out.result || impl.return_state != TacticalReturnState{}) {
        auto digest = impl.base_hash();
        if (digest) {
            auto native = impl.native_hash();
            if (!native) digest = native;
            else digest = core::Result<std::string>::success(Impl::with_native(digest.value(), native.value()));
        }
        if (!digest) { impl.failure = digest.error(); return core::Result<GameTick>::failure(*impl.failure); }
        out.state_sha256 = digest.value(); out.state_hash = sim::StateHash(out.state_sha256);
    } else if (impl.native_ai.state) {
        // Preserve the world's section cache and the bridge's already composed
        // Lua hash. Native ownership alone must not force a whole-world rehash.
        auto native = impl.native_hash();
        if (!native) { impl.failure = native.error(); return core::Result<GameTick>::failure(*impl.failure); }
        if (impl.hasher) {
            out.state_sha256.clear();
            out.state_hash = impl.hasher->derive(out.state_hash, [ai = std::move(native).value()](const std::string& base) {
                return Impl::with_native(base, ai);
            });
        } else {
            out.state_sha256 = Impl::with_native(out.state_hash.get(), native.value());
            out.state_hash = sim::StateHash(out.state_sha256);
        }
    }
    if (impl.story_engine) { // #683: the plot state joins the session digest (domain eawr-game-story-v1)
        const auto story = impl.story_engine->digest();
        if (impl.hasher) out.state_hash = impl.hasher->derive(out.state_hash, [story](const std::string& base) { return Impl::with_story(base, story); });
        else { out.state_sha256 = Impl::with_story(out.state_hash.get(), story); out.state_hash = sim::StateHash(out.state_sha256); }
    }
    // Capture the scalar inputs by value for asynchronous hashing. No reader
    // can accidentally hash the next completed publication.
    if (impl.interactive_seat.value) {
        const auto observer = impl.interactive_seat;
        const auto conflict = impl.summary_conflict;
        if (impl.hasher) out.state_hash = impl.hasher->derive(out.state_hash, [observer, conflict](const std::string& base) {
            return Impl::with_summary(base, observer, conflict);
        });
        else { out.state_sha256 = Impl::with_summary(out.state_hash.get(), observer, conflict); out.state_hash = sim::StateHash(out.state_sha256); }
    }
    const auto publication = *impl.timelines;
    const bool ai_sync = bool(impl.native_ai.state);
    if (impl.hasher) {
        out.state_sha256.clear();
        out.state_hash = impl.hasher->derive(out.state_hash, [publication, ai_sync](const std::string& base) { return Impl::with_timeline(base, publication, ai_sync); });
    } else {
        out.state_sha256 = Impl::with_timeline(out.state_hash.get(), publication, ai_sync);
        out.state_hash = sim::StateHash(out.state_sha256);
    }
    out.story_notices = std::move(impl.story_notices);
    out.story_triggers = std::move(impl.story_triggers);
    for (auto& note : std::exchange(impl.story_notes, {})) { // the plot loader's report, once (WST-02/04)
        core::Diagnostic line; line.code = "EAWR-STORY-0001"; line.severity = core::Severity::info; line.message = "story: " + std::move(note);
        out.diagnostics.push_back(std::move(line));
    }
    // #4058: an accepted SaveRequest is a history boundary on every peer, whether or not this peer writes the file.
    // A recording that cannot be sealed keeps its base; the capture would refuse it as well.
    if (std::any_of(out.save_events.begin(), out.save_events.end(),
        [](const auto& event) { return event.kind == galactic::GalacticEventKind::save_requested; })) {
        if (auto boundary = history_frontier()) impl.scheduled_base = std::move(boundary).value();
    }
    return core::Result<GameTick>::success(std::move(out));
}

core::Result<GameTick> GameSession::step(const sim::PartitionExecutor& executor) {
    try {
    auto& impl = *impl_;
    gc_profile::Scope game_step_scope(gc_profile::Phase::tick_game_step);
    if (impl.failure) return core::Result<GameTick>::failure(*impl.failure);
    if (impl.tick >= galactic::galactic_max_ticks)
        return fail<GameTick>("session tick limit reached", galactic::diagnostic_codes::simulation_complete);
    impl.rebase_history();
    impl.executor = &executor; // rewards of the plot engine apply inside this step (#683)
    const struct ExecutorScope { GameSession::Impl& owner; ~ExecutorScope() { owner.executor = nullptr; } } executor_scope{impl};
    StepContext context; context.out.mode = mode();
    impl.story_notices.clear();
    if (auto applied = apply_restore_prologue(); !applied) return core::Result<GameTick>::failure(applied.error());
    if (const auto entry = impl.replay_conflicts.find(impl.tick); entry != impl.replay_conflicts.end()) {
        auto content = impl.services(entry->second).content(entry->second);
        if (!content) { impl.failure = content.error(); return core::Result<GameTick>::failure(*impl.failure); }
        auto staged = stage_battle(entry->second, content.value());
        if (!staged) { impl.failure = staged.error(); return core::Result<GameTick>::failure(*impl.failure); }
        impl.replay_conflicts.erase(entry);
    }
    if (auto processed = process_session_commands(context, executor); !processed)
        return core::Result<GameTick>::failure(processed.error());
    if (impl.child) {
        if (auto stepped = step_child(context, executor); !stepped)
            return core::Result<GameTick>::failure(stepped.error());
    } else {
        if (auto stepped = step_galaxy(context, executor); !stepped)
            return core::Result<GameTick>::failure(stepped.error());
    }
    if (auto returned = apply_pending_returns(context, executor); !returned)
        return core::Result<GameTick>::failure(returned.error());
    return finish_step(context);
    } catch (const std::exception& error) {
        auto failed = fail<GameTick>("game session service threw: " + std::string(error.what()), galactic::diagnostic_codes::worker_failure);
        impl_->failure = failed.error();
        return failed;
    } catch (...) {
        auto failed = fail<GameTick>("game session service threw", galactic::diagnostic_codes::worker_failure);
        impl_->failure = failed.error();
        return failed;
    }
}
core::Result<GameSession> GameSession::from_recording(const GameRecording& recording,
    ContentResolver content, ScriptFactory scripts, ScriptRequirement requirement) {
    if (auto valid = galactic::validate_replay(recording.commands); !valid) return core::Result<GameSession>::failure(valid.error());
    auto created = create(recording.commands.setup);
    if (!created) return created;
    auto& impl = *created.value().impl_;
    // A bounded recording starts at its base (#4058): numbered from 1 within the window, after the dropped journal.
    if (recording.base.at.tick > recording.commands.final_tick_count
        || std::any_of(recording.commands.commands.begin(), recording.commands.commands.end(),
            [&](const auto& command) { return command.key.tick < recording.base.at.tick; }))
        return fail<GameSession>("recorded history starts before its base boundary");
    if (auto staged = impl.world().stage_native_replay(recording.commands.native_effects, recording.commands.final_tick_count,
        recording.base.native_dropped); !staged)
        return core::Result<GameSession>::failure(staged.error());
    impl.history = recording.base;
    impl.conflict_services = recording.conflict_services || !recording.admissions.empty();
    std::set<sim::EntityId> ids;
    std::optional<std::uint64_t> previous;
    for (const auto& entry : recording.conflicts) {
        if (!content || entry.entry_tick >= recording.commands.final_tick_count
            || (previous && entry.entry_tick <= *previous) || entry.conflict.id == 0 || !ids.insert(entry.conflict.id).second)
            return fail<GameSession>("recorded conflicts need a resolver, unique IDs and increasing in-range ticks");
        previous = entry.entry_tick;
        impl.replay_conflicts.emplace(entry.entry_tick, entry.conflict);
    }
    previous.reset();
    std::set<sim::EntityId> admitted;
    for (const auto& entry : recording.admissions) {
        if (!content || entry.entry_tick >= recording.commands.final_tick_count
            || (previous && entry.entry_tick < *previous) || !entry.conflict.id
            || !admitted.insert(entry.conflict.id).second
            || static_cast<std::uint8_t>(entry.arbitration.route) > static_cast<std::uint8_t>(ConflictRoute::tactical))
            return fail<GameSession>("invalid recorded conflict admission");
        previous = entry.entry_tick;
        ids.insert(entry.conflict.id);
        impl.replay_admissions.emplace(entry.entry_tick, entry);
    }
    // Selected children are recreated by their recorded choice, not by a second entry.
    std::erase_if(impl.replay_conflicts, [&](const auto& row) {
        if (!admitted.contains(row.second.id)) return false;
        impl.replay_selected.emplace(row.second.id, RecordedConflict{row.first, row.second}); return true;
    });
    impl.world().enable_conflict_requests(impl.conflict_services);
    for (const auto& command : recording.commands.commands) {
        if (const auto* tactical_input = std::get_if<galactic::TacticalLayerCommand>(&command.payload);
            tactical_input && !ids.contains(tactical_input->conflict))
            return fail<GameSession>("recorded tactical command names an absent conflict entry");
        impl.pending.emplace(command.key, command);
        impl.last.insert_or_assign(command.key.seat, std::pair{command.key.tick, command.key.sequence});
    }
    for (auto& layer : impl.layers) {
        layer.services.content = content; layer.services.scripts = scripts;
        layer.services.script_requirement = requirement;
    }
    for (const auto& fixup : recording.load_fixups) {
        if (!std::holds_alternative<galactic::PostLoadFixup>(fixup.payload) || fixup.key.tick > recording.commands.final_tick_count
            || impl.seat(fixup.key.seat) == nullptr
            || galactic::decode_payload(galactic::CommandLayer::session, galactic::session_opcodes::post_load_fixup,
                galactic::encode_payload(fixup.payload)) != std::optional(fixup.payload)
            || (!impl.replay_fixups.empty() && fixup.key.tick < impl.replay_fixups.rbegin()->first))
            return fail<GameSession>("invalid load prologue");
        impl.replay_fixups.emplace(fixup.key.tick, fixup);
    }
    return created;
}
core::Result<std::vector<std::uint8_t>> GameSession::save_checkpoint_state() const {
    using namespace galactic::checkpoint_detail;
    try {
        if (impl_->failure || impl_->restoring_child) throw Invalid("game is not at a healthy save barrier");
        auto world = impl_->world().save_checkpoint_state();
        if (!world) return core::Result<std::vector<std::uint8_t>>::failure(world.error());
        const auto child = [](auto& owner) -> std::optional<ChildOwner> {
            if (!owner) return {};
            auto state = owner->session.world().save_checkpoint_state();
            auto scripts = owner->session.save_scripts();
            if (!state) throw Invalid(state.error().message);
            if (!scripts) throw Invalid(scripts.error().message);
            return ChildOwner{owner->conflict, std::move(state).value(), std::move(scripts).value(),
                owner->session.record(), owner->sequences, owner->killers, owner->owns_pause};
        };
        Writer out; out(std::uint32_t{3}, world.value()); checkpoint_game_owner(out, *impl_);
        out(child(impl_->child), child(impl_->staged));
        std::optional<std::vector<std::uint8_t>> resolver;
        if (auto_resolve_needs_service() && impl_->auto_state_writer) {
            auto saved = impl_->auto_state_writer();
            if (!saved) throw Invalid(saved.error().message);
            resolver = std::move(saved).value();
        } else if (const auto progress = auto_resolve_progress(); progress && progress->initiated) {
            throw Invalid("stateful auto-resolve needs checkpoint services");
        }
        out(resolver);
        // #683, WST-60..62: the plot engine's triggered state and flag table (the plots are rebuilt from data).
        std::optional<galactic::story::State> story;
        if (impl_->story_engine) story = impl_->story_engine->state();
        out(story);
        return core::Result<std::vector<std::uint8_t>>::success(std::move(out.bytes));
    } catch (const Invalid& error) { return fail<std::vector<std::uint8_t>>(error.what()); }
}

core::Result<void> GameSession::load_checkpoint_state(std::span<const std::uint8_t> bytes) {
    using namespace galactic::checkpoint_detail;
    try {
        if (impl_->tick || impl_->failure || impl_->scripts || impl_->child || impl_->staged)
            throw Invalid("direct restore requires a fresh private game");
        Reader in(bytes); std::vector<std::uint8_t> world;
        std::uint32_t version{}; in(version);
        if (version != 3) throw Invalid("unsupported campaign owner format");
        in(world);
        if (auto restored = impl_->world().load_checkpoint_state(world); !restored) return restored;
        const auto recorded_base = impl_->history;
        checkpoint_game_owner(in, *impl_);
        // The recording's window and the owner's counters must agree on where the dropped history ended.
        if (impl_->history != recorded_base || impl_->history.native_dropped != impl_->world().native_dropped())
            throw Invalid("owner history base differs from the recording it restores");
        std::optional<ChildOwner> child, staged; in(child, staged);
        std::optional<std::vector<std::uint8_t>> resolver; in(resolver);
        std::optional<galactic::story::State> story; in(story);
        if (story.has_value() != impl_->story_engine.has_value())
            throw Invalid("saved story plots differ from the loaded campaign");
        if (story) if (auto restored = impl_->story_engine->restore(std::move(*story)); !restored) return restored;
        if (in.remaining() || impl_->tick > galactic::galactic_max_ticks || impl_->world().completed_tick() > impl_->tick
            || (!child && impl_->world().completed_tick() != impl_->tick)
            || child.has_value() != impl_->timelines->child.has_value())
            throw Invalid("inconsistent direct game barrier");
        const auto valid_timeline = [&](const ModeTimelineSnapshot& row, SessionMode mode) {
            return row.mode.mode == mode && row.completed_tick <= impl_->tick && row.value >= 0 && row.value <= 5
                && (mode == SessionMode::galaxy ? row.mode.conflict == 0 : row.mode.conflict != 0);
        };
        if (!valid_timeline(impl_->timelines->galaxy, SessionMode::galaxy)
            || (impl_->timelines->historical && !valid_timeline(*impl_->timelines->historical, SessionMode::battle))
            || (impl_->timelines->child && (!valid_timeline(*impl_->timelines->child, SessionMode::battle)
                || impl_->timelines->child->mode.conflict != child->conflict.id)))
            throw Invalid("invalid direct mode timeline");
        impl_->world().enable_conflict_requests(impl_->conflict_services);
        impl_->timelines->child_slot = impl_->timelines->child
            ? std::make_shared<Impl::ChildTimelinePublication>(Impl::ChildTimelinePublication{*impl_->timelines->child, false}) : nullptr;
        impl_->timelines->historical_slot = impl_->timelines->historical
            ? std::make_shared<Impl::ChildTimelinePublication>(Impl::ChildTimelinePublication{*impl_->timelines->historical, true}) : nullptr;
        const auto restore_child = [&](std::optional<ChildOwner>& saved, std::optional<Impl::Child>& owner) -> core::Result<void> {
            if (!saved) return core::Result<void>::success();
            if (!impl_->services(saved->conflict).content || saved->log.setup != saved->conflict.tactical
                || (owner.has_value()) || saved->scripts.completed_tick > impl_->tick)
                return fail<void>("inconsistent direct child barrier");
            // Parent cursors count attempted envelopes, including refusals. Script
            // keys in the child log use another cursor, and current seat roles need
            // not be their historical roles. Bound the total by recorded attempts.
            std::uint64_t attempts{};
            if (const auto dropped = impl_->history.child_attempts.find(saved->conflict.id); dropped != impl_->history.child_attempts.end())
                attempts = dropped->second;
            for (const auto& command : impl_->executed)
                if (const auto* order = std::get_if<galactic::TacticalLayerCommand>(&command.payload);
                    order && order->conflict == saved->conflict.id) ++attempts;
            for (const auto& [player, sequence] : saved->sequences) {
                if (std::none_of(saved->log.setup.players.begin(), saved->log.setup.players.end(),
                    [&](const auto& row) { return row.player_id == player; }) || sequence == 0 || sequence > attempts)
                    return fail<void>("child command cursors differ from recording");
                attempts -= sequence;
            }
            for (const auto& [id, killer] : saved->killers)
                if (id == 0 || (killer != 0 && std::none_of(saved->log.setup.players.begin(), saved->log.setup.players.end(),
                    [&](const auto& row) { return row.player_id == killer; })))
                    return fail<void>("invalid child death attribution");
            auto content = impl_->services(saved->conflict).content(saved->conflict);
            if (!content) return core::Result<void>::failure(content.error());
            auto child_world = tactical::TacticalSession::from_replay(saved->log, content.value());
            if (!child_world) return core::Result<void>::failure(child_world.error());
            if (auto loaded = child_world.value().load_checkpoint_state(saved->state); !loaded) return loaded;
            if (&owner == &impl_->child) {
                const auto entry = std::find_if(impl_->conflicts.begin(), impl_->conflicts.end(),
                    [&](const auto& row) { return row.conflict.id == saved->conflict.id; });
                // Save-only/paused service may lag the parent, but cannot precede
                // the selected child's birth barrier or its completed local work.
                if (entry == impl_->conflicts.end() || entry->entry_tick >= impl_->tick || !impl_->timelines->child
                    || impl_->timelines->child->completed_tick < entry->entry_tick + 1
                    || impl_->timelines->child->completed_tick < child_world.value().completed_tick()
                    || impl_->timelines->child->completed_tick > impl_->tick)
                    return fail<void>("WGA-378: invalid saved child timeline tick");
            }
            auto schedulers = impl_->scripts_for(saved->conflict);
            if (!schedulers) return core::Result<void>::failure(schedulers.error());
            auto hosted = authoritative::ScriptedTacticalSession::restore(std::move(child_world).value(), std::move(schedulers).value(), saved->scripts);
            if (!hosted) return core::Result<void>::failure(hosted.error());
            if (impl_->services(saved->conflict).configure) if (auto configured = impl_->services(saved->conflict).configure(hosted.value()); !configured) return configured;
            hosted.value().set_state_hasher(impl_->hasher); hosted.value().set_authoritative_hash(true);
            hosted.value().set_shared_mode_service([context = impl_.get()] { return context->update_child_timeline(); });
            auto scripts = hosted.value().script_state_hash();
            auto conflict = conflict_hash(saved->conflict);
            auto galaxy = impl_->galaxy_hash();
            if (!scripts) return core::Result<void>::failure(scripts.error());
            if (!conflict) return core::Result<void>::failure(conflict.error());
            if (!galaxy) return core::Result<void>::failure(galaxy.error());
            const auto hash = sim::StateHash(hosted.value().world().state_sha256());
            owner.emplace(Impl::Child{std::move(saved->conflict), std::move(hosted).value(),
                std::move(saved->sequences), std::move(saved->killers), std::move(galaxy).value(),
                std::move(scripts).value(), std::move(conflict).value(), hash});
            owner->owns_pause = saved->owns_pause;
            return core::Result<void>::success();
        };
        if (auto restored = restore_child(child, impl_->child); !restored) return restored;
        if (auto restored = restore_child(staged, impl_->staged); !restored) return restored;
        if (resolver) {
            if (!impl_->auto_state_reader || !auto_resolve_needs_service()) throw Invalid("missing saved auto-resolve owner");
            if (auto restored = impl_->auto_state_reader(*resolver, impl_->admission->conflict, *impl_->world().snapshot()); !restored) return restored;
        }
        auto canonical = save_checkpoint_state();
        if (!canonical || !std::equal(bytes.begin(), bytes.end(), canonical.value().begin(), canonical.value().end()))
            throw Invalid("noncanonical direct game barrier");
        return core::Result<void>::success();
    } catch (const Invalid& error) { return fail<void>(error.what()); }
}

core::Result<GameRecording> GameSession::checkpoint_recording() const {
    if (impl_->failure) return core::Result<GameRecording>::failure(*impl_->failure);
    auto result = recording();
    for (const auto& [key, command] : impl_->pending) {
        result.commands.commands.push_back(command);
        result.commands.final_tick_count = std::max(result.commands.final_tick_count, key.tick + 1);
    }
    for (const auto& [tick, conflict] : impl_->replay_conflicts) result.conflicts.push_back({tick, conflict});
    for (const auto& [id, selected] : impl_->replay_selected) { (void)id; result.conflicts.push_back(selected); }
    std::sort(result.conflicts.begin(), result.conflicts.end(), [](const auto& a, const auto& b) {
        return std::tie(a.entry_tick, a.conflict.id) < std::tie(b.entry_tick, b.conflict.id);
    });
    if (impl_->staged) result.conflicts.push_back({impl_->tick, impl_->staged->conflict});
    for (const auto& [tick, admission] : impl_->replay_admissions) { (void)tick; result.admissions.push_back(admission); }
    for (const auto& entry : impl_->queued_admissions) result.admissions.push_back(entry);
    for (const auto& entry : result.admissions) result.commands.final_tick_count = std::max(result.commands.final_tick_count, entry.entry_tick + 1);
    for (const auto& entry : result.conflicts) result.commands.final_tick_count = std::max(result.commands.final_tick_count, entry.entry_tick + 1);
    for (const auto& entry : result.commands.setup.outcomes) result.commands.final_tick_count = std::max(result.commands.final_tick_count, entry.completed_tick);
    for (const auto& [tick, command] : impl_->replay_fixups) result.load_fixups.push_back(command);
    return core::Result<GameRecording>::success(std::move(result));
}
const HistoryBase& GameSession::history_base() const noexcept { return impl_->history; }
core::Result<HistoryBoundary> history_frontier_of(const GameRecording& recording,
    std::span<const std::uint8_t> replay_bytes, const std::uint64_t completed_tick) {
    HistoryBoundary out; out.tick = completed_tick; out.last_key = recording.base.at.last_key;
    std::vector<std::uint8_t> chained(recording.base.at.digest.begin(), recording.base.at.digest.end());
    chained.insert(chained.end(), replay_bytes.begin(), replay_bytes.end());
    out.digest = core::sha256(chained);
    if (!recording.commands.commands.empty()) {
        const auto key = recording.commands.commands.back().key;
        if (!out.last_key || *out.last_key < key) out.last_key = key;
    }
    return core::Result<HistoryBoundary>::success(std::move(out));
}
core::Result<HistoryBoundary> GameSession::history_frontier() const {
    auto recorded = checkpoint_recording();
    if (!recorded) return core::Result<HistoryBoundary>::failure(recorded.error());
    auto replay = galactic::write_replay(recorded.value().commands);
    if (!replay) return core::Result<HistoryBoundary>::failure(replay.error());
    return history_frontier_of(recorded.value(), replay.value(), impl_->tick);
}
void GameSession::schedule_history_rebase(HistoryBoundary boundary) { impl_->scheduled_base = std::move(boundary); }
core::Result<void> GameSession::append_recording_window(const GameRecording& window) {
    auto& impl = *impl_;
    if (impl.failure) return core::Result<void>::failure(*impl.failure);
    impl.rebase_history();
    if (window.base != impl.history || impl.history.at.tick != impl.tick || impl.child || impl.staged)
        return fail<void>("recorded window does not start at this session's history boundary", galactic::diagnostic_codes::replay_malformed);
    const auto from = impl.tick;
    if (auto valid = galactic::validate_replay(window.commands); !valid) return valid;
    if (window.commands.final_tick_count < from) return fail<void>("recorded window ends before its boundary");
    std::set<sim::EntityId> ids;
    for (const auto& entry : window.conflicts) ids.insert(entry.conflict.id);
    for (const auto& entry : window.admissions) ids.insert(entry.conflict.id);
    for (const auto& command : window.commands.commands) {
        if (command.key.tick < from) return fail<void>("recorded window starts before its boundary");
        if (impl.pending.contains(command.key)) continue;
        if (const auto* tactical_input = std::get_if<galactic::TacticalLayerCommand>(&command.payload);
            tactical_input && !ids.contains(tactical_input->conflict))
            return fail<void>("recorded tactical command names an absent conflict entry");
        impl.pending.emplace(command.key, command);
        impl.last.insert_or_assign(command.key.seat, std::pair{command.key.tick, command.key.sequence});
    }
    std::set<sim::EntityId> known;
    for (const auto& [tick, conflict] : impl.replay_conflicts) known.insert(conflict.id);
    for (const auto& [id, selected] : impl.replay_selected) known.insert(id);
    for (const auto& [tick, admission] : impl.replay_admissions) known.insert(admission.conflict.id);
    for (const auto& entry : impl.queued_admissions) known.insert(entry.conflict.id);
    for (const auto& entry : window.conflicts) {
        if (entry.entry_tick < from || known.contains(entry.conflict.id)) continue;
        if (entry.entry_tick >= window.commands.final_tick_count || !entry.conflict.id)
            return fail<void>("recorded conflict is outside its window");
        impl.replay_conflicts.emplace(entry.entry_tick, entry.conflict);
    }
    // Every admitted conflict, including one admitted before the boundary, is later selected by its recorded choice.
    std::set<sim::EntityId> admitted;
    for (const auto& entry : window.admissions) {
        if (!entry.conflict.id || !admitted.insert(entry.conflict.id).second
            || static_cast<std::uint8_t>(entry.arbitration.route) > static_cast<std::uint8_t>(ConflictRoute::tactical))
            return fail<void>("invalid recorded conflict admission");
        if (entry.entry_tick < from || known.contains(entry.conflict.id)) continue;
        if (entry.entry_tick >= window.commands.final_tick_count) return fail<void>("recorded conflict admission is outside its window");
        impl.replay_admissions.emplace(entry.entry_tick, entry);
    }
    std::erase_if(impl.replay_conflicts, [&](const auto& row) {
        if (!admitted.contains(row.second.id)) return false;
        impl.replay_selected.emplace(row.second.id, RecordedConflict{row.first, row.second}); return true;
    });
    impl.conflict_services = impl.conflict_services || window.conflict_services || !window.admissions.empty();
    impl.world().enable_conflict_requests(impl.conflict_services);
    for (const auto& fixup : window.load_fixups) {
        if (fixup.key.tick < from) continue;
        const auto staged = impl.replay_fixups.equal_range(fixup.key.tick);
        const bool held = std::any_of(staged.first, staged.second, [&](const auto& row) { return row.second == fixup; })
            || std::find(impl.load_fixups.begin(), impl.load_fixups.end(), fixup) != impl.load_fixups.end();
        if (held) continue;
        if (!std::holds_alternative<galactic::PostLoadFixup>(fixup.payload) || fixup.key.tick > window.commands.final_tick_count
            || impl.seat(fixup.key.seat) == nullptr
            || (!impl.replay_fixups.empty() && fixup.key.tick < impl.replay_fixups.rbegin()->first))
            return fail<void>("invalid load prologue");
        impl.replay_fixups.emplace(fixup.key.tick, fixup);
    }
    return impl.world().stage_native_replay(window.commands.native_effects, window.commands.final_tick_count, impl.history.native_dropped);
}
void GameSession::Impl::rebase_history() {
    if (!scheduled_base) return;
    const auto boundary = *scheduled_base; scheduled_base.reset();
    // An unfinished conflict keeps the count of its tactical envelopes (the child cursor bound reads it on restore).
    for (const auto& command : executed)
        if (const auto* order = std::get_if<galactic::TacticalLayerCommand>(&command.payload); order && !ended.contains(order->conflict))
            ++history.child_attempts[order->conflict];
    std::erase_if(history.child_attempts, [&](const auto& row) { return ended.contains(row.first); });
    history.executed_dropped += executed.size(); executed.clear();
    world().rebase_history(); history.native_dropped = world().native_dropped();
    history.at = boundary;
    std::erase_if(load_fixups, [&](const auto& command) { return command.key.tick < boundary.tick; });
    // A finished conflict's selection is history; the auto-resolve return still reads its admission until applied.
    const auto finished = [&](const auto& entry) {
        const auto id = entry.conflict.id;
        return entry.entry_tick < boundary.tick && ended.contains(id)
            && std::none_of(world().pending_outcomes().begin(), world().pending_outcomes().end(),
                [&](const auto& outcome) { return outcome.conflict == id; });
    };
    std::erase_if(conflicts, finished); std::erase_if(admissions, finished);
}
core::Result<void> GameSession::apply_load_fixup(const galactic::GameCommand& command) {
    if (command.key.tick != impl_->tick || !std::holds_alternative<galactic::PostLoadFixup>(command.payload))
        return fail<void>("load prologue needs the current session frontier");
    const std::array controls{command};
    // WGUI-102 / TR-01: active-mode frames and synchronized logical FPS, never
    // the retained parent's timer or either mode's wall-clock speed setting.
    const auto observer_clock = impl_->child
        ? std::optional(std::pair{impl_->child->session.world().completed_tick(), tactical::logical_frames_per_second})
        : std::nullopt;
    auto applied = impl_->world().apply_session_controls(controls, false, false, observer_clock);
    if (!applied) return applied;
    // WGS player-load normalization may clear depth, but an open summary still owns its hold.
    if (impl_->summary_conflict && !impl_->world().clock().pause_depth) {
        if (auto held = impl_->world().adjust_conflict_pause(true); !held) return held;
    }
    if (impl_->child) impl_->child->galaxy_hash = impl_->galaxy_hash().value();
    impl_->load_fixups.push_back(command);
    return core::Result<void>::success();
}
core::Result<void> GameSession::apply_restore_prologue() {
    while (!impl_->replay_fixups.empty() && impl_->replay_fixups.begin()->first == impl_->tick) {
        const auto command = impl_->replay_fixups.begin()->second;
        auto applied = apply_load_fixup(command);
        if (!applied) return applied;
        impl_->replay_fixups.erase(impl_->replay_fixups.begin());
    }
    return core::Result<void>::success();
}
core::Result<std::optional<BattleCheckpoint>> GameSession::save_battle_checkpoint() const {
    using Save = std::optional<BattleCheckpoint>;
    if (impl_->failure) return core::Result<Save>::failure(*impl_->failure);
    if (!impl_->child) return core::Result<Save>::success({});
    auto state = impl_->child->session.world().save_checkpoint_state();
    if (!state) return core::Result<Save>::failure(state.error());
    auto scripts = impl_->child->session.save_scripts();
    if (!scripts) return core::Result<Save>::failure(scripts.error());
    return core::Result<Save>::success(BattleCheckpoint{std::move(state).value(), std::move(scripts).value(),
        impl_->child->sequences, impl_->child->killers, impl_->timelines->child->value,
        impl_->timelines->child->completed_tick});
}
core::Result<void> GameSession::restore_battle_checkpoint(const BattleCheckpoint& saved,
    const tactical::TacticalReplay& log, const std::uint64_t completed,
    const sim::PartitionExecutor& executor) {
    if (saved.timeline < 0 || saved.timeline > 5) return fail<void>("WGA-378: invalid saved child timeline");
    if (!impl_->child || impl_->child->session.completed_tick() != 0 || completed < impl_->tick
        || completed > galactic::galactic_max_ticks
        || completed - impl_->tick < saved.scripts.completed_tick || log.setup != impl_->child->conflict.tactical)
        return fail<void>("child checkpoint does not match its entry and shared barrier");
    // The timeline's serviced tick lags the parent across save-only and paused barriers: restore it, never derive it.
    if (!impl_->timelines->child || saved.timeline_tick < impl_->timelines->child->completed_tick || saved.timeline_tick > completed)
        return fail<void>("WGA-378: invalid saved child timeline tick");
    // Restore before child scheduler/native configuration can evaluate a token.
    impl_->timelines->child = ModeTimelineSnapshot{{SessionMode::battle, impl_->child->conflict.id}, saved.timeline_tick, saved.timeline};
    impl_->timelines->child_slot = std::make_shared<Impl::ChildTimelinePublication>(Impl::ChildTimelinePublication{*impl_->timelines->child, false});
    auto content = impl_->services(impl_->child->conflict).content(impl_->child->conflict);
    if (!content) return core::Result<void>::failure(content.error());
    auto world = tactical::TacticalSession::from_replay(log, content.value());
    if (!world) return core::Result<void>::failure(world.error());
    if (auto loaded = world.value().load_checkpoint_state(saved.state); !loaded) return loaded;
    if (world.value().completed_tick() != saved.scripts.completed_tick)
        return fail<void>("child state and host clocks differ");
    auto scripts = impl_->scripts_for(impl_->child->conflict);
    if (!scripts) return core::Result<void>::failure(scripts.error());
    auto hosted = authoritative::ScriptedTacticalSession::restore(std::move(world).value(), std::move(scripts).value(), saved.scripts);
    if (!hosted) return core::Result<void>::failure(hosted.error());
    if (impl_->services(impl_->child->conflict).configure) {
        if (auto configured = impl_->services(impl_->child->conflict).configure(hosted.value()); !configured) return configured;
    }
    hosted.value().set_state_hasher(impl_->hasher);
    hosted.value().set_authoritative_hash(true);
    auto script_hash = hosted.value().script_state_hash();
    if (!script_hash) return core::Result<void>::failure(script_hash.error());
    // WGS-44/62/65/66: keep the parent and its controls, restore the child's barrier,
    // and retain attribution/cursors for the eventual ordinary return route.
    impl_->restoring_child = true;
    while (impl_->tick < completed) {
        auto stepped = step(executor);
        if (!stepped) { impl_->restoring_child = false; return core::Result<void>::failure(stepped.error()); }
    }
    impl_->restoring_child = false;
    if (impl_->child->sequences != saved.sequences) return fail<void>("child command cursors differ from recording");
    for (const auto& [id, killer] : saved.killers) {
        if (id == 0 || (killer != 0 && std::none_of(log.setup.players.begin(), log.setup.players.end(),
            [&](const auto& row) { return row.player_id == killer; }))) return fail<void>("invalid child death attribution");
    }
    hosted.value().set_shared_mode_service([owner = impl_.get()] { return owner->update_child_timeline(); });
    impl_->child->session = std::move(hosted).value();
    impl_->timelines->child = ModeTimelineSnapshot{{SessionMode::battle, impl_->child->conflict.id}, saved.timeline_tick, saved.timeline};
    impl_->timelines->child_slot->snapshot = *impl_->timelines->child;
    impl_->child->killers = saved.killers;
    impl_->child->script_hash = std::move(script_hash).value();
    impl_->child->world_hash = sim::StateHash(impl_->child->session.world().state_sha256());
    return core::Result<void>::success();
}
} // namespace eawr::campaign
