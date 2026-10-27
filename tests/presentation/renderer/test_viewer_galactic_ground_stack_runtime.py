"""GGS-01/04/06/09: native ground artwork, clicks, launch and transfer drags."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
except ImportError:
    import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticGroundStackRuntime(unittest.TestCase):
    def test_empty_ground_slot_returns_for_ground_drag_and_accepts_landing(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-ground-slot-return-"))
        report, probe, driver = [directory / name for name in ["report.json", "probe.json", "driver.gd"]]
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
var viewport
var ground_point = Vector2.ZERO
var orbit_point = Vector2.ZERO
var space_point = Vector2.ZERO
var samples = {}
var directory = %s
func _initialize():
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(load("res://main.tscn").instantiate())
    viewport.notify_mouse_entered()
    current_scene = viewport
func key(code):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = down
        viewport.push_input(event, true)
func pointer(point, down, motion):
    var event
    if motion:
        event = InputEventMouseMotion.new()
        event.relative = Vector2(24, 0)
        event.button_mask = MOUSE_BUTTON_MASK_LEFT if down else 0
    else:
        event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = down
    event.position = point
    event.global_position = point
    viewport.push_input(event, true)
func sample(phase):
    var ground = current_scene.find_child("ground_presence_101", true, false)
    samples[phase] = {"visible": ground != null and ground.is_visible_in_tree(),
        "dragging": viewport.gui_is_dragging()}
    viewport.get_texture().get_image().save_png(directory + "/" + phase + ".png")
func _process(delta):
    elapsed += delta
    if elapsed < 1.5: return false
    frame += 1
    if frame == 10: key(KEY_PAGEUP)
    if frame == 12: key(KEY_PAGEDOWN)
    if frame == 68:
        var ground = current_scene.find_child("ground_presence_101", true, false)
        var orbit = current_scene.find_child("fleet_presence_101_0", true, false)
        var space = current_scene.find_child("fleet_presence_101_1", true, false)
        assert(ground and orbit and space)
        ground_point = ground.get_global_rect().get_center()
        orbit_point = orbit.get_global_rect().get_center()
        space_point = space.get_global_rect().get_center()
        sample("before")
    if frame == 90: pointer(ground_point, true, false)
    if frame == 94: pointer(ground_point + Vector2(24, 0), true, true)
    if frame == 98: pointer(orbit_point, true, true)
    if frame == 100: pointer(orbit_point, false, false)
    if frame == 150: sample("empty")
    if frame == 160: pointer(space_point, true, false)
    if frame == 164: pointer(space_point + Vector2(24, 0), true, true)
    if frame == 168: pointer(ground_point, true, true)
    if frame == 180: sample("space-held")
    if frame == 184: pointer(Vector2(1260, 20), false, false)
    if frame == 210: pointer(orbit_point, true, false)
    if frame == 214: pointer(orbit_point + Vector2(24, 0), true, true)
    if frame == 218: pointer(ground_point, true, true)
    if frame == 230: sample("ground-held")
    if frame == 235: pointer(ground_point, false, false)
    if frame == 290:
        sample("returned")
        var file = FileAccess.open(%s, FileAccess.WRITE)
        file.store_string(JSON.stringify(samples))
        file.close()
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(directory.as_posix()), json.dumps(probe.as_posix())), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", "ground-slot-return",
            "--eawr-game-root", support.GAME_ROOT,
            "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True,
            timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        samples = json.loads(probe.read_text(encoding="utf-8"))
        self.assertTrue(samples["before"]["visible"], samples)
        self.assertFalse(samples["empty"]["visible"], samples)
        self.assertTrue(samples["space-held"]["dragging"], samples)
        self.assertFalse(samples["space-held"]["visible"], samples)
        self.assertTrue(samples["ground-held"]["dragging"] and samples["ground-held"]["visible"], samples)
        self.assertTrue(samples["returned"]["visible"], samples)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["ground_hud"]["launch_orders"], 1)
        self.assertEqual(data["ground_hud"]["launch"]["units"], [8001, 8002])
        drag = data["fleet_hud"]["drag"]
        self.assertEqual((drag["orders"], drag["last_kind"]), (1, 2))
        ground = next(row for row in data["fleet_hud"]["presence"] if row["ground"] and row["planet"] == 101)
        self.assertTrue(ground["visible"])
        self.assertGreaterEqual(sum(row["count"] for row in ground["contents"]), 2)

    def test_count_only_payload_representative_keeps_tooltip_anonymous(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-ground-representative-"))
        report, capture, driver = [directory / name for name in ["report.json", "capture.png", "driver.gd"]]
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
var viewport
func _initialize():
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(load("res://main.tscn").instantiate())
    viewport.notify_mouse_entered()
    current_scene = viewport
func key(code):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = down
        viewport.push_input(event, true)
func _process(delta):
    elapsed += delta
    if elapsed < 1.5: return false
    frame += 1
    # Native previous/next navigation centres Alpha without leaving overview.
    if frame == 10: key(KEY_PAGEUP)
    if frame == 12: key(KEY_PAGEDOWN)
    if frame == 90:
        viewport.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % json.dumps(capture.as_posix()), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", "ground-representative",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True,
            timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        ground = [row for row in data["fleet_hud"]["presence"] if row["ground"]]
        self.assertEqual(len(ground), 1, ground)
        self.assertTrue(ground[0]["visible"] and ground[0]["ship"], ground)
        x, y, width, height = ground[0]["rect"]
        self.assertTrue(width > 0 and height > 0 and 0 <= x and 0 <= y
                        and x + width <= 1280 and y + height <= 720, ground)
        self.assertEqual(ground[0]["display_type"], 1001, ground)
        self.assertEqual(ground[0]["contents"], [{"type": None, "count": 2}])
        self.assertIsNone(ground[0]["fleet"])
        self.assertTrue(capture.is_file())

    def test_overview_ground_stack_click_and_launch(self):
        self._overview_ground_stack(False)

    def test_overview_ground_to_ground_transfer(self):
        self._overview_ground_stack(True)

    def _overview_ground_stack(self, transfer):
        from tools.fonts.extract_eaw_fonts import extract, locate_executable
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-ground-overview-"))
        fonts = directory / "fonts"
        extract(locate_executable(pathlib.Path(support.GAME_ROOT)), fonts, False, False)
        report, capture, before, probe, driver = [directory / name for name in
            ["report.json", "capture.png", "before.png", "probe.json", "driver.gd"]]
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
var viewport
var source = Vector2.ZERO
var target = Vector2.ZERO
func _initialize():
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(load("res://main.tscn").instantiate())
    viewport.notify_mouse_entered()
    current_scene = viewport
func key(code):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = down
        viewport.push_input(event, true)
func motion(point, held):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    event.relative = Vector2(24, 0)
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    viewport.push_input(event, true)
func mouse(point, down):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.position = point
    event.global_position = point
    event.pressed = down
    viewport.push_input(event, true)
func locate():
    var ground = current_scene.find_child("ground_presence_101", true, false)
    var orbit = current_scene.find_child(%s, true, false)
    var evidence = {"ground": ground != null and ground.is_visible_in_tree(),
        "orbit": orbit != null and orbit.is_visible_in_tree()}
    if ground and orbit:
        source = ground.get_global_rect().get_center()
        target = orbit.get_global_rect().get_center()
        # The nearby synthetic planets overlap a source orbit marker at the
        # receiver centre. Drop on the exposed edge of the receiving control.
        if orbit.name == "ground_presence_102":
            target.x = orbit.get_global_rect().end.x - 2.0
        evidence["source"] = [source.x, source.y]
        evidence["target"] = [target.x, target.y]
    var file = FileAccess.open(%s, FileAccess.WRITE)
    file.store_string(JSON.stringify(evidence))
    file.close()
func _process(delta):
    elapsed += delta
    if elapsed < 1.5: return false
    frame += 1
    if frame == 10: key(KEY_PAGEUP)
    if frame == 12: key(KEY_PAGEDOWN)
    if frame == 68: locate()
    if frame == 70: viewport.get_texture().get_image().save_png(%s)
    if frame == 72:
        motion(source, false)
        mouse(source, true)
    if frame == 74: mouse(source, false)
    if frame == 90: mouse(source, true)
    if frame == 94: motion(source + Vector2(24, 0), true)
    if frame == 98: motion(target, true)
    if frame == 100: mouse(target, false)
    if frame == 160:
        viewport.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps("ground_presence_102" if transfer else "fleet_presence_101_0"),
            *(json.dumps(str(path).replace("\\", "/")) for path in [probe, before, capture])), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", "ground-transfer" if transfer else "ground-launch-move",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-font-cache", str(fonts),
            "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True,
            timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        evidence = json.loads(probe.read_text(encoding="utf-8"))
        self.assertTrue(evidence["ground"] and evidence["orbit"], evidence)
        for point in [evidence["source"], evidence["target"]]:
            self.assertTrue(0 < point[0] < 1280 and 0 < point[1] < 720, evidence)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["map_input"]["selected"], 101)
        self.assertEqual(data["ground_hud"]["launch_orders"], 1, data["ground_hud"])
        self.assertEqual(data["ground_hud"]["launch"]["units"], [8001, 8002])
        if transfer:
            self.assertEqual(data["ground_hud"]["launch"]["command"], "transfer_surface opcode 25")
            self.assertEqual(data["ground_hud"]["launch"]["path"], [101, 102])
            ground = [row for row in data["fleet_hud"]["presence"] if row["ground"] and row["visible"]]
            self.assertEqual([row["planet"] for row in ground], [102], ground)
            self.assertEqual(ground[0]["contents"], [{"type": 1000, "count": 3}])
        self.assertTrue(before.is_file() and capture.is_file())
