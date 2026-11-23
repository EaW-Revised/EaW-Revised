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
var released_button = null
var frames_after_release = 0
func arr(v):
    return [v.x, v.y]
var caption_before = Vector2.ZERO
func caption_of(button):
    # The label that shares the button's caption (a sibling under the same frame; Godot renames the duplicate name).
    for child in button.get_parent().get_children():
        if child != button and child.has_meta("battle_text_anchor") and child.get_meta("battle_caption") == button.get_meta("battle_caption"):
            return child
    return null
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
    if elapsed > 240 and stage != 15:
        finish("the pending prompt never became ready")
        return false
    if clicked_at > 0.0 and is_instance_valid(released_button):
        # HCB-76: an accepted single-player choice hides the prompt immediately.
        frames_after_release += 1
        if not released_button.is_visible_in_tree() and not facts.has("hidden_frame"):
            facts["hidden_frame"] = frames_after_release
        if caption_of(released_button).position != caption_before:
            facts["caption_moved"] = true
    if clicked_at > 0.0 and @@BUTTON@@ == "choice_button_right":
        var progress = control(instance, "IDC_AUTO_RESOLVE_PROGRESS_DEFENDER")
        var resume = control(instance, "EawrResumeGame")
        if progress != null and progress.is_visible_in_tree() and resume != null:
            facts["progress_seen"] = true
            if resume.is_visible_in_tree(): facts["progress_banner_visible"] = true
    if beat < 0.15:
        return false
    beat = 0.0
    var battle = control(instance, "CampaignBattleDialog")
    var button = control(battle, @@BUTTON@@)
    if stage == 0:
        if not ready(button) or not ready(control(battle, "choice_button_left")): return false
        var resume = control(instance, "EawrResumeGame")
        if resume == null: return false
        var pause_button = control(instance, "b_play_pause")
        if pause_button == null or not pause_button.is_pressed(): return false
        facts["choice_clock_paused"] = true
        facts["pause_banner_visible"] = resume.is_visible_in_tree()
        target_point = button.get_global_rect().get_center()
        mouse_move(world_point, Vector2.ZERO)
        facts["ready"] = true
        facts["button_rect"] = [button.get_global_rect().position.x, button.get_global_rect().position.y,
            button.get_global_rect().size.x, button.get_global_rect().size.y]
        stage = 20 if "escape" in @@STRESS@@ else 1
        stage_at = elapsed
        return false
    if elapsed - stage_at < 0.4:
        return false
    stage_at = elapsed
    if stage == 20:
        key(KEY_ESCAPE, true); key(KEY_ESCAPE, false)
        stage = 21; return false
    if stage == 21:
        var menu = control(instance, "GalacticSaveMenu")
        var resume = control(instance, "EawrResumeGame")
        if menu == null or not menu.is_visible_in_tree() or resume == null or not resume.is_visible_in_tree(): return false
        facts["escape_menu_and_player_banner"] = true
        key(KEY_ESCAPE, true); key(KEY_ESCAPE, false)
        stage = 22; return false
    if stage == 22:
        var menu = control(instance, "GalacticSaveMenu")
        var resume = control(instance, "EawrResumeGame")
        if menu == null or menu.is_visible_in_tree() or resume == null or resume.is_visible_in_tree(): return false
        facts["closed_menu_banner_hidden"] = true
        stage = 1; return false
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
        mouse_move(world_point, Vector2.ZERO)
        stage = 121
        return false
    if stage == 121:
        root.get_texture().get_image().save_png(@@CAPTURE@@ + ".normal.png")
        mouse_move(target_point, Vector2.ZERO)
        stage = 13
        return false
    if stage == 13:
        facts["hovered"] = button.is_hovered()
        facts["hover_mode"] = button.get_draw_mode()
        facts["hover_texture"] = button.texture_hover != null
        if button.texture_hover != null:
            facts["hover_texture_size"] = arr(button.texture_hover.get_size())
        var picked = root.gui_get_hovered_control()
        facts["hover_control"] = "" if picked == null else str(picked.name)
        root.get_texture().get_image().save_png(@@CAPTURE@@ + ".hover.png")
        mouse_button(target_point, MOUSE_BUTTON_LEFT, true)
        released_button = button
        caption_before = caption_of(button).position
        note("press " + @@BUTTON@@)
        stage = 16
        return false
    if stage == 16:
        # Held: the original changes no mode on the press, so the button is still enabled and nothing moves.
        facts["held_enabled"] = not button.is_disabled()
        facts["held_caption"] = arr(caption_of(button).position - caption_before)
        mouse_button(target_point, MOUSE_BUTTON_LEFT, false)
        note("release " + @@BUTTON@@)
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
            if reached:
                var resume = control(instance, "EawrResumeGame")
                facts["summary_banner_visible"] = resume != null and resume.is_visible_in_tree()
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
        self.assertTrue(found["choice_clock_paused"], "the prompt held the clock: %s" % found)
        self.assertFalse(found["pause_banner_visible"], "the prompt's pause must not show the pause banner: %s" % found)
        self.assertFalse(found.get("early_activation", False), "an earlier input already answered the prompt: %s" % found)
        from PIL import Image, ImageChops, ImageStat
        x, y, w, h = found["button_rect"]
        box = (int(x), int(y), int(x + w), int(y + h))
        before = Image.open(str(capture) + ".normal.png").convert("RGB").crop(box)
        after = Image.open(str(capture) + ".hover.png").convert("RGB").crop(box)
        mean = sum(ImageStat.Stat(ImageChops.difference(before, after)).mean) / 3
        self.assertGreater(mean, 10, "the authored rollover must visibly change the button: %s" % mean)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        print("pending prompt input", button, stress, "seconds to outcome: %.1f" % found["seconds_to_outcome"],
              "captures:", directory, found["stages"])
        return data, found

    def check(self, button, stress):
        data, found = self.answer(button, stress)
        dialog = data["battle_dialog"]
        # HCB-76: the real pointer path keeps the held caption still and hides the accepted solo prompt.
        self.assertTrue(found["held_enabled"], "the held button is still enabled: %s" % found)
        self.assertEqual(found["held_caption"], [0, 0], "no caption shift while held: %s" % found)
        self.assertTrue(found["hovered"], "the pointer hovers the prompt button: %s" % found)
        self.assertTrue(found["hover_texture"], "the authored rollover is loaded: %s" % found)
        self.assertEqual(found["hover_mode"], 2, "the button draws its hover state: %s" % found)
        self.assertLessEqual(found.get("hidden_frame", 999), 2, "the prompt hides within two frames: %s" % found)
        sounds = data["galactic_audio"]["sounds"]
        self.assertGreaterEqual(sounds.get("GUI_Highlight_Mouse_Over", 0), 1, "rollover sound: %s" % sounds)
        self.assertEqual(sounds.get("GUI_Button_Press_2_SFX", 0), 1, "one accepted click sound: %s" % sounds)
        self.assertNotIn("caption_moved", found, "the caption does not move on release: %s" % found)
        self.assertEqual(dialog["choices"], 1, "the real click on %s reached the prompt: %s" % (button, found))
        self.assertNotEqual(dialog["phase"], "choice", "the prompt answered and moved on")
        if button == "choice_button_left":
            self.assertTrue(data["handoff"]["requested"], "Begin requested the tactical handoff: %s" % found)
            self.assertGreater(data["handoff"]["load_ms"], 0, "the tactical battle loaded: %s" % found)
        else:
            self.assertEqual(dialog["phase"], "summary", "Auto-resolve reached its result summary: %s" % found)
            self.assertTrue(dialog["auto_resolved"], "the summary is an auto-resolve: %s" % found)
            self.assertTrue(found.get("progress_seen"), "the auto-resolve progress was observed: %s" % found)
            self.assertFalse(found.get("progress_banner_visible", False), "no banner during system-held progress: %s" % found)
            self.assertFalse(found["summary_banner_visible"], "no banner during system-held summary: %s" % found)
            self.assertGreater(data["live_session"]["pause_depth"], 0, "the summary holds the clock")
        orders = [row["type"] for row in data["live_session"]["clock_orders"]]
        if "escape" not in stress:
            self.assertNotIn("ResumeGalaxy", orders, "nothing resumed the galaxy under the prompt (#7798)")
        else:
            self.assertTrue(found.get("escape_menu_and_player_banner"), found)
            self.assertTrue(found.get("closed_menu_banner_hidden"), found)
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

    def test_escape_pause_and_auto_resolve_hold_visibility(self):
        self.check("choice_button_right", ["escape"])


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT and os.environ.get("EAWR_REMAKE_MOD_ROOT"),
                     "requires GPU viewer, installed FoC and Remake")
class PendingPromptRemakeHover(unittest.TestCase):
    def test_remake_rollover_visible(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-remake-hover-"))
        driver = directory / "driver.gd"
        facts, normal, hover, report = (directory / name for name in ("facts.json", "normal.png", "hover.png", "report.json"))
        driver.write_text(r'''extends SceneTree
var elapsed = 0.0
var stage = 0
var button
var point
func move_to(p):
    var event = InputEventMouseMotion.new()
    event.position = p
    event.global_position = p
    event.relative = Vector2.ZERO
    Input.parse_input_event(event)
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    if elapsed > 120:
        quit(2)
        return false
    if stage == 0:
        button = root.find_child("choice_button_left", true, false)
        if button == null or not button.is_visible_in_tree() or button.is_disabled(): return false
        point = button.get_global_rect().get_center()
        move_to(Vector2(16,350))
        elapsed = 0
        stage = 1
    elif stage == 1 and elapsed > 0.5:
        root.get_texture().get_image().save_png(@@NORMAL@@)
        move_to(point)
        elapsed = 0
        stage = 2
    elif stage == 2 and elapsed > 0.5:
        root.get_texture().get_image().save_png(@@HOVER@@)
        var result = {"hovered": button.is_hovered(), "draw_mode": button.get_draw_mode(),
            "texture": button.texture_hover != null, "rect": [button.global_position.x, button.global_position.y,
            button.size.x, button.size.y]}
        if button.texture_hover != null: result["texture_size"] = [button.texture_hover.get_width(), button.texture_hover.get_height()]
        var file = FileAccess.open(@@FACTS@@, FileAccess.WRITE)
        file.store_string(JSON.stringify(result))
        file.close()
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''.replace("@@NORMAL@@", json.dumps(normal.as_posix())).replace("@@HOVER@@", json.dumps(hover.as_posix())).replace(
            "@@FACTS@@", json.dumps(facts.as_posix())), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path",
            str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--", "--eawr-galactic", "synthetic",
            "--eawr-galactic-hud-fixture", "battle-begin", "--eawr-game-root", support.GAME_ROOT,
            "--eawr-mod-root", os.environ["EAWR_REMAKE_MOD_ROOT"], "--eawr-galactic-paused", "yes",
            "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(facts.is_file(), result.stdout + result.stderr)
        found = json.loads(facts.read_text(encoding="utf-8"))
        self.assertTrue(found["hovered"] and found["texture"], found)
        self.assertEqual(found["draw_mode"], 2, found)
        from PIL import Image, ImageChops, ImageStat
        x, y, w, h = found["rect"]
        box = (int(x), int(y), int(x + w), int(y + h))
        before = Image.open(normal).convert("RGB").crop(box)
        after = Image.open(hover).convert("RGB").crop(box)
        mean = sum(ImageStat.Stat(ImageChops.difference(before, after)).mean) / 3
        self.assertGreater(mean, 10, "rollover must visibly change the button: %s, mean %s" % (found, mean))
        print("Remake rollover capture:", directory, found, "mean difference", mean)


if __name__ == "__main__":
    unittest.main()
