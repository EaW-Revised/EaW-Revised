"""Cases for test_live_session; collected by its legacy facade."""

from live_session_test_support import (
    ACCLAMATOR, CAMERA, CAPTURE_TICKS, CORUSCANT,
    CORVETTE, DUEL, DUEL_ARGS, DUEL_CAMERA,
    DUEL_CAPTURE_TICKS, DUEL_DEATH_TICK, EMPIRE_STATION, HIT_REASONS,
    LIVE_QUIT_BATTLE_SECONDS, LIVE_QUIT_EXIT_BOUND_SECONDS, LiveSessionRunner, MC80,
    NEBULON, NEBULON_ENGINES, NEBULON_FL, ORDERS,
    ROOT, S28, S28_LIVE_TICKS, STATION,
    decode_png, hit_rows_per_tick, json, os,
    pathlib, re, read, shutil,
    source_text, strict_json, subprocess, sys,
    tempfile, time, unittest,
)

sys.path.insert(0, str(ROOT / "tools"))
from native_cpp_fixture import compile_fixture, select_toolchain


class LiveSessionSourceCases:
    def test_station_upgrade_controlled_relationship_fixture(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest("requires a C++ compiler for the isolated announcement fixture")
        source = read("apps/viewer/src/battle_audio_events.cpp")
        branch = source.split("if (event.kind == tactical::EventKind::station_replaced) {", 1)[1]
        branch = branch.split("if (event.kind == tactical::EventKind::pad_structure_sold)", 1)[0]
        # Execute the consumer's actual branch with a controlled mixed-faction ally,
        # which the stock lobby and replay loader intentionally cannot construct.
        fixture = r'''
#include <cassert>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace data { enum class Category { faction }; }
struct Event { int player; };
struct Live {
    int relation;
    int local_player() const { return 2; }
    bool battle_participant(int) const { return relation != 3; }
    bool is_ally_of_local(int) const { return relation == 1; }
    std::string player_faction(int player) const { return player == 2 ? "Empire" : "Rebel"; }
    std::string local_faction() const { return "Empire"; }
};
struct Catalog {
    std::optional<std::string> resolve(const std::string& name, data::Category) { return name; }
};
struct Probe {
    Catalog catalog;
    Catalog* catalog_ = &catalog;
    bool empty = false;
    std::vector<std::string> heard;
    std::string tag(const std::string& faction, const std::string& field) {
        return empty ? "" : faction + ":" + field;
    }
    std::string event(const std::string& name, const std::string&) { return name; }
    void play(const std::string& sound, std::nullopt_t, bool, const char*) {
        if (!sound.empty()) heard.push_back(sound);
    }
    void update(const Live& live, const std::vector<Event>& events) {
        for (const auto& event : events) {
__BRANCH__
    }
};
int main() {
    for (int relation = 0; relation != 4; ++relation) {
        Probe probe;
        probe.update(Live{relation}, {{relation == 0 ? 2 : 1}});
        if (relation == 3) { assert(probe.heard.empty()); continue; }
        const std::string expected = relation == 0 ? "Empire:SFXEvent_Starbase_Upgraded"
            : relation == 1 ? "Rebel:SFXEvent_Starbase_Ally_Upgraded"
                            : "Rebel:SFXEvent_Starbase_Enemy_Upgraded";
        assert(probe.heard == std::vector<std::string>{expected});
        probe.empty = true;
        probe.update(Live{relation}, {{relation == 0 ? 2 : 1}});
        assert(probe.heard.size() == 1);
    }
}
'''.replace("__BRANCH__", branch)
        with tempfile.TemporaryDirectory(prefix="eawr-upgrade-contract-") as temporary:
            directory = pathlib.Path(temporary)
            cpp = directory / "fixture.cpp"
            executable = directory / ("fixture.exe" if os.name == "nt" else "fixture")
            cpp.write_text(fixture, encoding="utf-8")
            compile_fixture(toolchain, cpp, executable)
            checked = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_station_upgrade_uses_owner_faction_and_excludes_neutral(self):
        source = read("apps/viewer/src/battle_audio_events.cpp")
        branch = source.split("if (event.kind == tactical::EventKind::station_replaced) {", 1)[1]
        branch = branch.split("if (event.kind == tactical::EventKind::pad_structure_sold)", 1)[0]
        self.assertIn("if (!live.battle_participant(event.player)) continue;", branch)
        self.assertIn("owner_faction = live.player_faction(event.player)", branch)
        self.assertIn("catalog_->resolve(owner_faction, data::Category::faction)", branch)
        self.assertIn("tag(faction.value(), field), owner_faction", branch)
        self.assertNotIn("live.local_faction()", branch)
        self.assertIn("event.player == live.local_player()", branch)
        self.assertIn("live.is_ally_of_local(event.player)", branch)
        for field in ("SFXEvent_Starbase_Upgraded", "SFXEvent_Starbase_Ally_Upgraded",
                      "SFXEvent_Starbase_Enemy_Upgraded"):
            self.assertIn(field, branch)
        self.assertEqual(branch.count("play("), 1)
        resolver = read("apps/viewer/src/battle_audio_prepare.cpp")
        self.assertIn("if (text.empty()) return nullptr;", resolver)
        self.assertIn("if (!sfx || released_) return std::nullopt;", source)

    def test_presentation_never_names_the_session(self):
        # UI-07: the viewer reads snapshots and submits command values only.
        completed = subprocess.run([sys.executable, str(ROOT / "tools/check_presentation_boundary.py"),
                                    "--root", str(ROOT)], text=True, capture_output=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        view = read("apps/viewer/src/live_session_view.cpp") + read("apps/viewer/src/live_session_view.hpp")
        self.assertNotIn("tactical/session.hpp", view)
        self.assertIn("platform::LiveSession::start", view)
        self.assertIn("visible_entities", read("src/presentation/space/live_units.cpp"))


    def test_one_placement_conversion(self):
        # The #288 model turn lands in the scene's placement conversion; live units call it.
        populate = read("apps/viewer/src/space_populate.cpp")
        animation = (ROOT / "apps/viewer/src/space_populate_animation.cpp").read_text(encoding="utf-8")
        body = animation[animation.index("live_unit_transform("):]
        self.assertIn("scene::placement_transform(", body[:600])
        self.assertEqual(len(re.findall(r"live_unit_transform\(", populate)), 2)


    def test_refusals(self):
        mode = read("apps/viewer/src/map_mode.cpp")
        for message in ("--eawr-live-* options require --eawr-live-session",
                        "--eawr-live-session requires --eawr-populate",
                        "--eawr-live-session applies only to a kind-2 (space) map",
                        "--eawr-live-session is not composed with --eawr-space-place-object or -at"):
            self.assertIn(message, mode)
        self.assertIn("runs on", read("apps/viewer/src/live_session_view.cpp"))


    def test_audio_reads_the_session_only(self):
        # #84 (docs/behaviour/battle-audio.md): the battle audio takes the session read-only and
        # hears only the published snapshots and event log; --eawr-audio is checked.
        header = read("apps/viewer/src/battle_audio.hpp")
        self.assertIn("void frame(const LiveSessionView& live", header)
        self.assertNotIn("order_input", read("apps/viewer/src/battle_audio.cpp"))
        mode = read("apps/viewer/src/map_mode.cpp")
        for message in ("--eawr-audio expects on or off", "--eawr-audio requires --eawr-live-session"):
            self.assertIn(message, mode)
        self.assertIn("audio_sfx_contracts", read("tests/presentation/audio/CMakeLists.txt"))


    def test_lane_mute_applies_before_mode_selection_and_survives_battle_start(self):
        host = read("apps/viewer/src/viewer_host_ready.cpp")
        self.assertLess(host.index("mute_lane_audio_output();"), host.index("get_cmdline_user_args()"))
        self.assertIn('get_environment("EAWR_AUDIO_MUTE")', read("apps/viewer/src/audio_output.hpp"))
        self.assertIn("options_.muted = options_.muted || audio_output_muted();",
                      read("apps/viewer/src/battle_audio.cpp"))


    def test_ui07_scheduler_feeds_the_simulation_thread(self):
        # The UI-07 contract: one scheduler, OrderInput on it, taken by the session's command
        # source on the simulation thread; the scheduler outlives the session.
        view = read("apps/viewer/src/live_session_view.cpp")
        header = read("apps/viewer/src/live_session_view.hpp")
        self.assertIn("std::make_unique<ui::OrderInput>(*scheduler_)", view)
        self.assertIn("session_options.command_source", view)
        self.assertIn("scheduler->take(next_tick)", view)
        self.assertEqual(view.count(".take(") + view.count("->take("), 1)
        self.assertLess(header.index("scheduler_;"), header.index("session_;"))
        source = read("src/platform/live_session.cpp")
        self.assertIn("options_.command_source(next_tick)", source)
        self.assertIn("catch (...)", source)


    def test_simulation_thread_is_the_pool(self):
        source = read("src/platform/live_session.cpp")
        # #656: the live tick dispatches by cost on the pool.
        self.assertIn("const ThreadWorkerAdapter executor(options_.workers, ThreadWorkerAdapter::Dispatch::by_cost);", source)
        self.assertIn("thread_ = std::thread", source)


    def test_perf_overlay_reads_wall_clock_only(self):
        # #558: the overlay times the simulation thread's ticks beside the session, never inside
        # the hashed state, and is documented with its key and flag.
        source = read("src/platform/live_session.cpp")
        self.assertIn("costs_.push_back", source)
        self.assertNotIn("Clock", read("include/eawr/sim/tactical/session.hpp"))
        mode = read("apps/viewer/src/map_mode.cpp")
        self.assertIn("--eawr-perf-overlay expects on or off", read("apps/viewer/src/map_mode_hud.cpp"))
        self.assertIn("parse_perf_overlay_argument", mode)
        readme = read("apps/viewer/README.md")
        self.assertIn("--eawr-perf-overlay", readme)
        self.assertIn("F3", readme)
        self.assertIn("F3", read("docs/ui/perf-overlay.md"))


    def test_time_and_battle_end_notes_are_our_own_words(self):
        # #453, #459: the rules cite opaque evidence IDs, never addresses or original names (the names
        # themselves are ci_cleanroom_check's job over the whole tree, so this test does not spell them out).
        for name in ("tactical-time-controls.md", "battle-end.md"):
            note = (ROOT / "docs/behaviour" / name).read_text(encoding="utf-8")
            self.assertNotRegex(note, r"0x[0-9a-fA-F]{6,}")
            for heading in ("## Rules", "## Cases", "## Unknowns", "## Fidelity list"):
                self.assertIn(heading, note)
