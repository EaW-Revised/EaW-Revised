// WGA-485: bounded stock observation, sharing the campaign gate's real providers.
#include "galactic_support.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "ai_campaign_gate.hpp"
#include "../../src/campaign/galactic_ai_usability.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/data/retail_values.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using namespace galactic_test;
template<class T> T must(eawr::core::Result<T> value) { if (!value) throw std::runtime_error(value.error().message); return std::move(value).value(); }
void must(eawr::core::Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
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
int run() {
    std::cout << "CTEST_FULL_OUTPUT\n";
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
    galactic_campaign_gate::check(source && source->starting_active_player == "Underworld", "stock day campaign has its authored human seat");
    cp::CampaignSetupOptions options; options.forced_seed = 1;
    std::map<gc::PlayerId, std::string> names;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        const bool human = faction.name == source->starting_active_player;
        options.players.push_back({{id, id, 0}, faction.name, true}); names[id] = faction.name;
        options.seats.push_back({gc::SeatId{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
        if (human) options.local_observer = id;
    }
    std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
    const auto plan = must(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
    const auto* fog = data.constant("AIUsesFogOfWarGalactic");
    galactic_campaign_gate::check(fog, "stock AI fog policy exists");
    const auto setup = must(cp::prepare_stock_galactic_ai_runtime(files, loaded.catalog, data, types, plan, {},
        std::make_shared<eawr::platform::ThreadWorkerAdapter>(4),
        eawr::data::retail::boolean(fog->text).value(), gc::galactic_max_objects, gc::galactic_max_objects, "Normal_Default"));
    auto observed = std::make_shared<galactic_campaign_gate::Observations>();
    for (const auto& player : setup->lifecycle.players) observed->players.emplace(player.player, galactic_campaign_gate::PlayerProgress{});
    const auto services = must(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default"));
    auto root_owner = must(cp::CampaignRoot::create(plan, services));
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
    must(configuration.configure_native_ai(*root_owner.game()));

    struct Counts { std::uint64_t started{}, released{}, expired{}, hostile{}, first{}; };
    std::map<gc::PlayerId, Counts> counts;
    std::set<std::tuple<std::uint64_t, gc::PlayerId, std::string, std::string>> logged;
    std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> moves;
    std::map<gc::FleetId, gc::PlayerId> fleet_owners;
    for (const auto& fleet : root_owner.game()->galaxy().snapshot()->fleets()) fleet_owners[fleet.id] = fleet.owner;
    for (std::uint64_t tick = 1; tick <= 6000; ++tick) {
        const auto state = must(root_owner.game()->step(*setup->executor));
        const auto engine = observed->runtimes.front().engine.lock();
        if (!engine) throw std::runtime_error("day-run native engine owner expired");
        for (const auto& row : engine->records()) {
            if (row.event != "assembly_sync_started" && row.event != "assembly_sync_released" && row.event != "assembly_sync_expired") continue;
            if (!logged.emplace(row.tick, row.player, row.event, row.detail).second) continue;
            auto& count = counts[row.player];
            if (row.event == "assembly_sync_started") ++count.started;
            if (row.event == "assembly_sync_released") ++count.released;
            if (row.event == "assembly_sync_expired") ++count.expired;
        }
        for (const auto& fleet : state.galaxy->fleets()) fleet_owners[fleet.id] = fleet.owner;
        for (const auto& event : state.galaxy->events()) {
            if (event.kind != gc::GalacticEventKind::movement_begin || !moves.emplace(event.tick, event.subject, event.value).second) continue;
            const auto planet = std::find_if(state.galaxy->planets().begin(), state.galaxy->planets().end(),
                [&](const auto& row) { return row.id == event.value; });
            const auto player = fleet_owners.find(event.subject);
            if (player == fleet_owners.end() || planet == state.galaxy->planets().end()) throw std::runtime_error("day-run movement attribution missing");
            if (planet->owner != player->second) {
                auto& count = counts[player->second]; ++count.hostile;
                if (!count.first) count.first = event.tick;
            }
        }
        if (tick % 600 == 0) std::cout << "synchronized-day progress tick=" << state.completed_tick << std::endl;
    }
    galactic_campaign_gate::check(root_owner.game()->completed_tick() == 6000, "bounded stock observation reaches its exact tick limit");
    for (const auto& [player, progress] : observed->players) {
        const auto& count = counts[player];
        galactic_campaign_gate::check(count.expired <= count.released && (count.started || (!count.released && !count.expired)),
            "a countdown expiry always has a release and every release has a started group");
        if (names[player] == "Empire" || names[player] == "Rebel")
            galactic_campaign_gate::check(count.hostile && count.first && progress.attacks,
                "both stock opponents produce attributed hostile movement and conflicts by tick6000");
        std::cout << "SYNC_DAY player=" << names[player] << " seed=1 workers=4 ticks=6000 started=" << count.started
            << " released=" << count.released << " expired=" << count.expired << " hostile_moves=" << count.hostile
            << " first_hostile_tick=" << count.first << " conflicts=" << progress.attacks << std::endl;
    }
    return galactic_test::failures == 0 ? 0 : 1;
}
}
int main() { try { return run(); } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; } }
