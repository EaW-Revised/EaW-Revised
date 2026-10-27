#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "galactic/map_input.hpp"
#include "galactic/ground_roster.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/campaign/auto_resolve_live.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

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
void installed_loop(bool purchase_only) {
    const auto root = game_root();
    if (!root) { eawr::testing::skip_installed("SKIPPED: S2 stock-stat campaign requires EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
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
    // Synthetic authoring over stock combat/movement tables: three neighboring
    // planets, a passive defender, and a source-qualified orbital army start.
    // No generated assets or retail fixture bytes are committed.
    campaign->locations = campaign->planets = {"Shola", "Kamino", "Geonosis"};
    campaign->trade_routes = {"S2_Test_Route", "S2_Final_Route"};
    campaign->home_locations = {{"Empire", "Shola"}, {"Rebel", "Kamino"}};
    campaign->starting_active_player = "Empire";
    campaign->starting_forces = {{"Empire", "Shola", "E_Ground_Barracks"},
        {"Rebel", "Kamino", "R_Ground_Barracks"}, {"Rebel", "Kamino", "Corellian_Corvette"},
        {"Rebel", "Kamino", "Rebel_Infantry_Squad"},
        {"Rebel", "Geonosis", "R_Ground_Barracks"}, {"Rebel", "Geonosis", "Corellian_Corvette"},
        {"Rebel", "Geonosis", "Rebel_Infantry_Squad"}};
    for (unsigned i = 0; i < (purchase_only ? 1U : 6U); ++i) {
        // The purchase probe needs spare authored population capacity. Keep a
        // stock corvette escort there; the full invasion fixture keeps its destroyers.
        campaign->starting_forces.push_back({"Empire", "Shola", purchase_only ? "Tartan_Patrol_Cruiser" : "Star_Destroyer"});
        campaign->starting_forces.push_back({"Empire", "Shola", "Imperial_Stormtrooper_Squad"});
    }
    // Two defended invasions need a second army reserve: normal stock losses
    // consume companies, and WAR-56 leaves a surviving transport on the first
    // conquered surface. Retain stock combat tables and the six-ship escort.
    for (unsigned i = 0; i < (purchase_only ? 0U : 6U); ++i)
        campaign->starting_forces.push_back({"Empire", "Shola", "Imperial_Stormtrooper_Squad"});
    campaign->human_victory_conditions = {"GALACTIC_ALL_PLANETS_CONTROLLED"};
    campaign->ai_victory_conditions = {"GALACTIC_OPPONENT_CONTROLS_NO_PLANETS"};
    campaign->is_autoresolve_allowed = true; campaign->planet_auto_reveal = true;
    campaign->autoresolve_exclusion_locations.clear(); campaign->is_multiplayer = false;
    campaign->special_case_production.clear(); campaign->corruption_level_overrides.clear();
    data.trade_routes.push_back({"S2_Test_Route", {}, "Shola", "Kamino", Fixed::from_integer(2).value(),
        Fixed::from_integer(1).value(), Fixed::from_integer(1).value(), {}});
    data.trade_routes.push_back({"S2_Final_Route", {}, "Kamino", "Geonosis", Fixed::from_integer(2).value(),
        Fixed::from_integer(1).value(), Fixed::from_integer(1).value(), {}});
    for (auto& planet : data.planets) {
        if (planet.name == "Shola") planet.galactic_position = eawr::data::galactic::Position{};
        if (planet.name == "Kamino") planet.galactic_position = eawr::data::galactic::Position{Fixed::from_integer(12).value(), {}, {}};
        if (planet.name == "Geonosis") planet.galactic_position = eawr::data::galactic::Position{Fixed::from_integer(24).value(), {}, {}};
    }
    // This fixture changes placement only. The live startup provider expands
    // each stock company into its transport and payload identities.
    const auto* army = types.find("Imperial_Stormtrooper_Squad");
    if (!army) throw std::runtime_error("missing stock Imperial company");
    if (!purchase_only) for (auto& type : types.types)
        if (std::find(army->company_units.begin(), army->company_units.end(), type.name) != army->company_units.end())
            type.always_spawn_in_orbit = true;
    eawr::campaign::CampaignSetupOptions options; options.seed = 42;
    gc::PlayerId attacker{}, defender{}; gc::SeatId human_seat{};
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == "Empire";
        if (human) { attacker = id; human_seat = gc::SeatId{id}; options.solo_reference_human = id; }
        if (faction.name == "Rebel") defender = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
    }
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto prepared = take(eawr::platform::PreparedLiveCampaign::create(filesystem_owner, loaded.catalog,
            data, types, campaign_name, options, "Normal_Default"));
        gc::PlanetId home{}, enemy{}, last_enemy{};
        for (const auto& location : prepared->plan().locations()) {
            if (location.original_type == "Shola" && location.planet) home = location.planet->id;
            if (location.original_type == "Kamino" && location.planet) enemy = location.planet->id;
            if (location.original_type == "Geonosis" && location.planet) last_enemy = location.planet->id;
        }
        const auto initial = prepared->snapshot();
        auto access_probe = take(eawr::campaign::make_stock_auto_resolver(filesystem, loaded.catalog,
            data, types, prepared->plan()));
        const auto fleet = std::find_if(initial->fleets().begin(), initial->fleets().end(),
            [&](const auto& row) { return row.owner == attacker && row.orbit == home; });
        if (!home || !enemy || !last_enemy || fleet == initial->fleets().end()) throw std::runtime_error("incomplete S2 fixture placement");
        const auto moving_fleet = fleet->id;
        expect(initial->planets().size() == 3 && std::count_if(initial->planets().begin(), initial->planets().end(),
            [&](const auto& row) { return row.owner == attacker; }) == 1,
            "S2 starts with one owned and two enemy planets");
        eawr::platform::LiveGameSession::Options live_options;
        live_options.workers = workers; live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
        auto live = take(eawr::platform::LiveGameSession::start(std::move(prepared), live_options));
        std::uint64_t tick{};
        std::uint64_t command_cursor{};
        const auto wait_for_tick = [&](std::uint64_t target) {
            if (!live->wait_for(target, 5s)) return false;
            const auto events = live->galactic_events_after(command_cursor);
            if (events.incomplete) throw std::runtime_error("S2 command journal was not consumed in time");
            command_cursor = events.through;
            for (const auto& event : events.events) if (event.event.kind == gc::GalacticEventKind::command_rejected)
                throw std::runtime_error("S2 rejected command subject=" + std::to_string(event.event.subject)
                    + " reason=" + std::string(gc::to_string(event.event.reason)));
            return true;
        };
        // Fresh startup reveals the local home, then the normal observer
        // service discovers neighboring planets; input must wait for that fact.
        while (!view::observer_discovery(*live->frame().latest, attacker, enemy).value_or(false) && tick < 1000) {
            live->advance_to(++tick);
            if (!wait_for_tick(tick)) throw std::runtime_error("S2 observer discovery stalled");
        }
        const auto eligible = [&](const auto& row) {
            return row.planet == home && row.player == attacker && row.domain == gc::ProductionDomain::ground
                && (row.kind == gc::ProductionKind::unit || (!purchase_only && row.kind == gc::ProductionKind::structure))
                && row.admission == gc::ProductionAdmission::allowed;
        };
        // Map discovery alone does not publish the producer's current local
        // ownership knowledge (WGUI-91). Wait for the normal observer service.
        while (tick < 1200) {
            const auto observed = live->frame().latest;
            const auto offers = observed->production_offers();
            if (std::any_of(offers.begin(), offers.end(), eligible)) break;
            live->advance_to(++tick);
            if (!wait_for_tick(tick)) throw std::runtime_error("S2 producer observation stalled");
        }
        const auto production = live->frame().latest;
        const auto offers = production->production_offers();
        const auto offer = std::find_if(offers.begin(), offers.end(), eligible);
        if (offer == offers.end()) {
            std::cerr << "S2 production offers=" << offers.size() << " tick=" << tick << '\n';
            for (const auto& row : production->population()) if (row.player == attacker)
                std::cerr << "capacity=" << row.capacity.raw() << " used=" << row.used.raw() << '\n';
            for (const auto& row : offers) if (row.planet == home && row.domain == gc::ProductionDomain::ground) {
                const auto type = std::find_if(options.types.begin(), options.types.end(), [&](const auto& value) { return value.id == row.type; });
                std::cerr << (type == options.types.end() ? "unbound" : type->original_type)
                    << " admission=" << static_cast<unsigned>(row.admission) << '\n';
            }
            throw std::runtime_error("S2 home has no admitted stock ground production offer");
        }
        const auto purchased_type = offer->type;
        const auto before_purchase = view::ground_roster(*production, attacker, home);
        live->submit(eawr::platform::LiveGameOrder{human_seat,
            gc::ProductionCommand{home, purchased_type, gc::ProductionAction::buy, gc::ProductionDomain::ground, 0, {}}, {}});
        bool produced{}; auto cursor = live->galactic_events_after(0).through;
        const auto production_deadline = tick + 8000;
        while (!produced && tick < production_deadline) {
            live->advance_to(++tick);
            if (!wait_for_tick(tick)) throw std::runtime_error("S2 production stalled");
            const auto events = live->galactic_events_after(cursor); cursor = events.through;
            for (const auto& event : events.events)
                produced = produced || (event.event.kind == gc::GalacticEventKind::production_completed
                    && event.event.subject == home && event.event.value == purchased_type);
        }
        if (!produced) throw std::runtime_error("S2 stock production did not complete");
        const auto completed_purchase = live->frame().latest;
        const auto after_purchase = view::ground_roster(*completed_purchase, attacker, home);
        if (purchase_only) {
            const auto& surfaces = completed_purchase->control()->planets;
            const auto surface = std::find_if(surfaces.begin(), surfaces.end(),
                [&](const auto& row) { return row.planet == home; });
            expect(surface != surfaces.end() && surface->landed.size() == before_purchase.companies.size() + 1,
                "completed purchase creates a live surface transport before HUD publication");
            expect(after_purchase.contents_known && after_purchase.count_known,
                "completed local purchase retains observer content and count admission");
            expect(after_purchase.companies.size() == before_purchase.companies.size() + 1,
                "completed stock company appears in the observer-filtered viewer ground roster");
            expect(after_purchase.count == after_purchase.companies.size(),
                "published surface count agrees with the completed local company roster");
            expect(std::any_of(after_purchase.companies.begin(), after_purchase.companies.end(),
                [&](const auto& row) { return row.type == purchased_type; }),
                "completed company uses its authored display type rather than its runtime transport type");
        }
        expect(live->frame().latest->clock().galactic_frame > production->clock().galactic_frame,
            "live stock production advances through the normal campaign timer");
        if (purchase_only) {
            expect(live->rejected_orders().empty() && !live->failure(), "live purchase is an accepted seat command");
            live->stop(); const auto hashes = live->tick_hashes();
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "purchase-to-roster hashes agree on 1/2/4/8 workers");
            std::cout << "purchase workers=" << workers << " ticks=" << tick << " company=" << purchased_type << '\n';
            continue;
        }
        // WGS-80 retains separate startup fleets. Assemble the expedition with
        // ordinary seat commands so its selected ship also carries the army.
        const auto assembly = live->frame().latest;
        std::size_t assembled_members{};
        for (const auto& row : assembly->fleets()) if (row.owner == attacker && row.orbit == home) {
            assembled_members += row.members.size();
            if (row.id != moving_fleet)
                live->submit(eawr::platform::LiveGameOrder{human_seat, gc::MergeFleets{row.id, moving_fleet}, {}});
        }
        live->advance_to(++tick);
        if (!wait_for_tick(tick)) throw std::runtime_error("S2 fleet assembly stalled");
        const auto revealed = live->frame().latest;
        const auto assembled = std::find_if(revealed->fleets().begin(), revealed->fleets().end(),
            [&](const auto& row) { return row.id == moving_fleet; });
        if (assembled == revealed->fleets().end() || assembled->members.size() != assembled_members || assembled_members < 18)
            throw std::runtime_error("S2 ordinary fleet assembly did not retain all stock ships and orbital companies");
        view::MapInput input;
        bool victory{};
        const auto conquer = [&](gc::PlanetId target, bool terminal) {
            const auto attack_snapshot = live->frame().latest;
            const auto initial_result = live->frame().result;
            const auto intent = view::fleet_destination_intent(*attack_snapshot, attacker, moving_fleet, target,
                input_planets(*attack_snapshot, attacker), {});
            expect(intent && intent->kind == view::DropKind::move && intent->path.size() == 2 && intent->path.back() == target,
                "map destination resolves the authored trade route");
            if (!intent) throw std::runtime_error("S2 move intent refused");
            const auto move = input.command(*intent, input_planets(*attack_snapshot, attacker));
            if (!move) throw std::runtime_error("S2 map command refused");
            live->submit(eawr::platform::LiveGameOrder{human_seat, *move, {}});
            bool travelled{}, arrived{}, space_result{}, land_result{}, invaded{}, flipped{};
            eawr::sim::EntityId chosen{}, space_conflict{};
            const auto conquest_deadline = tick + 4000;
            for (; tick < conquest_deadline && !(terminal ? victory : flipped && land_result); ++tick) {
                const auto before = live->frame();
                if (before.pending_conflict && before.pending_conflict->choice_ready
                    && before.pending_conflict->conflict.id != chosen) {
                    chosen = before.pending_conflict->conflict.id;
                    const auto choice = eawr::platform::battle_choice_order(before, human_seat, chosen, false);
                    if (!choice) throw std::runtime_error("S2 automatic choice refused");
                    live->submit(*choice);
                }
                live->advance_to(tick + 1);
                if (!wait_for_tick(tick + 1)) {
                    if (const auto error = live->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
                    throw std::runtime_error("S2 live campaign stalled at tick " + std::to_string(tick));
                }
                const auto frame = live->frame();
                expect(!frame.battle, "S2 auto-resolve retains galaxy without a tactical child");
                for (const auto& row : frame.latest->fleets()) if (row.id == moving_fleet) {
                    travelled = travelled || row.motion.transit.has_value();
                    arrived = arrived || (travelled && !row.motion.transit && row.orbit == target);
                }
                if (frame.result && !space_result && (!initial_result || frame.result->conflict != initial_result->conflict)) {
                    expect(frame.result->winner == attacker, "stock-strength space resolution selects attacking fleet");
                    expect(frame.auto_resolve_outcome && !frame.auto_resolve_outcome->land
                        && frame.auto_resolve_outcome->planet == target, "each new expedition resolves its destination space conflict");
                    space_result = true;
                    space_conflict = frame.result->conflict;
                }
                if (space_result && arrived && !invaded && !frame.pending_conflict) {
                    const auto land = view::fleet_destination_intent(*frame.latest, attacker, moving_fleet, target,
                        input_planets(*frame.latest, attacker), {});
                    expect(land && land->kind == view::DropKind::land, "same-planet destination produces invasion");
                    if (!land) throw std::runtime_error("S2 invasion intent refused");
                    const auto command = input.command(*land, input_planets(*frame.latest, attacker));
                    if (!command) throw std::runtime_error("S2 invasion command refused");
                    live->submit(eawr::platform::LiveGameOrder{human_seat, *command, {}}); invaded = true;
                }
                if (frame.pending_conflict && frame.pending_conflict->choice_ready)
                    expect(frame.latest->clock().pause_depth != 0, "land admission owns its conflict pause");
                for (const auto& planet : frame.latest->planets()) if (planet.id == target) flipped = planet.owner == attacker;
                if (flipped && invaded && frame.result && frame.result->conflict != space_conflict
                    && frame.result->winner == attacker && frame.auto_resolve_outcome && frame.auto_resolve_outcome->land) land_result = true;
                if (frame.latest->victory()) {
                    const auto result = gc::campaign_result(*frame.latest->victory(), attacker, false, false);
                    if (result) { expect(result->local_won && result->condition.player == attacker, "conquest produces local victory"); victory = true; }
                }
            }
            const bool completed = travelled && arrived && space_result && invaded && land_result && flipped && (!terminal || victory);
            expect(completed, "S2 moves, auto-resolves space and land and changes ownership before terminal conquest");
            const auto conquest_frame = live->frame();
            if (!completed) {
                std::cerr << "S2 target=" << target << " attacker=" << attacker << " terminal=" << terminal << '\n';
                std::cerr << "S2 milestones travel=" << travelled << " arrival=" << arrived << " space=" << space_result
                    << " invasion=" << invaded << " land=" << land_result << " owner=" << flipped << " victory=" << victory
                    << " tick=" << tick << '\n';
                if (conquest_frame.result) std::cerr << "S2 result conflict=" << conquest_frame.result->conflict << " winner=" << conquest_frame.result->winner
                    << " land=" << (conquest_frame.auto_resolve_outcome && conquest_frame.auto_resolve_outcome->land) << '\n';
            }
            expect(conquest_frame.latest->clock().pause_depth == 0, "both automatic returns release only their conflict pause");
            expect(terminal || !victory, "intermediate conquest leaves the campaign active");
        };
        conquer(enemy, false);
        expect(live->rejected_orders().empty() && !live->failure(), "all live S2 seat commands are accepted");
        // Refresh must remove the defeated force's blocking presence. Exercise
        // an ordinary return and redeparture before the campaign ends.
        for (const auto destination : {home, enemy}) {
            const auto snapshot = live->frame().latest;
            const auto next = view::fleet_destination_intent(*snapshot, attacker, moving_fleet, destination,
                input_planets(*snapshot, attacker), {});
            if (!next) {
                std::cerr << "S2 route refused fleet=" << moving_fleet << " destination=" << destination
                    << " tick=" << tick << " movement=" << bool(snapshot->movement())
                    << " distance=" << bool(snapshot->maximum_movement_distance()) << '\n';
                for (const auto& row : snapshot->fleets())
                    std::cerr << "fleet=" << row.id << " owner=" << row.owner << " orbit=" << row.orbit
                        << " transit=" << bool(row.motion.transit) << " members=" << row.members.size() << '\n';
                for (const auto& row : input_planets(*snapshot, attacker))
                    std::cerr << "planet=" << row.id << " owner=" << row.allegiance << " discovered=" << row.discovered << '\n';
                if (snapshot->movement()) for (const auto& row : snapshot->movement()->access) if (row.player == attacker)
                    std::cerr << "access=" << row.planet << " accessible=" << row.accessible << " conflict=" << row.potential_conflict << '\n';
                throw std::runtime_error("S2 post-conquest map route refused");
            }
            const auto command = input.command(*next, input_planets(*snapshot, attacker));
            if (!command) throw std::runtime_error("S2 redeparture map command refused");
            live->submit(eawr::platform::LiveGameOrder{human_seat, *command, {}});
            bool departed{}, returned{}; const auto deadline = tick + 4000;
            while (!returned && tick < deadline) {
                live->advance_to(++tick);
                if (!wait_for_tick(tick)) throw std::runtime_error("S2 redeparture stalled");
                for (const auto& row : live->frame().latest->fleets()) if (row.id == moving_fleet) {
                    departed = departed || row.motion.transit.has_value();
                    returned = departed && !row.motion.transit && row.orbit == destination;
                }
            }
            expect(returned, "post-conquest return and redeparture use reconciled live movement access");
        }
        expect(live->rejected_orders().empty() && !live->failure(), "post-conquest moves remain ordinary accepted seat commands");
        const auto discovery_deadline = tick + 1200;
        while (!view::observer_discovery(*live->frame().latest, attacker, last_enemy).value_or(false) && tick < discovery_deadline) {
            live->advance_to(++tick);
            if (!wait_for_tick(tick)) throw std::runtime_error("S2 final-planet observation stalled");
        }
        conquer(last_enemy, true);
        const auto final = live->frame();
        expect(victory, "last conquest produces campaign victory");
        const eawr::sim::InlineExecutor probe_executor;
        const auto refreshed = take(access_probe.access(*final.latest, probe_executor));
        const auto published_access = final.latest->movement()->access;
        expect(refreshed == published_access, "terminal conquest publishes current access, independently recomputed from final presence");
        const auto post_victory = view::fleet_destination_intent(*final.latest, attacker, moving_fleet, home,
            input_planets(*final.latest, attacker), {});
        std::cout << "S2 terminal access current=" << (refreshed == published_access)
            << " return intent=" << bool(post_victory) << " pause=" << final.latest->clock().pause_depth << '\n';
        for (const auto& row : published_access) if (row.player == attacker)
            std::cout << "S2 terminal planet=" << row.planet << " accessible=" << row.accessible
                << " conflict=" << row.potential_conflict << '\n';
        if (final.latest->victory()) {
            const auto loss = gc::campaign_result(*final.latest->victory(), defender, false, false);
            expect(loss && !loss->local_won, "the conquered passive opponent receives defeat");
        }
        expect(live->rejected_orders().empty() && !live->failure(), "all live S2 seat commands are accepted");
        live->stop(); const auto hashes = live->tick_hashes();
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "S2 whole-chain hashes agree on 1/2/4/8 workers");
        std::cout << "S2 workers=" << workers << " ticks=" << tick << " victory=" << victory << '\n';
    }
}
} // namespace
int case_galactic_s2_loop_tests(int argc, char** argv) {
    try { installed_loop(argc == 2 && std::string_view(argv[1]) == "--purchase-only"); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
