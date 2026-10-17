#include "galactic_ai_runtime.hpp"
#include "eawr/core/gc_profile.hpp"
#include "../script/foc/galactic_ai.hpp"
#include "../script/foc/tactical_ai_internal.hpp"
#include "eawr/campaign/ai_selection_content.hpp"
#include "eawr/campaign/ai_perception_content.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <exception>
#include <cstring>
#include <chrono>

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
    GalacticAiRuntimeProviders::ChildScripts child_scripts;
    GameSession::ChildTimelineReaderFactory child_timeline_factory;
    std::vector<std::pair<ModeId, std::shared_ptr<GameSession::ChildTimelineValueReader>>> pending_child_timelines;
    GalacticAiRuntimeProviders::BattleListener battle;
    std::shared_ptr<foc::detail::Host> host = std::make_shared<foc::detail::Host>();
    std::shared_ptr<ai::GalacticWorld> world;
    std::shared_ptr<ai::Engine> engine;
    std::shared_ptr<ai::GalacticFunding> funding = std::make_shared<ai::GalacticFunding>();
    std::weak_ptr<ga::Manager> manager;
    std::weak_ptr<ComposedGalacticAiState> native_state;
    std::shared_ptr<ga::NativeActions> actions;
    std::unique_ptr<ga::gc::GalacticSession::NativeActionBarrier> native_actions_barrier;
    template<class T> core::Result<T> checked_native(core::Result<T> result) {
        if (!result) native_actions_barrier.reset();
        return result;
    }
    core::Result<void> begin_native_actions_barrier() {
        if (native_actions_barrier) return absent<void>("completed preceding native action barrier");
        std::vector<ga::gc::PlayerId> players;
        for (const auto& selected : setup->lifecycle.players) players.push_back(selected.player);
        std::sort(players.begin(), players.end());
        auto begun = galaxy->begin_native_barrier(players, ga::gc::galactic_max_commands);
        if (!begun) return core::Result<void>::failure(begun.error());
        native_actions_barrier = std::move(begun).value();
        return core::Result<void>::success();
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
    std::map<ga::gc::PlayerId, std::vector<ai::EvaluatedFundingTemplate>> initial;
    std::uint64_t sequence{};
    auth::ScriptScheduler* active_scripts{};
    const sim::PartitionExecutor* active_executor{};
    std::shared_ptr<const ai::GalacticConnectivity> connectivity{std::make_shared<const ai::GalacticConnectivity>()};
    std::shared_ptr<const ai::GalacticConnectivity> route_source, combined_routes;
    struct RoutePublication {
        std::uint64_t revision{};
        const ai::GalacticExecutionRoute* source{};
        std::shared_ptr<const std::vector<ai::GalacticExecutionRoute>> rows{};
    };
    std::map<ga::gc::PlayerId, RoutePublication> route_publications;
    std::mutex economy_mutex;
    std::shared_ptr<const std::vector<ai::GalacticBudgetPlayer>> budget_players;
    std::optional<std::uint64_t> budget_players_revision;
    std::weak_ptr<const ai::GalacticEconomyRuntime> last_economy;
    std::uint64_t economy_revision{};
    std::uint64_t economy_tick{};
    std::uint64_t economy_engine_revision{};
    std::optional<std::uint64_t> economy_production_revision;
    bool economy_production_present{}, economy_build_tasks_ready{}, build_tasks_ready{};
    bool build_budgets_cached{};
    std::uint64_t build_engine_revision{}, build_funding_revision{};
    std::optional<std::uint64_t> build_production_revision;
    GalacticAiRuntimeProviders::BuildBudgetPublication copied_build_budgets;

    core::Result<std::shared_ptr<const ai::GalacticEconomyRuntime>> copy_economy() {
        using Result = core::Result<std::shared_ptr<const ai::GalacticEconomyRuntime>>;
        if (!galaxy || !world->available()) return absent<std::shared_ptr<const ai::GalacticEconomyRuntime>>("completed economy world");
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
            ? providers.publish_freestore(*actions, *pool, *galaxy->snapshot(), *setup->executor) : core::Result<void>::success();
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
            ? providers.publish_freestore(*actions, *pool, *galaxy->snapshot(), *setup->executor) : core::Result<void>::success();
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
        return world->bind_publication(galaxy->snapshot(), route_publication(), executor, galaxy->completed_tick(), galaxy->next_entity_id(),
            lifecycle && lifecycle->perception_due(galaxy->completed_tick()));
    }
    core::Result<void> publish_refreshed_routes() {
        const auto published = route_publication();
        return world->publish_execution_route_generations(published->execution_available, published->revision, published->player_routes);
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
    core::Result<void> update_routes(ga::gc::PlayerId player, std::uint64_t frame, bool initialize, const sim::PartitionExecutor& executor) {
        // Route writes validate archival reservations against the live Engine.
        // A goal may have released its holds earlier in this barrier.
        if (auto synchronized = synchronize_reservations(); !synchronized) return synchronized;
        auto source = providers.execution_input(player, *galaxy->snapshot(), *world, *engine, nullptr);
        if (!source) return core::Result<void>::failure(source.error());
        auto input = std::move(source).value();
        if (input.player != player) return absent<void>("execution input player identity");
        auto state = retained;
        auto& row = state[player];
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
        const auto producer = [&input](const sim::PartitionExecutor& executor) -> core::Result<std::optional<ga::ExecutionRouteBuild>> {
            auto built = ga::build_execution_routes(input, executor);
            if (!built) return core::Result<std::optional<ga::ExecutionRouteBuild>>::failure(built.error());
            return core::Result<std::optional<ga::ExecutionRouteBuild>>::success(std::move(built).value());
        };
        auto staged = routes[player];
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
        routes[player] = std::move(staged);
        return publish_refreshed_routes();
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
        if (auto selected = select_native_player(player); !selected) return selected;
        const auto* account = world->account(player);
        if (!account || !initial.contains(player)) return absent<void>("initial template/account");
        auto result = funding->initialize(player, initial.at(player), account->credits);
        if (result) {
            initial.erase(player);
            const auto lifecycle = manager.lock();
            if (!active_scripts || !lifecycle) return absent<void>("fresh freestore host");
            if (auto initialized = update_routes(player, galaxy->clock().galactic_frame, true, *active_executor); !initialized) return initialized;
            const auto module = lifecycle->freestore(player);
            if (!module.empty()) {
                // Player identities occupy the freestore instance domain; plans
                // use the shared engine's separate first_plan_instance domain.
                if (auto created = active_scripts->create_instance(player, "Data/Scripts/FreeStore/" + std::string(module) + ".lua"); !created) return created;
                if (auto attached = ga::attach_freestore(*active_scripts, {player, player}, true); !attached) return attached;
            }
            if (providers.initialize_freestore) {
                if (auto initialized = providers.initialize_freestore(player, *actions, *galaxy->snapshot(), *active_executor); !initialized) return initialized;
                if (providers.publish_freestore)
                    if (auto published = providers.publish_freestore(*actions, *pool, *galaxy->snapshot(), *active_executor); !published) return published;
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
        if (auto published = publish_freestore(); !published) return published;
        if (input.service == ga::Service::execution) {
            if (!active_executor) return absent<void>("admitted execution worker adapter");
            if (auto refreshed = prepare_path_table(input.player, input.clocks.mode, *active_executor); !refreshed) return refreshed;
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
                        if (auto published = providers.publish_freestore(*actions, *pool, *galaxy->snapshot(), *active_executor); !published) return published;
                    if (auto landed = actions->service_freestore_landing(input.player, *pool->pool, *active_executor); !landed) return landed;
                    if (providers.publish_freestore)
                        if (auto published = providers.publish_freestore(*actions, *pool, *galaxy->snapshot(), *active_executor); !published) return published;
                }
                auto serviced = ga::service_freestore(scripts, {input.player, input.player}, *pool->pool, input.clocks.mode,
                    pool->pool->qualified ? &actions->qualified_freestore() : nullptr);
                if (!serviced) return serviced;
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
            auto committed = actions->commit_freestore_move(command);
            if (!committed) return core::Result<void>::failure(committed.error());
        }
        report.commands = std::move(kept);
        if (auto synchronized = synchronize_reservations(); !synchronized) return synchronized;
        if (native_actions_barrier) {
            auto committed = native_actions_barrier->commit(); native_actions_barrier.reset();
            if (!committed) return committed;
        }
        return core::Result<void>::success();
    }
    core::Result<void> prepare(const sim::PartitionExecutor& executor, const ga::gc::GalacticSession& parent, const ga::gc::GalacticTick& tick) {
        const auto reads = world->owner_reads();
        gc_profile::Scope profile_scope(gc_profile::Phase::runtime_prepare);
        {
            gc_profile::Scope freestore_scope(gc_profile::Phase::prepare_freestore);
            if (providers.publish_freestore) {
                if (auto published = providers.publish_freestore(*actions, *pool, *parent.snapshot(), executor); !published) return published;
            } else {
                auto copied = ga::copy_freestore(*parent.snapshot(), providers.freestore_types, executor);
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
class ScriptEngine final : public auth::GalacticScriptEngine {
public:
    ScriptEngine(std::shared_ptr<Runtime> owner, std::shared_ptr<ga::Manager> manager)
        : owner_(std::move(owner)), manager_(std::move(manager)) {}
    bool uses_sync_hash() const noexcept override { return true; }
    bool verifies_sync_hash() const noexcept override {
        const auto state = owner_->native_state.lock();
        return state && state->full_verification_enabled();
    }
    bool admits_service(const ga::gc::GalacticTick& tick) const noexcept override { return manager_->admits_service(tick); }
    core::Result<auth::ServiceOptions> before_service(const sim::PartitionExecutor& executor,
        const ga::gc::GalacticSession& parent, const ga::gc::GalacticTick& tick, auth::ScriptScheduler& scripts) override {
        const auto reads = owner_->world->owner_reads();
        const auto native_record_write = owner_->engine->native_record_scope();
        auto cleanup = std::make_shared<NativeBarrierCleanup>(owner_);
        owner_->active_executor = &executor;
        owner_->actions->set_executor(&executor);
        if (auto prepared = owner_->prepare(executor, parent, tick); !prepared) return core::Result<auth::ServiceOptions>::failure(prepared.error());
        const ga::Clocks clocks{tick.completed_tick, parent.clock().galactic_frame};
        if (auto begun = owner_->checked_native(owner_->begin_native_actions_barrier()); !begun)
            return core::Result<auth::ServiceOptions>::failure(begun.error());
        cleanup->barrier = owner_->native_actions_barrier.get();
        owner_->sequence = 0; // EventKey sequence is local to this synchronization barrier.
        core::Result<void> begun = [&] {
            gc_profile::Scope begin_scope(gc_profile::Phase::barrier_begin);
            return owner_->engine->begin_galactic_barrier(*owner_->world, clocks, scripts, owner_->sequence,
                owner_->setup->lifecycle.players, &executor);
        }();
        if (!begun) {
            owner_->native_actions_barrier.reset();
            return core::Result<auth::ServiceOptions>::failure(begun.error());
        }
        owner_->engine->set_galactic_funding(owner_->funding);
        owner_->engine->defer_galactic_effects(true); // S9a: the players' engine effects commit once, at finish_barrier
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
        owner_->active_executor = nullptr; owner_->actions->set_executor(owner_->setup->executor.get()); return result;
    }
private:
    struct NativeBarrierCleanup {
        std::shared_ptr<Runtime> owner;
        const ga::gc::GalacticSession::NativeActionBarrier* barrier{};
        explicit NativeBarrierCleanup(std::shared_ptr<Runtime> runtime) : owner(std::move(runtime)) {}
        ~NativeBarrierCleanup() {
            if (owner->native_actions_barrier && owner->native_actions_barrier.get() != barrier) return;
            owner->native_actions_barrier.reset();
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
    const ai::Engine& engine, const GalacticAiUsability& usable, const ai::DerivedExecutionRestore* reconstruction) {
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

core::Result<std::vector<ga::PlayerSelection>> select_live_galactic_ai_players(
    const data::galactic::GalacticData& data, const CampaignSetupPlan& plan, const ai::AiData& loaded,
    std::string_view authored_difficulty) {
    std::vector<ga::PlayerSelection> selected;
    // WGA-02 / WGS-08: select the campaign's authored player type only
    // for an active AI controller. Difficulty names remain mod-authored.
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
        const auto assignment = std::find_if(campaign->ai_player_control.begin(), campaign->ai_player_control.end(),
            [&](const auto& row) { return ai::upper_case(row.faction) == ai::upper_case(binding.faction); });
        if (assignment == campaign->ai_player_control.end()) continue;
        const auto type = loaded.players.find(ai::upper_case(assignment->name));
        if (type == loaded.players.end()) return absent<std::vector<ga::PlayerSelection>>("authored AI player type");
        bool mapped = false;
        for (const auto& [candidate, adjustment] : type->second.difficulty) if (adjustment == authored_adjustment) {
            if (!level.empty() && level != candidate)
                return absent<std::vector<ga::PlayerSelection>>("unambiguous authored difficulty level");
            level = candidate;
            mapped = true;
        }
        if (!mapped) return absent<std::vector<ga::PlayerSelection>>("authored difficulty level");
        ga::PlayerSelection player{id, assignment->name, level, {}, human_player};
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
    return core::Result<std::vector<ga::PlayerSelection>>::success(std::move(selected));
}

core::Result<std::shared_ptr<const GalacticAiRuntimeSetup>> prepare_stock_galactic_ai_runtime(
    const vfs::Vfs& files, const data::Catalog& catalog, const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& types, const CampaignSetupPlan& plan, std::vector<ga::PlayerSelection> selected,
    std::shared_ptr<const sim::PartitionExecutor> executor, bool uses_fog, std::size_t instances, std::size_t requests,
    std::string_view authored_difficulty, ga::ServicePeriods service_periods) {
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
        const auto loaded = require(ga::load_data(out->lifecycle));
        if (!authored_difficulty.empty()) {
            if (!out->lifecycle.players.empty()) return absent<std::shared_ptr<const GalacticAiRuntimeSetup>>("one player selection source");
            out->lifecycle.players = require(select_live_galactic_ai_players(data, plan, loaded, authored_difficulty));
        }
        for (const auto& asset : require(files.enumerate("data/scripts", ".lua"))) {
            const auto& path = asset.canonical_path;
            if (path.starts_with("data/scripts/library/") || path.starts_with("data/scripts/freestore/")
                || (path.starts_with("data/scripts/ai/") && path.substr(16).find('/') == std::string::npos)) out->modules.emplace(path, read(path));
        }
        out->plans = require(ai::galactic_goals::load_plan_catalog({}, out->modules));
        auto& perception = out->perception;
        if (out->lifecycle.players.empty()) return absent<std::shared_ptr<const GalacticAiRuntimeSetup>>("selected stock players");
        perception = *require(prepare_ai_perception_content(files, data, plan, *out->content, out->lifecycle.players.front().difficulty));
        if (plan.galaxy().seed != perception.seed) return absent<std::shared_ptr<const GalacticAiRuntimeSetup>>("qualified 32-bit AI seed");
        perception.uses_fog = uses_fog;
        for (auto& player : perception.players) {
            const auto chosen = std::find_if(out->lifecycle.players.begin(), out->lifecycle.players.end(),
                [&](const auto& row) { return row.player == player.player; });
            player.ai = chosen != out->lifecycle.players.end();
            if (player.ai) player.difficulty = chosen->difficulty;
        }
        for (const auto& location : plan.locations()) if (location.planet) {
            const auto original = require(catalog.resolve(location.original_type, data::Category::game_object));
            if (const auto* value = original.value("Layer_Z_Adjust")) {
                const auto z = ai::parse_real(value->value.raw_text);
                if (!z) return absent<std::shared_ptr<const GalacticAiRuntimeSetup>>("source planet Layer_Z_Adjust number");
                out->route_z_adjust.emplace(location.planet->id, *z);
            } else out->route_z_adjust.emplace(location.planet->id, ai::Real{}); // WGA-257: shared original-type constructor default
        }
        std::string fingerprint = "eawr-galactic-runtime-content-v3";
        const auto field = [&](std::string_view value) { fingerprint += std::to_string(value.size()) + ":"; fingerprint += value; };
        const auto node = [&](auto&& self, const data::XmlNode& value) -> void {
            field(value.name); field(value.raw_text); field(std::to_string(value.attributes.size()));
            for (const auto& attribute : value.attributes) { field(attribute.name); field(attribute.value); }
            field(std::to_string(value.children.size()));
            for (const auto& child : value.children) self(self, child);
        };
        for (const auto& binding : plan.type_bindings()) if (const auto* type = plan.original_type(binding.id)) {
            field(std::to_string(binding.id)); field(binding.original_type);
            const auto object = require(catalog.resolve(type->name, data::Category::game_object));
            for (const auto& value : object.values) node(node, value.value);
        }
        for (const auto& [path, bytes] : out->lifecycle.xml) { field(path); field(bytes); }
        for (const auto& [path, bytes] : out->modules) { field(path); field(bytes); }
        for (const auto& registry : catalog.registry_files()) if (registry.loaded && registry.input_sha256) { field(registry.included_path); field(*registry.input_sha256); }
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
        for (const auto& player : out->lifecycle.players) {
            field(std::to_string(player.player)); field(ai::upper_case(player.player_type)); field(ai::upper_case(player.difficulty));
            field(player.enemy ? std::to_string(*player.enemy) : "absent"); field(player.human ? std::to_string(*player.human) : "absent");
        }
        const auto digest = units::galactic_content_identity(types);
        field(core::sha256_hex(digest));
        field(std::to_string(out->content->registered_types.size()));
        for (auto id : out->content->registered_types) field(std::to_string(id));
        out->content_identity = core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(fingerprint.data()), fingerprint.size()));
        static_cast<void>(loaded);
        return Result::success(std::move(out));
    } catch (const std::exception& error) { return absent<std::shared_ptr<const GalacticAiRuntimeSetup>>(error.what()); }
}

core::Result<checkpoint::GameLoadOptions::PreparedGalacticScripts> prepare_galactic_ai_runtime(
    std::shared_ptr<const GalacticAiRuntimeSetup> setup, GalacticAiRuntimeProviders providers, GalacticAiRuntimeBirth birth) {
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
        runtime->plans = std::make_shared<const ai::galactic_goals::MaterializedPlanCatalog>(std::move(materialized).value());
        runtime->world = std::make_shared<ai::GalacticWorld>(input.perception); host.ai_world = runtime->world;
        runtime->world->enable_live_deltas(input.lifecycle.service_periods[static_cast<std::size_t>(ga::Service::perception)], input.executor);
        runtime->world->set_producer_executor_supplier([weak = std::weak_ptr<Runtime>(runtime)]() -> const sim::PartitionExecutor* {
            const auto owner = weak.lock(); return owner ? owner->active_executor : nullptr;
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
        if (!runtime->child_scripts.first || !runtime->child_scripts.second) return absent<Prepared>("paired child script services");
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
        auto story = auth::ScriptScheduler::create(config, {}); if (!story) return Result::failure(story.error());
        auto install = [runtime, birth](GameSession& game) -> core::Result<void> {
            if (!game.galactic_scripts()) return absent<void>("reconstructed galactic schedulers");
            runtime->world->bind_timeline_reader(game.active_timeline_reader());
            runtime->child_timeline_factory = game.child_timeline_reader_factory();
            for (const auto& [mode, reader] : runtime->pending_child_timelines)
                *reader = runtime->child_timeline_factory(mode);
            runtime->pending_child_timelines.clear();
            runtime->active_scripts = &game.galactic_scripts()->ai_scheduler();
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
                runtime->actions = std::make_shared<ga::NativeActions>(galaxy, runtime->funding, runtime->providers.placement, std::move(multipliers),
                    [weak](auto player, auto plan) -> std::optional<std::uint64_t> { auto owner = weak.lock(); return owner ? owner->engine->galactic_goal_for_plan(player, plan) : std::nullopt; },
                    runtime->setup->executor.get(), [content = runtime->setup->content](auto type) -> core::Result<sim::math::Fixed> {
                        const auto found = content->types.find(type); if (found == content->types.end()) return absent<sim::math::Fixed>("original build cost");
                        return script::numeric::to_fixed(ai::real(ai::truncate(found->second.value.cost)));
                    });
                if (auto bound = runtime->providers.bind_actions(game, galaxy, *runtime->actions, runtime->engine, runtime->world, [weak] {
                    const auto owner = weak.lock();
                    return owner ? (owner->active_executor ? owner->active_executor : owner->setup->executor.get()) : nullptr;
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
                    const auto owner = weak.lock(); return owner ? owner->active_scripts : nullptr;
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
                auto manager = ga::Manager::create(std::move(lifecycle), std::move(capabilities));
                if (!manager) return core::Result<GalacticAiContributors>::failure(manager.error());
                runtime->manager = manager.value();
                auto goals = std::make_shared<ai::GoalStateContributor>(runtime->engine, runtime->setup->content_identity);
                return core::Result<GalacticAiContributors>::success({manager.value(), runtime->world, goals, runtime->funding, runtime->execution, runtime->actions,
                    [weak](const auto& ledgers) -> core::Result<void> {
                        auto owner = weak.lock(); if (!owner) return absent<void>("funding identity owner");
                        for (const auto& [player, ledger] : ledgers) {
                            if (!owner->host->players.contains(player)) return absent<void>("restored funding player");
                            for (const auto& [id, goal] : ledger.goals) { static_cast<void>(goal); if (!owner->engine->galactic_goal(player, id)) return absent<void>("restored live funding goal"); }
                        }
                        return core::Result<void>::success();
                    }, runtime->providers.external, [weak] {
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
            }, [runtime](const auto& notice) { return runtime->battle(notice); }, std::move(measure));
            if (!installed) return core::Result<void>::failure(installed.error());
            runtime->native_state = installed.value();
            if (!game.battle()) {
                auto child = game.set_child_script_services(runtime->child_scripts.first, runtime->child_scripts.second);
                if (!child) return child;
            }
            const auto manager = runtime->manager.lock();
            if (!manager) return absent<void>("retained manager");
            if (auto bound = game.galactic_scripts()->set_ai_engine(std::make_shared<ScriptEngine>(runtime, manager)); !bound) return bound;
            if (runtime->providers.observe) runtime->providers.observe({runtime->engine, runtime->world, runtime->actions, installed.value(), std::move(digest_work), manager,
                runtime->child_scripts.parent_queries, runtime->funding,
                [weak](auto player, auto frame, const auto& executor) -> core::Result<void> {
                    const auto owner = weak.lock();
                    return owner ? owner->prepare_path_table(player, frame, executor) : absent<void>("path-table prepass owner");
                }});
            return core::Result<void>::success();
        };
        return Result::success({{std::move(scripts).value(), std::move(story).value()}, std::move(install),
            runtime->child_scripts.first, runtime->child_scripts.second});
    } catch (const std::exception& error) { return absent<Prepared>(error.what()); }
}
core::Result<void> configure_galactic_ai_runtime(platform::CampaignServiceConfiguration& configuration,
    std::shared_ptr<const GalacticAiRuntimeSetup> setup, GalacticAiRuntimeProviderFactory make_providers) {
    // Preparation checks capability presence now, before replacing configuration.
    if (!make_providers) return absent<void>("fresh runtime provider factory");
    auto providers = make_providers(); if (!providers) return core::Result<void>::failure(providers.error());
    auto checked = prepare_galactic_ai_runtime(setup, std::move(providers).value());
    if (!checked) return core::Result<void>::failure(checked.error());
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
