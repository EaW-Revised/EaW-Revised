#include <cstddef>
#include <cstdint>
#include <exception>
#include <ios>
#include <iostream>
#include <string_view>
#include <vector>
#include "../support/temp_dir.hpp"
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/campaign_end_provider.hpp"
#include "eawr/platform/campaign_scoring.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/units/galactic_types.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

// GHUD-S61 / WGP-97..103: the campaign scoring ledger (saved sim counters) and the provider that
// mounts the authored scoring module at the campaign end.
namespace {
using namespace galactic_test;
namespace platform = eawr::platform;
namespace fs = std::filesystem;
using eawr::sim::math::Fixed;

using eawr::testing::result_code_colon_value::take;

Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }

struct Tree {
    fs::path root = fs::temp_directory_path()
        / ("eawr-scoring-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Tree() { fs::create_directories(root); }
    ~Tree() { eawr::testing::remove_temp_directory(root); }
};
void write(const fs::path& path, const std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

// A test-authored module with the stock handler names and control IDs. Its equations are the
// qualified WGP-100/101 definitions in the test's own words: the point of these cases is that the
// mounted module, not engine code, produces the numbers.
constexpr std::string_view scoring_module = R"LUA(
function Base_Definitions()
    Builds = {}
    Deaths = {}
    KillPower = {}
    LostPower = {}
    Sacked = {}
    Lost = {}
    Quit = {}
    Wrapped = false
    PlanetTypeName = ""
end
function main() end
function Game_Mode_Starting_Event(mode, map) Mode = mode end
function Player_Quit_Event(player) Quit[player.Get_ID()] = true end
function Galactic_Production_End_Event(planet, object)
    local owner = planet.Get_Owner().Get_ID()
    Builds[owner] = (Builds[owner] or 0) + 1
    PlanetTypeName = planet.Get_Type().Get_Name()
    if object.Get_Type ~= nil then Wrapped = true end
end
function Galactic_Unit_Destroyed_Event(object, killer)
    local owner = object.Get_Owner().Get_ID()
    local power = object.Get_Game_Scoring_Type().Get_Combat_Rating()
    local by = killer.Get_ID()
    Deaths[owner] = (Deaths[owner] or 0) + 1
    KillPower[by] = (KillPower[by] or 0) + power
    LostPower[owner] = (LostPower[owner] or 0) + power
end
function Galactic_Planet_Faction_Change(planet, newplayer, oldplayer)
    local n, o = newplayer.Get_ID(), oldplayer.Get_ID()
    Sacked[n] = (Sacked[n] or 0) + 1
    Lost[o] = (Lost[o] or 0) + 1
end
function Get_Game_Stat_For_Control_ID(player, control, for_tactical)
    if for_tactical then return "TEXT_TITLE_TACTICAL" end
    local pid = player.Get_ID()
    if control == "IDC_MILITARY_EFFICIENCY_STATIC" then
        local b, d = Builds[pid] or 0, Deaths[pid] or 0
        if b == 0 then
            if d == 0 then return 0 end
            return -1
        end
        return (b - d) / b
    elseif control == "IDC_KILL_EFFICIENCY_STATIC" then
        local k, l = KillPower[pid] or 0, LostPower[pid] or 0
        if k == 0 then return 0 end
        if l == 0 then return k end
        return k / l
    elseif control == "IDC_CONQUEST_EFFICIENCY_STATIC" then
        local s, l = Sacked[pid] or 0, Lost[pid] or 0
        if s == 0 then return 0 end
        if l == 0 then return s end
        return s / l
    elseif control == "IDC_TITLE_STATIC" then
        if Quit[pid] then return "TEXT_TITLE_QUIT" end
        if Wrapped then return "TEXT_TITLE_WRAPPED" end
        return "TEXT_TITLE_" .. player.Get_Faction_Name() .. "_" .. PlanetTypeName
    end
    return nil
end
)LUA";

constexpr std::string_view objects_xml = R"xml(<Objects>
<SpaceUnit Name="Test_Frigate"><AI_Combat_Power>10</AI_Combat_Power><Score_Cost_Credits>100</Score_Cost_Credits>
<Build_Cost_Credits>200</Build_Cost_Credits></SpaceUnit>
<SpaceUnit Name="Test_Cruiser"><AI_Combat_Power>30</AI_Combat_Power><Score_Cost_Credits>300</Score_Cost_Credits>
<Build_Cost_Credits>900</Build_Cost_Credits></SpaceUnit>
</Objects>)xml";

struct Mounted {
    Tree tree;
    std::shared_ptr<const eawr::vfs::Vfs> files;
    std::shared_ptr<const eawr::data::Catalog> catalog;
    platform::CampaignScoringSource source;
};
std::unique_ptr<Mounted> mount(std::string_view module_text, std::string_view constants) {
    auto out = std::make_unique<Mounted>();
    const auto& root = out->tree.root;
    for (const auto& [name, body] : std::map<std::string, std::string>{
             {"GameObjectFiles", "<Game_Object_Files><File>objects.xml</File></Game_Object_Files>"},
             {"HardpointDataFiles", "<Hard_Point_Files><File>hardpoints.xml</File></Hard_Point_Files>"},
             {"FactionFiles", "<Faction_Files><File>factions.xml</File></Faction_Files>"},
             {"CampaignFiles", "<Campaign_Files><File>campaigns.xml</File></Campaign_Files>"},
             {"SFXEventFiles", "<SFXEvent_Files><File>sfx.xml</File></SFXEvent_Files>"}})
        write(root / "XML" / (name + ".xml"), body);
    write(root / "XML" / "objects.xml", objects_xml);
    write(root / "XML" / "hardpoints.xml", "<HardPoints/>");
    write(root / "XML" / "factions.xml", "<Factions><Faction Name=\"EMPIRE\"/><Faction Name=\"REBEL\"/></Factions>");
    write(root / "XML" / "campaigns.xml", "<Campaigns/>");
    write(root / "XML" / "sfx.xml", "<SFXEvents/>");
    if (!constants.empty()) write(root / "XML" / "GameConstants.xml", constants);
    if (!module_text.empty()) write(root / "Scripts" / "Miscellaneous" / "TestScoring.lua", module_text);
    write(root / "Scripts" / "Library" / "placeholder.lua", "-- the library directory exists in every installation\n");
    const std::array mounts{eawr::vfs::MountSpec{"base", root, "data", {}}};
    auto files = take(eawr::vfs::Vfs::mount(mounts));
    out->files = std::make_shared<const eawr::vfs::Vfs>(std::move(files));
    auto loaded = take(eawr::data::load_catalog(*out->files, eawr::data::Profile::eaw));
    out->catalog = std::make_shared<const eawr::data::Catalog>(std::move(loaded).catalog);
    auto types = std::make_shared<eawr::units::GalacticTypeTable>();
    for (const auto* name : {"Test_Frigate", "Test_Cruiser"}) {
        eawr::units::GalacticType type; type.name = name; types->types.push_back(type);
    }
    out->source.files = out->files; out->source.catalog = out->catalog; out->source.types = types;
    out->source.type_names = {{1000, "Test_Frigate"}, {1001, "Test_Cruiser"}};
    out->source.planet_types = {{5, "Test_World"}, {6, "Test_World"}, {7, "Test_Moon"}};
    out->source.campaign_name = "Test_Campaign";
    return out;
}
constexpr std::string_view constants_xml =
    "<Game_Constants><Game_Scoring_Script_Name>TestScoring</Game_Scoring_Script_Name></Game_Constants>";

// ---------------------------------------------------------------------------------------------
void ledger_encoding() {
    galactic::CampaignScoreLedger ledger;
    expect(ledger.empty(), "a fresh ledger is empty, so its state section is absent");
    ledger.builds[{1, 5, 1000}] = 3;
    ledger.deaths[{1, 2, 1000}] = 2;
    ledger.deaths[{2, 0, 1001}] = 1;
    ledger.changes[{6, 2, 1}] = 4;
    ledger.quits.insert(2);
    std::vector<std::uint8_t> bytes;
    galactic::detail::encode_score_ledger(bytes, ledger);
    const auto decoded = galactic::detail::decode_score_ledger(bytes);
    expect(decoded && *decoded == ledger, "the ledger decodes to the value it encoded");
    std::vector<std::uint8_t> again;
    if (decoded) galactic::detail::encode_score_ledger(again, *decoded);
    expect(again == bytes, "the ledger encoding is canonical");
    auto trailing = bytes; trailing.push_back(0);
    expect(!galactic::detail::decode_score_ledger(trailing), "trailing bytes refuse");
    auto truncated = bytes; truncated.pop_back();
    expect(!galactic::detail::decode_score_ledger(truncated), "a truncated ledger refuses");
    galactic::CampaignScoreLedger zero;
    zero.builds[{1, 5, 1000}] = 0;
    std::vector<std::uint8_t> zero_bytes;
    galactic::detail::encode_score_ledger(zero_bytes, zero);
    expect(!galactic::detail::decode_score_ledger(zero_bytes), "a zero count row is not canonical");
}

// ---------------------------------------------------------------------------------------------
galactic::GalacticSetup production_setup() {
    galactic::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4;
    options.fleets_per_player = 1; options.members_per_fleet = 2; options.debug_commands = true;
    auto config = galactic::synthetic_galaxy(options);
    config.rules.maximum_fleet_movement_distance = f(10000); config.rules.fiscal_cycle_frames = 3;
    config.fleet_control = galactic::FleetControlSetup{{}, {{1000, false, true, false}, {1001, false, true, false}}, {}};
    config.player_economy = galactic::PlayerEconomySetup{{{1, f(1000), f(10000), 2, 5}, {2, f(1000), f(10000), 2, 5}}};
    config.fiscal = galactic::FiscalSetup{f(2500), {{1, true, f(1)}, {2, true, f(1)}}, {}};
    config.production = galactic::ProductionSetup{5, {{1, true, false, true, f(50), f(0), {}, {}, {}},
        {2, true, false, true, f(50), f(0), {}, {}, {}}}, {}, {}, {}};
    for (const auto& planet : config.planets) {
        config.fiscal->planets.push_back({planet.id, f(10)});
        galactic::PlanetProduction production;
        production.planet = planet.id; production.target_owner = planet.owner; production.starbase_owner = planet.owner;
        production.ground_occupier = planet.owner; production.space_level = 1; production.maximum_space_level = 5;
        production.ground_slots = 3; production.space_slots = 3; production.maximum_ground_forces = 10;
        production.enabled = true; production.surface_accessible = true;
        galactic::ProductionOffer type; type.type = 1000; type.price = f(100);
        type.seconds = take(Fixed::from_decimal("0.1")); type.tech = 2; type.population = f(1);
        type.affiliated = true; type.direct_build_allowed = true;
        production.offers.push_back(type);
        config.production->planets.push_back(production);
    }
    return config;
}

galactic::PlanetId first_planet_of(const galactic::GalacticSetup& config, const galactic::PlayerId player) {
    for (const auto& planet : config.planets) if (planet.owner == player) return planet.id;
    throw std::runtime_error("the fixture has no planet of that player");
}

struct Run {
    galactic::CampaignScoreLedger ledger;
    std::string hash;
    std::string section_bytes_hash;
};
// One scripted campaign segment: a build completes, a planet changes hands twice, two units die
// with a recorded killer and one without, a player quits.
Run drive(const std::size_t workers) {
    const eawr::platform::ThreadWorkerAdapter pool(workers);
    const auto config = production_setup();
    auto game = take(eawr::campaign::GameSession::create(config));
    const auto planet = first_planet_of(config, 1), foreign = first_planet_of(config, 2);
    take(game.submit({{0, {1}, 1}, galactic::ProductionCommand{planet, 1000, galactic::ProductionAction::buy,
        galactic::ProductionDomain::ground, 0, {}}}));
    take(game.submit({{1, {1}, 2}, galactic::DebugSetPlanetOwner{foreign, 1}}));
    take(game.submit({{3, {2}, 1}, galactic::DebugSetPlanetOwner{foreign, 2}}));
    take(game.submit({{4, {2}, 2}, galactic::DebugSetPlanetOwner{foreign, 2}})); // no change, no record
    std::string last;
    for (int tick = 0; tick < 12; ++tick) last = take(game.step(pool)).state_hash.get();
    Run run; run.hash = last; run.ledger = game.galaxy().score_ledger();
    return run;
}

void session_producers() {
    const auto config = production_setup();
    const auto planet = first_planet_of(config, 1), foreign = first_planet_of(config, 2);
    std::optional<Run> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        auto run = drive(workers);
        if (!reference) reference = run;
        expect(run.ledger == reference->ledger && run.hash == reference->hash,
            "ledger and state hash equal on 1/2/4/8 workers");
    }
    const auto& ledger = reference->ledger;
    expect(ledger.builds.size() == 1 && ledger.builds.begin()->first == galactic::CampaignScoreLedger::BuildKey{1, planet, 1000}
        && ledger.builds.begin()->second == 1, "WGP-99: one completed build for the planet's owner");
    const galactic::CampaignScoreLedger::ChangeKey taken{foreign, 2, 1}, lost{foreign, 1, 2};
    expect(ledger.changes.size() == 2 && ledger.changes.at(taken) == 1 && ledger.changes.at(lost) == 1,
        "WGP-101: each ownership change records the new owner's acquisition and the old owner's loss, once");
    expect(ledger.deaths.empty(), "no death without a battle return");

    // Deaths: recorded final blow, missing killer, once only.
    auto world = take(galactic::GalacticSession::create(config));
    const auto& first = config.fleets[0].members; // player 1's fleet
    const auto& second = config.fleets[1].members; // player 2's fleet
    std::vector<eawr::sim::EntityId> destroyed{first[0].id, first[1].id, second[0].id};
    std::sort(destroyed.begin(), destroyed.end());
    std::vector<galactic::BattleKiller> killers{{first[0].id, 2}, {second[0].id, 1}};
    std::sort(killers.begin(), killers.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    take(world.apply_battle_losses(destroyed, killers));
    const auto& deaths = world.score_ledger().deaths;
    std::uint64_t total = 0;
    for (const auto& [key, count] : deaths) total += count;
    const auto type_first = first[0].type, type_second = second[0].type;
    expect(total == 3 && deaths.at({1, 2, type_first}) >= 1 && deaths.at({2, 1, type_second}) >= 1
        && deaths.contains({1, 0, first[1].type}), "WGP-98: owner, killer and type per death; a missing killer is the sentinel");
    const auto before = world.score_ledger();
    take(world.apply_battle_losses(destroyed, killers));
    expect(world.score_ledger() == before, "a loss is recorded once: removed identities are not victims again");

    take(world.record_player_quit(2));
    expect(world.score_ledger().quits.contains(2), "WGP-102: the quit table records the player");
    expect(!world.record_player_quit(99), "an unknown player cannot quit");
    expect(!world.score_ledger().empty() && world.state_sha256() != take(galactic::GalacticSession::create(config)).state_sha256(),
        "a nonempty ledger is part of the state hash");
    auto bare = take(galactic::GalacticSession::create(config));
    expect(bare.score_ledger().empty(), "a campaign without scoring activity has no section");
}

// ---------------------------------------------------------------------------------------------
void mounted_module() {
    const auto m = mount(scoring_module, constants_xml);
    galactic::CampaignScoreLedger ledger;
    const galactic::TypeId frigate = 1000, cruiser = 1001;
    ledger.builds[{1, 5, frigate}] = 200; // many events: bounded service slices
    ledger.deaths[{1, 2, frigate}] = 50;
    ledger.deaths[{1, 2, cruiser}] = 50;
    ledger.deaths[{2, 1, frigate}] = 150;
    ledger.changes[{5, 2, 1}] = 2;
    ledger.changes[{6, 1, 2}] = 1;
    const std::vector<platform::CampaignScoringPlayer> players{{1, "Empire"}, {2, "Rebel"}};
    const auto outcome = take(platform::evaluate_campaign_scoring(m->source, ledger, players, 1));
    expect(outcome.has_value(), "the named module mounts and answers");
    if (!outcome) return;
    // B=200, D=100: military 0.5; K=150*10=1500, L=50*10+50*30=2000: kill 0.75; sacked 2, lost 1: conquest 2.
    expect(outcome->efficiencies[0] && *outcome->efficiencies[0] == 0.5, "WGP-100: military efficiency from the mounted module");
    expect(outcome->efficiencies[1] && *outcome->efficiencies[1] == 2.0, "WGP-101: conquest efficiency from the mounted module");
    expect(outcome->efficiencies[2] && *outcome->efficiencies[2] == 0.75, "WGP-100: kill efficiency from authored combat power");
    expect(outcome->title_key == "TEXT_TITLE_EMPIRE_Test_World",
        "the title key is the module's string; the campaign (non-tactical) query is used; planet type is authored");

    // The rival's stats come from the same ledger (killer credit, owner debit): no builds but
    // 150 deaths is -1; sacked 1 and lost 2 is one half; K=2000 over L=1500.
    const auto other = take(platform::evaluate_campaign_scoring(m->source, ledger, players, 2));
    expect(other && other->efficiencies[0] && *other->efficiencies[0] == -1.0 && other->efficiencies[1]
        && *other->efficiencies[1] == 0.5 && other->efficiencies[2]
        && *other->efficiencies[2] == 2000.0 / 1500.0, "the same ledger answers for the other player");

    // WGP-102: a quitter's title is the module's quit key; efficiencies stay.
    auto quit = ledger; quit.quits.insert(1);
    const auto quitter = take(platform::evaluate_campaign_scoring(m->source, quit, players, 1));
    expect(quitter && quitter->title_key == "TEXT_TITLE_QUIT" && quitter->efficiencies == outcome->efficiencies,
        "a recorded quit changes the title only");

    // An empty ledger gives present zeros, not absent controls (WGP-97).
    const auto empty = take(platform::evaluate_campaign_scoring(m->source, {}, players, 1));
    expect(empty && empty->efficiencies[0] == 0.0 && empty->efficiencies[1] == 0.0 && empty->efficiencies[2] == 0.0,
        "an empty ledger yields present zero efficiencies");

    // Mod-faithful: no module name, no module file, no constants -> absent, never an error.
    const auto no_name = mount(scoring_module, "<Game_Constants/>");
    expect(take(platform::evaluate_campaign_scoring(no_name->source, ledger, players, 1)) == std::nullopt,
        "constants without a scoring module name provide no outcome");
    const auto no_module = mount("", constants_xml);
    expect(take(platform::evaluate_campaign_scoring(no_module->source, ledger, players, 1)) == std::nullopt,
        "a named module that is not authored provides no outcome");
    const auto no_constants = mount(scoring_module, "");
    expect(take(platform::evaluate_campaign_scoring(no_constants->source, ledger, players, 1)) == std::nullopt,
        "no constants file provides no outcome");
    const auto broken = mount("function Base_Definitions() error('boom') end", constants_xml);
    expect(!platform::evaluate_campaign_scoring(broken->source, ledger, players, 1),
        "a failing module is a failure the provider turns into an absent half");
}

// ---------------------------------------------------------------------------------------------
galactic::GalacticSetup ending(std::string_view local_faction, std::string_view other_faction) {
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
    return initial;
}

void production_provider() {
    const auto m = mount(scoring_module, constants_xml);
    auto source = std::make_shared<platform::CampaignScoringSource>(m->source);
    const auto provider = platform::campaign_end_inputs_provider(m->catalog, source);
    std::optional<platform::CampaignEndInputs> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        const auto config = ending("REBEL", "EMPIRE");
        auto game = take(eawr::campaign::GameSession::create(config));
        galactic::PlanetId taken{};
        for (const auto& planet : config.planets) if (planet.owner == 2) { taken = planet.id; break; }
        expect(taken != 0, "the fixture has a planet of the other player");
        take(game.submit({{1, {1}, 1}, galactic::DebugSetPlanetOwner{taken, 1}}));
        for (int tick = 0; tick < 4; ++tick) (void)take(game.step(pool));
        galactic::CampaignResult result;
        result.condition.player = 1; result.condition.kind = galactic::VictoryKind::credits; result.local_won = true;
        const auto inputs = take(provider(game, result, 1));
        expect(inputs.scoring_seam.empty(), "a mounted module leaves no scoring seam");
        expect(inputs.title_key == "TEXT_TITLE_REBEL_Test_World" || inputs.title_key.starts_with("TEXT_TITLE_REBEL_"),
            "the local faction comes from the victory monitor's player rows");
        expect(inputs.efficiencies[1] && *inputs.efficiencies[1] == 1.0
            && inputs.efficiencies[0] == 0.0 && inputs.efficiencies[2] == 0.0,
            "one acquisition without loss is the conquest efficiency; no builds or deaths are present zeros");
        if (!reference) reference = inputs;
        expect(inputs.title_key == reference->title_key && inputs.efficiencies == reference->efficiencies,
            "provider output equal on 1/2/4/8 workers");
    }
    // An absent module leaves the scoring half absent and names why; the authored half stays.
    const auto bare = mount("", "<Game_Constants/>");
    const auto bare_provider = platform::campaign_end_inputs_provider(bare->catalog,
        std::make_shared<platform::CampaignScoringSource>(bare->source));
    const eawr::platform::ThreadWorkerAdapter pool(1);
    auto game = take(eawr::campaign::GameSession::create(ending("REBEL", "EMPIRE")));
    galactic::CampaignResult result; result.condition.player = 1;
    const auto absent = take(bare_provider(game, result, 1));
    expect(absent.title_key.empty() && !absent.efficiencies[0] && !absent.efficiencies[1] && !absent.efficiencies[2]
        && !absent.scoring_seam.empty(), "an unavailable module leaves the half absent with its reason");
}

// ---------------------------------------------------------------------------------------------
// The installed module (--installed, game-data label): the authored equations (WGP-100..103) are
// computed by the real GameScoring module and cross-checked against the documented formula.
std::optional<fs::path> game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    return root ? std::optional<fs::path>{std::move(*root)} : std::nullopt;
}
void installed_module() {
    const auto root = game_root();
    if (!root) throw std::runtime_error("installed scoring module requires EAWR_EAW_GAME_ROOT");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, *root / folder / "Data")).mount);
    auto files = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(mounts)));
    auto loaded = take(eawr::data::load_catalog(*files, eawr::data::Profile::foc));
    auto catalog = std::make_shared<const eawr::data::Catalog>(std::move(loaded).catalog);
    platform::CampaignScoringSource source;
    source.files = files; source.catalog = catalog;
    source.types = std::make_shared<const eawr::units::GalacticTypeTable>(eawr::units::galactic_type_table(*catalog));
    source.campaign_name = "Installed_Scoring";
    // The data picks its own strongest and weakest scored types: nothing here names a retail unit.
    struct Scored { std::string name; Fixed power; };
    std::vector<Scored> scored;
    for (const auto& type : source.types->types) {
        const auto object = catalog->resolve(type.name, eawr::data::Category::game_object);
        if (!object) continue;
        const auto* tag = object.value().value("AI_Combat_Power");
        if (!tag) continue;
        const auto parsed = Fixed::from_decimal(tag->value.raw_text);
        if (parsed && parsed.value().raw() > 0) scored.push_back({type.name, parsed.value()});
    }
    expect(scored.size() >= 2, "the installed data authors combat power on several galactic types");
    if (scored.size() < 2) return;
    const auto weak = std::min_element(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.power.raw() < b.power.raw(); });
    const auto strong = std::max_element(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.power.raw() < b.power.raw(); });
    expect(strong->power.raw() > weak->power.raw(), "the strong and weak scored types differ");
    const galactic::TypeId strong_id = 7001, weak_id = 7002;
    source.type_names = {{strong_id, strong->name}, {weak_id, weak->name}};
    galactic::CampaignScoreLedger ledger;
    ledger.builds[{1, 11, strong_id}] = 4;
    ledger.deaths[{1, 2, strong_id}] = 1;
    ledger.deaths[{2, 1, weak_id}] = 3;
    ledger.changes[{11, 2, 1}] = 1;
    const std::vector<platform::CampaignScoringPlayer> players{{1, "Rebel"}, {2, "Empire"}};
    const auto outcome = take(platform::evaluate_campaign_scoring(source, ledger, players, 1));
    expect(outcome.has_value(), "the installed scoring module mounts for a campaign");
    if (!outcome) return;
    // WGP-100: military (B-D)/B; kill credited victim power over lost power. WGP-101: no loss -> acquisitions.
    expect(outcome->efficiencies[0] && *outcome->efficiencies[0] == 0.75, "installed military efficiency is (B-D)/B");
    expect(outcome->efficiencies[1] && *outcome->efficiencies[1] == 1.0,
        "installed conquest efficiency without a loss is the acquisition count");
    if (outcome->efficiencies[2]) {
        const double expected = 3.0 * static_cast<double>(weak->power.raw()) / static_cast<double>(strong->power.raw());
        expect(std::abs(*outcome->efficiencies[2] - expected) < 1e-9,
            "installed kill efficiency is credited victim power over lost power");
    } else expect(false, "installed kill efficiency is present");
    // WGP-103: the Rebel faction alone selects the Rebel key prefix.
    expect(outcome->title_key.starts_with("TEXT_REBEL_TITLE"), "the Rebel faction uses the Rebel title keys");
    const std::vector<platform::CampaignScoringPlayer> as_empire{{1, "Empire"}, {2, "Rebel"}};
    const auto empire = take(platform::evaluate_campaign_scoring(source, ledger, as_empire, 1));
    expect(empire && empire->title_key.starts_with("TEXT_EMPIRE_TITLE"), "every other faction uses the Empire title keys");
    const std::vector<platform::CampaignScoringPlayer> as_underworld{{1, "Underworld"}, {2, "Rebel"}};
    const auto underworld = take(platform::evaluate_campaign_scoring(source, ledger, as_underworld, 1));
    expect(underworld && underworld->title_key.starts_with("TEXT_EMPIRE_TITLE"), "Underworld uses the Empire title keys");
    // WGP-102/103: a recorded quit forces the rank score to zero while the efficiencies stay.
    auto quit = ledger; quit.quits.insert(1);
    const auto quitter = take(platform::evaluate_campaign_scoring(source, quit, players, 1));
    expect(quitter && quitter->title_key == "TEXT_REBEL_TITLE0" && quitter->efficiencies == outcome->efficiencies,
        "a quitter's rank score is zero and the efficiencies stay");
    // WGP-102/103: an empty ledger scores nothing: exactly the lowest rank.
    const auto empty = take(platform::evaluate_campaign_scoring(source, {}, players, 1));
    expect(empty && empty->title_key == "TEXT_REBEL_TITLE0" && empty->efficiencies[0] == 0.0,
        "an empty ledger gives the lowest rank and present zero efficiencies");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--installed") {
            installed_module();
        } else {
            ledger_encoding();
            session_producers();
            mounted_module();
            production_provider();
        }
    } catch (const std::exception& error) {
        std::cerr << "campaign scoring checks threw: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) { std::cerr << failures << " campaign scoring checks failed\n"; return 1; }
    std::cout << "campaign scoring checks passed\n";
    return 0;
}
