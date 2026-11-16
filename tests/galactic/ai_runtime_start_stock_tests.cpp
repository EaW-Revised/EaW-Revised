#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../support/env.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../support/result.hpp"
// #7128 regression: a real galactic campaign start installs the tactical AI globals and the galactic freestore bindings on
// one scheduler; both once registered Find_First_Object, so every campaign start died with EAWR-SCRIPT-0210 (duplicate binding).
// This starts the stock campaign through the production native-AI path and steps it, so a binding clash fails here.
#include "galactic_support.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "ai_campaign_gate.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"
#include "../../src/campaign/galactic_ai_usability.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/data/retail_values.hpp"
using eawr::testing::must;

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace ga = eawr::script::foc::galactic_ai;

std::filesystem::path game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("installed campaign start requires EAWR_EAW_GAME_ROOT");
    return std::filesystem::path(std::move(*root));
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
    const auto setup = must(cp::prepare_stock_galactic_ai_runtime(files, loaded.catalog, data, types, plan, {},
        std::make_shared<eawr::platform::ThreadWorkerAdapter>(2),
        eawr::data::retail::boolean(fog->text).value(), gc::galactic_max_objects, gc::galactic_max_objects, "Normal_Default",
        ga::retail_service_periods, ga::standard_async_schedule));
    galactic_campaign_gate::check(!setup->lifecycle.players.empty(), "the stock campaign has AI players");
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
    // The failing step: installing the native AI builds the galactic scripts, and a duplicate binding fails right here.
    must(configuration.configure_native_ai(*root_owner.game()));
    galactic_campaign_gate::check(observed->runtimes.size() == 1, "the stock campaign start owns one galactic AI runtime");
    for (std::uint64_t tick = 1; tick <= 8; ++tick) must(root_owner.game()->step(*setup->executor));
    std::cout << "GCAI-RUNTIME-START stock campaign started and stepped 8 ticks" << std::endl;
    return galactic_test::failures == 0 ? 0 : 1;
}
}
int case_galactic_ai_runtime_start_tests() { try { return run_all(); } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; } }
