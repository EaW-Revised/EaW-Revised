#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/galactic_story_scripts.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// legacy #6634: a story script's Letter_Box_In(seconds) / Letter_Box_Out(seconds) reach the plot owner's seat as local
// presentation requests, in script order, and change nothing the simulation hashes. A call without one finite,
// non-negative number is a named fault of that script thread, never a match abort. Synthetic names only.
namespace {
namespace st = eawr::sim::galactic::story;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
namespace cpn = eawr::campaign;
using eawr::testing::take;
using galactic_test::expect;

void write(const std::filesystem::path& path, const std::string& body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << body;
}

// Owner 1: the good scene (in over 2 s, a pause of frames, out at once). Owner 2: seven faulty calls, one per thread (missing, text, negative, extra, NaN, +inf, -inf).
constexpr std::string_view good_script = R"LUA(
function Base_Definitions() end
function main()
    Letter_Box_In(2)
    for i = 1, 20 do coroutine.yield(true) end
    Letter_Box_Out(0)
    for i = 1, 20 do coroutine.yield(true) end
    Letter_Box_In(0.5)
end
)LUA";
constexpr std::string_view bad_script = R"LUA(
function Base_Definitions() end
function Fault_One() Letter_Box_In() end
function Fault_Two() Letter_Box_In("soon") end
function Fault_Three() Letter_Box_Out(-1) end
function Fault_Four() Letter_Box_Out(1, 2) end
function Fault_Five() Letter_Box_In(0 / 0) end
function Fault_Six() Letter_Box_Out(1 / 0) end
function Fault_Seven() Letter_Box_Out(-1 / 0) end
function main()
    Create_Thread("Fault_One")
    Create_Thread("Fault_Two")
    Create_Thread("Fault_Three")
    Create_Thread("Fault_Four")
    Create_Thread("Fault_Five")
    Create_Thread("Fault_Six")
    Create_Thread("Fault_Seven")
    for i = 1, 5 do coroutine.yield(true) end
    Letter_Box_In(3)
end
)LUA";

std::vector<st::SubPlotDef> plots() {
    st::SubPlotDef good;
    good.name = "Good.xml"; good.script = "Letterbox_Good"; good.owner = 1;
    st::SubPlotDef bad;
    bad.name = "Bad.xml"; bad.script = "Letterbox_Bad"; bad.owner = 2;
    return {good, bad};
}

struct Outcome {
    std::string digest, trail;
};

Outcome run(const std::size_t workers, const std::filesystem::path& root, const bool replaying) {
    gc::SyntheticGalaxyOptions shape; shape.players = 2; shape.planets = 4;
    const auto setup = gc::synthetic_galaxy(shape);
    const auto folder = root / ((replaying ? "r" : "w") + std::to_string(workers));
    std::filesystem::remove_all(folder);
    write(folder / "loose" / "scripts" / "story" / "Letterbox_Good.lua", std::string(good_script));
    write(folder / "loose" / "scripts" / "story" / "Letterbox_Bad.lua", std::string(bad_script));
    eawr::vfs::MountSpec mount; mount.layer_id = std::string(replaying ? "letterbox-replay-" : "letterbox-") + std::to_string(workers); mount.data_root = folder / "loose";
    const auto files = take(eawr::vfs::Vfs::mount(std::vector<eawr::vfs::MountSpec>{mount}));
    const auto subs = plots();
    const auto content = take(cpn::GalacticStoryScripts::load(files, subs));
    expect(content->notes().empty(), "both scripts are Story modules of the mounted data");

    st::Config config;
    config.factions = {{"ALPHA", 1}, {"BETA", 2}};
    cpn::GalacticStoryPlayers players;
    players.factions = {"Alpha", "Beta"};
    players.players = {{1, "Alpha"}, {2, "Beta"}};
    auth::SessionConfig scheduler_config;
    scheduler_config.seed = setup.seed; scheduler_config.galactic_clock = true; scheduler_config.tick_duration = {1, 30};
    const auto link = std::make_shared<cpn::GalacticStoryLink>();
    auto ai = take(auth::ScriptScheduler::create(scheduler_config, {}));
    auto story = take(cpn::create_galactic_story_scheduler(*content, scheduler_config, false, players, {}, {}, link));
    std::vector<std::string> notes;
    take(cpn::start_galactic_story_scripts(story, *content, notes));
    expect(notes.empty(), "the scripts start");

    const eawr::platform::ThreadWorkerAdapter executor(workers);
    auto game = take(cpn::GameSession::create(setup));
    take(game.install_story(config, {{1, "plot.xml"}}, subs));
    take(game.install_galactic_scripts(std::move(ai), std::move(story)));
    game.bind_story_scripts(content);
    take(game.connect_story_link(link));
    if (replaying) {
        // A session reconstructing a recording makes no local presentation request. One native effect that falls due long
        // after this run's last tick keeps the world in native replay for every step below.
        gc::NativeEffect pending;
        pending.frontier = 1000; pending.sequence = 1; pending.phase = gc::NativeEffectPhase::barrier; pending.kind = gc::NativeEffectKind::cash;
        pending.payload = {0};
        take(game.configure_native_replay_owners([&](gc::GalacticSession& world) {
            const std::array<gc::NativeEffect, 1> effects{pending};
            return world.stage_native_replay(effects, 1000);
        }));
    }

    Outcome out;
    std::vector<std::string> requests_one, requests_two;
    std::size_t faults{};
    for (int i = 0; i < 90; ++i) {
        const auto tick = take(game.step(executor));
        for (const auto& row : tick.story_presentation) {
            const auto* request = std::get_if<st::ConsumerRequest>(&row.payload);
            if (!request || request->consumer != st::Consumer::camera) continue;
            (row.owner == 1 ? requests_one : requests_two).push_back(request->reward_name + ":" + request->parameters.front());
        }
        for (const auto& diagnostic : tick.diagnostics)
            if (diagnostic.message.find("Letter_Box_") != std::string::npos) ++faults;
    }
    if (replaying) {
        expect(requests_one.empty() && requests_two.empty(), "a session in native replay makes no letterbox request");
    } else {
        expect(requests_one == (std::vector<std::string>{"LETTER_BOX_IN:2", "LETTER_BOX_OUT:0", "LETTER_BOX_IN:0.5"}),
            "the owner's seat gets the calls in script order, with the seconds as written");
        expect(requests_two == std::vector<std::string>{"LETTER_BOX_IN:3"},
            "the other owner gets only its own good call; the seven faulty calls published nothing");
    }
    expect(faults >= 7, "each faulty call is a named script fault, the match runs on");
    out.digest = game.state_sha256();
    for (const auto& line : requests_one) out.trail += "1:" + line + ";";
    for (const auto& line : requests_two) out.trail += "2:" + line + ";";
    out.trail += std::to_string(faults);
    return out;
}
} // namespace

int case_galactic_story_letterbox_tests(int argc, char** argv) {
    try {
        const auto root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("story-letterbox");
        Outcome reference;
        bool first = true;
        // The recorded-session run: the same scripts, every request dropped, the faults unchanged.
        run(1, root, true); // its own checks: no request, the seven faults still reported
        for (const auto workers : eawr::platform::determinism_worker_counts()) {
            const auto result = run(workers, root, false);
            if (first) { reference = result; first = false; continue; }
            expect(result.digest == reference.digest, "the session digest is equal on " + std::to_string(workers) + " workers");
            expect(result.trail == reference.trail, "the requests and faults are equal on " + std::to_string(workers) + " workers");
        }
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
