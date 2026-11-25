#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../support/env.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../support/result.hpp"
// #8439: one Lua error in an AI callback must not stop a player's AI. The stock campaign runs through the production native-AI
// path with injected faults: the galactic free store's On_Unit_Service raises on every other call, and every other plan
// script's main raises on its first resume. The original logs the error and drops that call or thread; a plan left without a
// live thread ends (PL-42). So: the faults are reported, no AI instance and no AI player is retired, every free-store
// instance stays, and plans keep starting after the faults.
#include "galactic_support.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "ai_campaign_gate.hpp"
#include "galactic_ai_world.hpp"
#include "ai_engine.hpp"
#include "galactic_ai_plans.hpp"
#include "../../src/campaign/galactic_ai_usability.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
using eawr::testing::must;

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace ga = eawr::script::foc::galactic_ai;

std::filesystem::path game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("the installed Lua fault run requires EAWR_EAW_GAME_ROOT");
    return std::filesystem::path(std::move(*root));
}

constexpr std::string_view freestore_module = "data/scripts/freestore/galacticfreestore.lua";
constexpr std::string_view freestore_fault = "injected free-store fault";
constexpr std::string_view plan_fault = "injected plan fault";

// Appended after the module's own definitions, so it wraps them. Each instance has its own Lua state and counter.
std::string poisoned_freestore(std::string text) {
    text += "\nlocal eawr_fault_service = On_Unit_Service\n"
            "function On_Unit_Service(object)\n"
            "    EawrFaultNext = not EawrFaultNext\n" // Lua 5.0 has no modulo operator: a toggle raises on every other call
            "    if EawrFaultNext then error(\"";
    text += freestore_fault;
    text += "\") end\n"
            "    return eawr_fault_service(object)\n"
            "end\n";
    return text;
}
std::string poisoned_plan(std::string text) {
    text += "\nlocal eawr_fault_main = main\n"
            "function main(...)\n"
            "    if not EawrFaultRaised then EawrFaultRaised = true; error(\"";
    text += plan_fault;
    text += "\") end\n"
            "    return eawr_fault_main(...)\n"
            "end\n";
    return text;
}

int run_all() {
    std::cout << "CTEST_FULL_OUTPUT\n";
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
    galactic_campaign_gate::check(source && source->starting_active_player == "Underworld", "the stock campaign has its authored human seat");
    cp::CampaignSetupOptions options; options.forced_seed = 1;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        const bool human = faction.name == source->starting_active_player;
        options.players.push_back({{id, id, 0}, faction.name, true});
        options.seats.push_back({gc::SeatId{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
        if (human) options.solo_reference_human = id;
    }
    std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
    const auto plan = must(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
    const auto* fog = data.constant("AIUsesFogOfWarGalactic");
    galactic_campaign_gate::check(fog, "the stock AI fog policy exists");
    const auto stock = must(cp::prepare_stock_galactic_ai_runtime(files, loaded.catalog, data, types, plan, {},
        std::make_shared<eawr::platform::ThreadWorkerAdapter>(2),
        eawr::data::retail::boolean(fog->text).value(), gc::galactic_max_objects, gc::galactic_max_objects, "Normal_Default",
        ga::retail_service_periods, ga::standard_async_schedule));
    galactic_campaign_gate::check(!stock->lifecycle.players.empty(), "the stock campaign has AI players");

    // The same closure with the faults injected into its Lua modules.
    auto poisoned = std::make_shared<cp::GalacticAiRuntimeSetup>(*stock);
    const auto store = poisoned->modules.find(std::string(freestore_module));
    galactic_campaign_gate::check(store != poisoned->modules.end(), "the stock galactic free-store module is loaded");
    if (store == poisoned->modules.end()) return 1;
    store->second = poisoned_freestore(std::move(store->second));
    std::set<std::string> faulted_plans, clean_plans;
    bool odd = false;
    for (auto& [path, text] : poisoned->modules) {
        if (!eawr::script::foc::ai::galactic_goals::plan_directory_index(path) || text.find("function main") == std::string::npos) continue;
        const auto start = path.find_last_of('/') + 1;
        auto name = path.substr(start, path.size() - start - 4);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if ((odd = !odd)) { text = poisoned_plan(std::move(text)); faulted_plans.insert(name); }
        else clean_plans.insert(name);
    }
    galactic_campaign_gate::check(!faulted_plans.empty() && !clean_plans.empty(), "both poisoned and clean plan scripts exist");
    const std::shared_ptr<const cp::GalacticAiRuntimeSetup> setup = poisoned;

    auto observed = std::make_shared<galactic_campaign_gate::Observations>();
    const auto services = must(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default"));
    auto root_owner = must(cp::CampaignRoot::create(plan, services));
    const auto space = must(cp::prepare_stock_space_entry(files, loaded.catalog, data, types, plan));
    const auto usability = must(cp::prepare_stock_galactic_ai_usability(data, plan, setup->perception));
    cp::GalacticAiStockQueryFactory queries = [usability] {
        auto provider = std::make_shared<cp::GalacticAiUsabilityProvider>(usability);
        cp::GalacticAiStockQueries out;
        out.usability = [provider](auto who, const auto& snapshot, auto planet) { return provider->usability(who, snapshot, planet); };
        out.connectivity_publication = [provider](const auto& parent, auto& world, auto& engine, const auto& pool) { return provider->connectivity_publication(parent, world, engine, pool); };
        out.movement = [provider](auto who, const auto& snapshot, const auto& world, const auto& engine, const auto& pool) { return provider->movement(who, snapshot, world, engine, pool); };
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
    must(configuration.configure_native_ai(*root_owner.game()));
    galactic_campaign_gate::check(observed->runtimes.size() == 1, "the poisoned stock campaign owns one galactic AI runtime");
    if (observed->runtimes.size() != 1) return 1;
    auto& game = *root_owner.game();
    const auto view = observed->runtimes.front();

    constexpr std::uint64_t first_plan_instance = eawr::script::foc::ai::first_plan_instance;
    const auto freestore_instances = [&] {
        std::vector<std::uint64_t> out;
        for (const auto instance : game.galactic_scripts()->ai_scheduler().instances()) if (instance < first_plan_instance) out.push_back(instance);
        return out;
    };
    // Every running plan identity seen, with the tick it was first seen and its definition.
    std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> plans_seen;
    std::uint64_t first_store_fault{}, last_store_fault{}, first_plan_fault{}, store_faults{}, plan_faults{};
    std::vector<std::uint64_t> attached;
    constexpr std::uint64_t ticks = 3600; // two minutes of galactic time: several free-store unit services (30-45 s each)
    for (std::uint64_t step = 1; step <= ticks; ++step) {
        const auto tick = must(game.step(*setup->executor));
        if (attached.empty()) attached = freestore_instances();
        for (const auto& diagnostic : tick.diagnostics) {
            if (diagnostic.message.find(freestore_fault) != std::string::npos) {
                ++store_faults; if (!first_store_fault) first_store_fault = tick.completed_tick; last_store_fault = tick.completed_tick;
            }
            if (diagnostic.message.find(plan_fault) != std::string::npos) {
                ++plan_faults; if (!first_plan_fault) first_plan_fault = tick.completed_tick;
            }
        }
        const auto engine = view.engine.lock();
        galactic_campaign_gate::check(static_cast<bool>(engine), "the AI engine stays owned");
        if (!engine) return 1;
        for (const auto& selected : setup->lifecycle.players)
            for (const auto& goal : engine->galactic_goals(selected.player))
                if (goal.running_plan) plans_seen.try_emplace(goal.running_plan, tick.completed_tick, goal.definition);
    }
    const auto later = [&](std::uint64_t after, const std::set<std::string>* only) {
        return static_cast<std::size_t>(std::count_if(plans_seen.begin(), plans_seen.end(), [&](const auto& row) {
            auto name = row.second.second;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return row.second.first > after && (!only || only->contains(name));
        }));
    };
    const auto after_store = later(first_store_fault, nullptr);
    const auto after_plan = later(first_plan_fault, &clean_plans);
    const auto retired_instances = game.galactic_scripts()->ai_scheduler().retired_instances();
    const auto manager = view.manager.lock();
    const auto retirement_lines = view.retirements ? view.retirements->snapshot() : std::vector<std::string>{};
    std::cout << "GCAI-LUA-FAULT store_faults=" << store_faults << " first_store_fault=" << first_store_fault
              << " last_store_fault=" << last_store_fault << " plan_faults=" << plan_faults << " first_plan_fault=" << first_plan_fault
              << " plans_seen=" << plans_seen.size() << " plans_after_first_store_fault=" << after_store
              << " clean_plans_after_plan_fault=" << after_plan << " retired_instances=" << retired_instances.size()
              << " provider_retirements=" << (manager ? manager->lifecycle().retired_providers.size() : 0)
              << " freestore_instances=" << attached.size() << "->" << freestore_instances().size() << std::endl;
    galactic_campaign_gate::check(store_faults > 0, "a free-store callback raised the injected Lua error");
    galactic_campaign_gate::check(plan_faults > 0, "a plan thread raised the injected Lua error");
    galactic_campaign_gate::check(retired_instances.empty(), "no AI Lua instance is retired for a Lua error");
    galactic_campaign_gate::check(manager && manager->lifecycle().retired_providers.empty() && retirement_lines.empty(),
        "no AI player is retired");
    galactic_campaign_gate::check(!attached.empty() && freestore_instances() == attached,
        "every free-store instance survives its failing callbacks");
    galactic_campaign_gate::check(after_store > 0, "the AI starts plans after the first free-store fault");
    galactic_campaign_gate::check(after_plan > 0, "clean plans keep starting after the first plan fault");
    return galactic_test::failures == 0 ? 0 : 1;
}
}
int case_galactic_ai_lua_fault_tests() { try { return run_all(); } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; } }
