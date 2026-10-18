"""The main-menu demo attract battle (#2937, docs/behaviour/main-menu-attract.md MAT-01 to MAT-21).

`--eawr-attract` hosts the attract runner alone: the stock demo map's placed forces fight under the
AI through the live session path, with no HUD and no world input, seen through the map's saved
camera. `--eawr-attract-frames` ends the run after n frames (the battle is stopped, then the report
is written), `--eawr-attract-do <frame>:<step>` scripts the front end's keys and stop/start.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT (a FoC install). Set
EAWR_ATTRACT_CAPTURE_DIR to keep the captures for an eye check.
"""

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import source_text  # noqa: E402

SRC = ROOT / "apps/viewer/src"

RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")


class AttractStructure(unittest.TestCase):
    def test_the_runner_is_wired_into_the_host_and_the_build(self):
        ready = (SRC / "viewer_host_ready.cpp").read_text(encoding="utf-8")
        self.assertIn("AttractRunner::requested()", ready)
        process = (SRC / "viewer_host_process.cpp").read_text(encoding="utf-8")
        self.assertIn("attract_runner_->standalone_frame", process)
        build = source_text("apps/viewer/CMakeLists.txt")
        self.assertIn("src/attract_runner.cpp", build)
        sources = (ROOT / "src/skirmish/sources.cmake").read_text(encoding="utf-8")
        self.assertIn("attract.cpp", sources)

    def test_the_front_end_owns_the_runner_and_stops_it_before_a_battle(self):
        front = (SRC / "front_end_mode.cpp").read_text(encoding="utf-8")
        self.assertIn("std::make_unique<AttractRunner>", front)
        # MAT-13: a pending start, the exit and the front end's own teardown stop it.
        self.assertIn("setup->start_pending()", front)
        self.assertIn('stop_attract("exit")', front)
        self.assertIn("state_->attract->stop()", front)
        host = (SRC / "viewer_host_camera_setup.cpp").read_text(encoding="utf-8")
        self.assertIn("front_end_->input(event)", host)
        self.assertIn("front_end_->window_activated()", host)

    def test_the_demo_battle_builds_no_hud_and_takes_no_world_input(self):
        population = (SRC / "map_mode_ready_population.cpp").read_text(encoding="utf-8")
        # MAT-10, WGUI-76, MAT-18: no battle input, silent battle audio, the saved camera.
        self.assertIn("if (!state.options.attract)", population)
        self.assertIn("state.options.attract", population)
        self.assertIn("skirmish::attract_camera(map)", population)
        live = (SRC / "map_mode_live.cpp").read_text(encoding="utf-8")
        self.assertIn("if (state.options.attract) return;", live)
        ready = (SRC / "map_mode_ready.cpp").read_text(encoding="utf-8")
        self.assertIn("state.hud_options.reset()", ready)  # MAT-19

    def test_the_demo_battle_keeps_speech_and_tactical_music_off_the_voice_path(self):
        # MAT-18 (owner 10-12): the attract queues no speech, announcement or GUI event and starts no
        # tactical music; it is gated at the source and counted, not left to a bus mute.
        population = (SRC / "map_mode_ready_population.cpp").read_text(encoding="utf-8")
        self.assertIn(".attract_silent = state.options.attract.has_value()", population)
        events = (SRC / "battle_audio_events.cpp").read_text(encoding="utf-8")
        # The event queue entry (speech class, then 3D), the production cue's speech queue and the
        # campaign briefing.
        self.assertEqual(events.count("options_.attract_silent"), 4)
        self.assertIn("options_.attract_silent && speech_class(*sfx)", events)
        # MAT-18: 3D events are refused at the source (retail's multiplier 0); no bus is muted.
        self.assertIn("options_.attract_silent && sfx->is_3d", events)
        audio = (SRC / "battle_audio.cpp").read_text(encoding="utf-8")
        self.assertNotIn("sfx_3d_muted", audio)
        self.assertIn("if (!options_.attract_silent) server->set_bus_mute(0, options_.muted);", audio)
        prepare = (SRC / "battle_audio_prepare.cpp").read_text(encoding="utf-8")
        self.assertIn("if (!options_.attract_silent) {", prepare)
        # The report carries the counts the graphical case reads.
        runner = (SRC / "attract_runner.cpp").read_text(encoding="utf-8")
        self.assertIn(r'\"speech\": {\"refused\"', runner)
        self.assertIn(r'\"sfx_3d\": {\"refused\"', runner)

    def test_the_pool_return_comes_in_through_the_session_setup(self):
        # MAT-15/16 (legacy #3021): the demo start carries the pool-return record in its setup (the
        # skirmish start sets it, the live session keeps the start's setup), so a normal skirmish,
        # LAN and a replay never see it; nothing in the viewer gates the sim.
        prepare = source_text("apps/viewer/src/live_session_prepare.cpp")
        self.assertIn("setup_ = start_->setup;", prepare)
        start = (ROOT / "src/skirmish/start.cpp").read_text(encoding="utf-8")
        self.assertIn("start_.setup.attract_pool = std::move(pool);", start)
        self.assertIn("if (fixture_.placed) {", start)
        economy = (ROOT / "src/skirmish/economy.cpp").read_text(encoding="utf-8")
        self.assertIn("start.setup.attract_pool.has_value()", economy)
        for name in ("session_step_commands.cpp", "session_step_fighters.cpp"):
            text = source_text(f"src/sim/tactical/{name}")
            self.assertIn("if (impl_->setup.attract_pool)", text, name)

    def test_the_camera_is_a_presentation_only_seam_behind_the_saved_camera(self):
        # Owner 10-11: the attract uses the battle camera (#3005); this placeholder keeps the saved
        # camera alive through AttractCamera::update(dt, battle view), and never touches the sim.
        header = (SRC / "attract_camera.hpp").read_text(encoding="utf-8")
        self.assertIn("AttractCameraPose update(double delta, const AttractBattleView& battle)", header)
        camera = (SRC / "attract_camera.cpp").read_text(encoding="utf-8")
        self.assertNotIn("eawr/sim", header + camera)
        live = (SRC / "map_mode_live.cpp").read_text(encoding="utf-8")
        self.assertIn("state.space->set_attract_pose(pose.eye, pose.target)", live)
        self.assertIn("src/attract_camera.cpp", source_text("apps/viewer/CMakeLists.txt"))

    def test_stop_frees_the_battle_before_its_host_node(self):
        runner = (SRC / "attract_runner.cpp").read_text(encoding="utf-8")
        free = runner[runner.index("void free_battle()"):]
        free = free[:free.index("map_index.reset();")]
        self.assertLess(free.index("map.reset();"), free.index("memdelete(host);"))
        self.assertIn("is_instance_id_valid", free)
        host = (SRC / "viewer_host_camera_setup.cpp").read_text(encoding="utf-8")
        self.assertLess(host.index("attract_runner_.reset();"), host.index("map_mode_.reset();"))

    def test_nothing_is_a_fixed_content_list_but_the_games_own_two_ai_names(self):
        # Mods change GameConstants: the runner takes the maps, delays and timeout from it.
        runner = (SRC / "attract_runner.cpp").read_text(encoding="utf-8")
        for fixed in ("Demo_Attract_Kamino", "demo_attract_", "Front_End_Demo_Attract", "BasicRebel"):
            self.assertNotIn(fixed, runner)
        attract = (ROOT / "src/skirmish/attract.cpp").read_text(encoding="utf-8")
        self.assertNotIn("Demo_Attract_Kamino", attract)


def run_attract(test, directory, name, *extra, frames=400, capture=True, timeout=600):
    executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
    test.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
    report = directory / f"{name}.json"
    arguments = [executable, "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), "--",
                 "--eawr-attract", *extra, "--eawr-game-root", GAME_ROOT, "--eawr-attract-frames", str(frames),
                 "--eawr-report", str(report)]
    keep = os.environ.get("EAWR_ATTRACT_CAPTURE_DIR")
    if capture:
        picture = (pathlib.Path(keep) if keep else directory) / f"{name}.png"
        picture.parent.mkdir(parents=True, exist_ok=True)
        arguments += ["--eawr-capture", str(picture)]
    completed = subprocess.run(arguments, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               check=False, timeout=timeout)
    test.assertTrue(report.is_file(), completed.stdout)
    result = json.loads(report.read_text(encoding="utf-8"))
    test.assertEqual(result["failure"], "", completed.stdout)
    test.assertEqual(completed.returncode, 0, completed.stdout)
    test.assertNotIn("ERROR:", completed.stdout)
    return result


@unittest.skipUnless(RUNTIME and GAME_ROOT,
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class AttractGraphical(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-attract-"))

    def test_the_battle_runs_unseen_by_input_and_stop_frees_the_session(self):
        # The #2937 GPU case: the attract starts on the stock map, the forces move and fight, no HUD
        # or battle input exists, input reaches nothing, and stop() frees the session.
        result = run_attract(self, self.directory, "run", "--eawr-attract-do", "120:click",
                             "--eawr-attract-do", "140:key:other", frames=600)
        self.assertTrue(result["started"])
        battle = result["battle"]
        self.assertTrue(battle["live"])
        self.assertGreater(battle["units"], 10)
        self.assertGreater(battle["latest_tick"], 30)
        self.assertGreater(battle["moved"], 0, "the placed forces move under the AI")
        self.assertFalse(battle["hud"])
        self.assertFalse(battle["battle_input"])
        self.assertEqual(result["world_events"], 1, "the click reached the world, which rejects it (MAT-10)")
        self.assertEqual(result["keys_consumed"], 0)
        self.assertEqual(result["keys_passed"], 1)
        self.assertEqual(result["starts"], 1)
        self.assertEqual(result["stops"], 1)
        self.assertTrue(result["session_freed"])
        # Owner 10-11: the camera is alive (a placeholder for the battle camera, #3005): it starts
        # at the saved frame and has moved by the end of the run, while input stays blocked.
        camera = result["camera"]
        self.assertGreater(camera["updates"], 100)
        distance = sum((a - b) ** 2 for a, b in zip(camera["eye"], camera["first_eye"])) ** 0.5
        self.assertGreater(distance, 0.5, "the attract camera's transform changes over time")
        # MAT-18: no bus is muted. The front end's hover/press sounds, the effects slider's preview
        # and the menu music play on the same SFX/music buses, so they stay audible during the demo.
        buses = result["buses_muted"]
        during, after = buses["during"], buses["after"]
        for name in ("EAWR_SFX", "EAWR_Voice", "EAWR_Music"):
            if name in during:
                self.assertFalse(during[name], name + " stays unmuted: menu clicks and slider previews are audible")
        self.assertEqual(during, after, "the attract mutes nothing, so stop() has nothing to restore")
        # The demo's 3D sound events are refused at the source instead (retail's multiplier 0).
        sfx_3d = result["sfx_3d"]
        self.assertEqual(sfx_3d["started"], 0, "no 3D sound event started under the attract")
        self.assertGreater(sfx_3d["refused"], 0, "the gate saw the battle's 3D sound events")
        # MAT-18 (#3163): no speech, announcement or GUI event reached the voice path over the whole
        # run; the gate refused them (the battle's loading hint alone is one), nothing was queued
        # and nothing started.
        speech = result["speech"]
        self.assertEqual(speech["queued"], 0, "no speech event was queued under the attract")
        self.assertEqual(speech["started"], 0, "no speech or announcement started under the attract")
        self.assertGreater(speech["refused"], 0, "the gate saw the battle's own speech-class events")

    def test_the_forces_fight_for_a_whole_cycle_and_the_battle_does_not_run_dry(self):
        # The coordinator's question (#2937): without the pool return (MAT-15, MAT-16) the battle must
        # still have fighters left after a minimum cycle's worth of time. 30000 frames is about two
        # minutes of real-time battle on a host that draws at several hundred frames a second.
        result = run_attract(self, self.directory, "fight", frames=30000, capture=False, timeout=1500)
        battle = result["battle"]
        self.assertGreater(battle["damaged"] + battle["lost"], 0, "ships lose hull or are destroyed")
        self.assertGreater(battle["latest_tick"], 600, "the battle ran for real")
        self.assertLess(battle["lost"], battle["units"], "both sides are not wiped out")
        self.assertTrue(result["session_freed"])
        print(f"attract battle after {battle['latest_tick'] / 30:.0f} s: {battle['units']} units, "
              f"{battle['moved']} moved, {battle['damaged']} damaged, {battle['lost']} lost")

    def test_the_battle_refills_with_returned_types(self):
        # MAT-15/16 (legacy #3021): a destroyed force's type returns to its owner's pool and the AI
        # reinforces it, with no allied star base required. After a long run the battle still holds
        # most of what it started with, and units that were not there at the first sample have
        # arrived. (The twenty-minute map cycle is measured headless: skirmish_attract_cycle.)
        result = run_attract(self, self.directory, "refill", frames=90000, capture=False, timeout=2400)
        battle = result["battle"]
        self.assertGreater(battle["lost"], 0, "the placed forces lose units")
        self.assertGreater(battle["joined"], 0, "returned types arrive again")
        self.assertGreaterEqual(battle["alive"] * 2, battle["units"], "the unit count stays above half the start")
        self.assertTrue(result["session_freed"])
        print(f"attract refill after {battle['latest_tick'] / 30:.0f} s: {battle['alive']} alive of "
              f"{battle['units']} at the start, {battle['lost']} lost, {battle['joined']} joined")

    def test_escape_backspace_and_a_restart_leave_no_session_behind(self):
        # MAT-07 and MAT-13: Backspace restarts the map, Escape stops the attract; a later start
        # runs a fresh battle and the run ends with the session freed.
        result = run_attract(self, self.directory, "keys", "--eawr-attract-do", "90:key:backspace",
                             "--eawr-attract-do", "200:key:escape", "--eawr-attract-do", "260:start",
                             frames=520)
        self.assertEqual(result["starts"], 2)
        self.assertGreaterEqual(result["advances"], 3)  # first start, Backspace, the restart
        self.assertEqual(result["keys_consumed"], 2)
        self.assertGreaterEqual(result["stops"], 2)
        self.assertTrue(result["session_freed"])

    def test_a_dialog_hands_the_keys_back_to_the_front_end(self):
        result = run_attract(self, self.directory, "dialog", "--eawr-attract-do", "60:dialog:on",
                             "--eawr-attract-do", "100:key:enter", frames=300, capture=False)
        self.assertEqual(result["keys_consumed"], 0, "MAT-07: with a dialog up the keys are not the attract's")
        self.assertEqual(result["advances"], 1)

    def test_attract_off_is_a_plain_backdrop_and_the_frame_time_is_noted(self):
        off = run_attract(self, self.directory, "off", "off", frames=300)
        self.assertFalse(off["started"])
        self.assertFalse(off["battle"]["live"])
        self.assertEqual(off["failed_starts"], 0)
        on = run_attract(self, self.directory, "on", frames=300, capture=False)
        self.assertTrue(on["started"])
        print(f"attract frame time: on {on['frame_ms_mean']:.2f} ms mean / {on['frame_ms_max']:.2f} ms max,"
              f" off {off['frame_ms_mean']:.2f} ms mean / {off['frame_ms_max']:.2f} ms max")


if __name__ == "__main__":
    unittest.main()
