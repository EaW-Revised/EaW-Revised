"""Overview RAID circle releases enter the real seat command inbox."""
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
class GalacticRaidRuntime(unittest.TestCase):
    def test_overview_raid_circle_and_native_drop(self):
        initial, _ = support.PlanetHudRuntime.capture(
            self, "synthetic", "--eawr-galactic-hud-fixture", "fleet-drag-raid")
        source_rect = next(row["rect"] for row in initial["fleet_hud"]["presence"]
                           if row["fleet"] == 601 and row["owner"] == 1)
        x, y, w, h = source_rect
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-raid-"))
        driver, report, capture = [directory / name for name in ("driver.gd", "report.json", "raid-preview.png")]
        driver.write_text('''extends SceneTree
var viewport: SubViewport
var elapsed = 0.0
var frame = 0
var preview_frame = 0
var source = Vector2(%s, %s)
var target = Vector2.ZERO
func _initialize():
    var scene = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(scene)
    viewport.notify_mouse_entered()
    current_scene = viewport
func pointer(point, down, motion):
    var event
    if motion:
        event = InputEventMouseMotion.new()
        event.button_mask = MOUSE_BUTTON_MASK_LEFT if down else 0
        event.relative = Vector2(24, 0)
    else:
        event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = down
    event.position = point
    event.global_position = point
    viewport.push_input(event, true)
func _process(delta):
    elapsed += delta
    if elapsed < 1.5: return false
    frame += 1
    if frame == 30:
        pointer(source, false, true)
        pointer(source, true, false)
    if frame == 32: pointer(source + Vector2(24, 0), true, true)
    if frame >= 38 and preview_frame == 0:
        var slot = viewport.find_child("galaxy_raid_slot_102", true, false)
        if slot == null or not slot.is_visible_in_tree():
            if frame >= 240:
                push_error("RAID slot did not become visible during the native drag")
                quit(1)
            return false
        assert(viewport.gui_is_dragging())
        target = slot.get_global_rect().get_center()
        assert(Rect2(Vector2.ZERO, Vector2(1280,720)).has_point(target))
        preview_frame = frame
        print("RAID_PREVIEW=", slot.get_tooltip_text())
        pointer(target, true, true)
    if preview_frame > 0 and frame == preview_frame + 2: viewport.get_texture().get_image().save_png(%s)
    if preview_frame > 0 and frame == preview_frame + 4: pointer(target, false, false)
    if preview_frame > 0 and frame == preview_frame + 62:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (x + w / 2, y + h / 2, json.dumps(str(capture).replace("\\", "/"))), encoding="utf-8")
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", "fleet-drag-raid",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("SCRIPT ERROR:", result.stderr)
        self.assertIn("RAID_PREVIEW=RAID", result.stdout)
        self.assertTrue(capture.is_file())
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["destruction_hud"]["raid_accepts"], 1)
        self.assertIn({"fleet": 601, "destination": 102}, data["destruction_hud"]["retained_raids"])
        self.assertEqual(data["live_session"]["submitted_orders"], 1)
        self.assertEqual(data["fleet_hud"]["drag"]["active_fleet"], 0)
        self.assertEqual(data["map_input"]["selected"], initial["map_input"]["selected"])
