"""GREF-01..03: lit History metrics read the session's recorded publication."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticHistoryRuntime(unittest.TestCase):
    def test_history_three_metrics_lit(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-history-"))
        cache = directory / "fonts"
        extracted = subprocess.run([sys.executable, str(support.ROOT / "tools/fonts/extract_eaw_fonts.py"),
            "--game-root", support.GAME_ROOT, "--out", str(cache), "--allow-unknown-build", "--json"],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(extracted.returncode, 0, extracted.stdout + extracted.stderr)
        driver = directory / "driver.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var output = ''' + json.dumps(directory.as_posix()) + '''
var actions = ["b_droid_help", "IDC_DATA_CHART_RADIO", "IDC_RADIO_MILITARY", "IDC_RADIO_PLANETS", "IDC_RADIO_ECONOMY"]
func _initialize():
    call_deferred("start")
func start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(name, pressed):
    var modal = root.find_child("galactic_reference_pages", true, false)
    var target = modal.find_child(name, true, false) if modal.visible else root.find_child(name, true, false)
    assert(target != null and target.is_visible_in_tree(), "History control absent: " + name)
    var point = target.get_global_rect().get_center()
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.position = point
    event.global_position = point
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = pressed
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    for index in range(actions.size()):
        var at = 90 + index * 30
        if frame == at: click(actions[index], true)
        if frame == at + 1: click(actions[index], false)
        if frame == at + 12 and index > 0:
            var modal = root.find_child("galactic_reference_pages", true, false)
            var chart = modal.find_child("IDC_DATA_CHART_IMAGE", true, false)
            assert(chart != null and chart.is_visible_in_tree() and chart.texture != null, "History publication did not draw a chart")
            var maximum = modal.find_child("IDC_Y_MAX", true, false)
            var minimum = modal.find_child("IDC_Y_MIN", true, false)
            var start = modal.find_child("IDC_STATIC_START", true, false)
            assert(minimum.is_visible_in_tree() and start.is_visible_in_tree() and maximum.is_visible_in_tree(), "History axis labels missing")
            var image = chart.texture.get_image()
            var painted = 0
            for x in range(image.get_width()):
                for y in range(image.get_height()):
                    if image.get_pixel(x, y).a > 0.5: painted += 1
            assert(painted > 500, "History has no connected archived faction line")
            print("GREF History ", actions[index], " axes visible; chart_pixels=", painted)
            image.save_png(output + "/chart-" + str(index) + ".png")
            root.get_texture().get_image().save_png(output + "/history-" + str(index) + ".png")
    if frame == 250:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        report = directory / "report.json"
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "3", "--eawr-galactic-hud-fixture", "history",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-font-cache", str(cache),
            "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on",
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Assertion failed", result.stdout + result.stderr)
        self.assertEqual(len(list(directory.glob("history-*.png"))), 4)
        data = json.loads(report.read_text(encoding="utf-8"))
        # This clock fixture exposes fast-forward, so startup submits both
        # the normal-speed reset and the initial pause (TM-05 / WGM-05).
        self.assertEqual(data["live_session"]["submitted_orders"], 2,
            "History selectors must not issue simulation commands beyond startup's speed reset and pause")
        self.assertEqual(data["live_session"]["pause_depth"], 1)
        self.assertEqual(data["live_session"]["galactic_frame"], 62,
            "History selectors must leave the fixture's recorded active tick unchanged")


if __name__ == "__main__":
    unittest.main()
