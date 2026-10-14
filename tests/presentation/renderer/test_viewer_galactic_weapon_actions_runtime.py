"""S32: native modal input over copied observer snapshot fixtures."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support

ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticWeaponActionsRuntime(unittest.TestCase):
    layouts = {}

    def initial(self, fixture, seat=1):
        key = fixture, seat
        if key not in self.layouts:
            data, capture = support.GalacticCommandBarRuntime.capture(self, seat, fixture)
            self.layouts[key] = data
            menu = data["weapon_action_hud"]
            self.assertTrue(menu["active"])
            self.assertEqual(menu["source"], "controlled-observer-snapshot")
            self.assertEqual((menu["actor"], menu["planet"]), (10, 101))
            self.assertTrue(capture.is_file())
            for control in menu["controls"]:
                x, y, width, height = control["hit_rect"]
                self.assertGreater(width, 0, control)
                self.assertGreater(height, 0, control)
                self.assertGreaterEqual(x, 0, control)
                self.assertGreaterEqual(y, 0, control)
                self.assertLessEqual(x + width, 1280, control)
                self.assertLessEqual(y + height, 720, control)
        return self.layouts[key]

    def native(self, fixture, actions, seat=1, held=False):
        initial = self.initial(fixture, seat)
        points = {row["name"]: [row["hit_rect"][0] + row["hit_rect"][2] / 2,
            row["hit_rect"][1] + row["hit_rect"][3] / 2]
            for row in initial["weapon_action_hud"]["controls"]}
        points["world"] = [100, 100]
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-weapon-menu-"))
        driver, report = directory / "input.gd", directory / "report.json"
        driver.write_text('''extends SceneTree
var frame = 0
var actions = JSON.parse_string(''' + repr(json.dumps(actions)) + ''')
var points = JSON.parse_string(''' + repr(json.dumps(points)) + ''')
var capture_path = ''' + json.dumps((directory / "native.png").as_posix()) + '''
var held_mode = ''' + ('true' if held else 'false') + '''
var evidence_path = ''' + json.dumps((directory / 'held.json').as_posix()) + '''
var origin_pose
var modal_pose
var held_moved = false
func camera_projection():
    # The galaxy uses RenderingServer cameras, rather than a root Camera3D.
    # Sample the world-anchored labels that its actual camera projects.
    var pose = {}
    for label in root.find_children("planet_name_*", "Label", true, false):
        pose[str(label.name)] = [label.global_position.x, label.global_position.y]
    assert(not pose.is_empty())
    return pose
func key(code, down, ctrl=false):
    var event = InputEventKey.new()
    event.keycode = code
    event.pressed = down
    event.ctrl_pressed = ctrl
    Input.parse_input_event(event)
func right(down):
    var event = InputEventMouseButton.new()
    event.position = Vector2(100,100)
    event.global_position = event.position
    event.button_index = MOUSE_BUTTON_RIGHT
    event.pressed = down
    Input.parse_input_event(event)
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(name, down):
    if name != "world":
        var target = root.find_child(name, true, false)
        assert(target != null and target.is_visible_in_tree())
    var point = Vector2(points[name][0], points[name][1])
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = point
    event.global_position = point
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    if held_mode:
        var modal = root.find_child("weapon_action_modal",true,false)
        if frame == 30:
            assert(modal != null)
            modal.hide()
            origin_pose = camera_projection()
        if frame == 35:
            key(KEY_RIGHT,true)
            right(true)
        if frame == 50:
            held_moved = camera_projection() != origin_pose
            modal.show()
        if frame == 55: modal_pose = camera_projection()
        if frame == 60: key(KEY_PAGEDOWN,true)
        if frame == 61: key(KEY_PAGEDOWN,false)
        if frame == 70: key(KEY_HOME,true,true)
        if frame == 71: key(KEY_HOME,false,true)
        if frame == 80: key(KEY_SPACE,true)
        if frame == 81: key(KEY_SPACE,false)
        if frame == 90:
            var motion = InputEventMouseMotion.new()
            motion.position = Vector2(140,140)
            motion.relative = Vector2(40,40)
            Input.parse_input_event(motion)
        if frame == 100: click("hack_close",true)
        if frame == 101: click("hack_close",false)
        if frame == 130:
            var evidence = FileAccess.open(evidence_path,FileAccess.WRITE)
            assert(evidence != null)
            evidence.store_string(JSON.stringify({"held_moved":held_moved,"camera_unchanged":camera_projection() == modal_pose}))
            evidence.close()
    for index in range(actions.size()):
        if frame == 40 + index * 20: click(actions[index], true)
        if frame == 41 + index * 20: click(actions[index], false)
    if frame == (140 if held_mode else 80 + actions.size() * 20):
        root.get_texture().get_image().save_png(capture_path)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", str(seat),
            "--eawr-galactic-hud-fixture", fixture, "--eawr-game-root", GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report)], cwd=ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertEqual(data["map_input"]["selected"], initial["map_input"]["selected"])
        self.assertEqual(data["map_input"]["selection_invalidations"], 0)
        self.assertEqual(data["live_session"]["submitted_orders"], 1)
        self.assertTrue(data["paused"], "modal Space leaked to pause input")
        if held:
            evidence = json.loads((directory / "held.json").read_text())
            self.assertTrue(evidence["held_moved"], "held input must move before modal activation")
            self.assertTrue(evidence["camera_unchanged"], "modal must cancel held/captured camera input and not resurrect it on close")
        self.assertLessEqual(data["weapon_action_hud"]["rebuilds"], 2)
        return data["weapon_action_hud"]

    def test_fractional_cost_rounds_display_only(self):
        menu = self.initial("hack-fractional")["weapon_action_hud"]
        self.assertEqual(menu["cost_text"], "$ 251")
        accepted = self.native("hack-fractional", ["hack_hack"])
        self.assertEqual(accepted["accepted_price_raw"], menu["price_raw"])

    def test_modal_keyboard_and_held_world_gestures(self):
        closed = self.native("hack", [], held=True)
        self.assertEqual(closed["closes"],1)
        self.assertEqual(closed["choices_accepted"],0)

    def test_hack_native_accept_and_no_world_leak(self):
        menu = self.initial("hack")["weapon_action_hud"]
        controls = {row["name"]: row for row in menu["controls"]}
        self.assertTrue(controls["hack_hack"]["enabled"])
        self.assertTrue(menu["affordable"])
        self.assertEqual(menu["weapon"], 20)
        accepted = self.native("hack", ["world", "hack_hack"])
        self.assertEqual(accepted["choices_accepted"], 1)
        self.assertEqual(accepted["accepted_price_raw"], menu["price_raw"])
        self.assertFalse(accepted["active"])

    def test_insufficient_funds_and_native_cancel(self):
        menu = self.initial("hack-poor")["weapon_action_hud"]
        controls = {row["name"]: row for row in menu["controls"]}
        self.assertFalse(controls["hack_hack"]["enabled"])
        closed = self.native("hack-poor", ["world", "hack_hack", "hack_close"])
        self.assertEqual(closed["closes"], 1)
        self.assertEqual(closed["choices_accepted"], 0)
        self.assertIsNone(closed["accepted_price_raw"])
        self.assertFalse(closed["active"])

    def test_native_cancel_each_faction(self):
        for seat in [1, 3, 4]:
            with self.subTest(seat=seat):
                closed = self.native("hack", ["hack_close"], seat)
                self.assertEqual(closed["closes"], 1)
                self.assertEqual(closed["choices_accepted"], 0)

FIXED_SCALE = 1 << 24  # sim::math::Fixed::fractional_bits
LIVE_DRIVER = '''extends SceneTree
var frame = 0
var action = %(action)s
var seen = 0
var clicked_frame = 0
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(name, down):
    var target = root.find_child(name, true, false)
    assert(target != null and target.is_visible_in_tree())
    var rect = target.get_global_rect()
    var point = rect.position + rect.size / 2.0
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = point
    event.global_position = point
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    var modal = root.find_child("weapon_action_modal", true, false)
    if clicked_frame == 0 and modal != null and modal.is_visible_in_tree():
        seen += 1
        # Let the seat command publish and the menu settle for a few frames before acting.
        if seen == 20:
            clicked_frame = frame
            click(action, true)
    if clicked_frame != 0 and frame == clicked_frame + 1:
        click(action, false)
    if (clicked_frame != 0 and frame == clicked_frame + 150) or frame == 1800:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticLiveWeaponActionsRuntime(unittest.TestCase):
    """One focused case: the LIVE campaign HUD hack menu, owned by the host (no copied snapshot)."""

    def run_live(self, action):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-weapon-live-"))
        driver, report = directory / "input.gd", directory / "report.json"
        driver.write_text(LIVE_DRIVER % {"action": json.dumps(action)}, encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "1",
            "--eawr-galactic-hud-fixture", "hack-live", "--eawr-game-root", GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report)], cwd=ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        menu = data["weapon_action_hud"]
        self.assertEqual(menu["source"], "campaign-snapshot")
        # The host retracts or settles the preparation, so the published facts are gone by the report; only the
        # HUD's own counters and the executed economy remain as evidence.
        self.assertFalse(menu["active"], "the modal leaves the screen after acting")
        self.assertTrue(data["paused"], "modal input leaked to pause")
        return data

    def test_live_choose_pays_through_the_host(self):
        data = self.run_live("hack_hack")
        menu = data["weapon_action_hud"]
        self.assertEqual(menu["choices_accepted"], 1)
        self.assertEqual(menu["accepted_price_raw"], 250 * FIXED_SCALE)
        self.assertEqual(menu["closes"], 0)
        # The synthetic account is 12422.5 and the authored hack price is 250: only the host's executed command
        # can have moved the displayed balance.
        self.assertEqual(data["command_bar"]["account"], "12172")
        self.assertEqual(data["live_session"]["submitted_orders"], 2)  # pause + the queued typed execution

    def test_live_cancel_retracts_and_pays_nothing(self):
        data = self.run_live("hack_close")
        menu = data["weapon_action_hud"]
        self.assertEqual(menu["choices_accepted"], 0)
        self.assertEqual(menu["closes"], 1)
        self.assertIsNone(menu["accepted_price_raw"])
        self.assertEqual(data["command_bar"]["account"], "12422")
        self.assertEqual(data["live_session"]["submitted_orders"], 2)  # pause + the queued typed cancellation


if __name__ == "__main__":
    unittest.main()
