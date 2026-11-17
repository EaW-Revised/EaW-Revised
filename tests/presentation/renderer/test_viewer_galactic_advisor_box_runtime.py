"""#7196/#7444: the advisor popup draws every line (enemy fleet approaching, control changed) inside its authored back,
built like the encyclopedia popup (CARD-19) at the hint bone, in the authored advisor text style."""
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
            var glow = root.find_child("GalacticAdvisorGlow", true, false)
            var connector = root.find_child("GalacticAdvisorConnector", true, false)
            var ink = label.get_theme_color("font_color")
            var entry = {"text": label.text, "label": rect_of(label), "back": rect_of(back) if back != null else [],
                "back_visible": back != null and back.visible, "frame": rect_of(frame_node) if frame_node != null else [],
                "frame_visible": frame_node != null and frame_node.visible, "frame_frame": frame,
                "glow": rect_of(glow) if glow != null else [], "glow_visible": glow != null and glow.visible,
                "connector_visible": connector != null and connector.visible,
                "ink": [ink.r, ink.g, ink.b], "outline": label.get_theme_constant("outline_size")}
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
            # #7444 / CARD-19: the edge and the additive glow surround the back; the connector is drawn with it.
            self.assertTrue(entry["glow_visible"] and entry["connector_visible"], f"glow and connector drawn ({directory})")
            for outer, name in ((entry["frame"], "edge"), (entry["glow"], "glow")):
                ox, oy, ow, oh = outer
                self.assertTrue(ox < bx and oy < by and ox + ow > bx + bw and oy + oh > by + bh, f"the {name} surrounds the back")
            gx, gy, gw, gh = entry["glow"]
            fx, fy, fw, fh = entry["frame"]
            self.assertTrue(gx < fx and gx + gw > fx + fw, "the glow is larger than the edge")
            # #7444: every line takes the authored advisor text style's ink (stock text_galactic_help 192,192,192) and its
            # outline, whatever its producer's code colour (red control change, green approach).
            self.assertEqual([round(value * 255) for value in entry["ink"]], [192, 192, 192], "the authored advisor ink")
            self.assertGreater(entry["outline"], 0, "the authored text outline")
        # The same box for both lines: one bottom-left corner and one width; it grows upward with the text.
        self.assertEqual(first["back"][0], second["back"][0], f"both lines draw in the same box ({directory})")
        self.assertEqual(first["back"][2], second["back"][2])
        self.assertAlmostEqual(first["back"][1] + first["back"][3], second["back"][1] + second["back"][3], delta=0.5)
        self.assertEqual(first["label"][0], second["label"][0], "both lines start at the same x")
        # GHUD-C04 / CARD-19: the original draws the popup's bottom-left at (60,430) at 1280x720 (the stock hint bone).
        self.assertAlmostEqual(first["back"][0], 60.0, delta=2.0, msg="the popup's left edge at the hint bone")
        self.assertAlmostEqual(first["back"][1] + first["back"][3], 430.0, delta=2.0, msg="its bottom at the hint bone")


if __name__ == "__main__":
    unittest.main()
