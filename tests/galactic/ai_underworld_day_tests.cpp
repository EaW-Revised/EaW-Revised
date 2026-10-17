// #3944: seeded idle-human day run of the stock Underworld AI (Sandbox_Equal_Footing_Empire, Underworld is an AI seat).
// Observation first: per-player production by authored type, credit track, funding categories, hostile moves.
#include "galactic_support.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "ai_campaign_gate.hpp"
#include "../../src/campaign/galactic_ai_usability.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/core/gc_profile.hpp"
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
struct Result {
    std::string hash;                     // state hash at the last tick, for the 1/2/4/8 worker equality
    std::map<std::string, std::uint64_t> counters;
    std::size_t refusal_rows{};           // distinct (player, name) refusal counters the engine published
};
// `observe` turns the engine's refusal counters on (gc_profile::enabled); off, they must stay empty.
Result run(std::uint64_t ticks, std::size_t workers, std::string_view campaign_name, bool report, bool probe = false, bool observe = true) {
    eawr::gc_profile::enabled.store(observe);
    const auto root = game_root();
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    auto files = must(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = must(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    const auto* source = data.campaign(campaign_name);
    galactic_campaign_gate::check(source != nullptr, "day campaign exists");
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
    std::map<gc::TypeId, std::string> type_names, type_classes;
    for (const auto& name : originals) {
        options.types.push_back({options.types.size() + 1, name});
        const auto id = static_cast<gc::TypeId>(options.types.size());
        type_names[id] = name;
        if (const auto* type = types.find(name)) type_classes[id] = type->object_class;
    }
    const auto plan = must(cp::plan_live_campaign_setup(files, data, types, campaign_name, options, "Normal_Default"));
    const auto* fog = data.constant("AIUsesFogOfWarGalactic");
    galactic_campaign_gate::check(fog, "stock AI fog policy exists");
    const auto setup = must(cp::prepare_stock_galactic_ai_runtime(files, loaded.catalog, data, types, plan, {},
        std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers),
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
        out.connectivity_publication = [provider](const auto& parent, auto& world, auto& engine, const auto& pool) { return provider->connectivity_publication(parent, world, engine, pool); };
        out.movement = [provider](auto player, const auto& snapshot, const auto& world, const auto& engine, const auto& pool) { return provider->movement(player, snapshot, world, engine, pool); };
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

    Result result;
    std::map<gc::PlayerId, std::map<std::string, std::array<std::uint64_t, 2>>> production; // started, completed per type
    std::map<gc::PlayerId, std::uint64_t> hostile, first_hostile, moves;
    std::map<gc::PlayerId, std::int64_t> peak_credits, final_credits;
    std::map<std::uint64_t, std::pair<gc::PlayerId, std::string>> build_owners;
    std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> seen_moves;
    std::map<gc::FleetId, gc::PlayerId> fleet_owners;
    for (const auto& fleet : root_owner.game()->galaxy().snapshot()->fleets()) fleet_owners[fleet.id] = fleet.owner;
    // The corruption readers binary-search these tables by id: setup validation keeps them in ascending planet order.
    const auto initial = root_owner.game()->galaxy().snapshot();
    if (const auto* corruption = initial->economy_corruption())
        galactic_campaign_gate::check(std::is_sorted(corruption->planets.begin(), corruption->planets.end(),
            [](const auto& a, const auto& b) { return a.planet < b.planet; }), "economy corruption planets are in ascending id order");
    std::string last_hash;
    const auto state_report = [&](std::uint64_t tick, const std::shared_ptr<const gc::GalacticSnapshot>& galaxy) {
        const auto& view = observed->runtimes.front();
        const auto engine = view.engine.lock(); const auto funding = view.funding.lock();
        if (const auto& economy = galaxy->player_economy())
            for (const auto& row : economy->players)
                std::cout << "UW_DAY credits tick=" << tick << " player=" << names[row.player] << " credits=" << row.credits.floor_to_integer() << std::endl;
        if (!engine) return;
        if (probe) {
            // Observation only: these reads go through the ordinary perception path and may warm its caches.
            std::uint64_t corrupted{};
            if (const auto* corruption = galaxy->economy_corruption())
                for (const auto& at : corruption->planets) corrupted += at.corruption != gc::corruption_fixture::Type::none;
            std::cout << "UW_DAY probe tick=" << tick << " corrupted_planets=" << corrupted << " transitions=" << galaxy->corruption_transitions().size() << std::endl;
            namespace ai = eawr::script::foc::ai;
            for (const auto& selected : setup->lifecycle.players) {
                const auto* targets = engine->target_list(selected.player);
                std::uint64_t can{}, should{}, unavailable{}, evaluated{};
                for (const auto id : targets ? *targets : std::vector<std::uint64_t>{}) {
                    const auto* target = engine->target(id); if (!target) continue;
                    ++evaluated;
                    const auto can_value = engine->evaluate("CAN_CORRUPT_PLANET", selected.player, target);
                    const auto should_value = engine->evaluate("SHOULD_CORRUPT_PLANET", selected.player, target);
                    if (!can_value || !should_value) { ++unavailable; continue; }
                    can += ai::Real{} < *can_value; should += ai::Real{} < *should_value;
                }
                const auto save = engine->evaluate("SHOULD_SAVE_FOR_CORRUPTION", selected.player, nullptr);
                std::cout << "UW_DAY probe tick=" << tick << " player=" << names[selected.player] << " targets=" << evaluated << " can_corrupt=" << can
                    << " should_corrupt=" << should << " unavailable=" << unavailable << " save_for_corruption="
                    << (save ? (ai::Real{} < *save ? "1" : "0") : "unavailable") << std::endl;
            }
        }
        for (const auto& [key, count] : engine->galactic_refusals())
            std::cout << "UW_DAY unavailable tick=" << tick << " player=" << names[key.first] << " " << key.second << " count=" << count << std::endl;
        for (const auto& selected : setup->lifecycle.players) {
            std::map<std::string, std::uint64_t> definitions;
            for (const auto& goal : engine->galactic_goals(selected.player)) ++definitions[goal.definition.empty() ? "(no plan)" : goal.definition];
            for (const auto& [name, count] : definitions)
                std::cout << "UW_DAY goal tick=" << tick << " player=" << names[selected.player] << " plan=" << name << " count=" << count << std::endl;
            if (!funding) continue;
            const auto ledger = funding->ledger(selected.player); if (!ledger) continue;
            for (const auto& [name, id] : engine->data().goal_categories) {
                const auto found = ledger->categories.find(id); if (found == ledger->categories.end()) continue;
                const auto& category = found->second;
                std::cout << "UW_DAY funding tick=" << tick << " player=" << names[selected.player] << " category=" << name
                    << " weight=" << (category.evaluated_weight ? std::to_string(category.evaluated_weight->raw()) : std::string("unavailable"))
                    << " share_raw=" << category.share.raw() << " available_cash=" << category.available_cash.floor_to_integer() << std::endl;
            }
        }
    };
    for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
        const auto state = must(root_owner.game()->step(*setup->executor));
        const auto& view = observed->runtimes.front();
        if (const auto actions = view.actions.lock())
            for (const auto& [build, purchase] : actions->purchases()) {
                const auto name = type_names.find(purchase.requested_type);
                build_owners[build] = {purchase.player, name == type_names.end() ? std::string("?") : name->second};
            }
        for (const auto& fleet : state.galaxy->fleets()) fleet_owners[fleet.id] = fleet.owner;
        for (const auto& event : state.galaxy->events()) {
            if (event.kind == gc::GalacticEventKind::production_started || event.kind == gc::GalacticEventKind::production_completed) {
                const auto found = build_owners.find(event.sequence);
                if (found == build_owners.end()) continue;
                ++production[found->second.first][found->second.second][event.kind == gc::GalacticEventKind::production_started ? 0 : 1];
            }
            if (event.kind != gc::GalacticEventKind::movement_begin || !seen_moves.emplace(event.tick, event.subject, event.value).second) continue;
            const auto planet = std::find_if(state.galaxy->planets().begin(), state.galaxy->planets().end(),
                [&](const auto& row) { return row.id == event.value; });
            const auto player = fleet_owners.find(event.subject);
            if (player == fleet_owners.end() || planet == state.galaxy->planets().end()) continue;
            ++moves[player->second];
            if (planet->owner != player->second) { ++hostile[player->second]; if (!first_hostile[player->second]) first_hostile[player->second] = event.tick; }
        }
        if (const auto& economy = state.galaxy->player_economy())
            for (const auto& row : economy->players) {
                peak_credits[row.player] = std::max<std::int64_t>(peak_credits[row.player], row.credits.floor_to_integer());
                final_credits[row.player] = row.credits.floor_to_integer();
            }
        if (tick == ticks) last_hash = state.state_hash.get();
        if (report && (tick == 1 || tick % (probe ? 300 : 600) == 0 || tick == ticks)) state_report(tick, state.galaxy);
    }
    result.hash = last_hash;
    for (const auto& [player, by_type] : production) for (const auto& [type, counts] : by_type) {
        const auto klass = [&] { for (const auto& [id, name] : type_names) if (name == type) return type_classes[id]; return std::string(); }();
        if (report) std::cout << "UW_DAY production player=" << names[player] << " type=" << type << " class=" << klass
            << " started=" << counts[0] << " completed=" << counts[1] << std::endl;
        result.counters[names[player] + ".started"] += counts[0];
        result.counters[names[player] + ".started." + klass] += counts[0];
        result.counters[names[player] + ".completed"] += counts[1];
    }
    for (const auto& [player, count] : moves) {
        if (report) std::cout << "UW_DAY moves player=" << names[player] << " all=" << count << " hostile=" << hostile[player]
            << " first_hostile=" << first_hostile[player] << " peak_credits=" << peak_credits[player] << std::endl;
        result.counters[names[player] + ".moves"] = count; result.counters[names[player] + ".hostile"] = hostile[player];
    }
    for (const auto& [player, peak] : peak_credits) result.counters[names[player] + ".peak_credits"] = static_cast<std::uint64_t>(std::max<std::int64_t>(peak, 0));
    for (const auto& [player, credits] : final_credits) result.counters[names[player] + ".final_credits"] = static_cast<std::uint64_t>(std::max<std::int64_t>(credits, 0));
    if (const auto engine = observed->runtimes.front().engine.lock()) {
        const auto refusals = engine->galactic_refusals();
        result.refusal_rows = refusals.size();
        for (const auto& [key, count] : refusals) {
            result.counters[names[key.first] + ".refusal." + key.second] = count;
            if (key.second.starts_with("budget:")) result.counters[names[key.first] + ".unavailable_budgets"] += count;
        }
    }
    return result;
}
int main_run(int argc, char** argv) {
    std::cout << "CTEST_FULL_OUTPUT" << std::endl;
    if (argc > 1 && std::string_view(argv[1]) == "--equality") {
        // 1/2/4/8 workers must agree on the whole composed state and the same production/move counters.
        const std::uint64_t ticks = argc > 2 ? std::stoull(argv[2]) : 1800;
        std::optional<Result> first;
        // Observer counters stay empty when not requested (gc_profile off): no refusal row at all.
        galactic_campaign_gate::check(run(300, 1, "Sandbox_Equal_Footing_Empire", false, false, false).refusal_rows == 0,
            "refusal counters are empty when observation is off");
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto current = run(ticks, workers, "Sandbox_Equal_Footing_Empire", false);
            std::cout << "UW_DAY equality workers=" << workers << " ticks=" << ticks << " hash=" << current.hash << std::endl;
            if (!first) first = std::move(current);
            else {
                galactic_campaign_gate::check(current.hash == first->hash, "composed state hash differs across worker counts");
                galactic_campaign_gate::check(current.counters == first->counters, "production/move/refusal counters differ across worker counts");
            }
        }
        return 0;
    }
    const std::uint64_t ticks = argc > 1 ? std::stoull(argv[1]) : 5400;
    const std::string campaign = argc > 2 ? argv[2] : "Sandbox_Equal_Footing_Empire";
    const bool probe = argc > 4 && std::string_view(argv[4]) == "probe";
    const auto base = run(ticks, 4, campaign, true, probe);
    std::cout << "UW_DAY final workers=4 ticks=" << ticks << " hash=" << base.hash << std::endl;
    // Seed 1, 4 workers, 5400 ticks. Thresholds are about half of what the fixed AI did in the observation run and far
    // from the unfixed baseline (no structures, no starbase, credits 8000 -> 10960 by this tick).
    const auto count = [&](const std::string& key) { const auto found = base.counters.find(key); return found == base.counters.end() ? std::uint64_t{} : found->second; };
    if (campaign == "Sandbox_Equal_Footing_Empire") {
        galactic_test::expect(count("Underworld.unavailable_budgets") == 0, "no Underworld budget equation ends unavailable");
        galactic_test::expect(count("Underworld.started.SpecialStructure") >= 4, "Underworld starts structures it authored");
        galactic_test::expect(count("Underworld.started.StarBase") >= 1, "Underworld starts a starbase");
        galactic_test::expect(count("Underworld.started.HeroCompany") >= 2, "Underworld buys saboteur teams for its deployment plan");
        galactic_test::expect(count("Underworld.started.SpaceUnit") >= 1, "Underworld buys space units");
        galactic_test::expect(count("Underworld.final_credits") < 4000, "Underworld spends its credits instead of hoarding them");
        galactic_test::expect(count("Underworld.moves") >= 100, "Underworld keeps moving fleets");
        galactic_test::expect(count("Rebel.started.StarBase") >= 2 && count("Rebel.started.SpecialStructure") >= 8, "Rebel keeps building");
    }
    // "observe": deliberately nonzero so ctest prints the counters of a passing run (observation only).
    if (argc > 3 && std::string_view(argv[3]) == "observe") return 1;
    return galactic_test::failures == 0 ? 0 : 1;
}
} // namespace
int main(int argc, char** argv) { try { return main_run(argc, argv); } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; } }
