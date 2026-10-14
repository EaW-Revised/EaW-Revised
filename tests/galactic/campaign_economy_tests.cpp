#include "galactic_support.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <filesystem>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace dg = eawr::data::galactic;
using galactic_test::expect;
using eawr::core::Result;
using Fixed = eawr::sim::math::Fixed;
Fixed f(std::int64_t n) { return Fixed::from_integer(n).value(); }
dg::GalacticData data() {
    dg::GalacticData d;
    for (const auto& name : {"REBEL", "EMPIRE", "UNDERWORLD", "NEUTRAL"}) {
        dg::Faction row; row.name = name; row.is_neutral = std::string_view(name) == "NEUTRAL";
        row.displayed_tech_level_adjustment = row.is_neutral.value() ? 0 : (std::string_view(name) == "REBEL" ? 7 : -3);
        if (!*row.is_neutral) row.multiplayer_campaign_heroes = {"Hero", "Hero"};
        d.factions.push_back(row);
    }
    dg::Planet p; p.name = "P"; p.galactic_position = dg::Position{}; p.behaviors = {"PLANET"}; d.planets.push_back(p);
    d.constants = {{"MaximumFleetMovementDistance", "10", f(10)}, {"Fiscal_Cycle_Time_In_Secs", "45", f(45)}};
    dg::Campaign c; c.name = "Ordinary"; c.locations = {"P"}; c.planets = {"P"}; c.supports_custom_settings = true;
    c.starting_credits = {{"rebel", 12000}, {"REBEL", 8000}, {"EMPIRE", 9000}, {"UNDERWORLD", 10000}};
    c.starting_tech_levels = {{"REBEL", 2}, {"rebel", 4}, {"EMPIRE", 3}, {"UNDERWORLD", 1}};
    c.max_tech_levels = {{"REBEL", 4}, {"rebel", 1}, {"EMPIRE", 5}};
    c.home_locations = {{"REBEL", "P"}, {"EMPIRE", "P"}, {"UNDERWORLD", "P"}};
    d.campaigns.push_back(c);
    c.name = "Story"; c.is_story_campaign = true; c.supports_custom_settings = false; d.campaigns.push_back(c);
    c.name = "Tutorial"; c.tutorial = true; d.campaigns.push_back(c);
    c.name = "Multiplayer"; c.is_multiplayer = true; c.supports_custom_settings = true; d.campaigns.push_back(c);
    return d;
}
eawr::units::GalacticTypeTable types() {
    eawr::units::GalacticType ship; ship.name = "Ship"; ship.object_class = "SpaceUnit"; ship.can_hyperspace_without_ability = true;
    auto hero = ship; hero.name = "Hero"; eawr::units::GalacticTypeTable table; table.types = {ship, hero}; return table;
}
cp::CampaignSetupOptions options(gc::PlayerId human = 1) {
    cp::CampaignSetupOptions out; out.seed = 42;
    out.players = {{{1, 1, 0}, "REBEL", true}, {{2, 2, 0}, "EMPIRE", true}, {{3, 3, 0}, "UNDERWORLD", true}, {{4, 4, 0}, "NEUTRAL", true}};
    for (gc::PlayerId id = 1; id <= 3; ++id)
        out.seats.push_back({gc::SeatId{id}, id, id == human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, id == human ? 1U : 0U});
    out.types = {{7, "Ship"}, {8, "Hero"}}; return out;
}
struct Trace { std::vector<std::string> calls; bool production{true}; bool original_exists{}; bool created{true}; };
cp::CampaignSetupServices services(Trace& trace) {
    cp::CampaignSetupServices s;
    s.location = [&](const auto&) { trace.calls.push_back("location"); return Result<void>::success(); };
    s.political_control = [&](const auto&, const auto&, const auto&) { trace.calls.push_back("control"); return Result<gc::PlayerId>::success(1); };
    s.credit_cap = [&](const auto&, const auto&) { trace.calls.push_back("cap"); return Result<Fixed>::success(f(100)); };
    s.home = [&](const auto&) { trace.calls.push_back("home"); return Result<void>::success(); };
    s.assign_tech = [&](const cp::CampaignTech& request, const auto& setup) {
        trace.calls.push_back("tech");
        expect(setup.player_economy && setup.player_economy->players.size() == 4, "all credits assigned before tech");
        expect(request.unlock_initial_types && !request.notify_new_options, "startup tech side-effect flags");
        // Fixture provider with a prior max of five; current is committed before new max.
        return Result<std::int64_t>::success(std::min<std::int64_t>(5, std::max<std::int64_t>(0, request.requested_current)));
    };
    s.home_has_production = [&](const auto&) { trace.calls.push_back("production"); return Result<bool>::success(trace.production); };
    s.owned_original_types = [&](const auto, const auto&) {
        trace.calls.push_back("owned-originals");
        return Result<std::vector<gc::TypeId>>::success(trace.original_exists ? std::vector<gc::TypeId>{8} : std::vector<gc::TypeId>{});
    };
    s.place_hero = [&](const cp::CampaignHero& hero, const auto next, const auto& setup) {
        trace.calls.push_back("hero"); expect(!setup.player_economy, "hero pass precedes credits");
        cp::StartingForcePlacement placed; placed.next_id = next;
        if (trace.created) {
            placed.created = true; placed.next_id += 3;
            placed.fleets = {{next, hero.player, hero.home, {{next + 1, hero.original_type_id}, {next + 2, 7}}}};
        }
        return Result<cp::StartingForcePlacement>::success(std::move(placed));
    };
    return s;
}
cp::CampaignRoot root(const dg::GalacticData& d, const cp::CampaignSetupOptions& opts, Trace& trace, std::string_view name = "Ordinary") {
    auto planned = cp::plan_campaign_setup(d, types(), name, opts);
    if (!planned) throw std::runtime_error(planned.error().message);
    auto created = cp::CampaignRoot::create(std::move(planned).value(), services(trace));
    if (!created) throw std::runtime_error(created.error().message);
    return std::move(created).value();
}
void fiscal_inputs() {
    auto d = data(); auto opts = options();
    opts.fiscal_players = std::vector<gc::FiscalPlayer>{{1, true, f(1)}, {2, true, f(1)}, {3, true, f(1)}, {4, false, f(1)}};
    expect(!cp::plan_campaign_setup(d, types(), "Ordinary", opts), "fiscal cap missing refuses opt-in setup");
    d.constants.push_back({"Credit_Cap_Per_Planet", "123", f(123)});
    expect(!cp::plan_campaign_setup(d, types(), "Ordinary", opts), "planet credit input missing is not guessed");
    d.planets[0].planet_credit_value = f(17);
    Trace trace; auto campaign = root(d, opts, trace);
    const auto& fiscal = *campaign.game()->galaxy().setup().fiscal;
    expect(fiscal.credit_cap_per_planet == f(123) && fiscal.planets.size() == 1 && fiscal.planets[0].credits == f(17),
        "fiscal cap and income come from validated data, not stock literals");
    auto enhanced = d;
    enhanced.constants.push_back({"MaxCreditIncomeAlignmentBonus", "0.25", Fixed::from_decimal("0.25").value()});
    expect(!cp::plan_campaign_setup(enhanced, types(), "Ordinary", opts), "incomplete income constant group rejected");
    enhanced.constants.push_back({"MaxCreditIncomeAlignmentPenalty", "0.75", Fixed::from_decimal("0.75").value()});
    enhanced.constants.push_back({"Political_Income_Curve", "0,0,1,1,5,1", {}});
    for (auto& faction : enhanced.factions) faction.force_alignment = "Neutral";
    enhanced.factions[0].force_alignment = "Light";
    enhanced.planets[0].force_alignment = "Dark"; enhanced.planets[0].force_strength = f(1);
    const auto bound = cp::plan_campaign_setup(enhanced, types(), "Ordinary", opts);
    expect(bound && bound.value().fiscal()->contributions
        && bound.value().fiscal()->contributions->maximum_alignment_bonus == Fixed::from_decimal("0.25").value()
        && bound.value().fiscal()->contributions->factions[0].alignment == gc::IncomeAlignment::light
        && bound.value().fiscal()->contributions->planets[0].alignment == gc::IncomeAlignment::dark,
        "alignment constants and profiles bind authored values without stock literals");
    eawr::sim::InlineExecutor serial;
    for (int i = 0; i < 1350; ++i) expect(bool(campaign.game()->step(serial)), "campaign pre-boundary service");
    expect(campaign.game()->galaxy().snapshot()->player_economy()->players[0].credits == f(12000), "startup balance unchanged until boundary");
    expect(bool(campaign.game()->step(serial)), "campaign fiscal payment");
    expect(campaign.game()->galaxy().snapshot()->player_economy()->players[0].credits == f(100), "income service applies existing provider credit cap");
}
void live_fiscal_inputs() {
    const auto directory = std::filesystem::temp_directory_path() / ("eawr-live-fiscal-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory / "XML");
    {
        std::ofstream xml(directory / "XML" / "DifficultyAdjustments.xml");
        xml << "<Difficulty_Adjustments><Difficulty_Adjustment Name='Normal_Default'>"
               "<Credit_Multiplier>1</Credit_Multiplier></Difficulty_Adjustment>"
               "<Difficulty_Adjustment Name='Hard'><Credit_Multiplier>1.2</Credit_Multiplier>"
               "</Difficulty_Adjustment></Difficulty_Adjustments>";
    }
    const std::vector<eawr::vfs::MountSpec> mounts{{"fixture", directory, "data", {}}};
    const auto filesystem = eawr::vfs::Vfs::mount(mounts).value();
    auto d = data();
    d.constants.push_back({"Credit_Cap_Per_Planet", "40000", f(40000)});
    d.planets[0].planet_credit_value = f(100);
    for (auto& faction : d.factions) { faction.is_playable = !*faction.is_neutral; faction.can_control_planets = true; }
    auto opts = options();
    const auto plan = cp::plan_live_campaign_setup(filesystem, d, types(), "Ordinary", opts, "Hard").value();
    auto setup = plan.galaxy();
    expect(setup.fiscal && setup.player_economy, "live plan installs fiscal and starting accounts");
    const auto& players = setup.fiscal->players;
    expect(players[0].earns_planet_income && players[0].positive_credit_multiplier == f(1)
        && players[1].positive_credit_multiplier == Fixed::from_decimal("1.2").value()
        && !players[3].earns_planet_income, "human factor, selected AI difficulty and neutral eligibility sourced");
    expect(setup.player_economy->players[0].credits == f(12000)
        && setup.player_economy->players[1].credits == f(9000)
        && setup.player_economy->players[0].credit_cap == f(40000), "authored balances and minimum-one-planet cap");
    expect(setup.planets[0].owner == 4, "live layout retains neutral ownership seam");
    Trace trace;
    auto root_services = services(trace);
    const auto control = root_services.political_control;
    root_services.political_control = [&](const auto& planet, const auto& allegiance, const auto& staged) {
        expect(!staged.player_economy && !staged.fiscal, "live root control precedes starting credit assignment");
        return control(planet, allegiance, staged);
    };
    const auto credit_cap = root_services.credit_cap;
    root_services.credit_cap = [&](const auto& player, const auto& staged) {
        expect(!staged.player_economy && !staged.fiscal, "live root cap refresh precedes starting credit assignment");
        return credit_cap(player, staged);
    };
    const auto assigned_root = cp::CampaignRoot::create(plan, root_services).value();
    expect(assigned_root.game()->galaxy().clock().fiscal_cycle == 1
        && assigned_root.game()->galaxy().setup().player_economy->players[0].credits == f(12000),
        "live helper also enters ordered root initialization and reinstalls authored credits/fiscal state");
    d.factions[1].can_control_planets = false;
    d.factions[2].is_playable = false;
    const auto filtered = cp::plan_live_campaign_setup(filesystem, d, types(), "Ordinary", opts, "Hard").value();
    expect(!filtered.fiscal()->players[1].earns_planet_income && !filtered.fiscal()->players[2].earns_planet_income,
        "noncontrolling and nonplayable factions earn no planet income");
    expect(filtered.fiscal()->players[1].positive_credit_multiplier == players[1].positive_credit_multiplier,
        "noncontrolling AI retains selected positive-addition multiplier for refunds");
    d.factions[0].can_control_planets.reset();
    expect(!cp::plan_live_campaign_setup(filesystem, d, types(), "Ordinary", opts, "Hard"), "missing playable control flag refuses");
    expect(!cp::plan_live_campaign_setup(filesystem, d, types(), "Ordinary", opts, "Unknown"), "missing difficulty refuses");
    std::filesystem::remove(directory / "XML" / "DifficultyAdjustments.xml");
    std::filesystem::remove(directory / "XML");
    std::filesystem::remove(directory);
    // Explicit fixture ownership stands in for the pending live control service.
    setup.planets[0].owner = 2;
    std::string reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = cp::GameSession::create(setup).value();
        expect(game.galaxy().clock().fiscal_cycle == 1, "live fiscal starts on sourced cycle one");
        const auto identities = eawr::checkpoint::game_identities(setup);
        const auto saved = eawr::checkpoint::save_game_checkpoint(game, identities).value();
        auto restored = eawr::checkpoint::load_game_checkpoint(saved, identities, {}, executor).value();
        expect(restored.session.state_sha256() == game.state_sha256(), "live tick-zero checkpoint state round trip");
        expect(eawr::checkpoint::save_game_checkpoint(restored.session, identities).value() == saved,
            "live tick-zero checkpoint bytes round trip");
        gc::GalacticReplay replay; replay.setup = setup; replay.final_tick_count = 1351;
        const auto bytes = gc::write_replay(replay).value();
        expect(gc::write_replay(gc::parse_replay(bytes).value()).value() == bytes, "live fiscal replay round trip");
        for (int tick = 0; tick < 1351; ++tick) {
            static_cast<void>(game.step(executor).value());
            static_cast<void>(restored.session.step(executor).value());
        }
        const auto expected = eawr::sim::math::add(f(9000),
            eawr::sim::math::multiply(f(100), players[1].positive_credit_multiplier).value()).value();
        expect(game.galaxy().snapshot()->player_economy()->players[1].credits == expected,
            "first live fiscal payment uses authored income and AI difficulty");
        expect(game.state_sha256() == restored.session.state_sha256(), "live fiscal checkpoint continuation parity");
        if (reference.empty()) reference = game.state_sha256();
        else expect(game.state_sha256() == reference, "live fiscal 1/2/4/8 equality");
    }
}
void authored_and_custom() {
    auto d = data(); auto opts = options();
    for (const auto& name : {"Ordinary", "Story", "Tutorial", "Multiplayer"}) for (gc::PlayerId human = 1; human <= 3; ++human) {
        Trace trace; auto session = root(d, options(human), trace, name);
        const auto& rows = session.game()->galaxy().setup().player_economy->players;
        expect(rows[0].credits == f(12000) && rows[0].current_tech == 2 && rows[0].maximum_tech == 4, "first case-insensitive records win");
        expect(rows[2].maximum_tech == 5 && rows[3].credits == f(0) && rows[3].current_tech == 0, "sourced missing-record defaults");
        expect(rows[0].credit_cap == f(100) && rows[0].credits > rows[0].credit_cap, "startup credit assignment does not clamp to cap");
        expect(std::find(trace.calls.begin(), trace.calls.end(), "hero") == trace.calls.end(), "actual single-player mode excludes MP heroes despite menu metadata");
    }
    opts.custom = cp::CampaignCustomOptions{17000, 5, 2, false};
    Trace trace; auto custom = root(d, opts, trace);
    const auto& rows = custom.game()->galaxy().setup().player_economy->players;
    expect(rows[0].current_tech == 4 && rows[0].maximum_tech == 1 && rows[1].current_tech == 4 && rows[1].maximum_tech == 1,
        "any nonzero adjustment subtracts exactly one, including negative and magnitude seven");
    expect(rows[3].current_tech == 5 && rows[3].maximum_tech == 2, "zero adjustment and current-before-new-max order");
    expect(std::all_of(rows.begin(), rows.end(), [](const auto& row) { return row.credits == f(17000); }), "same custom credits reach every faction");
    for (const auto& name : {"Story", "Tutorial"})
        expect(!cp::plan_campaign_setup(d, types(), name, opts), "unsupported definition refuses custom admission");
    d.campaigns[1].supports_custom_settings = true; Trace admitted_story;
    expect(root(d, opts, admitted_story, "Story").game()->galaxy().setup().player_economy->players[0].credits == f(17000),
        "story metadata itself does not block a definition that admits custom options");
    d.campaigns[0].supports_custom_settings.reset();
    expect(bool(cp::plan_campaign_setup(d, types(), "Ordinary", opts)), "WGP-74 omitted custom admission uses the sourced true constructor default");
    d = data(); opts.custom->credits = std::int64_t{std::numeric_limits<std::int32_t>::max()} + 1;
    expect(!cp::plan_campaign_setup(d, types(), "Ordinary", opts), "custom credits cannot exceed runtime source integer");
    opts.custom->credits = -10; Trace negative;
    expect(root(d, opts, negative).game()->galaxy().setup().player_economy->players[0].credits == f(-10),
        "direct custom assignment preserves a representable negative value");
    opts.custom->credits = 17000; opts.custom->starting_tech = std::numeric_limits<std::int32_t>::min();
    expect(!cp::plan_campaign_setup(d, types(), "Ordinary", opts), "minus-one cannot wrap source integer");
    opts.custom->starting_tech = 0; opts.custom->maximum_tech = 0;
    Trace zero; auto low = root(d, opts, zero);
    expect(low.plan()->tech()[0].requested_current == -1 && low.game()->galaxy().setup().player_economy->players[0].current_tech == 0,
        "raw minus-one request stays distinct from provider's committed current");
    opts = options(); opts.players[1].active = false; opts.seats[1].active = false; Trace inactive;
    expect(root(d, opts, inactive).game()->galaxy().setup().player_economy->players[1].credits == f(9000),
        "credit/tech assignment visits existing inactive players rather than applying force-owner admission");
}
void starting_levels() {
    auto d = data();
    d.constants.push_back({"Credit_Cap_Per_Planet", "1000", f(1000)});
    d.planets[0].planet_credit_value = f(0);
    const auto checked = []<class T>(Result<T> result) -> T {
        if (!result) throw std::runtime_error(result.error().message);
        return std::move(result).value();
    };
    const std::vector<std::string> renamed{"Wardens", "Raiders", "Syndicate", "Unaligned"};
    for (std::size_t i = 0; i < d.factions.size(); ++i) {
        const auto before = d.factions[i].name; d.factions[i].name = renamed[i];
        d.factions[i].displayed_tech_level_adjustment = i == 0 ? 1 : 0;
        for (auto& row : d.campaigns[0].home_locations) if (row.faction == before) row.faction = renamed[i];
    }
    for (const std::int64_t level : {1, 2, 3, 4, 5}) {
        std::string baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto opts = options();
            opts.fiscal_players = std::vector<gc::FiscalPlayer>{
                {1, true, f(1)}, {2, true, f(1)}, {3, true, f(1)}, {4, false, f(1)}};
            for (std::size_t i = 0; i < opts.players.size(); ++i) opts.players[i].faction = renamed[i];
            opts.custom = cp::CampaignCustomOptions{12000, level, 5, true};
            Trace trace; auto providers = services(trace);
            providers.production = [](const auto&, const auto& setup) {
                gc::ProductionSetup production; production.maximum_human_queue = 5;
                for (const auto& account : setup.player_economy->players) {
                    gc::ProductionPlayer player; player.player = account.player; player.human = account.player == 1;
                    player.uses_population_cap = player.human; player.pay_as_you_go = !player.human;
                    player.population_capacity = f(100);
                    production.players.push_back(player);
                }
                for (const auto& planet : setup.planets) {
                    gc::PlanetProduction location; location.planet = planet.id;
                    location.target_owner = planet.owner; location.ground_occupier = planet.owner;
                    production.planets.push_back(location);
                }
                // One registered type can be affiliated with several players.
                production.technology_types = {{7, 2, true, true, {1, 2, 3, 4}}};
                for (auto& player : production.players) {
                    auto account = setup.player_economy->players[player.player - 1];
                    auto assigned = gc::assign_technology(account, player, production.technology_types, account.current_tech, true);
                    if (!assigned) return Result<gc::ProductionSetup>::failure(assigned.error());
                }
                return Result<gc::ProductionSetup>::success(std::move(production));
            };
            auto planned = checked(cp::plan_campaign_setup(d, types(), "Ordinary", opts));
            auto created = checked(cp::CampaignRoot::create(planned, providers));
            const auto& initial = created.game()->galaxy().setup();
            for (std::size_t i = 0; i < initial.player_economy->players.size(); ++i) {
                const auto expected = level - (i == 0 ? 1 : 0);
                expect(initial.player_economy->players[i].current_tech == expected,
                    "WGP-190 every human/AI/neutral player gets the same visible custom level with its own adjustment");
                expect(initial.production->players[i].unlocked == (expected > 2 ? std::vector<gc::TypeId>{7} : std::vector<gc::TypeId>{}),
                    "initial unlocks use strictly lower technology, not equal technology");
            }
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            const auto identities = eawr::checkpoint::game_identities(initial);
            const auto bytes = checked(eawr::checkpoint::save_game_checkpoint(*created.game(), identities));
            auto restored = checked(eawr::checkpoint::load_game_checkpoint(bytes, identities, {}, executor));
            expect(restored.session.galaxy().setup().player_economy == initial.player_economy
                && restored.session.galaxy().setup().production == initial.production, "custom tech and unlock state survive save/load");
            for (unsigned tick = 0; tick < 3; ++tick) {
                const auto hash = checked(created.game()->step(executor)).state_hash.get();
                expect(checked(restored.session.step(executor)).state_hash.get() == hash, "custom technology save continuation");
            }
            const auto hash = created.game()->state_sha256();
            if (baseline.empty()) baseline = hash; else expect(hash == baseline, "every selected level agrees at 1/2/4/8 workers");
        }
    }
}
void heroes() {
    auto d = data(); auto opts = options(); opts.mode = cp::CampaignSetupMode::multiplayer;
    Trace trace; auto created = root(d, opts, trace, "Multiplayer");
    expect(std::count(trace.calls.begin(), trace.calls.end(), "hero") == 3 && created.game()->galaxy().setup().fleets.size() == 3,
        "one live original hero per player despite duplicate authored hero types and expanded placement");
    const auto first_hero = std::find(trace.calls.begin(), trace.calls.end(), "hero");
    const auto last_home = std::find(trace.calls.rbegin(), trace.calls.rend(), "home").base();
    expect(first_hero >= last_home && first_hero < std::find(trace.calls.begin(), trace.calls.end(), "tech"), "heroes follow all homes and precede credits/tech");
    opts.custom = cp::CampaignCustomOptions{1000, 2, 5, false}; Trace off; static_cast<void>(root(d, opts, off, "Multiplayer"));
    expect(std::find(off.calls.begin(), off.calls.end(), "production") == off.calls.end(), "custom heroes off suppresses helper entirely");
    opts.custom->allow_heroes = true; Trace on; static_cast<void>(root(d, opts, on, "Multiplayer"));
    expect(std::count(on.calls.begin(), on.calls.end(), "hero") == 3, "custom heroes on enables helper");
    opts.custom.reset(); Trace owned; owned.original_exists = true; auto existing = root(d, opts, owned);
    expect(existing.game()->galaxy().setup().fleets.empty(), "owned ORIGINAL identity guards creation independently of current member type");
    Trace no_production; no_production.production = false; auto missing = root(d, opts, no_production);
    expect(missing.game()->galaxy().setup().fleets.empty(), "home without production creates nothing");
    d.campaigns[0].home_locations.clear(); Trace no_home; auto absent = root(d, opts, no_home);
    expect(absent.plan()->multiplayer_heroes().empty(), "missing home does not invent a replacement for heroes");
    d = data(); Trace no_live; no_live.created = false; static_cast<void>(root(d, opts, no_live));
    expect(std::count(no_live.calls.begin(), no_live.calls.end(), "hero") == 6, "no live placement does not suppress later duplicate records");
    auto planned = cp::plan_campaign_setup(d, types(), "Ordinary", opts).value(); Trace refusal; auto s = services(refusal); s.owned_original_types = {};
    expect(!cp::CampaignRoot::create(std::move(planned), s) && refusal.calls.empty(), "missing hero provider refuses before private setup work");
}
void persistence(const std::filesystem::path& fixtures) {
    auto opts = options(); opts.mode = cp::CampaignSetupMode::multiplayer;
    std::string reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers); Trace trace; auto session = root(data(), opts, trace);
        auto setup = session.game()->galaxy().setup();
        setup.seat_control.emplace();
        setup.seat_control->teams.push_back({1, {1}, false});
        for (gc::PlayerId player = 1; player <= 4; ++player)
            setup.seat_control->controllers.push_back({player, player == 1 ? gc::FactionControl::human : gc::FactionControl::ai,
                gc::FactionControl::ai, {1, 2}, {3, 4}});
        auto combined = cp::GameSession::create(setup).value();
        gc::GalacticReplay replay; replay.setup = setup; replay.final_tick_count = 8;
        const auto bytes = gc::write_replay(replay).value(); const auto decoded = gc::parse_replay(bytes).value();
        expect(decoded.setup == setup && gc::write_replay(decoded).value() == bytes, "economy+graph+fleet+seat extensions round trip canonically");
        auto streamed = gc::GalacticSession::from_replay(decoded).value();
        for (unsigned tick = 0; tick < 4; ++tick) {
            static_cast<void>(combined.step(executor).value()); static_cast<void>(streamed.step(executor).value());
            expect(streamed.snapshot()->player_economy() == setup.player_economy, "snapshots retain committed economy");
            expect(streamed.snapshot()->seat_control()->control == *setup.seat_control, "snapshots retain seat policy alongside economy");
        }
        if (reference.empty()) reference = streamed.state_sha256(); else expect(streamed.state_sha256() == reference, "1/2/4/8 startup economy replay parity");
        const auto ids = eawr::checkpoint::game_identities(setup); const auto saved = eawr::checkpoint::save_game_checkpoint(combined, ids).value();
        auto loaded = eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor).value();
        expect(loaded.session.galaxy().setup().player_economy == setup.player_economy, "existing players checkpoint persists economy");
        expect(loaded.session.galaxy().seat_control()->control == *setup.seat_control, "session checkpoint persists seat policy alongside economy");
        expect(loaded.session.state_sha256() == combined.state_sha256(), "checkpoint reconstructs same root state");
        auto damaged = replay; damaged.setup.player_economy->players[0].player = 4;
        expect(!gc::write_replay(damaged), "misbound player economy rejected");
        damaged = replay; damaged.setup.player_economy->players[0].current_tech = std::numeric_limits<std::int64_t>::max();
        expect(!gc::write_replay(damaged), "unrepresentable source tech integer rejected");
        const auto u32 = [&](std::size_t at) {
            std::uint32_t value{}; for (unsigned n = 0; n < 4; ++n) value |= std::uint32_t{bytes[at + n]} << (8U * n); return value;
        };
        std::size_t extension = 100; // fixed GC v1 header (96 bytes) and extension count
        for (std::uint32_t n = 0; n < u32(96); ++n) {
            const auto length = u32(extension + 2);
            if (bytes[extension] == 3) {
                auto hostile = bytes; hostile[extension + 6] = 65;
                expect(!gc::parse_replay(hostile), "hostile economy count bounded before row allocation");
                hostile = bytes; hostile[extension] = 2;
                expect(!gc::parse_replay(hostile), "duplicate/out-of-order economy extension rejected");
                hostile = bytes; hostile.resize(extension + 6 + length - 1);
                expect(!gc::parse_replay(hostile), "truncated economy extension rejected");
            }
            extension += 6 + length;
        }
        std::size_t legacy_count{};
        // Every corpus except the opt-in production corpus is economy-absent legacy.
        for (const auto& directory : std::filesystem::directory_iterator(fixtures)) {
            if (!directory.is_directory() || directory.path().filename() == "production-corpus") continue;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory.path())) {
                if (entry.path().extension() != ".eawr-gcreplay") continue;
                std::ifstream file(entry.path(), std::ios::binary); const std::vector<std::uint8_t> old{std::istreambuf_iterator<char>(file), {}};
                const auto parsed = gc::parse_replay(old).value();
                expect(!parsed.setup.player_economy && gc::write_replay(parsed).value() == old, "legacy GC replay bytes unchanged when economy absent");
                ++legacy_count;
            }
        }
        expect(legacy_count >= 6, "legacy, graph and fleet GC fixtures all checked");
        std::size_t production_count{};
        for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures / "production-corpus")) {
            if (!entry.is_regular_file() || entry.path().extension() != ".eawr-gcreplay") continue;
            std::ifstream production_file(entry.path(), std::ios::binary);
            const std::vector<std::uint8_t> production_bytes{std::istreambuf_iterator<char>(production_file), {}};
            const auto production = gc::parse_replay(production_bytes).value();
            expect(production.setup.player_economy && production.setup.production,
                "production replay carries both economy and production extensions");
            expect(gc::write_replay(production).value() == production_bytes, "production replay bytes round trip canonically");
            ++production_count;
        }
        expect(production_count >= 3, "all three production fixtures checked");
    }
}
} // namespace
int main(int argc, const char* const argv[]) {
    try {
        authored_and_custom(); heroes(); fiscal_inputs(); live_fiscal_inputs();
        starting_levels();
        expect(argc == 2, "legacy fixture directory supplied"); if (argc == 2) persistence(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << "campaign economy contract: " << error.what() << '\n';
        return 1;
    }
    return galactic_test::failures == 0 ? 0 : 1;
}
