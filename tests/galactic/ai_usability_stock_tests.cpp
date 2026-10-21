#include <algorithm>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "galactic_ai_usability.hpp"
#include "galactic_ai_selection_content.hpp"
#include "galactic_ai.hpp"
#include "eawr/campaign/ai_selection_content.hpp"
#include "eawr/campaign/ai_perception_content.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace ai = eawr::script::foc::ai;
using galactic_test::expect;
using eawr::testing::result_diagnostic::take;
std::string game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("installed usability requires verified game data");
#if !defined(_MSC_VER)
    if (root->empty()) throw std::runtime_error("installed usability requires verified game data");
#endif
    return std::move(*root);
}
void installed() {
    const auto root = std::filesystem::path(game_root());
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    auto filesystem = take(eawr::vfs::Vfs::mount(mounts));
    const auto catalog = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    const auto data = take(eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog));
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    const auto* authored = data.campaign("Sandbox_Equal_Footing_Empire");
    if (!authored) throw std::runtime_error("stock campaign family unavailable");
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == authored->starting_active_player;
        if (human) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
    }
    std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
    const auto plan = take(cp::plan_live_campaign_setup(filesystem, data, types, authored->name, options, "Normal_Default"));
    const auto services = take(cp::prepare_live_campaign_services(filesystem, catalog.catalog, data, types, plan, "Normal_Default"));
    const auto selection = take(cp::prepare_ai_selection_content(filesystem, catalog.catalog, data, types, plan));
    auto perception = *take(cp::prepare_ai_perception_content(filesystem, data, plan, *selection, "Normal"));
    const auto* fog = data.constant("AIUsesFogOfWarGalactic");
    if (!fog) throw std::runtime_error("authored GC AI fog flag is absent");
    const auto flag = ai::upper_case(ai::trimmed(fog->text));
    if (flag != "TRUE" && flag != "FALSE") throw std::runtime_error("authored GC AI fog flag is invalid");
    expect(perception.uses_fog == (flag == "TRUE"), "stock helper consumes the authored GC AI fog flag");
    const auto immutable = take(cp::prepare_stock_galactic_ai_usability(data, plan, perception));
    eawr::script::foc::galactic_ai::LifecycleSetup lifecycle;
    for (const auto& path : eawr::script::foc::galactic_ai::required_xml()) {
        const auto bytes = take(filesystem.open(path));
        lifecycle.xml.emplace(path, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    const auto rules = take(eawr::script::foc::galactic_ai::load_data(lifecycle));
    std::string reference;
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers); auto campaign = take(cp::CampaignRoot::create(plan, services));
        auto& session = campaign.game()->galaxy(); cp::GalacticAiUsabilityProvider provider(immutable);
        auto host = std::make_shared<eawr::script::foc::detail::Host>(); host->setup.content = selection->ai;
        ai::Engine engine(host, rules, {}); ai::GalacticWorld world(perception);
        const auto bound = world.bind(session.snapshot(), {}, executor, 0, session.next_entity_id());
        if (!bound) throw std::runtime_error(bound.error().message);
        std::ostringstream trace;
        const auto connectivity = take(provider.connectivity(session, world, engine, executor));
        for (const auto& fleet : session.snapshot()->fleets())
            expect(connectivity.stealth_fleets.contains(fleet.id) == take(session.snapshot()->fleet_combat_skip(fleet.id)),
                "installed connectivity publishes the owning live fleet predicate");
        for (const auto id : connectivity.stealth_fleets) trace << "stealth:" << id << ';';
        std::size_t ai_players{}, usable_planets{};
        for (const auto& player : perception.players) if (player.ai) {
            ++ai_players;
            for (const auto& planet : session.snapshot()->planets()) {
                const auto usable = take(provider.usability(player.player, *session.snapshot(), planet.id));
                usable_planets += usable;
                trace << player.player << ':' << planet.id << ':' << usable << ';';
            }
            const auto input = take(provider.movement(player.player, *session.snapshot(), world, engine, executor));
            expect(input.access.size() == session.snapshot()->planets().size(), "stock supplier publishes one real PathPlanet per live planet");
            for (const auto& row : input.access) {
                expect(row.ai_classification_succeeded && row.ai_relation.has_value(), "stock target ownership uses qualified production perception");
                trace << row.id << ':' << static_cast<unsigned>(row.relation) << ':' << static_cast<unsigned>(*row.ai_relation)
                    << ':' << row.ai_usable << ':' << row.ai_enemy_space_force.value_or(true) << ';';
            }
        }
        expect(ai_players > 0 && usable_planets > 0, "installed campaign exercises real AI usability");
        expect(connectivity.goal_reach.size() == ai_players * session.snapshot()->planets().size() * 6
            && connectivity.island_paths.size() == ai_players * session.snapshot()->planets().size(), "stock complete site/island domain");
        for (const auto& row : connectivity.goal_reach) {
            trace << row.player << ':' << row.source << ':' << row.policy << ':';
            for (const auto id : row.production_sites) trace << id << ',';
            trace << ';';
        }
        for (const auto& row : connectivity.island_paths)
            trace << row.player << ':' << row.planet << ':' << row.least_force << ':' << row.largest << ';';
        const auto work = provider.work();
        for (unsigned repeat = 0; repeat < 10; ++repeat)
            expect(take(provider.connectivity(session, world, engine, executor)).revision == connectivity.revision, "all stock AI players reuse completed connectivity");
        expect(provider.work().generations == work.generations && provider.work().searches == work.searches,
            "another AI player's cache fills do not trigger repeated preparation");
        const auto native = take(provider.capture()); cp::GalacticAiUsabilityProvider restored(immutable);
        const auto restore = restored.restore(native); expect(static_cast<bool>(restore), "stock native provider restores");
        const auto continued = take(restored.connectivity(session, world, engine, executor));
        expect(continued.stealth_fleets == connectivity.stealth_fleets, "installed restore derives identical stealth fleets from saved actors");
        expect(continued.revision == connectivity.revision && take(restored.state_hash()) == take(provider.state_hash()), "stock provider save continuation equality");
        expect(continued.goal_reach.size() == connectivity.goal_reach.size(), "stock restore preserves complete policy row identities");
        for (std::size_t i = 0; i < std::min(continued.goal_reach.size(), connectivity.goal_reach.size()); ++i) {
            const auto& row = continued.goal_reach[i]; const auto& original = connectivity.goal_reach[i];
            expect(row.player == original.player && row.source == original.source && row.policy == original.policy,
                "stock restored policy identity/order is exact");
            expect(row.policy == "Single_Hop_Disconnected" ? row.production_sites.empty() : row.production_sites == original.production_sites,
                "B-U07 unsaved policy 4 is empty before TTL; archived stock policy output is exact");
        }
        trace << take(world.state_hash()) << ':' << session.state_sha256();
        if (reference.empty()) reference = trace.str(); else expect(reference == trace.str(), "installed stock output and owner hashes equal on 1/2/4/8");
    }
}
}
int case_galactic_ai_usability_stock_tests() {
    try { installed(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
