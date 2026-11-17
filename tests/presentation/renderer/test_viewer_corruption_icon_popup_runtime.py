"""WGCI-1/2: hovering a planet's per-type corruption icon opens its encyclopedia popup (lit capture)."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_planet_hud_runtime as support

DRIVER = '''extends SceneTree
var elapsed = 0.0
var hovered = false
func _initialize():
    var instance = load("res://main.tscn").instantiate()
    root.add_child(instance)
    current_scene = instance
func per_type_icon():
    # The per-type icon has no plain tooltip; the information icon carries one.
    for node in root.find_children("planet_corruption_icon_*", "TextureRect", true, false):
        if not node.is_visible_in_tree() or node.tooltip_text != "":
            continue
        var rect = node.get_global_rect()
        if rect.position.x > 20 and rect.end.x < 1260 and rect.position.y > 40 and rect.end.y < 560:
            return node
    return null
func _process(delta):
    elapsed += delta
    if not hovered and elapsed > 3.0:
        var icon = per_type_icon()
        if icon != null:
            var motion = InputEventMouseMotion.new()
            motion.position = icon.get_global_rect().get_center()
            motion.global_position = motion.position
            Input.parse_input_event(motion)
            hovered = true
    if elapsed > 9.0:
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class CorruptionIconPopupRuntime(unittest.TestCase):
    def run_viewer(self, name, *extra):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-corruption-icon-"))
        report, capture, driver = [directory / file for file in ("report.json", name + ".png", "driver.gd")]
        driver.write_text(DRIVER % json.dumps(capture.as_posix()), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", "corruption-icons",
            "--eawr-game-root", support.GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-lighting", "sh", "--eawr-environment", "map",
            "--eawr-shadows", "on", "--eawr-report", str(report), *extra], cwd=support.ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        self.assertTrue(capture.is_file())
        return json.loads(report.read_text(encoding="utf-8")), capture

    def test_per_type_icon_popup_names_the_type_and_its_mission(self):
        data, capture = self.run_viewer("per_type_popup", "--eawr-galactic-filters", "4")
        popup = data["encyclopedia"]
        self.assertTrue(popup["visible"], popup)
        self.assertTrue(popup["planet_corruption"] and popup["planet_ability"], popup)
        self.assertGreater(popup["alpha"], 0)
        self.assertTrue(popup["title"], "the popup names the corruption type")
        self.assertTrue(popup["body"], "the popup carries the mission blurb")
        self.assertRegex(popup["icon"], r"^i_button_corruption_\w+_b\.tga$")
        self.assertIsNone(popup["price"])
        # Bribery, Piracy or Corrupt Militia: the fixture publishes those three types.
        self.assertTrue(any(word in popup["title"] for word in ("Bribery", "Piracy", "Militia")), popup["title"])
        self.assertTrue(capture.is_file())

    def test_no_popup_while_the_corruption_filter_is_off(self):
        data, _ = self.run_viewer("filter_off")
        # The information icon stays (it is not gated by the filter) but it has no popup; no per-type icon is drawn to hover.
        self.assertFalse(data["encyclopedia"]["planet_corruption"], data["encyclopedia"])
        self.assertFalse(data["encyclopedia"]["visible"], data["encyclopedia"])


if __name__ == "__main__":
    unittest.main()
