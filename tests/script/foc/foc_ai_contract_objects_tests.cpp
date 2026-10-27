#include "foc_ai_contract_fixtures.hpp"
#include "tactical_ai_world.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/skirmish/ai.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace foc_contract {


void proximity_contracts() {
    namespace m = eawr::sim::math;
    const auto fixed = [](int value) { return m::Fixed::from_integer(value).value(); };
    auto host = std::make_shared<foc::detail::Host>();
    auto view = std::make_shared<foc::detail::WorldView>();
    view->players = {{1, 1, 0}, {2, 2, 1}, {3, 2, 2}};
    // IDs deliberately disagree with discovery order. Corner is inside the box
    // despite being outside a sphere. Bounds overlap admits an outside centre.
    for (const auto& [id, owner, type, x, y, z] : {
            std::array<int, 6>{10, 1, 100, 0, 0, 0}, {20, 2, 200, 50, 50, 50},
            {30, 3, 200, 60, 0, 0}, {40, 1, 200, 0, 0, 0}, {50, 2, 300, 0, 0, 0},
            {60, 2, 200, 0, 0, 52}, {70, 2, 400, 0, 0, 0}}) {
        foc::detail::ViewUnit unit;
        unit.id = static_cast<sim::EntityId>(id); unit.owner = static_cast<tactical::PlayerId>(owner);
        unit.type = static_cast<tactical::TypeId>(type); unit.position = {fixed(x), fixed(y), fixed(z)};
        view->units.push_back(unit);
    }
    host->view = view;
    foc::AiType bounded;
    bounded.type_id = 200; bounded.name = "BOUNDED";
    bounded.proximity_bounds = std::array{m::Vec3{fixed(-10), {}, {}}, m::Vec3{fixed(10), {}, {}}};
    foc::AiType decoration; decoration.type_id = 400; decoration.name = "DECORATION"; decoration.decoration = true;
    host->types.emplace(200, &bounded); host->types.emplace(400, &decoration);
    const std::string module = R"lua(
count = 0
order = 0
function Setup(origin, player, kind)
    Object = origin
    enemy = player
    local increment = 1
    handler = function(self, nearby)
        if self ~= Object then error('callback argument order') end
        count = count + increment
        order = order * 10 + nearby.Get_Owner().Get_ID()
    end
    if Object.Event_Object_In_Range(handler, 50, player, kind) ~= true then error('registration return') end
end
function Repeat() Object.Service_Wrapper() end
function Cancel()
    Object.Event_Object_In_Range(handler, 50)
    Object.Cancel_Event_Object_In_Range(handler)
    Object.Service_Wrapper()
end
function Mutation()
    Object.Cancel_Event_Object_In_Range(handler)
    local second
    second = function(self, nearby)
        count = count + 100
        self.Cancel_Event_Object_In_Range(second)
    end
    local first
    first = function(self, nearby)
        count = count + 10
        self.Cancel_Event_Object_In_Range(first)
        self.Event_Object_In_Range(second, 50, player, kind)
    end
    Object.Event_Object_In_Range(first, 50)
    Object.Service_Wrapper()
end
function Marker()
    local callback
    callback = function(self, nearby)
        count = count + 1
        self.Cancel_Event_Object_In_Range(callback)
    end
    Object.Event_Object_In_Range(callback, 50)
    Object.Service_Wrapper()
    Object.Service_Wrapper()
end
function Spawn()
    local callback
    callback = function(self, nearby)
        if nearby.Get_Owner().Get_ID() == 1 then
            spawned = true
            self.Cancel_Event_Object_In_Range(callback)
        end
    end
    Object.Event_Object_In_Range(callback, 250)
    Object.Service_Wrapper()
end
function Artillery()
    local far = function(self, enemy) enemies_in_range = true end
    local near = function(self, enemy) enemies_near = true end
    Object.Event_Object_In_Range(far, 1000, enemy)
    Object.Event_Object_In_Range(near, 50, enemy)
    Object.Service_Wrapper()
end
function Coroutine() Create_Thread('Once') end
function Once() Object.Service_Wrapper() end
function Bad() Object.Event_Object_In_Range(123, 50) end
function Escape() GlobalValue.Set('function', handler) end
function Fault()
    local callback
    callback = function(self, nearby)
        self.Cancel_Event_Object_In_Range(callback)
        nearby.Garrison(self)
    end
    Object.Event_Object_In_Range(callback, 50)
    Object.Service_Wrapper()
end
)lua";
    std::string baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auth::ModuleManifest manifest;
        expect(manifest.add("PROX.LUA", module).has_value(), "L-32: proximity module loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) return;
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
        foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
        expect(errors.empty(), "L-32: engine methods register");
        for (unsigned id : {1U, 2U}) expect(scripts.value().create_instance(id, "PROX.LUA").has_value(), "L-32: isolated instance starts");
        std::uint64_t sequence = 0;
        const auto call = [&](unsigned id, std::string name, auth::ValueList arguments = {}) {
            auth::ScriptEvent event;
            event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, id, sequence++};
            event.target = id; event.kind = auth::ScriptEvent::Kind::call; event.name = std::move(name);
            event.arguments = std::move(arguments);
            expect(scripts.value().submit_event(std::move(event)).has_value(), "L-32: call queues");
        };
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto service = [&] {
            const auto report = scripts.value().service(executor);
            expect(report && report.value().diagnostics.empty(), "L-32: calls have no diagnostic");
        };
        const auto number = [&](unsigned id, const char* name, int expected) {
            const auto value = scripts.value().read_global(id, name);
            expect(value && value.value() && std::get_if<ai::Real>(&value.value()->data) != nullptr &&
                std::get<ai::Real>(value.value()->data) == ai::Real(expected), "L-32: " + std::string(name));
        };
        call(1, "Setup", {foc::detail::handle(foc::handle_game_object, 10), foc::detail::handle(foc::handle_player, 2),
            foc::detail::handle(foc::handle_type, 200)});
        call(2, "Setup", {foc::detail::handle(foc::handle_game_object, 10), foc::detail::handle(foc::handle_type, 200),
            foc::detail::handle(foc::handle_player, 2)});
        service(); number(1, "count", 0);
        call(1, "Repeat"); service(); number(1, "count", 2); number(1, "order", 23); number(2, "count", 0);
        const auto saved = scripts.value().save(executor);
        expect(saved.has_value(), "L-32: retained closure saves");
        if (saved) {
            expect(scripts.value().load(saved.value()).has_value(), "L-32: retained closure restores");
            if (workers == 1) {
                // Save v3 proximity record: object count, handle, generation, handler count.
                std::string prefix;
                const auto append = [&](std::uint64_t value, unsigned width) {
                    for (unsigned byte = 0; byte < width; ++byte)
                        prefix.push_back(static_cast<char>((value >> (8 * byte)) & 0xffU));
                };
                append(1, 4); append(foc::handle_game_object, 4); append(10, 8);
                append(1, 8); append(1, 4);
                std::string corrupt = saved.value();
                const auto record = corrupt.find(prefix);
                expect(record != std::string::npos, "L-32: saved proximity record found");
                if (record != std::string::npos) {
                    const auto before = scripts.value().state_hash();
                    corrupt.replace(record + prefix.size(), 8, 8, '\0');
                    const auto refused = scripts.value().load(corrupt);
                    expect(!refused && refused.error().code == auth::codes::load_rejected &&
                        refused.error().message.find("proximity handler ID") != std::string::npos,
                        "L-32: zero proximity handler ID is rejected");
                    const auto after = scripts.value().state_hash();
                    expect(before && after && before.value() == after.value(),
                        "L-32: rejected proximity save leaves scheduler unchanged");
                }
            }
        }
        call(1, "Repeat"); call(2, "Repeat"); service(); number(1, "count", 4); number(2, "count", 2);
        call(1, "Cancel"); service(); number(1, "count", 4);
        call(2, "Repeat"); service(); number(2, "count", 4);
        call(2, "Coroutine"); service(); number(2, "count", 6);
        call(1, "Mutation"); service(); number(1, "count", 114);
        call(1, "Marker"); service(); number(1, "count", 115);
        call(1, "Spawn"); call(1, "Artillery"); service();
        for (const char* name : {"spawned", "enemies_in_range", "enemies_near"}) {
            const auto value = scripts.value().read_global(1, name);
            expect(value && value.value() && std::get_if<bool>(&value.value()->data) && std::get<bool>(value.value()->data),
                "L-32: stock consumer " + std::string(name));
        }
        call(1, "Bad"); call(1, "Escape");
        const auto rejected = scripts.value().service(executor);
        expect(rejected && rejected.value().diagnostics.size() == 2, "L-32: wrong handler and function escape diagnose");
        if (rejected && rejected.value().diagnostics.size() == 2)
            expect(rejected.value().diagnostics[0].message.find("proximity handler must be a function") != std::string::npos &&
                rejected.value().diagnostics[0].message.find("cross-instance") == std::string::npos,
                "L-32: non-function handler has its own diagnostic");
        call(1, "Fault");
        const auto fault = scripts.value().service(executor);
        expect(fault && fault.value().diagnostics.size() == 1 && fault.value().diagnostics[0].code == auth::codes::missing_api,
            "L-16: proximity callback preserves missing API classification");
        const auto hash = scripts.value().state_hash(executor);
        expect(hash.has_value(), "L-32: state hashes");
        if (hash) { if (baseline.empty()) baseline = hash.value(); else expect(hash.value() == baseline, "L-32: 1/2/4/8 state equality"); }
    }
}


void reveal_all_contracts() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, 1}, {2, 2, 200, 1}, {3, 1, 300, 1}, {4, 4, 400, 0}};
    setup.units = {{10, 100, 1, {fixed(50), fixed(-50), {}}},
        {20, 200, 2, {fixed(450), fixed(-450), {}}}};
    t::FogRules rules{{}, {}, fixed(100), 6, 6};
    // Partition failure cannot publish partially assigned rows or the persistent hold.
    struct FailingExecutor final : eawr::sim::PartitionExecutor {
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(std::size_t count, const std::function<void(std::size_t)>& partition) const override {
            for (std::size_t index = 0; index < count / 2; ++index) partition(index);
            return eawr::core::Result<void>::failure(foc::detail::make_error(t::diagnostic_codes::worker_failure, "fixture failure"));
        }
    };
    t::FogCells fog_fixture(rules, setup.players);
    std::vector<std::uint8_t> before, after;
    fog_fixture.append_state(before);
    expect(!fog_fixture.reveal_all(1, FailingExecutor{}), "V-20: reveal reports partition failure");
    fog_fixture.append_state(after);
    expect(before == after, "V-20: failed reveal leaves canonical cells and future holds unchanged");
    expect(fog_fixture.reveal_all(1, eawr::sim::InlineExecutor{}).has_value(), "V-20: reveal succeeds after failure");
    expect(fog_fixture.copied_grid_bytes() == rules.cells_wide, "V-20: whole-map reveal allocates only one row of cell bytes");
    after.clear(); fog_fixture.append_state(after);
    expect(fog_fixture.reveal_all(1, eawr::sim::InlineExecutor{}).has_value(), "V-20: repeated reveal succeeds");
    before.clear(); fog_fixture.append_state(before);
    expect(before == after, "V-20: repeated reveal is idempotent");
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto made = t::TacticalSession::create(setup, {}, {}, {}, rules);
        expect(made.has_value(), "V-20: fog world starts");
        if (!made) return;
        auto host = ai::make_tactical_host();
        host->world = &made.value();
        host->view = foc::detail::build_view(made.value(), *made.value().snapshot());
        auth::ModuleManifest manifest;
        expect(manifest.add("BURN.LUA", "function Firesale(player)\n"
            " local result = FogOfWar.Reveal_All(player, 'extra argument')\n"
            " if result ~= nil then error('expected nil') end\n"
            " continued = true\n"
            "end\n").has_value(), "V-20: narrow firesale continuation module loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "V-20: scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "BURN.LUA").has_value(), "V-20: binding registers");
        for (const auto& [tick, recipient] : {std::pair{1U, 2U}, std::pair{2U, 1U}, std::pair{3U, 4U}}) {
            auth::ScriptEvent event;
            event.key = {tick, auth::first_simulation_producer, 1, tick};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Firesale";
            event.arguments = {auth::Value{auth::Handle{foc::handle_player, recipient}}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "V-20: AI/local/neutral reveal events queue");
        }
        auto linked = auth::ScriptedTacticalSession::create(std::move(made).value(), std::move(scripts).value());
        if (!linked) { expect(false, "V-20: scripted world links"); return; }
        auto& session = linked.value();
        expect(session.register_verb(foc::verb_reveal_all, foc::tactical_ai_detail::translate_order).has_value(),
            "V-20: reveal uses production command router");
        const auto previous_rows = session.world().fog_cells()->value_rows(1);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < 224; ++tick) {
            host->world = &session.world();
            host->view = foc::detail::build_view(session.world(), *session.world().snapshot());
            const auto stepped = session.step(executor);
            expect(stepped.has_value(), "V-20: firesale world steps");
            if (!stepped) return;
            expect(stepped.value().scripts.diagnostics.empty(), "V-20: supported call does not abort its thread");
            for (const auto& input : stepped.value().script_input)
                expect(input.submitted, "V-20: script reveal reaches replay queue");
            hashes.push_back(stepped.value().world.state_sha256);
            const auto* fog = session.world().fog_cells();
            const auto full = [&](std::size_t player, std::uint8_t value) {
                const auto cells = fog->values(player);
                return std::all_of(cells.begin(), cells.end(), [&](std::uint8_t cell) { return cell == value; });
            };
            expect(full(2, 0), "V-20: reveal does not leak to an allied observer");
            if (tick == 0) expect(full(0, 0) && full(1, 0), "V-20: call changes only the next tick");
            if (tick == 1) {
                expect(full(1, 255) && full(0, 0), "V-20: AI reveal leaves the local observer fogged");
                expect((session.world().snapshot()->instances()[0].visible_to & 2U) != 0,
                    "V-20: revealed enemy is present in recipient visibility");
            }
            if (tick >= 2) expect(full(0, 255) && full(1, 255), "V-20: explicit local reveal persists through refresh");
            if (tick >= 3) expect(full(3, 255), "V-20: non-commandable recipient has its own revealed grid");
        }
        expect((*previous_rows[0])[0] == 0, "V-20: previous fog rows remain immutable");
        const auto recorded = session.record();
        expect(recorded.commands.size() == 3, "V-20: AI/local/neutral reveals each record one command");
        const auto encoded = t::write_replay(recorded);
        expect(encoded.has_value(), "V-20: reveal replay encodes");
        if (!encoded) return;
        const auto parsed = t::parse_replay(encoded.value());
        expect(parsed.has_value() && parsed.value().commands == recorded.commands, "V-20: replay preserves recipient and issuer");
        if (!parsed) return;
        auto replayed = t::TacticalSession::from_replay(parsed.value(), {}, {}, {}, rules);
        expect(replayed.has_value(), "V-20: recorded reveals replay without Lua");
        if (!replayed) return;
        for (std::size_t tick = 0; tick < hashes.size(); ++tick) {
            auto stepped = replayed.value().step(executor);
            expect(stepped && stepped.value().state_sha256 == hashes[tick], "V-20: replay reproduces every world hash");
        }
        auto malformed = encoded.value();
        malformed[malformed.size() - 12] = 1; // last command's reserved payload word
        expect(!t::parse_replay(malformed), "V-20: nonzero reveal reserved field is rejected");
        auto invalid = recorded;
        std::get<t::RevealAllPayload>(invalid.commands[0].payload).player = 99;
        expect(!t::write_replay(invalid), "V-20: undeclared recipient is rejected");
        invalid = recorded; invalid.commands[0].units = {10};
        expect(!t::write_replay(invalid), "V-20: reveal with unit orders is rejected");
        if (workers == 1) baseline = hashes;
        else expect(hashes == baseline, "V-20: canonical reveal agrees on 1/2/4/8 workers");
    }
    // Invalid arguments and a non-tactical host retain script errors, never missing-API errors.
    for (const auto& test : {std::pair{true, std::string{}}, std::pair{true, std::string{"nil"}},
            std::pair{true, std::string{"123"}}, std::pair{false, std::string{}}}) {
        auto host = ai::make_tactical_host();
        if (test.first) host->view = std::make_shared<foc::detail::WorldView>();
        auth::ModuleManifest manifest;
        expect(manifest.add("BAD.LUA", "function Check() FogOfWar.Reveal_All(" + test.second + ") end\n").has_value(),
            "V-20: bad-argument module loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) return;
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "BAD.LUA").has_value(), "V-20: bad-argument binding registers");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0}; event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        expect(scripts.value().submit_event(std::move(event)).has_value(), "V-20: invalid call queues");
        const auto report = scripts.value().service(eawr::sim::InlineExecutor{});
        expect(report && report.value().commands.empty() && !report.value().diagnostics.empty(), "V-20: invalid call reports and issues no reveal");
        if (report) for (const auto& error : report.value().diagnostics) {
            expect(error.code != auth::codes::missing_api, "V-20: genuine argument error stays distinct from unsupported API");
            expect(error.message.find(test.first ? (test.second.empty() ? "requires a player" : "expected a player")
                : "only valid in a tactical game") != std::string::npos, "V-20: argument/mode error explains the rejected call");
        }
    }
}


void faction_controller_inventory() {
    // WSS-35 / GS-02: assigning a faction's controller is insufficient unless
    // its definition and space function set also reach the goal engine.
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
        return std::nullopt;
    };
    std::map<std::string, std::string, std::less<>> xml;
    for (const auto& path : ai::ai_xml_files()) xml.emplace(path, "<Root></Root>");
    const std::string player_path = "data/xml/ai/players/ai_player_underworld.xml";
    const std::string set_path = "data/xml/ai/goalfunctions/ai_goalset_underworld_space.xml";
    expect(xml.contains(player_path) && xml.contains(set_path), "WSS-35: the Underworld player and space set are loaded");
    xml[player_path] = "<AIPlayerType><Name>AI_Player_Underworld</Name>"
        "<GoalProposalFunctionSets>AI_GoalSet_Underworld_Space</GoalProposalFunctionSets>"
        "<Templates><Space>Test_Space</Space></Templates></AIPlayerType>";
    xml[set_path] = "<FunctionSet><Probe><Goal>Probe</Goal><Function>ReallyBig</Function></Probe></FunctionSet>";
    const auto controller = ai::load_ai_data(xml, converters);
    expect(controller && controller.value().players.contains("AI_PLAYER_UNDERWORLD")
        && controller.value().players.at("AI_PLAYER_UNDERWORLD").function_sets == std::vector<std::string>{"AI_GOALSET_UNDERWORLD_SPACE"}
        && controller.value().players.at("AI_PLAYER_UNDERWORLD").space_templates == std::vector<std::string>{"TEST_SPACE"}
        && controller.value().function_sets.at("AI_GOALSET_UNDERWORLD_SPACE").size() == 1,
        "WSS-35 / GS-02: the Underworld goal engine has its authored controller and space functions");
}




// The installed bytes of every module the setup's admitted object scripts need.
std::optional<auth::ModuleManifest> stock_manifest(const std::filesystem::path& root, const foc::AiSetup& setup) {
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto folder : {"corruption", "GameData"}) {
        auto mounted = eawr::vfs::resolve_manifest_mount(folder, root / folder / "Data");
        if (!mounted) { expect(false, "SOS: installed manifest mount"); return std::nullopt; }
        mounts.push_back(std::move(mounted).value().mount);
    }
    auto files = eawr::vfs::Vfs::mount(mounts);
    if (!files) { expect(false, "SOS: installed VFS"); return std::nullopt; }
    auth::ModuleManifest manifest;
    for (const auto& path : foc::required_modules(setup)) {
        auto bytes = files.value().open(path);
        if (!bytes) { expect(false, "SOS: installed module " + path); return std::nullopt; }
        std::string source(bytes.value().size(), '\0');
        std::transform(bytes.value().begin(), bytes.value().end(), source.begin(), [](std::byte b) { return static_cast<char>(b); });
        expect(manifest.add(path, std::move(source)).has_value(), "SOS: logical stock manifest");
    }
    return manifest;
}


// Installed stock chunks on synthetic immutable inputs: an executable SOS-15/19
// witness, independent of the manually authored fixture scripts below.
void stock_object_scripts(const std::filesystem::path& root) {
    foc::AiSetup setup;
    foc::AiType falcon; falcon.type_id = 100; falcon.lua_script = "ObjectScript_MillenniumFalcon";
    foc::AiType vader; vader.type_id = 101; vader.lua_script = "ObjectScript_VaderTIESquadron";
    setup.content.types = {falcon, vader};
    auto stock = stock_manifest(root, setup);
    if (!stock) return;
    const auto& manifest = *stock;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto host = std::make_shared<foc::detail::Host>(); host->setup = setup;
        host->setup.players = {{1, "REBEL", false, true, {}, false}, {2, "REBEL", false, false, {}, true}};
        for (const auto& type : host->setup.content.types) host->types.emplace(type.type_id, &type);
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        auto view = std::make_shared<foc::detail::WorldView>(); host->view = view;
        const auto fixed = [](int n) { return sim::math::Fixed::from_integer(n).value(); };
        for (const auto id : {10U, 11U}) {
            foc::detail::ViewUnit unit; unit.id = id; unit.type = 100; unit.owner = id == 10 ? 1U : 2U;
            unit.hull = fixed(40); unit.max_hull = fixed(100); unit.shield = Real(1);
            unit.abilities = {{tactical::AbilityKind::invulnerability, false, true, true}};
            view->units.push_back(unit);
        }
        foc::detail::ViewUnit enemy; enemy.id = 20; enemy.owner = 3; view->units.push_back(enemy);
        foc::detail::ViewUnit container; container.id = 30; container.type = 101; container.owner = 1;
        container.container = true; container.members = {31}; view->units.push_back(container);
        foc::detail::ViewUnit leader; leader.id = 31; leader.owner = 1;
        leader.abilities = {{tactical::AbilityKind::replenish_wingmen, false, true, true}}; view->units.push_back(leader);
        container.id = 40; container.members = {41}; view->units.push_back(container);
        leader.id = 41; leader.abilities.clear(); view->units.push_back(leader);
        ai::Engine engine(host, {}, {}); host->engine = &engine;
        ai::EngineContractAccess::threat(engine, 10, 20, 1); ai::EngineContractAccess::threat(engine, 11, 20, 1);
        auth::SessionConfig config; config.tick_duration = {1, 30}; config.script_directories = {"Data/Scripts/Library/"};
        auto created = auth::ScriptScheduler::create(config, manifest);
        if (!created) { expect(false, "SOS: stock scheduler starts"); return; }
        auto scripts = std::move(created).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, host, errors); foc::tactical_ai_detail::register_methods(scripts, host, errors);
        expect(errors.empty(), "SOS: real bindings for installed stock");
        std::array<std::uint32_t, 3> uses{};
        for (std::uint64_t tick = 1; tick <= 100; ++tick) {
            view->tick = tick;
            // Human is still above its 0.05 boundary while AI is below 0.2.
            if (tick == 35) for (auto& unit : view->units) if (unit.type == 100) unit.shield = Real(1) / Real(10);
            if (tick == 68) for (auto& unit : view->units) if (unit.type == 100) unit.shield = Real(1) / Real(100);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            auto prepared = foc::tactical_ai_detail::service_objects(executor, host, scripts, sequence, diagnostics, active, pumps);
            expect(prepared && diagnostics.empty() && active == 4 && pumps <= active, "SOS-42: bounded stock work counts");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) { expect(false, "SOS: stock service"); return; }
            for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.code << ' ' << diagnostic.message << '\n';
            expect(report.value().diagnostics.empty(), "SOS: installed scripts have no diagnostic");
            for (const auto& command : report.value().commands) {
                const auto id = command.issuer - foc::first_object_instance;
                expect(command.verb == foc::verb_ability && id != 40, "SOS-19: inherited script cannot grant absent ability");
                if (id == 10) { ++uses[0]; expect(tick >= 35, "SOS-15: AI above threshold holds"); }
                if (id == 11) { ++uses[1]; expect(tick >= 68, "SOS-15: human stricter threshold holds"); }
                if (id == 30) ++uses[2];
            }
            auto hash = scripts.state_hash(executor);
            if (!hash) { expect(false, "SOS: stock canonical hash"); return; }
            if (workers == 1) reference.push_back(hash.value());
            else expect(hash.value() == reference[static_cast<std::size_t>(tick-1)], "SOS: stock 1/2/4/8 equality");
        }
        expect(uses[0] > 0 && uses[1] > 0 && uses[2] > 0, "SOS-15/19: installed AI/human Falcon and Vader visibly fire");
        std::cout << workers << " workers: stock Falcon AI=" << uses[0] << " human=" << uses[1] << " Vader=" << uses[2] << '\n';
    }
}


// SOS-16 scene: Slave_I (102) for AI player 1 at the origin and for human player 2
// far away; enemy player 3 has a squadron container (103, rating 600) whose two
// craft (104, rating 300 each) sit within 200, and ships (105, rating 500).
struct SlaveScene {
    foc::AiSetup setup;
    std::shared_ptr<foc::detail::Host> host;
    std::shared_ptr<foc::detail::WorldView> view;
};


SlaveScene slave_scene(const foc::AiSetup& setup) {
    SlaveScene scene;
    scene.setup = setup;
    scene.host = std::make_shared<foc::detail::Host>();
    scene.host->setup = setup;
    scene.host->setup.players = {{1, "EMPIRE", false, true, {}, false}, {2, "EMPIRE", false, false, {}, true},
        {3, "REBEL", false, true, {}, false}};
    for (const auto& type : scene.host->setup.content.types) scene.host->types.emplace(type.type_id, &type);
    for (const auto& player : scene.host->setup.players) scene.host->players.emplace(player.player, &player);
    scene.view = std::make_shared<foc::detail::WorldView>();
    scene.view->players = {{1, 1, 0}, {2, 1, 1}, {3, 2, 2}};
    scene.host->view = scene.view;
    const auto at = [](int x, int y) {
        return sim::math::Vec3{sim::math::Fixed::from_integer(x).value(), sim::math::Fixed::from_integer(y).value(), {}};
    };
    const auto unit = [&](sim::EntityId id, tactical::TypeId type, tactical::PlayerId owner, sim::math::Vec3 position) {
        foc::detail::ViewUnit out; out.id = id; out.type = type; out.owner = owner; out.position = position;
        return out;
    };
    auto ai_slave = unit(50, 102, 1, at(0, 0));
    ai_slave.abilities = {{tactical::AbilityKind::harmonic_bomb, false, true, false}};
    auto human_slave = unit(51, 102, 2, at(5000, 0));
    human_slave.abilities = {{tactical::AbilityKind::harmonic_bomb, false, true, true}};
    auto team = unit(60, 103, 3, at(100, 0)); team.container = true; team.members = {61, 62};
    auto first = unit(61, 104, 3, at(100, 10)); first.craft = true; first.squadron = 60;
    auto second = unit(62, 104, 3, at(100, -10)); second.craft = true; second.squadron = 60;
    scene.view->units = {ai_slave, human_slave, team, first, second, unit(80, 105, 3, at(5100, 0)), unit(81, 105, 3, at(5100, 50))};
    return scene;
}


// Tick 40 adds a ship beside the AI Slave I; tick 70 adds the human's third contact.
void slave_scene_arrivals(foc::detail::WorldView& view, std::uint64_t tick) {
    const auto add = [&](sim::EntityId id, int x, int y) {
        foc::detail::ViewUnit out; out.id = id; out.type = 105; out.owner = 3;
        out.position = {sim::math::Fixed::from_integer(x).value(), sim::math::Fixed::from_integer(y).value(), {}};
        view.units.push_back(out);
        std::sort(view.units.begin(), view.units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    };
    if (tick == 40) add(70, 150, 0);
    if (tick == 70) add(82, 5100, -50);
}


foc::AiSetup slave_setup() {
    foc::AiSetup setup;
    foc::AiType slave; slave.type_id = 102; slave.lua_script = "Slave_I";
    foc::AiType squadron; squadron.type_id = 103; squadron.combat_power = Real(600);
    foc::AiType craft; craft.type_id = 104; craft.combat_power = Real(300);
    foc::AiType ship; ship.type_id = 105; ship.combat_power = Real(500);
    setup.content.types = {slave, squadron, craft, ship};
    return setup;
}


// SOS-16: installed Slave_I promotes craft to their team, counts each parent once
// and sums type combat ratings within 200. AI bombs at threat >= 1000 (team 600
// alone stays below; promotion-free counting would reach 1200 at once); an armed
// human needs more than two counted units.
void stock_slave_i(const std::filesystem::path& root) {
    const auto setup = slave_setup();
    auto stock = stock_manifest(root, setup);
    if (!stock) return;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = slave_scene(setup);
        auth::SessionConfig config; config.tick_duration = {1, 30}; config.script_directories = {"Data/Scripts/Library/"};
        auto created = auth::ScriptScheduler::create(config, *stock);
        if (!created) { expect(false, "SOS-16: stock scheduler starts"); return; }
        auto scripts = std::move(created).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
        foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
        expect(errors.empty(), "SOS-16: real bindings for installed stock");
        std::array<std::uint32_t, 2> uses{};
        for (std::uint64_t tick = 1; tick <= 160; ++tick) {
            scene.view->tick = tick;
            slave_scene_arrivals(*scene.view, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            auto prepared = foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps);
            expect(prepared && diagnostics.empty() && active == 2 && pumps <= active, "SOS-42: bounded Slave I work counts");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) { expect(false, "SOS-16: stock service"); return; }
            for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.code << ' ' << diagnostic.message << '\n';
            expect(report.value().diagnostics.empty(), "SOS-16: installed Slave_I has no diagnostic");
            for (const auto& command : report.value().commands) {
                const auto id = command.issuer - foc::first_object_instance;
                expect(command.verb == foc::verb_ability && command.arguments.size() == 4
                    && std::get<Real>(command.arguments[2].data) == Real(static_cast<std::int64_t>(tactical::AbilityKind::harmonic_bomb)),
                    "SOS-16: Slave I requests only its harmonic bomb");
                if (id == 50) { ++uses[0]; expect(tick > 40, "SOS-16: one team parent (600) stays below the AI threshold"); }
                if (id == 51) { ++uses[1]; expect(tick > 70, "SOS-16: two contacts do not arm the human bomb"); }
            }
            auto hash = scripts.state_hash(executor);
            if (!hash) { expect(false, "SOS-16: stock canonical hash"); return; }
            if (workers == 1) reference.push_back(hash.value());
            else expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-16: stock 1/2/4/8 equality");
        }
        expect(uses[0] > 0 && uses[1] > 0, "SOS-16: installed AI and armed human Slave I visibly bomb");
        std::cout << workers << " workers: stock Slave_I AI=" << uses[0] << " human=" << uses[1] << '\n';
    }
}


// SOS-16/43 synthetic contract on the production bindings: parent promotion,
// handle-keyed deduplication, type combat rating, and the sample set across a
// save/restore barrier.
void slave_i_contracts() {
    const std::string module = R"LUA(
function Base_Definitions() ServiceRate = 1; threat = 0; seen = {} end
function Unit_Prox(self_obj, trigger_obj)
    if not trigger_obj.Get_Owner().Is_Enemy(Object.Get_Owner()) then return end
    if trigger_obj.Get_Parent_Object() then trigger_obj = trigger_obj.Get_Parent_Object() end
    if seen[trigger_obj] == nil then
        seen[trigger_obj] = trigger_obj
        threat = threat + trigger_obj.Get_Type().Get_Combat_Rating()
    end
end
function main()
    Object.Event_Object_In_Range(Unit_Prox, 200)
    while true do
        if threat >= 1000 and Object.Is_Ability_Ready('HARMONIC_BOMB') then Object.Activate_Ability('HARMONIC_BOMB', true) end
        sampled = threat; threat = 0; seen = {}
        Object.Service_Wrapper()
        coroutine.yield(true)
    end
end
)LUA";
    const auto setup = slave_setup();
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = slave_scene(setup);
        const auto make_scripts = [&]() {
            auth::ModuleManifest modules;
            expect(modules.add("Data/Scripts/GameObject/SLAVE_I.lua", module).has_value(), "SOS-16: Slave I module");
            auth::SessionConfig config; config.tick_duration = {1, 30};
            auto created = auth::ScriptScheduler::create(config, std::move(modules));
            if (!created) throw std::runtime_error(created.error().message);
            auto scripts = std::move(created).value();
            std::vector<eawr::core::Diagnostic> errors;
            foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
            foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
            expect(errors.empty(), "SOS-16: production bindings");
            return scripts;
        };
        auto scripts = make_scripts();
        const auto sampled = [&]() {
            auto value = scripts.read_global(foc::first_object_instance + 50, "sampled");
            const auto* number = value && value.value() ? std::get_if<Real>(&value.value()->data) : nullptr;
            return number != nullptr ? *number : Real(-1);
        };
        std::uint32_t uses = 0;
        bool restored = false;
        for (std::uint64_t tick = 1; tick <= 120; ++tick) {
            scene.view->tick = tick;
            slave_scene_arrivals(*scene.view, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            expect(foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps).has_value()
                && diagnostics.empty(), "SOS-16: partitioned attachment and admission");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) throw std::runtime_error(report.error().message);
            expect(report.value().diagnostics.empty(), "SOS-16: no binding diagnostics");
            for (const auto& command : report.value().commands) {
                if (command.issuer != foc::first_object_instance + 50) continue;
                ++uses;
                expect(tick > 40, "SOS-16: promoted team counts once below the threshold");
            }
            // Before the ship arrives the sample is the team once (600), never its craft.
            if (tick < 40) expect(sampled() == Real(-1) || sampled() == Real(0) || sampled() == Real(600),
                "SOS-16: craft promote to one team parent with its type rating");
            if (tick > 40 && sampled() == Real(1100) && !restored) {
                // SOS-43: the handle-keyed sample and callback survive a restore.
                auto before = scripts.save(executor);
                if (!before) throw std::runtime_error(before.error().message);
                auto fresh = make_scripts();
                expect(fresh.load(before.value()).has_value(), "SOS-43: Slave I restore");
                auto after = fresh.save();
                expect(after && after.value() == before.value(), "SOS-43: Slave I canonical restore bytes");
                scripts = std::move(fresh);
                restored = true;
            }
            auto hash = scripts.state_hash(executor);
            expect(hash.has_value(), "SOS-16: state hashes");
            if (workers == 1 && hash) reference.push_back(hash.value());
            else if (hash) expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-16: 1/2/4/8 equality");
        }
        expect(restored && uses > 0, "SOS-16: team plus ship (600 + 500) bombs after a restore");
    }
}


// SOS-17 scene: Sundered_Heart (106) for AI player 1 at the origin and for human
// player 2 far away; enemy ships (105, rating 500) for player 3. Two enemies sit
// near each Sundered Heart; tick 40 adds a third near each.
struct SunderedScene {
    std::shared_ptr<foc::detail::Host> host;
    std::shared_ptr<foc::detail::WorldView> view;
};


foc::AiSetup sundered_setup() {
    foc::AiSetup setup;
    foc::AiType heart; heart.type_id = 106; heart.lua_script = "Sundered_Heart";
    foc::AiType ship; ship.type_id = 105; ship.combat_power = Real(500);
    setup.content.types = {heart, ship};
    return setup;
}


foc::detail::ViewUnit sundered_unit(sim::EntityId id, tactical::TypeId type, tactical::PlayerId owner, int x, int y) {
    foc::detail::ViewUnit out; out.id = id; out.type = type; out.owner = owner;
    out.position = {sim::math::Fixed::from_integer(x).value(), sim::math::Fixed::from_integer(y).value(), {}};
    return out;
}


SunderedScene sundered_scene(const foc::AiSetup& setup) {
    SunderedScene scene;
    scene.host = std::make_shared<foc::detail::Host>();
    scene.host->setup = setup;
    scene.host->setup.players = {{1, "REBEL", false, true, {}, false}, {2, "REBEL", false, false, {}, true},
        {3, "EMPIRE", false, true, {}, false}};
    for (const auto& type : scene.host->setup.content.types) scene.host->types.emplace(type.type_id, &type);
    for (const auto& player : scene.host->setup.players) scene.host->players.emplace(player.player, &player);
    scene.view = std::make_shared<foc::detail::WorldView>();
    scene.view->players = {{1, 1, 0}, {2, 1, 1}, {3, 2, 2}};
    scene.host->view = scene.view;
    auto ai_heart = sundered_unit(50, 106, 1, 0, 0);
    ai_heart.abilities = {{tactical::AbilityKind::weaken_enemy, false, true, false}};
    auto human_heart = sundered_unit(51, 106, 2, 5000, 0);
    human_heart.abilities = {{tactical::AbilityKind::weaken_enemy, false, true, true}};
    scene.view->units = {ai_heart, human_heart, sundered_unit(70, 105, 3, 400, 0), sundered_unit(71, 105, 3, 450, 50),
        sundered_unit(80, 105, 3, 5400, 0), sundered_unit(81, 105, 3, 5450, 50)};
    return scene;
}


void sundered_arrivals(foc::detail::WorldView& view, std::uint64_t tick) {
    if (tick != 40) return;
    view.units.push_back(sundered_unit(72, 105, 3, 450, -50));
    view.units.push_back(sundered_unit(82, 105, 3, 5450, -50));
    std::sort(view.units.begin(), view.units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
}


// SOS-17: installed Sundered_Heart exits for a human owner; its AI samples enemies
// within 1000 and, from the third distinct contact, weakens the best local threat
// center (radius 300) above threat 1000, at that point.
void stock_sundered_heart(const std::filesystem::path& root) {
    const auto setup = sundered_setup();
    auto stock = stock_manifest(root, setup);
    if (!stock) return;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = sundered_scene(setup);
        auth::SessionConfig config; config.tick_duration = {1, 30};
        config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/GameObject/"};
        auto created = auth::ScriptScheduler::create(config, *stock);
        if (!created) { expect(false, "SOS-17: stock scheduler starts"); return; }
        auto scripts = std::move(created).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
        foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
        expect(errors.empty(), "SOS-17: real bindings for installed stock");
        std::uint32_t uses = 0;
        for (std::uint64_t tick = 1; tick <= 160; ++tick) {
            scene.view->tick = tick;
            sundered_arrivals(*scene.view, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            auto prepared = foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps);
            expect(prepared && diagnostics.empty() && pumps <= active, "SOS-42: bounded Sundered Heart work counts");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) { expect(false, "SOS-17: stock service"); return; }
            for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.code << ' ' << diagnostic.message << '\n';
            expect(report.value().diagnostics.empty(), "SOS-17: installed Sundered_Heart has no diagnostic");
            for (const auto& command : report.value().commands) {
                const auto id = command.issuer - foc::first_object_instance;
                expect(id == 50, "SOS-17: a human-owned Sundered Heart exits");
                expect(tick > 40, "SOS-17: two contacts do not weaken");
                expect(command.verb == foc::verb_ability && command.arguments.size() == 7
                    && std::get<Real>(command.arguments[2].data) == Real(static_cast<std::int64_t>(tactical::AbilityKind::weaken_enemy)),
                    "SOS-17: Weaken Enemy at a point");
                // Three ships (<= 4) give their mean planar position.
                expect(command.arguments.size() == 7 && std::get_if<Real>(&command.arguments[4].data) != nullptr
                    && std::get<Real>(command.arguments[4].data) == ai::to_single(Real(1300) / Real(3))
                    && std::get_if<Real>(&command.arguments[5].data) != nullptr
                    && std::get<Real>(command.arguments[5].data) == Real(0), "WHE-68: weakened at the cluster mean");
                auto order = foc::tactical_ai_detail::translate_order(command);
                const auto* payload = order ? std::get_if<tactical::AbilityPayload>(&order.value().payload) : nullptr;
                expect(payload != nullptr && payload->ability == tactical::AbilityKind::weaken_enemy && payload->position,
                    "WHE-61: the order carries the weaken point");
                ++uses;
            }
            auto hash = scripts.state_hash(executor);
            if (!hash) { expect(false, "SOS-17: stock canonical hash"); return; }
            if (workers == 1) reference.push_back(hash.value());
            else expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-17: stock 1/2/4/8 equality");
        }
        expect(uses > 0, "SOS-17: installed AI Sundered Heart visibly weakens");
        std::cout << workers << " workers: stock Sundered_Heart AI=" << uses << '\n';
    }
}


// SOS-17/43, WHE-68 synthetic contract on the production bindings: an object-keyed
// sample crosses the boundary, the position wrapper is valid, answers its point
// and round-trips bit-exactly through save/load, and positions Weaken Enemy.
void sundered_contracts() {
    for (const std::uint32_t bits : {0x00000000U, 0x80000000U, 0x00000001U, 0x80000001U, 0x00400001U, 0x007fffffU,
             0x00800000U, 0x3f800000U, 0x43d8aaabU, 0xc479fff0U, 0x7f7fffffU, 0x7f800000U, 0xff800000U}) {
        expect(ai::binary32_bits(ai::binary32_value(bits)) == bits, "position wrapper: binary32 bits round-trip");
    }
    expect(ai::binary32_value(0x3f800000U) == Real(1), "position wrapper: 1.0 decodes exactly");
    for (const Real value : {Real(1) / Real(3), Real(1300) / Real(3), Real(-7) / Real(10), Real(123456789)}) {
        expect(ai::binary32_value(ai::binary32_bits(value)) == ai::to_single(value), "position wrapper: single rounding");
    }
    const std::string module = R"LUA(
function Base_Definitions() ServiceRate = 1; seen = {}; count = 0; fired = 0 end
function Unit_Prox(self_obj, trigger_obj)
    if not trigger_obj.Get_Owner().Is_Enemy(Object.Get_Owner()) then return end
    if seen[trigger_obj] == nil then
        seen[trigger_obj] = trigger_obj
        count = count + 1
        if count >= 3 and Object.Is_Ability_Ready('WEAKEN_ENEMY') then
            local pos, threat = Find_Best_Local_Threat_Center(seen, 300)
            if pos ~= nil and pos.Is_Valid() and threat > 1000 then
                held = pos; held_threat = threat
                held_x, held_y, held_z = pos.Get_XYZ()
                Object.Activate_Ability('WEAKEN_ENEMY', pos)
                fired = fired + 1
            end
        end
    end
end
function main()
    Object.Event_Object_In_Range(Unit_Prox, 1000)
    while true do
        count = 0; seen = {}
        Object.Service_Wrapper()
        coroutine.yield(true)
    end
end
)LUA";
    const auto setup = sundered_setup();
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = sundered_scene(setup);
        const auto make_scripts = [&]() {
            auth::ModuleManifest modules;
            expect(modules.add("Data/Scripts/GameObject/SUNDERED_HEART.lua", module).has_value(), "SOS-17: module");
            auth::SessionConfig config; config.tick_duration = {1, 30};
            auto created = auth::ScriptScheduler::create(config, std::move(modules));
            if (!created) throw std::runtime_error(created.error().message);
            auto scripts = std::move(created).value();
            std::vector<eawr::core::Diagnostic> errors;
            foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
            foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
            expect(errors.empty(), "SOS-17: production bindings");
            return scripts;
        };
        auto scripts = make_scripts();
        const auto global = [&](std::string_view name) -> std::optional<auth::Value> {
            auto value = scripts.read_global(foc::first_object_instance + 50, name);
            if (!value || !value.value() || std::holds_alternative<std::monostate>(value.value()->data)) return std::nullopt;
            return *value.value();
        };
        std::uint32_t uses = 0;
        bool restored = false;
        for (std::uint64_t tick = 1; tick <= 120; ++tick) {
            scene.view->tick = tick;
            sundered_arrivals(*scene.view, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            expect(foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps).has_value()
                && diagnostics.empty(), "SOS-17: partitioned attachment and admission");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) throw std::runtime_error(report.error().message);
            expect(report.value().diagnostics.empty(), "SOS-17: no binding diagnostics");
            for (const auto& command : report.value().commands) {
                if (command.issuer != foc::first_object_instance + 50) continue;
                ++uses;
                expect(tick > 40 && command.arguments.size() == 7, "SOS-17: the third contact weakens at a point");
            }
            if (!restored && global("held")) {
                const auto before_bytes = scripts.save(executor);
                if (!before_bytes) throw std::runtime_error(before_bytes.error().message);
                const auto held = global("held");
                const auto x = global("held_x");
                auto fresh = make_scripts();
                expect(fresh.load(before_bytes.value()).has_value(), "SOS-43: Sundered Heart restore");
                auto after = fresh.save();
                expect(after && after.value() == before_bytes.value(), "SOS-43: canonical restore bytes");
                scripts = std::move(fresh);
                const auto* wrapper = held ? std::get_if<auth::Handle>(&held->data) : nullptr;
                const auto restored_held = global("held");
                const auto* back = restored_held ? std::get_if<auth::Handle>(&restored_held->data) : nullptr;
                expect(wrapper != nullptr && back != nullptr && wrapper->kind == foc::handle_position && *wrapper == *back,
                    "SOS-43: the position wrapper round-trips exactly");
                const auto* x_number = x ? std::get_if<Real>(&x->data) : nullptr;
                expect(x_number != nullptr && *x_number == ai::to_single(Real(1300) / Real(3)), "WHE-68: wrapper answers the cluster mean");
                const auto threat = global("held_threat");
                const auto* threat_number = threat ? std::get_if<Real>(&threat->data) : nullptr;
                expect(threat_number != nullptr && *threat_number == Real(1500), "WHE-68: company power sums within the radius");
                restored = true;
            }
            auto hash = scripts.state_hash(executor);
            expect(hash.has_value(), "SOS-17: state hashes");
            if (workers == 1 && hash) reference.push_back(hash.value());
            else if (hash) expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-17: 1/2/4/8 equality");
        }
        expect(restored && uses > 0, "SOS-17: the synthetic Sundered Heart weakens");
    }
}


// SOS-11 scene: Interdictor-like ships (108) with MISSILE_SHIELD for AI player 1
// and human player 2; enemy player 3 has a laser ship (110) and a rocket ship
// (109). The AI's deadly enemy is the laser ship until tick 40, then the rocket
// ship; the human's is the rocket ship throughout and autofire arms at tick 70.
struct ShieldScene {
    std::shared_ptr<foc::detail::Host> host;
    std::shared_ptr<foc::detail::WorldView> view;
    std::unique_ptr<ai::Engine> engine;
};


foc::AiSetup shield_setup() {
    foc::AiSetup setup;
    foc::AiType cruiser; cruiser.type_id = 108; cruiser.lua_script = "ObjectScript_MissileShield";
    foc::AiType rockets; rockets.type_id = 109; rockets.object_projectile = 300;
    foc::AiType lasers; lasers.type_id = 110; lasers.object_projectile = 301;
    setup.content.types = {cruiser, rockets, lasers};
    setup.content.projectile_categories = {{300, foc::ai_projectile_rocket}, {301, 0U}};
    return setup;
}


ShieldScene shield_scene(const foc::AiSetup& setup) {
    ShieldScene scene;
    scene.host = std::make_shared<foc::detail::Host>();
    scene.host->setup = setup;
    scene.host->setup.players = {{1, "EMPIRE", false, true, {}, false}, {2, "EMPIRE", false, false, {}, true},
        {3, "REBEL", false, true, {}, false}};
    for (const auto& type : scene.host->setup.content.types) scene.host->types.emplace(type.type_id, &type);
    for (const auto& player : scene.host->setup.players) scene.host->players.emplace(player.player, &player);
    scene.view = std::make_shared<foc::detail::WorldView>();
    scene.view->players = {{1, 1, 0}, {2, 1, 1}, {3, 2, 2}};
    scene.host->view = scene.view;
    const auto unit = [](sim::EntityId id, tactical::TypeId type, tactical::PlayerId owner) {
        foc::detail::ViewUnit out; out.id = id; out.type = type; out.owner = owner; return out;
    };
    auto ai_cruiser = unit(50, 108, 1);
    ai_cruiser.abilities = {{tactical::AbilityKind::missile_shield, false, true, false}};
    auto human_cruiser = unit(51, 108, 2);
    human_cruiser.abilities = {{tactical::AbilityKind::missile_shield, false, true, false}};
    scene.view->units = {ai_cruiser, human_cruiser, unit(70, 110, 3), unit(71, 109, 3)};
    scene.engine = std::make_unique<ai::Engine>(scene.host, ai::AiData{}, std::vector<ai::PlanDef>{});
    scene.host->engine = scene.engine.get();
    ai::EngineContractAccess::threat(*scene.engine, 50, 70, 1);
    ai::EngineContractAccess::threat(*scene.engine, 51, 71, 1);
    return scene;
}


void shield_events(ShieldScene& scene, std::uint64_t tick) {
    if (tick == 40) ai::EngineContractAccess::threat(*scene.engine, 50, 71, 5);
    if (tick == 70) scene.view->units[1].abilities[0].autofire = true;
}


// Counts missile-shield requests; checks their kinds and the AI/human gates.
void shield_commands(const auth::ServiceReport& report, std::uint64_t tick, std::array<std::uint32_t, 2>& uses) {
    for (const auto& command : report.commands) {
        const auto id = command.issuer - foc::first_object_instance;
        expect(command.verb == foc::verb_ability && command.arguments.size() == 4
            && std::get<Real>(command.arguments[2].data) == Real(static_cast<std::int64_t>(tactical::AbilityKind::missile_shield)),
            "SOS-11: only MISSILE_SHIELD is requested");
        if (id == 50) { ++uses[0]; expect(tick > 40, "SOS-11: a laser-armed deadly enemy does not raise the shield"); }
        if (id == 51) { ++uses[1]; expect(tick > 70, "SOS-11: a human shield waits for autofire"); }
    }
}


// SOS-11: installed ObjectScript_MissileShield raises the shield against a deadly
// enemy that fires an affected projectile; AI owners act at once, human owners
// only with autofire.
void stock_missile_shield(const std::filesystem::path& root) {
    const auto setup = shield_setup();
    auto stock = stock_manifest(root, setup);
    if (!stock) return;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = shield_scene(setup);
        auth::SessionConfig config; config.tick_duration = {1, 30}; config.script_directories = {"Data/Scripts/Library/"};
        auto created = auth::ScriptScheduler::create(config, *stock);
        if (!created) { expect(false, "SOS-11: stock scheduler starts"); return; }
        auto scripts = std::move(created).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
        foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
        expect(errors.empty(), "SOS-11: real bindings for installed stock");
        std::array<std::uint32_t, 2> uses{};
        for (std::uint64_t tick = 1; tick <= 160; ++tick) {
            scene.view->tick = tick;
            shield_events(scene, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            auto prepared = foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps);
            expect(prepared && diagnostics.empty() && active == 2 && pumps <= active, "SOS-42: bounded missile-shield work counts");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) { expect(false, "SOS-11: stock service"); return; }
            for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.code << ' ' << diagnostic.message << '\n';
            expect(report.value().diagnostics.empty(), "SOS-11: installed missile shield has no diagnostic");
            shield_commands(report.value(), tick, uses);
            auto hash = scripts.state_hash(executor);
            if (!hash) { expect(false, "SOS-11: stock canonical hash"); return; }
            if (workers == 1) reference.push_back(hash.value());
            else expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-11: stock 1/2/4/8 equality");
        }
        expect(uses[0] > 0 && uses[1] > 0, "SOS-11: installed AI and armed human shields visibly rise");
        std::cout << workers << " workers: stock missile shield AI=" << uses[0] << " human=" << uses[1] << '\n';
    }
}


// SOS-11 synthetic contract on the production bindings: the deadly enemy's
// projectile types decide, with the AI/human autofire split and a restore.
void missile_shield_contracts() {
    const std::string module = R"LUA(
function Base_Definitions() ServiceRate = 1 end
function main()
    local human = Object.Get_Owner().Is_Human()
    while true do
        if (not human or Object.Is_Ability_Autofire('MISSILE_SHIELD')) and Object.Is_Ability_Ready('MISSILE_SHIELD') then
            local enemy = FindDeadlyEnemy(Object)
            if enemy then
                for _, projectile in pairs(enemy.Get_All_Projectile_Types()) do
                    if projectile.Is_Affected_By_Missile_Shield() then
                        Object.Activate_Ability('MISSILE_SHIELD', true)
                        break
                    end
                end
            end
        end
        coroutine.yield(true)
    end
end
)LUA";
    const auto setup = shield_setup();
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = shield_scene(setup);
        const auto make_scripts = [&]() {
            auth::ModuleManifest modules;
            expect(modules.add("Data/Scripts/GameObject/OBJECTSCRIPT_MISSILESHIELD.lua", module).has_value(), "SOS-11: module");
            auth::SessionConfig config; config.tick_duration = {1, 30};
            auto created = auth::ScriptScheduler::create(config, std::move(modules));
            if (!created) throw std::runtime_error(created.error().message);
            auto scripts = std::move(created).value();
            std::vector<eawr::core::Diagnostic> errors;
            foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
            foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
            expect(errors.empty(), "SOS-11: production bindings");
            return scripts;
        };
        auto scripts = make_scripts();
        std::array<std::uint32_t, 2> uses{};
        for (std::uint64_t tick = 1; tick <= 120; ++tick) {
            scene.view->tick = tick;
            shield_events(scene, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            expect(foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps).has_value()
                && diagnostics.empty(), "SOS-11: partitioned attachment and admission");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) throw std::runtime_error(report.error().message);
            expect(report.value().diagnostics.empty(), "SOS-11: no binding diagnostics");
            shield_commands(report.value(), tick, uses);
            if (tick == 55) {
                auto before = scripts.save(executor);
                if (!before) throw std::runtime_error(before.error().message);
                auto fresh = make_scripts();
                expect(fresh.load(before.value()).has_value(), "SOS-43: missile-shield restore");
                auto after = fresh.save();
                expect(after && after.value() == before.value(), "SOS-43: canonical restore bytes");
                scripts = std::move(fresh);
            }
            auto hash = scripts.state_hash(executor);
            expect(hash.has_value(), "SOS-11: state hashes");
            if (workers == 1 && hash) reference.push_back(hash.value());
            else if (hash) expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-11: 1/2/4/8 equality");
        }
        expect(uses[0] > 0 && uses[1] > 0, "SOS-11: AI and armed human shields rise");
    }
}


// SOS-15/19/40..44: synthetic behavior witnesses use the production attachment,
// pacing, bindings, callback graph and persistence paths.
// SOS-10/12/13/14/18: execute installed authored chunks through the production
// lifecycle on mod-shaped types; no runtime type-name test identifies a ship.
void stock_script_admission(const std::filesystem::path& root) {
    using K = tactical::AbilityKind;
    struct Case { const char* script; K ability; bool renamed{}; };
    for (const auto test : {Case{"ObjectScript_PowerToShields", K::defend},
             Case{"ObjectScript_PointDefense", K::laser_defense}, Case{"ObjectScript_MC30", K::cluster_bomb},
             Case{"ObjectScript_Krayt", K::self_destruct}, Case{"ObjectScript_IG2000", K::corrupt_systems},
             Case{"ObjectScript_Krayt", K::self_destruct, true}}) {
        const bool mc30 = test.ability == K::cluster_bomb;
        const bool krayt = test.ability == K::self_destruct;
        const bool proximity = mc30 || krayt || test.ability == K::corrupt_systems;
        foc::AiSetup setup;
        foc::AiType source; source.type_id = 110; source.lua_script = test.script;
        foc::AiType team; team.type_id = 103; team.combat_power = Real(600);
        foc::AiType craft; craft.type_id = 104; craft.combat_power = Real(300);
        foc::AiType ship; ship.type_id = 105; ship.combat_power = Real(700); ship.locomotor = true;
        ship.hardpoint_projectiles = {300};
        setup.content.types = {source, team, craft, ship};
        setup.content.projectile_categories.emplace(300, foc::ai_projectile_missile);
        auto stock = stock_manifest(root, setup);
        if (!stock) return;
        if (test.renamed) {
            const auto* original = stock->find("Data/Scripts/GameObject/OBJECTSCRIPT_KRAYT.lua");
            expect(original != nullptr, "SOS-01: original authored chunk exists");
            if (!original) return;
            const auto copy = *original;
            expect(stock->add("Data/Scripts/GameObject/MOD_AUTHORED.lua", copy).has_value(),
                "SOS-01: renamed authored copy enters the logical manifest");
            setup.content.types.front().lua_script = "Mod_Authored";
            const auto modules = foc::required_modules(setup);
            expect(std::find(modules.begin(), modules.end(), "Data/Scripts/GameObject/MOD_AUTHORED.lua") != modules.end(),
                "SOS-01: arbitrary authored module name is required from data");
        }
        std::vector<std::string> reference;
        std::vector<std::string> reference_commands;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto scene = slave_scene(setup);
            for (auto& unit : scene.view->units) {
                if (unit.id == 50 || unit.id == 51) {
                    unit.type = 110;
                    unit.abilities = {{test.ability, false, true, false}};
                    unit.hull = sim::math::Fixed::from_integer(unit.id == 50 ? 40 : 21).value();
                    unit.max_hull = sim::math::Fixed::from_integer(100).value();
                    unit.damage_rate = sim::math::Fixed::from_integer(20).value();
                }
                // The callback query sees the craft at 200; distance exclusion
                // must use their promoted parent's 149 position.
                if (unit.id == 60) unit.position.x = sim::math::Fixed::from_integer(mc30 ? 149 : 200).value();
                if (unit.craft) unit.position.x = sim::math::Fixed::from_integer(200).value();
                if (unit.id == 80 || unit.id == 81) unit.position.x = sim::math::Fixed::from_integer(5200).value();
                if (unit.owner == 3) unit.shield = Real(1);
            }
            // An inherited script cannot grant an ability absent in XML.
            auto absent = scene.view->units.front(); absent.id = 52; absent.abilities.clear();
            scene.view->units.insert(scene.view->units.begin() + 2, absent);
            if (!mc30 && !krayt) {
                std::erase_if(scene.view->units, [](const auto& unit) { return unit.container || unit.craft; });
                scene.view->units.push_back(sundered_unit(70, 105, 3, 200, 0));
                scene.view->units.back().shield = Real(1);
            }
            std::sort(scene.view->units.begin(), scene.view->units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            ai::Engine engine(scene.host, {}, {}); scene.host->engine = &engine;
            auth::SessionConfig config; config.tick_duration = {1, 30};
            config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/GameObject/"};
            const auto make_scripts = [&]() {
                auto created = auth::ScriptScheduler::create(config, *stock);
                if (!created) throw std::runtime_error(created.error().message);
                auto scripts = std::move(created).value();
                std::vector<eawr::core::Diagnostic> errors;
                foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
                foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
                expect(errors.empty(), "SOS migration: production bindings register");
                return scripts;
            };
            auto scripts = make_scripts();
            const auto number = [&](std::uint64_t object, std::string_view name) -> std::optional<Real> {
                auto value = scripts.read_global(foc::first_object_instance + object, name);
                if (!value || !value.value()) return {};
                const auto* n = std::get_if<Real>(&value.value()->data);
                return n ? std::optional<Real>(*n) : std::nullopt;
            };
            const auto restore = [&]() {
                auto saved = scripts.save(executor);
                if (!saved) throw std::runtime_error(saved.error().message);
                auto restored = make_scripts();
                expect(restored.load(saved.value()).has_value(), "SOS-43: migrated script restores");
                auto bytes = restored.save(executor);
                expect(bytes && bytes.value() == saved.value(), "SOS-43: sample/callback/commands restore byte-exactly");
                scripts = std::move(restored);
            };
            std::array<unsigned, 2> uses{};
            bool removed = false, previous_sample_fired = false, saved_sample = false, saved_command = false;
            unsigned empty_warnings = 0;
            std::uint64_t sample_tick = 0;
            std::vector<std::string> commands;
            for (std::uint64_t tick = 1; tick <= 320; ++tick) {
                scene.view->tick = tick;
                for (auto& unit : scene.view->units) {
                    if (tick == 40 && unit.id == 60) unit.position.x = sim::math::Fixed::from_integer(150).value();
                    if (unit.id == 50 || unit.id == 51) {
                        if (tick == 70) unit.damage_rate = sim::math::Fixed::from_integer(21).value();
                        if (tick == 100 && unit.id == 51) {
                            unit.abilities.front().autofire = true;
                            unit.hull = sim::math::Fixed::from_integer(20).value();
                        }
                        if (tick == 160 && unit.id == 51) unit.abilities.front().autofire = false;
                        if (tick == 220 && unit.id == 51) unit.abilities.front().autofire = true;
                        if (tick == 280) unit.abilities.front().ready = false;
                    }
                }
                if (tick == 70) {
                    scene.view->units.push_back(sundered_unit(71, 105, 3, 200, 50));
                    scene.view->units.back().shield = Real(1);
                    scene.view->units.push_back(sundered_unit(82, 105, 3, 5200, -50));
                    scene.view->units.back().shield = Real(1);
                    // Point defence only reacts to an affected deadly enemy.
                    ai::EngineContractAccess::threat(engine, 50, 71, 1);
                    ai::EngineContractAccess::threat(engine, 51, 82, 1);
                    std::sort(scene.view->units.begin(), scene.view->units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
                }
                if (proximity && sample_tick != 0 && tick == sample_tick + 1) {
                    // Keep the old sample across save/load, then move all AI
                    // contacts out before the next due update consumes it.
                    for (auto& unit : scene.view->units) if (unit.owner == 3 && unit.position.x < sim::math::Fixed::from_integer(1000).value())
                        unit.position.x = sim::math::Fixed::from_integer(10000).value();
                    removed = true;
                }
                std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
                std::vector<auth::ScriptDiagnostic> diagnostics;
                expect(foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps).has_value()
                    && diagnostics.empty() && pumps <= active, "SOS-42: migrated attachment work is bounded");
                auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
                auto report = scripts.service(executor, paced);
                if (!report) throw std::runtime_error(report.error().message);
                for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.code << ' ' << diagnostic.message << '\n';
                for (const auto& diagnostic : report.value().diagnostics) {
                    const bool empty_warning = test.ability == K::corrupt_systems
                        && diagnostic.code == auth::codes::script_warning
                        && diagnostic.message == "Find_Best_Local_Threat_Center -- no objects supplied";
                    expect(empty_warning, "SOS migration: only the sourced empty-sample warning is admitted");
                    if (empty_warning) ++empty_warnings;
                }
                for (const auto& command : report.value().commands) {
                    const auto id = command.issuer - foc::first_object_instance;
                    expect((id == 50 || id == 51) && command.verb == foc::verb_ability,
                        "SOS-44: only an authored ready ability can be commanded");
                    expect(tick > 70, "SOS migration: threshold/deadly-enemy gate holds");
                    expect(command.arguments.size() >= 4 && std::get<Real>(command.arguments[2].data) == Real(static_cast<int>(test.ability)),
                        "SOS migration: only the script's ability is requested");
                    expect(foc::tactical_ai_detail::translate_order(command).has_value(), "SOS migration: command translates to native payload");
                    if (id == 50) { ++uses[0]; if (removed) previous_sample_fired = true; }
                    if (id == 51) {
                        ++uses[1];
                        expect(test.ability != K::corrupt_systems && tick > 100 && (tick < 160 || tick > 220),
                            "SOS migration: human exit and autofire transitions retain their authored gates");
                    }
                    expect(tick < 280, "SOS migration: readiness prevents later commands");
                    commands.push_back(std::to_string(tick) + ":" + std::to_string(id));
                    if (!saved_command) { restore(); saved_command = true; }
                }
                if (mc30 || krayt) {
                    const auto threat = number(50, "nearby_unit_threat");
                    if (tick < 40 && threat) expect(*threat == (mc30 ? Real(0) : Real(600)),
                        "SOS-13/14: promote parent before distance exclusion and deduplicate craft");
                    if (mc30 && tick >= 40 && tick < 70 && threat) expect(*threat <= Real(600),
                        "SOS-13: one promoted team stays below the AI threshold");
                    if (tick >= 70 && threat && *threat >= Real(1200) && sample_tick == 0) {
                        sample_tick = tick; restore(); saved_sample = true;
                    }
                } else if (test.ability == K::corrupt_systems && tick >= 70 && sample_tick == 0) {
                    // This update's collected objects are consumed at the next pump.
                    sample_tick = tick + 30; // leave enough time for the due wrapper to collect
                }
                if (tick == 20) restore(); // before the next due service
                auto hash = scripts.state_hash(executor);
                expect(hash.has_value(), "SOS migration: canonical hash is available");
                if (workers == 1 && hash) reference.push_back(hash.value());
                else if (hash) expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)],
                    "SOS migration: 1/2/4/8 worker hashes agree");
            }
            expect(uses[0] > 0 && (test.ability == K::corrupt_systems ? uses[1] == 0 : uses[1] > 0),
                "SOS migration: stock AI and supported human paths visibly act");
            if (test.ability == K::corrupt_systems) expect(empty_warnings > 0 && uses[0] > 0,
                "WHE-68: empty table warns, returns nil and continues to a later IG-2000 activation");
            if (mc30 || krayt) expect(saved_sample && previous_sample_fired,
                "SOS-13/14/43: restored previous sample acts after current contacts leave");
            if (workers == 1) reference_commands = commands;
            else expect(commands == reference_commands, "SOS migration: 1/2/4/8 effects agree");
            std::cout << test.script << ' ' << workers << " workers AI=" << uses[0] << " human=" << uses[1] << '\n';
        }
    }
}

// SOS-44: data admission must not attach a second decision consumer while a
// native payload profile still owns the same object's script service.
void native_object_script_ownership() {
    const std::array names{"ObjectScript_PowerToShields", "ObjectScript_Krayt", "ObjectScript_MC30",
        "ObjectScript_PointDefense", "ObjectScript_IG2000"};
    for (unsigned kind = 0; kind < names.size(); ++kind) {
        auto setup = slave_setup(); setup.content.types.front().lua_script = names[kind];
        setup.content.types.front().point_defence_script = kind == 3;
        setup.content.types.front().corrupt_systems_script = kind == 4;
        auto scene = slave_scene(setup);
        tactical::TacticalSetup initial;
        initial.players = {{1, 1, 1, tactical::player_flag_commandable}};
        initial.units = {{50, 102, 1, {}, sim::math::identity_quat(), {}}};
        tactical::AbilityProfile ability; ability.kind = tactical::AbilityKind::turbo;
        ability.expiration_frames = 30; ability.recharge_frames = 30;
        tactical::UnitAbilityProfile profile; profile.type_id = 102; profile.abilities = {ability};
        profile.defend_script = kind == 0; profile.self_destruct_script = kind == 1; profile.mc30_script = kind == 2;
        tactical::AbilityTable table; table.profiles = {profile};
        auto created = tactical::TacticalSession::create(initial, {}, {}, {}, std::nullopt, {}, {}, table);
        expect(created.has_value(), "SOS-44: native ownership world creates");
        if (!created) continue;
        auto world = std::move(created).value(); scene.host->world = &world;
        auth::ModuleManifest manifest;
        expect(manifest.add(std::string("Data/Scripts/GameObject/") + names[kind] + ".lua", "function main() end").has_value(),
            "SOS-44: authored module is present despite native ownership");
        auto scheduler = auth::ScriptScheduler::create({}, std::move(manifest));
        if (!scheduler) { expect(false, "SOS-44: ownership scheduler creates"); continue; }
        std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
        std::vector<auth::ScriptDiagnostic> diagnostics;
        expect(foc::tactical_ai_detail::service_objects(sim::InlineExecutor{}, scene.host, scheduler.value(),
            sequence, diagnostics, active, pumps).has_value() && diagnostics.empty() && active == 0
            && scheduler.value().object_scripts().empty(), "SOS-44: all five native owners suppress duplicate Lua attachment");

        // SOS-46: both module admission routes freeze the same native omission,
        // including after rebuilding a world whose content still names the script.
        setup.players = {{1, "OWNER", false, false, ""}};
        auto admission_table = table;
        tactical::DurabilityTable health;
        if (kind == 1) {
            auto& manual = admission_table.profiles.front().abilities.front();
            manual.kind = tactical::AbilityKind::self_destruct;
            manual.spawned = tactical::SpawnedAbilityProfile{};
            manual.spawned->type = 999;
            manual.spawned->blast.damage = sim::math::Fixed::from_integer(1).value();
            manual.spawned->blast.radius = sim::math::Fixed::from_integer(1).value();
            tactical::DurabilityProfile hull; hull.type_id = 102;
            hull.max_hull = sim::math::Fixed::from_integer(100).value();
            health.profiles = {hull};
        }
        for (const bool omitted : {false, true}) for (const bool cached : {false, true}) {
            std::map<std::string, std::string> modules;
            for (const auto& path : foc::required_modules(setup, false)) modules.emplace(path, "");
            modules["Data/Scripts/Library/PGAICommands.lua"] =
                "function Base_Definitions() EnemyContrastTypes={} FriendlyContrastTypes={} end";
            if (!omitted) modules[std::string("Data/Scripts/GameObject/") + names[kind] + ".lua"] = "function main() end";
            if (cached) {
                auto selected = eawr::skirmish::select_ai_modules(modules, setup);
                expect(selected.has_value(), "SOS-46: cached native root admission succeeds");
                if (!selected) return;
                modules = std::move(selected).value();
            }
            auto session_world = tactical::TacticalSession::create(initial, {}, health, {}, std::nullopt, {}, {}, admission_table);
            if (!session_world) { expect(false, "SOS-46: native omission world creates"); return; }
            auto session = foc::create_session(std::move(session_world).value(), setup, modules);
            expect(session.has_value(), "SOS-46: missing native-owned root permits session creation");
            if (!session) return;
            const auto check_profiles = [&](const auto& bound) {
                const auto* admitted = bound.world().abilities().find(102);
                expect(admitted && admitted->abilities == admission_table.profiles.front().abilities
                    && admitted->special == profile.special
                    && admitted->defend_script == (!omitted && kind == 0)
                    && admitted->self_destruct_script == (!omitted && kind == 1)
                    && admitted->mc30_script == (!omitted && kind == 2),
                    "SOS-46: omit only native script decisions, preserving manual ability profiles");
            };
            check_profiles(session.value());
            expect(session.value().step(sim::InlineExecutor{}).has_value(), "SOS-46: native omission session advances");
            if (kind == 1) {
                const auto state = session.value().world().ability_state(50);
                expect(state && state->self_destruct_script.has_value() == !omitted,
                    "SOS-46: absent Krayt Lua runs no native self-destruct service; present control runs");
            }
            auto saved = session.value().save_scripts();
            auto checkpoint = session.value().world().save_checkpoint_state();
            auto rebuilt_world = tactical::TacticalSession::from_replay(session.value().record(), {}, health, {}, std::nullopt, {}, {}, admission_table);
            auto prepared = foc::prepare_session(setup, modules);
            expect(saved && checkpoint && rebuilt_world && prepared, "SOS-46: native omission checkpoint prepares");
            if (!saved || !checkpoint || !rebuilt_world || !prepared) return;
            expect(rebuilt_world.value().load_checkpoint_state(checkpoint.value()).has_value(), "SOS-46: native omission checkpoint loads");
            auto restored = auth::ScriptedTacticalSession::restore(std::move(rebuilt_world).value(), std::move(prepared.value().scripts), saved.value());
            expect(restored && prepared.value().bind(restored.value()), "SOS-46: restored bridge rebinds native omission");
            if (!restored) return;
            check_profiles(restored.value());
            const auto continued = session.value().step(sim::InlineExecutor{});
            const auto resumed = restored.value().step(sim::InlineExecutor{});
            expect(continued && resumed && continued.value().state_sha256 == resumed.value().state_sha256,
                "SOS-46: native omission checkpoint continuation is identical");
            if (kind == 1 && omitted) {
                const tactical::PlayerCommand manual{{restored.value().completed_tick(), 1, 0}, {50},
                    tactical::AbilityPayload{tactical::AbilityKind::self_destruct, tactical::AbilityAction::activate}};
                expect(restored.value().step(sim::InlineExecutor{}, std::span(&manual, 1)).has_value(),
                    "SOS-46: manual self-destruct still executes after missing-script restore");
                const auto state = restored.value().world().ability_state(50);
                expect(state && state->slots.front().active && !state->self_destruct_script,
                    "SOS-46: manual self-destruct countdown needs no object-script attachment");
            }
        }
    }
}


// SOS-42: production provider, independent partition order and a rebuilt host.
class ReverseObjectTurns final : public sim::PartitionExecutor {
public:
    std::size_t worker_count() const noexcept override { return 8; }
    eawr::core::Result<void> execute(std::size_t count, const std::function<void(std::size_t)>& run) const override {
        for (std::size_t at = count; at != 0; --at) run(at - 1);
        return eawr::core::Result<void>::success();
    }
};


// A test-only reference keeps the previous barrier admission, clock and draw key.
// It reuses the original host helper rather than estimating authored Lua cadence.
class LegacyObjectTurnReference final : public auth::ScriptEngine {
public:
    explicit LegacyObjectTurnReference(std::shared_ptr<foc::detail::Host> host) : host_(std::move(host)) {}
    eawr::core::Result<void> initialize_fresh(const tactical::TacticalSession& world, auth::ScriptScheduler& scripts) override {
        const sim::InlineExecutor executor;
        return prepare(executor,world,scripts);
    }
    eawr::core::Result<auth::ServiceOptions> before_service(const tactical::TacticalSession& world,
        const tactical::TacticalTick&, auth::ScriptScheduler& scripts) override {
        const sim::InlineExecutor executor;
        return options(executor,world,scripts);
    }
    eawr::core::Result<auth::ServiceOptions> before_service(const sim::PartitionExecutor& executor,
        const tactical::TacticalSession& world, const tactical::TacticalTick&, auth::ScriptScheduler& scripts) override {
        return options(executor,world,scripts);
    }
private:
    eawr::core::Result<void> prepare(const sim::PartitionExecutor& executor,
        const tactical::TacticalSession& world, auth::ScriptScheduler& scripts) {
        host_->world=&world; host_->snapshot=world.snapshot();
        host_->view=foc::detail::build_view(world,*host_->snapshot,&executor);
        std::uint64_t sequence{};
        std::uint32_t active{},pumps{};
        std::vector<auth::ScriptDiagnostic> diagnostics;
        auto result=foc::tactical_ai_detail::service_objects(executor,host_,scripts,sequence,diagnostics,active,pumps);
        if (!result) return result;
        if (!diagnostics.empty()) return eawr::core::Result<void>::failure(foc::detail::make_error(diagnostics.front().code,diagnostics.front().message));
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<auth::ServiceOptions> options(const sim::PartitionExecutor& executor,
        const tactical::TacticalSession& world, auth::ScriptScheduler& scripts) {
        if (auto result=prepare(executor,world,scripts);!result) return eawr::core::Result<auth::ServiceOptions>::failure(result.error());
        auth::ServiceOptions result;
        result.host_paced=[](std::uint64_t) { return true; };
        return eawr::core::Result<auth::ServiceOptions>::success(std::move(result));
    }
    std::shared_ptr<foc::detail::Host> host_;
};


void object_turn_cadence_pins() {
    struct Pin { const char* rule; sim::EntityId object; std::vector<std::uint64_t> before,after; };
    const std::vector<Pin> pins{
        {"SOS-15",10,{1,2,62,122,183},{1,14,75,136,197}},
        {"SOS-19",20,{1,2,62,122,183},{1,15,75,136,197}},
        {"SOS-16",30,{1,25,85,146,206},{1,47,107,167}},
        {"SOS-17",40,{1,4,64,124,185},{1,56,117,178}},
        {"SOS-11",50,{1,5,65,125,186},{1,40,100,161}}};
    const std::map<std::string,std::string> modules{
        {"Data/Scripts/GameObject/PHASE_PROBE.lua",
            "function Base_Definitions() ServiceRate=2; calls=0 end\n"
            "function main() sample=GameRandom.Get_Float(); while true do calls=calls+1; Object.Move_To({calls,0,0}); coroutine.yield(true) end end"},
        {"Data/Scripts/Library/PGAICommands.lua","function Base_Definitions() EnemyContrastTypes={}; FriendlyContrastTypes={} end"}};
    const sim::InlineExecutor executor;
    const eawr::platform::ThreadWorkerAdapter checkpoint_workers(4);
    for (const auto& pin : pins) {
        tactical::TacticalSetup initial;
        initial.seed=5678; initial.players={{1,1,1,tactical::player_flag_commandable}};
        initial.units={{pin.object,100,1}};
        foc::AiSetup config;
        config.seed=initial.seed;config.players={{1,"REBEL",false,false,""}};
        foc::AiType type;type.type_id=100;type.lua_script="PHASE_PROBE";config.content.types={type};
        for (const bool legacy : {true,false}) {
            auto world=tactical::TacticalSession::create(initial);
            auto session=[&]() -> eawr::core::Result<auth::ScriptedTacticalSession> {
                if (!legacy) return foc::create_session(std::move(world).value(),config,modules);
                auto host=std::make_shared<foc::detail::Host>();host->setup=config;
                for (const auto& entry : host->setup.content.types) host->types.emplace(entry.type_id,&entry);
                for (const auto& player : host->setup.players) host->players.emplace(player.player,&player);
                auth::ModuleManifest manifest;
                for (const auto& [path,bytes] : modules) expect(bool(manifest.add(path,bytes)),"SOS cadence manifest");
                auth::SessionConfig setup;setup.seed=config.seed;setup.tick_duration={1,30};
                setup.script_directories={"Data/Scripts/Library/","Data/Scripts/GameObject/"};
                auto scripts=auth::ScriptScheduler::create(setup,std::move(manifest));
                std::vector<eawr::core::Diagnostic> errors;
                foc::tactical_ai_detail::register_globals(scripts.value(),host,errors);
                foc::tactical_ai_detail::register_methods(scripts.value(),host,errors);
                expect(errors.empty(),"SOS cadence production bindings");
                auto result=auth::ScriptedTacticalSession::create(std::move(world).value(),std::move(scripts).value());
                expect(bool(result.value().register_verb(foc::verb_move,foc::tactical_ai_detail::translate_order)),"SOS cadence move translator");
                expect(bool(result.value().set_engine(std::make_shared<LegacyObjectTurnReference>(host))),"SOS cadence legacy owner");
                return result;
            }();
            expect(bool(session),"SOS cadence session creates");if (!session) return;
            std::vector<std::uint64_t> actual;
            std::optional<auth::ScriptedTacticalSession> uninterrupted;
            for (std::uint64_t frame=0;frame<220;++frame) {
                auto tick=session.value().step(uninterrupted
                    ? static_cast<const sim::PartitionExecutor&>(checkpoint_workers) : executor);
                expect(bool(tick),"SOS cadence step");if (!tick) return;
                expect(tick.value().scripts.diagnostics.empty(),"SOS cadence has no binding fault");
                if (uninterrupted) {
                    auto control_tick=uninterrupted->step(executor);
                    expect(control_tick && control_tick.value().state_sha256==tick.value().state_sha256,
                        "SOS-43: nonzero-rate checkpoint preserves every later hash across worker counts");
                }
                if (!legacy && frame==8) {
                    const auto saved=session.value().save_scripts();
                    const auto checkpoint=session.value().world().save_checkpoint_state();
                    auto prepared=foc::prepare_session(config,modules);
                    auto rebuilt_world=tactical::TacticalSession::from_replay(session.value().record());
                    expect(saved && checkpoint && prepared && rebuilt_world,"SOS-43: checkpoint between nonzero-rate pumps captures");
                    if (!saved || !checkpoint || !prepared || !rebuilt_world) return;
                    auto loaded=rebuilt_world.value().load_checkpoint_state(checkpoint.value());
                    expect(bool(loaded),"SOS-43: between-pump world checkpoint restores");if (!loaded) return;
                    auto rebuilt=auth::ScriptedTacticalSession::restore(std::move(rebuilt_world).value(),std::move(prepared.value().scripts),saved.value());
                    expect(rebuilt && prepared.value().bind(rebuilt.value()),"SOS-43: between-pump Lua clock and due admission restore");
                    if (!rebuilt) { std::cerr<<rebuilt.error().message<<'\n'; return; }
                    expect(rebuilt.value().save_scripts().value().scheduler==saved.value().scheduler,
                        "SOS-43: between-pump rebind preserves canonical Lua bytes");
                    uninterrupted.emplace(std::move(session).value());session=std::move(rebuilt);
                }
                if (!legacy && frame==1) {
                    auto saved=session.value().save_scripts();
                    auto inspect=foc::prepare_session(config,modules);
                    expect(saved && inspect && inspect.value().scripts.load(saved.value().scheduler),"SOS cadence state inspection");
                    if (saved && inspect) {
                        const auto id=foc::first_object_instance+pin.object;
                        const auto last=inspect.value().scripts.read_global(id,"LastService");
                        const auto sample=inspect.value().scripts.read_global(id,"sample");
                        auto phase=foc::tactical_ai_detail::object_phase_random(config.seed,1,id);
                        auth::RandomStream vm(config.seed,2|auth::turn_random_domain,id);
                        const auto offset=phase.next_unit();
                        const auto draw=Real(vm.next_below(std::uint64_t{1}<<15))/Real(32767);
                        expect(last && sample && last.value() && sample.value()
                            && std::get<Real>(last.value()->data)==Real(0)-offset*Real(2)
                            && std::get<Real>(sample.value()->data)==draw
                            && auth::random_word(config.seed,1|auth::object_phase_random_domain,id,0)
                                != auth::random_word(config.seed,1|auth::turn_random_domain,id,0),
                            "SOS-42: host phase and VM draw use independent per-instance keys");
                    }
                }
                for (const auto& command : tick.value().scripts.commands)
                    if (command.issuer==foc::first_object_instance+pin.object) actual.push_back(frame);
            }
            expect(actual==(legacy?pin.before:pin.after),std::string(pin.rule)+": literal old/new phased pump ticks");
            std::cout<<"SOS-CADENCE,"<<pin.rule<<','<<(legacy?"before":"after");
            for (const auto frame : actual) std::cout<<','<<frame;
            std::cout<<'\n';
        }
    }
}


void production_object_turns() {
    const std::string source = R"LUA(
root_ran=0
function Root() assert(Object.Is_Valid()); root_ran=root_ran+1 end
Create_Thread('Root')
function Base_Definitions() assert(Object==nil); ServiceRate=0; LastService=0; calls=0 end
function main()
    while true do
        calls=calls+1
        Object.Activate_Ability('INVULNERABILITY', true)
        Object.Move_To({calls, 0, 0})
        coroutine.yield(true)
    end
end
)LUA";
    tactical::TacticalSetup setup;
    setup.seed = 5678;
    setup.players = {{1,1,1,tactical::player_flag_commandable}, {2,2,2,tactical::player_flag_commandable}};
    setup.units = {{10,100,1}, {20,101,2}};
    tactical::AbilityProfile invulnerable;
    invulnerable.kind = tactical::AbilityKind::invulnerability;
    invulnerable.expiration_frames = 1;
    invulnerable.recharge_frames = 1;
    tactical::AbilityTable abilities;
    abilities.profiles = {{100, {invulnerable}, false}};
    foc::AiSetup config;
    config.seed = setup.seed;
    config.players = {{1,"REBEL",false,false,""}, {2,"EMPIRE",false,false,""}};
    foc::AiType type; type.type_id=100; type.lua_script="RENAMED_TURN_PROBE";
    config.content.types = {type};
    const std::map<std::string,std::string> modules{
        {"Data/Scripts/GameObject/RENAMED_TURN_PROBE.lua", source},
        {"Data/Scripts/Library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes={}; FriendlyContrastTypes={} end"}};
    const auto fresh_world = [&] { return tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, abilities); };
    std::vector<std::string> reference;
    const ReverseObjectTurns reverse;
    for (const unsigned workers : {1U,2U,4U,8U,0U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers == 0 ? 1U : workers);
        const sim::PartitionExecutor& executor = workers == 0 ? static_cast<const sim::PartitionExecutor&>(reverse) : pool;
        auto session = foc::create_session(std::move(fresh_world()).value(), config, modules);
        expect(session.has_value(), "SOS-42: production turn provider installs");
        if (!session) return;
        std::vector<std::string> hashes, world_hashes;
        std::optional<auth::ScriptedTacticalSession> uninterrupted;
        const eawr::platform::ThreadWorkerAdapter continuation(workers == 8 ? 2U : 8U);
        for (unsigned frame=0; frame<9; ++frame) {
            auto tick = session.value().step(frame > 4 ? static_cast<const sim::PartitionExecutor&>(continuation) : executor);
            expect(tick.has_value(), "SOS-42: production object turn completes");
            if (!tick) { std::cerr << tick.error().message << '\n'; return; }
            if (uninterrupted) {
                auto control_tick=uninterrupted->step(executor);
                expect(control_tick && control_tick.value().world.state_sha256==tick.value().world.state_sha256,
                    "SOS-43: restored world effects equal uninterrupted continuation on another worker count");
                expect(control_tick && control_tick.value().state_sha256==tick.value().state_sha256,
                    "SOS-43: restored script state equals uninterrupted continuation on another worker count");
            }
            expect(tick.value().scripts.diagnostics.empty(), "SOS-42: production bindings have no stale-read fault");
            hashes.push_back(tick.value().state_sha256);
            world_hashes.push_back(tick.value().world.state_sha256);
            if (frame == 0) expect(tick.value().world.object_turn.empty(), "SOS-30: initial barrier creates threads without pumping");
            if (frame == 1) {
                expect(tick.value().world.object_turn.size() == 1 && tick.value().world.object_turn[0].reason == tactical::RejectReason::none,
                    "SOS-15: first activation lands at tick 1, one tick before the previous bridge route");
                expect(tick.value().script_input.empty() && session.value().pending_script_commands() == 1,
                    "SOS-42: ability uses commit now while Move_To waits for the next tick");
            }
            if (frame == 4) {
                auto saved = session.value().save_scripts();
                auto checkpoint = session.value().world().save_checkpoint_state();
                auto world = tactical::TacticalSession::from_replay(session.value().record(), {}, {}, {}, std::nullopt, {}, {}, abilities);
                auto prepared = foc::prepare_session(config, modules);
                expect(saved && checkpoint && world && prepared, "SOS-43: turn barrier checkpoint captures both owners");
                if (!saved || !checkpoint || !world || !prepared) return;
                auto loaded = world.value().load_checkpoint_state(checkpoint.value());
                expect(loaded.has_value(), "SOS-43: turn world restores");
                if (!loaded) { std::cerr << loaded.error().message << '\n'; return; }
                auto rebuilt = auth::ScriptedTacticalSession::restore(std::move(world).value(), std::move(prepared.value().scripts), saved.value());
                if (!rebuilt) { expect(false,"SOS-43: bridge restore"); return; }
                const auto before_bind=rebuilt.value().save_scripts();
                const auto compare_restore=[](std::string_view stage,const std::string& before,const std::string& after) {
                    if (before==after) return;
                    std::size_t offset{};
                    while (offset<std::min(before.size(),after.size()) && before[offset]==after[offset]) ++offset;
                    std::cout<<"SOS-RESTORE-DELTA,"<<stage<<','<<before.size()<<','<<after.size()<<','<<offset;
                    for (std::size_t index=offset;index<std::min(offset+24,std::min(before.size(),after.size()));++index)
                        std::cout<<','<<static_cast<unsigned>(static_cast<unsigned char>(before[index]))
                            <<':'<<static_cast<unsigned>(static_cast<unsigned char>(after[index]));
                    std::cout<<'\n';
                };
                expect(before_bind.has_value(),"SOS-43: restored scheduler saves before bind");
                if (!before_bind) return;
                compare_restore("load",saved.value().scheduler,before_bind.value().scheduler);
                expect(bool(prepared.value().bind(rebuilt.value())), "SOS-43: fresh host derives due set from restored Lua");
                const auto after_bind=rebuilt.value().save_scripts();
                expect(after_bind.has_value(),"SOS-43: rebound scheduler saves");
                if (!after_bind) return;
                compare_restore("bind",before_bind.value().scheduler,after_bind.value().scheduler);
                expect(after_bind.value().scheduler == saved.value().scheduler, "SOS-43: rebuild never initializes twice");
                uninterrupted.emplace(std::move(session).value());
                session = std::move(rebuilt);
            }
        }
        auto recording = session.value().record();
        expect(recording.object_turns.size() >= 3 && recording.object_turns.front().tick == 1,
            "SOS-15/42: object-turn activations are retained in the world replay");
        auto replayed = tactical::TacticalSession::from_replay(recording, {}, {}, {}, std::nullopt, {}, {}, abilities);
        expect(replayed.has_value(), "SOS-42: world-only replay loads without a provider");
        if (!replayed) return;
        for (std::size_t frame=0; frame<world_hashes.size(); ++frame) {
            auto tick = replayed.value().step(executor);
            expect(tick && tick.value().state_sha256 == world_hashes[frame], "SOS-42: record reproduces every same-turn world effect");
        }
        if (reference.empty()) reference=hashes;
        else expect(reference == hashes, "SOS-42/43: 1/2/4/8 and reverse partitions produce equal turn states");
    }
    {
        foc::detail::Host host;
        auto world=fresh_world();host.world=&world.value();host.snapshot=world.value().snapshot();
        host.view=foc::detail::build_view(world.value(),*host.snapshot);
        foc::ai::TacticalWorld capabilities(host);host.in_object_turn=true;
        const std::vector<std::function<void()>> reads{
            [&] { static_cast<void>(host.economy(1)); },
            [&] { static_cast<void>(capabilities.account(1)); },
            [&] { static_cast<void>(capabilities.pads()); },
            [&] { static_cast<void>(capabilities.arriving(10)); },
            [&] { static_cast<void>(capabilities.build_allowed(1,10,100)); },
            [&] { static_cast<void>(capabilities.build_option(1,10,100)); },
            [&] { static_cast<void>(capabilities.production_counts(1,100)); },
            [&] { static_cast<void>(capabilities.construction_allowed(1,10)); },
            [&] { static_cast<void>(capabilities.constructing_for(10,1)); },
            [&] { static_cast<void>(capabilities.next_entity_id()); },
            [&] { static_cast<void>(capabilities.reinforcement_search_result(1,1)); },
            [&] { static_cast<void>(capabilities.events()); },
            [&] { static_cast<void>(capabilities.combat_events()); }};
        for (const auto& read : reads) {
            host.rejected_turn_read.store(false);read();
            expect(host.rejected_turn_read.load(),"SOS-42: dynamic capability rejects stale reads independently of the Lua API name");
        }
        host.rejected_turn_read.store(false);
        static_cast<void>(capabilities.view());static_cast<void>(capabilities.players());
        static_cast<void>(capabilities.economy());static_cast<void>(capabilities.completed_tick());
        expect(!host.rejected_turn_read.load(),"SOS-42: immutable rules and staged rows stay readable");
    }
    // Dynamic completed-world queries must diagnose rather than silently read t-1.
    const sim::InlineExecutor executor;
    for (const auto* query : {"Object.Get_Owner().Get_Credits()", "Find_Nearest(Object,Object.Get_Owner(),false)"}) {
        auto guarded_modules = modules;
        guarded_modules["Data/Scripts/GameObject/RENAMED_TURN_PROBE.lua"] =
            std::string("function Base_Definitions() ServiceRate=0 end\n")
            + "function main() Object.Activate_Ability('INVULNERABILITY',true); " + query + " end";
        auto guarded = foc::create_session(std::move(fresh_world()).value(), config, guarded_modules);
        expect(guarded && guarded.value().step(executor), "SOS-42: tripwire instance initializes");
        if (!guarded) return;
        auto tick = guarded.value().step(executor);
        expect(tick && !tick.value().scripts.diagnostics.empty() && guarded.value().record().object_turns.empty(),
            "SOS-42: stale-read tripwire faults the instance and drops its entire turn output");
    }
    {
        auto host=std::make_shared<foc::detail::Host>();
        auto view=std::make_shared<foc::detail::WorldView>();
        foc::AiType held;held.type_id=100;held.lua_script="UNADMITTED_TURN";
        host->setup.content.types={held};host->types.emplace(100,&host->setup.content.types.front());
        foc::detail::ViewUnit object;object.id=10;object.type=100;object.owner=1;
        tactical::AbilityStatus ability;ability.kind=tactical::AbilityKind::buzz_droids;ability.ready=true;
        object.abilities={ability};view->units={object};host->view=view;
        auth::ModuleManifest manifest;
        expect(bool(manifest.add("Data/Scripts/GameObject/UNADMITTED_TURN.lua",
            "function Base_Definitions() ServiceRate=0 end\nfunction main() Object.Activate_Ability('BUZZ_DROIDS',{0,0,0}) end")),
            "SOS-45: arbitrary module for an unadmitted turn handler");
        auth::SessionConfig script_config;script_config.tick_duration={1,30};script_config.script_directories={"Data/Scripts/GameObject/"};
        auto scripts=auth::ScriptScheduler::create(script_config,std::move(manifest));
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(),host,errors);
        foc::tactical_ai_detail::register_methods(scripts.value(),host,errors);
        std::uint64_t sequence{};std::uint32_t active{},pumps{};std::vector<auth::ScriptDiagnostic> diagnostics;
        expect(errors.empty() && foc::tactical_ai_detail::service_objects(executor,host,scripts.value(),sequence,diagnostics,active,pumps),
            "SOS-45: arbitrary turn module attaches");
        expect(bool(scripts.value().service(executor)),"SOS-45: arbitrary turn module initializes");
        host->in_object_turn=true;view->tick=1;sequence=0;
        expect(bool(foc::tactical_ai_detail::service_objects(executor,host,scripts.value(),sequence,diagnostics,active,pumps)),
            "SOS-45: arbitrary turn module becomes due");
        auto report=scripts.value().service_turn(executor,{foc::first_object_instance+10});
        expect(report && report.value().commands.empty() && std::any_of(report.value().diagnostics.begin(),report.value().diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code==auth::codes::unsupported_api; }),
            "SOS-45: unadmitted ability handler is exposed for every module instead of silently dropping a ready use");
    }

}


void object_script_contracts() {
    // SOS-46: production session creation keeps an unscripted object alongside
    // a working scripted control; the omission is immutable across worker counts.
    {
        tactical::TacticalSetup initial;
        initial.seed = 718;
        initial.players = {{1, 1, 1, tactical::player_flag_commandable}};
        initial.units = {{10, 100, 1}, {20, 101, 1}};
        foc::AiSetup config;
        config.seed = initial.seed;
        config.players = {{1, "FIRST", false, false, ""}};
        foc::AiType missing; missing.type_id = 100; missing.name = "MISSING_OWNER"; missing.lua_script = "MissingObjectRoot";
        foc::AiType unused = missing; unused.type_id = 102; unused.name = "UNPLACED_OWNER";
        foc::AiType healthy; healthy.type_id = 101; healthy.name = "CONTROL"; healthy.lua_script = "HealthyObjectRoot";
        config.content.types = {missing, unused, healthy};
        tactical::AbilityProfile ability;
        ability.kind = tactical::AbilityKind::invulnerability;
        ability.expiration_frames = 1; ability.recharge_frames = 1;
        tactical::AbilityTable abilities;
        abilities.profiles = {{101, {ability}, false}};
        const std::map<std::string, std::string> modules{
            {"Data/Scripts/Library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes={} FriendlyContrastTypes={} end"},
            {"Data/Scripts/GameObject/HealthyObjectRoot.lua",
                "function Base_Definitions() ServiceRate=0 end\n"
                "function main() while true do Object.Activate_Ability('INVULNERABILITY',true); coroutine.yield(true) end end"}};
        const auto make_world = [&] { return tactical::TacticalSession::create(initial, {}, {}, {}, std::nullopt, {}, {}, abilities); };
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto prepared = foc::prepare_session(config, modules);
            expect(prepared && prepared.value().load_notes.size() == 1
                && prepared.value().load_notes.front().severity == eawr::core::Severity::warning,
                "SOS-46: preparation exposes one warning for a shared absent root");
            auto world = make_world();
            expect(world.has_value(), "SOS-46: both objects exist in the synthetic world");
            if (!prepared || !world) return;
            auto session = auth::ScriptedTacticalSession::create(std::move(world).value(), std::move(prepared.value().scripts));
            if (!session) { expect(false, "SOS-46: scripted session creates"); return; }
            expect(prepared.value().bind(session.value()).has_value(), "SOS-46: missing root does not refuse the session bind");
            if (workers != 1) session.value().scramble_storage_for_testing();
            std::vector<std::string> hashes;
            std::size_t root_notes = 0, control_turns = 0;
            for (unsigned frame = 0; frame < 5; ++frame) {
                auto tick = session.value().step(executor);
                expect(tick.has_value(), "SOS-46: world keeps advancing after missing attachment");
                if (!tick) return;
                for (const auto& diagnostic : tick.value().scripts.diagnostics) {
                    expect(diagnostic.code == auth::codes::script_warning, "SOS-46: absent root raises no runtime error");
                    if (diagnostic.message.find("missing object Lua root") != std::string::npos) ++root_notes;
                }
                control_turns += tick.value().world.object_turn.size();
                const auto units = session.value().world().units();
                expect(units.size() == 2 && std::any_of(units.begin(), units.end(), [](const auto& unit) {
                    return unit.entity_id == 10 && unit.type_id == 100;
                }), "SOS-46: object with missing Lua remains present on every tick");
                hashes.push_back(tick.value().state_sha256);
            }
            expect(root_notes == 1 && control_turns > 0, "SOS-46: warn once, no attachment retries, healthy control still runs");
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "SOS-46: missing-root session is deterministic on 1/2/4/8 workers and shuffled storage");
        }
        auto changed = config;
        changed.content.types[1].lua_script = "DifferentUnplacedRoot";
        auto first = foc::create_session(std::move(make_world()).value(), config, modules);
        auto second = foc::create_session(std::move(make_world()).value(), changed, modules);
        expect(first && second, "SOS-46: sessions with different omitted unplaced roots create");
        if (first && second) {
            const sim::InlineExecutor executor;
            const auto a = first.value().step(executor);
            const auto b = second.value().step(executor);
            expect(a && b && a.value().world.state_sha256 == b.value().world.state_sha256
                && a.value().state_sha256 != b.value().state_sha256,
                "SOS-46: the frozen omission set enters the session identity even without a placed instance");
            auto saved = first.value().save_scripts();
            auto checkpoint = first.value().world().save_checkpoint_state();
            auto rebuilt_world = make_world();
            auto prepared = foc::prepare_session(changed, modules);
            expect(saved && saved.value().native && checkpoint && rebuilt_world && prepared,
                "SOS-46: an otherwise stateless owner retains its omission identity in the checkpoint");
            if (saved && checkpoint && rebuilt_world && prepared) {
                expect(rebuilt_world.value().load_checkpoint_state(checkpoint.value()).has_value(),
                    "SOS-46: identity refusal fixture restores the same world");
                auto restored = auth::ScriptedTacticalSession::restore(std::move(rebuilt_world).value(),
                    std::move(prepared.value().scripts), saved.value());
                expect(restored.has_value(), "SOS-46: identity refusal fixture restores the Lua checkpoint");
                if (restored) {
                    const auto bound = prepared.value().bind(restored.value());
                    expect(!bound && bound.error().code == auth::codes::load_rejected,
                        "SOS-46: checkpoint binding refuses a different omitted unplaced root");
                }
            }
        }
    }
    const auto fixed = [](int n) { return sim::math::Fixed::from_integer(n).value(); };
    const std::string falcon = R"LUA(
function Base_Definitions()
    assert(Object == nil)
    definitions = (definitions or 0) + 1
    ServiceRate = 1
    samples = 0
    Register_Event('Sample', function() samples = samples + 1 end)
end
function main()
    assert(Object.Is_Valid())
    while true do
        local human = Object.Get_Owner().Is_Human()
        local threat = FindDeadlyEnemy(Object)
        local low = Object.Get_Shield() < 0.2
        if human then low = Object.Is_Ability_Autofire('INVULNERABILITY') and
            Object.Get_Shield() < 0.05 and Object.Get_Hull() < 0.5 end
        if threat and low and Object.Is_Ability_Ready('INVULNERABILITY') then
            Object.Activate_Ability('INVULNERABILITY', true)
        end
        coroutine.yield(true)
    end
end
)LUA";
    const std::string vader = R"LUA(
function Base_Definitions() assert(Object == nil); ServiceRate = 1 end
function main()
    while true do
        if (not Object.Get_Owner().Is_Human() or Object.Is_Ability_Autofire('REPLENISH_WINGMEN')) and
            Object.Get_Contained_Object_Count() < 2 and Object.Is_Ability_Ready('REPLENISH_WINGMEN') then
            Object.Activate_Ability('REPLENISH_WINGMEN', true)
        end
        coroutine.yield(true)
    end
end
)LUA";
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto host = std::make_shared<foc::detail::Host>();
        auto view = std::make_shared<foc::detail::WorldView>();
        host->view = view;
        foc::AiType ship; ship.type_id = 100; ship.lua_script = "ObjectScript_MillenniumFalcon";
        foc::AiType team; team.type_id = 101; team.lua_script = "ObjectScript_VaderTIESquadron";
        host->setup.content.types = {ship, team};
        for (const auto& type : host->setup.content.types) host->types.emplace(type.type_id, &type);
        host->setup.players = {{1, "REBEL", false, true, {}, false}, {2, "EMPIRE", false, true, {}, false}};
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        foc::detail::ViewUnit object;
        object.id = 10; object.type = 100; object.owner = 1; object.hull = fixed(40); object.max_hull = fixed(100);
        object.shield = Real(1) / Real(10);
        object.abilities = {{tactical::AbilityKind::invulnerability, false, true, true}};
        foc::detail::ViewUnit enemy; enemy.id = 20; enemy.owner = 2;
        foc::detail::ViewUnit container; container.id = 30; container.type = 101; container.owner = 1;
        container.container = true; container.members = {31};
        foc::detail::ViewUnit leader; leader.id = 31; leader.owner = 1;
        leader.abilities = {{tactical::AbilityKind::replenish_wingmen, false, true, true}};
        view->units = {object, enemy, container, leader};
        ai::Engine engine(host, {}, {});
        host->engine = &engine;
        ai::EngineContractAccess::threat(engine, 10, 20, 1);
        const auto make_scripts = [&]() {
            auth::ModuleManifest modules;
            expect(modules.add("Data/Scripts/GameObject/OBJECTSCRIPT_MILLENNIUMFALCON.lua", falcon).has_value(), "SOS: Falcon module");
            expect(modules.add("Data/Scripts/GameObject/OBJECTSCRIPT_VADERTIESQUADRON.lua", vader).has_value(), "SOS: Vader module");
            expect(modules.add("Finished.lua", "function Base_Definitions() ServiceRate=0 end; function main() end").has_value(), "SOS: completion module");
            auth::SessionConfig config; config.tick_duration = {1, 30};
            auto created = auth::ScriptScheduler::create(config, std::move(modules));
            if (!created) throw std::runtime_error(created.error().message);
            auto scripts = std::move(created).value();
            std::vector<eawr::core::Diagnostic> errors;
            foc::tactical_ai_detail::register_globals(scripts, host, errors);
            foc::tactical_ai_detail::register_methods(scripts, host, errors);
            expect(errors.empty(), "SOS: production bindings");
            return scripts;
        };
        auto scripts = make_scripts();
        std::uint32_t active = 0, pumps = 0;
        std::vector<auth::ScriptDiagnostic> diagnostics;
        const auto prepare = [&]() {
            std::uint64_t sequence = 0;
            expect(foc::tactical_ai_detail::service_objects(executor, host, scripts, sequence, diagnostics, active, pumps).has_value(),
                "SOS: partitioned attachment and admission");
        };
        const auto tick = [&]() {
            view->tick = scripts.completed_tick() + 1;
            prepare();
            auth::ServiceOptions options; options.host_paced = [](std::uint64_t) { return true; };
            auto result = scripts.service(executor, options);
            if (!result) throw std::runtime_error(result.error().message);
            expect(result.value().diagnostics.empty() && diagnostics.empty(), "SOS: no script diagnostics");
            return std::move(result).value();
        };
        const auto restore = [&]() {
            auto before = scripts.save(executor);
            if (!before) throw std::runtime_error(before.error().message);
            auto fresh = make_scripts();
            expect(fresh.load(before.value()).has_value(), "SOS-32/43: restore into fresh scheduler");
            auto after = fresh.save();
            expect(after && after.value() == before.value(), "SOS-43: canonical restore bytes and callback identity");
            scripts = std::move(fresh);
        };
        prepare();
        expect(active == 2 && scripts.instances().size() == 2 && pumps == 0, "SOS-40: attach starting ship and real container once");
        restore(); // before definitions or any due service
        auto initialized = tick();
        expect(initialized.commands.empty() && pumps == 0, "SOS-30: definitions/main creation do not pump main");
        expect(scripts.reconcile_objects(scripts.object_scripts()).has_value() && scripts.instances().size() == 2,
            "SOS-40: repeated admission does not attach twice");
        restore();
        auto triggered = tick();
        expect(triggered.commands.size() == 2 && pumps == 2, "SOS-15/19: AI thresholds request Falcon and leader replenishment");
        restore(); // after commands were emitted, before downstream application
        auth::ScriptEvent sample; sample.key = {scripts.completed_tick()+1, auth::first_simulation_producer+2, 10, 0};
        sample.target = foc::first_object_instance + 10; sample.kind = auth::ScriptEvent::Kind::dispatch; sample.name = "Sample";
        expect(scripts.submit_event(sample).has_value(), "SOS-43: retained callback event");
        tick(); restore();
        auto samples = scripts.read_global(sample.target, "samples");
        expect(samples && samples.value() && std::get<Real>(samples.value()->data) == Real(1), "SOS-43: callback runs once across restore");
        auto definitions = scripts.read_global(sample.target, "definitions");
        expect(definitions && definitions.value() && std::get<Real>(definitions.value()->data) == Real(1), "SOS-32: restore never repeats definitions");
        // Exact due equality holds; pause is the absence of a world/script tick.
        auto snapshot = scripts.save();
        expect(snapshot && scripts.save().value() == snapshot.value(), "SOS: paused barriers do not advance state or pump");
        std::uint32_t human_uses = 0;
        for (int frame = 4; frame < 75; ++frame) {
            if (frame == 4) {
                // Human shield 0.1 fails, despite low hull and autofire.
                host->setup.players[0].ai = false;
                host->setup.players[0].human = true;
                view->units[2].members.push_back(32); // stale absent member must not count
            }
            if (frame == 36) view->units[0].shield = Real(1) / Real(100);
            if (frame == 40) view->units[3].abilities.clear();
            auto result = tick();
            if (frame < 36) for (const auto& command : result.commands)
                expect(command.issuer != sample.target, "SOS-15: human threshold is stricter than AI");
            for (const auto& command : result.commands) {
                if (command.issuer == sample.target && frame >= 36) ++human_uses;
                if (frame >= 40) expect(command.issuer != foc::first_object_instance+30,
                    "SOS-19: inherited script without authored readiness does not activate");
            }
            if (frame == 50) restore();
            auto hash = scripts.state_hash(executor);
            expect(hash.has_value(), "SOS-43: attachment state hashes");
            if (workers == 1 && hash) reference.push_back(hash.value());
            else if (hash) expect(hash.value() == reference[static_cast<std::size_t>(frame-4)], "SOS-42: 1/2/4/8 state equality");
        }
        expect(human_uses > 0, "SOS-15: human low shield/hull with autofire fires");
        sample.key.tick = scripts.completed_tick()+5;
        expect(scripts.submit_event(sample).has_value(), "SOS-44: stale future callback queued before destruction");
        // Destruction drops callbacks/events and stale generations cannot restart.
        view->units.erase(view->units.begin()); prepare();
        expect(scripts.instances().size() == 1 && scripts.object_scripts().size() == 1, "SOS-33/44: destruction retires registry and Lua state");
        view->units.insert(view->units.begin(), object); view->units[0].id = 40;
        std::sort(view->units.begin(), view->units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        tick();
        expect(scripts.instances().size() == 2 && scripts.instances().back() == foc::first_object_instance+40, "SOS-40/44: replacement/spawn uses fresh allocation generation");
        auto completed = scripts.reconcile_objects({{{1, 50}, 1, foc::first_object_instance+50, "Finished.lua"}});
        expect(completed && completed.value().empty(), "SOS: completion instance admitted");
        auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
        expect(scripts.service(executor, paced).has_value(), "SOS: completion initialized");
        auth::ScriptEvent pump; pump.key = {scripts.completed_tick()+1, auth::first_simulation_producer, 50, 0};
        pump.target = foc::first_object_instance+50; pump.kind = auth::ScriptEvent::Kind::pump;
        expect(scripts.submit_event(pump).has_value() && scripts.service(executor, paced).has_value(), "SOS: completion pumped");
        expect(scripts.instances().empty(), "SOS-31/44: finished main detaches");
        expect(scripts.reconcile_objects({{{1, 50}, 1, pump.target, "Finished.lua"}}).has_value(), "SOS: ended identity retained");
        expect(scripts.instances().empty(), "SOS-44: finished attachment never restarts");
        restore();
        auto replacement = scripts.reconcile_objects({{{1, 50}, 2, pump.target, "Finished.lua"}});
        expect(replacement && scripts.instances().size() == 1, "SOS-44: a new generation receives a fresh attachment");
        expect(scripts.reconcile_objects({}).has_value() && scripts.instances().empty(), "SOS: explicit retirement");
        auto missing = scripts.reconcile_objects({{{1, 51}, 1, pump.target+1, "Absent.lua"}});
        expect(missing && missing.value().size() == 1 && missing.value()[0].code == auth::codes::module_rejected,
            "SOS-40: unsupported module diagnoses once");
        auto retry = scripts.reconcile_objects({{{1, 51}, 1, pump.target+1, "Absent.lua"}});
        expect(retry && retry.value().empty() && scripts.instances().empty(), "SOS: rejected attachment does not retry each tick");
    }
}

} // namespace foc_contract
