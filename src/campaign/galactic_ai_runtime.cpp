#include "galactic_ai_runtime.hpp"
#include "eawr/vfs/vfs.hpp"
#include "galactic_ai_usability.hpp"
#include "galactic_ai_lane_inbox.hpp"
#include "eawr/campaign/galactic_world_immediate_host.hpp"
#include "../script/foc/ai_script_evaluators.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/campaign/story_plots.hpp"
#include "eawr/core/gc_profile.hpp"
#include "../script/foc/galactic_ai.hpp"
#include "../script/foc/galactic_ai_intent_commit.hpp"
#include "../script/foc/galactic_ai_intent_refusal.hpp"
#include "../script/foc/galactic_ai_outbox.hpp"
#include "../script/foc/galactic_ai_outbox_state.hpp"
#include "eawr/script/foc/galactic_ai_pass.hpp"
#include "../script/foc/tactical_ai_internal.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/ai_selection_content.hpp"
#include "eawr/campaign/ai_perception_content.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/sync_digest.hpp"
#include "eawr/sim/world.hpp"

#include <algorithm>
#include <exception>
#include <cstdlib>
#include <iterator>
#include <map>
#include <cstring>
#include <chrono>
#include <new>
#include <optional>
#include <span>
#include <system_error>
#include <variant>

namespace eawr::campaign {
namespace ga = script::foc::galactic_ai;
namespace ai = script::foc::ai;
namespace auth = script::authoritative;
namespace foc = script::foc;
namespace {
template<class T> core::Result<T> absent(std::string name) {
    core::Diagnostic error; error.code = "EAWR-GCAI-RUNTIME-0001";
    error.message = "galactic AI provider absent: " + std::move(name);
    return core::Result<T>::failure(std::move(error));
}
// The async lane's native component: sim-owned (outbox, inbox, billing view), so it is digested live every tick (W1 S4).
constexpr std::string_view async_lane_component = "galactic-async";
class MeasuredState final : public ga::StateContributor {
public:
    MeasuredState(std::shared_ptr<ComposedGalacticAiState> owner, std::shared_ptr<GalacticAiDigestWork> work)
        : owner_(std::move(owner)), work_(std::move(work)) {}
    core::Result<ga::NativeState> capture() const override { return owner_->capture(); }
    core::Result<void> restore(const ga::NativeState& state) override { return owner_->restore(state); }
    core::Result<std::string> state_hash() const override { return hash(false); }
    core::Result<std::string> sync_hash() const override { return hash(true); }
private:
    core::Result<std::string> hash(bool sync) const {
        const auto generations = sync ? owner_->sync_generations() : owner_->digest_generations();
        const auto sync_encodes = owner_->sync_encodes();
        const auto core_encodes = owner_->owner_encodes(); const auto external_encodes = owner_->external_encodes();
        const auto begin = std::chrono::steady_clock::now();
        auto result = sync ? owner_->sync_hash() : owner_->state_hash();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
        work_->last_nanoseconds = static_cast<std::uint64_t>(elapsed); work_->nanoseconds += work_->last_nanoseconds;
        work_->maximum_nanoseconds = std::max(work_->maximum_nanoseconds, work_->last_nanoseconds); ++work_->requests;
        work_->last_changed = (sync ? owner_->sync_generations() : owner_->digest_generations()) != generations;
        work_->owner_encodes = owner_->owner_encodes(); work_->external_encodes = owner_->external_encodes();
        work_->last_encoded = core_encodes != work_->owner_encodes || external_encodes != work_->external_encodes;
        if (sync) {
            constexpr std::array<std::string_view, 6> names{"galactic-ai-lifecycle", "GALACTIC_PERCEPTION", "galactic-goals", "funding", "galactic-execution", "galactic-production"};
            auto counts = owner_->sync_encodes(); work_->last_encoded = counts != sync_encodes;
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto at = counts.find(std::string(names[i])); work_->owner_encodes[i] = at == counts.end() ? 0 : at->second;
                if (at != counts.end()) counts.erase(at);
            }
            work_->external_encodes = std::move(counts);
        }
        work_->includes_internal_oracle = owner_->full_verification_enabled();
        if (!result) return result;
        // Benchmark processes disable the compositor's internal oracle. The
        // observation adapter independently proves each requested digest after
        // stopping the clock, without changing production verification policy.
        auto captured = owner_->capture();
        if (!captured) return core::Result<std::string>::failure(captured.error());
        work_->native_bytes = 4 + std::string_view("eawr-galactic-ai-native").size() + 4 + 8;
        for (const auto& [name, bytes] : captured.value().components) work_->native_bytes += 12 + name.size() + bytes.size();
        auto full = sync ? owner_->record_sync_hash(true) : ga::native_state_hash(captured.value());
        if (!full) return full;
        if (full.value() != result.value()) return absent<std::string>("observed native digest differs from cold reconstruction");
        return result;
    }
private:
    std::shared_ptr<ComposedGalacticAiState> owner_;
    std::shared_ptr<GalacticAiDigestWork> work_;
};
class Runtime final : public ga::Services, public std::enable_shared_from_this<Runtime> {
public:
    std::shared_ptr<const GalacticAiRuntimeSetup> setup;
    GalacticAiRuntimeProviders providers;
    std::shared_ptr<GalacticStoryLink> story_link; // the story scripts' route into the plot engine, bound at install
    GalacticAiRuntimeProviders::ChildScripts child_scripts;
    GameSession::ChildTimelineReaderFactory child_timeline_factory;
    std::vector<std::pair<ModeId, std::shared_ptr<GameSession::ChildTimelineValueReader>>> pending_child_timelines;
    GalacticAiRuntimeProviders::BattleListener battle;
    std::shared_ptr<foc::detail::Host> host = std::make_shared<foc::detail::Host>();
    std::shared_ptr<ai::GalacticWorld> world;
    std::shared_ptr<ai::Engine> engine;
    std::shared_ptr<ai::GoalStateContributor> goals;
    bool phase_active{};
    std::optional<ga::gc::PlayerId> tail_fault_provider;
    std::shared_ptr<ai::GalacticFunding> funding = std::make_shared<ai::GalacticFunding>();
    std::weak_ptr<ga::Manager> manager;
    std::weak_ptr<ComposedGalacticAiState> native_state;
    std::shared_ptr<ga::NativeActions> actions;
    std::unique_ptr<ga::gc::GalacticSession::NativeActionBarrier> native_actions_barrier;
    // Async AI A2.2: the barrier decides, it does not commit. The players' intents are retained per virtual
    // tick in the outbox and the world takes them through the ordered readmit at their apply tick.
    ga::IntentOutbox outbox{ga::synchronous_reference_schedule};
    // Batch driver. ai_ticks counts the admitted AI service ticks (a held or blocked AI neither snapshots nor applies);
    // ai_index is the current one. A batch of schedule.batch_ticks virtual ticks starts every batch_ticks-th admitted
    // tick, executed inline on the sim thread; its slices apply at ai_index = snapshot + delay + (j - 1).
    std::uint64_t ai_ticks{}, ai_index{};
    std::uint32_t virtual_offset{}; // 0-based virtual tick being served
    // The frame the AI lives in during the current virtual tick (monotone even while the galaxy's frame is paused).
    [[nodiscard]] std::uint64_t virtual_frame() const {
        const auto real = galaxy->clock().galactic_frame;
        const auto lifecycle = manager.lock();
        return lifecycle ? lifecycle->virtual_mode(real) : real + virtual_offset;
    }
    std::vector<auth::ScriptCommand> released_commands; // commands of applied slices, handed to the tick's report
    // A2.1 sim->AI inbox. On an async schedule a world delivery outside the AI window is queued and applied at the next window start
    // (drain_inbox), so the window reads the events of the world steps before it and nothing else. Inside the window (the AI's own
    // barrier work) and on the synchronous reference schedule a delivery applies at once.
    GalacticAiLaneInbox inbox;
    GalacticAiBillingView billing_view; // W1 S3: the AI budget the world bills against between windows (async only)
    // W1 S6 (async): the plot edits of the running attempt (published with its commit), the session's real plot applier, and the
    // "not applied" notes of the plot edits that became due this tick.
    std::vector<sim::galactic::story::PlotCommand> plot_buffer;
    std::function<core::Result<void>(const sim::galactic::story::PlotCommand&)> apply_plot;
    std::vector<core::LoadNote> due_plot_notes;
    std::vector<std::string> window_diagnostics; // W1 S6: diagnostics raised inside an async window, published with its commit
    std::vector<core::LoadNote> window_load_notes; // W1 S6 (review #6425): the runtime load notes of that window, published likewise
    std::map<ga::gc::PlayerId, sim::math::Fixed> credit_multipliers; // each selected AI player's difficulty credit multiplier
    // #6378: the rules of the world-side immediate host (story scripts; the object scripts of #6345 bind the same host).
    [[nodiscard]] GalacticWorldImmediateRules world_immediate_rules() const {
        GalacticWorldImmediateRules rules;
        rules.placement = providers.shadow_placement;
        for (const auto& selected : setup->lifecycle.players) rules.ai_players.insert(selected.player);
        rules.credit_multipliers = credit_multipliers;
        rules.executor = [executor = setup->executor] { return executor.get(); };
        return rules;
    }
    GalacticStoryGlobals story_globals; // W1 S5: the story-owned GlobalValue store (async only; the AI's registry is its replica)
    // A load diagnostic: inside an async window it waits for the window's publication (the world appends it when that becomes
    // visible); anywhere else it is appended at once.
    void note_diagnostic(std::string line) {
        if (outbox.schedule().apply_delay != 0 && window_open) window_diagnostics.push_back(std::move(line));
        else setup->load_diagnostics.push_back(std::move(line));
    }
    bool window_open{};
    [[nodiscard]] bool defers_deliveries() const noexcept { return outbox.schedule().apply_delay != 0 && !window_open; }
    // Arrival tags of the queued entries (sequence, AI tick count at arrival), for the apply tally only: never saved or hashed.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> inbox_tags;
    std::uint64_t delivery_sequence{}, drained_sequence{};
    void queue(GalacticAiLaneInbox::Entry entry) {
        inbox.push(std::move(entry)); inbox_tags.emplace_back(++delivery_sequence, ai_ticks); ++apply_tally->queued;
    }
    core::Result<void> deliver_battle(GalacticAiBattleFacts facts) {
        if (defers_deliveries()) { queue(std::move(facts)); return core::Result<void>::success(); }
        ++apply_tally->delivered_at_once;
        return battle ? battle(facts) : core::Result<void>::success();
    }
    core::Result<void> drain_inbox() {
        if (inbox.empty()) return core::Result<void>::success();
        const auto tags = std::move(inbox_tags); inbox_tags.clear();
        const auto entries = inbox.take();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            ++apply_tally->drained;
            if (index < tags.size() && tags[index].first) {
                const auto [tag_sequence, arrival] = tags[index];
                if (tag_sequence <= drained_sequence) ++apply_tally->out_of_order;
                if (ai_index < arrival || ai_index - arrival >= outbox.schedule().batch_ticks) ++apply_tally->late;
                drained_sequence = tag_sequence;
            }
            if (const auto* facts = std::get_if<GalacticAiBattleFacts>(&entry)) {
                // No battle listener is bound (no AI player with one): as at once, the notice has no reader.
                if (!battle) continue;
                if (auto delivered = battle(*facts); !delivered) return delivered;
            } else if (const auto* credit = std::get_if<ga::gc::GalacticSession::CreditNotification>(&entry)) {
                if (auto applied = actions->apply_credit(*credit); !applied) return applied;
            } else if (const auto* bill = std::get_if<GalacticAiRecordedBill>(&entry)) {
                if (auto applied = actions->apply_bill_effect(bill->build, bill->effect); !applied) return applied;
            } else if (auto applied = actions->apply_delivery(std::get<ga::NativeActions::Delivery>(entry)); !applied) return applied;
        }
        check_billing_view();
        return core::Result<void>::success();
    }
    // Right after a drain the AI must hold the newest publication with the logged credits and bills replayed (the AI decides
    // nothing between windows). Observation only: a difference is counted, never refused.
    void check_billing_view() {
        if (!billing_view.active()) return;
        const auto expected = billing_view.latest_replayed();
        bool equal = static_cast<bool>(expected) && actions->bill_rows() == expected.value().second;
        if (equal) for (const auto& selected : setup->lifecycle.players) {
            const auto held = funding->ledger(selected.player);
            const auto at = expected.value().first.find(selected.player);
            if ((held != nullptr) != (at != expected.value().first.end()) || (held && *held != at->second)) { equal = false; break; }
        }
        if (!equal) ++apply_tally->billing_view_mismatch;
    }
    // The window's publication, taken after its commit; the world bills against it from the window's first apply tick, and the
    // per-tick hashes read its AI digests from then on (W1 S4: the AI-owned native components and the AI scheduler).
    core::Result<void> capture_publication(std::uint64_t visible_at, auth::ScriptScheduler& scripts, const sim::PartitionExecutor& executor) {
        const auto begin = std::chrono::steady_clock::now();
        GalacticAiBillingPublication publication;
        publication.visible_at = visible_at; publication.log_cut = billing_view.log_end();
        for (const auto& selected : setup->lifecycle.players)
            if (const auto ledger = funding->ledger(selected.player)) publication.ledgers.emplace(selected.player, *ledger);
        publication.rows = actions->bill_rows();
        // The billing part is the view's own cost (R5); the digests below replace the per-tick AI hashing they come from.
        note_billing_time(begin);
        const auto state = native_state.lock();
        if (!state) return absent<void>("native state owner for the window publication");
        auto digests = state->sync_digests();
        if (!digests) return core::Result<void>::failure(digests.error());
        // The observation oracle (benchmarks and tests that observe the runtime) checks the published digests cold here, once;
        // per tick it then checks only the live lane part (MeasuredState through record_sync_hash(true)).
        if (providers.observe) {
            auto cold = state->cold_sync_digests();
            if (!cold) return core::Result<void>::failure(cold.error());
            if (cold.value() != digests.value()) return absent<void>("published native digests differ from cold reconstruction");
        }
        for (auto& [name, digest] : digests.value())
            if (name != async_lane_component) publication.ai_digests.emplace(name, std::move(digest));
        auto scheduler = scripts.sync_hash(executor, state->full_verification_enabled());
        if (!scheduler) return core::Result<void>::failure(scheduler.error());
        publication.scheduler_digest = std::move(scheduler).value();
        publication.plot_commands = std::move(plot_buffer); plot_buffer.clear();
        publication.diagnostics = std::move(window_diagnostics); window_diagnostics.clear();
        publication.load_notes = std::move(window_load_notes); window_load_notes.clear();
        publication.global_writes = engine->take_global_writes();
        billing_view.capture(std::move(publication));
        return core::Result<void>::success();
    }
    void note_billing_time(std::chrono::steady_clock::time_point begin) {
        apply_tally->billing_nanoseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
    }
    template<class T> core::Result<T> checked_native(core::Result<T> result) {
        if (!result && !phase_active) { native_actions_barrier.reset(); outbox.abandon_batch(); }
        return result;
    }
    // Async window overlay: the world as it will be once the lane's still-pending slices are applied. Built once per window
    // (virtual offset 0) before prepare(), which reads its snapshot; the lane barrier's root then continues it.
    std::optional<ga::gc::GalacticSession::NativeActionShadow> window_overlay;
    std::shared_ptr<const ga::gc::GalacticSnapshot> window_snapshot;
    struct OverlayCounters {
        std::uint64_t builds{};
        ga::gc::GalacticSession::NativeOverlayStats work;
    } overlay_counters;
    std::shared_ptr<GalacticAiApplyTally> apply_tally = std::make_shared<GalacticAiApplyTally>();
    [[nodiscard]] std::vector<ga::gc::PlayerId> lane_players() const {
        std::vector<ga::gc::PlayerId> players;
        for (const auto& selected : setup->lifecycle.players) players.push_back(selected.player);
        std::sort(players.begin(), players.end());
        return players;
    }
    core::Result<void> build_window_overlay(const sim::PartitionExecutor& executor) {
        window_overlay.reset(); window_snapshot.reset();
        if (virtual_offset || outbox.schedule().apply_delay == 0 || outbox.pending_slices().empty()) return core::Result<void>::success();
        const auto players = lane_players();
        if (players.empty()) return core::Result<void>::success();
        // The order and the stale floor of the NEXT apply, shared with commit_ordered_intents (plan_slice, slice_step_stale).
        ga::IntentCommitOptions options; options.refuse_stale_identities = true; options.identity_floor = outbox.lane_floor();
        options.invasion_floor = outbox.lane_invasion_floor();
        std::vector<ga::gc::GalacticSession::NativeOverlayRecord> records;
        for (const auto& slice : outbox.pending_slices())
            for (const auto& step : ga::plan_slice(slice.intents)) {
                if (ga::slice_step_stale(*galaxy, step.intent->record, options)) continue;
                records.push_back({step.intent->player, &step.intent->record});
            }
        const auto refused_before = overlay_counters.work.refused;
        auto built = galaxy->build_native_overlay(players.front(), records, executor, overlay_counters.work);
        if (!built) return core::Result<void>::failure(built.error());
        ++overlay_counters.builds;
        apply_tally->overlay_refused += overlay_counters.work.refused - refused_before;
        window_overlay.emplace(std::move(built).value());
        window_snapshot = window_overlay->snapshot();
        return core::Result<void>::success();
    }
    // The first virtual tick of a batch opens the lane barrier; later ones open their turn over what the batch decided.
    core::Result<void> begin_native_actions_barrier() {
        if (virtual_offset) {
            if (!native_actions_barrier) return core::Result<void>::success();
            outbox.begin_virtual_tick(virtual_offset + 1);
            return native_actions_barrier->begin_turn();
        }
        if (native_actions_barrier) return absent<void>("completed preceding native action barrier");
        const auto players = lane_players();
        const auto start = outbox.begin_batch(ai_index, {galaxy->next_entity_id(), galaxy->next_native_invasion_id()});
        ga::gc::GalacticSession::NativeBarrierOptions options;
        options.first_id = start.id; options.first_invasion = start.invasion; options.intent_sink = outbox.sink(); options.lane = true;
        options.overlay = std::move(window_overlay); window_overlay.reset(); window_snapshot.reset();
        // An asynchronous window owns a fixed identity block from its start (the outbox frontier is the block's end).
        const bool blocked = outbox.schedule().apply_delay != 0;
        if (blocked) {
            options.id_end = outbox.frontier().id; options.invasion_end = outbox.frontier().invasion;
            // The sim keeps allocating between the slices' apply ticks. Reserve the whole block now, once, at window start
            // (before the barrier opens), so no sim allocation can take an identity a pending slice still owns, whatever the
            // window ends up using; each slice is then readmitted with the world counter rewound to its own first identity
            // (GalacticSession::readmit_native_*). The barrier accepts a first identity inside the block it was told is reserved.
            // The invasion counter's block is reserved in the same step.
            if (auto reserved = galaxy->reserve_native_frontiers(galaxy->next_entity_id(), options.id_end,
                    galaxy->next_native_invasion_id(), options.invasion_end); !reserved) {
                outbox.abandon_batch(); return reserved;
            }
        }
        auto begun = galaxy->begin_native_barrier(players, ga::gc::galactic_max_commands, std::move(options));
        if (!begun) { outbox.abandon_batch(); return core::Result<void>::failure(begun.error()); }
        native_actions_barrier = std::move(begun).value();
        outbox.begin_virtual_tick(1);
        return core::Result<void>::success();
    }
    // The world takes every slice due at ai_index through the ordered readmit; their commands go to the tick's report.
    core::Result<void> apply_due_slices() {
        ga::IntentCommitOptions options; options.refuse_stale_identities = outbox.schedule().apply_delay != 0;
        options.identity_floor = outbox.lane_floor(); // read before take_due empties the outbox
        if (options.refuse_stale_identities) options.invasion_floor = outbox.lane_invasion_floor();
        const sim::InlineExecutor inline_executor;
        const auto& executor = active_executor ? *active_executor : static_cast<const sim::PartitionExecutor&>(inline_executor);
        auto due = outbox.take_due(ai_index);
        // The lane took these effects' notifications when it decided them; the readmission must not deliver them again.
        std::optional<ga::gc::GalacticSession::PreparedNotifications> prepared;
        if (!due.empty()) prepared.emplace(*galaxy);
        for (auto& slice : due) {
            if (!slice.intents.empty()) {
                auto committed = ga::commit_ordered_intents(*galaxy, std::move(slice.intents), executor, options);
                if (!committed) return core::Result<void>::failure(committed.error());
                apply_tally->stale += committed.value().stale;
                for (const auto& outcome : committed.value().outcomes)
                    if (outcome.fate == ga::IntentFate::refused && static_cast<std::size_t>(outcome.kind) < apply_tally->refused.size())
                        ++apply_tally->refused[static_cast<std::size_t>(outcome.kind)];
            }
            released_commands.insert(released_commands.end(), std::make_move_iterator(slice.commands.begin()),
                std::make_move_iterator(slice.commands.end()));
        }
        return core::Result<void>::success();
    }
    // The decisions of the batch are final: drop the barrier (its effects live on as intents), close the
    // batch and apply what is due (the synchronous reference applies the batch in its own tick).
    core::Result<void> finish_native_lane() {
        if (native_actions_barrier) {
            auto discarded = native_actions_barrier->discard(); native_actions_barrier.reset();
            if (!discarded) return discarded;
            if (auto settled = actions->settle_movement_cursors(); !settled) return settled;
        }
        outbox.end_batch();
        // The identity block was reserved when the window began (begin_native_actions_barrier).
        return apply_due_slices();
    }
    core::Result<void> select_native_player(ga::gc::PlayerId player) {
        return native_actions_barrier ? native_actions_barrier->select_player(player) : core::Result<void>::success();
    }
    std::shared_ptr<ga::LiveExecutionStateContributor> execution;
    std::shared_ptr<ga::FreestoreInput> pool = std::make_shared<ga::FreestoreInput>();
    std::shared_ptr<const ai::galactic_goals::MaterializedPlanCatalog> plans;
    ai::GalacticFreestoreSelectionProvider freestore_selection;
    ga::ExecutionRetainedState retained;
    std::map<ga::gc::PlayerId, ga::ExecutionRoutes> routes;
    ai::AiData data;
    ga::gc::GalacticSession* galaxy{};
    std::uint64_t sequence{};
    auth::ScriptScheduler* active_scripts{};
    // Borrowed from the installed GameSession parent. Native listeners die with
    // that session; fresh install and checkpoint reconstruction both rebind it.
    auth::ScriptScheduler* retained_scripts{}; // Survives native phase cleanup.
    const sim::PartitionExecutor* active_executor{};
    // S9b: a player's decision chain (run()) is handed pass_executor, never the pool: bulk work it needs and
    // can share is built before the first player by prepare_path_tables(), the rest is player-owned and
    // runs inline, as it will inside a player partition.
    ga::DecisionPassExecutor pass_executor;
    inline static thread_local const Runtime* decision_owner_{};
    inline static thread_local ga::gc::PlayerId decision_player_{};
    struct PathTable {
        std::uint64_t frame{};
        ga::ExecutionRouteInput input; // A: what the build read at the barrier start
        ga::ExecutionRouteBuild build;
    };
    const sim::PartitionExecutor& workers() const noexcept {
        return decision_owner_ == this ? static_cast<const sim::PartitionExecutor&>(pass_executor) : *setup->executor;
    }
    [[nodiscard]] const sim::PartitionExecutor* current_executor() const noexcept {
        return decision_owner_ == this ? &pass_executor : active_executor;
    }
    class DecisionPass {
    public:
        explicit DecisionPass(Runtime& owner, ga::gc::PlayerId player)
            : previous_(decision_owner_), previous_player_(decision_player_), actions_(owner.actions.get(), &owner.pass_executor) {
            decision_owner_ = &owner; decision_player_ = player;
        }
        DecisionPass(const DecisionPass&) = delete;
        DecisionPass& operator=(const DecisionPass&) = delete;
        ~DecisionPass() { decision_owner_ = previous_; decision_player_ = previous_player_; }
    private:
        const Runtime* previous_{};
        ga::gc::PlayerId previous_player_{};
        ga::NativeActions::ExecutorScope actions_;
    };
    std::shared_ptr<const ai::GalacticConnectivity> connectivity{std::make_shared<const ai::GalacticConnectivity>()};
    std::shared_ptr<const ai::GalacticConnectivity> route_source, combined_routes;
    struct RoutePublication {
        std::uint64_t revision{};
        const ai::GalacticExecutionRoute* source{};
        std::shared_ptr<const std::vector<ai::GalacticExecutionRoute>> rows{};
    };
    std::map<ga::gc::PlayerId, RoutePublication> route_publications;
    struct EconomyCopy {
        std::mutex economy_mutex;
        std::shared_ptr<const std::vector<ai::GalacticBudgetPlayer>> budget_players;
        std::optional<std::uint64_t> budget_players_revision;
        std::weak_ptr<const ai::GalacticEconomyRuntime> last_economy;
        std::uint64_t economy_revision{};
        std::uint64_t economy_tick{};
        std::uint64_t economy_engine_revision{};
        std::optional<std::uint64_t> economy_production_revision;
        bool economy_production_present{}, economy_build_tasks_ready{};
        bool build_budgets_cached{};
        std::uint64_t build_engine_revision{}, build_funding_revision{};
        std::optional<std::uint64_t> build_production_revision;
        GalacticAiRuntimeProviders::BuildBudgetPublication copied_build_budgets;
    };
    struct PlayerRuntimeState {
        std::map<ga::gc::PlayerId, std::vector<ai::EvaluatedFundingTemplate>> initial;
        std::map<ga::gc::PlayerId, PathTable> path_tables;
        // A mod can query another player's perception during its own decision chain. Keep that reader's
        // projection separate, including human/neutral readers which have no native AI PlayerState.
        std::map<ga::gc::PlayerId, EconomyCopy> economy;
    };
    // Key zero is the serial boundary/battle-reader copy; decision chains own their selected-player entry.
    std::map<ga::gc::PlayerId, PlayerRuntimeState> player_runtime;
    bool build_tasks_ready{};
    void clear_path_tables() {
        for (auto& [player, slice] : player_runtime) { static_cast<void>(player); slice.path_tables.clear(); }
    }

    core::Result<std::shared_ptr<const ai::GalacticEconomyRuntime>> copy_economy() {
        using Result = core::Result<std::shared_ptr<const ai::GalacticEconomyRuntime>>;
        if (!galaxy || !world->available()) return absent<std::shared_ptr<const ai::GalacticEconomyRuntime>>("completed economy world");
        auto& memo = player_runtime.at(decision_owner_ == this ? decision_player_ : 0).economy.at(engine->build_budget_reader());
        auto& economy_mutex = memo.economy_mutex;
        auto& budget_players = memo.budget_players;
        auto& budget_players_revision = memo.budget_players_revision;
        auto& last_economy = memo.last_economy;
        auto& economy_revision = memo.economy_revision;
        auto& economy_tick = memo.economy_tick;
        auto& economy_engine_revision = memo.economy_engine_revision;
        auto& economy_production_revision = memo.economy_production_revision;
        auto& economy_production_present = memo.economy_production_present;
        auto& economy_build_tasks_ready = memo.economy_build_tasks_ready;
        auto& build_budgets_cached = memo.build_budgets_cached;
        auto& build_engine_revision = memo.build_engine_revision;
        auto& build_funding_revision = memo.build_funding_revision;
        auto& build_production_revision = memo.build_production_revision;
        auto& copied_build_budgets = memo.copied_build_budgets;

        const auto lock = [&] {
            gc_profile::Scope wait_scope(gc_profile::Phase::economy_lock_wait);
            return std::unique_lock(economy_mutex);
        }();
        const auto tick = world->completed_tick(); const auto revision = funding->state_revision();
        const auto engine_revision = engine->native_state_revision();
        const auto production_revision = actions ? actions->state_revision() : std::optional<std::uint64_t>{};
        const auto task_revision = engine->galactic_build_tasks_revision();
        const auto purchase_revision = actions ? std::optional<std::uint64_t>{actions->build_state_revision()} : std::nullopt;
        const bool production_present = static_cast<bool>(actions);
        const bool tracked_production = !production_present || production_revision.has_value();
        if (tracked_production && tick == economy_tick && revision == economy_revision && engine_revision == economy_engine_revision
            && production_revision == economy_production_revision && production_present == economy_production_present
            && build_tasks_ready == economy_build_tasks_ready)
            if (auto previous = last_economy.lock()) return Result::success(std::move(previous));
        auto next = std::make_shared<ai::GalacticEconomyRuntime>();
        const auto& saved = funding->state();
        next->completed_tick = tick; next->timeline = saved.timeline;
        next->base_credits.clear();
        const auto& fiscal = galaxy->setup().fiscal;
        const auto& abilities = galaxy->setup().ability_host;
        const auto* control = galaxy->snapshot()->control();
        if (fiscal) for (const auto& at : fiscal->planets) {
            auto credits = at.credits;
            if (control && abilities) {
                const auto surface = std::find_if(control->planets.begin(), control->planets.end(),
                    [&](const auto& row) { return row.planet == at.planet; });
                if (surface != control->planets.end() && surface->destroyed) {
                    const auto ruined = std::find_if(abilities->ruined_income.begin(), abilities->ruined_income.end(),
                        [&](const auto& row) { return row.planet == at.planet; });
                    if (ruined != abilities->ruined_income.end()) credits = ruined->credits;
                }
            }
            // WGA-449: copy the existing fiscal/destruction owner's base
            // getter, before modifiers, and retain its native integer value.
            next->base_credits.emplace(at.planet, static_cast<std::int32_t>(script::numeric::from_fixed(credits)));
        }
        // Clock changes publish only this small header. Retained budget records
        // and the producer's immutable build heads are shared across boundaries.
        if (!budget_players || budget_players_revision != revision) {
            auto rows = std::make_shared<std::vector<ai::GalacticBudgetPlayer>>();
            rows->reserve(setup->perception.players.size());
            for (const auto& player : setup->perception.players) {
                ai::GalacticBudgetPlayer out; out.player = player.player;
                const auto ledger = saved.find(out.player);
                out.income = script::numeric::from_fixed(ledger == saved.end() ? saved.constructor_income : ledger->second.last_service_income);
                if (ledger != saved.end()) for (const auto& [category, values] : ledger->second.categories)
                    out.categories[category] = {script::numeric::from_fixed(values.income), script::numeric::from_fixed(values.cash)};
                rows->push_back(std::move(out));
            }
            budget_players = std::move(rows); budget_players_revision = revision;
        }
        next->budget_publication = budget_players;
        next->build_tasks_complete = false;
        if (build_tasks_ready && actions && providers.build_budgets) {
            if (!build_budgets_cached || build_engine_revision != task_revision
                || build_funding_revision != revision || build_production_revision != purchase_revision) {
                auto builds = providers.build_budgets(*engine, *actions, *funding);
                if (!builds) return Result::failure(builds.error());
                if (!builds.value()) return absent<std::shared_ptr<const ai::GalacticEconomyRuntime>>("immutable complete build-head publication");
                copied_build_budgets = std::move(builds).value(); build_budgets_cached = true;
                build_engine_revision = task_revision; build_funding_revision = revision;
                build_production_revision = purchase_revision;
            }
            next->build_publication = copied_build_budgets;
            next->build_tasks_complete = true;
        } else {
            build_budgets_cached = false; copied_build_budgets.reset();
        }
        economy_revision = revision; economy_tick = tick; last_economy = next;
        economy_engine_revision = engine_revision; economy_production_revision = production_revision;
        economy_production_present = production_present; economy_build_tasks_ready = build_tasks_ready;
        return Result::success(std::move(next));
    }
    core::Result<void> publish_economy() {
        auto copied = copy_economy();
        return copied ? world->bind_economy_runtime(std::move(copied).value()) : core::Result<void>::failure(copied.error());
    }

    core::Result<void> publish_freestore() {
        return providers.publish_freestore
            ? providers.publish_freestore(*actions, *pool, *galaxy->snapshot(), workers()) : core::Result<void>::success();
    }

    // The execution copy is archival. Readers use their player's native pool and
    // Engine reservations; copy changed archival rows only after the Lua/native commit.
    core::Result<void> synchronize_reservations() {
        gc_profile::Scope profile_scope(gc_profile::Phase::prepare_reservations);
        std::optional<ga::ExecutionRetainedState> state;
        std::map<sim::EntityId, std::uint64_t> reservations;
        const bool fixture = !pool->pool->qualified;
        for (const auto& selection : setup->lifecycle.players) {
            // Engine initialization precedes fresh route initialization.
            const auto row = retained.find(selection.player);
            if (row == retained.end()) continue;
            auto copied = engine->galactic_execution_reservations(selection.player);
            if (!copied) return core::Result<void>::failure(copied.error());
            if (row->second.reservations != copied.value()) {
                if (!state) state = retained;
                state->at(selection.player).reservations = copied.value();
            }
            if (fixture) for (const auto& [object, plan] : copied.value())
                if (!reservations.emplace(object, plan).second) return absent<void>("unique execution reservation owner");
        }
        if (state) if (auto replaced = execution->replace_retained(std::move(*state)); !replaced) return replaced;
        // Qualified membership and reservation lifetime belong to NativeActions.
        // E retains its own Engine associations; its copied map cannot reseed a pool.
        if (!pool->pool->qualified && pool->pool->reservations != reservations) {
            auto copied = std::make_shared<ga::FreestorePool>(*pool->pool);
            copied->reservations = std::move(reservations); pool->pool = std::move(copied);
        }
        return providers.publish_freestore
            ? providers.publish_freestore(*actions, *pool, *galaxy->snapshot(), workers()) : core::Result<void>::success();
    }

    std::shared_ptr<const ai::GalacticConnectivity> route_publication() {
        bool changed = route_source != connectivity || route_publications.size() != routes.size();
        for (auto at = route_publications.begin(); at != route_publications.end(); )
            if (!routes.contains(at->first)) at = route_publications.erase(at); else ++at;
        for (const auto& [player, route] : routes) {
            auto& memo = route_publications[player];
            if (!memo.rows || memo.revision != route.revision || memo.source != route.rows.data()) {
                memo.revision = route.revision; memo.source = route.rows.data();
                memo.rows = std::make_shared<const std::vector<ai::GalacticExecutionRoute>>(route.rows);
                changed = true;
            }
        }
        if (!changed && combined_routes) return combined_routes;
        auto published = std::make_shared<ai::GalacticConnectivity>(*connectivity);
        published->execution_routes.clear(); published->player_routes.clear();
        published->execution_available = !routes.empty();
        for (const auto& [player, route] : routes) {
            published->execution_available = published->execution_available && route.available;
            published->revision += route.revision;
            published->player_routes.emplace(player, route_publications.at(player).rows);
        }
        route_source = connectivity; combined_routes = std::move(published);
        return combined_routes;
    }
    core::Result<void> publish_routes(const sim::PartitionExecutor& executor) {
        gc_profile::Scope profile_scope(gc_profile::Phase::publish_routes);
        const auto lifecycle = manager.lock();
        if (lifecycle) world->set_perception_period(lifecycle->service_periods()[static_cast<std::size_t>(ga::Service::perception)]);
        auto bound = world->bind_publication(galaxy->snapshot(), route_publication(), executor, galaxy->completed_tick(), galaxy->next_entity_id(),
            lifecycle && lifecycle->perception_due(galaxy->completed_tick() + virtual_offset));
        for (auto& note : world->take_path_coefficient_diagnostics()) note_diagnostic(std::move(note));
        return bound;
    }
    core::Result<void> publish_refreshed_routes(const sim::PartitionExecutor& executor) {
        const auto published = route_publication();
        auto routes_published = world->publish_execution_route_generations(published->execution_available, published->revision, published->player_routes);
        if (!routes_published) return routes_published;
        auto coefficients = world->publish_path_coefficients(executor);
        for (auto& note : world->take_path_coefficient_diagnostics()) note_diagnostic(std::move(note));
        return coefficients;
    }
    core::Result<std::shared_ptr<const ai::GalacticConnectivity>> prepare_connectivity(
        const ga::gc::GalacticSession& parent, const sim::PartitionExecutor& executor) {
        if (providers.prepare_connectivity_publication)
            return providers.prepare_connectivity_publication(parent, *world, *engine, executor);
        auto prepared = providers.prepare_connectivity(parent, *world, *engine, executor);
        using Result = core::Result<std::shared_ptr<const ai::GalacticConnectivity>>;
        return prepared ? Result::success(std::make_shared<const ai::GalacticConnectivity>(std::move(prepared).value()))
            : Result::failure(prepared.error());
    }
    // Standalone deadline phase: the caller supplies the prepass executor. No
    // active service/partition is needed; run() uses the same seam in the current
    // sequential player chain to retain its exact perception-read boundary.
    core::Result<void> prepare_path_table(ga::gc::PlayerId player, std::uint64_t frame, const sim::PartitionExecutor& executor) {
        const auto reads = world->owner_reads();
        const auto route = routes.find(player);
        if (route == routes.end()) return absent<void>("initialized execution finder");
        if (route->second.next_refresh > frame) return core::Result<void>::success();
        return update_routes(player, frame, false, executor);
    }
    // The execution graph of a refresh: the player's planets in their retained order (WGA-27/77). It reads the
    // snapshot, the player's usability and its cached enemy-space perception, and writes only those caches.
    // `read_only` reads the same facts without writing the perception cache or visibility rows (the prepass and
    // the diagnostic reads; only the player's own refresh in its chain may write them, in its sequential order).
    core::Result<ga::ExecutionRouteInput> refresh_input(ga::gc::PlayerId player, const ga::ExecutionRetainedPlayer& row, bool read_only) {
        auto source = read_only ? providers.execution_input_read_only(player, *galaxy->snapshot(), *world, *engine)
            : providers.execution_input(player, *galaxy->snapshot(), *world, *engine, nullptr);
        if (!source) return core::Result<ga::ExecutionRouteInput>::failure(source.error());
        auto input = std::move(source).value();
        if (input.player != player) return absent<ga::ExecutionRouteInput>("execution input player identity");
        if (!row.initialized || input.planets.size() != row.node_order.size()) return absent<ga::ExecutionRouteInput>("retained execution node domain");
        std::vector<ga::ExecutionPlanet> ordered;
        for (const auto node : row.node_order) {
            const auto found = std::find_if(input.planets.begin(), input.planets.end(), [node](const auto& planet) { return planet.planet == node; });
            if (found == input.planets.end()) return absent<ga::ExecutionRouteInput>("retained execution node identity");
            ordered.push_back(*found);
        }
        input.planets = std::move(ordered); input.maximum_distance = row.maximum_distance;
        return core::Result<ga::ExecutionRouteInput>::success(std::move(input));
    }
    // Bit-exact equality of two path-table inputs (the build is a pure function of the input).
    static bool same_input(const ga::ExecutionRouteInput& a, const ga::ExecutionRouteInput& b) {
        if (a.player != b.player || a.maximum_distance.repr != b.maximum_distance.repr || a.trade_routes != b.trade_routes
            || a.planets.size() != b.planets.size()) return false;
        for (std::size_t i = 0; i < a.planets.size(); ++i) {
            const auto& x = a.planets[i]; const auto& y = b.planets[i];
            if (x.planet != y.planet || !(x.position == y.position) || x.layer_z_adjust.repr != y.layer_z_adjust.repr
                || x.enemy_space_power.repr != y.enemy_space_power.repr
                || x.nonallied_starbase_power.repr != y.nonallied_starbase_power.repr || x.usable != y.usable) return false;
        }
        return true;
    }
    // S9b prepass over S_t, before the first player's chain: the path table of every player whose execution
    // service runs this barrier and whose own deadline has passed, built from the barrier-start input A (read
    // without writing the perception cache). The build is the bulk part of a refresh. The chain still reads its
    // input at the point GFS-A02 always read it (update_routes) and installs the prebuilt table only when that
    // input equals A bit for bit; otherwise it builds from its own input as before, so the result never depends
    // on the prepass. A player whose perception service runs in the same barrier is left to its own chain (that
    // service refreshes the cache the table reads). A refusal here is left to the player's own service, which
    // reports it at the same point as before.
    core::Result<void> prepare_path_tables(const sim::PartitionExecutor& executor, const ga::gc::GalacticSession& parent,
        const ga::gc::GalacticTick& tick) {
        clear_path_tables();
        const auto lifecycle = manager.lock();
        if (!lifecycle || !providers.execution_input_read_only) return core::Result<void>::success();
        static_cast<void>(parent); // the frozen world is the runtime's own snapshot; only the AI's frame is virtual
        const auto frame = virtual_frame();
        const auto sync = tick.completed_tick + virtual_offset;
        for (const auto& [player, route] : routes) {
            auto& path_tables = player_runtime.at(player).path_tables;
            const auto row = retained.find(player);
            if (row == retained.end() || route.next_refresh > frame || !lifecycle->service_due(player, ga::Service::execution, sync)
                || lifecycle->service_due(player, ga::Service::perception, sync)) continue;
            gc_profile::Scope profile_scope(gc_profile::Phase::path_table_prepass); // one call per table built
            auto input = refresh_input(player, row->second, true);
            if (!input) continue;
            auto built = ga::build_execution_routes(input.value(), executor, route.memo);
            if (!built) continue;
            path_tables.insert_or_assign(player, PathTable{frame, std::move(input).value(), std::move(built).value()});
        }
        return core::Result<void>::success();
    }
    core::Result<void> update_routes(ga::gc::PlayerId player, std::uint64_t frame, bool initialize, const sim::PartitionExecutor& executor) {
        auto& path_tables = player_runtime.at(player).path_tables;
        // Route writes validate archival reservations against the live Engine.
        // A goal may have released its holds earlier in this barrier.
        if (auto synchronized = synchronize_reservations(); !synchronized) return synchronized;
        auto state = retained;
        auto& row = state[player];
        const auto prepared = initialize ? path_tables.end() : path_tables.find(player);
        auto source = providers.execution_input(player, *galaxy->snapshot(), *world, *engine, nullptr);
        if (!source) return core::Result<void>::failure(source.error());
        auto input = std::move(source).value();
        if (input.player != player) return absent<void>("execution input player identity");
        if (!initialize) {
            if (!row.initialized || input.planets.size() != row.node_order.size()) return absent<void>("retained execution node domain");
            std::vector<ga::ExecutionPlanet> ordered;
            for (const auto node : row.node_order) {
                const auto found = std::find_if(input.planets.begin(), input.planets.end(), [node](const auto& planet) { return planet.planet == node; });
                if (found == input.planets.end()) return absent<void>("retained execution node identity");
                ordered.push_back(*found);
            }
            input.planets = std::move(ordered); input.maximum_distance = row.maximum_distance;
        }
        const bool offered = prepared != path_tables.end() && prepared->second.frame == frame;
        const bool prebuilt = offered && same_input(prepared->second.input, input);
        const auto existing = routes.find(player);
        auto staged = existing == routes.end() ? ga::ExecutionRoutes{} : existing->second;
        const auto producer = [&, previous = staged.memo](const sim::PartitionExecutor& workers_) -> core::Result<std::optional<ga::ExecutionRouteBuild>> {
            using Built = core::Result<std::optional<ga::ExecutionRouteBuild>>;
            if (prebuilt) return Built::success(std::move(prepared->second.build));
            auto built = ga::build_execution_routes(input, workers_, previous);
            if (!built) return Built::failure(built.error());
            return Built::success(std::move(built).value());
        };
        if (initialize) {
            const auto& players = setup->perception.players;
            const auto found = std::find_if(players.begin(), players.end(), [player](const auto& p) { return p.player == player; });
            if (found == players.end()) return absent<void>("execution player index");
            const auto count = static_cast<std::uint32_t>(std::count_if(players.begin(), players.end(), [](const auto& p) { return !p.human; }));
            if (auto initialized = ga::initialize_execution_matrix(staged, frame, count,
                static_cast<std::uint32_t>(found - players.begin()), executor, producer); !initialized) return initialized;
            row.initialized = row.finder_present = true; row.maximum_distance = input.maximum_distance;
            row.node_order.clear(); for (const auto& planet : input.planets) row.node_order.push_back(planet.planet);
        } else {
            auto refreshed = ga::publish_execution_routes(staged, frame, true, executor, producer);
            if (!refreshed) return core::Result<void>::failure(refreshed.error());
            if (!refreshed.value()) return core::Result<void>::success();
        }
        row.next_refresh = staged.next_refresh;
        if (auto replaced = execution->replace_retained(std::move(state)); !replaced) return replaced;
        execution->replace_route(player, std::move(staged));
        if (prepared != path_tables.end()) path_tables.erase(prepared);
        return publish_refreshed_routes(executor);
    }

    core::Result<std::vector<ai::EvaluatedFundingTemplate>> evaluate(ga::gc::PlayerId player, const ga::TemplatePolicy& policy) {
        std::map<int, ai::EvaluatedFundingTemplate> grouped;
        const auto selected = std::find_if(setup->lifecycle.players.begin(), setup->lifecycle.players.end(),
            [player](const auto& row) { return row.player == player; });
        if (selected == setup->lifecycle.players.end()) return absent<std::vector<ai::EvaluatedFundingTemplate>>("selected funding player");
        const auto queries = ai::GalacticQueries(*engine, *world);
        const ga::QueryContext context{player, selected->enemy, selected->human, {}, {}, ga::Mode::galactic};
        const auto current = funding->ledger(player);
        for (const auto& [name, setting] : policy.equations) {
            auto& row = grouped[setting.priority]; row.priority = setting.priority;
            const auto category = data.goal_categories.find(name);
            if (category == data.goal_categories.end()) return absent<std::vector<ai::EvaluatedFundingTemplate>>("funding category " + name);
            std::optional<sim::math::Fixed> weight;
            if (!setting.value.empty()) {
                auto value = queries.equation(setting.value, context);
                // WGA-421: this consumer initializes the equation output from
                // the current share and ignores failed evaluation. Fresh
                // categories start at zero; query/Lua failure stays distinct.
                auto fixed = ai::funding_equation_weight(current.get(), category->second,
                    value ? std::optional{value.value()} : std::nullopt);
                if (!fixed) return core::Result<std::vector<ai::EvaluatedFundingTemplate>>::failure(fixed.error());
                weight = fixed.value();
            }
            row.equations.push_back({category->second, weight});
        }
        for (const auto& [name, setting] : policy.allowances) {
            auto& row = grouped[setting.priority]; row.priority = setting.priority;
            const auto category = data.goal_categories.find(name);
            if (category == data.goal_categories.end()) return absent<std::vector<ai::EvaluatedFundingTemplate>>("allowance category " + name);
            auto fixed = script::numeric::to_fixed(setting.value);
            if (!fixed) return core::Result<std::vector<ai::EvaluatedFundingTemplate>>::failure(fixed.error());
            row.allowances.push_back({category->second, fixed.value()});
        }
        std::vector<ai::EvaluatedFundingTemplate> result;
        for (auto& [priority, row] : grouped) { static_cast<void>(priority); result.push_back(std::move(row)); }
        return core::Result<std::vector<ai::EvaluatedFundingTemplate>>::success(std::move(result));
    }
    core::Result<void> initialize(ga::gc::PlayerId player) override {
        auto& initial = player_runtime.at(player).initial;
        if (auto selected = select_native_player(player); !selected) return selected;
        const auto* account = world->account(player);
        if (!account || !initial.contains(player)) return absent<void>("initial template/account");
        auto result = funding->initialize(player, initial.at(player), account->credits);
        if (result) {
            initial.erase(player);
            const auto lifecycle = manager.lock();
            if (!active_scripts || !lifecycle) return absent<void>("fresh freestore host");
            if (auto initialized = update_routes(player, virtual_frame(), true, *current_executor()); !initialized) return initialized;
            const auto module = lifecycle->freestore(player);
            if (!module.empty()) {
                // Player identities occupy the freestore instance domain; plans
                // use the shared engine's separate first_plan_instance domain.
                bool syntax_error = false;
                if (auto created = active_scripts->create_instance(player, "Data/Scripts/FreeStore/" + std::string(module) + ".lua", &syntax_error); !created) {
                    if (!syntax_error) return created;
                    note_diagnostic("galactic freestore disabled for player " + std::to_string(player) + ": " + created.error().message);
                } else if (auto attached = ga::attach_freestore(*active_scripts, {player, player}, true); !attached) return attached;
            }
            if (providers.initialize_freestore) {
                if (auto initialized = providers.initialize_freestore(player, *actions, *galaxy->snapshot(), *current_executor()); !initialized) return initialized;
                if (providers.publish_freestore)
                    if (auto published = providers.publish_freestore(*actions, *pool, *galaxy->snapshot(), *current_executor()); !published) return published;
            }
        }
        return result;
    }
    core::Result<void> clear_equations(ga::gc::PlayerId player) override {
        if (auto selected = select_native_player(player); !selected) return selected;
        return funding->clear_equations(player);
    }
    core::Result<void> apply_funding_writes(ga::gc::PlayerId player, const ga::TemplatePolicy& policy) override {
        if (auto selected = select_native_player(player); !selected) return selected;
        auto rows = evaluate(player, policy);
        if (!rows) return core::Result<void>::failure(rows.error());
        for (const auto& row : rows.value()) if (auto applied = funding->apply_template(player, row); !applied) return applied;
        return core::Result<void>::success();
    }
    core::Result<void> apply_policy(ga::gc::PlayerId player, const ga::TemplatePolicy& policy, ga::PolicyWrite write) override {
        auto& initial = player_runtime.at(player).initial;
        if (auto selected = select_native_player(player); !selected) return selected;
        if (auto applied = engine->apply_galactic_policy(player, policy); !applied) return applied;
        if (write == ga::PolicyWrite::ordinary) return core::Result<void>::success();
        auto rows = evaluate(player, policy);
        if (!rows) return core::Result<void>::failure(rows.error());
        if (write == ga::PolicyWrite::initial) initial[player] = std::move(rows).value();
        else for (const auto& row : rows.value()) if (auto applied = funding->apply_template(player, row); !applied) return applied;
        return core::Result<void>::success();
    }
    core::Result<void> run(const ga::ServiceInput& input, auth::ScriptScheduler& scripts) override {
        if (auto selected = select_native_player(input.player); !selected) return selected;
        const auto reads = world->owner_reads();
        const auto* barrier_executor = active_executor;
        if (input.service == ga::Service::execution && !barrier_executor) return absent<void>("admitted execution worker adapter");
        const DecisionPass decision_pass(*this, input.player);
        if (auto published = publish_freestore(); !published) return published;
        if (input.service == ga::Service::execution) {
            // The table is the prepass's (prepare_path_tables). A refresh it did not take builds here and keeps the
            // barrier executor: the one right after this player's own initialization in the campaign's first
            // barrier (initialization is its own serial step) and the one in a barrier where the player's
            // perception service also runs (see prepare_path_tables).
            if (auto refreshed = prepare_path_table(input.player, input.clocks.mode, *barrier_executor); !refreshed) return refreshed;
            // WGA-405: execution assignment precedes cleanup/native landing.
            if (auto assigned = engine->service_galactic_system(input.player, input.service, scripts, sequence); !assigned) return assigned;
            const auto lifecycle = manager.lock();
            if (!lifecycle) return absent<void>("live freestore manager");
            if (!lifecycle->freestore(input.player).empty()) {
                // GFS-A04/A11, GFC-A04/A11: the admitted native execution
                // frontier precedes Lua's separately authored rate checks.
                const auto native_frontier = pool->pool;
                if (native_frontier->qualified) {
                    for (const auto& actor : native_frontier->objects) {
                        const auto* facts = actions->qualified_freestore().facts(actor.id);
                        if (actor.owner != input.player || !facts || !facts->tracked_movement
                            || !actions->qualified_freestore().contains(input.player, actor.id)) continue;
                        if (auto cleaned = actions->service_freestore_movement(input.player, actor.id); !cleaned) return cleaned;
                    }
                    if (providers.publish_freestore)
                        if (auto published = providers.publish_freestore(*actions, *pool, *galaxy->snapshot(), *current_executor()); !published) return published;
                    if (auto landed = actions->service_freestore_landing(input.player, *pool->pool, *current_executor()); !landed) return landed;
                    if (providers.publish_freestore)
                        if (auto published = providers.publish_freestore(*actions, *pool, *galaxy->snapshot(), *current_executor()); !published) return published;
                }
                const auto instances = scripts.instances();
                if (std::find(instances.begin(), instances.end(), input.player) != instances.end()) {
                    auto serviced = ga::service_freestore(scripts, {input.player, input.player}, *pool->pool, input.clocks.mode,
                        pool->pool->qualified ? &actions->qualified_freestore() : nullptr);
                    if (!serviced) return serviced;
                }
            }
        }
        if (input.service != ga::Service::execution)
            if (auto serviced = engine->service_galactic_system(input.player, input.service, scripts, sequence); !serviced) return serviced;
        return core::Result<void>::success();
    }
    core::Result<void> finish_barrier(auth::ScriptScheduler& scripts) override {
        // Deferred attaches and commands inherit every player layer. This is an
        // explicit serial tail; its final publication follows after_service.
        if (native_actions_barrier) if (auto tail = native_actions_barrier->begin_tail(); !tail) return tail;
        const auto reads = world->owner_reads();
        if (auto committed = engine->commit_galactic_events(scripts, sequence); !committed) return committed;
        world->finish_perception_queries();
        return core::Result<void>::success();
    }
    core::Result<void> after_service(auth::ServiceReport& report) override {
        if (native_actions_barrier) if (auto tail = native_actions_barrier->begin_tail(); !tail) return tail;
        const auto reads = world->owner_reads();
        if (auto serviced = engine->after_service(report); !serviced) return serviced;
        std::vector<auth::ScriptCommand> kept;
        kept.reserve(report.commands.size());
        for (auto& command : report.commands) {
            if (command.verb != ga::verb_freestore_move) { kept.push_back(std::move(command)); continue; }
            tail_fault_provider = static_cast<ga::gc::PlayerId>(engine->galactic_chain_owner(command.issuer));
            auto committed = actions->commit_freestore_move(command);
            if (!committed) return core::Result<void>::failure(committed.error());
        }
        tail_fault_provider.reset();
        report.commands = std::move(kept);
        if (auto synchronized = synchronize_reservations(); !synchronized) return synchronized;
        // They leave the AI at the slice's apply tick, with its intents (a batch that is not open passes them through).
        static_cast<void>(outbox.add_commands(report.commands));
        if (native_actions_barrier && !phase_active)
            if (auto finished = finish_native_lane(); !finished) return finished;
        return core::Result<void>::success();
    }
    core::Result<void> prepare(const sim::PartitionExecutor& executor, const ga::gc::GalacticSession& parent, const ga::gc::GalacticTick& tick) {
        const auto reads = world->owner_reads();
        // Later virtual ticks of a batch read the same frozen world: the connectivity and the freestore copy are the
        // batch's. Only the publication is bound again (perception may fall due inside the batch).
        if (virtual_offset) return publish_routes(executor);
        gc_profile::Scope profile_scope(gc_profile::Phase::runtime_prepare);
        {
            gc_profile::Scope freestore_scope(gc_profile::Phase::prepare_freestore);
            // The freestore pool a window validates against is the overlaid world's (the lane's pending slices included).
            const auto& base = window_snapshot ? *window_snapshot : *parent.snapshot();
            if (providers.publish_freestore) {
                if (auto published = providers.publish_freestore(*actions, *pool, base, executor); !published) return published;
            } else {
                auto copied = ga::copy_freestore(base, providers.freestore_types, executor);
                if (!copied) return core::Result<void>::failure(copied.error());
                pool->pool = std::make_shared<const ga::FreestorePool>(std::move(copied).value());
            }
        }
        auto prepared = [&] {
            gc_profile::Scope connectivity_scope(gc_profile::Phase::prepare_connectivity);
            return prepare_connectivity(parent, executor);
        }();
        if (!prepared) return core::Result<void>::failure(prepared.error());
        connectivity = std::move(prepared).value();
        static_cast<void>(tick);
        return publish_routes(executor);
    }
};
// Async AI: the batch phase, the slices that are decided but not applied yet and the sim->AI inbox (native component
// "galactic-async"; the inbox block follows the outbox state and is absent while the inbox is empty).
// An external contributor of the composed native state: it restores with the other qualified owners and enters the
// per-tick digest through a revision-cached fingerprint, so a tick without new work does not encode the slices again.
class AsyncLaneState final : public ga::StateContributor {
public:
    static constexpr std::string_view component = async_lane_component;
    explicit AsyncLaneState(std::weak_ptr<Runtime> owner) : owner_(std::move(owner)) {}
    core::Result<ga::NativeState> capture() const override {
        const auto owner = owner_.lock();
        if (!owner) return absent<ga::NativeState>("async lane owner");
        auto bytes = ga::encode_outbox_state(owner->outbox, owner->ai_ticks);
        if (!bytes) return core::Result<ga::NativeState>::failure(bytes.error());
        owner->inbox.encode(bytes.value());
        if (auto view = owner->billing_view.encode(bytes.value()); !view) return core::Result<ga::NativeState>::failure(view.error());
        owner->story_globals.encode(bytes.value());
        ga::NativeState state; state.components.emplace(std::string(component), std::move(bytes).value());
        return core::Result<ga::NativeState>::success(std::move(state));
    }
    core::Result<void> restore(const ga::NativeState& state) override {
        const auto owner = owner_.lock();
        if (!owner) return absent<void>("async lane owner");
        const auto found = state.components.find(std::string(component));
        if (state.version != ga::native_state_version || found == state.components.end()) return absent<void>("async lane component");
        auto decoded = ga::decode_outbox_state(found->second, true);
        if (!decoded) return core::Result<void>::failure(decoded.error());
        if (decoded.value().schedule != owner->outbox.schedule()) return absent<void>("async lane schedule of this build");
        const auto tail = std::span<const std::uint8_t>(decoded.value().tail);
        auto inbox = owner->inbox.restore_prefix(tail);
        if (!inbox) return core::Result<void>::failure(inbox.error());
        auto view = owner->billing_view.restore_prefix(tail.subspan(inbox.value()));
        if (!view) return core::Result<void>::failure(view.error());
        const auto rest = tail.subspan(inbox.value() + view.value());
        auto globals = owner->story_globals.restore_prefix(rest);
        if (!globals) return core::Result<void>::failure(globals.error());
        if (globals.value() != rest.size()) return absent<void>("async lane state framing (unknown trailing block)");
        owner->inbox_tags.assign(owner->inbox.entries().size(), {});
        owner->ai_ticks = decoded.value().ai_ticks;
        owner->outbox.restore(decoded.value().frontier, decoded.value().floor, std::move(decoded.value().slices));
        return core::Result<void>::success();
    }
    core::Result<std::string> state_hash() const override {
        auto saved = capture();
        if (!saved) return core::Result<std::string>::failure(saved.error());
        return ga::native_state_hash(saved.value());
    }
    core::Result<std::string> record_sync_hash(bool cold = false) const override {
        const auto owner = owner_.lock();
        if (!owner) return absent<std::string>("async lane owner");
        if (cold || cached_.empty() || generation_ != owner->outbox.generation() || inbox_generation_ != owner->inbox.generation()
            || view_generation_ != owner->billing_view.generation() || globals_generation_ != owner->story_globals.generation()) {
            // The batch counter is hashed apart from the slices, so only a change of the slices or the inbox encodes them.
            auto bytes = ga::encode_outbox_state(owner->outbox, 0);
            if (!bytes) return core::Result<std::string>::failure(bytes.error());
            owner->inbox.encode(bytes.value());
            if (auto view = owner->billing_view.encode(bytes.value()); !view) return core::Result<std::string>::failure(view.error());
            owner->story_globals.encode(bytes.value());
            cached_ = core::sync_state_digest(bytes.value()); bytes_ = bytes.value().size();
            generation_ = owner->outbox.generation(); inbox_generation_ = owner->inbox.generation();
            view_generation_ = owner->billing_view.generation(); globals_generation_ = owner->story_globals.generation();
        }
        std::vector<std::uint8_t> head(cached_.begin(), cached_.end());
        for (unsigned i = 0; i < 8; ++i) head.push_back(static_cast<std::uint8_t>(owner->ai_ticks >> (i * 8U)));
        return core::Result<std::string>::success(core::sync_state_digest(head));
    }
    std::size_t record_byte_upper_bound() const noexcept override { return bytes_ + 64; }
private:
    std::weak_ptr<Runtime> owner_;
    mutable std::string cached_;
    mutable std::uint64_t generation_{}, inbox_generation_{}, view_generation_{}, globals_generation_{};
    mutable std::size_t bytes_{};
};
// Runtime holds a weak manager; the manager's services retain its other owners.
class Services final : public ga::Services {
public:
    explicit Services(std::shared_ptr<Runtime> owner) : owner_(std::move(owner)) {}
    core::Result<void> initialize(ga::gc::PlayerId p) override { return owner_->checked_native(owner_->initialize(p)); }
    core::Result<void> clear_equations(ga::gc::PlayerId p) override { return owner_->checked_native(owner_->clear_equations(p)); }
    core::Result<void> apply_funding_writes(ga::gc::PlayerId p, const ga::TemplatePolicy& v) override { return owner_->checked_native(owner_->apply_funding_writes(p, v)); }
    core::Result<void> apply_policy(ga::gc::PlayerId p, const ga::TemplatePolicy& v, ga::PolicyWrite w) override { return owner_->checked_native(owner_->apply_policy(p, v, w)); }
    core::Result<void> run(const ga::ServiceInput& input, auth::ScriptScheduler& scripts) override {
        return owner_->checked_native(owner_->run(input, scripts));
    }
    core::Result<void> finish_barrier(auth::ScriptScheduler& s) override { return owner_->checked_native(owner_->finish_barrier(s)); }
    core::Result<void> after_service(auth::ServiceReport& r) override { return owner_->checked_native(owner_->after_service(r)); }
private:
    std::shared_ptr<Runtime> owner_;
};
// One service attempt retains changed rows inside their actual owning writers.
// No native archive or entity-domain inventory is captured here.
struct NativePhaseAttempt {
    Runtime& owner;
    std::shared_ptr<ga::Manager> manager;
    std::unique_ptr<ga::gc::GalacticSession::NativeOutputPhase> output;
    std::unique_ptr<auth::ScriptScheduler::PhaseSavepoint> scripts;
    std::unique_ptr<ga::Manager::PhaseSavepoint> lifecycle;
    std::unique_ptr<ai::GalacticWorld::PhaseSavepoint> perception;
    std::unique_ptr<ai::GoalStateContributor::PhaseSavepoint> goals;
    std::unique_ptr<ai::GalacticFunding::PhaseSavepoint> funding;
    std::unique_ptr<ga::NativeActions::PhaseSavepoint> actions;
    std::unique_ptr<ga::LiveExecutionStateContributor::PhaseSavepoint> execution;
    std::unique_ptr<foc::AiJournal::PhaseSavepoint> journal;
    std::unique_ptr<foc::tactical_ai_detail::ScriptEvaluators::PhaseSavepoint> evaluators;
    std::vector<std::pair<std::shared_ptr<GalacticAiUsabilityProvider>, std::unique_ptr<GalacticAiUsabilityProvider::PhaseSavepoint>>> usability;
    decltype(Runtime::connectivity) connectivity, route_source, combined;
    decltype(Runtime::route_publications) route_publications;
    std::map<ga::gc::PlayerId, decltype(Runtime::PlayerRuntimeState::initial)> initial;
    std::uint64_t sequence{};
    std::size_t diagnostics{}, window_diagnostics{}, window_load_notes{};
    bool build_tasks{}, recoverable_external{true};

    NativePhaseAttempt(Runtime& runtime, std::shared_ptr<ga::Manager> retained_manager)
        : owner(runtime), manager(std::move(retained_manager)), connectivity(runtime.connectivity), route_source(runtime.route_source),
          combined(runtime.combined_routes), route_publications(runtime.route_publications), sequence(runtime.sequence),
          diagnostics(runtime.setup->load_diagnostics.size()), window_diagnostics(runtime.window_diagnostics.size()),
          window_load_notes(runtime.window_load_notes.size()),
          build_tasks(runtime.build_tasks_ready) {
        // Mod/player coordination headers, never a world-entity domain.
        for (const auto& [player, slice] : owner.player_runtime) initial.emplace(player, slice.initial);
        owner.phase_active = true; owner.tail_fault_provider.reset(); owner.engine->reset_native_fault_context();
    }
    ~NativePhaseAttempt() {
        owner.native_actions_barrier.reset(); // unsettled attempts preserve the existing terminal refusal
        owner.outbox.abandon_batch();
        owner.phase_active = false; owner.active_scripts = nullptr; owner.active_executor = nullptr;
        owner.actions->set_executor(owner.setup->executor.get()); owner.clear_path_tables();
    }
    core::Result<void> begin(auth::ScriptScheduler& scheduler, const sim::PartitionExecutor& executor) {
        const auto enlist = [](auto& token, auto begun) -> core::Result<void> {
            if (!begun) return core::Result<void>::failure(begun.error());
            token = std::move(begun).value(); return core::Result<void>::success();
        };
        if (auto result = enlist(output, owner.galaxy->begin_native_output_phase()); !result) return result;
        if (auto result = enlist(scripts, scheduler.begin_phase(executor)); !result) return result;
        if (auto result = enlist(lifecycle, manager->begin_phase()); !result) return result;
        if (auto result = enlist(perception, owner.world->begin_phase(executor)); !result) return result;
        if (auto result = enlist(goals, owner.goals->begin_phase()); !result) return result;
        if (auto result = enlist(funding, owner.funding->begin_phase()); !result) return result;
        if (auto result = enlist(actions, owner.actions->begin_phase()); !result) return result;
        // Custom providers may retain legacy external execution writers. Their
        // healthy path remains supported, but no sparse rollback can account
        // for writes that bypass this participant's owned mutators.
        if (owner.providers.execution_writers_tracked) {
            if (auto result = enlist(execution, owner.execution->begin_phase()); !result) return result;
        } else recoverable_external = false;
        if (owner.host->setup.journal) journal = owner.host->setup.journal->begin_phase();
        if (auto* evaluator = owner.engine->script_evaluators())
            if (auto result = enlist(evaluators, evaluator->begin_phase(executor)); !result) return result;
        for (const auto& [name, contributor] : owner.providers.external) {
            static_cast<void>(name);
            auto stock = std::dynamic_pointer_cast<GalacticAiUsabilityProvider>(contributor);
            if (!stock) { recoverable_external = false; continue; }
            auto begun = stock->begin_phase(); if (!begun) return core::Result<void>::failure(begun.error());
            usability.emplace_back(std::move(stock), std::move(begun).value());
        }
        for (auto& [provider, token] : usability) {
            static_cast<void>(token);
            if (auto rebuilt = provider->rebuild_phase_inputs(*owner.engine, executor); !rebuilt) return rebuilt;
        }
        return core::Result<void>::success();
    }
    core::Result<void> commit() {
        // All deterministic service callbacks have completed. The ordered
        // world publication may itself call external observers; failure here
        // remains infrastructure refusal and never retries.
        if (owner.native_actions_barrier) {
            auto committed = owner.finish_native_lane();
            if (!committed) { output->record_fault(); return committed; }
        }
        if (auto result = scripts->commit(); !result) return result;
        if (auto result = funding->commit(); !result) return result;
        if (auto result = actions->commit(); !result) return result;
        if (execution) if (auto result = execution->commit(); !result) return result;
        if (auto result = perception->commit(); !result) return result;
        for (auto& [provider, token] : usability) { static_cast<void>(provider); if (auto result = token->commit(); !result) return result; }
        if (evaluators) if (auto result = evaluators->commit(); !result) return result;
        if (auto result = goals->commit(); !result) return result;
        if (auto result = lifecycle->commit(); !result) return result;
        if (journal) journal->commit();
        return core::Result<void>::success();
    }
    core::Result<void> rollback() {
        if (output->published() || !recoverable_external) return absent<void>("unpublished complete native rollback ownership");
        if (owner.native_actions_barrier) {
            if (auto result = owner.native_actions_barrier->discard(); !result) return result;
            owner.native_actions_barrier.reset();
        }
        owner.outbox.abandon_batch();
        if (auto result = funding->rollback(); !result) return result;
        if (auto result = actions->rollback(); !result) return result;
        if (execution) if (auto result = execution->rollback(); !result) return result;
        if (auto result = perception->rollback(); !result) return result;
        owner.host->view = owner.world->view();
        if (auto result = goals->rollback(); !result) return result;
        if (auto result = lifecycle->rollback(); !result) return result;
        if (evaluators) if (auto result = evaluators->rollback(); !result) return result;
        for (auto& [provider, token] : usability) { static_cast<void>(provider); if (auto result = token->rollback(); !result) return result; }
        if (auto result = scripts->rollback(); !result) return result;
        if (journal) journal->rollback();
        owner.connectivity = std::move(connectivity); owner.route_source = std::move(route_source); owner.combined_routes = std::move(combined);
        owner.route_publications = std::move(route_publications); owner.sequence = sequence; owner.build_tasks_ready = build_tasks;
        owner.setup->load_diagnostics.resize(diagnostics); owner.window_diagnostics.resize(window_diagnostics);
        owner.window_load_notes.resize(window_load_notes);
        for (auto& [player, slice] : owner.player_runtime) {
            slice.initial = std::move(initial.at(player)); slice.path_tables.clear();
            for (auto& [reader, memo] : slice.economy) {
                static_cast<void>(reader); memo.budget_players.reset(); memo.budget_players_revision.reset();
                memo.last_economy.reset(); memo.build_budgets_cached = false;
            }
        }
        return core::Result<void>::success();
    }
};

class ScriptEngine final : public auth::GalacticScriptEngine {
public:
    ScriptEngine(std::shared_ptr<Runtime> owner, std::shared_ptr<ga::Manager> manager)
        : owner_(std::move(owner)), manager_(std::move(manager)) {}
    bool uses_sync_hash() const noexcept override { return true; }
    std::uint64_t frame_offset() const noexcept override {
        const auto real = owner_->galaxy->clock().galactic_frame;
        return manager_->virtual_mode(real) - real;
    }
    bool verifies_sync_hash() const noexcept override {
        const auto state = owner_->native_state.lock();
        return state && state->full_verification_enabled();
    }
    // W1 S4: the AI scheduler's sync hash as the newest visible publication holds it (async), instead of the live scheduler.
    std::optional<std::string> published_ai_sync_hash() const override {
        if (owner_->outbox.schedule().apply_delay == 0) return std::nullopt;
        const auto* visible = owner_->billing_view.visible();
        if (!visible || visible->scheduler_digest.empty()) return std::nullopt;
        return visible->scheduler_digest;
    }
    bool admits_service(const ga::gc::GalacticTick& tick) const noexcept override { return manager_->admits_service(tick); }
    core::Result<auth::ServiceReport> service_phase(const sim::PartitionExecutor& executor,
        ga::gc::GalacticSession& parent, const ga::gc::GalacticTick&, auth::ScriptScheduler& scripts,
        const std::function<core::Result<auth::ServiceReport>()>& run) override {
        using Result = core::Result<auth::ServiceReport>;
        if (owner_->phase_active || owner_->galaxy != &parent) return absent<auth::ServiceReport>("idle native service phase owner");
        // Admitted AI tick `ai_index`. A deferred schedule applies what is due first, so the batch snapshot sees it; the
        // synchronous reference applies its own batch at the end of this phase. Held ticks never reach this point.
        const auto& schedule = owner_->outbox.schedule();
        owner_->ai_index = owner_->ai_ticks++;
        owner_->released_commands.clear();
        owner_->active_executor = &executor;
        const std::uint32_t window = owner_->ai_index % schedule.batch_ticks == 0 ? schedule.batch_ticks : 0;
        // From here to the end of the phase the AI's own work runs: deliveries apply at once (the guard reopens the inbox on exit).
        struct WindowGuard {
            Runtime& owner;
            explicit WindowGuard(Runtime& value) : owner(value) { owner.window_open = true; }
            ~WindowGuard() { owner.window_open = false; }
        } window_guard(*owner_);
        // A2.1 cut: a window first applies what the world delivered during the steps before it, in arrival order, before the due
        // slices and before its own decisions (the order the deliveries had when they applied at once).
        GalacticAiDueOutput due;
        owner_->due_plot_notes.clear();
        if (schedule.apply_delay != 0) {
            // W1 S3: the first serviced tick publishes the AI budget as it stands (nothing logged yet); later windows publish
            // theirs from their first apply tick. Before the drain, so a drain is always checked against a publication.
            const auto begin = std::chrono::steady_clock::now();
            const bool bootstrap = !owner_->billing_view.active() && !owner_->billing_view.has_pending();
            if (bootstrap) if (auto captured = owner_->capture_publication(owner_->ai_index, scripts, executor); !captured) {
                owner_->active_executor = nullptr; return Result::failure(captured.error());
            }
            if (auto published = owner_->billing_view.publish_due(owner_->ai_index, &due); !published) {
                owner_->active_executor = nullptr; return Result::failure(published.error());
            }
            // The window's own load notes come first (they were raised in it). W1 S5: then the AI's GlobalValue writes of the
            // windows visible now reach the story store, so this tick's registry rebuild (window start) already holds them.
            for (auto& note : due.load_notes) owner_->due_plot_notes.push_back(std::move(note));
            if (owner_->story_globals.active()) owner_->story_globals.merge(due.global_writes, owner_->due_plot_notes);
            // Credits the world queued before the lane's first serviced tick are not in that first publication: log them, so the
            // view stays the AI's budget once the drain applies them. (No bill can be queued before it: the AI has no purchase yet.)
            if (bootstrap) for (const auto& entry : owner_->inbox.entries())
                if (const auto* credit = std::get_if<ga::gc::GalacticSession::CreditNotification>(&entry))
                    if (auto logged = owner_->billing_view.credit(*credit); !logged) {
                        owner_->active_executor = nullptr; return Result::failure(logged.error());
                    }
            owner_->note_billing_time(begin);
        }
        if (window) {
            if (auto drained = owner_->drain_inbox(); !drained) { owner_->active_executor = nullptr; return Result::failure(drained.error()); }
            // W1 S5: the AI's registry for this window = the story store (story writes up to now, AI writes visible up to now) with the
            // AI's own writes that are published but not visible yet replayed on top: it sees its own writes at once, the story's now.
            if (owner_->story_globals.active()) {
                auto replica = owner_->story_globals.values();
                for (auto& [key, value] : owner_->billing_view.pending_global_writes()) {
                    if (value) replica.insert_or_assign(std::move(key), std::move(*value)); else replica.erase(key);
                }
                owner_->engine->replace_globals(std::move(replica));
            }
        }
        if (schedule.apply_delay != 0) {
            auto applied = owner_->apply_due_slices();
            owner_->active_executor = nullptr;
            if (!applied) return Result::failure(applied.error());
            // W1 S6: the plot edits of the windows that became visible now, after their world actions (the window's order: decided
            // actions, then the plan scripts' plot edits). An edit the plot engine refuses is a runtime note, as when it applied at once.
            for (auto& line : due.diagnostics) owner_->setup->load_diagnostics.push_back(std::move(line));
            for (const auto& plot : due.plot_commands) {
                if (!owner_->apply_plot) break;
                if (auto done = owner_->apply_plot(plot); !done) {
                    core::LoadNote note; note.code = "EAWR-LOAD-1400";
                    note.object = "Get_Story_Plot:" + plot.subplot; note.tag = "Get_Story_Plot"; note.phase = core::LoadPhase::runtime;
                    note.action = core::LoadAction::runtime_degraded;
                    note.diagnostic_code = std::string(auth::codes::unsupported_api);
                    note.message = "story plot request not applied: " + done.error().message;
                    owner_->due_plot_notes.push_back(std::move(note));
                }
            }
        }
        if (!window) {
            auth::ServiceReport idle; idle.tick = scripts.completed_tick();
            idle.commands = std::move(owner_->released_commands); owner_->released_commands.clear();
            idle.load_notes = std::move(owner_->due_plot_notes); owner_->due_plot_notes.clear();
            return Result::success(std::move(idle));
        }
        std::vector<core::LoadNote> notes;
        for (std::size_t attempt_index = 0; attempt_index <= owner_->setup->lifecycle.players.size(); ++attempt_index) {
            NativePhaseAttempt attempt(*owner_, manager_);
            owner_->plot_buffer.clear(); // a retried attempt republishes only its own plot edits
            owner_->engine->clear_global_writes(); // and only its own GlobalValue writes
            if (auto begun = attempt.begin(scripts, executor); !begun) return Result::failure(begun.error());
            bool infrastructure{};
            auto reason = ga::NativeProviderFault::result;
            const auto failure = [](std::string message) {
                core::Diagnostic diagnostic; diagnostic.code = auth::codes::session_abort; diagnostic.message = std::move(message);
                return Result::failure(std::move(diagnostic));
            };
            const auto serve_window = [&]() -> Result {
                auth::ServiceReport total;
                for (std::uint32_t offset = 0; offset < window; ++offset) {
                    owner_->virtual_offset = offset; manager_->set_virtual_tick(offset + 1);
                    auto served = run();
                    if (!served) { owner_->virtual_offset = 0; manager_->set_virtual_tick(1); return served; }
                    auto& one = served.value();
                    total.tick = one.tick;
                    const auto append = [](auto& to, auto& from) { to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end())); };
                    append(total.commands, one.commands); append(total.diagnostics, one.diagnostics);
                    append(total.removed_instances, one.removed_instances); append(total.loads, one.loads);
                    append(total.managed_releases, one.managed_releases); append(total.load_notes, one.load_notes);
                    total.managed_handles.objects += one.managed_handles.objects; total.managed_handles.edges += one.managed_handles.edges;
                    total.managed_handles.entries += one.managed_handles.entries; total.managed_handles.host_values += one.managed_handles.host_values;
                    total.managed_handles.censuses += one.managed_handles.censuses; total.managed_handles.released += one.managed_handles.released;
                    total.managed_handles.nanoseconds += one.managed_handles.nanoseconds;
                    total.managed_handles.service_nanoseconds += one.managed_handles.service_nanoseconds;
                }
                owner_->virtual_offset = 0; manager_->set_virtual_tick(1);
                return Result::success(std::move(total));
            };
            auto result = [&]() -> Result {
                try { return serve_window(); }
                catch (const foc::tactical_ai_detail::EvaluatorInfrastructureError& error) { infrastructure = true; return failure(error.what()); }
                catch (const std::bad_alloc& error) { infrastructure = true; return failure(error.what()); }
                catch (const std::system_error& error) { infrastructure = true; return failure(error.what()); }
                catch (const std::exception& error) { reason = ga::NativeProviderFault::deterministic_exception; return failure(error.what()); }
                catch (...) { infrastructure = true; return failure("unknown native service infrastructure exception"); }
            }();
            owner_->virtual_offset = 0; manager_->set_virtual_tick(1);
            if (result) {
                if (auto committed = attempt.commit(); !committed) { attempt.output->record_fault(); return Result::failure(committed.error()); }
                result.value().load_notes.insert(result.value().load_notes.end(), notes.begin(), notes.end());
                if (schedule.apply_delay != 0) {
                    // W1 S6 (review #6425): the window's own load notes (its scripts' notes, native provider retirements) reach the
                    // world with its publication, at its apply tick; the report carries only the notes due now.
                    auto& window_notes = owner_->window_load_notes;
                    window_notes.insert(window_notes.end(), std::make_move_iterator(result.value().load_notes.begin()),
                        std::make_move_iterator(result.value().load_notes.end()));
                    result.value().load_notes.clear();
                    if (auto captured = owner_->capture_publication(owner_->ai_index + schedule.apply_delay, scripts, executor); !captured)
                        return Result::failure(captured.error());
                }
                result.value().load_notes.insert(result.value().load_notes.end(), owner_->due_plot_notes.begin(), owner_->due_plot_notes.end());
                owner_->due_plot_notes.clear();
                auto& released = owner_->released_commands;
                result.value().commands.insert(result.value().commands.end(), std::make_move_iterator(released.begin()),
                    std::make_move_iterator(released.end()));
                released.clear();
                return result;
            }
            // A callback is publication even when it fails or throws. Nothing
            // rolls back or reruns once that output is observable.
            if (attempt.output->published()) { attempt.output->record_fault(); return result; }
            const auto& diagnostic = result.error();
            infrastructure = infrastructure || diagnostic.code == sim::diagnostic_codes::worker_failure
                || diagnostic.code == ga::gc::diagnostic_codes::worker_failure || diagnostic.code == sim::tactical::diagnostic_codes::worker_failure
                || diagnostic.code == auth::codes::save_refused || diagnostic.code == auth::codes::load_rejected;
            auto faulted = manager_->faulted_provider();
            if (!faulted) faulted = owner_->engine->native_fault_provider();
            if (!faulted) faulted = owner_->tail_fault_provider;
            if (infrastructure || !attempt.recoverable_external || !faulted || !*faulted || manager_->provider_retired(*faulted)) return result;
            if (manager_->provider_threw() || owner_->engine->native_provider_threw()) reason = ga::NativeProviderFault::deterministic_exception;
            if (auto restored = attempt.rollback(); !restored) {
                auto error = restored.error();
                error.message += " (recovering from: " + diagnostic.message + ")";
                return Result::failure(std::move(error));
            }
            if (auto retired = manager_->retire_provider(*faulted, reason); !retired) return Result::failure(retired.error());
            if (auto retired = owner_->engine->retire_galactic_provider(*faulted, scripts); !retired) return Result::failure(retired.error());
            for (auto& note : manager_->retirement_notes())
                if (note.object == "native galactic player " + std::to_string(*faulted)) notes.push_back(std::move(note));
            // The completed attempt goes out of scope before any participant
            // begins again. Successful siblings recompute from the same root.
        }
        return absent<auth::ServiceReport>("bounded native provider retirement progress");
    }
    core::Result<auth::ServiceOptions> before_service(const sim::PartitionExecutor& executor,
        const ga::gc::GalacticSession& parent, const ga::gc::GalacticTick& tick, auth::ScriptScheduler& scripts) override {
        const auto reads = owner_->world->owner_reads();
        const auto native_record_write = owner_->engine->native_record_scope();
        auto cleanup = std::make_shared<NativeBarrierCleanup>(owner_);
        owner_->active_executor = &executor;
        owner_->actions->set_executor(&executor);
        if (auto overlaid = owner_->checked_native(owner_->build_window_overlay(executor)); !overlaid)
            return core::Result<auth::ServiceOptions>::failure(overlaid.error());
        if (auto prepared = owner_->prepare(executor, parent, tick); !prepared) return core::Result<auth::ServiceOptions>::failure(prepared.error());
        const ga::Clocks clocks{tick.completed_tick + owner_->virtual_offset, owner_->virtual_frame()};
        if (auto begun = owner_->checked_native(owner_->begin_native_actions_barrier()); !begun)
            return core::Result<auth::ServiceOptions>::failure(begun.error());
        cleanup->barrier = owner_->native_actions_barrier.get();
        owner_->sequence = 0; // EventKey sequence is local to this synchronization barrier.
        core::Result<void> begun = [&] {
            gc_profile::Scope begin_scope(gc_profile::Phase::barrier_begin);
            return owner_->engine->begin_galactic_barrier(*owner_->world, clocks, scripts, owner_->sequence,
                owner_->setup->lifecycle.players, &owner_->pass_executor);
        }();
        if (!begun) {
            if (!owner_->phase_active) { owner_->native_actions_barrier.reset(); owner_->outbox.abandon_batch(); }
            return core::Result<auth::ServiceOptions>::failure(begun.error());
        }
        owner_->engine->set_galactic_funding(owner_->funding);
        owner_->engine->defer_galactic_effects(true); // S9a: the players' engine effects commit once, at finish_barrier
        if (auto tables = owner_->prepare_path_tables(executor, parent, tick); !tables)
            return core::Result<auth::ServiceOptions>::failure(tables.error());
        if (owner_->native_actions_barrier) {
            auto started = owner_->checked_native(owner_->native_actions_barrier->begin_players());
            if (!started) return core::Result<auth::ServiceOptions>::failure(started.error());
        }
        owner_->active_scripts = &scripts;
        auto options = owner_->checked_native(manager_->before_service(executor, parent, tick, scripts));
        if (!options) return options;
        // The bridge can return on scheduler refusal without after_service.
        // Its local options retain this cleanup through both hooks and unwind.
        // S9a: each AI player's scripts run as a chain of their own over their own immediate-effect shadow.
        options.value().chain_of = [engine = owner_->engine](std::uint64_t instance) { return engine->galactic_chain_owner(instance); };
        auto paced = std::move(options.value().host_paced);
        options.value().host_paced = [paced = std::move(paced), cleanup = std::move(cleanup)](std::uint64_t instance) {
            static_cast<void>(cleanup);
            return paced && paced(instance);
        };
        return options;
    }
    core::Result<void> after_service(auth::ServiceReport& report) override {
        const auto native_record_write = owner_->engine->native_record_scope();
        auto result = owner_->checked_native(manager_->after_service(report));
        if (!owner_->phase_active) {
            owner_->clear_path_tables();
            owner_->active_executor = nullptr; owner_->actions->set_executor(owner_->setup->executor.get());
        }
        return result;
    }
private:
    struct NativeBarrierCleanup {
        std::shared_ptr<Runtime> owner;
        const ga::gc::GalacticSession::NativeActionBarrier* barrier{};
        explicit NativeBarrierCleanup(std::shared_ptr<Runtime> runtime) : owner(std::move(runtime)) {}
        ~NativeBarrierCleanup() {
            if (owner->phase_active) return;
            if (owner->native_actions_barrier && owner->native_actions_barrier.get() != barrier) return;
            owner->native_actions_barrier.reset();
            owner->clear_path_tables();
            owner->active_executor = nullptr;
            owner->actions->set_executor(owner->setup->executor.get());
        }
    };
    std::shared_ptr<Runtime> owner_;
    std::shared_ptr<ga::Manager> manager_;
};
} // namespace

core::Result<ga::ExecutionRouteInput> copy_stock_execution_input(const GalacticAiRuntimeSetup& setup,
    ga::gc::PlayerId player, const ga::gc::GalacticSnapshot& snapshot, const ai::GalacticWorld& world,
    const ai::Engine& engine, const GalacticAiUsability& usable, const ai::DerivedExecutionRestore* reconstruction, bool read_only) {
    using Result = core::Result<ga::ExecutionRouteInput>;
    if (!usable || !setup.perception.maximum_fleet_distance) return absent<ga::ExecutionRouteInput>("qualified execution usability/movement limit");
    const auto selected = std::find_if(setup.lifecycle.players.begin(), setup.lifecycle.players.end(),
        [player](const auto& row) { return row.player == player; });
    if (selected == setup.lifecycle.players.end() || !snapshot.control() || !snapshot.production())
        return absent<ga::ExecutionRouteInput>("selected execution player/control/production facts");
    // WGA-129: raw category-unbound perception, retaining B's cache semantics.
    auto equation = ai::parse_equation("EXECUTION_ENEMY_SPACE", "Variable_Target.EnemyForce.SpaceTotalUnnormalized", {});
    if (!equation) return Result::failure(equation.error());
    ga::ExecutionRouteInput out; out.player = player; out.maximum_distance = *setup.perception.maximum_fleet_distance;
    out.trade_routes.assign(snapshot.routes().begin(), snapshot.routes().end());
    ai::AiRandom unused_random;
    for (const auto& planet : snapshot.planets()) {
        const auto z = setup.route_z_adjust.find(planet.id);
        if (z == setup.route_z_adjust.end()) return absent<ga::ExecutionRouteInput>("qualified source planet Layer_Z_Adjust");
        auto usability = usable(player, snapshot, planet.id);
        if (!usability) return Result::failure(usability.error());
        const auto* target = engine.target_of_object(planet.id);
        if (!target) return absent<ga::ExecutionRouteInput>("retained execution planet target");
        const ai::Context context{player, selected->enemy, selected->human, target};
        const auto power = reconstruction ? world.reconstruction_enemy_space(*reconstruction, context)
            : read_only ? world.perception_read_only(engine, equation.value(), context, unused_random)
            : world.perception(engine, equation.value(), context, unused_random);
        if (!power) return absent<ga::ExecutionRouteInput>("cached enemy space total unnormalized");
        const auto relation = std::find_if(snapshot.control()->relations.begin(), snapshot.control()->relations.end(),
            [&](const auto& row) { return row.player == player && row.other == planet.owner; });
        if (relation == snapshot.control()->relations.end()) return absent<ga::ExecutionRouteInput>("execution planet alliance relation");
        ai::Real starbase{};
        const auto production = std::find_if(snapshot.production()->planets.begin(), snapshot.production()->planets.end(),
            [&](const auto& row) { return row.planet == planet.id; });
        if (production == snapshot.production()->planets.end()) return absent<ga::ExecutionRouteInput>("current execution starbase fact");
        if (!relation->allied && production->starbase_type) {
            const auto type = std::find_if(setup.perception.force_types.begin(), setup.perception.force_types.end(),
                [&](const auto& row) { return row.type == production->starbase_type; });
            if (type == setup.perception.force_types.end()) return absent<ga::ExecutionRouteInput>("current original starbase metric");
            starbase = type->power;
        }
        out.planets.push_back({planet.id, planet.position, z->second, *power, starbase, usability.value()});
    }
    return Result::success(std::move(out));
}

std::optional<sim::galactic::PlayerId> shared_story_local_player(const CampaignSetupPlan& plan) {
    if (plan.mode() == CampaignSetupMode::multiplayer && plan.rules_flavour() == CampaignRulesFlavour::single_player
        && plan.human_observers().size() == 1)
        return plan.human_observers().front();
    return std::nullopt;
}

core::Result<std::vector<ga::PlayerSelection>> select_live_galactic_ai_players(
    const data::galactic::GalacticData& data, const CampaignSetupPlan& plan, const ai::AiData& loaded,
    std::string_view authored_difficulty) {
    std::vector<ga::PlayerSelection> selected;
    // WGA-02 / WGS-08: single-player selection uses authored campaign pairs.
    // GCMP-AI-01 is the project's multiplayer AI extension, not retail parity.
    // Both paths select only active AI controllers; difficulty names remain authored.
    const auto* campaign = data.campaign(plan.campaign_name());
    if (!campaign) return absent<std::vector<ga::PlayerSelection>>("authored campaign AI selection");
    std::string level;
    const auto authored_adjustment = ai::upper_case(authored_difficulty);
    const auto human = [&](sim::galactic::PlayerId id) {
        return std::any_of(plan.galaxy().seats.begin(), plan.galaxy().seats.end(), [&](const auto& seat) {
            return seat.active && seat.player == id && seat.kind == sim::galactic::SeatKind::human;
        });
    };
    std::optional<sim::galactic::PlayerId> human_player;
    for (const auto& binding : plan.player_bindings())
        if (binding.active && human(binding.player.player_id)) { human_player = binding.player.player_id; break; }
    for (const auto& binding : plan.player_bindings()) {
        const auto id = binding.player.player_id;
        const bool controlled = std::any_of(plan.galaxy().seats.begin(), plan.galaxy().seats.end(), [&](const auto& seat) {
            return seat.active && seat.player == id && seat.kind == sim::galactic::SeatKind::ai;
        });
        if (!binding.active || !controlled || human(id)) continue;
        const auto* faction = data.faction(binding.faction);
        if (!faction) return absent<std::vector<ga::PlayerSelection>>("AI faction relations");
        // Neutral seat metadata does not create a galactic goal owner; the
        // existing native goal checkpoint contract excludes neutral players.
        if (faction->is_neutral.value_or(false)) continue;
        std::string_view player_type;
        if (plan.rules_flavour() == CampaignRulesFlavour::multiplayer && faction->is_playable.value_or(false)) {
            player_type = faction->basic_ai;
            if (player_type.empty()) return absent<std::vector<ga::PlayerSelection>>("multiplayer playable faction Basic_AI");
        } else {
            // Ambient factions retain the authored selection/absence they had before
            // the extension. Neutral remains outside native goal ownership above.
            const auto assignment = std::find_if(campaign->ai_player_control.begin(), campaign->ai_player_control.end(),
                [&](const auto& row) { return ai::upper_case(row.faction) == ai::upper_case(binding.faction); });
            if (assignment == campaign->ai_player_control.end()) continue;
            player_type = assignment->name;
        }
        const auto type = loaded.players.find(ai::upper_case(player_type));
        if (type == loaded.players.end()) return absent<std::vector<ga::PlayerSelection>>("authored AI player type");
        for (const auto& [candidate, adjustment] : type->second.difficulty) if (adjustment == authored_adjustment) {
            if (!level.empty() && level != candidate)
                return absent<std::vector<ga::PlayerSelection>>("unambiguous authored difficulty level");
            level = candidate;
        }
        // The level is one setting for every AI player; each type maps it to its own adjustment (a mod's minor and
        // story players author their own names), so a type that does not author the chosen adjustment still plays at
        // the chosen level. It is applied to all players below, once the level is known.
        ga::PlayerSelection player{id, std::string(player_type), level, {}, human_player};
        // WGA-431: prefer a human enemy, otherwise the last non-rogue
        // enemy in player order. Relations are directional (WGS-85).
        for (const auto& other : plan.player_bindings()) {
            const auto* enemy = data.faction(other.faction);
            if (!other.active || !enemy || enemy->is_neutral.value_or(false) || other.player.player_id == id) continue;
            const bool hostile = std::any_of(faction->enemies.begin(), faction->enemies.end(),
                [&](const auto& name) { return ai::upper_case(name) == ai::upper_case(other.faction); });
            if (!hostile) continue;
            player.enemy = other.player.player_id;
            if (human(other.player.player_id)) break;
        }
        selected.push_back(std::move(player));
    }
    if (level.empty() && !selected.empty()) return absent<std::vector<ga::PlayerSelection>>("authored difficulty level");
    for (auto& player : selected) player.difficulty = level;
    return core::Result<std::vector<ga::PlayerSelection>>::success(std::move(selected));
}

core::Result<std::shared_ptr<const GalacticAiRuntimeSetup>> prepare_stock_galactic_ai_runtime(
    const vfs::Vfs& files, const data::Catalog& catalog, const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& types, const CampaignSetupPlan& plan, std::vector<ga::PlayerSelection> selected,
    std::shared_ptr<const sim::PartitionExecutor> executor, bool uses_fog, std::size_t instances, std::size_t requests,
    std::string_view authored_difficulty, ga::ServicePeriods service_periods, ga::AsyncSchedule schedule) {
    using Result = core::Result<std::shared_ptr<const GalacticAiRuntimeSetup>>;
    const auto require = []<class T>(core::Result<T> value) -> T {
        if (!value) { throw std::runtime_error(value.error().message); }
        return std::move(value).value();
    };
    try {
        auto out = std::make_shared<GalacticAiRuntimeSetup>();
        if (std::any_of(service_periods.begin(), service_periods.end(), [](auto frames) { return frames == 0; }))
            return absent<std::shared_ptr<const GalacticAiRuntimeSetup>>("positive AI service periods");
        out->lifecycle.service_periods = service_periods;
        out->lifecycle.schedule = schedule; // production: ga::standard_async_schedule (K = 15, D = 30)
        out->executor = std::move(executor); out->immediate_instances = instances; out->immediate_requests = requests;
        out->content = require(prepare_ai_selection_content(files, catalog, data, types, plan));
        out->lifecycle.players = std::move(selected);
        const auto read = [&](std::string_view path) {
            const auto bytes = require(files.open(path)); std::string text(bytes.size(), '\0');
            if (!text.empty()) { std::memcpy(text.data(), bytes.data(), bytes.size()); }
            return text;
        };
        for (const auto& path : ga::required_xml()) out->lifecycle.xml.emplace(path, read(path));
        // The selected mode closure includes authored None and its templates.
        out->lifecycle.xml_paths = ga::required_xml();
        for (const auto& asset : require(files.enumerate("data/xml/ai/players", ".xml"))) {
            if (!out->lifecycle.xml.contains(asset.canonical_path)) {
                out->lifecycle.xml.emplace(asset.canonical_path, read(asset.canonical_path));
                out->lifecycle.xml_paths.push_back(asset.canonical_path);
            }
        }
        // AI-16: every mounted AI directory participates, including templates whose owning
        // files are not in the initial closure. Successful equation parsing is not a discovery gate.
        for (const auto folder : {"perceptualequations", "goalfunctions", "goals", "templates"})
            for (const auto& asset : require(files.enumerate(std::string("data/xml/ai/") + folder, ".xml")))
                if (!out->lifecycle.xml.contains(asset.canonical_path))
                    out->lifecycle.xml.emplace(asset.canonical_path, read(asset.canonical_path));
        for (auto& line : ga::include_mounted_ai_files(out->lifecycle))
            out->load_diagnostics.push_back("galactic AI file skipped: " + std::move(line));
        const auto loaded = require(ga::load_data(out->lifecycle));
        if (!authored_difficulty.empty()) {
            if (!out->lifecycle.players.empty()) return absent<std::shared_ptr<const GalacticAiRuntimeSetup>>("one player selection source");
            out->lifecycle.players = require(select_live_galactic_ai_players(data, plan, loaded, authored_difficulty));
        }
        for (auto& line : ga::unresolved_goal_equations(loaded, out->lifecycle.players))
            out->load_diagnostics.push_back(std::move(line)); // #4937: the engine skips their function sets instead of refusing startup
        for (const auto& asset : require(files.enumerate("data/scripts", ".lua"))) {
            const auto& path = asset.canonical_path;
            if (path.starts_with("data/scripts/library/") || path.starts_with("data/scripts/freestore/")
                || ai::galactic_goals::plan_directory_index(path)) out->modules.emplace(path, read(path));
        }
        out->multiplayer = plan.mode() == CampaignSetupMode::multiplayer;
        // WST-91/92: all loaded factions, all registered players, and only the recorded canonical observer.
        for (const auto& faction : data.factions) out->story_players.factions.push_back(faction.name);
        for (const auto& binding : plan.player_bindings()) {
            const auto story_selection = std::find_if(out->lifecycle.players.begin(), out->lifecycle.players.end(),
                [&](const auto& row) { return row.player == binding.player.player_id; });
            out->story_players.players.push_back({binding.player.player_id, binding.faction,
                story_selection == out->lifecycle.players.end() ? "Normal" : story_selection->difficulty});
        }
        for (const auto& [id, type] : out->content->types) out->story_players.types.emplace_back(type.name, id);
        out->story_players.local_player = plan.solo_reference_human();
        out->story_players.shared_local_player = shared_story_local_player(plan);
        for (const auto& location : plan.locations()) {
            if (location.planet) out->story_players.planets.emplace_back(location.original_type, location.planet->id);
        }
        {
            // The scripts follow the plots: a campaign whose plots cannot be listed here simply runs none.
            auto plots = load_story_plots(files, data, plan);
            if (!plots) out->load_diagnostics.push_back("story script: plots not listed: " + plots.error().message);
            const std::vector<sim::galactic::story::SubPlotDef> none;
            out->story_scripts = require(GalacticStoryScripts::load(files, plots ? std::span<const sim::galactic::story::SubPlotDef>(plots.value().subplots) : std::span<const sim::galactic::story::SubPlotDef>(none)));
            for (const auto& line : out->story_scripts->notes()) out->load_diagnostics.push_back("story script: " + line);
        }
        // RGC-163: the plan files of all five directories are read; one registry serves every mode, so a plan joins the galactic
        // catalog when it names a galactic goal type (the first directory keeps every plan that loads). Keep the frozen
        // module inventory: an admitted plan can require a helper at definition time or later, including computed paths.
        out->plans = require(ai::galactic_goals::load_plan_catalog({}, out->modules, [&](const auto& definition) {
            return std::any_of(definition.goals.begin(), definition.goals.end(), [&](const auto& name) {
                const auto goal = loaded.goals.find(name);
                return goal != loaded.goals.end() && goal->second.mode == "GALACTIC";
            });
        }));
        for (const auto& line : out->plans.pause_differences) out->load_diagnostics.push_back("galactic AI plan: " + line);
        for (const auto& line : out->plans.item_diagnostics) out->load_diagnostics.push_back("galactic AI plan: " + line);
        for (const auto& line : out->plans.skipped_plans) out->load_diagnostics.push_back("galactic AI plan skipped: " + line);
        auto& perception = out->perception;
        if (out->lifecycle.players.empty()) return absent<std::shared_ptr<const GalacticAiRuntimeSetup>>("selected stock players");
        perception = *require(prepare_ai_perception_content(files, data, plan, *out->content, out->lifecycle.players.front().difficulty));
        for (const auto* token : {"FRIENDLYINTERNALCONNECTIVITY", "FRIENDLYEXTERNALCONNECTIVITY", "ENEMYINTERNALCONNECTIVITY", "ENEMYEXTERNALCONNECTIVITY"})
            perception.path_coefficients_requested |= loaded.equations.uses_lookup({"VARIABLE_TARGET", token});
        if (perception.path_coefficients_requested && !perception.trade_movement_factor)
            out->load_diagnostics.push_back("galactic path coefficients unavailable: TradeRouteMovementFactor has no numeric owner");
        for (const auto& line : perception.spawn_diagnostics) out->load_diagnostics.push_back("galactic AI spawn: " + line);
        if (plan.galaxy().seed != perception.seed) return absent<std::shared_ptr<const GalacticAiRuntimeSetup>>("qualified 32-bit AI seed");
        perception.uses_fog = uses_fog;
        for (auto& player : perception.players) {
            const auto chosen = std::find_if(out->lifecycle.players.begin(), out->lifecycle.players.end(),
                [&](const auto& row) { return row.player == player.player; });
            player.ai = chosen != out->lifecycle.players.end();
            if (player.ai) player.difficulty = chosen->difficulty;
        }
        for (const auto& location : plan.locations()) if (location.planet) {
            // CGS-22: a planet type with no loadable base is skipped (noted once by space-entry preparation) and keeps the
            // constructor default adjustment; any other resolve failure still refuses.
            const auto resolved_original = catalog.resolve(location.original_type, data::Category::game_object);
            if (!resolved_original && stock_name_skippable(resolved_original.error())) { out->route_z_adjust.emplace(location.planet->id, ai::Real{}); continue; }
            const auto original = require(resolved_original);
            if (const auto* value = original.value("Layer_Z_Adjust")) {
                const auto z = ai::parse_real(value->value.raw_text);
                if (!z) return absent<std::shared_ptr<const GalacticAiRuntimeSetup>>("source planet Layer_Z_Adjust number");
                out->route_z_adjust.emplace(location.planet->id, *z);
            } else out->route_z_adjust.emplace(location.planet->id, ai::Real{}); // WGA-257: shared original-type constructor default
        }
        std::string fingerprint = "eawr-galactic-runtime-content-v4";
        // The same bytes, also cut into named groups so a mismatch can be compared group by group.
        std::vector<std::pair<std::string, std::string>> groups;
        const auto group = [&](std::string_view name) { groups.emplace_back(std::string(name), std::string()); };
        const auto field = [&](std::string_view value) {
            const auto text = std::to_string(value.size()) + ":" + std::string(value);
            fingerprint += text;
            if (!groups.empty()) groups.back().second += text;
        };
        const auto node = [&](auto&& self, const data::XmlNode& value) -> void {
            field(value.name); field(value.raw_text); field(std::to_string(value.attributes.size()));
            for (const auto& attribute : value.attributes) { field(attribute.name); field(attribute.value); }
            field(std::to_string(value.children.size()));
            for (const auto& child : value.children) self(self, child);
        };
        group("types");
        for (const auto& binding : plan.type_bindings()) if (const auto* type = plan.original_type(binding.id)) {
            auto resolved = catalog.resolve(type->name, data::Category::game_object);
            if (!resolved && stock_name_skippable(resolved.error())) continue;
            const auto object = require(std::move(resolved));
            field(std::to_string(binding.id)); field(binding.original_type);
            for (const auto& value : object.values) node(node, value.value);
        }
        group("lifecycle-xml");
        for (const auto& [path, bytes] : out->lifecycle.xml) { field(path); field(bytes); }
        group("ai-modules");
        for (const auto& [path, bytes] : out->modules) { field(path); field(bytes); }
        group("story-scripts");
        field(out->story_scripts->fingerprint());
        group("story-players");
        if (!out->story_scripts->entries().empty()) {
            field("story-player-lookup-v3");
            field(std::to_string(out->story_players.factions.size()));
            for (const auto& faction : out->story_players.factions) field(faction);
            field(std::to_string(out->story_players.players.size()));
            for (const auto& player : out->story_players.players) { field(std::to_string(player.id)); field(player.faction); field(player.difficulty); }
            field(std::to_string(out->story_players.types.size()));
            for (const auto& [name, id] : out->story_players.types) { field(name); field(std::to_string(id)); }
            field(out->story_players.local_player ? std::to_string(*out->story_players.local_player) : "no-local-player");
            if (out->story_players.shared_local_player) field("shared-local-" + std::to_string(*out->story_players.shared_local_player));
            field(std::to_string(out->story_players.planets.size()));
            for (const auto& [name, id] : out->story_players.planets) { field(name); field(std::to_string(id)); }
        }
        group("multiplayer");
        if (out->multiplayer) field("multiplayer-story-random");
        group("registry-files");
        for (const auto& registry : catalog.registry_files()) if (registry.loaded && registry.input_sha256) { field(registry.included_path); field(*registry.input_sha256); }
        group("start-settings");
        field(plan.campaign_name()); field(uses_fog ? "fog" : "visible");
        field(perception.is_story_campaign ? (*perception.is_story_campaign ? "story" : "ordinary") : "story-unavailable");
        field(std::to_string(perception.markup_hints.size()));
        for (const auto& [player, hints] : perception.markup_hints) {
            field(std::to_string(player)); field(hints.set);
            field(std::to_string(hints.defaults.size()));
            for (const auto& [name, value] : hints.defaults) { field(name); field(std::to_string(value.repr)); }
            field(std::to_string(hints.planets.size()));
            for (const auto& [planet, values] : hints.planets) {
                field(planet); field(std::to_string(values.size()));
                for (const auto& [name, value] : values) { field(name); field(std::to_string(value.repr)); }
            }
        }
        field(std::to_string(perception.seed));
        group("ai-players");
        for (const auto& player : out->lifecycle.players) {
            field(std::to_string(player.player)); field(ai::upper_case(player.player_type)); field(ai::upper_case(player.difficulty));
            field(player.enemy ? std::to_string(*player.enemy) : "absent"); field(player.human ? std::to_string(*player.human) : "absent");
        }
        group("type-table");
        const auto digest = units::galactic_content_identity(types);
        field(core::sha256_hex(digest));
        field(std::to_string(out->content->registered_types.size()));
        for (auto id : out->content->registered_types) field(std::to_string(id));
        out->content_identity = core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(fingerprint.data()), fingerprint.size()));
        for (const auto& [name, bytes] : groups) {
            if (!out->content_identity_detail.empty()) out->content_identity_detail += ' ';
            out->content_identity_detail += name + "=" + core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())).substr(0, 8);
        }
        // Once per load, also on a load that goes through: the build that writes a save then leaves its groups readable
        // for comparison with a build that refuses it.
        out->load_diagnostics.push_back("galactic AI content identity " + out->content_identity.substr(0, 8) + ": " + out->content_identity_detail);
        static_cast<void>(loaded);
        return Result::success(std::move(out));
    } catch (const std::exception& error) { return absent<std::shared_ptr<const GalacticAiRuntimeSetup>>(error.what()); }
}

core::Result<checkpoint::GameLoadOptions::PreparedGalacticScripts> prepare_galactic_ai_runtime(
    std::shared_ptr<const GalacticAiRuntimeSetup> setup, GalacticAiRuntimeProviders providers, GalacticAiRuntimeBirth birth,
    std::vector<std::string>* plan_diagnostics) {
    using Prepared = checkpoint::GameLoadOptions::PreparedGalacticScripts;
    using Result = core::Result<Prepared>;
    if (!setup || !setup->executor || !setup->content) return absent<Prepared>("campaign content/executor");
    if (!providers.execution_input || (!providers.prepare_connectivity && !providers.prepare_connectivity_publication)) return absent<Prepared>("B live connectivity and execution route inputs");
    if (!providers.bind_actions) return absent<Prepared>("R2 native action/journal owner");
    if (!providers.prepare_battle || !providers.prepare_child_scripts) return absent<Prepared>("R4 retained listeners and child parent context");
    if (providers.freestore_types.empty()) return absent<Prepared>("original freestore type inventory");
    if (!providers.placement || !providers.shadow_placement || !setup->immediate_instances || !setup->immediate_requests)
        return absent<Prepared>("stock native placement and explicit immediate work budget");
    if (setup->content_identity.size() != 64 || setup->modules.empty() || setup->lifecycle.players.empty()) return absent<Prepared>("selected stock closure identity/modules/players");
    try {
        auto runtime = std::make_shared<Runtime>(); runtime->setup = std::move(setup); runtime->providers = std::move(providers);
        const auto& input = *runtime->setup;
        runtime->player_runtime.try_emplace(0);
        for (const auto& player : input.lifecycle.players) runtime->player_runtime.try_emplace(player.player);
        for (auto& [actor, slice] : runtime->player_runtime) {
            static_cast<void>(actor); slice.economy.try_emplace(0);
            for (const auto& player : input.perception.players) slice.economy.try_emplace(player.player);
        }
        auto loaded = ga::load_data(input.lifecycle); if (!loaded) return Result::failure(loaded.error());
        runtime->data = std::move(loaded).value();
        auto& host = *runtime->host; host.setup.seed = input.perception.seed; host.setup.content = input.content->ai;
        for (const auto& type : host.setup.content.types) { host.types.emplace(type.type_id, &type); host.types_by_name.emplace(ai::upper_case(type.name), &type); }
        for (const auto& [id, type] : input.content->types) runtime->pool->types_by_name.emplace(type.name, id);
        // WGA-439: Can_Land_On_Planet reads the authored planet restriction lists and original category masks.
        for (const auto& [id, planet] : input.content->planets)
            runtime->pool->planet_restrictions.emplace(id, ga::FreestorePlanetRestriction{planet.restricted_categories, planet.restricted_types});
        for (const auto& type : input.content->ai.types) runtime->pool->type_categories.emplace(type.type_id, type.category_bits);
        for (const auto& player : input.perception.players) {
            const auto selected = std::find_if(input.lifecycle.players.begin(), input.lifecycle.players.end(), [&](const auto& p) { return p.player == player.player; });
            host.setup.players.push_back({player.player, player.faction, player.neutral, selected != input.lifecycle.players.end(),
                selected != input.lifecycle.players.end() ? selected->player_type : "", player.human});
        }
        for (const auto& player : host.setup.players) host.players.emplace(player.player, &player);
        auto materialized = ai::galactic_goals::materialize_plan_catalog(host, input.plans);
        if (!materialized) return Result::failure(materialized.error());
        if (plan_diagnostics) *plan_diagnostics = materialized.value().diagnostics;
        runtime->plans = std::make_shared<const ai::galactic_goals::MaterializedPlanCatalog>(std::move(materialized).value());
        runtime->world = std::make_shared<ai::GalacticWorld>(input.perception); host.ai_world = runtime->world;
        runtime->world->enable_live_deltas(input.lifecycle.service_periods[static_cast<std::size_t>(ga::Service::perception)], input.executor);
        runtime->world->set_producer_executor_supplier([weak = std::weak_ptr<Runtime>(runtime)]() -> const sim::PartitionExecutor* {
            const auto owner = weak.lock(); return owner ? owner->current_executor() : nullptr;
        });
        runtime->engine = std::make_shared<ai::Engine>(runtime->host, runtime->data, runtime->plans->plans); host.engine = runtime->engine.get();
        runtime->engine->set_galactic_stage_catalog(runtime->plans);
        const auto weak_timeline = std::weak_ptr<Runtime>(runtime);
        auto child = runtime->providers.prepare_child_scripts(runtime->engine, runtime->world, [weak_timeline](ModeId mode) {
            const auto owner = weak_timeline.lock();
            if (!owner) return std::shared_ptr<GameSession::ChildTimelineValueReader>{};
            auto reader = std::make_shared<GameSession::ChildTimelineValueReader>();
            if (owner->child_timeline_factory) *reader = owner->child_timeline_factory(mode);
            else owner->pending_child_timelines.emplace_back(mode, reader);
            return reader;
        });
        if (!child) return Result::failure(child.error());
        runtime->child_scripts = std::move(child).value();
        if (!runtime->child_scripts.parent_queries) return absent<Prepared>("child parent queries");
        host.find_target_queries = std::make_shared<ai::GalacticFindTargetQueries>(*runtime->engine, *runtime->world, nullptr, nullptr, ga::Mode::galactic);
        if (auto contrast = foc::tactical_ai_detail::load_contrast(host, input.modules); !contrast) return Result::failure(contrast.error());
        auth::ModuleManifest manifest;
        for (const auto& [path, bytes] : input.modules) if (auto added = manifest.add(path, bytes); !added) return Result::failure(added.error());
        auth::SessionConfig config; config.seed = input.perception.seed; config.galactic_clock = true;
        config.tick_duration = {1, ga::gc::galactic_frames_per_second}; config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/FreeStore/"};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest)); if (!scripts) return Result::failure(scripts.error());
        std::vector<core::Diagnostic> diagnostics;
        foc::tactical_ai_detail::register_globals(scripts.value(), runtime->host, diagnostics);
        foc::tactical_ai_detail::register_methods(scripts.value(), runtime->host, diagnostics);
        if (!diagnostics.empty()) return Result::failure(diagnostics.front());
        if (auto registered = ga::register_freestore_bindings(scripts.value(), runtime->pool); !registered) return Result::failure(registered.error());
        if (auto registered = ai::register_galactic_markup_bindings(scripts.value(), runtime->world, runtime->engine,
            [weak = std::weak_ptr<Runtime>(runtime)](auto player, auto source, auto target) -> core::Result<std::vector<sim::galactic::PlanetId>> {
                auto owner = weak.lock();
                if (!owner || !owner->routes.contains(player)) return absent<std::vector<sim::galactic::PlanetId>>("retained markup execution path");
                auto path = ga::cached_execution_path(owner->routes.at(player), player, source, target);
                if (!path) return core::Result<std::vector<sim::galactic::PlanetId>>::failure(path.error());
                return core::Result<std::vector<sim::galactic::PlanetId>>::success(std::move(path).value().planets);
            }); !registered) return Result::failure(registered.error());
        if (runtime->providers.publish_freestore) {
            const auto weak = std::weak_ptr<Runtime>(runtime);
            if (auto registered = ga::register_qualified_freestore_bindings(scripts.value(), [weak] {
                auto owner = weak.lock(); return owner && owner->actions ? owner->actions->freestore_owner().lock() : nullptr;
            }); !registered) return Result::failure(registered.error());
        }
        // WST-53: sub-plot scripts live in the story scheduler; a campaign whose plots name none keeps it empty. (`setup` was moved into the runtime above.)
        runtime->story_link = std::make_shared<GalacticStoryLink>();
        auto story = runtime->setup->story_scripts ? create_galactic_story_scheduler(*runtime->setup->story_scripts, config,
            runtime->setup->multiplayer, runtime->setup->story_players,
            [weak = std::weak_ptr<Runtime>(runtime)]() -> std::shared_ptr<const ga::gc::GalacticSnapshot> {
                const auto owner = weak.lock();
                return owner && owner->galaxy ? owner->galaxy->snapshot() : nullptr;
            }, [weak = std::weak_ptr<Runtime>(runtime)](const std::string& key) -> std::optional<auth::Value> {
                const auto owner = weak.lock();
                if (!owner) return std::nullopt;
                // W1 S5: on the async schedule the story reads its own store; the AI's registry is the AI's.
                if (owner->story_globals.active()) {
                    const auto found = owner->story_globals.values().find(key);
                    if (found == owner->story_globals.values().end()) return std::nullopt;
                    return ai::Engine::global_value_of(found->second);
                }
                return owner->engine->global_value(key);
            }, runtime->story_link) : auth::ScriptScheduler::create(config, {});
        if (!story) return Result::failure(story.error());
        auto install = [runtime, birth](GameSession& game) -> core::Result<void> {
            if (!game.galactic_scripts()) return absent<void>("reconstructed galactic schedulers");
            // W1 S5: on the async schedule the story's GlobalValue writes go to the story-owned store (sim-owned, hashed live), seeded
            // from the registry as it stands; a restore then replaces it with the saved store.
            if (runtime->outbox.schedule().apply_delay != 0 && !runtime->story_globals.active())
                runtime->story_globals.activate(runtime->engine->globals());
            game.galactic_scripts()->set_global_value_committer([weak = std::weak_ptr<Runtime>(runtime)](auth::ServiceReport& report) {
                const auto owner = weak.lock();
                if (!owner) return;
                if (owner->story_globals.active()) owner->story_globals.commit(report);
                else owner->engine->commit_global_values(report);
            });
            runtime->world->bind_timeline_reader(game.active_timeline_reader());
            runtime->child_timeline_factory = game.child_timeline_reader_factory();
            for (const auto& [mode, reader] : runtime->pending_child_timelines)
                *reader = runtime->child_timeline_factory(mode);
            runtime->pending_child_timelines.clear();
            runtime->active_scripts = &game.galactic_scripts()->ai_scheduler();
            runtime->retained_scripts = runtime->active_scripts;
            // Story chunks may query live objects before their main thread starts.
            runtime->galaxy = &game.galactic_scripts()->parent_world();
            // WST-56: plan scripts (the retail interventions) reach the campaign's plots through Get_Story_Plot.
            if (const auto* plots = game.story_engine()) {
                // W1 S6: on the async schedule the plan scripts' plot edits are buffered with the window and applied by the world when
                // its publication becomes visible; the reference applies them at once.
                if (runtime->outbox.schedule().apply_delay != 0) {
                    runtime->apply_plot = game.story_plot_applier(); // bound to the session's heap state, not to the movable handle
                    runtime->host->story_plots = foc::StoryPlotLink::from(plots->definitions(),
                        [weak = std::weak_ptr<Runtime>(runtime)](const sim::galactic::story::PlotCommand& plot) -> core::Result<void> {
                            const auto owner = weak.lock();
                            if (!owner) return absent<void>("plot buffer owner");
                            owner->plot_buffer.push_back(plot);
                            return core::Result<void>::success();
                        });
                } else runtime->host->story_plots = foc::StoryPlotLink::from(plots->definitions(),
                    game.story_plot_applier()); // bound to the session's heap state, not to the movable handle
            }
            if (runtime->setup->story_scripts && !runtime->setup->story_scripts->entries().empty()) {
                game.bind_story_scripts(runtime->setup->story_scripts);
                // WST-37/55/56: the story scripts' plot edits, Story_Event and TRIGGER_AI flags, committed in command order.
                if (game.story_engine())
                    if (auto linked = game.connect_story_link(runtime->story_link); !linked) return linked;
                if (birth == GalacticAiRuntimeBirth::fresh_campaign) {
                    std::vector<std::string> started;
                    if (auto begun = start_galactic_story_scripts(game.galactic_scripts()->story_scheduler(), *runtime->setup->story_scripts, started); !begun)
                        return begun;
                    for (auto& line : started) runtime->setup->load_diagnostics.push_back("story script: " + std::move(line));
                }
            }
            const auto weak = std::weak_ptr<Runtime>(runtime);
            auto digest_work = std::make_shared<GalacticAiDigestWork>();
            GalacticAiStateAdapter measure;
            if (runtime->providers.observe) measure = [digest_work](auto state) -> core::Result<std::shared_ptr<ga::StateContributor>> {
                return core::Result<std::shared_ptr<ga::StateContributor>>::success(std::make_shared<MeasuredState>(std::move(state), digest_work));
            };
            auto installed = install_composed_galactic_ai(game, [runtime, weak, &game, birth](auto& galaxy) -> core::Result<GalacticAiContributors> {
                runtime->galaxy = &galaxy;
                if (!runtime->providers.publish_freestore) {
                    auto pool = ga::copy_freestore(*galaxy.snapshot(), runtime->providers.freestore_types, *runtime->setup->executor);
                    if (!pool) return core::Result<GalacticAiContributors>::failure(pool.error());
                    runtime->pool->pool = std::make_shared<const ga::FreestorePool>(std::move(pool).value());
                } else {
                    // No query/service runs during installation. The actual native
                    // owner publishes after construction and again after restore.
                    runtime->pool->pool = std::make_shared<const ga::FreestorePool>();
                }
                if (auto bound = runtime->world->bind(galaxy.snapshot(), {}, *runtime->setup->executor, galaxy.completed_tick(), galaxy.next_entity_id()); !bound)
                    return core::Result<GalacticAiContributors>::failure(bound.error());
                runtime->world->bind_economy_supplier([weak] {
                    auto owner = weak.lock(); return owner ? owner->copy_economy() : absent<std::shared_ptr<const ai::GalacticEconomyRuntime>>("retained economy publisher");
                });
                if (auto published = runtime->publish_economy(); !published) return core::Result<GalacticAiContributors>::failure(published.error());
                runtime->execution = std::make_shared<ga::LiveExecutionStateContributor>(runtime->retained, runtime->routes, runtime->pool,
                    [weak](auto player, const ai::DerivedExecutionRestore* reconstruction) -> core::Result<ga::ExecutionRouteInput> {
                        auto owner = weak.lock(); if (!owner || !owner->galaxy) return absent<ga::ExecutionRouteInput>("retained parent");
                        return owner->providers.execution_input(player, *owner->galaxy->snapshot(), *owner->world, *owner->engine, reconstruction);
                    }, [weak](const auto& state) -> core::Result<void> {
                        auto owner = weak.lock(); if (!owner) return absent<void>("execution native owner");
                        for (const auto& [player, row] : state) {
                            if (!owner->host->players.contains(player)) return absent<void>("restored execution player");
                            for (const auto& [object, plan] : row.reservations) if (!owner->engine->galactic_reservation_valid(player, object, plan)) return absent<void>("restored execution reservation identity");
                        }
                        return core::Result<void>::success();
                    }, *runtime->setup->executor);
                if (runtime->providers.execution_writers_tracked) runtime->execution->enable_revision_tracking();
                std::map<ga::gc::PlayerId, sim::math::Fixed> multipliers;
                for (const auto& selection : runtime->setup->lifecycle.players) {
                    const auto& player_type = runtime->data.players.at(ai::upper_case(selection.player_type));
                    const auto& difficulty = runtime->data.difficulties.at(player_type.difficulty.at(ai::upper_case(selection.difficulty)));
                    auto multiplier = script::numeric::to_fixed(difficulty.credit_multiplier);
                    if (!multiplier) return core::Result<GalacticAiContributors>::failure(multiplier.error());
                    multipliers.emplace(selection.player, multiplier.value());
                }
                runtime->credit_multipliers = multipliers;
                runtime->actions = std::make_shared<ga::NativeActions>(galaxy, runtime->funding, runtime->providers.placement, std::move(multipliers),
                    [weak](auto player, auto plan) -> std::optional<std::uint64_t> { auto owner = weak.lock(); return owner ? owner->engine->galactic_goal_for_plan(player, plan) : std::nullopt; },
                    runtime->setup->executor.get(), [content = runtime->setup->content](auto type) -> core::Result<sim::math::Fixed> {
                        const auto found = content->types.find(type); if (found == content->types.end()) return absent<sim::math::Fixed>("original build cost");
                        return script::numeric::to_fixed(ai::real(ai::truncate(found->second.value.cost)));
                    });
                runtime->actions->bind_reconciliation_placement(runtime->providers.reconciliation_placement);
                runtime->outbox = ga::IntentOutbox(runtime->setup->lifecycle.schedule);
                runtime->actions->bind_lane_floor([weak]() -> sim::EntityId {
                    const auto owner = weak.lock(); return owner ? owner->outbox.lane_floor() : sim::EntityId{};
                });
                runtime->actions->bind_invasion_pending([weak](std::uint64_t request) {
                    const auto owner = weak.lock(); return owner && owner->outbox.holds_invasion(request);
                });
                runtime->actions->bind_pending_freestore_replay([weak](ga::QualifiedFreestore& staged) -> core::Result<void> {
                    const auto owner = weak.lock();
                    if (!owner) return core::Result<void>::success();
                    const sim::InlineExecutor inline_executor;
                    const auto* executor = owner->setup->executor ? owner->setup->executor.get() : &inline_executor;
                    for (const auto& entry : owner->inbox.entries()) {
                        const auto* delivery = std::get_if<ga::NativeActions::Delivery>(&entry);
                        const auto* batch = delivery ? std::get_if<ga::NativeActions::ActorChangesDelivery>(delivery) : nullptr;
                        if (!batch) continue;
                        const auto prepared = [batch](sim::EntityId actor) -> core::Result<ga::FreestoreFacts> {
                            const auto at = std::lower_bound(batch->facts.begin(), batch->facts.end(), actor,
                                [](const auto& row, auto id) { return row.first < id; });
                            if (at == batch->facts.end() || at->first != actor) return absent<ga::FreestoreFacts>("delivered actor facts");
                            return core::Result<ga::FreestoreFacts>::success(at->second);
                        };
                        if (auto replayed = apply_galactic_ai_actor_changes(staged, batch->changes, prepared, *executor); !replayed) return replayed;
                    }
                    return core::Result<void>::success();
                });
                runtime->actions->bind_pending_bill_remaining([weak](sim::EntityId build) -> std::optional<sim::math::Fixed> {
                    const auto owner = weak.lock();
                    if (!owner) return std::nullopt;
                    std::optional<sim::math::Fixed> latest;
                    for (const auto& entry : owner->inbox.entries())
                        if (const auto* recorded = std::get_if<GalacticAiRecordedBill>(&entry);
                            recorded && recorded->build == build && recorded->effect.kind == ga::BillEffect::Kind::spend)
                            latest = recorded->effect.remaining;
                    return latest;
                });
                // W1 S6 (review #6425): a load note the AI raises inside its window (a failed battle delivery) waits for the window's
                // publication; elsewhere it goes to the world at once.
                runtime->actions->bind_load_note_sink([weak](core::LoadNote& note) {
                    const auto owner = weak.lock();
                    if (!owner || owner->outbox.schedule().apply_delay == 0 || !owner->window_open) return false;
                    owner->window_load_notes.push_back(std::move(note));
                    return true;
                });
                // W1 S3: outside the window the world bills against the billing view and logs credits; the AI gets both through
                // the inbox and applies the recorded outcome at its next window start.
                runtime->actions->bind_funding_sinks(
                    [weak](const ga::gc::GalacticSession::NativeProductionNotice& notice)
                        -> std::optional<core::Result<ga::gc::GalacticSession::NativeProductionPayment>> {
                        using Payment = ga::gc::GalacticSession::NativeProductionPayment;
                        using Kind = ga::gc::GalacticSession::NativeProductionNotice::Kind;
                        const auto owner = weak.lock();
                        // Inside the window (the AI's own work) and on the reference schedule the AI decides at once.
                        if (!owner || !owner->defers_deliveries()) return std::nullopt;
                        // A mark comes only from the AI's own production admission (admit() holds the pending goal it marks), which
                        // runs inside the window; one noticed here would be the world writing the AI budget. Counted, must stay 0.
                        if (notice.kind == Kind::marked) { ++owner->apply_tally->marks_outside_window; return std::nullopt; }
                        const auto begin = std::chrono::steady_clock::now();
                        const auto snapshot = owner->galaxy->snapshot();
                        const auto& production = snapshot->production();
                        auto decided = owner->billing_view.active() && production
                            ? owner->billing_view.bill(notice, production->players) : std::nullopt;
                        if (decided) ++owner->apply_tally->bills_viewed;
                        else {
                            // The view cannot answer (no publication holds this purchase row): decide against the AI's state
                            // without writing it. The recorded outcome reaches the AI at its next window start like every other
                            // bill. Counted, must stay 0 (the publication of the window that bought the build is visible from the
                            // apply tick that queues it, before the world can bill it).
                            ++owner->apply_tally->billing_direct;
                            decided.emplace(owner->actions->decide_live_bill(notice));
                        }
                        owner->note_billing_time(begin);
                        if (!*decided) return core::Result<Payment>::failure(decided->error());
                        if (decided->value().effect.kind != ga::BillEffect::Kind::none)
                            owner->queue(GalacticAiRecordedBill{notice.build, decided->value().effect});
                        return core::Result<Payment>::success(decided->value().payment);
                    },
                    [weak](const ga::gc::GalacticSession::CreditNotification& credit) -> std::optional<core::Result<void>> {
                        const auto owner = weak.lock();
                        if (!owner || !owner->defers_deliveries()) return std::nullopt;
                        // Before the lane's first serviced tick the view has no publication yet: the credit waits in the inbox and
                        // the first publication logs it (service phase). Never applied to the AI outside its window.
                        if (owner->billing_view.active()) {
                            const auto begin = std::chrono::steady_clock::now();
                            auto applied = owner->billing_view.credit(credit);
                            owner->note_billing_time(begin);
                            if (!applied) return applied;
                        }
                        owner->queue(credit);
                        return core::Result<void>::success();
                    });
                runtime->actions->bind_delivery_sink([weak](ga::NativeActions::Delivery& delivery) {
                    const auto owner = weak.lock();
                    if (!owner) return false;
                    if (!owner->defers_deliveries()) { ++owner->apply_tally->delivered_at_once; return false; }
                    owner->queue(std::move(delivery));
                    return true;
                });
                runtime->actions->bind_identity_ceiling([weak]() -> sim::EntityId {
                    const auto owner = weak.lock(); return owner ? owner->outbox.frontier().id : sim::EntityId{};
                });
                runtime->outbox.bind_hooks([weak](ga::gc::PlayerId, const ga::IntentRecord& record) {
                    using Shadow = ga::gc::GalacticSession::NativeActionShadow;
                    ga::IntentHooks hooks;
                    const auto owner = weak.lock();
                    if (!owner) return hooks;
                    if (const auto* force = std::get_if<Shadow::ForceRequest>(&record.request); force && force->force)
                        hooks.force_builder = [placement = owner->providers.reconciliation_placement, request = force->force->request]
                            (const auto& view, auto first) {
                            ga::ActionRequest action; action.kind = ga::ActionKind::magic;
                            action.player = request.player; action.target = request.planet; action.type = request.type;
                            return placement(action, view, first);
                        };
                    return ga::refusal::standard_hooks(*owner->actions, record, std::move(hooks));
                });
                if (auto bound = runtime->providers.bind_actions(game, galaxy, *runtime->actions, runtime->engine, runtime->world, [weak] {
                    const auto owner = weak.lock();
                    return owner ? (owner->current_executor() ? owner->current_executor() : owner->setup->executor.get()) : nullptr;
                }); !bound) return core::Result<GalacticAiContributors>::failure(bound.error());
                // WGA-385..389: use the retained full execution matrix, never
                // ordinary-world reach, TaskForce pathing or a rebuilt route.
                runtime->actions->bind_freestore_path([weak](auto player, const auto& snapshot, auto source, auto destination)
                    -> core::Result<ga::NativeActions::FreestorePathInput> {
                    const auto owner = weak.lock();
                    if (!owner) return absent<ga::NativeActions::FreestorePathInput>("retained free-move execution matrix owner");
                    const auto route = owner->routes.find(player);
                    if (route == owner->routes.end()) return absent<ga::NativeActions::FreestorePathInput>("published player free-move execution matrix");
                    return copy_galactic_ai_freestore_path(route->second, player, snapshot, source, destination);
                }, game.native_conflict_readers().global);
                if (birth == GalacticAiRuntimeBirth::fresh_campaign && runtime->providers.prepare_fresh_freestore)
                    if (auto prepared = runtime->providers.prepare_fresh_freestore(*runtime->actions, *galaxy.snapshot(), *runtime->setup->executor); !prepared)
                        return core::Result<GalacticAiContributors>::failure(prepared.error());
                if (runtime->providers.publish_freestore)
                    if (auto published = runtime->providers.publish_freestore(*runtime->actions, *runtime->pool, *galaxy.snapshot(), *runtime->setup->executor); !published)
                        return core::Result<GalacticAiContributors>::failure(published.error());
                runtime->battle = runtime->providers.prepare_battle(runtime->actions, runtime->engine, [weak] {
                    const auto owner = weak.lock(); return owner ? owner->retained_scripts : nullptr;
                });
                if (!runtime->battle) return absent<GalacticAiContributors>("retained battle listener");
                runtime->actions->bind_handlers(); runtime->host->galactic_execution = runtime->actions;
                runtime->build_tasks_ready = birth == GalacticAiRuntimeBirth::fresh_campaign;
                runtime->engine->set_galactic_funding(runtime->funding);
                runtime->engine->set_galactic_build_observer([weak](auto player, auto build) -> core::Result<ai::GalacticBuildStatus> {
                    auto owner = weak.lock(); return owner ? owner->actions->build_status(player, build) : absent<ai::GalacticBuildStatus>("producer owner");
                });
                if (runtime->providers.prepare_freestore_selection) {
                    auto selector = runtime->providers.prepare_freestore_selection(runtime->pool, runtime->world, runtime->engine,
                        [weak] { auto owner = weak.lock(); return owner && owner->galaxy ? owner->galaxy->snapshot() : nullptr; });
                    if (!selector) return core::Result<GalacticAiContributors>::failure(selector.error());
                    runtime->freestore_selection = std::move(selector).value();
                    if (!runtime->freestore_selection) return absent<GalacticAiContributors>("qualified freestore selector");
                }
                runtime->engine->set_galactic_selection([weak](auto player, auto planet, const auto& goal, const auto& plan) -> core::Result<std::shared_ptr<const ai::GalacticSelectionInput>> {
                    auto owner = weak.lock(); if (!owner) return absent<std::shared_ptr<const ai::GalacticSelectionInput>>("selector owner");
                    auto provider = ai::stock_selection_provider(owner->setup->content, owner->plans, owner->world, owner->engine,
                        [weak] { auto active = weak.lock(); return active && active->galaxy ? active->galaxy->snapshot() : nullptr; }, owner->providers.heroes, owner->freestore_selection);
                    return provider(player, planet, goal, plan);
                });
                auto lifecycle = runtime->setup->lifecycle; lifecycle.plans.clear();
                for (const auto& plan : runtime->plans->plans) lifecycle.plans.push_back({plan.name, plan.goals});
                ga::Capabilities capabilities; capabilities.queries = std::make_shared<ai::GalacticQueries>(*runtime->engine, *runtime->world);
                capabilities.actions = runtime->actions; capabilities.funding = runtime->funding;
                capabilities.services = std::make_shared<Services>(runtime);
                capabilities.immediate_effects = ai::markup_immediate_host(
                    runtime->actions->immediate_host(runtime->providers.shadow_placement, runtime->setup->immediate_requests),
                    runtime->world->markup(), runtime->world, runtime->engine);
                capabilities.immediate_instance_limit = runtime->setup->immediate_instances;
                // RGC-176..178: the scripted story's Spawn_Unit and Prevent_AI_Usage write through an ordered native shadow of the
                // world (#6378: the world-side host; no AI-owned state, the AI takes the births and AI-usage writes from its actor
                // notifications). Only a campaign whose story scripts name one of them gives its story scheduler the host (its
                // instances then run in ID order); every other campaign keeps the partitioned story service, so its hashes do not
                // move. The bound is the story instance count, not the AI's.
                if (runtime->setup->story_scripts && runtime->setup->story_scripts->uses_immediate_effects() && game.galactic_scripts()) {
                    if (auto bound = game.galactic_scripts()->story_scheduler().set_immediate_effect_host(
                            make_galactic_world_immediate_host(game.galactic_scripts()->parent_world(), runtime->world_immediate_rules(),
                                runtime->setup->immediate_requests),
                            std::max<std::size_t>(runtime->setup->story_scripts->entries().size(), 1)); !bound)
                        return core::Result<GalacticAiContributors>::failure(bound.error());
                }
                auto manager = ga::Manager::create(std::move(lifecycle), std::move(capabilities));
                if (!manager) return core::Result<GalacticAiContributors>::failure(manager.error());
                runtime->manager = manager.value();
                runtime->goals = std::make_shared<ai::GoalStateContributor>(runtime->engine, runtime->setup->content_identity, runtime->setup->content_identity_detail);
                runtime->engine->set_galactic_retirement_reader([weak](auto player) {
                    const auto owner = weak.lock(); const auto lifecycle_owner = owner ? owner->manager.lock() : nullptr;
                    return lifecycle_owner && lifecycle_owner->provider_retired(player);
                });
                const auto goals = runtime->goals;
                auto external = runtime->providers.external;
                external.emplace(std::string(AsyncLaneState::component), std::make_shared<AsyncLaneState>(weak));
                return core::Result<GalacticAiContributors>::success({manager.value(), runtime->world, goals, runtime->funding, runtime->execution, runtime->actions,
                    [weak](const auto& ledgers) -> core::Result<void> {
                        auto owner = weak.lock(); if (!owner) return absent<void>("funding identity owner");
                        for (const auto& [player, ledger] : ledgers) {
                            if (!owner->host->players.contains(player)) return absent<void>("restored funding player");
                            for (const auto& [id, goal] : ledger.goals) { static_cast<void>(goal); if (!owner->engine->galactic_goal(player, id)) return absent<void>("restored live funding goal"); }
                        }
                        return core::Result<void>::success();
                    }, std::move(external), [weak] {
                        auto owner = weak.lock(); if (!owner) return absent<void>("retained economy publisher");
                        // The production owner is later in this forward restore.
                        // Publish Funding now; live build rows wait for all owners.
                        owner->build_tasks_ready = false;
                        return owner->publish_economy();
                    }, [weak] {
                        auto owner = weak.lock(); if (!owner) return absent<void>("retained freestore publisher");
                        if (const auto manager = owner->manager.lock())
                            owner->world->set_perception_period(manager->service_periods()[static_cast<std::size_t>(ga::Service::perception)]);
                        owner->build_tasks_ready = true;
                        if (const auto manager = owner->manager.lock()) owner->galaxy->add_runtime_load_notes(manager->retirement_notes());
                        if (auto published = owner->publish_economy(); !published) return published;
                        if (owner->providers.publish_freestore)
                            if (auto published = owner->providers.publish_freestore(*owner->actions, *owner->pool, *owner->galaxy->snapshot(), *owner->setup->executor); !published)
                                return published;
                        // B-U05: a load rebuilds the retained derived connectivity once, at this barrier,
                        // from archived rows. The next admitted service then runs its own producer cadence
                        // instead of spending it on the rebuild.
                        if (!owner->providers.prepare_connectivity && !owner->providers.prepare_connectivity_publication) return core::Result<void>::success();
                        auto rebuilt = owner->prepare_connectivity(*owner->galaxy, *owner->setup->executor);
                        if (!rebuilt) return core::Result<void>::failure(rebuilt.error());
                        owner->connectivity = std::move(rebuilt).value();
                        return core::Result<void>::success();
                    }});
            }, [runtime](const auto& notice) {
                // The AI listener reads the owned facts (now, or at the next async window start); an observer reads the full notice now.
                if (auto delivered = runtime->deliver_battle(galactic_ai_battle_facts(notice)); !delivered) return delivered;
                if (runtime->providers.observe_battle) runtime->providers.observe_battle(notice);
                return core::Result<void>::success();
            }, std::move(measure));
            if (!installed) return core::Result<void>::failure(installed.error());
            runtime->native_state = installed.value();
            // W1 S4: on the async schedule the native per-tick digest composes the newest visible publication's AI-owned digests with
            // the sim-owned lane digested live; before the first publication (and on the reference schedule) everything is live.
            installed.value()->bind_published_sync([weak]() -> std::optional<ga::NativeDigests> {
                const auto owner = weak.lock();
                if (!owner || owner->outbox.schedule().apply_delay == 0) return std::nullopt;
                const auto* visible = owner->billing_view.visible();
                if (!visible) return std::nullopt;
                return ga::NativeDigests(visible->ai_digests.begin(), visible->ai_digests.end());
            }, std::string(async_lane_component));
            if (!game.has_conflict_parent_queries(BattleLayer::space)) {
                auto child = game.add_conflict_parent_queries(BattleLayer::space, runtime->child_scripts.parent_queries);
                if (!child) return child;
            }
            const auto manager = runtime->manager.lock();
            if (!manager) return absent<void>("retained manager");
            if (auto bound = game.galactic_scripts()->set_ai_engine(std::make_shared<ScriptEngine>(runtime, manager)); !bound) return bound;
            if (runtime->providers.observe || runtime->providers.watch) {
                GalacticAiRuntimeView view{runtime->engine, runtime->world, runtime->actions, installed.value(), std::move(digest_work), manager,
                    runtime->child_scripts.parent_queries, runtime->funding,
                    [weak](auto player, auto frame, const auto& executor) -> core::Result<void> {
                        const auto owner = weak.lock();
                        return owner ? owner->prepare_path_table(player, frame, executor) : absent<void>("path-table prepass owner");
                    }, runtime->apply_tally};
                if (runtime->providers.watch) runtime->providers.watch(view);
                if (runtime->providers.observe) runtime->providers.observe(std::move(view));
            }
            return core::Result<void>::success();
        };
        return Result::success({{std::move(scripts).value(), std::move(story).value()}, std::move(install),
            {}, {}, runtime->child_scripts.parent_queries});
    } catch (const std::exception& error) { return absent<Prepared>(error.what()); }
}
core::Result<void> configure_galactic_ai_runtime(platform::CampaignServiceConfiguration& configuration,
    std::shared_ptr<const GalacticAiRuntimeSetup> setup, GalacticAiRuntimeProviderFactory make_providers) {
    // Preparation checks capability presence now, before replacing configuration.
    if (!make_providers) return absent<void>("fresh runtime provider factory");
    auto providers = make_providers(); if (!providers) return core::Result<void>::failure(providers.error());
    std::vector<std::string> plan_diagnostics;
    auto checked = prepare_galactic_ai_runtime(setup, std::move(providers).value(), GalacticAiRuntimeBirth::fresh_campaign, &plan_diagnostics);
    if (!checked) return core::Result<void>::failure(checked.error());
    // What the plan materialization skipped is reported once, from this check; later fresh prepares do not repeat it.
    for (const auto& line : plan_diagnostics) setup->load_diagnostics.push_back("galactic AI plan: " + line);
    const auto prepare = [setup = std::move(setup), make_providers](GalacticAiRuntimeBirth birth) {
        auto fresh = make_providers();
        if (!fresh) return core::Result<checkpoint::GameLoadOptions::PreparedGalacticScripts>::failure(fresh.error());
        return prepare_galactic_ai_runtime(setup, std::move(fresh).value(), birth);
    };
    const auto previous = configuration.restore.configure_session;
    // Replay installs typed economic owners with AI frozen. The fresh retained
    // NativeActions replaces these handlers after reconstruction, before restore.
    configuration.restore.configure_session = [previous, make_providers](GameSession& game) {
        if (previous) if (auto configured = previous(game); !configured) return configured;
        auto fresh = make_providers(); if (!fresh) return core::Result<void>::failure(fresh.error());
        const auto replay_placement = fresh.value().placement;
        return game.configure_native_replay_owners([replay_placement](auto& world) {
            ga::NativeActions::bind_economic_replay_owners(world, replay_placement);
            return core::Result<void>::success();
        });
    };
    configuration.restore.prepare_galactic = [prepare] { return prepare(GalacticAiRuntimeBirth::checkpoint_reconstruction); };
    configuration.configure_native_ai = [prepare](GameSession& game) {
        auto prepared = prepare(GalacticAiRuntimeBirth::fresh_campaign); if (!prepared) return core::Result<void>::failure(prepared.error());
        auto& schedulers = prepared.value().schedulers;
        if (auto scripts = game.install_galactic_scripts(std::move(schedulers.first), std::move(schedulers.second)); !scripts) return scripts;
        return prepared.value().configure_native_ai(game);
    };
    return core::Result<void>::success();
}
} // namespace eawr::campaign
