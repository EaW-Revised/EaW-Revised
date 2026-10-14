"""GHUD-S48/H120: the mounted campaign score screen of a finished synthetic campaign."""
import json
import os
import pathlib
import re
import subprocess
import tempfile
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
except ImportError:
    import test_viewer_planet_hud_runtime as support

FACTIONS = ("empire", "rebel", "underworld")
PICTURE = re.compile(r"^[a-z0-9_]+_(win|lose)\.tga$", re.IGNORECASE)


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticCampaignScoreRuntime(unittest.TestCase):
    def capture(self, fixture):
        data, capture = support.PlanetHudRuntime.capture(
            self, "synthetic", "--eawr-galactic-hud-fixture", fixture)
        return data["campaign_score"], data, capture

    @staticmethod
    def control(score, name):
        return next(row for row in score["controls"] if row["name"] == name)

    def drive(self, fixture, body, png=None):
        """Run the viewer with a scripted native-input driver; returns the report.

        With `png` the whole window (HUD included) is saved there for the owner."""
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-score-"))
        report, driver = directory / "report.json", directory / "driver.gd"
        save = ""
        if png:
            save = '''    if elapsed > 2.5 and not saved:
        saved = true
        root.get_texture().get_image().save_png(%s)
''' % json.dumps(str(png).replace("\\", "/"))
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var sent = 0
var saved = false
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    if elapsed > 1.0 and sent < 1:
        sent += 1
%s
%s
    if elapsed > 4.0:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (body, save), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True, timeout=300)
        self.log = result.stdout + result.stderr
        (directory / "viewer.log").write_text(self.log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(report.read_text(encoding="utf-8"))

    def click_body(self, row):
        x, y, w, h = row["rect"]
        return '''        for pressed in [true, false]:
            var event = InputEventMouseButton.new()
            event.position = Vector2(%s, %s)
            event.global_position = event.position
            event.button_index = MOUSE_BUTTON_LEFT
            event.pressed = pressed
            Input.parse_input_event(event)
''' % (x + w / 2, y + h / 2)

    def test_local_win_and_loss_for_each_faction(self):
        for faction in FACTIONS:
            for result, heading, marker in (("win", "TEXT_VICTORY", "_win"), ("loss", "TEXT_DEFEAT", "_lose")):
                with self.subTest(faction=faction, result=result):
                    score, data, capture = self.capture(f"score-{result}-{faction}-space")
                    self.assertTrue(capture.is_file())
                    self.assertTrue(score["shown"] and score["mounted"])
                    self.assertTrue(score["inputs_bound"])
                    self.assertEqual(score["route"], "score")
                    self.assertEqual(score["local_won"], result == "win")
                    self.assertEqual(score["heading_key"], heading)
                    self.assertTrue(score["space_draw"])
                    self.assertTrue(score["picture"].lower().endswith(marker + ".tga"), score["picture"])
                    self.assertRegex(score["picture"], PICTURE)
                    # The authored text and the three efficiencies reach their own controls.
                    self.assertNotEqual(self.control(score, "IDC_VICTORY_STATIC")["text"], heading)
                    self.assertTrue(self.control(score, "IDC_VICTORY_STATIC")["text"])
                    self.assertTrue(self.control(score, "IDC_TITLE_STATIC")["text"])
                    self.assertEqual([self.control(score, name)["text"] for name in (
                        "IDC_MILITARY_EFFICIENCY_STATIC", "IDC_CONQUEST_EFFICIENCY_STATIC",
                        "IDC_KILL_EFFICIENCY_STATIC")], ["82", "0%", "0"])
                    items = self.control(score, "IDC_VICTORY_LIST")["items"]
                    self.assertTrue(items, "the authored victory/defeat text fills the list")
                    self.assertTrue(self.control(score, "IDC_BATTLE_VICTORY_IMAGE")["visible"])
                    for row in score["controls"]:
                        if row["name"] == "IDC_CLOSE_BUTTON":
                            self.assertFalse(row["visible"], "WGP-37: the score hides its close button")
                    self.assertEqual(score["button_key"], "TEXT_BUTTON_MAIN_MENU")
                    self.assertEqual(data["map_input"]["selected"], 101, "modal input leaked to galaxy")

    def test_absent_stats_show_zero(self):
        score, _, _ = self.capture("score-win-empire-space-bare")
        self.assertEqual([self.control(score, name)["text"] for name in (
            "IDC_MILITARY_EFFICIENCY_STATIC", "IDC_CONQUEST_EFFICIENCY_STATIC",
            "IDC_KILL_EFFICIENCY_STATIC")], ["0%", "0%", "0%"])
        self.assertEqual(score["title_key"], "")
        self.assertEqual(self.control(score, "IDC_TITLE_STATIC")["text"], "")

    def test_ledger_numbers_come_from_the_mounted_module(self):
        """GHUD-S61: the production provider scores the session's own saved ledger.

        The fixture builds one unit, takes a planet and then loses both planets, so the stock
        module reads B=1 D=0 (military 1), no kill credit (0) and A=1 Z=2 (conquest 0.5)."""
        for faction, prefix in (("empire", "EMPIRE"), ("rebel", "REBEL"), ("underworld", "EMPIRE")):
            with self.subTest(faction=faction):
                score, _, _ = self.capture(f"score-win-{faction}-ledger-space")
                self.assertTrue(score["inputs_bound"])
                self.assertEqual(score["provider_seam"], "")
                self.assertEqual([self.control(score, name)["text"] for name in (
                    "IDC_MILITARY_EFFICIENCY_STATIC", "IDC_CONQUEST_EFFICIENCY_STATIC",
                    "IDC_KILL_EFFICIENCY_STATIC")], ["100", "50", "0"])
                # Rank score 30000 + 400 selects suffix 5 (WGP-102/103); only Rebel uses its own keys.
                self.assertEqual(score["title_key"], f"TEXT_{prefix}_TITLE5")
                self.assertTrue(self.control(score, "IDC_TITLE_STATIC")["text"])

    def test_picture_draw_never_touches_simulation_state(self):
        space, space_data, _ = self.capture("score-win-rebel-space")
        land, land_data, _ = self.capture("score-win-rebel-land")
        self.assertTrue(space["space_draw"])
        self.assertFalse(land["space_draw"])
        self.assertNotEqual(space["picture"], land["picture"])
        self.assertEqual(space["list_key"], land["list_key"])
        self.assertEqual(space_data["live_session"]["snapshot_sha256"],
                         land_data["live_session"]["snapshot_sha256"])

    def test_window_captures_for_the_owner(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-score-capture-"))
        for fixture in ("score-win-empire-space", "score-loss-rebel-land", "score-win-underworld-space",
                        "score-win-empire-ledger-space", "score-loss-rebel-ledger-land"):
            target = directory / (fixture + ".png")
            report = self.drive(fixture, "        pass", png=target)
            self.assertTrue(target.is_file() and target.stat().st_size > 20000, fixture)
            self.assertTrue(report["campaign_score"]["shown"])
            print("score capture:", target)

    HOST_ACTED = "EAWR score exit: host ends the session"

    def assert_host_decided(self, report):
        """The mode only published the destination and kept running; the host ended the session.

        The report is written by the mode (status score-exit) and the host-side stand-in logs
        that it acted on the destination. A mode that quit itself would return an exit code
        from process() and the host marker would never print."""
        self.assertEqual(report["status"], "score-exit")
        self.assertEqual(report["campaign_score"]["destination"], "main_menu")
        self.assertEqual(self.log.count(self.HOST_ACTED), 1, self.log)

    def test_no_destination_means_the_host_does_nothing(self):
        report = self.drive("score-win-empire-land", "        pass")
        self.assertNotEqual(report["status"], "score-exit")
        self.assertNotIn(self.HOST_ACTED, self.log)

    def test_quit_button_and_escape_emit_the_main_menu_request(self):
        score, _, _ = self.capture("score-win-empire-land")
        quit_button = self.control(score, "IDC_QUIT_BUTTON")
        self.assertTrue(quit_button["visible"])
        self.assertFalse(quit_button["disabled"])
        clicked = self.drive("score-win-empire-land", self.click_body(quit_button))
        self.assertEqual(clicked["status"], "score-exit")
        self.assertEqual(clicked["campaign_score"]["destination"], "main_menu")
        self.assertEqual(clicked["campaign_score"]["exits"], 1)
        self.assert_host_decided(clicked)
        escaped = self.drive("score-loss-underworld-space", '''        for pressed in [true, false]:
            var event = InputEventKey.new()
            event.keycode = KEY_ESCAPE
            event.physical_keycode = KEY_ESCAPE
            event.pressed = pressed
            Input.parse_input_event(event)
''')
        self.assertEqual(escaped["status"], "score-exit")
        self.assertEqual(escaped["campaign_score"]["escapes"], 1)
        self.assertEqual(escaped["campaign_score"]["destination"], "main_menu")
        self.assert_host_decided(escaped)


if __name__ == "__main__":
    unittest.main()
