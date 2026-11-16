#include "../support/env.hpp"
#include "../support/result.hpp"
#include "../support/temp_dir.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/campaign_switch.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/mp_lobby_catalog.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/units/unit_tables_galactic.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The LOAD_CAMPAIGN story reward (WST-95) on installed data. The test replaces the Underworld plot of the stock Equal
// Footing campaign with five events that each ask for another campaign, discovering the targets from the installed
// single-player entries so no name is special:
//   1. entry A, its human faction as a faction INDEX (Reward_Param2), level 2 (hard)
//   2. entry B (in lower case), its human faction as a NAME, level 0 (easy)
//   3. entry A with no side and no level
//   4. entry A with the side of entry B (a side the entry does not seat)
//   5. a campaign the data does not have
// Each request is published in the tick that fired it and the session refuses a save while it is pending; the host side
// (resolve_campaign_switch) turns 1..3 into the campaign, side and level a start needs and refuses 4 and 5 by name. The
// accepted targets then go through the start path's setup planner with the difficulty entry the solo start picks (#7096),
// and the result must be equal on 1/2/4/8 workers.
namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using eawr::testing::must;
using galactic_test::expect;

constexpr std::string_view campaign_name = "Sandbox_Equal_Footing_Underworld";

std::vector<eawr::vfs::MountSpec> base_mounts() {
    const auto root = eawr::testing::game_root();
    if (!root) throw std::runtime_error("the load campaign test needs EAWR_EAW_GAME_ROOT");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
    return mounts;
}

std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
std::string upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

struct Installed {
    std::shared_ptr<const eawr::vfs::Vfs> files;
    eawr::data::LoadResult loaded;
    eawr::data::galactic::GalacticData data;
    eawr::units::GalacticTypeTable types;
    explicit Installed(std::vector<eawr::vfs::MountSpec> mounts)
        : files(std::make_shared<const eawr::vfs::Vfs>(must(eawr::vfs::Vfs::mount(mounts)))),
          loaded(must(eawr::data::load_catalog(*files, eawr::data::Profile::foc))),
          data(must(eawr::data::galactic::load_galactic_data(*files, loaded.catalog))),
          types(eawr::units::galactic_type_table(loaded.catalog)) {}
    // The options a solo start of `campaign` builds: every faction a player, the human one on the faction `local`.
    [[nodiscard]] cp::CampaignSetupOptions options(std::string_view campaign, std::string_view local) const {
        const auto* source = data.campaign(campaign);
        if (!source) throw std::runtime_error("installed campaign missing: " + std::string(campaign));
        cp::CampaignSetupOptions out;
        out.seed = 42;
        out.rules_flavour = cp::campaign_rules_flavour(*source);
        for (const auto& faction : data.factions) {
            const auto id = static_cast<gc::PlayerId>(out.players.size() + 1);
            const bool human = upper(faction.name) == upper(std::string(local));
            out.players.push_back({{id, id, 0}, faction.name, true});
            out.seats.push_back({gc::SeatId{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
            if (human) out.solo_reference_human = id;
        }
        std::vector<std::string> originals;
        for (const auto& type : types.types) originals.push_back(type.name);
        std::sort(originals.begin(), originals.end());
        for (const auto& name : originals) out.types.push_back({out.types.size() + 1, name});
        return out;
    }
    [[nodiscard]] cp::LobbyAiLevels levels() const {
        const auto table = eawr::skirmish::load_difficulty_table(*files);
        if (!table) return {};
        return [table](std::string_view type) { return table->level_entries(type); };
    }
};

struct Target { std::string campaign, faction; std::size_t index{}; };

// The first two single-player entries with different human factions.
std::pair<Target, Target> choose(const Installed& installed) {
    std::vector<Target> found;
    for (const auto& row : cp::list_coop_sp_lobby_campaigns(installed.data, installed.levels())) {
        const auto human = std::find_if(row.factions.begin(), row.factions.end(), [](const auto& f) { return f.playable && !f.neutral; });
        if (human == row.factions.end()) continue;
        std::size_t index = 0;
        while (index < installed.data.factions.size() && upper(installed.data.factions[index].name) != upper(human->name)) ++index;
        if (index == installed.data.factions.size()) continue;
        if (found.empty() || upper(found.front().faction) != upper(human->name)) found.push_back({row.name, installed.data.factions[index].name, index});
        if (found.size() == 2) return {found[0], found[1]};
    }
    throw std::runtime_error("the installed data needs two single-player entries with different human factions");
}

std::string plot_text(const Target& a, const Target& b) {
    const auto event = [](int second, const std::string& campaign, const std::string& side, const std::string& level) {
        return "\t<Event Name=\"Switch_" + std::to_string(second) + "\">\n\t\t<Event_Type>STORY_ELAPSED</Event_Type>\n\t\t<Event_Param1>"
            + std::to_string(second) + "</Event_Param1>\n\t\t<Reward_Type>LOAD_CAMPAIGN</Reward_Type>\n\t\t<Reward_Param1>" + campaign
            + "</Reward_Param1>\n\t\t<Reward_Param2>" + side + "</Reward_Param2>\n\t\t<Reward_Param3>" + level + "</Reward_Param3>\n\t</Event>\n";
    };
    return "<?xml version=\"1.0\" ?>\n<Story>\n" + event(1, a.campaign, std::to_string(a.index), "2")
        + event(2, lower(b.campaign), lower(b.faction), "0") + event(3, a.campaign, "", "")
        + event(4, a.campaign, b.faction, "1") + event(5, "Not_A_Campaign_In_This_Game", "", "1") + "</Story>\n";
}

struct Run { std::string state; std::vector<std::string> published, resolved; std::vector<std::string> outcomes; };

} // namespace

namespace {
int run_case() {
    try {
        const Installed plain(base_mounts());
        const auto [a, b] = choose(plain);
        std::cout << "load campaign stock: A " << a.campaign << " (" << a.faction << " #" << a.index << "), B " << b.campaign << " (" << b.faction << ")\n";

        const auto directory = std::filesystem::current_path() / ("load-campaign-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
        std::filesystem::create_directories(directory / "xml");
        {
            std::ofstream out(directory / "xml" / "story_sandbox_56_underworld.xml", std::ios::binary);
            out << plot_text(a, b);
            if (!out) throw std::runtime_error("plot overlay write failed");
        }
        auto layered = base_mounts();
        layered.insert(layered.begin(), {"load-campaign-plot", directory, "data", {}});

        std::vector<Run> runs;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const Installed installed(layered);
            const auto plan = must(cp::plan_live_campaign_setup(*installed.files, installed.data, installed.types, campaign_name,
                installed.options(campaign_name, installed.data.campaign(campaign_name)->starting_active_player), "Normal_Default"));
            const auto services = must(cp::prepare_live_campaign_services(*installed.files, installed.loaded.catalog, installed.data, installed.types, plan, "Normal_Default"));
            auto root = must(cp::CampaignRoot::create(plan, services));
            const auto space = must(cp::prepare_stock_space_entry(*installed.files, installed.loaded.catalog, installed.data, installed.types, *root.plan()));
            must(cp::configure_live_campaign_conflicts(root, *installed.files, installed.loaded.catalog, installed.data, installed.types, space));
            auto* game = root.game();
            expect(game != nullptr, "the live campaign has a session");
            if (!game) return 1;
            eawr::platform::ThreadWorkerAdapter executor(workers);
            Run run;
            std::vector<cp::CampaignSwitchTarget> accepted;
            for (int i = 0; i < 400 && run.published.size() < 5; ++i) {
                const auto tick = must(game->step(executor));
                for (const auto& row : tick.story_triggers)
                    if (row.event.rfind("SWITCH_", 0) == 0) run.outcomes.push_back(row.event + ": " + row.outcome);
                if (!tick.campaign_switch) continue;
                const auto& request = *tick.campaign_switch;
                run.published.push_back(request.campaign + "|" + request.side + "|" + request.difficulty + "|" + std::to_string(request.tick));
                expect(game->pending_campaign_switch() == tick.campaign_switch, "the session holds the request until the host settles it");
                expect(!game->checkpoint_recording(), "a pending switch refuses the save (" + request.campaign + ")");
                // The host's side: resolve against the installed data, then settle.
                const auto resolved = cp::resolve_campaign_switch(installed.data, request);
                if (resolved) {
                    accepted.push_back(resolved.value());
                    run.resolved.push_back(resolved.value().campaign + "|" + resolved.value().faction + "|" + resolved.value().difficulty);
                } else run.resolved.push_back("refused: " + resolved.error().message);
                static_cast<void>(game->settle_campaign_switch());
                expect(!game->pending_campaign_switch() && game->checkpoint_recording(), "once settled the session saves again");
            }
            run.state = game->galaxy().state_sha256();
            expect(run.published.size() == 5, "all five requests are published, one per firing tick");
            expect(run.outcomes.size() == 5 && std::all_of(run.outcomes.begin(), run.outcomes.end(), [](const auto& row) { return row.find(": applied") != std::string::npos; }),
                "the simulation records every request as applied: the host decides what the data allows");
            if (run.resolved.size() == 5) {
                expect(run.resolved[0] == a.campaign + "|" + a.faction + "|hard", "request 1: the index form picks entry A, its human side, hard (" + run.resolved[0] + ")");
                expect(run.resolved[1] == b.campaign + "|" + b.faction + "|easy", "request 2: the name form picks entry B, its human side, easy (" + run.resolved[1] + ")");
                expect(run.resolved[2] == a.campaign + "|" + a.faction + "|normal", "request 3: an absent side takes the entry's human faction, an absent level reads Normal (" + run.resolved[2] + ")");
                expect(run.resolved[3].find("refused:") == 0 && run.resolved[3].find("is not playable in campaign " + a.campaign) != std::string::npos,
                    "request 4: a side the entry does not seat is refused (" + run.resolved[3] + ")");
                expect(run.resolved[4].find("refused:") == 0 && run.resolved[4].find("unknown campaign") != std::string::npos,
                    "request 5: an unknown campaign is refused (" + run.resolved[4] + ")");
            }
            if (workers == 1) {
                // The one start path: the planner resolves the entry, the human side and the solo difficulty entry.
                const auto levels = installed.levels();
                for (const auto& target : accepted) {
                    const auto entry = cp::solo_difficulty_entry(installed.data, target.campaign, target.difficulty, levels);
                    const std::string stock = target.difficulty == "easy" ? "Easy_Default" : target.difficulty == "hard" ? "Hard_Default" : "Normal_Default";
                    expect(entry == stock, "the stock entry of " + target.campaign + " at " + target.difficulty + " is " + stock + " (" + entry + ")");
                    const auto start = must(cp::plan_live_campaign_setup(*installed.files, installed.data, installed.types, target.campaign,
                        installed.options(target.campaign, target.faction), entry));
                    expect(upper(std::string(start.campaign_name())) == upper(target.campaign), "the planner starts the named campaign (" + target.campaign + ")");
                    const auto human = start.solo_reference_human();
                    const auto binding = std::find_if(start.player_bindings().begin(), start.player_bindings().end(),
                        [&](const auto& row) { return human && row.player.player_id == *human; });
                    expect(binding != start.player_bindings().end() && upper(binding->faction) == upper(target.faction),
                        "the human seat of the started campaign is the resolved side (" + target.faction + ")");
                }
            }
            runs.push_back(std::move(run));
        }
        for (const auto& run : runs)
            expect(run.state == runs.front().state && run.published == runs.front().published && run.resolved == runs.front().resolved,
                "1/2/4/8 workers record the same requests on the same ticks and reach the same state");
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
} // namespace

int case_galactic_story_load_campaign_stock_tests() { return run_case(); }
