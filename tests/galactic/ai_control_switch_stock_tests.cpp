#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <filesystem>
#include "../support/env.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../support/result.hpp"
// #6917: the control-switch story reward on the stock campaign: a switch queued on the production (async) schedule is saved while it
// waits, while the window that applies it runs, and after it applied; every save loads on 1, 2, 4 and 8 workers and continues exactly
// like the uninterrupted run, and the run itself equals on 1/2/4/8 workers.
#include "galactic_support.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "ai_campaign_gate.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"
#include "galactic_ai_intent_commit.hpp"
#include "galactic_ai_outbox_state.hpp"
#include "../../src/campaign/galactic_ai_lane_inbox.hpp"
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
namespace ck = galactic_campaign_gate::ck;

std::filesystem::path game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("installed campaign gate requires EAWR_EAW_GAME_ROOT");
    return std::filesystem::path(std::move(*root));
}

struct Result {
    std::string final_hash;
    std::string player_type_after;
};

Result run(const std::size_t workers) {
    Result result;
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
        std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers),
        eawr::data::retail::boolean(fog->text).value(), gc::galactic_max_objects, gc::galactic_max_objects, "Normal_Default",
        ga::retail_service_periods, ga::standard_async_schedule));
    galactic_campaign_gate::check(!setup->lifecycle.players.empty(), "the stock campaign has AI players");
    const auto player = setup->lifecycle.players.front().player;
    auto observed = std::make_shared<galactic_campaign_gate::Observations>();
    for (const auto& selected : setup->lifecycle.players) observed->players.emplace(selected.player, galactic_campaign_gate::PlayerProgress{});
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
    const auto identities = ck::game_identities(root_owner.game()->galaxy().setup());

    constexpr std::uint64_t switch_tick = 330, window = 15, follow = 24, end_tick = 480;
    std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> saves;
    std::map<std::uint64_t, std::string> hashes;
    const auto manager_type = [&]() -> std::string {
        const auto manager = observed->runtimes.front().manager.lock();
        return manager ? manager->player_type(player) : std::string();
    };
    const auto before_type = [&] { return manager_type(); };
    std::string authored_type;
    for (std::uint64_t tick = 1; tick <= end_tick; ++tick) {
        if (tick == switch_tick) {
            authored_type = before_type();
            galactic_campaign_gate::check(!authored_type.empty() && authored_type != "NONE", "the stock AI player starts as a playing type");
            const auto slot = root_owner.game()->ai_control_switch_slot();
            galactic_campaign_gate::check(slot && static_cast<bool>(*slot), "the installed native AI binds the control-switch slot");
            galactic_campaign_gate::check(must((*slot)(player, "None")).empty(), "the stock None type is accepted");
        }
        const auto state = must(root_owner.game()->step(*setup->executor));
        result.final_hash = state.state_hash.get();
        // The saves run from the tick the switch is queued through the window that applies it.
        if (tick >= switch_tick && tick < switch_tick + window)
            saves.emplace_back(tick, must(ck::save_game_checkpoint(*root_owner.game(), identities)));
        if (tick > switch_tick && tick <= switch_tick + window + follow) hashes.emplace(tick, state.state_hash.get());
    }
    result.player_type_after = manager_type();
    galactic_campaign_gate::check(result.player_type_after == "NONE", "the player plays as the switched type after the window that applied it");
    // Every save, from before the drain to after it, loads on every worker count and continues like the uninterrupted run.
    if (workers == 4) {
        const auto players_before = observed->players;
        std::size_t compared{};
        for (const auto& [saved_tick, bytes] : saves) {
            for (const std::size_t count : {1, 2, 4, 8}) {
                const eawr::platform::ThreadWorkerAdapter executor(count);
                auto restored = must(ck::load_game_checkpoint(bytes, identities, configuration.restore, executor));
                galactic_campaign_gate::check(must(ck::save_game_checkpoint(restored.session, identities)) == bytes,
                    "a save with the switch queued or applied loads and saves the same bytes");
                for (auto next = saved_tick + 1; next <= switch_tick + window + follow; ++next) {
                    const auto resumed = must(restored.session.step(executor));
                    galactic_campaign_gate::check(resumed.completed_tick == next && resumed.state_hash.get() == hashes.at(next),
                        "a session saved around the control switch continues exactly as the uninterrupted run");
                    ++compared;
                }
            }
        }
        observed->players = players_before;
        std::cout << "GCAI-CONTROL-SWITCH saves=" << saves.size() << " compared_ticks=" << compared << " equal=true" << std::endl;
        galactic_campaign_gate::check(!saves.empty() && compared != 0, "the control-switch sweep compared at least one continuation");
    }
    return result;
}

int run_all() {
    std::cout << "CTEST_FULL_OUTPUT\n";
    std::optional<Result> first;
    for (const std::size_t workers : {4, 1, 2, 8}) {
        const auto day = run(workers);
        if (!first) first = day;
        else galactic_campaign_gate::check(day.final_hash == first->final_hash && day.player_type_after == first->player_type_after,
            "the switched campaign equals on 1/2/4/8 workers (workers " + std::to_string(workers) + ")");
    }
    std::cout << "GCAI-CONTROL-SWITCH-WORKERS 4,1,2,8 final_hash_equal=true" << std::endl;
    return galactic_test::failures == 0 ? 0 : 1;
}
}
int case_galactic_ai_control_switch_tests() { try { return run_all(); } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; } }
