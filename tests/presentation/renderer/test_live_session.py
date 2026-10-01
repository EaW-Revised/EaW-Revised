"""Contracts for #80 part A: the production viewer runs the M2 tactical session live.

`--eawr-live-session m2` on Coruscant with `--eawr-populate` runs the authoritative session of
the M2 skirmish start (plan/phase-2/m2-skirmish.md) on its own simulation thread and draws its
units from the published snapshots, hiding what the local player does not see.
`--eawr-live-order` injects an order through the next-tick command path.

The structural half runs everywhere from committed sources. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT to run the Rebel
corvette's move and turn with captures, the viewer-attached hashes against a headless replay of
the recorded command stream (inside the viewer), and two worker counts against each other.
Captures stay in a temporary directory; nothing is written to the repository.
"""

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from test_space_map_mode import decode_png, strict_json  # noqa: E402

CORUSCANT = "data/art/maps/_mp_space_coruscant.ted"
CAMERA = ROOT / "apps/viewer/project/config/coruscant-live-session-camera.xml"
# The Rebel Corellian_Corvette, unit 5 of the M2 start (sim_headless --skirmish m2), stands on
# the Rebel spawn at (-5057, 4700) facing 336 degrees. It is ordered north-east at tick 15 and
# to face west at tick 150.
CORVETTE = 5
# Unit 1 is the Rebel station, Skirmish_Rebel_Star_Base_1 (death clone Rebel_Star_Base_1_Death_Clone).
STATION = 1
# Unit 6 is the Rebel Nebulon_B_Frigate. Its HardPoints: FL, FR, BL, BR weapons (each with a
# Death_Breakoff_Prop), then the engines (none).
NEBULON = 6
# Unit 7 is the Rebel Calamari_Cruiser (the MC80, SK-22); unit 8 is the Empire station,
# Skirmish_Empire_Star_Base_1.
MC80 = 7
EMPIRE_STATION = 8
# Unit 11 is the Empire Acclamator_Assault_Ship of the M2 start (#535's fog-edge crossing).
ACCLAMATOR = 11
# Unit 6 is the Rebel Nebulon_B_Frigate; HardPoints index 0 is HP_Nebulon_Weapon_FL, 4 the engines.
NEBULON = 6
NEBULON_FL = 0
NEBULON_ENGINES = 4
ORDERS = ("--eawr-live-order", f"15:move:{CORVETTE}@-4600,5150,0",
          "--eawr-live-order", f"150:face:{CORVETTE}@-5600,4700,0")
CAPTURE_TICKS = (0, 60, 120, 180, 240)
# #80: the #74 duel (tests/fidelity/S-15-duel-damage.json) as sim_headless --scenario writes its
# replay; the Nebulon-B (unit 1, Rebel player 2) kills the Tartan (unit 2) near tick 718. #475's
# collection-tree candidate order (CO-03) moved this from 713 once it landed in combat.cpp's
# hardpoint and priority-set scans (space-targeting CO rules); re-pinned after the #435 base-merge.
DUEL = ROOT / "tests/fidelity/fixtures/S-15-duel-damage.eawr-replay"
DUEL_CAMERA = ROOT / "apps/viewer/project/config/coruscant-duel-camera.xml"
# 720: the first --eawr-live-step-3 frame past the fatal hit at tick 718 (its impact and the
# death explosion); 719 is not a capture tick since it falls between frames at that step.
DUEL_CAPTURE_TICKS = (90, 300, 600, 720)
DUEL_DEATH_TICK = 718
DUEL_ARGS = ("--eawr-live-replay", str(DUEL), "--eawr-live-reveal", "on", "--eawr-live-player", "2",
             "--eawr-live-step", "3", "--eawr-environment", "map", "--eawr-lighting", "sh")
HIT_REASONS = ("shield", "armor_reduced", "detonation")
# #456: S-28 (tests/fidelity/S-28-squadron-vs-capital.json) as sim_headless --scenario writes its
# replay; the Acclamator (unit 1) fires concussion missiles at the corvette (unit 2), the first hit
# at tick 249.
S28 = ROOT / "tests/fidelity/fixtures/S-28-squadron-vs-capital.eawr-replay"
# The session's own simulation thread keeps ticking in the background past a held presented tick
# (BEP-02), so a run driven past --eawr-live-ticks can see a later missile the presented window
# never actually reached; bound comparisons to this same window on every --eawr-live-step tested.
S28_LIVE_TICKS = 300
# The live window-close test: how long the battle runs before the close, and the bound on the time
# from the close request to the process exit (FoC's own close is instant; the rig manages about a
# second, the bound leaves room for a busy desktop).
LIVE_QUIT_BATTLE_SECONDS = 60
LIVE_QUIT_EXIT_BOUND_SECONDS = 3.0


def hit_rows_per_tick(effects):
    # The battle effects' spawn log, as {tick: hit particles spawned}.
    counts = {}
    for tick, key, _ in effects["spawn_log"]:
        if key.split(":")[0] in HIT_REASONS:
            counts[tick] = counts.get(tick, 0) + 1
    return counts


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class LiveSessionSources(unittest.TestCase):
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
        body = populate[populate.index("live_unit_transform("):]
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
        host = read("apps/viewer/src/viewer_host.cpp")
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


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class LiveSessionGraphical(unittest.TestCase):
    def test_unicode_camera_bindings_and_live_file_paths(self):
        with tempfile.TemporaryDirectory(prefix="eawr live caf\u00e9 ") as temporary:
            directory = pathlib.Path(temporary)
            camera = directory / "camera caf\u00e9.xml"
            shutil.copy2(CAMERA, camera)
            shutil.copy2(CAMERA.parent / "space-live-camera-bindings.json", directory)
            replay = directory / "record caf\u00e9.eawr-replay"
            hashes = directory / "live caf\u00e9.hashes.csv"
            font_cache = directory / "fonts"
            shutil.copytree(os.environ.get("EAWR_FONT_CACHE", ROOT / "out/fonts"), font_cache)
            common = ("--eawr-live-ticks", "8", "--eawr-live-step", "1", "--eawr-audio", "off",
                      "--eawr-font-cache", str(font_cache))
            code, recorded = self._run(directory, "record", (
                *common, "--eawr-live-hashes", str(hashes), "--eawr-live-replay-out", str(replay)), camera=camera)
            self.assertEqual(code, 0, recorded.get("failure"))
            self.assertTrue(hashes.is_file())
            self.assertTrue(replay.is_file())
            self.assertTrue((directory / "record.png").is_file())
            self.assertEqual(recorded["hud"]["font_cache"]["directory"], font_cache.as_posix())
            self.assertEqual(recorded["map_camera"]["overview_clicks"]["override"]["file"], camera.name)
            code, replayed = self._run(directory, "replay", (
                *common, "--eawr-live-replay", str(replay)), camera=camera,
                session=("--eawr-live-session", "replay"))
            self.assertEqual(code, 0, replayed.get("failure"))
            self.assertEqual(replayed["live_session"]["replay"], replay.as_posix())
            self.assertTrue(replayed["live_session"]["headless_hashes_equal"])
            self.assertEqual(replayed["live_session"]["final_state_sha256"],
                             recorded["live_session"]["final_state_sha256"])

            name = "\u6218\u6597"
            code, captured = self._run(directory, name, (
                *common, "--eawr-live-capture-ticks", "2,4"), camera=camera)
            self.assertEqual(code, 0, captured.get("failure"))
            for filename in (f"{name}.png", f"{name}_t0002.png", f"{name}_t0004.png"):
                image = directory / filename
                self.assertTrue(image.is_file(), filename)
                self.assertGreater(image.stat().st_size, 0)
            self.assertEqual([file for file in captured["captures"] if file != "configured"],
                             [f"{name}_t0002.png", f"{name}_t0004.png"])

            missing = directory / "missing caf\u00e9.fog"
            code, refused = self._run(directory, "missing-fog", (
                "--eawr-fog-grid", str(missing), "--eawr-fog-sha256", "0" * 64,
                "--eawr-fog-team", "1", "--eawr-fog-revision", "0", "--eawr-fog-tick", "0"),
                session=(), camera=camera)
            self.assertEqual(code, 2)
            self.assertEqual(refused["failure"], f"cannot read fog grid: {missing.as_posix()}")

    def _run(self, directory: pathlib.Path, name: str, extra=(), session=("--eawr-live-session", "m2"), camera=CAMERA, env=None, map_name=CORUSCANT):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        report = directory / f"{name}.json"
        camera_args = ("--eawr-map-camera-config", str(camera)) if camera is not None else ()
        completed = subprocess.run(
            [executable, "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), "--",
             "--eawr-map", map_name, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
             "--eawr-report", str(report), "--eawr-capture", str(directory / f"{name}.png"),
             "--eawr-populate", *camera_args, *session, *extra],
            cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        self.assertTrue(report.is_file(), completed.stdout[-4000:])
        return completed.returncode, strict_json(report.read_text(encoding="utf-8"))

    def test_skirmish_loads_selected_faction_and_fleet_beyond_m2(self):
        # SC-01: default forces and fleet choices extend the unit tables at startup.
        # These types are absent from the pinned M2 load, so this catches silently
        # retaining that whitelist after accepting different lobby choices.
        with tempfile.TemporaryDirectory(prefix="eawr-selected-skirmish-") as temporary:
            code, result = self._run(pathlib.Path(temporary), "selected", (
                "--eawr-skirmish-players", "3,4",
                "--eawr-skirmish-slot", "3:Underworld:1:human",
                "--eawr-skirmish-slot", "4:Empire:0:ai",
                "--eawr-skirmish-fleet", "3:none",
                "--eawr-skirmish-fleet", "4:Star_Destroyer",
                "--eawr-skirmish-seed", "908",
                "--eawr-live-ai", "off", "--eawr-audio", "off",
                "--eawr-live-step", "1", "--eawr-live-ticks", "8",
                "--eawr-map-timed-frames", "8"),
                session=("--eawr-live-session", "skirmish"), camera=None,
                map_name="data/art/maps/_mp_space_polus.ted")
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["start_map"], "data/art/maps/_mp_space_polus.ted")
            self.assertEqual(live["start_seed"], 908)
            self.assertEqual(live["local_player"], 3)
            spawns = {row["player"]: row["record"] for row in live["start_markers"]
                      if row["use"] == "spawn"}
            self.assertEqual(set(spawns), {3, 4})
            for player, unit_type in ((3, "starviper_squadron"), (4, "star_destroyer")):
                units = [row for row in live["start_fleet"]
                         if row["player"] == player and row["type"].lower() == unit_type]
                self.assertTrue(units, live["start_fleet"])
                self.assertTrue(all(row["record"] == spawns[player] for row in units))
            self.assertTrue(live["headless_hashes_equal"])

    def test_corvette_moves_and_turns_with_equal_hashes(self):
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-") as temporary:
            directory = pathlib.Path(temporary)
            ticks = ",".join(str(tick) for tick in CAPTURE_TICKS)
            code, result = self._run(directory, "live", (
                *ORDERS, "--eawr-live-capture-ticks", ticks, "--eawr-live-workers", "4",
                "--eawr-live-hashes", str(directory / "live.hashes.csv"),
                "--eawr-live-replay-out", str(directory / "live.eawr-replay")))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual((live["fixture"], live["pacing"], live["workers"], live["local_player"]),
                             ("m2", "driven", 4, 1))
            self.assertEqual(live["rejected"], [])
            self.assertGreaterEqual(live["completed_ticks"], CAPTURE_TICKS[-1])
            # The viewer-attached run equals a headless run of its recorded command stream.
            self.assertIs(live["headless_hashes_equal"], True)
            rows = (directory / "live.hashes.csv").read_text(encoding="utf-8").splitlines()
            self.assertEqual(rows[0], "tick,sha256")
            self.assertEqual(len(rows) - 1, live["completed_ticks"])
            self.assertEqual(rows[-1].split(",")[1], live["final_state_sha256"])
            self.assertTrue((directory / "live.eawr-replay").is_file())
            # Own units are drawn; the Empire fleet and the unseen map objects are hidden.
            self.assertGreater(live["visible_units"], 0)
            self.assertGreater(live["hidden_units"], 0)
            units = result["populate"]["live_units"]
            self.assertEqual(units["session_records_skipped"], live["session_records"])
            # A squadron company is its team container and has no model (#75). Its craft are start
            # units: the X-wings draw; the Y-wing and TIE interceptor models do not compose through
            # the placed-ship path yet (fidelity list, #80). The laser pads draw since #80.
            self.assertTrue(all("Squadron" in item or "(Y-Wing)" in item or "(TIE_Interceptor)" in item
                                for item in units["not_drawn"]), units["not_drawn"])
            self.assertFalse(any("(X-Wing)" in item for item in units["not_drawn"]), units["not_drawn"])
            self.assertEqual(units["drawn"], live["units"] - len(units["not_drawn"]))
            # The corvette leaves the stack and turns: consecutive captures differ.
            frames = []
            for tick in CAPTURE_TICKS:
                capture = directory / f"live_t{tick:04d}.png"
                self.assertIn(capture.name, result["captures"])
                frames.append(decode_png(capture.read_bytes()))
            for before, after in zip(frames, frames[1:]):
                self.assertNotEqual(before[2], after[2])

            # The same orders with one worker end on the same state.
            code, single = self._run(directory, "single", (
                *ORDERS, "--eawr-live-ticks", str(CAPTURE_TICKS[-1]), "--eawr-live-workers", "1"))
            self.assertEqual(code, 0, single.get("failure"))
            self.assertEqual(single["live_session"]["completed_ticks"], live["completed_ticks"])
            self.assertEqual(single["live_session"]["final_state_sha256"], live["final_state_sha256"])

    def test_perf_overlay_reports_fps_frame_and_tick_cost(self):
        # #558: `--eawr-perf-overlay on` shows the overlay from the first frame; its report gives the
        # FPS, the frame time, the simulation's cost per tick and the overlay's own cost. The overlay
        # reads timers only, so the tick hashes equal a run without it, and the F3 key toggles it.
        with tempfile.TemporaryDirectory(prefix="eawr-perf-overlay-") as temporary:
            directory = pathlib.Path(temporary)
            common = (*ORDERS, "--eawr-live-ticks", "240", "--eawr-live-workers", "2")
            code, plain = self._run(directory, "plain", (
                *common, "--eawr-live-hashes", str(directory / "plain.hashes.csv")))
            self.assertEqual(code, 0, plain.get("failure"))
            self.assertIs(plain["perf_overlay"]["shown"], False, "off by default")
            self.assertEqual(plain["perf_overlay"]["key"], "F3")

            code, shown = self._run(directory, "shown", (
                *common, "--eawr-perf-overlay", "on", "--eawr-live-hashes", str(directory / "shown.hashes.csv")))
            self.assertEqual(code, 0, shown.get("failure"))
            overlay = shown["perf_overlay"]
            self.assertIs(overlay["shown"], True)
            self.assertGreater(overlay["frames"], 100)
            self.assertGreater(overlay["fps"], 0.0)
            self.assertGreater(overlay["frame_ms"], 0.0)
            self.assertGreaterEqual(overlay["frame_ms_worst"], overlay["frame_ms_avg"])
            self.assertEqual(overlay["tick_source"], "live_session")
            self.assertGreater(overlay["ticks"], 0)
            self.assertGreater(overlay["tick_ms"], 0.0)
            self.assertGreaterEqual(overlay["tick_ms_worst"], overlay["tick_ms_avg"])
            self.assertEqual([phase["name"] for phase in overlay["tick_phases"]], ["step", "fog"])
            self.assertIsNotNone(overlay["visible_units"])
            self.assertGreater(overlay["own_cost_ms"]["samples"], 100)
            self.assertLess(overlay["own_cost_ms"]["avg"], 0.1, overlay["own_cost_ms"])
            self.assertGreater(overlay["rect"][2], 0)
            # A timer read never enters a hash.
            self.assertIs(shown["live_session"]["headless_hashes_equal"], True)
            plain_rows = (directory / "plain.hashes.csv").read_text(encoding="utf-8").splitlines()[1:]
            shown_rows = (directory / "shown.hashes.csv").read_text(encoding="utf-8").splitlines()[1:]
            common_rows = min(len(plain_rows), len(shown_rows))
            self.assertGreaterEqual(common_rows, 240)
            self.assertEqual(plain_rows[:common_rows], shown_rows[:common_rows])

            # The key: F3 shows it, a second F3 hides it again, and a third shows it once more.
            code, keyed = self._run(directory, "keyed", (
                *common, "--eawr-live-input", "f20:key:F3", "--eawr-live-input", "f60:key:F3",
                "--eawr-live-input", "f80:key:F3"))
            self.assertEqual(code, 0, keyed.get("failure"))
            self.assertEqual((keyed["perf_overlay"]["shown"], keyed["perf_overlay"]["toggles"]), (True, 3))
            self.assertGreater(keyed["perf_overlay"]["frames"], 10)
            code, bad = self._run(directory, "bad", ("--eawr-perf-overlay", "maybe"))
            self.assertNotEqual(code, 0)
            self.assertIn("--eawr-perf-overlay expects on or off", bad["failure"])

    def test_reveal_draws_the_hidden_empire_fleet(self):
        # #80: --eawr-live-reveal on now works with --eawr-live-session m2 too (a viewer debug
        # aid), not only replay. The Empire station (unit 7) sits behind the Rebel local
        # player's fog at the start; revealed, it draws (and lists in live_session.hostile_units)
        # while the simulation, the AI's commands among them, is unaffected: the two runs'
        # per-tick hashes are identical.
        with tempfile.TemporaryDirectory(prefix="eawr-live-reveal-") as temporary:
            directory = pathlib.Path(temporary)
            code, hidden = self._run(directory, "hidden", (
                "--eawr-live-ticks", "10", "--eawr-live-hashes", str(directory / "hidden.hashes.csv")))
            self.assertEqual(code, 0, hidden.get("failure"))
            live = hidden["live_session"]
            self.assertEqual((live["fixture"], live["reveal"]), ("m2", False))
            self.assertGreater(live["hidden_units"], 0)
            self.assertFalse(any(unit["entity"] == EMPIRE_STATION for unit in live["hostile_units"]),
                             live["hostile_units"])

            code, revealed = self._run(directory, "revealed", (
                "--eawr-live-reveal", "on", "--eawr-live-ticks", "10",
                "--eawr-live-hashes", str(directory / "revealed.hashes.csv")))
            self.assertEqual(code, 0, revealed.get("failure"))
            live = revealed["live_session"]
            self.assertEqual((live["fixture"], live["reveal"]), ("m2", True))
            self.assertEqual(live["hidden_units"], 0)
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertTrue(any(unit["entity"] == EMPIRE_STATION for unit in live["hostile_units"]),
                            live["hostile_units"])
            # Presentation-only: reveal never reaches the m2 session, so both runs' ticks hash
            # the same (the AI's decisions, its commands and the recorded state are unchanged).
            self.assertEqual((directory / "hidden.hashes.csv").read_text(encoding="utf-8"),
                             (directory / "revealed.hashes.csv").read_text(encoding="utf-8"))

    def test_replay_resolves_map_object_types(self):
        # #501: an m2 recording's units include its map objects (e.g. unit 12,
        # Skirmish_Merchant_Dock), which are not in the FoC unit tables. Replaying the
        # recording back through --eawr-live-replay must resolve them the same way the live
        # path places them (LiveSessionView::prepare_replay), not just against the unit
        # tables. A plain replay of the same window reaches the exact recorded state
        # (bit-identical, sensors unchanged); --eawr-live-reveal legitimately diverges from
        # it past the point a hidden target enters its extended range (revealed_sensor_table,
        # space-targeting R-08 - it feeds targeting, not just presentation), so that leg only
        # checks it loads and stays internally consistent, as the other reveal replays do.
        ticks = 300
        with tempfile.TemporaryDirectory(prefix="eawr-live-replay-mapobj-") as temporary:
            directory = pathlib.Path(temporary)
            code, recorded = self._run(directory, "record", (
                "--eawr-live-ticks", str(ticks),
                "--eawr-live-replay-out", str(directory / "record.eawr-replay")))
            self.assertEqual(code, 0, recorded.get("failure"))
            live = recorded["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertTrue((directory / "record.eawr-replay").is_file())

            code, replayed = self._run(directory, "replay", (
                "--eawr-live-replay", str(directory / "record.eawr-replay"), "--eawr-live-ticks", str(ticks)),
                session=("--eawr-live-session", "replay"))
            self.assertEqual(code, 0, replayed.get("failure"))
            replay = replayed["live_session"]
            self.assertIs(replay["reveal"], False)
            self.assertIs(replay["headless_hashes_equal"], True)
            self.assertEqual(replay["final_state_sha256"], live["final_state_sha256"])

            code, revealed = self._run(directory, "replay_revealed", (
                "--eawr-live-replay", str(directory / "record.eawr-replay"), "--eawr-live-ticks", str(ticks),
                "--eawr-live-reveal", "on"), session=("--eawr-live-session", "replay"))
            self.assertEqual(code, 0, revealed.get("failure"))
            reveal = revealed["live_session"]
            self.assertIs(reveal["reveal"], True)
            self.assertIs(reveal["headless_hashes_equal"], True)

    def test_duel_fires_hits_and_explodes(self):
        # #80: the duel plays with the combat table bound; its shots draw as kites (Nebulon-B
        # turbolasers) and beams (Tartan lasers), hits spawn shield and detonation effects and the
        # Tartan's death its Death_Explosions, all from the snapshots: the hashes stay headless.
        with tempfile.TemporaryDirectory(prefix="eawr-live-duel-") as temporary:
            directory = pathlib.Path(temporary)
            ticks = ",".join(str(tick) for tick in DUEL_CAPTURE_TICKS)
            code, result = self._run(directory, "duel", (
                *DUEL_ARGS, "--eawr-live-capture-ticks", ticks, "--eawr-live-ticks", "750"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual((live["fixture"], live["reveal"], live["local_player"]), ("replay", True, 2))
            # #494 FW-14: a revealed map draws no fog plane.
            self.assertEqual(result["live_fog"]["status"], "revealed")
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["hidden_units"], 0)
            effects = result["battle_effects"]
            self.assertIsNone(effects["failure"])
            self.assertGreater(effects["max_kites"], 0)
            # BP-02: every kite's head (the short, glow-centred end) leads its flight.
            self.assertGreater(effects["kites_head_leading"], 0)
            self.assertEqual(effects["kites_head_trailing"], 0)
            self.assertGreater(effects["max_beams"], 0)
            self.assertEqual(effects["projectiles_not_drawn"], {})
            spawned = effects["spawned"]
            self.assertGreater(spawned.get("shield:Projectile_Shield_Absorb_Medium", 0)
                               + spawned.get("shield:Projectile_Shield_Absorb_Large", 0), 0, spawned)
            # #415 (BP-17 to BP-19): both duel ships have a SHIELD sub-object, and every shield hit
            # is placed where the shot's line first meets their collision meshes, facing that
            # triangle's normal. The stations have none.
            shields = effects["shield_hits"]
            for name in ("Nebulon_B_Frigate", "Tartan_Patrol_Cruiser"):
                self.assertIs(shields["meshes"][name]["shield"], True, shields)
                self.assertGreater(shields["meshes"][name]["triangles"], 0, shields)
            self.assertIs(shields["meshes"]["Skirmish_Rebel_Star_Base_1"]["shield"], False, shields)
            # A shot the collision box took that passes every mesh keeps the box contact and faces
            # back along the flight (BP-19); most meet a mesh.
            self.assertEqual(shields["no_mesh"], 0, shields)
            self.assertGreater(shields["on_mesh"], 3 * shields["mesh_missed"], shields)
            self.assertEqual(shields["on_mesh"] + shields["mesh_missed"],
                             sum(count for key, count in spawned.items() if key.startswith("shield:")), shields)
            # The cast starts at the projectile's frame-step start (BP-19): each mesh hit is met in
            # the step or ahead of it, one cast per shield hit, and the broad phase keeps the
            # triangles tested far below every triangle of every cast.
            self.assertEqual(shields["in_step"] + shields["ahead"], shields["on_mesh"], shields)
            casts = shields["casts"]
            self.assertEqual(casts["casts"], shields["on_mesh"] + shields["mesh_missed"], shields)
            largest = max(mesh["triangles"] for mesh in shields["meshes"].values())
            self.assertLess(casts["max_triangles_per_cast"], largest, shields)
            self.assertLess(casts["triangles_tested"], casts["casts"] * largest / 4, shields)
            for sample in shields["samples"]:
                self.assertAlmostEqual(sum(value * value for value in sample["direction"]), 1.0, places=2)
            self.assertTrue(any(sample["placed"] != sample["contact"] for sample in shields["samples"]), shields)
            # #438 (BP-21): retail shows no visible hull flash, so it stays off by default.
            # #76: without --eawr-live-defend the simulation's DEFEND drives the shell: the replay's
            # Rebel player is not human, so the Nebulon-B's script switches DEFEND on once the Tartan's
            # fire exceeds 20 a second (space-abilities AB-41; retail's frigate takes less, AB-U1).
            self.assertIs(live["defend"], False)
            self.assertIs(live["shield_flash"], False)
            self.assertEqual(live["shield_flashes"], 0)
            shells = result["populate"]["live_units"]["shield_shells"]
            self.assertEqual(shells, {"types": {"Nebulon_B_Frigate": "composed"}, "max_shown": 1})
            self.assertGreater(spawned.get("detonation:Large_Damage_Space", 0), 0, spawned)
            self.assertEqual(spawned.get("death:Large_Explosion_Space_Empire"), 1, spawned)
            self.assertEqual(effects["spawn_failed"], {})
            self.assertEqual(effects["effects_dropped"], 0)
            self.assertIs(effects["spawn_log_full"], False)
            for tick in DUEL_CAPTURE_TICKS:
                self.assertIn(f"duel_t{tick:04d}.png", result["captures"])
            # #370 review 1: every hit the player saw spawned its impact, tick by tick, the
            # Tartan's fatal hit included (its target left the session that same tick).
            hits = {int(tick): count for tick, count in effects["hit_events"].items()}
            self.assertEqual(hit_rows_per_tick(effects), hits)
            deaths = [row for row in effects["spawn_log"] if row[1] == "death:Large_Explosion_Space_Empire"]
            self.assertEqual([row[0] for row in deaths], [DUEL_DEATH_TICK])
            self.assertGreaterEqual(hit_rows_per_tick(effects).get(DUEL_DEATH_TICK, 0), 1, effects["hit_events"])
            # #370 review 2: at --eawr-live-step 3 a frame presenting tick 3k reaches ticks 3k-1,
            # 3k and 3k+1; an effect of tick t is born at presented tick t - 1, so it is first
            # drawn at age 2, 1 or 0, the last reached tick's at 0.
            ages = {}
            for tick, key, first_age in effects["spawn_log"]:
                self.assertIsNotNone(first_age, (tick, key))
                self.assertEqual(first_age, (1 - tick) % 3, (tick, key))
                ages[first_age] = ages.get(first_age, 0) + 1
            self.assertEqual(sorted(ages), [0, 1, 2], ages)

    def test_missiles_fly_as_models_with_their_trails(self):
        # #456 (BP-60 to BP-62): each concussion missile flies on a model slot of its type's pool,
        # and its p_concussion trail starts once for its whole flight, the catch-up samples of a
        # three-tick step included. No M2 type names a hit particle list (BP-63).
        with tempfile.TemporaryDirectory(prefix="eawr-live-missiles-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "missiles", (
                "--eawr-live-replay", str(S28), "--eawr-live-reveal", "on", "--eawr-live-player", "2",
                "--eawr-live-step", "3", "--eawr-live-ticks", "300"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            effects = result["battle_effects"]
            self.assertIsNone(effects["failure"])
            self.assertEqual(effects["projectiles_not_drawn"], {})
            pool = effects["projectile_models"]["Proj_Ship_Concussion_Missile"]
            self.assertEqual((pool["slots"], pool["refused"]), (32, 0), pool)
            self.assertGreater(pool["bindings"], 0, pool)
            self.assertGreater(effects["projectile_models_drawn"], 0)
            self.assertGreater(effects["spawned"].get("detonation:Conc_Missile_Detonation_Particle", 0), 0)
            self.assertEqual(effects["hit_picks"], {"by_projectile": 0, "by_event": 0})
            units = result["populate"]["live_units"]
            # BP-66: the ion override has its own prepared pool, even when this replay never fires it.
            ion_pool = effects["projectile_models"]["Proj_Ion_Cannon_Medium_Laser_Blue"]
            self.assertEqual((ion_pool["slots"], ion_pool["bindings"], ion_pool["refused"]), (32, 0, 0), ion_pool)
            self.assertEqual((units["projectile_slots_drawn"], units["projectile_slots_not_drawn"]), (64, []))
            self.assertEqual(result["unit_emitters"]["started"].get("p_concussion"), pool["bindings"],
                             result["unit_emitters"]["started"])

    def test_missile_trails_reach_their_true_last_tick_at_every_step(self):
        # #491: a catch-up sample must answer from the binding valid at the historical tick it
        # is sampling, not the one this frame's single bind() call just computed - or a missile
        # ending between two frames loses the last tick(s) of its trail to a slot bind() already
        # freed. --eawr-live-step only paces how the fixed replay is presented; at step 1 a
        # frame never reaches more than one new tick, so the catch-up loop never runs and
        # "projectile_model_last_tick" (BattleEffects, keyed by projectile ID) is exactly the
        # sim's own truth. Any --eawr-live-step > 1 must compute the identical value per missile;
        # an aggregate count (bindings, projectile_models_drawn) cannot see a trail cut short by
        # one or two ticks, so this compares the per-projectile last tick itself.
        last_tick_by_step = {}
        for step in (1, 2, 3, 5):
            with tempfile.TemporaryDirectory(prefix=f"eawr-live-missile-step{step}-") as temporary:
                directory = pathlib.Path(temporary)
                code, result = self._run(directory, f"missile_step{step}", (
                    "--eawr-live-replay", str(S28), "--eawr-live-reveal", "on", "--eawr-live-player", "2",
                    "--eawr-live-step", str(step), "--eawr-live-ticks", str(S28_LIVE_TICKS)),
                    session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
                self.assertEqual(code, 0, result.get("failure"))
                self.assertIs(result["live_session"]["headless_hashes_equal"], True)
                effects = result["battle_effects"]
                self.assertIsNone(effects["failure"])
                pool = effects["projectile_models"]["Proj_Ship_Concussion_Missile"]
                self.assertEqual(pool["refused"], 0, pool)
                self.assertGreater(pool["bindings"], 0, pool)
                # Only missiles the --eawr-live-ticks window itself reached: the simulation thread
                # keeps ticking after the presented view holds there, so a run can see a later
                # missile the window never actually presented (not an #491 symptom).
                last_tick_by_step[step] = {int(key): value for key, value in effects["projectile_model_last_tick"].items()
                                            if value <= S28_LIVE_TICKS}
        baseline = last_tick_by_step[1]
        self.assertTrue(baseline, "step 1 (no catch-up) should have posed at least one missile")
        for step, last_tick in last_tick_by_step.items():
            self.assertEqual(last_tick, baseline,
                              f"--eawr-live-step {step} must pose every missile's trail to the exact same "
                              "last tick as step 1, tick by tick, not just in aggregate (#491)")

    def test_lane_mute_keeps_the_audio_event_log(self):
        with tempfile.TemporaryDirectory(prefix="eawr-lane-mute-") as temporary:
            directory = pathlib.Path(temporary)
            results = []
            for name, flag, mute in (("explicit", "off", "0"), ("lane", "on", "1")):
                code, report = self._run(directory, name, (
                    *DUEL_ARGS, "--eawr-live-ticks", "750", "--eawr-live-audio-pace", "on",
                    "--eawr-audio", flag, "--eawr-live-input", "30:click:unit=1",
                    "--eawr-live-input", "60:rclick:unit=2"),
                    session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA,
                    env=dict(os.environ, EAWR_AUDIO_MUTE=mute))
                self.assertEqual(code, 0, report.get("failure"))
                self.assertIs(report["battle_audio"]["muted"], True)
                self.assertIs(report["live_session"]["headless_hashes_equal"], True)
                results.append(report)
            before, after = results
            self.assertEqual(before["live_session"]["final_state_sha256"],
                             after["live_session"]["final_state_sha256"])
            for field in ("requested", "responses", "music", "abilities", "ability_starts"):
                self.assertEqual(before["battle_audio"][field], after["battle_audio"][field], field)
            self.assertTrue(after["battle_audio"]["requested"])
            self.assertTrue(after["battle_audio"]["responses"])

    def test_duel_plays_its_sounds(self):
        # #84: the duel's shots, hits and the Tartan's death start FoC's SFXEvents, the player's
        # selection and attack order the Nebulon-B's responses, and the first Rebel shot the battle
        # music; the capture run is muted by default and the session's hashes stay headless.
        # #474: --eawr-live-audio-pace on caps the render rate to the battle's own tick rate, so
        # Godot's real audio engine schedules the same sound on every host regardless of how fast
        # it can otherwise draw frames (docs/behaviour/battle-audio.md#474).
        with tempfile.TemporaryDirectory(prefix="eawr-live-audio-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "audio", (
                *DUEL_ARGS, "--eawr-live-ticks", "750", "--eawr-live-capture-ticks", "720",
                "--eawr-live-audio-pace", "on",
                "--eawr-live-input", "30:click:unit=1", "--eawr-live-input", "60:rclick:unit=2"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            self.assertIs(sound["muted"], True)
            self.assertGreater(sound["sfx_events"], 3000)
            self.assertEqual((sound["space_3d"]["saturation_factor"], sound["space_3d"]["rolloff_factor"],
                              sound["space_3d"]["listener_z"]), (1.5, 2.0, 60.0))
            # Nothing the duel asks for is missing or undecodable.
            self.assertEqual(sound["missing_samples"], {})
            self.assertEqual(sound["problems"], [])
            requested = sound["requested"]
            self.assertGreater(sum(n for key, n in requested.items() if key.startswith("fire:")), 0, requested)
            self.assertGreater(sum(n for key, n in requested.items() if key.startswith("hit:")), 0, requested)
            self.assertEqual(requested.get("death:Unit_Corvette_Death_SFX"), 1, requested)
            results = sound["results"]
            self.assertGreater(results["fire"].get("playing", 0), 0, results)
            self.assertGreater(results["hit"].get("playing", 0), 0, results)
            self.assertEqual(results["death"], {"playing": 1}, results)
            self.assertNotIn("sample_missing", results["fire"])
            self.assertLessEqual(sound["max_voices"], 48)
            # #443: at the duel camera the shots and hits are heard as FoC balances them against
            # the music (its data volumes, BA-11 falloff and the 0.75 sliders, BA-45): the mixer
            # meters sound on the SFX bus for most of the fight, within a sane band of the music,
            # and the mix never reaches full scale. Before the fix the 3D players had no listener
            # and the SFX bus stayed silent (no frame above -60 dBFS).
            levels = sound["levels"]
            sfx, music, mix = levels["EAWR_SFX"], levels["EAWR_Music"], levels["EAWR_Mix"]
            self.assertGreater(sfx["frames"], sound["frames"] // 2, levels)
            self.assertGreater(music["frames"], sound["frames"] // 2, levels)
            self.assertGreaterEqual(sfx["rms_db"], music["rms_db"] - 6.0, levels)
            self.assertLessEqual(sfx["rms_db"], music["rms_db"] + 12.0, levels)
            self.assertLessEqual(mix["max_peak_db"], 0.0, levels)
            self.assertEqual(mix["clipped_frames"], 0, levels)
            self.assertLess(sound["start_gain_db"]["fire"]["mean"], 0.0)
            self.assertGreater(sound["start_gain_db"]["fire"]["mean"], -20.0)
            # BA-20 to BA-22: the Nebulon-B answers the click and the attack order on the Tartan.
            self.assertEqual(sound["responses"], ["select Nebulon_B_Frigate Unit_Select_Nebulon",
                                                  "attack Nebulon_B_Frigate Unit_Attack_Nebulon"])
            self.assertEqual(results["response_select"], {"playing": 1}, results)
            # BA-41 to BA-43: Rebel ambient first, then battle music with the first Rebel shot.
            music = sound["music"]
            self.assertIn(" ambient Space_Map_Rebel_Ambient_Music_Event Imperial_Attack_1.MP3", music[0])
            self.assertTrue(any(" battle Space_Map_Rebel_Battle_Music_Event UND_Unexpected_Forces.MP3" in row
                                for row in music), music)
            self.assertEqual(sound["music_mode"], "battle")

    def test_squadron_selection_plays_its_craft_lines(self):
        # #499 (docs/behaviour/battle-audio.md BA-24): a squadron's team container has no response
        # sounds or ranking category of its own (every M2 squadron); the FoC debug build resolves a
        # selected squadron to its leading live craft for both the unit-response ranking and the
        # sound played. Before the fix the container's own (missing) fields meant a selected
        # squadron played no line at all -- the owner's "no voice lines for fighters/bombers yet".
        # #424, tests/presentation/renderer/test_battle_input.py: the M2 start (sim_headless
        # --skirmish m2) Rebel X-wing squadron 2, Y-wing squadron 4, Empire TIE Interceptor
        # squadron 9; clicking a squadron's icon selects it regardless of where its craft draw.
        # Every Rebel company starts stacked on the same spawn marker, so their icons coincide on
        # screen and a click there hits whichever the pointer resolution finds first; SPREAD (test_
        # battle_input.py) moves the corvette and both X-wing squadrons apart first, leaving the
        # Y-wing squadron and the MC80 (unit 7) at the stack; the icon takes the click first (WU-23).
        x_wing_squadron, y_wing_squadron, tie_squadron = 2, 4, 9
        spread = ("--eawr-live-order", "1:move:5@-4450,5050,0", "--eawr-live-order", "1:move:2@-5450,5250,0",
                  "--eawr-live-order", "1:move:3@-4650,4250,0")
        with tempfile.TemporaryDirectory(prefix="eawr-live-squadron-audio-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "rebel-squadrons", (
                *spread, "--eawr-hud", "off", "--eawr-live-ticks", "90",
                "--eawr-live-input", f"30:click:icon={x_wing_squadron}",
                "--eawr-live-input", f"60:click:icon={y_wing_squadron}"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            self.assertEqual(sound["responses"], [
                "select X-Wing Unit_Select_X_Wing",
                "select Y-Wing Unit_Select_Y_Wing",
            ])
            self.assertEqual(sound["results"].get("response_select"), {"playing": 2}, sound["results"])

            # The Empire player's view: TIE Interceptor squadron 9 stands at its own spawn, far from
            # the Rebel-side default camera, so this run points at it instead (coruscant-empire-
            # live-session-camera.xml).
            empire_camera = ROOT / "apps/viewer/project/config/coruscant-empire-live-session-camera.xml"
            code, result = self._run(directory, "empire-squadron", (
                "--eawr-hud", "off", "--eawr-live-ticks", "60", "--eawr-live-player", "2",
                "--eawr-live-input", f"30:click:icon={tie_squadron}"), camera=empire_camera)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            self.assertEqual(sound["responses"], ["select TIE_Interceptor Unit_Select_Tie_Interceptor"])
            self.assertEqual(sound["results"].get("response_select"), {"playing": 1}, sound["results"])

    def test_squadron_selection_survives_its_leader_dying(self):
        # #499 review: the squadron's leading craft must be resolved by liveness, not simply the
        # first roster member the audio system last stood next to -- a dead craft stays in that
        # position map until its destruction event reaches the report, a tick or more after it
        # actually leaves the tactical snapshot. This run kills the Y-wing squadron's first two
        # craft (#424, test_battle_input.py: squadron 4, craft 46-48 in roster order) before the
        # icon is clicked. Every M2 squadron is one craft type, so a leader-agnostic roster walk
        # would happen to land on the same sound anyway; the regression this guards is a dropped
        # response (the resolution falling through to the team container's own, missing fields), not
        # a wrong one.
        y_wing_squadron = 4
        y_wings = (46, 47, 48)
        spread = ("--eawr-live-order", "1:move:5@-4450,5050,0", "--eawr-live-order", "1:move:2@-5450,5250,0",
                  "--eawr-live-order", "1:move:3@-4650,4250,0")
        with tempfile.TemporaryDirectory(prefix="eawr-live-squadron-leader-death-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "y-wing-leader-death", (
                *spread, "--eawr-hud", "off", "--eawr-live-ticks", "90",
                "--eawr-live-order", f"15:damage:{y_wings[0]}@100000",
                "--eawr-live-order", f"16:damage:{y_wings[1]}@100000",
                "--eawr-live-input", f"30:click:icon={y_wing_squadron}"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            results = sound["results"]
            self.assertEqual(results.get("death"), {"playing": 2}, results)
            self.assertEqual(sound["responses"], ["select Y-Wing Unit_Select_Y_Wing"])
            self.assertEqual(results.get("response_select"), {"playing": 1}, results)

    def test_ability_presses_play_their_sounds(self):
        # #559 (docs/behaviour/battle-audio.md BA-50 to BA-52): switching an ability on or off plays
        # its faction's toggle sound ("swhoong" of the S-foils, the shields) and, from the command
        # bar, the unit's own voice line; a natural end and the enemy's switches play nothing. The
        # box selects the whole Rebel fleet and each modelled ability's button is pressed on, then
        # off (the cut HUNT and ION_CANNON_SHOT are refused, AB-03).
        width, height = 1280, 720
        box = f"screen={0.05 * width:.3f},{0.05 * height:.3f}/screen={0.95 * width:.3f},{0.95 * height:.3f}"
        boxed = ("--eawr-live-input", f"20:box:{box}")
        with tempfile.TemporaryDirectory(prefix="eawr-live-ability-audio-") as temporary:
            directory = pathlib.Path(temporary)
            code, shown = self._run(directory, "buttons", (*boxed, "--eawr-live-ticks", "60"))
            self.assertEqual(code, 0, shown.get("failure"))
            buttons = shown["hud"]["ability_buttons"]["buttons"]
            modelled = [index for index, button in enumerate(buttons)
                        if button["name"] in ("DEFEND", "TURBO", "SPOILER_LOCK")]
            self.assertEqual(sorted(buttons[index]["name"] for index in modelled), ["DEFEND", "SPOILER_LOCK", "TURBO"])
            presses = []
            for step, index in enumerate(modelled):
                presses += ["--eawr-live-input", f"{30 + 6 * step}:click:ability={index}",
                            "--eawr-live-input", f"{120 + 6 * step}:click:ability={index}"]
            code, result = self._run(directory, "presses", (*boxed, *presses, "--eawr-live-ticks", "200"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            self.assertEqual(sound["missing_samples"], {})
            self.assertEqual(sound["problems"], [])
            log = sound["abilities"]
            for row in (
                "voice Nebulon_B_Frigate DEFEND on Unit_Defend_Nebulon",
                "voice Nebulon_B_Frigate DEFEND off <none>",
                "voice Corellian_Corvette TURBO on Unit_Speed_Corvette",
                "voice Corellian_Corvette TURBO off <none>",
                "voice X-Wing SPOILER_LOCK on Unit_Ability_On_X_Wing",
                "voice X-Wing SPOILER_LOCK off Unit_Ability_Off_X_Wing",
                "toggle Rebel DEFEND on GUI_Toggle_Shields_On",
                "toggle Rebel DEFEND off GUI_Toggle_Shields_Off",
                "toggle Rebel TURBO on GUI_Toggle_Turbo_On",
                "toggle Rebel TURBO off GUI_Toggle_Turbo_Off",
                "toggle Rebel SPOILER_LOCK on GUI_Toggle_Turbo_On",
                "toggle Rebel SPOILER_LOCK off GUI_Toggle_Turbo_Off",
            ):
                self.assertIn(row, log)
            # One toggle sound per switch, however many craft a squadron has (BA-50).
            self.assertEqual(sum(1 for row in log if row.startswith("toggle Rebel SPOILER_LOCK on")), 1, log)
            requested = sound["requested"]
            self.assertEqual(requested.get("ability_toggle:GUI_Toggle_Turbo_On"), 2, requested)
            self.assertEqual(requested.get("ability_voice:Unit_Speed_Corvette"), 1, requested)
            self.assertEqual(requested.get("ability_voice:Unit_Ability_Off_X_Wing"), 1, requested)
            # The enemy's own switches (the Tartan's POWER_TO_WEAPONS has none, and an enemy's
            # toggle entries are blank) never play.
            self.assertFalse(any(" enemy " in row and not row.endswith("<none>") for row in log), log)

    def test_timed_abilities_that_run_out_play_no_off_sound(self):
        # #559 (BA-51): a timed ability that ends by itself starts no sound. TURBO (20 s, 600 ticks)
        # and DEFEND (15 s, 450 ticks) are switched on by script and left to expire. The snapshot of
        # a timed ability's last active tick reads no frames left (like an untimed one), so an
        # end-of-time that only looked at the frames left played the off toggle at tick 650.
        with tempfile.TemporaryDirectory(prefix="eawr-live-ability-expiry-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "expiry", (
                "--eawr-live-order", "2:move:5@4000,4700,0", "--eawr-live-order", "50:ability:5@TURBO,on",
                "--eawr-live-order", "60:ability:6@DEFEND,on", "--eawr-live-ticks", "700"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            log = sound["abilities"]
            self.assertIn("toggle Rebel TURBO on GUI_Toggle_Turbo_On", log)
            self.assertIn("toggle Rebel DEFEND on GUI_Toggle_Shields_On", log)
            self.assertIn("expired TURBO", log)
            self.assertIn("expired DEFEND", log)
            self.assertFalse(any(row.startswith("toggle") and " off " in row for row in log), log)
            starts = sound["ability_starts"]
            self.assertFalse(any(row["event"].endswith("_Off") for row in starts), starts)
            self.assertEqual(sorted(row["event"] for row in starts), ["GUI_Toggle_Shields_On", "GUI_Toggle_Turbo_On"])
            requested = sound["requested"]
            self.assertIsNone(requested.get("ability_toggle:GUI_Toggle_Turbo_Off"), requested)
            self.assertIsNone(requested.get("ability_toggle:GUI_Toggle_Shields_Off"), requested)

    def test_switching_a_timed_ability_off_plays_its_off_sound(self):
        # #559 (BA-50): TURBO switched off by script before its time is out is a switch-off, not a
        # natural end: the faction's off toggle plays once, at the tick of the switch, and nothing
        # is logged as expired.
        with tempfile.TemporaryDirectory(prefix="eawr-live-ability-off-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "off", (
                "--eawr-live-order", "2:move:5@4000,4700,0", "--eawr-live-order", "50:ability:5@TURBO,on",
                "--eawr-live-order", "170:ability:5@TURBO,off", "--eawr-live-ticks", "260"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            log = sound["abilities"]
            self.assertEqual([row for row in log if row.startswith("toggle")],
                             ["toggle Rebel TURBO on GUI_Toggle_Turbo_On", "toggle Rebel TURBO off GUI_Toggle_Turbo_Off"], log)
            self.assertFalse(any(row.startswith("expired") for row in log), log)
            starts = sound["ability_starts"]
            self.assertEqual([row["event"] for row in starts], ["GUI_Toggle_Turbo_On", "GUI_Toggle_Turbo_Off"], starts)
            self.assertAlmostEqual(starts[1]["tick"], 170, delta=4)

    def test_enemy_switches_play_the_enemy_row(self):
        # #559 (BA-50): with the Empire as the local player the Rebel corvette's TURBO is an enemy's
        # switch: the faction's enemy rows are blank, so nothing plays (the log names the blank row).
        with tempfile.TemporaryDirectory(prefix="eawr-live-ability-enemy-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "enemy", (
                "--eawr-live-player", "2", "--eawr-live-order", "50:ability:5@TURBO,on",
                "--eawr-live-order", "110:ability:5@TURBO,off", "--eawr-live-ticks", "160"))
            self.assertEqual(code, 0, result.get("failure"))
            sound = result["battle_audio"]
            self.assertEqual([row for row in sound["abilities"] if row.startswith("toggle")],
                             ["toggle Rebel TURBO on enemy <none>", "toggle Rebel TURBO off enemy <none>"], sound["abilities"])
            self.assertEqual(sound["ability_starts"], [])

    def test_turbo_engine_boost_fades_out(self):
        # #559 (battle-presentation BP-65): when TURBO ends the corvette's turbo engine emitters stop
        # emitting and their particles drain instead of vanishing at once, and the normal engine
        # emitters that stood in for them drain the same way when TURBO starts. Corvette 5 of the M2
        # start is switched on at tick 50 and off at tick 170 by script.
        with tempfile.TemporaryDirectory(prefix="eawr-live-turbo-drain-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "turbo", (
                "--eawr-live-order", "2:move:5@4000,4700,0", "--eawr-live-order", "50:ability:5@TURBO,on",
                "--eawr-live-order", "170:ability:5@TURBO,off", "--eawr-live-ticks", "330"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            drains = result["unit_emitters"]["engine_drains"]
            self.assertGreaterEqual(drains["started"], 2, drains)
            self.assertEqual(drains["cut_short"], 0, drains)
            self.assertEqual(drains["finished"], drains["started"], drains)
            self.assertIn("PTE_Corvetteengines", result["unit_emitters"]["started"])

    def test_shield_flash_opt_in(self):
        # #438: preserve the debug-build BP-21 light-scale path as an explicit viewer option.
        with tempfile.TemporaryDirectory(prefix="eawr-live-shield-flash-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "flash", (
                *DUEL_ARGS, "--eawr-live-shield-flash", "on", "--eawr-live-ticks", "750"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["shield_flash"], True)
            self.assertGreater(live["shield_flashes"], 0)
            self.assertIs(live["headless_hashes_equal"], True)

    def test_defend_shows_the_shield_shell(self):
        # #427 (BP-22 to BP-24): the Nebulon-B (the duel's DEFEND type; the MC80 is not in S-15) shows its SHIELD
        # sub-object with its MeshShield.fx material while DEFEND runs; --eawr-live-defend forces it
        # on. The simulation's DEFEND (#76) can switch on at tick 30 at the earliest (its damage-rate
        # window, space-abilities AB-42), and since #536 the Tartan's lasers land about 8 damage at a
        # time, as in the S-15 recording, so the frigate's own DEFEND waits for the scripted tick-500
        # hit (AB-U1): only the forced shell shows. The shell is presentation only: the hashes stay
        # headless.
        with tempfile.TemporaryDirectory(prefix="eawr-live-defend-") as temporary:
            directory = pathlib.Path(temporary)
            frames = {}
            for name, defend in (("defend", "on"), ("plain", "off")):
                code, result = self._run(directory, name, (
                    *DUEL_ARGS, "--eawr-live-defend", defend, "--eawr-live-capture-ticks", "24",
                    "--eawr-live-ticks", "24"), session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
                self.assertEqual(code, 0, result.get("failure"))
                live = result["live_session"]
                self.assertIs(live["defend"], defend == "on")
                self.assertIs(live["headless_hashes_equal"], True)
                shells = result["populate"]["live_units"]["shield_shells"]
                self.assertEqual(shells, {"types": {"Nebulon_B_Frigate": "composed"},
                                          "max_shown": 1 if defend == "on" else 0})
                frames[name] = decode_png((directory / f"{name}_t0024.png").read_bytes())
            self.assertNotEqual(frames["defend"][2], frames["plain"][2])

    def test_attack_order_turns_the_tartan(self):
        # #361: `--eawr-live-order <tick>:attack:<unit>@<target>` reaches the session as an attack
        # command. In the duel the Tartan (unit 2, player 1) faces the Nebulon-B (unit 1) 800
        # units to its west; a face order turns it north (yaw 90) by about tick 140. Ordered at
        # tick 200 to attack the Nebulon-B, which is within its 800-unit attack distance, it
        # turns in place back onto the frigate's nearest hardpoint (space-weapon-fire A-04);
        # without the order it keeps facing north.
        with tempfile.TemporaryDirectory(prefix="eawr-live-attack-") as temporary:
            directory = pathlib.Path(temporary)
            face = ("--eawr-live-order", "30:face:2@400,0,0")
            args = ("--eawr-live-replay", str(DUEL), "--eawr-live-reveal", "on", "--eawr-live-player", "1",
                    "--eawr-live-ticks", "400")
            runs = {}
            for name, extra in (("faced", face), ("attack", (*face, "--eawr-live-order", "200:attack:2@1"))):
                code, result = self._run(directory, name, (*args, *extra),
                                         session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
                self.assertEqual(code, 0, result.get("failure"))
                live = result["live_session"]
                self.assertEqual(live["rejected"], [])
                self.assertIs(live["headless_hashes_equal"], True)
                self.assertGreaterEqual(live["completed_ticks"], 400)
                runs[name] = live
            orders = runs["attack"]["orders"]
            self.assertIn({"tick": 200, "kind": "attack", "unit": 2, "target": 1}, orders)
            yaw = {name: {unit["entity"]: unit["yaw"] for unit in live["own_units"]}[2] for name, live in runs.items()}
            self.assertAlmostEqual(yaw["faced"], 90.0, delta=0.01)
            # The turn back ends on the bearing to the Nebulon-B's nearest hardpoint, within a
            # few degrees of due west.
            self.assertLess(abs(abs(yaw["attack"]) - 180.0), 5.0, yaw)
            self.assertNotEqual(runs["faced"]["final_state_sha256"], runs["attack"]["final_state_sha256"])

    def test_duel_stall_keeps_every_event(self):
        # #370 review 3: a frame after a presentation stall reaches far more ticks than the
        # snapshot history keeps (64). The session's event log still hands it every tick's
        # hits and deaths: the Tartan's fatal hit, its death explosion and its death clone all
        # arrive, with the same hits as the unstalled run, and no event gap is reported.
        with tempfile.TemporaryDirectory(prefix="eawr-live-stall-") as temporary:
            directory = pathlib.Path(temporary)
            runs = {}
            for name, extra in (("paced", ()), ("stalled", ("--eawr-live-stall", "600:150"))):
                code, result = self._run(directory, name, (*DUEL_ARGS, "--eawr-live-ticks", "750", *extra),
                                         session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
                self.assertEqual(code, 0, result.get("failure"))
                self.assertIs(result["live_session"]["headless_hashes_equal"], True)
                self.assertEqual(result["live_session"]["event_gaps"], [])
                self.assertIsNone(result["battle_effects"]["failure"])
                self.assertEqual(result["battle_effects"]["effects_dropped"], 0)
                runs[name] = result
            stalled = runs["stalled"]
            self.assertEqual(stalled["live_session"]["stall"], {"tick": 600, "ticks": 150})
            # The frame after tick 600 presents 753: ticks 602 to 754 arrive at once.
            self.assertEqual(stalled["live_session"]["latest_tick"], 754)
            hits = {int(tick): count for tick, count in stalled["battle_effects"]["hit_events"].items()}
            paced = {int(tick): count for tick, count in runs["paced"]["battle_effects"]["hit_events"].items()}
            self.assertTrue(any(601 < tick < 690 for tick in paced), paced)
            self.assertEqual({tick: count for tick, count in hits.items() if tick <= 751}, paced)
            self.assertEqual(hit_rows_per_tick(stalled["battle_effects"]), hits)
            # The death (born at presented tick 717) is 36 ticks old when the frame presenting 753
            # reaches it, past its 30-frame lifetime: it is skipped, not spawned to drain at once
            # (#370 re-review 2), and stays in the spawn log without a drawn age.
            deaths = [row for row in stalled["battle_effects"]["spawn_log"]
                      if row[1] == "death:Large_Explosion_Space_Empire"]
            self.assertEqual(deaths, [[DUEL_DEATH_TICK, "death:Large_Explosion_Space_Empire", None]])
            self.assertEqual(stalled["battle_effects"]["expired"].get("death:Large_Explosion_Space_Empire"), 1)
            self.assertIsNone(stalled["battle_effects"]["spawned"].get("death:Large_Explosion_Space_Empire"))
            self.assertGreaterEqual(hits.get(DUEL_DEATH_TICK, 0), 1)
            clones = stalled["live_session"]["death_clones_shown"] + stalled["live_session"]["death_clones_retired"]
            self.assertEqual([(row["unit"], row.get("death_tick")) for row in clones], [(2, DUEL_DEATH_TICK)])
            # A capture tick inside the skip is refused.
            code, result = self._run(directory, "skipped", (
                *DUEL_ARGS, "--eawr-live-stall", "600:150", "--eawr-live-capture-ticks", "690"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertNotEqual(code, 0)
            self.assertIn("falls in the --eawr-live-stall skip", result["failure"])

    def test_duel_startup_stall_ages_every_effect(self):
        # #370 re-review 2: the session reaches tick 150 before the viewer's first frame. That
        # frame fires the hits of ticks 1 to 151 at once; each effect is born at its own tick
        # (presented tick - 1) and aged by the ticks since, so it is first drawn at age
        # 151 - tick, and one whose Particle_Lifetime_Frames are already over is skipped rather
        # than spawned with the rest at age zero.
        with tempfile.TemporaryDirectory(prefix="eawr-live-startup-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "startup", (
                *DUEL_ARGS, "--eawr-live-ticks", "300", "--eawr-live-stall", "start:150"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["stall"], {"tick": "start", "ticks": 150})
            self.assertEqual(live["event_gaps"], [])
            effects = result["battle_effects"]
            self.assertIsNone(effects["failure"])
            self.assertEqual(effects["effects_dropped"], 0)
            hits = {int(tick): count for tick, count in effects["hit_events"].items()}
            self.assertEqual(hit_rows_per_tick(effects), hits)
            types = effects["particle_types"]
            aged = expired = 0
            for tick, key, first_age in effects["spawn_log"]:
                if tick > 151:
                    continue
                lifetime = types[key.split(":", 1)[1]]["lifetime_frames"]
                self.assertGreater(lifetime, 0, key)
                if 151 - tick >= lifetime:
                    self.assertIsNone(first_age, (tick, key))
                    expired += 1
                else:
                    self.assertEqual(first_age, 151 - tick, (tick, key))
                    aged += first_age > 0
            # The duel fires before tick 150: some early effects are still alive, some over.
            self.assertGreater(aged, 0, effects["spawn_log"][:20])
            self.assertGreater(expired, 0, effects["spawn_log"][:20])
            self.assertEqual(sum(effects["expired"].values()), expired)

    def test_destroyed_corvette_plays_its_death_clone(self):
        # #81 (docs/behaviour/unit-animation.md UA-07, UA-08): scripted damage destroys the
        # corvette at tick 15; its Corellian_Corvette_Death_Clone takes its place and plays
        # rv_corvette_d_die_00 on the presentation clock. The hashes stay the headless ones.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-death-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "death", (
                "--eawr-live-order", f"15:damage:{CORVETTE}@100000", "--eawr-live-capture-ticks", "0,30,60,120",
                "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            clones = {row["unit"]: row for row in live["death_clones"]}
            self.assertEqual(clones[CORVETTE]["clone"], "Corellian_Corvette_Death_Clone")
            self.assertEqual(clones[CORVETTE]["clip"], "data/art/models/rv_corvette_d_die_00.ala")
            self.assertEqual([row["unit"] for row in live["death_clones_shown"]], [CORVETTE])
            # Death_Persistence_Duration -1: the clone holds its last frame and never leaves.
            self.assertEqual((clones[CORVETTE]["clip_type"], clones[CORVETTE]["persistence_ticks"]), ("DIE", None))
            self.assertEqual(live["death_clones_retired"], [])
            corvette = next(row for row in live["unit_clips"] if row["type"] == "Corellian_Corvette")
            self.assertEqual(sum(corvette["clips"].values()), 0)
            self.assertEqual(corvette["clone_clips"]["DIE"], 1)
            units = result["populate"]["live_units"]
            self.assertEqual(units["clips"]["Corellian_Corvette_Death_Clone data/art/models/rv_corvette_d_die_00.ala"],
                             "bound")
            self.assertGreater(units["clip_poses"], 0)
            self.assertEqual(units["drawn"], live["units"] - len(units["not_drawn"]))
            frames = [decode_png((directory / f"death_t{tick:04d}.png").read_bytes()) for tick in (30, 60, 120)]
            for before, after in zip(frames, frames[1:]):
                self.assertNotEqual(before[2], after[2])

    def test_destroyed_hardpoints_throw_their_breakoff_props(self):
        # #391 (docs/behaviour/battle-presentation.md BP-30 to BP-36): scripted damage destroys
        # the Nebulon-B's five hardpoints at ticks 15 to 75. The four weapons throw their
        # Hardpoint_Breakoff_Nebulon_Weapon_* props with their fires; the engines throw none.
        # Every prop lives 15 to 25 whole seconds, then leaves with its Medium_Explosion_Space.
        # Nothing reaches the simulation: the hashes stay the headless ones.
        orders = []
        for index, tick in enumerate((15, 30, 45, 60, 75)):
            orders += ["--eawr-live-order", f"{tick}:damage:{NEBULON}@100000,{index}"]
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-breakoff-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "breakoff", (
                *orders, "--eawr-live-step", "15", "--eawr-live-capture-ticks", "15,90,300",
                "--eawr-live-ticks", "900", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            props = result["breakoff_props"]
            self.assertIsNone(props["error"])
            nebulon = [row for row in props["prepared"] if row["unit"] == NEBULON]
            self.assertEqual([(row["hardpoint"], row["prop"], row["status"]) for row in nebulon],
                             [(index, f"Hardpoint_Breakoff_Nebulon_Weapon_{side}", "ready")
                              for index, side in enumerate(("FL", "FR", "BL", "BR"))])
            self.assertTrue(all(row["fire"] == "Space_Debris_Fire_Large" for row in nebulon))
            self.assertTrue(all(row["explosion"] == "Medium_Explosion_Space" for row in nebulon))
            spawned = [row for row in props["spawned"] if row["unit"] == NEBULON]
            self.assertEqual([row["hardpoint"] for row in spawned], [0, 1, 2, 3])
            # Each at the tick its hardpoint died: the order's tick, or the one after it.
            for row, ordered in zip(spawned, (15, 30, 45, 60)):
                self.assertIn(row["tick"] - ordered, (0, 1), row)
            for row in spawned:
                self.assertEqual(row["lifetime_frames"] % 30, 0)
                self.assertTrue(450 <= row["lifetime_frames"] <= 750, row)
            # By tick 900 every prop has run out and left with its explosion.
            self.assertEqual(props["live"], 0)
            self.assertEqual(sorted((row["unit"], row["hardpoint"]) for row in props["expired"]),
                             [(NEBULON, index) for index in range(4)])
            self.assertEqual(props["effects_started"].get("fire:Space_Debris_Fire_Large"), 4)
            self.assertEqual(props["effects_started"].get("death:Medium_Explosion_Space"), 4)
            self.assertEqual(props["effects_failed"], {})
            units = result["populate"]["live_units"]
            self.assertEqual(units["breakoffs_drawn"], len([row for row in props["prepared"] if row["status"] == "ready"]))
            # Expired props are hidden, not retired: a repaired hardpoint can throw its prop again.
            self.assertEqual(units["ships_retired"], 0)
            for name in ("breakoff_t0015.png", "breakoff_t0090.png", "breakoff_t0300.png"):
                self.assertIn(name, result["captures"])

    def test_breakoff_props_after_a_first_frame_stall(self):
        # #401 review 1 and 2: the first frame presents tick 200 and reaches the Nebulon-B's
        # hardpoint deaths of about ticks 15 and 150 at once. Tick 15's snapshot has left the
        # 64-tick history, so what the player saw then is unknown and that prop is not thrown
        # (never this frame's visibility instead); tick 150's is kept and its prop is thrown. The
        # props' effect clock starts at the oldest reached event, not at tick 200, so the fire
        # born at tick 149 or 150 keeps its age.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-breakoff-stall-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "breakoff_stall", (
                "--eawr-live-order", f"15:damage:{NEBULON}@100000,0",
                "--eawr-live-order", f"150:damage:{NEBULON}@100000,1",
                "--eawr-live-stall", "start:200", "--eawr-live-step", "15", "--eawr-live-ticks", "300",
                "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            props = result["breakoff_props"]
            self.assertIsNone(props["error"])
            spawned = [row for row in props["spawned"] if row["unit"] == NEBULON]
            self.assertEqual([row["hardpoint"] for row in spawned], [1], props["spawned"])
            self.assertIn(spawned[0]["tick"] - 150, (0, 1), spawned[0])
            self.assertGreaterEqual(props["unknown"], 1)
            self.assertLessEqual(props["clock_start"], spawned[0]["tick"] - 1)
            self.assertGreaterEqual(props["effects_started"].get("fire:Space_Debris_Fire_Large", 0), 1)

    def test_breakoff_explosion_ended_during_a_stall_is_skipped(self):
        # #401 review 2: the Nebulon-B's front-left prop is thrown at about tick 15 and dies 15 to
        # 25 s later (by presented tick 765), inside a stall that jumps from tick 30 to 945. Its
        # 30-frame Medium_Explosion_Space ended long before that frame: it is not started.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-breakoff-late-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "breakoff_late", (
                "--eawr-live-order", f"15:damage:{NEBULON}@100000,0",
                "--eawr-live-stall", "30:900", "--eawr-live-step", "15", "--eawr-live-ticks", "1000",
                "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            props = result["breakoff_props"]
            self.assertIsNone(props["error"])
            self.assertEqual([row["hardpoint"] for row in props["spawned"] if row["unit"] == NEBULON], [0])
            expired = [row for row in props["expired"] if row["unit"] == NEBULON]
            self.assertEqual([row["hardpoint"] for row in expired], [0])
            self.assertLess(expired[0]["death_tick"], 900)
            self.assertGreaterEqual(props["effects_skipped"].get("death:Medium_Explosion_Space", 0), 1)
            if all(row["death_tick"] < 900 for row in props["expired"]):
                self.assertIsNone(props["effects_started"].get("death:Medium_Explosion_Space"))

    def test_death_clones_without_a_clip_and_after_their_fade(self):
        # #363 review 2 and 3 (UA-08, DeathBehavior): with a death type the clone models lack
        # (CRUSHED), the corvette clone (Remove_Upon_Death) is removed at once and the Rebel
        # station's clone (no Remove_Upon_Death) keeps its pose; with a 1 s persistence it fades
        # 1 s + 0.25 s after it appeared and leaves: its ship is retired, its uploads released.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-clone-rules-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "rules", (
                "--eawr-live-order", f"15:damage:{STATION}@1000000", "--eawr-live-order", f"15:damage:{CORVETTE}@100000",
                "--eawr-live-death-anim", "CRUSHED", "--eawr-live-death-persistence", "1",
                "--eawr-live-capture-ticks", "0,30,90", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            # #77 (space-victory VT-05): the Rebel station's loss is the local player's defeat.
            self.assertEqual(live["outcome"], {
                "condition": "enemy_starbase_destroyed", "winner": 2, "winner_team": 1, "decided_tick": 15,
                "deciding_unit": STATION, "end_tick": 225, "local_result": "defeat"})
            clones = {row["unit"]: row for row in live["death_clones"]}
            self.assertEqual((clones[CORVETTE]["clip_type"], clones[CORVETTE]["clip"], clones[CORVETTE]["remove_upon_death"]),
                             ("CRUSHED", "", True))
            self.assertIn("removed at once", clones[CORVETTE]["status"])
            self.assertEqual((clones[STATION]["clone"], clones[STATION]["clip"], clones[STATION]["remove_upon_death"]),
                             ("Rebel_Star_Base_1_Death_Clone", "", False))
            self.assertIn("keeps its pose", clones[STATION]["status"])
            self.assertEqual(live["death_clones_shown"], [])
            self.assertEqual([(row["unit"], row["reason"], row["clone_tick"]) for row in live["death_clones_retired"]],
                             [(STATION, "faded out", 38)])
            units = result["populate"]["live_units"]
            # Both stations' clones are composed; the capital ships' clones never are.
            self.assertEqual(units["clones_drawn"], 2)
            self.assertEqual(units["drawn"], live["units"] - len(units["not_drawn"]))
            self.assertEqual(units["ships_retired"], 1)
            self.assertGreater(units["released_assets"], 0)
            # The corvette (in the camera's view; the station is not) is gone at tick 30 with no clone.
            frames = [decode_png((directory / f"rules_t{tick:04d}.png").read_bytes()) for tick in (0, 30)]
            self.assertNotEqual(frames[0][2], frames[1][2])

            # The corvette clone with its DIE clip and no persistence leaves once the clip has
            # ended and its fade has run out.
            code, result = self._run(directory, "fade", (
                "--eawr-live-order", f"15:damage:{CORVETTE}@100000", "--eawr-live-death-persistence", "0",
                "--eawr-live-ticks", "600", "--eawr-live-step", "3", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["death_clones_shown"], [])
            self.assertEqual([(row["unit"], row["reason"]) for row in live["death_clones_retired"]],
                             [(CORVETTE, "faded out")])
            self.assertIsNone(live["outcome"], "a corvette's loss decides nothing (VT-02)")
            self.assertGreater(result["populate"]["live_units"]["clip_poses"], 0)
            self.assertEqual(result["populate"]["live_units"]["ships_retired"], 1)

    def test_fog_plane_follows_the_local_fog_cells(self):
        # #494 (space-fog-presentation.md FW-01 to FW-13): the Rebel player's fog is drawn in the
        # world from the session's own fog cells, one texel per 100-unit cell of the map's fog grid
        # (Coruscant +-6500: 130 cells, V-18), and the session's hashes are unchanged.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-fog-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "fog", ("--eawr-live-ticks", "40", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            fog = result["live_fog"]
            self.assertEqual(fog["status"], "drawn", fog["notes"])
            self.assertFalse(fog["revealed"])
            self.assertEqual((fog["colour"], fog["height"], fog["cell_size"], fog["regrow_seconds"]),
                             ([255, 255, 255, 255], -80, 100, 6))
            self.assertEqual(fog["fade_step_per_frame"], 21 / 16)
            self.assertTrue(fog["grid_texture"].endswith("w_space_fow_grid.dds"), fog["grid_texture"])
            self.assertEqual(fog["source"], "cells")
            self.assertEqual({key: fog["grid"][key] for key in ("left", "top", "cell", "wide", "tall")},
                             {"left": -6500, "top": 6500, "cell": 100, "wide": 130, "tall": 130})
            self.assertGreater(fog["cell_tick"], 0)
            self.assertGreater(fog["revealers"], 0)
            self.assertGreater(fog["held_cells"], 0)
            # Most of the square is fogged at the start; the Rebel fleet's corner is not.
            self.assertGreater(fog["fogged_cells"], 130 * 130 // 2)
            self.assertGreater(fog["uploads"], 0)
            self.assertGreater(fog["advanced_logical_frames"], 0)

    def test_enemy_ship_ramps_across_the_fog_edge(self):
        # #535 (space-fog-presentation.md FW-16 to FW-18, K-4): the Rebel corvette sails towards the
        # Empire fleet; the Acclamator (unit 11) enters the fog's held cells near tick 1885 and
        # leaves them near tick 2227. Its opacity eases up and then down over a few frames instead
        # of stepping, and the session's hashes are unchanged (presentation only).
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-fade-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "fade", (
                "--eawr-live-order", f"15:move:{CORVETTE}@2500,-2600,0", "--eawr-live-ticks", "2400",
                "--eawr-live-step", "3", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            rows = [(row["tick"], row["opacity"]) for row in live["fading_log"] if row["entity"] == ACCLAMATOR]
            rising = [(tick, opacity) for tick, opacity in rows if tick < 2100]
            falling = [(tick, opacity) for tick, opacity in rows if tick >= 2100]
            self.assertGreaterEqual(len(rising), 5, rows)
            self.assertGreaterEqual(len(falling), 5, rows)
            for series, sign in ((rising, 1), (falling, -1)):
                values = [opacity for _, opacity in series]
                self.assertTrue(all(sign * (after - before) >= 0 for before, after in zip(values, values[1:])), series)
                self.assertTrue(all(abs(after - before) <= 0.5 for before, after in zip(values, values[1:])),
                                f"a step, not a ramp: {series}")
                self.assertGreaterEqual(sum(1 for value in values if 0.05 < value < 0.95), 3, series)
            self.assertLess(rising[0][1], 0.5)
            self.assertGreater(rising[-1][1], 0.99)
            self.assertLess(falling[-1][1], 0.1)

    def test_reveal_draws_everything_at_full_opacity(self):
        # #535 with #507: --eawr-live-reveal on shows every unit at full opacity; nothing fades.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-fade-reveal-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "revealed", (
                "--eawr-live-reveal", "on", "--eawr-live-order", f"15:move:{CORVETTE}@2500,-2600,0",
                "--eawr-live-ticks", "2400", "--eawr-live-step", "3", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual((live["fading_units"], live["fading"], live["fading_log"]), (0, [], []))

    def test_deployment_overlay_draws_the_red_grid(self):
        # #563 (space-fog-presentation.md FW-22, FW-23, K-5): the overlay is opt-in and draws with
        # the reinforcement tile; the session's hashes are unchanged.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-overlay-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "overlay", (
                "--eawr-live-deploy-overlay", "on", "--eawr-live-ticks", "40", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            fog = result["live_fog"]
            self.assertEqual(fog["status"], "drawn", fog["notes"])
            self.assertIs(fog["deploy_overlay"], True)
            self.assertEqual(fog["overlay_colour"], [255, 0, 0, 254])
            self.assertTrue(fog["grid_texture"].endswith("w_space_reinforce_fow_grid.dds"), fog["grid_texture"])

    def test_victory_is_reported(self):
        # #77 (space-victory VT-05, VT-10): the Empire station falls at tick 15; the live session
        # decides the local player's victory and its headless replay, with the same rules, agrees.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-victory-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "victory", (
                "--eawr-live-order", f"15:damage:{EMPIRE_STATION}@1000000", "--eawr-live-ticks", "60",
                "--eawr-live-step", "3", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["outcome"], {
                "condition": "enemy_starbase_destroyed", "winner": 1, "winner_team": 0, "decided_tick": 15,
                "deciding_unit": EMPIRE_STATION, "end_tick": 225, "local_result": "victory"})

    def _battle_end(self, directory, name, station, result_name, extra=()):
        # #453: the station falls at tick 15, so the battle ends at 225 (VT-11). Step 3: frame n
        # shows tick 3(n - 1); tick 225 is frame 76.
        return self._run(directory, name, (
            "--eawr-live-order", f"15:damage:{station}@1000000", "--eawr-live-ticks", "300",
            "--eawr-live-step", "3", "--eawr-live-workers", "2", "--eawr-live-capture-ticks", "12,18",
            # A capture shows the HUD as the frame before drew it: the end panel, opened on frame 76,
            # is taken a few frames later.
            "--eawr-live-capture-frames", "80",
            "--eawr-live-replay-out", str(directory / f"{name}.eawr-replay"), *extra))

    def test_victory_shows_the_message_and_ends_the_battle(self):
        # #453 (battle-end.md BE-02, BE-03, BEP-01 to BEP-03): "WE ARE VICTORIOUS!" from the frame
        # that carries the outcome, the session halts at end_tick 225 and the end panel opens; the
        # run stops at the end, however many ticks it asked for.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-end-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._battle_end(directory, "won", EMPIRE_STATION, "victory")
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["completed_ticks"], 225, "the session halts at end_tick")
            end = live["battle_end"]
            self.assertEqual((end["message"], end["title"], end["decided_tick"], end["end_tick"], end["halt_tick"]),
                             ("TEXT_WIN_TACTICAL", "TEXT_VICTORY", 15, 225, 225))
            self.assertEqual(end["ended_frame"], 76)
            self.assertEqual(live["time"]["state"], "ended")
            self.assertEqual(live["time"]["track"][-1]["cause"], "end")
            overlay = result["hud"]["battle_overlay"]
            self.assertEqual(overlay["message"]["text"], "WE ARE VICTORIOUS!")
            self.assertEqual(overlay["end_panel"]["title"], "Victory!")
            # BE-03: centred, its top at 0.4 of the height.
            x, y, width, _ = overlay["message"]["rect"]
            self.assertAlmostEqual(x + width / 2, 640.0, delta=1.0)
            self.assertAlmostEqual(y, 288.0, delta=1.0)
            # The message is on the frame after the outcome and not before; the panel at the end.
            images = [decode_png((directory / name).read_bytes()) for name in ("won_t0012.png", "won_t0018.png", "won_f0080.png")]
            self.assertNotEqual(images[0][2], images[1][2])
            self.assertNotEqual(images[1][2], images[2][2])
            self.assertEqual((directory / "won.eawr-replay.time.csv").read_text(encoding="utf-8").splitlines(),
                             ["tick,state,ticks_per_second", "225,ended,0"])

    def test_defeat_shows_the_message_and_quit_ends_the_run(self):
        # #453: the Rebel station falls: "WE HAVE BEEN DEFEATED!", then "Defeat!"; the end panel's
        # Quit Game ends the run before its remaining frames.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-defeat-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._battle_end(directory, "lost", STATION, "defeat",
                                            ("--eawr-live-input", "f80:click:hud=quit"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual((live["battle_end"]["message"], live["battle_end"]["title"]),
                             ("TEXT_LOSE_TACTICAL", "TEXT_DEFEAT"))
            self.assertEqual(live["outcome"]["local_result"], "defeat")
            self.assertIs(live["battle_end"]["quit"], True)
            self.assertEqual(live["completed_ticks"], 225)
            overlay = result["hud"]["battle_overlay"]
            self.assertEqual(overlay["message"]["text"], "WE HAVE BEEN DEFEATED!")
            self.assertEqual((overlay["end_panel"]["title"], overlay["quit_presses"]), ("Defeat!", 1))
            # Quit ends the run on frame 82 (the pointer reaches the button on 80, the click is
            # sent on 81 and dispatched on 82), before the 30 warm-up and 120 timed frames end.
            self.assertLess(live["frames"], 120)

    def test_time_panel_pauses_and_fast_forwards_without_changing_the_hashes(self):
        # #459 (tactical-time-controls.md TM-05 to TM-09, TP-01 to TP-06): the time panel's buttons,
        # clicked as a player would (the pointer reaches a button on the frame given, the click is
        # dispatched two frames later). Pause at frame 10,
        # a fast-forward press while paused does nothing, Resume Game plays, fast forward runs four
        # times as many ticks per frame, then back to normal speed and another pause and play. The
        # debug orders run at their ticks either way, so a plain run ends on the same state.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-time-") as temporary:
            directory = pathlib.Path(temporary)
            inputs = []
            for gesture in ("f10:click:hud=pause", "f12:click:hud=fast_forward", "f20:click:hud=resume",
                            "f22:click:hud=fast_forward", "f30:click:hud=fast_forward", "f32:click:hud=pause",
                            "f34:click:hud=pause"):
                inputs += ["--eawr-live-input", gesture]
            code, result = self._run(directory, "time", (
                *ORDERS, *inputs, "--eawr-live-ticks", "240", "--eawr-live-step", "3", "--eawr-live-workers", "2",
                "--eawr-live-capture-frames", "10,15,25", "--eawr-live-hashes", str(directory / "time.hashes.csv"),
                "--eawr-live-replay-out", str(directory / "time.eawr-replay")))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["headless_hashes_equal"], True)
            track = [(row["state"], row["ticks_per_second"], row["cause"]) for row in live["time"]["track"]]
            self.assertEqual(track, [("paused", 0, "pause"), ("play", 30, "resume"),
                                     ("fast_forward", 120, "fast_forward"), ("play", 30, "normal_speed"),
                                     ("paused", 0, "pause"), ("play", 30, "play")])
            ticks = [row["tick"] for row in live["time"]["track"]]
            self.assertEqual(ticks[0], ticks[1], "no tick ran while paused")
            self.assertEqual(ticks[2] - ticks[1], 6, "two frames at step 3 from Resume Game to fast forward")
            self.assertEqual(ticks[3] - ticks[2], 8 * 12, "eight fast-forward frames of 12 ticks")
            panel = result["hud"]["time_panel"]
            self.assertEqual(panel["fast_forward"]["presses"], 2, "the press while paused never reached the button")
            self.assertEqual(panel["pause"]["presses"], 3)
            self.assertEqual(result["hud"]["battle_overlay"]["resume_presses"], 1)
            csv = (directory / "time.eawr-replay.time.csv").read_text(encoding="utf-8").splitlines()
            self.assertEqual(csv[0], "tick,state,ticks_per_second")
            self.assertEqual(len(csv), 7)
            # Paused frames show the banner and hold the battle: frames 15 and 25 straddle Resume Game.
            paused = decode_png((directory / "time_f0015.png").read_bytes())
            playing = decode_png((directory / "time_f0025.png").read_bytes())
            self.assertNotEqual(paused[2], playing[2])
            code, plain = self._run(directory, "plain", (
                *ORDERS, "--eawr-live-ticks", "240", "--eawr-live-step", "3", "--eawr-live-workers", "2",
                "--eawr-live-hashes", str(directory / "plain.hashes.csv")))
            self.assertEqual(code, 0, plain.get("failure"))
            # Both runs draw at least the 150 warm-up and timed frames, so fast forward reaches
            # further; every tick both ran has the same hash.
            timed = (directory / "time.hashes.csv").read_text(encoding="utf-8").splitlines()[1:]
            untimed = (directory / "plain.hashes.csv").read_text(encoding="utf-8").splitlines()[1:]
            common = min(len(timed), len(untimed))
            self.assertGreaterEqual(common, 240)
            self.assertEqual(timed[:common], untimed[:common])

    def test_an_order_given_while_paused_runs_at_the_next_tick(self):
        # #459 TM-10, TP-03: select the corvette and order it on while paused; the order is stamped
        # for the tick the pause stopped before, and runs there once the battle plays.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-paused-order-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "queued", (
                "--eawr-live-input", "f10:click:hud=pause", "--eawr-live-input", f"f12:click:unit={CORVETTE}",
                "--eawr-live-input", "f13:rclick:@-4000,5500,0", "--eawr-live-input", "f16:click:hud=pause",
                "--eawr-live-ticks", "90", "--eawr-live-step", "3", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["rejected"], [])
            paused_at = live["time"]["track"][0]["tick"]
            notes = [note for note in result["battle_input"]["log"] if note.startswith("move @")]
            self.assertEqual(len(notes), 1, result["battle_input"]["log"])
            self.assertTrue(notes[0].endswith(f" tick {paused_at}"), (notes, paused_at))

    def test_simulation_fault_is_reported(self):
        # #316 review 2: an exception on the simulation thread fails the capture run in order,
        # with the error in the report, instead of terminating the process.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-fault-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "fault", (
                "--eawr-live-fault-tick", "6", "--eawr-live-capture-ticks", "0,30"))
            # The hook's std::runtime_error comes from the extension, built with
            # _HAS_EXCEPTIONS=0, so the session sees it as a non-standard exception.
            self.assertEqual(code, 2, result.get("failure"))
            self.assertIn("simulation thread stopped before tick 6", result["failure"])
            live = result["live_session"]
            self.assertEqual(live["status"], "simulation_failed")
            self.assertIn("simulation thread stopped before tick 6", live["error"])
            self.assertEqual(live["completed_ticks"], 6)
            # #615: the replay through the failed tick lands beside godot.log (user://logs).
            replay = pathlib.Path(live["failure_replay"])
            try:
                self.assertEqual(replay.parent.name, "logs", replay)
                self.assertRegex(replay.name, r"^eawr-live-failure-[0-9]+-pid[0-9]+-usec[0-9]+-tick7\.eawr-replay$")
                self.assertTrue(replay.read_bytes().startswith(b"EAWRPLY"), replay)
            finally:
                replay.unlink(missing_ok=True)

    def test_capture_ticks_are_the_ticks_shown(self):
        # #316 review 3: with coarse steps a capture tick must be one a frame shows, and the file
        # is named after it.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-step-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "between", (
                "--eawr-live-step", "30", "--eawr-live-capture-ticks", "5"))
            self.assertNotEqual(code, 0)
            self.assertIn("falls between frames", result["failure"])
            code, result = self._run(directory, "coarse", (
                "--eawr-live-step", "30", "--eawr-live-capture-ticks", "0,30,60"))
            self.assertEqual(code, 0, result.get("failure"))
            for name in ("coarse_t0000.png", "coarse_t0030.png", "coarse_t0060.png"):
                self.assertIn(name, result["captures"])
            # This run plays the battle for thousands of ticks. An FoC AI order may meet a target
            # that died in the tick before (target_not_live), which is the battle's course, not
            # the capture path (#520: PC-08 changes that course); no other order is rejected.
            live = result["live_session"]
            ai_players = set(live["ai"]["players"])
            self.assertEqual([entry for entry in live["rejected"]
                              if not (any(f"player {player}," in entry for player in ai_players)
                                      and "target_not_live" in entry)], [])

    def test_unit_emitters_follow_engines_and_destroyed_hardpoints(self):
        # #394 (battle-presentation BP-40 to BP-43): the live units run their own engine emitters,
        # never the turbo ones hidden at creation, and a destroyed hardpoint starts its damage
        # emitters. Destroying the Nebulon-B's engines stops its engine emitter and starts the
        # two damage emitters below HP_E_EmitDamage. Presentation only: the hashes stay headless.
        with tempfile.TemporaryDirectory(prefix="eawr-live-emitters-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "emitters", (
                "--eawr-live-ticks", "150",
                "--eawr-live-order", f"15:move:{NEBULON}@-4400,5400,0",
                "--eawr-live-order", f"60:damage:{NEBULON}@5000,{NEBULON_FL}",
                "--eawr-live-order", f"105:damage:{NEBULON}@5000,{NEBULON_ENGINES}"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            emitters = result["unit_emitters"]
            self.assertIsNone(emitters["failure"])
            self.assertEqual(emitters["start_failed"], {})
            started = emitters["started"]
            self.assertEqual(started.get("pe_nebulonengines"), 1, started)
            # FL: one damage emitter; the engines: two more, and the engine emitter stops.
            self.assertEqual(started.get("p_hp_stardestroyer_damage"), 3, started)
            self.assertEqual(emitters["stopped"].get("hardpoint_state"), 1, emitters["stopped"])
            self.assertGreater(emitters["max_particles"], 0)
            # The Empire player's view: the Tartan runs its "pe" engines, never its "pte" ones.
            code, result = self._run(directory, "tartan", ("--eawr-live-ticks", "30", "--eawr-live-player", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            emitters = result["unit_emitters"]
            started = emitters["started"]
            self.assertEqual(started.get("pe_tartanengine_sml"), 1, started)
            self.assertEqual(started.get("pe_tartanengine_lrg"), 1, started)
            self.assertFalse(any(name.lower().startswith("pte") for name in started), started)
            self.assertTrue(any(key.lower().startswith("pte_tartanengine") and "hidden at creation" in key
                                for key in emitters["not_admitted"]), emitters["not_admitted"])

    def test_death_clone_pieces_burn_until_they_vanish(self):
        # #421 (battle-presentation BP-47 to BP-50): the corvette's death clone runs its own
        # proxies on its DIE clip. Its four p_rebelsmokedeath and its p_debris01 start with the
        # clone and stop emitting (drain) when their pieces vanish; each p_explosion_big00 runs
        # only in the last frame its piece shows (clip frames 74, 80 and 83). Presentation only.
        with tempfile.TemporaryDirectory(prefix="eawr-live-clone-fire-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "clonefire", (
                "--eawr-live-ticks", "150", "--eawr-live-order", f"15:damage:{CORVETTE}@100000"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            emitters = result["unit_emitters"]
            self.assertIsNone(emitters["failure"])
            clones = emitters["death_clones"]
            self.assertEqual(clones["ships"], 1, clones)
            self.assertEqual(clones["failed"], {})
            self.assertEqual(clones["not_run"], {})
            expected = {"p_rebelsmokedeath": 4, "p_debris01": 1, "p_explosion_big00": 3}
            self.assertEqual(clones["started"], expected)
            # By tick 150 the clip (86 frames) has ended with every piece hidden.
            self.assertEqual(clones["hidden"], expected)
            self.assertGreater(clones["max_particles"], 0)
            rows = clones["start_log"]
            first = min(row["born"] for row in rows)
            self.assertTrue(all(row["born"] == first for row in rows if row["proxy"] != "p_explosion_big00"), rows)
            self.assertEqual(sorted(row["born"] - first for row in rows if row["proxy"] == "p_explosion_big00"),
                             [74, 80, 83], rows)
            # The Rebel station's clone: its fire billows, electrical damage and explosion chains
            # emit from their pieces' meshes (EnhancedMesh). In the first 45 clip frames only the
            # proxies its clip shows from frame 0 have started.
            code, result = self._run(directory, "stationfire", (
                "--eawr-live-ticks", "60", "--eawr-live-order", f"15:damage:{STATION}@1000000"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            clones = result["unit_emitters"]["death_clones"]
            self.assertEqual((clones["ships"], clones["failed"], clones["not_run"]), (1, {}, {}), clones)
            self.assertEqual(clones["started"], {"p_fire_billow2": 4, "p_fire_billow5": 1, "p_damage_elec04": 4,
                                                 "p_explosion_chain": 1, "p_explosion_chain3": 5}, clones)
            # #429 review 2 (BP-48): a proxy whose piece shows again is reset, as the debug
            # build resets the group (BP-48): its old drain goes.
            self.assertEqual(clones["reappearance"], "reset", clones)

    def test_death_clone_emitters_across_a_stall_past_the_fade(self):
        # #429 review 1: a stall that jumps from before the corvette's death (tick 15) past its
        # DIE clip and fade (no persistence) still runs the clone's samples at their own ticks:
        # its smoke, debris and last-frame explosions start where a paced run starts them, and
        # the clone's ship is retired only after those samples ran.
        expected = {"p_rebelsmokedeath": 4, "p_debris01": 1, "p_explosion_big00": 3}
        with tempfile.TemporaryDirectory(prefix="eawr-live-clone-stall-") as temporary:
            directory = pathlib.Path(temporary)
            runs = {}
            for name, extra in (("paced", ()), ("stalled", ("--eawr-live-stall", "10:120"))):
                code, result = self._run(directory, name, (
                    "--eawr-live-ticks", "150", "--eawr-live-order", f"15:damage:{CORVETTE}@100000",
                    "--eawr-live-death-persistence", "0", *extra))
                self.assertEqual(code, 0, result.get("failure"))
                live = result["live_session"]
                self.assertIs(live["headless_hashes_equal"], True)
                self.assertEqual([(row["unit"], row["reason"]) for row in live["death_clones_retired"]],
                                 [(CORVETTE, "faded out")])
                self.assertEqual(result["populate"]["live_units"]["ships_retired"], 1)
                emitters = result["unit_emitters"]
                self.assertIsNone(emitters["failure"])
                clones = emitters["death_clones"]
                self.assertEqual((clones["ships"], clones["failed"], clones["not_run"]), (1, {}, {}), clones)
                self.assertEqual(clones["started"], expected, (name, clones))
                self.assertEqual(clones["hidden"], expected, (name, clones))
                runs[name] = emitters
            # The stalled frame presents 130.5: every clone sample before it is caught up. The
            # oldest have left the snapshot history, so the ships skip them (least visible,
            # #406); the clone stands at its unit's last drawn pose and runs them all.
            self.assertGreater(runs["stalled"]["caught_up"], 100)
            self.assertGreater(runs["stalled"]["unknown_samples"], 0)
            births = {name: sorted((row["proxy"], row["born"]) for row in emitters["death_clones"]["start_log"])
                      for name, emitters in runs.items()}
            self.assertEqual(births["stalled"], births["paced"])

    def test_unit_emitters_after_a_stall(self):
        # #406 review: a frame after a presentation stall runs the samples it catches up on at
        # their own ticks. The Nebulon-B moves and loses its FL hardpoint at tick 45, inside a
        # stall that jumps from tick 30 to 90.5 (the default step is half a tick): its damage
        # emitter is born at the tick whose state shows the hardpoint destroyed, where the moving
        # ship stood then (the paced run's birth sample and origin), and is first drawn aged from
        # there, not from the frame's start with the whole skip at the final pose.
        orders = ("--eawr-live-order", f"15:move:{NEBULON}@-4400,5400,0",
                  "--eawr-live-order", f"45:damage:{NEBULON}@5000,{NEBULON_FL}")
        damage = "p_hp_stardestroyer_damage"
        with tempfile.TemporaryDirectory(prefix="eawr-live-emitters-stall-") as temporary:
            directory = pathlib.Path(temporary)
            runs = {}
            for name, extra in (("paced", ()), ("stalled", ("--eawr-live-stall", "30:60"))):
                code, result = self._run(directory, name, ("--eawr-live-ticks", "120", *orders, *extra))
                self.assertEqual(code, 0, result.get("failure"))
                self.assertIs(result["live_session"]["headless_hashes_equal"], True)
                emitters = result["unit_emitters"]
                self.assertIsNone(emitters["failure"])
                self.assertEqual(emitters["start_failed"], {})
                self.assertEqual(emitters["started"].get(damage), 1, emitters["started"])
                self.assertIs(emitters["start_log_full"], False)
                runs[name] = emitters
            paced = {(row["unit"], row["proxy"], row["tick"]): row for row in runs["paced"]["start_log"]}
            stalled = runs["stalled"]
            # Samples 30 to 88 run before the frame's own (89, due 90).
            self.assertEqual(stalled["caught_up"], 59)
            self.assertEqual(stalled["unknown_samples"], 0)
            # A paced frame draws a new emitter after its one sample.
            self.assertTrue(all(row["first_age"] == 1 for row in paced.values() if row["proxy"] == damage), paced)
            rows = [row for row in stalled["start_log"] if row["proxy"] == damage]
            self.assertEqual(len(rows), 1, stalled["start_log"])
            row = rows[0]
            self.assertTrue(31 < row["tick"] <= 90, row)
            match = paced.get((row["unit"], row["proxy"], row["tick"]))
            self.assertIsNotNone(match, (row, sorted(paced)))
            self.assertEqual(row["born"], match["born"], (row, match))
            # First drawn by the frame presenting 90.5 (due 90): aged by the samples since its
            # birth, as the paced run's emitter is at that tick.
            self.assertEqual(row["first_age"], 90 - row["born"], row)
            for axis in range(3):
                self.assertAlmostEqual(row["origin"][axis], match["origin"][axis], delta=0.5, msg=(row, match))
            # Past the snapshot history (a first-frame stall to tick 200, the kill at 45): where
            # the ship stood during the older ticks is unknown, so no emitter runs over them
            # (least visible); the damage emitter starts at the oldest tick the history holds.
            code, result = self._run(directory, "startup", (
                "--eawr-live-ticks", "260", *orders, "--eawr-live-stall", "start:200"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            emitters = result["unit_emitters"]
            self.assertIsNone(emitters["failure"])
            self.assertGreater(emitters["unknown_samples"], 0)
            rows = [row for row in emitters["start_log"] if row["proxy"] == damage]
            self.assertEqual(len(rows), 1, emitters["start_log"])
            self.assertGreater(rows[0]["tick"], 200 - 66, rows[0])
            self.assertEqual(rows[0]["first_age"], 200 - rows[0]["tick"] + 2, rows[0])

    def test_killed_fighters_spin_away_and_explode(self):
        # #447 (docs/behaviour/space-fighter-deaths.md SP-02 to SP-08): under the M2 seed the
        # X-wing 36 hit by the order of tick 150 (it dies in tick 151) spins away: its model flies
        # on and rolls for 60 ticks, then explodes and leaves; the Y-wing 46 (order 152, dies in
        # 153) only explodes. (The MC80 of #537 is unit 7, so craft IDs are one higher than before.)
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-spin-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "spin", (
                "--eawr-live-order", "150:damage:36@1000000", "--eawr-live-order", "152:damage:46@1000000",
                "--eawr-live-follow", "36", "--eawr-live-ticks", "230", "--eawr-live-step", "2",
                "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["spin_away"]["spins"], [{"unit": 36, "started": 151, "ended": 211}])
            self.assertEqual((live["spin_away"]["drawn_max"], live["spin_away"]["ships_max"]), (1, 1))
            spawned = result["battle_effects"]["spawned"]
            self.assertEqual(spawned.get("spin_away:Small_Explosion_Space"), 1)
            self.assertEqual(spawned.get("death:Small_Explosion_Space"), 2)

    def test_fighter_spin_away_in_a_seeded_dogfight(self):
        # #447: the Acclamator leads its TIE escort into the Rebel squadrons; under the M2 seed
        # some of the craft shot down in the dogfight spin away and explode 60 ticks later.
        # 7500 ticks, not 5400 (#465 review): #465's lead and out-of-combat fire against fighters
        # shifts which tick each craft dies on, so the three kills within the first 5400 ticks
        # draw differently than before; the fight keeps producing kills past that window and the
        # first one that spins away lands at tick 5589.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-dogfight-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "dogfight", (
                "--eawr-live-order", "2:move:11@-4300,3900,0", "--eawr-live-ticks", "7500",
                "--eawr-live-step", "30", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["headless_hashes_equal"], True)
            spins = live["spin_away"]["spins"]
            self.assertGreaterEqual(len(spins), 1)
            # 60 ticks, or 60 + k for a craft killed at a roll of exactly -20k degrees, whose path
            # rebuilds k frames in (SP-04, SC-04; unit 81 here spins for 62).
            for spin in spins:
                self.assertIn(spin["ended"] - spin["started"], range(60, 70), spin)
            self.assertGreaterEqual(live["spin_away"]["drawn_max"], 1)

    def test_engine_emitters_are_drawn_every_frame_of_a_turn(self):
        # #433: the corvette turns sharply (faced away, then ordered to its left) at the default
        # half-tick step, so every other frame falls between two 30 Hz emitter samples. Those
        # frames draw its engine emitters again at the pose drawn then, and each engine emitter
        # runs for the whole turn: started once, never stopped and started again. The start log
        # counts the frames between samples that drew each started emitter again (presented), so
        # the engine's own count shows it was drawn between samples through the turn (#439).
        # Presentation only: the hashes stay headless.
        with tempfile.TemporaryDirectory(prefix="eawr-live-glow-turn-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "turn", (
                "--eawr-live-ticks", "150",
                "--eawr-live-order", f"2:face:{CORVETTE}@-5739,3969,0",
                "--eawr-live-order", f"60:move:{CORVETTE}@-3229,2995,0"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            emitters = result["unit_emitters"]
            self.assertIsNone(emitters["failure"])
            self.assertEqual(emitters["start_failed"], {})
            self.assertNotIn("frame_overflow", emitters["stopped"])
            self.assertIs(emitters["start_log_full"], False)
            # PE_Corvetteengines is the corvette's only engine emitter.
            engines = [row for row in emitters["start_log"]
                       if row["unit"] == CORVETTE and row["proxy"].lower() == "pe_corvetteengines"]
            self.assertEqual(len(engines), 1, emitters["start_log"])
            # One frame in two falls between samples; the engine is drawn on each of them from
            # its start to the end of the session, the turn included.
            self.assertGreater(engines[0]["presented"], 0, engines[0])
            self.assertGreaterEqual(engines[0]["presented"], emitters["frames"] // 2 - engines[0]["tick"] - 4,
                                    (engines[0], emitters["frames"]))
            self.assertGreaterEqual(emitters["presented_frames"], engines[0]["presented"], emitters)
            self.assertGreaterEqual(emitters["presented_effects"], emitters["presented_frames"], emitters)

    def assert_map_teardown_before_host_destruction(self, text):
        # #961: Godot deletes the host's child nodes before its extension destructor.
        # The audio players are those children, so releasing the map from that
        # destructor dereferenced freed nodes even when --eawr-audio was off.
        # Check the actual lifecycle trace as well as the exit code: a stale
        # pointer can survive several clean exits before the allocator reuses it.
        teardown = text.index("map mode teardown begins")
        freed = text.index("space view freed")
        host = text.index("viewer host freed (members follow)")
        self.assertLess(teardown, freed, text[-4000:])
        self.assertLess(freed, host, text[-4000:])

    def test_quit_after_teardown_stress(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        with tempfile.TemporaryDirectory(prefix="eawr-live-teardown-") as temporary:
            directory = pathlib.Path(temporary)
            for frames in (300, 450, 600, 900):
                for mode, map_name in (("m2", CORUSCANT),
                                       ("skirmish", "data/art/maps/_mp_space_polus.ted")):
                    with self.subTest(frames=frames, mode=mode):
                        completed = subprocess.run(
                            [executable, "--resolution", "1280x720", "--path",
                             str(ROOT / "apps/viewer/project"), "--quit-after", str(frames), "--",
                             "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                             "--eawr-map", map_name, "--eawr-populate", "--eawr-camera-interactive",
                             "--eawr-live-session", mode, "--eawr-live-ai", "on",
                             "--eawr-map-effects", "on", "--eawr-audio", "off",
                             "--eawr-lighting", "sh", "--eawr-environment", "map",
                             "--eawr-shadows", "on", "--eawr-hud", "tactical",
                             "--eawr-report", str(directory / f"{mode}-{frames}.json")],
                            cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=180, check=False)
                        self.assertEqual(completed.returncode, 0, completed.stdout[-4000:])
                        self.assertIn("EAWR live session started", completed.stdout)
                        self.assertNotIn("headless replay done", completed.stdout)
                        self.assert_map_teardown_before_host_destruction(completed.stdout)

    @unittest.skipUnless(sys.platform == "win32", "closes the viewer's window through Win32")
    def test_window_close_quits_promptly_and_writes_the_report(self):
        # The owner's close of a running battle took seconds: the teardown replayed the whole battle
        # headlessly to compare hashes (one thread, about as long as the battle), and the report was
        # never written. An interactive session skips that replay unless --eawr-live-verify on asks
        # for it, and the window's close request writes the report before the engine quits.
        from window_close import close_windows_of

        with tempfile.TemporaryDirectory(prefix="eawr-live-quit-") as temporary:
            directory = pathlib.Path(temporary)
            report = directory / "quit.json"
            log = directory / "quit.log"
            executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
            self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
            with log.open("wb") as output:
                process = subprocess.Popen(
                    [executable, "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), "--",
                     "--eawr-map", CORUSCANT, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                     "--eawr-report", str(report), "--eawr-populate", "--eawr-camera-interactive",
                     "--eawr-map-camera-config", str(CAMERA), "--eawr-live-session", "m2",
                     "--eawr-live-ai", "on", "--eawr-hud", "tactical"],
                    cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 300
                while b"EAWR live session started" not in log.read_bytes():
                    self.assertIsNone(process.poll(), log.read_text(errors="replace")[-4000:])
                    self.assertLess(time.monotonic(), deadline, "the live session never started")
                    time.sleep(0.5)
                # The battle runs; the AI plans, the fleets move.
                time.sleep(LIVE_QUIT_BATTLE_SECONDS)
                self.assertIsNone(process.poll(), log.read_text(errors="replace")[-4000:])
                self.assertGreater(close_windows_of(process.pid), 0, "no viewer window to close")
                closed_at = time.perf_counter()
                process.wait(timeout=60)
                exit_seconds = time.perf_counter() - closed_at
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            text = log.read_text(errors="replace")
            self.assertEqual(process.returncode, 0, text[-4000:])
            self.assert_map_teardown_before_host_destruction(text)
            self.assertLess(exit_seconds, LIVE_QUIT_EXIT_BOUND_SECONDS, text[-4000:])
            self.assertNotIn("headless replay done", text)
            self.assertIn("EAWR shutdown", text)
            self.assertTrue(report.is_file(), text[-4000:])
            result = strict_json(report.read_text(encoding="utf-8"))
            self.assertEqual(result["status"], "space_environment_closed", result.get("failure"))
            live = result["live_session"]
            self.assertGreater(live["completed_ticks"], 0)
            self.assertIsNone(live["headless_hashes_equal"])

    def test_bad_orders_are_refused(self):
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-bad-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "unknown", ("--eawr-live-order", "5:move:999@0,0,0"))
            self.assertNotEqual(code, 0)
            self.assertIn("is not a unit of the start", result["failure"])
            code, result = self._run(directory, "malformed", ("--eawr-live-order", "5:jump:5"))
            self.assertNotEqual(code, 0)
            self.assertIn("--eawr-live-order expects", result["failure"])
            code, result = self._run(directory, "damage", ("--eawr-live-order", "5:damage:5@1,2,3"))
            self.assertNotEqual(code, 0)
            self.assertIn("<tick>:damage:<unit>@<amount>[,<hardpoint>]", result["failure"])
            code, result = self._run(directory, "hardpoint", ("--eawr-live-order", "5:damage:5@1,0.5"))
            self.assertNotEqual(code, 0)
            self.assertIn("<tick>:damage:<unit>@<amount>[,<hardpoint>]", result["failure"])
            for order in ("5:attack:5", "5:attack:5@0", "5:attack:5@x"):
                code, result = self._run(directory, "attack", ("--eawr-live-order", order))
                self.assertNotEqual(code, 0, order)
                self.assertIn("<tick>:attack:<unit>@<target unit>", result["failure"])
            # #449: a follow group names a tick and unit IDs, in tick order.
            for follow in (("5",), ("5:",), ("5:0",), ("x:5",), ("30:5", "10:6")):
                arguments = tuple(item for value in follow for item in ("--eawr-live-follow-group", value))
                code, result = self._run(directory, "follow", arguments)
                self.assertNotEqual(code, 0, follow)
                self.assertIn("--eawr-live-follow-group expects", result["failure"])


if __name__ == "__main__":
    unittest.main()
