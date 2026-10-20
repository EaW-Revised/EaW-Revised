"""Measure galaxy scaling on one exclusive GPU lease; retain raw frame samples."""
import json
import hashlib
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[3]
CAMPAIGNS = ("Sandbox_Heart_Of_The_Maw_Underworld", "Sandbox_Gateways_Underworld",
             "Sandbox_Equal_Footing_Underworld")
RUNTIME = os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")


def summarize(samples):
    ordered = sorted(samples)
    return {"avg": statistics.mean(ordered), "p95": ordered[int((len(ordered) - 1) * .95)],
            "worst": ordered[-1], "samples": len(ordered)}


@unittest.skipUnless(RUNTIME and os.environ.get("EAWR_EAW_GAME_ROOT"), "requires exclusive GPU and FoC")
class GalaxyFrameCostRuntime(unittest.TestCase):
    def baseline_project(self):
        manifest = json.loads(Path(os.environ["EAWR_JOB_BASE_MANIFEST"]).read_text(encoding="utf-8-sig"))
        self.assertEqual(manifest["commit"], os.environ["EAWR_JOB_BASE_COMMIT"])
        self.assertEqual(manifest["validationStatus"], "passed")
        self.assertFalse(manifest["dirty"])
        project = Path(tempfile.mkdtemp(prefix="eawr-galaxy-base-")) / "project"
        shutil.copytree(ROOT / "apps/viewer/project", project)
        with zipfile.ZipFile(os.environ["EAWR_JOB_BASE_ZIP"]) as archive:
            for entry in archive.infolist():
                self.assertTrue((project / entry.filename).resolve().is_relative_to(project.resolve()))
            archive.extractall(project)
        dll = project / "bin/libeawr_viewer.windows.template_release.x86_64.dll"
        self.assertEqual(hashlib.sha256(dll.read_bytes()).hexdigest(), manifest["sha256"])
        return project

    def measure(self, campaign, headless=False, project=None):
        profile_viewports = os.environ.get("EAWR_JOB_GALAXY_VIEWPORT_TIMINGS") == "1"
        directory = Path(tempfile.mkdtemp(prefix="eawr-galaxy-cost-"))
        report = directory / "report.json"
        samples_path = directory / "samples.json"
        driver = directory / "measure.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var started = 0
var warmup_started = 0
var previous = 0
var samples = []
var viewports = []
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
    if ''' + str(profile_viewports).lower() + ''': collect_viewports(root)
    if ''' + str(headless).lower() + ''':
        # Dummy rendering cannot bind the production skeletons. Keep valid
        # resources while suppressing the render loop for scene-side work.
        RenderingServer.set_render_loop_enabled(false)
        root.hide()
func collect_viewports(node):
    if node is Viewport:
        viewports.append(node)
        RenderingServer.viewport_set_measure_render_time(node.get_viewport_rid(), true)
    for child in node.get_children(): collect_viewports(child)
func _process(_delta):
    frame += 1
    if frame in [10, 11]:
        var event = InputEventKey.new()
        event.keycode = KEY_HOME
        event.pressed = frame == 10
        Input.parse_input_event(event)
    var now = Time.get_ticks_usec()
    if not warmup_started: warmup_started = now
    if not started and frame >= 120 and now - warmup_started >= 3000000:
        started = now
    if started and now > started:
        var render_cpu = {}
        var render_gpu = {}
        for viewport in viewports:
            render_cpu[str(viewport.get_path())] = RenderingServer.viewport_get_measured_render_time_cpu(viewport.get_viewport_rid())
            render_gpu[str(viewport.get_path())] = RenderingServer.viewport_get_measured_render_time_gpu(viewport.get_viewport_rid())
        samples.append({"frame_ms": (now - previous) / 1000.0,
            "process_ms": Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
            "draw_calls": Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME),
            "objects": Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME),
            "nodes": Performance.get_monitor(Performance.OBJECT_NODE_COUNT),
            "render_cpu_ms": render_cpu, "render_gpu_ms": render_gpu})
    previous = now
    if started and now - started >= 15000000:
        var file = FileAccess.open(''' + json.dumps(str(samples_path)) + ''', FileAccess.WRITE)
        file.store_string(JSON.stringify(samples))
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--disable-vsync",
                "--path", str(project or ROOT / "apps/viewer/project"), "--script", str(driver)]
        args += ["--", "--eawr-galactic", campaign, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                 "--eawr-galactic-seat", "1", "--eawr-galactic-paused", "no", "--eawr-audio", "on",
                 "--eawr-perf-overlay", "on", "--eawr-report", str(report)]
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertFalse(data["failure"], data["failure"])
        samples = json.loads(samples_path.read_text(encoding="utf-8"))
        self.assertGreater(len(samples), 30)
        row = {"campaign": campaign, "headless": headless,
               "scene_backend": "render-loop-disabled" if headless else "rendered", "directory": str(directory),
               "viewport_timings": profile_viewports,
               "planets": len(data["planets"]), "routes": len(data["routes"]), "fleets": len(data["fleets"]),
               "frame_ms": summarize([s["frame_ms"] for s in samples]),
               "process_ms": summarize([s["process_ms"] for s in samples]),
               "draw_calls": summarize([s["draw_calls"] for s in samples]),
               "nodes": summarize([s["nodes"] for s in samples]),
               "objects": summarize([s["objects"] for s in samples]),
               "perf_overlay": data["perf_overlay"], "hud_work": data["hud_work"],
               "hud_ms_per_update": data["hud_work"]["update_nanoseconds"] / max(1, data["hud_work"]["updates"]) / 1e6,
               "scene_layers": data["scene_layers"], "camera": data["camera"],
               "presentation_phases": data.get("presentation_phases", []),
               "animation_updates": data["animation_updates"], "frames": data["frames"]}
        row["duration_ms"] = sum(s["frame_ms"] for s in samples)
        for name in ("render_cpu_ms", "render_gpu_ms"):
            row[name] = {key: summarize([s[name][key] for s in samples]) for key in samples[0][name]}
        row["ms_per_planet"] = row["frame_ms"]["avg"] / row["planets"]
        row["ms_per_lane"] = row["frame_ms"]["avg"] / max(1, row["routes"])
        (directory / "measurement.json").write_text(json.dumps(row, indent=2), encoding="utf-8")
        print("GALAXY_MEASURE", json.dumps(row), flush=True)
        return row

    def test_map_scaling(self):
        camera = None
        for campaign in CAMPAIGNS:
            row = self.measure(campaign)
            if camera is not None:
                self.assertEqual(row["camera"], camera, "map scaling must use the same camera")
            camera = row["camera"]

    def test_headless_scene_scaling(self):
        camera = None
        for campaign in CAMPAIGNS:
            row = self.measure(campaign, headless=True)
            if camera is not None:
                self.assertEqual(row["camera"], camera, "headless scaling must use the same camera")
            camera = row["camera"]

    def test_scene_work_bounds(self):
        row = self.measure(CAMPAIGNS[-1])
        phases = {phase["name"]: phase for phase in row["presentation_phases"]}
        samples = phases["sample"]["calls"]
        self.assertGreater(samples, 0)
        self.assertGreater(phases["bind"]["calls"], samples, "fixture must bind multiple surfaces per sampled skeleton")
        self.assertLessEqual(samples, (row["planets"] + row["fleets"] + 2) * row["animation_updates"],
                             "sample work is bounded by objects, independent of their surface counts")
        work = row["hud_work"]
        self.assertGreater(work["max_offer_rows"], 0)
        self.assertGreater(work["offer_rows_visited"], 0)
        self.assertLessEqual(work["offer_rows_visited"], work["max_offer_rows"] * work["fact_rebuilds"],
                             "one publication-wide offer scan must not multiply by planet count")
        self.assertGreater(work["label_content_updates"], 0)
        self.assertLess(work["label_content_updates"], work["label_updates"],
                        "unchanged publications must retain native label content and theme overrides")
        self.assertGreater(work["text_style_updates"], 0)
        self.assertLess(work["text_style_updates"], work["command_updates"],
                        "clock-only publications must retain prepared readout, price and percentage styles")

    @unittest.skipUnless(os.environ.get("EAWR_JOB_BASE_ZIP"), "requires verified BASE artifact")
    def test_paired_scaling(self):
        baseline = self.baseline_project()
        rows = []
        camera = None
        for campaign in CAMPAIGNS:
            for headless in (False, True):
                for version, project in (("base", baseline), ("head", ROOT / "apps/viewer/project")):
                    row = {"version": version, **self.measure(campaign, headless=headless, project=project)}
                    if camera is not None:
                        self.assertEqual(row["camera"], camera)
                    camera = row["camera"]
                    rows.append(row)
        directory = Path(rows[-1]["directory"])
        (directory / "paired-measurements.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")

    @unittest.skipUnless(os.environ.get("EAWR_JOB_BASE_ZIP"), "requires verified BASE artifact")
    def test_paired_pixels(self):
        from PIL import Image, ImageChops
        baseline = self.baseline_project()
        directory = Path(tempfile.mkdtemp(prefix="eawr-galaxy-pixels-"))
        rows = []
        planet_ids = {}
        for index, (campaign, extra, paused) in enumerate([
                (CAMPAIGNS[0], [], True), (CAMPAIGNS[-1], [], True),
                (CAMPAIGNS[-1], ["--eawr-galactic-focus", "Coruscant", "--eawr-galactic-flight-ms", "750"], True),
                (CAMPAIGNS[-1], ["--eawr-galactic-zoom-probe", "Coruscant"], False),
                ("synthetic", ["--eawr-galactic-hud-fixture", "known"], True)]):
            images = []
            states = []
            for version, project in (("base", baseline), ("head", ROOT / "apps/viewer/project")):
                stem = directory / f"{index}-{version}"
                case_extra = extra
                if extra and extra[0] in ("--eawr-galactic-zoom-probe", "--eawr-galactic-focus"):
                    case_extra = [extra[0], str(planet_ids[extra[1]]), *extra[2:]]
                    if not paused:
                        case_extra += ["--eawr-galactic-ticks", "1"]
                args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--fixed-fps", "60", "--path", str(project)]
                capture = stem.with_suffix(".png")
                if not paused:
                    # The native capture waits for an asynchronous sim tick
                    # after the diagnostic zoom. Capture before that gate:
                    # both versions draw the same frame and 0.5 s idle steps.
                    driver = directory / f"zoom-{version}.gd"
                    driver.write_text('''extends SceneTree
var drawn = 0
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
    RenderingServer.frame_post_draw.connect(_drawn)
func _drawn():
    drawn += 1
    if drawn == 200:
        RenderingServer.frame_post_draw.disconnect(_drawn)
        root.get_texture().get_image().save_png(''' + json.dumps(str(capture)) + ''')
        call_deferred("_close")
func _close():
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
''', encoding="utf-8")
                    args += ["--script", str(driver)]
                    capture = directory / f"{index}-{version}-native.png"
                args += ["--", "--eawr-galactic", campaign, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                        "--eawr-galactic-seat", "1", "--eawr-galactic-paused", "yes" if paused else "no", "--eawr-audio", "off",
                        "--eawr-report", str(stem.with_suffix(".json")),
                        "--eawr-capture", str(capture), *case_extra]
                result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
                stem.with_suffix(".log").write_text(result.stdout + result.stderr, encoding="utf-8")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                data = json.loads(stem.with_suffix(".json").read_text(encoding="utf-8"))
                self.assertFalse(data["failure"], data["failure"])
                state = {key: data[key] for key in ("frames", "animation_time", "camera", "active_planet_alternate")}
                states.append(state)
                if not paused:
                    self.assertLess(data["frames"], 210, "capture must precede the asynchronous sim-tick gate")
                    self.assertGreaterEqual(data["frames"], 199)
                    self.assertEqual(data["animation_time"], data["frames"] * .5)
                    self.assertEqual(len(data["zoom_samples"]), 3)
                planet_ids = {p["name"]: p["id"] for p in data["planets"]}
                images.append(Image.open(stem.with_suffix(".png")).convert("RGBA"))
            if not paused:
                self.assertEqual(states[0], states[1], "paired zoom pixels require identical native capture states")
            self.assertEqual(images[0].size, images[1].size)
            difference = ImageChops.difference(*images)
            different = sum(pixel != (0, 0, 0, 0) for pixel in difference.getdata())
            difference.save(directory / f"{index}-difference.png")
            rows.append({"campaign": campaign, "extra": extra, "different_pixels": different, "states": states})
        (directory / "pixel-identity.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
        self.assertEqual([row["different_pixels"] for row in rows], [0] * len(rows), rows)


if __name__ == "__main__":
    unittest.main()
