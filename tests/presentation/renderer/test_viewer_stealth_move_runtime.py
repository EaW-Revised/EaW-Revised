"""#5093 / WGM-23: real-input return from a defended stealth destination."""
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
class StealthMoveRuntime(unittest.TestCase):
    def test_stealth_fleet_moves_again_after_hostile_arrival(self):
        args = ["--eawr-galactic-hud-fixture", "fleet-drag-stealth",
            "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750"]
        initial, _ = support.PlanetHudRuntime.capture(self, "synthetic", *args)
        def point(planet):
            row = next(row for row in initial["radar_hud"]["blips"]
                if row["radar"] == 0 and row["planet"] == planet)
            x, y, w, h = row["rect"]
            return [x + w / 2, y + h / 2]
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-stealth-return-"))
        report, driver = directory / "report.json", directory / "driver.gd"
        driver.write_text(DRIVER % {"out": json.dumps(directory.as_posix()),
            "outbound": json.dumps(point(102)), "inbound": json.dumps(point(101))}, encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", *args, "--eawr-game-root", support.GAME_ROOT,
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True,
            timeout=240, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("STEALTH_ARRIVED=102", result.stdout)
        self.assertIn("STEALTH_ARRIVED=101", result.stdout)
        data = json.loads(report.read_text(encoding="utf-8"))
        drag = data["fleet_hud"]["drag"]
        self.assertEqual(drag["orders"], 2)
        self.assertEqual((drag["last_source"], drag["last_destination"]), (601, 101))
        fleet = next(row for row in data["fleets"] if row["fleet"] == 601)
        self.assertEqual(fleet["planet"], 101)
        self.assertFalse(fleet["transit"])


DRIVER = '''extends SceneTree
var viewport: SubViewport
var elapsed = 0.0
var age = 0
var stage = 0
var leg = 0
var source = Vector2.ZERO
var target = Vector2.ZERO
var points = [%(outbound)s, %(inbound)s]
var directory = %(out)s
func _initialize():
    var scene = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280,720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(scene)
    viewport.notify_mouse_entered()
    current_scene = viewport
func control(name):
    var node = viewport.find_child(name,true,false)
    return node if node != null and node.is_visible_in_tree() else null
func motion(point, held):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    event.relative = Vector2(24,0)
    viewport.push_input(event,true)
func mouse(point, down):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = point
    event.global_position = point
    viewport.push_input(event,true)
func key(code):
    var event = InputEventKey.new()
    event.keycode = code
    event.pressed = true
    viewport.push_input(event,true)
    event = InputEventKey.new()
    event.keycode = code
    event.pressed = false
    viewport.push_input(event,true)
func capture(name):
    viewport.get_texture().get_image().save_png(directory + "/" + name + ".png")
func begin_leg():
    var cell = control("q1_0001")
    assert(cell != null, "the actual fleet member cell must draw")
    source = cell.get_global_rect().get_center()
    target = Vector2(points[leg][0],points[leg][1])
    age = 0
    stage = 1
func _process(delta):
    elapsed += delta
    if elapsed < 3.0: return false
    age += 1
    if stage == 0:
        begin_leg()
    elif stage == 1:
        if age == 1:
            motion(source,false)
            mouse(source,true)
        if age == 3: motion(source + Vector2(24,0),true)
        if age == 5: motion(target,true)
        if age == 10:
            print("STEALTH_DRAG=",leg," active=",viewport.gui_is_dragging())
            capture("drag-" + str(leg))
        if age == 12:
            mouse(target,false)
            stage = 2
            age = 0
    elif stage == 2:
        if age == 20:
            # Ordinary planet navigation refocuses the destination. Its member
            # cell appears after arrival; overview badges are hidden in focus.
            key(KEY_PAGEDOWN if leg == 0 else KEY_PAGEUP)
        var planet = 102 if leg == 0 else 101
        if age > 60 and control("q1_0001") != null:
            print("STEALTH_ARRIVED=",planet)
            capture("arrival-" + str(planet))
            if leg == 0:
                leg = 1
                begin_leg()
            else:
                current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
                quit()
    if elapsed > 180.0:
        print("STEALTH_TIMEOUT stage=",stage," leg=",leg)
        capture("timeout")
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit(1)
    return false
'''


if __name__ == "__main__":
    unittest.main()
