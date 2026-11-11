#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/galactic_story_scripts.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// #5857 PR 2a: Sleep through the real library chain. The installed game's own Library modules (PGStoryMode, PGStateMachine,
// PGCommands, PGBase, PGDebug) are pinned from the mounted data, a story script of the filter-script shape requires
// PGStoryMode, and a thread of it loops on the library's Sleep. Sleep is Lua there (GetCurrentTime, PumpEvents and the
// thread values are the host's), so the thread must advance with the mode clock and stand still while it is frozen.
// Nothing of the game is copied into the repository: the library comes from EAWR_EAW_GAME_ROOT at run time.
namespace {
namespace st = eawr::sim::galactic::story;
namespace auth = eawr::script::authoritative;
namespace cpn = eawr::campaign;
using eawr::testing::must;
using galactic_test::expect;

void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// The shape of a filter script: the event table is set in Definitions, a start event creates a thread that polls and sleeps.
constexpr std::string_view probe = R"LUA(
require("PGStoryMode")

function Definitions()
    StoryModeEvents = { Probe_Start = Probe_Begin }
    Rounds = 0
    Begun = 0
end

function Probe_Begin(message)
    if message == OnEnter then
        Begun = Begun + 1
        Create_Thread("Probe_Loop", 0.1)
    end
end

function Probe_Loop(interval)
    while true do
        Rounds = Rounds + 1
        Sleep(interval)
    end
end
)LUA";

std::int64_t integer_of(auth::ScriptScheduler& scripts, std::uint64_t instance, std::string_view name) {
    const auto value = must(scripts.read_global(instance, name));
    const auto* n = value ? std::get_if<eawr::script::numeric::LuaNumber>(&value->data) : nullptr;
    return n ? static_cast<std::int64_t>(static_cast<std::int32_t>(*n)) : -1;
}

void sleeps_through_the_library(const std::filesystem::path& root) {
    const auto game = eawr::testing::game_root();
    if (!game) throw std::runtime_error("the Sleep stock test needs EAWR_EAW_GAME_ROOT; unavailable data is not success");
    const auto loose = root / "loose";
    std::filesystem::remove_all(root);
    write(loose / "scripts" / "story" / "Sleep_Probe.lua", std::string(probe));
    std::vector<eawr::vfs::MountSpec> mounts;
    eawr::vfs::MountSpec probe_layer; probe_layer.layer_id = "sleep-probe"; probe_layer.data_root = loose;
    mounts.push_back(probe_layer);
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*game) / folder / "Data")).mount);
    const auto files = must(eawr::vfs::Vfs::mount(mounts));

    st::SubPlotDef plot;
    plot.name = "Sleep.xml"; plot.script = "Sleep_Probe"; plot.owner = 1;
    const std::vector<st::SubPlotDef> subs{plot};
    const auto content = must(cpn::GalacticStoryScripts::load(files, subs));
    expect(content->notes().empty() && content->find("Sleep.xml") && !content->find("Sleep.xml")->module.empty(),
        "the probe script is a Story module of the mounted data");
    const auto instance = content->find("Sleep.xml")->instance;

    auth::SessionConfig config;
    config.seed = 5; config.galactic_clock = true; config.tick_duration = {1, 30};
    auto scripts = must(cpn::create_galactic_story_scheduler(*content, config));
    std::vector<std::string> notes;
    must(cpn::start_galactic_story_scripts(scripts, *content, notes));
    expect(notes.empty(), "the script's chunk, with the real PGStoryMode chain behind its require, loads");

    const eawr::sim::InlineExecutor executor;
    const auto errors = [](const auth::ServiceReport& report) {
        std::size_t n{};
        for (const auto& row : report.diagnostics) if (row.code == auth::codes::script_error || row.code == auth::codes::missing_api) ++n;
        return n;
    };
    std::size_t faults{};
    // The scheduler's tick moves on every service; the mode frame (GetCurrentTime) may stand still, never lead the tick.
    std::uint64_t tick = 0;
    const auto service = [&](std::uint64_t frame) {
        ++tick;
        auth::ServiceOptions options; options.time_tick = std::min(frame, tick);
        const auto report = must(scripts.service(executor, options));
        faults += errors(report);
        for (const auto& row : report.diagnostics) std::cerr << "diagnostic: " << row.code << ' ' << row.message << '\n';
    };
    service(1); // Base_Definitions (the library's, which runs Definitions) then main (Process_States)
    expect(integer_of(scripts, instance, "Begun") == 0 && integer_of(scripts, instance, "Rounds") == 0, "nothing runs before the start event");
    std::uint64_t sequence = 0;
    must(cpn::forward_galactic_story_calls(scripts, *content, subs, std::vector<st::ScriptCall>{{0, "Probe_Start"}}, sequence));
    for (std::uint64_t frame = 2; frame <= 6; ++frame) service(frame);
    expect(integer_of(scripts, instance, "Begun") == 1, "Story_Event_Trigger from the real PGStoryMode enters the event's state");
    const auto early = integer_of(scripts, instance, "Rounds");
    expect(early >= 1, "the thread created by the state runs");
    for (std::uint64_t frame = 7; frame <= 36; ++frame) service(frame);
    const auto later = integer_of(scripts, instance, "Rounds");
    // Thirty more frames are a second of mode time: ten sleeps of a tenth of a second, give or take the frames a sleep rounds up to.
    expect(later - early >= 6 && later - early <= 11, "Sleep(0.1) from the library advances with the mode clock");
    // A frozen clock (a paused game) does not complete a sleep.
    for (int round = 0; round < 12; ++round) service(36);
    expect(integer_of(scripts, instance, "Rounds") == later, "a sleeping thread stands still while the mode clock is frozen");
    for (std::uint64_t frame = 37; frame <= 60; ++frame) service(frame);
    expect(integer_of(scripts, instance, "Rounds") > later, "and goes on when the clock moves");
    expect(faults == 0, "the library chain raises no script error and calls no unbound engine function");
}
} // namespace

int case_galactic_story_sleep_stock_tests(int argc, char** argv) {
    try { sleeps_through_the_library(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("story-sleep-stock")); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
