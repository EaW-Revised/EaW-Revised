#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/story_plots.hpp"
#include "eawr/campaign/story_rewards_live.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/units/unit_tables_galactic.hpp"
#include "eawr/vfs/vfs.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// #683 on the installed Equal Footing campaign (WGS-07): the plots load for each active player, the events fire in
// the expected order, their heroes, unique units and unlocks appear in the galaxy (part 2: WST-40/41/64), and the
// plot state is equal on 1/2/4/8 workers.
namespace {
namespace st = eawr::sim::galactic::story;
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using eawr::testing::must;
using galactic_test::expect;
using Names = std::vector<std::string>;

constexpr std::string_view campaign_name = "Sandbox_Equal_Footing_Underworld";

struct Installed {
    std::shared_ptr<const eawr::vfs::Vfs> files;
    eawr::data::LoadResult loaded;
    eawr::data::galactic::GalacticData data;
    eawr::units::GalacticTypeTable types;
    cp::CampaignSetupOptions options;
    static std::shared_ptr<const eawr::vfs::Vfs> mount() {
        const auto root = eawr::testing::game_root();
        if (!root) throw std::runtime_error("story engine stock tests need EAWR_EAW_GAME_ROOT");
        std::vector<eawr::vfs::MountSpec> mounts;
        for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
            mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
        return std::make_shared<const eawr::vfs::Vfs>(must(eawr::vfs::Vfs::mount(mounts)));
    }
    Installed() : files(mount()), loaded(must(eawr::data::load_catalog(*files, eawr::data::Profile::foc))),
        data(must(eawr::data::galactic::load_galactic_data(*files, loaded.catalog))),
        types(eawr::units::galactic_type_table(loaded.catalog)) {
        const auto* source = data.campaign(campaign_name);
        if (!source) throw std::runtime_error("installed Equal Footing campaign missing");
        options.seed = 42;
        for (const auto& faction : data.factions) { // the viewer's layout: every faction a seat, one human
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
    [[nodiscard]] gc::PlayerId player_of(const cp::CampaignSetupPlan& plan, std::string_view faction) const {
        for (const auto& binding : plan.player_bindings()) if (binding.faction == faction) return binding.player.player_id;
        throw std::runtime_error("no such player");
    }
};
// The first reward parameter of each record: the unit or hero a plot grants.
Names granted(const std::vector<st::Triggered>& rows) {
    Names out;
    for (const auto& row : rows) out.push_back(row.reward_params[0].empty() ? row.event : row.reward_params[0].front());
    return out;
}
// expect() that prints what came out when it differs.
void expect_granted(const std::vector<st::Triggered>& rows, const Names& wanted, std::string_view message) {
    const auto actual = granted(rows);
    if (actual != wanted) {
        std::cerr << "actual:";
        for (const auto& name : actual) std::cerr << ' ' << name;
        std::cerr << std::endl;
    }
    expect(actual == wanted, message);
}

// Live (not dead) objects of one original type that a player owns, from the published census.
std::size_t held(const gc::GalacticSnapshot& snapshot, const cp::CampaignSetupPlan& plan, gc::PlayerId owner, std::string_view name) {
    gc::TypeId id{};
    for (const auto& binding : plan.type_bindings()) if (binding.original_type == name) id = binding.id;
    if (!id) throw std::runtime_error("no such original type " + std::string(name));
    std::size_t count{};
    for (const auto& object : snapshot.actor_records()) if (!object.dead && object.owner == owner && object.original_type == id) ++count;
    return count;
}
gc::TypeId type_id(const cp::CampaignSetupPlan& plan, std::string_view name) {
    for (const auto& binding : plan.type_bindings()) if (binding.original_type == name) return binding.id;
    throw std::runtime_error("no such original type " + std::string(name));
}
bool listed(const std::vector<gc::TypeId>& ids, gc::TypeId id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }

void plots_per_faction(const Installed& installed) {
    const auto plan = installed.plan();
    auto plots = must(cp::load_story_plots(*installed.files, installed.data, plan));
    const auto rebel = installed.player_of(plan, "Rebel"), empire = installed.player_of(plan, "Empire"), underworld = installed.player_of(plan, "Underworld");
    expect(plots.plots.size() == 3 && plots.plots[0].owner == rebel && plots.plots[1].owner == empire && plots.plots[2].owner == underworld,
        "WGS-07 the Rebel, Empire and Underworld plots load for their players, in that order");
    expect(plots.subplots.size() == 3 && plots.subplots[0].owner == rebel && plots.subplots[1].owner == empire && plots.subplots[2].owner == underworld,
        "WST-03 each sub-plot belongs to the player its plot was loaded for");
    for (const auto& note : plots.notes) std::cout << "story load note: " << note << '\n';
    expect(plots.notes.empty(), "the stock plots load without a skipped file or dropped event");
    auto engine = must(st::Engine::create({}, std::move(plots.plots), std::move(plots.subplots)));
    expect(engine.diagnostics().empty() && engine.unavailable().empty(), "the sandbox plots use only evaluated events and known rewards");

    // Tech levels set up before the first service reach the plots first; none are set here.
    must(engine.service(eawr::sim::math::Fixed{}, false));
    expect_granted(engine.take_triggered(), (Names{"Rebel_X-Wing_Squadron", "Mon_Mothma_Team", "Droids_Team",
        "E_Ground_Research_Facility", "Grand_Moff_Tarkin_Team", "Emperor_Palpatine_Team"}),
        "start: the X-wing unlock, Mon Mothma and the droids; the Empire's research facility, Tarkin and Palpatine");
    must(engine.service(eawr::sim::math::Fixed::from_raw(eawr::sim::math::Fixed::scale), false));
    expect_granted(engine.take_triggered(), (Names{"UNIVERSAL_STORY_START", "Tyber_Zann_Team", "Bossk_Team", "Urai_Fen_Team", "IG88_Team", "Silri_Team"}),
        "the Underworld plot starts after one second (its intro goal record), then Tyber, Bossk, Urai Fen, IG-88 and Silri");
    st::EconomyNotice tech;
    tech.kind = st::Economy::tech_level; tech.value = 3;
    must(engine.notify(rebel, tech));
    expect_granted(engine.take_triggered(), (Names{"Han_Solo_Team", "Sundered_Heart", "Obi_Wan_Team", "Rogue_Squadron_Space",
        "Home_One", "Yoda_Team"}), "Rebel tech 3 releases Han and the Sundered Heart, Obi-Wan and Rogue Squadron, Home One and Yoda");
    must(engine.notify(empire, tech));
    expect_granted(engine.take_triggered(), (Names{"Boba_Fett_Team", "General_Veers_Team", "Accuser_Star_Destroyer"}),
        "Empire tech 3 releases Boba Fett, Veers and the Accuser");
    tech.value = 5;
    must(engine.notify(empire, tech));
    expect_granted(engine.take_triggered(), (Names{"Arc_Hammer", "Admonitor_Star_Destroyer"}), "Empire tech 5 releases the Arc Hammer and the Admonitor");
    must(engine.notify(rebel, tech));
    expect_granted(engine.take_triggered(), (Names{"Garm_Bel_Iblis_Team", "Luke_Skywalker_Jedi_Team"}), "Rebel tech 5 releases Garm and Luke");
    must(engine.notify(underworld, tech));
    expect(engine.take_triggered().empty(), "the Underworld plot has no tech events");
}

// The plots inside the campaign's own GameSession, stepped on 1/2/4/8 workers.
void session(const Installed& installed) {
    std::vector<std::string> baseline;
    std::string baseline_holocron;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const auto plan = installed.plan();
        const auto services = must(cp::prepare_live_campaign_services(*installed.files, installed.loaded.catalog, installed.data, installed.types, plan, "Normal_Default"));
        auto root = must(cp::CampaignRoot::create(plan, services));
        const auto space = must(cp::prepare_stock_space_entry(*installed.files, installed.loaded.catalog, installed.data, installed.types, *root.plan()));
        must(cp::configure_live_campaign_conflicts(root, *installed.files, installed.loaded.catalog, installed.data, installed.types, space));
        auto* game = root.game();
        expect(game && game->story_engine() && game->story_engine()->plots().size() == 3, "the live campaign installs its three plots");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> trail;
        std::uint64_t underworld_tick{};
        std::size_t refused{};
        std::string holocron; // WST-58: the stock intro goal and its summary request, as published
        std::size_t opens{};
        for (int i = 0; i < 40; ++i) {
            auto tick = must(game->step(executor));
            for (const auto& event : tick.story_presentation) {
                if (const auto* goal = std::get_if<st::GoalChanged>(&event.payload)) holocron += "goal " + goal->tag + " @" + std::to_string(tick.completed_tick) + ";";
                if (const auto* summary = std::get_if<st::SummaryRequest>(&event.payload)) {
                    holocron += "summary " + summary->tag + " chapter " + std::to_string(summary->chapter) + ";";
                    opens += summary->action == st::SummaryAction::open;
                }
            }
            for (const auto& row : tick.story_triggers) {
                trail.push_back(row.event);
                if (row.event == "SPAWN_TYBER") underworld_tick = tick.completed_tick;
                if (row.outcome.rfind("refused", 0) == 0 && (row.reward_type == "SPAWN_HERO" || row.reward_type == "UNIQUE_UNIT" || row.reward_type == "BUILDABLE_UNIT")) {
                    std::cerr << "refused " << row.event << " (" << row.reward_type << "): " << row.outcome << '\n'; ++refused;
                }
            }
            if (i == 0) expect(std::any_of(tick.story_triggers.begin(), tick.story_triggers.end(),
                [](const auto& row) { return row.event == "UNLOCK_X_WINGS"; }), "the Rebel plot's start event fires at the first service");
        }
        expect(underworld_tick > 25 && underworld_tick < 40, "the Underworld plot starts after about thirty frames");
        expect(game->story_engine()->triggered("Story_Sandbox_56_Underworld.XML", "Spawn_Tyber"), "Tyber is granted");
        expect(refused == 0, "no unit or unlock reward of the start events is refused");
        expect(opens == 1 && holocron.find("goal ") == holocron.rfind("goal "),
            "WST-58 the stock intro holocron goal appears once and opens its summary once");
        const auto* intro = game->story_goals(installed.player_of(plan, "Underworld"));
        expect(intro && intro->goals.size() == 1 && !intro->goals.front().remove_after_viewed, "WST-58 the stock intro goal is the tagged one and stays");
        std::cout << "story stock holocron workers=" << workers << ' ' << holocron << std::endl;
        // Part 2 (WST-40/64): the start heroes are in the galaxy and the X-wing unlock locks the Z-95.
        const auto rebel = installed.player_of(plan, "Rebel"), empire = installed.player_of(plan, "Empire"), underworld = installed.player_of(plan, "Underworld");
        auto snapshot = game->galaxy().snapshot();
        for (const auto& [owner, name] : {std::pair{empire, "Grand_Moff_Tarkin_Team"}, std::pair{empire, "Emperor_Palpatine_Team"},
                std::pair{rebel, "Mon_Mothma_Team"}, std::pair{rebel, "Droids_Team"}, std::pair{underworld, "Tyber_Zann_Team"},
                std::pair{underworld, "Bossk_Team"}, std::pair{underworld, "Urai_Fen_Team"}, std::pair{underworld, "IG88_Team"},
                std::pair{underworld, "Silri_Team"}}) {
            const auto count = held(*snapshot, plan, owner, name);
            if (count != 1) std::cerr << name << " held " << count << '\n';
            expect(count == 1, std::string(name) + " is placed once for its owner");
        }
        const auto locks = [&](gc::PlayerId owner) -> const gc::ProductionPlayer& {
            for (const auto& row : snapshot->production()->players) if (row.player == owner) return row;
            throw std::runtime_error("no production player");
        };
        expect(listed(locks(rebel).unlocked, type_id(plan, "Rebel_X-Wing_Squadron")) && listed(locks(rebel).locked, type_id(plan, "Z95_Headhunter_Rebel_Squadron")),
            "WST-64 the X-wing unlock lists the squadron and locks the Z-95");
        // The tech heroes arrive when the owner's level reaches their event (Boba at 1, Veers at 2).
        std::int64_t level{};
        for (const auto& row : snapshot->player_economy()->players) if (row.player == empire) level = row.current_tech;
        for (const auto& [wanted, name] : {std::pair{std::int64_t{1}, "Boba_Fett_Team"}, std::pair{std::int64_t{2}, "General_Veers_Team"}}) {
            if (level < wanted) {
                expect(held(*game->galaxy().snapshot(), plan, empire, name) == 0, std::string(name) + " waits for its tech level");
                must(game->set_technology({empire, wanted, true, true, gc::GalacticSession::TechnologyMaximum::keep}));
                must(game->step(executor));
                level = wanted;
            } else must(game->step(executor));
            expect(held(*game->galaxy().snapshot(), plan, empire, name) == 1, std::string(name) + " is placed when the level is reached");
        }
        // The journal alone reproduces it: a fresh session replays the recording with only the reward replayer bound.
        const auto recording = game->record();
        auto copy = must(cp::GameSession::from_replay(recording));
        must(cp::bind_story_rewards(copy, plan, installed.data));
        while (copy.completed_tick() < recording.final_tick_count) must(copy.step(executor));
        expect(copy.galaxy().state_sha256() == game->galaxy().state_sha256(), "the native journal reconstructs the story heroes, locks and levels");
        const auto digest = game->story_engine()->digest();
        std::cout << "story stock workers=" << workers << " triggers=" << trail.size() << " digest=" << digest << '\n';
        if (workers == 1) { baseline = {digest, game->state_sha256()}; baseline_holocron = holocron; }
        else {
            expect(baseline == (std::vector<std::string>{digest, game->state_sha256()}), "1/2/4/8 workers give equal plot and session digests");
            expect(baseline_holocron == holocron, "1/2/4/8 workers publish the same holocron requests on the same ticks");
        }
    }
}
} // namespace

int case_galactic_story_engine_stock_tests() {
    try {
        const Installed installed;
        plots_per_faction(installed);
        session(installed);
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
