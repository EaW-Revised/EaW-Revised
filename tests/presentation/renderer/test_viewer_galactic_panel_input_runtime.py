"""#4442: panel backgrounds own native hover and clicks above a world planet."""
import json
import math
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support
ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticPanelInputRuntime(unittest.TestCase):
    def test_panel_background_blocks_planet_hover_tooltip_and_selection(self):
        initial, _ = support.GalacticCommandBarRuntime.capture(self, 3)
        camera = initial["camera"]
        dot = lambda a, b: sum(x * y for x, y in zip(a, b))
        def normalized(a):
            length = math.sqrt(dot(a, a))
            return [x / length for x in a]
        forward = normalized([b - a for a, b in zip(camera["eye"], camera["target"])])
        right = normalized([-forward[2], 0, forward[0]])
        up = [right[1] * forward[2] - right[2] * forward[1],
              right[2] * forward[0] - right[0] * forward[2],
              right[0] * forward[1] - right[1] * forward[0]]
        click_points = {}
        for row in initial["planets"]:
            x, y, z = row["position"]
            offset = [value - eye for value, eye in zip((x, z, -y), camera["eye"])]
            scale = camera["height"] / (2 * dot(offset, forward)
                * math.tan(math.radians(camera["vertical_fov"]) / 2))
            click_points[str(row["id"])] = [camera["width"] / 2 + dot(offset, right) * scale,
                                          camera["height"] / 2 - dot(offset, up) * scale]
        results = []
        for covered in (True, False):
            directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-panel-input-"))
            report = directory / "report.json"
            observation = directory / "pointer.json"
            script = directory / "input.gd"
            script.write_text('''extends SceneTree
var frame = 0
var point = Vector2.ZERO
var label
var click_points = JSON.parse_string(POINTS)
var click_point = Vector2.ZERO
var panel
var planet = 0
var covered = COVERED
func _initialize():
    call_deferred("start")
func start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func ignore_children(node):
    for child in node.get_children():
        if child is Control: child.mouse_filter = Control.MOUSE_FILTER_IGNORE
        ignore_children(child)
func _process(_delta):
    frame += 1
    if frame == 100:
        for candidate in root.find_children("planet_name_*", "Label", true, false):
            var center = candidate.get_global_rect().get_center()
            if candidate.is_visible_in_tree() and Rect2(100, 100, 1000, 400).has_point(center):
                label = candidate
                point = center
                planet = int(str(candidate.name).trim_prefix("planet_name_"))
                break
        assert(label != null, "No visible world planet for positive pick control")
        label.tooltip_text = "Synthetic world planet"
        var projected = click_points[str(planet)]
        click_point = Vector2(projected[0], projected[1])
        var button = root.find_child("b_space_tab", true, false)
        assert(button != null)
        panel = button.get_parent().get_parent()
        assert(panel.is_class("EawrUiHitMask"))
        var panel_point = button.get_global_rect().get_center()
        panel.position += point - panel_point
        # Exercise an empty background: children supply no active target.
        ignore_children(panel)
        panel.visible = covered
        var motion = InputEventMouseMotion.new()
        motion.position = point
        motion.global_position = point
        Input.parse_input_event(motion)
    if frame == 240:
        var hovered = root.gui_get_hovered_control()
        var label_hovered = hovered == label
        var tooltip = false
        for node in root.find_children("*", "PopupPanel", true, false):
            if node.visible: tooltip = true
        var file = FileAccess.open(OBSERVATION, FileAccess.WRITE)
        file.store_string(JSON.stringify({"planet": planet, "label_hovered": label_hovered,
            "tooltip": tooltip, "panel_hovered": hovered == panel,
            "tooltip_text": hovered.get_tooltip() if hovered != null else ""}))
        file.close()
        # Label text is offset from the world pick area. Move the same background
        # over the actual projected planet before dispatching the selection click.
        panel.position += click_point - point
        for down in [true, false]:
            var event = InputEventMouseButton.new()
            event.button_index = MOUSE_BUTTON_LEFT
            event.position = click_point
            event.global_position = click_point
            event.pressed = down
            Input.parse_input_event(event)
    if frame == 260:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''.replace("POINTS", repr(json.dumps(click_points))).replace("COVERED", "true" if covered else "false").replace(
                "OBSERVATION", json.dumps(observation.as_posix())), encoding="utf-8")
            run = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--script", str(script), "--",
                "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "3",
                "--eawr-game-root", GAME_ROOT, "--eawr-galactic-paused", "yes",
                "--eawr-report", str(report)], cwd=ROOT, capture_output=True, text=True,
                check=False, timeout=300)
            (directory / "viewer.log").write_text(run.stdout + run.stderr, encoding="utf-8")
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertTrue(report.is_file(), run.stdout + run.stderr)
            data = json.loads(report.read_text(encoding="utf-8"))
            pointer = json.loads(observation.read_text(encoding="utf-8"))
            results.append((data, pointer))
        blocked, pointer = results[0]
        self.assertTrue(pointer["panel_hovered"], pointer)
        self.assertFalse(pointer["label_hovered"], pointer)
        self.assertFalse(pointer["tooltip"], pointer)
        self.assertEqual(pointer["tooltip_text"], "")
        self.assertEqual(blocked["map_input"]["selected"], 0)
        self.assertEqual(blocked["hud"]["selected"], 0)
        exposed, pointer = results[1]
        self.assertTrue(pointer["label_hovered"], pointer)
        self.assertEqual(pointer["tooltip_text"], "Synthetic world planet")
        self.assertEqual(exposed["map_input"]["selected"], pointer["planet"])


if __name__ == "__main__":
    unittest.main()

