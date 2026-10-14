"""WGUI-166 / GHUDL-17: hovering a fleet stack icon opens the shared encyclopedia popup."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
except ImportError:
    import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticFleetStackPopupRuntime(unittest.TestCase):
    def hover(self, fixture, badge):
        """Hovers the named overview stack icon past the encyclopedia delay; returns the report."""
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-stack-popup-"))
        report = directory / "report.json"
        driver = directory / "input.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var elapsed = 0.0
var capture_path = ''' + json.dumps((directory / "native.png").as_posix()) + '''
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func key(code):
    for pressed in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = pressed
        Input.parse_input_event(event)
func _process(delta):
    elapsed += delta
    if elapsed < 1.0: return false
    frame += 1
    # GHUDL-16: the fixture's planet starts left of the overview frame; native
    # previous/next navigation centres it before the icon is pointed at.
    if frame == 10: key(KEY_PAGEUP)
    if frame == 12: key(KEY_PAGEDOWN)
    if frame >= 70 and frame <= 230 and frame % 20 == 10:
        var target = root.find_child(''' + json.dumps(badge) + ''', true, false)
        assert(target != null and target.is_visible_in_tree())
        var motion = InputEventMouseMotion.new()
        motion.position = target.get_global_rect().get_center()
        motion.global_position = motion.position
        Input.parse_input_event(motion)
    if frame == 330:
        root.get_texture().get_image().save_png(capture_path)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        return data

    @staticmethod
    def hover_facts(data):
        popup = data["fleet_hud"]["stack_popup"]
        return {"key": popup["key"], "admitted": popup.get("admitted"), "pointer": popup.get("pointer"),
                "icons": [(row["icon"], row["rect"]) for row in data["fleet_hud"]["presence"] if row["planet"] == 101]}

    @staticmethod
    def presence(data, icon):
        return next(row for row in data["fleet_hud"]["presence"] if row["planet"] == 101 and row["icon"] == icon)

    def test_known_contents_list_each_type_with_its_count(self):
        # Mixed planet: the right icon merges the local slot 0 and 2 fleets.
        data = self.hover("fleet-contents", "fleet_presence_101_2")
        popup = data["fleet_hud"]["stack_popup"]
        self.assertNotEqual(popup["key"], 0, self.hover_facts(data))
        shown = popup["popup"]["tooltip"]
        self.assertIsNotNone(shown, self.hover_facts(data))
        self.assertTrue(shown["external"])
        self.assertGreater(shown["alpha"], 0)
        self.assertTrue(shown["text"])
        self.assertNotIn("TEXT_", shown["text"])  # the side's localized fleet name
        self.assertTrue(shown["icon_back"].lower().startswith("fleet_"))
        expected = [(row["type"], row["count"]) for row in self.presence(data, 2)["contents"]]
        self.assertEqual(expected, [(1000, 2), (1003, 1)])
        cells = shown["cells"]
        self.assertEqual([cell["count"] for cell in cells], [2, 1])
        self.assertTrue(all(cell["icon"] for cell in cells))
        self.assertLess(shown["population"], 0)  # usage, drawn in the usage colour

    def test_count_only_enemy_cells_name_no_type(self):
        data = self.hover("fleet-count", "fleet_presence_101_0")
        contents = self.presence(data, 0)["contents"]
        shown = data["fleet_hud"]["stack_popup"]["popup"]["tooltip"]
        self.assertIsNotNone(shown, self.hover_facts(data))
        cells = shown["cells"]
        self.assertEqual(len(cells), len(contents))
        for cell, row in zip(cells, contents):
            self.assertEqual(cell["count"], row["count"])
            self.assertEqual(bool(cell["icon"]), row["type"] is not None)
        self.assertTrue(any(row["type"] is None for row in contents))
        self.assertEqual(shown["population"], 0)  # no population from unknown types

    def test_centre_icon_opens_and_presence_only_icon_does_not(self):
        data = self.hover("fleet-contents", "fleet_presence_101_1")
        self.assertIsNotNone(data["fleet_hud"]["stack_popup"]["popup"]["tooltip"], self.hover_facts(data))
        # A presence-only enemy has nothing to list: its icon opens nothing. The
        # plain fixture has no attached superweapon, which would grant counts (WGUI-17).
        data = self.hover("fleet-presence-plain", "fleet_presence_101_0")
        self.assertEqual(self.presence(data, 0)["contents"], [])
        self.assertIsNone(data["fleet_hud"]["stack_popup"]["popup"]["tooltip"], self.hover_facts(data))


if __name__ == "__main__":
    unittest.main()
