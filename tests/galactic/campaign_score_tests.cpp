#include "galactic_support.hpp"
#include "galactic/campaign_score_model.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/campaign_end_inputs.hpp"
#include "eawr/platform/campaign_end_provider.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>

// GHUD-S48/H120: the campaign score screen's inputs and texts. Covers the authored
// faction provider, the dialog captions for a local win and loss as E/R/U, absent stats
// as zero, the quit request, and the simulation hash staying independent of the
// presentation picture draw (WGP-37/38/97/111).
namespace {
using namespace galactic_test;
namespace score = eawr::viewer::galactic;
namespace platform = eawr::platform;
namespace fs = std::filesystem;

struct Tree {
    fs::path root = fs::temp_directory_path()
        / ("eawr-score-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Tree() { fs::create_directories(root); }
    ~Tree() { std::error_code ignored; fs::remove_all(root, ignored); }
};
void write(const fs::path& path, const std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}
// One stock-shaped faction per row; the Underworld-style faction authors a second finale.
constexpr std::string_view factions_xml = R"xml(<Factions>
<Faction Name="EMPIRE">
<Victory_Text>TEXT_E_VICTORY</Victory_Text><Defeat_Text>TEXT_E_DEFEAT</Defeat_Text>
<Space_Win_Image>e_space_win.tga</Space_Win_Image><Land_Win_Image>e_land_win.tga</Land_Win_Image>
<Space_Lose_Image>e_space_lose.tga</Space_Lose_Image><Land_Lose_Image>e_land_lose.tga</Land_Lose_Image>
<Generic_Win_Movie>E_Win</Generic_Win_Movie><Superweapon_Win_Movie>E_Super</Superweapon_Win_Movie>
<Finale_Movie>E_Finale</Finale_Movie><Post_Credits_Movie>E_Post</Post_Credits_Movie>
</Faction>
<Faction Name="REBEL">
<Victory_Text>TEXT_R_VICTORY</Victory_Text><Defeat_Text>TEXT_R_DEFEAT</Defeat_Text>
<Space_Win_Image>r_space_win.tga</Space_Win_Image><Land_Win_Image>r_land_win.tga</Land_Win_Image>
<Space_Lose_Image>r_space_lose.tga</Space_Lose_Image><Land_Lose_Image>r_land_lose.tga</Land_Lose_Image>
<Generic_Win_Movie>R_Win</Generic_Win_Movie><Finale_Movie>R_Finale</Finale_Movie>
</Faction>
<Faction Name="UNDERWORLD">
<Victory_Text>TEXT_U_VICTORY</Victory_Text><Defeat_Text>TEXT_U_DEFEAT</Defeat_Text>
<Space_Win_Image>u_space_win.tga</Space_Win_Image><Land_Win_Image>u_land_win.tga</Land_Win_Image>
<Space_Lose_Image>u_space_lose.tga</Space_Lose_Image><Land_Lose_Image>u_land_lose.tga</Land_Lose_Image>
<Finale_Movie>U_Finale</Finale_Movie><Finale_Movie_2>U_Finale_2</Finale_Movie_2>
</Faction>
<Faction Name="BARE"/>
</Factions>)xml";

galactic::GalacticSetup ending(std::string_view local_faction, std::string_view other_faction, bool local_wins) {
    auto initial = galactic::synthetic_galaxy({.seed = 91, .planets = 16, .players = 3,
        .fleets_per_player = 2, .members_per_fleet = 2, .debug_commands = true, .coop_seat = true});
    initial.seats = {{{1}, 1, galactic::SeatKind::human, 1, true, 1},
        {{2}, 2, galactic::SeatKind::human, 2, true, 2}, {{3}, 1, galactic::SeatKind::human, 1, true, 1}};
    galactic::VictoryContext context;
    context.players = {{1, std::string(local_faction), false, true, true},
        {2, std::string(other_faction), false, true, true}, {3, "Neutral", true, false}};
    for (const auto& planet : initial.planets)
        context.planets.push_back({planet.id, 1001, planet.owner, planet.owner, planet.owner, false});
    context.types = {{"Fixture_Planet", 1001}};
    for (const auto& a : context.players) for (const auto& b : context.players)
        context.relations.push_back({a.player, b.player, a.player == b.player});
    galactic::VictoryAuthoring authored;
    authored.human = {galactic::VictoryKind::credits};
    authored.ai = authored.human;
    initial.victory = galactic::install_victory(authored, context, 1);
    expect(galactic::register_victory(*initial.victory, galactic::VictoryKind::credits,
        local_wins ? 1 : 2, 0, 0), "score fixture registers a mature winner");
    return initial;
}

struct Ended {
    std::shared_ptr<const platform::CampaignEndView> view;
    std::string hash_before, hash_after;
};
Ended finish(const std::shared_ptr<const eawr::data::Catalog>& catalog, std::string_view local_faction,
    std::string_view other_faction, bool local_wins, std::size_t workers, bool scoring) {
    auto services = std::make_shared<platform::CampaignServiceConfiguration>();
    services->campaign_end_inputs = [catalog, scoring](const eawr::campaign::GameSession& game,
        const galactic::CampaignResult& result, galactic::PlayerId local) {
        auto inputs = platform::campaign_end_inputs_from_game(*catalog, game, result, local);
        if (!inputs || !scoring) return inputs;
        // A stand-in scoring provider: a title key, a present value, an absent control
        // and a present zero (WGP-97).
        auto value = std::move(inputs).value();
        value.title_key = "TEXT_TITLE_7";
        value.efficiencies = {0.82, std::nullopt, 0.0};
        return eawr::core::Result<platform::CampaignEndInputs>::success(std::move(value));
    };
    platform::LiveGameSession::Options options;
    options.workers = workers; options.pacing = platform::LiveGameSession::Pacing::driven;
    options.services = services;
    auto live = platform::LiveGameSession::start(ending(local_faction, other_faction, local_wins), options);
    expect(static_cast<bool>(live), "score fixture session starts");
    Ended ended;
    if (!live) return ended;
    ended.hash_before = live.value()->frame().latest->sha256();
    auto consumed = live.value()->consume_campaign_result({1}, false, false);
    expect(static_cast<bool>(consumed), "score fixture end is consumed");
    if (consumed) ended.view = consumed.value();
    ended.hash_after = live.value()->frame().latest->sha256();
    live.value()->stop();
    return ended;
}

void provider_and_captions() {
    Tree tree;
    for (const auto& [name, body] : std::map<std::string, std::string>{
             {"GameObjectFiles", "<Game_Object_Files><File>objects.xml</File></Game_Object_Files>"},
             {"HardpointDataFiles", "<Hard_Point_Files><File>hardpoints.xml</File></Hard_Point_Files>"},
             {"FactionFiles", "<Faction_Files><File>factions.xml</File></Faction_Files>"},
             {"CampaignFiles", "<Campaign_Files><File>campaigns.xml</File></Campaign_Files>"},
             {"SFXEventFiles", "<SFXEvent_Files><File>sfx.xml</File></SFXEvent_Files>"}})
        write(tree.root / "XML" / (name + ".xml"), body);
    write(tree.root / "XML" / "objects.xml", "<Objects/>");
    write(tree.root / "XML" / "hardpoints.xml", "<HardPoints/>");
    write(tree.root / "XML" / "factions.xml", factions_xml);
    write(tree.root / "XML" / "campaigns.xml", "<Campaigns/>");
    write(tree.root / "XML" / "sfx.xml", "<SFXEvents/>");
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "score data mounts");
    if (!mounted) return;
    auto loaded = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::eaw);
    expect(static_cast<bool>(loaded), "score data loads");
    if (!loaded) return;
    const auto catalog = std::make_shared<const eawr::data::Catalog>(std::move(loaded).value().catalog);

    // The authored half comes from the local faction; the movies from the winner (WGP-37/38/105).
    auto mixed = platform::campaign_end_inputs_from_catalog(*catalog, "EMPIRE", "UNDERWORLD");
    expect(mixed && mixed.value().victory_text == "TEXT_E_VICTORY" && mixed.value().defeat_text == "TEXT_E_DEFEAT"
        && mixed.value().space_win_image == "e_space_win.tga" && mixed.value().land_lose_image == "e_land_lose.tga"
        && mixed.value().finale_movie == "U_Finale" && mixed.value().second_finale_movie == "U_Finale_2"
        && mixed.value().generic_win_movie.empty() && !mixed.value().victory_movies_enabled
        && mixed.value().title_key.empty(),
        "local faction supplies score text and pictures, the winner its movies, the scoring half stays absent");
    auto bare = platform::campaign_end_inputs_from_catalog(*catalog, "BARE", "MISSING");
    expect(bare && bare.value().victory_text.empty() && bare.value().space_lose_image.empty()
        && bare.value().finale_movie.empty(), "absent tags and unresolved factions supply nothing and do not fail");

    struct Row { std::string_view local, other, win_text, lose_text, win_space, lose_land; };
    const std::array rows{
        Row{"EMPIRE", "REBEL", "TEXT_E_VICTORY", "TEXT_E_DEFEAT", "e_space_win.tga", "e_land_lose.tga"},
        Row{"REBEL", "EMPIRE", "TEXT_R_VICTORY", "TEXT_R_DEFEAT", "r_space_win.tga", "r_land_lose.tga"},
        Row{"UNDERWORLD", "EMPIRE", "TEXT_U_VICTORY", "TEXT_U_DEFEAT", "u_space_win.tga", "u_land_lose.tga"}};
    for (const auto& row : rows) {
        std::string reference_hash;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            const auto won = finish(catalog, row.local, row.other, true, workers, true);
            const auto lost = finish(catalog, row.local, row.other, false, workers, true);
            const auto bare_end = finish(catalog, row.local, row.other, true, workers, false);
            expect(won.view && lost.view && bare_end.view, "score fixtures produce an end view");
            if (!won.view || !lost.view || !bare_end.view) continue;
            expect(won.hash_before == won.hash_after && lost.hash_before == lost.hash_after,
                "consuming the end result leaves the simulation snapshot hash unchanged");
            if (reference_hash.empty()) reference_hash = won.hash_after;
            expect(won.hash_after == reference_hash, "end-state hash is equal on 1/2/4/8 workers");

            const auto win_space = score::campaign_score_captions(*won.view, true);
            const auto win_land = score::campaign_score_captions(*won.view, false);
            expect(score::campaign_score_shown(*won.view) && won.view->local_won
                && win_space.heading_key == "TEXT_VICTORY" && win_space.list_key == row.win_text
                && win_space.picture == row.win_space && win_space.button_key == "TEXT_BUTTON_MAIN_MENU"
                && win_space.title_key == "TEXT_TITLE_7",
                "local win: heading, authored victory text, win picture and main-menu button");
            expect(win_land.picture != win_space.picture && win_land.heading_key == win_space.heading_key
                && win_land.list_key == win_space.list_key && win_land.efficiencies == win_space.efficiencies,
                "the presentation draw chooses only the picture");
            expect(win_space.efficiencies[0] == "82" && win_space.efficiencies[1] == "0%"
                && win_space.efficiencies[2] == "0",
                "WGP-97: present value, absent control (0%) and present zero stay distinct");

            const auto lose = score::campaign_score_captions(*lost.view, false);
            expect(!lost.view->local_won && lose.heading_key == "TEXT_DEFEAT" && lose.list_key == row.lose_text
                && lose.picture == row.lose_land,
                "local loss: defeat heading, authored defeat text and lose picture");

            const auto absent = score::campaign_score_captions(*bare_end.view, true);
            expect(absent.title_key.empty() && absent.efficiencies == std::array<std::string, 3>{"0%", "0%", "0%"},
                "GHUD-S48: absent scoring provider shows zero for every stat and no title");

            // WGP-111, WGS-116/117: the quit request is the exit of the end owner's callback.
            platform::CampaignEndPresentation presentation(*won.view);
            expect(presentation.score_exit(false, true) == platform::CampaignScoreDestination::none
                && presentation.active(), "a non-topmost score does not act");
            expect(presentation.score_exit(true, true) == platform::CampaignScoreDestination::main_menu
                && !presentation.active(), "the button's request is the main-menu destination, once");
            expect(presentation.score_exit(true, true) == platform::CampaignScoreDestination::none,
                "the quit request is not repeated");
        }
    }

    // WGP-37: the continue caption and the multiplayer gate.
    platform::CampaignEndView lan;
    lan.route = platform::CampaignEndRoute::score; lan.ending_mode = platform::CampaignEndingMode::lan;
    expect(score::campaign_score_captions(lan, true).button_key == "TEXT_BUTTON_CONTINUE",
        "multiplayer score uses the continue caption");
    platform::CampaignEndView quit; quit.route = platform::CampaignEndRoute::multiplayer_quit;
    expect(!score::campaign_score_shown(quit), "ordinary multiplayer results show no score screen");
    quit.explicit_quit = true;
    expect(score::campaign_score_shown(quit), "an explicit quit may enter the score screen (WGS-118)");
}
} // namespace

int main() {
    provider_and_captions();
    if (failures != 0) { std::cerr << failures << " campaign score checks failed\n"; return 1; }
    std::cout << "campaign score checks passed\n";
    return 0;
}
