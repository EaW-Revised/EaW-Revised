"""#6759: the galactic pause shows the pause button lit and a "Game Paused" banner with a Resume Game button at the top
centre; Resume Game (like the button) plays again. Native input on the stock campaign."""
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
WIDTH, HEIGHT = 1280, 720


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the GPU route and installed FoC data")
class GalacticPauseBannerRuntime(unittest.TestCase):
    @staticmethod
    def driver(clicks):
        """One native left click on the named node's centre at each (seconds, node name); the run closes 3 s after the last."""
        body = "".join(('''    if elapsed > %s and not done_%d:
        done_%d = true
        for pressed in [true, false]:
            var event = InputEventKey.new()
            event.keycode = KEY_SPACE
            event.physical_keycode = KEY_SPACE
            event.pressed = pressed
            Input.parse_input_event(event)
''' % (at, index, index)) if name == "KEY_SPACE" else ('''    if elapsed > %s and not done_%d:
        done_%d = true
        var node = root.find_child("%s", true, false)
        assert(node != null, "%s is not in the tree")
        var at = node.get_global_rect().get_center()
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
''' % (at, index, index, name, name)) for index, (at, name) in enumerate(clicks))
        flags = "".join("var done_%d = false\n" % index for index in range(len(clicks)))
        close_at = (clicks[-1][0] if clicks else 14.0) + 3.0
        return '''extends SceneTree
var elapsed = 0.0
%s
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
%s
    if elapsed > %s:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (flags, body, close_at)

    def view(self, clicks, campaign=CAMPAIGN, extra=("--eawr-galactic-paused", "no", "--eawr-galactic-dismiss-story", "yes")):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-pause-banner-"))
        report = directory / "report.json"
        script = directory / "driver.gd"
        script.write_text(self.driver(clicks), encoding="utf-8")
        settings = directory / "settings.txt"
        settings.write_text("eawr-settings 1\ngameplay.space_pauses_galactic=1\n", encoding="utf-8")
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "%dx%d" % (WIDTH, HEIGHT), "--path",
                   str(ROOT / "apps/viewer/project"), "--script", str(script), "--",
                   "--eawr-galactic", campaign, "--eawr-game-root", GAME_ROOT, "--eawr-report", str(report),
                   "--eawr-settings-file", str(settings), *extra]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (directory / "viewer.log").write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        self.assertTrue(report.is_file(), log)
        self.assertNotIn("ERROR:", log)
        return json.loads(report.read_text(encoding="utf-8"))

    @staticmethod
    def pause_button(data):
        return next(row for row in data["command_bar"]["controls"] if row["name"] == "b_play_pause")

    def test_pause_lights_the_button_and_shows_the_banner_until_resumed(self):
        paused = self.view([(14.0, "b_play_pause")])
        self.assertEqual(paused["live_session"]["pause_depth"], 1, "the press paused the galactic clock")
        self.assertTrue(self.pause_button(paused)["pressed"], "the pause button is drawn lit while paused")
        banner = paused["pause_banner"]
        self.assertIsNotNone(banner["paused_banner"], "a paused map shows the banner")
        text = banner["paused_banner"]["text"]
        self.assertTrue(text and text != "TEXT_GAME_PAUSED", "the banner text comes from the text database: %r" % text)
        x, y, w, h = banner["paused_banner"]["rect"]
        self.assertAlmostEqual(x + w / 2, WIDTH / 2, delta=WIDTH * 0.02, msg="the banner is centred")
        self.assertLess(y, HEIGHT * 0.15, "the banner hangs at the top of the screen")
        rx, ry, rw, rh = banner["resume_rect"]
        self.assertAlmostEqual(rx + rw / 2, WIDTH / 2, delta=WIDTH * 0.02, msg="Resume Game is centred under it")
        self.assertGreater(ry, y, "Resume Game is under the banner text")
        self.assertFalse(banner["quit_visible"], "the map has no Quit Game button")

        resumed = self.view([(14.0, "b_play_pause"), (17.0, "EawrResumeGame")])
        self.assertEqual(resumed["live_session"]["pause_depth"], 0, "Resume Game plays again")
        self.assertFalse(self.pause_button(resumed)["pressed"], "the button is dim again")
        self.assertIsNone(resumed["pause_banner"]["paused_banner"], "the banner is gone")

        again = self.view([(14.0, "b_play_pause"), (17.0, "b_play_pause")])
        self.assertEqual(again["live_session"]["pause_depth"], 0, "the button toggles back to play")
        self.assertIsNone(again["pause_banner"]["paused_banner"])

    def test_space_shows_pause_banner_without_a_prompt(self):
        paused = self.view([(14.0, "KEY_SPACE")], campaign="Sandbox_Gateways_Rebel", extra=("--eawr-galactic-paused", "no",
            "--eawr-galactic-dismiss-story", "yes", "--eawr-galactic-ai", "off"))
        self.assertEqual(paused["live_session"]["pause_depth"], 1)
        self.assertIsNotNone(paused["pause_banner"]["paused_banner"])

    def test_resume_game_is_refused_where_the_pause_button_is(self):
        """The clock controls are closed in this preview (the pause button is disabled, as in a multiplayer role without
        the clock): the map starts paused, the banner shows, Resume Game is drawn refused and a press on it does nothing,
        exactly like the button."""
        extra = ("--eawr-galactic-paused", "yes", "--eawr-galactic-seat", "1")
        idle = self.view([], campaign="synthetic", extra=extra)
        self.assertFalse(self.pause_button(idle)["enabled"], "this preview has the clock controls closed")
        self.assertIsNotNone(idle["pause_banner"]["paused_banner"], "the paused map shows its banner")
        self.assertFalse(idle["pause_banner"]["resume_enabled"], "Resume Game is refused like the button")
        pressed = self.view([(14.0, "EawrResumeGame")], campaign="synthetic", extra=extra)
        self.assertTrue(pressed["paused"], "a refused Resume Game leaves the game paused")
        self.assertEqual(pressed["live_session"]["pause_depth"], idle["live_session"]["pause_depth"])
        self.assertEqual(pressed["live_session"]["submitted_orders"], idle["live_session"]["submitted_orders"],
                         "a refused Resume Game submits no order")
        self.assertIsNotNone(pressed["pause_banner"]["paused_banner"])


if __name__ == "__main__":
    unittest.main()
