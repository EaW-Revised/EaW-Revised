#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "galactic/map_input.hpp"
#include "eawr/platform/live_game_session.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

// Live GC space battle started from the galaxy: the enemy side always fights with its
// tactical AI, independent of whether the native galactic AI is installed. The human seat
// never gets one.
namespace {
namespace gc = eawr::sim::galactic;
namespace view = eawr::viewer::galactic;
using galactic_test::expect;
using eawr::sim::math::Fixed;
using namespace std::chrono_literals;
using eawr::testing::result_diagnostic::take;
std::optional<std::string> game_root() {
    return eawr::testing::game_root();
}
std::vector<view::InputPlanet> input_planets(const gc::GalacticSnapshot& snapshot, gc::PlayerId player) {
    std::vector<view::InputPlanet> result;
    for (const auto& planet : snapshot.planets())
        result.push_back({planet.id, {}, planet.owner, view::observer_discovery(snapshot, player, planet.id).value_or(false)});
    return result;
}

struct BattleCommands {
    std::map<eawr::sim::tactical::PlayerId, std::size_t> by_issuer;
    std::uint64_t ticks{};
    std::vector<std::string> hashes;
};

// One live campaign on the stock combat tables: the human Empire fleet attacks the AI Rebel
// corvette at a neighbouring planet and chooses the tactical battle. Returns the commands
// each issuer put into the child battle's record.
BattleCommands run_battle(bool native_ai, unsigned workers, std::uint64_t battle_ticks, gc::PlayerId& human_player,
    gc::PlayerId& enemy_player) {
    const auto root = game_root();
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
    const auto filesystem_owner = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(mounts)));
    const auto& filesystem = *filesystem_owner;
    const auto loaded = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    auto data = take(eawr::data::galactic::load_galactic_data(filesystem, loaded.catalog));
    auto types = eawr::units::galactic_type_table(loaded.catalog);
    constexpr auto campaign_name = "Sandbox_Equal_Footing_Empire";
    auto campaign = std::find_if(data.campaigns.begin(), data.campaigns.end(),
        [](const auto& row) { return row.name == campaign_name; });
    if (campaign == data.campaigns.end()) throw std::runtime_error("missing stock campaign family");
    // Synthetic authoring over stock tables: two neighbouring planets, one route, an AI
    // defender with a stock corvette, and an Empire escort fleet in orbit. No retail bytes.
    campaign->locations = campaign->planets = {"Shola", "Kamino"};
    campaign->trade_routes = {"B3938_Route"};
    campaign->home_locations = {{"Empire", "Shola"}, {"Rebel", "Kamino"}};
    campaign->starting_active_player = "Empire";
    campaign->starting_forces = {{"Empire", "Shola", "E_Ground_Barracks"}, {"Rebel", "Kamino", "R_Ground_Barracks"},
        {"Rebel", "Kamino", "Corellian_Corvette"}, {"Rebel", "Kamino", "Corellian_Corvette"}};
    for (unsigned i = 0; i < 3; ++i) campaign->starting_forces.push_back({"Empire", "Shola", "Star_Destroyer"});
    campaign->human_victory_conditions = {"GALACTIC_ALL_PLANETS_CONTROLLED"};
    campaign->ai_victory_conditions = {"GALACTIC_OPPONENT_CONTROLS_NO_PLANETS"};
    campaign->is_autoresolve_allowed = true; campaign->planet_auto_reveal = true;
    campaign->autoresolve_exclusion_locations.clear(); campaign->is_multiplayer = false;
    campaign->special_case_production.clear(); campaign->corruption_level_overrides.clear();
    data.trade_routes.push_back({"B3938_Route", {}, "Shola", "Kamino", Fixed::from_integer(2).value(),
        Fixed::from_integer(1).value(), Fixed::from_integer(1).value(), {}});
    for (auto& planet : data.planets) {
        if (planet.name == "Shola") planet.galactic_position = eawr::data::galactic::Position{};
        if (planet.name == "Kamino") planet.galactic_position = eawr::data::galactic::Position{Fixed::from_integer(12).value(), {}, {}};
    }
    eawr::campaign::CampaignSetupOptions options; options.seed = 42;
    gc::SeatId human_seat{};
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == "Empire";
        if (human) { human_player = id; human_seat = gc::SeatId{id}; options.solo_reference_human = id; }
        if (faction.name == "Rebel") enemy_player = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
    }
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});

    auto prepared = take(eawr::platform::PreparedLiveCampaign::create(filesystem_owner, loaded.catalog,
        data, types, campaign_name, options, "Normal_Default", native_ai));
    expect(prepared->native_ai_installed() == native_ai, "native AI install follows the explicit opt-in");
    gc::PlanetId home{}, enemy{};
    for (const auto& location : prepared->plan().locations()) {
        if (location.original_type == "Shola" && location.planet) home = location.planet->id;
        if (location.original_type == "Kamino" && location.planet) enemy = location.planet->id;
    }
    const auto initial = prepared->snapshot();
    const auto fleet = std::find_if(initial->fleets().begin(), initial->fleets().end(),
        [&](const auto& row) { return row.owner == human_player && row.orbit == home; });
    if (!home || !enemy || fleet == initial->fleets().end()) throw std::runtime_error("incomplete fixture placement");
    const auto moving_fleet = fleet->id;
    eawr::platform::LiveGameSession::Options live_options;
    live_options.workers = workers; live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
    auto live = take(eawr::platform::LiveGameSession::start(std::move(prepared), live_options));
    std::uint64_t tick{}, command_cursor{};
    const auto wait_for_tick = [&](std::uint64_t target) {
        if (!live->wait_for(target, 20s)) return false;
        const auto events = live->galactic_events_after(command_cursor);
        command_cursor = events.through;
        for (const auto& event : events.events) if (event.event.kind == gc::GalacticEventKind::command_rejected)
            throw std::runtime_error("rejected command reason=" + std::string(gc::to_string(event.event.reason)));
        return true;
    };
    while (!view::observer_discovery(*live->frame().latest, human_player, enemy).value_or(false) && tick < 1000) {
        live->advance_to(++tick);
        if (!wait_for_tick(tick)) throw std::runtime_error("observer discovery stalled");
    }
    // Assemble the startup fleets, then send them along the authored route.
    for (const auto& row : live->frame().latest->fleets()) if (row.owner == human_player && row.orbit == home && row.id != moving_fleet)
        live->submit(eawr::platform::LiveGameOrder{human_seat, gc::MergeFleets{row.id, moving_fleet}, {}});
    live->advance_to(++tick);
    if (!wait_for_tick(tick)) throw std::runtime_error("fleet assembly stalled");
    view::MapInput input;
    const auto snapshot = live->frame().latest;
    const auto intent = view::fleet_destination_intent(*snapshot, human_player, moving_fleet, enemy,
        input_planets(*snapshot, human_player));
    if (!intent) throw std::runtime_error("move intent refused");
    const auto move = input.command(*intent, input_planets(*snapshot, human_player));
    if (!move) throw std::runtime_error("map command refused");
    live->submit(eawr::platform::LiveGameOrder{human_seat, *move, {}});
    eawr::sim::EntityId chosen{};
    const auto deadline = tick + 4000;
    while (tick < deadline) {
        const auto before = live->frame();
        if (before.battle || before.conflict) break;
        if (before.pending_conflict && before.pending_conflict->choice_ready && before.pending_conflict->conflict.id != chosen) {
            chosen = before.pending_conflict->conflict.id;
            expect(before.tactical_available, "the stock space conflict admits a tactical battle");
            const auto choice = eawr::platform::battle_choice_order(before, human_seat, chosen, true);
            if (!choice) throw std::runtime_error("tactical choice refused");
            live->submit(*choice);
        }
        live->advance_to(++tick);
        if (!wait_for_tick(tick)) {
            if (const auto error = live->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
            throw std::runtime_error("live campaign stalled at tick " + std::to_string(tick));
        }
    }
    const auto entered = live->frame();
    if (!entered.conflict) throw std::runtime_error("no tactical child was staged");
    const auto conflict = entered.conflict->id;
    // The pacer holds the child at tick zero until the presentation side reports ready.
    for (unsigned attempt = 0; attempt < 200 && !live->battle_is_ready(conflict); ++attempt) {
        live->battle_ready(conflict);
        std::this_thread::sleep_for(50ms);
    }
    if (!live->battle_is_ready(conflict)) {
        std::string why = "battle never became ready";
        if (const auto loading = live->loading(human_seat))
            why += " (loading outcome " + std::to_string(static_cast<int>(loading->outcome)) + ": "
                + (loading->failure ? eawr::core::format_diagnostic(*loading->failure) : std::string("none")) + ")";
        else why += " (no loading view)";
        why += " tactical_refusal=" + live->frame().tactical_refusal;
        if (const auto error = live->failure()) why += " failure=" + eawr::core::format_diagnostic(*error);
        throw std::runtime_error(why);
    }
    auto feed = live->battle_feed(conflict, human_seat);
    if (!feed) throw std::runtime_error("no battle feed for the human seat");
    feed->set_paused(false);
    feed->advance_to(battle_ticks);
    if (!feed->wait_for(battle_ticks, 60s) && feed->completed_tick() == 0) {
        if (const auto error = live->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
        throw std::runtime_error("battle did not advance: paused=" + std::to_string(feed->paused())
            + " ready=" + std::to_string(live->battle_is_ready(conflict)));
    }
    BattleCommands out;
    const auto record = feed->record();
    for (const auto& command : record.commands) ++out.by_issuer[command.key.player_id];
    out.ticks = feed->completed_tick();
    out.hashes = feed->tick_hashes();
    expect(!live->failure(), "live battle runs without a session failure");
    live->stop();
    return out;
}

void battle_ai(bool native_ai) {
    if (!game_root()) { eawr::testing::skip_installed("SKIPPED: live battle AI needs EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    constexpr std::uint64_t battle_ticks = 450; // 15 s of battle time
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        gc::PlayerId human{}, enemy{};
        const auto battle = run_battle(native_ai, workers, battle_ticks, human, enemy);
        const auto enemy_orders = battle.by_issuer.count(enemy) ? battle.by_issuer.at(enemy) : 0U;
        const auto human_orders = battle.by_issuer.count(human) ? battle.by_issuer.at(human) : 0U;
        std::cout << "native_ai=" << native_ai << " workers=" << workers << " ticks=" << battle.ticks
            << " enemy_orders=" << enemy_orders << " human_orders=" << human_orders << '\n';
        expect(battle.ticks > 0, "the child battle advanced");
        expect(enemy_orders > 0, "the enemy side's tactical AI issues orders within the first 15 s of battle");
        expect(human_orders == 0, "the human side gets no AI orders");
        if (workers == 1) reference = battle.hashes;
        else expect(battle.hashes == reference, "battle hashes agree on 1/2/4/8 workers");
    }
}
} // namespace
int case_galactic_live_battle_ai_tests(int argc, char** argv) {
    try {
        const bool on = argc == 2 && std::string_view(argv[1]) == "--galactic-ai-on";
        battle_ai(on);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
