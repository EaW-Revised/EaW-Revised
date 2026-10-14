"""GREF: one native-input case opens all reference pages and Summary categories."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticReferencePagesRuntime(unittest.TestCase):
    def test_help_tabs_categories_and_escape(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-reference-pages-"))
        cache = directory / "fonts"
        extracted = subprocess.run([sys.executable, str(support.ROOT / "tools/fonts/extract_eaw_fonts.py"),
            "--game-root", support.GAME_ROOT, "--out", str(cache), "--allow-unknown-build", "--json"],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(extracted.returncode, 0, extracted.stdout + extracted.stderr)
        actions = ["b_droid_help", "IDC_DATA_CHART_RADIO", "IDC_TECH_TREE_RADIO", "IDC_TECH_TREE_SPACE",
                   "IDC_SUMMARY_SCREEN_RADIO", "IDC_ECONOMY_RADIO", "IDC_FLEETS_RADIO", "IDC_BUTTON_EXIT", "b_droid_help"]
        expected = ["IDD_DROID_LOG", "IDD_DATA_CHART", "IDD_TECH_TREE_EMPIRE", "IDD_TECH_TREE_EMPIRE_SPACE",
                    "IDD_SUMMARY_SCREEN_PRODUCTION", "IDD_SUMMARY_SCREEN_ECONOMY", "IDD_SUMMARY_SCREEN_FLEETS", "", "IDD_SUMMARY_SCREEN_PRODUCTION"]
        driver = directory / "driver.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var actions = JSON.parse_string(''' + repr(json.dumps(actions)) + ''')
var expected = JSON.parse_string(''' + repr(json.dumps(expected)) + ''')
var output = ''' + json.dumps(directory.as_posix()) + '''
func _initialize():
    call_deferred("start")
func start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(name, down):
    var modal = root.find_child("galactic_reference_pages", true, false)
    var target = modal.find_child(name, true, false) if modal.visible else root.find_child(name, true, false)
    assert(target != null and target.is_visible_in_tree(), "reference control absent: " + name)
    var point = target.get_global_rect().get_center()
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.position = point
    event.global_position = point
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    for index in range(actions.size()):
        var at = 60 + index * 30
        if frame == at: click(actions[index], true)
        if frame == at + 1: click(actions[index], false)
        if frame == at + 10:
            var modal = root.find_child("galactic_reference_pages", true, false)
            assert(modal != null)
            if expected[index].is_empty(): assert(not modal.visible)
            else:
                assert(modal.visible and modal.find_child(expected[index], true, false) != null, "page did not open")
                var dialog = modal.find_child(expected[index], true, false)
                var last_background = -1
                for control in dialog.get_children():
                    if control.is_class("EawrUiFrame"): last_background = control.get_index()
                for control in dialog.get_children():
                    if control.is_class("EawrUiLabel") or control is BaseButton:
                        assert(control.get_index() > last_background, "dialog background covers text control")
                if index == 2 or index == 3:
                    var populated = 0
                    for icon in dialog.find_children("IDC_TECHTREE_*", "TextureRect", true, false):
                        if icon.is_visible_in_tree() and icon.texture != null:
                            assert(not icon.tooltip_text.is_empty(), "tree icon has no encyclopedia hover")
                            populated += 1
                    assert(populated > 0, "authored faction tree has no unit icons")
                    print("GREF populated tree ", expected[index], ": ", populated, " icons")
                    var arrows = {}
                    for arrow in dialog.find_children("IDC_*ARROW*", "TextureRect", true, false):
                        if arrow.is_visible_in_tree() and arrow.texture != null:
                            assert(arrow.stretch_mode == TextureRect.STRETCH_SCALE, "tree arrow not stretched to its rect")
                            var family = String(arrow.name).rstrip("0123456789")
                            arrows[family] = arrows.get(family, 0) + 1
                    assert(arrows.get("IDC_RIGHT_ARROW", 0) > 0, "tree header arrow absent")
                    if index == 2: assert(arrows.get("IDC_RRIGHT_ARROW", 0) > 0 and arrows.get("IDC_DOWN_ARROW", 0) > 0, "tree layout arrows absent")
                    print("GREF tree arrows ", expected[index], ": ", arrows)
                var previous_right = -1.0
                for tab in ["IDC_DROID_LOG_RADIO", "IDC_DATA_CHART_RADIO", "IDC_TECH_TREE_RADIO", "IDC_SUMMARY_SCREEN_RADIO"]:
                    var button = modal.find_child(tab, true, false)
                    var rect = button.get_global_rect()
                    assert(dialog.get_global_rect().encloses(rect), "reference tab outside panel: " + tab)
                    assert(rect.position.x >= previous_right - 0.01, "reference tabs overlap: " + tab)
                    assert(button.get_minimum_size() == Vector2.ZERO, "tab applies ordinary radio minimum size")
                    assert(button.get_node_or_null("reference_tab_caption") == null, "tab bypasses shared text widget")
                    assert(button.get_theme_font("Global_Default").get_font_name() == "Empire At War Bold", "tab lost authored command font")
                    previous_right = rect.end.x
                    print("GREF tab ", tab, " rect=", rect, " font=", button.get_theme_font("Global_Default").get_font_name())
                root.get_texture().get_image().save_png(output + "/page-" + str(index) + ".png")
    if frame == 350:
        var event = InputEventKey.new()
        event.keycode = KEY_ESCAPE
        event.pressed = true
        Input.parse_input_event(event)
    if frame == 360:
        assert(not root.find_child("galactic_reference_pages", true, false).visible, "Escape did not dismiss page")
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        report = directory / "report.json"
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "3", "--eawr-galactic-hud-fixture", "known",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-font-cache", str(cache),
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Assertion failed", result.stdout + result.stderr)
        self.assertEqual(len(list(directory.glob("page-*.png"))), 8)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["live_session"]["submitted_orders"], 1, "reference actions issued simulation commands")


if __name__ == "__main__":
    unittest.main()
