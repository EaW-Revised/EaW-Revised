#include <array>
#include <cstdint>
#include <exception>
#include <ios>
#include <map>
#include <memory>
#include <span>
#include <system_error>
#include <vector>
#include <cstddef>
#include <string_view>
#include <string>
#include <optional>
#include "../support/env.hpp"
#include "../support/check.hpp"
#include "../support/result.hpp"
// WGA-58..60/66/246..253: installed Normal plan selection and paid Lua consumer.
#include "galactic_ai_selection_content.hpp"
#include "galactic_ai.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_goal_state.hpp"
#include "../../src/campaign/galactic_ai_runtime.hpp"
#include "../../src/campaign/galactic_ai_usability.hpp"
#include "galactic_ai_freestore.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/campaign/ai_selection_content.hpp"
#include "eawr/campaign/ai_perception_content.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>
using eawr::testing::must;

namespace ai = eawr::script::foc::ai;
namespace foc = eawr::script::foc;
namespace ga = foc::galactic_ai;
namespace gc = eawr::sim::galactic;
namespace cp = eawr::campaign;
namespace auth = eawr::script::authoritative;
namespace {

void expect(bool value, std::string_view message) {
    if (!value) eawr::testing::require(value, message);
}
struct DiskCheckpoint {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("eawr-selector-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".checkpoint");
    ~DiskCheckpoint() { std::error_code error; std::filesystem::remove(path, error); }
};
std::string read(const eawr::vfs::Vfs& fs, std::string_view path) {
    const auto bytes = must(fs.open(path)); std::string value(bytes.size(), '\0');
    if (!value.empty()) std::memcpy(value.data(), bytes.data(), value.size());
    return value;
}
std::string game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("installed selector requires game root");
    return std::move(*root);
}
struct Owner {
    std::shared_ptr<foc::detail::Host> host = std::make_shared<foc::detail::Host>();
    std::shared_ptr<ai::GalacticWorld> world;
    std::shared_ptr<ai::Engine> engine;
    std::shared_ptr<ai::GalacticFunding> funding = std::make_shared<ai::GalacticFunding>();
    std::shared_ptr<ga::NativeActions> actions;
    auth::ScriptScheduler scripts;
    std::uint64_t sequence{};
    gc::PlayerId empire{};
    std::array<ga::PlayerSelection, 1> selected;
    std::shared_ptr<const ai::GalacticSelectionContent> selection_content;
    std::shared_ptr<const ai::galactic_goals::MaterializedPlanCatalog> selection_plans;
    std::shared_ptr<const cp::GalacticAiRuntimeSetup> runtime_setup;
    std::shared_ptr<cp::GalacticAiUsabilityProvider> usability;
    std::shared_ptr<ga::FreestoreInput> freestore;
    Owner(gc::GalacticSession& galaxy, const eawr::sim::PartitionExecutor& executor,
        const ai::GalacticPerceptionSetup& setup, ai::AiData data,
        std::shared_ptr<const ai::GalacticSelectionContent> content,
        const ai::galactic_goals::PlanCatalog& catalog, const std::map<std::string, std::string>& modules, bool activate_budget = true)
        : scripts(must(auth::ScriptScheduler::create({}, {}))) {
        host->setup.seed = setup.seed; host->setup.content = content->ai;
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type); host->types_by_name.emplace(ai::upper_case(type.name), &type);
        }
        for (const auto& row : setup.players) {
            if (row.faction == "Empire") empire = row.player;
            // This fixture owns only its selected Empire controller. World
            // perception retains every real player's AI/human facts, while
            // controller declarations match begin_galactic_barrier selection.
            const bool controlled = row.ai && row.faction == "Empire";
            host->setup.players.push_back({row.player, row.faction, row.neutral, controlled, controlled ? "BASICEMPIRE" : "", row.human});
        }
        for (const auto& row : host->setup.players) host->players.emplace(row.player, &row);
        const auto plans = std::make_shared<const ai::galactic_goals::MaterializedPlanCatalog>(must(ai::galactic_goals::materialize_plan_catalog(*host, catalog)));
        selection_content = content; selection_plans = plans;
        expect(plans->plans.size() == catalog.plans.size(), "full original plan catalog materializes");
        world = std::make_shared<ai::GalacticWorld>(setup); host->ai_world = world;
        must(world->bind_economy_runtime(ai::fresh_economy_runtime(setup.players))); // WGA-294/295 actual fresh constructor boundary
        engine = std::make_shared<ai::Engine>(host, data, plans->plans); host->engine = engine.get();
        engine->set_galactic_stage_catalog(plans);
        must(foc::tactical_ai_detail::load_contrast(*host, modules));
        auth::ModuleManifest manifest;
        for (const auto& [path, text] : modules) must(manifest.add(path, text));
        auth::SessionConfig config; config.script_directories = {"Data/Scripts/Library/"}; config.galactic_clock = true;
        config.tick_duration = {1, gc::galactic_frames_per_second};
        scripts = must(auth::ScriptScheduler::create(config, std::move(manifest)));
        std::vector<eawr::core::Diagnostic> diagnostics;
        foc::tactical_ai_detail::register_globals(scripts, host, diagnostics);
        foc::tactical_ai_detail::register_methods(scripts, host, diagnostics);
        expect(diagnostics.empty(), "stock bindings install without diagnostics");
        selected = {{{empire, "BASICEMPIRE", "NORMAL", {}, {}}}};
        bind(galaxy, executor);
        std::vector<ga::PlanInput> registrations;
        for (const auto& plan : plans->plans) registrations.push_back({plan.name, plan.goals});
        const auto& player_type = data.players.at("BASICEMPIRE");
        const auto policy = ga::apply_template(data, registrations, {}, data.templates.at(player_type.galactic_templates.front()));
        must(engine->apply_galactic_policy(empire, policy));
        ai::EvaluatedFundingTemplate budget;
        if (activate_budget) {
        for (const auto& [name, setting] : policy.equations) {
            const auto value = engine->evaluate(setting.value, empire, nullptr);
            if (!value) throw std::runtime_error("stock budget equation unavailable: " + setting.value);
            budget.equations.push_back({data.goal_categories.at(name), must(eawr::script::numeric::to_fixed(*value))});
        }
        for (const auto& account : galaxy.snapshot()->player_economy()->players) if (account.player == empire)
                must(funding->initialize(empire, std::array{budget}, account.credits));
        }
        engine->set_galactic_funding(funding);
        engine->set_galactic_selection(ai::stock_selection_provider(content, plans, world, engine, [&galaxy] { return galaxy.snapshot(); }));
        std::map<gc::PlayerId, eawr::sim::math::Fixed> credit_multipliers;
        expect(galaxy.setup().fiscal.has_value(), "stock fiscal owner available");
        for (const auto& row : galaxy.setup().fiscal->players) credit_multipliers.emplace(row.player, row.positive_credit_multiplier);
        actions = std::make_shared<ga::NativeActions>(galaxy, funding, ga::NativeActions::Placement{},
            std::move(credit_multipliers),
            [manager = engine](auto player, auto plan) { return manager->galactic_goal_for_plan(player, plan); }, &executor,
            [content](auto type) {
                return eawr::script::numeric::to_fixed(ai::real(ai::truncate(content->types.at(type).value.cost)));
            });
        actions->bind_handlers(); host->galactic_execution = actions;
        engine->set_galactic_build_observer([owner = actions](auto player, auto build) { return owner->build_status(player, build); });
        world->bind_economy_supplier([this, &galaxy]() -> eawr::core::Result<std::shared_ptr<const ai::GalacticEconomyRuntime>> {
            auto publication = std::make_shared<ai::GalacticEconomyRuntime>();
            publication->completed_tick = galaxy.snapshot()->completed_tick(); publication->timeline = funding->state().timeline;
            const auto builds = engine->galactic_build_budgets(*actions, *funding);
            if (!builds) return eawr::core::Result<std::shared_ptr<const ai::GalacticEconomyRuntime>>::failure(builds.error());
            publication->builds = *builds.value(); publication->build_tasks_complete = true;
            for (const auto& [player, ledger] : funding->state()) {
                ai::GalacticBudgetPlayer budget; budget.player = player; budget.income = eawr::script::numeric::from_fixed(ledger.last_service_income);
                for (const auto& [category, row] : ledger.categories)
                    budget.categories.emplace(category, ai::GalacticBudgetCategory{eawr::script::numeric::from_fixed(row.available_income), eawr::script::numeric::from_fixed(row.available_cash)});
                publication->budgets.push_back(std::move(budget));
            }
            return eawr::core::Result<std::shared_ptr<const ai::GalacticEconomyRuntime>>::success(publication);
        });
    }
    void bind(gc::GalacticSession& galaxy, const eawr::sim::PartitionExecutor& executor) {
        const auto snapshot = galaxy.snapshot();
        must(scripts.set_galactic_frame(snapshot->clock().galactic_frame));
        must(world->bind(snapshot, {}, executor, snapshot->completed_tick(), galaxy.next_entity_id()));
        must(engine->begin_galactic_barrier(*world, {snapshot->completed_tick(), snapshot->clock().galactic_frame}, scripts, sequence, selected, &executor));
        if (freestore) {
            auto connectivity = must(usability->connectivity(galaxy, *world, *engine, executor));
            const cp::GalacticAiUsability usable = [provider = usability](auto player, const auto& value, auto planet) {
                return provider->usability(player, value, planet);
            };
            auto input = must(cp::copy_stock_execution_input(*runtime_setup, empire, *snapshot, *world, *engine, usable));
            auto routes = must(ga::build_execution_routes(input, executor));
            connectivity.execution_available = true;
            connectivity.execution_routes = std::move(routes.rows);
            must(world->bind(snapshot, std::move(connectivity), executor, snapshot->completed_tick(), galaxy.next_entity_id()));
            freestore->pool = must(actions->qualified_freestore().publish(executor));
        }
    }
    void qualify(gc::GalacticSession& galaxy, const eawr::sim::PartitionExecutor& executor,
        std::shared_ptr<const cp::GalacticAiRuntimeSetup> runtime,
        std::shared_ptr<const cp::GalacticAiUsabilityContent> usability_content) {
        runtime_setup = std::move(runtime);
        usability = std::make_shared<cp::GalacticAiUsabilityProvider>(std::move(usability_content));
        const auto snapshot = galaxy.snapshot();
        const auto members = cp::freestore_member_qualifier(selection_content);
        std::vector<gc::GalacticSession::NativeActorChange> births;
        for (const auto& actor : snapshot->actor_records())
            births.push_back({gc::GalacticSession::NativeActorChange::Kind::created, actor.id,
                actor.generated_source, actor.dead, actor.delete_pending, {}});
        must(cp::apply_galactic_ai_actor_changes(actions->qualified_freestore(), births,
            [content = selection_content, members, snapshot](auto actor) {
                return cp::read_stock_galactic_ai_actor_facts(*content, members, *snapshot, actor);
            }, executor));
        const auto offers = must(ga::initial_freestore_offers(*snapshot, actions->qualified_freestore()));
        must(actions->qualified_freestore().initialize(empire, offers));
        engine->set_galactic_stage_usability([provider = usability](auto player, const auto& value, auto planet) {
            return provider->usability(player, value, planet);
        });
        engine->set_galactic_stage_garrison([](auto, const auto& value, auto actor) { return value.actor_garrison(actor); });
        engine->set_galactic_stage_combat_skip([](auto, const auto& value, auto actor) { return value.actor_combat_skip(actor); });
        freestore = std::make_shared<ga::FreestoreInput>();
        bind(galaxy, executor);
        const auto weak_engine = std::weak_ptr<ai::Engine>(engine);
        auto free_selection = ga::qualified_freestore_selection(freestore, world, engine,
            [weak_engine](auto player, auto target, auto actor, const auto& plan) {
                const auto owner = weak_engine.lock();
                if (!owner) throw std::runtime_error("Normal selector engine owner expired");
                return owner->galactic_freestore_selection_facts(player, target, actor, plan);
            });
        engine->set_galactic_selection(ai::stock_selection_provider(selection_content, selection_plans, world, engine,
            [&galaxy] { return galaxy.snapshot(); }, {}, std::move(free_selection)));
    }
    void script_service(const eawr::sim::PartitionExecutor& executor) {
        must(engine->commit_galactic_events(scripts, sequence));
        // Lua instances service in partitions and EvaluatePerception queries the
        // world: refresh its copied economy input once, before the fan-out (#2971).
        auto result = [&] { const auto reads = world->partitioned_reads(); return must(scripts.service(executor)); }();
        if (!result.diagnostics.empty()) throw std::runtime_error(result.diagnostics.front().message);
        must(engine->after_service(result));
    }
};
}
namespace eawr::script::foc::ai {
struct EngineContractAccess {
    static std::string starbase_stock_facts(Engine& e, const Owner& owner, gc::GalacticSession& galaxy) {
        const auto& content = *owner.selection_content;
        const auto marker = std::find_if(content.types.begin(), content.types.end(),
            [](const auto& row) { return row.second.name == "REBEL_STAR_BASE_1"; });
        expect(marker != content.types.end(), "installed station plan marker is bound");
        const auto first = content.initial_starbase_types.find(owner.empire);
        expect(first != content.initial_starbase_types.end() && content.types.at(first->second).name == "EMPIRE_STAR_BASE_1",
            "WGA-481 content publishes the sourced Empire initial-station lookup independently of the Rebel marker");
        auto initial = gc::PlanetProduction{}; initial.target_owner = owner.empire; initial.maximum_space_level = 5;
        expect(must(galactic_starbase_proposal(content, &initial, marker->first)) == first->second,
            "WGA-481 a Rebel marker at an empty Empire target resolves to an Empire station");
        const auto plan_at = std::find_if(e.plans_.begin(), e.plans_.end(), [](const auto& row) { return row.name == "UPGRADESTARBASEPLAN"; });
        expect(plan_at != e.plans_.end(), "installed upgrade plan is registered");
        auto plan = *plan_at; plan.allow_free_store = false;
        auto provider = stock_selection_provider(owner.selection_content, owner.selection_plans, owner.world, owner.engine,
            [&galaxy] { return galaxy.snapshot(); });
        const auto snapshot = galaxy.snapshot();
        std::shared_ptr<const GalacticSelectionInput> facts;
        gc::TypeId expected{};
        gc::PlanetId selected_target{};
        for (const auto& target : snapshot->production()->planets) {
            if (target.target_owner != owner.empire || !target.starbase_type) continue;
            const auto next = must(galactic_starbase_proposal(content, &target, marker->first));
            if (next == marker->first) continue;
            auto candidate = must(provider(owner.empire, target.planet, e.data_.goals.at("UPGRADE_STARBASE"), plan));
            if (!candidate->complete || !candidate->types.contains(next) || !candidate->types.at(next).target_cost) continue;
            expect(content.types.at(next).original->base_level == target.space_level + 1,
                "WGA-481 current station follows its authored next type rather than the marker's faction or level");
            expected = next; selected_target = target.planet; facts = std::move(candidate); break;
        }
        expect(facts != nullptr, "installed campaign has an admitted next Empire station quote");
        expect(facts->complete && facts->proposal_substitutions.at(marker->first) == expected
            && facts->types.contains(expected) && !facts->types.contains(marker->first),
            "WGA-481 actual content provider substitutes before demanding producer/type facts");
        const auto& base = facts->types.at(expected);
        expect(base.base && !base.structure && base.target_cost && base.target_build_seconds,
            "WGA-58/481 next station has an admitted target quote without special-structure slot classification");
        const auto ground = std::find_if(content.types.begin(), content.types.end(),
            [](const auto& row) { return row.second.ground_base && row.second.name == "EMPIRE_GROUND_BASE_1"; });
        expect(ground != content.types.end(), "installed ground-base proposal type is bound");
        auto ground_plan = plan; ground_plan.ignore_target = true;
        for (auto& force : ground_plan.taskforces) for (auto& team : force.teams) team.types = {ground->first};
        const auto ground_facts = must(provider(owner.empire, selected_target, e.data_.goals.at("UPGRADE_STARBASE"), ground_plan));
        const auto& ground_base = ground_facts->types.at(ground->first);
        expect(ground_facts->complete && ground_base.base && !ground_base.structure
            && ground_base.contrast_fallback_channel == gc::ProductionDomain::ground,
            "WGA-482 actual ground-base content takes the base production path, independently of special structures");
        const auto admitted = std::find_if(ground_facts->producers.begin(), ground_facts->producers.end(), [&](const auto& producer) {
            return producer.type == ground->first && producer.planet == selected_target && producer.admission == gc::ProductionAdmission::allowed;
        });
        expect(ground_base.target_cost.has_value() == (admitted != ground_facts->producers.end())
            && ground_base.target_build_seconds.has_value() == ground_base.target_cost.has_value(),
            "WGA-482 ground-base quote exists only for admitted target production; stock dummy bases do not invent a quote");
        if (admitted != ground_facts->producers.end()) expect(ground_base.target_cost == numeric::from_fixed(admitted->cost)
            && ground_base.target_build_seconds == numeric::from_fixed(admitted->build_seconds),
            "WGA-482 ground-base selection publishes the target's actual cost and time");
        {
            // WGA-489 through the stock provider: a generic hero company that authors no object script is an ordinary
            // unit (no hero stage); a named hero keeps the stage only while hero attachment is allowed.
            // HA-01/HA-04 keep an explicitly requested hero ordinary when extra attachment is denied.
            const auto find_type = [&](std::string_view name) {
                const auto at = std::find_if(content.types.begin(), content.types.end(), [&](const auto& row) { return row.second.name == name; });
                expect(at != content.types.end(), "WGA-489 installed hero company type is bound");
                return at->first;
            };
            const auto saboteur = find_type("UNDERWORLD_SABOTEUR_TEAM"), smuggler = find_type("SMUGGLER_TEAM_E"), mothma = find_type("MON_MOTHMA_TEAM");
            expect(content.types.at(saboteur).value.hero && content.types.at(saboteur).lua_script.empty() && !content.types.at(saboteur).named_hero
                && content.types.at(mothma).value.hero && content.types.at(mothma).lua_script.empty() && content.types.at(mothma).named_hero,
                "WGA-489 installed content: the saboteur is a generic unscripted hero, Mon Mothma a named unscripted hero");
            // A company with no script of its own takes the hero stage when a member unit authors one (the generic
            // fleet commanders); the saboteur and smuggler companies have neither, so their members are ordinary units.
            const auto fleet_empire = find_type("FLEET_COM_EMPIRE_TEAM"), fleet_rebel = find_type("FLEET_COM_REBEL_TEAM");
            expect(content.types.at(fleet_empire).lua_script.empty() && content.types.at(fleet_empire).member_script
                && content.types.at(fleet_rebel).lua_script.empty() && content.types.at(fleet_rebel).member_script
                && !content.types.at(saboteur).member_script && !content.types.at(smuggler).member_script,
                "WGA-489 installed content: fleet commander companies author their script on the member unit, saboteur and smuggler have none");
            auto hero_plan = plan; hero_plan.ignore_target = true;
            for (auto& force : hero_plan.taskforces) for (auto& team : force.teams) team.types = {saboteur, smuggler, mothma, fleet_empire, fleet_rebel};
            const auto hero_facts = must(provider(owner.empire, selected_target, e.data_.goals.at("UPGRADE_STARBASE"), hero_plan));
            expect(hero_facts->types.contains(saboteur) && hero_facts->types.contains(smuggler) && hero_facts->types.contains(mothma)
                && hero_facts->types.contains(fleet_empire) && hero_facts->types.contains(fleet_rebel)
                && !hero_facts->types.at(saboteur).hero && !hero_facts->types.at(smuggler).hero && hero_facts->types.at(mothma).hero
                && hero_facts->types.at(fleet_empire).hero && hero_facts->types.at(fleet_rebel).hero,
                "WGA-489 stock provider: generic heroes with no company or member script skip the hero stage; named heroes and member-scripted fleet commanders keep it");
        }
        {
            const auto rebel_plan = std::find_if(e.plans_.begin(), e.plans_.end(),
                [](const auto& row) { return row.name == "REBELADVANCETECHPLAN"; });
            const auto droids = std::find_if(content.types.begin(), content.types.end(),
                [](const auto& row) { return row.second.name == "DROIDS_TEAM"; });
            expect(rebel_plan != e.plans_.end() && droids != content.types.end() && droids->second.named_hero,
                "HA-01 installed Rebel tech plan names the actual named droid hero team");
            const auto& constraints = owner.selection_plans->constraints.at(rebel_plan->name);
            expect(constraints.forces.size() == 1 && constraints.forces.front().deny_heroes
                && rebel_plan->taskforces.size() == 1 && rebel_plan->taskforces.front().teams.size() == 1
                && rebel_plan->taskforces.front().teams.front().types == std::vector<gc::TypeId>{droids->first},
                "stock Rebel tech plan explicitly requires Droids_Team while denying extra hero attachment");
            const auto no_extra_heroes = [](auto, auto, const auto&) {
                return core::Result<std::vector<GalacticSelectionObject>>::success({});
            };
            const auto denied = stock_selection_provider(owner.selection_content, owner.selection_plans, owner.world, owner.engine,
                [&galaxy] { return galaxy.snapshot(); }, {}, no_extra_heroes);
            const auto rebel_facts = must(denied(owner.empire, 0, e.data_.goals.at("ADVANCE_TECH_REBEL"), *rebel_plan));
            expect(rebel_facts->complete && !rebel_facts->attachment_allowed.front().first && rebel_facts->types.contains(droids->first)
                && !rebel_facts->types.at(droids->first).hero && rebel_facts->types.at(droids->first).hero_weight == Real{},
                "HA-01/HA-04 actual Droids_Team retains its declared proposal with neutral hero weight and no hero-owner dependency");
        }
        return std::to_string(expected) + ":" + std::to_string(base.target_cost->repr) + ":" + std::to_string(base.target_build_seconds->repr)
            + ":" + std::to_string(ground->first) + ":" + std::to_string(ground_base.target_cost.has_value());
    }
    static void running_maintenance(Engine& e, tactical::PlayerId player) {
        auto& p = *e.player_ai(player);
        p.sleep_frames = 0; p.maintenance_due = true;
    }
    // WGA-341: Produce_Force disables the Potential contrast test only without
    // a staging planet. Stock AssembleForce passes the retained Get_Stage
    // planet (WGA-226/254), so the paid tasks carry it and contrast stays on.
    struct PaidStaging {
        bool goal_found{}, potential_found{}, contrast_disabled{};
        std::size_t paid_tasks{}, staged_paid_tasks{};
        bool operator==(const PaidStaging&) const = default;
    };
    static PaidStaging paid_staging(Engine& e, tactical::PlayerId player, std::uint64_t goal) {
        PaidStaging result;
        auto& p = *e.player_ai(player);
        const auto* retained = e.find_goal(p, goal);
        if (!retained) return result;
        result.goal_found = true;
        if (!retained->potential) return result;
        result.potential_found = true;
        result.contrast_disabled = retained->potential->target_contrast_disabled;
        for (const auto& task : p.tasks) {
            const auto force = p.taskforces.find(task.taskforce);
            if (task.source != 2 || force == p.taskforces.end() || force->second.plan != retained->plan) continue;
            ++result.paid_tasks;
            result.staged_paid_tasks += task.staging != 0 ? 1 : 0;
        }
        return result;
    }
    // A rebuilt owner holds the empty view it was created with until its first bind publishes one.
    static void rebuild_pre_publication_view(Engine& e) { e.host_->view = std::make_shared<detail::WorldView>(); }
    static void missing_stock_qualifications(Engine& e, tactical::PlayerId player) {
        const auto& p = *e.player_ai(player);
        expect(!p.functions.empty() && !p.targets.empty(), "stock qualification test has authored functions and real planets");
        Goal goal; goal.function = 0; goal.target = p.targets.front();
        for (const auto& plan : e.plans_) if (plan.magic || plan.allow_free_store) {
            const auto input = e.galactic_selection(p, goal, plan);
            expect(input && !input->complete && !input->missing_facts.empty(), "unsupported stock plan names and refuses its missing live qualifications");
        }
    }
    static std::string targeted_stock_facts(Engine& e, tactical::PlayerId player,
        const galactic_goals::MaterializedPlanCatalog& catalog) {
        auto& p = *e.player_ai(player);
        std::string trace; std::set<std::string> plans; std::size_t classified{};
        for (std::size_t function = 0; function < p.functions.size(); ++function) {
            const auto& entry = p.functions[function];
            if (!e.proposable(p, entry) || entry.goal->application.contains("GLOBAL")) continue;
            for (const auto target : p.targets) {
                if (!e.applies(p, *entry.goal, e.target(target))) continue;
                const auto allocated = must(e.allocate_galactic_goal(player, function, target));
                if (!allocated) continue;
                const auto& goal = p.proposed.back();
                const auto& definition = e.plans_.at(goal.potential->plan);
                if (definition.ignore_target || definition.magic
                    || catalog.constraints.at(definition.name).additional_cost.value_or(Real{}) != Real{}) {
                    must(e.release_galactic_goal(player, allocated->goal)); continue;
                }
                expect(definition.galactic_contrast_qualified, "installed targeted plan retains its own contrast definition");
                plans.insert(definition.name);
                const auto input = e.galactic_selection(p, goal, definition);
                expect(input && input->complete && input->missing_facts.empty() && input->unscaled_domain_totals,
                    "installed ordinary targeted plan composes qualified target/garrison facts through the production provider");
                expect(input->contrast.size() == 2 + 2 * definition.galactic_contrast_order.size()
                    && !input->contrast[0].category && !input->contrast[1].category,
                    "installed target facts retain unscaled domain totals before plan-authored categories");
                trace += definition.name + ':' + std::to_string(target) + ':';
                for (const auto& row : input->contrast) {
                    classified += row.category != 0;
                    trace += std::to_string(row.category) + ':' + std::to_string(row.force.repr) + ';';
                }
                for (const auto& [type, facts] : input->types) for (const auto& garrison : facts.garrisons) {
                    expect(garrison.all_main_domains, "installed proposal garrisons admit either classified main domain");
                    trace += std::to_string(type) + ':' + std::to_string(garrison.type) + ':' + std::to_string(garrison.power.repr) + ';';
                }
                must(e.release_galactic_goal(player, allocated->goal));
            }
        }
        // Day-one desire/policy can legitimately allocate no ordinary targeted
        // proposal. Record the actual sample across workers; natural engagement
        // is witnessed by the full campaign, not forced into this paid fixture.
        trace += "day-one-plans:" + std::to_string(plans.size()) + ":classified:" + std::to_string(classified);
        std::cout << "day-one targeted plan facts plans=" << plans.size() << " classified=" << classified << '\n';
        return trace;
    }
    static std::optional<std::uint64_t> reserve_stock(Engine& e, tactical::PlayerId player) {
        auto& p = *e.player_ai(player);
        for (std::size_t function = 0; function < p.functions.size(); ++function) {
            const auto& entry = p.functions[function];
            if (!e.proposable(p, entry) || entry.goal->application.contains("GLOBAL")) continue;
            for (const auto target : p.targets) {
                if (!e.applies(p, *entry.goal, e.target(target))) continue;
                const auto id = must(e.allocate_galactic_goal(player, function, target));
                if (!id) continue;
                auto& goal = p.proposed.back();
                if (e.select_units(p, goal, *goal.potential) && !e.plans_[goal.potential->plan].magic
                    && Real{} < goal.potential->cost && e.production_time_allowed(p, goal) && e.galactic_fundable(p, goal, false)) {
                    e.reserve(p, goal); p.next_function = p.functions.size(); return goal.id;
                }
                must(e.release_galactic_goal(player, id->goal));
            }
        }
        return {};
    }
};
}
int main(int argc, char** argv) {
    try {
        const bool normal_acceptance = argc == 2 && std::string_view(argv[1]) == "--normal";
        expect(argc == 1 || normal_acceptance, "stock selector accepts only the explicit --normal gate proof option");
        const auto root = std::filesystem::path(game_root());
        std::vector<eawr::vfs::MountSpec> mounts;
        for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
            mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
        auto fs = must(eawr::vfs::Vfs::mount(mounts));
        const auto loaded = must(eawr::data::load_catalog(fs, eawr::data::Profile::foc));
        const auto data = must(eawr::data::galactic::load_galactic_data(fs, loaded.catalog));
        const auto types = eawr::units::galactic_type_table(loaded.catalog);
        constexpr std::string_view campaign_name = "Sandbox_Equal_Footing_Empire";
        const auto* source = data.campaign(campaign_name); expect(source != nullptr, "stock campaign exists");
        cp::CampaignSetupOptions options; options.seed = 42;
        for (const auto& faction : data.factions) {
            const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
            options.players.push_back({{id, id, 0}, faction.name, true});
            const bool human = faction.name == "Rebel";
            if (human) options.solo_reference_human = id;
            if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
        }
        std::vector<std::string> originals;
        for (const auto& type : types.types) originals.push_back(type.name);
        std::sort(originals.begin(), originals.end());
        for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});
        const auto planned = must(cp::plan_live_campaign_setup(fs, data, types, campaign_name, options, "Normal_Default"));
        const auto services = must(cp::prepare_live_campaign_services(fs, loaded.catalog, data, types, planned, "Normal_Default"));
        const auto content = must(cp::prepare_ai_selection_content(fs, loaded.catalog, data, types, planned));
        std::size_t orbital_types{};
        for (const auto& [id, type] : content->types) {
            (void)id;
            if (!type.original || std::find(type.original->behavior.begin(), type.original->behavior.end(),
                "DUMMY_ORBITAL_STRUCTURE") == type.original->behavior.end()) continue;
            ++orbital_types;
            expect(type.value.structure && type.value.channel == gc::ProductionDomain::space,
                "RGC-01/04 stock orbital types take the AI targeted space-structure selection path");
        }
        expect(orbital_types > 0, "stock selection regression exercises authored orbital structures");
        const auto station = std::find_if(content->types.begin(), content->types.end(),
            [](const auto& row) { return row.second.starbase && row.second.next_base; });
        expect(station != content->types.end(), "installed station chain available for missing-link boundary");
        const auto dangling_catalog = must(eawr::data::with_overrides(loaded.catalog,
            std::array{eawr::data::ValueOverride{station->second.name, "Next_Level_Base", {"UNRESOLVED_NEXT_BASE"}}}));
        const auto dangling_content = must(cp::prepare_ai_selection_content(fs, dangling_catalog, data, types, planned));
        expect(dangling_content->types.at(station->first).next_base == 0
            && dangling_content->types.at(station->first).original->next_level_base == "UNRESOLVED_NEXT_BASE",
            "WGA-483 actual content preparation retains an unknown next-level name as a null resolved link");
        gc::PlanetProduction dangling_target; dangling_target.target_owner = dangling_target.starbase_owner = 1;
        dangling_target.starbase_type = station->first; dangling_target.space_level = 1; dangling_target.maximum_space_level = 5;
        expect(must(ai::galactic_starbase_proposal(*dangling_content, &dangling_target, station->first)) == station->first,
            "WGA-483 actual prepared unknown link preserves the original proposal");
        ga::LifecycleSetup selected;
        for (const auto& path : ga::required_xml()) selected.xml.emplace(path, read(fs, path));
        const auto ai_data = must(ga::load_data(selected));
        std::map<std::string, std::string> modules;
        for (const auto& asset : must(fs.enumerate("data/scripts", ".lua"))) {
            if (asset.canonical_path.starts_with("data/scripts/library/") || (asset.canonical_path.starts_with("data/scripts/ai/") && asset.canonical_path.substr(16).find('/') == std::string::npos))
                modules.emplace(asset.canonical_path, read(fs, asset.canonical_path));
        }
        // Combat tables are immutable selected content, not executable plan modules.
        const auto catalog = must(ai::galactic_goals::load_plan_catalog({}, modules));
        auto setup = *must(cp::prepare_ai_perception_content(fs, data, planned, *content, "Normal"));
        std::shared_ptr<const cp::GalacticAiRuntimeSetup> normal_runtime;
        std::shared_ptr<const cp::GalacticAiUsabilityContent> normal_usability;
        if (normal_acceptance) {
            const auto empire = std::find_if(setup.players.begin(), setup.players.end(), [](const auto& row) { return row.faction == "Empire"; });
            expect(empire != setup.players.end(), "Normal selection has its authored Empire player");
            normal_runtime = must(cp::prepare_stock_galactic_ai_runtime(fs, loaded.catalog, data, types, planned,
                {{empire->player, "BASICEMPIRE", "NORMAL", {}, {}}}, std::make_shared<eawr::platform::ThreadWorkerAdapter>(1), setup.uses_fog, 4096, 4096));
            normal_usability = must(cp::prepare_stock_galactic_ai_usability(data, planned, normal_runtime->perception));
        }
        expect(setup.economy_types_complete && setup.economy_types.size() == content->types.size()
            && setup.maximum_ground_level && ai::Real{} < *setup.maximum_ground_level
            && setup.maximum_space_level && ai::Real{} < *setup.maximum_space_level,
            "all original identities and authored raw base maxima publish for economy perception");
        for (const auto& planet : setup.planets)
            expect(std::any_of(setup.economy_types.begin(), setup.economy_types.end(), [&](const auto& row) { return row.type == planet.type; })
                && std::any_of(setup.force_types.begin(), setup.force_types.end(), [&](const auto& row) { return row.type == planet.type; }),
                "original planet CRC identity has qualified economy and force metadata");
        std::size_t indigenous_rows{}, multiple_indigenous_planets{}, empty_indigenous_planets{}, special_weapons{};
        for (const auto& planet : setup.planets) {
            expect(planet.indigenous_potential.has_value(), "installed authored indigenous omission is a known empty list");
            indigenous_rows += planet.indigenous_potential->size();
            multiple_indigenous_planets += planet.indigenous_potential->size() > 1;
            empty_indigenous_planets += planet.indigenous_potential->empty();
            for (const auto& row : *planet.indigenous_potential) {
                expect(planned.original_type(row.type) != nullptr, "indigenous rows preserve bound original type identity");
                for (const auto player : row.affiliations)
                    expect(std::any_of(setup.players.begin(), setup.players.end(), [&](const auto& value) { return value.player == player; }),
                        "indigenous affiliations name actual selected players");
            }
        }
        for (const auto& [id, type] : content->types) { (void)id; special_weapons += type.special_weapon; }
        expect(indigenous_rows && multiple_indigenous_planets && empty_indigenous_planets && special_weapons,
            "original campaign exercises empty, single and repeated indigenous authoring plus special weapon metadata");
        // WGA-279/280: exercise the actual catalog/provider path, including
        // present no-digit tokens versus occurrences missing their second token.
        const auto empty_planet = std::find_if(content->planets.begin(), content->planets.end(),
            [](const auto& row) { return row.second.indigenous_potential.empty(); });
        expect(empty_planet != content->planets.end(), "original known-empty planet available for authoring boundary");
        const auto location = std::find_if(planned.locations().begin(), planned.locations().end(),
            [&](const auto& row) { return row.planet && row.planet->id == empty_planet->first; });
        expect(location != planned.locations().end(), "authoring boundary preserves original planet identity");
        const auto& original_type = content->types.begin()->second;
        const auto boundary_catalog = must(eawr::data::with_overrides(loaded.catalog,
            std::array{eawr::data::ValueOverride{location->original_type, "Potential_Indigenous_Power",
                {original_type.name + " abc", original_type.name + " 5x", original_type.name + " +5",
                 original_type.name + " +-3", original_type.name, original_type.name + " -2 extra"}}}));
        const auto boundary_content = must(cp::prepare_ai_selection_content(fs, boundary_catalog, data, types, planned));
        const auto& boundary = boundary_content->planets.at(empty_planet->first).indigenous_potential;
        const std::array expected_powers{0, 5, 5, 0, -2};
        expect(boundary.size() == expected_powers.size(), "missing second token appends no row; repeated rows remain ordered");
        for (std::size_t row = 0; row < boundary.size(); ++row)
            expect(boundary[row].type == content->types.begin()->first && boundary[row].power == ai::real(expected_powers[row]),
                "original bound type preserves atoi decimal-prefix/no-digit/multiple-sign semantics");
        {
            // CGS-07: an unbound type name is a null reference; it is reported and the row is left out, not refused.
            const auto unresolved_catalog = must(eawr::data::with_overrides(loaded.catalog,
                std::array{eawr::data::ValueOverride{location->original_type, "Potential_Indigenous_Power", {std::string("UNRESOLVED_INDIGENOUS_TYPE 5")}}}));
            const auto unresolved = must(cp::prepare_ai_selection_content(fs, unresolved_catalog, data, types, planned));
            expect(unresolved->planets.at(empty_planet->first).indigenous_potential.empty()
                    && std::any_of(unresolved->unresolved_names.begin(), unresolved->unresolved_names.end(),
                        [](const auto& line) { return line.ends_with(": UNRESOLVED_INDIGENOUS_TYPE"); }),
                "unresolved indigenous type is reported and adds no synthetic row");
        }
        {
            const auto rejected_catalog = must(eawr::data::with_overrides(loaded.catalog,
                std::array{eawr::data::ValueOverride{location->original_type, "Potential_Indigenous_Power", {original_type.name + " 2147483648"}}}));
            expect(!cp::prepare_ai_selection_content(fs, rejected_catalog, data, types, planned),
                "out-of-range mod admission refuses without synthetic rows");
        }
        const auto identity = eawr::core::sha256_hex(std::span<const std::uint8_t>{});
        std::string baseline;
        for (const auto count : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter executor(count);
            auto campaign = must(cp::CampaignRoot::create(planned, services));
            auto galaxy = must(gc::GalacticSession::create(campaign.game()->galaxy().setup()));
            expect(galaxy.state_sha256() == campaign.game()->galaxy().state_sha256(), "mutable parent starts from exact stock live setup");
            Owner control(galaxy, executor, setup, ai_data, content, catalog, modules, normal_acceptance);
            ai::EngineContractAccess::missing_stock_qualifications(*control.engine, control.empire);
            if (!normal_acceptance) {
                const auto trace = galaxy.state_sha256() + control.scripts.state_digest() + must(control.world->state_hash());
                if (baseline.empty()) baseline = trace; else expect(trace == baseline, "installed authored content and named stock qualification refusals equal on 1/2/4/8");
                std::cout << "stock selector content indigenous=" << indigenous_rows << " multiple=" << multiple_indigenous_planets
                    << " omitted=" << empty_indigenous_planets << " special=" << special_weapons << " workers=" << count
                    << "; Normal paid continuation is a separate --normal gate proof\n";
                continue;
            }
            control.qualify(galaxy, executor, normal_runtime, normal_usability);
            const auto starbase_trace = ai::EngineContractAccess::starbase_stock_facts(*control.engine, control, galaxy);
            const auto targeted_trace = ai::EngineContractAccess::targeted_stock_facts(*control.engine, control.empire, *control.selection_plans);
            const auto goal = ai::EngineContractAccess::reserve_stock(*control.engine, control.empire);
            expect(goal.has_value(), "Normal Empire positive authored goal selects and reserves real paid proposal");
            ai::GoalStateContributor saved(control.engine, identity);
            const auto bytes = must(ga::encode_native_state(must(saved.capture())));
            const DiskCheckpoint disk;
            { std::ofstream output(disk.path, std::ios::binary);
                output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                output.close(); expect(static_cast<bool>(output), "installed mid-reservation checkpoint writes to disk"); }
            std::ifstream input(disk.path, std::ios::binary);
            const std::vector<std::uint8_t> from_disk{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            expect(from_disk == bytes, "installed mid-reservation checkpoint reads exact disk bytes");
            const auto funding = control.funding->state();
            const auto hash = must(saved.state_hash());
            // Replacement owner is independently constructed from the same installed content.
            auto replacement = must(gc::GalacticSession::create(galaxy.setup()));
            Owner resumed(replacement, executor, setup, ai_data, content, catalog, modules);
            resumed.qualify(replacement, executor, normal_runtime, normal_usability);
            must(resumed.world->restore(control.world->state())); must(resumed.funding->restore(funding));
            ai::GoalStateContributor restored(resumed.engine, identity); must(restored.restore(must(ga::decode_native_state(from_disk))));
            expect(must(restored.state_hash()) == hash, "installed mid-reservation continuation preserves complete native state");
            const auto continue_owner = [&](Owner& owner, gc::GalacticSession& parent) {
                owner.bind(parent, executor);
                must(owner.engine->service_galactic_system(owner.empire, ga::Service::goals, owner.scripts, owner.sequence));
                const auto running = owner.engine->galactic_goal(owner.empire, *goal);
                expect(running && running->running_plan, "completed live maintenance attaches selected stock plan");
                // Data/Scripts/AI/BuildWeaponStructurePlan.lua:63 calls Sleep(1)
                // before AssembleForce. Service must publish the real
                // stepped mode clock, as the production galactic bridge does.
                // One game second is authored; the second is a test margin
                // for the later native execution/admission service.
                const auto first_frame = parent.clock().galactic_frame;
                for (unsigned barrier = 0; barrier < 2 * gc::galactic_frames_per_second
                    && owner.actions->purchases().empty(); ++barrier) {
                    (void)must(parent.step(executor));
                    // Rebind completed snapshots, mode time and qualified
                    // producer/route facts after the actual parent step.
                    owner.bind(parent, executor);
                    owner.script_service(executor);
                    must(owner.engine->service_galactic_system(owner.empire, ga::Service::execution, owner.scripts, owner.sequence));
                }
                expect(!owner.actions->purchases().empty(), "actual installed Lua Produce_Force admits paid build through execution owner");
                expect(parent.clock().galactic_frame >= first_frame + gc::galactic_frames_per_second,
                    "actual parent mode clock services the stock plan's one-second wait before paid admission");
                const auto staging = ai::EngineContractAccess::paid_staging(*owner.engine, owner.empire, *goal);
                expect(staging.goal_found && staging.potential_found,
                    "retained Normal goal still carries its selected Potential plan after paid admission");
                expect(staging.paid_tasks != 0 && staging.paid_tasks == staging.staged_paid_tasks,
                    "actual stock AssembleForce passes the retained Get_Stage planet to every paid Produce_Force task");
                expect(!staging.contrast_disabled,
                    "actual stock staged Produce_Force keeps the retained Potential target contrast");
                ai::GoalStateContributor task_codec(owner.engine, identity);
                const auto task_state = must(task_codec.capture()); must(task_codec.restore(task_state));
                owner.engine->set_galactic_funding(owner.funding);
                expect(ai::EngineContractAccess::paid_staging(*owner.engine, owner.empire, *goal) == staging
                    && must(task_codec.capture()).components == task_state.components,
                    "staged contrast choice and actual paid task round trip together");
                const auto builds = must(owner.engine->galactic_build_budgets(*owner.actions, *owner.funding));
                expect(!builds->empty() && builds->front().builder == owner.empire && builds->front().build != 0,
                    "actual installed Normal paid task publishes recorded builder/channel/build correlation");
                const auto plan = owner.engine->galactic_goal(owner.empire, *goal)->running_plan;
                ai::EngineContractAccess::running_maintenance(*owner.engine, owner.empire);
                must(owner.engine->service_galactic_system(owner.empire, ga::Service::goals, owner.scripts, owner.sequence));
                const auto maintained = owner.engine->galactic_goal(owner.empire, *goal);
                const auto ledger = owner.funding->ledger(owner.empire);
                expect(maintained && maintained->running_plan == plan && ledger->goals.contains(*goal)
                    && ledger->goals.at(*goal).reassessed
                    && must(eawr::sim::math::add(ledger->goals.at(*goal).capital, ledger->goals.at(*goal).accumulated)) > eawr::sim::math::Fixed{},
                    "later completed stock goal pass restores funding for the same attached paid Lua plan");
                const auto funded = must(owner.engine->galactic_build_budgets(*owner.actions, *owner.funding));
                expect(std::any_of(funded->begin(), funded->end(), [&](const auto& row) {
                    return row.build == builds->front().build && ai::Real{} < row.spendable;
                }), "actual stock queued build regains spendable funds after maintenance");
            };
            continue_owner(control, galaxy); continue_owner(resumed, replacement);
            {   // Restoring an attached staged plan validates its stage planet against the rebuilt world's
                // completed view, not the pre-publication view a reconstructed owner still holds.
                ai::GoalStateContributor attached(control.engine, identity);
                const auto attached_state = must(attached.capture());
                const auto attached_hash = must(attached.state_hash());
                ai::EngineContractAccess::rebuild_pre_publication_view(*control.engine);
                must(attached.restore(attached_state));
                expect(must(attached.state_hash()) == attached_hash,
                    "attached staged plan restores against the completed world view and preserves native state");
            }
            const auto running = resumed.engine->galactic_goal(resumed.empire, *goal);
            const auto trace = replacement.state_sha256() + must(restored.state_hash()) + must(resumed.actions->state_hash()) + resumed.scripts.state_digest();
            expect(trace == galaxy.state_sha256() + must(saved.state_hash()) + must(control.actions->state_hash()) + control.scripts.state_digest()
                && resumed.funding->state() == control.funding->state(), "fresh restored owner preserves next authored decisions, funding, Lua and paid admissions");
            if (baseline.empty()) baseline = starbase_trace + targeted_trace + trace; else expect(starbase_trace + targeted_trace + trace == baseline, "Normal targeted facts/selection/reservation/Lua paid admission equal on 1/2/4/8");
            std::cout << "Normal Empire live paid plan " << resumed.engine->plan(running->running_plan)->definition << ", builds=" << resumed.actions->purchases().size() << ", workers=" << count << '\n';
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
