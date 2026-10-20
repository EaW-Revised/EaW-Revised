#include "foc_ai_contract_fixtures.hpp"
#include "tactical_ai_world.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/abilities.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace foc_contract {


// GS-22/31: running allocations are revalidated at the existing maintenance branch.
void running_target_contrast() {
    for (const bool removable : {false, true}) for (const bool proposals : {false, true})
        for (const bool competing : {false, true}) for (int change = 0; change != 13; ++change) {
            World world;
            world.view->units.front().health = ai::real(1);
            auto enemy = world.view->units.front(); enemy.id = 11; enemy.owner = 1;
            world.view->units.push_back(enemy);
            if (change == 12) {
                auto stronger = world.host->setup.content.types.front(); stronger.type_id = 2;
                stronger.name = "STRONGER"; stronger.combat_power = ai::real(2000);
                world.host->setup.content.types.push_back(stronger);
                world.host->types.clear(); world.host->types_by_name.clear();
                for (const auto& type : world.host->setup.content.types) {
                    world.host->types.emplace(type.type_id, &type);
                    world.host->types_by_name.emplace(type.name, &type);
                }
                world.view->units.back().type = 2;
            }
            const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
            auto equations = ai::EquationSet::parse({{"contrast.xml", "<Root><Desire>50</Desire><Positive>1</Positive></Root>"}}, converters);
            expect(static_cast<bool>(equations), "GS-22: maintenance equations load");
            if (!equations) return;
            ai::AiData data; data.equations = std::move(equations).value();
            ai::PlanDef definition; definition.name = "HELD";
            definition.min_contrast = definition.max_contrast = ai::real(1);
            definition.ignore_target = change == 4;
            definition.magic = change == 8;
            ai::Engine engine(world.host, std::move(data), {definition});
            ai::EngineContractAccess::target_fixture(engine, 1, 11);
            ai::GoalType goal; goal.application = {"ENEMY_UNIT"}; goal.category = "HIGH";
            ai::GoalType competitor; competitor.application = {"GLOBAL"}; competitor.category = "HIGH";
            auto& player = ai::EngineContractAccess::active_desire_fixture(engine, goal, competitor, removable, proposals);
            player.difficulty.space_contrast_multiplier = ai::real(1);
            ai::EngineContractAccess::contrast_fixture(engine, player);
            if (proposals && competing) {
                player.proposed.front().desire = ai::real(200);
                for (std::size_t index = 2; index <= 3; ++index) {
                    auto function = player.functions[1]; function.goal_name += std::to_string(index);
                    player.functions.push_back(std::move(function));
                    ai::Goal candidate; candidate.id = index + 1; candidate.function = index;
                    candidate.desire = ai::real(202 - static_cast<std::int64_t>(index));
                    player.proposed.push_back(candidate);
                }
            }
            auto& selected = *player.active.front().potential;
            selected.target_contrast = {{0, ai::real(1000)}};
            selected.current_contrast = {{0, ai::real(0)}};
            if (change == 1 || change == 4 || change == 5 || change == 6 || change == 8)
                world.view->units.front().health = (ai::real(1) / ai::real(4));
            if (change == 2) player.difficulty.space_contrast_multiplier = ai::real(2);
            if (change == 3) player.reserved[10] = 99;
            if (change == 5) selected.target_contrast_disabled = true;
            if (change == 6) {
                auto spare = world.view->units.front(); spare.id = 12; spare.health = ai::real(1);
                world.view->units.push_back(spare);
            }
            if (change == 7) world.view->units.erase(world.view->units.begin());
            if (change == 9 || change == 10) {
                world.host->contrast_order = {2};
                world.host->contrast[2] = {{2, {}, (ai::real(1) / ai::real(change == 9 ? 1 : 2))}};
                selected.contrast_indices = {1};
            }
            if (change == 11) world.view->units.pop_back();
            auth::ModuleManifest modules;
            expect(modules.add("HELD.LUA", "function Hold() Sleep(120) end\n").has_value(), "GS-22: held module loads");
            auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
            if (!scripts) { expect(false, "GS-22: scheduler starts"); return; }
            expect(scripts.value().create_instance(1, "HELD.LUA").has_value(), "GS-22: held instance starts");
            std::uint64_t sequence = 0;
            ai::EngineContractAccess::bind_scripts(engine, scripts.value(), sequence);
            ai::EngineContractAccess::set_selection_seed(engine, 2, 12345);
            const bool valid = change == 0 || change == 4 || change == 5 || change == 7 || change == 8 || change == 9;
            if (change != 8) {
                expect(ai::EngineContractAccess::contrast(engine, player) == valid,
                    "GS-22: live health, difficulty and reservations revalidate the retained allocation");
                expect(selected.units == std::vector<tactical::TypeId>{1}
                    && selected.freestore == std::vector<eawr::sim::EntityId>{10}
                    && ai::EngineContractAccess::selection_seed(engine, 2) == 12345,
                    "GS-22: recheck neither borrows a spare unit nor draws random numbers");
                // A direct failed test is sticky; reset only to exercise the pass's call.
                selected.valid = true;
            }
            ai::EngineContractAccess::maintain(engine, player);
            const bool replaced = proposals && competing && removable && valid && change != 8;
            const bool tested_magic = proposals && competing && removable && change == 8;
            const bool retained = valid && !replaced && !tested_magic;
            expect((engine.plan(1) != nullptr) == retained && (engine.taskforce(1) != nullptr) == retained,
                "GS-22/31: failed contrast removes a running force even when protected or without proposals");
            expect(static_cast<bool>(scripts.value().thread_slots(1)) == retained,
                "GS-22: abandonment removes the associated script");
            if (!retained) {
                const auto& records = engine.records();
                expect(std::any_of(records.begin(), records.end(), [&](const auto& row) {
                    return row.plan == "HELD" && row.event == "maintenance_end"
                        && row.detail == (replaced ? "replacement" : "target_contrast");
                }), "GS-22/31: cullable failure and replacement have distinct end reasons");
                expect(std::any_of(records.begin(), records.end(), [](const auto& row) {
                    return row.plan == "HELD" && row.event == "abandoned";
                }), "GS-22: maintenance drops report abandonment");
            }
            if (!retained) expect(!player.assigned.contains(10) && (!player.reserved.contains(10) || player.reserved.at(10) == 99),
                "GS-22: abandoned goal releases its units and reservations");
        }
}


void running_reservation_contrast() {
    for (const bool pooled : {false, true}) for (int ownership = 0; ownership != 3; ++ownership) {
        World world; world.view->units.front().health = ai::real(1);
        auto mode = std::make_shared<ModeWorld>(); mode->mode = "SPACE";
        mode->provide_account = true; mode->reservation_account.pool_tokens = {77};
        mode->reservation_pads.emplace(88, tactical::PadState{});
        world.host->ai_world = mode;
        ai::PlanDef definition; definition.name = "HELD"; definition.ignore_target = true;
        ai::Engine engine(world.host, {}, {definition});
        ai::GoalType goal; goal.category = "HIGH";
        auto& player = ai::EngineContractAccess::active_desire_fixture(engine, goal, goal, true, false);
        ai::EngineContractAccess::contrast_fixture(engine, player);
        auto& allocation = *player.active.front().potential;
        allocation.freestore = {0}; allocation.sources = {static_cast<std::uint8_t>(pooled ? 1 : 2)};
        allocation.pool_tokens = {77}; allocation.producers = {88};
        if (pooled) {
            if (ownership != 0) player.reserved_pool_tokens[77] = ownership == 1 ? 1 : 99;
        } else {
            ai::EngineContractAccess::retain_pad(engine, 88);
            if (ownership != 0) ai::EngineContractAccess::reserve_pad(engine, 88, ownership == 1 ? 2 : 1, 1);
        }
        expect(ai::EngineContractAccess::contrast(engine, player) == (ownership == 1),
            "GS-22: live pool and pad allocations require their own reservation even with ignored contrast");
        allocation.valid = true;
        if (pooled) mode->reservation_account.pool_tokens.clear();
        else ai::EngineContractAccess::release_retained_pad(engine);
        expect(ai::EngineContractAccess::contrast(engine, player),
            "GS-22: consumed pool tokens and released pad allocations are no longer tested");
    }
}


// Selection reserves reinforcements before Lua necessarily calls Produce_Force.
// Every retirement stage must leave the pool usable and native state restorable.
void retiring_pool_reservations() {
    std::map<std::pair<unsigned, bool>, std::vector<std::uint8_t>> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) for (unsigned stage = 0; stage < 6; ++stage)
        for (const bool abandoned : {false, true}) {
            std::cout << "POOL-RETIREMENT workers=" << workers << " stage=" << stage << " abandoned=" << abandoned << '\n';
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto host = std::make_shared<ai::Host>();
            auto mode = std::make_shared<ModeWorld>(); mode->mode = "SPACE"; mode->frame = 1;
            mode->provide_account = true;
            mode->reservation_account.pool = {1, 1, 1}; mode->reservation_account.pool_tokens = {77, 78, 88};
            tactical::Player account_owner; account_owner.player_id = 2;
            mode->reservation_players.push_back(account_owner);
            host->ai_world = mode; host->view = mode->view();
            host->setup.players = {{2, "SYNTHETIC", false, true, "POOL"}};
            host->players.emplace(2, &host->setup.players.front());
            foc::AiType unit; unit.type_id = 1; unit.name = "POOL_UNIT";
            auto production = unit; production.type_id = 2; production.name = "PRODUCTION_UNIT";
            host->setup.content.types = {unit, production};
            for (const auto& type : host->setup.content.types) host->types.emplace(type.type_id, &type);
            const auto make_engine = [&]() {
                ai::AiData data;
                const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
                auto equations = ai::EquationSet::parse({{"pool.xml", "<Root><Zero>0</Zero></Root>"}}, converters);
                if (!equations) throw std::runtime_error("pool retirement fixture equations failed");
                data.equations = std::move(equations).value();
                ai::GoalType goal; goal.name = "POOL"; goal.mode = "SPACE"; goal.application = {"GLOBAL"};
                data.goals.emplace(goal.name, goal); data.function_sets["POOL"] = {{"POOL", "Zero"}};
                ai::PlayerType type; type.name = "POOL"; type.function_sets = {"POOL"}; data.players.emplace(type.name, type);
                ai::PlanDef plan; plan.name = "POOL"; plan.module = "POOL.LUA"; plan.goals = {"POOL"};
                plan.taskforces.push_back({"Main", {{1, 3, 0, false, {1, 2}}}, false, true, {}, 1});
                return std::make_unique<ai::Engine>(host, std::move(data), std::vector<ai::PlanDef>{plan});
            };
            auth::ModuleManifest modules;
            expect(modules.add("POOL.LUA", stage == 3 ? "function Base_Definitions() end"
                : "function Base_Definitions() end function Main_Thread() coroutine.yield(true) end").has_value(),
                "pool retirement fixture module loads");
            auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
            auto engine = make_engine(); std::uint64_t sequence{};
            expect(scripts && engine->before_service(*mode, scripts.value(), sequence, &executor), "pool retirement owner initializes");
            if (!scripts) return;
            auto& player = ai::EngineContractAccess::checked_player(*engine, 2);
            const auto tokens = stage == 5 ? std::vector<std::uint64_t>{77} : std::vector<std::uint64_t>{77, 78};
            auto& goal = ai::EngineContractAccess::pool_goal(*engine, player, tokens, true, stage == 4);
            const auto attached = ai::EngineContractAccess::attach(*engine, player, goal, scripts.value(), sequence);
            expect(attached.has_value(), "selected pool goal attaches through production");
            if (!attached) return;
            const auto* plan = engine->plan(goal.plan);
            expect(plan && plan->taskforces.size() == (stage == 3 ? 0U : 1U), "missing thread drops its selected TaskForce");
            const auto other_goal = ai::EngineContractAccess::pool_goal(*engine, player, {88}, false).id;
            player.reserved_pool[2] = 9;
            if (stage == 4) player.reserved_pool_tokens.at(77) = other_goal;
            if (stage == 1 || stage == 2 || stage == 5) {
                auth::ServiceReport report; report.tick = 1;
                auth::ScriptCommand command; command.issuer = plan->instance; command.sequence = 7;
                command.verb = std::string(ai::verb_ai);
                command.arguments = {auth::Value::text("produce"), auth::Value{auth::Handle{ai::handle_taskforce, plan->taskforces.front()}}};
                report.commands.push_back(std::move(command));
                expect(engine->after_service(report).has_value() && player.tasks.size() == tokens.size(),
                    "Produce_Force transfers selected tokens to pending build tasks");
                if (stage != 1) {
                    ai::EngineContractAccess::service(*engine, player, 1);
                    expect(engine->taskforce(plan->taskforces.front())->pooled.size() == tokens.size(),
                        "execution transfers pending tokens to the TaskForce pool");
                }
            }
            if (stage == 5) {
                auto view = std::make_shared<ai::WorldView>();
                ai::ViewUnit arrived; arrived.id = 10; arrived.type = 1; arrived.owner = 2;
                arrived.purchase_token = 77; arrived.purchase_type = 1; view->units.push_back(arrived);
                mode->world_view = view; host->view = view;
                mode->reservation_account.pool = {1}; mode->reservation_account.pool_tokens = {88};
                ai::EngineContractAccess::await_pool_arrival(*engine, player, goal.plan);
                ai::EngineContractAccess::reinforce_service(*engine, player, 1);
                expect(!player.reserved_pool_tokens.contains(77) && player.reserved_pool.at(1) == 1
                    && engine->taskforce(plan->taskforces.front())->units == std::vector<eawr::sim::EntityId>{10},
                    "reinforcement arrival consumes and releases its token before retirement");
            }
            for (auto event : ai::EngineContractAccess::take_attachment_events(*engine))
                expect(scripts.value().submit_event(std::move(event)).has_value(), "pool retirement attachment events publish");
            auto barrier = scripts.value().service(executor);
            expect(barrier.has_value(), "pool retirement reaches a completed script barrier");
            if (barrier) expect(engine->after_service(barrier.value()).has_value(), "pool retirement script results commit");
            const std::string identity(64, 'a');
            const auto reserved = engine->save_tactical_native(identity, scripts.value());
            expect(reserved.has_value(), "reserved live goal has valid native state");
            if (!reserved) { std::cerr << reserved.error().message << '\n'; continue; }
            auto resumed = make_engine();
            const auto loaded = resumed->restore_tactical_native(reserved.value(), identity, scripts.value());
            expect(loaded.has_value(), "reserved goal restores before retirement");
            if (!loaded) { std::cerr << loaded.error().message << '\n'; continue; }
            const auto checkpoint = resumed->save_tactical_native(identity, scripts.value());
            expect(checkpoint && checkpoint.value() == reserved.value(), "reserved goal round-trips before retirement");
            auto& retiring = ai::EngineContractAccess::checked_player(*resumed, 2);
            ai::EngineContractAccess::retire_pool_goal(*resumed, retiring, scripts.value(), abandoned);
            const bool transferred = stage == 4;
            expect(retiring.reserved_pool_tokens.size() == (transferred ? 2U : 1U)
                && retiring.reserved_pool_tokens.at(88) == other_goal && retiring.reserved_pool.at(1) == (transferred ? 2U : 1U)
                && (!transferred || retiring.reserved_pool_tokens.at(77) == other_goal),
                "retirement releases every owned token once and preserves a reassigned token's new owner");
            expect(retiring.reserved_pool.at(2) == 9, "new production does not release a pool reservation of its type");
            expect(retiring.tasks.empty(), "retirement removes pending and completed build tasks");
            const auto saved = resumed->save_tactical_native(identity, scripts.value());
            expect(saved.has_value(), "retired goal leaves no orphan pool reservation in native state");
            if (!saved) { std::cerr << saved.error().message << '\n'; continue; }
            auto restored = make_engine();
            expect(restored->restore_tactical_native(saved.value(), identity, scripts.value()).has_value(), "pool retirement state restores");
            const auto roundtrip = restored->save_tactical_native(identity, scripts.value());
            expect(roundtrip && roundtrip.value() == saved.value(), "pool retirement native bytes round-trip canonically");
            auto& baseline = reference[{stage, abandoned}];
            if (baseline.empty()) baseline = saved.value();
            else expect(baseline == saved.value(), "pool retirement native bytes agree on 1/2/4/8 workers");
        }
}


void active_tactical_desire() {
    for (const bool positive : {false, true}) for (const bool removable : {false, true})
        for (const bool proposals : {false, true}) {
            World world;
            world.view->units.front().health = ai::real(positive ? 1 : 0);
            const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
                return std::nullopt;
            };
            auto equations = ai::EquationSet::parse({{"maintenance.xml", "<Root>"
                "<Desire>50 * (Variable_Target.Health > 0.5)</Desire><Positive>1</Positive></Root>"}}, converters);
            expect(static_cast<bool>(equations), "GS-30: active desire equations load");
            if (!equations) return;
            ai::AiData data; data.equations = std::move(equations).value();
            ai::PlanDef definition; definition.name = "HELD";
            ai::Engine engine(world.host, std::move(data), {definition});
            ai::EngineContractAccess::target_fixture(engine, 1, 10);
            ai::GoalType goal; goal.application = {"FRIENDLY_UNIT"}; goal.category = "HIGH";
            ai::GoalType competitor; competitor.application = {"GLOBAL"}; competitor.category = "HIGH";
            auto& player = ai::EngineContractAccess::active_desire_fixture(engine, goal, competitor, removable, proposals);
            auth::ModuleManifest modules;
            expect(modules.add("HELD.LUA", "function Hold() Sleep(120) end\n").has_value(), "GS-31: held module loads");
            auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
            expect(static_cast<bool>(scripts), "GS-31: held scheduler starts");
            if (!scripts) return;
            expect(scripts.value().create_instance(1, "HELD.LUA").has_value(), "GS-31: held instance starts");
            std::uint64_t sequence = 0;
            ai::EngineContractAccess::bind_scripts(engine, scripts.value(), sequence);
            ai::EngineContractAccess::maintain(engine, player);
            const auto found = std::find_if(player.active.begin(), player.active.end(), [](const auto& entry) { return entry.id == 1; });
            const bool retained = positive || !removable;
            expect((found != player.active.end()) == retained && (engine.plan(1) != nullptr) == retained,
                "GS-31: zero-desire trimming honors removability with and without proposals");
            if (found != player.active.end()) expect(found->desire == ai::real(positive ? 50 : 0) && !found->finished,
                "GS-30: tactical active desire uses current target state without finishing a protected plan");
            expect(static_cast<bool>(scripts.value().thread_slots(1)) == retained,
                "GS-31: abandonment removes the plan script while protected plans keep their instance");
        }
}


void active_tactical_preference() {
    for (const std::string raw : {"0.01", "0.02", "10", "40"}) {
        World world;
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
            return std::nullopt;
        };
        auto equations = ai::EquationSet::parse({{"preference.xml", "<Root><Desire>" + raw
            + "</Desire><Positive>1</Positive></Root>"}}, converters);
        expect(static_cast<bool>(equations), "GS-31: preference equations load");
        if (!equations) return;
        ai::AiData data; data.equations = std::move(equations).value();
        ai::PlanDef definition; definition.name = "HELD";
        ai::Engine engine(world.host, std::move(data), {definition});
        ai::EngineContractAccess::target_fixture(engine, 1, 10);
        ai::GoalType goal; goal.application = {"FRIENDLY_UNIT"}; goal.category = "HIGH";
        ai::GoalType competitor; competitor.application = {"FRIENDLY_UNIT"}; competitor.category = "HIGH";
        competitor.is_like = {"RETAINED"};
        auto& player = ai::EngineContractAccess::active_desire_fixture(engine, goal, competitor, true, true);
        goal.per_failure_desire_adjust = goal.per_activation_failure_desire_adjust = ai::real(-50);
        player.goal_outcomes[{"RETAINED", 1}].entries.emplace_back(false, 10000);
        player.activations[{"RETAINED", 1}].entries.emplace_back(false, 10000);
        player.proposed.front().target = 1;
        player.proposed.front().desire = raw == "40" ? ai::real(41) : raw == "10" ? ai::real(11) : ai::real(1) / ai::real(2);
        auth::ModuleManifest modules;
        expect(modules.add("HELD.LUA", "function Hold() Sleep(120) end\n").has_value(), "GS-31: preference module loads");
        auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
        expect(static_cast<bool>(scripts), "GS-31: preference scheduler starts");
        if (!scripts) return;
        expect(scripts.value().create_instance(1, "HELD.LUA").has_value(), "GS-31: preference instance starts");
        std::uint64_t sequence = 0;
        ai::EngineContractAccess::bind_scripts(engine, scripts.value(), sequence);
        ai::EngineContractAccess::maintain(engine, player);
        const bool retained = raw == "0.02" || raw == "10";
        expect(player.active.size() == 1 && player.active.front().id == (retained ? 1U : 2U)
            && (engine.plan(1) != nullptr) == retained,
            "GS-31: active preference prevents nearby like-goal replacement only inside its open bounds");
        if (retained) expect(player.active.front().desire == ai::to_single(ai::real(raw == "10" ? 10 : 2) / ai::real(raw == "10" ? 1 : 100)),
            "GS-30: tactical active reevaluation does not reapply proposal failure adjustments");
    }
}


void initial_goal_budget() {
    World world;
    ai::Engine engine(world.host, ai::AiData{}, {});
    ai::GoalType global;
    global.application = {"GLOBAL"};
    ai::PlayerAi player;
    player.functions.resize(151);
    for (auto& function : player.functions) function.goal = &global;
    ai::EngineContractAccess::initialize_goals(engine, player);
    expect(player.per_frame == 2, "GS-05: an empty target list estimates one target and rounds 151 evaluations up");
    ai::EngineContractAccess::goals(engine, player);
    expect(player.next_function == 2 && player.nontrivial == 2
        && ai::EngineContractAccess::maintenances(engine) == 0,
        "GS-05: the first proposal service consumes the initialized budget");
    for (int frame = 1; frame < 76; ++frame) ai::EngineContractAccess::goals(engine, player);
    expect(ai::EngineContractAccess::maintenances(engine) == 1 && player.nontrivial == 0 && player.per_frame == 2,
        "GS-05: first maintenance uses the actual pass count and resets its accumulator");

    ai::PlayerAi sparse;
    sparse.targets.resize(151);
    sparse.functions.resize(1);
    sparse.functions.front().goal = &global;
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 2, "GS-05: initialization estimates targets times functions before applicability");
    ai::EngineContractAccess::goals(engine, sparse);
    expect(sparse.per_frame == 1 && ai::EngineContractAccess::maintenances(engine) == 2,
        "GS-05: later budgets use the actual one global evaluation, not the initial estimate");
    sparse.targets.resize(150);
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 1, "GS-05: an exact five-second evaluation budget does not round up");
    sparse.functions.clear();
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 1, "GS-05: no goal functions still leaves a positive service budget");

    world.host->setup.players.resize(8);
    world.host->setup.players.front() = foc::AiPlayer{1, "REBEL", false, false, "", true};
    for (std::size_t index = 1; index < 8; ++index) {
        world.host->setup.players[index] = foc::AiPlayer{static_cast<eawr::sim::tactical::PlayerId>(index + 1),
            index == 2 ? "UNDERWORLD" : "EMPIRE", index == 7, index <= 2, ""};
    }
    sparse.targets.resize(2000);
    sparse.functions.resize(1);
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 3,
        "GS-05: two AI and five non-playable faction players cap at rounded 20/7 with one human");
    world.host->setup.players.resize(5);
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 5,
        "GS-05: three-faction FFA with Empire/Underworld AI and two faction players counts only the human");
}


void selection_team_starts() {
    World world;
    auto second = world.host->setup.content.types.front();
    second.type_id = 2;
    second.name = "OTHER_SHIP";
    world.host->setup.content.types.push_back(second);
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const auto& type : world.host->setup.content.types) {
        world.host->types.emplace(type.type_id, &type);
        world.host->types_by_name.emplace(type.name, &type);
    }
    auto unit = world.view->units.front();
    unit.id = 11;
    unit.type = 2;
    world.view->units.push_back(unit);
    const auto force = [](eawr::sim::tactical::TypeId type) {
        ai::TaskForceDef result;
        ai::TeamDef team;
        team.max_count = 1;
        team.types = {type};
        result.teams.push_back(team);
        return result;
    };
    ai::PlayerAi player;
    player.player = 2;
    ai::PotentialPlan potential;
    ai::PlanDef plan;
    plan.taskforces = {force(1), force(2)};
    ai::Engine singleton(world.host, {}, {plan});
    expect(ai::EngineContractAccess::selection(singleton, player, potential, 12345),
        "PL-22: types with a single eligible team select both available objects");
    expect(potential.taskforce_of_unit == std::vector<std::size_t>{0, 1}
        && player.sync.seed() == 12345,
        "PL-22: distinct single-team types consume no synchronized draw");
    world.view->units.clear();
    ai::Engine empty(world.host, {}, {plan});
    expect(!ai::EngineContractAccess::selection(empty, player, potential, 12345)
        && player.sync.seed() == 12345,
        "PL-22: a multi-team plan without proposals consumes no synchronized draw");
    world.view->units = {unit};
    world.view->units.front().id = 10;
    world.view->units.front().type = 1;
    world.view->units.push_back(unit);
    plan.taskforces = {force(1), force(2), force(1), force(2)};
    ai::Engine independent(world.host, {}, {plan});
    expect(ai::EngineContractAccess::selection(independent, player, potential, 12345),
        "PL-22: types with multiple eligible teams select both available objects");
    expect(potential.taskforce_of_unit == std::vector<std::size_t>{0, 3}
        && player.sync.seed() == 0xa4451d33U && ai::EngineContractAccess::engine_seed(independent) == 0,
        "PL-22: each type draws independently among its own teams before allocation");
    plan.taskforces = {force(1), force(2)};
    plan.ignore_target = true;
    potential = {};
    ai::Engine untargeted(world.host, {}, {plan});
    expect(ai::EngineContractAccess::selection(untargeted, player, potential, 12345)
        && player.sync.seed() != 12345 && ai::EngineContractAccess::engine_seed(untargeted) == 0,
        "GFS-A24: ignore-target contrast weights advance only the selecting player's stream");
}


void threat_grid() {
    World world;
    ai::ThreatGrid grid;
    const foc::AiBounds bounds{ai::real(-1000), ai::real(1000), ai::real(1000), ai::real(-1000)};
    grid.partition(bounds, 5, 5, ai::Constants{});
    grid.service(*world.view, *world.host, 0);
    const ai::Rect all{ai::real(-1000), ai::real(-1000), ai::real(2000), ai::real(2000)};
    // PG-02: radius 500, cell 400 x 400: each of the 3 x 3 covered cells holds 1000 * 160000 / 1000000.
    const Real enemy = grid.force(*world.host, *world.view, all, ~std::uint64_t{0}, 1, false, Real{}, 0);
    expect(enemy == ai::real(1440), "PG-05: the enemy threat over the map is 9 cells of 160");
    expect(grid.force(*world.host, *world.view, all, ~std::uint64_t{0}, 1, true, Real{}, 0) == Real{},
        "PG-05: an enemy unit is no friendly threat");
    expect(grid.force(*world.host, *world.view, all, 1, 1, false, Real{}, 0) == Real{}, "PG-05: the category filters");
    expect(grid.total_force(*world.host, *world.view, ~std::uint64_t{0}, 1, false, Real{}) == ai::real(1000),
        "PG-06: the total enemy force is the unit's power");
    expect(grid.force_visibility(*world.host, *world.view, ~std::uint64_t{0}, 1) == ai::real(1),
        "PG-07: an AI player sees every enemy object");
    const Real none = grid.force_visibility(*world.host, *world.view, 1, 1);
    expect(none == none && none == ai::real(1), "PG-07: no object of the category is fully visible, not NaN");
    const ai::Rect corner{ai::real(-1000), ai::real(-1000), ai::real(100), ai::real(100)};
    expect(grid.force(*world.host, *world.view, corner, ~std::uint64_t{0}, 1, false, Real{}, 0) == Real{},
        "PG-02: a zone covers only the cells within its radius");
}


void unit_ai_grid_admission() {
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto& types = world.host->setup.content.types;
        types.clear();
        const auto type = [](unsigned id, const char* name, unsigned power, bool tracking) {
            foc::AiType result; result.type_id = id; result.name = name;
            result.category_bits = 1; result.combat_power = ai::real(power); result.unit_ai = tracking;
            return result;
        };
        types.push_back(type(1, "SCOUT_GROUP", 420, false));
        types.push_back(type(2, "SCOUT_MEMBER", 140, true));
        types.push_back(type(3, "FIGHTER_GROUP", 300, false));
        types.push_back(type(4, "FIGHTER_MEMBER", 60, true));
        types.push_back(type(5, "TRACKED_SCOUT_GROUP", 420, true));
        const auto identity = foc::tactical_ai_detail::native_configuration(world.host->setup);
        auto changed = world.host->setup;
        changed.content.types.front().unit_ai = true;
        const auto other_identity = foc::tactical_ai_detail::native_configuration(changed);
        expect(identity && other_identity && identity.value() != other_identity.value(),
            "PG-03: UnitAI admission participates in the native configuration identity");
        world.host->types.clear();
        world.host->types_by_name.clear();
        for (const auto& entry : types) world.host->types.emplace(entry.type_id, &entry);
        world.view->units.clear();
        for (const auto root : {10U, 20U, 30U, 40U}) {
            const bool scout = root < 30;
            const auto add = [&](unsigned id, unsigned kind) {
                ai::ViewUnit unit; unit.id = id; unit.type = kind; unit.owner = scout ? 2 : 1;
                world.view->units.push_back(unit);
            };
            add(root, scout ? 1 : 3);
            for (unsigned member = 1; member <= (scout ? 3U : 5U); ++member) add(root + member, scout ? 2 : 4);
        }
        ai::ThreatGrid grid;
        grid.partition({ai::real(-1000), ai::real(1000), ai::real(1000), ai::real(-1000)}, 5, 5, ai::Constants{});
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto verify = [&](std::int64_t frame, unsigned scout_power, std::size_t objects) {
            grid.service(*world.view, *world.host, frame);
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            expect(grid.objects().size() == objects
                && grid.total_force(*world.host, *world.view, 1, 2, true, Real{}) == ai::real(scout_power)
                && grid.total_force(*world.host, *world.view, 1, 1, true, Real{}) == ai::real(600),
                "PG-03/06: authored UnitAI admission counts craft once across worker counts");
        };
        verify(0, 840, 16);
        for (auto& unit : world.view->units) if (unit.id == 10 || unit.id == 20) unit.type = 5;
        verify(1, 1680, 18);
        for (auto& unit : world.view->units) if (unit.id == 10 || unit.id == 20) unit.type = 1;
        verify(2, 840, 16);
    }
}


struct QueryExecutor final : eawr::sim::PartitionExecutor {
    explicit QueryExecutor(std::size_t workers) : pool(workers) {}
    std::size_t worker_count() const noexcept override { return pool.worker_count(); }
    eawr::core::Result<void> execute(std::size_t partitions,
        const std::function<void(std::size_t)>& partition) const override {
        return pool.execute(partitions, partition);
    }
    eawr::core::Result<void> execute_phase(std::string_view phase, std::size_t partitions,
        const std::function<void(std::size_t)>& partition) const override {
        expect(partitions == eawr::sim::tick_partition_count, "threat preparation uses the fixed 64 partitions");
        expect(!queries_only, "shared inputs: a player query must never dispatch a worker phase");
        ++phases[std::string(phase)];
        if (fail || partial_failure) {
            if (partial_failure) partition(0);
            eawr::core::Diagnostic error;
            error.code = "EAWR-TEST";
            error.message = "query executor refused the phase";
            return eawr::core::Result<void>::failure(std::move(error));
        }
        if (reverse) {
            for (auto index = partitions; index > 0; --index) partition(index - 1);
            return eawr::core::Result<void>::success();
        }
        return pool.execute_phase(phase, partitions, partition);
    }
    eawr::platform::ThreadWorkerAdapter pool;
    mutable std::map<std::string, std::size_t> phases;
    bool fail{}, partial_failure{}, reverse{}, queries_only{};
};


void shared_view_prepass() {
    namespace t = eawr::sim::tactical;
    t::TacticalSetup start;
    start.players = {{1, 1, 100, 1}, {2, 2, 200, 1}};
    for (eawr::sim::EntityId index = 0; index < 70; ++index) {
        t::Squadron squadron;
        squadron.container = 1000 + index;
        start.units.push_back({squadron.container, 20, 1});
        for (const auto offset : {1U, 101U, 201U, 301U}) {
            const auto id = index + offset;
            squadron.members.push_back(id);
            start.units.push_back({id, 1, 1});
        }
        start.squadrons.push_back(squadron);
    }
    std::sort(start.units.begin(), start.units.end(), [](const auto& left, const auto& right) { return left.entity_id < right.entity_id; });
    for (std::size_t index = 0; index < start.units.size(); ++index) {
        start.units[index].position.x = eawr::sim::math::Fixed::from_integer(static_cast<std::int64_t>(index)).value();
        start.units[index].garrison_enabled = index % 2 != 0;
    }
    auto made = t::TacticalSession::create(start);
    expect(made.has_value(), "shared view: interleaved member identities across 70 containers create");
    if (!made) return;
    const auto original_units = made.value().units();
    expect(!made.value().unit_state(999999), "shared view: lightweight reader rejects absent identities");
    made.value().scramble_storage_for_testing();
    for (const auto& unit : original_units)
        expect(made.value().unit_state(unit.entity_id) == std::optional{unit},
            "shared view: lightweight reader equals the complete UnitState census after storage scrambling");
    const auto published = made.value().snapshot();
    std::vector<t::TacticalInstance> inputs(published->instances().begin(), published->instances().end());
    for (std::size_t index = 0; index < inputs.size(); ++index) inputs[index].visible_to = index % 3 + 1;
    const t::TacticalSnapshot snapshot(published->completed_tick(),
        std::vector<t::SnapshotPlayer>(published->players().begin(), published->players().end()), inputs, {});
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool reverse : {false, true}) {
        QueryExecutor executor(workers); executor.reverse = reverse;
        const auto view = foc::detail::build_view(made.value(), snapshot, &executor);
        expect(executor.phases["ai-world-view"] == 2 && view->units.size() == start.units.size(),
            "shared view: both map replacement and value preparation run on the fixed partitions");
        for (std::size_t index = 0; index < start.units.size(); ++index) {
            const auto& unit = view->units[index];
            expect(unit.id == start.units[index].entity_id && unit.owner == 1 && unit.team == inputs[index].team && unit.visible_to == inputs[index].visible_to,
                "shared view: ordered unit and snapshot joins preserve identity, teams and visibility");
            const auto owner = std::find_if(start.squadrons.begin(), start.squadrons.end(), [&](const auto& squadron) {
                return std::find(squadron.members.begin(), squadron.members.end(), unit.id) != squadron.members.end();
            });
            if (owner != start.squadrons.end()) expect(unit.craft && !unit.container && unit.squadron == owner->container,
                "shared view: ordered member merge joins interleaved craft to the original container");
            else {
                const auto container = std::find_if(start.squadrons.begin(), start.squadrons.end(),
                    [&](const auto& squadron) { return squadron.container == unit.id; });
                expect(container != start.squadrons.end() && unit.container && !unit.craft && unit.members == container->members,
                    "shared view: container membership keeps original member order");
            }
            expect(unit.health == ai::real(1) && unit.shield == Real{} && unit.formation_target == 0 && !unit.formation_moving,
                "shared view: missing durability and formation retain the original defaults");
            const auto& candidate = view->capture_candidates[index];
            expect(candidate.body.entity_id == unit.id && candidate.body.owner == unit.owner && candidate.type == unit.type,
                "shared view: capture inputs retain ascending unit identities");
        }
        auto sparse_inputs = inputs;
        sparse_inputs.erase(sparse_inputs.begin());
        const t::TacticalSnapshot sparse(published->completed_tick(),
            std::vector<t::SnapshotPlayer>(published->players().begin(), published->players().end()), sparse_inputs, {});
        const auto sparse_view = foc::detail::build_view(made.value(), sparse, &executor);
        expect(sparse_view->units.size() == original_units.size() && sparse_view->units.front().id == original_units.front().entity_id
            && sparse_view->units.front().visible_to == 0,
            "shared view: requested snapshot omission retains the current live unit with default snapshot fields");
        executor.fail = true;
        bool refused = false;
        try { static_cast<void>(foc::detail::build_view(made.value(), snapshot, &executor)); }
        catch (const std::runtime_error&) { refused = true; }
        expect(refused, "shared view: failed preparation cannot return a partially populated view");
    }
    t::TacticalSetup small; small.players = start.players; small.units = {{1, 1, 1}};
    auto small_world = t::TacticalSession::create(small);
    expect(small_world.has_value(), "shared view: small inline fixture creates");
    if (small_world) {
        QueryExecutor executor(8); executor.fail = true;
        const auto view = foc::detail::build_view(small_world.value(), *small_world.value().snapshot(), &executor);
        expect(view->units.size() == 1 && executor.phases.empty(), "shared view: fixed row threshold avoids the pool for a small view");
    }
}


void shared_view_formation_join() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return m::Vec3{q(x), {}, {}}; };
    t::TacticalSetup start;
    start.players = {{1, 1, 100, t::player_flag_commandable}, {2, 2, 200, t::player_flag_commandable}};
    start.units = {{10, 100, 1, at(0)}, {11, 101, 1, at(0)}, {12, 101, 1, at(10)},
        {20, 100, 1, at(100)}, {21, 101, 1, at(100)}, {22, 101, 1, at(110)},
        {30, 100, 1, at(200)}, {31, 101, 1, at(200)}, {32, 101, 1, at(210)},
        {1000, 102, 2, at(2000)}, {1001, 102, 2, at(3000)}};
    start.squadrons = {{10, {11, 12}}, {20, {21, 22}}, {30, {31, 32}}};
    // Ensure the real worker path runs even with only three formation rows.
    for (eawr::sim::EntityId id = 2000; id < 2260; ++id) start.units.push_back({id, 102, 2, at(100000)});
    t::MotionTable motion;
    t::CraftProfile craft;
    craft.type_id = 101; craft.max_speed = q(5); craft.min_speed = q(1); craft.rate_of_turn = q(6);
    craft.lift = q(6); craft.thrust = q(1); craft.roll_rate = q(6); craft.bank_angle = q(70); craft.strafe_distance = q(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{100, {101, 101}, {at(0), at(10)}, q(1000), q(200), q(300), q(20)}};
    t::CombatProfile fighter, enemy;
    fighter.type_id = 101; fighter.max_attack_distance = q(450); enemy.type_id = 102;
    t::CombatTable combat; combat.profiles = {fighter, enemy};
    t::DurabilityProfile health; health.type_id = 101; health.max_hull = q(100);
    t::DurabilityTable durability; durability.profiles = {health};
    auto made = t::TacticalSession::create(start, {}, durability, motion, std::nullopt, combat);
    expect(made.has_value(), "formation join: valid moving/attacking squadron fixture creates");
    if (!made) return;
    auto& world = made.value();
    expect(world.submit({{0, 1, 0}, {10}, t::MovePayload{at(5000)}}).has_value()
        && world.submit({{0, 1, 1}, {20}, t::AttackPayload{1000}}).has_value()
        && world.submit({{0, 1, 2}, {30}, t::AttackPayload{1001}}).has_value()
        && world.submit({{0, 1, 3}, {11}, t::DamagePayload{q(25), t::attack_hull}}).has_value(),
        "formation join: distinct real formation orders and non-default damage submit");
    const eawr::sim::InlineExecutor inline_executor;
    expect(world.step(inline_executor).has_value(), "formation join: orders commit into live squadron states");
    const auto moving = world.squadron_state(10), first_attack = world.squadron_state(20), second_attack = world.squadron_state(30);
    expect(moving && moving->mode == t::SquadronMode::move && moving->target == 0
        && first_attack && first_attack->mode == t::SquadronMode::idle && first_attack->target == 1000
        && second_attack && second_attack->mode == t::SquadronMode::idle && second_attack->target == 1001,
        "formation join: fixture establishes distinct non-default modes and targets before testing the join");
    // Deliberately overlap an input member after valid simulation setup. The old
    // first-emplace contract picks the earlier container, whose movement differs.
    auto& duplicate_owner = const_cast<t::Squadron&>(world.squadrons()[1]);
    duplicate_owner.members.push_back(11);
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool reverse : {false, true}) {
        QueryExecutor executor(workers); executor.reverse = reverse;
        const auto view = foc::detail::build_view(world, *world.snapshot(), &executor);
        expect(executor.phases["ai-world-view"] == 2, "formation join: both preparation phases use the worker executor");
        const auto check = [&](const auto& ids, eawr::sim::EntityId target, bool travelling) {
            for (const auto id : ids) {
                const auto* row = view->find(id);
                expect(row && row->formation_target == target && row->formation_moving == travelling,
                    "formation join: each container and craft retains its own non-default target and movement flag");
            }
        };
        check(std::array<eawr::sim::EntityId, 3>{10, 11, 12}, 0, true);
        check(std::array<eawr::sim::EntityId, 3>{20, 21, 22}, 1000, false);
        check(std::array<eawr::sim::EntityId, 3>{30, 31, 32}, 1001, false);
        const auto* duplicate = view->find(11);
        expect(duplicate && duplicate->squadron == 10 && duplicate->formation_target == 0 && duplicate->formation_moving,
            "formation join: duplicate member keeps the first container instead of its attacking neighbour");
        const auto* damaged = view->find(11);
        expect(view->find(10)->can_move && !view->find(11)->can_move && !view->find(1000)->can_move,
            "EX-57: admitted formation containers move independently, their craft and stationary types do not");
        expect(damaged && damaged->hull == q(75) && damaged->max_hull == q(100)
            && damaged->health == ai::real(3) / ai::real(4),
            "formation join: non-default durability row survives the independent snapshot identity join");
    }
    duplicate_owner.members.pop_back();
    expect(world.stage_remove(2259).has_value(), "formation join: stage removal republishes current live identities");
    t::UnitState spawned; spawned.type_id = 102; spawned.owner = 2; spawned.position = at(4000);
    const auto added = world.stage_spawn(spawned);
    expect(added.has_value(), "formation join: stage spawn republishes current live identities");
    if (added) for (const auto workers : {1U, 2U, 4U, 8U}) {
        QueryExecutor executor(workers); executor.reverse = true;
        const auto view = foc::detail::build_view(world, *world.snapshot(), &executor);
        const auto census = world.units();
        expect(view->units.size() == census.size() && !view->find(2259) && view->find(added.value()),
            "formation join: view immediately sees staged removal and spawn without dropping a live unit");
        for (std::size_t index = 0; index < census.size(); ++index)
            expect(view->units[index].id == census[index].entity_id,
                "formation join: current publication identity domain matches the exact live census after staging");
    }
}


void shared_input_prepasses() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool reverse : {false, true}) {
        QueryExecutor executor(workers); executor.reverse = reverse;
        t::TacticalSetup start;
        start.players = {{1, 1, 100, 1}, {2, 1, 200, 1}, {99, 99, 300, 0}};
        start.units = {{1, 10, 2}};
        for (eawr::sim::EntityId id = 1000; id < 1260; ++id) start.units.push_back({id, 20, 2});
        t::EconomyRules rules;
        rules.players = {{1, q(1000), 20, true}, {2, q(1000), 20, true}, {99, {}, 20}};
        rules.menus = {{10, 200, {{50, t::BuildKind::unit, t::BuildQueue::units, q(100), 30, 30, 0, true}}},
            {20, 100, {{30, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}}, 0, false},
            {20, 200, {{30, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}}, 0, false}};
        rules.pads.neutral = 99;
        rules.pads.capture = {{20, q(100), q(1), {100, 200}, false, true, true}};
        rules.pads.construction = {{30, 40, q(100), 1, 1}};
        rules.pads.influence = {{10, false}, {20, false}, {30, false}, {40, false}};
        auto world = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
        expect(world.has_value(), "shared inputs: large allied producer/pad fixture creates");
        if (!world) continue;
        auto host = ai::make_tactical_host(); host->world = &world.value(); host->snapshot = world.value().snapshot();
        host->view = foc::detail::build_view(world.value(), *host->snapshot, &executor);
        for (const auto& [id, name] : {std::pair{10U, "BASE"}, {20U, "PAD"}, {30U, "UC"}, {40U, "MINE"}}) {
            foc::AiType type; type.type_id = id; type.name = name; type.star_base = id == 10;
            host->setup.content.types.push_back(type);
        }
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type); host->types_by_name.emplace(type.name, &type);
        }
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto parsed = ai::EquationSet::parse({{"inputs.xml", "<Root><Open>Variable_Self.OpenBuildPadCount</Open>"
            "<Built>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"UC\", Parameter_Type=\"UC\", Parameter_Type=\"MINE\"}</Built></Root>"}}, converters);
        expect(parsed.has_value(), "shared inputs: pad equations parse");
        if (!parsed) continue;
        ai::AiData data; data.equations = std::move(parsed).value();
        ai::Engine engine(host, std::move(data), {});
        ai::EngineContractAccess::perception_executor(engine, &executor);
        const auto uncached = engine.evaluate("Open", 1, nullptr);
        expect(uncached == ai::real(260), "shared inputs: all allied pads are initially open");
        ai::EngineContractAccess::request_open(engine, 1);
        ai::EngineContractAccess::prepare_inputs(engine, executor);
        expect(ai::EngineContractAccess::has_prepared_open(engine, 1) && !ai::EngineContractAccess::has_prepared_open(engine, 2),
            "shared inputs: only preceding actual open-query demand prepares broad-phase admission");
        expect(executor.phases["ai-producers"] == 1 && executor.phases["ai-pad-perception"] == 1,
            "shared inputs: each large census dispatches once before queries");
        std::vector<eawr::sim::EntityId> expected;
        for (eawr::sim::EntityId id = 1000; id < 1260; ++id) expected.push_back(id);
        expect(ai::EngineContractAccess::producers(engine, 1, 30) == expected
            && ai::EngineContractAccess::producers(engine, 1, 50) == std::vector<eawr::sim::EntityId>{1},
            "shared inputs: ordered merge preserves pad candidates and the distinct lower-ID station menu");
        const auto phases = executor.phases;
        executor.queries_only = true;
        expect(engine.evaluate("Open", 1, nullptr) == uncached && engine.producer(1, 30) == 1000,
            "shared inputs: prepared queries retain initial answers");
        ai::EngineContractAccess::reserve_pad(engine, 1000, 2, 30);
        expect(engine.evaluate("Open", 1, nullptr) == ai::real(259)
            && engine.evaluate("Built", 1, nullptr) == ai::real(3) && engine.producer(1, 30) == 1001,
            "shared inputs: later teammate reservation immediately changes open, typed aliases and producer selection");
        ai::EngineContractAccess::release_pad(engine, 1000);
        expect(engine.evaluate("Open", 1, nullptr) == uncached && engine.evaluate("Built", 1, nullptr) == Real{}
            && engine.producer(1, 30) == 1000 && executor.phases == phases,
            "shared inputs: release restores answers without any query-time worker phase");
        executor.queries_only = false;
        ai::EngineContractAccess::prepare_pads(engine, executor);
        expect(!ai::EngineContractAccess::has_prepared_open(engine, 1),
            "shared inputs: open-query demand expires after an unused service");
        executor.queries_only = true;
        expect(engine.evaluate("Open", 2, nullptr) == uncached,
            "shared inputs: a cold player uses the identical inline admission path without dispatch");
        ai::EngineContractAccess::reset_pads(engine);
        expect(world.value().stage_remove(1000).has_value(), "shared inputs: remove pad outside the service barrier");
        expect(engine.evaluate("Open", 1, nullptr) == ai::real(259),
            "shared inputs: unscoped pad read sees removal instead of stale prepared facts");
        executor.queries_only = false; executor.fail = true;
        bool refused = false;
        try { ai::EngineContractAccess::prepare_pads(engine, executor); } catch (const std::runtime_error&) { refused = true; }
        expect(refused && !ai::EngineContractAccess::has_prepared_pads(engine),
            "shared inputs: failed worker phase never publishes partial pad facts");
    }
}


void threat_preparation() {
    World world;
    auto& types = world.host->setup.content.types;
    types.front().combat_power = ai::real(1);
    auto large = types.front();
    large.type_id = 2;
    large.combat_power = ai::real(16777216);
    types.push_back(large);
    world.host->types.clear();
    for (const auto& type : types) world.host->types.emplace(type.type_id, &type);
    world.view->units.clear();
    for (const auto& [id, type, owner] : std::array<std::array<unsigned, 3>, 3>{{{1, 2, 2}, {2, 1, 1}, {3, 1, 1}}}) {
        ai::ViewUnit unit;
        unit.id = id; unit.type = type; unit.owner = owner;
        world.view->units.push_back(unit);
    }
    ai::ThreatGrid grid;
    const auto configure = [&] {
        grid.partition(foc::AiBounds{ai::real(-1000), ai::real(1000), ai::real(1000), ai::real(-1000)}, 5, 5, ai::Constants{});
        grid.service(*world.view, *world.host, 0);
    };
    configure();
    const auto all = ~std::uint64_t{};
    const auto original = grid.total_force(*world.host, *world.view, all, 0, true, Real{});
    expect(original.repr == ai::real(16777218).repr, "PG-06: player-first rounding fixture retains the two small contributions");
    const auto entity_fold = ai::to_single(ai::to_single(ai::real(16777216) + ai::real(1)) + ai::real(1));
    expect(original.repr != entity_fold.repr, "PG-06: fixture detects an entity-order or partition-subtotal fold");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        QueryExecutor executor(workers);
        {
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == original.repr,
                "PG-06: staged contributions retain player-first rounding");
            expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == original.repr,
                "PG-06: repeated query returns identical bits");
            expect(executor.phases.empty(), "PG-06: cold and repeated queries never dispatch a worker phase");
            expect(grid.query_counts().cold == 2 && grid.query_counts().repeats == 1
                && grid.query_counts().scalar_hits == 1, "PG-06: cold, repeated and scalar-hit demand is counted");
        }
        expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == original.repr,
            "PG-06: unscoped readers retain the const query path");
        expect(executor.phases.empty(), "PG-06: unscoped reads do not use the borrowed executor");
        {
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            grid.prepare_contributions();
            expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == original.repr
                && grid.query_counts().warm == 1 && executor.phases.empty(),
                "PG-06: small prepass stays inline and retains adversarial player-first rounding");
        }
    }

    types[1].weapons = {{0, ai::real(127), ai::real(431), true, true},
        {1, ai::real(251) / ai::real(7), ai::real(719), true, true}};
    auto other_category = types.front();
    other_category.type_id = 3;
    other_category.category_bits = 1;
    other_category.combat_power = ai::real(73) / ai::real(11);
    types.push_back(other_category);
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const auto& type : types) world.host->types.emplace(type.type_id, &type);
    world.host->setup.players.push_back(foc::AiPlayer{3, "NEUTRAL", true, false, ""});
    world.host->setup.players.push_back(foc::AiPlayer{4, "ALLIED", false, true, ""});
    world.host->players.clear();
    for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
    world.view->players.push_back(foc::detail::ViewPlayer{3, 3, 2});
    world.view->players.push_back(foc::detail::ViewPlayer{4, 1, 3});
    world.view->units.clear();
    for (unsigned index = 0; index < 83; ++index) {
        ai::ViewUnit unit;
        unit.id = index + 1; unit.type = index % 3 + 1; unit.owner = index % 11 == 0 ? 9 : index % 5;
        unit.health = ai::real(1) / ai::real(index % 7 + 1);
        unit.visible_to = index % 16;
        unit.hardpoint_destroyed = {false, index % 2 == 0};
        unit.position.x = eawr::sim::math::Fixed::from_integer(static_cast<int>(index % 5) * 310 - 620).value();
        unit.position.y = eawr::sim::math::Fixed::from_integer(static_cast<int>(index % 7) * 210 - 630).value();
        world.view->units.push_back(unit);
    }
    // A non-AI player uses the authored visibility mask instead of omniscience.
    world.host->setup.players.front().ai = false;
    configure();
    const std::array<ai::Rect, 4> rectangles{{
        {ai::real(-1000), ai::real(-1000), ai::real(2000), ai::real(2000)},
        {ai::real(-413), ai::real(-217), ai::real(713), ai::real(507)},
        {ai::real(3000), ai::real(3000), ai::real(1), ai::real(1)},
        {ai::real(-1001), ai::real(99), ai::real(2), ai::real(803)}}};
    struct Query {
        std::uint64_t category;
        eawr::sim::tactical::PlayerId player;
        bool friendly;
        Real attenuator;
        eawr::sim::EntityId excluded;
    };
    const std::array<Query, 10> queries{{
        {all, 0, true, Real{}, 0}, {2, 1, false, ai::real(1) / ai::real(3), 0},
        {2, 2, true, ai::real(1) / ai::real(7), 13}, {1, 1, true, ai::real(1), 0},
        {all, 2, false, ai::real(1), 17}, {all, 9, false, ai::real(1) / ai::real(2), 0},
        {all, 1, false, Real{}, 0}, {all, 1, false, Real::from_repr(0x8000000000000000ULL), 0},
        {all, 3, false, ai::real(1) / ai::real(2), 0}, {2, 4, true, ai::real(1) / ai::real(5), 7}}};
    std::vector<std::uint64_t> expected;
    const auto read = [&](const Query& query, const auto& append) {
        append(grid.total_force(*world.host, *world.view, query.category, query.player, query.friendly, query.attenuator).repr);
        for (const auto& rect : rectangles) {
            append(grid.force(*world.host, *world.view, rect, query.category, query.player, query.friendly, query.attenuator, query.excluded).repr);
        }
    };
    for (const auto& query : queries) read(query, [&](auto value) { expected.push_back(value); });
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        QueryExecutor executor(workers);
        {
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            grid.prepare_contributions();
            const auto prepass_phases = executor.phases;
            for (int repeat = 0; repeat < 2; ++repeat) {
                std::vector<std::uint64_t> actual;
                for (const auto& query : queries) read(query, [&](auto value) { actual.push_back(value); });
                expect(actual == expected, "PG-05/06: cell order, predicates, fractional health and query keys agree across workers");
            }
            expect(executor.phases == prepass_phases, "PG-05/06: all queries stay inline after the prepass");
            expect(executor.phases.size() <= 1 && executor.phases["ai-threat-cells"] == 0,
                "PG-05/06: only one contribution phase can run per prepass");
            expect(grid.query_counts().queries == queries.size() * 10
                && grid.query_counts().repeats == queries.size() * 10 - 8
                && grid.query_counts().cross_player > 0,
                "PG-05/06: repeated keys across query kinds and players are counted");
            if (workers != 1) expect(grid.query_counts().warm == grid.query_counts().queries
                && grid.query_counts().prepared_keys == 8,
                "PG-05/06: prior demand prepares all keys including distinct signed zero");
        }
        std::array<std::vector<std::uint64_t>, eawr::sim::tick_partition_count> concurrent;
        expect(executor.pool.execute(concurrent.size(), [&](const std::size_t partition) {
            for (const auto& query : queries) read(query, [&](auto value) { concurrent[partition].push_back(value); });
        }).has_value(), "PG-05/06: parallel unscoped readers complete");
        for (const auto& values : concurrent) expect(values == expected, "PG-05/06: Lua-style const readers preserve query results");
    }
    QueryExecutor executor(4);
    {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        const auto first = grid.total_force(*world.host, *world.view, all, 0, true, Real{});
        world.view->units.erase(world.view->units.begin() + 1);
        grid.service(*world.view, *world.host, 300);
        const auto changed = grid.total_force(*world.host, *world.view, all, 0, true, Real{});
        expect(changed.repr != first.repr && executor.phases.empty(),
            "PG-03: service invalidates cached totals after a death without dispatch");
        configure();
        expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == changed.repr,
            "PG-03: repartition invalidates the query cache");
        expect(executor.phases.empty(), "PG-03: repartition recomputes inline");
    }
    // Seed enough distinct demand for the fixed work-count partition threshold.
    {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        for (int denominator = 1; denominator <= 12; ++denominator)
            static_cast<void>(grid.total_force(*world.host, *world.view, all, 0, true,
                ai::real(1) / ai::real(denominator)));
    }
    executor.fail = true;
    bool refused = false;
    try {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        grid.prepare_contributions();
    } catch (const std::runtime_error&) {
        refused = true;
    }
    expect(refused, "PG-06: prepass executor failure cannot fabricate a threat result");
    expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr != 0,
        "PG-06: failure unwinds preparation before later const readers");
    executor.fail = false;
    executor.phases.clear();
    {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        grid.prepare_contributions();
        expect(grid.query_counts().prepared_keys == 0,
            "PG-06: demand unused for one tick expires instead of accumulating");
        static_cast<void>(grid.total_force(*world.host, *world.view, all, 0, true, ai::real(1)));
    }
    // Change only health between ticks: a warm key must use the CURRENT completed view.
    const auto previous = grid.total_force(*world.host, *world.view, all, 0, true, ai::real(1));
    // Use a large contribution so the final single-precision fold cannot hide the change.
    world.view->units[3].health = ai::real(1) / ai::real(7);
    const auto current = grid.total_force(*world.host, *world.view, all, 0, true, ai::real(1));
    expect(current.repr != previous.repr, "PG-06: current-view fixture detects stale health");
    {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        grid.prepare_contributions();
        expect(grid.total_force(*world.host, *world.view, all, 0, true, ai::real(1)).repr == current.repr
            && grid.query_counts().warm == 1, "PG-06: warm contributions read the current completed view");
    }
}


void threat_flat_storage() {
    World world;
    auto& types = world.host->setup.content.types;
    types.front().weapons = {{0, ai::real(127), ai::real(431), true, true},
        {1, ai::real(251) / ai::real(7), ai::real(719), true, true}};
    auto large = types.front(); large.type_id = 2; large.name = "LARGE"; large.combat_power = ai::real(16777216);
    large.weapons.clear(); types.push_back(large);
    auto other = large; other.type_id = 3; other.name = "OTHER"; other.category_bits = 1;
    other.combat_power = ai::real(73) / ai::real(11); types.push_back(other);
    auto zero_power = types.front(); zero_power.type_id = 4; zero_power.name = "ZERO_POWER";
    zero_power.category_bits = 8; zero_power.combat_power = Real{}; types.push_back(zero_power);
    auto zero_health = types.front(); zero_health.type_id = 5; zero_health.name = "ZERO_HEALTH";
    zero_health.category_bits = 16; types.push_back(zero_health);
    world.host->setup.content.categories.emplace("ZERO_POWER", 8);
    world.host->setup.content.categories.emplace("ZERO_HEALTH", 16);
    world.host->types.clear(); world.host->types_by_name.clear();
    for (const auto& type : types) {
        world.host->types.emplace(type.type_id, &type); world.host->types_by_name.emplace(type.name, &type);
    }
    world.host->setup.players.front().ai = false;
    world.host->setup.players.push_back(foc::AiPlayer{3, "NEUTRAL", true, false, ""});
    world.host->setup.players.push_back(foc::AiPlayer{4, "ALLIED", false, true, ""});
    world.host->players.clear();
    for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
    world.view->players.push_back(foc::detail::ViewPlayer{3, 3, 2});
    world.view->players.push_back(foc::detail::ViewPlayer{4, 1, 3});
    // Repeated owner traversal must repeat its individual rounded additions.
    world.view->players.push_back(world.view->players.front());
    constexpr eawr::sim::EntityId first = std::uint64_t{1} << 48;
    constexpr eawr::sim::EntityId stride = 1000003;
    const auto q = [](int value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    world.view->units.clear();
    for (unsigned index = 0; index < 96; ++index) {
        ai::ViewUnit unit;
        unit.id = first + index * stride; unit.type = index % 3 + 1;
        unit.owner = index % 11 == 0 ? 9 : index % 5;
        unit.health = ai::real(1) / ai::real(index % 7 + 1);
        unit.visible_to = index % 16;
        unit.hardpoint_destroyed = {false, index % 2 == 0};
        unit.position = {q(static_cast<int>(index % 5) * 310 - 620),
            q(static_cast<int>(index % 7) * 210 - 630), {}};
        world.view->units.push_back(unit);
    }
    world.view->units[1].type = 5; world.view->units[1].health = Real{};
    world.view->units[2].type = 4; world.view->units[2].health = ai::real(1); world.view->units[2].owner = 2;
    const foc::AiBounds bounds{ai::real(-1000), ai::real(1000), ai::real(1000), ai::real(-1000)};
    const auto all = ~std::uint64_t{};
    const auto negative_zero = Real::from_repr(0x8000000000000000ULL);
    const std::array<ai::Rect, 6> rectangles{{
        {ai::real(-1000), ai::real(-1000), ai::real(2000), ai::real(2000)},
        {ai::real(-600), ai::real(-600), ai::real(400), ai::real(400)},
        {ai::real(-1001), ai::real(99), ai::real(2), ai::real(803)},
        {ai::real(3000), ai::real(3000), ai::real(1), ai::real(1)},
        {Real{}, Real{}, ai::real(400), ai::real(400)},
        {negative_zero, Real{}, ai::real(400), ai::real(400)}}};
    const auto read = [&](const ai::ThreatGrid& subject) {
        std::vector<std::uint64_t> result;
        for (const auto category : {all, std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{8}, std::uint64_t{16}})
            for (const auto attenuation : {Real{}, negative_zero, ai::real(1) / ai::real(3), ai::real(1)})
                for (const auto player : {0U, 1U, 2U, 3U, 4U, 9U}) for (const bool friendly : {false, true}) {
                    result.push_back(subject.total_force(*world.host, *world.view, category, player, friendly, attenuation).repr);
                    for (const auto& rect : rectangles) for (const auto excluded : {eawr::sim::EntityId{}, first + 13 * stride})
                        result.push_back(subject.force(*world.host, *world.view, rect, category, player, friendly, attenuation, excluded).repr);
                }
        return result;
    };
    ai::ThreatGrid grid;
    grid.partition(bounds, 5, 5, ai::Constants{}); grid.service(*world.view, *world.host, 0);
    expect(grid.total_force(*world.host, *world.view, 8, 0, true, ai::real(1)).repr == Real{}.repr
        && grid.total_force(*world.host, *world.view, 16, 0, true, ai::real(1)).repr == Real{}.repr,
        "PG-06: admitted zero-power and zero-health categories have zero totals");
    expect(grid.force(*world.host, *world.view, rectangles.front(), 8, 2, true, Real{}, 0) > Real{},
        "PG-05/06: a zero company total still admits nonzero weapon-entry threat");
    const auto verify = [&](ai::ThreatGrid& subject, QueryExecutor& executor) {
        const auto expected = read(subject); // Original const, uncached formula is the oracle.
        {
            const auto preparation = subject.prepare(&executor, *world.host, *world.view);
            expect(read(subject) == expected, "PG-05/06: sparse-ID cold queries equal the original formula");
        }
        {
            const auto preparation = subject.prepare(&executor, *world.host, *world.view);
            static_cast<void>(subject.total_force(*world.host, *world.view, all, 0, true, Real{}));
            subject.prepare_contributions();
            const auto phases = executor.phases;
            executor.queries_only = true;
            expect(read(subject) == expected && subject.query_counts().scalar_hits == 0,
                "PG-05/06: flat rows preserve every oracle bit and clear pre-prepass scalars");
            expect(read(subject) == expected, "PG-05/06: scalar hits retain signed-zero keys and all query events");
            expect(subject.query_counts().queries == 1 + expected.size() * 2
                && subject.query_counts().warm == expected.size() * 2
                && subject.query_counts().cold == 1 && subject.query_counts().scalar_hits == expected.size()
                && subject.query_counts().prepared_keys == 20 && executor.phases == phases,
                "PG-05/06: warm, cold, scalar-hit and full-key demand contracts stay exact without query dispatch");
            executor.queries_only = false;
        }
        expect(read(subject) == expected, "PG-05/06: scope exit expires prepared pointers before const readers");
    };
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        QueryExecutor executor(workers); executor.reverse = workers == 8;
        verify(grid, executor);
        expect(executor.phases["ai-threat-total"] == 1, "PG-05/06: flat arithmetic uses the existing fixed partition phase");
    }
    world.view->units[0].position.x = q(700);
    world.view->units[1].owner = 4;
    world.view->units[2].health = ai::real(1) / ai::real(7);
    world.view->units[3].hardpoint_destroyed[1] = true;
    world.view->units[4].type = 3;
    world.view->units[5].visible_to = 0;
    world.view->units.erase(world.view->units.begin() + 6);
    auto born = world.view->units.back(); born.id += stride; world.view->units.push_back(born);
    grid.service(*world.view, *world.host, 300);
    QueryExecutor executor(4); executor.reverse = true;
    verify(grid, executor);
    grid.partition(bounds, 7, 3, ai::Constants{}); grid.service(*world.view, *world.host, 600);
    verify(grid, executor);
    auto copied = grid;
    grid.partition(bounds, 2, 2, ai::Constants{});
    verify(copied, executor);
    const auto expected = copied.total_force(*world.host, *world.view, all, 0, true, Real{});
    const auto retry_expected = read(copied);
    executor.partial_failure = true;
    {
        const auto preparation = copied.prepare(&executor, *world.host, *world.view);
        bool refused = false;
        try { copied.prepare_contributions(); } catch (const std::runtime_error&) { refused = true; }
        expect(refused && copied.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == expected.repr
            && copied.query_counts().warm == 0 && copied.query_counts().cold == 1,
            "PG-05/06: a partially executed prepass publishes no rows even when failure is caught inside preparation");
        executor.partial_failure = false;
        copied.prepare_contributions();
        const auto phases = executor.phases;
        executor.queries_only = true;
        expect(read(copied) == retry_expected && copied.query_counts().warm == retry_expected.size()
            && executor.phases == phases,
            "PG-05/06: retry in the same preparation publishes only complete fresh rows");
        executor.queries_only = false;
    }
    executor.partial_failure = false;
    ai::Engine engine(world.host, {}, {});
    auto& native_grid = ai::EngineContractAccess::grid(engine);
    native_grid = copied;
    verify(native_grid, executor); // Leave demand recorded at the save boundary.
    auto scripts = auth::ScriptScheduler::create({}, {});
    expect(scripts.has_value(), "PG-05/06: threat restore fixture creates its scheduler");
    if (!scripts) return;
    const std::string identity(64, 'a');
    const auto saved = engine.save_tactical_native(identity, scripts.value());
    expect(saved.has_value(), "PG-05/06: a demanded threat grid saves its authoritative state");
    if (!saved) return;
    const auto saved_units = world.view->units;
    for (const bool same_size : {false, true}) {
        world.view->units = saved_units;
        if (same_size) {
            for (auto& unit : world.view->units) unit.id += stride * 1000;
        } else {
            world.view->units.resize(7);
        }
        native_grid.partition(bounds, 2, 2, ai::Constants{});
        native_grid.service(*world.view, *world.host, 900);
        verify(native_grid, executor); // Build a stale layout with different ids/size/geometry.
        world.view->units = saved_units;
        expect(engine.restore_tactical_native(saved.value(), identity, scripts.value()).has_value(),
            "PG-05/06: decode into a non-fresh engine invalidates stale slot ids, sizes and cell references");
        verify(native_grid, executor);
        expect(engine.save_tactical_native(identity, scripts.value()).value() == saved.value(),
            "PG-05/06: flat rows, topology references and demand hints never enter native bytes");
    }
}


// EX-46: a squadron member is its team container, which has no weapon state. Target_In_Range fires for
// it when the team leader holds the formation's enemy target within its weapon reach; a ship member
// still fires on its own attack target.
void squadron_target_in_range() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    World world;
    const auto at = [](std::int64_t x) { return math::Vec3{math::Fixed::from_integer(x).value(), {}, {}}; };
    const auto unit = [&](sim::EntityId id, tactical::PlayerId owner, std::int64_t x) {
        ai::ViewUnit entry;
        entry.id = id;
        entry.type = 1;
        entry.owner = owner;
        entry.position = at(x);
        return entry;
    };
    // IDs ascend: container 10 (leader 11, second craft 12), ship 20, enemy 30.
    auto container = unit(10, 1, 0);
    container.container = true;
    container.members = {11, 12};
    container.formation_target = 30;
    auto leader = unit(11, 1, 0);
    leader.craft = true;
    leader.squadron = 10;
    auto second = leader;
    second.id = 12;
    auto ship = unit(20, 1, 0);
    ship.attack_target = 30;
    auto enemy = unit(30, 2, 800);
    const auto publish = [&](std::vector<ai::ViewUnit> units) {
        auto next = std::make_shared<ai::WorldView>(*world.view);
        next->units = std::move(units);
        world.view = next;
        world.host->view = next;
    };
    auth::ModuleManifest manifest;
    expect(manifest.add("RANGE.LUA", "function Default_Target_In_Range(tf, unit, target) end\n").has_value(),
        "EX-46: handler fixture loads");
    auth::SessionConfig config;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), std::move(manifest));
    expect(scripts.has_value() && scripts.value().create_instance(1, "RANGE.LUA").has_value(), "EX-46: plan instance loads");
    if (!scripts) return;
    ai::Engine engine(world.host, {}, {ai::PlanDef{}});
    ai::EngineContractAccess::event_force(engine, scripts.value(), {10, 20});
    const auto fired = [&](std::vector<ai::ViewUnit> units) {
        publish(std::move(units));
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
        for (const auto& event : ai::EngineContractAccess::run_events(engine)) {
            if (event.name != "Default_Target_In_Range" || event.arguments.size() != 3) continue;
            pairs.emplace_back(std::get<auth::Handle>(event.arguments[1].data).id, std::get<auth::Handle>(event.arguments[2].data).id);
        }
        return pairs;
    };
    using Pairs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    auto leader_holding = leader;
    leader_holding.attack_target = 30;
    auto second_holding = second;
    second_holding.attack_target = 30;
    // 800 apart, beyond the 500 reach: the leader holds the target but it is not in range yet.
    expect(fired({container, leader_holding, second_holding, ship, enemy}) == Pairs{{20, 30}},
        "EX-46: an out-of-range squadron stays silent while a ship member fires on its own target");
    enemy.position = at(400);
    expect(fired({container, leader_holding, second_holding, ship, enemy}) == Pairs{{10, 30}},
        "EX-46: the container fires once the leader holds the formation target within reach");
    expect(fired({container, leader_holding, second_holding, ship, enemy}).empty(), "EX-46: the same target does not repeat");
    expect(fired({container, leader, second_holding, ship, enemy}).empty(),
        "EX-46: a leader without the target does not fire, whatever the other craft hold");
    expect(fired({container, leader_holding, second_holding, ship, enemy}) == Pairs{{10, 30}},
        "EX-46: reacquiring the target fires again");
    auto friendly = enemy;
    friendly.owner = 1;
    expect(fired({container, leader_holding, second_holding, ship, friendly}).empty(),
        "EX-46: a friendly formation target is not a target in range");
    enemy.position = at(2000);
    expect(fired({container, leader_holding, second_holding, ship, enemy}).empty(), "EX-46: leaving the reach ends the range");
    enemy.position = at(400);
    expect(fired({container, second_holding, ship, enemy}) == Pairs{{10, 30}},
        "EX-46: the next live craft leads when the first dies");
}


// FH-23: the Lua freestore must see formation state even though the container has
// neither a ship locomotor nor weapon state. Exercise the real snapshot and bindings.
// WLD-10: the engine's stand-in for the Crusader's point-defence object script. It reads the deadly enemy's
// projectile types, switches LASER_DEFENSE on once a second per object, and only for an AI-owned,
// ready, inactive Crusader whose deadly enemy fires an affected projectile.
void point_defence_stand_in() {
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{fixed(x), {}, {}}; };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    // Unit 1: the AI's Crusader. Unit 2 fires a missile; unit 3 fires a laser-class shot only.
    setup.units = {{1, 100, 2, at(0), math::identity_quat(), {}}, {2, 101, 1, at(900), math::identity_quat(), {}},
        {3, 102, 1, at(-900), math::identity_quat(), {}}};
    tactical::AbilityProfile laser;
    laser.kind = tactical::AbilityKind::laser_defense;
    laser.expiration_frames = 1050;
    laser.recharge_frames = 1800;
    laser.recharge_at_activation = true;
    laser.gui_activated_ability_name = "Point_Defence";
    tactical::SpecialAbilityProfile handler;
    handler.name = "Point_Defence";
    handler.kind = tactical::SpecialAbilityKind::laser_defense;
    handler.style = tactical::SpecialActivationStyle::user_input;
    handler.service_interval = 1;
    handler.defence_duration_frames = 1050;
    handler.defence_look_frames = 15;
    handler.defence_radius = fixed(500);
    handler.defence_min_travel = fixed(100);
    handler.defence_categories = tactical::defence_category_missile;
    tactical::UnitAbilityProfile profile;
    profile.type_id = 100;
    profile.abilities = {laser};
    profile.special = {handler};
    tactical::AbilityTable table;
    table.profiles = {profile};
    tactical::CombatTable combat;
    tactical::CombatProfile crusader, missile_ship, laser_ship;
    crusader.type_id = 100;
    missile_ship.type_id = 101;
    laser_ship.type_id = 102;
    combat.profiles = {crusader, missile_ship, laser_ship};
    for (const bool owned_by_ai : {true, false}) {
        auto created = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, combat, {}, table);
        expect(created.has_value(), "WLD-10: point defence world starts");
        if (!created) return;
        auto world = std::move(created).value();
        auto host = ai::make_tactical_host();
        foc::AiType shooter, lasers, defender;
        defender.type_id = 100; defender.locomotor = true; defender.point_defence_script = true;
        shooter.type_id = 101; shooter.locomotor = true; shooter.hardpoint_projectiles = {0, 300};
        lasers.type_id = 102; lasers.locomotor = true; lasers.hardpoint_projectiles = {301};
        host->setup.content.types = {defender, shooter, lasers};
        host->types.emplace(100, &host->setup.content.types[0]);
        host->types.emplace(101, &host->setup.content.types[1]);
        host->types.emplace(102, &host->setup.content.types[2]);
        host->setup.content.projectile_categories.emplace(300, foc::ai_projectile_missile);
        host->view = foc::detail::build_view(world, *world.snapshot());
        ai::Engine engine(host, {}, {});
        if (owned_by_ai) ai::EngineContractAccess::collect_fixture(engine);
        const auto count = [&](const std::int64_t frame) {
            ai::EngineContractAccess::point_defence(engine, frame);
            return ai::EngineContractAccess::point_defence_orders(engine);
        };
        expect(count(1) == 0, "WLD-10: with no threat nobody switches laser defence on");
        ai::EngineContractAccess::threat(engine, 1, 3, 5);
        expect(count(1) == 0, "WLD-10: a deadly enemy that fires only lasers does not switch it on");
        ai::EngineContractAccess::threat(engine, 1, 2, 9);
        expect(count(2) == 0, "WLD-10: the object's phase in the second decides when it looks");
        const auto issued = count(1);
        if (!owned_by_ai) {
            expect(issued == 0, "WLD-10: a player without an AI never uses the stand-in");
            continue;
        }
        expect(issued == 1, "WLD-10: a deadly enemy with a missile switches laser defence on");
        const auto command = ai::EngineContractAccess::last_order(engine);
        expect(command && command->verb == foc::verb_ability && command->arguments.size() == 4
            && std::get<auth::Handle>(command->arguments[1].data).id == 1
            && std::get<Real>(command->arguments[2].data) == Real(static_cast<int>(tactical::AbilityKind::laser_defense))
            && std::get<Real>(command->arguments[3].data) == Real(static_cast<int>(tactical::AbilityAction::activate)),
            "WLD-10: the request is the ordinary ability activation of the Crusader");
        // The command has to survive the translation into a world order as well: an ability kind beyond the
        // verb's bound would be dropped as malformed, and the stand-in's decision would never reach the world.
        const auto translated = foc::tactical_ai_detail::translate_order(*command);
        const auto* translated_payload = translated ? std::get_if<tactical::AbilityPayload>(&translated.value().payload) : nullptr;
        expect(translated_payload != nullptr && translated_payload->ability == tactical::AbilityKind::laser_defense
            && translated_payload->action == tactical::AbilityAction::activate
            && translated.value().units == std::vector<eawr::sim::EntityId>{1} && translated.value().issuer == 2,
            "WLD-10: the stand-in's request translates to LASER_DEFENSE activate for the Crusader");
        // Once the world has it on, the stand-in leaves it alone.
        tactical::PlayerCommand activate{{world.completed_tick(), 2, 0}, {1},
            tactical::AbilityPayload{tactical::AbilityKind::laser_defense, tactical::AbilityAction::activate}};
        expect(world.submit(activate).has_value(), "WLD-10: the world accepts the activation");
        const eawr::platform::ThreadWorkerAdapter executor(1);
        expect(world.step(executor).has_value(), "WLD-10: the world steps");
        host->view = foc::detail::build_view(world, *world.snapshot());
        expect(count(31) == issued, "WLD-10: an active laser defence is not requested again");
    }
}


void corrupt_systems_stand_in() {
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    for (const bool human : {false, true}) {
        auto host = ai::make_tactical_host();
        host->setup.players = {{1, "REBEL", false, false, ""}, {2, "UNDERWORLD", false, !human, ""},
            {3, "NEUTRAL", true, false, ""}};
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        foc::AiType hero, enemy_type;
        hero.type_id = 100; hero.corrupt_systems_script = true;
        enemy_type.type_id = 101; enemy_type.combat_power = Real(1000);
        host->setup.content.types = {hero, enemy_type};
        host->types.emplace(100, &host->setup.content.types[0]);
        host->types.emplace(101, &host->setup.content.types[1]);
        auto view = std::make_shared<foc::detail::WorldView>();
        view->players = {{1, 1, 0}, {2, 2, 1}, {3, 3, 2}};
        foc::detail::ViewUnit source, enemy;
        source.id = 1; source.type = 100; source.owner = 2; source.team = 2;
        tactical::AbilityStatus status;
        status.kind = tactical::AbilityKind::corrupt_systems; status.ready = true;
        source.abilities = {status};
        enemy.id = 2; enemy.type = 101; enemy.owner = 1; enemy.team = 1;
        enemy.position.x = fixed(200); enemy.shield = Real(1);
        view->units = {source, enemy};
        auto neutral = enemy; neutral.id = 99; neutral.owner = neutral.team = 3;
        view->units.push_back(neutral);
        host->view = view;
        ai::Engine engine(host, {}, {});
        if (!human) ai::EngineContractAccess::collect_fixture(engine);
        const auto service = [&](std::int64_t frame) {
            std::sort(view->units.begin(), view->units.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
            ai::EngineContractAccess::corrupt_systems(engine, frame);
            return ai::EngineContractAccess::point_defence_orders(engine);
        };
        expect(service(1) == 0, "WHE-67: threat equal to 1000 does not cast");
        enemy.id = 3; enemy.position.x = fixed(300);
        view->units.push_back(enemy);
        expect(service(2) == 0, "WHE-67: service waits for the object's one-second phase");
        expect(service(1) == (human ? 0U : 1U), "WHE-67: only the AI casts above the threshold");
        if (human) continue;
        const auto* command = ai::EngineContractAccess::last_order(engine);
        expect(command && command->arguments.size() == 7
            && std::get<Real>(command->arguments[4].data) == Real(250),
            "WHE-68: a small cluster casts at its planar mean");
        if (command) expect(foc::tactical_ai_detail::translate_order(*command).has_value(),
            "WHE-67: the native point activation reaches the ordinary command path");
        view->units[1].shield = view->units[2].shield = Real{};
        expect(service(31) == 1, "WHE-67: unshielded enemies are ignored without a deadly enemy");
        ai::EngineContractAccess::threat(engine, 1, 2, 10);
        enemy_type.combat_power = Real(1001);
        host->setup.content.types[1].combat_power = enemy_type.combat_power;
        expect(service(31) == 2, "WHE-67: an unshielded deadly enemy still triggers corruption");
        view->units[0].abilities[0].ready = false;
        expect(service(61) == 2, "WHE-67: a cooling-down hero does not issue another request");
        view->units[0].abilities[0].ready = true;
        for (auto& unit : view->units) if (unit.owner == 1) unit.shield = Real(1);
        for (std::uint64_t id = 4; id <= 6; ++id) {
            enemy.id = id; enemy.shield = Real(1); enemy.position.x = fixed(250);
            view->units.push_back(enemy);
        }
        ai::AiRandom draws;
        draws.set_seed(ai::EngineContractAccess::selection_seed(engine, 2));
        for (unsigned draw = 0; draw < 4; ++draw) static_cast<void>(draws.next());
        expect(service(91) == 3 && ai::EngineContractAccess::selection_seed(engine, 2) == draws.seed(),
            "WHE-68: five candidates use exactly two random planar cluster centers");
    }
}


void corrupt_systems_cluster_coordinates() {
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x, std::int64_t y) { return math::Vec3{fixed(x), fixed(y), {}}; };
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
        setup.units = {{1, 100, 2, at(0, 0), math::identity_quat(), {}}};
        const std::array points{at(100, 0), at(200, 100), at(300, -100), at(500, 150), at(650, -150)};
        for (std::size_t index = 0; index < points.size(); ++index)
            setup.units.push_back({index + 2, 101, 1, points[index], math::identity_quat(), {}});
        tactical::AbilityProfile cast;
        cast.kind = tactical::AbilityKind::corrupt_systems;
        cast.gui_activated_ability_name = "area";
        cast.recharge_frames = 1800; cast.recharge_at_activation = true; cast.effective_radius = fixed(400);
        tactical::SpecialAbilityProfile handler;
        handler.name = "area"; handler.kind = tactical::SpecialAbilityKind::corrupt_systems;
        handler.style = tactical::SpecialActivationStyle::user_input;
        handler.corrupt_min_range = fixed(20); handler.corrupt_max_range = fixed(350); handler.corrupt_duration_frames = 360;
        tactical::AbilityTable abilities;
        abilities.profiles = {{100, {cast}, false, {handler}}};
        tactical::MotionTable motion;
        motion.rules = {fixed(15), fixed(300)};
        const tactical::MotionProfile source_motion{100, fixed(1), fixed(1), fixed(1), fixed(1), fixed(1)};
        const tactical::MotionProfile enemy_motion{101, fixed(1), fixed(1), fixed(1), fixed(1), fixed(1)};
        motion.profiles = {source_motion, enemy_motion};
        tactical::CombatTable combat;
        tactical::CombatProfile source_combat, enemy_combat;
        source_combat.type_id = 100; enemy_combat.type_id = 101;
        combat.profiles = {source_combat, enemy_combat};
        auto created = tactical::TacticalSession::create(setup, {}, {}, motion, std::nullopt, combat, {}, abilities);
        expect(created.has_value(), "WHE-68: asymmetric cluster world starts");
        if (!created) continue;
        auto world = std::move(created).value();
        auto host = ai::make_tactical_host();
        host->setup.players = {{1, "REBEL", false, false, ""}, {2, "UNDERWORLD", false, true, ""}};
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        foc::AiType hero, enemy_type;
        hero.type_id = 100; hero.corrupt_systems_script = true;
        enemy_type.type_id = 101; enemy_type.combat_power = Real(1000);
        host->setup.content.types = {hero, enemy_type};
        host->types.emplace(100, &host->setup.content.types[0]);
        host->types.emplace(101, &host->setup.content.types[1]);
        auto view = std::make_shared<foc::detail::WorldView>();
        view->players = {{1, 1, 0}, {2, 2, 1}};
        for (const auto& unit : setup.units) {
            foc::detail::ViewUnit shown;
            shown.id = unit.entity_id; shown.type = unit.type_id; shown.owner = shown.team = unit.owner;
            shown.position = unit.position; shown.shield = Real(1);
            if (shown.id == 1) {
                tactical::AbilityStatus status;
                status.kind = tactical::AbilityKind::corrupt_systems; status.ready = true;
                shown.abilities = {status};
            }
            view->units.push_back(shown);
        }
        host->view = view;
        ai::Engine engine(host, {}, {});
        ai::EngineContractAccess::collect_fixture(engine);
        ai::EngineContractAccess::set_selection_seed(engine, 2, 0);
        ai::EngineContractAccess::corrupt_systems(engine, 1);
        const auto* command = ai::EngineContractAccess::last_order(engine);
        expect(command && command->arguments.size() == 7, "WHE-68: asymmetric cluster issues a point request");
        if (!command || command->arguments.size() != 7) continue;
        expect(std::get<Real>(command->arguments[4].data) == Real(150)
            && std::get<Real>(command->arguments[5].data) == Real(50),
            "WHE-68: seed zero draws Y before X and selects the in-range point (150,50)");
        expect(ai::EngineContractAccess::selection_seed(engine, 2) == 2919708916U,
            "WHE-68: the asymmetric two-center search consumes exactly four synchronized draws");
        const auto order = foc::tactical_ai_detail::translate_order(*command);
        expect(order.has_value(), "WHE-68: asymmetric point request translates");
        if (!order) continue;
        expect(world.submit({{0, order.value().issuer, 0}, order.value().units, order.value().payload}).has_value(),
            "WHE-68: asymmetric point request enters the session");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(world.step(executor).has_value(), "WHE-68: asymmetric point request executes");
        const auto& instances = world.snapshot()->instances();
        const auto recipient = std::find_if(instances.begin(), instances.end(), [](const auto& instance) { return instance.entity_id == 2; });
        expect(recipient != instances.end() && recipient->corrupt.has_value(),
            "WHE-68: the selected point passes the authored 350 range and corrupts an enemy");
    }
}


void squadron_ability_context() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{fixed(x), {}, {}}; };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{10, 100, 1, at(0), math::identity_quat(), {}},
        {11, 101, 1, at(0), math::identity_quat(), {}}, {20, 102, 2, at(2000), math::identity_quat(), {}}};
    setup.squadrons = {{10, {11}}};
    tactical::MotionTable motion;
    tactical::CraftProfile craft;
    craft.type_id = 101;
    craft.max_speed = fixed(5); craft.min_speed = fixed(1); craft.rate_of_turn = fixed(6);
    craft.lift = fixed(6); craft.thrust = fixed(1); craft.roll_rate = fixed(6);
    craft.bank_angle = fixed(70); craft.strafe_distance = fixed(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{100, {101}, {at(0)}, fixed(1000), fixed(200), fixed(300), fixed(20)}};
    tactical::CombatTable combat;
    tactical::CombatProfile fighter, enemy;
    fighter.type_id = 101; fighter.max_attack_distance = fixed(450);
    enemy.type_id = 102;
    combat.profiles = {fighter, enemy};
    tactical::AbilityProfile lock;
    lock.kind = tactical::AbilityKind::spoiler_lock;
    tactical::UnitAbilityProfile abilities;
    abilities.type_id = 101;
    abilities.abilities = {lock};
    tactical::AbilityTable table;
    table.profiles = {abilities};
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(setup, {}, {}, motion, std::nullopt, combat, {}, table);
        expect(created.has_value(), "FH-23: squadron context world starts");
        if (!created) return;
        auto world = std::move(created).value();
        auto host = ai::make_tactical_host();
        foc::AiType type;
        type.type_id = 100; type.locomotor = true; type.category_bits = 1;
        host->setup.content.types = {type};
        host->setup.content.categories.emplace("FIGHTER", 1);
        host->types.emplace(100, &host->setup.content.types[0]);
        auth::ModuleManifest modules;
        expect(modules.add("CONTEXT.LUA",
            "function Check(unit)\n"
            " target = unit.Get_Attack_Target()\n"
            " busy = unit.Has_Active_Orders()\n"
            " if target then unit.Activate_Ability('SPOILER_LOCK', false)\n"
            " elseif not busy then unit.Activate_Ability('SPOILER_LOCK', true) end\n"
            "end\n").has_value(), "FH-23: query fixture loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        if (!scripts) { expect(false, "FH-23: query scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "CONTEXT.LUA").has_value(),
            "FH-23: real object bindings register");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const auto query = [&]() {
            host->view = foc::detail::build_view(world, *world.snapshot());
            auth::ScriptEvent event;
            event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
            event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 10}}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "FH-23: query queues");
            return scripts.value().service(executor);
        };
        const auto step = [&](tactical::CommandPayload payload, std::uint64_t sequence) {
            tactical::PlayerCommand command{{world.completed_tick(), 1, sequence}, {10}, std::move(payload)};
            expect(world.submit(command).has_value(), "FH-23: world order queues");
            auto result = world.step(executor);
            expect(result.has_value(), "FH-23: world steps");
            if (result) hashes.push_back(result.value().state_sha256);
        };
        const auto idle = query();
        expect(idle && idle.value().commands.size() == 1, "FH-23: idle freestore requests travel lock");
        step(tactical::AbilityPayload{tactical::AbilityKind::spoiler_lock, tactical::AbilityAction::activate}, 0);
        step(tactical::MovePayload{at(3000)}, 1);
        const auto travel = query();
        expect(travel && travel.value().commands.empty(), "FH-23: travelling formation is busy, no repeated idle toggle");
        expect(host->view->find(10)->formation_moving, "FH-23: container projects formation travel");
        expect(!host->view->find(10)->moving,
            "FH-23: Lua formation travel does not replace the separate taskforce completion input");
        step(tactical::AttackPayload{20}, 2);
        const auto engaged = query();
        expect(host->view->find(10)->formation_target == 20 && host->view->find(11)->formation_target == 20,
            "FH-23: container and craft project the live formation target");
        expect(host->view->find(10)->attack_target == 0 && host->view->find(11)->attack_target == 0,
            "PL-21: a distant formation target does not mark fighters engaged or fire an in-range event");
        const auto target = scripts.value().read_global(1, "target");
        expect(target && target.value() && std::get<auth::Handle>(target.value()->data).id == 20,
            "FH-23: Lua queries still return the commanded formation target");
        ai::Engine selection(host, {}, {});
        ai::PlayerAi player;
        player.player = 1;
        ai::PlanDef plan;
        plan.allow_engaged = false;
        const ai::Goal goal;
        expect(ai::EngineContractAccess::eligible(selection, player, goal, plan) == std::vector<eawr::sim::EntityId>{10},
            "PL-21: a fighter approaching its commanded target can still join an unengaged-only plan");
        auto combat_view = std::make_shared<foc::detail::WorldView>(*host->view);
        for (auto& unit : combat_view->units) if (unit.id == 11) unit.attack_target = 20;
        host->view = combat_view;
        expect(ai::EngineContractAccess::eligible(selection, player, goal, plan).empty(),
            "PL-21: actual craft combat excludes the squadron from an unengaged-only plan");
        plan.allow_engaged = true;
        expect(ai::EngineContractAccess::eligible(selection, player, goal, plan) == std::vector<eawr::sim::EntityId>{10},
            "PL-21: a plan allowing engaged fighters can take the same squadron");
        expect(engaged && engaged.value().commands.size() == 1,
            "FH-23: engaged freestore requests open foils");
        if (engaged && engaged.value().commands.size() == 1) {
            const auto& request = engaged.value().commands.front();
            expect(request.verb == foc::verb_ability && request.arguments.size() == 4
                && std::get<Real>(request.arguments[3].data) == Real(static_cast<int>(tactical::AbilityAction::deactivate)),
                "FH-23: the engaged request deactivates travel lock");
        }
        const auto busy = scripts.value().read_global(1, "busy");
        expect(busy && busy.value() && std::get<bool>(busy.value()->data), "FH-23: engaged squadron reports active orders");
        step(tactical::AbilityPayload{tactical::AbilityKind::spoiler_lock, tactical::AbilityAction::deactivate}, 3);
        const auto state = world.ability_state(11);
        expect(state && !state->slots[0].active, "FH-23: engaged X-wing foils are open");
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "FH-23: squadron context hashes equally across workers");
    }
}


// WAB-70/76: the stock AI's Activate_Ability("BUZZ_DROIDS", object) reads the container's own team
// status and becomes a point ability command at the object's position; the unit call answers true.
void buzz_droids_lua_point(const bool corrupt) {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{fixed(x), {}, {}}; };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{10, 100, 1, at(0), math::identity_quat(), {}},
        {11, 101, 1, at(0), math::identity_quat(), {}}, {20, 102, 2, at(200), math::identity_quat(), {}}};
    setup.squadrons = {{10, {11}}};
    tactical::MotionTable motion;
    tactical::CraftProfile craft;
    craft.type_id = 101;
    craft.max_speed = fixed(5); craft.min_speed = fixed(1); craft.rate_of_turn = fixed(6);
    craft.lift = fixed(6); craft.thrust = fixed(1); craft.roll_rate = fixed(6);
    craft.bank_angle = fixed(70); craft.strafe_distance = fixed(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{100, {101}, {at(0)}, fixed(1000), fixed(200), fixed(300), fixed(20)}};
    tactical::CombatTable combat;
    tactical::CombatProfile fighter, enemy;
    fighter.type_id = 101; fighter.max_attack_distance = fixed(450);
    enemy.type_id = 102;
    combat.profiles = {fighter, enemy};
    tactical::AbilityProfile buzz;
    buzz.kind = tactical::AbilityKind::buzz_droids;
    buzz.team = true;
    buzz.recharge_frames = 1800;
    buzz.buzz = tactical::BuzzDroidsProfile{103, fixed(5), fixed(400), fixed(300), fixed(175), fixed(1), fixed(1), fixed(4), 30, 1, 0};
    tactical::UnitAbilityProfile abilities;
    abilities.type_id = 100;
    abilities.abilities = {buzz};
    const auto kind = corrupt ? tactical::AbilityKind::corrupt_systems : tactical::AbilityKind::buzz_droids;
    const std::string ability_name = corrupt ? "CORRUPT_SYSTEMS" : "BUZZ_DROIDS";
    if (corrupt) {
        abilities.type_id = 101;
        auto& cast = abilities.abilities[0];
        cast.kind = kind;
        cast.team = false;
        cast.buzz.reset();
        cast.recharge_at_activation = true;
        cast.gui_activated_ability_name = "area";
        tactical::SpecialAbilityProfile handler;
        handler.kind = tactical::SpecialAbilityKind::corrupt_systems;
        handler.name = "area";
        handler.style = tactical::SpecialActivationStyle::user_input;
        handler.corrupt_duration_frames = 360;
        handler.corrupt_max_range = fixed(350);
        abilities.special = {handler};
    }
    tactical::AbilityTable table;
    table.profiles = {abilities};
    auto created = tactical::TacticalSession::create(setup, {}, {}, motion, std::nullopt, combat, {}, table);
    expect(created.has_value(), "WAB-76: buzz-droid AI world starts");
    if (!created) return;
    auto world = std::move(created).value();
    auto host = ai::make_tactical_host();
    foc::AiType type;
    type.type_id = corrupt ? 101 : 100; type.locomotor = true; type.category_bits = 1;
    type.abilities = {ability_name};
    host->setup.content.types = {type};
    host->types.emplace(type.type_id, &host->setup.content.types[0]);
    auth::ModuleManifest modules;
    expect(modules.add("BUZZ.LUA",
        "function Check(unit, enemy)\n"
        " ready = unit.Is_Ability_Ready('" + ability_name + "')\n"
        " issued = unit.Activate_Ability('" + ability_name + "', enemy)\n"
        "end\n").has_value(), "WAB-76: query fixture loads");
    auth::SessionConfig config;
    config.tick_duration = {1, 30};
    auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
    if (!scripts) { expect(false, "WAB-76: scheduler starts"); return; }
    std::vector<eawr::core::Diagnostic> errors;
    foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
    expect(errors.empty() && scripts.value().create_instance(1, "BUZZ.LUA").has_value(), "WAB-76: object bindings register");
    const eawr::platform::ThreadWorkerAdapter executor(1);
    expect(world.step(executor).has_value(), "WAB-76: world steps once");
    host->view = foc::detail::build_view(world, *world.snapshot());
    auth::ScriptEvent event;
    event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, 0};
    event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
    event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, corrupt ? 11U : 10U}}, auth::Value{auth::Handle{foc::handle_game_object, 20}}};
    expect(scripts.value().submit_event(std::move(event)).has_value(), "WAB-76: query queues");
    const auto served = scripts.value().service(executor);
    expect(served && served.value().commands.size() == 1, "WAB-76: one ability command for the squadron container");
    if (served && served.value().commands.size() == 1) {
        const auto& request = served.value().commands.front();
        expect(request.verb == foc::verb_ability && request.arguments.size() == 7
            && std::get<Real>(request.arguments[2].data) == Real(static_cast<int>(kind))
            && std::get<Real>(request.arguments[4].data) == Real(200),
            "WAB-76: the request carries the object's position as its point");
    }
    const auto ready = scripts.value().read_global(1, "ready");
    const auto issued = scripts.value().read_global(1, "issued");
    expect(ready && ready.value() && std::get<bool>(ready.value()->data), "WAB-70: the container's own team slot answers ready");
    expect(issued && issued.value() && std::get<bool>(issued.value()->data), "WAB-76: the unit call answers true once issued");
}


// EX-31: pad-capture plans must keep their travelling squadron reserved until its
// formation arrives. A container has no ship Motion; test the real simulation snapshot.
void squadron_movement_blocks() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{fixed(x), {}, {}}; };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{10, 100, 1, at(0), math::identity_quat(), {}},
        {11, 101, 1, at(0), math::identity_quat(), {}}, {20, 102, 2, at(2000), math::identity_quat(), {}}};
    setup.squadrons = {{10, {11}}};
    tactical::MotionTable motion;
    tactical::CraftProfile craft;
    craft.type_id = 101;
    craft.max_speed = fixed(5); craft.min_speed = fixed(1); craft.rate_of_turn = fixed(6);
    craft.lift = fixed(6); craft.thrust = fixed(1); craft.roll_rate = fixed(6);
    craft.bank_angle = fixed(70); craft.strafe_distance = fixed(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{100, {101}, {at(0)}, fixed(1000), fixed(200), fixed(300), fixed(20)}};
    std::optional<std::uint64_t> baseline_arrival;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(setup, {}, {}, motion);
        expect(created.has_value(), "EX-31: squadron movement world starts");
        if (!created) return;
        auto world = std::move(created).value();
        auto host = ai::make_tactical_host();
        auth::ModuleManifest modules;
        expect(modules.add("MOVEMENT.LUA", "function MainForce_Unit_Move_Finished(tf, unit) end\n").has_value(),
            "EX-31: movement handler loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        expect(scripts && scripts.value().create_instance(1, "MOVEMENT.LUA").has_value(),
            "EX-31: movement handler instance starts");
        if (!scripts) return;
        ai::PlanDef definition;
        definition.name = "FormationMove";
        ai::Engine engine(host, {}, {definition});
        std::uint64_t sequence = 0;
        ai::EngineContractAccess::movement_fixture(engine, scripts.value(), sequence);
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const tactical::PlayerCommand move{{0, 1, 0}, {10}, tactical::MovePayload{at(300)}};
        expect(world.submit(move).has_value(), "EX-31: ordinary squadron move submits");
        for (int tick = 0; tick < 2; ++tick) expect(world.step(executor).has_value(), "EX-31: squadron moves");
        host->view = foc::detail::build_view(world, *world.snapshot());
        expect(host->view->find(10)->formation_moving && !host->view->find(10)->moving,
            "EX-31: travelling container has formation motion and no ship Motion");
        ai::EngineContractAccess::movement_service(engine, 2);
        expect(!ai::EngineContractAccess::movement_finished(engine, 1) &&
            !ai::EngineContractAccess::movement_finished(engine, 2) && ai::EngineContractAccess::event_sequence(engine) == 0,
            "EX-31: move and ambush blocks do not finish or signal two ticks after departure");
        while (world.completed_tick() < 5000 && world.squadron_state(10)->mode == tactical::SquadronMode::move) {
            if (!world.step(executor)) { expect(false, "EX-31: travelling world steps"); return; }
        }
        expect(world.squadron_state(10)->mode != tactical::SquadronMode::move,
            "EX-31: squadron reaches its destination within the bounded fixture");
        host->view = foc::detail::build_view(world, *world.snapshot());
        ai::EngineContractAccess::movement_service(engine, static_cast<std::int64_t>(world.completed_tick()));
        expect(ai::EngineContractAccess::movement_finished(engine, 1) &&
            ai::EngineContractAccess::movement_finished(engine, 2) && ai::EngineContractAccess::event_sequence(engine) == 2,
            "EX-31: both blocks finish and signal exactly once at actual formation arrival");
        ai::EngineContractAccess::movement_service(engine, static_cast<std::int64_t>(world.completed_tick() + 1));
        expect(ai::EngineContractAccess::event_sequence(engine) == 2, "EX-31: completed formation blocks do not signal again");
        if (!baseline_arrival) baseline_arrival = world.completed_tick();
        else expect(world.completed_tick() == *baseline_arrival, "EX-31: arrival agrees on 1/2/4/8 workers");
    }
}


// EX-36 / WBP-40: arrival is not completion of guarding a live capture point.
void guard_block_lifecycle() {
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
    for (const bool lose_force : {false, true}) {
        World world;
        ai::ViewUnit target;
        target.id = 20;
        world.view->units.push_back(target);
        auth::ModuleManifest modules;
        expect(modules.add("GUARD.LUA", "function MainForce_Unit_Move_Finished(tf, unit) end\n"
            "function MainForce_Current_Target_Destroyed(tf) end\n").has_value(), "EX-36: guard handlers load");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        expect(scripts && scripts.value().create_instance(1, "GUARD.LUA").has_value(), "EX-36: guard instance starts");
        if (!scripts) return;
        ai::PlanDef definition;
        definition.name = "GuardCapturePoint";
        ai::Engine engine(world.host, {}, {definition});
        std::uint64_t sequence = 0;
        ai::EngineContractAccess::guard_fixture(engine, scripts.value(), sequence);
        ai::EngineContractAccess::movement_service(engine, 2);
        expect(!ai::EngineContractAccess::movement_finished(engine, 1)
            && ai::EngineContractAccess::movement_finished(engine, 2) && ai::EngineContractAccess::event_sequence(engine) == 2,
            "EX-36: stopped movers signal arrival once; only the ordinary move completes");
        for (const auto owner : {1U, 2U, 0U}) {
            world.view->units.back().owner = owner;
            ai::EngineContractAccess::movement_service(engine, 1802);
            expect(!ai::EngineContractAccess::movement_finished(engine, 1) && ai::EngineContractAccess::event_sequence(engine) == 2,
                "EX-36: guard survives neutral, friendly and hostile ownership after its mover list empties");
        }
        if (lose_force) ai::EngineContractAccess::empty_guard_force(engine);
        else world.view->units.pop_back();
        ai::EngineContractAccess::movement_service(engine, 1803);
        expect(ai::EngineContractAccess::movement_finished(engine, 1)
            && ai::EngineContractAccess::event_sequence(engine) == (lose_force ? 2U : 3U), "EX-36: force loss or target death releases the guard");
        ai::EngineContractAccess::movement_service(engine, 1804);
        expect(ai::EngineContractAccess::event_sequence(engine) == (lose_force ? 2U : 3U), "EX-36: released guard emits no duplicate events");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(scripts.value().service(executor).has_value(), "EX-36: lifecycle callbacks service on every worker count");
    }
    }
}


// EX-31 (#3087): an attack on a non-hostile object refuses its squadron and TARGETING movers (WHZ-51).
// A refused mover gets no order and signals nothing. With every mover refused, the block waits until
// the force empties, the target dies or its new owner is no enemy. Other movers move to the object.
void refused_attack_block() {
    for (const bool targeting : {true, false}) {
    for (const std::uint32_t owner : {4U, 2U}) { // the neutral player's, an enemy's
        World world;
        world.host->setup.players.push_back(foc::AiPlayer{4, "NEUTRAL", true, false, ""});
        world.host->players.clear();
        for (const foc::AiPlayer& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
        world.host->setup.content.types.front().targeting = targeting;
        ai::ViewUnit target;
        target.id = 20;
        target.type = 1;
        target.owner = owner;
        world.view->units.push_back(target);
        auth::ModuleManifest modules;
        expect(modules.add("ATTACK.LUA", "function MainForce_Unit_Move_Finished(tf, unit) end\n").has_value(),
            "EX-31: attack handler loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        expect(scripts && scripts.value().create_instance(1, "ATTACK.LUA").has_value(), "EX-31: attack instance starts");
        if (!scripts) return;
        ai::PlanDef definition;
        definition.name = "BombingRun";
        ai::Engine engine(world.host, {}, {definition});
        std::uint64_t sequence = 0;
        const auto& block = ai::EngineContractAccess::attack_fixture(engine, scripts.value(), sequence, 20);
        const bool refused = targeting && owner == 4;
        expect(block.movers.empty() == refused && (block.held_owner == 4) == refused && !block.finished
            && ai::EngineContractAccess::order_count(engine) == (refused ? 0U : 1U),
            "EX-31: only a TARGETING mover attacking a non-hostile object is refused, without an order");
        if (!refused) continue;
        for (const std::int64_t frame : {2, 1800}) {
            ai::EngineContractAccess::movement_service(engine, frame);
            expect(!block.finished && ai::EngineContractAccess::event_sequence(engine) == 0,
                "EX-31: a fully refused attack waits silently");
        }
        world.view->units.back().owner = 1;
        ai::EngineContractAccess::movement_service(engine, 1801);
        expect(block.finished && ai::EngineContractAccess::event_sequence(engine) == 0,
            "EX-31: the target's new non-enemy owner ends the wait");
    }
    }
}


// WTA-23, WTA-26 to WTA-28 (G-03, #731): an Acclamator of a destroy plan attacks the Rebel
// station from the quadrant its enemies defend least, then attacks the station.
void attack_positioning() {
    namespace math = eawr::sim::math;
    const auto at = [](std::int64_t x, std::int64_t y) {
        return math::Vec3{math::Fixed::from_integer(x).value(), math::Fixed::from_integer(y).value(), {}};
    };
    auto host = std::make_shared<ai::Host>();
    auto view = std::make_shared<ai::WorldView>();
    const auto type = [&](tactical::TypeId id, const char* name, std::int64_t power, std::int64_t reach,
                          std::int64_t half, bool locomotor) {
        foc::AiType entry;
        entry.type_id = id;
        entry.name = name;
        entry.category_bits = 2;
        entry.combat_power = ai::real(power);
        entry.max_attack_distance = ai::real(reach);
        entry.bounds_half_x = entry.bounds_half_y = ai::real(half);
        entry.locomotor = locomotor;
        host->setup.content.types.push_back(entry);
    };
    type(1, "REBEL_SHIP", 1000, 500, 50, true);
    type(2, "REBEL_STATION", 0, 0, 300, false);
    type(3, "ACCLAMATOR", 0, 1000, 200, true);
    for (const foc::AiType& entry : host->setup.content.types) {
        host->types.emplace(entry.type_id, &entry);
        host->types_by_name.emplace(entry.name, &entry);
    }
    host->setup.players = {foc::AiPlayer{1, "REBEL", false, false, ""}, foc::AiPlayer{2, "EMPIRE", false, true, ""}};
    for (const foc::AiPlayer& player : host->setup.players) host->players.emplace(player.player, &player);
    view->players = {foc::detail::ViewPlayer{1, 1, 0}, foc::detail::ViewPlayer{2, 2, 1}};
    const auto unit = [&](sim::EntityId id, tactical::TypeId kind, tactical::PlayerId owner, math::Vec3 position) {
        ai::ViewUnit entry;
        entry.id = id;
        entry.type = kind;
        entry.owner = owner;
        entry.position = position;
        entry.visible_to = 3;
        view->units.push_back(entry);
    };
    unit(20, 2, 1, at(0, 0));       // the station
    unit(21, 1, 1, at(-800, 0));    // a Rebel ship on its near (-x) side
    unit(30, 3, 2, at(-3000, 0));   // the Acclamator, attacking the station
    view->units.back().order = tactical::OrderKind::attack;
    view->units.back().order_target = 20;
    host->view = view;
    ai::Engine engine(host, {}, {});
    ai::EngineContractAccess::positioning_fixture(engine);
    const auto orders = [&] { return ai::EngineContractAccess::point_defence_orders(engine); };
    const auto number = [](const auth::Value& value) { return std::get<ai::Real>(value.data); };

    ai::EngineContractAccess::positioning_service(engine, 5);
    expect(orders() == 0, "WTA-23: a unit without the flag is not positioned");
    ai::EngineContractAccess::enable_positioning(engine, 10, true);
    expect(ai::EngineContractAccess::positioning_on(engine, 30), "WTA-23: Enable_Attack_Positioning(true) flags the members");
    ai::EngineContractAccess::positioning_service(engine, 10);
    expect(orders() == 0, "WTA-27: the first positioning waits for the next service");
    ai::EngineContractAccess::positioning_service(engine, 11);
    const auth::ScriptCommand* order = ai::EngineContractAccess::last_order(engine);
    expect(orders() == 1 && order != nullptr && order->verb == foc::verb_attack_move && order->arguments.size() == 5,
        "WTA-27: beyond its attack distance of the ideal position the unit attack-moves to a point");
    if (order != nullptr && order->arguments.size() == 5) {
        // WTA-28: s = min(|(300, 300)|, 1000 / 2) = 424.26; the +x boxes hold no Rebel force and
        // the first of them, +x+y, keeps the tie.
        const ai::Real x = number(order->arguments[2]);
        const ai::Real y = number(order->arguments[3]);
        expect(ai::real(424) < x && x < ai::real(425) && ai::real(424) < y && y < ai::real(425)
                && number(order->arguments[4]) == ai::Real{},
            "WTA-28: the ideal position is the centre of the least-defended quadrant box, at height 0");
    }
    ai::EngineContractAccess::positioning_service(engine, 160);
    expect(orders() == 1, "WTA-27: positioning runs every 150 frames");
    // The sim has taken the point attack move; the unit arrived on the quiet side.
    view->units.back().position = at(400, 400);
    view->units.back().order = tactical::OrderKind::attack_move;
    view->units.back().order_target = sim::invalid_entity_id;
    ai::EngineContractAccess::positioning_service(engine, 161);
    order = ai::EngineContractAccess::last_order(engine);
    expect(orders() == 2 && order != nullptr && order->verb == foc::verb_attack && order->arguments.size() == 3
            && std::get<auth::Handle>(order->arguments[2].data).id == 20,
        "WTA-27: within its attack distance it attacks the target");
    view->units.back().order = tactical::OrderKind::attack;
    view->units.back().order_target = 20;
    ai::EngineContractAccess::positioning_service(engine, 311);
    expect(orders() == 2, "an attack the unit already has is not ordered again");
    view->units.back().order = tactical::OrderKind::move;
    view->units.back().order_target = sim::invalid_entity_id;
    ai::EngineContractAccess::positioning_service(engine, 461);
    expect(orders() == 2, "WTA-27: a formation without an attack target is not positioned");
    // U-05/WHZ-51: retaining an object across a point move must not turn an ownership
    // change into an attack on an ally or neutral. The query retains the live destination,
    // so a later ownership change back to hostile can expose it again.
    std::int64_t frame = 611;
    const auto refuse_retained = [&](tactical::PlayerId owner, bool neutral, bool neutral_mover = false) {
        auto& target = view->units.front();
        auto& mover = view->units.back();
        target.owner = 1;
        host->setup.players.front().neutral = false;
        host->setup.players.back().neutral = false;
        mover.position = at(-3000, 0);
        mover.order = tactical::OrderKind::attack_move;
        mover.order_target = 20;
        const auto before = orders();
        ai::EngineContractAccess::positioning_service(engine, frame);
        expect(orders() == before + 1, "U-05: hostile object positioning retains its point-move target");
        target.owner = owner;
        host->setup.players.front().neutral = neutral;
        host->setup.players.back().neutral = neutral_mover;
        mover.position = at(400, 400);
        mover.order_target = sim::invalid_entity_id;
        ai::EngineContractAccess::positioning_service(engine, frame + 150);
        expect(orders() == before + 1, "WHZ-51: positioning silently refuses a retained nonhostile target");
        target.owner = 1;
        host->setup.players.front().neutral = false;
        host->setup.players.back().neutral = false;
        ai::EngineContractAccess::positioning_service(engine, frame + 151);
        const auto* resumed = ai::EngineContractAccess::last_order(engine);
        expect(orders() == before + 2 && resumed != nullptr && resumed->verb == foc::verb_attack
            && resumed->arguments.size() == 3
            && std::get<auth::Handle>(resumed->arguments[2].data).id == 20,
            "U-05/FC-20: hostility returning resumes the due retained destination without another 150-frame wait");
        frame += 450;
    };
    refuse_retained(2, false);
    refuse_retained(0, false);
    refuse_retained(1, true);
    refuse_retained(1, false, true);
    const auto replacement = [&](bool nonhostile, bool scripted) {
        auto& target = view->units.front();
        auto& mover = view->units.back();
        target.owner = 1;
        mover.position = at(-3000, 0);
        mover.order = tactical::OrderKind::attack;
        mover.order_target = 20;
        const auto before = orders();
        ai::EngineContractAccess::positioning_service(engine, frame);
        expect(orders() == before + 1, "U-05: replacement fixture starts with a retained positioning object");
        mover.order = tactical::OrderKind::attack_move;
        mover.order_target = sim::invalid_entity_id;
        if (nonhostile) {
            target.owner = 2;
            ai::EngineContractAccess::positioning_service(engine, frame + 150);
            expect(orders() == before + 1, "WHZ-51: replacement fixture first refuses its nonhostile object");
        }
        const auto point = at(3000, -3000);
        if (scripted) ai::EngineContractAccess::positioning_script_point_order(engine, point);
        else ai::EngineContractAccess::positioning_point_order(engine, point);
        const auto after_replacement = orders();
        if (!scripted) expect(after_replacement == before + 2, "EX-30: the TaskForce emits the replacement point order");
        target.owner = 1;
        ai::EngineContractAccess::positioning_service(engine, frame + (nonhostile ? 151 : 150));
        expect(orders() == after_replacement,
            "U-05: a replacement point attack-move cannot be redirected to the previous positioning object");
        frame += 450;
    };
    replacement(false, false);
    replacement(true, false);
    replacement(true, true);
    ai::EngineContractAccess::enable_positioning(engine, frame, false);
    expect(!ai::EngineContractAccess::positioning_on(engine, 30), "WTA-23: Enable_Attack_Positioning(false) clears the flag");
}


void target_choice() {
    int draws = 0;
    const auto choose = [&draws](std::vector<Real> scores, Real fraction, Real unit) {
        return ai::choose_target(scores, fraction, [&draws, unit] {
            ++draws;
            return unit;
        });
    };
    const Real half = ai::real(1) / ai::real(2);
    expect(!choose({}, half, 0), "FT-03: no candidate answers nil");
    expect(!choose({0, 0, 0}, half, 0) && !choose({0, 0, 0}, 0, 0), "FT-03: every score at 0 answers nil");
    expect(!choose({-1, -3}, half, 0) && !choose({-1, -3}, 0, 0) && !choose({-2, 0}, half, 0),
        "FT-03: every score at or below 0 answers nil");
    expect(draws == 0, "FT-03: no draw without a positive weight");
    expect(choose({0, 3, 0, -1}, half, 0) == 1U && draws == 0,
        "FT-03: a best with only zero or negative others is the answer, without a draw");
    expect(choose({0, 4}, half, 0) == 1U, "FT-03: a later positive score replaces the empty best");
    expect(choose({2, 2, 1}, 0, 0) == 0U && draws == 0, "FT-03: equal scores keep the earlier target as the best");
    // {2, 2, 1} at fraction 1/2: the draw weighs [#1: 2, #2: 1, best #0: 3 / (1/2) = 6], total 9.
    expect(choose({2, 2, 1}, half, 0) == 1U, "FT-01: the draw walks the others in list order first");
    expect(choose({2, 2, 1}, half, ai::real(1) / ai::real(4)) == 2U, "FT-01: 2.25 of 9 falls on the third target");
    expect(choose({2, 2, 1}, half, half) == 0U, "FT-01: the best weighs the others' total over the fraction");
    expect(draws == 3, "FT-01: one draw per weighted choice");
}


// DT-02: the decay step is read under its real tag name, and is 1 when the file has none.
void threat_decay_constant() {
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
        return std::nullopt;
    };
    const auto load = [&](const std::string& constants) {
        std::map<std::string, std::string, std::less<>> xml;
        for (const std::string& path : ai::ai_xml_files()) xml.emplace(path, "<Root></Root>");
        xml["data/xml/gameconstants.xml"] = constants;
        return ai::load_ai_data(xml, converters);
    };
    const auto authored = load("<Root><AI_SpaceThreatDecayStep>0.05</AI_SpaceThreatDecayStep></Root>");
    expect(authored && Real{} < authored.value().constants.threat_decay_step
            && authored.value().constants.threat_decay_step < ai::real(1) / ai::real(10),
        "DT-02: AI_SpaceThreatDecayStep is read from gameconstants.xml");
    const auto misspelled = load("<Root><AI_SpaceThreatDecay>0.05</AI_SpaceThreatDecay></Root>");
    expect(misspelled && misspelled.value().constants.threat_decay_step == ai::real(1),
        "DT-02: the engine default is 1 when the tag is absent");
}

} // namespace foc_contract
