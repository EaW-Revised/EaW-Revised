"""WGUI-71/87, GFC-28, WGP-122..125 (legacy #7265): a live solo stock galaxy
(the synthetic galaxy is co-op, so the single-player gate refuses hints there) shows the first droid hint, archives it, and the
Droid Log page lists it with the portrait and a working hints checkbox."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticAdvisorHintsRuntime(unittest.TestCase):
    def test_first_hint_reaches_the_droid_log(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-advisor-hints-"))
        cache = directory / "fonts"
        extracted = subprocess.run([sys.executable, str(support.ROOT / "tools/fonts/extract_eaw_fonts.py"),
            "--game-root", support.GAME_ROOT, "--out", str(cache), "--allow-unknown-build", "--json"],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(extracted.returncode, 0, extracted.stdout + extracted.stderr)
        observation = directory / "observation.json"
        driver = directory / "driver.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var hint_frame = -1
var hint_text = ""
var phase = 0
var result = {}
var output = OUTPUT
var started = 0
func _initialize():
    started = Time.get_ticks_msec()
    call_deferred("start")
func start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(name, down):
    var modal = root.find_child("galactic_reference_pages", true, false)
    var target = modal.find_child(name, true, false) if modal != null and modal.visible else root.find_child(name, true, false)
    assert(target != null and target.is_visible_in_tree(), "control absent: " + name)
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
func finish():
    var file = FileAccess.open(output + "/observation.json", FileAccess.WRITE)
    file.store_string(JSON.stringify(result))
    file.close()
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
func _process(_delta):
    frame += 1
    if phase == 0:
        # GFC-28: the first hint is due at galactic frame 90; the advisor line shows it in the hint green.
        var label = root.find_child("GalacticApproachAdvisor", true, false)
        if label != null and label.visible and not label.text.is_empty():
            hint_text = label.text
            hint_frame = frame
            var colour = label.get_theme_color("font_color")
            result["hint_text"] = hint_text
            result["hint_colour"] = [roundi(colour.r * 255), roundi(colour.g * 255), roundi(colour.b * 255)]
            root.get_texture().get_image().save_png(output + "/hint.png")
            phase = 1
        elif Time.get_ticks_msec() - started > 180000:
            result["hint_text"] = ""
            finish()
        return false
    if frame == hint_frame + 20: click("b_droid_help", true)
    if frame == hint_frame + 21: click("b_droid_help", false)
    if frame == hint_frame + 40:
        var modal = root.find_child("galactic_reference_pages", true, false)
        var dialog = modal.find_child("IDD_DROID_LOG", true, false)
        assert(dialog != null, "Droid Log did not open")
        var rows = []
        var list = dialog.find_child("IDC_DROID_LIST", true, false)
        if list != null and list.is_visible_in_tree():
            for index in range(list.item_count):
                var c = list.get_item_custom_fg_color(index)
                rows.append({"text": list.get_item_text(index), "colour": [roundi(c.r * 255), roundi(c.g * 255), roundi(c.b * 255)],
                    "selectable": list.is_item_selectable(index)})
        result["rows"] = rows
        var picture = dialog.find_child("IDC_DROID_PICTURE_", true, false)
        result["portrait"] = picture != null and picture.is_visible_in_tree() and picture.texture != null
        var check = dialog.find_child("IDC_HINTS_CHECK", true, false)
        result["check_enabled"] = check != null and not check.disabled
        result["check_pressed"] = check != null and check.button_pressed
        var legend = 0
        for icon in dialog.find_children("IDC_PA_ICON*", "TextureRect", true, false):
            if icon.is_visible_in_tree() and icon.texture != null: legend += 1
        result["legend_icons"] = legend
        root.get_texture().get_image().save_png(output + "/droid-log.png")
    if frame == hint_frame + 50: click("IDC_HINTS_CHECK", true)
    if frame == hint_frame + 51: click("IDC_HINTS_CHECK", false)
    if frame == hint_frame + 70:
        var modal = root.find_child("galactic_reference_pages", true, false)
        var check = modal.find_child("IDC_HINTS_CHECK", true, false)
        result["check_after_click"] = check != null and check.button_pressed
        finish()
    return false
'''.replace("OUTPUT", json.dumps(directory.as_posix())), encoding="utf-8")
        report = directory / "report.json"
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "Sandbox_Equal_Footing_Empire",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-font-cache", str(cache),
            "--eawr-galactic-paused", "no", "--eawr-galactic-dismiss-story", "yes", "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=420)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Assertion failed", result.stdout + result.stderr)
        data = json.loads(observation.read_text(encoding="utf-8"))
        print("advisor hints observation:", json.dumps(data))
        self.assertTrue(data["hint_text"], "WGUI-71: no droid hint appeared")
        self.assertEqual(data["hint_colour"], [0, 193, 91], "WGUI-71 hint colour")
        rows = data["rows"]
        self.assertGreaterEqual(len(rows), 3, "WGP-122: the archive holds date, message and separator")
        self.assertEqual(rows[0]["colour"], [255, 0, 0], "WGUI-87 Droid_Date_Color")
        self.assertTrue(rows[0]["text"].strip(), "WGP-122 cycle line")
        message = " ".join(row["text"] for row in rows[1:-1])
        self.assertEqual(message, data["hint_text"], "WGP-123: the wrapped message rows are the hint")
        self.assertTrue(all(row["colour"] == [0, 193, 91] for row in rows[1:-1]), "message rows keep the hint colour")
        self.assertEqual(rows[-1]["colour"], [128, 128, 128], "WGUI-87 Droid_Seperator_Color")
        self.assertFalse(any(row["selectable"] for row in rows), "WGP-123 entries are not selectable")
        self.assertTrue(data["portrait"], "WGP-124 faction helper portrait")
        self.assertGreater(data["legend_icons"], 0, "GREF-19 ability legend")
        self.assertTrue(data["check_enabled"] and data["check_pressed"], "WGP-125 checkbox starts as the OR of the flags")
        self.assertFalse(data["check_after_click"], "WGP-125 the checkbox turns hints off")


if __name__ == "__main__":
    unittest.main()
