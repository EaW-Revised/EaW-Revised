#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "galactic_ai_markup.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/ai_perception_content.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "../../src/campaign/galactic_ai_runtime.hpp"
#include "../../src/campaign/galactic_ai_usability.hpp"
#include "eawr/script/numeric/decimal.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <utility>

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace ai = eawr::script::foc::ai;
namespace ga = eawr::script::foc::galactic_ai;
using galactic_test::expect;
using eawr::testing::result_diagnostic::must;

struct PreparationLog {
    std::ostringstream text;
    std::streambuf* previous{std::clog.rdbuf(text.rdbuf())};
    ~PreparationLog() { std::clog.rdbuf(previous); }
};
std::filesystem::path game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("markup stock test requires game data");
    return std::filesystem::path(std::move(*root));
}
std::string run(const eawr::vfs::Vfs& files, const eawr::data::Catalog& catalog,
    const eawr::data::galactic::GalacticData& data, const eawr::units::GalacticTypeTable& types,
    std::uint64_t frames, unsigned workers, const std::filesystem::path& directory) {
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Underworld";
    const auto* authored = data.campaign(campaign); if (!authored) throw std::runtime_error("stock Equal Footing campaign missing");
    cp::CampaignSetupOptions options; options.seed = 42;
    std::vector<ga::PlayerSelection> selected;
    gc::PlayerId human{};
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        const bool active = faction.name == "Empire" || faction.name == "Rebel" || faction.name == authored->starting_active_player;
        options.players.push_back({{id, id, 0}, faction.name, active || !faction.is_playable.value_or(false)});
        if (faction.name == "Empire" || faction.name == "Rebel") {
            options.seats.push_back({gc::SeatId{id}, id, gc::SeatKind::ai, id - 1, true, 0});
            selected.push_back({id, faction.name == "Empire" ? "BASICEMPIRE" : "BASICREBEL", "NORMAL", {}, {}});
        }
        if (faction.name == authored->starting_active_player) {
            human = id; options.local_observer = id;
            options.seats.push_back({gc::SeatId{id}, id, gc::SeatKind::human, id - 1, true, 1});
        }
    }
    expect(selected.size() == 2 && human, "installed probe uses two stock AI seats and the authored human");
    if (selected.size() != 2 || !human) throw std::runtime_error("stock campaign seats unavailable");
    selected[0].enemy = selected[1].player; selected[1].enemy = selected[0].player;
    for (auto& player : selected) player.human = human;
    std::vector<std::string> names; for (const auto& type : types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
    const auto plan = must(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
    auto executor = std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers);
    const auto setup = must(cp::prepare_stock_galactic_ai_runtime(files, catalog, data, types, plan, selected, executor, false, 4096, 4096));
    for (const auto& player : selected) {
        const auto hints = setup->perception.markup_hints.find(player.player);
        expect(hints != setup->perception.markup_hints.end() && hints->second.set == "GALACTIC_HINTS"
            && hints->second.defaults.contains("PRIORITYTARGET"), "Markup_Filename loads installed authored hint-set defaults for each AI");
    }
    std::shared_ptr<const cp::GalacticAiRuntimeSetup> missing_setup;
    if (workers == 1) {
        auto modified_data = data;
        for (auto& definition : modified_data.campaigns) if (definition.name == campaign)
            definition.markup_filenames.insert(definition.markup_filenames.begin(), {"eMpIrE", "ShipyardsOfKuatHints"});
        eawr::data::DocumentOverrides changes({
            {"data/xml/ai/hintsets/testhintsets.xml", "Galactic_Hints/Mod_Hint", {"6 trailing"}},
            {"data/xml/enum/perceptiontokentype.xml", "Mod_Registered", {""}},
            {"data/xml/ai/galacticmarkup/shipyardsofkuathints.xml", "Planets/Kuat/Mod_Hint", {"3 trailing", "9"}},
            {"data/xml/ai/galacticmarkup/shipyardsofkuathints.xml", "Planets/Kuat/Mod_Registered", {"2"}},
            {"data/xml/ai/galacticmarkup/shipyardsofkuathints.xml", "Planets/Kuat/Mod_Unregistered", {"9"}}});
        PreparationLog preparation_log;
        const auto changed_runtime = must(cp::prepare_stock_galactic_ai_runtime(files, catalog, modified_data, types,
            plan, selected, executor, false, 4096, 4096));
        const auto* changed = &changed_runtime->perception;
        const auto empire = std::find_if(changed->players.begin(), changed->players.end(), [](const auto& player) { return ai::upper_case(player.faction) == "EMPIRE"; });
        if (empire == changed->players.end()) throw std::runtime_error("installed Empire observer absent");
        const auto& hints = changed->markup_hints.at(empire->player);
        expect(hints.defaults.at("MOD_HINT") == ai::real(6) && hints.planets.at("KUAT").at("MOD_HINT") == ai::real(3),
            "mod-authored hint defaults and override use lenient numbers and first duplicate value");
        expect(hints.planets.at("KUAT").at("MOD_REGISTERED") == ai::real(2)
            && !hints.planets.at("KUAT").contains("MOD_UNREGISTERED"), "markup vocabulary follows authored perception tokens and logs unknown names");
        expect(preparation_log.text.str().find("duplicate markup hint: MOD_HINT") != std::string::npos
            && preparation_log.text.str().find("unknown markup hint: MOD_UNREGISTERED") != std::string::npos,
            "production preparation surfaces duplicate and unknown hint warnings");
        for (auto& definition : modified_data.campaigns) if (definition.name == campaign)
            definition.markup_filenames.insert(definition.markup_filenames.begin(), {"Empire", "MissingMarkupFixture"});
        missing_setup = must(cp::prepare_stock_galactic_ai_runtime(files, catalog, modified_data, types,
            plan, selected, executor, false, 4096, 4096));
        expect(!missing_setup->perception.markup_hints.contains(empire->player)
            && preparation_log.text.str().find("unreadable galactic markup: data/xml/ai/galacticmarkup/MissingMarkupFixture.xml") != std::string::npos,
            "production preparation logs missing markup while continuing without that player's hint branch");
    }
    const auto services = must(cp::prepare_live_campaign_services(files, catalog, data, types, plan, "Normal_Default"));
    auto root = must(cp::CampaignRoot::create(plan, services));
    const auto space = must(cp::prepare_stock_space_entry(files, catalog, data, types, plan));
    const auto usability = must(cp::prepare_stock_galactic_ai_usability(data, plan, setup->perception));
    cp::GalacticAiStockQueryFactory queries = [usability] {
        auto provider = std::make_shared<cp::GalacticAiUsabilityProvider>(usability);
        cp::GalacticAiStockQueries out;
        out.usability = [provider](auto player, const auto& snapshot, auto planet) { return provider->usability(player, snapshot, planet); };
        out.connectivity_publication = [provider](const auto& parent, auto& world, auto& engine, const auto& workers_) { return provider->connectivity_publication(parent, world, engine, workers_); };
        out.movement = [provider](auto player, const auto& snapshot, const auto& world, const auto& engine, const auto& workers_) { return provider->movement(player, snapshot, world, engine, workers_); };
        out.external.emplace("GALACTIC_USABILITY", std::move(provider));
        return eawr::core::Result<cp::GalacticAiStockQueries>::success(std::move(out));
    };
    std::vector<cp::GalacticAiRuntimeView> views;
    auto factory = must(cp::stock_galactic_ai_providers(setup, services.stock_starting_forces, space, std::move(queries),
        [&views](auto view) { views.push_back(std::move(view)); }));
    eawr::platform::CampaignServiceConfiguration configuration;
    configuration.configure = [&](auto& game) { return cp::configure_live_campaign_conflicts(game, files, catalog, data, types, plan, space); };
    configuration.restore.configure_session = configuration.configure;
    configuration.restore.content = [space](const auto& conflict) { return space->content(conflict); };
    must(cp::configure_galactic_ai_runtime(configuration, setup, std::move(factory)));
    must(configuration.configure(*root.game())); must(configuration.configure_native_ai(*root.game()));
    if (views.empty()) throw std::runtime_error("markup runtime view absent");
    const auto world = views.front().world.lock(); const auto engine = views.front().engine.lock();
    if (!world || !engine) throw std::runtime_error("markup runtime owners absent");
    std::filesystem::create_directories(directory);
    std::ofstream totals(directory / ("totals-w" + std::to_string(workers) + ".tsv"));
    totals << "galactic_frame\tsynchronized_frame\tplayer\ttotal\tmarked_records\thandles\n";
    auto equation = must(ai::EquationSet::parse({{"probe.xml", "<Equations><Raw>Variable_Target.Markup</Raw></Equations>"}},
        [](std::string_view, std::string_view) -> std::optional<ai::Real> { return {}; }));
    const auto* raw = equation.find("RAW"); if (!raw) throw std::runtime_error("probe equation absent");
    std::uint64_t sampled = std::numeric_limits<std::uint64_t>::max();
    const auto sample = [&] {
        const auto frame = root.game()->galaxy().clock().galactic_frame;
        if (frame == sampled) return;
        sampled = frame; const auto state = world->markup()->state();
        for (const auto& player : selected) {
            ai::Real sum{}; std::size_t records{}, handles{};
            for (const auto& [key, value] : state.values) if (key.observer == player.player && !key.player_target) { sum += value; ++records; }
            for (const auto& [id, handle] : state.handles) { static_cast<void>(id); if (!handle.targets.empty() && handle.targets.front().observer == player.player) ++handles; }
            totals << frame << '\t' << root.game()->completed_tick() << '\t' << player.player << '\t'
                << eawr::script::numeric::decimal::format_shortest(sum.repr) << '\t' << records << '\t' << handles << '\n';
        }
        totals.flush();
    };
    sample();
    eawr::script::authoritative::ManagedHandleWork cost;
    std::uint64_t measured_services{}, maximum_census{}, maximum_objects{}, maximum_edges{}, maximum_entries{}, last_service{};
    while (root.game()->galaxy().clock().galactic_frame < frames) {
        if (root.game()->completed_tick() >= frames * 4) throw std::runtime_error("markup drift probe reached synchronization guard before requested galactic duration");
        must(root.game()->step(*executor));
        if (auto* bridge = root.game()->galactic_scripts()) {
            const auto& scheduler = bridge->ai_scheduler();
            if (scheduler.completed_tick() != last_service) {
                last_service = scheduler.completed_tick(); ++measured_services;
                const auto work = scheduler.managed_handle_work();
                cost.nanoseconds += work.nanoseconds; cost.service_nanoseconds += work.service_nanoseconds;
                cost.objects += work.objects; cost.edges += work.edges; cost.entries += work.entries;
                cost.censuses += work.censuses; cost.released += work.released;
                maximum_census = std::max(maximum_census, work.nanoseconds);
                maximum_objects = std::max(maximum_objects, work.objects); maximum_edges = std::max(maximum_edges, work.edges);
                maximum_entries = std::max(maximum_entries, work.entries);
            }
        }
        if (root.game()->galaxy().clock().galactic_frame % 600 == 0) sample();
    }
    sample();
    std::ofstream timing(directory / ("census-w" + std::to_string(workers) + ".tsv"));
    timing << "frames\tservices\tcensus_ns\tscheduler_ns\tmax_census_ns\tobjects\tedges\tentries\tmax_objects\tmax_edges\tmax_entries\tcensuses\treleased\n"
        << frames << '\t' << measured_services << '\t' << cost.nanoseconds << '\t' << cost.service_nanoseconds << '\t' << maximum_census
        << '\t' << cost.objects << '\t' << cost.edges << '\t' << cost.entries << '\t' << maximum_objects << '\t' << maximum_edges
        << '\t' << maximum_entries << '\t' << cost.censuses << '\t' << cost.released << '\n';
    std::cout << "markup census workers=" << workers << " frames=" << frames << " services=" << measured_services
        << " total_ns=" << cost.nanoseconds << " scheduler_ns=" << cost.service_nanoseconds << " max_ns=" << maximum_census
        << " objects=" << cost.objects << " edges=" << cost.edges << " entries=" << cost.entries
        << " max_objects=" << maximum_objects << " max_edges=" << maximum_edges << " max_entries=" << maximum_entries
        << " censuses=" << cost.censuses << " released=" << cost.released << '\n';
    if (missing_setup) {
        ai::GalacticWorld without_file(missing_setup->perception);
        must(without_file.bind(root.game()->galaxy().snapshot(), {}, *executor, world->synchronized_frame(), root.game()->galaxy().next_entity_id()));
        const auto hints = must(ai::EquationSet::parse({{"missing-hint.xml",
            "<Equations><Hint>Variable_Target.Hints.PriorityTarget</Hint><Raw>Variable_Target.Markup</Raw></Equations>"}},
            [](std::string_view, std::string_view) -> std::optional<ai::Real> { return {}; }));
        const auto empire = std::find_if(selected.begin(), selected.end(), [&](const auto& player) {
            return !missing_setup->perception.markup_hints.contains(player.player);
        });
        if (empire == selected.end()) throw std::runtime_error("missing markup observer absent");
        ai::Context context; context.player = empire->player;
        context.target = engine->target_of_object(missing_setup->perception.planets.front().planet);
        ai::AiRandom random; random.set_seed(42);
        expect(context.target && without_file.perception(*engine, *hints.find("RAW"), context, random) == ai::Real{}
            && !without_file.perception(*engine, *hints.find("HINT"), context, random),
            "installed player without a markup file fails Hints evaluation rather than producing numeric zero");
    }
    const auto state = world->markup()->state();
    expect(!state.handles.empty(), "unmodified stock PathBonus plan actually writes retained markup");
    for (const auto& player : selected) {
        bool marked{};
        for (const auto& [key, value] : state.values) if (key.observer == player.player && !key.player_target) {
            const auto* target = engine->target_of_object(key.target); if (!target) continue;
            ai::Context context; context.player = player.player; context.enemy = player.enemy; context.human = player.human; context.target = target;
            ai::AiRandom random; random.set_seed(42);
            expect(world->perception(*engine, *raw, context, random) == value, "installed Variable_Target.Markup equals retained authored-plan contribution");
            marked = marked || value != ai::Real{};
        }
        expect(marked, "each installed stock AI has a nonzero planet markup reader witness");
    }
    const auto saved = must(world->capture());
    ai::GalacticWorld restored(setup->perception);
    must(restored.bind(root.game()->galaxy().snapshot(), {}, *executor, world->synchronized_frame(), root.game()->galaxy().next_entity_id()));
    must(restored.restore(saved)); expect(restored.markup()->encode() == world->markup()->encode(), "installed native save retains authored-plan values and handles");
    return must(world->state_hash());
}
}
int case_galactic_ai_markup_stock_tests(int argc, char** argv) {
    try {
        const bool drift = argc > 1 && std::string_view(argv[1]) == "--drift";
        const auto directory = std::filesystem::path(argc > 2 ? argv[2] : "out/galactic-ai-markup");
        const auto frames = argc > 3 ? std::stoull(argv[3]) : drift ? 27000U : 600U;
        std::vector<eawr::vfs::MountSpec> mounts; const auto root = game_root();
        for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
            mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
        const auto files = must(eawr::vfs::Vfs::mount(mounts));
        const auto catalog = must(eawr::data::load_catalog(files, eawr::data::Profile::foc));
        const auto data = must(eawr::data::galactic::load_galactic_data(files, catalog.catalog));
        const auto types = eawr::units::galactic_type_table(catalog.catalog);
        std::string baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            if (drift && workers != 1) continue;
            const auto digest = run(files, catalog.catalog, data, types, frames, workers, directory);
            if (workers == 1) baseline = digest; else expect(digest == baseline, "installed markup state equal on 1/2/4/8 workers");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
