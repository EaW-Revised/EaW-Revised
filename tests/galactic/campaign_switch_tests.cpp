#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/campaign_switch.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/sim/galactic/synthetic.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// The LOAD_CAMPAIGN story reward (WST-95): the request a reward records, what resolves it, and the refusals. The
// simulation never restarts itself: the reward only publishes a CampaignSwitchRequest in its tick's output, a save
// refuses while it is pending, and the host settles it between ticks. The stock-data half is
// story_load_campaign_stock_tests.cpp.
namespace {
namespace cp = eawr::checkpoint;
namespace gc = eawr::sim::galactic;
namespace st = eawr::sim::galactic::story;
namespace ec = eawr::campaign;
using eawr::sim::math::Fixed;
using eawr::testing::take;
using galactic_test::expect;

eawr::data::galactic::GalacticData data_with_factions() {
    eawr::data::galactic::GalacticData data;
    for (const auto& [name, playable, neutral] : {std::tuple<const char*, bool, bool>{"Azure", true, false},
            {"Copper", true, false}, {"Rogue", false, true}, {"Husk", false, false}}) {
        eawr::data::galactic::Faction faction;
        faction.name = name; faction.is_playable = playable; faction.is_neutral = neutral;
        data.factions.push_back(std::move(faction));
    }
    for (const auto& [name, starter] : {std::pair<const char*, const char*>{"Solo_Azure", "Azure"}, {"Solo_Copper", "Copper"},
            {"Solo_Husk", "Husk"}, {"Solo_Nobody", ""}}) {
        eawr::data::galactic::Campaign campaign;
        campaign.name = name; campaign.starting_active_player = starter; campaign.is_multiplayer = false;
        data.campaigns.push_back(std::move(campaign));
    }
    return data;
}
ec::CampaignSwitchRequest request(std::string campaign, std::string side = {}, std::string difficulty = {}, std::string tech = {}) {
    ec::CampaignSwitchRequest out;
    out.campaign = std::move(campaign); out.side = std::move(side); out.difficulty = std::move(difficulty); out.tech = std::move(tech);
    return out;
}
bool refused_with(const eawr::core::Result<ec::CampaignSwitchTarget>& result, std::string_view text) {
    return !result && result.error().message.find(text) != std::string::npos
        && result.error().code == ec::campaign_switch_warning_code;
}

// The side is Reward_Param2 when given (a faction index of the data's table, or a name) and the entry's own human faction
// when absent; a side the entry does not seat, a neutral or unplayable faction and an unknown campaign are refused.
void resolution() {
    const auto data = data_with_factions();
    // Absent side: the entry's human faction. The campaign name matches in any case and resolves to its authored spelling.
    auto target = take(ec::resolve_campaign_switch(data, request("solo_azure")));
    expect(target.campaign == "Solo_Azure" && target.faction == "Azure" && target.difficulty == "normal" && !target.tech,
        "an absent side takes the entry's own human faction, an absent level reads Normal");
    // Side as a faction index: Reward_Param2 = 1 is the second faction of the table.
    target = take(ec::resolve_campaign_switch(data, request("Solo_Copper", "1", "2", "4")));
    expect(target.campaign == "Solo_Copper" && target.faction == "Copper" && target.difficulty == "hard" && target.tech == 4,
        "a side given as a faction index, the level index and the technology marker resolve");
    // Side as a name, in any case (the form the survival loader passes).
    target = take(ec::resolve_campaign_switch(data, request("Solo_Copper", "cOpPeR", "0")));
    expect(target.faction == "Copper" && target.difficulty == "easy" && !target.tech, "a side given as a faction name resolves in any case");
    target = take(ec::resolve_campaign_switch(data, request("Solo_Azure", "0", "Hard")));
    expect(target.faction == "Azure" && target.difficulty == "hard", "index 0 and the level's name resolve");
    // The level reads Normal for anything it cannot read; the marker only when it is a number.
    for (const char* level : {"", "1", "7", "-1", "nightmare"})
        expect(take(ec::resolve_campaign_switch(data, request("Solo_Azure", "", level))).difficulty == "normal",
            std::string("level ") + level + " reads Normal");
    expect(!take(ec::resolve_campaign_switch(data, request("Solo_Azure", "", "", "x"))).tech, "an unreadable technology marker is dropped");

    // Refusals, each named.
    expect(refused_with(ec::resolve_campaign_switch(data, request("")), "names no campaign"), "an empty campaign is refused");
    expect(refused_with(ec::resolve_campaign_switch(data, request("No_Such_Campaign", "0")), "unknown campaign No_Such_Campaign"),
        "an unknown campaign is refused");
    expect(refused_with(ec::resolve_campaign_switch(data, request("Solo_Azure", "1")), "side Copper is not playable in campaign Solo_Azure"),
        "a playable faction the entry does not seat is refused, not replaced");
    expect(refused_with(ec::resolve_campaign_switch(data, request("Solo_Azure", "Rogue")), "side Rogue is not playable"),
        "a neutral faction is refused");
    expect(refused_with(ec::resolve_campaign_switch(data, request("Solo_Husk", "Husk")), "side Husk is not playable"),
        "a faction the data does not let a player hold is refused");
    expect(refused_with(ec::resolve_campaign_switch(data, request("Solo_Husk")), "not playable"),
        "an entry whose starter is not playable is refused with the side absent");
    expect(refused_with(ec::resolve_campaign_switch(data, request("Solo_Nobody")), "seats no player"),
        "an entry with no starting faction is refused with the side absent");
    expect(refused_with(ec::resolve_campaign_switch(data, request("Solo_Azure", "9")), "not a faction index"),
        "a side index past the faction table is refused");
    expect(refused_with(ec::resolve_campaign_switch(data, request("Solo_Azure", "-1")), "not a faction index"), "a negative side index is refused");
    expect(refused_with(ec::resolve_campaign_switch(data, request("Solo_Azure", "Nobody")), "names no faction"), "an unknown side name is refused");
    expect(ec::campaign_switch_warning("because").rfind("EAWR-SCRIPT-0217 [warning]: LOAD_CAMPAIGN refused: because", 0) == 0,
        "the published text names the code, the severity and the reward");
}

// ---- the reward inside a session
st::SubPlotDef sub(std::string name, std::vector<st::EventDef> events) {
    st::SubPlotDef s;
    s.name = std::move(name); s.owner = 1; s.active = true; s.events = std::move(events);
    return s;
}
st::EventDef load(std::string name, const char* seconds, std::vector<std::string> campaign, std::vector<std::string> side = {},
    std::vector<std::string> level = {}, std::vector<std::string> tech = {}, bool perpetual = false) {
    st::EventDef e;
    e.name = std::move(name); e.type = "STORY_ELAPSED"; e.params[0] = {seconds}; e.perpetual = perpetual;
    e.reward_type = "LOAD_CAMPAIGN";
    e.reward_params[0] = std::move(campaign); e.reward_params[1] = std::move(side);
    e.reward_params[2] = std::move(level); e.reward_params[3] = std::move(tech);
    return e;
}
gc::GalacticSetup session_setup(bool multiplayer) {
    gc::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4;
    options.fleets_per_player = 1; options.members_per_fleet = 1;
    auto config = gc::synthetic_galaxy(options);
    config.rules.maximum_fleet_movement_distance = Fixed::from_raw(10000 * Fixed::scale);
    config.rules.fiscal_cycle_frames = 1000;
    if (multiplayer) galactic_test::mark_multiplayer(config);
    return config;
}
std::vector<st::SubPlotDef> plots(bool multiplayer) {
    if (multiplayer) return {sub("L.xml", {load("AGAIN", "1", {"Next_Campaign"}, {}, {}, {}, true)})};
    return {sub("L.xml", {load("FIRST", "1", {"Next_Campaign"}, {"1"}, {"2"}, {"4"}),
        load("SECOND", "2", {"Other_Campaign"}), load("UNNAMED", "3", {})})};
}
std::size_t warnings(const eawr::campaign::GameTick& tick) {
    return static_cast<std::size_t>(std::count_if(tick.diagnostics.begin(), tick.diagnostics.end(), [](const auto& row) {
        return row.code == ec::campaign_switch_warning_code && row.severity == eawr::core::Severity::warning; }));
}

void session_cases() {
    std::vector<std::string> baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const auto setup = session_setup(false);
        const auto ids = cp::game_identities(setup);
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(eawr::campaign::GameSession::create(setup));
        take(game.install_story({}, {{1, "l.xml"}}, plots(false)));
        std::map<std::string, std::string> outcome;
        std::optional<ec::CampaignSwitchRequest> published;
        std::uint64_t recorded_tick{};
        std::size_t published_count = 0, warned_second = 0, warned_unnamed = 0;
        bool saved_refused = false, frontier_refused = false, pending_matches = false, settled = false, saved_after = false;
        std::string refusal_text;
        for (int i = 0; i < 400 && warned_unnamed == 0; ++i) {
            auto tick = take(game.step(executor));
            for (const auto& row : tick.story_triggers) outcome[row.event] = row.outcome;
            if (tick.campaign_switch) {
                ++published_count; published = tick.campaign_switch; recorded_tick = tick.completed_tick;
                // The request is pending: the session cannot be saved, by the checkpoint or by the history frontier.
                pending_matches = game.pending_campaign_switch() == published;
                const auto saved = cp::save_game_checkpoint(game, ids);
                saved_refused = !saved && saved.error().message.find("a switch to campaign Next_Campaign is pending") != std::string::npos;
                frontier_refused = !game.history_frontier() && !game.checkpoint_recording();
            }
            if (outcome.contains("SECOND") && warned_second == 0) warned_second = warnings(tick);
            if (outcome.contains("SECOND") && !settled) {
                // The host settles the request between ticks: the session saves again and round-trips.
                const auto taken = game.settle_campaign_switch();
                settled = taken == published && !game.pending_campaign_switch();
                cp::GameLoadOptions options;
                options.configure_session = [&](eawr::campaign::GameSession& fresh) { return fresh.install_story({}, {{1, "l.xml"}}, plots(false)); };
                const auto saved = cp::save_game_checkpoint(game, ids);
                saved_after = static_cast<bool>(saved);
                if (saved) {
                    auto restored = take(cp::load_game_checkpoint(saved.value(), ids, options, executor));
                    saved_after = saved_after && restored.session.state_sha256() == game.state_sha256()
                        && !restored.session.pending_campaign_switch();
                }
            }
            if (outcome.contains("UNNAMED")) warned_unnamed = warnings(tick);
        }
        expect(published && published->campaign == "Next_Campaign" && published->side == "1" && published->difficulty == "2"
                && published->tech == "4" && published->tick == recorded_tick && published_count == 1,
            "the reward publishes its authored parameters once, in the step that fired it");
        expect(outcome["FIRST"] == "applied", "the recorded request is an applied reward");
        expect(pending_matches, "the session holds the request as pending until the host settles it");
        expect(saved_refused, "a save taken while a switch is pending is refused, by name");
        expect(frontier_refused, "the history frontier and the recording refuse too");
        expect(outcome["SECOND"].rfind("refused: a switch to campaign Next_Campaign is already pending", 0) == 0 && warned_second == 1,
            "a second request while one is pending is refused with a warning");
        expect(settled && saved_after, "once settled the session saves, loads and carries no request");
        expect(outcome["UNNAMED"].rfind("refused: the reward names no campaign", 0) == 0 && warned_unnamed == 1,
            "a reward that names no campaign is refused with a warning");
        const std::vector<std::string> trail{game.state_sha256(), std::to_string(recorded_tick)};
        if (workers == 1) baseline = trail;
        else expect(trail == baseline, "1/2/4/8 workers record the request on the same tick and reach the same state");
    }
}

// Multiplayer refuses the reward, logs it once per reason however often the plot fires it, and never holds a request.
void multiplayer_refusal() {
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const auto setup = session_setup(true);
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(eawr::campaign::GameSession::create(setup));
        take(game.install_story({}, {{1, "l.xml"}}, plots(true)));
        std::size_t fired = 0, warned = 0, published = 0;
        std::string last;
        for (int i = 0; i < 260; ++i) {
            const auto tick = take(game.step(executor));
            for (const auto& row : tick.story_triggers) { ++fired; last = row.outcome; }
            warned += warnings(tick);
            if (tick.campaign_switch) ++published;
        }
        expect(fired >= 3 && last == "refused: a multiplayer game cannot switch campaigns",
            "a multiplayer game refuses the reward every time its plot fires it");
        expect(warned == 1, "the refusal is logged once");
        expect(published == 0 && !game.pending_campaign_switch() && game.checkpoint_recording(), "no request is held, the game still saves");
    }
}
} // namespace

int case_galactic_campaign_switch_tests() {
    try {
        resolution();
        session_cases();
        multiplayer_refusal();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
