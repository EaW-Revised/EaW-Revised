"""WGCH-20..30/96: the LIVE hero-action HUD runs sabotage and neutralization through the shared ability host."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support

ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT

LIVE_DRIVER = '''extends SceneTree
var frame = 0
var action = %(action)s
var seen = 0
var clicked_frame = 0
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(name, down):
    var target = root.find_child(name, true, false)
    assert(target != null and target.is_visible_in_tree())
    var rect = target.get_global_rect()
    var point = rect.position + rect.size / 2.0
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
func _process(_delta):
    frame += 1
    var modal = root.find_child("hero_action_modal", true, false)
    if clicked_frame == 0 and modal != null and modal.is_visible_in_tree():
        seen += 1
        # Let the seat command publish and the menu settle for a few frames before acting.
        if seen == 20:
            clicked_frame = frame
            click(action, true)
    if clicked_frame != 0 and frame == clicked_frame + 1:
        click(action, false)
    if (clicked_frame != 0 and frame == clicked_frame + 150) or frame == 1800:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticLiveHeroActionsRuntime(unittest.TestCase):
    """The host owns each menu (no copied snapshot); the HUD queues its typed capability."""

    def run_live(self, fixture, action):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hero-live-"))
        driver, report = directory / "input.gd", directory / "report.json"
        driver.write_text(LIVE_DRIVER % {"action": json.dumps(action)}, encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "1",
            "--eawr-galactic-hud-fixture", fixture, "--eawr-game-root", GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report)], cwd=ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        menu = data["hero_action_hud"]
        self.assertEqual(menu["source"], "campaign-snapshot")
        # The host settles the preparation, so the published facts are gone by the report; the HUD's own counters
        # and the executed economy are the evidence.
        self.assertFalse(menu["active"], "the modal leaves the screen after acting")
        self.assertTrue(data["paused"], "modal input leaked to pause")
        return data

    def test_live_saboteur_destroys_first_structure_via_hud(self):
        data = self.run_live("sabotage-live", "h_0000")
        menu = data["hero_action_hud"]
        self.assertEqual(menu["choices_accepted"], 1)
        self.assertEqual(menu["accepted_index"], 0)
        self.assertEqual(menu["closes"], 0)
        # The synthetic account is 12422.5 and the first (ground) structure costs 100 with no prior history.
        self.assertEqual(data["command_bar"]["account"], "12322")
        self.assertEqual(data["live_session"]["submitted_orders"], 2)  # pause + the queued typed execution

    def test_live_sabotage_close_retracts_and_pays_nothing(self):
        data = self.run_live("sabotage-live", "h_close")
        menu = data["hero_action_hud"]
        self.assertEqual(menu["choices_accepted"], 0)
        self.assertEqual(menu["closes"], 1)
        self.assertEqual(data["command_bar"]["account"], "12422")

    def test_live_neutralization_removes_hostile_hero_via_hud(self):
        data = self.run_live("neutralization-live", "h_0000")
        menu = data["hero_action_hud"]
        self.assertEqual(menu["choices_accepted"], 1)
        self.assertEqual(menu["accepted_index"], 0)
        # The victim's authored cost is 20 with no curves, so the host debits 20 from 12422.5.
        self.assertEqual(data["command_bar"]["account"], "12402")
        self.assertEqual(data["live_session"]["submitted_orders"], 2)


if __name__ == "__main__":
    unittest.main()
