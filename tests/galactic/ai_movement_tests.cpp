#include "galactic_support.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_movement_runtime.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/auto_resolve_live.hpp"
#include "eawr/campaign/conflict_policy.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/control.hpp"
#include "eawr/campaign/invasion_offer.hpp"
#include "eawr/campaign/tactical_return.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace eawr::script::foc::ai {
struct EngineContractAccess {
    static void movement_fixture(Engine& engine, const GoalType& definition) {
        PlayerAi player; player.player = 1; player.galactic = true;
        Goal goal; goal.id = 7; goal.function = 0; player.active.push_back(goal);
        GoalFunctionEntry function; function.goal = &definition; player.functions.push_back(function);
        engine.players_.push_back(std::move(player));
        Plan plan; plan.goal = 7; plan.id = galactic_id_base(0) | 7; plan.player = 1; plan.instance = galactic_instance_base(0); plan.taskforces = {galactic_id_base(0) | 8};
        engine.players_.back().running.emplace(plan.id, plan); engine.players_.back().plan_of_instance.emplace(plan.instance, plan.id);
        TaskForce force; force.id = galactic_id_base(0) | 8; force.plan = plan.id; force.units = {20, 21, 22}; force.galactic_fleet = 10;
        engine.players_.back().taskforces.emplace(force.id, std::move(force));
        auto& retained = engine.players_.back(); retained.slot = 0; retained.next_plan_local = 8; retained.next_taskforce_local = 9; retained.next_instance_local = 1;

        engine.rebuild_runtime_index();
    }
    static void blocks(Engine& engine) { engine.service_blocks(); }
};
}

namespace {
namespace gc = eawr::sim::galactic;
namespace ga = eawr::script::foc::galactic_ai;
namespace ai = eawr::script::foc::ai;
namespace cp = eawr::campaign;
using galactic_test::expect;
using eawr::sim::math::Fixed;
Fixed f(std::int64_t value) { return Fixed::from_integer(value).value(); }
template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
    return std::move(result).value();
}
void done(eawr::core::Result<void> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
}
gc::GalacticSetup fixture() {
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{gc::SeatId{1}, 1, gc::SeatKind::ai, 0, true, 0}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 1}, {3, {f(24), {}, {}}, 1}};
    s.rules.maximum_fleet_movement_distance = f(12); s.rules.fiscal_cycle_frames = 100;
    s.fleets = {{10, 1, 1, {{20, 1}, {21, 2}, {22, 2}}}, {11, 1, 1, {{23, 1}}}};
    s.fleet_control.emplace(); s.fleet_control->types = {{1, false, true, false}, {2, true, true, false}};
    s.movement.emplace(); auto& m = *s.movement;
    m.mode_modifier = f(1); m.types = {{1, f(1), f(100), 0, true, false, false, false}, {2, f(1), f(50), 0, true, false, false, false, true}};
    m.players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}, {3, f(1), f(1), false}};
    m.planets = {{1, f(1), false}, {2, f(1), false}, {3, f(1), false}};
    m.contents = {{21, {}}, {22, {}}};
    for (const auto& p : s.players) for (const auto& q : s.planets) m.access.push_back({p.player_id, q.id, true, false});
    s.control.emplace(); auto& c = *s.control; c.change_seconds = f(7); c.max_ground_forces = 2;
    c.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : c.players) for (const auto& b : c.players) c.relations.push_back({a.player, b.player, a.player == b.player});
    c.types = {{1, 1, false, false}, {2, 0, false, true}};
    for (const auto& p : s.planets) {
        gc::ControlPlanet row; row.planet = p.id; row.type = 100 + p.id; row.surface_accessible = true; row.ground_base = {1, 1};
        c.planets.push_back(row);
    }
    return s;
}
// Fixture/stock harness supplies ordinary-world inputs from live control. The
// qualified TaskForce AI-planet classifier remains a runtime adapter seam;
// the installed proof deliberately exercises the generic native command.
ga::NativeActions::MovementInput copied(gc::PlayerId player, const gc::GalacticSnapshot& snapshot) {
    ga::NativeActions::MovementInput input; input.ai_player = true;
    const auto* control = snapshot.control();
    if (!control || !snapshot.movement()) throw std::runtime_error("movement control inputs missing");
    const auto allied = [&](gc::PlayerId other) {
        return std::any_of(control->relations.begin(), control->relations.end(), [&](const auto& r) { return r.player == player && r.other == other && r.allied; });
    };
    const auto neutral = [&](gc::PlayerId other) {
        return std::any_of(control->players.begin(), control->players.end(), [&](const auto& r) { return r.player == other && r.neutral; });
    };
    for (const auto& p : snapshot.planets()) {
        gc::PathPlanet row; row.id = p.id;
        row.relation = allied(p.owner) ? gc::PathRelation::allied : neutral(p.owner) ? gc::PathRelation::neutral : gc::PathRelation::hostile;
        row.ai_classification_succeeded = false; // WGM-19's explicit unavailable-classifier input
        for (const auto& q : control->planets) if (q.planet == p.id && q.starbase.level)
            row.starbase_relation = allied(q.starbase.owner) ? gc::PathRelation::allied : gc::PathRelation::hostile;
        for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == p.id) {
            row.fleet_presence_visible = true; row.nonallied_fleet = row.nonallied_fleet || !allied(fleet.owner);
        }
        // Harness observations are complete; absent defense is an explicit
        // observation, not the conservative unavailable-AI-perception default.
        row.ai_enemy_space_force = row.nonallied_fleet;
        for (const auto& q : snapshot.movement()->planets) if (q.planet == p.id) row.corrupted = q.corrupted;
        input.access.push_back(row); input.orbital_occupants[p.id] = 0;
    }
    for (const auto& state : control->state) input.orbital_occupants[state.planet] = state.orbital_player;
    for (const auto& p : snapshot.movement()->players) if (p.player == player) input.benefits_from_corruption = p.benefits_from_corruption;
    input.multiplayer = std::count_if(snapshot.seats().begin(), snapshot.seats().end(), [](const auto& seat) { return seat.kind == gc::SeatKind::human && seat.active; }) > 1;
    return input;
}
void bind(ga::NativeActions& actions, gc::GalacticSession& world, bool& pending, std::uint64_t& searches) {
    actions.bind_admission_gate([] { return false; });
    actions.bind_movement([&](gc::PlayerId player, const gc::GalacticSnapshot& snapshot) {
        ++searches; return eawr::core::Result<ga::NativeActions::MovementInput>::success(copied(player, snapshot));
    }, [&] { return pending; }, [&](auto player, auto actor) { return world.recheck_native_orbit(player, actor); });
}
void shared_movement_block() {
    auto world = take(gc::GalacticSession::create(fixture()));
    auto actions = std::make_shared<ga::NativeActions>(world, std::make_shared<ai::GalacticFunding>(), ga::NativeActions::Placement{}, std::map<gc::PlayerId, Fixed>{});
    bool pending{}; std::uint64_t searches{}; bind(*actions, world, pending, searches);
    auto host = std::make_shared<eawr::script::foc::detail::Host>();
    host->view = std::make_shared<eawr::script::foc::detail::WorldView>(); host->galactic_execution = actions;
    auto engine = std::make_shared<ai::Engine>(host, ai::AiData{}, std::vector<ai::PlanDef>{}); host->engine = engine.get();
    ai::GoalType definition; definition.reachability = "FRIENDLY_ONLY";
    ai::EngineContractAccess::movement_fixture(*engine, definition);
    namespace auth = eawr::script::authoritative;
    const auto issue = [&](std::uint64_t sequence, std::string operation, auth::Value argument) {
        auth::ServiceReport report; auth::ScriptCommand command;
        command.issuer = ai::first_plan_instance; command.sequence = sequence; command.verb = ai::verb_ai;
        command.arguments = {auth::Value{std::move(operation)}, auth::Value{auth::Handle{ai::handle_taskforce, ai::galactic_id_base(0) | 8}}, std::move(argument)};
        report.commands.push_back(std::move(command)); done(engine->after_service(report));
    };
    issue(1, "galactic_move", auth::Value{auth::Handle{eawr::script::foc::handle_game_object, 3}});
    const auto id = ai::block_id(ai::first_plan_instance, 1); const auto* block = engine->block(id);
    expect(block && block->galactic_request && !block->finished && block->galactic_actor == 10 && world.snapshot()->fleets().front().motion.transit,
        "real shared Engine block retains native request/actor and separates admission from movement completion");
    ga::bind_movement_references(*actions, engine);
    const auto saved = take(actions->capture()); done(actions->restore(saved));
    expect(ga::movement_subscribed(*actions, *engine, block->galactic_request), "restored native request binds exact retained shared block");
    auto missing = saved; auto& bytes = missing.components.at("galactic-production");
    // Codec 6's counter and empty qualified-freestore frame occupy 40 trailing bytes.
    bytes.erase(bytes.end() - 40 - 128, bytes.end() - 40);
    std::fill(bytes.end() - 40 - 8, bytes.end() - 40, std::uint8_t{});
    expect(!actions->restore(missing), "restore refuses a shared block with no saved native request");
    const auto before = searches; ai::EngineContractAccess::blocks(*engine);
    expect(searches == before && !engine->block(id)->finished, "shared block service observes actual native state without autonomous path advancement");
    done(actions->movement_conflict(block->galactic_request, 2, true));
    pending = true;
    ai::EngineContractAccess::blocks(*engine);
    expect(!engine->block(id)->finished, "battle begin clears completion independently of retained block observation");
}
void movement_associations() {
    auto world = take(gc::GalacticSession::create(fixture()));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    bool pending{}; std::uint64_t searches{}; bind(actions, world, pending, searches);
    ga::ActionRequest request; request.kind = ga::ActionKind::move; request.player = 1; request.object = 10; request.target = 3;
    request.plan = 7; request.taskforce = 8; request.block = 9; request.members = {20, 21, 22};
    const auto revision = actions.state_revision(); const auto admitted = take(actions.admit(request));
    expect(actions.state_revision() != revision, "movement admission invalidates retained native digest revision");
    actions.bind_movement_references([&](std::uint64_t id, const ga::NativeActions::Movement& row) {
        if (id == admitted.request && row.plan_id == 7 && row.taskforce_id == 8 && row.block_id == 9)
            return eawr::core::Result<void>::success();
        eawr::core::Diagnostic error; error.code = "test-association"; error.message = "wrong reconstructed consumer";
        return eawr::core::Result<void>::failure(std::move(error));
    });
    const auto saved = take(actions.capture()); done(actions.restore(saved));
    const auto recaptured = take(actions.capture());
    expect(recaptured.version == saved.version && recaptured.components == saved.components, "associated movement exact recapture preserves consumer IDs and registration");
    auto malformed = saved; auto& bytes = malformed.components.at("galactic-production");
    bytes[bytes.size() - 40 - 128 + 88] ^= 1;
    expect(!actions.restore(malformed), "malformed restored movement association refuses");
    const auto unchanged = take(actions.capture());
    expect(unchanged.version == saved.version && unchanged.components == saved.components, "malformed restored movement association refuses before publication");
    done(actions.retire_movement(admitted.request));
    expect(!actions.movements().at(admitted.request).registered
        && !actions.movement_conflict(admitted.request, 2, true), "retired movement subscription cannot reopen on battle begin");
}
void taskforce_containment() {
    auto world = take(gc::GalacticSession::create(fixture()));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    actions.bind_admission_gate([] { return false; });
    ga::ActionRequest request; request.player = 1; request.taskforce = 4; request.kind = ga::ActionKind::regroup;
    request.members = {20, 23, 23};
    const auto formed = take(actions.admit(request));
    expect(formed.admitted && formed.request == 10 && world.snapshot()->fleets().size() == 1,
        "TaskForce formation retains first actual member fleet and merges each distinct other fleet once");
    expect(actions.containing_fleet(1, 23) == 10 && !actions.containing_fleet(2, 23), "formation lookup preserves actual owner and containment");
    request.kind = ga::ActionKind::land; request.object = 10; (void)take(actions.admit(request));
    request.kind = ga::ActionKind::launch; request.members = {20, 21, 22};
    const auto launched = take(actions.admit(request));
    const auto first = actions.containing_fleet(1, 21), second = actions.containing_fleet(1, 22);
    expect(launched.admitted && first && second && *first != 10 && *second != 10 && first != second
        && launched.request == *second && world.snapshot()->control()->planets.front().landed.empty(),
        "WGA-486 per-parent launches retain their own fleets and return the last actual fleet without reusing the retained fleet");
}
std::uint64_t section_size(const gc::GalacticSession& world, std::uint16_t wanted) {
    const auto bytes = world.canonical_state_bytes(); std::size_t offset = 12;
    const auto integer = [&](unsigned width) {
        std::uint64_t value{};
        for (unsigned index = 0; index < width; ++index) value |= std::uint64_t(bytes.at(offset++)) << (index * 8);
        return value;
    };
    const auto count = integer(4);
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto id = integer(2); (void)integer(2); const auto size = integer(8);
        if (id == wanted) return size;
        if (size > bytes.size() - offset) throw std::runtime_error("invalid canonical state section framing");
        offset += static_cast<std::size_t>(size);
    }
    throw std::runtime_error("canonical state section absent");
}
cp::GameSession::NativeConflictService fixture_invasion_provider() {
    return [](const gc::GalacticSnapshot& snapshot, const gc::GalacticSession::NativeInvasion& request, eawr::sim::EntityId first_free)
        -> eawr::core::Result<std::optional<cp::RecordedAdmission>> {
        cp::ForceQualifier qualify = [&](const gc::FleetMember& member, gc::PlayerId owner, eawr::sim::EntityId parent)
            -> eawr::core::Result<cp::AdmissionForce> {
            cp::AdmissionForce row; row.force = {member.id, member.type, owner, parent};
            row.ground_capable = take(gc::has_ground_capability(*snapshot.control(), member, snapshot.movement()));
            row.transport = member.type == 2;
            row.deleted = std::binary_search(snapshot.control()->absent_forces.begin(), snapshot.control()->absent_forces.end(), member.id);
            return eawr::core::Result<cp::AdmissionForce>::success(row);
        };
        const gc::GalacticEvent event{snapshot.completed_tick(), gc::GalacticEventKind::land_conflict_requested, {}, request.fleet, request.request, request.planet};
        const auto presence = take(cp::collect_conflict_presence(snapshot, event, qualify, [&](const gc::Fleet& fleet) {
            cp::OuterFleetFacts facts; facts.fleet_behavior = true;
            for (const auto& member : fleet.members) facts.ground_capable |= take(qualify(member, fleet.owner, fleet.id)).ground_capable;
            return eawr::core::Result<cp::OuterFleetFacts>::success(facts);
        }, &request));
        if (!cp::admits_land_conflict(presence)) return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success({});
        cp::ConflictRecord record; record.id = first_free; record.planet = request.planet; record.layer = cp::BattleLayer::land;
        record.invader = request.invader; record.defender = request.defender; record.forces = cp::live_conflict_force_manifest(record, presence);
        record.tactical.first_free_entity_id = first_free + 1;
        record.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
        return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success(cp::RecordedAdmission{snapshot.completed_tick(), std::move(record), {cp::ConflictRoute::pending_tactical, false, false}});
    };
}
void invasion_queue_disk() {
    for (const bool defended : {false, true}) {
        std::string baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            auto setup = fixture(); setup.planets.front().owner = 2;
            setup.control->planets.front().ground_base = defended ? gc::ControlBase{2, 1} : gc::ControlBase{};
            setup.control->ground_capability_types = {{1, f(1), false, false}, {2, {}, true, false}};
            setup.movement->contents = {{21, {{30, 1}}}, {22, {{31, 1}}}};
            auto session = take(cp::GameSession::create(setup)); gc::GalacticSession* world{};
            unsigned provider_calls{};
            const auto configure = [&](cp::GameSession& game) {
                game.set_conflict_service({}, [](const cp::ConflictRecord&) { return eawr::core::Result<eawr::sim::tactical::TacticalContent>::success({}); }, {}, eawr::campaign::ScriptRequirement::none);
                game.set_native_conflict_service([&, source = fixture_invasion_provider()](const auto& snapshot, const auto& request, auto first_free) {
                    ++provider_calls; return source(snapshot, request, first_free);
                }); game.set_conflict_gate([](const auto&) { return false; });
                return game.configure_native_replay_owners([&](auto& owner) { world = &owner; return eawr::core::Result<void>::success(); });
            };
            done(configure(session)); world->set_hash_verification(true);
            const auto original = world->state_sha256();
            expect(!take(world->admit_native_invasion(2, 10)).admitted && world->state_sha256() == original, "invasion shares actual player ownership validation without mutation");
            const auto admitted = take(world->admit_native_invasion(1, 10));
            const auto* request = world->native_invasion(admitted.request);
            expect(admitted.admitted && admitted.queued && request && request->transports == std::vector<gc::FleetMember>{{21, 2}, {22, 2}}
                && request->state == gc::GalacticSession::NativeInvasion::State::pending, "invasion retains direct transports in fleet order before any completion");
            expect(take(world->admit_native_fleet(1, gc::MergeFleets{10, 11})).admitted,
                "queued invasion retains explicit transport identities after its original fleet is merged away");
            const auto build = eawr::checkpoint::game_identities(setup);
            const auto queued_bytes = take(eawr::checkpoint::save_game_checkpoint(session, build));
            eawr::checkpoint::GameLoadOptions load; load.script_requirement = eawr::campaign::ScriptRequirement::none; load.configure_session = configure;
            load.content = [](const cp::ConflictRecord&) { return eawr::core::Result<eawr::sim::tactical::TacticalContent>::success({}); };
            auto restored = take(eawr::checkpoint::load_game_checkpoint(queued_bytes, build, load, executor));
            expect(restored.session.state_sha256() == session.state_sha256(), "disk load restores an admitted native invasion before its first queue service");
            std::ostringstream trace;
            for (unsigned frame = 0; frame < 3; ++frame) {
                (void)take(session.step(executor)); (void)take(restored.session.step(executor));
                expect(session.state_sha256() == restored.session.state_sha256(), "native invasion queue continuation equals disk reconstruction");
                trace << session.state_sha256();
            }
            if (defended) {
                expect(session.pending_conflict() && session.pending_conflict()->layer == cp::BattleLayer::land,
                    "capable defended native invasion reaches the existing campaign conflict queue");
                expect(session.pending_conflict()->id < session.galaxy().next_entity_id()
                    && restored.session.pending_conflict()->id < restored.session.galaxy().next_entity_id(),
                    "live and continued queue dispatch use reserved conflicts below the real next entity id");
            } else {
                const auto& landed = session.galaxy().snapshot()->control()->planets.front().landed;
                expect(landed.size() == 2 && landed[0].id == 21 && landed[1].id == 22 && !session.pending_conflict(),
                    "undefended completion lands original transports in explicit order without a battle");
                const auto* completed = world->native_invasion(admitted.request);
                expect(completed && completed->state == gc::GalacticSession::NativeInvasion::State::completed && completed->success,
                    "matching undefended action completion is independent of tactical results");
            }
            const auto completed_bytes = take(eawr::checkpoint::save_game_checkpoint(session, build));
            const auto calls_before_replay = provider_calls;
            auto after = take(eawr::checkpoint::load_game_checkpoint(completed_bytes, build, load, executor));
            expect(after.session.state_sha256() == session.state_sha256(), "disk restore reproduces actual invasion dispatch/completion, including section18");
            expect(provider_calls == calls_before_replay, "recorded native dispatch reconstructs without running the capability/admission provider");
            if (defended) {
                const auto admissions = after.session.recording().admissions;
                expect(after.session.pending_conflict() && admissions.size() == 1
                    && admissions.front().conflict.id == after.session.pending_conflict()->id
                    && after.session.pending_conflict()->id < after.session.galaxy().next_entity_id(),
                    "replayed native dispatch references the real replayed campaign admission and its reserved allocator id");
            }
            if (baseline.empty()) baseline = trace.str(); else expect(baseline == trace.str(), "native invasion queue and real completion equality on 1/2/4/8");
        }
    }
}
void invasion_retention() {
    std::string summary_baseline;
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto setup = fixture(); setup.seats.front().kind = gc::SeatKind::human; setup.seats.front().human_side_id = 1;
        setup.planets.front().owner = 2; setup.control->planets.front().ground_base = {};
        setup.fleets.back().members = {{23, 2}};
        setup.movement->contents = {{21, {{40, 1}}}, {22, {{41, 1}}}, {23, {{42, 1}}}};
        auto game = take(cp::GameSession::create(setup));
        gc::GalacticSession* owner{};
        const auto configure_summary = [&](cp::GameSession& current) {
            current.set_native_conflict_service(fixture_invasion_provider());
            return current.configure_native_replay_owners([&](auto& world) { owner = &world; return eawr::core::Result<void>::success(); });
        };
        done(configure_summary(game));
        cp::ConflictRecord context; context.id = 90; context.planet = 1; context.invader = 1; context.defender = 2;
        context.tactical.first_free_entity_id = 100;
        context.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
        cp::BattleResult result; result.conflict = 90; result.winner = 1;
        result.invasion_offers = cp::tactical_invasion_offers(context, result, *owner->snapshot());
        expect(result.invasion_offers.size() == 2 && result.invasion_offers.front().transports == std::vector<eawr::sim::EntityId>{21, 22},
            "space return prepares live cargo transports in fleet traversal order");
        const auto observed = cp::observe_invasion_offer(result, context, *owner->snapshot(), 1, true);
        expect(observed.accept_permitted && !observed.visible_admitted && observed.candidates.size() == 2 && observed.cargo.size() == 3,
            "prepared native selection preserves the visible-offer qualification seam");
        expect(cp::observe_invasion_offer(result, context, *owner->snapshot(), 2, true).candidates.empty(),
            "opponent observer receives no winner cargo selection");
        result.invasion_offers.front().transports = {21, 999};
        done(game.restore_latest_tactical_result(context, result));
        expect(!game.accept_invasion_offer({2}, 90) && !game.accept_invasion_offer({1}, 91),
            "opponent and stale result accept intents do not mutate the queue");
        done(game.accept_invasion_offer({1}, 90));
        const auto* request = owner->pending_native_invasion();
        expect(request && request->transports == std::vector<gc::FleetMember>{{21, 2}} && request->reader_released,
            "accept skips missing retained IDs and excludes unselected transports");
        const auto* next = request ? owner->native_invasion(request->request + 1) : nullptr;
        expect(next && next->fleet == 11 && next->transports == std::vector<gc::FleetMember>{{23, 2}},
            "accept queues transports from every prepared fleet rather than only the first fleet");
        expect(!game.accept_invasion_offer({1}, 90), "accepted selection cannot enqueue twice");
        const auto build = eawr::checkpoint::game_identities(setup);
        eawr::checkpoint::GameLoadOptions load; load.script_requirement = eawr::campaign::ScriptRequirement::none; load.configure_session = configure_summary;
        auto restored = take(eawr::checkpoint::load_game_checkpoint(take(eawr::checkpoint::save_game_checkpoint(game, build)), build, load, executor));
        expect(restored.session.state_sha256() == game.state_sha256(), "retained admission journal survives disk reconstruction");
        (void)take(game.step(executor)); (void)take(restored.session.step(executor));
        expect(restored.session.state_sha256() == game.state_sha256(), "native owner completes the retained selection identically after restore");
        const auto trace = game.state_sha256();
        if (summary_baseline.empty()) summary_baseline = trace;
        else expect(summary_baseline == trace, "summary invasion equality on 1/2/4/8 workers");
        auto own = setup; own.planets.front().owner = 1;
        // WGM-C04 reconstructs tick-zero ownership from live surface presence.
        own.control->planets.front().ground_base = {1, 1};
        auto own_game = take(cp::GameSession::create(own));
        expect(cp::observe_invasion_offer(result, context, *own_game.galaxy().snapshot(), 1, true).candidates.empty(),
            "current own planet suppresses the retained offer");
        auto blocked = setup; blocked.control->planets.front().surface_accessible = false;
        auto blocked_game = take(cp::GameSession::create(blocked));
        expect(cp::observe_invasion_offer(result, context, *blocked_game.galaxy().snapshot(), 1, true).candidates.empty(),
            "current inaccessible surface suppresses the retained offer");
        auto capped_setup = setup;
        capped_setup.fleets.front().members.clear(); capped_setup.movement->contents.clear();
        for (eawr::sim::EntityId i = 0; i < 130; ++i) {
            capped_setup.fleets.front().members.push_back({100 + i, 2});
            capped_setup.movement->contents.push_back({100 + i, {{1000 + i, 1}}});
        }
        // The other fleet still contains transport23; its empty payload is a
        // known input, distinct from an unavailable movement container row.
        capped_setup.movement->contents.insert(capped_setup.movement->contents.begin(), {23, {}});
        auto capped_game = take(cp::GameSession::create(capped_setup)); done(configure_summary(capped_game));
        cp::BattleResult capped; capped.conflict = 90; capped.winner = 1;
        capped.invasion_offers = cp::tactical_invasion_offers(context, capped, *owner->snapshot());
        expect(capped.invasion_offers.front().transports.size() == 130, "candidate preparation is not capped at the accepted-event limit");
        done(capped_game.restore_latest_tactical_result(context, capped));
        done(capped_game.accept_invasion_offer({1}, 90));
        expect(owner->pending_native_invasion() && owner->pending_native_invasion()->transports.size() == 127,
            "WGP-136 caps accepted retained selection across fleets at 127");
        auto changed_owner = capped_setup;
        changed_owner.fleets.push_back({12, 2, 1, {}});
        changed_owner.fleets.back().members.assign(changed_owner.fleets.front().members.begin(),
            changed_owner.fleets.front().members.begin() + 127);
        changed_owner.fleets.front().members.erase(changed_owner.fleets.front().members.begin(),
            changed_owner.fleets.front().members.begin() + 127);
        auto changed_game = take(cp::GameSession::create(changed_owner)); done(configure_summary(changed_game));
        done(changed_game.restore_latest_tactical_result(context, capped));
        expect(!changed_game.accept_invasion_offer({1}, 90) && !owner->pending_native_invasion(),
            "WGP-136 ownership filtering never backfills transports beyond the first 127 resolved IDs");
    }
    eawr::platform::ThreadWorkerAdapter executor(1);
    auto session = take(cp::GameSession::create(fixture())); gc::GalacticSession* world{};
    const auto configure = [&](cp::GameSession& game) {
        game.set_native_conflict_service(fixture_invasion_provider());
        return game.configure_native_replay_owners([&](auto& owner) { world = &owner; return eawr::core::Result<void>::success(); });
    };
    done(configure(session));
    for (unsigned cycle = 0; cycle < 1024; ++cycle) {
        const auto admitted = take(world->admit_native_invasion(1, 10));
        expect(admitted.admitted && !admitted.queued, "equal-owner invasion returns its retained reader without queuing a battle");
        done(world->retire_native_invasion(admitted.request));
        expect(!world->native_invasion(admitted.request), "reader release prunes a terminal unqueued result");
        expect(section_size(*world, 18) == 24, "section18 remains constant sized across a long sequence of released terminal invasions");
    }
    const auto build = eawr::checkpoint::game_identities(fixture());
    eawr::checkpoint::GameLoadOptions load; load.script_requirement = eawr::campaign::ScriptRequirement::none; load.configure_session = configure;
    auto restored = take(eawr::checkpoint::load_game_checkpoint(take(eawr::checkpoint::save_game_checkpoint(session, build)), build, load, executor));
    expect(restored.session.state_sha256() == session.state_sha256(), "reader release journal reconstructs pruning without remapping the monotonic request allocator");

    auto setup = fixture(); setup.planets.front().owner = 2; setup.control->planets.front().ground_base = {};
    auto pending = take(cp::GameSession::create(setup)); done(configure(pending));
    const auto admitted = take(world->admit_native_invasion(1, 10)); done(world->retire_native_invasion(admitted.request));
    expect(world->native_invasion(admitted.request) && world->native_invasion(admitted.request)->reader_released,
        "plan release retains an outstanding invasion until its actual completion");
    const auto pending_build = eawr::checkpoint::game_identities(setup);
    auto abandoned = take(eawr::checkpoint::load_game_checkpoint(take(eawr::checkpoint::save_game_checkpoint(pending, pending_build)), pending_build, load, executor));
    const auto readers = pending.native_conflict_readers(); auto moved = std::move(pending);
    expect(readers.global() && !readers.admission(), "borrowed move-stable readers distinguish queued invasion from ordinary pending admission");
    (void)take(moved.step(executor)); (void)take(abandoned.session.step(executor));
    expect(moved.state_sha256() == abandoned.session.state_sha256() && !world->native_invasion(admitted.request)
        && !readers.global() && !readers.admission(),
        "released pending invasion is pruned only by real completion with exact disk continuation");

    auto consumed = take(cp::GameSession::create(setup)); done(configure(consumed));
    auto actions = std::make_shared<ga::NativeActions>(*world, std::make_shared<ai::GalacticFunding>(), ga::NativeActions::Placement{}, std::map<gc::PlayerId, Fixed>{});
    actions->bind_admission_gate([] { return false; });
    auto host = std::make_shared<eawr::script::foc::detail::Host>();
    host->view = std::make_shared<eawr::script::foc::detail::WorldView>(); host->galactic_execution = actions;
    auto engine = std::make_shared<ai::Engine>(host, ai::AiData{}, std::vector<ai::PlanDef>{}); host->engine = engine.get();
    ai::GoalType definition; ai::EngineContractAccess::movement_fixture(*engine, definition);
    namespace auth = eawr::script::authoritative;
    auth::ServiceReport admit_report; auth::ScriptCommand command;
    command.issuer = ai::first_plan_instance; command.sequence = 1; command.verb = ai::verb_ai;
    command.arguments = {auth::Value{std::string("galactic_invade")}, auth::Value{auth::Handle{ai::handle_taskforce, ai::galactic_id_base(0) | 8}}};
    admit_report.commands.push_back(std::move(command)); done(engine->after_service(admit_report));
    const auto block_id = ai::block_id(ai::first_plan_instance, 1);
    const auto request = engine->block(block_id)->galactic_request;
    (void)take(consumed.step(executor)); ai::EngineContractAccess::blocks(*engine);
    expect(engine->block(block_id)->finished && engine->block(block_id)->result, "shared block observes actual successful completion before releasing its reader");
    auth::ServiceReport release_report; auth::ScriptCommand release;
    release.issuer = ai::first_plan_instance; release.sequence = 2; release.verb = ai::verb_ai;
    release.arguments = {auth::Value{std::string("galactic_retire_invasion")}, auth::Value{auth::Handle{ai::handle_block, block_id}}};
    release_report.commands.push_back(std::move(release)); done(engine->after_service(release_report));
    expect(!world->native_invasion(request) && !engine->block(block_id)->galactic_request
        && engine->block(block_id)->finished && engine->block(block_id)->result,
        "terminal reader barrier prunes world result while retaining the shared block's completion and true result");
}
void equal_owner_invasion_block() {
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto session = take(cp::GameSession::create(fixture())); gc::GalacticSession* world{};
        const auto configure = [&](cp::GameSession& game) {
            game.set_native_conflict_service(fixture_invasion_provider());
            return game.configure_native_replay_owners([&](auto& owner) { world = &owner; return eawr::core::Result<void>::success(); });
        };
        done(configure(session));
        auto actions = std::make_shared<ga::NativeActions>(*world, std::make_shared<ai::GalacticFunding>(), ga::NativeActions::Placement{}, std::map<gc::PlayerId, Fixed>{});
        actions->bind_admission_gate([] { return false; });
        auto host = std::make_shared<eawr::script::foc::detail::Host>();
        host->view = std::make_shared<eawr::script::foc::detail::WorldView>(); host->galactic_execution = actions;
        auto engine = std::make_shared<ai::Engine>(host, ai::AiData{}, std::vector<ai::PlanDef>{}); host->engine = engine.get();
        ai::GoalType definition; ai::EngineContractAccess::movement_fixture(*engine, definition);
        namespace auth = eawr::script::authoritative;
        auth::ServiceReport admission; auth::ScriptCommand command;
        command.issuer = ai::first_plan_instance; command.sequence = 1; command.verb = ai::verb_ai;
        command.arguments = {auth::Value{std::string("galactic_invade")}, auth::Value{auth::Handle{ai::handle_taskforce, ai::galactic_id_base(0) | 8}}};
        admission.commands.push_back(std::move(command)); done(engine->after_service(admission));
        const auto id = ai::block_id(ai::first_plan_instance, 1); const auto request = engine->block(id)->galactic_request;
        expect(world->native_invasion(request)->state == gc::GalacticSession::NativeInvasion::State::completed
            && !world->native_invasion(request)->success && !world->pending_native_invasion(),
            "equal-owner admission is terminal false without queueing a conflict");
        auth::ModuleManifest manifest;
        done(manifest.add("INVADE.LUA", "finished=false; result=true; function Check(block) finished=block.IsFinished(); result=block.Result() end"));
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = take(auth::ScriptScheduler::create(config, std::move(manifest)));
        std::vector<eawr::core::Diagnostic> errors;
        eawr::script::foc::detail::register_plan_bindings(scripts, host, errors);
        expect(errors.empty(), "actual shared block bindings register for equal-owner terminal polling");
        done(scripts.create_instance(ai::first_plan_instance, "INVADE.LUA"));
        const auto poll = [&](std::uint64_t sequence) {
            auth::ScriptEvent event; event.key = {scripts.completed_tick() + 1, auth::first_simulation_producer, 1, sequence};
            event.target = ai::first_plan_instance; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
            event.arguments = {auth::Value{auth::Handle{ai::handle_block, id}}}; done(scripts.submit_event(std::move(event)));
            auto report = take(scripts.service(executor)); expect(report.diagnostics.empty(), "equal-owner block Lua poll succeeds");
            expect(std::get<bool>(take(scripts.read_global(ai::first_plan_instance, "finished"))->data)
                && !std::get<bool>(take(scripts.read_global(ai::first_plan_instance, "result"))->data),
                "actual IsFinished returns true and Result returns false before and after reader retirement");
            done(engine->after_service(report));
        };
        poll(0); poll(1);
        expect(!world->native_invasion(request) && engine->block(id)->finished && !engine->block(id)->result,
            "equal-owner terminal poll retires its record while retaining a false block result");
        const auto build = eawr::checkpoint::game_identities(fixture()); eawr::checkpoint::GameLoadOptions load; load.script_requirement = eawr::campaign::ScriptRequirement::none; load.configure_session = configure;
        auto restored = take(eawr::checkpoint::load_game_checkpoint(take(eawr::checkpoint::save_game_checkpoint(session, build)), build, load, executor));
        expect(restored.session.state_sha256() == session.state_sha256()
            && restored.session.record().native_effects == session.record().native_effects,
            "equal-owner disk replay reconstructs the exact admitted nonqueued receipt and terminal retirement");
        if (baseline.empty()) baseline = session.state_sha256(); else expect(baseline == session.state_sha256(), "equal-owner block/retirement equality on1/2/4/8");
    }
}
void native_journal_disk() {
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto session = take(cp::GameSession::create(fixture()));
        gc::GalacticSession* owner{};
        done(session.configure_native_replay_owners([&](auto& world) {
            owner = &world; return eawr::core::Result<void>::success();
        }));
        expect(take(owner->land_native_transports(1, 10)).admitted, "journal landing uses actual transports");
        expect(take(owner->launch_native_transport(1, 22, 10)).admitted, "journal launch retains actor");
        expect(take(owner->launch_native_transport(1, 21, 10)).admitted, "journal second launch retains actor");
        expect(take(owner->admit_native_fleet(1, gc::MergeFleets{11, 10})).admitted, "journal merge uses ordinary membership");
        expect(take(owner->admit_native_fleet(1, gc::MoveFleet{10, {1, 2}, {}})).admitted, "journal movement enters real transit");
        const auto build = eawr::checkpoint::game_identities(fixture());
        // Zero-frontier effects must reconstruct even when there are no ticks.
        const auto zero = take(eawr::checkpoint::save_game_checkpoint(session, build));
        auto restored_zero = take(eawr::checkpoint::load_game_checkpoint(zero, build, {}, executor));
        expect(restored_zero.session.state_sha256() == session.state_sha256(), "disk restore applies native effects at frontier zero");
        (void)take(session.step(executor)); (void)take(session.step(executor));
        const auto bytes = take(eawr::checkpoint::save_game_checkpoint(session, build));
        auto restored = take(eawr::checkpoint::load_game_checkpoint(bytes, build, {}, executor));
        expect(restored.session.state_sha256() == session.state_sha256(), "full disk reconstruction restores native transit");
        expect(restored.session.record().native_effects == session.record().native_effects, "native journal history retained exactly on disk restore");
        std::ostringstream trace;
        for (unsigned frame = 0; frame < 60; ++frame) {
            (void)take(session.step(executor)); (void)take(restored.session.step(executor));
            expect(restored.session.state_sha256() == session.state_sha256(), "uninterrupted and disk-restored native transit remain equal");
            trace << session.state_sha256();
        }
        if (baseline.empty()) baseline = trace.str(); else expect(baseline == trace.str(), "native disk continuation equality across 1/2/4/8 workers");
        auto replay = session.record();
        expect(take(gc::parse_replay(take(gc::write_replay(replay)))) == replay, "tag16 native journal canonical roundtrip");
        replay.native_effects.front().sequence = 0;
        expect(!gc::write_replay(replay), "native effect journal refuses noncanonical owner sequence");
    }
}
void contracts() {
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto world = take(gc::GalacticSession::create(fixture())); world.set_hash_verification(true);
        auto funding = std::make_shared<ai::GalacticFunding>();
        auto actions = std::make_unique<ga::NativeActions>(world, funding, ga::NativeActions::Placement{}, std::map<gc::PlayerId, Fixed>{});
        bool pending{}; std::uint64_t searches{}; bind(*actions, world, pending, searches);
        const auto original = world.state_sha256();
        auto denied = take(world.admit_native_fleet(2, gc::MergeFleets{10, 11}));
        expect(!denied.admitted && denied.reason == gc::GalacticRejectReason::not_authorized && world.state_sha256() == original,
            "native regroup uses ordinary ownership validation without mutation");
        expect(!world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {21, 21}}), "native fleet payload uses ordinary duplicate-member validation");
        auto landed = take(world.land_native_transports(1, 10));
        const auto& surface = world.snapshot()->control()->planets.front().landed;
        expect(landed.admitted && surface.size() == 2 && surface[0].id == 22 && surface[1].id == 21,
            "WGA-234 direct allied landing scans reverse fleet order with authored equality capacity");
        expect(take(world.land_native_transports(1, 10)).admitted && world.snapshot()->control()->planets.front().landed.size() == 2,
            "capacity equality returns an admitted empty landing operation");
        expect(take(world.launch_native_transport(1, 22, 10)).admitted && take(world.launch_native_transport(1, 21, 10)).admitted,
            "WGA-227 surface transports launch into retained fleet with real containment");
        expect(world.snapshot()->control()->planets.front().landed.empty(), "same-call readers see completed launch containment");
        auto merged = take(world.admit_native_fleet(1, gc::MergeFleets{11, 10}));
        expect(merged.admitted && world.snapshot()->fleets().size() == 1 && world.snapshot()->fleets().front().members.back().id == 23,
            "WGA-227 formation reuses accepted merge and original identities");
        (void)world.cached_state_hash();
        ga::ActionRequest request; request.kind = ga::ActionKind::move; request.player = 1; request.object = 10; request.target = 3;
        const auto admitted = take(actions->admit(request));
        expect(admitted.admitted && world.snapshot()->fleets().front().motion.transit->path == std::vector<gc::PlanetId>{1, 2},
            "first native reader observes departure and only ordinary path node one");
        bool restored{}, gated{}, resumed{}; std::uint64_t steps{};
        std::ostringstream trace;
        while (!take(actions->poll_movement(admitted.request)).finished && steps < 120) {
            (void)take(world.step(executor)); ++steps;
            if (!restored && steps == 2) {
                const auto state = take(actions->capture()); const auto hash = take(actions->state_hash());
                actions.reset(); actions = std::make_unique<ga::NativeActions>(world, funding, ga::NativeActions::Placement{}, std::map<gc::PlayerId, Fixed>{});
                bind(*actions, world, pending, searches); done(actions->restore(state));
                expect(take(actions->state_hash()) == hash, "exact retained request/actor/policy/event cursor restore mid-transit");
                auto corrupt = state; corrupt.components.begin()->second.push_back(0);
                expect(!actions->restore(corrupt) && take(actions->state_hash()) == hash, "failed movement restore leaves native state intact"); restored = true;
            }
            if (!gated && world.snapshot()->fleets().front().orbit == 2) {
                pending = true; const auto before = searches;
                const auto waiting = take(actions->poll_movement(admitted.request));
                expect(!waiting.finished && waiting.advance_requested && searches == before && world.snapshot()->fleets().front().orbit == 2,
                    "WGA-229 any global conflict defers next hop and retains advance request");
                done(actions->movement_conflict(admitted.request, 2, true));
                expect(!take(actions->poll_movement(admitted.request)).advance_requested, "WGA-231 begin clears movement flags");
                done(actions->movement_conflict(admitted.request, 2, false));
                pending = false; gated = true;
                const auto saved = take(actions->capture()); done(actions->restore(saved));
                resumed = true;
            }
            const auto state = take(actions->poll_movement(admitted.request));
            trace << world.cached_state_hash().get() << take(actions->state_hash()) << state.finished;
        }
        const auto final = take(actions->poll_movement(admitted.request));
        expect(final.finished && final.actor == 10 && world.snapshot()->fleets().front().orbit == 3 && searches == 2 && restored && gated && resumed,
            "two real hops complete with retained actor result and one world-path search per eligible hop");
        if (baseline.empty()) baseline = trace.str(); else expect(baseline == trace.str(), "movement hashes/state/event continuation equal on 1/2/4/8 workers");
    }
}
void changed_path() {
    eawr::sim::InlineExecutor executor;
    auto world = take(gc::GalacticSession::create(fixture()));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    bool deny_destination{}; std::uint64_t searches{};
    actions.bind_admission_gate([] { return false; });
    actions.bind_movement([&](auto player, const auto& snapshot) {
        ++searches; auto input = copied(player, snapshot);
        if (deny_destination) {
            input.access.back().relation = gc::PathRelation::hostile;
            input.access.back().ai_classification_succeeded = true;
            input.access.back().ai_relation = gc::PathRelation::hostile;
        }
        return eawr::core::Result<ga::NativeActions::MovementInput>::success(std::move(input));
    }, [] { return false; }, [&](auto player, auto actor) { return world.recheck_native_orbit(player, actor); });
    ga::ActionRequest request; request.kind = ga::ActionKind::move; request.player = 1; request.object = 10; request.target = 3;
    const auto accepted = take(actions.admit(request));
    std::uint64_t frames{};
    while (world.snapshot()->fleets().front().orbit != 2 && frames++ < 60) (void)take(world.step(executor));
    deny_destination = true;
    const auto result = take(actions.poll_movement(accepted.request));
    expect(result.finished && result.actor == 10 && world.snapshot()->fleets().front().orbit == 2 && searches == 2,
        "changed live path facts terminate at intermediate system rather than traverse cached predecessors or invent success");
}
// Known deviation (#3981): without an AI bribe owner a corruption-benefiting move ends before an occupied intermediate hop.
void bribed_traversal_without_owner() {
    auto world = take(gc::GalacticSession::create(fixture()));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    actions.bind_admission_gate([] { return false; });
    actions.bind_movement([&](auto player, const auto& snapshot) {
        auto input = copied(player, snapshot);
        input.benefits_from_corruption = true; input.orbital_occupants[2] = 2;
        return eawr::core::Result<ga::NativeActions::MovementInput>::success(std::move(input));
    }, [] { return false; }, [&](auto player, auto actor) { return world.recheck_native_orbit(player, actor); });
    ga::ActionRequest request; request.kind = ga::ActionKind::move; request.player = 1; request.object = 10; request.target = 3;
    const auto accepted = take(actions.admit(request));
    const auto state = take(actions.poll_movement(accepted.request));
    expect(state.finished && world.snapshot()->fleets().front().orbit == 1 && !world.snapshot()->fleets().front().motion.transit,
        "an unowned AI bribe hop ends the move at the previous planet instead of faulting");
}
void skipped_arrival_polls() {
    eawr::sim::InlineExecutor executor; auto world = take(gc::GalacticSession::create(fixture()));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    bool pending{}; std::uint64_t searches{}; bind(actions, world, pending, searches);
    ga::ActionRequest request; request.kind = ga::ActionKind::move; request.player = 1; request.object = 10; request.target = 3;
    const auto accepted = take(actions.admit(request));
    for (unsigned frame = 0; frame < 60; ++frame) (void)take(world.step(executor));
    expect(world.snapshot()->fleets().front().orbit == 2 && actions.movements().at(accepted.request).advance_requested,
        "arrival notification retained after its tick events disappear without a block poll");
    const auto saved = take(actions.capture()); done(actions.restore(saved));
    expect(!take(actions.poll_movement(accepted.request)).finished && world.snapshot()->fleets().front().motion.transit,
        "late poll consumes retained arrival after native restore and advances second hop");
    for (unsigned frame = 0; frame < 60; ++frame) (void)take(world.step(executor));
    expect(take(actions.poll_movement(accepted.request)).finished && world.snapshot()->fleets().front().orbit == 3 && searches == 2,
        "late final poll completes despite missing the final arrival tick too");
}
void arrival_then_conflict_before_poll() {
    eawr::sim::InlineExecutor executor;
    auto world = take(gc::GalacticSession::create(fixture()));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    bool pending{}; std::uint64_t searches{}; bind(actions, world, pending, searches);
    ga::ActionRequest request; request.kind = ga::ActionKind::move; request.player = 1; request.object = 10; request.target = 3;
    const auto accepted = take(actions.admit(request));
    for (unsigned frame = 0; frame < 120 && world.snapshot()->fleets().front().orbit != 2; ++frame)
        (void)take(world.step(executor));
    expect(world.snapshot()->fleets().front().orbit == 2 && actions.movements().at(accepted.request).advance_requested,
        "ordered arrival listener consumes event before any block poll");
    pending = true; done(actions.movement_conflict(accepted.request, 2, true));
    const auto orbit_entry = world.snapshot()->fleets().front().motion.orbit_entry;
    const auto state = take(actions.poll_movement(accepted.request));
    expect(!state.finished && !state.advance_requested && searches == 1
        && world.snapshot()->fleets().front().motion.orbit_entry == orbit_entry,
        "poll cannot replay consumed arrival after conflict begin clears both flags");
    pending = false; done(actions.movement_conflict(accepted.request, 2, false));
    (void)take(actions.poll_movement(accepted.request));
    expect(searches == 2 && world.snapshot()->fleets().front().motion.transit.has_value(),
        "matching conflict end resumes the next ordinary hop");
}
void packed_remnant_hash() {
    auto setup = fixture(); setup.fleet_control->types = {{1, false, true, false}, {2, false, true, true}, {3, false, true, true}};
    setup.movement->types[0].squadron_capacity = 1;
    setup.movement->types[1] = {2, {}, f(50), 0, false, true, false, false};
    setup.movement->types.push_back({3, {}, f(60), 0, false, true, false, false});
    // WGM-118/151 classifies each squadron by its first fighter. Keep the
    // carrier hyperspace-capable while both fighters enter the slow bucket.
    setup.movement->types.push_back({4, {}, {}, 0, false, false, false, false});
    setup.fleet_control->types.push_back({4, false, true, false});
    setup.fleets[0].members.back().type = 3;
    setup.movement->contents = {{21, {{31, 4}}}, {22, {{32, 4}}}};
    setup.control->types.push_back({3, 0, false, false});
    setup.control->types.push_back({4, 1, false, false});
    auto world = take(gc::GalacticSession::create(setup)); world.set_hash_verification(true);
    eawr::sim::InlineExecutor executor; (void)take(world.step(executor));
    const auto frontier = world.next_entity_id(); (void)world.cached_state_hash();
    const auto admitted = take(world.admit_native_fleet(1, gc::MoveFleet{10, {1, 2}, {}}));
    expect(admitted.admitted && world.next_entity_id() == frontier + 1 && world.snapshot()->fleets().size() == 3,
        "native packed departure allocates a real remnant with persistent identities");
    expect(world.cached_state_hash().get() == world.state_sha256(), "remnant allocation invalidates cached session/allocator section");
}
void native_management_gate() {
    auto setup = fixture(); setup.seats.front().kind = gc::SeatKind::human; setup.seats.front().human_side_id = 1;
    setup.fleet_control->gates.queued_campaign_conflict = true;
    setup.control->planets.front().landed.push_back({24, 2, 1});
    setup.movement->contents.push_back({24, {}});
    auto world = take(gc::GalacticSession::create(setup));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    actions.bind_admission_gate([] { return true; });
    const auto original = world.state_sha256();
    ga::ActionRequest request; request.player = 1; request.kind = ga::ActionKind::regroup; request.object = 11; request.target = 10;
    expect(!take(actions.admit(request)).admitted, "native regroup receives actual pending-local-conflict management gate");
    request.kind = ga::ActionKind::land; request.object = 10;
    expect(!take(actions.admit(request)).admitted, "native landing preserves the ordinary pending-local-conflict landing gate");
    request.kind = ga::ActionKind::launch; request.object = 24;
    expect(!take(actions.admit(request)).admitted && world.snapshot()->control()->planets.front().landed.front().id == 24,
        "native launch preserves gate and surface containment on rejected fleet admission");
    expect(world.state_sha256() == original, "all gated native management operations retain the original world hash");
}
void installed() {
    std::string root;
#if defined(_MSC_VER)
    char* environment{}; std::size_t size{};
    if (_dupenv_s(&environment, &size, "EAWR_EAW_GAME_ROOT") == 0 && environment) {
        root = environment; std::free(environment);
    }
#else
    if (const auto* environment = std::getenv("EAWR_EAW_GAME_ROOT")) root = environment;
#endif
    if (root.empty()) throw std::runtime_error("installed movement requires EAWR_EAW_GAME_ROOT");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(root) / folder / "Data")).mount);
    const auto files = take(eawr::vfs::Vfs::mount(mounts));
    const auto catalog = take(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = take(eawr::data::galactic::load_galactic_data(files, catalog.catalog));
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Rebel";
    const auto* source = data.campaign(campaign); if (!source) throw std::runtime_error("stock campaign absent");
    cp::CampaignSetupOptions options; options.seed = 42; gc::PlayerId empire{};
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == source->starting_active_player;
        if (human) options.local_observer = id;
        if (faction.name == "Empire") empire = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
    }
    std::vector<std::string> names; for (const auto& type : types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end()); for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
    const auto plan = take(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
    const auto services = take(cp::prepare_live_campaign_services(files, catalog.catalog, data, types, plan, "Normal_Default"));
    auto root_campaign = take(cp::CampaignRoot::create(plan, services));
    const auto setup = root_campaign.game()->galaxy().setup();
    if (!empire || empire == options.local_observer) throw std::runtime_error("stock Normal Empire AI absent");
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto world = take(gc::GalacticSession::create(setup)); world.set_hash_verification(true);
        bool pending{}; std::uint64_t searches{};
        auto funding = std::make_shared<ai::GalacticFunding>();
        ga::NativeActions actions(world, funding, {}, {}); bind(actions, world, pending, searches);
        ga::ActionRequest request; request.kind = ga::ActionKind::move; request.player = empire;
        // A real owned fleet and a friendly two-hop route: no debug relocation,
        // type-only force fabrication or execution threat-matrix predecessor.
        const auto facts = copied(empire, *world.snapshot());
        gc::PathOptions policy; policy.ai_player = true; policy.ai_restrictions = false; policy.allow_bad_if_no_force = false;
        for (const auto& fleet : world.snapshot()->fleets()) {
            if (fleet.owner != empire || !fleet.orbit || fleet.members.empty()) continue;
            for (const auto& destination : setup.planets) {
                if (destination.owner != empire || destination.id == fleet.orbit) continue;
                const auto path = take(gc::find_planet_path(*world.graph(), fleet.orbit, destination.id, facts.access, policy));
                if (path.planets.size() >= 3) { request.object = fleet.id; request.target = destination.id; break; }
            }
            if (request.object) break;
        }
        if (!request.object) throw std::runtime_error("stock Normal Empire has no two-hop friendly route");
        const auto accepted = take(actions.admit(request)); if (!accepted.admitted) throw std::runtime_error("stock fleet cannot depart");
        bool restored{}; std::uint64_t steps{}; std::ostringstream trace;
        while (!take(actions.poll_movement(accepted.request)).finished && steps < 12000) {
            (void)take(world.step(executor)); ++steps;
            if (!restored && steps == 2) {
                const auto saved = take(actions.capture()); done(actions.restore(saved)); restored = true;
            }
            trace << world.cached_state_hash().get() << take(actions.state_hash());
        }
        const auto final = take(actions.poll_movement(accepted.request));
        expect(final.finished && final.actor == request.object && searches >= 2 && restored,
            "installed Normal Empire native movement traverses two ordinary hops and restores mid-transit");
        const auto at = std::find_if(world.snapshot()->fleets().begin(), world.snapshot()->fleets().end(), [&](const auto& fleet) { return fleet.id == request.object; });
        expect(at != world.snapshot()->fleets().end() && at->orbit == request.target, "installed real fleet reaches retained destination");
        if (baseline.empty()) baseline = trace.str(); else expect(baseline == trace.str(), "installed ordinary movement equality on 1/2/4/8 workers");
        std::cout << "stock movement workers=" << workers << " fleet=" << request.object << " destination=" << request.target
            << " hops=" << searches << " frames=" << steps << '\n';
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") installed();
        else { shared_movement_block(); movement_associations(); taskforce_containment(); invasion_queue_disk(); invasion_retention(); equal_owner_invasion_block(); native_journal_disk(); contracts(); changed_path(); bribed_traversal_without_owner(); skipped_arrival_polls(); arrival_then_conflict_before_poll(); packed_remnant_hash(); native_management_gate(); }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
