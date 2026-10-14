"""GHUD-S06: native income filter and queued population read the immutable forecast."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support
ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticEconomyRuntime(unittest.TestCase):
    capture = support.GalacticCommandBarRuntime.capture

    def native_run(self, actions, seat=3):
        if getattr(self, "production_hit_points_seat", None) != seat:
            initial, _ = self.capture(seat, "production")
            self.production_hit_points = {control["name"]:
                [control["hit_rect"][0] + control["hit_rect"][2] / 2,
                 control["hit_rect"][1] + control["hit_rect"][3] / 2]
                for control in initial["command_bar"]["controls"]}
            self.production_hit_points_seat = seat
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-production-input-"))
        report = directory / "report.json"
        driver = directory / "input.gd"
        driver.write_text('''extends SceneTree
var frame = 0
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
    frame += 1
    for index in range(actions.size()):
        if frame == 40 + index * 120: click(actions[index], true)
        if frame == 41 + index * 120: click(actions[index], false)
    if frame == 200:
        var hovered = root.gui_get_hovered_control()
        hovered_income = hovered != null and hovered.name == "Text_Day_Credits"
        if hovered_income:
            root.get_texture().get_image().save_png(capture_path.get_base_dir() + "/income-hover.png")
    if frame == 160 + actions.size() * 120:
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
        self.assertEqual(data["map_input"]["selected"], 101)
        self.assertEqual(data["map_input"]["selection_invalidations"], 0)
        return data

    def test_income_hover_filter_and_queued_population(self):
        initial, _ = self.capture(3, "production")
        bar = initial["command_bar"]
        self.assertTrue(bar["income"])
        self.assertTrue(bar["population"])
        self.assertIn("Base:", bar["income_tooltip"])
        self.assertRegex(bar["income_tooltip"], r"Trade Routes: \$ -?\d+")
        self.assertIn("Political: Unavailable", bar["income_tooltip"])
        self.assertIn("Total:", bar["income_tooltip"])
        self.assertNotIn("Upkeep", bar["income_tooltip"])
        labels = {row["name"]: row for row in bar["economy_labels"]}
        self.assertEqual(labels["Text_Day_Credits"]["text"], bar["income"])
        self.assertEqual(labels["Text_Pop_Cap"]["text"], bar["population"])
        self.assertEqual(labels["Text_Day_Credits"]["tooltip"], bar["income_tooltip"])
        choices = {row["type_id"]: row["button"] for row in bar["production_choices"]}
        data = self.native_run(["b_filters0", "hover:Text_Day_Credits", choices[1000]])
        self.assertTrue(data["command_bar"]["income_layer_visible"])
        self.assertTrue(any(row["visible"] for row in data["command_bar"]["income_labels"]))
        self.assertEqual(data["command_bar"]["income"], bar["income"])
        usage, capacity = map(int, bar["population"].split(" / "))
        self.assertEqual(data["command_bar"]["population"], f"{usage + 1} / {capacity}")
        self.assertEqual(data["live_session"]["submitted_orders"], 2)
        restored = self.native_run(["b_filters0", "b_filters0"])
        self.assertFalse(restored["command_bar"]["income_layer_visible"])
        self.assertFalse(any(row["visible"] for row in restored["command_bar"]["income_labels"]))

    def test_missing_providers_clear_economic_readouts(self):
        data, _ = self.capture(1)
        self.assertEqual(data["command_bar"]["income"], "")
        self.assertEqual(data["command_bar"]["population"], "")
        self.assertEqual(data["command_bar"]["income_tooltip"], "")

    def test_income_owner_colour_and_level_rows_make_room_for_credits(self):
        for seat in [1, 3]:
            with self.subTest(seat=seat):
                without = self.native_run(["b_filters1"], seat)
                with_income = self.native_run(["b_filters1", "b_filters0"], seat)
                names = {row["planet"]: row for row in with_income["hud"]["labels"]}
                incomes = {row["planet"]: row for row in with_income["command_bar"]["income_labels"]
                           if row["visible"]}
                self.assertTrue(incomes)
                for planet, row in incomes.items():
                    self.assertEqual(row["colour"], names[planet]["colour"])
                    self.assertAlmostEqual(row["rect"][0], names[planet]["global_rect"][0], delta=1)
                pips_before = [row for row in without["hud"]["level_pips"] if row["visible"]]
                pips_after = [row for row in with_income["hud"]["level_pips"] if row["visible"]]
                self.assertTrue(pips_before)
                self.assertEqual(len(pips_before), len(pips_after))
                for before, after in zip(pips_before, pips_after):
                    self.assertEqual(before["planet"], after["planet"])
                    self.assertEqual(before["rect"][0], after["rect"][0])
                    self.assertAlmostEqual(after["rect"][1] - before["rect"][1], 11 * 720 / 768, delta=0.01)
                    if after["planet"] in incomes:
                        owner = incomes[after["planet"]]["colour"]
                        intensity = max(after["colour"]) / max(owner)
                        for actual, channel in zip(after["colour"], owner):
                            self.assertAlmostEqual(actual, channel * intensity, delta=1, msg="presence pip lost its owner tint")
                        income = incomes[after["planet"]]["rect"]
                        self.assertLessEqual(income[1] + income[3], after["rect"][1], "income overlaps a level icon")


if __name__ == "__main__":
    unittest.main()
