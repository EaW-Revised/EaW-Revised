"""Exclusive-host Remake galaxy idle measurements, with raw viewport samples.

Select one arm per rig lease with EAWR_JOB_GC_IDLE_ARM (off, medium, high,
medium_ss2) and EAWR_JOB_GC_IDLE_CAP (0 or a positive wall-clock frame cap).
The observer waits sixty seconds after the galactic scene becomes playable.
"""
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
ARMS = {"off": "msaa=0,smaa=0,ss=1", "medium": "msaa=4,smaa=1,ss=1",
        "high": "msaa=8,smaa=1,ss=1", "medium_ss2": "msaa=4,smaa=1,ss=2"}

DRIVER = '''extends SceneTree
var playable_at = 0
var started = 0
var previous = 0
var frame = 0
var samples = []
var inventory = []
var viewports = []
var previous_requests = {}
var timing_frame = 0
var ended = false
var caps = __CAPS__
var cap_index = 0
var settle_until = 0
var root_probe = __ROOT_AB__
var root_disabled = false
func _initialize():
    call_deferred("_start")
func _start():
    root.content_scale_factor = 1.0
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_size = Vector2i(__WIDTH__, __HEIGHT__)
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    # Private desktops cannot supply a reliable OS cursor position. An empty
    # modal-group CanvasItem blocks the pointer feed without drawing pixels.
    var input_blocker = Node2D.new()
    input_blocker.name = "GcIdleInputBlocker"
    input_blocker.add_to_group("eawr_ui_modal")
    root.add_child(input_blocker)
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
    RenderingServer.frame_post_draw.connect(_drawn)
func collect(node):
    if node is Viewport:
        viewports.append(node)
        RenderingServer.viewport_set_measure_render_time(node.get_viewport_rid(), true)
        var entry = {"path": str(node.get_path()), "size": [node.size.x, node.size.y],
            "visible_size": [node.get_visible_rect().size.x, node.get_visible_rect().size.y],
            "scaling_3d_scale": node.scaling_3d_scale, "msaa_3d": node.msaa_3d,
            "screen_space_aa": node.screen_space_aa, "disable_3d": node.disable_3d}
        if node is SubViewport:
            entry["update_mode"] = node.render_target_update_mode
        inventory.append(entry)
        if node.has_meta("eawr_gc_refresh_requests"):
            previous_requests[str(node.get_path())] = int(node.get_meta("eawr_gc_refresh_requests"))
    for child in node.get_children(): collect(child)
func _drawn():
    if ended: return
    var now = Time.get_ticks_usec()
    var scenes = root.find_child("GalacticScenes", true, false)
    var loading = root.find_child("EawrLoadingScreen", true, false)
    if scenes == null or (loading != null and loading.visible): return
    if playable_at == 0:
        playable_at = now
        print("GC_IDLE_PLAYABLE ", now)
    if started == 0 and now - playable_at >= __WARMUP__ * 1000000:
        collect(root)
        started = now
        previous = now
        print("GC_IDLE_SAMPLING ", JSON.stringify(inventory))
        return
    if started == 0: return
    if now < settle_until:
        for viewport in viewports:
            if viewport.has_meta("eawr_gc_refresh_requests"):
                previous_requests[str(viewport.get_path())] = int(viewport.get_meta("eawr_gc_refresh_requests"))
        timing_frame = frame
        previous = now
        return
    var row = {"elapsed_ms": (now - started) / 1000.0, "frame_ms": (now - previous) / 1000.0,
        "root_no3d": root_disabled,
        "process_ms": Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
        "draw_calls": Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME),
        "primitives": Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME),
        "max_fps": Engine.max_fps, "vsync": DisplayServer.window_get_vsync_mode(), "refresh_requests": {}}
    for viewport in viewports:
        if viewport.has_meta("eawr_gc_refresh_requests"):
            row["refresh_requests"][str(viewport.get_path())] = int(viewport.get_meta("eawr_gc_refresh_requests"))
    frame += 1
    # Timestamp queries synchronize with the render thread; bound their frequency.
    if frame % 6 == 0:
        var timings = {}
        for viewport in viewports:
            var path = str(viewport.get_path())
            var interval = frame - timing_frame
            var refreshes = interval
            if viewport.has_meta("eawr_gc_refresh_requests"):
                var requests = int(viewport.get_meta("eawr_gc_refresh_requests"))
                refreshes = requests - int(previous_requests.get(path, requests))
                previous_requests[path] = requests
            var gpu_ms = RenderingServer.viewport_get_measured_render_time_gpu(viewport.get_viewport_rid())
            # Inactive viewports retain old timestamps. Preserve that raw value
            # and estimate amortized work from refresh requests in this interval.
            timings[path] = {
                "cpu_ms": RenderingServer.viewport_get_measured_render_time_cpu(viewport.get_viewport_rid()),
                "gpu_ms": gpu_ms, "weighted_gpu_ms": gpu_ms * min(refreshes, interval) / float(interval),
                "refresh_requests": refreshes, "interval_frames": interval,
                "draw_calls": viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_DRAW_CALLS_IN_FRAME),
                "objects": viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_OBJECTS_IN_FRAME),
                "primitives": viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_PRIMITIVES_IN_FRAME)}
        row["viewports"] = timings
        timing_frame = frame
    samples.append(row)
    previous = now
    if now - started >= __SECONDS__ * 1000000:
        if cap_index + 1 == caps.size() and root_probe and not root_disabled:
            root.get_texture().get_image().save_png(__BEFORE__)
            root.disable_3d = true
            root_disabled = true
            caps.append(0)
        if cap_index + 1 < caps.size():
            cap_index += 1
            Engine.max_fps = caps[cap_index]
            settle_until = now + 2000000
            started = settle_until
            previous = now
            return
        ended = true
        var file = FileAccess.open(__SAMPLES__, FileAccess.WRITE)
        file.store_string(JSON.stringify({"playable_at_usec": playable_at, "sample_start_usec": started,
            "adapter": RenderingServer.get_video_adapter_name(), "inventory": inventory, "samples": samples}))
        file.close()
        root.get_texture().get_image().save_png(__CAPTURE__)
        call_deferred("_close")
func _close():
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
'''


def summary(values):
    values = sorted(values)
    return {"mean": statistics.mean(values), "p50": statistics.median(values),
            "p95": values[int((len(values) - 1) * .95)], "count": len(values)}


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and
                     os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires exclusive GPU and Remake")
class GalacticIdleGpu(unittest.TestCase):
    @unittest.skipIf(os.environ.get("EAWR_JOB_BASE_ZIP"), "paired run measures both artifacts")
    def test_idle_arm(self):
        self.measure()

    def baseline_project(self):
        manifest = json.loads(Path(os.environ["EAWR_JOB_BASE_MANIFEST"]).read_text(encoding="utf-8-sig"))
        self.assertEqual(manifest["commit"], os.environ["EAWR_JOB_BASE_COMMIT"])
        self.assertEqual(manifest["validationStatus"], "passed")
        self.assertFalse(manifest["dirty"])
        project = Path(tempfile.mkdtemp(prefix="eawr-gc-idle-base-")) / "project"
        shutil.copytree(ROOT / "apps/viewer/project", project)
        with zipfile.ZipFile(os.environ["EAWR_JOB_BASE_ZIP"]) as archive:
            for entry in archive.infolist():
                self.assertTrue((project / entry.filename).resolve().is_relative_to(project.resolve()))
            archive.extractall(project)
        dll = project / "bin/libeawr_viewer.windows.template_release.x86_64.dll"
        self.assertEqual(hashlib.sha256(dll.read_bytes()).hexdigest(), manifest["sha256"])
        return project

    @unittest.skipUnless(os.environ.get("EAWR_JOB_BASE_ZIP"), "requires verified BASE artifact")
    @unittest.skipIf(os.environ.get("EAWR_JOB_GC_IDLE_LIVE") == "1", "live timings use their separate case")
    def test_paired_artifacts(self):
        from PIL import Image, ImageChops
        baseline = self.baseline_project()
        arms = os.environ.get("EAWR_JOB_GC_IDLE_ARMS", os.environ.get("EAWR_JOB_GC_IDLE_ARM", "medium")).split(",")
        paired = []
        for arm in arms:
            base_rows = self.measure(project=baseline, arm=arm, version="base")
            head_rows = self.measure(arm=arm, version="head")
            self.assert_pair_identity(base_rows, head_rows)
            self.assertTrue(base_rows[0]["paused"] and head_rows[0]["paused"],
                            "pixel proof must use a held animation clock")
            directory = Path(head_rows[0]["directory"])
            base_image = Image.open(Path(base_rows[0]["directory"]) / "capture.png").convert("RGB")
            head_image = Image.open(directory / "capture.png").convert("RGB")
            self.assertEqual(base_image.size, head_image.size)
            difference = ImageChops.difference(base_image, head_image)
            different = sum(pixel != (0, 0, 0) for pixel in difference.getdata())
            difference.save(directory / "base-head-difference.png")
            comparison = {"arm": arm, "different_pixels": different,
                          "base": base_rows, "head": head_rows}
            paired.append(comparison)
            (directory / "paired-measurement.json").write_text(json.dumps(comparison, indent=2), encoding="utf-8")
            self.assertEqual(different, 0, "compiled BASE/HEAD must preserve the paused image")
        (directory / "paired-series.json").write_text(json.dumps(paired, indent=2), encoding="utf-8")

    def assert_pair_identity(self, base, head):
        self.assertEqual(base[0]["camera"], head[0]["camera"])
        self.assertEqual(base[0]["adapter"], head[0]["adapter"])
        for key in ("msaa_samples", "screen_space_aa", "anisotropic_filtering", "supersampling"):
            self.assertEqual(base[0]["render_profile"][key], head[0]["render_profile"][key], key)
        # Scheduling changes are expected; every size, filter and AA property must match.
        inventories = [[{k: v for k, v in entry.items() if k != "update_mode"}
                        for entry in rows[0]["inventory"]] for rows in (base, head)]
        self.assertEqual(*inventories, "paired artifacts must use the same viewport sizes and AA")

    @unittest.skipUnless(os.environ.get("EAWR_JOB_BASE_ZIP") and os.environ.get("EAWR_JOB_GC_IDLE_LIVE") == "1",
                         "requires a separate live BASE/HEAD lease")
    def test_paired_live(self):
        baseline = self.baseline_project()
        paired = []
        for arm in os.environ.get("EAWR_JOB_GC_IDLE_ARMS", "medium").split(","):
            base = self.measure(project=baseline, arm=arm, version="base-live", paused_override=False)
            head = self.measure(arm=arm, version="head-live", paused_override=False)
            self.assert_pair_identity(base, head)
            for rows in (base, head):
                live = rows[0]["live_session"]
                self.assertEqual(live["pause_depth"], 0)
                self.assertTrue(live["native_ai_installed_at_start"])
                self.assertGreater(live["galactic_frame"], 0)
                self.assertGreater(rows[0]["animation_updates"], 1)
            paired.append({"arm": arm, "base": base, "head": head,
                           "pixel_comparison": "live clocks differ; paused case supplies exact pixel gate"})
        directory = Path(head[0]["directory"])
        (directory / "paired-live-series.json").write_text(json.dumps(paired, indent=2), encoding="utf-8")

    @unittest.skipUnless(os.environ.get("EAWR_JOB_BASE_ZIP") and os.environ.get("EAWR_JOB_GC_IDLE_TRANSITIONS") == "1",
                         "requires a separate cache transition lease")
    def test_paired_cache_transitions(self):
        from PIL import Image, ImageChops
        baseline = self.baseline_project()
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-cache-transitions-"))
        phases = ("cold", "profile", "resized_first", "resized", "hidden", "resumed", "camera")
        states = []
        for version, project in (("base", baseline), ("head", ROOT / "apps/viewer/project")):
            output = directory / version
            output.mkdir()
            driver = output / "transitions.gd"
            driver.write_text('''extends SceneTree
var frame = 0
var output = __OUTPUT__
var trace = []
var blocker
var container
func _initialize():
    call_deferred("_start")
func _start():
    root.content_scale_size = Vector2i(1280, 720)
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    # The rig gives every launch a private user directory. Keep accepting the
    # native video page from also switching this fixture into fullscreen.
    var settings = FileAccess.open("user://eawr-settings.cfg", FileAccess.WRITE)
    settings.store_string("eawr-settings 1\\nvideo.windowed=1\\nvideo.window_width=1280\\nvideo.window_height=720\\nvideo.vsync=0\\nvideo.render_profile=medium\\n")
    settings.close()
    blocker = Node2D.new()
    blocker.add_to_group("eawr_ui_modal")
    root.add_child(blocker)
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func key(code):
    # The empty modal blocks the desktop cursor and world keys. Admit only
    # these explicit fixture gestures through the ordinary viewport routing.
    blocker.remove_from_group("eawr_ui_modal")
    for pressed in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = pressed
        root.push_input(event)
    blocker.add_to_group("eawr_ui_modal")
func control(name):
    for node in root.find_children(name, "", true, false):
        if node is Control and node.is_visible_in_tree(): return node
    assert(false, "visible native control absent: " + name)
func press(name):
    var button = control(name)
    assert(button is BaseButton and not button.disabled)
    button.emit_signal("pressed")
func capture(name):
    # In separate-thread mode, resolve the viewport texture only after the
    # queued draw completes; otherwise BASE/HEAD can read different old buffers.
    RenderingServer.force_sync()
    root.get_texture().get_image().save_png(output + "/" + name + ".png")
    var row = {"phase": name, "frame": frame, "requests": {},
        "size": [root.size.x, root.size.y], "msaa_3d": root.msaa_3d, "screen_space_aa": root.screen_space_aa}
    var scenes = root.find_child("GalacticScenes", true, false)
    for viewport in scenes.get_parent().get_children():
        if viewport is SubViewport and viewport.has_meta("eawr_gc_refresh_requests"):
            row.requests[str(viewport.name)] = int(viewport.get_meta("eawr_gc_refresh_requests"))
    trace.append(row)
func _process(_delta):
    # In separate-thread mode post-draw signals are deferred and may be batched.
    # Actions precede the nodes' process step; next process boundary + sync
    # reads that step's completed draw, including the first frame after a change.
    var scenes = root.find_child("GalacticScenes", true, false)
    var loading = root.find_child("EawrLoadingScreen", true, false)
    if scenes == null or (loading != null and loading.visible): return false
    frame += 1
    if frame == 60:
        container = scenes.get_parent().get_parent().get_parent()
        assert(container is SubViewportContainer)
        capture("cold")
        key(KEY_ESCAPE)
    if frame == 70: press("IDC_INGAME_OPTIONS")
    if frame == 80: press("IDC_IN_GAME_OPTIONS_VIDEO")
    if frame == 90:
        var slider = control("IDC_ANTI_ALIAS_SLIDER")
        assert(slider is Range)
        slider.value = 0
        press("IDC_OK")
    if frame == 100: press("IDC_OK_IN_GAME")
    if frame == 110: press("IDC_GAME_OPTIONS_RESUME")
    if frame == 111: capture("profile")
    if frame == 120: root.content_scale_size = Vector2i(1600, 900)
    if frame == 121: capture("resized_first")
    if frame == 124: capture("resized")
    if frame == 130: container.hide()
    if frame == 131: capture("hidden")
    if frame == 140: container.show()
    if frame == 141: capture("resumed")
    if frame == 150: key(KEY_HOME)
    if frame == 151:
        capture("camera")
        var file = FileAccess.open(output + "/trace.json", FileAccess.WRITE)
        file.store_string(JSON.stringify(trace))
        file.close()
        call_deferred("_close")
    return false
func _close():
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
'''.replace("__OUTPUT__", json.dumps(output.as_posix())), encoding="utf-8")
            args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--fixed-fps", "60",
                    "--quit-after", "600", "--render-thread", "separate", "--log-file", str(output / "engine.log"),
                    "--disable-vsync", "--path", str(project), "--script", str(driver), "--",
                    "--eawr-galactic", "GC_Standard_Web_Way_Imperial", "--eawr-galactic-paused", "yes",
                    "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                    "--eawr-mod-root", os.environ["EAWR_REMAKE_MOD_ROOT"], "--eawr-galactic-seat", "1",
                    "--eawr-galactic-dismiss-story", "yes", "--eawr-audio", "off",
                    "--eawr-render-profile", "medium", "--eawr-diagnostic-vsync", "0",
                    "--eawr-report", str(output / "report.json")]
            result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=480, check=False)
            (output / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-12000:])
            self.assertNotIn("ERROR:", result.stdout + result.stderr)
            report = json.loads((output / "report.json").read_text(encoding="utf-8"))
            self.assertFalse(report["failure"], report["failure"])
            self.assertEqual(report["render_profile"]["msaa_samples"], 0, "native options must apply AA Off")
            self.assertEqual(report["render_profile"]["screen_space_aa"], "disabled")
            states.append({key: report[key] for key in ("camera", "render_profile", "animation_time")})
        self.assertEqual(states[0], states[1])
        differences = {}
        for phase in phases:
            images = [Image.open(directory / version / (phase + ".png")).convert("RGB") for version in ("base", "head")]
            self.assertEqual(images[0].size, images[1].size)
            self.assertEqual(images[0].size, (1280, 720) if phase in ("cold", "profile") else (1600, 900))
            delta = ImageChops.difference(*images)
            delta.save(directory / (phase + "-difference.png"))
            differences[phase] = sum(pixel != (0, 0, 0) for pixel in delta.getdata())
        (directory / "pixel-identity.json").write_text(json.dumps(differences, indent=2), encoding="utf-8")
        self.assertEqual(differences, dict.fromkeys(phases, 0))
        trace = json.loads((directory / "head/trace.json").read_text(encoding="utf-8"))
        by_phase = {row["phase"]: row["requests"] for row in trace}
        records = {row["phase"]: row for row in trace}
        self.assertNotEqual(records["cold"]["screen_space_aa"], records["profile"]["screen_space_aa"])
        self.assertEqual(records["profile"]["screen_space_aa"], 0)
        camera_delta = ImageChops.difference(Image.open(directory / "base/camera.png").convert("RGB"),
                                           Image.open(directory / "base/resumed.png").convert("RGB"))
        self.assertIsNotNone(camera_delta.getbbox(), "native Home must actually change the camera image")
        self.assertEqual(set(by_phase["cold"]), {"background", "midground", "midforeground"})
        self.assertEqual(by_phase["hidden"], by_phase["resized"], "covered layers must retain their last result")
        for phase, previous in (("profile", "cold"), ("resized", "profile"), ("resumed", "hidden"), ("camera", "resumed")):
            for layer in by_phase[phase]:
                self.assertGreater(by_phase[phase][layer], by_phase[previous][layer], (phase, layer))

    @unittest.skipUnless(os.environ.get("EAWR_JOB_BASE_ZIP") and os.environ.get("EAWR_JOB_GC_IDLE_RADAR_BASELINE") == "1",
                         "requires verified BASE/HEAD radar comparison")
    def test_paired_advanced_radar_reference(self):
        from unittest.mock import patch
        from tests.presentation.renderer import test_viewer_galactic_radar_runtime as radar
        baseline = self.baseline_project()
        base_root = baseline.parent / "reference-root"
        shutil.copytree(baseline, base_root / "apps/viewer/project")
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-advanced-paired-"))
        rows = []
        for version, root in (("base", base_root), ("head", ROOT)):
            result = unittest.TestResult()
            case = radar.GalacticRadarRuntime("test_native_advanced_radar_retains_overlay_then_zoom_closes_it")
            with patch.object(radar.support, "ROOT", root):
                case.run(result)
            rows.append({"version": version, "tests_run": result.testsRun,
                         "failures": [trace for _, trace in result.failures],
                         "errors": [trace for _, trace in result.errors],
                         "skipped": [reason for _, reason in result.skipped],
                         "passed": result.wasSuccessful() and not result.skipped})
        (directory / "paired-advanced-radar.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
        self.assertTrue(all(row["passed"] for row in rows), rows)

    @unittest.skipUnless(os.environ.get("EAWR_JOB_BASE_ZIP") and os.environ.get("EAWR_JOB_GC_CACHE_ADVANCED") == "1",
                         "requires paired advanced-map transition qualification")
    def test_paired_advanced_map_pixels(self):
        from PIL import Image, ImageChops
        baseline = self.baseline_project()
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-cache-advanced-"))
        phases = ("focused", "opened_first", "positioned", "closed_first", "closed_settled")
        for version, project in (("base", baseline), ("head", ROOT / "apps/viewer/project")):
            output = directory / version
            output.mkdir()
            driver = output / "advanced.gd"
            driver.write_text('''extends SceneTree
var frame = 0
var trace = []
var output = __OUTPUT__
var blocker
var ready_msec = 0
func _initialize():
    call_deferred("_start")
func _start():
    root.content_scale_size = Vector2i(1280, 720)
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    blocker = Node2D.new()
    blocker.add_to_group("eawr_ui_modal")
    root.add_child(blocker)
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func control(name):
    for node in root.find_children(name, "", true, false):
        if node is Control and node.is_visible_in_tree(): return node
    assert(false, "visible native radar control absent: " + name)
func click(name, button, uv = Vector2(0.5, 0.5)):
    var node = control(name)
    var rect = node.get_global_rect()
    var point = rect.position + rect.size * uv
    blocker.remove_from_group("eawr_ui_modal")
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    root.push_input(motion)
    for pressed in [true, false]:
        var event = InputEventMouseButton.new()
        event.position = point
        event.global_position = point
        event.button_index = button
        event.pressed = pressed
        root.push_input(event)
    blocker.add_to_group("eawr_ui_modal")
func capture(label):
    RenderingServer.force_sync()
    root.get_texture().get_image().save_png(output + "/" + label + ".png")
    var advanced = root.find_child("radar_galactic_adv_map", true, false)
    var row = {"phase": label, "advanced": advanced != null and advanced.is_visible_in_tree(), "requests": {},
        "max_fps": Engine.max_fps, "wall_ms": Time.get_ticks_msec() - ready_msec}
    var scenes = root.find_child("GalacticScenes", true, false)
    for viewport in scenes.get_parent().get_children():
        if viewport is SubViewport and viewport.has_meta("eawr_gc_refresh_requests"):
            row.requests[str(viewport.name)] = int(viewport.get_meta("eawr_gc_refresh_requests"))
    trace.append(row)
func _process(_delta):
    # Startup applies the saved video settings after _start. Keep this pixel
    # driver's fixed steps paced while the interactive camera uses wall time.
    Engine.max_fps = 60
    var loading = root.find_child("EawrLoadingScreen", true, false)
    if root.find_child("GalacticScenes", true, false) == null or (loading != null and loading.visible): return false
    if ready_msec == 0: ready_msec = Time.get_ticks_msec()
    frame += 1
    if frame == 120:
        capture("focused")
        click("radar_galactic", MOUSE_BUTTON_WHEEL_UP)
    if frame == 121: capture("opened_first")
    if frame == 140: click("radar_galactic_adv_map", MOUSE_BUTTON_LEFT, Vector2(0.25, 0.75))
    if frame == 141: capture("positioned")
    if frame == 160: click("b_zoom_adv_map", MOUSE_BUTTON_LEFT)
    if frame == 161: capture("closed_first")
    if frame == 181:
        capture("closed_settled")
        var file = FileAccess.open(output + "/trace.json", FileAccess.WRITE)
        file.store_string(JSON.stringify(trace))
        file.close()
        call_deferred("finish")
    return false
func finish():
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
'''.replace("__OUTPUT__", json.dumps(output.as_posix())), encoding="utf-8")
            report = self.run_cache_driver(driver, output, project, ["--eawr-galactic", "synthetic",
                "--eawr-galactic-hud-fixture", "ground-contents", "--eawr-galactic-focus", "101",
                "--eawr-galactic-flight-ms", "750"], fixed_fps=False)
            self.assertEqual(report["radar_hud"]["positions"], 1)
            self.assertEqual(report["radar_hud"]["zoom_requests"], 2)
            self.assertFalse(report["radar_hud"]["advanced"])
            self.assertEqual(report["map_input"]["selected"], 101)
            trace = json.loads((output / "trace.json").read_text(encoding="utf-8"))
            self.assertEqual([row["phase"] for row in trace], list(phases))
            self.assertEqual([row["advanced"] for row in trace], [False, True, True, False, False])
            self.assertTrue(all(row["max_fps"] == 60 for row in trace))
            self.assertGreaterEqual(trace[0]["wall_ms"], 1500, "initial native flight must finish before input")
        differences = {}
        for phase in phases:
            images = [Image.open(directory / version / f"{phase}.png").convert("RGB") for version in ("base", "head")]
            self.assertEqual(images[0].size, images[1].size)
            delta = ImageChops.difference(*images)
            delta.save(directory / f"{phase}-difference.png")
            differences[phase] = sum(pixel != (0, 0, 0) for pixel in delta.getdata())
        (directory / "pixel-identity.json").write_text(json.dumps(differences, indent=2), encoding="utf-8")
        self.assertEqual(differences, dict.fromkeys(phases, 0))
        trace = json.loads((directory / "head/trace.json").read_text(encoding="utf-8"))
        self.assertEqual(trace[1]["requests"], trace[2]["requests"], "galaxy must stay cached while advanced map owns display")
        for layer, requests in trace[0]["requests"].items():
            self.assertGreater(trace[3]["requests"][layer], requests, "first close must refresh the galaxy")

    @unittest.skipUnless(os.environ.get("EAWR_JOB_GC_CACHE_LOAD") == "1", "requires native corruption load qualification")
    def test_cache_corruption_in_game_load(self):
        from PIL import Image, ImageChops
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-cache-load-"))
        driver = directory / "load.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var phase = 0
var loaded_frame = 0
var output = __OUTPUT__
var trace = []
var blocker
var owner
func _initialize():
    call_deferred("_start")
func _start():
    root.content_scale_size = Vector2i(1280, 720)
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    blocker = Node2D.new()
    blocker.add_to_group("eawr_ui_modal")
    root.add_child(blocker)
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func key(code):
    blocker.remove_from_group("eawr_ui_modal")
    for pressed in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = pressed
        root.push_input(event)
    blocker.add_to_group("eawr_ui_modal")
func capture(label):
    RenderingServer.force_sync()
    var row = {"phase": label, "frame": frame, "requests": {},
        "particles": owner.get_meta("eawr_cache_fixture_particles", 0),
        "lines": owner.get_meta("eawr_cache_fixture_lines", 0),
        "native_corrupted": owner.get_meta("eawr_cache_fixture_native_corrupted", -1),
        "native_pause_depth": owner.get_meta("eawr_cache_fixture_native_pause_depth", 0),
        "loaded": owner.get_meta("eawr_cache_fixture_loaded", false)}
    var scenes = root.find_child("GalacticScenes", true, false)
    for viewport in scenes.get_parent().get_children():
        if viewport is SubViewport and str(viewport.name) in ["background", "midground", "midforeground"]:
            viewport.get_texture().get_image().save_png(output + "/" + label + "-" + str(viewport.name) + ".png")
            row.requests[str(viewport.name)] = int(viewport.get_meta("eawr_gc_refresh_requests", 0))
    root.get_texture().get_image().save_png(output + "/" + label + ".png")
    trace.append(row)
func finish():
    var file = FileAccess.open(output + "/trace.json", FileAccess.WRITE)
    file.store_string(JSON.stringify(trace))
    file.close()
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
func _process(_delta):
    var scenes = root.find_child("GalacticScenes", true, false)
    var loading = root.find_child("EawrLoadingScreen", true, false)
    if scenes == null or (loading != null and loading.visible): return false
    if owner == null:
        for node in root.find_children("*", "", true, false):
            if node.has_meta("eawr_cache_fixture_loaded"): owner = node
        if owner == null: return false
    frame += 1
    var menu = root.find_child("GalacticSaveMenu", true, false)
    if phase == 0 and frame == 120:
        capture("clean")
        owner.set_meta("eawr_cache_fixture_corruption", true)
        phase = 1
    elif phase == 1 and frame == 360:
        capture("corrupt")
        assert(owner.get_meta("eawr_cache_fixture_particles", 0) > 0)
        assert(owner.get_meta("eawr_cache_fixture_lines", 0) > 0)
        key(KEY_F6)
        phase = 2
    elif phase == 2 and menu != null:
        var modal = menu.find_child("SaveModal", true, false)
        if modal != null and modal.is_visible_in_tree():
            for button in modal.find_children("IDC_DIALOG_BOX_BUTTON_LEFT", "", true, false):
                if button is BaseButton and button.is_visible_in_tree() and not button.disabled:
                    button.emit_signal("pressed")
                    phase = 3
                    break
    elif phase == 3 and menu != null and not menu.visible:
        key(KEY_F5)
        phase = 4
    elif phase == 4 and owner.get_meta("eawr_cache_fixture_loaded", false):
        capture("loaded_first")
        loaded_frame = frame
        phase = 5
    elif phase == 5 and frame >= loaded_frame + 30 and not menu.visible:
        capture("loaded_settled")
        call_deferred("finish")
        phase = 6
    return false
'''.replace("__OUTPUT__", json.dumps(directory.as_posix())), encoding="utf-8")
        report = self.run_cache_driver(driver, directory, ROOT / "apps/viewer/project", [
            "--eawr-galactic", "Sandbox_Equal_Footing_Underworld",
            "--eawr-galactic-ai", "off",
            "--eawr-galactic-hud-fixture", "cache-corruption-load"])
        trace = json.loads((directory / "trace.json").read_text(encoding="utf-8"))
        self.assertEqual([row["phase"] for row in trace], ["clean", "corrupt", "loaded_first", "loaded_settled"])
        self.assertTrue(trace[2]["loaded"] and trace[3]["loaded"], "must replace the real native session owner")
        self.assertTrue(all(row["native_corrupted"] == 0 for row in trace),
                        "the actual native checkpoint/publication must have no corruption, before and after load")
        self.assertTrue(all(row["native_pause_depth"] > 0 for row in trace),
                        "the real native owner must be paused before every pixel witness")
        self.assertGreater(trace[1]["particles"], 0)
        self.assertGreater(trace[1]["lines"], 0)
        self.assertEqual(report["animation_time"], 0, "native background animation must remain held")
        self.assertEqual(report["corruption"]["attachments"], 0)
        self.assertEqual(report["corruption"]["connections"], 0)
        differences = {}
        for layer in ("background", "midground", "midforeground"):
            clean = Image.open(directory / f"clean-{layer}.png").convert("RGB")
            for phase in ("corrupt", "loaded_first", "loaded_settled"):
                image = Image.open(directory / f"{phase}-{layer}.png").convert("RGB")
                delta = ImageChops.difference(clean, image)
                delta.save(directory / f"{phase}-{layer}-difference.png")
                differences[f"{phase}-{layer}"] = sum(pixel != (0, 0, 0) for pixel in delta.getdata())
        (directory / "pixel-identity.json").write_text(json.dumps(differences, indent=2), encoding="utf-8")
        for layer in ("midground", "midforeground"):
            self.assertGreater(differences[f"corrupt-{layer}"], 0, "repro must actually draw cloud/line pixels")
        for layer in ("background", "midground", "midforeground"):
            self.assertEqual(differences[f"loaded_first-{layer}"], 0)
            self.assertEqual(differences[f"loaded_settled-{layer}"], 0)
        # No camera/profile/visibility invalidation may mask the owner teardown.
        self.assertEqual(trace[1]["requests"]["background"], trace[3]["requests"]["background"])

    def run_cache_driver(self, driver, directory, project, campaign_args, *, fixed_fps=True):
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--quit-after", "3600", "--render-thread", "separate", "--disable-vsync",
                "--log-file", str(directory / "engine.log"), "--path", str(project), "--script", str(driver), "--",
                "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"], "--eawr-galactic-paused", "yes",
                "--eawr-galactic-dismiss-story", "yes", "--eawr-audio", "off", "--eawr-render-profile", "retail",
                "--eawr-report", str(directory / "report.json"), *campaign_args]
        if fixed_fps:
            args[1:1] = ["--fixed-fps", "60"]
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=600, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-12000:])
        self.assertNotIn("ERROR:", result.stdout + result.stderr)
        report = json.loads((directory / "report.json").read_text(encoding="utf-8"))
        self.assertFalse(report["failure"], report["failure"])
        return report

    def measure(self, project=None, arm=None, version="head", paused_override=None):
        arm = arm or os.environ.get("EAWR_JOB_GC_IDLE_ARM", "medium")
        cap = int(os.environ.get("EAWR_JOB_GC_IDLE_CAP", "0"))
        caps = [int(value) for value in os.environ.get("EAWR_JOB_GC_IDLE_CAPS", str(cap)).split(",")]
        self.assertEqual(caps[0], cap)
        warmup = int(os.environ.get("EAWR_JOB_GC_IDLE_WARMUP", "60"))
        seconds = int(os.environ.get("EAWR_JOB_GC_IDLE_SECONDS", "15"))
        root_ab = os.environ.get("EAWR_JOB_GC_IDLE_ROOT_AB") == "1"
        paused = os.environ.get("EAWR_JOB_GC_IDLE_PAUSED", "1") != "0"
        if paused_override is not None:
            paused = paused_override
        campaign = os.environ.get("EAWR_JOB_GC_IDLE_CAMPAIGN", "GC_Standard_Web_Way_Imperial")
        width, height = [int(value) for value in os.environ.get("EAWR_JOB_GC_IDLE_SIZE", "2560x1440").split("x")]
        directory = Path(tempfile.mkdtemp(prefix=f"eawr-gc-idle-{version}-{arm}-{cap}-"))
        driver = DRIVER.replace("__WARMUP__", str(warmup)).replace("__SECONDS__", str(seconds))
        driver = driver.replace("__CAPS__", json.dumps(caps))
        driver = driver.replace("__ROOT_AB__", str(root_ab).lower())
        driver = driver.replace("__BEFORE__", json.dumps((directory / "root-before.png").as_posix()))
        driver = driver.replace("__WIDTH__", str(width)).replace("__HEIGHT__", str(height))
        driver = driver.replace("__SAMPLES__", json.dumps((directory / "samples.json").as_posix()))
        driver = driver.replace("__CAPTURE__", json.dumps((directory / "capture.png").as_posix()))
        (directory / "observer.gd").write_text(driver, encoding="utf-8")
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", f"{width}x{height}", "--disable-vsync",
                "--render-thread", os.environ.get("EAWR_JOB_GC_IDLE_THREAD", "separate"),
                "--log-file", str(directory / "engine.log"),
                "--max-fps", str(cap), "--path", str(project or ROOT / "apps/viewer/project"),
                "--script", str(directory / "observer.gd"), "--", "--eawr-galactic", campaign,
                "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"], "--eawr-mod-root", os.environ["EAWR_REMAKE_MOD_ROOT"],
                "--eawr-galactic-seat", "1", "--eawr-galactic-paused", "yes" if paused else "no", "--eawr-audio", "off",
                "--eawr-galactic-dismiss-story", "yes",
                "--eawr-aa", ARMS[arm], "--eawr-diagnostic-vsync", "0",
                "--eawr-report", str(directory / "report.json")]
        (directory / "command.json").write_text(json.dumps(args, indent=2), encoding="utf-8")
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True,
                                timeout=warmup + (seconds + 2) * (len(caps) + int(root_ab)) + 480, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-12000:])
        self.assertNotIn("ERROR:", result.stdout + result.stderr)
        data = json.loads((directory / "samples.json").read_text(encoding="utf-8"))
        report = json.loads((directory / "report.json").read_text(encoding="utf-8"))
        self.assertFalse(report["failure"], report["failure"])
        self.assertEqual((report["camera"]["width"], report["camera"]["height"]), (width, height))
        self.assertGreaterEqual((data["sample_start_usec"] - data["playable_at_usec"]) / 1e6, warmup)
        self.assertEqual({s["max_fps"] for s in data["samples"]}, set(caps))
        rows = []
        phases = list(dict.fromkeys((s["max_fps"], s["root_no3d"]) for s in data["samples"]))
        for cap, root_no3d in phases:
            samples = [s for s in data["samples"] if (s["max_fps"], s["root_no3d"]) == (cap, root_no3d)]
            self.assertGreater(len(samples), 30)
            timed = [s["viewports"] for s in samples if "viewports" in s]
            world = next(path for path in timed[0] if path.endswith("/world"))
            self.assertGreater(statistics.mean(s[world]["draw_calls"] for s in timed), 0,
                               "the fixed camera must actually draw the galaxy planets")
            row = {"arm": arm, "version": version, "aa": ARMS[arm], "cap": cap, "warmup_seconds": warmup,
               "root_no3d": root_no3d,
               "paused": paused, "animate_during_pause": report["animate_during_pause"],
               "animation_updates": report["animation_updates"], "frames": report["frames"],
               "live_session": report["live_session"],
               "campaign": campaign, "adapter": data["adapter"], "directory": str(directory),
               "camera": report["camera"], "render_profile": report["render_profile"], "inventory": data["inventory"],
               "scene_layers": report["scene_layers"], "frame_ms": summary([s["frame_ms"] for s in samples]),
               "fps": len(samples) * 1000 / sum(s["frame_ms"] for s in samples),
               "process_ms": summary([s["process_ms"] for s in samples]), "viewports": {},
               "draw_calls": summary([s["draw_calls"] for s in samples]),
               "primitives": summary([s["primitives"] for s in samples]),
               "refresh_requests_first": samples[0]["refresh_requests"],
               "refresh_requests_last": samples[-1]["refresh_requests"]}
            for path in timed[0]:
                row["viewports"][path] = {key: summary([s[path][key] for s in timed]) for key in timed[0][path]}
            if paused and not report["animate_during_pause"]:
                self.assertEqual(row["refresh_requests_first"], row["refresh_requests_last"],
                                 "held nonworld layers must retain their filtered image")
            rows.append(row)
            print("GC_IDLE_MEASURE", json.dumps(row), flush=True)
        (directory / "measurement.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
        if root_ab:
            from PIL import Image, ImageChops
            before = Image.open(directory / "root-before.png").convert("RGB")
            after = Image.open(directory / "capture.png").convert("RGB")
            difference = ImageChops.difference(before, after)
            different = sum(pixel != (0, 0, 0) for pixel in difference.getdata())
            difference.save(directory / "root-difference.png")
            (directory / "root-pixels.json").write_text(json.dumps({"different_pixels": different}), encoding="utf-8")
            self.assertEqual(different, 0, "root 3D diagnostic must preserve the paused image")
        return rows


if __name__ == "__main__":
    unittest.main()
