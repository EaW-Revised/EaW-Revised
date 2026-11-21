#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/story_plots.hpp"
#include "eawr/campaign/story_rewards_live.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/corruption_model.hpp"
#include "eawr/units/unit_tables_galactic.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
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
// WGCH-200/201/203: the live provider resolves planet types from the installed Consortium campaign plan.
void corruption_story_hooks(const Installed& installed) {
    const auto plan = installed.plan();
    const auto services = must(cp::prepare_live_campaign_services(*installed.files, installed.loaded.catalog, installed.data, installed.types, plan, "Normal_Default"));
    auto root = must(cp::CampaignRoot::create(plan, services));
    const auto config = root.game()->record().setup;
    const auto player = installed.player_of(plan, "Underworld");
    const auto seat = std::find_if(config.seats.begin(), config.seats.end(), [&](const auto& row) { return row.player == player; });
    expect(seat != config.seats.end() && config.ability_host && config.ability_host->initial_corruption.size() >= 2,
        "stock Consortium campaign supplies a seat and two initially corrupted planets");
    if (seat == config.seats.end() || !config.ability_host || config.ability_host->initial_corruption.size() < 2) return;
    const auto planet = config.ability_host->initial_corruption[0].planet;
    const auto other = config.ability_host->initial_corruption[1].planet;
    const auto name_of = [&](gc::PlanetId id) {
        for (const auto& location : plan.locations()) if (location.planet && location.planet->id == id) return location.original_type;
        throw std::runtime_error("stock corruption planet has no type name");
    };
    const auto reward = [&](std::string name, std::string type, std::string target, std::vector<std::string> flag = {}) {
        st::EventDef event; event.name = name; event.type = "STORY_CLICK_GUI"; event.params[0] = {name};
        event.reward_type = std::move(type); event.reward_params[0] = {std::move(target)}; event.reward_params[1] = std::move(flag);
        return event;
    };
    // A planet nobody corrupts at start, whose type is authored once, so "the first planet of that type" is unambiguous.
    std::optional<gc::PlanetId> clean;
    const auto& locations = plan.locations();
    for (const auto& location : locations) {
        if (!location.planet) continue;
        const auto id = location.planet->id;
        const auto same_type = std::count_if(locations.begin(), locations.end(),
            [&](const auto& candidate) { return candidate.original_type == location.original_type; });
        const bool corrupted = std::any_of(config.ability_host->initial_corruption.begin(), config.ability_host->initial_corruption.end(),
            [&](const auto& row) { return row.planet == id; });
        // It must author corruption content (a corruptible planet), so a decline cannot come from a planet without any.
        const auto content = std::find_if(config.ability_host->corruption_planets.begin(), config.ability_host->corruption_planets.end(),
            [&](const auto& row) { return row.planet == id; });
        const bool authored = content != config.ability_host->corruption_planets.end()
            && std::any_of(content->slots.begin(), content->slots.end(),
                [](const auto& slot) { return slot.type != gc::corruption_fixture::Type::none; });
        if (same_type == 1 && !corrupted && authored) { clean = id; break; }
    }
    expect(clean.has_value(), "the stock campaign has an uncorrupted planet of a unique type that authors corruption content");
    if (!clean) return;
    st::SubPlotDef plot; plot.name = "CorruptionPolicy.xml"; plot.owner = player; plot.active = true;
    plot.events = {reward("RESTRICT", "RESTRICT_CORRUPTION", name_of(planet)),
        reward("LIFT", "RESTRICT_CORRUPTION", name_of(planet), {"0"}),
        reward("ENABLE", "ENABLE_GALACTIC_CORRUPTION_HOLOGRAM", name_of(planet)),
        reward("DISABLE", "ENABLE_GALACTIC_CORRUPTION_HOLOGRAM", name_of(planet), {"0"}),
        reward("REMOVE", "REMOVE_CORRUPTION", name_of(planet)),
        reward("SHORT", "REMOVE_CORRUPTION", name_of(other), {"2.5"}),
        reward("MISSING", "RESTRICT_CORRUPTION", "UNKNOWN_PLANET"),
        reward("BAD_FLAG", "RESTRICT_CORRUPTION", name_of(planet), {"bad"}),
        reward("BAD_TIME", "REMOVE_CORRUPTION", name_of(planet), {"-1"}),
        reward("REMOVE_AGAIN", "REMOVE_CORRUPTION", name_of(planet)),
        reward("REMOVE_CLEAN", "REMOVE_CORRUPTION", name_of(*clean))};
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = must(cp::GameSession::create(config));
        must(game.install_story({}, {{player, "CorruptionPolicy.xml"}}, {plot}));
        must(cp::bind_story_rewards(game, plan, installed.data));
        std::uint64_t sequence{}; std::vector<std::string> hashes;
        const auto click = [&](const char* name, bool applied) {
            must(game.submit({{game.completed_tick(), seat->seat_id, ++sequence}, gc::GuiClick{name}}));
            const auto tick = must(game.step(executor)); hashes.push_back(tick.state_hash.get());
            expect(tick.story_triggers.size() == 1 && (tick.story_triggers[0].outcome == "applied") == applied,
                std::string("stock corruption story reward outcome: ") + name);
        };
        const auto row = [&](gc::PlanetId id) -> const gc::LiveCorruptionTransition& {
            const auto& rows = game.galaxy().ability_state()->transitions;
            return *std::find_if(rows.begin(), rows.end(), [&](const auto& value) { return value.planet == id; });
        };
        click("RESTRICT", true); click("ENABLE", true);
        expect(row(planet).restricted && row(planet).hologram_enabled && !row(other).restricted && !row(other).hologram_enabled,
            "stock story flags affect only the first named planet");
        const auto identities = eawr::checkpoint::game_identities(config);
        // A restored game reinstalls the plots and the live reward provider, like the session that saved it.
        eawr::checkpoint::GameLoadOptions load;
        load.configure_session = [&](cp::GameSession& fresh) -> eawr::core::Result<void> {
            if (auto story = fresh.install_story({}, {{player, "CorruptionPolicy.xml"}}, {plot}); !story) return story;
            return cp::bind_story_rewards(fresh, plan, installed.data);
        };
        const auto saved = must(eawr::checkpoint::save_game_checkpoint(game, identities));
        auto restored = must(eawr::checkpoint::load_game_checkpoint(saved, identities, load, executor));
        expect(restored.session.galaxy().ability_state()->transitions == game.galaxy().ability_state()->transitions,
            "stock checkpoint reconstructs restriction and hologram flags");
        click("MISSING", false); click("BAD_FLAG", false); click("BAD_TIME", false);
        click("REMOVE", true);
        expect(row(planet).transition.decreasing && row(planet).transition.end - row(planet).transition.start == 300,
            "actor-free REMOVE_CORRUPTION defaults to ten seconds despite actor restriction");
        // A second removal while one runs is declined and changes neither the transition nor the journal.
        const auto running = row(planet).transition;
        const auto journaled = game.galaxy().record().native_effects.size();
        click("REMOVE_AGAIN", false);
        expect(row(planet).transition.start == running.start && row(planet).transition.end == running.end
            && row(planet).transition.decreasing && game.galaxy().record().native_effects.size() == journaled,
            "a second REMOVE_CORRUPTION while one runs is declined without a journal entry");
        // Removal on a planet that is not corrupted is declined and leaves it untouched.
        // Preconditions: the planet has a live row and no corruption, so the decline is the "nothing to remove" rule and not a missing row.
        const auto& clean_rows = game.galaxy().ability_state()->transitions;
        const auto clean_row = std::find_if(clean_rows.begin(), clean_rows.end(), [&](const auto& value) { return value.planet == *clean; });
        const auto& economy_rows = game.galaxy().snapshot()->economy_corruption()->planets;
        const auto clean_economy = std::find_if(economy_rows.begin(), economy_rows.end(), [&](const auto& value) { return value.planet == *clean; });
        expect(clean_row != clean_rows.end() && clean_economy != economy_rows.end()
            && clean_economy->corruption == gc::corruption_fixture::Type::none && !clean_row->transition.active(),
            "the REMOVE_CLEAN planet has live corruption rows and no corruption before the click");
        if (clean_row == clean_rows.end() || clean_economy == economy_rows.end()) return;
        const auto clean_journaled = game.galaxy().record().native_effects.size();
        click("REMOVE_CLEAN", false);
        expect(!row(*clean).transition.active() && game.galaxy().record().native_effects.size() == clean_journaled,
            "REMOVE_CORRUPTION on an uncorrupted planet is declined without a journal entry");
        // Save in the middle of the removal: the loaded copy finishes it in step with the original.
        const auto middle = must(eawr::checkpoint::save_game_checkpoint(game, identities));
        auto midway = must(eawr::checkpoint::load_game_checkpoint(middle, identities, load, executor));
        expect(midway.session.galaxy().ability_state()->transitions == game.galaxy().ability_state()->transitions,
            "a checkpoint taken mid-removal restores the running transition");
        for (unsigned guard = 0; row(planet).transition.active(); ++guard) {
            expect(guard < 600, "the story removal completes within a bounded number of ticks");
            if (guard >= 600) break;
            const auto hash = must(game.step(executor)).state_hash.get();
            hashes.push_back(hash);
            expect(must(midway.session.step(executor)).state_hash.get() == hash, "the mid-removal checkpoint continues equal to the uninterrupted game");
        }
        expect(!row(planet).transition.active() && row(planet).transition.current == gc::corruption_fixture::Type::none
            && midway.session.galaxy().ability_state()->transitions == game.galaxy().ability_state()->transitions,
            "the removal completes to no corruption in both the original and the restored game");
        click("SHORT", true);
        expect(row(other).transition.decreasing && row(other).transition.end - row(other).transition.start == 75,
            "REMOVE_CORRUPTION honors the authored fractional duration");
        click("LIFT", true); click("DISABLE", true);
        expect(!row(planet).restricted && !row(planet).hologram_enabled, "zero clears both story flags");
        if (workers == 1) baseline = hashes;
        else expect(hashes == baseline, "stock corruption story hooks are equal on 1/2/4/8 workers");
    }
}
} // namespace

int case_galactic_story_engine_stock_tests() {
    try {
        const Installed installed;
        plots_per_faction(installed);
        session(installed);
        corruption_story_hooks(installed);
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
