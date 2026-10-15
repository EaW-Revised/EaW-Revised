"""GHUD-S11..13: mounted progress and campaign results with native input."""
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
class GalacticBattleDialogRuntime(unittest.TestCase):
    def capture(self, fixture):
        return support.PlanetHudRuntime.capture(self, "synthetic", "--eawr-galactic-hud-fixture", fixture)

    @staticmethod
    def control(data, name):
        return next(row for row in data["battle_dialog"]["controls"] if row["name"] == name)

    def click(self, fixture, row, retry=False, wheel=False, before=None, resize=None, overlap=False):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-battle-dialog-"))
        report, capture, driver = (directory / name for name in ("report.json", "capture.png", "driver.gd"))
        x, y, w, h = row["rect"]
        before_code = ""
        if resize:
            before_code = '''    if elapsed > 0.6 and not prepared:
        prepared = true
        root.size = Vector2i(%s, %s)
''' % resize
        if before:
            bx, by, bw, bh = before["rect"]
            before_code = '''    if elapsed > 0.6 and not prepared:
        prepared = true
        for pressed in [true, false]:
            var event = InputEventMouseButton.new()
            event.position = Vector2(%s, %s)
            event.global_position = event.position
            event.button_index = MOUSE_BUTTON_LEFT
            event.pressed = pressed
            Input.parse_input_event(event)
''' % (bx + bw / 2, by + bh / 2)
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var sent = 0
var prepared = false
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
%s
func _process(delta):
    elapsed += delta
%s
    if elapsed > 1.0 + sent * 0.7 and sent < %s:
        sent += 1
        for pressed in [true, false]:
            var event = InputEventMouseButton.new()
            event.position = Vector2(%s, %s)
            event.global_position = event.position
            event.button_index = %s
            event.pressed = pressed
            Input.parse_input_event(event)
    if elapsed > 2.0:
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (('''    var overlay = ColorRect.new()
    overlay.name = "SyntheticFleetOverlay"
    overlay.color = Color(0, 1, 0, 1)
    overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
    overlay.z_as_relative = false
    overlay.z_index = 100
    current_scene.find_child("CampaignBattleDialog", true, false).get_parent().add_child(overlay)
    overlay.position = Vector2(%s, %s)
    overlay.size = Vector2(%s, %s)
''' % (x, y, w, h)) if overlap else "", before_code, 0 if resize else 2 if retry else 1, x + w / 2, y + h / 2,
       "MOUSE_BUTTON_WHEEL_DOWN" if wheel else "MOUSE_BUTTON_LEFT",
       json.dumps(str(capture).replace("\\", "/"))), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True, timeout=300)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        if overlap:
            from PIL import Image
            image = Image.open(capture).convert("RGB")
            region = image.crop((int(x + w * .2), int(y + h * .2), int(x + w * .8), int(y + h * .8)))
            green = sum(g > 250 and r < 5 and b < 5 for r, g, b in region.getdata())
            self.assertLess(green, region.width * region.height * .01,
                            "fleet overlay at drag-preview depth covers the pending button")
        self.assertEqual(data["map_input"]["selected"], 101, "modal input leaked to galaxy")
        return data

    def test_retained_invasion_offer_native_buttons(self):
        candidate, _ = self.capture("battle-invasion-candidate")
        self.assertFalse(self.control(candidate, "IDC_LAND_INVADE_BUTTON")["visible"])
        admitted, _ = self.capture("battle-invasion")
        invade = self.control(admitted, "IDC_LAND_INVADE_BUTTON")
        self.assertTrue(invade["visible"])
        self.assertFalse(invade["disabled"])
        accepted = self.click("battle-invasion", invade)
        self.assertEqual(accepted["battle_dialog"]["phase"], "hidden")
        self.assertEqual(accepted["battle_dialog"]["choices"], 1)
        returned = self.click("battle-invasion", self.control(admitted, "IDC_CONTINUE_BUTTON"))
        self.assertEqual(returned["battle_dialog"]["phase"], "hidden")
        self.assertEqual(returned["battle_dialog"]["choices"], 1)

    def keyboard_camera(self, fixture, key="KEY_NONE", prompt_point=(0, 0), home=False, zoom=False):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-pending-camera-"))
        report, capture, driver = (directory / name for name in ("report.json", "capture.png", "driver.gd"))
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var stage = 0
var scrolling_sent = false
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func key_event(code, pressed):
    var event = InputEventKey.new()
    event.keycode = code
    event.physical_keycode = code
    event.pressed = pressed
    Input.parse_input_event(event)
func _process(delta):
    elapsed += min(delta, 0.1)
    if elapsed > 0.6 and stage == 0:
        stage = 1
        key_event(%s, true)
    if elapsed > 0.8 and not scrolling_sent and %s:
        scrolling_sent = true
        var event = InputEventMouseButton.new()
        event.position = Vector2(16, 350)
        event.global_position = event.position
        event.button_index = MOUSE_BUTTON_RIGHT
        event.pressed = true
        Input.parse_input_event(event)
    if elapsed > 0.9 and stage == 1:
        stage = 2
        var event = InputEventMouseMotion.new()
        event.position = Vector2(%s, %s)
        event.global_position = event.position
        event.relative = Vector2(1, 0)
        Input.parse_input_event(event)
    if elapsed > 1.3 and stage == 2:
        stage = 3
        key_event(%s, false)
    if elapsed > 1.6 and stage == 3:
        stage = 4
        if %s:
            key_event(KEY_HOME, true)
            key_event(KEY_HOME, false)
        if %s:
            for pressed in [true, false]:
                var event = InputEventMouseButton.new()
                event.position = Vector2(16, 350)
                event.global_position = event.position
                event.button_index = MOUSE_BUTTON_WHEEL_UP
                event.pressed = pressed
                Input.parse_input_event(event)
    if elapsed > %s:
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (key, "true" if zoom else "false", *prompt_point, key, "true" if home else "false",
       "true" if zoom else "false", 3.0 if zoom else 2.0,
       json.dumps(str(capture).replace("\\", "/"))), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-lighting", "sh", "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["battle_dialog"]["choices"], 0)
        self.assertEqual(data["map_input"]["selected"], 101)
        self.assertEqual(data["battle_dialog"]["phase"], "choice" if fixture == "battle-choice" else "progress")
        self.assertTrue(capture.is_file())
        print("pending camera capture:", capture)
        return data

    def test_pending_prompt_keeps_keyboard_camera_live(self):
        # WGUI-171: exercise the per-frame held state, including a blocked
        # mouse motion over the prompt between key press and key release.
        baseline = self.keyboard_camera("battle-choice")
        x, y, w, h = self.control(baseline, "fight")["rect"]
        point = (x + w / 2, y + h / 2)
        target = baseline["camera"]["target"]
        for key in ("KEY_D", "KEY_RIGHT"):
            with self.subTest(key=key):
                moved = self.keyboard_camera("battle-choice", key, point)
                self.assertGreater(sum((a - b) ** 2 for a, b in zip(moved["camera"]["target"], target)), 1)
        reset = self.keyboard_camera("battle-choice", "KEY_D", point, home=True)
        for actual, expected in zip(reset["camera"]["target"], target):
            self.assertAlmostEqual(actual, expected, places=4)
        zoomed = self.keyboard_camera("battle-choice", "KEY_D", point, zoom=True)
        self.assertEqual(zoomed["flights"][0]["state"], "focused",
                         "blocked prompt motion left a mouse-scroll hold that suppresses wheel zoom")
        self.assertEqual(zoomed["flights"][0]["planet"], 101)
        modal = self.keyboard_camera("battle-progress", "KEY_D")
        for actual, expected in zip(modal["camera"]["target"], target):
            self.assertAlmostEqual(actual, expected, places=4)

    def test_summary_returned_salvage_bounty(self):
        summary, capture = self.capture("battle-summary-bounty")
        self.assertTrue(capture.is_file())
        self.assertEqual(summary["battle_dialog"]["phase"], "summary")
        bounty = self.control(summary, "IDC_STATIC_BOUNTY")
        self.assertTrue(bounty["visible"])
        self.assertEqual(bounty["text"], "Salvage collected: $ 42")
        returned = self.click("battle-summary-bounty", self.control(summary, "IDC_CONTINUE_BUTTON"))
        self.assertEqual(returned["battle_dialog"]["phase"], "hidden")
        self.assertEqual(returned["battle_dialog"]["choices"], 1)

    def test_pending_shell_variants_native_input(self):
        prompt, _ = self.capture("battle-choice")
        self.assertTrue(self.control(prompt, "fight")["disabled"])
        refused = self.click("battle-choice", self.control(prompt, "fight"))
        self.assertEqual(refused["battle_dialog"]["choices"], 0)
        chosen = self.click("battle-choice", self.control(prompt, "auto"))
        self.assertEqual(chosen["battle_dialog"]["phase"], "progress")
        self.assertEqual(chosen["battle_dialog"]["choices"], 1)
        refused = self.click("battle-refusal", self.control(prompt, "auto"))
        self.assertEqual(refused["battle_dialog"]["phase"], "choice")
        self.assertFalse(self.control(refused, "auto")["disabled"])
        self.assertFalse(any(row["name"] in ("retreat", "IDC_WITHDRAW_BUTTON") for row in refused["battle_dialog"]["controls"]))
        retried = self.click("battle-refusal", self.control(prompt, "auto"), retry=True)
        self.assertEqual(retried["battle_dialog"]["phase"], "progress")
        self.assertEqual(retried["battle_dialog"]["choices"], 2)
        self.assertEqual(prompt["battle_dialog"]["pending_shell"], "pending_battle_shell")
        self.assertEqual(prompt["battle_dialog"]["placement_seam"], "GHUD-S75")
        single, _ = self.capture("battle-single")
        self.assertEqual(single["battle_dialog"]["pending_shell"], "story_pending_battle_shell")
        self.assertFalse(any(row["name"] == "auto" for row in single["battle_dialog"]["controls"]))
        self.assertEqual(self.control(single, "fight")["text"], "Begin")
        begun = self.click("battle-single", self.control(single, "fight"))
        self.assertEqual(begun["battle_dialog"]["phase"], "hidden")
        self.assertEqual(begun["battle_dialog"]["choices"], 1)
        ordinary, _ = self.capture("battle-begin")
        begun = self.click("battle-begin", self.control(ordinary, "fight"))
        self.assertEqual(begun["battle_dialog"]["phase"], "hidden")
        self.assertEqual(begun["battle_dialog"]["choices"], 1)

    def test_pending_strip_covers_fleet_overlay_and_uses_authored_text_anchor(self):
        prompt, _ = self.capture("battle-choice")
        title = self.control(prompt, "pending_title")
        self.assertEqual(title["text"], "Coruscant: SPACE BATTLE IMMINENT!")
        self.assertTrue(prompt["battle_dialog"]["animated_pose"])
        x, y, w, h = title["rect"]
        ax, ay = title["text_anchor"]
        factor = 0 if title["alignment"] == 0 else 1 if title["alignment"] == 2 else .5
        self.assertAlmostEqual(x + w * factor, ax, delta=.01)
        self.assertAlmostEqual(y + h / 2, ay, delta=.01)
        self.assertGreaterEqual(ay, 0, "pending title must draw inside the viewport")
        self.assertLess(ay, 100, "pending title belongs at the top of the viewport")
        for name in ("fight", "auto"):
            bx, by, bw, bh = self.control(prompt, name)["rect"]
            self.assertGreaterEqual(bx, 0)
            self.assertGreaterEqual(by, 0)
            self.assertLessEqual(bx + bw, 1280)
            self.assertLessEqual(by + bh, 720)
        land, _ = self.capture("battle-land")
        self.assertEqual(self.control(land, "pending_title")["text"], "Coruscant: PLANETARY BATTLE IMMINENT!")
        self.assertGreater(prompt["battle_dialog"]["overlay_z"], 100)
        covered = self.click("battle-choice", self.control(prompt, "fight"), overlap=True)
        self.assertEqual(covered["battle_dialog"]["phase"], "choice")

    def test_progress_lite_withdraw_observer_and_summary(self):
        full, full_capture = self.capture("battle-progress")
        self.assertTrue(full_capture.is_file())
        self.assertTrue(full["battle_dialog"]["mounted"])
        self.assertEqual(full["battle_dialog"]["phase"], "progress")
        headings = sorted((row for row in full["battle_dialog"]["controls"]
                           if row["name"] == "IDC_STATIC"), key=lambda row: row["rect"][0])
        self.assertEqual([row["text"] for row in headings], ["Rebel Alliance", "Empire"])
        self.assertAlmostEqual(full["battle_dialog"]["progress"], 1 - 2 / (10 * .6666), places=5)
        for name in ("IDC_AUTO_RESOLVE_PROGRESS_DEFENDER", "IDC_AUTO_RESOLVE_PROGRESS_INVADER"):
            self.assertAlmostEqual(self.control(full, name)["value"], full["battle_dialog"]["progress"], places=5)
        self.assertEqual(self.control(full, "IDC_DEFENDING_FORCES_LIST")["items"],
                         ["Imperial Star Destroyer", "TIE Fighter", "TIE Fighter", "TIE Bomber", "TIE Scout"])
        advanced = self.control(full, "IDC_ADVANCED_RESOLVE_INFO")
        resized = self.click("battle-progress", advanced, resize=(1440, 900))
        self.assertGreaterEqual(resized["battle_dialog"]["rebuilds"], 2)
        self.assertEqual(resized["battle_dialog"]["choices"], 0)
        for name in ("IDC_AUTO_RESOLVE_PROGRESS_DEFENDER", "IDC_AUTO_RESOLVE_PROGRESS_INVADER"):
            self.assertAlmostEqual(self.control(resized, name)["value"], full["battle_dialog"]["progress"], places=5,
                                   msg="recreated native bar failed to sample unchanged logical time")
        lite = self.click("battle-progress", advanced)
        self.assertFalse(lite["battle_dialog"]["advanced"])
        self.assertGreaterEqual(lite["battle_dialog"]["rebuilds"], 2)
        for name in ("IDC_AUTO_RESOLVE_PROGRESS_DEFENDER", "IDC_AUTO_RESOLVE_PROGRESS_INVADER"):
            self.assertAlmostEqual(self.control(lite, name)["value"], full["battle_dialog"]["progress"], places=5)
        lite_withdrawn = self.click("battle-progress", self.control(lite, "IDC_WITHDRAW_BUTTON"), before=advanced)
        self.assertFalse(lite_withdrawn["battle_dialog"]["advanced"])
        self.assertEqual(lite_withdrawn["battle_dialog"]["choices"], 1)
        self.assertTrue(self.control(lite_withdrawn, "IDC_WITHDRAW_BUTTON")["disabled"])
        withdrawn = self.click("battle-progress", self.control(full, "IDC_WITHDRAW_BUTTON"))
        self.assertEqual(withdrawn["battle_dialog"]["choices"], 1)
        self.assertTrue(self.control(withdrawn, "IDC_WITHDRAW_BUTTON")["visible"])
        self.assertTrue(self.control(withdrawn, "IDC_WITHDRAW_BUTTON")["disabled"])
        observer, _ = self.capture("battle-observer")
        self.assertFalse(self.control(observer, "IDC_WITHDRAW_BUTTON")["visible"])
        self.assertEqual(self.control(observer, "IDC_STATIC_MEDIUM")["text"], "")
        self.assertEqual(self.control(observer, "IDC_DEFENDING_FORCES_LIST")["items"], [])
        self.assertEqual(self.control(observer, "IDC_INVADING_FORCES_LIST")["items"], [])
        for row in observer["battle_dialog"]["controls"]:
            if row["name"] == "IDC_STATIC":
                self.assertEqual(row["text"], "")
        self.assertAlmostEqual(self.control(observer, "IDC_AUTO_RESOLVE_PROGRESS_DEFENDER")["value"],
                               self.control(observer, "IDC_AUTO_RESOLVE_PROGRESS_INVADER")["value"], places=5)
        summary, summary_capture = self.capture("battle-summary")
        self.assertTrue(summary_capture.is_file())
        self.assertEqual(summary["battle_dialog"]["phase"], "summary")
        self.assertEqual(summary["battle_dialog"]["loss_totals"], [2, 3])
        self.assertEqual(summary["battle_dialog"]["time"], "00:00:00")
        for name in ("IDC_SAVE_REPLAY_BUTTON", "IDC_LAND_INVADE_BUTTON", "IDC_STATIC_BOUNTY"):
            self.assertFalse(self.control(summary, name)["visible"])
        returned = self.click("battle-summary", self.control(summary, "IDC_CONTINUE_BUTTON"))
        self.assertEqual(returned["battle_dialog"]["phase"], "hidden")
        self.assertEqual(returned["battle_dialog"]["choices"], 1)

    def test_summary_count_captions_and_native_continue(self):
        summary, capture = self.capture("battle-summary")
        self.assertTrue(capture.is_file())
        self.assertEqual(self.control(summary, "IDC_YOUR_UNIT_NAME0")["text"], "x2")
        self.assertEqual(self.control(summary, "IDC_ENEMY_UNIT_NAME0")["text"], "x3")
        self.assertEqual(self.control(summary, "IDC_YOUR_UNIT_NAME1")["text"], "")
        self.assertEqual(summary["battle_dialog"]["summary_refreshes"], 1,
                         "unchanged rendered summary must not rewrite static captions")
        returned = self.click("battle-summary", self.control(summary, "IDC_CONTINUE_BUTTON"))
        self.assertEqual(returned["battle_dialog"]["phase"], "hidden")
        self.assertEqual(returned["battle_dialog"]["choices"], 1)
        slots, _ = self.capture("battle-summary-slots")
        self.assertEqual(self.control(slots, "IDC_YOUR_UNIT_NAME0")["text"], "")
        self.assertEqual(self.control(slots, "IDC_YOUR_UNIT_NAME11")["text"], "x2")
        self.assertTrue(self.control(slots, "IDC_YOUR_SCROLLBAR")["visible"])
        self.assertEqual(self.control(slots, "IDC_YOUR_HERO0")["text"], "x2")
        self.assertEqual(self.control(slots, "IDC_YOUR_HERO4")["text"], "")
        self.assertEqual(len([row for row in slots["battle_dialog"]["controls"]
                             if row["name"].startswith("IDC_YOUR_HERO")]), 5)
        self.assertEqual(slots["battle_dialog"]["loss_totals"][0], 32)
        self.assertEqual(slots["battle_dialog"]["summary_refreshes"], 1)
        scrolled = self.click("battle-summary-slots", self.control(slots, "IDC_YOUR_SCROLLBAR"), wheel=True)
        self.assertGreater(scrolled["battle_dialog"]["summary_refreshes"], 1)
        self.assertEqual(self.control(scrolled, "IDC_YOUR_UNIT_NAME0")["text"], "x2")


if __name__ == "__main__":
    unittest.main()
