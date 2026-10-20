"""WGUI-89: stock campaign hero portraits, real input selection and camera recentering."""
import json
import math
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support
ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT

DRIVER = '''extends SceneTree
var elapsed = 0.0
var stage = 0
var wait = 0.0
var directory = %(directory)s
var do_click = %(click)s
var pressed = false
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func key(down):
    var event = InputEventKey.new()
    event.keycode = KEY_D
    event.physical_keycode = KEY_D
    event.pressed = down
    Input.parse_input_event(event)
func click(down):
    var button = root.find_child("galactic_hero_0", true, false)
    assert(button != null and button.is_visible_in_tree() and not button.disabled)
    var point = button.get_global_rect().get_center()
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = point
    event.global_position = point
    Input.parse_input_event(event)
func shot(label):
    root.get_texture().get_image().save_png(directory + "/" + label + ".png")
func _process(delta):
    elapsed += delta
    var button = root.find_child("galactic_hero_0", true, false)
    if stage == 0 and elapsed > 2.0 and button != null and button.is_visible_in_tree():
        shot("bar-before-pan")
        key(true)
        stage = 1
        wait = elapsed
    elif stage == 1 and elapsed - wait > 1.0:
        key(false)
        stage = 2
        wait = elapsed
    elif stage == 2 and elapsed - wait > 0.5:
        shot("panned")
        if do_click: click(true)
        stage = 3
        wait = elapsed
    elif stage == 3:
        if do_click and not pressed:
            click(false)
            pressed = true
        if elapsed - wait > 2.0:
            shot("clicked" if do_click else "pan-only")
            current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
            quit()
    if elapsed > 90.0:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticHeroBarRuntime(unittest.TestCase):
    def test_stock_empire_bar_click_selects_and_recentres_own_hero(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hero-bar-"))

        def launch(name, click):
            run = directory / name
            run.mkdir()
            driver, report = run / "input.gd", run / "report.json"
            driver.write_text(DRIVER % {"directory": json.dumps(run.as_posix()),
                                       "click": "true" if click else "false"}, encoding="utf-8")
            command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                       "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                       "--eawr-galactic", "Sandbox_Equal_Footing_Empire", "--eawr-game-root", GAME_ROOT,
                       "--eawr-galactic-paused", "yes", "--eawr-environment", "map",
                       "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-report", str(report)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
            (run / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertTrue(report.is_file(), result.stdout + result.stderr)
            data = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
            return data

        panned = launch("pan", False)
        clicked = launch("click", True)
        bar = clicked["hero_map_bar"]
        self.assertTrue(bar["visible"])
        self.assertEqual(bar["component"], "g_hero_icon")
        self.assertGreater(len(bar["entries"]), 0, "story-spawned stock named heroes must be present at start")
        self.assertLessEqual(len(bar["entries"]), 6)
        self.assertEqual([row["type"] for row in bar["entries"]], sorted({row["type"] for row in bar["entries"]}))
        entry = bar["entries"][0]
        self.assertTrue(entry["icon"])
        self.assertTrue(entry["tooltip"])
        self.assertGreater(entry["rect"][0], 950)
        self.assertLess(entry["rect"][1], 20)
        self.assertAlmostEqual(entry["rect"][2], 38.4, delta=1)
        self.assertEqual(bar["clicks"], 1)
        self.assertEqual(bar["selected_type"], entry["type"])
        self.assertEqual(clicked["map_input"]["selected"], entry["planet"])
        self.assertTrue(clicked["paused"], "the hero click must not leak through the HUD")
        planet = next(row for row in clicked["planets"] if row["id"] == entry["planet"])
        target = clicked["camera"]["target"]
        self.assertAlmostEqual(target[0], planet["position"][0], delta=0.1)
        self.assertAlmostEqual(target[2], -planet["position"][1], delta=0.1)
        self.assertGreater(math.dist(panned["camera"]["target"], target), 1.0,
                           "real keyboard input first moved the camera away; the icon brought it back")
        self.assertTrue(bar["foreign_named_actors"], "stock campaign must supply a foreign hero negative control")
        displayed = {row["actor"] for row in bar["entries"]}
        for foreign in bar["foreign_named_actors"]:
            self.assertNotEqual(foreign["owner"], bar["local_player"])
            self.assertNotIn(foreign["actor"], displayed)
        self.assertTrue((directory / "click" / "bar-before-pan.png").is_file())
        self.assertTrue((directory / "click" / "clicked.png").is_file())
        print("HERO-BAR-DIRECTORY", directory)


if __name__ == "__main__":
    unittest.main()
