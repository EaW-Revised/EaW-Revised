"""Battle startup evidence; run on an exclusive private GPU lane."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

if __package__:
    from .battle_load_process import run_battles
else:
    from battle_load_process import run_battles

ROOT = Path(__file__).resolve().parents[3]
MAPS = ("polus", "themaw", "coruscant")


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the viewer GPU lane and game data")
class BattleLoadGpu(unittest.TestCase):
    def test_direct_map_shader_cache(self):
        self.measure_direct_shader_cache("land", [
            "--eawr-map", "data/art/maps/_land_planet_alderaan_02.ted", "--eawr-populate"])

    def test_direct_live_shader_cache(self):
        self.measure_direct_shader_cache("live", [
            "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
            "--eawr-live-session", "m2", "--eawr-live-ticks", "240", "--eawr-live-workers", "4",
            "--eawr-live-order", "15:move:5@-4600,5150,0",
            "--eawr-live-order", "150:face:5@-5600,4700,0", "--eawr-map-camera-config",
            str(ROOT / "apps/viewer/project/config/coruscant-live-session-camera.xml")])

    def measure_direct_shader_cache(self, route, options):
        from PIL import Image

        with tempfile.TemporaryDirectory(prefix=f"battle-direct-{route}-") as temporary:
            output = Path(os.environ.get("EAWR_BATTLE_LOAD_OUT", temporary)).resolve() / f"direct-{route}"
            output.mkdir(parents=True, exist_ok=True)
            controls, phases = [], []
            for cache in ("off", "on"):
                report, capture, hashes, replay = [output / f"{cache}{suffix}" for suffix in
                    (".json", ".png", ".hashes.csv", ".eawr-replay")]
                command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                    "--path", str(ROOT / "apps/viewer/project"), "--", *options,
                    "--eawr-shader-cache", cache, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                    "--eawr-report", str(report), "--eawr-capture", str(capture)]
                if route == "live":
                    command += ["--eawr-live-hashes", str(hashes), "--eawr-live-replay-out", str(replay)]
                completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, timeout=360, check=False)
                (output / f"{cache}.log").write_text(completed.stdout, encoding="utf-8")
                self.assertEqual(completed.returncode, 0, completed.stdout[-6000:])
                self.assertNotIn("ERROR:", completed.stdout)
                self.assertNotIn("leaked", completed.stdout)
                data = json.loads(report.read_text(encoding="utf-8"))
                runs = data["startup"]["runs"]
                self.assertEqual(len(runs), 1)
                phases.append(runs[0]["phases"])
                with Image.open(capture) as image:
                    pixels = (image.size, image.convert("RGBA").tobytes())
                if route == "live":
                    self.assertTrue(data["live_session"]["headless_hashes_equal"])
                    evidence = (data["live_session"]["final_state_sha256"], hashes.read_bytes(), replay.read_bytes())
                else:
                    self.assertGreater(data["populate"]["drawn"], 0)
                    evidence = (data["populate"]["scene_sha256"], data["populate"]["drawn"])
                controls.append((pixels, evidence))
            # On must reuse accepted immutable shaders even within the first
            # process; driver disk-cache warmth cannot reduce these API calls.
            self.assertGreater(phases[1]["shader_compile"]["calls"], 0)
            for phase in ("shader_compile", "shader_reflection"):
                self.assertLess(phases[1][phase]["calls"], phases[0][phase]["calls"])
            self.assertEqual(controls[0], controls[1])

    def test_owner_preview_routes(self):
        with tempfile.TemporaryDirectory(prefix="battle-load-preview-") as temporary:
            output = Path(os.environ.get("EAWR_BATTLE_LOAD_OUT", temporary)).resolve()
            output.mkdir(parents=True, exist_ok=True)
            routes = {
                "m2": ["--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
                    "--eawr-camera-interactive", "--eawr-live-session", "m2", "--eawr-live-reveal", "off",
                    "--eawr-map-camera-config", str(ROOT / "apps/viewer/project/config/coruscant-live-session-camera.xml")],
                "skirmish": ["--eawr-map", "data/art/maps/_mp_space_polus.ted", "--eawr-populate",
                    "--eawr-camera-interactive", "--eawr-live-session", "skirmish", "--eawr-live-reveal", "off",
                    "--eawr-skirmish-slot", "1:Empire:0:human", "--eawr-skirmish-slot", "2:Rebel:1:ai",
                    "--eawr-skirmish-fleet", "1:Tartan_Patrol_Cruiser,Acclamator_Assault_Ship",
                    "--eawr-skirmish-fleet", "2:Y-Wing_Squadron,Corellian_Corvette,Nebulon_B_Frigate,Calamari_Cruiser"],
                "setup": ["--eawr-skirmish-setup"],
            }
            for name, route in routes.items():
                with self.subTest(route=name):
                    command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--max-fps", "60", "--resolution", "1280x720",
                        "--path", str(ROOT / "apps/viewer/project"), "--quit-after", "600" if name == "setup" else "900",
                        "--", "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"], "--eawr-map-effects", "on",
                        "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on",
                        "--eawr-audio", "off", "--eawr-live-ai", "on", "--eawr-hud", "tactical", *route,
                        "--eawr-report", str(output / f"owner-preview-{name}.json")]
                    completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, timeout=400, check=False)
                    (output / f"owner-preview-{name}.log").write_text(completed.stdout, encoding="utf-8")
                    self.assertEqual(completed.returncode, 0, completed.stdout[-6000:])
                    self.assertIn("EAWR skirmish setup ready" if name == "setup" else "EAWR live session started",
                        completed.stdout)
                    self.assertNotIn("ERROR:", completed.stdout)
                    self.assertNotIn("leaked", completed.stdout)

    def test_owner_two_start_smoke(self):
        # Keep the review smoke's ordinary user cache and omitted perf trace:
        # benchmark-only command lines must not be the sole startup regression.
        with tempfile.TemporaryDirectory(prefix="battle-load-owner-") as temporary:
            output = Path(os.environ.get("EAWR_BATTLE_LOAD_OUT", temporary)).resolve()
            controls = []
            for cache in ("off", "on"):
                directory = output / f"owner-two-start-{cache}"
                directory.mkdir(parents=True, exist_ok=True)
                report, capture, hashes, replay = [directory / name for name in
                    ("report.json", "capture.png", "hashes.csv", "session.eawr-replay")]
                command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--max-fps", "60",
                    "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), "--",
                    "--eawr-skirmish-setup", "--eawr-setup-test", "--eawr-load-bench-map",
                    "data/art/maps/_mp_space_polus.ted", "--eawr-load-bench-starts", "2",
                    "--eawr-shader-cache", cache, "--eawr-content-cache", "on",
                    "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                    "--eawr-report", str(report), "--eawr-capture", str(capture),
                    "--eawr-live-ticks", "8", "--eawr-live-hashes", str(hashes),
                    "--eawr-live-replay-out", str(replay), "--eawr-map-timed-frames", "8",
                    "--eawr-live-ai", "on", "--eawr-live-workers", "4", "--eawr-audio", "on",
                    "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"]
                completed, memory, battles = run_battles(
                    command, ROOT, report, capture, hashes, replay, timeout=240)
                self.assertEqual(completed.returncode, 0, completed.stdout[-6000:])
                self.assertEqual(len(battles), 2)
                self.assertEqual(len(memory), 2)
                for returned in memory:
                    self.assertGreater(returned.get("settled_working_set_bytes", 0), 0)
                runs = json.loads(report.read_text(encoding="utf-8"))["startup"]["runs"]
                self.assertEqual(len(runs), 2)
                for run in runs:
                    self.assertTrue(run["first_frame_drawn"])
                compiles = [run["phases"]["shader_compile"]["calls"] for run in runs]
                self.assertGreater(compiles[0], 0)
                if cache == "on":
                    self.assertLessEqual(compiles[0], 64)
                    self.assertEqual(compiles[1], 0)
                else:
                    self.assertEqual(compiles[0], compiles[1])
                evidence = []
                for battle in battles:
                    data = json.loads(Path(battle["report"]).read_text(encoding="utf-8"))
                    self.assertTrue(data["live_session"]["headless_hashes_equal"])
                    evidence.append((Path(battle["hashes"]).read_bytes(), Path(battle["replay"]).read_bytes()))
                self.assertEqual(evidence[0], evidence[1])
                controls.append(evidence)
                self.assertNotIn("ERROR:", completed.stdout)
                self.assertNotIn("leaked", completed.stdout)
            self.assertEqual(controls[0], controls[1])

    def test_scene_startup_profile(self):
        self.measure(("setup",), iterations=1)

    def test_shader_cache_before(self):
        self.measure(("setup",), cache_shaders=False)

    def test_shader_cache_after(self):
        self.measure(("setup",), cache_shaders=True)

    def test_startup_runs(self):
        self.measure(("direct", "setup"))

    def test_direct_baseline(self):
        self.measure(("direct",))

    def test_startup_before(self):
        self.measure(("direct", "setup"), cache_content=False)

    def measure(self, modes, cache_content=True, cache_shaders=None, iterations=3):
        with tempfile.TemporaryDirectory(prefix="battle-load-") as temporary:
            output = Path(os.environ.get("EAWR_BATTLE_LOAD_OUT", temporary)).resolve()
            output.mkdir(parents=True, exist_ok=True)
            samples = []
            matrix = uuid.uuid4().hex[:8]
            for map_name in MAPS:
                map_path = f"data/art/maps/_mp_space_{map_name}.ted"
                for mode in modes:
                    for iteration in range(iterations):
                        # The rig runner gives this pair a private shared cache root.
                        group = f"battle-load-{matrix}-{map_name}-{mode}-{iteration}"
                        for cache in ("cold", "warm"):
                            name = f"{map_name}-{mode}-{iteration}-{cache}"
                            report = output / f"{name}.json"
                            capture = output / f"{name}.png"
                            trace = output / f"{name}.csv"
                            hashes = output / f"{name}.hashes.csv"
                            replay = output / f"{name}.eawr-replay"
                            options = (["--eawr-skirmish-setup", "--eawr-setup-test",
                                        "--eawr-load-bench-map", map_path, "--eawr-load-bench-starts",
                                        "3" if cache_shaders is not None else "2"]
                                       if mode == "setup" else ["--eawr-map", map_path,
                                                               "--eawr-live-session", "skirmish", "--eawr-populate"])
                            if cache_shaders is not None:
                                options += ["--eawr-shader-cache", "on" if cache_shaders else "off"]
                            command = [
                                os.environ["EAWR_GODOT_EXECUTABLE"],
                                "--log-file", str(report.with_suffix(".engine.log")),
                                "--resolution", "1280x720",
                                "--path", str(ROOT / "apps/viewer/project"), "--", *options,
                                "--eawr-load-bench-cache", group,
                                "--eawr-content-cache", "on" if cache_content else "off",
                                "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                                "--eawr-report", str(report), "--eawr-capture", str(capture),
                                "--eawr-perf-trace", str(trace), "--eawr-live-ticks", "8",
                                "--eawr-live-hashes", str(hashes), "--eawr-live-replay-out", str(replay),
                                "--eawr-map-timed-frames", "8", "--eawr-live-ai", "on",
                                "--eawr-live-workers", "4",
                                "--eawr-audio", "on", "--eawr-lighting", "sh",
                                "--eawr-environment", "map", "--eawr-shadows", "on"]
                            memory, battles = [], []
                            if cache_shaders is not None:
                                # Leave at least one second on the restored setup
                                # (60 frames) for the 300 ms settled memory sample.
                                command[1:1] = ["--max-fps", "60"]
                                completed, memory, battles = run_battles(
                                    command, ROOT, report, capture, hashes, replay)
                                self.assertEqual(len(memory), 3)
                                for returned in memory:
                                    self.assertGreater(returned.get("settled_working_set_bytes", 0), 0)
                            else:
                                completed = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                                                           stderr=subprocess.STDOUT, text=True, timeout=360)
                            (output / f"{name}.log").write_text(completed.stdout, encoding="utf-8")
                            self.assertEqual(completed.returncode, 0, completed.stdout[-6000:])
                            data = json.loads(report.read_text(encoding="utf-8"))
                            self.assertEqual(data["live_session"]["start_map"], map_path)
                            self.assertTrue(data["live_session"]["headless_hashes_equal"])
                            runs = data["startup"]["runs"]
                            self.assertEqual(len(runs), (3 if cache_shaders is not None else 2)
                                             if mode == "setup" else 1)
                            for start, run in enumerate(runs):
                                self.assertTrue(run["first_frame_drawn"])
                                self.assertGreater(run["total_ms"], 0)
                                # A deterministic work budget: Start must reuse
                                # the catalog already parsed for its setup screen.
                                self.assertEqual(run["phases"]["xml_catalog"]["calls"],
                                                 0 if mode == "setup" and cache_content else 1)
                                if cache_shaders is True:
                                    # Count actual compiler invocations, never elapsed
                                    # time: one admission per unique shader on Start 1,
                                    # none on the later Starts from the same setup.
                                    compiled = run["phases"]["shader_compile"]["calls"]
                                    self.assertLessEqual(compiled, 64 if start == 0 else 0)
                                    if start == 0:
                                        self.assertGreater(compiled, 0)
                                    self.assertLessEqual(run["phases"]["shader_reflection"]["calls"],
                                                         128 if start == 0 else 0)
                                elif cache_shaders is False:
                                    self.assertEqual(run["phases"]["shader_compile"]["calls"],
                                                     run["phases"]["material_shader_creation"]["calls"])
                            self.assertTrue(Path(str(trace) + ".startup.csv").is_file())
                            # The rig retains text evidence, JSON and images; preserve
                            # exact replay/hash bytes without widening its export list.
                            hashes_evidence = hashes.with_suffix(".txt")
                            replay_evidence = replay.with_suffix(".replay.txt")
                            hashes_evidence.write_bytes(hashes.read_bytes())
                            replay_evidence.write_bytes(replay.read_bytes())
                            startup_csv = Path(str(trace) + ".startup.csv")
                            startup_csv.with_suffix(".txt").write_bytes(startup_csv.read_bytes())
                            samples.append({"name": name, "map": map_name, "mode": mode,
                                            "cache": cache, "iteration": iteration, "startup": runs,
                                            "backend": data["backend"], "capture": str(capture),
                                            "final_state_sha256": data["live_session"]["final_state_sha256"],
                                            "hashes": str(hashes_evidence), "replay": str(replay_evidence),
                                            "memory_after_return": memory, "battles": battles,
                                            "command": command,
                                            "headless_hashes_equal": True})
                            (output / "samples.json").write_text(json.dumps(samples, indent=2), encoding="utf-8")


if __name__ == "__main__":
    unittest.main()
