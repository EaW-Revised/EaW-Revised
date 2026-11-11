#pragma once

#include "eawr/checkpoint/game.hpp"
#include "eawr/campaign/setup_plan.hpp"
#include "eawr/campaign/ai_selection_content.hpp"
#include "eawr/campaign/galactic_story_scripts.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "../script/foc/galactic_ai_actions.hpp"
#include "../script/foc/galactic_ai_goal_state.hpp"
#include "../script/foc/galactic_ai_selection_content.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace eawr::campaign {
// Immutable selected stock closure. No live pointers or saved state belong here.
struct GalacticAiRuntimeSetup {
    script::foc::galactic_ai::LifecycleSetup lifecycle;
    script::foc::ai::GalacticPerceptionSetup perception;
    std::shared_ptr<const script::foc::ai::GalacticSelectionContent> content;
    script::foc::ai::galactic_goals::PlanCatalog plans;
    std::map<std::string, std::string> modules;
    // WST-53: the sub-plot scripts of this campaign's plots (null when the plots name none); part of the content identity.
    std::shared_ptr<const GalacticStoryScripts> story_scripts;
    GalacticStoryPlayers story_players;
    // RGC-196: the game mode is a multiplayer one; the story scripts' Free_Random then uses the synchronised stream.
    bool multiplayer{};
    std::string content_identity;
    // The digests (first 8 hex) of the groups content_identity is made of, for naming a mismatch; not part of any identity.
    std::string content_identity_detail;
    // CGS-07/AI-16: what the load went on without. Mutable: the install step appends what materialization skips.
    mutable std::vector<std::string> load_diagnostics;
    std::shared_ptr<const sim::PartitionExecutor> executor;
    std::size_t immediate_instances{}, immediate_requests{};
    std::map<sim::galactic::PlanetId, script::foc::ai::Real> route_z_adjust;
};
using GalacticAiUsability = std::function<core::Result<bool>(sim::galactic::PlayerId,
    const sim::galactic::GalacticSnapshot&, sim::galactic::PlanetId)>;
using GalacticAiActorFactsReader = std::function<core::Result<script::foc::galactic_ai::FreestoreFacts>(
    sim::EntityId)>;
using GalacticAiSnapshotActorFactsReader = std::function<core::Result<script::foc::galactic_ai::FreestoreFacts>(
    const sim::galactic::GalacticSnapshot&, sim::EntityId)>;
[[nodiscard]] core::Result<script::foc::galactic_ai::FreestoreFacts> read_stock_galactic_ai_actor_facts(
    const script::foc::ai::GalacticSelectionContent&, const FreestoreMemberQualifier&,
    const sim::galactic::GalacticSnapshot&, sim::EntityId,
    const sim::galactic::GalacticSession::NativeActionShadow* = nullptr);
[[nodiscard]] core::Result<script::foc::galactic_ai::NativeActions::FreestorePathInput> copy_galactic_ai_freestore_path(
    const script::foc::galactic_ai::ExecutionRoutes&, sim::galactic::PlayerId,
    const sim::galactic::GalacticSnapshot&, sim::galactic::PlanetId source, sim::galactic::PlanetId destination);
[[nodiscard]] core::Result<script::foc::galactic_ai::NativeActions::FreestoreMoveInput> read_stock_galactic_ai_freestore_move(
    const GalacticAiUsability&, sim::galactic::PlayerId, const script::foc::galactic_ai::FreestoreFacts&,
    const sim::galactic::GalacticSnapshot&);
void bind_galactic_ai_actor_changes(script::foc::galactic_ai::NativeActions&,
    GalacticAiSnapshotActorFactsReader, std::function<const sim::PartitionExecutor*()>);
// Prepare only the actual event frontier in partitioned work, then commit in
// writer order. The reader closes over its immutable completed snapshot or
// private creation view. Generated sources precede their children in that order.
[[nodiscard]] core::Result<void> initialize_freestore_with_world_usage(
    script::foc::galactic_ai::QualifiedFreestore&, sim::galactic::PlayerId,
    std::span<const sim::EntityId> offers, std::span<const sim::galactic::CensusObject> actors);
[[nodiscard]] core::Result<void> apply_galactic_ai_actor_changes(
    script::foc::galactic_ai::QualifiedFreestore&,
    std::span<const sim::galactic::GalacticSession::NativeActorChange>,
    const GalacticAiActorFactsReader&, const sim::PartitionExecutor&);
[[nodiscard]] core::Result<script::foc::galactic_ai::ExecutionRouteInput> copy_stock_execution_input(
    const GalacticAiRuntimeSetup&, sim::galactic::PlayerId, const sim::galactic::GalacticSnapshot&,
    const script::foc::ai::GalacticWorld&, const script::foc::ai::Engine&, const GalacticAiUsability&,
    const script::foc::ai::DerivedExecutionRestore* = nullptr, bool read_only = false);
// Production callers select from campaign authoring and actual controller state.
// The selected adjustment record is resolved through the authored player mappings.
[[nodiscard]] core::Result<std::vector<script::foc::galactic_ai::PlayerSelection>> select_live_galactic_ai_players(
    const data::galactic::GalacticData&, const CampaignSetupPlan&, const script::foc::ai::AiData&,
    std::string_view authored_difficulty);
// Co-op on single-player rules (D5): with exactly one human seat, every peer agrees on that seat's faction as "local".
// Multiplayer rules, or any other human count, give no shared local player.
[[nodiscard]] std::optional<sim::galactic::PlayerId> shared_story_local_player(const CampaignSetupPlan&);
[[nodiscard]] core::Result<std::shared_ptr<const GalacticAiRuntimeSetup>> prepare_stock_galactic_ai_runtime(
    const vfs::Vfs&, const data::Catalog&, const data::galactic::GalacticData&, const units::GalacticTypeTable&,
    const CampaignSetupPlan&, std::vector<script::foc::galactic_ai::PlayerSelection>,
    std::shared_ptr<const sim::PartitionExecutor>, bool uses_fog,
    std::size_t immediate_instances, std::size_t immediate_requests,
    std::string_view authored_difficulty = {},
    script::foc::galactic_ai::ServicePeriods service_periods = script::foc::galactic_ai::retail_service_periods,
    // Production runs the one standard schedule. Tests that assert the per-tick timeline of a decision pass the
    // synchronous reference (K = 1, D = 0); a test of the async behaviour itself uses the default.
    script::foc::galactic_ai::AsyncSchedule schedule = script::foc::galactic_ai::standard_async_schedule);
// Qualified owning-lane bindings. Missing functions refuse production startup.
// Execution writers use replace_retained/mark_state_changed on the passed owner.
struct GalacticAiDigestWork {
    std::uint64_t requests{}, nanoseconds{}, maximum_nanoseconds{}, last_nanoseconds{};
    bool last_changed{};
    bool last_encoded{};
    bool includes_internal_oracle{};
    std::array<std::uint64_t, 6> owner_encodes{};
    std::map<std::string, std::uint64_t, std::less<>> external_encodes;
    std::uint64_t native_bytes{};
};
// Async apply-time outcomes, observer only (never saved, hashed or read by a decision): the pending records the window overlay could
// not replay, the intents refused as stale and, by intent kind (IntentKind order), the intents the world refused at their apply tick.
struct GalacticAiApplyTally {
    std::uint64_t overlay_refused{}, stale{};
    std::array<std::uint64_t, 10> refused{};
    // Sim->AI deliveries: applied at once (reference schedule, or the AI's own window work), queued for the next window start, and
    // drained. Of the drained ones: applied later than the first window start after their arrival, or out of arrival order.
    // Entries restored from a save carry no arrival tag and are drained without being judged.
    std::uint64_t delivered_at_once{}, queued{}, drained{}, late{}, out_of_order{};
    // W1 S3 billing view: bills decided against it, bills it could not answer (decided by the AI at once instead), drains where the
    // AI's ledgers or purchase rows differ from the newest publication with the log replayed (must stay 0), and the time the world
    // side spent on the view (bills, credits, publications, captures; observation only).
    std::uint64_t bills_viewed{}, billing_direct{}, billing_view_mismatch{}, billing_nanoseconds{};
    // Production marks noticed outside the AI window (must stay 0: marks come only from the AI's own admission, which runs in the
    // window or, on the reference schedule, at once).
    std::uint64_t marks_outside_window{};
};
struct GalacticAiRuntimeView {
    std::weak_ptr<const script::foc::ai::Engine> engine;
    std::weak_ptr<const script::foc::ai::GalacticWorld> world;
    std::weak_ptr<const script::foc::galactic_ai::NativeActions> actions;
    std::weak_ptr<const ComposedGalacticAiState> state;
    std::shared_ptr<const GalacticAiDigestWork> digest_work;
    std::weak_ptr<const script::foc::galactic_ai::Manager> manager;
    GalacticParentQueryFactory child_parent_queries;
    std::weak_ptr<const script::foc::ai::GalacticFunding> funding;
    std::function<core::Result<void>(sim::galactic::PlayerId, std::uint64_t, const sim::PartitionExecutor&)> prepare_path_table{};
    std::shared_ptr<const GalacticAiApplyTally> apply_tally{};
};
struct GalacticAiRuntimeProviders {
    using Actions = script::foc::galactic_ai::NativeActions;
    using Execution = script::foc::galactic_ai::LiveExecutionStateContributor;
    using Engine = script::foc::ai::Engine;
    using World = script::foc::ai::GalacticWorld;
    std::function<core::Result<script::foc::galactic_ai::ExecutionRouteInput>(
        sim::galactic::PlayerId, const sim::galactic::GalacticSnapshot&, const World&, const Engine&, const script::foc::ai::DerivedExecutionRestore*)> execution_input;
    // The same input read without writing the perception cache or the visibility rows (S9b-prep path-table prepass).
    // Unset: no prepass; every table builds in the player's chain.
    std::function<core::Result<script::foc::galactic_ai::ExecutionRouteInput>(
        sim::galactic::PlayerId, const sim::galactic::GalacticSnapshot&, const World&, const Engine&)> execution_input_read_only;
    std::function<core::Result<script::foc::ai::GalacticConnectivity>(
        const sim::galactic::GalacticSession&, World&, Engine&, const sim::PartitionExecutor&)> prepare_connectivity;
    using ConnectivityPublication = std::shared_ptr<const script::foc::ai::GalacticConnectivity>;
    std::function<core::Result<ConnectivityPublication>(
        const sim::galactic::GalacticSession&, World&, Engine&, const sim::PartitionExecutor&)> prepare_connectivity_publication{};
    std::function<core::Result<void>(GameSession&, sim::galactic::GalacticSession&, Actions&, std::shared_ptr<Engine>,
        std::shared_ptr<World>, std::function<const sim::PartitionExecutor*()>)> bind_actions;
    struct ChildScripts {
        GalacticParentQueryFactory parent_queries;
    };
    // Entry/restore prepares each fixed child reader before equations run. The
    // native installer completes pending bindings after the game owner exists.
    using ChildTimelineBinder = std::function<std::shared_ptr<GameSession::ChildTimelineValueReader>(ModeId)>;
    std::function<core::Result<ChildScripts>(std::shared_ptr<Engine>, std::shared_ptr<World>, ChildTimelineBinder)> prepare_child_scripts;
    // The native AI battle listener reads the owned facts (an async window may apply them later, A2.1); an observer of the full
    // notice runs when the notice is published and never reads AI state.
    using BattleListener = std::function<core::Result<void>(const GalacticAiBattleFacts&)>;
    std::function<BattleListener(std::shared_ptr<Actions>, std::shared_ptr<Engine>,
        std::function<script::authoritative::ScriptScheduler*()>)> prepare_battle;
    std::function<void(const GalacticAiBattleNotice&)> observe_battle;
    Actions::Placement placement;
    Actions::ShadowPlacement shadow_placement;
    Actions::ReconciliationPlacement reconciliation_placement;
    script::foc::ai::GalacticHeroSelectionProvider heroes;
    using SnapshotSupplier = std::function<std::shared_ptr<const sim::galactic::GalacticSnapshot>()>;
    using FreestoreSelectorFactory = std::function<core::Result<script::foc::ai::GalacticFreestoreSelectionProvider>(
        std::shared_ptr<const script::foc::galactic_ai::FreestoreInput>, std::shared_ptr<const World>,
        std::shared_ptr<const Engine>, SnapshotSupplier)>;
    FreestoreSelectorFactory prepare_freestore_selection;
    // Birth-only raw fact preparation. Reconstruction obtains actor facts from
    // the decoded production owner and must never invoke this callback.
    std::function<core::Result<void>(Actions&, const sim::galactic::GalacticSnapshot&,
        const sim::PartitionExecutor&)> prepare_fresh_freestore;
    // WGA-303: after the actual manager attaches its fresh script, offer the
    // current ordered initial frontier once. Archived initialized managers skip it.
    std::function<core::Result<void>(sim::galactic::PlayerId, Actions&,
        const sim::galactic::GalacticSnapshot&, const sim::PartitionExecutor&)> initialize_freestore;
    // Pure copied publication of the retained production owner's pool, including
    // after its native restore. The producer owns membership and raw actor facts.
    std::function<core::Result<void>(const Actions&, script::foc::galactic_ai::FreestoreInput&,
        const sim::galactic::GalacticSnapshot&, const sim::PartitionExecutor&)> publish_freestore;
    using BuildBudgetPublication = std::shared_ptr<const std::vector<script::foc::ai::GalacticBudgetBuild>>;
    using BuildBudgetPublisher = std::function<core::Result<BuildBudgetPublication>(
        const Engine&, const Actions&, const script::foc::ai::GalacticFunding&)>;
    BuildBudgetPublisher build_budgets;
    std::vector<script::foc::galactic_ai::FreestoreType> freestore_types;
    decltype(GalacticAiContributors::external) external;
    // Opt in only when every external execution writer uses the owned mutators.
    bool execution_writers_tracked{};
    // Observation holds weak owners and process-local timing only.
    std::function<void(GalacticAiRuntimeView)> observe;
    // The same view for a read-only watcher (sim_headless --live-goals). Unlike `observe` it does not turn on the digest measurement
    // wrapper, which re-captures and re-hashes the whole native state on every digest request.
    std::function<void(GalacticAiRuntimeView)> watch;
};
enum class GalacticAiRuntimeBirth { fresh_campaign, checkpoint_reconstruction };
// Fresh schedulers carry the exact host used by their post-reconstruction native
// installer. Calling this twice produces disjoint worlds, engines and listeners.
[[nodiscard]] core::Result<checkpoint::GameLoadOptions::PreparedGalacticScripts>
prepare_galactic_ai_runtime(std::shared_ptr<const GalacticAiRuntimeSetup>, GalacticAiRuntimeProviders,
    GalacticAiRuntimeBirth = GalacticAiRuntimeBirth::fresh_campaign, std::vector<std::string>* plan_diagnostics = nullptr);
using GalacticAiRuntimeProviderFactory = std::function<core::Result<GalacticAiRuntimeProviders>()>;
struct StartingForceInputs;
class StockSpaceEntry;
// Each call creates the one qualified B owner used by usability, connectivity,
// movement and the corresponding external native participant.
struct GalacticAiStockQueries {
    GalacticAiUsability usability;
    decltype(GalacticAiRuntimeProviders::prepare_connectivity) connectivity;
    decltype(GalacticAiRuntimeProviders::prepare_connectivity_publication) connectivity_publication{};
    std::function<core::Result<GalacticAiRuntimeProviders::Actions::MovementInput>(sim::galactic::PlayerId,
        const sim::galactic::GalacticSnapshot&, const GalacticAiRuntimeProviders::World&,
        const GalacticAiRuntimeProviders::Engine&, const sim::PartitionExecutor&)> movement;
    decltype(GalacticAiRuntimeProviders::external) external;
    GalacticAiRuntimeProviders::FreestoreSelectorFactory freestore_selection;
    GalacticAiRuntimeProviders::BuildBudgetPublisher build_budgets;
    FreestoreMemberQualifier freestore_members;
    std::function<core::Result<sim::galactic::TypeId>(const sim::galactic::GalacticSnapshot&,
        sim::EntityId)> freestore_current_type;
    // Sparse structural facts only. The retained adapter preserves actor flags
    // or applies the actual constructor/source mutation from its event payload.
    std::function<core::Result<script::foc::galactic_ai::FreestoreFacts>(
        const sim::galactic::GalacticSnapshot&, sim::EntityId)> freestore_actor_facts;
    // Exact actor/current/original/parent/lifecycle references from reconstruction.
    // Engine potential-plan associations are checked by the retained adapter.
    // lane_floor: actors from that identity up that the world does not hold yet are decided-but-not-applied (async AI).
    std::function<core::Result<void>(const sim::galactic::GalacticSnapshot&,
        const script::foc::galactic_ai::QualifiedFreestore&, sim::EntityId lane_floor)> freestore_references;
};
using GalacticAiStockQueryFactory = std::function<core::Result<GalacticAiStockQueries>()>;
[[nodiscard]] core::Result<GalacticAiRuntimeProviderFactory> stock_galactic_ai_providers(
    std::shared_ptr<const GalacticAiRuntimeSetup>, std::shared_ptr<const StartingForceInputs>,
    std::shared_ptr<const StockSpaceEntry>, GalacticAiStockQueryFactory,
    std::function<void(GalacticAiRuntimeView)> observe = {},
    std::function<void(const GalacticAiBattleNotice&)> observe_battle = {},
    std::function<void(GalacticAiRuntimeView)> watch = {});
// Observer only: per AI player, the goals by goal function (count, active, planned, running plan, largest desire), the retained
// movement rows and the engine's refusal counters (#3944; empty unless gc_profile::enabled). Reads the live engine, so the caller
// keeps the session between ticks. Never saved, hashed or read by a decision.
[[nodiscard]] std::vector<std::string> describe_galactic_ai_goals(const GalacticAiRuntimeView&, const GalacticAiRuntimeSetup&);
[[nodiscard]] core::Result<void> configure_galactic_ai_runtime(platform::CampaignServiceConfiguration&,
    std::shared_ptr<const GalacticAiRuntimeSetup>, GalacticAiRuntimeProviderFactory);
} // namespace eawr::campaign
