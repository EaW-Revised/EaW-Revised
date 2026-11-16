"""#6291: the galactic cinematic tour. The camera button and the Space key start it (the battle-camera key acts on its
release); the letterbox bands slide in and the GUI is hidden; the game keeps running (not paused); any mouse press ends
it and the GUI returns. Native input on the stock campaign."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
CAMPAIGN = "Sandbox_Equal_Footing_Underworld"
KEY_SPACE = 32  # Godot's KEY_SPACE
START = 14.0


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the GPU route and installed FoC data")
class GalacticTourRuntime(unittest.TestCase):
    @staticmethod
    def driver(steps, close_at):
        """Native input at the given seconds: {"at", "node"} a left click on a node's centre, {"at", "x", "y"} a left click
        at a point, {"at", "key"} a key press and release. The run ends at `close_at`."""
        return '''extends SceneTree
var elapsed = 0.0
var steps = JSON.parse_string('%s')
var done = []
func _initialize():
    for step in steps:
        done.append(false)
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    for index in range(steps.size()):
        var step = steps[index]
        if done[index] or elapsed < step["at"]:
            continue
        done[index] = true
        if step.has("key"):
            for pressed in [true, false]:
                var key = InputEventKey.new()
                key.keycode = int(step["key"])
                key.physical_keycode = int(step["key"])
                key.pressed = pressed
                Input.parse_input_event(key)
            continue
        var at = Vector2(step.get("x", 0), step.get("y", 0))
        if step.has("node"):
            var node = root.find_child(step["node"], true, false)
            assert(node != null, "%%s is not in the tree" %% step["node"])
            at = node.get_global_rect().get_center()
        var motion = InputEventMouseMotion.new()
        motion.position = at
        motion.global_position = at
        Input.parse_input_event(motion)
        for pressed in [true, false]:
            var event = InputEventMouseButton.new()
            event.position = at
            event.global_position = at
            event.button_index = MOUSE_BUTTON_LEFT
            event.pressed = pressed
            Input.parse_input_event(event)
    if elapsed > %s:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(steps), close_at)

    def view(self, steps, close_at, paused="no", space_pauses=False):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-tour-"))
        report = directory / "report.json"
        script = directory / "driver.gd"
        script.write_text(self.driver(steps, close_at), encoding="utf-8")
        settings = directory / "settings.txt"
        settings.write_text("eawr-settings 1\ngameplay.space_pauses_galactic=%d\n" % (1 if space_pauses else 0), encoding="utf-8")
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"),
                   "--script", str(script), "--", "--eawr-galactic", CAMPAIGN, "--eawr-game-root", GAME_ROOT,
                   "--eawr-report", str(report), "--eawr-galactic-paused", paused, "--eawr-galactic-dismiss-story", "yes",
                   "--eawr-settings-file", str(settings)]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (directory / "viewer.log").write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        self.assertTrue(report.is_file(), log)
        self.assertNotIn("ERROR:", log)
        return json.loads(report.read_text(encoding="utf-8"))

    def assert_touring(self, data, label):
        tour = data["cinematic_tour"]
        self.assertTrue(tour["active"], label + ": the tour plays")
        self.assertEqual(tour["started"], 1, label)
        self.assertGreater(tour["planets"], 2, label + ": it visits the planets")
        self.assertGreater(tour["seconds"], 1.0, label + ": the tour's clock runs")
        self.assertEqual(tour["letterbox"], 1.0, label + ": the bands are in")
        self.assertFalse(tour["hud_visible"], label + ": the GUI is hidden")
        self.assertFalse(data["paused"], label + ": the game keeps running")
        self.assertEqual(data["live_session"]["pause_depth"], 0, label)
        self.assertGreater(data["live_session"]["galactic_frame"], 0, label + ": the days advance")

    def test_the_camera_button_starts_the_tour(self):
        data = self.view([{"at": START, "node": "b_camera_g"}], START + 7.0)
        self.assert_touring(data, "button")

    def test_space_starts_the_tour_and_does_not_pause(self):
        data = self.view([{"at": START, "key": KEY_SPACE}], START + 7.0)
        self.assert_touring(data, "space")
        # A second Space while it plays starts a new tour instead of ending it.
        again = self.view([{"at": START, "key": KEY_SPACE}, {"at": START + 3.0, "key": KEY_SPACE}], START + 8.0)
        self.assertTrue(again["cinematic_tour"]["active"])
        self.assertEqual(again["cinematic_tour"]["restarts"], 1)

    def test_a_mouse_press_ends_the_tour_and_the_gui_returns(self):
        data = self.view([{"at": START, "node": "b_camera_g"}, {"at": START + 4.0, "x": 640, "y": 360}], START + 9.0)
        tour = data["cinematic_tour"]
        self.assertFalse(tour["active"], "the click ended the tour")
        self.assertEqual(tour["letterbox"], 0.0, "the bands slid out")
        self.assertTrue(tour["hud_visible"], "the GUI is back")
        self.assertFalse(data["paused"])

    def test_the_camera_keys_do_nothing_during_a_tour(self):
        # Home, Page Up and Page Down would change the selection and the camera: while a tour plays they are consumed, so the
        # camera and selection the player returns to are those of a tour with no keys at all.
        KEY_HOME, KEY_PAGEUP, KEY_PAGEDOWN = 4194317, 4194323, 4194324  # Godot 4 KEY_HOME, KEY_PAGEUP, KEY_PAGEDOWN
        click = {"at": START + 6.0, "x": 640, "y": 360}
        plain = self.view([{"at": START, "key": KEY_SPACE}, click], START + 11.0)
        keyed = self.view([{"at": START, "key": KEY_SPACE}, {"at": START + 2.0, "key": KEY_PAGEDOWN},
                           {"at": START + 3.0, "key": KEY_PAGEUP}, {"at": START + 4.0, "key": KEY_HOME}, click], START + 11.0)
        for data in (plain, keyed):
            self.assertFalse(data["cinematic_tour"]["active"])
            self.assertEqual(data["cinematic_tour"]["started"], 1)
        self.assertEqual(keyed["camera"], plain["camera"], "the camera keys moved the camera under the tour")
        self.assertEqual(keyed["map_input"], plain["map_input"], "the camera keys changed the selection under the tour")

    def test_the_tour_is_refused_while_paused_and_the_banner_stays(self):
        data = self.view([{"at": START, "key": KEY_SPACE}, {"at": START + 2.0, "node": "b_camera_g"}], START + 6.0,
                         paused="yes")
        self.assertTrue(data["paused"])
        self.assertEqual(data["cinematic_tour"]["started"], 0, "Space and the camera button start no tour while paused")
        self.assertTrue(data["cinematic_tour"]["hud_visible"], "the GUI (and the pause banner) stay")
        self.assertIsNotNone(data["pause_banner"]["paused_banner"])

    def test_space_pauses_option_on_space_only_pauses_and_the_button_still_tours(self):
        """DEV-13: with "Space pauses the galactic game" on, Space pauses (banner up, Resume Game offered) and never starts the
        tour; a second Space resumes nothing it did not pause (a toggle); the tour stays on the camera button."""
        data = self.view([{"at": START, "key": KEY_SPACE}], START + 4.0, space_pauses=True)
        self.assertTrue(data["paused"], "Space pauses")
        self.assertEqual(data["live_session"]["pause_depth"], 1)
        self.assertEqual(data["cinematic_tour"]["started"], 0, "Space does not start the tour")
        self.assertTrue(data["cinematic_tour"]["hud_visible"], "the GUI stays")
        self.assertIsNotNone(data["pause_banner"]["paused_banner"], "the paused banner shows with the option on")
        toggled = self.view([{"at": START, "key": KEY_SPACE}, {"at": START + 2.0, "key": KEY_SPACE}], START + 5.0,
                            space_pauses=True)
        self.assertFalse(toggled["paused"], "a second Space plays again")
        self.assertEqual(toggled["cinematic_tour"]["started"], 0)
        self.assertIsNone(toggled["pause_banner"]["paused_banner"])
        button = self.view([{"at": START, "node": "b_camera_g"}], START + 7.0, space_pauses=True)
        self.assert_touring(button, "camera button with the option on")

    def test_space_pauses_option_off_the_pause_button_and_banner_still_work(self):
        """The default (off): Space never pauses, and the clock panel's pause button pauses with the banner as before."""
        data = self.view([{"at": START, "node": "b_play_pause"}], START + 3.0)
        self.assertTrue(data["paused"], "the pause button works with the option off")
        self.assertIsNotNone(data["pause_banner"]["paused_banner"])

    def test_the_run_without_input_has_no_tour(self):
        data = self.view([], START + 3.0)
        self.assertFalse(data["cinematic_tour"]["active"])
        self.assertEqual(data["cinematic_tour"]["started"], 0)
        self.assertTrue(data["cinematic_tour"]["hud_visible"])


if __name__ == "__main__":
    unittest.main()
