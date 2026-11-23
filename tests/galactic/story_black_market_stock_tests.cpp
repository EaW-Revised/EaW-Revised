#include "../support/env.hpp"
#include "../support/result.hpp"
#include "../support/temp_dir.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/planet_economy.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/story_rewards_live.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/units/unit_tables_galactic.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <string>
#include <vector>

// The GIVE_BLACK_MARKET story reward (WGEP-139) through the live reward handler on the installed Equal Footing campaign.
// The test replaces the Underworld plot with two events that both fire one second in: one names the first item the
// campaign's own black-market list registers, one names an item no list holds. The first must add that item to exactly one
// player's unlock list and journal one grant; the second must be declined with its reason and leave no journal entry; and
// the native journal alone (a fresh session with only the reward replayer bound, no item names) must reproduce the state.
namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using eawr::testing::must;
using galactic_test::expect;

constexpr std::string_view campaign_name = "Sandbox_Equal_Footing_Underworld";
constexpr std::uint8_t op_black_market = 5; // the story-reward journal operation of a black-market grant

std::vector<eawr::vfs::MountSpec> base_mounts() {
    const auto root = eawr::testing::game_root();
    if (!root) throw std::runtime_error("the black-market reward test needs EAWR_EAW_GAME_ROOT");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
    return mounts;
}

struct Installed {
    std::shared_ptr<const eawr::vfs::Vfs> files;
    eawr::data::LoadResult loaded;
    eawr::data::galactic::GalacticData data;
    eawr::units::GalacticTypeTable types;
    cp::CampaignSetupOptions options;
    explicit Installed(std::vector<eawr::vfs::MountSpec> mounts)
        : files(std::make_shared<const eawr::vfs::Vfs>(must(eawr::vfs::Vfs::mount(mounts)))),
          loaded(must(eawr::data::load_catalog(*files, eawr::data::Profile::foc))),
          data(must(eawr::data::galactic::load_galactic_data(*files, loaded.catalog))),
          types(eawr::units::galactic_type_table(loaded.catalog)) {
        const auto* source = data.campaign(campaign_name);
        if (!source) throw std::runtime_error("installed Equal Footing campaign missing");
        options.seed = 42;
        for (const auto& faction : data.factions) {
            const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
            const bool local = faction.name == source->starting_active_player;
            options.players.push_back({{id, id, 0}, faction.name, true});
            options.seats.push_back({gc::SeatId{id}, id, local ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, local ? id : 0});
            if (local) options.solo_reference_human = id;
        }
        std::vector<std::string> originals;
        for (const auto& type : types.types) originals.push_back(type.name);
        std::sort(originals.begin(), originals.end());
        for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
    }
    [[nodiscard]] cp::CampaignSetupPlan plan() const {
        return must(cp::plan_live_campaign_setup(*files, data, types, campaign_name, options, "Normal_Default"));
    }
};

constexpr std::string_view unregistered = "Not_A_Black_Market_Item_In_This_Game";

std::string plot_text(const std::string& registered) {
    const auto event = [](std::string name, const std::string& item) {
        return "\t<Event Name=\"" + name + "\">\n\t\t<Event_Type>STORY_ELAPSED</Event_Type>\n\t\t<Event_Param1>1</Event_Param1>\n"
            "\t\t<Reward_Type>GIVE_BLACK_MARKET</Reward_Type>\n\t\t<Reward_Param1>" + item + "</Reward_Param1>\n\t</Event>\n";
    };
    return "<?xml version=\"1.0\" ?>\n<Story>\n" + event("Give_Registered_Item", registered)
        + event("Give_Unregistered_Item", std::string(unregistered)) + "</Story>\n";
}

// Every player's unlock list, by player: the first difference names who gained what.
std::map<gc::PlayerId, std::set<std::uint32_t>> unlock_lists(const gc::GalacticSession& world) {
    std::map<gc::PlayerId, std::set<std::uint32_t>> lists;
    if (const auto* state = world.economy_corruption())
        for (const auto& row : state->players) lists[row.player] = row.unlocks;
    return lists;
}
} // namespace

int case_galactic_story_black_market_stock_tests() {
    try {
        const auto names = must(cp::load_black_market_item_names(*std::make_shared<const eawr::vfs::Vfs>(must(eawr::vfs::Vfs::mount(base_mounts())))));
        expect(!names.empty(), "the installed game registers black-market items");
        if (names.empty()) return 1;
        const std::string registered = names.front();
        constexpr std::uint32_t registered_id = 1; // the item id is its 1-based registration ordinal

        const auto directory = std::filesystem::current_path() / ("story-black-market-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
        std::filesystem::create_directories(directory / "xml");
        {
            std::ofstream out(directory / "xml" / "story_sandbox_56_underworld.xml", std::ios::binary);
            out << plot_text(registered);
            if (!out) throw std::runtime_error("plot overlay write failed");
        }
        auto layered = base_mounts();
        layered.insert(layered.begin(), {"black-market-reward-plot", directory, "data", {}});

        const Installed installed(layered);
        const auto plan = installed.plan();
        const auto services = must(cp::prepare_live_campaign_services(*installed.files, installed.loaded.catalog, installed.data, installed.types, plan, "Normal_Default"));
        auto root = must(cp::CampaignRoot::create(plan, services));
        const auto space = must(cp::prepare_stock_space_entry(*installed.files, installed.loaded.catalog, installed.data, installed.types, *root.plan()));
        must(cp::configure_live_campaign_conflicts(root, *installed.files, installed.loaded.catalog, installed.data, installed.types, space));
        auto* game = root.game();
        expect(game != nullptr, "the live campaign has a session");
        if (!game) return 1;
        const auto before = unlock_lists(game->galaxy());
        expect(!before.empty(), "the campaign carries the corruption economy players");
        for (const auto& [player, list] : before)
            expect(!list.contains(registered_id), "no player holds the item before the plot fires");

        eawr::platform::ThreadWorkerAdapter executor(2);
        std::string given, declined;
        for (int i = 0; i < 40; ++i) {
            auto tick = must(game->step(executor));
            for (const auto& row : tick.story_triggers) {
                if (row.event == "GIVE_REGISTERED_ITEM") given = row.outcome;
                if (row.event == "GIVE_UNREGISTERED_ITEM") declined = row.outcome;
            }
        }
        expect(given == "applied", "WGEP-139: the registered item reward applies (" + given + ")");
        expect(declined.rfind("refused:", 0) == 0 && declined.find("not a registered black-market item") != std::string::npos,
            "an unregistered item name is declined with its reason (" + declined + ")");

        const auto after = unlock_lists(game->galaxy());
        std::size_t gained{};
        for (const auto& [player, list] : after) {
            const auto old = before.find(player);
            const std::set<std::uint32_t> was = old == before.end() ? std::set<std::uint32_t>{} : old->second;
            if (list == was) continue;
            ++gained;
            std::set<std::uint32_t> expected = was;
            expected.insert(registered_id);
            expect(list == expected, "the owning player's unlock list gains only the named item");
        }
        expect(gained == 1, "exactly one player's unlock list changed");

        // One applied grant is journaled, the declined name leaves nothing behind.
        const auto recording = game->record();
        std::size_t grants{};
        for (const auto& effect : recording.native_effects) {
            if (effect.kind != gc::NativeEffectKind::story_reward || effect.payload.empty() || effect.payload.front() != op_black_market) continue;
            ++grants;
            expect(effect.payload.size() == 9, "the journaled grant carries one player and one item id");
            if (effect.payload.size() == 9) {
                std::uint32_t item{};
                for (unsigned i = 0; i < 4; ++i) item |= std::uint32_t{effect.payload[5 + i]} << (8 * i);
                expect(item == registered_id, "the journal holds the item's registration ordinal, not its name");
            }
        }
        expect(grants == 1, "exactly one black-market grant is journaled; the declined name has no journal entry");

        // The journal alone reproduces it: a fresh session replays the recording with only the reward replayer bound.
        auto copy = must(cp::GameSession::from_replay(recording));
        must(cp::bind_story_rewards(copy, plan, installed.data));
        while (copy.completed_tick() < recording.final_tick_count) must(copy.step(executor));
        expect(copy.galaxy().state_sha256() == game->galaxy().state_sha256(), "the native journal reconstructs the black-market grant");
        expect(unlock_lists(copy.galaxy()) == after, "the replayed session holds the same unlock lists");
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
