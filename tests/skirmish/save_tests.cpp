// Skirmish battle saves (docs/checkpoint-format.md "Skirmish battle saves"): the tactical-host
// codec, and with installed game data a real-content restore-and-continue check. A skirmish
// space battle with its non-human players on the FoC AI (default: the stock Coruscant 3v3; or
// a mod chain's map) is saved between two ticks at three sample ticks, loaded into a fresh
// session and continued; every later authoritative (world and script) hash and the final
// command record must equal an uninterrupted run, on 1 and 8 workers. --save-fixture checks the
// committed small space save (tests/skirmish/fixtures/saves/), and regenerates it with --write.
#include "../support/check.hpp"
#include "../support/env.hpp"

#include "eawr/checkpoint/checkpoint.hpp"
#include "eawr/checkpoint/skirmish.hpp"
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/battle_scoring.hpp"
#include "eawr/platform/saved_games.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/script/authoritative/tactical_bridge.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"
#include "skirmish_save_hints.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace checkpoint = eawr::checkpoint;
namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;
namespace skirmish = eawr::skirmish;
namespace tactical = eawr::sim::tactical;
using Bytes = std::vector<std::uint8_t>;

eawr::testing::Checker checker;
void expect(const bool condition, const std::string& message) { checker.check(condition, message, std::cerr); }

auth::TacticalScriptSave sample_host() {
    auth::TacticalScriptSave saved;
    saved.completed_tick = 1234;
    saved.scheduler = "scheduler-bytes";
    saved.native = Bytes{1, 2, 3, 4};
    auth::ScriptCommand command;
    command.tick = 1235;
    command.issuer = 2;
    command.sequence = 7;
    command.verb = "Move";
    command.arguments.push_back(auth::Value::number(eawr::script::numeric::LuaNumber::from_repr(0x4059000000000000ULL)));
    command.arguments.push_back(auth::Value::text("target"));
    command.arguments.push_back(auth::Value{true});
    command.arguments.push_back(auth::Value{auth::Handle{3, 99}});
    command.arguments.push_back(auth::Value{auth::ValueList{auth::Value{}, auth::Value{false}}});
    saved.pending.push_back(command);
    command.sequence = 8;
    command.arguments.clear();
    saved.pending.push_back(command);
    saved.last_keys.emplace(1, std::pair<std::uint64_t, std::uint64_t>{1234, 3});
    saved.last_keys.emplace(2, std::pair<std::uint64_t, std::uint64_t>{1235, 8});
    return saved;
}

void codec_contracts() {
    expect(checkpoint::section_name(20) == "tactical-host" && checkpoint::known_section(20),
        "section 20 is the reserved tactical-host section");
    const auto saved = sample_host();
    const auto bytes = checkpoint::encode_tactical_host(saved);
    expect(static_cast<bool>(bytes), "a host with pending commands, native state and cursors encodes");
    if (!bytes) return;
    const auto decoded = checkpoint::decode_tactical_host(bytes.value());
    expect(static_cast<bool>(decoded), "the encoded host decodes");
    if (!decoded) return;
    const auto again = checkpoint::encode_tactical_host(decoded.value());
    expect(again && again.value() == bytes.value(), "the host round-trips to identical bytes");
    expect(decoded.value().completed_tick == saved.completed_tick && decoded.value().scheduler == saved.scheduler
        && decoded.value().native == saved.native && decoded.value().pending.size() == 2
        && decoded.value().pending[0].arguments.size() == 5 && decoded.value().last_keys == saved.last_keys,
        "the decoded host carries every field");
    auto stateless = saved;
    stateless.native.reset();
    stateless.pending.clear();
    stateless.last_keys.clear();
    const auto plain = checkpoint::encode_tactical_host(stateless);
    const auto plain_decoded = plain ? checkpoint::decode_tactical_host(plain.value())
                                     : eawr::core::Result<auth::TacticalScriptSave>::failure(plain.error());
    expect(plain_decoded && !plain_decoded.value().native && plain_decoded.value().pending.empty(),
        "a stateless host keeps native absence");

    const auto refused = [&](Bytes damaged, const std::string& what) {
        const auto result = checkpoint::decode_tactical_host(damaged, "save.eawr-ckp");
        expect(!result && result.error().code == checkpoint::diagnostic_codes::section_invalid
            && result.error().logical_path == std::string("save.eawr-ckp"), what + " is refused");
    };
    const auto& good = bytes.value();
    refused(Bytes(good.begin(), good.end() - 1), "a truncated host");
    auto trailing = good;
    trailing.push_back(0);
    refused(trailing, "trailing host bytes");
    auto tag = good;
    tag[0] = 2;
    refused(tag, "another host tag");
    refused({}, "an empty host");
    // The native presence u32 follows the tag, the tick and the scheduler string.
    auto presence = good;
    presence[4 + 8 + 8 + saved.scheduler.size()] = 2;
    refused(presence, "an invalid native presence");
    // The last cursor's player becomes the first's: a duplicate cursor.
    auto duplicate = good;
    duplicate[duplicate.size() - 20] = 1;
    refused(duplicate, "a duplicate routing cursor");
    // An absurd pending count larger than the bytes left.
    auto count = good;
    const auto pending_at = 4 + 8 + 8 + saved.scheduler.size() + 4 + 8 + saved.native->size();
    for (std::size_t i = 0; i < 8; ++i) count[pending_at + i] = 0xff;
    refused(count, "a pending count larger than its bytes");
}

// SAE-10 cursor rules on a content-free corpus battle. A reinforcement whose position search fails
// advances its issuer's cursors (the world's submitted-command cursor and the script host's routing
// cursor) but is never recorded. Dropping the issuer's newest command from the sealed log gives exactly
// that shape: the saved cursor leads the recorded input.
void cursor_contracts(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    const Bytes bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto replay = tactical::parse_replay(bytes, path);
    expect(static_cast<bool>(replay), "the cursor fixture parses: " + path);
    if (!replay || replay.value().commands.empty()) {
        expect(false, "the cursor fixture has commands");
        return;
    }
    const auto factory = checkpoint::content_free_factory();
    const eawr::platform::ThreadWorkerAdapter executor(1);
    const auto& last = replay.value().commands.back();
    const auto player = last.key.player_id;
    const auto last_tick = last.key.tick;
    const auto stepped_to = [&](const std::uint64_t completed) -> std::optional<tactical::TacticalSession> {
        auto world = factory(replay.value());
        if (!world) { expect(false, "the cursor fixture loads: " + world.error().message); return std::nullopt; }
        while (world.value().completed_tick() < completed) {
            if (auto stepped = world.value().step(executor); !stepped) {
                expect(false, "the cursor fixture steps: " + stepped.error().message);
                return std::nullopt;
            }
        }
        return std::move(world).value();
    };
    const auto load_with = [&](const Bytes& state, std::vector<tactical::PlayerCommand> commands) {
        auto log = replay.value();
        log.commands = std::move(commands);
        auto world = factory(log);
        if (!world) return eawr::core::Result<void>::failure(world.error());
        return world.value().load_checkpoint_state(state);
    };
    auto without_last = replay.value().commands;
    without_last.pop_back();

    // World level, saved after the newest command executed.
    if (auto world = stepped_to(last_tick + 1)) {
        const auto state = world->save_checkpoint_state();
        expect(static_cast<bool>(state), "the world saves after its newest command");
        if (state) {
            expect(static_cast<bool>(load_with(state.value(), replay.value().commands)), "an equal world cursor loads");
            expect(static_cast<bool>(load_with(state.value(), without_last)),
                "a world cursor leading the log to an executed tick loads (a dropped reinforcement)");
            auto ahead = replay.value().commands;
            auto extra = last;
            ++extra.key.sequence;
            ahead.push_back(extra);
            expect(!load_with(state.value(), ahead), "a world cursor behind the log is refused");
        }
    }
    // World level, saved while the newest command is still queued for the next tick.
    if (auto world = stepped_to(last_tick)) {
        const auto state = world->save_checkpoint_state();
        expect(state && !load_with(state.value(), without_last),
            "a world cursor leading the log to the unexecuted tick is refused");
    }

    // Host level: the same battle stepped through the script host with its commands as player input.
    auto empty = replay.value();
    empty.commands.clear();
    empty.object_turns.clear();
    const auto scheduler = [&]() {
        auth::SessionConfig config;
        config.seed = replay.value().setup.seed;
        config.tick_duration = auth::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
        return auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
    };
    auto world = factory(empty);
    auto scripts = scheduler();
    if (!world || !scripts) { expect(false, "the host fixture creates"); return; }
    auto hosted = auth::ScriptedTacticalSession::create(std::move(world).value(), std::move(scripts).value());
    if (!hosted) { expect(false, "the host fixture wraps: " + hosted.error().message); return; }
    std::size_t next = 0;
    const auto& commands = replay.value().commands;
    while (hosted.value().completed_tick() < last_tick + 2) {
        std::vector<tactical::PlayerCommand> input;
        while (next < commands.size() && commands[next].key.tick == hosted.value().completed_tick()) input.push_back(commands[next++]);
        if (auto stepped = hosted.value().step(executor, input); !stepped) {
            expect(false, "the host fixture steps: " + stepped.error().message);
            return;
        }
    }
    const auto saved = hosted.value().save_scripts();
    const auto state = hosted.value().world().save_checkpoint_state();
    const auto log = hosted.value().world().record();
    if (!saved || !state) { expect(false, "the host fixture saves"); return; }
    const auto completed = hosted.value().completed_tick();
    const auto host_cursor = saved.value().last_keys.find(player);
    expect(host_cursor != saved.value().last_keys.end(), "the host fixture routed the newest command's player");
    if (host_cursor == saved.value().last_keys.end()) return;
    const auto restore_with = [&](const std::pair<std::uint64_t, std::uint64_t> key) {
        auto rebuilt = factory(log);
        auto fresh = scheduler();
        if (!rebuilt || !fresh) return false;
        if (!rebuilt.value().load_checkpoint_state(state.value())) return false;
        auto edited = saved.value();
        edited.last_keys[player] = key;
        return static_cast<bool>(auth::ScriptedTacticalSession::restore(std::move(rebuilt).value(), std::move(fresh).value(), edited));
    };
    const auto cursor = host_cursor->second;
    expect(restore_with(cursor), "an equal routing cursor restores");
    expect(restore_with({completed - 1, cursor.second + 5}),
        "a routing cursor leading the recorded input to an executed tick restores (a dropped reinforcement)");
    expect(cursor.second == 0 || !restore_with({cursor.first, cursor.second - 1}), "a routing cursor behind the recorded input is refused");
    expect(!restore_with({completed, 0}), "a routing cursor at the unexecuted tick is refused");
}

struct Hosted {
    auth::ScriptedTacticalSession session;
    std::string closure;
};

// A skirmish battle with every content step of the production AI battle (sim_headless's AI
// report and the viewer's live battle): the mounted chain, the selected map's start, the
// session content, fog, economy, the FoC AI with its goal system and the battle scoring.
struct Scene {
    std::optional<eawr::vfs::Vfs> files;
    std::optional<eawr::data::LoadResult> catalog;
    std::optional<eawr::units::UnitTables> tables;
    std::optional<skirmish::Fixture> fixture;
    std::optional<skirmish::StartInputs> inputs;
    std::optional<skirmish::SkirmishStart> start;
    std::optional<skirmish::SessionContent> content;
    std::optional<tactical::EconomyRules> economy;
    std::optional<tactical::VictoryRules> victory;
    std::optional<tactical::TacticalSetup> setup;
    std::optional<foc::AiSetup> ai;
    std::map<std::string, std::string> modules;
    std::unique_ptr<eawr::platform::BattleScoring> scoring;
};

// `mods` is the leaf;parent;... chain (empty: stock FoC); `map` a logical .ted path (empty: the
// stock Coruscant 3v3 with five AI players, as the AI serial profile).
std::unique_ptr<Scene> load_scene(const std::filesystem::path& root, const std::string& mods, const std::string& map,
    const std::uint64_t seed) {
    const auto failed = [](const std::string& what, const eawr::core::Diagnostic& diagnostic) {
        expect(false, what + ": " + eawr::core::format_diagnostic(diagnostic));
        return std::unique_ptr<Scene>{};
    };
    auto scene = std::make_unique<Scene>();
    auto roots = eawr::vfs::mod_chain_roots(mods);
    roots.emplace_back("expansion", root / "corruption" / "Data");
    roots.emplace_back("base", root / "GameData" / "Data");
    auto chain = eawr::vfs::resolve_manifest_chain(roots);
    if (!chain) return failed("the content chain resolves", chain.error());
    std::vector<eawr::vfs::MountSpec> specs;
    for (auto& layer : chain.value()) specs.push_back(std::move(layer.mount));
    auto files = eawr::vfs::Vfs::mount(specs);
    if (!files) return failed("the content chain mounts", files.error());
    scene->files.emplace(std::move(files).value());
    const auto& vfs = *scene->files;
    auto catalog = eawr::data::load_catalog_strict(vfs, mods.empty() ? eawr::data::Profile::foc : eawr::data::Profile::remake);
    if (!catalog) return failed("the catalog loads", catalog.error());
    scene->catalog.emplace(std::move(catalog).value());
    const auto& types = scene->catalog->catalog;
    eawr::scene::VfsAssetCache cache(vfs);
    eawr::units::LoadInput input;
    input.catalog = &types;
    input.filesystem = &vfs;
    input.model = cache.access().model;
    auto tables = eawr::units::load_unit_tables(input);
    if (!tables) return failed("the unit tables load", tables.error());
    scene->tables.emplace(std::move(tables).value());

    skirmish::FixtureOptions options;
    options.seed = seed;
    if (!map.empty()) {
        options.map = map;
    } else {
        options.map = std::string("data/art/maps/_mp_space_coruscant.ted");
        options.slots = std::vector<skirmish::LobbySlot>{};
        for (std::uint32_t index = 0; index < 6; ++index) {
            const bool rebel = index < 3;
            skirmish::LobbySlot slot;
            slot.slot = index + 1;
            slot.faction = rebel ? "Rebel" : "Empire";
            slot.team = rebel ? 0U : 1U;
            slot.human = index == 0;
            slot.fleet = rebel
                ? std::vector<std::string>{"Y-Wing_Squadron", "Corellian_Corvette", "Nebulon_B_Frigate", "Calamari_Cruiser"}
                : std::vector<std::string>{"Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Victory_Destroyer", "Imperial_Star_Destroyer"};
            options.slots->push_back(std::move(slot));
        }
    }
    auto fixture = skirmish::fixture_from_options(options, vfs, types);
    if (!fixture) return failed("the skirmish fixture resolves", fixture.error());
    scene->fixture.emplace(std::move(fixture).value());
    auto inputs = skirmish::read_start_inputs(*scene->fixture, vfs, types, *scene->tables);
    if (!inputs) return failed("the start inputs read", inputs.error());
    scene->inputs.emplace(std::move(inputs).value());
    // SC-01: the selected start may need types outside the first closure, as in the viewer and CLI.
    const auto missing = skirmish::missing_start_types(*scene->fixture, *scene->inputs, *scene->tables);
    if (!missing.empty()) {
        eawr::units::LoadInput extended = input;
        extended.space_map = scene->fixture->map;
        for (const auto type : eawr::units::pinned_m2_types()) extended.types.emplace_back(type);
        extended.types.insert(extended.types.end(), missing.begin(), missing.end());
        for (const auto type : eawr::units::pinned_m2_obstacles()) extended.obstacles.emplace_back(type);
        auto more = eawr::units::load_unit_tables(extended);
        if (!more) return failed("the start's unit tables load", more.error());
        scene->tables.emplace(std::move(more).value());
        scene->inputs->tables = &*scene->tables;
    }
    const auto& unit_tables = *scene->tables;
    auto start = skirmish::build_start(*scene->fixture, *scene->inputs);
    if (!start) return failed("the start builds", start.error());
    scene->start.emplace(std::move(start).value());
    std::vector<eawr::core::Diagnostic> skipped;
    auto content = skirmish::session_content(unit_tables, skirmish::human_slots(*scene->fixture), {},
        mods.empty() ? eawr::units::ContentErrors::strict : eawr::units::ContentErrors::skip_object, &skipped);
    if (!content) return failed("the session content builds", content.error());
    scene->content.emplace(std::move(content).value());
    skirmish::drop_skipped_starts(*scene->start, skipped, &scene->content->load_notes);
    const auto skipped_types = skirmish::skipped_type_ids(skipped);
    skirmish::bind_skipped_types(*scene->start, skipped_types);
    if (auto bound = skirmish::bind_difficulty(*scene->content, *scene->start, *scene->inputs); !bound)
        return failed("the AI difficulty binds", bound.error());
    auto fog = skirmish::fog_rules(*scene->inputs);
    if (!fog) return failed("the fog grid builds", fog.error());
    scene->content->fog = fog.value();
    scene->content->map_box = skirmish::map_box(*scene->inputs);
    auto economy = skirmish::economy_rules(*scene->start, *scene->inputs, unit_tables);
    if (!economy) return failed("the economy loads", economy.error());
    scene->economy.emplace(std::move(economy).value());
    static_cast<void>(skirmish::drop_skipped_menu_options(*scene->economy, skipped_types, nullptr));
    scene->victory.emplace(skirmish::victory_rules(*scene->start, unit_tables));
    scene->setup.emplace(skirmish::recording_setup(*scene->fixture, *scene->start));
    scene->ai.emplace(skirmish::ai_setup(*scene->start, *scene->inputs, unit_tables));
    if (auto enabled = skirmish::enable_goal_system(vfs, *scene->ai); !enabled)
        return failed("the goal system loads", enabled.error());
    auto modules = skirmish::ai_modules(vfs, *scene->ai);
    if (!modules) return failed("the AI's Lua files load", modules.error());
    scene->modules = std::move(modules).value();
    std::map<tactical::PlayerId, std::string> factions;
    for (const auto& player : scene->start->players) factions.emplace(player.player.player_id, player.faction);
    auto scoring = eawr::platform::BattleScoring::create(vfs, types, unit_tables, std::move(factions), scene->fixture->map);
    if (!scoring) return failed("the battle scoring loads", scoring.error());
    scene->scoring = std::move(scoring).value();
    std::size_t ai_players = 0;
    for (const auto& player : scene->ai->players) ai_players += player.ai ? 1U : 0U;
    std::cout << "SKIRMISH-SCENE,map=" << scene->fixture->map << ",players=" << scene->ai->players.size()
        << ",ai=" << ai_players << ",units=" << scene->setup->units.size() << ",skipped_types=" << skipped_types.size()
        << std::endl;
    expect(ai_players > 0, "the scene runs the AI");
    return scene;
}

// The AI host of one session: the scene's AI with a fresh scorer, as the live battle wraps it.
eawr::core::Result<foc::PreparedAiSession> prepare(const Scene& scene) {
    auto setup = *scene.ai;
    auto scorer = scene.scoring->fresh();
    if (!scorer) return eawr::core::Result<foc::PreparedAiSession>::failure(scorer.error());
    setup.scoring = std::move(scorer).value();
    return foc::prepare_session(setup, scene.modules);
}

eawr::core::Result<tactical::TacticalSession> world_of(const Scene& scene, const tactical::TacticalReplay& replay) {
    const auto& content = *scene.content;
    return tactical::TacticalSession::from_replay(replay, content.sensors, content.durability, content.motion, content.fog,
        content.combat, *scene.victory, content.abilities, *scene.economy, content.map_box);
}

std::optional<Hosted> start(const Scene& scene) {
    const auto& content = *scene.content;
    auto world = tactical::TacticalSession::create(*scene.setup, content.sensors, content.durability, content.motion,
        content.fog, content.combat, *scene.victory, content.abilities, *scene.economy, content.map_box);
    if (!world) { expect(false, "the battle world creates: " + world.error().message); return std::nullopt; }
    auto prepared = prepare(scene);
    if (!prepared) { expect(false, "the AI host prepares: " + prepared.error().message); return std::nullopt; }
    auto session = auth::ScriptedTacticalSession::create(std::move(world).value(), std::move(prepared.value().scripts));
    if (!session) { expect(false, "the scripted battle creates: " + session.error().message); return std::nullopt; }
    if (auto bound = prepared.value().bind(session.value()); !bound) {
        expect(false, "the AI binds: " + bound.error().message);
        return std::nullopt;
    }
    return Hosted{std::move(session).value(), prepared.value().closure_sha256};
}

// The first tick (1-based) at which `hashes` leaves `reference`, from `from` on; 0 when none.
std::uint64_t first_difference(const std::vector<std::string>& reference, const std::vector<std::string>& hashes,
    const std::uint64_t from) {
    for (std::uint64_t index = 0; index < hashes.size(); ++index) {
        const auto tick = from + index;
        if (tick > reference.size() || reference[tick - 1] != hashes[index]) return tick;
    }
    return 0;
}

int installed(const std::filesystem::path& root, const std::uint64_t ticks, const std::string& mods, const std::string& map) {
    auto content = load_scene(root, mods, map, 67);
    if (!content) return 1;
    const std::vector<std::uint64_t> samples{ticks / 4, ticks / 2, ticks * 3 / 4};

    // The reference: one uninterrupted run on one worker, never saved.
    std::vector<std::string> reference;
    std::vector<tactical::PlayerCommand> reference_commands;
    std::string closure;
    {
        auto hosted = start(*content);
        if (!hosted) return 1;
        closure = hosted->closure;
        const eawr::platform::ThreadWorkerAdapter executor(1, eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost);
        for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
            const auto stepped = hosted->session.step(executor);
            if (!stepped) { expect(false, "reference tick: " + stepped.error().message); return 1; }
            reference.push_back(stepped.value().state_sha256);
        }
        reference_commands = hosted->session.record().commands;
    }
    expect(reference.size() == ticks && !reference.back().empty(), "the reference run hashes every tick");
    expect(!closure.empty(), "the AI host names its script closure");

    const auto build = checkpoint::skirmish_identities(content->setup->content_identity,
        checkpoint::script_closure_digest(closure));
    const checkpoint::TacticalSessionFactory world_factory = [&](const tactical::TacticalReplay& replay) {
        return world_of(*content, replay);
    };
    const checkpoint::SkirmishHostFactory host_factory = [&](const tactical::TacticalReplay&) {
        using HostResult = eawr::core::Result<checkpoint::SkirmishHost>;
        auto prepared = prepare(*content);
        if (!prepared) return HostResult::failure(prepared.error());
        return HostResult::success(checkpoint::SkirmishHost{std::move(prepared.value().scripts), std::move(prepared.value().bind)});
    };

    // The saving run: eight workers, saved at each sample tick. Saving must not change the battle.
    std::vector<std::pair<std::uint64_t, Bytes>> saves;
    {
        auto hosted = start(*content);
        if (!hosted) return 1;
        const eawr::platform::ThreadWorkerAdapter executor(8, eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
            const auto stepped = hosted->session.step(executor);
            if (!stepped) { expect(false, "saving run tick: " + stepped.error().message); return 1; }
            hashes.push_back(stepped.value().state_sha256);
            if (std::find(samples.begin(), samples.end(), tick) == samples.end()) continue;
            const auto seats = checkpoint::default_seats(hosted->session.record().setup);
            auto bytes = checkpoint::save_skirmish_checkpoint(hosted->session, seats, build);
            if (!bytes) { expect(false, "save at tick " + std::to_string(tick) + ": " + bytes.error().message); return 1; }
            expect(bytes.value().size() <= checkpoint::max_bytes, "the save fits the checkpoint budget");
            std::cout << "SKIRMISH-SAVE,tick=" << tick << ",bytes=" << bytes.value().size()
                << ",sha256=" << eawr::core::sha256_hex(bytes.value()) << std::endl;
            saves.emplace_back(tick, std::move(bytes).value());
        }
        const auto differs = first_difference(reference, hashes, 1);
        expect(differs == 0, "saving on 8 workers leaves every hash equal to the unsaved 1-worker run (first difference at tick "
            + std::to_string(differs) + ")");
        expect(hosted->session.record().commands == reference_commands, "saving leaves the command record unchanged");
    }

    // Loads refuse another script closure, a missing host and a damaged host before any session exists.
    if (!saves.empty()) {
        const eawr::platform::ThreadWorkerAdapter executor(1, eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost);
        const auto& bytes = saves.front().second;
        auto other = build;
        other.lua_save[0] ^= 1U;
        const auto foreign = checkpoint::load_skirmish_checkpoint(bytes, other, world_factory, host_factory, executor);
        expect(!foreign && foreign.error().code == checkpoint::diagnostic_codes::other_build,
            "a save from another AI closure is refused as another build's");
        auto parsed = checkpoint::parse_checkpoint(bytes);
        expect(static_cast<bool>(parsed), "the save parses as a checkpoint");
        if (parsed) {
            const auto host_id = static_cast<std::uint16_t>(checkpoint::SectionId::tactical_host);
            auto without = parsed.value();
            std::erase_if(without.sections, [&](const auto& row) { return row.id == host_id; });
            const auto plain = checkpoint::write_checkpoint(without);
            const auto missing = plain ? checkpoint::load_skirmish_checkpoint(plain.value(), build, world_factory, host_factory, executor)
                                       : eawr::core::Result<checkpoint::SkirmishRestore>::failure(plain.error());
            expect(!missing && missing.error().code == checkpoint::diagnostic_codes::inconsistent,
                "a battle save without the host is refused");
            auto damaged = parsed.value();
            for (auto& row : damaged.sections) if (row.id == host_id) row.bytes.pop_back();
            const auto broken = checkpoint::write_checkpoint(damaged);
            const auto bad = broken ? checkpoint::load_skirmish_checkpoint(broken.value(), build, world_factory, host_factory, executor)
                                    : eawr::core::Result<checkpoint::SkirmishRestore>::failure(broken.error());
            expect(!bad && bad.error().code == checkpoint::diagnostic_codes::section_invalid, "a truncated host section is refused");
            const auto tactical_only = checkpoint::load_tactical_checkpoint(bytes, build, world_factory, executor);
            expect(static_cast<bool>(tactical_only), "the plain tactical loader accepts a skirmish save's battle");
        }
    }

    // Each save, loaded on 1 and on 8 workers and continued to the end, matches the reference.
    for (const std::size_t workers : {std::size_t{1}, std::size_t{8}}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost);
        for (const auto& [saved_tick, bytes] : saves) {
            auto loaded = checkpoint::load_skirmish_checkpoint(bytes, build, world_factory, host_factory, executor, "save.eawr-ckp");
            if (!loaded) {
                expect(false, "load of tick " + std::to_string(saved_tick) + " on " + std::to_string(workers) + " workers: "
                    + loaded.error().message);
                continue;
            }
            auto& restored = loaded.value();
            expect(restored.work.resimulated_ticks == 0 && restored.session.completed_tick() == saved_tick,
                "the load restores the saved tick without re-simulating");
            std::vector<std::string> hashes;
            for (auto tick = saved_tick + 1; tick <= ticks; ++tick) {
                const auto stepped = restored.session.step(executor);
                if (!stepped) { expect(false, "resumed tick: " + stepped.error().message); break; }
                hashes.push_back(stepped.value().state_sha256);
            }
            const auto differs = first_difference(reference, hashes, saved_tick + 1);
            const bool commands_equal = restored.session.record().commands == reference_commands;
            std::cout << "SKIRMISH-CONTINUE,saved=" << saved_tick << ",workers=" << workers << ",ticks=" << hashes.size()
                << ",first_difference=" << differs << ",commands_equal=" << commands_equal
                << ",final=" << (hashes.empty() ? std::string() : hashes.back()) << std::endl;
            expect(hashes.size() == ticks - saved_tick && differs == 0,
                "a battle loaded at tick " + std::to_string(saved_tick) + " on " + std::to_string(workers)
                + " workers continues with every hash equal (first difference at tick " + std::to_string(differs) + ")");
            expect(commands_equal, "the loaded battle records the same commands as the uninterrupted one");
        }
    }
    std::cout << "SKIRMISH-REFERENCE,ticks=" << ticks << ",final=" << reference.back() << std::endl;
    return checker.finish();
}

// RFC 4648 base64: a fresh save in the test log, so a pool build can regenerate the committed one
// (tools/replay/skirmish_save_fixture.py) without a local build.
std::string base64(const Bytes& bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    text.reserve((bytes.size() + 2U) / 3U * 4U);
    for (std::size_t index = 0; index < bytes.size(); index += 3U) {
        const std::size_t left = bytes.size() - index;
        const std::uint32_t chunk = (std::uint32_t{bytes[index]} << 16U)
            | (left > 1U ? std::uint32_t{bytes[index + 1U]} << 8U : 0U) | (left > 2U ? std::uint32_t{bytes[index + 2U]} : 0U);
        text.push_back(alphabet[(chunk >> 18U) & 63U]);
        text.push_back(alphabet[(chunk >> 12U) & 63U]);
        text.push_back(left > 1U ? alphabet[(chunk >> 6U) & 63U] : '=');
        text.push_back(left > 2U ? alphabet[chunk & 63U] : '=');
    }
    return text;
}

// The committed small space save: the content-free tactical-v2 corpus battle through a script host
// without scripts, its commands as player input, saved two ticks after its last command. With
// `write` it is regenerated; otherwise a fresh save must equal it byte for byte (a format change
// regenerates it: no back-compat before 1.0), and the committed save loads and continues with every
// hash equal to the uninterrupted battle on 1 and 8 workers.
int save_fixture(const std::string& replay_path, const std::string& save_path, const bool write) {
    std::ifstream file(replay_path, std::ios::binary);
    const Bytes log_bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto replay = tactical::parse_replay(log_bytes, replay_path);
    if (!replay || replay.value().commands.empty()) {
        expect(false, "the save fixture's battle parses and has commands: " + replay_path);
        return checker.finish();
    }
    const auto& setup = replay.value().setup;
    const auto factory = checkpoint::content_free_factory();
    const auto scheduler = [&]() {
        auth::SessionConfig config;
        config.seed = setup.seed;
        config.tick_duration = auth::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
        return auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
    };
    const auto build = checkpoint::skirmish_identities(setup.content_identity, {});
    const std::uint64_t saved_tick = replay.value().commands.back().key.tick + 2U;
    const std::uint64_t end = saved_tick + 30U;
    const auto& commands = replay.value().commands;

    // The uninterrupted battle on one worker, saved after `saved_tick`.
    auto empty = replay.value();
    empty.commands.clear();
    empty.object_turns.clear();
    auto world = factory(empty);
    auto scripts = scheduler();
    if (!world || !scripts) { expect(false, "the save fixture's battle creates"); return checker.finish(); }
    auto hosted = auth::ScriptedTacticalSession::create(std::move(world).value(), std::move(scripts).value());
    if (!hosted) { expect(false, "the save fixture's host wraps: " + hosted.error().message); return checker.finish(); }
    const eawr::platform::ThreadWorkerAdapter one(1);
    std::vector<std::string> reference;
    Bytes fresh;
    std::size_t next = 0;
    while (hosted.value().completed_tick() < end) {
        std::vector<tactical::PlayerCommand> input;
        while (next < commands.size() && commands[next].key.tick == hosted.value().completed_tick()) input.push_back(commands[next++]);
        const auto stepped = hosted.value().step(one, input);
        if (!stepped) { expect(false, "the save fixture's battle steps: " + stepped.error().message); return checker.finish(); }
        reference.push_back(stepped.value().state_sha256);
        if (hosted.value().completed_tick() != saved_tick) continue;
        auto bytes = checkpoint::save_skirmish_checkpoint(hosted.value(), checkpoint::default_seats(setup), build);
        if (!bytes) { expect(false, "the save fixture saves: " + bytes.error().message); return checker.finish(); }
        fresh = std::move(bytes).value();
    }
    expect(!fresh.empty() && !reference.back().empty(), "the save fixture's battle saves and hashes every tick");
    if (write) {
        std::filesystem::create_directories(std::filesystem::path(save_path).parent_path());
        std::ofstream output(save_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(fresh.data()), static_cast<std::streamsize>(fresh.size()));
        expect(static_cast<bool>(output), "the save fixture is written: " + save_path);
        std::cout << "SKIRMISH-SAVE-FIXTURE,written=" << save_path << ",saved=" << saved_tick << ",bytes=" << fresh.size()
                  << ",sha256=" << eawr::core::sha256_hex(fresh) << std::endl;
        return checker.finish();
    }

    std::ifstream committed_file(save_path, std::ios::binary);
    const Bytes committed((std::istreambuf_iterator<char>(committed_file)), std::istreambuf_iterator<char>());
    expect(committed == fresh, "the committed save equals a fresh one; after a format change regenerate it with "
        "skirmish_save_tests --save-fixture <tactical-v2.eawr-replay> <save> --write, or from this log with "
        "tools/replay/skirmish_save_fixture.py");
    if (committed != fresh) {
        std::cout << "SKIRMISH-SAVE-FIXTURE-BYTES,sha256=" << eawr::core::sha256_hex(fresh) << ",base64=" << base64(fresh)
                  << std::endl;
        if (committed.empty()) return checker.finish();
    }
    const checkpoint::SkirmishHostFactory host = [&](const tactical::TacticalReplay&) {
        using HostResult = eawr::core::Result<checkpoint::SkirmishHost>;
        auto fresh_scripts = scheduler();
        if (!fresh_scripts) return HostResult::failure(fresh_scripts.error());
        return HostResult::success(checkpoint::SkirmishHost{std::move(fresh_scripts).value(), {}});
    };
    for (const std::size_t workers : {std::size_t{1}, std::size_t{8}}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto loaded = checkpoint::load_skirmish_checkpoint(committed, build, factory, host, executor, save_path);
        if (!loaded) {
            expect(false, "the committed save loads on " + std::to_string(workers) + " workers: " + loaded.error().message);
            continue;
        }
        auto& restored = loaded.value().session;
        expect(restored.completed_tick() == saved_tick, "the committed save restores its tick");
        std::vector<std::string> hashes;
        while (restored.completed_tick() < end) {
            const auto stepped = restored.step(executor);
            if (!stepped) { expect(false, "the committed save continues: " + stepped.error().message); break; }
            hashes.push_back(stepped.value().state_sha256);
        }
        const auto differs = first_difference(reference, hashes, saved_tick + 1);
        expect(hashes.size() == end - saved_tick && differs == 0, "the committed save continues on " + std::to_string(workers)
            + " workers with every hash equal (first difference at tick " + std::to_string(differs) + ")");
        expect(restored.record().commands == hosted.value().record().commands, "the committed save records the same commands");
        std::cout << "SKIRMISH-SAVE-FIXTURE,saved=" << saved_tick << ",workers=" << workers << ",ticks=" << hashes.size()
                  << ",first_difference=" << differs << std::endl;
    }

    // #3989: a player's save file. The viewer's hints go into section 0x8000; the file lists as a skirmish
    // battle save, keeps its hints, and its battle still loads.
    namespace viewer = eawr::viewer;
    viewer::SkirmishSaveHints hints;
    hints.camera = viewer::SkirmishSaveCamera{{12.5F, -3.0F, 400.25F}, 0.75F, 45.0F};
    hints.label = "Kuat";
    hints.scoring = "saved scoring";
    const auto hint_bytes = viewer::encode_skirmish_save_hints(hints);
    expect(viewer::decode_skirmish_save_hints(hint_bytes) == hints, "the skirmish save hints round-trip");
    auto legacy_hints = hints;
    legacy_hints.scoring.clear();
    auto legacy = viewer::encode_skirmish_save_hints(legacy_hints);
    legacy.resize(legacy.size() - 4U);
    legacy[7] = 1;
    expect(viewer::decode_skirmish_save_hints(legacy) == legacy_hints, "version 1 hints remain readable");
    expect(viewer::decode_skirmish_save_hints(viewer::encode_skirmish_save_hints({})) == viewer::SkirmishSaveHints{},
        "hints without a camera round-trip");
    auto damaged = hint_bytes;
    damaged.pop_back();
    expect(!viewer::decode_skirmish_save_hints(damaged), "truncated hints decode to nothing");
    auto foreign = hint_bytes;
    foreign[7] = 3;
    expect(!viewer::decode_skirmish_save_hints(foreign), "hints of another version decode to nothing");

    const auto directory = std::filesystem::temp_directory_path() / "eawr-skirmish-save-files";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    const auto written = eawr::platform::write_battle_save(directory, "Battle 1", committed, hint_bytes, false);
    expect(!eawr::platform::skirmish_save_setup(Bytes{1, 2, 3}), "invalid skirmish setup bytes are refused");
    expect(static_cast<bool>(written), "a battle save is written" + (written ? std::string() : ": " + written.error().message));
    expect(!eawr::platform::write_battle_save(directory, "Battle 1", committed, hint_bytes, false),
        "an existing save is not overwritten without consent");
    expect(static_cast<bool>(eawr::platform::write_battle_save(directory, "Battle 1", committed, hint_bytes, true)),
        "an existing save is overwritten with consent");
    expect(!eawr::platform::write_battle_save(directory, "bad/name", committed, hint_bytes, false), "an invalid name is refused");
    expect(!eawr::platform::write_battle_save(directory, "Broken", Bytes{1, 2, 3}, hint_bytes, false),
        "bytes that are no checkpoint are refused");
    const auto listed = eawr::platform::list_saved_games(directory);
    expect(listed && listed.value().size() == 1, "the save directory lists the battle save");
    if (listed && listed.value().size() == 1) {
        const auto& slot = listed.value().front();
        expect(slot.name == "Battle 1" && slot.skirmish && !slot.campaign && !slot.diagnostic && slot.child
                   && slot.completed_tick == saved_tick,
            "the battle save lists as a skirmish save at its tick, not as a broken campaign");
    }
    std::ifstream saved_file(directory / "Battle 1.eawrsave", std::ios::binary);
    const Bytes saved_bytes((std::istreambuf_iterator<char>(saved_file)), std::istreambuf_iterator<char>());
    const auto parsed = checkpoint::parse_checkpoint(saved_bytes);
    const auto* section = parsed ? parsed.value().find(checkpoint::SectionId::presentation_hints) : nullptr;
    expect(section != nullptr && section->bytes == hint_bytes, "the battle save keeps the viewer's hints");
    const eawr::platform::ThreadWorkerAdapter one_worker(1);
    auto reloaded = checkpoint::load_skirmish_checkpoint(saved_bytes, build, factory, host, one_worker, "Battle 1.eawrsave");
    expect(reloaded && reloaded.value().session.completed_tick() == saved_tick, "the battle save with hints loads");
    std::filesystem::remove_all(directory, ignored);
    return checker.finish();
}

} // namespace

int main(int argc, char** argv) {
    codec_contracts();
    if (argc > 2 && std::string_view(argv[1]) == "--cursor-fixture") cursor_contracts(argv[2]);
    if (argc > 3 && std::string_view(argv[1]) == "--save-fixture") {
        const bool write = argc > 4 && std::string_view(argv[4]) == "--write";
        if (const auto result = save_fixture(argv[2], argv[3], write); result != 0 || write) return result;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--installed") {
        // --installed [ticks] [--mod-root <leaf;parent;...>] [--map <logical .ted path>]
        std::uint64_t ticks = 2400;
        std::string mods, map;
        for (int index = 2; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--mod-root" && index + 1 < argc) mods = argv[++index];
            else if (argument == "--map" && index + 1 < argc) map = argv[++index];
            else ticks = std::strtoull(argv[index], nullptr, 10);
        }
        if (ticks < 8) {
            std::cerr << "usage: skirmish_save_tests --installed [ticks >= 8] [--mod-root <chain>] [--map <path>]\n";
            return 1;
        }
        const auto root = eawr::testing::environment("EAWR_EAW_GAME_ROOT");
        if (!root) {
            std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the skirmish save continue check\n";
            return checker.finish();
        }
        if (const auto result = installed(*root, ticks, mods, map); result != 0) return result;
    }
    if (checker.failures == 0) std::cout << "skirmish save tests passed\n";
    return checker.finish();
}
