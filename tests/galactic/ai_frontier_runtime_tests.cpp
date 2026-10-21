#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <ios>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>
#include "../support/check.hpp"
#include "../support/env.hpp"
#include "../support/result.hpp"
// Same seeded stock campaign, with full preparation as the independent oracle.
#include "galactic_support.hpp"
#include "galactic_ai_world.hpp"
#include "../../src/campaign/galactic_ai_runtime.hpp"
#include "../../src/campaign/galactic_ai_usability.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace cp = eawr::campaign;
namespace ck = eawr::checkpoint;
namespace gc = eawr::sim::galactic;
namespace ga = eawr::script::foc::galactic_ai;
namespace ai = eawr::script::foc::ai;
namespace {
using eawr::testing::result_code_space_value::must;

void expect(bool value, std::string_view message) {
    if (!value) eawr::testing::require(value, message);
}
std::string game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("frontier stock probe needs game data");
    return std::move(*root);
}
struct Evidence {
    std::vector<std::string> hashes;
    std::uint64_t binds{}, sparse_binds{}, planets{}, force_planets{}, force_terms{};
    std::uint64_t active_barriers{}, maximum_binds{};
    double barrier_ms{}, world_bind_ms{}, active_barrier_ms{}, active_world_bind_ms{}, maximum_active_barrier_ms{};
};
Evidence run(const eawr::vfs::Vfs& files, const eawr::data::Catalog& catalog,
    const eawr::data::galactic::GalacticData& data, const eawr::units::GalacticTypeTable& types,
    const cp::CampaignSetupPlan& plan, const std::vector<ga::PlayerSelection>& selected,
    std::size_t workers, bool sparse, std::uint64_t frames) {
    const auto executor = std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers);
    const auto setup = must(cp::prepare_stock_galactic_ai_runtime(files, catalog, data, types, plan,
        selected, executor, false, 4096, 4096));
    const auto services = must(cp::prepare_live_campaign_services(files, catalog, data, types, plan, "Normal_Default"));
    auto root = must(cp::CampaignRoot::create(plan, services));
    const auto space = must(cp::prepare_stock_space_entry(files, catalog, data, types, plan));
    const auto usability = must(cp::prepare_stock_galactic_ai_usability(data, plan, setup->perception));
    cp::GalacticAiStockQueryFactory queries = [usability] {
        auto provider = std::make_shared<cp::GalacticAiUsabilityProvider>(usability);
        cp::GalacticAiStockQueries out;
        out.usability = [provider](auto player, const auto& snapshot, auto planet) { return provider->usability(player, snapshot, planet); };
        out.connectivity_publication = [provider](const auto& parent, auto& world, auto& engine, const auto& pool) {
            return provider->connectivity_publication(parent, world, engine, pool);
        };
        out.movement = [provider](auto player, const auto& snapshot, const auto& world, const auto& engine, const auto& pool) {
            return provider->movement(player, snapshot, world, engine, pool);
        };
        out.external.emplace("GALACTIC_USABILITY", std::move(provider));
        return eawr::core::Result<cp::GalacticAiStockQueries>::success(std::move(out));
    };
    std::vector<cp::GalacticAiRuntimeView> runtimes;
    const auto stock = must(cp::stock_galactic_ai_providers(setup, services.stock_starting_forces, space,
        std::move(queries), [&](auto view) { runtimes.push_back(std::move(view)); }));
    cp::GalacticAiRuntimeProviderFactory factory = [stock, sparse] {
        auto providers = must(stock());
        auto bind = std::move(providers.bind_actions);
        providers.bind_actions = [bind, sparse](auto& game, auto& galaxy, auto& actions, auto engine, auto world, auto pool) {
            world->enable_sparse_native_frontiers(sparse);
            return bind(game, galaxy, actions, std::move(engine), std::move(world), std::move(pool));
        };
        return eawr::core::Result<cp::GalacticAiRuntimeProviders>::success(std::move(providers));
    };
    eawr::platform::CampaignServiceConfiguration configuration;
    configuration.configure = [&](auto& game) { return cp::configure_live_campaign_conflicts(game, files, catalog, data, types, plan, space); };
    configuration.restore.configure_session = configuration.configure;
    configuration.restore.content = [space](const auto& conflict) { return space->content(conflict); };
    must(cp::configure_galactic_ai_runtime(configuration, setup, std::move(factory)));
    must(configuration.configure(*root.game())); must(configuration.configure_native_ai(*root.game()));
    expect(runtimes.size() == 1, "frontier probe owns one fresh stock runtime");
    const auto control = runtimes.front();
    const auto identities = ck::game_identities(root.game()->galaxy().setup());
    std::optional<ck::GameRestore> restored;
    Evidence evidence;
    std::optional<std::uint64_t> counted_barrier;
    const auto verify_records = [](const cp::GalacticAiRuntimeView& runtime) {
        const auto world = runtime.world.lock();
        expect(world && must(world->record_sync_hash()) == must(world->record_sync_hash(true)),
            "frontier perception record marks match cold reconstruction");
    };
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
        const auto before = eawr::gc_profile::snapshot();
        const auto tick = must(root.game()->step(*executor));
        const auto phase = eawr::gc_profile::difference(before, eawr::gc_profile::snapshot());
        evidence.barrier_ms += eawr::gc_profile::milliseconds(phase, eawr::gc_profile::Phase::ai_barrier_wall);
        evidence.world_bind_ms += eawr::gc_profile::milliseconds(phase, eawr::gc_profile::Phase::world_bind);
        evidence.hashes.push_back(tick.state_hash.get());
        if (restored) expect(must(restored->session.step(*executor)).state_hash.get() == evidence.hashes.back(),
            "full/sparse stock frontier continuation equals its independently restored owner");
        const auto world = control.world.lock();
        expect(static_cast<bool>(world), "stock frontier world remains owned");
        const auto work = world->native_frontier_work();
        if (work.binds && counted_barrier != work.barrier) {
            counted_barrier = work.barrier; ++evidence.active_barriers;
            const auto barrier_ms = eawr::gc_profile::milliseconds(phase, eawr::gc_profile::Phase::ai_barrier_wall);
            evidence.active_barrier_ms += barrier_ms;
            evidence.active_world_bind_ms += eawr::gc_profile::milliseconds(phase, eawr::gc_profile::Phase::world_bind);
            evidence.maximum_active_barrier_ms = std::max(evidence.maximum_active_barrier_ms, barrier_ms);
            evidence.binds += work.binds; evidence.sparse_binds += work.sparse_binds;
            evidence.planets += work.prepared_planets; evidence.force_planets += work.prepared_force_planets;
            evidence.force_terms += work.force_terms;
            evidence.maximum_binds = std::max(evidence.maximum_binds, work.binds);
        }
        if (frame + 1 == frames / 2) {
            verify_records(control);
            const auto saved = must(ck::save_game_checkpoint(*root.game(), identities));
            restored.emplace(must(ck::load_game_checkpoint(saved, identities, configuration.restore, *executor)));
            expect(must(ck::save_game_checkpoint(restored->session, identities)) == saved,
                "fresh stock frontier reconstruction recaptures canonical checkpoint bytes");
        }
    }
    verify_records(control); verify_records(runtimes.back());
    return evidence;
}
}
int case_galactic_ai_frontier_runtime_tests(int argc, char** argv) {
    try {
        const std::uint64_t frames = argc > 1 ? std::stoull(argv[1]) : 600;
        expect(frames >= 600, "frontier stock probe spans two native perception deadlines");
        std::vector<eawr::vfs::MountSpec> mounts;
        for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
            mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(game_root()) / folder / "Data")).mount);
        const auto files = must(eawr::vfs::Vfs::mount(mounts));
        const auto loaded = must(eawr::data::load_catalog(files, eawr::data::Profile::foc));
        const auto data = must(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
        const auto types = eawr::units::galactic_type_table(loaded.catalog);
        constexpr std::string_view campaign = "Sandbox_Equal_Footing_Underworld";
        const auto* source = data.campaign(campaign);
        expect(source != nullptr, "seeded frontier campaign exists");
        cp::CampaignSetupOptions options; options.seed = 1;
        std::vector<ga::PlayerSelection> selected;
        gc::PlayerId human{};
        for (const auto& faction : data.factions) {
            const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
            const bool local = faction.name == source->starting_active_player;
            const bool opponent = faction.name == "Empire" || faction.name == "Rebel";
            options.players.push_back({{id, id, 0}, faction.name, local || opponent || !faction.is_playable.value_or(false)});
            if (local || opponent) options.seats.push_back({gc::SeatId{id}, id, local ? gc::SeatKind::human : gc::SeatKind::ai,
                id - 1, true, local ? 1U : 0U});
            if (local) { human = id; options.solo_reference_human = id; }
            if (opponent) selected.push_back({id, faction.name == "Empire" ? "BASICEMPIRE" : "BASICREBEL", "NORMAL", {}, {}});
        }
        expect(human && selected.size() == 2, "seeded frontier campaign uses both stock AI controllers");
        selected[0].enemy = selected[1].player; selected[1].enemy = selected[0].player;
        for (auto& player : selected) player.human = human;
        std::vector<std::string> originals;
        for (const auto& type : types.types) originals.push_back(type.name);
        std::sort(originals.begin(), originals.end());
        for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
        const auto plan = must(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
        eawr::gc_profile::enabled.store(true, std::memory_order_relaxed);
        std::vector<std::string> baseline;
        std::ofstream report("native-frontier.json", std::ios::trunc);
        report << "{\"seed\":1,\"frames\":" << frames << ",\"runs\":[";
        bool first = true;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            std::optional<Evidence> full;
            for (const bool sparse : {false, true}) {
                const auto evidence = run(files, loaded.catalog, data, types, plan, selected, workers, sparse, frames);
                if (baseline.empty()) baseline = evidence.hashes;
                else expect(evidence.hashes == baseline, "every seeded stock tick equals full preparation on 1/2/4/8 workers");
                if (!sparse) full = evidence;
                else expect(evidence.binds && evidence.sparse_binds && evidence.binds == full->binds
                    && evidence.force_planets < full->force_planets,
                    "seeded stock frontier prepares fewer force planets than full preparation with identical reads");
                if (!first) report << ',';
                first = false;
                report << "{\"workers\":" << workers << ",\"sparse\":" << (sparse ? "true" : "false")
                    << ",\"binds\":" << evidence.binds << ",\"sparse_binds\":" << evidence.sparse_binds
                    << ",\"active_barriers\":" << evidence.active_barriers << ",\"maximum_binds\":" << evidence.maximum_binds
                    << ",\"prepared_planets\":" << evidence.planets << ",\"prepared_force_planets\":" << evidence.force_planets
                    << ",\"force_terms\":" << evidence.force_terms << ",\"barrier_ms\":" << evidence.barrier_ms
                    << ",\"active_barrier_ms\":" << evidence.active_barrier_ms
                    << ",\"active_world_bind_ms\":" << evidence.active_world_bind_ms
                    << ",\"maximum_active_barrier_ms\":" << evidence.maximum_active_barrier_ms
                    << ",\"world_bind_ms\":" << evidence.world_bind_ms << ",\"final_hash\":\"" << evidence.hashes.back() << "\"}";
                report.flush();
                std::cout << "FRONTIER workers=" << workers << " sparse=" << sparse << " binds=" << evidence.binds
                    << " force_planets=" << evidence.force_planets << " force_terms=" << evidence.force_terms
                    << " barrier_ms=" << evidence.barrier_ms << " world_bind_ms=" << evidence.world_bind_ms << std::endl;
            }
        }
        report << "],\"passed\":true}\n";
        expect(static_cast<bool>(report), "frontier timing receipt is durable");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
