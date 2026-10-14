#include "galactic_support.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/campaign/control.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "../../apps/viewer/src/galactic/fleet_visibility.hpp"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <map>

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace dg = eawr::data::galactic;
using galactic_test::expect;
using eawr::core::Result;
using Fixed = eawr::sim::math::Fixed;
Fixed f(std::int64_t n) { return Fixed::from_integer(n).value(); }
template <typename T> Result<T> refusal() {
    eawr::core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup);
    d.message = "unimplemented WGS-U08 test provider";
    return Result<T>::failure(std::move(d));
}
dg::GalacticData data() {
    dg::GalacticData d;
    for (const auto& name : {"REBEL", "EMPIRE", "UNDERWORLD", "NEUTRAL"}) {
        dg::Faction faction; faction.name = name; faction.is_neutral = std::string_view(name) == "NEUTRAL";
        d.factions.push_back(faction);
    }
    dg::Planet a; a.name = "A"; a.galactic_position = dg::Position{f(0), f(0), f(0)}; a.behaviors = {"PLANET"};
    dg::Planet b = a; b.name = "B"; b.galactic_position->x = f(4);
    d.planets = {a, b};
    auto art = a; art.name = "Galaxy_Art"; art.behaviors = {"IDLE"};
    d.galaxy_art_objects.push_back(art); // A Planet-typed XML record, classified by the validated loader's WGS-01a boundary.
    dg::TradeRoute route; route.name = "AB"; route.point_a = "A"; route.point_b = "B"; route.hs_speed_factor = f(1);
    d.trade_routes = {route};
    d.constants = {{"MaximumFleetMovementDistance", "10", f(10)}, {"Fiscal_Cycle_Time_In_Secs", "45", f(45)}};
    dg::Campaign c; c.name = "Ordinary"; c.file = "data/xml/campaigns.xml";
    c.locations = {"Galaxy_Art", "A", "B"}; c.planets = {"A", "B"}; c.trade_routes = {"AB"};
    c.home_locations = {{"rebel", "A"}, {"REBEL", "B"}, {"EMPIRE", "Absent"}, {"UNDERWORLD", "B"}};
    c.starting_forces = {{"UNDERWORLD", "B", "Ship"}, {"REBEL", "A", "Ship"}, {"REBEL", "A", "Ship"},
        {"EMPIRE", "A", "Ship"}, {"Absent", "A", "Missing"}};
    c.special_case_production = {{"REBEL", "A", "Ship"}, {"REBEL", "A", "Ship"}};
    c.starting_active_player = "REBEL";
    d.campaigns.push_back(c);
    c.name = "Story"; c.is_story_campaign = true; c.supports_custom_settings = false; d.campaigns.push_back(c);
    c.name = "Tutorial"; c.tutorial = true; d.campaigns.push_back(c);
    c.name = "Multiplayer"; c.is_multiplayer = true; d.campaigns.push_back(c);
    return d;
}
eawr::units::GalacticTypeTable types() {
    eawr::units::GalacticType ship; ship.name = "Ship"; ship.object_class = "SpaceUnit";
    ship.can_hyperspace_without_ability = true; ship.build_requires_initial_placement = true;
    eawr::units::GalacticType base; base.name = "Base"; base.object_class = "StarBase";
    base.can_hyperspace_without_ability = false;
    eawr::units::GalacticTypeTable result; result.types = {ship, base}; return result;
}
cp::CampaignSetupOptions options(gc::PlayerId human = 1) {
    cp::CampaignSetupOptions o; o.seed = 999; o.forced_seed = 42;
    o.players = {{{1, 1, 0}, "REBEL", true}, {{2, 2, 0}, "EMPIRE", true},
        {{3, 3, 0}, "UNDERWORLD", true}, {{4, 4, 0}, "NEUTRAL", true}};
    for (gc::PlayerId id = 1; id <= 3; ++id)
        o.seats.push_back({gc::SeatId{id}, id, id == human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, id == human ? 1U : 0U});
    o.types = {{7, "Ship"}, {8, "Base"}}; return o;
}
cp::CampaignSetupServices services(std::vector<std::string>& trace) {
    cp::CampaignSetupServices s;
    s.location = [&](const auto& location) {
        expect(location.initial_owner == 4 && (!location.planet || location.planet->owner == 4), "locations initially neutral");
        trace.push_back("location:" + location.original_type); return Result<void>::success();
    };
    s.mark_initial_placement = [&](const auto& mark) {
        trace.push_back("mark:" + std::to_string(mark.planet)); return Result<void>::success();
    };
    s.place = [&](const cp::StartingForceRequest& request, const eawr::sim::EntityId next, const auto&) {
        expect(request.target_allegiance == f(1), "target allegiance precedes delegated creation");
        trace.push_back("force:" + std::to_string(request.planet) + ":" + std::to_string(request.owner));
        cp::StartingForcePlacement placement;
        placement.fleets = {{next, request.owner, request.planet, {{next + 1, request.type}}}};
        placement.next_id = next + 2; placement.created = true;
        return Result<cp::StartingForcePlacement>::success(std::move(placement));
    };
    s.unify_human_guard_fleets = [&](const auto planet, const auto player, const auto& setup) {
        trace.push_back("unify:" + std::to_string(planet) + ":" + std::to_string(player));
        return Result<std::vector<gc::Fleet>>::success(setup.fleets);
    };
    s.political_control = [&](const auto& planet, std::span<const cp::StartingAllegiance> targets, const auto& setup) {
        expect(setup.fleets.size() == 4, "all authored forces exist before political initialization");
        expect(!targets.empty() && targets.front().target == f(1), "political service receives typed target amounts");
        trace.push_back("control:" + std::to_string(planet.id));
        // The test service chooses independently of force-record order: A's final record is
        // Empire, yet A's owner is Rebel. Real control arithmetic belongs to its own provider.
        return Result<gc::PlayerId>::success(planet.id == 1 ? 1 : 3);
    };
    s.credit_cap = [&](const auto& player, const auto& setup) {
        expect(setup.planets[0].owner == 1 && setup.planets[1].owner == 3, "credit caps follow complete control pass");
        trace.push_back("cap:" + std::to_string(player.player_id)); return Result<Fixed>::success(f(100));
    };
    s.home = [&](const auto& home) {
        trace.push_back("home:" + std::to_string(home.player)); return Result<void>::success();
    };
    s.assign_tech = [](const cp::CampaignTech& request, const auto&) {
        return Result<std::int64_t>::success(request.requested_current);
    };
    return s;
}
cp::CampaignSetupPlan plan(std::string_view name = "Ordinary", gc::PlayerId human = 1) {
    return cp::plan_campaign_setup(data(), types(), name, options(human)).value();
}
void full_initial_control() {
    std::vector<std::string> trace;
    auto live = services(trace);
    live.political_control = {};
    live.initial_political_control = [](const auto&, const auto& setup) {
        gc::PoliticalControlSetup presence;
        presence.change_seconds = f(7); presence.max_ground_forces = 10;
        for (const auto& player : setup.players)
            presence.players.push_back({player.player_id, player.player_id == 4, player.player_id != 4, player.faction_id});
        for (const auto& a : presence.players) for (const auto& b : presence.players)
            presence.relations.push_back({a.player, b.player, a.player == b.player});
        presence.types = {{7, 1, false, false}};
        for (const auto& planet : setup.planets) {
            gc::ControlPlanet row; row.planet = planet.id; row.type = 100 + planet.id;
            row.surface_accessible = true; row.ground_base = {planet.id == 1 ? 1U : 3U, 2};
            presence.planets.push_back(row);
        }
        for (const auto& route : setup.routes) presence.routes.push_back({route.id, f(1)});
        return cp::prepare_initial_control(setup, std::move(presence));
    };
    live.credit_cap = [&](const auto& player, const auto& setup) {
        expect(setup.control && setup.control->state[0].current_player == 1 && setup.control->state[1].current_player == 3,
            "complete initial control is retained before credit caps");
        expect(setup.planets[0].owner == 1 && setup.planets[1].owner == 3, "owners commit before credit caps");
        return Result<Fixed>::success(f(player.player_id * 100));
    };
    auto root = cp::CampaignRoot::create(plan(), live);
    expect(bool(root), "CampaignRoot accepts full initial control without an owner-only hook");
    if (root) {
        const auto& setup = root.value().game()->galaxy().setup();
        expect(setup.control && setup.control->state[0].current == f(2) && setup.control->state[0].target == f(2),
            "root retains CGS-02 current/target state without trade");
        expect(root.value().game()->galaxy().snapshot()->control()->state == setup.control->state,
            "session construction preserves root initial state");
    }
    const auto valid = live.initial_political_control;
    live.initial_political_control = [valid](const auto& p, const auto& setup) {
        auto result = valid(p, setup); result.value().state.front().target = f(4); return result;
    };
    expect(!cp::CampaignRoot::create(plan(), live), "partial current/target initialization refuses");
}
void victory_authoring_contracts() {
    auto d = data();
    d.campaigns[0].human_victory_conditions = {"gAlAcTiC_CrEdItS_Accrued", "Galactic_Credits_Accrued"};
    d.campaigns[0].ai_victory_conditions = {"Galactic_Cycles_Elapsed"};
    d.campaigns[0].human_victory_credit_target = 17;
    d.campaigns[0].ai_victory_cycle_limit = 60;
    auto opts = options(2); // starting faction is still REBEL, although a different seat is human.
    auto p = cp::plan_campaign_setup(d, types(), "Ordinary", opts).value();
    const auto& s = *p.galaxy().victory;
    expect(s.installed[0].player == 1 && s.installed[0].kind == gc::VictoryKind::credits
        && s.installed[1].kind == gc::VictoryKind::credits && s.installed[0].parameters.credits == 17,
        "WGS-15 starting faction chooses human list independently of human flag and retains duplicates");
    expect(s.installed[4].player == 2 && s.installed[4].kind == gc::VictoryKind::cycles
        && s.installed[4].parameters.cycles == 60, "WGS-16 nonstarting human-controlled faction chooses AI authored parameters");
    opts.mode = cp::CampaignSetupMode::multiplayer;
    p = cp::plan_campaign_setup(d, types(), "Ordinary", opts).value();
    expect(p.galaxy().victory->installed[5].kind == gc::VictoryKind::credits,
        "WGS-15 multiplayer uses human list for every nonneutral faction");
    d.campaigns[0].human_victory_conditions = {"Galactic_All_Planets_Controlled", "Galactic_Opponent_Controls_No_Planets",
        "Galactic_Credits_Accrued", "Galactic_Percentage_Control", "Galactic_Conquer_Enemy_Homeworld_And_Leader",
        "Galactic_Kill_Enemy_Leader", "Galactic_Control_Named_Planets", "Galactic_Cycles_Elapsed",
        "Galactic_Super_Weapon_Destruction", "Galactic_Super_Weapon_Destroys_Last_Enemy_Planet",
        "Galactic_Super_Weapon_Kills_Enemy_Leader"};
    const auto complete = cp::plan_campaign_setup(d, types(), "Ordinary", opts);
    expect(static_cast<bool>(complete), "WGS-83 every sourced authored victory enum is accepted by campaign setup");
    if (complete) expect(complete.value().galaxy().victory->installed[10].kind == gc::VictoryKind::superweapon_kill_leader,
        "WGS-83 authored order reaches the last event condition before defaults");
}
void planning() {
    auto input = data(); auto opts = options();
    const auto built = cp::plan_campaign_setup(input, types(), "ordinary", opts);
    expect(static_cast<bool>(built), "selected campaign resolves case insensitively"); if (!built) return;
    const auto& p = built.value();
    expect(p.galaxy().seed == 42 && p.first_free_entity_id() == 4 && !p.galaxy().debug_commands, "new root owns seed/frontier and disables debug inputs");
    expect(p.locations().size() == 3 && !p.locations()[0].planet && p.galaxy().planets.size() == 2, "galaxy art is a location, never a conquerable planet");
    expect(p.forces().size() == 4 && p.forces()[0].planet == 1 && p.forces()[3].planet == 2,
        "planet traversal precedes authored force traversal and skips missing owners");
    expect(p.initial_placements().size() == 1, "required special placement is marked only once per original type");
    auto omitted = types(); omitted.types[0].can_hyperspace_without_ability.reset();
    const auto unresolved = cp::plan_campaign_setup(data(), omitted, "Ordinary", options());
    expect(unresolved && unresolved.value().diagnostics().empty()
        && unresolved.value().galaxy().fleet_control->types.front().can_hyperspace_without_ability,
        "WGM-100 chain omission resolves to true after inheritance");
    auto unused = types(); unused.types[1].can_hyperspace_without_ability.reset();
    const auto unaffected = cp::plan_campaign_setup(data(), unused, "Ordinary", options());
    expect(unaffected && unaffected.value().diagnostics().empty() && unaffected.value().galaxy().fleet_control->types.size() == 1,
        "unused bound types do not participate in trait checks");
    if (unresolved) {
        std::vector<std::string> resolved_trace;
        expect(static_cast<bool>(cp::CampaignRoot::create(unresolved.value(), services(resolved_trace))),
            "live creation uses the sourced omitted movement default");
    }
    auto explicit_false = types(); explicit_false.types[0].can_hyperspace_without_ability = false;
    const auto immobile = cp::plan_campaign_setup(data(), explicit_false, "Ordinary", options());
    expect(immobile && !immobile.value().galaxy().fleet_control->types.front().can_hyperspace_without_ability,
        "explicit false movement flag remains false");
    auto bad_art = data(); bad_art.campaigns[0].planets.insert(bad_art.campaigns[0].planets.begin(), "Galaxy_Art");
    expect(!cp::plan_campaign_setup(bad_art, types(), "Ordinary", options()), "Planet-typed IDLE art cannot be admitted as a campaign planet");
    auto defaults = types(); defaults.types[0].build_requires_initial_placement.reset();
    const auto missing_placement = cp::plan_campaign_setup(data(), defaults, "Ordinary", options());
    expect(missing_placement && missing_placement.value().initial_placements().empty()
        && missing_placement.value().forces().size() == p.forces().size(),
        "WGS-72 omission skips special marking without suppressing starting forces");
    expect(p.homes()[0].planet == 1 && p.homes()[1].original_type == "Absent" && !p.homes()[1].planet
        && p.homes()[3].original_type.empty(), "first faction home match; missing context has no replacement");
    input.campaigns[0].starting_forces.clear(); opts.seed = 0;
    expect(p.forces().size() == 4 && p.galaxy().seed == 42, "plan owns immutable copies of loader and option inputs");
    expect(!cp::plan_campaign_setup(data(), types(), "Unknown", options()), "unknown definition refuses");
    opts = options(); opts.players.back().active = false;
    expect(!cp::plan_campaign_setup(data(), types(), "Ordinary", opts), "inactive neutral refuses");
    opts = options(); opts.types.push_back({9, "ship"});
    expect(!cp::plan_campaign_setup(data(), types(), "Ordinary", opts), "original type cannot get a second identity");
    opts = options(); opts.players[1].player.faction_id = opts.players[0].player.faction_id;
    expect(!cp::plan_campaign_setup(data(), types(), "Ordinary", opts), "different factions cannot alias their recorded identity");
    opts = options(); opts.seats[0].active = false;
    const auto disconnected = cp::plan_campaign_setup(data(), types(), "Ordinary", opts).value();
    expect(disconnected.forces()[0].human, "an inactive reconnectable human seat does not turn its owner into AI");
    opts = options(); opts.players[2].active = false; opts.seats[2].active = false;
    auto inactive = cp::plan_campaign_setup(data(), types(), "Ordinary", opts);
    expect(inactive && inactive.value().forces().size() == 3, "inactive force owner skipped");
    opts.seats[2].active = true;
    expect(!cp::plan_campaign_setup(data(), types(), "Ordinary", opts), "active seat cannot bind an inactive player");
    opts = options(); opts.forced_seed.reset();
    expect(cp::plan_campaign_setup(data(), types(), "Ordinary", opts).value().galaxy().seed == 999, "seed without override stays authored session input");
    expect(p.galaxy().campaign_identity != plan("Story").galaxy().campaign_identity, "selected campaign joins root identity");
    expect(p.galaxy().campaign_identity != inactive.value().galaxy().campaign_identity, "active owner selection joins root identity");
    input = data(); input.constants.pop_back();
    expect(!cp::plan_campaign_setup(input, types(), "Ordinary", options()), "missing fiscal input refuses instead of stock literal");
}
void order_and_parity() {
    std::vector<std::string> trace;
    auto root = cp::CampaignRoot::create(plan(), services(trace)).value();
    const std::vector<std::string> expected{"location:Galaxy_Art", "location:A", "location:B", "mark:1",
        "force:1:1", "unify:1:1", "force:1:1", "unify:1:1", "force:1:2", "force:2:3",
        "control:1", "control:2", "cap:1", "cap:2", "cap:3", "cap:4", "home:1", "home:2", "home:3", "home:4"};
    expect(trace == expected, "neutral locations, per-planet forces/guards, all control, caps and homes have separate ordered passes");
    expect(root.credit_caps().size() == 4 && root.game()->galaxy().next_entity_id() == 12, "root retains cap receipts and one shared identity frontier");
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        trace.clear(); auto running = cp::CampaignRoot::create(plan(), services(trace)).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 12; ++tick) {
            running.game()->scramble_storage_for_testing(); hashes.push_back(running.game()->step(executor).value().state_hash.get());
        }
        if (workers == 1) reference = hashes; else expect(hashes == reference, "authored setup state has 1/2/4/8 parity");
        const auto bytes = gc::write_replay(running.game()->record()).value();
        auto replay = cp::GameSession::from_replay(gc::parse_replay(bytes).value()).value();
        for (const auto& hash : hashes) expect(replay.step(executor).value().state_hash.get() == hash, "authored roster/control/identity replays under every worker count");
        const auto identities = eawr::checkpoint::game_identities(running.game()->galaxy().setup());
        const auto saved = eawr::checkpoint::save_game_checkpoint(*running.game(), identities).value();
        auto loaded = eawr::checkpoint::load_game_checkpoint(saved, identities, {}, executor);
        expect(loaded && loaded.value().session.state_sha256() == running.game()->state_sha256(), "created GameSession uses existing checkpoint composition");
    }
    for (const auto human : {1U, 2U, 3U}) for (const auto name : {"Ordinary", "Story", "Tutorial", "Multiplayer"}) {
        trace.clear(); auto variant = cp::CampaignRoot::create(plan(name, human), services(trace));
        expect(static_cast<bool>(variant), "three human-faction variants and ordinary/story/tutorial/multiplayer plans start");
        if (variant) expect(variant.value().game()->galaxy().setup().seats[human - 1].kind == gc::SeatKind::human, "selected human faction retains its seat");
    }
    root.stop_root(); root.stop_root();
    expect(!root.game() && !root.plan() && root.credit_caps().empty(), "final root stop clears owned galaxy and setup context, idempotently");
}
void observer_start() {
    auto faction = data().factions[0]; faction.minimum_visible_base_level = 7; faction.benefits_from_corruption = true;
    gc::ObserverPlayer explicit_player; explicit_player.minimum_visible_base_level = 2;
    const auto sourced_player = cp::prepare_observer_player(faction, explicit_player);
    expect(sourced_player.minimum_visible_base_level == 7 && sourced_player.benefits_from_corruption
        && sourced_player.default_intelligence == 17 && !sourced_player.canonical_empire,
        "observer adapter applies faction threshold/benefit and canonical default without enum ordinal");
    auto metadata_planet = data().planets[0]; metadata_planet.planet_starbase_stealth = false;
    metadata_planet.planet_groundbase_stealth = true;
    gc::ObserverPlanet explicit_planet; explicit_planet.space_stealth = true;
    const auto sourced_planet = cp::prepare_observer_planet(metadata_planet, explicit_planet);
    expect(!sourced_planet.space_stealth && sourced_planet.ground_stealth, "independent authored stealth flags override explicit producer values");
    auto opts = options(); opts.local_observer = 1;
    const auto selected = cp::plan_campaign_setup(data(), types(), "Ordinary", opts).value();
    std::vector<std::string> trace;
    auto live = services(trace);
    live.observer = [](const auto& context, const auto& setup) {
        expect(context.local_observer() == gc::PlayerId{1} && setup.player_economy.has_value(), "observer install follows authored setup and economy");
        gc::ObserverSetup observer; observer.reveal_delay_seconds = f(1); observer.automatic_reveal_range = f(10);
        const auto metadata = data();
        for (std::size_t i = 0; i < setup.players.size(); ++i) {
            gc::ObserverPlayer player; player.player = setup.players[i].player_id;
            player.ai = player.player != 1; player.minimum_visible_base_level = 3; player.allies = {player.player};
            observer.players.push_back(cp::prepare_observer_player(metadata.factions[i], player));
        }
        for (std::size_t i = 0; i < setup.planets.size(); ++i) {
            const auto& source = setup.planets[i];
            gc::ObserverPlanet planet; planet.planet = source.id;
            planet.allegiance = planet.target_owner = planet.current_owner = planet.current_controller = source.owner;
            planet.political_control = f(1); planet.automatic_reveal = true;
            observer.planets.push_back(cp::prepare_observer_planet(metadata.planets[i], planet));
        }
        observer.types = {{7, false, false, false}}; // explicit action/runtime traits
        return Result<gc::ObserverSetup>::success(std::move(observer));
    };
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto root = cp::CampaignRoot::create(selected, live).value();
        const auto initial = root.game()->galaxy().snapshot();
        expect(initial->observer() && initial->observations().size() == 8, "real campaign publishes complete per-player observations at tick zero");
        expect(initial->observations()[0].discovered && !initial->observations()[1].discovered,
            "WGS-06 startup reveals only the first authored matching local home, without topology expansion");
        expect(initial->observer()->records[0].apparent_owner == -1 && initial->observer()->records[0].mask != 0,
            "WGUI-99 home receives explicit clear and recomputation");
        for (std::size_t i = 1; i < initial->observer()->records.size(); ++i) {
            const auto& row = initial->observer()->records[i];
            expect(!row.discovered && row.reveal_counter == 0 && row.mask == 0 && row.apparent_owner == 0,
                "nonhome records retain fresh constructor values, including AI records");
        }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto identities = eawr::checkpoint::game_identities(root.game()->galaxy().setup());
        const auto save = eawr::checkpoint::save_game_checkpoint(*root.game(), identities).value();
        auto restored = eawr::checkpoint::load_game_checkpoint(save, identities, {}, executor).value();
        expect(restored.session.galaxy().snapshot()->sha256() == initial->sha256(), "tick-zero observer checkpoint round trip");
        std::vector<std::string> hashes;
        for (int i = 0; i < 5; ++i) {
            const auto tick = root.game()->step(executor).value(); hashes.push_back(tick.state_hash.get());
            expect(restored.session.step(executor).value().state_hash.get() == hashes.back(), "observer startup checkpoint continuation");
        }
        if (workers == 1) reference = hashes; else expect(hashes == reference, "observer startup 1/2/4/8 equality");
        auto replay = cp::GameSession::from_replay(gc::parse_replay(gc::write_replay(root.game()->record()).value()).value()).value();
        expect(replay.galaxy().snapshot()->sha256() == initial->sha256(), "replay starts from installed observer state");
        for (const auto& hash : hashes) expect(replay.step(executor).value().state_hash.get() == hash, "observer start replay continuation");
    }
    auto no_home_data = data(); no_home_data.campaigns[0].home_locations.erase(no_home_data.campaigns[0].home_locations.begin(),
        no_home_data.campaigns[0].home_locations.begin() + 2);
    const auto no_home = cp::plan_campaign_setup(no_home_data, types(), "Ordinary", opts).value();
    auto missing = cp::CampaignRoot::create(no_home, live).value();
    expect(std::none_of(missing.game()->galaxy().snapshot()->observations().begin(), missing.game()->galaxy().snapshot()->observations().end(),
        [](const auto& row) { return row.discovered; }), "missing home never invents a replacement or startup flood");
    expect(!cp::CampaignRoot::create(plan(), live), "observer provider requires explicit local identity");
    auto retained = live;
    retained.observer = [live](const auto& context, const auto& setup) {
        auto result = live.observer(context, setup); result.value().records.push_back({}); return result;
    };
    expect(!cp::CampaignRoot::create(selected, retained), "startup rejects retained or fabricated observer records");
    auto sourced_rules = data();
    sourced_rules.constants.push_back({"Planet_Reveal_Delay_Time", "0.5", Fixed::from_decimal("0.5").value()});
    sourced_rules.campaigns[0].planet_auto_reveal = false;
    const auto sourced_plan = cp::plan_campaign_setup(sourced_rules, types(), "Ordinary", opts).value();
    const auto sourced_root = cp::CampaignRoot::create(sourced_plan, live).value();
    const auto& installed = *sourced_root.game()->galaxy().setup().observer;
    expect(installed.reveal_delay_seconds == Fixed::from_decimal("0.5").value() && installed.automatic_reveal_range == f(10)
        && std::none_of(installed.planets.begin(), installed.planets.end(), [](const auto& planet) { return planet.automatic_reveal; }),
        "authored delay, movement range and explicit false auto-reveal override provider rule inputs");
}
void layout_observer() {
    auto authored = data(); authored.constants.push_back({"Planet_Reveal_Delay_Time", "0.5", Fixed::from_decimal("0.5").value()});
    authored.campaigns[0].planet_auto_reveal = false;
    const auto selected = cp::plan_campaign_setup(authored, types(), "Ordinary", options()).value();
    auto layout = selected.galaxy();
    expect(static_cast<bool>(cp::initialize_layout_observer(authored, selected, 1, layout)), "neutral layout installs sourced preview observers");
    const auto game = gc::GalacticSession::create(layout).value();
    expect(game.snapshot()->observations()[0].discovered && !game.snapshot()->observations()[1].discovered,
        "preview publishes the actual authored local home while force/control initialization is pending");
    expect(game.snapshot()->observations()[0].known_owner == gc::PlayerId{4} && game.snapshot()->fleets().empty(),
        "preview observer reports real neutral-layout owner, never starting-force inferred ownership");
    auto invalid_layout = selected.galaxy(); invalid_layout.fleets = {{10, 1, 1, {{11, 7}}}};
    expect(!cp::initialize_layout_observer(authored, selected, 1, invalid_layout) && !invalid_layout.observer,
        "layout producer refuses already placed forces atomically without inventing runtime traits");
    invalid_layout = selected.galaxy();
    expect(!cp::initialize_layout_observer(authored, selected, 99, invalid_layout), "unknown preview local observer refused");
    const auto absent_rules = plan(); invalid_layout = absent_rules.galaxy();
    expect(!cp::initialize_layout_observer(data(), absent_rules, 1, invalid_layout), "missing authored preview rules refuse instead of guessing");
    authored.campaigns[0].planet_auto_reveal.reset();
    const auto default_policy = cp::plan_campaign_setup(authored, types(), "Ordinary", options()).value();
    expect(default_policy.observer_auto_reveal() == true, "GOBS-01 missing campaign auto-reveal retains the verified enabled constructor default");
}
void provider_refusals() {
    auto nested_types = types();
    const auto nested_plan = cp::plan_campaign_setup(data(), nested_types, "Ordinary", options()).value();
    std::vector<std::string> nested_trace;
    auto nested_services = services(nested_trace);
    nested_services.place = [](const cp::StartingForceRequest& request, const auto next, const auto&) {
        return Result<cp::StartingForcePlacement>::success({{{next, request.owner, request.planet, {{next + 1, request.type}, {next + 2, 8}}}}, {}, next + 3, true, {}, {}, {}, {}, {}, 0, {}, {}});
    };
    const auto nested_root = cp::CampaignRoot::create(nested_plan, nested_services);
    expect(nested_root && nested_root.value().game()->galaxy().setup().fleet_control->types.size() == 2,
        "provider-expanded bound child types resolve verified traits at live placement");
    nested_types.types[1].can_hyperspace_without_ability.reset();
    const auto unresolved_child_plan = cp::plan_campaign_setup(data(), nested_types, "Ordinary", options()).value();
    expect(unresolved_child_plan.diagnostics().empty(), "unused unresolved child is not checked during planning");
    const auto unresolved_child = cp::CampaignRoot::create(unresolved_child_plan, nested_services);
    expect(unresolved_child && unresolved_child.value().game()->galaxy().setup().fleet_control->types.back().can_hyperspace_without_ability,
        "provider-expanded child also receives WGM-100's shared default");
    std::vector<std::string> trace;
    auto absent = services(trace); absent.place = {};
    auto missing = cp::CampaignRoot::create(plan(), absent);
    expect(!missing && missing.error().message.find("WGS-U08") != std::string::npos && trace.empty(), "missing placement refuses before staged work");
    auto bad = services(trace);
    bad.place = [](const auto&, const auto, const auto&) { return refusal<cp::StartingForcePlacement>(); };
    expect(!cp::CampaignRoot::create(plan(), bad), "unimplemented nested production provider refuses instead of losing force");
    bad = services(trace);
    bad.place = [](const auto& request, const auto next, const auto&) {
        return Result<cp::StartingForcePlacement>::success({{{next, request.owner, request.planet, {{next, request.type}}}}, {}, next + 2, true, {}, {}, {}, {}, {}, 0, {}, {}});
    };
    expect(!cp::CampaignRoot::create(plan(), bad), "provider cannot alias fleet/member IDs");
    bad = services(trace);
    bad.political_control = [](const auto&, auto, const auto&) { return Result<gc::PlayerId>::success(900); };
    expect(!cp::CampaignRoot::create(plan(), bad), "control provider cannot assign nonexistent player");
    bad = services(trace);
    bad.unify_human_guard_fleets = [](auto, auto, const auto&) { return Result<std::vector<gc::Fleet>>::success({}); };
    expect(!cp::CampaignRoot::create(plan(), bad), "guard unification cannot discard persistent units");
    bad = services(trace);
    bad.credit_cap = [](const auto&, const auto&) { return refusal<Fixed>(); };
    expect(!cp::CampaignRoot::create(plan(), bad), "credit-cap refusal publishes no root");
}
void retained_parent() {
    namespace tactical = eawr::sim::tactical;
    namespace auth = eawr::script::authoritative;
    std::vector<std::string> trace; auto root = cp::CampaignRoot::create(plan(), services(trace)).value();
    auth::SessionConfig script_config; script_config.seed = 42; script_config.galactic_clock = true;
    auth::ModuleManifest modules;
    expect(static_cast<bool>(modules.add("Root.lua", "RootMarker = 991\nCount = 0\nfunction Main() while true do Count = Count + 1 coroutine.yield(true) end end\nCreate_Thread('Main')\n")), "root global script admitted");
    auto story = auth::ScriptScheduler::create(script_config, std::move(modules)).value();
    expect(static_cast<bool>(story.create_instance(1, "Root.lua")), "root Lua globals created once");
    expect(static_cast<bool>(root.game()->install_galactic_scripts(auth::ScriptScheduler::create(script_config, {}).value(), std::move(story))),
        "root owns galactic script host");
    const auto* script_host = root.game()->galactic_scripts();
    const auto* context = root.plan(); const auto seed = root.game()->galaxy().setup().seed; const auto call_count = trace.size();
    const auto economy = root.game()->galaxy().setup().player_economy;
    cp::ConflictRecord conflict; conflict.id = root.game()->galaxy().next_entity_id(); conflict.planet = 1;
    conflict.invader = 1; conflict.defender = 2;
    conflict.tactical.content_identity = root.game()->galaxy().setup().content_identity;
    conflict.tactical.first_free_entity_id = conflict.id + 1;
    conflict.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    for (const auto& fleet : root.game()->galaxy().setup().fleets) {
        if (fleet.owner > 2 || std::any_of(conflict.forces.begin(), conflict.forces.end(), [&](const auto& row) { return row.owner == fleet.owner; })) continue;
        const auto member = fleet.members.front(); conflict.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
        tactical::UnitState unit; unit.entity_id = member.id; unit.type_id = member.type; unit.owner = fleet.owner;
        conflict.tactical.units.push_back(unit);
    }
    conflict.seats = {{gc::SeatId{1}, 1}, {gc::SeatId{2}, 2}};
    tactical::TacticalContent content;
    tactical::DurabilityProfile durability; durability.type_id = 7; durability.max_hull = f(100); content.durability.profiles.push_back(durability);
    content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed; content.victory.contenders = {1, 2};
    content.victory.controlled_players = {1, 2}; content.victory.humans = {1}; content.victory.relevant_types = {7}; content.victory.countdown_frames = 2;
    auto staged = root.game()->stage_battle(conflict, content);
    expect(static_cast<bool>(staged), "authored root stages ordinary child"); if (!staged) return;
    const eawr::sim::InlineExecutor executor;
    static_cast<void>(root.game()->step(executor).value());
    const auto frozen = root.game()->galaxy().clock().galactic_frame;
    const auto frozen_scripts = root.game()->save_galactic_scripts().value().value();
    tactical::TacticalReplay wrapper; wrapper.setup.players = conflict.tactical.players; wrapper.final_tick_count = 1;
    wrapper.commands = {{{0, 1, 1}, {conflict.forces.back().id}, tactical::DamagePayload{f(1000)}}};
    const auto bytes = tactical::write_replay(wrapper).value();
    const auto header = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8U);
    const auto start = header + wrapper.setup.players.size() * 24 + 4;
    gc::TacticalLayerCommand input{conflict.id, bytes[start + 20], {bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end()}};
    expect(static_cast<bool>(root.game()->submit({{1, gc::SeatId{1}, 1}, input})), "child action submitted through seat envelope");
    while (root.game()->mode() == cp::SessionMode::battle && root.game()->completed_tick() < 12) {
        static_cast<void>(root.game()->step(executor).value());
        expect(root.game()->galaxy().clock().galactic_frame == frozen, "child retains frozen galaxy");
        const auto scripts = root.game()->save_galactic_scripts().value().value();
        expect(root.game()->galactic_scripts() == script_host && scripts.ai == frozen_scripts.ai && scripts.story == frozen_scripts.story,
            "child stop/return retains the same root host and Lua globals without script reinitialization");
    }
    expect(root.game()->mode() == cp::SessionMode::galaxy && root.plan() == context && trace.size() == call_count
        && root.game()->galaxy().setup().seed == seed, "child return keeps root context/seed and invokes no campaign setup pass");
    expect(root.game()->galaxy().next_entity_id() > conflict.id, "child allocator reconciles without resetting root frontier");
    expect(root.game()->galaxy().setup().player_economy == economy, "child return retains committed root credits/tech/caps");
    root.stop_root(); expect(!root.game() && !root.plan(), "root teardown after child return releases retained parent");
}

std::optional<std::string> game_root() {
#if defined(_MSC_VER)
    char* value = nullptr; std::size_t size{};
    if (_dupenv_s(&value, &size, "EAWR_EAW_GAME_ROOT") != 0 || !value) return std::nullopt;
    std::string result(value); std::free(value); return result.empty() ? std::nullopt : std::optional<std::string>(result);
#else
    const auto* value = std::getenv("EAWR_EAW_GAME_ROOT");
    return value && *value ? std::optional<std::string>(value) : std::nullopt;
#endif
}
void installed() {
    const auto root = game_root();
    if (!root) { std::cout << "SKIPPED: stock campaign setup needs EAWR_EAW_GAME_ROOT\n"; return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}}) {
        auto mount = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(mount), "stock manifest mount"); if (!mount) return;
        mounts.push_back(std::move(mount).value().mount);
    }
    const auto filesystem_owner = std::make_shared<const eawr::vfs::Vfs>(eawr::vfs::Vfs::mount(mounts).value());
    const auto& filesystem = *filesystem_owner;
    const auto catalog = eawr::data::load_catalog(filesystem, eawr::data::Profile::foc).value();
    const auto loaded = dg::load_galactic_data(filesystem, catalog.catalog).value();
    expect(!loaded.planets.empty() && std::all_of(loaded.planets.begin(), loaded.planets.end(),
        [](const auto& planet) { return planet.planet_credit_value.has_value(); }),
        "stock planetary behavior records retain validated Planet_Credit_Value for WGUI-13 basic display income");
    const auto table = eawr::units::galactic_type_table(catalog.catalog);
    cp::CampaignSetupOptions opts; opts.seed = 42;
    for (const auto& faction : loaded.factions) {
        const auto id = static_cast<gc::PlayerId>(opts.players.size() + 1);
        opts.players.push_back({{id, id, 0}, faction.name, true});
        if (!faction.is_neutral.value_or(false)) opts.seats.push_back({gc::SeatId{id}, id, gc::SeatKind::ai, id - 1, true, 0});
    }
    for (const auto& type : table.types) opts.types.push_back({opts.types.size() + 1, type.name});
    std::size_t count{};
    std::optional<gc::GalacticSetup> largest_observer_layout;
    bool stock_live_creation_checked{};
    for (const auto& campaign : loaded.campaigns) {
        if (campaign.planets.empty()) continue;
        const auto graph = cp::build_campaign_galaxy(loaded, table, campaign, opts.players.back().player.player_id, 1);
        expect(static_cast<bool>(graph), "stock campaign graph " + campaign.name);
        if (graph) {
            expect(graph.value().planets.size() == campaign.planets.size(), "stock graph contains only qualified planets");
            expect(graph.value().routes.size() == campaign.trade_routes.size(), "stock graph retains authored routes");
            for (const auto& name : graph.value().names)
                expect(!loaded.galaxy_art_object(name.name), "stock art has no graph identity or political owner");
        }
        expect(loaded.galaxy_art_object("Galaxy_Core_Art_Model") && !loaded.planet("Galaxy_Core_Art_Model"),
            "stock backdrop remains classified as art");
        const auto built = cp::plan_campaign_setup(loaded, table, campaign.name, opts);
        expect(static_cast<bool>(built), "stock authored setup plan " + campaign.name);
        if (!built) { std::cerr << eawr::core::format_diagnostic(built.error()) << '\n'; continue; }
        ++count;
        auto observer_layout = built.value().galaxy();
        const auto installed_observer = cp::initialize_layout_observer(loaded, built.value(), opts.players.front().player.player_id, observer_layout);
        expect(static_cast<bool>(installed_observer), "stock campaign preview installs sourced observer layout " + campaign.name);
        if (installed_observer && (!largest_observer_layout || observer_layout.planets.size() > largest_observer_layout->planets.size()))
            largest_observer_layout = std::move(observer_layout);
        expect(built.value().diagnostics().empty(), "sourced stock defaults leave no unresolved movement diagnostics");
        for (const auto& force : built.value().forces()) {
            const auto resolved = built.value().resolve_fleet_type(force.type);
            const auto* original = table.find(force.original_type);
            expect(resolved && original && resolved.value().can_hyperspace_without_ability
                == original->can_hyperspace_without_ability.value_or(true),
                "stock traits use inherited/authored values before WGM-100 default");
        }
        if (!stock_live_creation_checked && !built.value().forces().empty()) {
            cp::CampaignSetupServices live;
            live.location = [](const auto&) { return Result<void>::success(); };
            live.mark_initial_placement = [](const auto&) { return Result<void>::success(); };
            // Test representation only: production scene/ground/company dispatch remains WGS-U08.
            live.place = [](const cp::StartingForceRequest& request, const auto next, const auto&) {
                return Result<cp::StartingForcePlacement>::success({{{next, request.owner, request.planet, {{next + 1, request.type}}}}, {}, next + 2, true, {}, {}, {}, {}, {}, 0, {}, {}});
            };
            live.unify_human_guard_fleets = [](const auto, const auto, const auto& setup) {
                return Result<std::vector<gc::Fleet>>::success(setup.fleets);
            };
            live.political_control = [](const auto& planet, auto, const auto&) { return Result<gc::PlayerId>::success(planet.owner); };
            live.credit_cap = [](const auto&, const auto&) { return Result<Fixed>::success(f(0)); };
            live.home = [](const auto&) { return Result<void>::success(); };
            live.assign_tech = [](const auto& request, const auto&) { return Result<std::int64_t>::success(request.requested_current); };
            const auto created = cp::CampaignRoot::create(built.value(), live);
            expect(static_cast<bool>(created), "stock campaign creates live with sourced defaults and explicit test services");
            if (created) {
                expect(created.value().game()->galaxy().setup().fleets.size() == built.value().forces().size(),
                    "every stock authored force reaches live placement");
                std::cout << "stock live creation: " << campaign.name << '\n';
            } else std::cerr << eawr::core::format_diagnostic(created.error()) << '\n';
            stock_live_creation_checked = true;
        }
        expect(built.value().locations().size() == campaign.locations.size(), "every authored scene/planet location retained");
        expect(built.value().galaxy().planets.size() == campaign.planets.size(), "only qualified planetary locations become owned planets");
        for (const auto& location : built.value().locations()) if (loaded.galaxy_art_object(location.original_type))
            expect(!location.planet, "stock Planet-typed backdrop has no planetary identity or control owner");
        expect(loaded.galaxy_art_object("Galaxy_Core_Art_Model") && !loaded.planet("Galaxy_Core_Art_Model"),
            "stock core backdrop is classified as art by the sourced loader boundary");
        expect(built.value().galaxy().routes.size() == campaign.trade_routes.size(), "stock authored routes retained");
        std::cout << "stock setup " << campaign.name << ": locations=" << built.value().locations().size()
                  << " forces=" << built.value().forces().size() << " homes=" << built.value().homes().size() << '\n';
    }
    expect(count >= 30, "stock ordinary/story/tutorial/multiplayer campaign coverage");
    expect(stock_live_creation_checked, "stock live creation exercised with authored starting forces");
    const auto live_plan = cp::plan_live_campaign_setup(filesystem, loaded, table,
        "Sandbox_Equal_Footing_Underworld", opts, "Normal_Default");
    expect(static_cast<bool>(live_plan), "stock preview prepares live fiscal state");
    if (!live_plan) { std::cerr << eawr::core::format_diagnostic(live_plan.error()) << '\n'; return; }
    auto setup = live_plan.value().galaxy();
    {
        auto preview_options = opts;
        const auto local_binding = std::find_if(preview_options.players.begin(), preview_options.players.end(),
            [&](const auto& binding) { return loaded.faction(binding.faction) == loaded.faction("UNDERWORLD"); });
        expect(local_binding != preview_options.players.end(), "stock preview binds the authored starting faction");
        if (local_binding == preview_options.players.end()) return;
        preview_options.local_observer = local_binding->player.player_id;
        for (auto& seat : preview_options.seats) if (seat.player == *preview_options.local_observer)
            seat = {seat.seat_id, seat.player, gc::SeatKind::human, 0, true, 1};
        const auto prepared = eawr::platform::PreparedLiveCampaign::create(filesystem_owner, catalog.catalog,
            loaded, table, "Sandbox_Equal_Footing_Underworld", preview_options, "Normal_Default");
        expect(static_cast<bool>(prepared), "stock preview creates the production live campaign");
        if (!prepared) { std::cerr << eawr::core::format_diagnostic(prepared.error()) << '\n'; return; }
        const auto publication = prepared.value()->snapshot();
        expect(publication->observer().has_value(), "stock preview publishes the local observer");
        if (publication->observer()) {
            const auto local = publication->observer()->local_player;
            const auto sight = eawr::viewer::galactic::live_fleet_visibility(*publication);
            const auto fleets = gc::observed_fleets(*publication, local, {}, 8, sight);
            std::size_t home_fleets{};
            for (const auto& home : prepared.value()->plan().homes()) {
                if (home.player != local || !home.planet) continue;
                for (const auto& fleet : publication->fleets()) {
                    if (fleet.owner != local || fleet.orbit != *home.planet) continue;
                    ++home_fleets;
                    // WGEP-40: every local fleet of a slot shares one card, which
                    // names the first fleet; the others are held by its groups.
                    expect(std::any_of(fleets.cards.begin(), fleets.cards.end(),
                        [&](const auto& card) { return card.local && gc::holds_fleet(card, fleet.id); }),
                        "live stock home fleet has a selectable observer-local card");
                }
            }
            expect(home_fleets > 0, "stock preview exercises local home fleets through live current sight");
            std::cout << "stock live home fleet cards: " << home_fleets << '\n';
            // WGUI-72 (#3079): a live group shows its representative's original
            // type, so an orbiting company is its company, not its transport.
            std::size_t remapped{};
            for (const auto& card : fleets.cards) for (const auto& group : card.groups) {
                if (!card.local || !group.representative) continue;
                const auto record = std::find_if(publication->actor_records().begin(), publication->actor_records().end(),
                    [&](const auto& row) { return row.id == *group.representative; });
                if (record == publication->actor_records().end() || !record->original_type) continue;
                expect(group.type == record->original_type, "live fleet group uses the representative's original type");
                remapped += record->original_type != record->current_type;
            }
            std::cout << "stock live fleet groups shown by a differing original type: " << remapped << '\n';
        }
    }
    const auto recipient = std::find_if(setup.fiscal->players.begin(), setup.fiscal->players.end(),
        [](const auto& player) { return player.earns_planet_income; });
    expect(recipient != setup.fiscal->players.end(), "stock preview has a playable fiscal recipient");
    if (recipient == setup.fiscal->players.end()) return;
    const auto index = static_cast<std::size_t>(recipient - setup.fiscal->players.begin());
    // Explicit test control provider: production live ownership is a separate WGS-U08 service.
    const auto ownership_provider = [&](const auto&) { return recipient->player; };
    Fixed income;
    for (auto& planet : setup.planets) planet.owner = ownership_provider(planet);
    expect(setup.fiscal->contributions.has_value(), "stock fiscal contribution profiles are authored");
    if (!setup.fiscal->contributions) return;
    const auto& authored = *setup.fiscal->contributions;
    // WGEP-09: this explicit all-one-owner fixture makes every admitted route
    // allied. Independently fold positive factors by endpoint before multiplying.
    std::map<gc::PlanetId, Fixed> route_factors;
    for (const auto& planet : setup.planets) route_factors.emplace(planet.id, Fixed{});
    for (std::size_t route = 0; route < setup.routes.size(); ++route) {
        const auto factor = authored.routes[route].factor;
        if (factor.raw() <= 0) continue;
        for (const auto endpoint : {setup.routes[route].from, setup.routes[route].to})
            route_factors.at(endpoint) = eawr::sim::math::add(route_factors.at(endpoint), factor).value();
    }
    const auto player_alignment = authored.factions[index].alignment;
    for (std::size_t planet = 0; planet < setup.fiscal->planets.size(); ++planet) {
        const auto& basic = setup.fiscal->planets[planet];
        const auto& profile = authored.planets[planet];
        auto amount = basic.credits;
        // WGEP-07: no alignment contribution for a neutral faction or planet.
        if (player_alignment != gc::IncomeAlignment::neutral && profile.alignment != gc::IncomeAlignment::neutral) {
            const bool matching = player_alignment == profile.alignment;
            const auto strength = eawr::sim::math::multiply(basic.credits, profile.strength).value();
            const auto alignment = eawr::sim::math::multiply(strength,
                matching ? authored.maximum_alignment_bonus : authored.maximum_alignment_penalty).value();
            amount = matching ? eawr::sim::math::add(amount, alignment).value()
                : eawr::sim::math::subtract(amount, alignment).value();
        }
        const auto trade = eawr::sim::math::multiply(basic.credits, route_factors.at(basic.planet)).value();
        amount = eawr::sim::math::add(amount, trade).value();
        // No bonus producers in this layout; political/corruption writers keep
        // their documented zero/one defaults. Do not call the shared provider.
        income = eawr::sim::math::add(income, amount).value();
    }
    const auto initial = setup.player_economy->players[index].credits;
    const auto cap = eawr::sim::math::multiply(f(static_cast<std::int64_t>(setup.planets.size())), setup.fiscal->credit_cap_per_planet).value();
    setup.player_economy->players[index].credit_cap = cap;
    const auto expected = std::min(cap, eawr::sim::math::add(initial,
        eawr::sim::math::multiply(income, recipient->positive_credit_multiplier).value()).value());
    std::string reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto live = cp::GameSession::create(setup).value();
        expect(live.galaxy().clock().fiscal_cycle == 1, "stock preview begins at fiscal cycle one");
        const auto ids = eawr::checkpoint::game_identities(setup);
        const auto checkpoint = eawr::checkpoint::save_game_checkpoint(live, ids).value();
        auto restored = eawr::checkpoint::load_game_checkpoint(checkpoint, ids, {}, executor).value();
        expect(restored.session.state_sha256() == live.state_sha256(), "stock fiscal tick-zero checkpoint restores");
        for (std::uint32_t tick = 0; tick <= setup.rules.fiscal_cycle_frames; ++tick) {
            static_cast<void>(live.step(executor).value());
            static_cast<void>(restored.session.step(executor).value());
        }
        expect(expected != initial && live.galaxy().snapshot()->player_economy()->players[index].credits == expected,
            "stock first income tick pays authored base, alignment and trade with selected difficulty and cap");
        expect(restored.session.state_sha256() == live.state_sha256(), "stock fiscal checkpoint continuation equals uninterrupted run");
        if (reference.empty()) reference = live.state_sha256();
        else expect(live.state_sha256() == reference, "stock fiscal 1/2/4/8 equality");
        std::cout << "stock live fiscal: workers=" << workers << " cycle=1 first payment verified, tick-zero restore verified\n";
    }
    if (largest_observer_layout) {
        const auto& layout = *largest_observer_layout;
        constexpr std::size_t iterations = 2000;
        std::size_t rows{};
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < iterations; ++i)
            rows += gc::observed_planets(*layout.observer, layout.fleets).size();
        const auto end = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
        auto session = gc::GalacticSession::create(layout).value();
        const eawr::platform::ThreadWorkerAdapter benchmark_executor(1);
        const auto step_begin = std::chrono::steady_clock::now();
        std::uint64_t rebuilt_records{}, clean_publications{};
        for (std::size_t i = 0; i < iterations; ++i) {
            const auto tick = session.step(benchmark_executor).value();
            rebuilt_records += tick.work.observer_projection_records;
            clean_publications += tick.work.observer_projection_records == 0;
        }
        expect(rebuilt_records <= layout.observer->records.size(), "stock idle layout rebuilds at most one projection while discovery settles");
        expect(clean_publications >= iterations - 1, "stock idle layout shares observations on all subsequent publications");
        const auto step_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - step_begin).count();
        std::cout << "observer projection benchmark: planets=" << layout.planets.size() << " players=" << layout.players.size()
                  << " iterations=" << iterations << " rows=" << rows << " rebuilt_records=" << rebuilt_records
                  << " clean_publications=" << clean_publications << " projection_us=" << us << " step_us=" << step_us << '\n';
    }
}
} // namespace
int main(int argc, const char* const[]) {
    if (argc > 1) installed(); else { full_initial_control(); victory_authoring_contracts(); planning(); order_and_parity(); observer_start(); layout_observer(); provider_refusals(); retained_parent(); }
    return galactic_test::failures == 0 ? 0 : 1;
}
