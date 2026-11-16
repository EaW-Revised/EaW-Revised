"""#7196: the advisor popup draws every line (enemy fleet approaching, control changed) inside its authored back."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticAdvisorBoxRuntime(unittest.TestCase):
    def test_both_advisor_lines_share_the_authored_back(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-advisor-box-"))
        cache = directory / "fonts"
        extracted = subprocess.run([sys.executable, str(support.ROOT / "tools/fonts/extract_eaw_fonts.py"),
            "--game-root", support.GAME_ROOT, "--out", str(cache), "--allow-unknown-build", "--json"],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(extracted.returncode, 0, extracted.stdout + extracted.stderr)
        driver = directory / "driver.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var output = ''' + json.dumps(directory.as_posix()) + '''
var seen = []
func _initialize():
    call_deferred("start")
func start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func rect_of(node):
    var rect = node.get_global_rect()
    return [rect.position.x, rect.position.y, rect.size.x, rect.size.y]
func _process(_delta):
    frame += 1
    var label = root.find_child("GalacticApproachAdvisor", true, false)
    var back = root.find_child("GalacticAdvisorBack", true, false)
    var frame_node = root.find_child("GalacticAdvisorFrame", true, false)
    if label != null and label.visible and label.modulate.a > 0.95:
        var known = false
        for entry in seen:
            if entry["text"] == label.text: known = true
        if not known:
            var entry = {"text": label.text, "label": rect_of(label), "back": rect_of(back) if back != null else [],
                "back_visible": back != null and back.visible, "frame": rect_of(frame_node) if frame_node != null else [],
                "frame_visible": frame_node != null and frame_node.visible, "frame_frame": frame}
            seen.append(entry)
            var image = root.get_texture().get_image()
            var box = Rect2i(Vector2i(int(entry["back"][0]) - 16, int(entry["back"][1]) - 16),
                Vector2i(int(entry["back"][2]) + 32, int(entry["back"][3]) + 32)) if back != null else Rect2i(0, 0, 640, 360)
            image.get_region(box.intersection(Rect2i(Vector2i.ZERO, image.get_size()))).save_png(
                output + "/advisor-" + str(seen.size()) + ".png")
    if seen.size() >= 2 or frame == 6000:
        var file = FileAccess.open(output + "/advisor.json", FileAccess.WRITE)
        file.store_string(JSON.stringify(seen))
        file.close()
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
            "--eawr-galactic-test-advisor", "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Assertion failed", result.stdout + result.stderr)
        entries = json.loads((directory / "advisor.json").read_text(encoding="utf-8"))
        self.assertEqual(len(entries), 2, f"both advisor lines should show (artifacts in {directory})")
        texts = " | ".join(entry["text"].lower() for entry in entries)
        self.assertIn("control changed", texts)
        self.assertIn("incoming", texts)
        first, second = entries
        for entry in entries:
            self.assertTrue(entry["back_visible"] and entry["frame_visible"], f"the authored back is drawn ({directory})")
            bx, by, bw, bh = entry["back"]
            lx, ly, lw, lh = entry["label"]
            self.assertGreaterEqual(lx, bx - 0.5, "the text starts inside the back")
            self.assertGreaterEqual(ly, by - 0.5)
            self.assertLessEqual(lx + lw, bx + bw + 0.5, "the text ends inside the back")
            self.assertLessEqual(ly + lh, by + bh + 0.5)
            self.assertEqual(entry["frame"], entry["back"], "the frame covers the back")
        # The same box for both lines: one origin and one width (the height follows the text).
        self.assertEqual(first["back"][:3], second["back"][:3], f"both lines draw in the same box ({directory})")
        self.assertEqual(first["label"][:2], second["label"][:2], "both lines start at the same point")


if __name__ == "__main__":
    unittest.main()
