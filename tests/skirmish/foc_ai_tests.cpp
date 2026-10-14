// The FoC tactical AI in the M2 battle (#79, docs/behaviour/foc-tactical-ai.md "#79 host"):
// the Empire AI's retail freestore and goal engine against an idle Rebel player on the pinned start. The
// fixture must give the same script commands, world events and state hashes on 1, 2, 4 and
// 8 workers (ADR-009), a headless replay of its record must reproduce the world without
// running a script, and the per-tick Lua load stays below the service budget. A real-time
// run paused mid-attack and resumed on fast forward must replay like an unpaused one (#459).
//
//   foc_ai_tests [ticks [hits.csv]]   (needs EAWR_EAW_GAME_ROOT; skipped otherwise)
//   foc_ai_tests --combat-engagement-trace [ticks [trace.txt [seed]]]
//                                     the economy-disabled diagnostic, including later admitted AI units
//   foc_ai_tests --stock-engagement-trace [ticks [trace.txt [seed]]]
//                                     the full stock battle, including actual human-player contact
//   foc_ai_tests --passive-contact-control [ticks]
//                                     disables AI planning/scripts; the contact guard must fail
//   foc_ai_tests --underworld [ticks [trace.csv]] the Underworld AI's StarVipers against an idle Rebel fleet:
//                                     a stand-in freestore fires BUZZ_DROIDS through the Lua calls (WAB-70..76)
//   foc_ai_tests --vengeance [ticks]            the Underworld AI's Vengeance frigates against the same fleet:
//                                     a stand-in freestore switches SELF_DESTRUCT on through the Lua calls (WSD-01..09)
//   foc_ai_tests --underworld-stock [ticks]       the same fleets under the stock Underworld plans alone: the
//                                     plans' own Target_In_Range handler activates BUZZ_DROIDS (EX-46)
//   foc_ai_tests --interceptor [ticks]          the Underworld AI's Interceptor IV frigates against the same fleet:
//                                     a stand-in freestore switches FULL_SALVO on through the Lua calls (WAB-80..83)
//   foc_ai_tests --cloaked [ticks]              the Underworld AI against a Rebel fleet whose Vengeance cloaks:
//                                     the AI's queries stay stealth-blind and it moves on from refused attacks (WST-16)
//
// hits.csv lists every projectile hit of the one-worker run (tick, shooter, target, x, y, z):
// where and when the fleets meet, for placing an eye-check camera.

#include "eawr/data/xml.hpp"
#include "eawr/platform/live_scripts.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/space.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/setup.hpp"
#include "../../apps/sim_headless/squadron_trace.hpp"
#include "../../src/script/foc/ai_engine.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

struct Run {
    std::uint64_t special_use_purchases{}, special_projectiles{}; // observation only in the ordinary battle
    std::set<std::uint64_t> special_projectile_ids;
    std::vector<std::string> hashes;       // combined world and script state per tick
    std::vector<std::string> world_hashes; // world state per tick
    std::vector<std::string> commands;     // routed script commands, in order
    std::vector<std::string> events;       // world events, in order
    std::vector<tactical::Event> ai_rejections; // optional all-admitted observation
    std::set<std::string> diagnostics;     // distinct script diagnostics
    std::int64_t max_instructions{};
    std::int64_t total_instructions{};
    std::uint64_t point_defence_zaps{};    // shots a Crusader's laser defence shot down
    std::uint64_t crusader_hits{};         // projectile hits on the AI player's Crusaders
    std::map<std::string, std::uint64_t> crusader_hit_by; // ... by the shooter's type
    std::uint64_t ai_hits{};               // projectile hits by the AI player's units
    std::uint64_t first_ai_hit{};          // the tick of the AI's first hit, 0 if none
    // The stock economy witness also observes units admitted after the starting roster.
    std::uint64_t stock_hits{}, first_stock_hit{};
    std::uint64_t player_hits{}, first_player_hit{}; // all-admitted AI hits on this fixture's human player
    std::uint64_t pad_constructions_started{}, pad_constructions_completed{}, pending_pad_constructions{};
    std::string hits_csv;                  // every projectile hit
    std::map<std::string, std::uint64_t> orders_by_type; // the AI player's accepted orders per unit type
    std::set<eawr::sim::EntityId> artillery_seen; // observation only: stock proximity consumers
    std::set<eawr::sim::EntityId> droids;  // WAB-71: every Buzz_Droids object seen
    std::int64_t blast_hull_drop{};        // WSD-06: the player's hull lost in the tick after an ability blast object existed
    std::uint64_t blast_objects{};         // WSD-06: ticks that showed an ability blast object
    std::uint64_t sd_alive_first{}, sd_alive_last{}, sd_ready_ticks{}; // WSD-09 diagnostics: a SELF_DESTRUCT holder's life
    std::int64_t sd_min_gap = -1;          // its closest approach to a player unit (world units)
    std::uint64_t plan_buzz{};             // EX-46: accepted BUZZ_DROIDS script requests
    std::uint64_t shield_leech_frames{}; // WLS-04: successful object-target locks in the AI battle
    std::optional<std::uint64_t> first_leech_lock, first_tractor_lock;
    std::optional<std::uint64_t> first_leech_ready_target, first_tractor_ready_target;
    std::uint64_t tractor_frames{};      // WAB-36: frames with a tractor beam locked onto a target
    std::uint64_t concentrate_frames{};  // WAB-36: frames with CONCENTRATE_FIRE on a target
    std::uint64_t recruit_hits{};        // WHE-24: hits on a concentrated target by other shooters
    std::set<eawr::sim::EntityId> corrupted; // WHE-33: recipients reached by the AI's cast
    std::uint64_t plan_lure{};             // WLU-10: accepted LURE script requests
    std::uint64_t lure_frames{};           // WLU-02: craft frames with LURE switched on
    std::vector<std::uint64_t> hidden_refusals; // WST-11/16: ticks of the AI's attack orders refused on a hidden enemy
    std::vector<std::pair<std::uint64_t, eawr::sim::EntityId>> ai_hit_targets; // WST-16: (tick, target) of the AI's hits
    std::vector<std::uint64_t> ai_accepted;     // WST-16: ticks of the AI player's accepted orders
    std::vector<std::string> cloak_contacts; // selected target and visibility at launch/contact
    std::uint64_t salvo_ready_ticks{}, salvo_active_ticks{};
    std::int64_t salvo_min_gap = -1;
    double seconds{};
    tactical::TacticalReplay record;
    foc::AiJournal journal;
};

std::string argument_text(const auth::Value& value) {
    if (const auto* object = std::get_if<auth::Handle>(&value.data))
        return "handle(" + std::to_string(object->kind) + ":" + std::to_string(object->id) + ")";
    if (const auto* number = std::get_if<eawr::script::numeric::LuaNumber>(&value.data)) {
        const auto integer = eawr::script::numeric::to_exact_integer(*number);
        return integer ? std::to_string(integer.value()) : "number-bits(" + std::to_string(number->repr) + ")";
    }
    if (const auto* text = std::get_if<std::string>(&value.data)) return *text;
    if (const auto* flag = std::get_if<bool>(&value.data)) return *flag ? "true" : "false";
    if (const auto* list = std::get_if<auth::ValueList>(&value.data)) {
        std::string out = "[";
        for (const auto& item : *list) out += argument_text(item) + " ";
        return out + "]";
    }
    return "nil";
}

std::string command_text(const auth::RoutedCommand& routed) {
    std::string out = routed.command.verb + " t" + std::to_string(routed.command.tick) + " i" +
        std::to_string(routed.command.issuer) + " s" + std::to_string(routed.command.sequence);
    out += routed.submitted ? " key " + std::to_string(routed.key.tick) + "/" + std::to_string(routed.key.player_id) + "/" +
            std::to_string(routed.key.sequence)
                            : " dropped " + routed.dropped.message;
    for (const auto& argument : routed.command.arguments) out += " " + argument_text(argument);
    return out;
}

struct Content {
    skirmish::SkirmishStart start;
    skirmish::SessionContent content;
    tactical::VictoryRules victory;
    foc::AiSetup ai;
    std::map<std::string, std::string> modules;
    std::map<std::string, std::string> plan_modules; // modules and the selected plans
    std::map<tactical::TypeId, std::string> type_names;
    tactical::EconomyRules economy; // SAE-01/02/03: the shared skirmish-start economy.
};

// L-54: run the installed evaluator with a range outside [0, 1]. Its own
// state and result expose a host that ignores the supplied float bounds.
void progressive_random_probe(const std::filesystem::path& root, const Content& content) {
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto mount = eawr::vfs::resolve_manifest_mount(id, root / folder / "Data");
        if (!mount) { expect(false, "L-54: evaluator layer mounts"); return; }
        specs.push_back(std::move(mount).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) { expect(false, "L-54: evaluator VFS mounts"); return; }
    const std::string path = "Data/Scripts/Evaluators/progressiverandom.lua";
    const auto bytes = filesystem.value().open(path);
    if (!bytes) { expect(false, "L-54: installed progressive evaluator loads"); return; }
    std::string source(bytes.value().size(), '\0');
    std::transform(bytes.value().begin(), bytes.value().end(), source.begin(),
        [](std::byte value) { return static_cast<char>(value); });
    auto modules = content.modules;
    modules[path] = std::move(source);
    auto setup = content.ai;
    setup.xml.clear();
    setup.freestore_module = "Data/Scripts/Test/RandomProbe.lua";
    modules[setup.freestore_module] =
        "require(\"Data/Scripts/Evaluators/progressiverandom\")\n"
        "function Base_Definitions() ServiceRate = 10000; UnitServiceRate = 10000 end\n"
        "function main()\n"
        "  threshold = 1.5\n"
        "  local result = Evaluate(2, 3)\n"
        "  assert(result == 0 and roll >= 2 and roll <= 3 and threshold > 1.5)\n"
        "end\n";
    std::string baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors, content.content.durability,
            content.content.motion, content.content.fog, content.content.combat, content.victory, content.content.abilities,
            content.economy, content.content.map_box);
        if (!world) { expect(false, "L-54: evaluator world starts"); return; }
        auto session = foc::create_session(std::move(world).value(), setup, modules);
        if (!session) { expect(false, "L-54: evaluator session starts: " + session.error().message); return; }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto tick = session.value().step(executor);
        expect(tick && tick.value().scripts.diagnostics.empty(), "L-54: installed evaluator respects its float range");
        if (!tick) return;
        if (workers == 1) baseline = tick.value().state_sha256;
        else expect(tick.value().state_sha256 == baseline, "L-54: installed evaluator agrees on 1/2/4/8 workers");
        std::cout << "ProgressiveRandom range probe: " << workers << " workers, "
                  << (tick.value().scripts.diagnostics.empty() ? "PASS" : "FAIL") << std::endl;
    }
}

std::optional<Content> load(const std::filesystem::path& root, bool rebel_ai = false, bool underworld = false,
                            std::vector<std::string> underworld_fleet = {}, bool crusader = false,
                            std::optional<std::uint64_t> battle_seed = std::nullopt,
                            std::vector<std::string> player_fleet = {},
                            std::vector<std::string> ai_fleet = {}, bool three_vs_three = false,
                            bool serial_profile = false) {
    // The Crusader battle's Rebel player fields only torpedo bombers, so its deadly enemy fires missiles.
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, root / folder / "Data");
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return std::nullopt;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return std::nullopt;
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "FoC catalog loads");
    if (!catalog) return std::nullopt;
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    if (crusader) {
        // The pinned M2 list and obstacles, plus the Underworld gunship.
        for (const auto id : eawr::units::pinned_m2_types()) input.types.emplace_back(id);
        input.types.emplace_back("Crusader_Gunship");
        for (const auto id : eawr::units::pinned_m2_obstacles()) input.obstacles.emplace_back(id);
    }
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "FoC unit tables load");
    if (!tables) return std::nullopt;
    auto fixture = skirmish::m2_fixture();
    if (serial_profile) {
        skirmish::FixtureOptions options;
        options.map = "data/art/maps/_mp_space_coruscant.ted";
        options.seed = 67;
        options.slots = std::vector<skirmish::LobbySlot>{};
        for (std::uint32_t index = 0; index < 6; ++index) {
            const bool rebel = index < 3;
            options.slots->push_back({index + 1, rebel ? "Rebel" : "Empire", rebel ? 0U : 1U,
                index == 0, rebel
                    ? std::vector<std::string>{"Y-Wing_Squadron", "Corellian_Corvette", "Nebulon_B_Frigate", "Calamari_Cruiser"}
                    : std::vector<std::string>{"Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Victory_Destroyer", "Imperial_Star_Destroyer"},
                std::nullopt});
        }
        auto chosen = skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog);
        expect(static_cast<bool>(chosen), "serial profile's stock Coruscant fixture resolves");
        if (!chosen) return std::nullopt;
        fixture = std::move(chosen).value();
    } else if (three_vs_three) {
        skirmish::SetupMapQuery query;
        query.minimum_capacity = 6;
        const auto maps = skirmish::setup_maps(filesystem.value(), catalog.value().catalog, query);
        expect(static_cast<bool>(maps), "stock diagnostic maps load");
        if (!maps) return std::nullopt;
        bool selected = false;
        for (const auto& map : maps.value()) {
            if (!map.unavailable.empty()) continue;
            for (const auto& rebel : map.teams) {
                if (rebel.spawn_capacity.value_or(0) < 3
                    || std::find(rebel.factions.begin(), rebel.factions.end(), "Rebel") == rebel.factions.end()) continue;
                for (const auto& empire : map.teams) {
                    if (empire.team == rebel.team || empire.spawn_capacity.value_or(0) < 3
                        || std::find(empire.factions.begin(), empire.factions.end(), "Empire") == empire.factions.end()) continue;
                    skirmish::FixtureOptions options;
                    options.map = map.path;
                    options.slots = std::vector<skirmish::LobbySlot>{};
                    for (std::uint32_t index = 0; index < 3; ++index)
                        options.slots->push_back({index + 1, "Rebel", rebel.team, index == 0, {}, std::nullopt});
                    for (std::uint32_t index = 0; index < 3; ++index)
                        options.slots->push_back({index + 4, "Empire", empire.team, false, {}, std::nullopt});
                    const auto chosen = skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog);
                    if (!chosen) continue;
                    fixture = chosen.value();
                    selected = true;
                    break;
                }
                if (selected) break;
            }
            if (selected) break;
        }
        expect(selected, "installed content supplies an authored 3v3 space start");
        if (!selected) return std::nullopt;
        std::cout << "SQUADRON-TRACE map=" << fixture.map << " players=" << fixture.slots.size() << '\n';
    }
    if (battle_seed) fixture.seed = *battle_seed;
    if (rebel_ai) {
        fixture.slots[0].faction = "Empire";
        fixture.slots[0].fleet = {"Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship"};
        fixture.slots[1].faction = "Rebel";
        fixture.slots[1].fleet = {"Rebel_X-Wing_Squadron", "Corellian_Corvette", "Nebulon_B_Frigate"};
    }
    if (underworld) {
        fixture.slots[0].fleet = {"Rebel_X-Wing_Squadron", "Corellian_Corvette", "Corellian_Corvette"};
        if (std::find(underworld_fleet.begin(), underworld_fleet.end(), "IG-2000") != underworld_fleet.end())
            fixture.slots[0].fleet = {"Nebulon_B_Frigate"};
        if (!player_fleet.empty()) fixture.slots[0].fleet = player_fleet; // WST-16: the idle player's own fleet
        fixture.slots[1].faction = "Underworld";
        fixture.slots[1].fleet = underworld_fleet.empty()
            ? std::vector<std::string>{"StarViper_Squadron", "StarViper_Squadron", "StarViper_Squadron", "Skipray_Squadron"}
            : underworld_fleet;
    }
    // WLD-10: the Empire AI's fleet carries an Underworld Crusader against the idle Rebel torpedo bombers.
    if (crusader) {
        fixture.slots[1].fleet.push_back("Crusader_Gunship");
        fixture.slots[0].fleet = {"Y-Wing_Squadron", "Y-Wing_Squadron"};
    }
    // WAB-36 (legacy #2997): the AI (slots[1]) fields the given fleet: the Empire against idle Rebel
    // corvettes, or with rebel_ai the Rebels against the idle Empire group above.
    if (!ai_fleet.empty()) {
        fixture.slots[1].fleet = ai_fleet;
        if (!rebel_ai) fixture.slots[0].fleet = {"Corellian_Corvette", "Corellian_Corvette"};
    }
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(inputs), "FoC start inputs read");
    if (!inputs) return std::nullopt;
    if (underworld || !ai_fleet.empty() || serial_profile) {
        // As the Underworld plan fixture: keep the pinned M2 types and add the Underworld
        // fleet, the factions' authored forces and the station marker types.
        auto extended = input;
        for (const auto type : eawr::units::pinned_m2_types()) extended.types.emplace_back(type);
        for (const auto obstacle : eawr::units::pinned_m2_obstacles()) extended.obstacles.emplace_back(obstacle);
        for (const auto& slot : fixture.slots) extended.types.insert(extended.types.end(), slot.fleet.begin(), slot.fleet.end());
        for (const auto& forces : inputs.value().faction_forces)
            for (const auto& type : forces.space_skirmish_default_forces) extended.types.push_back(type);
        for (const auto& placement : inputs.value().placements)
            for (const auto& candidate : placement.marker_for) extended.types.push_back(candidate.type);
        tables = eawr::units::load_unit_tables(extended);
        expect(static_cast<bool>(tables), "selected fixture's unit tables load");
        if (!tables) return std::nullopt;
        inputs.value().tables = &tables.value();
    }
    auto start = skirmish::build_start(fixture, inputs.value());
    expect(static_cast<bool>(start), "FoC start builds" + (start ? std::string() : ": " + start.error().message));
    if (!start) return std::nullopt;
    if (three_vs_three || serial_profile) start.value().setup = skirmish::recording_setup(fixture, start.value());
    if (!rebel_ai && !three_vs_three && !serial_profile) {
        // FH-20/WHZ-50: this controlled two-faction engagement omits Hutt map fixtures.
        // Their authored Structure category is a valid nearer target in the stock scene.
        std::set<eawr::sim::EntityId> omitted;
        for (const auto& unit : start.value().units) {
            const auto owner = std::find_if(start.value().players.begin(), start.value().players.end(),
                [&](const auto& player) { return player.player.player_id == unit.state.owner; });
            if (unit.role == skirmish::UnitRole::map_object && owner != start.value().players.end()
                && owner->faction == "Hutts") omitted.insert(unit.state.entity_id);
        }
        std::erase_if(start.value().units, [&](const auto& unit) { return omitted.contains(unit.state.entity_id); });
        std::erase_if(start.value().setup.units, [&](const auto& unit) { return omitted.contains(unit.entity_id); });
    }
    auto economy = skirmish::economy_rules(start.value(), inputs.value(), tables.value());
    expect(static_cast<bool>(economy), "skirmish economy loads: " + (economy ? std::string() : economy.error().message));
    if (!economy) return std::nullopt;
    auto content = skirmish::session_content(tables.value(), skirmish::human_slots(fixture));
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return std::nullopt;
    // #495: the AI battle runs on the map's fog grid, as the live session does.
    auto fog = skirmish::fog_rules(inputs.value());
    expect(static_cast<bool>(fog), "the M2 fog grid builds");
    if (!fog) return std::nullopt;
    content.value().fog = fog.value();
    content.value().map_box = skirmish::map_box(inputs.value());
    Content out{start.value(), std::move(content).value(), skirmish::victory_rules(start.value(), tables.value()),
        skirmish::ai_setup(start.value(), inputs.value(), tables.value()), {}, {}, {}, std::move(economy).value()};
    // FT-02/WNO-11: the freestore's regional fallback needs the perception/goal host
    // when its forced nearest query finds no object. Match the production live AI setup.
    auto enabled = skirmish::enable_goal_system(filesystem.value(), out.ai);
    expect(static_cast<bool>(enabled), "the tactical goal system loads");
    if (!enabled) return std::nullopt;
    auto modules = skirmish::ai_modules(filesystem.value(), out.ai);
    expect(static_cast<bool>(modules), "the AI's Lua files load: " + (modules ? std::string() : modules.error().message));
    if (!modules) return std::nullopt;
    out.modules = std::move(modules).value();
    auto plan_modules = skirmish::ai_modules(filesystem.value(), out.ai, true);
    expect(static_cast<bool>(plan_modules), "the selected plans load from the FoC view");
    if (!plan_modules) return std::nullopt;
    out.plan_modules = std::move(plan_modules).value();
    for (const auto& type : tables.value().units) out.type_names.emplace(skirmish::type_id(type.id), type.id);
    return out;
}

// Diagnostic battle with the stock scripts. Requests and actual transitions are separate:
// redundant requests and timed expiration must remain visible in a cause investigation.
int ability_trace(const Content& content, std::uint64_t ticks, const std::filesystem::path& path) {
    auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors, content.content.durability,
        content.content.motion, content.content.fog, content.content.combat, content.victory, content.content.abilities,
        content.economy, content.content.map_box);
    if (!world) { std::cerr << world.error().message << '\n'; return 1; }
    auto setup = content.ai;
    setup.journal = std::make_shared<foc::AiJournal>();
    auto session = foc::create_session(std::move(world).value(), setup, content.modules);
    if (!session) { std::cerr << session.error().message << '\n'; return 1; }
    const eawr::platform::ThreadWorkerAdapter executor(1);
    std::ofstream output(path);
    output << "tick,event,unit,type,ability,action,issuer,nearest_enemy,attack_range,enemies_in_range,formation_target,order\n";
    std::map<std::pair<eawr::sim::EntityId, tactical::AbilityKind>, bool> active;
    std::map<eawr::sim::EntityId, eawr::sim::EntityId> last_target;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto result = session.value().step(executor);
        if (!result) { std::cerr << result.error().message << '\n'; return 1; }
        const auto& current = session.value().world();
        const auto row = [&](std::string_view event, eawr::sim::EntityId id, tactical::AbilityKind kind,
            std::int64_t action, std::uint64_t issuer) {
            const auto units = current.units();
            const auto unit = std::find_if(units.begin(), units.end(), [&](const auto& item) { return item.entity_id == id; });
            if (unit == units.end()) return;
            double nearest = -1;
            for (const auto& other : units) {
                if ((other.owner != 1 && other.owner != 2) || other.owner == unit->owner) continue;
                const double dx = static_cast<double>(other.position.x.raw() - unit->position.x.raw()) / eawr::sim::math::Fixed::scale;
                const double dy = static_cast<double>(other.position.y.raw() - unit->position.y.raw()) / eawr::sim::math::Fixed::scale;
                const double distance = std::hypot(dx, dy);
                if (nearest < 0 || distance < nearest) nearest = distance;
            }
            const auto mind = current.squadron_state(id);
            auto type = unit->type_id;
            if (mind && !mind->roster.empty()) {
                const auto member = std::find_if(units.begin(), units.end(), [&](const auto& item) {
                    return std::find(mind->roster.begin(), mind->roster.end(), item.entity_id) != mind->roster.end();
                });
                if (member != units.end()) type = member->type_id;
            }
            double range = 0;
            for (const auto& profile : content.ai.content.types) if (profile.type_id == type) {
                const auto fixed = eawr::script::numeric::to_fixed(profile.max_attack_distance);
                if (fixed) range = static_cast<double>(fixed.value().raw()) / eawr::sim::math::Fixed::scale;
            }
            output << current.completed_tick() << ',' << event << ',' << id << ',' << content.type_names.at(unit->type_id)
                << ',' << tactical::to_string(kind) << ',' << action << ',' << issuer << ',' << nearest << ',' << range << ','
                << (nearest >= 0 && nearest <= range ? 1 : 0) << ','
                << (mind ? mind->target : 0) << ',' << static_cast<int>(unit->order.kind) << '\n';
        };
        for (const auto& routed : result.value().script_input) {
            const auto& command = routed.command;
            if (command.verb != foc::verb_ability || command.arguments.size() != 4) continue;
            const auto& id = std::get<auth::Handle>(command.arguments[1].data);
            const auto kind = eawr::script::numeric::to_exact_integer(std::get<eawr::script::numeric::LuaNumber>(command.arguments[2].data));
            const auto action = eawr::script::numeric::to_exact_integer(std::get<eawr::script::numeric::LuaNumber>(command.arguments[3].data));
            if (kind && action) row(routed.submitted ? "request" : "dropped", id.id,
                static_cast<tactical::AbilityKind>(kind.value()), action.value(), command.issuer);
        }
        for (const auto& instance : result.value().world.snapshot->instances()) {
            if (instance.owner != 2) continue;
            for (const auto& slot : instance.abilities) {
                const auto key = std::pair{instance.entity_id, slot.kind};
                const bool before = active[key];
                if (slot.active != before) row("transition", instance.entity_id, slot.kind, slot.active ? 1 : 2, 0);
                active[key] = slot.active;
            }
        }
        const auto current_units = current.units();
        for (const auto& squadron : current.squadrons()) {
            const auto mind = current.squadron_state(squadron.container);
            if (!mind || mind->target == 0 || mind->target == last_target[squadron.container]) continue;
            last_target[squadron.container] = mind->target;
            bool locked = false;
            for (const auto member : squadron.members) {
                const auto state = current.ability_state(member);
                const auto unit = std::find_if(current_units.begin(), current_units.end(),
                    [&](const auto& item) { return item.entity_id == member; });
                if (!state || unit == current_units.end() || unit->owner != 2) continue;
                const auto* profile = current.abilities().find(unit->type_id);
                if (profile == nullptr) continue;
                const auto slot = tactical::ability_slot(*profile, tactical::AbilityKind::spoiler_lock);
                locked = locked || (slot && state->slots[*slot].active);
            }
            row("engagement", squadron.container, tactical::AbilityKind::spoiler_lock, locked ? 1 : 2, 0);
        }
    }
    std::ofstream journal(path.string() + ".plans.csv");
    journal << "tick,plan,event,detail\n";
    for (const auto& event : setup.journal->plans)
        journal << event.tick << ',' << event.plan << ',' << event.event << ",\"" << event.detail << "\"\n";
    std::cout << "Ability trace: " << ticks << " ticks, " << path << '\n';
    return output && journal ? 0 : 1;
}

// `input`: the idle player's own commands, each stepped in at its key's tick.
std::optional<Run> run(const Content& content, std::size_t workers, std::uint64_t ticks,
    const std::vector<tactical::PlayerCommand>& input = {}, bool observe_stock = false, bool observe_autofire = false,
    std::ostream* squadron_rows = nullptr) {
    sim_headless::SquadronTrace squadron_trace;
    if (squadron_rows) sim_headless::SquadronTrace::header(*squadron_rows);
    auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors, content.content.durability,
        content.content.motion, content.content.fog, content.content.combat, content.victory,
        content.content.abilities, content.economy, content.content.map_box);
    expect(static_cast<bool>(world), "the world is created: "
        + (world ? std::string() : world.error().code + " " + world.error().message));
    if (!world) return std::nullopt;
    if (workers == 1) {
        const auto snapshot = world.value().snapshot();
        std::cout << "Initial object fog (snapshot player bits):";
        for (std::size_t index = 0; index < snapshot->players().size(); ++index)
            std::cout << " bit" << index << "=player" << snapshot->players()[index].player_id;
        std::cout << '\n';
        for (const auto& instance : snapshot->instances()) {
            const auto name = content.type_names.find(instance.type_id);
            std::cout << "  unit " << instance.entity_id << " owner " << instance.owner
                      << " type " << (name == content.type_names.end() ? "(unknown)" : name->second)
                      << " visible_to " << instance.visible_to << " x "
                      << instance.fixed_transform.rows[0][3].trunc_to_integer() << " y "
                      << instance.fixed_transform.rows[1][3].trunc_to_integer() << " reveal_range "
                      << (instance.reveal_range ? instance.reveal_range->trunc_to_integer() : 0) << '\n';
        }
    }
    auto setup = content.ai;
    setup.journal = std::make_shared<foc::AiJournal>();
    auto session = foc::create_session(std::move(world).value(), setup, content.modules);
    expect(static_cast<bool>(session), "the AI session is created: " + (session ? std::string() : session.error().code + " " + session.error().message));
    if (!session) return std::nullopt;
    std::set<eawr::sim::EntityId> ai_units, crusaders;
    std::map<eawr::sim::EntityId, tactical::PlayerId> observed_owners;
    for (const auto& unit : content.start.units) {
        if (unit.state.owner == 2) ai_units.insert(unit.state.entity_id);
        if (observe_stock) observed_owners.emplace(unit.state.entity_id, unit.state.owner);
        if (unit.state.owner == 2 && unit.state.type_id == skirmish::type_id("Crusader_Gunship")) crusaders.insert(unit.state.entity_id);
    }
    auto stock_ai_units = observe_stock ? ai_units : std::set<eawr::sim::EntityId>{};
    // Match live dispatch: small named phases stay inline; larger phases still use the pool (ADR-009).
    const eawr::platform::ThreadWorkerAdapter executor(workers,
        eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost);
    Run out;
    std::int64_t previous_hull = 0;
    bool blast_armed = false;
    std::size_t leech_trace = 0;
    const auto began = std::chrono::steady_clock::now();
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        const auto previous_snapshot = session.value().world().snapshot();
        std::vector<tactical::PlayerCommand> due;
        for (const auto& command : input)
            if (command.key.tick == session.value().world().completed_tick()) due.push_back(command);
        // Independently sample readiness before switch-on. The target is sampled after
        // this tick's targeting/commands, so a same-tick acquisition has a witness too.
        std::set<std::pair<eawr::sim::EntityId, tactical::AbilityKind>> ready_before;
        if (observe_autofire) {
            const auto& battle = session.value().world();
            for (const auto& source : battle.units()) {
                if (source.owner != 2) continue;
                const auto state = battle.ability_state(source.entity_id);
                const auto* profile = content.content.abilities.find(source.type_id);
                const auto health = battle.durability_state(source.entity_id);
                if (!state || !profile) continue;
                tactical::AbilityGate gate;
                gate.hardpoint_profile = content.content.durability.find(source.type_id);
                gate.hardpoint_state = health ? &*health : nullptr;
                for (const auto kind : {tactical::AbilityKind::tractor_beam, tactical::AbilityKind::leech_shields}) {
                    const auto slot = tactical::ability_slot(*profile, kind);
                    if (slot && !state->slots[*slot].active
                        && tactical::ability_ready(profile->abilities[*slot], state->slots[*slot], gate, tick))
                        ready_before.emplace(source.entity_id, kind);
                }
            }
        }
        auto stepped = session.value().step(executor, due);
        expect(static_cast<bool>(stepped), "step " + std::to_string(tick + 1) + (stepped ? std::string() : ": " + stepped.error().message));
        if (!stepped) return std::nullopt;
        if (squadron_rows) squadron_trace.observe(*squadron_rows, session.value().world(), content.content);
        if (observe_autofire) {
            const auto& battle = session.value().world();
            const auto live = battle.units();
            for (const auto& source : live) {
                if (source.owner != 2) continue;
                const auto state = battle.ability_state(source.entity_id);
                const auto attack = battle.combat_state(source.entity_id);
                const auto* profile = content.content.abilities.find(source.type_id);
                if (!state || !attack || !profile || attack->attack_target == 0) continue;
                const auto target = std::find_if(live.begin(), live.end(), [&](const auto& unit) {
                    return unit.entity_id == attack->attack_target;
                });
                if (target == live.end() || target->owner == source.owner) continue;
                const auto* combat = content.content.combat.find(source.type_id);
                for (const auto& nested : profile->special) {
                    const auto kind = nested.kind == tactical::SpecialAbilityKind::tractor_beam ? tactical::AbilityKind::tractor_beam
                        : nested.kind == tactical::SpecialAbilityKind::leech_shields ? tactical::AbilityKind::leech_shields
                        : tactical::AbilityKind::none;
                    if (kind == tactical::AbilityKind::none) continue;
                    auto& first_ready = kind == tactical::AbilityKind::tractor_beam ? out.first_tractor_ready_target : out.first_leech_ready_target;
                    const auto& first_lock = kind == tactical::AbilityKind::tractor_beam ? out.first_tractor_lock : out.first_leech_lock;
                    if (first_ready || first_lock) continue;
                    if (!ready_before.contains({source.entity_id, kind})) continue;
                    const auto maximum = nested.beam_max_range.raw() > 0 ? nested.beam_max_range
                        : combat ? combat->max_attack_distance.value_or(eawr::sim::math::Fixed{}) : eawr::sim::math::Fixed{};
                    auto minimum = nested.beam_min_range.raw() > 0 ? nested.beam_min_range
                        : combat ? combat->min_attack_distance : eawr::sim::math::Fixed{};
                    // Conservative fully-in-range witness: centre inside the maximum, outside
                    // a positive minimum plus either footprint extent. No new range assumption for mods.
                    if (const auto* footprint = content.content.motion.footprint(target->type_id); footprint && minimum.raw() > 0)
                        minimum = eawr::sim::math::Fixed::from_raw(minimum.raw() + std::max(footprint->x_extent.raw(), footprint->y_extent.raw()));
                    const auto target_health = battle.durability_state(target->entity_id);
                    const auto* target_combat = content.content.combat.find(target->type_id);
                    if (target_health && target_health->hull.raw() > 0 && target_combat
                        && tactical::special_type_matches(nested.filter, target->type_id, target_combat->category_bits)
                        && (kind != tactical::AbilityKind::leech_shields || target_health->shields.raw() > 0)
                        && tactical::within_range(source.position, target->position, maximum, tactical::RangeMetric::planar)
                        && (minimum.raw() <= 0 || !tactical::within_range(source.position, target->position, minimum, tactical::RangeMetric::planar))) first_ready = tick;
                }
            }
        }
        const auto& result = stepped.value();
        if (workers == 1) {
            for (const auto& unit : result.world.snapshot->instances()) {
                const auto type = content.type_names.find(unit.type_id);
                if (type != content.type_names.end() && (type->second == "Broadside_Class_Cruiser" ||
                    type->second == "Marauder_Missile_Cruiser")) out.artillery_seen.insert(unit.entity_id);
            }
        }
        // WAB-83: witness the range precondition independently of the stand-in script.
        for (const auto& holder : result.world.snapshot->instances()) {
            if (holder.owner != 2) continue;
            if (std::any_of(holder.abilities.begin(), holder.abilities.end(),
                [](const auto& status) { return status.kind == tactical::AbilityKind::full_salvo && status.active; }))
                ++out.salvo_active_ticks;
            const bool ready = std::any_of(holder.abilities.begin(), holder.abilities.end(),
                [](const auto& status) { return status.kind == tactical::AbilityKind::full_salvo && status.ready; });
            if (!ready) continue;
            ++out.salvo_ready_ticks;
            for (const auto& other : result.world.snapshot->instances()) {
                if (other.owner != 1) continue;
                const auto dx = (holder.fixed_transform.rows[0][3].raw() - other.fixed_transform.rows[0][3].raw()) >> 24;
                const auto dy = (holder.fixed_transform.rows[1][3].raw() - other.fixed_transform.rows[1][3].raw()) >> 24;
                const auto dz = (holder.fixed_transform.rows[2][3].raw() - other.fixed_transform.rows[2][3].raw()) >> 24;
                const auto gap = static_cast<std::int64_t>(std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz)));
                if (out.salvo_min_gap < 0 || gap < out.salvo_min_gap) out.salvo_min_gap = gap;
            }
        }
        // WSD-06: the player's hull in the tick after a blast object existed, against the tick before it detonated.
        std::int64_t player_hull = 0;
        for (const auto& instance : result.world.snapshot->instances()) {
            if (instance.owner == 1 && instance.durability) player_hull += instance.durability->hull.trunc_to_integer();
        }
        for (const auto& holder : result.world.snapshot->instances()) {
            const bool has = holder.owner == 2 && std::any_of(holder.abilities.begin(), holder.abilities.end(),
                [](const auto& status) { return status.kind == tactical::AbilityKind::self_destruct; });
            if (!has) continue;
            if (out.sd_alive_first == 0) out.sd_alive_first = tick + 1;
            out.sd_alive_last = tick + 1;
            for (const auto& status : holder.abilities) {
                if (status.kind == tactical::AbilityKind::self_destruct && status.ready) ++out.sd_ready_ticks;
            }
            for (const auto& other : result.world.snapshot->instances()) {
                if (other.owner != 1) continue;
                const std::int64_t dx = (holder.fixed_transform.rows[0][3].raw() - other.fixed_transform.rows[0][3].raw()) >> 24;
                const std::int64_t dy = (holder.fixed_transform.rows[1][3].raw() - other.fixed_transform.rows[1][3].raw()) >> 24;
                const auto gap = static_cast<std::int64_t>(std::sqrt(static_cast<double>(dx * dx + dy * dy)));
                if (out.sd_min_gap < 0 || gap < out.sd_min_gap) out.sd_min_gap = gap;
            }
        }
        if (blast_armed) out.blast_hull_drop += std::max<std::int64_t>(0, previous_hull - player_hull);
        blast_armed = !result.world.snapshot->ability_spawns().empty();
        if (blast_armed) ++out.blast_objects;
        previous_hull = player_hull;
        // WLS-04: retain a bounded admission trace when the real-data fixture refuses a lock.
        for (const auto& routed : result.script_input) {
            const auto& arguments = routed.command.arguments;
            if (workers != 1 || leech_trace >= 4 || routed.command.verb != foc::verb_ability
                || arguments.size() != 5 || !routed.submitted) continue;
            ++leech_trace;
            const auto source = std::get<auth::Handle>(arguments[1].data).id;
            const auto target = std::get<auth::Handle>(arguments[4].data).id;
            std::cout << "leech-admission " << command_text(routed) << " source " << source << " target " << target;
            for (const auto& instance : result.world.snapshot->instances()) {
                if (instance.entity_id != source && instance.entity_id != target) continue;
                const auto state = session.value().world().durability_state(instance.entity_id);
                std::cout << " [" << content.type_names.at(instance.type_id) << " nebula " << instance.in_nebula
                          << " shields " << (state ? state->shields.raw() : 0) << ']';
            }
            for (const auto& event : result.world.snapshot->events())
                if (event.player == routed.key.player_id && event.sequence == routed.key.sequence)
                    std::cout << ' ' << tactical::to_string(event.kind) << ':' << tactical::to_string(event.reason);
            std::cout << '\n';
        }
        out.hashes.push_back(result.state_sha256);
        out.world_hashes.push_back(result.world.state_sha256);
        // WSW-36..40: log the untouched combat fixture's stock special activity,
        // without imposing an unlock or changing its economy/setup to force it.
        for (const auto& account : result.world.snapshot->economy()) if (account.player == 2) {
            for (const auto& [type, count] : account.lifetime) {
                const auto name = content.type_names.find(type);
                if (name != content.type_names.end() && (name->second == "RS_Ion_Cannon_Use_Upgrade" ||
                    name->second == "ES_Hypervelocity_Gun_Use_Upgrade" || name->second == "US_Plasma_Cannon_Use_Upgrade"))
                    out.special_use_purchases = std::max(out.special_use_purchases, count);
            }
        }
        for (const auto& projectile : result.world.snapshot->projectiles()) {
            if (projectile.owner != 2 || out.special_projectile_ids.contains(projectile.id)) continue;
            const auto instances = result.world.snapshot->instances();
            const auto source = std::lower_bound(instances.begin(), instances.end(), projectile.shooter,
                [](const auto& unit, eawr::sim::EntityId id) { return unit.entity_id < id; });
            if (source == instances.end() || source->entity_id != projectile.shooter) continue;
            if (source->planetary_weapon || std::any_of(source->manual_weapons.begin(), source->manual_weapons.end(),
                [&](const auto& weapon) { return weapon.hardpoint == projectile.weapon; })) {
                ++out.special_projectiles;
                out.special_projectile_ids.insert(projectile.id);
            }
        }
        for (const auto& routed : result.script_input) {
            out.commands.push_back(command_text(routed));
            const auto& command = routed.command;
            if (!routed.submitted || command.verb != foc::verb_ability || command.arguments.size() < 4) continue;
            const auto kind = eawr::script::numeric::to_exact_integer(std::get<eawr::script::numeric::LuaNumber>(command.arguments[2].data));
            if (kind && static_cast<tactical::AbilityKind>(kind.value()) == tactical::AbilityKind::buzz_droids) ++out.plan_buzz;
            if (kind && static_cast<tactical::AbilityKind>(kind.value()) == tactical::AbilityKind::lure) ++out.plan_lure;
        }
        for (const auto& event : result.world.snapshot->events()) {
            if (observe_stock && event.player == 2) {
                if (event.kind == tactical::EventKind::pad_construction_started) ++out.pad_constructions_started;
                if (event.kind == tactical::EventKind::pad_construction_completed) ++out.pad_constructions_completed;
                if (event.kind == tactical::EventKind::order_rejected) out.ai_rejections.push_back(event);
            }
            if (event.kind == tactical::EventKind::order_rejected && event.player == 2
                && event.reason == tactical::RejectReason::target_hidden) out.hidden_refusals.push_back(event.tick);
            if (event.kind == tactical::EventKind::order_accepted && event.player == 2) out.ai_accepted.push_back(event.tick);
            if (event.kind == tactical::EventKind::order_accepted && event.player == 2) {
                const auto instances = result.world.snapshot->instances();
                const auto unit = std::find_if(instances.begin(), instances.end(),
                    [&](const tactical::TacticalInstance& instance) { return instance.entity_id == event.unit; });
                const auto name = unit == instances.end() ? content.type_names.end() : content.type_names.find(unit->type_id);
                ++out.orders_by_type[name == content.type_names.end() ? std::string("(gone)") : name->second];
            }
            out.events.push_back(std::to_string(event.tick) + " " + std::string(tactical::to_string(event.kind)) + " " +
                std::to_string(event.unit));
        }
        std::map<eawr::sim::EntityId, eawr::sim::EntityId> focus; // WHE-24: concentrated target -> its holder
        for (const auto& instance : result.world.snapshot->instances()) {
            // The snapshot publishes a target for the beams only; the slot keeps the concentrated one.
            for (std::size_t index = 0; index < instance.abilities.size(); ++index) {
                if (instance.abilities[index].kind != tactical::AbilityKind::concentrate_fire || !instance.abilities[index].active) continue;
                ++out.concentrate_frames;
                const auto state = session.value().world().ability_state(instance.entity_id);
                if (state && index < state->slots.size() && state->slots[index].target != 0)
                    focus.emplace(state->slots[index].target, instance.entity_id);
            }
            if (observe_stock) {
                observed_owners.insert_or_assign(instance.entity_id, instance.owner);
                if (instance.owner == 2) stock_ai_units.insert(instance.entity_id);
            }
            if (instance.corrupt) out.corrupted.insert(instance.entity_id);
            const auto name = content.type_names.find(instance.type_id);
            if (name != content.type_names.end() && name->second == "Buzz_Droids") out.droids.insert(instance.entity_id);
            for (const auto& ability : instance.abilities)
                if (ability.kind == tactical::AbilityKind::leech_shields && ability.active && ability.target != 0)
                    { ++out.shield_leech_frames; if (!out.first_leech_lock) out.first_leech_lock = tick; }
            for (const auto& ability : instance.abilities)
                if (ability.kind == tactical::AbilityKind::tractor_beam && ability.active && ability.target != 0)
                    { ++out.tractor_frames; if (!out.first_tractor_lock) out.first_tractor_lock = tick; }
            for (const auto& ability : instance.abilities)
                if (ability.kind == tactical::AbilityKind::lure && ability.active) ++out.lure_frames;
        }
        for (const auto& event : result.world.snapshot->combat_events()) {
            if (ai_units.contains(event.shooter)
                && (event.kind == tactical::CombatEventKind::weapon_fired || event.kind == tactical::CombatEventKind::projectile_hit)) {
                const auto before = previous_snapshot->instances();
                const auto target = std::find_if(before.begin(), before.end(),
                    [&](const auto& instance) { return instance.entity_id == event.target; });
                if (target != before.end() && target->stealth_on
                    && target->type_id == skirmish::type_id("Vengeance_Frigate")) {
                    const auto after = result.world.snapshot->instances();
                    const auto published = std::find_if(after.begin(), after.end(),
                        [&](const auto& instance) { return instance.entity_id == event.target; });
                    out.cloak_contacts.push_back(std::to_string(event.tick) + " "
                        + std::string(tactical::to_string(event.kind)) + " shooter=" + std::to_string(event.shooter)
                        + " target=" + std::to_string(event.target) + " selected=" + std::to_string(event.selected_target)
                        + " hidden-before=" + std::to_string(target->stealth_invisible)
                        + " hidden-after=" + std::to_string(published != after.end() && published->stealth_invisible)
                        + " cloak-after=" + std::to_string(published != after.end() && published->stealth_on));
                }
            }
            if (event.kind == tactical::CombatEventKind::point_defence_zap) ++out.point_defence_zaps;
            if (event.kind != tactical::CombatEventKind::projectile_hit) continue;
            if (const auto held = focus.find(event.target); held != focus.end() && held->second != event.shooter)
                ++out.recruit_hits;
            if (observe_stock && stock_ai_units.contains(event.shooter)) {
                ++out.stock_hits;
                if (out.first_stock_hit == 0) out.first_stock_hit = event.tick;
                // Retain last published owners so a lethal hit still has a target identity.
                // A hit on a neutral pad is not contact with the human player's fleet.
                const auto target = observed_owners.find(event.target);
                if (target != observed_owners.end() && target->second == 1) {
                    ++out.player_hits;
                    if (out.first_player_hit == 0) out.first_player_hit = event.tick;
                }
            }
            if (crusaders.contains(event.target)) {
                ++out.crusader_hits;
                const auto instances = result.world.snapshot->instances();
                const auto shooter = std::find_if(instances.begin(), instances.end(),
                    [&](const auto& instance) { return instance.entity_id == event.shooter; });
                const auto name = shooter == instances.end() ? content.type_names.end() : content.type_names.find(shooter->type_id);
                ++out.crusader_hit_by[name == content.type_names.end() ? std::string("(gone)") : name->second];
            }
            if (ai_units.contains(event.shooter)) {
                out.ai_hit_targets.emplace_back(event.tick, event.target);
                ++out.ai_hits;
                if (out.first_ai_hit == 0) out.first_ai_hit = event.tick;
            }
            out.hits_csv += std::to_string(event.tick) + "," + std::to_string(event.shooter) + "," +
                std::to_string(event.target) + "," + std::to_string(event.aim.x.trunc_to_integer()) + "," +
                std::to_string(event.aim.y.trunc_to_integer()) + "," + std::to_string(event.aim.z.trunc_to_integer()) + "\n";
        }
        for (const auto& diagnostic : result.scripts.diagnostics) out.diagnostics.insert(diagnostic.code + " " + diagnostic.message);
        std::int64_t tick_load = 0;
        for (const auto& load : result.scripts.loads) tick_load += load.instructions;
        out.max_instructions = std::max(out.max_instructions, tick_load);
        out.total_instructions += tick_load;
    }
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    if (observe_stock) for (const auto& [id, pad] : session.value().world().pads()) {
        (void)id;
        if (pad.under_construction != 0) ++out.pending_pad_constructions;
    }
    out.record = session.value().record();
    out.journal = *setup.journal;
    return out;
}

// Stock plans retain the construction/economy inputs they use (WBP-40, SAE-01/08).
// The economy-disabled variant is diagnostic only; it does not establish stock AI liveness.
std::optional<Content> combat_engagement_witness(const std::filesystem::path& root, std::uint64_t seed,
    bool economy_disabled = false) {
    auto content = load(root, false, false, {}, false, seed);
    if (content && economy_disabled) content->economy = {};
    return content;
}

std::pair<std::uint64_t, std::uint64_t> attack_activity(const Run& result) {
    std::uint64_t start = 0, order = 0;
    for (const auto& event : result.journal.plans) {
        if (event.player != 2 || (event.plan != "destroyunit" && event.plan != "destroyunitminimal"
            && event.plan != "flankplan" && event.plan != "turboattacklocation")) continue;
        if (event.event == "started" && (start == 0 || event.tick < start)) start = event.tick;
        if (event.event == "order" && (order == 0 || event.tick < order)) order = event.tick;
    }
    return {start, order};
}

void check_combat_engagement(const Run& result, const std::uint64_t seed, const std::uint64_t ticks) {
    expect(result.player_hits > 0, "the stock AI hits the player on combat seed " + std::to_string(seed));
    if (seed == 68) {
        const auto [start, order] = attack_activity(result);
        expect(start > 0 && start <= ticks && order > 0 && order <= ticks,
            "the stock combat witness starts and orders an attack-family plan");
    }
}

int passive_contact_control(const std::filesystem::path& root, const std::uint64_t ticks) {
    auto content = combat_engagement_witness(root, 68);
    if (!content) return 1;
    // Keep the same stock world and content. Only the AI's service is disabled.
    for (auto& player : content->ai.players) {
        player.ai = false;
        player.player_type.clear();
    }
    const auto result = run(*content, 1, ticks, {}, true);
    if (!result) return 1;
    const auto [start, order] = attack_activity(*result);
    std::cout << "forced-passive control: seed 68 ticks " << ticks << " player hits " << result->player_hits
              << " first hit " << result->first_player_hit << " attack " << start << '/' << order << '\n';
    // The exact same acceptance guard is used by the normal installed battle below.
    check_combat_engagement(*result, 68, ticks);
    return failures != 0 ? 1 : 0;
}

int stock_economy_witness(const std::filesystem::path& root) {
    constexpr std::uint64_t contact_window = 7200, closure_window = 18000;
    auto content = load(root, false, false, {}, false, 68);
    if (!content) return 1;
    expect(content->economy.player(2) != nullptr && !content->economy.menus.empty(),
        "stock economy witness retains its shared ledger and menus");
    std::optional<Run> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto result = run(*content, workers, contact_window, {}, true);
        if (!result) return 1;
        const auto [start, order] = attack_activity(*result);
        std::cout << "stock economy witness seed 68 workers " << workers << " ticks " << contact_window
            << " attack " << start << '/' << order << " AI hits " << result->stock_hits
            << " first hit " << result->first_stock_hit << " seconds " << result->seconds
            << " final " << result->hashes.back() << '\n';
        expect(start > 0 && start <= contact_window && order > 0 && order <= contact_window,
            "stock economy witness starts and orders an attack within the contact window");
        expect(result->stock_hits > 0 && result->first_stock_hit > 0 && result->first_stock_hit <= contact_window,
            "stock economy witness reaches an observed AI hit by tick 7200");
        expect(result->max_instructions < auth::Quotas{}.instructions_per_service / 4,
            "stock economy witness stays within the Lua budget");
        if (!baseline) baseline = std::move(*result);
        else expect(result->hashes == baseline->hashes && result->commands == baseline->commands
            && result->events == baseline->events && result->diagnostics == baseline->diagnostics
            && result->stock_hits == baseline->stock_hits && result->first_stock_hit == baseline->first_stock_hit,
            "stock economy witness hashes, commands, events, diagnostics and hits match on 1/2/4/8 workers");
    }
    auto short_replay = eawr::platform::headless_tick_hashes(baseline->record, content->content.sensors,
        content->content.durability, content->content.motion, content->content.combat, content->victory,
        content->content.fog, content->content.abilities, content->economy, content->content.map_box);
    expect(short_replay && short_replay.value() == baseline->world_hashes,
        "stock economy contact witness replays without scripts");

    auto closure = run(*content, 1, closure_window, {}, true);
    if (!closure) return 1;
    expect(closure->hashes.size() >= baseline->hashes.size()
        && std::equal(baseline->hashes.begin(), baseline->hashes.end(), closure->hashes.begin()),
        "stock economy closure run retains the contact witness prefix");
    expect(closure->stock_hits > 0 && closure->first_stock_hit > 0 && closure->first_stock_hit <= contact_window,
        "stock economy witness has hits by tick 18000 and first contact by tick 7200");
    expect(closure->max_instructions < auth::Quotas{}.instructions_per_service / 4,
        "stock economy closure witness stays within the Lua budget");
    expect(closure->pad_constructions_started > 0
        && closure->pad_constructions_started == closure->pad_constructions_completed
        && closure->pending_pad_constructions == 0,
        "stock economy witness completes all started pad constructions by tick 18000");
    // A plan still open at the cutoff counts as in flight when it started within the longest
    // completed secure-pad plan's duration of the cutoff; any older open plan is stuck (#3048).
    std::map<std::string, std::vector<std::uint64_t>> active; // target -> open start ticks
    std::uint64_t secure_starts = 0, secure_ends = 0, longest = 0;
    for (const auto& event : closure->journal.plans) {
        if (event.player != 2 || event.plan != "movetolocationrush") continue;
        if (event.event == "started") { active[event.target].push_back(event.tick); ++secure_starts; }
        else if (event.event == "finished" || event.event == "abandoned") {
            auto& open = active[event.target];
            expect(!open.empty(), "a secure-pad plan ends only after it starts");
            if (!open.empty()) {
                longest = std::max(longest, event.tick - open.front());
                open.erase(open.begin());
                ++secure_ends;
            }
        }
    }
    std::uint64_t in_flight = 0, stuck = 0;
    for (const auto& [target, open] : active) {
        for (const std::uint64_t start : open) ++(start + longest >= closure_window ? in_flight : stuck);
    }
    expect(secure_ends > 0 && stuck == 0 && secure_starts == secure_ends + in_flight,
        "stock economy witness ends every secure-pad plan started a plan's length before tick 18000");
    std::cout << "stock economy closure seed 68 ticks " << closure_window << " AI hits " << closure->stock_hits
        << " first hit " << closure->first_stock_hit << " pad builds " << closure->pad_constructions_started
        << '/' << closure->pad_constructions_completed << " pending " << closure->pending_pad_constructions
        << " secure plans " << secure_starts << '/' << secure_ends << " in flight " << in_flight
        << " longest " << longest << " seconds " << closure->seconds
        << " final " << closure->hashes.back() << '\n';
    auto long_replay = eawr::platform::headless_tick_hashes(closure->record, content->content.sensors,
        content->content.durability, content->content.motion, content->content.combat, content->victory,
        content->content.fog, content->content.abilities, content->economy, content->content.map_box);
    expect(long_replay && long_replay.value() == closure->world_hashes,
        "stock economy closure witness replays without scripts");
    return failures == 0 ? 0 : 1;
}

// FH-20: category search admits Hutt structures independently of R-09 opportunity priority.
void hutt_nearest_probe(const Content& content) {
    Content probe = content;
    probe.ai.xml.clear(); // This probe isolates the nearest binding from independent plans.
    const auto resource = skirmish::type_id("Orbital_Resource_Container");
    const auto type = std::find_if(probe.ai.content.types.begin(), probe.ai.content.types.end(),
        [&](const auto& entry) { return entry.type_id == resource; });
    const auto property = probe.ai.content.properties.find("NOTOPPORTUNITYTARGET");
    expect(type != probe.ai.content.types.end() && property != probe.ai.content.properties.end()
        && (type->property_bits & property->second) != 0 && !type->space_evaluator,
        "FH-20: stock Hutt resource retains its opportunity exclusion and no goal evaluator");
    const auto source = std::find_if(probe.ai.content.types.begin(), probe.ai.content.types.end(),
        [&](const auto& entry) { return entry.type_id == skirmish::type_id("Acclamator_Assault_Ship"); });
    expect(source != probe.ai.content.types.end() && source->locomotor && !source->star_base,
        "FH-13: the nearest probe source enters the freestore");
    const auto at = [](const std::int64_t x) {
        return eawr::sim::math::Vec3{eawr::sim::math::Fixed::from_raw(x * eawr::sim::math::Fixed::scale), {}, {}};
    };
    probe.start.setup.units = {
        {1, skirmish::type_id("Acclamator_Assault_Ship"), 2, at(0), eawr::sim::math::identity_quat(), {}},
        {2, resource, 7, at(100), eawr::sim::math::identity_quat(), {}},
        {3, resource, 4, at(50), eawr::sim::math::identity_quat(), {}},
        {4, skirmish::type_id("Skirmish_Rebel_Star_Base_1"), 1, at(1000), eawr::sim::math::identity_quat(), {}}};
    probe.start.setup.squadrons.clear();
    // FL-14: this query-only fixture replaces the starting companies, so their
    // registered objects and replenishment templates no longer belong to it.
    probe.start.setup.free_garrisons.clear();
    probe.start.units.clear();
    probe.content.fog.reset();
    probe.victory = {};
    probe.ai.freestore_module = "Data/Scripts/Test/HuttNearestProbe.lua";
    probe.modules[probe.ai.freestore_module] =
        "function Base_Definitions()\n"
        "  ServiceRate = 0\n"
        "  UnitServiceRate = 0\n"
        "end\n"
        "function main() end\n"
        "function On_Unit_Service(object)\n"
        "  if target_ordered then return end\n"
        "  target = Find_Nearest(object, \"Structure | Capital\", PlayerObject, false)\n"
        "  if target then\n"
        "    object.Attack_Target(target)\n"
        "    target_ordered = true\n"
        "  end\n"
        "end\n";
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const auto result = run(probe, workers, 120);
        expect(result.has_value(), "FH-20: nearest Hutt probe runs");
        if (!result) continue;
        std::size_t found = 0;
        for (const auto& command : result->record.commands) {
            if (const auto* attack = std::get_if<tactical::AttackPayload>(&command.payload)) {
                expect(attack->target == 2, "FH-20: nearest non-neutral Hutt structure wins over neutral scenery and farther starbase");
                ++found;
            }
        }
        expect(found == 1, "FH-20: the initial nearest search returns the Hutt structure despite opportunity exclusion");
        expect(result->diagnostics.empty(), "FH-20: the focused nearest script runs without diagnostics");
        std::cout << "Hutt nearest probe: " << workers << " workers, " << found << " attacks, "
                  << result->commands.size() << " routed commands\n";
        for (const auto& diagnostic : result->diagnostics) std::cout << diagnostic << '\n';
        if (workers == 1) reference = result->hashes;
        else expect(result->hashes == reference, "FH-20: Hutt nearest probe hashes agree on 1/2/4/8 workers");
    }
}

// #76 (space-abilities.md AB-44): a stand-in freestore that asks each of the AI's units for
// POWER_TO_WEAPONS through the Lua ability calls. The requests reach the world as ability
// commands, the same on 1 and 2 workers, and the record replays them without the script.
void ability_probe(const Content& content) {
    Content probe = content;
    probe.ai.xml.clear(); // This probe isolates explicit ability requests from independent plans.
    probe.ai.freestore_module = "Data/Scripts/Test/AbilityProbe.lua";
    probe.modules[probe.ai.freestore_module] =
        "function Base_Definitions()\n"
        "  ServiceRate = 1\n"
        "  UnitServiceRate = 1\n"
        "end\n"
        "function main()\n"
        "end\n"
        "function On_Unit_Service(object)\n"
        "  if object.Has_Ability(\"Power_To_Weapons\") and object.Is_Ability_Ready(\"Power_To_Weapons\")\n"
        "      and not object.Is_Ability_Active(\"Power_To_Weapons\") then\n"
        "    object.Activate_Ability(\"Power_To_Weapons\", true)\n"
        "  end\n"
        "end\n";
    const auto one = run(probe, 1, 300);
    const auto two = run(probe, 2, 300);
    expect(one && two && one->hashes == two->hashes, "the ability probe hashes alike on 1 and 2 workers");
    if (!one) return;
    std::size_t requests = 0;
    for (const auto& command : one->record.commands) {
        if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
            requests += ability->ability == tactical::AbilityKind::power_to_weapons
                && ability->action == tactical::AbilityAction::activate ? 1 : 0;
        }
    }
    std::cout << "ability probe: " << requests << " POWER_TO_WEAPONS request(s)\n";
    expect(requests > 0, "the Lua Activate_Ability reaches the world as an ability command");
    auto replayed = eawr::platform::headless_tick_hashes(one->record, content.content.sensors, content.content.durability,
        content.content.motion, content.content.combat, content.victory, content.content.fog,
        content.content.abilities, content.economy, content.content.map_box);
    expect(replayed && replayed.value() == one->world_hashes, "the ability probe's record replays without the script");
}

// WST-15: an AI battle with a Vengeance in the Underworld AI's fleet. The stock plans and the event library
// call Try_Ability(.., "STEALTH"); a focused stand-in freestore also switches it on. The requests
// reach the world as STEALTH ability commands, the battle hashes alike on 1, 2, 4 and 8 workers and
// the record replays without the scripts.
void vengeance_probe(const std::filesystem::path& root) {
    auto content = load(root, false, true, {"Vengeance_Frigate", "StarViper_Squadron"});
    if (!content) return;
    const auto vengeance_type = skirmish::type_id("Vengeance_Frigate");
    expect(std::any_of(content->start.units.begin(), content->start.units.end(), [&](const auto& unit) {
        return unit.state.type_id == vengeance_type && unit.state.owner == 2;
    }), "WST-15: the AI player's start carries a Vengeance");
    const auto count = [](const Run& result) {
        std::size_t requests = 0;
        for (const auto& command : result.record.commands) {
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
                requests += ability->ability == tactical::AbilityKind::stealth
                    && ability->action == tactical::AbilityAction::activate ? 1 : 0;
            }
        }
        return requests;
    };
    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, 2400);
        if (!result) return;
        runs.push_back(std::move(*result));
    }
    for (std::size_t index = 1; index < runs.size(); ++index) {
        expect(runs[index].hashes == runs.front().hashes, "WST-15: the Vengeance battle hashes alike at every worker count");
        expect(runs[index].commands == runs.front().commands, "WST-15: the Vengeance battle's script commands match");
    }
    std::cout << "vengeance battle: " << count(runs.front()) << " STEALTH request(s) from the stock plans, "
              << runs.front().ai_hits << " AI hit(s)\n";
    for (const auto& diagnostic : runs.front().diagnostics) std::cout << "vengeance diagnostic: " << diagnostic << '\n';
    auto replayed = eawr::platform::headless_tick_hashes(runs.front().record, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities,
        content->economy, content->content.map_box);
    expect(replayed && replayed.value() == runs.front().world_hashes, "WST-15: the Vengeance battle replays without the scripts");

    Content probe = *content;
    probe.ai.xml.clear();
    probe.ai.freestore_module = "Data/Scripts/Test/StealthProbe.lua";
    probe.modules[probe.ai.freestore_module] =
        "function Base_Definitions()\n"
        "  ServiceRate = 1\n"
        "  UnitServiceRate = 1\n"
        "end\n"
        "function main()\n"
        "end\n"
        "function On_Unit_Service(object)\n"
        "  if object.Has_Ability(\"STEALTH\") and object.Is_Ability_Ready(\"STEALTH\")\n"
        "      and not object.Is_Ability_Active(\"STEALTH\") then\n"
        "    object.Activate_Ability(\"STEALTH\", true)\n"
        "  end\n"
        "end\n";
    const auto one = run(probe, 1, 300);
    const auto two = run(probe, 2, 300);
    expect(one && two && one->hashes == two->hashes, "WST-15: the stealth probe hashes alike on 1 and 2 workers");
    if (!one) return;
    std::cout << "stealth probe: " << count(*one) << " STEALTH request(s)\n";
    expect(count(*one) > 0, "WST-15: the Lua Activate_Ability(\"STEALTH\") reaches the world as an ability command");
    auto probe_replay = eawr::platform::headless_tick_hashes(one->record, probe.content.sensors, probe.content.durability,
        probe.content.motion, probe.content.combat, probe.victory, probe.content.fog, probe.content.abilities,
        probe.economy, probe.content.map_box);
    expect(probe_replay && probe_replay.value() == one->world_hashes, "WST-15: the stealth probe's record replays without the script");
}

// WST-16 (#1905): the Underworld AI against an idle Rebel fleet whose Vengeance switches its cloak on
// at once. As in the debug build, the AI's queries and perception do not test the cloak: it may still
// plan on the hidden frigate, but only its attack orders on it are refused (WST-11), it never hits it
// while hidden, and it moves on: it keeps ordering and hitting the visible corvettes, and the refusals
// do not repeat every frame. The battle hashes alike on 1, 2, 4 and 8 workers and replays without the
// scripts.
int cloaked_enemy_probe(const std::filesystem::path& root, const std::uint64_t ticks) {
    auto content = load(root, false, true, {}, false, std::nullopt, {"Vengeance_Frigate", "Corellian_Corvette", "Corellian_Corvette"});
    if (!content) return 1;
    const auto vengeance_type = skirmish::type_id("Vengeance_Frigate");
    std::vector<eawr::sim::EntityId> cloakers;
    for (const auto& unit : content->start.units)
        if (unit.state.type_id == vengeance_type && unit.state.owner == 1) cloakers.push_back(unit.state.entity_id);
    expect(!cloakers.empty(), "WST-16: the idle player's start carries a Vengeance");
    if (cloakers.empty()) return 1;
    // Staged so contact does not depend on the AI's approach: the stock starts are about 4700 units apart and
    // the AI never reached either target within 2400 ticks. The Underworld fleet moves as one block to within
    // reach of the Vengeance, keeping its own formation offsets.
    {
        const auto idle = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [&](const auto& unit) { return unit.owner == 1 && unit.type_id == vengeance_type; });
        const auto raider = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [](const auto& unit) { return unit.owner == 2; });
        expect(idle != content->start.setup.units.end() && raider != content->start.setup.units.end(),
            "WST-16: both players start with units");
        if (idle == content->start.setup.units.end() || raider == content->start.setup.units.end()) return 1;
        const auto shift = [](const eawr::sim::math::Fixed from, const eawr::sim::math::Fixed to, const std::int64_t gap) {
            return to.trunc_to_integer() + gap - from.trunc_to_integer();
        };
        const auto dx = shift(raider->position.x, idle->position.x, 700);
        const auto dy = shift(raider->position.y, idle->position.y, 0);
        const auto dz = shift(raider->position.z, idle->position.z, 0);
        const auto move = [&](tactical::UnitState& unit) {
            if (unit.owner != 2) return;
            unit.position.x = eawr::sim::math::Fixed::from_integer(unit.position.x.trunc_to_integer() + dx).value();
            unit.position.y = eawr::sim::math::Fixed::from_integer(unit.position.y.trunc_to_integer() + dy).value();
            unit.position.z = eawr::sim::math::Fixed::from_integer(unit.position.z.trunc_to_integer() + dz).value();
        };
        for (auto& unit : content->start.setup.units) move(unit);
        for (auto& unit : content->start.units) move(unit.state);
        // WST-11 tests targeting, not damage immunity: DG-30 projectiles aimed at a visible
        // corvette can collide with the hidden frigate, and damage leaves its cloak on (WST-05).
        // Keep the stated three-ship enemy fleet, including no free starting fighter forces,
        // and move the visible ships into a separate firing corridor. The cloak remains the
        // nearer query target; stock AI must refuse it and continue fighting the visible ships.
        const auto corvette_type = skirmish::type_id("Corellian_Corvette");
        const auto retained = [&](const tactical::UnitState& unit) {
            return unit.owner != 1 || unit.type_id == vengeance_type || unit.type_id == corvette_type;
        };
        std::set<eawr::sim::EntityId> removed;
        for (const auto& unit : content->start.setup.units)
            if (!retained(unit)) removed.insert(unit.entity_id);
        std::erase_if(content->start.setup.units, [&](const auto& unit) { return !retained(unit); });
        std::erase_if(content->start.units, [&](const auto& unit) { return !retained(unit.state); });
        std::erase_if(content->start.setup.squadrons, [&](const auto& squadron) { return removed.contains(squadron.container); });
        std::erase_if(content->start.setup.free_garrisons, [](const auto& force) { return force.player == 1; });
        const auto separate = [&](tactical::UnitState& unit) {
            if (unit.owner == 1 && unit.type_id == corvette_type)
                unit.position.y = eawr::sim::math::Fixed::from_raw(unit.position.y.raw() + 1200 * eawr::sim::math::Fixed::scale);
        };
        for (auto& unit : content->start.setup.units) separate(unit);
        for (auto& unit : content->start.units) separate(unit.state);
        expect(std::count_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [](const auto& unit) { return unit.owner == 1; }) == 3,
            "WST-16: only the cloaked frigate and two visible corvettes form the enemy fleet");
        content->victory = {};
    }
    std::vector<tactical::PlayerCommand> input;
    for (std::size_t index = 0; index < cloakers.size(); ++index)
        input.push_back({{1, 1, index}, {cloakers[index]}, tactical::AbilityPayload{tactical::AbilityKind::stealth,
            tactical::AbilityAction::activate}});
    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, ticks, input);
        if (!result) return 1;
        runs.push_back(std::move(*result));
    }
    for (std::size_t index = 1; index < runs.size(); ++index) {
        expect(runs[index].hashes == runs.front().hashes, "WST-16: the cloaked-enemy battle hashes alike at every worker count");
        expect(runs[index].commands == runs.front().commands, "WST-16: the cloaked-enemy battle's script commands match");
    }
    const auto& first = runs.front();
    // The Vengeance's 60 s cloak holds from tick 1 while it neither fires nor meets a detector.
    const std::uint64_t cloak_end = 1 + 60 * 30;
    const auto hits_on = [&](const bool cloaked) {
        return std::count_if(first.ai_hit_targets.begin(), first.ai_hit_targets.end(), [&](const auto& hit) {
            return (std::find(cloakers.begin(), cloakers.end(), hit.second) != cloakers.end()) == cloaked && hit.first < cloak_end;
        });
    };
    const auto accepted_while_cloaked = std::count_if(first.ai_accepted.begin(), first.ai_accepted.end(),
        [&](const auto tick) { return tick > 1 && tick < cloak_end; });
    std::set<std::uint64_t> refusal_ticks(first.hidden_refusals.begin(), first.hidden_refusals.end());
    std::cout << "cloaked-enemy battle: " << ticks << " ticks, " << first.hidden_refusals.size()
              << " attack order(s) refused on the hidden Vengeance in " << refusal_ticks.size() << " tick(s), "
              << accepted_while_cloaked << " accepted AI order(s) while it was cloaked, " << hits_on(false)
              << " AI hit(s) on visible units and " << hits_on(true) << " on the cloaked Vengeance before tick " << cloak_end << '\n';
    for (const auto& contact : first.cloak_contacts) std::cout << "cloaked-enemy contact: " << contact << '\n';
    for (const auto& diagnostic : first.diagnostics) std::cout << "cloaked-enemy diagnostic: " << diagnostic << '\n';
    expect(!first.hidden_refusals.empty(), "WST-16: the AI tried the hidden Vengeance (its attack orders on it were refused)");
    expect(hits_on(true) == 0, "WST-11: the AI never hits the Vengeance while its cloak hides it");
    expect(hits_on(false) > 0, "WST-16: the AI moves on and engages the visible corvettes");
    expect(accepted_while_cloaked > 0, "WST-16: the AI keeps issuing accepted orders while the Vengeance is cloaked");
    expect(refusal_ticks.size() * 30 < std::min<std::uint64_t>(ticks, cloak_end),
        "WST-16: refused attack orders on the hidden frigate do not repeat every frame (under one per second)");
    auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities, content->economy, content->content.map_box);
    expect(replayed && replayed.value() == first.world_hashes, "WST-16: the cloaked-enemy battle replays without the scripts");
    if (failures != 0) {
        std::cerr << failures << " cloaked-enemy check(s) failed\n";
        return 1;
    }
    std::cout << "cloaked-enemy battle passed\n";
    return 0;
}

// WLD-10: an AI battle with a Crusader present. The AI switches its point defence on through the engine's
// stand-in for the Crusader's object script when an enemy's weapons fire missiles, the request reaches the
// world as an ability command, the Crusader shoots shots down, and the battle hashes alike on 1, 2, 4 and 8
// workers and replays without the script.
void crusader_probe(const std::filesystem::path& root) {
    auto content = load(root, false, false, {}, true);
    if (!content) return;
    const auto crusader_type = skirmish::type_id("Crusader_Gunship");
    expect(std::any_of(content->start.units.begin(), content->start.units.end(), [&](const auto& unit) {
        return unit.state.type_id == crusader_type && unit.state.owner == 2;
    }), "WLD-10: the AI player's start carries a Crusader");
    // Staged so the battle cannot depend on how far the AI's fleet gets: the Crusader starts within the torpedo
    // bombers' reach (the Rebel player is idle), so its torpedoes come at it from the first seconds.
    {
        const auto bomber_name = [&](const tactical::UnitState& unit) {
            const auto name = content->type_names.find(unit.type_id);
            return name == content->type_names.end() ? std::string() : name->second;
        };
        const auto bomber = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [&](const auto& unit) { return unit.owner == 1 && bomber_name(unit).starts_with("Y-Wing"); });
        expect(bomber != content->start.setup.units.end(), "WLD-10: the Rebel player's start carries torpedo bombers");
        if (bomber == content->start.setup.units.end()) return;
        auto target = bomber->position;
        target.x = eawr::sim::math::Fixed::from_integer(target.x.trunc_to_integer() + 450).value();
        const auto move = [&](tactical::UnitState& unit) {
            if (unit.owner == 2 && unit.type_id == crusader_type) unit.position = target;
        };
        for (auto& unit : content->start.setup.units) move(unit);
        for (auto& unit : content->start.units) move(unit.state);
    }
    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, 1800);
        if (!result) return;
        runs.push_back(std::move(*result));
    }
    for (std::size_t index = 1; index < runs.size(); ++index) {
        expect(runs[index].hashes == runs.front().hashes, "WLD-10: the Crusader battle hashes alike at every worker count");
        expect(runs[index].commands == runs.front().commands, "WLD-10: the Crusader battle's script commands match");
    }
    std::size_t requests = 0;
    for (const auto& command : runs.front().record.commands) {
        if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
            requests += ability->ability == tactical::AbilityKind::laser_defense
                && ability->action == tactical::AbilityAction::activate ? 1 : 0;
        }
    }
    std::cout << "crusader battle: " << requests << " LASER_DEFENSE request(s), " << runs.front().point_defence_zaps
              << " shot(s) shot down, " << runs.front().crusader_hits << " hit(s) on the Crusader, "
              << runs.front().ai_hits << " AI hit(s)\n";
    for (const auto& [shooter, count] : runs.front().crusader_hit_by)
        std::cout << "crusader hit by " << shooter << ": " << count << '\n';
    for (const auto& diagnostic : runs.front().diagnostics) std::cout << "crusader diagnostic: " << diagnostic << '\n';
    // The object script reacts to its deadly enemy, which the threat table names once the Crusader has been
    // hit: the torpedoes hit it, its AI switches the point defence on through the engine's stand-in, the
    // request reaches the world as an accepted ability command and the Crusader then shoots torpedoes down.
    expect(runs.front().crusader_hits > 0, "WLD-10: the Rebel torpedo bombers attack the AI's Crusader");
    expect(requests > 0, "WLD-10: a hit Crusader's AI switches its laser defence on");
    expect(runs.front().point_defence_zaps > 0, "WLD-05: a switched-on Crusader shoots down incoming torpedoes");
    auto replayed = eawr::platform::headless_tick_hashes(runs.front().record, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities,
        content->economy, content->content.map_box);
    expect(replayed && replayed.value() == runs.front().world_hashes, "WLD-10: the Crusader battle replays without the script");
}

} // namespace

int serial_profile(const std::filesystem::path& root, const std::uint64_t ticks,
    const std::vector<std::size_t>& worker_counts) {
    struct QueryReporting {
        QueryReporting() {
            foc::ai::ThreatGrid::observe_queries([](const std::uint64_t tick, const std::size_t keys,
                const foc::ai::ThreatGrid::QueryCounts& counts) noexcept {
                std::cout << "THREAT-COUNTS," << tick << ',' << counts.prepared_keys << ',' << keys << ','
                    << counts.queries << ',' << counts.warm << ',' << counts.cold << ',' << counts.repeats << ','
                    << counts.cross_player << ',' << counts.scalar_hits << '\n';
            });
        }
        ~QueryReporting() { foc::ai::ThreatGrid::observe_queries(nullptr); }
    } query_reporting;
    auto content = load(root, false, false, {}, false, {}, {}, {}, false, true);
    if (!content) return 1;
    std::vector<std::string> reference;
    for (const std::size_t workers : worker_counts) {
        eawr::platform::LiveSession::Options options;
        options.workers = workers;
        options.pacing = eawr::platform::LiveSession::Pacing::driven;
        options.measure_phases = true;
        auto scripts = std::make_shared<eawr::platform::LiveScripts>();
        scripts->wrap = [&content](tactical::TacticalSession world) {
            return foc::create_session(std::move(world), content->ai, content->modules);
        };
        options.scripts = scripts;
        auto live = eawr::platform::LiveSession::start(content->start.setup, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, options,
            content->victory, content->content.fog, content->content.abilities, content->economy,
            content->content.map_box);
        expect(static_cast<bool>(live), "stock serial profile starts");
        if (!live) return 1;
        std::map<std::string, double> totals;
        std::uint64_t samples = 0;
        for (std::uint64_t begin = 0; begin < ticks; begin += 256) {
            const auto end = std::min(ticks, begin + 256);
            live.value()->advance_to(end);
            expect(live.value()->wait_for(end, std::chrono::minutes(10)), "stock serial profile reaches its batch");
            if (live.value()->failure()) { live.value()->stop(); return 1; }
            const auto costs = live.value()->tick_costs_after(begin);
            expect(costs.size() == end - begin, "serial profile retains every measured tick");
            for (const auto& cost : costs) {
                if (cost.tick <= 150) continue;
                ++samples;
                totals["total"] += cost.total_ms;
                for (const auto& phase : cost.phases) totals[phase.name] += phase.ms;
            }
        }
        live.value()->stop();
        const auto hashes = live.value()->tick_hashes();
        expect(hashes.size() == ticks, "stock serial profile retains every world hash");
        const auto recording = live.value()->record();
        expect(!recording.commands.empty(), "stock serial profile observes active AI commands");
        if (reference.empty()) {
            reference = hashes;
            for (std::size_t index = 0; index < hashes.size(); ++index)
                std::cout << "SERIAL-HASH," << index + 1 << ',' << hashes[index] << '\n';
        } else expect(hashes == reference, "stock 3v3 world hashes are byte-equal on 1/2/4/6/8 workers");
        std::cout << "SERIAL-IDENTITY," << workers << ',' << ticks << ',' << samples << ','
            << recording.commands.size() << ',' << content->start.units.size() << ','
            << (hashes.empty() ? std::string{} : hashes.back()) << '\n';
        std::cout.precision(12);
        for (const auto& [name, total] : totals)
            std::cout << "SERIAL-PHASE," << workers << ',' << name << ','
                << (samples ? total / static_cast<double>(samples) : 0.0) << '\n';
    }
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the FoC AI battle\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--serial-profile") {
        std::vector<std::size_t> workers{1, 6, 2, 4, 8};
        if (argc > 3) {
            workers.clear();
            for (int index = 3; index < argc; ++index)
                workers.push_back(static_cast<std::size_t>(std::strtoull(argv[index], nullptr, 10)));
        }
        return serial_profile(*root, argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5400, workers);
    }
    if (argc > 1 && std::string_view(argv[1]) == "--passive-contact-control")
        return passive_contact_control(*root, argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 7200);
    if (argc > 1 && std::string_view(argv[1]) == "--squadron-idle-trace") {
        const auto ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 3600;
        auto content = load(*root, false, false, {}, false, 68, {}, {}, true);
        if (!content) return 1;
        std::ostringstream rows;
        const auto result = run(*content, 1, ticks, {}, false, false, &rows);
        if (!result) return 1;
        const std::string path = argc > 3 ? argv[3] : "squadron-idle-trace.csv";
        std::ofstream csv(path);
        csv << rows.str();
        const auto replay = tactical::write_replay(result->record);
        expect(static_cast<bool>(csv) && static_cast<bool>(replay), "stock squadron trace and command recording serialize");
        if (replay) {
            std::ofstream recorded(path + ".eawr-replay", std::ios::binary);
            recorded.write(reinterpret_cast<const char*>(replay.value().data()), static_cast<std::streamsize>(replay.value().size()));
            expect(static_cast<bool>(recorded), "stock 3v3 command recording writes");
        }
        std::cout << "SQ-TRACE-BEGIN\n" << rows.str() << "SQ-TRACE-END\n";
        std::cout << "Stock 3v3 diagnostic: ticks=" << ticks << " commands=" << result->commands.size()
            << " script_diagnostics=" << result->diagnostics.size() << '\n';
        expect(!result->commands.empty(), "stock 3v3 diagnostic observes active AI commands");
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--stock-economy-witness") {
        if (argc != 2) return 1;
        return stock_economy_witness(*root);
    }
    const bool stock = argc > 1 && std::string_view(argv[1]) == "--underworld-stock";
    if (argc > 1 && (std::string_view(argv[1]) == "--underworld" || stock)) {
        // WAB-70..76: the Underworld AI fires buzz droids from its StarVipers; the world,
        // the script commands and a headless replay of the record agree on 1/2/4/8 workers.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 3600;
        auto content = load(*root, false, true);
        if (!content) return 1;
        // In this short M2 engagement the stock Underworld plans (move, scout, destroy-unit, area
        // sweep) reach no BUZZ_DROIDS call, so a stand-in freestore asks every StarViper squadron
        // for it through the stock Lua calls, as the ability probe does for POWER_TO_WEAPONS.
        // The stock mode keeps the authored plans: BUZZ_DROIDS then comes from their own handlers (EX-46).
        if (!stock) content->ai.xml.clear();
        if (!stock) {
        content->ai.freestore_module = "Data/Scripts/Test/BuzzDroidsProbe.lua";
        content->modules[content->ai.freestore_module] =
            "function Base_Definitions()\n"
            "  ServiceRate = 1\n"
            "  UnitServiceRate = 1\n"
            "end\n"
            "function main()\n"
            "  FogOfWar.Reveal_All(PlayerObject)\n"
            "end\n"
            "function On_Unit_Service(object)\n"
            "  enemy = Find_Nearest(object, PlayerObject, false)\n"
            "  if not enemy then return end\n"
            "  if object.Has_Ability(\"BUZZ_DROIDS\") and object.Is_Ability_Ready(\"BUZZ_DROIDS\")\n"
            "      and object.Get_Distance(enemy) < 350 then\n"
            "    object.Activate_Ability(\"BUZZ_DROIDS\", enemy)\n"
            "  elseif not object.Has_Attack_Target() then\n"
            "    object.Attack_Target(enemy)\n"
            "  end\n"
            "end\n";
        }
        if (argc > 3 && !stock) return ability_trace(*content, ticks, argv[3]); // plan and ability journal
        std::vector<Run> runs;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const Run& first = runs.front();
        for (std::size_t index = 1; index < runs.size(); ++index) {
            expect(runs[index].hashes == first.hashes, "WAB-72: Underworld battle hashes match worker count 1");
            expect(runs[index].commands == first.commands, "WAB-76: Underworld script commands match worker count 1");
        }
        std::size_t buzz = 0;
        for (const auto& command : first.record.commands) {
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload))
                buzz += ability->ability == tactical::AbilityKind::buzz_droids ? 1 : 0;
        }
        std::cout << "Underworld battle: " << ticks << " ticks, " << buzz << " BUZZ_DROIDS command(s), "
                  << first.droids.size() << " droid object(s), AI hits " << first.ai_hits << '\n';
        std::cout << "AI orders by unit type:";
        for (const auto& [type, count] : first.orders_by_type) std::cout << ' ' << type << '=' << count;
        std::cout << '\n';
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        expect(buzz > 0, "WAB-76: the Lua Activate_Ability reaches the world as a BUZZ_DROIDS point command");
        if (stock) {
            std::cout << "BUZZ_DROIDS script requests (stock plans only): " << first.plan_buzz << '\n';
            expect(first.plan_buzz > 0, "EX-46: the stock plans' Target_In_Range handler activates BUZZ_DROIDS for a squadron");
        }
        expect(!first.droids.empty(), "WAB-71: an AI activation creates a droid object");
        expect(!first.orders_by_type.contains("Buzz_Droids"), "WAB-71: droids have no locomotor and stay out of the freestore");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WAB-70: the Underworld record replays without the script");
        if (failures != 0) return 1;
        std::cout << "Underworld buzz-droid battle passed\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--cloaked") {
        return cloaked_enemy_probe(*root, argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2400);
    }
    if (argc > 1 && (std::string_view(argv[1]) == "--keldabe" || std::string_view(argv[1]) == "--keldabe-autofire")) {
        // WAB-36: --keldabe-autofire keeps the stock plans; the Keldabe's autofire alone must lock its target.
        const bool autofire = std::string_view(argv[1]) == "--keldabe-autofire";
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1800;
        auto content = load(*root, false, true, {"Kedalbe_Battleship"});
        if (!content) return 1;
        // WLS-04: stock Lua makes no shield-leech call. A fixture freestore exercises the targeted API.
        // Start within the nested beam range: ordinary weapons can otherwise stop the approach farther away.
        // The conservative readiness witness needs a shielded ship target whose centre
        // is fully in range. The nearby starbase has a larger footprint and can admit
        // a lock before that conservative bound, so isolate the autofire fixture's ships.
        if (autofire) {
            const auto keldabe = skirmish::type_id("Kedalbe_Battleship");
            const auto corvette = skirmish::type_id("Corellian_Corvette");
            const auto retained = [&](const auto& unit) { return unit.type_id == keldabe || unit.type_id == corvette; };
            std::erase_if(content->start.setup.units, [&](const auto& unit) { return !retained(unit); });
            std::erase_if(content->start.units, [&](const auto& unit) { return !retained(unit.state); });
            content->start.setup.squadrons.clear();
            content->start.setup.free_garrisons.clear();
            content->victory = {};
        }
        const auto enemy = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(), [](const auto& unit) {
            return unit.owner == 1 && unit.type_id == skirmish::type_id("Corellian_Corvette");
        });
        expect(enemy != content->start.setup.units.end(), "WLS-04: the installed battle contains a shielded Corvette");
        if (enemy == content->start.setup.units.end()) return 1;
        auto position = enemy->position;
        position.x = eawr::sim::math::Fixed::from_raw(position.x.raw() + 800 * eawr::sim::math::Fixed::scale);
        for (auto& unit : content->start.setup.units)
            if (unit.type_id == skirmish::type_id("Kedalbe_Battleship")) unit.position = position;
        for (auto& unit : content->start.units)
            if (unit.state.type_id == skirmish::type_id("Kedalbe_Battleship")) unit.state.position = position;
        if (!autofire) {
            content->ai.xml.clear();
            content->ai.freestore_module = "Data/Scripts/Test/ShieldLeechProbe.lua";
            content->modules[content->ai.freestore_module] =
                "function Base_Definitions() ServiceRate = 1; UnitServiceRate = 1 end\n"
                "function main() FogOfWar.Reveal_All(PlayerObject) end\n"
                "function On_Unit_Service(object)\n"
                "  enemy = Find_Nearest(object, 'Corellian_Corvette', PlayerObject, false)\n"
                "  if not enemy then return end\n"
                "  if object.Has_Ability('LEECH_SHIELDS') and object.Is_Ability_Ready('LEECH_SHIELDS')\n"
                "    and not object.Is_Ability_Active('LEECH_SHIELDS') and object.Get_Distance(enemy) < 1800\n"
                "    and enemy.Get_Shield() > 0 then\n"
                "    object.Activate_Ability('LEECH_SHIELDS', enemy)\n"
                "  end\n"
                "  if not object.Has_Attack_Target() then object.Attack_Target(enemy) end\n"
                "end\n";
        }
        std::vector<Run> runs;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto result = run(*content, workers, ticks, {}, false, true);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const auto& first = runs.front();
        for (std::size_t i = 1; i < runs.size(); ++i) {
            expect(runs[i].hashes == first.hashes, "WLS-04: Keldabe battle hashes match on 1/2/4/8 workers");
            expect(runs[i].commands == first.commands, "WLS-04: targeted Lua commands match on 1/2/4/8 workers");
        }
        const auto requests = std::count_if(first.record.commands.begin(), first.record.commands.end(), [](const auto& command) {
            const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload);
            return ability && ability->ability == tactical::AbilityKind::leech_shields && ability->target != 0;
        });
        if (autofire) {
            expect(requests == 0, "WAB-36: the stock plans make no shield-leech request");
            expect(first.shield_leech_frames > 0, "WAB-36: the AI Keldabe autofires its shield leech under the stock plans");
            expect(first.first_leech_lock && first.first_leech_ready_target && *first.first_leech_lock <= *first.first_leech_ready_target,
                "WAB-36: installed leech locks no later than its first ready in-range attack target");
            if (first.first_leech_lock && first.first_leech_ready_target) std::cout << "leech first lock tick " << *first.first_leech_lock
                << ", conservative ready target tick " << *first.first_leech_ready_target << '\n';
        } else {
            expect(requests > 0, "WLS-04: targeted Lua activation reaches the Keldabe world");
            expect(first.shield_leech_frames > 0, "WLS-04: the installed Keldabe accepts and services a targeted lock");
        }
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WLS-04: Keldabe battle replays without scripts");
        std::cout << (autofire ? "Keldabe stock-plan battle: " : "Keldabe battle: ") << requests << " targeted shield-leech commands, " << first.shield_leech_frames
                  << " active lock frames, AI hits " << first.ai_hits << '\n';
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--concentrate") {
        // WAB-36, WHE-24 (legacy #2997): no stock skirmish script asks for CONCENTRATE_FIRE. The AI
        // Home One's autofire alone switches it on under the stock plans, and its escorts, recruited
        // at activation, fire on the concentrated target.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1800;
        auto content = load(*root, true, false, {}, false, std::nullopt, {},
            {"Home_One", "Nebulon_B_Frigate", "Corellian_Corvette"});
        if (!content) return 1;
        const auto home_one = skirmish::type_id("Home_One");
        const std::set<tactical::TypeId> kept{home_one, skirmish::type_id("Nebulon_B_Frigate"),
            skirmish::type_id("Corellian_Corvette"), skirmish::type_id("Tartan_Patrol_Cruiser"),
            skirmish::type_id("Acclamator_Assault_Ship")};
        // Only the two fleets: stations, fighters and map objects would be other attack targets.
        std::erase_if(content->start.setup.units, [&](const auto& unit) { return !kept.contains(unit.type_id); });
        std::erase_if(content->start.units, [&](const auto& unit) { return !kept.contains(unit.state.type_id); });
        content->start.setup.squadrons.clear();
        content->start.setup.free_garrisons.clear();
        content->victory = {};
        const auto enemy = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(), [](const auto& unit) {
            return unit.owner == 1 && unit.type_id == skirmish::type_id("Tartan_Patrol_Cruiser");
        });
        expect(enemy != content->start.setup.units.end(), "WAB-36: the installed battle contains an Empire Tartan");
        if (enemy == content->start.setup.units.end()) return 1;
        // The Rebel group starts within weapon reach of the Empire group, Home One first in line.
        const auto anchor = enemy->position;
        std::int64_t offset = 800;
        std::map<eawr::sim::EntityId, eawr::sim::math::Vec3> placed;
        for (auto& unit : content->start.setup.units) {
            if (unit.owner != 2) continue;
            if (unit.type_id != home_one) offset += 200;
            const std::int64_t distance = unit.type_id == home_one ? 800 : offset;
            auto position = anchor;
            position.x = eawr::sim::math::Fixed::from_raw(anchor.x.raw() + distance * eawr::sim::math::Fixed::scale);
            unit.position = position;
            placed.emplace(unit.entity_id, position);
        }
        for (auto& unit : content->start.units)
            if (const auto found = placed.find(unit.state.entity_id); found != placed.end()) unit.state.position = found->second;
        std::vector<Run> runs;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const auto& first = runs.front();
        for (std::size_t i = 1; i < runs.size(); ++i) {
            expect(runs[i].hashes == first.hashes, "WAB-36: Home One battle hashes match on 1/2/4/8 workers");
            expect(runs[i].commands == first.commands, "WAB-36: Home One battle commands match on 1/2/4/8 workers");
        }
        const auto requests = std::count_if(first.record.commands.begin(), first.record.commands.end(), [](const auto& command) {
            const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload);
            return ability && ability->ability == tactical::AbilityKind::concentrate_fire;
        });
        expect(requests == 0, "WAB-36: the stock plans make no concentrate-fire request");
        expect(first.concentrate_frames > 0, "WAB-36: the AI Home One autofires CONCENTRATE_FIRE under the stock plans");
        expect(first.recruit_hits > 0, "WHE-24: the recruited escorts fire on the concentrated target");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WAB-36: Home One battle replays without scripts");
        std::cout << "Home One stock-plan battle: " << requests << " concentrate commands, " << first.concentrate_frames
                  << " concentrate frames, " << first.recruit_hits << " hits on the target by other shooters\n";
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--tractor") {
        // WAB-36 (legacy #2997): no stock skirmish script asks for TRACTOR_BEAM. The AI Star Destroyer's
        // autofire alone locks it onto its attack target under the stock plans.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1800;
        auto content = load(*root, false, false, {}, false, std::nullopt, {}, {"Star_Destroyer"});
        if (!content) return 1;
        const auto enemy = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(), [](const auto& unit) {
            return unit.owner == 1 && unit.type_id == skirmish::type_id("Corellian_Corvette");
        });
        expect(enemy != content->start.setup.units.end(), "WAB-36: the installed battle contains a Rebel Corvette");
        if (enemy == content->start.setup.units.end()) return 1;
        // Start inside the tractor's 1000 range of a corvette, as the Keldabe battle does for its beam.
        auto position = enemy->position;
        position.x = eawr::sim::math::Fixed::from_raw(position.x.raw() + 800 * eawr::sim::math::Fixed::scale);
        const auto destroyer = skirmish::type_id("Star_Destroyer");
        const auto corvette = skirmish::type_id("Corellian_Corvette");
        // Only the destroyer and the corvettes: the Rebel station and fighters are nearer attack targets
        // that the tractor's category filter (Transport, Corvette, Frigate) refuses, as in the IG-2000 battle.
        const auto retained = [&](const auto& unit) { return unit.type_id == destroyer || unit.type_id == corvette; };
        std::erase_if(content->start.setup.units, [&](const auto& unit) { return !retained(unit); });
        std::erase_if(content->start.units, [&](const auto& unit) { return !retained(unit.state); });
        content->start.setup.squadrons.clear();
        content->start.setup.free_garrisons.clear();
        content->victory = {};
        for (auto& unit : content->start.setup.units) if (unit.type_id == destroyer) unit.position = position;
        for (auto& unit : content->start.units) if (unit.state.type_id == destroyer) unit.state.position = position;
        std::vector<Run> runs;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto result = run(*content, workers, ticks, {}, false, true);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const auto& first = runs.front();
        for (std::size_t i = 1; i < runs.size(); ++i) {
            expect(runs[i].hashes == first.hashes, "WAB-36: Star Destroyer battle hashes match on 1/2/4/8 workers");
            expect(runs[i].commands == first.commands, "WAB-36: Star Destroyer battle commands match on 1/2/4/8 workers");
        }
        const auto requests = std::count_if(first.record.commands.begin(), first.record.commands.end(), [](const auto& command) {
            const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload);
            return ability && ability->ability == tactical::AbilityKind::tractor_beam;
        });
        expect(requests == 0, "WAB-36: the stock plans make no tractor-beam request");
        expect(first.tractor_frames > 0, "WAB-36: the AI Star Destroyer autofires its tractor beam under the stock plans");
        expect(first.first_tractor_lock && first.first_tractor_ready_target && *first.first_tractor_lock <= *first.first_tractor_ready_target,
            "WAB-36: installed tractor locks no later than its first ready in-range attack target");
        if (first.first_tractor_lock && first.first_tractor_ready_target) std::cout << "tractor first lock tick " << *first.first_tractor_lock
            << ", conservative ready target tick " << *first.first_tractor_ready_target << '\n';
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WAB-36: Star Destroyer battle replays without scripts");
        std::cout << "Star Destroyer stock-plan battle: " << requests << " tractor commands, " << first.tractor_frames
                  << " tractor lock frames, AI hits " << first.ai_hits << '\n';
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--ig2000") {
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 450;
        auto content = load(*root, false, true, {"IG-2000"});
        if (!content) return 1;
        // Exercise the authored object script in a close engagement with the full production
        // AI host and installed unit/weapon data.
        const auto hero = skirmish::type_id("IG-2000"), target = skirmish::type_id("Nebulon_B_Frigate");
        const auto ai_type = [&](const eawr::sim::tactical::TypeId id) -> const eawr::script::foc::AiType* {
            const auto& types = content->ai.content.types;
            const auto found = std::find_if(types.begin(), types.end(), [&](const auto& type) { return type.type_id == id; });
            return found == types.end() ? nullptr : &*found;
        };
        const auto* hero_type = ai_type(hero);
        const auto* target_type = ai_type(target);
        expect(hero_type && hero_type->hero && target_type && !target_type->hero,
            "WTA-36: the AI's Is_Hero answers true for the installed IG-2000 and false for a Nebulon-B");
        const auto retained = [&](const auto& unit) { return unit.type_id == hero || unit.type_id == target; };
        std::erase_if(content->start.setup.units, [&](const auto& unit) { return !retained(unit); });
        std::erase_if(content->start.units, [&](const auto& unit) { return !retained(unit.state); });
        content->start.setup.squadrons.clear();
        // FL-14: the removed stations' registered garrisons do not belong to this fixture.
        content->start.setup.free_garrisons.clear();
        for (auto& unit : content->start.setup.units)
            unit.position = {eawr::sim::math::Fixed::from_integer(unit.type_id == hero ? 0 : 200).value(), {}, {}};
        for (auto& unit : content->start.units)
            unit.state.position = {eawr::sim::math::Fixed::from_integer(unit.state.type_id == hero ? 0 : 200).value(), {}, {}};
        content->victory = {};
        std::vector<Run> runs;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const Run& first = runs.front();
        for (std::size_t index = 1; index < runs.size(); ++index) {
            expect(runs[index].hashes == first.hashes, "WHE-67: IG-2000 battle hashes match on 1/2/4/8 workers");
            expect(runs[index].commands == first.commands, "WHE-67: IG-2000 commands match on 1/2/4/8 workers");
        }
        std::size_t requests = 0;
        for (const auto& command : first.record.commands)
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload))
                requests += ability->ability == tactical::AbilityKind::corrupt_systems && ability->position ? 1 : 0;
        expect(requests > 0 && !first.corrupted.empty(), "WHE-67: the production object script casts and corrupts the enemy");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WHE-67: the IG-2000 record replays without AI");
        std::cout << "IG-2000 battle: " << requests << " cast requests, " << first.corrupted.size() << " corrupted enemies\n";
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--interceptor") {
        // WAB-80..83: the Underworld AI switches FULL_SALVO on for its Interceptor IV frigates; the world, the
        // script commands and a headless replay of the record agree on 1/2/4/8 workers.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 3600;
        auto content = load(*root, false, true, {"Interceptor4_Frigate", "Interceptor4_Frigate"});
        if (!content) return 1;
        // WAB-83: isolate the ability call from station-attachment approach geometry (OR-07).
        // A ship can hit an attached station weapon while staying beyond the stand-in's
        // body-distance trigger. Stage only the Interceptors and two corvettes inside that
        // trigger, preserving their relative formation, before the first Lua service.
        const auto interceptor = skirmish::type_id("Interceptor4_Frigate");
        const auto corvette = skirmish::type_id("Corellian_Corvette");
        const auto enemy = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [&](const auto& unit) { return unit.owner == 1 && unit.type_id == corvette; });
        const auto holder = std::find_if(content->start.setup.units.begin(), content->start.setup.units.end(),
            [&](const auto& unit) { return unit.owner == 2 && unit.type_id == interceptor; });
        expect(enemy != content->start.setup.units.end() && holder != content->start.setup.units.end(),
            "WAB-83: the controlled encounter contains the installed ability holder and target");
        if (enemy == content->start.setup.units.end() || holder == content->start.setup.units.end()) return 1;
        const auto dx = enemy->position.x.raw() + 800 * eawr::sim::math::Fixed::scale - holder->position.x.raw();
        const auto dy = enemy->position.y.raw() - holder->position.y.raw();
        const auto dz = enemy->position.z.raw() - holder->position.z.raw();
        const auto retained = [&](const auto& unit) { return unit.type_id == interceptor || unit.type_id == corvette; };
        std::erase_if(content->start.setup.units, [&](const auto& unit) { return !retained(unit); });
        std::erase_if(content->start.units, [&](const auto& unit) { return !retained(unit.state); });
        content->start.setup.squadrons.clear();
        content->start.setup.free_garrisons.clear();
        content->victory = {};
        const auto place = [&](tactical::UnitState& unit) {
            if (unit.type_id != interceptor) return;
            unit.position.x = eawr::sim::math::Fixed::from_raw(unit.position.x.raw() + dx);
            unit.position.y = eawr::sim::math::Fixed::from_raw(unit.position.y.raw() + dy);
            unit.position.z = eawr::sim::math::Fixed::from_raw(unit.position.z.raw() + dz);
        };
        for (auto& unit : content->start.setup.units) place(unit);
        for (auto& unit : content->start.units) place(unit.state);
        // The stand-in makes the stock handler's Has_Ability / readiness / active / activation calls.
        content->ai.xml.clear();
        content->ai.freestore_module = "Data/Scripts/Test/FullSalvoProbe.lua";
        content->modules[content->ai.freestore_module] =
            "function Base_Definitions()\n"
            "  ServiceRate = 1\n"
            "  UnitServiceRate = 1\n"
            "end\n"
            "function main()\n"
            "  FogOfWar.Reveal_All(PlayerObject)\n"
            "end\n"
            "function On_Unit_Service(object)\n"
            "  enemy = Find_Nearest(object, PlayerObject, false)\n"
            "  if not enemy then return end\n"
            "  if object.Has_Ability(\"FULL_SALVO\") and object.Is_Ability_Ready(\"FULL_SALVO\")\n"
            "      and not object.Is_Ability_Active(\"FULL_SALVO\") and object.Get_Distance(enemy) < 1200 then\n"
            "    object.Activate_Ability(\"FULL_SALVO\", true)\n"
            "  end\n"
            "  if not object.Has_Attack_Target() then\n"
            "    object.Attack_Target(enemy)\n"
            "  end\n"
            "end\n";
        std::vector<Run> runs;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const Run& first = runs.front();
        for (std::size_t index = 1; index < runs.size(); ++index) {
            expect(runs[index].hashes == first.hashes, "WAB-80: Interceptor battle hashes match worker count 1");
            expect(runs[index].commands == first.commands, "WAB-83: Interceptor script commands match worker count 1");
        }
        std::size_t requests = 0;
        for (const auto& command : first.record.commands) {
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
                requests += ability->ability == tactical::AbilityKind::full_salvo
                    && ability->action == tactical::AbilityAction::activate ? 1 : 0;
            }
        }
        std::cout << "Interceptor battle: " << ticks << " ticks, " << requests << " FULL_SALVO request(s), AI hits "
                  << first.ai_hits << "; ready holder frames " << first.salvo_ready_ticks
                  << ", active holder frames " << first.salvo_active_ticks << ", closest player gap while ready " << first.salvo_min_gap << '\n';
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        expect(requests > 0, "WAB-83: the Lua Activate_Ability reaches the world as a FULL_SALVO activation");
        expect(first.salvo_active_ticks > 0, "WAB-80: the installed FULL_SALVO request activates the world ability");
        expect(first.ai_hits > 0, "WAB-81: the Interceptors' weapons hit the player");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WAB-80: the Interceptor record replays without the script");
        if (failures != 0) return 1;
        std::cout << "Interceptor full-salvo battle passed\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--awing") {
        // WLU-10: the Underworld AI's A-wings (the A-Wing variant keeps LURE). The stock escort plan's
        // Try_Ability(.., "LURE") is counted from a stock run; a stand-in freestore then makes the Lua calls
        // that switch LURE on once an X-wing (a listed target type) is inside the radius. The world, the
        // script commands and a headless replay of the record agree on 1/2/4/8 workers.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 3600;
        auto content = load(*root, false, true, {"Underworld_A_Wing_Squadron", "Underworld_A_Wing_Squadron"});
        if (!content) return 1;
        const auto stock_run = run(*content, 1, ticks);
        if (!stock_run) return 1;
        std::cout << "A-wing stock battle: " << ticks << " ticks, " << stock_run->plan_lure << " LURE script request(s), "
                  << stock_run->lure_frames << " luring craft frame(s)\n";
        content->ai.xml.clear();
        content->ai.freestore_module = "Data/Scripts/Test/LureProbe.lua";
        content->modules[content->ai.freestore_module] =
            "function Base_Definitions()\n"
            "  ServiceRate = 1\n"
            "  UnitServiceRate = 1\n"
            "end\n"
            "function main()\n"
            "  FogOfWar.Reveal_All(PlayerObject)\n"
            "end\n"
            "function On_Unit_Service(object)\n"
            "  enemy = Find_Nearest(object, \"Rebel_X-Wing_Squadron\", PlayerObject, false)\n"
            "  if not enemy then enemy = Find_Nearest(object, PlayerObject, false) end\n"
            "  if not enemy then return end\n"
            "  if object.Has_Ability(\"LURE\") and object.Is_Ability_Ready(\"LURE\")\n"
            "      and object.Get_Distance(enemy) < 350 then\n"
            "    object.Activate_Ability(\"LURE\", true)\n"
            "  elseif not object.Has_Attack_Target() then\n"
            "    object.Attack_Target(enemy)\n"
            "  end\n"
            "end\n";
        std::vector<Run> runs;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const Run& first = runs.front();
        for (std::size_t index = 1; index < runs.size(); ++index) {
            expect(runs[index].hashes == first.hashes, "WLU-10: A-wing battle hashes match worker count 1");
            expect(runs[index].commands == first.commands, "WLU-10: A-wing script commands match worker count 1");
        }
        std::size_t requests = 0;
        for (const auto& command : first.record.commands) {
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
                requests += ability->ability == tactical::AbilityKind::lure
                    && ability->action == tactical::AbilityAction::activate ? 1 : 0;
            }
        }
        std::cout << "A-wing battle: " << ticks << " ticks, " << requests << " LURE request(s), " << first.lure_frames
                  << " luring craft frame(s), AI hits " << first.ai_hits << '\n';
        for (const auto& [name, count] : first.orders_by_type) std::cout << "  accepted orders " << name << ": " << count << '\n';
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        expect(requests > 0, "WLU-10: the Lua Activate_Ability reaches the world as a LURE activation");
        expect(first.lure_frames > 0, "WLU-02: an AI activation switches the craft's LURE on");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WLU-10: the A-wing record replays without the script");
        if (failures != 0) return 1;
        std::cout << "A-wing lure battle passed\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--vengeance") {
        // WSD-01..07: the Underworld AI's Vengeance frigates; a stand-in freestore makes the Lua calls that switch
        // SELF_DESTRUCT on once an enemy is inside the blast (the stock library never names the ability, WSD-09).
        // The world, the script commands and a headless replay of the record agree on 1/2/4/8 workers; the
        // blast leaves the Vengeance dead and hurts the player.
        const std::uint64_t ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5400;
        auto content = load(*root, false, true, {"Vengeance_Frigate", "Vengeance_Frigate"});
        if (!content) return 1;
        content->ai.xml.clear();
        content->ai.freestore_module = "Data/Scripts/Test/SelfDestructProbe.lua";
        content->modules[content->ai.freestore_module] =
            "function Base_Definitions()\n"
            "  ServiceRate = 1\n"
            "  UnitServiceRate = 1\n"
            "end\n"
            "function main()\n"
            "  FogOfWar.Reveal_All(PlayerObject)\n"
            "end\n"
            "function On_Unit_Service(object)\n"
            "  enemy = Find_Nearest(object, \"Corellian_Corvette\", PlayerObject, false)\n"
            "  if not enemy then enemy = Find_Nearest(object, PlayerObject, false) end\n"
            "  if not enemy then return end\n"
            "  if object.Has_Ability(\"SELF_DESTRUCT\") and object.Is_Ability_Ready(\"SELF_DESTRUCT\")\n"
            "      and not object.Is_Ability_Active(\"SELF_DESTRUCT\") then\n"
            "    if object.Get_Distance(enemy) < 700 then\n"
            "      object.Activate_Ability(\"SELF_DESTRUCT\", true)\n"
            "    else\n"
            "      object.Move_To(enemy)\n"
            "    end\n"
            "  elseif not object.Has_Attack_Target() then\n"
            "    object.Attack_Target(enemy)\n"
            "  end\n"
            "end\n";
        std::vector<Run> runs;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            runs.push_back(std::move(*result));
        }
        const Run& first = runs.front();
        for (std::size_t index = 1; index < runs.size(); ++index) {
            expect(runs[index].hashes == first.hashes, "WSD-03: Vengeance battle hashes match worker count 1");
            expect(runs[index].commands == first.commands, "WSD-09: Vengeance script commands match worker count 1");
        }
        std::size_t requests = 0;
        for (const auto& command : first.record.commands) {
            if (const auto* ability = std::get_if<tactical::AbilityPayload>(&command.payload)) {
                requests += ability->ability == tactical::AbilityKind::self_destruct
                    && ability->action == tactical::AbilityAction::activate ? 1 : 0;
            }
        }
        std::size_t destroyed_ai = 0;
        for (const auto& event : first.events) {
            if (event.find(" unit_destroyed ") == std::string::npos) continue;
            const auto id = std::stoull(event.substr(event.rfind(' ') + 1));
            for (const auto& unit : content->start.units) destroyed_ai += unit.state.entity_id == id && unit.state.owner == 2 ? 1 : 0;
        }
        std::cout << "Vengeance battle: " << ticks << " ticks, " << requests << " SELF_DESTRUCT request(s), "
                  << first.blast_objects << " blast tick(s), player hull lost to blasts " << first.blast_hull_drop
                  << ", Underworld units destroyed " << destroyed_ai << '\n';
        std::cout << "  holder alive ticks " << first.sd_alive_first << ".." << first.sd_alive_last << ", ready ticks "
                  << first.sd_ready_ticks << ", closest approach " << first.sd_min_gap << ", " << first.commands.size()
                  << " routed command(s)\n";
        for (const auto& [name, count] : first.orders_by_type) std::cout << "  accepted orders " << name << ": " << count << '\n';
        std::size_t shown = 0;
        for (const auto& event : first.events) {
            if (event.find(" order_rejected ") == std::string::npos && event.find(" ability_") == std::string::npos) continue;
            if (shown++ < 6) std::cout << "  event " << event << '\n';
        }
        for (const auto& command : first.commands) {
            if (command.rfind("ability", 0) == 0 || command.find("dropped") != std::string::npos) std::cout << "  command " << command << '\n';
        }
        for (const auto& diagnostic : first.diagnostics) std::cout << "  " << diagnostic << '\n';
        expect(requests > 0, "WSD-09: the Lua Activate_Ability reaches the world as a SELF_DESTRUCT activation");
        expect(first.blast_objects > 0 && destroyed_ai > 0, "WSD-05: the countdown ends the Vengeance with its blast object");
        expect(first.blast_hull_drop >= 400, "WSD-06: the blast hurts the player's ships in reach");
        auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first.world_hashes, "WSD-03: the Vengeance record replays without the script");
        if (failures != 0) return 1;
        std::cout << "Vengeance self-destruct battle passed\n";
        return 0;
    }
    const bool source_free_trace = argc > 1 && std::string_view(argv[1]) == "--combat-trace-source-free";
    const bool ordinary_plans_trace = argc > 1 && std::string_view(argv[1]) == "--combat-trace-ordinary-plans";
    const bool economy_disabled_trace = argc > 1 && std::string_view(argv[1]) == "--combat-engagement-trace";
    const bool stock_engagement_trace = argc > 1 && std::string_view(argv[1]) == "--stock-engagement-trace";
    const bool engagement_trace = economy_disabled_trace || stock_engagement_trace;
    const bool combat_tracing = source_free_trace || ordinary_plans_trace || engagement_trace
        || (argc > 1 && std::string_view(argv[1]) == "--combat-trace");
    if (combat_tracing) {
        const auto ticks = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 600;
        const std::uint64_t battle_seed = argc > 4 ? std::strtoull(argv[4], nullptr, 10)
            : engagement_trace ? 68 : skirmish::m2_fixture().seed;
        auto content = engagement_trace ? combat_engagement_witness(*root, battle_seed, economy_disabled_trace)
            : load(*root, false, false, {}, false, battle_seed);
        if (!content) return 1;
        if (ordinary_plans_trace) std::erase_if(content->modules, [](const auto& module) {
            return module.first.ends_with("/groundtospacedamage.lua") || module.first.ends_with("/groundtospacedisable.lua");
        });
        if (source_free_trace) {
            std::set<tactical::TypeId> sources;
            for (const auto& type : content->ai.content.types) if (type.space_special_weapon) sources.insert(type.type_id);
            std::erase_if(content->start.setup.units, [&](const auto& unit) { return sources.contains(unit.type_id); });
            std::erase_if(content->start.units, [&](const auto& unit) { return sources.contains(unit.state.type_id); });
        }
        const auto result = run(*content, 1, ticks, {}, engagement_trace);
        if (!result) return 1;
        std::ofstream output(argc > 3 ? argv[3] : "combat-trace.txt");
        for (const auto& command : result->commands) output << command << '\n';
        for (const auto& event : result->journal.plans)
            output << event.tick << ',' << event.player << ',' << event.plan << ',' << event.goal << ','
                   << event.target << ',' << event.event << ',' << event.detail << '\n';
        output << "AI hits " << result->ai_hits << '\n';
        output << "AI first hit " << result->first_ai_hit << '\n';
        if (engagement_trace) {
            output << "All admitted AI hits " << result->stock_hits << '\n';
            output << "All admitted AI first hit " << result->first_stock_hit << '\n';
            output << "AI hits on player " << result->player_hits << '\n';
            output << "AI first hit on player " << result->first_player_hit << '\n';
            for (const auto& event : result->ai_rejections)
                output << event.tick << ",order_rejected," << event.unit << ','
                    << tactical::to_string(event.order) << ',' << tactical::to_string(event.reason) << '\n';
            std::ofstream hits(std::string(argc > 3 ? argv[3] : "combat-trace.txt") + ".hits.csv");
            hits << "tick,shooter,target,x,y,z\n" << result->hits_csv;
            expect(static_cast<bool>(hits), "the engagement hit trace is written");
        }
        expect(static_cast<bool>(output), "the combat trace is written");
        return failures != 0 ? 1 : 0;
    }
    const bool tracing = argc > 1 && std::string_view(argv[1]) == "--ability-trace";
    const std::uint64_t ticks = tracing ? (argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 3600)
                                      : (argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 3600);
    auto content = load(*root, tracing);
    if (!content) return 1;
    if (tracing) return ability_trace(*content, ticks, argc > 3 ? argv[3] : "abilities.csv");

    progressive_random_probe(*root, *content);
    if (failures != 0) return 1;

    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, ticks);
        if (!result) return 1;
        std::cout << workers << " worker(s): " << ticks << " ticks in " << result->seconds << " s, final "
                  << result->hashes.back() << '\n';
        runs.push_back(std::move(*result));
    }
    const Run& first = runs.front();
    if (argc > 2) {
        std::ofstream hits(argv[2], std::ios::binary);
        hits << "tick,shooter,target,x,y,z\n" << first.hits_csv;
    }
    for (std::size_t index = 1; index < runs.size(); ++index) {
        expect(runs[index].hashes == first.hashes, "every tick's combined hash matches worker count 1");
        expect(runs[index].commands == first.commands, "the script commands match worker count 1");
        expect(runs[index].events == first.events, "the world events match worker count 1");
        expect(runs[index].diagnostics == first.diagnostics, "the script diagnostics match worker count 1");
    }

    std::size_t submitted = 0;
    for (const auto& command : first.commands) submitted += command.find(" key ") != std::string::npos ? 1 : 0;
    std::cout << "script commands: " << first.commands.size() << " (" << submitted << " submitted), AI hits: "
              << first.ai_hits << ", world events: " << first.events.size() << '\n';
    std::cout << "AI orders by unit type:";
    for (const auto& [type, count] : first.orders_by_type) std::cout << ' ' << type << '=' << count;
    std::cout << '\n';
    std::map<std::string, std::uint64_t> plan_events;
    for (const auto& event : first.journal.plans) ++plan_events[event.event];
    std::cout << "AI plan activity:";
    for (const auto& [event, count] : plan_events) std::cout << ' ' << event << '=' << count;
    std::cout << "; artillery units seen=" << first.artillery_seen.size() << '\n';
    std::cout << "Lua load: max " << first.max_instructions << " instructions in a tick, mean "
              << (first.total_instructions / static_cast<std::int64_t>(ticks)) << " (budget "
              << auth::Quotas{}.instructions_per_service << " per instance)\n";
    for (const auto& diagnostic : first.diagnostics) std::cout << "diagnostic: " << diagnostic << '\n';
    expect(submitted > 0, "the AI orders its units");
    // #452: the AI's Attack_Move and Guard_Target reach the session as the real orders, never dropped.
    std::map<std::string, std::size_t> verbs;
    for (const auto& command : first.commands) {
        const std::string verb = command.substr(0, command.find(' '));
        ++verbs[verb];
        if (verb == foc::verb_attack_move || verb == foc::verb_guard) {
            expect(command.find(" key ") != std::string::npos, "the order is submitted: " + command);
        }
    }
    for (const auto& [verb, count] : verbs) std::cout << "  " << verb << ": " << count << '\n';
    // GS-04/PL-01 and WSW-01/38: source identities and the stock plan inventory
    // affect keyed choices. Seed 67 remains the deterministic record witness;
    // nearby seeds below exercise combat progression.
    if (first.ai_hits == 0) {
        std::cout << "No-hit AI routing:\n";
        for (std::size_t index = 0; index < std::min<std::size_t>(40, first.commands.size()); ++index)
            std::cout << first.commands[index] << '\n';
    }
    std::cout << "Stock special-weapon observation: completed use purchases " << first.special_use_purchases
        << ", travelling special projectiles " << first.special_projectiles
        << " (stock skirmish economy)\n";
    // #518: the squadrons the Empire station and the Acclamator launch (SK-23) are the AI's
    // squadrons like its tick-zero ones: it orders their team containers, never their craft (FH-13).
    const auto ordered = [&first](const char* type) {
        const auto found = first.orders_by_type.find(type);
        return found == first.orders_by_type.end() ? std::uint64_t{0} : found->second;
    };
    expect(ordered("TIE_Fighter_Squadron") + ordered("TIE_Bomber_Squadron") > 0,
           "#518: the AI orders its launched squadrons as squadrons");
    expect(ordered("TIE_Fighter") + ordered("TIE_Bomber") + ordered("TIE_Interceptor") == 0,
           "#518: the AI never orders a squadron's craft on its own");
    expect(first.max_instructions < auth::Quotas{}.instructions_per_service / 4, "the AI's Lua load stays below a quarter of the budget");

    // The record replays without scripts: the world hashes come back from the commands alone.
    const auto replay_began = std::chrono::steady_clock::now();
    auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities, content->economy, content->content.map_box);
    const double replay_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - replay_began).count();
    expect(replayed && replayed.value() == first.world_hashes, "the headless replay of the record reproduces the world");
    std::cout << "world alone (headless replay, 1 worker): " << replay_seconds << " s; with the AI: " << first.seconds
              << " s\n";

    // The live session hosts the same AI on its simulation thread (driven pacing, 4 workers).
    eawr::platform::LiveSession::Options options;
    options.workers = 4;
    options.pacing = eawr::platform::LiveSession::Pacing::driven;
    auto scripts = std::make_shared<eawr::platform::LiveScripts>();
    scripts->wrap = [&content](tactical::TacticalSession world) {
        return foc::create_session(std::move(world), content->ai, content->modules);
    };
    options.scripts = scripts;
    auto live = eawr::platform::LiveSession::start(content->start.setup, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, options, content->victory, content->content.fog, content->content.abilities, content->economy, content->content.map_box);
    expect(static_cast<bool>(live), "the live session starts with the AI");
    if (live) {
        live.value()->advance_to(ticks);
        expect(live.value()->wait_for(ticks, std::chrono::minutes(10)), "the live session reaches the last tick");
        live.value()->stop();
        expect(live.value()->tick_hashes() == first.world_hashes, "the live session's hashes are the AI run's world hashes");
        const auto report = live.value()->script_report();
        expect(report.ticks == ticks && report.max_instructions == first.max_instructions,
               "the live session reports the same Lua load");
    }

    std::optional<Content> combat_content;
    std::optional<Run> combat_first;
    std::uint64_t combat_window = ticks;
    for (const std::uint64_t seed : {66U, 68U}) {
        // This is a project progression window, not a sourced FoC deadline.
        // WDF-05 map-relative grids moved seed 66's first hit from 3487 to 5244.
        // WTA-01/02's 7357 first-hit measurement was at f728e45d. Main merged before
        // 0038e7de, including the refused-attack hold (WTA-19, legacy #3109): bombers now
        // wait instead of failing and re-proposing, moving seed 66's first hit to 10850.
        // The live event-counter correction leaves the 21600-tick command/plan trace
        // byte-identical at 0038e7de and 05d84570 (365 hits), SHA-256:
        // b60f61f514f255639633aaacd951b6e14464637f70dd74d1a2b9545c3883ca68.
        // Keep the WDF-05 margin formula: 10850 + 1970 = 12820, rounded up to 12900.
        // Explicit horizons remain available.
        // FM-20/WSQ-58's sourced hold removes the earlier incidental idle drift.
        // Keep the stock witness's 7200-tick coverage bound, with its full rules:
        // an empty economy cannot exercise the production purchase/construction path.
        const std::uint64_t witness_ticks = argc == 1 ? (seed == 66 ? 12900 : 7200) : ticks;
        auto witness = seed == 68 ? combat_engagement_witness(*root, seed)
                                 : load(*root, false, false, {}, false, seed);
        if (!witness) return 1;
        if (seed == 68) {
            std::cout << "combat-engagement witness (stock economy): seed 68 window " << witness_ticks << '\n';
            expect(witness->economy.player(2) != nullptr && !witness->economy.menus.empty(),
                "the combat/pause witness retains the stock ledger and construction menus");
        }
        std::optional<Run> baseline;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto result = run(*witness, workers, witness_ticks, {}, true);
            if (!result) return 1;
            std::cout << "combat seed " << seed << " workers " << workers << " AI hits " << result->ai_hits
                      << " first hit " << result->first_ai_hit << " (authored roster)\n";
            if (seed == 68) std::cout << "combat seed 68 all-admitted workers " << workers
                << " AI hits " << result->stock_hits << " first hit " << result->first_stock_hit << '\n';
            std::cout << "combat seed " << seed << " player-contact workers " << workers
                << " AI hits " << result->player_hits << " first hit " << result->first_player_hit << '\n';
            check_combat_engagement(*result, seed, witness_ticks);
            expect(result->max_instructions < auth::Quotas{}.instructions_per_service / 4,
                "the combat witness stays within the Lua budget");
            if (!baseline) baseline = std::move(result);
            else {
                expect(result->hashes == baseline->hashes && result->commands == baseline->commands
                    && result->events == baseline->events && result->diagnostics == baseline->diagnostics
                    && result->player_hits == baseline->player_hits && result->first_player_hit == baseline->first_player_hit,
                    "combat witness hashes, commands, events, diagnostics and player contact match on 1/2/4/8 workers");
            }
        }
        const auto plain = eawr::platform::headless_tick_hashes(baseline->record, witness->content.sensors,
            witness->content.durability, witness->content.motion, witness->content.combat, witness->victory,
            witness->content.fog, witness->content.abilities, witness->economy, witness->content.map_box);
        expect(plain && plain.value() == baseline->world_hashes, "combat witness replays without scripts");
        if (seed == 68) {
            combat_content = std::move(witness);
            combat_first = std::move(baseline);
            combat_window = witness_ticks;
        }
    }
    auto combat_scripts = std::make_shared<eawr::platform::LiveScripts>();
    combat_scripts->wrap = [&combat_content](tactical::TacticalSession world) {
        return foc::create_session(std::move(world), combat_content->ai, combat_content->modules);
    };

    // #459 with the AI: a real-time battle paused mid-attack and resumed on fast forward
    // (TM-07, TP-02). The freestore services inside the tick, on sim seconds (FH-11), so it
    // pauses with the sim, never runs while paused, and services per tick at any rate; its
    // commands keep their next-tick keys. The replay equals an unpaused run's.
    const std::uint64_t pause_tick = combat_first->first_player_hit + 30;
    const std::uint64_t halt = pause_tick + 150;
    expect(combat_first->first_player_hit > 0 && halt <= combat_window, "the AI attacks early enough to pause mid-attack");
    const auto with_ai = [&](eawr::platform::LiveSession::Pacing pacing, std::uint32_t rate) {
        eawr::platform::LiveSession::Options live_options = options;
        live_options.scripts = combat_scripts;
        live_options.pacing = pacing;
        live_options.target_rate = rate;
        return eawr::platform::LiveSession::start(combat_content->start.setup, combat_content->content.sensors,
            combat_content->content.durability, combat_content->content.motion, combat_content->content.combat, live_options,
            combat_content->victory, combat_content->content.fog, combat_content->content.abilities,
            combat_content->economy, combat_content->content.map_box);
    };
    auto paused = with_ai(eawr::platform::LiveSession::Pacing::real_time, 1000);
    auto unpaused = with_ai(eawr::platform::LiveSession::Pacing::driven, 30);
    expect(paused && unpaused, "the paused and unpaused AI sessions start");
    if (combat_first->first_player_hit > 0 && halt <= combat_window && paused && unpaused) {
        using namespace std::chrono_literals;
        auto& session = *paused.value();
        session.halt_at(halt);
        // The approach at the highest rate, then fast forward (TM-06) into the attack.
        expect(session.wait_for(combat_first->first_player_hit > 60 ? combat_first->first_player_hit - 60 : 0, std::chrono::minutes(5)),
               "the paused run reaches the approach");
        session.set_target_rate(120);
        expect(session.wait_for(pause_tick, std::chrono::minutes(5)), "the paused run reaches the attack");
        session.set_paused(true);
        std::this_thread::sleep_for(100ms);
        const std::uint64_t paused_at = session.completed_tick();
        const auto services = session.script_report().ticks;
        const auto frame = session.frame().latest;
        expect(paused_at >= pause_tick && paused_at < halt, "the battle pauses mid-attack");
        expect(services == paused_at, "the AI serviced every tick up to the pause");
        std::this_thread::sleep_for(500ms);
        expect(session.completed_tick() == paused_at, "no tick runs while the AI battle is paused");
        expect(session.script_report().ticks == services, "the AI never services while paused");
        expect(session.frame().latest == frame, "the paused battle publishes no new frame");
        session.set_paused(false);
        expect(session.wait_for(halt, std::chrono::minutes(5)), "the resumed run reaches the halt tick");
        expect(!session.wait_for(halt + 1, 300ms), "the resumed run stops at the halt tick");
        session.stop();

        unpaused.value()->advance_to(halt);
        expect(unpaused.value()->wait_for(halt, std::chrono::minutes(5)), "the unpaused run reaches the halt tick");
        unpaused.value()->stop();

        expect(!session.failure() && !unpaused.value()->failure(), "neither AI run failed");
        const auto hashes = session.tick_hashes();
        expect(hashes.size() == halt && std::equal(hashes.begin(), hashes.end(), combat_first->world_hashes.begin()),
               "pausing mid-attack leaves every world hash unchanged");
        expect(hashes == unpaused.value()->tick_hashes(), "the paused and unpaused runs hash alike");
        expect(session.script_report().ticks == halt
                   && session.script_report().max_instructions == unpaused.value()->script_report().max_instructions
                   && session.script_report().total_instructions == unpaused.value()->script_report().total_instructions,
               "the AI serviced once per tick on fast forward, as unpaused");
        const auto replay = session.record();
        const auto plain_replay = unpaused.value()->record();
        const auto bytes = tactical::write_replay(replay);
        const auto plain_bytes = tactical::write_replay(plain_replay);
        expect(bytes && plain_bytes && bytes.value() == plain_bytes.value(),
               "the paused run's replay equals the unpaused run's");
        std::size_t around_pause = 0;
        for (const auto& command : replay.commands) {
            if (command.key.tick > paused_at && command.key.tick <= paused_at + 60) ++around_pause;
        }
        std::cout << "paused at tick " << paused_at << " (first AI hit on player " << combat_first->first_player_hit << "), halted at "
                  << halt << ", " << replay.commands.size() << " commands, " << around_pause
                  << " in the 60 ticks after the pause\n";
    }

    // Every selected plan also loads its definitions through the same production bindings.
    auto files = content->plan_modules;
    auto plans = foc::inspect_plans(content->ai, files);
    expect(static_cast<bool>(plans), "the selected plans are inspected");
    if (plans) {
        expect(plans.value().size() == 25, "AI-30/WSW-37/38: 25 selected combat, economy and planetary weapon plans");
        for (const std::string name : {"groundtospacedamage", "groundtospacedisable"}) {
            expect(std::any_of(plans.value().begin(), plans.value().end(), [&](const auto& plan) {
                return plan.path.ends_with("/" + name + ".lua") && plan.loaded;
            }), "WSW-37/38: installed planetary plan loads: " + name);
        }
        for (const auto& plan : plans.value()) {
            std::cout << "plan " << plan.path << ": category " << (plan.category.empty() ? "(none)" : plan.category)
                      << (plan.loaded ? "" : " NOT LOADED") << '\n';
            for (const auto& diagnostic : plan.diagnostics) std::cout << "  " << diagnostic << '\n';
            expect(plan.loaded && !plan.category.empty(), "the plan " + plan.path + " loads and names its goal category");
        }
    }
    for (const auto& line : foc::unsupported_plan_calls()) std::cout << "unsupported: " << line << '\n';

    hutt_nearest_probe(*content);
    ability_probe(*content);
    vengeance_probe(*root);
    crusader_probe(*root);

    if (failures != 0) {
        std::cerr << failures << " FoC AI check(s) failed\n";
        return 1;
    }
    std::cout << "FoC AI contracts passed\n";
    return 0;
}
