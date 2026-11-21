"""#8023: the pending battle prompt's own Begin / Auto-resolve buttons take real pointer input in a live campaign.

#7798 let the camera (middle/right-button drag, arrow keys, edge scroll) and the planet finder work under the prompt and
refuse every resume. The prompt itself must stay answerable whatever camera input is held when it opens: each case
launches the stock authored campaign (a real session, not the fixture), waits for the real pending prompt, performs the
named camera/keyboard input and then clicks the button with a real left press and release. Space and Enter under the
prompt must not resume the galaxy (#7798): the accepted journal holds no clock order.

After the click the driver waits on the outcome with a bounded condition (Begin: the tactical battle host is ready;
Auto-resolve: the result summary is shown) and prints the measured seconds. The campaign runs with the "prompt-save"
proof mode, which skips the automatic Fight activation so the choice stays with native input: this covers the
native-input path under the save-proof handoff, not the owner's default session. Before the left click the driver
checks that no earlier input (the right-button release on the button) already answered the prompt.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
    from .campaign_space_cases import qualified_campaign_arguments
except ImportError:
    import test_viewer_planet_hud_runtime as support
    from campaign_space_cases import qualified_campaign_arguments

DRIVER = r'''extends SceneTree
var instance
var elapsed = 0.0
var beat = 0.0
var stage = 0
var stage_at = 0.0
var clicked_at = 0.0
var facts = {"stages": [], "ready": false}
var target_point = Vector2.ZERO
var world_point = Vector2(16, 350)
func _initialize():
    instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func control(parent, name):
    return null if parent == null else parent.find_child(name, true, false)
func ready(node):
    return node != null and node.is_visible_in_tree() and not node.is_disabled()
func mouse_button(point, index, pressed):
    var event = InputEventMouseButton.new()
    event.position = point
    event.global_position = point
    event.button_index = index
    event.pressed = pressed
    Input.parse_input_event(event)
func mouse_move(point, relative):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    event.relative = relative
    Input.parse_input_event(event)
func key(code, pressed):
    var event = InputEventKey.new()
    event.keycode = code
    event.physical_keycode = code
    event.pressed = pressed
    Input.parse_input_event(event)
func finish(error):
    facts["error"] = error
    var file = FileAccess.open(@@FACTS@@, FileAccess.WRITE)
    file.store_string(JSON.stringify(facts))
    file.close()
    root.get_texture().get_image().save_png(@@CAPTURE@@)
    instance.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit(0 if error == "" else 2)
func note(name):
    facts["stages"].append(name)
func _process(delta):
    elapsed += delta
    beat += delta
    if elapsed > 240 and stage < 14:
        finish("the pending prompt never became ready")
        return false
    if beat < 0.15:
        return false
    beat = 0.0
    var battle = control(instance, "CampaignBattleDialog")
    var button = control(battle, @@BUTTON@@)
    if stage == 0:
        if not ready(button) or not ready(control(battle, "choice_button_left")): return false
        target_point = button.get_global_rect().get_center()
        facts["ready"] = true
        facts["button_rect"] = [button.get_global_rect().position.x, button.get_global_rect().position.y,
            button.get_global_rect().size.x, button.get_global_rect().size.y]
        stage = 1
        stage_at = elapsed
        return false
    if elapsed - stage_at < 0.4:
        return false
    stage_at = elapsed
    # Camera / keyboard input the prompt must tolerate, one action per beat.
    if "key" in @@STRESS@@:
        if stage == 1:
            key(KEY_RIGHT, true); note("arrow held"); stage = 2; return false
    elif stage == 1:
        stage = 2
    if "middle" in @@STRESS@@:
        if stage == 2:
            mouse_button(world_point, MOUSE_BUTTON_MIDDLE, true); note("middle press"); stage = 3; return false
        if stage == 3:
            mouse_move(world_point + Vector2(0, 40), Vector2(0, 40)); note("middle drag"); stage = 4; return false
        if stage == 4:
            mouse_move(world_point + Vector2(0, 80), Vector2(0, 40)); stage = 5; return false
        if stage == 5:
            mouse_button(world_point + Vector2(0, 80), MOUSE_BUTTON_MIDDLE, false); note("middle release"); stage = 6; return false
    elif stage == 2:
        stage = 6
    if "right" in @@STRESS@@:
        # A right-button drag whose release lands on the prompt's button, the one place the prompt takes the pointer.
        if stage == 6:
            mouse_button(world_point, MOUSE_BUTTON_RIGHT, true); note("right press"); stage = 7; return false
        if stage == 7:
            mouse_move(world_point + Vector2(0, 40), Vector2(0, 40)); note("right drag"); stage = 8; return false
        if stage == 8:
            mouse_move(target_point, target_point - world_point - Vector2(0, 40)); stage = 9; return false
        if stage == 9:
            mouse_button(target_point, MOUSE_BUTTON_RIGHT, false); note("right release on the button"); stage = 10; return false
    elif stage == 6:
        stage = 10
    if "world" in @@STRESS@@:
        if stage == 10:
            mouse_button(Vector2(640, 300), MOUSE_BUTTON_LEFT, true)
            mouse_button(Vector2(640, 300), MOUSE_BUTTON_LEFT, false)
            note("world click"); stage = 11; return false
    elif stage == 10:
        stage = 11
    if "space" in @@STRESS@@:
        if stage == 11:
            key(KEY_SPACE, true); key(KEY_SPACE, false); key(KEY_ENTER, true); key(KEY_ENTER, false)
            note("space and enter"); stage = 12; return false
    elif stage == 11:
        stage = 12
    if stage == 12:
        # An activation disables every button at once (BattleDialog::activate): a button that is no longer enabled here
        # means an earlier input (the right-button release) already answered the prompt, so the left click below
        # would not be the accepted choice.
        facts["early_activation"] = not ready(button)
        mouse_move(target_point, Vector2.ZERO)
        stage = 13
        return false
    if stage == 13:
        mouse_button(target_point, MOUSE_BUTTON_LEFT, true)
        mouse_button(target_point, MOUSE_BUTTON_LEFT, false)
        note("click " + @@BUTTON@@)
        clicked_at = elapsed
        stage = 14
        return false
    if stage == 14:
        if "key" in @@STRESS@@: key(KEY_RIGHT, false)
        stage = 15
        return false
    if stage == 15:
        # Wait on the outcome, not on a clock: Begin reaches the tactical handoff (the ready battle host), Auto-resolve
        # reaches the result summary. The measured seconds go into the facts; only a generous bound fails the run.
        var reached = false
        if @@BUTTON@@ == "choice_button_left":
            reached = root.find_child("CampaignSpaceBattleReady", true, false) != null
        else:
            var done = control(instance, "IDC_CONTINUE_BUTTON")
            reached = done != null and done.is_visible_in_tree()
        facts["seconds_to_outcome"] = elapsed - clicked_at
        if reached:
            note("outcome reached after %.1f s" % (elapsed - clicked_at))
            finish("")
        elif elapsed - clicked_at > 180.0:
            finish("the answered prompt did not reach its outcome in 180 s")
    return false
'''


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class PendingPromptInputRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.font_cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-prompt-input-fonts-"))
        extracted = subprocess.run([
            sys.executable, str(support.ROOT / "tools/fonts/extract_eaw_fonts.py"),
            "--game-root", support.GAME_ROOT, "--out", str(cls.font_cache),
            "--allow-unknown-build", "--json"], capture_output=True, text=True, timeout=120)
        if extracted.returncode != 0:
            raise RuntimeError(extracted.stdout + extracted.stderr)

    def answer(self, button, stress):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-prompt-input-"))
        report, driver, facts, capture = (directory / name for name in ("report.json", "driver.gd", "facts.json", "capture.png"))
        driver.write_text(DRIVER.replace("@@FACTS@@", json.dumps(facts.as_posix())).replace(
            "@@CAPTURE@@", json.dumps(capture.as_posix())).replace("@@BUTTON@@", json.dumps(button)).replace(
            "@@STRESS@@", json.dumps(stress)), encoding="utf-8")
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-font-cache", str(self.font_cache),
            "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-report", str(report)]
            + qualified_campaign_arguments("prompt-save"),
            cwd=support.ROOT, capture_output=True, text=True, timeout=900)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(facts.is_file(), result.stdout + result.stderr)
        found = json.loads(facts.read_text(encoding="utf-8"))
        self.assertEqual(found["error"], "", found)
        self.assertFalse(found.get("early_activation", False), "an earlier input already answered the prompt: %s" % found)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        print("pending prompt input", button, stress, "seconds to outcome: %.1f" % found["seconds_to_outcome"],
              "captures:", directory, found["stages"])
        return data, found

    def check(self, button, stress):
        data, found = self.answer(button, stress)
        dialog = data["battle_dialog"]
        self.assertEqual(dialog["choices"], 1, "the real click on %s reached the prompt: %s" % (button, found))
        self.assertNotEqual(dialog["phase"], "choice", "the prompt answered and moved on")
        if button == "choice_button_left":
            self.assertTrue(data["handoff"]["requested"], "Begin requested the tactical handoff: %s" % found)
            self.assertGreater(data["handoff"]["load_ms"], 0, "the tactical battle loaded: %s" % found)
        else:
            self.assertEqual(dialog["phase"], "summary", "Auto-resolve reached its result summary: %s" % found)
            self.assertTrue(dialog["auto_resolved"], "the summary is an auto-resolve: %s" % found)
        orders = [row["type"] for row in data["live_session"]["clock_orders"]]
        self.assertNotIn("ResumeGalaxy", orders, "nothing resumed the galaxy under the prompt (#7798)")
        self.assertEqual(data["failure"], "")

    def test_begin_and_auto_resolve_with_real_clicks(self):
        self.check("choice_button_left", [])
        self.check("choice_button_right", [])

    def test_buttons_answer_with_held_camera_input(self):
        for button in ("choice_button_left", "choice_button_right"):
            with self.subTest(button=button, stress="arrow key held"):
                self.check(button, ["key"])
            with self.subTest(button=button, stress="middle-button drag"):
                self.check(button, ["middle"])
            with self.subTest(button=button, stress="right-button drag released on the button"):
                self.check(button, ["right"])

    def test_buttons_answer_after_every_input_and_space_does_not_resume(self):
        self.check("choice_button_right", ["key", "middle", "right", "world", "space"])
        self.check("choice_button_left", ["key", "middle", "right", "world", "space"])

    def test_pause_key_with_the_prompt_pending_keeps_buttons_live(self):
        # Space under the pending prompt must neither resume the galaxy (#7798) nor kill the buttons.
        for button in ("choice_button_left", "choice_button_right"):
            with self.subTest(button=button, stress="space then click"):
                self.check(button, ["space"])


if __name__ == "__main__":
    unittest.main()
