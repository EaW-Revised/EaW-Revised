"""S29: native modal input over copied observer snapshot fixtures."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support

ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticBlackMarketRuntime(unittest.TestCase):
    layouts = {}

    def initial(self, fixture, seat=1):
        key = fixture, seat
        if key not in self.layouts:
            data, capture = support.GalacticCommandBarRuntime.capture(self, seat, fixture)
            self.layouts[key] = data
            menu = data["black_market_hud"]
            self.assertTrue(menu["active"])
            self.assertEqual(menu["source"], "controlled-observer-snapshot")
            self.assertEqual((menu["buyer"], menu["planet"]), (10, 101))
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
            for row in initial["black_market_hud"]["controls"]}
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
var hover_evidence_path = ''' + json.dumps((directory / 'hover.json').as_posix()) + '''
var origin_pose
var modal_pose
var held_moved = false
var hover_target = ""
var hover_started_msec = -1
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
    var hover_only = name.begins_with("hover_")
    if hover_only:
        hover_target = name
        if hover_started_msec < 0: hover_started_msec = Time.get_ticks_msec()
    if hover_only: name = name.substr(6)
    if name != "world":
        var target = root.find_child(name, true, false)
        assert(target != null and target.is_visible_in_tree())
    var point = Vector2(points[name][0], points[name][1])
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    if hover_only: return
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = point
    event.global_position = point
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    if held_mode:
        var modal = root.find_child("black_market_modal",true,false)
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
        if frame == 100: click("market_close",true)
        if frame == 101: click("market_close",false)
        if frame == 130:
            var evidence = FileAccess.open(evidence_path,FileAccess.WRITE)
            assert(evidence != null)
            evidence.store_string(JSON.stringify({"held_moved":held_moved,"camera_unchanged":camera_projection() == modal_pose}))
            evidence.close()
    for index in range(actions.size()):
        if frame == 40 + index * 20: click(actions[index], true)
        if frame == 41 + index * 20: click(actions[index], false)
    if frame >= (140 if held_mode else 80 + actions.size() * 20):
        if hover_target != "":
            # Encyclopedia_Delay and fade use wall time, not rendered frames.
            # Keep the pointer admitted and await the pane, with a bounded timeout.
            click(hover_target, false)
            var pane = root.find_child("BlackMarketEncyclopedia",true,false)
            var visible = pane != null and pane.is_visible_in_tree() and pane.modulate.a >= 0.99
            var elapsed = Time.get_ticks_msec() - hover_started_msec
            if not visible and elapsed < 5000: return false
            var evidence = FileAccess.open(hover_evidence_path,FileAccess.WRITE)
            assert(evidence != null)
            evidence.store_string(JSON.stringify({"elapsed_msec":elapsed,"visible":visible}))
            evidence.close()
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
        self.assertLessEqual(data["black_market_hud"]["rebuilds"], 2)
        return data["black_market_hud"]

    def test_owned_and_unaffordable_visible_disabled(self):
        menu = self.initial("market")["black_market_hud"]
        controls = {row["name"]: row for row in menu["controls"]}
        self.assertTrue(controls["market_00"]["enabled"])
        self.assertTrue(controls["market_01"]["owned"])
        self.assertFalse(controls["market_01"]["enabled"])
        self.assertFalse(controls["market_04"]["enabled"])
        closed = self.native("market", ["world", "market_01", "market_04", "market_close"])
        self.assertEqual(closed["choices_accepted"], 0)
        self.assertEqual(closed["closes"], 1)
        self.assertEqual((closed["buyer"], closed["planet"], closed["absence_raw"]), (0, 0, 0))

    def test_fractional_display_admission_copies_exact_price(self):
        initial = self.initial("market-fractional")["black_market_hud"]
        row = next(row for row in initial["controls"] if row["name"] == "market_00")
        self.assertTrue(row["enabled"])
        accepted = self.native("market-fractional", ["market_00"])
        self.assertEqual(accepted["accepted_price_raw"], row["price_raw"])
        self.assertEqual((accepted["accepted_buyer"], accepted["accepted_planet"], accepted["accepted_index"]), (10, 101, 0))
        self.assertFalse(accepted["active"])
        self.assertEqual((accepted["buyer"], accepted["planet"], accepted["absence_raw"]), (0, 0, 0))

    def test_slot_collision_keeps_original_prepared_index(self):
        initial = self.initial("market-overflow")["black_market_hud"]
        self.assertEqual(initial["omitted"], 2)
        row = next(row for row in initial["controls"] if row["name"] == "market_00")
        self.assertEqual((row["index"], row["item"]), (0, 2))
        accepted = self.native("market-overflow", ["market_00"])
        self.assertEqual((accepted["accepted_index"], accepted["accepted_item"]), (0, 2))

    def test_local_human_seat_and_foreign_buyer_filter(self):
        menu = self.initial("market", seat=2)["black_market_hud"]
        self.assertTrue(menu["active"])
        data, _ = support.GalacticCommandBarRuntime.capture(self, 1, "market-foreign")
        self.assertFalse(data["black_market_hud"]["active"])

    def test_encyclopedia_authored_text_and_three_slots(self):
        menu = self.native("market", ["hover_market_00"])
        self.assertTrue(menu["encyclopedia_active"])
        self.assertEqual(menu["encyclopedia_item"], 1)
        self.assertTrue(menu["encyclopedia_title"])
        self.assertTrue(menu["encyclopedia_body"])
        self.assertEqual(len(menu["encyclopedia_units"]), 3)
        self.assertTrue(menu["encyclopedia_units"][0])
        self.assertEqual(menu["encyclopedia_units"][1:], ["", ""])

    def test_modal_cancels_held_keyboard_and_pointer_input(self):
        closed = self.native("market", [], held=True)
        self.assertEqual(closed["closes"], 1)
        self.assertEqual(closed["choices_accepted"], 0)


if __name__ == "__main__":
    unittest.main()
