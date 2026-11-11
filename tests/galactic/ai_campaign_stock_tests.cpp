// R5 runtime preparation contracts. --release is an honest provider/progression
// gate; --installed proves stock preparation and explicit unavailable refusal.
#include "ai_campaign_stock_private.hpp"
#include "galactic_support.hpp"
#include "raid_fixture.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/platform/agreed_campaign.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/sim/galactic/offer_pool.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../../src/campaign/galactic_ai_runtime.hpp"
#include "ai_campaign_gate.hpp"
#include "ai_learning_gate.hpp"
#include "ai_history_diff.hpp"
#include "ai_quality_run.hpp"
#include "../../src/campaign/galactic_ai_usability.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/campaign/setup.hpp"
#include "../../src/platform/galactic/save_repository.hpp"
#include "../../src/checkpoint/game_session_internal.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include <ctime>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
using namespace galactic_test;
using namespace galactic_campaign_stock_test;
namespace cp = eawr::campaign;
namespace ck = eawr::checkpoint;
namespace auth = eawr::script::authoritative;
namespace ga = eawr::script::foc::galactic_ai;
template<class T> T must(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + " " + result.error().message);
    return std::move(result).value();
}
void must(eawr::core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void unattended_human_contracts() {
    namespace gate = galactic_campaign_gate;
    auto setup = galactic::synthetic_galaxy({});
    setup.seats.back().kind = galactic::SeatKind::ai; setup.seats.back().human_side_id = 0;
    setup.seats.front().kind = galactic::SeatKind::human;
    setup.seats.front().human_side_id = 1;
    cp::ConflictRecord conflict;
    conflict.id = 90000; conflict.planet = setup.planets.front().id;
    conflict.invader = 2; conflict.defender = 1;
    conflict.tactical.content_identity = setup.content_identity;
    conflict.tactical.first_free_entity_id = conflict.id + 1;
    conflict.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    conflict.seats = {{{1}, 1}, {{2}, 2}};
    const auto configure = [](cp::GameSession& game) {
        game.set_conflict_service([](const auto&, const auto&, auto) {
            return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success({});
        }, [](const auto&) {
            return eawr::core::Result<eawr::sim::tactical::TacticalContent>::success({});
        }, {}, cp::ScriptRequirement::none);
        game.set_auto_resolver([](const auto& entry, const auto&, const auto&) {
            cp::AutoResolveOutcomeApplication out;
            out.planet = entry.planet; out.invader = entry.invader; out.defender = entry.defender;
            out.planet_type = "Fixture"; out.result.conflict = entry.id; out.result.winner = entry.defender;
            return eawr::core::Result<std::optional<cp::AutoResolveOutcomeApplication>>::success(out);
        });
        return eawr::core::Result<void>::success();
    };
    for (const bool interactive : {false, true}) {
        auto initial = setup; if (interactive) initial.interactive_seat = {1};
        auto game = must(cp::GameSession::create(initial)); must(configure(game));
        must(game.submit({{game.completed_tick(), {1}, 41}, galactic::SetSpeed{false, eawr::sim::math::Fixed::from_raw(eawr::sim::math::Fixed::scale)}}));
        eawr::platform::ThreadWorkerAdapter first_executor(1); (void)must(game.step(first_executor));
        must(game.queue_conflict(conflict, {cp::ConflictRoute::pending_tactical, false, false}));
        eawr::platform::ThreadWorkerAdapter executor(1);
        expect(!gate::unattended_human_input(game), "announcement is not answered before readiness");
        for (unsigned step = 0; step < 4 && (!game.pending_conflict_view() || !game.pending_conflict_view()->choice_ready); ++step)
            (void)must(game.step(executor));
        expect(game.pending_conflict_view() && game.pending_conflict_view()->choice_ready
            && game.galaxy().clock().pause_depth == 1, "real human conflict owns a ready paused prompt");
        const auto frame = game.galaxy().clock().galactic_frame;
        const auto identities = ck::game_identities(initial);
        const auto bytes = must(ck::save_game_checkpoint(game, identities));
        galactic_test::mark_saved(game);
        ck::GameLoadOptions load; load.script_requirement = cp::ScriptRequirement::none; load.configure_session = configure;
        load.content = [](const auto&) { return eawr::core::Result<eawr::sim::tactical::TacticalContent>::success({}); };
        std::vector<ck::GameRestore> restored;
        std::vector<std::unique_ptr<eawr::platform::ThreadWorkerAdapter>> workers;
        for (const unsigned count : {1U, 2U, 4U, 8U}) {
            workers.push_back(std::make_unique<eawr::platform::ThreadWorkerAdapter>(count));
            restored.push_back(must(ck::load_game_checkpoint(bytes, identities, load, *workers.back())));
        }
        for (unsigned step = 0; step < 5; ++step) {
            const auto command = gate::unattended_human_input(game);
            expect(!gate::unattended_human_input(game), "same boundary never duplicates an accepted answer");
            if (!step) expect(command && std::get<galactic::BattleChoice>(command->payload).conflict == conflict.id,
                "normal Auto Resolve command admitted for the actual human conflict");
            if (!step) expect(command && command->key.sequence == 42, "policy derives monotonic sequence from accepted recording state");
            if (interactive && game.battle_summary_hold()) expect(command
                && galactic::opcode_of(command->payload) == 9, "interactive summary uses opcode 9 acknowledgement");
            const auto tick = must(game.step(executor));
            for (std::size_t i = 0; i < restored.size(); ++i) {
                expect(gate::unattended_human_input(restored[i].session) == command, "restored policy derives identical command keys and payloads");
                gate::compare(tick, must(restored[i].session.step(*workers[i])));
                expect(restored[i].session.record().commands == game.record().commands,
                    "restored accepted commands and outcomes equal on 1/2/4/8");
            }
        }
        expect(!game.pending_conflict() && !game.battle_summary_hold() && !game.galaxy().clock().pause_depth
            && game.galaxy().clock().galactic_frame > frame, "human input releases conflict and summary pauses and advances galaxy");
    }
}
void paid_selection_identity_contracts() {
    using Access = eawr::script::foc::ai::EngineContractAccess;
    eawr::script::foc::ai::BuildTask task;
    task.source = 2; task.issued = 334; task.completion = 2167;
    expect(Access::paid_task_matches(task, 2167), "paid task matches the admitted build request carried in completion");
    expect(!Access::paid_task_matches(task, 334), "paid task does not match its issue frame as a build id");
    task.staging = 1;
    expect(!Access::paid_task_matches(task, 2167), "staging paid task is not an unstaged selection");
    task.staging = 0; task.failed = true;
    expect(!Access::paid_task_matches(task, 2167), "failed paid task is not a selection");
    task.failed = false; task.source = 1;
    expect(!Access::paid_task_matches(task, 2167), "non-purchase task source is not a paid selection");
}
void blocked_invasion_overlap_contract() {
    namespace gate = galactic_campaign_gate;
    // A raid arrival retains an explicit pending invasion; with no land admission service it stays
    // pending, so an ordinary human conflict admitted afterwards is the first conflict it waits behind.
    const auto setup = raid_test::setup();
    cp::ConflictRecord conflict;
    conflict.id = 90000; conflict.planet = 1; conflict.invader = 2; conflict.defender = 1;
    conflict.tactical.content_identity = setup.content_identity;
    conflict.tactical.first_free_entity_id = conflict.id + 1;
    conflict.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    conflict.seats = {{{1}, 1}, {{2}, 2}};
    const auto configure = [](cp::GameSession& game) {
        game.set_conflict_service([](const auto&, const auto&, auto) {
            return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success({});
        }, [](const auto&) {
            return eawr::core::Result<eawr::sim::tactical::TacticalContent>::success({});
        }, {}, cp::ScriptRequirement::none);
    };
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto game = must(cp::GameSession::create(setup)); configure(game);
        eawr::platform::ThreadWorkerAdapter executor(workers);
        must(game.submit({{0, {1}, 1}, galactic::MoveFleet{10, {1, 2}, {}, true}}));
        for (unsigned step = 0; step < 40 && !game.galaxy().pending_native_invasion(); ++step) (void)must(game.step(executor));
        const auto* invasion = game.galaxy().pending_native_invasion();
        expect(invasion && invasion->state == galactic::GalacticSession::NativeInvasion::State::pending
            && invasion->fleet == 10 && invasion->planet == 2, "raid arrival retains a pending invasion");
        expect(!game.has_pending_conflict_for_admission() && !gate::invasion_blocked_by_conflict(game),
            "a pending invasion alone is not blocked");
        const auto request = invasion ? invasion->request : 0;
        must(game.queue_conflict(conflict, {cp::ConflictRoute::pending_tactical, false, false}));
        for (unsigned step = 0; step < 4 && !game.has_pending_conflict_for_admission(); ++step) (void)must(game.step(executor));
        expect(game.pending_conflict() && game.pending_conflict()->id == conflict.id, "the ordinary conflict is the pending admission");
        const auto* retained = game.galaxy().pending_native_invasion();
        expect(retained && retained->request == request
            && retained->state == galactic::GalacticSession::NativeInvasion::State::pending,
            "the invasion stays retained and pending behind the first conflict");
        expect(gate::invasion_blocked_by_conflict(game), "gate predicate observes the overlap");
    }
}
void worker_verdict_contracts() {
    namespace gate = galactic_campaign_gate;
    const std::array<std::size_t, 4> standard{1, 2, 4, 8};
    const std::array<std::size_t, 5> with_hardware{1, 2, 4, 8, 20};
    std::set<std::uint32_t> workers{1, 2, 4, 8};
    expect(gate::expected_workers_present(workers, standard), "standard four-worker evidence passes its captured set");
    expect(!gate::expected_workers_present(workers, with_hardware), "missing hardware-worker evidence refuses");
    workers.insert(20);
    expect(gate::expected_workers_present(workers, with_hardware), "fifth hardware-worker evidence passes its captured set");
    expect(!gate::expected_workers_present(workers, standard), "unexpected worker evidence refuses");
    workers.erase(1); workers.insert(16);
    expect(!gate::expected_workers_present(workers, with_hardware), "matching count cannot replace a missing required worker");
    std::map<std::uint32_t, std::uint64_t> continuations;
    for (auto worker : with_hardware) continuations.emplace(static_cast<std::uint32_t>(worker), gate::minimum_branch_frames);
    expect(gate::continuation_workers_valid(continuations, with_hardware), "all five compared branches pass their frame minimum");
    --continuations.at(20);
    expect(!gate::continuation_workers_valid(continuations, with_hardware), "short hardware-worker branch still refuses");
    continuations.at(20) = gate::minimum_branch_frames; continuations.erase(2);
    expect(!gate::continuation_workers_valid(continuations, with_hardware), "missing standard branch still refuses");
    const auto expected = eawr::platform::determinism_worker_counts(); workers.clear();
    for (auto worker : expected) workers.insert(static_cast<std::uint32_t>(worker));
    expect(gate::expected_workers_present(workers, expected), "actual host capture and verdict use the same worker set");
}










std::filesystem::path game_root() {
#ifdef _MSC_VER
    char* value{}; std::size_t size{};
    if (_dupenv_s(&value, &size, "EAWR_EAW_GAME_ROOT") || !value) throw std::runtime_error("installed campaign gate requires EAWR_EAW_GAME_ROOT");
    std::filesystem::path root(value); std::free(value); return root;
#else
    const auto* value = std::getenv("EAWR_EAW_GAME_ROOT");
    if (!value) throw std::runtime_error("installed campaign gate requires EAWR_EAW_GAME_ROOT");
    return value;
#endif
}
// Installed structural/lifecycle proof, separate from opponent progression.
// Both constructors are actual stock native force writers; no AI barrier is
// active when the first birth delivers through the configured worker fallback.
void installed_world_phase_births(const cp::CampaignSetupPlan& plan, const cp::CampaignSetupServices& services,
    const cp::GalacticAiRuntimeSetup& setup, galactic::PlayerId player) {
    const auto ship = std::find_if(setup.content->types.begin(), setup.content->types.end(), [](const auto& row) {
        return row.second.name == "ACCLAMATOR_ASSAULT_SHIP";
    });
    expect(ship != setup.content->types.end(), "installed ordinary Acclamator constructor is qualified");
    if (ship == setup.content->types.end()) return;
    const auto members = cp::freestore_member_qualifier(setup.content);
    std::string baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        auto configured = std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers);
        const eawr::sim::PartitionExecutor* admitted{};
        auto root = must(cp::CampaignRoot::create(plan, services));
        auto world = must(galactic::GalacticSession::create(root.game()->galaxy().setup()));
        const auto planet = std::find_if(world.snapshot()->planets().begin(), world.snapshot()->planets().end(),
            [player](const auto& row) { return row.owner == player; });
        expect(planet != world.snapshot()->planets().end(), "installed constructor has a real owned source planet");
        if (planet == world.snapshot()->planets().end()) return;
        const auto location = planet->id;
        ga::NativeActions actions(world, std::make_shared<eawr::script::foc::ai::GalacticFunding>(),
            [inputs = services.stock_starting_forces](const auto& request, const auto& snapshot, auto first) {
                return cp::stock_native_force_builder(inputs, {request.player, request.target, request.type, {}})(snapshot, first);
            }, {}, {}, configured.get());
        const cp::GalacticAiSnapshotActorFactsReader reader = [content = setup.content, members](const auto& snapshot, auto actor) {
            return cp::read_stock_galactic_ai_actor_facts(*content, members, snapshot, actor);
        };
        cp::bind_galactic_ai_actor_changes(actions, reader, [&admitted, configured] {
            return admitted ? admitted : configured.get();
        });
        actions.bind_freestore_births([content = setup.content, members](auto& retained, const auto& snapshot,
            const auto& shadow, auto changes, const auto& executor) {
            return cp::apply_galactic_ai_actor_changes(retained, changes, [&snapshot, &shadow, content, members](auto actor) {
                return cp::read_stock_galactic_ai_actor_facts(*content, members, snapshot, actor, &shadow);
            }, executor);
        });
        using Change = galactic::GalacticSession::NativeActorChange;
        std::vector<Change> initial;
        for (const auto& actor : world.snapshot()->actor_records())
            initial.push_back({Change::Kind::created, actor.id, actor.generated_source, actor.dead, actor.delete_pending, {}, {}, {}});
        must(cp::apply_galactic_ai_actor_changes(actions.qualified_freestore(), initial,
            [&](auto actor) { return reader(*world.snapshot(), actor); }, *configured));
        const auto offers = must(ga::initial_freestore_offers(*world.snapshot(), actions.qualified_freestore()));
        must(actions.qualified_freestore().initialize(player, offers));
        actions.bind_handlers();
        const auto before = actions.qualified_freestore().object_count(player);
        ga::ActionRequest request; request.kind = ga::ActionKind::magic;
        request.player = player; request.target = location; request.type = ship->first;
        const auto born = must(actions.admit(request));
        expect(born.admitted && actions.created(player).size() == 1 && !admitted
            && actions.qualified_freestore().contains(player, born.request)
            && actions.qualified_freestore().object_count(player) == before + 1,
            "actual world-phase stock birth offers through configured workers without an active AI barrier");
        const auto* born_facts = actions.qualified_freestore().facts(born.request);
        const auto* current = born_facts ? setup.content->types.at(born_facts->current_type).original.get() : nullptr;
        bool loaded_unit_ai{};
        if (current) for (const auto* behavior : {&current->behavior, &current->space_behavior, &current->land_behavior})
            loaded_unit_ai = loaded_unit_ai || std::find(behavior->begin(), behavior->end(), "UNIT_AI") != behavior->end();
        expect(current && born_facts->unit_ai == loaded_unit_ai && born_facts->prefer_ground,
            "stock current behavior supplies unit-AI presence and ordinary birth initializes preference true");
        if (born_facts && loaded_unit_ai) {
            must(actions.qualified_freestore().set_prefer_ground(born.request, false));
            const auto reread = must(reader(*world.snapshot(), born.request));
            expect(reread.unit_ai && !actions.qualified_freestore().facts(born.request)->prefer_ground,
                "stock type reader does not replace the retained actor preference with a fresh default");
        }
        auto host = actions.immediate_host([inputs = services.stock_starting_forces](const auto& input, const auto& view, auto first) {
            return cp::stock_native_force_shadow_builder(inputs, {input.player, input.target, input.type, {}})(view, first);
        }, 4);
        auto shadow = must(host->capture());
        const auth::ValueList args{auth::Value{auth::Handle{eawr::script::foc::handle_player, player}},
            auth::Value{auth::Handle{eawr::script::foc::handle_game_object, location}},
            auth::Value{auth::ValueList{auth::Value{auth::Handle{eawr::script::foc::handle_type, ship->first}}}}};
        const auto made = must(shadow->apply("galactic.magic_force", args));
        const auto& actors = std::get<auth::ValueList>(made.at(0).data);
        const auto private_actor = std::get<auth::Handle>(actors.at(0).data).id;
        const auth::ValueList count_args{auth::Value{auth::Handle{ga::handle_galactic_freestore, player}}};
        const auto count = [&] { return std::get<ga::Number>(must(shadow->query("galactic.freestore_count", count_args)).at(0).data); };
        expect(count() == ga::Number(static_cast<std::int64_t>(before + 2)), "same-pass count sees actual private birth");
        must(shadow->apply("galactic.prevent_ai_usage", {auth::Value{auth::Handle{eawr::script::foc::handle_game_object, private_actor}}, auth::Value{true}}));
        expect(count() == ga::Number(static_cast<std::int64_t>(before + 1)), "same-pass prevent removes the new private membership");
        must(host->publish(std::move(shadow)));
        expect(world.snapshot()->current_type(private_actor) && actions.qualified_freestore().facts(private_actor)
            && !actions.qualified_freestore().contains(player, private_actor)
            && actions.qualified_freestore().object_count(player) == before + 1,
            "private publication preserves prevent and suppresses duplicate world birth offers");
        const auto result = world.state_sha256() + must(actions.state_hash());
        if (baseline.empty()) baseline = result;
        expect(result == baseline, "actual world/private constructor and pool bytes are equal on1/2/4/8 workers");
    }
}
// WGA-309/436..439 installed evidence: units of the stock free store actually move. The movement counter is the
// number of distinct free-store movement requests the native owner retained; the hop witness is a free-store
// fleet observed in transit. Fixed frame count, so every worker count compares the same state.
struct MovementProbe {
    std::size_t workers{};
    std::uint64_t frames{}, requests{}, first_request_frame{}, hop_frame{}, refusals{};
    std::vector<std::pair<std::uint64_t, std::string>> hashes;
    std::string final_hash, failure;
};
std::vector<MovementProbe> movement_probes;
void freestore_movement_probe(cp::GameSession& game, const cp::GalacticAiRuntimeSetup& setup,
    const std::shared_ptr<galactic_campaign_gate::Observations>& observed, std::uint64_t frames, std::size_t workers) {
    MovementProbe out; out.workers = workers;
    try {
        if (observed->runtimes.empty()) throw std::runtime_error("campaign factory did not publish native runtime owners");
        const auto view = observed->runtimes.front();
        std::set<std::uint64_t> seen;
        while (out.frames < frames) {
            const auto tick = must(game.step(*setup.executor));
            ++out.frames;
            const auto actions = view.actions.lock();
            if (!actions) throw std::runtime_error("native action owner lifetime ended during the probe");
            out.refusals = actions->freestore_move_refusals();
            for (const auto& [request, movement] : actions->movements()) {
                if (!movement.freestore_actor || !movement.actor || !seen.insert(request).second) continue;
                if (seen.size() == 1) out.first_request_frame = out.frames;
            }
            if (!out.hop_frame) for (const auto& entry : actions->movements()) {
                const auto& movement = entry.second;
                if (!movement.freestore_actor || !movement.registered || movement.finished) continue;
                const auto fleets = tick.galaxy->fleets();
                const auto fleet = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == movement.actor; });
                if (fleet != fleets.end() && fleet->motion.transit) { out.hop_frame = out.frames; break; }
            }
            if (out.frames % 300 == 0) out.hashes.emplace_back(out.frames, game.state_sha256());
        }
        out.requests = seen.size();
        out.final_hash = game.state_sha256();
    } catch (const std::exception& error) { out.failure = error.what(); }
    movement_probes.push_back(std::move(out));
}
// R1 (co-op design): the installed single-player map with one human team and AI factions only, run under networked
// transport with multiplayer rules and again with single-player rules. Reports whether a victory condition fires.
int coop_victory_r1(std::uint64_t ticks) {
    const auto root = game_root();
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    auto files = must(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = must(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Underworld";
    const auto* source = data.campaign(campaign);
    expect(source && source->starting_active_player == "Underworld", "stock campaign authors the Underworld human seat");
    for (const auto flavour : {cp::CampaignRulesFlavour::multiplayer, cp::CampaignRulesFlavour::single_player}) {
        cp::CampaignSetupOptions options; options.seed = 42;
        options.mode = cp::CampaignSetupMode::multiplayer; options.rules_flavour = flavour;
        std::vector<ga::PlayerSelection> selected;
        for (const auto& faction : data.factions) {
            const auto id = static_cast<galactic::PlayerId>(options.players.size() + 1);
            const bool human = faction.name == source->starting_active_player;
            const bool ai = faction.name == "Empire" || faction.name == "Rebel";
            options.players.push_back({{id, id, 0}, faction.name, human || ai || !faction.is_playable.value_or(false)});
            if (human || ai) options.seats.push_back({galactic::SeatId{id}, id, human ? galactic::SeatKind::human : galactic::SeatKind::ai,
                id - 1, true, human ? 1U : 0U});
            if (ai) selected.push_back({id, faction.name == "Empire" ? "BASICEMPIRE" : "BASICREBEL", "NORMAL", {}, {}});
        }
        const auto human_seat = std::find_if(options.seats.begin(), options.seats.end(),
            [](const auto& seat) { return seat.kind == galactic::SeatKind::human; });
        if (human_seat == options.seats.end()) throw std::runtime_error("co-op fixture needs a human faction");
        auto teammate = *human_seat;
        teammate.seat_id = galactic::SeatId{static_cast<std::uint32_t>(data.factions.size() + 1)};
        teammate.lobby_slot = static_cast<std::uint32_t>(data.factions.size());
        options.seats.push_back(teammate);
        std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
        std::sort(originals.begin(), originals.end());
        for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
        const auto plan = must(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
        const auto prepared = must(cp::prepare_stock_galactic_ai_runtime(files, loaded.catalog, data, types, plan, selected,
            std::make_shared<eawr::platform::ThreadWorkerAdapter>(1), false, 4096, 4096));
        expect(plan.human_observers().size() == 1 && prepared->story_players.shared_local_player ==
            (flavour == cp::CampaignRulesFlavour::single_player ? std::optional{plan.human_observers().front()} : std::nullopt),
            "prepared story players expose the agreed human faction only under networked single-player rules");
        const auto services = must(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default"));
        auto root_owner = must(cp::CampaignRoot::create(plan, services));
        const eawr::platform::ThreadWorkerAdapter executor(1);
        bool fired = false; std::uint64_t first_tick = 0;
        for (std::uint64_t tick = 0; tick < ticks; ++tick) {
            must(root_owner.game()->step(executor));
            const auto& victory = root_owner.game()->galaxy().snapshot()->victory();
            if (!fired && victory && (victory->pending || victory->result_dispatched)) { fired = true; first_tick = tick + 1; }
        }
        std::cout << "R1 real map " << campaign << " " << (flavour == cp::CampaignRulesFlavour::multiplayer ? "multiplayer" : "single_player")
                  << " rules, human team vs AI: victory fired within " << ticks << " ticks = " << (fired ? "yes" : "no");
        if (fired) std::cout << " (first at tick " << first_tick << ")";
        std::cout << std::endl;
        // Measured on the installed map: the human team does not win against AI-only opposition within the window
        // under either rules flavour. The window is 1800 ticks (one minute), past the 30-frame victory delay.
        expect(!fired, "co-op human team vs AI only does not end the campaign early on the installed single-player map");
    }
    return failures ? 2 : 0;
}
int installed(bool release, const std::filesystem::path& directory, bool benchmark, std::uint64_t probe_frames = 0,
    std::size_t probe_workers = 1, bool human_encounter_only = false, bool hash_baseline = false) {
    const auto root = game_root();
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    auto files = must(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = must(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    // Stock single-player authoring supplies Human to eager perception and Lua
    // equations (WGA-417/418/422). Keep both observed AI players and use the
    // actual Equal Footing Underworld campaign's authored human seat.
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Underworld";
    const auto* source = data.campaign(campaign);
    expect(source && source->starting_active_player == "Underworld", "stock release campaign authors the Underworld human seat");
    cp::CampaignSetupOptions options; options.seed = 42;
    galactic::PlayerId human{};
    std::vector<ga::PlayerSelection> selected;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<galactic::PlayerId>(options.players.size() + 1);
        const bool selected_faction = faction.name == "Empire" || faction.name == "Rebel" || faction.name == source->starting_active_player;
        options.players.push_back({{id, id, 0}, faction.name, selected_faction || !faction.is_playable.value_or(false)});
        if (faction.name == "Empire" || faction.name == "Rebel") {
            options.seats.push_back({galactic::SeatId{id}, id, galactic::SeatKind::ai, id - 1, true, 0});
            selected.push_back({id, faction.name == "Empire" ? "BASICEMPIRE" : "BASICREBEL", "NORMAL", {}, {}});
        }
        if (faction.name == source->starting_active_player) {
            human = id; options.solo_reference_human = id;
            options.seats.push_back({galactic::SeatId{id}, id, galactic::SeatKind::human, id - 1, true, 1});
        }
    }
    expect(selected.size() == 2, "frozen Empire/Rebel AI seats exist in installed data");
    expect(human != 0, "frozen authored human exists in installed data");
    selected[0].enemy = selected[1].player; selected[1].enemy = selected[0].player;
    for (auto& player : selected) player.human = human;
    std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
    const auto plan = must(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
    // EAWR_GCAI_TEST_SCHEDULE=reference selects the synchronous reference schedule (the async parity comparison arm).
    const auto setup = must(cp::prepare_stock_galactic_ai_runtime(files, loaded.catalog, data, types, plan, selected,
        std::make_shared<eawr::platform::ThreadWorkerAdapter>(probe_workers), false, 4096, 4096, {},
        eawr::script::foc::galactic_ai::retail_service_periods, galactic_campaign_gate::test_schedule()));
    const auto human_facts = std::find_if(setup->perception.players.begin(), setup->perception.players.end(),
        [human](const auto& row) { return row.player == human; });
    expect(human_facts != setup->perception.players.end() && human_facts->human && !human_facts->ai
        && human_facts->faction == source->starting_active_player, "release Human comes from the actual prepared active human seat");
    expect(!setup->plans.plans.empty() && !setup->modules.empty() && !setup->perception.force_types.empty()
        && setup->content_identity.size() == 64, "installed selected plan/Lua/type closure prepared from real providers");
    if (release) {
        std::filesystem::create_directories(directory);
        auto observed = std::make_shared<galactic_campaign_gate::Observations>();
        for (const auto& player : selected) observed->players.emplace(player.player, galactic_campaign_gate::PlayerProgress{});
        std::ofstream frozen(directory / "case.json", std::ios::trunc);
        frozen << "{\"campaign\":\"" << campaign << "\",\"seed\":42,\"difficulty\":\"NORMAL\",\"closure\":\"" << setup->content_identity
            << "\",\"uses_fog\":false,\"mode_fps\":30,\"game_seconds_limit\":900,\"immediate_instances\":4096,\"immediate_requests\":4096,\"human\":{\"id\":" << human
            << ",\"faction\":\"Underworld\"},\"unattended_human_policy\":\"ordinary_auto_resolve_and_interactive_summary_ack\",\"players\":[";
        bool first = true;
        for (const auto& player : selected) {
            if (!first) { frozen << ','; }
            first = false;
            frozen << "{\"id\":" << player.player << ",\"type\":"; galactic_campaign_gate::json_string(frozen, player.player_type); frozen << '}';
        }
        frozen << "]}\n"; frozen.close();
        expect(static_cast<bool>(frozen), "frozen installed gate identity report is durable");
        bool initialized{};
        try {
            const auto services = must(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default"));
            auto root_owner = must(cp::CampaignRoot::create(plan, services));
            for (const auto& actor : root_owner.game()->galaxy().snapshot()->actor_records()) {
                const auto original = setup->content->types.find(actor.original_type);
                const auto current = setup->content->types.find(actor.current_type);
                expect(original != setup->content->types.end() && original->second.original
                    && current != setup->content->types.end() && current->second.original,
                    "installed constructor actor identities resolve in immutable AI content");
            }
            const auto space = must(cp::prepare_stock_space_entry(files, loaded.catalog, data, types, plan));
            const auto usability = must(cp::prepare_stock_galactic_ai_usability(data, plan, setup->perception));
            cp::GalacticAiStockQueryFactory queries = [usability] {
                auto provider = std::make_shared<cp::GalacticAiUsabilityProvider>(usability);
                cp::GalacticAiStockQueries out;
                out.usability = [provider](auto player, const auto& snapshot, auto planet) { return provider->usability(player, snapshot, planet); };
                out.connectivity_publication = [provider](const auto& parent, auto& world, auto& engine, const auto& workers) { return provider->connectivity_publication(parent, world, engine, workers); };
                out.movement = [provider](auto player, const auto& snapshot, const auto& world, const auto& engine, const auto& workers) { return provider->movement(player, snapshot, world, engine, workers); };
                out.external.emplace("GALACTIC_USABILITY", std::move(provider));
                return eawr::core::Result<cp::GalacticAiStockQueries>::success(std::move(out));
            };
            auto factory = must(cp::stock_galactic_ai_providers(setup, services.stock_starting_forces, space, std::move(queries),
                [observed](auto view) { observed->runtimes.push_back(std::move(view)); },
                [observed](const auto& notice) { observed->battle(notice); }));
            eawr::platform::CampaignServiceConfiguration configuration;
            configuration.configure = [&](auto& game) { return cp::configure_live_campaign_conflicts(game, files, loaded.catalog, data, types, plan, space); };
            configuration.restore.configure_session = configuration.configure;
            configuration.restore.content = [space](const auto& conflict) { return space->content(conflict); };
            must(cp::configure_galactic_ai_runtime(configuration, setup, std::move(factory)));
            must(configuration.configure(*root_owner.game()));
            must(configuration.configure_native_ai(*root_owner.game())); initialized = true;
            if (probe_frames == 1) {
                // Short stock proof (--prepared-stealth): the stock droid team is born by the prepared AI game's own first
                // ticks (hero placement), and that birth must keep its authored stealth (WGM-38/163). A retail transport of
                // stealthy members skips combat, which the staging move of the Rebel tech plan relies on.
                std::size_t teams{}, stealthy_members{}, members{};
                for (unsigned tick = 0; tick < 30 && !teams; ++tick) {
                    must(root_owner.game()->step(*setup->executor));
                    const auto born = root_owner.game()->galaxy().snapshot();
                    for (const auto& transport : born->actor_records()) {
                        const auto type = setup->content->types.find(transport.original_type);
                        if (type == setup->content->types.end() || eawr::script::foc::ai::upper_case(type->second.name) != "DROIDS_TEAM") continue;
                        ++teams;
                        for (const auto& member : born->actor_records()) if (member.container == transport.id) {
                            ++members; stealthy_members += member.abilities.present && member.abilities.combat_skip;
                        }
                        const auto skip = born->actor_combat_skip(transport.id);
                        if (!skip || !skip.value()) throw std::runtime_error("prepared AI game: stock droid team transport " + std::to_string(transport.id)
                            + " does not skip combat (members=" + std::to_string(members) + " stealthy=" + std::to_string(stealthy_members) + ")");
                    }
                }
                if (!teams) throw std::runtime_error("prepared AI game: the stock droid team was not born within 30 ticks");
                if (stealthy_members != members || !members) throw std::runtime_error("prepared AI game: stock droid team lost its authored stealth (members="
                    + std::to_string(members) + " stealthy=" + std::to_string(stealthy_members) + ")");
            }
            if (!probe_frames && !human_encounter_only && !hash_baseline)
                galactic_history_diff::verify(plan, services, configuration, *setup->executor, directory);
            if (probe_frames) {
                freestore_movement_probe(*root_owner.game(), *setup, observed, probe_frames, probe_workers);
                return movement_probes.back().failure.empty() ? 0 : 2;
            }
            const auto report = galactic_campaign_gate::run(*root_owner.game(), *setup, configuration, observed, directory,
                galactic_learning_gate::probe(directory / "learning.json"), human_encounter_only, hash_baseline,
                !benchmark && !human_encounter_only && !hash_baseline);
            report.write(std::cout);
            // W1 floors A (owner, 11-07) for digest_mature (the benchmark run) against the stored base: main e536b8302, async, seed 42.
            // That base stops at galactic frame 9900 on #6337; a run that stops elsewhere has no base yet (refresh it when #6337 or a
            // reviewed upstream change moves the stop). Floors: builds 10 % (min 2); expansions, attacks, defences and battles within 2;
            // movements 10 %; maximum tech exact; first tech-3 frame within 300.
            bool floors = true;
            if (benchmark) {
                struct FloorBase { eawr::sim::galactic::PlayerId id; std::uint64_t started, completed, expansions, attacks, defences, movements;
                    std::int64_t tech; std::uint64_t tech3; };
                constexpr std::array<FloorBase, 2> floor_base{{{1, 40, 31, 6, 12, 7, 640, 1, 0}, {2, 39, 21, 4, 7, 12, 765, 3, 5164}}};
                constexpr std::uint64_t base_frames = 9900, base_battles = 19;
                const auto within = [](std::uint64_t value, std::uint64_t base, std::uint64_t floor_value) {
                    return (value > base ? value - base : base - value) <= floor_value;
                };
                const auto share = [](std::uint64_t base) { return std::max<std::uint64_t>(2, base / 10); };
                floors = report.galactic_frames == base_frames && within(report.battles, base_battles, 2);
                for (const auto& base : floor_base) {
                    const auto at = report.players.find(base.id);
                    const bool held = at != report.players.end() && within(at->second.builds_started, base.started, share(base.started))
                        && within(at->second.builds_completed, base.completed, share(base.completed))
                        && within(at->second.expansions, base.expansions, 2) && within(at->second.attacks, base.attacks, 2)
                        && within(at->second.defences, base.defences, 2)
                        && within(at->second.movements, base.movements, std::max<std::uint64_t>(1, base.movements / 10))
                        && at->second.maximum_tech == base.tech
                        && (base.tech3 ? within(at->second.first_tech3_frame, base.tech3, 300) : at->second.first_tech3_frame == 0);
                    floors = floors && held;
                }
                std::cout << "GCAI-ASYNC-FLOORS digest_mature frames=" << report.galactic_frames << " base_frames=" << base_frames
                    << " pass=" << (floors ? "true" : "false") << std::endl;
            }
            if (hash_baseline) return report.mature_samples == 512 && report.failure.empty() && !failures ? 0 : 2;
            if (human_encounter_only) return report.stop_reason == "unattended_human_encounter_advanced" && report.failure.empty() ? 0 : 2;
            return (benchmark ? report.passed && floors : report.correctness) && !failures ? 0 : 2;
        } catch (const std::exception& error) {
            std::ofstream refused(directory / "result.json", std::ios::trunc);
            refused << "{\"release_gate_passed\":false,\"run_started\":" << (initialized ? "true" : "false") << ",\"failure\":";
            galactic_campaign_gate::json_string(refused, error.what()); refused << "}\n"; refused.close();
            std::cerr << error.what() << '\n'; return 2;
        }
    }
    const auto birth_services = must(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default"));
    installed_world_phase_births(plan, birth_services, *setup, selected.front().player);
    eawr::platform::CampaignServiceConfiguration configuration;
    const auto configured = cp::configure_galactic_ai_runtime(configuration, setup, {});
    expect(!configured && configured.error().code == "EAWR-GCAI-RUNTIME-0001" && !configuration.configure_native_ai,
        "unavailable runtime refuses without publishing a half configuration");
    std::cout << "{\"campaign\":\"" << campaign << "\",\"seed\":42,\"difficulty\":\"NORMAL\","
        "\"players\":[\"BASICEMPIRE\",\"BASICREBEL\"],\"mode_fps\":30,\"game_seconds_limit\":900,"
        "\"immediate_instances\":4096,\"immediate_requests\":4096,\"closure\":\"" << setup->content_identity
        << "\",\"release_gate_passed\":false,\"run_started\":false,\"builds\":0,\"expansions\":0,"
        "\"battles\":0,\"heroes\":0,\"terminal_result\":null,\"live_save_boundaries_verified\":0}"
        << '\n' << configured.error().message << '\n';
    // No tick limit, forced victory or empty campaign can pass the release gate.
    return failures ? 1 : 0;
}

// The installed Equal Footing campaign as the viewer lays it out: every faction a seat, one human, the rest AI.
struct InstalledLive final {
    std::shared_ptr<const eawr::vfs::Vfs> files;
    eawr::data::LoadResult loaded;
    eawr::data::galactic::GalacticData data;
    eawr::units::GalacticTypeTable types;
    std::string campaign;
    cp::CampaignSetupOptions setup;
    galactic::SeatId human{};
    std::set<galactic::PlayerId> ai_players;
    static std::shared_ptr<const eawr::vfs::Vfs> mount_installed(const std::string& mod_root) {
        auto layers = mod_root.empty() ? std::vector<eawr::vfs::LayerRoot>{} : eawr::vfs::mod_chain_roots(mod_root);
        layers.emplace_back("expansion", game_root() / "corruption" / "Data");
        layers.emplace_back("base", game_root() / "GameData" / "Data");
        auto chain = must(eawr::vfs::resolve_manifest_chain(layers));
        std::vector<eawr::vfs::MountSpec> mounts;
        for (auto& manifest : chain) mounts.push_back(std::move(manifest.mount));
        return std::make_shared<const eawr::vfs::Vfs>(must(eawr::vfs::Vfs::mount(mounts)));
    }
    explicit InstalledLive(std::string mod_root = {}, std::string campaign_name = "Sandbox_Equal_Footing_Underworld")
        : files(mount_installed(mod_root)), loaded(must(eawr::data::load_catalog(*files,
              mod_root.empty() ? eawr::data::Profile::foc : eawr::data::Profile::remake))),
        data(must(eawr::data::galactic::load_galactic_data(*files, loaded.catalog))),
        types(eawr::units::galactic_type_table(loaded.catalog)), campaign(std::move(campaign_name)) {
        const auto* source = data.campaign(campaign);
        if (!source) throw std::runtime_error("installed live AI campaign missing");
        setup.seed = 42;
        for (const auto& faction : data.factions) {
            const auto id = static_cast<galactic::PlayerId>(setup.players.size() + 1);
            const bool local = faction.name == source->starting_active_player;
            const bool playable = faction.is_playable.value_or(false);
            setup.players.push_back({{id, id, 0}, faction.name, true});
            // Match the viewer's full seat layout, including its neutral/nonplayable
            // AI metadata. Only playable opponents supply the progression witnesses.
            setup.seats.push_back({galactic::SeatId{id}, id, local ? galactic::SeatKind::human : galactic::SeatKind::ai, id - 1, true, local ? id : 0});
            if (local) { setup.solo_reference_human = id; human = {id}; }
            else if (playable) ai_players.insert(id);
        }
        expect(human.value != 0 && !ai_players.empty(), "installed production case has actual human and AI controllers");
        std::vector<std::string> originals;
        for (const auto& type : types.types) originals.push_back(type.name);
        std::sort(originals.begin(), originals.end());
        for (const auto& name : originals) setup.types.push_back({setup.types.size() + 1, name});
    }
    [[nodiscard]] auto prepare(bool install_native_ai, ga::ServicePeriods periods = ga::retail_service_periods, bool observe = false) const {
        return must(eawr::platform::PreparedLiveCampaign::create(files, loaded.catalog, data, types, campaign, setup, "Normal_Default", install_native_ai, periods, observe));
    }
};
struct ProfileEvidence final {
    std::vector<std::vector<std::string>> hashes{};
    std::vector<std::string> galaxy_sha{};
    // Count prepass builds through the existing opt-in profile scopes.
    std::uint64_t path_tables{};
};
void check_path_table_prepass(const ProfileEvidence& evidence, bool expect_tables) {
    std::cout << "GC-AI-PATH-TABLES path_tables_built=" << evidence.path_tables << std::endl;
    if (expect_tables) expect(evidence.path_tables > 0, "the path-table prepass built the refreshed tables");
}
int live_production(const std::filesystem::path& directory, std::uint64_t frames, bool performance_only,
    bool cadence_window = false, ga::ServicePeriods periods = ga::retail_service_periods,
    ProfileEvidence* evidence = nullptr, bool short_record_contract = false) {
    using Live = eawr::platform::LiveGameSession;
    if (!frames) throw std::runtime_error("live production probe requires nonzero frames");
    if (cadence_window && frames < (short_record_contract ? 32U : 600U))
        throw std::runtime_error("cadence proof requires at least two perception deadlines");
    const bool profiling = eawr::gc_profile::enabled.exchange(false, std::memory_order_relaxed);
    const InstalledLive fixture;
    const auto& files = fixture.files;
    const auto& loaded = fixture.loaded;
    const auto& data = fixture.data;
    const auto& types = fixture.types;
    const auto campaign = fixture.campaign;
    const auto& setup = fixture.setup;
    const auto human = fixture.human;
    const auto& ai_players = fixture.ai_players;
    const auto prepare = [&](bool install_native_ai) { return fixture.prepare(install_native_ai, periods); };
    const auto advance = [](Live& live, std::uint64_t tick) {
        live.advance_to(tick);
        if (!live.wait_for(tick, std::chrono::minutes(30))) {
            const auto error = live.failure();
            throw std::runtime_error(error ? eawr::core::format_diagnostic(*error) : "live AI frame timed out");
        }
    };
    // Native AI capabilities must not constrain ordinary (default-OFF) mod starts.
    for (const bool missing_fog : {true, false}) {
        auto modified = data;
        if (missing_fog) {
            std::erase_if(modified.constants, [](const auto& row) { return row.tag == "AIUsesFogOfWarGalactic"; });
            expect(!modified.constant("AIUsesFogOfWarGalactic"), "mod fixture removes native AI fog policy");
        } else {
            bool changed{};
            for (auto& authored : modified.campaigns) {
                if (authored.name != campaign) continue;
                for (auto& assignment : authored.ai_player_control) {
                    const auto player = std::find_if(setup.players.begin(), setup.players.end(), [&](const auto& row) {
                        return eawr::script::foc::ai::upper_case(row.faction) == eawr::script::foc::ai::upper_case(assignment.faction)
                            && ai_players.contains(row.player.player_id);
                    });
                    if (player == setup.players.end()) continue;
                    assignment.name = "EAWR_UNMAPPED_NATIVE_PLAYER"; changed = true; break;
                }
            }
            expect(changed, "mod fixture removes one active native AI player mapping");
        }
        auto off = must(eawr::platform::PreparedLiveCampaign::create(files, loaded.catalog, modified, types,
            campaign, setup, "Normal_Default"));
        Live::Options off_options; off_options.pacing = Live::Pacing::driven;
        auto running = must(Live::start(std::move(off), off_options));
        advance(*running, 1);
        expect(!running->failure(), "default-OFF campaign starts and steps without native AI inputs");
        running->stop();
        auto on = eawr::platform::PreparedLiveCampaign::create(files, loaded.catalog, modified, types,
            campaign, setup, "Normal_Default", true, periods);
        expect(!on && on.error().message.find(missing_fog ? "fog policy" : "authored AI player type") != std::string::npos,
            "explicit native AI opt-in clearly refuses its missing authored input");
        std::cout << "native AI off/on missing " << (missing_fog ? "fog policy" : "AI player mapping") << " checked\n";
    }
    const auto measure = [&](Live& live, std::vector<eawr::gc_profile::Snapshot>& phases,
        const std::function<void()>& midpoint = {}, const std::function<void()>& after_frame = {}) {
        std::vector<double> samples;
        for (std::uint64_t index = 0; index < frames; ++index) {
            const auto phase_begin = profiling ? eawr::gc_profile::snapshot() : eawr::gc_profile::Snapshot{};
            eawr::gc_profile::enabled.store(profiling, std::memory_order_relaxed);
            const auto begin = std::chrono::steady_clock::now();
            advance(live, live.completed_tick() + 1);
            samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
            eawr::gc_profile::enabled.store(false, std::memory_order_relaxed);
            if (profiling) phases.push_back(eawr::gc_profile::difference(phase_begin, eawr::gc_profile::snapshot()));
            if (after_frame) after_frame();
            if (midpoint && index + 1 == frames / 2) midpoint();
        }
        return samples;
    };
    const auto mean = [](const auto& samples) { return std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size()); };
    // Nearest-rank percentiles summarize each independent run without pairing frames.
    const auto percentile = [](auto samples, std::size_t percent) {
        std::sort(samples.begin(), samples.end());
        return samples[(samples.size() * percent + 99) / 100 - 1];
    };
    std::filesystem::create_directories(directory);
    std::ofstream report(directory / "live-production.json", std::ios::trunc);
    report << "{\"frames\":" << frames << ",\"cadence_window\":" << (cadence_window ? "true" : "false") << ",\"periods\":[";
    for (std::size_t i = 0; i < periods.size(); ++i) report << (i ? "," : "") << periods[i];
    report << "],\"runs\":[";
    std::vector<std::string> reference;
    std::vector<galactic::GalacticEvent> reference_events;
    for (const std::size_t workers : std::vector<std::size_t>{1U, 2U, 4U, 8U}) {
        Live::Options options; options.workers = workers; options.pacing = Live::Pacing::driven;
        options.saves.directory = (directory / ("w" + std::to_string(workers))).string();
        // Same initialized galaxy and ordinary campaign services; native AI
        // preparation and installation are off. Delta includes AI-caused sim work.
        auto baseline = must(Live::start(prepare(false), options));
        advance(*baseline, 1);
        std::vector<eawr::gc_profile::Snapshot> baseline_phases;
        const auto baseline_samples = measure(*baseline, baseline_phases, cadence_window
            ? std::function<void()>{[&] { advance(*baseline, baseline->completed_tick() + 1); }} : std::function<void()>{});
        const auto baseline_ms = mean(baseline_samples);
        const auto baseline_p50 = percentile(baseline_samples, 50);
        const auto baseline_p95 = percentile(baseline_samples, 95);
        baseline->stop();
        auto prepared_live = prepare(true);
        const auto live_layout = prepared_live->layout();
        const auto prepared_snapshot = prepared_live->snapshot();
        auto live = must(Live::start(std::move(prepared_live), options));
        expect(live->frame().latest == prepared_snapshot,
            "production AI is installed before prepared snapshot publication and owner transfer");
        std::vector<galactic::GalacticEvent> events;
        std::uint64_t event_cursor{};
        std::uint64_t last_save_token{};
        std::string boundary_galaxy_sha;
        const auto collect = [&] {
            const auto rows = live->galactic_events_after(event_cursor);
            expect(!rows.incomplete, "cadence proof drains each committed event before bounded retention expires");
            for (const auto& row : rows.events) events.push_back(row.event);
            event_cursor = rows.through;
        };
        const auto capture = [&](std::string name) {
            eawr::platform::LiveSaveRequest request; request.seat = human; request.name = name; request.overwrite = true;
            const auto tick = live->completed_tick() + 1;
            const auto token = live->request_save(std::move(request));
            last_save_token = token;
            advance(*live, tick);
            if (!live->wait_save(token, std::chrono::minutes(5))) throw std::runtime_error("live AI save timed out");
            const auto outcome = live->terminal_save_outcome(token);
            if (!outcome || outcome->phase != eawr::platform::LiveSavePhase::completed) throw std::runtime_error("live AI save failed");
            collect();
            auto saved = must(ck::parse_checkpoint(must(eawr::platform::save_detail::read(options.saves.directory, name))));
            const auto* section = saved.find(ck::SectionId::galactic_ai);
            if (!section) throw std::runtime_error("production save missing native AI section");
            const auto ai = must(cp::decode_galactic_ai_save(section->bytes));
            if (!ai.state) throw std::runtime_error("production startup did not install native galactic AI");
            expect(ai.state->components.contains(std::string(ga::lifecycle_component)), "production native owner includes galactic services");
            if (name == "boundary" && evidence) {
                const auto* owner = saved.find(ck::SectionId::galactic_owner);
                if (!owner) throw std::runtime_error("profile equality needs the direct owner checkpoint");
                // Reconstruct only the existing owner payload, outside the timed
                // window, to read canonical galaxy SHA without a new live API.
                // The owner keeps only the journal's length: the session section's recording supplies its entries (#4058).
                const auto* recorded = saved.find(ck::SectionId::session);
                if (!recorded) throw std::runtime_error("profile equality needs the session recording");
                ck::detail::Reader context(recorded->bytes);
                const auto prefix = ck::detail::read_session_prefix(context);
                if (context.failed()) throw std::runtime_error("profile equality needs a valid session prefix");
                // The recording and the owner reference their production offer tables in the checkpoint's offer pool.
                const auto* pool_section = saved.find(ck::SectionId::offer_pool);
                if (!pool_section) throw std::runtime_error("profile equality needs the offer pool");
                auto offer_pool = galactic::OfferPool::decode(pool_section->bytes);
                if (!offer_pool) throw std::runtime_error("profile equality needs a valid offer pool");
                const auto replay = must(galactic::parse_replay(prefix.replay, {}, &*offer_pool));
                // The campaign owner leads with its format version and the galactic owner; only the galactic part is read.
                // The recording's window follows the history already dropped by earlier saves, so it is staged on that base.
                ck::detail::Reader campaign_owner(owner->bytes);
                (void)campaign_owner.u32();
                const auto galactic_owner = campaign_owner.take(static_cast<std::size_t>(campaign_owner.u64()));
                if (campaign_owner.failed()) throw std::runtime_error("profile equality needs a valid campaign owner");
                auto imported = must(galactic::GalacticSession::create(replay.setup));
                must(imported.stage_native_replay(replay.native_effects, replay.final_tick_count, prefix.base.native_dropped));
                must(imported.load_checkpoint_state(galactic_owner, &*offer_pool));
                boundary_galaxy_sha = imported.state_sha256();
            }
            return *ai.state;
        };
        const auto initial_ai = capture("initial");
        if (workers == 1) {
            auto off = must(Live::start(prepare(false), options));
            const auto refused = off->prepare_load("initial", false);
            expect(!refused && refused.error().message.find("native AI section needs a fresh state contributor") != std::string::npos,
                "native AI checkpoint clearly requires opt-in when loading through an OFF owner");
            off->stop();
        }
        std::unique_ptr<eawr::platform::PreparedLiveRestore> midpoint_restore;
        std::vector<eawr::gc_profile::Snapshot> live_phases;
        const auto live_samples = measure(*live, live_phases, cadence_window ? std::function<void()>{[&] {
            capture("midwindow");
            midpoint_restore = must(live->prepare_load("midwindow", false));
        }} : std::function<void()>{}, collect);
        const auto live_ms = mean(live_samples);
        const auto live_p50 = percentile(live_samples, 50);
        const auto live_p95 = percentile(live_samples, 95);
        std::cout << "live production workers=" << workers << " frames=" << frames << " baseline_ms_per_frame=" << baseline_ms
            << " live_ms_per_frame=" << live_ms << " added_ai_ms_per_frame=" << live_ms - baseline_ms
            << " baseline_p50_ms=" << baseline_p50 << " baseline_p95_ms=" << baseline_p95
            << " live_p50_ms=" << live_p50 << " live_p95_ms=" << live_p95
            << " added_ai_p50_ms=" << live_p50 - baseline_p50 << " added_ai_p95_ms=" << live_p95 - baseline_p95
            << " fps=" << 1000.0 / live_ms << std::endl;
        std::size_t builds{}, movements{}, conflicts{};
        const auto snapshot = live->frame().latest;
        const auto owner_of = [&](const galactic::GalacticEvent& event) {
            if (event.kind == galactic::GalacticEventKind::production_started) {
                for (const auto& planet : snapshot->planets()) if (planet.id == event.subject) return planet.owner;
            } else {
                for (const auto& fleet : snapshot->fleets()) if (fleet.id == event.subject) return fleet.owner;
            }
            return galactic::PlayerId{};
        };
        for (const auto& row : live->galactic_events_after(0).events) {
            if (row.event.kind == galactic::GalacticEventKind::conflict_pending) ++conflicts;
            if (ai_players.contains(owner_of(row.event))) {
                if (row.event.kind == galactic::GalacticEventKind::production_started) ++builds;
                if (row.event.kind == galactic::GalacticEventKind::movement_begin) ++movements;
            }
        }
        if (!performance_only) {
            expect(builds > 0, "authored AI seat issues real builds through production startup within N frames");
            expect(movements > 0, "authored AI seat issues free movement through production startup within N frames");
        }
        const auto saved_ai = capture("boundary");
        const auto final_save_tick = live->completed_tick();
        const auto final_save_token = last_save_token;
        expect(initial_ai.components.at(std::string(ga::lifecycle_component)) != saved_ai.components.at(std::string(ga::lifecycle_component)),
            "production galactic service clocks/deadlines advance");
        auto restored = cadence_window ? std::move(midpoint_restore) : must(live->prepare_load("boundary", false));
        const auto boundary = restored->completed_tick();
        const auto end = live->completed_tick() + 12;
        advance(*live, end);
        collect();
        live->stop();
        const auto hashes = live->tick_hashes();
        if (evidence) { evidence->hashes.push_back(hashes); evidence->galaxy_sha.push_back(boundary_galaxy_sha); }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "production live AI agrees at every frame on 1/2/4/8 workers");
        if (workers == 1) reference_events = events;
        else expect(events == reference_events, "production live AI commits identical events on 1/2/4/8 workers");
        auto restored_options = options;
        if (cadence_window) restored_options.workers = 8 / workers;
        auto resumed = must(Live::start(std::move(restored), restored_options));
        std::vector<galactic::GalacticEvent> resumed_events;
        std::uint64_t resumed_cursor{};
        for (auto tick = boundary + 1; tick <= end; ++tick) {
            // Save requests are recorded orders: continue with identical input
            // on both branches, including the control's final proof checkpoint.
            std::optional<std::uint64_t> paired_save;
            if (cadence_window && tick == final_save_tick) {
                eawr::platform::LiveSaveRequest request; request.seat = human; request.name = "boundary"; request.overwrite = true;
                paired_save = resumed->request_save(std::move(request));
                expect(*paired_save == final_save_token, "restored proof uses the same recorded save token");
            }
            advance(*resumed, tick);
            if (paired_save) {
                if (!resumed->wait_save(*paired_save, std::chrono::minutes(5))) throw std::runtime_error("paired continuation save timed out");
                const auto outcome = resumed->terminal_save_outcome(*paired_save);
                if (!outcome || outcome->phase != eawr::platform::LiveSavePhase::completed) throw std::runtime_error("paired continuation save failed");
            }
            const auto rows = resumed->galactic_events_after(resumed_cursor);
            expect(!rows.incomplete, "restored cadence proof drains each continuation event");
            for (const auto& row : rows.events) if (row.event.tick > boundary) resumed_events.push_back(row.event);
            resumed_cursor = rows.through;
        }
        resumed->stop();
        const auto tail = resumed->tick_hashes();
        expect(tail.size() == end - boundary && hashes.size() >= tail.size()
            && std::equal(tail.begin(), tail.end(), hashes.end() - static_cast<std::ptrdiff_t>(tail.size())),
            "production save/load reconstructs fresh AI and preserves uninterrupted continuation");
        if (cadence_window) {
            std::vector<galactic::GalacticEvent> control_events;
            for (const auto& event : events) if (event.tick > boundary) control_events.push_back(event);
            expect(resumed_events == control_events, "midwindow restore on different workers preserves committed orders/events across perception deadlines");
        }
        report << (workers == 1 ? "" : ",") << "{\"workers\":" << workers << ",\"baseline_ms_per_frame\":" << baseline_ms
            << ",\"live_ms_per_frame\":" << live_ms << ",\"added_ai_ms_per_frame\":" << live_ms - baseline_ms
            << ",\"baseline_p50_ms\":" << baseline_p50 << ",\"baseline_p95_ms\":" << baseline_p95
            << ",\"live_p50_ms\":" << live_p50 << ",\"live_p95_ms\":" << live_p95
            << ",\"added_ai_p50_ms\":" << live_p50 - baseline_p50 << ",\"added_ai_p95_ms\":" << live_p95 - baseline_p95
            << ",\"fps\":" << 1000.0 / live_ms
            << ",\"restored_workers\":" << restored_options.workers << ",\"restore_boundary\":" << boundary << ",\"restore_end\":" << end
            << ",\"builds\":" << builds << ",\"movement_requests\":" << movements << ",\"conflicts\":" << conflicts;
        if (profiling) {
            if (evidence) for (const auto& frame : live_phases)
                evidence->path_tables += frame[static_cast<std::size_t>(eawr::gc_profile::Phase::path_table_prepass)].calls;
            report << ",\"ai_compute_budget\":";
            eawr::gc_profile::write_budget(report, live_phases);
            std::cout << "GC-AI-BUDGET workers=" << workers << ' ';
            eawr::gc_profile::write_budget(std::cout, live_phases);
            std::cout << std::endl;
            report << ",\"profile_frames\":[";
            for (std::size_t index = 0; index < live_phases.size(); ++index) {
                // Cadence sampling skips initial and midpoint checkpoint frames.
                const auto tick = index + 2 + (cadence_window && index >= frames / 2 ? 1 : 0);
                report << (index ? "," : "");
                eawr::gc_profile::write_frame(report, live_phases[index], tick);
                std::cout << "GC-AI-PROFILE-FRAME workers=" << workers << ' ';
                eawr::gc_profile::write_frame(std::cout, live_phases[index], tick);
                std::cout << '\n';
            }
            report << ']';
        }
        report << ",\"hashes\":[";
        for (std::size_t index = 0; index < hashes.size(); ++index)
            report << (index ? "," : "") << '"' << hashes[index] << '"';
        report << "],\"canonical_galaxy_sha_at_boundary\":\"" << boundary_galaxy_sha << "\"}";
        std::cout << "GC-AI-PROFILE-GALAXY workers=" << workers << " sha=" << boundary_galaxy_sha << std::endl;
        // One line per worker count that a later head can be compared with: SHA-256 over every live tick hash in order.
        std::string joined;
        for (const auto& hash : hashes) joined += hash;
        std::cout << "GC-AI-TICK-HASHES workers=" << workers << " ticks=" << hashes.size() << " sha256="
            << eawr::core::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(joined.data()), joined.size())) << std::endl;
        report.flush();
    }
    report << "],\"passed\":" << (failures ? "false" : "true") << "}\n";
    return failures ? 1 : 0;
}
// GC tick cost (#4049): the live galactic step stepped one tick at a time, AI off and on, with the per-tick wall time
// the performance overlay reads and the gc_profile phase attribution of every tick. Diagnostics only: nothing here
// feeds the simulation, and the tick hashes are written out so a later head can be compared with BASE.
std::vector<std::string> split_list(std::string_view text) {
    std::vector<std::string> parts;
    while (!text.empty()) {
        const auto cut = text.find(',');
        parts.emplace_back(text.substr(0, cut));
        text = cut == std::string_view::npos ? std::string_view{} : text.substr(cut + 1);
    }
    return parts;
}
std::string environment_or(const char* name, std::string fallback) {
#ifdef _MSC_VER
    char* value{}; std::size_t size{};
    if (_dupenv_s(&value, &size, name) || !value) return fallback;
    std::string text(value); std::free(value); return text.empty() ? fallback : text;
#else
    const auto* value = std::getenv(name);
    return value && *value ? std::string(value) : fallback;
#endif
}
// #5519: the lockstep exchange encoding equals the serial canonical encoding on the installed stock campaign (real
// production offer tables, census and fleets) at every 30th tick, for 1, 2, 4 and 8 workers.
int exchange_encode(const std::uint64_t ticks) {
    const InstalledLive fixture;
    auto prepared = fixture.prepare(false);
    auto& game = eawr::platform::AgreedCampaignAccess::session(*prepared);
    const eawr::platform::ThreadWorkerAdapter stepping(1);
    std::uint64_t compared = 0;
    for (std::uint64_t i = 0; i < ticks; ++i) {
        static_cast<void>(must(game.step(stepping)));
        if (game.completed_tick() % 30 != 0) continue;
        const auto serial = game.galaxy().canonical_state_bytes();
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(game.galaxy().exchange_state_bytes(executor) == serial,
                "exchange encoding equals the serial encoding at " + std::to_string(workers) + " workers, tick " + std::to_string(game.completed_tick()));
        }
        ++compared;
    }
    expect(compared > 0, "at least one exchange tick was compared");
    return failures ? 1 : 0;
}
// Workshop folder of a mod id, searched like the quality sweep does; empty when this host does not have it.
std::string workshop_mod_folder(const std::string& id) {
    std::vector<std::filesystem::path> roots;
    if (const auto configured = environment_or("EAWR_WORKSHOP_ROOT", ""); !configured.empty()) roots.emplace_back(configured);
    const auto installed_root = game_root();
    roots.push_back(installed_root.parent_path().parent_path() / "workshop" / "content" / "32470");
    roots.push_back(installed_root.parent_path() / "workshop" / "content" / "32470");
    roots.push_back(installed_root.parent_path().parent_path() / "mods" / "workshop" / "content" / "32470");
    for (const auto& base : roots) {
        std::error_code unreadable;
        if (std::filesystem::is_directory(base / id, unreadable) && !unreadable) return (base / id).string();
    }
    return {};
}
// The installed layered campaign must save in solo and co-op and restore its direct owner.
int remake_save_probe(std::uint64_t ticks) {
    const auto leaf = environment_or("EAWR_REMAKE_MOD_ROOT", "");
    if (leaf.empty()) { std::cout << "SKIPPED: Remake mod host required\n"; return 77; }
    const auto workshop = std::filesystem::path(leaf).parent_path();
    const auto chain = (workshop / "3229239424").string() + ";" + (workshop / "2794270450").string() + ";" + leaf;
    bool failed = false;
    for (const bool coop : {false, true}) {
        InstalledLive fixture(chain, "GC_Mini_Web_Way_Rebel");
        if (coop) {
            fixture.setup.mode = cp::CampaignSetupMode::multiplayer;
            fixture.setup.rules_flavour = cp::CampaignRulesFlavour::single_player;
            fixture.setup.solo_reference_human.reset();
            auto teammate = *std::find_if(fixture.setup.seats.begin(), fixture.setup.seats.end(),
                [](const auto& seat) { return seat.kind == galactic::SeatKind::human; });
            teammate.seat_id = {999}; teammate.lobby_slot = static_cast<std::uint32_t>(fixture.setup.seats.size());
            fixture.setup.seats.push_back(teammate);
        }
        auto prepared = fixture.prepare(false);
        auto& game = eawr::platform::AgreedCampaignAccess::session(*prepared);
        const eawr::platform::ThreadWorkerAdapter executor(1);
        for (std::uint64_t tick = 0; tick <= ticks; ++tick) {
            if (tick == 0 || tick == ticks) {
                std::cout << "SAVE-PROBE mode=" << (coop ? "coop" : "solo") << " tick=" << tick << std::endl;
                auto state = game.save_checkpoint_state();
                if (!state) { std::cout << "OWNER ERROR " << state.error().message << std::endl; failed = true; }
                else std::cout << "OWNER bytes=" << state.value().size() << std::endl;
                const auto save_begin = std::chrono::steady_clock::now();
                auto saved = ck::save_game_checkpoint(game, ck::game_identities(game.galaxy().setup()));
                const auto save_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - save_begin).count();
                if (!saved) { std::cout << "SAVE ERROR " << saved.error().message << std::endl; failed = true; }
                else {
                    // The size per section, so the native AI section and the one offer pool are visible in the log.
                    if (const auto container = ck::parse_checkpoint(saved.value()))
                        for (const auto& section : container.value().sections)
                            std::cout << "CHECKPOINT-SECTION " << section.id << " " << ck::section_name(section.id) << " " << section.bytes.size() << std::endl;
                    std::cout << "SAVE bytes=" << saved.value().size() << " capture_ms=" << save_ms << std::endl;
                    // Each planet's offer table is stored once (the offer pool), so the layered campaign's save stays well
                    // below the ~225 MB it took when the setup, production state and owner barrier each carried every table.
                    expect(saved.value().size() < 80U * 1024U * 1024U, "the layered campaign's checkpoint stores its offer tables once");
                    if (coop && tick == ticks) {
                        const auto expected = game.state_sha256();
                        auto plan = prepared->plan();
                        auto second_plan = prepared->plan();
                        prepared.reset();
                        // Restore alone, twice in the same run, so a before/after comparison is not one noisy sample.
                        const auto restore_once = [&](auto restore_plan, const char* label) {
                            const auto restore_begin = std::chrono::steady_clock::now();
                            auto restored = must(eawr::platform::PreparedLiveCampaign::from_plan(fixture.files, fixture.loaded.catalog,
                                fixture.data, fixture.types, std::move(restore_plan), "Normal_Default", false, {}, false, saved.value()));
                            std::cout << "RESTORE " << label << " ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - restore_begin).count() << std::endl;
                            expect(eawr::platform::AgreedCampaignAccess::session(*restored).state_sha256() == expected,
                                "large co-op checkpoint restores the saved game state");
                        };
                        restore_once(std::move(plan), "first");
                        restore_once(std::move(second_plan), "second");
                    }
                }
            }
            if (tick < ticks) must(game.step(executor));
        }
    }
    return failed || failures ? 1 : 0;
}
// Process CPU time (user + kernel, all threads) in ms, for CPU% of the logical processors next to ms/tick.
double process_cpu_ms() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    const auto to_ms = [](const FILETIME& t) { return static_cast<double>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 1e4; };
    return to_ms(kernel) + to_ms(user);
#else
    return 1000.0 * static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
}
// Command-line form of the EAWR_TICK_PROFILE_* environment, for pool jobs that cannot set it: modes and workers are
// comma lists, "hw" in the workers list is std::thread::hardware_concurrency(). The environment still wins.
int tick_profile(std::filesystem::path directory, std::uint64_t ticks, std::string default_modes = "off,on",
    std::string default_workers = "1") {
    using Live = eawr::platform::LiveGameSession;
    namespace gp = eawr::gc_profile;
    directory = environment_or("EAWR_TICK_PROFILE_DIR", directory.string());
    ticks = std::stoull(environment_or("EAWR_TICK_PROFILE_TICKS", std::to_string(ticks)));
    const auto modes = split_list(environment_or("EAWR_TICK_PROFILE_MODES", default_modes));
    auto worker_counts = split_list(environment_or("EAWR_TICK_PROFILE_WORKERS", default_workers));
    for (auto& worker_text : worker_counts)
        if (worker_text == "hw") worker_text = std::to_string(std::max(1u, std::thread::hardware_concurrency()));
    std::map<std::string, std::uint64_t> mode_digest;
    std::filesystem::create_directories(directory);
    std::string profile_mod_root = environment_or("EAWR_TICK_PROFILE_MOD_ROOT", "");
    if (const auto mod_id = environment_or("EAWR_TICK_PROFILE_MOD_ID", ""); profile_mod_root.empty() && !mod_id.empty()) {
        profile_mod_root = workshop_mod_folder(mod_id);
        if (profile_mod_root.empty()) { std::cout << "SKIPPED: workshop mod " << mod_id << " is not installed on this host" << std::endl; return 77; }
    }
    // Statistics (not the csv) cover only ticks from this offset on, so the opening can be left out (steady state).
    const std::uint64_t stats_from = std::stoull(environment_or("EAWR_TICK_PROFILE_STATS_FROM", "0"));
    const InstalledLive fixture(profile_mod_root,
        environment_or("EAWR_TICK_PROFILE_CAMPAIGN", "Sandbox_Equal_Footing_Underworld"));
    constexpr auto phase_count = static_cast<std::size_t>(gp::Phase::count);
    constexpr auto cpu_begin = static_cast<std::size_t>(gp::Phase::perception);
    for (const auto& mode : modes) for (const auto& worker_text : worker_counts) {
        const std::size_t workers = std::stoull(worker_text);
        Live::Options options; options.workers = workers; options.pacing = Live::Pacing::driven;
        options.saves.directory = (directory / ("saves-" + mode + "-w" + worker_text)).string();
        auto live = must(Live::start(fixture.prepare(mode == "on"), options));
        // The paused galaxy never steps on its own (#5518): an AI invasion of the human seat parks the session on a
        // ready conflict choice that a presentation would answer. Answer it once per conflict by resolving, as a
        // player pressing Resolve, and report any other wait by name instead of timing out on the tick.
        eawr::sim::EntityId answered_conflict{};
        const auto advance = [&](std::uint64_t tick) {
            using Clock = std::chrono::steady_clock;
            live->advance_to(tick);
            const auto began = Clock::now();
            while (!live->wait_for(tick, std::chrono::milliseconds(500))) {
                if (const auto error = live->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
                const auto elapsed = Clock::now() - began;
                const auto frame = live->frame();
                const auto waiting = eawr::platform::awaited_input(frame);
                if (elapsed > std::chrono::minutes(30))
                    throw std::runtime_error("tick profile frame timed out at tick " + std::to_string(tick) + (waiting ? ": " + waiting->describe() : std::string()));
                if (!waiting) continue;
                if (waiting->kind == eawr::platform::LiveAwaitedInput::Kind::conflict_choice && waiting->conflict != answered_conflict && frame.latest) {
                    bool answered = false;
                    for (const auto& seat : frame.latest->seats()) {
                        if (const auto order = eawr::platform::battle_choice_order(frame, seat.seat_id, waiting->conflict, false)) {
                            live->submit(*order);
                            answered_conflict = waiting->conflict;
                            answered = true;
                            const auto& conflict = frame.pending_conflict->conflict;
                            std::cerr << "TICK-PROFILE-ANSWER tick=" << tick << " session_tick=" << frame.session_tick << " conflict=" << conflict.id
                                << " layer=" << (conflict.layer == eawr::campaign::BattleLayer::land ? "land" : "space") << " planet=" << conflict.planet
                                << " invader=" << conflict.invader << " defender=" << conflict.defender << " human_seat=" << seat.seat_id.value
                                << " pause_depth=" << frame.latest->clock().pause_depth << std::endl;
                            break;
                        }
                    }
                    if (answered) continue; // otherwise a forced or unadmitted choice falls through to the named failure below
                }
                if (elapsed > std::chrono::seconds(20))
                    throw std::runtime_error("tick profile cannot publish tick " + std::to_string(tick) + ": " + waiting->describe());
            }
        };
        advance(1);
        struct Row { std::uint64_t tick{}; double wall_ms{}, step_ms{}; gp::Snapshot phases{}; std::string snapshot_hash; };
        const bool state_digest = environment_or("EAWR_TICK_PROFILE_STATE_DIGEST", "0") == "1";
        std::vector<Row> rows;
        rows.reserve(ticks);
        const bool was_enabled = gp::enabled.exchange(true, std::memory_order_relaxed);
        double cpu_begin_ms = process_cpu_ms(), cpu_end_ms = cpu_begin_ms;
        for (std::uint64_t index = 0; index < ticks; ++index) {
            if (index == stats_from) cpu_begin_ms = process_cpu_ms();
            const auto tick = live->completed_tick() + 1;
            const auto before = gp::snapshot();
            const auto begin = std::chrono::steady_clock::now();
            advance(tick);
            Row row; row.tick = tick;
            row.wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            row.phases = gp::difference(before, gp::snapshot());
            for (const auto& cost : live->tick_costs_after(tick - 1)) if (cost.tick == tick) row.step_ms = cost.total_ms;
            // Outside the timing interval: the full snapshot codec is independent of the tick hash format.
            if (state_digest) row.snapshot_hash = live->frame().latest->sha256();
            rows.push_back(row);
        }
        cpu_end_ms = process_cpu_ms();
        gp::enabled.store(was_enabled, std::memory_order_relaxed);
        const auto label = mode + "-w" + worker_text;
        if (state_digest) {
            // A direct owner checkpoint and native AI state prove simulation equality across hash domains.
            // Save after the timed window; no cached section digest is part of these owner payloads.
            eawr::platform::LiveSaveRequest request;
            request.seat = fixture.human; request.name = "state-equality"; request.overwrite = true;
            const auto token = live->request_save(std::move(request));
            advance(live->completed_tick() + 1);
            if (!live->wait_save(token, std::chrono::minutes(5))) throw std::runtime_error("profile state save timed out");
            const auto outcome = live->terminal_save_outcome(token);
            if (!outcome || outcome->phase != eawr::platform::LiveSavePhase::completed)
                throw std::runtime_error("profile state save failed");
            namespace ck = eawr::checkpoint;
            const auto saved = must(ck::parse_checkpoint(must(eawr::platform::save_detail::read(
                options.saves.directory, "state-equality"))));
            std::ofstream evidence(directory / ("state-" + label + ".txt"));
            for (auto id : {ck::SectionId::galactic_owner, ck::SectionId::galactic_ai}) {
                const auto* section = saved.find(id);
                if (id == ck::SectionId::galactic_owner && !section)
                    throw std::runtime_error("profile state save lacks direct owner state");
                evidence << static_cast<unsigned>(id) << ','
                    << (section ? eawr::core::sha256_hex(section->bytes) : "absent") << '\n';
            }
        }
        live->stop();
        const auto hashes_all = live->tick_hashes();
        {
            std::ofstream csv(directory / ("ticks-" + label + ".csv"), std::ios::trunc);
            csv << "tick,wall_ms,step_ms";
            for (std::size_t i = 0; i < phase_count; ++i) csv << ',' << gp::names[i] << "_incl_ms," << gp::names[i] << "_excl_ms";
            csv << ",hash,snapshot_sha256\n";
            for (const auto& row : rows) {
                csv << row.tick << ',' << row.wall_ms << ',' << row.step_ms;
                for (const auto& value : row.phases) csv << ',' << static_cast<double>(value.inclusive_ns) / 1e6 << ',' << static_cast<double>(value.exclusive_ns) / 1e6;
                csv << ',' << (row.tick >= 1 && row.tick <= hashes_all.size() ? hashes_all[row.tick - 1] : std::string())
                    << ',' << row.snapshot_hash << '\n';
            }
        }
        std::uint64_t digest = 1469598103934665603ULL;
        {
            std::ofstream hashes(directory / ("hashes-" + label + ".txt"), std::ios::trunc);
            for (const auto& hash : hashes_all) {
                hashes << hash << '\n';
                for (const unsigned char c : hash) digest = (digest ^ c) * 1099511628211ULL;
            }
        }
        double measured_wall_ms = 0;
        if (stats_from > 0 && stats_from < rows.size()) rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(stats_from));
        for (const auto& row : rows) measured_wall_ms += row.wall_ms;
        const auto stats = [](std::vector<double> values) {
            std::sort(values.begin(), values.end());
            const auto at = [&](std::size_t percent) { return values[(values.size() * percent + 99) / 100 - 1]; };
            return std::array<double, 5>{std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size()),
                at(50), at(95), at(99), values.back()};
        };
        std::vector<double> step, wall;
        for (const auto& row : rows) { step.push_back(row.step_ms); wall.push_back(row.wall_ms); }
        const auto step_stats = stats(step), wall_stats = stats(wall);
        // The same ticks must hash the same at every worker count (summary line for a log-less pool receipt).
        if (const auto first = mode_digest.emplace(mode, digest); first.first->second != digest) {
            std::cerr << "tick profile: " << label << " hashes differ from the first worker count of mode " << mode << "\n";
            ++failures;
        }
        const auto logical = std::max(1u, std::thread::hardware_concurrency());
        std::cout << "TICK-CPU " << label << " stats_from=" << stats_from << " ticks=" << rows.size() << " logical=" << logical
            << " workers=" << workers << " cpu_ms_per_tick=" << (cpu_end_ms - cpu_begin_ms) / static_cast<double>(rows.size())
            << " wall_ms_per_tick=" << measured_wall_ms / static_cast<double>(rows.size())
            << " cpu_percent_of_logical=" << 100.0 * (cpu_end_ms - cpu_begin_ms) / (measured_wall_ms * logical) << std::endl;
        std::cout << "TICK-PROFILE " << label << " ticks=" << rows.size() << " hash_fnv=" << digest
            << " hashes=" << hashes_all.size() << " step_ms avg/p50/p95/p99/max=" << step_stats[0] << '/' << step_stats[1]
            << '/' << step_stats[2] << '/' << step_stats[3] << '/' << step_stats[4]
            << " wall_ms avg/p50/p95/p99/max=" << wall_stats[0] << '/' << wall_stats[1] << '/' << wall_stats[2] << '/' << wall_stats[3]
            << '/' << wall_stats[4] << std::endl;
        for (std::size_t i = 0; i < phase_count; ++i) {
            std::vector<double> incl, excl;
            for (const auto& row : rows) {
                incl.push_back(static_cast<double>(row.phases[i].inclusive_ns) / 1e6);
                excl.push_back(static_cast<double>(row.phases[i].exclusive_ns) / 1e6);
            }
            const auto inclusive = stats(incl);
            if (inclusive[4] == 0.0) continue;
            const auto exclusive = stats(excl);
            std::uint64_t calls{};
            for (const auto& row : rows) calls += row.phases[i].calls;
            std::cout << "TICK-PHASE " << label << ' ' << gp::names[i] << (i >= cpu_begin ? " (worker-summed)" : "")
                << " calls/tick=" << static_cast<double>(calls) / static_cast<double>(rows.size())
                << " incl avg/p95/max=" << inclusive[0] << '/' << inclusive[2] << '/' << inclusive[4]
                << " excl avg/p95/max=" << exclusive[0] << '/' << exclusive[2] << '/' << exclusive[4] << std::endl;
        }
        // The worst ticks: where in the 300/30 cadence they sit and which wall phases carry them.
        std::vector<std::size_t> order(rows.size());
        std::iota(order.begin(), order.end(), std::size_t{});
        std::sort(order.begin(), order.end(), [&](auto a, auto b) { return rows[a].step_ms > rows[b].step_ms; });
        for (std::size_t rank = 0; rank < std::min<std::size_t>(12, order.size()); ++rank) {
            const auto& row = rows[order[rank]];
            std::vector<std::size_t> top(cpu_begin);
            std::iota(top.begin(), top.end(), std::size_t{});
            std::sort(top.begin(), top.end(), [&](auto a, auto b) { return row.phases[a].exclusive_ns > row.phases[b].exclusive_ns; });
            std::cout << "TICK-WORST " << label << " tick=" << row.tick << " mod300=" << row.tick % 300 << " mod30=" << row.tick % 30
                << " step_ms=" << row.step_ms;
            for (std::size_t k = 0; k < 4; ++k)
                std::cout << ' ' << gp::names[top[k]] << '=' << static_cast<double>(row.phases[top[k]].exclusive_ns) / 1e6;
            std::cout << std::endl;
        }
        // Mean step cost per 300-tick window, so a slow drift shows next to the spikes.
        for (std::size_t from = 0; from < rows.size(); from += 300) {
            double sum{}; std::size_t count{};
            for (std::size_t i = from; i < std::min(rows.size(), from + 300); ++i) { sum += rows[i].step_ms; ++count; }
            std::cout << "TICK-WINDOW " << label << " from_tick=" << rows[from].tick << " avg_step_ms=" << sum / static_cast<double>(count) << std::endl;
        }
    }
    return failures ? 1 : 0;
}
// Async-AI quality sweep (tools/gc_quality): one seed of one arm, compact NDJSON decision log
// (ai_quality_run.hpp). --quality-run <dir|-> <ticks> <seed> <arm> <probe> <profile> [workshop ids, ';' chain | -] [campaign | -].
// Workshop ids resolve through EAWR_WORKSHOP_ROOT or the Steam layout beside the game root; a missing
// mod folder skips the case (exit 77) so a host without the mod never reports a failure.
int quality_run(int argc, char** argv) {
    if (argc < 8) throw std::runtime_error("--quality-run <dir|-> <ticks> <seed> <arm> <probe> <profile> [mod ids|-] [campaign|-]");
    galactic_quality_run::Options options;
    options.directory = std::string_view(argv[2]) == "-" ? std::filesystem::path{} : std::filesystem::path(argv[2]);
    options.ticks = std::stoull(argv[3]);
    options.seed = std::stoull(argv[4]);
    options.arm = argv[5];
    options.probe = argv[6];
    options.profile = argv[7];
    std::string chain;
    const std::string mods = argc > 8 && std::string_view(argv[8]) != "-" ? argv[8] : "";
    std::string campaign = argc > 9 && std::string_view(argv[9]) != "-" ? argv[9] : "Sandbox_Equal_Footing_Underworld";
    // Workshop folders: EAWR_WORKSHOP_ROOT, beside a Steam game root, beside a mirrored game root, or in a mods folder beside the mirror.
    std::vector<std::filesystem::path> workshop_roots;
    if (const auto configured = environment_or("EAWR_WORKSHOP_ROOT", ""); !configured.empty()) workshop_roots.emplace_back(configured);
    const auto installed_root = game_root();
    workshop_roots.push_back(installed_root.parent_path().parent_path() / "workshop" / "content" / "32470");
    workshop_roots.push_back(installed_root.parent_path() / "workshop" / "content" / "32470");
    workshop_roots.push_back(installed_root.parent_path().parent_path() / "mods" / "workshop" / "content" / "32470");
    std::size_t begin = 0;
    while (!mods.empty() && begin <= mods.size()) {
        const auto end = mods.find(';', begin);
        const auto id = mods.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        std::filesystem::path folder;
        for (const auto& base : workshop_roots) {
            std::error_code unreadable; // a folder the host may not read counts as absent
            if (std::filesystem::is_directory(base / id, unreadable) && !unreadable) { folder = base / id; break; }
        }
        if (folder.empty()) {
            std::cout << "SKIPPED: workshop mod " << id << " is not installed on this host" << std::endl;
            return 77;
        }
        chain += (chain.empty() ? "" : ";") + folder.string();
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    InstalledLive fixture(chain, campaign);
    fixture.setup.seed = options.seed;
    options.campaign = campaign;
    return galactic_quality_run::run(fixture, options, ga::retail_service_periods);
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--remake-save-probe") return remake_save_probe(argc > 2 ? std::stoull(argv[2]) : 3000);
        if (argc > 1 && std::string_view(argv[1]) == "--human-encounter")
            return installed(true, argc > 2 ? argv[2] : "out/galactic-human-encounter", true, 0, 1, true);
        if (argc > 1 && std::string_view(argv[1]) == "--coop-victory-r1") return coop_victory_r1(argc > 2 ? std::stoull(argv[2]) : 600);
        if (argc > 1 && std::string_view(argv[1]) == "--quality-run") return quality_run(argc, argv);
        if (argc > 1 && std::string_view(argv[1]) == "--exchange-encode")
            return exchange_encode(argc > 2 ? std::stoull(argv[2]) : 300);
        if (argc > 1 && std::string_view(argv[1]) == "--tick-profile")
            return tick_profile(argc > 2 ? argv[2] : "out/galactic-tick-profile", argc > 3 ? std::stoull(argv[3]) : 30,
                argc > 4 ? argv[4] : "off,on", argc > 5 ? argv[5] : "1");
        if (argc > 1 && std::string_view(argv[1]) == "--live-record-contracts") {
            // Exercise the real composed owners and cross-worker midpoint restore in a short
            // window. Accelerated service periods cross several deadlines; this is not timing evidence.
            ProfileEvidence evidence;
            const auto code = live_production(argc > 2 ? argv[2] : "out/galactic-ai-live-record-contracts",
                32, true, true, ga::ServicePeriods{8, 3, 2, 2, 8, 5}, &evidence, true);
            check_path_table_prepass(evidence, false);
            return code || failures ? 1 : 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--live-profile-comparison") {
            const std::filesystem::path directory = argc > 2 ? argv[2] : "out/galactic-ai-profile";
            const auto frames = argc > 3 ? std::stoull(argv[3]) : 600;
            ProfileEvidence off, on;
            eawr::gc_profile::enabled.store(false, std::memory_order_relaxed);
            live_production(directory / "off", frames, true, true, ga::retail_service_periods, &off);
            eawr::gc_profile::enabled.store(true, std::memory_order_relaxed);
            live_production(directory / "on", frames, true, true, ga::retail_service_periods, &on);
            check_path_table_prepass(on, frames >= 200);
            expect(off.hashes == on.hashes && !off.hashes.empty(), "profile off/on preserves every live tick hash at 1/2/4/8 workers");
            expect(off.galaxy_sha == on.galaxy_sha && !off.galaxy_sha.empty(), "profile off/on preserves canonical galaxy SHA at 1/2/4/8 workers");
            std::cout << "GC-AI-PROFILE-EQUALITY " << (failures ? "FAIL" : "PASS") << std::endl;
            return failures ? 1 : 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--live-cadence") {
            ga::ServicePeriods periods = ga::retail_service_periods;
            if (argc > 4) periods[2] = periods[3] = std::stoull(argv[4]);
            if (argc > 5) periods[1] = periods[5] = std::stoull(argv[5]);
            return live_production(argc > 2 ? argv[2] : "out/galactic-ai-live-cadence", argc > 3 ? std::stoull(argv[3]) : 600,
                true, true, periods);
        }
        if (argc > 1 && (std::string_view(argv[1]) == "--live-production" || std::string_view(argv[1]) == "--live-performance"))
            return live_production(argc > 2 ? argv[2] : "out/galactic-ai-live-production", argc > 3 ? std::stoull(argv[3]) : 1800,
                std::string_view(argv[1]) == "--live-performance");
        if (argc > 1 && std::string_view(argv[1]) == "--prepared-stealth") {
            // Short stock proof of the prepared AI game's stealth facts (checked inside installed() before any frame).
            const std::filesystem::path directory = argc > 2 ? argv[2] : "out/galactic-ai-prepared-stealth";
            std::filesystem::create_directories(directory);
            return installed(true, directory, false, 1, 1);
        }
        if (argc > 1 && std::string_view(argv[1]) == "--freestore-movement") {
            const std::filesystem::path directory = argc > 2 ? argv[2] : "out/galactic-ai-freestore-movement";
            const std::uint64_t frames = argc > 3 ? std::stoull(argv[3]) : 1800;
            std::filesystem::create_directories(directory);
            for (const std::size_t workers : eawr::platform::determinism_worker_counts())
                installed(true, directory / ("w" + std::to_string(workers)), false, frames, workers);
            bool ok = movement_probes.size() == eawr::platform::determinism_worker_counts().size();
            std::ofstream report(directory / "freestore-movement.json", std::ios::trunc);
            report << "{\"frames\":" << frames << ",\"runs\":[";
            for (std::size_t index = 0; index < movement_probes.size(); ++index) {
                const auto& run = movement_probes[index];
                ok = ok && run.failure.empty() && run.requests > 0 && run.hop_frame > 0 && run.refusals == 0
                    && run.final_hash == movement_probes.front().final_hash && run.hashes == movement_probes.front().hashes;
                report << (index ? "," : "") << "{\"workers\":" << run.workers << ",\"frames\":" << run.frames
                    << ",\"freestore_movement_requests\":" << run.requests << ",\"first_request_frame\":" << run.first_request_frame
                    << ",\"hop_frame\":" << run.hop_frame << ",\"unbound_refusals\":" << run.refusals
                    << ",\"final_hash\":\"" << run.final_hash << "\",\"failure\":\"" << run.failure << "\",\"hashes\":[";
                for (std::size_t at = 0; at < run.hashes.size(); ++at)
                    report << (at ? "," : "") << "{\"frame\":" << run.hashes[at].first << ",\"sha256\":\"" << run.hashes[at].second << "\"}";
                report << "]}";
            }
            report << "],\"passed\":" << (ok && !failures ? "true" : "false") << "}\n"; report.close();
            std::cout << "freestore movement evidence " << (ok && !failures ? "PASS" : "FAIL") << ": "
                << (directory / "freestore-movement.json").string() << '\n';
            return ok && !failures ? 0 : 2;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--hash-baseline")
            return installed(true, argc > 2 ? argv[2] : "out/hash-baseline", true, 0, 1, false, true);
        if (argc > 1) {
            const auto mode = std::string_view(argv[1]);
            return installed(mode == "--release" || mode == "--release-benchmark",
                argc > 2 ? argv[2] : "out/galactic-ai-campaign-gate", mode == "--release-benchmark");
        }
        prepared_restore_contracts();
        authored_selection_contracts();
        mp_ai_uses_authored_faction_basic_ai();
        shared_story_local_player_needs_sp_rules_and_one_human();
        mp_ai_uses_agreed_difficulty_mapping();
        mp_ai_preserves_human_inactive_and_ambient_absence();
        mp_ai_refuses_unsupported_playable_inputs();
        unattended_human_contracts();
        worker_verdict_contracts();
        paid_selection_identity_contracts();
        blocked_invasion_overlap_contract();
        qualified_safety_contracts();
        no_source_transit_contracts();
        nested_actor_facts_contracts();
        actor_parent_depth_contracts();
        sparse_actor_change_contracts();
        free_fleet_signal_contracts();
        native_provider_isolation_contracts();
        real_factory_contracts();
        real_factory_contracts(true);
        real_factory_contracts(false, true);
    } catch (const std::exception& error) {
        if (argc > 1 && (std::string_view(argv[1]) == "--release" || std::string_view(argv[1]) == "--release-benchmark")) {
            const std::filesystem::path directory = argc > 2 ? argv[2] : "out/galactic-ai-campaign-gate";
            std::error_code ignored; std::filesystem::create_directories(directory, ignored);
            std::ofstream refused(directory / "result.json", std::ios::trunc);
            refused << "{\"release_gate_passed\":false,\"run_started\":false,\"failure\":";
            galactic_campaign_gate::json_string(refused, error.what()); refused << "}\n";
        }
        std::cerr << error.what() << '\n'; return 1;
    }
    return failures ? 1 : 0;
}
