"""GHUD-S20: native admitted object hover and encyclopedia pane input routing."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support
ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticEncyclopediaRuntime(unittest.TestCase):
    capture = support.GalacticCommandBarRuntime.capture

    def native_run(self, actions, seat=3):
        if not hasattr(self, "production_hit_points"):
            initial, _ = self.capture(seat, "production")
            self.production_hit_points = {control["name"]:
                [control["hit_rect"][0] + control["hit_rect"][2] / 2,
                 control["hit_rect"][1] + control["hit_rect"][3] / 2]
                for control in initial["command_bar"]["controls"]}
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-production-input-"))
        report = directory / "report.json"
        driver = directory / "input.gd"
        driver.write_text('''extends SceneTree
var action_index = 0
var release_pending = false
var next_action_time = -1.0
var finish_time = -1.0
var actions = JSON.parse_string(''' + repr(json.dumps(actions)) + ''')
var hovered_income = false
var capture_path = ''' + json.dumps((directory / "native.png").as_posix()) + '''
var hit_points = JSON.parse_string(''' + repr(json.dumps(self.production_hit_points)) + ''')
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(name, down):
    var hover_only = name.begins_with("hover:")
    if hover_only: name = name.substr(6)
    var right = name.begins_with("right:")
    if right: name = name.substr(6)
    var target = root.find_child(name, true, false)
    assert(target != null and target.visible)
    if name.begins_with("b_Create_") or name.begins_with("queue"):
        assert(target.tooltip_text.is_empty(), "object hover must use only the timed shared encyclopedia")
    var position = target.get_global_rect().get_center()
    if hit_points.has(name):
        var point = hit_points[name]
        position = Vector2(point[0], point[1])
    var motion = InputEventMouseMotion.new()
    motion.position = position
    motion.global_position = position
    Input.parse_input_event(motion)
    if hover_only: return
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_RIGHT if right else MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = position
    event.global_position = position
    Input.parse_input_event(event)
func _process(_delta):
    var now = Time.get_ticks_msec() / 1000.0
    if next_action_time < 0.0: next_action_time = now + 1.0
    if release_pending:
        click(actions[action_index], false)
        release_pending = false
        action_index += 1
        next_action_time = now + 2.0
        if action_index == actions.size(): finish_time = now + 3.0
    elif action_index < actions.size() and now >= next_action_time:
        click(actions[action_index], true)
        release_pending = true
    # CARD-18 is a wall-clock delay, independent of frame rate and campaign pause.
    if finish_time >= 0.0 and now >= finish_time:
        if actions.has("hover:Text_Day_Credits"): assert(hovered_income)
        root.get_texture().get_image().save_png(capture_path)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", str(seat),
            "--eawr-galactic-hud-fixture", "production", "--eawr-game-root", GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report)],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertNotIn("SCRIPT ERROR:", result.stdout + result.stderr)
        self.assertEqual(data["map_input"]["selected"], 101)
        self.assertEqual(data["map_input"]["selection_invalidations"], 0)
        popup = data["encyclopedia"]
        if popup["visible"]:
            x, y, width, height = popup["rect"]
            self.assertGreaterEqual(x, 0)
            self.assertGreaterEqual(y, 0)
            self.assertLessEqual(x + width, 1280.01)  # JSON float precision
            self.assertLessEqual(y + height, 720.01)
            title = next(row for row in data["command_bar"]["planet_block"] if row["name"] == "Text_Planet")
            self.assertLess(y + height, title["rect"][1], "ordinary build hover must clear the command readouts")
        return data

    def test_admitted_object_hover_and_pane_click_do_not_select_world(self):
        initial, _ = self.capture(3, "production")
        button = next(row["button"] for row in initial["command_bar"]["production_choices"] if row["type_id"] == 1000)
        data = self.native_run(["hover:" + button, "GalacticEncyclopedia", "hover:" + button])
        popup = data["encyclopedia"]
        self.assertEqual(popup["type"], 1000)
        self.assertTrue(popup["visible"])
        self.assertGreater(popup["alpha"], 0)
        self.assertTrue(popup["title"])
        self.assertNotIn("TEXT_", popup["title"])
        self.assertEqual(popup["price"], 100)
        self.assertTrue(popup["icon"])
        self.assertEqual(data["live_session"]["submitted_orders"], initial["live_session"]["submitted_orders"])

    def test_disabled_starbase_hover_uses_prepared_price_and_denial(self):
        initial, _ = self.capture(3, "production")
        button = next(row["button"] for row in initial["command_bar"]["production_choices"] if row["type_id"] == 1001)
        data = self.native_run([button, "hover:" + button])
        popup = data["encyclopedia"]
        self.assertEqual(popup["type"], 1001)
        self.assertTrue(popup["visible"])
        self.assertEqual(popup["price"], 100)
        self.assertIn("No available slot", popup["body"])
        self.assertEqual(data["command_bar"]["account"], "900")
        self.assertEqual(data["live_session"]["submitted_orders"], initial["live_session"]["submitted_orders"] + 1)

    def test_unknown_preview_has_no_object_details(self):
        data, _ = self.capture(1)
        self.assertEqual(data["encyclopedia"]["type"], 0)
        self.assertFalse(data["encyclopedia"]["visible"])

    def test_queue_hover_reuses_localized_production_name(self):
        initial, _ = self.capture(3, "production")
        button = next(row["button"] for row in initial["command_bar"]["production_choices"] if row["type_id"] == 1000)
        production = self.native_run(["hover:" + button])["encyclopedia"]
        data = self.native_run([button, "hover:queue00"])
        cards = data["queue_encyclopedia"]
        queued = cards["tooltip"]
        self.assertIsNotNone(queued)
        self.assertTrue(queued["external"])
        self.assertGreater(queued["alpha"], 0)
        self.assertEqual(queued["text"], production["title"])
        self.assertEqual(queued["price"], 100)
        self.assertNotIn("TEXT_", queued["text"])
        self.assertEqual(cards["popup_content_builds"], 1,
                         "stationary queue hover must retain its full details")
        self.assertEqual(data["queue_hover_publications"], 1)

    def test_technology_queue_hover_uses_queued_type_and_galactic_cost(self):
        initial, _ = self.capture(3, "production")
        # This fixture's type 1002 is a technology offer, priced at 200.
        button = next(row["button"] for row in initial["command_bar"]["production_choices"] if row["type_id"] == 1002)
        production = self.native_run(["hover:" + button])["encyclopedia"]
        data = self.native_run([button, "hover:queue00"])
        cards = data["queue_encyclopedia"]
        queued = cards["tooltip"]
        self.assertIsNotNone(queued)
        self.assertEqual(queued["text"], production["title"])
        self.assertEqual(queued["description"], production["body"])
        self.assertEqual(queued["price"], 200)
        self.assertEqual(cards["popup_content_builds"], 1)
        self.assertEqual(data["queue_hover_publications"], 1)

    def test_removing_hovered_queue_entry_clears_shared_tooltip(self):
        initial, _ = self.capture(3, "production")
        button = next(row["button"] for row in initial["command_bar"]["production_choices"] if row["type_id"] == 1000)
        data = self.native_run([button, "hover:queue00", "right:queue00"])
        self.assertIsNone(data["queue_encyclopedia"]["tooltip"])
        self.assertEqual(data["queue_hover_publications"], 2)
        self.assertFalse(data["encyclopedia"]["visible"])
        self.assertFalse(data["production_queues"])
        self.assertEqual(data["live_session"]["submitted_orders"], initial["live_session"]["submitted_orders"] + 2)


if __name__ == "__main__":
    unittest.main()
